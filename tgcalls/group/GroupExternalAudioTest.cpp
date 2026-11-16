#include "group/GroupExternalAudio.h"

#include <cstdint>
#include <cstdio>
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

// Little-endian s16 bytes of samples first, first + 1, ... (wrapping at 16 bits).
std::vector<uint8_t> rampBytes(int first, int count) {
    std::vector<uint8_t> bytes;
    bytes.reserve(count * 2);
    for (int i = 0; i < count; i++) {
        const uint16_t value = (uint16_t)(int16_t)(first + i);
        bytes.push_back(value & 0xff);
        bytes.push_back(value >> 8);
    }
    return bytes;
}

void testTenMillisecondsComeOutAsOneFrame() {
    std::vector<float> buffer;
    webrtc::Mutex mutex;
    tgcalls::appendGroupExternalAudioSamples(buffer, mutex, rampBytes(-240, 480));

    tgcalls::ExternalAudioRecorder recorder(&buffer, &mutex);
    const auto frame = recorder.Record();
    CHECK_TRUE(frame.num_samples == 480);
    CHECK_TRUE(frame.num_channels == 1);
    CHECK_TRUE(frame.samples_per_sec == 48000);
    CHECK_TRUE(frame.bytes_per_sample == 2);
    const int16_t *samples = (const int16_t *)frame.audio_samples;
    CHECK_TRUE(samples[0] == -240);
    CHECK_TRUE(samples[479] == 239);

    CHECK_TRUE(recorder.Record().num_samples == 0);
}

void testLessThanTenMillisecondsWaits() {
    std::vector<float> buffer;
    webrtc::Mutex mutex;
    tgcalls::appendGroupExternalAudioSamples(buffer, mutex, rampBytes(0, 300));

    tgcalls::ExternalAudioRecorder recorder(&buffer, &mutex);
    CHECK_TRUE(recorder.Record().num_samples == 0);

    tgcalls::appendGroupExternalAudioSamples(buffer, mutex, rampBytes(300, 300));
    const auto frame = recorder.Record();
    CHECK_TRUE(frame.num_samples == 480);
    CHECK_TRUE(((const int16_t *)frame.audio_samples)[0] == 0);
    CHECK_TRUE(buffer.size() == 120);
}

void testAtMostTwoSecondsAreKeptNewestFirst() {
    std::vector<float> buffer;
    webrtc::Mutex mutex;
    // 3 s: the oldest second is dropped.
    tgcalls::appendGroupExternalAudioSamples(buffer, mutex, rampBytes(0, 3 * 48000));
    CHECK_TRUE(buffer.size() == 2 * 48000);

    tgcalls::ExternalAudioRecorder recorder(&buffer, &mutex);
    const auto frame = recorder.Record();
    CHECK_TRUE(frame.num_samples == 480);
    CHECK_TRUE(((const int16_t *)frame.audio_samples)[0] == (int16_t)48000);
}

void testAnOddByteCountIsIgnored() {
    std::vector<float> buffer;
    webrtc::Mutex mutex;
    auto bytes = rampBytes(0, 480);
    bytes.push_back(0);
    tgcalls::appendGroupExternalAudioSamples(buffer, mutex, bytes);
    CHECK_TRUE(buffer.empty());
}

} // namespace

int main() {
    testTenMillisecondsComeOutAsOneFrame();
    testLessThanTenMillisecondsWaits();
    testAtMostTwoSecondsAreKeptNewestFirst();
    testAnOddByteCountIsIgnored();
    if (g_failures == 0) {
        std::printf("PASS group_external_audio_test\n");
        return 0;
    }
    std::printf("%d failure(s)\n", g_failures);
    return 1;
}
