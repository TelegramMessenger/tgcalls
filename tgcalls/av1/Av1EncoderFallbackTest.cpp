// When the AV1 encoder fails, WebRTC switches to the codec the engine would pin next (H.265, else H.264) instead of
// its default VP8, and the engine hears of it so that it stops pinning AV1.

#include "av1/Av1EncoderFallback.h"

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
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

struct Behaviour {
    int32_t initResult = WEBRTC_VIDEO_CODEC_OK;
    int32_t encodeResult = WEBRTC_VIDEO_CODEC_OK;
};

class FakeEncoder final : public webrtc::VideoEncoder {
public:
    explicit FakeEncoder(Behaviour behaviour) : _behaviour(behaviour) {}

    int32_t InitEncode(const webrtc::VideoCodec *, const Settings &) override {
        return _behaviour.initResult;
    }
    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback *) override {
        return WEBRTC_VIDEO_CODEC_OK;
    }
    int32_t Release() override {
        return WEBRTC_VIDEO_CODEC_OK;
    }
    int32_t Encode(const webrtc::VideoFrame &, const std::vector<webrtc::VideoFrameType> *) override {
        return _behaviour.encodeResult;
    }
    void SetRates(const RateControlParameters &) override {
    }
    EncoderInfo GetEncoderInfo() const override {
        EncoderInfo info;
        info.implementation_name = "Fake";
        return info;
    }

private:
    Behaviour _behaviour;
};

// Creates AV1 and H264 encoders behaving as told; `createAv1` false makes AV1 creation fail.
class FakeFactory final : public webrtc::VideoEncoderFactory {
public:
    Behaviour av1;
    Behaviour h264;
    bool createAv1 = true;

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return {webrtc::SdpVideoFormat("H264"), webrtc::SdpVideoFormat("AV1")};
    }
    std::unique_ptr<webrtc::VideoEncoder> CreateVideoEncoder(const webrtc::SdpVideoFormat &format) override {
        if (format.name == "AV1") {
            return createAv1 ? std::make_unique<FakeEncoder>(av1) : nullptr;
        }
        return std::make_unique<FakeEncoder>(h264);
    }
};

struct Setup {
    FakeFactory *inner = nullptr;
    std::shared_ptr<tgcalls::Av1EncoderFallback> fallback;
    std::unique_ptr<webrtc::VideoEncoderFactory> factory;
    std::unique_ptr<webrtc::VideoEncoderFactory::EncoderSelectorInterface> selector;
    int reports = 0;
};

std::unique_ptr<Setup> MakeSetup() {
    auto setup = std::make_unique<Setup>();
    auto inner = std::make_unique<FakeFactory>();
    setup->inner = inner.get();
    Setup *raw = setup.get();
    setup->fallback = std::make_shared<tgcalls::Av1EncoderFallback>([raw] {
        raw->reports++;
    });
    setup->factory = tgcalls::WrapEncoderFactoryWithAv1Fallback(std::move(inner), setup->fallback);
    setup->selector = setup->factory->GetEncoderSelector();
    return setup;
}

webrtc::SdpVideoFormat H265() {
    return webrtc::SdpVideoFormat("H265", {{"level-id", "93"}, {"tx-mode", "SRST"}});
}

int32_t Init(webrtc::VideoEncoder &encoder) {
    webrtc::VideoCodec codec;
    codec.width = 640;
    codec.height = 360;
    return encoder.InitEncode(&codec, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200));
}

int32_t EncodeOne(webrtc::VideoEncoder &encoder) {
    webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
        .set_video_frame_buffer(webrtc::I420Buffer::Create(640, 360))
        .set_timestamp_us(1)
        .build();
    return encoder.Encode(frame, nullptr);
}

void TestAv1InitFailureReportsAndSwitchesToTheFallback() {
    auto setup = MakeSetup();
    setup->inner->av1.initResult = WEBRTC_VIDEO_CODEC_ERROR;
    setup->fallback->setFallbackFormat(H265());
    CHECK_TRUE(setup->selector != nullptr);
    auto encoder = setup->factory->CreateVideoEncoder(webrtc::SdpVideoFormat("AV1"));
    CHECK_TRUE(encoder != nullptr);
    if (!encoder || !setup->selector) {
        return;
    }
    setup->selector->OnCurrentEncoder(webrtc::SdpVideoFormat("AV1"));
    CHECK_TRUE(Init(*encoder) == WEBRTC_VIDEO_CODEC_ERROR);
    CHECK_TRUE(setup->reports == 1);
    const auto broken = setup->selector->OnEncoderBroken();
    CHECK_TRUE(broken.has_value() && *broken == H265());
}

void TestAv1EncodeFailureReports() {
    auto setup = MakeSetup();
    setup->inner->av1.encodeResult = WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
    setup->fallback->setFallbackFormat(H265());
    auto encoder = setup->factory->CreateVideoEncoder(webrtc::SdpVideoFormat("AV1"));
    if (!encoder || !setup->selector) {
        CHECK_TRUE(false);
        return;
    }
    setup->selector->OnCurrentEncoder(webrtc::SdpVideoFormat("AV1"));
    CHECK_TRUE(Init(*encoder) == WEBRTC_VIDEO_CODEC_OK);
    CHECK_TRUE(setup->reports == 0);
    CHECK_TRUE(EncodeOne(*encoder) == WEBRTC_VIDEO_CODEC_ENCODER_FAILURE);
    CHECK_TRUE(setup->reports == 1);
    const auto broken = setup->selector->OnEncoderBroken();
    CHECK_TRUE(broken.has_value() && *broken == H265());
}

void TestAv1CreationFailureReports() {
    // WebRTC never tells the selector about an encoder it could not create.
    auto setup = MakeSetup();
    setup->inner->createAv1 = false;
    setup->fallback->setFallbackFormat(H265());
    CHECK_TRUE(setup->factory->CreateVideoEncoder(webrtc::SdpVideoFormat("AV1")) == nullptr);
    CHECK_TRUE(setup->reports == 1);
    if (!setup->selector) {
        CHECK_TRUE(false);
        return;
    }
    const auto broken = setup->selector->OnEncoderBroken();
    CHECK_TRUE(broken.has_value() && *broken == H265());
}

void TestAnotherBrokenEncoderKeepsWebRtcsChoice() {
    auto setup = MakeSetup();
    setup->inner->h264.initResult = WEBRTC_VIDEO_CODEC_ERROR;
    setup->fallback->setFallbackFormat(H265());
    auto encoder = setup->factory->CreateVideoEncoder(webrtc::SdpVideoFormat("H264"));
    if (!encoder || !setup->selector) {
        CHECK_TRUE(false);
        return;
    }
    setup->selector->OnCurrentEncoder(webrtc::SdpVideoFormat("H264"));
    CHECK_TRUE(Init(*encoder) == WEBRTC_VIDEO_CODEC_ERROR);
    CHECK_TRUE(setup->reports == 0);
    CHECK_TRUE(!setup->selector->OnEncoderBroken().has_value());
}

void TestNoFallbackBeforeTheFirstNegotiation() {
    auto setup = MakeSetup();
    setup->inner->av1.initResult = WEBRTC_VIDEO_CODEC_ERROR;
    auto encoder = setup->factory->CreateVideoEncoder(webrtc::SdpVideoFormat("AV1"));
    if (!encoder || !setup->selector) {
        CHECK_TRUE(false);
        return;
    }
    setup->selector->OnCurrentEncoder(webrtc::SdpVideoFormat("AV1"));
    CHECK_TRUE(Init(*encoder) == WEBRTC_VIDEO_CODEC_ERROR);
    CHECK_TRUE(setup->reports == 1);
    CHECK_TRUE(!setup->selector->OnEncoderBroken().has_value());
}

void TestAWorkingAv1EncoderNeitherReportsNorSwitches() {
    auto setup = MakeSetup();
    setup->fallback->setFallbackFormat(H265());
    auto encoder = setup->factory->CreateVideoEncoder(webrtc::SdpVideoFormat("AV1"));
    if (!encoder || !setup->selector) {
        CHECK_TRUE(false);
        return;
    }
    setup->selector->OnCurrentEncoder(webrtc::SdpVideoFormat("AV1"));
    CHECK_TRUE(Init(*encoder) == WEBRTC_VIDEO_CODEC_OK);
    CHECK_TRUE(EncodeOne(*encoder) == WEBRTC_VIDEO_CODEC_OK);
    CHECK_TRUE(setup->reports == 0);
    CHECK_TRUE(!setup->selector->OnAvailableBitrate(webrtc::DataRate::KilobitsPerSec(300)).has_value());
    CHECK_TRUE(encoder->GetEncoderInfo().implementation_name == "Fake");
}

} // namespace

int main() {
    TestAv1InitFailureReportsAndSwitchesToTheFallback();
    TestAv1EncodeFailureReports();
    TestAv1CreationFailureReports();
    TestAnotherBrokenEncoderKeepsWebRtcsChoice();
    TestNoFallbackBeforeTheFirstNegotiation();
    TestAWorkingAv1EncoderNeitherReportsNorSwitches();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all AV1 encoder fallback checks passed\n");
    return 0;
}
