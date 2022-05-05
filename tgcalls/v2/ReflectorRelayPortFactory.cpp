#include "v2/ReflectorRelayPortFactory.h"

#include "p2p/base/turn_port.h"

#include "v2/ReflectorPort.h"

namespace tgcalls {

ReflectorRelayPortFactory::ReflectorRelayPortFactory(std::vector<RtcServer> servers) :
_servers(servers) {
}

ReflectorRelayPortFactory::~ReflectorRelayPortFactory() {
}

std::unique_ptr<cricket::Port> ReflectorRelayPortFactory::Create(const cricket::CreateRelayPortArgs& args, rtc::AsyncPacketSocket* udp_socket) {
    if (args.config->credentials.username == "reflector") {
        uint8_t id = 0;
        for (const auto &server : _servers) {
            rtc::SocketAddress serverAddress(server.host, server.port);
            if (args.server_address->address == serverAddress) {
                id = server.id;
                break;
            }
        }

        if (id == 0) {
            return nullptr;
        }

        auto port = ReflectorPort::CreateUnique(
            args.network_thread, args.socket_factory, args.network, udp_socket,
            args.username, args.password, *args.server_address, id,
            args.config->credentials, args.config->priority);
        if (!port) {
            return nullptr;
        }
        return port;
    } else {
        auto port = cricket::TurnPort::CreateUnique(
            args.network_thread, args.socket_factory, args.network, udp_socket,
            args.username, args.password, *args.server_address,
            args.config->credentials, args.config->priority, args.turn_customizer);
        if (!port) {
            return nullptr;
        }
        port->SetTlsCertPolicy(args.config->tls_cert_policy);
        port->SetTurnLoggingId(args.config->turn_logging_id);
        return port;
    }
}

std::unique_ptr<cricket::Port> ReflectorRelayPortFactory::Create(const cricket::CreateRelayPortArgs& args, int min_port, int max_port) {
    if (args.config->credentials.username == "reflector") {
        uint8_t id = 0;
        for (const auto &server : _servers) {
            rtc::SocketAddress serverAddress(server.host, server.port);
            if (args.server_address->address == serverAddress) {
                id = server.id;
                break;
            }
        }

        if (id == 0) {
            return nullptr;
        }

        auto port = ReflectorPort::CreateUnique(
            args.network_thread, args.socket_factory, args.network, min_port,
            max_port, args.username, args.password, *args.server_address, id,
            args.config->credentials, args.config->priority);
        if (!port) {
            return nullptr;
        }
        return port;
    } else {
        auto port = cricket::TurnPort::CreateUnique(
            args.network_thread, args.socket_factory, args.network, min_port,
            max_port, args.username, args.password, *args.server_address,
            args.config->credentials, args.config->priority,
            args.config->tls_alpn_protocols, args.config->tls_elliptic_curves,
            args.turn_customizer, args.config->tls_cert_verifier);
        if (!port) {
            return nullptr;
        }
        port->SetTlsCertPolicy(args.config->tls_cert_policy);
        port->SetTurnLoggingId(args.config->turn_logging_id);
        return port;
    }
}

} // namespace tgcalls
