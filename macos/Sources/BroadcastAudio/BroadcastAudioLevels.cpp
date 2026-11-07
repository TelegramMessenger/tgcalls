#include "BroadcastAudioLevels.h"

#include "modules/audio_processing/agc2/cpu_features.h"
#include "modules/audio_processing/agc2/vad_wrapper.h"
#include "modules/audio_processing/audio_buffer.h"
#include "modules/audio_processing/include/audio_frame_view.h"
#include "modules/audio_processing/include/audio_processing.h"

#include <algorithm>
#include <cmath>

namespace broadcast_audio {

namespace {

constexpr int kVadResultHistoryLength = 8;

// The engine's VadHistory: voice when the mean of the last 8 results is above 0.8.
class VadHistory {
public:
    bool update(float vadProbability) {
        for (int i = 1; i < kVadResultHistoryLength; i++) {
            _history[i - 1] = _history[i];
        }
        _history[kVadResultHistoryLength - 1] = vadProbability;
        float movingAverage = 0.0f;
        for (int i = 0; i < kVadResultHistoryLength; i++) {
            movingAverage += _history[i];
        }
        movingAverage /= (float)kVadResultHistoryLength;
        return movingAverage > 0.8f;
    }

private:
    float _history[kVadResultHistoryLength] = {};
};

// The engine's CombinedVad: webrtc's detector, skipped (counted as 0) for a block whose peak is ≤ 0.01.
class CombinedVad {
public:
    CombinedVad() :
    _vad(500, webrtc::GetAvailableCpuFeatures(), webrtc::AudioProcessing::kSampleRate48kHz) {
    }

    bool update(webrtc::AudioBuffer *buffer) {
        if (buffer->num_channels() <= 0) {
            return _history.update(0.0f);
        }
        webrtc::AudioFrameView<float> frameView(buffer->channels(), (int)buffer->num_channels(), (int)buffer->num_frames());
        float peak = 0.0f;
        for (const auto &x : frameView.channel(0)) {
            peak = std::max(std::fabs(x), peak);
        }
        if (peak <= 0.01f) {
            return _history.update(0.0f);
        }
        return _history.update(_vad.Analyze(frameView));
    }

private:
    webrtc::VoiceActivityDetectorWrapper _vad;
    VadHistory _history;
};

// The engine's SparseVad: the detector at most once per 400 samples, the level as the peak over ≥ 4400 samples
// divided by 8000, both held between updates.
class SparseVad {
public:
    std::pair<float, bool> update(webrtc::AudioBuffer *buffer) {
        _sampleCount += buffer->num_frames();
        if (_sampleCount >= 400) {
            _sampleCount = 0;
            _currentValue = _vad.update(buffer);
        }
        const float *samples = buffer->channels()[0];
        for (size_t i = 0; i < buffer->num_frames(); i++) {
            const float sample = std::fabs(samples[i]);
            if (_peak < sample) {
                _peak = sample;
            }
            _peakCount += 1;
        }
        if (_peakCount >= 4400) {
            _currentLevel = _peak / 8000.0f;
            _peak = 0.0f;
            _peakCount = 0;
        }
        return std::make_pair(_currentLevel, _currentValue);
    }

private:
    CombinedVad _vad;
    bool _currentValue = false;
    size_t _sampleCount = 0;
    int _peakCount = 0;
    float _peak = 0.0f;
    float _currentLevel = 0.0f;
};

} // namespace

class LevelSlots {
public:
    LevelSlots() :
    _buffer(48000, 1, 48000, 1, 48000, 1),
    _config(48000, 1) {
    }

    std::pair<float, bool> update(uint32_t ssrc, const int16_t *samples) {
        _clock++;
        size_t index = Levels::kSlots;
        size_t leastRecent = 0;
        for (size_t i = 0; i < Levels::kSlots; i++) {
            if (_used[i] && _ssrcs[i] == ssrc) {
                index = i;
                break;
            }
            if (!_used[i]) {
                if (index == Levels::kSlots || _used[index]) {
                    index = i;
                }
            } else if (_lastUse[i] < _lastUse[leastRecent] || !_used[leastRecent]) {
                leastRecent = i;
            }
        }
        if (index == Levels::kSlots || (_used[index] && _ssrcs[index] != ssrc)) {
            index = leastRecent;
        }
        _used[index] = true;
        _ssrcs[index] = ssrc;
        _lastUse[index] = _clock;
        _buffer.CopyFrom(samples, _config);
        return _vads[index].update(&_buffer);
    }

private:
    webrtc::AudioBuffer _buffer;
    webrtc::StreamConfig _config;
    SparseVad _vads[Levels::kSlots];
    uint32_t _ssrcs[Levels::kSlots] = {};
    uint64_t _lastUse[Levels::kSlots] = {};
    bool _used[Levels::kSlots] = {};
    uint64_t _clock = 0;
};

Levels::Levels() :
_slots(std::make_unique<LevelSlots>()) {
}

Levels::~Levels() = default;

std::pair<float, bool> Levels::update(uint32_t ssrc, const int16_t *samples) {
    return _slots->update(ssrc, samples);
}

} // namespace broadcast_audio
