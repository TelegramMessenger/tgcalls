#include "broadcast_listener.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <unistd.h>

#include "broadcast_client.h"

namespace {

// A listener's microphone: silence (it is muted anyway; the fake device still pulls frames).
class SilentRecorder : public tgcalls::FakeAudioDeviceModule::Recorder {
public:
    SilentRecorder() : _buffer(480 * 2, 0) {
    }

    tgcalls::AudioFrame Record() override {
        tgcalls::AudioFrame frame;
        frame.audio_samples = _buffer.data();
        frame.num_samples = 480;
        frame.bytes_per_sample = sizeof(int16_t);
        frame.num_channels = 2;
        frame.samples_per_sec = 48000;
        frame.elapsed_time_ms = 0;
        frame.ntp_time_ms = 0;
        return frame;
    }

private:
    std::vector<int16_t> _buffer;
};

} // namespace

std::unique_ptr<ParticipantState> createBroadcastListener(int id, GoInt sfuHandle, std::shared_ptr<tgcalls::Threads> threads,
                                                          BroadcastListenerOptions options) {
    auto state = std::make_unique<ParticipantState>();
    state->id = id;
    state->isReference = false;
    state->muted = true;
    state->broadcastUnified = options.unified;
    state->broadcastAlive = std::make_shared<std::atomic<bool>>(true);
    state->logPath = "/tmp/tgcalls_broadcast_p" + std::to_string(id) + "_" + std::to_string(getpid()) + ".log";

    std::string tag = "L" + std::to_string(id);
    ParticipantState *statePtr = state.get();

    tgcalls::GroupInstanceDescriptor descriptor;
    descriptor.threads = threads;
    descriptor.config.need_log = true;
    descriptor.config.logPath = {state->logPath};
    descriptor.networkStateUpdated = [statePtr, tag](tgcalls::GroupNetworkState networkState) {
        groupLog(tag.c_str(), "network state: connected=%d transitioning=%d", networkState.isConnected ? 1 : 0,
                 networkState.isTransitioningFromBroadcastToRtc ? 1 : 0);
        statePtr->connected.store(networkState.isConnected);
        if (networkState.isConnected) {
            statePtr->wasConnected.store(true);
        }
    };
    descriptor.audioLevelsUpdated = [statePtr](tgcalls::GroupLevelsUpdate const &update) {
        std::lock_guard<std::mutex> lock(statePtr->audioLevelsMutex);
        for (const auto &level : update.updates) {
            if (level.ssrc != 0 && level.value.level > 0.05f) {
                statePtr->receivedAudio.store(true);
            }
            if (level.ssrc != 0) {
                auto &slot = statePtr->maxAudioLevelPerSsrc[level.ssrc];
                if (level.value.level > slot) {
                    slot = level.value.level;
                }
            }
        }
    };
    descriptor.createAudioDeviceModule = tgcalls::FakeAudioDeviceModule::Creator(
        options.renderer, std::make_shared<SilentRecorder>(),
        tgcalls::FakeAudioDeviceModule::Options{.samples_per_sec = 48000, .num_channels = 2});
    // After promotion the engine asks who unknown audio SSRCs belong to.
    descriptor.requestMediaChannelDescriptions = [sfuHandle](std::vector<uint32_t> const &ssrcs,
        std::function<void(std::vector<tgcalls::MediaChannelDescription> &&)> callback) -> std::shared_ptr<tgcalls::RequestMediaChannelDescriptionTask> {
        std::vector<tgcalls::MediaChannelDescription> descriptions;
        for (uint32_t ssrc : ssrcs) {
            tgcalls::MediaChannelDescription description;
            description.type = tgcalls::MediaChannelDescription::Type::Audio;
            description.audioSsrc = ssrc;
            description.userId = cliUserId((int)GoSfu_QuerySsrc(sfuHandle, (GoUint)ssrc));
            descriptions.push_back(std::move(description));
        }
        callback(std::move(descriptions));
        return std::make_shared<SimpleRequestMediaChannelDescriptionTask>();
    };

    auto callbacks = makeBroadcastRequestCallbacks(sfuHandle, options.clientId, options.unified, state->broadcastAlive,
                                                   options.onRejoinNeeded);
    descriptor.requestCurrentTime = callbacks.requestCurrentTime;
    descriptor.requestAudioBroadcastPart = callbacks.requestAudioBroadcastPart;
    descriptor.requestVideoBroadcastPart = callbacks.requestVideoBroadcastPart;

    descriptor.outgoingAudioBitrateKbit = 32;
    descriptor.disableAudioInput = options.unified;
    descriptor.disableIncomingChannels = false;
    descriptor.useDummyChannel = true;
    descriptor.videoContentType = tgcalls::VideoContentType::None;

    state->instance = std::make_unique<tgcalls::GroupInstanceCustomImpl>(std::move(descriptor));
    state->instance->setIsMuted(true);
    state->instance->setConnectionMode(tgcalls::GroupConnectionMode::GroupConnectionModeBroadcast, false, options.unified);
    groupLog(tag.c_str(), "listener in broadcast mode (%s)", options.unified ? "unified" : "listener audio");
    return state;
}

bool promoteListenerToSpeaker(ParticipantState *listener, GoInt sfuHandle) {
    std::string tag = "L" + std::to_string(listener->id);
    listener->instance->setConnectionMode(tgcalls::GroupConnectionMode::GroupConnectionModeNone, true, false);

    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    std::string json;
    uint32_t ssrc = 0;
    listener->instance->emitJoinPayload([&](tgcalls::GroupJoinPayload const &payload) {
        std::lock_guard<std::mutex> lock(mutex);
        json = payload.json;
        ssrc = payload.audioSsrc;
        ready = true;
        cv.notify_one();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(5), [&] { return ready; })) {
            fprintf(stderr, "Error: emitJoinPayload timed out promoting listener %d\n", listener->id);
            return false;
        }
    }
    listener->audioSsrc = ssrc;

    char *responseRaw = GoSfu_Join(sfuHandle, (GoInt)listener->id, const_cast<char *>(json.c_str()), 1);
    if (!responseRaw) {
        fprintf(stderr, "Error: GoSfu_Join returned null promoting listener %d\n", listener->id);
        return false;
    }
    std::string response(responseRaw);
    GoSfu_Free(responseRaw);
    if (response.find("\"error\"") != std::string::npos) {
        fprintf(stderr, "Error: GoSfu_Join failed promoting listener %d: %s\n", listener->id, response.c_str());
        return false;
    }

    listener->instance->setConnectionMode(tgcalls::GroupConnectionMode::GroupConnectionModeRtc, false, false);
    listener->instance->setJoinResponsePayload(response);
    listener->instance->setIsMuted(true);
    groupLog(tag.c_str(), "promoted to speaker (ssrc %u), muted", ssrc);
    return true;
}

void rejoinBroadcast(ParticipantState *listener) {
    listener->instance->setConnectionMode(tgcalls::GroupConnectionMode::GroupConnectionModeNone, false, false);
    listener->instance->setConnectionMode(tgcalls::GroupConnectionMode::GroupConnectionModeBroadcast, false,
                                          listener->broadcastUnified);
}
