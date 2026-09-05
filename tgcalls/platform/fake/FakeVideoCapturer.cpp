#include "FakeVideoCapturer.h"

#include "api/video/i420_buffer.h"
#include "api/video/video_rotation.h"
#include "rtc_base/time_utils.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace tgcalls {

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kFps = 30;
constexpr uint8_t kBackgroundY = 80;
constexpr uint8_t kBarY = 235;
constexpr int kBarWidth = 64;

struct UVTint { uint8_t u; uint8_t v; };
// red, green, blue, yellow, cyan, magenta — the same order as the CLI's
// FakeVideoTrackSource, so a participant looks the same whichever feed it uses.
constexpr UVTint kTints[6] = {
    {90, 240}, {54, 34}, {240, 110}, {16, 146}, {166, 16}, {166, 240},
};

} // namespace

rtc::scoped_refptr<FakePlatformVideoSource> FakePlatformVideoSource::Create() {
    return rtc::scoped_refptr<FakePlatformVideoSource>(
        new rtc::RefCountedObject<FakePlatformVideoSource>());
}

void FakePlatformVideoSource::PushFrame(const webrtc::VideoFrame &frame) {
    OnFrame(frame);
}

FakeVideoCapturer::FakeVideoCapturer(rtc::scoped_refptr<FakePlatformVideoSource> source,
                                     std::string deviceId,
                                     std::function<void(VideoState)> stateUpdated)
: _source(std::move(source))
, _stateUpdated(std::move(stateUpdated)) {
    int tintIndex = std::atoi(deviceId.c_str());
    if (tintIndex < 0) tintIndex = -tintIndex;
    _uTint = kTints[tintIndex % 6].u;
    _vTint = kTints[tintIndex % 6].v;
}

FakeVideoCapturer::~FakeVideoCapturer() {
    stop();
}

void FakeVideoCapturer::setState(VideoState state) {
    if (state == VideoState::Active) {
        start();
    } else {
        stop();
    }
    if (_stateUpdated) {
        _stateUpdated(state);
    }
}

void FakeVideoCapturer::setUncroppedOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
    std::lock_guard<std::mutex> lock(_sinkMutex);
    _uncroppedSink = std::move(sink);
}

void FakeVideoCapturer::start() {
    if (_running.exchange(true)) return;
    _thread = std::thread(&FakeVideoCapturer::generateThread, this);
}

void FakeVideoCapturer::stop() {
    if (!_running.exchange(false)) return;
    if (_thread.joinable()) {
        _thread.join();
    }
}

void FakeVideoCapturer::generateThread() {
    int frameNumber = 0;
    while (_running.load(std::memory_order_relaxed)) {
        auto buffer = webrtc::I420Buffer::Create(kWidth, kHeight);

        memset(buffer->MutableDataY(), kBackgroundY, buffer->StrideY() * kHeight);
        const int uvHeight = (kHeight + 1) / 2;
        memset(buffer->MutableDataU(), _uTint, buffer->StrideU() * uvHeight);
        memset(buffer->MutableDataV(), _vTint, buffer->StrideV() * uvHeight);

        // A bright bar sweeping left to right: motion for the encoder.
        const int barX = (frameNumber * 8) % (kWidth - kBarWidth);
        for (int y = kHeight / 4; y < kHeight * 3 / 4; ++y) {
            memset(buffer->MutableDataY() + y * buffer->StrideY() + barX, kBarY, kBarWidth);
        }

        auto frame = webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(buffer)
            .set_rotation(webrtc::kVideoRotation_0)
            .set_timestamp_us(rtc::TimeMicros())
            .build();

        _source->PushFrame(frame);

        std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink;
        {
            std::lock_guard<std::mutex> lock(_sinkMutex);
            sink = _uncroppedSink;
        }
        if (sink) {
            sink->OnFrame(frame);
        }

        ++frameNumber;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / kFps));
    }
}

} // namespace tgcalls
