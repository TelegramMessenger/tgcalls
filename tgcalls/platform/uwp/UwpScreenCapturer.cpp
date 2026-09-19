#include "UwpScreenCapturer.h"

#include "api/video/i420_buffer.h"
#include "api/video/nv12_buffer.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_rotation.h"
#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/thread.h"

#include <stdint.h>
#include <string.h>
#include <memory>
#include <algorithm>
#include "third_party/libyuv/include/libyuv.h"
#include <StaticThreads.h>

namespace tgcalls {
namespace {

constexpr auto kPreferredWidth = 640;
constexpr auto kPreferredHeight = 480;
// The size a screen share aims at. Not a box to fit into: the source is halved while it is
// at least twice this, which is the rule tgcalls' desktop path uses, so a 2560x1600 screen
// goes out as 1280x800. Halving averages exactly four pixels into one; fitting a screen
// into the box instead lands on ratios like 0.45, where a text stroke falls between
// samples and smears across its neighbours - which is what made small text illegible here
// while Telegram Desktop, sharing the same screen over the same 1.3 Mbps, stayed readable.
constexpr int kTargetWidth = 1280;
constexpr int kTargetHeight = 720;
constexpr auto kPreferredFps = 24;

// We must use a BGRA pixel format that has 4 bytes per pixel, as required by
// the DesktopFrame interface.
constexpr auto kPixelFormat = winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized;

// A screen that is not changing still reaches the encoder this often, so a receiver that
// joins, or asks for a key frame, has something to decode.
constexpr int64_t kStillFrameIntervalMs = 1000;
// How long the frame pool has to stay empty before the source counts as paused.
constexpr int64_t kPauseAfterEmptyPollsMs = 2000;

// Two, as upstream's WgcCaptureSession uses: with one, TryGetNextFrame returns null
// whenever the compositor is mid-deposit, which this capturer reports as a pause.
constexpr int kNumBuffers = 2;

// Halved while the source is at least twice the target in either direction, and even
// because NV12 has no odd dimensions. Powers of two only, deliberately - see the comment
// on the constants.
std::pair<int, int> CappedSize(int width, int height) {
	while ((width >= kTargetWidth * 2 || height >= kTargetHeight * 2)
		&& width >= 4 && height >= 4) {
		width /= 2;
		height /= 2;
	}

	return { std::max(2, width & ~1), std::max(2, height & ~1) };
}


} // namespace

UwpScreenCapturer::UwpScreenCapturer(
	std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink, GraphicsCaptureItem item)
: _sink(sink),
item_(item) {
}

UwpScreenCapturer::~UwpScreenCapturer() {
	winrt::slim_lock_guard const guard(lock_);

	// item_ belongs to UwpContext, which outlives the capturer and is shared with the
	// capturers that replace it, so a handler left registered here is invoked on freed
	// memory the next time that source closes.
	if (closed_token_) {
		item_.Closed(closed_token_);
		closed_token_ = {};
	}

	_onFatalError = nullptr;
	_onPause = nullptr;

	stop();
}

void UwpScreenCapturer::create() {
	winrt::slim_lock_guard const guard(lock_);

	RTC_DCHECK(!is_capture_started_);

	if (item_closed_) {
		RTC_LOG(LS_ERROR) << "The target source has been closed.";
		//RecordStartCaptureResult(StartCaptureResult::kSourceClosed);
		onFatalError();
		return;
	}

	HRESULT hr = D3D11CreateDevice(
	  /*adapter=*/nullptr, D3D_DRIVER_TYPE_HARDWARE,
	  /*software_rasterizer=*/nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
	  /*feature_levels=*/nullptr, /*feature_levels_size=*/0, D3D11_SDK_VERSION,
	  d3d11_device_.put(), /*feature_level=*/nullptr, /*device_context=*/nullptr);
	if (hr == DXGI_ERROR_UNSUPPORTED) {
		// If a hardware device could not be created, use WARP which is a high speed
		// software device.
		hr = D3D11CreateDevice(
			/*adapter=*/nullptr, D3D_DRIVER_TYPE_WARP,
			/*software_rasterizer=*/nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			/*feature_levels=*/nullptr, /*feature_levels_size=*/0,
			D3D11_SDK_VERSION, d3d11_device_.put(), /*feature_level=*/nullptr,
			/*device_context=*/nullptr);
	}

	if (FAILED(hr)) {
		onFatalError();
		return;
	}

	RTC_DCHECK(d3d11_device_);
	RTC_DCHECK(item_);

	// Listen for the Closed event, to detect if the source we are capturing is
	// closed (e.g. application window is closed or monitor is disconnected). If
	// it is, we should abort the capture. The subscription outlives a stop, so that
	// a source closing while paused is still noticed; it is removed in the destructor.
	if (!closed_token_) {
		closed_token_ = item_.Closed({ this, &UwpScreenCapturer::OnClosed });
	}

	winrt::com_ptr<IDXGIDevice> dxgi_device;
	hr = d3d11_device_->QueryInterface(IID_PPV_ARGS(&dxgi_device));
	if (FAILED(hr)) {
		//RecordStartCaptureResult(StartCaptureResult::kDxgiDeviceCastFailed);
		onFatalError();
		return;
	}

	hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), direct3d_device_.put());
	if (FAILED(hr)) {
		//RecordStartCaptureResult(StartCaptureResult::kD3dDeviceCreationFailed);
		onFatalError();
		return;
	}

	// Cast to FramePoolStatics2 so we can use CreateFreeThreaded and avoid the
	// need to have a DispatcherQueue. We don't listen for the FrameArrived event,
	// so there's no difference.

	previous_size_ = item_.Size();
	// The capped size, not the panel's: this is what the pipeline is told to expect.
	_dimensions = CappedSize(previous_size_.Width, previous_size_.Height);

	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice directDevice;
	direct3d_device_->QueryInterface(winrt::guid_of<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>(), winrt::put_abi(directDevice));

	try
	{
		frame_pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(directDevice, kPixelFormat, kNumBuffers, previous_size_);
		//frame_pool_.FrameArrived({ this, &UwpScreenCapturer::OnFrameArrived });

		session_ = frame_pool_.CreateCaptureSession(item_);

		// Menus, tooltips and other popups of a captured window are windows of their own, and
		// stay out of the capture unless this is set. Windows 11 24H2 and later, hence the
		// try_as rather than a version check.
		if (const auto session6 = session_.try_as<IGraphicsCaptureSession6>()) {
			session6.IncludeSecondaryWindows(true);
		}

		session_.StartCapture();

		is_capture_started_ = true;
		queueController_ = DispatcherQueueController::CreateOnDedicatedThread();
		queue_ = queueController_.DispatcherQueue();

		repeatingTimer_ = queue_.CreateTimer();
		repeatingTimer_.Interval(std::chrono::milliseconds{ 1000 / kPreferredFps });
		tick_token_ = repeatingTimer_.Tick({this, &UwpScreenCapturer::OnFrameArrived});
		repeatingTimer_.Start();
	}
	catch (...)
	{
		onFatalError();
	}
}

//void UwpScreenCapturer::OnFrameArrived(Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&) {
void UwpScreenCapturer::OnFrameArrived(DispatcherQueueTimer const& sender, winrt::Windows::Foundation::IInspectable const& args) {
	winrt::slim_lock_guard const guard(lock_);

	// The WinRT calls in here throw where the D3D calls beside them return an HRESULT, and
	// the tick is reached through a WRL delegate: a throw crossing it is a fail-fast, and the
	// unwind returns the frame in flight to a pool whose device has usually gone with it.
	try {
		ProcessFrame();
	} catch (...) {
		onFatalError();
	}
}

void UwpScreenCapturer::ProcessFrame() {
	if (item_closed_) {
		RTC_LOG(LS_ERROR) << "The target source has been closed.";
		onFatalError();
		return;
	}

	// A tick already dispatched when the capture was stopped is not an error, and
	// everything it reads below has been released by then.
	if (!is_capture_started_ || _state != VideoState::Active) {
		return;
	}

	auto capture_frame = frame_pool_.TryGetNextFrame();
	if (!capture_frame) {
		// An empty pool says nothing about a pause: the compositor can be mid-deposit, and from
		// Windows 11 24H2 the system withholds frames whose content has not changed, so a
		// motionless screen produces nothing but empty polls - and this capturer keeps sending
		// the last frame through them. The source itself is what to ask: a window that is gone
		// or minimized reports no size. The run of empty polls only keeps that off the fast
		// path.
		const int64_t now_ms = rtc::TimeMillis();
		if (first_empty_poll_ms_ == 0) {
			first_empty_poll_ms_ = now_ms;
		}

		if (!_paused && now_ms - first_empty_poll_ms_ >= kPauseAfterEmptyPollsMs) {
			bool sourceIsGone = false;
			try {
				const auto size = item_.Size();
				sourceIsGone = size.Width <= 0 || size.Height <= 0;
			} catch (...) {
				// No answer is not an answer: leave the state alone.
			}

			if (sourceIsGone) {
				_paused = true;

				if (_onPause){
					_onPause(true);
				}
			}
		}

		// Windows 11 24H2 and later hand back no frame at all while the content is unchanged,
		// so the repeats a still screen needs can only come from here.
		MaybeRepeatLastFrame(now_ms);
		return /*hr*/;
	}

	first_empty_poll_ms_ = 0;

	if (_paused) {
		_paused = false;

		if (_onPause){
			_onPause(false);
		}
	}

	// We need to get this CaptureFrame as an ID3D11Texture2D so that we can get
	// the raw image data in the format required by the DesktopFrame interface.
	auto d3d_surface = capture_frame.Surface();

	auto direct3DDxgiInterfaceAccess
		= d3d_surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

	winrt::com_ptr<ID3D11Texture2D> texture_2D;
	auto hr = direct3DDxgiInterfaceAccess->GetInterface(IID_PPV_ARGS(&texture_2D));
	if (FAILED(hr)) {
		//RecordGetFrameResult(GetFrameResult::kTexture2dCastFailed);
		onFatalError();
		return;
	}

	if (!mapped_texture_) {
		hr = CreateMappedTexture(texture_2D);
		if (FAILED(hr)) {
			//RecordGetFrameResult(GetFrameResult::kCreateMappedTextureFailed);
			onFatalError();
			return;
		}
	}

	// `texture_2D` can only be read by the GPU, so it is copied into `mapped_texture_`,
	// which carries D3D11_CPU_ACCESS_READ.
	winrt::com_ptr<ID3D11DeviceContext> d3d_context;
	d3d11_device_->GetImmediateContext(d3d_context.put());

	auto new_size = capture_frame.ContentSize();

	// Before the copy, as upstream's WgcCaptureSession does: the copy below takes only the
	// region the two textures share, so a mapped texture still holding the previous size
	// would keep a band of stale pixels down its edge.
	if (previous_size_.Width != new_size.Width ||
		previous_size_.Height != new_size.Height) {
		hr = CreateMappedTexture(texture_2D, new_size.Width, new_size.Height);
		if (FAILED(hr)) {
			onFatalError();
			return;
		}

		winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice directDevice;
		direct3d_device_->QueryInterface(winrt::guid_of<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>(), winrt::put_abi(directDevice));

		frame_pool_.Recreate(directDevice, kPixelFormat, kNumBuffers, new_size);
	}

	const int image_height = std::min(previous_size_.Height, new_size.Height);
	const int image_width = std::min(previous_size_.Width, new_size.Width);
	previous_size_ = new_size;

	if (_state != VideoState::Active || item_closed_
		|| image_width <= 0 || image_height <= 0) {
		return;
	}

	// Only the shared region: CopyResource would take the whole surface, which across a
	// resize is not the size of the destination.
	D3D11_BOX region = {};
	region.right = static_cast<UINT>(image_width);
	region.bottom = static_cast<UINT>(image_height);
	region.back = 1;
	d3d_context->CopySubresourceRegion(mapped_texture_.get(), /*DstSubresource=*/0,
																			   /*DstX=*/0, /*DstY=*/0, /*DstZ=*/0,
																			   texture_2D.get(), /*SrcSubresource=*/0, &region);

	D3D11_MAPPED_SUBRESOURCE map_info;
	hr = d3d_context->Map(mapped_texture_.get(), /*subresource_index=*/0,
										D3D11_MAP_READ, /*D3D11_MAP_FLAG_DO_NOT_WAIT=*/0,
										&map_info);
	if (FAILED(hr)) {
		onFatalError();
		return;
	}

	const size_t row_length = static_cast<size_t>(image_width) * 4;
	frame_buffer_.resize(row_length * image_height);

	// Copy, unmap, and do everything else afterwards: a staging texture stays mapped for as
	// long as the CPU reads it, and working in place holds the immediate context on the
	// device the capture session shares with the compositor.
	{
		const uint8_t* src = static_cast<const uint8_t*>(map_info.pData);
		uint8_t* dst = frame_buffer_.data();
		for (int i = 0; i < image_height; ++i) {
			memcpy(dst, src, row_length);
			dst += row_length;
			src += map_info.RowPitch;
		}
	}

	d3d_context->Unmap(mapped_texture_.get(), 0);

	const auto [out_width, out_height] = CappedSize(image_width, image_height);
	const size_t out_stride = static_cast<size_t>(out_width) * 4;

	const uint8_t* out_data = frame_buffer_.data();
	int out_data_stride = static_cast<int>(row_length);
	const bool scaled = out_width != image_width || out_height != image_height;
	if (scaled) {
		scaled_buffer_.resize(out_stride * out_height);
		libyuv::ARGBScale(frame_buffer_.data(), static_cast<int>(row_length),
										image_width, image_height,
										scaled_buffer_.data(), static_cast<int>(out_stride),
										out_width, out_height, libyuv::kFilterBox);
		out_data = scaled_buffer_.data();
		out_data_stride = static_cast<int>(out_stride);
	}

	// Compared row by row against the frame that went out last, which is what upstream's
	// detect_updated_region option buys for the desktop capturers we cannot link: rows that
	// match cost nothing to encode, and a screen that has not changed at all is not sent.
	// The comparison runs on the delivered frame, so the cap pays for it too.
	absl::optional<webrtc::VideoFrame::UpdateRect> update_rect;
	bool content_changed = true;
	if (previous_buffer_.size() == out_stride * out_height) {
		// One pixel from the middle of each row first: at full resolution the row by row scan
		// below reads both frames in full, which is worth paying only once something has moved.
		// A change that hides from every middle pixel costs nothing but the rect, since the
		// frame is sent either way.
		const size_t middle = (static_cast<size_t>(out_width) / 2) * 4;
		bool differs = false;
		for (int i = 0; i < out_height && !differs; ++i) {
			const size_t offset = out_stride * i + middle;
			differs = memcmp(previous_buffer_.data() + offset, out_data + offset, 4) != 0;
		}

		int first = -1;
		int last = -1;
		if (differs) {
			for (int i = 0; i < out_height; ++i) {
				const size_t offset = out_stride * i;
				if (memcmp(previous_buffer_.data() + offset, out_data + offset, out_stride) != 0) {
					if (first < 0) {
						first = i;
					}
					last = i;
				}
			}
		}

		content_changed = first >= 0;
		if (content_changed) {
			update_rect = webrtc::VideoFrame::UpdateRect{ 0, first, out_width, last - first + 1 };
		}
	}

	// Delivered whether or not anything changed: an identical frame costs the encoder almost
	// nothing and is how it converges on a low quantiser. What the comparison above buys is
	// the update rect, and knowing when the picture last moved.
	last_delivery_ms_ = rtc::TimeMillis();
	OnFrame(out_data, out_data_stride, out_width, out_height, update_rect);

	if (scaled) {
		scaled_buffer_.swap(previous_buffer_);
	} else {
		frame_buffer_.swap(previous_buffer_);
	}
	previous_width_ = out_width;
	previous_height_ = out_height;
	previous_stride_ = out_data_stride;
}

void UwpScreenCapturer::MaybeRepeatLastFrame(int64_t now_ms) {
	if (previous_buffer_.empty() || _state != VideoState::Active || item_closed_) {
		return;
	}

	// A floor, nothing more: repeating a still screen until the encoder has converged is
	// the frame cadence adapter's job now that zero hertz mode is on. This keeps a receiver
	// fed if it is not, since from Windows 11 24H2 a motionless screen produces no frames
	// here at all.
	if (now_ms - last_delivery_ms_ < kStillFrameIntervalMs) {
		return;
	}

	last_delivery_ms_ = now_ms;
	// No update rect: nothing changed, and an empty one would tell the encoder to leave the
	// picture alone, which is the opposite of what a repeat is for.
	OnFrame(previous_buffer_.data(), previous_stride_, previous_width_, previous_height_,
				absl::nullopt);
}

void UwpScreenCapturer::OnClosed(GraphicsCaptureItem const& sender, winrt::Windows::Foundation::IInspectable const&)
{
	winrt::slim_lock_guard const guard(lock_);

	RTC_LOG(LS_INFO) << "Capture target has been closed.";
	item_closed_ = true;
	is_capture_started_ = false;

	onFatalError();
}

HRESULT UwpScreenCapturer::CreateMappedTexture(winrt::com_ptr<ID3D11Texture2D> src_texture, UINT width, UINT height) {
	if (mapped_texture_ != nullptr) {
		mapped_texture_ = nullptr;
	}

  D3D11_TEXTURE2D_DESC src_desc;
  src_texture->GetDesc(&src_desc);
  D3D11_TEXTURE2D_DESC map_desc;
  map_desc.Width = width == 0 ? src_desc.Width : width;
  map_desc.Height = height == 0 ? src_desc.Height : height;
  map_desc.MipLevels = src_desc.MipLevels;
  map_desc.ArraySize = src_desc.ArraySize;
  map_desc.Format = src_desc.Format;
  map_desc.SampleDesc = src_desc.SampleDesc;
  map_desc.Usage = D3D11_USAGE_STAGING;
  map_desc.BindFlags = 0;
  map_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  map_desc.MiscFlags = 0;
  return d3d11_device_->CreateTexture2D(&map_desc, nullptr, mapped_texture_.put());
}

void UwpScreenCapturer::setState(VideoState state) {
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

void UwpScreenCapturer::setPreferredCaptureAspectRatio(float aspectRatio) {
	_aspectRatio = aspectRatio;
}

void UwpScreenCapturer::setOnFatalError(std::function<void ()> error) {
	bool failed;
	{
		winrt::slim_lock_guard const guard(lock_);

		failed = _fatalError;
		if (!failed) {
			_onFatalError = std::move(error);
		}
	}

	// Reaches back into the app, which tears the capture down: never under lock_.
	if (failed) {
		error();
	}
}

void UwpScreenCapturer::setOnPause(std::function<void(bool)> pause) {
	bool paused;
	{
		winrt::slim_lock_guard const guard(lock_);

		paused = _paused;
		_onPause = pause;
	}

	if (paused) {
		pause(true);
	}
}

std::pair<int, int> UwpScreenCapturer::resolution() const {
	return _dimensions;
}

int UwpScreenCapturer::maxFps() const {
	return kPreferredFps;
}

void UwpScreenCapturer::stop() {
	// Detach everything up front: this also runs from the destructor, where a throwing
	// WinRT call would terminate, and where a half-torn-down capturer is not an option.
	auto const timer = std::move(repeatingTimer_);
	auto const controller = std::move(queueController_);
	auto const queue = std::move(queue_);
	auto const session = std::move(session_);
	auto const framePool = std::move(frame_pool_);
	auto const tick = tick_token_;

	tick_token_ = {};
	mapped_texture_ = nullptr;
	direct3d_device_ = nullptr;
	d3d11_device_ = nullptr;
	is_capture_started_ = false;

	try {
		if (timer != nullptr) {
			timer.Stop();
			timer.Tick(tick);
		}

		if (session != nullptr) {
			session.Close();
		}

		if (framePool != nullptr) {
			framePool.Close();
		}

		if (controller != nullptr) {
			// CreateOnDedicatedThread starts a thread that only exits once the queue is shut
			// down, and the shutdown cannot be asked for from the queue's own thread - which
			// is where a frame error lands us.
			if (queue != nullptr && queue.HasThreadAccess()) {
				StaticThreads::getWorkerThread()->PostTask([controller] {
					controller.ShutdownQueueAsync();
				});
			} else {
				controller.ShutdownQueueAsync();
			}
		}
	} catch (...) {
		// The capture is going away either way.
	}
}

void UwpScreenCapturer::onFatalError() {
	if (_fatalError) {
		return;
	}

	stop();

	_fatalError = true;
	if (_onFatalError) {
		_onFatalError();
	}
}

void UwpScreenCapturer::destroy() {
	winrt::slim_lock_guard const guard(lock_);

	// Reversible: setState brings the capture back with create(), so the item and its
	// Closed subscription are left alone.
	stop();
}

void UwpScreenCapturer::OnFrame(const uint8_t* bytes, int stride, int width, int height,
																const absl::optional<webrtc::VideoFrame::UpdateRect>& updateRect) {
	if (_state != VideoState::Active) {
		return;
	}

	const int dst_width = width & ~1;
	const int dst_height = abs(height) & ~1;

	// NV12, not I420: every Media Foundation H.264 encoder takes NV12, so converting to
	// I420 here only buys the encoder a second pass over the frame to undo it. Whatever
	// needs I420 - a software encoder, a renderer - still gets it from ToI420().
	rtc::scoped_refptr<webrtc::NV12Buffer> buffer =
		webrtc::NV12Buffer::Create(dst_width, dst_height);

	const int conversionResult = libyuv::ARGBToNV12(
		bytes, stride,
		buffer->MutableDataY(), buffer->StrideY(),
		buffer->MutableDataUV(), buffer->StrideUV(),
		dst_width, dst_height);
	if (conversionResult < 0) {
		RTC_LOG(LS_ERROR) << "Failed to convert capture frame from BGRA to NV12.";
		return;
	}

	webrtc::VideoFrame captureFrame =
		webrtc::VideoFrame::Builder()
		.set_video_frame_buffer(buffer)
		.set_timestamp_rtp(0)
		.set_timestamp_ms(rtc::TimeMillis())
		.set_rotation(webrtc::kVideoRotation_0)
		.set_update_rect(updateRect)
		.build();

	_sink->OnFrame(captureFrame);
}

}  // namespace tgcalls
