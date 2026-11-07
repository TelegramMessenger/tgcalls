#ifndef BROADCAST_MEDIA_IO_H
#define BROADCAST_MEDIA_IO_H

#include <cstddef>
#include <cstdint>
#include <vector>

struct AVIOContext;

namespace broadcast_media {

// An FFmpeg reader over a copy of a byte range, with seek (the engine's AVIOContextImpl, Tests/EngineReference/, does
// the same).
class MemoryIO {
public:
    MemoryIO(const uint8_t *data, size_t size);
    ~MemoryIO();

    MemoryIO(MemoryIO const &) = delete;
    MemoryIO &operator=(MemoryIO const &) = delete;

    // Owned by this object; null if FFmpeg could not allocate it.
    AVIOContext *context() const;

private:
    static int read(void *opaque, uint8_t *buffer, int size);
    static int64_t seek(void *opaque, int64_t offset, int whence);

    std::vector<uint8_t> _data;
    size_t _position = 0;
    AVIOContext *_context = nullptr;
};

} // namespace broadcast_media

#endif
