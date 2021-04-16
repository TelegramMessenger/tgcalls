#include "tgcalls/platform/tdesktop/VideoCapturerTrackSource.h"

#include "tgcalls/platform/tdesktop/VideoCameraCapturer.h"
#include "tgcalls/platform/tdesktop/DesktopCapturer.h"

#include "modules/video_capture/video_capture_factory.h"

namespace tgcalls {

rtc::scoped_refptr<VideoCapturerTrackSource> VideoCapturerTrackSource::Create() {
	//return new rtc::RefCountedObject<VideoCapturerTrackSource>(
	//	CreateTag{},
	//	std::make_unique<VideoCameraCapturer>());
	return new rtc::RefCountedObject<VideoCapturerTrackSource>(
		CreateTag{},
		std::make_unique<DesktopCapturer>());
}

VideoCapturerTrackSource::VideoCapturerTrackSource(
	const CreateTag &,
	std::unique_ptr<VideoCameraCapturer> capturer) :
VideoTrackSource(/*remote=*/false),
_videoCapturer(std::move(capturer)) {
}

VideoCapturerTrackSource::VideoCapturerTrackSource(
	const CreateTag &,
	std::unique_ptr<DesktopCapturer> capturer) :
VideoTrackSource(/*remote=*/false),
_desktopCapturer(std::move(capturer)) {
}

VideoCameraCapturer *VideoCapturerTrackSource::videoCapturer() const {
	return _videoCapturer.get();
}

DesktopCapturer *VideoCapturerTrackSource::desktopCapturer() const {
	return _desktopCapturer.get();
}

rtc::VideoSourceInterface<webrtc::VideoFrame>* VideoCapturerTrackSource::source() {
	return _desktopCapturer.get();
}

} // namespace tgcalls
