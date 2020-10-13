#include "GroupInstanceImpl.h"

#include <memory>
#include "api/scoped_refptr.h"
#include "rtc_base/thread.h"
#include "rtc_base/logging.h"
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
#include "LogSinkImpl.h"

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

class FrameEncryptorImpl : public webrtc::FrameEncryptorInterface {
public:
    FrameEncryptorImpl() {
    }
    
    virtual int Encrypt(cricket::MediaType media_type,
                        uint32_t ssrc,
                        rtc::ArrayView<const uint8_t> additional_data,
                        rtc::ArrayView<const uint8_t> frame,
                        rtc::ArrayView<uint8_t> encrypted_frame,
                        size_t* bytes_written) override {
        memcpy(encrypted_frame.data(), frame.data(), frame.size());
        for (auto it = encrypted_frame.begin(); it != encrypted_frame.end(); it++) {
            *it ^= 123;
        }
        *bytes_written = frame.size();
        return 0;
    }

    virtual size_t GetMaxCiphertextByteSize(cricket::MediaType media_type,
                                            size_t frame_size) override {
        return frame_size;
    }
};

class FrameDecryptorImpl : public webrtc::FrameDecryptorInterface {
public:
    FrameDecryptorImpl() {
    }
    
    virtual webrtc::FrameDecryptorInterface::Result Decrypt(cricket::MediaType media_type,
                           const std::vector<uint32_t>& csrcs,
                           rtc::ArrayView<const uint8_t> additional_data,
                           rtc::ArrayView<const uint8_t> encrypted_frame,
                           rtc::ArrayView<uint8_t> frame) override {
        memcpy(frame.data(), encrypted_frame.data(), encrypted_frame.size());
        for (auto it = frame.begin(); it != frame.end(); it++) {
            *it ^= 123;
        }
        return webrtc::FrameDecryptorInterface::Result(webrtc::FrameDecryptorInterface::Status::kOk, encrypted_frame.size());
    }

    virtual size_t GetMaxPlaintextByteSize(cricket::MediaType media_type,
                                           size_t encrypted_frame_size) override {
        return encrypted_frame_size;
    }
};

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
        if (transceiver->receiver()) {
            rtc::scoped_refptr<FrameDecryptorImpl> decryptor(new rtc::RefCountedObject<FrameDecryptorImpl>());
            transceiver->receiver()->SetFrameDecryptor(decryptor);
        }
        
        _onTrack(transceiver);
    }

    virtual void OnRemoveTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
    }

    virtual void OnInterestingUsage(int usage_pattern) {
    }
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

} // namespace

class GroupInstanceManager : public std::enable_shared_from_this<GroupInstanceManager> {
public:
	GroupInstanceManager(GroupInstanceDescriptor &&descriptor) :
    _sdpAnswerEmitted(descriptor.sdpAnswerEmitted) {
	}

	~GroupInstanceManager() {
        assert(getMediaThread()->IsCurrent());

        if (_peerConnection) {
            _peerConnection->Close();
        }
	}

	void start() {
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());

        PlatformInterface::SharedInstance()->configurePlatformAudio();
        
        _streamIds.push_back("stream0");
        
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
        //config.continual_gathering_policy = webrtc::PeerConnectionInterface::ContinualGatheringPolicy::GATHER_CONTINUALLY;
        /*config.audio_jitter_buffer_fast_accelerate = true;
        config.prioritize_most_likely_ice_candidate_pairs = true;
        config.presume_writable_when_fully_relayed = true;
        config.audio_jitter_buffer_enable_rtx_handling = true;*/
        
        webrtc::CryptoOptions cryptoOptions;
        webrtc::CryptoOptions::SFrame sframe;
        sframe.require_frame_encryption = true;
        cryptoOptions.sframe = sframe;
        config.crypto_options = cryptoOptions;

        _observer.reset(new PeerConnectionObserverImpl(
            [weak](std::string sdp, int mid, std::string sdpMid) {
                getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sdp, mid, sdpMid](){
                    auto strong = weak.lock();
                    if (strong) {
                        //strong->emitIceCandidate(sdp, mid, sdpMid);
                    }
                });
            },
            [weak](bool isConnected) {
                getMediaThread()->PostTask(RTC_FROM_HERE, [weak, isConnected](){
                    auto strong = weak.lock();
                    if (strong) {
                        //strong->updateIsConnected(isConnected);
                    }
                });
            },
            [weak](rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
                getMediaThread()->PostTask(RTC_FROM_HERE, [weak, transceiver](){
                    auto strong = weak.lock();
                    if (!strong) {
                        return;
                    }
                    //strong->onTrack(transceiver);
                });
            }
        ));
        _peerConnection = _nativeFactory->CreatePeerConnection(config, nullptr, nullptr, _observer.get());
        assert(_peerConnection != nullptr);

        for (int i = 0; i < 2; i++) {
            cricket::AudioOptions options;
            rtc::scoped_refptr<webrtc::AudioSourceInterface> audioSource = _nativeFactory->CreateAudioSource(options);
            std::stringstream name;
            name << "audio";
            name << i;
            rtc::scoped_refptr<webrtc::AudioTrackInterface> localAudioTrack = _nativeFactory->CreateAudioTrack(name.str(), audioSource);
            _peerConnection->AddTrack(localAudioTrack, _streamIds);
        }
        
        for (auto &it : _peerConnection->GetTransceivers()) {
            if (it->sender()) {
                rtc::scoped_refptr<FrameEncryptorImpl> encryptor(new rtc::RefCountedObject<FrameEncryptorImpl>());
                it->sender()->SetFrameEncryptor(encryptor);
            }
        }
	}
    
    void setOfferSdp(std::string const &offerSdp) {
        if (_appliedRemoteRescription == offerSdp) {
            return;
        }
        _appliedRemoteRescription = offerSdp;
        
        webrtc::SdpParseError error;
        webrtc::SessionDescriptionInterface *sessionDescription = webrtc::CreateSessionDescription("offer", offerSdp, &error);
        if (!sessionDescription) {
            return;
        }
        
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
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
    
    void emitAnswer() {
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());

        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        options.offer_to_receive_audio = 1;

        rtc::scoped_refptr<CreateSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<CreateSessionDescriptionObserverImpl>([weak](std::string sdp, std::string type) {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sdp, type](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }

                webrtc::SdpParseError error;
                webrtc::SessionDescriptionInterface *sessionDescription = webrtc::CreateSessionDescription(type, sdp, &error);
                if (sessionDescription != nullptr) {
                    rtc::scoped_refptr<SetSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<SetSessionDescriptionObserverImpl>([weak, sdp]() {
                        auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        if (!strong->_didEmitAnswer) {
                            strong->_didEmitAnswer = true;
                            strong->_sdpAnswerEmitted(std::string(sdp));
                        }
                    }));
                    strong->_peerConnection->SetLocalDescription(observer, sessionDescription);
                }
            });
        }));
        _peerConnection->CreateAnswer(observer, options);
    }

private:
    std::function<void(std::string const &)> _sdpAnswerEmitted;
    
    bool _didEmitAnswer = false;
    std::string _appliedRemoteRescription;
    
    std::vector<std::string> _streamIds;
    rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> _nativeFactory;
    std::unique_ptr<PeerConnectionObserverImpl> _observer;
    rtc::scoped_refptr<webrtc::PeerConnectionInterface> _peerConnection;
    std::unique_ptr<webrtc::MediaConstraints> _nativeConstraints;
};

GroupInstanceImpl::GroupInstanceImpl(GroupInstanceDescriptor &&descriptor) {
    rtc::LogMessage::LogToDebug(rtc::LS_INFO);
    rtc::LogMessage::SetLogToStderr(true);
    if (_logSink) {
		rtc::LogMessage::AddLogToStream(_logSink.get(), rtc::LS_INFO);
	}
    
	_manager.reset(new ThreadLocalObject<GroupInstanceManager>(getMediaThread(), [descriptor = std::move(descriptor)]() mutable {
		return new GroupInstanceManager(std::move(descriptor));
	}));
	_manager->perform(RTC_FROM_HERE, [](GroupInstanceManager *manager) {
		manager->start();
	});
}

GroupInstanceImpl::~GroupInstanceImpl() {
	if (_logSink) {
		rtc::LogMessage::RemoveLogToStream(_logSink.get());
	}
}

void GroupInstanceImpl::setOfferSdp(std::string const &offerSdp) {
    _manager->perform(RTC_FROM_HERE, [offerSdp](GroupInstanceManager *manager) {
        manager->setOfferSdp(offerSdp);
    });
}

} // namespace tgcalls
