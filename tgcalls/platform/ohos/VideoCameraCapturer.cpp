// VideoCameraCapturer implementation — see VideoCameraCapturer.h.
#include "VideoCameraCapturer.h"

#include <rtc_base/logging.h>

#include <cctype>
#include <mutex>
#include <unordered_map>

#include "api/video/video_sink_interface.h"

namespace tgcalls {
namespace {

// Remembers the last applied camera facing mode per OhosVideoTrackSource. The
// underlying CameraCapturer session lives inside the source and is created only
// once (in OhosInterface::makeVideoSource); VideoCameraCapturer instances are
// recreated on every switchToDevice, so the current facing must be remembered
// across instances for a Back → Front → Back cycle to work.
std::mutex g_facingMutex;
std::unordered_map<webrtc::OhosVideoTrackSource *, std::string> g_lastFacing;

// tgcalls device ids: "" / "Front" / "Back" / "user" / "environment".
// Maps to CameraCapturer facingMode: "user" = front, "environment" = back.
std::string FacingModeForDeviceId(const std::string &deviceId) {
    std::string lower;
    for (char c : deviceId) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return (lower == "back" || lower == "environment") ? "environment" : "user";
}

}  // namespace

VideoCameraCapturer::VideoCameraCapturer(
    webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source,
    std::string deviceId,
    std::function<void(VideoState)> stateUpdated,
    std::function<void(PlatformCaptureInfo)> captureInfoUpdated)
    : _source(std::move(source)),
      _stateUpdated(std::move(stateUpdated)),
      _captureInfoUpdated(std::move(captureInfoUpdated)),
      _deviceId(std::move(deviceId)) {
    RTC_LOG(LS_INFO) << "VideoCameraCapturer ctor deviceId=" << _deviceId;

    if (!_source) {
        RTC_LOG(LS_ERROR) << "VideoCameraCapturer: null source (makeVideoSource failed)";
        return;
    }

    // OhosInterface::makeVideoSource always returns an OhosVideoTrackSource.
    _videoTrackSource = webrtc::scoped_refptr<webrtc::OhosVideoTrackSource>(
        static_cast<webrtc::OhosVideoTrackSource *>(_source.get()));
    if (!_videoTrackSource) {
        RTC_LOG(LS_ERROR) << "VideoCameraCapturer: source is not OhosVideoTrackSource";
        return;
    }

    // Forward camera start/stop events to the tgcalls state callback.
    _videoTrackSource->SetCapturerObserver(this);
}

VideoCameraCapturer::~VideoCameraCapturer() {
    RTC_LOG(LS_INFO) << "VideoCameraCapturer dtor";
    if (_videoTrackSource) {
        _videoTrackSource->SetCapturerObserver(nullptr);
        if (_uncroppedSink) {
            _source->RemoveSink(_uncroppedSink.get());
            _uncroppedSink.reset();
        }
        _videoTrackSource->Stop();
    }
}

void VideoCameraCapturer::setState(VideoState state) {
    if (_state == state) {
        return;
    }
    _state = state;
    if (!_videoTrackSource) {
        return;
    }
    switch (state) {
        case VideoState::Active: {
            RTC_LOG(LS_INFO) << "VideoCameraCapturer start capture";
            const std::string facing = FacingModeForDeviceId(_deviceId);
            bool shouldSwitch = false;
            {
                std::lock_guard<std::mutex> lock(g_facingMutex);
                auto it = g_lastFacing.find(_videoTrackSource.get());
                if (it == g_lastFacing.end()) {
                    // Fresh source: OhosInterface::makeVideoSource always starts
                    // on the front camera ("user"); skip the redundant switch.
                    g_lastFacing.emplace(_videoTrackSource.get(), "user");
                    shouldSwitch = (facing != "user");
                } else {
                    shouldSwitch = (it->second != facing);
                    if (shouldSwitch) {
                        it->second = facing;
                    }
                }
            }
            if (shouldSwitch) {
                RTC_LOG(LS_INFO) << "VideoCameraCapturer switch facingMode=" << facing;
                _videoTrackSource->SwitchCamera(facing);
            }
            _videoTrackSource->Start();
            break;
        }
        case VideoState::Paused:
        case VideoState::Inactive: {
            RTC_LOG(LS_INFO) << "VideoCameraCapturer stop capture";
            _videoTrackSource->Stop();
            break;
        }
        default:
            break;
    }
}

void VideoCameraCapturer::setPreferredCaptureAspectRatio(float aspectRatio) {
    _preferredAspectRatio = aspectRatio;
    // Per-sink adaptation (aspect/crop) is handled by OhosVideoTrackSource.
}

void VideoCameraCapturer::setUncroppedOutput(std::shared_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
    if (!_source) {
        _uncroppedSink = std::move(sink);
        return;
    }
    if (_uncroppedSink) {
        _source->RemoveSink(_uncroppedSink.get());
    }
    _uncroppedSink = std::move(sink);
    if (_uncroppedSink) {
        // Public interface -> dispatches to OhosVideoTrackSource::AddOrUpdateSink,
        // which also auto-starts the capturer once the sink is registered.
        _source->AddOrUpdateSink(_uncroppedSink.get(), webrtc::VideoSinkWants());
    }
}

int VideoCameraCapturer::getRotation() {
    // Frame rotation is carried on each webrtc::VideoFrame by the capturer.
    return 0;
}

void VideoCameraCapturer::setOnFatalError(std::function<void()> error) {
    _onFatalError = std::move(error);
}

void VideoCameraCapturer::setOnPause(std::function<void(bool)> pause) {
    _onPause = std::move(pause);
}

void VideoCameraCapturer::withNativeImplementation(std::function<void(void *)> completion) {
    // The CameraCapturer is owned privately by OhosVideoTrackSource; native
    // handle access is not exposed. Callers should use the public API instead.
    completion(nullptr);
}

void VideoCameraCapturer::OnCapturerStarted(bool success) {
    RTC_LOG(LS_INFO) << "VideoCameraCapturer::OnCapturerStarted success=" << success;
    if (success) {
        if (_stateUpdated) {
            _stateUpdated(VideoState::Active);
        }
    } else {
        if (_onFatalError) {
            _onFatalError();
        }
        if (_stateUpdated) {
            _stateUpdated(VideoState::Inactive);
        }
    }
}

void VideoCameraCapturer::OnCapturerStopped() {
    RTC_LOG(LS_INFO) << "VideoCameraCapturer::OnCapturerStopped";
    if (_stateUpdated) {
        _stateUpdated(VideoState::Inactive);
    }
}

void VideoCameraCapturer::OnFrameCaptured(
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer,
    int64_t timestampUs,
    webrtc::VideoRotation rotation) {
    // Frames are consumed by OhosVideoTrackSource's internal broadcaster; this
    // observer slot is only used for start/stop state reporting.
}

}  // namespace tgcalls
