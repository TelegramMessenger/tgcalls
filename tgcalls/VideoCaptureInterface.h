#ifndef TGCALLS_VIDEO_CAPTURE_INTERFACE_H
#define TGCALLS_VIDEO_CAPTURE_INTERFACE_H

#include <memory>

#include "api/video/video_sink_interface.h"
#include "api/video/video_frame.h"

namespace tgcalls {

class VideoCaptureInterface {
protected:
	VideoCaptureInterface() = default;

public:
	static std::shared_ptr<VideoCaptureInterface> makeInstance();

	virtual ~VideoCaptureInterface();

	virtual void switchCamera() = 0;
	virtual void setIsVideoEnabled(bool isVideoEnabled) = 0;
	virtual void setVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) = 0;
};

} // namespace tgcalls

#endif
