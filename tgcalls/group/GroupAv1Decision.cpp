#include "group/GroupAv1Decision.h"

#include "absl/strings/match.h"

namespace tgcalls {

namespace {

GroupAv1TopLayerDecision no(std::string reason) {
    GroupAv1TopLayerDecision decision;
    decision.useAv1 = false;
    decision.reason = std::move(reason);
    return decision;
}

} // namespace

GroupAv1TopLayerDecision decideGroupAv1TopLayer(GroupAv1TopLayerInputs const &inputs) {
    if (!inputs.allowed) {
        return no("not allowed");
    }
    if (!inputs.inAppScreencast) {
        return no("not the in-app screen share");
    }
    if (inputs.encrypted) {
        return no("encrypted");
    }
    if (!inputs.serverListsAv1) {
        return no("server did not list AV1");
    }
    if (!inputs.deviceCanEncode) {
        return no("chip");
    }
    if (!absl::EqualsIgnoreCase(inputs.baseCodecName, "H264")) {
        return no("base codec " + inputs.baseCodecName);
    }
    if (inputs.layers == GroupAv1ScreencastLayers::Single) {
        if (inputs.layerCount != 1) {
            return no("layers " + std::to_string(inputs.layerCount));
        }
    } else if (inputs.layerCount < 2) {
        return no("single layer");
    }
    GroupAv1TopLayerDecision decision;
    decision.useAv1 = true;
    return decision;
}

bool groupAv1JoinsWithSingleLayer(GroupAv1TopLayerInputs const &inputs) {
    return inputs.layers == GroupAv1ScreencastLayers::Single && inputs.allowed && inputs.inAppScreencast
        && !inputs.encrypted && inputs.deviceCanEncode;
}

} // namespace tgcalls
