// When a group screen share sends its top layer in AV1, and the reason it does not, in a fixed order.

#include "group/GroupAv1Decision.h"

#include <cstdio>
#include <string>

static int failures = 0;

#define CHECK_TRUE(cond)                                                                   \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "%s:%d: CHECK_TRUE(%s) failed\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                    \
        }                                                                                  \
    } while (0)

namespace {

tgcalls::GroupAv1TopLayerInputs Good() {
    tgcalls::GroupAv1TopLayerInputs inputs;
    inputs.allowed = true;
    inputs.serverListsAv1 = true;
    inputs.deviceCanEncode = true;
    inputs.inAppScreencast = true;
    inputs.encrypted = false;
    inputs.baseCodecName = "H264";
    inputs.layerCount = 2;
    return inputs;
}

void CheckNo(tgcalls::GroupAv1TopLayerInputs const &inputs, std::string const &reason) {
    const auto decision = tgcalls::decideGroupAv1TopLayer(inputs);
    CHECK_TRUE(!decision.useAv1);
    CHECK_TRUE(decision.reason == reason);
    if (decision.reason != reason) {
        std::fprintf(stderr, "  reason: '%s', expected '%s'\n", decision.reason.c_str(), reason.c_str());
    }
}

void TestAllGood() {
    const auto decision = tgcalls::decideGroupAv1TopLayer(Good());
    CHECK_TRUE(decision.useAv1);
    CHECK_TRUE(decision.reason.empty());

    auto three = Good();
    three.layerCount = 3;
    CHECK_TRUE(tgcalls::decideGroupAv1TopLayer(three).useAv1);

    auto lowercase = Good();
    lowercase.baseCodecName = "h264";
    CHECK_TRUE(tgcalls::decideGroupAv1TopLayer(lowercase).useAv1);
}

void TestEachInputAlone() {
    auto inputs = Good();
    inputs.allowed = false;
    CheckNo(inputs, "not allowed");

    inputs = Good();
    inputs.inAppScreencast = false;
    CheckNo(inputs, "not the in-app screen share");

    inputs = Good();
    inputs.encrypted = true;
    CheckNo(inputs, "encrypted");

    inputs = Good();
    inputs.serverListsAv1 = false;
    CheckNo(inputs, "server did not list AV1");

    inputs = Good();
    inputs.deviceCanEncode = false;
    CheckNo(inputs, "chip");

    inputs = Good();
    inputs.baseCodecName = "VP8";
    CheckNo(inputs, "base codec VP8");

    inputs = Good();
    inputs.layerCount = 1;
    CheckNo(inputs, "single layer");
}

void TestFirstFailureWins() {
    auto inputs = Good();
    inputs.encrypted = true;
    inputs.layerCount = 1;
    CheckNo(inputs, "encrypted");

    inputs = Good();
    inputs.serverListsAv1 = false;
    inputs.deviceCanEncode = false;
    CheckNo(inputs, "server did not list AV1");

    inputs = Good();
    inputs.allowed = false;
    inputs.inAppScreencast = false;
    CheckNo(inputs, "not allowed");
}

tgcalls::GroupAv1TopLayerInputs GoodSingle() {
    auto inputs = Good();
    inputs.layers = tgcalls::GroupAv1ScreencastLayers::Single;
    inputs.layerCount = 1;
    return inputs;
}

// One layer, AV1 as the sender's codec: the share joined with one layer and the server listed AV1.
void TestSingleLayer() {
    const auto decision = tgcalls::decideGroupAv1TopLayer(GoodSingle());
    CHECK_TRUE(decision.useAv1);
    CHECK_TRUE(decision.reason.empty());

    auto two = GoodSingle();
    two.layerCount = 2;
    CheckNo(two, "layers 2");

    auto noServer = GoodSingle();
    noServer.serverListsAv1 = false;
    CheckNo(noServer, "server did not list AV1");

    // The other modes still need two or more layers.
    auto every = Good();
    every.layers = tgcalls::GroupAv1ScreencastLayers::Every;
    CHECK_TRUE(tgcalls::decideGroupAv1TopLayer(every).useAv1);
    every.layerCount = 1;
    CheckNo(every, "single layer");
}

// The layer count goes into the join request, before the server's answer: one layer whenever AV1 could be sent then.
void TestJoinsWithSingleLayer() {
    CHECK_TRUE(tgcalls::groupAv1JoinsWithSingleLayer(GoodSingle()));

    auto inputs = GoodSingle();
    inputs.serverListsAv1 = false;
    inputs.baseCodecName = "";
    inputs.layerCount = 0;
    CHECK_TRUE(tgcalls::groupAv1JoinsWithSingleLayer(inputs));

    inputs = GoodSingle();
    inputs.allowed = false;
    CHECK_TRUE(!tgcalls::groupAv1JoinsWithSingleLayer(inputs));

    inputs = GoodSingle();
    inputs.inAppScreencast = false;
    CHECK_TRUE(!tgcalls::groupAv1JoinsWithSingleLayer(inputs));

    inputs = GoodSingle();
    inputs.encrypted = true;
    CHECK_TRUE(!tgcalls::groupAv1JoinsWithSingleLayer(inputs));

    inputs = GoodSingle();
    inputs.deviceCanEncode = false;
    CHECK_TRUE(!tgcalls::groupAv1JoinsWithSingleLayer(inputs));

    inputs = GoodSingle();
    inputs.layers = tgcalls::GroupAv1ScreencastLayers::Top;
    CHECK_TRUE(!tgcalls::groupAv1JoinsWithSingleLayer(inputs));
}

} // namespace

int main() {
    TestAllGood();
    TestEachInputAlone();
    TestFirstFailureWins();
    TestSingleLayer();
    TestJoinsWithSingleLayer();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("group_av1_decision_test: all checks passed\n");
    return 0;
}
