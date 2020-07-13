#include "MediaManager.h"

#include "Instance.h"
#include "VideoCaptureInterfaceImpl.h"
#include "VideoCapturerInterface.h"
#include "CodecSelectHelper.h"
#include "platform/PlatformInterface.h"

#include "api/audio_codecs/audio_decoder_factory_template.h"
#include "api/audio_codecs/audio_encoder_factory_template.h"
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "media/engine/webrtc_media_engine.h"
#include "system_wrappers/include/field_trial.h"
#include "api/video/builtin_video_bitrate_allocator_factory.h"
#include "call/call.h"

namespace tgcalls {
namespace {

constexpr uint32_t ssrcAudioIncoming = 1;
constexpr uint32_t ssrcAudioOutgoing = 2;
constexpr uint32_t ssrcAudioFecIncoming = 5;
constexpr uint32_t ssrcAudioFecOutgoing = 6;
constexpr uint32_t ssrcVideoIncoming = 3;
constexpr uint32_t ssrcVideoOutgoing = 4;
constexpr uint32_t ssrcVideoFecIncoming = 7;
constexpr uint32_t ssrcVideoFecOutgoing = 8;

rtc::Thread *makeWorkerThread() {
	static std::unique_ptr<rtc::Thread> value = rtc::Thread::Create();
	value->SetName("WebRTC-Worker", nullptr);
	value->Start();
	return value.get();
}

} // namespace

rtc::Thread *MediaManager::getWorkerThread() {
	static rtc::Thread *value = makeWorkerThread();
	return value;
}

MediaManager::MediaManager(
	rtc::Thread *thread,
	bool isOutgoing,
	std::shared_ptr<VideoCaptureInterface> videoCapture,
	std::function<void(const rtc::CopyOnWriteBuffer &)> packetEmitted,
	std::function<void(bool)> localVideoCaptureActiveUpdated,
	std::function<void(const SignalingMessage &)> sendSignalingMessage) :
_thread(thread),
_eventLog(std::make_unique<webrtc::RtcEventLogNull>()),
_taskQueueFactory(webrtc::CreateDefaultTaskQueueFactory()),
_packetEmitted(std::move(packetEmitted)),
_localVideoCaptureActiveUpdated(std::move(localVideoCaptureActiveUpdated)),
_sendSignalingMessage(std::move(sendSignalingMessage)),
_videoCapture(std::move(videoCapture)) {
	_ssrcAudio.incoming = isOutgoing ? ssrcAudioIncoming : ssrcAudioOutgoing;
	_ssrcAudio.outgoing = (!isOutgoing) ? ssrcAudioIncoming : ssrcAudioOutgoing;
	_ssrcAudio.fecIncoming = isOutgoing ? ssrcAudioFecIncoming : ssrcAudioFecOutgoing;
	_ssrcAudio.fecOutgoing = (!isOutgoing) ? ssrcAudioFecIncoming : ssrcAudioFecOutgoing;
	_ssrcVideo.incoming = isOutgoing ? ssrcVideoIncoming : ssrcVideoOutgoing;
	_ssrcVideo.outgoing = (!isOutgoing) ? ssrcVideoIncoming : ssrcVideoOutgoing;
	_ssrcVideo.fecIncoming = isOutgoing ? ssrcVideoFecIncoming : ssrcVideoFecOutgoing;
	_ssrcVideo.fecOutgoing = (!isOutgoing) ? ssrcVideoFecIncoming : ssrcVideoFecOutgoing;

	_audioNetworkInterface = std::unique_ptr<MediaManager::NetworkInterfaceImpl>(new MediaManager::NetworkInterfaceImpl(this, false));
	_videoNetworkInterface = std::unique_ptr<MediaManager::NetworkInterfaceImpl>(new MediaManager::NetworkInterfaceImpl(this, true));

	webrtc::field_trial::InitFieldTrialsFromString(
		"WebRTC-Audio-SendSideBwe/Enabled/"
		"WebRTC-Audio-Allocation/min:6kbps,max:32kbps/"
		"WebRTC-Audio-OpusMinPacketLossRate/Enabled-1/"
		"WebRTC-FlexFEC-03/Enabled/"
		"WebRTC-FlexFEC-03-Advertised/Enabled/"
	);

	PlatformInterface::SharedInstance()->configurePlatformAudio();

	_videoBitrateAllocatorFactory = webrtc::CreateBuiltinVideoBitrateAllocatorFactory();

	cricket::MediaEngineDependencies mediaDeps;
	mediaDeps.task_queue_factory = _taskQueueFactory.get();
	mediaDeps.audio_encoder_factory = webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus>();
	mediaDeps.audio_decoder_factory = webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus>();

	mediaDeps.video_encoder_factory = PlatformInterface::SharedInstance()->makeVideoEncoderFactory();
	mediaDeps.video_decoder_factory = PlatformInterface::SharedInstance()->makeVideoDecoderFactory();

	_myVideoFormats = ComposeSupportedFormats(
		mediaDeps.video_encoder_factory->GetSupportedFormats(),
		mediaDeps.video_decoder_factory->GetSupportedFormats());

	mediaDeps.audio_processing = webrtc::AudioProcessingBuilder().Create();
	_mediaEngine = cricket::CreateMediaEngine(std::move(mediaDeps));
	_mediaEngine->Init();
	webrtc::Call::Config callConfig(_eventLog.get());
	callConfig.task_queue_factory = _taskQueueFactory.get();
	callConfig.trials = &_fieldTrials;
	callConfig.audio_state = _mediaEngine->voice().GetAudioState();
	_call.reset(webrtc::Call::Create(callConfig));
	_audioChannel.reset(_mediaEngine->voice().CreateMediaChannel(_call.get(), cricket::MediaConfig(), cricket::AudioOptions(), webrtc::CryptoOptions::NoGcm()));
	_videoChannel.reset(_mediaEngine->video().CreateMediaChannel(_call.get(), cricket::MediaConfig(), cricket::VideoOptions(), webrtc::CryptoOptions::NoGcm(), _videoBitrateAllocatorFactory.get()));

	_audioChannel->AddSendStream(cricket::StreamParams::CreateLegacy(_ssrcAudio.outgoing));

	const uint32_t opusClockrate = 48000;
	const uint16_t opusSdpPayload = 111;
	const char *opusSdpName = "opus";
	const uint8_t opusSdpChannels = 2;
	const uint32_t opusSdpBitrate = 0;

	const uint8_t opusMinBitrateKbps = 6;
	const uint8_t opusMaxBitrateKbps = 32;
	const uint8_t opusStartBitrateKbps = 8;
	const uint8_t opusPTimeMs = 120;

	cricket::AudioCodec opusCodec(opusSdpPayload, opusSdpName, opusClockrate, opusSdpBitrate, opusSdpChannels);
	opusCodec.AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamTransportCc));
	opusCodec.SetParam(cricket::kCodecParamMinBitrate, opusMinBitrateKbps);
	opusCodec.SetParam(cricket::kCodecParamStartBitrate, opusStartBitrateKbps);
	opusCodec.SetParam(cricket::kCodecParamMaxBitrate, opusMaxBitrateKbps);
	opusCodec.SetParam(cricket::kCodecParamUseInbandFec, 1);
	opusCodec.SetParam(cricket::kCodecParamPTime, opusPTimeMs);

	cricket::AudioSendParameters audioSendPrameters;
	audioSendPrameters.codecs.push_back(opusCodec);
	audioSendPrameters.extensions.emplace_back(webrtc::RtpExtension::kTransportSequenceNumberUri, 1);
	audioSendPrameters.options.echo_cancellation = false;
	//audioSendPrameters.options.experimental_ns = false;
	audioSendPrameters.options.noise_suppression = false;
	audioSendPrameters.options.auto_gain_control = false;
	audioSendPrameters.options.highpass_filter = false;
	audioSendPrameters.options.typing_detection = false;
	//audioSendPrameters.max_bandwidth_bps = 16000;
	audioSendPrameters.rtcp.reduced_size = true;
	audioSendPrameters.rtcp.remote_estimate = true;
	_audioChannel->SetSendParameters(audioSendPrameters);
	_audioChannel->SetInterface(_audioNetworkInterface.get(), webrtc::MediaTransportConfig());

	cricket::AudioRecvParameters audioRecvParameters;
	audioRecvParameters.codecs.emplace_back(opusSdpPayload, opusSdpName, opusClockrate, opusSdpBitrate, opusSdpChannels);
	audioRecvParameters.extensions.emplace_back(webrtc::RtpExtension::kTransportSequenceNumberUri, 1);
	audioRecvParameters.rtcp.reduced_size = true;
	audioRecvParameters.rtcp.remote_estimate = true;

	_audioChannel->SetRecvParameters(audioRecvParameters);
	_audioChannel->AddRecvStream(cricket::StreamParams::CreateLegacy(_ssrcAudio.incoming));
	_audioChannel->SetPlayout(true);

	_videoChannel->SetInterface(_videoNetworkInterface.get(), webrtc::MediaTransportConfig());

	_sendSignalingMessage({ _myVideoFormats });

	if (_videoCapture != nullptr) {
		((VideoCaptureInterfaceImpl *)_videoCapture.get())->_impl->getSyncAssumingSameThread()->setIsActiveUpdated(this->_localVideoCaptureActiveUpdated);

		setSendVideo(true);
	}
}

MediaManager::~MediaManager() {
	assert(_thread->IsCurrent());

	_call->SignalChannelNetworkState(webrtc::MediaType::AUDIO, webrtc::kNetworkDown);
	_call->SignalChannelNetworkState(webrtc::MediaType::VIDEO, webrtc::kNetworkDown);

	_audioChannel->OnReadyToSend(false);
	_audioChannel->SetSend(false);
	_audioChannel->SetAudioSend(_ssrcAudio.outgoing, false, nullptr, &_audioSource);

	_audioChannel->SetPlayout(false);

	_audioChannel->RemoveRecvStream(_ssrcAudio.incoming);
	_audioChannel->RemoveSendStream(_ssrcAudio.outgoing);

	_audioChannel->SetInterface(nullptr, webrtc::MediaTransportConfig());

	setSendVideo(false);
}

void MediaManager::setIsConnected(bool isConnected) {
	if (_isConnected == isConnected) {
		return;
	}
	_isConnected = isConnected;

	if (_isConnected) {
		_call->SignalChannelNetworkState(webrtc::MediaType::AUDIO, webrtc::kNetworkUp);
		_call->SignalChannelNetworkState(webrtc::MediaType::VIDEO, webrtc::kNetworkUp);
	} else {
		_call->SignalChannelNetworkState(webrtc::MediaType::AUDIO, webrtc::kNetworkDown);
		_call->SignalChannelNetworkState(webrtc::MediaType::VIDEO, webrtc::kNetworkDown);
	}
	if (_audioChannel) {
		_audioChannel->OnReadyToSend(_isConnected);
		_audioChannel->SetSend(_isConnected);
		_audioChannel->SetAudioSend(_ssrcAudio.outgoing, _isConnected && !_muteOutgoingAudio, nullptr, &_audioSource);
	}
	if (computeIsSendingVideo() && _videoChannel) {
		_videoChannel->OnReadyToSend(_isConnected);
		_videoChannel->SetSend(_isConnected);
	}
}

void MediaManager::receivePacket(const rtc::CopyOnWriteBuffer &packet) {
	if (packet.size() < 1) {
		return;
	}

	uint8_t header = ((uint8_t *)packet.data())[0];
	rtc::CopyOnWriteBuffer unwrappedPacket = packet.Slice(1, packet.size() - 1);

	if (header == 0xba) {
		if (_audioChannel) {
			_audioChannel->OnPacketReceived(unwrappedPacket, -1);
		}
	} else if (header == 0xbf) {
		if (_videoChannel) {
			_videoChannel->OnPacketReceived(unwrappedPacket, -1);
		}
	}
}

void MediaManager::notifyPacketSent(const rtc::SentPacket &sentPacket) {
	_call->OnSentPacket(sentPacket);
}

void MediaManager::setPeerVideoFormats(VideoFormatsMessage &&peerFormats) {
	if (!_videoCodecs.empty()) {
		return;
	}

	assert(!_videoCodecOut.has_value());
	auto formats = ComputeCommonFormats(
		_myVideoFormats,
		std::move(peerFormats));
	auto codecs = AssignPayloadTypesAndDefaultCodecs(std::move(formats));
	if (codecs.myEncoderIndex >= 0) {
		assert(codecs.myEncoderIndex < codecs.list.size());
		_videoCodecOut = codecs.list[codecs.myEncoderIndex];
	}
	_videoCodecs = std::move(codecs.list);
	if (_videoCodecOut.has_value()) {
		checkIsSendingVideoChanged(false);
	}
}

bool MediaManager::computeIsSendingVideo() const {
	return _sendVideo && _videoCodecOut.has_value();
}

void MediaManager::setSendVideo(bool sendVideo) {
	if (_sendVideo == sendVideo) {
		return;
	}
	const auto wasSending = computeIsSendingVideo();
	_sendVideo = sendVideo;
	checkIsSendingVideoChanged(wasSending);
}

void MediaManager::checkIsSendingVideoChanged(bool wasSending) {
	const auto sending = computeIsSendingVideo();
	if (sending == wasSending) {
		return;
	} else if (sending) {
		auto codec = *_videoCodecOut;

		codec.SetParam(cricket::kCodecParamMinBitrate, 64);
		codec.SetParam(cricket::kCodecParamStartBitrate, 512);
		codec.SetParam(cricket::kCodecParamMaxBitrate, 2500);

		cricket::VideoSendParameters videoSendParameters;
		videoSendParameters.codecs.push_back(codec);

		if (_enableFlexfec) {
			for (auto &c : _videoCodecs) {
				if (c.name == cricket::kFlexfecCodecName) {
					videoSendParameters.codecs.push_back(c);
					break;
				}
			}
		}

		videoSendParameters.extensions.emplace_back(webrtc::RtpExtension::kTransportSequenceNumberUri, 1);
		//send_parameters.max_bandwidth_bps = 800000;
		//send_parameters.rtcp.reduced_size = true;
		//videoSendParameters.rtcp.remote_estimate = true;
		_videoChannel->SetSendParameters(videoSendParameters);

		if (_enableFlexfec) {
			cricket::StreamParams videoSendStreamParams;
			cricket::SsrcGroup videoSendSsrcGroup(cricket::kFecFrSsrcGroupSemantics, {_ssrcVideo.outgoing, _ssrcVideo.fecOutgoing});
			videoSendStreamParams.ssrcs = {_ssrcVideo.outgoing};
			videoSendStreamParams.ssrc_groups.push_back(videoSendSsrcGroup);
			videoSendStreamParams.cname = "cname";
			_videoChannel->AddSendStream(videoSendStreamParams);

			if (_videoCapture != nullptr) {
				_videoChannel->SetVideoSend(_ssrcVideo.outgoing, NULL, ((VideoCaptureInterfaceImpl *)_videoCapture.get())->_impl->getSyncAssumingSameThread()->_videoSource.get());
			}
			_videoChannel->SetVideoSend(_ssrcVideo.fecOutgoing, NULL, nullptr);
		} else {
			_videoChannel->AddSendStream(cricket::StreamParams::CreateLegacy(_ssrcVideo.outgoing));
			if (_videoCapture != nullptr) {
				_videoChannel->SetVideoSend(_ssrcVideo.outgoing, NULL, ((VideoCaptureInterfaceImpl *)_videoCapture.get())->_impl->getSyncAssumingSameThread()->_videoSource);
			}
		}

		cricket::VideoRecvParameters videoRecvParameters;

		const auto codecs = {
			cricket::kFlexfecCodecName,
			cricket::kH264CodecName,
			cricket::kH265CodecName,
			cricket::kVp8CodecName,
			cricket::kVp9CodecName,
			cricket::kAv1CodecName,
		};
		for (const auto &c : _videoCodecs) {
			for (const auto known : codecs) {
				if (c.name == known) {
					videoRecvParameters.codecs.push_back(c);
					break;
				}
			}
		}

		videoRecvParameters.extensions.emplace_back(webrtc::RtpExtension::kTransportSequenceNumberUri, 1);
		//recv_parameters.rtcp.reduced_size = true;
		videoRecvParameters.rtcp.remote_estimate = true;

		cricket::StreamParams videoRecvStreamParams;
		cricket::SsrcGroup videoRecvSsrcGroup(cricket::kFecFrSsrcGroupSemantics, {_ssrcVideo.incoming, _ssrcVideo.fecIncoming});
		videoRecvStreamParams.ssrcs = {_ssrcVideo.incoming};
		videoRecvStreamParams.ssrc_groups.push_back(videoRecvSsrcGroup);
		videoRecvStreamParams.cname = "cname";

		_videoChannel->AddRecvStream(videoRecvStreamParams);
		_videoChannel->SetRecvParameters(videoRecvParameters);
		if (_currentIncomingVideoSink) {
			_videoChannel->SetSink(_ssrcVideo.incoming, _currentIncomingVideoSink.get());
		}

		_videoChannel->OnReadyToSend(_isConnected);
		_videoChannel->SetSend(_isConnected);
	} else {
		_videoChannel->SetVideoSend(_ssrcVideo.outgoing, NULL, nullptr);
		_videoChannel->SetVideoSend(_ssrcVideo.fecOutgoing, NULL, nullptr);

		_videoChannel->RemoveRecvStream(_ssrcVideo.incoming);
		_videoChannel->RemoveRecvStream(_ssrcVideo.fecIncoming);
		_videoChannel->RemoveSendStream(_ssrcVideo.outgoing);
		if (_enableFlexfec) {
			_videoChannel->RemoveSendStream(_ssrcVideo.fecOutgoing);
		}
	}
}

void MediaManager::setMuteOutgoingAudio(bool mute) {
	_muteOutgoingAudio = mute;

	_audioChannel->SetAudioSend(_ssrcAudio.outgoing, _isConnected && !_muteOutgoingAudio, nullptr, &_audioSource);
}

void MediaManager::setIncomingVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	_currentIncomingVideoSink = sink;
	_videoChannel->SetSink(_ssrcVideo.incoming, _currentIncomingVideoSink.get());
}

void MediaManager::receiveSignalingMessage(SignalingMessage &&message) {
	const auto formats = absl::get_if<VideoFormatsMessage>(&message.data);
	assert(formats != nullptr);
	setPeerVideoFormats(std::move(*formats));
}

MediaManager::NetworkInterfaceImpl::NetworkInterfaceImpl(MediaManager *mediaManager, bool isVideo) :
_mediaManager(mediaManager),
_isVideo(isVideo) {
}

bool MediaManager::NetworkInterfaceImpl::SendPacket(rtc::CopyOnWriteBuffer *packet, const rtc::PacketOptions& options) {
	rtc::CopyOnWriteBuffer wrappedPacket;
	uint8_t header = _isVideo ? 0xbf : 0xba;
	wrappedPacket.AppendData(&header, 1);
	wrappedPacket.AppendData(*packet);

	_mediaManager->_packetEmitted(wrappedPacket);
	rtc::SentPacket sentPacket(options.packet_id, rtc::TimeMillis(), options.info_signaled_after_sent);
	_mediaManager->notifyPacketSent(sentPacket);
	return true;
}

bool MediaManager::NetworkInterfaceImpl::SendRtcp(rtc::CopyOnWriteBuffer *packet, const rtc::PacketOptions& options) {
	rtc::CopyOnWriteBuffer wrappedPacket;
	uint8_t header = _isVideo ? 0xbf : 0xba;
	wrappedPacket.AppendData(&header, 1);
	wrappedPacket.AppendData(*packet);

	_mediaManager->_packetEmitted(wrappedPacket);
	rtc::SentPacket sentPacket(options.packet_id, rtc::TimeMillis(), options.info_signaled_after_sent);
	_mediaManager->notifyPacketSent(sentPacket);
	return true;
}

int MediaManager::NetworkInterfaceImpl::SetOption(cricket::MediaChannel::NetworkInterface::SocketType, rtc::Socket::Option, int) {
	return -1;
}

} // namespace tgcalls
