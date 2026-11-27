#include "v2/MtProtoDtlsTransport.h"

#include "EncryptedConnection.h"

#include "api/crypto/crypto_options.h"
#include "api/environment/environment_factory.h"
#include "api/ice_transport_interface.h"
#include "api/sctp_transport_interface.h"
#include "media/sctp/dcsctp_transport.h"
#include "pc/rtp_transport.h"
#include "rtc_base/logging.h"
#include "rtc_base/network/received_packet.h"
#include "rtc_base/rtc_certificate.h"
#include "rtc_base/ssl_identity.h"
#include "rtc_base/thread.h"
#include "system_wrappers/include/clock.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <thread>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

// Non-zero material, shared by both ends. EncryptedConnection seeds its keys from
// `key + 88 + (isOutgoing ? 0 : 8)` on one side and the mirror offset on the other,
// so a pair built from the SAME direction only interoperates when the key is all
// zeroes - which is exactly what an all-zero fixture would hide.
std::shared_ptr<std::array<uint8_t, 256>> makeSharedKeyMaterial() {
    auto key = std::make_shared<std::array<uint8_t, 256>>();
    for (size_t i = 0; i < key->size(); i++) {
        (*key)[i] = (uint8_t)(i * 7 + 13);
    }
    return key;
}

tgcalls::EncryptionKey outgoingKey(std::shared_ptr<std::array<uint8_t, 256>> key) {
    return tgcalls::EncryptionKey(key, true);
}

tgcalls::EncryptionKey incomingKey(std::shared_ptr<std::array<uint8_t, 256>> key) {
    return tgcalls::EncryptionKey(key, false);
}

// A local stand-in for P2PTransportChannel. webrtc's webrtc::FakeIceTransport
// is not declared in third-party/webrtc/BUILD, so bazel's strict header check
// rejects it; this stubs the interface directly.
class FakeIceTransport : public webrtc::IceTransportInternal {
public:
    FakeIceTransport() : _transportName("fake") {
    }

    void setWritable(bool writable) {
        _writable = writable;
        NotifyWritableState(this);
    }
    void fireReadyToSend() { NotifyReadyToSend(this); }
    // What P2PTransportChannel::GetError() would report: its error_ is only
    // written on a failed send and never cleared, so a stale value is realistic.
    void setError(int error) { _error = error; }
    void deliverPacket(const char *data, size_t size) {
        NotifyPacketReceived(webrtc::ReceivedIpPacket(std::span<const uint8_t>((const uint8_t *)data, size), webrtc::SocketAddress()));
    }
    std::string lastSentPacket() const { return _lastSentPacket; }
    int sentPacketCount() const { return _sentPacketCount; }

    // webrtc::PacketTransportInternal
    const std::string &transport_name() const override { return _transportName; }
    bool writable() const override { return _writable; }
    bool receiving() const override { return _receiving; }
    // Return value override: kFakeSendReturnsLength (default) reports the byte
    // count as a UDP socket would; any other value is returned verbatim.
    static constexpr int kFakeSendReturnsLength = 0x7fffffff;
    void setSendResult(int result) { _sendResult = result; }
    int lastSentFlags() const { return _lastSentFlags; }

    int SendPacket(const char *data, size_t len, const webrtc::AsyncSocketPacketOptions &, int flags) override {
        _lastSentPacket.assign(data, len);
        _lastSentFlags = flags;
        _sentPacketCount++;
        return _sendResult == kFakeSendReturnsLength ? (int)len : _sendResult;
    }
    int SetOption(webrtc::Socket::Option, int) override { return 0; }
    bool GetOption(webrtc::Socket::Option, int *) override { return false; }
    int GetError() override { return _error; }
    std::optional<webrtc::NetworkRoute> network_route() const override { return std::nullopt; }

    // webrtc::IceTransportInternal
    webrtc::IceTransportStateInternal GetState() const override { return webrtc::IceTransportStateInternal::STATE_INIT; }
    webrtc::IceTransportState GetIceTransportState() const override { return webrtc::IceTransportState::kNew; }
    int component() const override { return 1; }
    webrtc::IceRole GetIceRole() const override { return webrtc::ICEROLE_CONTROLLING; }
    void SetIceRole(webrtc::IceRole) override {}
    void SetIceParameters(const webrtc::IceParameters &) override {}
    void SetRemoteIceParameters(const webrtc::IceParameters &) override {}
    void SetRemoteIceMode(webrtc::IceMode) override {}
    void SetIceConfig(const webrtc::IceConfig &) override {}
    void MaybeStartGathering() override {}
    void AddRemoteCandidate(const webrtc::Candidate &) override {}
    void RemoveRemoteCandidate(const webrtc::Candidate &) override {}
    void RemoveAllRemoteCandidates() override {}
    webrtc::IceGatheringState gathering_state() const override { return webrtc::kIceGatheringNew; }
    bool GetStats(webrtc::IceTransportStats *) override { return false; }
    std::optional<int> GetRttEstimate() override { return std::nullopt; }
    std::optional<const webrtc::CandidatePair> GetSelectedCandidatePair() const override { return std::nullopt; }

private:
    std::string _transportName;
    bool _writable = false;
    bool _receiving = false;
    int _sentPacketCount = 0;
    std::string _lastSentPacket;
    int _lastSentFlags = -1;
    int _sendResult = kFakeSendReturnsLength;
    int _error = 0;
};

// What JsepTransportController hands a DtlsTransportFactory: the ICE transport behind an IceTransportInterface.
class FakeIceTransportRef : public webrtc::IceTransportInterface {
public:
    explicit FakeIceTransportRef(webrtc::IceTransportInternal *ice) : _ice(ice) {
    }
    webrtc::IceTransportInternal *internal() override { return _ice; }

private:
    webrtc::IceTransportInternal *_ice;
};

webrtc::scoped_refptr<webrtc::IceTransportInterface> wrap(FakeIceTransport *ice) {
    return webrtc::make_ref_counted<FakeIceTransportRef>(ice);
}

// What a consumer above the transport sees of each packet: since M159 the demux label is the packet's
// DecryptionInfo (kDtlsDecrypted for SCTP, kSrtpEncrypted for RTP and RTCP), where M123 had read `flags`.
struct ReadPacketProbe {
    int lastDecryption = -1;
    int count = 0;
    std::string lastPayload;

    void attach(webrtc::PacketTransportInternal *transport) {
        transport->RegisterReceivedPacketCallback(this, [this](webrtc::PacketTransportInternal *, const webrtc::ReceivedIpPacket &packet) {
            lastDecryption = packet.decryption_info();
            lastPayload.assign((const char *)packet.payload().data(), packet.payload().size());
            count++;
        });
    }
};

struct CapturingLogSink : public webrtc::LogSink {
    std::vector<std::string> lines;
    void OnLogMessage(const std::string &message) override {
        lines.push_back(message);
    }
};

std::vector<uint8_t> makeRtpPacket(uint16_t seq) {
    // 12-byte RTP header (version 2, PT 111) + payload; >= 16 bytes so that
    // dcsctp's parser gets past its size check and would reach the checksum.
    return {0x80, 0x6f, (uint8_t)(seq >> 8), (uint8_t)seq, 0, 0, 0, 1, 0x11, 0x22, 0x33, 0x44, 'a', 'u', 'd', 'i', 'o'};
}

// A sender/receiver pair over one key, as a real call has.
struct Pair {
    std::shared_ptr<std::array<uint8_t, 256>> key = makeSharedKeyMaterial();
    FakeIceTransport senderIce;
    FakeIceTransport receiverIce;
    tgcalls::MtProtoDtlsTransport sender{wrap(&senderIce), outgoingKey(key)};
    tgcalls::MtProtoDtlsTransport receiver{wrap(&receiverIce), incomingKey(key)};

    // Encrypts on the sender, delivers the wire bytes to the receiver's ICE.
    void send(const std::string &payload, int flags) {
        webrtc::AsyncSocketPacketOptions options;
        sender.SendPacket(payload.data(), payload.size(), options, flags);
        const std::string onTheWire = senderIce.lastSentPacket();
        receiverIce.deliverPacket(onTheWire.data(), onTheWire.size());
    }
};

// ---------------------------------------------------------------------------
// DTLS-slot contract: the controller and the stats wrapper must see a
// transport that becomes connected without any handshake.
// ---------------------------------------------------------------------------

void TestStateBecomesConnectedWhenIceWritable() {
    FakeIceTransport ice;
    tgcalls::MtProtoDtlsTransport transport(wrap(&ice), outgoingKey(makeSharedKeyMaterial()));

    std::vector<webrtc::DtlsTransportState> published;
    transport.SubscribeDtlsTransportState([&published](webrtc::DtlsTransportInternal *, webrtc::DtlsTransportState state) {
        published.push_back(state);
    });

    CHECK_TRUE(transport.dtls_state() == webrtc::DtlsTransportState::kNew);
    CHECK_TRUE(!transport.writable());

    ice.setWritable(true);
    CHECK_TRUE(transport.writable());
    CHECK_TRUE(transport.dtls_state() == webrtc::DtlsTransportState::kConnected);
    CHECK_TRUE(published.size() == 1);
    CHECK_TRUE(!published.empty() && published[0] == webrtc::DtlsTransportState::kConnected);

    // Losing ICE writability is an ICE matter; the DTLS state stays connected
    // (stock DTLS behaves the same: writable() drops, dtls_state() does not).
    ice.setWritable(false);
    CHECK_TRUE(!transport.writable());
    CHECK_TRUE(transport.dtls_state() == webrtc::DtlsTransportState::kConnected);
    CHECK_TRUE(published.size() == 1);
}

void TestNegotiationSurfaceRoundTrips() {
    FakeIceTransport ice;
    tgcalls::MtProtoDtlsTransport transport(wrap(&ice), outgoingKey(makeSharedKeyMaterial()));

    CHECK_TRUE(transport.IsDtlsActive());
    CHECK_TRUE(transport.ice_transport() == &ice);
    CHECK_TRUE(transport.component() == 1);

    auto certificate = webrtc::RTCCertificate::Create(webrtc::SSLIdentity::Create("test", webrtc::KT_DEFAULT));
    CHECK_TRUE(transport.SetLocalCertificate(certificate));

    webrtc::SSLRole role;
    CHECK_TRUE(!transport.GetDtlsRole(&role));
    const uint8_t digest[32] = {1, 2, 3};
    CHECK_TRUE(transport.SetRemoteParameters("sha-256", digest, sizeof(digest), webrtc::SSL_SERVER).ok());
    CHECK_TRUE(transport.GetDtlsRole(&role));
    CHECK_TRUE(role == webrtc::SSL_SERVER);

    int value = 0;
    CHECK_TRUE(!transport.GetSrtpCryptoSuite(&value));
    CHECK_TRUE(!transport.GetSslCipherSuite(&value));
    CHECK_TRUE(!transport.GetSslVersionBytes(&value));
    CHECK_TRUE(transport.GetSslPeerSignatureAlgorithm() == 0);
    CHECK_TRUE(transport.GetRemoteSSLCertChain() == nullptr);
    CHECK_TRUE(transport.GetSslGroupId() == 0);
    CHECK_TRUE(!transport.GetTlsCipherSuiteName().has_value());
    webrtc::ZeroOnFreeBuffer<uint8_t> material;
    CHECK_TRUE(!transport.AppendSrtpKeyingMaterial(material));
    CHECK_TRUE(material.size() == 0);
}

void TestFactoryCreatesTransportOverGivenIce() {
    FakeIceTransport ice;
    tgcalls::MtProtoDtlsTransportFactory factory(outgoingKey(makeSharedKeyMaterial()));
    auto transport = factory.CreateDtlsTransport(wrap(&ice), webrtc::CryptoOptions(), webrtc::SSL_PROTOCOL_DTLS_12);
    CHECK_TRUE(transport != nullptr);
    CHECK_TRUE(transport && transport->ice_transport() == &ice);
}

// ---------------------------------------------------------------------------
// Framing: `flags` is ground truth on send, as in 13.0.0; on receive the framing decides the packet's DecryptionInfo.
// ---------------------------------------------------------------------------

void TestSendPacketIsEncrypted() {
    FakeIceTransport ice;
    tgcalls::MtProtoDtlsTransport transport(wrap(&ice), outgoingKey(makeSharedKeyMaterial()));
    const std::string payload = "PLAINTEXTPAYLOAD";
    webrtc::AsyncSocketPacketOptions options;
    transport.SendPacket(payload.data(), payload.size(), options, webrtc::PF_SRTP_BYPASS);
    CHECK_TRUE(ice.sentPacketCount() == 1);
    CHECK_TRUE(ice.lastSentPacket().find(payload) == std::string::npos);
}

// PacketTransportInternal contract: report the CALLER's byte count.
// RtpTransport::SendPacket treats any other value as a failed send.
void TestSendPacketReturnsPlaintextLength() {
    FakeIceTransport ice;
    tgcalls::MtProtoDtlsTransport transport(wrap(&ice), outgoingKey(makeSharedKeyMaterial()));
    const std::string payload = "PLAINTEXTPAYLOAD";
    webrtc::AsyncSocketPacketOptions options;
    const int ret = transport.SendPacket(payload.data(), payload.size(), options, webrtc::PF_SRTP_BYPASS);
    CHECK_TRUE(ret == (int)payload.size());
}

void TestReadPacketFlagsFollowFraming() {
    Pair pair;
    ReadPacketProbe probe;
    probe.attach(&pair.receiver);

    // RTP path: PF_SRTP_BYPASS in, kSrtpEncrypted out (what the plain RtpTransport takes), payload intact.
    const std::string rtp = "RTPPAYLOAD";
    pair.send(rtp, webrtc::PF_SRTP_BYPASS);
    CHECK_TRUE(probe.count == 1);
    CHECK_TRUE(probe.lastPayload == rtp);
    CHECK_TRUE(probe.lastDecryption == webrtc::ReceivedIpPacket::kSrtpEncrypted);

    // SCTP path: 0 in, kDtlsDecrypted out (the only kind DcSctpTransport takes), the magic prefix added and
    // stripped on the way.
    const std::string sctp = std::string("\x13\x88\x13\x88\x01\x02\x03\x04\x00\x00\x00\x00", 12) + "CHUNK";
    pair.send(sctp, 0);
    CHECK_TRUE(probe.count == 2);
    CHECK_TRUE(probe.lastPayload == sctp);
    CHECK_TRUE(probe.lastDecryption == webrtc::ReceivedIpPacket::kDtlsDecrypted);
}

// The wire carries the 13.0.0 framing: an SCTP frame is prefixed 0xdcdcdcdc
// before encryption, an RTP frame is not. Decrypt on the receiver's connection
// and inspect the plaintext.
void TestWireFramingMatches13() {
    Pair pair;
    webrtc::AsyncSocketPacketOptions options;

    const std::string sctp = "SCTPBYTES";
    pair.sender.SendPacket(sctp.data(), sctp.size(), options, 0);
    tgcalls::EncryptedConnection decryptor(tgcalls::EncryptedConnection::Type::Transport, incomingKey(pair.key), [](int, int) {});
    const std::string sctpWire = pair.senderIce.lastSentPacket();
    const auto sctpPlain = decryptor.handleIncomingRawPacket(sctpWire.data(), sctpWire.size());
    CHECK_TRUE(sctpPlain.has_value());
    if (sctpPlain) {
        const auto &m = sctpPlain->main.message;
        CHECK_TRUE(m.size() == 4 + sctp.size());
        uint32_t magic = 0;
        memcpy(&magic, m.data(), 4);
        CHECK_TRUE(magic == 0xdcdcdcdc);
    }

    const std::string rtp = "RTPBYTES";
    pair.sender.SendPacket(rtp.data(), rtp.size(), options, webrtc::PF_SRTP_BYPASS);
    const std::string rtpWire = pair.senderIce.lastSentPacket();
    const auto rtpPlain = decryptor.handleIncomingRawPacket(rtpWire.data(), rtpWire.size());
    CHECK_TRUE(rtpPlain.has_value());
    if (rtpPlain) {
        const auto &m = rtpPlain->main.message;
        CHECK_TRUE(m.size() == rtp.size());
        CHECK_TRUE(std::string((const char *)m.data(), m.size()) == rtp);
    }
}


// P2PTransportChannel::SendPacket rejects any non-zero flags with EINVAL, so
// whatever the caller passed must be consumed by the framing and never forwarded.
void TestFlagsNeverReachIce() {
    FakeIceTransport ice;
    tgcalls::MtProtoDtlsTransport transport(wrap(&ice), outgoingKey(makeSharedKeyMaterial()));
    webrtc::AsyncSocketPacketOptions options;
    const std::string payload = "PAYLOAD";
    transport.SendPacket(payload.data(), payload.size(), options, webrtc::PF_SRTP_BYPASS);
    CHECK_TRUE(ice.lastSentFlags() == 0);
    transport.SendPacket(payload.data(), payload.size(), options, 0);
    CHECK_TRUE(ice.lastSentFlags() == 0);
}

// A failed ICE send is reported as the ICE channel reports it, so RtpTransport's
// ENOTCONN handling and dcsctp's blocking-error handling both keep working.
void TestIceSendFailurePropagates() {
    FakeIceTransport ice;
    tgcalls::MtProtoDtlsTransport transport(wrap(&ice), outgoingKey(makeSharedKeyMaterial()));
    webrtc::AsyncSocketPacketOptions options;
    const std::string payload = "PAYLOAD";

    ice.setSendResult(-1);
    ice.setError(ENOTCONN);
    CHECK_TRUE(transport.SendPacket(payload.data(), payload.size(), options, webrtc::PF_SRTP_BYPASS) == -1);
    CHECK_TRUE(transport.GetError() == ENOTCONN);

    ice.setSendResult(FakeIceTransport::kFakeSendReturnsLength);
    ice.setError(0);
    CHECK_TRUE(transport.SendPacket(payload.data(), payload.size(), options, webrtc::PF_SRTP_BYPASS) == (int)payload.size());
    CHECK_TRUE(transport.GetError() == 0);
}

// A framing/encryption failure is OURS and must not surface the ICE channel's
// stale error: RtpTransport reads ENOTCONN there as a lost connection.
void TestEncryptionFailureReportsOwnError() {
    FakeIceTransport ice;
    tgcalls::MtProtoDtlsTransport transport(wrap(&ice), outgoingKey(makeSharedKeyMaterial()));
    webrtc::AsyncSocketPacketOptions options;

    ice.setError(ENOTCONN); // stale, from some earlier failed send
    const std::string huge(200 * 1024, 'x'); // over EncryptedConnection's outer packet limit
    CHECK_TRUE(transport.SendPacket(huge.data(), huge.size(), options, webrtc::PF_SRTP_BYPASS) == -1);
    CHECK_TRUE(ice.sentPacketCount() == 0);
    CHECK_TRUE(transport.GetError() == EMSGSIZE);

    // A later successful send clears it and ICE's error becomes visible again.
    const std::string payload = "PAYLOAD";
    CHECK_TRUE(transport.SendPacket(payload.data(), payload.size(), options, webrtc::PF_SRTP_BYPASS) == (int)payload.size());
    CHECK_TRUE(transport.GetError() == ENOTCONN);
}

// EncryptedConnection may pack a resend behind a fresh message ("additional"
// messages). Each surfaces separately, each with the label its own framing says.
void TestAdditionalMessagesSurfaceWithTheirOwnFraming() {
    auto key = makeSharedKeyMaterial();
    FakeIceTransport receiverIce;
    tgcalls::MtProtoDtlsTransport receiver(wrap(&receiverIce), incomingKey(key));
    std::vector<std::pair<int, std::string>> seen;
    receiver.RegisterReceivedPacketCallback(&seen, [&seen](webrtc::PacketTransportInternal *, const webrtc::ReceivedIpPacket &packet) {
        seen.emplace_back(packet.decryption_info(), std::string((const char *)packet.payload().data(), packet.payload().size()));
    });

    // A bare sender connection standing in for a peer whose first message needed
    // an ack that never came: after the resend delay it rides along with the next.
    tgcalls::EncryptedConnection sender(tgcalls::EncryptedConnection::Type::Transport, outgoingKey(key), [](int, int) {});
    const std::string sctpFrame = std::string("\xdc\xdc\xdc\xdc", 4) + "SCTPA";
    webrtc::CopyOnWriteBuffer a(std::span<const uint8_t>((const uint8_t *)sctpFrame.data(), sctpFrame.size()));
    const auto first = sender.prepareForSendingRawMessage(a, /*messageRequiresAck=*/true);
    CHECK_TRUE(first.has_value()); // dropped on purpose: never delivered
    std::this_thread::sleep_for(std::chrono::milliseconds(350)); // > minDelayBeforeMessageResend (300 ms, Transport)
    const std::string rtpFrame = "RTPB";
    webrtc::CopyOnWriteBuffer b(std::span<const uint8_t>((const uint8_t *)rtpFrame.data(), rtpFrame.size()));
    const auto second = sender.prepareForSendingRawMessage(b, /*messageRequiresAck=*/false);
    CHECK_TRUE(second.has_value());
    if (second) {
        receiverIce.deliverPacket((const char *)second->bytes.data(), second->bytes.size());
    }

    CHECK_TRUE(seen.size() == 2);
    if (seen.size() == 2) {
        CHECK_TRUE(seen[0].first == webrtc::ReceivedIpPacket::kSrtpEncrypted);
        CHECK_TRUE(seen[0].second == rtpFrame);
        CHECK_TRUE(seen[1].first == webrtc::ReceivedIpPacket::kDtlsDecrypted);
        CHECK_TRUE(seen[1].second == "SCTPA");
    }
}

// A decrypted frame too short to carry the prefix surfaces as RTP-class; the
// RtpTransport above then drops it on its own size check.
void TestShortFrameSurfacesWithoutPrefixCheck() {
    Pair pair;
    ReadPacketProbe probe;
    probe.attach(&pair.receiver);
    pair.send("abc", webrtc::PF_SRTP_BYPASS);
    CHECK_TRUE(probe.count == 1);
    CHECK_TRUE(probe.lastPayload == "abc");
    CHECK_TRUE(probe.lastDecryption == webrtc::ReceivedIpPacket::kSrtpEncrypted);
}

// ---------------------------------------------------------------------------
// The production stack above the transport: webrtc::RtpTransport and
// webrtc::DcSctpTransport, exactly as JsepTransportController wires them.
// ---------------------------------------------------------------------------

struct ReadyToSendProbe {
    int downFlips = 0;
};

// P2PTransportChannel::error_ is written only on a failed send and never
// cleared. After one genuine ENOTCONN, a send that SUCCEEDS but returned the
// wrong length would make RtpTransport read that stale ENOTCONN and drop
// ready-to-send -> Call network DOWN -> pacer paused.
void TestStaleEnotconnMustNotDropReadyToSendAfterSuccessfulSend() {
    webrtc::AutoThread thread;
    const webrtc::Environment env = webrtc::CreateEnvironment();
    FakeIceTransport ice;
    tgcalls::MtProtoDtlsTransport transport(wrap(&ice), outgoingKey(makeSharedKeyMaterial()));

    webrtc::RtpTransport rtpTransport(/*rtcp_mux_enabled=*/true, env.field_trials());
    rtpTransport.SetRtpPacketTransport(&transport);

    ReadyToSendProbe probe;
    rtpTransport.SubscribeReadyToSend(&probe, [&probe](bool ready) {
        if (!ready) {
            probe.downFlips++;
        }
    });

    ice.setWritable(true);
    ice.fireReadyToSend();
    CHECK_TRUE(rtpTransport.IsReadyToSend());

    ice.setError(ENOTCONN);

    const auto rtp = makeRtpPacket(1);
    webrtc::CopyOnWriteBuffer packet(std::span<const uint8_t>(rtp.data(), rtp.size()));
    webrtc::AsyncSocketPacketOptions options;
    // BaseChannel::SendPacket always passes PF_SRTP_BYPASS for RTP and RTCP.
    const bool sent = rtpTransport.SendRtpPacket(&packet, options, webrtc::PF_SRTP_BYPASS);

    CHECK_TRUE(ice.sentPacketCount() == 1);
    CHECK_TRUE(sent);
    CHECK_TRUE(rtpTransport.IsReadyToSend());
    CHECK_TRUE(probe.downFlips == 0);

    rtpTransport.UnsubscribeReadyToSend(&probe);
}

// DcSctpTransport registers for the same packets as RtpTransport and takes only
// kDtlsDecrypted ones. RTP must therefore surface as kSrtpEncrypted, or every
// media packet is parsed (copy + CRC32c) and logged as an error.
void TestReceivedRtpMustNotReachSctp() {
    webrtc::AutoThread thread;
    const webrtc::Environment env = webrtc::CreateEnvironment();
    Pair pair;

    webrtc::RtpTransport rtpTransport(/*rtcp_mux_enabled=*/true, env.field_trials());
    rtpTransport.SetRtpPacketTransport(&pair.receiver);
    webrtc::DcSctpTransport sctp(env, &thread, &pair.receiver);
    webrtc::SctpOptions sctpOptions;
    sctpOptions.local_port = 5000;
    sctpOptions.remote_port = 5000;
    sctpOptions.max_message_size = 256 * 1024;
    CHECK_TRUE(sctp.Start(sctpOptions));

    ReadPacketProbe transportProbe;
    transportProbe.attach(&pair.receiver);

    CapturingLogSink sink;
    webrtc::LogMessage::AddLogToStream(&sink, webrtc::LS_ERROR);

    const int kPackets = 50;
    for (int i = 0; i < kPackets; i++) {
        const auto rtp = makeRtpPacket((uint16_t)i);
        pair.send(std::string((const char *)rtp.data(), rtp.size()), webrtc::PF_SRTP_BYPASS);
    }

    webrtc::LogMessage::RemoveLogToStream(&sink);

    int parseFailures = 0;
    for (const auto &line : sink.lines) {
        if (line.find("PARSE_FAILED") != std::string::npos) {
            parseFailures++;
        }
    }
    CHECK_TRUE(transportProbe.count == kPackets);
    CHECK_TRUE(transportProbe.lastDecryption == webrtc::ReceivedIpPacket::kSrtpEncrypted);
    CHECK_TRUE(parseFailures == 0);
}

} // namespace

int main() {
    TestStateBecomesConnectedWhenIceWritable();
    TestNegotiationSurfaceRoundTrips();
    TestFactoryCreatesTransportOverGivenIce();

    TestSendPacketIsEncrypted();
    TestSendPacketReturnsPlaintextLength();
    TestReadPacketFlagsFollowFraming();
    TestWireFramingMatches13();
    TestFlagsNeverReachIce();
    TestIceSendFailurePropagates();
    TestEncryptionFailureReportsOwnError();
    TestAdditionalMessagesSurfaceWithTheirOwnFraming();
    TestShortFrameSurfacesWithoutPrefixCheck();

    TestStaleEnotconnMustNotDropReadyToSendAfterSuccessfulSend();
    TestReceivedRtpMustNotReachSctp();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
