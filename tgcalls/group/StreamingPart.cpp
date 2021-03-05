#include "StreamingPart.h"

#include <opusfile/opusfile.h>
#include "rtc_base/logging.h"
#include "rtc_base/third_party/base64/base64.h"

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
    // channelUpdate frame_id:int channel_id:int ssrc:int = ChannelUpdate;
    // meta channels:int updates:vector<channelUpdate> = Meta;
    
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

}

absl::optional<StreamingPart> StreamingPart::parse(std::vector<uint8_t> const &data) {
    int error = OPUS_OK;
    OggOpusFile *opusFile = op_open_memory(data.data(), data.size(), &error);
    if (opusFile == nullptr || error != OPUS_OK) {
        if (opusFile) {
            op_free(opusFile);
        }
        return {};
    }
    
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
    
    auto opusHead = op_head(opusFile, 0);
    
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
