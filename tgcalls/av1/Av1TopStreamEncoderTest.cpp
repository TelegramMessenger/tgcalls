// The encoder of a group screen share's top simulcast stream: AV1 while it works, H264 for good after any AV1 failure,
// and never an error surfaced to WebRTC (which would move the whole sender to another codec).

#include "av1/Av1TopStreamEncoder.h"

#include "api/video/i420_buffer.h"
#include "api/video/video_bitrate_allocation.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/include/video_error_codes.h"

#include <cstdio>
#include <memory>
#include <string>
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

// What a fake encoder saw; owned by the test, so it outlives the encoder.
struct Record {
    std::string name;
    int32_t initResult = WEBRTC_VIDEO_CODEC_OK;
    int32_t encodeResult = WEBRTC_VIDEO_CODEC_OK;
    int inits = 0;
    webrtc::VideoCodecType initCodecType = webrtc::kVideoCodecGeneric;
    int initWidth = 0;
    int initHeight = 0;
    unsigned int initMaxFramerate = 0;
    unsigned int initStartBitrate = 0;
    int encodes = 0;
    std::vector<webrtc::VideoFrameType> lastFrameTypes;
    int rateCalls = 0;
    uint32_t lastRateBps = 0;
    double lastFramerate = 0.0;
    int keyframesAsked = 0;
    webrtc::EncodedImageCallback *callback = nullptr;
    int releases = 0;
};

class FakeEncoder final : public webrtc::VideoEncoder {
public:
    explicit FakeEncoder(Record *record) : _record(record) {}

    int32_t InitEncode(const webrtc::VideoCodec *codec, const Settings &) override {
        _record->inits++;
        _record->initCodecType = codec->codecType;
        _record->initWidth = codec->width;
        _record->initHeight = codec->height;
        _record->initMaxFramerate = codec->maxFramerate;
        _record->initStartBitrate = codec->startBitrate;
        return _record->initResult;
    }
    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback *callback) override {
        _record->callback = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }
    int32_t Release() override {
        _record->releases++;
        return WEBRTC_VIDEO_CODEC_OK;
    }
    int32_t Encode(const webrtc::VideoFrame &, const std::vector<webrtc::VideoFrameType> *types) override {
        _record->encodes++;
        _record->lastFrameTypes = types ? *types : std::vector<webrtc::VideoFrameType>();
        if (types && !types->empty() && (*types)[0] == webrtc::VideoFrameType::kVideoFrameKey) {
            _record->keyframesAsked++;
        }
        return _record->encodeResult;
    }
    void SetRates(const RateControlParameters &parameters) override {
        _record->rateCalls++;
        _record->lastRateBps = parameters.bitrate.get_sum_bps();
        _record->lastFramerate = parameters.framerate_fps;
    }
    EncoderInfo GetEncoderInfo() const override {
        EncoderInfo info;
        info.implementation_name = _record->name;
        return info;
    }

private:
    Record *_record;
};

class NullCallback final : public webrtc::EncodedImageCallback {
public:
    void OnFrameDropped(uint32_t, int, bool) override {}

    Result OnEncodedImage(const webrtc::EncodedImage &, const webrtc::CodecSpecificInfo *) override {
        return Result(Result::OK);
    }
};

struct Setup {
    Record av1;
    int failures = 0;
    NullCallback callback;
    std::unique_ptr<webrtc::VideoEncoder> encoder;
    // Frames go in at 30 fps, the AV1 top stream's cap, so none of them is dropped for the rate.
    int64_t timestampUs = 1000000;
};

// The Top stream's wrapper (a failure callback); `reportToWebRtc`: Single's and Every's (none).
std::unique_ptr<Setup> MakeSetup(bool reportToWebRtc = false) {
    auto setup = std::make_unique<Setup>();
    setup->av1.name = "FakeAv1";
    Setup *raw = setup.get();
    std::function<void()> onFailure;
    if (!reportToWebRtc) {
        onFailure = [raw] {
            raw->failures++;
        };
    }
    setup->encoder = tgcalls::CreateAv1TopStreamEncoder(std::make_unique<FakeEncoder>(&setup->av1), std::move(onFailure));
    return setup;
}

webrtc::VideoCodec H264Settings() {
    webrtc::VideoCodec codec;
    codec.codecType = webrtc::kVideoCodecH264;
    codec.width = 1280;
    codec.height = 720;
    codec.maxFramerate = 30;
    codec.startBitrate = 1000;
    codec.maxBitrate = 2000;
    return codec;
}

int32_t Init(Setup &setup) {
    const auto codec = H264Settings();
    return setup.encoder->InitEncode(&codec, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200));
}

webrtc::VideoFrame Frame(int64_t timestampUs = 1000) {
    return webrtc::VideoFrame::Builder()
        .set_video_frame_buffer(webrtc::I420Buffer::Create(1280, 720))
        .set_timestamp_us(timestampUs)
        .build();
}

int32_t EncodeDelta(Setup &setup) {
    const std::vector<webrtc::VideoFrameType> types = {webrtc::VideoFrameType::kVideoFrameDelta};
    setup.timestampUs += 1000000 / 30;
    return setup.encoder->Encode(Frame(setup.timestampUs), &types);
}

void SetRate(Setup &setup, uint32_t bps) {
    webrtc::VideoBitrateAllocation allocation;
    allocation.SetBitrate(0, 0, bps);
    setup.encoder->SetRates(webrtc::VideoEncoder::RateControlParameters(allocation, 30.0));
}

void TestAv1GetsAv1Settings() {
    auto setup = MakeSetup();
    CHECK_TRUE(Init(*setup) == WEBRTC_VIDEO_CODEC_OK);
    CHECK_TRUE(setup->av1.inits == 1);
    CHECK_TRUE(setup->av1.initCodecType == webrtc::kVideoCodecAV1);
    CHECK_TRUE(setup->av1.initWidth == 1280 && setup->av1.initHeight == 720);
    CHECK_TRUE(setup->av1.initMaxFramerate == 30);
    CHECK_TRUE(setup->av1.initStartBitrate == 1000);
    CHECK_TRUE(setup->failures == 0);
    CHECK_TRUE(setup->encoder->GetEncoderInfo().implementation_name == "FakeAv1");
}

// Top: a failed AV1 stream sends nothing more (WebRTC's mixed-codec simulcast would send an H264 image there as AV1)
// and the engine is told once, which configures the stream for H264 and so recreates the encoder.
void TestInitFailureStopsTheStream() {
    auto setup = MakeSetup();
    setup->av1.initResult = WEBRTC_VIDEO_CODEC_ERROR;
    CHECK_TRUE(Init(*setup) == WEBRTC_VIDEO_CODEC_OK);
    CHECK_TRUE(setup->failures == 1);
    CHECK_TRUE(setup->av1.releases >= 1);
    CHECK_TRUE(EncodeDelta(*setup) == WEBRTC_VIDEO_CODEC_OK);
    CHECK_TRUE(setup->av1.encodes == 0);

    // For good: a later InitEncode (a new frame size) does not try AV1 again.
    setup->av1.initResult = WEBRTC_VIDEO_CODEC_OK;
    CHECK_TRUE(Init(*setup) == WEBRTC_VIDEO_CODEC_OK);
    CHECK_TRUE(setup->av1.inits == 1);
    CHECK_TRUE(setup->failures == 1);
}

void TestEncodeFailureStopsTheStream() {
    auto setup = MakeSetup();
    CHECK_TRUE(Init(*setup) == WEBRTC_VIDEO_CODEC_OK);
    setup->encoder->RegisterEncodeCompleteCallback(&setup->callback);
    CHECK_TRUE(setup->av1.callback == &setup->callback);
    SetRate(*setup, 800000);
    CHECK_TRUE(EncodeDelta(*setup) == WEBRTC_VIDEO_CODEC_OK);
    CHECK_TRUE(setup->av1.encodes == 1);

    setup->av1.encodeResult = WEBRTC_VIDEO_CODEC_ERROR;
    CHECK_TRUE(EncodeDelta(*setup) == WEBRTC_VIDEO_CODEC_OK);
    CHECK_TRUE(setup->failures == 1);
    CHECK_TRUE(setup->av1.releases >= 1);

    // For good: AV1 is not tried again, even if it would now work, and rates no longer reach it.
    setup->av1.encodeResult = WEBRTC_VIDEO_CODEC_OK;
    const int rateCalls = setup->av1.rateCalls;
    SetRate(*setup, 900000);
    CHECK_TRUE(EncodeDelta(*setup) == WEBRTC_VIDEO_CODEC_OK);
    CHECK_TRUE(setup->av1.encodes == 2);
    CHECK_TRUE(setup->av1.rateCalls == rateCalls);
    CHECK_TRUE(setup->failures == 1);
}

// The failure released the AV1 encoder; WebRTC's own Release() later (the stream is reconfigured or torn down) must not
// release it a second time.
void TestReleaseAfterFailureReleasesTheEncoderOnce() {
    auto setup = MakeSetup();
    CHECK_TRUE(Init(*setup) == WEBRTC_VIDEO_CODEC_OK);
    setup->encoder->RegisterEncodeCompleteCallback(&setup->callback);
    SetRate(*setup, 800000);
    setup->av1.encodeResult = WEBRTC_VIDEO_CODEC_ERROR;
    CHECK_TRUE(EncodeDelta(*setup) == WEBRTC_VIDEO_CODEC_OK);
    CHECK_TRUE(setup->failures == 1);

    CHECK_TRUE(setup->encoder->Release() == WEBRTC_VIDEO_CODEC_OK);
    CHECK_TRUE(setup->av1.releases == 1);
}

// Single and Every: AV1 is the sender's codec, and WebRTC switches it (the factory's encoder selector).
void TestFailureWithoutCallbackGoesToWebRtc() {
    auto initFails = MakeSetup(true);
    initFails->av1.initResult = WEBRTC_VIDEO_CODEC_ERROR;
    CHECK_TRUE(Init(*initFails) == WEBRTC_VIDEO_CODEC_ERROR);

    auto encodeFails = MakeSetup(true);
    CHECK_TRUE(Init(*encodeFails) == WEBRTC_VIDEO_CODEC_OK);
    encodeFails->av1.encodeResult = WEBRTC_VIDEO_CODEC_ERROR;
    CHECK_TRUE(EncodeDelta(*encodeFails) == WEBRTC_VIDEO_CODEC_ERROR);
}

void TestZeroRateIsNotAFailure() {
    auto setup = MakeSetup();
    CHECK_TRUE(Init(*setup) == WEBRTC_VIDEO_CODEC_OK);
    SetRate(*setup, 0);
    SetRate(*setup, 900000);
    CHECK_TRUE(setup->av1.rateCalls == 2 && setup->av1.lastRateBps == 900000);
    CHECK_TRUE(EncodeDelta(*setup) == WEBRTC_VIDEO_CODEC_OK);
    CHECK_TRUE(setup->av1.encodes == 1);
    CHECK_TRUE(setup->failures == 0);
}

// The AV1 top layer runs at most 30 fps (software SVT-AV1) whatever the stream's rate. A keyframe request is never
// dropped.
void TestAv1RunsAtMost30Fps() {
    auto setup = MakeSetup();
    webrtc::VideoCodec codec = H264Settings();
    codec.maxFramerate = 60;
    CHECK_TRUE(setup->encoder->InitEncode(&codec, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)) == WEBRTC_VIDEO_CODEC_OK);
    CHECK_TRUE(setup->av1.initMaxFramerate <= 30);
    webrtc::VideoBitrateAllocation allocation;
    allocation.SetBitrate(0, 0, 1500000);
    setup->encoder->SetRates(webrtc::VideoEncoder::RateControlParameters(allocation, 60.0));
    CHECK_TRUE(setup->av1.lastFramerate > 0.0 && setup->av1.lastFramerate <= 30.0);

    const std::vector<webrtc::VideoFrameType> delta = {webrtc::VideoFrameType::kVideoFrameDelta};
    const std::vector<webrtc::VideoFrameType> key = {webrtc::VideoFrameType::kVideoFrameKey};
    int64_t timestampUs = 1000000;
    for (int i = 0; i < 60; i++) {
        CHECK_TRUE(setup->encoder->Encode(Frame(timestampUs), &delta) == WEBRTC_VIDEO_CODEC_OK);
        timestampUs += 1000000 / 60;
    }
    std::printf("60 fps in: %d AV1 frames encoded\n", setup->av1.encodes);
    CHECK_TRUE(setup->av1.encodes >= 28 && setup->av1.encodes <= 32);

    // A keyframe request right after an encoded frame is still encoded.
    CHECK_TRUE(setup->encoder->Encode(Frame(timestampUs), &key) == WEBRTC_VIDEO_CODEC_OK);
    timestampUs += 1000;
    const int before = setup->av1.keyframesAsked;
    CHECK_TRUE(setup->encoder->Encode(Frame(timestampUs), &key) == WEBRTC_VIDEO_CODEC_OK);
    CHECK_TRUE(setup->av1.keyframesAsked == before + 1);

}

} // namespace

int main() {
    TestAv1GetsAv1Settings();
    TestInitFailureStopsTheStream();
    TestEncodeFailureStopsTheStream();
    TestReleaseAfterFailureReleasesTheEncoderOnce();
    TestFailureWithoutCallbackGoesToWebRtc();
    TestZeroRateIsNotAFailure();
    TestAv1RunsAtMost30Fps();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("av1_top_stream_encoder_test: all checks passed\n");
    return 0;
}
