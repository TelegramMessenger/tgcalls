#include "VideoCapturerTrackSource.h"

#include "modules/video_capture/video_capture_factory.h"

namespace tgcalls {

rtc::scoped_refptr<VideoCapturerTrackSource> VideoCapturerTrackSource::Create() {
	const size_t kWidth = 640;
	const size_t kHeight = 480;
	const size_t kFps = 30;

#ifdef WEBRTC_MAC
	int num_devices = 1;
#else // WEBRTC_MAC
	std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
		webrtc::VideoCaptureFactory::CreateDeviceInfo());
	if (!info) {
		return nullptr;
	}
	int num_devices = info->NumberOfDevices();
#endif // WEBRTC_MAC

	for (int i = 0; i < num_devices; ++i) {
		if (auto capturer = webrtc::test::CreateVideoCapturer(kWidth, kHeight, kFps, i)) {
			return new rtc::RefCountedObject<VideoCapturerTrackSource>(
				std::move(capturer));
		}
	}
	return nullptr;
}

} // namespace tgcalls
