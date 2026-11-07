#include <TgVoipWebrtc/BroadcastMediaC.h>

#include "BroadcastAudioCoreBridge.h"
#include "BroadcastListenerAudio.h"
#include "BroadcastPartHeader.h"
#include "BroadcastVideoDecoder.h"

#include <cstring>

bool BroadcastPartHeaderParse(const uint8_t *data, size_t size, BroadcastPartHeaderInfo *info) {
    const auto layout = broadcast_media::parsePartHeaderLayout(data, size);
    if (!layout || !info) {
        return false;
    }
    info->containerOffset = layout->containerOffset;
    info->containerLength = layout->containerLength;
    info->endpointOffset = layout->endpointOffset;
    info->endpointLength = layout->endpointLength;
    info->rotation = layout->rotation;
    info->sliceOffset = layout->sliceOffset;
    info->sliceSize = layout->sliceSize;
    return true;
}

bool BroadcastListenerAudioReadInfo(const uint8_t *data, size_t size, BroadcastListenerAudioInfo *info) {
    if (!info) {
        return false;
    }
    const auto parsed = broadcast_media::readListenerAudioInfo(data, size);
    if (!parsed) {
        return false;
    }
    std::memset(info, 0, sizeof(*info));
    info->durationMs = parsed->durationMs;
    const size_t capacity = sizeof(info->endpoints) / sizeof(info->endpoints[0]);
    for (auto const &it : parsed->videoChannels) {
        if (info->endpointCount == capacity || it.first.size() >= sizeof(info->endpoints[0].endpointId)) {
            return false;
        }
        BroadcastListenerAudioEndpoint &endpoint = info->endpoints[info->endpointCount];
        std::memcpy(endpoint.endpointId, it.first.data(), it.first.size());
        endpoint.endpointId[it.first.size()] = 0;
        endpoint.channelId = it.second;
        info->endpointCount++;
    }
    return true;
}

struct BroadcastListenerAudioPart {
    broadcast_media::DecodedListenerAudio audio;
};

BroadcastListenerAudioPartRef BroadcastListenerAudioDecode(const uint8_t *data, size_t size) {
    auto decoded = broadcast_media::decodeListenerAudio(data, size);
    if (!decoded) {
        return nullptr;
    }
    auto part = new BroadcastListenerAudioPart();
    part->audio = std::move(*decoded);
    return part;
}

void BroadcastListenerAudioPartRelease(BroadcastListenerAudioPartRef part) {
    delete part;
}

bool BroadcastListenerAudioPartEnqueue(BroadcastListenerAudioPartRef part, BroadcastAudioCoreRef core, int64_t timestampMs) {
    if (!part || !core) {
        return false;
    }
    const auto shared = BroadcastAudioCoreShared(core);
    return shared && broadcast_media::enqueueListenerAudio(*shared, timestampMs, part->audio);
}

struct BroadcastVideoDecoder {
    std::shared_ptr<broadcast_media::VideoDecoder> decoder;
};

struct BroadcastVideoPart {
    std::unique_ptr<broadcast_media::VideoPart> part;
};

BroadcastVideoDecoderRef BroadcastVideoDecoderCreate(bool allowHardware) {
    auto handle = new BroadcastVideoDecoder();
    handle->decoder = std::make_shared<broadcast_media::VideoDecoder>(allowHardware);
    return handle;
}

void BroadcastVideoDecoderRelease(BroadcastVideoDecoderRef decoder) {
    delete decoder;
}

BroadcastVideoPartRef BroadcastVideoPartOpen(BroadcastVideoDecoderRef decoder, const uint8_t *data, size_t size) {
    if (!decoder) {
        return nullptr;
    }
    auto part = broadcast_media::VideoPart::open(decoder->decoder, data, size);
    if (!part) {
        return nullptr;
    }
    auto handle = new BroadcastVideoPart();
    handle->part = std::move(part);
    return handle;
}

void BroadcastVideoPartRelease(BroadcastVideoPartRef part) {
    delete part;
}

bool BroadcastVideoPartFrameAt(BroadcastVideoPartRef part, double seconds, BroadcastVideoFrame *frame) {
    if (!part || !frame) {
        return false;
    }
    broadcast_media::VideoFrame const *decoded = part->part->frameAt(seconds);
    if (!decoded) {
        return false;
    }
    std::memset(frame, 0, sizeof(*frame));
    frame->index = decoded->index;
    frame->pts = decoded->pts;
    frame->rotation = decoded->rotation;
    frame->width = decoded->width;
    frame->height = decoded->height;
    frame->pixelBuffer = decoded->pixelBuffer;
    if (!decoded->pixelBuffer) {
        frame->y = decoded->y.data();
        frame->u = decoded->u.data();
        frame->v = decoded->v.data();
        frame->strideY = decoded->strideY;
        frame->strideU = decoded->strideU;
        frame->strideV = decoded->strideV;
    }
    return true;
}
