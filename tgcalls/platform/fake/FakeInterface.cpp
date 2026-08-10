#include "FakeInterface.h"

//#include "api/video_track_source_proxy.h"

namespace tgcalls {

std::unique_ptr<webrtc::VideoEncoderFactory> FakeInterface::makeVideoEncoderFactory(bool preferHardwareEncoding, bool isScreencast) {
  return nullptr;
}

std::unique_ptr<webrtc::VideoDecoderFactory> FakeInterface::makeVideoDecoderFactory() {
  return nullptr;
}

webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> FakeInterface::makeVideoSource(webrtc::Thread *signalingThread,
                                                                                     webrtc::Thread *workerThread) {
  return nullptr;
}

bool FakeInterface::supportsEncoding(const std::string &codecName) {
  return false;
  //return (codecName == webrtc::kH264CodecName) || (codecName == webrtc::kVp8CodecName);
}

void FakeInterface::adaptVideoSource(webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> videoSource, int width,
                                     int height, int fps) {
}

std::unique_ptr<VideoCapturerInterface> FakeInterface::makeVideoCapturer(
    webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source, std::string deviceId,
    std::function<void(VideoState)> stateUpdated, std::function<void(PlatformCaptureInfo)> captureInfoUpdated,
    std::shared_ptr<PlatformContext> platformContext, std::pair<int, int> &outResolution) {
  return nullptr;
  //return std::make_unique<VideoCapturerInterfaceImpl>(source, deviceId, stateUpdated, outResolution);
}

std::unique_ptr<PlatformInterface> CreatePlatformInterface() {
  return std::make_unique<FakeInterface>();
}

}  // namespace tgcalls
