#include "group/GroupReferenceFieldTrials.h"

#include "group/ScreencastSendParameters.h"

namespace tgcalls {

namespace {

constexpr absl::string_view kDiscardUnknownSsrcVideo = "WebRTC-Video-DiscardPacketsWithUnknownSsrc";
constexpr absl::string_view kMixedCodecSimulcast = "WebRTC-MixedCodecSimulcast";

} // namespace

GroupReferenceFieldTrials::GroupReferenceFieldTrials(bool iosInAppScreencast) : _iosInAppScreencast(iosInAppScreencast) {
}

std::string GroupReferenceFieldTrials::Lookup(absl::string_view key) const {
    if (key == kDiscardUnknownSsrcVideo || key == kMixedCodecSimulcast) {
        return "Enabled";
    }
    if (_iosInAppScreencast) {
        std::string screencastValue = iosScreencastFieldTrial(std::string(key));
        if (!screencastValue.empty()) {
            return screencastValue;
        }
    }
    // Nothing from the process-wide trial string (webrtc::DeprecatedGlobalFieldTrials): the custom group engine (a
    // ReplayKit share's, started during this call) and the 1:1 engines rewrite it, so this call's trials would change
    // mid-call and depend on which calls ran before it.
    return std::string();
}

std::unique_ptr<webrtc::FieldTrialsView> GroupReferenceFieldTrials::CreateCopy() const {
    return std::make_unique<GroupReferenceFieldTrials>(_iosInAppScreencast);
}

} // namespace tgcalls
