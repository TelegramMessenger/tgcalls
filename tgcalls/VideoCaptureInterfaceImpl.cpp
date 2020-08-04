#include "VideoCaptureInterfaceImpl.h"

#include "VideoCapturerInterface.h"
#include "Manager.h"
#include "MediaManager.h"
#include "platform/PlatformInterface.h"

namespace tgcalls {

VideoCaptureInterfaceObject::VideoCaptureInterfaceObject() {
	_videoSource = PlatformInterface::SharedInstance()->makeVideoSource(Manager::getMediaThread(), MediaManager::getWorkerThread());
	//this should outlive the capturer
	_videoCapturer = PlatformInterface::SharedInstance()->makeVideoCapturer(_videoSource, _useFrontCamera, [this](bool isActive) {
		if (this->_isActiveUpdated) {
			this->_isActiveUpdated(isActive);
		}
	});
}

VideoCaptureInterfaceObject::~VideoCaptureInterfaceObject() {
	if (_currentUncroppedSink != nullptr) {
		//_videoSource->RemoveSink(_currentSink.get());
		_videoCapturer->setUncroppedVideoOutput(nullptr);
	}
}

void VideoCaptureInterfaceObject::switchCamera() {
	_useFrontCamera = !_useFrontCamera;
    if (_videoCapturer && _currentUncroppedSink) {
		_videoCapturer->setUncroppedVideoOutput(nullptr);
    }
	_videoCapturer = PlatformInterface::SharedInstance()->makeVideoCapturer(_videoSource, _useFrontCamera, [this](bool isActive) {
		if (this->_isActiveUpdated) {
			this->_isActiveUpdated(isActive);
		}
	});
    if (_currentUncroppedSink) {
		_videoCapturer->setUncroppedVideoOutput(_currentUncroppedSink);
    }
	_videoCapturer->setIsEnabled(_isVideoEnabled);
}

void VideoCaptureInterfaceObject::setIsVideoEnabled(bool isVideoEnabled) {
	if (_isVideoEnabled != isVideoEnabled) {
		_isVideoEnabled = isVideoEnabled;
		_videoCapturer->setIsEnabled(isVideoEnabled);
	}
}

void VideoCaptureInterfaceObject::setPreferredAspectRatio(float aspectRatio) {
	_videoCapturer->setPreferredCaptureAspectRatio(aspectRatio);
}

void VideoCaptureInterfaceObject::setVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	_videoCapturer->setUncroppedVideoOutput(sink);
	_currentUncroppedSink = sink;
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
