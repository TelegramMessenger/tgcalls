#include "av1/Av1VideoCodecFactories.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "api/video_codecs/sdp_video_format.h"
#include "media/base/media_constants.h"
#include "modules/video_coding/codecs/av1/dav1d_decoder.h"

#include "av1/SvtAv1Encoder.h"

namespace tgcalls {

namespace {

bool IsAv1(const webrtc::SdpVideoFormat &format) {
    return absl::EqualsIgnoreCase(format.name, cricket::kAv1CodecName);
}

std::vector<webrtc::SdpVideoFormat> WithAv1Last(std::vector<webrtc::SdpVideoFormat> formats) {
    std::vector<webrtc::SdpVideoFormat> result;
    for (auto &format : formats) {
        if (!IsAv1(format)) {
            result.push_back(WithSdpDefaultParameters(std::move(format)));
        }
    }
    result.push_back(WithSdpDefaultParameters(webrtc::SdpVideoFormat(cricket::kAv1CodecName)));
    return result;
}

// The inner factory's own entry for a format this wrapper listed with the SDP defaults; `format` itself when none is.
webrtc::SdpVideoFormat InnerFormat(const std::vector<webrtc::SdpVideoFormat> &innerFormats,
                                   const webrtc::SdpVideoFormat &format) {
    for (const auto &innerFormat : innerFormats) {
        if (absl::EqualsIgnoreCase(innerFormat.name, format.name) &&
            WithSdpDefaultParameters(innerFormat).parameters == format.parameters) {
            return innerFormat;
        }
    }
    return format;
}

class Av1EncoderFactory : public webrtc::VideoEncoderFactory {
public:
    Av1EncoderFactory(std::unique_ptr<webrtc::VideoEncoderFactory> inner, bool allowAv1Encoding)
        : _inner(std::move(inner)), _allowAv1Encoding(allowAv1Encoding) {}

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return WithAv1Last(_inner->GetSupportedFormats());
    }

    std::vector<webrtc::SdpVideoFormat> GetImplementations() const override {
        return WithAv1Last(_inner->GetImplementations());
    }

    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat &format,
                                   absl::optional<std::string> scalabilityMode) const override {
        if (IsAv1(format)) {
            CodecSupport support;
            support.is_supported = !scalabilityMode || *scalabilityMode == "L1T1";
            return support;
        }
        return _inner->QueryCodecSupport(InnerFormat(_inner->GetSupportedFormats(), format), scalabilityMode);
    }

    std::unique_ptr<webrtc::VideoEncoder> CreateVideoEncoder(const webrtc::SdpVideoFormat &format) override {
        if (IsAv1(format)) {
            return _allowAv1Encoding ? CreateSvtAv1Encoder() : CreateRefusingAv1Encoder();
        }
        return _inner->CreateVideoEncoder(InnerFormat(_inner->GetSupportedFormats(), format));
    }

    std::unique_ptr<EncoderSelectorInterface> GetEncoderSelector() const override {
        return _inner->GetEncoderSelector();
    }

private:
    std::unique_ptr<webrtc::VideoEncoderFactory> _inner;
    bool _allowAv1Encoding;
};

class Dav1dDecoderFactory : public webrtc::VideoDecoderFactory {
public:
    explicit Dav1dDecoderFactory(std::unique_ptr<webrtc::VideoDecoderFactory> inner) : _inner(std::move(inner)) {}

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return WithAv1Last(_inner->GetSupportedFormats());
    }

    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat &format, bool referenceScaling) const override {
        if (IsAv1(format)) {
            CodecSupport support;
            support.is_supported = !referenceScaling;
            return support;
        }
        return _inner->QueryCodecSupport(InnerFormat(_inner->GetSupportedFormats(), format), referenceScaling);
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment &env,
                                                 const webrtc::SdpVideoFormat &format) override {
        if (IsAv1(format)) {
            return webrtc::CreateDav1dDecoder();
        }
        return _inner->Create(env, InnerFormat(_inner->GetSupportedFormats(), format));
    }

    std::unique_ptr<webrtc::VideoDecoder> CreateVideoDecoder(const webrtc::SdpVideoFormat &format) override {
        if (IsAv1(format)) {
            return webrtc::CreateDav1dDecoder();
        }
        return _inner->CreateVideoDecoder(InnerFormat(_inner->GetSupportedFormats(), format));
    }

private:
    std::unique_ptr<webrtc::VideoDecoderFactory> _inner;
};

} // namespace

webrtc::SdpVideoFormat WithSdpDefaultParameters(webrtc::SdpVideoFormat format) {
    // emplace keeps a value that is already there.
    auto &parameters = format.parameters;
    if (absl::EqualsIgnoreCase(format.name, cricket::kVp9CodecName)) {
        parameters.emplace(cricket::kVP9ProfileId, "0");
    } else if (absl::EqualsIgnoreCase(format.name, cricket::kH264CodecName)) {
        parameters.emplace(cricket::kH264FmtpPacketizationMode, "0");
    } else if (IsAv1(format)) {
        parameters.emplace(cricket::kAv1FmtpProfile, "0");
        parameters.emplace(cricket::kAv1FmtpLevelIdx, "5");
        parameters.emplace(cricket::kAv1FmtpTier, "0");
    } else if (absl::EqualsIgnoreCase(format.name, cricket::kH265CodecName)) {
        parameters.emplace(cricket::kH265FmtpLevelId, "93");
        parameters.emplace(cricket::kH265FmtpTxMode, "SRST");
    }
    return format;
}

std::unique_ptr<webrtc::VideoEncoderFactory> WrapEncoderFactoryWithAv1(std::unique_ptr<webrtc::VideoEncoderFactory> inner,
                                                                       bool allowAv1Encoding) {
    return std::make_unique<Av1EncoderFactory>(std::move(inner), allowAv1Encoding);
}

std::unique_ptr<webrtc::VideoDecoderFactory> WrapDecoderFactoryWithDav1d(std::unique_ptr<webrtc::VideoDecoderFactory> inner) {
    return std::make_unique<Dav1dDecoderFactory>(std::move(inner));
}

} // namespace tgcalls
