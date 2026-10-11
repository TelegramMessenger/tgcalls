#ifndef TGCALLS_SIGNALING_TRANSLATOR_H_
#define TGCALLS_SIGNALING_TRANSLATOR_H_

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "v2/Signaling.h"
#include "api/jsep.h"
#include "api/jsep_session_description.h"
#include "pc/session_description.h"
#include "api/peer_connection_interface.h"

namespace tgcalls {

// Speaks InstanceV2Impl's channel protocol (call versions 7.0.0-13.0.0) on behalf
// of a PeerConnection.
//
// In that protocol media is a set of one-way channels. An offer lists the
// sender's own outgoing channels - the complete set, the receiver replaces what
// it had - and an answer echoes the offer's exchangeId and lists the offerer's
// channels the answerer accepted. Each side offers its own channels. The data
// channel is not negotiated at all: it is SCTP on port 5000 over the same
// transport, always present.
//
// The local PeerConnection negotiates against a remote description synthesised
// here from those messages; the peer never sees our SDP. Mids are therefore
// local to each side: remote channels get mids of our choosing ("r<ssrc>"), and
// the MID RTP header extension is never negotiated, because the demuxer drops
// RTP whose MID it does not know.
class SignalingTranslator {
public:
    explicit SignalingTranslator(bool isOutgoing);

    // Outbound.

    // The InitialSetupMessage to send ahead of the next NegotiateChannels, or
    // nullopt when the local ICE credentials and fingerprint are unchanged since
    // the last one sent. `setup` is fixed by role, exactly as InstanceV2Impl does.
    absl::optional<signaling::InitialSetupMessage> takeInitialSetupIfChanged(
        const cricket::SessionDescription *localDescription);
    // Our offer: our sending sections, under a fresh exchangeId that becomes the
    // pending offer.
    signaling::NegotiateChannelsMessage makeOffer(
        const cricket::SessionDescription *localOffer);
    // Our answer to the remote offer last given to buildRemoteOffer.
    signaling::NegotiateChannelsMessage makeAnswer(
        const cricket::SessionDescription *localAnswer) const;
    signaling::CandidatesMessage extractCandidates(
        const webrtc::IceCandidateInterface *candidate) const;

    // Inbound.

    void setRemoteSetup(signaling::InitialSetupMessage message);
    bool hasRemoteSetup() const;

    enum class MessageKind {
        // Answers the pending offer, which stops being pending.
        Answer,
        // Answers an offer we sent earlier and no longer wait for (answered,
        // rolled back or failed).
        Stale,
        Offer
    };
    // Classify on receipt, before anything else is processed, so that an offer
    // following the answer it trails is not mistaken for glare.
    MessageKind classify(const signaling::NegotiateChannelsMessage &message);
    bool hasPendingOffer() const;
    // The pending offer was rolled back or failed: an answer to it is stale.
    void abandonPendingOffer();

    // A remote offer whose m-lines follow `currentLocal` (null before the first
    // exchange), with the remote's new channels appended.
    std::unique_ptr<webrtc::SessionDescriptionInterface> buildRemoteOffer(
        const signaling::NegotiateChannelsMessage &offer,
        const cricket::SessionDescription *currentLocal);
    // A remote answer mirroring `localOffer`.
    std::unique_ptr<webrtc::SessionDescriptionInterface> buildRemoteAnswer(
        const signaling::NegotiateChannelsMessage &answer,
        const cricket::SessionDescription *localOffer);

    std::vector<std::unique_ptr<webrtc::IceCandidateInterface>> parseCandidates(
        const signaling::CandidatesMessage &message) const;

private:
    struct RemoteChannel {
        signaling::MediaContent content;
        // Listed in the remote's latest offer.
        bool isOffered = false;
    };

    std::string remoteChannelMid(uint32_t ssrc);
    cricket::TransportDescription makeRemoteTransportDescription() const;
    void addRemoteChannel(cricket::SessionDescription *description, std::string const &mid, RemoteChannel const &channel) const;
    void addLocalChannelMirror(cricket::SessionDescription *description, cricket::ContentInfo const &localContent, signaling::MediaContent const *acceptedContent) const;
    static void addApplication(cricket::SessionDescription *description, std::string const &mid, cricket::ContentInfo const *localContent);
    static void addRejected(cricket::SessionDescription *description, cricket::ContentInfo const &localContent);
    std::unique_ptr<webrtc::SessionDescriptionInterface> finishDescription(
        std::unique_ptr<cricket::SessionDescription> description,
        webrtc::SdpType type) const;

    bool _isOutgoing = false;

    absl::optional<signaling::InitialSetupMessage> _remoteSetup;

    absl::optional<uint32_t> _pendingOfferExchangeId;
    std::set<uint32_t> _sentOfferExchangeIds;
    uint32_t _answeringExchangeId = 0;

    struct SentSetup {
        std::string ufrag;
        std::string pwd;
        std::string fingerprint;
    };
    absl::optional<SentSetup> _lastSentSetup;

    // Keyed by the mid we gave the channel.
    std::map<std::string, RemoteChannel> _remoteChannels;
    std::map<uint32_t, std::string> _remoteChannelMidsBySsrc;
    // What the remote accepted of our channels, by our ssrc.
    std::map<uint32_t, signaling::MediaContent> _acceptedLocalContents;
};

}  // namespace tgcalls

#endif  // TGCALLS_SIGNALING_TRANSLATOR_H_
