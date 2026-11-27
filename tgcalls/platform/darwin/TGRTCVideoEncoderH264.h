/*
 *  Copyright 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#import <Foundation/Foundation.h>

#import "RTCMacros.h"
#import "RTCVideoCodecInfo.h"
#import "RTCVideoEncoder.h"

RTC_OBJC_EXPORT
@interface TGRTCVideoEncoderH264 : NSObject <RTCVideoEncoder>

- (instancetype)initWithCodecInfo:(RTCVideoCodecInfo *)codecInfo;

/** screenContent: rate control for a screen capture. VideoToolbox's H264 encoder starves itself
 *  whenever kVTCompressionPropertyKey_DataRateLimits is set, whatever its value (screen-like
 *  content at 836x1192 and 60 fps, 2.6 Mbit/s average: 0.4-0.9 Mbit/s out at QP 44-51 with a
 *  limit of 1.5x to 20x the average, up to the average without one), so a screen capture sets
 *  none, and a minimum frame QP of 20 instead keeps a still screen from spending the budget on
 *  refining an image that does not change. */
- (instancetype)initWithCodecInfo:(RTCVideoCodecInfo *)codecInfo screenContent:(bool)screenContent;

@end
