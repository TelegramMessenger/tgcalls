#ifndef BROADCAST_AUDIO_SPSC_QUEUE_H
#define BROADCAST_AUDIO_SPSC_QUEUE_H

#include <array>
#include <atomic>
#include <cstddef>

namespace broadcast_audio {

// A lock-free ring for exactly one producer thread and one consumer thread, of fixed capacity: neither side allocates
// or blocks, so the audio device's thread can be either end.
template <typename T, size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

public:
    // Producer. False when full.
    bool push(T const &value) {
        const size_t tail = _tail.load(std::memory_order_relaxed);
        const size_t head = _head.load(std::memory_order_acquire);
        if (tail - head >= Capacity) {
            return false;
        }
        _items[tail & (Capacity - 1)] = value;
        _tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer. False when empty.
    bool pop(T &value) {
        const size_t head = _head.load(std::memory_order_relaxed);
        const size_t tail = _tail.load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        value = _items[head & (Capacity - 1)];
        _head.store(head + 1, std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> _items{};
    std::atomic<size_t> _head{0};
    std::atomic<size_t> _tail{0};
};

} // namespace broadcast_audio

#endif
