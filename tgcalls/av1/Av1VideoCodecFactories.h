#ifndef TGCALLS_AV1_AV1VIDEOCODECFACTORIES_H
#define TGCALLS_AV1_AV1VIDEOCODECFACTORIES_H

#include <memory>

#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"

namespace tgcalls {

// `format` with the fmtp parameters WebRTC's SDP parser fills in when a description omits them
// (BackfillCodecParameters in pc/webrtc_sdp.cc): VP9 profile-id 0, H264 packetization-mode 0, AV1 profile 0,
// level-idx 5 and tier 0, H265 level-id 93 and tx-mode SRST. Values already present are kept.
webrtc::SdpVideoFormat WithSdpDefaultParameters(webrtc::SdpVideoFormat format);

// `inner`'s formats with AV1 appended last, so the codecs before it keep their payload types, each listed with
// WithSdpDefaultParameters. A send codec negotiated from a parsed remote description carries those defaults, and
// pinning it (RtpEncodingParameters::codec) needs exact parameter equality with the sender's own list
// (Codec::MatchesRtpCodec): a bare "AV1" or "H265" could never be pinned. Spelling out a default changes nothing on
// the wire, since the peer's parser fills in the same values. `inner` is handed its own formats back. Creating AV1
// gives the SVT-AV1 encoder when `allowAv1Encoding`, else the refusing encoder: a device lists AV1 so that a two-way
// video line carries it, but only a device allowed to encode AV1 ever does. Everything else comes from `inner`.
std::unique_ptr<webrtc::VideoEncoderFactory> WrapEncoderFactoryWithAv1(std::unique_ptr<webrtc::VideoEncoderFactory> inner,
                                                                       bool allowAv1Encoding);

// `inner`'s formats with a bare AV1 appended last, as Darwin's ObjC decoder factory lists it; creating AV1 gives WebRTC's
// dav1d decoder. The fake platform puts WrapDecoderFactoryWithSdpDefaults on top, as Darwin does.
std::unique_ptr<webrtc::VideoDecoderFactory> WrapDecoderFactoryWithDav1d(std::unique_ptr<webrtc::VideoDecoderFactory> inner);

// `inner`'s formats listed with WithSdpDefaultParameters, `inner` handed its own format back. Every AV1 decoder factory
// needs it, not only the encoder side: a two-way video line's codec list takes the DECODER's parameters
// (NegotiateCodecs(recv, send) in pc/media_session.cc keeps "ours"), and the camera's codec preferences, which come from
// the encoder's list, match it by exact parameter equality (MatchCodecPreference). An encoder listing "AV1" or "H265"
// with the defaults against a decoder listing them bare drops both from every offer and answer.
std::unique_ptr<webrtc::VideoDecoderFactory> WrapDecoderFactoryWithSdpDefaults(std::unique_ptr<webrtc::VideoDecoderFactory> inner);

} // namespace tgcalls

#endif
