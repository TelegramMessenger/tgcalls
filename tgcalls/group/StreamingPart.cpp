#include "StreamingPart.h"

#include <opusfile/opusfile.h>
#include "rtc_base/logging.h"
#include "rtc_base/third_party/base64/base64.h"

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
        _context = avio_alloc_context(_buffer.data(), _buffer.size(), 0, this, &AVIOContextImpl::read, NULL, &AVIOContextImpl::seek);
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
        }
        
        return bytesToRead;
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

}

class StreamingPartInternal {
public:
    StreamingPartInternal(std::vector<uint8_t> &&fileData) :
    _avIoContext(std::move(fileData)) {
#ifdef DEBUG
    av_log_set_level(AV_LOG_ERROR);
#else
    av_log_set_level(AV_LOG_QUIET);
#endif
    av_register_all();
        
        int ret = 0;
        
        _frame = av_frame_alloc();
        
        AVInputFormat *inputFormat = av_find_input_format("ogg");
        if (!inputFormat) {
            return;
        }
        
        _inputFormatContext = avformat_alloc_context();
        if (!_inputFormatContext) {
            return;
        }
        
        _inputFormatContext->pb = _avIoContext.getContext();
        
        if ((ret = avformat_open_input(&_inputFormatContext, "", inputFormat, nullptr)) < 0) {
            return;
        }
        
        if ((ret = avformat_find_stream_info(_inputFormatContext, nullptr)) < 0) {
            avformat_close_input(&_inputFormatContext);
            _inputFormatContext = nullptr;
            
            return;
        }
        
        AVCodecParameters *audioCodecParameters = nullptr;
        for (int i = 0; i < _inputFormatContext->nb_streams; i++) {
            AVStream *inStream = _inputFormatContext->streams[i];
            
            AVCodecParameters *inCodecpar = inStream->codecpar;
            if (inCodecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
                continue;
            }
            audioCodecParameters = inCodecpar;
            
            break;
        }
        
        if (audioCodecParameters) {
            AVCodec *codec = avcodec_find_decoder(audioCodecParameters->codec_id);
            if (codec) {
                _codecContext = avcodec_alloc_context3(codec);
                ret = avcodec_parameters_to_context(_codecContext, audioCodecParameters);
                if (ret < 0) {
                    avcodec_free_context(&_codecContext);
                    _codecContext = nullptr;
                } else {
                    ret = avcodec_open2(_codecContext, codec, nullptr);
                    if (ret < 0) {
                        avcodec_free_context(&_codecContext);
                        _codecContext = nullptr;
                    }
                }
            }
        }
    }
    
    ~StreamingPartInternal() {
        if (_frame) {
            av_frame_unref(_frame);
        }
        if (_codecContext) {
            avcodec_free_context(&_codecContext);
        }
        if (_inputFormatContext) {
            avformat_close_input(&_inputFormatContext);
        }
    }
    
    int readPcm(std::vector<uint8_t> &outPcm) {
        int outPcmOffset = 0;
        
        while (!_didReadToEnd && outPcmOffset < outPcm.size()) {
            if (_pcmBufferOffset >= _pcmBufferSize) {
                fillPcmBuffer();
            }
            
            int readFromPcmBuffer = std::min(_pcmBufferSize - _pcmBufferOffset, ((int)outPcm.size()) - outPcmOffset);
            if (readFromPcmBuffer != 0) {
                memcpy(outPcm.data() + outPcmOffset, _pcmBuffer.data() + _pcmBufferOffset, readFromPcmBuffer);
                _pcmBufferOffset += readFromPcmBuffer;
                outPcmOffset += readFromPcmBuffer;
            }
        }
        
        return outPcmOffset;
    }
    
private:
    void fillPcmBuffer() {
        if (_didReadToEnd) {
            return;
        }
        if (!_inputFormatContext) {
            _didReadToEnd = true;
            return;
        }
        if (!_codecContext) {
            _didReadToEnd = true;
            return;
        }
        
        int ret = 0;
        
        ret = av_read_frame(_inputFormatContext, &_packet);
        if (ret < 0) {
            _didReadToEnd = true;
            return;
        }
        
        ret = avcodec_send_packet(_codecContext, &_packet);
        if (ret < 0) {
            _didReadToEnd = true;
            return;
        }
        
        int bytesPerSample = av_get_bytes_per_sample(_codecContext->sample_fmt);
        if (bytesPerSample != 2) {
            _didReadToEnd = true;
            return;
        }
        
        while (ret >= 0) {
            ret = avcodec_receive_frame(_codecContext, _frame);
            if (ret != 0) {
                _didReadToEnd = true;
                return;
            }
            
            if (_pcmBuffer.size() < _frame->nb_samples * bytesPerSample * _frame->channels) {
                _pcmBuffer.resize(_frame->nb_samples * bytesPerSample * _frame->channels);
            }
            
            memcpy(_pcmBuffer.data(), _frame->data[0], _frame->nb_samples * bytesPerSample * _frame->channels);
            _pcmBufferSize = _frame->nb_samples * bytesPerSample * _frame->channels;
            _pcmBufferOffset = 0;
        }
    }
    
private:
    AVIOContextImpl _avIoContext;
    
    AVFormatContext *_inputFormatContext = nullptr;
    AVPacket _packet;
    AVCodecContext *_codecContext = nullptr;
    AVFrame *_frame = nullptr;
    
    bool _didReadToEnd = false;
    
    std::vector<uint8_t> _pcmBuffer;
    int _pcmBufferOffset = 0;
    int _pcmBufferSize = 0;
};

absl::optional<StreamingPart> StreamingPart::parse(std::vector<uint8_t> const &data) {
    /*std::vector<uint8_t> dataCopy = data;
    StreamingPartInternal test(std::move(dataCopy));
    std::vector<uint8_t> testOutPcm;
    testOutPcm.resize(48000);
    test.readPcm(testOutPcm);*/
    
    int error = OPUS_OK;
    OggOpusFile *opusFile = op_open_memory(data.data(), data.size(), &error);
    if (opusFile == nullptr || error != OPUS_OK) {
        if (opusFile) {
            op_free(opusFile);
        }
        return {};
    }
    
    auto opusHead = op_head(opusFile, 0);
    
    std::vector<ChannelUpdate> channelUpdates;
    
    auto tags = op_tags(opusFile, 0);
    if (tags && tags->comments != 0) {
        for (int i = 0; i < tags->comments; i++) {
            std::string comment = std::string(tags->user_comments[i], tags->comment_lengths[i]);
            
            if (comment.find("tg_meta=", 0) == 0) {
                comment.replace(0, 8, "");
                std::string result;
                size_t data_used = 0;
                rtc::Base64::Decode(comment, rtc::Base64::DO_LAX, &result, &data_used);
                
                if (result.size() != 0) {
                    int offset = 0;
                    channelUpdates = parseChannelUpdates(result, offset);
                }
            }
        }
    }
    
    StreamingPart results;
    std::set<uint32_t> allSsrcs;
    
    if (channelUpdates.size() == 0) {
        return results;
    }
    
    for (const auto &it : channelUpdates) {
        allSsrcs.insert(it.ssrc);
    }
    
    for (const auto ssrc : allSsrcs) {
        StreamingPartChannel emptyPart;
        emptyPart.ssrc = ssrc;
        results.channels.push_back(emptyPart);
    }
    
    float *interleavedChannelBuffer = (float *)malloc(5670 * opusHead->channel_count);
    float **separatedChannelBuffer = (float **)malloc(sizeof(float *) * opusHead->channel_count);
    for (int i = 0; i < opusHead->channel_count; i++) {
        separatedChannelBuffer[i] = (float *)malloc(5670);
    }
    
    std::map<uint32_t, int> currentChannelIdToSsrcMapping;
    
    for (const auto &update : channelUpdates) {
        if (currentChannelIdToSsrcMapping.find(update.ssrc) == currentChannelIdToSsrcMapping.end()) {
            currentChannelIdToSsrcMapping.insert(std::make_pair(update.ssrc, update.id));
        }
    }
    
    int frameIndex = 0;
    while (true) {
        int readResult = op_read_float(opusFile, interleavedChannelBuffer, 5760, NULL);
        if (readResult > 0) {
            int readBytesPerChannel = readResult * 2;
            
            for (int i = 0; i < readResult; i++) {
                for (int j = 0; j < opusHead->channel_count; j++) {
                    separatedChannelBuffer[j][i] = interleavedChannelBuffer[i * opusHead->channel_count + j];
                }
            }
            
            for (const auto &update : channelUpdates) {
                if (update.frameIndex == frameIndex || update.frameIndex == frameIndex + 1) {
                    currentChannelIdToSsrcMapping.insert(std::make_pair(update.ssrc, update.id));
                }
            }
            
            for (int i = 0; i < results.channels.size(); i++) {
                auto it = currentChannelIdToSsrcMapping.find(results.channels[i].ssrc);
                
                int sourceChannelIndex = -1;
                if (it != currentChannelIdToSsrcMapping.end()) {
                    sourceChannelIndex = it->second;
                }
                
                int samplesOffset = (int)(results.channels[i].pcmData.size());
                
                results.channels[i].pcmData.resize(results.channels[i].pcmData.size() + readBytesPerChannel);
                int16_t *samples = (int16_t *)(results.channels[i].pcmData.data() + samplesOffset);

                if (sourceChannelIndex != -1) {
                    for (int j = 0; j < readResult; j++) {
                        samples[j] = (int16_t)(separatedChannelBuffer[sourceChannelIndex][j] * INT16_MAX);
                    }
                } else {
                    for (int j = 0; j < readResult; j++) {
                        samples[j] = 0;
                    }
                }
            }
            
            frameIndex += 2;
        } else {
            break;
        }
    }
    
    free(interleavedChannelBuffer);
    for (int i = 0; i < opusHead->channel_count; i++) {
        free(separatedChannelBuffer[i]);
    }
    free(separatedChannelBuffer);
    op_free(opusFile);
    
    return results;
}

}
