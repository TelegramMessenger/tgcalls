#include "BroadcastMediaIO.h"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace broadcast_media {

namespace {

constexpr int kBufferSize = 4 * 1024;

} // namespace

MemoryIO::MemoryIO(const uint8_t *data, size_t size) :
_data(data, data + size) {
    uint8_t *buffer = (uint8_t *)av_malloc(kBufferSize);
    if (!buffer) {
        return;
    }
    _context = avio_alloc_context(buffer, kBufferSize, 0, this, &MemoryIO::read, nullptr, &MemoryIO::seek);
    if (!_context) {
        av_free(buffer);
    }
}

MemoryIO::~MemoryIO() {
    if (_context) {
        // FFmpeg may have replaced the buffer it was given; free whichever it holds.
        av_freep(&_context->buffer);
        avio_context_free(&_context);
    }
}

AVIOContext *MemoryIO::context() const {
    return _context;
}

int MemoryIO::read(void *opaque, uint8_t *buffer, int size) {
    MemoryIO *io = static_cast<MemoryIO *>(opaque);
    if (size <= 0 || io->_position >= io->_data.size()) {
        return AVERROR_EOF;
    }
    const size_t count = std::min((size_t)size, io->_data.size() - io->_position);
    std::memcpy(buffer, io->_data.data() + io->_position, count);
    io->_position += count;
    return (int)count;
}

// avio_seek hands the callback absolute positions (SEEK_SET) or asks the size (AVSEEK_SIZE); the others are handled
// for completeness. Positions clamp to the data, as the engine's AVIOContextImpl does.
int64_t MemoryIO::seek(void *opaque, int64_t offset, int whence) {
    MemoryIO *io = static_cast<MemoryIO *>(opaque);
    const int64_t size = (int64_t)io->_data.size();
    if (whence & AVSEEK_SIZE) {
        return size;
    }
    int64_t base = 0;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_CUR:
        base = (int64_t)io->_position;
        break;
    case SEEK_END:
        base = size;
        break;
    default:
        base = 0;
        break;
    }
    const int64_t target = std::clamp<int64_t>(base + offset, 0, size);
    io->_position = (size_t)target;
    return target;
}

} // namespace broadcast_media
