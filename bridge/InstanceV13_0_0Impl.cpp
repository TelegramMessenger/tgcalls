// InstanceV13_0_0Impl — implementacja tgcalls::Instance dla protokołu 13.0.0 (tgcalls v2, signaling V3).
//
// Strategia (ADR-0009): media pipeline + ContentNegotiationContext przejęte z naszego
// InstanceV4_0_0Impl (sprawdzony port InstanceV2Impl na legacy webrtc, hardening M1/M2),
// warstwa sygnalizacji przeniesiona 1:1 z upstream InstanceV2Impl w wariancie V3:
//   signaling::Message JSON -> gzip -> EncryptedConnection::encryptRawPacket
//     -> SignalingSctpConnection (SCTP nad TDLib sendCallSignalingData; fallback
//        ExternalSignalingConnection przy custom param network_signaling_nosctp).
// Flow: caller wysyła InitialSetup(setup=actpass) + NegotiateChannels(offer) od razu po
// starcie; callee po odebraniu InitialSetup odsyła własny (setup=passive); wymiana
// NegotiateChannels przechodzi przez ContentNegotiationContext (exchangeId rozstrzyga
// kolizje), kanały audio materializują się z coordinatedState.

#include "InstanceV13_0_0Impl.h"

#include "tgcall_logging.h"

#include <hilog/log.h>

#include "tgcalls/ChannelManager.h"
#include "tgcalls/EncryptedConnection.h"
#include "tgcalls/FieldTrialsConfig.h"
#include "tgcalls/Instance.h"
#include "tgcalls/LogSinkImpl.h"
#include "tgcalls/StaticThreads.h"
#include "tgcalls/ThreadLocalObject.h"
#include "tgcalls/platform/PlatformInterface.h"
#include "tgcalls/utils/gzip.h"
#include "tgcalls/v2/ContentNegotiation.h"
#include "tgcalls/v2/ExternalSignalingConnection.h"
#include "tgcalls/v2/NativeNetworkingImpl.h"
#include "tgcalls/v2/Signaling.h"
#include "tgcalls/v2/SignalingConnection.h"
#include "tgcalls/v2/SignalingSctpConnection.h"

#include "third-party/json11.hpp"

#include "api/audio_codecs/audio_decoder_factory_template.h"
#include "api/audio_codecs/audio_encoder_factory_template.h"
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"
#include "api/candidate.h"
#include "api/jsep_ice_candidate.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/units/time_delta.h"
#include "call/call.h"
#include "call/call_config.h"
#include "call/packet_receiver.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/enable_media.h"
#include "api/environment/environment_factory.h"
#include "api/rtc_event_log/rtc_event_log.h"
#include "media/base/media_channel.h"
#include "media/base/media_config.h"
#include "media/engine/webrtc_media_engine.h"
#include "pc/media_factory.h"
#include "modules/audio_device/include/audio_device.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "p2p/base/p2p_constants.h"
#include "pc/channel.h"
#include "pc/rtp_sender.h"
#include "pc/rtp_transport.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/crypto_random.h"
#include "rtc_base/logging.h"
#include "rtc_base/ssl_fingerprint.h"
#include "rtc_base/unique_id_generator.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

// CreateDefaultAudioDeviceModule() is exported by libohos_webrtc.so and
// actually returns scoped_refptr<OhosAudioDeviceModule> (a subclass of
// AudioDeviceModule). We declare it here with the base class return type to
// avoid pulling the sdk/ohos OHOS ADM header (and its napi dependency) into
// this TU; scoped_refptr<T> layouts are identical for derived/base types.
namespace webrtc {
webrtc::scoped_refptr<webrtc::AudioDeviceModule> CreateDefaultAudioDeviceModule();
}  // namespace webrtc

namespace tgcalls {

namespace {

// Incoming-audio gain multiplier applied via VoiceMediaReceiveChannelInterface::
// SetOutputVolume in IncomingAudioChannel_13_0_0. Compensates for the
// AUDIOSTREAM_USAGE_VOICE_COMMUNICATION earpiece-style routing in
// third_party/ohos_webrtc OHAudioPlayer that leaves the watch speaker quiet.
// Same value as InstanceV4_0_0Impl — validated by 4.3 smoke tests.
constexpr double kIncomingVolumeBoost = 2.0;

// V3 signaling messages must stay well under the SCTP max message size configured in
// SignalingSctpConnection::Start (262144); upstream uses 2 MiB as the gunzip safety cap.
constexpr size_t kMaxGunzipSize = 2 * 1024 * 1024;

bool ShouldLogMediaPacketCount(size_t count) {
    return count <= 5 || count == 10 || count == 25 || (count % 50) == 0;
}

bool GetCustomParameterBool(std::map<std::string, json11::Json> const &parameters, std::string const &name) {
    const auto value = parameters.find(name);
    return value != parameters.end() && value->second.is_bool() && value->second.bool_value();
}

const char *V3MessageTypeName(const tgcalls::signaling::Message &message) {
    if (absl::get_if<tgcalls::signaling::InitialSetupMessage>(&message.data) != nullptr) {
        return "InitialSetup";
    }
    if (absl::get_if<tgcalls::signaling::NegotiateChannelsMessage>(&message.data) != nullptr) {
        return "NegotiateChannels";
    }
    if (absl::get_if<tgcalls::signaling::CandidatesMessage>(&message.data) != nullptr) {
        return "Candidates";
    }
    if (absl::get_if<tgcalls::signaling::MediaStateMessage>(&message.data) != nullptr) {
        return "MediaState";
    }
    return "Unknown";
}

}  // namespace

// Same wrapper as OutgoingAudioChannel_4_0_0 — port from tgcalls/v2/InstanceV2Impl.cpp
// OutgoingAudioChannel adapted for legacy webrtc snapshot, trimmed to Opus only.
// Duplicated per ADR-0009 (R8): each instance variant stays self-contained like upstream;
// shared-helper extraction is deferred until both variants are stable.
class OutgoingAudioChannel_13_0_0 : public sigslot::has_slots<> {
public:
    OutgoingAudioChannel_13_0_0(
        webrtc::Call *call,
        ChannelManager *channelManager,
        webrtc::LocalAudioSinkAdapter *audioSource,
        webrtc::RtpTransport *rtpTransport,
        const signaling::MediaContent &mediaContent,
        std::shared_ptr<Threads> threads)
        : _threads(std::move(threads)),
          _ssrc(mediaContent.ssrc),
          _call(call),
          _channelManager(channelManager),
          _audioSource(audioSource) {
        webrtc::AudioOptions audioOptions;
        // ADR-0006: webrtc::AudioProcessing AEC/NS/AGC are intentionally off — Huawei OHAudio HAL
        // performs native audio enhancement before PCM reaches OH_AudioCapturer; layering webrtc
        // APM on top double-processes the signal and AGC flattens it to silence. Validated by
        // 4.3 smoke A/B 2026-05-13. Do NOT flip these to true without revising ADR-0006.
        audioOptions.echo_cancellation = false;
        audioOptions.noise_suppression = false;
        audioOptions.auto_gain_control = false;
        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "OutgoingAudioChannel_13_0_0 audio options applied (ADR-0006) aec=%{public}d ns=%{public}d agc=%{public}d ssrc=%{public}u",
                     audioOptions.echo_cancellation.value_or(true) ? 1 : 0,
                     audioOptions.noise_suppression.value_or(true) ? 1 : 0,
                     audioOptions.auto_gain_control.value_or(true) ? 1 : 0,
                     _ssrc);

        const std::string contentId = std::to_string(_ssrc);

        _outgoingAudioChannel = _channelManager->CreateVoiceChannel(
            call,
            webrtc::MediaConfig(),
            contentId,
            /*srtp_required=*/false,
            tgcalls::NativeNetworkingImpl::getDefaulCryptoOptions(),
            audioOptions);
        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "OutgoingAudioChannel_13_0_0 CreateVoiceChannel ssrc=%{public}u channel=%{public}d codecs=%{public}zu",
                     _ssrc, _outgoingAudioChannel ? 1 : 0, mediaContent.payloadTypes.size());
        if (!_outgoingAudioChannel) {
            return;
        }
        _threads->getNetworkThread()->BlockingCall([this, rtpTransport]() {
            _outgoingAudioChannel->SetRtpTransport(rtpTransport);
        });

        std::vector<webrtc::Codec> codecs;
        for (const auto &payloadType : mediaContent.payloadTypes) {
            if (payloadType.name != "opus") {
                continue;
            }
            webrtc::Codec codec = webrtc::CreateAudioCodec(
                payloadType.id,
                payloadType.name,
                payloadType.clockrate,
                payloadType.channels);
            codec.SetParam(webrtc::kCodecParamUseInbandFec, 1);
            codec.SetParam(webrtc::kCodecParamPTime, 60);
            for (const auto &feedback : payloadType.feedbackTypes) {
                codec.AddFeedbackParam(webrtc::FeedbackParam(feedback.type, feedback.subtype));
            }
            codecs.push_back(std::move(codec));
            break;
        }

        auto outgoingDescription = std::make_unique<webrtc::AudioContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            outgoingDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        outgoingDescription->set_rtcp_mux(true);
        outgoingDescription->set_rtcp_reduced_size(true);
        outgoingDescription->set_direction(webrtc::RtpTransceiverDirection::kSendOnly);
        outgoingDescription->set_codecs(codecs);
        outgoingDescription->set_bandwidth(-1);
        outgoingDescription->AddStream(webrtc::StreamParams::CreateLegacy(_ssrc));

        auto incomingDescription = std::make_unique<webrtc::AudioContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            incomingDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        incomingDescription->set_rtcp_mux(true);
        incomingDescription->set_rtcp_reduced_size(true);
        incomingDescription->set_direction(webrtc::RtpTransceiverDirection::kRecvOnly);
        incomingDescription->set_codecs(codecs);
        incomingDescription->set_bandwidth(-1);

        _threads->getWorkerThread()->BlockingCall([this, &outgoingDescription, &incomingDescription]() {
            std::string errorDesc;
            const webrtc::RTCError localError = _outgoingAudioChannel->SetLocalContent(outgoingDescription.get(), webrtc::SdpType::kOffer);
            const webrtc::RTCError remoteError = _outgoingAudioChannel->SetRemoteContent(incomingDescription.get(), webrtc::SdpType::kAnswer);
            const bool localOk = localError.ok();
            const bool remoteOk = remoteError.ok();
            OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "OutgoingAudioChannel_13_0_0 SetContent local=%{public}d remote=%{public}d err=%{public}s",
                         localOk ? 1 : 0, remoteOk ? 1 : 0, errorDesc.c_str());
        });

        setIsMuted(false);
    }

    ~OutgoingAudioChannel_13_0_0() {
        if (!_outgoingAudioChannel) {
            OH_LOG_Print(LOG_APP, LOG_WARN, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "OutgoingAudioChannel_13_0_0 destructor skipped — channel never created");
            return;
        }
        _outgoingAudioChannel->Enable(false);
        webrtc::ChannelInterface *channelToDestroy = _outgoingAudioChannel;
        _threads->getNetworkThread()->BlockingCall([channelToDestroy]() {
            if (channelToDestroy) {
                channelToDestroy->SetRtpTransport(nullptr);
            }
        });
        _channelManager->DestroyChannel(channelToDestroy);
        _outgoingAudioChannel = nullptr;
    }

    void setIsMuted(bool isMuted) {
        if (_isMuted == isMuted) {
            return;
        }
        _isMuted = isMuted;
        if (!_outgoingAudioChannel) {
            return;
        }
        _outgoingAudioChannel->Enable(!_isMuted);
        _threads->getWorkerThread()->BlockingCall([this]() {
            _outgoingAudioChannel->voice_media_send_channel()->SetAudioSend(_ssrc, !_isMuted, nullptr, _audioSource);
        });
        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "OutgoingAudioChannel_13_0_0 setIsMuted muted=%{public}d ssrc=%{public}u",
                     _isMuted ? 1 : 0, _ssrc);
    }

    bool isReady() const {
        return _outgoingAudioChannel != nullptr;
    }

    uint32_t ssrc() const {
        return _ssrc;
    }

    // Pulls send-side stats off the inner webrtc::VoiceChannel (audio level bars + send
    // diagnostics). Must be invoked on the worker thread.
    bool getSendStats(webrtc::VoiceMediaSendInfo *info) const {
        if (!_outgoingAudioChannel) {
            return false;
        }
        return _outgoingAudioChannel->voice_media_send_channel()->GetStats(info);
    }

private:
    std::shared_ptr<Threads> _threads;
    uint32_t _ssrc = 0;
    webrtc::Call *_call = nullptr;
    ChannelManager *_channelManager = nullptr;
    webrtc::LocalAudioSinkAdapter *_audioSource = nullptr;
    webrtc::ChannelInterface *_outgoingAudioChannel = nullptr;
    bool _isMuted = true;
};

// Same wrapper as IncomingAudioChannel_4_0_0 — receive-only VoiceChannel fed from
// RtpTransport demux, decoded by Opus, played through Huawei OHOS ADM.
class IncomingAudioChannel_13_0_0 : public sigslot::has_slots<> {
public:
    IncomingAudioChannel_13_0_0(
        ChannelManager *channelManager,
        webrtc::Call *call,
        webrtc::RtpTransport *rtpTransport,
        const signaling::MediaContent &mediaContent,
        std::shared_ptr<Threads> threads)
        : _threads(std::move(threads)),
          _ssrc(mediaContent.ssrc),
          _channelManager(channelManager),
          _call(call) {
        webrtc::AudioOptions audioOptions;
        audioOptions.audio_jitter_buffer_fast_accelerate = true;
        audioOptions.audio_jitter_buffer_min_delay_ms = 50;

        const std::string streamId = std::to_string(_ssrc);

        _audioChannel = _channelManager->CreateVoiceChannel(
            call,
            webrtc::MediaConfig(),
            streamId,
            /*srtp_required=*/false,
            tgcalls::NativeNetworkingImpl::getDefaulCryptoOptions(),
            audioOptions);
        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "IncomingAudioChannel_13_0_0 CreateVoiceChannel ssrc=%{public}u channel=%{public}d codecs=%{public}zu",
                     _ssrc, _audioChannel ? 1 : 0, mediaContent.payloadTypes.size());
        if (!_audioChannel) {
            return;
        }
        _threads->getNetworkThread()->BlockingCall([this, rtpTransport]() {
            _audioChannel->SetRtpTransport(rtpTransport);
        });

        std::vector<webrtc::Codec> codecs;
        for (const auto &payloadType : mediaContent.payloadTypes) {
            webrtc::Codec codec = webrtc::CreateAudioCodec(
                payloadType.id,
                payloadType.name,
                payloadType.clockrate,
                payloadType.channels);
            for (const auto &parameter : payloadType.parameters) {
                codec.SetParam(parameter.first, parameter.second);
            }
            for (const auto &feedback : payloadType.feedbackTypes) {
                codec.AddFeedbackParam(webrtc::FeedbackParam(feedback.type, feedback.subtype));
            }
            codecs.push_back(std::move(codec));
        }

        auto outgoingDescription = std::make_unique<webrtc::AudioContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            outgoingDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        outgoingDescription->set_rtcp_mux(true);
        outgoingDescription->set_rtcp_reduced_size(true);
        outgoingDescription->set_direction(webrtc::RtpTransceiverDirection::kRecvOnly);
        outgoingDescription->set_codecs(codecs);
        outgoingDescription->set_bandwidth(-1);

        auto incomingDescription = std::make_unique<webrtc::AudioContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            incomingDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        incomingDescription->set_rtcp_mux(true);
        incomingDescription->set_rtcp_reduced_size(true);
        incomingDescription->set_direction(webrtc::RtpTransceiverDirection::kSendOnly);
        incomingDescription->set_codecs(codecs);
        incomingDescription->set_bandwidth(-1);
        webrtc::StreamParams streamParams = webrtc::StreamParams::CreateLegacy(_ssrc);
        streamParams.set_stream_ids({streamId});
        incomingDescription->AddStream(streamParams);

        _threads->getWorkerThread()->BlockingCall([this, &outgoingDescription, &incomingDescription]() {
            std::string errorDesc;
            const webrtc::RTCError localError = _audioChannel->SetLocalContent(outgoingDescription.get(), webrtc::SdpType::kOffer);
            const webrtc::RTCError remoteError = _audioChannel->SetRemoteContent(incomingDescription.get(), webrtc::SdpType::kAnswer);
            const bool localOk = localError.ok();
            const bool remoteOk = remoteError.ok();
            OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "IncomingAudioChannel_13_0_0 SetContent local=%{public}d remote=%{public}d err=%{public}s",
                         localOk ? 1 : 0, remoteOk ? 1 : 0, errorDesc.c_str());
            // Compensate for AUDIOSTREAM_USAGE_VOICE_COMMUNICATION earpiece-style routing in
            // third_party/ohos_webrtc OHAudioPlayer — same pattern and rationale as
            // IncomingAudioChannel_4_0_0 (must run on the worker thread).
            _audioChannel->voice_media_receive_channel()->SetOutputVolume(_ssrc, kIncomingVolumeBoost);
            OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "IncomingAudioChannel_13_0_0 SetOutputVolume ssrc=%{public}u gain=%{public}.1f",
                         _ssrc, kIncomingVolumeBoost);
        });

        _audioChannel->Enable(true);
    }

    ~IncomingAudioChannel_13_0_0() {
        if (!_audioChannel) {
            OH_LOG_Print(LOG_APP, LOG_WARN, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "IncomingAudioChannel_13_0_0 destructor skipped — channel never created");
            return;
        }
        _audioChannel->Enable(false);
        webrtc::ChannelInterface *channelToDestroy = _audioChannel;
        _threads->getNetworkThread()->BlockingCall([channelToDestroy]() {
            if (channelToDestroy) {
                channelToDestroy->SetRtpTransport(nullptr);
            }
        });
        _channelManager->DestroyChannel(channelToDestroy);
        _audioChannel = nullptr;
    }

    uint32_t ssrc() const {
        return _ssrc;
    }

    bool isReady() const {
        return _audioChannel != nullptr;
    }

    // Pulls receive-side stats off the inner webrtc::VoiceChannel — mirror of
    // OutgoingAudioChannel_13_0_0::getSendStats. Must be invoked on the worker thread.
    bool getReceiveStats(webrtc::VoiceMediaReceiveInfo *info) const {
        if (!_audioChannel) {
            return false;
        }
        return _audioChannel->voice_media_receive_channel()->GetStats(info, /*reset_legacy=*/false);
    }

private:
    std::shared_ptr<Threads> _threads;
    uint32_t _ssrc = 0;
    webrtc::ChannelInterface *_audioChannel = nullptr;
    ChannelManager *_channelManager = nullptr;
    webrtc::Call *_call = nullptr;
};

class InstanceV13_0_0ImplInternal {
public:
    InstanceV13_0_0ImplInternal(Descriptor &&descriptor, std::shared_ptr<Threads> threads);
    ~InstanceV13_0_0ImplInternal();

    void start();
    void receiveSignalingData(const std::vector<uint8_t> &data);
    void setVideoCapture(std::shared_ptr<VideoCaptureInterface> videoCapture);
    void setRequestedVideoAspect(float aspect);
    void setNetworkType(NetworkType networkType);
    void setMuteMicrophone(bool muteMicrophone);
    void setIncomingVideoOutput(std::weak_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> sink);
    void setAudioInputDevice(std::string id);
    void setAudioOutputDevice(std::string id);
    void setIsLowBatteryLevel(bool isLowBatteryLevel);
    void stop(std::function<void(FinalState)> completion);

private:
    // --- V3 signaling layer (ported from InstanceV2Impl, SignalingProtocolVersion::V3) ---
    void createSignalingConnection();
    void beginSignaling();
    void sendSignalingMessage(const signaling::Message &message);
    void onSignalingData(const std::vector<uint8_t> &data);
    void processSignalingMessage(const webrtc::CopyOnWriteBuffer &data);
    void processSignalingData(const std::vector<uint8_t> &data);
    void sendInitialSetup();
    void sendOfferIfNeeded();
    void sendMediaState();
    void sendDataChannelMessage(const signaling::Message &message);
    void onDataChannelStateUpdated(bool isDataChannelOpen);
    void onDataChannelMessage(const std::string &message);
    void emitLocalCandidate(const webrtc::Candidate &candidate);
    void commitPendingIceCandidates();
    void onNetworkStateUpdated(const tgcalls::InstanceNetworking::State &state);
    void onTransportMessageReceived(const webrtc::CopyOnWriteBuffer &packet, bool isUnresolved);
    void onRtcpPacketReceived(const webrtc::CopyOnWriteBuffer &packet, int64_t packetTimeUs);

    // --- media pipeline (same as InstanceV4_0_0Impl) ---
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> createAudioDeviceModule();
    void setUpMediaPipeline();
    void createNegotiatedChannels();
    void teardownMediaPipelineOnWorkerThread();
    void shutdownInstance();

    std::string _version;
    tgcalls::Config _config;
    std::vector<tgcalls::RtcServer> _rtcServers;
    absl::optional<tgcalls::Proxy> _proxy;
    std::map<std::string, json11::Json> _customParameters;
    std::shared_ptr<tgcalls::Threads> _threads;
    std::shared_ptr<tgcalls::ThreadLocalObject<tgcalls::InstanceNetworking>> _networking;
    std::shared_ptr<const std::array<uint8_t, tgcalls::EncryptionKey::kSize>> _encryptionKey;
    bool _encryptionKeyIsOutgoing = false;
    std::function<void(tgcalls::State)> _stateUpdated;
    std::function<void(const std::vector<uint8_t> &)> _signalingDataEmitted;
    std::function<void(float)> _audioLevelUpdated;
    tgcalls::NetworkType _networkType = tgcalls::NetworkType::Unknown;
    bool _muteMicrophone = false;
    bool _isLowBatteryLevel = false;
    std::weak_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> _incomingVideoOutput;
    std::string _audioInputDeviceId;
    std::string _audioOutputDeviceId;
    std::shared_ptr<tgcalls::VideoCaptureInterface> _videoCapture;
    float _requestedVideoAspect = 0.0f;

    // V3 signaling state. All mutated on the media thread (ThreadLocalObject home thread);
    // _signalingAlive gates PostTask continuations that could otherwise outlive `this` —
    // flipped to false on the media thread in shutdownInstance, so reads in queued media
    // tasks are sequenced with the flip (same-thread ordering, no race).
    std::shared_ptr<SignalingConnection> _signalingConnection;
    std::unique_ptr<EncryptedConnection> _signalingEncryptedConnection;
    std::shared_ptr<std::atomic<bool>> _signalingAlive = std::make_shared<std::atomic<bool>>(true);
    bool _handshakeCompleted = false;
    bool _initialSetupSent = false;
    bool _connectedOnce = false;
    bool _isDataChannelOpen = false;
    std::vector<webrtc::Candidate> _pendingIceCandidates;
    size_t _signalingRxPacketCount = 0;
    size_t _signalingRxByteCount = 0;
    size_t _signalingTxPacketCount = 0;

    std::mutex _mediaDiagnosticsMutex;
    size_t _incomingRtpPacketCount = 0;
    size_t _incomingRtpByteCount = 0;
    size_t _unresolvedRtpPacketCount = 0;
    size_t _incomingRtcpPacketCount = 0;
    size_t _incomingRtcpByteCount = 0;

    // Media pipeline (same members as InstanceV4_0_0Impl).
    webrtc::Environment _env;
    std::unique_ptr<webrtc::RtcEventLogNull> _eventLog;
    std::unique_ptr<webrtc::TaskQueueFactory> _taskQueueFactory;
    std::unique_ptr<webrtc::UniqueRandomIdGenerator> _uniqueRandomIdGenerator;
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> _audioDeviceModule;
    webrtc::LocalAudioSinkAdapter _audioSource;
    std::unique_ptr<ChannelManager> _channelManager;
    std::unique_ptr<webrtc::Call> _call;
    webrtc::RtpTransport *_rtpTransport = nullptr;
    std::unique_ptr<ContentNegotiationContext> _contentNegotiationContext;
    absl::optional<std::string> _outgoingAudioChannelId;
    std::unique_ptr<OutgoingAudioChannel_13_0_0> _outgoingAudioChannel;
    std::unique_ptr<IncomingAudioChannel_13_0_0> _incomingAudioChannel;
    bool _mediaPipelineReady = false;
    // Gates delayed worker-thread tasks that read _call/_outgoingAudioChannel after teardown —
    // same pattern and same-thread sequencing rationale as InstanceV4_0_0Impl::_diagAlive.
    std::shared_ptr<std::atomic<bool>> _diagAlive = std::make_shared<std::atomic<bool>>(true);
    // Periodic audio-level task (in-call UI bars); strong ref lives here, the self-re-posting
    // lambda captures a weak_ptr to avoid a cycle, reset on the worker thread during teardown.
    std::shared_ptr<std::function<void()>> _audioLevelTask;
    std::function<webrtc::scoped_refptr<webrtc::AudioDeviceModule>(webrtc::TaskQueueFactory *)>
        _createAudioDeviceModuleHook;
};

InstanceV13_0_0ImplInternal::InstanceV13_0_0ImplInternal(
    Descriptor &&descriptor,
    std::shared_ptr<Threads> threads)
    : _version(std::move(descriptor.version)),
      _config(descriptor.config),
      _rtcServers(std::move(descriptor.rtcServers)),
      _threads(std::move(threads)),
      _encryptionKey(descriptor.encryptionKey.value),
      _encryptionKeyIsOutgoing(descriptor.encryptionKey.isOutgoing),
      _stateUpdated(std::move(descriptor.stateUpdated)),
      _signalingDataEmitted(std::move(descriptor.signalingDataEmitted)),
      _audioLevelUpdated(std::move(descriptor.audioLevelUpdated)),
      _eventLog(std::make_unique<webrtc::RtcEventLogNull>()),
      _taskQueueFactory(webrtc::CreateDefaultTaskQueueFactory()),
      _env(webrtc::EnvironmentFactory().Create()),
      _createAudioDeviceModuleHook(std::move(descriptor.createAudioDeviceModule)) {
    if (descriptor.proxy) {
        _proxy = *(descriptor.proxy.get());
    }
    if (!_config.customParameters.empty()) {
        std::string parsingError;
        const auto json = json11::Json::parse(_config.customParameters, parsingError);
        if (json.is_object()) {
            _customParameters = json.object_items();
        } else {
            OH_LOG_Print(LOG_APP, LOG_WARN, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "InstanceV13_0_0Impl customParameters parse failed err=%{public}s",
                         parsingError.c_str());
        }
    }
    RTC_LOG(LS_INFO) << "InstanceV13_0_0Impl created version=" << _version
                     << " rtcServers=" << _rtcServers.size()
                     << " keyOutgoing=" << (_encryptionKeyIsOutgoing ? 1 : 0)
                     << " customParams=" << _customParameters.size();
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV13_0_0Impl created version=%{public}s rtcServers=%{public}zu keyOutgoing=%{public}d customParams=%{public}zu",
                 _version.c_str(), _rtcServers.size(), _encryptionKeyIsOutgoing ? 1 : 0,
                 _customParameters.size());
}

InstanceV13_0_0ImplInternal::~InstanceV13_0_0ImplInternal() {
    if (_threads) {
        // Idempotent fallback when stop() was not invoked — same defensive ordering as stop().
        shutdownInstance();
    } else {
        _incomingAudioChannel.reset();
        _outgoingAudioChannel.reset();
        _channelManager.reset();
    }
    _contentNegotiationContext.reset();
}

void InstanceV13_0_0ImplInternal::start() {
    if (!_encryptionKey) {
        RTC_LOG(LS_ERROR) << "InstanceV13_0_0Impl::start aborted, missing encryption key";
        return;
    }

    // allow_p2p handling — hybrid (#65 E4, supersedes the plain pass-through from review
    // P2 of PR #68):
    //  * When the server hands us REAL TURN servers (callServerTypeWebrtc with
    //    credentials), honor allow_p2p verbatim — upstream V3 parity, no IP leak on
    //    relay-only calls.
    //  * When it does not (observed: our api_id receives the TURN hosts bare, while web
    //    peers get credentials for the same machines — #65#issuecomment-2822), a
    //    relay-only call would leave us with reflector-relay candidates only, which a
    //    browser peer cannot use → zero ICE pairs. Fall back to gathering host/srflx
    //    candidates despite allow_p2p=0 — the proven 4.0.0-era path: the browser stays
    //    relay-only ('iceTransportPolicy: relay') but grants TURN permissions to our
    //    advertised addresses, so the our-host <-> their-relay pair connects. This
    //    knowingly trades IP privacy for a working call and is logged loudly.
    bool hasUsableTurnServer = false;
    for (const auto &server : _rtcServers) {
        if (server.isTurn && !server.login.empty() && server.login != "reflector") {
            hasUsableTurnServer = true;
            break;
        }
    }
    const bool enableP2P = _config.enableP2P || !hasUsableTurnServer;
    if (enableP2P && !_config.enableP2P) {
        RTC_LOG(LS_WARNING) << "InstanceV13_0_0Impl privacy-degraded P2P fallback engaged — "
                               "no TURN credentials from server on a relay-only call";
        OH_LOG_Print(LOG_APP, LOG_WARN, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV13_0_0Impl privacy-degraded P2P fallback engaged (allowP2p=0, usable TURN servers=0)");
    }
    // Upstream V3 parity: InstanceV2Impl hardcodes enableTCP=false regardless of the
    // TDLib config — TCP host candidates are useless to a 13.0.0 peer and only added
    // noise to the exchange (smoke 2026-06-11 sent 3 tcp/local candidates for nothing).
    const bool enableTcp = false;
    RTC_LOG(LS_INFO) << "InstanceV13_0_0Impl networking config p2p=" << (enableP2P ? 1 : 0)
                     << " configP2p=" << (_config.enableP2P ? 1 : 0)
                     << " usableTurn=" << (hasUsableTurnServer ? 1 : 0)
                     << " tcp=" << (enableTcp ? 1 : 0)
                     << " configTcp=" << (_config.allowTCP ? 1 : 0)
                     << " stunMarking=" << (_config.enableStunMarking ? 1 : 0);
    const tgcalls::EncryptionKey networkingKey(_encryptionKey, _encryptionKeyIsOutgoing);
    _networking = std::make_shared<tgcalls::ThreadLocalObject<tgcalls::InstanceNetworking>>(
        _threads->getNetworkThread(),
        [this,
         threads = _threads,
         networkingKey,
         rtcServers = _rtcServers,
         proxy = _proxy,
         enableStunMarking = _config.enableStunMarking,
         enableTcp,
         enableP2P,
         customParameters = _customParameters]() {
            return std::static_pointer_cast<tgcalls::InstanceNetworking>(
                std::make_shared<tgcalls::NativeNetworkingImpl>(tgcalls::InstanceNetworking::Configuration{
                    .encryptionKey = networkingKey,
                    .isOutgoing = networkingKey.isOutgoing,
                    .enableStunMarking = enableStunMarking,
                    .enableTCP = enableTcp,
                    .enableP2P = enableP2P,
                    .rtcServers = rtcServers,
                    .proxy = proxy,
                    .stateUpdated = [this](const tgcalls::InstanceNetworking::State &state) {
                        onNetworkStateUpdated(state);
                    },
                    .candidateGathered = [this](const webrtc::Candidate &candidate) {
                        emitLocalCandidate(candidate);
                    },
                    .transportMessageReceived = [this](webrtc::CopyOnWriteBuffer const &packet, bool isUnresolved) {
                        onTransportMessageReceived(packet, isUnresolved);
                    },
                    .rtcpPacketReceived = [this](webrtc::CopyOnWriteBuffer const &packet, int64_t packetTimeUs) {
                        onRtcpPacketReceived(packet, packetTimeUs);
                    },
                    .dataChannelStateUpdated = [this, threads, alive = _signalingAlive](bool isOpen) {
                        threads->getMediaThread()->PostTask([this, alive, isOpen]() {
                            if (!alive->load()) {
                                return;
                            }
                            onDataChannelStateUpdated(isOpen);
                        });
                    },
                    .dataChannelMessageReceived = [this, threads, alive = _signalingAlive](std::string const &message) {
                        threads->getMediaThread()->PostTask([this, alive, message]() {
                            if (!alive->load()) {
                                return;
                            }
                            onDataChannelMessage(message);
                        });
                    },
                    .threads = threads,
                    .customParameters = customParameters}));
        });

    if (_stateUpdated) {
        _stateUpdated(tgcalls::State::WaitInit);
    }

    setUpMediaPipeline();

    createSignalingConnection();
    beginSignaling();
}

void InstanceV13_0_0ImplInternal::createSignalingConnection() {
    // V3 default transport: real SCTP packets tunnelled over the external TDLib signaling
    // channel (SignalingSctpConnection starts emitting INIT immediately). The server can opt
    // a call out via the network_signaling_nosctp custom parameter — then encrypted blobs go
    // raw, exactly like upstream InstanceV2Impl and tweb both do.
    const bool useSctp = !GetCustomParameterBool(_customParameters, "network_signaling_nosctp");
    auto alive = _signalingAlive;
    const auto onIncoming = [this, alive, threads = _threads](const std::vector<uint8_t> &data) {
        threads->getMediaThread()->PostTask([this, alive, data]() {
            if (!alive->load()) {
                return;
            }
            onSignalingData(data);
        });
    };
    const auto emitData = [signalingDataEmitted = _signalingDataEmitted](const std::vector<uint8_t> &data) {
        if (signalingDataEmitted) {
            signalingDataEmitted(data);
        }
    };

    if (useSctp) {
        _signalingConnection = std::make_shared<SignalingSctpConnection>(_threads, onIncoming, emitData, _encryptionKeyIsOutgoing);
    } else {
        _signalingConnection = std::make_shared<ExternalSignalingConnection>(onIncoming, emitData);
    }
    _signalingConnection->start();
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV13_0_0Impl signaling connection started sctp=%{public}d",
                 useSctp ? 1 : 0);
}

void InstanceV13_0_0ImplInternal::beginSignaling() {
    const tgcalls::EncryptionKey encryptionKey(_encryptionKey, _encryptionKeyIsOutgoing);
    // The service-send callback drives EncryptedConnection's V2 Message-protocol ACK/resend
    // machinery; the V3 raw-packet path (encryptRawPacket/decryptRawPacket) never schedules
    // it. Keep a logging stub so an unexpected invocation is visible in hilog.
    _signalingEncryptedConnection = std::make_unique<EncryptedConnection>(
        EncryptedConnection::Type::Signaling,
        encryptionKey,
        [](int delayMs, int cause) {
            RTC_LOG(LS_WARNING) << "InstanceV13_0_0Impl unexpected EncryptedConnection service request delayMs="
                                << delayMs << " cause=" << cause;
        });

    if (_encryptionKeyIsOutgoing) {
        sendInitialSetup();
        sendOfferIfNeeded();
    }
}

void InstanceV13_0_0ImplInternal::sendSignalingMessage(const signaling::Message &message) {
    if (!_signalingConnection || !_signalingEncryptedConnection) {
        RTC_LOG(LS_ERROR) << "InstanceV13_0_0Impl sendSignalingMessage dropped — signaling not ready";
        return;
    }

    const std::vector<uint8_t> serialized = message.serialize();

    // V3 wire: JSON -> gzip (always on send; receive side auto-detects) -> raw-packet
    // encryption -> SCTP/raw connection.
    std::vector<uint8_t> packetData;
    if (const auto compressed = gzipData(serialized)) {
        packetData = std::move(compressed.value());
    } else {
        RTC_LOG(LS_ERROR) << "InstanceV13_0_0Impl could not gzip signaling message";
        return;
    }

    const auto encrypted = _signalingEncryptedConnection->encryptRawPacket(
        webrtc::CopyOnWriteBuffer(packetData.data(), packetData.size()));
    if (!encrypted) {
        RTC_LOG(LS_ERROR) << "InstanceV13_0_0Impl could not encrypt signaling message";
        return;
    }

    _signalingTxPacketCount += 1;
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV13_0_0Impl signaling send type=%{public}s json=%{public}zu gz=%{public}zu enc=%{public}zu tx=%{public}zu",
                 V3MessageTypeName(message), serialized.size(), packetData.size(),
                 encrypted->size(), _signalingTxPacketCount);

    _signalingConnection->send(std::vector<uint8_t>(encrypted->data(), encrypted->data() + encrypted->size()));
}

void InstanceV13_0_0ImplInternal::receiveSignalingData(const std::vector<uint8_t> &data) {
    _signalingRxPacketCount += 1;
    _signalingRxByteCount += data.size();
    if (_signalingConnection) {
        _signalingConnection->receiveExternal(data);
    } else {
        RTC_LOG(LS_ERROR) << "InstanceV13_0_0Impl receiveSignalingData dropped — no connection";
    }
}

void InstanceV13_0_0ImplInternal::onSignalingData(const std::vector<uint8_t> &data) {
    if (!_signalingEncryptedConnection) {
        RTC_LOG(LS_ERROR) << "InstanceV13_0_0Impl onSignalingData dropped — encryption not ready";
        return;
    }
    if (const auto decrypted = _signalingEncryptedConnection->decryptRawPacket(
            webrtc::CopyOnWriteBuffer(data.data(), data.size()))) {
        processSignalingMessage(decrypted.value());
    } else {
        RTC_LOG(LS_WARNING) << "InstanceV13_0_0Impl signaling decrypt failed bytes=" << data.size();
        OH_LOG_Print(LOG_APP, LOG_WARN, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV13_0_0Impl signaling decrypt failed bytes=%{public}zu",
                     data.size());
    }
}

void InstanceV13_0_0ImplInternal::processSignalingMessage(const webrtc::CopyOnWriteBuffer &data) {
    std::vector<uint8_t> decryptedData(data.data(), data.data() + data.size());
    if (isGzip(decryptedData)) {
        if (const auto decompressed = gunzipData(decryptedData, kMaxGunzipSize)) {
            processSignalingData(decompressed.value());
        } else {
            RTC_LOG(LS_ERROR) << "InstanceV13_0_0Impl could not gunzip signaling message";
        }
    } else {
        processSignalingData(decryptedData);
    }
}

void InstanceV13_0_0ImplInternal::processSignalingData(const std::vector<uint8_t> &data) {
    const auto message = signaling::Message::parse(data);
    if (!message) {
        RTC_LOG(LS_WARNING) << "InstanceV13_0_0Impl signaling parse failed bytes=" << data.size();
        return;
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV13_0_0Impl signaling received type=%{public}s bytes=%{public}zu rx=%{public}zu",
                 V3MessageTypeName(message.value()), data.size(), _signalingRxPacketCount);

    const auto messageData = &message->data;
    if (const auto initialSetup = absl::get_if<signaling::InitialSetupMessage>(messageData)) {
        PeerIceParameters remoteIceParameters;
        remoteIceParameters.ufrag = initialSetup->ufrag;
        remoteIceParameters.pwd = initialSetup->pwd;
        remoteIceParameters.supportsRenomination = initialSetup->supportsRenomination;

        std::string sslSetup;
        std::string fingerprintHash;
        std::string fingerprintValue;
        if (!initialSetup->fingerprints.empty()) {
            fingerprintHash = initialSetup->fingerprints[0].hash;
            fingerprintValue = initialSetup->fingerprints[0].fingerprint;
            sslSetup = initialSetup->fingerprints[0].setup;
        }

        _networking->perform([remoteIceParameters, fingerprintHash, fingerprintValue, sslSetup](tgcalls::InstanceNetworking *networking) {
            std::unique_ptr<webrtc::SSLFingerprint> fingerprint;
            if (!fingerprintHash.empty()) {
                fingerprint = webrtc::SSLFingerprint::CreateUniqueFromRfc4572(fingerprintHash, fingerprintValue);
            }
            networking->setRemoteParams(remoteIceParameters, fingerprint.get(), sslSetup);
        });

        _handshakeCompleted = true;

        // Callee answers the caller's InitialSetup with its own — upstream V3 flow.
        if (!_encryptionKeyIsOutgoing) {
            sendInitialSetup();
        }

        commitPendingIceCandidates();
    } else if (const auto negotiateChannels = absl::get_if<signaling::NegotiateChannelsMessage>(messageData)) {
        auto negotiationContents = std::make_unique<ContentNegotiationContext::NegotiationContents>();
        negotiationContents->exchangeId = negotiateChannels->exchangeId;
        negotiationContents->contents = negotiateChannels->contents;

        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV13_0_0Impl NegotiateChannels received exchangeId=%{public}u contents=%{public}zu",
                     negotiateChannels->exchangeId, negotiateChannels->contents.size());

        if (!_contentNegotiationContext) {
            RTC_LOG(LS_ERROR) << "InstanceV13_0_0Impl NegotiateChannels dropped — no negotiation context";
            return;
        }

        if (const auto response = _contentNegotiationContext->setRemoteNegotiationContent(std::move(negotiationContents))) {
            signaling::NegotiateChannelsMessage data;
            data.exchangeId = response->exchangeId;
            data.contents = response->contents;

            signaling::Message message;
            message.data = std::move(data);
            sendSignalingMessage(message);
        }

        sendOfferIfNeeded();

        createNegotiatedChannels();
    } else if (const auto candidatesList = absl::get_if<signaling::CandidatesMessage>(messageData)) {
        for (const auto &candidate : candidatesList->iceCandidates) {
            std::unique_ptr<webrtc::IceCandidate> parseCandidate = webrtc::IceCandidate::Create(
                std::string(), 0, candidate.sdpString, nullptr);
            if (!parseCandidate) {
                RTC_LOG(LS_WARNING) << "InstanceV13_0_0Impl could not parse candidate: " << candidate.sdpString;
                continue;
            }
            // #65 E4 diagnostics: relay-only smoke needs the remote candidate types/addresses
            // visible in hilog (RTC_LOG has no sink on-device). Remove with task 4.4 C1.
            OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "InstanceV13_0_0Impl remote candidate protocol=%{public}s type=%{public}s address=%{public}s",
                         parseCandidate->candidate().protocol().c_str(),
                         std::string(webrtc::IceCandidateTypeToString(parseCandidate->candidate().type())).c_str(),
                         parseCandidate->candidate().address().ToString().c_str());
            _pendingIceCandidates.push_back(parseCandidate->candidate());
        }

        if (_handshakeCompleted) {
            commitPendingIceCandidates();
        }
    } else if (const auto mediaState = absl::get_if<signaling::MediaStateMessage>(messageData)) {
        // Remote mute / battery flags — audio-only watch UI has no consumer yet; log for smoke.
        RTC_LOG(LS_INFO) << "InstanceV13_0_0Impl remote media state muted=" << (mediaState->isMuted ? 1 : 0)
                         << " lowBattery=" << (mediaState->isBatteryLow ? 1 : 0);
    }
}

void InstanceV13_0_0ImplInternal::sendInitialSetup() {
    if (!_networking) {
        return;
    }
    {
        // Dedupe: the callee path can receive the caller's InitialSetup more than once
        // (SCTP retransmits surface as repeated decrypted payloads upstream too).
        if (_initialSetupSent) {
            return;
        }
        _initialSetupSent = true;
    }

    auto alive = _signalingAlive;
    _networking->perform([this, alive, threads = _threads, isOutgoing = _encryptionKeyIsOutgoing](tgcalls::InstanceNetworking *networking) {
        auto localFingerprint = networking->getLocalFingerprint();
        std::string hash;
        std::string fingerprint;
        if (localFingerprint) {
            hash = localFingerprint->algorithm;
            fingerprint = localFingerprint->GetRfc4572Fingerprint();
        }

        // Upstream InstanceV2Impl::sendInitialSetup role mapping: caller offers actpass,
        // callee answers passive (NativeNetworkingImpl::setRemoteParams then resolves the
        // caller to SSL_CLIENT against our passive and vice versa).
        std::string setup;
        if (isOutgoing) {
            setup = "actpass";
        } else {
            setup = "passive";
        }

        const auto localIceParams = networking->getLocalIceParameters();
        const std::string ufrag = localIceParams.ufrag;
        const std::string pwd = localIceParams.pwd;
        const bool supportsRenomination = localIceParams.supportsRenomination;

        threads->getMediaThread()->PostTask([this, alive, ufrag, pwd, supportsRenomination, hash, fingerprint, setup]() {
            if (!alive->load()) {
                return;
            }

            signaling::InitialSetupMessage data;
            data.ufrag = ufrag;
            data.pwd = pwd;
            data.supportsRenomination = supportsRenomination;

            signaling::DtlsFingerprint dtlsFingerprint;
            dtlsFingerprint.hash = hash;
            dtlsFingerprint.fingerprint = fingerprint;
            dtlsFingerprint.setup = setup;
            data.fingerprints.push_back(std::move(dtlsFingerprint));

            OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "InstanceV13_0_0Impl sending InitialSetup setup=%{public}s renomination=%{public}d",
                         setup.c_str(), supportsRenomination ? 1 : 0);

            signaling::Message message;
            message.data = std::move(data);
            sendSignalingMessage(message);
        });
    });
}

void InstanceV13_0_0ImplInternal::sendOfferIfNeeded() {
    if (!_contentNegotiationContext) {
        return;
    }
    if (const auto offer = _contentNegotiationContext->getPendingOffer()) {
        signaling::NegotiateChannelsMessage data;
        data.exchangeId = offer->exchangeId;
        data.contents = offer->contents;

        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV13_0_0Impl sending NegotiateChannels offer exchangeId=%{public}u contents=%{public}zu",
                     data.exchangeId, data.contents.size());

        signaling::Message message;
        message.data = std::move(data);
        sendSignalingMessage(message);
    }
}

void InstanceV13_0_0ImplInternal::onDataChannelStateUpdated(bool isDataChannelOpen) {
    if (_isDataChannelOpen == isDataChannelOpen) {
        return;
    }
    _isDataChannelOpen = isDataChannelOpen;
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV13_0_0Impl data channel state open=%{public}d",
                 _isDataChannelOpen ? 1 : 0);
    if (_isDataChannelOpen) {
        // Upstream V3 pushes the initial local media state as soon as the channel opens.
        sendMediaState();
    }
}

void InstanceV13_0_0ImplInternal::sendDataChannelMessage(const signaling::Message &message) {
    if (!_isDataChannelOpen) {
        RTC_LOG(LS_ERROR) << "InstanceV13_0_0Impl sendDataChannelMessage called, but data channel is not open";
        return;
    }
    auto data = message.serialize();
    std::string stringData(data.begin(), data.end());
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV13_0_0Impl data channel send type=%{public}s bytes=%{public}zu",
                 V3MessageTypeName(message), stringData.size());
    _networking->perform([stringData = std::move(stringData)](tgcalls::InstanceNetworking *networking) {
        networking->sendDataChannelMessage(stringData);
    });
}

void InstanceV13_0_0ImplInternal::onDataChannelMessage(const std::string &message) {
    // Data channel rides inside the DTLS media transport, so the payload arrives as
    // plaintext signaling JSON — feed it to the shared dispatcher (upstream parity).
    RTC_LOG(LS_INFO) << "InstanceV13_0_0Impl data channel message received bytes=" << message.size();
    std::vector<uint8_t> data(message.begin(), message.end());
    processSignalingData(data);
}

void InstanceV13_0_0ImplInternal::sendMediaState() {
    // Upstream V3 contract: MediaState travels over the media data channel only, gated on
    // its open state (the initial push happens in onDataChannelStateUpdated). Watch is
    // audio-only: video fields stay inactive.
    if (!_isDataChannelOpen) {
        return;
    }
    signaling::MediaStateMessage mediaState;
    mediaState.isMuted = _muteMicrophone;
    mediaState.videoState = signaling::MediaStateMessage::VideoState::Inactive;
    mediaState.videoRotation = signaling::MediaStateMessage::VideoRotation::Rotation0;
    mediaState.screencastState = signaling::MediaStateMessage::VideoState::Inactive;
    mediaState.isBatteryLow = _isLowBatteryLevel;

    signaling::Message message;
    message.data = std::move(mediaState);
    sendDataChannelMessage(message);
}

void InstanceV13_0_0ImplInternal::emitLocalCandidate(const webrtc::Candidate &candidate) {
    webrtc::Candidate patchedCandidate = candidate;
    patchedCandidate.set_component(1);

    webrtc::JsepIceCandidate iceCandidate("", 0, patchedCandidate);
    std::string serializedCandidate;
    if (!iceCandidate.ToString(&serializedCandidate)) {
        RTC_LOG(LS_WARNING) << "InstanceV13_0_0Impl local candidate serialize failed";
        return;
    }

    signaling::CandidatesMessage candidatesMessage;
    signaling::IceCandidate localCandidate;
    localCandidate.sdpString = serializedCandidate;
    candidatesMessage.iceCandidates.push_back(std::move(localCandidate));

    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV13_0_0Impl sending candidate protocol=%{public}s type=%{public}s",
                 patchedCandidate.protocol().c_str(),
                 std::string(webrtc::IceCandidateTypeToString(patchedCandidate.type())).c_str());

    signaling::Message message;
    message.data = std::move(candidatesMessage);
    sendSignalingMessage(message);
}

void InstanceV13_0_0ImplInternal::commitPendingIceCandidates() {
    if (_pendingIceCandidates.empty()) {
        return;
    }
    auto candidates = _pendingIceCandidates;
    _pendingIceCandidates.clear();
    _networking->perform([candidates = std::move(candidates)](tgcalls::InstanceNetworking *networking) {
        networking->addCandidates(candidates);
    });
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV13_0_0Impl committed pending candidates");
}

void InstanceV13_0_0ImplInternal::onNetworkStateUpdated(const tgcalls::InstanceNetworking::State &state) {
    RTC_LOG(LS_INFO) << "InstanceV13_0_0Impl networking state version=" << _version
                     << " ready=" << (state.isReadyToSendData ? 1 : 0)
                     << " failed=" << (state.isFailed ? 1 : 0)
                     << " route=" << (state.route.has_value() ? 1 : 0)
                     << " connection=" << (state.connection.has_value() ? 1 : 0);
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV13_0_0Impl networking state ready=%{public}d failed=%{public}d",
                 state.isReadyToSendData ? 1 : 0, state.isFailed ? 1 : 0);

    if (!_stateUpdated) {
        return;
    }
    if (state.isFailed) {
        _stateUpdated(tgcalls::State::Failed);
        return;
    }
    if (state.isReadyToSendData) {
        _connectedOnce = true;
        _stateUpdated(tgcalls::State::Established);
        // Initial MediaState push is driven by onDataChannelStateUpdated (upstream V3
        // contract) — nothing to send here.
    } else if (_connectedOnce) {
        _stateUpdated(tgcalls::State::Reconnecting);
    }
}

void InstanceV13_0_0ImplInternal::onTransportMessageReceived(
    const webrtc::CopyOnWriteBuffer &packet,
    bool isUnresolved) {
    size_t packetCount = 0;
    size_t byteCount = 0;
    size_t unresolvedCount = 0;
    {
        std::lock_guard<std::mutex> lock(_mediaDiagnosticsMutex);
        _incomingRtpPacketCount += 1;
        _incomingRtpByteCount += packet.size();
        if (isUnresolved) {
            _unresolvedRtpPacketCount += 1;
        }
        packetCount = _incomingRtpPacketCount;
        byteCount = _incomingRtpByteCount;
        unresolvedCount = _unresolvedRtpPacketCount;
    }

    if (ShouldLogMediaPacketCount(packetCount)) {
        RTC_LOG(LS_INFO) << "InstanceV13_0_0Impl media ingress RTP version=" << _version
                         << " packets=" << packetCount
                         << " bytes=" << byteCount
                         << " lastBytes=" << packet.size()
                         << " unresolved=" << unresolvedCount;
    }
}

void InstanceV13_0_0ImplInternal::onRtcpPacketReceived(
    const webrtc::CopyOnWriteBuffer &packet,
    int64_t packetTimeUs) {
    size_t packetCount = 0;
    {
        std::lock_guard<std::mutex> lock(_mediaDiagnosticsMutex);
        _incomingRtcpPacketCount += 1;
        _incomingRtcpByteCount += packet.size();
        packetCount = _incomingRtcpPacketCount;
    }
    if (ShouldLogMediaPacketCount(packetCount)) {
        RTC_LOG(LS_INFO) << "InstanceV13_0_0Impl media ingress RTCP version=" << _version
                         << " packets=" << packetCount
                         << " packetTimeUs=" << packetTimeUs;
    }

    if (_call) {
        webrtc::CopyOnWriteBuffer copy = packet;
        _threads->getWorkerThread()->PostTask([this, copy = std::move(copy)]() {
            if (_call) {
                _call->Receiver()->DeliverRtcpPacket(copy);
            }
        });
    }
}

webrtc::scoped_refptr<webrtc::AudioDeviceModule>
InstanceV13_0_0ImplInternal::createAudioDeviceModule() {
    const auto check = [](const webrtc::scoped_refptr<webrtc::AudioDeviceModule> &result) {
        return (result && result->Init() == 0) ? result : nullptr;
    };
    if (_createAudioDeviceModuleHook) {
        if (auto result = check(_createAudioDeviceModuleHook(_taskQueueFactory.get()))) {
            return result;
        }
    }
    // Per ADR-0002: kPlatformDefaultAudio auto-selects Huawei OHOS ADM when WEBRTC_OHOS is set.
    // In webrtc_latest the static AudioDeviceModule::Create was removed; the
    // equivalent factory is CreateDefaultAudioDeviceModule(), exported by
    // libohos_webrtc.so and declared above.
    return check(webrtc::CreateDefaultAudioDeviceModule());
}

void InstanceV13_0_0ImplInternal::setUpMediaPipeline() {
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV13_0_0Impl setUpMediaPipeline begin version=%{public}s ready=%{public}d networking=%{public}d",
                 _version.c_str(), _mediaPipelineReady ? 1 : 0, _networking ? 1 : 0);
    if (_mediaPipelineReady || !_networking) {
        return;
    }

    _threads->getWorkerThread()->BlockingCall([this]() {
        _audioDeviceModule = createAudioDeviceModule();
    });
    if (!_audioDeviceModule) {
        RTC_LOG(LS_ERROR) << "InstanceV13_0_0Impl ADM create failed — media pipeline aborted";
        OH_LOG_Print(LOG_APP, LOG_ERROR, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV13_0_0Impl ADM create failed — media pipeline aborted");
        return;
    }

    webrtc::PeerConnectionFactoryDependencies pcfDeps;
    pcfDeps.network_thread = _threads->getNetworkThread();
    pcfDeps.signaling_thread = _threads->getMediaThread();
    pcfDeps.worker_thread = _threads->getWorkerThread();
    pcfDeps.adm = _audioDeviceModule;
    pcfDeps.audio_encoder_factory = webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus>();
    pcfDeps.audio_decoder_factory = webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus>();
    // Video factories from the OHOS PlatformInterface (builtin VP8/VP9/H264
    // software codecs exported by libohos_webrtc.so).
    pcfDeps.video_encoder_factory = tgcalls::PlatformInterface::SharedInstance()->makeVideoEncoderFactory(false, false);
    pcfDeps.video_decoder_factory = tgcalls::PlatformInterface::SharedInstance()->makeVideoDecoderFactory();
    // The AudioProcessing instance is built internally by BuiltinAudioProcessingBuilder when
    // MediaFactory::CreateMediaEngine runs.
    pcfDeps.audio_processing_builder = std::make_unique<webrtc::BuiltinAudioProcessingBuilder>();

    webrtc::EnableMedia(pcfDeps);
    auto mediaEngine = pcfDeps.media_factory->CreateMediaEngine(_env, pcfDeps);
    if (!mediaEngine) {
        RTC_LOG(LS_ERROR) << "InstanceV13_0_0Impl MediaEngine create failed";
        return;
    }

    _channelManager = ChannelManager::Create(
        std::move(mediaEngine),
        _threads->getWorkerThread(),
        _threads->getNetworkThread());

    _threads->getNetworkThread()->BlockingCall([this]() {
        auto *networking = _networking->getSyncAssumingSameThread();
        _rtpTransport = networking->getRtpTransport();
    });
    if (!_rtpTransport) {
        RTC_LOG(LS_ERROR) << "InstanceV13_0_0Impl rtpTransport unavailable — media pipeline aborted";
        return;
    }

    _threads->getWorkerThread()->BlockingCall([this]() {
        webrtc::CallConfig callConfig(_env, _threads->getWorkerThread(), _threads->getNetworkThread());
        callConfig.audio_state = _channelManager->media_engine()->voice().GetAudioState();
        _call = webrtc::Call::Create(std::move(callConfig));
    });
    if (!_call) {
        RTC_LOG(LS_ERROR) << "InstanceV13_0_0Impl webrtc::Call::Create failed";
        return;
    }

    _uniqueRandomIdGenerator = std::make_unique<webrtc::UniqueRandomIdGenerator>();
    _contentNegotiationContext = std::make_unique<ContentNegotiationContext>(
        fieldTrialsBasedConfig,
        _encryptionKeyIsOutgoing,
        _channelManager->media_engine(),
        _uniqueRandomIdGenerator.get());
    _contentNegotiationContext->copyCodecsFromChannelManager(
        _channelManager->media_engine(),
        /*randomize=*/false);

    _outgoingAudioChannelId = _contentNegotiationContext->addOutgoingChannel(
        signaling::MediaContent::Type::Audio);

    // Upstream V3 parity: the pending offer is NOT self-answered (that was a 4.0.0 bridge
    // workaround, ADR-0008). It rides out in sendOfferIfNeeded as NegotiateChannels and the
    // peer's answer finalises both directions through setRemoteNegotiationContent.

    // Upstream starts networking right after the media pipeline is wired (InstanceV2Impl
    // start path) — ICE gathering can begin before signaling finishes the handshake.
    _networking->perform([](tgcalls::InstanceNetworking *networking) {
        networking->start();
    });

    _mediaPipelineReady = true;
    RTC_LOG(LS_INFO) << "InstanceV13_0_0Impl media pipeline ready version=" << _version
                     << " outgoingChannelId=" << _outgoingAudioChannelId.value_or("")
                     << " adm=" << (_audioDeviceModule ? 1 : 0)
                     << " call=" << (_call ? 1 : 0);
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV13_0_0Impl media pipeline ready outgoingChannelId=%{public}s",
                 _outgoingAudioChannelId.value_or("").c_str());
}

void InstanceV13_0_0ImplInternal::createNegotiatedChannels() {
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV13_0_0Impl createNegotiatedChannels mediaReady=%{public}d",
                 _mediaPipelineReady ? 1 : 0);
    if (!_mediaPipelineReady) {
        return;
    }
    const auto coordinatedState = _contentNegotiationContext->coordinatedState();
    if (!coordinatedState) {
        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV13_0_0Impl coordinatedState absent");
        return;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV13_0_0Impl coordinatedState outgoing=%{public}zu incoming=%{public}zu",
                 coordinatedState->outgoingContents.size(), coordinatedState->incomingContents.size());

    // Incoming audio channel from CNC coordinated state — peer's outgoing SSRC + codecs.
    for (const auto &content : coordinatedState->incomingContents) {
        if (content.type != signaling::MediaContent::Type::Audio) {
            continue;
        }
        if (_incomingAudioChannel && _incomingAudioChannel->ssrc() != content.ssrc) {
            // Renegotiation: destroy on the worker thread per webrtc::ChannelManager docs
            // (destructor off the worker thread crashed — 4.3 M1 hardening).
            _threads->getWorkerThread()->BlockingCall([this]() {
                _incomingAudioChannel.reset();
            });
        }
        if (!_incomingAudioChannel) {
            _threads->getWorkerThread()->BlockingCall([this, &content]() {
                _incomingAudioChannel = std::make_unique<IncomingAudioChannel_13_0_0>(
                    _channelManager.get(),
                    _call.get(),
                    _rtpTransport,
                    content,
                    _threads);
            });
            if (_incomingAudioChannel && !_incomingAudioChannel->isReady()) {
                OH_LOG_Print(LOG_APP, LOG_ERROR, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                             "InstanceV13_0_0Impl incoming channel inner VoiceChannel null — dropping wrapper ssrc=%{public}u",
                             content.ssrc);
                _threads->getWorkerThread()->BlockingCall([this]() {
                    _incomingAudioChannel.reset();
                });
            } else {
                OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                             "InstanceV13_0_0Impl incoming audio channel created ssrc=%{public}u",
                             content.ssrc);
            }
        }
        break;  // single audio stream
    }

    // Outgoing audio channel from coordinatedState->outgoingContents matched by the SSRC the
    // CNC allocated for our channel id — upstream InstanceV2Impl::createNegotiatedChannels
    // shape (no self-answer involved, the peer's NegotiateChannels answer coordinated it).
    if (_outgoingAudioChannelId) {
        const auto audioSsrc = _contentNegotiationContext->outgoingChannelSsrc(_outgoingAudioChannelId.value());
        if (audioSsrc) {
            if (_outgoingAudioChannel && _outgoingAudioChannel->ssrc() != audioSsrc.value()) {
                _threads->getWorkerThread()->BlockingCall([this]() {
                    _outgoingAudioChannel.reset();
                });
            }

            absl::optional<signaling::MediaContent> outgoingAudioContent;
            for (const auto &content : coordinatedState->outgoingContents) {
                if (content.type == signaling::MediaContent::Type::Audio && content.ssrc == audioSsrc.value()) {
                    outgoingAudioContent = content;
                    break;
                }
            }

            if (outgoingAudioContent && !_outgoingAudioChannel) {
                _threads->getWorkerThread()->BlockingCall([this, &outgoingAudioContent]() {
                    _outgoingAudioChannel = std::make_unique<OutgoingAudioChannel_13_0_0>(
                        _call.get(),
                        _channelManager.get(),
                        &_audioSource,
                        _rtpTransport,
                        outgoingAudioContent.value(),
                        _threads);
                });
                if (_outgoingAudioChannel && !_outgoingAudioChannel->isReady()) {
                    OH_LOG_Print(LOG_APP, LOG_ERROR, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                                 "InstanceV13_0_0Impl outgoing channel inner VoiceChannel null — dropping wrapper ssrc=%{public}u",
                                 audioSsrc.value());
                    _threads->getWorkerThread()->BlockingCall([this]() {
                        _outgoingAudioChannel.reset();
                    });
                } else if (_outgoingAudioChannel) {
                    _outgoingAudioChannel->setIsMuted(_muteMicrophone);
                    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                                 "InstanceV13_0_0Impl outgoing audio channel created ssrc=%{public}u",
                                 audioSsrc.value());

                    // M2.1-parity diagnostic (#65 E4): did SetAudioSend(true) propagate down to
                    // ADM->StartRecording() on real hardware? scoped_refptr by-value capture
                    // keeps the ADM alive even if the instance shuts down before the task fires.
                    auto admForLog = _audioDeviceModule;
                    _threads->getWorkerThread()->PostDelayedTask([admForLog]() {
                        if (!admForLog) {
                            return;
                        }
                        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                                     "InstanceV13_0_0Impl ADM check t+2s Recording=%{public}d Playing=%{public}d initialized=%{public}d playoutInitialized=%{public}d",
                                     admForLog->Recording() ? 1 : 0,
                                     admForLog->Playing() ? 1 : 0,
                                     admForLog->RecordingIsInitialized() ? 1 : 0,
                                     admForLog->PlayoutIsInitialized() ? 1 : 0);
                    }, webrtc::TimeDelta::Millis(2000));

                    // M2.4.2 audio-level pipeline for the in-call UI bars — same worker-thread
                    // polling, /8000 scaling and weak-task re-post pattern as InstanceV4_0_0Impl.
                    if (_audioLevelUpdated) {
                        auto alive = _diagAlive;
                        const uint32_t ssrcSnapshot = audioSsrc.value();
                        _audioLevelTask = std::make_shared<std::function<void()>>();
                        std::weak_ptr<std::function<void()>> weakTask = _audioLevelTask;
                        *_audioLevelTask = [this, alive, ssrcSnapshot, weakTask]() {
                            if (!alive->load()) {
                                return;
                            }
                            if (_outgoingAudioChannel && _audioLevelUpdated) {
                                webrtc::VoiceMediaSendInfo sendInfo;
                                if (_outgoingAudioChannel->getSendStats(&sendInfo)) {
                                    int rawLevel = 0;
                                    for (const auto &sender : sendInfo.senders) {
                                        if (sender.ssrc() == ssrcSnapshot || sender.local_stats.empty()) {
                                            rawLevel = sender.audio_level;
                                            break;
                                        }
                                    }
                                    // VoiceSenderInfo::audio_level is linear [0, 32767]; upstream
                                    // tgcalls treats peak/8000 as "full" — match it so the bars
                                    // are readable for normal speech.
                                    float level = static_cast<float>(rawLevel) / 8000.0f;
                                    if (level < 0.0f) {
                                        level = 0.0f;
                                    } else if (level > 1.0f) {
                                        level = 1.0f;
                                    }
                                    _audioLevelUpdated(level);
                                }
                            }
                            if (auto self = weakTask.lock()) {
                                _threads->getWorkerThread()->PostDelayedTask(*self,
                                                                             webrtc::TimeDelta::Millis(150));
                            }
                        };
                        _threads->getWorkerThread()->PostDelayedTask(*_audioLevelTask,
                                                                     webrtc::TimeDelta::Millis(150));
                    }
                }
            }
        }
    }
}

void InstanceV13_0_0ImplInternal::shutdownInstance() {
    // V3 signaling teardown first: flip the alive gate (media thread — same thread as the
    // queued PostTask continuations, so the flip sequences with them), then drop the SCTP
    // connection (its destructor BlockingCalls the network thread and drains the transport)
    // and the encryption state.
    _signalingAlive->store(false);
    _signalingConnection.reset();
    _signalingEncryptedConnection.reset();

    // Then the 4.3-hardened four-step ordering (see stop() comment in InstanceV4_0_0Impl):
    // stop callbacks -> fence network thread -> teardown media -> reset networking owner.
    if (_networking) {
        _networking->perform([](tgcalls::InstanceNetworking *networking) {
            networking->stop();
        });
        if (_threads) {
            _threads->getNetworkThread()->BlockingCall([]() {});
        }
    }

    teardownMediaPipelineOnWorkerThread();

    _networking.reset();
}

void InstanceV13_0_0ImplInternal::teardownMediaPipelineOnWorkerThread() {
    if (!_threads) {
        return;
    }
    // Channels MUST be destroyed before _networking (their destructors touch the RtpTransport
    // owned by NativeNetworkingImpl) and on the worker thread — 4.3 M1 hardening.
    _threads->getWorkerThread()->BlockingCall([this]() {
        _diagAlive->store(false);
        _audioLevelTask.reset();
        _incomingAudioChannel.reset();
        _outgoingAudioChannel.reset();
        _channelManager.reset();
        _call.reset();
        _audioDeviceModule = nullptr;
    });
    _rtpTransport = nullptr;
    _mediaPipelineReady = false;
}

void InstanceV13_0_0ImplInternal::setNetworkType(tgcalls::NetworkType networkType) {
    _networkType = networkType;
}

void InstanceV13_0_0ImplInternal::setMuteMicrophone(bool muteMicrophone) {
    if (_muteMicrophone == muteMicrophone) {
        return;
    }
    _muteMicrophone = muteMicrophone;
    if (_outgoingAudioChannel) {
        _outgoingAudioChannel->setIsMuted(muteMicrophone);
    }
    // Tell the peer (upstream pushes MediaState on every local mute flip).
    sendMediaState();
}

void InstanceV13_0_0ImplInternal::setIncomingVideoOutput(
    std::weak_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
    _incomingVideoOutput = std::move(sink);
}

void InstanceV13_0_0ImplInternal::setAudioInputDevice(std::string id) {
    _audioInputDeviceId = std::move(id);
}

void InstanceV13_0_0ImplInternal::setAudioOutputDevice(std::string id) {
    _audioOutputDeviceId = std::move(id);
}

void InstanceV13_0_0ImplInternal::setIsLowBatteryLevel(bool isLowBatteryLevel) {
    if (_isLowBatteryLevel == isLowBatteryLevel) {
        return;
    }
    _isLowBatteryLevel = isLowBatteryLevel;
    // Upstream V2/V3 push MediaState on battery flips, same as on mute flips.
    sendMediaState();
}

void InstanceV13_0_0ImplInternal::setVideoCapture(
    std::shared_ptr<tgcalls::VideoCaptureInterface> videoCapture) {
    _videoCapture = std::move(videoCapture);
}

void InstanceV13_0_0ImplInternal::setRequestedVideoAspect(float aspect) {
    _requestedVideoAspect = aspect;
}

void InstanceV13_0_0ImplInternal::stop(std::function<void(FinalState)> completion) {
    size_t incomingRtcpPacketCount = 0;
    {
        std::lock_guard<std::mutex> lock(_mediaDiagnosticsMutex);
        incomingRtcpPacketCount = _incomingRtcpPacketCount;
    }

    // Final two-direction RTP proof read straight from the webrtc send/receive channels.
    // The transportMessageReceived counters can NOT prove media flow here: in this snapshot
    // NativeNetworkingImpl::RtpPacketReceived_n is never invoked (RTP is demuxed directly
    // into the VoiceChannels), so those counters stay 0 on a perfectly healthy call.
    // Stats must be read on the worker thread, before shutdownInstance() destroys channels.
    bool txOk = false;
    int txPackets = 0;
    int64_t txBytes = 0;
    int txLost = 0;
    int64_t txRttMs = 0;
    bool rxOk = false;
    int rxPackets = 0;
    int64_t rxBytes = 0;
    int rxLost = 0;
    int rxJitterMs = 0;
    if (_outgoingAudioChannel || _incomingAudioChannel) {
        _threads->getWorkerThread()->BlockingCall([&]() {
            if (_outgoingAudioChannel) {
                webrtc::VoiceMediaSendInfo sendInfo;
                if (_outgoingAudioChannel->getSendStats(&sendInfo)) {
                    for (const auto &sender : sendInfo.senders) {
                        if (sender.ssrc() == _outgoingAudioChannel->ssrc() || sender.local_stats.empty()) {
                            txOk = true;
                            txPackets = sender.packets_sent;
                            txBytes = sender.payload_bytes_sent;
                            txLost = sender.packets_lost;
                            txRttMs = sender.rtt_ms;
                            break;
                        }
                    }
                }
            }
            if (_incomingAudioChannel) {
                webrtc::VoiceMediaReceiveInfo receiveInfo;
                if (_incomingAudioChannel->getReceiveStats(&receiveInfo)) {
                    for (const auto &receiver : receiveInfo.receivers) {
                        if (receiver.ssrc() == _incomingAudioChannel->ssrc() || receiver.local_stats.empty()) {
                            rxOk = true;
                            rxPackets = receiver.packets_received;
                            rxBytes = receiver.payload_bytes_received;
                            rxLost = receiver.packets_lost;
                            rxJitterMs = receiver.jitter_ms;
                            break;
                        }
                    }
                }
            }
        });
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV13_0_0Impl stopping version=%{public}s signalingRx=%{public}zu signalingTx=%{public}zu rtcpPackets=%{public}zu mediaReady=%{public}d outgoingChan=%{public}d incomingChan=%{public}d",
                 _version.c_str(),
                 _signalingRxPacketCount,
                 _signalingTxPacketCount,
                 incomingRtcpPacketCount,
                 _mediaPipelineReady ? 1 : 0,
                 _outgoingAudioChannel ? 1 : 0,
                 _incomingAudioChannel ? 1 : 0);
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV13_0_0Impl final media stats txOk=%{public}d txPackets=%{public}d txBytes=%{public}lld txLost=%{public}d txRttMs=%{public}lld rxOk=%{public}d rxPackets=%{public}d rxBytes=%{public}lld rxLost=%{public}d rxJitterMs=%{public}d",
                 txOk ? 1 : 0,
                 txPackets,
                 (long long)txBytes,
                 txLost,
                 (long long)txRttMs,
                 rxOk ? 1 : 0,
                 rxPackets,
                 (long long)rxBytes,
                 rxLost,
                 rxJitterMs);

    shutdownInstance();

    if (completion) {
        completion(tgcalls::FinalState());
    }
}

InstanceV13_0_0Impl::InstanceV13_0_0Impl(Descriptor &&descriptor) {
    if (descriptor.config.logPath.data.size() != 0) {
        _logSink = std::make_unique<LogSinkImpl>(descriptor.config.logPath);
    }
#ifdef DEBUG
    webrtc::LogMessage::LogToDebug(webrtc::LS_VERBOSE);
#else
    webrtc::LogMessage::LogToDebug(webrtc::LS_INFO);
#endif
    webrtc::LogMessage::SetLogToStderr(false);
    if (_logSink) {
        webrtc::LogMessage::AddLogToStream(_logSink.get(), webrtc::LS_INFO);
    }

    _threads = StaticThreads::getThreads();
    _internal.reset(new ThreadLocalObject<InstanceV13_0_0ImplInternal>(_threads->getMediaThread(), [descriptor = std::move(descriptor), threads = _threads]() mutable {
        return std::make_shared<InstanceV13_0_0ImplInternal>(std::move(descriptor), threads);
    }));
    _internal->perform([](InstanceV13_0_0ImplInternal *internal) {
        internal->start();
    });
}

InstanceV13_0_0Impl::~InstanceV13_0_0Impl() {
    webrtc::LogMessage::RemoveLogToStream(_logSink.get());
}

void InstanceV13_0_0Impl::receiveSignalingData(const std::vector<uint8_t> &data) {
    _internal->perform([data](InstanceV13_0_0ImplInternal *internal) {
        internal->receiveSignalingData(data);
    });
}

void InstanceV13_0_0Impl::setVideoCapture(std::shared_ptr<VideoCaptureInterface> videoCapture) {
    _internal->perform([videoCapture](InstanceV13_0_0ImplInternal *internal) {
        internal->setVideoCapture(videoCapture);
    });
}

void InstanceV13_0_0Impl::setRequestedVideoAspect(float aspect) {
    _internal->perform([aspect](InstanceV13_0_0ImplInternal *internal) {
        internal->setRequestedVideoAspect(aspect);
    });
}

void InstanceV13_0_0Impl::setNetworkType(NetworkType networkType) {
    _internal->perform([networkType](InstanceV13_0_0ImplInternal *internal) {
        internal->setNetworkType(networkType);
    });
}

void InstanceV13_0_0Impl::setMuteMicrophone(bool muteMicrophone) {
    _internal->perform([muteMicrophone](InstanceV13_0_0ImplInternal *internal) {
        internal->setMuteMicrophone(muteMicrophone);
    });
}

void InstanceV13_0_0Impl::setIncomingVideoOutput(std::weak_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
    _internal->perform([sink](InstanceV13_0_0ImplInternal *internal) {
        internal->setIncomingVideoOutput(sink);
    });
}

void InstanceV13_0_0Impl::setAudioInputDevice(std::string id) {
    _internal->perform([id](InstanceV13_0_0ImplInternal *internal) {
        internal->setAudioInputDevice(id);
    });
}

void InstanceV13_0_0Impl::setAudioOutputDevice(std::string id) {
    _internal->perform([id](InstanceV13_0_0ImplInternal *internal) {
        internal->setAudioOutputDevice(id);
    });
}

void InstanceV13_0_0Impl::setIsLowBatteryLevel(bool isLowBatteryLevel) {
    _internal->perform([isLowBatteryLevel](InstanceV13_0_0ImplInternal *internal) {
        internal->setIsLowBatteryLevel(isLowBatteryLevel);
    });
}

void InstanceV13_0_0Impl::setInputVolume(float level) {
    (void)level;
}

void InstanceV13_0_0Impl::setOutputVolume(float level) {
    (void)level;
}

void InstanceV13_0_0Impl::setAudioOutputDuckingEnabled(bool enabled) {
    (void)enabled;
}

void InstanceV13_0_0Impl::setAudioOutputGainControlEnabled(bool enabled) {
    (void)enabled;
}

void InstanceV13_0_0Impl::setEchoCancellationStrength(int strength) {
    (void)strength;
}

std::vector<std::string> InstanceV13_0_0Impl::GetVersions() {
    // ADR-0009: only 13.0.0 — upstream treats 12.0.0 identically (both map to V3) but the
    // production ecosystem (WebK, Android) negotiates 13.0.0; a smaller declared surface is
    // a smaller test surface.
    std::vector<std::string> result;
    result.push_back("13.0.0");
    return result;
}

int InstanceV13_0_0Impl::GetConnectionMaxLayer() {
    return 92;
}

std::string InstanceV13_0_0Impl::getLastError() {
    return "";
}

std::string InstanceV13_0_0Impl::getDebugInfo() {
    return "";
}

int64_t InstanceV13_0_0Impl::getPreferredRelayId() {
    return 0;
}

TrafficStats InstanceV13_0_0Impl::getTrafficStats() {
    return {};
}

PersistentState InstanceV13_0_0Impl::getPersistentState() {
    return {};
}

void InstanceV13_0_0Impl::stop(std::function<void(FinalState)> completion) {
    std::string debugLog;
    if (_logSink) {
        debugLog = _logSink->result();
    }
    _internal->perform([completion, debugLog = std::move(debugLog)](InstanceV13_0_0ImplInternal *internal) mutable {
        internal->stop([completion, debugLog = std::move(debugLog)](FinalState finalState) mutable {
            finalState.debugLog = debugLog;
            completion(finalState);
        });
    });
}

template <>
bool Register<InstanceV13_0_0Impl>() {
    return Meta::RegisterOne<InstanceV13_0_0Impl>();
}

} // namespace tgcalls
