#include "BroadcastAudioCoreBridge.h"

#include <cmath>
#include <vector>

struct BroadcastAudioCore {
    std::shared_ptr<broadcast_audio::Core> core;
};

std::shared_ptr<broadcast_audio::Core> BroadcastAudioCoreShared(BroadcastAudioCoreRef core) {
    return core->core;
}

BroadcastAudioCoreRef BroadcastAudioCoreCreate(void) {
    auto handle = new BroadcastAudioCore();
    handle->core = std::make_shared<broadcast_audio::Core>();
    return handle;
}

void BroadcastAudioCoreRelease(BroadcastAudioCoreRef core) {
    delete core;
}

double BroadcastAudioCoreNow(BroadcastAudioCoreRef core) {
    return (double)core->core->nowUs() / 1e6;
}

bool BroadcastAudioCoreEnqueueSegment(BroadcastAudioCoreRef core, int64_t timestampMs, const uint32_t *ssrcs, const int16_t *const *samples, const size_t *sampleCounts, const size_t *startSamples, size_t channelCount) {
    std::vector<broadcast_audio::ChannelSamples> channels(channelCount);
    for (size_t i = 0; i < channelCount; i++) {
        channels[i].ssrc = ssrcs[i];
        channels[i].samples = samples[i];
        channels[i].count = sampleCounts[i];
        channels[i].startSample = startSamples ? startSamples[i] : 0;
    }
    return core->core->enqueueSegment(timestampMs, channels.data(), channels.size());
}

bool BroadcastAudioCorePlay(BroadcastAudioCoreRef core, int64_t timestampMs, double hostTime, int32_t offsetMs) {
    return core->core->play(timestampMs, (int64_t)std::llround(hostTime * 1e6), offsetMs);
}

void BroadcastAudioCoreSetActive(BroadcastAudioCoreRef core, bool active) {
    core->core->setActive(active);
}

void BroadcastAudioCoreSetVolume(BroadcastAudioCoreRef core, uint32_t ssrc, float volume) {
    core->core->setVolume(ssrc, volume);
}

void BroadcastAudioCoreReset(BroadcastAudioCoreRef core) {
    core->core->reset();
}

size_t BroadcastAudioCoreTakeLevels(BroadcastAudioCoreRef core, BroadcastAudioLevel *levels, size_t capacity) {
    const auto taken = core->core->takeLevels();
    const size_t count = std::min(taken.size(), capacity);
    for (size_t i = 0; i < count; i++) {
        levels[i].ssrc = taken[i].ssrc;
        levels[i].level = taken[i].level;
        levels[i].voice = taken[i].voice;
    }
    return count;
}
