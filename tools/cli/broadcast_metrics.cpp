#include "broadcast_metrics.h"

#include <algorithm>
#include <chrono>
#include <cmath>

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
constexpr double kFixtureLoopMs = 10000.0;

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
    std::vector<int64_t> latencies;
    const Frame *previous = nullptr;
    for (const auto &frame : _frames) {
        if (frame.arrivalMs < startMs) {
            continue;
        }
        report.frames++;
        report.widths[frame.width]++;
        if (!report.startMs) {
            report.startMs = frame.arrivalMs - startMs;
        }
        if (previous) {
            int64_t gap = frame.arrivalMs - previous->arrivalMs;
            if (gap > kVideoStallMs) {
                report.stallCount++;
                report.stallMs += gap - kVideoFrameMs;
            }
        }
        if (frame.counter >= 0) {
            // The fixture loops every 10 s and part T shows slice (T / 1000) % 10, so a counter is a media time
            // modulo 10 s; the distance from live is the arrival minus it, modulo 10 s.
            double latency = std::fmod((double)frame.arrivalMs - frame.counter * 1000.0 / 30.0, kFixtureLoopMs);
            if (latency < 0) {
                latency += kFixtureLoopMs;
            }
            latencies.push_back((int64_t)std::lround(latency));
        }
        previous = &frame;
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
