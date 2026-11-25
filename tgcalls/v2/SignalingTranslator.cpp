#include "v2/SignalingTranslator.h"

#include "api/jsep_ice_candidate.h"
#include "pc/media_protocol_names.h"
#include "rtc_base/crypto_random.h"
#include "rtc_base/ssl_fingerprint.h"
#include "rtc_base/logging.h"
#include "p2p/base/transport_description.h"

#include <algorithm>

namespace tgcalls {

namespace {

// InstanceV2Impl's data channel: SctpDataChannelProviderInterfaceImpl starts its
// transport with these values and never negotiates them.
const char kDataMid[] = "d";
const int kSctpPort = 5000;
const int kSctpMaxMessageSize = 262144;

// Never negotiated: mids are local to each side here (see the class comment),
// and RID/repaired-RID are MID-scoped.
std::vector<webrtc::RtpExtension> filterRtpExtensions(std::vector<webrtc::RtpExtension> const &extensions) {
    std::vector<webrtc::RtpExtension> result;
    for (const auto &extension : extensions) {
        if (extension.uri == webrtc::RtpExtension::kMidUri ||
            extension.uri == webrtc::RtpExtension::kRidUri ||
            extension.uri == webrtc::RtpExtension::kRepairedRidUri) {
            continue;
        }
        result.push_back(extension);
    }
    return result;
}

bool sendsMedia(webrtc::RtpTransceiverDirection direction) {
    return direction == webrtc::RtpTransceiverDirection::kSendOnly || direction == webrtc::RtpTransceiverDirection::kSendRecv;
}

bool receivesMedia(webrtc::RtpTransceiverDirection direction) {
    return direction == webrtc::RtpTransceiverDirection::kRecvOnly || direction == webrtc::RtpTransceiverDirection::kSendRecv;
}

bool isRtpMedia(webrtc::ContentInfo const &content) {
    const auto mediaDescription = content.media_description();
    if (!mediaDescription) {
        return false;
    }
    return mediaDescription->type() == webrtc::MediaType::AUDIO || mediaDescription->type() == webrtc::MediaType::VIDEO;
}

uint32_t firstSsrc(webrtc::ContentInfo const &content) {
    const auto &streams = content.media_description()->streams();
    if (streams.empty()) {
        return 0;
    }
    return streams[0].first_ssrc();
}

} // namespace

SignalingTranslator::SignalingTranslator(bool isOutgoing)
    : _isOutgoing(isOutgoing) {
}

std::optional<signaling::InitialSetupMessage> SignalingTranslator::takeInitialSetupIfChanged(
    const webrtc::SessionDescription *localDescription) {
    if (!localDescription) {
        return std::nullopt;
    }

    const webrtc::TransportInfo *transportInfo = nullptr;
    for (const auto &content : localDescription->contents()) {
        if (content.rejected) {
            continue;
        }
        transportInfo = localDescription->GetTransportInfoByName(content.mid());
        if (transportInfo) {
            break;
        }
    }
    if (!transportInfo) {
        return std::nullopt;
    }

    signaling::InitialSetupMessage result;
    result.ufrag = transportInfo->description.ice_ufrag;
    result.pwd = transportInfo->description.ice_pwd;
    result.supportsRenomination = true;

    signaling::DtlsFingerprint dtlsFingerprint;
    if (transportInfo->description.identity_fingerprint) {
        dtlsFingerprint.hash = transportInfo->description.identity_fingerprint->algorithm;
        dtlsFingerprint.fingerprint = transportInfo->description.identity_fingerprint->GetRfc4572Fingerprint();
    }
    // Not the role from our SDP: InstanceV2Impl applies `setup` on every
    // InitialSetupMessage it receives, so a mid-call "actpass" from a callee would
    // move a 13.0.0 caller's DTLS role. The remote description we synthesise
    // pins the same roles (makeRemoteTransportDescription).
    dtlsFingerprint.setup = _isOutgoing ? "actpass" : "passive";
    result.fingerprints.push_back(dtlsFingerprint);

    if (_lastSentSetup &&
        _lastSentSetup->ufrag == result.ufrag &&
        _lastSentSetup->pwd == result.pwd &&
        _lastSentSetup->fingerprint == dtlsFingerprint.fingerprint) {
        return std::nullopt;
    }
    _lastSentSetup = SentSetup{ result.ufrag, result.pwd, dtlsFingerprint.fingerprint };

    return result;
}

signaling::NegotiateChannelsMessage SignalingTranslator::makeOffer(
    const webrtc::SessionDescription *localOffer) {
    signaling::NegotiateChannelsMessage result;

    uint32_t exchangeId = 0;
    do {
        exchangeId = webrtc::CreateRandomId();
    } while (exchangeId == 0 || _sentOfferExchangeIds.find(exchangeId) != _sentOfferExchangeIds.end());
    result.exchangeId = exchangeId;
    _pendingOfferExchangeId = exchangeId;
    _sentOfferExchangeIds.insert(exchangeId);

    if (!localOffer) {
        return result;
    }

    for (const auto &content : localOffer->contents()) {
        if (content.rejected || !isRtpMedia(content)) {
            continue;
        }
        if (_remoteChannels.find(content.mid()) != _remoteChannels.end()) {
            continue;
        }
        if (!sendsMedia(content.media_description()->direction()) || firstSsrc(content) == 0) {
            continue;
        }
        auto mappedContent = signaling::convertContentInfoToSignalingContent(content);
        mappedContent.rtpExtensions = filterRtpExtensions(mappedContent.rtpExtensions);
        result.contents.push_back(std::move(mappedContent));
    }

    return result;
}

signaling::NegotiateChannelsMessage SignalingTranslator::makeAnswer(
    const webrtc::SessionDescription *localAnswer) const {
    signaling::NegotiateChannelsMessage result;
    result.exchangeId = _answeringExchangeId;

    if (!localAnswer) {
        return result;
    }

    for (const auto &content : localAnswer->contents()) {
        if (content.rejected || !isRtpMedia(content)) {
            continue;
        }
        const auto channel = _remoteChannels.find(content.mid());
        if (channel == _remoteChannels.end() || !channel->second.isOffered) {
            continue;
        }
        if (!receivesMedia(content.media_description()->direction())) {
            continue;
        }
        // Our section is recvonly and carries no ssrc; the answer names the
        // offerer's stream, as InstanceV2Impl's getAnswer does.
        auto mappedContent = signaling::convertContentInfoToSignalingContent(content);
        mappedContent.ssrc = channel->second.content.ssrc;
        mappedContent.ssrcGroups = channel->second.content.ssrcGroups;
        mappedContent.rtpExtensions = filterRtpExtensions(mappedContent.rtpExtensions);
        result.contents.push_back(std::move(mappedContent));
    }

    return result;
}

signaling::CandidatesMessage SignalingTranslator::extractCandidates(
    const webrtc::IceCandidate *candidate) const {
    signaling::CandidatesMessage result;

    std::string sdpString;
    if (candidate->ToString(&sdpString)) {
        signaling::IceCandidate iceCandidate;
        iceCandidate.sdpString = sdpString;
        result.iceCandidates.push_back(std::move(iceCandidate));
    }

    return result;
}

void SignalingTranslator::setRemoteSetup(signaling::InitialSetupMessage message) {
    _remoteSetup = std::move(message);
}

bool SignalingTranslator::hasRemoteSetup() const {
    return _remoteSetup.has_value();
}

SignalingTranslator::MessageKind SignalingTranslator::classify(const signaling::NegotiateChannelsMessage &message) {
    if (_pendingOfferExchangeId && _pendingOfferExchangeId.value() == message.exchangeId) {
        _pendingOfferExchangeId.reset();
        return MessageKind::Answer;
    }
    if (_sentOfferExchangeIds.find(message.exchangeId) != _sentOfferExchangeIds.end()) {
        return MessageKind::Stale;
    }
    return MessageKind::Offer;
}

bool SignalingTranslator::hasPendingOffer() const {
    return _pendingOfferExchangeId.has_value();
}

void SignalingTranslator::abandonPendingOffer() {
    _pendingOfferExchangeId.reset();
}

std::unique_ptr<webrtc::SessionDescriptionInterface> SignalingTranslator::buildRemoteOffer(
    const signaling::NegotiateChannelsMessage &offer,
    const webrtc::SessionDescription *currentLocal) {
    if (!_remoteSetup) {
        return nullptr;
    }

    _answeringExchangeId = offer.exchangeId;

    // The offer is the remote's complete set of outgoing channels.
    for (auto &it : _remoteChannels) {
        it.second.isOffered = false;
    }
    for (const auto &content : offer.contents) {
        if (content.ssrc == 0) {
            continue;
        }
        auto &channel = _remoteChannels[remoteChannelMid(content.ssrc)];
        channel.content = content;
        channel.content.rtpExtensions = filterRtpExtensions(content.rtpExtensions);
        channel.isOffered = true;
    }

    auto description = std::make_unique<webrtc::SessionDescription>();
    std::set<std::string> presentMids;
    bool hasApplication = false;

    if (currentLocal) {
        for (const auto &localContent : currentLocal->contents()) {
            presentMids.insert(localContent.mid());

            if (localContent.rejected || !localContent.media_description()) {
                addRejected(description.get(), localContent);
            } else if (localContent.type == webrtc::MediaProtocolType::kSctp) {
                addApplication(description.get(), localContent.mid(), &localContent);
                hasApplication = true;
            } else if (const auto channel = _remoteChannels.find(localContent.mid()); channel != _remoteChannels.end()) {
                addRemoteChannel(description.get(), localContent.mid(), channel->second);
            } else if (isRtpMedia(localContent)) {
                const signaling::MediaContent *acceptedContent = nullptr;
                if (sendsMedia(localContent.media_description()->direction())) {
                    const auto accepted = _acceptedLocalContents.find(firstSsrc(localContent));
                    if (accepted != _acceptedLocalContents.end()) {
                        acceptedContent = &accepted->second;
                    }
                }
                addLocalChannelMirror(description.get(), localContent, acceptedContent);
            } else {
                addRejected(description.get(), localContent);
            }
        }
    }

    for (const auto &content : offer.contents) {
        if (content.ssrc == 0) {
            continue;
        }
        const auto mid = remoteChannelMid(content.ssrc);
        if (!presentMids.insert(mid).second) {
            continue;
        }
        addRemoteChannel(description.get(), mid, _remoteChannels[mid]);
    }

    if (!hasApplication && presentMids.find(kDataMid) == presentMids.end()) {
        // Before our first exchange as the callee; InstanceV2Impl's data
        // channel is always there.
        addApplication(description.get(), kDataMid, nullptr);
    }

    return finishDescription(std::move(description), webrtc::SdpType::kOffer);
}

std::unique_ptr<webrtc::SessionDescriptionInterface> SignalingTranslator::buildRemoteAnswer(
    const signaling::NegotiateChannelsMessage &answer,
    const webrtc::SessionDescription *localOffer) {
    if (!_remoteSetup || !localOffer) {
        return nullptr;
    }

    std::map<uint32_t, signaling::MediaContent> acceptedBySsrc;
    for (const auto &content : answer.contents) {
        if (content.ssrc == 0) {
            continue;
        }
        auto mappedContent = content;
        mappedContent.rtpExtensions = filterRtpExtensions(content.rtpExtensions);
        acceptedBySsrc[content.ssrc] = std::move(mappedContent);
    }

    auto description = std::make_unique<webrtc::SessionDescription>();

    for (const auto &localContent : localOffer->contents()) {
        if (localContent.rejected || !localContent.media_description()) {
            addRejected(description.get(), localContent);
        } else if (localContent.type == webrtc::MediaProtocolType::kSctp) {
            addApplication(description.get(), localContent.mid(), &localContent);
        } else if (const auto channel = _remoteChannels.find(localContent.mid()); channel != _remoteChannels.end()) {
            addRemoteChannel(description.get(), localContent.mid(), channel->second);
        } else if (isRtpMedia(localContent)) {
            const signaling::MediaContent *acceptedContent = nullptr;
            const auto ssrc = firstSsrc(localContent);
            if (sendsMedia(localContent.media_description()->direction()) && ssrc != 0) {
                const auto accepted = acceptedBySsrc.find(ssrc);
                if (accepted != acceptedBySsrc.end()) {
                    _acceptedLocalContents[ssrc] = accepted->second;
                    acceptedContent = &_acceptedLocalContents[ssrc];
                } else {
                    _acceptedLocalContents.erase(ssrc);
                }
            }
            addLocalChannelMirror(description.get(), localContent, acceptedContent);
        } else {
            addRejected(description.get(), localContent);
        }
    }

    return finishDescription(std::move(description), webrtc::SdpType::kAnswer);
}

std::vector<std::unique_ptr<webrtc::IceCandidate>> SignalingTranslator::parseCandidates(
    const signaling::CandidatesMessage &message) const {
    std::vector<std::unique_ptr<webrtc::IceCandidate>> result;

    for (const auto &candidate : message.iceCandidates) {
        webrtc::SdpParseError parseError;
        // No mid: ours are not the peer's, and every section is bundled onto
        // one transport, so the first m-line is always the right one.
        webrtc::IceCandidate *iceCandidate = webrtc::CreateIceCandidate("", 0, candidate.sdpString, &parseError);
        if (iceCandidate) {
            result.emplace_back(std::unique_ptr<webrtc::IceCandidate>(iceCandidate));
        } else {
            RTC_LOG(LS_ERROR) << "SignalingTranslator: Could not parse candidate: " << candidate.sdpString;
        }
    }

    return result;
}

std::string SignalingTranslator::remoteChannelMid(uint32_t ssrc) {
    const auto it = _remoteChannelMidsBySsrc.find(ssrc);
    if (it != _remoteChannelMidsBySsrc.end()) {
        return it->second;
    }
    // PeerConnection's own mids are numeric, so these never collide with them.
    std::string mid = "r" + std::to_string(ssrc);
    _remoteChannelMidsBySsrc[ssrc] = mid;
    return mid;
}

webrtc::TransportDescription SignalingTranslator::makeRemoteTransportDescription() const {
    webrtc::TransportDescription transportDescription;
    transportDescription.ice_ufrag = _remoteSetup->ufrag;
    transportDescription.ice_pwd = _remoteSetup->pwd;
    transportDescription.ice_mode = webrtc::ICEMODE_FULL;

    if (!_remoteSetup->fingerprints.empty()) {
        auto fingerprint = webrtc::SSLFingerprint::CreateUniqueFromRfc4572(
            _remoteSetup->fingerprints[0].hash, _remoteSetup->fingerprints[0].fingerprint);
        if (fingerprint) {
            transportDescription.identity_fingerprint = std::move(fingerprint);
        }
    }
    // InstanceV2Impl's roles: the caller is the DTLS client and the callee the
    // server, for the whole call. Pinning the remote role in every description
    // keeps PeerConnection there too, whichever side offers.
    transportDescription.connection_role = _isOutgoing ? webrtc::CONNECTIONROLE_PASSIVE : webrtc::CONNECTIONROLE_ACTIVE;

    return transportDescription;
}

void SignalingTranslator::addRemoteChannel(webrtc::SessionDescription *description, std::string const &mid, RemoteChannel const &channel) const {
    auto contentInfo = signaling::convertSignalingContentToContentInfo(
        mid, channel.content,
        channel.isOffered ? webrtc::RtpTransceiverDirection::kSendOnly : webrtc::RtpTransceiverDirection::kInactive);
    auto mediaDescription = contentInfo.media_description();
    if (!channel.isOffered) {
        mediaDescription->mutable_streams().clear();
    }
    mediaDescription->set_rtp_header_extensions(filterRtpExtensions(mediaDescription->rtp_header_extensions()));
    description->AddContent(std::move(contentInfo));
}

void SignalingTranslator::addLocalChannelMirror(webrtc::SessionDescription *description, webrtc::ContentInfo const &localContent, signaling::MediaContent const *acceptedContent) const {
    std::unique_ptr<webrtc::MediaContentDescription> mediaDescription;
    if (acceptedContent) {
        auto contentInfo = signaling::convertSignalingContentToContentInfo(
            localContent.mid(), *acceptedContent, webrtc::RtpTransceiverDirection::kRecvOnly);
        mediaDescription = contentInfo.media_description()->Clone();
    } else {
        // Not (or no longer) received by the peer.
        mediaDescription = localContent.media_description()->Clone();
        mediaDescription->set_direction(webrtc::RtpTransceiverDirection::kInactive);
    }
    // The peer receives this stream, it does not send one. An ssrc here would be
    // ssrc 0 at best, and two of those in one BUNDLE group fail demuxing.
    mediaDescription->mutable_streams().clear();
    mediaDescription->set_rtp_header_extensions(filterRtpExtensions(mediaDescription->rtp_header_extensions()));

    webrtc::ContentInfo contentInfo(webrtc::MediaProtocolType::kRtp, localContent.mid(), std::move(mediaDescription));
    contentInfo.rejected = false;
    contentInfo.bundle_only = false;
    description->AddContent(std::move(contentInfo));
}

void SignalingTranslator::addApplication(webrtc::SessionDescription *description, std::string const &mid, webrtc::ContentInfo const *localContent) {
    std::unique_ptr<webrtc::MediaContentDescription> mediaDescription;
    if (localContent && localContent->media_description()) {
        mediaDescription = localContent->media_description()->Clone();
    } else {
        auto sctpDescription = std::make_unique<webrtc::SctpDataContentDescription>();
        sctpDescription->set_protocol(webrtc::kMediaProtocolUdpDtlsSctp);
        sctpDescription->set_use_sctpmap(false);
        sctpDescription->set_port(kSctpPort);
        sctpDescription->set_max_message_size(kSctpMaxMessageSize);
        mediaDescription = std::move(sctpDescription);
    }

    webrtc::ContentInfo contentInfo(webrtc::MediaProtocolType::kSctp, mid, std::move(mediaDescription));
    contentInfo.rejected = false;
    contentInfo.bundle_only = false;
    description->AddContent(std::move(contentInfo));
}

void SignalingTranslator::addRejected(webrtc::SessionDescription *description, webrtc::ContentInfo const &localContent) {
    webrtc::ContentInfo contentInfo(localContent.type, localContent.mid(),
                                    localContent.media_description() ? localContent.media_description()->Clone() : nullptr);
    contentInfo.rejected = true;
    contentInfo.bundle_only = false;
    description->AddContent(std::move(contentInfo));
}

std::unique_ptr<webrtc::SessionDescriptionInterface> SignalingTranslator::finishDescription(
    std::unique_ptr<webrtc::SessionDescription> description,
    webrtc::SdpType type) const {
    const auto transportDescription = makeRemoteTransportDescription();

    webrtc::ContentGroup bundleGroup(webrtc::GROUP_TYPE_BUNDLE);
    std::vector<std::string> contentNames;
    for (const auto &content : description->contents()) {
        contentNames.push_back(content.mid());
        // A rejected m-line is not part of BUNDLE (RFC 8843 7.3.3); listing one
        // fails "A BUNDLE group in answer contains a MID='x' that was not in
        // the offered group".
        if (!content.rejected) {
            bundleGroup.AddContentName(content.mid());
        }
    }
    for (const auto &name : contentNames) {
        description->AddTransportInfo(webrtc::TransportInfo(name, transportDescription));
    }
    if (!bundleGroup.content_names().empty()) {
        description->AddGroup(bundleGroup);
    }

    return webrtc::CreateSessionDescription(type, "0", "0", std::move(description));
}

}  // namespace tgcalls
