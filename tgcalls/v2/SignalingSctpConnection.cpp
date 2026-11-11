#include "v2/SignalingSctpConnection.h"

#include <algorithm>
#include <iterator>
#include <random>

#include "rtc_base/async_tcp_socket.h"
#include "p2p/base/basic_packet_socket_factory.h"
#include "rtc_base/logging.h"
#include "p2p/base/packet_transport_internal.h"
#include "media/sctp/sctp_transport_factory.h"
#include "media/sctp/dcsctp_transport.h"
#include "v2/CustomDcSctpSocket.h"
#include "net/dcsctp/public/dcsctp_socket_factory.h"
#include "system_wrappers/include/clock.h"

#include "FieldTrialsConfig.h"
#include "v2/CustomParameters.h"

namespace tgcalls {

// RFC 4960's RTO.Max. A longer handshake timer is never meaningful, and a
// seconds-for-milliseconds slip (1500000) would hold the first INIT
// retransmission for 25 minutes; dcsctp caps a timer at one day.
static constexpr int kMaxSignalingSctpTimerMs = 60000;

static void applySignalingSctpTimerOverride(std::map<std::string, json11::Json> const &customParameters, std::string const &name, int &timerMs) {
    // A value outside 1...kMaxSignalingSctpTimerMs keeps the default: the
    // factory would run dcsctp's own 1000/1000/3000 ms for one <= 0.
    const int value = getCustomParameterInt(customParameters, name);
    if (value > 0 && value <= kMaxSignalingSctpTimerMs) {
        timerMs = value;
    }
}

SignalingSctpConnection::Options SignalingSctpConnection::Options::fromCustomParameters(std::map<std::string, json11::Json> const &customParameters) {
    Options options;
    applySignalingSctpTimerOverride(customParameters, "network_sctp_t1_init_ms", options.t1InitTimeoutMs);
    applySignalingSctpTimerOverride(customParameters, "network_sctp_t1_cookie_ms", options.t1CookieTimeoutMs);
    applySignalingSctpTimerOverride(customParameters, "network_sctp_max_backoff_ms", options.maxBackoffMs);
    options.sendBeforeConnected = getCustomParameterBool(customParameters, "network_sctp_send_before_connected");
    options.fastPath = getCustomParameterBool(customParameters, "network_sctp_fast_path");
    return options;
}

namespace signaling_fast_path {

namespace {

constexpr uint8_t kMagic[4] = {'T', 'G', 'F', 'P'};
constexpr uint32_t kChecksum = 0x54474650;
constexpr uint8_t kChunkType = 0xff;
constexpr uint8_t kVersion = 1;

void writeUint32(std::vector<uint8_t> &data, size_t offset, uint32_t value) {
    data[offset] = (uint8_t)(value >> 24);
    data[offset + 1] = (uint8_t)(value >> 16);
    data[offset + 2] = (uint8_t)(value >> 8);
    data[offset + 3] = (uint8_t)value;
}

uint32_t readUint32(std::vector<uint8_t> const &data, size_t offset) {
    return ((uint32_t)data[offset] << 24) | ((uint32_t)data[offset + 1] << 16) | ((uint32_t)data[offset + 2] << 8) | (uint32_t)data[offset + 3];
}

}  // namespace

std::vector<uint8_t> encode(uint32_t position, std::vector<uint8_t> const &message) {
    std::vector<uint8_t> packet(kHeaderSize + message.size(), 0);
    std::copy(kMagic, kMagic + 4, packet.begin() + 4);
    writeUint32(packet, 8, kChecksum);
    packet[12] = kChunkType;
    packet[13] = kVersion;
    writeUint32(packet, 16, position);
    std::copy(message.begin(), message.end(), packet.begin() + kHeaderSize);
    return packet;
}

absl::optional<Envelope> decode(std::vector<uint8_t> const &packet) {
    if (packet.size() < kHeaderSize) {
        return absl::nullopt;
    }
    if (packet[0] != 0 || packet[1] != 0 || packet[2] != 0 || packet[3] != 0) {
        return absl::nullopt;
    }
    if (!std::equal(kMagic, kMagic + 4, packet.begin() + 4)) {
        return absl::nullopt;
    }
    if (packet[12] != kChunkType || packet[13] != kVersion || packet[14] != 0 || packet[15] != 0) {
        return absl::nullopt;
    }
    Envelope envelope;
    envelope.position = readUint32(packet, 16);
    envelope.message.assign(packet.begin() + kHeaderSize, packet.end());
    return envelope;
}

std::vector<std::vector<uint8_t>> Receiver::onEnvelope(Envelope &&envelope) {
    std::vector<std::vector<uint8_t>> out;
    if (_isBroken || envelope.position >= kMaxPosition || envelope.position < _nextPosition) {
        return out;
    }
    // A repeated envelope keeps the first copy.
    _held.emplace(envelope.position, std::move(envelope.message));
    deliverHeld(out);
    return out;
}

std::vector<std::vector<uint8_t>> Receiver::onSctpMessage(std::vector<uint8_t> &&message) {
    std::vector<std::vector<uint8_t>> out;
    if (_isBroken) {
        out.push_back(std::move(message));
        return out;
    }
    // Every SCTP message either fills _nextPosition or repeats a position an
    // envelope filled, so position <= _nextPosition.
    const uint64_t position = _sctpMessageCount++;
    if (position < _nextPosition) {
        const auto delivered = _deliveredFromEnvelopes.find(position);
        if (delivered != _deliveredFromEnvelopes.end() && delivered->second == message) {
            _deliveredFromEnvelopes.erase(delivered);
            return out;
        }
    }
    if (position != _nextPosition) {
        _isBroken = true;
        _held.clear();
        _deliveredFromEnvelopes.clear();
        out.push_back(std::move(message));
        return out;
    }
    out.push_back(std::move(message));
    _nextPosition++;
    deliverHeld(out);
    return out;
}

void Receiver::deliverHeld(std::vector<std::vector<uint8_t>> &out) {
    while (true) {
        const auto held = _held.find(_nextPosition);
        if (held == _held.end()) {
            break;
        }
        out.push_back(held->second);
        _deliveredFromEnvelopes.emplace(_nextPosition, std::move(held->second));
        _held.erase(held);
        _nextPosition++;
    }
}

}  // namespace signaling_fast_path

// Factory that creates CustomDcSctpSocket (with the t1 timer backoff cap fix)
// instead of the stock DcSctpSocket, with optional timer overrides for the
// signaling channel.
class CustomDcSctpSocketFactory : public dcsctp::DcSctpSocketFactory {
public:
    explicit CustomDcSctpSocketFactory(SignalingSctpConnection::Options opts = {})
        : _opts(opts) {}

    std::unique_ptr<dcsctp::DcSctpSocketInterface> Create(
        absl::string_view log_prefix,
        dcsctp::DcSctpSocketCallbacks& callbacks,
        std::unique_ptr<dcsctp::PacketObserver> packet_observer,
        const dcsctp::DcSctpOptions& options) override {
        dcsctp::DcSctpOptions opts = options;
        if (_opts.t1InitTimeoutMs > 0) {
            opts.t1_init_timeout = dcsctp::DurationMs(_opts.t1InitTimeoutMs);
        }
        if (_opts.t1CookieTimeoutMs > 0) {
            opts.t1_cookie_timeout = dcsctp::DurationMs(_opts.t1CookieTimeoutMs);
        }
        if (_opts.maxBackoffMs > 0) {
            opts.max_timer_backoff_duration = dcsctp::DurationMs(_opts.maxBackoffMs);
        }
        return std::make_unique<dcsctp::CustomDcSctpSocket>(
            log_prefix, callbacks, std::move(packet_observer), opts);
    }
private:
    SignalingSctpConnection::Options _opts;
};

class SignalingPacketTransport : public rtc::PacketTransportInternal {
public:
    SignalingPacketTransport(std::shared_ptr<Threads> threads, std::function<void(const std::vector<uint8_t> &)> emitData, bool writable) :
    _threads(threads),
    _emitData(emitData),
    _writable(writable),
    _transportName("signaling") {
    }

    virtual ~SignalingPacketTransport() {
    }

    void receiveData(std::vector<uint8_t> const &data) {
        RTC_LOG(LS_INFO) << "SignalingPacketTransport: adding data of " << data.size() << " bytes";
        SignalReadPacket.emit(this, (const char *)data.data(), data.size(), -1, 0);
    }

    void setWritable(bool writable) {
        if (_writable != writable) {
            _writable = writable;
            SignalWritableState.emit(this);
        }
    }

    virtual const std::string& transport_name() const override {
        return _transportName;
    }

    virtual bool writable() const override {
        return _writable;
    }

    virtual bool receiving() const override {
        return false;
    }

    // Attempts to send the given packet.
    // The return value is < 0 on failure. The return value in failure case is not
    // descriptive. Depending on failure cause and implementation details
    // GetError() returns an descriptive errno.h error value.
    // This mimics posix socket send() or sendto() behavior.
    // TODO(johan): Reliable, meaningful, consistent error codes for all
    // implementations would be nice.
    // TODO(johan): Remove the default argument once channel code is updated.
    virtual int SendPacket(const char* data,
                           size_t len,
                           const rtc::PacketOptions& options,
                           int flags = 0) override {
        _emitData(std::vector<uint8_t>(data, data + len));

        rtc::SentPacket sentPacket;
        sentPacket.packet_id = options.packet_id;
        SignalSentPacket.emit(this, sentPacket);

        return (int)len;
    }

    virtual int SetOption(rtc::Socket::Option opt, int value) override {
        return 0;
    }

    virtual bool GetOption(rtc::Socket::Option opt, int* value) override {
        return 0;
    }

    virtual int GetError() override {
        return 0;
    }

    virtual absl::optional<rtc::NetworkRoute> network_route() const override {
        return absl::nullopt;
    }

private:
    std::shared_ptr<Threads> _threads;
    std::function<void(const std::vector<uint8_t> &)> _onIncomingData;
    std::function<void(const std::vector<uint8_t> &)> _emitData;
    bool _writable;
    std::string _transportName;
};

SignalingSctpConnection::SignalingSctpConnection(std::shared_ptr<Threads> threads, std::function<void(const std::vector<uint8_t> &)> onIncomingData, std::function<void(const std::vector<uint8_t> &)> emitData, bool isInitiator, Options options) :
_threads(threads),
_emitData(emitData),
_onIncomingData(onIncomingData),
_isInitiator(isInitiator),
_fastPath(options.fastPath) {
    RTC_LOG(LS_INFO) << "SignalingSctpConnection: " << (isInitiator ? "initiator" : "responder") << ", t1_init " << options.t1InitTimeoutMs << " ms, t1_cookie " << options.t1CookieTimeoutMs << " ms, max backoff " << options.maxBackoffMs << " ms, send before connected " << (options.sendBeforeConnected ? 1 : 0) << ", fast path " << (options.fastPath ? 1 : 0);

    // dcsctp queues a message it is given before the association is up and
    // sends it in the packet that carries the COOKIE ECHO (RFC 9260 section
    // 5.1), which every receiver must accept and dcsctp does: the caller's
    // first messages (11.0.0's offer; 13.0.0's NegotiateChannels offer and
    // InitialSetup) reach the peer one signaling round trip earlier. Before the COOKIE ACK dcsctp sends that
    // one packet only, so data that does not fit beside the cookie still waits
    // for the COOKIE ACK, as all of it did before.
    _isReadyToSend = options.sendBeforeConnected;

    _threads->getNetworkThread()->BlockingCall([&]() {
        _packetTransport = std::make_unique<SignalingPacketTransport>(threads, emitData, /*writable=*/isInitiator);

        // Use DcSctpTransport directly with CustomDcSctpSocketFactory so the
        // signaling SCTP gets the t1 timer backoff cap fix (the stock
        // DcSctpSocket doesn't wire max_timer_backoff_duration to t1 timers).
        _sctpTransport = std::make_unique<webrtc::DcSctpTransport>(
            _threads->getNetworkThread(),
            _packetTransport.get(),
            webrtc::Clock::GetRealTimeClock(),
            std::make_unique<CustomDcSctpSocketFactory>(options));
        _sctpTransport->OpenStream(0);
        _sctpTransport->SetDataChannelSink(this);

        _sctpTransport->Start(5000, 5000, 262144);
    });
}

void SignalingSctpConnection::OnReadyToSend() {
    assert(_threads->getNetworkThread()->IsCurrent());

    _isReadyToSend = true;
    _isAssociationUp = true;
    _sentWithoutEnvelope.clear();

    auto pendingData = std::move(_pendingData);
    _pendingData.clear();

    for (size_t i = 0; i < pendingData.size(); i++) {
        const auto &data = pendingData[i];

        webrtc::SendDataParams params;
        params.type = webrtc::DataMessageType::kBinary;
        params.ordered = true;

        rtc::CopyOnWriteBuffer payload;
        payload.AppendData(data.data(), data.size());

        webrtc::RTCError sendError = _sctpTransport->SendData(0, params, payload);

        if (sendError.ok()) {
            RTC_LOG(LS_INFO) << "SignalingSctpConnection: sent data of " << data.size() << " bytes";
        } else {
            // Keep this message and every one after it, in order: a message
            // missing from the SCTP stream would shift the position of every
            // later one, which the peer's fast-path receiver relies on.
            _isReadyToSend = false;
            _pendingData.insert(_pendingData.end(), std::make_move_iterator(pendingData.begin() + i), std::make_move_iterator(pendingData.end()));
            RTC_LOG(LS_WARNING) << "SignalingSctpConnection: OnReadyToSend flush error '" << sendError.message() << "' (type=" << ToString(sendError.type()) << "), storing data until ready to send (" << _pendingData.size() << " items)";
            break;
        }
    }
}

void SignalingSctpConnection::deliverIncoming(std::vector<std::vector<uint8_t>> &&messages) {
    for (const auto &message : messages) {
        _onIncomingData(message);
    }
}

void SignalingSctpConnection::OnTransportClosed(webrtc::RTCError error) {
    assert(_threads->getNetworkThread()->IsCurrent());
}

void SignalingSctpConnection::OnDataReceived(int channel_id, webrtc::DataMessageType type, const rtc::CopyOnWriteBuffer& buffer) {
    assert(_threads->getNetworkThread()->IsCurrent());

    std::vector<uint8_t> message(buffer.data(), buffer.data() + buffer.size());
    if (!_fastPath) {
        _onIncomingData(message);
        return;
    }
    const bool wasBroken = _fastPathReceiver.isBroken();
    auto messages = _fastPathReceiver.onSctpMessage(std::move(message));
    if (!wasBroken && _fastPathReceiver.isBroken()) {
        RTC_LOG(LS_ERROR) << "SignalingSctpConnection: an SCTP message does not match the fast-path message at its position; delivering SCTP only from now on";
    }
    deliverIncoming(std::move(messages));
}

SignalingSctpConnection::~SignalingSctpConnection() {
    _threads->getNetworkThread()->BlockingCall([&]() {
        _sctpTransport.reset();
        _packetTransport.reset();
    });
}

void SignalingSctpConnection::start() {
}

void SignalingSctpConnection::receiveExternal(const std::vector<uint8_t> &data) {
    _threads->getNetworkThread()->BlockingCall([&]() {
        // An envelope is the peer's first packet as much as an INIT is: the
        // callee starts its own handshake on either.
        _packetTransport->setWritable(true);
        if (_fastPath) {
            if (auto envelope = signaling_fast_path::decode(data)) {
                if (!_peerSendsFastPath) {
                    _peerSendsFastPath = true;
                    RTC_LOG(LS_INFO) << "SignalingSctpConnection: the peer sends on the fast path";
                    if (!_isAssociationUp) {
                        for (const auto &sent : _sentWithoutEnvelope) {
                            _emitData(signaling_fast_path::encode(sent.position, sent.message));
                            RTC_LOG(LS_INFO) << "SignalingSctpConnection: sent earlier message " << sent.position << " (" << sent.message.size() << " bytes) on the fast path";
                        }
                    }
                    _sentWithoutEnvelope.clear();
                }
                RTC_LOG(LS_INFO) << "SignalingSctpConnection: received message " << envelope->position << " (" << envelope->message.size() << " bytes) on the fast path";
                deliverIncoming(_fastPathReceiver.onEnvelope(std::move(*envelope)));
                return;
            }
        }
        _packetTransport->receiveData(data);
    });
}

void SignalingSctpConnection::send(const std::vector<uint8_t> &data) {
    _threads->getNetworkThread()->BlockingCall([&]() {
        const uint32_t position = _nextSendPosition++;
        if (_fastPath && !_isAssociationUp && position < signaling_fast_path::kMaxPosition) {
            if (_isInitiator || _peerSendsFastPath) {
                _emitData(signaling_fast_path::encode(position, data));
                RTC_LOG(LS_INFO) << "SignalingSctpConnection: sent message " << position << " (" << data.size() << " bytes) on the fast path";
            } else {
                signaling_fast_path::Envelope sent;
                sent.position = position;
                sent.message = data;
                _sentWithoutEnvelope.push_back(std::move(sent));
            }
        }

        if (_isReadyToSend) {
            webrtc::SendDataParams params;
            params.type = webrtc::DataMessageType::kBinary;
            params.ordered = true;
            
            rtc::CopyOnWriteBuffer payload;
            payload.AppendData(data.data(), data.size());
            
            webrtc::RTCError sendError = _sctpTransport->SendData(0, params, payload);
            
            if (!sendError.ok()) {
                _isReadyToSend = false;
                _pendingData.push_back(data);
                RTC_LOG(LS_WARNING) << "SignalingSctpConnection: send error '" << sendError.message() << "' (type=" << ToString(sendError.type()) << "), storing data until ready to send (" << _pendingData.size() << " items)";
            } else if (!_isAssociationUp) {
                RTC_LOG(LS_INFO) << "SignalingSctpConnection: sent data of " << data.size() << " bytes before the association is up";
            } else {
                RTC_LOG(LS_INFO) << "SignalingSctpConnection: sent data of " << data.size() << " bytes";
            }
        } else {
            _pendingData.push_back(data);
            RTC_LOG(LS_INFO) << "SignalingSctpConnection: not ready to send, storing data until ready to send (" << _pendingData.size() << " items)";
        }
    });
}

}
