// Host test (macOS) of H.265 video on WebRTC M159 as the iOS app sends and receives it, without a device:
// - VideoToolbox encodes HEVC and h265_nalu_rewriter turns each frame into Annex B with the parameter sets before every
//   keyframe, as TGRTCVideoEncoderH265 does;
// - WebRTC's RTP packetizer and depacketizer carry it, and H26xPacketBuffer assembles frames, as
//   RtpVideoStreamReceiver2 does (since M159 it needs VPS/SPS/PPS inside a keyframe and starts a stream only on one);
// - VideoToolbox decodes the assembled frames through h265_nalu_rewriter, as TGRTCVideoDecoderH265 does.

#include "platform/darwin/h265_nalu_rewriter.h"

#include "api/video/video_codec_type.h"
#include "api/video/video_frame_type.h"
#include "modules/rtp_rtcp/source/create_video_rtp_depacketizer.h"
#include "modules/rtp_rtcp/source/rtp_format.h"
#include "modules/rtp_rtcp/source/rtp_packet_received.h"
#include "modules/rtp_rtcp/source/rtp_packet_to_send.h"
#include "modules/rtp_rtcp/source/rtp_video_header.h"
#include "modules/rtp_rtcp/source/video_rtp_depacketizer.h"
#include "modules/video_coding/h26x_packet_buffer.h"
#include "rtc_base/buffer.h"
#include "rtc_base/copy_on_write_buffer.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <memory>
#include <span>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kFrames = 60;
constexpr int kKeyframeInterval = 30;

struct EncodedFrame {
    std::vector<uint8_t> annexb;
    bool keyframe = false;
};

void onEncoded(void *context, void *, OSStatus status, VTEncodeInfoFlags, CMSampleBufferRef sampleBuffer) {
    if (status != noErr || !sampleBuffer) {
        return;
    }
    bool keyframe = true;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, 0);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        auto attachment = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
        keyframe = !CFDictionaryContainsKey(attachment, kCMSampleAttachmentKey_NotSync);
    }
    webrtc::Buffer buffer;
    if (!webrtc::H265CMSampleBufferToAnnexBBuffer(sampleBuffer, keyframe, &buffer)) {
        return;
    }
    EncodedFrame frame;
    frame.annexb.assign(buffer.data(), buffer.data() + buffer.size());
    frame.keyframe = keyframe;
    static_cast<std::vector<EncodedFrame> *>(context)->push_back(std::move(frame));
}

void setProperty(VTSessionRef session, CFStringRef key, int32_t value) {
    CFNumberRef number = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value);
    VTSessionSetProperty(session, key, number);
    CFRelease(number);
}

// A moving gradient, so delta frames carry data.
CVPixelBufferRef makePicture(int index) {
    CVPixelBufferRef pixelBuffer = nullptr;
    CVPixelBufferCreate(nullptr, kWidth, kHeight, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, nullptr, &pixelBuffer);
    if (!pixelBuffer) {
        return nullptr;
    }
    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    auto *luma = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
    const size_t lumaStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
    for (int y = 0; y < kHeight; y++) {
        for (int x = 0; x < kWidth; x++) {
            luma[y * lumaStride + x] = static_cast<uint8_t>((x + y + index * 8) & 0xff);
        }
    }
    auto *chroma = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));
    const size_t chromaStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
    for (int y = 0; y < kHeight / 2; y++) {
        std::memset(chroma + y * chromaStride, 128, kWidth);
    }
    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
    return pixelBuffer;
}

// Real-time HEVC without frame reordering, a keyframe every kKeyframeInterval frames (TGRTCVideoEncoderH265's session).
std::vector<EncodedFrame> encode() {
    std::vector<EncodedFrame> frames;
    VTCompressionSessionRef session = nullptr;
    const OSStatus status = VTCompressionSessionCreate(nullptr, kWidth, kHeight, kCMVideoCodecType_HEVC, nullptr, nullptr,
                                                       nullptr, onEncoded, &frames, &session);
    if (status != noErr || !session) {
        std::printf("VTCompressionSessionCreate (HEVC) failed: %d\n", (int)status);
        return frames;
    }
    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    setProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kKeyframeInterval);
    setProperty(session, kVTCompressionPropertyKey_AverageBitRate, 1500000);
    VTCompressionSessionPrepareToEncodeFrames(session);
    for (int i = 0; i < kFrames; i++) {
        CVPixelBufferRef picture = makePicture(i);
        if (!picture) {
            break;
        }
        CFDictionaryRef options = nullptr;
        if (i % kKeyframeInterval == 0) {
            const void *keys[] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
            const void *values[] = { kCFBooleanTrue };
            options = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        }
        VTCompressionSessionEncodeFrame(session, picture, CMTimeMake(i, 30), kCMTimeInvalid, options, nullptr, nullptr);
        VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        if (options) {
            CFRelease(options);
        }
        CVPixelBufferRelease(picture);
    }
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    return frames;
}

// WebRTC's H.265 packetizer at the app's payload size; one RTP timestamp per frame.
std::vector<webrtc::CopyOnWriteBuffer> packetize(std::vector<EncodedFrame> const &frames, std::vector<int> *frameOfPacket) {
    std::vector<webrtc::CopyOnWriteBuffer> packets;
    uint16_t sequenceNumber = 1000;
    for (size_t i = 0; i < frames.size(); i++) {
        webrtc::RTPVideoHeader header;
        header.codec = webrtc::kVideoCodecH265;
        header.frame_type = frames[i].keyframe ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
        header.width = kWidth;
        header.height = kHeight;
        webrtc::RtpPacketizer::PayloadSizeLimits limits;
        limits.max_payload_len = 1200;
        auto packetizer = webrtc::RtpPacketizer::Create(webrtc::RtpPacketizer::PacketizationFormat::kH265,
                                                        frames[i].annexb, limits, header);
        while (true) {
            webrtc::RtpPacketToSend packet(nullptr);
            packet.SetPayloadType(100);
            packet.SetSequenceNumber(sequenceNumber);
            packet.SetTimestamp(static_cast<uint32_t>(i * 3000));
            packet.SetSsrc(1234);
            if (!packetizer->NextPacket(&packet)) {
                break;
            }
            sequenceNumber++;
            packets.push_back(packet.Buffer());
            frameOfPacket->push_back(static_cast<int>(i));
        }
    }
    return packets;
}

struct AssembledFrame {
    std::vector<uint8_t> annexb;
    bool keyframe = false;
    uint32_t rtpTimestamp = 0;
};

// The receive side of RtpVideoStreamReceiver2 for H.265: depacketize, H26xPacketBuffer, then each frame's payloads
// from its first packet to its last.
std::vector<AssembledFrame> receive(std::vector<webrtc::CopyOnWriteBuffer> const &packets, size_t firstPacket) {
    std::vector<AssembledFrame> frames;
    auto depacketizer = webrtc::CreateVideoRtpDepacketizer(webrtc::kVideoCodecH265);
    webrtc::H26xPacketBuffer packetBuffer(/*h264_idr_only_keyframes_allowed=*/false);
    AssembledFrame current;
    for (size_t i = firstPacket; i < packets.size(); i++) {
        webrtc::RtpPacketReceived rtp;
        if (!rtp.Parse(packets[i])) {
            CHECK_TRUE(false && "RTP parse");
            continue;
        }
        auto parsed = depacketizer->Parse(rtp.PayloadBuffer());
        if (!parsed) {
            CHECK_TRUE(false && "depacketize");
            continue;
        }
        parsed->video_header.is_last_packet_in_frame = rtp.Marker();
        auto packet = std::make_unique<webrtc::H26xPacketBuffer::Packet>(rtp, 1000 + static_cast<int64_t>(i), parsed->video_header);
        packet->video_payload = parsed->video_payload;
        auto result = packetBuffer.InsertPacket(std::move(packet));
        for (auto &assembled : result.packets) {
            if (assembled->is_first_packet_in_frame()) {
                current = AssembledFrame();
                current.keyframe = assembled->video_header.frame_type == webrtc::VideoFrameType::kVideoFrameKey;
                current.rtpTimestamp = assembled->timestamp;
            }
            current.annexb.insert(current.annexb.end(), assembled->video_payload.cdata(),
                                  assembled->video_payload.cdata() + assembled->video_payload.size());
            if (assembled->is_last_packet_in_frame()) {
                frames.push_back(std::move(current));
                current = AssembledFrame();
            }
        }
    }
    return frames;
}

struct DecodeCounts {
    int decoded = 0;
    int wrongSize = 0;
    int errors = 0;
};

void onDecoded(void *context, void *, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image, CMTime, CMTime) {
    auto *counts = static_cast<DecodeCounts *>(context);
    if (status != noErr || !image) {
        counts->errors++;
        return;
    }
    counts->decoded++;
    if ((int)CVPixelBufferGetWidth(image) != kWidth || (int)CVPixelBufferGetHeight(image) != kHeight) {
        counts->wrongSize++;
    }
}

// TGRTCVideoDecoderH265's path: the format from a keyframe's parameter sets, the frame as an hvcc sample.
DecodeCounts decode(std::vector<AssembledFrame> const &frames) {
    DecodeCounts counts;
    CMVideoFormatDescriptionRef format = nullptr;
    VTDecompressionSessionRef session = nullptr;
    for (const auto &frame : frames) {
        if (frame.keyframe) {
            CMVideoFormatDescriptionRef keyFormat = webrtc::CreateH265VideoFormatDescription(frame.annexb.data(), frame.annexb.size());
            if (!keyFormat) {
                counts.errors++;
                continue;
            }
            if (!format || !CMFormatDescriptionEqual(format, keyFormat)) {
                if (session) {
                    VTDecompressionSessionInvalidate(session);
                    CFRelease(session);
                    session = nullptr;
                }
                if (format) {
                    CFRelease(format);
                }
                format = keyFormat;
                VTDecompressionOutputCallbackRecord callback = { onDecoded, &counts };
                if (VTDecompressionSessionCreate(nullptr, format, nullptr, nullptr, &callback, &session) != noErr) {
                    counts.errors++;
                    session = nullptr;
                }
            } else {
                CFRelease(keyFormat);
            }
        }
        if (!session) {
            counts.errors++;
            continue;
        }
        CMSampleBufferRef sample = nullptr;
        if (!webrtc::H265AnnexBBufferToCMSampleBuffer(frame.annexb.data(), frame.annexb.size(), format, &sample)) {
            counts.errors++;
            continue;
        }
        VTDecodeInfoFlags infoFlags = 0;
        if (VTDecompressionSessionDecodeFrame(session, sample, 0, nullptr, &infoFlags) != noErr) {
            counts.errors++;
        }
        CFRelease(sample);
    }
    if (session) {
        VTDecompressionSessionWaitForAsynchronousFrames(session);
        VTDecompressionSessionInvalidate(session);
        CFRelease(session);
    }
    if (format) {
        CFRelease(format);
    }
    return counts;
}

// Every frame of the stream arrives, is assembled (keyframes carry VPS/SPS/PPS, which H26xPacketBuffer requires) and
// decodes at its size.
void TestEveryFrameArrivesAndDecodes(std::vector<EncodedFrame> const &encoded) {
    std::vector<int> frameOfPacket;
    const auto packets = packetize(encoded, &frameOfPacket);
    const auto assembled = receive(packets, 0);
    std::printf("full stream: %d encoded (%d keyframes), %d packets, %d assembled\n", (int)encoded.size(),
                (int)std::count_if(encoded.begin(), encoded.end(), [](auto const &f) { return f.keyframe; }),
                (int)packets.size(), (int)assembled.size());
    CHECK_TRUE(assembled.size() == encoded.size());
    CHECK_TRUE(!assembled.empty() && assembled[0].keyframe);
    const auto counts = decode(assembled);
    std::printf("full stream: %d decoded, %d errors, %d of a wrong size\n", counts.decoded, counts.errors, counts.wrongSize);
    CHECK_TRUE(counts.decoded == (int)encoded.size());
    CHECK_TRUE(counts.errors == 0 && counts.wrongSize == 0);
}

// A receiver that starts mid-stream (a joiner, or the first packets lost): nothing before the next keyframe, then every
// frame from it on.
void TestJoiningMidStreamStartsAtTheNextKeyframe(std::vector<EncodedFrame> const &encoded) {
    std::vector<int> frameOfPacket;
    const auto packets = packetize(encoded, &frameOfPacket);
    size_t firstPacket = 0;
    while (firstPacket < packets.size() && frameOfPacket[firstPacket] < 5) {
        firstPacket++;
    }
    const auto assembled = receive(packets, firstPacket);
    const int expected = kFrames - kKeyframeInterval;
    std::printf("joining at frame 5: %d assembled (expected %d, from the keyframe at %d)\n", (int)assembled.size(), expected, kKeyframeInterval);
    CHECK_TRUE((int)assembled.size() == expected);
    CHECK_TRUE(!assembled.empty() && assembled[0].keyframe && assembled[0].rtpTimestamp == kKeyframeInterval * 3000);
    const auto counts = decode(assembled);
    CHECK_TRUE(counts.decoded == expected && counts.errors == 0);
}

// Telegram's M123 builds (Intel's H.265 RTP code) drop an aggregation packet (they read its payload header as one
// byte), and with it every keyframe's VPS/SPS/PPS: no H.265 video at all from an M159 sender. M159 must send each NAL
// unit in a packet of its own (or fragmented), as M123 senders do.
void TestNoAggregationPackets(std::vector<EncodedFrame> const &encoded) {
    std::vector<int> frameOfPacket;
    const auto packets = packetize(encoded, &frameOfPacket);
    int aggregationPackets = 0;
    for (const auto &bytes : packets) {
        webrtc::RtpPacketReceived rtp;
        if (!rtp.Parse(bytes) || rtp.payload_size() < 2) {
            continue;
        }
        const uint8_t type = (rtp.payload()[0] >> 1) & 0x3f;
        if (type == 48) {
            aggregationPackets++;
        }
    }
    std::printf("aggregation packets: %d of %d\n", aggregationPackets, (int)packets.size());
    CHECK_TRUE(aggregationPackets == 0);
}

// Control: keyframes without their VPS/SPS/PPS (an encoder that sent them only once) never start a stream on M159.
// It shows the checks above see what the packet buffer requires.
void TestKeyframesWithoutParameterSetsNeverStart(std::vector<EncodedFrame> encoded) {
    for (auto &frame : encoded) {
        if (!frame.keyframe) {
            continue;
        }
        std::vector<uint8_t> stripped;
        const auto indices = webrtc::H265::FindNaluIndices(std::span<const uint8_t>(frame.annexb.data(), frame.annexb.size()));
        for (const auto &index : indices) {
            const uint8_t type = (frame.annexb[index.payload_start_offset] >> 1) & 0x3f;
            if (type == 32 || type == 33 || type == 34) {
                continue;
            }
            stripped.insert(stripped.end(), frame.annexb.begin() + index.start_offset,
                            frame.annexb.begin() + index.payload_start_offset + index.payload_size);
        }
        frame.annexb = std::move(stripped);
    }
    std::vector<int> frameOfPacket;
    const auto assembled = receive(packetize(encoded, &frameOfPacket), 0);
    std::printf("keyframes without parameter sets: %d assembled\n", (int)assembled.size());
    CHECK_TRUE(assembled.empty());
}

}  // namespace

int main() {
    const auto encoded = encode();
    CHECK_TRUE((int)encoded.size() == kFrames);
    if ((int)encoded.size() == kFrames) {
        CHECK_TRUE(encoded[0].keyframe && encoded[kKeyframeInterval].keyframe);
        TestEveryFrameArrivesAndDecodes(encoded);
        TestJoiningMidStreamStartsAtTheNextKeyframe(encoded);
        TestKeyframesWithoutParameterSetsNeverStart(encoded);
        TestNoAggregationPackets(encoded);
    }
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
