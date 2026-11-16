#include "v2/ContentNegotiation.h"

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
class StubVoiceEngine : public cricket::VoiceEngineInterface {
public:
    StubVoiceEngine() : _codecs({ cricket::CreateAudioCodec(111, "opus", 48000, 2) }) {
    }

    void Init() override {
    }
    rtc::scoped_refptr<webrtc::AudioState> GetAudioState() const override {
        return nullptr;
    }
    const std::vector<cricket::AudioCodec> &send_codecs() const override {
        return _codecs;
    }
    const std::vector<cricket::AudioCodec> &recv_codecs() const override {
        return _codecs;
    }
    bool StartAecDump(webrtc::FileWrapper file, int64_t max_size_bytes) override {
        return false;
    }
    void StopAecDump() override {
    }
    absl::optional<webrtc::AudioDeviceModule::Stats> GetAudioDeviceStats() override {
        return absl::nullopt;
    }
    std::vector<webrtc::RtpHeaderExtensionCapability> GetRtpHeaderExtensions() const override {
        return {};
    }

private:
    std::vector<cricket::AudioCodec> _codecs;
};

class StubVideoEngine : public cricket::VideoEngineInterface {
public:
    std::vector<cricket::VideoCodec> send_codecs() const override {
        return { cricket::CreateVideoCodec(96, "VP8") };
    }
    std::vector<cricket::VideoCodec> recv_codecs() const override {
        return { cricket::CreateVideoCodec(96, "VP8") };
    }
    std::vector<webrtc::RtpHeaderExtensionCapability> GetRtpHeaderExtensions() const override {
        return {};
    }
};

struct Side {
    rtc::UniqueRandomIdGenerator ids;
    ContentNegotiationContext context;

    Side(bool isOutgoing, cricket::MediaEngineInterface *engine) :
    context(tgcalls::fieldTrialsBasedConfig, isOutgoing, engine, &ids) {
        context.copyCodecsFromChannelManager(engine, false);
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
    cricket::CompositeMediaEngine engine(std::make_unique<StubVoiceEngine>(), std::make_unique<StubVideoEngine>());
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
    cricket::CompositeMediaEngine engine(std::make_unique<StubVoiceEngine>(), std::make_unique<StubVideoEngine>());
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
    cricket::CompositeMediaEngine engine(std::make_unique<StubVoiceEngine>(), std::make_unique<StubVideoEngine>());
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
    cricket::CompositeMediaEngine engine(std::make_unique<StubVoiceEngine>(), std::make_unique<StubVideoEngine>());
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

} // namespace

int main() {
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
