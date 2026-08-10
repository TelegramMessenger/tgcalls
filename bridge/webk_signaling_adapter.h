#ifndef TGCALL_WEBK_SIGNALING_ADAPTER_H
#define TGCALL_WEBK_SIGNALING_ADAPTER_H

#include <cstdint>

#include "tgcalls/v2/Signaling.h"
#include "tgcalls/v2_4_0_0/Signaling_4_0_0.h"

namespace tgcall {

struct WebKToV2InitialSetupMapping {
  tgcalls::signaling::InitialSetupMessage initialSetup;
  tgcalls::signaling::NegotiateChannelsMessage negotiation;
  bool hasAudio = false;
  bool hasVideo = false;
  bool hasScreencast = false;
};

WebKToV2InitialSetupMapping MapWebKInitialSetupToV2(
    const tgcalls::signaling_4_0_0::InitialSetupMessage &initialSetup,
    uint32_t exchangeId);

tgcalls::signaling::CandidatesMessage MapWebKCandidatesToV2(
    const tgcalls::signaling_4_0_0::CandidatesMessage &candidates);

tgcalls::signaling_4_0_0::InitialSetupMessage MapV2InitialSetupToWebK(
    const tgcalls::signaling::InitialSetupMessage &initialSetup,
    const tgcalls::signaling::NegotiateChannelsMessage &negotiation);

tgcalls::signaling_4_0_0::CandidatesMessage MapV2CandidatesToWebK(
    const tgcalls::signaling::CandidatesMessage &candidates);

tgcalls::signaling_4_0_0::MediaStateMessage MapV2MediaStateToWebK(
    const tgcalls::signaling::MediaStateMessage &mediaState);

}  // namespace tgcall

#endif  // TGCALL_WEBK_SIGNALING_ADAPTER_H
