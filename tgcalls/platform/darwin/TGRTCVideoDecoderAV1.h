#import <Foundation/Foundation.h>

#import "RTCMacros.h"
#import "RTCVideoDecoder.h"

// AV1 through VideoToolbox (Av1VideoToolboxDecoder), for chips that decode AV1 in hardware; the decoder factory picks
// it only when Av1VideoToolboxDecoder::IsSupported(), and dav1d otherwise.
RTC_OBJC_EXPORT
@interface TGRTCVideoDecoderAV1 : NSObject <RTCVideoDecoder>
@end
