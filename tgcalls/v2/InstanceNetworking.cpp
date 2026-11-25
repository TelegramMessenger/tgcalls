#include "v2/InstanceNetworking.h"

namespace tgcalls {

namespace {

// The candidate type as WebRTC named it before it became an enum (M123's
// LOCAL/STUN/PRFLX/RELAY_PORT_TYPE): the call's stats log carries these strings.
std::string legacyCandidateTypeName(webrtc::IceCandidateType type) {
    switch (type) {
        case webrtc::IceCandidateType::kHost:
            return "local";
        case webrtc::IceCandidateType::kSrflx:
            return "stun";
        case webrtc::IceCandidateType::kPrflx:
            return "prflx";
        case webrtc::IceCandidateType::kRelay:
            return "relay";
    }
    return "local";
}

}  // namespace

InstanceNetworking::ConnectionDescription::CandidateDescription InstanceNetworking::connectionDescriptionFromCandidate(webrtc::Candidate const &candidate) {
    InstanceNetworking::ConnectionDescription::CandidateDescription result;

    result.type = legacyCandidateTypeName(candidate.type());
    result.protocol = candidate.protocol();
    result.address = candidate.address().ToString();

    return result;
}

}  // namespace tgcalls
