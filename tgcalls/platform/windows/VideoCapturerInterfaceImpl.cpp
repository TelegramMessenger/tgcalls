#include "VideoCapturerInterfaceImpl.h"

namespace tgcalls {

VideoCapturerInterfaceImpl::VideoCapturerInterfaceImpl(
	rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source,
	bool useFrontCamera,
	std::function<void(bool)> isActiveUpdated)
: _source(source) {
}

VideoCapturerInterfaceImpl::~VideoCapturerInterfaceImpl() {
}

void VideoCapturerInterfaceImpl::setIsEnabled(bool isEnabled) {
	// #TODO
}

} // namespace tgcalls
