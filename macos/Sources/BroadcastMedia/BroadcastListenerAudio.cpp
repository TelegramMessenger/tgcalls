#include "BroadcastListenerAudio.h"

#include "BroadcastAudioCore.h"
#include "BroadcastMediaIO.h"

#include "rtc_base/third_party/base64/base64.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/common.h>
}

#include <algorithm>
#include <bitset>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <set>
#include <sstream>

// A port of the engine's AudioStreamingPart, AudioStreamingPartInternal and AudioStreamingPartPersistentDecoder
// (their test-only copy: Tests/EngineReference/), keeping their decisions so the output matches the engine's sample for sample. Differences:
// a channel id the stream does not have reads as unmapped (the engine read past its buffer), the float format reads
// data[0] (the engine read &data[0]; libopus gives s16, so neither path runs today), and an unknown sample format ends
// the part (the engine aborted).

namespace broadcast_media {

namespace {

constexpr int kFrameSamples = 480;
constexpr int kMaxChannels = 8;

struct ChannelUpdate {
    int32_t frameIndex = 0;
    int32_t channelId = 0;
    uint32_t ssrc = 0;
};

std::optional<uint32_t> readUInt32(std::string const &data, size_t &offset) {
    if (offset + 4 > data.size()) {
        return std::nullopt;
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        value |= (uint32_t)(uint8_t)data[offset + (size_t)i] << (8 * i);
    }
    offset += 4;
    return value;
}

// int32 channels, int32 count, count × {int32 frameIndex, int32 channelId, int32 ssrc}; any short read drops them all.
std::vector<ChannelUpdate> parseChannelUpdates(std::string const &data) {
    size_t offset = 0;
    if (!readUInt32(data, offset)) {
        return {};
    }
    const auto count = readUInt32(data, offset);
    if (!count) {
        return {};
    }
    std::vector<ChannelUpdate> result;
    for (uint32_t i = 0; i < count.value(); i++) {
        const auto frameIndex = readUInt32(data, offset);
        const auto channelId = readUInt32(data, offset);
        const auto ssrc = readUInt32(data, offset);
        if (!frameIndex || !channelId || !ssrc) {
            return {};
        }
        result.push_back(ChannelUpdate{(int32_t)frameIndex.value(), (int32_t)channelId.value(), ssrc.value()});
    }
    return result;
}

uint32_t stringToUInt32(std::string const &string) {
    std::stringstream stream(string);
    uint32_t value = 0;
    stream >> value;
    return value;
}

std::vector<std::string> splitString(std::string const &string, char delimiter) {
    std::vector<std::string> result;
    std::istringstream stream(string);
    std::string item;
    while (std::getline(stream, item, delimiter)) {
        result.push_back(item);
    }
    return result;
}

int16_t sampleFloatToInt16(float sample) {
    return av_clip_int16(static_cast<int32_t>(lrint(sample * 32767)));
}

// The part opened for reading: its Ogg demuxer over a copy of the bytes, the first audio stream and its metadata.
class OpenedPart {
public:
    static std::unique_ptr<OpenedPart> open(const uint8_t *data, size_t size) {
        if (!data || size == 0) {
            return nullptr;
        }
        auto part = std::unique_ptr<OpenedPart>(new OpenedPart(data, size));
        if (!part->_format || part->streamIndex < 0) {
            return nullptr;
        }
        return part;
    }

    ~OpenedPart() {
        if (_format) {
            avformat_close_input(&_format);
        }
        if (codecParameters) {
            avcodec_parameters_free(&codecParameters);
        }
    }

    AVFormatContext *format() const {
        return _format;
    }

    AVRational timeBase() const {
        return _format->streams[streamIndex]->time_base;
    }

    int streamIndex = -1;
    AVCodecParameters *codecParameters = nullptr;
    int durationMs = 0;
    std::vector<ChannelUpdate> updates;
    // Endpoint → ACTIVE_MASK bit.
    std::map<std::string, int32_t> endpointBits;

private:
    OpenedPart(const uint8_t *data, size_t size) :
    _io(data, size) {
        const AVInputFormat *inputFormat = av_find_input_format("ogg");
        if (!inputFormat || !_io.context()) {
            return;
        }
        _format = avformat_alloc_context();
        if (!_format) {
            return;
        }
        _format->pb = _io.context();
        if (avformat_open_input(&_format, "", inputFormat, nullptr) < 0) {
            // avformat_open_input frees the context on failure.
            _format = nullptr;
            return;
        }
        if (avformat_find_stream_info(_format, nullptr) < 0) {
            avformat_close_input(&_format);
            _format = nullptr;
            return;
        }
        for (unsigned int i = 0; i < _format->nb_streams; i++) {
            AVStream *stream = _format->streams[i];
            if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
                continue;
            }
            codecParameters = avcodec_parameters_alloc();
            avcodec_parameters_copy(codecParameters, stream->codecpar);
            streamIndex = (int)i;
            durationMs = (int)(stream->duration * av_q2d(stream->time_base) * 1000);
            readMetadata(stream->metadata);
            break;
        }
    }

    void readMetadata(AVDictionary *metadata) {
        if (!metadata) {
            return;
        }
        AVDictionaryEntry *entry = av_dict_get(metadata, "TG_META", nullptr, 0);
        if (entry && entry->value) {
            std::string decoded;
            size_t used = 0;
            rtc::Base64::Decode(std::string(entry->value), rtc::Base64::DO_LAX, &decoded, &used);
            if (!decoded.empty()) {
                updates = parseChannelUpdates(decoded);
            }
        }
        uint32_t mask = 0;
        entry = av_dict_get(metadata, "ACTIVE_MASK", nullptr, 0);
        if (entry && entry->value) {
            mask = stringToUInt32(entry->value);
        }
        std::vector<std::string> endpoints;
        entry = av_dict_get(metadata, "ENDPOINTS", nullptr, 0);
        if (entry && entry->value) {
            endpoints = splitString(entry->value, ' ');
        }
        const std::bitset<32> bits(mask);
        if (bits.count() == endpoints.size()) {
            size_t index = 0;
            for (size_t bit = 0; bit < bits.size(); bit++) {
                if (bits[bit]) {
                    endpointBits.insert(std::make_pair(endpoints[index], (int32_t)bit));
                    index++;
                }
            }
        }
    }

    MemoryIO _io;
    AVFormatContext *_format = nullptr;
};

bool sameCodecParameters(AVCodecParameters const *lhs, AVCodecParameters const *rhs) {
    return lhs->codec_id == rhs->codec_id && lhs->format == rhs->format && lhs->ch_layout.nb_channels == rhs->ch_layout.nb_channels;
}

// One part's Opus decoder: a fresh one per part, as the old engine's MediaSegment carried its own (its
// StreamingMediaContext); the engine's persistent decoder serves only unified AAC.
struct ListenerAudioCodec {
    AVCodecParameters *parameters = nullptr;
    AVRational timeBase = {0, 1};
    AVCodecContext *context = nullptr;
    bool created = false;

    ~ListenerAudioCodec() {
        reset();
    }

    void reset() {
        if (context) {
            avcodec_free_context(&context);
        }
        if (parameters) {
            avcodec_parameters_free(&parameters);
        }
        created = false;
    }

    void maybeReset(AVCodecParameters const *newParameters, AVRational newTimeBase) {
        if (created && sameCodecParameters(parameters, newParameters) && timeBase.num == newTimeBase.num && timeBase.den == newTimeBase.den) {
            return;
        }
        reset();
        created = true;
        parameters = avcodec_parameters_alloc();
        avcodec_parameters_copy(parameters, newParameters);
        timeBase = newTimeBase;
        const AVCodec *codec = avcodec_find_decoder(newParameters->codec_id);
        if (!codec) {
            return;
        }
        context = avcodec_alloc_context3(codec);
        if (avcodec_parameters_to_context(context, newParameters) < 0) {
            avcodec_free_context(&context);
            return;
        }
        context->pkt_timebase = newTimeBase;
        if (avcodec_open2(context, codec, nullptr) < 0) {
            avcodec_free_context(&context);
        }
    }

    int decode(AVCodecParameters const *newParameters, AVRational newTimeBase, AVPacket *packet, AVFrame *frame) {
        maybeReset(newParameters, newTimeBase);
        if (!context) {
            return -1;
        }
        int result = avcodec_send_packet(context, packet);
        if (result < 0) {
            return result;
        }
        const int bytesPerSample = av_get_bytes_per_sample(context->sample_fmt);
        if (bytesPerSample != 2 && bytesPerSample != 4) {
            return -1;
        }
        return avcodec_receive_frame(context, frame);
    }
};

// Interleaved PCM in 10 ms reads, as AudioStreamingPartInternal::readPcm gives it.
class PcmReader {
public:
    PcmReader(OpenedPart &part, ListenerAudioCodec &codec) :
    _part(part),
    _codec(codec) {
        _packet = av_packet_alloc();
        _frame = av_frame_alloc();
    }

    ~PcmReader() {
        av_packet_free(&_packet);
        av_frame_free(&_frame);
    }

    struct Result {
        int samples = 0;
        int channels = 0;
    };

    Result read(std::vector<int16_t> &out) {
        if (_readToEnd || !_packet || !_frame) {
            return Result();
        }
        Result result;
        if (_bufferOffset >= _bufferSize) {
            fill();
        }
        if (out.size() != (size_t)(kFrameSamples * _channels)) {
            out.resize((size_t)(kFrameSamples * _channels));
        }
        const int wanted = _channels != 0 ? (int)out.size() / _channels : 0;
        int written = 0;
        while (written < wanted) {
            if (_bufferOffset >= _bufferSize) {
                fill();
                if (_bufferOffset >= _bufferSize) {
                    break;
                }
            }
            const int count = std::min(_bufferSize - _bufferOffset, wanted - written);
            if (count != 0) {
                std::copy(_buffer.begin() + _bufferOffset * _channels, _buffer.begin() + (_bufferOffset + count) * _channels, out.begin() + written * _channels);
                _bufferOffset += count;
                written += count;
                result.samples += count;
            }
        }
        result.channels = _channels;
        return result;
    }

private:
    void fill() {
        _bufferSize = 0;
        _bufferOffset = 0;
        if (_readToEnd) {
            return;
        }
        int result = 0;
        while (true) {
            result = av_read_frame(_part.format(), _packet);
            if (result < 0) {
                _readToEnd = true;
                return;
            }
            if (_packet->stream_index != _part.streamIndex) {
                av_packet_unref(_packet);
                continue;
            }
            result = _codec.decode(_part.codecParameters, _part.timeBase(), _packet, _frame);
            av_packet_unref(_packet);
            if (result == AVERROR(EAGAIN)) {
                continue;
            }
            break;
        }
        if (result != 0) {
            _readToEnd = true;
            return;
        }
        const int frameChannels = _frame->ch_layout.nb_channels;
        if (_channels == 0) {
            _channels = frameChannels;
        }
        if (_channels == 0 || frameChannels != _channels || frameChannels > kMaxChannels) {
            _readToEnd = true;
            return;
        }
        const int samples = _frame->nb_samples;
        if (_buffer.size() < (size_t)(samples * frameChannels)) {
            _buffer.resize((size_t)(samples * frameChannels));
        }
        switch (_frame->format) {
        case AV_SAMPLE_FMT_S16:
            std::memcpy(_buffer.data(), _frame->data[0], (size_t)samples * 2 * (size_t)frameChannels);
            break;
        case AV_SAMPLE_FMT_S16P: {
            int16_t *to = _buffer.data();
            for (int sample = 0; sample < samples; sample++) {
                for (int channel = 0; channel < frameChannels; channel++) {
                    *to++ = ((const int16_t *)_frame->data[channel])[sample];
                }
            }
            break;
        }
        case AV_SAMPLE_FMT_FLT: {
            const float *from = (const float *)_frame->data[0];
            for (int i = 0; i < samples * frameChannels; i++) {
                _buffer[(size_t)i] = sampleFloatToInt16(from[i]);
            }
            break;
        }
        case AV_SAMPLE_FMT_FLTP: {
            int16_t *to = _buffer.data();
            for (int sample = 0; sample < samples; sample++) {
                for (int channel = 0; channel < frameChannels; channel++) {
                    *to++ = sampleFloatToInt16(((const float *)_frame->data[channel])[sample]);
                }
            }
            break;
        }
        default:
            _readToEnd = true;
            return;
        }
        _bufferSize = samples;
        _bufferOffset = 0;
    }

    OpenedPart &_part;
    ListenerAudioCodec &_codec;
    AVPacket *_packet = nullptr;
    AVFrame *_frame = nullptr;
    std::vector<int16_t> _buffer;
    int _bufferSize = 0;
    int _bufferOffset = 0;
    int _channels = 0;
    bool _readToEnd = false;
};

struct Mapping {
    uint32_t ssrc = 0;
    int32_t channel = 0;
};

// AudioStreamingPartState::updateCurrentMapping.
void updateMapping(std::vector<Mapping> &mapping, uint32_t ssrc, int32_t channel) {
    for (int i = (int)mapping.size() - 1; i >= 0; i--) {
        Mapping const &entry = mapping[(size_t)i];
        if (entry.ssrc == ssrc && entry.channel == channel) {
            return;
        } else if (entry.ssrc == ssrc || entry.channel == channel) {
            mapping.erase(mapping.begin() + i);
        }
    }
    mapping.push_back(Mapping{ssrc, channel});
}

std::optional<int32_t> mappedChannel(std::vector<Mapping> const &mapping, uint32_t ssrc) {
    for (auto const &entry : mapping) {
        if (entry.ssrc == ssrc) {
            return entry.channel;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<ListenerAudioInfo> readListenerAudioInfo(const uint8_t *data, size_t size) {
    const auto part = OpenedPart::open(data, size);
    if (!part) {
        return std::nullopt;
    }
    ListenerAudioInfo info;
    // The engine never sets a part's remaining time when the part has no channel map (AudioStreamingPart's constructor).
    info.durationMs = part->updates.empty() ? 0 : part->durationMs;
    for (auto const &it : part->endpointBits) {
        info.videoChannels[it.first] = it.second + 1;
    }
    return info;
}

std::optional<DecodedListenerAudio> decodeListenerAudio(const uint8_t *data, size_t size) {
    const auto part = OpenedPart::open(data, size);
    if (!part) {
        return std::nullopt;
    }
    DecodedListenerAudio result;
    // The engine reads nothing from a part without a channel map, and reports no audio length for it.
    if (part->updates.empty()) {
        return result;
    }
    result.durationMs = part->durationMs;

    std::set<uint32_t> ssrcs;
    for (auto const &update : part->updates) {
        ssrcs.insert(update.ssrc);
    }
    struct Accumulated {
        std::vector<int16_t> samples;
        size_t first = SIZE_MAX;
        size_t end = 0;
    };
    std::map<uint32_t, Accumulated> accumulated;
    for (uint32_t ssrc : ssrcs) {
        accumulated[ssrc];
    }

    ListenerAudioCodec codec;
    PcmReader reader(*part, codec);
    std::vector<Mapping> mapping;
    std::vector<int16_t> pcm;
    int frameIndex = 0;
    size_t position = 0;
    while (true) {
        for (auto const &update : part->updates) {
            if (update.frameIndex == frameIndex) {
                updateMapping(mapping, update.ssrc, update.channelId);
            }
        }
        const auto read = reader.read(pcm);
        if (read.samples <= 0) {
            break;
        }
        for (auto &it : accumulated) {
            Accumulated &entry = it.second;
            entry.samples.resize(position + (size_t)read.samples, 0);
            const auto channel = mappedChannel(mapping, it.first);
            if (!channel || channel.value() < 0 || channel.value() >= read.channels) {
                continue;
            }
            for (int j = 0; j < read.samples; j++) {
                entry.samples[position + (size_t)j] = pcm[(size_t)(channel.value() + j * read.channels)];
            }
            entry.first = std::min(entry.first, position);
            entry.end = position + (size_t)read.samples;
        }
        position += (size_t)read.samples;
        frameIndex++;
    }

    for (auto &it : accumulated) {
        Accumulated &entry = it.second;
        if (entry.first == SIZE_MAX) {
            continue;
        }
        SsrcAudio audio;
        audio.ssrc = it.first;
        audio.startSample = entry.first;
        audio.samples.assign(entry.samples.begin() + (long)entry.first, entry.samples.begin() + (long)entry.end);
        result.ssrcs.push_back(std::move(audio));
    }
    return result;
}

bool enqueueListenerAudio(broadcast_audio::Core &core, int64_t timestampMs, DecodedListenerAudio const &audio) {
    std::vector<broadcast_audio::ChannelSamples> entries;
    entries.reserve(audio.ssrcs.size());
    for (auto const &ssrc : audio.ssrcs) {
        broadcast_audio::ChannelSamples entry;
        entry.ssrc = ssrc.ssrc;
        entry.samples = ssrc.samples.data();
        entry.count = ssrc.samples.size();
        entry.startSample = ssrc.startSample;
        entries.push_back(entry);
    }
    return core.enqueueSegment(timestampMs, entries.data(), entries.size());
}

} // namespace broadcast_media
