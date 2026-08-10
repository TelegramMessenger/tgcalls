#pragma once

#include "tgcalls/VideoCaptureInterface.h"

#include <cstdint>
#include <memory>
#include <string>

namespace tgcall {

// 视频采集器句柄管理（VideoCaptureInterface 包装，对齐 NativeInstance.java 的
// createVideoCapturer/setVideoStateCapturer/switchCameraCapturer/destroyVideoCapturer）。
//
// 底层为 OhosInterface 的相机采集（CameraCapturer + OhosVideoTrackSource），
// deviceId 支持 "Front"/"Back"/"user"/"environment"；setState/switchToDevice
// /setPreferredAspectRatio 均已可用。
int32_t CreateVideoCapturer(int type, const std::string &deviceId);
bool SetVideoStateCapturer(int32_t capturerId, int videoState);
bool SwitchCameraCapturer(int32_t capturerId, bool front);
void DestroyVideoCapturer(int32_t capturerId);
std::shared_ptr<tgcalls::VideoCaptureInterface> GetVideoCapturer(int32_t capturerId);

}  // namespace tgcall
