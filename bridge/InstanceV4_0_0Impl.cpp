// InstanceV4_0_0Impl — implementacja tgcalls::Instance dla protokołu 4.0.0 (Web-K wire format).
//
// Strategia: fuzja dwóch źródeł (patrz docs/adr/0001-tgcalls-instance-selection.md rev 3).
//   - signaling 4.0.0 + transport portowany z tgcall/src/main/cpp/diagnostic_tgcall_instance.cpp (krok 2b, ten plik)
//   - media pipeline portowany z third_party/tgcalls/tgcalls/v2/InstanceV2Impl.cpp (krok 2c, TODO)
//   - integracja warstw w processSignalingData (krok 2d, TODO)
//
// Po kroku 2b ten plik obsługuje pełną negocjację 4.0.0 (ICE/DTLS/Candidates/MediaState) ale nie ma
// jeszcze media pipeline'u (webrtc::Call, AudioReceiveStream). To stan zbliżony do działania
// DiagnosticTgCallInstance: transport works, audio still silent. Kroki 2c/2d dorzucą media path.

#include "InstanceV4_0_0Impl.h"

#include "tgcall_logging.h"
#include "webk_signaling_adapter.h"

#include <hilog/log.h>

#include "tgcalls/ChannelManager.h"
#include "tgcalls/CryptoHelper.h"
#include "tgcalls/FieldTrialsConfig.h"
#include "tgcalls/Instance.h"
#include "tgcalls/LogSinkImpl.h"
#include "tgcalls/StaticThreads.h"
#include "tgcalls/ThreadLocalObject.h"
#include "tgcalls/platform/PlatformInterface.h"
#include "tgcalls/v2/ContentNegotiation.h"
#include "tgcalls/v2/NativeNetworkingImpl.h"
#include "tgcalls/v2/Signaling.h"
#include "tgcalls/v2_4_0_0/Signaling_4_0_0.h"

#include "api/audio_codecs/audio_decoder_factory_template.h"
#include "api/audio_codecs/audio_encoder_factory_template.h"
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"
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
#include "rtc_base/crypto_random.h"
#include "rtc_base/ip_address.h"
#include "rtc_base/logging.h"
#include "rtc_base/ssl_fingerprint.h"
#include "rtc_base/unique_id_generator.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
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

constexpr size_t kMessageKeySize = 16;
constexpr size_t kSequenceSize = 4;

// Incoming-audio gain multiplier applied via VoiceMediaReceiveChannelInterface::
// SetOutputVolume in IncomingAudioChannel_4_0_0. Compensates for the
// AUDIOSTREAM_USAGE_VOICE_COMMUNICATION earpiece-style routing in
// third_party/ohos_webrtc OHAudioPlayer that leaves the watch speaker quiet.
// 2.0 = +6 dB; subjective sweet spot determined by smoke — bump up if still
// quiet, drop if clipping becomes audible.
constexpr double kIncomingVolumeBoost = 2.0;

bool ShouldLogMediaPacketCount(size_t count) {
    return count <= 5 || count == 10 || count == 25 || (count % 50) == 0;
}

void WriteBigEndian32(std::vector<uint8_t> &buffer, uint32_t value) {
    buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    buffer.push_back(static_cast<uint8_t>(value & 0xff));
}

uint32_t ReadBigEndian32(const uint8_t *buffer) {
    return (static_cast<uint32_t>(buffer[0]) << 24) |
           (static_cast<uint32_t>(buffer[1]) << 16) |
           (static_cast<uint32_t>(buffer[2]) << 8) |
           static_cast<uint32_t>(buffer[3]);
}

bool ConstTimeDifferent(const uint8_t *lhs, const uint8_t *rhs, size_t size) {
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) {
        difference = static_cast<uint8_t>(difference | (lhs[i] ^ rhs[i]));
    }
    return difference != 0;
}

std::vector<uint8_t> EncryptRawSignalingPacket(
    const std::vector<uint8_t> &payload,
    const std::array<uint8_t, tgcalls::EncryptionKey::kSize> &key,
    bool keyIsOutgoing,
    uint32_t sequence) {
    std::vector<uint8_t> prepared;
    prepared.reserve(kSequenceSize + payload.size());
    WriteBigEndian32(prepared, sequence);
    prepared.insert(prepared.end(), payload.begin(), payload.end());

    const int x = (keyIsOutgoing ? 0 : 8) + 128;
    const std::array<uint8_t, tgcalls::kSha256Size> messageKeyLarge = tgcalls::ConcatSHA256(
        tgcalls::MemorySpan{key.data() + 88 + x, 32},
        tgcalls::MemorySpan{prepared.data(), prepared.size()});

    std::array<uint8_t, kMessageKeySize> messageKey = {};
    std::memcpy(messageKey.data(), messageKeyLarge.data() + 8, messageKey.size());

    std::vector<uint8_t> result(kMessageKeySize + prepared.size());
    std::memcpy(result.data(), messageKey.data(), messageKey.size());
    tgcalls::AesKeyIv keyIv = tgcalls::PrepareAesKeyIv(key.data(), messageKey.data(), x);
    tgcalls::AesProcessCtr(
        tgcalls::MemorySpan{prepared.data(), prepared.size()},
        result.data() + kMessageKeySize,
        std::move(keyIv));
    return result;
}

bool DecryptRawSignalingPacket(
    const std::vector<uint8_t> &encrypted,
    const std::array<uint8_t, tgcalls::EncryptionKey::kSize> &key,
    bool keyIsOutgoing,
    std::vector<uint8_t> &payload,
    uint32_t &sequence) {
    if (encrypted.size() < kMessageKeySize + kSequenceSize) {
        return false;
    }

    const int x = (keyIsOutgoing ? 8 : 0) + 128;
    const uint8_t *messageKey = encrypted.data();
    const uint8_t *encryptedData = encrypted.data() + kMessageKeySize;
    const size_t encryptedSize = encrypted.size() - kMessageKeySize;

    std::vector<uint8_t> decrypted(encryptedSize);
    tgcalls::AesKeyIv keyIv = tgcalls::PrepareAesKeyIv(key.data(), messageKey, x);
    tgcalls::AesProcessCtr(
        tgcalls::MemorySpan{encryptedData, encryptedSize},
        decrypted.data(),
        std::move(keyIv));

    const std::array<uint8_t, tgcalls::kSha256Size> messageKeyLarge = tgcalls::ConcatSHA256(
        tgcalls::MemorySpan{key.data() + 88 + x, 32},
        tgcalls::MemorySpan{decrypted.data(), decrypted.size()});
    if (ConstTimeDifferent(messageKeyLarge.data() + 8, messageKey, kMessageKeySize)) {
        return false;
    }

    sequence = ReadBigEndian32(decrypted.data());
    if (sequence == 0) {
        return false;
    }

    payload.assign(decrypted.begin() + kSequenceSize, decrypted.end());
    return true;
}

const char *WebKMessageTypeName(const tgcalls::signaling_4_0_0::Message &message) {
    if (absl::get_if<tgcalls::signaling_4_0_0::InitialSetupMessage>(&message.data) != nullptr) {
        return "InitialSetup";
    }
    if (absl::get_if<tgcalls::signaling_4_0_0::CandidatesMessage>(&message.data) != nullptr) {
        return "Candidates";
    }
    if (absl::get_if<tgcalls::signaling_4_0_0::MediaStateMessage>(&message.data) != nullptr) {
        return "MediaState";
    }
    return "Unknown";
}

tgcalls::signaling_4_0_0::MediaContent BuildLocalMediaContent(
    const tgcalls::signaling_4_0_0::MediaContent &remoteContent) {
    tgcalls::signaling_4_0_0::MediaContent localContent;
    std::map<uint32_t, uint32_t> ssrcMap;

    const auto mapSsrc = [&ssrcMap](uint32_t remoteSsrc) {
        const auto existing = ssrcMap.find(remoteSsrc);
        if (existing != ssrcMap.end()) {
            return existing->second;
        }
        uint32_t localSsrc = webrtc::CreateRandomNonZeroId();
        while (localSsrc == 0) {
            localSsrc = webrtc::CreateRandomNonZeroId();
        }
        ssrcMap[remoteSsrc] = localSsrc;
        return localSsrc;
    };

    if (remoteContent.ssrc != 0) {
        localContent.ssrc = mapSsrc(remoteContent.ssrc);
    }

    localContent.ssrcGroups.reserve(remoteContent.ssrcGroups.size());
    for (const tgcalls::signaling_4_0_0::SsrcGroup &remoteGroup : remoteContent.ssrcGroups) {
        tgcalls::signaling_4_0_0::SsrcGroup localGroup;
        localGroup.semantics = remoteGroup.semantics;
        localGroup.ssrcs.reserve(remoteGroup.ssrcs.size());
        for (uint32_t remoteSsrc : remoteGroup.ssrcs) {
            const uint32_t localSsrc = mapSsrc(remoteSsrc);
            localGroup.ssrcs.push_back(localSsrc);
            if (localContent.ssrc == 0) {
                localContent.ssrc = localSsrc;
            }
        }
        localContent.ssrcGroups.push_back(std::move(localGroup));
    }

    if (localContent.ssrc == 0) {
        localContent.ssrc = webrtc::CreateRandomNonZeroId();
    }

    localContent.payloadTypes = remoteContent.payloadTypes;
    localContent.rtpExtensions = remoteContent.rtpExtensions;
    return localContent;
}

bool BuildLocalInitialSetup(
    const tgcalls::signaling_4_0_0::InitialSetupMessage &remoteInitialSetup,
    const tgcalls::PeerIceParameters &localIceParameters,
    const webrtc::SSLFingerprint &localFingerprint,
    bool isOutgoing,
    tgcalls::signaling_4_0_0::InitialSetupMessage &localInitialSetup) {
    if (!remoteInitialSetup.audio.has_value()) {
        return false;
    }

    tgcalls::signaling_4_0_0::DtlsFingerprint fingerprint;
    fingerprint.hash = localFingerprint.algorithm;
    // Web-K 4.0.0 only completes ICE nomination when the callee answers with setup=active;
    // emitting setup=passive caused Telegram Web to stop nominating any candidate pair.
    // The matching SSL role override lives in configureRemoteInitialSetup so tgcalls picks
    // SSL_CLIENT and actually starts the DTLS handshake.
    fingerprint.setup = isOutgoing ? "actpass" : "active";
    fingerprint.fingerprint = localFingerprint.GetRfc4572Fingerprint();

    localInitialSetup.ufrag = localIceParameters.ufrag;
    localInitialSetup.pwd = localIceParameters.pwd;
    localInitialSetup.fingerprints.push_back(std::move(fingerprint));
    localInitialSetup.audio = BuildLocalMediaContent(remoteInitialSetup.audio.value());
    // Web-K rejects an answer that drops offered media sections before it reaches setRemoteDescription.
    if (remoteInitialSetup.video.has_value()) {
        localInitialSetup.video = BuildLocalMediaContent(remoteInitialSetup.video.value());
    }
    if (remoteInitialSetup.screencast.has_value()) {
        localInitialSetup.screencast = BuildLocalMediaContent(remoteInitialSetup.screencast.value());
    }
    return true;
}

bool EndsWithReflectorHostname(const std::string &hostname) {
    const std::string suffix = ".reflector";
    return hostname.size() >= suffix.size() &&
           hostname.compare(hostname.size() - suffix.size(), suffix.size(), suffix) == 0;
}

uint8_t ParseReflectorServerId(const std::string &hostname) {
    const std::string prefix = "reflector-";
    if (hostname.compare(0, prefix.size(), prefix) != 0) {
        return 0;
    }

    uint32_t value = 0;
    bool hasDigit = false;
    for (size_t index = prefix.size(); index < hostname.size(); index += 1) {
        const char character = hostname[index];
        if (character == '-') {
            break;
        }
        if (character < '0' || character > '9') {
            return 0;
        }
        hasDigit = true;
        value = value * 10 + static_cast<uint32_t>(character - '0');
        if (value > 255) {
            return 0;
        }
    }

    return hasDigit ? static_cast<uint8_t>(value) : 0;
}

bool RewriteReflectorCandidateAddressForWebK(
    webrtc::SocketAddress &address,
    const std::vector<tgcalls::RtcServer> &rtcServers) {
    const std::string hostname = address.hostname();
    if (!EndsWithReflectorHostname(hostname)) {
        return false;
    }

    const uint8_t serverId = ParseReflectorServerId(hostname);
    if (serverId == 0) {
        return false;
    }

    for (const tgcalls::RtcServer &server : rtcServers) {
        if (server.id != serverId || server.login != "reflector" || server.host.empty()) {
            continue;
        }

        webrtc::IPAddress ipAddress;
        if (!webrtc::IPFromString(server.host, &ipAddress)) {
            return false;
        }

        address.SetIP(ipAddress);
        return true;
    }

    return false;
}

void ReplaceAll(std::string &value, const std::string &from, const std::string &to) {
    if (from.empty()) {
        return;
    }

    size_t position = 0;
    while ((position = value.find(from, position)) != std::string::npos) {
        value.replace(position, from.length(), to);
        position += to.length();
    }
}

void NormalizeWebKCandidateSdp(std::string &candidate) {
    ReplaceAll(candidate, " typ local ", " typ host ");
    ReplaceAll(candidate, " typ stun ", " typ srflx ");
}

}  // namespace

// Port from tgcalls/v2/InstanceV2Impl.cpp OutgoingAudioChannel — adapted for legacy
// webrtc snapshot (no webrtc::Environment) and trimmed to Opus only.
class OutgoingAudioChannel_4_0_0 : public sigslot::has_slots<> {
public:
    OutgoingAudioChannel_4_0_0(
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
        // APM on top double-processes the signal and AGC flattens it to silence (smoke A/B
        // 2026-05-13: AEC/NS/AGC=true -> Web hears silence; =false -> full duplex audio, no echo
        // at 2 m separation). Do NOT flip these to true without revising ADR-0006.
        audioOptions.echo_cancellation = false;
        audioOptions.noise_suppression = false;
        audioOptions.auto_gain_control = false;
        // Runtime trace so every smoke hilog confirms ADR-0006 is active in this build —
        // single line per call setup, addresses code-review residual risk #2 (no automated test
        // for audio options config).
        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "OutgoingAudioChannel_4_0_0 audio options applied (ADR-0006) aec=%{public}d ns=%{public}d agc=%{public}d ssrc=%{public}u",
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
                     "OutgoingAudioChannel_4_0_0 CreateVoiceChannel ssrc=%{public}u channel=%{public}d codecs=%{public}zu",
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
                         "OutgoingAudioChannel_4_0_0 SetContent local=%{public}d remote=%{public}d err=%{public}s",
                         localOk ? 1 : 0, remoteOk ? 1 : 0, errorDesc.c_str());
        });

        setIsMuted(false);
    }

    ~OutgoingAudioChannel_4_0_0() {
        if (!_outgoingAudioChannel) {
            OH_LOG_Print(LOG_APP, LOG_WARN, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "OutgoingAudioChannel_4_0_0 destructor skipped — channel never created");
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
        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "OutgoingAudioChannel_4_0_0 setIsMuted before muted=%{public}d audioSource=%{public}p ssrc=%{public}u",
                     _isMuted ? 1 : 0, (void *)_audioSource, _ssrc);
        _threads->getWorkerThread()->BlockingCall([this]() {
            _outgoingAudioChannel->voice_media_send_channel()->SetAudioSend(_ssrc, !_isMuted, nullptr, _audioSource);
        });
        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "OutgoingAudioChannel_4_0_0 setIsMuted after muted=%{public}d ssrc=%{public}u",
                     _isMuted ? 1 : 0, _ssrc);
    }

    bool isReady() const {
        return _outgoingAudioChannel != nullptr;
    }

    uint32_t ssrc() const {
        return _ssrc;
    }

    // M2.2 diagnostic: pulls send-side stats off the inner webrtc::VoiceChannel without
    // exposing the raw pointer to callers. Must be invoked on the worker thread.
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

// Port from tgcalls/v2/InstanceV2Impl.cpp IncomingV2AudioChannel — adapted for legacy
// webrtc snapshot. Owns a receive-only VoiceChannel that feeds AudioReceiveStream
// from RtpTransport demux, decoded by Opus, played through Huawei OHOS ADM.
class IncomingAudioChannel_4_0_0 : public sigslot::has_slots<> {
public:
    IncomingAudioChannel_4_0_0(
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
                     "IncomingAudioChannel_4_0_0 CreateVoiceChannel ssrc=%{public}u channel=%{public}d codecs=%{public}zu",
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
                         "IncomingAudioChannel_4_0_0 SetContent local=%{public}d remote=%{public}d err=%{public}s",
                         localOk ? 1 : 0, remoteOk ? 1 : 0, errorDesc.c_str());
            // Compensate for AUDIOSTREAM_USAGE_VOICE_COMMUNICATION earpiece-style
            // routing in third_party/ohos_webrtc OHAudioPlayer: watch has no
            // earpiece, system maps the stream to the speaker at low gain.
            // Apply programmatic gain on the receive channel — same pattern as
            // third_party/tgcalls/v2/InstanceV2Impl IncomingV2AudioChannel::setVolume.
            // Must run on the worker thread (same affinity as SetContent above).
            _audioChannel->voice_media_receive_channel()->SetOutputVolume(_ssrc, kIncomingVolumeBoost);
            OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "IncomingAudioChannel_4_0_0 SetOutputVolume ssrc=%{public}u gain=%{public}.1f",
                         _ssrc, kIncomingVolumeBoost);
        });

        _audioChannel->Enable(true);
    }

    ~IncomingAudioChannel_4_0_0() {
        if (!_audioChannel) {
            OH_LOG_Print(LOG_APP, LOG_WARN, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "IncomingAudioChannel_4_0_0 destructor skipped — channel never created");
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

private:
    std::shared_ptr<Threads> _threads;
    uint32_t _ssrc = 0;
    webrtc::ChannelInterface *_audioChannel = nullptr;
    ChannelManager *_channelManager = nullptr;
    webrtc::Call *_call = nullptr;
};

class InstanceV4_0_0ImplInternal {
public:
    InstanceV4_0_0ImplInternal(Descriptor &&descriptor, std::shared_ptr<Threads> threads);
    ~InstanceV4_0_0ImplInternal();

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
    void configureRemoteInitialSetup(
        const tgcalls::signaling_4_0_0::InitialSetupMessage &remoteInitialSetup);
    void addRemoteCandidates(
        const tgcalls::signaling_4_0_0::CandidatesMessage &remoteCandidates);
    void emitLocalInitialSetup(
        const tgcalls::signaling_4_0_0::InitialSetupMessage &remoteInitialSetup,
        const tgcalls::signaling::NegotiateChannelsMessage *answerNegotiation);
    // ADR-0008: caller-role initial signaling — proactively emits the first
    // InitialSetup for an outgoing call, built from scratch (no remote to echo).
    void emitCallerInitialSetup();
    void emitLocalCandidate(const webrtc::Candidate &candidate);
    void emitMediaState();
    void onTransportMessageReceived(const webrtc::CopyOnWriteBuffer &packet, bool isUnresolved);
    void onRtcpPacketReceived(const webrtc::CopyOnWriteBuffer &packet, int64_t packetTimeUs);
    void onDataChannelStateUpdated(bool isOpen);
    void onDataChannelMessageReceived(const std::string &message);
    bool encryptWebKMessage(
        const tgcalls::signaling_4_0_0::Message &message,
        uint32_t &sequence,
        std::vector<uint8_t> &encryptedPayload);

    webrtc::scoped_refptr<webrtc::AudioDeviceModule> createAudioDeviceModule();
    void setUpMediaPipeline();
    void createNegotiatedChannels();
    void teardownMediaPipelineOnWorkerThread();
    void shutdownInstance();

    std::string _version;
    tgcalls::Config _config;
    std::vector<tgcalls::RtcServer> _rtcServers;
    absl::optional<tgcalls::Proxy> _proxy;
    std::shared_ptr<tgcalls::Threads> _threads;
    std::shared_ptr<tgcalls::ThreadLocalObject<tgcalls::InstanceNetworking>> _networking;
    std::shared_ptr<const std::array<uint8_t, tgcalls::EncryptionKey::kSize>> _encryptionKey;
    bool _encryptionKeyIsOutgoing = false;
    std::mutex _signalingMutex;
    uint32_t _outgoingSequence = 1;
    bool _networkingStarted = false;
    bool _initialSetupEmitScheduled = false;
    bool _initialSetupEmitted = false;
    bool _mediaStateEmitted = false;
    std::function<void(tgcalls::State)> _stateUpdated;
    std::function<void(const std::vector<uint8_t> &)> _signalingDataEmitted;
    // M2.4.2: periodic outgoing-microphone level [0.0, 1.0], emitted from the worker thread.
    std::function<void(float)> _audioLevelUpdated;
    tgcalls::NetworkType _networkType = tgcalls::NetworkType::Unknown;
    bool _muteMicrophone = false;
    bool _isLowBatteryLevel = false;
    std::weak_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> _incomingVideoOutput;
    std::string _audioInputDeviceId;
    std::string _audioOutputDeviceId;
    std::shared_ptr<tgcalls::VideoCaptureInterface> _videoCapture;
    float _requestedVideoAspect = 0.0f;
    size_t _signalingPacketCount = 0;
    size_t _signalingByteCount = 0;
    std::mutex _mediaDiagnosticsMutex;
    size_t _incomingRtpPacketCount = 0;
    size_t _incomingRtpByteCount = 0;
    size_t _unresolvedRtpPacketCount = 0;
    size_t _incomingRtcpPacketCount = 0;
    size_t _incomingRtcpByteCount = 0;
    size_t _dataChannelMessageCount = 0;
    size_t _dataChannelMessageByteCount = 0;
    bool _dataChannelOpen = false;

    // Media pipeline (ported from InstanceV2ImplInternal, adapted to legacy webrtc API).
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
    uint32_t _outgoingAudioSsrc = 0;
    absl::optional<signaling::MediaContent> _outgoingAudioOffer;
    // ADR-0008: exchangeId of our pending outgoing offer (caller role). Used by the
    // self-answer in setUpMediaPipeline to match _pendingOutgoingOffer and finalise our
    // outgoing-media negotiation locally.
    uint32_t _outgoingOfferExchangeId = 0;
    std::unique_ptr<OutgoingAudioChannel_4_0_0> _outgoingAudioChannel;
    std::unique_ptr<IncomingAudioChannel_4_0_0> _incomingAudioChannel;
    bool _mediaPipelineReady = false;
    // M2.2 diagnostic: gates delayed tasks that read _call/_outgoingAudioChannel after teardown.
    // Set to false on the worker thread inside teardownMediaPipelineOnWorkerThread, BEFORE the
    // unique_ptr resets, so a captured-by-value shared_ptr<atomic<bool>> in a PostDelayedTask
    // can early-return safely. Both the flip and the delayed tasks live on the worker thread,
    // so reads of `*alive` happen sequentially with the flip — no race window.
    std::shared_ptr<std::atomic<bool>> _diagAlive = std::make_shared<std::atomic<bool>>(true);
    // M2.4.2: holds the periodic audio-level task. The self-re-posting lambda captures a
    // weak_ptr to this (not a shared_ptr) to avoid a reference cycle; this strong ref keeps
    // it alive and is reset on the worker thread during teardown to stop the chain.
    std::shared_ptr<std::function<void()>> _audioLevelTask;
    std::function<webrtc::scoped_refptr<webrtc::AudioDeviceModule>(webrtc::TaskQueueFactory *)>
        _createAudioDeviceModuleHook;
};

InstanceV4_0_0ImplInternal::InstanceV4_0_0ImplInternal(
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
    RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl created version=" << _version
                     << " rtcServers=" << _rtcServers.size()
                     << " keyOutgoing=" << (_encryptionKeyIsOutgoing ? 1 : 0);
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV4_0_0Impl created version=%{public}s rtcServers=%{public}zu keyOutgoing=%{public}d",
                 _version.c_str(), _rtcServers.size(), _encryptionKeyIsOutgoing ? 1 : 0);
}

InstanceV4_0_0ImplInternal::~InstanceV4_0_0ImplInternal() {
    if (_threads) {
        // Idempotent fallback when stop() was not invoked (e.g. exception path).
        // Same defensive ordering as stop() — fences networking callbacks before
        // tearing down the media pipeline so a stray RTCP callback cannot race
        // with _call.reset(). No-op when stop() already ran.
        shutdownInstance();
    } else {
        _incomingAudioChannel.reset();
        _outgoingAudioChannel.reset();
        _channelManager.reset();
    }
    _contentNegotiationContext.reset();
}

void InstanceV4_0_0ImplInternal::start() {
    if (!_encryptionKey) {
        RTC_LOG(LS_ERROR) << "InstanceV4_0_0Impl::start aborted, missing encryption key";
        return;
    }
    // Web-K hardcodes enableP2P regardless of TDLib config — both directions must agree on P2P.
    const bool enableP2P = _config.enableP2P || true;
    RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl networking config p2p=" << (enableP2P ? 1 : 0)
                     << " tcp=" << (_config.allowTCP ? 1 : 0)
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
         enableTcp = _config.allowTCP,
         enableP2P]() {
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
                        RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl networking state version=" << _version
                                         << " ready=" << (state.isReadyToSendData ? 1 : 0)
                                         << " failed=" << (state.isFailed ? 1 : 0)
                                         << " route=" << (state.route.has_value() ? 1 : 0)
                                         << " connection=" << (state.connection.has_value() ? 1 : 0);
                        if (state.connection.has_value()) {
                            RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl networking connection local="
                                             << state.connection->local.protocol << "/"
                                             << state.connection->local.type << "/"
                                             << state.connection->local.address
                                             << " remote=" << state.connection->remote.protocol << "/"
                                             << state.connection->remote.type << "/"
                                             << state.connection->remote.address;
                        }
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
                    .dataChannelStateUpdated = [this](bool isOpen) {
                        onDataChannelStateUpdated(isOpen);
                    },
                    .dataChannelMessageReceived = [this](std::string const &message) {
                        onDataChannelMessageReceived(message);
                    },
                    .threads = threads,
                    .customParameters = std::map<std::string, json11::Json>()}));
        });
    if (_stateUpdated) {
        _stateUpdated(tgcalls::State::WaitInit);
        _stateUpdated(tgcalls::State::Established);
    }

    setUpMediaPipeline();

    // ADR-0008: the watch as caller must send the first InitialSetup — the callee (Web)
    // waits for it. The callee path stays purely reactive (emits from receiveSignalingData).
    if (_encryptionKeyIsOutgoing) {
        bool shouldEmit = false;
        {
            std::lock_guard<std::mutex> lock(_signalingMutex);
            if (!_initialSetupEmitScheduled) {
                _initialSetupEmitScheduled = true;
                shouldEmit = true;
            }
        }
        if (shouldEmit) {
            emitCallerInitialSetup();
        }
    }
}

webrtc::scoped_refptr<webrtc::AudioDeviceModule>
InstanceV4_0_0ImplInternal::createAudioDeviceModule() {
    const auto check = [](const webrtc::scoped_refptr<webrtc::AudioDeviceModule> &result) {
        return (result && result->Init() == 0) ? result : nullptr;
    };
    if (_createAudioDeviceModuleHook) {
        if (auto result = check(_createAudioDeviceModuleHook(_taskQueueFactory.get()))) {
            return result;
        }
    }
    // Per ADR-0002 rev 2: webrtc::AudioDeviceModule::Create with kPlatformDefaultAudio
    // auto-selects Huawei OHOS ADM (OHAudioPlayer + OHAudioRecorder) when WEBRTC_OHOS
    // is defined. The OHOS-side log to confirm: "OHOS Audio APIs will be utilized."
    // In webrtc_latest the static AudioDeviceModule::Create was removed; the
    // equivalent factory is CreateDefaultAudioDeviceModule(), exported by
    // libohos_webrtc.so and declared above.
    return check(webrtc::CreateDefaultAudioDeviceModule());
}

void InstanceV4_0_0ImplInternal::setUpMediaPipeline() {
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV4_0_0Impl setUpMediaPipeline begin version=%{public}s ready=%{public}d networking=%{public}d",
                 _version.c_str(), _mediaPipelineReady ? 1 : 0, _networking ? 1 : 0);
    if (_mediaPipelineReady || !_networking) {
        return;
    }

    _threads->getWorkerThread()->BlockingCall([this]() {
        _audioDeviceModule = createAudioDeviceModule();
    });
    if (!_audioDeviceModule) {
        RTC_LOG(LS_ERROR) << "InstanceV4_0_0Impl ADM create failed — media pipeline aborted";
        OH_LOG_Print(LOG_APP, LOG_ERROR, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV4_0_0Impl ADM create failed — media pipeline aborted");
        return;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV4_0_0Impl ADM created");

    _threads->getWorkerThread()->BlockingCall([this]() {
        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV4_0_0Impl ADM baseline Recording=%{public}d Playing=%{public}d initialized=%{public}d playoutInitialized=%{public}d",
                     _audioDeviceModule->Recording() ? 1 : 0,
                     _audioDeviceModule->Playing() ? 1 : 0,
                     _audioDeviceModule->RecordingIsInitialized() ? 1 : 0,
                     _audioDeviceModule->PlayoutIsInitialized() ? 1 : 0);
    });

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
        RTC_LOG(LS_ERROR) << "InstanceV4_0_0Impl MediaEngine create failed";
        OH_LOG_Print(LOG_APP, LOG_ERROR, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV4_0_0Impl MediaEngine create failed");
        return;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV4_0_0Impl MediaEngine created");

    _channelManager = ChannelManager::Create(
        std::move(mediaEngine),
        _threads->getWorkerThread(),
        _threads->getNetworkThread());
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV4_0_0Impl ChannelManager created=%{public}d",
                 _channelManager ? 1 : 0);

    _threads->getNetworkThread()->BlockingCall([this]() {
        auto *networking = _networking->getSyncAssumingSameThread();
        _rtpTransport = networking->getRtpTransport();
    });
    if (!_rtpTransport) {
        RTC_LOG(LS_ERROR) << "InstanceV4_0_0Impl rtpTransport unavailable — media pipeline aborted";
        OH_LOG_Print(LOG_APP, LOG_ERROR, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV4_0_0Impl rtpTransport unavailable — media pipeline aborted");
        return;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV4_0_0Impl rtpTransport obtained");

    _threads->getWorkerThread()->BlockingCall([this]() {
        webrtc::CallConfig callConfig(_env, _threads->getWorkerThread(), _threads->getNetworkThread());
        callConfig.audio_state = _channelManager->media_engine()->voice().GetAudioState();
        _call = webrtc::Call::Create(std::move(callConfig));
    });
    if (!_call) {
        RTC_LOG(LS_ERROR) << "InstanceV4_0_0Impl webrtc::Call::Create failed";
        OH_LOG_Print(LOG_APP, LOG_ERROR, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV4_0_0Impl webrtc::Call::Create failed");
        return;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV4_0_0Impl webrtc::Call created");

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

    // Draw the pending offer: ContentNegotiation.cpp:515 assigns the SSRC into the channel
    // description during getPendingOffer(). We need the offer's audio content for two things:
    // instantiating OutgoingAudioChannel, and — for the caller (ADR-0008) — the media block
    // of the InitialSetup we proactively send. The exchangeId is captured for the self-answer
    // below.
    auto pendingOffer = _contentNegotiationContext->getPendingOffer();
    if (pendingOffer) {
        _outgoingOfferExchangeId = pendingOffer->exchangeId;
        for (const auto &content : pendingOffer->contents) {
            if (content.type == signaling::MediaContent::Type::Audio && content.ssrc != 0) {
                _outgoingAudioSsrc = content.ssrc;
                _outgoingAudioOffer = content;
                break;
            }
        }
        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV4_0_0Impl pending offer drawn contents=%{public}zu outgoingAudioSsrc=%{public}u exchangeId=%{public}u",
                     pendingOffer->contents.size(),
                     _outgoingAudioSsrc,
                     _outgoingOfferExchangeId);
    } else {
        OH_LOG_Print(LOG_APP, LOG_WARN, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV4_0_0Impl pending offer null after addOutgoingChannel");
    }

    // ADR-0008: caller self-answers its own pending offer. In Web-K's model an InitialSetup
    // fully declares one side's media in a single message (standard WebRTC offer/answer, see
    // tweb callConnectionInstance.ts) — there is no separate round that answers our outgoing
    // media. Feeding our own offer back as the answer finalises our outgoing-media
    // negotiation locally (populates ContentNegotiationContext::_outgoingChannels by SSRC
    // match) and clears _pendingOutgoingOffer. With no pending offer left, the callee's
    // InitialSetup is then processed through the same getAnswer path the incoming-call
    // (callee) flow already uses — receiveSignalingData stays symmetric for both directions.
    if (_encryptionKeyIsOutgoing && _outgoingAudioOffer.has_value()) {
        auto selfAnswer = std::make_unique<ContentNegotiationContext::NegotiationContents>();
        selfAnswer->exchangeId = _outgoingOfferExchangeId;
        selfAnswer->contents.push_back(_outgoingAudioOffer.value());
        _contentNegotiationContext->setRemoteNegotiationContent(std::move(selfAnswer));
        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV4_0_0Impl caller self-answered pending offer exchangeId=%{public}u",
                     _outgoingOfferExchangeId);
    }

    _mediaPipelineReady = true;
    RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl media pipeline ready version=" << _version
                     << " outgoingChannelId=" << _outgoingAudioChannelId.value_or("")
                     << " adm=" << (_audioDeviceModule ? 1 : 0)
                     << " call=" << (_call ? 1 : 0);
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV4_0_0Impl media pipeline ready version=%{public}s outgoingChannelId=%{public}s",
                 _version.c_str(), _outgoingAudioChannelId.value_or("").c_str());
}

void InstanceV4_0_0ImplInternal::createNegotiatedChannels() {
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV4_0_0Impl createNegotiatedChannels mediaReady=%{public}d",
                 _mediaPipelineReady ? 1 : 0);
    if (!_mediaPipelineReady) {
        return;
    }
    const auto coordinatedState = _contentNegotiationContext->coordinatedState();
    if (!coordinatedState) {
        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV4_0_0Impl coordinatedState absent");
        return;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV4_0_0Impl coordinatedState outgoing=%{public}zu incoming=%{public}zu",
                 coordinatedState->outgoingContents.size(), coordinatedState->incomingContents.size());

    // First materialise incoming audio channel from CNC coordinated state — this is where
    // Web Telegram's outgoing audio SSRC + codec params live.
    for (const auto &content : coordinatedState->incomingContents) {
        if (content.type != signaling::MediaContent::Type::Audio) {
            continue;
        }
        if (_incomingAudioChannel && _incomingAudioChannel->ssrc() != content.ssrc) {
            // Renegotiation: destroy the previous incoming channel on the worker thread per
            // webrtc::ChannelManager docs. Resetting from the signaling thread crashed the
            // destructor's Enable(false)/DestroyChannel path (cppcrash 2026-05-12 16:42).
            _threads->getWorkerThread()->BlockingCall([this]() {
                _incomingAudioChannel.reset();
            });
        }
        if (!_incomingAudioChannel) {
            _threads->getWorkerThread()->BlockingCall([this, &content]() {
                _incomingAudioChannel = std::make_unique<IncomingAudioChannel_4_0_0>(
                    _channelManager.get(),
                    _call.get(),
                    _rtpTransport,
                    content,
                    _threads);
            });
            if (_incomingAudioChannel && !_incomingAudioChannel->isReady()) {
                OH_LOG_Print(LOG_APP, LOG_ERROR, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                             "InstanceV4_0_0Impl incoming channel inner VoiceChannel null — dropping wrapper ssrc=%{public}u",
                             content.ssrc);
                _threads->getWorkerThread()->BlockingCall([this]() {
                    _incomingAudioChannel.reset();
                });
            } else {
                RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl incoming audio channel created version="
                                 << _version << " ssrc=" << content.ssrc;
                OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                             "InstanceV4_0_0Impl incoming audio channel created ssrc=%{public}u",
                             content.ssrc);
            }
        }
        break;  // M1 only handles one audio stream
    }

    // Outgoing audio channel must be created — without it Web Telegram hangs up after ~4s
    // because no RTP (not even silence keepalive) is reaching them. Smoke 2026-05-12 16:57
    // confirmed this: deferring construction kept call alive only 4s; smoke 2026-05-12 16:33
    // with construction kept it 32s. So construct it, but on the worker thread per
    // webrtc::ChannelManager docs ("operations all occur on the worker thread"). The crash
    // in smoke 2026-05-12 16:42 was the destructor running on the wrong thread.
    if (!_outgoingAudioChannel && _outgoingAudioOffer.has_value()) {
        _threads->getWorkerThread()->BlockingCall([this]() {
            _outgoingAudioChannel = std::make_unique<OutgoingAudioChannel_4_0_0>(
                _call.get(),
                _channelManager.get(),
                &_audioSource,
                _rtpTransport,
                _outgoingAudioOffer.value(),
                _threads);
        });
        if (_outgoingAudioChannel && !_outgoingAudioChannel->isReady()) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "InstanceV4_0_0Impl outgoing channel inner VoiceChannel null — dropping wrapper ssrc=%{public}u",
                         _outgoingAudioSsrc);
            _threads->getWorkerThread()->BlockingCall([this]() {
                _outgoingAudioChannel.reset();
            });
        } else if (_outgoingAudioChannel) {
            _outgoingAudioChannel->setIsMuted(_muteMicrophone);
            RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl outgoing audio channel created version="
                             << _version << " ssrc=" << _outgoingAudioSsrc;
            OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "InstanceV4_0_0Impl outgoing audio channel created ssrc=%{public}u",
                         _outgoingAudioSsrc);
            // M2.1 diagnostic: did SetAudioSend(true) propagate down to ADM->StartRecording()?
            // scoped_refptr by-value capture keeps ADM alive even if the instance shuts down
            // before the delayed task fires. webrtc::Call posts the start asynchronously, so a
            // single 2s deferred check is enough — Recording() is a sync flag (ohaudio_recorder.cc:96).
            auto admForLog = _audioDeviceModule;
            _threads->getWorkerThread()->PostDelayedTask([admForLog]() {
                if (!admForLog) {
                    return;
                }
                OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                             "InstanceV4_0_0Impl ADM check t+2s Recording=%{public}d Playing=%{public}d initialized=%{public}d playoutInitialized=%{public}d",
                             admForLog->Recording() ? 1 : 0,
                             admForLog->Playing() ? 1 : 0,
                             admForLog->RecordingIsInitialized() ? 1 : 0,
                             admForLog->PlayoutIsInitialized() ? 1 : 0);
            }, webrtc::TimeDelta::Millis(2000));

            // M2.2 diagnostic: are outgoing RTP packets actually being produced by the encoder?
            // _diagAlive is checked first because _call and _outgoingAudioChannel are unique_ptrs
            // owned by the instance — the alive flag is set false on the worker thread before
            // those resets, so reads here are safe (same-thread sequencing).
            auto alive = _diagAlive;
            const uint32_t ssrcSnapshot = _outgoingAudioSsrc;
            const auto logSendStats = [this, alive, ssrcSnapshot](int tSeconds) {
                if (!alive->load()) {
                    return;
                }
                webrtc::Call::Stats callStats = _call ? _call->GetStats() : webrtc::Call::Stats{};
                webrtc::VoiceMediaSendInfo sendInfo;
                bool sendOk = _outgoingAudioChannel && _outgoingAudioChannel->getSendStats(&sendInfo);
                int64_t payloadBytes = 0;
                int packetsSent = 0;
                int packetsLost = 0;
                int64_t rttMs = 0;
                double targetBitrate = 0.0;
                bool matched = false;
                if (sendOk) {
                    for (const auto &sender : sendInfo.senders) {
                        if (sender.ssrc() == ssrcSnapshot || sender.local_stats.empty()) {
                            payloadBytes = sender.payload_bytes_sent;
                            packetsSent = sender.packets_sent;
                            packetsLost = sender.packets_lost;
                            rttMs = sender.rtt_ms;
                            targetBitrate = sender.target_bitrate.has_value() ? sender.target_bitrate->bps() : 0.0;
                            matched = true;
                            break;
                        }
                    }
                }
                OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                             "InstanceV4_0_0Impl outgoing send stats t+%{public}ds sendOk=%{public}d senders=%{public}zu matched=%{public}d ssrc=%{public}u packets=%{public}d bytes=%{public}lld lost=%{public}d rttMs=%{public}lld targetBps=%{public}d sendBwBps=%{public}d recvBwBps=%{public}d pacerMs=%{public}lld callRttMs=%{public}lld",
                             tSeconds,
                             sendOk ? 1 : 0,
                             sendOk ? sendInfo.senders.size() : 0,
                             matched ? 1 : 0,
                             ssrcSnapshot,
                             packetsSent,
                             (long long)payloadBytes,
                             packetsLost,
                             (long long)rttMs,
                             (int)targetBitrate,
                             callStats.send_bandwidth_bps,
                             callStats.recv_bandwidth_bps,
                             (long long)callStats.pacer_delay_ms,
                             (long long)callStats.rtt_ms);
            };
            _threads->getWorkerThread()->PostDelayedTask([logSendStats]() { logSendStats(5); },
                                                         webrtc::TimeDelta::Millis(5000));
            _threads->getWorkerThread()->PostDelayedTask([logSendStats]() { logSendStats(10); },
                                                         webrtc::TimeDelta::Millis(10000));

            // M2.4.2: periodic outgoing-microphone level, polled from VoiceSenderInfo on the
            // worker thread and pushed out via _audioLevelUpdated for the in-call UI bars.
            // Same _diagAlive guard + same-thread sequencing as logSendStats above. The task
            // re-posts itself, but captures a weak_ptr to _audioLevelTask (not a shared_ptr)
            // to avoid a self-reference cycle — the strong ref lives in the member and is
            // reset on the worker thread during teardown, which stops the chain.
            if (_audioLevelUpdated) {
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
                            // VoiceSenderInfo::audio_level is a linear [0, 32767] peak. Speech
                            // peaks land around 2000-6000, so dividing by 32767 leaves the UI
                            // bars almost always dark. Upstream tgcalls treats peak / 8000 as
                            // "full" (MediaManager.cpp AudioCapturePostProcessor) — match that.
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

void InstanceV4_0_0ImplInternal::shutdownInstance() {
    // Defensive shutdown ordering for callback-driven systems. Each step closes a
    // distinct race window — see the comment in stop() for the rationale. This helper
    // is idempotent: calling it twice (e.g. from stop() then from the destructor) is
    // safe because every step short-circuits when the relevant member is already null.
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

void InstanceV4_0_0ImplInternal::teardownMediaPipelineOnWorkerThread() {
    if (!_threads) {
        return;
    }
    // webrtc::ChannelManager docs: "operations all occur on the worker thread".
    // Channel destructors call Enable(false), DestroyChannel and SetRtpTransport(nullptr);
    // the last one dereferences the underlying RtpTransport owned by NativeNetworkingImpl,
    // so this MUST run before _networking is reset (smoke crash 2026-05-13 11:24:44 tgc-net
    // ran the SetRtpTransport lambda after _networking had already freed the transport).
    _threads->getWorkerThread()->BlockingCall([this]() {
        _diagAlive->store(false);
        // M2.4.2: drop the strong ref so the periodic audio-level task chain unwinds
        // (a pending copy still runs once, but weakTask.lock() then fails -> no re-post).
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

bool InstanceV4_0_0ImplInternal::encryptWebKMessage(
    const tgcalls::signaling_4_0_0::Message &message,
    uint32_t &sequence,
    std::vector<uint8_t> &encryptedPayload) {
    if (!_encryptionKey) {
        return false;
    }

    const std::vector<uint8_t> serializedPayload = message.serialize();
    {
        std::lock_guard<std::mutex> lock(_signalingMutex);
        sequence = _outgoingSequence;
        _outgoingSequence += 1;
        encryptedPayload = EncryptRawSignalingPacket(
            serializedPayload,
            *_encryptionKey,
            _encryptionKeyIsOutgoing,
            sequence);
    }
    return true;
}

void InstanceV4_0_0ImplInternal::configureRemoteInitialSetup(
    const tgcalls::signaling_4_0_0::InitialSetupMessage &remoteInitialSetup) {
    if (!_networking) {
        return;
    }

    RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl configuring remote setup version=" << _version
                     << " ufragLen=" << remoteInitialSetup.ufrag.size()
                     << " pwdLen=" << remoteInitialSetup.pwd.size()
                     << " fingerprints=" << remoteInitialSetup.fingerprints.size();
    const bool isCallee = !_encryptionKeyIsOutgoing;
    _networking->perform([remoteInitialSetup, isCallee](tgcalls::InstanceNetworking *networking) {
        tgcalls::PeerIceParameters remoteIce(
            remoteInitialSetup.ufrag,
            remoteInitialSetup.pwd,
            false);

        std::unique_ptr<webrtc::SSLFingerprint> remoteFingerprint;
        std::string sslSetup;
        if (!remoteInitialSetup.fingerprints.empty()) {
            const tgcalls::signaling_4_0_0::DtlsFingerprint &fingerprint =
                remoteInitialSetup.fingerprints.front();
            remoteFingerprint = webrtc::SSLFingerprint::CreateUniqueFromRfc4572(
                fingerprint.hash,
                fingerprint.fingerprint);
            sslSetup = fingerprint.setup;
        }

        // tgcalls::NativeNetworkingImpl::setRemoteParams falls back to
        // (_isOutgoing ? SSL_CLIENT : SSL_SERVER) when the remote announces "actpass". That
        // would make us a DTLS server while we tell the peer setup=active, so both ends wait
        // for ClientHello and DTLS never starts (ready stays 0 after ICE connected). Override
        // the role here by claiming the remote is "passive" so tgcalls picks SSL_CLIENT and
        // actually sends ClientHello, consistent with the setup=active we emit upstream.
        if (isCallee && sslSetup == "actpass") {
            sslSetup = "passive";
        }

        networking->setRemoteParams(remoteIce, remoteFingerprint.get(), sslSetup);
    });
}

void InstanceV4_0_0ImplInternal::addRemoteCandidates(
    const tgcalls::signaling_4_0_0::CandidatesMessage &remoteCandidates) {
    if (!_networking) {
        return;
    }

    _networking->perform([remoteCandidates](tgcalls::InstanceNetworking *networking) {
        std::vector<webrtc::Candidate> parsedCandidates;
        parsedCandidates.reserve(remoteCandidates.iceCandidates.size());
        for (const tgcalls::signaling_4_0_0::IceCandidate &candidate : remoteCandidates.iceCandidates) {
            std::unique_ptr<webrtc::IceCandidate> parsedCandidate = webrtc::IceCandidate::Create(
                std::string(), 0, candidate.sdpString, nullptr);
            if (!parsedCandidate) {
                RTC_LOG(LS_WARNING) << "InstanceV4_0_0Impl remote candidate parse failed size="
                                    << candidate.sdpString.size();
                continue;
            }
            RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl remote candidate parsed protocol="
                             << parsedCandidate->candidate().protocol()
                             << " type=" << parsedCandidate->candidate().type()
                             << " relay=" << parsedCandidate->candidate().relay_protocol()
                             << " address=" << parsedCandidate->candidate().address().ToString()
                             << " related=" << parsedCandidate->candidate().related_address().ToString();
            parsedCandidates.push_back(parsedCandidate->candidate());
        }
        if (!parsedCandidates.empty()) {
            networking->addCandidates(parsedCandidates);
        }
    });
}

void InstanceV4_0_0ImplInternal::emitLocalInitialSetup(
    const tgcalls::signaling_4_0_0::InitialSetupMessage &remoteInitialSetup,
    const tgcalls::signaling::NegotiateChannelsMessage *answerNegotiation) {
    if (!_networking || !_signalingDataEmitted) {
        return;
    }

    // Capture answer by value so the network-thread lambda can outlive the caller's
    // unique_ptr returned from ContentNegotiationContext::setRemoteNegotiationContent.
    absl::optional<tgcalls::signaling::NegotiateChannelsMessage> answerCopy;
    if (answerNegotiation != nullptr) {
        answerCopy = *answerNegotiation;
    }

    _networking->perform([this, remoteInitialSetup, answerCopy = std::move(answerCopy)](tgcalls::InstanceNetworking *networking) {
        tgcalls::signaling_4_0_0::InitialSetupMessage localInitialSetup;
        const tgcalls::PeerIceParameters localIce = networking->getLocalIceParameters();
        std::unique_ptr<webrtc::SSLFingerprint> localFingerprint = networking->getLocalFingerprint();
        if (!localFingerprint) {
            RTC_LOG(LS_WARNING) << "InstanceV4_0_0Impl initial setup emit skipped version="
                                << _version << " reason=noFingerprint";
            OH_LOG_Print(LOG_APP, LOG_WARN, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "InstanceV4_0_0Impl initial setup emit skipped reason=noFingerprint");
            return;
        }

        // Build with BuildLocalInitialSetup (echo remote structure with our random SSRCs)
        // because Web-K's parser is strict: an emit that drops/reorders payloadTypes / rtpExtensions
        // / ssrcGroups causes Web Telegram to silently reject the message and ICE hangs in
        // "checking" forever (smoke 2026-05-12 16:16 reproduced this). After the structural
        // echo, splice in OUR outgoing audio SSRC from the ContentNegotiationContext answer so
        // Web-K's media stack knows which SSRC to expect for our outgoing RTP.
        if (!BuildLocalInitialSetup(
                remoteInitialSetup,
                localIce,
                *localFingerprint,
                _encryptionKeyIsOutgoing,
                localInitialSetup)) {
            RTC_LOG(LS_WARNING) << "InstanceV4_0_0Impl initial setup emit skipped version="
                                << _version << " reason=buildFailed";
            OH_LOG_Print(LOG_APP, LOG_WARN, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "InstanceV4_0_0Impl initial setup build failed");
            return;
        }

        uint32_t splicedAudioSsrc = _outgoingAudioSsrc;
        if (splicedAudioSsrc != 0 && localInitialSetup.audio.has_value()) {
            // The CNC answer's contents carry *remote* (incoming-from-our-view) SSRCs after
            // setRemoteNegotiationContent, which is the WRONG side. The watch's outgoing SSRC
            // lives behind ContentNegotiationContext::outgoingChannelSsrc(_outgoingAudioChannelId)
            // and is captured into _outgoingAudioSsrc when the channel id is resolved (see
            // createNegotiatedChannels). Splice it into our InitialSetup so Web Telegram knows
            // which SSRC to expect for our outgoing RTP.
            localInitialSetup.audio->ssrc = splicedAudioSsrc;
            for (auto &group : localInitialSetup.audio->ssrcGroups) {
                for (auto &ssrc : group.ssrcs) {
                    ssrc = splicedAudioSsrc;
                }
            }
        }

        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV4_0_0Impl initial setup emit hasAnswer=%{public}d splicedAudioSsrc=%{public}u audio=%{public}d video=%{public}d screencast=%{public}d",
                     answerCopy.has_value() ? 1 : 0,
                     splicedAudioSsrc,
                     localInitialSetup.audio.has_value() ? 1 : 0,
                     localInitialSetup.video.has_value() ? 1 : 0,
                     localInitialSetup.screencast.has_value() ? 1 : 0);

        tgcalls::signaling_4_0_0::Message outboundInitialSetup;
        outboundInitialSetup.data = localInitialSetup;
        uint32_t outboundSequence = 0;
        std::vector<uint8_t> encryptedPayload;
        if (!encryptWebKMessage(outboundInitialSetup, outboundSequence, encryptedPayload)) {
            return;
        }

        RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl emitting initial setup version=" << _version
                         << " seq=" << outboundSequence
                         << " payload=" << outboundInitialSetup.serialize().size()
                         << " encrypted=" << encryptedPayload.size()
                         << " setup=" << (localInitialSetup.fingerprints.empty()
                                              ? ""
                                              : localInitialSetup.fingerprints.front().setup.c_str())
                         << " audio=" << (localInitialSetup.audio.has_value() ? 1 : 0)
                         << " video=" << (localInitialSetup.video.has_value() ? 1 : 0)
                         << " screencast=" << (localInitialSetup.screencast.has_value() ? 1 : 0);
        _signalingDataEmitted(encryptedPayload);
        {
            std::lock_guard<std::mutex> lock(_signalingMutex);
            _initialSetupEmitted = true;
        }

        bool shouldStartNetworking = false;
        {
            std::lock_guard<std::mutex> lock(_signalingMutex);
            if (!_networkingStarted) {
                _networkingStarted = true;
                shouldStartNetworking = true;
            }
        }
        if (shouldStartNetworking) {
            RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl networking start version=" << _version;
            networking->start();
        }
        emitMediaState();
    });
}

void InstanceV4_0_0ImplInternal::emitCallerInitialSetup() {
    if (!_networking || !_signalingDataEmitted) {
        return;
    }
    if (!_outgoingAudioOffer.has_value()) {
        OH_LOG_Print(LOG_APP, LOG_WARN, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV4_0_0Impl caller initial setup skipped reason=noPendingOffer");
        return;
    }

    // Capture the offer by value — the network-thread lambda must not race the worker
    // thread that could tear down _outgoingAudioOffer during teardown.
    const tgcalls::signaling::MediaContent audioOffer = _outgoingAudioOffer.value();
    const uint32_t exchangeId = _outgoingOfferExchangeId;

    _networking->perform([this, audioOffer, exchangeId](tgcalls::InstanceNetworking *networking) {
        const tgcalls::PeerIceParameters localIce = networking->getLocalIceParameters();
        std::unique_ptr<webrtc::SSLFingerprint> localFingerprint = networking->getLocalFingerprint();
        if (!localFingerprint) {
            RTC_LOG(LS_WARNING) << "InstanceV4_0_0Impl caller initial setup skipped version="
                                << _version << " reason=noFingerprint";
            OH_LOG_Print(LOG_APP, LOG_WARN, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "InstanceV4_0_0Impl caller initial setup skipped reason=noFingerprint");
            return;
        }

        // Build the InitialSetup from scratch (the caller has no remote message to echo):
        // local ICE params + local fingerprint with setup=actpass (same as upstream v2's
        // InstanceV2Impl::sendInitialSetup for the outgoing side), plus our own pending
        // audio offer as the media content. MapV2InitialSetupToWebK assembles the Web-K
        // signaling_4_0_0::InitialSetupMessage from the v2-shaped pieces.
        tgcalls::signaling::InitialSetupMessage v2InitialSetup;
        v2InitialSetup.ufrag = localIce.ufrag;
        v2InitialSetup.pwd = localIce.pwd;
        v2InitialSetup.supportsRenomination = false;

        tgcalls::signaling::DtlsFingerprint v2Fingerprint;
        v2Fingerprint.hash = localFingerprint->algorithm;
        v2Fingerprint.setup = "actpass";
        v2Fingerprint.fingerprint = localFingerprint->GetRfc4572Fingerprint();
        v2InitialSetup.fingerprints.push_back(std::move(v2Fingerprint));

        tgcalls::signaling::NegotiateChannelsMessage v2Negotiation;
        v2Negotiation.exchangeId = exchangeId;
        v2Negotiation.contents.push_back(audioOffer);

        tgcalls::signaling_4_0_0::InitialSetupMessage localInitialSetup =
            tgcall::MapV2InitialSetupToWebK(v2InitialSetup, v2Negotiation);

        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV4_0_0Impl caller initial setup emit exchangeId=%{public}u audio=%{public}d",
                     exchangeId,
                     localInitialSetup.audio.has_value() ? 1 : 0);

        tgcalls::signaling_4_0_0::Message outboundInitialSetup;
        outboundInitialSetup.data = localInitialSetup;
        uint32_t outboundSequence = 0;
        std::vector<uint8_t> encryptedPayload;
        if (!encryptWebKMessage(outboundInitialSetup, outboundSequence, encryptedPayload)) {
            return;
        }

        RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl emitting caller initial setup version=" << _version
                         << " seq=" << outboundSequence
                         << " encrypted=" << encryptedPayload.size()
                         << " setup=" << (localInitialSetup.fingerprints.empty()
                                              ? ""
                                              : localInitialSetup.fingerprints.front().setup.c_str())
                         << " audio=" << (localInitialSetup.audio.has_value() ? 1 : 0);
        _signalingDataEmitted(encryptedPayload);
        {
            std::lock_guard<std::mutex> lock(_signalingMutex);
            _initialSetupEmitted = true;
        }

        bool shouldStartNetworking = false;
        {
            std::lock_guard<std::mutex> lock(_signalingMutex);
            if (!_networkingStarted) {
                _networkingStarted = true;
                shouldStartNetworking = true;
            }
        }
        if (shouldStartNetworking) {
            RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl networking start version=" << _version << " role=caller";
            networking->start();
        }
        emitMediaState();
    });
}

void InstanceV4_0_0ImplInternal::emitLocalCandidate(const webrtc::Candidate &candidate) {
    if (!_signalingDataEmitted) {
        return;
    }

    webrtc::Candidate patchedCandidate = candidate;
    patchedCandidate.set_component(1);

    if (patchedCandidate.protocol() != webrtc::UDP_PROTOCOL_NAME) {
        RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl local candidate skipped version=" << _version
                         << " protocol=" << patchedCandidate.protocol()
                         << " type=" << patchedCandidate.type()
                         << " address=" << patchedCandidate.address().ToString();
        return;
    }

    webrtc::SocketAddress candidateAddress = patchedCandidate.address();
    const std::string originalCandidateAddress = candidateAddress.ToString();
    if (RewriteReflectorCandidateAddressForWebK(candidateAddress, _rtcServers)) {
        patchedCandidate.set_address(candidateAddress);
        RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl local reflector candidate rewritten version="
                         << _version << " from=" << originalCandidateAddress
                         << " to=" << candidateAddress.ToString();
    }
    webrtc::SocketAddress relatedCandidateAddress = patchedCandidate.related_address();
    const std::string originalRelatedCandidateAddress = relatedCandidateAddress.ToString();
    if (RewriteReflectorCandidateAddressForWebK(relatedCandidateAddress, _rtcServers)) {
        patchedCandidate.set_related_address(relatedCandidateAddress);
        RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl local reflector related candidate rewritten version="
                         << _version << " from=" << originalRelatedCandidateAddress
                         << " to=" << relatedCandidateAddress.ToString();
    }

    webrtc::JsepIceCandidate iceCandidate("", 0, patchedCandidate);
    std::string serializedCandidate;
    if (!iceCandidate.ToString(&serializedCandidate)) {
        RTC_LOG(LS_WARNING) << "InstanceV4_0_0Impl local candidate serialize failed";
        return;
    }
    NormalizeWebKCandidateSdp(serializedCandidate);

    tgcalls::signaling::CandidatesMessage v2Candidates;
    tgcalls::signaling::IceCandidate v2Candidate;
    v2Candidate.sdpString = serializedCandidate;
    v2Candidates.iceCandidates.push_back(std::move(v2Candidate));

    tgcalls::signaling_4_0_0::Message outboundMessage;
    outboundMessage.data = tgcall::MapV2CandidatesToWebK(v2Candidates);
    uint32_t outboundSequence = 0;
    std::vector<uint8_t> encryptedPayload;
    if (!encryptWebKMessage(outboundMessage, outboundSequence, encryptedPayload)) {
        return;
    }

    RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl emitting candidate version=" << _version
                     << " seq=" << outboundSequence
                     << " protocol=" << patchedCandidate.protocol()
                     << " type=" << patchedCandidate.type()
                     << " address=" << patchedCandidate.address().ToString()
                     << " encrypted=" << encryptedPayload.size();
    _signalingDataEmitted(encryptedPayload);
}

void InstanceV4_0_0ImplInternal::emitMediaState() {
    if (!_signalingDataEmitted) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(_signalingMutex);
        if (_mediaStateEmitted) {
            return;
        }
        _mediaStateEmitted = true;
    }

    tgcalls::signaling_4_0_0::MediaStateMessage mediaState;
    mediaState.isMuted = _muteMicrophone;
    mediaState.videoState = tgcalls::signaling_4_0_0::MediaStateMessage::VideoState::Inactive;
    mediaState.videoRotation = tgcalls::signaling_4_0_0::MediaStateMessage::VideoRotation::Rotation0;
    mediaState.screencastState = tgcalls::signaling_4_0_0::MediaStateMessage::VideoState::Inactive;
    mediaState.isBatteryLow = _isLowBatteryLevel;

    tgcalls::signaling_4_0_0::Message outboundMessage;
    outboundMessage.data = mediaState;
    uint32_t outboundSequence = 0;
    std::vector<uint8_t> encryptedPayload;
    if (!encryptWebKMessage(outboundMessage, outboundSequence, encryptedPayload)) {
        return;
    }

    RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl emitting media state version=" << _version
                     << " seq=" << outboundSequence
                     << " encrypted=" << encryptedPayload.size()
                     << " muted=" << (_muteMicrophone ? 1 : 0)
                     << " lowBattery=" << (_isLowBatteryLevel ? 1 : 0);
    _signalingDataEmitted(encryptedPayload);
}

void InstanceV4_0_0ImplInternal::onTransportMessageReceived(
    const webrtc::CopyOnWriteBuffer &packet,
    bool isUnresolved) {
    // RTP packets are demuxed by NativeNetworkingImpl's RtpTransport directly into the
    // VoiceChannel registered with it (see IncomingAudioChannel_4_0_0). This callback is
    // diagnostic-only — `isUnresolved` should drop to 0 once a receive channel exists.
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
        RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl media ingress RTP version=" << _version
                         << " packets=" << packetCount
                         << " bytes=" << byteCount
                         << " lastBytes=" << packet.size()
                         << " unresolved=" << unresolvedCount;
    }
}

void InstanceV4_0_0ImplInternal::onRtcpPacketReceived(
    const webrtc::CopyOnWriteBuffer &packet,
    int64_t packetTimeUs) {
    size_t packetCount = 0;
    size_t byteCount = 0;
    {
        std::lock_guard<std::mutex> lock(_mediaDiagnosticsMutex);
        _incomingRtcpPacketCount += 1;
        _incomingRtcpByteCount += packet.size();
        packetCount = _incomingRtcpPacketCount;
        byteCount = _incomingRtcpByteCount;
    }

    if (ShouldLogMediaPacketCount(packetCount)) {
        RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl media ingress RTCP version=" << _version
                         << " packets=" << packetCount
                         << " bytes=" << byteCount
                         << " lastBytes=" << packet.size()
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

void InstanceV4_0_0ImplInternal::onDataChannelStateUpdated(bool isOpen) {
    {
        std::lock_guard<std::mutex> lock(_mediaDiagnosticsMutex);
        _dataChannelOpen = isOpen;
    }
    RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl data channel state version=" << _version
                     << " open=" << (isOpen ? 1 : 0);
}

void InstanceV4_0_0ImplInternal::onDataChannelMessageReceived(const std::string &message) {
    size_t messageCount = 0;
    size_t byteCount = 0;
    {
        std::lock_guard<std::mutex> lock(_mediaDiagnosticsMutex);
        _dataChannelMessageCount += 1;
        _dataChannelMessageByteCount += message.size();
        messageCount = _dataChannelMessageCount;
        byteCount = _dataChannelMessageByteCount;
    }
    RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl data channel message version=" << _version
                     << " messages=" << messageCount
                     << " bytes=" << byteCount
                     << " lastBytes=" << message.size();
}

void InstanceV4_0_0ImplInternal::setNetworkType(tgcalls::NetworkType networkType) {
    _networkType = networkType;
}

void InstanceV4_0_0ImplInternal::setMuteMicrophone(bool muteMicrophone) {
    _muteMicrophone = muteMicrophone;
    if (_outgoingAudioChannel) {
        _outgoingAudioChannel->setIsMuted(muteMicrophone);
    }
}

void InstanceV4_0_0ImplInternal::setIncomingVideoOutput(
    std::weak_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
    _incomingVideoOutput = std::move(sink);
}

void InstanceV4_0_0ImplInternal::setAudioInputDevice(std::string id) {
    _audioInputDeviceId = std::move(id);
}

void InstanceV4_0_0ImplInternal::setAudioOutputDevice(std::string id) {
    _audioOutputDeviceId = std::move(id);
}

void InstanceV4_0_0ImplInternal::setIsLowBatteryLevel(bool isLowBatteryLevel) {
    _isLowBatteryLevel = isLowBatteryLevel;
}

void InstanceV4_0_0ImplInternal::setVideoCapture(
    std::shared_ptr<tgcalls::VideoCaptureInterface> videoCapture) {
    _videoCapture = std::move(videoCapture);
}

void InstanceV4_0_0ImplInternal::setRequestedVideoAspect(float aspect) {
    _requestedVideoAspect = aspect;
}

void InstanceV4_0_0ImplInternal::receiveSignalingData(const std::vector<uint8_t> &data) {
    _signalingPacketCount += 1;
    _signalingByteCount += data.size();
    RTC_LOG(LS_VERBOSE) << "InstanceV4_0_0Impl received signaling version=" << _version
                        << " packets=" << _signalingPacketCount
                        << " bytes=" << _signalingByteCount;
    if (!_encryptionKey) {
        RTC_LOG(LS_WARNING) << "InstanceV4_0_0Impl signaling parse skipped reason=missingKey packet="
                            << _signalingPacketCount;
        return;
    }

    std::vector<uint8_t> decryptedPayload;
    uint32_t sequence = 0;
    if (!DecryptRawSignalingPacket(data, *_encryptionKey, _encryptionKeyIsOutgoing, decryptedPayload, sequence)) {
        RTC_LOG(LS_WARNING) << "InstanceV4_0_0Impl signaling decrypt failed packet="
                            << _signalingPacketCount
                            << " encrypted=" << data.size()
                            << " keyOutgoing=" << (_encryptionKeyIsOutgoing ? 1 : 0);
        return;
    }

    const absl::optional<tgcalls::signaling_4_0_0::Message> parsedMessage =
        tgcalls::signaling_4_0_0::Message::parse(decryptedPayload);
    if (!parsedMessage.has_value()) {
        RTC_LOG(LS_WARNING) << "InstanceV4_0_0Impl signaling parse failed packet="
                            << _signalingPacketCount
                            << " seq=" << sequence
                            << " decrypted=" << decryptedPayload.size();
        return;
    }

    RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl signaling parsed version=" << _version
                     << " packet=" << _signalingPacketCount
                     << " seq=" << sequence
                     << " type=" << WebKMessageTypeName(parsedMessage.value())
                     << " encrypted=" << data.size()
                     << " decrypted=" << decryptedPayload.size();

    const tgcalls::signaling_4_0_0::InitialSetupMessage *initialSetup =
        absl::get_if<tgcalls::signaling_4_0_0::InitialSetupMessage>(&parsedMessage.value().data);
    if (initialSetup != nullptr) {
        configureRemoteInitialSetup(*initialSetup);
        bool shouldEmitInitialSetup = false;
        bool shouldEmitMediaState = false;
        {
            std::lock_guard<std::mutex> lock(_signalingMutex);
            if (!_initialSetupEmitScheduled) {
                // Mark before scheduling the network-thread emit to dedupe repeated InitialSetup packets.
                _initialSetupEmitScheduled = true;
                shouldEmitInitialSetup = true;
            } else {
                shouldEmitMediaState = _initialSetupEmitted;
            }
        }

        // Bridge Web-K's single-round InitialSetupMessage into V2-style content negotiation:
        // extract audio/video/screencast media descriptions, feed them to ContentNegotiationContext
        // as a remote offer, capture our answer (with OUR outgoing SSRC), use that answer to build
        // the Web-K InitialSetup we send back. createNegotiatedChannels() then materialises both
        // IncomingAudioChannel_4_0_0 and OutgoingAudioChannel_4_0_0 from coordinatedState.
        OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                     "InstanceV4_0_0Impl initial setup received mediaReady=%{public}d ctx=%{public}d audio=%{public}d video=%{public}d",
                     _mediaPipelineReady ? 1 : 0,
                     _contentNegotiationContext ? 1 : 0,
                     initialSetup->audio.has_value() ? 1 : 0,
                     initialSetup->video.has_value() ? 1 : 0);

        std::unique_ptr<ContentNegotiationContext::NegotiationContents> answer;
        if (_mediaPipelineReady && _contentNegotiationContext) {
            // ADR-0008: both directions reach this point with no pending outgoing offer
            // (the caller neutralised it via the self-answer in setUpMediaPipeline; the
            // callee never matched its own). So setRemoteNegotiationContent takes the
            // getAnswer path for both, and the synthesised exchangeId is irrelevant —
            // Web-K's wire format carries none, so the packet sequence is as good as any.
            const uint32_t exchangeId = sequence;
            const tgcall::WebKToV2InitialSetupMapping mapping =
                tgcall::MapWebKInitialSetupToV2(*initialSetup, exchangeId);
            OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                         "InstanceV4_0_0Impl Web-K mapping audio=%{public}d video=%{public}d contents=%{public}zu",
                         mapping.hasAudio ? 1 : 0,
                         mapping.hasVideo ? 1 : 0,
                         mapping.negotiation.contents.size());
            if (mapping.hasAudio || mapping.hasVideo || mapping.hasScreencast) {
                auto remoteContent = std::make_unique<ContentNegotiationContext::NegotiationContents>();
                remoteContent->exchangeId = mapping.negotiation.exchangeId;
                remoteContent->contents = mapping.negotiation.contents;
                RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl content negotiation remote version="
                                 << _version << " exchangeId=" << remoteContent->exchangeId
                                 << " contents=" << remoteContent->contents.size()
                                 << " hasAudio=" << (mapping.hasAudio ? 1 : 0);
                answer = _contentNegotiationContext->setRemoteNegotiationContent(std::move(remoteContent));
                OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                             "InstanceV4_0_0Impl CNC answer present=%{public}d contents=%{public}zu",
                             answer ? 1 : 0,
                             answer ? answer->contents.size() : static_cast<size_t>(0));
                createNegotiatedChannels();
            }
        }

        // Build outbound InitialSetup AFTER ContentNegotiationContext has produced our answer —
        // so that the emit carries our outgoing SSRC, not a meaningless echo of remote contents.
        tgcalls::signaling::NegotiateChannelsMessage negotiationForEmit;
        const tgcalls::signaling::NegotiateChannelsMessage *negotiationForEmitPtr = nullptr;
        if (answer) {
            negotiationForEmit.exchangeId = answer->exchangeId;
            negotiationForEmit.contents = answer->contents;
            negotiationForEmitPtr = &negotiationForEmit;
        }
        if (shouldEmitInitialSetup) {
            emitLocalInitialSetup(*initialSetup, negotiationForEmitPtr);
        } else if (shouldEmitMediaState) {
            emitMediaState();
        }
        return;
    }

    const tgcalls::signaling_4_0_0::CandidatesMessage *candidates =
        absl::get_if<tgcalls::signaling_4_0_0::CandidatesMessage>(&parsedMessage.value().data);
    if (candidates != nullptr) {
        addRemoteCandidates(*candidates);
    }
}

void InstanceV4_0_0ImplInternal::stop(std::function<void(FinalState)> completion) {
    size_t incomingRtpPacketCount = 0;
    size_t incomingRtpByteCount = 0;
    size_t unresolvedRtpPacketCount = 0;
    size_t incomingRtcpPacketCount = 0;
    size_t incomingRtcpByteCount = 0;
    size_t dataChannelMessageCount = 0;
    size_t dataChannelMessageByteCount = 0;
    bool dataChannelOpen = false;
    {
        std::lock_guard<std::mutex> lock(_mediaDiagnosticsMutex);
        incomingRtpPacketCount = _incomingRtpPacketCount;
        incomingRtpByteCount = _incomingRtpByteCount;
        unresolvedRtpPacketCount = _unresolvedRtpPacketCount;
        incomingRtcpPacketCount = _incomingRtcpPacketCount;
        incomingRtcpByteCount = _incomingRtcpByteCount;
        dataChannelMessageCount = _dataChannelMessageCount;
        dataChannelMessageByteCount = _dataChannelMessageByteCount;
        dataChannelOpen = _dataChannelOpen;
    }

    RTC_LOG(LS_INFO) << "InstanceV4_0_0Impl stopping version=" << _version
                     << " signalingPackets=" << _signalingPacketCount
                     << " signalingBytes=" << _signalingByteCount
                     << " rtpPackets=" << incomingRtpPacketCount
                     << " rtpBytes=" << incomingRtpByteCount
                     << " unresolvedRtp=" << unresolvedRtpPacketCount
                     << " rtcpPackets=" << incomingRtcpPacketCount
                     << " rtcpBytes=" << incomingRtcpByteCount
                     << " dataChannelOpen=" << (dataChannelOpen ? 1 : 0)
                     << " dataMessages=" << dataChannelMessageCount
                     << " dataBytes=" << dataChannelMessageByteCount;
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "InstanceV4_0_0Impl stopping version=%{public}s signalingPackets=%{public}zu rtpPackets=%{public}zu rtpBytes=%{public}zu unresolvedRtp=%{public}zu rtcpPackets=%{public}zu mediaReady=%{public}d outgoingChan=%{public}d incomingChan=%{public}d",
                 _version.c_str(),
                 _signalingPacketCount,
                 incomingRtpPacketCount,
                 incomingRtpByteCount,
                 unresolvedRtpPacketCount,
                 incomingRtcpPacketCount,
                 _mediaPipelineReady ? 1 : 0,
                 _outgoingAudioChannel ? 1 : 0,
                 _incomingAudioChannel ? 1 : 0);
    // Shutdown ordering — four steps, each fixing a different race:
    //   1. networking->stop()      — unsubscribes transport callbacks (no new
    //                                 onRtcpPacketReceived / onTransport... fired).
    //   2. fence on networkThread  — drains in-flight callbacks already enqueued so
    //                                 they cannot race with _call.reset() / channel
    //                                 destructors on worker thread.
    //   3. teardown media          — channels run SetRtpTransport(nullptr) while
    //                                 RtpTransport is still alive (owned by
    //                                 _networking, freed in step 4).
    //   4. _networking.reset()     — final destruction of NativeNetworkingImpl
    //                                 and the underlying DtlsSrtpTransport.
    shutdownInstance();

    if (completion) {
        completion(tgcalls::FinalState());
    }
}

InstanceV4_0_0Impl::InstanceV4_0_0Impl(Descriptor &&descriptor) {
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
    _internal.reset(new ThreadLocalObject<InstanceV4_0_0ImplInternal>(_threads->getMediaThread(), [descriptor = std::move(descriptor), threads = _threads]() mutable {
        return std::make_shared<InstanceV4_0_0ImplInternal>(std::move(descriptor), threads);
    }));
    _internal->perform([](InstanceV4_0_0ImplInternal *internal) {
        internal->start();
    });
}

InstanceV4_0_0Impl::~InstanceV4_0_0Impl() {
    webrtc::LogMessage::RemoveLogToStream(_logSink.get());
}

void InstanceV4_0_0Impl::receiveSignalingData(const std::vector<uint8_t> &data) {
    _internal->perform([data](InstanceV4_0_0ImplInternal *internal) {
        internal->receiveSignalingData(data);
    });
}

void InstanceV4_0_0Impl::setVideoCapture(std::shared_ptr<VideoCaptureInterface> videoCapture) {
    _internal->perform([videoCapture](InstanceV4_0_0ImplInternal *internal) {
        internal->setVideoCapture(videoCapture);
    });
}

void InstanceV4_0_0Impl::setRequestedVideoAspect(float aspect) {
    _internal->perform([aspect](InstanceV4_0_0ImplInternal *internal) {
        internal->setRequestedVideoAspect(aspect);
    });
}

void InstanceV4_0_0Impl::setNetworkType(NetworkType networkType) {
    _internal->perform([networkType](InstanceV4_0_0ImplInternal *internal) {
        internal->setNetworkType(networkType);
    });
}

void InstanceV4_0_0Impl::setMuteMicrophone(bool muteMicrophone) {
    _internal->perform([muteMicrophone](InstanceV4_0_0ImplInternal *internal) {
        internal->setMuteMicrophone(muteMicrophone);
    });
}

void InstanceV4_0_0Impl::setIncomingVideoOutput(std::weak_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
    _internal->perform([sink](InstanceV4_0_0ImplInternal *internal) {
        internal->setIncomingVideoOutput(sink);
    });
}

void InstanceV4_0_0Impl::setAudioInputDevice(std::string id) {
    _internal->perform([id](InstanceV4_0_0ImplInternal *internal) {
        internal->setAudioInputDevice(id);
    });
}

void InstanceV4_0_0Impl::setAudioOutputDevice(std::string id) {
    _internal->perform([id](InstanceV4_0_0ImplInternal *internal) {
        internal->setAudioOutputDevice(id);
    });
}

void InstanceV4_0_0Impl::setIsLowBatteryLevel(bool isLowBatteryLevel) {
    _internal->perform([isLowBatteryLevel](InstanceV4_0_0ImplInternal *internal) {
        internal->setIsLowBatteryLevel(isLowBatteryLevel);
    });
}

void InstanceV4_0_0Impl::setInputVolume(float level) {
    (void)level;
}

void InstanceV4_0_0Impl::setOutputVolume(float level) {
    (void)level;
}

void InstanceV4_0_0Impl::setAudioOutputDuckingEnabled(bool enabled) {
    (void)enabled;
}

void InstanceV4_0_0Impl::setAudioOutputGainControlEnabled(bool enabled) {
    (void)enabled;
}

void InstanceV4_0_0Impl::setEchoCancellationStrength(int strength) {
    (void)strength;
}

std::vector<std::string> InstanceV4_0_0Impl::GetVersions() {
    std::vector<std::string> result;
    result.push_back("4.0.0");
    return result;
}

int InstanceV4_0_0Impl::GetConnectionMaxLayer() {
    return 92;
}

std::string InstanceV4_0_0Impl::getLastError() {
    return "";
}

std::string InstanceV4_0_0Impl::getDebugInfo() {
    return "";
}

int64_t InstanceV4_0_0Impl::getPreferredRelayId() {
    return 0;
}

TrafficStats InstanceV4_0_0Impl::getTrafficStats() {
    return {};
}

PersistentState InstanceV4_0_0Impl::getPersistentState() {
    return {};
}

void InstanceV4_0_0Impl::stop(std::function<void(FinalState)> completion) {
    std::string debugLog;
    if (_logSink) {
        debugLog = _logSink->result();
    }
    _internal->perform([completion, debugLog = std::move(debugLog)](InstanceV4_0_0ImplInternal *internal) mutable {
        internal->stop([completion, debugLog = std::move(debugLog)](FinalState finalState) mutable {
            finalState.debugLog = debugLog;
            completion(finalState);
        });
    });
}

template <>
bool Register<InstanceV4_0_0Impl>() {
    return Meta::RegisterOne<InstanceV4_0_0Impl>();
}

} // namespace tgcalls
