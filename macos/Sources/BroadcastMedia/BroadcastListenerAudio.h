#ifndef BROADCAST_MEDIA_LISTENER_AUDIO_H
#define BROADCAST_MEDIA_LISTENER_AUDIO_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace broadcast_audio {
class Core;
}

namespace broadcast_media {

// What the controller needs from a loaded listener-audio part (Ogg Opus), read without decoding.
struct ListenerAudioInfo {
    // The stream's duration × time base, as the engine's AudioStreamingPartInternal computes it; 0 for a part without
    // a channel map (no speakers), as the engine reports it, so the controller moves on once the next part is ready.
    int durationMs = 0;
    // Endpoint → video channel id: the k-th set bit i of ACTIVE_MASK is id i + 1 for ENDPOINTS[k], only when the counts
    // match.
    std::map<std::string, int32_t> videoChannels;
};

std::optional<ListenerAudioInfo> readListenerAudioInfo(const uint8_t *data, size_t size);

// One ssrc's audio in a part: 48 kHz mono signed 16-bit, the part's samples [startSample, startSample + size), from
// the ssrc's first mapped 10 ms frame to its last (zeros between, where it was unmapped).
struct SsrcAudio {
    uint32_t ssrc = 0;
    size_t startSample = 0;
    std::vector<int16_t> samples;
};

struct DecodedListenerAudio {
    int durationMs = 0;
    std::vector<SsrcAudio> ssrcs;
};

// Decodes one listener-audio part on its own, with a fresh Opus decoder, as the engine does (each of its segments
// carries its own decoder). Parts may be decoded on any thread, in any order, and again. The channel map follows the
// part's TG_META: an update for frame f applies before frame f is read and evicts entries with its ssrc or its
// channel. A part with no channel map, or with more than 8 channels, has no audio. nullopt when the bytes are not an
// Ogg stream with audio; otherwise what could be decoded.
std::optional<DecodedListenerAudio> decodeListenerAudio(const uint8_t *data, size_t size);

// The part as the audio core's segment `timestampMs`; false when the core refuses it. On the core's control thread
// (its control calls come from one thread at a time).
bool enqueueListenerAudio(broadcast_audio::Core &core, int64_t timestampMs, DecodedListenerAudio const &audio);

} // namespace broadcast_media

#endif
