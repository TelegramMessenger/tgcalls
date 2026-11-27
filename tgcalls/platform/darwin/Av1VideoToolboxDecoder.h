#ifndef TGCALLS_PLATFORM_DARWIN_AV1VIDEOTOOLBOXDECODER_H
#define TGCALLS_PLATFORM_DARWIN_AV1VIDEOTOOLBOXDECODER_H

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace tgcalls {

// AV1 through VideoToolbox, for chips that decode AV1 in hardware. Takes temporal units as RTP delivers them
// (low-overhead OBUs), builds the format description from the first sequence header and recreates the session when a
// later sequence header changes it. Decodes synchronously; the output is full-range NV12, as tgcalls' H.264 and H.265
// VideoToolbox decoders give.
class Av1VideoToolboxDecoder {
public:
    using Output = std::function<void(CVPixelBufferRef pixelBuffer, uint32_t rtpTimestamp)>;

    static bool IsSupported();

    explicit Av1VideoToolboxDecoder(Output output);
    ~Av1VideoToolboxDecoder();

    Av1VideoToolboxDecoder(const Av1VideoToolboxDecoder &) = delete;
    Av1VideoToolboxDecoder &operator=(const Av1VideoToolboxDecoder &) = delete;

    // False when the frame could not be decoded, including a frame before any sequence header (a keyframe is needed).
    bool Decode(const uint8_t *temporalUnit, size_t size, uint32_t rtpTimestamp);

    struct CallbackContext {
        Output output;
        OSStatus status = 0;
    };

private:
    bool recreateSession(const std::vector<uint8_t> &record, uint32_t width, uint32_t height, bool fullRange);
    void destroySession();

    std::unique_ptr<CallbackContext> _context;
    std::vector<uint8_t> _record;
    uint32_t _width = 0;
    uint32_t _height = 0;
    bool _fullRange = false;
    CMVideoFormatDescriptionRef _format = nullptr;
    VTDecompressionSessionRef _session = nullptr;
};

} // namespace tgcalls

#endif
