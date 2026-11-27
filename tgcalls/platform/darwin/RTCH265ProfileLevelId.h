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

RTC_OBJC_EXPORT extern NSString *const kRTCVideoCodecH265Name;
RTC_OBJC_EXPORT extern NSString *const kRTCLevel31Main;

// H.265's advertisement: RFC 7798's parameters, with the values a peer that sends none of them implies (Main
// profile, tier 0, level 3.1, SRST). WebRTC M159 matches H.265 on profile, tier and tx-mode; our M123 peers match
// on profile, tier and level as well and advertise H.265 by name only, so the level must stay 3.1 for them to
// keep choosing it.
NSDictionary<NSString *, NSString *> *TGRTCH265StandardParameters(void);
