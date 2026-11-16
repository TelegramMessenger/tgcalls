#ifndef TGCALLS_AV1_AV1ENCODERFALLBACK_H
#define TGCALLS_AV1_AV1ENCODERFALLBACK_H

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder_factory.h"

namespace tgcalls {

// What happens when the AV1 encoder fails, shared by InstanceV2ReferenceImpl and its encoder factory. WebRTC replaces
// a broken encoder (not created, InitEncode failed, Encode failed: VideoStreamEncoder::RequestEncoderSwitch) with the
// codec the factory's encoder selector names, and with VP8 when it names none. Here a broken AV1 encoder is replaced by
// the codec the engine would pin next (H.265, else H.264: setFallbackFormat), and the engine is told so that it stops
// pinning AV1 for the rest of the call.
class Av1EncoderFallback {
public:
    // `onAv1EncoderFailed` runs on the thread where the encoder failed, once per failure.
    explicit Av1EncoderFallback(std::function<void()> onAv1EncoderFailed);

    // The codec to switch to; none until the first negotiation, and then WebRTC's own choice stands.
    void setFallbackFormat(std::optional<webrtc::SdpVideoFormat> format);

    // For the factory's encoders and selector (WrapEncoderFactoryWithAv1Fallback); any thread.
    void reportAv1Failure();
    std::optional<webrtc::SdpVideoFormat> fallbackFormat() const;
    void noteCreated(bool isAv1);
    bool lastCreatedIsAv1() const;

private:
    std::function<void()> _onAv1EncoderFailed;
    mutable std::mutex _mutex;
    std::optional<webrtc::SdpVideoFormat> _fallbackFormat;
    bool _lastCreatedIsAv1 = false;
};

// `inner` with AV1 encoders that report their failures to `fallback`, and an encoder selector that answers a broken
// AV1 encoder with `fallback`'s format. Everything else is `inner`'s, its own selector included.
std::unique_ptr<webrtc::VideoEncoderFactory> WrapEncoderFactoryWithAv1Fallback(
    std::unique_ptr<webrtc::VideoEncoderFactory> inner, std::shared_ptr<Av1EncoderFallback> fallback);

} // namespace tgcalls

#endif
