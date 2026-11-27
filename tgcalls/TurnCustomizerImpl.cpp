#include "TurnCustomizerImpl.h"

#include "api/transport/stun.h"

#include <optional>
#include <string>

namespace tgcalls {

TurnCustomizerImpl::TurnCustomizerImpl() {
}

TurnCustomizerImpl::~TurnCustomizerImpl() {
}

// STUN marking: every TURN request carries SOFTWARE "Telegram ". TurnPort signs an authenticated request (allocate
// after the server's challenge, refresh, permission, channel bind) before it calls this, and adding anything but
// integrity attributes to a signed message is an RTC_CHECK since M159 (release builds abort): the signature comes off,
// SOFTWARE goes in, and the message is signed again with the same key, so the marking sits inside the signed part.
// Test: turn_customizer_test.
void TurnCustomizerImpl::MaybeModifyOutgoingStunMessage(webrtc::PortInterface* port, webrtc::StunMessage* message) {
    std::optional<std::string> signingKey;
    if (message->IntegrityOk()) {
        signingKey = message->password();
        message->RemoveAttribute(webrtc::STUN_ATTR_MESSAGE_INTEGRITY);
    }
    message->AddAttribute(std::make_unique<webrtc::StunByteStringAttribute>(webrtc::STUN_ATTR_SOFTWARE, "Telegram "));
    if (signingKey) {
        message->AddMessageIntegrity(*signingKey);
    }
}

bool TurnCustomizerImpl::AllowChannelData(webrtc::PortInterface* port, const void *data, size_t size, bool payload) {
    return true;
}

}
