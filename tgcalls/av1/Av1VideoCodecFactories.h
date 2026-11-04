#ifndef TGCALLS_AV1_AV1VIDEOCODECFACTORIES_H
#define TGCALLS_AV1_AV1VIDEOCODECFACTORIES_H

#include <memory>

#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"

namespace tgcalls {

// `inner`'s formats with AV1 appended last, so the codecs before it keep their payload types. Creating AV1 gives the
// SVT-AV1 encoder when `allowAv1Encoding`, else the refusing encoder: a device lists AV1 so that a two-way video line
// carries it, but only a device allowed to encode AV1 ever does. Everything else comes from `inner`.
std::unique_ptr<webrtc::VideoEncoderFactory> WrapEncoderFactoryWithAv1(std::unique_ptr<webrtc::VideoEncoderFactory> inner,
                                                                       bool allowAv1Encoding);

// `inner`'s formats with AV1 appended last; creating AV1 gives WebRTC's dav1d decoder.
std::unique_ptr<webrtc::VideoDecoderFactory> WrapDecoderFactoryWithDav1d(std::unique_ptr<webrtc::VideoDecoderFactory> inner);

} // namespace tgcalls

#endif
