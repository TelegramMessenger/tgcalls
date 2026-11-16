#ifndef TGCALLS_AV1_AV1SENDCODEC_H
#define TGCALLS_AV1_AV1SENDCODEC_H

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "api/rtp_parameters.h"
#include "third-party/json11.hpp"

namespace tgcalls {

// What one InstanceV2ReferenceImpl call does with AV1, from the server's customParameters:
// - video_av1 (default true): false keeps AV1 out of this call's codec factories (not offered, not decoded);
// - video_av1_send (default true): false never sends AV1, but still offers and decodes it;
// - video_av1_send_any_device (default false): true lets a device the chip gate excludes send AV1.
// CLI-only, read under TGCALLS_ALLOW_TEST_KNOBS: video_av1_test_device_incapable (this side behaves as an older
// device) and video_av1_test_offer_av1_first (AV1 leads the send preference list, as a peer's stock list may).
struct Av1CallSettings {
    bool offerAv1 = true;
    bool sendAv1 = true;
    bool deviceMaySendAv1 = false;
    bool testOfferAv1First = false; bool testEncoderFails = false;
};

Av1CallSettings Av1CallSettingsFromCustomParameters(const std::map<std::string, json11::Json> &customParameters,
                                                    bool cpuCanEncodeAv1);

// The codecs a video sender may be pinned to among those negotiated, in order.
// - AV1 not negotiated: none. A peer without AV1 gets today's call, the default codec.
// - AV1 allowed: AV1 alone. If its pin is rejected, the default codec stays.
// - AV1 not allowed: every H.265, then every H.264, then the other media codecs (never AV1, RTX, RED or FEC).
std::vector<webrtc::RtpCodec> VideoSendCodecCandidates(const std::vector<webrtc::RtpCodec> &available, bool av1Allowed);

// Pins the first candidate `tryPin` accepts. A pin needs exact parameter equality with the sender's own codec list and
// with the negotiated codec, so one can be rejected (a peer's H.264 or H.265 level, say). After a rejection it goes on
// to the next candidate only while the sender still sends AV1 (`sendsAv1`): a device told to leave AV1 must leave it,
// and one already off AV1 keeps its codec rather than falling further. Returns the codec pinned, if any.
// The debug override's candidates: every negotiated codec named `codecName` (case-insensitive), in negotiated order;
// empty when it was not negotiated, for an empty name, and for RTX, RED and FEC.
std::vector<webrtc::RtpCodec> VideoSendCodecOverrideCandidates(const std::vector<webrtc::RtpCodec> &available,
                                                               const std::string &codecName);

std::optional<webrtc::RtpCodec> PinVideoSendCodec(const std::vector<webrtc::RtpCodec> &candidates,
                                                  const std::function<bool(const webrtc::RtpCodec &)> &tryPin,
                                                  const std::function<bool()> &sendsAv1);

} // namespace tgcalls

#endif
