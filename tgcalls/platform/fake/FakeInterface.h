#pragma once

#include "platform/PlatformInterface.h"
#include "VideoCapturerInterface.h"

namespace tgcalls {

// Test-only knob. By default the fake platform's video factories advertise
// their formats in the order the iOS platform factories do (see
// FakeInterface.cpp for why that matters); pass `true` to expose the raw
// builtin-factory order instead.
void setFakePlatformBuiltinCodecOrder(bool useBuiltinOrder);

class FakeInterface : public PlatformInterface {
 public:
  std::unique_ptr<webrtc::VideoEncoderFactory> makeVideoEncoderFactory(bool preferHardwareEncoding, bool isScreencast) override;
  std::unique_ptr<webrtc::VideoDecoderFactory> makeVideoDecoderFactory() override;
#if TGCALLS_ENABLE_AV1
  std::unique_ptr<webrtc::VideoEncoderFactory> makeVideoEncoderFactoryWithAv1(bool preferHardwareEncoding, bool allowAv1Encoding) override;
  std::unique_ptr<webrtc::VideoDecoderFactory> makeVideoDecoderFactoryWithAv1() override;
  // The CLI decides "not capable" per call with the video_av1_test_device_incapable knob.
  bool canEncodeAv1Realtime() override { return true; }
#endif
  bool supportsEncoding(const std::string &codecName) override;
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> makeVideoSource(rtc::Thread *signalingThread,
                                                                        rtc::Thread *workerThread) override;
  void adaptVideoSource(webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> videoSource, int width, int height,
                        int fps) override;
  std::unique_ptr<VideoCapturerInterface> makeVideoCapturer(
      webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source, std::string deviceId,
      std::function<void(VideoState)> stateUpdated, std::function<void(PlatformCaptureInfo)> captureInfoUpdated,
      std::shared_ptr<PlatformContext> platformContext, std::pair<int, int> &outResolution) override;
};

}  // namespace tgcalls

