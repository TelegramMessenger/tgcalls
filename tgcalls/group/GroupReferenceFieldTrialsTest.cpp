// Host test for GroupReferenceFieldTrials, the reference group engine's field trials.
// - It answers its own keys and nothing from the process-wide trial string: the custom group engine (a ReplayKit
//   share's, started during a reference call) and the 1:1 engines rewrite that string, which would change a running
//   reference call's trials, and its trials at start would depend on which calls ran before.
// - An in-app screen capture's connection also answers iosScreencastFieldTrial's keys.
// - A copy (an Environment may copy its field trials) answers the same.

#include "group/GroupReferenceFieldTrials.h"

#include "api/environment/deprecated_global_field_trials.h"

#include <cstdio>
#include <memory>
#include <string>

namespace {

int g_failures = 0;

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

// What the custom group engine's start() sets.
const char kCustomEngineTrials[] =
    "WebRTC-Audio-Allocation/min:32kbps,max:32kbps/"
    "WebRTC-BweLossExperiment/Enabled/";

void TestTheProcessWideTrialsDoNotReachTheEngine() {
    webrtc::DeprecatedGlobalFieldTrials::Set("");
    tgcalls::GroupReferenceFieldTrials trials(false);
    webrtc::DeprecatedGlobalFieldTrials::Set(kCustomEngineTrials);

    CHECK_TRUE(trials.Lookup("WebRTC-Audio-Allocation").empty());
    CHECK_TRUE(trials.Lookup("WebRTC-BweLossExperiment").empty());
    CHECK_TRUE(trials.CreateCopy()->Lookup("WebRTC-Audio-Allocation").empty());

    webrtc::DeprecatedGlobalFieldTrials::Set("");
}

void TestItsOwnKeysAreOn() {
    tgcalls::GroupReferenceFieldTrials trials(false);
    CHECK_TRUE(trials.Lookup("WebRTC-Video-DiscardPacketsWithUnknownSsrc") == "Enabled");
    CHECK_TRUE(trials.Lookup("WebRTC-MixedCodecSimulcast") == "Enabled");
    CHECK_TRUE(trials.CreateCopy()->Lookup("WebRTC-MixedCodecSimulcast") == "Enabled");
    CHECK_TRUE(trials.Lookup("WebRTC-Bwe-ProbingConfiguration").empty());
}

void TestAnInAppScreenCaptureAnswersItsProbingKey() {
    tgcalls::GroupReferenceFieldTrials trials(true);
    CHECK_TRUE(trials.Lookup("WebRTC-Bwe-ProbingConfiguration") == "skip_if_est_larger_than_fraction_of_max:0.9");
    CHECK_TRUE(trials.CreateCopy()->Lookup("WebRTC-Bwe-ProbingConfiguration") == "skip_if_est_larger_than_fraction_of_max:0.9");
    CHECK_TRUE(trials.Lookup("WebRTC-Video-DiscardPacketsWithUnknownSsrc") == "Enabled");
}

}  // namespace

int main() {
    TestTheProcessWideTrialsDoNotReachTheEngine();
    TestItsOwnKeysAreOn();
    TestAnInAppScreenCaptureAnswersItsProbingKey();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
