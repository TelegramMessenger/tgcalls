#ifndef TGCALLS_CALL_PLAYOUT_RECORDER_H
#define TGCALLS_CALL_PLAYOUT_RECORDER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tgcalls {

// What a call plays, as a 16-bit PCM WAV file: debug builds launched with TG_CALL_PLAYOUT_WAV=1 record every playout
// of AudioDeviceIOS (OnGetPlayoutData, after the mix: every participant after NetEq, tones included) into the app's
// Documents (tgcalls_audio_device_ios.mm). Test: call_playout_recorder_test.
//
// The audio thread only copies into a ring (Push: no lock, no allocation, no I/O; what does not fit is dropped and
// counted); a writer drains it to the file and rewrites the header's sizes after every drain, so the file stays
// playable if the app is killed mid-call.
class CallPlayoutRecorder {
public:
    // Creates the file at `path` with the WAV header. `capacitySamples` is the ring's size in samples (all channels);
    // `backgroundWriter` starts a thread that drains every 200 ms. Null if the file cannot be created.
    static std::unique_ptr<CallPlayoutRecorder> Create(const std::string &path, int sampleRate, int channels,
                                                       size_t capacitySamples, bool backgroundWriter);

    // Stops the writer, drains what is left and closes the file.
    ~CallPlayoutRecorder();

    CallPlayoutRecorder(const CallPlayoutRecorder &) = delete;
    CallPlayoutRecorder &operator=(const CallPlayoutRecorder &) = delete;

    int sampleRate() const { return _sampleRate; }
    int channels() const { return _channels; }

    // Audio thread (one producer): interleaved samples.
    void Push(const int16_t *samples, size_t count);

    // Writer (one consumer): writes what the ring holds and updates the header. Returns the samples written.
    size_t Drain();

    uint64_t droppedSamples() const { return _dropped.load(std::memory_order_relaxed); }
    uint64_t writtenSamples() const {
        std::lock_guard<std::mutex> lock(_drainMutex);
        return _written;
    }

private:
    CallPlayoutRecorder(FILE *file, int sampleRate, int channels, size_t capacitySamples);
    void WriteHeader();
    void WriterLoop();

    FILE *_file = nullptr;
    const int _sampleRate;
    const int _channels;
    std::vector<int16_t> _ring;
    std::atomic<uint64_t> _writeIndex{0};
    std::atomic<uint64_t> _readIndex{0};
    std::atomic<uint64_t> _dropped{0};
    uint64_t _written = 0;

    mutable std::mutex _drainMutex;
    std::mutex _stopMutex;
    std::condition_variable _stopCondition;
    bool _stop = false;
    std::thread _writer;
};

} // namespace tgcalls

#endif
