#include "v2/ReflectorRelayPortFactory.h"

#include "p2p/base/turn_port.h"

#include "v2/ReflectorPort.h"

namespace tgcalls {

ReflectorRelayPortFactory::ReflectorRelayPortFactory(std::vector<RtcServer> servers, bool standaloneReflectorMode, uint32_t standaloneReflectorRoleId, webrtc::SocketFactory *underlyingSocketFactory, bool resolveRemoteCandidateIp) :
_servers(servers),
_standaloneReflectorMode(standaloneReflectorMode),
_standaloneReflectorRoleId(standaloneReflectorRoleId),
_underlyingSocketFactory(underlyingSocketFactory),
_resolveRemoteCandidateIp(resolveRemoteCandidateIp) {
}

ReflectorRelayPortFactory::~ReflectorRelayPortFactory() {
}

std::unique_ptr<webrtc::Port> ReflectorRelayPortFactory::Create(webrtc::CreateRelayPortArgs&& args, webrtc::AsyncPacketSocket* udp_socket) {
    if (args.config->credentials.username == "reflector") {
        uint8_t id = 0;
        for (const auto &server : _servers) {
            webrtc::SocketAddress serverAddress(server.host, server.port);
            if (args.server_address->address == serverAddress) {
                id = server.id;
                break;
            }
        }

        if (id == 0) {
            return nullptr;
        }
        // Read before `args` is moved into the port.
        const int relativePriority = args.relative_priority;

        auto port = ReflectorPort::Create(std::move(args), _underlyingSocketFactory, udp_socket, id, relativePriority, _standaloneReflectorMode, _standaloneReflectorRoleId, _resolveRemoteCandidateIp);
        if (!port) {
            return nullptr;
        }
        return port;
    } else {
        // TurnPort::Create takes the TLS policy and logging id from args.config itself.
        return webrtc::TurnPort::Create(std::move(args), udp_socket);
    }
}

std::unique_ptr<webrtc::Port> ReflectorRelayPortFactory::Create(webrtc::CreateRelayPortArgs&& args, int min_port, int max_port) {
    if (args.config->credentials.username == "reflector") {
        uint8_t id = 0;
        for (const auto &server : _servers) {
            webrtc::SocketAddress serverAddress(server.host, server.port);
            if (args.server_address->address == serverAddress) {
                id = server.id;
                break;
            }
        }

        if (id == 0) {
            return nullptr;
        }
        // Read before `args` is moved into the port.
        const int relativePriority = args.relative_priority;

        auto port = ReflectorPort::Create(std::move(args), _underlyingSocketFactory, min_port, max_port, id, relativePriority, _standaloneReflectorMode, _standaloneReflectorRoleId, _resolveRemoteCandidateIp);
        if (!port) {
            return nullptr;
        }
        return port;
    } else {
        return webrtc::TurnPort::Create(std::move(args), min_port, max_port);
    }
}

} // namespace tgcalls
