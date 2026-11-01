#include "broadcast_client.h"

#include <thread>
#include <utility>
#include <vector>

namespace {

// Server kinds and statuses (tools/go_sfu/broadcast_exports.go).
constexpr int kKindAudio = 0;
constexpr int kKindVideo = 1;
constexpr int kKindUnified = 2;
constexpr int kStatusSuccess = 0;
constexpr int kStatusNotReady = 1;
constexpr int kStatusJoinMissing = 3;

class FetchTask : public tgcalls::BroadcastPartTask {
public:
    explicit FetchTask(std::shared_ptr<std::atomic<bool>> cancelled) :
    _cancelled(std::move(cancelled)) {
    }

    void cancel() override {
        _cancelled->store(true);
    }

private:
    std::shared_ptr<std::atomic<bool>> _cancelled;
};

int serverQuality(tgcalls::VideoChannelDescription::Quality quality) {
    switch (quality) {
    case tgcalls::VideoChannelDescription::Quality::Thumbnail:
        return 0;
    case tgcalls::VideoChannelDescription::Quality::Medium:
        return 1;
    case tgcalls::VideoChannelDescription::Quality::Full:
        return 2;
    }
    return 2;
}

tgcalls::BroadcastPart::Status partStatus(int status) {
    switch (status) {
    case kStatusSuccess:
        return tgcalls::BroadcastPart::Status::Success;
    case kStatusNotReady:
        return tgcalls::BroadcastPart::Status::NotReady;
    default:
        return tgcalls::BroadcastPart::Status::ResyncNeeded;
    }
}

std::shared_ptr<tgcalls::BroadcastPartTask> startFetch(GoInt sfu, int clientId, int kind, int64_t timestampMs, int channelId,
                                                       int quality, std::shared_ptr<std::atomic<bool>> alive,
                                                       std::function<void()> onRejoinNeeded,
                                                       std::function<void(tgcalls::BroadcastPart &&)> completion) {
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    std::thread([sfu, clientId, kind, timestampMs, channelId, quality, alive, cancelled, onRejoinNeeded, completion]() {
        int status = 0;
        long long responseMs = 0;
        int length = 0;
        char *data = GoSfu_BroadcastFetch(sfu, clientId, kind, timestampMs, channelId, quality, &status, &responseMs, &length);
        std::vector<uint8_t> bytes;
        if (data) {
            bytes.assign((uint8_t *)data, (uint8_t *)data + length);
            GoSfu_Free(data);
        }
        if (cancelled->load() || !alive->load()) {
            return;
        }
        if (status == kStatusJoinMissing) {
            onRejoinNeeded();
            return;
        }
        tgcalls::BroadcastPart part;
        part.timestampMilliseconds = timestampMs;
        part.responseTimestamp = (double)responseMs / 1000.0;
        part.status = partStatus(status);
        part.data = std::move(bytes);
        completion(std::move(part));
    }).detach();
    return std::make_shared<FetchTask>(cancelled);
}

} // namespace

BroadcastRequestCallbacks makeBroadcastRequestCallbacks(GoInt sfuHandle, int clientId, bool unified,
                                                        std::shared_ptr<std::atomic<bool>> alive,
                                                        std::function<void()> onRejoinNeeded) {
    BroadcastRequestCallbacks callbacks;
    callbacks.requestCurrentTime = [sfuHandle, unified, alive](std::function<void(int64_t)> completion) -> std::shared_ptr<tgcalls::BroadcastPartTask> {
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        std::thread([sfuHandle, unified, alive, cancelled, completion]() {
            int64_t timestamp = unified ? GoSfu_BroadcastLastPartMs(sfuHandle) : GoSfu_BroadcastServerTimeMs(sfuHandle);
            if (!cancelled->load() && alive->load()) {
                completion(timestamp);
            }
        }).detach();
        return std::make_shared<FetchTask>(cancelled);
    };
    callbacks.requestAudioBroadcastPart = [sfuHandle, clientId, alive, onRejoinNeeded](int64_t timestampMs, int64_t, std::function<void(tgcalls::BroadcastPart &&)> completion) {
        return startFetch(sfuHandle, clientId, kKindAudio, timestampMs, 0, 0, alive, onRejoinNeeded, std::move(completion));
    };
    callbacks.requestVideoBroadcastPart = [sfuHandle, clientId, unified, alive, onRejoinNeeded](int64_t timestampMs, int64_t, int32_t channelId, tgcalls::VideoChannelDescription::Quality quality, std::function<void(tgcalls::BroadcastPart &&)> completion) {
        int kind = unified ? kKindUnified : kKindVideo;
        return startFetch(sfuHandle, clientId, kind, timestampMs, channelId, serverQuality(quality), alive, onRejoinNeeded, std::move(completion));
    };
    return callbacks;
}
