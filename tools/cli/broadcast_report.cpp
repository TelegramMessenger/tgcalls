#include "broadcast_report.h"

#include "third-party/json11.hpp"

namespace broadcast {

namespace {

json11::Json optionalJson(std::optional<int64_t> value) {
    return value ? json11::Json((double)*value) : json11::Json(nullptr);
}

json11::Json optionalJson(std::optional<double> value) {
    return value ? json11::Json(*value) : json11::Json(nullptr);
}

json11::Json audioJson(AudioReport const &report) {
    json11::Json::object heard;
    for (const auto &[key, value] : report.heardBlocks) {
        heard[key] = value;
    }
    return json11::Json::object{
        {"startMs", optionalJson(report.startMs)},
        {"stallCount", report.stallCount},
        {"stallMs", (double)report.stallMs},
        {"latencyMedianMs", optionalJson(report.latencyMedianMs)},
        {"latencyMaxMs", optionalJson(report.latencyMaxMs)},
        {"heardBlocks", heard},
    };
}

json11::Json videoJson(VideoReport const &report) {
    json11::Json::object widths;
    for (const auto &[width, count] : report.widths) {
        widths[std::to_string(width)] = count;
    }
    return json11::Json::object{
        {"startMs", optionalJson(report.startMs)},
        {"frames", report.frames},
        {"repeatedFrames", report.repeatedFrames},
        {"outOfOrderFrames", report.outOfOrderFrames},
        {"droppedFrames", report.droppedFrames},
        {"unplacedFrames", report.unplacedFrames},
        {"stallCount", report.stallCount},
        {"stallMs", (double)report.stallMs},
        {"latencyMedianMs", optionalJson(report.latencyMedianMs)},
        {"latencyMaxMs", optionalJson(report.latencyMaxMs)},
        {"widths", widths},
    };
}

json11::Json controllerJson(ControllerPlayback const &playback) {
    return json11::Json::object{
        {"startMs", optionalJson(playback.startMs)},
        {"stallCount", playback.stallCount},
        {"stallMs", playback.stallMs},
        {"latencyMedianMs", optionalJson(playback.latencyMedianMs)},
    };
}

} // namespace

std::string broadcastReportJson(BroadcastReportInput const &input) {
    json11::Json::object video;
    json11::Json audio;
    if (input.unifiedController) {
        video["unified"] = controllerJson(*input.unifiedController);
        audio = json11::Json::object{};
    } else {
        for (const auto &[endpoint, report] : input.video) {
            video[endpoint] = videoJson(report);
        }
        audio = audioJson(input.audio);
    }
    json11::Json::object levels;
    for (const auto &[name, stats] : input.levels) {
        levels[name] = json11::Json::object{
            {"meanLevel", stats.meanLevel},
            {"speakingFraction", stats.speakingFraction},
            {"updates", stats.updates},
        };
    }
    json11::Json handoff = nullptr;
    if (input.handoff) {
        handoff = json11::Json::object{
            {"promotedAtMs", (double)input.handoff->promotedAtMs},
            {"gapBlocks", input.handoff->gapBlocks},
            {"overlapBlocks", input.handoff->overlapBlocks},
            {"rtcFirstHeardMs", optionalJson(input.handoff->rtcFirstHeardMs)},
        };
    }
    std::string error;
    json11::Json server = json11::Json::parse(input.serverStatsJson, error);
    json11::Json result = json11::Json::object{
        {"schema", 1},
        {"implementation", input.implementation},
        {"scenario", input.scenario},
        {"profile", input.profile},
        {"events", input.events},
        {"availabilityDelayMs", input.availabilityDelayMs},
        {"durationS", input.durationS},
        {"repetition", input.repetition},
        {"rejoins", input.rejoins},
        {"audio", audio},
        {"video", video},
        {"levels", levels},
        {"handoff", handoff},
        {"server", server},
    };
    return result.dump();
}

} // namespace broadcast
