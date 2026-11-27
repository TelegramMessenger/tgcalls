#ifndef TGCALLS_AV1_TOP_LAYER_FACTORY_H
#define TGCALLS_AV1_TOP_LAYER_FACTORY_H

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "group/GroupAv1Decision.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace tgcalls {

// Whether, and how, a group screen share's encoder factory encodes AV1. Shared by the engine (which decides,
// GroupAv1Decision, before its outgoing video is set up) and its encoder factory (which reads it when WebRTC creates the
// encoder). Thread-safe.
class Av1TopLayerSwitch {
public:
    void setEnabled(bool enabled);
    bool enabled() const;

    // How the share sends AV1 (GroupInstanceDescriptor::av1ScreencastLayers). Read when an encoder is created.
    void setLayers(GroupAv1ScreencastLayers layers);
    GroupAv1ScreencastLayers layers() const;

    // Called (on an encoder thread) when AV1 failed and H264 took over for good (a stream's encoder in Top and Every, the
    // whole sender in Single); the engine posts to its own thread from it.
    void setOnFallback(std::function<void()> onFallback);
    void reportFallback();

    // Testing (the CLI's --av1-test-encoder-fails): the top stream's AV1 encoder fails InitEncode.
    void setTestAv1Fails(bool fails);
    bool testAv1Fails() const;

private:
    std::atomic<bool> _enabled{false};
    std::atomic<GroupAv1ScreencastLayers> _layers{GroupAv1ScreencastLayers::Top};
    std::atomic<bool> _testAv1Fails{false};
    mutable std::mutex _mutex;
    std::function<void()> _onFallback;
};

// What the AV1 top layer needs from a platform's screen-share encoders.
struct Av1TopLayerPlatform {
    // A simulcast encoder of `format` (tgcalls' CustomSimulcastEncoderAdapter over the platform's H264 encoders) whose
    // top stream, or with `everyStream` each of its streams whatever their number (`format` then AV1), comes from
    // `streamEncoder`.
    std::function<std::unique_ptr<webrtc::VideoEncoder>(const webrtc::Environment &env,
                                                        const webrtc::SdpVideoFormat &format,
                                                        std::function<std::unique_ptr<webrtc::VideoEncoder>()> streamEncoder,
                                                        bool everyStream)> makeSimulcastEncoder;
};

// The screen share's encoder factory with AV1. It lists `inner`'s formats and AV1 (the group table's entry,
// GroupVideoPayloadTypes, so a negotiated AV1 106 survives WebRTC's match against the encoder factory and the sender
// learns its payload type). While `av1TopLayer` is enabled:
// - Top: H264 is `platform`'s simulcast encoder whose top stream is SVT-AV1 (CreateAv1TopStreamEncoder; WebRTC's
//   mixed-codec simulcast sends that stream as AV1, its encoding's codec); when AV1 fails the stream stops and the
//   engine configures it for H264. AV1 itself is refused, since the sender's first codec stays H264.
// - Single, Every: AV1 is the sender's codec, SVT-AV1 at most kAv1TopStreamMaxFramerate with no fallback of its own (a
//   failure reaches WebRTC, whose switch the selector answers with H264): in Every `platform`'s simulcast encoder with
//   an SVT-AV1 for every stream (WebRTC's AV1 simulcast, a scalability mode on each encoding). H264 is `inner`'s.
// Anything else, or the switch off: `inner`'s encoder, as today.
std::unique_ptr<webrtc::VideoEncoderFactory> WrapScreencastEncoderFactoryWithAv1TopLayer(
    std::unique_ptr<webrtc::VideoEncoderFactory> inner,
    Av1TopLayerPlatform platform,
    std::shared_ptr<Av1TopLayerSwitch> av1TopLayer);

} // namespace tgcalls

#endif
