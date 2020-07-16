#include "VideoCapturerInterfaceImpl.h"

#include "VideoCapturerTrackSource.h"
#include "VideoCameraCapturer.h"

#include "api/video_track_source_proxy.h"

namespace tgcalls {
namespace {

static VideoCameraCapturer *GetCapturer(
		const rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> nativeSource) {
	const auto proxy = static_cast<webrtc::VideoTrackSourceProxy*>(nativeSource.get());
	const auto internal = static_cast<VideoCapturerTrackSource*>(proxy->internal());
	return internal->capturer();
}

} // namespace

VideoCapturerInterfaceImpl::VideoCapturerInterfaceImpl(
	rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source,
	bool useFrontCamera,
	std::function<void(bool)> isActiveUpdated)
: _source(source)
, _isActiveUpdated(isActiveUpdated) {
}

VideoCapturerInterfaceImpl::~VideoCapturerInterfaceImpl() {
}

void VideoCapturerInterfaceImpl::setIsEnabled(bool isEnabled) {
	GetCapturer(_source)->setIsEnabled(isEnabled);
	if (_isActiveUpdated) {
		_isActiveUpdated(isEnabled);
	}
}

} // namespace tgcalls
