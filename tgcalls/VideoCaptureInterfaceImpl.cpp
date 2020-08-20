#include "VideoCaptureInterfaceImpl.h"

#include "VideoCapturerInterface.h"
#include "Manager.h"
#include "MediaManager.h"
#include "platform/PlatformInterface.h"

namespace tgcalls {

VideoCaptureInterfaceObject::VideoCaptureInterfaceObject(std::string deviceId, std::shared_ptr<PlatformContext> platformContext)
: _videoSource(PlatformInterface::SharedInstance()->makeVideoSource(Manager::getMediaThread(), MediaManager::getWorkerThread())) {
	_platformContext = platformContext;
	//this should outlive the capturer
	switchToDevice(deviceId);
}

VideoCaptureInterfaceObject::~VideoCaptureInterfaceObject() {
	if (_videoCapturer && _currentUncroppedSink != nullptr) {
		//_videoSource->RemoveSink(_currentSink.get());
		_videoCapturer->setUncroppedOutput(nullptr);
	}
}

webrtc::VideoTrackSourceInterface *VideoCaptureInterfaceObject::source() {
	return _videoSource;
}

void VideoCaptureInterfaceObject::switchToDevice(std::string deviceId) {
    if (_videoCapturer && _currentUncroppedSink) {
		_videoCapturer->setUncroppedOutput(nullptr);
    }
	if (_videoSource) {
		_videoCapturer = PlatformInterface::SharedInstance()->makeVideoCapturer(_videoSource, deviceId, [this](VideoState state) {
			if (this->_stateUpdated) {
				this->_stateUpdated(state);
			}
		}, _platformContext);
	}
	if (_videoCapturer) {
		if (_preferredAspectRatio > 0) {
			_videoCapturer->setPreferredCaptureAspectRatio(_preferredAspectRatio);
		}
		if (_currentUncroppedSink) {
			_videoCapturer->setUncroppedOutput(_currentUncroppedSink);
		}
		_videoCapturer->setState(_state);
	}
}

void VideoCaptureInterfaceObject::setState(VideoState state) {
	if (_state != state) {
		_state = state;
		if (_videoCapturer) {
			_videoCapturer->setState(state);
		}
	}
}

void VideoCaptureInterfaceObject::setPreferredAspectRatio(float aspectRatio) {
	_preferredAspectRatio = aspectRatio;
	if (_videoCapturer) {
		_videoCapturer->setPreferredCaptureAspectRatio(aspectRatio);
	}
}

void VideoCaptureInterfaceObject::setOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	if (_videoCapturer) {
		_videoCapturer->setUncroppedOutput(sink);
	}
	_currentUncroppedSink = sink;
}

void VideoCaptureInterfaceObject::setStateUpdated(std::function<void(VideoState)> stateUpdated) {
	_stateUpdated = stateUpdated;
}

VideoCaptureInterfaceImpl::VideoCaptureInterfaceImpl(std::string deviceId, std::shared_ptr<PlatformContext> platformContext) :
_impl(Manager::getMediaThread(), [deviceId, platformContext]() {
	return new VideoCaptureInterfaceObject(deviceId, platformContext);
}) {
}

VideoCaptureInterfaceImpl::~VideoCaptureInterfaceImpl() = default;

void VideoCaptureInterfaceImpl::switchToDevice(std::string deviceId) {
	_impl.perform(RTC_FROM_HERE, [deviceId](VideoCaptureInterfaceObject *impl) {
		impl->switchToDevice(deviceId);
	});
}

void VideoCaptureInterfaceImpl::setState(VideoState state) {
	_impl.perform(RTC_FROM_HERE, [state](VideoCaptureInterfaceObject *impl) {
		impl->setState(state);
	});
}

void VideoCaptureInterfaceImpl::setPreferredAspectRatio(float aspectRatio) {
    _impl.perform(RTC_FROM_HERE, [aspectRatio](VideoCaptureInterfaceObject *impl) {
        impl->setPreferredAspectRatio(aspectRatio);
    });
}

void VideoCaptureInterfaceImpl::setOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	_impl.perform(RTC_FROM_HERE, [sink](VideoCaptureInterfaceObject *impl) {
		impl->setOutput(sink);
	});
}

ThreadLocalObject<VideoCaptureInterfaceObject> *VideoCaptureInterfaceImpl::object() {
	return &_impl;
}

} // namespace tgcalls
