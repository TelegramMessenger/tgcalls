#ifndef TGCALLS_AV1_TOP_STREAM_ENCODER_H
#define TGCALLS_AV1_TOP_STREAM_ENCODER_H

#include "api/video_codecs/video_encoder.h"

#include <functional>
#include <memory>

namespace tgcalls {

// The encoder of a group screen share's AV1 stream (the simulcast adapter's stream hook, or the whole sender in Single):
// `av1`, handed the stream's VideoCodec as kVideoCodecAV1 (WebRTC may configure the stream as H264).
//
// When AV1 fails (InitEncode, or an Encode returning an error):
// - without `onFailure` (AV1 as the sender's codec: GroupAv1ScreencastLayers::Single, Every) the error goes to WebRTC,
//   which switches the sender's codec (the factory's encoder selector answers with H264);
// - with it (the AV1 top stream of Top) the stream sends nothing more and `onFailure` is called once. Top is WebRTC's
//   mixed-codec simulcast, which sends each stream with its configured codec: the engine then configures the stream
//   for H264, which recreates the encoder. An H264 image on the AV1-configured stream would go out as AV1.
//
// While AV1 encodes, the stream runs at most kAv1TopStreamMaxFramerate (software SVT-AV1): frames above it are dropped
// (never one carrying a keyframe request) and AV1 is configured and rated for that rate. The cap lives here, not on the
// stream's encoding: WebRTC changes an encoding's max_framerate without re-initialising the encoder.
constexpr int kAv1TopStreamMaxFramerate = 30;

std::unique_ptr<webrtc::VideoEncoder> CreateAv1TopStreamEncoder(
    std::unique_ptr<webrtc::VideoEncoder> av1,
    std::function<void()> onFailure);

} // namespace tgcalls

#endif
