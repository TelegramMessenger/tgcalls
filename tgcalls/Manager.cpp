#include "Manager.h"

#include "rtc_base/byte_buffer.h"

namespace tgcalls {

static rtc::Thread *makeNetworkThread() {
	static std::unique_ptr<rtc::Thread> value = rtc::Thread::CreateWithSocketServer();
	value->SetName("WebRTC-Network", nullptr);
	value->Start();
	return value.get();
}


static rtc::Thread *getNetworkThread() {
	static rtc::Thread *value = makeNetworkThread();
	return value;
}

static rtc::Thread *makeMediaThread() {
	static std::unique_ptr<rtc::Thread> value = rtc::Thread::Create();
	value->SetName("WebRTC-Media", nullptr);
	value->Start();
	return value.get();
}

rtc::Thread *Manager::getMediaThread() {
	static rtc::Thread *value = makeMediaThread();
	return value;
}

Manager::Manager(rtc::Thread *thread, Descriptor &&descriptor) :
_thread(thread),
_encryptionKey(std::move(descriptor.encryptionKey)),
_enableP2P(descriptor.config.enableP2P),
_rtcServers(std::move(descriptor.rtcServers)),
_videoCapture(std::move(descriptor.videoCapture)),
_stateUpdated(std::move(descriptor.stateUpdated)),
_videoStateUpdated(std::move(descriptor.videoStateUpdated)),
_remoteVideoIsActiveUpdated(std::move(descriptor.remoteVideoIsActiveUpdated)),
_signalingDataEmitted(std::move(descriptor.signalingDataEmitted)) {
	assert(_thread->IsCurrent());
}

Manager::~Manager() {
	assert(_thread->IsCurrent());
}

void Manager::start() {
	auto weakThis = std::weak_ptr<Manager>(shared_from_this());
	_networkManager.reset(new ThreadLocalObject<NetworkManager>(getNetworkThread(), [encryptionKey = _encryptionKey, enableP2P = _enableP2P, rtcServers = _rtcServers, thread = _thread, weakThis, signalingDataEmitted = _signalingDataEmitted]() {
		return new NetworkManager(
			getNetworkThread(),
			encryptionKey,
			enableP2P,
			rtcServers,
			[thread, weakThis](const NetworkManager::State &state) {
				thread->PostTask(RTC_FROM_HERE, [weakThis, state]() {
					auto strongThis = weakThis.lock();
					if (strongThis == nullptr) {
						return;
					}
					const auto mappedState = state.isReadyToSendData
						? State::Established
						: State::Reconnecting;
					strongThis->_stateUpdated(mappedState);

					strongThis->_mediaManager->perform([state](MediaManager *mediaManager) {
						mediaManager->setIsConnected(state.isReadyToSendData);
					});
				});
			},
			[thread, weakThis](const rtc::CopyOnWriteBuffer &packet) {
				thread->PostTask(RTC_FROM_HERE, [weakThis, packet]() {
					auto strongThis = weakThis.lock();
					if (strongThis == nullptr) {
						return;
					}
					strongThis->_mediaManager->perform([packet](MediaManager *mediaManager) {
						mediaManager->receivePacket(packet);
					});
				});
			},
			[signalingDataEmitted](const SignalingMessage &message) {
				signalingDataEmitted(SerializeMessage(message));
			}
		);
	}));
	bool isOutgoing = _encryptionKey.isOutgoing;
	_mediaManager.reset(new ThreadLocalObject<MediaManager>(getMediaThread(), [isOutgoing, thread = _thread, videoCapture = _videoCapture, weakThis]() {
		return new MediaManager(
			getMediaThread(),
			isOutgoing,
			videoCapture,
			[thread, weakThis](const rtc::CopyOnWriteBuffer &packet) {
				thread->PostTask(RTC_FROM_HERE, [weakThis, packet]() {
					auto strongThis = weakThis.lock();
					if (strongThis == nullptr) {
						return;
					}
					strongThis->_networkManager->perform([packet](NetworkManager *networkManager) {
						networkManager->sendPacket(packet);
					});
				});
			},
			[thread, weakThis](bool isActive) {
				thread->PostTask(RTC_FROM_HERE, [weakThis, isActive]() {
					auto strongThis = weakThis.lock();
					if (strongThis == nullptr) {
						return;
					}
					strongThis->notifyIsLocalVideoActive(isActive);
				});
			}
		);
	}));
}

void Manager::receiveSignalingData(const std::vector<uint8_t> &data) {
	if (auto message = DeserializeMessage(data)) {
		receiveSignalingMessage(std::move(*message));
	}
}

void Manager::receiveSignalingMessage(SignalingMessage &&message) {
	const auto data = &message.data;
	if (const auto switchToVideo = absl::get_if<SwitchToVideoMessage>(data)) {
		_mediaManager->perform([](MediaManager *mediaManager) {
			mediaManager->setSendVideo(true);
		});
		_videoStateUpdated(true);
	} else if (const auto remoteVideoIsActive = absl::get_if<RemoteVideoIsActiveMessage>(data)) {
		_remoteVideoIsActiveUpdated(remoteVideoIsActive->active);
	} else if (const auto candidatesList = absl::get_if<CandidatesListMessage>(data)) {
		_networkManager->perform([message = std::move(message)](NetworkManager *networkManager) mutable {
			networkManager->receiveSignalingMessage(std::move(message));
		});
	}
}

void Manager::setSendVideo(bool sendVideo) {
	if (sendVideo) {
		if (!_isVideoRequested) {
			_isVideoRequested = true;

			_signalingDataEmitted(SerializeMessage(SignalingMessage{ SwitchToVideoMessage{} }));

			_mediaManager->perform([](MediaManager *mediaManager) {
				mediaManager->setSendVideo(true);
			});

			_videoStateUpdated(true);
		}
	}
}

void Manager::setMuteOutgoingAudio(bool mute) {
	_mediaManager->perform([mute](MediaManager *mediaManager) {
		mediaManager->setMuteOutgoingAudio(mute);
	});
}

void Manager::notifyIsLocalVideoActive(bool isActive) {
	_signalingDataEmitted(SerializeMessage(SignalingMessage{ RemoteVideoIsActiveMessage{ isActive } }));
}

void Manager::setIncomingVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	_mediaManager->perform([sink](MediaManager *mediaManager) {
		mediaManager->setIncomingVideoOutput(sink);
	});
}

} // namespace tgcalls
