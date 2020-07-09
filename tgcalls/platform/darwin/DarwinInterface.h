#ifndef DARWIN_INTERFACE_H
#define DARWIN_INTERFACE_H

#include "platform/PlatformInterface.h"

namespace tgcalls {

class DarwinInterface : public PlatformInterface {
public:
	void configurePlatformAudio() override;
	std::unique_ptr<webrtc::VideoEncoderFactory> makeVideoEncoderFactory() override;
	std::unique_ptr<webrtc::VideoDecoderFactory> makeVideoDecoderFactory() override;
	bool supportsH265Encoding() override;
	rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> makeVideoSource(rtc::Thread *signalingThread, rtc::Thread *workerThread) override;
	std::unique_ptr<VideoCapturerInterface> makeVideoCapturer(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source, bool useFrontCamera, std::function<void(bool)> isActiveUpdated) override;

};

} // namespace tgcalls

#endif
