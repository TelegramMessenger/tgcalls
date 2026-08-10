#include "v2/NativeNetworkingImpl.h"

#include "p2p/base/basic_packet_socket_factory.h"
#include "p2p/client/basic_port_allocator.h"
#include "p2p/base/p2p_transport_channel.h"
#include "p2p/base/basic_async_resolver_factory.h"
#include "api/packet_socket_factory.h"
#include "rtc_base/rtc_certificate_generator.h"
#include "p2p/base/ice_credentials_iterator.h"
#include "api/jsep_ice_candidate.h"
#include "p2p/base/default_ice_transport_factory.h"
#include "p2p/dtls/dtls_transport.h"
#include "p2p/dtls/dtls_transport_factory.h"
#include "pc/dtls_srtp_transport.h"
#include "pc/dtls_transport.h"
#include "pc/jsep_transport_controller.h"
#include "api/async_dns_resolver.h"
#include "api/environment/environment_factory.h"
#include "api/ice_transport_interface.h"
#include "api/make_ref_counted.h"
#include "api/transport/ecn_marking.h"
#include "rtc_base/crypto_random.h"
#include "rtc_base/time_utils.h"

#include "TurnCustomizerImpl.h"
#include "ReflectorRelayPortFactory.h"
#include "SctpDataChannelProviderInterfaceImpl.h"
#include "StaticThreads.h"
#include "platform/PlatformInterface.h"
#include "p2p/base/turn_port.h"

#include "ReflectorPort.h"
#include "FieldTrialsConfig.h"
#include "EncryptedConnection.h"

namespace tgcalls {

namespace {

bool getCustomParameterBool(std::map<std::string, json11::Json> const &parameters, std::string const &name) {
    const auto value = parameters.find(name);
    if (value != parameters.end() && value->second.is_bool() && value->second.bool_value()) {
        return true;
    } else {
        return false;
    }
}

class WrappedBasicPacketSocketFactory : public webrtc::PacketSocketFactory {
public:
    WrappedBasicPacketSocketFactory(std::unique_ptr<webrtc::BasicPacketSocketFactory> &&impl, bool standaloneReflectorMode) :
    _impl(std::move(impl)),
    _standaloneReflectorMode(standaloneReflectorMode) {
    }

    virtual ~WrappedBasicPacketSocketFactory() {
    }

    virtual std::unique_ptr<webrtc::AsyncPacketSocket> CreateUdpSocket(const webrtc::Environment& env, const webrtc::SocketAddress& address, uint16_t min_port, uint16_t max_port) override {
        in_addr v4addr;
        inet_pton(AF_INET, "0.1.2.3", &v4addr);
        webrtc::IPAddress ipAddress(v4addr);
        if (_standaloneReflectorMode && address.ipaddr() == ipAddress && address.port() != 12345) {
            return nullptr;
        } else {
            webrtc::SocketAddress updatedAddress = address;
            if (updatedAddress.port() == 12345) {
                updatedAddress.SetPort(0);
            }
            return _impl->CreateUdpSocket(env, updatedAddress, min_port, max_port);
        }
    }
    
    virtual std::unique_ptr<webrtc::AsyncListenSocket> CreateServerTcpSocket(const webrtc::Environment& env, const webrtc::SocketAddress &local_address, uint16_t min_port, uint16_t max_port, int opts) override {
        in_addr v4addr;
        inet_pton(AF_INET, "0.1.2.3", &v4addr);
        webrtc::IPAddress ipAddress(v4addr);
        if (_standaloneReflectorMode && local_address.ipaddr() == ipAddress) {
            return nullptr;
        } else {
            return _impl->CreateServerTcpSocket(env, local_address, min_port, max_port, opts);
        }
    }

    virtual std::unique_ptr<webrtc::AsyncPacketSocket> CreateClientTcpSocket(const webrtc::Environment& env, const webrtc::SocketAddress &local_address, const webrtc::SocketAddress& remote_address, const webrtc::PacketSocketTcpOptions& tcp_options) override {
        in_addr v4addr;
        inet_pton(AF_INET, "0.1.2.3", &v4addr);
        webrtc::IPAddress ipAddress(v4addr);
        if (_standaloneReflectorMode && local_address.ipaddr() == ipAddress) {
            return nullptr;
        } else {
            return _impl->CreateClientTcpSocket(env, local_address, remote_address, tcp_options);
        }
    }

    virtual std::unique_ptr<webrtc::AsyncDnsResolverInterface> CreateAsyncDnsResolver() override {
        return _impl->CreateAsyncDnsResolver();
    }
private:
    std::unique_ptr<webrtc::BasicPacketSocketFactory> _impl;
    bool _standaloneReflectorMode = false;
};

class WrappedNetworkManager: public webrtc::NetworkManager {
public:
    WrappedNetworkManager(const webrtc::Environment &env, webrtc::NetworkMonitorFactory *networkMonitorFactory, webrtc::SocketFactory *socketFactory) {
        in_addr v4addr;
        inet_pton(AF_INET, "0.1.2.3", &v4addr);
        webrtc::IPAddress ipAddress(v4addr);
        _sharedReflectorNetwork = std::make_unique<webrtc::Network>(
            "shared-reflector-network",
            "shared-reflector-network",
            ipAddress,
            0,
            webrtc::AdapterType::ADAPTER_TYPE_UNKNOWN
        );
        _sharedReflectorNetwork->AddIP(ipAddress);
        
        _impl = std::make_unique<webrtc::BasicNetworkManager>(env, socketFactory, networkMonitorFactory);
        
        _impl->SubscribeNetworksChanged(this, [this]() {
            NotifyNetworksChanged();
        });
        _impl->SubscribeError(this, [this]() {
            NotifyError();
        });
    }
    
public:

    virtual void Initialize() override {
        _impl->Initialize();
    }

    virtual void StartUpdating() override {
        _impl->StartUpdating();
    }
    
    virtual void StopUpdating() override {
        _impl->StopUpdating();
    }

    virtual std::vector<const webrtc::Network *> GetNetworks() const override {
        //return _impl->GetNetworks();
        
        std::vector<const webrtc::Network *> result;
        result.push_back(_sharedReflectorNetwork.get());
        return result;
    }

    virtual EnumerationPermission enumeration_permission() const override {
        return _impl->enumeration_permission();
    }

    virtual std::vector<const webrtc::Network *> GetAnyAddressNetworks() override {
        return _impl->GetAnyAddressNetworks();
    }
    
    virtual void DumpNetworks() override {
        _impl->DumpNetworks();
    }
    
    bool GetDefaultLocalAddress(int family, webrtc::IPAddress *ipaddr) const override {
        return _impl->GetDefaultLocalAddress(family, ipaddr);
    }

    webrtc::MdnsResponderInterface* GetMdnsResponder() const override {
        return _impl->GetMdnsResponder();
    }

    virtual void set_vpn_list(const std::vector<webrtc::NetworkMask> &vpn) override {
        _impl->set_vpn_list(vpn);
    }
    
private:
    std::unique_ptr<webrtc::BasicNetworkManager> _impl;
    std::unique_ptr<webrtc::Network> _sharedReflectorNetwork;
};

class MtProtoPacketTransport : public webrtc::PacketTransportInternal {
public:
    MtProtoPacketTransport(
        webrtc::PacketTransportInternal *rawTransport,
        EncryptionKey encryptionKey
    ) :
    _rawTransport(rawTransport) {
        _rawTransport->SubscribeWritableState(this, [this](webrtc::PacketTransportInternal *transport) {
            InternalOnWritableState(transport);
        });
        _rawTransport->SubscribeReadyToSend(this, [this](webrtc::PacketTransportInternal *transport) {
            InternalOnReadyToSend(transport);
        });
        _rawTransport->SubscribeReceivingState(this, [this](webrtc::PacketTransportInternal *transport) {
            InternalOnReceivingState(transport);
        });
        _rawTransport->RegisterReceivedPacketCallback(this, [this](webrtc::PacketTransportInternal *transport, const webrtc::ReceivedIpPacket &packet) {
            InternalOnReadPacket(packet);
        });
        _rawTransport->SubscribeSentPacket(this, [this](webrtc::PacketTransportInternal *transport, const webrtc::SentPacketInfo &packet) {
            InternalOnSentPacket(packet);
        });
        _rawTransport->SubscribeNetworkRouteChanged(this, [this](std::optional<webrtc::NetworkRoute> route) {
            InternalOnNetworkRouteChanged(route);
        });
        _rawTransport->SetOnCloseCallback([this]() {
            InternalOnClosed();
        });
        
        _transportEncryption = std::make_unique<EncryptedConnection>(
            EncryptedConnection::Type::Transport,
            encryptionKey,
            [=](int delayMs, int cause) {
            }
        );
    }
    
    virtual ~MtProtoPacketTransport() {
        _rawTransport->UnsubscribeWritableState(this);
        _rawTransport->UnsubscribeReadyToSend(this);
        _rawTransport->UnsubscribeReceivingState(this);
        _rawTransport->DeregisterReceivedPacketCallback(this);
        _rawTransport->UnsubscribeSentPacket(this);
        _rawTransport->UnsubscribeNetworkRouteChanged(this);
        _rawTransport->SetOnCloseCallback(nullptr);
    }
    
    virtual const std::string& transport_name() const override {
        return _rawTransport->transport_name();
    }
    
    virtual bool writable() const override {
        return _rawTransport->writable();
    }
    
    virtual bool receiving() const override {
        return _rawTransport->receiving();
    }
    
    virtual int SendPacket(
        const char *data,
        size_t len,
        const webrtc::AsyncSocketPacketOptions &options,
        int flags
    ) override {
        if (flags != 0) {
            webrtc::CopyOnWriteBuffer buffer;
            buffer.AppendData((const unsigned char *)data, len);
            SendPacketInternal(buffer, options);
            return 0;
        } else {
            webrtc::CopyOnWriteBuffer buffer;
            uint32_t magic = 0xdcdcdcdc; // SCTP
            buffer.AppendData((const unsigned char *)&magic, 4);
            buffer.AppendData((const unsigned char *)data, len);
            SendPacketInternal(buffer, options);
            return 0;
        }
    }
    
    virtual int SetOption(webrtc::Socket::Option opt, int value) override {
        return _rawTransport->SetOption(opt, value);
    }
    
    virtual bool GetOption(webrtc::Socket::Option opt, int* value) override {
        return _rawTransport->GetOption(opt, value);
    }
    
    virtual int GetError() override {
        return _rawTransport->GetError();
    }
    
    virtual absl::optional<webrtc::NetworkRoute> network_route() const override {
        return _rawTransport->network_route();
    }
    
private:
    void InternalOnWritableState(PacketTransportInternal *transport) {
        NotifyWritableState(this);
    }
    
    void InternalOnReadyToSend(PacketTransportInternal *transport) {
        NotifyReadyToSend(this);
    }
    
    void InternalOnReceivingState(PacketTransportInternal *transport) {
        NotifyReceivingState(this);
    }
    
    void InternalOnReadPacket(const webrtc::ReceivedIpPacket &packet) {
        if (const auto result = _transportEncryption->handleIncomingRawPacket(reinterpret_cast<const char *>(packet.payload().data()), packet.payload().size())) {
            ProcessReadPacketInternal(result.value().main.message, packet.arrival_time().has_value() ? packet.arrival_time()->us() : 0);

            for (const auto &additional : result.value().additional) {
                ProcessReadPacketInternal(additional.message, packet.arrival_time().has_value() ? packet.arrival_time()->us() : 0);
            }
        }
    }
    
    void InternalOnSentPacket(const webrtc::SentPacketInfo &packet) {
        NotifySentPacket(this, packet);
    }
    
    void InternalOnNetworkRouteChanged(std::optional<webrtc::NetworkRoute> route) {
        NotifyNetworkRouteChanged(route);
    }
    
    void InternalOnClosed() {
        NotifyOnClose();
    }
    
private:
    void SendPacketInternal(webrtc::CopyOnWriteBuffer &packet, const webrtc::AsyncSocketPacketOptions &options) {
        if (const auto encryptedPacket = _transportEncryption->prepareForSendingRawMessage(packet, false)) {
            _rawTransport->SendPacket((const char *)encryptedPacket->bytes.data(), encryptedPacket->bytes.size(), options);
        }
    }
    
    void ProcessReadPacketInternal(webrtc::CopyOnWriteBuffer const &data, int64_t timestamp) {
        if (data.size() >= 4) {
            uint32_t header = 0;
            memcpy(&header, data.data(), 4);
            uint32_t magic = 0xdcdcdcdc; // SCTP
            if (header == magic) {
                NotifyPacketReceived(webrtc::ReceivedIpPacket::CreateFromLegacy((const char *)(data.data() + 4), data.size() - 4, timestamp));
            } else {
                NotifyPacketReceived(webrtc::ReceivedIpPacket::CreateFromLegacy((const char *)data.data(), data.size(), timestamp));
            }
        } else {
            NotifyPacketReceived(webrtc::ReceivedIpPacket::CreateFromLegacy((const char *)data.data(), data.size(), timestamp));
        }
    }
    
private:
    webrtc::PacketTransportInternal *_rawTransport = nullptr;
    std::unique_ptr<EncryptedConnection> _transportEncryption;
};

class MtProtoRtpTransport : public webrtc::RtpTransport {
public:
    explicit MtProtoRtpTransport(webrtc::IceTransportInternal *iceTransport, EncryptionKey encryptionKey) :
    webrtc::RtpTransport(true, fieldTrialsBasedConfig) {
        _packetTransport = std::make_unique<MtProtoPacketTransport>(iceTransport, encryptionKey);
        SetRtpPacketTransport(_packetTransport.get());
    }
    
    virtual bool IsSrtpActive() const override {
        return true;
    }
    
    virtual void OnWritableState(webrtc::PacketTransportInternal *packet_transport) override {
        webrtc::RtpTransport::OnWritableState(packet_transport);
        
        SignalWritableState(packet_transport);
    }
    
public:
    sigslot::signal1<webrtc::PacketTransportInternal *> SignalWritableState;
    sigslot::signal1<webrtc::PacketTransportInternal *> SignalReceivingState;
    
private:
    std::unique_ptr<MtProtoPacketTransport> _packetTransport;
};

}

InstanceNetworking::ConnectionDescription::CandidateDescription InstanceNetworking::connectionDescriptionFromCandidate(webrtc::Candidate const &candidate) {
    InstanceNetworking::ConnectionDescription::CandidateDescription result;
    
    result.type = std::string(webrtc::IceCandidateTypeToString(candidate.type()));
    result.protocol = candidate.protocol();
    result.address = candidate.address().ToString();
    
    return result;
}

webrtc::CryptoOptions NativeNetworkingImpl::getDefaulCryptoOptions() {
    auto options = webrtc::CryptoOptions();
    options.srtp.enable_aes128_sha1_80_crypto_cipher = true;
    options.srtp.enable_gcm_crypto_suites = true;
    return options;
}

NativeNetworkingImpl::NativeNetworkingImpl(Configuration &&configuration) :
_threads(std::move(configuration.threads)),
_isOutgoing(configuration.isOutgoing),
_encryptionKey(configuration.encryptionKey),
_enableStunMarking(configuration.enableStunMarking),
_enableTCP(configuration.enableTCP),
_enableP2P(configuration.enableP2P),
_rtcServers(configuration.rtcServers),
_proxy(configuration.proxy),
_customParameters(configuration.customParameters),
_env(webrtc::CreateEnvironment()),
_stateUpdated(std::move(configuration.stateUpdated)),
_candidateGathered(std::move(configuration.candidateGathered)),
_transportMessageReceived(std::move(configuration.transportMessageReceived)),
_rtcpPacketReceived(std::move(configuration.rtcpPacketReceived)),
_dataChannelStateUpdated(configuration.dataChannelStateUpdated),
_dataChannelMessageReceived(configuration.dataChannelMessageReceived) {
    assert(_threads->getNetworkThread()->IsCurrent());
    
    _localIceParameters = PeerIceParameters(webrtc::CreateRandomString(webrtc::ICE_UFRAG_LENGTH), webrtc::CreateRandomString(webrtc::ICE_PWD_LENGTH), true);
    
    _localCertificate = webrtc::RTCCertificateGenerator::GenerateCertificate(webrtc::KeyParams(webrtc::KT_ECDSA), absl::nullopt);
    
    _underlyingSocketFactory = _threads->getNetworkThread()->socketserver();
    
    _networkMonitorFactory = PlatformInterface::SharedInstance()->createNetworkMonitorFactory();
    if (getCustomParameterBool(_customParameters, "network_standalone_reflectors")) {
        _socketFactory = std::make_unique<WrappedBasicPacketSocketFactory>(std::make_unique<webrtc::BasicPacketSocketFactory>(_threads->getNetworkThread()->socketserver()), true);
        _networkManager = std::make_unique<WrappedNetworkManager>(_env, _networkMonitorFactory.get(), _threads->getNetworkThread()->socketserver());
    } else {
        _socketFactory = std::make_unique<webrtc::BasicPacketSocketFactory>(_threads->getNetworkThread()->socketserver());
        _networkManager = std::make_unique<webrtc::BasicNetworkManager>(_env, _threads->getNetworkThread()->socketserver(), _networkMonitorFactory.get());
    }
    
    _asyncResolverFactory = std::make_unique<webrtc::BasicAsyncDnsResolverFactory>();
    
    if (getCustomParameterBool(_customParameters, "network_use_mtproto")) {
        
    } else {
        _dtlsSrtpTransport = std::make_unique<webrtc::DtlsSrtpTransport>(true, fieldTrialsBasedConfig);
        _dtlsSrtpTransport->SetDtlsTransports(nullptr, nullptr);
        _dtlsSrtpTransport->SubscribeReadyToSend(this, [this](bool value) {
            this->DtlsReadyToSend(value);
        });
        _dtlsSrtpTransport->SubscribeRtcpPacketReceived(this, [this](webrtc::CopyOnWriteBuffer packet, std::optional<webrtc::Timestamp> arrivalTime, webrtc::EcnMarking ecn) {
            this->OnRtcpPacketReceived_n(&packet, arrivalTime.has_value() ? arrivalTime->us() : 0);
        });
    }
    resetDtlsSrtpTransport();
}

NativeNetworkingImpl::~NativeNetworkingImpl() {
    assert(_threads->getNetworkThread()->IsCurrent());

    RTC_LOG(LS_INFO) << "NativeNetworkingImpl::~NativeNetworkingImpl()";

    _mtProtoRtpTransport.reset();
    _dtlsSrtpTransport.reset();
    _dtlsTransport.reset();
    _dataChannelInterface.reset();
    _transportChannel = nullptr;
    _asyncResolverFactory.reset();
    _portAllocator.reset();
    _networkManager.reset();
    _underlyingSocketFactory = nullptr;
    _socketFactory.reset();
    _networkMonitorFactory.reset();
}

void NativeNetworkingImpl::resetDtlsSrtpTransport() {
    if (_enableStunMarking) {
        _turnCustomizer.reset(new TurnCustomizerImpl());
    }
    
    bool standaloneReflectorMode = getCustomParameterBool(_customParameters, "network_standalone_reflectors");
    
    uint32_t standaloneReflectorRoleId = 0;
    if (standaloneReflectorMode) {
        if (_isOutgoing) {
            standaloneReflectorRoleId = 1;
        } else {
            standaloneReflectorRoleId = 2;
        }
    }
    
    _relayPortFactory.reset(new ReflectorRelayPortFactory(_rtcServers, standaloneReflectorMode, standaloneReflectorRoleId, _underlyingSocketFactory));

    _portAllocator.reset(new webrtc::BasicPortAllocator(_env, _networkManager.get(), _socketFactory.get(), _turnCustomizer.get(), _relayPortFactory.get()));

    uint32_t flags = _portAllocator->flags();
    
    if (getCustomParameterBool(_customParameters, "network_use_default_route")) {
        flags |= webrtc::PORTALLOCATOR_DISABLE_ADAPTER_ENUMERATION;
    }
    
    if (getCustomParameterBool(_customParameters, "network_enable_shared_socket")) {
        flags |= webrtc::PORTALLOCATOR_ENABLE_SHARED_SOCKET;
    }
    
    flags |=
        webrtc::PORTALLOCATOR_ENABLE_IPV6 |
        webrtc::PORTALLOCATOR_ENABLE_IPV6_ON_WIFI;

    if (!_enableTCP) {
        flags |= webrtc::PORTALLOCATOR_DISABLE_TCP;
    }
    
    if (_proxy || !_enableP2P) {
        flags |= webrtc::PORTALLOCATOR_DISABLE_UDP;
        flags |= webrtc::PORTALLOCATOR_DISABLE_STUN;
        uint32_t candidateFilter = _portAllocator->candidate_filter();
        candidateFilter &= ~(webrtc::CF_REFLEXIVE);
        _portAllocator->SetCandidateFilter(candidateFilter);
    }
    
    _portAllocator->set_step_delay(webrtc::kMinimumStepDelay);

    _portAllocator->set_flags(flags);
    _portAllocator->Initialize();

    webrtc::ServerAddresses stunServers;
    std::vector<webrtc::RelayServerConfig> turnServers;

    for (auto &server : _rtcServers) {
        if (server.isTurn) {
            turnServers.push_back(webrtc::RelayServerConfig(
                webrtc::SocketAddress(server.host, server.port),
                server.login,
                server.password,
                server.isTcp ? webrtc::PROTO_TCP : webrtc::PROTO_UDP
            ));
        } else {
            webrtc::SocketAddress stunAddress = webrtc::SocketAddress(server.host, server.port);
            stunServers.insert(stunAddress);
        }
    }

    _portAllocator->SetConfiguration(stunServers, turnServers, 0, webrtc::NO_PRUNE, _turnCustomizer.get());

    webrtc::IceTransportInit iceTransportInit(_env);
    iceTransportInit.set_port_allocator(_portAllocator.get());
    iceTransportInit.set_async_dns_resolver_factory(_asyncResolverFactory.get());
    
    _transportChannel = webrtc::make_ref_counted<webrtc::DefaultIceTransport>(webrtc::P2PTransportChannel::Create("transport", 0, std::move(iceTransportInit)));

    webrtc::IceConfig iceConfig;
    iceConfig.continual_gathering_policy = webrtc::GATHER_CONTINUALLY;
    iceConfig.prioritize_most_likely_candidate_pairs = true;
    iceConfig.regather_on_failed_networks_interval = webrtc::kRegatherOnFailedNetworksInterval;
    if (getCustomParameterBool(_customParameters, "network_skip_initial_ping")) {
        iceConfig.presume_writable_when_fully_relayed = true;
    }
    _transportChannel->internal()->SetIceConfig(iceConfig);

    webrtc::IceParameters localIceParameters(
        _localIceParameters.ufrag,
        _localIceParameters.pwd,
        _localIceParameters.supportsRenomination
    );

    _transportChannel->internal()->SetIceParameters(localIceParameters);
    _transportChannel->internal()->SetIceRole(_isOutgoing ? webrtc::ICEROLE_CONTROLLING : webrtc::ICEROLE_CONTROLLED);
    _transportChannel->internal()->SetRemoteIceMode(webrtc::ICEMODE_FULL);

    _transportChannel->internal()->SubscribeCandidateGathered(this, [this](webrtc::IceTransportInternal *transport, const webrtc::Candidate &candidate) {
        this->candidateGathered(transport, candidate);
    });
    _transportChannel->internal()->SubscribeIceTransportStateChanged(this, [this](webrtc::IceTransportInternal *transport) {
        this->transportStateChanged(transport);
    });
    _transportChannel->internal()->SetCandidatePairChangeCallback([this](webrtc::CandidatePairChangeEvent const &event) {
        this->candidatePairChanged(event);
    });
    _transportChannel->internal()->SubscribeNetworkRouteChanged(this, [this](std::optional<webrtc::NetworkRoute> route) {
        this->transportRouteChanged(route);
    });

    if (getCustomParameterBool(_customParameters, "network_use_mtproto")) {
        _mtProtoRtpTransport = std::make_unique<MtProtoRtpTransport>(_transportChannel->internal(), _encryptionKey);
        
        ((MtProtoRtpTransport *)_mtProtoRtpTransport.get())->SignalWritableState.connect(this, &NativeNetworkingImpl::OnTransportWritableState_n);
        ((MtProtoRtpTransport *)_mtProtoRtpTransport.get())->SignalReceivingState.connect(this, &NativeNetworkingImpl::OnTransportReceivingState_n);
        
        _mtProtoRtpTransport->SubscribeReadyToSend(this, [this](bool value) {
            this->DtlsReadyToSend(value);
        });
        _mtProtoRtpTransport->SubscribeRtcpPacketReceived(this, [this](webrtc::CopyOnWriteBuffer packet, std::optional<webrtc::Timestamp> arrivalTime, webrtc::EcnMarking ecn) {
            this->OnRtcpPacketReceived_n(&packet, arrivalTime.has_value() ? arrivalTime->us() : 0);
        });
    } else {
        webrtc::CryptoOptions cryptoOptions = NativeNetworkingImpl::getDefaulCryptoOptions();
        _dtlsTransport.reset(new webrtc::DtlsTransportInternalImpl(webrtc::CreateEnvironment(), _transportChannel, cryptoOptions));
        
        _dtlsTransport->SubscribeWritableState(this, [this](webrtc::PacketTransportInternal *transport) {
            this->OnTransportWritableState_n(transport);
        });
        _dtlsTransport->SubscribeReceivingState(this, [this](webrtc::PacketTransportInternal *transport) {
            this->OnTransportReceivingState_n(transport);
        });
        
        _dtlsTransport->SetLocalCertificate(_localCertificate);
        
        _dtlsSrtpTransport->SetDtlsTransports(_dtlsTransport.get(), nullptr);
    }
}

void NativeNetworkingImpl::start() {
    _transportChannel->internal()->MaybeStartGathering();

    webrtc::PacketTransportInternal *sctpPacketTransport = nullptr;
    if (_mtProtoRtpTransport) {
        sctpPacketTransport = _mtProtoRtpTransport->rtp_packet_transport();
    } else {
        sctpPacketTransport = _dtlsTransport.get();
    }
    
    const auto weak = std::weak_ptr<NativeNetworkingImpl>(shared_from_this());
    _dataChannelInterface.reset(new SctpDataChannelProviderInterfaceImpl(
        sctpPacketTransport,
        _isOutgoing,
        [weak, threads = _threads](bool state) {
            assert(threads->getNetworkThread()->IsCurrent());
            const auto strong = weak.lock();
            if (!strong) {
                return;
            }
            strong->_dataChannelStateUpdated(state);
        },
        [weak, threads = _threads]() {
            assert(threads->getNetworkThread()->IsCurrent());
            const auto strong = weak.lock();
            if (!strong) {
                return;
            }
            //strong->restartDataChannel();
        },
        [weak, threads = _threads](std::string const &message) {
            assert(threads->getNetworkThread()->IsCurrent());
            const auto strong = weak.lock();
            if (!strong) {
                return;
            }
            strong->_dataChannelMessageReceived(message);
        },
        _threads
    ));
    
    _lastDisconnectedTimestamp = webrtc::TimeMillis();
    checkConnectionTimeout();
}

void NativeNetworkingImpl::stop() {
    _transportChannel->internal()->UnsubscribeNetworkRouteChanged(this);
    
    _dataChannelInterface.reset();
    
    if (_dtlsTransport) {
        _dtlsTransport->UnsubscribeWritableState(this);
        _dtlsTransport->UnsubscribeReceivingState(this);
    }
    if (_dtlsSrtpTransport) {
        _dtlsSrtpTransport->SetDtlsTransports(nullptr, nullptr);
    }
    
    if (_mtProtoRtpTransport) {
        ((MtProtoRtpTransport *)_mtProtoRtpTransport.get())->SignalWritableState.disconnect(this);
        ((MtProtoRtpTransport *)_mtProtoRtpTransport.get())->SignalReceivingState.disconnect(this);
        _mtProtoRtpTransport.reset();
    }
    
    _dtlsTransport.reset();
    _transportChannel = nullptr;
    _portAllocator.reset();
    
    _localIceParameters = PeerIceParameters(webrtc::CreateRandomString(webrtc::ICE_UFRAG_LENGTH), webrtc::CreateRandomString(webrtc::ICE_PWD_LENGTH), true);
    
    _localCertificate = webrtc::RTCCertificateGenerator::GenerateCertificate(webrtc::KeyParams(webrtc::KT_ECDSA), absl::nullopt);
}

PeerIceParameters NativeNetworkingImpl::getLocalIceParameters() {
    return _localIceParameters;
}

std::unique_ptr<webrtc::SSLFingerprint> NativeNetworkingImpl::getLocalFingerprint() {
    auto certificate = _localCertificate;
    if (!certificate) {
        return nullptr;
    }
    return webrtc::SSLFingerprint::CreateFromCertificate(*certificate);
}

void NativeNetworkingImpl::setRemoteParams(PeerIceParameters const &remoteIceParameters, webrtc::SSLFingerprint *fingerprint, std::string const &sslSetup) {
    _remoteIceParameters = remoteIceParameters;

    webrtc::IceParameters parameters(
        remoteIceParameters.ufrag,
        remoteIceParameters.pwd,
        remoteIceParameters.supportsRenomination
    );

    _transportChannel->internal()->SetRemoteIceParameters(parameters);

    webrtc::SSLRole sslRole;
    if (sslSetup == "active") {
        sslRole = webrtc::SSLRole::SSL_SERVER;
    } else if (sslSetup == "passive") {
        sslRole = webrtc::SSLRole::SSL_CLIENT;
    } else {
        sslRole = _isOutgoing ? webrtc::SSLRole::SSL_CLIENT : webrtc::SSLRole::SSL_SERVER;
    }

    if (fingerprint) {
        if (_dtlsTransport) {
            _dtlsTransport->SetRemoteParameters(fingerprint->algorithm, fingerprint->digest.data(), fingerprint->digest.size(), sslRole);
        }
    }
    
    processPendingLocalStandaloneReflectorCandidates();
}

void NativeNetworkingImpl::addCandidates(std::vector<webrtc::Candidate> const &candidates) {
    bool standaloneReflectorMode = getCustomParameterBool(_customParameters, "network_standalone_reflectors");
    
    for (const auto &candidate : candidates) {
        if (standaloneReflectorMode) {
            if (absl::EndsWith(candidate.address().hostname(), ".reflector")) {
                continue;
            }
        }
        
        _transportChannel->internal()->AddRemoteCandidate(candidate);
    }
}

void NativeNetworkingImpl::sendDataChannelMessage(std::string const &message) {
    if (_dataChannelInterface) {
        _dataChannelInterface->sendDataChannelMessage(message);
    }
}

webrtc::RtpTransport *NativeNetworkingImpl::getRtpTransport() {
    if (_mtProtoRtpTransport) {
        return _mtProtoRtpTransport.get();
    } else {
        return _dtlsSrtpTransport.get();
    }
}

void NativeNetworkingImpl::checkConnectionTimeout() {
    const auto weak = std::weak_ptr<NativeNetworkingImpl>(shared_from_this());
    _threads->getNetworkThread()->PostDelayedTask([weak]() {
        auto strong = weak.lock();
        if (!strong) {
            return;
        }

        int64_t currentTimestamp = webrtc::TimeMillis();
        const int64_t maxTimeout = 20000;

        if (!strong->_isConnected && strong->_lastDisconnectedTimestamp + maxTimeout < currentTimestamp) {
            RTC_LOG(LS_INFO) << "NativeNetworkingImpl timeout " << (currentTimestamp - strong->_lastDisconnectedTimestamp) << " ms";
            
            strong->_isFailed = true;
            strong->notifyStateUpdated();
        }

        strong->checkConnectionTimeout();
    }, webrtc::TimeDelta::Millis(1000));
}

void NativeNetworkingImpl::candidateGathered(webrtc::IceTransportInternal *transport, const webrtc::Candidate &candidate) {
    assert(_threads->getNetworkThread()->IsCurrent());

    bool standaloneReflectorMode = getCustomParameterBool(_customParameters, "network_standalone_reflectors");
    if (standaloneReflectorMode && absl::EndsWith(candidate.address().hostname(), ".reflector")) {
        _pendingLocalStandaloneReflectorCandidates.push_back(candidate);
        
        if (_remoteIceParameters) {
            processPendingLocalStandaloneReflectorCandidates();
        }
    } else {
        _candidateGathered(candidate);
    }
}

void NativeNetworkingImpl::processPendingLocalStandaloneReflectorCandidates() {
    if (!_remoteIceParameters) {
        return;
    }
    
    auto candidates = _pendingLocalStandaloneReflectorCandidates;
    _pendingLocalStandaloneReflectorCandidates.clear();
    
    for (const auto &candidate : candidates) {
        auto remoteHostname = candidate.address().hostname();
        if (!remoteHostname.empty()) {
            uint32_t standaloneReflectorRoleId = 0;
            if (_isOutgoing) {
                standaloneReflectorRoleId = 1;
            } else {
                standaloneReflectorRoleId = 2;
            }
            
            std::string prefixFormat = "reflector-";
            std::string suffixFormat = "-" + std::to_string(standaloneReflectorRoleId) + ".reflector";
            if (!absl::StartsWith(remoteHostname, prefixFormat) || !absl::EndsWith(remoteHostname, suffixFormat)) {
                return;
            }
            
            auto startPosition = prefixFormat.size();
            auto tagString = remoteHostname.substr(startPosition, remoteHostname.size() - suffixFormat.size() - startPosition);
            
            std::stringstream tagStringStream(tagString);
            
            uint32_t resolvedServerId = 0;
            tagStringStream >> resolvedServerId;
            
            uint32_t remoteReflectorRoleId = 0;
            if (!_isOutgoing) {
                remoteReflectorRoleId = 1;
            } else {
                remoteReflectorRoleId = 2;
            }
            
            if (resolvedServerId != 0) {
                webrtc::Candidate remoteCandidate = candidate;
                webrtc::SocketAddress address = remoteCandidate.address();
                const auto remoteHost = "reflector-" + std::to_string(resolvedServerId) + "-" + std::to_string(remoteReflectorRoleId) + ".reflector";
                address.SetIP(remoteHost);
                address.SetResolvedIP(remoteCandidate.address().ipaddr());
                remoteCandidate.set_address(address);
                remoteCandidate.set_username(_remoteIceParameters->ufrag);
                remoteCandidate.set_password(_remoteIceParameters->pwd);
                _transportChannel->internal()->AddRemoteCandidate(remoteCandidate);
            }
        }
    }
}

void NativeNetworkingImpl::candidateGatheringState(webrtc::IceTransportInternal *transport) {
    assert(_threads->getNetworkThread()->IsCurrent());
}

void NativeNetworkingImpl::OnTransportWritableState_n(webrtc::PacketTransportInternal *transport) {
    assert(_threads->getNetworkThread()->IsCurrent());

    UpdateAggregateStates_n();
}
void NativeNetworkingImpl::OnTransportReceivingState_n(webrtc::PacketTransportInternal *transport) {
    assert(_threads->getNetworkThread()->IsCurrent());

    UpdateAggregateStates_n();
}

void NativeNetworkingImpl::DtlsReadyToSend(bool isReadyToSend) {
    UpdateAggregateStates_n();

    if (isReadyToSend) {
        const auto weak = std::weak_ptr<NativeNetworkingImpl>(shared_from_this());
        _threads->getNetworkThread()->PostTask([weak]() {
            const auto strong = weak.lock();
            if (!strong) {
                return;
            }
            strong->UpdateAggregateStates_n();
        });
    }
}

void NativeNetworkingImpl::transportStateChanged(webrtc::IceTransportInternal *transport) {
    UpdateAggregateStates_n();
}

void NativeNetworkingImpl::transportReadyToSend(webrtc::IceTransportInternal *transport) {
    assert(_threads->getNetworkThread()->IsCurrent());
}

void NativeNetworkingImpl::transportRouteChanged(absl::optional<webrtc::NetworkRoute> route) {
    assert(_threads->getNetworkThread()->IsCurrent());
    
    if (route.has_value()) {
        /*webrtc::IceTransportStats iceTransportStats;
        if (_transportChannel->internal()->GetStats(&iceTransportStats)) {
        }*/
        
        RTC_LOG(LS_INFO) << "NativeNetworkingImpl route changed: " << route->DebugString();
        
        bool localIsWifi = route->local.adapter_type() == webrtc::AdapterType::ADAPTER_TYPE_WIFI;
        bool remoteIsWifi = route->remote.adapter_type() == webrtc::AdapterType::ADAPTER_TYPE_WIFI;
        
        RTC_LOG(LS_INFO) << "NativeNetworkingImpl is wifi: local=" << localIsWifi << ", remote=" << remoteIsWifi;
        
        std::string localDescription = route->local.uses_turn() ? "turn" : "p2p";
        std::string remoteDescription = route->remote.uses_turn() ? "turn" : "p2p";
        
        RouteDescription routeDescription(localDescription, remoteDescription);
        
        if (!_currentRouteDescription || routeDescription != _currentRouteDescription.value()) {
            _currentRouteDescription = std::move(routeDescription);
            notifyStateUpdated();
        }
    }
}

void NativeNetworkingImpl::candidatePairChanged(webrtc::CandidatePairChangeEvent const &event) {
    ConnectionDescription connectionDescription;
    
    connectionDescription.local = InstanceNetworking::connectionDescriptionFromCandidate(event.selected_candidate_pair.local);
    connectionDescription.remote = InstanceNetworking::connectionDescriptionFromCandidate(event.selected_candidate_pair.remote);
    
    if (!_currentConnectionDescription || _currentConnectionDescription.value() != connectionDescription) {
        _currentConnectionDescription = std::move(connectionDescription);
        notifyStateUpdated();
    }
}

void NativeNetworkingImpl::RtpPacketReceived_n(webrtc::CopyOnWriteBuffer *packet, int64_t packet_time_us, bool isUnresolved) {
    if (_transportMessageReceived) {
        _transportMessageReceived(*packet, isUnresolved);
    }
}

void NativeNetworkingImpl::OnRtcpPacketReceived_n(webrtc::CopyOnWriteBuffer *packet, int64_t packet_time_us) {
    if (_rtcpPacketReceived) {
        _rtcpPacketReceived(*packet, packet_time_us);
    }
}

void NativeNetworkingImpl::UpdateAggregateStates_n() {
    assert(_threads->getNetworkThread()->IsCurrent());

    auto state = _transportChannel->internal()->GetIceTransportState();
    bool isConnected = false;
    switch (state) {
        case webrtc::IceTransportState::kConnected:
        case webrtc::IceTransportState::kCompleted:
            isConnected = true;
            break;
        default:
            break;
    }

    if (_mtProtoRtpTransport) {
        if (!_mtProtoRtpTransport->IsWritable(false)) {
            isConnected = false;
        }
    } else {
        if (!_dtlsSrtpTransport->IsWritable(false)) {
            isConnected = false;
        }
    }

    if (_isConnected != isConnected) {
        _isConnected = isConnected;
        
        if (!isConnected) {
            _lastDisconnectedTimestamp = webrtc::TimeMillis();
        }

        notifyStateUpdated();

        if (_dataChannelInterface) {
            _dataChannelInterface->updateIsConnected(isConnected);
        }
    }
}

void NativeNetworkingImpl::notifyStateUpdated() {
    NativeNetworkingImpl::State emitState;
    emitState.isReadyToSendData = _isConnected;
    emitState.route = _currentRouteDescription;
    emitState.connection = _currentConnectionDescription;
    emitState.isFailed = _isFailed;
    _stateUpdated(emitState);
}

void NativeNetworkingImpl::sctpReadyToSendData() {
}

} // namespace tgcalls
