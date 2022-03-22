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

std::string contentIdBySsrc(uint32_t ssrc) {
    std::ostringstream contentIdString;
    
    contentIdString << ssrc;
    
    return contentIdString.str();
}

}

ContentCoordinationContext::ContentCoordinationContext(bool isOutgoing, cricket::ChannelManager *channelManager, rtc::UniqueRandomIdGenerator *uniqueRandomIdGenerator) :
_isOutgoing(isOutgoing),
_uniqueRandomIdGenerator(uniqueRandomIdGenerator) {
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

void ContentCoordinationContext::addOutgoingChannel(signaling::MediaContent::Type mediaType) {
    std::string channelId = takeNextOutgoingChannelId();
    
    cricket::MediaType mappedMediaType;
    std::vector<webrtc::RtpHeaderExtensionCapability> rtpExtensions;
    switch (mediaType) {
        case signaling::MediaContent::Type::Audio: {
            mappedMediaType = cricket::MediaType::MEDIA_TYPE_AUDIO;
            rtpExtensions = _rtpAudioExtensions;
            break;
        }
        case signaling::MediaContent::Type::Video: {
            mappedMediaType = cricket::MediaType::MEDIA_TYPE_VIDEO;
            rtpExtensions = _rtpVideoExtensions;
            break;
        }
        default: {
            RTC_FATAL() << "Unknown media type";
            break;
        }
    }
    cricket::MediaDescriptionOptions offerDescription(mappedMediaType, channelId, webrtc::RtpTransceiverDirection::kSendOnly, false);
    offerDescription.header_extensions = rtpExtensions;
    
    switch (mediaType) {
        case signaling::MediaContent::Type::Audio: {
            offerDescription.AddAudioSender(channelId, { channelId });
            break;
        }
        case signaling::MediaContent::Type::Video: {
            cricket::SimulcastLayerList simulcastLayers;
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
}

/*void ContentCoordinationContext::removeOutgoingChannel(std::string const &channelId) {
    for (size_t i = 0; i < _outgoingChannels.size(); i++) {
        if (_outgoingChannels[i].mid == channelId) {
            _outgoingChannels.erase(_outgoingChannels.begin() + i);
            break;
        }
    }
}*/

static cricket::MediaDescriptionOptions getOutgoingContentDescription(std::string const &name, signaling::MediaContent const &content) {
    auto mappedContent = convertSingalingContentToContentInfo(name, content, webrtc::RtpTransceiverDirection::kRecvOnly);
    
    cricket::MediaDescriptionOptions contentDescription(mappedContent.media_description()->type(), mappedContent.name, webrtc::RtpTransceiverDirection::kSendOnly, false);
    for (const auto &extension : mappedContent.media_description()->rtp_header_extensions()) {
        contentDescription.header_extensions.emplace_back(extension.uri, extension.id);
    }
    
    return contentDescription;
}

std::unique_ptr<cricket::SessionDescription> ContentCoordinationContext::currentSessionDescriptionFromCoordinatedState() {
    if (_channelIdOrder.empty()) {
        return nullptr;
    }
    
    auto sessionDescription = std::make_unique<cricket::SessionDescription>();
    
    for (const auto &id : _channelIdOrder) {
        for (const auto &channel : _incomingChannels) {
            if (contentIdBySsrc(channel.ssrc) == id) {
                auto mappedContent = convertSingalingContentToContentInfo(contentIdBySsrc(channel.ssrc), channel, webrtc::RtpTransceiverDirection::kRecvOnly);
                
                cricket::TransportDescription transportDescription;
                cricket::TransportInfo transportInfo(contentIdBySsrc(channel.ssrc), transportDescription);
                sessionDescription->AddTransportInfo(transportInfo);
                
                sessionDescription->AddContent(std::move(mappedContent));
                
                break;
            }
        }
        
        for (const auto &channel : _outgoingChannels) {
            if (channel.id == id) {
                auto mappedContent = convertSingalingContentToContentInfo(channel.id, channel.content, webrtc::RtpTransceiverDirection::kSendOnly);
                
                cricket::TransportDescription transportDescription;
                cricket::TransportInfo transportInfo(channel.id, transportDescription);
                sessionDescription->AddTransportInfo(transportInfo);
                
                sessionDescription->AddContent(std::move(mappedContent));
                
                break;
            }
        }
    }
    
    return sessionDescription;
}

static cricket::MediaDescriptionOptions getIncomingContentDescription(signaling::MediaContent const &content) {
    auto mappedContent = convertSingalingContentToContentInfo(contentIdBySsrc(content.ssrc), content, webrtc::RtpTransceiverDirection::kSendOnly);
    
    cricket::MediaDescriptionOptions contentDescription(mappedContent.media_description()->type(), mappedContent.name, webrtc::RtpTransceiverDirection::kRecvOnly, false);
    for (const auto &extension : mappedContent.media_description()->rtp_header_extensions()) {
        contentDescription.header_extensions.emplace_back(extension.uri, extension.id);
    }
    
    return contentDescription;
}

std::unique_ptr<ContentCoordinationContext::NegotiationContents> ContentCoordinationContext::getOffer() {
    if (!_needNegotiation) {
        return nullptr;
    }
    
    _pendingOutgoingOffer = std::make_unique<PendingOutgoingOffer>();
    _pendingOutgoingOffer->exchangeId = _uniqueRandomIdGenerator->GenerateId();
    
    cricket::MediaSessionOptions offerOptions;
    offerOptions.offer_extmap_allow_mixed = true;
    offerOptions.bundle_enabled = true;
    
    for (const auto &id : _channelIdOrder) {
        for (const auto &channel : _outgoingChannelDescriptions) {
            if (channel.description.mid == id) {
                offerOptions.media_description_options.push_back(channel.description);
                
                break;
            }
        }
        
        for (const auto &content : _incomingChannels) {
            if (contentIdBySsrc(content.ssrc) == id) {
                offerOptions.media_description_options.push_back(getIncomingContentDescription(content));
                
                break;
            }
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
    
    auto currentSessionDescription = currentSessionDescriptionFromCoordinatedState();
    
    std::unique_ptr<cricket::SessionDescription> offer = _sessionDescriptionFactory->CreateOffer(offerOptions, currentSessionDescription.get());
    
    auto mappedOffer = std::make_unique<ContentCoordinationContext::NegotiationContents>();
    
    mappedOffer->exchangeId = _pendingOutgoingOffer->exchangeId;
    
    for (const auto &content : offer->contents()) {
        auto mappedContent = convertContentInfoToSingalingContent(content);
        
        /*if (_coordinatedState) {
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
        }*/
        
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

std::unique_ptr<ContentCoordinationContext::NegotiationContents> ContentCoordinationContext::setRemoteNegotiationContent(std::unique_ptr<NegotiationContents> &&remoteNegotiationContent) {
    if (!remoteNegotiationContent) {
        return nullptr;
    }
    
    if (_pendingOutgoingOffer) {
        if (remoteNegotiationContent->exchangeId == _pendingOutgoingOffer->exchangeId) {
            setAnswer(std::move(remoteNegotiationContent));
            return nullptr;
        } else {
            // race condition detected — highest exchangeId wins
            if (_pendingOutgoingOffer->exchangeId < remoteNegotiationContent->exchangeId) {
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

std::unique_ptr<ContentCoordinationContext::NegotiationContents> ContentCoordinationContext::getAnswer(std::unique_ptr<ContentCoordinationContext::NegotiationContents> &&offer) {
    auto mappedOffer = std::make_unique<cricket::SessionDescription>();
    
    cricket::MediaSessionOptions answerOptions;
    answerOptions.offer_extmap_allow_mixed = true;
    answerOptions.bundle_enabled = true;
    
    for (const auto &id : _channelIdOrder) {
        /*for (const auto &channel : _outgoingChannels) {
            if (channel.description.mid == id) {
                answerOptions.media_description_options.push_back(channel.description);
                
                cricket::MediaDescriptionOptions contentDescription(channel.description.type, channel.description.mid, webrtc::RtpTransceiverDirection::kSendOnly, false);
                contentDescription.header_extensions = channel.description.header_extensions;
                
                
                mappedOffer->AddContent(std::move(mappedContent));
                
                cricket::TransportDescription transportDescription;
                cricket::TransportInfo transportInfo(contentId, transportDescription);
                mappedOffer->AddTransportInfo(transportInfo);
                
                break;
            }
        }*/
        
        for (const auto &content : offer->contents) {
            if (contentIdBySsrc(content.ssrc) == id) {
                answerOptions.media_description_options.push_back(getIncomingContentDescription(content));
                
                auto mappedContent = convertSingalingContentToContentInfo(contentIdBySsrc(content.ssrc), content, webrtc::RtpTransceiverDirection::kSendOnly);
                
                cricket::MediaDescriptionOptions contentDescription(mappedContent.media_description()->type(), mappedContent.name, webrtc::RtpTransceiverDirection::kRecvOnly, false);
                for (const auto &extension : mappedContent.media_description()->rtp_header_extensions()) {
                    contentDescription.header_extensions.emplace_back(extension.uri, extension.id);
                }
                answerOptions.media_description_options.push_back(contentDescription);
                
                cricket::TransportDescription transportDescription;
                cricket::TransportInfo transportInfo(mappedContent.mid(), transportDescription);
                mappedOffer->AddTransportInfo(transportInfo);
                
                mappedOffer->AddContent(std::move(mappedContent));
                
                break;
            }
        }
    }
    
    for (const auto &content : offer->contents) {
        if (std::find(_channelIdOrder.begin(), _channelIdOrder.end(), contentIdBySsrc(content.ssrc)) == _channelIdOrder.end()) {
            _channelIdOrder.push_back(contentIdBySsrc(content.ssrc));
            
            answerOptions.media_description_options.push_back(getIncomingContentDescription(content));
            
            auto mappedContent = convertSingalingContentToContentInfo(contentIdBySsrc(content.ssrc), content, webrtc::RtpTransceiverDirection::kSendOnly);
            
            cricket::TransportDescription transportDescription;
            cricket::TransportInfo transportInfo(mappedContent.mid(), transportDescription);
            mappedOffer->AddTransportInfo(transportInfo);
            
            mappedOffer->AddContent(std::move(mappedContent));
        }
    }
    
    /*int nextRemoteContentId = 0;
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
    
    while (mappedOffer->contents().size() < 4) {
        std::ostringstream contentIdString;
        contentIdString << mappedOffer->contents().size();
        
        std::unique_ptr<cricket::MediaContentDescription> contentDescription;
        
        auto audioDescription = std::make_unique<cricket::AudioContentDescription>();
        
        contentDescription = std::move(audioDescription);
        
        contentDescription->set_direction(webrtc::RtpTransceiverDirection::kRecvOnly);
        contentDescription->set_rtcp_mux(true);
        
        cricket::ContentInfo mappedContentInfo(cricket::MediaProtocolType::kRtp);
        mappedContentInfo.name = contentIdString.str();
        mappedContentInfo.rejected = false;
        mappedContentInfo.bundle_only = false;
        mappedContentInfo.set_media_description(std::move(contentDescription));
        
        mappedOffer->AddContent(std::move(mappedContentInfo));
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
    
    for (const auto &channel : _outgoingChannels) {
        answerOptions.media_description_options.push_back(channel);
    }*/
    
    auto currentSessionDescription = currentSessionDescriptionFromCoordinatedState();
    
    std::unique_ptr<cricket::SessionDescription> answer = _sessionDescriptionFactory->CreateAnswer(mappedOffer.get(), answerOptions, currentSessionDescription.get());
    
    auto mappedAnswer = std::make_unique<NegotiationContents>();
    
    mappedAnswer->exchangeId = offer->exchangeId;
    
    std::vector<signaling::MediaContent> incomingChannels;
    
    for (const auto &content : answer->contents()) {
        auto mappedContent = convertContentInfoToSingalingContent(content);
        /*if (content.media_description()->direction() == webrtc::RtpTransceiverDirection::kSendOnly) {
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
        } else */
        
        if (content.media_description()->direction() == webrtc::RtpTransceiverDirection::kRecvOnly) {
            /*int nextRemoteContentId = 0;
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
            }*/
            
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

void ContentCoordinationContext::setAnswer(std::unique_ptr<ContentCoordinationContext::NegotiationContents> &&answer) {
    if (!_pendingOutgoingOffer) {
        return;
    }
    if (_pendingOutgoingOffer->exchangeId != answer->exchangeId) {
        return;
    }
    
    _pendingOutgoingOffer.reset();
    _needNegotiation = false;
    
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

std::string ContentCoordinationContext::takeNextOutgoingChannelId() {
    std::ostringstream result;
    result << (_isOutgoing ? "0" : "1") << _nextOutgoingChannelId;
    _nextOutgoingChannelId++;
    
    return result.str();
}

std::unique_ptr<ContentCoordinationContext::CoordinatedState> ContentCoordinationContext::coordinatedState() const {
    auto result = std::make_unique<ContentCoordinationContext::CoordinatedState>();
    
    result->incomingContents = _incomingChannels;
    for (const auto &channel : _outgoingChannels) {
        result->outgoingContents.push_back(channel.content);
    }
    
    return result;
}

} // namespace tgcalls
