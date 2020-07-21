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

} // namespace

rtc::Thread *Manager::getMediaThread() {
	static rtc::Thread *value = makeMediaThread();
	return value;
}

Manager::Manager(rtc::Thread *thread, Descriptor &&descriptor) :
_thread(thread),
_encryptionKey(descriptor.encryptionKey),
_signaling(EncryptedConnection::Type::Signaling, _encryptionKey),
_enableP2P(descriptor.config.enableP2P),
_rtcServers(std::move(descriptor.rtcServers)),
_videoCapture(std::move(descriptor.videoCapture)),
_stateUpdated(std::move(descriptor.stateUpdated)),
_remoteVideoIsActiveUpdated(std::move(descriptor.remoteVideoIsActiveUpdated)) {
	assert(_thread->IsCurrent());

	_sendSignalingMessage = [=, send = std::move(descriptor.signalingDataEmitted)](const Message &message) {
		if (auto prepared = _signaling.prepareForSending(message)) {
			send(prepared->bytes);
			return prepared->counter;
		}
		return uint32_t(0);
	};
	_sendTransportMessage = [=](Message &&message) {
		_networkManager->perform([message = std::move(message)](NetworkManager *networkManager) {
			networkManager->sendMessage(message);
		});
	};

	if (_videoCapture) {
		_videoState = VideoState::OutgoingRequested;
    }
}

Manager::~Manager() {
	assert(_thread->IsCurrent());
}

void Manager::start() {
	const auto weak = std::weak_ptr<Manager>(shared_from_this());
	const auto thread = _thread;
	const auto sendSignalingMessage = [=](Message &&message) {
		thread->PostTask(RTC_FROM_HERE, [=, message = std::move(message)]() mutable {
			const auto strong = weak.lock();
			if (!strong) {
				return;
			}
			strong->_sendSignalingMessage(std::move(message));
		});
	};
	_networkManager.reset(new ThreadLocalObject<NetworkManager>(getNetworkThread(), [weak, thread, sendSignalingMessage, encryptionKey = _encryptionKey, enableP2P = _enableP2P, rtcServers = _rtcServers] {
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
			[=](DecryptedMessage &&message) {
				thread->PostTask(RTC_FROM_HERE, [=, message = std::move(message)]() mutable {
					if (const auto strong = weak.lock()) {
						strong->receiveMessage(std::move(message));
					}
				});
			},
			sendSignalingMessage);
	}));
	bool isOutgoing = _encryptionKey.isOutgoing;
	_mediaManager.reset(new ThreadLocalObject<MediaManager>(getMediaThread(), [weak, isOutgoing, thread, sendSignalingMessage, videoCapture = _videoCapture]() {
		return new MediaManager(
			getMediaThread(),
			isOutgoing,
			videoCapture,
			sendSignalingMessage,
			[=](Message &&message) {
				thread->PostTask(RTC_FROM_HERE, [=, message = std::move(message)]() mutable {
					const auto strong = weak.lock();
					if (!strong) {
						return;
					}
					strong->_sendTransportMessage(std::move(message));
				});
			});
	}));
}

void Manager::receiveSignalingData(const std::vector<uint8_t> &data) {
	if (auto decrypted = _signaling.handleIncomingPacket((const char*)data.data(), data.size())) {
		receiveMessage(std::move(decrypted->main));
		for (auto &message : decrypted->additional) {
			receiveMessage(std::move(message));
		}
	}
}

void Manager::receiveMessage(DecryptedMessage &&message) {
	const auto data = &message.message.data;
	if (const auto candidatesList = absl::get_if<CandidatesListMessage>(data)) {
		_networkManager->perform([message = std::move(message)](NetworkManager *networkManager) mutable {
			networkManager->receiveSignalingMessage(std::move(message));
		});
	} else if (const auto videoFormats = absl::get_if<VideoFormatsMessage>(data)) {
		_mediaManager->perform([message = std::move(message)](MediaManager *mediaManager) mutable {
			mediaManager->receiveMessage(std::move(message));
		});
	} else if (absl::get_if<RequestVideoMessage>(data)) {
		if (_videoState == VideoState::Possible) {
            _videoState = VideoState::IncomingRequested;
            _stateUpdated(_state, _videoState);
		} else if (_videoState == VideoState::OutgoingRequested) {
            _videoState = VideoState::Active;
            _stateUpdated(_state, _videoState);

            _mediaManager->perform([videoCapture = _videoCapture](MediaManager *mediaManager) {
                mediaManager->setSendVideo(videoCapture);
            });
        }
    } else if (const auto remoteVideoIsActive = absl::get_if<RemoteVideoIsActiveMessage>(data)) {
		_remoteVideoIsActiveUpdated(remoteVideoIsActive->active);
	} else {
		_mediaManager->perform([=, message = std::move(message)](MediaManager *mediaManager) mutable {
			mediaManager->receiveMessage(std::move(message));
		});
	}
}

void Manager::requestVideo(std::shared_ptr<VideoCaptureInterface> videoCapture) {
	assert(videoCapture != nullptr);

	if (_videoCapture == videoCapture || !_didConnectOnce) {
		return;
	}
    _videoCapture = videoCapture;
    if (_videoState == VideoState::Possible) {
        _videoState = VideoState::OutgoingRequested;

		_sendTransportMessage({ RequestVideoMessage() });
        _stateUpdated(_state, _videoState);
    } else if (_videoState == VideoState::IncomingRequested) {
        _videoState = VideoState::Active;

		_sendTransportMessage({ RequestVideoMessage() });
        _stateUpdated(_state, _videoState);

        _mediaManager->perform([videoCapture](MediaManager *mediaManager) {
            mediaManager->setSendVideo(videoCapture);
        });
	} else if (_videoState == VideoState::Active) {
        _mediaManager->perform([videoCapture](MediaManager *mediaManager) {
            mediaManager->setSendVideo(videoCapture);
        });
	}
}

void Manager::setMuteOutgoingAudio(bool mute) {
	_mediaManager->perform([mute](MediaManager *mediaManager) {
		mediaManager->setMuteOutgoingAudio(mute);
	});
}

void Manager::setIncomingVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	_mediaManager->perform([sink](MediaManager *mediaManager) {
		mediaManager->setIncomingVideoOutput(sink);
	});
}

} // namespace tgcalls
