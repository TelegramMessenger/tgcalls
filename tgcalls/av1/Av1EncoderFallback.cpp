#include "av1/Av1EncoderFallback.h"

#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "api/video_codecs/video_encoder.h"
#include "media/base/media_constants.h"
#include "modules/video_coding/include/video_error_codes.h"

namespace tgcalls {

namespace {

bool IsAv1(const webrtc::SdpVideoFormat &format) {
    return absl::EqualsIgnoreCase(format.name, cricket::kAv1CodecName);
}

// An AV1 encoder that reports failing to start or to encode; otherwise `inner` itself.
class ReportingAv1Encoder final : public webrtc::VideoEncoder {
public:
    ReportingAv1Encoder(std::unique_ptr<webrtc::VideoEncoder> inner, std::shared_ptr<Av1EncoderFallback> fallback)
        : _inner(std::move(inner)), _fallback(std::move(fallback)) {}

    void SetFecControllerOverride(webrtc::FecControllerOverride *fecControllerOverride) override {
        _inner->SetFecControllerOverride(fecControllerOverride);
    }

    int InitEncode(const webrtc::VideoCodec *codecSettings, const Settings &settings) override {
        const int result = _inner->InitEncode(codecSettings, settings);
        if (result != WEBRTC_VIDEO_CODEC_OK) {
            _fallback->reportAv1Failure();
        }
        return result;
    }

    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback *callback) override {
        return _inner->RegisterEncodeCompleteCallback(callback);
    }

    int32_t Release() override {
        return _inner->Release();
    }

    int32_t Encode(const webrtc::VideoFrame &frame, const std::vector<webrtc::VideoFrameType> *frameTypes) override {
        const int32_t result = _inner->Encode(frame, frameTypes);
        // VideoStreamEncoder replaces the encoder on any negative result.
        if (result < 0) {
            _fallback->reportAv1Failure();
        }
        return result;
    }

    void SetRates(const RateControlParameters &parameters) override {
        _inner->SetRates(parameters);
    }

    void OnPacketLossRateUpdate(float packetLossRate) override {
        _inner->OnPacketLossRateUpdate(packetLossRate);
    }

    void OnRttUpdate(int64_t rttMs) override {
        _inner->OnRttUpdate(rttMs);
    }

    void OnLossNotification(const LossNotification &lossNotification) override {
        _inner->OnLossNotification(lossNotification);
    }

    EncoderInfo GetEncoderInfo() const override {
        return _inner->GetEncoderInfo();
    }

private:
    std::unique_ptr<webrtc::VideoEncoder> _inner;
    std::shared_ptr<Av1EncoderFallback> _fallback;
};

// Names the fallback when the encoder that broke is AV1; any other encoder is the inner selector's (or WebRTC's VP8).
class Av1FallbackEncoderSelector final : public webrtc::VideoEncoderFactory::EncoderSelectorInterface {
public:
    Av1FallbackEncoderSelector(std::unique_ptr<EncoderSelectorInterface> inner, std::shared_ptr<Av1EncoderFallback> fallback)
        : _inner(std::move(inner)), _fallback(std::move(fallback)) {}

    void OnCurrentEncoder(const webrtc::SdpVideoFormat &format) override {
        if (_inner) {
            _inner->OnCurrentEncoder(format);
        }
    }

    absl::optional<webrtc::SdpVideoFormat> OnAvailableBitrate(const webrtc::DataRate &rate) override {
        return _inner ? _inner->OnAvailableBitrate(rate) : absl::nullopt;
    }

    absl::optional<webrtc::SdpVideoFormat> OnResolutionChange(const webrtc::RenderResolution &resolution) override {
        return _inner ? _inner->OnResolutionChange(resolution) : absl::nullopt;
    }

    absl::optional<webrtc::SdpVideoFormat> OnEncoderBroken() override {
        // The last encoder the factory was asked for: WebRTC does not call OnCurrentEncoder for one it could not create.
        if (_fallback->lastCreatedIsAv1()) {
            if (const auto format = _fallback->fallbackFormat()) {
                return *format;
            }
            return absl::nullopt;
        }
        return _inner ? _inner->OnEncoderBroken() : absl::nullopt;
    }

private:
    std::unique_ptr<EncoderSelectorInterface> _inner;
    std::shared_ptr<Av1EncoderFallback> _fallback;
};

class Av1FallbackEncoderFactory final : public webrtc::VideoEncoderFactory {
public:
    Av1FallbackEncoderFactory(std::unique_ptr<webrtc::VideoEncoderFactory> inner, std::shared_ptr<Av1EncoderFallback> fallback)
        : _inner(std::move(inner)), _fallback(std::move(fallback)) {}

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return _inner->GetSupportedFormats();
    }

    std::vector<webrtc::SdpVideoFormat> GetImplementations() const override {
        return _inner->GetImplementations();
    }

    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat &format,
                                   absl::optional<std::string> scalabilityMode) const override {
        return _inner->QueryCodecSupport(format, scalabilityMode);
    }

    std::unique_ptr<webrtc::VideoEncoder> CreateVideoEncoder(const webrtc::SdpVideoFormat &format) override {
        const bool isAv1 = IsAv1(format);
        _fallback->noteCreated(isAv1);
        std::unique_ptr<webrtc::VideoEncoder> encoder = _inner->CreateVideoEncoder(format);
        if (!isAv1) {
            return encoder;
        }
        if (!encoder) {
            _fallback->reportAv1Failure();
            return nullptr;
        }
        return std::make_unique<ReportingAv1Encoder>(std::move(encoder), _fallback);
    }

    std::unique_ptr<EncoderSelectorInterface> GetEncoderSelector() const override {
        return std::make_unique<Av1FallbackEncoderSelector>(_inner->GetEncoderSelector(), _fallback);
    }

private:
    std::unique_ptr<webrtc::VideoEncoderFactory> _inner;
    std::shared_ptr<Av1EncoderFallback> _fallback;
};

} // namespace

Av1EncoderFallback::Av1EncoderFallback(std::function<void()> onAv1EncoderFailed)
    : _onAv1EncoderFailed(std::move(onAv1EncoderFailed)) {}

void Av1EncoderFallback::setFallbackFormat(std::optional<webrtc::SdpVideoFormat> format) {
    std::lock_guard<std::mutex> lock(_mutex);
    _fallbackFormat = std::move(format);
}

void Av1EncoderFallback::reportAv1Failure() {
    if (_onAv1EncoderFailed) {
        _onAv1EncoderFailed();
    }
}

std::optional<webrtc::SdpVideoFormat> Av1EncoderFallback::fallbackFormat() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _fallbackFormat;
}

void Av1EncoderFallback::noteCreated(bool isAv1) {
    std::lock_guard<std::mutex> lock(_mutex);
    _lastCreatedIsAv1 = isAv1;
}

bool Av1EncoderFallback::lastCreatedIsAv1() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _lastCreatedIsAv1;
}

std::unique_ptr<webrtc::VideoEncoderFactory> WrapEncoderFactoryWithAv1Fallback(
    std::unique_ptr<webrtc::VideoEncoderFactory> inner, std::shared_ptr<Av1EncoderFallback> fallback) {
    return std::make_unique<Av1FallbackEncoderFactory>(std::move(inner), std::move(fallback));
}

} // namespace tgcalls
