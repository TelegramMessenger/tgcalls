#include "BroadcastPartHeader.h"

#include <cstring>

namespace broadcast_media {

namespace {

constexpr uint32_t kSignature = 0xa12e810d;

class Reader {
public:
    Reader(const uint8_t *data, size_t size) : _data(data), _size(size) {
    }

    size_t offset() const {
        return _offset;
    }

    std::optional<int32_t> int32() {
        if (_size - _offset < 4) {
            return std::nullopt;
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; i++) {
            value |= (uint32_t)_data[_offset + i] << (8 * i);
        }
        _offset += 4;
        return (int32_t)value;
    }

    // A TL string, as VideoStreamingPart's readSerializedString reads it: a first byte of 254 is followed by a 3-byte
    // length and padding to a multiple of 4 of the length; any other first byte is the length, padded so length + 1
    // is a multiple of 4. Returns the string's offset and length.
    std::optional<std::pair<size_t, size_t>> tlString() {
        if (_size - _offset < 1) {
            return std::nullopt;
        }
        size_t length = _data[_offset];
        size_t padding = 0;
        _offset += 1;
        if (length == 254) {
            if (_size - _offset < 3) {
                return std::nullopt;
            }
            length = (size_t)_data[_offset] | ((size_t)_data[_offset + 1] << 8) | ((size_t)_data[_offset + 2] << 16);
            _offset += 3;
            padding = roundUp(length, 4) - length;
        } else {
            padding = roundUp(length + 1, 4) - (length + 1);
        }
        if (_size - _offset < length) {
            return std::nullopt;
        }
        const size_t start = _offset;
        _offset += length;
        // The engine skips the padding without checking it is there; a missing padding fails the next read.
        if (_size - _offset < padding) {
            _offset = _size;
        } else {
            _offset += padding;
        }
        return std::make_pair(start, length);
    }

private:
    static size_t roundUp(size_t value, size_t multiple) {
        return (value + multiple - 1) / multiple * multiple;
    }

    const uint8_t *_data;
    size_t _size;
    size_t _offset = 0;
};

} // namespace

std::optional<PartHeaderLayout> parsePartHeaderLayout(const uint8_t *data, size_t size) {
    if (!data) {
        return std::nullopt;
    }
    Reader reader(data, size);
    const auto signature = reader.int32();
    if (!signature || (uint32_t)signature.value() != kSignature) {
        return std::nullopt;
    }
    const auto container = reader.tlString();
    if (!container) {
        return std::nullopt;
    }
    if (!reader.int32()) {
        return std::nullopt;
    }
    const auto eventCount = reader.int32();
    if (!eventCount || eventCount.value() <= 0) {
        return std::nullopt;
    }
    const auto offset = reader.int32();
    if (!offset) {
        return std::nullopt;
    }
    const auto endpoint = reader.tlString();
    if (!endpoint) {
        return std::nullopt;
    }
    const auto rotation = reader.int32();
    if (!rotation) {
        return std::nullopt;
    }
    if (!reader.int32()) {
        return std::nullopt;
    }
    const size_t headerEnd = reader.offset();
    if (offset.value() < 0) {
        return std::nullopt;
    }
    const size_t remaining = size - headerEnd;
    if ((size_t)offset.value() >= remaining) {
        return std::nullopt;
    }
    PartHeaderLayout layout;
    layout.containerOffset = container->first;
    layout.containerLength = container->second;
    layout.endpointOffset = endpoint->first;
    layout.endpointLength = endpoint->second;
    layout.rotation = rotation.value();
    layout.sliceOffset = headerEnd + (size_t)offset.value();
    layout.sliceSize = size - layout.sliceOffset;
    return layout;
}

std::optional<PartHeader> parsePartHeader(const uint8_t *data, size_t size) {
    const auto layout = parsePartHeaderLayout(data, size);
    if (!layout) {
        return std::nullopt;
    }
    PartHeader header;
    header.container.assign((const char *)data + layout->containerOffset, layout->containerLength);
    header.endpointId.assign((const char *)data + layout->endpointOffset, layout->endpointLength);
    header.rotation = layout->rotation;
    header.sliceOffset = layout->sliceOffset;
    header.sliceSize = layout->sliceSize;
    return header;
}

} // namespace broadcast_media
