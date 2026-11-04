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
#endif
    return settings;
}

std::optional<webrtc::RtpCodec> ChooseVideoSendCodec(const std::vector<webrtc::RtpCodec> &available, bool av1Allowed) {
    if (av1Allowed) {
        if (auto av1 = FirstNamed(available, cricket::kAv1CodecName)) {
            return av1;
        }
    }
    if (auto h265 = FirstNamed(available, cricket::kH265CodecName)) {
        return h265;
    }
    if (auto h264 = FirstNamed(available, cricket::kH264CodecName)) {
        return h264;
    }
    for (const auto &codec : available) {
        if (!IsNamed(codec, cricket::kAv1CodecName) && !IsResilienceCodec(codec)) {
            return codec;
        }
    }
    return std::nullopt;
}

} // namespace tgcalls
