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

// `formats` with any AV1 entry moved to a single bare "AV1" at the end.
std::vector<webrtc::SdpVideoFormat> WithAv1Last(std::vector<webrtc::SdpVideoFormat> formats) {
    std::vector<webrtc::SdpVideoFormat> result;
    for (auto &format : formats) {
        if (!IsAv1(format)) {
            result.push_back(std::move(format));
        }
    }
    result.push_back(webrtc::SdpVideoFormat(cricket::kAv1CodecName));
    return result;
}

std::vector<webrtc::SdpVideoFormat> WithSdpDefaults(std::vector<webrtc::SdpVideoFormat> formats) {
    for (auto &format : formats) {
        format = WithSdpDefaultParameters(std::move(format));
    }
    return formats;
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
        return WithSdpDefaults(WithAv1Last(_inner->GetSupportedFormats()));
    }

    std::vector<webrtc::SdpVideoFormat> GetImplementations() const override {
        return WithSdpDefaults(WithAv1Last(_inner->GetImplementations()));
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

// Lists formats as they are, AV1 bare, as Darwin's ObjC decoder factory does; the SDP defaults are added above it.
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
        return _inner->QueryCodecSupport(format, referenceScaling);
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment &env,
                                                 const webrtc::SdpVideoFormat &format) override {
        if (IsAv1(format)) {
            return webrtc::CreateDav1dDecoder();
        }
        return _inner->Create(env, format);
    }

    std::unique_ptr<webrtc::VideoDecoder> CreateVideoDecoder(const webrtc::SdpVideoFormat &format) override {
        if (IsAv1(format)) {
            return webrtc::CreateDav1dDecoder();
        }
        return _inner->CreateVideoDecoder(format);
    }

private:
    std::unique_ptr<webrtc::VideoDecoderFactory> _inner;
};

class SdpDefaultsDecoderFactory : public webrtc::VideoDecoderFactory {
public:
    explicit SdpDefaultsDecoderFactory(std::unique_ptr<webrtc::VideoDecoderFactory> inner) : _inner(std::move(inner)) {}

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return WithSdpDefaults(_inner->GetSupportedFormats());
    }

    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat &format, bool referenceScaling) const override {
        return _inner->QueryCodecSupport(InnerFormat(_inner->GetSupportedFormats(), format), referenceScaling);
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment &env,
                                                 const webrtc::SdpVideoFormat &format) override {
        return _inner->Create(env, InnerFormat(_inner->GetSupportedFormats(), format));
    }

    std::unique_ptr<webrtc::VideoDecoder> CreateVideoDecoder(const webrtc::SdpVideoFormat &format) override {
        return _inner->CreateVideoDecoder(InnerFormat(_inner->GetSupportedFormats(), format));
    }

private:
    std::unique_ptr<webrtc::VideoDecoderFactory> _inner;
};

#if TGCALLS_ALLOW_TEST_KNOBS
webrtc::SdpVideoFormat WithH264Level(webrtc::SdpVideoFormat format, const std::string &levelHex) {
    if (absl::EqualsIgnoreCase(format.name, cricket::kH264CodecName)) {
        auto it = format.parameters.find(cricket::kH264FmtpProfileLevelId);
        if (it != format.parameters.end() && it->second.size() == 6 && levelHex.size() == 2) {
            it->second = it->second.substr(0, 4) + levelHex;
        }
    }
    return format;
}

std::vector<webrtc::SdpVideoFormat> WithH264Level(std::vector<webrtc::SdpVideoFormat> formats, const std::string &levelHex) {
    for (auto &format : formats) {
        format = WithH264Level(std::move(format), levelHex);
    }
    return formats;
}

webrtc::SdpVideoFormat InnerH264LevelFormat(const std::vector<webrtc::SdpVideoFormat> &innerFormats,
                                            const webrtc::SdpVideoFormat &format, const std::string &levelHex) {
    for (const auto &innerFormat : innerFormats) {
        if (absl::EqualsIgnoreCase(innerFormat.name, format.name) &&
            WithH264Level(innerFormat, levelHex).parameters == format.parameters) {
            return innerFormat;
        }
    }
    return format;
}

class H264LevelEncoderFactory : public webrtc::VideoEncoderFactory {
public:
    H264LevelEncoderFactory(std::unique_ptr<webrtc::VideoEncoderFactory> inner, std::string levelHex)
        : _inner(std::move(inner)), _levelHex(std::move(levelHex)) {}

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return WithH264Level(_inner->GetSupportedFormats(), _levelHex);
    }

    std::vector<webrtc::SdpVideoFormat> GetImplementations() const override {
        return WithH264Level(_inner->GetImplementations(), _levelHex);
    }

    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat &format,
                                   absl::optional<std::string> scalabilityMode) const override {
        return _inner->QueryCodecSupport(InnerH264LevelFormat(_inner->GetSupportedFormats(), format, _levelHex),
                                         scalabilityMode);
    }

    std::unique_ptr<webrtc::VideoEncoder> CreateVideoEncoder(const webrtc::SdpVideoFormat &format) override {
        return _inner->CreateVideoEncoder(InnerH264LevelFormat(_inner->GetSupportedFormats(), format, _levelHex));
    }

    std::unique_ptr<EncoderSelectorInterface> GetEncoderSelector() const override {
        return _inner->GetEncoderSelector();
    }

private:
    std::unique_ptr<webrtc::VideoEncoderFactory> _inner;
    std::string _levelHex;
};

class H264LevelDecoderFactory : public webrtc::VideoDecoderFactory {
public:
    H264LevelDecoderFactory(std::unique_ptr<webrtc::VideoDecoderFactory> inner, std::string levelHex)
        : _inner(std::move(inner)), _levelHex(std::move(levelHex)) {}

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return WithH264Level(_inner->GetSupportedFormats(), _levelHex);
    }

    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat &format, bool referenceScaling) const override {
        return _inner->QueryCodecSupport(InnerH264LevelFormat(_inner->GetSupportedFormats(), format, _levelHex),
                                         referenceScaling);
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment &env,
                                                 const webrtc::SdpVideoFormat &format) override {
        return _inner->Create(env, InnerH264LevelFormat(_inner->GetSupportedFormats(), format, _levelHex));
    }

    std::unique_ptr<webrtc::VideoDecoder> CreateVideoDecoder(const webrtc::SdpVideoFormat &format) override {
        return _inner->CreateVideoDecoder(InnerH264LevelFormat(_inner->GetSupportedFormats(), format, _levelHex));
    }

private:
    std::unique_ptr<webrtc::VideoDecoderFactory> _inner;
    std::string _levelHex;
};
#endif

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

#if TGCALLS_ALLOW_TEST_KNOBS
std::unique_ptr<webrtc::VideoEncoderFactory> WrapEncoderFactoryWithH264LevelForTesting(
    std::unique_ptr<webrtc::VideoEncoderFactory> inner, std::string levelHex) {
    return std::make_unique<H264LevelEncoderFactory>(std::move(inner), std::move(levelHex));
}

std::unique_ptr<webrtc::VideoDecoderFactory> WrapDecoderFactoryWithH264LevelForTesting(
    std::unique_ptr<webrtc::VideoDecoderFactory> inner, std::string levelHex) {
    return std::make_unique<H264LevelDecoderFactory>(std::move(inner), std::move(levelHex));
}
#endif

std::unique_ptr<webrtc::VideoDecoderFactory> WrapDecoderFactoryWithSdpDefaults(std::unique_ptr<webrtc::VideoDecoderFactory> inner) {
return std::make_unique<SdpDefaultsDecoderFactory>(std::move(inner));
}

} // namespace tgcalls
