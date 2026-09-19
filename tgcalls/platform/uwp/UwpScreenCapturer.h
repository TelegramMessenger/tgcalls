#ifndef TGCALLS_UWP_SCREEN_CAPTURER_H
#define TGCALLS_UWP_SCREEN_CAPTURER_H

#include "api/scoped_refptr.h"
#include "api/video/video_frame.h"
#include "api/video/video_source_interface.h"
#include "media/base/video_adapter.h"
#include "modules/video_capture/video_capture.h"

#include "VideoCaptureInterface.h"

#include <memory>
#include <vector>
#include <stddef.h>

#include <d3d11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.graphics.capture.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.System.h>

using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::System;

namespace tgcalls {

class UwpScreenCapturer {
public:
	explicit UwpScreenCapturer(
		std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink, GraphicsCaptureItem item);
	~UwpScreenCapturer();

	void setState(VideoState state);
	void setPreferredCaptureAspectRatio(float aspectRatio);
	void setOnFatalError(std::function<void ()> error);
	void setOnPause(std::function<void(bool)> pause);

	std::pair<int, int> resolution() const;
	int maxFps() const;

private:
	void create();
	void destroy();

	// Both assume lock_ is held.
	void stop();
	void onFatalError();

	// Takes the source stride: the caller hands over its own compacted copy, and the
	// stride is an argument rather than an assumption about the width.
	// Takes the source stride and, when only part of the screen changed, the area that
	// did: the software encoders use it to leave the rest alone.
	void OnFrame(const uint8_t* bytes, int stride, int width, int height,
		const absl::optional<webrtc::VideoFrame::UpdateRect>& updateRect);

	bool item_closed_ = false;
	bool is_capture_started_ = false;
	SizeInt32 previous_size_{};
	Direct3D11CaptureFramePool frame_pool_ = nullptr;
	GraphicsCaptureItem item_;
	winrt::event_token closed_token_{};
	GraphicsCaptureSession session_ = nullptr;
	winrt::com_ptr<ID3D11Device> d3d11_device_;
	winrt::com_ptr<IInspectable> direct3d_device_;
	winrt::com_ptr<ID3D11Texture2D> mapped_texture_ = nullptr;
	// The staging texture is copied here before it is converted. A member, because at
	// screen resolution this is several megabytes and the tick runs 30 times a second.
	std::vector<uint8_t> frame_buffer_;
	// The capture scaled down to the cap, when the screen is bigger than it.
	std::vector<uint8_t> scaled_buffer_;
	// The frame that went out last, at delivered size: it is both what the next capture is
	// compared against and what gets sent again while the screen sits still.
	std::vector<uint8_t> previous_buffer_;
	int previous_width_ = 0;
	int previous_height_ = 0;
	int previous_stride_ = 0;
	int64_t last_delivery_ms_ = 0;
	int64_t first_empty_poll_ms_ = 0;

	void MaybeRepeatLastFrame(int64_t now_ms);
	winrt::slim_mutex lock_;
	DispatcherQueue queue_= nullptr;
	DispatcherQueueController queueController_= nullptr;
	DispatcherQueueTimer repeatingTimer_= nullptr;
	winrt::event_token tick_token_{};
	HRESULT CreateMappedTexture(winrt::com_ptr<ID3D11Texture2D> src_texture, UINT width = 0, UINT height = 0);

	//void OnFrameArrived(Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const& args);
	void OnFrameArrived(DispatcherQueueTimer const& sender, winrt::Windows::Foundation::IInspectable const& args);
	void ProcessFrame();
	void OnClosed(GraphicsCaptureItem const& sender, winrt::Windows::Foundation::IInspectable const& args);

	std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> _sink;

	VideoState _state = VideoState::Inactive;
	std::pair<int, int> _dimensions;
	std::function<void()> _onFatalError;
	bool _fatalError = false;
	std::function<void(bool)> _onPause;
	bool _paused = false;
	float _aspectRatio = 0.;
};

}  // namespace tgcalls

#endif
