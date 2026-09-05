#pragma once

#include "VideoCaptureInterface.h"
#include "VideoCapturerInterface.h"

#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "media/base/adapted_video_track_source.h"
#include "rtc_base/ref_counted_object.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace tgcalls {

// The fake platform's video source (PlatformInterface::makeVideoSource). It
// produces nothing on its own: FakeVideoCapturer pushes frames into it, the
// way the iOS capturer pushes into the platform's ObjC track source. That is
// what lets a host test drive an engine through the app's real contract —
// VideoCaptureInterface::Create → descriptor.videoCapture / setVideoCapture —
// instead of the CLI-only descriptor.getVideoSource shortcut.
class FakePlatformVideoSource : public rtc::AdaptedVideoTrackSource {
public:
    static rtc::scoped_refptr<FakePlatformVideoSource> Create();

    void PushFrame(const webrtc::VideoFrame &frame);

    SourceState state() const override { return kLive; }
    bool remote() const override { return false; }
    bool is_screencast() const override { return false; }
    absl::optional<bool> needs_denoising() const override { return false; }

protected:
    FakePlatformVideoSource() = default;
};

// Fake camera (PlatformInterface::makeVideoCapturer): while Active, generates
// 1280x720 I420 frames at 30 fps — a colour tint chosen by `deviceId` (parsed
// as an integer; the CLI passes the participant id) with a bar sweeping across
// so the encoder sees motion — into the platform source and the uncropped
// preview sink. 720p is what three simulcast layers need.
class FakeVideoCapturer : public VideoCapturerInterface {
public:
    FakeVideoCapturer(rtc::scoped_refptr<FakePlatformVideoSource> source,
                      std::string deviceId,
                      std::function<void(VideoState)> stateUpdated);
    ~FakeVideoCapturer() override;

    void setState(VideoState state) override;
    void setPreferredCaptureAspectRatio(float aspectRatio) override {}
    void setUncroppedOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) override;
    int getRotation() override { return 0; }

private:
    void start();
    void stop();
    void generateThread();

    rtc::scoped_refptr<FakePlatformVideoSource> _source;
    std::function<void(VideoState)> _stateUpdated;
    uint8_t _uTint = 128;
    uint8_t _vTint = 128;

    std::mutex _sinkMutex;
    std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> _uncroppedSink;

    std::atomic<bool> _running{false};
    std::thread _thread;
};

} // namespace tgcalls
