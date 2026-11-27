#ifndef TGCALLS_CONTENT_NEGOTIATION_H
#define TGCALLS_CONTENT_NEGOTIATION_H

#include <cstdint>
#include <memory>

#include "api/environment/environment.h"
#include "media/base/media_engine.h"
#include "pc/media_session.h"
#include "pc/session_description.h"
#include "p2p/base/transport_description_factory.h"

#include "v2/Signaling.h"

namespace tgcalls {

class ContentNegotiationContext {
public:
    struct NegotiationContents {
        uint32_t exchangeId = 0;
        std::vector<signaling::MediaContent> contents;
    };
    
    struct PendingOutgoingOffer {
        uint32_t exchangeId = 0;
    };
    
    struct PendingOutgoingChannel {
        webrtc::MediaDescriptionOptions description;
        
        uint32_t ssrc = 0;
        std::vector<signaling::SsrcGroup> ssrcGroups;
        
        PendingOutgoingChannel(webrtc::MediaDescriptionOptions &&description_) :
        description(std::move(description_)) {
        }
    };
    
    struct OutgoingChannel {
        std::string id;
        signaling::MediaContent content;
        
        OutgoingChannel(std::string id_, signaling::MediaContent content_) :
        id(id_), content(content_) {
        }
    };
    
    struct CoordinatedState {
        std::vector<signaling::MediaContent> outgoingContents;
        std::vector<signaling::MediaContent> incomingContents;
    };
    
public:
    ContentNegotiationContext(const webrtc::Environment &env, bool isOutgoing, webrtc::MediaEngineInterface *mediaEngine, webrtc::UniqueRandomIdGenerator *uniqueRandomIdGenerator);
    ~ContentNegotiationContext();
    
    void copyCodecsFromChannelManager(webrtc::MediaEngineInterface *mediaEngine);
    
    std::string addOutgoingChannel(signaling::MediaContent::Type mediaType);
    void removeOutgoingChannel(std::string const &id);
    
    std::unique_ptr<NegotiationContents> getPendingOffer();
    std::unique_ptr<NegotiationContents> setRemoteNegotiationContent(std::unique_ptr<NegotiationContents> &&remoteNegotiationContent);
    
    std::unique_ptr<CoordinatedState> coordinatedState() const;
    std::optional<uint32_t> outgoingChannelSsrc(std::string const &id) const;
    
private:
    std::string takeNextOutgoingChannelId();
    std::unique_ptr<webrtc::SessionDescription> currentSessionDescriptionFromCoordinatedState();
    
    std::unique_ptr<NegotiationContents> getAnswer(std::unique_ptr<NegotiationContents> &&offer);
    void applyCodecPreferences(webrtc::MediaSessionOptions &options) const;
    void setAnswer(std::unique_ptr<NegotiationContents> &&answer);
    
private:
    bool _isOutgoing = false;
    webrtc::UniqueRandomIdGenerator *_uniqueRandomIdGenerator = nullptr;
    
    std::unique_ptr<webrtc::TransportDescriptionFactory> _transportDescriptionFactory;
    // The codecs the session description factory offers and answers with: the media engine's, payload types kept.
    std::unique_ptr<webrtc::CodecLookupHelper> _codecLookupHelper;
    std::unique_ptr<webrtc::MediaSessionDescriptionFactory> _sessionDescriptionFactory;
    // Audio is Opus alone (empty until copyCodecsFromChannelManager).
    std::vector<webrtc::RtpCodecCapability> _audioCodecPreferences;
    
    std::vector<std::string> _channelIdOrder;
    
    std::vector<webrtc::RtpHeaderExtensionCapability> _rtpAudioExtensions;
    std::vector<webrtc::RtpHeaderExtensionCapability> _rtpVideoExtensions;
    
    std::vector<PendingOutgoingChannel> _outgoingChannelDescriptions;
    bool _needNegotiation = false;
    
    std::vector<OutgoingChannel> _outgoingChannels;
    std::vector<signaling::MediaContent> _incomingChannels;
    
    std::unique_ptr<PendingOutgoingOffer> _pendingOutgoingOffer;
    
    int _nextOutgoingChannelId = 0;
    
};

} // namespace tgcalls

#endif
