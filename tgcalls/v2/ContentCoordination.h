#ifndef TGCALLS_CONTENT_COORDINATION_H
#define TGCALLS_CONTENT_COORDINATION_H

#include <memory>

#include "pc/channel_manager.h"
#include "pc/media_session.h"
#include "pc/session_description.h"
#include "p2p/base/transport_description_factory.h"

#include "v2/Signaling.h"

namespace tgcalls {

class ContentCoordinationContext {
public:
    enum class MediaType {
        Audio, Video
    };
    
    struct Offer {
        int exchangeId = 0;
        std::vector<signaling::MediaContent> outgoingContents;
        std::vector<signaling::MediaContent> incomingContents;
    };
    
    struct PendingOutgoingOffer {
        int exchangeId = 0;
    };
    
    struct Answer {
        int exchangeId = 0;
        std::vector<signaling::MediaContent> outgoingContents;
        std::vector<signaling::MediaContent> incomingContents;
    };
    
    struct CoordinatedState {
        std::vector<signaling::MediaContent> outgoingContents;
        std::vector<signaling::MediaContent> incomingContents;
    };
    
public:
    ContentCoordinationContext(bool isOutgoing, cricket::ChannelManager *channelManager, rtc::UniqueRandomIdGenerator *uniqueRandomIdGenerator);
    ~ContentCoordinationContext();
    
    void addOutgoingChannel(MediaType mediaType);
    
    std::unique_ptr<Offer> getOffer();
    std::unique_ptr<Answer> getAnwer(std::unique_ptr<Offer> offer);
    void setAnswer(std::unique_ptr<Answer> answer);
    
    CoordinatedState *coordinatedState() const {
        return _coordinatedState.get();
    }
    
private:
    std::string takeNextOutgoingChannelId();
    
private:
    bool _isOutgoing = false;
    std::unique_ptr<cricket::TransportDescriptionFactory> _transportDescriptionFactory;
    std::unique_ptr<cricket::MediaSessionDescriptionFactory> _sessionDescriptionFactory;
    
    std::vector<webrtc::RtpHeaderExtensionCapability> _rtpAudioExtensions;
    std::vector<webrtc::RtpHeaderExtensionCapability> _rtpVideoExtensions;
    
    std::vector<cricket::MediaDescriptionOptions> _outgoingChannels;
    
    int _nextExchangeId = 0;
    std::unique_ptr<PendingOutgoingOffer> _pendingOutgoingOffer;
    
    std::unique_ptr<CoordinatedState> _coordinatedState;
    
    int _nextOutgoingChannelId = 0;
    
};

} // namespace tgcalls

#endif
