#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "AudioFrame.h"
#include "api/scoped_refptr.h"

namespace webrtc {
class AudioDeviceModule;
class TaskQueueFactory;
}  // namespace webrtc

namespace tgcalls {
class FakeAudioDeviceModule {
 public:
  class Renderer {
   public:
    virtual ~Renderer() = default;
    virtual bool Render(const AudioFrame &samples) = 0;
    virtual void BeginFrame(double timestamp) {
    }
    virtual void AddFrameChannel(uint32_t ssrc, const tgcalls::AudioFrame &frame) {
    }
    virtual void EndFrame() {
    }
    virtual int32_t WaitForUs() {
       return 10000;
    }
  };
  class Recorder {
  public:
    virtual ~Recorder() = default;
    virtual AudioFrame Record() = 0;
    virtual int32_t WaitForUs() {
      return 10000;
    }
  };
  using Task = std::function<double()>;
  struct Options {
    uint32_t samples_per_sec{48000};
    uint32_t num_channels{2};
    std::function<void(Task)> scheduler_;
    // Report echo cancellation, AGC and noise suppression as built into the device, so WebRTC's voice engine keeps its
    // software ones off (WebRtcVoiceEngine::ApplyOptions), as a phone's audio unit does. Off: WebRTC's defaults.
    bool reports_builtin_processing{false};
  };
  static std::function<webrtc::scoped_refptr<webrtc::AudioDeviceModule>(webrtc::TaskQueueFactory *)> Creator(
      std::shared_ptr<Renderer> renderer,
      std::shared_ptr<Recorder> recorder,
      Options options);
};
}  // namespace tgcalls
