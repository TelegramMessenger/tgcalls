#ifndef TGCALLS_MACOS_CALL_AUDIO_TAP_H
#define TGCALLS_MACOS_CALL_AUDIO_TAP_H

#import <Foundation/Foundation.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

@class OngoingCallIncomingAudio;

namespace tgcalls_macos {

class CallAudioTapRing;

class CallAudioTapSlot {
public:
    CallAudioTapSlot();
    ~CallAudioTapSlot();

    void push(uint32_t ssrc, const int16_t *interleaved, size_t frames, size_t channels, uint32_t sampleRate);

    void setEnabled(bool enabled);
    NSArray<OngoingCallIncomingAudio *> * _Nonnull drain();
    uint64_t dropped() const;

private:
    std::atomic<bool> _enabled{false};
    std::atomic<CallAudioTapRing *> _ring{nullptr};
    std::atomic_flag _producing = ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> _dropped{0};
};

}

@interface OngoingCallIncomingAudioPump : NSObject

- (instancetype _Nonnull)initWithSlot:(std::shared_ptr<tgcalls_macos::CallAudioTapSlot>)slot;
- (void)setHandler:(void (^ _Nullable)(NSArray<OngoingCallIncomingAudio *> * _Nonnull))handler;

@end

#endif
