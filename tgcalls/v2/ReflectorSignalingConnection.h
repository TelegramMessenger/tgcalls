#ifndef TGCALLS_REFLECTOR_SIGNALING_CONNECTION_H_
#define TGCALLS_REFLECTOR_SIGNALING_CONNECTION_H_

#include "rtc_base/third_party/sigslot/sigslot.h"
#include "rtc_base/socket_address.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/byte_buffer.h"

#include <vector>

#include <absl/types/optional.h>

#include "StaticThreads.h"
#include "SignalingConnection.h"

namespace rtc {
class Socket;
}

namespace tgcalls {

class ReflectorSignalingConnection : public sigslot::has_slots<>, public SignalingConnection {
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
    ReflectorSignalingConnection(std::shared_ptr<Threads> threads, std::function<void(const std::vector<uint8_t> &)> onIncomingData, std::string const &ip, uint16_t port, std::string const &peerTag);
    virtual ~ReflectorSignalingConnection();

    virtual void start() override;
    virtual void send(const std::vector<uint8_t> &data) override;

private:
    void restartSocket();
    void cleanupSocket();
    void reconnect();
    void sendConnectionHeader();
    void sendDataToSocket(const std::vector<uint8_t> &data);
    bool consumeIncomingData(rtc::ByteBufferReader &reader);
    void processIncomingPacket(rtc::CopyOnWriteBuffer const &header, rtc::CopyOnWriteBuffer const &data);
    
    void onSocketConnect(rtc::Socket *socket);
    void onSocketClose(rtc::Socket *socket, int error);
    void onReadPacket(rtc::Socket *socket);
    
private:
    std::shared_ptr<Threads> _threads;
    rtc::SocketAddress _address;
    std::unique_ptr<rtc::Socket> _socket;
    std::function<void(const std::vector<uint8_t> &)> _onIncomingData;
    
    rtc::CopyOnWriteBuffer _peerTag;
    bool _isConnected = false;
    
    std::vector<std::vector<uint8_t>> _pendingDataToSend;
    
    std::vector<uint8_t> _readBuffer;
    absl::optional<PacketReadState> _pendingRead;
};

}  // namespace tgcalls

#endif  // TGCALLS_REFLECTOR_SIGNALING_CONNECTION_H_
