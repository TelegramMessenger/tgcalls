#include "CallPlayoutRecorder.h"

#include <algorithm>
#include <chrono>

namespace tgcalls {

namespace {

void PutU16(uint8_t *at, uint16_t value) {
    at[0] = uint8_t(value);
    at[1] = uint8_t(value >> 8);
}

void PutU32(uint8_t *at, uint32_t value) {
    at[0] = uint8_t(value);
    at[1] = uint8_t(value >> 8);
    at[2] = uint8_t(value >> 16);
    at[3] = uint8_t(value >> 24);
}

} // namespace

std::unique_ptr<CallPlayoutRecorder> CallPlayoutRecorder::Create(const std::string &path, int sampleRate,
                                                                 int channels, size_t capacitySamples,
                                                                 bool backgroundWriter) {
    if (sampleRate <= 0 || channels <= 0 || capacitySamples == 0) {
        return nullptr;
    }
    FILE *file = std::fopen(path.c_str(), "wb");
    if (!file) {
        return nullptr;
    }
    std::unique_ptr<CallPlayoutRecorder> recorder(new CallPlayoutRecorder(file, sampleRate, channels, capacitySamples));
    recorder->WriteHeader();
    if (backgroundWriter) {
        recorder->_writer = std::thread([raw = recorder.get()] { raw->WriterLoop(); });
    }
    return recorder;
}

CallPlayoutRecorder::CallPlayoutRecorder(FILE *file, int sampleRate, int channels, size_t capacitySamples) :
_file(file),
_sampleRate(sampleRate),
_channels(channels),
_ring(capacitySamples) {
}

CallPlayoutRecorder::~CallPlayoutRecorder() {
    if (_writer.joinable()) {
        {
            std::lock_guard<std::mutex> lock(_stopMutex);
            _stop = true;
        }
        _stopCondition.notify_all();
        _writer.join();
    }
    Drain();
    std::fclose(_file);
}

void CallPlayoutRecorder::Push(const int16_t *samples, size_t count) {
    const uint64_t write = _writeIndex.load(std::memory_order_relaxed);
    const uint64_t read = _readIndex.load(std::memory_order_acquire);
    const size_t capacity = _ring.size();
    const size_t space = capacity - size_t(write - read);
    const size_t n = std::min(count, space);
    const size_t start = size_t(write % capacity);
    const size_t first = std::min(n, capacity - start);
    std::copy(samples, samples + first, _ring.begin() + start);
    std::copy(samples + first, samples + n, _ring.begin());
    _writeIndex.store(write + n, std::memory_order_release);
    if (n < count) {
        _dropped.fetch_add(count - n, std::memory_order_relaxed);
    }
}

size_t CallPlayoutRecorder::Drain() {
    std::lock_guard<std::mutex> lock(_drainMutex);
    const uint64_t read = _readIndex.load(std::memory_order_relaxed);
    const uint64_t write = _writeIndex.load(std::memory_order_acquire);
    const size_t capacity = _ring.size();
    const size_t n = size_t(write - read);
    if (n == 0) {
        return 0;
    }
    const size_t start = size_t(read % capacity);
    const size_t first = std::min(n, capacity - start);
    // The file is little-endian 16-bit PCM, as the samples are in memory on Apple's CPUs.
    std::fseek(_file, 0, SEEK_END);
    std::fwrite(_ring.data() + start, sizeof(int16_t), first, _file);
    std::fwrite(_ring.data(), sizeof(int16_t), n - first, _file);
    _readIndex.store(read + n, std::memory_order_release);
    _written += n;
    WriteHeader();
    return n;
}

void CallPlayoutRecorder::WriteHeader() {
    const uint32_t dataBytes = uint32_t(std::min<uint64_t>(_written * 2, 0xffffffffu - 36));
    uint8_t header[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
    PutU32(header + 4, 36 + dataBytes);
    PutU32(header + 16, 16);
    PutU16(header + 20, 1);
    PutU16(header + 22, uint16_t(_channels));
    PutU32(header + 24, uint32_t(_sampleRate));
    PutU32(header + 28, uint32_t(_sampleRate * _channels * 2));
    PutU16(header + 32, uint16_t(_channels * 2));
    PutU16(header + 34, 16);
    header[36] = 'd';
    header[37] = 'a';
    header[38] = 't';
    header[39] = 'a';
    PutU32(header + 40, dataBytes);
    std::fseek(_file, 0, SEEK_SET);
    std::fwrite(header, 1, sizeof(header), _file);
    std::fflush(_file);
}

void CallPlayoutRecorder::WriterLoop() {
    std::unique_lock<std::mutex> lock(_stopMutex);
    while (!_stop) {
        _stopCondition.wait_for(lock, std::chrono::milliseconds(200));
        lock.unlock();
        Drain();
        lock.lock();
    }
}

} // namespace tgcalls
