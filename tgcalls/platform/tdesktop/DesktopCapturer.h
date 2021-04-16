#ifndef TGCALLS_DESKTOP_CAPTURER_H
#define TGCALLS_DESKTOP_CAPTURER_H

#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_source_interface.h"
#include "media/base/video_adapter.h"
#include "media/base/video_broadcaster.h"
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

class DesktopCapturer :
	public rtc::VideoSourceInterface<webrtc::VideoFrame>,
	public webrtc::DesktopCapturer::Callback {
public:
	DesktopCapturer();
	~DesktopCapturer();

	void setState(VideoState state);
	void setDeviceId(std::string deviceId);
	void setPreferredCaptureAspectRatio(float aspectRatio);

	std::pair<int, int> resolution() const;

	void AddOrUpdateSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
		const rtc::VideoSinkWants& wants) override;
	void RemoveSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override;

	void OnCaptureResult(
		webrtc::DesktopCapturer::Result result,
		std::unique_ptr<webrtc::DesktopFrame> frame);

private:
	void create();
	void destroy();
	void captureAndSchedule();
	void updateVideoAdapter();

	rtc::VideoBroadcaster _broadcaster;
	std::shared_ptr<webrtc::DesktopCapturer> _module;
	std::weak_ptr<webrtc::DesktopCapturer> _guard;
	rtc::Thread *_thread = nullptr;

	rtc::scoped_refptr<webrtc::I420Buffer> _i420buffer;
	int64_t _nextTimestamp = 0;

	VideoState _state = VideoState::Inactive;
	std::string _requestedDeviceId;
	std::pair<int, int> _dimensions;
	float _aspectRatio = 0.;

};

}  // namespace tgcalls

#endif
