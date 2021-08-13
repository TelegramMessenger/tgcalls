#include "VideoStreamingPart.h"

#include "rtc_base/logging.h"
#include "rtc_base/third_party/base64/base64.h"
#include "api/video/i420_buffer.h"

extern "C" {
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <string>
#include <set>
#include <map>

namespace tgcalls {

namespace {

static absl::optional<uint32_t> readInt32(std::string const &data, int &offset) {
    if (offset + 4 > data.length()) {
        return absl::nullopt;
    }

    int32_t value = 0;
    memcpy(&value, data.data() + offset, 4);
    offset += 4;

    return value;
}

struct ChannelUpdate {
    int frameIndex = 0;
    int id = 0;
    uint32_t ssrc = 0;
};

static std::vector<ChannelUpdate> parseChannelUpdates(std::string const &data, int &offset) {
    std::vector<ChannelUpdate> result;

    auto channels = readInt32(data, offset);
    if (!channels) {
        return {};
    }

    auto count = readInt32(data, offset);
    if (!count) {
        return {};
    }

    for (int i = 0; i < count.value(); i++) {
        auto frameIndex = readInt32(data, offset);
        if (!frameIndex) {
            return {};
        }

        auto channelId = readInt32(data, offset);
        if (!channelId) {
            return {};
        }

        auto ssrc = readInt32(data, offset);
        if (!ssrc) {
            return {};
        }

        ChannelUpdate update;
        update.frameIndex = frameIndex.value();
        update.id = channelId.value();
        update.ssrc = ssrc.value();

        result.push_back(update);
    }

    return result;
}

class AVIOContextImpl {
public:
    AVIOContextImpl(std::vector<uint8_t> &&fileData) :
    _fileData(std::move(fileData)) {
        _buffer.resize(4 * 1024);
        _context = avio_alloc_context(_buffer.data(), (int)_buffer.size(), 0, this, &AVIOContextImpl::read, NULL, &AVIOContextImpl::seek);
    }

    ~AVIOContextImpl() {
        av_free(_context);
    }

    static int read(void *opaque, unsigned char *buffer, int bufferSize) {
        AVIOContextImpl *instance = static_cast<AVIOContextImpl *>(opaque);

        int bytesToRead = std::min(bufferSize, ((int)instance->_fileData.size()) - instance->_fileReadPosition);
        if (bytesToRead < 0) {
            bytesToRead = 0;
        }

        if (bytesToRead > 0) {
            memcpy(buffer, instance->_fileData.data() + instance->_fileReadPosition, bytesToRead);
            instance->_fileReadPosition += bytesToRead;

            return bytesToRead;
        } else {
            return AVERROR_EOF;
        }
    }

    static int64_t seek(void *opaque, int64_t offset, int whence) {
        AVIOContextImpl *instance = static_cast<AVIOContextImpl *>(opaque);

        if (whence == 0x10000) {
            return (int64_t)instance->_fileData.size();
        } else {
            int64_t seekOffset = std::min(offset, (int64_t)instance->_fileData.size());
            if (seekOffset < 0) {
                seekOffset = 0;
            }
            instance->_fileReadPosition = (int)seekOffset;
            return seekOffset;
        }
    }

    AVIOContext *getContext() {
        return _context;
    }

private:
    std::vector<uint8_t> _fileData;
    int _fileReadPosition = 0;

    std::vector<uint8_t> _buffer;
    AVIOContext *_context = nullptr;
};

class MediaDataPacket {
public:
    MediaDataPacket() {
        _packet = new AVPacket();
        av_init_packet(_packet);
    }

    MediaDataPacket(MediaDataPacket &&other) {
        _packet = other._packet;
        other._packet = nullptr;
    }

    ~MediaDataPacket() {
        if (_packet) {
            av_packet_unref(_packet);
            delete _packet;
        }
    }

    AVPacket *packet() {
        return _packet;
    }

private:
    AVPacket *_packet = nullptr;
};

class DecodableFrame {
public:
    DecodableFrame(MediaDataPacket packet, int64_t pts, int64_t dts, int64_t duration):
    _packet(std::move(packet)),
    _pts(pts),
    _dts(dts),
    _duration(duration) {
    }

    ~DecodableFrame() {
    }

    MediaDataPacket &packet() {
        return _packet;
    }

    int64_t pts() {
        return _pts;
    }

    int64_t dts() {
        return _dts;
    }

    int64_t duration() {
        return _duration;
    }

private:
    MediaDataPacket _packet;
    int64_t _pts;
    int64_t _dts;
    int64_t _duration;
};

class Frame {
public:
    Frame() {
        _frame = av_frame_alloc();
    }

    Frame(Frame &&other) {
        _frame = other._frame;
        other._frame = nullptr;
    }

    ~Frame() {
        if (_frame) {
            av_frame_unref(_frame);
        }
    }

    AVFrame *frame() {
        return  _frame;
    }

private:
    AVFrame *_frame = nullptr;
};

}

class VideoStreamingPartInternal {
public:
    VideoStreamingPartInternal(std::vector<uint8_t> &&fileData) :
    _avIoContext(std::move(fileData)) {
        int ret = 0;

        AVInputFormat *inputFormat = av_find_input_format("mp4");
        if (!inputFormat) {
            _didReadToEnd = true;
            return;
        }

        _inputFormatContext = avformat_alloc_context();
        if (!_inputFormatContext) {
            _didReadToEnd = true;
            return;
        }

        _inputFormatContext->pb = _avIoContext.getContext();

        if ((ret = avformat_open_input(&_inputFormatContext, "", inputFormat, nullptr)) < 0) {
            _didReadToEnd = true;
            return;
        }

        if ((ret = avformat_find_stream_info(_inputFormatContext, nullptr)) < 0) {
            _didReadToEnd = true;

            avformat_close_input(&_inputFormatContext);
            _inputFormatContext = nullptr;
            return;
        }

        AVCodecParameters *videoCodecParameters = nullptr;
        AVStream *videoStream = nullptr;
        for (int i = 0; i < _inputFormatContext->nb_streams; i++) {
            AVStream *inStream = _inputFormatContext->streams[i];

            AVCodecParameters *inCodecpar = inStream->codecpar;
            if (inCodecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
                continue;
            }
            videoCodecParameters = inCodecpar;
            videoStream = inStream;

            /*if (inStream->metadata) {
                AVDictionaryEntry *entry = av_dict_get(inStream->metadata, "TG_META", nullptr, 0);
                if (entry && entry->value) {
                    std::string result;
                    size_t data_used = 0;
                    std::string sourceBase64 = (const char *)entry->value;
                    rtc::Base64::Decode(sourceBase64, rtc::Base64::DO_LAX, &result, &data_used);

                    if (result.size() != 0) {
                        int offset = 0;
                        _channelUpdates = parseChannelUpdates(result, offset);
                    }
                }
            }*/

            break;
        }

        if (videoCodecParameters && videoStream) {
            AVCodec *codec = avcodec_find_decoder(videoCodecParameters->codec_id);
            if (codec) {
                _codecContext = avcodec_alloc_context3(codec);
                ret = avcodec_parameters_to_context(_codecContext, videoCodecParameters);
                if (ret < 0) {
                    _didReadToEnd = true;

                    avcodec_free_context(&_codecContext);
                    _codecContext = nullptr;
                } else {
                    _codecContext->pkt_timebase = videoStream->time_base;

                    ret = avcodec_open2(_codecContext, codec, nullptr);
                    if (ret < 0) {
                        _didReadToEnd = true;

                        avcodec_free_context(&_codecContext);
                        _codecContext = nullptr;
                    } else {
                        _videoStream = videoStream;
                    }
                }
            }
        }
    }

    ~VideoStreamingPartInternal() {
        if (_codecContext) {
            avcodec_close(_codecContext);
            avcodec_free_context(&_codecContext);
        }
        if (_inputFormatContext) {
            avformat_close_input(&_inputFormatContext);
        }
    }

    absl::optional<MediaDataPacket> readPacket() {
        if (_didReadToEnd) {
            return absl::nullopt;
        }
        if (!_inputFormatContext) {
            return absl::nullopt;
        }

        MediaDataPacket packet;
        int result = av_read_frame(_inputFormatContext, packet.packet());
        if (result < 0) {
            _didReadToEnd = true;
            return absl::nullopt;
        }

        return packet;
    }

    std::shared_ptr<DecodableFrame> readNextDecodableFrame() {
        while (true) {
            absl::optional<MediaDataPacket> packet = readPacket();
            if (packet) {
                if (_videoStream && packet->packet()->stream_index == _videoStream->index) {
                    return std::make_shared<DecodableFrame>(std::move(packet.value()), packet->packet()->pts, packet->packet()->dts, packet->packet()->duration);
                }
            } else {
                return nullptr;
            }
        }
    }

    absl::optional<webrtc::VideoFrame> convertCurrentFrame() {
        rtc::scoped_refptr<webrtc::I420Buffer> i420Buffer = webrtc::I420Buffer::Copy(
            _frame.frame()->width,
            _frame.frame()->height,
            _frame.frame()->data[0],
            _frame.frame()->linesize[0],
            _frame.frame()->data[1],
            _frame.frame()->linesize[1],
            _frame.frame()->data[2],
            _frame.frame()->linesize[2]
        );
        if (i420Buffer) {
            auto videoFrame = webrtc::VideoFrame::Builder()
                .set_video_frame_buffer(i420Buffer)
                //.set_timestamp_us(timeStampNs)
                .build();
            return videoFrame;
        } else {
            return absl::nullopt;
        }
    }

    std::vector<webrtc::VideoFrame> getNextFrame() {
        if (!_codecContext) {
            return {};
        }

        while (true) {
            if (_didReadToEnd) {
                if (!_finalFrames.empty()) {
                    auto frame = _finalFrames[0];
                    _finalFrames.erase(_finalFrames.begin());
                    return { frame };
                } else {
                    break;
                }
            } else {
                const auto frame = readNextDecodableFrame();
                if (frame) {
                    auto status = avcodec_send_packet(_codecContext, frame->packet().packet());
                    if (status == 0) {
                        auto status = avcodec_receive_frame(_codecContext, _frame.frame());
                        if (status == 0) {
                            auto convertedFrame = convertCurrentFrame();
                            if (convertedFrame) {
                                _frameIndex++;
                                return { convertedFrame.value() };
                            }
                        } else if (status == -35) {
                            // more data needed
                        } else {
                            _didReadToEnd = true;
                            break;
                        }
                    } else {
                        _didReadToEnd = true;
                        return {};
                    }
                } else {
                    _didReadToEnd = true;
                    int status = avcodec_send_packet(_codecContext, nullptr);
                    if (status == 0) {
                        while (true) {
                            auto status = avcodec_receive_frame(_codecContext, _frame.frame());
                            if (status == 0) {
                                auto convertedFrame = convertCurrentFrame();
                                if (convertedFrame) {
                                    _finalFrames.push_back(convertedFrame.value());
                                }
                            } else {
                                break;
                            }
                        }
                    }
                }
            }
        }

        return {};
    }

private:
    AVIOContextImpl _avIoContext;

    AVFormatContext *_inputFormatContext = nullptr;
    AVPacket _packet;
    AVCodecContext *_codecContext = nullptr;
    AVStream *_videoStream = nullptr;
    Frame _frame;

    std::vector<webrtc::VideoFrame> _finalFrames;

    int _frameIndex = 0;
    bool _didReadToEnd = false;
};

class VideoStreamingPartState {
public:
    VideoStreamingPartState(std::vector<uint8_t> &&data) :
    _parsedPart(std::move(data)) {
    }

    ~VideoStreamingPartState() {
    }

    std::vector<webrtc::VideoFrame> getNextFrame() {
        return _parsedPart.getNextFrame();
    }

private:
    VideoStreamingPartInternal _parsedPart;
};

VideoStreamingPart::VideoStreamingPart(std::vector<uint8_t> &&data) {
    if (!data.empty()) {
        _state = new VideoStreamingPartState(std::move(data));
    }
}

VideoStreamingPart::~VideoStreamingPart() {
    if (_state) {
        delete _state;
    }
}

std::vector<webrtc::VideoFrame> VideoStreamingPart::getNextFrame() {
    return _state
        ? _state->getNextFrame()
        : std::vector<webrtc::VideoFrame>();
}

}
