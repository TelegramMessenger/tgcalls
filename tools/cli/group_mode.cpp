#include "group_mode.h"
#include "group_participant.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

// CGo header
#include "submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/go_sfu.h"

int runGroupMode(int customParticipants, int referenceParticipants, int duration, bool quiet, bool video, const std::string& networkScenario, const std::set<int>& mutedParticipants, bool earlyVideoRequest, bool videoSinkChurn, bool e2e, VideoFeed videoFeed, bool requestOwnVideo, int unmuteAfterSeconds) {
    gGroupQuiet = quiet;
    gGroupStartTime = std::chrono::steady_clock::now();

    int participants = customParticipants + referenceParticipants;
    if (participants < 2) {
        fprintf(stderr, "Error: need at least 2 participants total\n");
        return 1;
    }

    // The two own the run's timeline and the unmute branch is an `else if` below, so
    // combining them would run the scenario, never unmute anyone, find no late
    // unmuters to validate, and report SUCCESS without having tested anything.
    if (unmuteAfterSeconds > 0 && !networkScenario.empty()) {
        fprintf(stderr, "Error: --unmute-after cannot be combined with --network-scenario\n");
        return 1;
    }

    groupLog("Group", "initializing Go SFU...");

    int rc = GoSfu_Init();
    if (rc != 0) {
        fprintf(stderr, "Error: GoSfu_Init failed with %d\n", rc);
        return 1;
    }

    GoInt sfuHandle = GoSfu_Create();
    if (sfuHandle <= 0) {
        fprintf(stderr, "Error: GoSfu_Create failed\n");
        return 1;
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
        auto state = createParticipant(i, isReference, sfuHandle, threads, quiet, video, &states, muted, earlyVideoRequest, e2e, videoFeed, requestOwnVideo);
        if (!state) {
            anyFailed = true;
            continue;
        }
        states.push_back(std::move(state));
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

    // Run for the specified duration, optionally with network scenario.
    if (!networkScenario.empty() && networkScenario == "step-down-up") {
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
            GoSfu_SetNetworkParams(sfuHandle, s->id, 1, 0, 0, 0.0, 80000);
        }
        std::this_thread::sleep_for(std::chrono::seconds(phase));

        // Phase 3: cap to 200 kbps (should allow upswitch to layer 1).
        groupLog("Group", "phase 3: cap 200kbps");
        for (const auto& s : states) {
            GoSfu_SetNetworkParams(sfuHandle, s->id, 1, 0, 0, 0.0, 200000);
        }
        std::this_thread::sleep_for(std::chrono::seconds(phase));

        // Phase 4: uncap (should allow upswitch to layer 2).
        groupLog("Group", "phase 4: uncapped");
        for (const auto& s : states) {
            GoSfu_SetNetworkParams(sfuHandle, s->id, 1, 0, 0, 0.0, 0);
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
            s->sfuSelfRequestedLayer.store((int)GoSfu_QueryRequestedLayer(sfuHandle, (GoInt)s->id, (GoInt)s->id));
        }
    }

    // Stop all participants (using GoSfu_Destroy for bulk teardown)
    groupLog("Group", "stopping participants...");

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
    GoSfu_Destroy(sfuHandle);
    GoSfu_Shutdown();

    // Validate and print summary
    auto result = validateGroupState(states, video);
    bool success = printGroupSummary(customParticipants, referenceParticipants, duration, video, result, anyFailed);

    // Clean up log files
    for (const auto& s : states) {
        unlink(s->logPath.c_str());
    }

    fflush(stdout);
    fflush(stderr);
    _exit(success ? 0 : 1);
}
