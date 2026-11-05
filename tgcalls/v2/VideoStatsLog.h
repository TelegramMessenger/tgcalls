#ifndef TGCALLS_VIDEO_STATS_LOG_H
#define TGCALLS_VIDEO_STATS_LOG_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace webrtc {
class RTCStatsReport;
}

namespace tgcalls {

// Cumulative counters of one video RTP stream, as GetStats reports them.
struct VideoStreamCounters {
    std::string codec;             // the codec's MIME subtype: "AV1", "H265", ...
    std::string implementation;    // encoderImplementation / decoderImplementation
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t frames = 0;           // framesEncoded / framesDecoded
    double codecSeconds = 0.0;     // totalEncodeTime / totalDecodeTime
    uint64_t bytes = 0;
    std::optional<uint64_t> qpSum;  // absent when the codec reports no QP (dav1d)
    uint64_t keyFrames = 0;
    double targetBitrateBps = 0.0;        // sent streams only
    std::string qualityLimitationReason;  // sent streams only
    uint64_t framesDropped = 0;           // received streams only
    uint64_t freezes = 0;                 // received streams only
};

struct VideoStatsSample {
    int64_t timestampMs = 0;
    std::map<std::string, VideoStreamCounters> sending;    // by stats id
    std::map<std::string, VideoStreamCounters> receiving;  // by stats id
    double processCpuSeconds = -1.0;  // user + system; negative when unknown
};

VideoStatsSample MakeVideoStatsSample(const webrtc::RTCStatsReport &report, int64_t timestampMs,
                                      double processCpuSeconds);

// The interval between two samples as one log line: per direction the stream that advanced most, with its frame rate,
// encode or decode time per frame, bitrate and average QP, then the process's CPU use (100% is one core). Nothing when
// the current sample has no video stream. A stream missing from `previous` counts from zero.
std::optional<std::string> FormatVideoStatsInterval(const VideoStatsSample &previous, const VideoStatsSample &current);

// CPU time the process has used, user + system, in seconds; negative where unavailable.
double ProcessCpuSeconds();

} // namespace tgcalls

#endif
