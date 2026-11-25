#ifndef TGCALLS_VIDEO_CAPTURER_INTERFACE_H
#define TGCALLS_VIDEO_CAPTURER_INTERFACE_H

#include "Instance.h"

#include <memory>
#include <functional>

namespace webrtc {
template <typename VideoFrameT>
class VideoSinkInterface;
} // namespace webrtc

namespace webrtc {
class VideoFrame;
} // namespace webrtc

namespace tgcalls {

class VideoCapturerInterface {
public:
	virtual ~VideoCapturerInterface() = default;

	virtual void setState(VideoState state) = 0;
	virtual void setPreferredCaptureAspectRatio(float aspectRatio) = 0;
	virtual void setUncroppedOutput(std::shared_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> sink) = 0;
    virtual int getRotation() = 0;
    virtual void setOnFatalError(std::function<void()> error) {
      // TODO: make this function pure virtual when everybody implements it.
    }
    virtual void setOnPause(std::function<void(bool)> pause) {
      // TODO: make this function pure virtual when everybody implements it.
    }
    virtual void withNativeImplementation(std::function<void(void *)> completion) {
        completion(nullptr);
    }
    
};

} // namespace tgcalls

#endif
