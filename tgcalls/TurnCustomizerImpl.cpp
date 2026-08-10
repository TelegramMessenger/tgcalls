#include "TurnCustomizerImpl.h"

#include "api/transport/stun.h"

namespace tgcalls {

TurnCustomizerImpl::TurnCustomizerImpl() {
}

TurnCustomizerImpl::~TurnCustomizerImpl() {
}

void TurnCustomizerImpl::MaybeModifyOutgoingStunMessage(webrtc::PortInterface* port, webrtc::StunMessage* message) {
    message->AddAttribute(std::make_unique<webrtc::StunByteStringAttribute>(webrtc::STUN_ATTR_SOFTWARE, "Telegram "));
}

bool TurnCustomizerImpl::AllowChannelData(webrtc::PortInterface* port, const void *data, size_t size, bool payload) {
    return true;
}

}
