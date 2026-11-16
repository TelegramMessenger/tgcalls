// The AV1 keys an InstanceV2ReferenceImpl call reads, and which negotiated codec its video sender is pinned to.

#include "av1/Av1SendCodec.h"

#include "api/rtp_parameters.h"
#include "third-party/json11.hpp"

#include <cstdio>
#include <map>
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

std::map<std::string, json11::Json> Parameters(const char *json) {
    std::string error;
    return json11::Json::parse(json, error).object_items();
}

webrtc::RtpCodec Codec(const char *name) {
    webrtc::RtpCodec codec;
    codec.name = name;
    codec.kind = cricket::MEDIA_TYPE_VIDEO;
    codec.clock_rate = 90000;
    return codec;
}

std::vector<std::string> Candidates(const std::vector<webrtc::RtpCodec> &available, bool av1Allowed) {
    std::vector<std::string> names;
    for (const auto &codec : tgcalls::VideoSendCodecCandidates(available, av1Allowed)) {
        names.push_back(codec.name);
    }
    return names;
}

using Names = std::vector<std::string>;

void TestDefaults() {
    auto settings = tgcalls::Av1CallSettingsFromCustomParameters(Parameters("{}"), true);
    CHECK_TRUE(settings.offerAv1);
    CHECK_TRUE(settings.sendAv1);
    CHECK_TRUE(settings.deviceMaySendAv1);
    CHECK_TRUE(!settings.testOfferAv1First);
    auto older = tgcalls::Av1CallSettingsFromCustomParameters(Parameters("{}"), false);
    CHECK_TRUE(older.offerAv1);
    CHECK_TRUE(!older.deviceMaySendAv1);
}

void TestKillswitches() {
    auto noAv1 = tgcalls::Av1CallSettingsFromCustomParameters(Parameters("{\"video_av1\":false}"), true);
    CHECK_TRUE(!noAv1.offerAv1);
    auto noSend = tgcalls::Av1CallSettingsFromCustomParameters(Parameters("{\"video_av1_send\":false}"), true);
    CHECK_TRUE(noSend.offerAv1);
    CHECK_TRUE(!noSend.sendAv1);
}

void TestAnyDeviceOverridesGate() {
    auto settings = tgcalls::Av1CallSettingsFromCustomParameters(Parameters("{\"video_av1_send_any_device\":true}"), false);
    CHECK_TRUE(settings.deviceMaySendAv1);
}

void TestTestKnobs() {
    // tgcalls_core, which this test links, is built with TGCALLS_ALLOW_TEST_KNOBS.
    auto incapable = tgcalls::Av1CallSettingsFromCustomParameters(Parameters("{\"video_av1_test_device_incapable\":true}"), true);
    CHECK_TRUE(!incapable.deviceMaySendAv1);
    auto first = tgcalls::Av1CallSettingsFromCustomParameters(Parameters("{\"video_av1_test_offer_av1_first\":true}"), true);
    CHECK_TRUE(first.testOfferAv1First);
}

// Allowed: AV1 alone. A rejected AV1 pin leaves the default codec rather than moving a capable device elsewhere.
void TestAv1AloneWhenAllowed() {
    CHECK_TRUE(Candidates({Codec("H265"), Codec("H264"), Codec("VP8"), Codec("AV1")}, true) == Names({"AV1"}));
}

void TestH265ThenH264WhenNotAllowed() {
    CHECK_TRUE(Candidates({Codec("AV1"), Codec("VP8"), Codec("H264"), Codec("H265")}, false) == Names({"H265", "H264", "VP8"}));
    CHECK_TRUE(Candidates({Codec("AV1"), Codec("VP8"), Codec("H264"), Codec("H264")}, false) == Names({"H264", "H264", "VP8"}));
    CHECK_TRUE(Candidates({Codec("AV1"), Codec("VP9"), Codec("VP8")}, false) == Names({"VP9", "VP8"}));
}

// A peer without AV1 gets today's call: nothing is pinned, allowed or not.
void TestNoAv1NegotiatedPinsNothing() {
    CHECK_TRUE(Candidates({Codec("H264"), Codec("H265"), Codec("VP8")}, true).empty());
    CHECK_TRUE(Candidates({Codec("H264"), Codec("H265"), Codec("VP8")}, false).empty());
}

void TestSkipsRtxRedFec() {
    CHECK_TRUE(Candidates({Codec("AV1"), Codec("rtx"), Codec("red"), Codec("ulpfec"), Codec("flexfec-03"), Codec("VP8")}, false) ==
               Names({"VP8"}));
    CHECK_TRUE(Candidates({Codec("AV1"), Codec("rtx")}, false).empty());
}

// A pin the sender rejects (exact parameter equality, e.g. a peer's H265 level) moves on to the next candidate while
// the sender still sends AV1: a device told to leave AV1 must leave it.
void TestRejectedPinFallsBackWhileOnAv1() {
    std::vector<std::string> tried;
    const auto pinned = tgcalls::PinVideoSendCodec(
        tgcalls::VideoSendCodecCandidates({Codec("AV1"), Codec("H264"), Codec("H265")}, false),
        [&](const webrtc::RtpCodec &codec) {
            tried.push_back(codec.name);
            return codec.name != "H265";
        },
        [] { return true; });
    CHECK_TRUE(pinned.has_value() && pinned->name == "H264");
    CHECK_TRUE(tried == Names({"H265", "H264"}));
}

// Already off AV1, a rejected pin keeps the codec being sent rather than falling further down the list.
void TestRejectedPinKeepsANonAv1Codec() {
    std::vector<std::string> tried;
    const auto pinned = tgcalls::PinVideoSendCodec(
        tgcalls::VideoSendCodecCandidates({Codec("AV1"), Codec("H264"), Codec("H265")}, false),
        [&](const webrtc::RtpCodec &codec) {
            tried.push_back(codec.name);
            return false;
        },
        [] { return false; });
    CHECK_TRUE(!pinned.has_value());
    CHECK_TRUE(tried == Names({"H265"}));
}

std::vector<std::string> OverrideCandidates(const std::vector<webrtc::RtpCodec> &available, const std::string &name) {
    std::vector<std::string> names;
    for (const auto &codec : tgcalls::VideoSendCodecOverrideCandidates(available, name)) {
        names.push_back(codec.name);
    }
    return names;
}

void TestOverrideTakesEveryVariantOfTheNamedCodec() {
    // The debug override: the named codec alone, every negotiated variant of it (H264 has one per profile), whatever
    // the AV1 gates say; the name is matched as SDP matches it, case-insensitively.
    const std::vector<webrtc::RtpCodec> available = {Codec("AV1"), Codec("H264"), Codec("rtx"), Codec("H264"), Codec("VP8")};
    CHECK_TRUE(OverrideCandidates(available, "H264") == Names({"H264", "H264"}));
    CHECK_TRUE(OverrideCandidates(available, "h264") == Names({"H264", "H264"}));
    CHECK_TRUE(OverrideCandidates(available, "AV1") == Names({"AV1"}));
    CHECK_TRUE(OverrideCandidates(available, "VP8") == Names({"VP8"}));
}

void TestOverrideOfACodecNotNegotiatedIsEmpty() {
    CHECK_TRUE(OverrideCandidates({Codec("AV1"), Codec("H264")}, "VP9").empty());
    CHECK_TRUE(OverrideCandidates({Codec("AV1"), Codec("H264")}, "").empty());
    // Never a resilience codec: it is not a codec a sender can be pinned to.
    CHECK_TRUE(OverrideCandidates({Codec("H264"), Codec("rtx")}, "rtx").empty());
}

} // namespace

int main() {
    TestDefaults();
    TestKillswitches();
    TestAnyDeviceOverridesGate();
    TestTestKnobs();
    TestAv1AloneWhenAllowed();
    TestH265ThenH264WhenNotAllowed();
    TestNoAv1NegotiatedPinsNothing();
    TestSkipsRtxRedFec();
    TestRejectedPinFallsBackWhileOnAv1();
    TestRejectedPinKeepsANonAv1Codec();
    TestOverrideTakesEveryVariantOfTheNamedCodec();
    TestOverrideOfACodecNotNegotiatedIsEmpty();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all AV1 send codec checks passed\n");
    return 0;
}
