#include "WindowsInterface.h"

namespace tgcalls {

std::unique_ptr<webrtc::VideoEncoderFactory> WindowsInterface::makeVideoEncoderFactory() {
	return nullptr;
}

std::unique_ptr<webrtc::VideoDecoderFactory> WindowsInterface::makeVideoDecoderFactory() {
	return nullptr;
}

rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> WindowsInterface::makeVideoSource(rtc::Thread *signalingThread, rtc::Thread *workerThread) {
	return nullptr;
}

std::unique_ptr<VideoCapturerInterface> WindowsInterface::makeVideoCapturer(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source, bool useFrontCamera, std::function<void(bool)> isActiveUpdated) {
	return nullptr;
}

std::unique_ptr<PlatformInterface> CreatePlatformInterface() {
	return std::make_unique<WindowsInterface>();
}

} // namespace tgcalls
