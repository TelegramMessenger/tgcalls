#include "group/GroupVideoPayloadTypes.h"

#include "absl/strings/match.h"
#include "api/video_codecs/scalability_mode.h"
#include "media/base/media_constants.h"
#include "rtc_base/logging.h"

#include <algorithm>
#include <string>

namespace tgcalls {

namespace {

static void addDefaultFeedbackParams(webrtc::Codec *codec) {
    // Don't add any feedback params for RED and ULPFEC.
    if (codec->name == webrtc::kRedCodecName || codec->name == webrtc::kUlpfecCodecName) {
        return;
    }
    codec->AddFeedbackParam(webrtc::FeedbackParam(webrtc::kRtcpFbParamRemb, webrtc::kParamValueEmpty));
    codec->AddFeedbackParam(webrtc::FeedbackParam(webrtc::kRtcpFbParamTransportCc, webrtc::kParamValueEmpty));
    // Don't add any more feedback params for FLEXFEC.
    if (codec->name == webrtc::kFlexfecCodecName) {
        return;
    }
    codec->AddFeedbackParam(webrtc::FeedbackParam(webrtc::kRtcpFbParamCcm, webrtc::kRtcpFbCcmParamFir));
    codec->AddFeedbackParam(webrtc::FeedbackParam(webrtc::kRtcpFbParamNack, webrtc::kParamValueEmpty));
    codec->AddFeedbackParam(webrtc::FeedbackParam(webrtc::kRtcpFbParamNack, webrtc::kRtcpFbNackParamPli));
}

struct H264FormatParameters {
    std::string profileLevelId;
    std::string packetizationMode;
    std::string levelAssymetryAllowed;
};

static H264FormatParameters parseH264FormatParameters(webrtc::SdpVideoFormat const &format) {
    H264FormatParameters result;

    for (const auto &parameter : format.parameters) {
        if (parameter.first == "profile-level-id") {
            result.profileLevelId = parameter.second;
        } else if (parameter.first == "packetization-mode") {
            result.packetizationMode = parameter.second;
        } else if (parameter.first == "level-asymmetry-allowed") {
            result.levelAssymetryAllowed = parameter.second;
        }
    }

    return result;
}

static int getH264ProfileLevelIdPriority(std::string const &profileLevelId) {
    if (profileLevelId == webrtc::kH264ProfileLevelConstrainedHigh) {
        return 0;
    } else if (profileLevelId == webrtc::kH264ProfileLevelConstrainedBaseline) {
        return 1;
    } else {
        return 2;
    }
}

static int getH264PacketizationModePriority(std::string const &packetizationMode) {
    if (packetizationMode == "1") {
        return 0;
    } else {
        return 1;
    }
}

static int getH264LevelAssymetryAllowedPriority(std::string const &levelAssymetryAllowed) {
    if (levelAssymetryAllowed == "1") {
        return 0;
    } else {
        return 1;
    }
}

} // namespace

std::vector<webrtc::SdpVideoFormat> filterGroupVideoFormats(std::vector<webrtc::SdpVideoFormat> const &formats) {
    std::vector<webrtc::SdpVideoFormat> filteredFormats;

    std::vector<std::string> filterCodecNames = {
        webrtc::kVp8CodecName,
        webrtc::kVp9CodecName,
        webrtc::kH264CodecName
    };

    std::vector<webrtc::SdpVideoFormat> vp9Formats;
    std::vector<webrtc::SdpVideoFormat> h264Formats;

    for (const auto &format : formats) {
        if (std::find(filterCodecNames.begin(), filterCodecNames.end(), format.name) == filterCodecNames.end()) {
            continue;
        }

        if (format.name == webrtc::kVp9CodecName) {
            vp9Formats.push_back(format);
        } else if (format.name == webrtc::kH264CodecName) {
            h264Formats.push_back(format);
        } else {
            filteredFormats.push_back(format);
        }
    }

    if (!vp9Formats.empty()) {
        bool added = false;
        for (const auto &format : vp9Formats) {
            if (added) {
                break;
            }
            for (const auto &parameter : format.parameters) {
                if (parameter.first == "profile-id") {
                    if (parameter.second == "0") {
                        filteredFormats.push_back(format);
                        added = true;
                        break;
                    }
                }
            }
        }

        if (!added) {
            filteredFormats.push_back(vp9Formats[0]);
        }
    }

    if (!h264Formats.empty()) {
        std::sort(h264Formats.begin(), h264Formats.end(), [](const webrtc::SdpVideoFormat &lhs, const webrtc::SdpVideoFormat &rhs) {
            auto lhsParameters = parseH264FormatParameters(lhs);
            auto rhsParameters = parseH264FormatParameters(rhs);

            int lhsLevelIdPriority = getH264ProfileLevelIdPriority(lhsParameters.profileLevelId);
            int lhsPacketizationModePriority = getH264PacketizationModePriority(lhsParameters.packetizationMode);
            int lhsLevelAssymetryAllowedPriority = getH264LevelAssymetryAllowedPriority(lhsParameters.levelAssymetryAllowed);

            int rhsLevelIdPriority = getH264ProfileLevelIdPriority(rhsParameters.profileLevelId);
            int rhsPacketizationModePriority = getH264PacketizationModePriority(rhsParameters.packetizationMode);
            int rhsLevelAssymetryAllowedPriority = getH264LevelAssymetryAllowedPriority(rhsParameters.levelAssymetryAllowed);

            if (lhsLevelIdPriority != rhsLevelIdPriority) {
                return lhsLevelIdPriority < rhsLevelIdPriority;
            }
            if (lhsPacketizationModePriority != rhsPacketizationModePriority) {
                return lhsPacketizationModePriority < rhsPacketizationModePriority;
            }
            if (lhsLevelAssymetryAllowedPriority != rhsLevelAssymetryAllowedPriority) {
                return lhsLevelAssymetryAllowedPriority < rhsLevelAssymetryAllowedPriority;
            }

            return false;
        });

        filteredFormats.push_back(h264Formats[0]);
    }

    return filteredFormats;
}

namespace {

static std::vector<GroupVideoPayloadType> assignLegacyGroupVideoPayloadTypes(std::vector<webrtc::SdpVideoFormat> const &formats) {
    if (formats.empty()) {
        return {};
    }

    constexpr int kFirstDynamicPayloadType = 100;
    constexpr int kLastDynamicPayloadType = 127;

    int payload_type = kFirstDynamicPayloadType;

    std::vector<GroupVideoPayloadType> result;

    std::vector<std::string> filterCodecNames = {
        webrtc::kVp8CodecName,
        webrtc::kVp9CodecName,
        webrtc::kH264CodecName,
    };

    for (const auto &codecName : filterCodecNames) {
        for (const auto &format : formats) {
            if (format.name != codecName) {
                continue;
            }

            webrtc::Codec codec = webrtc::CreateVideoCodec(format);
            codec.id = payload_type;
            addDefaultFeedbackParams(&codec);

            // Increment payload type.
            ++payload_type;
            if (payload_type > kLastDynamicPayloadType) {
                RTC_LOG(LS_ERROR) << "Out of dynamic payload types, skipping the rest.";
                break;
            }

            std::optional<webrtc::Codec> rtxCodec;
            // Add associated RTX codec for non-FEC codecs.
            if (!absl::EqualsIgnoreCase(codec.name, webrtc::kUlpfecCodecName) &&
                !absl::EqualsIgnoreCase(codec.name, webrtc::kFlexfecCodecName)) {
                rtxCodec = webrtc::CreateVideoRtxCodec(payload_type, codec.id);

                // Increment payload type.
                ++payload_type;
                if (payload_type > kLastDynamicPayloadType) {
                    RTC_LOG(LS_ERROR) << "Out of dynamic payload types, skipping the rest.";
                    break;
                }
            }

            result.push_back(GroupVideoPayloadType{codec, rtxCodec});
        }
    }

    return result;
}

} // namespace

webrtc::SdpVideoFormat groupAv1VideoFormat() {
    return webrtc::SdpVideoFormat(webrtc::kAv1CodecName, {
        {"profile", "0"},
        {"level-idx", "5"},
        {"tier", "0"},
    });
}

std::vector<GroupVideoPayloadType> assignGroupVideoPayloadTypes(std::vector<webrtc::SdpVideoFormat> const &formats) {
    std::vector<GroupVideoPayloadType> result = assignLegacyGroupVideoPayloadTypes(formats);
#if defined(TGCALLS_ENABLE_AV1) && TGCALLS_ENABLE_AV1
    webrtc::Codec av1 = webrtc::CreateVideoCodec(groupAv1VideoFormat());
    av1.id = kGroupAv1PayloadType;
    // A sender with AV1 on two layers sets a scalability mode on each encoding (else WebRTC sends AV1 as one stream):
    // L1T3 (SvtAv1Encoder's three temporal layers), and SetRtpSendParameters accepts only a mode the send codec lists.
    av1.scalability_modes.push_back(webrtc::ScalabilityMode::kL1T1);
    av1.scalability_modes.push_back(webrtc::ScalabilityMode::kL1T3);
    addDefaultFeedbackParams(&av1);
    result.push_back(GroupVideoPayloadType{av1, webrtc::CreateVideoRtxCodec(kGroupAv1RtxPayloadType, kGroupAv1PayloadType)});
#endif
    return result;
}

bool groupPayloadTypesListAv1(std::vector<GroupJoinPayloadVideoPayloadType> const &serverPayloadTypes) {
    for (const auto &payloadType : serverPayloadTypes) {
        if (absl::EqualsIgnoreCase(payloadType.name, webrtc::kAv1CodecName)) {
            return true;
        }
    }
    return false;
}

uint32_t groupDependencyDescriptorExtensionId(std::vector<std::pair<uint32_t, std::string>> const &serverExtensionMap) {
    for (const auto &extension : serverExtensionMap) {
        if (extension.second == kGroupDependencyDescriptorUri) {
            return extension.first;
        }
    }
    for (const auto &extension : serverExtensionMap) {
        if (extension.first == kGroupDependencyDescriptorDefaultExtensionId) {
            return 0;
        }
    }
    return kGroupDependencyDescriptorDefaultExtensionId;
}

std::vector<webrtc::RtpExtension> groupReferenceVideoHeaderExtensions(std::vector<webrtc::RtpExtension> const &extensions, uint32_t dependencyDescriptorId) {
    std::vector<webrtc::RtpExtension> result;
    bool idTaken = false;
    for (const auto &extension : extensions) {
        if (extension.uri == webrtc::RtpExtension::kRidUri
            || extension.uri == webrtc::RtpExtension::kRepairedRidUri
            || extension.uri == kGroupDependencyDescriptorUri) {
            continue;
        }
        if (dependencyDescriptorId != 0 && extension.id.value() == (int)dependencyDescriptorId) {
            idTaken = true;
        }
        result.push_back(extension);
    }
    if (dependencyDescriptorId != 0 && !idTaken) {
        result.emplace_back(kGroupDependencyDescriptorUri, webrtc::RtpHeaderExtensionId((int)dependencyDescriptorId));
    }
    return result;
}

bool setGroupAv1TopLayerCodecs(webrtc::RtpParameters &parameters, bool av1Top) {
    if (parameters.encodings.size() < 2) {
        return false;
    }
    const webrtc::RtpCodecParameters *h264 = nullptr;
    const webrtc::RtpCodecParameters *av1 = nullptr;
    for (const auto &codec : parameters.codecs) {
        if (!h264 && absl::EqualsIgnoreCase(codec.name, webrtc::kH264CodecName)) {
            h264 = &codec;
        } else if (!av1 && absl::EqualsIgnoreCase(codec.name, webrtc::kAv1CodecName)) {
            av1 = &codec;
        }
    }
    if (!h264 || (av1Top && !av1)) {
        return false;
    }
    const webrtc::RtpCodec lower = *h264;
    const webrtc::RtpCodec top = av1Top ? webrtc::RtpCodec(*av1) : webrtc::RtpCodec(*h264);
    for (size_t i = 0; i < parameters.encodings.size(); i++) {
        parameters.encodings[i].codec = i + 1 < parameters.encodings.size() ? lower : top;
    }
    return true;
}

} // namespace tgcalls
