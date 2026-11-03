#ifndef BROADCAST_MEDIA_PART_HEADER_H
#define BROADCAST_MEDIA_PART_HEADER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace broadcast_media {

// The header in front of a video or unified broadcast part (the engine's VideoStreamingPart, Tests/EngineReference/,
// reads the same): the container
// the media slice is in, and the first event's endpoint and rotation.
struct PartHeader {
    // An FFmpeg input format name, e.g. "mp4".
    std::string container;
    std::string endpointId;
    // As the header says: 0, 90, 180 or 270 (any other value is kept; renderers treat it as 0).
    int32_t rotation = 0;
    // From the start of the part; the slice runs to the part's end.
    size_t sliceOffset = 0;
    size_t sliceSize = 0;
};

// All integers little-endian: int32 signature 0xa12e810d, the container as a TL string, int32 activeMask (ignored),
// int32 eventCount (> 0), then the first event only: int32 offset, TL string endpointId, int32 rotation, int32 extra.
// The header ends after that event, whatever eventCount says. The slice starts `offset` bytes after the header; nullopt
// when anything is missing or the slice would be empty.
std::optional<PartHeader> parsePartHeader(const uint8_t *data, size_t size);

// The same parse with the strings as offsets and lengths into `data` (the C API's form).
struct PartHeaderLayout {
    size_t containerOffset = 0;
    size_t containerLength = 0;
    size_t endpointOffset = 0;
    size_t endpointLength = 0;
    int32_t rotation = 0;
    size_t sliceOffset = 0;
    size_t sliceSize = 0;
};

std::optional<PartHeaderLayout> parsePartHeaderLayout(const uint8_t *data, size_t size);

} // namespace broadcast_media

#endif
