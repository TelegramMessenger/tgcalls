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

    absl::optional<webrtc::CopyOnWriteBuffer> encryptOutgoing(std::vector<uint8_t> const &data);
    absl::optional<webrtc::CopyOnWriteBuffer> decryptIncoming(std::vector<uint8_t> const &data);

private:
    std::unique_ptr<EncryptedConnection> _connection;
};

} // namespace tgcalls

#endif
