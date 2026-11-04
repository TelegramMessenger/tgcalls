#include "av1/Av1Obu.h"

namespace tgcalls {

namespace {

bool ReadLeb128(const uint8_t *data, size_t size, size_t &offset, uint64_t &value) {
    value = 0;
    for (int i = 0; i < 8; i++) {
        if (offset >= size) {
            return false;
        }
        uint8_t byte = data[offset++];
        value |= uint64_t(byte & 0x7f) << (i * 7);
        if (!(byte & 0x80)) {
            return true;
        }
    }
    return false;
}

class BitReader {
public:
    BitReader(const uint8_t *data, size_t size) : _data(data), _size(size) {}

    uint32_t read(int bits) {
        uint32_t value = 0;
        for (int i = 0; i < bits; i++) {
            size_t byte = _position >> 3;
            if (byte >= _size) {
                _overrun = true;
                return 0;
            }
            value = (value << 1) | ((_data[byte] >> (7 - int(_position & 7))) & 1);
            _position++;
        }
        return value;
    }

    bool readFlag() { return read(1) != 0; }

    uint32_t readUvlc() {
        int leadingZeros = 0;
        while (leadingZeros < 32 && !_overrun && read(1) == 0) {
            leadingZeros++;
        }
        if (leadingZeros >= 32 || _overrun) {
            _overrun = true;
            return 0;
        }
        return read(leadingZeros) + (1u << leadingZeros) - 1;
    }

    bool overrun() const { return _overrun; }

private:
    const uint8_t *_data;
    size_t _size;
    size_t _position = 0;
    bool _overrun = false;
};

constexpr uint32_t kColorPrimariesBt709 = 1;
constexpr uint32_t kTransferCharacteristicsSrgb = 13;
constexpr uint32_t kMatrixCoefficientsIdentity = 0;

} // namespace

std::vector<Av1Obu> ParseAv1Obus(const uint8_t *data, size_t size) {
    std::vector<Av1Obu> result;
    size_t offset = 0;
    while (offset < size) {
        const size_t start = offset;
        const uint8_t header = data[offset++];
        if (header & 0x80) {
            return {};
        }
        const int type = (header >> 3) & 0xf;
        const bool hasExtension = (header >> 2) & 1;
        const bool hasSize = (header >> 1) & 1;
        if (hasExtension) {
            if (offset >= size) {
                return {};
            }
            offset++;
        }
        uint64_t payloadSize = 0;
        if (hasSize) {
            if (!ReadLeb128(data, size, offset, payloadSize)) {
                return {};
            }
        } else {
            payloadSize = size - offset;
        }
        if (payloadSize > size - offset) {
            return {};
        }
        Av1Obu obu;
        obu.type = type;
        obu.data = data + start;
        obu.headerSize = offset - start;
        obu.size = obu.headerSize + size_t(payloadSize);
        result.push_back(obu);
        offset += size_t(payloadSize);
    }
    return result;
}

std::optional<Av1SequenceHeader> ParseAv1SequenceHeader(const Av1Obu &obu) {
    if (obu.type != kAv1ObuSequenceHeader || obu.size <= obu.headerSize) {
        return std::nullopt;
    }
    BitReader r(obu.data + obu.headerSize, obu.size - obu.headerSize);
    Av1SequenceHeader header;

    header.profile = int(r.read(3));
    r.readFlag(); // still_picture
    const bool reducedStillPictureHeader = r.readFlag();
    if (reducedStillPictureHeader) {
        header.level = int(r.read(5));
    } else {
        const bool timingInfoPresent = r.readFlag();
        bool decoderModelInfoPresent = false;
        int bufferDelayLength = 0;
        if (timingInfoPresent) {
            r.read(32); // num_units_in_display_tick
            r.read(32); // time_scale
            if (r.readFlag()) { // equal_picture_interval
                r.readUvlc();
            }
            decoderModelInfoPresent = r.readFlag();
            if (decoderModelInfoPresent) {
                bufferDelayLength = int(r.read(5)) + 1;
                r.read(32); // num_units_in_decoding_tick
                r.read(5); // buffer_removal_time_length_minus_1
                r.read(5); // frame_presentation_time_length_minus_1
            }
        }
        const bool initialDisplayDelayPresent = r.readFlag();
        const int operatingPoints = int(r.read(5)) + 1;
        for (int i = 0; i < operatingPoints; i++) {
            r.read(12); // operating_point_idc
            const int level = int(r.read(5));
            int tier = 0;
            if (level > 7) {
                tier = int(r.read(1));
            }
            if (i == 0) {
                header.level = level;
                header.tier = tier;
            }
            if (decoderModelInfoPresent && r.readFlag()) {
                r.read(bufferDelayLength); // decoder_buffer_delay
                r.read(bufferDelayLength); // encoder_buffer_delay
                r.readFlag(); // low_delay_mode_flag
            }
            if (initialDisplayDelayPresent && r.readFlag()) {
                r.read(4);
            }
        }
    }

    const int widthBits = int(r.read(4)) + 1;
    const int heightBits = int(r.read(4)) + 1;
    header.maxWidth = r.read(widthBits) + 1;
    header.maxHeight = r.read(heightBits) + 1;

    if (!reducedStillPictureHeader && r.readFlag()) { // frame_id_numbers_present_flag
        r.read(4);
        r.read(3);
    }
    r.readFlag(); // use_128x128_superblock
    r.readFlag(); // enable_filter_intra
    r.readFlag(); // enable_intra_edge_filter
    if (!reducedStillPictureHeader) {
        r.readFlag(); // enable_interintra_compound
        r.readFlag(); // enable_masked_compound
        r.readFlag(); // enable_warped_motion
        r.readFlag(); // enable_dual_filter
        const bool enableOrderHint = r.readFlag();
        if (enableOrderHint) {
            r.readFlag(); // enable_jnt_comp
            r.readFlag(); // enable_ref_frame_mvs
        }
        int forceScreenContentTools = 2;
        if (!r.readFlag()) { // seq_choose_screen_content_tools
            forceScreenContentTools = int(r.read(1));
        }
        if (forceScreenContentTools > 0) {
            if (!r.readFlag()) { // seq_choose_integer_mv
                r.read(1); // seq_force_integer_mv
            }
        }
        if (enableOrderHint) {
            r.read(3); // order_hint_bits_minus_1
        }
    }
    r.readFlag(); // enable_superres
    r.readFlag(); // enable_cdef
    r.readFlag(); // enable_restoration

    // color_config()
    header.highBitdepth = r.readFlag();
    int bitDepth = header.highBitdepth ? 10 : 8;
    if (header.profile == 2 && header.highBitdepth) {
        header.twelveBit = r.readFlag();
        bitDepth = header.twelveBit ? 12 : 10;
    }
    header.monochrome = header.profile == 1 ? false : r.readFlag();
    uint32_t colorPrimaries = 2;
    uint32_t transferCharacteristics = 2;
    uint32_t matrixCoefficients = 2;
    if (r.readFlag()) { // color_description_present_flag
        colorPrimaries = r.read(8);
        transferCharacteristics = r.read(8);
        matrixCoefficients = r.read(8);
    }
    if (header.monochrome) {
        r.readFlag(); // color_range
        header.subsamplingX = 1;
        header.subsamplingY = 1;
        header.chromaSamplePosition = 0;
    } else if (colorPrimaries == kColorPrimariesBt709 && transferCharacteristics == kTransferCharacteristicsSrgb &&
               matrixCoefficients == kMatrixCoefficientsIdentity) {
        header.subsamplingX = 0;
        header.subsamplingY = 0;
    } else {
        r.readFlag(); // color_range
        if (header.profile == 0) {
            header.subsamplingX = 1;
            header.subsamplingY = 1;
        } else if (header.profile == 1) {
            header.subsamplingX = 0;
            header.subsamplingY = 0;
        } else if (bitDepth == 12) {
            header.subsamplingX = int(r.read(1));
            header.subsamplingY = header.subsamplingX ? int(r.read(1)) : 0;
        } else {
            header.subsamplingX = 1;
            header.subsamplingY = 0;
        }
        if (header.subsamplingX && header.subsamplingY) {
            header.chromaSamplePosition = int(r.read(2));
        }
    }

    if (r.overrun()) {
        return std::nullopt;
    }
    return header;
}

} // namespace tgcalls
