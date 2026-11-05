#include "av1/Av1SendCodec.h"

#include "absl/strings/match.h"
#include "media/base/media_constants.h"

#include "v2/CustomParameters.h"

namespace tgcalls {

namespace {

bool IsNamed(const webrtc::RtpCodec &codec, const char *name) {
    return absl::EqualsIgnoreCase(codec.name, name);
}

bool IsResilienceCodec(const webrtc::RtpCodec &codec) {
    return IsNamed(codec, cricket::kRtxCodecName) || IsNamed(codec, cricket::kRedCodecName) ||
           IsNamed(codec, cricket::kUlpfecCodecName) || IsNamed(codec, cricket::kFlexfecCodecName);
}

std::optional<webrtc::RtpCodec> FirstNamed(const std::vector<webrtc::RtpCodec> &available, const char *name) {
    for (const auto &codec : available) {
        if (IsNamed(codec, name)) {
            return codec;
        }
    }
    return std::nullopt;
}

} // namespace

Av1CallSettings Av1CallSettingsFromCustomParameters(const std::map<std::string, json11::Json> &customParameters,
                                                    bool cpuCanEncodeAv1) {
    Av1CallSettings settings;
    settings.offerAv1 = getCustomParameterBoolDefaultTrue(customParameters, "video_av1");
    settings.sendAv1 = getCustomParameterBoolDefaultTrue(customParameters, "video_av1_send");
    settings.deviceMaySendAv1 = cpuCanEncodeAv1 || getCustomParameterBool(customParameters, "video_av1_send_any_device");
#if TGCALLS_ALLOW_TEST_KNOBS
    if (getCustomParameterBool(customParameters, "video_av1_test_device_incapable")) {
        settings.deviceMaySendAv1 = false;
    }
    settings.testOfferAv1First = getCustomParameterBool(customParameters, "video_av1_test_offer_av1_first");
    settings.testEncoderFails = getCustomParameterBool(customParameters, "video_av1_test_encoder_fails");
#endif
    return settings;
}

std::vector<webrtc::RtpCodec> VideoSendCodecCandidates(const std::vector<webrtc::RtpCodec> &available, bool av1Allowed) {
    const auto av1 = FirstNamed(available, cricket::kAv1CodecName);
    if (!av1) {
        return {};
    }
    if (av1Allowed) {
        return {*av1};
    }
    std::vector<webrtc::RtpCodec> candidates;
    for (const char *name : {cricket::kH265CodecName, cricket::kH264CodecName}) {
        for (const auto &codec : available) {
            if (IsNamed(codec, name)) {
                candidates.push_back(codec);
            }
        }
    }
    for (const auto &codec : available) {
        if (!IsNamed(codec, cricket::kAv1CodecName) && !IsNamed(codec, cricket::kH265CodecName) &&
            !IsNamed(codec, cricket::kH264CodecName) && !IsResilienceCodec(codec)) {
            candidates.push_back(codec);
        }
    }
    return candidates;
}

std::vector<webrtc::RtpCodec> VideoSendCodecOverrideCandidates(const std::vector<webrtc::RtpCodec> &available,
                                                               const std::string &codecName) {
    std::vector<webrtc::RtpCodec> candidates;
    if (codecName.empty()) {
        return candidates;
    }
    for (const auto &codec : available) {
        if (absl::EqualsIgnoreCase(codec.name, codecName) && !IsResilienceCodec(codec)) {
            candidates.push_back(codec);
        }
    }
    return candidates;
}

std::optional<webrtc::RtpCodec> PinVideoSendCodec(const std::vector<webrtc::RtpCodec> &candidates,
                                                  const std::function<bool(const webrtc::RtpCodec &)> &tryPin,
                                                  const std::function<bool()> &sendsAv1) {
    for (const auto &codec : candidates) {
        if (tryPin(codec)) {
            return codec;
        }
        if (!sendsAv1()) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace tgcalls
