#ifndef TGCALLS_MANAGER_H
#define TGCALLS_MANAGER_H

#include "ThreadLocalObject.h"
#include "NetworkManager.h"
#include "MediaManager.h"
#include "Instance.h"

namespace tgcalls {

class Manager final : public std::enable_shared_from_this<Manager> {
public:
	static rtc::Thread *getMediaThread();

	Manager(rtc::Thread *thread, Descriptor &&descriptor);
	~Manager();

	void start();
	void receiveSignalingData(const std::vector<uint8_t> &data);
	void setSendVideo(bool sendVideo);
	void setMuteOutgoingAudio(bool mute);
	void notifyIsLocalVideoActive(bool isActive);
	void setIncomingVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink);

private:
	void receiveSignalingMessage(SignalingMessage &&message);

	rtc::Thread *_thread;
	EncryptionKey _encryptionKey;
	bool _enableP2P = false;
	std::vector<RtcServer> _rtcServers;
	std::shared_ptr<VideoCaptureInterface> _videoCapture;
	std::function<void (const State &)> _stateUpdated;
	std::function<void (bool)> _videoStateUpdated;
	std::function<void (bool)> _remoteVideoIsActiveUpdated;
	std::function<void (const std::vector<uint8_t> &)> _signalingDataEmitted;
	std::unique_ptr<ThreadLocalObject<NetworkManager>> _networkManager;
	std::unique_ptr<ThreadLocalObject<MediaManager>> _mediaManager;
	bool _isVideoRequested = false;

};

} // namespace tgcalls

#endif
