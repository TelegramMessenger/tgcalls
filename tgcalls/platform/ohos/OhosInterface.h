// OhosInterface — tgcalls PlatformInterface implementation for HarmonyOS.
//
// Bridges tgcalls into the OHOS-native media stack shipped by libohos_webrtc.so:
//   - video encoding/decoding via webrtc::adapter::SoftwareVideoEncoderFactory /
//     SoftwareVideoDecoderFactory (builtin VP8/VP9/H264 software codecs)
//   - camera capture via webrtc::CameraCapturer (OH_CameraManager based),
//     carried by webrtc::OhosVideoTrackSource into the shared VideoBroadcaster
//   - audio remains managed by libohos_webrtc's OhosAudioDeviceModule
#ifndef TGCALLS_OHOS_INTERFACE_H
#define TGCALLS_OHOS_INTERFACE_H

#include "platform/PlatformInterface.h"

#include "api/media_stream_interface.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "rtc_base/thread.h"

#include <memory>
#include <string>
#include <utility>

namespace tgcalls {

class OhosInterface : public PlatformInterface {
public:
    OhosInterface() = default;
    ~OhosInterface() override = default;

    void configurePlatformAudio(int numChannels) override;

    std::unique_ptr<webrtc::VideoEncoderFactory> makeVideoEncoderFactory(bool preferHardwareEncoding, bool isScreencast) override;
    std::unique_ptr<webrtc::VideoDecoderFactory> makeVideoDecoderFactory() override;
    bool supportsEncoding(const std::string &codecName) override;
    webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> makeVideoSource(webrtc::Thread *signalingThread, webrtc::Thread *workerThread) override;
    void adaptVideoSource(webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> videoSource, int width, int height, int fps) override;
    std::unique_ptr<VideoCapturerInterface> makeVideoCapturer(
        webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source,
        std::string deviceId,
        std::function<void(VideoState)> stateUpdated,
        std::function<void(PlatformCaptureInfo)> captureInfoUpdated,
        std::shared_ptr<PlatformContext> platformContext,
        std::pair<int, int> &outResolution) override;
};

}  // namespace tgcalls

#endif  // TGCALLS_OHOS_INTERFACE_H
