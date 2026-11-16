#include "v2/VideoStatsLog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "api/stats/rtc_stats_report.h"
#include "api/stats/rtcstats_objects.h"

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

namespace tgcalls {

namespace {

std::string CodecName(const webrtc::RTCStatsReport &report, const absl::optional<std::string> &codecId) {
    if (!codecId) {
        return "?";
    }
    const webrtc::RTCStats *stats = report.Get(*codecId);
    if (!stats || std::strcmp(stats->type(), webrtc::RTCCodecStats::kType) != 0) {
        return "?";
    }
    const auto &codec = stats->cast_to<webrtc::RTCCodecStats>();
    if (!codec.mime_type) {
        return "?";
    }
    const std::string &mimeType = *codec.mime_type;
    const size_t slash = mimeType.find('/');
    return slash == std::string::npos ? mimeType : mimeType.substr(slash + 1);
}

template <typename T>
T ValueOr(const absl::optional<T> &value, T fallback) {
    return value ? *value : fallback;
}

uint64_t Delta(uint64_t current, uint64_t previous) {
    return current >= previous ? current - previous : current;
}

struct StreamInterval {
    const VideoStreamCounters *current = nullptr;
    uint64_t frames = 0;
    double codecSeconds = 0.0;
    uint64_t bytes = 0;
    std::optional<uint64_t> qpSum;
    uint64_t keyFrames = 0;
    uint64_t framesDropped = 0;
    uint64_t freezes = 0;
};

// The stream that advanced most over the interval: a camera turned off and on again leaves a stream behind.
std::optional<StreamInterval> BusiestStream(const std::map<std::string, VideoStreamCounters> &previous,
                                            const std::map<std::string, VideoStreamCounters> &current) {
    std::optional<StreamInterval> busiest;
    static const VideoStreamCounters kZero;
    for (const auto &[id, counters] : current) {
        const auto found = previous.find(id);
        const VideoStreamCounters &before = found == previous.end() ? kZero : found->second;
        StreamInterval interval;
        interval.current = &counters;
        interval.frames = Delta(counters.frames, before.frames);
        interval.codecSeconds = std::max(0.0, counters.codecSeconds - before.codecSeconds);
        interval.bytes = Delta(counters.bytes, before.bytes);
        if (counters.qpSum) {
            interval.qpSum = Delta(*counters.qpSum, before.qpSum.value_or(0));
        }
        interval.keyFrames = Delta(counters.keyFrames, before.keyFrames);
        interval.framesDropped = Delta(counters.framesDropped, before.framesDropped);
        interval.freezes = Delta(counters.freezes, before.freezes);
        if (!busiest || interval.frames > busiest->frames ||
            (interval.frames == busiest->frames && interval.bytes > busiest->bytes)) {
            busiest = interval;
        }
    }
    return busiest;
}

std::string Format(const char *format, double a) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), format, a);
    return buffer;
}

std::string DescribeStream(const char *direction, const StreamInterval &interval, double seconds, bool sending) {
    const VideoStreamCounters &stream = *interval.current;
    std::string text = direction;
    text += " " + stream.codec;
    if (!stream.implementation.empty()) {
        text += " (" + stream.implementation + ")";
    }
    if (interval.frames == 0) {
        text += " no frames";
        if (!sending) {
            text += ", dropped " + std::to_string(interval.framesDropped) + ", freezes " + std::to_string(interval.freezes);
        }
        return text;
    }
    if (stream.width > 0 && stream.height > 0) {
        text += " " + std::to_string(stream.width) + "x" + std::to_string(stream.height) + ",";
    }
    text += Format(" %.1f fps", double(interval.frames) / seconds);
    text += Format(sending ? ", encode %.1f ms/frame" : ", decode %.1f ms/frame",
                   interval.codecSeconds * 1000.0 / double(interval.frames));
    text += ", " + std::to_string(std::lround(double(interval.bytes) * 8.0 / seconds / 1000.0)) + " kbps";
    if (sending && stream.targetBitrateBps > 0.0) {
        text += " (target " + std::to_string(std::lround(stream.targetBitrateBps / 1000.0)) + ")";
    }
    if (interval.qpSum) {
        text += ", qp " + std::to_string(std::lround(double(*interval.qpSum) / double(interval.frames)));
    }
    text += ", keyframes " + std::to_string(interval.keyFrames);
    if (sending) {
        if (!stream.qualityLimitationReason.empty()) {
            text += ", quality limit " + stream.qualityLimitationReason;
        }
    } else {
        text += ", dropped " + std::to_string(interval.framesDropped) + ", freezes " + std::to_string(interval.freezes);
    }
    return text;
}

} // namespace

VideoStatsSample MakeVideoStatsSample(const webrtc::RTCStatsReport &report, int64_t timestampMs,
                                      double processCpuSeconds) {
    VideoStatsSample sample;
    sample.timestampMs = timestampMs;
    sample.processCpuSeconds = processCpuSeconds;
    for (const auto *stats : report.GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>()) {
        if (ValueOr<std::string>(stats->kind, "") != "video") {
            continue;
        }
        VideoStreamCounters counters;
        counters.codec = CodecName(report, stats->codec_id);
        counters.implementation = ValueOr<std::string>(stats->encoder_implementation, "");
        counters.width = ValueOr<uint32_t>(stats->frame_width, 0);
        counters.height = ValueOr<uint32_t>(stats->frame_height, 0);
        counters.frames = ValueOr<uint32_t>(stats->frames_encoded, 0);
        counters.codecSeconds = ValueOr<double>(stats->total_encode_time, 0.0);
        counters.bytes = ValueOr<uint64_t>(stats->bytes_sent, 0);
        counters.qpSum = stats->qp_sum;
        counters.keyFrames = ValueOr<uint32_t>(stats->key_frames_encoded, 0);
        counters.targetBitrateBps = ValueOr<double>(stats->target_bitrate, 0.0);
        counters.qualityLimitationReason = ValueOr<std::string>(stats->quality_limitation_reason, "");
        sample.sending.emplace(stats->id(), std::move(counters));
    }
    for (const auto *stats : report.GetStatsOfType<webrtc::RTCInboundRtpStreamStats>()) {
        if (ValueOr<std::string>(stats->kind, "") != "video") {
            continue;
        }
        VideoStreamCounters counters;
        counters.codec = CodecName(report, stats->codec_id);
        counters.implementation = ValueOr<std::string>(stats->decoder_implementation, "");
        counters.width = ValueOr<uint32_t>(stats->frame_width, 0);
        counters.height = ValueOr<uint32_t>(stats->frame_height, 0);
        counters.frames = ValueOr<uint32_t>(stats->frames_decoded, 0);
        counters.codecSeconds = ValueOr<double>(stats->total_decode_time, 0.0);
        counters.bytes = ValueOr<uint64_t>(stats->bytes_received, 0);
        counters.qpSum = stats->qp_sum;
        counters.keyFrames = ValueOr<uint32_t>(stats->key_frames_decoded, 0);
        counters.framesDropped = ValueOr<uint32_t>(stats->frames_dropped, 0);
        counters.freezes = ValueOr<uint32_t>(stats->freeze_count, 0);
        sample.receiving.emplace(stats->id(), std::move(counters));
    }
    return sample;
}

std::optional<std::string> FormatVideoStatsInterval(const VideoStatsSample &previous, const VideoStatsSample &current) {
    const double seconds = double(current.timestampMs - previous.timestampMs) / 1000.0;
    if (seconds <= 0.0) {
        return std::nullopt;
    }
    const auto sent = BusiestStream(previous.sending, current.sending);
    const auto received = BusiestStream(previous.receiving, current.receiving);
    if (!sent && !received) {
        return std::nullopt;
    }
    std::string line;
    if (sent) {
        line += DescribeStream("send", *sent, seconds, true);
    }
    if (received) {
        if (!line.empty()) {
            line += "; ";
        }
        line += DescribeStream("receive", *received, seconds, false);
    }
    if (previous.processCpuSeconds >= 0.0 && current.processCpuSeconds >= previous.processCpuSeconds) {
        const double percent = (current.processCpuSeconds - previous.processCpuSeconds) / seconds * 100.0;
        line += "; process CPU " + std::to_string(std::lround(percent)) + "%";
    }
    return line;
}

double ProcessCpuSeconds() {
#if defined(_WIN32)
    return -1.0;
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1.0;
    }
    return double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
           double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000000.0;
#endif
}

} // namespace tgcalls
