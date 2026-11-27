#ifndef TGCALLS_ANDROID_INTERFACE_H
#define TGCALLS_ANDROID_INTERFACE_H

#include "sdk/android/native_api/video/video_source.h"
#include "platform/PlatformInterface.h"
#include "VideoCapturerInterface.h"

namespace tgcalls {

class AndroidInterface : public PlatformInterface {
public:
    void configurePlatformAudio(int numChannels = 1) override;
	std::unique_ptr<webrtc::VideoEncoderFactory> makeVideoEncoderFactory(bool preferHardwareEncoding = false, bool isScreencast = false) override;
	std::unique_ptr<webrtc::VideoDecoderFactory> makeVideoDecoderFactory() override;
	bool supportsEncoding(const std::string &codecName) override;
	webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> makeVideoSource(webrtc::Thread *signalingThread, webrtc::Thread *workerThread) override;
    void adaptVideoSource(webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> videoSource, int width, int height, int fps) override;
	std::unique_ptr<VideoCapturerInterface> makeVideoCapturer(webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source, std::string deviceId, std::function<void(VideoState)> stateUpdated, std::function<void(PlatformCaptureInfo)> captureInfoUpdated, std::shared_ptr<PlatformContext> platformContext, std::pair<int, int> &outResolution) override;
    std::unique_ptr<webrtc::NetworkMonitorFactory> createNetworkMonitorFactory() override;
private:
	std::unique_ptr<webrtc::VideoEncoderFactory> hardwareVideoEncoderFactory;
	std::unique_ptr<webrtc::VideoEncoderFactory> softwareVideoEncoderFactory;

};

} // namespace tgcalls

#endif
