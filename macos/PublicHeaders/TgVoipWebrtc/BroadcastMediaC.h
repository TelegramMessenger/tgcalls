#ifndef BROADCAST_MEDIA_C_H
#define BROADCAST_MEDIA_C_H

// Broadcast part parsing and decoding for Swift (the app and the testbench runner). The decoders live in
// submodules/TgVoipWebrtc/BroadcastMedia (C++); see its CLAUDE.md.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <CoreVideo/CoreVideo.h>

#include <TgVoipWebrtc/BroadcastAudioCoreC.h>

#ifdef __cplusplus
extern "C" {
#endif

// A video or unified part's header. The string fields are offsets and lengths into the parsed bytes.
typedef struct {
    size_t containerOffset;
    size_t containerLength;
    size_t endpointOffset;
    size_t endpointLength;
    int32_t rotation;
    // The media slice: from sliceOffset to the end of the part.
    size_t sliceOffset;
    size_t sliceSize;
} BroadcastPartHeaderInfo;

bool BroadcastPartHeaderParse(const uint8_t *_Nonnull data, size_t size, BroadcastPartHeaderInfo *_Nonnull info);

typedef struct {
    char endpointId[256];
    int32_t channelId;
} BroadcastListenerAudioEndpoint;

// What the controller needs from a listener-audio part, read without decoding: its length and its endpoint → video
// channel id map (ACTIVE_MASK / ENDPOINTS), sorted by endpoint.
typedef struct {
    int32_t durationMs;
    size_t endpointCount;
    BroadcastListenerAudioEndpoint endpoints[32];
} BroadcastListenerAudioInfo;

// False when the bytes are not an Ogg stream with audio, or an endpoint is longer than 255 bytes.
bool BroadcastListenerAudioReadInfo(const uint8_t *_Nonnull data, size_t size, BroadcastListenerAudioInfo *_Nonnull info);

typedef struct BroadcastListenerAudioPart *BroadcastListenerAudioPartRef;

// Decodes one listener-audio part on its own, with a fresh Opus decoder as the engine does: on any thread, in any
// order, again if needed. NULL when the bytes are not an Ogg stream with audio.
BroadcastListenerAudioPartRef _Nullable BroadcastListenerAudioDecode(const uint8_t *_Nonnull data, size_t size);
void BroadcastListenerAudioPartRelease(BroadcastListenerAudioPartRef _Nonnull part);

// Hands a decoded part to the audio core as segment `timestampMs`. Call it on the core's control thread, the one that
// makes every other BroadcastAudioCore control call (the core takes them through a single-producer queue). False when
// the core refuses it (64 segments in flight).
bool BroadcastListenerAudioPartEnqueue(BroadcastListenerAudioPartRef _Nonnull part, BroadcastAudioCoreRef _Nonnull core, int64_t timestampMs);

typedef struct BroadcastVideoDecoder *BroadcastVideoDecoderRef;
typedef struct BroadcastVideoPart *BroadcastVideoPartRef;

// A decoded frame, borrowed from its part until the next call on the part or its release (retain the pixel buffer to
// keep it). Hardware frames have a pixel buffer and no planes; software frames have I420 planes and no pixel buffer.
typedef struct {
    int32_t index;
    double pts;
    int32_t rotation;
    int32_t width;
    int32_t height;
    CVPixelBufferRef _Nullable pixelBuffer;
    const uint8_t *_Nullable y;
    const uint8_t *_Nullable u;
    const uint8_t *_Nullable v;
    int32_t strideY;
    int32_t strideU;
    int32_t strideV;
} BroadcastVideoFrame;

// One endpoint's decoder, kept across that endpoint's parts. A decoder and all its parts (open, frame, release) belong
// to one serial queue, and the parts play one after another in segment order. Taking a frame may decode, so do it on
// that queue, not on a display link. In Swift, `pixelBuffer` comes as Unmanaged: take it unretained (and retain it to
// keep it past the next call).
BroadcastVideoDecoderRef _Nonnull BroadcastVideoDecoderCreate(bool allowHardware);
void BroadcastVideoDecoderRelease(BroadcastVideoDecoderRef _Nonnull decoder);

// A video part (header and slice); NULL when it cannot be read. The part keeps its decoder alive.
BroadcastVideoPartRef _Nullable BroadcastVideoPartOpen(BroadcastVideoDecoderRef _Nonnull decoder, const uint8_t *_Nonnull data, size_t size);
void BroadcastVideoPartRelease(BroadcastVideoPartRef _Nonnull part);

// The frame to show `seconds` after the part's first frame; false when the part has no frames.
bool BroadcastVideoPartFrameAt(BroadcastVideoPartRef _Nonnull part, double seconds, BroadcastVideoFrame *_Nonnull frame);

#ifdef __cplusplus
}
#endif

#endif
