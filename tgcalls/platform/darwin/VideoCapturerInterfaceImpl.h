#ifndef VIDEO_CAPTURER_INTERFACE_IMPL_H
#define VIDEO_CAPTURER_INTERFACE_IMPL_H

#include "VideoCapturerInterface.h"

#include "sdk/objc/native/src/objc_video_track_source.h"
#include "api/video_track_source_proxy.h"

@interface VideoCapturerInterfaceImplHolder : NSObject

@property (nonatomic) void *reference;

@end

namespace tgcalls {

class VideoCapturerInterfaceImpl : public VideoCapturerInterface {
public:
	VideoCapturerInterfaceImpl(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source, bool useFrontCamera, std::function<void(bool)> isActiveUpdated);
	~VideoCapturerInterfaceImpl() override;

	void setIsEnabled(bool isEnabled) override;

private:
	rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> _source;
	VideoCapturerInterfaceImplHolder *_implReference;
};

} // namespace tgcalls

#endif
