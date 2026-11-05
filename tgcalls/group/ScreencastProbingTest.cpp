#include "group/ScreencastSendParameters.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "api/field_trials_view.h"
#include "api/rtc_event_log/rtc_event_log.h"
#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "modules/congestion_controller/goog_cc/probe_controller.h"

// WebRTC's own ProbeController, driven as goog_cc drives it for the in-app screencast's connection:
// periodic ALR probing on (what cricket::MediaConfig::video.periodic_alr_bandwidth_probing turns on),
// with and without the reference engine's field trial (iosScreencastFieldTrial). A misspelled key or
// value would be ignored by WebRTC without a word, which only this test would show.

namespace {

int g_failures = 0;

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

class ScreencastTrials final : public webrtc::FieldTrialsView {
public:
    explicit ScreencastTrials(bool withScreencastTrial) : _withScreencastTrial(withScreencastTrial) {}

    std::string Lookup(absl::string_view key) const override {
        return _withScreencastTrial ? tgcalls::iosScreencastFieldTrial(std::string(key)) : std::string();
    }

private:
    bool _withScreencastTrial = false;
};

// The connection's caps as both engines set them: transport max = iosScreencastTotalMaxBitrateBps(), the
// streams' total = 240p layer + top layer + audio.
const webrtc::DataRate kMaxBitrate = webrtc::DataRate::BitsPerSec(tgcalls::iosScreencastTotalMaxBitrateBps());
const webrtc::DataRate kMaxTotalAllocated = webrtc::DataRate::BitsPerSec(100000 + 2000000 + 32000);

// The probes ProbeController asks for 6 s into an application-limited period (a still screen) with
// the given estimate, start-up probing already over.
std::vector<webrtc::ProbeClusterConfig> periodicProbes(bool withScreencastTrial, webrtc::DataRate estimate) {
    ScreencastTrials trials(withScreencastTrial);
    webrtc::RtcEventLogNull eventLog;
    webrtc::ProbeController controller(&trials, &eventLog);
    controller.EnablePeriodicAlrProbing(true);

    webrtc::Timestamp now = webrtc::Timestamp::Seconds(100);
    webrtc::NetworkAvailability availability;
    availability.at_time = now;
    availability.network_available = true;
    (void)controller.OnNetworkAvailability(availability);
    (void)controller.SetBitrates(webrtc::DataRate::KilobitsPerSec(100), webrtc::DataRate::KilobitsPerSec(400), kMaxBitrate, now);
    (void)controller.OnMaxTotalAllocatedBitrate(kMaxTotalAllocated, now);

    now += webrtc::TimeDelta::Seconds(2);
    (void)controller.Process(now);
    (void)controller.SetEstimatedBitrate(estimate, webrtc::BandwidthLimitedCause::kDelayBasedLimited, now);
    controller.SetAlrStartTimeMs(now.ms());

    now += webrtc::TimeDelta::Seconds(6);
    return controller.Process(now);
}

void testTheTrialIsTheOneWebRtcParses() {
    ScreencastTrials trials(true);
    webrtc::ProbeControllerConfig config(&trials);
    CHECK_TRUE(std::fabs(config.skip_if_estimate_larger_than_fraction_of_max.Get() - 0.9) < 0.0001);
    // Only that setting: the probe interval and scale stay WebRTC's.
    CHECK_TRUE(config.alr_probing_interval.Get() == webrtc::TimeDelta::Seconds(5));
    CHECK_TRUE(std::fabs(config.alr_probe_scale.Get() - 2.0) < 0.0001);
}

void testOnlyTheProbingKeyIsAnswered() {
    CHECK_TRUE(tgcalls::iosScreencastFieldTrial("WebRTC-Bwe-ProbingConfiguration").size() > 0);
    CHECK_TRUE(tgcalls::iosScreencastFieldTrial("WebRTC-Bwe-AlrProbing").empty());
    CHECK_TRUE(tgcalls::iosScreencastFieldTrial("WebRTC-Video-DiscardPacketsWithUnknownSsrc").empty());
    CHECK_TRUE(tgcalls::iosScreencastFieldTrial("").empty());
}

void testALowEstimateIsProbedAtTwiceItself() {
    const auto probes = periodicProbes(true, webrtc::DataRate::KilobitsPerSec(500));
    CHECK_TRUE(probes.size() == 1);
    if (!probes.empty()) {
        CHECK_TRUE(probes[0].target_data_rate == webrtc::DataRate::KilobitsPerSec(1000));
    }
}

void testAProbeNeverExceedsTheCap() {
    const auto probes = periodicProbes(true, webrtc::DataRate::KilobitsPerSec(1500));
    CHECK_TRUE(probes.size() == 1);
    if (!probes.empty()) {
        CHECK_TRUE(probes[0].target_data_rate == kMaxBitrate);
    }
}

void testAnEstimateAtTheCapIsNotProbed() {
    CHECK_TRUE(periodicProbes(true, kMaxBitrate).empty());
    // Within 10% of the cap: nothing left worth finding.
    CHECK_TRUE(periodicProbes(true, webrtc::DataRate::KilobitsPerSec(1900)).empty());
}

void testWithoutTheTrialAnEstimateAtTheCapIsStillProbed() {
    // The custom engine's case (its field trials are the process's): a probe every 5 s at the cap.
    const auto probes = periodicProbes(false, kMaxBitrate);
    CHECK_TRUE(probes.size() == 1);
    if (!probes.empty()) {
        CHECK_TRUE(probes[0].target_data_rate == kMaxBitrate);
    }
}

} // namespace

int main() {
    testTheTrialIsTheOneWebRtcParses();
    testOnlyTheProbingKeyIsAnswered();
    testALowEstimateIsProbedAtTwiceItself();
    testAProbeNeverExceedsTheCap();
    testAnEstimateAtTheCapIsNotProbed();
    testWithoutTheTrialAnEstimateAtTheCapIsStillProbed();
    if (g_failures == 0) {
        std::printf("PASS screencast_probing_test\n");
        return 0;
    }
    std::printf("%d failure(s)\n", g_failures);
    return 1;
}
