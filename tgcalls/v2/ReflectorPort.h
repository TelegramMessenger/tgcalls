#ifndef TGCALLS_REFLECTOR_PORT_H_
#define TGCALLS_REFLECTOR_PORT_H_

#include <cstdint>
#include <stdio.h>

#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "absl/memory/memory.h"
#include "api/async_dns_resolver.h"
#include "p2p/base/port.h"
#include "p2p/client/basic_port_allocator.h"
#include "rtc_base/async_packet_socket.h"
#include "rtc_base/ssl_certificate.h"
#include "rtc_base/third_party/sigslot/sigslot.h"

namespace webrtc {
class TurnCustomizer;
}

namespace tgcalls {

extern const int STUN_ATTR_TURN_LOGGING_ID;
extern const char TURN_PORT_TYPE[];
class TurnAllocateRequest;
class TurnEntry;

class ReflectorPort : public webrtc::Port {
public:
    enum PortState {
        STATE_CONNECTING,    // Initial state, cannot send any packets.
        STATE_CONNECTED,     // Socket connected, ready to send stun requests.
        STATE_READY,         // Received allocate success, can send any packets.
        STATE_RECEIVEONLY,   // Had REFRESH_REQUEST error, cannot send any packets.
        STATE_DISCONNECTED,  // TCP connection died, cannot send/receive any
        // packets.
    };
    
    // Create a TURN port using the shared UDP socket, `socket`.
    static std::unique_ptr<ReflectorPort> Create(
        const webrtc::CreateRelayPortArgs& args,
        webrtc::SocketFactory *underlying_socket_factory,
        webrtc::AsyncPacketSocket* socket,
        uint8_t serverId,
        int server_priority,
        bool standaloneReflectorMode,
        uint32_t standaloneReflectorRoleId
    ) {
        // Do basic parameter validation.
        if (args.config->credentials.username.size() > 32) {
            RTC_LOG(LS_ERROR) << "Attempt to use REFLECTOR with a too long username "
            << "of length " << args.config->credentials.username.size();
            return nullptr;
        }
        // Do not connect to low-numbered ports. The default STUN port is 3478.
        if (!AllowedReflectorPort(args.server_address->address.port())) {
            RTC_LOG(LS_ERROR) << "Attempt to use REFLECTOR to connect to port "
            << args.server_address->address.port();
            return nullptr;
        }
        // Using `new` to access a non-public constructor.
        return absl::WrapUnique(new ReflectorPort(args, underlying_socket_factory, socket, serverId, server_priority, standaloneReflectorMode, standaloneReflectorRoleId));
    }
    
    // Create a TURN port that will use a new socket, bound to `network` and
    // using a port in the range between `min_port` and `max_port`.
    static std::unique_ptr<ReflectorPort> Create(
        const webrtc::CreateRelayPortArgs& args,
        webrtc::SocketFactory *underlying_socket_factory,
        uint16_t min_port,
        uint16_t max_port,
        uint8_t serverId,
        int server_priority,
        bool standaloneReflectorMode,
        uint32_t standaloneReflectorRoleId
    ) {
        // Do basic parameter validation.
        if (args.config->credentials.username.size() > 32) {
            RTC_LOG(LS_ERROR) << "Attempt to use TURN with a too long username "
            << "of length " << args.config->credentials.username.size();
            return nullptr;
        }
        // Do not connect to low-numbered ports. The default STUN port is 3478.
        if (!AllowedReflectorPort(args.server_address->address.port())) {
            RTC_LOG(LS_ERROR) << "Attempt to use TURN to connect to port "
            << args.server_address->address.port();
            return nullptr;
        }
        // Using `new` to access a non-public constructor.
        return absl::WrapUnique(new ReflectorPort(args, underlying_socket_factory, min_port, max_port, serverId, server_priority, standaloneReflectorMode, standaloneReflectorRoleId));
    }
    
    ~ReflectorPort() override;
    
    const webrtc::ProtocolAddress& server_address() const { return server_address_; }
    // Returns an empty address if the local address has not been assigned.
    webrtc::SocketAddress GetLocalAddress() const;
    
    bool ready() const { return state_ == STATE_READY; }
    bool connected() const {
        return state_ == STATE_READY || state_ == STATE_CONNECTED;
    }
    const webrtc::RelayCredentials& credentials() const { return credentials_; }
    
    webrtc::ProtocolType GetProtocol() const override;
    
    // Sets state to STATE_RECEIVEONLY.
    void Release();
    
    void PrepareAddress() override;
    webrtc::Connection* CreateConnection(const webrtc::Candidate& c,
                                          PortInterface::CandidateOrigin origin) override;
    int SendTo(const void* data,
               size_t size,
               const webrtc::SocketAddress& addr,
               const webrtc::AsyncSocketPacketOptions& options,
               bool payload) override;
    int SetOption(webrtc::Socket::Option opt, int value) override;
    int GetOption(webrtc::Socket::Option opt, int* value) override;
    int GetError() override;
    
    bool HandleIncomingPacket(webrtc::AsyncPacketSocket* socket,
                              const webrtc::ReceivedIpPacket& packet) override;
    bool CanHandleIncomingPacketsFrom(
                                      const webrtc::SocketAddress& addr) const override;
    virtual void OnReadPacket(webrtc::AsyncPacketSocket* socket,
                              const webrtc::ReceivedIpPacket& packet);
    
    void OnSentPacket(webrtc::AsyncPacketSocket* socket,
                      const webrtc::SentPacketInfo& sent_packet) override;
    virtual void OnReadyToSend(webrtc::AsyncPacketSocket* socket);
    bool SupportsProtocol(absl::string_view protocol) const override;
    
    void OnSocketConnect(webrtc::AsyncPacketSocket* socket);
    void OnSocketClose(webrtc::AsyncPacketSocket* socket, int error);
    
    int error() const { return error_; }
    
    webrtc::AsyncPacketSocket* socket() const { return socket_; }
    
    // Signal with resolved server address.
    // Parameters are port, server address and resolved server address.
    // This signal will be sent only if server address is resolved successfully.
    sigslot::
    signal3<ReflectorPort*, const webrtc::SocketAddress&, const webrtc::SocketAddress&>
    SignalResolvedServerAddress;
    
    // Signal when ReflectorPort is closed,
    // e.g remote socket closed (TCP)
    //  or receiveing a REFRESH response with lifetime 0.
    sigslot::signal1<ReflectorPort*> SignalReflectorPortClosed;
    
    // All public methods/signals below are for testing only.
    sigslot::signal2<ReflectorPort*, int> SignalTurnRefreshResult;
    sigslot::signal3<ReflectorPort*, const webrtc::SocketAddress&, int>
    SignalCreatePermissionResult;
    
    // Visible for testing.
    // Shuts down the turn port, usually because of some fatal errors.
    void Close();
    
    void HandleConnectionDestroyed(webrtc::Connection* conn) override;
    
protected:
    ReflectorPort(const webrtc::CreateRelayPortArgs& args,
                  webrtc::SocketFactory *underlying_socket_factory,
                  webrtc::AsyncPacketSocket* socket,
                  uint8_t serverId,
                  int server_priority,
                  bool standaloneReflectorMode,
                  uint32_t standaloneReflectorRoleId);
    
    ReflectorPort(const webrtc::CreateRelayPortArgs& args,
                  webrtc::SocketFactory *underlying_socket_factory,
                  uint16_t min_port,
                  uint16_t max_port,
                  uint8_t serverId,
                  int server_priority,
                  bool standaloneReflectorMode,
                  uint32_t standaloneReflectorRoleId);
    
    webrtc::DiffServCodePoint StunDscpValue() const override;
    
private:
    typedef std::map<webrtc::Socket::Option, int> SocketOptionsMap;
    typedef std::set<webrtc::SocketAddress> AttemptedServerSet;
    
    static bool AllowedReflectorPort(int port);
    
    bool CreateReflectorClientSocket();
    
    void ResolveTurnAddress(const webrtc::SocketAddress& address);
    
    void OnSendStunPacket(const void* data, size_t size, webrtc::StunRequest* request);
    
    void OnAllocateError(int error_code, const std::string& reason);
    
    void DispatchPacket(const char* data,
                        size_t size,
                        const webrtc::SocketAddress& remote_addr,
                        int64_t packet_time_us,
                        webrtc::ProtocolType proto);
    
    int Send(const void* data, size_t size, const webrtc::AsyncSocketPacketOptions& options);
    
    // Marks the connection with remote address `address` failed and
    // pruned (a.k.a. write-timed-out). Returns true if a connection is found.
    bool FailAndPruneConnection(const webrtc::SocketAddress& address);
    
    // Reconstruct the URL of the server which the candidate is gathered from.
    std::string ReconstructedServerUrl(bool use_hostname);
    
    void SendReflectorHello();
    
    webrtc::CopyOnWriteBuffer peer_tag_;
    uint32_t randomTag_ = 0;
    
    webrtc::ProtocolAddress server_address_;
    uint8_t serverId_ = 0;
    
    std::map<std::string, uint32_t> resolved_peer_tags_by_hostname_;
    
    webrtc::RelayCredentials credentials_;
    AttemptedServerSet attempted_server_addresses_;
    
    webrtc::AsyncPacketSocket* socket_;
    webrtc::SocketFactory *underlying_socket_factory_;
    SocketOptionsMap socket_options_;
    std::unique_ptr<webrtc::AsyncDnsResolverInterface> resolver_;
    int error_;
    webrtc::DiffServCodePoint stun_dscp_value_;
    
    PortState state_;
    // By default the value will be set to 0. This value will be used in
    // calculating the candidate priority.
    int server_priority_;
    bool standaloneReflectorMode_ = false;
    uint32_t standaloneReflectorRoleId_ = 0;

    webrtc::ScopedTaskSafety task_safety_;
    
    bool is_running_ping_task_ = false;
};

}  // namespace tgcalls

#endif  // TGCALLS_REFLECTOR_PORT_H_
