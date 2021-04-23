#pragma once

#include <functional>

namespace webrtc {
class VideoTrackSourceInterface;
}

namespace tgcalls {
class FakeVideoTrackSource {
 public:
  static std::function<webrtc::VideoTrackSourceInterface*()> create();
};
}