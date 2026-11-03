#include "BroadcastAudioCore.h"

#include "BroadcastAudioLevels.h"
#include "BroadcastSpscQueue.h"

#include "api/audio/audio_frame.h"
#include "audio/utility/audio_frame_operations.h"
#include "common_audio/resampler/include/push_resampler.h"
#include "modules/audio_mixer/frame_combiner.h"
#include "rtc_base/logging.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace broadcast_audio {

namespace {

constexpr size_t kCommandCapacity = 256;
// Every segment in flight can come back at once (a reset), with room to spare.
constexpr size_t kRetiredCapacity = 128;
constexpr size_t kPlayCapacity = 16;
// 10 ms records for 8 channels: 512 blocks (5.12 s) between two reports before any is dropped.
constexpr size_t kLevelRecordCapacity = 4096;
// A block can straddle two segments, each with its own ssrcs.
// A block can straddle two segments; a 10 ms block of one segment holds at most its 8 channels' ssrcs, plus those a
// channel switches to inside the block, so 12 a segment is ample.
constexpr size_t kMaxBlockSsrcs = 24;
constexpr size_t kMaxSegmentSamples = 48000;
constexpr size_t kMaxDeviceChannels = 8;
constexpr int64_t kSamplesPerMs = 48;
constexpr int64_t kResyncToleranceSamples = Core::kResyncToleranceUs * kSamplesPerMs / 1000;

struct Segment {
    int64_t timestampMs = 0;
    size_t channelCount = 0;
    uint32_t ssrcs[Core::kMaxChannelsPerSegment] = {};
    // Stamped from the control thread's volumes when enqueued, rewritten by later volume changes.
    float gains[Core::kMaxChannelsPerSegment] = {};
    size_t starts[Core::kMaxChannelsPerSegment] = {};
    std::vector<int16_t> samples[Core::kMaxChannelsPerSegment];
};

int64_t floorDiv(int64_t value, int64_t divisor) {
    int64_t quotient = value / divisor;
    if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) {
        quotient--;
    }
    return quotient;
}

// A host time in microseconds as a position on the 48 kHz sample axis the cursor moves on.
int64_t hostSamples(int64_t us) {
    return floorDiv(us * kSamplesPerMs, 1000);
}

struct Command {
    enum class Type : uint8_t {
        segment,
        play,
        volume,
        reset
    };
    Type type = Type::reset;
    Segment *segment = nullptr;
    int64_t timestampMs = 0;
    int64_t startUs = 0;
    int32_t offsetMs = 0;
    uint32_t ssrc = 0;
    float gain = 1.0f;
};

struct Play {
    int64_t timestampMs = 0;
    int64_t startUs = 0;
    int32_t offsetMs = 0;
    // startUs on the sample axis.
    int64_t start = 0;
};

// One ssrc's samples in the block being rendered, assembled across the plays the block straddles.
struct BlockSsrc {
    uint32_t ssrc = 0;
    float gain = 1.0f;
    int16_t samples[Core::kBlockSamples] = {};
};

} // namespace

// Everything the device thread touches, allocated once with the core.
class CoreDevice {
public:
    explicit CoreDevice(Clock const &clock) :
    _clock(clock),
    _combiner(false) {
        _mixList.reserve(kMaxBlockSsrcs);
    }

    ~CoreDevice() {
        Command command;
        while (_commands.pop(command)) {
            delete command.segment;
        }
        for (size_t i = 0; i < _segmentCount; i++) {
            delete _segments[i];
        }
        Segment *segment = nullptr;
        while (_retired.pop(segment)) {
            delete segment;
        }
    }

    // Shared queues: control → device, device → control.
    SpscQueue<Command, kCommandCapacity> _commands;
    SpscQueue<Segment *, kRetiredCapacity> _retired;
    SpscQueue<Level, kLevelRecordCapacity> _levelRecords;
    std::atomic<bool> _active{false};

    bool render(int16_t *samples, size_t frames, size_t channels, uint32_t sampleRate) {
        // Taken even while inactive, so the control thread's commands (the app sets volumes on every participant
        // update) never fill the queue while the call's own playout is heard.
        drainCommands();
        if (!_active.load(std::memory_order_acquire)) {
            return false;
        }
        if (channels == 0 || channels > kMaxDeviceChannels || sampleRate == 0 || sampleRate % 100 != 0 || frames != sampleRate / 100) {
            return false;
        }
        renderMono(_clock());
        writeOutput(samples, frames, channels, sampleRate);
        return true;
    }

private:
    void retire(Segment *segment) {
        if (!_retired.push(segment)) {
            // Cannot happen with kRetiredCapacity ≥ the segments in flight; keep it rather than free it here.
            _pendingRetire[_pendingRetireCount++ % kRetiredCapacity] = segment;
        }
    }

    void retryPendingRetires() {
        while (_pendingRetireCount > _pendingRetireDone) {
            if (!_retired.push(_pendingRetire[_pendingRetireDone % kRetiredCapacity])) {
                return;
            }
            _pendingRetireDone++;
        }
    }

    void drainCommands() {
        retryPendingRetires();
        Command command;
        while (_commands.pop(command)) {
            switch (command.type) {
            case Command::Type::segment:
                if (_segmentCount < Core::kSegmentCapacity) {
                    _segments[_segmentCount++] = command.segment;
                } else {
                    retire(command.segment);
                }
                break;
            case Command::Type::play:
                if (_playCount == kPlayCapacity) {
                    std::memmove(&_plays[0], &_plays[1], sizeof(Play) * (kPlayCapacity - 1));
                    _playCount--;
                }
                _plays[_playCount++] = Play{command.timestampMs, command.startUs, command.offsetMs, hostSamples(command.startUs)};
                break;
            case Command::Type::volume:
                // Segments enqueued later were stamped with this volume already.
                for (size_t i = 0; i < _segmentCount; i++) {
                    Segment *segment = _segments[i];
                    for (size_t channel = 0; channel < segment->channelCount; channel++) {
                        if (segment->ssrcs[channel] == command.ssrc) {
                            segment->gains[channel] = command.gain;
                        }
                    }
                }
                break;
            case Command::Type::reset:
                for (size_t i = 0; i < _segmentCount; i++) {
                    retire(_segments[i]);
                }
                _segmentCount = 0;
                _playCount = 0;
                break;
            }
        }
    }

    // The play a sample position belongs to: the latest whose start it has reached (the later one on a tie).
    int activePlayAt(int64_t position) const {
        int selected = -1;
        for (size_t i = 0; i < _playCount; i++) {
            if (_plays[i].start <= position && (selected < 0 || _plays[i].start >= _plays[(size_t)selected].start)) {
                selected = (int)i;
            }
        }
        return selected;
    }

    int64_t nextPlayStartAfter(int64_t position) const {
        int64_t next = INT64_MAX;
        for (size_t i = 0; i < _playCount; i++) {
            if (_plays[i].start > position && _plays[i].start < next) {
                next = _plays[i].start;
            }
        }
        return next;
    }

    // Plays before the one a block starts in are dropped, and segments before it go back.
    void prune(int64_t position) {
        const int active = activePlayAt(position);
        if (active < 0) {
            return;
        }
        const Play play = _plays[(size_t)active];
        size_t kept = 0;
        for (size_t i = 0; i < _playCount; i++) {
            if (_plays[i].start >= play.start) {
                _plays[kept++] = _plays[i];
            }
        }
        _playCount = kept;
        size_t keptSegments = 0;
        for (size_t i = 0; i < _segmentCount; i++) {
            if (_segments[i]->timestampMs < play.timestampMs) {
                retire(_segments[i]);
            } else {
                _segments[keptSegments++] = _segments[i];
            }
        }
        _segmentCount = keptSegments;
    }

    Segment *segmentFor(int64_t timestampMs) const {
        for (size_t i = 0; i < _segmentCount; i++) {
            if (_segments[i]->timestampMs == timestampMs) {
                return _segments[i];
            }
        }
        return nullptr;
    }

    BlockSsrc *blockSsrc(uint32_t ssrc, float gain) {
        for (size_t i = 0; i < _blockSsrcCount; i++) {
            if (_blockSsrcs[i].ssrc == ssrc) {
                _blockSsrcs[i].gain = gain;
                return &_blockSsrcs[i];
            }
        }
        if (_blockSsrcCount == kMaxBlockSsrcs) {
            return nullptr;
        }
        BlockSsrc &entry = _blockSsrcs[_blockSsrcCount++];
        entry.ssrc = ssrc;
        entry.gain = gain;
        std::memset(entry.samples, 0, sizeof(entry.samples));
        return &entry;
    }

    // Copies the positions [from, to) of the block starting at `blockStart` from `play`'s segment, per ssrc.
    void copyRun(Play const &play, int64_t from, int64_t to, int64_t blockStart) {
        Segment *segment = segmentFor(play.timestampMs);
        if (!segment) {
            return;
        }
        const int64_t source = (int64_t)play.offsetMs * kSamplesPerMs + (from - play.start);
        for (size_t channel = 0; channel < segment->channelCount; channel++) {
            std::vector<int16_t> const &data = segment->samples[channel];
            // The entry holds the segment's samples [entryStart, entryStart + size).
            const int64_t entryStart = (int64_t)segment->starts[channel];
            const int64_t begin = std::max<int64_t>(source, entryStart);
            const int64_t end = std::min<int64_t>(source + (to - from), entryStart + (int64_t)data.size());
            if (begin >= end) {
                continue;
            }
            BlockSsrc *entry = blockSsrc(segment->ssrcs[channel], segment->gains[channel]);
            if (!entry) {
                continue;
            }
            std::memcpy(entry->samples + (from - blockStart) + (begin - source), data.data() + (begin - entryStart), (size_t)(end - begin) * sizeof(int16_t));
        }
    }

    // The cursor is a position on the host clock, in 48 kHz samples. It advances a block per render and resyncs to
    // the clock only when it drifts more than the tolerance (devices pull in bursts). Each position plays from the
    // play it belongs to, at offset + (position - start), so a block straddles plays and segments played back to
    // back are heard without a gap.
    void renderMono(int64_t now) {
        std::memset(_mono, 0, sizeof(_mono));
        const int64_t hostNow = hostSamples(now);
        if (!_hasCursor || std::llabs(_cursor - hostNow) > kResyncToleranceSamples) {
            _cursor = hostNow;
            _hasCursor = true;
        }
        const int64_t blockStart = _cursor;
        const int64_t blockEnd = blockStart + (int64_t)Core::kBlockSamples;
        _cursor = blockEnd;

        prune(blockStart);
        _blockSsrcCount = 0;
        int64_t position = blockStart;
        while (position < blockEnd) {
            const int active = activePlayAt(position);
            const int64_t runEnd = std::min(blockEnd, nextPlayStartAfter(position));
            if (active >= 0) {
                copyRun(_plays[(size_t)active], position, runEnd, blockStart);
            }
            position = runEnd;
        }
        if (_blockSsrcCount == 0) {
            return;
        }

        _mixList.clear();
        for (size_t i = 0; i < _blockSsrcCount; i++) {
            BlockSsrc const &entry = _blockSsrcs[i];
            // Levels on the block as decoded, before gain; a full queue drops the record.
            const auto value = _levels.update(entry.ssrc, entry.samples);
            Level record;
            record.ssrc = entry.ssrc;
            record.level = value.first;
            record.voice = value.second;
            _levelRecords.push(record);

            webrtc::AudioFrame &frame = _frames[i];
            frame.UpdateFrame(0, entry.samples, Core::kBlockSamples, (int)Core::kSampleRate, webrtc::AudioFrame::SpeechType::kNormalSpeech, webrtc::AudioFrame::VADActivity::kVadActive);
            if (entry.gain < 0.99f || entry.gain > 1.01f) {
                webrtc::AudioFrameOperations::ScaleWithSat(entry.gain, &frame);
            }
            _mixList.push_back(&frame);
        }
        _combiner.Combine(rtc::ArrayView<webrtc::AudioFrame *const>(_mixList.data(), _mixList.size()), 1, (int)Core::kSampleRate, _mixList.size(), &_mixed);
        std::memcpy(_mono, _mixed.data(), sizeof(_mono));
    }

    void writeOutput(int16_t *samples, size_t frames, size_t channels, uint32_t sampleRate) {
        int16_t *interleaved = sampleRate == Core::kSampleRate ? samples : _interleaved;
        for (size_t i = 0; i < Core::kBlockSamples; i++) {
            for (size_t channel = 0; channel < channels; channel++) {
                interleaved[i * channels + channel] = _mono[i];
            }
        }
        if (sampleRate == Core::kSampleRate) {
            return;
        }
        if (_resampler.InitializeIfNeeded((int)Core::kSampleRate, (int)sampleRate, channels) != 0) {
            std::memset(samples, 0, frames * channels * sizeof(int16_t));
            return;
        }
        const int written = _resampler.Resample(_interleaved, Core::kBlockSamples * channels, samples, frames * channels);
        if (written != (int)(frames * channels)) {
            std::memset(samples, 0, frames * channels * sizeof(int16_t));
        }
    }

    Clock const &_clock;

    Segment *_segments[Core::kSegmentCapacity] = {};
    size_t _segmentCount = 0;
    Play _plays[kPlayCapacity];
    size_t _playCount = 0;
    bool _hasCursor = false;
    int64_t _cursor = 0;

    Segment *_pendingRetire[kRetiredCapacity] = {};
    size_t _pendingRetireCount = 0;
    size_t _pendingRetireDone = 0;

    BlockSsrc _blockSsrcs[kMaxBlockSsrcs];
    size_t _blockSsrcCount = 0;

    Levels _levels;
    webrtc::AudioFrame _frames[kMaxBlockSsrcs];
    webrtc::AudioFrame _mixed;
    std::vector<webrtc::AudioFrame *> _mixList;
    webrtc::FrameCombiner _combiner;
    webrtc::PushResampler<int16_t> _resampler;
    int16_t _mono[Core::kBlockSamples] = {};
    int16_t _interleaved[Core::kBlockSamples * kMaxDeviceChannels] = {};
};

Core::Core(Clock clock) :
_clock(std::move(clock)),
_device(std::make_unique<CoreDevice>(_clock)) {
}

Core::~Core() = default;

void Core::collect() {
    Segment *segment = nullptr;
    while (_device->_retired.pop(segment)) {
        delete segment;
        if (_segmentsInFlight > 0) {
            _segmentsInFlight--;
        }
    }
}

bool Core::enqueueSegment(int64_t timestampMs, const ChannelSamples *channels, size_t channelCount) {
    collect();
    if (_segmentsInFlight >= kSegmentCapacity) {
        return false;
    }
    if (channelCount > kMaxChannelsPerSegment) {
        RTC_LOG(LS_WARNING) << "broadcast audio: segment " << timestampMs << " has " << channelCount << " entries, keeping " << kMaxChannelsPerSegment;
        channelCount = kMaxChannelsPerSegment;
    }
    auto segment = new Segment();
    segment->timestampMs = timestampMs;
    segment->channelCount = channelCount;
    for (size_t i = 0; i < channelCount; i++) {
        segment->ssrcs[i] = channels[i].ssrc;
        segment->gains[i] = volume(channels[i].ssrc);
        const size_t start = std::min(channels[i].startSample, kMaxSegmentSamples);
        segment->starts[i] = start;
        const size_t count = std::min(channels[i].count, kMaxSegmentSamples - start);
        if (channels[i].samples && count > 0) {
            segment->samples[i].assign(channels[i].samples, channels[i].samples + count);
        }
    }
    Command command;
    command.type = Command::Type::segment;
    command.segment = segment;
    if (!_device->_commands.push(command)) {
        delete segment;
        return false;
    }
    _segmentsInFlight++;
    return true;
}

bool Core::play(int64_t timestampMs, int64_t startUs, int32_t offsetMs) {
    collect();
    Command command;
    command.type = Command::Type::play;
    command.timestampMs = timestampMs;
    command.startUs = startUs;
    command.offsetMs = offsetMs;
    return _device->_commands.push(command);
}

void Core::setActive(bool active) {
    _device->_active.store(active, std::memory_order_release);
}

bool Core::isActive() const {
    return _device->_active.load(std::memory_order_acquire);
}

float Core::volume(uint32_t ssrc) const {
    const auto it = _volumes.find(ssrc);
    return it == _volumes.end() ? 1.0f : it->second;
}

// The control thread keeps every ssrc's volume (the engine's map was unbounded too) and stamps it into each segment;
// a change also rewrites the segments already queued. The app sets every participant's volume on every update, so an
// unchanged one queues nothing.
void Core::setVolume(uint32_t ssrc, float gain) {
    collect();
    if (volume(ssrc) == gain) {
        return;
    }
    Command command;
    command.type = Command::Type::volume;
    command.ssrc = ssrc;
    command.gain = gain;
    if (!_device->_commands.push(command)) {
        // Not recorded either, so the app's next update tries again.
        RTC_LOG(LS_WARNING) << "broadcast audio: command queue full, volume for " << ssrc << " dropped";
        return;
    }
    _volumes[ssrc] = gain;
}

void Core::reset() {
    collect();
    Command command;
    command.type = Command::Type::reset;
    if (!_device->_commands.push(command)) {
        RTC_LOG(LS_WARNING) << "broadcast audio: command queue full, reset dropped";
    }
}

// The RTP path's rule for a report window: the maximum level, voice if any update had it; reported × 2, as the engine
// reported every level.
std::vector<Level> Core::takeLevels() {
    collect();
    std::vector<Level> result;
    Level record;
    while (_device->_levelRecords.pop(record)) {
        auto it = std::find_if(result.begin(), result.end(), [&](Level const &level) {
            return level.ssrc == record.ssrc;
        });
        if (it == result.end()) {
            result.push_back(record);
        } else {
            it->level = std::max(it->level, record.level);
            it->voice = it->voice || record.voice;
        }
    }
    for (auto &level : result) {
        level.level *= 2.0f;
    }
    return result;
}

int64_t Core::nowUs() const {
    return _clock();
}

bool Core::render(int16_t *samples, size_t frames, size_t channels, uint32_t sampleRate) {
    return _device->render(samples, frames, channels, sampleRate);
}

} // namespace broadcast_audio
