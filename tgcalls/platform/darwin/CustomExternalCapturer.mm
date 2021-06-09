#include "CustomExternalCapturer.h"

#import <AVFoundation/AVFoundation.h>

#include "rtc_base/logging.h"
#import "base/RTCLogging.h"
#import "base/RTCVideoFrameBuffer.h"
#import "TGRTCCVPixelBuffer.h"
#import "sdk/objc/native/src/objc_video_track_source.h"
#import "sdk/objc/native/src/objc_frame_buffer.h"
#import "api/video_track_source_proxy.h"

#import "helpers/UIDevice+RTCDevice.h"

#import "helpers/AVCaptureSession+DevicePosition.h"
#import "helpers/RTCDispatcher+Private.h"
#import "base/RTCVideoFrame.h"

#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv.h"

#include "DarwinVideoSource.h"

static const int64_t kNanosecondsPerSecond = 1000000000;

static tgcalls::DarwinVideoTrackSource *getObjCVideoSource(const rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> nativeSource) {
    webrtc::VideoTrackSourceProxy *proxy_source =
    static_cast<webrtc::VideoTrackSourceProxy *>(nativeSource.get());
    return static_cast<tgcalls::DarwinVideoTrackSource *>(proxy_source->internal());
}

@interface CustomExternalCapturer () {
    rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> _source;
}

@end

@implementation CustomExternalCapturer

- (instancetype)initWithSource:(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface>)source {
    self = [super init];
    if (self != nil) {
        _source = source;
    }
    return self;
}

- (void)dealloc {
}

+ (void)passSampleBuffer:(CMSampleBufferRef)sampleBuffer toSource:(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface>)source {
    if (CMSampleBufferGetNumSamples(sampleBuffer) != 1 || !CMSampleBufferIsValid(sampleBuffer) ||
        !CMSampleBufferDataIsReady(sampleBuffer)) {
        return;
    }

    CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (pixelBuffer == nil) {
        return;
    }

    TGRTCCVPixelBuffer *rtcPixelBuffer = [[TGRTCCVPixelBuffer alloc] initWithPixelBuffer:pixelBuffer];

    /*int width = rtcPixelBuffer.width / 4;
    int height = rtcPixelBuffer.height / 4;

    CVPixelBufferRef outputPixelBufferRef = NULL;
    OSType pixelFormat = CVPixelBufferGetPixelFormatType(rtcPixelBuffer.pixelBuffer);
    CVPixelBufferCreate(NULL, width, height, pixelFormat, NULL, &outputPixelBufferRef);
    if (outputPixelBufferRef) {
        int bufferSize = [rtcPixelBuffer bufferSizeForCroppingAndScalingToWidth:width height:height];
        std::vector<uint8_t> croppingBuffer;
        if (croppingBuffer.size() < bufferSize) {
            croppingBuffer.resize(bufferSize);
        }
        if ([rtcPixelBuffer cropAndScaleTo:outputPixelBufferRef withTempBuffer:croppingBuffer.data()]) {
            rtcPixelBuffer = [[TGRTCCVPixelBuffer alloc] initWithPixelBuffer:outputPixelBufferRef];
        }
        CVPixelBufferRelease(outputPixelBufferRef);
    }*/

    int64_t timeStampNs = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sampleBuffer)) *
    kNanosecondsPerSecond;
    RTCVideoFrame *videoFrame = [[RTCVideoFrame alloc] initWithBuffer:rtcPixelBuffer rotation:RTCVideoRotation_0 timeStampNs:timeStampNs];

    getObjCVideoSource(source)->OnCapturedFrame(videoFrame);
}

+ (void)passPixelBuffer:(CVPixelBufferRef)pixelBuffer rotation:(RTCVideoRotation)rotation toSource:(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface>)source croppingBuffer:(std::vector<uint8_t> &)croppingBuffer {
    TGRTCCVPixelBuffer *rtcPixelBuffer = [[TGRTCCVPixelBuffer alloc] initWithPixelBuffer:pixelBuffer];

    int64_t timeStampNs = CACurrentMediaTime() * kNanosecondsPerSecond;
    RTCVideoFrame *videoFrame = [[RTCVideoFrame alloc] initWithBuffer:(id<RTCVideoFrameBuffer>)[rtcPixelBuffer toI420] rotation:rotation timeStampNs:timeStampNs];

    getObjCVideoSource(source)->OnCapturedFrame(videoFrame);
}

- (void)addSampleBuffer:(CMSampleBufferRef)sampleBuffer {
    [CustomExternalCapturer passSampleBuffer:sampleBuffer toSource:_source];
}

@end
//
