#import "tgcalls/platform/darwin/ScreenCaptureKitCapturer.h"

#if defined(TGCALLS_USE_SCREENCAPTUREKIT) && !defined(WEBRTC_IOS)

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <optional>

#import "components/video_frame_buffer/RTCCVPixelBuffer.h"
#include "sdk/objc/native/src/objc_frame_buffer.h"
#include "rtc_base/time_utils.h"

namespace {

NSString *const kDisableScreenCaptureKitKey = @"TGCallsDisableScreenCaptureKit";

CGSize outputSizeForContent(CGSize contentPixelSize, tgcalls::DesktopSize aspectSize) {
    CGFloat maxLongSide = MAX(aspectSize.width, aspectSize.height);
    CGFloat maxShortSide = MIN(aspectSize.width, aspectSize.height);
    CGFloat longSide = MAX(contentPixelSize.width, contentPixelSize.height);
    CGFloat shortSide = MIN(contentPixelSize.width, contentPixelSize.height);
    CGFloat scale = 1.0;
    if (maxLongSide > 0.0 && maxShortSide > 0.0 && longSide > 0.0 && shortSide > 0.0) {
        scale = MIN(1.0, MIN(maxLongSide / longSide, maxShortSide / shortSide));
    }
    int width = (int)floor(contentPixelSize.width * scale);
    int height = (int)floor(contentPixelSize.height * scale);
    width -= width % 4;
    height -= height % 4;
    return CGSizeMake(MAX(width, 4), MAX(height, 4));
}

struct DisplayInfo {
    CGFloat scale = 1.0;
    CGSize pixelSize = CGSizeZero;
    CFStringRef colorSpaceName = nullptr;
};

DisplayInfo displayInfo(CGDirectDisplayID displayId) {
    DisplayInfo info;
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayId);
    if (mode != nullptr) {
        size_t pointWidth = CGDisplayModeGetWidth(mode);
        size_t pixelWidth = CGDisplayModeGetPixelWidth(mode);
        info.pixelSize = CGSizeMake(pixelWidth, CGDisplayModeGetPixelHeight(mode));
        if (pointWidth != 0) {
            info.scale = (CGFloat)pixelWidth / (CGFloat)pointWidth;
        }
        CGDisplayModeRelease(mode);
    } else {
        info.pixelSize = CGSizeMake(CGDisplayPixelsWide(displayId), CGDisplayPixelsHigh(displayId));
    }
    CGColorSpaceRef colorSpace = CGDisplayCopyColorSpace(displayId);
    if (colorSpace != nullptr) {
        info.colorSpaceName = CGColorSpaceCopyName(colorSpace);
        CGColorSpaceRelease(colorSpace);
    }
    return info;
}

CGDirectDisplayID displayContainingRect(CGRect rect) {
    CGDirectDisplayID displayId = CGMainDisplayID();
    uint32_t count = 0;
    CGPoint center = CGPointMake(CGRectGetMidX(rect), CGRectGetMidY(rect));
    CGDirectDisplayID found = 0;
    if (CGGetDisplaysWithPoint(center, 1, &found, &count) == kCGErrorSuccess && count > 0) {
        displayId = found;
    }
    return displayId;
}

}

API_AVAILABLE(macos(12.3))
@interface ScreenCaptureKitCapturer () <SCStreamOutput, SCStreamDelegate>
@end

@implementation ScreenCaptureKitCapturer {
    std::optional<tgcalls::DesktopCaptureSource> _source;
    tgcalls::DesktopCaptureSourceData _data;
    void (^_frameReceived)(RTC_OBJC_TYPE(RTCVideoFrame) *);
    dispatch_queue_t _queue;
    SCStream *_stream;
    uint64_t _generation;
    BOOL _isRunning;
    BOOL _isPaused;
    BOOL _hasFatalError;
    BOOL _isReconfiguring;
    CGSize _configuredSize;
    NSString *_colorSpaceName;
    std::function<void()> _onFatalError;
    std::function<void(bool)> _onPause;
    std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> _secondarySink;
    RTC_OBJC_TYPE(RTCCVPixelBuffer) *_lastBuffer;
    int64_t _lastDeliveryNs;
    dispatch_source_t _repeatTimer;
}

+ (BOOL)isEnabled {
    return ![[NSUserDefaults standardUserDefaults] boolForKey:kDisableScreenCaptureKitKey];
}

- (instancetype)initWithSource:(tgcalls::DesktopCaptureSource)source data:(tgcalls::DesktopCaptureSourceData)data frameReceived:(void (^)(RTC_OBJC_TYPE(RTCVideoFrame) *))frameReceived {
    self = [super init];
    if (self != nil) {
        _source = source;
        _data = data;
        _frameReceived = [frameReceived copy];
        _queue = dispatch_queue_create("org.telegram.tgcalls.ScreenCaptureKitCapturer", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    }
    return self;
}

- (void)dealloc {
    if (_repeatTimer != nil) {
        dispatch_source_cancel(_repeatTimer);
    }
}

- (void)start {
    dispatch_async(_queue, ^{
        if (self->_isRunning || self->_hasFatalError) {
            return;
        }
        self->_isRunning = YES;
        uint64_t generation = ++self->_generation;
        __weak ScreenCaptureKitCapturer *weakSelf = self;
        [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:NO completionHandler:^(SCShareableContent *content, NSError *error) {
            ScreenCaptureKitCapturer *strongSelf = weakSelf;
            if (strongSelf == nil) {
                return;
            }
            dispatch_async(strongSelf->_queue, ^{
                [strongSelf startWithContent:content error:error generation:generation];
            });
        }];
    });
}

- (void)stop {
    dispatch_async(_queue, ^{
        [self stopCapture];
    });
}

- (void)stopCapture {
    _isRunning = NO;
    _generation += 1;
    if (_repeatTimer != nil) {
        dispatch_source_cancel(_repeatTimer);
        _repeatTimer = nil;
    }
    SCStream *stream = _stream;
    _stream = nil;
    _lastBuffer = nil;
    _isReconfiguring = NO;
    if (stream != nil) {
        [stream stopCaptureWithCompletionHandler:^(NSError *error) {
        }];
    }
}

- (void)setOnFatalError:(std::function<void()>)error {
    dispatch_async(_queue, ^{
        self->_onFatalError = error;
        if (self->_hasFatalError && error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                error();
            });
        }
    });
}

- (void)setOnPause:(std::function<void(bool)>)pause {
    dispatch_async(_queue, ^{
        self->_onPause = pause;
        if (self->_isPaused && pause) {
            dispatch_async(dispatch_get_main_queue(), ^{
                pause(true);
            });
        }
    });
}

- (void)setSecondaryOutput:(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>)sink {
    dispatch_async(_queue, ^{
        self->_secondarySink = sink;
    });
}

- (void)startWithContent:(SCShareableContent *)content error:(NSError *)error generation:(uint64_t)generation {
    if (!_isRunning || generation != _generation) {
        return;
    }
    if (content == nil) {
        [self reportFatalError];
        return;
    }
    SCContentFilter *filter = nil;
    DisplayInfo info;
    CGSize contentPixelSize = CGSizeZero;
    if (_source->isWindow()) {
        SCWindow *window = nil;
        for (SCWindow *candidate in content.windows) {
            if ((long long)candidate.windowID == _source->uniqueId()) {
                window = candidate;
                break;
            }
        }
        if (window == nil) {
            [self reportFatalError];
            return;
        }
        filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:window];
        info = displayInfo(displayContainingRect(window.frame));
        contentPixelSize = CGSizeMake(window.frame.size.width * info.scale, window.frame.size.height * info.scale);
    } else {
        SCDisplay *display = nil;
        for (SCDisplay *candidate in content.displays) {
            if ((long long)candidate.displayID == _source->uniqueId()) {
                display = candidate;
                break;
            }
        }
        if (display == nil) {
            [self reportFatalError];
            return;
        }
        filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
        info = displayInfo(display.displayID);
        contentPixelSize = info.pixelSize;
    }

    SCStreamConfiguration *configuration = [[SCStreamConfiguration alloc] init];
    _configuredSize = outputSizeForContent(contentPixelSize, _data.aspectSize);
    configuration.width = (size_t)_configuredSize.width;
    configuration.height = (size_t)_configuredSize.height;
    configuration.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    configuration.colorMatrix = kCGDisplayStreamYCbCrMatrix_ITU_R_601_4;
    _colorSpaceName = info.colorSpaceName != nullptr ? (__bridge_transfer NSString *)info.colorSpaceName : nil;
    if (_colorSpaceName != nil) {
        configuration.colorSpaceName = (__bridge CFStringRef)_colorSpaceName;
    }
    configuration.minimumFrameInterval = CMTimeMake(1, (int32_t)MAX(1.0, round(_data.fps)));
    configuration.showsCursor = _data.captureMouse;
    configuration.queueDepth = 5;

    SCStream *stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:self];
    NSError *addError = nil;
    if (![stream addStreamOutput:self type:SCStreamOutputTypeScreen sampleHandlerQueue:_queue error:&addError]) {
        [self reportFatalError];
        return;
    }
    _stream = stream;
    __weak ScreenCaptureKitCapturer *weakSelf = self;
    [stream startCaptureWithCompletionHandler:^(NSError *startError) {
        if (startError == nil) {
            return;
        }
        ScreenCaptureKitCapturer *strongSelf = weakSelf;
        if (strongSelf == nil) {
            return;
        }
        dispatch_async(strongSelf->_queue, ^{
            if (strongSelf->_stream == stream) {
                [strongSelf reportFatalError];
            }
        });
    }];
    [self startRepeatTimer];
}

- (void)startRepeatTimer {
    if (_repeatTimer != nil) {
        dispatch_source_cancel(_repeatTimer);
    }
    double fps = MAX(1.0, _data.fps);
    uint64_t interval = (uint64_t)(NSEC_PER_SEC / fps);
    _repeatTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _queue);
    dispatch_source_set_timer(_repeatTimer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)interval), interval / 2, interval / 20);
    __weak ScreenCaptureKitCapturer *weakSelf = self;
    dispatch_source_set_event_handler(_repeatTimer, ^{
        [weakSelf repeatLastFrameIfNeeded:interval];
    });
    dispatch_resume(_repeatTimer);
}

- (void)repeatLastFrameIfNeeded:(uint64_t)interval {
    if (!_isRunning || _isPaused || _lastBuffer == nil) {
        return;
    }
    if (rtc::TimeNanos() - _lastDeliveryNs >= (int64_t)(interval * 3 / 2)) {
        [self deliver:_lastBuffer];
    }
}

- (void)setPaused:(BOOL)paused {
    if (_isPaused == paused) {
        return;
    }
    _isPaused = paused;
    std::function<void(bool)> onPause = _onPause;
    if (onPause) {
        dispatch_async(dispatch_get_main_queue(), ^{
            onPause(paused);
        });
    }
}

- (void)reportFatalError {
    [self stopCapture];
    _hasFatalError = YES;
    std::function<void()> onFatalError = _onFatalError;
    if (onFatalError) {
        dispatch_async(dispatch_get_main_queue(), ^{
            onFatalError();
        });
    }
}

- (void)deliver:(RTC_OBJC_TYPE(RTCCVPixelBuffer) *)buffer {
    int64_t timestampNs = rtc::TimeNanos();
    _lastBuffer = buffer;
    _lastDeliveryNs = timestampNs;
    RTC_OBJC_TYPE(RTCVideoFrame) *frame = [[RTC_OBJC_TYPE(RTCVideoFrame) alloc] initWithBuffer:buffer rotation:RTCVideoRotation_0 timeStampNs:timestampNs];
    if (_frameReceived) {
        _frameReceived(frame);
    }
    if (const auto sink = _secondarySink) {
        webrtc::VideoFrame nativeFrame = webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(rtc::make_ref_counted<webrtc::ObjCFrameBuffer>(buffer))
            .set_rotation(webrtc::kVideoRotation_0)
            .set_timestamp_us(timestampNs / rtc::kNumNanosecsPerMicrosec)
            .build();
        sink->OnFrame(nativeFrame);
    }
}

- (void)reconfigureForContentPixelSize:(CGSize)contentPixelSize {
    CGSize desired = outputSizeForContent(contentPixelSize, _data.aspectSize);
    if (_isReconfiguring || (fabs(desired.width - _configuredSize.width) < 8 && fabs(desired.height - _configuredSize.height) < 8)) {
        return;
    }
    SCStream *stream = _stream;
    if (stream == nil) {
        return;
    }
    _isReconfiguring = YES;
    SCStreamConfiguration *configuration = [[SCStreamConfiguration alloc] init];
    configuration.width = (size_t)desired.width;
    configuration.height = (size_t)desired.height;
    configuration.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    configuration.colorMatrix = kCGDisplayStreamYCbCrMatrix_ITU_R_601_4;
    if (_colorSpaceName != nil) {
        configuration.colorSpaceName = (__bridge CFStringRef)_colorSpaceName;
    }
    configuration.minimumFrameInterval = CMTimeMake(1, (int32_t)MAX(1.0, round(_data.fps)));
    configuration.showsCursor = _data.captureMouse;
    configuration.queueDepth = 5;
    __weak ScreenCaptureKitCapturer *weakSelf = self;
    [stream updateConfiguration:configuration completionHandler:^(NSError *error) {
        ScreenCaptureKitCapturer *strongSelf = weakSelf;
        if (strongSelf == nil) {
            return;
        }
        dispatch_async(strongSelf->_queue, ^{
            if (strongSelf->_stream != stream) {
                return;
            }
            strongSelf->_isReconfiguring = NO;
            if (error == nil) {
                strongSelf->_configuredSize = desired;
            }
        });
    }];
}

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen || stream != _stream || !_isRunning || !CMSampleBufferIsValid(sampleBuffer)) {
        return;
    }
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (attachments == nullptr || CFArrayGetCount(attachments) == 0) {
        return;
    }
    NSDictionary *info = (__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments, 0);
    NSNumber *status = info[SCStreamFrameInfoStatus];
    if (status == nil) {
        return;
    }
    switch ((SCFrameStatus)status.integerValue) {
        case SCFrameStatusComplete:
            break;
        case SCFrameStatusBlank:
        case SCFrameStatusSuspended:
            [self setPaused:YES];
            return;
        default:
            return;
    }
    CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (pixelBuffer == nullptr) {
        return;
    }
    int bufferWidth = (int)CVPixelBufferGetWidth(pixelBuffer);
    int bufferHeight = (int)CVPixelBufferGetHeight(pixelBuffer);
    CGRect contentRect = CGRectMake(0, 0, bufferWidth, bufferHeight);
    CGFloat scaleFactor = 1.0;
    CGFloat contentScale = 1.0;
    CGSize sourcePixelSize = CGSizeMake(bufferWidth, bufferHeight);
    NSDictionary *rectDictionary = info[SCStreamFrameInfoContentRect];
    NSNumber *scaleFactorValue = info[SCStreamFrameInfoScaleFactor];
    NSNumber *contentScaleValue = info[SCStreamFrameInfoContentScale];
    if (rectDictionary != nil && scaleFactorValue != nil) {
        CGRect pointRect = CGRectZero;
        if (CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)rectDictionary, &pointRect)) {
            scaleFactor = scaleFactorValue.doubleValue;
            contentRect = CGRectMake(pointRect.origin.x * scaleFactor, pointRect.origin.y * scaleFactor, pointRect.size.width * scaleFactor, pointRect.size.height * scaleFactor);
        }
    }
    if (contentScaleValue != nil && contentScaleValue.doubleValue > 0.0) {
        contentScale = contentScaleValue.doubleValue;
    }
    sourcePixelSize = CGSizeMake(round(contentRect.size.width / contentScale), round(contentRect.size.height / contentScale));
    int cropX = MAX(0, (int)round(contentRect.origin.x)) & ~1;
    int cropY = MAX(0, (int)round(contentRect.origin.y)) & ~1;
    int cropWidth = MIN(bufferWidth - cropX, (int)round(contentRect.size.width));
    int cropHeight = MIN(bufferHeight - cropY, (int)round(contentRect.size.height));
    cropWidth -= cropWidth % 4;
    cropHeight -= cropHeight % 4;
    if (cropWidth < 4 || cropHeight < 4) {
        return;
    }
    [self reconfigureForContentPixelSize:sourcePixelSize];
    [self setPaused:NO];
    RTC_OBJC_TYPE(RTCCVPixelBuffer) *buffer = [[RTC_OBJC_TYPE(RTCCVPixelBuffer) alloc] initWithPixelBuffer:pixelBuffer adaptedWidth:cropWidth adaptedHeight:cropHeight cropWidth:cropWidth cropHeight:cropHeight cropX:cropX cropY:cropY];
    [self deliver:buffer];
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    dispatch_async(_queue, ^{
        if (stream == self->_stream) {
            [self reportFatalError];
        }
    });
}

@end

#endif
