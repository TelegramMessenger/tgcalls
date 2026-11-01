// What the broadcast testbench measures from a listener's playout and decoded frames.

#include "broadcast_metrics.h"

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
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

webrtc::VideoFrame codedFrame(int width, int height, int counter) {
    auto buffer = webrtc::I420Buffer::Create(width, height);
    webrtc::I420Buffer::SetBlack(buffer.get());
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < width; x++) {
            int bit = x / 16;
            buffer->MutableDataY()[y * buffer->StrideY() + x] = (bit < 16 && (counter & (1 << bit))) ? 235 : 16;
        }
    }
    return webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).build();
}

void testCounterWrapIsContinuous() {
    broadcast::CodedFrameSink sink;
    const int counters[] = {297, 298, 299, 0, 1, 2};
    int64_t arrival = 1'700'000'000'000;
    for (int counter : counters) {
        CHECK_TRUE(broadcast::readFrameCounter(codedFrame(320, 180, counter)) == counter);
        sink.onFrameAt(codedFrame(320, 180, counter), arrival);
        arrival += 33;
    }
    auto report = sink.report(1'700'000'000'000 - 500);
    CHECK_TRUE(report.frames == 6);
    CHECK_TRUE(report.stallCount == 0 && report.stallMs == 0);
    CHECK_TRUE(report.startMs.has_value() && *report.startMs == 500);
    CHECK_TRUE(report.widths.size() == 1 && report.widths.at(320) == 6);
    // Latency (now - counter·1000/30) mod 10 s: constant across the wrap, within the 33 vs 33.3 ms rounding.
    CHECK_TRUE(report.latencyMedianMs.has_value());
    CHECK_TRUE(report.latencyMaxMs.has_value() && *report.latencyMaxMs - *report.latencyMedianMs <= 2);
}

void testVideoStallIsAGapOver100Ms() {
    broadcast::CodedFrameSink sink;
    int64_t arrival = 1'700'000'000'000;
    for (int i = 0; i < 10; i++) {
        sink.onFrameAt(codedFrame(640, 360, i), arrival);
        arrival += (i == 4) ? 250 : 33;
    }
    auto report = sink.report(1'700'000'000'000);
    CHECK_TRUE(report.stallCount == 1 && report.stallMs == 250 - 33);
}

// Synthesizes one 10 ms stereo block of the given tones on the generation clock `generationMs`.
struct ToneSynth {
    double phases[4] = {0, 0, 0, 0};

    std::vector<int16_t> block(int64_t generationMs, bool marker, bool speakers, double markerAmplitude = 3000.0) {
        std::vector<int16_t> samples(480 * 2, 0);
        for (int i = 0; i < 480; i++) {
            double value = 0.0;
            int64_t sampleMs = generationMs + i / 48;
            if (marker) {
                phases[0] += 2.0 * M_PI * broadcast::SpeakerTone::marker().frequencyAt(sampleMs) / 48000.0;
                value += markerAmplitude * std::sin(phases[0]);
            }
            if (speakers) {
                phases[1] += 2.0 * M_PI * broadcast::kSpeakerFrequencies[0] / 48000.0;
                phases[2] += 2.0 * M_PI * broadcast::kSpeakerFrequencies[1] / 48000.0;
                value += 3000.0 * std::sin(phases[1]) + 3000.0 * std::sin(phases[2]);
            }
            int16_t sample = (int16_t)std::lround(value);
            samples[2 * i] = sample;
            samples[2 * i + 1] = sample;
        }
        return samples;
    }
};

void testRendererMeasuresStartStallsAndLatency() {
    broadcast::RecordingRenderer renderer(false);
    ToneSynth synth;
    const int64_t start = 1'700'000'000'000;
    int64_t t = start;
    auto render = [&](bool tones) {
        auto samples = synth.block(t - 3000, tones, tones);
        renderer.renderAt(samples.data(), samples.size(), 2, t + 10);
        t += 10;
    };
    for (int i = 0; i < 100; i++) render(false); // 1 s before anything plays
    for (int i = 0; i < 300; i++) render(true);
    for (int i = 0; i < 10; i++) render(false);  // a 100 ms stall
    for (int i = 0; i < 200; i++) render(true);
    auto report = renderer.audioReport(start, std::nullopt);
    CHECK_TRUE(report.startMs.has_value() && *report.startMs >= 1000 && *report.startMs <= 1020);
    CHECK_TRUE(report.stallCount == 1 && report.stallMs == 100);
    CHECK_TRUE(report.latencyMedianMs.has_value() && std::abs(*report.latencyMedianMs - 3000) <= 10);
    CHECK_TRUE(report.heardBlocks.at("marker") >= 490 && report.heardBlocks.at("2300") >= 490 && report.heardBlocks.at("1000") == 0);
}

void testHandoffCountsGapAndOverlap() {
    broadcast::RecordingRenderer renderer(false);
    ToneSynth broadcastCopy;
    ToneSynth rtcCopy;
    const int64_t start = 1'700'000'000'000;
    int64_t t = start;
    // 3 s of broadcast (3 s behind), promotion at 3 s, broadcast continues to 3.5 s, then 50 ms of nothing, then
    // 1.5 s of WebRTC audio (100 ms behind), of which the first 30 ms also carry the broadcast copy (an overlap).
    for (; t < start + 3500; t += 10) {
        auto samples = broadcastCopy.block(t - 3000, true, true);
        renderer.renderAt(samples.data(), samples.size(), 2, t + 10);
    }
    for (int i = 0; i < 5; i++, t += 10) {
        std::vector<int16_t> silence(960, 0);
        renderer.renderAt(silence.data(), silence.size(), 2, t + 10);
    }
    for (int i = 0; i < 150; i++, t += 10) {
        auto samples = rtcCopy.block(t - 100, true, true);
        if (i < 3) {
            auto old = broadcastCopy.block(t - 3000, true, false);
            for (size_t k = 0; k < samples.size(); k++) {
                samples[k] = (int16_t)std::max(-32768, std::min(32767, (int)samples[k] + (int)old[k]));
            }
        }
        renderer.renderAt(samples.data(), samples.size(), 2, t + 10);
    }
    auto handoff = renderer.handoffReport(start, start + 3000);
    CHECK_TRUE(handoff.promotedAtMs == 3000);
    CHECK_TRUE(handoff.gapBlocks == 5);
    CHECK_TRUE(handoff.overlapBlocks >= 2 && handoff.overlapBlocks <= 3);
    CHECK_TRUE(handoff.rtcFirstHeardMs.has_value() && *handoff.rtcFirstHeardMs > 3550 && *handoff.rtcFirstHeardMs < 4100);
    // The broadcast's distance from live is measured before the promotion, and the gap is not a stall.
    auto audio = renderer.audioReport(start, start + 3000);
    CHECK_TRUE(audio.latencyMedianMs.has_value() && std::abs(*audio.latencyMedianMs - 3000) <= 10);
    CHECK_TRUE(audio.stallCount == 0);
}

void testLevelsPerSpeaker() {
    broadcast::LevelRecorder levels;
    tgcalls::GroupLevelsUpdate update;
    update.updates.push_back(tgcalls::GroupLevelUpdate{1001, tgcalls::GroupLevelValue{0.2f, true, false}});
    update.updates.push_back(tgcalls::GroupLevelUpdate{1002, tgcalls::GroupLevelValue{0.0f, false, false}});
    levels.record(update);
    update.updates[0].value.level = 0.4f;
    update.updates[0].value.voice = false;
    levels.record(update);
    auto stats = levels.stats({{1001, "speaker0"}, {1002, "speaker1"}});
    CHECK_TRUE(std::abs(stats.at("speaker0").meanLevel - 0.3) < 1e-6);
    CHECK_TRUE(std::abs(stats.at("speaker0").speakingFraction - 0.5) < 1e-6);
    CHECK_TRUE(stats.at("speaker1").speakingFraction == 0.0);
}

// The fake audio device sleeps the returned wait after each frame; with deadline-based waits 50 frames take 500 ms
// whatever each frame's own processing costs (3 ms here, which a fixed 10 ms wait would turn into 650 ms).
void testPacerKeepsTenMillisecondPeriod() {
    broadcast::RealTimePacer pacer;
    auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        std::this_thread::sleep_for(std::chrono::microseconds(pacer.nextWaitUs()));
    }
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    CHECK_TRUE(elapsedMs >= 480 && elapsedMs <= 540);
    if (elapsedMs < 480 || elapsedMs > 540) {
        std::printf("  50 paced frames took %lld ms\n", (long long)elapsedMs);
    }
}

} // namespace

int main() {
    testPacerKeepsTenMillisecondPeriod();
    testCounterWrapIsContinuous();
    testVideoStallIsAGapOver100Ms();
    testRendererMeasuresStartStallsAndLatency();
    testHandoffCountsGapAndOverlap();
    testLevelsPerSpeaker();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
