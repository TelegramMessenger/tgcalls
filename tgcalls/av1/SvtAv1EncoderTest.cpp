// SvtAv1Encoder as WebRTC sees it: keyframes with sequence headers, rate changes without re-initialising, a new
// resolution after a second InitEncode, NV12 input, the encoder info, and the refusing encoder.

#include "av1/Av1Obu.h"
#include "av1/SvtAv1Encoder.h"

#include "api/video/i420_buffer.h"
#include "api/video/nv12_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"

#include <sys/resource.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

static int failures = 0;

#define CHECK_TRUE(cond)                                                                   \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "%s:%d: CHECK_TRUE(%s) failed\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                    \
        }                                                                                  \
    } while (0)

namespace {

struct EncodedFrame {
    std::vector<uint8_t> data;
    bool isKey = false;
    webrtc::CodecSpecificInfo info;
    uint32_t width = 0;
    uint32_t height = 0;
    int qp = -1;
};

class Collector : public webrtc::EncodedImageCallback {
public:
    Result OnEncodedImage(const webrtc::EncodedImage &image, const webrtc::CodecSpecificInfo *info) override {
        EncodedFrame frame;
        frame.data.assign(image.data(), image.data() + image.size());
        frame.isKey = image._frameType == webrtc::VideoFrameType::kVideoFrameKey;
        if (info) {
            frame.info = *info;
        }
        frame.width = image._encodedWidth;
        frame.height = image._encodedHeight;
        frame.qp = image.qp_;
        frames.push_back(std::move(frame));
        return Result(Result::OK);
    }
    std::vector<EncodedFrame> frames;
};

webrtc::VideoCodec CodecSettings(int width, int height, int startKbps) {
    webrtc::VideoCodec codec;
    codec.codecType = webrtc::kVideoCodecAV1;
    codec.width = uint16_t(width);
    codec.height = uint16_t(height);
    codec.maxFramerate = 30;
    codec.startBitrate = uint32_t(startKbps);
    codec.maxBitrate = 2500;
    codec.qpMax = 63;
    codec.SetScalabilityMode(webrtc::ScalabilityMode::kL1T1);
    return codec;
}

webrtc::VideoEncoder::Settings EncoderSettings() {
    return webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 4, 1200);
}

void SetRate(webrtc::VideoEncoder &encoder, uint32_t bps) {
    webrtc::VideoEncoder::RateControlParameters parameters;
    parameters.framerate_fps = 30;
    parameters.bitrate.SetBitrate(0, 0, bps);
    encoder.SetRates(parameters);
}

// Moving pseudo-random texture over a gradient, as the SVT-AV1 API test uses.
rtc::scoped_refptr<webrtc::I420Buffer> MakeI420(int index, int width, int height) {
    auto buffer = webrtc::I420Buffer::Create(width, height);
    for (int row = 0; row < height; row++) {
        uint8_t *y = buffer->MutableDataY() + row * buffer->StrideY();
        for (int col = 0; col < width; col++) {
            uint32_t x = uint32_t(col + index * 3);
            uint32_t hash = (x * 2654435761u) ^ (uint32_t(row) * 40503u) ^ ((x >> 3) * uint32_t(row >> 3) * 97u);
            y[col] = uint8_t(((x + uint32_t(row)) & 0xff) / 2 + ((hash >> 13) & 0x7f));
        }
    }
    for (int row = 0; row < (height + 1) / 2; row++) {
        for (int col = 0; col < (width + 1) / 2; col++) {
            buffer->MutableDataU()[row * buffer->StrideU() + col] = uint8_t(128 + ((col + index) % 32));
            buffer->MutableDataV()[row * buffer->StrideV() + col] = uint8_t(128 - ((row + index) % 32));
        }
    }
    return buffer;
}

webrtc::VideoFrame MakeFrame(rtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer, int index) {
    return webrtc::VideoFrame::Builder()
        .set_video_frame_buffer(buffer)
        .set_timestamp_rtp(uint32_t(index) * 3000)
        .set_timestamp_us(int64_t(index) * 33333)
        .build();
}

bool Encode(webrtc::VideoEncoder &encoder, int index, int width, int height, bool forceKey) {
    std::vector<webrtc::VideoFrameType> types = {forceKey ? webrtc::VideoFrameType::kVideoFrameKey
                                                          : webrtc::VideoFrameType::kVideoFrameDelta};
    return encoder.Encode(MakeFrame(MakeI420(index, width, height), index), &types) == WEBRTC_VIDEO_CODEC_OK;
}

std::optional<tgcalls::Av1SequenceHeader> SequenceHeaderOf(const EncodedFrame &frame) {
    for (const auto &obu : tgcalls::ParseAv1Obus(frame.data.data(), frame.data.size())) {
        if (obu.type == tgcalls::kAv1ObuSequenceHeader) {
            return tgcalls::ParseAv1SequenceHeader(obu);
        }
    }
    return std::nullopt;
}

double MeanBytes(const std::vector<EncodedFrame> &frames, size_t from, size_t to) {
    double total = 0;
    for (size_t i = from; i < to; i++) {
        total += double(frames[i].data.size());
    }
    return total / double(to - from);
}

void TestInitEncodeAndFirstKeyframe() {
    auto encoder = tgcalls::CreateSvtAv1Encoder();
    Collector collector;
    encoder->RegisterEncodeCompleteCallback(&collector);
    auto codec = CodecSettings(1280, 720, 800);
    CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    SetRate(*encoder, 800000);
    CHECK_TRUE(Encode(*encoder, 0, 1280, 720, false));
    CHECK_TRUE(collector.frames.size() == 1);
    if (collector.frames.empty()) {
        return;
    }
    const auto &first = collector.frames[0];
    CHECK_TRUE(first.isKey);
    CHECK_TRUE(first.info.codecType == webrtc::kVideoCodecAV1);
    CHECK_TRUE(first.info.generic_frame_info.has_value());
    CHECK_TRUE(first.info.template_structure.has_value());
    CHECK_TRUE(first.info.end_of_picture);
    CHECK_TRUE(first.width == 1280 && first.height == 720);
    auto header = SequenceHeaderOf(first);
    CHECK_TRUE(header.has_value());
    encoder->Release();
}

void TestKeyframeOnRequestCarriesSequenceHeader() {
    auto encoder = tgcalls::CreateSvtAv1Encoder();
    Collector collector;
    encoder->RegisterEncodeCompleteCallback(&collector);
    auto codec = CodecSettings(640, 360, 600);
    CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    SetRate(*encoder, 600000);
    for (int i = 0; i < 30; i++) {
        CHECK_TRUE(Encode(*encoder, i, 640, 360, i == 20));
    }
    CHECK_TRUE(collector.frames.size() == 30);
    if (collector.frames.size() != 30) {
        return;
    }
    CHECK_TRUE(collector.frames[20].isKey);
    CHECK_TRUE(SequenceHeaderOf(collector.frames[20]).has_value());
    CHECK_TRUE(collector.frames[20].info.template_structure.has_value());
    CHECK_TRUE(!collector.frames[19].isKey);
    CHECK_TRUE(!collector.frames[21].isKey);
    encoder->Release();
}

void TestSetRatesDoesNotReinit() {
    auto encoder = tgcalls::CreateSvtAv1Encoder();
    Collector collector;
    encoder->RegisterEncodeCompleteCallback(&collector);
    auto codec = CodecSettings(640, 360, 1200);
    CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    SetRate(*encoder, 1200000);
    for (int i = 0; i < 240; i++) {
        if (i == 120) {
            SetRate(*encoder, 300000);
        }
        CHECK_TRUE(Encode(*encoder, i, 640, 360, false));
    }
    CHECK_TRUE(collector.frames.size() == 240);
    if (collector.frames.size() != 240) {
        return;
    }
    for (size_t i = 1; i < collector.frames.size(); i++) {
        CHECK_TRUE(!collector.frames[i].isKey);
    }
    double before = MeanBytes(collector.frames, 60, 120);
    double after = MeanBytes(collector.frames, 180, 240);
    std::printf("SetRates: %.0f bytes/frame at 1200 kbps, %.0f at 300 kbps\n", before, after);
    CHECK_TRUE(after < 0.6 * before);
    // The quality-scaling thresholds assume QP on the 0-63 scale, and a lower bitrate must raise it.
    int maxQp = 0;
    double qpBefore = 0, qpAfter = 0;
    for (size_t i = 0; i < collector.frames.size(); i++) {
        CHECK_TRUE(collector.frames[i].qp >= 0 && collector.frames[i].qp <= 63);
        maxQp = std::max(maxQp, collector.frames[i].qp);
        if (i >= 60 && i < 120) {
            qpBefore += collector.frames[i].qp / 60.0;
        } else if (i >= 180) {
            qpAfter += collector.frames[i].qp / 60.0;
        }
    }
    std::printf("SetRates: mean QP %.1f at 1200 kbps, %.1f at 300 kbps (max %d)\n", qpBefore, qpAfter, maxQp);
    CHECK_TRUE(qpAfter > qpBefore);
    encoder->Release();
}

void TestReinitAtNewResolution() {
    auto encoder = tgcalls::CreateSvtAv1Encoder();
    Collector collector;
    encoder->RegisterEncodeCompleteCallback(&collector);
    auto codec = CodecSettings(1280, 720, 800);
    CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    SetRate(*encoder, 800000);
    for (int i = 0; i < 10; i++) {
        CHECK_TRUE(Encode(*encoder, i, 1280, 720, false));
    }
    auto smaller = CodecSettings(640, 360, 400);
    CHECK_TRUE(encoder->InitEncode(&smaller, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    SetRate(*encoder, 400000);
    CHECK_TRUE(Encode(*encoder, 10, 640, 360, false));
    CHECK_TRUE(collector.frames.size() == 11);
    if (collector.frames.size() != 11) {
        return;
    }
    const auto &frame = collector.frames[10];
    CHECK_TRUE(frame.isKey);
    CHECK_TRUE(frame.width == 640 && frame.height == 360);
    auto header = SequenceHeaderOf(frame);
    CHECK_TRUE(header.has_value() && header->maxWidth == 640 && header->maxHeight == 360);
    encoder->Release();
}

void TestNv12InputEncodes() {
    auto encoder = tgcalls::CreateSvtAv1Encoder();
    Collector collector;
    encoder->RegisterEncodeCompleteCallback(&collector);
    auto codec = CodecSettings(640, 360, 600);
    CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    SetRate(*encoder, 600000);
    for (int i = 0; i < 5; i++) {
        auto i420 = MakeI420(i, 640, 360);
        auto nv12 = webrtc::NV12Buffer::Copy(*i420);
        std::vector<webrtc::VideoFrameType> types = {webrtc::VideoFrameType::kVideoFrameDelta};
        CHECK_TRUE(encoder->Encode(MakeFrame(nv12, i), &types) == WEBRTC_VIDEO_CODEC_OK);
    }
    CHECK_TRUE(collector.frames.size() == 5);
    encoder->Release();
}

void TestEncoderInfo() {
    auto encoder = tgcalls::CreateSvtAv1Encoder();
    auto info = encoder->GetEncoderInfo();
    CHECK_TRUE(info.implementation_name == "SvtAv1");
    CHECK_TRUE(!info.is_hardware_accelerated);
    CHECK_TRUE(info.scaling_settings.thresholds.has_value());
}

void TestRefusingEncoder() {
    auto encoder = tgcalls::CreateRefusingAv1Encoder();
    auto codec = CodecSettings(640, 360, 600);
    CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) == WEBRTC_VIDEO_CODEC_ERROR);
}

long MaxResidentKilobytes() {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
    return long(usage.ru_maxrss / 1024);
#else
    return long(usage.ru_maxrss);
#endif
}

// Not a pass/fail check: the spec asks for the encoder's footprint at the call's resolution.
void MeasureMemoryAt720p() {
    long before = MaxResidentKilobytes();
    auto encoder = tgcalls::CreateSvtAv1Encoder();
    Collector collector;
    encoder->RegisterEncodeCompleteCallback(&collector);
    auto codec = CodecSettings(1280, 720, 1200);
    CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    SetRate(*encoder, 1200000);
    for (int i = 0; i < 300; i++) {
        CHECK_TRUE(Encode(*encoder, i, 1280, 720, false));
    }
    long after = MaxResidentKilobytes();
    std::printf("memory: peak resident grew by %ld KB over 300 frames at 1280x720\n", after - before);
    encoder->Release();
}

} // namespace

int main() {
    // Memory first, while the peak resident size still reflects only this process's start-up.
    MeasureMemoryAt720p();
    TestInitEncodeAndFirstKeyframe();
    TestKeyframeOnRequestCarriesSequenceHeader();
    TestSetRatesDoesNotReinit();
    TestReinitAtNewResolution();
    TestNv12InputEncodes();
    TestEncoderInfo();
    TestRefusingEncoder();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all SvtAv1Encoder checks passed\n");
    return 0;
}
