#ifndef BROADCAST_AUDIO_CORE_C_H
#define BROADCAST_AUDIO_CORE_C_H

// The broadcast audio core for Swift (the app and the testbench runner): plays listener-format broadcast audio
// through a call's audio device. The core lives in submodules/TgVoipWebrtc/BroadcastAudio (C++); see its CLAUDE.md.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BroadcastAudioCore *BroadcastAudioCoreRef;

typedef struct {
    uint32_t ssrc;
    // The maximum level × 2 since the last call.
    float level;
    // Whether any update since the last call had voice.
    bool voice;
} BroadcastAudioLevel;

// A core on the default clock (CLOCK_UPTIME_RAW: CACurrentMediaTime's base).
BroadcastAudioCoreRef _Nonnull BroadcastAudioCoreCreate(void);
// Releases this handle; audio devices that the core is attached to keep it alive.
void BroadcastAudioCoreRelease(BroadcastAudioCoreRef _Nonnull core);
// The core's clock in seconds: the base `play` host times are in.
double BroadcastAudioCoreNow(BroadcastAudioCoreRef _Nonnull core);
// One segment: `channelCount` entries (at most 32) of 48 kHz mono signed 16-bit PCM. Entry i is ssrc `ssrcs[i]`'s
// samples [startSamples[i], startSamples[i] + sampleCounts[i]) of the segment, clipped at 48000; `startSamples` may be
// NULL for zeros. Copies; false when 64 segments are already in flight.
bool BroadcastAudioCoreEnqueueSegment(BroadcastAudioCoreRef _Nonnull core, int64_t timestampMs, const uint32_t *_Nonnull ssrcs, const int16_t *_Nonnull const *_Nonnull samples, const size_t *_Nonnull sampleCounts, const size_t *_Nullable startSamples, size_t channelCount);
// The playhead enters the segment at `hostTime` (seconds, BroadcastAudioCoreNow's base), `offsetMs` into it.
bool BroadcastAudioCorePlay(BroadcastAudioCoreRef _Nonnull core, int64_t timestampMs, double hostTime, int32_t offsetMs);
// While active, the core's audio replaces the call's own playout; detaching restores it in the next 10 ms block.
void BroadcastAudioCoreSetActive(BroadcastAudioCoreRef _Nonnull core, bool active);
void BroadcastAudioCoreSetVolume(BroadcastAudioCoreRef _Nonnull core, uint32_t ssrc, float volume);
void BroadcastAudioCoreReset(BroadcastAudioCoreRef _Nonnull core);
// Fills up to `capacity` levels (one per ssrc heard since the last call); returns how many.
size_t BroadcastAudioCoreTakeLevels(BroadcastAudioCoreRef _Nonnull core, BroadcastAudioLevel *_Nonnull levels, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
