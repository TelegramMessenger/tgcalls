// Host test for GroupOpusDred: the group engines' Opus DRED encoder factory and field trials
// (docs/superpowers/specs/2026-11-26-opus-dred-group-calls-design.md). Built twice: group_opus_dred_test with
// GroupOpusDred.cpp as Bazel builds it (TGCALLS_ENABLE_OPUS_DRED), group_opus_dred_unavailable_test without the define,
// as a build whose WebRTC has no DRED (Telegram-Mac's Package.swift, tdesktop) compiles it: DRED then stays off.
//
//   ./build-input/bazel-9.2.0-darwin-arm64 test //submodules/TgVoipWebrtc:group_opus_dred_test \
//     //submodules/TgVoipWebrtc:group_opus_dred_unavailable_test --test_output=errors

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "api/audio_codecs/audio_format.h"
#include "api/environment/deprecated_global_field_trials.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "api/units/time_delta.h"
#include "api/units/data_rate.h"
#include "api/units/timestamp.h"
#include "audio/audio_send_stream.h"
#include "system_wrappers/include/clock.h"
#include "group/GroupOpusDred.h"
#include "third-party/webrtc/tests/OpusDredTestSignal.h"

namespace {

int g_failures = 0;

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

// The custom group engine's trials before DRED (GroupInstanceCustomImpl::start).
const char kTodaysCustomEngineTrials[] =
    "WebRTC-Audio-Allocation/min:32kbps,max:32kbps/"
    "WebRTC-Audio-MinimizeResamplingOnMobile/Enabled/"
    "WebRTC-BweLossExperiment/Enabled/"
    "WebRTC-Video-DiscardPacketsWithUnknownSsrc/Enabled/"
    "WebRTC-MixedCodecSimulcast/Enabled/";

// What both engines negotiate for their outgoing Opus.
webrtc::SdpAudioFormat GroupOpusFormat() {
    return webrtc::SdpAudioFormat("opus", 48000, 2, {{"minptime", "10"}, {"ptime", "60"}, {"useinbandfec", "1"}});
}

std::vector<webrtc::Buffer> EncodeThroughFactory(int dredLoss, bool includeL16,
                                                 tgcalls::GroupOpusDnnWeights weights = opus_dred_test::DnnWeights()) {
    const webrtc::Environment env = webrtc::CreateEnvironment();
    auto factory = tgcalls::CreateGroupAudioEncoderFactory(dredLoss, std::move(weights), includeL16);
    auto encoder = factory->Create(env, GroupOpusFormat(), {.payload_type = 111});
    CHECK_TRUE(encoder != nullptr);
    if (!encoder) {
        return {};
    }
    uint32_t rtpTimestamp = 0;
    return opus_dred_test::EncodePcm(*encoder, opus_dred_test::MakeVoicedTestSignal(48000, 3000), &rtpTimestamp);
}

void TestDredSenderPutsDredInEveryPacketWithoutFec() {
    const auto packets = EncodeThroughFactory(25, false);
    size_t withDred = 0, withFec = 0, counted = 0;
    for (size_t i = 0; i < packets.size(); i++) {
        if (opus_packet_has_lbrr(packets[i].data(), static_cast<opus_int32>(packets[i].size())) > 0) {
            withFec++;
        }
        if (i * 60 < 500) {
            continue;
        }
        counted++;
        if (opus_dred_test::DredReach(packets[i]) > 0) {
            withDred++;
        }
    }
    std::printf("dred sender: %zu packets, DRED in %zu of %zu after 500 ms, FEC in %zu\n", packets.size(), withDred,
                counted, withFec);
    CHECK_TRUE(packets.size() >= 45);
    if (tgcalls::groupOpusDredAvailable()) {
        CHECK_TRUE(counted > 0 && withDred * 10 >= counted * 9);
        CHECK_TRUE(withFec == 0);
    } else {
        CHECK_TRUE(withDred == 0);
    }
}

// What the engines take from the descriptor (GroupInstanceDescriptor::dredFixedPacketLossPercent, dredReceiveEnabled,
// opusDnnWeights).
void TestDescriptorValuesInThisBuild() {
    const bool available = tgcalls::groupOpusDredAvailable();
    std::printf("Opus DRED in this build: %s\n", available ? "yes" : "no");
    CHECK_TRUE(opus_dred_test::DnnWeights() != nullptr);
    CHECK_TRUE(tgcalls::groupOpusDnnWeightsUsable(opus_dred_test::DnnWeights()) == available);
    CHECK_TRUE(tgcalls::groupDredFixedPacketLossPercent(25, true) == (available ? 25 : 0));
    CHECK_TRUE(tgcalls::groupDredFixedPacketLossPercent(150, true) == (available ? 100 : 0));
    CHECK_TRUE(tgcalls::groupDredFixedPacketLossPercent(-3, true) == 0);
    CHECK_TRUE(tgcalls::groupDredFixedPacketLossPercent(0, true) == 0);
    CHECK_TRUE(tgcalls::groupDredReceiveEnabled(true, true) == available);
    CHECK_TRUE(!tgcalls::groupDredReceiveEnabled(false, true));
    // No usable weights: no DRED either way.
    CHECK_TRUE(tgcalls::groupDredFixedPacketLossPercent(25, false) == 0);
    CHECK_TRUE(!tgcalls::groupDredReceiveEnabled(true, false));
}

std::vector<tgcalls::GroupOpusDnnWeights> UnusableWeights() {
    const auto& good = *opus_dred_test::DnnWeights();
    return {
        nullptr,
        std::make_shared<std::vector<uint8_t>>(good.begin(), good.begin() + good.size() / 2),
        std::make_shared<std::vector<uint8_t>>(good.size(), 0xA5),
        std::make_shared<std::vector<uint8_t>>(),
    };
}

bool SamePackets(const std::vector<webrtc::Buffer>& a, const std::vector<webrtc::Buffer>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

// The weights are the app's to give (libopus is built without them): with none, or a malformed blob, a sender asked
// for DRED sends exactly what today's sender sends.
void TestWithoutUsableWeightsTheSenderIsTodays() {
    const auto today = EncodeThroughFactory(0, false, nullptr);
    for (const auto& weights : UnusableWeights()) {
        CHECK_TRUE(!tgcalls::groupOpusDnnWeightsUsable(weights));
        CHECK_TRUE(SamePackets(EncodeThroughFactory(25, false, weights), today));
    }
}

// The group engines' decoders get the weights from CreateGroupAudioDecoderFactory: with them (and the receive trial)
// a gap is rebuilt from DRED; without, it is not.
void TestDecoderFactoryGivesTheWeights() {
    const webrtc::Environment env =
        webrtc::CreateEnvironment(std::make_unique<webrtc::FieldTrials>("WebRTC-Audio-OpusDred/Enabled/"));
    const auto packets = EncodeThroughFactory(25, false);
    CHECK_TRUE(packets.size() > 40);
    if (packets.size() <= 40) {
        return;
    }
    for (const bool withWeights : {true, false}) {
        auto factory = tgcalls::CreateGroupAudioDecoderFactory(withWeights ? opus_dred_test::DnnWeights() : nullptr,
                                                               true);
        bool listsL16 = false, listsOpus = false;
        for (const auto& spec : factory->GetSupportedDecoders()) {
            listsL16 = listsL16 || spec.format.name == "L16";
            listsOpus = listsOpus || spec.format.name == "opus";
        }
        CHECK_TRUE(listsL16 && listsOpus);
        auto decoder = factory->Create(env, webrtc::SdpAudioFormat("opus", 48000, 2));
        CHECK_TRUE(decoder != nullptr);
        if (!decoder) {
            continue;
        }
        const auto& packet = packets[40];
        auto results = decoder->ParsePayloadWithGap(webrtc::Buffer(packet.data(), packet.size()), 1000000, 14400);
        size_t rebuilt = 0;
        for (const auto& result : results) {
            rebuilt += result.priority == 2 ? 1 : 0;
        }
        std::printf("decoder factory %s weights: %zu rebuilt frames\n", withWeights ? "with" : "without", rebuilt);
        CHECK_TRUE((rebuilt > 0) == (withWeights && tgcalls::groupOpusDredAvailable()));
    }
}

// The engines' log line (GroupRef: Opus DRED …): what was sent and received, and why not.
void TestDescription() {
    if (!tgcalls::groupOpusDredAvailable()) {
        CHECK_TRUE(tgcalls::groupOpusDredDescription(0, false, opus_dred_test::DnnWeights()) ==
                   "send off, receive off (not in this build)");
        return;
    }
    CHECK_TRUE(tgcalls::groupOpusDredDescription(25, true, opus_dred_test::DnnWeights()) ==
               "send 25%, receive on, weights 1998208 bytes");
    CHECK_TRUE(tgcalls::groupOpusDredDescription(0, false, nullptr) == "send off, receive off, no weights");
    CHECK_TRUE(tgcalls::groupOpusDredDescription(0, false, std::make_shared<std::vector<uint8_t>>(100, 0xA5)) ==
               "send off, receive off, weights 100 bytes, not usable");
}

void TestWithoutDredTheFactoryIsTodays() {
    const webrtc::Environment env = webrtc::CreateEnvironment();
    auto factory = tgcalls::CreateGroupAudioEncoderFactory(0, opus_dred_test::DnnWeights(), true);
    bool listsL16 = false, listsOpus = false;
    for (const auto& spec : factory->GetSupportedEncoders()) {
        listsL16 = listsL16 || spec.format.name == "L16";
        listsOpus = listsOpus || spec.format.name == "opus";
    }
    CHECK_TRUE(listsL16 && listsOpus);
    const auto packets = EncodeThroughFactory(0, true);
    CHECK_TRUE(!packets.empty());
    for (const auto& packet : packets) {
        CHECK_TRUE(opus_dred_test::DredReach(packet) == 0);
    }
}

void TestCustomEngineTrials() {
    CHECK_TRUE(std::string(tgcalls::groupCustomEngineFieldTrials(false, false)) == kTodaysCustomEngineTrials);
    if (!tgcalls::groupOpusDredAvailable()) {
        CHECK_TRUE(std::string(tgcalls::groupCustomEngineFieldTrials(true, true)) == kTodaysCustomEngineTrials);
        CHECK_TRUE(std::string(tgcalls::groupCustomEngineFieldTrials(true, false)) == kTodaysCustomEngineTrials);
        CHECK_TRUE(std::string(tgcalls::groupCustomEngineFieldTrials(false, true)) == kTodaysCustomEngineTrials);
        return;
    }
    const std::string both = tgcalls::groupCustomEngineFieldTrials(true, true);
    CHECK_TRUE(both.find("WebRTC-Audio-Allocation/min:40kbps,max:40kbps/") != std::string::npos);
    CHECK_TRUE(both.find("WebRTC-Audio-OpusDred/Enabled/") != std::string::npos);
    CHECK_TRUE(both.find("WebRTC-Audio-OpusGeneratePlc/Enabled/") != std::string::npos);
    CHECK_TRUE(both.find("min:32kbps") == std::string::npos);
    const std::string receiveOnly = tgcalls::groupCustomEngineFieldTrials(true, false);
    CHECK_TRUE(receiveOnly.find("WebRTC-Audio-Allocation/min:32kbps,max:32kbps/") != std::string::npos);
    CHECK_TRUE(receiveOnly.find("WebRTC-Audio-OpusDred/Enabled/") != std::string::npos);
}

// The custom engine's trial string is process-wide, and a ReplayKit share runs its own custom-engine connection beside
// the call's: it must not undo the DRED trials the call's connection set (new receive streams would lose DRED, a later
// outgoing stream the 40 kbit/s allocation).
void TestAShareKeepsTheMainConnectionsDredTrials() {
    const std::string today = kTodaysCustomEngineTrials;
    CHECK_TRUE(std::string(tgcalls::groupCustomEngineFieldTrialsForConnection(true, false, false)) == today);
    if (!tgcalls::groupOpusDredAvailable()) {
        CHECK_TRUE(std::string(tgcalls::groupCustomEngineFieldTrialsForConnection(false, true, true)) == today);
        CHECK_TRUE(std::string(tgcalls::groupCustomEngineFieldTrialsForConnection(true, false, false)) == today);
        return;
    }
    CHECK_TRUE(std::string(tgcalls::groupCustomEngineFieldTrialsForConnection(false, true, true)) ==
               tgcalls::groupCustomEngineFieldTrials(true, true));
    CHECK_TRUE(std::string(tgcalls::groupCustomEngineFieldTrialsForConnection(true, false, false)) ==
               tgcalls::groupCustomEngineFieldTrials(true, true));
    CHECK_TRUE(std::string(tgcalls::groupCustomEngineFieldTrialsForConnection(false, true, false)) ==
               tgcalls::groupCustomEngineFieldTrials(true, false));
    CHECK_TRUE(std::string(tgcalls::groupCustomEngineFieldTrialsForConnection(true, false, false)) ==
               tgcalls::groupCustomEngineFieldTrials(true, false));
    CHECK_TRUE(std::string(tgcalls::groupCustomEngineFieldTrialsForConnection(false, false, false)) == today);
    CHECK_TRUE(std::string(tgcalls::groupCustomEngineFieldTrialsForConnection(true, false, false)) == today);
}

// Once the main connection has ended, a later share (in a call whose main connection runs the reference engine, the
// default, which sets no process-wide trials) does not take its flags; an older connection's end does not clear a
// newer one's.
void TestAnEndedMainConnectionsTrialsAreNotTaken() {
    const std::string today = kTodaysCustomEngineTrials;
    uint64_t first = 0;
    uint64_t second = 0;
    tgcalls::groupCustomEngineFieldTrialsForConnection(false, true, true, &first);
    tgcalls::groupCustomEngineMainConnectionEnded(first);
    CHECK_TRUE(std::string(tgcalls::groupCustomEngineFieldTrialsForConnection(true, false, false)) == today);
    if (!tgcalls::groupOpusDredAvailable()) {
        return;
    }
    tgcalls::groupCustomEngineFieldTrialsForConnection(false, true, true, &first);
    tgcalls::groupCustomEngineFieldTrialsForConnection(false, true, false, &second);
    CHECK_TRUE(first != second);
    tgcalls::groupCustomEngineMainConnectionEnded(first);
    CHECK_TRUE(std::string(tgcalls::groupCustomEngineFieldTrialsForConnection(true, false, false)) ==
               tgcalls::groupCustomEngineFieldTrials(true, false));
    tgcalls::groupCustomEngineMainConnectionEnded(second);
    CHECK_TRUE(std::string(tgcalls::groupCustomEngineFieldTrialsForConnection(true, false, false)) == today);
}

void TestSingleKeys() {
    if (!tgcalls::groupOpusDredAvailable()) {
        CHECK_TRUE(tgcalls::groupOpusDredFieldTrial("WebRTC-Audio-OpusDred", true, true).empty());
        CHECK_TRUE(tgcalls::groupOpusDredFieldTrial("WebRTC-Audio-OpusGeneratePlc", true, true).empty());
        CHECK_TRUE(tgcalls::groupOpusDredFieldTrial("WebRTC-Audio-Allocation", true, true).empty());
        return;
    }
    CHECK_TRUE(tgcalls::groupOpusDredFieldTrial("WebRTC-Audio-OpusDred", true, false) == "Enabled");
    CHECK_TRUE(tgcalls::groupOpusDredFieldTrial("WebRTC-Audio-OpusGeneratePlc", true, false) == "Enabled");
    CHECK_TRUE(tgcalls::groupOpusDredFieldTrial("WebRTC-Audio-Allocation", false, true) == "min:40kbps,max:40kbps");
    CHECK_TRUE(tgcalls::groupOpusDredFieldTrial("WebRTC-Audio-OpusDred", false, true).empty());
    CHECK_TRUE(tgcalls::groupOpusDredFieldTrial("WebRTC-Audio-Allocation", true, false).empty());
    CHECK_TRUE(tgcalls::groupOpusDredFieldTrial("WebRTC-MixedCodecSimulcast", true, true).empty());
}

// 1:1 calls (docs/superpowers/specs/2026-11-26-opus-dred-p2p-calls-design.md): InstanceV2ReferenceImpl's DRED, in
// libopus's adaptive mode.

// What InstanceV2ReferenceImpl negotiates for its Opus (20 ms frames).
webrtc::SdpAudioFormat P2POpusFormat() {
    return webrtc::SdpAudioFormat("opus", 48000, 2, {{"minptime", "10"}, {"useinbandfec", "1"}});
}

// Per second of audio, the packets of an encoder from CreateP2PAudioEncoderFactory at 64 kbit/s, told `loss` every
// 500 ms on a clock that runs at the audio's pace (WebRTC smooths the reports over its clock).
std::vector<std::vector<webrtc::Buffer>> EncodeP2P(bool dred, tgcalls::GroupOpusDnnWeights weights,
                                                   const std::vector<std::pair<float, int>>& lossForSeconds) {
    webrtc::SimulatedClock clock(webrtc::Timestamp::Seconds(1000));
    const webrtc::Environment env = webrtc::CreateEnvironment(&clock);
    auto factory = tgcalls::CreateP2PAudioEncoderFactory(dred, std::move(weights));
    auto encoder = factory->Create(env, P2POpusFormat(), {.payload_type = 111});
    CHECK_TRUE(encoder != nullptr);
    if (!encoder) {
        return {};
    }
    encoder->OnReceivedTargetAudioBitrate(tgcalls::kP2PDredMaxBitrateBps);
    const auto pcm = opus_dred_test::MakeVoicedTestSignal(48000, 500);
    uint32_t rtpTimestamp = 0;
    std::vector<std::vector<webrtc::Buffer>> perSecond;
    for (const auto& [loss, seconds] : lossForSeconds) {
        for (int second = 0; second < seconds; second++) {
            std::vector<webrtc::Buffer> packets;
            for (int half = 0; half < 2; half++) {
                encoder->OnReceivedUplinkPacketLossFraction(loss);
                for (auto& packet : opus_dred_test::EncodePcm(*encoder, pcm, &rtpTimestamp)) {
                    packets.push_back(std::move(packet));
                }
                clock.AdvanceTime(webrtc::TimeDelta::Millis(500));
            }
            perSecond.push_back(std::move(packets));
        }
    }
    return perSecond;
}

double ShareWithDred(const std::vector<webrtc::Buffer>& packets) {
    size_t with = 0;
    for (const auto& packet : packets) {
        with += opus_dred_test::DredReach(packet) > 0 ? 1 : 0;
    }
    return packets.empty() ? 0.0 : static_cast<double>(with) / packets.size();
}

void TestP2PEnabledRule() {
    const auto usable = opus_dred_test::DnnWeights();
    const bool available = tgcalls::groupOpusDredAvailable();
    CHECK_TRUE(tgcalls::p2pOpusDredEnabled(true, usable) == available);
    CHECK_TRUE(!tgcalls::p2pOpusDredEnabled(false, usable));
    for (const auto& weights : UnusableWeights()) {
        CHECK_TRUE(!tgcalls::p2pOpusDredEnabled(true, weights));
        CHECK_TRUE(!tgcalls::p2pOpusDredEnabled(false, weights));
    }
}

// No fixed figure: no DRED while the peer reports no loss, DRED in every packet once 20% has been reported a while.
void TestP2PEncoderIsAdaptive() {
    const auto seconds = EncodeP2P(true, opus_dred_test::DnnWeights(), {{0.0f, 3}, {0.2f, 20}});
    CHECK_TRUE(seconds.size() == 23);
    if (seconds.size() != 23) {
        return;
    }
    double atNoLoss = 0.0;
    for (int i = 0; i < 3; i++) {
        atNoLoss = std::max(atNoLoss, ShareWithDred(seconds[i]));
    }
    const double atLossEnd = ShareWithDred(seconds.back());
    std::printf("1:1 encoder: DRED in %.0f%% at 0%% loss, %.0f%% after 20 s of 20%%\n", atNoLoss * 100,
                atLossEnd * 100);
    CHECK_TRUE(atNoLoss == 0.0);
    CHECK_TRUE(tgcalls::groupOpusDredAvailable() ? atLossEnd >= 0.9 : atLossEnd == 0.0);
}

// DRED off, or no usable weights: the encoder is today's, at any loss.
void TestP2PWithoutDredIsTodays() {
    const std::vector<std::pair<float, int>> schedule = {{0.0f, 2}, {0.2f, 10}};
    const auto today = EncodeP2P(false, nullptr, schedule);
    CHECK_TRUE(!today.empty());
    const auto flatten = [](const std::vector<std::vector<webrtc::Buffer>>& seconds) {
        std::vector<webrtc::Buffer> all;
        for (const auto& packets : seconds) {
            for (const auto& packet : packets) {
                all.emplace_back(packet.data(), packet.size());
            }
        }
        return all;
    };
    const auto todayPackets = flatten(today);
    CHECK_TRUE(SamePackets(flatten(EncodeP2P(false, opus_dred_test::DnnWeights(), schedule)), todayPackets));
    for (const auto& weights : UnusableWeights()) {
        CHECK_TRUE(SamePackets(flatten(EncodeP2P(true, weights, schedule)), todayPackets));
    }
    auto factory = tgcalls::CreateP2PAudioEncoderFactory(true, opus_dred_test::DnnWeights());
    bool onlyOpus = true;
    for (const auto& spec : factory->GetSupportedEncoders()) {
        onlyOpus = onlyOpus && spec.format.name == "opus";
    }
    CHECK_TRUE(onlyOpus && !factory->GetSupportedEncoders().empty());
}

void TestP2PFieldTrials() {
    const auto lookup = [](bool dred, absl::string_view key) {
        const webrtc::Environment env = webrtc::CreateEnvironment(tgcalls::CreateP2PFieldTrials(dred));
        return env.field_trials().Lookup(key);
    };
    const std::string expected = tgcalls::groupOpusDredAvailable() ? "Enabled" : "";
    CHECK_TRUE(lookup(true, "WebRTC-Audio-OpusDred") == expected);
    CHECK_TRUE(lookup(true, "WebRTC-Audio-OpusGeneratePlc") == expected);
    CHECK_TRUE(lookup(false, "WebRTC-Audio-OpusDred").empty());
    CHECK_TRUE(lookup(false, "WebRTC-Audio-OpusGeneratePlc").empty());
    // An Environment may copy its trials (the default CreateCopy aborts); the copy answers the same.
    const auto copy = tgcalls::CreateP2PFieldTrials(true)->CreateCopy();
    CHECK_TRUE(copy != nullptr && copy->Lookup("WebRTC-Audio-OpusDred") == expected);
    CHECK_TRUE(copy != nullptr && copy->Lookup("WebRTC-Audio-Allocation") == lookup(true, "WebRTC-Audio-Allocation"));
}

// With DRED the call's audio floats between 48 and 64 kbit/s with the bandwidth estimate (owner, 2026-11-26), read
// by WebRTC's AudioSendStream as it reads it (a misspelled value would be ignored silently); without, nothing (today's
// fixed 32 * 1024).
void TestP2PAudioAllocation() {
    for (const bool dred : {false, true}) {
        const auto trials = tgcalls::CreateP2PFieldTrials(dred);
        const webrtc::AudioAllocationConfig allocation(*trials);
        if (dred && tgcalls::groupOpusDredAvailable()) {
            CHECK_TRUE(allocation.min_bitrate == webrtc::DataRate::BitsPerSec(tgcalls::kP2PDredMinBitrateBps));
            CHECK_TRUE(allocation.max_bitrate == webrtc::DataRate::BitsPerSec(tgcalls::kP2PDredMaxBitrateBps));
            CHECK_TRUE(tgcalls::kP2PDredMinBitrateBps == 48000 && tgcalls::kP2PDredMaxBitrateBps == 64000);
        } else {
            CHECK_TRUE(!allocation.min_bitrate.has_value());
            CHECK_TRUE(!allocation.max_bitrate.has_value());
        }
        CHECK_TRUE(!allocation.bitrate_priority.has_value());
        CHECK_TRUE(allocation.priority_bitrate.IsZero());
    }
}

// A 1:1 call beside a custom-engine group call: the call reads its own trials only, never the process-wide string.
void TestP2PTrialsIgnoreTheProcessWideString() {
    webrtc::DeprecatedGlobalFieldTrials::Set(tgcalls::groupCustomEngineFieldTrials(true, true));
    const webrtc::Environment processWide = webrtc::CreateEnvironment();
    CHECK_TRUE(!processWide.field_trials().Lookup("WebRTC-Audio-Allocation").empty());
    for (const bool dred : {false, true}) {
        const webrtc::Environment call = webrtc::CreateEnvironment(tgcalls::CreateP2PFieldTrials(dred));
        // The group string's 40 kbit/s never; the call's own range with DRED, nothing without.
        const std::string allocation = call.field_trials().Lookup("WebRTC-Audio-Allocation");
        CHECK_TRUE(allocation == (dred && tgcalls::groupOpusDredAvailable() ? "min:48kbps,max:64kbps" : ""));
        CHECK_TRUE(call.field_trials().Lookup("WebRTC-Video-DiscardPacketsWithUnknownSsrc").empty());
        if (!dred) {
            CHECK_TRUE(call.field_trials().Lookup("WebRTC-Audio-OpusDred").empty());
        }
    }
    webrtc::DeprecatedGlobalFieldTrials::Set("");
}

void TestP2PDescription() {
    const auto weights = opus_dred_test::DnnWeights();
    if (!tgcalls::groupOpusDredAvailable()) {
        CHECK_TRUE(tgcalls::p2pOpusDredDescription(true, weights) == "off (not in this build)");
        return;
    }
    CHECK_TRUE(tgcalls::p2pOpusDredDescription(true, weights) == "on, weights 1998208 bytes");
    CHECK_TRUE(tgcalls::p2pOpusDredDescription(false, weights) == "off (audio_dred false)");
    CHECK_TRUE(tgcalls::p2pOpusDredDescription(true, nullptr) == "off, no weights");
    CHECK_TRUE(tgcalls::p2pOpusDredDescription(true, std::make_shared<std::vector<uint8_t>>(100, 0xA5)) ==
               "off, weights 100 bytes, not usable");
}

}  // namespace

int main() {
    TestDescriptorValuesInThisBuild();
    TestDredSenderPutsDredInEveryPacketWithoutFec();
    TestWithoutUsableWeightsTheSenderIsTodays();
    TestDecoderFactoryGivesTheWeights();
    TestDescription();
    TestWithoutDredTheFactoryIsTodays();
    TestCustomEngineTrials();
    TestSingleKeys();
    TestAShareKeepsTheMainConnectionsDredTrials();
    TestAnEndedMainConnectionsTrialsAreNotTaken();
    TestP2PEnabledRule();
    TestP2PEncoderIsAdaptive();
    TestP2PWithoutDredIsTodays();
    TestP2PFieldTrials();
    TestP2PAudioAllocation();
    TestP2PTrialsIgnoreTheProcessWideString();
    TestP2PDescription();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
