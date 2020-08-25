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
	VideoCaptureInterfaceObject(std::shared_ptr<PlatformContext> platformContext, bool screenCast);
	~VideoCaptureInterfaceObject();

	void switchCamera();
	void setState(VideoState state);
    void setPreferredAspectRatio(float aspectRatio);
	void setOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink);
	void setStateUpdated(std::function<void(VideoState)> stateUpdated);
    void enableScreenCast();
    void disableScreenCast();
public:
	rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> _videoSource;

private:
	std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> _currentUncroppedSink;
	std::shared_ptr<PlatformContext> _platformContext;
	std::unique_ptr<VideoCapturerInterface> _videoCapturer;
	std::function<void(VideoState)> _stateUpdated;
	bool _useFrontCamera = true;
    bool _enableScreenCast = false;
	VideoState _state = VideoState::Active;
};

class VideoCaptureInterfaceImpl : public VideoCaptureInterface {
public:
	VideoCaptureInterfaceImpl(std::shared_ptr<PlatformContext> platformContext, bool screenCast);
	virtual ~VideoCaptureInterfaceImpl();

	void switchCamera() override;
	void setState(VideoState state) override;
    void setPreferredAspectRatio(float aspectRatio) override;
	void setOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) override;
    void enableScreenCast() override;
    void disableScreenCast() override;
	ThreadLocalObject<VideoCaptureInterfaceObject> *object();

private:
	ThreadLocalObject<VideoCaptureInterfaceObject> _impl;

};

} // namespace tgcalls

#endif
