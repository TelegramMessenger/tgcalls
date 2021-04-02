#include "v2/InstanceV2Impl.h"

#include "LogSinkImpl.h"
#include "VideoCaptureInterfaceImpl.h"
#include "VideoCapturerInterface.h"
#include "v2/NativeNetworkingImpl.h"
#include "v2/Signaling.h"

#include "CodecSelectHelper.h"
#include "platform/PlatformInterface.h"

#include "api/audio_codecs/audio_decoder_factory_template.h"
#include "api/audio_codecs/audio_encoder_factory_template.h"
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_decoder_multi_channel_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"
#include "api/audio_codecs/L16/audio_decoder_L16.h"
#include "api/audio_codecs/L16/audio_encoder_L16.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "media/engine/webrtc_media_engine.h"
#include "system_wrappers/include/field_trial.h"
#include "api/video/builtin_video_bitrate_allocator_factory.h"
#include "call/call.h"
#include "modules/rtp_rtcp/source/rtp_utility.h"
#include "api/call/audio_sink.h"
#include "modules/audio_processing/audio_buffer.h"
#include "absl/strings/match.h"
#include "modules/audio_processing/agc2/vad_with_level.h"
#include "pc/channel_manager.h"
#include "media/base/rtp_data_engine.h"
#include "audio/audio_state.h"
#include "modules/audio_coding/neteq/default_neteq_factory.h"
#include "modules/audio_coding/include/audio_coding_module.h"
#include "api/candidate.h"

#include "AudioFrame.h"
#include "ThreadLocalObject.h"
#include "Manager.h"
#include "NetworkManager.h"
#include "VideoCaptureInterfaceImpl.h"
#include "platform/PlatformInterface.h"
#include "LogSinkImpl.h"
#include "CodecSelectHelper.h"
#include "AudioDeviceHelper.h"

#include <random>
#include <sstream>

namespace tgcalls {
namespace {

static int stringToInt(std::string const &string) {
    std::stringstream stringStream(string);
    int value = 0;
    stringStream >> value;
    return value;
}

class OutgoingAudioChannel : public sigslot::has_slots<> {
public:
    OutgoingAudioChannel(
        uint32_t ssrc,
        webrtc::Call *call,
        cricket::ChannelManager *channelManager,
        rtc::UniqueRandomIdGenerator *uniqueRandomIdGenerator,
        webrtc::LocalAudioSinkAdapter *audioSource,
        webrtc::RtpTransport *rtpTransport,
        std::shared_ptr<Threads> threads
    ) :
    _ssrc(ssrc),
    _call(call),
    _channelManager(channelManager),
    _audioSource(audioSource) {
        cricket::AudioOptions audioOptions;
        bool _disableOutgoingAudioProcessing = false;

        if (_disableOutgoingAudioProcessing) {
            audioOptions.echo_cancellation = false;
            audioOptions.noise_suppression = false;
            audioOptions.auto_gain_control = false;
            audioOptions.highpass_filter = false;
            audioOptions.typing_detection = false;
            audioOptions.experimental_agc = false;
            audioOptions.experimental_ns = false;
            audioOptions.residual_echo_detector = false;
        } else {
            audioOptions.echo_cancellation = true;
            audioOptions.noise_suppression = true;
        }

        std::vector<std::string> streamIds;
        streamIds.push_back("1");

        _outgoingAudioChannel = _channelManager->CreateVoiceChannel(call, cricket::MediaConfig(), rtpTransport, threads->getMediaThread(), "0", false, NativeNetworkingImpl::getDefaulCryptoOptions(), uniqueRandomIdGenerator, audioOptions);

        const uint8_t opusMinBitrateKbps = 16;
        const uint8_t opusMaxBitrateKbps = 32;
        const uint8_t opusStartBitrateKbps = 32;
        const uint8_t opusPTimeMs = 60;

        cricket::AudioCodec opusCodec(111, "opus", 48000, 0, 2);
        opusCodec.AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamTransportCc));
        opusCodec.SetParam(cricket::kCodecParamMinBitrate, opusMinBitrateKbps);
        opusCodec.SetParam(cricket::kCodecParamStartBitrate, opusStartBitrateKbps);
        opusCodec.SetParam(cricket::kCodecParamMaxBitrate, opusMaxBitrateKbps);
        opusCodec.SetParam(cricket::kCodecParamUseInbandFec, 1);
        opusCodec.SetParam(cricket::kCodecParamPTime, opusPTimeMs);

        auto outgoingAudioDescription = std::make_unique<cricket::AudioContentDescription>();
        outgoingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(webrtc::RtpExtension::kAudioLevelUri, 1));
        outgoingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(webrtc::RtpExtension::kAbsSendTimeUri, 2));
        outgoingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(webrtc::RtpExtension::kTransportSequenceNumberUri, 3));
        outgoingAudioDescription->set_rtcp_mux(true);
        outgoingAudioDescription->set_rtcp_reduced_size(true);
        outgoingAudioDescription->set_direction(webrtc::RtpTransceiverDirection::kSendOnly);
        outgoingAudioDescription->set_codecs({ opusCodec });
        outgoingAudioDescription->set_bandwidth(1032000);
        outgoingAudioDescription->AddStream(cricket::StreamParams::CreateLegacy(ssrc));

        auto incomingAudioDescription = std::make_unique<cricket::AudioContentDescription>();
        incomingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(webrtc::RtpExtension::kAudioLevelUri, 1));
        incomingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(webrtc::RtpExtension::kAbsSendTimeUri, 2));
        incomingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(webrtc::RtpExtension::kTransportSequenceNumberUri, 3));
        incomingAudioDescription->set_rtcp_mux(true);
        incomingAudioDescription->set_rtcp_reduced_size(true);
        incomingAudioDescription->set_direction(webrtc::RtpTransceiverDirection::kRecvOnly);
        incomingAudioDescription->set_codecs({ opusCodec });
        incomingAudioDescription->set_bandwidth(1032000);

        _outgoingAudioChannel->SetPayloadTypeDemuxingEnabled(false);
        _outgoingAudioChannel->SetLocalContent(outgoingAudioDescription.get(), webrtc::SdpType::kOffer, nullptr);
        _outgoingAudioChannel->SetRemoteContent(incomingAudioDescription.get(), webrtc::SdpType::kAnswer, nullptr);

        _outgoingAudioChannel->SignalSentPacket().connect(this, &OutgoingAudioChannel::OnSentPacket_w);
        _outgoingAudioChannel->UpdateRtpTransport(nullptr);

        setIsMuted(false);
    }

    ~OutgoingAudioChannel() {
        _outgoingAudioChannel->SignalSentPacket().disconnect(this);
        _outgoingAudioChannel->media_channel()->SetAudioSend(_ssrc, false, nullptr, _audioSource);
        _outgoingAudioChannel->Enable(false);
        _channelManager->DestroyVoiceChannel(_outgoingAudioChannel);
        _outgoingAudioChannel = nullptr;
    }

    void setIsMuted(bool isMuted) {
        if (_isMuted != isMuted) {
            _isMuted = false;

            _outgoingAudioChannel->Enable(!_isMuted);
            _outgoingAudioChannel->media_channel()->SetAudioSend(_ssrc, !_isMuted, nullptr, _audioSource);
        }
    }

private:
    void OnSentPacket_w(const rtc::SentPacket& sent_packet) {
        _call->OnSentPacket(sent_packet);
    }

private:
    uint32_t _ssrc = 0;
    webrtc::Call *_call = nullptr;
    cricket::ChannelManager *_channelManager = nullptr;
    webrtc::LocalAudioSinkAdapter *_audioSource = nullptr;
    cricket::VoiceChannel *_outgoingAudioChannel = nullptr;

    bool _isMuted = true;
};

class IncomingAudioChannel : public sigslot::has_slots<> {
public:
    IncomingAudioChannel(
        cricket::ChannelManager *channelManager,
        webrtc::Call *call,
        webrtc::RtpTransport *rtpTransport,
        rtc::UniqueRandomIdGenerator *randomIdGenerator,
        signaling::MediaContent const &mediaContent,
        //std::function<void(AudioSinkImpl::Update)> &&onAudioLevelUpdated,
        //std::function<void(uint32_t, const AudioFrame &)> onAudioFrame,
        std::shared_ptr<Threads> threads) :
    _ssrc(mediaContent.ssrc),
    _channelManager(channelManager),
    _call(call) {
        _creationTimestamp = rtc::TimeMillis();

        cricket::AudioOptions audioOptions;
        audioOptions.audio_jitter_buffer_fast_accelerate = true;
        audioOptions.audio_jitter_buffer_min_delay_ms = 50;

        std::string streamId = std::string("stream1");

        _audioChannel = _channelManager->CreateVoiceChannel(call, cricket::MediaConfig(), rtpTransport, threads->getMediaThread(), std::string("audio1"), false, NativeNetworkingImpl::getDefaulCryptoOptions(), randomIdGenerator, audioOptions);

        std::vector<cricket::AudioCodec> audioCodecs;

        for (const auto &payloadType : mediaContent.payloadTypes) {
            cricket::AudioCodec opusCodec(payloadType.id, payloadType.name, payloadType.clockrate, 0, payloadType.channels);
            for (const auto &it : payloadType.parameters) {
                if (it.first == "useinbandfec") {
                    opusCodec.SetParam(cricket::kCodecParamUseInbandFec, stringToInt(it.second));
                } else if (it.first == "minptime") {
                    opusCodec.SetParam(cricket::kCodecParamPTime, stringToInt(it.second));
                }
            }
        }

        auto outgoingAudioDescription = std::make_unique<cricket::AudioContentDescription>();
        outgoingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(webrtc::RtpExtension::kAudioLevelUri, 1));
        outgoingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(webrtc::RtpExtension::kAbsSendTimeUri, 2));
        outgoingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(webrtc::RtpExtension::kTransportSequenceNumberUri, 3));
        outgoingAudioDescription->set_rtcp_mux(true);
        outgoingAudioDescription->set_rtcp_reduced_size(true);
        outgoingAudioDescription->set_direction(webrtc::RtpTransceiverDirection::kRecvOnly);
        outgoingAudioDescription->set_codecs(audioCodecs);
        outgoingAudioDescription->set_bandwidth(1032000);

        auto incomingAudioDescription = std::make_unique<cricket::AudioContentDescription>();
        incomingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(webrtc::RtpExtension::kAudioLevelUri, 1));
        incomingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(webrtc::RtpExtension::kAbsSendTimeUri, 2));
        incomingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(webrtc::RtpExtension::kTransportSequenceNumberUri, 3));
        incomingAudioDescription->set_rtcp_mux(true);
        incomingAudioDescription->set_rtcp_reduced_size(true);
        incomingAudioDescription->set_direction(webrtc::RtpTransceiverDirection::kSendOnly);
        incomingAudioDescription->set_codecs(audioCodecs);
        incomingAudioDescription->set_bandwidth(1032000);
        cricket::StreamParams streamParams = cricket::StreamParams::CreateLegacy(mediaContent.ssrc);
        streamParams.set_stream_ids({ streamId });
        incomingAudioDescription->AddStream(streamParams);

        _audioChannel->SetPayloadTypeDemuxingEnabled(false);
        _audioChannel->SetLocalContent(outgoingAudioDescription.get(), webrtc::SdpType::kOffer, nullptr);
        _audioChannel->SetRemoteContent(incomingAudioDescription.get(), webrtc::SdpType::kAnswer, nullptr);

        outgoingAudioDescription.reset();
        incomingAudioDescription.reset();

        //std::unique_ptr<AudioSinkImpl> audioLevelSink(new AudioSinkImpl(onAudioLevelUpdated, _ssrc, std::move(onAudioFrame)));
        //_audioChannel->media_channel()->SetRawAudioSink(ssrc.networkSsrc, std::move(audioLevelSink));

        _audioChannel->SignalSentPacket().connect(this, &IncomingAudioChannel::OnSentPacket_w);
        _audioChannel->UpdateRtpTransport(nullptr);

        _audioChannel->Enable(true);
    }

    ~IncomingAudioChannel() {
        _audioChannel->SignalSentPacket().disconnect(this);
        _audioChannel->Enable(false);
        _channelManager->DestroyVoiceChannel(_audioChannel);
        _audioChannel = nullptr;
    }

    void setVolume(double value) {
        _audioChannel->media_channel()->SetOutputVolume(_ssrc, value);
    }

    void updateActivity() {
        _activityTimestamp = rtc::TimeMillis();
    }

    int64_t getActivity() {
        return _activityTimestamp;
    }

private:
    void OnSentPacket_w(const rtc::SentPacket& sent_packet) {
        _call->OnSentPacket(sent_packet);
    }

private:
    uint32_t _ssrc = 0;
    // Memory is managed by _channelManager
    cricket::VoiceChannel *_audioChannel = nullptr;
    // Memory is managed externally
    cricket::ChannelManager *_channelManager = nullptr;
    webrtc::Call *_call = nullptr;
    int64_t _creationTimestamp = 0;
    int64_t _activityTimestamp = 0;
};

} // namespace

class InstanceV2ImplInternal : public std::enable_shared_from_this<InstanceV2ImplInternal> {
public:
    InstanceV2ImplInternal(Descriptor &&descriptor, std::shared_ptr<Threads> threads) :
    _threads(threads),
    _rtcServers(descriptor.rtcServers),
    _encryptionKey(std::move(descriptor.encryptionKey)),
    _stateUpdated(descriptor.stateUpdated),
    _signalBarsUpdated(descriptor.signalBarsUpdated),
    _audioLevelUpdated(descriptor.audioLevelUpdated),
    _remoteBatteryLevelIsLowUpdated(descriptor.remoteBatteryLevelIsLowUpdated),
    _remoteMediaStateUpdated(descriptor.remoteMediaStateUpdated),
    _remotePrefferedAspectRatioUpdated(descriptor.remotePrefferedAspectRatioUpdated),
    _signalingDataEmitted(descriptor.signalingDataEmitted),
    _createAudioDeviceModule(descriptor.createAudioDeviceModule),
    _eventLog(std::make_unique<webrtc::RtcEventLogNull>()),
    _taskQueueFactory(webrtc::CreateDefaultTaskQueueFactory()) {
        auto generator = std::mt19937(std::random_device()());
        auto distribution = std::uniform_int_distribution<uint32_t>();
        do {
            _outgoingAudioSsrc = distribution(generator) & 0x7fffffffU;
        } while (!_outgoingAudioSsrc);
    }

    ~InstanceV2ImplInternal() {
        _networking->perform(RTC_FROM_HERE, [](NativeNetworkingImpl *networking) {
            networking->stop();
        });
        _threads->getNetworkThread()->Invoke<void>(RTC_FROM_HERE, []() {
        });
    }

    void start() {
        const auto weak = std::weak_ptr<InstanceV2ImplInternal>(shared_from_this());

        _networking.reset(new ThreadLocalObject<NativeNetworkingImpl>(_threads->getNetworkThread(), [weak, threads = _threads, isOutgoing = _encryptionKey.isOutgoing, rtcServers = _rtcServers]() {
            return new NativeNetworkingImpl((NativeNetworkingImpl::Configuration){
                .isOutgoing = isOutgoing,
                .enableStunMarking = false,
                .enableTCP = false,
                .enableP2P = true,
                .rtcServers = rtcServers,
                .stateUpdated = [threads, weak](const NativeNetworkingImpl::State &state) {
                    threads->getMediaThread()->PostTask(RTC_FROM_HERE, [=] {
                        const auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        strong->onNetworkStateUpdated(state);
                    });
                },
                .candidateGathered = [threads, weak](const cricket::Candidate &candidate) {
                    threads->getMediaThread()->PostTask(RTC_FROM_HERE, [=] {
                        const auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }

                        strong->sendCandidate(candidate);
                    });
                },
                .transportMessageReceived = [threads, weak](rtc::CopyOnWriteBuffer const &packet, bool isMissing) {
                    threads->getMediaThread()->PostTask(RTC_FROM_HERE, [=] {
                        const auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                    });
                },
                .rtcpPacketReceived = [threads, weak](rtc::CopyOnWriteBuffer const &packet, int64_t timestamp) {
                    threads->getMediaThread()->PostTask(RTC_FROM_HERE, [=] {
                        const auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                    });
                },
                .dataChannelStateUpdated = [threads, weak](bool isDataChannelOpen) {
                    threads->getMediaThread()->PostTask(RTC_FROM_HERE, [=] {
                        const auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        strong->onDataChannelStateUpdated(isDataChannelOpen);
                    });
                },
                .dataChannelMessageReceived = [threads, weak](std::string const &message) {
                    threads->getMediaThread()->PostTask(RTC_FROM_HERE, [=] {
                        const auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        strong->onDataChannelMessage(message);
                    });
                },
                .threads = threads
            });
        }));

        PlatformInterface::SharedInstance()->configurePlatformAudio();

        cricket::MediaEngineDependencies mediaDeps;
        mediaDeps.task_queue_factory = _taskQueueFactory.get();
        mediaDeps.audio_encoder_factory = webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus, webrtc::AudioEncoderL16>();
        mediaDeps.audio_decoder_factory = webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus, webrtc::AudioDecoderL16>();

        mediaDeps.video_encoder_factory = PlatformInterface::SharedInstance()->makeVideoEncoderFactory();
        mediaDeps.video_decoder_factory = PlatformInterface::SharedInstance()->makeVideoDecoderFactory();

        _audioDeviceModule = createAudioDeviceModule();
        if (!_audioDeviceModule) {
            return;
        }
        mediaDeps.adm = _audioDeviceModule;

        _availableVideoFormats = mediaDeps.video_encoder_factory->GetSupportedFormats();

        std::unique_ptr<cricket::MediaEngineInterface> mediaEngine = cricket::CreateMediaEngine(std::move(mediaDeps));

        _channelManager.reset(new cricket::ChannelManager(std::move(mediaEngine), std::make_unique<cricket::RtpDataEngine>(), _threads->getMediaThread(), _threads->getNetworkThread()));
        _channelManager->Init();

        //setAudioInputDevice(_initialInputDeviceId);
        //setAudioOutputDevice(_initialOutputDeviceId);

        webrtc::Call::Config callConfig(_eventLog.get());
        callConfig.task_queue_factory = _taskQueueFactory.get();
        callConfig.trials = &_fieldTrials;
        callConfig.audio_state = _channelManager->media_engine()->voice().GetAudioState();
        _call.reset(webrtc::Call::Create(callConfig));

        _uniqueRandomIdGenerator.reset(new rtc::UniqueRandomIdGenerator());

        _threads->getNetworkThread()->Invoke<void>(RTC_FROM_HERE, [this]() {
            _rtpTransport = _networking->getSyncAssumingSameThread()->getRtpTransport();
        });

        _videoBitrateAllocatorFactory = webrtc::CreateBuiltinVideoBitrateAllocatorFactory();

        /*if (_enableVideo) {
            _outgoingVideoChannel = _channelManager->CreateVideoChannel(_call.get(), cricket::MediaConfig(), _rtpTransport, _threads->getMediaThread(), "1", false, GroupNetworkManager::getDefaulCryptoOptions(), _uniqueRandomIdGenerator.get(), cricket::VideoOptions(), _videoBitrateAllocatorFactory.get());
        }

        configureSendVideo();

        if (_outgoingVideoChannel) {
            _outgoingVideoChannel->SignalSentPacket().connect(this, &GroupInstanceCustomInternal::OnSentPacket_w);
            _outgoingVideoChannel->UpdateRtpTransport(nullptr);
        }

        if (_audioLevelsUpdated) {
            beginLevelsTimer(50);
        }

        if (_videoCapture) {
            setVideoCapture(_videoCapture, [](GroupJoinPayload) {}, true);
        }

        if (_useDummyChannel) {
            addIncomingAudioChannel("_dummy", ChannelId(1), true);
        }*/

        //beginNetworkStatusTimer(0);

        //adjustBitratePreferences(true);

        _outgoingAudioChannel.reset(new OutgoingAudioChannel(
            _outgoingAudioSsrc,
            _call.get(),
            _channelManager.get(),
            _uniqueRandomIdGenerator.get(),
            &_audioSource,
            _rtpTransport,
            _threads
        ));

        _networking->perform(RTC_FROM_HERE, [](NativeNetworkingImpl *networking) {
            networking->start();
        });

        beginSignaling();
    }

    void sendSignalingMessage(signaling::Message const &message) {
        _signalingDataEmitted(message.serialize());
    }

    void beginSignaling() {
        const auto weak = std::weak_ptr<InstanceV2ImplInternal>(shared_from_this());

        _networking->perform(RTC_FROM_HERE, [weak, threads = _threads, isOutgoing = _encryptionKey.isOutgoing](NativeNetworkingImpl *networking) {
            auto localFingerprint = networking->getLocalFingerprint();
            std::string hash = localFingerprint->algorithm;
            std::string fingerprint = localFingerprint->GetRfc4572Fingerprint();
            std::string setup = isOutgoing ? "passive" : "active";

            auto localIceParams = networking->getLocalIceParameters();
            std::string ufrag = localIceParams.ufrag;
            std::string pwd = localIceParams.pwd;

            threads->getMediaThread()->PostTask(RTC_FROM_HERE, [weak, ufrag, pwd, hash, fingerprint, setup, localIceParams]() {
                const auto strong = weak.lock();
                if (!strong) {
                    return;
                }

                signaling::InitialSetupMessage data;

                signaling::MediaContent audioContent;
                audioContent.ssrc = strong->_outgoingAudioSsrc;

                signaling::PayloadType audioPayloadType;
                audioPayloadType.id = 111;
                audioPayloadType.name = "opus";
                audioPayloadType.channels = 2;
                audioPayloadType.clockrate = 48000;
                audioPayloadType.parameters.push_back(std::make_pair("minptime", "10"));
                audioPayloadType.parameters.push_back(std::make_pair("useinbandfec", "1"));

                audioContent.payloadTypes.push_back(std::move(audioPayloadType));

                data.audio = audioContent;

                data.ufrag = ufrag;
                data.pwd = pwd;

                signaling::DtlsFingerprint dtlsFingerprint;
                dtlsFingerprint.hash = hash;
                dtlsFingerprint.fingerprint = fingerprint;
                dtlsFingerprint.setup = setup;
                data.fingerprints.push_back(std::move(dtlsFingerprint));

                signaling::Message message;
                message.data = std::move(data);
                strong->sendSignalingMessage(message);
            });
        });
    }

    void receiveSignalingData(const std::vector<uint8_t> &data) {
        const auto message = signaling::Message::parse(data);
        if (!message) {
            return;
        }
        const auto messageData = &message->data;
        if (const auto initialSetup = absl::get_if<signaling::InitialSetupMessage>(messageData)) {
            PeerIceParameters remoteIceParameters;
            remoteIceParameters.ufrag = initialSetup->ufrag;
            remoteIceParameters.pwd = initialSetup->pwd;

            std::unique_ptr<rtc::SSLFingerprint> fingerprint;
            if (initialSetup->fingerprints.size() != 0) {
                fingerprint = rtc::SSLFingerprint::CreateUniqueFromRfc4572(initialSetup->fingerprints[0].hash, initialSetup->fingerprints[0].fingerprint);
            }

            _networking->perform(RTC_FROM_HERE, [threads = _threads, remoteIceParameters = std::move(remoteIceParameters), fingerprint = std::move(fingerprint)](NativeNetworkingImpl *networking) {
                networking->setRemoteParams(remoteIceParameters, fingerprint.get());
            });

            if (const auto audio = initialSetup->audio) {
                _incomingAudioChannel.reset(new IncomingAudioChannel(
                    _channelManager.get(),
                    _call.get(),
                    _rtpTransport,
                    _uniqueRandomIdGenerator.get(),
                    audio.value(),
                    //std::function<void(AudioSinkImpl::Update)> &&onAudioLevelUpdated,
                    //std::function<void(uint32_t, const AudioFrame &)> onAudioFrame,
                    _threads
                ));
            }

            _handshakeCompleted = true;
            commitPendingIceCandidates();
        } else if (const auto candidatesList = absl::get_if<signaling::CandidatesMessage>(messageData)) {
            for (const auto &candidate : candidatesList->iceCandidates) {
                rtc::SocketAddress address(candidate.ip, candidate.port);

                cricket::Candidate parsedCandidate(
                    /*component=*/candidate.component,
                    /*protocol=*/candidate.protocol,
                    /*address=*/address,
                    /*priority=*/candidate.priority,
                    /*username=*/candidate.username,
                    /*password=*/candidate.password,
                    /*type=*/candidate.type,
                    /*generation=*/candidate.generation,
                    /*foundation=*/candidate.foundation,
                    /*network_id=*/candidate.networkId,
                    /*network_cost=*/candidate.networkCost
                );
                _pendingIceCandidates.push_back(std::move(parsedCandidate));
            }

            if (_handshakeCompleted) {
                commitPendingIceCandidates();
            }
        }
    }

    void commitPendingIceCandidates() {
        if (_pendingIceCandidates.size() == 0) {
            return;
        }
        _networking->perform(RTC_FROM_HERE, [threads = _threads, parsedCandidates = _pendingIceCandidates](NativeNetworkingImpl *networking) {
            networking->addCandidates(parsedCandidates);
        });
        _pendingIceCandidates.clear();
    }

    void onNetworkStateUpdated(NativeNetworkingImpl::State const &state) {
        State mappedState;
        if (state.isReadyToSendData) {
            mappedState = State::Established;
        } else {
            mappedState = State::Reconnecting;
        }
        _stateUpdated(mappedState);
    }

    void onDataChannelStateUpdated(bool isDataChannelOpen) {
        if (_isDataChannelOpen != isDataChannelOpen) {
            _isDataChannelOpen = isDataChannelOpen;

            if (_isDataChannelOpen) {
                std::ostringstream s;
                s << "hello from " << _outgoingAudioSsrc;
                sendDataChannelMessage(s.str());
            }
        }
    }

    void sendDataChannelMessage(std::string const &message) {
        if (!_isDataChannelOpen) {
            RTC_LOG(LS_ERROR) << "sendDataChannelMessage called, but data channel is not open";
            return;
        }
        _networking->perform(RTC_FROM_HERE, [message](NativeNetworkingImpl *networking) {
            networking->sendDataChannelMessage(message);
        });
    }

    void onDataChannelMessage(std::string const &message) {
        RTC_LOG(LS_INFO) << "dataChannelMessage received: " << message;
    }

    void sendCandidate(const cricket::Candidate &candidate) {
        signaling::CandidatesMessage data;

        signaling::IceCandidate serializedCandidate;

        serializedCandidate.component = candidate.component();
        serializedCandidate.protocol = candidate.protocol();
        if (candidate.address().hostname().size() != 0) {
            serializedCandidate.ip = candidate.address().hostname();
        } else {
            serializedCandidate.ip = candidate.address().ipaddr().ToString();
        }
        serializedCandidate.port = candidate.address().port();
        serializedCandidate.priority = candidate.priority();
        serializedCandidate.username = candidate.username();
        serializedCandidate.password = candidate.password();
        serializedCandidate.type = candidate.type();
        serializedCandidate.generation = candidate.generation();
        serializedCandidate.foundation = candidate.foundation();
        serializedCandidate.networkId = candidate.network_id();
        serializedCandidate.networkCost = candidate.network_cost();

        data.iceCandidates.push_back(std::move(serializedCandidate));

        signaling::Message message;
        message.data = std::move(data);
        sendSignalingMessage(message);
    }

    void setVideoCapture(std::shared_ptr<VideoCaptureInterface> videoCapture) {

    }

    void setRequestedVideoAspect(float aspect) {

    }

    void setNetworkType(NetworkType networkType) {

    }

    void setMuteMicrophone(bool muteMicrophone) {
        if (_outgoingAudioChannel) {
            _outgoingAudioChannel->setIsMuted(muteMicrophone);
        }
    }

    void setIncomingVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {

    }


    void setAudioInputDevice(std::string id) {

    }

    void setAudioOutputDevice(std::string id) {

    }

    void setIsLowBatteryLevel(bool isLowBatteryLevel) {

    }

    void stop(std::function<void(FinalState)> completion) {
        completion({});
    }

private:
    rtc::scoped_refptr<webrtc::AudioDeviceModule> createAudioDeviceModule() {
        const auto create = [&](webrtc::AudioDeviceModule::AudioLayer layer) {
            return webrtc::AudioDeviceModule::Create(
                layer,
                _taskQueueFactory.get());
        };
        const auto check = [&](const rtc::scoped_refptr<webrtc::AudioDeviceModule> &result) {
            return (result && result->Init() == 0) ? result : nullptr;
        };
        if (_createAudioDeviceModule) {
            if (const auto result = check(_createAudioDeviceModule(_taskQueueFactory.get()))) {
                return result;
            }
        }
        return check(create(webrtc::AudioDeviceModule::kPlatformDefaultAudio));
    }

private:
    std::shared_ptr<Threads> _threads;
    std::vector<RtcServer> _rtcServers;
    EncryptionKey _encryptionKey;
    std::function<void(State)> _stateUpdated;
    std::function<void(int)> _signalBarsUpdated;
    std::function<void(float)> _audioLevelUpdated;
    std::function<void(bool)> _remoteBatteryLevelIsLowUpdated;
    std::function<void(AudioState, VideoState)> _remoteMediaStateUpdated;
    std::function<void(float)> _remotePrefferedAspectRatioUpdated;
    std::function<void(const std::vector<uint8_t> &)> _signalingDataEmitted;
    std::function<rtc::scoped_refptr<webrtc::AudioDeviceModule>(webrtc::TaskQueueFactory*)> _createAudioDeviceModule;

    uint32_t _outgoingAudioSsrc = 0;
    std::vector<webrtc::SdpVideoFormat> _availableVideoFormats;

    bool _handshakeCompleted = false;
    std::vector<cricket::Candidate> _pendingIceCandidates;
    bool _isDataChannelOpen = false;

    std::unique_ptr<webrtc::RtcEventLogNull> _eventLog;
    std::unique_ptr<webrtc::TaskQueueFactory> _taskQueueFactory;
    std::unique_ptr<cricket::MediaEngineInterface> _mediaEngine;
    std::unique_ptr<webrtc::Call> _call;
    webrtc::FieldTrialBasedConfig _fieldTrials;
    webrtc::LocalAudioSinkAdapter _audioSource;
    rtc::scoped_refptr<webrtc::AudioDeviceModule> _audioDeviceModule;

    std::unique_ptr<rtc::UniqueRandomIdGenerator> _uniqueRandomIdGenerator;
    webrtc::RtpTransport *_rtpTransport = nullptr;
    std::unique_ptr<cricket::ChannelManager> _channelManager;
    std::unique_ptr<webrtc::VideoBitrateAllocatorFactory> _videoBitrateAllocatorFactory;

    std::shared_ptr<ThreadLocalObject<NativeNetworkingImpl>> _networking;

    std::unique_ptr<OutgoingAudioChannel> _outgoingAudioChannel;
    std::unique_ptr<IncomingAudioChannel> _incomingAudioChannel;

    std::shared_ptr<VideoCaptureInterface> _videoCapture;
};

InstanceV2Impl::InstanceV2Impl(Descriptor &&descriptor) {
    if (descriptor.config.logPath.data.size() != 0) {
      //_logSink = std::make_unique<LogSinkImpl>(descriptor.config.logPath);
    }
    rtc::LogMessage::LogToDebug(rtc::LS_INFO);
    rtc::LogMessage::SetLogToStderr(true);
    if (_logSink) {
        rtc::LogMessage::AddLogToStream(_logSink.get(), rtc::LS_INFO);
    }

    _threads = StaticThreads::getThreads();
    _internal.reset(new ThreadLocalObject<InstanceV2ImplInternal>(_threads->getMediaThread(), [descriptor = std::move(descriptor), threads = _threads]() mutable {
        return new InstanceV2ImplInternal(std::move(descriptor), threads);
    }));
    _internal->perform(RTC_FROM_HERE, [](InstanceV2ImplInternal *internal) {
        internal->start();
    });
}

InstanceV2Impl::~InstanceV2Impl() {
}

void InstanceV2Impl::receiveSignalingData(const std::vector<uint8_t> &data) {
    _internal->perform(RTC_FROM_HERE, [data](InstanceV2ImplInternal *internal) {
        internal->receiveSignalingData(data);
    });
}

void InstanceV2Impl::setVideoCapture(std::shared_ptr<VideoCaptureInterface> videoCapture) {
    _internal->perform(RTC_FROM_HERE, [videoCapture](InstanceV2ImplInternal *internal) {
        internal->setVideoCapture(videoCapture);
    });
}

void InstanceV2Impl::setRequestedVideoAspect(float aspect) {
    _internal->perform(RTC_FROM_HERE, [aspect](InstanceV2ImplInternal *internal) {
        internal->setRequestedVideoAspect(aspect);
    });
}

void InstanceV2Impl::setNetworkType(NetworkType networkType) {
    _internal->perform(RTC_FROM_HERE, [networkType](InstanceV2ImplInternal *internal) {
        internal->setNetworkType(networkType);
    });
}

void InstanceV2Impl::setMuteMicrophone(bool muteMicrophone) {
    _internal->perform(RTC_FROM_HERE, [muteMicrophone](InstanceV2ImplInternal *internal) {
        internal->setMuteMicrophone(muteMicrophone);
    });
}

void InstanceV2Impl::setIncomingVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
    _internal->perform(RTC_FROM_HERE, [sink](InstanceV2ImplInternal *internal) {
        internal->setIncomingVideoOutput(sink);
    });
}

void InstanceV2Impl::setAudioInputDevice(std::string id) {
    _internal->perform(RTC_FROM_HERE, [id](InstanceV2ImplInternal *internal) {
        internal->setAudioInputDevice(id);
    });
}

void InstanceV2Impl::setAudioOutputDevice(std::string id) {
    _internal->perform(RTC_FROM_HERE, [id](InstanceV2ImplInternal *internal) {
        internal->setAudioOutputDevice(id);
    });
}

void InstanceV2Impl::setIsLowBatteryLevel(bool isLowBatteryLevel) {
    _internal->perform(RTC_FROM_HERE, [isLowBatteryLevel](InstanceV2ImplInternal *internal) {
        internal->setIsLowBatteryLevel(isLowBatteryLevel);
    });
}

void InstanceV2Impl::setInputVolume(float level) {
}

void InstanceV2Impl::setOutputVolume(float level) {
}

void InstanceV2Impl::setAudioOutputDuckingEnabled(bool enabled) {
}

void InstanceV2Impl::setAudioOutputGainControlEnabled(bool enabled) {
}

void InstanceV2Impl::setEchoCancellationStrength(int strength) {
}

std::vector<std::string> InstanceV2Impl::GetVersions() {
    std::vector<std::string> result;
    result.push_back("4.0.0");
    return result;
}

int InstanceV2Impl::GetConnectionMaxLayer() {
    return 92;
}

std::string InstanceV2Impl::getLastError() {
    return "";
}

std::string InstanceV2Impl::getDebugInfo() {
    return "";
}

int64_t InstanceV2Impl::getPreferredRelayId() {
    return 0;
}

TrafficStats InstanceV2Impl::getTrafficStats() {
    return {};
}

PersistentState InstanceV2Impl::getPersistentState() {
    return {};
}

void InstanceV2Impl::stop(std::function<void(FinalState)> completion) {
    _internal->perform(RTC_FROM_HERE, [completion](InstanceV2ImplInternal *internal) {
        internal->stop(completion);
    });
}

template <>
bool Register<InstanceV2Impl>() {
    return Meta::RegisterOne<InstanceV2Impl>();
}

} // namespace tgcalls
