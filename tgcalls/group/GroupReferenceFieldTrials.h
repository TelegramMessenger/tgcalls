#ifndef TGCALLS_GROUP_REFERENCE_FIELD_TRIALS_H
#define TGCALLS_GROUP_REFERENCE_FIELD_TRIALS_H

#include "api/field_trials_view.h"

#include <memory>
#include <string>

#include "absl/strings/string_view.h"

namespace tgcalls {

// Field trials for GroupInstanceReferenceImpl's PeerConnectionFactory.
//
// Video packets whose SSRC no m-section signals must be dropped, never turned
// into a default (unsignaled) receive stream. A packet the network thread has
// already handed to the worker when the worker removes an endpoint's receive
// stream (the app requested fewer endpoints) is otherwise received as unknown
// and creates such a stream on the old, still-open transceiver's channel.
// That channel's remote content never changes again, so the stream is never
// reset, and it keeps the SSRC's binding in the Call's demuxer: when the same
// endpoint is requested again, the new transceiver's receive stream cannot
// register for the SSRC, the old stream decodes every frame into a sink that
// was detached with the endpoint ("VideoReceiveStreamInterface not connected
// to a VideoSink"), and the participant's tile stays frozen for the rest of
// the call. Signaled SSRCs are always available here: buildRemoteAnswer puts
// every requested endpoint's SSRC groups into its m-section.
//
// Top: the screen share's encodings name different codecs (setGroupAv1TopLayerCodecs), WebRTC's mixed-codec simulcast.
//
// An iOS in-app screen capture's connection also answers iosScreencastFieldTrial's keys (its periodic
// bandwidth probes stop near the cap); being this factory's own view, it changes no other connection.
//
// Every other key is unset: nothing comes from the process-wide trial string, which other engines rewrite (the custom
// group engine of a ReplayKit share started during this call, the 1:1 engines). Test: group_reference_field_trials_test.
class GroupReferenceFieldTrials final : public webrtc::FieldTrialsView {
public:
    explicit GroupReferenceFieldTrials(bool iosInAppScreencast);

    std::string Lookup(absl::string_view key) const override;

    // An Environment may copy its field trials; the default aborts.
    std::unique_ptr<webrtc::FieldTrialsView> CreateCopy() const override;

private:
    bool _iosInAppScreencast = false;
};

} // namespace tgcalls

#endif
