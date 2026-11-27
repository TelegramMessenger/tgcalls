#ifndef TGCALLS_SIGNALING_ENCRYPTION_H
#define TGCALLS_SIGNALING_ENCRYPTION_H

#include <cstdint>
#include "Instance.h"
#include "EncryptedConnection.h"

namespace tgcalls {

class SignalingEncryption {
public:
    SignalingEncryption(EncryptionKey const &encryptionKey);
    ~SignalingEncryption();

    std::optional<webrtc::CopyOnWriteBuffer> encryptOutgoing(std::vector<uint8_t> const &data);
    std::optional<webrtc::CopyOnWriteBuffer> decryptIncoming(std::vector<uint8_t> const &data);

private:
    std::unique_ptr<EncryptedConnection> _connection;
};

} // namespace tgcalls

#endif
