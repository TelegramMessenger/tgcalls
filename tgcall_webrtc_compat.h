/*
 * tgcall_webrtc_compat.h
 *
 * HarmonyOS port compatibility header.
 *
 * This header is force-included via `-include` for every TU.
 *
 * It bridges differences between the tgcalls codebase and the
 * (much newer) upstream WebRTC revision shipped as libohos_webrtc.so:
 *
 *   * scoped_refptr: upstream now defines `webrtc::scoped_refptr`
 *     directly in api/scoped_refptr.h (namespace webrtc), so no
 *     rtc:: alias is needed anymore.
 *   * SctpTransportFactory::CreateSctpTransport() changed its signature
 *     from `(PacketTransportInternal*)` to
 *     `(const Environment&, DtlsTransportInternal*)`. tgcalls feeds it
 *     several custom PacketTransportInternal implementations (e.g.
 *     SignalingPacketTransport / DirectPacketTransport) that are not
 *     real DTLS transports. TgcallDtlsTransportAdapter wraps such a
 *     transport and presents it as a DtlsTransportInternal, forwarding
 *     the underlying packet / writable / close signals so that
 *     DcSctpTransport can keep working.
 */
#ifndef TGCALL_WEBRTC_COMPAT_H_
#define TGCALL_WEBRTC_COMPAT_H_

#include "api/scoped_refptr.h"

#include <memory>
#include <optional>
#include <string>

#include "absl/strings/string_view.h"
#include "p2p/dtls/dtls_transport_internal.h"
#include "p2p/base/packet_transport_internal.h"
#include "rtc_base/async_packet_socket.h"
#include "rtc_base/buffer.h"
#include "rtc_base/network/received_packet.h"
#include "rtc_base/network/sent_packet.h"

namespace tgcalls {

// Wraps any PacketTransportInternal so it can be handed to the new
// SctpTransportFactory::CreateSctpTransport(const Environment&,
// DtlsTransportInternal*). All DTLS-specific methods return defaults
// since tgcalls' custom transports do not implement DTLS.
class TgcallDtlsTransportAdapter final : public webrtc::DtlsTransportInternal {
public:
  explicit TgcallDtlsTransportAdapter(webrtc::PacketTransportInternal *inner)
      : inner_(inner) {
    if (!inner_) {
      return;
    }
    inner_->SubscribeWritableState(this, [this](webrtc::PacketTransportInternal *) {
      NotifyWritableState(this);
    });
    inner_->SubscribeReadyToSend(this, [this](webrtc::PacketTransportInternal *) {
      NotifyReadyToSend(this);
    });
    inner_->SubscribeReceivingState(this, [this](webrtc::PacketTransportInternal *) {
      NotifyReceivingState(this);
    });
    inner_->RegisterReceivedPacketCallback(
        this, [this](webrtc::PacketTransportInternal *,
                     const webrtc::ReceivedIpPacket &packet) {
          NotifyPacketReceived(packet);
        });
    inner_->SetOnCloseCallback([this]() { NotifyOnClose(); });
  }

  ~TgcallDtlsTransportAdapter() override {
    if (!inner_) {
      return;
    }
    inner_->UnsubscribeWritableState(this);
    inner_->UnsubscribeReadyToSend(this);
    inner_->UnsubscribeReceivingState(this);
    inner_->DeregisterReceivedPacketCallback(this);
    inner_->SetOnCloseCallback(nullptr);
  }

  TgcallDtlsTransportAdapter(const TgcallDtlsTransportAdapter &) = delete;
  TgcallDtlsTransportAdapter &operator=(const TgcallDtlsTransportAdapter &) = delete;

  // ---- PacketTransportInternal ----
  const std::string &transport_name() const override {
    return inner_->transport_name();
  }
  bool writable() const override { return inner_->writable(); }
  bool receiving() const override { return inner_->receiving(); }
  int SendPacket(const char *data, size_t len,
                 const webrtc::AsyncSocketPacketOptions &options,
                 int flags = 0) override {
    return inner_->SendPacket(data, len, options, flags);
  }
  int SetOption(webrtc::Socket::Option opt, int value) override {
    return inner_->SetOption(opt, value);
  }
  int GetError() override { return inner_->GetError(); }

  // ---- DtlsTransportInternal (defaults; no real DTLS) ----
  webrtc::DtlsTransportState dtls_state() const override {
    return webrtc::DtlsTransportState::kNew;
  }
  int component() const override { return 0; }
  bool IsDtlsActive() const override { return false; }
  bool GetDtlsRole(webrtc::SSLRole *role) const override { return false; }
  bool SetDtlsRole(webrtc::SSLRole role) override { return false; }
  bool GetSslVersionBytes(int *version) const override { return false; }
  uint16_t GetSslGroupId() const override { return 0; }
  bool GetSrtpCryptoSuite(int *cipher) const override { return false; }
  bool GetSslCipherSuite(int *cipher) const override { return false; }
  std::optional<absl::string_view> GetTlsCipherSuiteName() const override {
    return std::nullopt;
  }
  uint16_t GetSslPeerSignatureAlgorithm() const override { return 0; }
  bool SetLocalCertificate(
      const webrtc::scoped_refptr<webrtc::RTCCertificate> &certificate) override {
    return false;
  }
  std::unique_ptr<webrtc::SSLCertChain> GetRemoteSSLCertChain() const override {
    return nullptr;
  }
  bool AppendSrtpKeyingMaterial(
      webrtc::ZeroOnFreeBuffer<uint8_t> &keying_material) override {
    return false;
  }
  webrtc::RTCError SetRemoteParameters(
      absl::string_view digest_alg, const uint8_t *digest, size_t digest_len,
      std::optional<webrtc::SSLRole> role) override {
    return webrtc::RTCError(webrtc::RTCErrorType::UNSUPPORTED_OPERATION);
  }
  webrtc::IceTransportInternal *ice_transport() override { return nullptr; }

private:
  webrtc::PacketTransportInternal *inner_;
};

}  // namespace tgcalls

#endif  // TGCALL_WEBRTC_COMPAT_H_
