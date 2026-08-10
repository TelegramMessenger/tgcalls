#include "v2/SignalingEncryption.h"

namespace tgcalls {

SignalingEncryption::SignalingEncryption(EncryptionKey const &encryptionKey) {
    _connection.reset(new EncryptedConnection(EncryptedConnection::Type::Signaling, encryptionKey, [](int, int) {
    }));
}

SignalingEncryption::~SignalingEncryption() {

}

absl::optional<webrtc::CopyOnWriteBuffer> SignalingEncryption::encryptOutgoing(std::vector<uint8_t> const &data) {
    return _connection->encryptRawPacket(webrtc::CopyOnWriteBuffer(data.data(), data.size()));
}

absl::optional<webrtc::CopyOnWriteBuffer> SignalingEncryption::decryptIncoming(std::vector<uint8_t> const &data) {
    return _connection->decryptRawPacket(webrtc::CopyOnWriteBuffer(data.data(), data.size()));
}

} // namespace tgcalls
