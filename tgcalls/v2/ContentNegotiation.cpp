#include "v2/ContentNegotiation.h"

#include "rtc_base/rtc_certificate_generator.h"
#include "media/base/media_engine.h"
#include "call/payload_type.h"
#include "call/payload_type_picker.h"
#include "pc/codec_vendor.h"
#include "pc/rtp_parameters_conversion.h"
#include "media/base/codec.h"
#include <map>
#include <sstream>

namespace tgcalls {

namespace {

signaling::MediaContent convertContentInfoToSingalingContent(webrtc::ContentInfo const &content) {
    return signaling::convertContentInfoToSignalingContent(content);
}

webrtc::ContentInfo convertSingalingContentToContentInfo(std::string const &contentId, signaling::MediaContent const &content, webrtc::RtpTransceiverDirection direction) {
    return signaling::convertSignalingContentToContentInfo(contentId, content, direction);
}

webrtc::ContentInfo createInactiveContentInfo(std::string const &contentId) {
    std::unique_ptr<webrtc::MediaContentDescription> contentDescription;

    auto audioDescription = std::make_unique<webrtc::AudioContentDescription>();
    contentDescription = std::move(audioDescription);

    contentDescription->set_direction(webrtc::RtpTransceiverDirection::kInactive);
    contentDescription->set_rtcp_mux(true);

    webrtc::ContentInfo mappedContentInfo(webrtc::MediaProtocolType::kRtp, contentId, std::move(contentDescription));
    mappedContentInfo.rejected = false;
    mappedContentInfo.bundle_only = false;

    return mappedContentInfo;
}

std::string contentIdBySsrc(uint32_t ssrc) {
    return std::to_string(ssrc);
}

}

namespace {

// One payload type numbering for every channel (they share one transport): a payload type a channel has is kept,
// a codec seen before gets its number again, a new one the picker's. Header extensions keep the IDs asked for when
// free.
class ContentNegotiationPayloadTypeSuggester : public webrtc::PayloadTypeSuggester {
public:
    webrtc::RTCErrorOr<webrtc::PayloadType> SuggestPayloadType(absl::string_view mid, const webrtc::Codec &codec, bool pick_from_top_of_range) override {
        webrtc::PayloadTypeRecorder &recorder = lookupRecorder(mid);
        webrtc::RTCErrorOr<webrtc::PayloadType> current = recorder.LookupPayloadType(codec);
        if (current.ok()) {
            return current;
        }
        webrtc::RTCErrorOr<webrtc::PayloadType> suggested = _picker.SuggestMapping(codec, pick_from_top_of_range);
        if (suggested.ok()) {
            _picker.AddMapping(suggested.value(), codec);
            recorder.AddMapping(suggested.value(), codec);
        }
        return suggested;
    }

    webrtc::RTCError AddLocalMapping(absl::string_view mid, webrtc::PayloadType payloadType, const webrtc::Codec &codec) override {
        lookupRecorder(mid).AddMapping(payloadType, codec);
        return _picker.AddMapping(payloadType, codec);
    }

    webrtc::RTCErrorOr<webrtc::RtpHeaderExtensionId> SuggestRtpHeaderExtensionId(absl::string_view mid, const webrtc::RtpExtension &extension, webrtc::RtpTransceiverIdDomain idDomain) override {
        return _extensionPicker.SuggestMapping(extension.uri, extension.encrypt, extension.id, idDomain, nullptr);
    }

    webrtc::RTCError AddRtpHeaderExtensionMapping(absl::string_view mid, const webrtc::RtpExtension &extension, bool local) override {
        return _extensionPicker.AddMapping(extension.id, extension.uri, extension.encrypt);
    }

private:
    webrtc::PayloadTypeRecorder &lookupRecorder(absl::string_view mid) {
        auto it = _recorders.find(std::string(mid));
        if (it == _recorders.end()) {
            it = _recorders.emplace(std::string(mid), std::make_unique<webrtc::PayloadTypeRecorder>(_picker)).first;
        }
        return *it->second;
    }

    webrtc::PayloadTypePicker _picker;
    webrtc::RtpHeaderExtensionPicker _extensionPicker;
    std::map<std::string, std::unique_ptr<webrtc::PayloadTypeRecorder>> _recorders;
};

// What PeerConnection hands its MediaSessionDescriptionFactory, for these negotiations: the media engine's codecs
// (CodecVendor's legacy path keeps the engine's payload types, as M123's set_audio_codecs / set_video_codecs did) and
// the suggester above.
class ContentNegotiationCodecLookupHelper : public webrtc::CodecLookupHelper {
public:
    ContentNegotiationCodecLookupHelper(webrtc::MediaEngineInterface *mediaEngine, const webrtc::FieldTrialsView &fieldTrials) :
    _codecVendor(mediaEngine, /*rtx_enabled=*/true, fieldTrials) {
    }

    webrtc::PayloadTypeSuggester *PayloadTypeSuggester() override {
        return &_payloadTypeSuggester;
    }

    webrtc::CodecVendor *GetCodecVendor() override {
        return &_codecVendor;
    }

private:
    ContentNegotiationPayloadTypeSuggester _payloadTypeSuggester;
    webrtc::CodecVendor _codecVendor;
};

}

ContentNegotiationContext::ContentNegotiationContext(const webrtc::Environment &env, bool isOutgoing, webrtc::MediaEngineInterface *mediaEngine, webrtc::UniqueRandomIdGenerator *uniqueRandomIdGenerator) :
_isOutgoing(isOutgoing),
_uniqueRandomIdGenerator(uniqueRandomIdGenerator) {
    _transportDescriptionFactory = std::make_unique<webrtc::TransportDescriptionFactory>(env.field_trials());

    // tempCertificate is only used to fill in the local SDP
    auto tempCertificate = webrtc::RTCCertificateGenerator::GenerateCertificate(webrtc::KeyParams(webrtc::KT_ECDSA), std::nullopt);
    _transportDescriptionFactory->set_certificate(tempCertificate);
    
    _codecLookupHelper = std::make_unique<ContentNegotiationCodecLookupHelper>(mediaEngine, env.field_trials());
    // No SCTP factory: the data channel is never negotiated.
    _sessionDescriptionFactory = std::make_unique<webrtc::MediaSessionDescriptionFactory>(env, uniqueRandomIdGenerator, _transportDescriptionFactory.get(), nullptr, _codecLookupHelper.get());

    _needNegotiation = true;
}

ContentNegotiationContext::~ContentNegotiationContext() {

}

void ContentNegotiationContext::copyCodecsFromChannelManager(webrtc::MediaEngineInterface *mediaEngine) {
    // Audio is Opus alone; video is every codec the engine has (CodecVendor reads them from the engine).
    _audioCodecPreferences.clear();
    for (const auto &codec : mediaEngine->voice().LegacySendCodecs()) {
        if (codec.name == "opus") {
            _audioCodecPreferences.push_back(webrtc::ToRtpCodecCapability(codec));
            break;
        }
    }

    int absSendTimeUriId = 2;
    int transportSequenceNumberUriId = 3;
    int videoRotationUri = 13;

    _rtpAudioExtensions.emplace_back(webrtc::RtpExtension::kAbsSendTimeUri, webrtc::RtpHeaderExtensionId(absSendTimeUriId));
    _rtpAudioExtensions.emplace_back(webrtc::RtpExtension::kTransportSequenceNumberUri, webrtc::RtpHeaderExtensionId(transportSequenceNumberUriId));

    _rtpVideoExtensions.emplace_back(webrtc::RtpExtension::kAbsSendTimeUri, webrtc::RtpHeaderExtensionId(absSendTimeUriId));
    _rtpVideoExtensions.emplace_back(webrtc::RtpExtension::kTransportSequenceNumberUri, webrtc::RtpHeaderExtensionId(transportSequenceNumberUriId));
    _rtpVideoExtensions.emplace_back(webrtc::RtpExtension::kVideoRotationUri, webrtc::RtpHeaderExtensionId(videoRotationUri));
}

std::string ContentNegotiationContext::addOutgoingChannel(signaling::MediaContent::Type mediaType) {
    std::string channelId = takeNextOutgoingChannelId();

    webrtc::MediaType mappedMediaType;
    std::vector<webrtc::RtpHeaderExtensionCapability> rtpExtensions;
    switch (mediaType) {
        case signaling::MediaContent::Type::Audio: {
            mappedMediaType = webrtc::MediaType::AUDIO;
            rtpExtensions = _rtpAudioExtensions;
            break;
        }
        case signaling::MediaContent::Type::Video: {
            mappedMediaType = webrtc::MediaType::VIDEO;
            rtpExtensions = _rtpVideoExtensions;
            break;
        }
        default: {
            RTC_FATAL() << "Unknown media type";
            break;
        }
    }
    webrtc::MediaDescriptionOptions offerDescription(mappedMediaType, channelId, webrtc::RtpTransceiverDirection::kSendOnly, false);
    offerDescription.header_extensions = rtpExtensions;

    switch (mediaType) {
        case signaling::MediaContent::Type::Audio: {
            offerDescription.AddAudioSender(channelId, { channelId });
            break;
        }
        case signaling::MediaContent::Type::Video: {
            webrtc::SimulcastLayerList simulcastLayers;
            offerDescription.AddVideoSender(channelId, { channelId }, {}, simulcastLayers, 1);
            break;
        }
        default: {
            RTC_FATAL() << "Unknown media type";
            break;
        }
    }

    _outgoingChannelDescriptions.emplace_back(std::move(offerDescription));
    _needNegotiation = true;

    return channelId;
}

void ContentNegotiationContext::removeOutgoingChannel(std::string const &id) {
    for (size_t i = 0; i < _outgoingChannelDescriptions.size(); i++) {
        if (_outgoingChannelDescriptions[i].description.mid == id) {
            _outgoingChannelDescriptions.erase(_outgoingChannelDescriptions.begin() + i);

            _needNegotiation = true;

            break;
        }
    }
}

std::unique_ptr<webrtc::SessionDescription> ContentNegotiationContext::currentSessionDescriptionFromCoordinatedState() {
    if (_channelIdOrder.empty()) {
        return nullptr;
    }

    auto sessionDescription = std::make_unique<webrtc::SessionDescription>();

    for (const auto &id : _channelIdOrder) {
        bool found = false;

        for (const auto &channel : _incomingChannels) {
            if (contentIdBySsrc(channel.ssrc) == id) {
                found = true;

                auto mappedContent = convertSingalingContentToContentInfo(contentIdBySsrc(channel.ssrc), channel, webrtc::RtpTransceiverDirection::kRecvOnly);

                // only required for content negotiation
                auto localCertificate = webrtc::RTCCertificateGenerator::GenerateCertificate(webrtc::KeyParams(webrtc::KT_ECDSA), std::nullopt);
                std::unique_ptr<webrtc::SSLFingerprint> fingerprint;
                if (localCertificate) {
                    fingerprint = webrtc::SSLFingerprint::CreateFromCertificate(*localCertificate.get());
                }
                
                std::vector<std::string> transportOptions;
                webrtc::TransportDescription transportDescription(
                    transportOptions,
                    "ufrag",
                    "pwd",
                    webrtc::IceMode::ICEMODE_FULL,
                    webrtc::ConnectionRole::CONNECTIONROLE_ACTPASS,
                    fingerprint.get()
                );
                webrtc::TransportInfo transportInfo(contentIdBySsrc(channel.ssrc), transportDescription);
                sessionDescription->AddTransportInfo(transportInfo);

                sessionDescription->AddContent(std::move(mappedContent));

                break;
            }
        }

        for (const auto &channel : _outgoingChannels) {
            if (channel.id == id) {
                found = true;

                auto mappedContent = convertSingalingContentToContentInfo(channel.id, channel.content, webrtc::RtpTransceiverDirection::kSendOnly);

                // only required for content negotiation
                auto localCertificate = webrtc::RTCCertificateGenerator::GenerateCertificate(webrtc::KeyParams(webrtc::KT_ECDSA), std::nullopt);
                std::unique_ptr<webrtc::SSLFingerprint> fingerprint;
                if (localCertificate) {
                    fingerprint = webrtc::SSLFingerprint::CreateFromCertificate(*localCertificate.get());
                }
                
                std::vector<std::string> transportOptions;
                webrtc::TransportDescription transportDescription(
                    transportOptions,
                    "ufrag",
                    "pwd",
                    webrtc::IceMode::ICEMODE_FULL,
                    webrtc::ConnectionRole::CONNECTIONROLE_ACTPASS,
                    fingerprint.get()
                );
                webrtc::TransportInfo transportInfo(mappedContent.mid(), transportDescription);
                sessionDescription->AddTransportInfo(transportInfo);

                sessionDescription->AddContent(std::move(mappedContent));

                break;
            }
        }

        if (!found) {
            auto mappedContent = createInactiveContentInfo("_" + id);

            // only required for content negotiation
            auto localCertificate = webrtc::RTCCertificateGenerator::GenerateCertificate(webrtc::KeyParams(webrtc::KT_ECDSA), std::nullopt);
            std::unique_ptr<webrtc::SSLFingerprint> fingerprint;
            if (localCertificate) {
                fingerprint = webrtc::SSLFingerprint::CreateFromCertificate(*localCertificate.get());
            }
            
            std::vector<std::string> transportOptions;
            webrtc::TransportDescription transportDescription(
                transportOptions,
                "ufrag",
                "pwd",
                webrtc::IceMode::ICEMODE_FULL,
                webrtc::ConnectionRole::CONNECTIONROLE_ACTPASS,
                fingerprint.get()
            );
            webrtc::TransportInfo transportInfo(mappedContent.mid(), transportDescription);
            sessionDescription->AddTransportInfo(transportInfo);

            sessionDescription->AddContent(std::move(mappedContent));
        }
    }

    return sessionDescription;
}

static webrtc::MediaDescriptionOptions getIncomingContentDescription(signaling::MediaContent const &content) {
    auto mappedContent = convertSingalingContentToContentInfo(contentIdBySsrc(content.ssrc), content, webrtc::RtpTransceiverDirection::kSendOnly);

    webrtc::MediaDescriptionOptions contentDescription(mappedContent.media_description()->type(), mappedContent.mid(), webrtc::RtpTransceiverDirection::kRecvOnly, false);
    for (const auto &extension : mappedContent.media_description()->rtp_header_extensions()) {
        contentDescription.header_extensions.emplace_back(extension.uri, extension.id);
    }

    return contentDescription;
}

std::unique_ptr<ContentNegotiationContext::NegotiationContents> ContentNegotiationContext::getPendingOffer() {
    if (!_needNegotiation) {
        return nullptr;
    }
    if (_pendingOutgoingOffer) {
        return nullptr;
    }

    _needNegotiation = false;

    _pendingOutgoingOffer = std::make_unique<PendingOutgoingOffer>();
    _pendingOutgoingOffer->exchangeId = _uniqueRandomIdGenerator->GenerateId();

    auto currentSessionDescription = currentSessionDescriptionFromCoordinatedState();

    webrtc::MediaSessionOptions offerOptions;
    offerOptions.offer_extmap_allow_mixed = true;
    offerOptions.bundle_enabled = true;

    for (const auto &id : _channelIdOrder) {
        bool found = false;

        for (const auto &channel : _outgoingChannelDescriptions) {
            if (channel.description.mid == id) {
                found = true;
                offerOptions.media_description_options.push_back(channel.description);

                break;
            }
        }

        for (const auto &content : _incomingChannels) {
            if (contentIdBySsrc(content.ssrc) == id) {
                found = true;
                offerOptions.media_description_options.push_back(getIncomingContentDescription(content));

                break;
            }
        }

        if (!found) {
            webrtc::MediaDescriptionOptions contentDescription(webrtc::MediaType::AUDIO, "_" + id, webrtc::RtpTransceiverDirection::kInactive, false);
            offerOptions.media_description_options.push_back(contentDescription);
        }
    }

    for (const auto &channel : _outgoingChannelDescriptions) {
        if (std::find(_channelIdOrder.begin(), _channelIdOrder.end(), channel.description.mid) == _channelIdOrder.end()) {
            _channelIdOrder.push_back(channel.description.mid);

            offerOptions.media_description_options.push_back(channel.description);
        }

        for (const auto &content : _incomingChannels) {
            if (std::find(_channelIdOrder.begin(), _channelIdOrder.end(), contentIdBySsrc(content.ssrc)) == _channelIdOrder.end()) {
                _channelIdOrder.push_back(contentIdBySsrc(content.ssrc));

                offerOptions.media_description_options.push_back(getIncomingContentDescription(content));
            }
        }
    }

    applyCodecPreferences(offerOptions);
    auto offerOrError = _sessionDescriptionFactory->CreateOfferOrError(offerOptions, currentSessionDescription.get());
    if (!offerOrError.ok()) {
        return nullptr;
    }
    auto offer = offerOrError.MoveValue();

    auto mappedOffer = std::make_unique<ContentNegotiationContext::NegotiationContents>();

    mappedOffer->exchangeId = _pendingOutgoingOffer->exchangeId;

    for (const auto &content : offer->contents()) {
        auto mappedContent = convertContentInfoToSingalingContent(content);

        if (content.media_description()->direction() == webrtc::RtpTransceiverDirection::kSendOnly) {
            mappedOffer->contents.push_back(std::move(mappedContent));

            for (auto &channel : _outgoingChannelDescriptions) {
                if (channel.description.mid == content.mid()) {
                    channel.ssrc = mappedContent.ssrc;
                    channel.ssrcGroups = mappedContent.ssrcGroups;
                }
            }
        }
    }

    return mappedOffer;
}

std::unique_ptr<ContentNegotiationContext::NegotiationContents> ContentNegotiationContext::setRemoteNegotiationContent(std::unique_ptr<NegotiationContents> &&remoteNegotiationContent) {
    if (!remoteNegotiationContent) {
        return nullptr;
    }

    if (_pendingOutgoingOffer) {
        if (remoteNegotiationContent->exchangeId == _pendingOutgoingOffer->exchangeId) {
            setAnswer(std::move(remoteNegotiationContent));
            return nullptr;
        } else {
            // race condition detected — call initiator wins
            if (!_isOutgoing) {
                _pendingOutgoingOffer.reset();
                return getAnswer(std::move(remoteNegotiationContent));
            } else {
                return nullptr;
            }
        }
    } else {
        return getAnswer(std::move(remoteNegotiationContent));
    }
}

std::unique_ptr<ContentNegotiationContext::NegotiationContents> ContentNegotiationContext::getAnswer(std::unique_ptr<ContentNegotiationContext::NegotiationContents> &&offer) {
    auto currentSessionDescription = currentSessionDescriptionFromCoordinatedState();

    auto mappedOffer = std::make_unique<webrtc::SessionDescription>();

    webrtc::MediaSessionOptions answerOptions;
    answerOptions.offer_extmap_allow_mixed = true;
    answerOptions.bundle_enabled = true;

    for (const auto &id : _channelIdOrder) {
        bool found = false;

        for (const auto &channel : _outgoingChannels) {
            if (channel.id == id) {
                found = true;

                auto mappedContent = convertSingalingContentToContentInfo(channel.id, channel.content, webrtc::RtpTransceiverDirection::kRecvOnly);

                webrtc::MediaDescriptionOptions contentDescription(mappedContent.media_description()->type(), mappedContent.mid(), webrtc::RtpTransceiverDirection::kSendOnly, false);
                for (const auto &extension : mappedContent.media_description()->rtp_header_extensions()) {
                    contentDescription.header_extensions.emplace_back(extension.uri, extension.id);
                }
                answerOptions.media_description_options.push_back(contentDescription);

                // only required for content negotiation
                auto localCertificate = webrtc::RTCCertificateGenerator::GenerateCertificate(webrtc::KeyParams(webrtc::KT_ECDSA), std::nullopt);
                std::unique_ptr<webrtc::SSLFingerprint> fingerprint;
                if (localCertificate) {
                    fingerprint = webrtc::SSLFingerprint::CreateFromCertificate(*localCertificate.get());
                }
                
                std::vector<std::string> transportOptions;
                webrtc::TransportDescription transportDescription(
                    transportOptions,
                    "ufrag",
                    "pwd",
                    webrtc::IceMode::ICEMODE_FULL,
                    webrtc::ConnectionRole::CONNECTIONROLE_ACTPASS,
                    fingerprint.get()
                );
                webrtc::TransportInfo transportInfo(channel.id, transportDescription);
                mappedOffer->AddTransportInfo(transportInfo);

                mappedOffer->AddContent(std::move(mappedContent));

                break;
            }
        }

        for (const auto &content : offer->contents) {
            if (contentIdBySsrc(content.ssrc) == id) {
                found = true;

                auto mappedContent = convertSingalingContentToContentInfo(contentIdBySsrc(content.ssrc), content, webrtc::RtpTransceiverDirection::kSendOnly);

                webrtc::MediaDescriptionOptions contentDescription(mappedContent.media_description()->type(), mappedContent.mid(), webrtc::RtpTransceiverDirection::kRecvOnly, false);
                for (const auto &extension : mappedContent.media_description()->rtp_header_extensions()) {
                    contentDescription.header_extensions.emplace_back(extension.uri, extension.id);
                }
                answerOptions.media_description_options.push_back(contentDescription);

                // only required for content negotiation
                auto localCertificate = webrtc::RTCCertificateGenerator::GenerateCertificate(webrtc::KeyParams(webrtc::KT_ECDSA), std::nullopt);
                std::unique_ptr<webrtc::SSLFingerprint> fingerprint;
                if (localCertificate) {
                    fingerprint = webrtc::SSLFingerprint::CreateFromCertificate(*localCertificate.get());
                }
                
                std::vector<std::string> transportOptions;
                webrtc::TransportDescription transportDescription(
                    transportOptions,
                    "ufrag",
                    "pwd",
                    webrtc::IceMode::ICEMODE_FULL,
                    webrtc::ConnectionRole::CONNECTIONROLE_ACTPASS,
                    fingerprint.get()
                );
                webrtc::TransportInfo transportInfo(mappedContent.mid(), transportDescription);
                mappedOffer->AddTransportInfo(transportInfo);

                mappedOffer->AddContent(std::move(mappedContent));

                break;
            }
        }

        if (!found) {
            auto mappedContent = createInactiveContentInfo("_" + id);

            webrtc::MediaDescriptionOptions contentDescription(webrtc::MediaType::AUDIO, "_" + id, webrtc::RtpTransceiverDirection::kInactive, false);
            answerOptions.media_description_options.push_back(contentDescription);

            // only required for content negotiation
            auto localCertificate = webrtc::RTCCertificateGenerator::GenerateCertificate(webrtc::KeyParams(webrtc::KT_ECDSA), std::nullopt);
            std::unique_ptr<webrtc::SSLFingerprint> fingerprint;
            if (localCertificate) {
                fingerprint = webrtc::SSLFingerprint::CreateFromCertificate(*localCertificate.get());
            }
            
            std::vector<std::string> transportOptions;
            webrtc::TransportDescription transportDescription(
                transportOptions,
                "ufrag",
                "pwd",
                webrtc::IceMode::ICEMODE_FULL,
                webrtc::ConnectionRole::CONNECTIONROLE_ACTPASS,
                fingerprint.get()
            );
            webrtc::TransportInfo transportInfo(mappedContent.mid(), transportDescription);
            mappedOffer->AddTransportInfo(transportInfo);

            mappedOffer->AddContent(std::move(mappedContent));
        }
    }

    for (const auto &content : offer->contents) {
        if (std::find(_channelIdOrder.begin(), _channelIdOrder.end(), contentIdBySsrc(content.ssrc)) == _channelIdOrder.end()) {
            _channelIdOrder.push_back(contentIdBySsrc(content.ssrc));

            answerOptions.media_description_options.push_back(getIncomingContentDescription(content));

            auto mappedContent = convertSingalingContentToContentInfo(contentIdBySsrc(content.ssrc), content, webrtc::RtpTransceiverDirection::kSendOnly);

            // only required for content negotiation
            auto localCertificate = webrtc::RTCCertificateGenerator::GenerateCertificate(webrtc::KeyParams(webrtc::KT_ECDSA), std::nullopt);
            std::unique_ptr<webrtc::SSLFingerprint> fingerprint;
            if (localCertificate) {
                fingerprint = webrtc::SSLFingerprint::CreateFromCertificate(*localCertificate.get());
            }
            
            std::vector<std::string> transportOptions;
            webrtc::TransportDescription transportDescription(
                transportOptions,
                "ufrag",
                "pwd",
                webrtc::IceMode::ICEMODE_FULL,
                webrtc::ConnectionRole::CONNECTIONROLE_ACTPASS,
                fingerprint.get()
            );
            webrtc::TransportInfo transportInfo(mappedContent.mid(), transportDescription);
            mappedOffer->AddTransportInfo(transportInfo);

            mappedOffer->AddContent(std::move(mappedContent));
        }
    }

    applyCodecPreferences(answerOptions);
    auto answerOrError = _sessionDescriptionFactory->CreateAnswerOrError(mappedOffer.get(), answerOptions, currentSessionDescription.get());
    if (!answerOrError.ok()) {
        return nullptr;
    }
    auto answer = answerOrError.MoveValue();

    auto mappedAnswer = std::make_unique<NegotiationContents>();

    mappedAnswer->exchangeId = offer->exchangeId;

    std::vector<signaling::MediaContent> incomingChannels;

    for (const auto &content : answer->contents()) {
        auto mappedContent = convertContentInfoToSingalingContent(content);

        if (content.media_description()->direction() == webrtc::RtpTransceiverDirection::kRecvOnly) {
            for (const auto &offerContent : offer->contents) {
                if (contentIdBySsrc(offerContent.ssrc) == content.mid()) {
                    mappedContent.ssrc = offerContent.ssrc;
                    mappedContent.ssrcGroups = offerContent.ssrcGroups;

                    break;
                }
            }

            incomingChannels.push_back(mappedContent);
            mappedAnswer->contents.push_back(std::move(mappedContent));
        }
    }

    _incomingChannels = incomingChannels;

    return mappedAnswer;
}

void ContentNegotiationContext::setAnswer(std::unique_ptr<ContentNegotiationContext::NegotiationContents> &&answer) {
    if (!_pendingOutgoingOffer) {
        return;
    }
    if (_pendingOutgoingOffer->exchangeId != answer->exchangeId) {
        return;
    }

    _pendingOutgoingOffer.reset();

    _outgoingChannels.clear();

    for (const auto &content : answer->contents) {
        for (const auto &pendingChannel : _outgoingChannelDescriptions) {
            if (pendingChannel.ssrc != 0 && content.ssrc == pendingChannel.ssrc) {
                _outgoingChannels.emplace_back(pendingChannel.description.mid, content);

                break;
            }
        }
    }
}

std::string ContentNegotiationContext::takeNextOutgoingChannelId() {
    const auto result = "m" + std::to_string(_nextOutgoingChannelId);
    _nextOutgoingChannelId++;

    return result;
}

std::unique_ptr<ContentNegotiationContext::CoordinatedState> ContentNegotiationContext::coordinatedState() const {
    auto result = std::make_unique<ContentNegotiationContext::CoordinatedState>();

    result->incomingContents = _incomingChannels;
    for (const auto &channel : _outgoingChannels) {
        bool found = false;

        for (const auto &channelDescription : _outgoingChannelDescriptions) {
            if (channelDescription.description.mid == channel.id) {
                found = true;
                break;
            }
        }

        if (found) {
            result->outgoingContents.push_back(channel.content);
        }
    }

    return result;
}

std::optional<uint32_t> ContentNegotiationContext::outgoingChannelSsrc(std::string const &id) const {
    for (const auto &channel : _outgoingChannels) {
        bool found = false;

        for (const auto &channelDescription : _outgoingChannelDescriptions) {
            if (channelDescription.description.mid == channel.id) {
                found = true;
                break;
            }
        }

        if (found && channel.id == id) {
            if (channel.content.ssrc != 0) {
                return channel.content.ssrc;
            }
        }
    }

    return std::nullopt;
}

void ContentNegotiationContext::applyCodecPreferences(webrtc::MediaSessionOptions &options) const {
    for (auto &description : options.media_description_options) {
        if (description.type == webrtc::MediaType::AUDIO) {
            description.codec_preferences = _audioCodecPreferences;
        }
    }
}

} // namespace tgcalls
