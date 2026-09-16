#include "v2/MtProtoDtlsTransport.h"

#include "EncryptedConnection.h"

#include <cerrno>
#include <cstring>

namespace {

// Matches 13.0.0's MtProtoPacketTransport (NativeNetworkingImpl.cpp).
constexpr uint32_t kSctpMagic = 0xdcdcdcdc;

} // namespace

namespace tgcalls {

MtProtoDtlsTransport::MtProtoDtlsTransport(cricket::IceTransportInternal *ice, EncryptionKey encryptionKey) :
_ice(ice) {
    RTC_DCHECK_RUN_ON(&_sequenceChecker);
    _transportEncryption = std::make_unique<EncryptedConnection>(
        EncryptedConnection::Type::Transport,
        encryptionKey,
        [](int delayMs, int cause) {
        }
    );

    _ice->SignalWritableState.connect(this, &MtProtoDtlsTransport::onIceWritableState);
    _ice->SignalReceivingState.connect(this, &MtProtoDtlsTransport::onIceReceivingState);
    _ice->SignalReadyToSend.connect(this, &MtProtoDtlsTransport::onIceReadyToSend);
    _ice->SignalReadPacket.connect(this, &MtProtoDtlsTransport::onIceReadPacket);
    _ice->SignalSentPacket.connect(this, &MtProtoDtlsTransport::onIceSentPacket);
    _ice->SignalNetworkRouteChanged.connect(this, &MtProtoDtlsTransport::onIceNetworkRouteChanged);
    _ice->SignalClosed.connect(this, &MtProtoDtlsTransport::onIceClosed);
}

MtProtoDtlsTransport::~MtProtoDtlsTransport() {
    _ice->SignalWritableState.disconnect(this);
    _ice->SignalReceivingState.disconnect(this);
    _ice->SignalReadyToSend.disconnect(this);
    _ice->SignalReadPacket.disconnect(this);
    _ice->SignalSentPacket.disconnect(this);
    _ice->SignalNetworkRouteChanged.disconnect(this);
    _ice->SignalClosed.disconnect(this);
}

// ---- rtc::PacketTransportInternal ----

const std::string &MtProtoDtlsTransport::transport_name() const {
    return _ice->transport_name();
}

bool MtProtoDtlsTransport::writable() const {
    return _ice->writable();
}

bool MtProtoDtlsTransport::receiving() const {
    return _ice->receiving();
}

int MtProtoDtlsTransport::SendPacket(const char *data, size_t len, const rtc::PacketOptions &options, int flags) {
    RTC_DCHECK_RUN_ON(&_sequenceChecker);
    // `flags` is ground truth (13.0.0 semantics): BaseChannel sends RTP and RTCP
    // with PF_SRTP_BYPASS, DcSctpTransport sends SCTP with 0.
    rtc::CopyOnWriteBuffer buffer;
    if (flags == 0) {
        uint32_t magic = kSctpMagic;
        buffer.AppendData((const unsigned char *)&magic, 4);
    }
    buffer.AppendData((const unsigned char *)data, len);

    const auto encryptedPacket = _transportEncryption->prepareForSendingRawMessage(buffer, false);
    if (!encryptedPacket) {
        // Our failure (oversized packet or exhausted counter), not the network's.
        // Report it as such: with _lastError clear, GetError() would forward the
        // ICE channel's never-cleared last error, and a stale ENOTCONN there is
        // exactly what makes RtpTransport drop ready-to-send.
        _lastError = EMSGSIZE;
        return -1;
    }

    // Flags never reach the ICE channel: P2PTransportChannel::SendPacket rejects
    // any non-zero value with EINVAL.
    const int sent = _ice->SendPacket((const char *)encryptedPacket->bytes.data(), encryptedPacket->bytes.size(), options, 0);
    _lastError = 0;
    if (sent < 0) {
        return sent;
    }

    // PacketTransportInternal contract: report the CALLER's byte count, never the
    // ciphertext's. RtpTransport::SendPacket treats any other value as a failed
    // send and then consults the ICE channel's last error, which
    // P2PTransportChannel never clears; a stale ENOTCONN would then drop
    // ready-to-send and pause the pacer for a call whose packets are going out.
    return (int)len;
}

int MtProtoDtlsTransport::SetOption(rtc::Socket::Option opt, int value) {
    return _ice->SetOption(opt, value);
}

bool MtProtoDtlsTransport::GetOption(rtc::Socket::Option opt, int *value) {
    return _ice->GetOption(opt, value);
}

int MtProtoDtlsTransport::GetError() {
    return _lastError != 0 ? _lastError : _ice->GetError();
}

absl::optional<rtc::NetworkRoute> MtProtoDtlsTransport::network_route() const {
    return _ice->network_route();
}

// ---- cricket::DtlsTransportInternal ----

webrtc::DtlsTransportState MtProtoDtlsTransport::dtls_state() const {
    return _dtlsState;
}

int MtProtoDtlsTransport::component() const {
    return _ice->component();
}

bool MtProtoDtlsTransport::IsDtlsActive() const {
    return true;
}

bool MtProtoDtlsTransport::GetDtlsRole(rtc::SSLRole *role) const {
    if (!_dtlsRole) {
        return false;
    }
    *role = *_dtlsRole;
    return true;
}

bool MtProtoDtlsTransport::SetDtlsRole(rtc::SSLRole role) {
    RTC_DCHECK_RUN_ON(&_sequenceChecker);
    _dtlsRole = role;
    return true;
}

bool MtProtoDtlsTransport::GetSslVersionBytes(int *version) const {
    return false;
}

bool MtProtoDtlsTransport::GetSrtpCryptoSuite(int *cipher) {
    // No SRTP: the plain RtpTransport above us never asks for keys, and
    // returning false keeps the stats wrapper's fields absent rather than wrong.
    return false;
}

bool MtProtoDtlsTransport::GetSslCipherSuite(int *cipher) {
    return false;
}

uint16_t MtProtoDtlsTransport::GetSslPeerSignatureAlgorithm() const {
    return 0;
}

rtc::scoped_refptr<rtc::RTCCertificate> MtProtoDtlsTransport::GetLocalCertificate() const {
    return _localCertificate;
}

bool MtProtoDtlsTransport::SetLocalCertificate(const rtc::scoped_refptr<rtc::RTCCertificate> &certificate) {
    RTC_DCHECK_RUN_ON(&_sequenceChecker);
    // Stored only so the controller's fingerprint lands in the SDP; it is never
    // used for a handshake.
    _localCertificate = certificate;
    return true;
}

std::unique_ptr<rtc::SSLCertChain> MtProtoDtlsTransport::GetRemoteSSLCertChain() const {
    return nullptr;
}

bool MtProtoDtlsTransport::ExportKeyingMaterial(absl::string_view label, const uint8_t *context, size_t context_len, bool use_context, uint8_t *result, size_t result_len) {
    return false;
}

bool MtProtoDtlsTransport::SetRemoteFingerprint(absl::string_view digest_alg, const uint8_t *digest, size_t digest_len) {
    return SetRemoteParameters(digest_alg, digest, digest_len, absl::nullopt).ok();
}

webrtc::RTCError MtProtoDtlsTransport::SetRemoteParameters(absl::string_view digest_alg, const uint8_t *digest, size_t digest_len, absl::optional<rtc::SSLRole> role) {
    RTC_DCHECK_RUN_ON(&_sequenceChecker);
    // The fingerprint is accepted and discarded, never verified: mtproto's
    // shared key is the authentication. The role is what JsepTransport
    // negotiated from a=setup, and the only part anyone reads back.
    if (role) {
        _dtlsRole = role;
    }
    return webrtc::RTCError::OK();
}

cricket::IceTransportInternal *MtProtoDtlsTransport::ice_transport() {
    return _ice;
}

// ---- ICE signal bridges ----

void MtProtoDtlsTransport::onIceWritableState(rtc::PacketTransportInternal *) {
    RTC_DCHECK_RUN_ON(&_sequenceChecker);
    if (_ice->writable() && _dtlsState == webrtc::DtlsTransportState::kNew) {
        // No handshake: the shared key existed before the call started.
        setDtlsState(webrtc::DtlsTransportState::kConnected);
    }
    SignalWritableState(this);
}

void MtProtoDtlsTransport::onIceReceivingState(rtc::PacketTransportInternal *) {
    RTC_DCHECK_RUN_ON(&_sequenceChecker);
    SignalReceivingState(this);
}

void MtProtoDtlsTransport::onIceReadyToSend(rtc::PacketTransportInternal *) {
    RTC_DCHECK_RUN_ON(&_sequenceChecker);
    if (writable()) {
        SignalReadyToSend(this);
    }
}

void MtProtoDtlsTransport::onIceReadPacket(rtc::PacketTransportInternal *, const char *data, size_t size, const int64_t &timestamp, int flags) {
    RTC_DCHECK_RUN_ON(&_sequenceChecker);
    if (const auto packet = _transportEncryption->handleIncomingRawPacket(data, size)) {
        emitDecryptedMessage(packet.value().main.message, timestamp);
        for (const auto &additional : packet.value().additional) {
            emitDecryptedMessage(additional.message, timestamp);
        }
    }
    // Undecryptable: nothing is emitted. EncryptedConnection logs one
    // "ERROR! Bad incoming data hash." per such packet, as it does in 13.0.0;
    // a flood of those in a call log means the peer is not speaking mtproto.
}

void MtProtoDtlsTransport::emitDecryptedMessage(rtc::CopyOnWriteBuffer const &message, int64_t timestamp) {
    // The prefix IS the demux: SCTP surfaces with flags 0, which DcSctpTransport
    // accepts; everything else with PF_SRTP_BYPASS, which DcSctpTransport skips
    // and RtpTransport ignores. Active DTLS emits the same flag for SRTP.
    if (message.size() >= 4) {
        uint32_t header = 0;
        memcpy(&header, message.data(), 4);
        if (header == kSctpMagic) {
            SignalReadPacket(this, (const char *)(message.data() + 4), message.size() - 4, timestamp, 0);
            return;
        }
    }
    SignalReadPacket(this, (const char *)message.data(), message.size(), timestamp, cricket::PF_SRTP_BYPASS);
}

void MtProtoDtlsTransport::onIceSentPacket(rtc::PacketTransportInternal *, const rtc::SentPacket &packet) {
    RTC_DCHECK_RUN_ON(&_sequenceChecker);
    SignalSentPacket(this, packet);
}

void MtProtoDtlsTransport::onIceNetworkRouteChanged(absl::optional<rtc::NetworkRoute> route) {
    RTC_DCHECK_RUN_ON(&_sequenceChecker);
    SignalNetworkRouteChanged(route);
}

void MtProtoDtlsTransport::onIceClosed(rtc::PacketTransportInternal *) {
    RTC_DCHECK_RUN_ON(&_sequenceChecker);
    SignalClosed(this);
}

void MtProtoDtlsTransport::setDtlsState(webrtc::DtlsTransportState state) {
    if (_dtlsState == state) {
        return;
    }
    _dtlsState = state;
    // Published so the aggregate PeerConnectionState and the webrtc::DtlsTransport
    // stats wrapper see a connected transport.
    SendDtlsState(this, state);
}

// ---- factory ----

MtProtoDtlsTransportFactory::MtProtoDtlsTransportFactory(EncryptionKey encryptionKey) :
_encryptionKey(std::move(encryptionKey)) {
}

std::unique_ptr<cricket::DtlsTransportInternal> MtProtoDtlsTransportFactory::CreateDtlsTransport(
    cricket::IceTransportInternal *ice,
    const webrtc::CryptoOptions &crypto_options,
    rtc::SSLProtocolVersion max_version
) {
    return std::make_unique<MtProtoDtlsTransport>(ice, _encryptionKey);
}

} // namespace tgcalls
