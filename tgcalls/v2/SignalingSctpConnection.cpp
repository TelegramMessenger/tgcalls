#include "v2/SignalingSctpConnection.h"

#include <random>

#include "rtc_base/async_tcp_socket.h"
#include "p2p/base/basic_packet_socket_factory.h"
#include "rtc_base/logging.h"
#include "p2p/base/packet_transport_internal.h"
#include "media/sctp/sctp_transport_factory.h"

#include "FieldTrialsConfig.h"

namespace tgcalls {

class SignalingPacketTransport : public rtc::PacketTransportInternal {
public:
    SignalingPacketTransport(std::shared_ptr<Threads> threads, std::function<void(const std::vector<uint8_t> &)> emitData) :
    _threads(threads),
    _emitData(emitData),
    _transportName("signaling") {
    }

    virtual ~SignalingPacketTransport() {
    }

    void receiveData(std::vector<uint8_t> const &data) {
        SignalReadPacket.emit(this, (const char *)data.data(), data.size(), -1, 0);
    }

    virtual const std::string& transport_name() const override {
        return _transportName;
    }

    virtual bool writable() const override {
        return true;
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
    std::string _transportName;
};

SignalingSctpConnection::SignalingSctpConnection(std::shared_ptr<Threads> threads, std::function<void(const std::vector<uint8_t> &)> onIncomingData, std::function<void(const std::vector<uint8_t> &)> emitData) :
_threads(threads),
_emitData(emitData),
_onIncomingData(onIncomingData) {
    _threads->getNetworkThread()->Invoke<void>(RTC_FROM_HERE, [&]() {
        _packetTransport = std::make_unique<SignalingPacketTransport>(threads, emitData);

        _sctpTransportFactory = std::make_unique<cricket::SctpTransportFactory>(_threads->getNetworkThread(), fieldTrialsBasedConfig);

        _sctpTransport = _sctpTransportFactory->CreateSctpTransport(_packetTransport.get());
        _sctpTransport->SignalReadyToSendData.connect(this, &SignalingSctpConnection::sctpReadyToSendData);
        _sctpTransport->SignalDataReceived.connect(this, &SignalingSctpConnection::sctpDataReceived);
        _sctpTransport->SignalClosedAbruptly.connect(this, &SignalingSctpConnection::sctpClosedAbruptly);

        _sctpTransport->Start(5000, 5000, 262144);
    });
}

void SignalingSctpConnection::sctpReadyToSendData() {
    assert(_threads->getNetworkThread()->IsCurrent());
}

void SignalingSctpConnection::sctpClosedAbruptly(webrtc::RTCError error) {
    assert(_threads->getNetworkThread()->IsCurrent());
}

void SignalingSctpConnection::sctpDataReceived(const cricket::ReceiveDataParams& params, const rtc::CopyOnWriteBuffer& buffer) {
    assert(_threads->getNetworkThread()->IsCurrent());

    _onIncomingData(std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size()));
}

SignalingSctpConnection::~SignalingSctpConnection() {
    _threads->getNetworkThread()->Invoke<void>(RTC_FROM_HERE, [&]() {
        _sctpTransport.reset();
        _sctpTransportFactory.reset();
        _packetTransport.reset();
    });
}

void SignalingSctpConnection::start() {
}

void SignalingSctpConnection::receiveExternal(const std::vector<uint8_t> &data) {
    _threads->getNetworkThread()->Invoke<void>(RTC_FROM_HERE, [&]() {
        _packetTransport->receiveData(data);
    });
}

void SignalingSctpConnection::send(const std::vector<uint8_t> &data) {
    _threads->getNetworkThread()->Invoke<void>(RTC_FROM_HERE, [&]() {
        webrtc::SendDataParams params;
        params.type = webrtc::DataMessageType::kBinary;
        params.ordered = true;

        rtc::CopyOnWriteBuffer payload;
        payload.AppendData(data.data(), data.size());

        cricket::SendDataResult result;
        _sctpTransport->SendData(0, params, payload, &result);
    });
}

}
