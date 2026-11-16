#pragma once

// Time-coded speaker tones for the broadcast testbench, and their detector.
//
// Speaker 0 emits the marker: a tone whose pitch steps every 100 ms of its sender's clock through a sequence in
// which every step changes pitch and any three consecutive pitch differences occur once per period. A listener that
// hears it can tell when the audio it plays was generated, so its playout latency (the distance from live) and, at a
// broadcast-to-WebRTC handoff, which copy it hears, are measurable from the audio alone. Speakers 1-3 emit fixed
// tones, so the listener can also tell who it hears.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace broadcast {

constexpr double kMarkerFrequencies[8] = {500, 700, 900, 1100, 1300, 1500, 1700, 1900};
// Speakers 1-3; speaker 0 is the marker.
constexpr double kSpeakerFrequencies[3] = {2300, 2900, 3500};
// The unified fixture's AAC tone.
constexpr double kUnifiedFrequency = 1000;
constexpr double kToneAmplitude = 3000.0;
constexpr int64_t kMarkerStepMs = 100;
// The difference sequence is the de Bruijn sequence B(7, 3).
constexpr int kMarkerPeriodSteps = 343;

// The marker symbol (an index into kMarkerFrequencies) of the 100 ms step containing `generationMs`.
// Adjacent steps always differ.
int markerSymbolAt(int64_t generationMs);

// The order the emulated server shows the fixture's 1 s slices in (go_sfu broadcast_fixtures.go builds the same
// one): every step changes slice and any two consecutive steps occur once per 81 parts (differences from the de
// Bruijn sequence B(9, 2)), so three consecutive parts locate a part modulo kFixtureSliceOrderPeriod; the counter
// alone, which repeats every 10 s, would alias a listener more than 10 s behind live.
constexpr int kFixtureSlices = 10;
constexpr int64_t kFixtureSliceOrderPeriod = 162;

// The slice part `partIndex` (its timestamp / 1000) shows.
int fixtureSliceAt(int64_t partIndex);
// The index modulo kFixtureSliceOrderPeriod of the last of three consecutive parts that showed slices a, b, c, or
// nullopt when no three consecutive parts show them.
std::optional<int> locateFixturePart(int a, int b, int c);

struct SpeakerTone {
    enum class Kind { Fixed, Marker };

    Kind kind = Kind::Fixed;
    double frequency = 440.0;

    static SpeakerTone fixed(double frequency);
    static SpeakerTone marker();

    // The tone's frequency at `nowMs` of its sender's clock.
    double frequencyAt(int64_t nowMs) const;
};

// Goertzel detection of a set of tones in one 10 ms block of 48 kHz mono audio.
class ToneDetector {
public:
    explicit ToneDetector(std::vector<double> frequencies);

    // Each frequency's amplitude in the block, on the int16 scale (a full-block tone of amplitude A reads ~A).
    std::vector<double> amplitudes(const int16_t *mono48k, size_t samples) const;
    // Each frequency present: amplitude ≥ 0.25 · kToneAmplitude.
    std::vector<bool> detect(const int16_t *mono48k, size_t samples) const;

private:
    std::vector<double> _frequencies;
};

// The marker symbol with the largest amplitude among the first 8 entries of `amplitudes` (the kMarkerFrequencies
// order), or -1 when none reaches the detection threshold.
int strongestMarkerSymbol(std::vector<double> const &amplitudes);

// Waits for tgcalls' FakeAudioDeviceModule, which sleeps the returned wait after each 10 ms frame: deadline-based,
// so frames come at real time whatever each frame costs (a fixed 10 ms wait runs slow by that cost, and a speaker's
// RTP clock would drift behind the server's).
class RealTimePacer {
public:
    // Microseconds until the next frame is due (0 when late); the first call starts the schedule.
    int32_t nextWaitUs();

private:
    int64_t _startUs = -1;
    int64_t _frames = 0;
};

// Turns the per-block strongest marker symbol into the playout latency.
class MarkerDecoder {
public:
    // One 10 ms block that ended at `blockEndMs` of the playout clock; `symbolOrMinus1` from strongestMarkerSymbol.
    // An isolated -1 block is skipped; three or more in a row reset the decoder.
    void push(int64_t blockEndMs, int symbolOrMinus1);

    // Playout clock minus generation clock for the current step, once four consecutive steps have been seen.
    std::optional<int64_t> latencyMs() const;
    // Counts each recomputation of latencyMs() (a new step decoded, or a reset), so a caller can compare
    // consecutive decodes even when they are equal.
    uint64_t updates() const;

private:
    struct Step {
        int symbol;
        int64_t startMs;
    };

    std::vector<Step> _steps;
    int _missingBlocks = 0;
    int _candidateSymbol = -1;
    int64_t _candidateStartMs = 0;
    int _candidateBlocks = 0;
    std::optional<int64_t> _latencyMs;
    uint64_t _updates = 0;
};

} // namespace broadcast
