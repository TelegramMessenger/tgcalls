// The group calls' fixed video payload table: VP8/VP9/H264 keep their numbers, AV1 is always 106 with RTX 107, the AV1
// entry is the one the AV1 decoder factory lists, and how the SFU's list names AV1.

#include "group/GroupVideoPayloadTypes.h"
#include "platform/fake/FakeInterface.h"

#include "api/video_codecs/scalability_mode.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "media/base/media_constants.h"

#include <algorithm>
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

webrtc::SdpVideoFormat Vp8() {
    return webrtc::SdpVideoFormat(webrtc::kVp8CodecName);
}

webrtc::SdpVideoFormat Vp9() {
    return webrtc::SdpVideoFormat(webrtc::kVp9CodecName, {{"profile-id", "0"}});
}

webrtc::SdpVideoFormat H264() {
    return webrtc::SdpVideoFormat(webrtc::kH264CodecName, {
        {"level-asymmetry-allowed", "1"},
        {"packetization-mode", "1"},
        {"profile-level-id", "42e01f"},
    });
}

std::string Param(const webrtc::Codec &codec, const std::string &name) {
    std::string value;
    codec.GetParam(name, &value);
    return value;
}

const tgcalls::GroupVideoPayloadType *Find(const std::vector<tgcalls::GroupVideoPayloadType> &table, const std::string &name) {
    for (const auto &entry : table) {
        if (entry.videoCodec.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

void CheckEntry(const std::vector<tgcalls::GroupVideoPayloadType> &table, const std::string &name, int id, int rtxId) {
    const auto entry = Find(table, name);
    CHECK_TRUE(entry != nullptr);
    if (!entry) {
        return;
    }
    CHECK_TRUE(entry->videoCodec.id == id);
    CHECK_TRUE(entry->rtxCodec.has_value());
    if (entry->rtxCodec) {
        CHECK_TRUE(entry->rtxCodec->id == rtxId);
        CHECK_TRUE(Param(*entry->rtxCodec, "apt") == std::to_string(id));
    }
}

void TestTodayNumbersUnchanged() {
    const auto table = tgcalls::assignGroupVideoPayloadTypes(tgcalls::filterGroupVideoFormats({Vp8(), Vp9(), H264()}));
    CheckEntry(table, webrtc::kVp8CodecName, 100, 101);
    CheckEntry(table, webrtc::kVp9CodecName, 102, 103);
    CheckEntry(table, webrtc::kH264CodecName, 104, 105);
}

void TestAv1AlwaysLast() {
    const auto table = tgcalls::assignGroupVideoPayloadTypes(tgcalls::filterGroupVideoFormats({Vp8(), Vp9(), H264()}));
    CHECK_TRUE(!table.empty() && table.back().videoCodec.name == webrtc::kAv1CodecName);
    CheckEntry(table, webrtc::kAv1CodecName, tgcalls::kGroupAv1PayloadType, tgcalls::kGroupAv1RtxPayloadType);
    CHECK_TRUE(tgcalls::kGroupAv1PayloadType == 106);
    CHECK_TRUE(tgcalls::kGroupAv1RtxPayloadType == 107);
    if (const auto av1 = Find(table, webrtc::kAv1CodecName)) {
        CHECK_TRUE(Param(av1->videoCodec, "profile") == "0");
        CHECK_TRUE(Param(av1->videoCodec, "level-idx") == "5");
        CHECK_TRUE(Param(av1->videoCodec, "tier") == "0");
    }

    // Without VP9, H264 moves up (today's behaviour) and AV1 stays where it is.
    const auto withoutVp9 = tgcalls::assignGroupVideoPayloadTypes(tgcalls::filterGroupVideoFormats({Vp8(), H264()}));
    CheckEntry(withoutVp9, webrtc::kH264CodecName, 102, 103);
    CheckEntry(withoutVp9, webrtc::kAv1CodecName, 106, 107);
}

void TestAv1MatchesTheDecoderFactory() {
    tgcalls::FakeInterface platform;
    bool found = false;
    for (const auto &format : platform.makeVideoDecoderFactoryWithAv1()->GetSupportedFormats()) {
        if (format.name == webrtc::kAv1CodecName) {
            found = true;
            CHECK_TRUE(format.IsSameCodec(tgcalls::groupAv1VideoFormat()));
            CHECK_TRUE(format.parameters == tgcalls::groupAv1VideoFormat().parameters);
        }
    }
    CHECK_TRUE(found);
}

tgcalls::GroupJoinPayloadVideoPayloadType ServerEntry(std::string name) {
    tgcalls::GroupJoinPayloadVideoPayloadType entry;
    entry.name = std::move(name);
    return entry;
}

void TestServerListsAv1() {
    CHECK_TRUE(tgcalls::groupPayloadTypesListAv1({ServerEntry("H264"), ServerEntry("AV1")}));
    CHECK_TRUE(tgcalls::groupPayloadTypesListAv1({ServerEntry("av1")}));
    CHECK_TRUE(!tgcalls::groupPayloadTypesListAv1({ServerEntry("H264")}));
    CHECK_TRUE(!tgcalls::groupPayloadTypesListAv1({}));
    CHECK_TRUE(!tgcalls::groupPayloadTypesListAv1({ServerEntry("AV1X")}));
}

// The table's AV1 lists L1T1 and L1T3: a sender with AV1 on two layers sets L1T3 on each encoding (WebRTC sends AV1 as
// one stream otherwise), and SetRtpSendParameters accepts only a mode the send codec lists.
void TestAv1ListsL1T1AndL1T3() {
    bool found = false;
    for (const auto &payloadType : tgcalls::assignGroupVideoPayloadTypes({})) {
        if (payloadType.videoCodec.name == webrtc::kAv1CodecName) {
            found = true;
            const auto &modes = payloadType.videoCodec.scalability_modes;
            CHECK_TRUE(std::find(modes.begin(), modes.end(), webrtc::ScalabilityMode::kL1T1) != modes.end());
            CHECK_TRUE(std::find(modes.begin(), modes.end(), webrtc::ScalabilityMode::kL1T3) != modes.end());
        }
    }
    CHECK_TRUE(found);
}

// The AV1 Dependency Descriptor's header extension ID: the server's when its join response lists the URI, else 11;
// never an ID the server gives another extension.
void TestDependencyDescriptorExtensionId() {
    const std::string dd = "https://aomediacodec.github.io/av1-rtp-spec/#dependency-descriptor-rtp-header-extension";
    const std::vector<std::pair<uint32_t, std::string>> production = {
        {2, "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"},
        {3, "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"},
        {13, "urn:3gpp:video-orientation"},
    };
    CHECK_TRUE(tgcalls::groupDependencyDescriptorExtensionId(production) == 11);
    CHECK_TRUE(tgcalls::groupDependencyDescriptorExtensionId({}) == 11);

    auto listed = production;
    listed.push_back({9, dd});
    CHECK_TRUE(tgcalls::groupDependencyDescriptorExtensionId(listed) == 9);

    auto taken = production;
    taken.push_back({11, "urn:ietf:params:rtp-hdrext:sdes:mid"});
    CHECK_TRUE(tgcalls::groupDependencyDescriptorExtensionId(taken) == 0);

    CHECK_TRUE(tgcalls::kGroupDependencyDescriptorUri == dd);
}

// What GroupInstanceReferenceImpl's PeerConnection offers on a video m-line (its log, 2026-11-18).
std::vector<webrtc::RtpExtension> PeerConnectionVideoExtensions() {
    return {
        webrtc::RtpExtension("http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time", webrtc::RtpHeaderExtensionId(2)),
        webrtc::RtpExtension("http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01", webrtc::RtpHeaderExtensionId(3)),
        webrtc::RtpExtension("urn:ietf:params:rtp-hdrext:sdes:mid", webrtc::RtpHeaderExtensionId(4)),
        webrtc::RtpExtension("http://www.webrtc.org/experiments/rtp-hdrext/playout-delay", webrtc::RtpHeaderExtensionId(5)),
        webrtc::RtpExtension("http://www.webrtc.org/experiments/rtp-hdrext/video-content-type", webrtc::RtpHeaderExtensionId(6)),
        webrtc::RtpExtension("http://www.webrtc.org/experiments/rtp-hdrext/video-timing", webrtc::RtpHeaderExtensionId(7)),
        webrtc::RtpExtension("http://www.webrtc.org/experiments/rtp-hdrext/color-space", webrtc::RtpHeaderExtensionId(8)),
        webrtc::RtpExtension("urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id", webrtc::RtpHeaderExtensionId(10)),
        webrtc::RtpExtension("urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id", webrtc::RtpHeaderExtensionId(11)),
        webrtc::RtpExtension("urn:3gpp:video-orientation", webrtc::RtpHeaderExtensionId(13)),
        webrtc::RtpExtension("urn:ietf:params:rtp-hdrext:toffset", webrtc::RtpHeaderExtensionId(14)),
    };
}

const webrtc::RtpExtension *FindExtension(const std::vector<webrtc::RtpExtension> &extensions, const std::string &uri) {
    for (const auto &extension : extensions) {
        if (extension.uri == uri) {
            return &extension;
        }
    }
    return nullptr;
}

const webrtc::RtpExtension *FindExtensionId(const std::vector<webrtc::RtpExtension> &extensions, int id) {
    for (const auto &extension : extensions) {
        if (extension.id.value() == id) {
            return &extension;
        }
    }
    return nullptr;
}

// The reference engine's video header extensions: without the stream IDs (unused: SSRC simulcast), so that 10 and 11
// are free from the first offer on, and with the Dependency Descriptor at the given ID unless another extension holds
// it (an ID can't be re-mapped once negotiated).
void TestReferenceVideoHeaderExtensions() {
    const std::string dd = tgcalls::kGroupDependencyDescriptorUri;
    const auto offered = PeerConnectionVideoExtensions();

    // Before the join response (no ID yet): only the stream IDs go.
    const auto initial = tgcalls::groupReferenceVideoHeaderExtensions(offered, 0);
    CHECK_TRUE(initial.size() == offered.size() - 2);
    CHECK_TRUE(FindExtension(initial, "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id") == nullptr);
    CHECK_TRUE(FindExtension(initial, "urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id") == nullptr);
    CHECK_TRUE(FindExtension(initial, dd) == nullptr);
    CHECK_TRUE(FindExtension(initial, "urn:ietf:params:rtp-hdrext:sdes:mid") != nullptr);
    CHECK_TRUE(FindExtensionId(initial, 3) != nullptr && FindExtensionId(initial, 3)->uri == "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01");

    // The default ID, 11 (repaired-rtp-stream-id's in PeerConnection's list).
    const auto withDefault = tgcalls::groupReferenceVideoHeaderExtensions(offered, 11);
    CHECK_TRUE(FindExtension(withDefault, dd) != nullptr && FindExtension(withDefault, dd)->id.value() == 11);
    CHECK_TRUE(FindExtensionId(withDefault, 11) != nullptr && FindExtensionId(withDefault, 11)->uri == dd);
    CHECK_TRUE(withDefault.size() == offered.size() - 1);

    // A free ID the server lists.
    const auto atTwelve = tgcalls::groupReferenceVideoHeaderExtensions(offered, 12);
    CHECK_TRUE(FindExtension(atTwelve, dd) != nullptr && FindExtension(atTwelve, dd)->id.value() == 12);
    CHECK_TRUE(FindExtensionId(atTwelve, 11) == nullptr);

    // An ID another kept extension holds: no descriptor, and that extension stays.
    const auto taken = tgcalls::groupReferenceVideoHeaderExtensions(offered, 5);
    CHECK_TRUE(FindExtension(taken, dd) == nullptr);
    CHECK_TRUE(FindExtensionId(taken, 5) != nullptr && FindExtensionId(taken, 5)->uri == "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay");

    // Idempotent: a description already carrying it gets it once, at the ID asked for; 0 removes it.
    const auto again = tgcalls::groupReferenceVideoHeaderExtensions(withDefault, 11);
    CHECK_TRUE(again.size() == withDefault.size());
    int count = 0;
    for (const auto &extension : again) {
        if (extension.uri == dd) {
            count++;
        }
    }
    CHECK_TRUE(count == 1);
    CHECK_TRUE(FindExtension(tgcalls::groupReferenceVideoHeaderExtensions(withDefault, 0), dd) == nullptr);
}

} // namespace

int main() {
    TestTodayNumbersUnchanged();
    TestAv1AlwaysLast();
    TestAv1MatchesTheDecoderFactory();
    TestServerListsAv1();
    TestDependencyDescriptorExtensionId();
    TestAv1ListsL1T1AndL1T3();
    TestReferenceVideoHeaderExtensions();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("group_video_payload_types_test: all checks passed\n");
    return 0;
}
