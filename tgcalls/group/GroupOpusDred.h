#ifndef TGCALLS_GROUP_OPUS_DRED_H
#define TGCALLS_GROUP_OPUS_DRED_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "api/audio_codecs/audio_decoder_factory.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/field_trials_view.h"
#include "api/scoped_refptr.h"

namespace tgcalls {

// Opus DRED in group calls (telegram-ios docs/superpowers/specs/2026-11-26-opus-dred-group-calls-design.md).
//
// A listener on bad Wi-Fi loses packets on the SFU's leg to it, which the sender never hears about, so a sender with
// DRED on (`GroupInstanceDescriptor::dredFixedPacketLossPercent` > 0) tells libopus a fixed loss figure, whatever the
// network reports, at a fixed bitrate, without in-band FEC: libopus then puts up to 1 s of DRED in every packet. A
// listener with DRED receiving on (`dredReceiveEnabled`) rebuilds lost audio from it (`WebRTC-Audio-OpusDred`, the
// WebRTC fork's TGCALLS PATCH: Opus DRED) and conceals the rest with Opus's neural concealment
// (`WebRTC-Audio-OpusGeneratePlc`). Test: group_opus_dred_test.
//
// libopus is built without its neural weights (about 2 MB): the app gives them as `GroupInstanceDescriptor::
// opusDnnWeights`, the blob libopus's dnn/write_lpcnet_weights.c writes (//third-party/opus:opus_dnn_weights). Without
// them, or with a blob that is not well formed, the call has no DRED, sending or receiving.

constexpr int kGroupDredBitrateBps = 40000;
constexpr int kGroupDredDuration10ms = 100;

// libopus's neural weights blob; shared, since every Opus encoder and decoder of the call keeps it (libopus reads the
// models in place).
using GroupOpusDnnWeights = std::shared_ptr<const std::vector<uint8_t>>;

// Whether this build's WebRTC has Opus DRED: Bazel defines TGCALLS_ENABLE_OPUS_DRED (for GroupOpusDred.cpp, the one file
// that reads it); Telegram-Mac's Package.swift and tdesktop do not, since their WebRTC has no DRED. Without it nothing
// here asks for DRED, whatever the descriptor says. Test: group_opus_dred_unavailable_test.
bool groupOpusDredAvailable();

// Whether `weights` can give this call DRED: this build has it and the blob is well formed.
bool groupOpusDnnWeightsUsable(const GroupOpusDnnWeights &weights);

// What the engines take from GroupInstanceDescriptor: the fixed loss figure clamped to 0...100, and 0 (no DRED sent)
// where DRED is unavailable or the weights are not usable; receiving only where both are there.
int groupDredFixedPacketLossPercent(int requested, bool weightsUsable);
bool groupDredReceiveEnabled(bool requested, bool weightsUsable);

// The group engines' audio encoder factory: Opus (and L16 when asked), as webrtc::CreateAudioEncoderFactory<...>;
// with `dredFixedPacketLossPercent` > 0 (clamped to 100) and usable `weights` every Opus encoder gets 1 s of DRED,
// that fixed loss figure, kGroupDredBitrateBps and no in-band FEC.
webrtc::scoped_refptr<webrtc::AudioEncoderFactory> CreateGroupAudioEncoderFactory(int dredFixedPacketLossPercent,
                                                                                  GroupOpusDnnWeights weights,
                                                                                  bool includeL16);

// The group engines' audio decoder factory: Opus (and L16 when asked), as webrtc::CreateAudioDecoderFactory<...>,
// whose Opus decoders get usable `weights` (they rebuild from DRED only with WebRTC-Audio-OpusDred).
webrtc::scoped_refptr<webrtc::AudioDecoderFactory> CreateGroupAudioDecoderFactory(GroupOpusDnnWeights weights,
                                                                                  bool includeL16);

// For the engines' log: "send 25%, receive on, weights 1998208 bytes", "…, no weights", "…, weights N bytes, not
// usable" or "… (not in this build)".
std::string groupOpusDredDescription(int dredFixedPacketLossPercent, bool receive, const GroupOpusDnnWeights &weights);

// The value of a DRED field trial for the reference engine's own trials (GroupReferenceFieldTrials): receiving turns
// on WebRTC-Audio-OpusDred and WebRTC-Audio-OpusGeneratePlc, sending pins WebRTC-Audio-Allocation at 40 kbit/s.
// Empty for any other key, and for these when off.
std::string groupOpusDredFieldTrial(absl::string_view key, bool receive, bool send);

// The custom group engine's whole process-wide trial string (GroupInstanceCustomImpl::start): today's, with the audio
// allocation at 40 instead of 32 kbit/s when sending DRED and the two receive trials when receiving. Static storage:
// webrtc::DeprecatedGlobalFieldTrials::Set keeps the pointer.
const char *groupCustomEngineFieldTrials(bool receive, bool send);

// The trial string a custom-engine connection sets when it starts. A main connection's own flags, remembered until it
// ends; a screen share's connection (a ReplayKit share runs its own custom engine beside the call's) takes the running
// main connection's flags instead of its own, so it does not undo the call's DRED trials: the string is process-wide
// and read whenever a stream is created. The share's audio then also runs at the call's allocation (40 kbit/s with
// DRED). With no custom main connection running (the reference engine sets no process-wide trials), a share takes
// its own. A main connection gets `mainConnectionToken` for groupCustomEngineMainConnectionEnded.
const char *groupCustomEngineFieldTrialsForConnection(bool isScreencast, bool receive, bool send,
                                                      uint64_t *mainConnectionToken = nullptr);

// A main connection that groupCustomEngineFieldTrialsForConnection gave `mainConnectionToken` has ended: its flags are
// forgotten, unless a newer main connection has replaced them.
void groupCustomEngineMainConnectionEnded(uint64_t mainConnectionToken);

// Opus DRED in 1:1 calls (InstanceV2ReferenceImpl; telegram-ios
// docs/superpowers/specs/2026-11-26-opus-dred-p2p-calls-design.md), in libopus's adaptive mode: the encoder gets 1 s of
// DRED and the weights, nothing else, so libopus plans DRED from the loss WebRTC hears from the peer (none at 0%), with
// in-band FEC as negotiated. The call's JSON parameters switch it (`audio_dred`, default on); the weights are
// `Descriptor::opusDnnWeights`. Test: group_opus_dred_test.

// The call's audio bitrate with DRED on, DRED included (owner, 2026-11-26): it floats between these with the bandwidth
// estimate (CreateP2PFieldTrials' WebRTC-Audio-Allocation; the audio sender's max_bitrate_bps is the max). Without
// DRED, today's fixed 32 * 1024 (WebRTC's voice engine sends at the sender's max: min = max).
constexpr int kP2PDredMinBitrateBps = 48000;
constexpr int kP2PDredMaxBitrateBps = 64000;

// Whether a 1:1 call has DRED, sending and receiving: asked for, in this build, with usable weights.
bool p2pOpusDredEnabled(bool requested, const GroupOpusDnnWeights &weights);

// InstanceV2ReferenceImpl's audio encoder factory: Opus, as webrtc::CreateAudioEncoderFactory<AudioEncoderOpus>; with
// `dred` and usable `weights` every Opus encoder gets 1 s of DRED and the weights. Its decoders:
// CreateGroupAudioDecoderFactory(weights, false).
webrtc::scoped_refptr<webrtc::AudioEncoderFactory> CreateP2PAudioEncoderFactory(bool dred, GroupOpusDnnWeights weights);

// The 1:1 call's own field trials, for every Environment the engine creates. With `dred` (in this build): the two DRED
// receive trials, and WebRTC-Audio-Allocation letting the audio float between kP2PDredMinBitrateBps and
// kP2PDredMaxBitrateBps with the bandwidth estimate. Every other key unset; nothing from the process-wide string. A
// view of tgcalls' own, as GroupReferenceFieldTrials, not webrtc::FieldTrials: in older WebRTCs (another tgcalls
// build's) that registers itself process-wide and allows one instance.
std::unique_ptr<webrtc::FieldTrialsView> CreateP2PFieldTrials(bool dred);

// For the engine's log: "on, weights 1998208 bytes", "off (audio_dred false)", "off, no weights", "off, weights N bytes,
// not usable" or "off (not in this build)".
std::string p2pOpusDredDescription(bool requested, const GroupOpusDnnWeights &weights);

} // namespace tgcalls

#endif
