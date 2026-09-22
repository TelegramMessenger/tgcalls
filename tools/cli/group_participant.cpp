#include "group_participant.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>
#include <unistd.h>

#include <algorithm>

#include "rtc_base/helpers.h"
#include "third-party/json11.hpp"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

std::chrono::steady_clock::time_point gGroupStartTime = std::chrono::steady_clock::now();
std::atomic<bool> gGroupQuiet{false};

double groupElapsed() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - gGroupStartTime).count();
}

void groupLog(const char* tag, const char* fmt, ...) {
    if (gGroupQuiet) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%7.3f] %s: %s\n", groupElapsed(), tag, buf);
}

// ---------------------------------------------------------------------------
// GroupSineRecorder
// ---------------------------------------------------------------------------

GroupSineRecorder::GroupSineRecorder() {
    buffer_.resize(kFrameSamples * kChannels);
}

tgcalls::AudioFrame GroupSineRecorder::Record() {
    for (size_t i = 0; i < kFrameSamples; ++i) {
        double t = static_cast<double>(phase_) / kSampleRate;
        int16_t sample = static_cast<int16_t>(kAmplitude * std::sin(2.0 * M_PI * kFrequency * t));
        for (size_t ch = 0; ch < kChannels; ++ch) {
            buffer_[i * kChannels + ch] = sample;
        }
        ++phase_;
    }

    tgcalls::AudioFrame frame;
    frame.audio_samples = buffer_.data();
    frame.num_samples = kFrameSamples;
    frame.bytes_per_sample = sizeof(int16_t);
    frame.num_channels = kChannels;
    frame.samples_per_sec = kSampleRate;
    frame.elapsed_time_ms = 0;
    frame.ntp_time_ms = 0;
    return frame;
}

int32_t GroupSineRecorder::WaitForUs() {
    return 10000; // 10ms
}

// ---------------------------------------------------------------------------
// GroupNoOpRenderer
// ---------------------------------------------------------------------------

bool GroupNoOpRenderer::Render(const tgcalls::AudioFrame&) { return true; }

// ---------------------------------------------------------------------------
// SimpleRequestMediaChannelDescriptionTask
// ---------------------------------------------------------------------------

void SimpleRequestMediaChannelDescriptionTask::cancel() {}

// ---------------------------------------------------------------------------
// churnVideoSinks
// ---------------------------------------------------------------------------

void churnVideoSinks(const std::vector<std::unique_ptr<ParticipantState>>& states) {
    for (const auto& state : states) {
        if (!state || !state->instance) continue;
        std::string tag = "P" + std::to_string(state->id);

        std::vector<std::string> endpointIds;
        {
            std::lock_guard<std::mutex> lock(state->videoSinksMutex);
            for (const auto& [endpointId, sink] : state->videoSinks) {
                endpointIds.push_back(endpointId);
            }
        }

        for (const auto& endpointId : endpointIds) {
            auto fresh = std::make_shared<FakeVideoSink>();
            state->instance->addIncomingVideoOutput(
                endpointId,
                std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>(fresh));

            std::shared_ptr<FakeVideoSink> old;
            {
                std::lock_guard<std::mutex> lock(state->videoSinksMutex);
                old = state->videoSinks[endpointId];
                state->videoSinks[endpointId] = fresh;
            }
            int oldFrames = old ? old->frameCount() : -1;
            old.reset(); // The only strong reference: the old sink is gone now.

            groupLog(tag.c_str(), "replaced video sink for endpoint %s (old sink had %d frames)",
                     endpointId.c_str(), oldFrames);
        }
    }
}

// ---------------------------------------------------------------------------
// dropAllVideoRequests / rerequestAllVideo (--video-rerequest)
// ---------------------------------------------------------------------------

void dropAllVideoRequests(const std::vector<std::unique_ptr<ParticipantState>>& states) {
    for (const auto& state : states) {
        if (!state || !state->instance) continue;
        std::string tag = "P" + std::to_string(state->id);
        size_t sinks = 0;
        {
            std::lock_guard<std::mutex> lock(state->videoSinksMutex);
            sinks = state->videoSinks.size();
        }
        state->instance->setRequestedVideoChannels({});
        groupLog(tag.c_str(), "video-rerequest: dropped all video requests (%zu sink(s) kept)", sinks);
    }
}

void rerequestAllVideo(const std::vector<std::unique_ptr<ParticipantState>>& states) {
    for (const auto& state : states) {
        if (!state || !state->instance) continue;
        std::string tag = "P" + std::to_string(state->id);
        std::vector<tgcalls::VideoChannelDescription> fullSet;
        {
            std::lock_guard<std::mutex> lock(state->videoSinksMutex);
            fullSet = state->requestedVideoChannels;
            state->videoRerequested = !fullSet.empty();
            // Baseline taken here, not at the drop: the drop is applied on the
            // engine's media thread some time after setRequestedVideoChannels
            // returns, and every frame that lands in between would count as
            // "after the re-request" and let a frozen tile pass. By now the
            // proxy has been detached for a second and the sinks are quiet.
            state->videoFramesAtRerequest.clear();
            for (const auto& [endpointId, sink] : state->videoSinks) {
                state->videoFramesAtRerequest[endpointId] = sink->frameCount();
            }
        }
        if (fullSet.empty()) continue;
        groupLog(tag.c_str(), "video-rerequest: requesting %zu endpoint(s) again", fullSet.size());
        state->instance->setRequestedVideoChannels(std::move(fullSet));
    }
}

// ---------------------------------------------------------------------------
// requestVideoFromEndpoints
// ---------------------------------------------------------------------------

namespace {
// Set once from main() before any participant exists, read afterwards only.
bool gHasRequestedVideoMaxQuality = false;
tgcalls::VideoChannelDescription::Quality gRequestedVideoMaxQuality = tgcalls::VideoChannelDescription::Quality::Full;
}

void setRequestedVideoMaxQuality(tgcalls::VideoChannelDescription::Quality quality) {
    gHasRequestedVideoMaxQuality = true;
    gRequestedVideoMaxQuality = quality;
}

void requestVideoFromEndpoints(
    ParticipantState* state,
    GoInt sfuHandle,
    const std::vector<std::string>& endpointIds
) {
    if (!state || !state->instance) return;
    std::string tag = "P" + std::to_string(state->id);

    // The SFU's ActiveVideoSsrcs never names the receiver itself, but the app
    // builds its list from the participant roster, which includes the local
    // participant, and hands it to the engine unfiltered. --request-own-video
    // reproduces that and expects the ENGINE to drop the own endpoint (see
    // GoSfu_QueryRequestedLayer in validation).
    std::vector<std::string> wanted = endpointIds;
    if (state->requestOwnVideo && !state->endpointId.empty()) {
        wanted.push_back(state->endpointId);
    }

    bool added = false;
    for (const auto& endpointId : wanted) {
        if (endpointId.empty()) continue;
        if (endpointId == state->endpointId && !state->requestOwnVideo) continue;

        {
            std::lock_guard<std::mutex> lock(state->videoSinksMutex);
            if (state->videoSinks.count(endpointId) > 0) continue;
        }

        int remoteId = 0;
        if (sscanf(endpointId.c_str(), "%d", &remoteId) != 1) continue;

        char* ssrcsRaw = GoSfu_QueryVideoSsrcs(sfuHandle, (GoInt)remoteId);
        if (!ssrcsRaw) continue;
        std::string ssrcsJson(ssrcsRaw);
        GoSfu_Free(ssrcsRaw);

        std::string err;
        auto layers = json11::Json::parse(ssrcsJson, err);
        if (!err.empty() || !layers.is_array() || layers.array_items().empty()) continue;

        tgcalls::VideoChannelDescription desc;
        desc.audioSsrc = 0;
        desc.userId = cliUserId(remoteId);
        desc.endpointId = endpointId;
        if (gHasRequestedVideoMaxQuality) {
            desc.maxQuality = gRequestedVideoMaxQuality;
            desc.minQuality = tgcalls::VideoChannelDescription::Quality::Thumbnail;
        } else {
            desc.maxQuality = tgcalls::VideoChannelDescription::Quality::Full;
            desc.minQuality = tgcalls::VideoChannelDescription::Quality::Full;
        }

        tgcalls::MediaSsrcGroup simGroup;
        simGroup.semantics = "SIM";
        for (const auto& layer : layers.array_items()) {
            uint32_t ssrc = static_cast<uint32_t>(static_cast<int64_t>(layer["ssrc"].number_value()));
            uint32_t fidSsrc = static_cast<uint32_t>(static_cast<int64_t>(layer["fidSsrc"].number_value()));
            if (ssrc == 0) continue;
            simGroup.ssrcs.push_back(ssrc);
            if (fidSsrc != 0) {
                tgcalls::MediaSsrcGroup fidGroup;
                fidGroup.semantics = "FID";
                fidGroup.ssrcs = {ssrc, fidSsrc};
                desc.ssrcGroups.push_back(std::move(fidGroup));
            }
        }
        desc.ssrcGroups.insert(desc.ssrcGroups.begin(), std::move(simGroup));

        auto sink = std::make_shared<FakeVideoSink>();
        {
            std::lock_guard<std::mutex> lock(state->videoSinksMutex);
            state->videoSinks[endpointId] = sink;
            state->requestedVideoChannels.push_back(std::move(desc));
        }
        state->instance->addIncomingVideoOutput(
            endpointId,
            std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>(sink));

        groupLog(tag.c_str(), "requesting video from endpoint %s", endpointId.c_str());
        added = true;
    }

    if (!added) return;

    std::vector<tgcalls::VideoChannelDescription> fullSet;
    {
        std::lock_guard<std::mutex> lock(state->videoSinksMutex);
        fullSet = state->requestedVideoChannels;
    }
    state->instance->setRequestedVideoChannels(std::move(fullSet));
}

// ---------------------------------------------------------------------------
// Fake end-to-end crypto (--e2e)
// ---------------------------------------------------------------------------

namespace {

uint8_t fakeE2eKeyByte(int64_t participantId, size_t index) {
    // Cheap deterministic key schedule; distinct per participant.
    uint64_t h = (uint64_t)(participantId + 1) * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t)index * 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    return (uint8_t)(h & 0xff);
}

} // namespace

tgcalls::GroupEncryptDecryptFunction makeFakeE2eTransform(int64_t ownUserId) {
    return [ownUserId](std::vector<uint8_t> const &data, int64_t userId,
                              bool isEncrypt, int32_t plaintextPrefixLength)
            -> std::vector<uint8_t> {
        const size_t prefix = (size_t)std::max(0, plaintextPrefixLength);

        if (isEncrypt) {
            if (data.size() < prefix) {
                return {};
            }
            const uint32_t nonce = (uint32_t)rtc::CreateRandomId();
            std::vector<uint8_t> out;
            out.reserve(data.size() + 6);
            out.assign(data.begin(), data.end());
            uint8_t tag = 0;
            for (size_t i = prefix; i < out.size(); i++) {
                const uint8_t k = (uint8_t)(fakeE2eKeyByte(ownUserId, i) ^
                                            (uint8_t)(nonce >> ((i % 4) * 8)));
                tag = (uint8_t)(tag + out[i]);
                out[i] = (uint8_t)(out[i] ^ k);
            }
            for (int b = 0; b < 4; b++) {
                out.push_back((uint8_t)(nonce >> (b * 8)));
            }
            out.push_back(tag);
            out.push_back((uint8_t)prefix);
            return out;
        }

        // Decrypt. The prefix length travels in the trailer; the caller passes 0.
        if (data.size() < 6) {
            return {};
        }
        const size_t storedPrefix = data[data.size() - 1];
        const uint8_t expectedTag = data[data.size() - 2];
        uint32_t nonce = 0;
        for (int b = 0; b < 4; b++) {
            nonce |= ((uint32_t)data[data.size() - 6 + b]) << (b * 8);
        }
        std::vector<uint8_t> out(data.begin(), data.end() - 6);
        if (out.size() < storedPrefix) {
            return {};
        }
        uint8_t tag = 0;
        for (size_t i = storedPrefix; i < out.size(); i++) {
            const uint8_t k = (uint8_t)(fakeE2eKeyByte(userId, i) ^
                                        (uint8_t)(nonce >> ((i % 4) * 8)));
            out[i] = (uint8_t)(out[i] ^ k);
            tag = (uint8_t)(tag + out[i]);
        }
        if (tag != expectedTag) {
            return {};   // wrong key, i.e. wrong userId
        }
        return out;
    };
}

// ---------------------------------------------------------------------------
// createParticipant
// ---------------------------------------------------------------------------

std::unique_ptr<ParticipantState> createParticipant(
    int id,
    bool isReference,
    GoInt sfuHandle,
    std::shared_ptr<tgcalls::Threads> threads,
    bool quiet,
    bool video,
    std::vector<std::unique_ptr<ParticipantState>>* allStates,
    bool muted,
    bool earlyVideoRequest,
    bool e2e,
    VideoFeed videoFeed,
    bool requestOwnVideo
) {
    auto state = std::make_unique<ParticipantState>();
    state->id = id;
    state->isReference = isReference;
    state->muted = muted;
    state->requestOwnVideo = requestOwnVideo;
    state->logPath = "/tmp/tgcalls_group_p" + std::to_string(id) + "_" + std::to_string(getpid()) + ".log";

    std::string tag = "P" + std::to_string(id);

    auto recorder = std::make_shared<GroupSineRecorder>();
    auto renderer = std::make_shared<GroupNoOpRenderer>();

    ParticipantState* statePtr = state.get();
    GoInt sfuH = sfuHandle;

    tgcalls::GroupInstanceDescriptor descriptor;
    descriptor.threads = threads;
    descriptor.config.need_log = true;
    descriptor.config.logPath = {state->logPath};
    descriptor.networkStateUpdated = [statePtr, tag](tgcalls::GroupNetworkState networkState) {
        groupLog(tag.c_str(), "network state: connected=%s", networkState.isConnected ? "true" : "false");
        statePtr->connected.store(networkState.isConnected);
        if (networkState.isConnected) {
            statePtr->wasConnected.store(true);
        }
    };
    descriptor.audioLevelsUpdated = [statePtr, tag](tgcalls::GroupLevelsUpdate const &update) {
        std::lock_guard<std::mutex> lock(statePtr->audioLevelsMutex);
        for (const auto& level : update.updates) {
            if (level.value.level > 0.01f) {
                groupLog(tag.c_str(), "audio level: ssrc=%u level=%.3f voice=%d",
                         level.ssrc, level.value.level, level.value.voice);
            }
            if (level.ssrc != 0 && level.value.level > 0.05f) {
                statePtr->receivedAudio.store(true);
            }
            if (level.ssrc != 0) {
                auto& slot = statePtr->maxAudioLevelPerSsrc[level.ssrc];
                if (level.value.level > slot) slot = level.value.level;
            }
        }
    };
    descriptor.createAudioDeviceModule = tgcalls::FakeAudioDeviceModule::Creator(
        renderer, recorder,
        tgcalls::FakeAudioDeviceModule::Options{.samples_per_sec = 48000, .num_channels = 2}
    );

    descriptor.requestMediaChannelDescriptions = [sfuH, tag, allStates](
        std::vector<uint32_t> const &ssrcs,
        std::function<void(std::vector<tgcalls::MediaChannelDescription> &&)> callback
    ) -> std::shared_ptr<tgcalls::RequestMediaChannelDescriptionTask> {
        std::set<uint32_t> audioSsrcs;
        for (const auto& s : *allStates) {
            if (s->audioSsrc != 0) audioSsrcs.insert(s->audioSsrc);
        }
        std::vector<tgcalls::MediaChannelDescription> descriptions;
        for (uint32_t ssrc : ssrcs) {
            GoInt ownerID = GoSfu_QuerySsrc(sfuH, (GoUint)ssrc);
            bool isAudio = audioSsrcs.count(ssrc) > 0;
            groupLog(tag.c_str(), "requestMediaChannelDescriptions: ssrc=%u -> owner=%lld type=%s",
                     ssrc, (long long)ownerID, isAudio ? "audio" : "video");
            tgcalls::MediaChannelDescription desc;
            desc.type = isAudio ? tgcalls::MediaChannelDescription::Type::Audio
                                : tgcalls::MediaChannelDescription::Type::Video;
            desc.audioSsrc = ssrc;
            desc.userId = cliUserId((int)ownerID);
            descriptions.push_back(std::move(desc));
        }
        callback(std::move(descriptions));
        return std::make_shared<SimpleRequestMediaChannelDescriptionTask>();
    };

    if (e2e) {
        // Conference calls always carry an encryption context in the app, and both
        // engines gate every transformer install on this callback being set.
        //
        // Deliberately NOT setting descriptor.isConference: it is unrelated to
        // encryption (nothing in either engine reads it for that) and its one
        // effect in CustomImpl is to drop outgoing video from 3 simulcast layers
        // to 1, which also removes the SIM ssrc-group that the testbench SFU
        // resolves a sender's video through -- setting it here silently zeroed
        // every video pair.
        descriptor.e2eEncryptDecrypt = makeFakeE2eTransform(cliUserId(id));
    }

    descriptor.outgoingAudioBitrateKbit = 32;
    descriptor.disableIncomingChannels = false;
    descriptor.useDummyChannel = true;

    // Video configuration
    if (video) {
        state->sendsVideo = true;
        state->endpointId = std::to_string(id);
        descriptor.videoContentType = tgcalls::VideoContentType::Generic;
        descriptor.videoCodecPreferences = {tgcalls::VideoCodecName::H264};
        // Set the outgoing video min bitrate to 600 kbps so the sender's
        // BWE floor is high enough to activate all 3 simulcast layers
        // (audio 32k + L0 min 50k + L1 min 100k + L2 min 300k = 482k).
        // On localhost, delay-based BWE over the loopback pacer has been
        // observed to drift down to ~80 kbps, keeping L2 disabled. Clamping
        // the min forces the encoder to keep L2 producing.
        descriptor.minOutgoingVideoBitrateKbit = 600;

        switch (videoFeed) {
        case VideoFeed::Source: {
            auto videoSource = FakeVideoTrackSource::Create(id);
            state->videoSource = videoSource;
            descriptor.getVideoSource = [videoSource]() -> webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> {
                return videoSource;
            };
            break;
        }
        case VideoFeed::CaptureAtJoin:
        case VideoFeed::CaptureLate: {
            // The app's contract: a VideoCaptureInterface built by the platform
            // (here the fake platform's FakeVideoCapturer, tinted by deviceId).
            // Capture starts as soon as it is created, like the camera preview.
            state->videoCapture = tgcalls::VideoCaptureInterface::Create(threads, std::to_string(id));
            state->videoViaCapture = true;
            if (videoFeed == VideoFeed::CaptureAtJoin) {
                descriptor.videoCapture = state->videoCapture;
                groupLog(tag.c_str(), "video via VideoCaptureInterface at join (descriptor.videoCapture)");
            }
            break;
        }
        }

        descriptor.dataChannelMessageReceived = [statePtr, sfuH, tag](std::string const &message) {
            std::string parseErr;
            auto json = json11::Json::parse(message, parseErr);
            if (!parseErr.empty() || !json.is_object()) return;
            auto cls = json["colibriClass"].string_value();
            if (cls != "ActiveVideoSsrcs") return;

            auto ssrcsArray = json["ssrcs"].array_items();
            if (ssrcsArray.empty()) return;

            std::vector<std::string> endpointIds;
            for (const auto& entry : ssrcsArray) {
                endpointIds.push_back(entry["endpointId"].string_value());
            }
            groupLog(tag.c_str(), "ActiveVideoSsrcs: %zu endpoint(s) announced", endpointIds.size());
            requestVideoFromEndpoints(statePtr, sfuH, endpointIds);
        };
    } else {
        descriptor.videoContentType = tgcalls::VideoContentType::None;
    }

    // Create instance
    if (isReference) {
        state->instance = std::make_unique<tgcalls::GroupInstanceReferenceImpl>(std::move(descriptor));
        groupLog(tag.c_str(), "created GroupInstanceReferenceImpl");
    } else {
        state->instance = std::make_unique<tgcalls::GroupInstanceCustomImpl>(std::move(descriptor));
        groupLog(tag.c_str(), "created GroupInstanceCustomImpl");
    }

    // Set connection mode
    state->instance->setConnectionMode(
        tgcalls::GroupConnectionMode::GroupConnectionModeRtc, false, false);

    // Emit join payload
    std::mutex joinMutex;
    std::condition_variable joinCv;
    bool joinReady = false;
    std::string joinJson;
    uint32_t joinSsrc = 0;

    state->instance->emitJoinPayload([&](tgcalls::GroupJoinPayload const &payload) {
        std::lock_guard<std::mutex> lock(joinMutex);
        joinJson = payload.json;
        joinSsrc = payload.audioSsrc;
        joinReady = true;
        joinCv.notify_one();
    });

    // Mirror the real app's call order: PresentationGroupCall calls
    // setRequestedVideoChannels for the participants it already knows about
    // right after the context issues emitJoinPayload, so the request lands on
    // the media thread behind the initial CreateOffer — before the join
    // response is applied and before the data channel opens. Creation is
    // sequential, so every earlier participant has already joined the SFU and
    // its simulcast SSRCs are queryable.
    if (video && earlyVideoRequest) {
        std::vector<std::string> endpointIds;
        for (const auto& other : *allStates) {
            if (other && other->sendsVideo && !other->endpointId.empty()) {
                endpointIds.push_back(other->endpointId);
            }
        }
        if (!endpointIds.empty()) {
            groupLog(tag.c_str(), "early video request for %zu endpoint(s) before the join response",
                     endpointIds.size());
            requestVideoFromEndpoints(statePtr, sfuHandle, endpointIds);
        }
    }

    {
        std::unique_lock<std::mutex> lock(joinMutex);
        if (!joinCv.wait_for(lock, std::chrono::seconds(5), [&] { return joinReady; })) {
            fprintf(stderr, "Error: emitJoinPayload timed out for participant %d\n", id);
            return nullptr;
        }
    }

    state->audioSsrc = joinSsrc;
    groupLog(tag.c_str(), "join payload ready: ssrc=%u, json=%zu bytes", joinSsrc, joinJson.size());

    // Join SFU
    GoInt iceControlling = isReference ? 0 : 1;
    char* responseRaw = GoSfu_Join(sfuHandle, (GoInt)id, const_cast<char*>(joinJson.c_str()), iceControlling);
    if (!responseRaw) {
        fprintf(stderr, "Error: GoSfu_Join returned null for participant %d\n", id);
        return nullptr;
    }
    std::string response(responseRaw);
    GoSfu_Free(responseRaw);

    if (response.find("\"error\"") != std::string::npos) {
        fprintf(stderr, "Error: GoSfu_Join failed for participant %d: %s\n", id, response.c_str());
        return nullptr;
    }

    groupLog(tag.c_str(), "SFU join response: %zu bytes", response.size());

    state->instance->setJoinResponsePayload(response);
    state->instance->setIsMuted(state->muted);

    if (video && videoFeed == VideoFeed::CaptureLate) {
        // requestVideo: — the camera goes on after the call is up.
        state->instance->setVideoCapture(state->videoCapture);
        groupLog(tag.c_str(), "video via VideoCaptureInterface after join (setVideoCapture)");
    }

    groupLog(tag.c_str(), "joined (%s)", state->muted ? "muted" : "unmuted");
    return state;
}

// ---------------------------------------------------------------------------
// stopParticipant
// ---------------------------------------------------------------------------

void stopParticipant(ParticipantState* state, GoInt sfuHandle) {
    if (!state || !state->instance) return;

    std::string tag = "P" + std::to_string(state->id);

    // Remove from SFU first so broadcasts go out to remaining participants.
    GoInt rc = GoSfu_Leave(sfuHandle, (GoInt)state->id);
    if (rc != 0) {
        groupLog(tag.c_str(), "GoSfu_Leave returned %lld (may already be removed)", (long long)rc);
    }

    // Stop video source.
    if (state->videoSource) {
        state->videoSource->Stop();
    }
    if (state->videoCapture) {
        state->videoCapture->setState(tgcalls::VideoState::Inactive);
    }

    // Stop instance with timeout. Heap-allocate sync state so the stop callback
    // is safe even if it fires after the 5s timeout (avoids stack-frame UB).
    struct StopState {
        std::mutex mu;
        std::condition_variable cv;
        std::atomic<bool> done{false};
    };
    auto stopState = std::make_shared<StopState>();

    state->instance->stop([stopState]() {
        stopState->done.store(true);
        std::lock_guard<std::mutex> lock(stopState->mu);
        stopState->cv.notify_all();
    });

    {
        std::unique_lock<std::mutex> lock(stopState->mu);
        stopState->cv.wait_for(lock, std::chrono::seconds(5), [&] { return stopState->done.load(); });
    }

    state->instance.reset();
    state->videoCapture.reset();

    // Clean up log file. TGCALLS_CLI_KEEP_LOGS=1 keeps it: the engine writes
    // the log only at stop, so this is the only way to read a passing run's
    // internal log (renegotiation counts, SDP churn).
    if (const char* keep = std::getenv("TGCALLS_CLI_KEEP_LOGS"); !(keep && keep[0] == '1')) {
        unlink(state->logPath.c_str());
    } else {
        groupLog(tag.c_str(), "log kept at %s", state->logPath.c_str());
    }

    groupLog(tag.c_str(), "stopped and cleaned up");
}

// ---------------------------------------------------------------------------
// validateGroupState
// ---------------------------------------------------------------------------

GroupValidationResult validateGroupState(
    const std::vector<std::unique_ptr<ParticipantState>>& states,
    bool video
) {
    GroupValidationResult result{};
    result.totalParticipants = static_cast<int>(states.size());

    for (const auto& s : states) {
        if (s->wasConnected.load()) result.connectedCount++;
        if (s->receivedAudio.load()) result.audioReceivedCount++;
    }

    if (video) {
        int videoParticipants = 0;
        for (const auto& s : states) {
            if (s->sendsVideo) videoParticipants++;
        }
        result.videoExpectedPairs = videoParticipants * (videoParticipants - 1);

        for (const auto& s : states) {
            std::lock_guard<std::mutex> lock(s->videoSinksMutex);
            for (const auto& [endpointId, sink] : s->videoSinks) {
                int frames = sink->frameCount();
                if (endpointId == s->endpointId) {
                    // --request-own-video: the sink for the own endpoint is
                    // never a received pair (the SFU forwards nothing to its
                    // sender), so it must not inflate the count. With a
                    // VideoCaptureInterface feed the engine must serve it from
                    // the camera preview instead (the app's local tile).
                    if (s->videoViaCapture && frames == 0) {
                        result.ownPreviewMissing++;
                        groupLog("Validate", "FAIL: P%d (%s) own endpoint %s: 0 preview frames on its own sink",
                                 s->id, s->isReference ? "ref" : "custom", endpointId.c_str());
                    } else {
                        groupLog("Validate", "P%d <- own endpoint %s: %d preview frames (not a pair)",
                                 s->id, endpointId.c_str(), frames);
                    }
                    continue;
                }
                if (frames > 0) {
                    result.videoReceivedPairs++;
                }
                groupLog("Validate", "P%d <- endpoint %s: %d video frames (%dx%d)",
                         s->id, endpointId.c_str(), frames,
                         sink->lastWidth(), sink->lastHeight());
                if (s->videoRerequested) {
                    // The sink kept counting across the drop, so frames that
                    // arrived before the drop must not satisfy the check: only
                    // growth past the recorded count proves the re-requested
                    // transceiver is delivering.
                    auto before = s->videoFramesAtRerequest.find(endpointId);
                    int framesBefore = before != s->videoFramesAtRerequest.end() ? before->second : 0;
                    result.videoRerequestExpectedPairs++;
                    if (frames <= framesBefore) {
                        result.videoRerequestStalledPairs++;
                        groupLog("Validate", "FAIL: P%d (%s) <- endpoint %s: no video frame after the re-request (%d before, %d now)",
                                 s->id, s->isReference ? "ref" : "custom", endpointId.c_str(), framesBefore, frames);
                    } else {
                        groupLog("Validate", "OK:   P%d (%s) <- endpoint %s: %d frame(s) after the re-request",
                                 s->id, s->isReference ? "ref" : "custom", endpointId.c_str(), frames - framesBefore);
                    }
                }
            }
        }

        // The engine must never ask the SFU for the participant's own video,
        // whatever the app put in the requested list (see requestOwnVideo).
        for (const auto& s : states) {
            int layer = s->sfuSelfRequestedLayer.load();
            if (layer >= 0) {
                result.selfVideoRequests++;
                groupLog("Validate", "FAIL: P%d (%s) asked the SFU to forward its OWN video (endpoint %s, layer %d)",
                         s->id, s->isReference ? "ref" : "custom", s->endpointId.c_str(), layer);
            }
        }
    }

    // Audio-level invariant: no peer may report a muted participant's SSRC at
    // a non-trivial level. The synthetic 0.1 level reported by a buggy
    // ReferenceImpl will trip this check; a real PCM-derived level reads ~0
    // for an audio track that is producing silence. Note this check alone
    // cannot tell silence from no stream at all — the wire check below can.
    constexpr float kMutedLevelThreshold = 0.05f;
    bool mutedInvariantHeld = true;

    // Determine: is the test only validating connection/audio? In a full
    // group the existing audioReceivedCount check requires every participant
    // (including muted ones) to receive audio from someone — that's still
    // valid as long as at least one peer is unmuted, but a muted participant
    // can't produce audio for OTHERS, so we relax: every UNMUTED participant
    // must have receivedAudio.
    int unmutedReceived = 0;
    int unmutedTotal = 0;
    for (const auto& s : states) {
        if (!s->muted) {
            unmutedTotal++;
            if (s->receivedAudio.load()) unmutedReceived++;
        }
    }

    for (const auto& muted : states) {
        if (!muted->muted || muted->audioSsrc == 0) continue;
        for (const auto& peer : states) {
            if (peer.get() == muted.get()) continue;
            std::lock_guard<std::mutex> lock(peer->audioLevelsMutex);
            auto it = peer->maxAudioLevelPerSsrc.find(muted->audioSsrc);
            float maxLevel = (it != peer->maxAudioLevelPerSsrc.end()) ? it->second : 0.0f;
            if (maxLevel >= kMutedLevelThreshold) {
                groupLog("Validate",
                         "FAIL: P%d (%s) reported muted P%d (ssrc=%u) at level %.3f (>= %.3f)",
                         peer->id, peer->isReference ? "ref" : "custom",
                         muted->id, muted->audioSsrc, maxLevel, kMutedLevelThreshold);
                mutedInvariantHeld = false;
            } else {
                groupLog("Validate",
                         "OK:   P%d (%s) reported muted P%d (ssrc=%u) at max level %.3f",
                         peer->id, peer->isReference ? "ref" : "custom",
                         muted->id, muted->audioSsrc, maxLevel);
            }
        }
    }

    // Wire invariant: a participant that stayed muted for the whole run must
    // not have sent a single audio RTP packet to the SFU. A merely disabled
    // track still encodes and sends silence, which decodes to level ~0 and so
    // passes the level check above while costing the SFU and every peer a full
    // Opus stream. Counts come from the SFU before teardown; -1 means the run
    // never queried them (group-churn).
    for (const auto& s : states) {
        int64_t packets = s->sfuAudioPacketsReceived.load();
        if (packets < 0 || s->audioSsrc == 0) continue;
        if (s->muted) {
            result.mutedParticipants++;
            if (packets > 0) {
                result.mutedAudioLeaks++;
                groupLog("Validate",
                         "FAIL: muted P%d (%s) sent %lld audio RTP packets to the SFU (ssrc=%u)",
                         s->id, s->isReference ? "ref" : "custom", (long long)packets, s->audioSsrc);
            } else {
                groupLog("Validate",
                         "OK:   muted P%d (%s) sent no audio RTP to the SFU (ssrc=%u)",
                         s->id, s->isReference ? "ref" : "custom", s->audioSsrc);
            }
        } else {
            groupLog("Validate",
                     "P%d (%s) sent %lld audio RTP packets to the SFU (ssrc=%u)",
                     s->id, s->isReference ? "ref" : "custom", (long long)packets, s->audioSsrc);
        }
    }

    // Late unmute: a participant that was silent while everyone else joined and
    // renegotiated, and only then started sending. Its SSRC is brand new to every
    // peer at a point where their audio m-lines already exist, which is the case
    // the app hits when someone turns their microphone on mid-call. Every peer
    // must end up hearing it.
    for (const auto& late : states) {
        if (!late->unmutedLate || late->audioSsrc == 0) continue;
        for (const auto& peer : states) {
            if (peer.get() == late.get()) continue;
            result.lateUnmuteExpectedPairs++;
            std::lock_guard<std::mutex> lock(peer->audioLevelsMutex);
            auto it = peer->maxAudioLevelPerSsrc.find(late->audioSsrc);
            float maxLevel = (it != peer->maxAudioLevelPerSsrc.end()) ? it->second : 0.0f;
            if (maxLevel >= kMutedLevelThreshold) {
                result.lateUnmuteHeardPairs++;
                groupLog("Validate",
                         "OK:   P%d (%s) heard late-unmuted P%d (ssrc=%u) at max level %.3f",
                         peer->id, peer->isReference ? "ref" : "custom",
                         late->id, late->audioSsrc, maxLevel);
            } else {
                groupLog("Validate",
                         "FAIL: P%d (%s) never heard late-unmuted P%d (ssrc=%u), max level %.3f",
                         peer->id, peer->isReference ? "ref" : "custom",
                         late->id, late->audioSsrc, maxLevel);
            }
        }
    }

    // Every unmuted sender must reach every peer's level reports, not only be
    // audible. Levels come from the sink on the sender's dedicated receiver
    // track; a receiver whose packets are stolen by a stray catch-all stream
    // still plays audio (the catch-all decodes it) but never reports a level,
    // so the app would never show that participant as speaking.
    for (const auto& sender : states) {
        if (sender->muted || sender->audioSsrc == 0) continue;
        for (const auto& peer : states) {
            if (peer.get() == sender.get()) continue;
            result.audioLevelExpectedPairs++;
            std::lock_guard<std::mutex> lock(peer->audioLevelsMutex);
            auto it = peer->maxAudioLevelPerSsrc.find(sender->audioSsrc);
            float maxLevel = (it != peer->maxAudioLevelPerSsrc.end()) ? it->second : 0.0f;
            if (maxLevel >= kMutedLevelThreshold) {
                result.audioLevelHeardPairs++;
            } else {
                groupLog("Validate",
                         "FAIL: P%d (%s) never reported a level for P%d (ssrc=%u), max %.3f",
                         peer->id, peer->isReference ? "ref" : "custom",
                         sender->id, sender->audioSsrc, maxLevel);
            }
        }
    }

    // Every participant's log sink receives the whole process's RTC log while
    // that participant is alive, so the first participant's file holds every
    // engine's lines from the start of the run until that participant was
    // stopped, which is after the whole call ran. Scan it once.
    for (const auto& s : states) {
        FILE* f = fopen(s->logPath.c_str(), "r");
        if (!f) continue;
        static const char kNeedle[] = "Creating unsignaled receive stream for SSRC=";
        char line[4096];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, kNeedle)) {
                result.unsignaledAudioStreams++;
                groupLog("Validate", "FAIL: %s", line);
            }
        }
        fclose(f);
        break;
    }

    bool hasMuted = false;
    for (const auto& s : states) {
        if (s->muted) { hasMuted = true; break; }
    }

    if (hasMuted) {
        // Relaxed audio-received check: only unmuted participants are required
        // to receive remote audio.
        result.success = (result.connectedCount == result.totalParticipants &&
                          unmutedReceived == unmutedTotal &&
                          mutedInvariantHeld);
    } else {
        result.success = (result.connectedCount == result.totalParticipants &&
                          result.audioReceivedCount == result.totalParticipants);
    }
    if (video && result.videoExpectedPairs > 0) {
        result.success = result.success && (result.videoReceivedPairs >= result.videoExpectedPairs);
    }
    if (video) {
        result.success = result.success && (result.selfVideoRequests == 0) && (result.ownPreviewMissing == 0);
    }
    if (result.videoRerequestExpectedPairs > 0) {
        result.success = result.success && (result.videoRerequestStalledPairs == 0);
    }
    if (result.lateUnmuteExpectedPairs > 0) {
        result.success = result.success &&
                         (result.lateUnmuteHeardPairs == result.lateUnmuteExpectedPairs);
    }
    result.success = result.success && (result.mutedAudioLeaks == 0);
    result.success = result.success && (result.audioLevelHeardPairs == result.audioLevelExpectedPairs);
    result.success = result.success && (result.unsignaledAudioStreams == 0);

    return result;
}

// ---------------------------------------------------------------------------
// printGroupSummary
// ---------------------------------------------------------------------------

bool printGroupSummary(
    int customParticipants,
    int referenceParticipants,
    int duration,
    bool video,
    const GroupValidationResult& result,
    bool anyFailed
) {
    bool success = result.success && !anyFailed;

    printf("\n=== Group Call Summary ===\n");
    printf("Custom participants:    %d\n", customParticipants);
    printf("Reference participants: %d\n", referenceParticipants);
    printf("Total participants:     %d\n", result.totalParticipants);
    printf("Duration:               %ds\n", duration);
    printf("SFU:                    Go/Pion (in-process)\n");
    printf("Connected:              %d/%d\n", result.connectedCount, result.totalParticipants);
    printf("Audio received:         %d/%d\n", result.audioReceivedCount, result.totalParticipants);
    printf("Audio levels heard:     %d/%d pairs\n", result.audioLevelHeardPairs, result.audioLevelExpectedPairs);
    printf("Unsignaled audio:       %d stream(s) (must be 0)\n", result.unsignaledAudioStreams);
    if (video) {
        printf("Video received:         %d/%d\n", result.videoReceivedPairs, result.videoExpectedPairs);
        printf("Self video requests:    %d (must be 0)\n", result.selfVideoRequests);
        printf("Own preview missing:    %d (must be 0)\n", result.ownPreviewMissing);
    }
    if (result.videoRerequestExpectedPairs > 0) {
        printf("Video after re-request: %d/%d stalled (must be 0)\n", result.videoRerequestStalledPairs, result.videoRerequestExpectedPairs);
    }
    if (result.lateUnmuteExpectedPairs > 0) {
        printf("Late unmute heard:      %d/%d\n", result.lateUnmuteHeardPairs, result.lateUnmuteExpectedPairs);
    }
    if (result.mutedParticipants > 0) {
        printf("Muted audio leaks:      %d/%d (must be 0)\n", result.mutedAudioLeaks, result.mutedParticipants);
    }
    printf("Result:                 %s\n", success ? "SUCCESS" : "FAILED");

    return success;
}
