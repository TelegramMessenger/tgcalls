#include "v2/ReflectorPort.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/strings/match.h"
#include "absl/types/optional.h"
#include "api/transport/stun.h"
#include "p2p/base/connection.h"
#include "p2p/base/p2p_constants.h"
#include "rtc_base/async_packet_socket.h"
#include "rtc_base/byte_order.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/net_helpers.h"
#include "rtc_base/socket_address.h"
#include "rtc_base/strings/string_builder.h"
#include "rtc_base/task_utils/to_queued_task.h"
#include "system_wrappers/include/field_trial.h"

namespace tgcalls {

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

// Retry at most twice (i.e. three different ALLOCATE requests) on
// STUN_ERROR_ALLOCATION_MISMATCH error per rfc5766.
static const size_t MAX_ALLOCATE_MISMATCH_RETRIES = 2;

static int GetRelayPreference(cricket::ProtocolType proto) {
    switch (proto) {
        case cricket::PROTO_TCP:
            return cricket::ICE_TYPE_PREFERENCE_RELAY_TCP;
        case cricket::PROTO_TLS:
            return cricket::ICE_TYPE_PREFERENCE_RELAY_TLS;
        default:
            RTC_DCHECK(proto == cricket::PROTO_UDP);
            return cricket::ICE_TYPE_PREFERENCE_RELAY_UDP;
    }
}

ReflectorPort::ReflectorPort(rtc::Thread* thread,
                             rtc::PacketSocketFactory* factory,
                             rtc::Network* network,
                             rtc::AsyncPacketSocket* socket,
                             const std::string& username,
                             const std::string& password,
                             const cricket::ProtocolAddress& server_address,
                             const cricket::RelayCredentials& credentials,
                             int server_priority,
                             webrtc::TurnCustomizer* customizer)
: Port(thread, cricket::RELAY_PORT_TYPE, factory, network, username, password),
server_address_(server_address),
tls_cert_verifier_(nullptr),
credentials_(credentials),
socket_(socket),
error_(0),
stun_dscp_value_(rtc::DSCP_NO_CHANGE),
request_manager_(thread),
state_(STATE_CONNECTING),
server_priority_(server_priority),
allocate_mismatch_retries_(0),
turn_customizer_(customizer) {
    peer_tag_ = parseHex(credentials.password);
    
    request_manager_.SignalSendPacket.connect(this, &ReflectorPort::OnSendStunPacket);
}

ReflectorPort::ReflectorPort(rtc::Thread* thread,
                             rtc::PacketSocketFactory* factory,
                             rtc::Network* network,
                             uint16_t min_port,
                             uint16_t max_port,
                             const std::string& username,
                             const std::string& password,
                             const cricket::ProtocolAddress& server_address,
                             const cricket::RelayCredentials& credentials,
                             int server_priority,
                             const std::vector<std::string>& tls_alpn_protocols,
                             const std::vector<std::string>& tls_elliptic_curves,
                             webrtc::TurnCustomizer* customizer,
                             rtc::SSLCertificateVerifier* tls_cert_verifier)
: Port(thread,
       cricket::RELAY_PORT_TYPE,
       factory,
       network,
       min_port,
       max_port,
       username,
       password),
server_address_(server_address),
tls_alpn_protocols_(tls_alpn_protocols),
tls_elliptic_curves_(tls_elliptic_curves),
tls_cert_verifier_(tls_cert_verifier),
credentials_(credentials),
socket_(NULL),
error_(0),
stun_dscp_value_(rtc::DSCP_NO_CHANGE),
request_manager_(thread),
state_(STATE_CONNECTING),
server_priority_(server_priority),
allocate_mismatch_retries_(0),
turn_customizer_(customizer) {
    peer_tag_ = parseHex(credentials.password);
    
    request_manager_.SignalSendPacket.connect(this, &ReflectorPort::OnSendStunPacket);
}

ReflectorPort::~ReflectorPort() {
    // TODO(juberti): Should this even be necessary?
    
    // release the allocation by sending a refresh with
    // lifetime 0.
    if (ready()) {
        Release();
    }
    
    if (!SharedSocket()) {
        delete socket_;
    }
}

rtc::SocketAddress ReflectorPort::GetLocalAddress() const {
    return socket_ ? socket_->GetLocalAddress() : rtc::SocketAddress();
}

cricket::ProtocolType ReflectorPort::GetProtocol() const {
    return server_address_.proto;
}

cricket::TlsCertPolicy ReflectorPort::GetTlsCertPolicy() const {
    return tls_cert_policy_;
}

void ReflectorPort::SetTlsCertPolicy(cricket::TlsCertPolicy tls_cert_policy) {
    tls_cert_policy_ = tls_cert_policy;
}

void ReflectorPort::SetTurnLoggingId(const std::string& turn_logging_id) {
    turn_logging_id_ = turn_logging_id;
}

std::vector<std::string> ReflectorPort::GetTlsAlpnProtocols() const {
    return tls_alpn_protocols_;
}

std::vector<std::string> ReflectorPort::GetTlsEllipticCurves() const {
    return tls_elliptic_curves_;
}

void ReflectorPort::PrepareAddress() {
    if (peer_tag_.size() != 16) {
        RTC_LOG(LS_ERROR) << "Allocation can't be started without setting the"
        " peer tag.";
        OnAllocateError(cricket::STUN_ERROR_UNAUTHORIZED,
                        "Missing REFLECTOR server credentials.");
        return;
    }
    
    if (!server_address_.address.port()) {
        // We will set default REFLECTOR port, if no port is set in the address.
        server_address_.address.SetPort(599);
    }
    
    if (!AllowedReflectorPort(server_address_.address.port())) {
        // This can only happen after a 300 ALTERNATE SERVER, since the port can't
        // be created with a disallowed port number.
        RTC_LOG(LS_ERROR) << "Attempt to start allocation with disallowed port# "
        << server_address_.address.port();
        OnAllocateError(cricket::STUN_ERROR_SERVER_ERROR,
                        "Attempt to start allocation to a disallowed port");
        return;
    }
    if (server_address_.address.IsUnresolvedIP()) {
        ResolveTurnAddress(server_address_.address);
    } else {
        // If protocol family of server address doesn't match with local, return.
        if (!IsCompatibleAddress(server_address_.address)) {
            RTC_LOG(LS_ERROR) << "IP address family does not match. server: "
            << server_address_.address.family()
            << " local: " << Network()->GetBestIP().family();
            OnAllocateError(cricket::STUN_ERROR_GLOBAL_FAILURE,
                            "IP address family does not match.");
            return;
        }
        
        // Insert the current address to prevent redirection pingpong.
        attempted_server_addresses_.insert(server_address_.address);
        
        RTC_LOG(LS_INFO) << ToString() << ": Trying to connect to REFLECTOR server via "
        << ProtoToString(server_address_.proto) << " @ "
        << server_address_.address.ToSensitiveString();
        if (!CreateReflectorClientSocket()) {
            RTC_LOG(LS_ERROR) << "Failed to create REFLECTOR client socket";
            OnAllocateError(cricket::SERVER_NOT_REACHABLE_ERROR,
                            "Failed to create REFLECTOR client socket.");
            return;
        }
        if (server_address_.proto == cricket::PROTO_UDP) {
            SendReflectorHello();
        }
    }
}

void ReflectorPort::SendReflectorHello() {
    if (state_ == STATE_READY) {
        return;
    }
    
    if (state_ != STATE_READY) {
        state_ = STATE_READY;
        
        // For relayed candidate, Base is the candidate itself.
        AddAddress(server_address_.address,          // Candidate address.
                   server_address_.address,          // Base address.
                   rtc::SocketAddress(),  // Related address.
                   cricket::UDP_PROTOCOL_NAME,
                   ProtoToString(server_address_.proto),  // The first hop protocol.
                   "",  // TCP canddiate type, empty for turn candidates.
                   cricket::RELAY_PORT_TYPE, GetRelayPreference(server_address_.proto),
                   server_priority_, ReconstructedServerUrl(false /* use_hostname */),
                   true);
    }
    
    RTC_LOG(LS_WARNING)
    << ToString()
    << ": REFLECTOR sending hello to " << server_address_.address.ToString();
    
    rtc::ByteBufferWriter bufferWriter;
    bufferWriter.WriteBytes((const char *)peer_tag_.data(), peer_tag_.size());
    
    rtc::PacketOptions options;
    Send(bufferWriter.Data(), bufferWriter.Length(), options);
    
    /*if (!is_running_ping_task_) {
        is_running_ping_task_ = true;
        
        thread()->PostDelayedTask(ToQueuedTask(task_safety_.flag(), [this] {
            is_running_ping_task_ = false;
            SendReflectorHello();
        }), 1000);
    }*/
}

bool ReflectorPort::CreateReflectorClientSocket() {
    RTC_DCHECK(!socket_ || SharedSocket());
    
    if (server_address_.proto == cricket::PROTO_UDP && !SharedSocket()) {
        socket_ = socket_factory()->CreateUdpSocket(
                                                    rtc::SocketAddress(Network()->GetBestIP(), 0), min_port(), max_port());
    } else if (server_address_.proto == cricket::PROTO_TCP ||
               server_address_.proto == cricket::PROTO_TLS) {
        RTC_DCHECK(!SharedSocket());
        int opts = rtc::PacketSocketFactory::OPT_STUN;
        
        // Apply server address TLS and insecure bits to options.
        if (server_address_.proto == cricket::PROTO_TLS) {
            if (tls_cert_policy_ ==
                cricket::TlsCertPolicy::TLS_CERT_POLICY_INSECURE_NO_CHECK) {
                opts |= rtc::PacketSocketFactory::OPT_TLS_INSECURE;
            } else {
                opts |= rtc::PacketSocketFactory::OPT_TLS;
            }
        }
        
        rtc::PacketSocketTcpOptions tcp_options;
        tcp_options.opts = opts;
        tcp_options.tls_alpn_protocols = tls_alpn_protocols_;
        tcp_options.tls_elliptic_curves = tls_elliptic_curves_;
        tcp_options.tls_cert_verifier = tls_cert_verifier_;
        socket_ = socket_factory()->CreateClientTcpSocket(
                                                          rtc::SocketAddress(Network()->GetBestIP(), 0), server_address_.address,
                                                          proxy(), user_agent(), tcp_options);
    }
    
    if (!socket_) {
        error_ = SOCKET_ERROR;
        return false;
    }
    
    // Apply options if any.
    for (SocketOptionsMap::iterator iter = socket_options_.begin();
         iter != socket_options_.end(); ++iter) {
        socket_->SetOption(iter->first, iter->second);
    }
    
    if (!SharedSocket()) {
        // If socket is shared, AllocationSequence will receive the packet.
        socket_->SignalReadPacket.connect(this, &ReflectorPort::OnReadPacket);
    }
    
    socket_->SignalReadyToSend.connect(this, &ReflectorPort::OnReadyToSend);
    
    socket_->SignalSentPacket.connect(this, &ReflectorPort::OnSentPacket);
    
    // TCP port is ready to send stun requests after the socket is connected,
    // while UDP port is ready to do so once the socket is created.
    if (server_address_.proto == cricket::PROTO_TCP ||
        server_address_.proto == cricket::PROTO_TLS) {
        socket_->SignalConnect.connect(this, &ReflectorPort::OnSocketConnect);
        socket_->SignalClose.connect(this, &ReflectorPort::OnSocketClose);
    } else {
        state_ = STATE_CONNECTED;
    }
    return true;
}

void ReflectorPort::OnSocketConnect(rtc::AsyncPacketSocket* socket) {
    // This slot should only be invoked if we're using a connection-oriented
    // protocol.
    RTC_DCHECK(server_address_.proto == cricket::PROTO_TCP ||
               server_address_.proto == cricket::PROTO_TLS);
    
    // Do not use this port if the socket bound to an address not associated with
    // the desired network interface. This is seen in Chrome, where TCP sockets
    // cannot be given a binding address, and the platform is expected to pick
    // the correct local address.
    //
    // However, there are two situations in which we allow the bound address to
    // not be one of the addresses of the requested interface:
    // 1. The bound address is the loopback address. This happens when a proxy
    // forces TCP to bind to only the localhost address (see issue 3927).
    // 2. The bound address is the "any address". This happens when
    // multiple_routes is disabled (see issue 4780).
    //
    // Note that, aside from minor differences in log statements, this logic is
    // identical to that in TcpPort.
    const rtc::SocketAddress& socket_address = socket->GetLocalAddress();
    if (absl::c_none_of(Network()->GetIPs(),
                        [socket_address](const rtc::InterfaceAddress& addr) {
        return socket_address.ipaddr() == addr;
    })) {
        if (socket->GetLocalAddress().IsLoopbackIP()) {
            RTC_LOG(LS_WARNING) << "Socket is bound to the address:"
            << socket_address.ipaddr().ToSensitiveString()
            << ", rather than an address associated with network:"
            << Network()->ToString()
            << ". Still allowing it since it's localhost.";
        } else if (IPIsAny(Network()->GetBestIP())) {
            RTC_LOG(LS_WARNING)
            << "Socket is bound to the address:"
            << socket_address.ipaddr().ToSensitiveString()
            << ", rather than an address associated with network:"
            << Network()->ToString()
            << ". Still allowing it since it's the 'any' address"
            ", possibly caused by multiple_routes being disabled.";
        } else {
            RTC_LOG(LS_WARNING) << "Socket is bound to the address:"
            << socket_address.ipaddr().ToSensitiveString()
            << ", rather than an address associated with network:"
            << Network()->ToString() << ". Discarding REFLECTOR port.";
            OnAllocateError(
                            cricket::STUN_ERROR_GLOBAL_FAILURE,
                            "Address not associated with the desired network interface.");
            return;
        }
    }
    
    state_ = STATE_CONNECTED;  // It is ready to send stun requests.
    if (server_address_.address.IsUnresolvedIP()) {
        server_address_.address = socket_->GetRemoteAddress();
    }
    
    RTC_LOG(LS_INFO) << "ReflectorPort connected to "
    << socket->GetRemoteAddress().ToSensitiveString()
    << " using tcp.";
    
    //TODO: Initiate server ping
}

void ReflectorPort::OnSocketClose(rtc::AsyncPacketSocket* socket, int error) {
    RTC_LOG(LS_WARNING) << ToString()
    << ": Connection with server failed with error: "
    << error;
    RTC_DCHECK(socket == socket_);
    Close();
}

void ReflectorPort::OnAllocateMismatch() {
    if (allocate_mismatch_retries_ >= MAX_ALLOCATE_MISMATCH_RETRIES) {
        RTC_LOG(LS_WARNING) << ToString() << ": Giving up on the port after "
        << allocate_mismatch_retries_
        << " retries for STUN_ERROR_ALLOCATION_MISMATCH";
        OnAllocateError(cricket::STUN_ERROR_ALLOCATION_MISMATCH,
                        "Maximum retries reached for allocation mismatch.");
        return;
    }
    
    RTC_LOG(LS_INFO) << ToString()
    << ": Allocating a new socket after "
    "STUN_ERROR_ALLOCATION_MISMATCH, retry: "
    << allocate_mismatch_retries_ + 1;
    if (SharedSocket()) {
        ResetSharedSocket();
    } else {
        delete socket_;
    }
    socket_ = NULL;
    
    ResetNonce();
    PrepareAddress();
    ++allocate_mismatch_retries_;
}

cricket::Connection* ReflectorPort::CreateConnection(const cricket::Candidate& remote_candidate,
                                                     CandidateOrigin origin) {
    // REFLECTOR-UDP can only connect to UDP candidates.
    if (!SupportsProtocol(remote_candidate.protocol())) {
        return nullptr;
    }
    if (remote_candidate.address() != server_address_.address) {
        return nullptr;
    }
    
    if (state_ == STATE_DISCONNECTED || state_ == STATE_RECEIVEONLY) {
        return nullptr;
    }
    
    cricket::ProxyConnection* conn = new cricket::ProxyConnection(this, 0, remote_candidate);
    AddOrReplaceConnection(conn);
    
    return conn;
    
    // If the remote endpoint signaled us an mDNS candidate, we do not form a pair
    // with the relay candidate to avoid IP leakage in the CreatePermission
    // request.
    /*if (absl::EndsWith(remote_candidate.address().hostname(), cricket::LOCAL_TLD)) {
        return nullptr;
    }
    
    // A REFLECTOR port will have two candiates, STUN and TURN. STUN may not
    // present in all cases. If present stun candidate will be added first
    // and TURN candidate later.
    for (size_t index = 0; index < Candidates().size(); ++index) {
        const cricket::Candidate& local_candidate = Candidates()[index];
        if (local_candidate.type() == cricket::RELAY_PORT_TYPE &&
            local_candidate.address().family() ==
            remote_candidate.address().family()) {
            // Create an entry, if needed, so we can get our permissions set up
            // correctly.
            if (CreateOrRefreshEntry(remote_candidate.address(), next_channel_number_,
                                     remote_candidate.username())) {
                // An entry was created.
                next_channel_number_++;
            }
            cricket::ProxyConnection* conn =
            new cricket::ProxyConnection(this, index, remote_candidate);
            AddOrReplaceConnection(conn);
            return conn;
        }
    }
    return nullptr;*/
}

bool ReflectorPort::FailAndPruneConnection(const rtc::SocketAddress& address) {
    cricket::Connection* conn = GetConnection(address);
    if (conn != nullptr) {
        conn->FailAndPrune();
        return true;
    }
    return false;
}

int ReflectorPort::SetOption(rtc::Socket::Option opt, int value) {
    // Remember the last requested DSCP value, for STUN traffic.
    if (opt == rtc::Socket::OPT_DSCP)
        stun_dscp_value_ = static_cast<rtc::DiffServCodePoint>(value);
    
    if (!socket_) {
        // If socket is not created yet, these options will be applied during socket
        // creation.
        socket_options_[opt] = value;
        return 0;
    }
    return socket_->SetOption(opt, value);
}

int ReflectorPort::GetOption(rtc::Socket::Option opt, int* value) {
    if (!socket_) {
        SocketOptionsMap::const_iterator it = socket_options_.find(opt);
        if (it == socket_options_.end()) {
            return -1;
        }
        *value = it->second;
        return 0;
    }
    
    return socket_->GetOption(opt, value);
}

int ReflectorPort::GetError() {
    return error_;
}

int ReflectorPort::SendTo(const void* data,
                          size_t size,
                          const rtc::SocketAddress& addr,
                          const rtc::PacketOptions& options,
                          bool payload) {
    rtc::ByteBufferWriter bufferWriter;
    bufferWriter.WriteBytes((const char *)peer_tag_.data(), peer_tag_.size());
    bufferWriter.WriteBytes((const char *)data, size);
    
    Send(bufferWriter.Data(), bufferWriter.Length(), options);
    
    return static_cast<int>(size);
    
    /*// Try to find an entry for this specific address; we should have one.
    TurnEntry* entry = FindEntry(addr);
    if (!entry) {
        RTC_LOG(LS_ERROR) << "Did not find the TurnEntry for address "
        << addr.ToSensitiveString();
        return 0;
    }
    
    if (!ready()) {
        error_ = ENOTCONN;
        return SOCKET_ERROR;
    }
    
    // Send the actual contents to the server using the usual mechanism.
    rtc::PacketOptions modified_options(options);
    CopyPortInformationToPacketInfo(&modified_options.info_signaled_after_sent);
    int sent = entry->Send(data, size, payload, modified_options);
    if (sent <= 0) {
        return SOCKET_ERROR;
    }
    
    // The caller of the function is expecting the number of user data bytes,
    // rather than the size of the packet.
    return static_cast<int>(size);*/
}

bool ReflectorPort::CanHandleIncomingPacketsFrom(
                                                 const rtc::SocketAddress& addr) const {
                                                     return server_address_.address == addr;
                                                 }

bool ReflectorPort::HandleIncomingPacket(rtc::AsyncPacketSocket* socket,
                                         const char* data,
                                         size_t size,
                                         const rtc::SocketAddress& remote_addr,
                                         int64_t packet_time_us) {
    if (socket != socket_) {
        // The packet was received on a shared socket after we've allocated a new
        // socket for this REFLECTOR port.
        return false;
    }
    
    // This is to guard against a STUN response from previous server after
    // alternative server redirection. TODO(guoweis): add a unit test for this
    // race condition.
    if (remote_addr != server_address_.address) {
        RTC_LOG(LS_WARNING) << ToString()
        << ": Discarding REFLECTOR message from unknown address: "
        << remote_addr.ToSensitiveString()
        << " server_address_: "
        << server_address_.address.ToSensitiveString();
        return false;
    }
    
    // The message must be at least 16 bytes (peer tag).
    if (size < 16) {
        RTC_LOG(LS_WARNING) << ToString()
        << ": Received REFLECTOR message that was too short (" << size << ")";
        return false;
    }
    
    if (state_ == STATE_DISCONNECTED) {
        RTC_LOG(LS_WARNING)
        << ToString()
        << ": Received REFLECTOR message while the REFLECTOR port is disconnected";
        return false;
    }
    
    uint8_t receivedPeerTag[16];
    memcpy(receivedPeerTag, data, 16);
    
    if (memcmp(receivedPeerTag, peer_tag_.data(), 16) != 0) {
        RTC_LOG(LS_WARNING)
        << ToString()
        << ": Received REFLECTOR message with incorrect peer_tag";
        return false;
    }
    
    if (state_ != STATE_READY) {
        state_ = STATE_READY;
        
        // For relayed candidate, Base is the candidate itself.
        AddAddress(server_address_.address,          // Candidate address.
                   server_address_.address,          // Base address.
                   rtc::SocketAddress(),  // Related address.
                   cricket::UDP_PROTOCOL_NAME,
                   ProtoToString(server_address_.proto),  // The first hop protocol.
                   "",  // TCP canddiate type, empty for turn candidates.
                   cricket::RELAY_PORT_TYPE, GetRelayPreference(server_address_.proto),
                   server_priority_, ReconstructedServerUrl(false /* use_hostname */),
                   true);
    }
    
    if (size > 16) {
        DispatchPacket(data + 16, size - 16, remote_addr, cricket::ProtocolType::PROTO_UDP, packet_time_us);
    }
    
    return true;
}

void ReflectorPort::OnReadPacket(rtc::AsyncPacketSocket* socket,
                                 const char* data,
                                 size_t size,
                                 const rtc::SocketAddress& remote_addr,
                                 const int64_t& packet_time_us) {
    HandleIncomingPacket(socket, data, size, remote_addr, packet_time_us);
}

void ReflectorPort::OnSentPacket(rtc::AsyncPacketSocket* socket,
                                 const rtc::SentPacket& sent_packet) {
    PortInterface::SignalSentPacket(sent_packet);
}

void ReflectorPort::OnReadyToSend(rtc::AsyncPacketSocket* socket) {
    if (ready()) {
        Port::OnReadyToSend();
    }
}

bool ReflectorPort::SupportsProtocol(const std::string& protocol) const {
    // Turn port only connects to UDP candidates.
    return protocol == cricket::UDP_PROTOCOL_NAME;
}

// Update current server address port with the alternate server address port.
bool ReflectorPort::SetAlternateServer(const rtc::SocketAddress& address) {
    // Check if we have seen this address before and reject if we did.
    AttemptedServerSet::iterator iter = attempted_server_addresses_.find(address);
    if (iter != attempted_server_addresses_.end()) {
        RTC_LOG(LS_WARNING) << ToString() << ": Redirection to ["
        << address.ToSensitiveString()
        << "] ignored, allocation failed.";
        return false;
    }
    
    // If protocol family of server address doesn't match with local, return.
    if (!IsCompatibleAddress(address)) {
        RTC_LOG(LS_WARNING) << "Server IP address family does not match with "
        "local host address family type";
        return false;
    }
    
    // Block redirects to a loopback address.
    // See: https://bugs.chromium.org/p/chromium/issues/detail?id=649118
    if (address.IsLoopbackIP()) {
        RTC_LOG(LS_WARNING) << ToString()
        << ": Blocking attempted redirect to loopback address.";
        return false;
    }
    
    RTC_LOG(LS_INFO) << ToString() << ": Redirecting from REFLECTOR server ["
    << server_address_.address.ToSensitiveString()
    << "] to TURN server [" << address.ToSensitiveString()
    << "]";
    server_address_ = cricket::ProtocolAddress(address, server_address_.proto);
    
    // Insert the current address to prevent redirection pingpong.
    attempted_server_addresses_.insert(server_address_.address);
    return true;
}

void ReflectorPort::ResolveTurnAddress(const rtc::SocketAddress& address) {
    if (resolver_)
        return;
    
    RTC_LOG(LS_INFO) << ToString() << ": Starting TURN host lookup for "
    << address.ToSensitiveString();
    resolver_ = socket_factory()->CreateAsyncDnsResolver();
    resolver_->Start(address, [this] {
        // If DNS resolve is failed when trying to connect to the server using TCP,
        // one of the reason could be due to DNS queries blocked by firewall.
        // In such cases we will try to connect to the server with hostname,
        // assuming socket layer will resolve the hostname through a HTTP proxy (if
        // any).
        auto& result = resolver_->result();
        if (result.GetError() != 0 && (server_address_.proto == cricket::PROTO_TCP ||
                                       server_address_.proto == cricket::PROTO_TLS)) {
            if (!CreateReflectorClientSocket()) {
                OnAllocateError(cricket::SERVER_NOT_REACHABLE_ERROR,
                                "TURN host lookup received error.");
            }
            return;
        }
        
        // Copy the original server address in `resolved_address`. For TLS based
        // sockets we need hostname along with resolved address.
        rtc::SocketAddress resolved_address = server_address_.address;
        if (result.GetError() != 0 ||
            !result.GetResolvedAddress(Network()->GetBestIP().family(),
                                       &resolved_address)) {
            RTC_LOG(LS_WARNING) << ToString() << ": TURN host lookup received error "
            << result.GetError();
            error_ = result.GetError();
            OnAllocateError(cricket::SERVER_NOT_REACHABLE_ERROR,
                            "TURN host lookup received error.");
            return;
        }
        // Signal needs both resolved and unresolved address. After signal is sent
        // we can copy resolved address back into `server_address_`.
        SignalResolvedServerAddress(this, server_address_.address,
                                    resolved_address);
        server_address_.address = resolved_address;
        PrepareAddress();
    });
}

void ReflectorPort::OnSendStunPacket(const void* data,
                                     size_t size,
                                     cricket::StunRequest* request) {
    RTC_DCHECK(connected());
    rtc::PacketOptions options(StunDscpValue());
    options.info_signaled_after_sent.packet_type = rtc::PacketType::kTurnMessage;
    CopyPortInformationToPacketInfo(&options.info_signaled_after_sent);
    if (Send(data, size, options) < 0) {
        RTC_LOG(LS_ERROR) << ToString() << ": Failed to send TURN message, error: "
        << socket_->GetError();
    }
}

void ReflectorPort::OnAllocateSuccess(const rtc::SocketAddress& address,
                                      const rtc::SocketAddress& stun_address) {
    state_ = STATE_READY;
    
    rtc::SocketAddress related_address = stun_address;
    
    // For relayed candidate, Base is the candidate itself.
    AddAddress(address,          // Candidate address.
               address,          // Base address.
               related_address,  // Related address.
               cricket::UDP_PROTOCOL_NAME,
               ProtoToString(server_address_.proto),  // The first hop protocol.
               "",  // TCP canddiate type, empty for turn candidates.
               cricket::RELAY_PORT_TYPE, GetRelayPreference(server_address_.proto),
               server_priority_, ReconstructedServerUrl(false /* use_hostname */),
               true);
}

void ReflectorPort::OnAllocateError(int error_code, const std::string& reason) {
    // We will send SignalPortError asynchronously as this can be sent during
    // port initialization. This way it will not be blocking other port
    // creation.
    /*thread()->Post(RTC_FROM_HERE, this, MSG_ALLOCATE_ERROR);
    std::string address = GetLocalAddress().HostAsSensitiveURIString();
    int port = GetLocalAddress().port();
    if (server_address_.proto == cricket::PROTO_TCP &&
        server_address_.address.IsPrivateIP()) {
        address.clear();
        port = 0;
    }*/
    //SignalCandidateError(this, cricket::IceCandidateErrorEvent(address, port, ReconstructedServerUrl(true /* use_hostname */), error_code, reason));
}

void ReflectorPort::OnRefreshError() {
    // Need to clear the requests asynchronously because otherwise, the refresh
    // request may be deleted twice: once at the end of the message processing
    // and the other in HandleRefreshError().
    //thread()->Post(RTC_FROM_HERE, this, MSG_REFRESH_ERROR);
}

void ReflectorPort::HandleRefreshError() {
    request_manager_.Clear();
    state_ = STATE_RECEIVEONLY;
    // Fail and prune all connections; stop sending data.
    for (auto kv : connections()) {
        kv.second->FailAndPrune();
    }
}

void ReflectorPort::Release() {
    // Remove any pending refresh requests.
    request_manager_.Clear();
    
    state_ = STATE_RECEIVEONLY;
}

void ReflectorPort::Close() {
    if (!ready()) {
        OnAllocateError(cricket::SERVER_NOT_REACHABLE_ERROR, "");
    }
    request_manager_.Clear();
    // Stop the port from creating new connections.
    state_ = STATE_DISCONNECTED;
    // Delete all existing connections; stop sending data.
    for (auto kv : connections()) {
        kv.second->Destroy();
    }
    
    SignalReflectorPortClosed(this);
}

rtc::DiffServCodePoint ReflectorPort::StunDscpValue() const {
    return stun_dscp_value_;
}

// static
bool ReflectorPort::AllowedReflectorPort(int port) {
    return true;
    
    /*// Port 53, 80 and 443 are used for existing deployments.
    // Ports above 1024 are assumed to be OK to use.
    if (port == 53 || port == 80 || port == 443 || port >= 1024) {
        return true;
    }
    // Allow any port if relevant field trial is set. This allows disabling the
    // check.
    if (webrtc::field_trial::IsEnabled("WebRTC-Turn-AllowSystemPorts")) {
        return true;
    }
    return false;*/
}

void ReflectorPort::OnMessage(rtc::Message* message) {
    switch (message->message_id) {
        default:
            Port::OnMessage(message);
    }
}

void ReflectorPort::DispatchPacket(const char* data,
                                   size_t size,
                                   const rtc::SocketAddress& remote_addr,
                                   cricket::ProtocolType proto,
                                   int64_t packet_time_us) {
    if (!remote_addr.EqualIPs(server_address_.address)) {
        return;
    }
    
    if (cricket::Connection* conn = GetConnection(server_address_.address)) {
        conn->OnReadPacket(data, size, packet_time_us);
    } else {
        Port::OnReadPacket(data, size, server_address_.address, proto);
    }
}

void ReflectorPort::SendRequest(cricket::StunRequest* req, int delay) {
    request_manager_.SendDelayed(req, delay);
}

void ReflectorPort::AddRequestAuthInfo(cricket::StunMessage* msg) {
    // If we've gotten the necessary data from the server, add it to our request.
    RTC_DCHECK(!hash_.empty());
    msg->AddAttribute(std::make_unique<cricket::StunByteStringAttribute>(
                                                                         cricket::STUN_ATTR_USERNAME, credentials_.username));
    msg->AddAttribute(
                      std::make_unique<cricket::StunByteStringAttribute>(cricket::STUN_ATTR_REALM, realm_));
    msg->AddAttribute(
                      std::make_unique<cricket::StunByteStringAttribute>(cricket::STUN_ATTR_NONCE, nonce_));
    const bool success = msg->AddMessageIntegrity(hash());
    RTC_DCHECK(success);
}

int ReflectorPort::Send(const void* data,
                        size_t len,
                        const rtc::PacketOptions& options) {
    return socket_->SendTo(data, len, server_address_.address, options);
}

void ReflectorPort::UpdateHash() {
    const bool success = cricket::ComputeStunCredentialHash(credentials_.username, realm_,
                                                   credentials_.password, &hash_);
    RTC_DCHECK(success);
}

bool ReflectorPort::UpdateNonce(cricket::StunMessage* response) {
    // When stale nonce error received, we should update
    // hash and store realm and nonce.
    // Check the mandatory attributes.
    const cricket::StunByteStringAttribute* realm_attr =
    response->GetByteString(cricket::STUN_ATTR_REALM);
    if (!realm_attr) {
        RTC_LOG(LS_ERROR) << "Missing STUN_ATTR_REALM attribute in "
        "stale nonce error response.";
        return false;
    }
    set_realm(realm_attr->GetString());
    
    const cricket::StunByteStringAttribute* nonce_attr =
    response->GetByteString(cricket::STUN_ATTR_NONCE);
    if (!nonce_attr) {
        RTC_LOG(LS_ERROR) << "Missing STUN_ATTR_NONCE attribute in "
        "stale nonce error response.";
        return false;
    }
    set_nonce(nonce_attr->GetString());
    return true;
}

void ReflectorPort::ResetNonce() {
    hash_.clear();
    nonce_.clear();
    realm_.clear();
}

void ReflectorPort::HandleConnectionDestroyed(cricket::Connection* conn) {
    /*// Schedule an event to destroy TurnEntry for the connection, which is
    // already destroyed.
    const rtc::SocketAddress& remote_address = conn->remote_candidate().address();
    TurnEntry* entry = FindEntry(remote_address);
    RTC_DCHECK(entry != NULL);
    ScheduleEntryDestruction(entry);*/
}

std::string ReflectorPort::ReconstructedServerUrl(bool use_hostname) {
    // draft-petithuguenin-behave-turn-uris-01
    // turnURI       = scheme ":" turn-host [ ":" turn-port ]
    //                 [ "?transport=" transport ]
    // scheme        = "turn" / "turns"
    // transport     = "udp" / "tcp" / transport-ext
    // transport-ext = 1*unreserved
    // turn-host     = IP-literal / IPv4address / reg-name
    // turn-port     = *DIGIT
    std::string scheme = "turn";
    std::string transport = "tcp";
    switch (server_address_.proto) {
        case cricket::PROTO_SSLTCP:
        case cricket::PROTO_TLS:
            scheme = "turns";
            break;
        case cricket::PROTO_UDP:
            transport = "udp";
            break;
        case cricket::PROTO_TCP:
            break;
    }
    rtc::StringBuilder url;
    url << scheme << ":"
    << (use_hostname ? server_address_.address.hostname()
        : server_address_.address.ipaddr().ToString())
    << ":" << server_address_.address.port() << "?transport=" << transport;
    return url.Release();
}

void ReflectorPort::TurnCustomizerMaybeModifyOutgoingStunMessage(
                                                                 cricket::StunMessage* message) {
    if (turn_customizer_ == nullptr) {
        return;
    }
    
    turn_customizer_->MaybeModifyOutgoingStunMessage(this, message);
}

bool ReflectorPort::TurnCustomizerAllowChannelData(const void* data,
                                                   size_t size,
                                                   bool payload) {
    return true;
}

void ReflectorPort::MaybeAddTurnLoggingId(cricket::StunMessage* msg) {
}

}  // namespace cricket
