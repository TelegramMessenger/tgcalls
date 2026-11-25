// SVT-AV1 encoder -> RtpPacketizerAv1 -> VideoRtpDepacketizerAv1 -> dav1d: what a call's AV1 path does end to end,
// minus the network.

#include "av1/SvtAv1Encoder.h"

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/rtp_rtcp/source/rtp_packet_to_send.h"
#include "modules/rtp_rtcp/source/rtp_packetizer_av1.h"
#include "modules/rtp_rtcp/source/video_rtp_depacketizer_av1.h"
#include "modules/video_coding/codecs/av1/dav1d_decoder.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
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
    // From the generic frame info (the dependency descriptor's): -1 when absent.
    int temporalId = -1;
    int decodeTargets = 0;
    bool hasStructure = false;
};

class EncodedCollector : public webrtc::EncodedImageCallback {
public:
    void OnFrameDropped(uint32_t, int, bool) override {}

    Result OnEncodedImage(const webrtc::EncodedImage &image, const webrtc::CodecSpecificInfo *info) override {
        EncodedFrame frame;
        frame.data.assign(image.data(), image.data() + image.size());
        frame.isKey = image._frameType == webrtc::VideoFrameType::kVideoFrameKey;
        if (info && info->generic_frame_info) {
            frame.temporalId = info->generic_frame_info->temporal_id;
            frame.decodeTargets = int(info->generic_frame_info->decode_target_indications.size());
        }
        frame.hasStructure = info && info->template_structure.has_value();
        frames.push_back(std::move(frame));
        return Result(Result::OK);
    }
    std::vector<EncodedFrame> frames;
};

class DecodedCollector : public webrtc::DecodedImageCallback {
public:
    int32_t Decoded(webrtc::VideoFrame &frame) override {
        frames.push_back(frame.video_frame_buffer()->ToI420());
        return 0;
    }
    std::vector<webrtc::scoped_refptr<webrtc::I420BufferInterface>> frames;
};

// The same moving texture the encoder test uses.
webrtc::scoped_refptr<webrtc::I420Buffer> MakeI420(int index, int width, int height) {
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

webrtc::VideoCodec CodecSettings(int width, int height, int kbps,
                                 webrtc::ScalabilityMode mode = webrtc::ScalabilityMode::kL1T1) {
    webrtc::VideoCodec codec;
    codec.codecType = webrtc::kVideoCodecAV1;
    codec.width = uint16_t(width);
    codec.height = uint16_t(height);
    codec.maxFramerate = 30;
    codec.startBitrate = uint32_t(kbps);
    codec.maxBitrate = 2500;
    codec.qpMax = 63;
    codec.SetScalabilityMode(mode);
    return codec;
}

void Encode(webrtc::VideoEncoder &encoder, int index, int width, int height, bool forceKey) {
    std::vector<webrtc::VideoFrameType> types = {forceKey ? webrtc::VideoFrameType::kVideoFrameKey
                                                          : webrtc::VideoFrameType::kVideoFrameDelta};
    auto frame = webrtc::VideoFrame::Builder()
                     .set_video_frame_buffer(MakeI420(index, width, height))
                     .set_timestamp_rtp(uint32_t(index) * 3000)
                     .set_timestamp_us(int64_t(index) * 33333)
                     .build();
    CHECK_TRUE(encoder.Encode(frame, &types) == WEBRTC_VIDEO_CODEC_OK);
}

void SetRate(webrtc::VideoEncoder &encoder, uint32_t bps) {
    webrtc::VideoEncoder::RateControlParameters parameters;
    parameters.framerate_fps = 30;
    parameters.bitrate.SetBitrate(0, 0, bps);
    encoder.SetRates(parameters);
}

// Packetizes one encoded frame as a sender would and reassembles it as a receiver would.
std::vector<uint8_t> ThroughRtp(const EncodedFrame &frame) {
    webrtc::RtpPacketizer::PayloadSizeLimits limits;
    limits.max_payload_len = 1200;
    webrtc::RtpPacketizerAv1 packetizer(frame.data, limits,
                                        frame.isKey ? webrtc::VideoFrameType::kVideoFrameKey
                                                    : webrtc::VideoFrameType::kVideoFrameDelta,
                                        true);
    std::vector<std::vector<uint8_t>> payloads;
    webrtc::RtpPacketToSend packet(nullptr);
    while (packetizer.NextPacket(&packet)) {
        auto payload = packet.payload();
        payloads.emplace_back(payload.begin(), payload.end());
    }
    std::vector<std::span<const uint8_t>> views;
    for (const auto &payload : payloads) {
        views.emplace_back(payload);
    }
    webrtc::VideoRtpDepacketizerAv1 depacketizer;
    auto assembled = depacketizer.AssembleFrame(views);
    if (!assembled) {
        return {};
    }
    return std::vector<uint8_t>(assembled->data(), assembled->data() + assembled->size());
}

std::unique_ptr<webrtc::VideoDecoder> MakeDecoder(int width, int height, DecodedCollector &collector) {
    auto decoder = webrtc::CreateDav1dDecoder();
    webrtc::VideoDecoder::Settings settings;
    settings.set_codec_type(webrtc::kVideoCodecAV1);
    settings.set_max_render_resolution({width, height});
    settings.set_number_of_cores(2);
    CHECK_TRUE(decoder->Configure(settings));
    decoder->RegisterDecodeCompleteCallback(&collector);
    return decoder;
}

bool Decode(webrtc::VideoDecoder &decoder, const std::vector<uint8_t> &data, bool isKey, int index) {
    webrtc::EncodedImage image;
    image.SetEncodedData(webrtc::EncodedImageBuffer::Create(data.data(), data.size()));
    image._frameType = isKey ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
    image.SetRtpTimestamp(uint32_t(index) * 3000);
    return decoder.Decode(image, false, 0) == WEBRTC_VIDEO_CODEC_OK;
}

double LumaPsnr(const webrtc::I420BufferInterface &a, const webrtc::I420BufferInterface &b) {
    double sse = 0;
    for (int row = 0; row < a.height(); row++) {
        for (int col = 0; col < a.width(); col++) {
            double d = double(a.DataY()[row * a.StrideY() + col]) - double(b.DataY()[row * b.StrideY() + col]);
            sse += d * d;
        }
    }
    double mse = sse / double(a.width() * a.height());
    return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

std::vector<EncodedFrame> EncodeSequence(int count, int width, int height, int kbps, int forcedKeyAt,
                                         int resizeAt, int newWidth, int newHeight,
                                         webrtc::ScalabilityMode mode = webrtc::ScalabilityMode::kL1T1) {
    auto encoder = tgcalls::CreateSvtAv1Encoder();
    EncodedCollector collector;
    encoder->RegisterEncodeCompleteCallback(&collector);
    auto codec = CodecSettings(width, height, kbps, mode);
    CHECK_TRUE(encoder->InitEncode(&codec, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 4, 1200)) == WEBRTC_VIDEO_CODEC_OK);
    SetRate(*encoder, uint32_t(kbps) * 1000);
    for (int i = 0; i < count; i++) {
        if (i == resizeAt) {
            auto resized = CodecSettings(newWidth, newHeight, kbps / 2);
            CHECK_TRUE(encoder->InitEncode(&resized, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 4, 1200)) == WEBRTC_VIDEO_CODEC_OK);
            SetRate(*encoder, uint32_t(kbps / 2) * 1000);
        }
        bool resized = resizeAt >= 0 && i >= resizeAt;
        Encode(*encoder, i, resized ? newWidth : width, resized ? newHeight : height, i == forcedKeyAt);
    }
    encoder->Release();
    return collector.frames;
}

void TestRoundTripKeyAndDeltaFrames() {
    auto encoded = EncodeSequence(60, 640, 360, 800, -1, -1, 0, 0);
    CHECK_TRUE(encoded.size() == 60);
    DecodedCollector decoded;
    auto decoder = MakeDecoder(640, 360, decoded);
    for (size_t i = 0; i < encoded.size(); i++) {
        auto data = ThroughRtp(encoded[i]);
        CHECK_TRUE(!data.empty());
        CHECK_TRUE(Decode(*decoder, data, encoded[i].isKey, int(i)));
    }
    CHECK_TRUE(decoded.frames.size() == 60);
    // The test texture is noise, hard to code at 800 kbps; what matters is that each decoded frame is its own source
    // frame, so the PSNR against it must clear 25 dB and beat the PSNR against the next source frame by a wide margin.
    double minPsnr = 99.0;
    double maxPsnrAgainstNext = 0.0;
    for (size_t i = 0; i < decoded.frames.size(); i++) {
        CHECK_TRUE(decoded.frames[i]->width() == 640 && decoded.frames[i]->height() == 360);
        minPsnr = std::min(minPsnr, LumaPsnr(*decoded.frames[i], *MakeI420(int(i), 640, 360)));
        maxPsnrAgainstNext = std::max(maxPsnrAgainstNext, LumaPsnr(*decoded.frames[i], *MakeI420(int(i) + 1, 640, 360)));
    }
    std::printf("round trip: 60 frames, minimum luma PSNR %.1f dB against the source, at most %.1f dB against the next "
                "source frame\n", minPsnr, maxPsnrAgainstNext);
    CHECK_TRUE(minPsnr > 25.0);
    CHECK_TRUE(minPsnr > maxPsnrAgainstNext + 10.0);
}

void TestDecodeFromMidStreamKeyframe() {
    auto encoded = EncodeSequence(60, 640, 360, 800, 30, -1, 0, 0);
    CHECK_TRUE(encoded.size() == 60);
    CHECK_TRUE(encoded[30].isKey);
    DecodedCollector decoded;
    auto decoder = MakeDecoder(640, 360, decoded);
    for (size_t i = 30; i < encoded.size(); i++) {
        CHECK_TRUE(Decode(*decoder, ThroughRtp(encoded[i]), encoded[i].isKey, int(i)));
    }
    CHECK_TRUE(decoded.frames.size() == 30);
}

void TestResolutionChangeDecodes() {
    auto encoded = EncodeSequence(60, 640, 360, 800, -1, 30, 320, 180);
    CHECK_TRUE(encoded.size() == 60);
    DecodedCollector decoded;
    auto decoder = MakeDecoder(640, 360, decoded);
    for (size_t i = 0; i < encoded.size(); i++) {
        CHECK_TRUE(Decode(*decoder, ThroughRtp(encoded[i]), encoded[i].isKey, int(i)));
    }
    CHECK_TRUE(decoded.frames.size() == 60);
    for (size_t i = 30; i < decoded.frames.size(); i++) {
        CHECK_TRUE(decoded.frames[i]->width() == 320 && decoded.frames[i]->height() == 180);
    }
}

bool SameLuma(const webrtc::I420BufferInterface &a, const webrtc::I420BufferInterface &b) {
    if (a.width() != b.width() || a.height() != b.height()) {
        return false;
    }
    for (int row = 0; row < a.height(); row++) {
        if (std::memcmp(a.DataY() + row * a.StrideY(), b.DataY() + row * b.StrideY(), size_t(a.width())) != 0) {
            return false;
        }
    }
    return true;
}

// Decodes the frames whose temporal id is at most `maxTemporalId` and checks each picture against the same frame of
// the full decode: a frame that referenced a dropped layer would decode, but to another picture.
int DecodeLayersMatchingFull(const std::vector<EncodedFrame> &encoded, int maxTemporalId,
                             const std::vector<webrtc::scoped_refptr<webrtc::I420BufferInterface>> &full) {
    DecodedCollector decoded;
    auto decoder = MakeDecoder(640, 360, decoded);
    std::vector<size_t> indices;
    for (size_t i = 0; i < encoded.size(); i++) {
        if (encoded[i].temporalId > maxTemporalId) {
            continue;
        }
        CHECK_TRUE(Decode(*decoder, ThroughRtp(encoded[i]), encoded[i].isKey, int(i)));
        indices.push_back(i);
    }
    CHECK_TRUE(decoded.frames.size() == indices.size());
    int matching = 0;
    for (size_t k = 0; k < decoded.frames.size() && k < indices.size(); k++) {
        if (indices[k] < full.size() && SameLuma(*decoded.frames[k], *full[indices[k]])) {
            matching++;
        }
    }
    return matching;
}

// L1T3 (what an AV1 simulcast encoding asks for): SVT-AV1's three-layer low-delay structure, described per frame by
// WebRTC's L1T3 structure (the dependency descriptor's): temporal ids 0, 2, 1, 2 from every keyframe, a forced keyframe
// mid-pattern included, three decode targets, the structure on keyframes; and the lower decode targets decode on their
// own (T0 alone, T0 and T1) to the very pictures of the full stream.
void TestL1T3LayersDecodeOnTheirOwn() {
    auto encoded = EncodeSequence(26, 640, 360, 800, 10, -1, 0, 0, webrtc::ScalabilityMode::kL1T3);
    CHECK_TRUE(encoded.size() == 26);
    const int pattern[4] = {0, 2, 1, 2};
    int patternErrors = 0;
    for (size_t i = 0; i < encoded.size(); i++) {
        const int sinceKey = int(i) < 10 ? int(i) : int(i) - 10;
        const int expected = pattern[sinceKey % 4];
        if (encoded[i].temporalId != expected) {
            patternErrors++;
            std::fprintf(stderr, "  frame %zu: temporal id %d, expected %d\n", i, encoded[i].temporalId, expected);
        }
        CHECK_TRUE(encoded[i].decodeTargets == 3);
        CHECK_TRUE(encoded[i].hasStructure == encoded[i].isKey);
    }
    CHECK_TRUE(patternErrors == 0);
    CHECK_TRUE(encoded[0].isKey && encoded[10].isKey);

    DecodedCollector full;
    auto decoder = MakeDecoder(640, 360, full);
    for (size_t i = 0; i < encoded.size(); i++) {
        CHECK_TRUE(Decode(*decoder, ThroughRtp(encoded[i]), encoded[i].isKey, int(i)));
    }
    CHECK_TRUE(full.frames.size() == encoded.size());
    int t0 = 0;
    int t01 = 0;
    for (const auto &frame : encoded) {
        t0 += frame.temporalId == 0 ? 1 : 0;
        t01 += frame.temporalId <= 1 ? 1 : 0;
    }
    const int t0Matching = DecodeLayersMatchingFull(encoded, 0, full.frames);
    const int t01Matching = DecodeLayersMatchingFull(encoded, 1, full.frames);
    std::printf("L1T3: T0 alone %d/%d pictures as in the full stream, T0+T1 %d/%d\n", t0Matching, t0, t01Matching, t01);
    CHECK_TRUE(t0 >= 6 && t0Matching == t0);
    CHECK_TRUE(t01 >= 12 && t01Matching == t01);
}

} // namespace

int main() {
    TestRoundTripKeyAndDeltaFrames();
    TestDecodeFromMidStreamKeyframe();
    TestResolutionChangeDecodes();
    TestL1T3LayersDecodeOnTheirOwn();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all AV1 round-trip checks passed\n");
    return 0;
}
