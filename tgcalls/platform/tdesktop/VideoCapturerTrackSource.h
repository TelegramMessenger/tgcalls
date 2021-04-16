#ifndef TGCALLS_VIDEO_CAPTURER_TRACK_SOURCE_H
#define TGCALLS_VIDEO_CAPTURER_TRACK_SOURCE_H

#include "pc/video_track_source.h"
#include "VideoCameraCapturer.h"

namespace tgcalls {

class VideoCameraCapturer;
class DesktopCapturer;

class VideoCapturerTrackSource : public webrtc::VideoTrackSource {
private:
	struct CreateTag {
	};

public:
	static rtc::scoped_refptr<VideoCapturerTrackSource> Create();

	VideoCapturerTrackSource(
		const CreateTag &,
		std::unique_ptr<VideoCameraCapturer> capturer);
	VideoCapturerTrackSource(
		const CreateTag &,
		std::unique_ptr<DesktopCapturer> capturer);

	VideoCameraCapturer *videoCapturer() const;
	DesktopCapturer *desktopCapturer() const;

private:
	rtc::VideoSourceInterface<webrtc::VideoFrame> *source() override;

	std::unique_ptr<VideoCameraCapturer> _videoCapturer;
	std::unique_ptr<DesktopCapturer> _desktopCapturer;

};

} // namespace tgcalls

#endif
