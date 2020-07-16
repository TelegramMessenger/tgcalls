#ifndef TGCALLS_VIDEO_CAPTURER_INTERFACE_IMPL_H
#define TGCALLS_VIDEO_CAPTURER_INTERFACE_IMPL_H

#include "VideoCapturerInterface.h"

#include "api/media_stream_interface.h"

namespace tgcalls {

class VideoCapturerInterfaceImpl : public VideoCapturerInterface {
public:
	VideoCapturerInterfaceImpl(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source, bool useFrontCamera, std::function<void(bool)> isActiveUpdated);
	~VideoCapturerInterfaceImpl() override;

	void setIsEnabled(bool isEnabled) override;

private:
	rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> _source;
	std::function<void(bool)> _isActiveUpdated;

};

} // namespace tgcalls

#endif
