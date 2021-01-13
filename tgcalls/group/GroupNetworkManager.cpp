#include "group/GroupNetworkManager.h"

#include "p2p/base/basic_packet_socket_factory.h"
#include "p2p/client/basic_port_allocator.h"
#include "p2p/base/p2p_transport_channel.h"
#include "p2p/base/basic_async_resolver_factory.h"
#include "api/packet_socket_factory.h"
#include "rtc_base/task_utils/to_queued_task.h"
#include "p2p/base/ice_credentials_iterator.h"
#include "api/jsep_ice_candidate.h"

#include "StaticThreads.h"

namespace tgcalls {

class TurnCustomizerImpl : public webrtc::TurnCustomizer {
public:
    TurnCustomizerImpl() {
    }
    
    virtual ~TurnCustomizerImpl() {
    }
    
    void MaybeModifyOutgoingStunMessage(cricket::PortInterface* port,
                                        cricket::StunMessage* message) override {
        message->AddAttribute(std::make_unique<cricket::StunByteStringAttribute>(cricket::STUN_ATTR_SOFTWARE, "Telegram "));
    }
    
    bool AllowChannelData(cricket::PortInterface* port, const void *data, size_t size, bool payload) override {
        return true;
    }
};

GroupNetworkManager::GroupNetworkManager(
    std::function<void(const State &)> stateUpdated,
    std::function<void(rtc::CopyOnWriteBuffer const &)> transportMessageReceived) :
_stateUpdated(std::move(stateUpdated)),
_transportMessageReceived(std::move(transportMessageReceived)),
_localIceParameters(rtc::CreateRandomString(cricket::ICE_UFRAG_LENGTH), rtc::CreateRandomString(cricket::ICE_PWD_LENGTH)) {
    assert(StaticThreads::getNetworkThread()->IsCurrent());
}

GroupNetworkManager::~GroupNetworkManager() {
    assert(StaticThreads::getNetworkThread()->IsCurrent());
    
    RTC_LOG(LS_INFO) << "GroupNetworkManager::~GroupNetworkManager()";

    _transportChannel.reset();
    _asyncResolverFactory.reset();
    _portAllocator.reset();
    _networkManager.reset();
    _socketFactory.reset();
}

void GroupNetworkManager::start() {
    _socketFactory.reset(new rtc::BasicPacketSocketFactory(StaticThreads::getNetworkThread()));

    _networkManager = std::make_unique<rtc::BasicNetworkManager>();
    
    /*if (_enableStunMarking) {
        _turnCustomizer.reset(new TurnCustomizerImpl());
    }*/
    
    _portAllocator.reset(new cricket::BasicPortAllocator(_networkManager.get(), _socketFactory.get(), _turnCustomizer.get(), nullptr));

    uint32_t flags = 0;
    //flags |= cricket::PORTALLOCATOR_DISABLE_TCP;
    /*if (!_enableP2P) {
        flags |= cricket::PORTALLOCATOR_DISABLE_UDP;
        flags |= cricket::PORTALLOCATOR_DISABLE_STUN;
    }*/
    
    _portAllocator->set_flags(_portAllocator->flags() | flags);
    _portAllocator->Initialize();

    cricket::ServerAddresses stunServers;
    std::vector<cricket::RelayServerConfig> turnServers;

    /*for (auto &server : _rtcServers) {
        if (server.isTurn) {
            turnServers.push_back(cricket::RelayServerConfig(
                rtc::SocketAddress(server.host, server.port),
                server.login,
                server.password,
                cricket::PROTO_UDP
            ));
        } else {
            rtc::SocketAddress stunAddress = rtc::SocketAddress(server.host, server.port);
            stunServers.insert(stunAddress);
        }
    }*/

    _portAllocator->SetConfiguration(stunServers, turnServers, 2, webrtc::NO_PRUNE, _turnCustomizer.get());

    _asyncResolverFactory = std::make_unique<webrtc::BasicAsyncResolverFactory>();
    _transportChannel.reset(new cricket::P2PTransportChannel("transport", 0, _portAllocator.get(), _asyncResolverFactory.get(), nullptr));

    cricket::IceConfig iceConfig;
    iceConfig.continual_gathering_policy = cricket::GATHER_ONCE;
    iceConfig.prioritize_most_likely_candidate_pairs = true;
    iceConfig.regather_on_failed_networks_interval = 8000;
    _transportChannel->SetIceConfig(iceConfig);

    cricket::IceParameters localIceParameters(
        _localIceParameters.ufrag,
        _localIceParameters.pwd,
        false
    );

    _transportChannel->SetIceParameters(localIceParameters);
    const bool isOutgoing = false;
    _transportChannel->SetIceRole(isOutgoing ? cricket::ICEROLE_CONTROLLING : cricket::ICEROLE_CONTROLLED);

    //_transportChannel->SignalCandidateGathered.connect(this, &GroupNetworkManager::candidateGathered);
    //_transportChannel->SignalGatheringState.connect(this, &GroupNetworkManager::candidateGatheringState);
    _transportChannel->SignalIceTransportStateChanged.connect(this, &GroupNetworkManager::transportStateChanged);
    _transportChannel->SignalReadPacket.connect(this, &GroupNetworkManager::transportPacketReceived);

    _transportChannel->MaybeStartGathering();

    _transportChannel->SetRemoteIceMode(cricket::ICEMODE_LITE);
}

PeerIceParameters GroupNetworkManager::getLocalIceParameters() {
    return _localIceParameters;
}

void GroupNetworkManager::setRemoteParams(PeerIceParameters const &remoteIceParameters, std::vector<cricket::Candidate> const &iceCandidates) {
    _remoteIceParameters = remoteIceParameters;

    cricket::IceParameters parameters(
        remoteIceParameters.ufrag,
        remoteIceParameters.pwd,
        false
    );

    _transportChannel->SetRemoteIceParameters(parameters);

    for (const auto &candidate : iceCandidates) {
        _transportChannel->AddRemoteCandidate(candidate);
    }
}

void GroupNetworkManager::sendMessage(rtc::CopyOnWriteBuffer const &message) {
    rtc::PacketOptions packetOptions;
    _transportChannel->SendPacket((const char *)message.data(), message.size(), packetOptions, 0);
}

void GroupNetworkManager::checkConnectionTimeout() {
    const auto weak = std::weak_ptr<GroupNetworkManager>(shared_from_this());
    StaticThreads::getNetworkThread()->PostDelayedTask(RTC_FROM_HERE, [weak]() {
        auto strong = weak.lock();
        if (!strong) {
            return;
        }
        
        int64_t currentTimestamp = rtc::TimeMillis();
        const int64_t maxTimeout = 20000;
        
        if (strong->_lastNetworkActivityMs + maxTimeout < currentTimestamp) {
            GroupNetworkManager::State emitState;
            emitState.isReadyToSendData = false;
            emitState.isFailed = true;
            strong->_stateUpdated(emitState);
        }
        
        strong->checkConnectionTimeout();
    }, 1000);
}

void GroupNetworkManager::candidateGathered(cricket::IceTransportInternal *transport, const cricket::Candidate &candidate) {
    assert(StaticThreads::getNetworkThread()->IsCurrent());
}

void GroupNetworkManager::candidateGatheringState(cricket::IceTransportInternal *transport) {
    assert(StaticThreads::getNetworkThread()->IsCurrent());
}

void GroupNetworkManager::transportStateChanged(cricket::IceTransportInternal *transport) {
    assert(StaticThreads::getNetworkThread()->IsCurrent());

    auto state = transport->GetIceTransportState();
    bool isConnected = false;
    switch (state) {
        case webrtc::IceTransportState::kConnected:
        case webrtc::IceTransportState::kCompleted:
            isConnected = true;
            break;
        default:
            break;
    }
    GroupNetworkManager::State emitState;
    emitState.isReadyToSendData = isConnected;
    _stateUpdated(emitState);
    
    if (_isConnected != isConnected) {
        _isConnected = isConnected;
    }
}

void GroupNetworkManager::transportReadyToSend(cricket::IceTransportInternal *transport) {
    assert(StaticThreads::getNetworkThread()->IsCurrent());
}

void GroupNetworkManager::transportPacketReceived(rtc::PacketTransportInternal *transport, const char *bytes, size_t size, const int64_t &timestamp, int unused) {
    assert(StaticThreads::getNetworkThread()->IsCurrent());
    
    _lastNetworkActivityMs = rtc::TimeMillis();

    if (_transportMessageReceived) {
        rtc::CopyOnWriteBuffer buffer;
        buffer.AppendData(bytes, size);
        _transportMessageReceived(buffer);
    }
}

} // namespace tgcalls
