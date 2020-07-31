#include "VideoCaptureInterfaceImpl.h"

#include "VideoCapturerInterface.h"
#include "Manager.h"
#include "MediaManager.h"
#include "platform/PlatformInterface.h"

namespace tgcalls {

VideoCaptureInterfaceObject::VideoCaptureInterfaceObject() {
	_useFrontCamera = true;
	_isVideoEnabled = true;
	_videoSource = PlatformInterface::SharedInstance()->makeVideoSource(Manager::getMediaThread(), MediaManager::getWorkerThread());
	//this should outlive the capturer
	_platformCapturer = PlatformInterface::SharedInstance()->makePlatformVideoCapturer(_videoSource, _useFrontCamera, [this](bool isActive) {
		if (this->_isActiveUpdated) {
			this->_isActiveUpdated(isActive);
		}
	});
}

VideoCaptureInterfaceObject::~VideoCaptureInterfaceObject() {
	if (_currentSink != nullptr) {
		//_videoSource->RemoveSink(_currentSink.get());
        _platformCapturer->setVideoOutput(nullptr);
	}
}

void VideoCaptureInterfaceObject::switchCamera() {
	_useFrontCamera = !_useFrontCamera;
    if (_platformCapturer && _currentSink) {
        _platformCapturer->setVideoOutput(nullptr);
    }
	_platformCapturer = PlatformInterface::SharedInstance()->makePlatformVideoCapturer(_videoSource, _useFrontCamera, [this](bool isActive) {
		if (this->_isActiveUpdated) {
			this->_isActiveUpdated(isActive);
		}
	});
    if (_currentSink) {
        _platformCapturer->setVideoOutput(_currentSink);
    }
    _platformCapturer->getVideoCapturerInterface()->setIsEnabled(_isVideoEnabled);
}

void VideoCaptureInterfaceObject::setIsVideoEnabled(bool isVideoEnabled) {
	if (_isVideoEnabled != isVideoEnabled) {
		_isVideoEnabled = isVideoEnabled;
		_platformCapturer->getVideoCapturerInterface()->setIsEnabled(isVideoEnabled);
	}
}

void VideoCaptureInterfaceObject::setPreferredAspectRatio(float aspectRatio) {
    _platformCapturer->setPreferredCaptureAspectRatio(aspectRatio);
}

void VideoCaptureInterfaceObject::setVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	/*if (_currentSink != nullptr) {
		_videoSource->RemoveSink(_currentSink.get());
	}
	_currentSink = sink;
	if (_currentSink != nullptr) {
		_videoSource->AddOrUpdateSink(_currentSink.get(), rtc::VideoSinkWants());
	}*/
    _platformCapturer->setVideoOutput(sink);
}

void VideoCaptureInterfaceObject::setIsActiveUpdated(std::function<void (bool)> isActiveUpdated) {
	_isActiveUpdated = isActiveUpdated;
}

VideoCaptureInterfaceImpl::VideoCaptureInterfaceImpl() :
_impl(Manager::getMediaThread(), []() {
	return new VideoCaptureInterfaceObject();
}) {
}

VideoCaptureInterfaceImpl::~VideoCaptureInterfaceImpl() = default;

void VideoCaptureInterfaceImpl::switchCamera() {
	_impl.perform(RTC_FROM_HERE, [](VideoCaptureInterfaceObject *impl) {
		impl->switchCamera();
	});
}

void VideoCaptureInterfaceImpl::setIsVideoEnabled(bool isVideoEnabled) {
	_impl.perform(RTC_FROM_HERE, [isVideoEnabled](VideoCaptureInterfaceObject *impl) {
		impl->setIsVideoEnabled(isVideoEnabled);
	});
}

void VideoCaptureInterfaceImpl::setPreferredAspectRatio(float aspectRatio) {
    _impl.perform(RTC_FROM_HERE, [aspectRatio](VideoCaptureInterfaceObject *impl) {
        impl->setPreferredAspectRatio(aspectRatio);
    });
}

void VideoCaptureInterfaceImpl::setVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	_impl.perform(RTC_FROM_HERE, [sink](VideoCaptureInterfaceObject *impl) {
		impl->setVideoOutput(sink);
	});
}

ThreadLocalObject<VideoCaptureInterfaceObject> *VideoCaptureInterfaceImpl::object() {
	return &_impl;
}

} // namespace tgcalls
