#include "group/GroupExternalAudio.h"

#include <algorithm>

#include "common_audio/include/audio_util.h"

namespace tgcalls {

void appendGroupExternalAudioSamples(std::vector<float> &buffer, webrtc::Mutex &mutex, std::vector<uint8_t> const &samples) {
    if (samples.size() % 2 != 0) {
        return;
    }
    webrtc::MutexLock lock(&mutex);

    size_t previousSize = buffer.size();
    buffer.resize(buffer.size() + samples.size() / 2);
    webrtc::S16ToFloatS16((const int16_t *)samples.data(), samples.size() / 2, buffer.data() + previousSize);

    if (buffer.size() > 2 * 48000) {
        buffer.erase(buffer.begin(), buffer.begin() + (buffer.size() - 2 * 48000));
    }
}

ExternalAudioRecorder::ExternalAudioRecorder(std::vector<float> *externalAudioSamples, webrtc::Mutex *externalAudioSamplesMutex) :
_externalAudioSamples(externalAudioSamples),
_externalAudioSamplesMutex(externalAudioSamplesMutex) {
    _samples.resize(480);
}

ExternalAudioRecorder::~ExternalAudioRecorder() {
}

AudioFrame ExternalAudioRecorder::Record() {
    AudioFrame result;

    _externalAudioSamplesMutex->Lock();
    if (!_externalAudioSamples->empty() && _externalAudioSamples->size() >= 480) {
        size_t takenSamples = std::min(_samples.size(), _externalAudioSamples->size());
        webrtc::FloatS16ToS16(_externalAudioSamples->data(), takenSamples, _samples.data());

        result.num_samples = takenSamples;

        if (takenSamples != 0) {
            _externalAudioSamples->erase(_externalAudioSamples->begin(), _externalAudioSamples->begin() + takenSamples);
        }
    } else {
        result.num_samples = 0;
    }
    _externalAudioSamplesMutex->Unlock();

    result.audio_samples = _samples.data();
    result.bytes_per_sample = 2;
    result.num_channels = 1;
    result.samples_per_sec = 48000;
    result.elapsed_time_ms = 0;
    result.ntp_time_ms = 0;

    return result;
}

int32_t ExternalAudioRecorder::WaitForUs() {
    _externalAudioSamplesMutex->Lock();
    _externalAudioSamplesMutex->Unlock();

    return 1000;
}

} // namespace tgcalls
