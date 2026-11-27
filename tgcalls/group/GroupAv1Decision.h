#ifndef TGCALLS_GROUP_AV1_DECISION_H
#define TGCALLS_GROUP_AV1_DECISION_H

#include <string>

namespace tgcalls {

// How an AV1 screen share sends its video. Single: one layer, AV1 as the sender's codec (the production SFU forwarded no
// AV1 layer beside an H264 one, 2026-11-08). Top: the top of two or more layers in AV1, those below in H264. Every: each
// of two or more layers in AV1.
enum class GroupAv1ScreencastLayers {
    Single,
    Top,
    Every
};

// What decides whether a group screen share sends AV1 (GroupAv1ScreencastLayers says how).
struct GroupAv1TopLayerInputs {
    // Debug Settings "Group calls: AV1 screen sharing" (GroupInstanceDescriptor::allowAv1ScreencastSending).
    bool allowed = false;
    // The SFU's join response lists AV1 (groupPayloadTypesListAv1).
    bool serverListsAv1 = false;
    // PlatformInterface::canEncodeAv1Realtime(): the A18/M4 chip gate.
    bool deviceCanEncode = false;
    // The in-app ScreenCaptureKit share in the main app (GroupInstanceDescriptor::iosInAppScreencast).
    bool inAppScreencast = false;
    // End-to-end encrypted (e2eEncryptDecrypt set): the frame transformer encrypts the OBU headers the AV1 packetizer
    // must read.
    bool encrypted = false;
    // The codec the engine chose for the share (the layers below the top one).
    std::string baseCodecName;
    // The share's simulcast layers.
    int layerCount = 0;
    // GroupInstanceDescriptor::av1ScreencastLayers.
    GroupAv1ScreencastLayers layers = GroupAv1ScreencastLayers::Top;
};

struct GroupAv1TopLayerDecision {
    bool useAv1 = false;
    // The first condition that failed, when !useAv1: "not allowed", "not the in-app screen share", "encrypted",
    // "server did not list AV1", "chip", "base codec <name>", "single layer" (Top, Every), "layers <n>" (Single). Empty
    // when useAv1.
    std::string reason;
};

// After the server's join response. In Single the share needs exactly one layer, in Top and Every two or more; the base
// codec (H264) is the layers below the top one, or in Single the codec WebRTC switches to if AV1 fails.
GroupAv1TopLayerDecision decideGroupAv1TopLayer(GroupAv1TopLayerInputs const &inputs);

// Before the join (the share's SSRCs go into the join request): whether the share joins with one layer, i.e. Single and
// AV1 possible on this side (allowed, the in-app share, not encrypted, the chip). The server's answer and the base codec
// are not known yet; a server without AV1 then gets that one layer in H264.
bool groupAv1JoinsWithSingleLayer(GroupAv1TopLayerInputs const &inputs);

} // namespace tgcalls

#endif
