#include "av1/SvtAv1Encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

#include "api/scoped_refptr.h"
#include "api/video/encoded_image.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/scalability_mode.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "modules/video_coding/svc/create_scalability_structure.h"
#include "modules/video_coding/svc/scalable_video_controller.h"
#include "rtc_base/logging.h"

#include "av1/Av1Obu.h"

#include "svt-av1/EbSvtAv1Enc.h"

namespace tgcalls {

namespace {

// Starting values; the device check tunes them. SVT-AV1 enables presets up to M12 in real-time mode.
constexpr int8_t kPreset = 11;
// SVT-AV1 takes a parallelism level (1-6), not a thread count.
constexpr uint32_t kParallelismLevel = 3;
// Quality-scaling thresholds on the scale SVT-AV1 reports a picture's QP in (0-63).
constexpr int kLowQp = 30;
constexpr int kHighQp = 52;

uint32_t FrameRateToInteger(double framerate) {
    return uint32_t(std::max<long>(1, std::lround(framerate)));
}

bool HasSequenceHeader(const std::vector<uint8_t> &data) {
    for (const auto &obu : ParseAv1Obus(data.data(), data.size())) {
        if (obu.type == kAv1ObuSequenceHeader) {
            return true;
        }
    }
    return false;
}

class SvtAv1Encoder final : public webrtc::VideoEncoder {
public:
    ~SvtAv1Encoder() override {
        Release();
    }

    int32_t InitEncode(const webrtc::VideoCodec *codecSettings, const Settings &settings) override {
        if (!codecSettings || codecSettings->width < 1 || codecSettings->height < 1) {
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        Release();
        _codec = *codecSettings;
        _scalabilityMode = webrtc::ScalabilityMode::kL1T1;
        _svcController = webrtc::CreateScalabilityStructure(_scalabilityMode);
        if (!_svcController) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        _targetBitrateBps = std::max<uint32_t>(_codec.startBitrate, 50) * 1000;
        _framerate = _codec.maxFramerate > 0 ? double(_codec.maxFramerate) : 30.0;
        if (!createHandle(_codec.width, _codec.height)) {
            Release();
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback *callback) override {
        _callback = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override {
        destroyHandle();
        _svcController.reset();
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Encode(const webrtc::VideoFrame &frame, const std::vector<webrtc::VideoFrameType> *frameTypes) override {
        if (!_handle || !_callback || !_svcController) {
            return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
        }
        rtc::scoped_refptr<webrtc::I420BufferInterface> buffer = frame.video_frame_buffer()->ToI420();
        if (!buffer) {
            RTC_LOG(LS_ERROR) << "SvtAv1Encoder: cannot convert the frame to I420";
            return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
        }
        if (uint32_t(buffer->width()) != _width || uint32_t(buffer->height()) != _height) {
            // WebRTC calls InitEncode before a new size; a frame that arrives at another size anyway gets a fresh
            // encoder rather than a failed call.
            destroyHandle();
            if (!createHandle(uint32_t(buffer->width()), uint32_t(buffer->height()))) {
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
        }

        bool keyRequested = _needsKeyframe;
        if (frameTypes) {
            for (auto type : *frameTypes) {
                if (type == webrtc::VideoFrameType::kVideoFrameKey) {
                    keyRequested = true;
                }
            }
        }
        std::vector<webrtc::ScalableVideoController::LayerFrameConfig> layers = _svcController->NextFrameConfig(keyRequested);
        if (layers.empty()) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        const webrtc::ScalableVideoController::LayerFrameConfig &layer = layers.front();

        EbSvtIOFormat picture;
        std::memset(&picture, 0, sizeof(picture));
        picture.luma = const_cast<uint8_t *>(buffer->DataY());
        picture.cb = const_cast<uint8_t *>(buffer->DataU());
        picture.cr = const_cast<uint8_t *>(buffer->DataV());
        picture.y_stride = uint32_t(buffer->StrideY());
        picture.cb_stride = uint32_t(buffer->StrideU());
        picture.cr_stride = uint32_t(buffer->StrideV());

        EbBufferHeaderType input;
        std::memset(&input, 0, sizeof(input));
        input.size = sizeof(input);
        input.p_buffer = reinterpret_cast<uint8_t *>(&picture);
        input.n_filled_len = uint32_t(buffer->StrideY() * buffer->height() + (buffer->StrideU() + buffer->StrideV()) * buffer->ChromaHeight());
        input.pts = _pts++;
        input.pic_type = layer.IsKeyframe() ? EB_AV1_KEY_PICTURE : EB_AV1_INVALID_PICTURE;

        // Rate and frame-rate changes ride on this picture.
        SvtAv1RateInfo rateInfo;
        std::memset(&rateInfo, 0, sizeof(rateInfo));
        SvtAv1FrameRateInfo frameRateInfo;
        std::memset(&frameRateInfo, 0, sizeof(frameRateInfo));
        EbPrivDataNode rateNode;
        std::memset(&rateNode, 0, sizeof(rateNode));
        EbPrivDataNode frameRateNode;
        std::memset(&frameRateNode, 0, sizeof(frameRateNode));
        EbPrivDataNode *events = nullptr;
        if (_targetBitrateBps != _configuredBitrateBps) {
            rateInfo.target_bit_rate = _targetBitrateBps;
            rateNode.node_type = RATE_CHANGE_EVENT;
            rateNode.data = &rateInfo;
            rateNode.size = sizeof(rateInfo);
            rateNode.next = events;
            events = &rateNode;
        }
        // WebRTC's measured frame rate jitters by a frame or two; only a real change reaches the rate control.
        const uint32_t frameRate = FrameRateToInteger(_framerate);
        const bool frameRateChanged = std::abs(int(frameRate) - int(_configuredFrameRate)) * 10 > int(_configuredFrameRate);
        if (frameRateChanged) {
            frameRateInfo.frame_rate_numerator = frameRate;
            frameRateInfo.frame_rate_denominator = 1;
            frameRateNode.node_type = FRAME_RATE_CHANGE_EVENT;
            frameRateNode.data = &frameRateInfo;
            frameRateNode.size = sizeof(frameRateInfo);
            frameRateNode.next = events;
            events = &frameRateNode;
        }
        input.p_app_private = events;

        if (svt_av1_enc_send_picture(_handle, &input) != EB_ErrorNone) {
            // Never ask for a packet after a failed send: in low delay svt_av1_enc_get_packet would block forever.
            RTC_LOG(LS_ERROR) << "SvtAv1Encoder: send_picture failed";
            return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
        }
        _configuredBitrateBps = _targetBitrateBps;
        if (frameRateChanged) {
            _configuredFrameRate = frameRate;
        }

        EbBufferHeaderType *output = nullptr;
        if (svt_av1_enc_get_packet(_handle, &output, 0) != EB_ErrorNone || !output) {
            RTC_LOG(LS_ERROR) << "SvtAv1Encoder: get_packet failed";
            return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
        }
        std::vector<uint8_t> data(output->p_buffer, output->p_buffer + output->n_filled_len);
        const bool isKeyframe = output->pic_type == EB_AV1_KEY_PICTURE;
        const int qp = int(output->qp);
        svt_av1_enc_release_out_buffer(&output);
        _needsKeyframe = false;

        if (data.empty()) {
            return WEBRTC_VIDEO_CODEC_OK;
        }
        if (isKeyframe && !HasSequenceHeader(data)) {
            data.insert(data.begin(), _sequenceHeader.begin(), _sequenceHeader.end());
        }

        webrtc::EncodedImage image;
        image.SetEncodedData(webrtc::EncodedImageBuffer::Create(data.data(), data.size()));
        image._frameType = isKeyframe ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
        image.SetRtpTimestamp(frame.timestamp());
        image.capture_time_ms_ = frame.render_time_ms();
        image.rotation_ = frame.rotation();
        image.content_type_ = webrtc::VideoContentType::UNSPECIFIED;
        image.timing_.flags = webrtc::VideoSendTiming::kInvalid;
        image._encodedWidth = _width;
        image._encodedHeight = _height;
        image.qp_ = qp;
        image.SetColorSpace(frame.color_space());

        webrtc::CodecSpecificInfo info;
        info.codecType = webrtc::kVideoCodecAV1;
        info.end_of_picture = true;
        info.scalability_mode = _scalabilityMode;
        info.generic_frame_info = _svcController->OnEncodeDone(layer);
        if (isKeyframe && info.generic_frame_info) {
            info.template_structure = _svcController->DependencyStructure();
            info.template_structure->resolutions = {webrtc::RenderResolution(int(_width), int(_height))};
        }
        _callback->OnEncodedImage(image, &info);
        return WEBRTC_VIDEO_CODEC_OK;
    }

    void SetRates(const RateControlParameters &parameters) override {
        const uint32_t bitrate = parameters.bitrate.get_sum_bps();
        if (bitrate == 0 || parameters.framerate_fps < 1.0) {
            return;
        }
        _targetBitrateBps = bitrate;
        _framerate = parameters.framerate_fps;
    }

    EncoderInfo GetEncoderInfo() const override {
        EncoderInfo info;
        info.supports_native_handle = false;
        info.implementation_name = "SvtAv1";
        info.has_trusted_rate_controller = true;
        info.is_hardware_accelerated = false;
        info.scaling_settings = VideoEncoder::ScalingSettings(kLowQp, kHighQp);
        info.preferred_pixel_formats = {webrtc::VideoFrameBuffer::Type::kI420};
        return info;
    }

private:
    bool createHandle(uint32_t width, uint32_t height) {
        EbSvtAv1EncConfiguration config;
        std::memset(&config, 0, sizeof(config));
        if (svt_av1_enc_init_handle(&_handle, &config) != EB_ErrorNone) {
            _handle = nullptr;
            return false;
        }
        const uint32_t frameRate = FrameRateToInteger(_framerate);
        config.source_width = width;
        config.source_height = height;
        config.frame_rate_numerator = frameRate;
        config.frame_rate_denominator = 1;
        config.encoder_bit_depth = 8;
        // tgcalls' camera and VideoToolbox codecs work in full-range NV12.
        config.color_range = EB_CR_FULL_RANGE;
        config.rtc = true;
        config.pred_structure = LOW_DELAY;
        config.rate_control_mode = SVT_AV1_RC_MODE_CBR;
        config.target_bit_rate = _targetBitrateBps;
        config.intra_period_length = -1;
        config.enc_mode = kPreset;
        config.level_of_parallelism = kParallelismLevel;
        if (svt_av1_enc_set_parameter(_handle, &config) != EB_ErrorNone || svt_av1_enc_init(_handle) != EB_ErrorNone) {
            svt_av1_enc_deinit_handle(_handle);
            _handle = nullptr;
            return false;
        }
        _initialized = true;

        _sequenceHeader.clear();
        EbBufferHeaderType *streamHeader = nullptr;
        if (svt_av1_enc_stream_header(_handle, &streamHeader) == EB_ErrorNone && streamHeader) {
            _sequenceHeader.assign(streamHeader->p_buffer, streamHeader->p_buffer + streamHeader->n_filled_len);
            svt_av1_enc_stream_header_release(streamHeader);
        }

        _width = width;
        _height = height;
        _configuredBitrateBps = _targetBitrateBps;
        _configuredFrameRate = frameRate;
        _needsKeyframe = true;
        RTC_LOG(LS_INFO) << "SvtAv1Encoder: initialized " << width << "x" << height << ", "
                         << _targetBitrateBps / 1000 << " kbps";
        return true;
    }

    void destroyHandle() {
        if (_handle) {
            if (_initialized) {
                svt_av1_enc_deinit(_handle);
            }
            svt_av1_enc_deinit_handle(_handle);
            _handle = nullptr;
        }
        _initialized = false;
    }

    EbComponentType *_handle = nullptr;
    bool _initialized = false;
    webrtc::EncodedImageCallback *_callback = nullptr;
    webrtc::VideoCodec _codec;
    webrtc::ScalabilityMode _scalabilityMode = webrtc::ScalabilityMode::kL1T1;
    std::unique_ptr<webrtc::ScalableVideoController> _svcController;
    std::vector<uint8_t> _sequenceHeader;
    uint32_t _width = 0;
    uint32_t _height = 0;
    uint32_t _targetBitrateBps = 0;
    uint32_t _configuredBitrateBps = 0;
    double _framerate = 0.0;
    uint32_t _configuredFrameRate = 0;
    int64_t _pts = 0;
    bool _needsKeyframe = true;
};

class RefusingAv1Encoder final : public webrtc::VideoEncoder {
public:
    int32_t InitEncode(const webrtc::VideoCodec *, const Settings &) override {
        RTC_LOG(LS_WARNING) << "Av1: encoding refused on this device";
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback *) override {
        return WEBRTC_VIDEO_CODEC_OK;
    }
    int32_t Release() override {
        return WEBRTC_VIDEO_CODEC_OK;
    }
    int32_t Encode(const webrtc::VideoFrame &, const std::vector<webrtc::VideoFrameType> *) override {
        return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }
    void SetRates(const RateControlParameters &) override {
    }
    EncoderInfo GetEncoderInfo() const override {
        EncoderInfo info;
        info.implementation_name = "Av1Refused";
        return info;
    }
};

} // namespace

std::unique_ptr<webrtc::VideoEncoder> CreateSvtAv1Encoder() {
    return std::make_unique<SvtAv1Encoder>();
}

std::unique_ptr<webrtc::VideoEncoder> CreateRefusingAv1Encoder() {
    return std::make_unique<RefusingAv1Encoder>();
}

} // namespace tgcalls
