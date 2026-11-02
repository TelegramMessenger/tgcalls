#pragma once
// The broadcast testbench's report (schema 1), as broadcast mode writes it for the old engine and the new player's
// runner (telegram-ios Tests/BroadcastRunner) writes it for the app-side player: the same keys, so
// broadcast_report.py compares one against the other.

#include <map>
#include <optional>
#include <string>

#include "broadcast_metrics.h"

namespace broadcast {

// A stream's playback read from the player's controller (the new player's streams render through a player the
// testbench cannot run): written as video.unified's start, stalls and distance from live, in ms from the start.
struct ControllerPlayback {
    std::optional<double> startMs;
    int stallCount = 0;
    double stallMs = 0.0;
    std::optional<double> latencyMedianMs;
};

struct BroadcastReportInput {
    std::string scenario;
    std::string profile;
    std::string events;
    std::string implementation = "engine";
    int availabilityDelayMs = 0;
    int durationS = 0;
    int repetition = 0;
    int rejoins = 0;
    AudioReport audio;
    std::map<std::string, VideoReport> video;
    std::map<std::string, LevelStats> levels;
    std::optional<HandoffReport> handoff;
    // GoSfu_BroadcastStats' JSON.
    std::string serverStatsJson = "{}";
    // When set, `video.unified` holds only the controller's four values and `audio` is empty.
    std::optional<ControllerPlayback> unifiedController;
};

// The report as one line of JSON, keys sorted.
std::string broadcastReportJson(BroadcastReportInput const &input);

} // namespace broadcast
