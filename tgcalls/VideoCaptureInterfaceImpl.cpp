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
	_videoCapturer = PlatformInterface::SharedInstance()->makeVideoCapturer(_videoSource, _useFrontCamera, [this](bool isActive) {
		if (this->_isActiveUpdated) {
			this->_isActiveUpdated(isActive);
		}
	});
}

VideoCaptureInterfaceObject::~VideoCaptureInterfaceObject() {
	if (_currentSink != nullptr) {
		_videoSource->RemoveSink(_currentSink.get());
	}
}

void VideoCaptureInterfaceObject::switchCamera() {
	_useFrontCamera = !_useFrontCamera;
	_videoCapturer = PlatformInterface::SharedInstance()->makeVideoCapturer(_videoSource, _useFrontCamera, [this](bool isActive) {
		if (this->_isActiveUpdated) {
			this->_isActiveUpdated(isActive);
		}
	});
}

void VideoCaptureInterfaceObject::setIsVideoEnabled(bool isVideoEnabled) {
	if (_isVideoEnabled != isVideoEnabled) {
		_isVideoEnabled = isVideoEnabled;
		_videoCapturer->setIsEnabled(isVideoEnabled);
	}
}

void VideoCaptureInterfaceObject::setVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	if (_currentSink != nullptr) {
		_videoSource->RemoveSink(_currentSink.get());
	}
	_currentSink = sink;
	if (_currentSink != nullptr) {
		_videoSource->AddOrUpdateSink(_currentSink.get(), rtc::VideoSinkWants());
	}
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
	_impl.perform([](VideoCaptureInterfaceObject *impl) {
		impl->switchCamera();
	});
}

void VideoCaptureInterfaceImpl::setIsVideoEnabled(bool isVideoEnabled) {
	_impl.perform([isVideoEnabled](VideoCaptureInterfaceObject *impl) {
		impl->setIsVideoEnabled(isVideoEnabled);
	});
}

void VideoCaptureInterfaceImpl::setVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	_impl.perform([sink](VideoCaptureInterfaceObject *impl) {
		impl->setVideoOutput(sink);
	});
}

ThreadLocalObject<VideoCaptureInterfaceObject> *VideoCaptureInterfaceImpl::object() {
	return &_impl;
}

} // namespace tgcalls
