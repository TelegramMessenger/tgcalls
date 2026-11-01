#pragma once

// What the broadcast testbench measures: a listener's playout (RecordingRenderer, a fake audio device renderer),
// its decoded video frames (CodedFrameSink) and the audio levels its engine reports (LevelRecorder). All times are
// wall-clock milliseconds (system_clock), the clock the speakers' marker tone is generated on; reports give them
// relative to the moment the listener entered broadcast.

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "FakeAudioDeviceModule.h"
#include "AudioFrame.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "group/GroupInstanceImpl.h"

#include "broadcast_signals.h"

namespace broadcast {

int64_t wallClockMs();

struct AudioReport {
    std::optional<int64_t> startMs;
    int stallCount = 0;
    int64_t stallMs = 0;
    std::optional<int64_t> latencyMedianMs;
    std::optional<int64_t> latencyMaxMs;
    // "marker", "2300", "2900", "1000": blocks in which each was heard.
    std::map<std::string, int> heardBlocks;
};

struct HandoffReport {
    int64_t promotedAtMs = 0;
    // Blocks with no marker between the promotion and the first block heard at WebRTC latency (below 1000 ms).
    int gapBlocks = 0;
    // Blocks in that window carrying two marker tones near full amplitude (both copies at once).
    int overlapBlocks = 0;
    std::optional<int64_t> rtcFirstHeardMs;
};

class RecordingRenderer : public tgcalls::FakeAudioDeviceModule::Renderer {
public:
    // `unified`: the expected tone is the unified fixture's 1000 Hz; otherwise the marker and speakers 1-2.
    explicit RecordingRenderer(bool unified);

    bool Render(const tgcalls::AudioFrame &frame) override;
    // Real-time playout (RealTimePacer): the fake device would otherwise play slow by each block's cost.
    int32_t WaitForUs() override;
    // One 10 ms block of interleaved playout that ended at `timeMs`.
    void renderAt(const int16_t *interleaved, size_t totalSamples, size_t channels, int64_t timeMs);

    // Over the blocks after `startMs`; latency is measured before `promotedAtMs` when given, and the handoff window
    // is not counted as stalls.
    AudioReport audioReport(int64_t startMs, std::optional<int64_t> promotedAtMs) const;
    HandoffReport handoffReport(int64_t startMs, int64_t promotedAtMs) const;

private:
    struct Block {
        int64_t timeMs;
        bool expected;     // any expected tone
        int marker;        // strongest marker symbol, -1 for none
        int fullMarkers;   // marker tones at ≥ 0.6 of full amplitude
        bool speaker1;
        bool speaker2;
        bool unifiedTone;
        std::optional<int64_t> latencyMs;
    };

    std::optional<int64_t> handoffEndMs(int64_t promotedAtMs) const;

    bool _unified;
    ToneDetector _detector;
    RealTimePacer _pacer;
    mutable std::mutex _mutex;
    MarkerDecoder _decoder;
    uint64_t _lastDecoderUpdates = 0;
    std::optional<int64_t> _lastRawLatency;
    std::optional<int64_t> _acceptedLatency;
    std::vector<Block> _blocks;
    std::vector<int16_t> _mono;
};

struct VideoReport {
    std::optional<int64_t> startMs;
    int frames = 0;
    int stallCount = 0;
    int64_t stallMs = 0;
    std::optional<int64_t> latencyMedianMs;
    std::optional<int64_t> latencyMaxMs;
    std::map<int, int> widths;
};

// The fixture's burned-in frame counter (bit k = the 16x16 block at x = 16k in the top rows, luma > 128 = 1).
int readFrameCounter(webrtc::VideoFrame const &frame);

class CodedFrameSink : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    void OnFrame(const webrtc::VideoFrame &frame) override;
    void onFrameAt(const webrtc::VideoFrame &frame, int64_t arrivalMs);
    VideoReport report(int64_t startMs) const;

private:
    struct Frame {
        int64_t arrivalMs;
        int width;
        int counter;
    };
    mutable std::mutex _mutex;
    std::vector<Frame> _frames;
};

struct LevelStats {
    double meanLevel = 0.0;
    double speakingFraction = 0.0;
    int updates = 0;
};

class LevelRecorder {
public:
    void record(tgcalls::GroupLevelsUpdate const &update);
    // Keyed by the names given to the SSRCs (speakers' SSRCs differ from run to run; their roles do not).
    std::map<std::string, LevelStats> stats(std::map<uint32_t, std::string> const &names) const;

private:
    struct Sums {
        double level = 0.0;
        int voice = 0;
        int updates = 0;
    };
    mutable std::mutex _mutex;
    std::map<uint32_t, Sums> _sums;
};

} // namespace broadcast
