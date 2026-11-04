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

std::optional<std::string> Chosen(const std::vector<webrtc::RtpCodec> &available, bool av1Allowed) {
    auto codec = tgcalls::ChooseVideoSendCodec(available, av1Allowed);
    if (!codec) {
        return std::nullopt;
    }
    return codec->name;
}

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

void TestAv1ChosenWhenAllowed() {
    CHECK_TRUE(Chosen({Codec("H265"), Codec("H264"), Codec("VP8"), Codec("AV1")}, true) == std::optional<std::string>("AV1"));
}

void TestH265ThenH264WhenNotAllowed() {
    CHECK_TRUE(Chosen({Codec("AV1"), Codec("VP8"), Codec("H264"), Codec("H265")}, false) == std::optional<std::string>("H265"));
    CHECK_TRUE(Chosen({Codec("AV1"), Codec("VP8"), Codec("H264")}, false) == std::optional<std::string>("H264"));
    CHECK_TRUE(Chosen({Codec("AV1"), Codec("VP9"), Codec("VP8")}, false) == std::optional<std::string>("VP9"));
}

void TestNoAv1Negotiated() {
    CHECK_TRUE(Chosen({Codec("H264"), Codec("H265"), Codec("VP8")}, true) == std::optional<std::string>("H265"));
}

void TestSkipsRtxRedFec() {
    CHECK_TRUE(Chosen({Codec("AV1"), Codec("rtx"), Codec("red"), Codec("ulpfec"), Codec("flexfec-03"), Codec("VP8")}, false) ==
               std::optional<std::string>("VP8"));
    CHECK_TRUE(!Chosen({Codec("AV1"), Codec("rtx")}, false).has_value());
}

} // namespace

int main() {
    TestDefaults();
    TestKillswitches();
    TestAnyDeviceOverridesGate();
    TestTestKnobs();
    TestAv1ChosenWhenAllowed();
    TestH265ThenH264WhenNotAllowed();
    TestNoAv1Negotiated();
    TestSkipsRtxRedFec();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all AV1 send codec checks passed\n");
    return 0;
}
