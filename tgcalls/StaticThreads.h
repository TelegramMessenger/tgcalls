#pragma once

#include <cstddef>
#include <memory>

namespace webrtc {
class Thread;
}

namespace tgcalls {

class Threads {
public:
  virtual ~Threads() = default;
  virtual webrtc::Thread *getNetworkThread() = 0;
  virtual webrtc::Thread *getMediaThread() = 0;
  virtual webrtc::Thread *getWorkerThread() = 0;

  // it is not possible to decrease pool size
  static void setPoolSize(size_t size);
  static std::shared_ptr<Threads> getThreads();
};

namespace StaticThreads {
webrtc::Thread *getNetworkThread();
webrtc::Thread *getMediaThread();
webrtc::Thread *getWorkerThread();
std::shared_ptr<Threads> &getThreads();
}

}
