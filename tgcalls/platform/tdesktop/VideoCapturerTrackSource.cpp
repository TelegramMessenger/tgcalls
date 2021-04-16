#include "tgcalls/platform/tdesktop/VideoCapturerTrackSource.h"

namespace tgcalls {

VideoCapturerTrackSource::VideoCapturerTrackSource()
: VideoTrackSource(/*remote=*/false) {
}

rtc::VideoSinkInterface<webrtc::VideoFrame> *VideoCapturerTrackSource::sink() {
	return &_broadcaster;
}

rtc::VideoSourceInterface<webrtc::VideoFrame> *VideoCapturerTrackSource::source() {
	return &_broadcaster;
}

} // namespace tgcalls
