#ifndef TGCALLS_AV1_AV1OBU_H
#define TGCALLS_AV1_AV1OBU_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace tgcalls {

constexpr int kAv1ObuSequenceHeader = 1;
constexpr int kAv1ObuTemporalDelimiter = 2;

// One OBU of a low-overhead bitstream (AV1 spec 5.2). `data`/`size` cover the whole OBU, header included; the payload
// starts `headerSize` bytes in. Points into the parsed buffer.
struct Av1Obu {
    int type = 0;
    const uint8_t *data = nullptr;
    size_t size = 0;
    size_t headerSize = 0;
};

// Splits a temporal unit into OBUs. Every OBU but the last must carry obu_size; the last may run to the end. Empty on
// malformed input.
std::vector<Av1Obu> ParseAv1Obus(const uint8_t *data, size_t size);

// The sequence header fields the codec configuration record and the size checks need (AV1 spec 5.5).
struct Av1SequenceHeader {
    int profile = 0;
    int level = 0;
    int tier = 0;
    bool highBitdepth = false;
    bool twelveBit = false;
    bool monochrome = false;
    int subsamplingX = 1;
    int subsamplingY = 1;
    int chromaSamplePosition = 0;
    bool fullRange = false;
    uint32_t maxWidth = 0;
    uint32_t maxHeight = 0;
};

std::optional<Av1SequenceHeader> ParseAv1SequenceHeader(const Av1Obu &obu);

// The AV1CodecConfigurationRecord ("av1C", AV1-ISOBMFF 2.3) for a sequence header OBU: four bytes of fields from the
// header, then the OBU itself as configOBUs. Empty if the OBU does not parse.
std::vector<uint8_t> MakeAv1CodecConfigurationRecord(const Av1Obu &sequenceHeader);

} // namespace tgcalls

#endif
