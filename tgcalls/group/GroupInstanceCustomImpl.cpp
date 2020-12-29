#include "GroupInstanceCustomImpl.h"

#include <memory>

#include "Instance.h"
#include "VideoCaptureInterfaceImpl.h"
#include "VideoCapturerInterface.h"
#include "CodecSelectHelper.h"
#include "Message.h"
#include "platform/PlatformInterface.h"

#include "api/audio_codecs/audio_decoder_factory_template.h"
#include "api/audio_codecs/audio_encoder_factory_template.h"
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "media/engine/webrtc_media_engine.h"
#include "system_wrappers/include/field_trial.h"
#include "api/video/builtin_video_bitrate_allocator_factory.h"
#include "call/call.h"
#include "modules/rtp_rtcp/source/rtp_utility.h"
#include "api/call/audio_sink.h"
#include "modules/audio_processing/audio_buffer.h"

#include "ThreadLocalObject.h"
#include "Manager.h"
#include "NetworkManager.h"
#include "VideoCaptureInterfaceImpl.h"
#include "platform/PlatformInterface.h"
#include "LogSinkImpl.h"

#include <random>
#include <sstream>
#include <iostream>

namespace tgcalls {

namespace {

rtc::Thread *makeNetworkThread() {
    static std::unique_ptr<rtc::Thread> value = rtc::Thread::CreateWithSocketServer();
    value->SetName("WebRTC-Group-Network", nullptr);
    value->Start();
    return value.get();
}

rtc::Thread *getNetworkThread() {
    static rtc::Thread *value = makeNetworkThread();
    return value;
}

rtc::Thread *makeWorkerThread() {
    static std::unique_ptr<rtc::Thread> value = rtc::Thread::Create();
    value->SetName("WebRTC-Group-Worker", nullptr);
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

class NetworkInterfaceImpl : public cricket::MediaChannel::NetworkInterface {
public:
    NetworkInterfaceImpl(std::function<void(rtc::CopyOnWriteBuffer *, rtc::SentPacket)> sendPacket) :
    _sendPacket(sendPacket) {
        
    }
    
    bool SendPacket(rtc::CopyOnWriteBuffer *packet, const rtc::PacketOptions& options) {
        rtc::SentPacket sentPacket(options.packet_id, rtc::TimeMillis(), options.info_signaled_after_sent);
        _sendPacket(packet, sentPacket);
        return true;
    }

    bool SendRtcp(rtc::CopyOnWriteBuffer *packet, const rtc::PacketOptions& options) {
        return true;
    }

    int SetOption(cricket::MediaChannel::NetworkInterface::SocketType, rtc::Socket::Option, int) {
        return -1;
    }

private:
    std::function<void(rtc::CopyOnWriteBuffer *, rtc::SentPacket)> _sendPacket;
};

class IncomingAudioChannel {
public:
    IncomingAudioChannel(cricket::MediaEngineInterface *mediaEngine, webrtc::Call *call, std::function<void(std::vector<uint8_t> const &, rtc::SentPacket)> sendPacket, uint32_t ssrc) :
    _ssrc(ssrc) {
        cricket::AudioOptions audioOptions;
        audioOptions.echo_cancellation = true;
        audioOptions.noise_suppression = true;
        audioOptions.audio_jitter_buffer_fast_accelerate = true;
        
        _audioChannel.reset(mediaEngine->voice().CreateMediaChannel(call, cricket::MediaConfig(), audioOptions, webrtc::CryptoOptions::NoGcm()));
        
        _audioInterface.reset(new NetworkInterfaceImpl([sendPacket = std::move(sendPacket)](rtc::CopyOnWriteBuffer *buffer, rtc::SentPacket sentPacket) {
            std::vector<uint8_t> data;
            
            data.resize(buffer->size());
            memcpy(data.data(), buffer->data(), buffer->size());
            sendPacket(data, sentPacket);
        }));
        _audioChannel->SetInterface(_audioInterface.get());
        
        const uint32_t opusClockrate = 48000;
        const uint16_t opusSdpPayload = 111;
        const char *opusSdpName = "opus";
        const uint8_t opusSdpChannels = 2;
        const uint32_t opusSdpBitrate = 0;

        cricket::AudioRecvParameters audioRecvParameters;
        audioRecvParameters.codecs.emplace_back(opusSdpPayload, opusSdpName, opusClockrate, opusSdpBitrate, opusSdpChannels);
        audioRecvParameters.extensions.emplace_back(webrtc::RtpExtension::kTransportSequenceNumberUri, 1);
        audioRecvParameters.rtcp.reduced_size = true;
        audioRecvParameters.rtcp.remote_estimate = true;
        
        std::vector<std::string> streamIds;
        streamIds.push_back("1");

        _audioChannel->SetRecvParameters(audioRecvParameters);
        cricket::StreamParams audioRecvStreamParams = cricket::StreamParams::CreateLegacy(ssrc);
        audioRecvStreamParams.set_stream_ids(streamIds);
        _audioChannel->AddRecvStream(audioRecvStreamParams);
        _audioChannel->SetPlayout(true);
    }
    
    ~IncomingAudioChannel() {
        _audioChannel->SetPlayout(false);
        _audioChannel->RemoveRecvStream(_ssrc);
        _audioChannel->SetInterface(nullptr);
    }
    
private:
    uint32_t _ssrc = 0;
    std::unique_ptr<cricket::VoiceMediaChannel> _audioChannel;
    std::unique_ptr<NetworkInterfaceImpl> _audioInterface;
};

} // namespace

class GroupInstanceCustomManager : public std::enable_shared_from_this<GroupInstanceCustomManager> {
public:
    GroupInstanceCustomManager(GroupInstanceCustomDescriptor &&descriptor) :
    _sendPacket(std::move(descriptor.sendPacket)),
    _eventLog(std::make_unique<webrtc::RtcEventLogNull>()),
    _taskQueueFactory(webrtc::CreateDefaultTaskQueueFactory()){
        auto generator = std::mt19937(std::random_device()());
        auto distribution = std::uniform_int_distribution<uint32_t>();
        do {
            _outgoingAudioSsrc = distribution(generator);
        } while (!_outgoingAudioSsrc);
    }

    ~GroupInstanceCustomManager() {
        _call->SignalChannelNetworkState(webrtc::MediaType::AUDIO, webrtc::kNetworkDown);

        _outgoingAudioChannel->OnReadyToSend(false);
        _outgoingAudioChannel->SetSend(false);
        _outgoingAudioChannel->SetAudioSend(_outgoingAudioSsrc, false, nullptr, &_audioSource);
    }

    void start() {
        const auto weak = std::weak_ptr<GroupInstanceCustomManager>(shared_from_this());
        
        webrtc::field_trial::InitFieldTrialsFromString(
            "WebRTC-Audio-SendSideBwe/Enabled/"
            "WebRTC-Audio-Allocation/min:32kbps,max:32kbps/"
            "WebRTC-Audio-OpusMinPacketLossRate/Enabled-1/"
            "WebRTC-FlexFEC-03/Enabled/"
            "WebRTC-FlexFEC-03-Advertised/Enabled/"
        );

        //PlatformInterface::SharedInstance()->configurePlatformAudio();

        cricket::MediaEngineDependencies mediaDeps;
        mediaDeps.task_queue_factory = _taskQueueFactory.get();
        mediaDeps.audio_encoder_factory = webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus>();
        mediaDeps.audio_decoder_factory = webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus>();

        mediaDeps.video_encoder_factory = PlatformInterface::SharedInstance()->makeVideoEncoderFactory();
        mediaDeps.video_decoder_factory = PlatformInterface::SharedInstance()->makeVideoDecoderFactory();

        webrtc::AudioProcessingBuilder builder;
        mediaDeps.audio_processing = builder.Create();

        _audioDeviceModule = createAudioDeviceModule();
        if (!_audioDeviceModule) {
            return;
        }
        mediaDeps.adm = _audioDeviceModule;

        _mediaEngine = cricket::CreateMediaEngine(std::move(mediaDeps));
        _mediaEngine->Init();

        webrtc::Call::Config callConfig(_eventLog.get());
        callConfig.task_queue_factory = _taskQueueFactory.get();
        callConfig.trials = &_fieldTrials;
        callConfig.audio_state = _mediaEngine->voice().GetAudioState();
        _call.reset(webrtc::Call::Create(callConfig));

        cricket::AudioOptions audioOptions;
        audioOptions.echo_cancellation = true;
        audioOptions.noise_suppression = true;
        audioOptions.audio_jitter_buffer_fast_accelerate = true;

        std::vector<std::string> streamIds;
        streamIds.push_back("1");

        _outgoingAudioChannel.reset(_mediaEngine->voice().CreateMediaChannel(_call.get(), cricket::MediaConfig(), audioOptions, webrtc::CryptoOptions::NoGcm()));

        const uint32_t opusClockrate = 48000;
        const uint16_t opusSdpPayload = 111;
        const char *opusSdpName = "opus";
        const uint8_t opusSdpChannels = 2;
        const uint32_t opusSdpBitrate = 0;

        const uint8_t opusMinBitrateKbps = 6;
        const uint8_t opusMaxBitrateKbps = 32;
        const uint8_t opusStartBitrateKbps = 8;
        const uint8_t opusPTimeMs = 120;

        cricket::AudioCodec opusCodec(opusSdpPayload, opusSdpName, opusClockrate, opusSdpBitrate, opusSdpChannels);
        opusCodec.AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamTransportCc));
        opusCodec.SetParam(cricket::kCodecParamMinBitrate, opusMinBitrateKbps);
        opusCodec.SetParam(cricket::kCodecParamStartBitrate, opusStartBitrateKbps);
        opusCodec.SetParam(cricket::kCodecParamMaxBitrate, opusMaxBitrateKbps);
        opusCodec.SetParam(cricket::kCodecParamUseInbandFec, 1);
        opusCodec.SetParam(cricket::kCodecParamPTime, opusPTimeMs);

        cricket::AudioSendParameters audioSendPrameters;
        audioSendPrameters.codecs.push_back(opusCodec);
        audioSendPrameters.extensions.emplace_back(webrtc::RtpExtension::kTransportSequenceNumberUri, 1);
        audioSendPrameters.options.echo_cancellation = true;
        audioSendPrameters.options.noise_suppression = true;
        audioSendPrameters.options.auto_gain_control = true;
        audioSendPrameters.options.typing_detection = false;
        audioSendPrameters.rtcp.reduced_size = true;
        audioSendPrameters.rtcp.remote_estimate = true;
        _outgoingAudioChannel->SetSendParameters(audioSendPrameters);
        _outgoingAudioChannel->AddSendStream(cricket::StreamParams::CreateLegacy(_outgoingAudioSsrc));
        
        _outgoingAudioInterface.reset(new NetworkInterfaceImpl([weak, ssrc = _outgoingAudioSsrc](rtc::CopyOnWriteBuffer *buffer, rtc::SentPacket sentPacket) {
            std::vector<uint8_t> data;
            data.resize(8 + buffer->size());
            
            uint32_t magic = 0xcafebabeU;
            memcpy(data.data(), &magic, 4);
            memcpy(data.data() + 4, &ssrc, 4);
            
            memcpy(data.data() + 8, buffer->data(), buffer->size());
                
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, data = std::move(data), sentPacket]() mutable {
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                strong->_sendPacket(data);
                strong->_call->OnSentPacket(sentPacket);
            });
        }));
        _outgoingAudioChannel->SetInterface(_outgoingAudioInterface.get());
        
        setIsConnected(true);
    }

    void stop() {
        
    }
    
    void setIsConnected(bool isConnected) {
        if (_isConnected == isConnected) {
            return;
        }
        
        if (isConnected) {
            _call->SignalChannelNetworkState(webrtc::MediaType::AUDIO, webrtc::kNetworkUp);
            _call->SignalChannelNetworkState(webrtc::MediaType::VIDEO, webrtc::kNetworkUp);
        } else {
            _call->SignalChannelNetworkState(webrtc::MediaType::AUDIO, webrtc::kNetworkDown);
            _call->SignalChannelNetworkState(webrtc::MediaType::VIDEO, webrtc::kNetworkDown);
        }
        if (_outgoingAudioChannel) {
            _outgoingAudioChannel->OnReadyToSend(isConnected);
            _outgoingAudioChannel->SetSend(isConnected);
            _outgoingAudioChannel->SetAudioSend(_outgoingAudioSsrc, isConnected, nullptr, &_audioSource);
        }
    }
    
    void receivePacket(std::vector<uint8_t> &&data) {
        if (data.size() < 9) {
            return;
        }
        uint32_t magic = 0;
        memcpy(&magic, data.data(), 4);
        if (magic != 0xcafebabeU) {
            return;
        }
        uint32_t ssrc = 0;
        memcpy(&ssrc, data.data() + 4, 4);
        if (ssrc == _outgoingAudioSsrc) {
            return;
        }
        
        auto it = _incomingAudioChannels.find(ssrc);
        if (it == _incomingAudioChannels.end()) {
            const auto weak = std::weak_ptr<GroupInstanceCustomManager>(shared_from_this());
            
            std::unique_ptr<IncomingAudioChannel> channel(new IncomingAudioChannel(
                _mediaEngine.get(),
                _call.get(),
                [weak](std::vector<uint8_t> const &data, rtc::SentPacket sentPacket) {
                    getMediaThread()->PostTask(RTC_FROM_HERE, [weak, data = std::move(data), sentPacket]() mutable {
                        auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        //strong->_sendPacket(data);
                        strong->_call->OnSentPacket(sentPacket);
                    });
                },
                ssrc
            ));
            _incomingAudioChannels.insert(std::make_pair(ssrc, std::move(channel)));
            it = _incomingAudioChannels.find(ssrc);
        }
        
        rtc::CopyOnWriteBuffer audioPacket;
        audioPacket.AppendData(data.data() + 8, data.size() - 8);
        _call->Receiver()->DeliverPacket(webrtc::MediaType::AUDIO, audioPacket, -1);
    }
    
private:
    rtc::scoped_refptr<webrtc::AudioDeviceModule> createAudioDeviceModule() {
        const auto check = [&](webrtc::AudioDeviceModule::AudioLayer layer) {
            auto result = webrtc::AudioDeviceModule::Create(
                layer,
                _taskQueueFactory.get());
            return (result && (result->Init() == 0)) ? result : nullptr;
        };
        if (auto result = check(webrtc::AudioDeviceModule::kPlatformDefaultAudio)) {
            return result;
    #ifdef WEBRTC_LINUX
        } else if (auto result = check(webrtc::AudioDeviceModule::kLinuxAlsaAudio)) {
            return result;
    #endif // WEBRTC_LINUX
        }
        return nullptr;
    }

private:
    std::function<void(std::vector<uint8_t> const &)> _sendPacket;
    
    std::unique_ptr<webrtc::RtcEventLogNull> _eventLog;
    std::unique_ptr<webrtc::TaskQueueFactory> _taskQueueFactory;
    std::unique_ptr<cricket::MediaEngineInterface> _mediaEngine;
    std::unique_ptr<webrtc::Call> _call;
    webrtc::FieldTrialBasedConfig _fieldTrials;
    webrtc::LocalAudioSinkAdapter _audioSource;
    rtc::scoped_refptr<webrtc::AudioDeviceModule> _audioDeviceModule;
    
    std::unique_ptr<cricket::VoiceMediaChannel> _outgoingAudioChannel;
    uint32_t _outgoingAudioSsrc = 0;
    std::unique_ptr<NetworkInterfaceImpl> _outgoingAudioInterface;
    
    std::map<uint32_t, std::unique_ptr<IncomingAudioChannel>> _incomingAudioChannels;
    
    bool _isConnected = false;
};

GroupInstanceCustomImpl::GroupInstanceCustomImpl(GroupInstanceCustomDescriptor &&descriptor) {
    rtc::LogMessage::LogToDebug(rtc::LS_INFO);
    rtc::LogMessage::SetLogToStderr(true);
    /*if (_logSink) {
        rtc::LogMessage::AddLogToStream(_logSink.get(), rtc::LS_INFO);
    }*/

    _manager.reset(new ThreadLocalObject<GroupInstanceCustomManager>(getMediaThread(), [descriptor = std::move(descriptor)]() mutable {
        return new GroupInstanceCustomManager(std::move(descriptor));
    }));
    _manager->perform(RTC_FROM_HERE, [](GroupInstanceCustomManager *manager) {
        manager->start();
    });
}

GroupInstanceCustomImpl::~GroupInstanceCustomImpl() {
    /*if (_logSink) {
        rtc::LogMessage::RemoveLogToStream(_logSink.get());
    }*/
    _manager = nullptr;

    // Wait until _manager is destroyed
    getMediaThread()->Invoke<void>(RTC_FROM_HERE, [] {});
}

void GroupInstanceCustomImpl::stop() {
    _manager->perform(RTC_FROM_HERE, [](GroupInstanceCustomManager *manager) {
        manager->stop();
    });
}

void GroupInstanceCustomImpl::receivePacket(std::vector<uint8_t> &&data) {
    _manager->perform(RTC_FROM_HERE, [data = std::move(data)](GroupInstanceCustomManager *manager) mutable {
        manager->receivePacket(std::move(data));
    });
}

} // namespace tgcalls
