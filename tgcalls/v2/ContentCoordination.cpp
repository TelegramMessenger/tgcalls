#include "v2/ContentCoordination.h"

#include "rtc_base/rtc_certificate_generator.h"

#include <sstream>

namespace tgcalls {

namespace {

signaling::MediaContent convertContentInfoToSingalingContent(cricket::ContentInfo const &content) {
    signaling::MediaContent mappedContent;
    
    switch (content.media_description()->type()) {
        case cricket::MediaType::MEDIA_TYPE_AUDIO: {
            mappedContent.type = signaling::MediaContent::Type::Audio;
            
            for (const auto &codec : content.media_description()->as_audio()->codecs()) {
                signaling::PayloadType mappedPayloadType;
                mappedPayloadType.id = codec.id;
                mappedPayloadType.name = codec.name;
                mappedPayloadType.clockrate = codec.clockrate;
                mappedPayloadType.channels = (uint32_t)codec.channels;
                
                for (const auto &feedbackType : codec.feedback_params.params()) {
                    signaling::FeedbackType mappedFeedbackType;
                    mappedFeedbackType.type = feedbackType.id();
                    mappedFeedbackType.subtype = feedbackType.param();
                    mappedPayloadType.feedbackTypes.push_back(std::move(mappedFeedbackType));
                }
                
                for (const auto &parameter : codec.params) {
                    mappedPayloadType.parameters.push_back(std::make_pair(parameter.first, parameter.second));
                }
                std::sort(mappedPayloadType.parameters.begin(), mappedPayloadType.parameters.end(), [](std::pair<std::string, std::string> const &lhs, std::pair<std::string, std::string> const &rhs) -> bool {
                    return lhs.first < rhs.first;
                });
                
                mappedContent.payloadTypes.push_back(std::move(mappedPayloadType));
            }
            break;
        }
        case cricket::MediaType::MEDIA_TYPE_VIDEO: {
            mappedContent.type = signaling::MediaContent::Type::Video;
            
            for (const auto &codec : content.media_description()->as_video()->codecs()) {
                signaling::PayloadType mappedPayloadType;
                mappedPayloadType.id = codec.id;
                mappedPayloadType.name = codec.name;
                mappedPayloadType.clockrate = codec.clockrate;
                mappedPayloadType.channels = 0;
                
                for (const auto &feedbackType : codec.feedback_params.params()) {
                    signaling::FeedbackType mappedFeedbackType;
                    mappedFeedbackType.type = feedbackType.id();
                    mappedFeedbackType.subtype = feedbackType.param();
                    mappedPayloadType.feedbackTypes.push_back(std::move(mappedFeedbackType));
                }
                
                for (const auto &parameter : codec.params) {
                    mappedPayloadType.parameters.push_back(std::make_pair(parameter.first, parameter.second));
                }
                std::sort(mappedPayloadType.parameters.begin(), mappedPayloadType.parameters.end(), [](std::pair<std::string, std::string> const &lhs, std::pair<std::string, std::string> const &rhs) -> bool {
                    return lhs.first < rhs.first;
                });
                
                mappedContent.payloadTypes.push_back(std::move(mappedPayloadType));
            }
            break;
        }
        default: {
            RTC_FATAL() << "Unknown media type";
            break;
        }
    }
    
    if (!content.media_description()->streams().empty()) {
        mappedContent.ssrc = content.media_description()->streams()[0].first_ssrc();
        for (const auto &ssrcGroup : content.media_description()->streams()[0].ssrc_groups) {
            signaling::SsrcGroup mappedSsrcGroup;
            mappedSsrcGroup.semantics = ssrcGroup.semantics;
            mappedSsrcGroup.ssrcs = ssrcGroup.ssrcs;
            mappedContent.ssrcGroups.push_back(std::move(mappedSsrcGroup));
        }
    }
    
    for (const auto &extension : content.media_description()->rtp_header_extensions()) {
        mappedContent.rtpExtensions.push_back(extension);
    }
    
    return mappedContent;
}

cricket::ContentInfo convertSingalingContentToContentInfo(std::string const &contentId, signaling::MediaContent const &content, webrtc::RtpTransceiverDirection direction) {
    std::unique_ptr<cricket::MediaContentDescription> contentDescription;
    
    switch (content.type) {
        case signaling::MediaContent::Type::Audio: {
            auto audioDescription = std::make_unique<cricket::AudioContentDescription>();
            
            for (const auto &payloadType : content.payloadTypes) {
                cricket::AudioCodec mappedCodec((int)payloadType.id, payloadType.name, (int)payloadType.clockrate, 0, payloadType.channels);
                for (const auto &parameter : payloadType.parameters) {
                    mappedCodec.params.insert(parameter);
                }
                audioDescription->AddCodec(mappedCodec);
            }
            
            contentDescription = std::move(audioDescription);
            
            break;
        }
        case signaling::MediaContent::Type::Video: {
            auto videoDescription = std::make_unique<cricket::VideoContentDescription>();
            
            for (const auto &payloadType : content.payloadTypes) {
                cricket::VideoCodec mappedCodec((int)payloadType.id, payloadType.name);
                for (const auto &parameter : payloadType.parameters) {
                    mappedCodec.params.insert(parameter);
                }
                videoDescription->AddCodec(mappedCodec);
            }
            
            contentDescription = std::move(videoDescription);
            
            break;
        }
        default: {
            RTC_FATAL() << "Unknown media type";
            break;
        }
    }
    
    cricket::StreamParams streamParams;
    streamParams.set_stream_ids({ contentId });
    streamParams.add_ssrc(content.ssrc);
    for (const auto &ssrcGroup : content.ssrcGroups) {
        streamParams.ssrc_groups.push_back(cricket::SsrcGroup(ssrcGroup.semantics, ssrcGroup.ssrcs));
        for (const auto &ssrc : ssrcGroup.ssrcs) {
            if (!streamParams.has_ssrc(ssrc)) {
                streamParams.add_ssrc(ssrc);
            }
        }
    }
    contentDescription->AddStream(streamParams);
    
    for (const auto &extension : content.rtpExtensions) {
        contentDescription->AddRtpHeaderExtension(extension);
    }
    
    contentDescription->set_direction(direction);
    contentDescription->set_rtcp_mux(true);
    
    cricket::ContentInfo mappedContentInfo(cricket::MediaProtocolType::kRtp);
    mappedContentInfo.name = contentId;
    mappedContentInfo.rejected = false;
    mappedContentInfo.bundle_only = false;
    mappedContentInfo.set_media_description(std::move(contentDescription));
    
    return mappedContentInfo;
}

std::unique_ptr<cricket::SessionDescription> sessionDescriptionFromCoordinatedState(bool isOutgoing, ContentCoordinationContext::CoordinatedState const *state) {
    auto sessionDescription = std::make_unique<cricket::SessionDescription>();
    
    int nextLocalContentId = 0;
    for (const auto &content : state->outgoingContents) {
        std::ostringstream contentIdString;
        contentIdString << (isOutgoing ? "0" : "1") << nextLocalContentId;
        nextLocalContentId++;
        
        auto mappedContent = convertSingalingContentToContentInfo(contentIdString.str(), content, webrtc::RtpTransceiverDirection::kSendOnly);
        sessionDescription->AddContent(std::move(mappedContent));
    }
    
    int nextRemoteContentId = 0;
    for (const auto &content : state->incomingContents) {
        std::ostringstream contentIdString;
        contentIdString << (isOutgoing ? "1" : "0") << nextRemoteContentId;
        nextRemoteContentId++;
        
        auto mappedContent = convertSingalingContentToContentInfo(contentIdString.str(), content, webrtc::RtpTransceiverDirection::kRecvOnly);
        sessionDescription->AddContent(std::move(mappedContent));
    }
    
    return sessionDescription;
}

}

ContentCoordinationContext::ContentCoordinationContext(bool isOutgoing, cricket::ChannelManager *channelManager, rtc::UniqueRandomIdGenerator *uniqueRandomIdGenerator) :
_isOutgoing(isOutgoing) {
    _transportDescriptionFactory = std::make_unique<cricket::TransportDescriptionFactory>();
    
    // tempCertificate is only used to fill in the local SDP
    auto tempCertificate = rtc::RTCCertificateGenerator::GenerateCertificate(rtc::KeyParams(rtc::KT_ECDSA), absl::nullopt);
    _transportDescriptionFactory->set_secure(cricket::SecurePolicy::SEC_REQUIRED);
    _transportDescriptionFactory->set_certificate(tempCertificate);
    
    _sessionDescriptionFactory = std::make_unique<cricket::MediaSessionDescriptionFactory>(_transportDescriptionFactory.get(), uniqueRandomIdGenerator);
    
    cricket::AudioCodecs audioSendCodecs;
    cricket::AudioCodecs audioRecvCodecs;
    cricket::VideoCodecs videoSendCodecs;
    cricket::VideoCodecs videoRecvCodecs;
    
    channelManager->GetSupportedAudioSendCodecs(&audioSendCodecs);
    channelManager->GetSupportedAudioReceiveCodecs(&audioRecvCodecs);
    channelManager->GetSupportedVideoSendCodecs(&videoSendCodecs);
    channelManager->GetSupportedVideoReceiveCodecs(&videoRecvCodecs);
    
    for (const auto &codec : audioSendCodecs) {
        if (codec.name == "opus") {
            audioSendCodecs = { codec };
            audioRecvCodecs = { codec };
            break;
        }
    }
    
    _sessionDescriptionFactory->set_audio_codecs(audioSendCodecs, audioRecvCodecs);
    _sessionDescriptionFactory->set_video_codecs(videoSendCodecs, videoRecvCodecs);
    
    _rtpAudioExtensions.emplace_back(webrtc::RtpExtension::kAbsSendTimeUri, 2);
    _rtpAudioExtensions.emplace_back(webrtc::RtpExtension::kTransportSequenceNumberUri, 3);
    
    _rtpVideoExtensions.emplace_back(webrtc::RtpExtension::kAbsSendTimeUri, 2);
    _rtpVideoExtensions.emplace_back(webrtc::RtpExtension::kTransportSequenceNumberUri, 3);
    _rtpVideoExtensions.emplace_back(webrtc::RtpExtension::kVideoRotationUri, 13);
}

ContentCoordinationContext::~ContentCoordinationContext() {
    
}

void ContentCoordinationContext::addOutgoingChannel(ContentCoordinationContext::MediaType mediaType) {
    std::string channelId = takeNextOutgoingChannelId();
    
    cricket::MediaType mappedMediaType;
    std::vector<webrtc::RtpHeaderExtensionCapability> rtpExtensions;
    switch (mediaType) {
        case ContentCoordinationContext::MediaType::Audio: {
            mappedMediaType = cricket::MediaType::MEDIA_TYPE_AUDIO;
            rtpExtensions = _rtpAudioExtensions;
            break;
        }
        case ContentCoordinationContext::MediaType::Video: {
            mappedMediaType = cricket::MediaType::MEDIA_TYPE_VIDEO;
            rtpExtensions = _rtpVideoExtensions;
            break;
        }
        default: {
            RTC_FATAL() << "Unknown media type";
            break;
        }
    }
    cricket::MediaDescriptionOptions offerAudio(mappedMediaType, channelId, webrtc::RtpTransceiverDirection::kSendOnly, false);
    offerAudio.header_extensions = rtpExtensions;
    
    switch (mediaType) {
        case ContentCoordinationContext::MediaType::Audio: {
            offerAudio.AddAudioSender(channelId, { channelId });
            break;
        }
        case ContentCoordinationContext::MediaType::Video: {
            cricket::SimulcastLayerList simulcastLayers;
            offerAudio.AddVideoSender(channelId, { channelId }, {}, simulcastLayers, 1);
            break;
        }
        default: {
            RTC_FATAL() << "Unknown media type";
            break;
        }
    }
    
    _outgoingChannels.push_back(offerAudio);
}

/*void ContentCoordinationContext::removeOutgoingChannel(std::string const &channelId) {
    for (size_t i = 0; i < _outgoingChannels.size(); i++) {
        if (_outgoingChannels[i].mid == channelId) {
            _outgoingChannels.erase(_outgoingChannels.begin() + i);
            break;
        }
    }
}*/

std::unique_ptr<ContentCoordinationContext::Offer> ContentCoordinationContext::getOffer() {
    _pendingOutgoingOffer = std::make_unique<PendingOutgoingOffer>();
    _pendingOutgoingOffer->exchangeId = _nextExchangeId;
    _nextExchangeId += 1;
    
    cricket::MediaSessionOptions offerOptions;
    offerOptions.offer_extmap_allow_mixed = true;
    offerOptions.bundle_enabled = true;
    
    std::unique_ptr<cricket::SessionDescription> _currentSessionDescription;
    if (_coordinatedState) {
        _currentSessionDescription = sessionDescriptionFromCoordinatedState(_isOutgoing, _coordinatedState.get());
        
        for (const auto &content : _currentSessionDescription->contents()) {
            webrtc::RtpTransceiverDirection direction;
            if (content.media_description()->direction() == webrtc::RtpTransceiverDirection::kRecvOnly) {
                direction = webrtc::RtpTransceiverDirection::kRecvOnly;
            } else {
                direction = webrtc::RtpTransceiverDirection::kSendOnly;
            }
            
            cricket::MediaDescriptionOptions contentDescription(content.media_description()->type(), content.name, direction, false);
            for (const auto &extension : content.media_description()->rtp_header_extensions()) {
                contentDescription.header_extensions.emplace_back(extension.uri, extension.id);
            }
            
            offerOptions.media_description_options.push_back(contentDescription);
        }
    }
    
    for (const auto &channel : _outgoingChannels) {
        offerOptions.media_description_options.push_back(channel);
    }
    
    std::unique_ptr<cricket::SessionDescription> offer = _sessionDescriptionFactory->CreateOffer(offerOptions, _currentSessionDescription.get());
    
    auto mappedOffer = std::make_unique<ContentCoordinationContext::Offer>();
    
    mappedOffer->exchangeId = _pendingOutgoingOffer->exchangeId;
    
    for (const auto &content : offer->contents()) {
        auto mappedContent = convertContentInfoToSingalingContent(content);
        
        if (_coordinatedState) {
            int nextRemoteContentId = 0;
            for (const auto &findContent : _coordinatedState->incomingContents) {
                std::ostringstream contentIdString;
                contentIdString << (_isOutgoing ? "1" : "0") << nextRemoteContentId;
                nextRemoteContentId++;
                std::string contentId = contentIdString.str();
                if (contentId == content.name) {
                    mappedContent.ssrc = findContent.ssrc;
                    mappedContent.ssrcGroups = findContent.ssrcGroups;
                    break;
                }
            }
        }
        
        if (content.media_description()->direction() == webrtc::RtpTransceiverDirection::kSendOnly) {
            mappedOffer->outgoingContents.push_back(std::move(mappedContent));
        } else {
            mappedOffer->incomingContents.push_back(std::move(mappedContent));
        }
    }
    
    return mappedOffer;
}

std::unique_ptr<ContentCoordinationContext::Answer> ContentCoordinationContext::getAnwer(std::unique_ptr<ContentCoordinationContext::Offer> offer) {
    _nextExchangeId = offer->exchangeId + 1;
    
    auto mappedOffer = std::make_unique<cricket::SessionDescription>();
    
    cricket::MediaSessionOptions answerOptions;
    answerOptions.offer_extmap_allow_mixed = true;
    answerOptions.bundle_enabled = true;
    
    int nextRemoteContentId = 0;
    for (const auto &content : offer->outgoingContents) {
        std::ostringstream contentIdString;
        contentIdString << (_isOutgoing ? "1" : "0") << nextRemoteContentId;
        nextRemoteContentId++;
        std::string contentId = contentIdString.str();
        
        auto mappedContent = convertSingalingContentToContentInfo(contentId, content, webrtc::RtpTransceiverDirection::kSendOnly);
        
        cricket::MediaDescriptionOptions contentDescription(mappedContent.media_description()->type(), mappedContent.name, webrtc::RtpTransceiverDirection::kRecvOnly, false);
        for (const auto &extension : mappedContent.media_description()->rtp_header_extensions()) {
            contentDescription.header_extensions.emplace_back(extension.uri, extension.id);
        }
        answerOptions.media_description_options.push_back(contentDescription);
        
        mappedOffer->AddContent(std::move(mappedContent));
        
        cricket::TransportDescription transportDescription;
        cricket::TransportInfo transportInfo(contentId, transportDescription);
        mappedOffer->AddTransportInfo(transportInfo);
    }
    
    int nextLocalContentId = 0;
    for (const auto &content : offer->incomingContents) {
        std::ostringstream contentIdString;
        contentIdString << (!_isOutgoing ? "1" : "0") << nextLocalContentId;
        nextLocalContentId++;
        std::string contentId = contentIdString.str();
        
        auto mappedContent = convertSingalingContentToContentInfo(contentId, content, webrtc::RtpTransceiverDirection::kRecvOnly);
        
        cricket::MediaDescriptionOptions contentDescription(mappedContent.media_description()->type(), mappedContent.name, webrtc::RtpTransceiverDirection::kSendOnly, false);
        for (const auto &extension : mappedContent.media_description()->rtp_header_extensions()) {
            contentDescription.header_extensions.emplace_back(extension.uri, extension.id);
        }
        answerOptions.media_description_options.push_back(contentDescription);
        
        mappedOffer->AddContent(std::move(mappedContent));
        
        cricket::TransportDescription transportDescription;
        cricket::TransportInfo transportInfo(contentId, transportDescription);
        mappedOffer->AddTransportInfo(transportInfo);
    }
    
    std::unique_ptr<cricket::SessionDescription> _currentSessionDescription;
    if (_coordinatedState) {
        _currentSessionDescription = sessionDescriptionFromCoordinatedState(_isOutgoing, _coordinatedState.get());
    }
    
    std::unique_ptr<cricket::SessionDescription> answer = _sessionDescriptionFactory->CreateAnswer(mappedOffer.get(), answerOptions, _currentSessionDescription.get());
    
    auto mappedAnswer = std::make_unique<Answer>();
    
    mappedAnswer->exchangeId = offer->exchangeId;
    
    auto resultingState = std::make_unique<CoordinatedState>();
    
    for (const auto &content : answer->contents()) {
        auto mappedContent = convertContentInfoToSingalingContent(content);
        if (content.media_description()->direction() == webrtc::RtpTransceiverDirection::kSendOnly) {
            if (_coordinatedState) {
                int nextRemoteContentId = 0;
                for (const auto &findContent : _coordinatedState->outgoingContents) {
                    std::ostringstream contentIdString;
                    contentIdString << (_isOutgoing ? "0" : "1") << nextRemoteContentId;
                    nextRemoteContentId++;
                    std::string contentId = contentIdString.str();
                    if (contentId == content.name) {
                        mappedContent.ssrc = findContent.ssrc;
                        mappedContent.ssrcGroups = findContent.ssrcGroups;
                        break;
                    }
                }
            }
            
            resultingState->outgoingContents.push_back(mappedContent);
            mappedAnswer->outgoingContents.push_back(std::move(mappedContent));
        } else {
            int nextRemoteContentId = 0;
            for (const auto &findContent : offer->outgoingContents) {
                std::ostringstream contentIdString;
                contentIdString << (_isOutgoing ? "1" : "0") << nextRemoteContentId;
                nextRemoteContentId++;
                std::string contentId = contentIdString.str();
                if (contentId == content.name) {
                    mappedContent.ssrc = findContent.ssrc;
                    mappedContent.ssrcGroups = findContent.ssrcGroups;
                    break;
                }
            }
            
            resultingState->incomingContents.push_back(mappedContent);
            mappedAnswer->incomingContents.push_back(std::move(mappedContent));
        }
    }
    
    _coordinatedState = std::move(resultingState);
    
    return mappedAnswer;
}

void ContentCoordinationContext::setAnswer(std::unique_ptr<ContentCoordinationContext::Answer> answer) {
    _pendingOutgoingOffer.reset();
    _nextExchangeId = answer->exchangeId + 1;
    
    auto resultingState = std::make_unique<CoordinatedState>();
    
    for (const auto &content : answer->incomingContents) {
        resultingState->outgoingContents.push_back(content);
    }
    for (const auto &content : answer->outgoingContents) {
        resultingState->incomingContents.push_back(content);
    }
    
    _coordinatedState = std::move(resultingState);
}

std::string ContentCoordinationContext::takeNextOutgoingChannelId() {
    std::ostringstream result;
    result << (_isOutgoing ? "0" : "1") << _nextOutgoingChannelId;
    _nextOutgoingChannelId++;
    
    return result.str();
}

} // namespace tgcalls
