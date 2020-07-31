#ifndef TGCALLS_VIDEO_CAPTURE_INTERFACE_IMPL_H
#define TGCALLS_VIDEO_CAPTURE_INTERFACE_IMPL_H

#include "VideoCaptureInterface.h"
#include <memory>
#include "ThreadLocalObject.h"
#include "api/media_stream_interface.h"
#include "platform/PlatformInterface.h"

namespace tgcalls {

class VideoCapturerInterface;

class VideoCaptureInterfaceObject {
public:
	VideoCaptureInterfaceObject();
	~VideoCaptureInterfaceObject();

	void switchCamera();
	void setIsVideoEnabled(bool isVideoEnabled);
    void setPreferredAspectRatio(float aspectRatio);
	void setVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink);
	void setIsActiveUpdated(std::function<void (bool)> isActiveUpdated);

public:
	rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> _videoSource;
    std::shared_ptr<VideoCapturer> _platformCapturer;

private:
	std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> _currentSink;
	std::function<void (bool)> _isActiveUpdated;
	bool _useFrontCamera;
	bool _isVideoEnabled;
};

class VideoCaptureInterfaceImpl : public VideoCaptureInterface {
public:
	VideoCaptureInterfaceImpl();
	virtual ~VideoCaptureInterfaceImpl();

	void switchCamera() override;
	void setIsVideoEnabled(bool isVideoEnabled) override;
    void setPreferredAspectRatio(float aspectRatio) override;
	void setVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) override;

	ThreadLocalObject<VideoCaptureInterfaceObject> *object();

private:
	ThreadLocalObject<VideoCaptureInterfaceObject> _impl;

};

} // namespace tgcalls

#endif
