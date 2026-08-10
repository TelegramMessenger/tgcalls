#pragma once

#include "tgcalls/Instance.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tgcall {

constexpr size_t kTgCallEncryptionKeySize = 256;

struct TgCallSession {
  int32_t instanceId = 0;
  std::string protocolVersion;
  std::string serversJson;
  std::string encryptionKeyBase64;
  bool isOutgoing = false;
  bool allowP2p = false;
  std::string customParameters;
  // Descriptor 全量输入（NativeInstance.java 对齐）
  std::string configJson;                // tgcalls::Config 全字段 JSON（可空串 = 默认）
  std::string proxyJson;                 // tgcalls::Proxy JSON（可空串 = 无代理）
  std::string persistentStateBase64;     // tgcalls::PersistentState（可空）
  int initialNetworkType = 6;            // tgcalls::NetworkType 枚举值（6 = WiFi）
  std::string mediaDevicesConfigJson;    // 音频输入/输出设备与音量（可空串 = 默认）
  size_t signalingPacketCount = 0;
  std::vector<std::string> emittedSignalingDataBase64;
  std::shared_ptr<tgcalls::Instance> instance;
  // 关联的视频采集器句柄（setVideoCapture 时设置，供 switchCamera/setVideoState 使用）。
  int32_t videoCapturerId = 0;
  // ADR-0008: the emitted-signaling thread-safe function (native -> ArkTS push) is
  // intentionally NOT stored here. Its lifetime is owned by the signalingDataEmitted
  // lambda (see EmittedSignalingNotifier in tgcall_native.cpp), so it can never be
  // released while the networking thread is mid-call into it.
};

using TgCallSignalingDataCallback = std::function<void(const std::vector<uint8_t> &data)>;
using TgCallAudioLevelCallback = std::function<void(float level)>;
using TgCallStateCallback = std::function<void(tgcalls::State state)>;
using TgCallRemoteMediaStateCallback = std::function<void(tgcalls::AudioState audioState, tgcalls::VideoState videoState)>;
using TgCallRemoteBatteryCallback = std::function<void(bool isLow)>;
using TgCallRemoteAspectRatioCallback = std::function<void(float ratio)>;
using TgCallSignalBarsCallback = std::function<void(int signalBars)>;

std::shared_ptr<const std::array<uint8_t, kTgCallEncryptionKeySize>> DecodeEncryptionKeyBase64(
    const std::string &keyBase64);

// 解析 TgCallSession 的 JSON 字段（configJson / proxyJson / persistentStateBase64 /
// mediaDevicesConfigJson / initialNetworkType）并构造完整 Descriptor。
tgcalls::Descriptor BuildDescriptor(
    const TgCallSession &session,
    std::shared_ptr<const std::array<uint8_t, kTgCallEncryptionKeySize>> encryptionKey,
    TgCallSignalingDataCallback signalingDataCallback,
    TgCallAudioLevelCallback audioLevelCallback,
    TgCallStateCallback stateCallback,
    TgCallRemoteMediaStateCallback remoteMediaStateCallback,
    TgCallRemoteBatteryCallback remoteBatteryCallback,
    TgCallRemoteAspectRatioCallback remoteAspectRatioCallback,
    TgCallSignalBarsCallback signalBarsCallback);

// 将 tgcalls::State / AudioState / VideoState 转 int，供 ArkTS 回调使用。
int StateToInt(tgcalls::State state);
int AudioStateToInt(tgcalls::AudioState state);
int VideoStateToInt(tgcalls::VideoState state);

// 将 tgcalls::TrafficStats / FinalState 序列化为 JSON 字符串。
std::string TrafficStatsToJson(const tgcalls::TrafficStats &stats);
std::string FinalStateToJson(const tgcalls::FinalState &finalState);

}  // namespace tgcall
