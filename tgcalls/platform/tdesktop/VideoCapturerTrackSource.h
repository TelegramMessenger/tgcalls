#ifndef TGCALLS_VIDEO_CAPTURER_TRACK_SOURCE_H
#define TGCALLS_VIDEO_CAPTURER_TRACK_SOURCE_H

#include "pc/video_track_source.h"
#include "test/platform_video_capturer.h"

namespace tgcalls {

class VideoCapturerTrackSource : public webrtc::VideoTrackSource {
public:
	static rtc::scoped_refptr<VideoCapturerTrackSource> Create();

protected:
	explicit VideoCapturerTrackSource(
		std::unique_ptr<webrtc::test::TestVideoCapturer> capturer)
		: VideoTrackSource(/*remote=*/false), capturer_(std::move(capturer)) {}

private:
	rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
		return capturer_.get();
	}
	std::unique_ptr<webrtc::test::TestVideoCapturer> capturer_;

};

} // namespace tgcalls

#endif
