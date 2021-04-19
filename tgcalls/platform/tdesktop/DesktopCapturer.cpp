#include "tgcalls/platform/tdesktop/DesktopCapturer.h"

#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_and_cursor_composer.h"
#include "third_party/libyuv/include/libyuv.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_rotation.h"
#include "rtc_base/checks.h"
#include "rtc_base/thread.h"
#include "rtc_base/logging.h"
#include "system_wrappers/include/clock.h"

#include <stdint.h>
#include <memory>
#include <algorithm>

#ifdef WEBRTC_WIN

#include <d3d11.h>

HRESULT WINAPI D3D11CreateDevice(
		_In_opt_ IDXGIAdapter* pAdapter,
		D3D_DRIVER_TYPE DriverType,
		HMODULE Software,
		UINT Flags,
		_In_reads_opt_(FeatureLevels) CONST D3D_FEATURE_LEVEL* pFeatureLevels,
		UINT FeatureLevels,
		UINT SDKVersion,
		_COM_Outptr_opt_ ID3D11Device** ppDevice,
		_Out_opt_ D3D_FEATURE_LEVEL* pFeatureLevel,
		_COM_Outptr_opt_ ID3D11DeviceContext** ppImmediateContext) {
	return S_FALSE;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, _COM_Outptr_ void **ppFactory) {
	return S_FALSE;
}

#endif

namespace tgcalls {
namespace {

constexpr auto kPreferredWidth = 640;
constexpr auto kPreferredHeight = 480;
constexpr auto kPreferredFps = 30;

rtc::Thread *makeDesktopThread() {
	static std::unique_ptr<rtc::Thread> value = rtc::Thread::Create();
	value->SetName("WebRTC-DesktopCapturer", nullptr);
	value->Start();
	return value.get();
}

rtc::Thread *getDesktopThread() {
	static rtc::Thread *value = makeDesktopThread();
	return value;
}

} // namespace

DesktopCapturer::DesktopCapturer(
	rtc::VideoSinkInterface<webrtc::VideoFrame> *sink)
: _sink(sink)
, _thread(getDesktopThread()) {
}

DesktopCapturer::~DesktopCapturer() {
	destroy();
}

void DesktopCapturer::create() {
	_thread->Invoke<void>(RTC_FROM_HERE, [&] {
		auto options = webrtc::DesktopCaptureOptions::CreateDefault();
		options.set_detect_updated_region(true);

#ifdef WEBRTC_WIN
		options.set_allow_directx_capturer(true);
#endif // WEBRTC_WIN

		_module = std::make_shared<webrtc::DesktopAndCursorComposer>(
			webrtc::DesktopCapturer::CreateScreenCapturer(options),
			options);
		_guard = _module;
		if (_module) {
			_module->Start(this);
			captureAndSchedule();
		}
	});
}

void DesktopCapturer::captureAndSchedule() {
	_thread->PostDelayedTask(RTC_FROM_HERE, [this, guard = _guard] {
		if (!guard.lock()) {
			return;
		}
		captureAndSchedule();
	}, 1000 / 24);
	_module->CaptureFrame();
}

void DesktopCapturer::setState(VideoState state) {
	if (_state == state) {
		return;
	}
	_state = state;
	if (_state == VideoState::Active) {
		create();
	} else {
		destroy();
	}
}

void DesktopCapturer::setDeviceId(std::string deviceId) {
	if (_requestedDeviceId == deviceId) {
		return;
	}
	destroy();
	_requestedDeviceId = deviceId;
	if (_state == VideoState::Active) {
		create();
	}
}

void DesktopCapturer::setPreferredCaptureAspectRatio(float aspectRatio) {
	_aspectRatio = aspectRatio;
}

std::pair<int, int> DesktopCapturer::resolution() const {
	return _dimensions;
}

void DesktopCapturer::destroy() {
	if (!_module) {
		return;
	}
	_thread->Invoke<void>(RTC_FROM_HERE, [&] {
		_module = nullptr;
	});
}

void DesktopCapturer::OnCaptureResult(
		webrtc::DesktopCapturer::Result result,
		std::unique_ptr<webrtc::DesktopFrame> frame) {
	if (_state != VideoState::Active
		|| result != webrtc::DesktopCapturer::Result::SUCCESS
		|| !frame) {
		return;
	}

	const auto width = frame->size().width();
	const auto height = frame->size().height();
	const auto yStride = width;
	const auto uvStride = (width + 1) / 2;
	if (!_i420buffer.get()
		|| _i420buffer->width() != width
		|| _i420buffer->height() != height) {
		_i420buffer = webrtc::I420Buffer::Create(
			width,
			height,
			yStride,
			uvStride,
			uvStride);
	}

	libyuv::ConvertToI420(
		frame->data(),
		width * height,
		_i420buffer->MutableDataY(),
		_i420buffer->StrideY(),
		_i420buffer->MutableDataU(),
		_i420buffer->StrideU(),
		_i420buffer->MutableDataV(),
		_i420buffer->StrideV(),
		0,
		0,
		width,
		height,
		width,
		height,
		libyuv::kRotate0,
		libyuv::FOURCC_ARGB);

	const auto nativeVideoFrame = webrtc::VideoFrame(
		_i420buffer,
		webrtc::kVideoRotation_0,
		webrtc::Clock::GetRealTimeClock()->CurrentTime().us());
	_sink->OnFrame(nativeVideoFrame);
}

}  // namespace tgcalls
