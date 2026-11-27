// The report writer's exact output: broadcast mode's reports and the new player's runner's must keep these keys.
// Run: bazel test //submodules/TgVoipWebrtc/tgcalls/tools/cli:broadcast_report_writer_test

#include "broadcast_report.h"

#include <cstdio>
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

void checkEqual(std::string const &actual, std::string const &expected) {
    if (actual != expected) {
        std::printf("expected: %s\nactual:   %s\n", expected.c_str(), actual.c_str());
        CHECK_TRUE(false);
    }
}

void testAListenerReport() {
    broadcast::BroadcastReportInput input;
    input.scenario = "tiles";
    input.profile = "wifi";
    input.events = "";
    input.availabilityDelayMs = 300;
    input.durationS = 25;
    input.repetition = 2;
    input.rejoins = 1;
    input.audio.startMs = 812;
    input.audio.stallCount = 1;
    input.audio.stallMs = 300;
    input.audio.latencyMedianMs = 3900;
    input.audio.heardBlocks = {{"marker", 100}, {"2300", 90}};
    broadcast::VideoReport video;
    video.startMs = 3400;
    video.frames = 600;
    video.droppedFrames = 3;
    video.unplacedFrames = 1;
    video.latencyMedianMs = 3500;
    video.latencyMaxMs = 3520;
    video.widths = {{320, 600}};
    input.video["ep-0"] = video;
    broadcast::LevelStats levels;
    levels.meanLevel = 0.5;
    levels.speakingFraction = 0.25;
    levels.updates = 200;
    input.levels["speaker0"] = levels;
    input.serverStatsJson = "{\"requests\":{\"audio\":{\"success\":25}},\"bytes\":{\"audio\":300000}}";
    checkEqual(broadcast::broadcastReportJson(input),
        "{\"audio\": {\"heardBlocks\": {\"2300\": 90, \"marker\": 100}, \"latencyMaxMs\": null, \"latencyMedianMs\": 3900, "
        "\"stallCount\": 1, \"stallMs\": 300, \"startMs\": 812}, \"availabilityDelayMs\": 300, \"durationS\": 25, "
        "\"events\": \"\", \"handoff\": null, \"implementation\": \"engine\", \"levels\": {\"speaker0\": {\"meanLevel\": 0.5, "
        "\"speakingFraction\": 0.25, \"updates\": 200}}, \"profile\": \"wifi\", \"rejoins\": 1, \"repetition\": 2, "
        "\"scenario\": \"tiles\", \"schema\": 1, \"server\": {\"bytes\": {\"audio\": 300000}, \"requests\": {\"audio\": "
        "{\"success\": 25}}}, \"video\": {\"ep-0\": {\"droppedFrames\": 3, \"frames\": 600, \"latencyMaxMs\": 3520, "
        "\"latencyMedianMs\": 3500, \"outOfOrderFrames\": 0, \"repeatedFrames\": 0, \"stallCount\": 0, \"stallMs\": 0, "
        "\"startMs\": 3400, \"unplacedFrames\": 1, \"widths\": {\"320\": 600}}}}");
}

void testAHandoffReport() {
    broadcast::BroadcastReportInput input;
    input.scenario = "handoff";
    input.profile = "lte";
    input.implementation = "player";
    broadcast::HandoffReport handoff;
    handoff.promotedAtMs = 12014;
    handoff.gapBlocks = 11;
    handoff.overlapBlocks = 0;
    handoff.rtcFirstHeardMs = 12130;
    input.handoff = handoff;
    const std::string json = broadcast::broadcastReportJson(input);
    CHECK_TRUE(json.find("\"handoff\": {\"gapBlocks\": 11, \"overlapBlocks\": 0, \"promotedAtMs\": 12014, \"rtcFirstHeardMs\": 12130}") != std::string::npos);
    CHECK_TRUE(json.find("\"implementation\": \"player\"") != std::string::npos);
}

void testAStreamReadFromTheController() {
    broadcast::BroadcastReportInput input;
    input.scenario = "unified";
    input.profile = "3g";
    input.implementation = "player";
    broadcast::ControllerPlayback playback;
    playback.startMs = 150;
    playback.stallCount = 0;
    playback.stallMs = 0;
    playback.latencyMedianMs = 3600;
    input.unifiedController = playback;
    const std::string json = broadcast::broadcastReportJson(input);
    CHECK_TRUE(json.find("\"audio\": {}") != std::string::npos);
    CHECK_TRUE(json.find("\"video\": {\"unified\": {\"latencyMedianMs\": 3600, \"stallCount\": 0, \"stallMs\": 0, \"startMs\": 150}}") != std::string::npos);
}

} // namespace

int main() {
    testAListenerReport();
    testAHandoffReport();
    testAStreamReadFromTheController();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
