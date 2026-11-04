// AV1 reaches only the factories an engine asks for (the fake platform here), last in their lists; a device not
// allowed to encode AV1 gets a refusing encoder; the chip gate.

#include "av1/Av1DeviceGate.h"
#include "av1/Av1VideoCodecFactories.h"
#include "platform/fake/FakeInterface.h"

#include "api/environment/environment_factory.h"
#include "api/jsep.h"
#include "pc/session_description.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "modules/video_coding/include/video_error_codes.h"

#include <cstdio>
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

std::vector<std::string> Names(const std::vector<webrtc::SdpVideoFormat> &formats) {
    std::vector<std::string> names;
    for (const auto &format : formats) {
        names.push_back(format.name);
    }
    return names;
}

const std::vector<std::string> kTodayNames = {"H264", "H264", "VP8", "VP9"};

std::string Param(const webrtc::CodecParameterMap &parameters, const char *key) {
    const auto it = parameters.find(key);
    return it == parameters.end() ? std::string() : it->second;
}

int InitAv1(webrtc::VideoEncoder &encoder) {
    webrtc::VideoCodec codec;
    codec.codecType = webrtc::kVideoCodecAV1;
    codec.width = 640;
    codec.height = 360;
    codec.maxFramerate = 30;
    codec.startBitrate = 600;
    codec.maxBitrate = 2500;
    codec.qpMax = 63;
    codec.SetScalabilityMode(webrtc::ScalabilityMode::kL1T1);
    return encoder.InitEncode(&codec, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 2, 1200));
}

void TestPlainFactoriesHaveNoAv1() {
    tgcalls::FakeInterface platform;
    CHECK_TRUE(Names(platform.makeVideoEncoderFactory(true, false)->GetSupportedFormats()) == kTodayNames);
    CHECK_TRUE(Names(platform.makeVideoDecoderFactory()->GetSupportedFormats()) == kTodayNames);
}

void TestAv1FactoriesAppendAv1Last() {
    tgcalls::FakeInterface platform;
    std::vector<std::string> expected = kTodayNames;
    expected.push_back("AV1");
    CHECK_TRUE(Names(platform.makeVideoEncoderFactoryWithAv1(true, true)->GetSupportedFormats()) == expected);
    CHECK_TRUE(Names(platform.makeVideoEncoderFactoryWithAv1(true, false)->GetSupportedFormats()) == expected);
    CHECK_TRUE(Names(platform.makeVideoDecoderFactoryWithAv1()->GetSupportedFormats()) == expected);
}

void TestEncodingNotAllowedGivesRefusingEncoder() {
    tgcalls::FakeInterface platform;
    auto refusing = platform.makeVideoEncoderFactoryWithAv1(true, false)->CreateVideoEncoder(webrtc::SdpVideoFormat("AV1"));
    CHECK_TRUE(refusing != nullptr);
    if (refusing) {
        CHECK_TRUE(InitAv1(*refusing) == WEBRTC_VIDEO_CODEC_ERROR);
    }
    auto encoding = platform.makeVideoEncoderFactoryWithAv1(true, true)->CreateVideoEncoder(webrtc::SdpVideoFormat("AV1"));
    CHECK_TRUE(encoding != nullptr);
    if (encoding) {
        CHECK_TRUE(InitAv1(*encoding) == WEBRTC_VIDEO_CODEC_OK);
        encoding->Release();
    }
    // The other codecs still come from the inner factory.
    auto vp8 = platform.makeVideoEncoderFactoryWithAv1(true, false)->CreateVideoEncoder(webrtc::SdpVideoFormat("VP8"));
    CHECK_TRUE(vp8 != nullptr);
}

void TestDecoderFactoryCreatesAv1Decoder() {
    tgcalls::FakeInterface platform;
    auto factory = platform.makeVideoDecoderFactoryWithAv1();
    auto decoder = factory->Create(webrtc::CreateEnvironment(), webrtc::SdpVideoFormat("AV1"));
    CHECK_TRUE(decoder != nullptr);
    if (decoder) {
        webrtc::VideoDecoder::Settings settings;
        settings.set_codec_type(webrtc::kVideoCodecAV1);
        settings.set_number_of_cores(2);
        CHECK_TRUE(decoder->Configure(settings));
    }
}

// The defaults are the ones WebRTC's own SDP parser fills in: parse a description that omits them and compare.
void TestSdpDefaultsMatchTheParser() {
    const char *sdp =
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96 98 100 102 35\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:0\r\n"
        "a=sendrecv\r\n"
        "a=rtcp-mux\r\n"
        "a=rtpmap:96 VP8/90000\r\n"
        "a=rtpmap:98 VP9/90000\r\n"
        "a=rtpmap:100 H264/90000\r\n"
        "a=fmtp:100 profile-level-id=42e01f\r\n"
        "a=rtpmap:102 H265/90000\r\n"
        "a=rtpmap:35 AV1/90000\r\n";
    webrtc::SdpParseError error;
    auto description = webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error);
    CHECK_TRUE(description != nullptr);
    if (!description) {
        std::fprintf(stderr, "SDP parse error: %s\n", error.description.c_str());
        return;
    }
    const auto &contents = description->description()->contents();
    CHECK_TRUE(contents.size() == 1);
    if (contents.size() != 1) {
        return;
    }
    int compared = 0;
    for (const auto &codec : contents[0].media_description()->codecs()) {
        webrtc::CodecParameterMap written;
        if (codec.name == "H264") {
            written["profile-level-id"] = "42e01f";
        }
        const auto expected = tgcalls::WithSdpDefaultParameters(webrtc::SdpVideoFormat(codec.name, written));
        CHECK_TRUE(expected.parameters == codec.params);
        compared++;
    }
    CHECK_TRUE(compared == 5);

    // A value already present is kept.
    const auto explicitLevel = tgcalls::WithSdpDefaultParameters(webrtc::SdpVideoFormat("AV1", {{"level-idx", "8"}}));
    CHECK_TRUE(Param(explicitLevel.parameters, "level-idx") == "8");
    CHECK_TRUE(Param(explicitLevel.parameters, "profile") == "0");
}

void TestAv1FactoriesListSdpDefaults() {
    tgcalls::FakeInterface platform;
    for (const auto &formats : {
        platform.makeVideoEncoderFactoryWithAv1(true, true)->GetSupportedFormats(),
        platform.makeVideoDecoderFactoryWithAv1()->GetSupportedFormats(),
    }) {
        for (const auto &format : formats) {
            CHECK_TRUE(format.parameters == tgcalls::WithSdpDefaultParameters(format).parameters);
        }
        CHECK_TRUE(!formats.empty() && formats.back().name == "AV1");
        if (!formats.empty()) {
            const webrtc::CodecParameterMap av1 = {{"profile", "0"}, {"level-idx", "5"}, {"tier", "0"}};
            CHECK_TRUE(formats.back().parameters == av1);
        }
    }
}

// An inner factory that lists H265 bare, as Darwin's does, and records the formats it is asked about.
class RecordingEncoderFactory : public webrtc::VideoEncoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return {webrtc::SdpVideoFormat("H265")};
    }
    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat &format,
                                   absl::optional<std::string> scalabilityMode) const override {
        queried.push_back(format);
        return CodecSupport();
    }
    std::unique_ptr<webrtc::VideoEncoder> CreateVideoEncoder(const webrtc::SdpVideoFormat &format) override {
        created.push_back(format);
        return nullptr;
    }
    mutable std::vector<webrtc::SdpVideoFormat> queried;
    std::vector<webrtc::SdpVideoFormat> created;
};

void TestInnerFactoryGetsItsOwnFormat() {
    auto inner = std::make_unique<RecordingEncoderFactory>();
    auto *recorder = inner.get();
    auto factory = tgcalls::WrapEncoderFactoryWithAv1(std::move(inner), true);
    const auto listed = factory->GetSupportedFormats();
    CHECK_TRUE(listed.size() == 2 && listed[0].name == "H265");
    if (listed.size() != 2) {
        return;
    }
    CHECK_TRUE(Param(listed[0].parameters, "level-id") == "93");
    factory->CreateVideoEncoder(listed[0]);
    factory->QueryCodecSupport(listed[0], absl::nullopt);
    CHECK_TRUE(recorder->created.size() == 1 && recorder->created[0].parameters.empty());
    CHECK_TRUE(recorder->queried.size() == 1 && recorder->queried[0].parameters.empty());
}

void TestCpuFamilyGate() {
    // Before the A18 / M4 generation: no AV1 encoding.
    const uint32_t older[] = {
        0x1e2d6381, // SWIFT
        0x37a09642, // CYCLONE
        0x2c91a47e, // TYPHOON
        0x92fb37c8, // TWISTER
        0x67ceee93, // HURRICANE
        0xe81e7ef6, // MONSOON_MISTRAL
        0x07d34b9f, // VORTEX_TEMPEST
        0x462504d2, // LIGHTNING_THUNDER
        0x1b588bb3, // FIRESTORM_ICESTORM
        0xda33d83d, // BLIZZARD_AVALANCHE
        0x8765edea, // EVEREST_SAWTOOTH
        0xfa33415e, // IBIZA (M3)
        0x72015832, // PALMA (M3 Max)
        0x2876f5b5, // COLL (A17 Pro)
        0x5f4dea93, // LOBOS (M3 Pro)
    };
    for (uint32_t family : older) {
        CHECK_TRUE(!tgcalls::IsAv1EncodeCapableCpuFamily(family));
    }
    const uint32_t newer[] = {
        0x6f5129ac, // DONAN (M4)
        0x17d5b93a, // BRAVA (M4 Pro / Max)
        0x75d4acb9, // TAHITI (A18 Pro)
        0x204526d0, // TUPAI (A18)
        0xf76c5b1a, // SOTRA
        0x12345678, // unknown: a later chip
    };
    for (uint32_t family : newer) {
        CHECK_TRUE(tgcalls::IsAv1EncodeCapableCpuFamily(family));
    }
    std::printf("this machine %s encode AV1 in real time by the gate\n",
                tgcalls::CurrentCpuCanEncodeAv1Realtime() ? "may" : "may not");
}

} // namespace

int main() {
    TestPlainFactoriesHaveNoAv1();
    TestAv1FactoriesAppendAv1Last();
    TestEncodingNotAllowedGivesRefusingEncoder();
    TestDecoderFactoryCreatesAv1Decoder();
    TestSdpDefaultsMatchTheParser();
    TestAv1FactoriesListSdpDefaults();
    TestInnerFactoryGetsItsOwnFormat();
    TestCpuFamilyGate();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all AV1 platform factory checks passed\n");
    return 0;
}
