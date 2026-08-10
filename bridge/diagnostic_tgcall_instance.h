#pragma once

#include "tgcalls/Instance.h"
#include "tgcalls/ThreadLocalObject.h"
#include "tgcalls/v2/InstanceNetworking.h"
#include "tgcalls/v2_4_0_0/Signaling_4_0_0.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace webrtc {
class Candidate;
}

namespace tgcall {

class DiagnosticTgCallInstance final : public tgcalls::Instance {
public:
  explicit DiagnosticTgCallInstance(tgcalls::Descriptor &&descriptor);
  ~DiagnosticTgCallInstance() override = default;

  void setNetworkType(tgcalls::NetworkType networkType) override;
  void setMuteMicrophone(bool muteMicrophone) override;
  void setAudioOutputGainControlEnabled(bool enabled) override;
  void setEchoCancellationStrength(int strength) override;
  bool supportsVideo() override;
  void setIncomingVideoOutput(std::weak_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> sink) override;
  void setAudioInputDevice(std::string id) override;
  void setAudioOutputDevice(std::string id) override;
  void setInputVolume(float level) override;
  void setOutputVolume(float level) override;
  void setAudioOutputDuckingEnabled(bool enabled) override;
  void setIsLowBatteryLevel(bool isLowBatteryLevel) override;
  std::string getLastError() override;
  std::string getDebugInfo() override;
  int64_t getPreferredRelayId() override;
  tgcalls::TrafficStats getTrafficStats() override;
  tgcalls::PersistentState getPersistentState() override;
  void receiveSignalingData(const std::vector<uint8_t> &data) override;
  void setVideoCapture(std::shared_ptr<tgcalls::VideoCaptureInterface> videoCapture) override;
  void sendVideoDeviceUpdated() override;
  void setRequestedVideoAspect(float aspect) override;
  void stop(std::function<void(tgcalls::FinalState)> completion) override;

private:
  void configureRemoteInitialSetup(
      const tgcalls::signaling_4_0_0::InitialSetupMessage &remoteInitialSetup);
  void addRemoteCandidates(
      const tgcalls::signaling_4_0_0::CandidatesMessage &remoteCandidates);
  void emitLocalInitialSetup(
      const tgcalls::signaling_4_0_0::InitialSetupMessage &remoteInitialSetup);
  void emitLocalCandidate(const webrtc::Candidate &candidate);
  void emitMediaState();
  void onTransportMessageReceived(const webrtc::CopyOnWriteBuffer &packet, bool isUnresolved);
  void onRtcpPacketReceived(const webrtc::CopyOnWriteBuffer &packet, int64_t packetTimeUs);
  void onDataChannelStateUpdated(bool isOpen);
  void onDataChannelMessageReceived(const std::string &message);
  bool encryptWebKMessage(
      const tgcalls::signaling_4_0_0::Message &message,
      uint32_t &sequence,
      std::vector<uint8_t> &encryptedPayload);

  std::string version_;
  tgcalls::Config config_;
  std::vector<tgcalls::RtcServer> rtcServers_;
  absl::optional<tgcalls::Proxy> proxy_;
  std::shared_ptr<tgcalls::Threads> threads_;
  std::shared_ptr<tgcalls::ThreadLocalObject<tgcalls::InstanceNetworking>> networking_;
  std::shared_ptr<const std::array<uint8_t, tgcalls::EncryptionKey::kSize>> encryptionKey_;
  bool encryptionKeyIsOutgoing_ = false;
  std::mutex webKSignalingMutex_;
  uint32_t webKOutgoingSequence_ = 1;
  bool webKNetworkingStarted_ = false;
  bool webKInitialSetupEmitScheduled_ = false;
  bool webKInitialSetupEmitted_ = false;
  bool webKMediaStateEmitted_ = false;
  std::function<void(tgcalls::State)> stateUpdated_;
  std::function<void(const std::vector<uint8_t> &)> signalingDataEmitted_;
  tgcalls::NetworkType networkType_ = tgcalls::NetworkType::Unknown;
  bool muteMicrophone_ = false;
  bool audioOutputGainControlEnabled_ = false;
  int echoCancellationStrength_ = 0;
  std::weak_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> incomingVideoOutput_;
  std::string audioInputDeviceId_;
  std::string audioOutputDeviceId_;
  float inputVolume_ = 1.0f;
  float outputVolume_ = 1.0f;
  bool audioOutputDuckingEnabled_ = false;
  bool isLowBatteryLevel_ = false;
  std::shared_ptr<tgcalls::VideoCaptureInterface> videoCapture_;
  float requestedVideoAspect_ = 0.0f;
  size_t signalingPacketCount_ = 0;
  size_t signalingByteCount_ = 0;
  std::mutex mediaDiagnosticsMutex_;
  size_t incomingRtpPacketCount_ = 0;
  size_t incomingRtpByteCount_ = 0;
  size_t unresolvedRtpPacketCount_ = 0;
  size_t incomingRtcpPacketCount_ = 0;
  size_t incomingRtcpByteCount_ = 0;
  size_t dataChannelMessageCount_ = 0;
  size_t dataChannelMessageByteCount_ = 0;
  bool dataChannelOpen_ = false;
};

}  // namespace tgcall
