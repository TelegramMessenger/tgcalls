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
            result.push_back(std::move(format));
        }
    }
    result.push_back(webrtc::SdpVideoFormat(cricket::kAv1CodecName));
    return result;
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
        return _inner->QueryCodecSupport(format, scalabilityMode);
    }

    std::unique_ptr<webrtc::VideoEncoder> CreateVideoEncoder(const webrtc::SdpVideoFormat &format) override {
        if (IsAv1(format)) {
            return _allowAv1Encoding ? CreateSvtAv1Encoder() : CreateRefusingAv1Encoder();
        }
        return _inner->CreateVideoEncoder(format);
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

} // namespace

std::unique_ptr<webrtc::VideoEncoderFactory> WrapEncoderFactoryWithAv1(std::unique_ptr<webrtc::VideoEncoderFactory> inner,
                                                                       bool allowAv1Encoding) {
    return std::make_unique<Av1EncoderFactory>(std::move(inner), allowAv1Encoding);
}

std::unique_ptr<webrtc::VideoDecoderFactory> WrapDecoderFactoryWithDav1d(std::unique_ptr<webrtc::VideoDecoderFactory> inner) {
    return std::make_unique<Dav1dDecoderFactory>(std::move(inner));
}

} // namespace tgcalls
