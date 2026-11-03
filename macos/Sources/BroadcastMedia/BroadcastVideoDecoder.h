#ifndef BROADCAST_MEDIA_VIDEO_DECODER_H
#define BROADCAST_MEDIA_VIDEO_DECODER_H

#include <CoreVideo/CoreVideo.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace broadcast_media {

// One decoded frame of a video part. Hardware frames carry a pixel buffer (retained by the frame); software frames
// carry tight I420 planes.
struct VideoFrame {
    VideoFrame() = default;
    ~VideoFrame();
    VideoFrame(VideoFrame &&other) noexcept;
    VideoFrame &operator=(VideoFrame &&other) noexcept;
    VideoFrame(VideoFrame const &) = delete;
    VideoFrame &operator=(VideoFrame const &) = delete;

    // Within the part, from 0.
    int index = 0;
    // Seconds after the part's first frame.
    double pts = 0.0;
    // The part header's.
    int32_t rotation = 0;
    int width = 0;
    int height = 0;
    CVPixelBufferRef pixelBuffer = nullptr;
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;
    int strideY = 0;
    int strideU = 0;
    int strideV = 0;
};

struct VideoDecoderState;

// One endpoint's decoder. It keeps its codec while parts keep the same codec parameters and time base (the engine's
// areCodecParametersEqual), and is flushed between parts. H.264 and HEVC decode through VideoToolbox (FFmpeg's
// hwaccel) when hardware is allowed and the device offers it; everything else, or a refusal, decodes in software to
// I420. Software frames in any other pixel format are skipped and counted.
//
// Threads: a decoder and all its parts (open, frameAt, destruction) belong to one serial queue, and its parts are
// played one after another in segment order. frameAt decodes lazily, so call it on that queue, not on a display link;
// switching between two unfinished parts of one decoder flushes the codec each time.
class VideoDecoder {
public:
    explicit VideoDecoder(bool allowHardware);
    ~VideoDecoder();

    VideoDecoder(VideoDecoder const &) = delete;
    VideoDecoder &operator=(VideoDecoder const &) = delete;

    // How many times a codec was created (tests: a part with unchanged parameters reuses the codec).
    int codecCreations() const;
    int skippedFrames() const;

private:
    friend class VideoPart;
    std::unique_ptr<VideoDecoderState> _state;
};

struct VideoPartState;

// One part being played. Frames decode lazily, at most two ahead of the one shown.
class VideoPart {
public:
    // nullptr when the header is unreadable, its container unknown, or the slice has no video stream.
    static std::unique_ptr<VideoPart> open(std::shared_ptr<VideoDecoder> decoder, const uint8_t *data, size_t size);
    ~VideoPart();

    VideoPart(VideoPart const &) = delete;
    VideoPart &operator=(VideoPart const &) = delete;

    // The frame to show `seconds` after the part's first frame, as the engine's getFrameAtRelativeTimestamp picks it:
    // the latest whose pts is at or before `seconds`, the first frame before its pts, the last after the end. nullptr
    // when the part has no frames. The pointer is valid until the next call or the part's destruction.
    VideoFrame const *frameAt(double seconds);

    std::string const &endpointId() const;

private:
    VideoPart();
    std::optional<VideoFrame> nextFrame();

    std::unique_ptr<VideoPartState> _state;
    std::deque<VideoFrame> _available;
    bool _finished = false;
};

} // namespace broadcast_media

#endif
