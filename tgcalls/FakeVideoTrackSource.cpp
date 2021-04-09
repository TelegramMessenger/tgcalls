#include "FakeVideoTrackSource.h"

#include "api/video/i420_buffer.h"
#include "media/base/video_broadcaster.h"
#include "pc/video_track_source.h"

#include "libyuv.h"

#include <thread>

namespace tgcalls {

int WIDTH = 1280;
int HEIGHT = 720;

webrtc::VideoFrame genFrame(int i, int n) {
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  int width = WIDTH;
  int height = HEIGHT;
  auto bytes_ptr = std::make_unique<std::uint8_t[]>(width * height * 3);
  auto bytes = bytes_ptr.get();
  auto set_rgb = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    auto dest = bytes + (x * width + y) * 3;
    dest[0] = b;
    dest[1] = g;
    dest[2] = r;
  };
  auto angle = (double)i / n * M_PI;
  auto co = cos(angle);
  auto si = sin(angle);

  for (int i = 0; i < height; i++) {
    for (int j = 0; j < width; j++) {
      double sx = i * 20.0 / HEIGHT;
      double sy = j * 20.0 / HEIGHT;
      int x = int(floor(sx * co - sy * si));
      int y = int(floor(sx * si + sy * co));
      std::uint8_t color = ((y & 1) ^ (x & 1)) * 255;
      set_rgb(i, j, color, color, color);
    }
  }

  rtc::scoped_refptr<webrtc::I420Buffer> buffer = webrtc::I420Buffer::Create(width, height);

  libyuv::RGB24ToI420(bytes, width * 3, buffer->MutableDataY(), buffer->StrideY(), buffer->MutableDataU(),
                      buffer->StrideU(), buffer->MutableDataV(), buffer->StrideV(), width, height);

  return webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).build();
}

class FakeVideoSource : public rtc::VideoSourceInterface<webrtc::VideoFrame> {
 public:
  FakeVideoSource() {
    data_ = std::make_shared<Data>();
    std::thread([data = data_] {
      int N = 30;
      std::vector<webrtc::VideoFrame> frames;
      frames.reserve(N);
      for (int i = 0; i < N; i++) {
        frames.push_back(genFrame(i, N));
      }

      std::uint32_t step = 0;
      while (!data->flag_) {
        step++;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        auto &frame = frames[step % N];
        frame.set_id(static_cast<std::uint16_t>(step));
        frame.set_timestamp_us(rtc::TimeMicros());
        data->broadcaster_.OnFrame(frame);
      }
    }).detach();
  }
  ~FakeVideoSource() {
    data_->flag_ = true;
  }
  using VideoFrameT = webrtc::VideoFrame;
  void AddOrUpdateSink(rtc::VideoSinkInterface<VideoFrameT> *sink, const rtc::VideoSinkWants &wants) override {
    RTC_LOG(WARNING) << "ADD";
    data_->broadcaster_.AddOrUpdateSink(sink, wants);
  }
  // RemoveSink must guarantee that at the time the method returns,
  // there is no current and no future calls to VideoSinkInterface::OnFrame.
  void RemoveSink(rtc::VideoSinkInterface<VideoFrameT> *sink) {
    RTC_LOG(WARNING) << "REMOVE";
    data_->broadcaster_.RemoveSink(sink);
  }

 private:
  struct Data {
    std::atomic<bool> flag_;
    rtc::VideoBroadcaster broadcaster_;
  };
  std::shared_ptr<Data> data_;
};

class FakeVideoTrackSourceImpl : public webrtc::VideoTrackSource {
 public:
  static rtc::scoped_refptr<FakeVideoTrackSourceImpl> Create() {
    return rtc::scoped_refptr<FakeVideoTrackSourceImpl>(new rtc::RefCountedObject<FakeVideoTrackSourceImpl>());
  }

  FakeVideoTrackSourceImpl() : VideoTrackSource(false) {
  }

 protected:
  FakeVideoSource source_;
  rtc::VideoSourceInterface<webrtc::VideoFrame> *source() override {
    return &source_;
  }
};

std::function<webrtc::VideoTrackSourceInterface*()> FakeVideoTrackSource::create() {
  auto source = FakeVideoTrackSourceImpl::Create();
  return [source] {
    return source.get();
  };
}
}
