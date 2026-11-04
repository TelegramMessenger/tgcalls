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
#import "RTCVideoEncoderFactory.h"

NS_ASSUME_NONNULL_BEGIN

/** This encoder factory include support for all codecs bundled with WebRTC. If using custom
 *  codecs, create custom implementations of RTCVideoEncoderFactory and RTCVideoDecoderFactory.
 */
RTC_OBJC_EXPORT
@interface TGRTCDefaultVideoEncoderFactory : NSObject <RTCVideoEncoderFactory>

@property(nonatomic, retain) RTCVideoCodecInfo *preferredCodec;

- (instancetype)initWithPreferHardwareH264:(bool)preferHardwareH264 preferX264:(bool)preferX264;

/** highestH264Level: advertise a single H264 format, constrained high at level 5.2 (the
 *  highest VideoToolbox level), instead of the per-device constrained high and baseline
 *  entries. Used by the iOS screencast, which encodes screen-sized frames at 60 fps. */
- (instancetype)initWithPreferHardwareH264:(bool)preferHardwareH264 preferX264:(bool)preferX264 highestH264Level:(bool)highestH264Level;

+ (NSArray<RTCVideoCodecInfo *> *)supportedCodecs;

@end

NS_ASSUME_NONNULL_END
