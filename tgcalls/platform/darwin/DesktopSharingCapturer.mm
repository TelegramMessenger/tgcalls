#import "DesktopSharingCapturer.h"

#include "modules/desktop_capture/mac/screen_capturer_mac.h"
#include "modules/desktop_capture/desktop_and_cursor_composer.h"
#include "modules/desktop_capture/desktop_capturer_differ_wrapper.h"
#include "third_party/libyuv/include/libyuv.h"
#include "api/video/i420_buffer.h"


#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv.h"
#import "helpers/RTCDispatcher+Private.h"
#import <QuartzCore/QuartzCore.h>
#import "DesktopCaptureSourceHelper.h"
#import "DesktopCaptureSource.h"
#import "DesktopCaptureSourceManager.h"

static RTCVideoFrame *customToObjCVideoFrame(const webrtc::VideoFrame &frame, RTCVideoRotation &rotation) {
    rotation = RTCVideoRotation(frame.rotation());
    RTCVideoFrame *videoFrame =
    [[RTCVideoFrame alloc] initWithBuffer:webrtc::ToObjCVideoFrameBuffer(frame.video_frame_buffer())
                                 rotation:rotation
                              timeStampNs:frame.timestamp_us() * rtc::kNumNanosecsPerMicrosec];
    videoFrame.timeStamp = frame.timestamp();

    return videoFrame;
}

static webrtc::ObjCVideoTrackSource *getObjCVideoSource(const rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> nativeSource) {
    webrtc::VideoTrackSourceProxy *proxy_source =
    static_cast<webrtc::VideoTrackSourceProxy *>(nativeSource.get());
    return static_cast<webrtc::ObjCVideoTrackSource *>(proxy_source->internal());
}



class RendererAdapterImpl : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    RendererAdapterImpl(void (^frameReceived)(CGSize, RTCVideoFrame *, RTCVideoRotation)) {
        _frameReceived = [frameReceived copy];
    }

    void OnFrame(const webrtc::VideoFrame& nativeVideoFrame) override {
        RTCVideoRotation rotation = RTCVideoRotation_0;
        RTCVideoFrame* videoFrame = customToObjCVideoFrame(nativeVideoFrame, rotation);

        CGSize currentSize = (videoFrame.rotation % 180 == 0) ? CGSizeMake(videoFrame.width, videoFrame.height) : CGSizeMake(videoFrame.height, videoFrame.width);

        if (_frameReceived) {
            _frameReceived(currentSize, videoFrame, rotation);
        }
    }

private:
    void (^_frameReceived)(CGSize, RTCVideoFrame *, RTCVideoRotation);
};



@implementation DesktopSharingCapturer {
    absl::optional<DesktopCaptureSourceHelper> renderer;
    std::shared_ptr<RendererAdapterImpl> _sink;
    BOOL _isPaused;

}
- (instancetype)initWithSource:(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface>)trackSource captureSource:(DesktopCaptureSource)captureSource {
    self = [super init];
    if (self != nil) {
        _sink.reset(new RendererAdapterImpl(^(CGSize size, RTCVideoFrame *videoFrame, RTCVideoRotation rotation) {
            getObjCVideoSource(trackSource)->OnCapturedFrame(videoFrame);
        }));

        const auto data = DesktopCaptureSourceData{
	        /*.aspectSize = */{ 1280, 720 },
	        /*.fps = */24.,
	        /*.captureMouse = */true,
        };
        renderer.emplace(captureSource, data);
        renderer->setOutput(_sink);
    }
    return self;
}

-(void)start {
    renderer->start();
}
-(void)stop {
    renderer->stop();
}

- (void)setIsEnabled:(bool)isEnabled {
    BOOL updated = _isPaused != !isEnabled;
    _isPaused = !isEnabled;
    if (updated) {
        if (isEnabled) {
            renderer->start();
        } else {
            renderer->stop();
        }
    }
}


- (void)setPreferredCaptureAspectRatio:(float)aspectRatio {

}


- (void)setUncroppedSink:(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame> >)sink {
    renderer->setSecondaryOutput(sink);
}


@end








