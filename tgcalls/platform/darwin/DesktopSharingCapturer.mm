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

#include "tgcalls/desktop_capturer/DesktopCaptureSource.h"
#include "tgcalls/desktop_capturer/DesktopCaptureSourceHelper.h"
#include "tgcalls/desktop_capturer/DesktopCaptureSourceManager.h"
#include "DarwinVideoSource.h"

#import "helpers/RTCDispatcher+Private.h"
#ifdef TGCALLS_USE_SCREENCAPTUREKIT
#import "tgcalls/platform/darwin/ScreenCaptureKitCapturer.h"
#endif
#import <QuartzCore/QuartzCore.h>

static RTCVideoFrame *customToObjCVideoFrame(const webrtc::VideoFrame &frame, RTCVideoRotation &rotation) {
    rotation = RTCVideoRotation(frame.rotation());
    RTCVideoFrame *videoFrame =
    [[RTCVideoFrame alloc] initWithBuffer:webrtc::ToObjCVideoFrameBuffer(frame.video_frame_buffer())
                                 rotation:rotation
                              timeStampNs:frame.timestamp_us() * webrtc::kNumNanosecsPerMicrosec];
    videoFrame.timeStamp = frame.rtp_timestamp();

    return videoFrame;
}

static tgcalls::DarwinVideoTrackSource *getObjCVideoSource(const webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> nativeSource) {
    webrtc::VideoTrackSourceProxy *proxy_source =
    static_cast<webrtc::VideoTrackSourceProxy *>(nativeSource.get());
    return static_cast<tgcalls::DarwinVideoTrackSource *>(proxy_source->internal());
}

class RendererAdapterImpl : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
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
    std::optional<tgcalls::DesktopCaptureSourceHelper> renderer;
    std::shared_ptr<RendererAdapterImpl> _sink;
    BOOL _isPaused;
#ifdef TGCALLS_USE_SCREENCAPTUREKIT
    id _screenCaptureKitCapturer;
#endif
}
- (instancetype)initWithSource:(webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>)trackSource captureSource:(tgcalls::DesktopCaptureSource)captureSource {
    self = [super init];
    if (self != nil) {
        _sink.reset(new RendererAdapterImpl(^(CGSize size, RTCVideoFrame *videoFrame, RTCVideoRotation rotation) {
            getObjCVideoSource(trackSource)->OnCapturedFrame(videoFrame);
        }));

        tgcalls::DesktopCaptureSourceData data{
	        /*.aspectSize = */{ 1920, 1080 },
	        /*.fps = */25,
	        /*.captureMouse = */true,
        };
#ifdef TGCALLS_USE_SCREENCAPTUREKIT
        if (@available(macOS 12.3, *)) {
            if ([ScreenCaptureKitCapturer isEnabled]) {
                _screenCaptureKitCapturer = [[ScreenCaptureKitCapturer alloc] initWithSource:captureSource data:data frameReceived:^(RTC_OBJC_TYPE(RTCVideoFrame) *frame) {
                    getObjCVideoSource(trackSource)->OnCapturedFrame(frame);
                }];
            }
        }
        if (_screenCaptureKitCapturer != nil) {
            return self;
        }
#endif
        renderer.emplace(captureSource, data);
        renderer->setOutput(_sink);
    }
    return self;
}

-(void)setOnFatalError:(std::function<void ()>)error {
#ifdef TGCALLS_USE_SCREENCAPTUREKIT
    if (@available(macOS 12.3, *)) {
        if (_screenCaptureKitCapturer != nil) {
            [(ScreenCaptureKitCapturer *)_screenCaptureKitCapturer setOnFatalError:error];
            return;
        }
    }
#endif
    renderer->setOnFatalError(error);
}
-(void)setOnPause:(std::function<void (bool)>)pause {
#ifdef TGCALLS_USE_SCREENCAPTUREKIT
    if (@available(macOS 12.3, *)) {
        if (_screenCaptureKitCapturer != nil) {
            [(ScreenCaptureKitCapturer *)_screenCaptureKitCapturer setOnPause:pause];
            return;
        }
    }
#endif
    renderer->setOnPause(pause);
}

-(void)start {
#ifdef TGCALLS_USE_SCREENCAPTUREKIT
    if (@available(macOS 12.3, *)) {
        if (_screenCaptureKitCapturer != nil) {
            [(ScreenCaptureKitCapturer *)_screenCaptureKitCapturer start];
            return;
        }
    }
#endif
    renderer->start();
}

-(void)stop {
#ifdef TGCALLS_USE_SCREENCAPTUREKIT
    if (@available(macOS 12.3, *)) {
        if (_screenCaptureKitCapturer != nil) {
            [(ScreenCaptureKitCapturer *)_screenCaptureKitCapturer stop];
            return;
        }
    }
#endif
    renderer->stop();
}

- (void)setIsEnabled:(bool)isEnabled {
    BOOL updated = _isPaused != !isEnabled;
    _isPaused = !isEnabled;
    if (updated) {
        if (isEnabled) {
            [self start];
        } else {
            [self stop];
        }
    }
}

- (void)setPreferredCaptureAspectRatio:(float)aspectRatio {
}

- (void)setUncroppedSink:(std::shared_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame> >)sink {
#ifdef TGCALLS_USE_SCREENCAPTUREKIT
    if (@available(macOS 12.3, *)) {
        if (_screenCaptureKitCapturer != nil) {
            [(ScreenCaptureKitCapturer *)_screenCaptureKitCapturer setSecondaryOutput:sink];
            return;
        }
    }
#endif
    renderer->setSecondaryOutput(sink);
}

@end
