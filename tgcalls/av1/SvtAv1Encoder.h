#ifndef TGCALLS_AV1_SVTAV1ENCODER_H
#define TGCALLS_AV1_SVTAV1ENCODER_H

#include <memory>

#include "api/video_codecs/video_encoder.h"

namespace tgcalls {

// AV1 through SVT-AV1 in real-time mode: low delay, CBR, one layer (L1T1). A bitrate change reaches the running encoder;
// a new size (a second InitEncode, or a frame of another size) re-initialises it and starts with a keyframe. Every
// keyframe carries a sequence header.
std::unique_ptr<webrtc::VideoEncoder> CreateSvtAv1Encoder();

// Stands in for AV1 on a device that may negotiate AV1 but must never encode it: InitEncode fails, so WebRTC moves
// the sender to another negotiated codec. Logs "Av1: encoding refused on this device".
std::unique_ptr<webrtc::VideoEncoder> CreateRefusingAv1Encoder();

} // namespace tgcalls

#endif
