#include "av1/Av1TopStreamEncoder.h"

#include "api/video_codecs/video_codec.h"
#include "common_video/framerate_controller.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

#include <optional>

#include <utility>
#include <vector>

namespace tgcalls {

namespace {

class Av1TopStreamEncoder final : public webrtc::VideoEncoder {
public:
    Av1TopStreamEncoder(std::unique_ptr<webrtc::VideoEncoder> av1, std::function<void()> onFailure) :
    _av1(std::move(av1)),
    _onFailure(std::move(onFailure)),
    _av1Framerate(kAv1TopStreamMaxFramerate) {
    }

    void SetFecControllerOverride(webrtc::FecControllerOverride *fecControllerOverride) override {
        _av1->SetFecControllerOverride(fecControllerOverride);
    }

    int32_t InitEncode(const webrtc::VideoCodec *codecSettings, const Settings &settings) override {
        if (!codecSettings) {
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        if (_failed) {
            return WEBRTC_VIDEO_CODEC_OK;
        }
        webrtc::VideoCodec av1Settings = *codecSettings;
        av1Settings.codecType = webrtc::kVideoCodecAV1;
        if (av1Settings.maxFramerate == 0 || av1Settings.maxFramerate > (uint32_t)kAv1TopStreamMaxFramerate) {
            av1Settings.maxFramerate = kAv1TopStreamMaxFramerate;
        }
        // Reset() also lifts the limit (to max double): set it again.
        _av1Framerate.Reset();
        _av1Framerate.SetMaxFramerate(kAv1TopStreamMaxFramerate);
        const int32_t result = _av1->InitEncode(&av1Settings, settings);
        if (result != WEBRTC_VIDEO_CODEC_OK) {
            RTC_LOG(LS_WARNING) << "Av1TopStreamEncoder: AV1 InitEncode failed (" << result << ")";
            return fail(result);
        }
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback *callback) override {
        return _av1->RegisterEncodeCompleteCallback(callback);
    }

    int32_t Release() override {
        // A failure released AV1 already, and the stream never re-inits it (InitEncode returns at once).
        if (_failed) {
            return WEBRTC_VIDEO_CODEC_OK;
        }
        return _av1->Release();
    }

    int32_t Encode(const webrtc::VideoFrame &frame, const std::vector<webrtc::VideoFrameType> *frameTypes) override {
        if (_failed) {
            return WEBRTC_VIDEO_CODEC_OK;
        }
        bool keyFrameRequested = false;
        if (frameTypes) {
            for (const auto type : *frameTypes) {
                if (type == webrtc::VideoFrameType::kVideoFrameKey) {
                    keyFrameRequested = true;
                }
            }
        }
        const int64_t timestampNs = frame.timestamp_us() * 1000;
        if (keyFrameRequested) {
            _av1Framerate.KeepFrame(timestampNs);
        } else if (_av1Framerate.ShouldDropFrame(timestampNs)) {
            return WEBRTC_VIDEO_CODEC_OK;
        }
        const int32_t result = _av1->Encode(frame, frameTypes);
        if (result >= 0) {
            return result;
        }
        RTC_LOG(LS_WARNING) << "Av1TopStreamEncoder: AV1 Encode failed (" << result << ")";
        return fail(result);
    }

    void SetRates(const RateControlParameters &parameters) override {
        if (_failed) {
            return;
        }
        RateControlParameters av1Parameters = parameters;
        if (av1Parameters.framerate_fps > kAv1TopStreamMaxFramerate) {
            av1Parameters.framerate_fps = kAv1TopStreamMaxFramerate;
        }
        _av1->SetRates(av1Parameters);
    }

    void OnPacketLossRateUpdate(float packetLossRate) override {
        _av1->OnPacketLossRateUpdate(packetLossRate);
    }

    void OnRttUpdate(int64_t rttMs) override {
        _av1->OnRttUpdate(rttMs);
    }

    void OnLossNotification(const LossNotification &lossNotification) override {
        _av1->OnLossNotification(lossNotification);
    }

    EncoderInfo GetEncoderInfo() const override {
        return _av1->GetEncoderInfo();
    }

private:
    // Without `_onFailure` the error goes to WebRTC; with it the stream stops for good and the engine is told.
    int32_t fail(int32_t error) {
        if (!_onFailure) {
            RTC_LOG(LS_WARNING) << "Av1TopStreamEncoder: AV1 failed, reported to WebRTC";
            return error;
        }
        _av1->Release();
        _failed = true;
        RTC_LOG(LS_WARNING) << "Av1TopStreamEncoder: AV1 failed; the stream stops until it is configured for H264";
        _onFailure();
        return WEBRTC_VIDEO_CODEC_OK;
    }

    std::unique_ptr<webrtc::VideoEncoder> _av1;
    std::function<void()> _onFailure;
    bool _failed = false;
    webrtc::FramerateController _av1Framerate;
};

} // namespace

std::unique_ptr<webrtc::VideoEncoder> CreateAv1TopStreamEncoder(
    std::unique_ptr<webrtc::VideoEncoder> av1,
    std::function<void()> onFailure) {
    return std::make_unique<Av1TopStreamEncoder>(std::move(av1), std::move(onFailure));
}

} // namespace tgcalls
