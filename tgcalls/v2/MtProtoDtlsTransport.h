#ifndef TGCALLS_MTPROTO_DTLS_TRANSPORT_H
#define TGCALLS_MTPROTO_DTLS_TRANSPORT_H

#include <memory>
#include <string>

#include "Instance.h"
#include "api/dtls_transport_interface.h"
#include "p2p/base/dtls_transport_factory.h"
#include "p2p/base/dtls_transport_internal.h"
#include "p2p/base/ice_transport_internal.h"
#include "rtc_base/buffer.h"
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
class MtProtoDtlsTransport : public cricket::DtlsTransportInternal {
public:
    MtProtoDtlsTransport(cricket::IceTransportInternal *ice, EncryptionKey encryptionKey);
    ~MtProtoDtlsTransport() override;

    // rtc::PacketTransportInternal
    const std::string &transport_name() const override;
    bool writable() const override;
    bool receiving() const override;
    int SendPacket(const char *data, size_t len, const rtc::PacketOptions &options, int flags) override;
    int SetOption(rtc::Socket::Option opt, int value) override;
    bool GetOption(rtc::Socket::Option opt, int *value) override;
    int GetError() override;
    absl::optional<rtc::NetworkRoute> network_route() const override;

    // cricket::DtlsTransportInternal
    webrtc::DtlsTransportState dtls_state() const override;
    int component() const override;
    bool IsDtlsActive() const override;
    bool GetDtlsRole(rtc::SSLRole *role) const override;
    bool SetDtlsRole(rtc::SSLRole role) override;
    bool GetSslVersionBytes(int *version) const override;
    bool GetSrtpCryptoSuite(int *cipher) override;
    bool GetSslCipherSuite(int *cipher) override;
    uint16_t GetSslPeerSignatureAlgorithm() const override;
    rtc::scoped_refptr<rtc::RTCCertificate> GetLocalCertificate() const override;
    bool SetLocalCertificate(const rtc::scoped_refptr<rtc::RTCCertificate> &certificate) override;
    std::unique_ptr<rtc::SSLCertChain> GetRemoteSSLCertChain() const override;
    bool ExportKeyingMaterial(absl::string_view label, const uint8_t *context, size_t context_len, bool use_context, uint8_t *result, size_t result_len) override;
    bool SetRemoteFingerprint(absl::string_view digest_alg, const uint8_t *digest, size_t digest_len) override;
    webrtc::RTCError SetRemoteParameters(absl::string_view digest_alg, const uint8_t *digest, size_t digest_len, absl::optional<rtc::SSLRole> role) override;
    cricket::IceTransportInternal *ice_transport() override;

private:
    // The seven rtc::PacketTransportInternal signals of the ICE transport,
    // re-emitted with `this`: the controller keys transports by pointer.
    void onIceWritableState(rtc::PacketTransportInternal *transport);
    void onIceReceivingState(rtc::PacketTransportInternal *transport);
    void onIceReadyToSend(rtc::PacketTransportInternal *transport);
    void onIceReadPacket(rtc::PacketTransportInternal *transport, const char *data, size_t size, const int64_t &timestamp, int flags);
    void onIceSentPacket(rtc::PacketTransportInternal *transport, const rtc::SentPacket &packet);
    void onIceNetworkRouteChanged(absl::optional<rtc::NetworkRoute> route);
    void onIceClosed(rtc::PacketTransportInternal *transport);

    void emitDecryptedMessage(rtc::CopyOnWriteBuffer const &message, int64_t timestamp);
    void setDtlsState(webrtc::DtlsTransportState state);

    cricket::IceTransportInternal *_ice = nullptr;
    std::unique_ptr<EncryptedConnection> _transportEncryption;
    webrtc::DtlsTransportState _dtlsState = webrtc::DtlsTransportState::kNew;
    absl::optional<rtc::SSLRole> _dtlsRole;
    rtc::scoped_refptr<rtc::RTCCertificate> _localCertificate;
    std::string _remoteFingerprintAlgorithm;
    rtc::Buffer _remoteFingerprintValue;
};

// Installed on PeerConnectionDependencies::dtls_transport_factory by
// InstanceV2ReferenceImpl and CallCoreHost, always together with
// Options::external_transport_security, from the one network_use_mtproto
// decision.
class MtProtoDtlsTransportFactory : public cricket::DtlsTransportFactory {
public:
    explicit MtProtoDtlsTransportFactory(EncryptionKey encryptionKey);

    std::unique_ptr<cricket::DtlsTransportInternal> CreateDtlsTransport(
        cricket::IceTransportInternal *ice,
        const webrtc::CryptoOptions &crypto_options,
        rtc::SSLProtocolVersion max_version) override;

private:
    EncryptionKey _encryptionKey;
};

} // namespace tgcalls

#endif
