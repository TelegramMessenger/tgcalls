#include "InstanceImplReference.h"

#include <memory>
#include "api/scoped_refptr.h"
#include "rtc_base/thread.h"
#include "api/peer_connection_interface.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "media/engine/webrtc_media_engine.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/rtc_event_log/rtc_event_log_factory.h"
#include "sdk/media_constraints.h"
#include "api/peer_connection_interface.h"
#include "api/video_track_source_proxy.h"
#include "system_wrappers/include/field_trial.h"
#include "api/stats/rtcstats_objects.h"

#include "ThreadLocalObject.h"
#include "Manager.h"
#include "NetworkManager.h"
#include "VideoCaptureInterfaceImpl.h"
#include "platform/PlatformInterface.h"

namespace tgcalls {
namespace {

rtc::Thread *makeNetworkThread() {
    static std::unique_ptr<rtc::Thread> value = rtc::Thread::CreateWithSocketServer();
    value->SetName("WebRTC-Reference-Network", nullptr);
    value->Start();
    return value.get();
}

rtc::Thread *getNetworkThread() {
    static rtc::Thread *value = makeNetworkThread();
    return value;
}

rtc::Thread *makeWorkerThread() {
    static std::unique_ptr<rtc::Thread> value = rtc::Thread::Create();
    value->SetName("WebRTC-Reference-Worker", nullptr);
    value->Start();
    return value.get();
}

rtc::Thread *getWorkerThread() {
    static rtc::Thread *value = makeWorkerThread();
    return value;
}

rtc::Thread *getSignalingThread() {
    return Manager::getMediaThread();
}

rtc::Thread *getMediaThread() {
    return Manager::getMediaThread();
}

VideoCaptureInterfaceObject *GetVideoCaptureAssumingSameThread(VideoCaptureInterface *videoCapture) {
    return videoCapture
        ? static_cast<VideoCaptureInterfaceImpl*>(videoCapture)->object()->getSyncAssumingSameThread()
        : nullptr;
}

class PeerConnectionObserverImpl : public webrtc::PeerConnectionObserver {
private:
    std::function<void(std::string, int, std::string)> _discoveredIceCandidate;
    std::function<void(bool)> _connectionStateChanged;
    std::function<void(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)> _onTrack;
    
public:
    PeerConnectionObserverImpl(
        std::function<void(std::string, int, std::string)> discoveredIceCandidate,
        std::function<void(bool)> connectionStateChanged,
        std::function<void(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)> onTrack
    ) :
    _discoveredIceCandidate(discoveredIceCandidate),
    _connectionStateChanged(connectionStateChanged),
    _onTrack(onTrack) {
    }
    
    virtual void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) {
        bool isConnected = false;
        if (new_state == webrtc::PeerConnectionInterface::SignalingState::kStable) {
            isConnected = true;
        }
        _connectionStateChanged(isConnected);
    }
    
    virtual void OnAddStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) {
    }
    
    virtual void OnRemoveStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) {
    }
    
    virtual void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
    }
    
    virtual void OnRenegotiationNeeded() {
    }
    
    virtual void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) {
    }
    
    virtual void OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) {
    }
    
    virtual void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
    }
    
    virtual void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) {
    }
    
    virtual void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
        std::string sdp;
        candidate->ToString(&sdp);
        _discoveredIceCandidate(sdp, candidate->sdp_mline_index(), candidate->sdp_mid());
    }
    
    virtual void OnIceCandidateError(const std::string& host_candidate, const std::string& url, int error_code, const std::string& error_text) {
    }
    
    virtual void OnIceCandidateError(const std::string& address,
                                     int port,
                                     const std::string& url,
                                     int error_code,
                                     const std::string& error_text) {
    }
    
    virtual void OnIceCandidatesRemoved(const std::vector<cricket::Candidate>& candidates) {
    }
    
    virtual void OnIceConnectionReceivingChange(bool receiving) {
    }
    
    virtual void OnIceSelectedCandidatePairChanged(const cricket::CandidatePairChangeEvent& event) {
    }
    
    virtual void OnAddTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) {
    }
    
    virtual void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
        _onTrack(transceiver);
    }
    
    virtual void OnRemoveTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
    }
    
    virtual void OnInterestingUsage(int usage_pattern) {
    }
};

class RTCStatsCollectorCallbackImpl : public webrtc::RTCStatsCollectorCallback {
public:
    RTCStatsCollectorCallbackImpl(std::function<void(const rtc::scoped_refptr<const webrtc::RTCStatsReport> &)> completion) :
    _completion(completion) {
    }
    
    virtual void OnStatsDelivered(const rtc::scoped_refptr<const webrtc::RTCStatsReport> &report) override {
        _completion(report);
    }
    
private:
    std::function<void(const rtc::scoped_refptr<const webrtc::RTCStatsReport> &)> _completion;
};

class CreateSessionDescriptionObserverImpl : public webrtc::CreateSessionDescriptionObserver {
private:
    std::function<void(std::string, std::string)> _completion;
    
public:
    CreateSessionDescriptionObserverImpl(std::function<void(std::string, std::string)> completion) :
    _completion(completion) {
    }
    
    virtual void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
        if (desc) {
            std::string sdp;
            desc->ToString(&sdp);
            
            _completion(sdp, desc->type());
        }
    }
    
    virtual void OnFailure(webrtc::RTCError error) override {
    }
};

class SetSessionDescriptionObserverImpl : public webrtc::SetSessionDescriptionObserver {
private:
    std::function<void()> _completion;
    
public:
    SetSessionDescriptionObserverImpl(std::function<void()> completion) :
    _completion(completion) {
    }

    virtual void OnSuccess() override {
        _completion();
    }
    
    virtual void OnFailure(webrtc::RTCError error) override {
    }
};

struct StatsData {
    int32_t packetsReceived = 0;
    int32_t packetsLost = 0;
};

} //namespace

class InstanceImplReferenceInternal final : public std::enable_shared_from_this<InstanceImplReferenceInternal> {
public:
    InstanceImplReferenceInternal(
        EncryptionKey encryptionKey,
        std::vector<RtcServer> const &rtcServers,
        bool enableP2P,
        std::function<void(State, VideoState)> stateUpdated,
        std::function<void(int)> signalBarsUpdated,
        std::function<void(const std::vector<uint8_t> &)> signalingDataEmitted,
        std::function<void(bool)> remoteVideoIsActiveUpdated,
        std::shared_ptr<VideoCaptureInterface> videoCapture
    ) :
    _encryptionKey(encryptionKey),
    _rtcServers(rtcServers),
    _enableP2P(enableP2P),
    _stateUpdated(stateUpdated),
    _signalBarsUpdated(signalBarsUpdated),
    _signalingDataEmitted(signalingDataEmitted),
    _remoteVideoIsActiveUpdated(remoteVideoIsActiveUpdated),
    _videoCapture(videoCapture),
    _state(State::Reconnecting),
    _videoState(VideoState::Possible) {
        assert(getMediaThread()->IsCurrent());
        
        __unused static const auto onceToken = [] {
            rtc::LogMessage::LogToDebug(rtc::LS_INFO);
            rtc::LogMessage::SetLogToStderr(true);
            return 0;
        }();
        
        webrtc::field_trial::InitFieldTrialsFromString(
            "WebRTC-Audio-SendSideBwe/Enabled/"
            "WebRTC-Audio-Allocation/min:6kbps,max:32kbps/"
            "WebRTC-Audio-OpusMinPacketLossRate/Enabled-1/"
            "WebRTC-FlexFEC-03/Enabled/"
            "WebRTC-FlexFEC-03-Advertised/Enabled/"
            "WebRTC-Audio-BitrateAdaptation/Enabled/WebRTC-Audio-FecAdaptation/Enabled/"
        );
        
        _streamIds.push_back("stream");
    }
    
    ~InstanceImplReferenceInternal() {
        assert(getMediaThread()->IsCurrent());
        
        _peerConnection->Close();
    }
    
    void start() {
        const auto weak = std::weak_ptr<InstanceImplReferenceInternal>(shared_from_this());
        
        _signalingConnection.reset(new EncryptedConnection(
            EncryptedConnection::Type::Signaling,
            _encryptionKey,
            [weak](int delayMs, int cause) {
                if (delayMs == 0) {
                    getMediaThread()->PostTask(RTC_FROM_HERE, [weak, cause](){
                        auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        strong->sendPendingServiceMessages(cause);
                    });
                } else {
                    getMediaThread()->PostDelayedTask(RTC_FROM_HERE, [weak, cause]() {
                        auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        strong->sendPendingServiceMessages(cause);
                    }, delayMs);
                }
            }
        ));
        
        if (_videoCapture) {
            _videoState = VideoState::OutgoingRequested;
        }
        
        webrtc::PeerConnectionFactoryDependencies dependencies;
        dependencies.network_thread = getNetworkThread();
        dependencies.worker_thread = getWorkerThread();
        dependencies.signaling_thread = getSignalingThread();
        dependencies.task_queue_factory = webrtc::CreateDefaultTaskQueueFactory();
        
        cricket::MediaEngineDependencies mediaDeps;
        mediaDeps.task_queue_factory = dependencies.task_queue_factory.get();
        mediaDeps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
        mediaDeps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
        mediaDeps.video_encoder_factory = PlatformInterface::SharedInstance()->makeVideoEncoderFactory();
        mediaDeps.video_decoder_factory = PlatformInterface::SharedInstance()->makeVideoDecoderFactory();
        
        webrtc::AudioProcessing *apm = webrtc::AudioProcessingBuilder().Create();
        webrtc::AudioProcessing::Config audioConfig;
        webrtc::AudioProcessing::Config::NoiseSuppression noiseSuppression;
        noiseSuppression.enabled = true;
        noiseSuppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
        audioConfig.noise_suppression = noiseSuppression;
        
        audioConfig.high_pass_filter.enabled = true;
        
        apm->ApplyConfig(audioConfig);
        
        mediaDeps.audio_processing = apm;
        
        dependencies.media_engine = cricket::CreateMediaEngine(std::move(mediaDeps));
        dependencies.call_factory = webrtc::CreateCallFactory();
        dependencies.event_log_factory =
            std::make_unique<webrtc::RtcEventLogFactory>(dependencies.task_queue_factory.get());
        dependencies.network_controller_factory = nullptr;
        dependencies.media_transport_factory = nullptr;
        
        _nativeFactory = webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));
        
        webrtc::PeerConnectionInterface::RTCConfiguration config;
        config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
        config.continual_gathering_policy = webrtc::PeerConnectionInterface::ContinualGatheringPolicy::GATHER_CONTINUALLY;
        config.audio_jitter_buffer_fast_accelerate = true;
        config.prioritize_most_likely_ice_candidate_pairs = true;
        config.presume_writable_when_fully_relayed = true;
        config.audio_jitter_buffer_enable_rtx_handling = true;
        
        for (auto &server : _rtcServers) {
            if (server.isTurn) {
                webrtc::PeerConnectionInterface::IceServer iceServer;
                std::ostringstream uri;
                uri << "turn:";
                uri << server.host;
                uri << ":";
                uri << server.port;
                iceServer.uri = uri.str();
                iceServer.username = server.login;
                iceServer.password = server.password;
                config.servers.push_back(iceServer);
            } else {
                webrtc::PeerConnectionInterface::IceServer iceServer;
                std::ostringstream uri;
                uri << "stun:";
                uri << server.host;
                uri << ":";
                uri << server.port;
                iceServer.uri = uri.str();
                config.servers.push_back(iceServer);
            }
        }
        
        if (!_enableP2P) {
            config.type = webrtc::PeerConnectionInterface::kRelay;
        }
        
        _observer.reset(new PeerConnectionObserverImpl(
            [weak](std::string sdp, int mid, std::string sdpMid) {
                getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sdp, mid, sdpMid](){
                    auto strong = weak.lock();
                    if (strong) {
                        strong->emitIceCandidate(sdp, mid, sdpMid);
                    }
                });
            },
            [weak](bool isConnected) {
                getMediaThread()->PostTask(RTC_FROM_HERE, [weak, isConnected](){
                    auto strong = weak.lock();
                    if (strong) {
                        strong->updateIsConnected(isConnected);
                    }
                });
            },
            [weak](rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
                getMediaThread()->PostTask(RTC_FROM_HERE, [weak, transceiver](){
                    auto strong = weak.lock();
                    if (!strong) {
                        return;
                    }
                    strong->onTrack(transceiver);
                });
            }
        ));
        _peerConnection = _nativeFactory->CreatePeerConnection(config, nullptr, nullptr, _observer.get());
        assert(_peerConnection != nullptr);
        
        cricket::AudioOptions options;
        rtc::scoped_refptr<webrtc::AudioSourceInterface> audioSource = _nativeFactory->CreateAudioSource(options);
        _localAudioTrack = _nativeFactory->CreateAudioTrack("audio0", audioSource);
        _peerConnection->AddTrack(_localAudioTrack, _streamIds);
        
        if (_videoCapture) {
            beginSendingVideo();
        }
        
        if (_encryptionKey.isOutgoing) {
            emitOffer();
        }
        
        beginStatsTimer(1000);
    }
    
    void setMuteMicrophone(bool muteMicrophone) {
        _localAudioTrack->set_enabled(!muteMicrophone);
    }
    
    void setIncomingVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
        if (!sink) {
            return;
        }
        _currentSink = sink;
        if (_remoteVideoTrack) {
            _remoteVideoTrack->AddOrUpdateSink(_currentSink.get(), rtc::VideoSinkWants());
        }
    }
    
    void requestVideo(std::shared_ptr<VideoCaptureInterface> videoCapture) {
        _videoCapture = videoCapture;
        if (_videoState == VideoState::Possible) {
            _videoState = VideoState::OutgoingRequested;

            emitRequestVideo();
            
            _stateUpdated(_state, _videoState);
        } else if (_videoState == VideoState::IncomingRequested) {
            _videoState = VideoState::Active;

            emitRequestVideo();
            
            _stateUpdated(_state, _videoState);

            beginSendingVideo();
        }
    }
    
    void receiveSignalingData(const std::vector<uint8_t> &data) {
        if (const auto packet = _signalingConnection->handleIncomingPacket((const char *)data.data(), data.size())) {
            const auto mainMessage = &packet->main.message.data;
            if (const auto signalingData = absl::get_if<UnstructuredDataMessage>(mainMessage)) {
                processSignalingData(signalingData->data);
            }
            for (auto &it : packet->additional) {
                const auto additionalMessage = &it.message.data;
                if (const auto signalingData = absl::get_if<UnstructuredDataMessage>(additionalMessage)) {
                    processSignalingData(signalingData->data);
                }
            }
        }
    }
    
    void processSignalingData(const rtc::CopyOnWriteBuffer &decryptedPacket) {
        rtc::ByteBufferReader reader((const char *)decryptedPacket.data(), decryptedPacket.size());
        uint8_t command = 0;
        if (!reader.ReadUInt8(&command)) {
            return;
        }
        if (command == 1) {
            uint32_t sdpLength = 0;
            if (!reader.ReadUInt32(&sdpLength)) {
                return;
            }
            std::string sdp;
            if (!reader.ReadString(&sdp, sdpLength)) {
                return;
            }
            uint32_t mid = 0;
            if (!reader.ReadUInt32(&mid)) {
                return;
            }
            uint32_t sdpMidLength = 0;
            if (!reader.ReadUInt32(&sdpMidLength)) {
                return;
            }
            std::string sdpMid;
            if (!reader.ReadString(&sdpMid, sdpMidLength)) {
                return;
            }
            webrtc::SdpParseError error;
            webrtc::IceCandidateInterface *iceCandidate = webrtc::CreateIceCandidate(sdpMid, mid, sdp, &error);
            if (iceCandidate != nullptr) {
                std::unique_ptr<webrtc::IceCandidateInterface> nativeCandidate = std::unique_ptr<webrtc::IceCandidateInterface>(iceCandidate);
                _peerConnection->AddIceCandidate(std::move(nativeCandidate), [](auto error) {
                });
            }
        } else if (command == 2) {
            uint32_t sdpLength = 0;
            if (!reader.ReadUInt32(&sdpLength)) {
                return;
            }
            std::string sdp;
            if (!reader.ReadString(&sdp, sdpLength)) {
                return;
            }
            uint32_t typeLength = 0;
            if (!reader.ReadUInt32(&typeLength)) {
                return;
            }
            std::string type;
            if (!reader.ReadString(&type, typeLength)) {
                return;
            }
            webrtc::SdpParseError error;
            webrtc::SessionDescriptionInterface *sessionDescription = webrtc::CreateSessionDescription(type, sdp, &error);
            if (sessionDescription != nullptr) {
                const auto weak = std::weak_ptr<InstanceImplReferenceInternal>(shared_from_this());
                rtc::scoped_refptr<SetSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<SetSessionDescriptionObserverImpl>([weak]() {
                    getMediaThread()->PostTask(RTC_FROM_HERE, [weak](){
                        auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        strong->emitAnswer();
                    });
                }));
                _peerConnection->SetRemoteDescription(observer, sessionDescription);
            }
        } else if (command == 3) {
            uint32_t sdpLength = 0;
            if (!reader.ReadUInt32(&sdpLength)) {
                return;
            }
            std::string sdp;
            if (!reader.ReadString(&sdp, sdpLength)) {
                return;
            }
            uint32_t typeLength = 0;
            if (!reader.ReadUInt32(&typeLength)) {
                return;
            }
            std::string type;
            if (!reader.ReadString(&type, typeLength)) {
                return;
            }
            webrtc::SdpParseError error;
            webrtc::SessionDescriptionInterface *sessionDescription = webrtc::CreateSessionDescription(type, sdp, &error);
            if (sessionDescription != nullptr) {
                rtc::scoped_refptr<SetSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<SetSessionDescriptionObserverImpl>([]() {
                }));
                _peerConnection->SetRemoteDescription(observer, sessionDescription);
            }
        } else if (command == 4) {
            uint8_t value = 0;
            if (!reader.ReadUInt8(&value)) {
                return;
            }
            _remoteVideoIsActiveUpdated(value != 0);
        } else if (command == 5) {
            if (_videoState == VideoState::Possible) {
                _videoState = VideoState::IncomingRequested;
                _stateUpdated(_state, _videoState);
            } else if (_videoState == VideoState::OutgoingRequested) {
                _videoState = VideoState::Active;
                _stateUpdated(_state, _videoState);
                
                beginSendingVideo();
            }
        }
    }
    
private:
    void beginStatsTimer(int timeoutMs) {
        const auto weak = std::weak_ptr<InstanceImplReferenceInternal>(shared_from_this());
        getMediaThread()->PostDelayedTask(RTC_FROM_HERE, [weak]() {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                strong->collectStats();
            });
        }, timeoutMs);
    }
    
    void collectStats() {
        const auto weak = std::weak_ptr<InstanceImplReferenceInternal>(shared_from_this());
        
        rtc::scoped_refptr<RTCStatsCollectorCallbackImpl> observer(new rtc::RefCountedObject<RTCStatsCollectorCallbackImpl>([weak](const rtc::scoped_refptr<const webrtc::RTCStatsReport> &stats) {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, stats](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                strong->reportStats(stats);
                strong->beginStatsTimer(5000);
            });
        }));
        _peerConnection->GetStats(observer);
    }
    
    void reportStats(const rtc::scoped_refptr<const webrtc::RTCStatsReport> &stats) {
        int32_t inboundPacketsReceived = 0;
        int32_t inboundPacketsLost = 0;
        
        for (auto it = stats->begin(); it != stats->end(); it++) {
            if (it->type() == std::string("inbound-rtp")) {
                for (auto &member : it->Members()) {
                    if (member->name() == std::string("packetsLost")) {
                        inboundPacketsLost = *(member->cast_to<webrtc::RTCStatsMember<int>>());
                    } else if (member->name() == std::string("packetsReceived")) {
                        inboundPacketsReceived = *(member->cast_to<webrtc::RTCStatsMember<unsigned int>>());
                    }
                }
            }
        }
        
        int32_t deltaPacketsReceived = inboundPacketsReceived - _statsData.packetsReceived;
        int32_t deltaPacketsLost = inboundPacketsLost - _statsData.packetsLost;
        
        _statsData.packetsReceived = inboundPacketsReceived;
        _statsData.packetsLost = inboundPacketsLost;
        
        float signalBarsNorm = 5.0f;
        
        if (deltaPacketsReceived > 0) {
            float lossRate = ((float)deltaPacketsLost) / ((float)deltaPacketsReceived);
            float adjustedLossRate = lossRate / 0.1f;
            adjustedLossRate = fmaxf(0.0f, adjustedLossRate);
            adjustedLossRate = fminf(1.0f, adjustedLossRate);
            float adjustedQuality = 1.0f - adjustedLossRate;
            _signalBarsUpdated((int)(adjustedQuality * signalBarsNorm));
        } else {
            _signalBarsUpdated((int)(1.0f * signalBarsNorm));
        }
    }
    
    void sendPendingServiceMessages(int cause) {
        if (const auto prepared = _signalingConnection->prepareForSendingService(cause)) {
            _signalingDataEmitted(prepared->bytes);
        }
    }
    
    void emitSignaling(const rtc::ByteBufferWriter &buffer) {
        rtc::CopyOnWriteBuffer packet;
        packet.SetData(buffer.Data(), buffer.Length());
        
        if (const auto prepared = _signalingConnection->prepareForSending(Message{ UnstructuredDataMessage{ packet } })) {
            _signalingDataEmitted(prepared->bytes);
        }
    }
    
    void emitIceCandidate(std::string sdp, int mid, std::string sdpMid) {
        rtc::ByteBufferWriter writer;
        writer.WriteUInt8(1);
        writer.WriteUInt32((uint32_t)sdp.size());
        writer.WriteString(sdp);
        writer.WriteUInt32((uint32_t)mid);
        writer.WriteUInt32((uint32_t)sdpMid.size());
        writer.WriteString(sdpMid);
        
        emitSignaling(writer);
    }
    
    void emitOffer() {
        const auto weak = std::weak_ptr<InstanceImplReferenceInternal>(shared_from_this());
        
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        options.offer_to_receive_audio = 1;
        if (_videoCapture) {
            options.offer_to_receive_video = 1;
        }
        
        rtc::scoped_refptr<CreateSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<CreateSessionDescriptionObserverImpl>([weak](std::string sdp, std::string type) {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sdp, type](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                
                webrtc::SdpParseError error;
                webrtc::SessionDescriptionInterface *sessionDescription = webrtc::CreateSessionDescription(type, sdp, &error);
                if (sessionDescription != nullptr) {
                    rtc::scoped_refptr<SetSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<SetSessionDescriptionObserverImpl>([weak, sdp, type]() {
                        auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        strong->emitOfferData(sdp, type);
                    }));
                    strong->_peerConnection->SetLocalDescription(observer, sessionDescription);
                }
            });
        }));
        _peerConnection->CreateOffer(observer, options);
    }
    
    void emitOfferData(std::string sdp, std::string type) {
        rtc::ByteBufferWriter writer;
        writer.WriteUInt8(2);
        writer.WriteUInt32((uint32_t)sdp.size());
        writer.WriteString(sdp);
        writer.WriteUInt32((uint32_t)type.size());
        writer.WriteString(type);
        
        emitSignaling(writer);
    }
    
    void emitAnswerData(std::string sdp, std::string type) {
        rtc::ByteBufferWriter writer;
        writer.WriteUInt8(3);
        writer.WriteUInt32((uint32_t)sdp.size());
        writer.WriteString(sdp);
        writer.WriteUInt32((uint32_t)type.size());
        writer.WriteString(type);
        
        emitSignaling(writer);
    }
    
    void emitAnswer() {
        const auto weak = std::weak_ptr<InstanceImplReferenceInternal>(shared_from_this());
        
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        options.offer_to_receive_audio = 1;
        if (_videoCapture) {
            options.offer_to_receive_video = 1;
        }
        
        rtc::scoped_refptr<CreateSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<CreateSessionDescriptionObserverImpl>([weak](std::string sdp, std::string type) {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sdp, type](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                
                webrtc::SdpParseError error;
                webrtc::SessionDescriptionInterface *sessionDescription = webrtc::CreateSessionDescription(type, sdp, &error);
                if (sessionDescription != nullptr) {
                    rtc::scoped_refptr<SetSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<SetSessionDescriptionObserverImpl>([weak, sdp, type]() {
                        auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        strong->emitAnswerData(sdp, type);
                    }));
                    strong->_peerConnection->SetLocalDescription(observer, sessionDescription);
                }
            });
        }));
        _peerConnection->CreateAnswer(observer, options);

    }
    
    void emitVideoIsActive(bool isActive) {
        rtc::ByteBufferWriter writer;
        writer.WriteUInt8(4);
        writer.WriteUInt8(isActive ? 1 : 0);
        
        emitSignaling(writer);
    }
    
    void emitRequestVideo() {
        rtc::ByteBufferWriter writer;
        writer.WriteUInt8(5);
        
        emitSignaling(writer);
    }
    
    void updateIsConnected(bool isConnected) {
        if (isConnected) {
            _state = State::Established;
            if (!_didConnectOnce) {
                _didConnectOnce = true;
                if (_videoState == VideoState::OutgoingRequested) {
                    _videoState = VideoState::Active;
                }
            }
        } else {
            _state = State::Reconnecting;
        }
        _stateUpdated(_state, _videoState);
    }
    
    void onTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
        if (!_remoteVideoTrack) {
            if (transceiver->media_type() == cricket::MediaType::MEDIA_TYPE_VIDEO) {
                _remoteVideoTrack = static_cast<webrtc::VideoTrackInterface *>(transceiver->receiver()->track().get());
            }
            if (_remoteVideoTrack && _currentSink) {
                _remoteVideoTrack->AddOrUpdateSink(_currentSink.get(), rtc::VideoSinkWants());
            }
        }
    }
    
    void beginSendingVideo() {
        if (!_videoCapture) {
            return;
        }
        
        VideoCaptureInterfaceObject *videoCaptureImpl = GetVideoCaptureAssumingSameThread(_videoCapture.get());
        
        const auto weak = std::weak_ptr<InstanceImplReferenceInternal>(shared_from_this());
        
        videoCaptureImpl->setIsActiveUpdated([weak](bool isActive) {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, isActive](){
                auto strong = weak.lock();
                if (strong) {
                    strong->emitVideoIsActive(isActive);
                }
            });
        });
        
        _localVideoTrack = _nativeFactory->CreateVideoTrack("video0", videoCaptureImpl->_videoSource);
        _peerConnection->AddTrack(_localVideoTrack, _streamIds);
        for (auto &it : _peerConnection->GetTransceivers()) {
            if (it->media_type() == cricket::MediaType::MEDIA_TYPE_VIDEO) {
                auto capabilities = _nativeFactory->GetRtpSenderCapabilities(
                    cricket::MediaType::MEDIA_TYPE_VIDEO);

                std::vector<webrtc::RtpCodecCapability> codecs;
                for (auto &codec : capabilities.codecs) {
                    if (codec.name == cricket::kH265CodecName) {
                        codecs.insert(codecs.begin(), codec);
                    } else {
                        codecs.push_back(codec);
                    }
                }
                it->SetCodecPreferences(codecs);
                
                break;
            }
        }
        
        if (_didConnectOnce && _encryptionKey.isOutgoing) {
            emitOffer();
        }
    }
    
private:
    EncryptionKey _encryptionKey;
    std::vector<RtcServer> _rtcServers;
    bool _enableP2P;
    std::function<void(State, VideoState)> _stateUpdated;
    std::function<void(int)> _signalBarsUpdated;
    std::function<void(const std::vector<uint8_t> &)> _signalingDataEmitted;
    std::function<void(bool)> _remoteVideoIsActiveUpdated;
    std::shared_ptr<VideoCaptureInterface> _videoCapture;
    std::unique_ptr<EncryptedConnection> _signalingConnection;
    
    State _state;
    VideoState _videoState;
    bool _didConnectOnce = false;
    
    std::vector<std::string> _streamIds;
    
    StatsData _statsData;
    
    rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> _nativeFactory;
    std::unique_ptr<PeerConnectionObserverImpl> _observer;
    rtc::scoped_refptr<webrtc::PeerConnectionInterface> _peerConnection;
    std::unique_ptr<webrtc::MediaConstraints> _nativeConstraints;
    rtc::scoped_refptr<webrtc::AudioTrackInterface> _localAudioTrack;
    rtc::scoped_refptr<webrtc::VideoTrackInterface> _localVideoTrack;
    rtc::scoped_refptr<webrtc::VideoTrackInterface> _remoteVideoTrack;
    
    std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> _currentSink;
};

InstanceImplReference::InstanceImplReference(Descriptor &&descriptor) :
onStateUpdated_(std::move(descriptor.stateUpdated)),
onSignalBarsUpdated_(std::move(descriptor.signalBarsUpdated)) {
	internal_.reset(new ThreadLocalObject<InstanceImplReferenceInternal>(getMediaThread(), [descriptor = std::move(descriptor)]() {
        return new InstanceImplReferenceInternal(
            descriptor.encryptionKey,
            descriptor.rtcServers,
            descriptor.config.enableP2P,
            descriptor.stateUpdated,
            descriptor.signalBarsUpdated,
            descriptor.signalingDataEmitted,
            descriptor.remoteVideoIsActiveUpdated,
            descriptor.videoCapture
        );
    }));
    internal_->perform([](InstanceImplReferenceInternal *internal){
        internal->start();
    });
}

InstanceImplReference::~InstanceImplReference() {
	
}

void InstanceImplReference::setNetworkType(NetworkType networkType) {
}

void InstanceImplReference::setMuteMicrophone(bool muteMicrophone) {
    internal_->perform([muteMicrophone = muteMicrophone](InstanceImplReferenceInternal *internal) {
        internal->setMuteMicrophone(muteMicrophone);
    });
}

void InstanceImplReference::receiveSignalingData(const std::vector<uint8_t> &data) {
    internal_->perform([data](InstanceImplReferenceInternal *internal) {
        internal->receiveSignalingData(data);
    });
}

void InstanceImplReference::requestVideo(std::shared_ptr<VideoCaptureInterface> videoCapture) {
    internal_->perform([videoCapture](InstanceImplReferenceInternal *internal) {
        internal->requestVideo(videoCapture);
    });
}

void InstanceImplReference::setIncomingVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
    internal_->perform([sink](InstanceImplReferenceInternal *internal) {
        internal->setIncomingVideoOutput(sink);
    });
}

void InstanceImplReference::setAudioOutputGainControlEnabled(bool enabled) {
}

void InstanceImplReference::setEchoCancellationStrength(int strength) {
}

void InstanceImplReference::setAudioInputDevice(std::string id) {
}

void InstanceImplReference::setAudioOutputDevice(std::string id) {
}

void InstanceImplReference::setInputVolume(float level) {
}

void InstanceImplReference::setOutputVolume(float level) {
}

void InstanceImplReference::setAudioOutputDuckingEnabled(bool enabled) {
}

int InstanceImplReference::GetConnectionMaxLayer() {
    return 92;
}

std::string InstanceImplReference::GetVersion() {
    return "2.8.8";
}

std::string InstanceImplReference::getLastError() {
	return "ERROR_UNKNOWN";
}

std::string InstanceImplReference::getDebugInfo() {
	return "";
}

int64_t InstanceImplReference::getPreferredRelayId() {
    return 0;
}

TrafficStats InstanceImplReference::getTrafficStats() {
	auto result = TrafficStats();
	return result;
}

PersistentState InstanceImplReference::getPersistentState() {
	return PersistentState();
}

FinalState InstanceImplReference::stop() {
	auto result = FinalState();

	result.persistentState = getPersistentState();
	result.debugLog = "";
	result.trafficStats = getTrafficStats();
	result.isRatingSuggested = false;

	return result;
}

template <>
bool Register<InstanceImplReference>() {
	return Meta::RegisterOne<InstanceImplReference>();
}

} // namespace tgcalls
