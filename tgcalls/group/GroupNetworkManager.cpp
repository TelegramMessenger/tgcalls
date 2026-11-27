#include "group/GroupNetworkManager.h"

#include "p2p/base/basic_packet_socket_factory.h"
#include "p2p/client/basic_port_allocator.h"
#include "p2p/base/p2p_transport_channel.h"
#include "p2p/base/basic_async_resolver_factory.h"
#include "api/packet_socket_factory.h"
#include "rtc_base/rtc_certificate_generator.h"
#include "p2p/base/ice_credentials_iterator.h"
#include "api/jsep_ice_candidate.h"
#include "p2p/dtls/dtls_transport.h"
#include "p2p/dtls/dtls_transport_factory.h"
#include "pc/dtls_srtp_transport.h"
#include "pc/dtls_transport.h"
#include "pc/ice_transport.h"
#include "rtc_base/crypto_random.h"
#include "rtc_base/time_utils.h"
#include "modules/rtp_rtcp/source/rtp_util.h"
#include "modules/rtp_rtcp/source/byte_io.h"
#include "platform/PlatformInterface.h"
#include "TurnCustomizerImpl.h"
#include "SctpDataChannelProviderInterfaceImpl.h"
#include "StaticThreads.h"
#include "call/rtp_packet_sink_interface.h"
#include "modules/rtp_rtcp/source/rtp_packet_received.h"
#include "modules/rtp_rtcp/source/rtp_header_extensions.h"

namespace tgcalls {

enum {
    kRtcpExpectedVersion = 2,
    kRtcpMinHeaderLength = 4,
    kRtcpMinParseLength = 8,

    kRtpExpectedVersion = 2,
    kRtpMinParseLength = 12
};

static void updateHeaderWithVoiceActivity(webrtc::CopyOnWriteBuffer *packet, const uint8_t* ptrRTPDataExtensionEnd, const uint8_t* ptr, bool voiceActivity, bool zeroAudioLevel) {
    while (ptrRTPDataExtensionEnd - ptr > 0) {
        //  0
        //  0 1 2 3 4 5 6 7
        // +-+-+-+-+-+-+-+-+
        // |  ID   |  len  |
        // +-+-+-+-+-+-+-+-+

        // Note that 'len' is the header extension element length, which is the
        // number of bytes - 1.
        const int id = (*ptr & 0xf0) >> 4;
        const int len = (*ptr & 0x0f);
        ptr++;

        if (id == 0) {
            // Padding byte, skip ignoring len.
            continue;
        }

        if (id == 15) {
            RTC_LOG(LS_VERBOSE)
            << "RTP extension header 15 encountered. Terminate parsing.";
            return;
        }

        if (ptrRTPDataExtensionEnd - ptr < (len + 1)) {
            RTC_LOG(LS_WARNING) << "Incorrect one-byte extension len: " << (len + 1)
            << ", bytes left in buffer: "
            << (ptrRTPDataExtensionEnd - ptr);
            return;
        }

        if (id == 1) { // kAudioLevelUri
            uint8_t audioLevel = ptr[0] & 0x7f;
            if (zeroAudioLevel) {
                if (audioLevel < 47) {
                    audioLevel = 0;
                } else if (audioLevel < 107) {
                    audioLevel = 106;
                } else {
                    audioLevel = 127;
                }
            }
            bool parsedVoiceActivity = (ptr[0] & 0x80) != 0;

            if (parsedVoiceActivity != voiceActivity) {
                ptrdiff_t byteOffset = ptr - packet->data();
                uint8_t *mutableBytes = packet->MutableData();
                uint8_t audioActivityBit = voiceActivity ? 0x80 : 0;
                mutableBytes[byteOffset] = audioLevel | audioActivityBit;
            }
            return;
        }

        ptr += (len + 1);
    }
}

#if 0 // Currently unused.
static void readHeaderVoiceActivity(const uint8_t* ptrRTPDataExtensionEnd, const uint8_t* ptr, bool &didRead, uint8_t &audioLevel, bool &voiceActivity) {
    while (ptrRTPDataExtensionEnd - ptr > 0) {
        //  0
        //  0 1 2 3 4 5 6 7
        // +-+-+-+-+-+-+-+-+
        // |  ID   |  len  |
        // +-+-+-+-+-+-+-+-+

        // Note that 'len' is the header extension element length, which is the
        // number of bytes - 1.
        const int id = (*ptr & 0xf0) >> 4;
        const int len = (*ptr & 0x0f);
        ptr++;

        if (id == 0) {
            // Padding byte, skip ignoring len.
            continue;
        }

        if (id == 15) {
            RTC_LOG(LS_VERBOSE)
            << "RTP extension header 15 encountered. Terminate parsing.";
            return;
        }

        if (ptrRTPDataExtensionEnd - ptr < (len + 1)) {
            RTC_LOG(LS_WARNING) << "Incorrect one-byte extension len: " << (len + 1)
            << ", bytes left in buffer: "
            << (ptrRTPDataExtensionEnd - ptr);
            return;
        }

        if (id == 1) { // kAudioLevelUri
            didRead = true;
            audioLevel = ptr[0] & 0x7f;
            voiceActivity = (ptr[0] & 0x80) != 0;

            return;
        }

        ptr += (len + 1);
    }
}
#endif


static void maybeUpdateRtpVoiceActivity(webrtc::CopyOnWriteBuffer *packet, bool voiceActivity, bool zeroAudioLevel) {
    const uint8_t *_ptrRTPDataBegin = packet->data();
    const uint8_t *_ptrRTPDataEnd = packet->data() + packet->size();

    const ptrdiff_t length = _ptrRTPDataEnd - _ptrRTPDataBegin;
    if (length < kRtpMinParseLength) {
        return;
    }

    // Version
    const uint8_t V = _ptrRTPDataBegin[0] >> 6;
    // eXtension
    const bool X = ((_ptrRTPDataBegin[0] & 0x10) == 0) ? false : true;
    const uint8_t CC = _ptrRTPDataBegin[0] & 0x0f;

    const uint8_t PT = _ptrRTPDataBegin[1] & 0x7f;

    const uint8_t* ptr = &_ptrRTPDataBegin[4];

    ptr += 4;

    ptr += 4;

    if (V != kRtpExpectedVersion) {
        return;
    }

    const size_t CSRCocts = CC * 4;

    if ((ptr + CSRCocts) > _ptrRTPDataEnd) {
        return;
    }

    if (PT != 111) {
        return;
    }

    for (uint8_t i = 0; i < CC; ++i) {
        ptr += 4;
    }

    if (X) {
      /* RTP header extension, RFC 3550.
       0                   1                   2                   3
       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |      defined by profile       |           length              |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |                        header extension                       |
      |                             ....                              |
      */
      const ptrdiff_t remain = _ptrRTPDataEnd - ptr;
      if (remain < 4) {
          return;
      }

      uint16_t definedByProfile = webrtc::ByteReader<uint16_t>::ReadBigEndian(ptr);
      ptr += 2;

      // in 32 bit words
      size_t XLen = webrtc::ByteReader<uint16_t>::ReadBigEndian(ptr);
      ptr += 2;
      XLen *= 4;  // in bytes

      if (static_cast<size_t>(remain) < (4 + XLen)) {
          return;
      }
      static constexpr uint16_t kRtpOneByteHeaderExtensionId = 0xBEDE;
      if (definedByProfile == kRtpOneByteHeaderExtensionId) {
          const uint8_t* ptrRTPDataExtensionEnd = ptr + XLen;
          updateHeaderWithVoiceActivity(packet, ptrRTPDataExtensionEnd, ptr, voiceActivity, zeroAudioLevel);
      }
    }
}

#if 0 // Currently unused.
static void maybeReadRtpVoiceActivity(webrtc::CopyOnWriteBuffer *packet, bool &didRead, uint32_t &ssrc, uint8_t &audioLevel, bool &voiceActivity) {
    const uint8_t *_ptrRTPDataBegin = packet->data();
    const uint8_t *_ptrRTPDataEnd = packet->data() + packet->size();

    const ptrdiff_t length = _ptrRTPDataEnd - _ptrRTPDataBegin;
    if (length < kRtpMinParseLength) {
        return;
    }

    // Version
    const uint8_t V = _ptrRTPDataBegin[0] >> 6;
    // eXtension
    const bool X = ((_ptrRTPDataBegin[0] & 0x10) == 0) ? false : true;
    const uint8_t CC = _ptrRTPDataBegin[0] & 0x0f;

    const uint8_t PT = _ptrRTPDataBegin[1] & 0x7f;

    const uint8_t* ptr = &_ptrRTPDataBegin[4];

    ptr += 4;

    ssrc = webrtc::ByteReader<uint32_t>::ReadBigEndian(ptr);
    ptr += 4;

    if (V != kRtpExpectedVersion) {
        return;
    }

    const size_t CSRCocts = CC * 4;

    if ((ptr + CSRCocts) > _ptrRTPDataEnd) {
        return;
    }

    if (PT != 111) {
        return;
    }

    for (uint8_t i = 0; i < CC; ++i) {
        ptr += 4;
    }

    if (X) {
      /* RTP header extension, RFC 3550.
       0                   1                   2                   3
       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |      defined by profile       |           length              |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |                        header extension                       |
      |                             ....                              |
      */
      const ptrdiff_t remain = _ptrRTPDataEnd - ptr;
      if (remain < 4) {
          return;
      }

      uint16_t definedByProfile = webrtc::ByteReader<uint16_t>::ReadBigEndian(ptr);
      ptr += 2;

      // in 32 bit words
      size_t XLen = webrtc::ByteReader<uint16_t>::ReadBigEndian(ptr);
      ptr += 2;
      XLen *= 4;  // in bytes

      if (static_cast<size_t>(remain) < (4 + XLen)) {
          return;
      }
      static constexpr uint16_t kRtpOneByteHeaderExtensionId = 0xBEDE;
      if (definedByProfile == kRtpOneByteHeaderExtensionId) {
          const uint8_t* ptrRTPDataExtensionEnd = ptr + XLen;
          readHeaderVoiceActivity(ptrRTPDataExtensionEnd, ptr, didRead, audioLevel, voiceActivity);
      }
    }
}
#endif

class WrappedDtlsSrtpTransport : public webrtc::DtlsSrtpTransport {
public:
    bool _voiceActivity = false;

public:
    WrappedDtlsSrtpTransport(bool rtcp_mux_enabled, const webrtc::FieldTrialsView& fieldTrials, std::function<void(webrtc::RtpPacketReceived const &, bool)> &&processRtpPacket, bool zeroAudioLevel) :
    webrtc::DtlsSrtpTransport(rtcp_mux_enabled, fieldTrials),
    _processRtpPacket(std::move(processRtpPacket)),
    _zeroAudioLevel(zeroAudioLevel) {
    }

    virtual ~WrappedDtlsSrtpTransport() {
    }

    bool SendRtpPacket(webrtc::CopyOnWriteBuffer *packet, const webrtc::AsyncSocketPacketOptions& options, int flags) override {
        maybeUpdateRtpVoiceActivity(packet, _voiceActivity, _zeroAudioLevel);
        return webrtc::DtlsSrtpTransport::SendRtpPacket(packet, options, flags);
    }
    
    void ProcessRtpPacket(webrtc::RtpPacketReceived const &packet, bool isUnresolved) override {
        _processRtpPacket(packet, isUnresolved);
    }

private:
    std::function<void(webrtc::RtpPacketReceived const &, bool)> _processRtpPacket;
    bool _zeroAudioLevel;
};

webrtc::CryptoOptions GroupNetworkManager::getDefaulCryptoOptions() {
    auto options = webrtc::CryptoOptions();
    options.srtp.enable_aes128_sha1_80_crypto_cipher = false;
    options.srtp.enable_gcm_crypto_suites = true;
    return options;
}

GroupNetworkManager::GroupNetworkManager(
    const webrtc::Environment &env,
    std::function<void(const State &)> stateUpdated,
    std::function<void(uint32_t, int)> unknownSsrcPacketReceived,
    std::function<void(bool)> dataChannelStateUpdated,
    std::function<void(std::string const &)> dataChannelMessageReceived,
    std::function<void(uint32_t, uint8_t, bool)> audioActivityUpdated,
    bool zeroAudioLevel,
    std::function<void(uint32_t)> anyActivityUpdated,
    std::shared_ptr<Threads> threads) :
_env(env),
_threads(std::move(threads)),
_stateUpdated(std::move(stateUpdated)),
_unknownSsrcPacketReceived(std::move(unknownSsrcPacketReceived)),
_dataChannelStateUpdated(dataChannelStateUpdated),
_dataChannelMessageReceived(dataChannelMessageReceived),
_audioActivityUpdated(audioActivityUpdated),
_zeroAudioLevel(zeroAudioLevel),
_anyActivityUpdated(anyActivityUpdated) {
    assert(_threads->getNetworkThread()->IsCurrent());

    _localIceParameters = PeerIceParameters(webrtc::CreateRandomString(webrtc::ICE_UFRAG_LENGTH), webrtc::CreateRandomString(webrtc::ICE_PWD_LENGTH), false);

    _localCertificate = webrtc::RTCCertificateGenerator::GenerateCertificate(webrtc::KeyParams(webrtc::KT_ECDSA), std::nullopt);

    _networkMonitorFactory = PlatformInterface::SharedInstance()->createNetworkMonitorFactory();

    _socketFactory.reset(new webrtc::BasicPacketSocketFactory(_threads->getNetworkThread()->socketserver()));
    _networkManager = std::make_unique<webrtc::BasicNetworkManager>(_env, _threads->getNetworkThread()->socketserver(), _networkMonitorFactory.get());
    _asyncResolverFactory = std::make_unique<webrtc::BasicAsyncDnsResolverFactory>();

    _dtlsSrtpTransport = std::make_unique<WrappedDtlsSrtpTransport>(true, _env.field_trials(), [this](webrtc::RtpPacketReceived const &packet, bool isUnresolved) {
        this->RtpPacketReceived_n(packet, isUnresolved);
    }, _zeroAudioLevel);
    _dtlsSrtpTransport->SetDtlsTransports(nullptr, nullptr);
    _dtlsSrtpTransport->SubscribeReadyToSend(this, [this](bool value) {
        this->DtlsReadyToSend(value);
    });

    resetDtlsSrtpTransport();
}

GroupNetworkManager::~GroupNetworkManager() {
    assert(_threads->getNetworkThread()->IsCurrent());

    RTC_LOG(LS_INFO) << "GroupNetworkManager::~GroupNetworkManager()";

    _dataChannelInterface.reset();
    _dtlsSrtpTransport.reset();
    _dtlsTransport.reset();
    _iceTransport = nullptr;
    _transportChannel.reset();
    _asyncResolverFactory.reset();
    _portAllocator.reset();
    _networkManager.reset();
    _socketFactory.reset();
}

void GroupNetworkManager::resetDtlsSrtpTransport() {
    std::unique_ptr<webrtc::BasicPortAllocator> portAllocator = std::make_unique<webrtc::BasicPortAllocator>(_env, _networkManager.get(), _socketFactory.get(), _turnCustomizer.get(), nullptr);
    
    uint32_t flags = portAllocator->flags();
    
    flags |=
        webrtc::PORTALLOCATOR_ENABLE_IPV6 |
        webrtc::PORTALLOCATOR_ENABLE_IPV6_ON_WIFI;
    
    portAllocator->set_flags(flags);
    portAllocator->Initialize();

    portAllocator->SetConfiguration({}, {}, 2, webrtc::NO_PRUNE, _turnCustomizer.get());

    webrtc::IceTransportInit iceTransportInit(_env);
    iceTransportInit.set_port_allocator(portAllocator.get());
    iceTransportInit.set_async_dns_resolver_factory(_asyncResolverFactory.get());

    auto transportChannel = webrtc::P2PTransportChannel::Create("transport", 0, std::move(iceTransportInit));

    webrtc::IceConfig iceConfig;
    iceConfig.continual_gathering_policy = webrtc::GATHER_CONTINUALLY;
    iceConfig.prioritize_most_likely_candidate_pairs = true;
    iceConfig.regather_on_failed_networks_interval = webrtc::TimeDelta::Millis(2000);
    transportChannel->SetIceConfig(iceConfig);

    webrtc::IceParameters localIceParameters(
        _localIceParameters.ufrag,
        _localIceParameters.pwd,
        false
    );

    transportChannel->SetIceParameters(localIceParameters);
    const bool isOutgoing = false;
    transportChannel->SetIceRole(isOutgoing ? webrtc::ICEROLE_CONTROLLING : webrtc::ICEROLE_CONTROLLED);
    transportChannel->SetRemoteIceMode(webrtc::ICEMODE_FULL);

    transportChannel->SubscribeIceTransportStateChanged(this, [this](webrtc::IceTransportInternal *transport) {
        transportStateChanged(transport);
    });
    transportChannel->RegisterReceivedPacketCallback(this, [this](webrtc::PacketTransportInternal *transport, const webrtc::ReceivedIpPacket &packet) {
        transportPacketReceived(transport, packet);
    });

    webrtc::CryptoOptions cryptoOptions = GroupNetworkManager::getDefaulCryptoOptions();

    webrtc::scoped_refptr<webrtc::IceTransportInterface> iceTransport = webrtc::make_ref_counted<webrtc::IceTransportWithPointer>(transportChannel.get());
    auto dtlsTransport = std::make_unique<webrtc::DtlsTransportInternalImpl>(_env, iceTransport, cryptoOptions);

    dtlsTransport->SubscribeWritableState(this, [this](webrtc::PacketTransportInternal *transport) {
        OnTransportWritableState_n(transport);
    });
    dtlsTransport->SubscribeReceivingState(this, [this](webrtc::PacketTransportInternal *transport) {
        OnTransportReceivingState_n(transport);
    });

    dtlsTransport->SetDtlsRole(webrtc::SSLRole::SSL_SERVER);
    dtlsTransport->SetLocalCertificate(_localCertificate);

    _dtlsSrtpTransport->SetDtlsTransports(dtlsTransport.get(), nullptr);

    // The previous DTLS transport goes before the ICE transport it uses.
    _dtlsTransport = std::move(dtlsTransport);
    _iceTransport = std::move(iceTransport);
    _transportChannel = std::move(transportChannel);
    _portAllocator = std::move(portAllocator);
}

void GroupNetworkManager::start() {
    _transportChannel->MaybeStartGathering();

    restartDataChannel();
}

void GroupNetworkManager::restartDataChannel() {
    _dataChannelStateUpdated(false);

    // The old channel goes first: the DTLS transport holds a single close callback, which the new SCTP transport sets
    // and the old one's destruction would clear.
    _dataChannelInterface.reset();

    const auto weak = std::weak_ptr<GroupNetworkManager>(shared_from_this());
    _dataChannelInterface.reset(new SctpDataChannelProviderInterfaceImpl(
        _env,
        _dtlsTransport.get(),
        true,
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
            strong->restartDataChannel();
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

    _dataChannelInterface->updateIsConnected(_isConnected);
}

void GroupNetworkManager::stop() {
    // The ICE transport's state callback cannot be removed (8154 has no unsubscribe for it); the transport goes in
    // resetDtlsSrtpTransport() below, and transportStateChanged ignores any transport but the current one.
    _transportChannel->DeregisterReceivedPacketCallback(this);

    _dtlsTransport->UnsubscribeWritableState(this);
    _dtlsTransport->UnsubscribeReceivingState(this);

    _dataChannelInterface.reset();

    _localIceParameters = PeerIceParameters(webrtc::CreateRandomString(webrtc::ICE_UFRAG_LENGTH), webrtc::CreateRandomString(webrtc::ICE_PWD_LENGTH), false);

    _localCertificate = webrtc::RTCCertificateGenerator::GenerateCertificate(webrtc::KeyParams(webrtc::KT_ECDSA), std::nullopt);

    resetDtlsSrtpTransport();
}

PeerIceParameters GroupNetworkManager::getLocalIceParameters() {
    return _localIceParameters;
}

std::unique_ptr<webrtc::SSLFingerprint> GroupNetworkManager::getLocalFingerprint() {
    auto certificate = _localCertificate;
    if (!certificate) {
        return nullptr;
    }
    return webrtc::SSLFingerprint::CreateFromCertificate(*certificate);
}

void GroupNetworkManager::setRemoteParams(PeerIceParameters const &remoteIceParameters, std::vector<webrtc::Candidate> const &iceCandidates, webrtc::SSLFingerprint *fingerprint) {
    _remoteIceParameters = remoteIceParameters;

    webrtc::IceParameters parameters(
        remoteIceParameters.ufrag,
        remoteIceParameters.pwd,
        true
    );

    _transportChannel->SetRemoteIceParameters(parameters);

    for (const auto &candidate : iceCandidates) {
        _transportChannel->AddRemoteCandidate(candidate);
    }

    if (fingerprint) {
        // The role is ours (SSL_SERVER, set when the transport was made).
        _dtlsTransport->SetRemoteParameters(fingerprint->algorithm, fingerprint->digest.data(), fingerprint->digest.size(), std::nullopt);
    }
}

void GroupNetworkManager::sendDataChannelMessage(std::string const &message) {
    if (_dataChannelInterface) {
        _dataChannelInterface->sendDataChannelMessage(message);
    }
}

void GroupNetworkManager::setOutgoingVoiceActivity(bool isSpeech) {
    if (_dtlsSrtpTransport) {
        ((WrappedDtlsSrtpTransport *)_dtlsSrtpTransport.get())->_voiceActivity = isSpeech;
    }
}

webrtc::RtpTransport *GroupNetworkManager::getRtpTransport() {
    return _dtlsSrtpTransport.get();
}

void GroupNetworkManager::checkConnectionTimeout() {
    const auto weak = std::weak_ptr<GroupNetworkManager>(shared_from_this());
    _threads->getNetworkThread()->PostDelayedTask([weak]() {
        auto strong = weak.lock();
        if (!strong) {
            return;
        }

        int64_t currentTimestamp = webrtc::TimeMillis();
        const int64_t maxTimeout = 20000;

        if (strong->_lastNetworkActivityMs + maxTimeout < currentTimestamp) {
            GroupNetworkManager::State emitState;
            emitState.isReadyToSendData = false;
            emitState.isFailed = true;
            strong->_stateUpdated(emitState);
        }

        strong->checkConnectionTimeout();
    }, webrtc::TimeDelta::Millis(1000));
}

void GroupNetworkManager::candidateGathered(webrtc::IceTransportInternal *transport, const webrtc::Candidate &candidate) {
    assert(_threads->getNetworkThread()->IsCurrent());
}

void GroupNetworkManager::candidateGatheringState(webrtc::IceTransportInternal *transport) {
    assert(_threads->getNetworkThread()->IsCurrent());
}

void GroupNetworkManager::OnTransportWritableState_n(webrtc::PacketTransportInternal *transport) {
    assert(_threads->getNetworkThread()->IsCurrent());

    UpdateAggregateStates_n();
}
void GroupNetworkManager::OnTransportReceivingState_n(webrtc::PacketTransportInternal *transport) {
    assert(_threads->getNetworkThread()->IsCurrent());

    UpdateAggregateStates_n();
}

void GroupNetworkManager::DtlsReadyToSend(bool isReadyToSend) {
    UpdateAggregateStates_n();

    if (isReadyToSend) {
        const auto weak = std::weak_ptr<GroupNetworkManager>(shared_from_this());
        _threads->getNetworkThread()->PostTask([weak]() {
            const auto strong = weak.lock();
            if (!strong) {
                return;
            }
            strong->UpdateAggregateStates_n();
        });
    }
}

void GroupNetworkManager::transportStateChanged(webrtc::IceTransportInternal *transport) {
    // 8154's IceTransportInternal cannot remove a state subscriber, so a replaced channel (stop()) or one being destroyed
    // (stop(), the destructor: _transportChannel already points past it) may still report here.
    if (transport != _transportChannel.get()) {
        return;
    }
    UpdateAggregateStates_n();
}

void GroupNetworkManager::transportReadyToSend(webrtc::IceTransportInternal *transport) {
    assert(_threads->getNetworkThread()->IsCurrent());
}

void GroupNetworkManager::transportPacketReceived(webrtc::PacketTransportInternal *transport, const webrtc::ReceivedIpPacket &packet) {
    assert(_threads->getNetworkThread()->IsCurrent());

    _lastNetworkActivityMs = webrtc::TimeMillis();
}

void GroupNetworkManager::RtpPacketReceived_n(webrtc::RtpPacketReceived const &packet, bool isUnresolved) {
    if (packet.HasExtension<webrtc::AudioLevelExtension>()) {
        webrtc::AudioLevel level;
        if (packet.GetExtension<webrtc::AudioLevelExtension>(&level)) {
            if (_audioActivityUpdated) {
                _audioActivityUpdated(packet.Ssrc(), (uint8_t)level.level(), level.voice_activity());
            }
        }
    }
    
    if (_anyActivityUpdated) {
        _anyActivityUpdated(packet.Ssrc());
    }

    // Every packet, whatever isUnresolved says: the fork's RtpTransport::ProcessRtpPacket hook always passes true (as
    // on M123), and the engine sorts known SSRCs from unknown ones itself (GroupInstanceCustomImpl's
    // receiveUnknownSsrcPacket also keeps known audio channels' activity up to date from these).
    if (_unknownSsrcPacketReceived) {
        uint32_t ssrc = packet.Ssrc();
        int payloadType = packet.PayloadType();

        _unknownSsrcPacketReceived(ssrc, payloadType);
    }
}

void GroupNetworkManager::UpdateAggregateStates_n() {
    assert(_threads->getNetworkThread()->IsCurrent());

    auto state = _transportChannel->GetIceTransportState();
    bool isConnected = false;
    switch (state) {
        case webrtc::IceTransportState::kConnected:
        case webrtc::IceTransportState::kCompleted:
            isConnected = true;
            break;
        default:
            break;
    }

    if (!_dtlsSrtpTransport->IsWritable(false)) {
        isConnected = false;
    }

    if (_isConnected != isConnected) {
        _isConnected = isConnected;

        GroupNetworkManager::State emitState;
        emitState.isReadyToSendData = isConnected;
        _stateUpdated(emitState);

        if (_dataChannelInterface) {
            _dataChannelInterface->updateIsConnected(isConnected);
        }
    }
}

void GroupNetworkManager::sctpReadyToSendData() {
}

} // namespace tgcalls
