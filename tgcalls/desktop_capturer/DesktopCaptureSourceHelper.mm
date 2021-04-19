//
//  DesktopCaptureSourceHelper.m
//  TgVoipWebrtc
//
//  Created by Mikhail Filimonov on 28.12.2020.
//  Copyright © 2020 Mikhail Filimonov. All rights reserved.
//

#import "DesktopCaptureSourceHelper.h"
#include <iostream>
#include <memory>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>
#include "api/scoped_refptr.h"
#include "rtc_base/thread.h"

#include "modules/desktop_capture/mac/screen_capturer_mac.h"
#include "modules/desktop_capture/desktop_and_cursor_composer.h"
#include "modules/desktop_capture/desktop_capturer_differ_wrapper.h"
#include "third_party/libyuv/include/libyuv.h"
#include "api/video/i420_buffer.h"

#include "rtc_base/weak_ptr.h"

#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv.h"
#import "helpers/RTCDispatcher+Private.h"
#import <QuartzCore/QuartzCore.h>
#include "ThreadLocalObject.h"

namespace {

CGSize aspectFitted(CGSize from, CGSize to) {
    CGFloat scale = MAX(from.width / MAX(1.0, to.width), from.height / MAX(1.0, to.height));
    return NSMakeSize(ceil(to.width * scale), ceil(to.height * scale));
}

void onMain(std::function<void()> method) {
    dispatch_async(dispatch_get_main_queue(), ^{
        method();
    });
}

void onMainDelayed(int delayMs, std::function<void()> method) {
    const auto time = dispatch_time(
        DISPATCH_TIME_NOW,
        ((long long)delayMs * NSEC_PER_SEC) / 1000);
    dispatch_after(time, dispatch_get_main_queue(), ^{
        method();
    });
}

} // namespace

class SourceFrameCallbackImpl : public webrtc::DesktopCapturer::Callback {
public:
    std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> _sink;
    std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> _secondarySink;

    SourceFrameCallbackImpl(CGSize size, int fps) : size_(size), fps_(fps) {
    }

    void OnCaptureResult(
            webrtc::DesktopCapturer::Result result,
            std::unique_ptr<webrtc::DesktopFrame> frame) override {
        if (result != webrtc::DesktopCapturer::Result::SUCCESS) {
            return;
        }

        std::unique_ptr<webrtc::DesktopFrame> output_frame_;
        
        CGSize fittedSize = aspectFitted(size_, CGSizeMake(frame->size().width(), frame->size().height()));
        while ((int(fittedSize.width) / 2) % 16 != 0) {
            fittedSize.width -= 1;
        }
        
        webrtc::DesktopSize output_size(fittedSize.width,
                                        fittedSize.height);

        output_frame_.reset(new webrtc::BasicDesktopFrame(output_size));
        
        webrtc::DesktopRect output_rect_ = webrtc::DesktopRect::MakeSize(output_size);

        uint8_t* output_rect_data = output_frame_->data() +
            output_frame_->stride() * output_rect_.top() +
            webrtc::DesktopFrame::kBytesPerPixel * output_rect_.left();

        
        libyuv::ARGBScale(frame->data(), frame->stride(), frame->size().width(),
                             frame->size().height(), output_rect_data,
                           output_frame_->stride(), output_size.width(),
                          output_size.height(), libyuv::kFilterBilinear);

        int width = output_frame_->size().width();
        int height = output_frame_->size().height();
        int stride_y = width;
        int stride_uv = (width + 1) / 2;

        if (!i420_buffer_.get() || i420_buffer_->width() != output_frame_->size().width() || i420_buffer_->height() != height) {
            i420_buffer_ = webrtc::I420Buffer::Create(output_frame_->size().width(), height, stride_y, stride_uv, stride_uv);
        }
        
        int i420Result = libyuv::ConvertToI420(output_frame_->data(), width * height,
                                               i420_buffer_->MutableDataY(), i420_buffer_->StrideY(),
                                               i420_buffer_->MutableDataU(), i420_buffer_->StrideU(),
                                               i420_buffer_->MutableDataV(), i420_buffer_->StrideV(),
                                               0, 0,
                                               width, height,
                                               output_frame_->size().width(), height,
                                               libyuv::kRotate0,
                                               libyuv::FOURCC_ARGB);


        assert(i420Result == 0);
        webrtc::VideoFrame nativeVideoFrame = webrtc::VideoFrame(i420_buffer_, webrtc::kVideoRotation_0, next_timestamp_ / rtc::kNumNanosecsPerMicrosec);
        if (_sink != NULL) {
            _sink->OnFrame(nativeVideoFrame);
        }
        if (_secondarySink != NULL) {
            _secondarySink->OnFrame(nativeVideoFrame);
        }
        next_timestamp_ += rtc::kNumNanosecsPerSec / double(fps_);
    }

private:
    rtc::scoped_refptr<webrtc::I420Buffer> i420_buffer_;
    int64_t next_timestamp_ = 0;
    CGSize size_;
    int fps_ = 0;

};

@interface DesktopCaptureSource ()
- (webrtc::DesktopCapturer::Source)getSource;
@end

@interface DesktopSourceRenderer : NSObject
-(id)initWithSource:(DesktopCaptureSource *)source data: (DesktopCaptureSourceData *)data;
-(void)Stop;
-(void)Start;
-(void)SetOutput:(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>)sink;
-(void)SetSecondaryOutput:(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>)sink;
@end

static int count = 0;

@implementation DesktopSourceRenderer
{
    std::unique_ptr<webrtc::DesktopCapturer> _capturer;
    std::shared_ptr<SourceFrameCallbackImpl> _callback;
    std::shared_ptr<bool> timerGuard;
    bool isRunning;
    double delayMs;
}
-(id)initWithSource:(DesktopCaptureSource *)source data: (DesktopCaptureSourceData *)data {
    if (self = [super init]) {
        delayMs = 1000 / double(data.fps);
        isRunning = false;
        _callback = std::make_shared<SourceFrameCallbackImpl>(data.aspectSize, data.fps);

        auto options = webrtc::DesktopCaptureOptions::CreateDefault();
        options.set_disable_effects(true);
        options.set_detect_updated_region(true);
        options.set_allow_iosurface(true);
        
        if (source.isWindow) {
            _capturer = webrtc::DesktopCapturer::CreateWindowCapturer(options);
        } else {
            _capturer = webrtc::DesktopCapturer::CreateScreenCapturer(options);
        }
        if (data.captureMouse) {
            _capturer = std::make_unique<webrtc::DesktopAndCursorComposer>(
                std::move(_capturer),
                options);
        }
        _capturer->SelectSource([source getSource].id);
        _capturer->Start(_callback.get());
    }
    return self;
}

-(void)Start {
    if (isRunning) {
        return;
    }
    count++;
    NSLog(@"current capture count: %d", count);

    isRunning = true;
    timerGuard = std::make_shared<bool>(true);
    [self Loop];
}

-(void)Stop {
    if (isRunning) {
        count--;
        NSLog(@"current capture count: %d", count);
    }
    isRunning = false;
    timerGuard = nullptr;
}

-(void)Loop {
    if(!isRunning) {
        return;
    }

    _capturer->CaptureFrame();
    __weak id weakSelf = self;
    const auto guard = std::weak_ptr<bool>(timerGuard);
    onMainDelayed(delayMs, [=] {
        if (guard.lock()) {
            [weakSelf Loop];
        }
    });
}

-(void)SetOutput:(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>)sink {
    _callback->_sink = sink;
}
-(void)SetSecondaryOutput:(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>)sink {
    _callback->_secondarySink = sink;
}

-(void)dealloc {
    int bp = 0;
    bp += 1;
}

@end


@interface DesktopCaptureSourceHelper ()
@end

@implementation DesktopCaptureSourceHelper
{
    std::shared_ptr<DesktopSourceRenderer*> _renderer;
}

-(instancetype)initWithWindow:(DesktopCaptureSource *)source data: (DesktopCaptureSourceData *)data  {
    if (self = [super init]) {
        _renderer = std::make_shared<DesktopSourceRenderer*>(nil);
        onMain([source, data, renderer = _renderer] {
            *renderer = [[DesktopSourceRenderer alloc] initWithSource:source data:data];
        });
    }
    return self;
}

-(void)setOutput:(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>)sink {
    onMain([sink, renderer = _renderer] {
        [*renderer SetOutput:sink];
    });
}

-(void)setSecondaryOutput:(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>)sink {
    onMain([sink, renderer = _renderer] {
        [*renderer SetSecondaryOutput:sink];
    });
}

-(void)start {
    onMain([renderer = _renderer] {
        [*renderer Start];
    });
}

-(void)stop {
    onMain([renderer = _renderer] {
        [*renderer Stop];
    });
}

-(void)dealloc {
	onMain([renderer = std::move(_renderer)] {
	});
}

@end
