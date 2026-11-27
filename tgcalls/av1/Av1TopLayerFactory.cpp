#include "av1/Av1TopLayerFactory.h"

#include "av1/Av1TopStreamEncoder.h"
#include "av1/SvtAv1Encoder.h"
#include "group/GroupVideoPayloadTypes.h"

#include "absl/strings/match.h"
#include "api/video_codecs/scalability_mode.h"
#include "media/base/media_constants.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

#include <utility>
#include <vector>

namespace tgcalls {

void Av1TopLayerSwitch::setEnabled(bool enabled) {
    _enabled.store(enabled);
}

bool Av1TopLayerSwitch::enabled() const {
    return _enabled.load();
}

void Av1TopLayerSwitch::setLayers(GroupAv1ScreencastLayers layers) {
    _layers.store(layers);
}

GroupAv1ScreencastLayers Av1TopLayerSwitch::layers() const {
    return _layers.load();
}

void Av1TopLayerSwitch::setOnFallback(std::function<void()> onFallback) {
    std::lock_guard<std::mutex> lock(_mutex);
    _onFallback = std::move(onFallback);
}

void Av1TopLayerSwitch::reportFallback() {
    std::function<void()> onFallback;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        onFallback = _onFallback;
    }
    if (onFallback) {
        onFallback();
    }
}

void Av1TopLayerSwitch::setTestAv1Fails(bool fails) {
    _testAv1Fails.store(fails);
}

bool Av1TopLayerSwitch::testAv1Fails() const {
    return _testAv1Fails.load();
}

namespace {

// The CLI's failing AV1 encoder (Av1TopLayerSwitch::setTestAv1Fails).
class FailingTopStreamAv1Encoder final : public webrtc::VideoEncoder {
public:
    int32_t InitEncode(const webrtc::VideoCodec *, const Settings &) override {
        RTC_LOG(LS_WARNING) << "Av1TopLayer: test AV1 encoder fails to initialise";
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback *) override {
        return WEBRTC_VIDEO_CODEC_OK;
    }
    int32_t Release() override {
        return WEBRTC_VIDEO_CODEC_OK;
    }
    int32_t Encode(const webrtc::VideoFrame &, const std::vector<webrtc::VideoFrameType> *) override {
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    void SetRates(const RateControlParameters &) override {
    }
    EncoderInfo GetEncoderInfo() const override {
        EncoderInfo info;
        info.implementation_name = "FailingAv1";
        return info;
    }
};

// The H264 format the sender runs (else the factory's first H264), and whether its encoder is AV1 (Single), shared by
// the factory (which creates its encoders) and its encoder selector.
struct Av1TopLayerCurrentFormat {
    std::mutex mutex;
    std::optional<webrtc::SdpVideoFormat> h264;
    std::optional<webrtc::SdpVideoFormat> firstH264;
    bool isAv1 = false;

    void note(const webrtc::SdpVideoFormat &format) {
        std::lock_guard<std::mutex> lock(mutex);
        isAv1 = absl::EqualsIgnoreCase(format.name, webrtc::kAv1CodecName);
        if (absl::EqualsIgnoreCase(format.name, webrtc::kH264CodecName)) {
            h264 = format;
        }
    }

    std::optional<webrtc::SdpVideoFormat> get() {
        std::lock_guard<std::mutex> lock(mutex);
        return h264 ? h264 : firstH264;
    }

    bool currentIsAv1() {
        std::lock_guard<std::mutex> lock(mutex);
        return isAv1;
    }
};

// WebRTC asks the selector what to switch to when an encoder breaks (VideoStreamEncoder::RequestEncoderSwitch, on any
// Encode or InitEncode failure). Without an answer it tries VP8 and then drops the first negotiated codec; a sender
// with an AV1 top layer has negotiated [H264, AV1], so an H264 error would make the whole sender AV1, which this
// factory refuses, and the share would go dark. The answer is the H264 the sender runs ("already using this codec":
// nothing changes, as for a sender with H264 alone). A stream's AV1 failure in Top and Every never reaches WebRTC
// (Av1TopStreamEncoder); in Single the AV1 sender's does, and the answer is H264, negotiated after AV1 (the engine is
// told: Av1TopLayerSwitch::reportFallback).
class Av1TopLayerEncoderSelector final : public webrtc::VideoEncoderFactory::EncoderSelectorInterface {
public:
    Av1TopLayerEncoderSelector(std::unique_ptr<EncoderSelectorInterface> inner, std::shared_ptr<Av1TopLayerCurrentFormat> current, std::shared_ptr<Av1TopLayerSwitch> av1TopLayer) :
    _inner(std::move(inner)),
    _current(std::move(current)),
    _av1TopLayer(std::move(av1TopLayer)) {
    }

    void OnCurrentEncoder(const webrtc::SdpVideoFormat &format) override {
        _current->note(format);
        if (_inner) {
            _inner->OnCurrentEncoder(format);
        }
    }

    std::optional<webrtc::SdpVideoFormat> OnAvailableBitrate(const webrtc::DataRate &rate) override {
        return _inner ? _inner->OnAvailableBitrate(rate) : std::nullopt;
    }

    std::optional<webrtc::SdpVideoFormat> OnResolutionChange(const webrtc::RenderResolution &resolution) override {
        return _inner ? _inner->OnResolutionChange(resolution) : std::nullopt;
    }

    std::optional<webrtc::SdpVideoFormat> OnEncoderBroken() override {
        if (_current->currentIsAv1()) {
            RTC_LOG(LS_WARNING) << "Av1TopLayer: the AV1 sender broke; switching it to H264";
            if (_av1TopLayer) {
                _av1TopLayer->reportFallback();
            }
        }
        if (const auto h264 = _current->get()) {
            return *h264;
        }
        return _inner ? _inner->OnEncoderBroken() : std::nullopt;
    }

private:
    std::unique_ptr<EncoderSelectorInterface> _inner;
    std::shared_ptr<Av1TopLayerCurrentFormat> _current;
    std::shared_ptr<Av1TopLayerSwitch> _av1TopLayer;
};

void AddGroupAv1Format(std::vector<webrtc::SdpVideoFormat> &formats) {
    auto av1 = groupAv1VideoFormat();
    for (const auto &format : formats) {
        if (format.IsSameCodec(av1)) {
            return;
        }
    }
    // The modes QueryCodecSupport accepts (Every: L1T3 on each encoding). The engine's send codecs are built from these
    // formats, and PeerConnection's sender (GroupInstanceReferenceImpl) accepts only a scalability mode they list.
    av1.scalability_modes.push_back(webrtc::ScalabilityMode::kL1T1);
    av1.scalability_modes.push_back(webrtc::ScalabilityMode::kL1T3);
    formats.push_back(av1);
}

class Av1TopLayerEncoderFactory final : public webrtc::VideoEncoderFactory {
public:
    Av1TopLayerEncoderFactory(
        std::unique_ptr<webrtc::VideoEncoderFactory> inner,
        Av1TopLayerPlatform platform,
        std::shared_ptr<Av1TopLayerSwitch> av1TopLayer) :
    _inner(std::move(inner)),
    _platform(std::move(platform)),
    _av1TopLayer(std::move(av1TopLayer)) {
        for (const auto &format : _inner->GetSupportedFormats()) {
            if (absl::EqualsIgnoreCase(format.name, webrtc::kH264CodecName)) {
                _current->firstH264 = format;
                break;
            }
        }
    }

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        auto formats = _inner->GetSupportedFormats();
        AddGroupAv1Format(formats);
        return formats;
    }

    std::vector<webrtc::SdpVideoFormat> GetImplementations() const override {
        auto formats = _inner->GetImplementations();
        AddGroupAv1Format(formats);
        return formats;
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment &env, const webrtc::SdpVideoFormat &format) override {
        _current->note(format);
        const bool enabled = _av1TopLayer && _av1TopLayer->enabled();
        const auto layers = _av1TopLayer ? _av1TopLayer->layers() : GroupAv1ScreencastLayers::Top;
        if (absl::EqualsIgnoreCase(format.name, webrtc::kAv1CodecName)) {
            if (!enabled || layers == GroupAv1ScreencastLayers::Top) {
                // In Top AV1 is the top stream's encoder (below), never the whole sender.
                return CreateRefusingAv1Encoder();
            }
            // AV1 as the sender's codec: no fallback of its own, since each simulcast encoding sends the images of the
            // one codec it names (WebRTC's mixed-codec simulcast), so an AV1 stream cannot carry H264 images; a failure
            // goes to WebRTC, which switches the sender (the selector).
            const auto av1TopLayer = _av1TopLayer;
            const auto makeAv1 = [av1TopLayer]() -> std::unique_ptr<webrtc::VideoEncoder> {
                std::unique_ptr<webrtc::VideoEncoder> av1;
                if (av1TopLayer->testAv1Fails()) {
                    av1 = std::make_unique<FailingTopStreamAv1Encoder>();
                } else {
                    av1 = CreateSvtAv1Encoder();
                }
                return CreateAv1TopStreamEncoder(std::move(av1), nullptr);
            };
            if (layers == GroupAv1ScreencastLayers::Single) {
                RTC_LOG(LS_INFO) << "Av1TopLayer: the share is one AV1 stream";
                return makeAv1();
            }
            if (!_platform.makeSimulcastEncoder) {
                return CreateRefusingAv1Encoder();
            }
            // Every: WebRTC's AV1 simulcast (a scalability mode on each encoding), an AV1 encoder for every stream.
            RTC_LOG(LS_INFO) << "Av1TopLayer: the share is AV1 simulcast, an AV1 encoder per stream";
            return _platform.makeSimulcastEncoder(env, format, makeAv1, true);
        }
        if (absl::EqualsIgnoreCase(format.name, webrtc::kH264CodecName) && enabled && layers == GroupAv1ScreencastLayers::Top
            && _platform.makeSimulcastEncoder) {
            const auto av1TopLayer = _av1TopLayer;
            return _platform.makeSimulcastEncoder(env, format, [av1TopLayer]() {
                std::unique_ptr<webrtc::VideoEncoder> av1;
                if (av1TopLayer->testAv1Fails()) {
                    av1 = std::make_unique<FailingTopStreamAv1Encoder>();
                } else {
                    av1 = CreateSvtAv1Encoder();
                }
                RTC_LOG(LS_INFO) << "Av1TopLayer: the top stream is encoded with AV1";
                return CreateAv1TopStreamEncoder(std::move(av1), [av1TopLayer]() {
                    av1TopLayer->reportFallback();
                });
            }, false);
        }
        return _inner->Create(env, format);
    }

    // WebRTC replaces a scalability mode the factory does not support by its default, L1T2
    // (FallbackToDefaultScalabilityModeIfNotSupported); AV1 takes L1T1 and L1T3, what SvtAv1Encoder encodes (Every
    // sets L1T3 on each encoding). The rest is `inner`'s answer.
    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat &format, std::optional<std::string> scalabilityMode) const override {
        if (absl::EqualsIgnoreCase(format.name, webrtc::kAv1CodecName)) {
            CodecSupport support;
            support.is_supported = !scalabilityMode || *scalabilityMode == "L1T1" || *scalabilityMode == "L1T3";
            return support;
        }
        return _inner->QueryCodecSupport(format, scalabilityMode);
    }

    std::unique_ptr<EncoderSelectorInterface> GetEncoderSelector() const override {
        return std::make_unique<Av1TopLayerEncoderSelector>(_inner->GetEncoderSelector(), _current, _av1TopLayer);
    }

private:
    std::unique_ptr<webrtc::VideoEncoderFactory> _inner;
    Av1TopLayerPlatform _platform;
    std::shared_ptr<Av1TopLayerSwitch> _av1TopLayer;
    std::shared_ptr<Av1TopLayerCurrentFormat> _current = std::make_shared<Av1TopLayerCurrentFormat>();
};

} // namespace

std::unique_ptr<webrtc::VideoEncoderFactory> WrapScreencastEncoderFactoryWithAv1TopLayer(
    std::unique_ptr<webrtc::VideoEncoderFactory> inner,
    Av1TopLayerPlatform platform,
    std::shared_ptr<Av1TopLayerSwitch> av1TopLayer) {
    return std::make_unique<Av1TopLayerEncoderFactory>(std::move(inner), std::move(platform), std::move(av1TopLayer));
}

} // namespace tgcalls
