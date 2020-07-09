#ifndef TGCALLS_PLATFORM_INTERFACE_H
#define TGCALLS_PLATFORM_INTERFACE_H

#include "rtc_base/thread.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/media_stream_interface.h"

namespace tgcalls {

class VideoCapturerInterface;

class PlatformInterface {
public:
	static PlatformInterface *SharedInstance();
	virtual ~PlatformInterface() = default;

	virtual void configurePlatformAudio() {
	}
	virtual std::unique_ptr<webrtc::VideoEncoderFactory> makeVideoEncoderFactory() = 0;
	virtual std::unique_ptr<webrtc::VideoDecoderFactory> makeVideoDecoderFactory() = 0;
	virtual bool supportsH265Encoding() {
		return false;
	}
	virtual rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> makeVideoSource(rtc::Thread *signalingThread, rtc::Thread *workerThread) = 0;
	virtual std::unique_ptr<VideoCapturerInterface> makeVideoCapturer(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source, bool useFrontCamera, std::function<void(bool)> isActiveUpdated) = 0;

};

std::unique_ptr<PlatformInterface> CreatePlatformInterface();

inline PlatformInterface *PlatformInterface::SharedInstance() {
	static const auto result = CreatePlatformInterface();
	return result.get();
}

} // namespace tgcalls

#endif
