#include "broadcast_metrics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

namespace broadcast {

namespace {

constexpr int64_t kBlockMs = 10;
constexpr double kPresentAmplitude = 0.25 * kToneAmplitude;
constexpr double kFullAmplitude = 0.6 * kToneAmplitude;
constexpr int kMinStallBlocks = 3;
constexpr int64_t kRtcLatencyMs = 1000;
// Two consecutive decoded latencies must agree this closely before one is believed: right after a handoff the
// decoder sees steps of both copies, and a triple mixing them decodes to an arbitrary latency.
constexpr int64_t kLatencyAgreementMs = 20;
constexpr int64_t kVideoStallMs = 100;
constexpr int64_t kVideoFrameMs = 33;
constexpr int kFixtureFramesPerSlice = 30;
constexpr double kFixtureOrderPeriodMs = kFixtureSliceOrderPeriod * 1000.0;

// Detector order: the 8 marker frequencies, speaker 1, speaker 2, the unified tone.
constexpr size_t kSpeaker1Index = 8;
constexpr size_t kSpeaker2Index = 9;
constexpr size_t kUnifiedIndex = 10;

std::vector<double> detectorFrequencies() {
    std::vector<double> frequencies(std::begin(kMarkerFrequencies), std::end(kMarkerFrequencies));
    frequencies.push_back(kSpeakerFrequencies[0]);
    frequencies.push_back(kSpeakerFrequencies[1]);
    frequencies.push_back(kUnifiedFrequency);
    return frequencies;
}

template <typename T>
std::optional<T> median(std::vector<T> values) {
    if (values.empty()) {
        return std::nullopt;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

} // namespace

int64_t wallClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

RecordingRenderer::RecordingRenderer(bool unified) :
_unified(unified),
_detector(detectorFrequencies()) {
}

bool RecordingRenderer::Render(const tgcalls::AudioFrame &frame) {
    // num_samples is the interleaved total (AudioTransport's nSamplesOut).
    renderAt(frame.audio_samples, frame.num_samples, std::max<size_t>(1, frame.num_channels), wallClockMs());
    return true;
}

int32_t RecordingRenderer::WaitForUs() {
    return _pacer.nextWaitUs();
}

void RecordingRenderer::renderAt(const int16_t *interleaved, size_t totalSamples, size_t channels, int64_t timeMs) {
    size_t frames = totalSamples / channels;
    std::lock_guard<std::mutex> lock(_mutex);
    _mono.resize(frames);
    for (size_t i = 0; i < frames; i++) {
        int sum = 0;
        for (size_t c = 0; c < channels; c++) {
            sum += interleaved[i * channels + c];
        }
        _mono[i] = (int16_t)(sum / (int)channels);
    }
    std::vector<double> amplitudes = _detector.amplitudes(_mono.data(), frames);

    Block block;
    block.timeMs = timeMs;
    block.marker = strongestMarkerSymbol(amplitudes);
    block.fullMarkers = 0;
    for (size_t i = 0; i < 8; i++) {
        if (amplitudes[i] >= kFullAmplitude) {
            block.fullMarkers++;
        }
    }
    block.speaker1 = amplitudes[kSpeaker1Index] >= kPresentAmplitude;
    block.speaker2 = amplitudes[kSpeaker2Index] >= kPresentAmplitude;
    block.unifiedTone = amplitudes[kUnifiedIndex] >= kPresentAmplitude;
    block.expected = _unified ? block.unifiedTone : (block.marker >= 0 || block.speaker1 || block.speaker2);
    if (!_unified) {
        _decoder.push(timeMs, block.marker);
        if (_decoder.updates() != _lastDecoderUpdates) {
            _lastDecoderUpdates = _decoder.updates();
            std::optional<int64_t> raw = _decoder.latencyMs();
            if (raw && _lastRawLatency && std::abs(*raw - *_lastRawLatency) <= kLatencyAgreementMs) {
                _acceptedLatency = raw;
            } else {
                _acceptedLatency.reset();
            }
            _lastRawLatency = raw;
        }
        block.latencyMs = _acceptedLatency;
    }
    _blocks.push_back(block);
}

std::optional<int64_t> RecordingRenderer::handoffEndMs(int64_t promotedAtMs) const {
    for (const auto &block : _blocks) {
        if (block.timeMs >= promotedAtMs && block.latencyMs && *block.latencyMs < kRtcLatencyMs) {
            return block.timeMs;
        }
    }
    return std::nullopt;
}

AudioReport RecordingRenderer::audioReport(int64_t startMs, std::optional<int64_t> promotedAtMs) const {
    std::lock_guard<std::mutex> lock(_mutex);
    AudioReport report;
    report.heardBlocks = {{"marker", 0}, {"2300", 0}, {"2900", 0}, {"1000", 0}};
    std::optional<int64_t> windowEnd;
    if (promotedAtMs) {
        windowEnd = handoffEndMs(*promotedAtMs);
        if (!windowEnd) {
            windowEnd = INT64_MAX;
        }
    }
    auto inWindow = [&](int64_t timeMs) {
        return promotedAtMs && timeMs >= *promotedAtMs && timeMs <= *windowEnd;
    };

    std::vector<int64_t> latencies;
    int silentRun = 0;
    for (const auto &block : _blocks) {
        if (block.timeMs <= startMs) {
            continue;
        }
        report.heardBlocks["marker"] += block.marker >= 0 ? 1 : 0;
        report.heardBlocks["2300"] += block.speaker1 ? 1 : 0;
        report.heardBlocks["2900"] += block.speaker2 ? 1 : 0;
        report.heardBlocks["1000"] += block.unifiedTone ? 1 : 0;
        if (!report.startMs) {
            if (block.expected) {
                report.startMs = block.timeMs - startMs;
            }
            continue;
        }
        if (block.latencyMs && (!promotedAtMs || block.timeMs < *promotedAtMs)) {
            latencies.push_back(*block.latencyMs);
        }
        if (!block.expected && !inWindow(block.timeMs)) {
            silentRun++;
        } else {
            if (silentRun >= kMinStallBlocks) {
                report.stallCount++;
                report.stallMs += silentRun * kBlockMs;
            }
            silentRun = 0;
        }
    }
    if (silentRun >= kMinStallBlocks) {
        report.stallCount++;
        report.stallMs += silentRun * kBlockMs;
    }
    report.latencyMedianMs = median(latencies);
    if (!latencies.empty()) {
        report.latencyMaxMs = *std::max_element(latencies.begin(), latencies.end());
    }
    return report;
}

HandoffReport RecordingRenderer::handoffReport(int64_t startMs, int64_t promotedAtMs) const {
    std::lock_guard<std::mutex> lock(_mutex);
    HandoffReport report;
    report.promotedAtMs = promotedAtMs - startMs;
    std::optional<int64_t> end = handoffEndMs(promotedAtMs);
    if (end) {
        report.rtcFirstHeardMs = *end - startMs;
    }
    for (const auto &block : _blocks) {
        if (block.timeMs < promotedAtMs || (end && block.timeMs > *end)) {
            continue;
        }
        if (block.marker < 0) {
            report.gapBlocks++;
        }
        if (block.fullMarkers >= 2) {
            report.overlapBlocks++;
        }
    }
    return report;
}

int readFrameCounter(webrtc::VideoFrame const &frame) {
    auto i420 = frame.video_frame_buffer()->ToI420();
    if (!i420 || i420->width() < 16 * 16 || i420->height() < 16) {
        return -1;
    }
    int value = 0;
    for (int bit = 0; bit < 16; bit++) {
        if (i420->DataY()[8 * i420->StrideY() + 16 * bit + 8] > 128) {
            value |= 1 << bit;
        }
    }
    return value;
}

void CodedFrameSink::OnFrame(const webrtc::VideoFrame &frame) {
    onFrameAt(frame, wallClockMs());
}

void CodedFrameSink::onFrameAt(const webrtc::VideoFrame &frame, int64_t arrivalMs) {
    Frame entry{arrivalMs, frame.width(), readFrameCounter(frame)};
    std::lock_guard<std::mutex> lock(_mutex);
    _frames.push_back(entry);
}

VideoReport CodedFrameSink::report(int64_t startMs) const {
    std::lock_guard<std::mutex> lock(_mutex);
    VideoReport report;
    std::vector<Frame> frames;
    for (const auto &frame : _frames) {
        if (frame.arrivalMs >= startMs) {
            frames.push_back(frame);
        }
    }
    for (size_t i = 0; i < frames.size(); i++) {
        report.widths[frames[i].width]++;
        if (!report.startMs) {
            report.startMs = frames[i].arrivalMs - startMs;
        }
        if (i > 0) {
            int64_t gap = frames[i].arrivalMs - frames[i - 1].arrivalMs;
            if (gap > kVideoStallMs) {
                report.stallCount++;
                report.stallMs += gap - kVideoFrameMs;
            }
        }
    }

    // Runs of one slice (consecutive parts always show different slices, so a run is one part, shown once or more).
    struct Run {
        int slice;
        std::vector<size_t> frames;
    };
    std::vector<Run> runs;
    for (size_t i = 0; i < frames.size(); i++) {
        int counter = frames[i].counter;
        if (counter < 0 || counter >= kFixtureSlices * kFixtureFramesPerSlice) {
            report.unplacedFrames++;
            continue;
        }
        int slice = counter / kFixtureFramesPerSlice;
        if (runs.empty() || runs.back().slice != slice) {
            runs.push_back(Run{slice, {}});
        }
        runs.back().frames.push_back(i);
    }

    // Window i (runs i-2, i-1, i) locates run i; a window counts only when the next one locates the next part, and
    // then the four runs both cover are placed. A run placed at two different parts is not placed.
    std::vector<std::optional<int>> located(runs.size());
    for (size_t i = 2; i < runs.size(); i++) {
        located[i] = locateFixturePart(runs[i - 2].slice, runs[i - 1].slice, runs[i].slice);
    }
    std::vector<std::optional<int64_t>> runPart(runs.size());
    std::vector<bool> conflicting(runs.size(), false);
    for (size_t i = 2; i + 1 < runs.size(); i++) {
        if (!located[i] || !located[i + 1] || *located[i + 1] != (*located[i] + 1) % kFixtureSliceOrderPeriod) {
            continue;
        }
        for (size_t j = 0; j < 4; j++) {
            int64_t part = ((*located[i] - 2 + (int64_t)j) % kFixtureSliceOrderPeriod + kFixtureSliceOrderPeriod) %
                kFixtureSliceOrderPeriod;
            size_t run = i - 2 + j;
            if (runPart[run] && *runPart[run] != part) {
                conflicting[run] = true;
            }
            runPart[run] = part;
        }
    }

    // Placed frames, in arrival order: latency = arrival minus media time, modulo the order's period; the media frame
    // index (arrival - latency in frames) tells repeats, frames out of order and frames skipped.
    std::vector<int64_t> latencies;
    std::set<int64_t> shown;
    std::optional<int64_t> latestShown;
    for (size_t r = 0; r < runs.size(); r++) {
        if (!runPart[r] || conflicting[r]) {
            report.unplacedFrames += (int)runs[r].frames.size();
            continue;
        }
        for (size_t index : runs[r].frames) {
            Frame const &frame = frames[index];
            double mediaMs = *runPart[r] * 1000.0 + (frame.counter % kFixtureFramesPerSlice) * 1000.0 / 30.0;
            double latency = std::fmod((double)frame.arrivalMs - mediaMs, kFixtureOrderPeriodMs);
            if (latency < 0) {
                latency += kFixtureOrderPeriodMs;
            }
            latencies.push_back((int64_t)std::lround(latency));
            int64_t mediaFrame = std::llround(((double)frame.arrivalMs - latency) * 30.0 / 1000.0);
            if (!shown.insert(mediaFrame).second) {
                report.repeatedFrames++;
                continue;
            }
            report.frames++;
            if (latestShown && mediaFrame < *latestShown) {
                report.outOfOrderFrames++;
                continue;
            }
            if (latestShown && mediaFrame > *latestShown + 1) {
                report.droppedFrames += (int)(mediaFrame - *latestShown - 1);
            }
            latestShown = mediaFrame;
        }
    }
    report.latencyMedianMs = median(latencies);
    if (!latencies.empty()) {
        report.latencyMaxMs = *std::max_element(latencies.begin(), latencies.end());
    }
    return report;
}

void LevelRecorder::record(tgcalls::GroupLevelsUpdate const &update) {
    std::lock_guard<std::mutex> lock(_mutex);
    for (const auto &level : update.updates) {
        if (level.ssrc == 0) {
            continue;
        }
        auto &sums = _sums[level.ssrc];
        sums.level += level.value.level;
        sums.voice += level.value.voice ? 1 : 0;
        sums.updates++;
    }
}

std::map<std::string, LevelStats> LevelRecorder::stats(std::map<uint32_t, std::string> const &names) const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::map<std::string, LevelStats> result;
    for (const auto &[ssrc, name] : names) {
        auto it = _sums.find(ssrc);
        LevelStats stats;
        if (it != _sums.end() && it->second.updates > 0) {
            stats.updates = it->second.updates;
            stats.meanLevel = it->second.level / it->second.updates;
            stats.speakingFraction = (double)it->second.voice / it->second.updates;
        }
        result[name] = stats;
    }
    return result;
}

} // namespace broadcast
