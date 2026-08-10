// VideoCameraCapturer — tgcalls VideoCapturerInterface adapter over the OHOS
// video stack (webrtc::OhosVideoTrackSource driving a webrtc::CameraCapturer).
//
// The shared VideoTrackSourceInterface created by OhosInterface::makeVideoSource
// IS the OhosVideoTrackSource. This adapter drives it (Start/Stop/SwitchCamera)
// and forwards capture start/stop events to the tgcalls state callback. Frames
// flow:
//   OH_CameraManager capture
//     -> webrtc::CameraCapturer (sdk/ohos camera_capturer)
//     -> webrtc::OhosVideoTrackSource (adapt + broadcast to registered sinks)
//     -> outgoing video channel (subscribed via the public interface)
#ifndef TGCALLS_OHOS_VIDEO_CAMERA_CAPTURER_H
#define TGCALLS_OHOS_VIDEO_CAMERA_CAPTURER_H

#include "VideoCapturerInterface.h"
#include "VideoCaptureInterface.h"
#include "Instance.h"
#include "platform/PlatformInterface.h"

#include "video/video_capturer.h"
#include "video/video_track_source.h"

#include "api/media_stream_interface.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"

#include <functional>
#include <memory>
#include <string>

namespace tgcalls {

class VideoCameraCapturer : public VideoCapturerInterface, public webrtc::VideoCapturer::Observer {
public:
    VideoCameraCapturer(
        webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source,
        std::string deviceId,
        std::function<void(VideoState)> stateUpdated,
        std::function<void(PlatformCaptureInfo)> captureInfoUpdated);
    ~VideoCameraCapturer() override;

    // VideoCapturerInterface
    void setState(VideoState state) override;
    void setPreferredCaptureAspectRatio(float aspectRatio) override;
    void setUncroppedOutput(std::shared_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> sink) override;
    int getRotation() override;
    void setOnFatalError(std::function<void()> error) override;
    void setOnPause(std::function<void(bool)> pause) override;
    void withNativeImplementation(std::function<void(void *)> completion) override;

    // webrtc::VideoCapturer::Observer
    void OnCapturerStarted(bool success) override;
    void OnCapturerStopped() override;
    void OnFrameCaptured(
        webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer,
        int64_t timestampUs,
        webrtc::VideoRotation rotation) override;

private:
    webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> _source;
    webrtc::scoped_refptr<webrtc::OhosVideoTrackSource> _videoTrackSource;
    std::function<void(VideoState)> _stateUpdated;
    std::function<void(PlatformCaptureInfo)> _captureInfoUpdated;
    std::function<void()> _onFatalError;
    std::function<void(bool)> _onPause;
    std::shared_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> _uncroppedSink;
    std::string _deviceId;
    VideoState _state = VideoState::Inactive;
    float _preferredAspectRatio = 0.0f;
};

}  // namespace tgcalls

#endif  // TGCALLS_OHOS_VIDEO_CAMERA_CAPTURER_H
