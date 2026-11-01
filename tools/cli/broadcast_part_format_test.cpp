// The emulated broadcast server's parts, read by the decoders today's engine plays broadcast with: the old
// decoders are the format's specification.

#include "broadcast_signals.h"

#include "group/AudioStreamingPart.h"
#include "group/AudioStreamingPartPersistentDecoder.h"
#include "group/VideoStreamingPart.h"

#include <opus/opus.h>

#include "submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/go_sfu.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
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

constexpr int kSampleRate = 48000;
constexpr int kFrameSamples = 960;
constexpr int kSlots = 50;

// One second of a tone, Opus-encoded in 20 ms packets (48 kHz mono, 32 kbit/s), as a speaker's engine sends it.
std::vector<std::vector<uint8_t>> encodeTone(double frequency) {
    int error = 0;
    OpusEncoder *encoder = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &error);
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(32000));
    std::vector<std::vector<uint8_t>> packets;
    double phase = 0.0;
    std::vector<int16_t> frame(kFrameSamples);
    for (int s = 0; s < kSlots; s++) {
        for (int i = 0; i < kFrameSamples; i++) {
            phase += 2.0 * M_PI * frequency / kSampleRate;
            frame[i] = (int16_t)std::lround(broadcast::kToneAmplitude * std::sin(phase));
        }
        unsigned char buffer[4000];
        int length = opus_encode(encoder, frame.data(), kFrameSamples, buffer, sizeof(buffer));
        CHECK_TRUE(length > 0);
        packets.emplace_back(buffer, buffer + std::max(length, 0));
    }
    opus_encoder_destroy(encoder);
    return packets;
}

std::vector<uint8_t> buildPart(std::vector<uint32_t> ssrcs, std::string const &endpointsJson,
                               std::vector<std::vector<std::vector<uint8_t>>> const &slots /* [slot][channel] */) {
    std::vector<uint8_t> flat;
    std::vector<int> offsets;
    for (auto const &slot : slots) {
        for (auto const &packet : slot) {
            offsets.push_back((int)flat.size());
            flat.insert(flat.end(), packet.begin(), packet.end());
        }
    }
    offsets.push_back((int)flat.size());
    int outLength = 0;
    char *result = GoSfu_TestBuildOpusPart(ssrcs.data(), (int)ssrcs.size(), const_cast<char *>(endpointsJson.c_str()),
                                           flat.data(), offsets.data(), &outLength);
    CHECK_TRUE(result != nullptr);
    CHECK_TRUE(outLength > 0);
    std::vector<uint8_t> bytes;
    if (result) {
        bytes.assign((uint8_t *)result, (uint8_t *)result + outLength);
        GoSfu_Free(result);
    }
    return bytes;
}

void testOldDecoderReadsEmulatedAudio() {
    std::vector<double> frequencies = {broadcast::kMarkerFrequencies[0], broadcast::kSpeakerFrequencies[0], broadcast::kSpeakerFrequencies[1]};
    std::vector<std::vector<std::vector<uint8_t>>> channels;
    for (double frequency : frequencies) {
        channels.push_back(encodeTone(frequency));
    }
    std::vector<std::vector<std::vector<uint8_t>>> slots(kSlots);
    for (int s = 0; s < kSlots; s++) {
        for (int c = 0; c < 3; c++) {
            bool empty = c == 2 && s >= 10 && s < 15;
            slots[s].push_back(empty ? std::vector<uint8_t>() : channels[c][s]);
        }
    }
    std::vector<uint8_t> bytes = buildPart({1001, 1002, 1003}, "{\"0\":\"ep-a\",\"2\":\"ep-c\"}", slots);

    tgcalls::AudioStreamingPart part(std::move(bytes), "ogg", false);
    CHECK_TRUE(part.getRemainingMilliseconds() == 1000);
    std::map<std::string, int32_t> expectedMapping = {{"ep-a", 0}, {"ep-c", 2}};
    CHECK_TRUE(part.getEndpointMapping() == expectedMapping);

    tgcalls::AudioStreamingPartPersistentDecoder decoder;
    broadcast::ToneDetector detector(frequencies);
    int heard[3] = {0, 0, 0};
    int nonEmpty[3] = {0, 0, 0};
    int heardWhileEmpty = 0;
    for (int block = 0; block < 100; block++) {
        auto blockChannels = part.get10msPerChannel(decoder);
        CHECK_TRUE(blockChannels.size() == 3);
        if (blockChannels.size() != 3) {
            break;
        }
        for (int c = 0; c < 3; c++) {
            CHECK_TRUE(blockChannels[c].ssrc == (uint32_t)(1001 + c));
            CHECK_TRUE(blockChannels[c].numSamples == 480);
            CHECK_TRUE(blockChannels[c].pcmData.size() >= 480);
            if (blockChannels[c].pcmData.size() < 480) {
                continue;
            }
            bool present = detector.detect(blockChannels[c].pcmData.data(), 480)[c];
            bool empty = c == 2 && block >= 20 && block < 30;
            if (empty) {
                // The first empty block carries the decoder's transition out of the last real packet.
                if (present && block > 20) {
                    std::printf("  channel 2 heard in empty block %d\n", block);
                    heardWhileEmpty++;
                }
            } else if (block >= 2) {
                nonEmpty[c]++;
                heard[c] += present ? 1 : 0;
            }
        }
    }
    for (int c = 0; c < 3; c++) {
        CHECK_TRUE(heard[c] * 100 >= nonEmpty[c] * 90);
        if (heard[c] * 100 < nonEmpty[c] * 90) {
            std::printf("  channel %d heard in %d of %d blocks\n", c, heard[c], nonEmpty[c]);
        }
    }
    CHECK_TRUE(heardWhileEmpty == 0);
    if (heardWhileEmpty != 0) {
        std::printf("  channel 2 heard in %d of its empty blocks 21-29\n", heardWhileEmpty);
    }
}

// The live path: speakers' 60 ms Opus packets (what GroupInstanceCustomImpl sends), as RTP, through the server's
// decode-and-re-encode into one part.
void testOldDecoderReadsTranscodedAudio() {
    std::vector<double> frequencies = {broadcast::kMarkerFrequencies[0], broadcast::kSpeakerFrequencies[0], broadcast::kSpeakerFrequencies[1]};
    const int frame60 = 2880;
    const int64_t anchorMs = 1'700'000'000'000; // a multiple of 20 ms
    const int64_t partMs = anchorMs + 1000;
    std::vector<uint8_t> flat;
    std::vector<int> offsets;
    std::vector<long long> arrivals;
    std::vector<int> senders;
    std::vector<uint32_t> ssrcs;
    for (int c = 0; c < 3; c++) {
        int error = 0;
        OpusEncoder *encoder = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &error);
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(32000));
        double phase = 0.0;
        std::vector<int16_t> pcm(frame60);
        for (int i = 0; i < 40; i++) { // 2.4 s of 60 ms packets
            for (int j = 0; j < frame60; j++) {
                phase += 2.0 * M_PI * frequencies[c] / kSampleRate;
                pcm[j] = (int16_t)std::lround(broadcast::kToneAmplitude * std::sin(phase));
            }
            unsigned char payload[4000];
            int length = opus_encode(encoder, pcm.data(), frame60, payload, sizeof(payload));
            CHECK_TRUE(length > 0);
            uint8_t header[12] = {0x80, 111, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            uint32_t timestamp = (uint32_t)(i * frame60);
            uint32_t ssrc = 1001 + c;
            for (int k = 0; k < 4; k++) {
                header[4 + k] = (uint8_t)(timestamp >> (24 - 8 * k));
                header[8 + k] = (uint8_t)(ssrc >> (24 - 8 * k));
            }
            offsets.push_back((int)flat.size());
            flat.insert(flat.end(), header, header + 12);
            flat.insert(flat.end(), payload, payload + std::max(length, 0));
            arrivals.push_back(anchorMs + i * 60);
            senders.push_back(c);
            ssrcs.push_back(ssrc);
        }
        opus_encoder_destroy(encoder);
    }
    offsets.push_back((int)flat.size());
    int outLength = 0;
    std::string endpoints = "{\"1\":\"ep-b\"}";
    char *result = GoSfu_TestBroadcastAudioPart(flat.data(), offsets.data(), arrivals.data(), senders.data(), ssrcs.data(),
                                                (int)arrivals.size(), partMs, const_cast<char *>(endpoints.c_str()), &outLength);
    CHECK_TRUE(result != nullptr && outLength > 0);
    if (!result) {
        return;
    }
    std::vector<uint8_t> bytes((uint8_t *)result, (uint8_t *)result + outLength);
    GoSfu_Free(result);

    tgcalls::AudioStreamingPart part(std::move(bytes), "ogg", false);
    CHECK_TRUE(part.getRemainingMilliseconds() == 1000);
    std::map<std::string, int32_t> expectedMapping = {{"ep-b", 1}};
    CHECK_TRUE(part.getEndpointMapping() == expectedMapping);
    tgcalls::AudioStreamingPartPersistentDecoder decoder;
    broadcast::ToneDetector detector(frequencies);
    int heard[3] = {0, 0, 0};
    int blocks = 0;
    for (int block = 0; block < 100; block++) {
        auto blockChannels = part.get10msPerChannel(decoder);
        CHECK_TRUE(blockChannels.size() == 3);
        if (blockChannels.size() != 3) {
            break;
        }
        if (block < 2) {
            continue;
        }
        blocks++;
        for (int c = 0; c < 3; c++) {
            CHECK_TRUE(blockChannels[c].ssrc == (uint32_t)(1001 + c));
            if (blockChannels[c].pcmData.size() >= 480 && detector.detect(blockChannels[c].pcmData.data(), 480)[c]) {
                heard[c]++;
            }
        }
    }
    for (int c = 0; c < 3; c++) {
        CHECK_TRUE(heard[c] * 100 >= blocks * 90);
        if (heard[c] * 100 < blocks * 90) {
            std::printf("  transcoded channel %d heard in %d of %d blocks\n", c, heard[c], blocks);
        }
    }
}

const char *kFixtureDir = "submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/testdata/broadcast";

std::vector<uint8_t> fixturePart(int kind, std::string const &endpoint, int quality, int64_t tsMs) {
    int outLength = 0;
    char *result = GoSfu_TestFixturePart(const_cast<char *>(kFixtureDir), kind, const_cast<char *>(endpoint.c_str()), quality, tsMs, &outLength);
    CHECK_TRUE(result != nullptr && outLength > 0);
    std::vector<uint8_t> bytes;
    if (result) {
        bytes.assign((uint8_t *)result, (uint8_t *)result + outLength);
        GoSfu_Free(result);
    }
    return bytes;
}

// The fixture's burned-in frame counter: bit k is the 16x16 block at x = 16k in the top rows (luma 235 = 1).
int readFrameCounter(webrtc::VideoFrame const &frame) {
    auto i420 = frame.video_frame_buffer()->ToI420();
    int value = 0;
    for (int bit = 0; bit < 16; bit++) {
        if (i420->DataY()[8 * i420->StrideY() + 16 * bit + 8] > 128) {
            value |= 1 << bit;
        }
    }
    return value;
}

// Reads one video part's 30 frames as the engine does; returns how many matched the expectations.
int checkVideoFrames(tgcalls::VideoStreamingPart &part, int expectedWidth, int firstCounter) {
    tgcalls::VideoStreamingSharedState state;
    double previousPts = -1.0;
    int good = 0;
    for (int i = 0; i < 30; i++) {
        auto frame = part.getFrameAtRelativeTimestamp(&state, (i + 0.5) / 30.0);
        if (!frame) {
            std::printf("  no frame at %d\n", i);
            continue;
        }
        bool ok = frame->pts > previousPts && frame->frame.width() == expectedWidth && readFrameCounter(frame->frame) == firstCounter + i;
        if (!ok) {
            std::printf("  frame %d: pts %.3f width %d counter %d\n", i, frame->pts, frame->frame.width(), readFrameCounter(frame->frame));
        }
        previousPts = frame->pts;
        good += ok ? 1 : 0;
    }
    return good;
}

void testOldDecoderReadsFixtureVideo() {
    const int widths[3] = {320, 640, 1280};
    for (int quality = 0; quality < 3; quality++) {
        tgcalls::VideoStreamingPart part(fixturePart(1, "ep-b", quality, 3000), tgcalls::VideoStreamingPart::ContentType::Video);
        auto endpoint = part.getActiveEndpointId();
        CHECK_TRUE(endpoint.has_value() && endpoint.value() == "ep-b");
        CHECK_TRUE(checkVideoFrames(part, widths[quality], 90) == 30);
    }
}

void testOldDecoderReadsUnified() {
    // The engine parses a unified part twice, as video and as audio.
    std::vector<uint8_t> bytes = fixturePart(2, "", 2, 3000);
    std::vector<uint8_t> copy = bytes;
    tgcalls::VideoStreamingPart video(std::move(bytes), tgcalls::VideoStreamingPart::ContentType::Video);
    auto endpoint = video.getActiveEndpointId();
    // The engine shows unified frames on its "unified" sink whatever the part names (StreamingMediaContext.cpp,
    // the unified render loop); the server names "".
    CHECK_TRUE(endpoint.has_value() && endpoint.value() == "");
    CHECK_TRUE(checkVideoFrames(video, 1280, 90) == 30);

    tgcalls::VideoStreamingPart audio(std::move(copy), tgcalls::VideoStreamingPart::ContentType::Audio);
    int remaining = audio.getAudioRemainingMilliseconds();
    CHECK_TRUE(remaining >= 980 && remaining <= 1010);
    tgcalls::AudioStreamingPartPersistentDecoder decoder;
    broadcast::ToneDetector detector({broadcast::kUnifiedFrequency});
    int blocks = 0;
    int heard = 0;
    for (int block = 0; block < 98; block++) {
        auto channels = audio.getAudio10msPerChannel(decoder);
        if (channels.empty()) {
            break;
        }
        CHECK_TRUE(channels[0].ssrc == 1);
        if (block < 3 || channels[0].pcmData.size() < 480) {
            continue;
        }
        blocks++;
        heard += detector.detect(channels[0].pcmData.data(), 480)[0] ? 1 : 0;
    }
    CHECK_TRUE(blocks > 80 && heard * 100 >= blocks * 90);
    if (!(blocks > 80 && heard * 100 >= blocks * 90)) {
        std::printf("  unified tone heard in %d of %d blocks\n", heard, blocks);
    }
}

} // namespace

int main() {
    testOldDecoderReadsEmulatedAudio();
    testOldDecoderReadsTranscodedAudio();
    testOldDecoderReadsFixtureVideo();
    testOldDecoderReadsUnified();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
