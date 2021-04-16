#include "tgcalls/platform/tdesktop/VideoCapturerInterfaceImpl.h"

#include "tgcalls/platform/tdesktop/VideoCapturerTrackSource.h"
#include "tgcalls/platform/tdesktop/VideoCameraCapturer.h"
#include "tgcalls/platform/tdesktop/DesktopCapturer.h"

#include "api/video_track_source_proxy.h"

namespace tgcalls {
namespace {

VideoCameraCapturer *GetVideoCapturer(
	const rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> nativeSource) {
	const auto proxy = static_cast<webrtc::VideoTrackSourceProxy*>(
		nativeSource.get());
	const auto internal = static_cast<VideoCapturerTrackSource*>(
		proxy->internal());
	return internal->videoCapturer();
}

DesktopCapturer *GetDesktopCapturer(
	const rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> nativeSource) {
	const auto proxy = static_cast<webrtc::VideoTrackSourceProxy*>(
		nativeSource.get());
	const auto internal = static_cast<VideoCapturerTrackSource*>(
		proxy->internal());
	return internal->desktopCapturer();
}

} // namespace

VideoCapturerInterfaceImpl::VideoCapturerInterfaceImpl(
	rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source,
	std::string deviceId,
	std::function<void(VideoState)> stateUpdated,
	std::pair<int, int> &outResolution)
: _source(source)
, _stateUpdated(stateUpdated) {
	if (const auto video = GetVideoCapturer(_source)) {
		video->setDeviceId(deviceId);
		video->setState(VideoState::Active);
		outResolution = video->resolution();
	} else if (const auto desktop = GetDesktopCapturer(_source)) {
		desktop->setState(VideoState::Active);
		outResolution = { 1280, 960 };
	}
}

VideoCapturerInterfaceImpl::~VideoCapturerInterfaceImpl() {
}

void VideoCapturerInterfaceImpl::setState(VideoState state) {
	if (const auto video = GetVideoCapturer(_source)) {
		video->setState(state);
	} else if (const auto desktop = GetDesktopCapturer(_source)) {
		desktop->setState(state);
	}
	if (_stateUpdated) {
		_stateUpdated(state);
	}
}

void VideoCapturerInterfaceImpl::setPreferredCaptureAspectRatio(
		float aspectRatio) {
	if (const auto video = GetVideoCapturer(_source)) {
		video->setPreferredCaptureAspectRatio(aspectRatio);
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
