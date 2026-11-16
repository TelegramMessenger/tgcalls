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
#include "media/base/media_channel.h"

#include <map>
#include <string>
#include <vector>

#include <absl/types/optional.h>

#include "StaticThreads.h"
#include "SignalingConnection.h"
#include "third-party/json11.hpp"

namespace rtc {
class Socket;
}

namespace cricket {
class SctpTransportInternal;
};

namespace tgcalls {

class SignalingPacketTransport;

class SignalingSctpConnection : public sigslot::has_slots<>, public SignalingConnection, public webrtc::DataChannelSink {
private:
    struct PacketReadState {
        rtc::CopyOnWriteBuffer headerData;
        int remainingHeaderSize = 0;
        bool isHeaderCompleted = false;

        rtc::CopyOnWriteBuffer data;
        int remainingDataSize = 0;
        bool isDataCompleted = false;
    };

public:
    struct Options {
        int t1InitTimeoutMs;   // <= 0: dcsctp's own default (1000 ms)
        int t1CookieTimeoutMs; // <= 0: dcsctp's own default (1000 ms)
        int maxBackoffMs;      // <= 0: dcsctp's own default (3000 ms)
        Options() : t1InitTimeoutMs(400), t1CookieTimeoutMs(400), maxBackoffMs(750) {}

        // The defaults above with the server's overrides applied
        // (network_sctp_t1_init_ms, network_sctp_t1_cookie_ms,
        // network_sctp_max_backoff_ms). An absent key, or a value outside
        // 1...60000 ms, keeps the default. Every engine that carries its
        // signaling over SCTP builds its options here.
        static Options fromCustomParameters(std::map<std::string, json11::Json> const &customParameters);
    };

    // No default for `options`: a caller that omitted it would silently run the
    // defaults whatever the server sent, which is how 11.0.0 and 18/19 ignored
    // the overrides until 2026-10-27. Pass Options::fromCustomParameters(...).
    SignalingSctpConnection(std::shared_ptr<Threads> threads, std::function<void(const std::vector<uint8_t> &)> onIncomingData, std::function<void(const std::vector<uint8_t> &)> emitData, bool isInitiator, Options options);
    virtual ~SignalingSctpConnection();

    virtual void receiveExternal(const std::vector<uint8_t> &data) override;
    virtual void start() override;
    virtual void send(const std::vector<uint8_t> &data) override;

    virtual void OnDataReceived(int channel_id,
                                webrtc::DataMessageType type,
                                const rtc::CopyOnWriteBuffer& buffer) override;
    virtual void OnReadyToSend() override;
    virtual void OnTransportClosed(webrtc::RTCError error) override;

    // Unused
    virtual void OnChannelClosing(int channel_id) override{}
    virtual void OnChannelClosed(int channel_id) override{}

private:
    std::shared_ptr<Threads> _threads;
    std::function<void(const std::vector<uint8_t> &)> _emitData;
    std::function<void(const std::vector<uint8_t> &)> _onIncomingData;

    std::unique_ptr<SignalingPacketTransport> _packetTransport;
    std::unique_ptr<cricket::SctpTransportInternal> _sctpTransport;
    
    bool _isReadyToSend = false;
    std::vector<std::vector<uint8_t>> _pendingData;
};

}  // namespace tgcalls

#endif  // TGCALLS_SIGNALING_SCTP_CONNECTION_H_
