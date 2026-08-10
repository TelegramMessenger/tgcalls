#include "tgcall_video_capturer.h"

#include "tgcall_logging.h"
#include "tgcalls/StaticThreads.h"

#include <hilog/log.h>

#include <mutex>
#include <unordered_map>

namespace {
std::mutex g_capturersMutex;
std::unordered_map<int32_t, std::shared_ptr<tgcalls::VideoCaptureInterface>> g_capturers;
int32_t g_nextCapturerId = 1;
}  // namespace

namespace tgcall {

int32_t CreateVideoCapturer(int type, const std::string &deviceId) {
  const bool isScreenCapture = (type == 1);
  std::shared_ptr<tgcalls::VideoCaptureInterface> capturer =
      tgcalls::VideoCaptureInterface::Create(
          tgcalls::StaticThreads::getThreads(),
          deviceId,
          isScreenCapture,
          nullptr);
  if (!capturer) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(g_capturersMutex);
  const int32_t capturerId = g_nextCapturerId++;
  if (g_nextCapturerId <= 0) {
    g_nextCapturerId = 1;
  }
  g_capturers.emplace(capturerId, std::move(capturer));
  OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
               "TgCall video capturer created capturerId=%{public}d type=%{public}d",
               capturerId,
               type);
  return capturerId;
}

bool SetVideoStateCapturer(int32_t capturerId, int videoState) {
  std::shared_ptr<tgcalls::VideoCaptureInterface> capturer = GetVideoCapturer(capturerId);
  if (!capturer) {
    return false;
  }
  if (videoState >= 0 && videoState <= 2) {
    capturer->setState(static_cast<tgcalls::VideoState>(videoState));
  }
  return true;
}

bool SwitchCameraCapturer(int32_t capturerId, bool front) {
  std::shared_ptr<tgcalls::VideoCaptureInterface> capturer = GetVideoCapturer(capturerId);
  if (!capturer) {
    return false;
  }
  // front=true → 前置摄像头（"Front"）；front=false → 后置摄像头（"Back"）。
  // VideoCameraCapturer 将 "Back"/"environment" 映射为 SwitchCamera("environment")",
  // 其余 deviceId（含 "Front"/"user"/""）保持前置。
  capturer->switchToDevice(front ? "Front" : "Back", false);
  OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
               "TgCall video capturer switchCamera capturerId=%{public}d front=%{public}d",
               capturerId,
               front ? 1 : 0);
  return true;
}

void DestroyVideoCapturer(int32_t capturerId) {
  {
    std::lock_guard<std::mutex> lock(g_capturersMutex);
    g_capturers.erase(capturerId);
  }
  OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
               "TgCall video capturer destroyed capturerId=%{public}d",
               capturerId);
}

std::shared_ptr<tgcalls::VideoCaptureInterface> GetVideoCapturer(int32_t capturerId) {
  std::lock_guard<std::mutex> lock(g_capturersMutex);
  const auto it = g_capturers.find(capturerId);
  return it == g_capturers.end() ? nullptr : it->second;
}

}  // namespace tgcall
