#include "broadcast_signals.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace broadcast {

namespace {

constexpr int kAlphabet = 7;
constexpr int kOrder = 3;
constexpr int64_t kBlockMs = 10;
constexpr double kDetectionThreshold = 0.25 * kToneAmplitude;
// A pitch change counts only once the new pitch holds for two blocks: a single misread block would otherwise add
// two steps and corrupt the next three positions.
constexpr int kBlocksToConfirmStep = 2;
constexpr int kMissingBlocksToReset = 3;

int64_t floorDiv(int64_t value, int64_t divisor) {
    int64_t quotient = value / divisor;
    if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) {
        quotient -= 1;
    }
    return quotient;
}

void deBruijnStep(int alphabet, int order, int t, int p, std::vector<int> &a, std::vector<int> &sequence) {
    if (t > order) {
        if (order % p == 0) {
            for (int j = 1; j <= p; j++) {
                sequence.push_back(a[j]);
            }
        }
    } else {
        a[t] = a[t - p];
        deBruijnStep(alphabet, order, t + 1, p, a, sequence);
        for (int j = a[t - p] + 1; j < alphabet; j++) {
            a[t] = j;
            deBruijnStep(alphabet, order, t + 1, t, a, sequence);
        }
    }
}

// The de Bruijn sequence B(alphabet, order), by the standard Lyndon-word construction (go_sfu's deBruijn is the
// same, so both sides build the same slice order).
std::vector<int> deBruijnSequence(int alphabet, int order) {
    std::vector<int> a((size_t)(alphabet * order + 1), 0);
    std::vector<int> sequence;
    deBruijnStep(alphabet, order, 1, 1, a, sequence);
    return sequence;
}

// The difference sequence d (B(7, 3)), its prefix sums, and the position of every cyclic triple of differences.
struct MarkerTables {
    std::vector<int> differences;
    std::vector<int64_t> prefix;
    int64_t periodSum = 0;
    std::array<int, kAlphabet * kAlphabet * kAlphabet> positionOfTriple{};

    MarkerTables() {
        differences = deBruijnSequence(kAlphabet, kOrder);
        prefix.resize(differences.size() + 1, 0);
        for (size_t i = 0; i < differences.size(); i++) {
            prefix[i + 1] = prefix[i] + differences[i];
        }
        periodSum = prefix[differences.size()];
        int count = (int)differences.size();
        for (int n = 0; n < count; n++) {
            int d1 = differences[(n - 2 + count) % count];
            int d2 = differences[(n - 1 + count) % count];
            int d3 = differences[n];
            positionOfTriple[(d1 * kAlphabet + d2) * kAlphabet + d3] = n;
        }
    }
};

MarkerTables const &markerTables() {
    static MarkerTables tables;
    return tables;
}

// The fixture slice order: s_n = s_(n-1) + 1 + d_(n mod 81) (mod 10) over one period, s_0 = 0, d = B(9, 2); and the
// position (mod 81) of every cyclic pair of differences.
constexpr int kSliceOrderSteps = 81;

struct SliceOrderTables {
    std::array<int, kFixtureSliceOrderPeriod> order{};
    std::array<int, (kFixtureSlices - 1) * (kFixtureSlices - 1)> positionOfPair{};

    SliceOrderTables() {
        std::vector<int> differences = deBruijnSequence(kFixtureSlices - 1, 2);
        for (int n = 1; n < kFixtureSliceOrderPeriod; n++) {
            order[n] = (order[n - 1] + 1 + differences[n % kSliceOrderSteps]) % kFixtureSlices;
        }
        for (int n = 0; n < kSliceOrderSteps; n++) {
            int d1 = differences[(n - 1 + kSliceOrderSteps) % kSliceOrderSteps];
            int d2 = differences[n];
            positionOfPair[d1 * (kFixtureSlices - 1) + d2] = n;
        }
    }
};

SliceOrderTables const &sliceOrderTables() {
    static SliceOrderTables tables;
    return tables;
}

} // namespace

int markerSymbolAt(int64_t generationMs) {
    MarkerTables const &tables = markerTables();
    // s_n = sum over i = 0..n of (1 + d_i), mod 8, so adjacent symbols differ by 1 + d_n, which is 1..7.
    int64_t n = floorDiv(generationMs, kMarkerStepMs);
    int64_t terms = n + 1;
    int64_t periods = floorDiv(terms, kMarkerPeriodSteps);
    int64_t remainder = terms - periods * kMarkerPeriodSteps;
    int64_t total = terms + periods * tables.periodSum + tables.prefix[(size_t)remainder];
    return (int)(((total % 8) + 8) % 8);
}

int fixtureSliceAt(int64_t partIndex) {
    int64_t position = ((partIndex % kFixtureSliceOrderPeriod) + kFixtureSliceOrderPeriod) % kFixtureSliceOrderPeriod;
    return sliceOrderTables().order[(size_t)position];
}

std::optional<int> locateFixturePart(int a, int b, int c) {
    if (a < 0 || a >= kFixtureSlices || b < 0 || b >= kFixtureSlices || c < 0 || c >= kFixtureSlices) {
        return std::nullopt;
    }
    // A step is 1 + d with d in 0..8, so a repeated slice (a step of 0 mod 10) is no step of the order.
    int d1 = (b - a - 1 + 2 * kFixtureSlices) % kFixtureSlices;
    int d2 = (c - b - 1 + 2 * kFixtureSlices) % kFixtureSlices;
    if (d1 == kFixtureSlices - 1 || d2 == kFixtureSlices - 1) {
        return std::nullopt;
    }
    SliceOrderTables const &tables = sliceOrderTables();
    int position = tables.positionOfPair[d1 * (kFixtureSlices - 1) + d2];
    // The two halves of the period share the steps; their slices differ by 5.
    for (int candidate : {position, position + kSliceOrderSteps}) {
        if (tables.order[(size_t)candidate] == c) {
            return candidate;
        }
    }
    return std::nullopt;
}

SpeakerTone SpeakerTone::fixed(double frequency) {
    SpeakerTone tone;
    tone.kind = Kind::Fixed;
    tone.frequency = frequency;
    return tone;
}

SpeakerTone SpeakerTone::marker() {
    SpeakerTone tone;
    tone.kind = Kind::Marker;
    tone.frequency = 0.0;
    return tone;
}

double SpeakerTone::frequencyAt(int64_t nowMs) const {
    if (kind == Kind::Marker) {
        return kMarkerFrequencies[markerSymbolAt(nowMs)];
    }
    return frequency;
}

ToneDetector::ToneDetector(std::vector<double> frequencies) :
_frequencies(std::move(frequencies)) {
}

std::vector<double> ToneDetector::amplitudes(const int16_t *mono48k, size_t samples) const {
    std::vector<double> result;
    result.reserve(_frequencies.size());
    for (double frequency : _frequencies) {
        double coefficient = 2.0 * std::cos(2.0 * M_PI * frequency / 48000.0);
        double s1 = 0.0;
        double s2 = 0.0;
        for (size_t i = 0; i < samples; i++) {
            double s0 = (double)mono48k[i] + coefficient * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
        result.push_back(samples == 0 ? 0.0 : 2.0 * std::sqrt(std::max(0.0, power)) / (double)samples);
    }
    return result;
}

std::vector<bool> ToneDetector::detect(const int16_t *mono48k, size_t samples) const {
    std::vector<double> values = amplitudes(mono48k, samples);
    std::vector<bool> result;
    result.reserve(values.size());
    for (double value : values) {
        result.push_back(value >= kDetectionThreshold);
    }
    return result;
}

int strongestMarkerSymbol(std::vector<double> const &amplitudes) {
    int best = -1;
    double bestValue = kDetectionThreshold;
    for (int i = 0; i < 8 && i < (int)amplitudes.size(); i++) {
        if (amplitudes[i] >= bestValue) {
            best = i;
            bestValue = amplitudes[i];
        }
    }
    return best;
}

int32_t RealTimePacer::nextWaitUs() {
    int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (_startUs < 0) {
        _startUs = nowUs;
    }
    _frames++;
    int64_t dueUs = _startUs + _frames * kBlockMs * 1000;
    return (int32_t)std::max<int64_t>(0, dueUs - nowUs);
}

void MarkerDecoder::push(int64_t blockEndMs, int symbolOrMinus1) {
    if (symbolOrMinus1 < 0) {
        _missingBlocks++;
        _candidateBlocks = 0;
        if (_missingBlocks >= kMissingBlocksToReset && (!_steps.empty() || _latencyMs)) {
            _steps.clear();
            _latencyMs.reset();
            _updates++;
        }
        return;
    }
    _missingBlocks = 0;

    if (!_steps.empty() && _steps.back().symbol == symbolOrMinus1) {
        _candidateBlocks = 0;
        return;
    }
    if (_candidateBlocks > 0 && _candidateSymbol == symbolOrMinus1) {
        _candidateBlocks++;
    } else {
        _candidateSymbol = symbolOrMinus1;
        _candidateStartMs = blockEndMs - kBlockMs;
        _candidateBlocks = 1;
    }
    if (_candidateBlocks < kBlocksToConfirmStep) {
        return;
    }
    _candidateBlocks = 0;
    _steps.push_back(Step{_candidateSymbol, _candidateStartMs});
    if (_steps.size() > 4) {
        _steps.erase(_steps.begin());
    }
    if (_steps.size() < 4) {
        return;
    }

    auto difference = [this](size_t k) {
        return (_steps[k].symbol - _steps[k - 1].symbol - 1 + 16) % 8;
    };
    int d1 = difference(1);
    int d2 = difference(2);
    int d3 = difference(3);
    if (d1 >= kAlphabet || d2 >= kAlphabet || d3 >= kAlphabet) {
        _latencyMs.reset();
        _updates++;
        return;
    }
    int position = markerTables().positionOfTriple[(d1 * kAlphabet + d2) * kAlphabet + d3];
    int64_t period = (int64_t)kMarkerPeriodSteps * kMarkerStepMs;
    int64_t latency = (_steps[3].startMs - (int64_t)position * kMarkerStepMs) % period;
    if (latency < 0) {
        latency += period;
    }
    _latencyMs = latency;
    _updates++;
}

std::optional<int64_t> MarkerDecoder::latencyMs() const {
    return _latencyMs;
}

uint64_t MarkerDecoder::updates() const {
    return _updates;
}

} // namespace broadcast
