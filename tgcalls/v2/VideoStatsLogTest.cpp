// The periodic "video stats" line of an InstanceV2ReferenceImpl call: what GetStats reports about the video streams,
// turned into per-interval rates.

#include "v2/VideoStatsLog.h"

#include "api/stats/rtc_stats_report.h"
#include "api/stats/rtcstats_objects.h"
#include "api/units/timestamp.h"

#include <cstdio>
#include <memory>
#include <string>

static int failures = 0;

#define CHECK_TRUE(cond)                                                                   \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "%s:%d: CHECK_TRUE(%s) failed\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                    \
        }                                                                                  \
    } while (0)

#define CHECK_LINE(actual, expected)                                                                         \
    do {                                                                                                     \
        const std::optional<std::string> line_ = (actual);                                                   \
        if (!line_ || *line_ != (expected)) {                                                                \
            std::fprintf(stderr, "%s:%d: got\n  %s\nexpected\n  %s\n", __FILE__, __LINE__,                   \
                         line_ ? line_->c_str() : "(nothing)", std::string(expected).c_str());               \
            failures++;                                                                                      \
        }                                                                                                    \
    } while (0)

namespace {

const webrtc::Timestamp kTime = webrtc::Timestamp::Millis(1);

struct Send {
    uint32_t frames = 0;
    double encodeSeconds = 0.0;
    uint64_t bytes = 0;
    uint64_t qpSum = 0;
    uint32_t keyFrames = 0;
};

struct Receive {
    uint32_t frames = 0;
    double decodeSeconds = 0.0;
    uint64_t bytes = 0;
    uint64_t qpSum = 0;
    uint32_t keyFrames = 0;
    uint32_t dropped = 0;
    uint32_t freezes = 0;
};

void AddCodec(webrtc::RTCStatsReport &report, const std::string &id, const std::string &mimeType) {
    auto codec = std::make_unique<webrtc::RTCCodecStats>(id, kTime);
    codec->mime_type = mimeType;
    report.AddStats(std::move(codec));
}

void AddSend(webrtc::RTCStatsReport &report, const std::string &id, const std::string &codecId, const Send &send) {
    auto stats = std::make_unique<webrtc::RTCOutboundRtpStreamStats>(id, kTime);
    stats->kind = "video";
    stats->codec_id = codecId;
    stats->encoder_implementation = "SvtAv1";
    stats->frame_width = 1280;
    stats->frame_height = 720;
    stats->frames_encoded = send.frames;
    stats->total_encode_time = send.encodeSeconds;
    stats->bytes_sent = send.bytes;
    stats->qp_sum = send.qpSum;
    stats->key_frames_encoded = send.keyFrames;
    stats->target_bitrate = 1500000.0;
    stats->quality_limitation_reason = "none";
    report.AddStats(std::move(stats));
}

void AddReceive(webrtc::RTCStatsReport &report, const std::string &id, const std::string &codecId, const Receive &receive) {
    auto stats = std::make_unique<webrtc::RTCInboundRtpStreamStats>(id, kTime);
    stats->kind = "video";
    stats->codec_id = codecId;
    stats->decoder_implementation = "dav1d";
    stats->frame_width = 640;
    stats->frame_height = 480;
    stats->frames_decoded = receive.frames;
    stats->total_decode_time = receive.decodeSeconds;
    stats->bytes_received = receive.bytes;
    if (receive.qpSum != 0) {
        stats->qp_sum = receive.qpSum;
    }
    stats->key_frames_decoded = receive.keyFrames;
    stats->frames_dropped = receive.dropped;
    stats->freeze_count = receive.freezes;
    report.AddStats(std::move(stats));
}

void AddAudioSend(webrtc::RTCStatsReport &report) {
    auto stats = std::make_unique<webrtc::RTCOutboundRtpStreamStats>("OTaudio", kTime);
    stats->kind = "audio";
    stats->bytes_sent = 4000;
    report.AddStats(std::move(stats));
}

rtc::scoped_refptr<webrtc::RTCStatsReport> Report() {
    auto report = webrtc::RTCStatsReport::Create(kTime);
    AddCodec(*report, "CAV1", "video/AV1");
    AddCodec(*report, "CH264", "video/H264");
    return report;
}

void TestSendAndReceive() {
    auto first = Report();
    AddSend(*first, "OT1", "CAV1", Send{0, 0.0, 0, 0, 1});
    AddReceive(*first, "IT1", "CAV1", Receive{0, 0.0, 0, 0, 1, 0, 0});
    // 5 s later: 150 frames in 1.2 s of encoding at qp 34, 1450 kbit/s; 149 frames in 0.1788 s of decoding at qp 31,
    // 1390 kbit/s, 2 dropped; 2.15 s of process CPU (43% of one core).
    auto second = Report();
    AddSend(*second, "OT1", "CAV1", Send{150, 1.2, 906250, 5100, 1});
    AddReceive(*second, "IT1", "CAV1", Receive{149, 0.1788, 868750, 4619, 1, 2, 0});

    const auto previous = tgcalls::MakeVideoStatsSample(*first, 10000, 10.0);
    const auto current = tgcalls::MakeVideoStatsSample(*second, 15000, 12.15);
    CHECK_LINE(tgcalls::FormatVideoStatsInterval(previous, current),
               "send AV1 (SvtAv1) 1280x720, 30.0 fps, encode 8.0 ms/frame, 1450 kbps (target 1500), qp 34, "
               "keyframes 0, quality limit none; receive AV1 (dav1d) 640x480, 29.8 fps, decode 1.2 ms/frame, "
               "1390 kbps, qp 31, keyframes 0, dropped 2, freezes 0; process CPU 43%");
}

void TestAudioOnlyLogsNothing() {
    auto first = Report();
    AddAudioSend(*first);
    auto second = Report();
    AddAudioSend(*second);
    const auto previous = tgcalls::MakeVideoStatsSample(*first, 0, 1.0);
    const auto current = tgcalls::MakeVideoStatsSample(*second, 5000, 2.0);
    CHECK_TRUE(current.sending.empty());
    CHECK_TRUE(!tgcalls::FormatVideoStatsInterval(previous, current).has_value());
}

void TestReportsTheStreamThatAdvanced() {
    // A camera turned off and on again leaves the old stream's counters behind; the new stream, absent from the
    // previous sample, counts from zero.
    auto first = Report();
    AddSend(*first, "OTold", "CH264", Send{900, 3.0, 1000000, 9000, 2});
    auto second = Report();
    AddSend(*second, "OTold", "CH264", Send{900, 3.0, 1000000, 9000, 2});
    AddSend(*second, "OTnew", "CAV1", Send{50, 0.5, 312500, 1500, 1});
    const auto previous = tgcalls::MakeVideoStatsSample(*first, 0, -1.0);
    const auto current = tgcalls::MakeVideoStatsSample(*second, 5000, -1.0);
    CHECK_LINE(tgcalls::FormatVideoStatsInterval(previous, current),
               "send AV1 (SvtAv1) 1280x720, 10.0 fps, encode 10.0 ms/frame, 500 kbps (target 1500), qp 30, "
               "keyframes 1, quality limit none");
}

void TestStreamWithoutFrames() {
    auto first = Report();
    AddReceive(*first, "IT1", "CAV1", Receive{300, 0.3, 500000, 9000, 1, 0, 0});
    auto second = Report();
    AddReceive(*second, "IT1", "CAV1", Receive{300, 0.3, 500000, 9000, 1, 0, 1});
    const auto previous = tgcalls::MakeVideoStatsSample(*first, 0, 4.0);
    const auto current = tgcalls::MakeVideoStatsSample(*second, 5000, 4.5);
    CHECK_LINE(tgcalls::FormatVideoStatsInterval(previous, current),
               "receive AV1 (dav1d) no frames, dropped 0, freezes 1; process CPU 10%");
}

void TestReceiveWithoutQp() {
    // dav1d reports no QP: the line leaves it out rather than reading 0.
    auto first = Report();
    AddReceive(*first, "IT1", "CAV1", Receive{0, 0.0, 0, 0, 1, 0, 0});
    auto second = Report();
    AddReceive(*second, "IT1", "CAV1", Receive{100, 0.1, 625000, 0, 1, 0, 0});
    const auto previous = tgcalls::MakeVideoStatsSample(*first, 0, -1.0);
    const auto current = tgcalls::MakeVideoStatsSample(*second, 5000, -1.0);
    CHECK_LINE(tgcalls::FormatVideoStatsInterval(previous, current),
               "receive AV1 (dav1d) 640x480, 20.0 fps, decode 1.0 ms/frame, 1000 kbps, keyframes 0, dropped 0, freezes 0");
}

void TestProcessCpuSeconds() {
    const double first = tgcalls::ProcessCpuSeconds();
    volatile double sink = 0.0;
    for (int i = 0; i < 20000000; i++) {
        sink += double(i) * 0.5;
    }
    const double second = tgcalls::ProcessCpuSeconds();
    CHECK_TRUE(first >= 0.0);
    CHECK_TRUE(second > first);
}

} // namespace

int main() {
    TestSendAndReceive();
    TestAudioOnlyLogsNothing();
    TestReportsTheStreamThatAdvanced();
    TestStreamWithoutFrames();
    TestReceiveWithoutQp();
    TestProcessCpuSeconds();
    if (failures == 0) {
        std::printf("video_stats_log_test: all passed\n");
        return 0;
    }
    std::printf("video_stats_log_test: %d failure(s)\n", failures);
    return 1;
}
