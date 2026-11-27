#include "v2/SignalingEncryption.h"

namespace tgcalls {

SignalingEncryption::SignalingEncryption(EncryptionKey const &encryptionKey) {
    _connection.reset(new EncryptedConnection(EncryptedConnection::Type::Signaling, encryptionKey, [](int, int) {
    }));
}

SignalingEncryption::~SignalingEncryption() {

}

std::optional<webrtc::CopyOnWriteBuffer> SignalingEncryption::encryptOutgoing(std::vector<uint8_t> const &data) {
    return _connection->encryptRawPacket(webrtc::CopyOnWriteBuffer(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(data.data()), data.size())));
}

std::optional<webrtc::CopyOnWriteBuffer> SignalingEncryption::decryptIncoming(std::vector<uint8_t> const &data) {
    return _connection->decryptRawPacket(webrtc::CopyOnWriteBuffer(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(data.data()), data.size())));
}

} // namespace tgcalls
