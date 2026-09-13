#include "tgcalls/platform/tdesktop/VideoCapturerTrackSource.h"

#include "api/video_track_source_constraints.h"

namespace tgcalls {

VideoCapturerTrackSource::VideoCapturerTrackSource()
: VideoTrackSource(/*remote=*/false)
, _broadcaster(std::make_shared<rtc::VideoBroadcaster>()) {
}

auto VideoCapturerTrackSource::sink()
-> std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> {
	return _broadcaster;
}

void VideoCapturerTrackSource::SetSourceConstraints(double maxFps) {
	_broadcaster->ProcessConstraints(
		webrtc::VideoTrackSourceConstraints{ 0., maxFps });
}

rtc::VideoSourceInterface<webrtc::VideoFrame> *VideoCapturerTrackSource::source() {
	return _broadcaster.get();
}

} // namespace tgcalls
