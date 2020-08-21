#import "ScreenCapturer.h"

#import "modules/desktop_capture/mac/screen_capturer_mac.h"
#include "third_party/libyuv/include/libyuv.h"
#include "api/video/i420_buffer.h"


#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv.h"
#import "helpers/RTCDispatcher+Private.h"
#import <QuartzCore/QuartzCore.h>

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

class DesktopFrameCallbackImpl : public webrtc::DesktopCapturer::Callback {
public:
    std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> _sink;
    DesktopFrameCallbackImpl(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source) {
        _source = source;
    }
    virtual void OnCaptureResult(webrtc::DesktopCapturer::Result result,
                                 std::unique_ptr<webrtc::DesktopFrame> frame) {
        if (result != webrtc::DesktopCapturer::Result::SUCCESS) {
            return;
        }
        
        int width = frame->size().width();
        int height = frame->size().height();
        
        if (!i420_buffer_.get() ||
            i420_buffer_->width() * i420_buffer_->height() < width * height) {
            i420_buffer_ = webrtc::I420Buffer::Create(500, 300);
        }
        
        
        
        int i420Result = libyuv::ConvertToI420(frame->data(), 0, i420_buffer_->MutableDataY(),
                              i420_buffer_->StrideY(), i420_buffer_->MutableDataU(),
                              i420_buffer_->StrideU(), i420_buffer_->MutableDataV(),
                              i420_buffer_->StrideV(), 0, 0, width, height, 500,
                              300, libyuv::kRotate0, libyuv::FOURCC_ARGB);
        
        assert(i420Result == 0);

        webrtc::VideoFrame nativeVideoFrame = webrtc::VideoFrame(i420_buffer_, webrtc::kVideoRotation_0, frame->capture_time_ms());
        
        if (_sink != NULL) {
            _sink->OnFrame(nativeVideoFrame);
        }
        RTCVideoRotation rotation = RTCVideoRotation_0;
        RTCVideoFrame* videoFrame = customToObjCVideoFrame(nativeVideoFrame, rotation);
        getObjCVideoSource(_source)->OnCapturedFrame(videoFrame);
    }
private:
    rtc::scoped_refptr<webrtc::I420Buffer> i420_buffer_;
    rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> _source;
};

@implementation AppScreenCapturer {
    std::unique_ptr<webrtc::DesktopCapturer> _capturer;
    std::shared_ptr<DesktopFrameCallbackImpl> _callback;
    dispatch_queue_t _frameQueue;

}
- (instancetype)initWithSource:(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface>)source {
    self = [super init];
    if (self != nil) {
        
        _callback.reset(new DesktopFrameCallbackImpl(source));
        _capturer = webrtc::ScreenCapturerMac::CreateScreenCapturer(webrtc::DesktopCaptureOptions::CreateDefault());
        _capturer->Start(_callback.get());
        
//        std::vector<cricket::VideoFormat> formats;
//        formats.push_back(cricket::VideoFormat(800, 600, cricket::VideoFormat::FpsToInterval(20), cricket::FOURCC_ARGB));
//
//
        [RTCDispatcher dispatchAsyncOnType:RTCDispatcherTypeCaptureSession block:^{
            [self captureFrame];
        }];
        
    }
    return self;
}

-(void)captureFrame {
    self->_capturer->CaptureFrame();
    
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 0.2 * NSEC_PER_SEC), self.frameQueue, ^{
        [RTCDispatcher dispatchAsyncOnType:RTCDispatcherTypeCaptureSession block:^{
            [self captureFrame];
        }];
        
    });
}

-(void)setSink:(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>)sink {
    dispatch_async(self.frameQueue, ^{
        self->_callback->_sink = sink;
    });
}

- (dispatch_queue_t)frameQueue {
    if (!_frameQueue) {
        _frameQueue =
        dispatch_queue_create("org.webrtc.desktopcapturer.video", DISPATCH_QUEUE_SERIAL);
        dispatch_set_target_queue(_frameQueue,
                                  dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0));
    }
    return _frameQueue;
}

-(void)dealloc {
    _capturer.reset();
    _callback.reset();
}

@end
