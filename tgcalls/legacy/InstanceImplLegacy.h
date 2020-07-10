#ifndef INSTANCE_IMPL_LEGACY_H
#define TGVOIP_IMPL_LEGACY_H

#include "Instance.h"

#include "VoIPController.h"
#include "VoIPServerConfig.h"

namespace tgcalls {

class InstanceImplLegacy : public Instance {
public:
	explicit InstanceImplLegacy(Descriptor &&descriptor);
	~InstanceImplLegacy();

	static int GetConnectionMaxLayer();
	static std::string GetVersion();

	void receiveSignalingData(const std::vector<uint8_t> &data) override;
	void setSendVideo(bool sendVideo) override;
	void setNetworkType(NetworkType networkType) override;
	void setMuteMicrophone(bool muteMicrophone) override;
	void setIncomingVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) override;
	void setAudioOutputGainControlEnabled(bool enabled) override;
	void setEchoCancellationStrength(int strength) override;
	void setAudioInputDevice(std::string id) override;
	void setAudioOutputDevice(std::string id) override;
	void setInputVolume(float level) override;
	void setOutputVolume(float level) override;
	void setAudioOutputDuckingEnabled(bool enabled) override;

	std::string getLastError() override;
	std::string getDebugInfo() override;
	int64_t getPreferredRelayId() override;
	TrafficStats getTrafficStats() override;
	PersistentState getPersistentState() override;
	FinalState stop() override;

private:
	tgvoip::VoIPController *controller_;
	std::function<void(State)> onStateUpdated_;
	std::function<void(int)> onSignalBarsUpdated_;

	static void ControllerStateCallback(tgvoip::VoIPController *controller, int state);
	static void SignalBarsCallback(tgvoip::VoIPController *controller, int signalBars);

};

void SetLegacyGlobalServerConfig(const std::string &serverConfig);

} // namespace tgcalls

#endif