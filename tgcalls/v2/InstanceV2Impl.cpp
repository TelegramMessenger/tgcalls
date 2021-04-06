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
#include "api/jsep_ice_candidate.h"

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

static std::string intToString(int value) {
    std::ostringstream stringStream;
    stringStream << value;
    return stringStream.str();
}

static VideoCaptureInterfaceObject *GetVideoCaptureAssumingSameThread(VideoCaptureInterface *videoCapture) {
    return videoCapture
        ? static_cast<VideoCaptureInterfaceImpl*>(videoCapture)->object()->getSyncAssumingSameThread()
        : nullptr;
}

struct OutgoingVideoFormat {
    cricket::VideoCodec videoCodec;
    cricket::VideoCodec rtxCodec;
};

static void addDefaultFeedbackParams(cricket::VideoCodec *codec) {
    // Don't add any feedback params for RED and ULPFEC.
    if (codec->name == cricket::kRedCodecName || codec->name == cricket::kUlpfecCodecName) {
        return;
    }
    codec->AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamRemb, cricket::kParamValueEmpty));
    codec->AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamTransportCc, cricket::kParamValueEmpty));
    // Don't add any more feedback params for FLEXFEC.
    if (codec->name == cricket::kFlexfecCodecName) {
        return;
    }
    codec->AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamCcm, cricket::kRtcpFbCcmParamFir));
    codec->AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamNack, cricket::kParamValueEmpty));
    codec->AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamNack, cricket::kRtcpFbNackParamPli));
}

static absl::optional<OutgoingVideoFormat> assignPayloadTypes(std::vector<webrtc::SdpVideoFormat> const &formats) {
    if (formats.empty()) {
        return absl::nullopt;
    }

    constexpr int kFirstDynamicPayloadType = 120;
    constexpr int kLastDynamicPayloadType = 127;

    int payload_type = kFirstDynamicPayloadType;

    auto result = OutgoingVideoFormat();

    bool codecSelected = false;

    for (const auto &format : formats) {
        if (codecSelected) {
            break;
        }

        cricket::VideoCodec codec(format);
        codec.id = payload_type;
        addDefaultFeedbackParams(&codec);

        if (!absl::EqualsIgnoreCase(codec.name, cricket::kVp8CodecName)) {
            continue;
        }

        result.videoCodec = codec;
        codecSelected = true;

        // Increment payload type.
        ++payload_type;
        if (payload_type > kLastDynamicPayloadType) {
            RTC_LOG(LS_ERROR) << "Out of dynamic payload types, skipping the rest.";
            break;
        }

        // Add associated RTX codec for non-FEC codecs.
        if (!absl::EqualsIgnoreCase(codec.name, cricket::kUlpfecCodecName) &&
            !absl::EqualsIgnoreCase(codec.name, cricket::kFlexfecCodecName)) {
            result.rtxCodec = cricket::VideoCodec::CreateRtxCodec(124, codec.id);

            // Increment payload type.
            ++payload_type;
            if (payload_type > kLastDynamicPayloadType) {
                RTC_LOG(LS_ERROR) << "Out of dynamic payload types, skipping the rest.";
                break;
            }
        }
    }
    return result;
}

class OutgoingAudioChannel : public sigslot::has_slots<> {
public:
    OutgoingAudioChannel(
        webrtc::Call *call,
        cricket::ChannelManager *channelManager,
        rtc::UniqueRandomIdGenerator *uniqueRandomIdGenerator,
        webrtc::LocalAudioSinkAdapter *audioSource,
        webrtc::RtpTransport *rtpTransport,
        std::shared_ptr<Threads> threads
    ) :
    _call(call),
    _channelManager(channelManager),
    _audioSource(audioSource) {
        auto generator = std::mt19937(std::random_device()());
        auto distribution = std::uniform_int_distribution<uint32_t>();
        do {
            _mediaContent.ssrc = distribution(generator) & 0x7fffffffU;
        } while (!_mediaContent.ssrc);

        _mediaContent.rtpExtensions.emplace_back(1, webrtc::RtpExtension::kAudioLevelUri);
        _mediaContent.rtpExtensions.emplace_back(2, webrtc::RtpExtension::kAbsSendTimeUri);
        _mediaContent.rtpExtensions.emplace_back(3, webrtc::RtpExtension::kTransportSequenceNumberUri);

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

        signaling::PayloadType audioPayloadType;
        audioPayloadType.id = 111;
        audioPayloadType.name = "opus";
        audioPayloadType.channels = 2;
        audioPayloadType.clockrate = 48000;
        audioPayloadType.parameters.push_back(std::make_pair("minptime", "10"));
        audioPayloadType.parameters.push_back(std::make_pair("useinbandfec", "1"));

        _mediaContent.payloadTypes.push_back(std::move(audioPayloadType));

        _outgoingAudioChannel = _channelManager->CreateVoiceChannel(call, cricket::MediaConfig(), rtpTransport, threads->getMediaThread(), "audio0", false, NativeNetworkingImpl::getDefaulCryptoOptions(), uniqueRandomIdGenerator, audioOptions);

        const uint8_t opusMinBitrateKbps = 16;
        const uint8_t opusMaxBitrateKbps = 32;
        const uint8_t opusStartBitrateKbps = 32;
        const uint8_t opusPTimeMs = 60;

        cricket::AudioCodec opusCodec(109, "opus", 48000, 0, 2);
        opusCodec.AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamTransportCc));
        opusCodec.SetParam(cricket::kCodecParamMinBitrate, opusMinBitrateKbps);
        opusCodec.SetParam(cricket::kCodecParamStartBitrate, opusStartBitrateKbps);
        opusCodec.SetParam(cricket::kCodecParamMaxBitrate, opusMaxBitrateKbps);
        opusCodec.SetParam(cricket::kCodecParamUseInbandFec, 1);
        opusCodec.SetParam(cricket::kCodecParamPTime, opusPTimeMs);

        auto outgoingAudioDescription = std::make_unique<cricket::AudioContentDescription>();
        for (const auto &rtpExtension : _mediaContent.rtpExtensions) {
            outgoingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        outgoingAudioDescription->set_rtcp_mux(true);
        outgoingAudioDescription->set_rtcp_reduced_size(true);
        outgoingAudioDescription->set_direction(webrtc::RtpTransceiverDirection::kSendOnly);
        outgoingAudioDescription->set_codecs({ opusCodec });
        outgoingAudioDescription->set_bandwidth(1032000);
        outgoingAudioDescription->AddStream(cricket::StreamParams::CreateLegacy(_mediaContent.ssrc));

        auto incomingAudioDescription = std::make_unique<cricket::AudioContentDescription>();
        for (const auto &rtpExtension : _mediaContent.rtpExtensions) {
            incomingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
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
        _outgoingAudioChannel->media_channel()->SetAudioSend(_mediaContent.ssrc, false, nullptr, _audioSource);
        _outgoingAudioChannel->Enable(false);
        _channelManager->DestroyVoiceChannel(_outgoingAudioChannel);
        _outgoingAudioChannel = nullptr;
    }

    void setIsMuted(bool isMuted) {
        if (_isMuted != isMuted) {
            _isMuted = false;

            _outgoingAudioChannel->Enable(!_isMuted);
            _outgoingAudioChannel->media_channel()->SetAudioSend(_mediaContent.ssrc, !_isMuted, nullptr, _audioSource);
        }
    }

public:
    signaling::MediaContent const &mediaContent() {
        return _mediaContent;
    }

private:
    void OnSentPacket_w(const rtc::SentPacket& sent_packet) {
        _call->OnSentPacket(sent_packet);
    }

private:
    webrtc::Call *_call = nullptr;
    cricket::ChannelManager *_channelManager = nullptr;
    webrtc::LocalAudioSinkAdapter *_audioSource = nullptr;
    cricket::VoiceChannel *_outgoingAudioChannel = nullptr;

    signaling::MediaContent _mediaContent;

    bool _isMuted = true;
};

class IncomingV2AudioChannel : public sigslot::has_slots<> {
public:
    IncomingV2AudioChannel(
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
            cricket::AudioCodec audioCodec(payloadType.id, payloadType.name, payloadType.clockrate, 0, payloadType.channels);
            for (const auto &it : payloadType.parameters) {
                if (it.first == "useinbandfec") {
                    audioCodec.SetParam(cricket::kCodecParamUseInbandFec, stringToInt(it.second));
                } else if (it.first == "minptime") {
                    audioCodec.SetParam(cricket::kCodecParamPTime, stringToInt(it.second));
                }
            }
            audioCodecs.push_back(std::move(audioCodec));
        }

        auto outgoingAudioDescription = std::make_unique<cricket::AudioContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            outgoingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        outgoingAudioDescription->set_rtcp_mux(true);
        outgoingAudioDescription->set_rtcp_reduced_size(true);
        outgoingAudioDescription->set_direction(webrtc::RtpTransceiverDirection::kRecvOnly);
        outgoingAudioDescription->set_codecs(audioCodecs);
        outgoingAudioDescription->set_bandwidth(1032000);

        auto incomingAudioDescription = std::make_unique<cricket::AudioContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            incomingAudioDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
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

        _audioChannel->SignalSentPacket().connect(this, &IncomingV2AudioChannel::OnSentPacket_w);
        _audioChannel->UpdateRtpTransport(nullptr);

        _audioChannel->Enable(true);
    }

    ~IncomingV2AudioChannel() {
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

class OutgoingVideoChannel : public sigslot::has_slots<> {
public:
    OutgoingVideoChannel(
        std::shared_ptr<Threads> threads,
        cricket::ChannelManager *channelManager,
        webrtc::Call *call,
        webrtc::RtpTransport *rtpTransport,
        rtc::UniqueRandomIdGenerator *randomIdGenerator,
        webrtc::VideoBitrateAllocatorFactory *videoBitrateAllocatorFactory,
        //std::function<void(AudioSinkImpl::Update)> &&onAudioLevelUpdated,
        //std::function<void(uint32_t, const AudioFrame &)> onAudioFrame,
        std::vector<webrtc::SdpVideoFormat> const &availableVideoFormats
    ) :
    _call(call),
    _channelManager(channelManager) {
        auto generator = std::mt19937(std::random_device()());
        auto distribution = std::uniform_int_distribution<uint32_t>();
        do {
            _mediaContent.ssrc = distribution(generator) & 0x7fffffffU;
        } while (!_mediaContent.ssrc);

        _mediaContent.rtpExtensions.emplace_back(2, webrtc::RtpExtension::kAbsSendTimeUri);
        _mediaContent.rtpExtensions.emplace_back(3, webrtc::RtpExtension::kTransportSequenceNumberUri);
        _mediaContent.rtpExtensions.emplace_back(13, webrtc::RtpExtension::kVideoRotationUri);

        signaling::SsrcGroup fidGroup;
        fidGroup.semantics = "FID";
        fidGroup.ssrcs.push_back(_mediaContent.ssrc);
        fidGroup.ssrcs.push_back(_mediaContent.ssrc + 1);
        _mediaContent.ssrcGroups.push_back(std::move(fidGroup));

        _outgoingVideoChannel = _channelManager->CreateVideoChannel(call, cricket::MediaConfig(), rtpTransport, threads->getMediaThread(), "video0", false, NativeNetworkingImpl::getDefaulCryptoOptions(), randomIdGenerator, cricket::VideoOptions(), videoBitrateAllocatorFactory);

        std::vector<cricket::VideoCodec> videoCodecs;

        if (const auto payloadTypes = assignPayloadTypes(availableVideoFormats)) {
            std::vector<OutgoingVideoFormat> outgoingFormats;
            outgoingFormats.push_back(payloadTypes.value());

            for (const auto &format : outgoingFormats) {
                videoCodecs.push_back(format.videoCodec);
                videoCodecs.push_back(format.rtxCodec);

                signaling::PayloadType videoPayload;
                videoPayload.id = format.videoCodec.id;
                videoPayload.name = format.videoCodec.name;
                videoPayload.clockrate = format.videoCodec.clockrate;
                videoPayload.channels = 0;

                std::vector<signaling::FeedbackType> videoFeedbackTypes;

                signaling::FeedbackType fbGoogRemb;
                fbGoogRemb.type = "goog-remb";
                videoFeedbackTypes.push_back(fbGoogRemb);

                signaling::FeedbackType fbTransportCc;
                fbTransportCc.type = "transport-cc";
                videoFeedbackTypes.push_back(fbTransportCc);

                signaling::FeedbackType fbCcmFir;
                fbCcmFir.type = "ccm";
                fbCcmFir.subtype = "fir";
                videoFeedbackTypes.push_back(fbCcmFir);

                signaling::FeedbackType fbNack;
                fbNack.type = "nack";
                videoFeedbackTypes.push_back(fbNack);

                signaling::FeedbackType fbNackPli;
                fbNackPli.type = "nack";
                fbNackPli.subtype = "pli";
                videoFeedbackTypes.push_back(fbNackPli);

                videoPayload.feedbackTypes = videoFeedbackTypes;
                videoPayload.parameters = {};

                _mediaContent.payloadTypes.push_back(std::move(videoPayload));

                signaling::PayloadType rtxPayload;
                rtxPayload.id = format.rtxCodec.id;
                rtxPayload.name = format.rtxCodec.name;
                rtxPayload.clockrate = format.rtxCodec.clockrate;
                rtxPayload.parameters.push_back(std::make_pair("apt", intToString(videoPayload.id)));
                _mediaContent.payloadTypes.push_back(std::move(rtxPayload));
            }
        }

        auto outgoingVideoDescription = std::make_unique<cricket::VideoContentDescription>();
        for (const auto &rtpExtension : _mediaContent.rtpExtensions) {
            outgoingVideoDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }

        outgoingVideoDescription->set_rtcp_mux(true);
        outgoingVideoDescription->set_rtcp_reduced_size(true);
        outgoingVideoDescription->set_direction(webrtc::RtpTransceiverDirection::kSendOnly);
        outgoingVideoDescription->set_codecs(videoCodecs);
        outgoingVideoDescription->set_bandwidth(1032000);

        cricket::StreamParams videoSendStreamParams;

        for (const auto &ssrcGroup : _mediaContent.ssrcGroups) {
            for (auto ssrc : ssrcGroup.ssrcs) {
                videoSendStreamParams.ssrcs.push_back(ssrc);
            }

            cricket::SsrcGroup mappedGroup(ssrcGroup.semantics, ssrcGroup.ssrcs);
            videoSendStreamParams.ssrc_groups.push_back(std::move(mappedGroup));
        }

        videoSendStreamParams.cname = "cname";

        outgoingVideoDescription->AddStream(videoSendStreamParams);

        auto incomingVideoDescription = std::make_unique<cricket::VideoContentDescription>();
        for (const auto &rtpExtension : _mediaContent.rtpExtensions) {
            incomingVideoDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        incomingVideoDescription->set_rtcp_mux(true);
        incomingVideoDescription->set_rtcp_reduced_size(true);
        incomingVideoDescription->set_direction(webrtc::RtpTransceiverDirection::kRecvOnly);
        incomingVideoDescription->set_codecs(videoCodecs);
        incomingVideoDescription->set_bandwidth(1032000);

        _outgoingVideoChannel->SetPayloadTypeDemuxingEnabled(false);
        _outgoingVideoChannel->SetLocalContent(outgoingVideoDescription.get(), webrtc::SdpType::kOffer, nullptr);
        _outgoingVideoChannel->SetRemoteContent(incomingVideoDescription.get(), webrtc::SdpType::kAnswer, nullptr);

        webrtc::RtpParameters rtpParameters = _outgoingVideoChannel->media_channel()->GetRtpSendParameters(_mediaContent.ssrc);

        _outgoingVideoChannel->media_channel()->SetRtpSendParameters(_mediaContent.ssrc, rtpParameters);

        _outgoingVideoChannel->SignalSentPacket().connect(this, &OutgoingVideoChannel::OnSentPacket_w);
        _outgoingVideoChannel->UpdateRtpTransport(nullptr);

        _outgoingVideoChannel->Enable(false);
        _outgoingVideoChannel->media_channel()->SetVideoSend(_mediaContent.ssrc, NULL, nullptr);
    }

    ~OutgoingVideoChannel() {
        _outgoingVideoChannel->SignalSentPacket().disconnect(this);
        _outgoingVideoChannel->media_channel()->SetVideoSend(_mediaContent.ssrc, nullptr, nullptr);
        _outgoingVideoChannel->Enable(false);
        _channelManager->DestroyVideoChannel(_outgoingVideoChannel);
        _outgoingVideoChannel = nullptr;
    }

    void setVideoCapture(std::shared_ptr<VideoCaptureInterface> videoCapture) {
        _videoCapture = videoCapture;

        if (_videoCapture) {
            _outgoingVideoChannel->Enable(true);
            auto videoCaptureImpl = GetVideoCaptureAssumingSameThread(_videoCapture.get());
            _outgoingVideoChannel->media_channel()->SetVideoSend(_mediaContent.ssrc, NULL, videoCaptureImpl->source());
        } else {
            _outgoingVideoChannel->Enable(false);
            _outgoingVideoChannel->media_channel()->SetVideoSend(_mediaContent.ssrc, NULL, nullptr);
        }
    }

public:
    signaling::MediaContent const &mediaContent() {
        return _mediaContent;
    }

    std::shared_ptr<VideoCaptureInterface> videoCapture() {
        return _videoCapture;
    }

private:
    void OnSentPacket_w(const rtc::SentPacket& sent_packet) {
        _call->OnSentPacket(sent_packet);
    }

private:
    signaling::MediaContent _mediaContent;

    webrtc::Call *_call = nullptr;
    cricket::ChannelManager *_channelManager = nullptr;
    cricket::VideoChannel *_outgoingVideoChannel = nullptr;

    std::shared_ptr<VideoCaptureInterface> _videoCapture;
};

class VideoSinkImpl : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    VideoSinkImpl() {
    }

    virtual ~VideoSinkImpl() {
    }

    virtual void OnFrame(const webrtc::VideoFrame& frame) override {
        //_lastFrame = frame;
        for (int i = (int)(_sinks.size()) - 1; i >= 0; i--) {
            auto strong = _sinks[i].lock();
            if (!strong) {
                _sinks.erase(_sinks.begin() + i);
            } else {
                strong->OnFrame(frame);
            }
        }
    }

    virtual void OnDiscardedFrame() override {
        for (int i = (int)(_sinks.size()) - 1; i >= 0; i--) {
            auto strong = _sinks[i].lock();
            if (!strong) {
                _sinks.erase(_sinks.begin() + i);
            } else {
                strong->OnDiscardedFrame();
            }
        }
    }

    void addSink(std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> impl) {
        _sinks.push_back(impl);
        if (_lastFrame) {
            auto strong = impl.lock();
            if (strong) {
                strong->OnFrame(_lastFrame.value());
            }
        }
    }

private:
    std::vector<std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>> _sinks;
    absl::optional<webrtc::VideoFrame> _lastFrame;
};

class IncomingV2VideoChannel : public sigslot::has_slots<> {
public:
    IncomingV2VideoChannel(
        cricket::ChannelManager *channelManager,
        webrtc::Call *call,
        webrtc::RtpTransport *rtpTransport,
        rtc::UniqueRandomIdGenerator *randomIdGenerator,
        std::vector<webrtc::SdpVideoFormat> const &availableVideoFormats,
        signaling::MediaContent const &mediaContent,
        std::shared_ptr<Threads> threads) :
    _channelManager(channelManager),
    _call(call) {
        _videoSink.reset(new VideoSinkImpl());

        std::string streamId = "1";

        _videoBitrateAllocatorFactory = webrtc::CreateBuiltinVideoBitrateAllocatorFactory();

        _videoChannel = _channelManager->CreateVideoChannel(call, cricket::MediaConfig(), rtpTransport, threads->getMediaThread(), "video2", false, NativeNetworkingImpl::getDefaulCryptoOptions(), randomIdGenerator, cricket::VideoOptions(), _videoBitrateAllocatorFactory.get());

        std::vector<cricket::VideoCodec> videoCodecs;
        for (const auto &payloadType : mediaContent.payloadTypes) {
            cricket::VideoCodec codec(payloadType.id, payloadType.name);
            for (const auto &feedbackParam : payloadType.feedbackTypes) {
                cricket::FeedbackParam parsedFeedbackParam(feedbackParam.type, feedbackParam.subtype);
                codec.AddFeedbackParam(parsedFeedbackParam);
            }
            for (const auto &parameter : payloadType.parameters) {
                codec.SetParam(parameter.first, parameter.second);
            }
            codec.clockrate = payloadType.clockrate;
            videoCodecs.push_back(codec);
        }

        auto outgoingVideoDescription = std::make_unique<cricket::VideoContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            outgoingVideoDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        outgoingVideoDescription->set_rtcp_mux(true);
        outgoingVideoDescription->set_rtcp_reduced_size(true);
        outgoingVideoDescription->set_direction(webrtc::RtpTransceiverDirection::kRecvOnly);
        outgoingVideoDescription->set_codecs(videoCodecs);
        outgoingVideoDescription->set_bandwidth(1032000);

        cricket::StreamParams videoRecvStreamParams;

        _mainVideoSsrc = mediaContent.ssrc;

        std::vector<uint32_t> allSsrcs;
        for (const auto &group : mediaContent.ssrcGroups) {
            for (auto ssrc : group.ssrcs) {
                if (std::find(allSsrcs.begin(), allSsrcs.end(), ssrc) == allSsrcs.end()) {
                    allSsrcs.push_back(ssrc);
                }
            }

            cricket::SsrcGroup parsedGroup(group.semantics, group.ssrcs);
            videoRecvStreamParams.ssrc_groups.push_back(parsedGroup);
        }
        videoRecvStreamParams.ssrcs = allSsrcs;

        videoRecvStreamParams.cname = "cname";
        videoRecvStreamParams.set_stream_ids({ streamId });

        auto incomingVideoDescription = std::make_unique<cricket::VideoContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            incomingVideoDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        incomingVideoDescription->set_rtcp_mux(true);
        incomingVideoDescription->set_rtcp_reduced_size(true);
        incomingVideoDescription->set_direction(webrtc::RtpTransceiverDirection::kSendOnly);
        incomingVideoDescription->set_codecs(videoCodecs);
        incomingVideoDescription->set_bandwidth(1032000);

        incomingVideoDescription->AddStream(videoRecvStreamParams);

        _videoChannel->SetPayloadTypeDemuxingEnabled(false);
        _videoChannel->SetLocalContent(outgoingVideoDescription.get(), webrtc::SdpType::kOffer, nullptr);
        _videoChannel->SetRemoteContent(incomingVideoDescription.get(), webrtc::SdpType::kAnswer, nullptr);

        _videoChannel->media_channel()->SetSink(_mainVideoSsrc, _videoSink.get());

        _videoChannel->SignalSentPacket().connect(this, &IncomingV2VideoChannel::OnSentPacket_w);
        _videoChannel->UpdateRtpTransport(nullptr);

        _videoChannel->Enable(true);
    }

    ~IncomingV2VideoChannel() {
        _videoChannel->Enable(false);
        _channelManager->DestroyVideoChannel(_videoChannel);
        _videoChannel = nullptr;
    }

    void addSink(std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> impl) {
        _videoSink->addSink(impl);
    }

private:
    void OnSentPacket_w(const rtc::SentPacket& sent_packet) {
        _call->OnSentPacket(sent_packet);
    }

private:
    uint32_t _mainVideoSsrc = 0;
    std::unique_ptr<VideoSinkImpl> _videoSink;
    std::unique_ptr<webrtc::VideoBitrateAllocatorFactory> _videoBitrateAllocatorFactory;
    // Memory is managed by _channelManager
    cricket::VideoChannel *_videoChannel;
    // Memory is managed externally
    cricket::ChannelManager *_channelManager = nullptr;
    webrtc::Call *_call = nullptr;
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
    _taskQueueFactory(webrtc::CreateDefaultTaskQueueFactory()),
    _videoCapture(descriptor.videoCapture) {
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
                        strong->_call->Receiver()->DeliverPacket(webrtc::MediaType::ANY, packet, timestamp);
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

        _outgoingAudioChannel.reset(new OutgoingAudioChannel(
            _call.get(),
            _channelManager.get(),
            _uniqueRandomIdGenerator.get(),
            &_audioSource,
            _rtpTransport,
            _threads
        ));

        _outgoingVideoChannel.reset(new OutgoingVideoChannel(
            _threads,
            _channelManager.get(),
            _call.get(),
            _rtpTransport,
            _uniqueRandomIdGenerator.get(),
            _videoBitrateAllocatorFactory.get(),
            _availableVideoFormats
        ));

        _networking->perform(RTC_FROM_HERE, [](NativeNetworkingImpl *networking) {
            networking->start();
        });

        if (_videoCapture) {
            setVideoCapture(_videoCapture);
        }

        beginSignaling();

        adjustBitratePreferences(true);
    }

    void sendSignalingMessage(signaling::Message const &message) {
        auto data = message.serialize();

        RTC_LOG(LS_INFO) << "sendSignalingMessage: " << std::string(data.begin(), data.end());
        _signalingDataEmitted(data);
    }

    void beginSignaling() {
        if (_encryptionKey.isOutgoing) {
            sendInitialSetup();
        }
    }

    void sendInitialSetup() {
        const auto weak = std::weak_ptr<InstanceV2ImplInternal>(shared_from_this());

        _networking->perform(RTC_FROM_HERE, [weak, threads = _threads, isOutgoing = _encryptionKey.isOutgoing](NativeNetworkingImpl *networking) {
            auto localFingerprint = networking->getLocalFingerprint();
            std::string hash = localFingerprint->algorithm;
            std::string fingerprint = localFingerprint->GetRfc4572Fingerprint();
            std::string setup = isOutgoing ? "active" : "passive";

            auto localIceParams = networking->getLocalIceParameters();
            std::string ufrag = localIceParams.ufrag;
            std::string pwd = localIceParams.pwd;

            threads->getMediaThread()->PostTask(RTC_FROM_HERE, [weak, ufrag, pwd, hash, fingerprint, setup, localIceParams]() {
                const auto strong = weak.lock();
                if (!strong) {
                    return;
                }

                signaling::InitialSetupMessage data;

                if (strong->_outgoingAudioChannel) {
                    data.audio = strong->_outgoingAudioChannel->mediaContent();
                }
                if (strong->_outgoingVideoChannel) {
                    data.video = strong->_outgoingVideoChannel->mediaContent();
                }

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
        RTC_LOG(LS_INFO) << "receiveSignalingData: " << std::string(data.begin(), data.end());

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
            std::string sslSetup;
            if (initialSetup->fingerprints.size() != 0) {
                fingerprint = rtc::SSLFingerprint::CreateUniqueFromRfc4572(initialSetup->fingerprints[0].hash, initialSetup->fingerprints[0].fingerprint);
                sslSetup = initialSetup->fingerprints[0].setup;
            }

            _networking->perform(RTC_FROM_HERE, [threads = _threads, remoteIceParameters = std::move(remoteIceParameters), fingerprint = std::move(fingerprint), sslSetup = std::move(sslSetup)](NativeNetworkingImpl *networking) {
                networking->setRemoteParams(remoteIceParameters, fingerprint.get(), sslSetup);
            });

            if (const auto audio = initialSetup->audio) {
                _incomingAudioChannel.reset(new IncomingV2AudioChannel(
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

            if (const auto video = initialSetup->video) {
                _incomingVideoChannel.reset(new IncomingV2VideoChannel(
                    _channelManager.get(),
                    _call.get(),
                    _rtpTransport,
                    _uniqueRandomIdGenerator.get(),
                    _availableVideoFormats,
                    video.value(),
                    _threads
                ));
            }

            if (!_encryptionKey.isOutgoing) {
                sendInitialSetup();
            }

            _handshakeCompleted = true;
            commitPendingIceCandidates();
        } else if (const auto candidatesList = absl::get_if<signaling::CandidatesMessage>(messageData)) {
            for (const auto &candidate : candidatesList->iceCandidates) {
                webrtc::JsepIceCandidate parseCandidate{ std::string(), 0 };
                if (!parseCandidate.Initialize(candidate.sdpString, nullptr)) {
                    RTC_LOG(LS_ERROR) << "Could not parse candidate: " << candidate.sdpString;
                    continue;
                }
                _pendingIceCandidates.push_back(parseCandidate.candidate());
            }

            if (_handshakeCompleted) {
                commitPendingIceCandidates();
            }
        } else if (const auto mediaState = absl::get_if<signaling::MediaStateMessage>(messageData)) {
            AudioState mappedAudioState;
            if (mediaState->isMuted) {
                mappedAudioState = AudioState::Muted;
            } else {
                mappedAudioState = AudioState::Active;
            }

            VideoState mappedVideoState;
            switch (mediaState->videoState) {
                case signaling::MediaStateMessage::VideoState::Inactive: {
                    mappedVideoState = VideoState::Inactive;
                    break;
                }
                case signaling::MediaStateMessage::VideoState::Suspended: {
                    mappedVideoState = VideoState::Paused;
                    break;
                }
                case signaling::MediaStateMessage::VideoState::Active: {
                    mappedVideoState = VideoState::Active;
                    break;
                }
                default: {
                    RTC_FATAL() << "Unknown videoState";
                    break;
                }
            }

            if (_remoteMediaStateUpdated) {
                _remoteMediaStateUpdated(mappedAudioState, mappedVideoState);
            }

            if (_remoteBatteryLevelIsLowUpdated) {
                _remoteBatteryLevelIsLowUpdated(mediaState->isBatteryLow);
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
                sendMediaState();
            }
        }
    }

    void sendDataChannelMessage(signaling::Message const &message) {
        if (!_isDataChannelOpen) {
            RTC_LOG(LS_ERROR) << "sendDataChannelMessage called, but data channel is not open";
            return;
        }
        auto data = message.serialize();
        std::string stringData(data.begin(), data.end());
        RTC_LOG(LS_INFO) << "sendDataChannelMessage: " << stringData;
        _networking->perform(RTC_FROM_HERE, [stringData = std::move(stringData)](NativeNetworkingImpl *networking) {
            networking->sendDataChannelMessage(stringData);
        });
    }

    void onDataChannelMessage(std::string const &message) {
        RTC_LOG(LS_INFO) << "dataChannelMessage received: " << message;
        std::vector<uint8_t> data(message.begin(), message.end());
        receiveSignalingData(data);
    }

    void sendMediaState() {
        if (!_isDataChannelOpen) {
            return;
        }
        signaling::Message message;
        signaling::MediaStateMessage data;
        data.isMuted = _isMicrophoneMuted;
        data.isBatteryLow = _isBatteryLow;
        if (_outgoingVideoChannel) {
            if (_outgoingVideoChannel->videoCapture()) {
                data.videoState = signaling::MediaStateMessage::VideoState::Active;
            } else{
                data.videoState = signaling::MediaStateMessage::VideoState::Inactive;
            }
        } else {
            data.videoState = signaling::MediaStateMessage::VideoState::Inactive;
        }
        message.data = std::move(data);
        sendDataChannelMessage(message);
    }

    void sendCandidate(const cricket::Candidate &candidate) {
        cricket::Candidate patchedCandidate = candidate;
        patchedCandidate.set_component(1);

        signaling::CandidatesMessage data;

        signaling::IceCandidate serializedCandidate;

        webrtc::JsepIceCandidate iceCandidate{ std::string(), 0 };
        iceCandidate.SetCandidate(patchedCandidate);
        std::string serialized;
        const auto success = iceCandidate.ToString(&serialized);
        assert(success);

        serializedCandidate.sdpString = serialized;

        data.iceCandidates.push_back(std::move(serializedCandidate));

        signaling::Message message;
        message.data = std::move(data);
        sendSignalingMessage(message);
    }

    void setVideoCapture(std::shared_ptr<VideoCaptureInterface> videoCapture) {
        _videoCapture = videoCapture;
        
        if (_outgoingVideoChannel) {
            _outgoingVideoChannel->setVideoCapture(videoCapture);

            sendMediaState();

            adjustBitratePreferences(true);
        }
    }

    void setRequestedVideoAspect(float aspect) {

    }

    void setNetworkType(NetworkType networkType) {

    }

    void setMuteMicrophone(bool muteMicrophone) {
        if (_isMicrophoneMuted != muteMicrophone) {
            _isMicrophoneMuted = muteMicrophone;

            if (_outgoingAudioChannel) {
                _outgoingAudioChannel->setIsMuted(muteMicrophone);
            }

            sendMediaState();
        }
    }

    void setIncomingVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
        if (_incomingVideoChannel) {
            _incomingVideoChannel->addSink(sink);
        }
    }

    void setAudioInputDevice(std::string id) {

    }

    void setAudioOutputDevice(std::string id) {

    }

    void setIsLowBatteryLevel(bool isLowBatteryLevel) {
        if (_isBatteryLow != isLowBatteryLevel) {
            _isBatteryLow = isLowBatteryLevel;
            sendMediaState();
        }
    }

    void stop(std::function<void(FinalState)> completion) {
        completion({});
    }

    void adjustBitratePreferences(bool resetStartBitrate) {
        webrtc::BitrateConstraints preferences;
        if (_videoCapture) {
            preferences.min_bitrate_bps = 64000;
            if (resetStartBitrate) {
                preferences.start_bitrate_bps = (100 + 800 + 32 + 100) * 1000;
            }
            preferences.max_bitrate_bps = (100 + 200 + 800 + 32 + 100) * 1000;
        } else {
            preferences.min_bitrate_bps = 32000;
            if (resetStartBitrate) {
                preferences.start_bitrate_bps = 32000;
            }
            preferences.max_bitrate_bps = 32000;
        }

        _call->GetTransportControllerSend()->SetSdpBitrateParameters(preferences);
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
    bool _isMicrophoneMuted = false;

    std::vector<webrtc::SdpVideoFormat> _availableVideoFormats;
    std::unique_ptr<OutgoingVideoChannel> _outgoingVideoChannel;

    bool _isBatteryLow = false;

    std::unique_ptr<IncomingV2AudioChannel> _incomingAudioChannel;
    std::unique_ptr<IncomingV2VideoChannel> _incomingVideoChannel;

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
