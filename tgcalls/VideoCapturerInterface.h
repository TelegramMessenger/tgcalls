#ifndef TGCALLS_VIDEO_CAPTURER_INTERFACE_H
#define TGCALLS_VIDEO_CAPTURER_INTERFACE_H

#include <memory>

namespace rtc {
template <typename VideoFrameT>
class VideoSinkInterface;
} // namespace rtc

namespace webrtc {
class VideoFrame;
} // namespace webrtc

namespace tgcalls {

class VideoCapturerInterface {
public:
	virtual ~VideoCapturerInterface() = default;

	virtual void setIsEnabled(bool isEnabled) = 0;
	virtual void setPreferredCaptureAspectRatio(float aspectRatio) = 0;
	virtual void setUncroppedVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) = 0;
};

} // namespace tgcalls

#endif
