#include "BroadcastAudioClock.h"

#include <chrono>
#include <ctime>

namespace broadcast_audio {

int64_t defaultClockMicroseconds() {
#if defined(__APPLE__)
    return (int64_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000);
#else
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

} // namespace broadcast_audio
