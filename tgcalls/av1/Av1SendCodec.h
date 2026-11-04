#ifndef TGCALLS_AV1_AV1SENDCODEC_H
#define TGCALLS_AV1_AV1SENDCODEC_H

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
    bool testOfferAv1First = false;
};

Av1CallSettings Av1CallSettingsFromCustomParameters(const std::map<std::string, json11::Json> &customParameters,
                                                    bool cpuCanEncodeAv1);

// The codec a video sender is pinned to among those negotiated: AV1 when allowed and negotiated; otherwise H.265,
// then H.264, then the first other media codec (never RTX, RED or FEC). Empty when nothing qualifies.
std::optional<webrtc::RtpCodec> ChooseVideoSendCodec(const std::vector<webrtc::RtpCodec> &available, bool av1Allowed);

} // namespace tgcalls

#endif
