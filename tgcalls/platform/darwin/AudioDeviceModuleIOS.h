#ifndef TGCALLS_AUDIO_DEVICE_MODULE_IOS
#define TGCALLS_AUDIO_DEVICE_MODULE_IOS

namspace tgcalls {

class WrappedAudioDeviceModule : public webrtc::AudioDeviceModule {
private:
    rtc::scoped_refptr<webrtc::AudioDeviceModule> _impl;

public:
    WrappedAudioDeviceModule(rtc::scoped_refptr<webrtc::AudioDeviceModule> impl) :
    _impl(impl) {
    }

    virtual ~WrappedAudioDeviceModule() {
    }

    virtual int32_t ActiveAudioLayer(AudioLayer *audioLayer) const override {
        return _impl->ActiveAudioLayer(audioLayer);
    }

    virtual int32_t RegisterAudioCallback(webrtc::AudioTransport *audioCallback) override {
        return _impl->RegisterAudioCallback(audioCallback);
    }

    virtual int32_t Init() override {
        return _impl->Init();
    }

    virtual int32_t Terminate() override {
        return _impl->Terminate();
    }

    virtual bool Initialized() const override {
        return _impl->Initialized();
    }

    virtual int16_t PlayoutDevices() override {
        return _impl->PlayoutDevices();
    }

    virtual int16_t RecordingDevices() override {
        return _impl->RecordingDevices();
    }

    virtual int32_t PlayoutDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize], char guid[webrtc::kAdmMaxGuidSize]) override {
        return _impl->PlayoutDeviceName(index, name, guid);
    }

    virtual int32_t RecordingDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize], char guid[webrtc::kAdmMaxGuidSize]) override {
        return _impl->RecordingDeviceName(index, name, guid);
    }

    virtual int32_t SetPlayoutDevice(uint16_t index) override {
        return _impl->SetPlayoutDevice(index);
    }

    virtual int32_t SetPlayoutDevice(WindowsDeviceType device) override {
        return _impl->SetPlayoutDevice(device);
    }

    virtual int32_t SetRecordingDevice(uint16_t index) override {
        return _impl->SetRecordingDevice(index);
    }

    virtual int32_t SetRecordingDevice(WindowsDeviceType device) override {
        return _impl->SetRecordingDevice(device);
    }

    virtual int32_t PlayoutIsAvailable(bool *available) override {
        return _impl->PlayoutIsAvailable(available);
    }

    virtual int32_t InitPlayout() override {
        return _impl->InitPlayout();
    }

    virtual bool PlayoutIsInitialized() const override {
        return _impl->PlayoutIsInitialized();
    }

    virtual int32_t RecordingIsAvailable(bool *available) override {
        return _impl->RecordingIsAvailable(available);
    }

    virtual int32_t InitRecording() override {
        return _impl->InitRecording();
    }

    virtual bool RecordingIsInitialized() const override {
        return _impl->RecordingIsInitialized();
    }

    virtual int32_t StartPlayout() override {
        return _impl->StartPlayout();
    }

    virtual int32_t StopPlayout() override {
        return _impl->StopPlayout();
    }

    virtual bool Playing() const override {
        return _impl->Playing();
    }

    virtual int32_t StartRecording() override {
        return _impl->StartRecording();
    }

    virtual int32_t StopRecording() override {
        return _impl->StopRecording();
    }

    virtual bool Recording() const override {
        return _impl->Recording();
    }

    virtual int32_t InitSpeaker() override {
        return _impl->InitSpeaker();
    }

    virtual bool SpeakerIsInitialized() const override {
        return _impl->SpeakerIsInitialized();
    }

    virtual int32_t InitMicrophone() override {
        return _impl->InitMicrophone();
    }

    virtual bool MicrophoneIsInitialized() const override {
        return _impl->MicrophoneIsInitialized();
    }

    virtual int32_t SpeakerVolumeIsAvailable(bool *available) override {
        return _impl->SpeakerVolumeIsAvailable(available);
    }

    virtual int32_t SetSpeakerVolume(uint32_t volume) override {
        return _impl->SetSpeakerVolume(volume);
    }

    virtual int32_t SpeakerVolume(uint32_t* volume) const override {
        return _impl->SpeakerVolume(volume);
    }

    virtual int32_t MaxSpeakerVolume(uint32_t *maxVolume) const override {
        return _impl->MaxSpeakerVolume(maxVolume);
    }

    virtual int32_t MinSpeakerVolume(uint32_t *minVolume) const override {
        return _impl->MinSpeakerVolume(minVolume);
    }

    virtual int32_t MicrophoneVolumeIsAvailable(bool *available) override {
        return _impl->MicrophoneVolumeIsAvailable(available);
    }

    virtual int32_t SetMicrophoneVolume(uint32_t volume) override {
        return _impl->SetMicrophoneVolume(volume);
    }

    virtual int32_t MicrophoneVolume(uint32_t *volume) const override {
        return _impl->MicrophoneVolume(volume);
    }

    virtual int32_t MaxMicrophoneVolume(uint32_t *maxVolume) const override {
        return _impl->MaxMicrophoneVolume(maxVolume);
    }

    virtual int32_t MinMicrophoneVolume(uint32_t *minVolume) const override {
        return _impl->MinMicrophoneVolume(minVolume);
    }

    virtual int32_t SpeakerMuteIsAvailable(bool *available) override {
        return _impl->SpeakerMuteIsAvailable(available);
    }

    virtual int32_t SetSpeakerMute(bool enable) override {
        return _impl->SetSpeakerMute(enable);
    }

    virtual int32_t SpeakerMute(bool *enabled) const override {
        return _impl->SpeakerMute(enabled);
    }

    virtual int32_t MicrophoneMuteIsAvailable(bool *available) override {
        return _impl->MicrophoneMuteIsAvailable(available);
    }

    virtual int32_t SetMicrophoneMute(bool enable) override {
        return _impl->SetMicrophoneMute(enable);
    }

    virtual int32_t MicrophoneMute(bool *enabled) const override {
        return _impl->MicrophoneMute(enabled);
    }

    virtual int32_t StereoPlayoutIsAvailable(bool *available) const override {
        return _impl->StereoPlayoutIsAvailable(available);
    }

    virtual int32_t SetStereoPlayout(bool enable) override {
        return _impl->SetStereoPlayout(enable);
    }

    virtual int32_t StereoPlayout(bool *enabled) const override {
        return _impl->StereoPlayout(enabled);
    }

    virtual int32_t StereoRecordingIsAvailable(bool *available) const override {
        return _impl->StereoRecordingIsAvailable(available);
    }

    virtual int32_t SetStereoRecording(bool enable) override {
        return _impl->SetStereoRecording(enable);
    }

    virtual int32_t StereoRecording(bool *enabled) const override {
        return _impl->StereoRecording(enabled);
    }

    virtual int32_t PlayoutDelay(uint16_t* delayMS) const override {
        return _impl->PlayoutDelay(delayMS);
    }

    virtual bool BuiltInAECIsAvailable() const override {
        return _impl->BuiltInAECIsAvailable();
    }

    virtual bool BuiltInAGCIsAvailable() const override {
        return _impl->BuiltInAGCIsAvailable();
    }

    virtual bool BuiltInNSIsAvailable() const override {
        return _impl->BuiltInNSIsAvailable();
    }

    virtual int32_t EnableBuiltInAEC(bool enable) override {
        return _impl->EnableBuiltInAEC(enable);
    }

    virtual int32_t EnableBuiltInAGC(bool enable) override {
        return _impl->EnableBuiltInAGC(enable);
    }

    virtual int32_t EnableBuiltInNS(bool enable) override {
        return _impl->EnableBuiltInNS(enable);
    }

    virtual int32_t GetPlayoutUnderrunCount() const override {
        return _impl->GetPlayoutUnderrunCount();
    }

#if defined(WEBRTC_IOS)
    virtual int GetPlayoutAudioParameters(webrtc::AudioParameters *params) const override {
        return _impl->GetPlayoutAudioParameters(params);
    }
    virtual int GetRecordAudioParameters(webrtc::AudioParameters *params) const override {
        return _impl->GetRecordAudioParameters(params);
    }
#endif  // WEBRTC_IOS
};

}

#endif
