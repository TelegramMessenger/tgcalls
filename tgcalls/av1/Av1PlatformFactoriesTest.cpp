// AV1 reaches only the factories an engine asks for (the fake platform here), last in their lists; a device not
// allowed to encode AV1 gets a refusing encoder; the chip gate.

#include "av1/Av1DeviceGate.h"
#include "platform/fake/FakeInterface.h"

#include "api/environment/environment_factory.h"
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
    TestCpuFamilyGate();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all AV1 platform factory checks passed\n");
    return 0;
}
