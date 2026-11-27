#ifndef TGCALLS_GROUP_EXTERNAL_AUDIO_H
#define TGCALLS_GROUP_EXTERNAL_AUDIO_H

#include <cstdint>
#include <functional>
#include <vector>

#include "FakeAudioDeviceModule.h"
#include "rtc_base/synchronization/mutex.h"

namespace tgcalls {

// A group screencast's app audio, as addExternalAudioSamples hands it over (48 kHz mono s16, native
// byte order), shared by both group engines.

// Appends samples to buffer (as float s16 values), keeping the newest 2 s. An odd byte count is ignored.
void appendGroupExternalAudioSamples(std::vector<float> &buffer, webrtc::Mutex &mutex, std::vector<uint8_t> const &samples);

// FakeAudioDeviceModule's recorder for that buffer: 10 ms (480 samples) per Record() once that much is
// buffered, nothing before.
//
// levelUpdated (optional) gets the level of the audio it records: the share connection's own level, which an
// encrypted call stamps into every Opus frame's trailer and its peers read (a share connection has no microphone
// processor to measure it). It is the microphone's measure (AudioCapturePostProcessor): the peak over at least 4400
// samples, divided by 4000 (float s16), on the recording thread.
class ExternalAudioRecorder : public FakeAudioDeviceModule::Recorder {
public:
    ExternalAudioRecorder(std::vector<float> *externalAudioSamples, webrtc::Mutex *externalAudioSamplesMutex, std::function<void(float)> levelUpdated = nullptr);
    ~ExternalAudioRecorder() override;

    AudioFrame Record() override;
    int32_t WaitForUs() override;

private:
    std::vector<float> *_externalAudioSamples = nullptr;
    webrtc::Mutex *_externalAudioSamplesMutex = nullptr;
    std::vector<int16_t> _samples;
    std::function<void(float)> _levelUpdated;
    float _peak = 0.0f;
    int _peakCount = 0;
};

} // namespace tgcalls

#endif
