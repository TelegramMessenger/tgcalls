#include "group/GroupOpusDred.h"

#include <algorithm>
#include <mutex>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "api/audio_codecs/L16/audio_decoder_L16.h"
#include "api/audio_codecs/L16/audio_encoder_L16.h"
#include "api/audio_codecs/audio_decoder_factory_template.h"
#include "api/audio_codecs/audio_encoder_factory_template.h"
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"
#include "api/make_ref_counted.h"
#ifdef TGCALLS_ENABLE_OPUS_DRED
#include "modules/audio_coding/codecs/opus/opus_interface.h"
#endif

namespace tgcalls {

namespace {

constexpr absl::string_view kOpusDredTrial = "WebRTC-Audio-OpusDred";
constexpr absl::string_view kOpusGeneratePlcTrial = "WebRTC-Audio-OpusGeneratePlc";
constexpr absl::string_view kAudioAllocationTrial = "WebRTC-Audio-Allocation";
constexpr absl::string_view kDredAllocation = "min:40kbps,max:40kbps";

#define TGCALLS_GROUP_CUSTOM_ENGINE_TRIALS_TAIL                                                                        \
    "WebRTC-Audio-MinimizeResamplingOnMobile/Enabled/"                                                                 \
    "WebRTC-BweLossExperiment/Enabled/"                                                                                \
    "WebRTC-Video-DiscardPacketsWithUnknownSsrc/Enabled/"                                                              \
    /* Top: the screen share's encodings name different codecs (setGroupAv1TopLayerCodecs). */                         \
    "WebRTC-MixedCodecSimulcast/Enabled/"
#define TGCALLS_GROUP_DRED_RECEIVE_TRIALS "WebRTC-Audio-OpusDred/Enabled/WebRTC-Audio-OpusGeneratePlc/Enabled/"

constexpr char kCustomEngineTrials[] =
    "WebRTC-Audio-Allocation/min:32kbps,max:32kbps/" TGCALLS_GROUP_CUSTOM_ENGINE_TRIALS_TAIL;
constexpr char kCustomEngineTrialsReceive[] =
    "WebRTC-Audio-Allocation/min:32kbps,max:32kbps/" TGCALLS_GROUP_CUSTOM_ENGINE_TRIALS_TAIL
    TGCALLS_GROUP_DRED_RECEIVE_TRIALS;
constexpr char kCustomEngineTrialsSend[] =
    "WebRTC-Audio-Allocation/min:40kbps,max:40kbps/" TGCALLS_GROUP_CUSTOM_ENGINE_TRIALS_TAIL;
constexpr char kCustomEngineTrialsReceiveSend[] =
    "WebRTC-Audio-Allocation/min:40kbps,max:40kbps/" TGCALLS_GROUP_CUSTOM_ENGINE_TRIALS_TAIL
    TGCALLS_GROUP_DRED_RECEIVE_TRIALS;

GroupOpusDnnWeights UsableOrNull(GroupOpusDnnWeights weights) {
    return groupOpusDnnWeightsUsable(weights) ? std::move(weights) : nullptr;
}

// Group calls: a fixed loss figure (> 0), no FEC, a fixed bitrate. 1:1 calls: adaptive, DRED's duration and the
// weights only.
struct DredEncoderMode {
    int fixedPacketLossPercent = 0;
    bool adaptive = false;
};

class GroupAudioEncoderFactory : public webrtc::AudioEncoderFactory {
public:
    GroupAudioEncoderFactory(DredEncoderMode mode, GroupOpusDnnWeights weights, bool includeL16) :
    _weights(UsableOrNull(std::move(weights))),
    _dredFixedPacketLossPercent(mode.adaptive ? 0 : groupDredFixedPacketLossPercent(mode.fixedPacketLossPercent,
                                                                                    _weights != nullptr)),
    _dredAdaptive(mode.adaptive && groupOpusDredAvailable() && _weights != nullptr),
    _inner(includeL16 ? webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus, webrtc::AudioEncoderL16>()
                      : webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus>()) {
    }

    std::vector<webrtc::AudioCodecSpec> GetSupportedEncoders() override {
        return _inner->GetSupportedEncoders();
    }

    std::optional<webrtc::AudioCodecInfo> QueryAudioEncoder(const webrtc::SdpAudioFormat &format) override {
        return _inner->QueryAudioEncoder(format);
    }

    std::unique_ptr<webrtc::AudioEncoder> Create(const webrtc::Environment &env, const webrtc::SdpAudioFormat &format,
                                                 Options options) override {
#ifdef TGCALLS_ENABLE_OPUS_DRED
        // The DRED fields of AudioEncoderOpusConfig are the WebRTC fork's (TGCALLS PATCH: Opus DRED).
        if ((_dredFixedPacketLossPercent > 0 || _dredAdaptive) && absl::EqualsIgnoreCase(format.name, "opus")) {
            if (std::optional<webrtc::AudioEncoderOpusConfig> config = webrtc::AudioEncoderOpus::SdpToConfig(format)) {
                config->dred_duration_10ms = kGroupDredDuration10ms;
                config->dnn_weights = _weights;
                if (!_dredAdaptive) {
                    config->fixed_packet_loss_percent = _dredFixedPacketLossPercent;
                    config->fec_enabled = false;
                    config->bitrate_bps = kGroupDredBitrateBps;
                }
                return webrtc::AudioEncoderOpus::MakeAudioEncoder(env, *config, options);
            }
        }
#endif
        return _inner->Create(env, format, std::move(options));
    }

private:
    const GroupOpusDnnWeights _weights;
    const int _dredFixedPacketLossPercent;
    const bool _dredAdaptive;
    const webrtc::scoped_refptr<webrtc::AudioEncoderFactory> _inner;
};

class GroupAudioDecoderFactory : public webrtc::AudioDecoderFactory {
public:
    GroupAudioDecoderFactory(GroupOpusDnnWeights weights, bool includeL16) :
    _weights(UsableOrNull(std::move(weights))),
    _inner(includeL16 ? webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus, webrtc::AudioDecoderL16>()
                      : webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus>()) {
    }

    std::vector<webrtc::AudioCodecSpec> GetSupportedDecoders() override {
        return _inner->GetSupportedDecoders();
    }

    bool IsSupportedDecoder(const webrtc::SdpAudioFormat &format) override {
        return _inner->IsSupportedDecoder(format);
    }

    std::unique_ptr<webrtc::AudioDecoder> Create(const webrtc::Environment &env,
                                                 const webrtc::SdpAudioFormat &format) override {
#ifdef TGCALLS_ENABLE_OPUS_DRED
        // AudioDecoderOpusConfig::dnn_weights is the WebRTC fork's (TGCALLS PATCH: Opus DRED).
        if (_weights && absl::EqualsIgnoreCase(format.name, "opus")) {
            if (std::optional<webrtc::AudioDecoderOpus::Config> config = webrtc::AudioDecoderOpus::SdpToConfig(format)) {
                config->dnn_weights = _weights;
                return webrtc::AudioDecoderOpus::MakeAudioDecoder(env, *config);
            }
        }
#endif
        return _inner->Create(env, format);
    }

private:
    const GroupOpusDnnWeights _weights;
    const webrtc::scoped_refptr<webrtc::AudioDecoderFactory> _inner;
};

} // namespace

bool groupOpusDredAvailable() {
#ifdef TGCALLS_ENABLE_OPUS_DRED
    return true;
#else
    return false;
#endif
}

bool groupOpusDnnWeightsUsable(const GroupOpusDnnWeights &weights) {
#ifdef TGCALLS_ENABLE_OPUS_DRED
    return weights && WebRtcOpus_DnnWeightsAreWellFormed(weights->data(), weights->size()) == 1;
#else
    return false;
#endif
}

int groupDredFixedPacketLossPercent(int requested, bool weightsUsable) {
    return groupOpusDredAvailable() && weightsUsable ? std::clamp(requested, 0, 100) : 0;
}

bool groupDredReceiveEnabled(bool requested, bool weightsUsable) {
    return groupOpusDredAvailable() && weightsUsable && requested;
}

webrtc::scoped_refptr<webrtc::AudioEncoderFactory> CreateGroupAudioEncoderFactory(int dredFixedPacketLossPercent,
                                                                                  GroupOpusDnnWeights weights,
                                                                                  bool includeL16) {
    return webrtc::make_ref_counted<GroupAudioEncoderFactory>(
        DredEncoderMode{.fixedPacketLossPercent = dredFixedPacketLossPercent}, std::move(weights), includeL16);
}

webrtc::scoped_refptr<webrtc::AudioDecoderFactory> CreateGroupAudioDecoderFactory(GroupOpusDnnWeights weights,
                                                                                  bool includeL16) {
    return webrtc::make_ref_counted<GroupAudioDecoderFactory>(std::move(weights), includeL16);
}

std::string groupOpusDredDescription(int dredFixedPacketLossPercent, bool receive, const GroupOpusDnnWeights &weights) {
    std::string result = "send " +
        (dredFixedPacketLossPercent > 0 ? std::to_string(dredFixedPacketLossPercent) + "%" : std::string("off")) +
        ", receive " + (receive ? "on" : "off");
    if (!groupOpusDredAvailable()) {
        return result + " (not in this build)";
    }
    if (!weights) {
        return result + ", no weights";
    }
    return result + ", weights " + std::to_string(weights->size()) + " bytes" +
        (groupOpusDnnWeightsUsable(weights) ? "" : ", not usable");
}

std::string groupOpusDredFieldTrial(absl::string_view key, bool receive, bool send) {
    receive = receive && groupOpusDredAvailable();
    send = send && groupOpusDredAvailable();
    if (receive && (key == kOpusDredTrial || key == kOpusGeneratePlcTrial)) {
        return "Enabled";
    }
    if (send && key == kAudioAllocationTrial) {
        return std::string(kDredAllocation);
    }
    return std::string();
}

const char *groupCustomEngineFieldTrials(bool receive, bool send) {
    receive = receive && groupOpusDredAvailable();
    send = send && groupOpusDredAvailable();
    if (receive) {
        return send ? kCustomEngineTrialsReceiveSend : kCustomEngineTrialsReceive;
    }
    return send ? kCustomEngineTrialsSend : kCustomEngineTrials;
}

namespace {

// The running custom-engine main connection's DRED flags (bit 0 receive, bit 1 send) and its token; token 0: none.
std::mutex mainConnectionMutex;
uint64_t mainConnectionToken = 0;
uint64_t lastIssuedMainConnectionToken = 0;
int mainConnectionDredFlags = 0;

} // namespace

const char *groupCustomEngineFieldTrialsForConnection(bool isScreencast, bool receive, bool send,
                                                      uint64_t *token) {
    std::lock_guard<std::mutex> lock(mainConnectionMutex);
    if (isScreencast) {
        if (mainConnectionToken != 0) {
            return groupCustomEngineFieldTrials((mainConnectionDredFlags & 1) != 0, (mainConnectionDredFlags & 2) != 0);
        }
        return groupCustomEngineFieldTrials(receive, send);
    }
    mainConnectionToken = ++lastIssuedMainConnectionToken;
    mainConnectionDredFlags = (receive ? 1 : 0) | (send ? 2 : 0);
    if (token) {
        *token = mainConnectionToken;
    }
    return groupCustomEngineFieldTrials(receive, send);
}

void groupCustomEngineMainConnectionEnded(uint64_t token) {
    std::lock_guard<std::mutex> lock(mainConnectionMutex);
    if (token != 0 && token == mainConnectionToken) {
        mainConnectionToken = 0;
        mainConnectionDredFlags = 0;
    }
}

bool p2pOpusDredEnabled(bool requested, const GroupOpusDnnWeights &weights) {
    return requested && groupOpusDnnWeightsUsable(weights);
}

webrtc::scoped_refptr<webrtc::AudioEncoderFactory> CreateP2PAudioEncoderFactory(bool dred, GroupOpusDnnWeights weights) {
    return webrtc::make_ref_counted<GroupAudioEncoderFactory>(DredEncoderMode{.adaptive = dred}, std::move(weights),
                                                              false);
}

namespace {

// "min:48kbps,max:64kbps": WebRTC-Audio-Allocation's format (AudioAllocationConfig).
std::string p2pDredAudioAllocation() {
    return "min:" + std::to_string(kP2PDredMinBitrateBps / 1000) + "kbps,max:" +
        std::to_string(kP2PDredMaxBitrateBps / 1000) + "kbps";
}

class P2PFieldTrials final : public webrtc::FieldTrialsView {
public:
    explicit P2PFieldTrials(bool dred) : _dred(dred && groupOpusDredAvailable()) {
    }

    std::string Lookup(absl::string_view key) const override {
        if (_dred && key == kAudioAllocationTrial) {
            return p2pDredAudioAllocation();
        }
        return groupOpusDredFieldTrial(key, _dred, false);
    }

    // An Environment may copy its field trials; the default aborts.
    std::unique_ptr<webrtc::FieldTrialsView> CreateCopy() const override {
        return std::make_unique<P2PFieldTrials>(_dred);
    }

private:
    const bool _dred;
};

} // namespace

std::unique_ptr<webrtc::FieldTrialsView> CreateP2PFieldTrials(bool dred) {
    return std::make_unique<P2PFieldTrials>(dred);
}

std::string p2pOpusDredDescription(bool requested, const GroupOpusDnnWeights &weights) {
    if (!groupOpusDredAvailable()) {
        return "off (not in this build)";
    }
    if (!requested) {
        return "off (audio_dred false)";
    }
    if (!weights) {
        return "off, no weights";
    }
    if (!groupOpusDnnWeightsUsable(weights)) {
        return "off, weights " + std::to_string(weights->size()) + " bytes, not usable";
    }
    return "on, weights " + std::to_string(weights->size()) + " bytes";
}

} // namespace tgcalls
