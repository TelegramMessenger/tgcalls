#ifndef BROADCAST_AUDIO_CORE_H
#define BROADCAST_AUDIO_CORE_H

#include "BroadcastAudioClock.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace broadcast_audio {

// One entry of a segment: 48 kHz mono signed 16-bit PCM of one ssrc, the segment's samples [startSample, startSample +
// count), clipped at 48 000. Several entries may share an ssrc (a speaker mapped, unmapped and mapped again).
struct ChannelSamples {
    uint32_t ssrc = 0;
    const int16_t *samples = nullptr;
    size_t count = 0;
    size_t startSample = 0;
};

// What the speaking indicators read for an ssrc over the last report window: the maximum level × 2, and whether any
// update had voice.
struct Level {
    uint32_t ssrc = 0;
    float level = 0.0f;
    bool voice = false;
};

class CoreDevice;

// Plays listener-format broadcast audio through a call's audio device, aligned to the playback controller's host
// clock. The controller's thread hands over 1-second segments (per-ssrc PCM) and `play` events; the device's thread
// calls `render` for each 10 ms block, which overwrites the call's playout while the core is active.
//
// The position rule: a cursor moves on the host clock in 48 kHz samples, a block per render, and resyncs to `now`
// only when it drifts more than kResyncToleranceUs from it (devices pull in bursts). Each sample position plays from
// the latest `play` whose start it has reached, at `offset + (position - start)` into its segment, so a block may
// straddle two plays and segments played back to back have no gap. Zeros where nothing is due.
//
// Control-side calls must come from one thread at a time; `render` from the device's thread. `render` never
// allocates (except PushResampler's re-initialisation on a rate or channel change) and never blocks.
class Core {
public:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr size_t kBlockSamples = 480;
    static constexpr size_t kSegmentCapacity = 64;
    // Entries (ChannelSamples) per segment: a part has at most 8 channels, but speakers changing channels can name more
    // ssrcs within one second.
    static constexpr size_t kMaxChannelsPerSegment = 32;
    static constexpr int64_t kResyncToleranceUs = 40000;

    explicit Core(Clock clock = defaultClockMicroseconds);
    ~Core();

    Core(Core const &) = delete;
    Core &operator=(Core const &) = delete;

    // Control thread. Copies the samples; false when kSegmentCapacity segments are already in flight. Channels beyond
    // kMaxChannelsPerSegment are dropped.
    bool enqueueSegment(int64_t timestampMs, const ChannelSamples *channels, size_t channelCount);
    // Control thread: the playhead enters the segment at `startUs` on the core's clock, `offsetMs` into it. False when
    // the command queue is full.
    bool play(int64_t timestampMs, int64_t startUs, int32_t offsetMs);
    void setActive(bool active);
    bool isActive() const;
    // Any number of ssrcs; applies to queued segments too. An unchanged volume costs nothing.
    void setVolume(uint32_t ssrc, float gain);
    // Forgets every segment and play.
    void reset();
    // The levels since the last call, per ssrc.
    std::vector<Level> takeLevels();
    int64_t nowUs() const;

    // Device thread. Overwrites `samples` (`frames` × `channels` interleaved signed 16-bit at `sampleRate`) with the
    // audio due now, zeros where nothing is due. Returns false, leaving the buffer untouched, when inactive or when
    // the block is not 10 ms.
    bool render(int16_t *samples, size_t frames, size_t channels, uint32_t sampleRate);

private:
    void collect();
    float volume(uint32_t ssrc) const;

    Clock _clock;
    std::unique_ptr<CoreDevice> _device;
    size_t _segmentsInFlight = 0;
    std::unordered_map<uint32_t, float> _volumes;
};

} // namespace broadcast_audio

#endif
