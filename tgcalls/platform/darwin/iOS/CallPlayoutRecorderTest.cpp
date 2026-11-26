// Host test for CallPlayoutRecorder (the debug playout recording, TG_CALL_PLAYOUT_WAV=1):
//
//   ./build-input/bazel-9.2.0-darwin-arm64 test //submodules/TgVoipWebrtc:call_playout_recorder_test --test_output=errors

#include "CallPlayoutRecorder.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

std::string TempPath(const char *name) {
    const char *dir = std::getenv("TEST_TMPDIR");
    return std::string(dir ? dir : "/tmp") + "/" + name;
}

std::vector<uint8_t> ReadFile(const std::string &path) {
    std::vector<uint8_t> bytes;
    if (FILE *file = std::fopen(path.c_str(), "rb")) {
        uint8_t buffer[4096];
        size_t count = 0;
        while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            bytes.insert(bytes.end(), buffer, buffer + count);
        }
        std::fclose(file);
    }
    return bytes;
}

uint32_t U32(const std::vector<uint8_t> &b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

uint16_t U16(const std::vector<uint8_t> &b, size_t at) {
    return uint16_t(b[at] | b[at + 1] << 8);
}

std::vector<int16_t> Ramp(size_t count, int16_t start) {
    std::vector<int16_t> samples(count);
    for (size_t i = 0; i < count; i++) {
        samples[i] = int16_t(start + int16_t(i));
    }
    return samples;
}

// A 44-byte PCM WAV header with the format and sizes of what was written.
void CheckHeader(const std::vector<uint8_t> &b, int sampleRate, int channels, uint32_t dataBytes) {
    CHECK_TRUE(b.size() == 44 + dataBytes);
    if (b.size() < 44) {
        return;
    }
    CHECK_TRUE(std::string(b.begin(), b.begin() + 4) == "RIFF");
    CHECK_TRUE(U32(b, 4) == 36 + dataBytes);
    CHECK_TRUE(std::string(b.begin() + 8, b.begin() + 16) == "WAVEfmt ");
    CHECK_TRUE(U32(b, 16) == 16);
    CHECK_TRUE(U16(b, 20) == 1);
    CHECK_TRUE(U16(b, 22) == channels);
    CHECK_TRUE(U32(b, 24) == uint32_t(sampleRate));
    CHECK_TRUE(U32(b, 28) == uint32_t(sampleRate * channels * 2));
    CHECK_TRUE(U16(b, 32) == channels * 2);
    CHECK_TRUE(U16(b, 34) == 16);
    CHECK_TRUE(std::string(b.begin() + 36, b.begin() + 40) == "data");
    CHECK_TRUE(U32(b, 40) == dataBytes);
}

// What is pushed is what the file holds, in order, and the header is valid after every drain (a call that ends with
// the app killed still leaves a playable file).
void TestPushedSamplesAreWrittenInOrder() {
    const std::string path = TempPath("playout-order.wav");
    auto recorder = tgcalls::CallPlayoutRecorder::Create(path, 48000, 1, 4800, false);
    CHECK_TRUE(recorder != nullptr);
    if (!recorder) {
        return;
    }
    const auto first = Ramp(480, 0);
    const auto second = Ramp(960, 480);
    recorder->Push(first.data(), first.size());
    CHECK_TRUE(recorder->Drain() == 480);
    CheckHeader(ReadFile(path), 48000, 1, 960);
    recorder->Push(second.data(), second.size());
    CHECK_TRUE(recorder->Drain() == 960);
    recorder.reset();
    const auto bytes = ReadFile(path);
    CheckHeader(bytes, 48000, 1, 2880);
    bool inOrder = bytes.size() == 44 + 2880;
    for (size_t i = 0; inOrder && i < 1440; i++) {
        inOrder = int16_t(U16(bytes, 44 + i * 2)) == int16_t(i);
    }
    CHECK_TRUE(inOrder);
}

// The ring wraps around its end without losing or reordering samples.
void TestTheRingWraps() {
    const std::string path = TempPath("playout-wrap.wav");
    auto recorder = tgcalls::CallPlayoutRecorder::Create(path, 48000, 2, 1000, false);
    CHECK_TRUE(recorder != nullptr);
    if (!recorder) {
        return;
    }
    int16_t next = 0;
    for (int round = 0; round < 7; round++) {
        const auto samples = Ramp(700, next);
        next = int16_t(next + 700);
        recorder->Push(samples.data(), samples.size());
        CHECK_TRUE(recorder->Drain() == 700);
    }
    CHECK_TRUE(recorder->droppedSamples() == 0);
    recorder.reset();
    const auto bytes = ReadFile(path);
    CheckHeader(bytes, 48000, 2, 7 * 700 * 2);
    bool inOrder = bytes.size() == 44 + 7 * 700 * 2;
    for (size_t i = 0; inOrder && i < 7 * 700; i++) {
        inOrder = int16_t(U16(bytes, 44 + i * 2)) == int16_t(i);
    }
    CHECK_TRUE(inOrder);
}

// The audio thread never waits: what does not fit is dropped and counted.
void TestAFullRingDropsAndCounts() {
    const std::string path = TempPath("playout-full.wav");
    auto recorder = tgcalls::CallPlayoutRecorder::Create(path, 48000, 1, 1000, false);
    CHECK_TRUE(recorder != nullptr);
    if (!recorder) {
        return;
    }
    const auto samples = Ramp(600, 0);
    recorder->Push(samples.data(), samples.size());
    recorder->Push(samples.data(), samples.size());
    CHECK_TRUE(recorder->droppedSamples() == 200);
    CHECK_TRUE(recorder->Drain() == 1000);
}

// With its own writer the recorder writes everything by the time it is destroyed.
void TestTheBackgroundWriterWritesEverything() {
    const std::string path = TempPath("playout-writer.wav");
    auto recorder = tgcalls::CallPlayoutRecorder::Create(path, 16000, 1, 16000, true);
    CHECK_TRUE(recorder != nullptr);
    if (!recorder) {
        return;
    }
    const auto samples = Ramp(160, 0);
    for (int i = 0; i < 50; i++) {
        recorder->Push(samples.data(), samples.size());
    }
    recorder.reset();
    CheckHeader(ReadFile(path), 16000, 1, 50 * 160 * 2);
}

void TestAnUnwritablePathGivesNoRecorder() {
    CHECK_TRUE(tgcalls::CallPlayoutRecorder::Create("/nonexistent-dir/x.wav", 48000, 1, 1000, false) == nullptr);
}

}  // namespace

int main() {
    TestPushedSamplesAreWrittenInOrder();
    TestTheRingWraps();
    TestAFullRingDropsAndCounts();
    TestTheBackgroundWriterWritesEverything();
    TestAnUnwritablePathGivesNoRecorder();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
