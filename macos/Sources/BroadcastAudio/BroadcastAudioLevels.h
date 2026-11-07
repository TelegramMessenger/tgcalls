#ifndef BROADCAST_AUDIO_LEVELS_H
#define BROADCAST_AUDIO_LEVELS_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace broadcast_audio {

class LevelSlots;

// Speaking levels and voice per ssrc: the old engine's `SparseVad` (its StreamingMediaContext.cpp at tgcalls 66a4591), which the test checks
// update by update against a verbatim copy. Per 10 ms block of 48 kHz mono: the level is the peak over ≥ 4400
// samples / 8000, held between updates; voice is webrtc's VoiceActivityDetectorWrapper(500, cpu features, 48000)
// through the mean of the last 8 results > 0.8, and no voice for a block whose peak is ≤ 0.01.
//
// Every detector and the conversion buffer are built in the constructor: `update` neither allocates nor blocks. A new
// ssrc takes a free slot, else the least recently used one, whose detector state it inherits.
class Levels {
public:
    static constexpr size_t kSlots = 32;

    Levels();
    ~Levels();

    // `samples`: one 10 ms block (480 samples) before gain.
    std::pair<float, bool> update(uint32_t ssrc, const int16_t *samples);

private:
    std::unique_ptr<LevelSlots> _slots;
};

} // namespace broadcast_audio

#endif
