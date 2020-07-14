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

std::function<void(const SignalingMessage&)> SendSerialized(std::function<void(const std::vector<uint8_t> &packet)> send, EncryptionKey encryptionKey) {
	return [send = std::move(send), encryptionKey](const SignalingMessage &message) {
        auto encryptedPacket = NetworkManager::encryptPacket(SerializeMessage(message), encryptionKey);
        if (encryptedPacket.has_value()) {
            std::vector<uint8_t> result;
            result.resize(encryptedPacket->size());
            memcpy(result.data(), encryptedPacket->data(), encryptedPacket->size());
            send(result);
        }
	};
}

} // namespace

rtc::Thread *Manager::getMediaThread() {
	static rtc::Thread *value = makeMediaThread();
	return value;
}

Manager::Manager(rtc::Thread *thread, Descriptor &&descriptor) :
_thread(thread),
_encryptionKey(descriptor.encryptionKey),
_enableP2P(descriptor.config.enableP2P),
_rtcServers(std::move(descriptor.rtcServers)),
_videoCapture(std::move(descriptor.videoCapture)),
_stateUpdated(std::move(descriptor.stateUpdated)),
_remoteVideoIsActiveUpdated(std::move(descriptor.remoteVideoIsActiveUpdated)),
_sendSignalingMessage(SendSerialized(std::move(descriptor.signalingDataEmitted), std::move(descriptor.encryptionKey))),
_state(State::Reconnecting),
_videoState(VideoState::Possible),
_didConnectOnce(false) {
	assert(_thread->IsCurrent());
    if (_videoCapture != nullptr) {
        _videoState = VideoState::OutgoingRequested;
    }
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
                    if (state.isReadyToSendData) {
                        if (!strong->_didConnectOnce) {
                            strong->_didConnectOnce = true;
                            if (strong->_videoState == VideoState::OutgoingRequested) {
                                strong->_videoState = VideoState::Active;
                            }
                        }
                    }
                    strong->_state = mappedState;
					strong->_stateUpdated(mappedState, strong->_videoState);

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
    rtc::CopyOnWriteBuffer packet;
    packet.AppendData(data.data(), data.size());
    auto decryptedPacket = NetworkManager::decryptPacket(packet, _encryptionKey);
    if (decryptedPacket.has_value()) {
        if (auto message = DeserializeMessage(decryptedPacket.value())) {
            receiveSignalingMessage(std::move(*message));
        }
    }
}

void Manager::receiveSignalingMessage(SignalingMessage &&message) {
	const auto data = &message.data;
	if (const auto switchToVideo = absl::get_if<RequestVideoMessage>(data)) {
		if (_videoState == VideoState::Possible) {
            _videoState = VideoState::IncomingRequested;
            _stateUpdated(_state, _videoState);
        }
	} else if (const auto switchToVideo = absl::get_if<AcceptVideoMessage>(data)) {
        if (_videoState == VideoState::OutgoingRequested) {
            _videoState = VideoState::Active;
            _stateUpdated(_state, _videoState);

            _mediaManager->perform([videoCapture = _videoCapture](MediaManager *mediaManager) {
                mediaManager->setSendVideo(videoCapture);
            });
        }
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

void Manager::requestVideo(std::shared_ptr<VideoCaptureInterface> videoCapture) {
    if (videoCapture != nullptr) {
        _videoCapture = videoCapture;
        if (_videoState == VideoState::Possible) {
            _videoState = VideoState::OutgoingRequested;

            _sendSignalingMessage({ RequestVideoMessage() });
            _stateUpdated(_state, _videoState);
        }
    }
}

void Manager::acceptVideo(std::shared_ptr<VideoCaptureInterface> videoCapture) {
    if (videoCapture != nullptr) {
        _videoCapture = videoCapture;
        if (_videoState == VideoState::IncomingRequested) {
            _videoState = VideoState::Active;

            _sendSignalingMessage({ AcceptVideoMessage() });
            _stateUpdated(_state, _videoState);

            _mediaManager->perform([videoCapture](MediaManager *mediaManager) {
                mediaManager->setSendVideo(videoCapture);
            });
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
