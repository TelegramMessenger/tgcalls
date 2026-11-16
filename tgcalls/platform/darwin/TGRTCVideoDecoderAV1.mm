#import "TGRTCVideoDecoderAV1.h"

#import "base/RTCVideoFrame.h"
#import "base/RTCVideoFrameBuffer.h"
#import "components/video_frame_buffer/RTCCVPixelBuffer.h"

#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

#include "platform/darwin/Av1VideoToolboxDecoder.h"

#include <memory>

@implementation TGRTCVideoDecoderAV1 {
    std::unique_ptr<tgcalls::Av1VideoToolboxDecoder> _decoder;
    RTCVideoDecoderCallback _callback;
}

- (NSInteger)startDecodeWithNumberOfCores:(int)numberOfCores {
    return WEBRTC_VIDEO_CODEC_OK;
}

- (NSInteger)decode:(RTCEncodedImage *)inputImage
        missingFrames:(BOOL)missingFrames
    codecSpecificInfo:(__nullable id<RTCCodecSpecificInfo>)info
         renderTimeMs:(int64_t)renderTimeMs {
    if (!inputImage.buffer || inputImage.buffer.length == 0) {
        return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    if (!_decoder) {
        // The decoder outlives no call of this object: it is destroyed in releaseDecoder and dealloc, and decodes
        // synchronously, so the block never runs after self is gone.
        __unsafe_unretained TGRTCVideoDecoderAV1 *unretainedSelf = self;
        _decoder = std::make_unique<tgcalls::Av1VideoToolboxDecoder>([unretainedSelf](CVPixelBufferRef pixelBuffer, uint32_t rtpTimestamp) {
            RTCVideoDecoderCallback callback = unretainedSelf->_callback;
            if (!callback) {
                return;
            }
            RTCCVPixelBuffer *frameBuffer = [[RTCCVPixelBuffer alloc] initWithPixelBuffer:pixelBuffer];
            RTCVideoFrame *frame = [[RTCVideoFrame alloc] initWithBuffer:frameBuffer
                                                                rotation:RTCVideoRotation_0
                                                             timeStampNs:rtc::TimeNanos()];
            frame.timeStamp = (int32_t)rtpTimestamp;
            callback(frame);
        });
    }
    if (!_decoder->Decode((const uint8_t *)inputImage.buffer.bytes, inputImage.buffer.length, (uint32_t)inputImage.timeStamp)) {
        // An error makes WebRTC ask the sender for a keyframe, which carries the sequence header.
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    return WEBRTC_VIDEO_CODEC_OK;
}

- (void)setCallback:(RTCVideoDecoderCallback)callback {
    _callback = callback;
}

- (NSInteger)releaseDecoder {
    _decoder.reset();
    _callback = nil;
    return WEBRTC_VIDEO_CODEC_OK;
}

- (void)dealloc {
    _decoder.reset();
}

- (NSString *)implementationName {
    return @"VideoToolboxAV1";
}

@end
