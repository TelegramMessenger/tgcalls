#pragma once

// `--mode broadcast`: today's engine as a broadcast listener against the Go SFU's emulated broadcast server, with the
// listener's playout, frames and levels measured into a JSON report (schema 1). Scenarios:
// - audio: three speakers in WebRTC (the marker, 2300 Hz, 2900 Hz), the listener hears them through broadcast.
// - tiles: the same, plus video tiles ep-0 (thumbnail), ep-1 (medium), ep-2 (full); at 10 s ep-0 and ep-2 swap.
// - unified: no speakers; an RTMP stream's listener (unified parts, video and a 1000 Hz tone).
// - handoff: as audio; at 12 s the listener is promoted to speaker and hears the speakers through WebRTC.

#include <string>

struct BroadcastRunConfig {
    std::string scenario = "audio";
    std::string profile = "wifi";
    std::string events;
    std::string reportPath;
    std::string fixtureDir = "submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/testdata/broadcast";
    // Seconds the listener spends in the call, from entering broadcast.
    int durationS = 25;
    int availabilityDelayMs = 300;
    int repetition = 0;
    bool quiet = false;
};

// Runs one scenario and exits the process (the CLI's teardown convention): 0 when the report was written.
int runBroadcastMode(const BroadcastRunConfig &config);
