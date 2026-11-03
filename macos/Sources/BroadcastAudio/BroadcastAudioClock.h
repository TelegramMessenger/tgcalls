#ifndef BROADCAST_AUDIO_CLOCK_H
#define BROADCAST_AUDIO_CLOCK_H

#include <cstdint>
#include <functional>

namespace broadcast_audio {

// Monotonic microseconds.
using Clock = std::function<int64_t()>;

// CLOCK_UPTIME_RAW on Apple platforms: mach_absolute_time, the base of CACurrentMediaTime, which the playback
// controller's `now` must share.
int64_t defaultClockMicroseconds();

} // namespace broadcast_audio

#endif
