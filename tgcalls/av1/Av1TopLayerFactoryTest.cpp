// The group screen share's encoder factory with an AV1 top layer (the fake platform here, as the CLI runs it): it lists
// AV1 for negotiation, keeps today's encoder while the switch is off, and with the switch on encodes the top simulcast
// stream with SVT-AV1 and the streams below with H264, through tgcalls' simulcast adapter. Real encoders (OpenH264,
// SVT-AV1).

#include "av1/Av1TopLayerFactory.h"
#include "group/GroupVideoPayloadTypes.h"
#include "platform/fake/FakeInterface.h"

#include "api/environment/environment_factory.h"
#include "api/units/data_rate.h"
#include "api/video/encoded_image.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_bitrate_allocation.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/scalability_mode.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "media/base/media_constants.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"

#include <algorithm>
#include <cstdio>
#include <map>
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

struct StreamImages {
    int images = 0;
    int av1 = 0;
    int h264 = 0;
    int keyframes = 0;
    webrtc::VideoFrameType lastType = webrtc::VideoFrameType::kEmptyFrame;
    int lastWidth = 0;
    int lastHeight = 0;
};

class Collector final : public webrtc::EncodedImageCallback {
public:
    void OnFrameDropped(uint32_t, int, bool) override {}

    std::map<int, StreamImages> streams;

    Result OnEncodedImage(const webrtc::EncodedImage &image, const webrtc::CodecSpecificInfo *info) override {
        auto &stream = streams[image.SimulcastIndex().value_or(0)];
        stream.images++;
        if (info && info->codecType == webrtc::kVideoCodecAV1) {
            stream.av1++;
        } else if (info && info->codecType == webrtc::kVideoCodecH264) {
            stream.h264++;
        }
        if (image._frameType == webrtc::VideoFrameType::kVideoFrameKey) {
            stream.keyframes++;
        }
        stream.lastType = image._frameType;
        stream.lastWidth = image._encodedWidth;
        stream.lastHeight = image._encodedHeight;
        return Result(Result::OK);
    }
};

webrtc::SdpVideoFormat H264() {
    return webrtc::SdpVideoFormat(webrtc::kH264CodecName, {
        {"level-asymmetry-allowed", "1"},
        {"packetization-mode", "1"},
        {"profile-level-id", "42e01f"},
    });
}

// H264 simulcast as WebRTC hands it to the encoder: `streams` streams, the top one `width`x`height` at most `topFps`,
// each lower one half the size of the next, at up to 60 fps.
webrtc::VideoCodec Simulcast(int streams, int width, int height, int topFps) {
    webrtc::VideoCodec codec;
    codec.codecType = webrtc::kVideoCodecH264;
    codec.width = width;
    codec.height = height;
    codec.maxFramerate = 60;
    codec.startBitrate = 1000;
    codec.maxBitrate = 3000;
    codec.minBitrate = 30;
    codec.qpMax = 51;
    codec.numberOfSimulcastStreams = streams;
    for (int i = 0; i < streams; i++) {
        const int divisor = 1 << (streams - 1 - i);
        auto &stream = codec.simulcastStream[i];
        stream.width = width / divisor;
        stream.height = height / divisor;
        stream.maxFramerate = (i == streams - 1) ? topFps : 60;
        stream.numberOfTemporalLayers = 1;
        stream.maxBitrate = (i == streams - 1) ? 2000 : 300;
        stream.targetBitrate = (i == streams - 1) ? 1500 : 200;
        stream.minBitrate = 30;
        stream.qpMax = 51;
        stream.active = true;
    }
    return codec;
}

webrtc::VideoEncoder::Settings EncoderSettings() {
    return webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200);
}

void SetRates(webrtc::VideoEncoder &encoder, int streams, double fps) {
    webrtc::VideoBitrateAllocation allocation;
    for (int i = 0; i < streams; i++) {
        allocation.SetBitrate(i, 0, (i == streams - 1) ? 1500000 : 200000);
    }
    encoder.SetRates(webrtc::VideoEncoder::RateControlParameters(allocation, fps));
}

struct Clock {
    int64_t timestampUs = 1000000;
    uint32_t rtpTimestamp = 90000;
};

int32_t EncodeFrame(webrtc::VideoEncoder &encoder, Clock &clock, int width, int height, int fps,
                    std::vector<webrtc::VideoFrameType> types) {
    auto buffer = webrtc::I420Buffer::Create(width, height);
    webrtc::I420Buffer::SetBlack(buffer.get());
    const auto frame = webrtc::VideoFrame::Builder()
        .set_video_frame_buffer(buffer)
        .set_timestamp_us(clock.timestampUs)
        .set_timestamp_rtp(clock.rtpTimestamp)
        .build();
    clock.timestampUs += 1000000 / fps;
    clock.rtpTimestamp += 90000 / fps;
    return encoder.Encode(frame, &types);
}

struct Setup {
    tgcalls::FakeInterface platform;
    std::shared_ptr<tgcalls::Av1TopLayerSwitch> av1TopLayer = std::make_shared<tgcalls::Av1TopLayerSwitch>();
    std::unique_ptr<webrtc::VideoEncoderFactory> factory;
    int fallbacks = 0;
};

std::unique_ptr<Setup> MakeSetup(bool enabled) {
    auto setup = std::make_unique<Setup>();
    setup->av1TopLayer->setEnabled(enabled);
    Setup *raw = setup.get();
    setup->av1TopLayer->setOnFallback([raw] {
        raw->fallbacks++;
    });
    setup->factory = setup->platform.makeScreencastVideoEncoderFactoryWithAv1TopLayer(true, setup->av1TopLayer);
    return setup;
}

void TestFormatsListAv1() {
    auto setup = MakeSetup(false);
    int av1 = 0;
    for (const auto &format : setup->factory->GetSupportedFormats()) {
        if (format.IsSameCodec(tgcalls::groupAv1VideoFormat()) && format.parameters == tgcalls::groupAv1VideoFormat().parameters) {
            av1++;
        }
    }
    CHECK_TRUE(av1 == 1);
    int implementations = 0;
    for (const auto &format : setup->factory->GetImplementations()) {
        if (format.IsSameCodec(tgcalls::groupAv1VideoFormat())) {
            implementations++;
        }
    }
    CHECK_TRUE(implementations == 1);
}

void TestSwitchOffIsToday() {
    auto setup = MakeSetup(false);
    auto today = setup->platform.makeVideoEncoderFactory(true, true);
    const auto encoder = setup->factory->Create(webrtc::CreateEnvironment(), H264());
    const auto todays = today->Create(webrtc::CreateEnvironment(), H264());
    CHECK_TRUE(encoder && todays);
    if (encoder && todays) {
        CHECK_TRUE(encoder->GetEncoderInfo().implementation_name == todays->GetEncoderInfo().implementation_name);
    }
}

void TestTwoStreamsTopIsAv1() {
    auto setup = MakeSetup(true);
    auto encoder = setup->factory->Create(webrtc::CreateEnvironment(), H264());
    Collector collector;
    const auto codec = Simulcast(2, 1280, 720, 30);
    CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    encoder->RegisterEncodeCompleteCallback(&collector);
    SetRates(*encoder, 2, 60.0);
    Clock clock;
    for (int i = 0; i < 20; i++) {
        const auto type = i == 0 ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
        EncodeFrame(*encoder, clock, 1280, 720, 60, {type, type});
    }
    encoder->Release();
    const auto &low = collector.streams[0];
    const auto &top = collector.streams[1];
    std::printf("two streams: low %d images (H264 %d), top %d images (AV1 %d)\n", low.images, low.h264, top.images, top.av1);
    CHECK_TRUE(low.images >= 18 && low.h264 == low.images && low.av1 == 0);
    CHECK_TRUE(top.images >= 8 && top.images <= 11);
    CHECK_TRUE(top.av1 == top.images && top.h264 == 0);
    CHECK_TRUE(top.lastWidth == 1280 && top.lastHeight == 720);
    CHECK_TRUE(setup->fallbacks == 0);
}

// Every layer in AV1 (GroupAv1ScreencastLayers::Every): AV1 is the sender's codec, WebRTC's AV1 simulcast (a
// scalability mode on each encoding), and every stream an AV1 encoder of its own, each at most 30 fps.
void TestEveryStreamIsAv1() {
    auto setup = MakeSetup(true);
    setup->av1TopLayer->setLayers(tgcalls::GroupAv1ScreencastLayers::Every);
    auto encoder = setup->factory->Create(webrtc::CreateEnvironment(), tgcalls::groupAv1VideoFormat());
    CHECK_TRUE(encoder != nullptr);
    if (!encoder) {
        return;
    }
    Collector collector;
    auto codec = Simulcast(2, 1280, 720, 30);
    codec.codecType = webrtc::kVideoCodecAV1;
    CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    encoder->RegisterEncodeCompleteCallback(&collector);
    SetRates(*encoder, 2, 60.0);
    Clock clock;
    for (int i = 0; i < 20; i++) {
        const auto type = i == 0 ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
        EncodeFrame(*encoder, clock, 1280, 720, 60, {type, type});
    }
    encoder->Release();
    const auto &low = collector.streams[0];
    const auto &top = collector.streams[1];
    std::printf("every stream: low %d images (AV1 %d), top %d images (AV1 %d)\n", low.images, low.av1, top.images, top.av1);
    CHECK_TRUE(low.images >= 8 && low.images <= 11);
    CHECK_TRUE(low.av1 == low.images && low.h264 == 0);
    CHECK_TRUE(low.lastWidth == 640 && low.lastHeight == 360);
    CHECK_TRUE(top.images >= 8 && top.images <= 11);
    CHECK_TRUE(top.av1 == top.images && top.h264 == 0);
    CHECK_TRUE(setup->fallbacks == 0);
}

// WebRTC replaces a scalability mode the encoder factory does not report as supported (QueryCodecSupport) by its
// default, L1T2: AV1 supports L1T1 and L1T3, what SvtAv1Encoder encodes, and nothing else.
void TestAv1SupportsL1T1AndL1T3() {
    auto setup = MakeSetup(true);
    setup->av1TopLayer->setLayers(tgcalls::GroupAv1ScreencastLayers::Every);
    CHECK_TRUE(setup->factory->QueryCodecSupport(tgcalls::groupAv1VideoFormat(), std::string("L1T1")).is_supported);
    CHECK_TRUE(setup->factory->QueryCodecSupport(tgcalls::groupAv1VideoFormat(), std::string("L1T3")).is_supported);
    CHECK_TRUE(!setup->factory->QueryCodecSupport(tgcalls::groupAv1VideoFormat(), std::string("L1T2")).is_supported);
    CHECK_TRUE(!setup->factory->QueryCodecSupport(tgcalls::groupAv1VideoFormat(), std::string("L2T1")).is_supported);
    CHECK_TRUE(setup->factory->QueryCodecSupport(tgcalls::groupAv1VideoFormat(), std::nullopt).is_supported);
}

// The AV1 format the factory lists names L1T1 and L1T3: PeerConnection's sender (GroupInstanceReferenceImpl) checks a
// scalability mode against its codec preferences, the engine's send codecs built from these formats.
void TestFormatsListAv1ScalabilityModes() {
    auto setup = MakeSetup(false);
    bool found = false;
    for (const auto &format : setup->factory->GetSupportedFormats()) {
        if (!format.IsSameCodec(tgcalls::groupAv1VideoFormat())) {
            continue;
        }
        found = true;
        const auto &modes = format.scalability_modes;
        CHECK_TRUE(std::find(modes.begin(), modes.end(), webrtc::ScalabilityMode::kL1T1) != modes.end());
        CHECK_TRUE(std::find(modes.begin(), modes.end(), webrtc::ScalabilityMode::kL1T3) != modes.end());
        CHECK_TRUE(modes.size() == 2);
    }
    CHECK_TRUE(found);
}

// Every with H264 (the server did not list AV1, or AV1 failed): the platform's encoder, no AV1 stream.
void TestEveryH264IsToday() {
    auto setup = MakeSetup(true);
    setup->av1TopLayer->setLayers(tgcalls::GroupAv1ScreencastLayers::Every);
    auto encoder = setup->factory->Create(webrtc::CreateEnvironment(), H264());
    Collector collector;
    const auto codec = Simulcast(1, 1280, 720, 60);
    CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    encoder->RegisterEncodeCompleteCallback(&collector);
    SetRates(*encoder, 1, 30.0);
    Clock clock;
    for (int i = 0; i < 6; i++) {
        EncodeFrame(*encoder, clock, 1280, 720, 30, {i == 0 ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta});
    }
    encoder->Release();
    const auto &stream = collector.streams[0];
    CHECK_TRUE(stream.images >= 5);
    CHECK_TRUE(stream.h264 == stream.images && stream.av1 == 0);
}

// A failing AV1 encoder in Every reports the failure to WebRTC (which switches the sender to H264 through the
// selector): an AV1 sender's stream has no payload type for H264 images.
void TestEveryFailingAv1Surfaces() {
    auto setup = MakeSetup(true);
    setup->av1TopLayer->setLayers(tgcalls::GroupAv1ScreencastLayers::Every);
    setup->av1TopLayer->setTestAv1Fails(true);
    auto encoder = setup->factory->Create(webrtc::CreateEnvironment(), tgcalls::groupAv1VideoFormat());
    CHECK_TRUE(encoder != nullptr);
    if (encoder) {
        auto codec = Simulcast(2, 1280, 720, 30);
        codec.codecType = webrtc::kVideoCodecAV1;
        CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) != WEBRTC_VIDEO_CODEC_OK);
    }
    auto selector = setup->factory->GetEncoderSelector();
    CHECK_TRUE(selector != nullptr);
    if (selector) {
        selector->OnCurrentEncoder(tgcalls::groupAv1VideoFormat());
        const auto answer = selector->OnEncoderBroken();
        CHECK_TRUE(answer.has_value() && answer->name == webrtc::kH264CodecName);
        CHECK_TRUE(setup->fallbacks == 1);
    }
}

void TestOneStreamNoAv1() {
    auto setup = MakeSetup(true);
    auto encoder = setup->factory->Create(webrtc::CreateEnvironment(), H264());
    Collector collector;
    const auto codec = Simulcast(1, 1280, 720, 60);
    CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    encoder->RegisterEncodeCompleteCallback(&collector);
    SetRates(*encoder, 1, 30.0);
    Clock clock;
    for (int i = 0; i < 6; i++) {
        EncodeFrame(*encoder, clock, 1280, 720, 30, {i == 0 ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta});
    }
    encoder->Release();
    int images = 0;
    int av1 = 0;
    for (const auto &it : collector.streams) {
        images += it.second.images;
        av1 += it.second.av1;
    }
    CHECK_TRUE(images >= 5);
    CHECK_TRUE(av1 == 0);
}

void TestTopKeyframeRequestOnly() {
    auto setup = MakeSetup(true);
    auto encoder = setup->factory->Create(webrtc::CreateEnvironment(), H264());
    Collector collector;
    const auto codec = Simulcast(2, 1280, 720, 30);
    CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    encoder->RegisterEncodeCompleteCallback(&collector);
    SetRates(*encoder, 2, 30.0);
    Clock clock;
    const auto key = webrtc::VideoFrameType::kVideoFrameKey;
    const auto delta = webrtc::VideoFrameType::kVideoFrameDelta;
    EncodeFrame(*encoder, clock, 1280, 720, 30, {key, key});
    for (int i = 0; i < 3; i++) {
        EncodeFrame(*encoder, clock, 1280, 720, 30, {delta, delta});
    }
    const int lowKeyframes = collector.streams[0].keyframes;
    const int topKeyframes = collector.streams[1].keyframes;
    EncodeFrame(*encoder, clock, 1280, 720, 30, {delta, key});
    encoder->Release();
    CHECK_TRUE(collector.streams[1].keyframes == topKeyframes + 1);
    CHECK_TRUE(collector.streams[1].lastType == key);
    CHECK_TRUE(collector.streams[0].keyframes == lowKeyframes);
    CHECK_TRUE(collector.streams[0].lastType == delta);
}

void TestReinitAtNewSizeKeepsAv1() {
    auto setup = MakeSetup(true);
    auto encoder = setup->factory->Create(webrtc::CreateEnvironment(), H264());
    Collector collector;
    const auto first = Simulcast(2, 1280, 720, 30);
    CHECK_TRUE(encoder->InitEncode(&first, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    encoder->RegisterEncodeCompleteCallback(&collector);
    SetRates(*encoder, 2, 30.0);
    Clock clock;
    EncodeFrame(*encoder, clock, 1280, 720, 30, {webrtc::VideoFrameType::kVideoFrameKey, webrtc::VideoFrameType::kVideoFrameKey});

    // The phone rotated or unfolded: the same encoder is initialised again at a larger size.
    const auto second = Simulcast(2, 1536, 864, 30);
    CHECK_TRUE(encoder->InitEncode(&second, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    SetRates(*encoder, 2, 30.0);
    const int av1Before = collector.streams[1].av1;
    for (int i = 0; i < 4; i++) {
        const auto type = i == 0 ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
        EncodeFrame(*encoder, clock, 1536, 864, 30, {type, type});
    }
    encoder->Release();
    CHECK_TRUE(collector.streams[1].av1 >= av1Before + 3);
    CHECK_TRUE(collector.streams[1].h264 == 0);
    CHECK_TRUE(collector.streams[1].lastWidth == 1536 && collector.streams[1].lastHeight == 864);
    CHECK_TRUE(collector.streams[0].av1 == 0);
    CHECK_TRUE(setup->fallbacks == 0);
}

void TestAv1FormatIsRefused() {
    auto setup = MakeSetup(true);
    auto encoder = setup->factory->Create(webrtc::CreateEnvironment(), tgcalls::groupAv1VideoFormat());
    CHECK_TRUE(encoder != nullptr);
    if (encoder) {
        webrtc::VideoCodec codec = Simulcast(1, 1280, 720, 30);
        codec.codecType = webrtc::kVideoCodecAV1;
        CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) != WEBRTC_VIDEO_CODEC_OK);
    }
}

// WebRTC answers an encoder failure (RequestEncoderSwitch: Encode or InitEncode failed) with the selector's format,
// else VP8, else by dropping the first negotiated codec. With AV1 in use the sender's negotiated codecs are [H264, AV1]:
// without an answer an H264 error would make the whole sender AV1, which this factory refuses, and the share would go
// dark. The selector keeps the sender on the H264 it runs.
void TestBrokenEncoderStaysOnH264() {
    auto setup = MakeSetup(true);
    auto selector = setup->factory->GetEncoderSelector();
    CHECK_TRUE(selector != nullptr);
    if (!selector) {
        return;
    }
    auto encoder = setup->factory->Create(webrtc::CreateEnvironment(), H264());
    CHECK_TRUE(encoder != nullptr);
    const auto answer = selector->OnEncoderBroken();
    CHECK_TRUE(answer.has_value());
    if (answer) {
        CHECK_TRUE(answer->IsSameCodec(H264()));
    }
    CHECK_TRUE(!selector->OnAvailableBitrate(webrtc::DataRate::KilobitsPerSec(100)).has_value());

    // Told the current encoder without creating one (WebRTC reports it after creation): the same answer.
    auto other = MakeSetup(true);
    auto otherSelector = other->factory->GetEncoderSelector();
    CHECK_TRUE(otherSelector != nullptr);
    if (otherSelector) {
        otherSelector->OnCurrentEncoder(H264());
        const auto otherAnswer = otherSelector->OnEncoderBroken();
        CHECK_TRUE(otherAnswer.has_value() && otherAnswer->IsSameCodec(H264()));
    }
}

// Top's failing AV1 top stream sends nothing more and the engine is told (it then configures the stream for H264);
// the low stream goes on.
void TestFailingAv1StopsTheTopStream() {
    auto setup = MakeSetup(true);
    setup->av1TopLayer->setTestAv1Fails(true);
    auto encoder = setup->factory->Create(webrtc::CreateEnvironment(), H264());
    Collector collector;
    const auto codec = Simulcast(2, 1280, 720, 30);
    CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    encoder->RegisterEncodeCompleteCallback(&collector);
    SetRates(*encoder, 2, 30.0);
    Clock clock;
    for (int i = 0; i < 4; i++) {
        const auto type = i == 0 ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
        EncodeFrame(*encoder, clock, 1280, 720, 30, {type, type});
    }
    encoder->Release();
    CHECK_TRUE(collector.streams[1].images == 0);
    CHECK_TRUE(collector.streams[0].images >= 3 && collector.streams[0].h264 == collector.streams[0].images);
    CHECK_TRUE(setup->fallbacks == 1);
}

// One layer (GroupAv1ScreencastLayers::Single): AV1 is the sender's own codec. Every image AV1, at most 30 fps of 60.
void TestSingleStreamIsAv1() {
    auto setup = MakeSetup(true);
    setup->av1TopLayer->setLayers(tgcalls::GroupAv1ScreencastLayers::Single);
    auto encoder = setup->factory->Create(webrtc::CreateEnvironment(), tgcalls::groupAv1VideoFormat());
    CHECK_TRUE(encoder != nullptr);
    if (!encoder) {
        return;
    }
    Collector collector;
    webrtc::VideoCodec codec = Simulcast(1, 1280, 720, 60);
    codec.codecType = webrtc::kVideoCodecAV1;
    CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    encoder->RegisterEncodeCompleteCallback(&collector);
    SetRates(*encoder, 1, 60.0);
    Clock clock;
    for (int i = 0; i < 20; i++) {
        EncodeFrame(*encoder, clock, 1280, 720, 60, {i == 0 ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta});
    }
    encoder->Release();
    const auto &stream = collector.streams[0];
    std::printf("single stream: %d images (AV1 %d)\n", stream.images, stream.av1);
    CHECK_TRUE(collector.streams.size() == 1);
    CHECK_TRUE(stream.images >= 8 && stream.images <= 11);
    CHECK_TRUE(stream.av1 == stream.images && stream.h264 == 0);
    CHECK_TRUE(stream.keyframes >= 1);
    CHECK_TRUE(stream.lastWidth == 1280 && stream.lastHeight == 720);
    CHECK_TRUE(setup->fallbacks == 0);
}

// One layer with H264 (the server did not list AV1, or AV1 failed): the platform's encoder, every image H264.
void TestSingleH264IsToday() {
    auto setup = MakeSetup(true);
    setup->av1TopLayer->setLayers(tgcalls::GroupAv1ScreencastLayers::Single);
    auto encoder = setup->factory->Create(webrtc::CreateEnvironment(), H264());
    Collector collector;
    const auto codec = Simulcast(1, 1280, 720, 60);
    CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) == WEBRTC_VIDEO_CODEC_OK);
    encoder->RegisterEncodeCompleteCallback(&collector);
    SetRates(*encoder, 1, 30.0);
    Clock clock;
    for (int i = 0; i < 6; i++) {
        EncodeFrame(*encoder, clock, 1280, 720, 30, {i == 0 ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta});
    }
    encoder->Release();
    const auto &stream = collector.streams[0];
    CHECK_TRUE(stream.images >= 5);
    CHECK_TRUE(stream.h264 == stream.images && stream.av1 == 0);
}

// A failing AV1 encoder in Single reports the failure to WebRTC: the one stream has no payload type for another codec's
// images, so it cannot go on in H264 by itself; WebRTC switches the sender (the selector, below).
void TestSingleFailingAv1Surfaces() {
    auto setup = MakeSetup(true);
    setup->av1TopLayer->setLayers(tgcalls::GroupAv1ScreencastLayers::Single);
    setup->av1TopLayer->setTestAv1Fails(true);
    auto encoder = setup->factory->Create(webrtc::CreateEnvironment(), tgcalls::groupAv1VideoFormat());
    CHECK_TRUE(encoder != nullptr);
    if (encoder) {
        webrtc::VideoCodec codec = Simulcast(1, 1280, 720, 60);
        codec.codecType = webrtc::kVideoCodecAV1;
        CHECK_TRUE(encoder->InitEncode(&codec, EncoderSettings()) != WEBRTC_VIDEO_CODEC_OK);
    }
}

// A broken AV1 sender in Single: the selector answers H264 (negotiated after AV1), and the engine is told.
void TestSingleBrokenAv1SwitchesToH264() {
    auto setup = MakeSetup(true);
    setup->av1TopLayer->setLayers(tgcalls::GroupAv1ScreencastLayers::Single);
    auto selector = setup->factory->GetEncoderSelector();
    CHECK_TRUE(selector != nullptr);
    if (!selector) {
        return;
    }
    selector->OnCurrentEncoder(tgcalls::groupAv1VideoFormat());
    const auto answer = selector->OnEncoderBroken();
    CHECK_TRUE(answer.has_value() && answer->name == webrtc::kH264CodecName);
    CHECK_TRUE(setup->fallbacks == 1);
}

} // namespace

int main() {
    TestFormatsListAv1();
    TestSwitchOffIsToday();
    TestTwoStreamsTopIsAv1();
    TestEveryStreamIsAv1();
    TestEveryH264IsToday();
    TestEveryFailingAv1Surfaces();
    TestAv1SupportsL1T1AndL1T3();
    TestFormatsListAv1ScalabilityModes();
    TestSingleStreamIsAv1();
    TestSingleH264IsToday();
    TestSingleFailingAv1Surfaces();
    TestSingleBrokenAv1SwitchesToH264();
    TestOneStreamNoAv1();
    TestTopKeyframeRequestOnly();
    TestReinitAtNewSizeKeepsAv1();
    TestAv1FormatIsRefused();
    TestFailingAv1StopsTheTopStream();
    TestBrokenEncoderStaysOnH264();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("av1_top_layer_factory_test: all checks passed\n");
    return 0;
}
