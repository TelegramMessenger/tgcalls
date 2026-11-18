#ifndef TGCALLS_SCREEN_CAPTURE_KIT_CAPTURER_H
#define TGCALLS_SCREEN_CAPTURE_KIT_CAPTURER_H

#if defined(TGCALLS_USE_SCREENCAPTUREKIT) && !defined(WEBRTC_IOS)

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

#include <functional>
#include <memory>

#import "api/video/video_frame.h"
#import "api/video/video_sink_interface.h"
#import "base/RTCVideoFrame.h"
#import "tgcalls/desktop_capturer/DesktopCaptureSource.h"

API_AVAILABLE(macos(12.3))
@interface ScreenCaptureKitCapturer : NSObject

+ (BOOL)isEnabled;

- (instancetype)initWithSource:(tgcalls::DesktopCaptureSource)source data:(tgcalls::DesktopCaptureSourceData)data frameReceived:(void (^)(RTC_OBJC_TYPE(RTCVideoFrame) *frame))frameReceived;

- (void)start;
- (void)stop;
- (void)setOnFatalError:(std::function<void()>)error;
- (void)setOnPause:(std::function<void(bool)>)pause;
- (void)setSecondaryOutput:(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>)sink;

@end

#endif

#endif
