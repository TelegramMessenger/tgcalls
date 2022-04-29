#include "v2/ReflectorSignalingConnection.h"

#include "rtc_base/async_tcp_socket.h"
#include "p2p/base/basic_packet_socket_factory.h"

namespace tgcalls {

namespace {

namespace {

rtc::CopyOnWriteBuffer parseHex(std::string const &string) {
    rtc::CopyOnWriteBuffer result;
    
    for (size_t i = 0; i < string.length(); i += 2) {
        std::string byteString = string.substr(i, 2);
        char byte = (char)strtol(byteString.c_str(), NULL, 16);
        result.AppendData(&byte, 1);
    }
    
    return result;
}

}

}

ReflectorSignalingConnection::ReflectorSignalingConnection(std::shared_ptr<Threads> threads, std::function<void(const std::vector<uint8_t> &)> onIncomingData, std::string const &ip, uint16_t port, std::string const &peerTag) :
_threads(threads),
_address(ip, port),
_onIncomingData(onIncomingData) {
    _peerTag = parseHex(peerTag);
}

ReflectorSignalingConnection::~ReflectorSignalingConnection() {
    if (_socket) {
        cleanupSocket();
    }
}

void ReflectorSignalingConnection::start() {
    restartSocket();
}

void ReflectorSignalingConnection::restartSocket() {
    rtc::IPAddress localAddress(0);
    switch (_address.family()) {
        case AF_INET:
            localAddress = rtc::IPAddress(INADDR_ANY);
            break;
        case AF_INET6:
            localAddress = rtc::IPAddress(in6addr_any);
            break;
        default:
            break;
    }
    
    auto socketFactory = _threads->getNetworkThread()->socketserver();
    _socket.reset(socketFactory->CreateSocket(localAddress.family(), SOCK_STREAM));
    
    if (_socket) {
        _socket->SignalConnectEvent.connect(this, &ReflectorSignalingConnection::onSocketConnect);
        _socket->SignalCloseEvent.connect(this, &ReflectorSignalingConnection::onSocketClose);
        _socket->SignalReadEvent.connect(this, &ReflectorSignalingConnection::onReadPacket);
        
        _socket->Connect(_address);
    }
}

void ReflectorSignalingConnection::cleanupSocket() {
    if (!_socket) {
        return;
    }
    
    _isConnected = false;
    
    _socket->SignalConnectEvent.disconnect(this);
    _socket->SignalCloseEvent.disconnect(this);
    _socket->SignalReadEvent.disconnect(this);
    
    _socket->Close();
    
    _socket.reset();
}

void ReflectorSignalingConnection::onSocketConnect(rtc::Socket *socket) {
    if (_socket.get() != socket) {
        return;
    }
    
    _isConnected = true;
    
    sendConnectionHeader();
    sendDataToSocket({});
    
    for (const auto &packet : _pendingDataToSend) {
        sendDataToSocket(packet);
    }
    _pendingDataToSend.clear();
}

void ReflectorSignalingConnection::onSocketClose(rtc::Socket* socket, int error) {
    if (_socket.get() != socket) {
        return;
    }
    
    cleanupSocket();
    restartSocket();
}

void ReflectorSignalingConnection::onReadPacket(rtc::Socket* socket) {
    if (_socket.get() != socket) {
        return;
    }
    
    int readBlockSize = 8 * 1024 * 1024;
    int totalReadBytes = 0;
    
    while (true) {
        if (_readBuffer.size() < totalReadBytes + readBlockSize) {
            _readBuffer.resize(totalReadBytes + readBlockSize);
        }
        
        int readBytes = _socket->Recv(_readBuffer.data() + totalReadBytes, readBlockSize, nullptr);
        if (readBytes < 0) {
            break;
        }
        
        totalReadBytes += readBytes;
    }
    
    rtc::ByteBufferReader reader((const char *)_readBuffer.data(), _readBuffer.size());
    while(true) {
        if (!consumeIncomingData(reader)) {
            break;
        }
    }
}

bool ReflectorSignalingConnection::consumeIncomingData(rtc::ByteBufferReader &reader) {
    if (reader.Length() == 0) {
        return false;
    }
    
    if (!_pendingRead) {
        _pendingRead = PacketReadState();
        _pendingRead->remainingHeaderSize = 32;
    }
    
    while (_pendingRead->remainingHeaderSize > 0) {
        int readSize = std::min((int)reader.Length(), _pendingRead->remainingHeaderSize);
        if (readSize > 0) {
            std::vector<uint8_t> tempBytes;
            tempBytes.resize(readSize);
            reader.ReadBytes((char *)tempBytes.data(), readSize);
            _pendingRead->headerData.AppendData(tempBytes.data(), tempBytes.size());
            _pendingRead->remainingHeaderSize -= readSize;
            if (_pendingRead->remainingHeaderSize == 0) {
                _pendingRead->isHeaderCompleted = true;
                
                uint32_t dataSize = 0;
                memcpy(&dataSize, _pendingRead->headerData.data() + 16, 4);
                
                if (dataSize < 2 * 1024 * 1024) {
                    _pendingRead->remainingDataSize = dataSize;
                } else {
                    return false;
                }
                
                break;
            }
        } else {
            break;
        }
    }
    
    if (_pendingRead->isHeaderCompleted) {
        while (_pendingRead->remainingDataSize > 0) {
            int readSize = std::min((int)reader.Length(), _pendingRead->remainingDataSize);
            if (readSize > 0) {
                std::vector<uint8_t> tempBytes;
                tempBytes.resize(readSize);
                reader.ReadBytes((char *)tempBytes.data(), readSize);
                _pendingRead->data.AppendData(tempBytes.data(), tempBytes.size());
                _pendingRead->remainingDataSize -= readSize;
                if (_pendingRead->remainingDataSize == 0) {
                    _pendingRead->isDataCompleted = true;
                    
                    break;
                }
            } else {
                break;
            }
        }
    }
    
    if (_pendingRead->isHeaderCompleted && _pendingRead->isDataCompleted) {
        processIncomingPacket(_pendingRead->headerData, _pendingRead->data);
        _pendingRead.reset();
    }
    
    return true;
}

void ReflectorSignalingConnection::processIncomingPacket(rtc::CopyOnWriteBuffer const &header, rtc::CopyOnWriteBuffer const &data) {
    if (_peerTag.size() != 16) {
        return;
    }
    if (header.size() < 16) {
        return;
    }

    if (memcmp(_peerTag.data(), header.data(), 16) != 0) {
        return;
    }
    
    if (data.size() != 0) {
        _onIncomingData(std::vector<uint8_t>(data.data(), data.data() + data.size()));
    }
}

void ReflectorSignalingConnection::send(const std::vector<uint8_t> &data) {
    if (_peerTag.size() != 16) {
        return;
    }
    
    rtc::PacketOptions options;
    if (_socket && _isConnected) {
        sendDataToSocket(data);
    } else {
        _pendingDataToSend.push_back(data);
    }
}

void ReflectorSignalingConnection::sendConnectionHeader() {
    if (!_socket || !_isConnected) {
        return;
    }
    
    rtc::ByteBufferWriter writer;
    writer.WriteUInt32(0xeeeeeeee);
    
    _socket->Send(writer.Data(), writer.Length());
}

void ReflectorSignalingConnection::sendDataToSocket(const std::vector<uint8_t> &data) {
    if (!_socket || !_isConnected) {
        return;
    }
    
    rtc::ByteBufferWriter writer;
    uint32_t dataSize = (uint32_t)data.size() + 16;
    dataSize = (dataSize + 3) & ~(4 - 1);
    writer.WriteBytes((const char *)&dataSize, 4);
    writer.WriteBytes((const char *)_peerTag.data(), _peerTag.size());
    if (!data.empty()) {
        writer.WriteBytes((const char *)data.data(), data.size());
    }
    for (uint32_t i = 16 + (uint32_t)data.size(); i < dataSize; i++) {
        writer.WriteUInt8(0);
    }
    
    int totalSentBytes = 0;
    while (totalSentBytes < writer.Length()) {
        int sentBytes = _socket->Send(writer.Data(), writer.Length());
        if (sentBytes <= 0) {
            break;
        }
        totalSentBytes += sentBytes;
    }
}

}
