#include "InstanceImpl.h"

#include <mutex>

#include "Manager.h"
#include "MediaManager.h"
#include "VideoCaptureInterfaceImpl.h"
#include "VideoCapturerInterface.h"

#if TARGET_OS_IPHONE || TARGET_OS_OSX
#import <Foundation/Foundation.h>
#include <sys/time.h>
#endif

namespace tgcalls {
namespace {

rtc::Thread *makeManagerThread() {
	static std::unique_ptr<rtc::Thread> value = rtc::Thread::Create();
	value->SetName("WebRTC-Manager", nullptr);
	value->Start();
	return value.get();
}


rtc::Thread *getManagerThread() {
	static rtc::Thread *value = makeManagerThread();
	return value;
}

} // namespace

class InstanceImpl::LogSinkImpl final : public rtc::LogSink {
public:
	LogSinkImpl() = default;
	~LogSinkImpl() = default;

	void OnLogMessage(const std::string &msg, rtc::LoggingSeverity severity, const char *tag) override;
	void OnLogMessage(const std::string &message, rtc::LoggingSeverity severity) override;
	void OnLogMessage(const std::string &message) override;

public:
	std::ostringstream _data;

};


void InstanceImpl::LogSinkImpl::OnLogMessage(const std::string &msg, rtc::LoggingSeverity severity, const char *tag) {
	OnLogMessage(std::string(tag) + ": " + msg);
}

void InstanceImpl::LogSinkImpl::OnLogMessage(const std::string &message, rtc::LoggingSeverity severity) {
	OnLogMessage(message);
}

void InstanceImpl::LogSinkImpl::OnLogMessage(const std::string &message) {
	time_t rawTime;
	time(&rawTime);
	struct tm timeinfo;
	timeval curTime = { 0 };

#ifdef TARGET_OS_WIN
	localtime_s(&timeinfo, &rawTime);

	FILETIME ft;
	unsigned __int64 full = 0;
	GetSystemTimeAsFileTime(&ft);

	full |= ft.dwHighDateTime;
	full <<= 32;
	full |= ft.dwLowDateTime;

	const auto deltaEpochInMicrosecs = 11644473600000000Ui64;
	full -= deltaEpochInMicrosecs;
	full /= 10;
	curTime.tv_sec = (long)(full / 1000000UL);
	curTime.tv_usec = (long)(full % 1000000UL);
#else
	localtime_r(&rawTime, &timeinfo);
	gettimeofday(&curTime, nullptr);
#endif

	int32_t milliseconds = curTime.tv_usec / 1000;

	_data << (timeinfo.tm_year + 1900);
	_data << "-" << (timeinfo.tm_mon + 1);
	_data << "-" << (timeinfo.tm_mday);
	_data << " " << timeinfo.tm_hour;
	_data << ":" << timeinfo.tm_min;
	_data << ":" << timeinfo.tm_sec;
	_data << ":" << milliseconds;
	_data << " " << message;
}

InstanceImpl::InstanceImpl(Descriptor &&descriptor)
: _logSink(std::make_unique<LogSinkImpl>())
, _stateUpdated(std::move(descriptor.stateUpdated))
, _signalingDataEmitted(std::move(descriptor.signalingDataEmitted)) {
	static const auto onceToken = [] {
		rtc::LogMessage::LogToDebug(rtc::LS_INFO);
		rtc::LogMessage::SetLogToStderr(true);
		return 0;
	}();
	rtc::LogMessage::AddLogToStream(_logSink.get(), rtc::LS_INFO);

	_manager.reset(new ThreadLocalObject<Manager>(getManagerThread(), [descriptor = std::move(descriptor)]() mutable {
		return new Manager(getManagerThread(), std::move(descriptor));
	}));
	_manager->perform([](Manager *manager) {
		manager->start();
	});
}

InstanceImpl::~InstanceImpl() {
	rtc::LogMessage::RemoveLogToStream(_logSink.get());
}

void InstanceImpl::receiveSignalingData(const std::vector<uint8_t> &data) {
	_manager->perform([data](Manager *manager) {
		manager->receiveSignalingData(data);
	});
};

void InstanceImpl::setSendVideo(bool sendVideo) {
	_manager->perform([sendVideo](Manager *manager) {
		manager->setSendVideo(sendVideo);
	});
};

void InstanceImpl::setNetworkType(NetworkType networkType) {
	/*message::NetworkType mappedType;

	switch (networkType) {
		case NetworkType::Unknown:
			mappedType = message::NetworkType::nUnknown;
			break;
		case NetworkType::Gprs:
			mappedType = message::NetworkType::nGprs;
			break;
		case NetworkType::Edge:
			mappedType = message::NetworkType::nEdge;
			break;
		case NetworkType::ThirdGeneration:
			mappedType = message::NetworkType::n3gOrAbove;
			break;
		case NetworkType::Hspa:
			mappedType = message::NetworkType::n3gOrAbove;
			break;
		case NetworkType::Lte:
			mappedType = message::NetworkType::n3gOrAbove;
			break;
		case NetworkType::WiFi:
			mappedType = message::NetworkType::nHighSpeed;
			break;
		case NetworkType::Ethernet:
			mappedType = message::NetworkType::nHighSpeed;
			break;
		case NetworkType::OtherHighSpeed:
			mappedType = message::NetworkType::nHighSpeed;
			break;
		case NetworkType::OtherLowSpeed:
			mappedType = message::NetworkType::nEdge;
			break;
		case NetworkType::OtherMobile:
			mappedType = message::NetworkType::n3gOrAbove;
			break;
		case NetworkType::Dialup:
			mappedType = message::NetworkType::nGprs;
			break;
		default:
			mappedType = message::NetworkType::nUnknown;
			break;
	}

	controller_->SetNetworkType(mappedType);*/
}

void InstanceImpl::setMuteMicrophone(bool muteMicrophone) {
	_manager->perform([muteMicrophone](Manager *manager) {
		manager->setMuteOutgoingAudio(muteMicrophone);
	});
}

void InstanceImpl::setIncomingVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	_manager->perform([sink](Manager *manager) {
		manager->setIncomingVideoOutput(sink);
	});
}

void InstanceImpl::setAudioOutputGainControlEnabled(bool enabled) {
}

void InstanceImpl::setEchoCancellationStrength(int strength) {
}

void InstanceImpl::setAudioInputDevice(std::string id) {
	// TODO: not implemented
}

void InstanceImpl::setAudioOutputDevice(std::string id) {
	// TODO: not implemented
}

void InstanceImpl::setInputVolume(float level) {
	// TODO: not implemented
}

void InstanceImpl::setOutputVolume(float level) {
	// TODO: not implemented
}

void InstanceImpl::setAudioOutputDuckingEnabled(bool enabled) {
	// TODO: not implemented
}

std::string InstanceImpl::getLastError() {
	return "";  // TODO: not implemented
}

std::string InstanceImpl::getDebugInfo() {
	return "";  // TODO: not implemented
}

int64_t InstanceImpl::getPreferredRelayId() {
	return 0;  // we don't have endpoint ids
}

TrafficStats InstanceImpl::getTrafficStats() {
	return TrafficStats{};  // TODO: not implemented
}

PersistentState InstanceImpl::getPersistentState() {
	return PersistentState{};  // we dont't have such information
}

FinalState InstanceImpl::stop() {
	FinalState finalState;
	finalState.debugLog = _logSink->_data.str();
	finalState.isRatingSuggested = false;

	return finalState;
}

/*void InstanceImpl::controllerStateCallback(Controller::State state) {
	if (onStateUpdated_) {
		const auto mappedState = [&] {
			switch (state) {
			case Controller::State::WaitInit:
				return State::WaitInit;
			case Controller::State::WaitInitAck:
				return State::WaitInitAck;
			case Controller::State::Established:
				return State::Estabilished;
			case Controller::State::Failed:
				return State::Failed;
			case Controller::State::Reconnecting:
				return State::Reconnecting;
			default:
				return State::Estabilished;
			}
		}();

		onStateUpdated_(mappedState);
	}
}*/

int InstanceImpl::GetConnectionMaxLayer() {
	return 92;  // TODO: retrieve from LayerBase
}

std::string InstanceImpl::GetVersion() {
	return "2.7.7"; // TODO: version not known while not released
}

template <>
bool Register<InstanceImpl>() {
	return Meta::RegisterOne<InstanceImpl>();
}

} // namespace tgcalls
