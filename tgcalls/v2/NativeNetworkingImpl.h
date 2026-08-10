#ifndef TGCALLS_NATIVE_NETWORKING_IMPL_H
#define TGCALLS_NATIVE_NETWORKING_IMPL_H

#ifdef WEBRTC_WIN
// Compiler errors in conflicting Windows headers if not included here.
#include <cstdint>
#include <winsock2.h>
#endif // WEBRTC_WIN

#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/third_party/sigslot/sigslot.h"
#include "api/candidate.h"
#include "media/base/media_channel.h"
#include "rtc_base/ssl_fingerprint.h"
#include "pc/sctp_data_channel.h"
#include "p2p/base/port.h"

#include <functional>
#include <memory>

#include "InstanceNetworking.h"
#include "Message.h"
#include "ThreadLocalObject.h"
#include "Instance.h"

namespace webrtc {
class BasicPacketSocketFactory;
class BasicNetworkManager;
class PacketTransportInternal;
struct NetworkRoute;
} // namespace webrtc

namespace webrtc {
class BasicPortAllocator;
class IceTransportInternal;
class IceTransportInterface;
class DtlsTransportInternal;
class RelayPortFactoryInterface;
} // namespace webrtc

namespace webrtc {
class TurnCustomizer;
class DtlsSrtpTransport;
class RtpTransport;
class AsyncDnsResolverFactoryInterface;
} // namespace webrtc

namespace tgcalls {

struct Message;
class SctpDataChannelProviderInterfaceImpl;
class Threads;

class NativeNetworkingImpl : public InstanceNetworking, public sigslot::has_slots<>, public std::enable_shared_from_this<NativeNetworkingImpl> {
public:
    static webrtc::CryptoOptions getDefaulCryptoOptions();

    NativeNetworkingImpl(Configuration &&configuration);
    virtual ~NativeNetworkingImpl();

    virtual void start() override;
    virtual void stop() override;

    virtual PeerIceParameters getLocalIceParameters() override;
    virtual std::unique_ptr<webrtc::SSLFingerprint> getLocalFingerprint() override;
    virtual void setRemoteParams(PeerIceParameters const &remoteIceParameters, webrtc::SSLFingerprint *fingerprint, std::string const &sslSetup) override;
    virtual void addCandidates(std::vector<webrtc::Candidate> const &candidates) override;

    virtual void sendDataChannelMessage(std::string const &message) override;

    virtual webrtc::RtpTransport *getRtpTransport() override;

private:
    void resetDtlsSrtpTransport();
    void checkConnectionTimeout();
    void candidateGathered(webrtc::IceTransportInternal *transport, const webrtc::Candidate &candidate);
    void candidateGatheringState(webrtc::IceTransportInternal *transport);
    void OnTransportWritableState_n(webrtc::PacketTransportInternal *transport);
    void OnTransportReceivingState_n(webrtc::PacketTransportInternal *transport);
    void transportStateChanged(webrtc::IceTransportInternal *transport);
    void transportReadyToSend(webrtc::IceTransportInternal *transport);
    void transportRouteChanged(absl::optional<webrtc::NetworkRoute> route);
    void candidatePairChanged(webrtc::CandidatePairChangeEvent const &event);
    void DtlsReadyToSend(bool DtlsReadyToSend);
    void UpdateAggregateStates_n();
    void RtpPacketReceived_n(webrtc::CopyOnWriteBuffer *packet, int64_t packet_time_us, bool isUnresolved);
    void OnRtcpPacketReceived_n(webrtc::CopyOnWriteBuffer *packet, int64_t packet_time_us);

    void sctpReadyToSendData();
    
    void notifyStateUpdated();
    
    void processPendingLocalStandaloneReflectorCandidates();

    std::shared_ptr<Threads> _threads;
    bool _isOutgoing = false;
    EncryptionKey _encryptionKey;
    bool _enableStunMarking = false;
    bool _enableTCP = false;
    bool _enableP2P = false;
    std::vector<RtcServer> _rtcServers;
    absl::optional<Proxy> _proxy;
    std::map<std::string, json11::Json> _customParameters;

    std::function<void(const InstanceNetworking::State &)> _stateUpdated;
    std::function<void(const webrtc::Candidate &)> _candidateGathered;
    std::function<void(webrtc::CopyOnWriteBuffer const &, bool)> _transportMessageReceived;
    std::function<void(webrtc::CopyOnWriteBuffer const &, int64_t)> _rtcpPacketReceived;
    std::function<void(bool)> _dataChannelStateUpdated;
    std::function<void(std::string const &)> _dataChannelMessageReceived;

    std::unique_ptr<webrtc::NetworkMonitorFactory> _networkMonitorFactory;
    webrtc::Environment _env;
    webrtc::SocketFactory *_underlyingSocketFactory = nullptr;
    std::unique_ptr<webrtc::PacketSocketFactory> _socketFactory;
    std::unique_ptr<webrtc::NetworkManager> _networkManager;
    std::unique_ptr<webrtc::TurnCustomizer> _turnCustomizer;
    std::unique_ptr<webrtc::RelayPortFactoryInterface> _relayPortFactory;
    std::unique_ptr<webrtc::BasicPortAllocator> _portAllocator;
    std::unique_ptr<webrtc::AsyncDnsResolverFactoryInterface> _asyncResolverFactory;
    webrtc::scoped_refptr<webrtc::IceTransportInterface> _transportChannel;
    std::unique_ptr<webrtc::RtpTransport> _mtProtoRtpTransport;
    std::unique_ptr<webrtc::DtlsTransportInternal> _dtlsTransport;
    std::unique_ptr<webrtc::DtlsSrtpTransport> _dtlsSrtpTransport;

    std::unique_ptr<SctpDataChannelProviderInterfaceImpl> _dataChannelInterface;

    webrtc::scoped_refptr<webrtc::RTCCertificate> _localCertificate;
    PeerIceParameters _localIceParameters;
    absl::optional<PeerIceParameters> _remoteIceParameters;

    bool _isConnected = false;
    bool _isFailed = false;
    int64_t _lastDisconnectedTimestamp = 0;
    absl::optional<RouteDescription> _currentRouteDescription;
    absl::optional<ConnectionDescription> _currentConnectionDescription;
    
    std::vector<webrtc::Candidate> _pendingLocalStandaloneReflectorCandidates;
};

} // namespace tgcalls

#endif
