#include "tgcalls/platform/tdesktop/VideoCapturerInterfaceImpl.h"

#include "tgcalls/platform/tdesktop/VideoCapturerTrackSource.h"
#include "tgcalls/platform/tdesktop/VideoCameraCapturer.h"
#include "tgcalls/platform/tdesktop/DesktopCapturer.h"

#include "api/video_track_source_proxy.h"

namespace tgcalls {
namespace {

rtc::VideoSinkInterface<webrtc::VideoFrame> *GetSink(
		const rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> &nativeSource) {
	const auto proxy = static_cast<webrtc::VideoTrackSourceProxy*>(
		nativeSource.get());
	const auto internal = static_cast<VideoCapturerTrackSource*>(
		proxy->internal());
	return internal->sink();
}

} // namespace

VideoCapturerInterfaceImpl::VideoCapturerInterfaceImpl(
	rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source,
	std::string deviceId,
	std::function<void(VideoState)> stateUpdated,
	std::pair<int, int> &outResolution)
: _source(source)
, _sink(GetSink(source))
, _stateUpdated(stateUpdated) {
	if (deviceId.find("desktop_capturer_") == 0) {
		_desktopCapturer = std::make_unique<DesktopCapturer>(_sink);
		_desktopCapturer->setState(VideoState::Active);
		outResolution = _desktopCapturer->resolution();
	} else {
		_cameraCapturer = std::make_unique<VideoCameraCapturer>(_sink);
		_cameraCapturer->setDeviceId(deviceId);
		_cameraCapturer->setState(VideoState::Active);
		outResolution = { 1280, 960 };
	}
}

VideoCapturerInterfaceImpl::~VideoCapturerInterfaceImpl() {
}

void VideoCapturerInterfaceImpl::setState(VideoState state) {
	if (_desktopCapturer) {
		_desktopCapturer->setState(state);
	} else if (_cameraCapturer) {
		_cameraCapturer->setState(state);
	}
	if (_stateUpdated) {
		_stateUpdated(state);
	}
}

void VideoCapturerInterfaceImpl::setPreferredCaptureAspectRatio(
		float aspectRatio) {
	if (_cameraCapturer) {
		_cameraCapturer->setPreferredCaptureAspectRatio(aspectRatio);
	}
}

void VideoCapturerInterfaceImpl::setUncroppedOutput(
		std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	if (_uncroppedSink != nullptr) {
		_source->RemoveSink(_uncroppedSink.get());
	}
	_uncroppedSink = sink;
	if (_uncroppedSink != nullptr) {
		_source->AddOrUpdateSink(
			_uncroppedSink.get(),
			rtc::VideoSinkWants());
	}
}

} // namespace tgcalls
