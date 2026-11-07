#ifndef TGCALLS_UWP_SCREEN_CAPTURER_H
#define TGCALLS_UWP_SCREEN_CAPTURER_H

#include <cstdint>
#include "api/scoped_refptr.h"
#include "api/video/nv12_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_source_interface.h"
#include "common_video/include/video_frame_buffer_pool.h"
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

	// Takes, when only part of the screen changed, the area that did: the software
	// encoders use it to leave the rest alone.
	void OnFrame(rtc::scoped_refptr<webrtc::NV12Buffer> buffer, int64_t timestamp_ms,
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
	winrt::com_ptr<ID3D11DeviceContext> d3d_context_;

	// The capture is scaled and converted to NV12 on the GPU by UwpScreenCapturer.hlsl, and
	// only the result is read back: the capture region is copied into source_, the shader
	// writes luma_ and chroma_, and those are copied to the staging pair.
	winrt::com_ptr<ID3D11ComputeShader> converter_;
	winrt::com_ptr<ID3D11Buffer> converter_parameters_;
	winrt::com_ptr<ID3D11Texture2D> source_;
	winrt::com_ptr<ID3D11ShaderResourceView> source_view_;
	winrt::com_ptr<ID3D11Texture2D> luma_;
	winrt::com_ptr<ID3D11UnorderedAccessView> luma_view_;
	winrt::com_ptr<ID3D11Texture2D> chroma_;
	winrt::com_ptr<ID3D11UnorderedAccessView> chroma_view_;
	winrt::com_ptr<ID3D11Texture2D> luma_staging_;
	winrt::com_ptr<ID3D11Texture2D> chroma_staging_;
	int output_width_ = 0;
	int output_height_ = 0;
	int block_ = 0;

	// A frame is read back on the tick after the one that submitted it, by when the GPU is
	// done with it, so that Map never waits. The capture frame is held until then: the GPU
	// reads it, and once released the compositor can draw the next one into it.
	Direct3D11CaptureFrame pending_frame_ = nullptr;
	int64_t pending_capture_ms_ = 0;

	webrtc::VideoFrameBufferPool buffer_pool_;
	// The frame that went out last: it is both what the next capture is compared against and
	// what gets sent again while the screen sits still.
	rtc::scoped_refptr<webrtc::NV12Buffer> previous_frame_;
	int64_t last_delivery_ms_ = 0;
	int64_t first_empty_poll_ms_ = 0;

	void MaybeRepeatLastFrame(int64_t now_ms);
	winrt::slim_mutex lock_;
	DispatcherQueue queue_= nullptr;
	DispatcherQueueController queueController_= nullptr;
	DispatcherQueueTimer repeatingTimer_= nullptr;
	winrt::event_token tick_token_{};
	HRESULT CreateConverter();
	HRESULT CreateTextures(int width, int height, int block);
	void DeliverPendingFrame();

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
