#include "v2/CompatSignalingFraming.h"

#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

using tgcalls::CompatSignalingFraming;
using tgcalls::compatSignalingFraming;

// The legacy versions frame signaling as InstanceV2Impl frames them under the
// same version: the peer is an unchanged client of that version.
void TestLegacyVersionsMatchInstanceV2Impl() {
    CHECK_TRUE(compatSignalingFraming("7.0.0", false) == CompatSignalingFraming::V1);
    CHECK_TRUE(compatSignalingFraming("8.0.0", false) == CompatSignalingFraming::V2);
    CHECK_TRUE(compatSignalingFraming("9.0.0", false) == CompatSignalingFraming::V2);
    CHECK_TRUE(compatSignalingFraming("12.0.0", false) == CompatSignalingFraming::V3);
    CHECK_TRUE(compatSignalingFraming("13.0.0", false) == CompatSignalingFraming::V3);
}

// The server serves 14.0.0 to one end of a call whose other end it serves
// 8.0.0, which builds InstanceV2Impl. Each end picks its framing from its own
// version string, so 14.0.0 must speak 8.0.0's wire or no message crosses.
void TestOwnVersionSpeaksTheWireOfEightZeroZero() {
    CHECK_TRUE(compatSignalingFraming("14.0.0", false) == CompatSignalingFraming::V2);
}

void TestAnUnknownVersionIsV3() {
    CHECK_TRUE(compatSignalingFraming("15.0.0", false) == CompatSignalingFraming::V3);
    CHECK_TRUE(compatSignalingFraming("", false) == CompatSignalingFraming::V3);
}

// network_kcp_experiment moves InstanceV2Impl's every version to V3, and both
// ends get the same custom parameters, so it moves ours too: it is how a
// 14.0.0 end talks to a 13.0.0 one.
void TestKcpExperimentMovesEveryVersionToV3() {
    for (const char *version : {"7.0.0", "8.0.0", "9.0.0", "13.0.0", "14.0.0"}) {
        CHECK_TRUE(compatSignalingFraming(version, true) == CompatSignalingFraming::V3);
    }
}

} // namespace

int main() {
    TestLegacyVersionsMatchInstanceV2Impl();
    TestOwnVersionSpeaksTheWireOfEightZeroZero();
    TestAnUnknownVersionIsV3();
    TestKcpExperimentMovesEveryVersionToV3();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
