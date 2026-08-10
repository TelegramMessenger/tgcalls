#ifndef TGCALLS_SIGNALING_SCTP_CONNECTION_H_
#define TGCALLS_SIGNALING_SCTP_CONNECTION_H_

#ifdef WEBRTC_WIN
#include <cstdint>
#include <WinSock2.h>
#endif // WEBRTC_WIN

#include "rtc_base/third_party/sigslot/sigslot.h"
#include "rtc_base/socket_address.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/byte_buffer.h"
#include "api/environment/environment.h"
#include "api/transport/data_channel_transport_interface.h"
#include "media/base/media_channel.h"

#include <vector>

#include <absl/types/optional.h>

#include "StaticThreads.h"
#include "SignalingConnection.h"

namespace webrtc {
class Socket;
}


namespace webrtc {
class SctpTransportFactory;
class SctpTransportInternal;
};

namespace tgcalls {

class SignalingPacketTransport;

class SignalingSctpConnection : public sigslot::has_slots<>, public SignalingConnection, public webrtc::DataChannelSink {
private:
    struct PacketReadState {
        webrtc::CopyOnWriteBuffer headerData;
        int remainingHeaderSize = 0;
        bool isHeaderCompleted = false;

        webrtc::CopyOnWriteBuffer data;
        int remainingDataSize = 0;
        bool isDataCompleted = false;
    };

public:
    struct Options {
        int t1InitTimeoutMs;   // 0 = use default
        int t1CookieTimeoutMs; // 0 = use default
        int maxBackoffMs;      // 0 = use default
        Options() : t1InitTimeoutMs(400), t1CookieTimeoutMs(400), maxBackoffMs(750) {}
    };

    SignalingSctpConnection(std::shared_ptr<Threads> threads, std::function<void(const std::vector<uint8_t> &)> onIncomingData, std::function<void(const std::vector<uint8_t> &)> emitData, bool isInitiator, Options options = Options());
    virtual ~SignalingSctpConnection();

    virtual void receiveExternal(const std::vector<uint8_t> &data) override;
    virtual void start() override;
    virtual void send(const std::vector<uint8_t> &data) override;

    virtual void OnDataReceived(int channel_id,
                                webrtc::DataMessageType type,
                                const webrtc::CopyOnWriteBuffer& buffer) override;
    virtual void OnTransportConnected() override;
    virtual void OnReadyToSend() override;
    virtual void OnTransportClosed(webrtc::RTCError error) override;

    // Unused
    virtual void OnChannelClosing(int channel_id) override{}
    virtual void OnChannelClosed(int channel_id) override{}
    virtual void OnBufferedAmountLow(int channel_id) override{}
    virtual void OnMaxMessageSize(int max_message_size) override{}

private:
    std::shared_ptr<Threads> _threads;
    std::function<void(const std::vector<uint8_t> &)> _emitData;
    std::function<void(const std::vector<uint8_t> &)> _onIncomingData;

    std::unique_ptr<SignalingPacketTransport> _packetTransport;

    webrtc::Environment _environment;
    std::unique_ptr<webrtc::SctpTransportFactory> _sctpTransportFactory;
    std::unique_ptr<TgcallDtlsTransportAdapter> _dtlsTransportAdapter;
    std::unique_ptr<webrtc::SctpTransportInternal> _sctpTransport;
    
    bool _isReadyToSend = false;
    std::vector<std::vector<uint8_t>> _pendingData;
};

}  // namespace tgcalls

#endif  // TGCALLS_SIGNALING_SCTP_CONNECTION_H_
