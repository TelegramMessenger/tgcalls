#include "v2/ContentNegotiation.h"

#include "api/environment/environment_factory.h"

#include "FieldTrialsConfig.h"

#include "media/base/codec.h"
#include "media/base/media_engine.h"
#include "rtc_base/unique_id_generator.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

using tgcalls::ContentNegotiationContext;
using tgcalls::signaling::MediaContent;

// ContentNegotiationContext only reads the engine's codec lists, so a stub with
// Opus and VP8 stands in for the real one.
class StubVoiceEngine : public webrtc::VoiceEngineInterface {
public:
    StubVoiceEngine() : _codecs({ webrtc::CreateAudioCodec(111, "opus", 48000, 2) }) {
    }

    std::unique_ptr<webrtc::VoiceMediaSendChannelInterface> CreateSendChannel(const webrtc::Environment &, webrtc::Call *, const webrtc::MediaConfig &, const webrtc::AudioOptions &, const webrtc::CryptoOptions &, absl::AnyInvocable<void()>) override {
        return nullptr;
    }
    std::unique_ptr<webrtc::VoiceMediaReceiveChannelInterface> CreateReceiveChannel(const webrtc::Environment &, webrtc::Call *, const webrtc::MediaConfig &, const webrtc::AudioOptions &, const webrtc::CryptoOptions &, absl::AnyInvocable<void(uint32_t)>, absl::AnyInvocable<void(uint32_t, const webrtc::RtpPacketInfos &, webrtc::Timestamp) const>) override {
        return nullptr;
    }
    void Init() override {
    }
    void Terminate() override {
    }
    void ApplyGlobalOptions(const webrtc::AudioOptions &) override {
    }
    webrtc::scoped_refptr<webrtc::AudioState> GetAudioState() const override {
        return nullptr;
    }
    const std::vector<webrtc::Codec> &LegacySendCodecs() const override {
        return _codecs;
    }
    const std::vector<webrtc::Codec> &LegacyRecvCodecs() const override {
        return _codecs;
    }
    const webrtc::scoped_refptr<webrtc::AudioEncoderFactory> &encoder_factory() const override {
        return _encoderFactory;
    }
    const webrtc::scoped_refptr<webrtc::AudioDecoderFactory> &decoder_factory() const override {
        return _decoderFactory;
    }
    bool StartAecDump(webrtc::FileWrapper file, int64_t max_size_bytes) override {
        return false;
    }
    void StopAecDump() override {
    }
    std::optional<webrtc::AudioDeviceModule::Stats> GetAudioDeviceStats() override {
        return std::nullopt;
    }
    std::vector<webrtc::RtpHeaderExtensionCapability> GetRtpHeaderExtensions(const webrtc::FieldTrialsView *) const override {
        return {};
    }

private:
    std::vector<webrtc::Codec> _codecs;
    webrtc::scoped_refptr<webrtc::AudioEncoderFactory> _encoderFactory;
    webrtc::scoped_refptr<webrtc::AudioDecoderFactory> _decoderFactory;
};

class StubVideoEngine : public webrtc::VideoEngineInterface {
public:
    explicit StubVideoEngine(std::vector<webrtc::Codec> codecs = { webrtc::CreateVideoCodec(96, "VP8") }) : _codecs(std::move(codecs)) {
    }

    std::unique_ptr<webrtc::VideoMediaSendChannelInterface> CreateSendChannel(const webrtc::Environment &, webrtc::Call *, const webrtc::MediaConfig &, const webrtc::VideoOptions &, const webrtc::CryptoOptions &, webrtc::VideoBitrateAllocatorFactory *, webrtc::VideoMediaSendChannelInterface::EncoderSwitchRequestCallback, absl::AnyInvocable<void()>) override {
        return nullptr;
    }
    std::unique_ptr<webrtc::VideoMediaReceiveChannelInterface> CreateReceiveChannel(const webrtc::Environment &, webrtc::Call *, const webrtc::MediaConfig &, const webrtc::CryptoOptions &, absl::AnyInvocable<void(uint32_t)>, absl::AnyInvocable<void(uint32_t, const webrtc::RtpPacketInfos &, webrtc::Timestamp) const>) override {
        return nullptr;
    }
    std::vector<webrtc::Codec> LegacySendCodecs() const override {
        return _codecs;
    }
    std::vector<webrtc::Codec> LegacyRecvCodecs() const override {
        return _codecs;
    }
    std::vector<webrtc::Codec> LegacySendCodecs(bool) const override {
        return LegacySendCodecs();
    }
    std::vector<webrtc::Codec> LegacyRecvCodecs(bool) const override {
        return LegacyRecvCodecs();
    }
    webrtc::VideoEncoderFactory *encoder_factory() const override {
        return nullptr;
    }
    webrtc::VideoDecoderFactory *decoder_factory() const override {
        return nullptr;
    }
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats(bool) const override {
        std::vector<webrtc::SdpVideoFormat> formats;
        for (const auto &codec : _codecs) {
            formats.push_back(webrtc::SdpVideoFormat(codec.name, codec.params));
        }
        return formats;
    }
    std::vector<webrtc::RtpHeaderExtensionCapability> GetRtpHeaderExtensions(const webrtc::FieldTrialsView *) const override {
        return {};
    }

private:
    std::vector<webrtc::Codec> _codecs;
};

struct Side {
    webrtc::UniqueRandomIdGenerator ids;
    ContentNegotiationContext context;

    Side(bool isOutgoing, webrtc::MediaEngineInterface *engine) :
    context(webrtc::CreateEnvironment(), isOutgoing, engine, &ids) {
        context.copyCodecsFromChannelManager(engine);
    }
};

// Delivers `offer` to `answerer` and its answer back to `offerer`.
void deliver(std::unique_ptr<ContentNegotiationContext::NegotiationContents> &&offer, Side &offerer, Side &answerer) {
    CHECK_TRUE(offer != nullptr);
    auto answer = answerer.context.setRemoteNegotiationContent(std::move(offer));
    CHECK_TRUE(answer != nullptr);
    offerer.context.setRemoteNegotiationContent(std::move(answer));
}

// Runs offer/answer rounds until the offerer has nothing left to negotiate,
// as InstanceV2Impl's sendOfferIfNeeded does after each change.
void settle(Side &offerer, Side &answerer) {
    for (int round = 0; round < 8; round++) {
        auto offer = offerer.context.getPendingOffer();
        if (!offer) {
            return;
        }
        deliver(std::move(offer), offerer, answerer);
    }
    CHECK_TRUE(false && "negotiation did not settle");
}

int outgoingCount(Side const &side, MediaContent::Type type) {
    int count = 0;
    // Keep the state alive: a range-for over a member of the returned
    // temporary would iterate a destroyed vector.
    const auto state = side.context.coordinatedState();
    for (const auto &content : state->outgoingContents) {
        if (content.type == type) {
            count++;
        }
    }
    return count;
}

// The camera goes off after its channel was negotiated: the plain case.
void TestRemoveNegotiatedChannel() {
    webrtc::CompositeMediaEngine engine(std::make_unique<StubVoiceEngine>(), std::make_unique<StubVideoEngine>());
    Side caller(true, &engine);
    Side callee(false, &engine);

    caller.context.addOutgoingChannel(MediaContent::Type::Audio);
    const auto videoId = caller.context.addOutgoingChannel(MediaContent::Type::Video);
    settle(caller, callee);
    CHECK_TRUE(outgoingCount(caller, MediaContent::Type::Audio) == 1);
    CHECK_TRUE(outgoingCount(caller, MediaContent::Type::Video) == 1);

    caller.context.removeOutgoingChannel(videoId);
    settle(caller, callee);
    CHECK_TRUE(outgoingCount(caller, MediaContent::Type::Audio) == 1);
    CHECK_TRUE(outgoingCount(caller, MediaContent::Type::Video) == 0);
}

// The camera goes on mid-call and off again before the peer answered the offer
// that added it. removeOutgoingChannel used to walk the negotiated channels
// (audio only) while indexing the pending ones, so it never reached the video
// entry: the channel stayed in every later offer.
void TestRemoveChannelWhileItsOfferIsInFlight() {
    webrtc::CompositeMediaEngine engine(std::make_unique<StubVoiceEngine>(), std::make_unique<StubVideoEngine>());
    Side caller(true, &engine);
    Side callee(false, &engine);

    caller.context.addOutgoingChannel(MediaContent::Type::Audio);
    settle(caller, callee);

    const auto videoId = caller.context.addOutgoingChannel(MediaContent::Type::Video);
    auto offer = caller.context.getPendingOffer();
    caller.context.removeOutgoingChannel(videoId);
    deliver(std::move(offer), caller, callee);
    settle(caller, callee);

    CHECK_TRUE(outgoingCount(caller, MediaContent::Type::Audio) == 1);
    CHECK_TRUE(outgoingCount(caller, MediaContent::Type::Video) == 0);

    // The camera on again: exactly one video channel, not a second one beside
    // a leftover.
    caller.context.addOutgoingChannel(MediaContent::Type::Video);
    settle(caller, callee);
    CHECK_TRUE(outgoingCount(caller, MediaContent::Type::Video) == 1);
}

// A video call whose camera goes off before the first answer: nothing has been
// negotiated yet, so the old loop ran zero times.
void TestRemoveChannelBeforeFirstAnswer() {
    webrtc::CompositeMediaEngine engine(std::make_unique<StubVoiceEngine>(), std::make_unique<StubVideoEngine>());
    Side caller(true, &engine);
    Side callee(false, &engine);

    caller.context.addOutgoingChannel(MediaContent::Type::Audio);
    const auto videoId = caller.context.addOutgoingChannel(MediaContent::Type::Video);
    auto offer = caller.context.getPendingOffer();
    caller.context.removeOutgoingChannel(videoId);
    deliver(std::move(offer), caller, callee);
    settle(caller, callee);

    CHECK_TRUE(outgoingCount(caller, MediaContent::Type::Audio) == 1);
    CHECK_TRUE(outgoingCount(caller, MediaContent::Type::Video) == 0);
}

// Camera to screencast and back to the camera within one in-flight offer: the
// screencast channel must go as well.
void TestSwitchChannelsWhileOfferIsInFlight() {
    webrtc::CompositeMediaEngine engine(std::make_unique<StubVoiceEngine>(), std::make_unique<StubVideoEngine>());
    Side caller(true, &engine);
    Side callee(false, &engine);

    caller.context.addOutgoingChannel(MediaContent::Type::Audio);
    settle(caller, callee);

    const auto cameraId = caller.context.addOutgoingChannel(MediaContent::Type::Video);
    auto offer = caller.context.getPendingOffer();
    caller.context.removeOutgoingChannel(cameraId);
    const auto screencastId = caller.context.addOutgoingChannel(MediaContent::Type::Video);
    caller.context.removeOutgoingChannel(screencastId);
    deliver(std::move(offer), caller, callee);
    settle(caller, callee);

    CHECK_TRUE(outgoingCount(caller, MediaContent::Type::Video) == 0);
}

webrtc::Codec h265Codec(webrtc::CodecParameterMap params) {
    webrtc::Codec codec = webrtc::CreateVideoCodec(96, "H265");
    codec.params = std::move(params);
    return codec;
}

// The H.265 advertisement of our iOS factories (TGRTCH265StandardParameters).
webrtc::CodecParameterMap standardH265Parameters() {
    return { { "profile-id", "1" }, { "tier-flag", "0" }, { "level-id", "93" }, { "tx-mode", "SRST" } };
}

bool sendsH265(Side const &side) {
    const auto state = side.context.coordinatedState();
    for (const auto &content : state->outgoingContents) {
        if (content.type != MediaContent::Type::Video) {
            continue;
        }
        for (const auto &payloadType : content.payloadTypes) {
            if (payloadType.name == "H265") {
                return true;
            }
        }
    }
    return false;
}

// Both sides send video; returns whether each negotiated H.265 for it.
std::pair<bool, bool> negotiateVideo(webrtc::CodecParameterMap callerParams, webrtc::CodecParameterMap calleeParams) {
    webrtc::CompositeMediaEngine callerEngine(std::make_unique<StubVoiceEngine>(), std::make_unique<StubVideoEngine>(std::vector<webrtc::Codec>{ h265Codec(callerParams) }));
    webrtc::CompositeMediaEngine calleeEngine(std::make_unique<StubVoiceEngine>(), std::make_unique<StubVideoEngine>(std::vector<webrtc::Codec>{ h265Codec(calleeParams) }));
    Side caller(true, &callerEngine);
    Side callee(false, &calleeEngine);

    caller.context.addOutgoingChannel(MediaContent::Type::Audio);
    caller.context.addOutgoingChannel(MediaContent::Type::Video);
    settle(caller, callee);
    callee.context.addOutgoingChannel(MediaContent::Type::Audio);
    callee.context.addOutgoingChannel(MediaContent::Type::Video);
    settle(callee, caller);
    settle(caller, callee);

    return { sendsH265(caller), sendsH265(callee) };
}

// H.265 advertised with its standard parameters still negotiates with a peer that advertises it by name only (our
// M123 factories), as offerer and as answerer.
void TestStandardH265NegotiatesWithNameOnlyPeer() {
    const auto standardOffers = negotiateVideo(standardH265Parameters(), {});
    CHECK_TRUE(standardOffers.first);
    CHECK_TRUE(standardOffers.second);

    const auto nameOnlyOffers = negotiateVideo({}, standardH265Parameters());
    CHECK_TRUE(nameOnlyOffers.first);
    CHECK_TRUE(nameOnlyOffers.second);

    // Control: a profile the name-only peer does not imply is not H.265 it can take.
    auto otherProfile = standardH265Parameters();
    otherProfile["profile-id"] = "3";
    const auto mismatch = negotiateVideo(otherProfile, {});
    CHECK_TRUE(!mismatch.first);
}

} // namespace

int main() {
    TestStandardH265NegotiatesWithNameOnlyPeer();
    TestRemoveNegotiatedChannel();
    TestRemoveChannelWhileItsOfferIsInFlight();
    TestRemoveChannelBeforeFirstAnswer();
    TestSwitchChannelsWhileOfferIsInFlight();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
