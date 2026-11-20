#import "CallAudioTap.h"

#import <TgVoipWebrtc/OngoingCallThreadLocalContext.h>

#include "BroadcastSpscQueue.h"

#include <algorithm>
#include <cstring>
#include <vector>

@interface OngoingCallIncomingAudio ()

- (instancetype _Nonnull)initWithSsrc:(uint32_t)ssrc sampleRate:(int32_t)sampleRate samples:(NSData * _Nonnull)samples;

@end

@implementation OngoingCallIncomingAudio

- (instancetype _Nonnull)initWithSsrc:(uint32_t)ssrc sampleRate:(int32_t)sampleRate samples:(NSData * _Nonnull)samples {
    self = [super init];
    if (self != nil) {
        _ssrc = ssrc;
        _sampleRate = sampleRate;
        _samples = samples;
    }
    return self;
}

@end

namespace tgcalls_macos {

namespace {

constexpr size_t kChunkSamples = 480;
constexpr size_t kRingChunks = 512;

struct Chunk {
    uint32_t ssrc = 0;
    uint32_t sampleRate = 0;
    uint32_t count = 0;
    int16_t samples[kChunkSamples];
};

}

class CallAudioTapRing {
public:
    broadcast_audio::SpscQueue<Chunk, kRingChunks> queue;
};

CallAudioTapSlot::CallAudioTapSlot() {
}

CallAudioTapSlot::~CallAudioTapSlot() {
    delete _ring.load(std::memory_order_acquire);
}

void CallAudioTapSlot::push(uint32_t ssrc, const int16_t *interleaved, size_t frames, size_t channels, uint32_t sampleRate) {
    if (!_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    CallAudioTapRing *ring = _ring.load(std::memory_order_acquire);
    if (ring == nullptr || interleaved == nullptr || frames == 0 || channels == 0 || sampleRate == 0) {
        return;
    }
    const size_t total = frames * channels;
    size_t nonZero = 0;
    while (nonZero < total && interleaved[nonZero] == 0) {
        nonZero++;
    }
    if (nonZero == total) {
        return;
    }
    if (_producing.test_and_set(std::memory_order_acquire)) {
        _dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Chunk chunk;
    chunk.ssrc = ssrc;
    chunk.sampleRate = sampleRate;
    size_t offset = 0;
    while (offset < frames) {
        const size_t count = std::min(kChunkSamples, frames - offset);
        const int16_t *source = interleaved + offset * channels;
        if (channels == 1) {
            memcpy(chunk.samples, source, count * sizeof(int16_t));
        } else {
            for (size_t i = 0; i < count; i++) {
                int32_t sum = 0;
                for (size_t c = 0; c < channels; c++) {
                    sum += source[i * channels + c];
                }
                chunk.samples[i] = (int16_t)(sum / (int32_t)channels);
            }
        }
        chunk.count = (uint32_t)count;
        if (!ring->queue.push(chunk)) {
            _dropped.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        offset += count;
    }
    _producing.clear(std::memory_order_release);
}

void CallAudioTapSlot::setEnabled(bool enabled) {
    if (enabled && _ring.load(std::memory_order_acquire) == nullptr) {
        _ring.store(new CallAudioTapRing(), std::memory_order_release);
    }
    _enabled.store(enabled, std::memory_order_relaxed);
}

NSArray<OngoingCallIncomingAudio *> *CallAudioTapSlot::drain() {
    CallAudioTapRing *ring = _ring.load(std::memory_order_acquire);
    if (ring == nullptr) {
        return @[];
    }
    struct Pending {
        uint32_t ssrc;
        uint32_t sampleRate;
        NSMutableData *data;
    };
    std::vector<Pending> pending;
    Chunk chunk;
    while (ring->queue.pop(chunk)) {
        Pending *target = nullptr;
        for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
            if (it->ssrc == chunk.ssrc) {
                if (it->sampleRate == chunk.sampleRate) {
                    target = &*it;
                }
                break;
            }
        }
        if (target == nullptr) {
            pending.push_back({ chunk.ssrc, chunk.sampleRate, [[NSMutableData alloc] init] });
            target = &pending.back();
        }
        [target->data appendBytes:chunk.samples length:chunk.count * sizeof(int16_t)];
    }
    NSMutableArray<OngoingCallIncomingAudio *> *result = [[NSMutableArray alloc] initWithCapacity:pending.size()];
    for (auto &item : pending) {
        [result addObject:[[OngoingCallIncomingAudio alloc] initWithSsrc:item.ssrc sampleRate:(int32_t)item.sampleRate samples:item.data]];
    }
    return result;
}

uint64_t CallAudioTapSlot::dropped() const {
    return _dropped.load(std::memory_order_relaxed);
}

}

@interface OngoingCallIncomingAudioPump () {
    std::shared_ptr<tgcalls_macos::CallAudioTapSlot> _slot;
    dispatch_queue_t _queue;
    dispatch_source_t _timer;
    void (^_handler)(NSArray<OngoingCallIncomingAudio *> * _Nonnull);
}

@end

@implementation OngoingCallIncomingAudioPump

- (instancetype _Nonnull)initWithSlot:(std::shared_ptr<tgcalls_macos::CallAudioTapSlot>)slot {
    self = [super init];
    if (self != nil) {
        _slot = slot;
        _queue = dispatch_queue_create("org.telegram.CallIncomingAudio", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    }
    return self;
}

- (void)dealloc {
    _slot->setEnabled(false);
    if (_timer != nil) {
        dispatch_source_cancel(_timer);
    }
}

- (void)setHandler:(void (^ _Nullable)(NSArray<OngoingCallIncomingAudio *> * _Nonnull))handler {
    void (^copied)(NSArray<OngoingCallIncomingAudio *> * _Nonnull) = [handler copy];
    std::shared_ptr<tgcalls_macos::CallAudioTapSlot> slot = _slot;
    __weak OngoingCallIncomingAudioPump *weakSelf = self;
    dispatch_async(_queue, ^{
        __strong OngoingCallIncomingAudioPump *strongSelf = weakSelf;
        if (strongSelf == nil) {
            return;
        }
        strongSelf->_handler = copied;
        if (copied != nil) {
            slot->setEnabled(true);
            if (strongSelf->_timer == nil) {
                dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, strongSelf->_queue);
                dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 40 * NSEC_PER_MSEC), 40 * NSEC_PER_MSEC, 10 * NSEC_PER_MSEC);
                dispatch_source_set_event_handler(timer, ^{
                    __strong OngoingCallIncomingAudioPump *pump = weakSelf;
                    if (pump == nil || pump->_handler == nil) {
                        return;
                    }
                    NSArray<OngoingCallIncomingAudio *> *chunks = slot->drain();
                    if (chunks.count != 0) {
                        pump->_handler(chunks);
                    }
                });
                strongSelf->_timer = timer;
                dispatch_resume(timer);
            }
        } else {
            slot->setEnabled(false);
            if (strongSelf->_timer != nil) {
                dispatch_source_cancel(strongSelf->_timer);
                strongSelf->_timer = nil;
            }
            slot->drain();
        }
    });
}

@end
