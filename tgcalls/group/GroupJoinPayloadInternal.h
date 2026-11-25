#ifndef TGCALLS_GROUP_JOIN_PAYLOAD_INTERNAL_H
#define TGCALLS_GROUP_JOIN_PAYLOAD_INTERNAL_H

#include "GroupJoinPayload.h"

#include <vector>
#include <string>
#include <stdint.h>

#include <optional>

#include "api/candidate.h"

namespace tgcalls {

struct GroupJoinResponsePayload {
    GroupJoinTransportDescription transport;
    std::optional<GroupJoinVideoInformation> videoInformation;

    static std::optional<GroupJoinResponsePayload> parse(std::string const &data);
};

struct GroupJoinInternalPayload {
    GroupJoinTransportDescription transport;

    uint32_t audioSsrc = 0;
    std::optional<GroupParticipantVideoInformation> videoInformation;

    std::string serialize();
};

// The ICE candidate type a join response names ("host", "srflx", "prflx", "relay"); anything else counts as a host
// candidate.
webrtc::IceCandidateType groupIceCandidateType(std::string const &type);

}

#endif
