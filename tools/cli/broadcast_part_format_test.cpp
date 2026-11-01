// The emulated broadcast server's parts, read by the decoders today's engine plays broadcast with: the old
// decoders are the format's specification.

#include "broadcast_signals.h"

#include "group/AudioStreamingPart.h"
#include "group/AudioStreamingPartPersistentDecoder.h"

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

} // namespace

int main() {
    testOldDecoderReadsEmulatedAudio();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
