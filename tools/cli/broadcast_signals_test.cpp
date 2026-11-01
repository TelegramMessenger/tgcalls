#include "broadcast_signals.h"

#include <opus/opus.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <tuple>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

constexpr int kSampleRate = 48000;
constexpr int kBlockSamples = 480;
constexpr int64_t kOffsetMs = 3270;

// Phase-continuous synthesis of the given tones, sample by sample on the playout clock.
// The generation clock of every tone is the playout clock minus `offsetMs`.
std::vector<int16_t> synthesize(std::vector<broadcast::SpeakerTone> const &tones, int64_t durationMs, int64_t offsetMs) {
    std::vector<int16_t> samples((size_t)(durationMs * kSampleRate / 1000));
    std::vector<double> phases(tones.size(), 0.0);
    for (size_t i = 0; i < samples.size(); i++) {
        int64_t playoutMs = (int64_t)(i * 1000 / kSampleRate);
        double value = 0.0;
        for (size_t t = 0; t < tones.size(); t++) {
            double frequency = tones[t].frequencyAt(playoutMs - offsetMs);
            phases[t] += 2.0 * M_PI * frequency / kSampleRate;
            value += broadcast::kToneAmplitude * std::sin(phases[t]);
        }
        samples[i] = (int16_t)std::lround(std::max(-32768.0, std::min(32767.0, value)));
    }
    return samples;
}

std::vector<int16_t> opusRoundTrip(std::vector<int16_t> const &input) {
    int error = 0;
    OpusEncoder *encoder = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_AUDIO, &error);
    OpusDecoder *decoder = opus_decoder_create(kSampleRate, 1, &error);
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(32000));
    std::vector<int16_t> output(input.size(), 0);
    const int frame = 960;
    unsigned char packet[4000];
    for (size_t offset = 0; offset + frame <= input.size(); offset += frame) {
        int length = opus_encode(encoder, input.data() + offset, frame, packet, sizeof(packet));
        CHECK_TRUE(length > 0);
        int decoded = opus_decode(decoder, packet, length, output.data() + offset, frame, 0);
        CHECK_TRUE(decoded == frame);
    }
    opus_encoder_destroy(encoder);
    opus_decoder_destroy(decoder);
    return output;
}

std::vector<double> detectorFrequencies() {
    std::vector<double> frequencies(std::begin(broadcast::kMarkerFrequencies), std::end(broadcast::kMarkerFrequencies));
    frequencies.insert(frequencies.end(), std::begin(broadcast::kSpeakerFrequencies), std::end(broadcast::kSpeakerFrequencies));
    return frequencies;
}

void testDifferenceSequenceIsComplete() {
    std::set<std::tuple<int, int, int>> triples;
    std::vector<int> symbols;
    for (int n = 0; n < broadcast::kMarkerPeriodSteps + 3; n++) {
        symbols.push_back(broadcast::markerSymbolAt((int64_t)n * broadcast::kMarkerStepMs));
    }
    for (size_t n = 1; n < symbols.size(); n++) {
        CHECK_TRUE(symbols[n] != symbols[n - 1]);
        CHECK_TRUE(symbols[n] >= 0 && symbols[n] < 8);
    }
    for (int n = 3; n < broadcast::kMarkerPeriodSteps + 3; n++) {
        auto diff = [&](int k) { return (symbols[k] - symbols[k - 1] - 1 + 16) % 8; };
        triples.insert(std::make_tuple(diff(n - 2), diff(n - 1), diff(n)));
    }
    CHECK_TRUE((int)triples.size() == broadcast::kMarkerPeriodSteps);
    CHECK_TRUE(broadcast::kMarkerPeriodSteps == 343);
}

// `delayMs` is what sits between synthesis and the detector (Opus' algorithmic delay is 6.5 ms at 48 kHz).
void checkLatency(std::vector<int16_t> const &samples, int64_t skipMs, bool expectSpeakers, double delayMs) {
    broadcast::ToneDetector detector(detectorFrequencies());
    broadcast::MarkerDecoder decoder;
    int checkedBlocks = 0;
    int wrongLatency = 0;
    int correctTones = 0;
    for (size_t offset = 0; offset + kBlockSamples <= samples.size(); offset += kBlockSamples) {
        int64_t blockEndMs = (int64_t)((offset + kBlockSamples) * 1000 / kSampleRate);
        std::vector<double> amplitudes = detector.amplitudes(samples.data() + offset, kBlockSamples);
        int symbol = broadcast::strongestMarkerSymbol(amplitudes);
        decoder.push(blockEndMs, symbol);
        if (blockEndMs <= skipMs) {
            continue;
        }
        checkedBlocks++;
        auto latency = decoder.latencyMs();
        if (!latency || std::abs((double)*latency - ((double)kOffsetMs + delayMs)) > 10.0) {
            wrongLatency++;
        }
        int expectedSymbol = broadcast::markerSymbolAt((int64_t)std::floor((double)blockEndMs - 5.0 - delayMs) - kOffsetMs);
        std::vector<bool> present = detector.detect(samples.data() + offset, kBlockSamples);
        bool ok = symbol == expectedSymbol;
        if (expectSpeakers) {
            ok = ok && present[8] && present[9] && !present[10];
        }
        if (ok) {
            correctTones++;
        }
    }
    CHECK_TRUE(checkedBlocks > 0);
    CHECK_TRUE(wrongLatency == 0);
    CHECK_TRUE(correctTones * 100 >= checkedBlocks * 98);
    if (wrongLatency != 0 || correctTones * 100 < checkedBlocks * 98) {
        std::printf("  blocks %d, wrong latency %d, correct tones %d\n", checkedBlocks, wrongLatency, correctTones);
    }
}

void testMarkerLatencyRoundTrip() {
    auto samples = synthesize({broadcast::SpeakerTone::marker()}, 10000, kOffsetMs);
    checkLatency(samples, 400, false, 0.0);
}

void testDetectionSurvivesOpus() {
    auto samples = synthesize({
        broadcast::SpeakerTone::marker(),
        broadcast::SpeakerTone::fixed(broadcast::kSpeakerFrequencies[0]),
        broadcast::SpeakerTone::fixed(broadcast::kSpeakerFrequencies[1])
    }, 10000, kOffsetMs);
    checkLatency(opusRoundTrip(samples), 400, true, 6.5);
}

void testSilenceDetectsNothing() {
    std::vector<int16_t> silence(kSampleRate, 0);
    broadcast::ToneDetector detector(detectorFrequencies());
    broadcast::MarkerDecoder decoder;
    for (size_t offset = 0; offset + kBlockSamples <= silence.size(); offset += kBlockSamples) {
        std::vector<bool> present = detector.detect(silence.data() + offset, kBlockSamples);
        for (bool value : present) {
            CHECK_TRUE(!value);
        }
        int symbol = broadcast::strongestMarkerSymbol(detector.amplitudes(silence.data() + offset, kBlockSamples));
        CHECK_TRUE(symbol == -1);
        decoder.push((int64_t)((offset + kBlockSamples) * 1000 / kSampleRate), symbol);
    }
    CHECK_TRUE(!decoder.latencyMs().has_value());
}

// Three consecutive parts' slices locate the last one modulo the order's period; slices no three consecutive parts
// show (a repeat, or a pair of steps from elsewhere in the order) locate nothing.
void testFixtureSliceOrderLocatesParts() {
    const int64_t period = broadcast::kFixtureSliceOrderPeriod;
    CHECK_TRUE(period == 162);
    int located = 0;
    for (int64_t n = 0; n < period; n++) {
        int a = broadcast::fixtureSliceAt(n - 2);
        int b = broadcast::fixtureSliceAt(n - 1);
        int c = broadcast::fixtureSliceAt(n);
        CHECK_TRUE(c >= 0 && c < broadcast::kFixtureSlices && a != b && b != c);
        std::optional<int> position = broadcast::locateFixturePart(a, b, c);
        located += (position.has_value() && *position == n) ? 1 : 0;
    }
    CHECK_TRUE(located == period);
    CHECK_TRUE(broadcast::fixtureSliceAt(1'700'000'003) == broadcast::fixtureSliceAt(1'700'000'003 + period));
    CHECK_TRUE(!broadcast::locateFixturePart(3, 3, 4).has_value());
    // The half check: the same two steps from the other half of the order, shifted by 5, still locate; shifted by
    // anything else they do not.
    int a = broadcast::fixtureSliceAt(10), b = broadcast::fixtureSliceAt(11), c = broadcast::fixtureSliceAt(12);
    std::optional<int> other = broadcast::locateFixturePart((a + 5) % 10, (b + 5) % 10, (c + 5) % 10);
    CHECK_TRUE(other.has_value() && *other == 12 + 81);
    CHECK_TRUE(!broadcast::locateFixturePart((a + 1) % 10, (b + 1) % 10, (c + 1) % 10).has_value());
}

} // namespace

int main() {
    testDifferenceSequenceIsComplete();
    testMarkerLatencyRoundTrip();
    testDetectionSurvivesOpus();
    testSilenceDetectsNothing();
    testFixtureSliceOrderLocatesParts();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
