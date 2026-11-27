/*
 *  Copyright 2017 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#import "TGRTCDefaultVideoEncoderFactory.h"

#import "RTCH264ProfileLevelId.h"
#import "TGRTCVideoEncoderH264.h"
#import "api/video_codec/RTCVideoCodecConstants.h"
#import "api/video_codec/RTCVideoEncoderVP8.h"
#import "base/RTCVideoCodecInfo.h"
#if defined(RTC_ENABLE_VP9)
#import "api/video_codec/RTCVideoEncoderVP9.h"
#endif
#ifndef WEBRTC_DISABLE_H265
#import "RTCH265ProfileLevelId.h"
#import "TGRTCVideoEncoderH265.h"
#endif

#import "sdk/objc/api/video_codec/RTCNativeVideoEncoder.h"
#import "sdk/objc/api/video_codec/RTCNativeVideoEncoderBuilder+Native.h"
#import "modules/video_coding/codecs/h264/include/h264.h"
#import "h264_encoder_impl.h"

// The software H.264 encoder, built by CustomObjCVideoEncoderFactory with the call's environment.
@interface TGRTCSoftwareH264EncoderBuilder : RTC_OBJC_TYPE(RTCNativeVideoEncoder) <RTC_OBJC_TYPE(RTCNativeVideoEncoderBuilder)>
- (instancetype)initWithX264:(bool)x264;
@end

@implementation TGRTCSoftwareH264EncoderBuilder {
    bool _x264;
}

- (instancetype)initWithX264:(bool)x264 {
    self = [super init];
    if (self) {
        _x264 = x264;
    }
    return self;
}

- (std::unique_ptr<webrtc::VideoEncoder>)build:(const webrtc::Environment &)env {
    return [self buildWithEnvironment:env format:webrtc::SdpVideoFormat::H264()];
}

- (std::unique_ptr<webrtc::VideoEncoder>)buildWithEnvironment:(const webrtc::Environment &)env format:(const webrtc::SdpVideoFormat &)format {
#ifdef TGCALLS_ENABLE_X264
    if (_x264) {
        return std::make_unique<webrtc::H264EncoderX264Impl>(webrtc::CreateVideoCodec(format));
    }
#endif
    return webrtc::CreateH264Encoder(env, webrtc::H264EncoderSettings::Parse(format));
}

@end
@interface TGRTCDefaultVideoEncoderFactory () {
    bool _preferHardwareH264;
    bool _preferX264;
    bool _highestH264Level;
    bool _screenContent;
}

@end

@implementation TGRTCDefaultVideoEncoderFactory

@synthesize preferredCodec;

- (instancetype)initWithPreferHardwareH264:(bool)preferHardwareH264 preferX264:(bool)preferX264 {
    return [self initWithPreferHardwareH264:preferHardwareH264 preferX264:preferX264 highestH264Level:false];
}

- (instancetype)initWithPreferHardwareH264:(bool)preferHardwareH264 preferX264:(bool)preferX264 highestH264Level:(bool)highestH264Level {
    return [self initWithPreferHardwareH264:preferHardwareH264 preferX264:preferX264 highestH264Level:highestH264Level screenContent:false];
}

- (instancetype)initWithPreferHardwareH264:(bool)preferHardwareH264 preferX264:(bool)preferX264 highestH264Level:(bool)highestH264Level screenContent:(bool)screenContent {
    self = [super init];
    if (self != nil) {
        _preferHardwareH264 = preferHardwareH264;
        _preferX264 = preferX264;
        _highestH264Level = highestH264Level;
        _screenContent = screenContent;
    }
    return self;
}

// Constrained high, level 5.2 (level_idc 52 = 0x34). One entry on purpose: the group
// engine keeps the first H264 format after sorting them, and its priority ranks only the
// level-3.1 strings, so two level-5.2 entries would tie.
static RTCVideoCodecInfo *highestLevelH264CodecInfo() {
  NSDictionary<NSString *, NSString *> *parameters = @{
    @"profile-level-id" : @"640c34",
    @"level-asymmetry-allowed" : @"1",
    @"packetization-mode" : @"1",
  };
  return [[RTCVideoCodecInfo alloc] initWithName:kRTCVideoCodecH264Name parameters:parameters];
}

+ (NSArray<RTCVideoCodecInfo *> *)supportedCodecs {
  NSDictionary<NSString *, NSString *> *constrainedHighParams = @{
    @"profile-level-id" : kRTCMaxSupportedH264ProfileLevelConstrainedHigh,
    @"level-asymmetry-allowed" : @"1",
    @"packetization-mode" : @"1",
  };
  RTCVideoCodecInfo *constrainedHighInfo =
      [[RTCVideoCodecInfo alloc] initWithName:kRTCVideoCodecH264Name
                                   parameters:constrainedHighParams];

  NSDictionary<NSString *, NSString *> *constrainedBaselineParams = @{
    @"profile-level-id" : kRTCMaxSupportedH264ProfileLevelConstrainedBaseline,
    @"level-asymmetry-allowed" : @"1",
    @"packetization-mode" : @"1",
  };
  RTCVideoCodecInfo *constrainedBaselineInfo =
      [[RTCVideoCodecInfo alloc] initWithName:kRTCVideoCodecH264Name
                                   parameters:constrainedBaselineParams];

  RTCVideoCodecInfo *vp8Info = [[RTCVideoCodecInfo alloc] initWithName:kRTCVideoCodecVp8Name];

#if defined(RTC_ENABLE_VP9)
  RTCVideoCodecInfo *vp9Info = [[RTCVideoCodecInfo alloc] initWithName:kRTCVideoCodecVp9Name];
#endif

#if !defined(WEBRTC_DISABLE_H265)
  RTCVideoCodecInfo *h265Info = [[RTCVideoCodecInfo alloc] initWithName:kRTCVideoCodecH265Name parameters:TGRTCH265StandardParameters()];
#endif

  NSMutableArray *result = [[NSMutableArray alloc] initWithArray:@[
    constrainedHighInfo,
    constrainedBaselineInfo,
    vp8Info,
#if defined(RTC_ENABLE_VP9)
    vp9Info,
#endif
  ]];
    
#if !defined(WEBRTC_DISABLE_H265)
#ifdef WEBRTC_IOS
  if (@available(iOS 11.0, *)) {
    if ([[AVAssetExportSession allExportPresets] containsObject:AVAssetExportPresetHEVCHighestQuality]) {
      [result addObject:h265Info];
    }
  }
#else // WEBRTC_IOS
  if (@available(macOS 10.13, *)) {
    if ([[AVAssetExportSession allExportPresets] containsObject:AVAssetExportPresetHEVCHighestQuality]) {
      [result addObject:h265Info];
    }
  }
#endif // WEBRTC_IOS
#endif
    
    return result;
}

- (id<RTCVideoEncoder>)createEncoder:(RTCVideoCodecInfo *)info {
  if ([info.name isEqualToString:kRTCVideoCodecH264Name]) {
      if (_preferHardwareH264) {
          return [[TGRTCVideoEncoderH264 alloc] initWithCodecInfo:info screenContent:_screenContent];
      } else {
          
          return [[TGRTCSoftwareH264EncoderBuilder alloc] initWithX264:_preferX264];
      }
  } else if ([info.name isEqualToString:kRTCVideoCodecVp8Name]) {
    return [RTCVideoEncoderVP8 vp8Encoder];
  }
#if defined(RTC_ENABLE_VP9)
  if ([info.name isEqualToString:kRTCVideoCodecVp9Name]) {
    return [RTCVideoEncoderVP9 vp9Encoder];
  }
#endif

#if !defined(WEBRTC_DISABLE_H265)
#ifdef WEBRTC_IOS
  if (@available(iOS 11, *)) {
    if ([info.name isEqualToString:kRTCVideoCodecH265Name]) {
      return [[RTCVideoEncoderH265 alloc] initWithCodecInfo:info];
    }
  }
#else // WEBRTC_IOS
  if (@available(macOS 10.13, *)) {
    if ([info.name isEqualToString:kRTCVideoCodecH265Name]) {
      return [[RTCVideoEncoderH265 alloc] initWithCodecInfo:info];
    }
  }
#endif // WEBRTC_IOS
#endif // !DISABLE_H265

  return nil;
}

- (NSArray<RTCVideoCodecInfo *> *)supportedCodecs {
  NSMutableArray<RTCVideoCodecInfo *> *codecs = [[[self class] supportedCodecs] mutableCopy];

  if (_highestH264Level) {
    NSUInteger firstH264Index = NSNotFound;
    for (NSUInteger i = 0; i < codecs.count; i++) {
      if ([codecs[i].name isEqualToString:kRTCVideoCodecH264Name]) {
        firstH264Index = i;
        break;
      }
    }
    if (firstH264Index != NSNotFound) {
      NSIndexSet *h264Indices = [codecs indexesOfObjectsPassingTest:^BOOL(RTCVideoCodecInfo *codec, NSUInteger idx, BOOL *stop) {
        return [codec.name isEqualToString:kRTCVideoCodecH264Name];
      }];
      [codecs removeObjectsAtIndexes:h264Indices];
      [codecs insertObject:highestLevelH264CodecInfo() atIndex:firstH264Index];
    }
  }

  NSMutableArray<RTCVideoCodecInfo *> *orderedCodecs = [NSMutableArray array];
  NSUInteger index = [codecs indexOfObject:self.preferredCodec];
  if (index != NSNotFound) {
    [orderedCodecs addObject:[codecs objectAtIndex:index]];
    [codecs removeObjectAtIndex:index];
  }
  [orderedCodecs addObjectsFromArray:codecs];

  return [orderedCodecs copy];
}

@end
