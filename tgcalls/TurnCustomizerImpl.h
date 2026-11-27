#ifndef TGCALLS_TURN_CUSTOMIZER_H
#define TGCALLS_TURN_CUSTOMIZER_H

#include "api/turn_customizer.h"

namespace tgcalls {

class TurnCustomizerImpl : public webrtc::TurnCustomizer {
public:
    TurnCustomizerImpl();
    virtual ~TurnCustomizerImpl();

    void MaybeModifyOutgoingStunMessage(webrtc::PortInterface* port, webrtc::StunMessage* message) override;
    bool AllowChannelData(webrtc::PortInterface* port, const void *data, size_t size, bool payload) override;
};

} // namespace tgcalls

#endif
