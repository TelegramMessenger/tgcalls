#include "Manager.h"

#include "rtc_base/byte_buffer.h"

namespace tgcalls {
namespace {

rtc::Thread *makeNetworkThread() {
	static std::unique_ptr<rtc::Thread> value = rtc::Thread::CreateWithSocketServer();
	value->SetName("WebRTC-Network", nullptr);
	value->Start();
	return value.get();
}

rtc::Thread *getNetworkThread() {
	static rtc::Thread *value = makeNetworkThread();
	return value;
}

rtc::Thread *makeMediaThread() {
	static std::unique_ptr<rtc::Thread> value = rtc::Thread::Create();
	value->SetName("WebRTC-Media", nullptr);
	value->Start();
	return value.get();
}

std::function<void(const SignalingMessage&)> SendSerialized(std::function<void(const std::vector<uint8_t> &)> send) {
	return [send = std::move(send)](const SignalingMessage &message) {
		send(SerializeMessage(message));
	};
}

} // namespace

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
_sendSignalingMessage(SendSerialized(std::move(descriptor.signalingDataEmitted))) {
	assert(_thread->IsCurrent());
}

Manager::~Manager() {
	assert(_thread->IsCurrent());
}

void Manager::start() {
	const auto weak = std::weak_ptr<Manager>(shared_from_this());
	_networkManager.reset(new ThreadLocalObject<NetworkManager>(getNetworkThread(), [weak, encryptionKey = _encryptionKey, enableP2P = _enableP2P, rtcServers = _rtcServers, thread = _thread, sendSignalingMessage = _sendSignalingMessage] {
		return new NetworkManager(
			getNetworkThread(),
			encryptionKey,
			enableP2P,
			rtcServers,
			[=](const NetworkManager::State &state) {
				thread->PostTask(RTC_FROM_HERE, [=] {
					const auto strong = weak.lock();
					if (!strong) {
						return;
					}
					const auto mappedState = state.isReadyToSendData
						? State::Established
						: State::Reconnecting;
					strong->_stateUpdated(mappedState);

					strong->_mediaManager->perform([=](MediaManager *mediaManager) {
						mediaManager->setIsConnected(state.isReadyToSendData);
					});
				});
			},
			[=](const rtc::CopyOnWriteBuffer &packet) {
				thread->PostTask(RTC_FROM_HERE, [=] {
					const auto strong = weak.lock();
					if (!strong) {
						return;
					}
					strong->_mediaManager->perform([=](MediaManager *mediaManager) {
						mediaManager->receivePacket(packet);
					});
				});
			},
			sendSignalingMessage);
	}));
	bool isOutgoing = _encryptionKey.isOutgoing;
	_mediaManager.reset(new ThreadLocalObject<MediaManager>(getMediaThread(), [weak, isOutgoing, thread = _thread, videoCapture = _videoCapture, sendSignalingMessage = _sendSignalingMessage]() {
		return new MediaManager(
			getMediaThread(),
			isOutgoing,
			videoCapture,
			[=](const rtc::CopyOnWriteBuffer &packet) {
				thread->PostTask(RTC_FROM_HERE, [=] {
					const auto strong = weak.lock();
					if (!strong) {
						return;
					}
					strong->_networkManager->perform([packet](NetworkManager *networkManager) {
						networkManager->sendPacket(packet);
					});
				});
			},
			[=](bool isActive) {
				thread->PostTask(RTC_FROM_HERE, [=] {
					const auto strong = weak.lock();
					if (!strong) {
						return;
					}
					strong->notifyIsLocalVideoActive(isActive);
				});
			},
			sendSignalingMessage);
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
	} else if (const auto videoFormats = absl::get_if<VideoFormatsMessage>(data)) {
		_mediaManager->perform([message = std::move(message)](MediaManager *mediaManager) mutable {
			mediaManager->receiveSignalingMessage(std::move(message));
		});
	}
}

void Manager::setSendVideo(bool sendVideo) {
	if (sendVideo) {
		if (!_isVideoRequested) {
			_isVideoRequested = true;

			_sendSignalingMessage({ SwitchToVideoMessage{} });

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
	_sendSignalingMessage({ RemoteVideoIsActiveMessage{ isActive } });
}

void Manager::setIncomingVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	_mediaManager->perform([sink](MediaManager *mediaManager) {
		mediaManager->setIncomingVideoOutput(sink);
	});
}

} // namespace tgcalls
