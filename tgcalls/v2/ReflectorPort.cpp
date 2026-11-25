#include "v2/ReflectorPort.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <random>
#include <sstream>

#include "absl/algorithm/container.h"
#include "absl/strings/match.h"
#include <optional>
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
#include "api/environment/deprecated_global_field_trials.h"
#include "rtc_base/byte_order.h"

#include "RawTcpSocket.h"

namespace tgcalls {

namespace {

webrtc::CopyOnWriteBuffer parseHex(std::string const &string) {
    webrtc::CopyOnWriteBuffer result;
    
    for (size_t i = 0; i < string.length(); i += 2) {
        std::string byteString = string.substr(i, 2);
        char byte = (char)strtol(byteString.c_str(), NULL, 16);
        result.AppendData(&byte, 1);
    }
    
    return result;
}

int BindSocket(webrtc::Socket* socket,
               const webrtc::SocketAddress& local_address,
               uint16_t min_port,
               uint16_t max_port) {
    int ret = -1;
    if (min_port == 0 && max_port == 0) {
        // If there's no port range, let the OS pick a port for us.
        ret = socket->Bind(local_address);
    } else {
        // Otherwise, try to find a port in the provided range.
        for (int port = min_port; ret < 0 && port <= max_port; ++port) {
            ret = socket->Bind(webrtc::SocketAddress(local_address.ipaddr(), port));
        }
    }
    return ret;
}

std::unique_ptr<webrtc::AsyncPacketSocket> CreateClientRawTcpSocket(
                                              webrtc::SocketFactory *socket_factory_,
                                              const webrtc::SocketAddress& local_address,
                                              const webrtc::SocketAddress& remote_address) {
    std::unique_ptr<webrtc::Socket> socket(socket_factory_->CreateSocket(local_address.family(), SOCK_STREAM));
    if (!socket) {
        return nullptr;
    }
    
    if (BindSocket(socket.get(), local_address, 0, 0) < 0) {
        // Allow BindSocket to fail if we're binding to the ANY address, since this
        // is mostly redundant in the first place. The socket will be bound when we
        // call Connect() instead.
        if (local_address.IsAnyIP()) {
            RTC_LOG(LS_WARNING) << "TCP bind failed with error " << socket->GetError()
            << "; ignoring since socket is using 'any' address.";
        } else {
            RTC_LOG(LS_ERROR) << "TCP bind failed with error " << socket->GetError();
            return nullptr;
        }
    }
    
    // Set TCP_NODELAY (via OPT_NODELAY) for improved performance; this causes
    // small media packets to be sent immediately rather than being buffered up,
    // reducing latency.
    //
    // Must be done before calling Connect, otherwise it may fail.
    if (socket->SetOption(webrtc::Socket::OPT_NODELAY, 1) != 0) {
        RTC_LOG(LS_ERROR) << "Setting TCP_NODELAY option failed with error "
        << socket->GetError();
    }
    
    if (socket->Connect(remote_address) < 0) {
        RTC_LOG(LS_ERROR) << "TCP connect failed with error " << socket->GetError();
        return nullptr;
    }
    
    // Finally, wrap that socket in the reflector's TCP framing.
    return std::make_unique<webrtc::RawTcpSocket>(std::move(socket));
}

// The base Port's parameters, as TurnPort::Create builds them. Moves the callbacks out of `args`.
webrtc::Port::PortParametersRef ReflectorPortParameters(webrtc::CreateRelayPortArgs &args) {
    return {
        .env = args.env,
        .network_thread = args.network_thread,
        .socket_factory = args.socket_factory,
        .network = args.network,
        .ice_username_fragment = args.username,
        .ice_password = args.password,
        .content_name = args.content_name,
        .component = args.component,
        .generation = args.generation,
        .send_retransmit_count_attribute = args.send_retransmit_count_attribute,
        .lna_permission_factory = args.lna_permission_factory,
        .ice_tiebreaker = args.ice_tiebreaker,
        .callbacks = std::move(args.callbacks),
    };
}

}

static int GetRelayPreference(webrtc::ProtocolType proto) {
    switch (proto) {
        case webrtc::PROTO_TCP:
            return webrtc::ICE_TYPE_PREFERENCE_RELAY_TCP;
        case webrtc::PROTO_TLS:
            return webrtc::ICE_TYPE_PREFERENCE_RELAY_TLS;
        default:
            RTC_DCHECK(proto == webrtc::PROTO_UDP);
            return webrtc::ICE_TYPE_PREFERENCE_RELAY_UDP;
    }
}

ReflectorPort::ReflectorPort(webrtc::CreateRelayPortArgs args,
                             webrtc::SocketFactory *underlying_socket_factory,
                             webrtc::AsyncPacketSocket* socket,
                             uint8_t serverId,
                             int server_priority,
                             bool standaloneReflectorMode,
                             uint32_t standaloneReflectorRoleId,
                             bool resolveRemoteCandidateIp)
: Port(ReflectorPortParameters(args), webrtc::IceCandidateType::kRelay),
server_address_(*args.server_address),
credentials_(args.config->credentials),
socket_(socket),
underlying_socket_factory_(underlying_socket_factory),
error_(0),
stun_dscp_value_(webrtc::DSCP_NO_CHANGE),
state_(STATE_CONNECTING),
server_priority_(server_priority),
standaloneReflectorMode_(standaloneReflectorMode),
standaloneReflectorRoleId_(standaloneReflectorRoleId),
resolve_remote_candidate_ip_(resolveRemoteCandidateIp) {
    serverId_ = serverId;
    
    if (standaloneReflectorMode_) {
        randomTag_ = standaloneReflectorRoleId_;
    } else {
        auto generator = std::mt19937(std::random_device()());
        auto distribution = std::uniform_int_distribution<uint32_t>();
        do {
            randomTag_ = distribution(generator);
        } while (!randomTag_);
    }
    
    auto rawPeerTag = parseHex(args.config->credentials.password);
    if (rawPeerTag.size() == 16) {
        peer_tag_.AppendData(rawPeerTag.data(), rawPeerTag.size() - 4);
    } else {
        for (int i = 0; i < 16; i++) {
            uint8_t zero = 0;
            peer_tag_.AppendData(&zero, 1);
        }
    }
    peer_tag_.AppendData((uint8_t *)&randomTag_, 4);
}

ReflectorPort::ReflectorPort(webrtc::CreateRelayPortArgs args,
                             webrtc::SocketFactory *underlying_socket_factory,
                             uint16_t min_port,
                             uint16_t max_port,
                             uint8_t serverId,
                             int server_priority,
                             bool standaloneReflectorMode,
                             uint32_t standaloneReflectorRoleId,
                             bool resolveRemoteCandidateIp)
: Port(ReflectorPortParameters(args), webrtc::IceCandidateType::kRelay, min_port, max_port),
server_address_(*args.server_address),
credentials_(args.config->credentials),
socket_(NULL),
underlying_socket_factory_(underlying_socket_factory),
error_(0),
stun_dscp_value_(webrtc::DSCP_NO_CHANGE),
state_(STATE_CONNECTING),
server_priority_(server_priority),
standaloneReflectorMode_(standaloneReflectorMode),
standaloneReflectorRoleId_(standaloneReflectorRoleId),
resolve_remote_candidate_ip_(resolveRemoteCandidateIp) {
    serverId_ = serverId;

    if (standaloneReflectorMode_) {
        randomTag_ = standaloneReflectorRoleId_;
    } else {
        auto generator = std::mt19937(std::random_device()());
        auto distribution = std::uniform_int_distribution<uint32_t>();
        do {
            randomTag_ = distribution(generator);
        } while (!randomTag_);
    }
    
    auto rawPeerTag = parseHex(args.config->credentials.password);
    if (rawPeerTag.size() == 16) {
        peer_tag_.AppendData(rawPeerTag.data(), rawPeerTag.size() - 4);
    } else {
        for (int i = 0; i < 16; i++) {
            uint8_t zero = 0;
            peer_tag_.AppendData(&zero, 1);
        }
    }
    peer_tag_.AppendData((uint8_t *)&randomTag_, 4);
}

ReflectorPort::~ReflectorPort() {
    // TODO(juberti): Should this even be necessary?

    // release the allocation by sending a refresh with
    // lifetime 0.
    if (ready()) {
        Release();
    }

    // Unsubscribe before `owned_socket_` goes (after this body), as TurnPort does:
    // unsubscribing mutates the socket's callback lists. Unsubscribing a tag that
    // was never subscribed is a no-op.
    if (socket_) {
        socket_->UnsubscribeSentPacket(this);
        socket_->UnsubscribeReadyToSend(this);
        socket_->UnsubscribeConnect(this);
        socket_->UnsubscribeCloseEvent(this);
    }
}

webrtc::SocketAddress ReflectorPort::GetLocalAddress() const {
    return socket_ ? socket_->GetLocalAddress() : webrtc::SocketAddress();
}

webrtc::ProtocolType ReflectorPort::GetProtocol() const {
    return server_address_.proto;
}

void ReflectorPort::PrepareAddress() {
    if (peer_tag_.size() != 16) {
        RTC_LOG(LS_ERROR) << "Allocation can't be started without setting the"
        " peer tag.";
        OnAllocateError(webrtc::STUN_ERROR_UNAUTHORIZED,
                        "Missing REFLECTOR server credentials.");
        return;
    }
    if (serverId_ == 0) {
        RTC_LOG(LS_ERROR) << "Allocation can't be started without setting the"
        " server id.";
        OnAllocateError(webrtc::STUN_ERROR_UNAUTHORIZED,
                        "Missing REFLECTOR server id.");
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
        OnAllocateError(webrtc::STUN_ERROR_SERVER_ERROR,
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
            OnAllocateError(webrtc::STUN_ERROR_GLOBAL_FAILURE,
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
            OnAllocateError(webrtc::STUN_ERROR_SERVER_NOT_REACHABLE,
                            "Failed to create REFLECTOR client socket.");
            return;
        }
        if (server_address_.proto == webrtc::PROTO_UDP) {
            SendReflectorHello();
        }
    }
}

void ReflectorPort::SendReflectorHello() {
    if (!(state_ == STATE_CONNECTED || state_ == STATE_READY)) {
        return;
    }

    RTC_LOG(LS_WARNING)
    << ToString()
    << ": REFLECTOR sending ping to " << server_address_.address.ToString();

    if (server_address_.proto == webrtc::PROTO_TCP) {
        webrtc::ByteBufferWriter bufferWriter;
        bufferWriter.Write(std::span<const uint8_t>((const uint8_t *)peer_tag_.data(), peer_tag_.size()));
        bufferWriter.WriteUInt32(0);
        
        while (bufferWriter.Length() % 4 != 0) {
            bufferWriter.WriteUInt8(0);
        }
        
        webrtc::AsyncSocketPacketOptions options;
        Send(bufferWriter.Data(), bufferWriter.Length(), options);
    } else {
        webrtc::ByteBufferWriter bufferWriter;
        bufferWriter.Write(std::span<const uint8_t>((const uint8_t *)peer_tag_.data(), peer_tag_.size()));
        for (int i = 0; i < 12; i++) {
            bufferWriter.WriteUInt8(0xffu);
        }
        bufferWriter.WriteUInt8(0xfeu);
        for (int i = 0; i < 3; i++) {
            bufferWriter.WriteUInt8(0xffu);
        }
        bufferWriter.WriteUInt64(123);
        
        while (bufferWriter.Length() % 4 != 0) {
            bufferWriter.WriteUInt8(0);
        }
        
        webrtc::AsyncSocketPacketOptions options;
        Send(bufferWriter.Data(), bufferWriter.Length(), options);
    }

    if (!is_running_ping_task_) {
        is_running_ping_task_ = true;

        int timeoutMs = 10000;
        // Send pings faster until response arrives
        if (state_ == STATE_CONNECTED) {
            timeoutMs = 500;
        }

        thread()->PostDelayedTask(SafeTask(task_safety_.flag(), [this] {
            is_running_ping_task_ = false;
            SendReflectorHello();
        }), webrtc::TimeDelta::Millis(timeoutMs));
    }
}

bool ReflectorPort::CreateReflectorClientSocket() {
    RTC_DCHECK(!socket_ || SharedSocket());

    if (server_address_.proto == webrtc::PROTO_UDP && !SharedSocket()) {
        if (standaloneReflectorMode_ && Network()->name() == "shared-reflector-network") {
            const webrtc::IPAddress ipv4_any_address(INADDR_ANY);
            owned_socket_ = socket_factory()->CreateUdpSocket(env(), webrtc::SocketAddress(ipv4_any_address, 12345), min_port(), max_port());
        } else {
            owned_socket_ = socket_factory()->CreateUdpSocket(env(), webrtc::SocketAddress(Network()->GetBestIP(), 0), min_port(), max_port());
        }
        socket_ = owned_socket_.get();
    } else if (server_address_.proto == webrtc::PROTO_TCP) {
        RTC_DCHECK(!SharedSocket());
        owned_socket_ = CreateClientRawTcpSocket(
            underlying_socket_factory_,
            webrtc::SocketAddress(Network()->GetBestIP(), 0), server_address_.address);
        socket_ = owned_socket_.get();
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
        socket_->RegisterReceivedPacketCallback([this](webrtc::AsyncPacketSocket *socket, const webrtc::ReceivedIpPacket &packet) {
            this->OnReadPacket(socket, packet);
        });
    }

    socket_->SubscribeReadyToSend(this, [this, flag = task_safety_.flag()](webrtc::AsyncPacketSocket* socket) {
        if (flag->alive()) {
            OnReadyToSend(socket);
        }
    });

    socket_->SubscribeSentPacket(this, [this, flag = task_safety_.flag()](webrtc::AsyncPacketSocket* socket, const webrtc::SentPacketInfo& info) {
        if (flag->alive()) {
            OnSentPacket(socket, info);
        }
    });

    // TCP port is ready to send stun requests after the socket is connected,
    // while UDP port is ready to do so once the socket is created.
    if (server_address_.proto == webrtc::PROTO_TCP ||
        server_address_.proto == webrtc::PROTO_TLS) {
        socket_->SubscribeConnect(this, [this](webrtc::AsyncPacketSocket* socket) { OnSocketConnect(socket); });
        socket_->SubscribeCloseEvent(this, [this](webrtc::AsyncPacketSocket* socket, int error) { OnSocketClose(socket, error); });
    } else {
        state_ = STATE_CONNECTED;
    }
    return true;
}

void ReflectorPort::OnSocketConnect(webrtc::AsyncPacketSocket* socket) {
    // This slot should only be invoked if we're using a connection-oriented
    // protocol.
    RTC_DCHECK(server_address_.proto == webrtc::PROTO_TCP ||
               server_address_.proto == webrtc::PROTO_TLS);

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
    const webrtc::SocketAddress& socket_address = socket->GetLocalAddress();
    if (absl::c_none_of(Network()->GetIPs(),
                        [socket_address](const webrtc::InterfaceAddress& addr) {
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
                            webrtc::STUN_ERROR_GLOBAL_FAILURE,
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

    if (server_address_.proto == webrtc::PROTO_TCP && state_ != STATE_READY) {
        state_ = STATE_READY;

        RTC_LOG(LS_INFO)
        << ToString()
        << ": REFLECTOR " << server_address_.address.ToString() << " is now ready";
        
        const auto ipFormat = "reflector-" + std::to_string((uint32_t)serverId_) + "-" + std::to_string(randomTag_) + ".reflector";
        webrtc::SocketAddress candidateAddress(ipFormat, server_address_.address.port());
        if (standaloneReflectorMode_) {
            candidateAddress.SetResolvedIP(server_address_.address.ipaddr());
        }
        
        // For relayed candidate, Base is the candidate itself.
        AddAddress(candidateAddress,          // Candidate address.
                   server_address_.address,          // Base address.
                   webrtc::SocketAddress(),  // Related address.
                   webrtc::UDP_PROTOCOL_NAME,
                   ProtoToString(server_address_.proto),  // The first hop protocol.
                   "",  // TCP canddiate type, empty for turn candidates.
                   webrtc::IceCandidateType::kRelay, GetRelayPreference(server_address_.proto),
                   server_priority_, ReconstructedServerUrl(false /* use_hostname */),
                   true);
        
        SendReflectorHello();
    }
}

void ReflectorPort::OnSocketClose(webrtc::AsyncPacketSocket* socket, int error) {
    RTC_LOG(LS_WARNING) << ToString()
    << ": Connection with server failed with error: "
    << error;
    RTC_DCHECK(socket == socket_);
    //Close();
}

webrtc::Connection* ReflectorPort::CreateConnection(const webrtc::Candidate& remote_candidate,
                                                     CandidateOrigin origin) {
    // REFLECTOR-UDP can only connect to UDP candidates.
    if (!SupportsProtocol(remote_candidate.protocol())) {
        return nullptr;
    }

    auto remoteHostname = remote_candidate.address().hostname();
    if (remoteHostname.empty()) {
        return nullptr;
    }
    const auto ipFormat = "reflector-" + std::to_string((uint32_t)serverId_) + "-";
    if (!absl::StartsWith(remoteHostname, ipFormat) || !absl::EndsWith(remoteHostname, ".reflector")) {
        return nullptr;
    }
    if (remote_candidate.address().port() != server_address_.address.port()) {
        return nullptr;
    }

    if (state_ == STATE_DISCONNECTED || state_ == STATE_RECEIVEONLY) {
        return nullptr;
    }
    
    webrtc::Candidate updated_remote_candidate = remote_candidate;
    if (server_address_.proto == webrtc::PROTO_TCP || resolve_remote_candidate_ip_) {
        webrtc::SocketAddress updated_address = updated_remote_candidate.address();
        updated_address.SetResolvedIP(server_address_.address.ipaddr());
        updated_remote_candidate.set_address(updated_address);
    }

    if (GetConnection(updated_remote_candidate.address()) != nullptr) {
        return nullptr;
    }

    webrtc::ProxyConnection* conn = new webrtc::ProxyConnection(env(), NewWeakPtr(), 0, updated_remote_candidate);
    AddOrReplaceConnection(conn);

    return conn;
}

bool ReflectorPort::FailAndPruneConnection(const webrtc::SocketAddress& address) {
    webrtc::Connection* conn = GetConnection(address);
    if (conn != nullptr) {
        conn->FailAndPrune();
        return true;
    }
    return false;
}

int ReflectorPort::SetOption(webrtc::Socket::Option opt, int value) {
    // Remember the last requested DSCP value, for STUN traffic.
    if (opt == webrtc::Socket::OPT_DSCP)
        stun_dscp_value_ = static_cast<webrtc::DiffServCodePoint>(value);

    if (!socket_) {
        // If socket is not created yet, these options will be applied during socket
        // creation.
        socket_options_[opt] = value;
        return 0;
    }
    return socket_->SetOption(opt, value);
}

int ReflectorPort::GetOption(webrtc::Socket::Option opt, int* value) {
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

int ReflectorPort::SendTo(std::span<const uint8_t> payloadData,
                          const webrtc::SocketAddress& addr,
                          const webrtc::AsyncSocketPacketOptions& options,
                          bool payload) {
    const void *data = payloadData.data();
    const size_t size = payloadData.size();
    webrtc::CopyOnWriteBuffer targetPeerTag;

    auto syntheticHostname = addr.hostname();

    uint32_t resolvedPeerTag = 0;
    auto resolvedPeerTagIt = resolved_peer_tags_by_hostname_.find(syntheticHostname);
    if (resolvedPeerTagIt != resolved_peer_tags_by_hostname_.end()) {
        resolvedPeerTag = resolvedPeerTagIt->second;
    } else {
        const auto prefixFormat = "reflector-" + std::to_string((uint32_t)serverId_) + "-";
        std::string suffixFormat = ".reflector";
        if (!absl::StartsWith(syntheticHostname, prefixFormat) || !absl::EndsWith(syntheticHostname, suffixFormat)) {
            RTC_LOG(LS_ERROR) << ToString()
            << ": Discarding SendTo request with destination "
            << addr.ToString();

            return -1;
        }

        auto startPosition = prefixFormat.size();
        auto tagString = syntheticHostname.substr(startPosition, syntheticHostname.size() - suffixFormat.size() - startPosition);

        std::stringstream tagStringStream(tagString);
        tagStringStream >> resolvedPeerTag;

        if (resolvedPeerTag == 0) {
            RTC_LOG(LS_ERROR) << ToString()
            << ": Discarding SendTo request with destination "
            << addr.ToString() << " (could not parse peer tag)";

            return -1;
        }

        resolved_peer_tags_by_hostname_.insert(std::make_pair(syntheticHostname, resolvedPeerTag));
    }
    
    targetPeerTag.AppendData(peer_tag_.data(), peer_tag_.size() - 4);
    targetPeerTag.AppendData((uint8_t *)&resolvedPeerTag, 4);
    
    webrtc::ByteBufferWriter bufferWriter;
    bufferWriter.Write(std::span<const uint8_t>((const uint8_t *)targetPeerTag.data(), targetPeerTag.size()));
    
    bufferWriter.Write(std::span<const uint8_t>((const uint8_t *)&randomTag_, 4));
    
    bufferWriter.WriteUInt32((uint32_t)size);
    bufferWriter.Write(std::span<const uint8_t>((const uint8_t *)data, size));
    
    while (bufferWriter.Length() % 4 != 0) {
        bufferWriter.WriteUInt8(0);
    }
    
    webrtc::AsyncSocketPacketOptions modified_options(options);
    CopyPortInformationToPacketInfo(&modified_options.info_signaled_after_sent);
    
    modified_options.info_signaled_after_sent.turn_overhead_bytes = bufferWriter.Length() - size;
    
    Send(bufferWriter.Data(), bufferWriter.Length(), modified_options);
    
    return static_cast<int>(size);
}

bool ReflectorPort::CanHandleIncomingPacketsFrom(
                                                 const webrtc::SocketAddress& addr) const {
                                                     return server_address_.address == addr;
                                                 }

bool ReflectorPort::HandleIncomingPacket(webrtc::AsyncPacketSocket* socket, webrtc::ReceivedIpPacket const &packet) {
    if (socket != socket_) {
        // The packet was received on a shared socket after we've allocated a new
        // socket for this REFLECTOR port.
        return false;
    }
    
    uint8_t const *data = packet.payload().data();
    size_t size = packet.payload().size();
    webrtc::SocketAddress const &remote_addr = packet.source_address();
    auto packet_time_us = packet.arrival_time();
    
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

    if (memcmp(receivedPeerTag, peer_tag_.data(), 16 - 4) != 0) {
        RTC_LOG(LS_WARNING)
        << ToString()
        << ": Received REFLECTOR message with incorrect peer_tag";
        return false;
    }

    if (state_ != STATE_READY) {
        state_ = STATE_READY;

        RTC_LOG(LS_INFO)
        << ToString()
        << ": REFLECTOR " << server_address_.address.ToString() << " is now ready";
        
        const auto ipFormat = "reflector-" + std::to_string((uint32_t)serverId_) + "-" + std::to_string(randomTag_) + ".reflector";
        webrtc::SocketAddress candidateAddress(ipFormat, server_address_.address.port());
        if (standaloneReflectorMode_) {
            candidateAddress.SetResolvedIP(server_address_.address.ipaddr());
        }
        
        // For relayed candidate, Base is the candidate itself.
        AddAddress(candidateAddress,          // Candidate address.
                   server_address_.address,          // Base address.
                   webrtc::SocketAddress(),  // Related address.
                   webrtc::UDP_PROTOCOL_NAME,
                   ProtoToString(server_address_.proto),  // The first hop protocol.
                   "",  // TCP canddiate type, empty for turn candidates.
                   webrtc::IceCandidateType::kRelay, GetRelayPreference(server_address_.proto),
                   server_priority_, ReconstructedServerUrl(false /* use_hostname */),
                   true);
    }

    if (size > 16 + 4 + 4) {
        bool isSpecialPacket = false;
        if (size >= 16 + 12) {
            uint8_t specialTag[12];
            memcpy(specialTag, data + 16, 12);

            uint8_t expectedSpecialTag[12];
            memset(expectedSpecialTag, 0xff, 12);

            if (memcmp(specialTag, expectedSpecialTag, 12) == 0) {
                isSpecialPacket = true;
            }
        }

        if (!isSpecialPacket) {
            uint32_t senderTag = 0;
            memcpy(&senderTag, data + 16, 4);

            uint32_t dataSize = 0;
            memcpy(&dataSize, data + 16 + 4, 4);
            dataSize = be32toh(dataSize);
            if (dataSize > size - 16 - 4 - 4) {
                RTC_LOG(LS_WARNING)
                << ToString()
                << ": Received data packet with invalid size tag";
            } else {
                const auto ipFormat = "reflector-" + std::to_string((uint32_t)serverId_) + "-" + std::to_string(senderTag) + ".reflector";
                webrtc::SocketAddress candidateAddress(ipFormat, server_address_.address.port());
                candidateAddress.SetResolvedIP(server_address_.address.ipaddr());
                
                int64_t packet_timestamp = -1;
                if (packet_time_us.has_value()) {
                    packet_timestamp = packet_time_us->us_or(-1);
                }
                DispatchPacket(webrtc::ReceivedIpPacket::CreateFromLegacy(data + 16 + 4 + 4, dataSize, packet_timestamp, candidateAddress), webrtc::ProtocolType::PROTO_UDP);
            }
        }
    }

    return true;
}

void ReflectorPort::OnReadPacket(webrtc::AsyncPacketSocket* socket, webrtc::ReceivedIpPacket const &packet) {
    HandleIncomingPacket(socket, packet);
}

void ReflectorPort::OnSentPacket(webrtc::AsyncPacketSocket* socket,
                                 const webrtc::SentPacketInfo& sent_packet) {
    NotifySentPacket(sent_packet);
}

void ReflectorPort::OnReadyToSend(webrtc::AsyncPacketSocket* socket) {
    if (ready()) {
        Port::OnReadyToSend();
    }
}

bool ReflectorPort::SupportsProtocol(absl::string_view protocol) const {
    // Turn port only connects to UDP candidates.
    return protocol == webrtc::UDP_PROTOCOL_NAME;
}

void ReflectorPort::ResolveTurnAddress(const webrtc::SocketAddress& address) {
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
        if (result.GetError() != 0 && (server_address_.proto == webrtc::PROTO_TCP ||
                                       server_address_.proto == webrtc::PROTO_TLS)) {
            if (!CreateReflectorClientSocket()) {
                OnAllocateError(webrtc::STUN_ERROR_SERVER_NOT_REACHABLE,
                                "TURN host lookup received error.");
            }
            return;
        }

        // Copy the original server address in `resolved_address`. For TLS based
        // sockets we need hostname along with resolved address.
        webrtc::SocketAddress resolved_address = server_address_.address;
        if (result.GetError() != 0 ||
            !result.GetResolvedAddress(Network()->GetBestIP().family(),
                                       &resolved_address)) {
            RTC_LOG(LS_WARNING) << ToString() << ": TURN host lookup received error "
            << result.GetError();
            error_ = result.GetError();
            OnAllocateError(webrtc::STUN_ERROR_SERVER_NOT_REACHABLE,
                            "TURN host lookup received error.");
            return;
        }
        server_address_.address = resolved_address;
        PrepareAddress();
    });
}

void ReflectorPort::OnSendStunPacket(const void* data,
                                     size_t size,
                                     webrtc::StunRequest* request) {
    RTC_DCHECK(connected());
    webrtc::AsyncSocketPacketOptions options(StunDscpValue());
    options.info_signaled_after_sent.packet_type = webrtc::PacketType::kTurnMessage;
    CopyPortInformationToPacketInfo(&options.info_signaled_after_sent);
    if (Send(data, size, options) < 0) {
        RTC_LOG(LS_ERROR) << ToString() << ": Failed to send TURN message, error: "
        << socket_->GetError();
    }
}

void ReflectorPort::OnAllocateError(int error_code, const std::string& reason) {
    // We will send SignalPortError asynchronously as this can be sent during
    // port initialization. This way it will not be blocking other port
    // creation.
    thread()->PostTask(
      SafeTask(task_safety_.flag(), [this] { NotifyPortError(this); }));
    std::string address = GetLocalAddress().HostAsSensitiveURIString();
    int port = GetLocalAddress().port();
    if (server_address_.proto == webrtc::PROTO_TCP &&
        server_address_.address.IsPrivateIP()) {
        address.clear();
        port = 0;
    }
    SendCandidateError(webrtc::IceCandidateErrorEvent(address, port, ReconstructedServerUrl(true /* use_hostname */), error_code, reason));
}

void ReflectorPort::Release() {
    state_ = STATE_RECEIVEONLY;
}

void ReflectorPort::Close() {
    if (!ready()) {
        OnAllocateError(webrtc::STUN_ERROR_SERVER_NOT_REACHABLE, "");
    }
    // Stop the port from creating new connections.
    state_ = STATE_DISCONNECTED;
    // Delete all existing connections; stop sending data. Do NOT hand-roll this
    // loop: Connection::Destroy() erases from the same map connections()
    // returns, so iterating it directly invalidates the iterator mid-walk.
    DestroyAllConnections();
}

webrtc::DiffServCodePoint ReflectorPort::StunDscpValue() const {
    return stun_dscp_value_;
}

// static
bool ReflectorPort::AllowedReflectorPort(int port) {
    return true;
}

void ReflectorPort::DispatchPacket(webrtc::ReceivedIpPacket const &packet, webrtc::ProtocolType proto) {
    if (webrtc::Connection* conn = GetConnection(packet.source_address())) {
        conn->OnReadPacket(packet);
    } else {
        Port::OnReadPacket(packet, proto);
    }
}

int ReflectorPort::Send(const void* data,
                        size_t len,
                        const webrtc::AsyncSocketPacketOptions& options) {
    return socket_->SendTo(data, len, server_address_.address, options);
}

void ReflectorPort::HandleConnectionDestroyed(webrtc::Connection* conn) {
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
        case webrtc::PROTO_SSLTCP:
        case webrtc::PROTO_TLS:
            scheme = "turns";
            break;
        case webrtc::PROTO_UDP:
            transport = "udp";
            break;
        case webrtc::PROTO_TCP:
            break;
    }
    webrtc::StringBuilder url;
    url << scheme << ":"
    << (use_hostname ? server_address_.address.hostname()
        : server_address_.address.ipaddr().ToString())
    << ":" << server_address_.address.port() << "?transport=" << transport;
    return url.Release();
}

}  // namespace webrtc
