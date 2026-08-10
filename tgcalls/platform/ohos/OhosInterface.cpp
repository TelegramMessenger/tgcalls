// OhosInterface implementation — see OhosInterface.h.
#include "OhosInterface.h"

#include <rtc_base/logging.h>

#include <stddef.h>
#include <string>

#include "camera/camera_capturer.h"
#include "helper/camera.h"
#include "video/video_info.h"
#include "video/video_track_source.h"
#include "video_codec/software_video_decoder_factory.h"
#include "video_codec/software_video_encoder_factory.h"

#include "VideoCameraCapturer.h"

namespace tgcalls {

void OhosInterface::configurePlatformAudio(int numChannels) {
    // Audio I/O is owned by libohos_webrtc's OhosAudioDeviceModule
    // (CreateDefaultAudioDeviceModule()); nothing to configure here.
}

std::unique_ptr<webrtc::VideoEncoderFactory> OhosInterface::makeVideoEncoderFactory(bool preferHardwareEncoding, bool isScreencast) {
    // Software (builtin VP8/VP9/H264) encoder factory, exported by libohos_webrtc.so.
    RTC_LOG(LS_INFO) << "OhosInterface::makeVideoEncoderFactory";
    return std::make_unique<webrtc::adapter::SoftwareVideoEncoderFactory>();
}

std::unique_ptr<webrtc::VideoDecoderFactory> OhosInterface::makeVideoDecoderFactory() {
    RTC_LOG(LS_INFO) << "OhosInterface::makeVideoDecoderFactory";
    return std::make_unique<webrtc::adapter::SoftwareVideoDecoderFactory>();
}

bool OhosInterface::supportsEncoding(const std::string &codecName) {
    webrtc::adapter::SoftwareVideoEncoderFactory factory;
    for (const webrtc::SdpVideoFormat &format : factory.GetSupportedFormats()) {
        if (format.name == codecName) {
            return true;
        }
    }
    return false;
}

webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> OhosInterface::makeVideoSource(webrtc::Thread *signalingThread, webrtc::Thread *workerThread) {
    // The OHOS video track source IS the shared VideoTrackSourceInterface handed
    // to MediaManager -> SetVideoSend. The outgoing video channel subscribes
    // through the public interface (AddOrUpdateSink), which dispatches into
    // OhosVideoTrackSource::AddOrUpdateSink and auto-starts the camera as soon as
    // the first sink (local preview or the video send channel) is registered.
    std::string cameraId;
    {
        auto devices = ohos::CameraManager::GetInstance().GetSupportedCameras();
        for (std::size_t i = 0; i < devices.Size(); i++) {
            Camera_Device *device = devices[i];
            if (device != nullptr && device->cameraId != nullptr && device->cameraPosition == CAMERA_POSITION_FRONT) {
                cameraId = device->cameraId;
                break;
            }
        }
        if (cameraId.empty() && devices.Size() > 0 && devices[0] != nullptr && devices[0]->cameraId != nullptr) {
            cameraId = devices[0]->cameraId;
        }
    }
    if (cameraId.empty()) {
        RTC_LOG(LS_ERROR) << "OhosInterface::makeVideoSource: no camera available";
        return nullptr;
    }
    RTC_LOG(LS_INFO) << "OhosInterface::makeVideoSource cameraId=" << cameraId;

    // Note: sdk/ohos video_info.h declares its types in the top-level `video`
    // namespace (not webrtc::video).
    video::VideoProfile profile;
    profile.format = video::PixelFormat::YU12;
    profile.resolution = {1280, 720};
    profile.frameRateRange = {15, 30};
    std::unique_ptr<webrtc::CameraCapturer> capturer = webrtc::CameraCapturer::Create(cameraId, profile);
    if (!capturer) {
        RTC_LOG(LS_ERROR) << "OhosInterface::makeVideoSource: CameraCapturer::Create failed";
        return nullptr;
    }

    return webrtc::OhosVideoTrackSource::Create(std::move(capturer), signalingThread, nullptr);
}

void OhosInterface::adaptVideoSource(webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> videoSource, int width, int height, int fps) {
    // Per-frame adaptation (resolution/rotation) is handled inside
    // webrtc::OhosVideoTrackSource; nothing to do here.
}

std::unique_ptr<VideoCapturerInterface> OhosInterface::makeVideoCapturer(
    webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source,
    std::string deviceId,
    std::function<void(VideoState)> stateUpdated,
    std::function<void(PlatformCaptureInfo)> captureInfoUpdated,
    std::shared_ptr<PlatformContext> platformContext,
    std::pair<int, int> &outResolution) {
    outResolution = std::make_pair(1280, 720);
    return std::make_unique<VideoCameraCapturer>(std::move(source), deviceId, stateUpdated, captureInfoUpdated);
}

std::unique_ptr<PlatformInterface> CreatePlatformInterface() {
    return std::make_unique<OhosInterface>();
}

}  // namespace tgcalls
