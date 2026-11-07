#include "BroadcastVideoDecoder.h"

#include "BroadcastMediaIO.h"
#include "BroadcastPartHeader.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <cstring>

// A port of the engine's VideoStreamingPart (its test-only copy: Tests/EngineReference/VideoStreamingPart.cpp): the
// same container opening,
// codec reuse rule and frame choice, so software frames come out identical to the engine's. Differences: one decoder
// per endpoint (the engine shared one across endpoints), VideoToolbox for the codecs that have it (the engine's path is
// commented out), software frames that are not YUV420P / YUVJ420P skipped (the engine copied them as I420), the canonical
// receive-before-send decode loop, and frame times falling back to best_effort_timestamp.

namespace broadcast_media {

VideoFrame::~VideoFrame() {
    if (pixelBuffer) {
        CVPixelBufferRelease(pixelBuffer);
    }
}

VideoFrame::VideoFrame(VideoFrame &&other) noexcept {
    *this = std::move(other);
}

VideoFrame &VideoFrame::operator=(VideoFrame &&other) noexcept {
    if (this != &other) {
        if (pixelBuffer) {
            CVPixelBufferRelease(pixelBuffer);
        }
        index = other.index;
        pts = other.pts;
        rotation = other.rotation;
        width = other.width;
        height = other.height;
        pixelBuffer = other.pixelBuffer;
        other.pixelBuffer = nullptr;
        y = std::move(other.y);
        u = std::move(other.u);
        v = std::move(other.v);
        strideY = other.strideY;
        strideU = other.strideU;
        strideV = other.strideV;
    }
    return *this;
}

namespace {

// The engine's areCodecParametersEqual.
bool sameCodecParameters(AVCodecParameters const &lhs, AVCodecParameters const &rhs) {
    if (lhs.codec_id != rhs.codec_id || lhs.extradata_size != rhs.extradata_size) {
        return false;
    }
    if (lhs.extradata_size != 0 && std::memcmp(lhs.extradata, rhs.extradata, (size_t)lhs.extradata_size) != 0) {
        return false;
    }
    return lhs.format == rhs.format &&
        lhs.profile == rhs.profile &&
        lhs.level == rhs.level &&
        lhs.width == rhs.width &&
        lhs.height == rhs.height &&
        lhs.sample_aspect_ratio.num == rhs.sample_aspect_ratio.num &&
        lhs.sample_aspect_ratio.den == rhs.sample_aspect_ratio.den &&
        lhs.field_order == rhs.field_order &&
        lhs.color_range == rhs.color_range &&
        lhs.color_primaries == rhs.color_primaries &&
        lhs.color_trc == rhs.color_trc &&
        lhs.color_space == rhs.color_space &&
        lhs.chroma_location == rhs.chroma_location;
}

// VideoToolbox when it is offered and a device exists; otherwise the first software format. A VideoToolbox setup that
// fails makes FFmpeg ask again without it.
enum AVPixelFormat chooseFormat(AVCodecContext *context, const enum AVPixelFormat *formats) {
    if (context->hw_device_ctx) {
        for (const enum AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; format++) {
            if (*format == AV_PIX_FMT_VIDEOTOOLBOX) {
                return *format;
            }
        }
    }
    for (const enum AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; format++) {
        const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get(*format);
        if (descriptor && !(descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            return *format;
        }
    }
    return AV_PIX_FMT_NONE;
}

} // namespace

struct VideoDecoderState {
    bool allowHardware = false;
    AVCodecContext *context = nullptr;
    AVCodecParameters *parameters = nullptr;
    AVRational timeBase = {0, 1};
    int creations = 0;
    int skipped = 0;
    // The part whose frames the codec holds.
    const void *activePart = nullptr;

    ~VideoDecoderState() {
        release();
    }

    void release() {
        if (context) {
            avcodec_free_context(&context);
        }
        if (parameters) {
            avcodec_parameters_free(&parameters);
        }
    }

    // The codec for these parameters: kept when they match the last part's, otherwise created anew.
    bool prepare(AVCodecParameters const *newParameters, AVRational newTimeBase) {
        if (context && parameters && sameCodecParameters(*parameters, *newParameters) && timeBase.num == newTimeBase.num && timeBase.den == newTimeBase.den) {
            return true;
        }
        release();
        creations++;
        const AVCodec *codec = avcodec_find_decoder(newParameters->codec_id);
        if (!codec) {
            return false;
        }
        context = avcodec_alloc_context3(codec);
        if (!context || avcodec_parameters_to_context(context, newParameters) < 0) {
            release();
            return false;
        }
        context->pkt_timebase = newTimeBase;
        if (allowHardware) {
            AVBufferRef *device = nullptr;
            if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) >= 0) {
                context->hw_device_ctx = device;
            }
        }
        context->get_format = chooseFormat;
        if (avcodec_open2(context, codec, nullptr) < 0) {
            release();
            return false;
        }
        parameters = avcodec_parameters_alloc();
        avcodec_parameters_copy(parameters, newParameters);
        timeBase = newTimeBase;
        return true;
    }
};

VideoDecoder::VideoDecoder(bool allowHardware) :
_state(std::make_unique<VideoDecoderState>()) {
    _state->allowHardware = allowHardware;
}

VideoDecoder::~VideoDecoder() = default;

int VideoDecoder::codecCreations() const {
    return _state->creations;
}

int VideoDecoder::skippedFrames() const {
    return _state->skipped;
}

struct VideoPartState {
    std::shared_ptr<VideoDecoder> decoder;
    std::unique_ptr<MemoryIO> io;
    AVFormatContext *format = nullptr;
    int streamIndex = -1;
    AVCodecParameters *parameters = nullptr;
    AVRational timeBase = {0, 1};
    std::string endpointId;
    int32_t rotation = 0;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    int frameIndex = 0;
    bool hasFirstPts = false;
    double firstPts = 0.0;
    bool sentEnd = false;

    ~VideoPartState() {
        if (format) {
            avformat_close_input(&format);
        }
        if (parameters) {
            avcodec_parameters_free(&parameters);
        }
        av_packet_free(&packet);
        av_frame_free(&frame);
    }
};

VideoPart::VideoPart() :
_state(std::make_unique<VideoPartState>()) {
}

VideoPart::~VideoPart() {
    if (!_state->decoder) {
        return;
    }
    VideoDecoderState &decoder = *_state->decoder->_state;
    if (decoder.activePart == _state.get()) {
        decoder.activePart = nullptr;
    }
}

std::unique_ptr<VideoPart> VideoPart::open(std::shared_ptr<VideoDecoder> decoder, const uint8_t *data, size_t size) {
    if (!decoder) {
        return nullptr;
    }
    const auto header = parsePartHeader(data, size);
    if (!header) {
        return nullptr;
    }
    const AVInputFormat *inputFormat = av_find_input_format(header->container.c_str());
    if (!inputFormat) {
        return nullptr;
    }
    auto part = std::unique_ptr<VideoPart>(new VideoPart());
    VideoPartState &state = *part->_state;
    state.decoder = std::move(decoder);
    state.endpointId = header->endpointId;
    state.rotation = header->rotation;
    state.io = std::make_unique<MemoryIO>(data + header->sliceOffset, header->sliceSize);
    state.packet = av_packet_alloc();
    state.frame = av_frame_alloc();
    if (!state.io->context() || !state.packet || !state.frame) {
        return nullptr;
    }
    state.format = avformat_alloc_context();
    if (!state.format) {
        return nullptr;
    }
    state.format->pb = state.io->context();
    if (avformat_open_input(&state.format, "", inputFormat, nullptr) < 0) {
        state.format = nullptr;
        return nullptr;
    }
    if (avformat_find_stream_info(state.format, nullptr) < 0) {
        return nullptr;
    }
    for (unsigned int i = 0; i < state.format->nb_streams; i++) {
        AVStream *stream = state.format->streams[i];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
            continue;
        }
        state.streamIndex = (int)i;
        state.parameters = avcodec_parameters_alloc();
        avcodec_parameters_copy(state.parameters, stream->codecpar);
        state.timeBase = stream->time_base;
        break;
    }
    if (state.streamIndex < 0) {
        return nullptr;
    }
    return part;
}

std::string const &VideoPart::endpointId() const {
    return _state->endpointId;
}

std::optional<VideoFrame> VideoPart::nextFrame() {
    VideoPartState &state = *_state;
    VideoDecoderState &decoder = *state.decoder->_state;
    auto finish = [&]() {
        // As the engine resets its decoder after draining a part.
        if (decoder.context) {
            avcodec_flush_buffers(decoder.context);
        }
        if (decoder.activePart == &state) {
            decoder.activePart = nullptr;
        }
        return std::optional<VideoFrame>();
    };
    if (decoder.activePart != &state || !decoder.context) {
        // Starting this part (or resuming it after another part's failed setup took the codec): the codec for its
        // parameters, cleared of whatever an earlier part left in it.
        if (!decoder.prepare(state.parameters, state.timeBase)) {
            // prepare released the codec: no part holds it now.
            decoder.activePart = nullptr;
            return std::nullopt;
        }
        avcodec_flush_buffers(decoder.context);
        decoder.activePart = &state;
    }
    while (true) {
        const int received = avcodec_receive_frame(decoder.context, state.frame);
        if (received == 0) {
            VideoFrame frame;
            frame.index = state.frameIndex;
            frame.rotation = state.rotation;
            frame.width = state.frame->width;
            frame.height = state.frame->height;
            const int64_t timestamp = state.frame->pts != AV_NOPTS_VALUE ? state.frame->pts : state.frame->best_effort_timestamp;
            const double seconds = (double)timestamp * av_q2d(state.timeBase);
            bool converted = true;
            if (state.frame->format == AV_PIX_FMT_VIDEOTOOLBOX && state.frame->data[3]) {
                frame.pixelBuffer = CVPixelBufferRetain((CVPixelBufferRef)state.frame->data[3]);
            } else if (state.frame->format == AV_PIX_FMT_YUV420P || state.frame->format == AV_PIX_FMT_YUVJ420P) {
                const int width = state.frame->width;
                const int height = state.frame->height;
                const int chromaWidth = (width + 1) / 2;
                const int chromaHeight = (height + 1) / 2;
                frame.strideY = width;
                frame.strideU = chromaWidth;
                frame.strideV = chromaWidth;
                frame.y.resize((size_t)(width * height));
                frame.u.resize((size_t)(chromaWidth * chromaHeight));
                frame.v.resize((size_t)(chromaWidth * chromaHeight));
                for (int row = 0; row < height; row++) {
                    std::memcpy(frame.y.data() + (size_t)(row * width), state.frame->data[0] + (size_t)row * (size_t)state.frame->linesize[0], (size_t)width);
                }
                for (int row = 0; row < chromaHeight; row++) {
                    std::memcpy(frame.u.data() + (size_t)(row * chromaWidth), state.frame->data[1] + (size_t)row * (size_t)state.frame->linesize[1], (size_t)chromaWidth);
                    std::memcpy(frame.v.data() + (size_t)(row * chromaWidth), state.frame->data[2] + (size_t)row * (size_t)state.frame->linesize[2], (size_t)chromaWidth);
                }
            } else {
                converted = false;
                decoder.skipped++;
            }
            av_frame_unref(state.frame);
            if (!converted) {
                continue;
            }
            if (!state.hasFirstPts) {
                state.hasFirstPts = true;
                state.firstPts = seconds;
            }
            frame.pts = seconds - state.firstPts;
            state.frameIndex++;
            return frame;
        }
        if (received != AVERROR(EAGAIN) || state.sentEnd) {
            return finish();
        }
        // The codec needs the next packet of the video stream, or the end of the stream to drain.
        while (true) {
            if (av_read_frame(state.format, state.packet) < 0) {
                avcodec_send_packet(decoder.context, nullptr);
                state.sentEnd = true;
                break;
            }
            if (state.packet->stream_index != state.streamIndex) {
                av_packet_unref(state.packet);
                continue;
            }
            const int sent = avcodec_send_packet(decoder.context, state.packet);
            av_packet_unref(state.packet);
            if (sent < 0 && sent != AVERROR(EAGAIN)) {
                return finish();
            }
            break;
        }
    }
}

VideoFrame const *VideoPart::frameAt(double seconds) {
    while (true) {
        while (_available.size() >= 2) {
            if (seconds >= _available[1].pts) {
                _available.pop_front();
            } else {
                break;
            }
        }
        if (_available.size() < 2 && !_finished) {
            auto frame = nextFrame();
            if (frame) {
                _available.push_back(std::move(*frame));
            } else {
                _finished = true;
            }
            continue;
        }
        if (_available.empty()) {
            return nullptr;
        }
        for (size_t i = 1; i < _available.size(); i++) {
            if (seconds < _available[i].pts) {
                return &_available[i - 1];
            }
        }
        return &_available.back();
    }
}

} // namespace broadcast_media
