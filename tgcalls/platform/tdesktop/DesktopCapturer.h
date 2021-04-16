#ifndef TGCALLS_DESKTOP_CAPTURER_H
#define TGCALLS_DESKTOP_CAPTURER_H

#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_source_interface.h"
#include "media/base/video_adapter.h"
#include "modules/desktop_capture/desktop_capturer.h"

#include "VideoCaptureInterface.h"

#include <memory>
#include <vector>
#include <stddef.h>

namespace webrtc {
class DesktopFrame;
} // namespace webrtc

namespace rtc {
class Thread;
} // namespace rtc

namespace tgcalls {

class DesktopCapturer : public webrtc::DesktopCapturer::Callback {
public:
	DesktopCapturer(
		rtc::VideoSinkInterface<webrtc::VideoFrame> *sink);
	~DesktopCapturer();

	void setState(VideoState state);
	void setDeviceId(std::string deviceId);
	void setPreferredCaptureAspectRatio(float aspectRatio);

	std::pair<int, int> resolution() const;

	void OnCaptureResult(
		webrtc::DesktopCapturer::Result result,
		std::unique_ptr<webrtc::DesktopFrame> frame);

private:
	void create();
	void destroy();
	void captureAndSchedule();

	rtc::VideoSinkInterface<webrtc::VideoFrame> *_sink = nullptr;
	std::shared_ptr<webrtc::DesktopCapturer> _module;
	std::weak_ptr<webrtc::DesktopCapturer> _guard;
	rtc::Thread *_thread = nullptr;

	rtc::scoped_refptr<webrtc::I420Buffer> _i420buffer;

	VideoState _state = VideoState::Inactive;
	std::string _requestedDeviceId;
	std::pair<int, int> _dimensions;
	float _aspectRatio = 0.;

};

}  // namespace tgcalls

#endif
