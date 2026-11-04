// The VideoToolbox AV1 decoder against SVT-AV1 output, on a Mac whose chip decodes AV1 (M3 or later): the av1C record,
// output identical to dav1d's (AV1 decoding is normative), and a new session when the sequence header changes.

#include "av1/Av1Obu.h"
#include "av1/SvtAv1Encoder.h"
#include "platform/darwin/Av1VideoToolboxDecoder.h"

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/codecs/av1/dav1d_decoder.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"

#include <CoreVideo/CoreVideo.h>

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
};

class EncodedCollector : public webrtc::EncodedImageCallback {
public:
    Result OnEncodedImage(const webrtc::EncodedImage &image, const webrtc::CodecSpecificInfo *) override {
        EncodedFrame frame;
        frame.data.assign(image.data(), image.data() + image.size());
        frame.isKey = image._frameType == webrtc::VideoFrameType::kVideoFrameKey;
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
    std::vector<rtc::scoped_refptr<webrtc::I420BufferInterface>> frames;
};

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

webrtc::VideoCodec CodecSettings(int width, int height, int kbps) {
    webrtc::VideoCodec codec;
    codec.codecType = webrtc::kVideoCodecAV1;
    codec.width = uint16_t(width);
    codec.height = uint16_t(height);
    codec.maxFramerate = 30;
    codec.startBitrate = uint32_t(kbps);
    codec.maxBitrate = 2500;
    codec.qpMax = 63;
    codec.SetScalabilityMode(webrtc::ScalabilityMode::kL1T1);
    return codec;
}

std::vector<EncodedFrame> EncodeSequence(int count, int width, int height, int resizeAt, int newWidth, int newHeight) {
    auto encoder = tgcalls::CreateSvtAv1Encoder();
    EncodedCollector collector;
    encoder->RegisterEncodeCompleteCallback(&collector);
    const webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 4, 1200);
    auto codec = CodecSettings(width, height, 800);
    CHECK_TRUE(encoder->InitEncode(&codec, settings) == WEBRTC_VIDEO_CODEC_OK);
    for (int i = 0; i < count; i++) {
        bool resized = resizeAt >= 0 && i >= resizeAt;
        if (i == resizeAt) {
            auto smaller = CodecSettings(newWidth, newHeight, 400);
            CHECK_TRUE(encoder->InitEncode(&smaller, settings) == WEBRTC_VIDEO_CODEC_OK);
        }
        std::vector<webrtc::VideoFrameType> types = {webrtc::VideoFrameType::kVideoFrameDelta};
        auto frame = webrtc::VideoFrame::Builder()
                         .set_video_frame_buffer(MakeI420(i, resized ? newWidth : width, resized ? newHeight : height))
                         .set_timestamp_rtp(uint32_t(i) * 3000)
                         .set_timestamp_us(int64_t(i) * 33333)
                         .build();
        CHECK_TRUE(encoder->Encode(frame, &types) == WEBRTC_VIDEO_CODEC_OK);
    }
    encoder->Release();
    return collector.frames;
}

struct VideoToolboxOutput {
    std::vector<CVPixelBufferRef> buffers;
    std::vector<uint32_t> timestamps;
    ~VideoToolboxOutput() {
        for (auto buffer : buffers) {
            CVPixelBufferRelease(buffer);
        }
    }
};

std::unique_ptr<tgcalls::Av1VideoToolboxDecoder> MakeVideoToolboxDecoder(VideoToolboxOutput &output) {
    return std::make_unique<tgcalls::Av1VideoToolboxDecoder>([&output](CVPixelBufferRef buffer, uint32_t timestamp) {
        output.buffers.push_back(CVPixelBufferRetain(buffer));
        output.timestamps.push_back(timestamp);
    });
}

std::vector<rtc::scoped_refptr<webrtc::I420BufferInterface>> DecodeWithDav1d(const std::vector<EncodedFrame> &frames) {
    DecodedCollector collector;
    auto decoder = webrtc::CreateDav1dDecoder();
    webrtc::VideoDecoder::Settings settings;
    settings.set_codec_type(webrtc::kVideoCodecAV1);
    settings.set_max_render_resolution({1280, 720});
    settings.set_number_of_cores(2);
    CHECK_TRUE(decoder->Configure(settings));
    decoder->RegisterDecodeCompleteCallback(&collector);
    for (size_t i = 0; i < frames.size(); i++) {
        webrtc::EncodedImage image;
        image.SetEncodedData(webrtc::EncodedImageBuffer::Create(frames[i].data.data(), frames[i].data.size()));
        image._frameType = frames[i].isKey ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
        image.SetRtpTimestamp(uint32_t(i) * 3000);
        CHECK_TRUE(decoder->Decode(image, false, 0) == WEBRTC_VIDEO_CODEC_OK);
    }
    return collector.frames;
}

// Compares a VideoToolbox NV12 buffer with a dav1d I420 frame, sample for sample.
bool SameSamples(CVPixelBufferRef nv12, const webrtc::I420BufferInterface &i420) {
    if (int(CVPixelBufferGetWidth(nv12)) != i420.width() || int(CVPixelBufferGetHeight(nv12)) != i420.height()) {
        return false;
    }
    CVPixelBufferLockBaseAddress(nv12, kCVPixelBufferLock_ReadOnly);
    bool same = true;
    const uint8_t *y = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(nv12, 0));
    const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(nv12, 0);
    for (int row = 0; row < i420.height() && same; row++) {
        for (int col = 0; col < i420.width(); col++) {
            if (y[row * yStride + col] != i420.DataY()[row * i420.StrideY() + col]) {
                same = false;
                break;
            }
        }
    }
    const uint8_t *uv = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(nv12, 1));
    const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(nv12, 1);
    for (int row = 0; row < i420.ChromaHeight() && same; row++) {
        for (int col = 0; col < i420.ChromaWidth(); col++) {
            if (uv[row * uvStride + 2 * col] != i420.DataU()[row * i420.StrideU() + col] ||
                uv[row * uvStride + 2 * col + 1] != i420.DataV()[row * i420.StrideV() + col]) {
                same = false;
                break;
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(nv12, kCVPixelBufferLock_ReadOnly);
    return same;
}

void TestAv1ConfigRecordFromSequenceHeader() {
    auto frames = EncodeSequence(1, 640, 360, -1, 0, 0);
    CHECK_TRUE(frames.size() == 1);
    if (frames.empty()) {
        return;
    }
    std::optional<tgcalls::Av1Obu> sequenceHeader;
    for (const auto &obu : tgcalls::ParseAv1Obus(frames[0].data.data(), frames[0].data.size())) {
        if (obu.type == tgcalls::kAv1ObuSequenceHeader) {
            sequenceHeader = obu;
        }
    }
    CHECK_TRUE(sequenceHeader.has_value());
    if (!sequenceHeader) {
        return;
    }
    auto parsed = tgcalls::ParseAv1SequenceHeader(*sequenceHeader);
    CHECK_TRUE(parsed.has_value());
    auto record = tgcalls::MakeAv1CodecConfigurationRecord(*sequenceHeader);
    CHECK_TRUE(record.size() == 4 + sequenceHeader->size);
    if (record.size() != 4 + sequenceHeader->size || !parsed) {
        return;
    }
    CHECK_TRUE(record[0] == 0x81);
    CHECK_TRUE((record[1] >> 5) == 0);
    CHECK_TRUE((record[1] & 0x1f) == parsed->level);
    CHECK_TRUE(((record[2] >> 7) & 1) == parsed->tier);
    CHECK_TRUE(((record[2] >> 6) & 1) == 0); // high_bitdepth
    CHECK_TRUE(((record[2] >> 5) & 1) == 0); // twelve_bit
    CHECK_TRUE(((record[2] >> 4) & 1) == 0); // monochrome
    CHECK_TRUE(((record[2] >> 3) & 1) == 1); // chroma_subsampling_x
    CHECK_TRUE(((record[2] >> 2) & 1) == 1); // chroma_subsampling_y
    CHECK_TRUE((record[2] & 3) == parsed->chromaSamplePosition);
    CHECK_TRUE(record[3] == 0);
    CHECK_TRUE(std::equal(record.begin() + 4, record.end(), sequenceHeader->data));
}

void TestMatchesDav1dBitExact() {
    if (!tgcalls::Av1VideoToolboxDecoder::IsSupported()) {
        std::printf("SKIPPED TestMatchesDav1dBitExact: VideoToolbox cannot decode AV1 on this machine\n");
        return;
    }
    auto frames = EncodeSequence(30, 640, 360, -1, 0, 0);
    CHECK_TRUE(frames.size() == 30);
    VideoToolboxOutput output;
    auto decoder = MakeVideoToolboxDecoder(output);
    for (size_t i = 0; i < frames.size(); i++) {
        CHECK_TRUE(decoder->Decode(frames[i].data.data(), frames[i].data.size(), uint32_t(i) * 3000));
    }
    auto reference = DecodeWithDav1d(frames);
    CHECK_TRUE(output.buffers.size() == 30);
    CHECK_TRUE(reference.size() == 30);
    int identical = 0;
    for (size_t i = 0; i < std::min(output.buffers.size(), reference.size()); i++) {
        CHECK_TRUE(CVPixelBufferGetPixelFormatType(output.buffers[i]) == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);
        CHECK_TRUE(output.timestamps[i] == uint32_t(i) * 3000);
        if (SameSamples(output.buffers[i], *reference[i])) {
            identical++;
        }
    }
    std::printf("VideoToolbox vs dav1d: %d of %zu frames identical\n", identical, frames.size());
    CHECK_TRUE(identical == 30);
}

void TestNewSequenceHeaderRecreatesSession() {
    if (!tgcalls::Av1VideoToolboxDecoder::IsSupported()) {
        std::printf("SKIPPED TestNewSequenceHeaderRecreatesSession\n");
        return;
    }
    auto frames = EncodeSequence(60, 640, 360, 30, 320, 180);
    CHECK_TRUE(frames.size() == 60);
    VideoToolboxOutput output;
    auto decoder = MakeVideoToolboxDecoder(output);
    for (size_t i = 0; i < frames.size(); i++) {
        CHECK_TRUE(decoder->Decode(frames[i].data.data(), frames[i].data.size(), uint32_t(i) * 3000));
    }
    CHECK_TRUE(output.buffers.size() == 60);
    for (size_t i = 0; i < output.buffers.size(); i++) {
        const size_t expectedWidth = i < 30 ? 640 : 320;
        const size_t expectedHeight = i < 30 ? 360 : 180;
        CHECK_TRUE(CVPixelBufferGetWidth(output.buffers[i]) == expectedWidth);
        CHECK_TRUE(CVPixelBufferGetHeight(output.buffers[i]) == expectedHeight);
    }
}

} // namespace

int main() {
    TestAv1ConfigRecordFromSequenceHeader();
    TestMatchesDav1dBitExact();
    TestNewSequenceHeaderRecreatesSession();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all VideoToolbox AV1 checks passed\n");
    return 0;
}
