#include "tgcalls/platform/tdesktop/VideoCapturerTrackSource.h"

namespace tgcalls {

VideoCapturerTrackSource::VideoCapturerTrackSource()
: VideoTrackSource(/*remote=*/false)
, _broadcaster(std::make_shared<webrtc::VideoBroadcaster>()) {
}

auto VideoCapturerTrackSource::sink()
-> std::shared_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> {
	return _broadcaster;
}

webrtc::VideoSourceInterface<webrtc::VideoFrame> *VideoCapturerTrackSource::source() {
	return _broadcaster.get();
}

} // namespace tgcalls
