#ifndef TGCALLS_GROUP_VIDEO_PAYLOAD_TYPES_H
#define TGCALLS_GROUP_VIDEO_PAYLOAD_TYPES_H

#include <optional>
#include "api/rtp_parameters.h"
#include "api/video_codecs/sdp_video_format.h"
#include "media/base/codec.h"

#include "group/GroupJoinPayload.h"

#include <vector>

namespace tgcalls {

// Group calls never negotiate video payload types per pair: every client numbers its codecs with this table and the
// SFU forwards RTP unchanged, so the numbers are a protocol fact shared with every other client. VP8 100, VP9 102,
// H264 104, each with its RTX at +1 (numbered in that order over the codecs a device has), and, in builds with AV1
// (TGCALLS_ENABLE_AV1), AV1 106 with RTX 107 on every device. GroupInstanceCustomImpl uses the table for its channels;
// GroupInstanceReferenceImpl pins the same numbers in its offers and answers.
struct GroupVideoPayloadType {
    webrtc::Codec videoCodec;
    std::optional<webrtc::Codec> rtxCodec;
};

constexpr int kGroupAv1PayloadType = 106;
constexpr int kGroupAv1RtxPayloadType = 107;

// The formats the table is built from: VP8, the profile-0 VP9 (else the first VP9), and the preferred H264 (constrained
// high, then constrained baseline; packetization mode 1; level asymmetry allowed).
std::vector<webrtc::SdpVideoFormat> filterGroupVideoFormats(std::vector<webrtc::SdpVideoFormat> const &formats);

// The table for `formats` (filterGroupVideoFormats' output), then AV1 106/107 in builds with AV1, whatever `formats`
// holds: every such build receives AV1.
std::vector<GroupVideoPayloadType> assignGroupVideoPayloadTypes(std::vector<webrtc::SdpVideoFormat> const &formats);

// The table's AV1 entry: the SDP defaults the AV1 factories list (profile 0, level-idx 5, tier 0).
webrtc::SdpVideoFormat groupAv1VideoFormat();

// Whether the SFU's join response lists AV1: an entry named AV1 (any case); its id and parameters are not used.
bool groupPayloadTypesListAv1(std::vector<GroupJoinPayloadVideoPayloadType> const &serverPayloadTypes);

// The AV1 RTP Dependency Descriptor (webrtc::RtpExtension::kDependencyDescriptorUri). The SFU forwards an AV1 stream
// only with it: it finds frames, keyframes and their dependencies there.
constexpr const char *kGroupDependencyDescriptorUri =
    "https://aomediacodec.github.io/av1-rtp-spec/#dependency-descriptor-rtp-header-extension";
constexpr uint32_t kGroupDependencyDescriptorDefaultExtensionId = 11;

// The descriptor's header extension ID: the one the join response's video "rtp-hdrexts" give its URI, else
// kGroupDependencyDescriptorDefaultExtensionId (the production server did not list it in 2026-11), or 0 (none) when
// the server gives that ID to another extension.
uint32_t groupDependencyDescriptorExtensionId(std::vector<std::pair<uint32_t, std::string>> const &serverExtensionMap);

// GroupInstanceReferenceImpl's video header extensions for an AV1 screen share: every video m-line of its offers (with
// no descriptor) and its answer's outgoing m-line. PeerConnection offers rtp-stream-id and repaired-rtp-stream-id (10
// and 11), which an SSRC-signalled group sender never uses and whose IDs the descriptor may need; an ID can't be
// re-mapped once negotiated (ValidateRtpExtensions), so they go from the first offer on. Returns `extensions` without
// those two and without the descriptor, then the descriptor at `dependencyDescriptorId` when that is non-zero and no
// kept extension holds it.
std::vector<webrtc::RtpExtension> groupReferenceVideoHeaderExtensions(std::vector<webrtc::RtpExtension> const &extensions, uint32_t dependencyDescriptorId);

// The screen share's Top (GroupAv1ScreencastLayers::Top) is WebRTC's mixed-codec simulcast (WebRTC-MixedCodecSimulcast):
// each encoding names its codec, from `parameters.codecs` (the negotiated ones): H264 on every layer but the top one,
// AV1 on the top one while `av1Top`, else H264 there too (after AV1 failed; an encoding left without a codec would
// keep the last one). False, with `parameters` unchanged, with fewer than two encodings or without either codec.
bool setGroupAv1TopLayerCodecs(webrtc::RtpParameters &parameters, bool av1Top);

} // namespace tgcalls

#endif
