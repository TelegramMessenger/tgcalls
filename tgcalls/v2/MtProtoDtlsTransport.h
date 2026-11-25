#ifndef TGCALLS_MTPROTO_DTLS_TRANSPORT_H
#define TGCALLS_MTPROTO_DTLS_TRANSPORT_H

#include <memory>
#include <string>

#include "Instance.h"
#include "api/dtls_transport_interface.h"
#include "api/sequence_checker.h"
#include "p2p/dtls/dtls_transport_factory.h"
#include "p2p/dtls/dtls_transport_internal.h"
#include "api/ice_transport_interface.h"
#include "rtc_base/network/received_packet.h"
#include "rtc_base/network/sent_packet.h"
#include "p2p/base/ice_transport_internal.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/rtc_certificate.h"

namespace tgcalls {

class EncryptedConnection;

// mtproto in the DTLS slot. Injected through
// PeerConnectionDependencies::dtls_transport_factory (a tgcalls seam in the
// vendored webrtc) together with Options::external_transport_security, which
// makes JsepTransportController build a plain RtpTransport above us while
// DTLS stays enabled for SDP: certificates, fingerprints and the SCTP factory
// are stock. We never handshake - the shared secret exists before the call
// starts - and report kConnected the first time ICE becomes writable.
//
// Framing is byte-identical to 13.0.0's MtProtoPacketTransport, and `flags`
// is ground truth in both directions: BaseChannel sends RTP/RTCP with
// PF_SRTP_BYPASS (framed bare) and DcSctpTransport sends SCTP with 0 (framed
// with the 0xdcdcdcdc prefix); on receive the prefix becomes flags 0 and its
// absence PF_SRTP_BYPASS, so DcSctpTransport (which skips flags != 0) never
// parses media and RtpTransport (which ignores flags) demuxes it.
//
// Design record: telegram-ios
// docs/superpowers/specs/2026-09-16-tgcalls-mtproto-dtls-slot-design.md.
class MtProtoDtlsTransport : public webrtc::DtlsTransportInternal {
public:
    MtProtoDtlsTransport(webrtc::scoped_refptr<webrtc::IceTransportInterface> ice, EncryptionKey encryptionKey);
    ~MtProtoDtlsTransport() override;

    // webrtc::PacketTransportInternal
    const std::string &transport_name() const override;
    bool writable() const override;
    bool receiving() const override;
    int SendPacket(const char *data, size_t len, const webrtc::AsyncSocketPacketOptions &options, int flags) override;
    int SetOption(webrtc::Socket::Option opt, int value) override;
    bool GetOption(webrtc::Socket::Option opt, int *value) override;
    int GetError() override;
    std::optional<webrtc::NetworkRoute> network_route() const override;

    // webrtc::DtlsTransportInternal
    webrtc::DtlsTransportState dtls_state() const override;
    int component() const override;
    bool IsDtlsActive() const override;
    bool GetDtlsRole(webrtc::SSLRole *role) const override;
    bool SetDtlsRole(webrtc::SSLRole role) override;
    bool GetSslVersionBytes(int *version) const override;
    uint16_t GetSslGroupId() const override;
    bool GetSrtpCryptoSuite(int *cipher) const override;
    bool GetSslCipherSuite(int *cipher) const override;
    std::optional<absl::string_view> GetTlsCipherSuiteName() const override;
    uint16_t GetSslPeerSignatureAlgorithm() const override;
    bool SetLocalCertificate(const webrtc::scoped_refptr<webrtc::RTCCertificate> &certificate) override;
    std::unique_ptr<webrtc::SSLCertChain> GetRemoteSSLCertChain() const override;
    bool AppendSrtpKeyingMaterial(webrtc::ZeroOnFreeBuffer<uint8_t> &keying_material) override;
    void MaybeStartDtlsInStun() override;
    webrtc::RTCError SetRemoteParameters(absl::string_view digest_alg, const uint8_t *digest, size_t digest_len, std::optional<webrtc::SSLRole> role) override;
    webrtc::IceTransportInternal *ice_transport() override;

private:
    // The webrtc::PacketTransportInternal events of the ICE transport, re-emitted
    // with `this` (the controller keys transports by pointer), as stock
    // DtlsTransport subscribes to them.
    void onIceWritableState(webrtc::PacketTransportInternal *transport);
    void onIceReceivingState(webrtc::PacketTransportInternal *transport);
    void onIceReadyToSend(webrtc::PacketTransportInternal *transport);
    void onIceReadPacket(webrtc::PacketTransportInternal *transport, const webrtc::ReceivedIpPacket &packet);
    void onIceSentPacket(webrtc::PacketTransportInternal *transport, const webrtc::SentPacketInfo &packet);
    void onIceNetworkRouteChanged(std::optional<webrtc::NetworkRoute> route);

    void emitDecryptedMessage(webrtc::CopyOnWriteBuffer const &message, const webrtc::ReceivedIpPacket &original);
    void setDtlsState(webrtc::DtlsTransportState state);

    // Everything runs on the network thread, like stock webrtc::DtlsTransport:
    // the controller constructs us there and every DtlsTransportInternal call
    // webrtc makes hops there first. The checker attaches on construction.
    webrtc::SequenceChecker _sequenceChecker;

    // Keeps the ICE transport alive as long as we are, as stock DtlsTransport does.
    webrtc::scoped_refptr<webrtc::IceTransportInterface> _iceRef;
    webrtc::IceTransportInternal *_ice = nullptr;
    std::unique_ptr<EncryptedConnection> _transportEncryption;
    webrtc::DtlsTransportState _dtlsState = webrtc::DtlsTransportState::kNew;
    std::optional<webrtc::SSLRole> _dtlsRole;
    webrtc::scoped_refptr<webrtc::RTCCertificate> _localCertificate;
    // Set when OUR framing/encryption fails, so GetError() does not surface the
    // ICE channel's stale error (which RtpTransport reads as a lost connection).
    int _lastError = 0;
};

// Installed on PeerConnectionDependencies::dtls_transport_factory by
// InstanceV2ReferenceImpl and CallCoreHost, always together with
// Options::external_transport_security, from the one network_use_mtproto
// decision.
class MtProtoDtlsTransportFactory : public webrtc::DtlsTransportFactory {
public:
    explicit MtProtoDtlsTransportFactory(EncryptionKey encryptionKey);

    std::unique_ptr<webrtc::DtlsTransportInternal> CreateDtlsTransport(
        webrtc::scoped_refptr<webrtc::IceTransportInterface> ice,
        const webrtc::CryptoOptions &crypto_options,
        webrtc::SSLProtocolVersion max_version) override;

private:
    EncryptionKey _encryptionKey;
};

} // namespace tgcalls

#endif
