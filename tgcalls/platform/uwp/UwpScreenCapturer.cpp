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

#include "UwpScreenCapturerShader.h"

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

// Halved while the source is at least twice the target in either direction. Powers of
// two only, deliberately - see the comment on the constants.
int Halvings(int width, int height) {
	int halvings = 0;
	while ((width >= kTargetWidth * 2 || height >= kTargetHeight * 2)
		&& width >= 4 && height >= 4) {
		width /= 2;
		height /= 2;
		++halvings;
	}

	return halvings;
}

// Even because NV12 has no odd dimensions: the odd row or column is cropped, never
// resampled away.
std::pair<int, int> CappedSize(int width, int height) {
	const int halvings = Halvings(width, height);
	return { std::max(2, (width >> halvings) & ~1), std::max(2, (height >> halvings) & ~1) };
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

	// Feature level 11 for the compute shader that converts the capture. A GPU without it,
	// or no hardware device at all, gets WARP, which runs the shader on the CPU.
	const D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

	HRESULT hr = D3D11CreateDevice(
	  /*adapter=*/nullptr, D3D_DRIVER_TYPE_HARDWARE,
	  /*software_rasterizer=*/nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
	  feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION,
	  d3d11_device_.put(), /*feature_level=*/nullptr, /*device_context=*/nullptr);
	if (FAILED(hr)) {
		hr = D3D11CreateDevice(
			/*adapter=*/nullptr, D3D_DRIVER_TYPE_WARP,
			/*software_rasterizer=*/nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			feature_levels, ARRAYSIZE(feature_levels),
			D3D11_SDK_VERSION, d3d11_device_.put(), /*feature_level=*/nullptr,
			/*device_context=*/nullptr);
	}

	if (SUCCEEDED(hr)) {
		hr = CreateConverter();
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

	DeliverPendingFrame();

	// A readback that failed has stopped the capture.
	if (!is_capture_started_) {
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

	auto new_size = capture_frame.ContentSize();

	if (previous_size_.Width != new_size.Width ||
		previous_size_.Height != new_size.Height) {
		winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice directDevice;
		direct3d_device_->QueryInterface(winrt::guid_of<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>(), winrt::put_abi(directDevice));

		frame_pool_.Recreate(directDevice, kPixelFormat, kNumBuffers, new_size);
	}

	// The frame in hand still has the previous size when the source has just been resized,
	// so only what both sizes cover is read.
	const int image_height = std::min(previous_size_.Height, new_size.Height);
	const int image_width = std::min(previous_size_.Width, new_size.Width);
	previous_size_ = new_size;

	if (_state != VideoState::Active || item_closed_
		|| image_width <= 0 || image_height <= 0) {
		return;
	}

	const auto [out_width, out_height] = CappedSize(image_width, image_height);
	const int block = 1 << Halvings(image_width, image_height);

	if (out_width != output_width_ || out_height != output_height_ || block != block_) {
		hr = CreateTextures(out_width, out_height, block);
		if (FAILED(hr)) {
			onFatalError();
			return;
		}
	}

	// Whole blocks only, from the top left: what a source that is not a multiple of the
	// block leaves over is cropped, as the even rounding crops an odd row or column.
	D3D11_BOX region = {};
	region.right = static_cast<UINT>(out_width * block);
	region.bottom = static_cast<UINT>(out_height * block);
	region.back = 1;
	d3d_context_->CopySubresourceRegion(source_.get(), /*DstSubresource=*/0,
		/*DstX=*/0, /*DstY=*/0, /*DstZ=*/0,
		texture_2D.get(), /*SrcSubresource=*/0, &region);

	ID3D11ShaderResourceView* const sources[] = { source_view_.get() };
	ID3D11UnorderedAccessView* const targets[] = { luma_view_.get(), chroma_view_.get() };
	ID3D11Buffer* const parameters[] = { converter_parameters_.get() };
	d3d_context_->CSSetShader(converter_.get(), nullptr, 0);
	d3d_context_->CSSetShaderResources(0, 1, sources);
	d3d_context_->CSSetUnorderedAccessViews(0, 2, targets, nullptr);
	d3d_context_->CSSetConstantBuffers(0, 1, parameters);

	// One thread per chroma sample, in groups of 8x8.
	d3d_context_->Dispatch((out_width / 2 + 7) / 8, (out_height / 2 + 7) / 8, 1);

	// Unbound, or the next frame's copy into source_ finds it still bound for reading.
	ID3D11ShaderResourceView* const no_sources[] = { nullptr };
	ID3D11UnorderedAccessView* const no_targets[] = { nullptr, nullptr };
	d3d_context_->CSSetShaderResources(0, 1, no_sources);
	d3d_context_->CSSetUnorderedAccessViews(0, 2, no_targets, nullptr);

	d3d_context_->CopyResource(luma_staging_.get(), luma_.get());
	d3d_context_->CopyResource(chroma_staging_.get(), chroma_.get());

	// Submitted now, so that it has run by the next tick rather than starting then.
	d3d_context_->Flush();

	pending_frame_ = capture_frame;
	pending_capture_ms_ = rtc::TimeMillis();
}

void UwpScreenCapturer::DeliverPendingFrame() {
	if (pending_frame_ == nullptr) {
		return;
	}

	// Released on the way out, once the readback below has waited out everything that
	// read it.
	const auto frame = std::move(pending_frame_);
	pending_frame_ = nullptr;

	const int width = output_width_;
	const int height = output_height_;

	rtc::scoped_refptr<webrtc::NV12Buffer> buffer = buffer_pool_.CreateNV12Buffer(width, height);
	if (buffer == nullptr) {
		buffer = webrtc::NV12Buffer::Create(width, height);
	}

	D3D11_MAPPED_SUBRESOURCE map_info;
	HRESULT hr = d3d_context_->Map(luma_staging_.get(), 0, D3D11_MAP_READ, 0, &map_info);
	if (FAILED(hr)) {
		onFatalError();
		return;
	}

	libyuv::CopyPlane(static_cast<const uint8_t*>(map_info.pData), static_cast<int>(map_info.RowPitch),
		buffer->MutableDataY(), buffer->StrideY(), width, height);
	d3d_context_->Unmap(luma_staging_.get(), 0);

	hr = d3d_context_->Map(chroma_staging_.get(), 0, D3D11_MAP_READ, 0, &map_info);
	if (FAILED(hr)) {
		onFatalError();
		return;
	}

	// Interleaved UV, so a chroma row is as many bytes as a luma one.
	libyuv::CopyPlane(static_cast<const uint8_t*>(map_info.pData), static_cast<int>(map_info.RowPitch),
		buffer->MutableDataUV(), buffer->StrideUV(), width, height / 2);
	d3d_context_->Unmap(chroma_staging_.get(), 0);

	// Compared against the frame that went out last, which is what upstream's
	// detect_updated_region option buys for the desktop capturers we cannot link: rows that
	// match cost nothing to encode. Two luma rows and the chroma row they share at a time,
	// since a change to either moves both.
	absl::optional<webrtc::VideoFrame::UpdateRect> update_rect;
	if (previous_frame_ != nullptr && previous_frame_->width() == width
		&& previous_frame_->height() == height) {
		const uint8_t* const y = buffer->DataY();
		const uint8_t* const uv = buffer->DataUV();
		const uint8_t* const previous_y = previous_frame_->DataY();
		const uint8_t* const previous_uv = previous_frame_->DataUV();
		const int stride_y = buffer->StrideY();
		const int stride_uv = buffer->StrideUV();
		const int previous_stride_y = previous_frame_->StrideY();
		const int previous_stride_uv = previous_frame_->StrideUV();

		const auto pair_differs = [&](int pair, int offset, int length) {
			return memcmp(previous_y + previous_stride_y * (pair * 2) + offset, y + stride_y * (pair * 2) + offset, length) != 0
				|| memcmp(previous_y + previous_stride_y * (pair * 2 + 1) + offset, y + stride_y * (pair * 2 + 1) + offset, length) != 0
				|| memcmp(previous_uv + previous_stride_uv * pair + offset, uv + stride_uv * pair + offset, length) != 0;
		};

		// Two samples from the middle of each pair first: the full scan reads both frames in
		// full, which is worth paying only once something has moved. A change that hides from
		// every middle sample costs nothing but the rect, since the frame is sent either way.
		const int middle = (width / 2) & ~1;
		bool differs = false;
		for (int pair = 0; pair < height / 2 && !differs; ++pair) {
			differs = pair_differs(pair, middle, 2);
		}

		int first = -1;
		int last = -1;
		if (differs) {
			for (int pair = 0; pair < height / 2; ++pair) {
				if (pair_differs(pair, 0, width)) {
					if (first < 0) {
						first = pair * 2;
					}
					last = pair * 2 + 1;
				}
			}
		}

		if (first >= 0) {
			update_rect = webrtc::VideoFrame::UpdateRect{ 0, first, width, last - first + 1 };
		}
	}

	// Delivered whether or not anything changed: an identical frame costs the encoder almost
	// nothing and is how it converges on a low quantiser. What the comparison above buys is
	// the update rect.
	last_delivery_ms_ = rtc::TimeMillis();
	OnFrame(buffer, pending_capture_ms_, update_rect);

	previous_frame_ = std::move(buffer);
}

void UwpScreenCapturer::MaybeRepeatLastFrame(int64_t now_ms) {
	if (previous_frame_ == nullptr || _state != VideoState::Active || item_closed_) {
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
	OnFrame(previous_frame_, now_ms, absl::nullopt);
}

void UwpScreenCapturer::OnClosed(GraphicsCaptureItem const& sender, winrt::Windows::Foundation::IInspectable const&)
{
	winrt::slim_lock_guard const guard(lock_);

	RTC_LOG(LS_INFO) << "Capture target has been closed.";
	item_closed_ = true;
	is_capture_started_ = false;

	onFatalError();
}

HRESULT UwpScreenCapturer::CreateConverter() {
	d3d11_device_->GetImmediateContext(d3d_context_.put());

	HRESULT hr = d3d11_device_->CreateComputeShader(kScreenToNV12Shader, sizeof(kScreenToNV12Shader),
		nullptr, converter_.put());
	if (FAILED(hr)) {
		return hr;
	}

	D3D11_BUFFER_DESC desc = {};
	desc.ByteWidth = 16;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	return d3d11_device_->CreateBuffer(&desc, nullptr, converter_parameters_.put());
}

HRESULT UwpScreenCapturer::CreateTextures(int width, int height, int block) {
	output_width_ = 0;
	output_height_ = 0;
	block_ = 0;

	source_view_ = nullptr;
	luma_view_ = nullptr;
	chroma_view_ = nullptr;

	const auto create = [this](UINT texture_width, UINT texture_height, DXGI_FORMAT format,
		UINT bind_flags, bool staging, winrt::com_ptr<ID3D11Texture2D>& texture) {
		texture = nullptr;

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = texture_width;
		desc.Height = texture_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
		desc.BindFlags = bind_flags;
		desc.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
		return d3d11_device_->CreateTexture2D(&desc, nullptr, texture.put());
	};

	HRESULT hr = create(width * block, height * block, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, false, source_);
	if (SUCCEEDED(hr)) {
		hr = d3d11_device_->CreateShaderResourceView(source_.get(), nullptr, source_view_.put());
	}
	if (SUCCEEDED(hr)) {
		hr = create(width, height, DXGI_FORMAT_R8_UNORM, D3D11_BIND_UNORDERED_ACCESS, false, luma_);
	}
	if (SUCCEEDED(hr)) {
		hr = d3d11_device_->CreateUnorderedAccessView(luma_.get(), nullptr, luma_view_.put());
	}
	if (SUCCEEDED(hr)) {
		hr = create(width / 2, height / 2, DXGI_FORMAT_R8G8_UNORM, D3D11_BIND_UNORDERED_ACCESS, false, chroma_);
	}
	if (SUCCEEDED(hr)) {
		hr = d3d11_device_->CreateUnorderedAccessView(chroma_.get(), nullptr, chroma_view_.put());
	}
	if (SUCCEEDED(hr)) {
		hr = create(width, height, DXGI_FORMAT_R8_UNORM, 0, true, luma_staging_);
	}
	if (SUCCEEDED(hr)) {
		hr = create(width / 2, height / 2, DXGI_FORMAT_R8G8_UNORM, 0, true, chroma_staging_);
	}
	if (FAILED(hr)) {
		return hr;
	}

	// The Parameters constant buffer, as UwpScreenCapturer.hlsl declares it.
	const UINT parameters[4] = { static_cast<UINT>(block), static_cast<UINT>(width / 2), static_cast<UINT>(height / 2), 0 };
	d3d_context_->UpdateSubresource(converter_parameters_.get(), 0, nullptr, parameters, 0, 0);

	output_width_ = width;
	output_height_ = height;
	block_ = block;
	return S_OK;
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
	pending_frame_ = nullptr;
	source_view_ = nullptr;
	source_ = nullptr;
	luma_view_ = nullptr;
	luma_ = nullptr;
	chroma_view_ = nullptr;
	chroma_ = nullptr;
	luma_staging_ = nullptr;
	chroma_staging_ = nullptr;
	output_width_ = 0;
	output_height_ = 0;
	block_ = 0;
	converter_parameters_ = nullptr;
	converter_ = nullptr;
	d3d_context_ = nullptr;
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

void UwpScreenCapturer::OnFrame(rtc::scoped_refptr<webrtc::NV12Buffer> buffer, int64_t timestamp_ms,
																const absl::optional<webrtc::VideoFrame::UpdateRect>& updateRect) {
	if (_state != VideoState::Active) {
		return;
	}

	webrtc::VideoFrame captureFrame =
		webrtc::VideoFrame::Builder()
		.set_video_frame_buffer(std::move(buffer))
		.set_timestamp_rtp(0)
		.set_timestamp_ms(timestamp_ms)
		.set_rotation(webrtc::kVideoRotation_0)
		.set_update_rect(updateRect)
		.build();

	_sink->OnFrame(captureFrame);
}

}  // namespace tgcalls
