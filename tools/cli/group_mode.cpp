#include "group_mode.h"

#include <csignal>
#include <cstdlib>
#include "group_participant.h"
#include "split_channel.h"
#include "split_group.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>

#include "third-party/json11.hpp"

// CGo header
#include "submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/go_sfu.h"

int runGroupMode(int customParticipants, int referenceParticipants, int duration, bool quiet, bool video, const std::string& networkScenario, const std::set<int>& mutedParticipants, bool earlyVideoRequest, bool videoSinkChurn, bool e2e, VideoFeed videoFeed, bool requestOwnVideo, int unmuteAfterSeconds, bool videoRerequest, const GroupSplitOptions& split) {
    gGroupQuiet = quiet;
    gGroupStartTime = std::chrono::steady_clock::now();

    const bool splitHost = split.role == "host";
    const bool splitJoin = split.role == "join";
    int participants = customParticipants + referenceParticipants;
    if (participants < (split.role.empty() ? 2 : 1)) {
        fprintf(stderr, "Error: need at least %d participant(s)\n", split.role.empty() ? 2 : 1);
        return 1;
    }
    // A split call (--split): the socket to the other process, and its end of the protocol (split_group.h).
    std::shared_ptr<SplitChannel> splitChannel;
    std::shared_ptr<SplitGroupClient> splitClient;
    std::unique_ptr<SplitGroupHost> splitHostServer;
    if (splitHost || splitJoin) {
        signal(SIGPIPE, SIG_IGN);
    }

    // The two own the run's timeline and the unmute branch is an `else if` below, so
    // combining them would run the scenario, never unmute anyone, find no late
    // unmuters to validate, and report SUCCESS without having tested anything.
    if (unmuteAfterSeconds > 0 && !networkScenario.empty()) {
        fprintf(stderr, "Error: --unmute-after cannot be combined with --network-scenario\n");
        return 1;
    }

    GoInt sfuHandle = 0;
    if (splitJoin) {
        // No SFU here: the host's, over the socket. Its participants are senders this process's must hear and see.
        std::string error;
        std::shared_ptr<SplitChannel> channel = SplitChannel::connect(split.socketPath, 60000, error);
        if (!channel) {
            fprintf(stderr, "Error: --split join: %s\n", error.c_str());
            return 1;
        }
        splitChannel = channel;
        splitClient = std::make_shared<SplitGroupClient>(splitChannel);
        std::vector<SplitParticipantInfo> hostParticipants;
        if (!splitClient->hello(hostParticipants, error)) {
            fprintf(stderr, "Error: --split join: %s\n", error.c_str());
            return 1;
        }
        for (const auto& info : hostParticipants) {
            addRemoteParticipant(info);
        }
        setRemoteSfu(splitClient);
        groupLog("Split", "joining the host's call, %zu participant(s) there", hostParticipants.size());
    } else {
        groupLog("Group", "initializing Go SFU...");

        int rc = GoSfu_Init();
        if (rc != 0) {
            fprintf(stderr, "Error: GoSfu_Init failed with %d\n", rc);
            return 1;
        }

        sfuHandle = GoSfu_Create();
        if (sfuHandle <= 0) {
            fprintf(stderr, "Error: GoSfu_Create failed\n");
            return 1;
        }

        if (gGroupScreencast.sfuListsAv1) {
            GoSfu_SetListAv1(sfuHandle, 1);
            groupLog("Group", "the SFU lists AV1 in its join responses");
        }
        if (gGroupScreencast.sfuDependencyDescriptorId > 0) {
            GoSfu_SetDependencyDescriptorId(sfuHandle, gGroupScreencast.sfuDependencyDescriptorId);
            groupLog("Group", "the SFU lists the dependency descriptor at %d", gGroupScreencast.sfuDependencyDescriptorId);
        }
    }
    if (splitHost) {
        // Listening before the participants exist lets the joiner connect at once; its hello waits for them.
        std::string error;
        std::shared_ptr<SplitChannel> channel = SplitChannel::listen(split.socketPath, error);
        if (!channel) {
            fprintf(stderr, "Error: --split host: %s\n", error.c_str());
            return 1;
        }
        splitChannel = channel;
    }

    const char* videoFeedName = videoFeed == VideoFeed::Source ? "source"
        : videoFeed == VideoFeed::CaptureAtJoin ? "capture-at-join" : "capture-late";
    groupLog("Group", "created SFU handle=%lld, custom=%d, reference=%d, duration=%ds, video=%d, earlyVideoRequest=%d, videoFeed=%s, requestOwnVideo=%d",
             (long long)sfuHandle, customParticipants, referenceParticipants, duration, video ? 1 : 0, earlyVideoRequest ? 1 : 0,
             videoFeedName, requestOwnVideo ? 1 : 0);

    auto threads = tgcalls::StaticThreads::getThreads();

    // Create participants
    std::vector<std::unique_ptr<ParticipantState>> states;
    bool anyFailed = false;

    for (int i = 0; i < participants; ++i) {
        bool isReference = (i >= customParticipants);
        bool muted = mutedParticipants.count(i) > 0;
        auto state = createParticipant(i + split.participantIdOffset, isReference, sfuHandle, threads, quiet, video, &states, muted, earlyVideoRequest, e2e, videoFeed, requestOwnVideo);
        if (!state) {
            anyFailed = true;
            continue;
        }
        states.push_back(std::move(state));
    }

    // A split call starts its run once both processes' participants exist.
    if (splitHost) {
        std::string error;
        std::vector<SplitParticipantInfo> own;
        for (const auto& s : states) {
            own.push_back(splitParticipantInfo(*s));
        }
        if (!splitChannel->accept(60000, error)) {
            fprintf(stderr, "Error: --split host: %s\n", error.c_str());
            anyFailed = true;
        } else {
            splitHostServer = std::make_unique<SplitGroupHost>(splitChannel, sfuHandle, std::move(own));
            if (!splitHostServer->waitReady(60000)) {
                fprintf(stderr, "Error: --split host: %s\n", splitHostServer->error().c_str());
                anyFailed = true;
            }
        }
    }
    if (splitJoin && !splitClient->ready()) {
        anyFailed = true;
    }

    // Wait for all participants to connect
    groupLog("Group", "waiting for connections...");
    bool allConnected = false;
    auto waitStart = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(15)) {
        int connectedCount = 0;
        for (const auto& s : states) {
            if (s->wasConnected.load()) connectedCount++;
        }
        if (connectedCount == (int)states.size()) {
            allConnected = true;
            groupLog("Group", "all %d participants connected", (int)states.size());
            break;
        }
        groupLog("Group", "connected: %d/%d", connectedCount, (int)states.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (!allConnected) {
        int connectedCount = 0;
        for (const auto& s : states) {
            if (s->wasConnected.load()) connectedCount++;
        }
        groupLog("Group", "connection timeout: %d/%d connected", connectedCount, (int)states.size());
    }

    if (gGroupScreencast.sharerIngressLoss > 0.0) {
        for (const auto& s : states) {
            if (s->sharesScreen) {
                sfuSetNetworkParams(sfuHandle, s->id, 0, 0, 0, gGroupScreencast.sharerIngressLoss, 0);
                groupLog("Group", "P%d: %.0f%% of its packets lost on the way into the SFU", s->id, gGroupScreencast.sharerIngressLoss * 100.0);
            }
        }
    }

    // Optionally replace every incoming video sink a few times once frames
    // are flowing (mirrors the app recreating tile views on quality changes).
    if (video && videoSinkChurn) {
        constexpr int kSinkChurnRounds = 5;
        groupLog("Group", "video-sink-churn: waiting 3s for frames, then %d replacement rounds", kSinkChurnRounds);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        for (int round = 0; round < kSinkChurnRounds; ++round) {
            groupLog("Group", "video-sink-churn: round %d", round + 1);
            churnVideoSinks(states);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    // Optionally drop every video request and issue it again on the same
    // PeerConnection (the app does this whenever the UI leaves and re-enters
    // the video grid, e.g. on backgrounding). Video keeps flowing under a
    // downlink delay while the engines renegotiate, because on a real network
    // the packets the SFU forwarded before it read the empty
    // ReceiverVideoConstraints are still in flight when the receive streams
    // are torn down; without the delay the in-process SFU stops the streams
    // before the renegotiation lands and the case is never exercised.
    //
    // The cycle repeats: the hazard is a packet that the network thread has
    // already handed to the worker when the worker tears the stream down, and
    // at the harness's packet rates a single cycle rarely lands in that
    // window. Validation compares against the counts recorded at the last
    // drop, so every round must come back for the run to pass.
    if (video && videoRerequest) {
        constexpr int kEgressDelayMs = 150;
        constexpr int kRerequestRounds = 5;
        groupLog("Group", "video-rerequest: waiting 3s for frames, then %dms downlink delay and %d drop/re-request rounds", kEgressDelayMs, kRerequestRounds);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        for (const auto& s : states) {
            sfuSetNetworkParams(sfuHandle, s->id, 1, kEgressDelayMs, 0, 0.0, 0);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        for (int round = 0; round < kRerequestRounds; ++round) {
            groupLog("Group", "video-rerequest: round %d", round + 1);
            dropAllVideoRequests(states);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            rerequestAllVideo(states);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    // --video-quality-switch: receivers alternate between a sharer's low layer (thumbnail) and its top layer (full),
    // which with AV1 on the top layer switches each receive stream between H264 and AV1 decoding. Every sink must get
    // frames in every interval.
    int qualitySwitchPairs = 0;
    int qualitySwitchStalled = 0;
    const bool qualitySwitch = video && gGroupScreencast.qualitySwitchMs > 0;

    // Run for the specified duration, optionally with network scenario.
    if (qualitySwitch) {
        std::set<std::string> sharerEndpoints;
        for (const auto& s : states) {
            if (s->sharesScreen) sharerEndpoints.insert(s->endpointId);
        }
        auto snapshot = [&]() {
            std::map<std::pair<int, std::string>, int> counts;
            for (const auto& s : states) {
                std::lock_guard<std::mutex> lock(s->videoSinksMutex);
                for (const auto& endpoint : sharerEndpoints) {
                    auto it = s->videoSinks.find(endpoint);
                    if (it != s->videoSinks.end() && endpoint != s->endpointId) {
                        counts[{s->id, endpoint}] = it->second->frameCount();
                    }
                }
            }
            return counts;
        };
        groupLog("Group", "video-quality-switch: waiting 3s for frames, then switching every %dms", gGroupScreencast.qualitySwitchMs);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        auto last = snapshot();
        std::set<std::pair<int, std::string>> checked;
        std::set<std::pair<int, std::string>> stalled;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
        int round = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto quality = (round % 2 == 0) ? tgcalls::VideoChannelDescription::Quality::Thumbnail
                                                  : tgcalls::VideoChannelDescription::Quality::Full;
            groupLog("Group", "video-quality-switch: round %d, %s", round + 1, round % 2 == 0 ? "thumbnail" : "full");
            setRequestedVideoQualityFor(states, sharerEndpoints, quality);
            std::this_thread::sleep_for(std::chrono::milliseconds(gGroupScreencast.qualitySwitchMs));
            const auto now = snapshot();
            for (const auto& entry : now) {
                checked.insert(entry.first);
                auto previous = last.find(entry.first);
                if (previous != last.end() && entry.second <= previous->second) {
                    groupLog("Group", "video-quality-switch: P%d got no frames from %s in round %d", entry.first.first, entry.first.second.c_str(), round + 1);
                    stalled.insert(entry.first);
                }
            }
            last = now;
            round++;
        }
        qualitySwitchPairs = (int)checked.size();
        qualitySwitchStalled = (int)stalled.size();
    } else if (!networkScenario.empty() && networkScenario == "step-down-up") {
        // Scenario: start uncapped, then step down, step up, uncap.
        // Split duration into 4 phases.
        int phase = std::max(duration / 4, 2);
        groupLog("Group", "network-scenario '%s': phase duration=%ds", networkScenario.c_str(), phase);

        // Phase 1: uncapped (should be layer 2 on high BW).
        groupLog("Group", "phase 1: uncapped");
        std::this_thread::sleep_for(std::chrono::seconds(phase));

        // Phase 2: cap to 80 kbps (should force downswitch to layer 0).
        groupLog("Group", "phase 2: cap 80kbps");
        for (const auto& s : states) {
            sfuSetNetworkParams(sfuHandle, s->id, 1, 0, 0, 0.0, 80000);
        }
        std::this_thread::sleep_for(std::chrono::seconds(phase));

        // Phase 3: cap to 200 kbps (should allow upswitch to layer 1).
        groupLog("Group", "phase 3: cap 200kbps");
        for (const auto& s : states) {
            sfuSetNetworkParams(sfuHandle, s->id, 1, 0, 0, 0.0, 200000);
        }
        std::this_thread::sleep_for(std::chrono::seconds(phase));

        // Phase 4: uncap (should allow upswitch to layer 2).
        groupLog("Group", "phase 4: uncapped");
        for (const auto& s : states) {
            sfuSetNetworkParams(sfuHandle, s->id, 1, 0, 0, 0.0, 0);
        }
        std::this_thread::sleep_for(std::chrono::seconds(phase));
    } else if (unmuteAfterSeconds > 0) {
        // Mid-call unmute. The muted participants sent nothing while everyone
        // joined, so their SSRCs are unknown to every peer until now — and by
        // now each peer has already negotiated the audio m-lines for the SSRCs
        // it discovered at join. This is the app's "someone turns their
        // microphone on after a while" case.
        int settle = std::min(unmuteAfterSeconds, duration);
        groupLog("Group", "running for %ds, then unmuting %d participant(s)",
                 settle, (int)mutedParticipants.size());
        std::this_thread::sleep_for(std::chrono::seconds(settle));

        // Nothing is signalled to the peers: they are told about this participant by
        // no one and must notice the new SSRC from the media itself, exactly as they
        // must in a real call. The unmute deliberately lands after the peers have
        // renegotiated for the SSRCs they saw at join.
        for (const auto& s : states) {
            if (!s->muted || !s->instance) continue;
            groupLog("Group", "unmuting P%d (ssrc=%u)", s->id, s->audioSsrc);
            s->instance->setIsMuted(false);
            s->muted = false;
            s->unmutedLate = true;
        }

        int remaining = std::max(duration - settle, 5);
        groupLog("Group", "running for %d more seconds after the unmute...", remaining);
        std::this_thread::sleep_for(std::chrono::seconds(remaining));
    } else {
        groupLog("Group", "running for %d seconds...", duration);
        std::this_thread::sleep_for(std::chrono::seconds(duration));
    }

    // Ask the SFU, while it is still alive, whether any participant requested
    // its OWN video. --request-own-video puts the own endpoint in the list the
    // way the app does; the engine has to drop it before it reaches the SFU.
    if (video) {
        for (const auto& s : states) {
            s->sfuSelfRequestedLayer.store(sfuQueryRequestedLayer(sfuHandle, s->id, s->id));
        }
    }

    // What each video sender's layers carried, by payload type (the SFU's counts): which codec each layer used.
    if (video) {
        for (const auto& s : states) {
            if (!s->sendsVideo) continue;
            const std::string raw = sfuQueryPayloadTypeStats(sfuHandle, s->id);
            if (raw.empty()) continue;
            std::string error;
            const auto json = json11::Json::parse(raw, error);
            if (!error.empty() || !json.is_object()) continue;
            for (const auto& layer : json.object_items()) {
                for (const auto& pt : layer.second.object_items()) {
                    const double frames = pt.second["frames"].number_value();
                    const double spanMs = pt.second["lastMs"].number_value() - pt.second["firstMs"].number_value();
                    const double fps = spanMs > 0 ? (frames - 1) * 1000.0 / spanMs : 0.0;
                    printf("Wire: P%d%s layer %s pt %s: %.0f packets, %.0f frames, %.1f fps\n", s->id,
                           s->sharesScreen ? " (screen)" : "", layer.first.c_str(), pt.first.c_str(),
                           pt.second["packets"].number_value(), frames, fps);
                    printf("Wire: P%d%s layer %s pt %s dependency descriptor: %.0f packets\n", s->id,
                           s->sharesScreen ? " (screen)" : "", layer.first.c_str(), pt.first.c_str(),
                           pt.second["dd"].number_value());
                }
            }
        }
    }

    // Ask the SFU how many audio RTP packets each participant actually sent it.
    // Validation requires zero for anyone who stayed muted for the whole run.
    for (const auto& s : states) {
        if (s->audioSsrc != 0) {
            s->sfuAudioPacketsReceived.store(sfuQuerySsrcPackets(sfuHandle, s->audioSsrc));
        }
    }

    // A split call: the joiner has asked the host's SFU everything it needs; the host keeps the SFU and its own
    // participants until then.
    if (splitJoin) {
        if (!splitClient->done() || splitClient->failed()) {
            anyFailed = true;
        }
    }
    if (splitHostServer && !splitHostServer->waitDone(60000)) {
        fprintf(stderr, "Error: --split host: %s\n", splitHostServer->error().c_str());
        anyFailed = true;
    }

    // Stop all participants (using GoSfu_Destroy for bulk teardown)
    groupLog("Group", "stopping participants...");

    // Stop the sharers' screen audio before their instances stop.
    for (auto& s : states) {
        stopScreencastAudio(s.get());
    }

    // Stop video sources first
    for (auto& s : states) {
        if (s->videoSource) {
            s->videoSource->Stop();
        }
        if (s->videoCapture) {
            s->videoCapture->setState(tgcalls::VideoState::Inactive);
        }
    }

    // Stop instances
    std::atomic<int> stopCount{0};
    std::mutex stopMutex;
    std::condition_variable stopCv;

    for (const auto& s : states) {
        if (s->instance) {
            int pid_local = s->id;
            s->instance->stop([&stopCount, &stopMutex, &stopCv, pid_local]() {
                groupLog("Group", "participant %d stopped", pid_local);
                stopCount.fetch_add(1);
                std::lock_guard<std::mutex> lock(stopMutex);
                stopCv.notify_all();
            });
        }
    }

    {
        std::unique_lock<std::mutex> lock(stopMutex);
        stopCv.wait_for(lock, std::chrono::seconds(5), [&] {
            return stopCount.load() >= (int)states.size();
        });
    }

    for (auto& s : states) {
        s->instance.reset();
        s->videoCapture.reset();
    }

    // Destroy SFU
    splitHostServer.reset();
    if (!splitJoin) {
        GoSfu_Destroy(sfuHandle);
        GoSfu_Shutdown();
    }

    // Validate and print summary
    auto result = validateGroupState(states, video);
    std::string splitLine;
    if (!split.role.empty()) {
        splitLine = split.role + " (protocol " + std::to_string(kSplitProtocolVersion) + "), " +
            std::to_string(remoteParticipants().size()) + " participant(s) in the other process";
    }
    bool success = printGroupSummary(customParticipants, referenceParticipants, duration, video, result, anyFailed, splitLine);
    if (qualitySwitch) {
        printf("Video after quality switch: %d/%d stalled (must be 0)\n", qualitySwitchStalled, qualitySwitchPairs);
        if (qualitySwitchStalled != 0 || qualitySwitchPairs == 0) {
            printf("Result:                 FAILED (video after quality switch)\n");
            success = false;
        }
    }

    // Clean up log files
    for (const auto& s : states) {
        // TGCALLS_CLI_KEEP_LOGS=1 keeps the engine logs of a passing run
        // (see stopParticipant in group_participant.cpp).
        if (const char* keep = std::getenv("TGCALLS_CLI_KEEP_LOGS"); !(keep && keep[0] == '1')) {
            unlink(s->logPath.c_str());
        }
    }

    fflush(stdout);
    fflush(stderr);
    _exit(success ? 0 : 1);
}
