#include "diagnostic_tgcall_instance.h"

#include "tgcall_logging.h"
#include "webk_signaling_adapter.h"
#include "tgcalls/CryptoHelper.h"
#include "tgcalls/StaticThreads.h"
#include "tgcalls/v2/NativeNetworkingImpl.h"
#include "tgcalls/v2_4_0_0/Signaling_4_0_0.h"

#include <hilog/log.h>

#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

#include "api/candidate.h"
#include "api/jsep_ice_candidate.h"
#include "p2p/base/p2p_constants.h"
#include "rtc_base/crypto_random.h"
#include "rtc_base/ip_address.h"
#include "rtc_base/ssl_fingerprint.h"

namespace {
constexpr size_t kMessageKeySize = 16;
constexpr size_t kSequenceSize = 4;

bool ShouldLogMediaPacketCount(size_t count) {
  return count <= 5 || count == 10 || count == 25 || (count % 50) == 0;
}

void WriteBigEndian32(std::vector<uint8_t> &buffer, uint32_t value) {
  buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  buffer.push_back(static_cast<uint8_t>(value & 0xff));
}

uint32_t ReadBigEndian32(const uint8_t *buffer) {
  return (static_cast<uint32_t>(buffer[0]) << 24) |
         (static_cast<uint32_t>(buffer[1]) << 16) |
         (static_cast<uint32_t>(buffer[2]) << 8) |
         static_cast<uint32_t>(buffer[3]);
}

bool ConstTimeDifferent(const uint8_t *lhs, const uint8_t *rhs, size_t size) {
  uint8_t difference = 0;
  for (size_t i = 0; i < size; ++i) {
    difference = static_cast<uint8_t>(difference | (lhs[i] ^ rhs[i]));
  }
  return difference != 0;
}

std::vector<uint8_t> EncryptRawSignalingPacket(
    const std::vector<uint8_t> &payload,
    const std::array<uint8_t, tgcalls::EncryptionKey::kSize> &key,
    bool keyIsOutgoing,
    uint32_t sequence) {
  std::vector<uint8_t> prepared;
  prepared.reserve(kSequenceSize + payload.size());
  WriteBigEndian32(prepared, sequence);
  prepared.insert(prepared.end(), payload.begin(), payload.end());

  const int x = (keyIsOutgoing ? 0 : 8) + 128;
  const std::array<uint8_t, tgcalls::kSha256Size> messageKeyLarge = tgcalls::ConcatSHA256(
      tgcalls::MemorySpan{key.data() + 88 + x, 32},
      tgcalls::MemorySpan{prepared.data(), prepared.size()});

  std::array<uint8_t, kMessageKeySize> messageKey = {};
  std::memcpy(messageKey.data(), messageKeyLarge.data() + 8, messageKey.size());

  std::vector<uint8_t> result(kMessageKeySize + prepared.size());
  std::memcpy(result.data(), messageKey.data(), messageKey.size());
  tgcalls::AesKeyIv keyIv = tgcalls::PrepareAesKeyIv(key.data(), messageKey.data(), x);
  tgcalls::AesProcessCtr(
      tgcalls::MemorySpan{prepared.data(), prepared.size()},
      result.data() + kMessageKeySize,
      std::move(keyIv));
  return result;
}

bool DecryptRawSignalingPacket(
    const std::vector<uint8_t> &encrypted,
    const std::array<uint8_t, tgcalls::EncryptionKey::kSize> &key,
    bool keyIsOutgoing,
    std::vector<uint8_t> &payload,
    uint32_t &sequence) {
  if (encrypted.size() < kMessageKeySize + kSequenceSize) {
    return false;
  }

  const int x = (keyIsOutgoing ? 8 : 0) + 128;
  const uint8_t *messageKey = encrypted.data();
  const uint8_t *encryptedData = encrypted.data() + kMessageKeySize;
  const size_t encryptedSize = encrypted.size() - kMessageKeySize;

  std::vector<uint8_t> decrypted(encryptedSize);
  tgcalls::AesKeyIv keyIv = tgcalls::PrepareAesKeyIv(key.data(), messageKey, x);
  tgcalls::AesProcessCtr(
      tgcalls::MemorySpan{encryptedData, encryptedSize},
      decrypted.data(),
      std::move(keyIv));

  const std::array<uint8_t, tgcalls::kSha256Size> messageKeyLarge = tgcalls::ConcatSHA256(
      tgcalls::MemorySpan{key.data() + 88 + x, 32},
      tgcalls::MemorySpan{decrypted.data(), decrypted.size()});
  if (ConstTimeDifferent(messageKeyLarge.data() + 8, messageKey, kMessageKeySize)) {
    return false;
  }

  sequence = ReadBigEndian32(decrypted.data());
  if (sequence == 0) {
    return false;
  }

  payload.assign(decrypted.begin() + kSequenceSize, decrypted.end());
  return true;
}

const char *WebKMessageTypeName(const tgcalls::signaling_4_0_0::Message &message) {
  if (absl::get_if<tgcalls::signaling_4_0_0::InitialSetupMessage>(&message.data) != nullptr) {
    return "InitialSetup";
  }
  if (absl::get_if<tgcalls::signaling_4_0_0::CandidatesMessage>(&message.data) != nullptr) {
    return "Candidates";
  }
  if (absl::get_if<tgcalls::signaling_4_0_0::MediaStateMessage>(&message.data) != nullptr) {
    return "MediaState";
  }
  return "Unknown";
}

void LogWebKMessageDetails(
    const tgcalls::signaling_4_0_0::Message &message,
    const std::string &version,
    size_t packetCount,
    uint32_t sequence,
    size_t encryptedSize,
    size_t decryptedSize) {
  const tgcalls::signaling_4_0_0::InitialSetupMessage *initialSetup =
      absl::get_if<tgcalls::signaling_4_0_0::InitialSetupMessage>(&message.data);
  if (initialSetup != nullptr) {
    const tgcall::WebKToV2InitialSetupMapping mappedInitialSetup =
        tgcall::MapWebKInitialSetupToV2(*initialSetup, 1);
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "Diagnostic Web-K signaling parsed version=%{public}s packet=%{public}zu seq=%{public}u type=%{public}s encrypted=%{public}zu decrypted=%{public}zu fingerprints=%{public}zu audio=%{public}d video=%{public}d screencast=%{public}d",
                 version.c_str(),
                 packetCount,
                 sequence,
                 WebKMessageTypeName(message),
                 encryptedSize,
                 decryptedSize,
                 initialSetup->fingerprints.size(),
                 initialSetup->audio.has_value() ? 1 : 0,
                 initialSetup->video.has_value() ? 1 : 0,
                 initialSetup->screencast.has_value() ? 1 : 0);
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "Diagnostic Web-K adapter mapped initial setup version=%{public}s exchangeId=%{public}u fingerprints=%{public}zu contents=%{public}zu audio=%{public}d video=%{public}d screencast=%{public}d",
                 version.c_str(),
                 mappedInitialSetup.negotiation.exchangeId,
                 mappedInitialSetup.initialSetup.fingerprints.size(),
                 mappedInitialSetup.negotiation.contents.size(),
                 mappedInitialSetup.hasAudio ? 1 : 0,
                 mappedInitialSetup.hasVideo ? 1 : 0,
                 mappedInitialSetup.hasScreencast ? 1 : 0);
    return;
  }

  const tgcalls::signaling_4_0_0::CandidatesMessage *candidates =
      absl::get_if<tgcalls::signaling_4_0_0::CandidatesMessage>(&message.data);
  if (candidates != nullptr) {
    const tgcalls::signaling::CandidatesMessage mappedCandidates =
        tgcall::MapWebKCandidatesToV2(*candidates);
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "Diagnostic Web-K signaling parsed version=%{public}s packet=%{public}zu seq=%{public}u type=%{public}s encrypted=%{public}zu decrypted=%{public}zu candidates=%{public}zu",
                 version.c_str(),
                 packetCount,
                 sequence,
                 WebKMessageTypeName(message),
                 encryptedSize,
                 decryptedSize,
                 candidates->iceCandidates.size());
    OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
                 "Diagnostic Web-K adapter mapped candidates version=%{public}s candidates=%{public}zu",
                 version.c_str(),
                 mappedCandidates.iceCandidates.size());
    return;
  }

  OH_LOG_Print(LOG_APP, LOG_INFO, tgcall::kTgCallLogDomain, tgcall::kTgCallLogTag,
               "Diagnostic Web-K signaling parsed version=%{public}s packet=%{public}zu seq=%{public}u type=%{public}s encrypted=%{public}zu decrypted=%{public}zu",
               version.c_str(),
               packetCount,
               sequence,
               WebKMessageTypeName(message),
               encryptedSize,
               decryptedSize);
}

tgcalls::signaling_4_0_0::MediaContent BuildDiagnosticLocalMediaContent(
    const tgcalls::signaling_4_0_0::MediaContent &remoteContent) {
  tgcalls::signaling_4_0_0::MediaContent localContent;
  std::map<uint32_t, uint32_t> ssrcMap;

  const auto mapSsrc = [&ssrcMap](uint32_t remoteSsrc) {
    const auto existing = ssrcMap.find(remoteSsrc);
    if (existing != ssrcMap.end()) {
      return existing->second;
    }
    uint32_t localSsrc = webrtc::CreateRandomNonZeroId();
    while (localSsrc == 0) {
      localSsrc = webrtc::CreateRandomNonZeroId();
    }
    ssrcMap[remoteSsrc] = localSsrc;
    return localSsrc;
  };

  if (remoteContent.ssrc != 0) {
    localContent.ssrc = mapSsrc(remoteContent.ssrc);
  }

  localContent.ssrcGroups.reserve(remoteContent.ssrcGroups.size());
  for (const tgcalls::signaling_4_0_0::SsrcGroup &remoteGroup : remoteContent.ssrcGroups) {
    tgcalls::signaling_4_0_0::SsrcGroup localGroup;
    localGroup.semantics = remoteGroup.semantics;
    localGroup.ssrcs.reserve(remoteGroup.ssrcs.size());
    for (uint32_t remoteSsrc : remoteGroup.ssrcs) {
      const uint32_t localSsrc = mapSsrc(remoteSsrc);
      localGroup.ssrcs.push_back(localSsrc);
      if (localContent.ssrc == 0) {
        localContent.ssrc = localSsrc;
      }
    }
    localContent.ssrcGroups.push_back(std::move(localGroup));
  }

  if (localContent.ssrc == 0) {
    localContent.ssrc = webrtc::CreateRandomNonZeroId();
  }

  localContent.payloadTypes = remoteContent.payloadTypes;
  localContent.rtpExtensions = remoteContent.rtpExtensions;
  return localContent;
}

bool BuildDiagnosticLocalInitialSetup(
    const tgcalls::signaling_4_0_0::InitialSetupMessage &remoteInitialSetup,
    const tgcalls::PeerIceParameters &localIceParameters,
    const webrtc::SSLFingerprint &localFingerprint,
    bool isOutgoing,
    tgcalls::signaling_4_0_0::InitialSetupMessage &localInitialSetup) {
  if (!remoteInitialSetup.audio.has_value()) {
    return false;
  }

  tgcalls::signaling_4_0_0::DtlsFingerprint fingerprint;
  fingerprint.hash = localFingerprint.algorithm;
  // Web-K 4.0.0 only completes ICE nomination when the callee answers with setup=active;
  // emitting setup=passive caused Telegram Web to stop nominating any candidate pair.
  // The matching SSL role override lives in configureRemoteInitialSetup so tgcalls picks
  // SSL_CLIENT and actually starts the DTLS handshake.
  fingerprint.setup = isOutgoing ? "actpass" : "active";
  fingerprint.fingerprint = localFingerprint.GetRfc4572Fingerprint();

  localInitialSetup.ufrag = localIceParameters.ufrag;
  localInitialSetup.pwd = localIceParameters.pwd;
  localInitialSetup.fingerprints.push_back(std::move(fingerprint));
  localInitialSetup.audio = BuildDiagnosticLocalMediaContent(remoteInitialSetup.audio.value());
  // Web-K rejects an answer that drops offered media sections before it reaches setRemoteDescription.
  if (remoteInitialSetup.video.has_value()) {
    localInitialSetup.video = BuildDiagnosticLocalMediaContent(remoteInitialSetup.video.value());
  }
  if (remoteInitialSetup.screencast.has_value()) {
    localInitialSetup.screencast = BuildDiagnosticLocalMediaContent(remoteInitialSetup.screencast.value());
  }
  return true;
}

bool EndsWithReflectorHostname(const std::string &hostname) {
  const std::string suffix = ".reflector";
  return hostname.size() >= suffix.size() &&
         hostname.compare(hostname.size() - suffix.size(), suffix.size(), suffix) == 0;
}

uint8_t ParseReflectorServerId(const std::string &hostname) {
  const std::string prefix = "reflector-";
  if (hostname.compare(0, prefix.size(), prefix) != 0) {
    return 0;
  }

  uint32_t value = 0;
  bool hasDigit = false;
  for (size_t index = prefix.size(); index < hostname.size(); index += 1) {
    const char character = hostname[index];
    if (character == '-') {
      break;
    }
    if (character < '0' || character > '9') {
      return 0;
    }
    hasDigit = true;
    value = value * 10 + static_cast<uint32_t>(character - '0');
    if (value > 255) {
      return 0;
    }
  }

  return hasDigit ? static_cast<uint8_t>(value) : 0;
}

bool RewriteReflectorCandidateAddressForWebK(
    webrtc::SocketAddress &address,
    const std::vector<tgcalls::RtcServer> &rtcServers) {
  const std::string hostname = address.hostname();
  if (!EndsWithReflectorHostname(hostname)) {
    return false;
  }

  const uint8_t serverId = ParseReflectorServerId(hostname);
  if (serverId == 0) {
    return false;
  }

  for (const tgcalls::RtcServer &server : rtcServers) {
    if (server.id != serverId || server.login != "reflector" || server.host.empty()) {
      continue;
    }

    webrtc::IPAddress ipAddress;
    if (!webrtc::IPFromString(server.host, &ipAddress)) {
      return false;
    }

    address.SetIP(ipAddress);
    return true;
  }

  return false;
}

bool IsReflectorCandidateAddress(const webrtc::SocketAddress &address) {
  return EndsWithReflectorHostname(address.hostname());
}

void ReplaceAll(std::string &value, const std::string &from, const std::string &to) {
  if (from.empty()) {
    return;
  }

  size_t position = 0;
  while ((position = value.find(from, position)) != std::string::npos) {
    value.replace(position, from.length(), to);
    position += to.length();
  }
}

void NormalizeWebKCandidateSdp(std::string &candidate) {
  ReplaceAll(candidate, " typ local ", " typ host ");
  ReplaceAll(candidate, " typ stun ", " typ srflx ");
}
}  // namespace

namespace tgcall {

DiagnosticTgCallInstance::DiagnosticTgCallInstance(tgcalls::Descriptor &&descriptor)
    : version_(std::move(descriptor.version)),
      config_(descriptor.config),
      rtcServers_(std::move(descriptor.rtcServers)),
      encryptionKey_(descriptor.encryptionKey.value),
      encryptionKeyIsOutgoing_(descriptor.encryptionKey.isOutgoing),
      stateUpdated_(std::move(descriptor.stateUpdated)),
      signalingDataEmitted_(std::move(descriptor.signalingDataEmitted)) {
  if (descriptor.proxy) {
    proxy_ = *(descriptor.proxy.get());
  }
  OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
               "Diagnostic tgcalls::Instance created version=%{public}s rtcServers=%{public}zu keyOutgoing=%{public}d",
               version_.c_str(),
               rtcServers_.size(),
               encryptionKeyIsOutgoing_ ? 1 : 0);
  if (version_ == "4.0.0" && encryptionKey_) {
    const bool diagnosticEnableP2P = config_.enableP2P || version_ == "4.0.0";
    OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                 "Diagnostic Web-K networking config p2p=%{public}d tcp=%{public}d stunMarking=%{public}d",
                 diagnosticEnableP2P ? 1 : 0,
                 config_.allowTCP ? 1 : 0,
                 config_.enableStunMarking ? 1 : 0);
    threads_ = tgcalls::Threads::getThreads();
    const tgcalls::EncryptionKey networkingKey(encryptionKey_, encryptionKeyIsOutgoing_);
    networking_ = std::make_shared<tgcalls::ThreadLocalObject<tgcalls::InstanceNetworking>>(
        threads_->getNetworkThread(),
        [this,
         threads = threads_,
         networkingKey,
         rtcServers = rtcServers_,
         proxy = proxy_,
         enableStunMarking = config_.enableStunMarking,
         enableTcp = config_.allowTCP,
         enableP2P = diagnosticEnableP2P]() {
          return std::static_pointer_cast<tgcalls::InstanceNetworking>(
              std::make_shared<tgcalls::NativeNetworkingImpl>(tgcalls::InstanceNetworking::Configuration{
                  .encryptionKey = networkingKey,
                  .isOutgoing = networkingKey.isOutgoing,
                  .enableStunMarking = enableStunMarking,
                  .enableTCP = enableTcp,
                  .enableP2P = enableP2P,
                  .rtcServers = rtcServers,
                  .proxy = proxy,
                  .stateUpdated = [this](const tgcalls::InstanceNetworking::State &state) {
                    OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                                 "Diagnostic networking state version=%{public}s ready=%{public}d failed=%{public}d route=%{public}d connection=%{public}d",
                                 version_.c_str(),
                                 state.isReadyToSendData ? 1 : 0,
                                 state.isFailed ? 1 : 0,
                                 state.route.has_value() ? 1 : 0,
                                 state.connection.has_value() ? 1 : 0);
                    if (state.connection.has_value()) {
                      OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                                   "Diagnostic networking connection local=%{public}s/%{public}s/%{public}s remote=%{public}s/%{public}s/%{public}s",
                                   state.connection->local.protocol.c_str(),
                                   state.connection->local.type.c_str(),
                                   state.connection->local.address.c_str(),
                                   state.connection->remote.protocol.c_str(),
                                   state.connection->remote.type.c_str(),
                                   state.connection->remote.address.c_str());
                    }
                  },
                  .candidateGathered = [this](const webrtc::Candidate &candidate) {
                    emitLocalCandidate(candidate);
                  },
                  .transportMessageReceived = [this](webrtc::CopyOnWriteBuffer const &packet, bool isUnresolved) {
                    onTransportMessageReceived(packet, isUnresolved);
                  },
                  .rtcpPacketReceived = [this](webrtc::CopyOnWriteBuffer const &packet, int64_t packetTimeUs) {
                    onRtcpPacketReceived(packet, packetTimeUs);
                  },
                  .dataChannelStateUpdated = [this](bool isOpen) {
                    onDataChannelStateUpdated(isOpen);
                  },
                  .dataChannelMessageReceived = [this](std::string const &message) {
                    onDataChannelMessageReceived(message);
                  },
                  .threads = threads,
                  .customParameters = std::map<std::string, json11::Json>()}));
        });
  }
  // Diagnostic shortcut only; real tgcalls implementations emit states from their own lifecycle.
  if (stateUpdated_) {
    stateUpdated_(tgcalls::State::WaitInit);
    stateUpdated_(tgcalls::State::Established);
  }
}

bool DiagnosticTgCallInstance::encryptWebKMessage(
    const tgcalls::signaling_4_0_0::Message &message,
    uint32_t &sequence,
    std::vector<uint8_t> &encryptedPayload) {
  if (!encryptionKey_) {
    return false;
  }

  const std::vector<uint8_t> serializedPayload = message.serialize();
  {
    std::lock_guard<std::mutex> lock(webKSignalingMutex_);
    sequence = webKOutgoingSequence_;
    webKOutgoingSequence_ += 1;
    encryptedPayload = EncryptRawSignalingPacket(
        serializedPayload,
        *encryptionKey_,
        encryptionKeyIsOutgoing_,
        sequence);
  }
  return true;
}

void DiagnosticTgCallInstance::configureRemoteInitialSetup(
    const tgcalls::signaling_4_0_0::InitialSetupMessage &remoteInitialSetup) {
  if (!networking_) {
    return;
  }

  OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
               "Diagnostic Web-K configuring remote setup version=%{public}s ufragLen=%{public}zu pwdLen=%{public}zu fingerprints=%{public}zu",
               version_.c_str(),
               remoteInitialSetup.ufrag.size(),
               remoteInitialSetup.pwd.size(),
               remoteInitialSetup.fingerprints.size());
  const bool isCallee = !encryptionKeyIsOutgoing_;
  networking_->perform([remoteInitialSetup, isCallee](tgcalls::InstanceNetworking *networking) {
    tgcalls::PeerIceParameters remoteIce(
        remoteInitialSetup.ufrag,
        remoteInitialSetup.pwd,
        false);

    std::unique_ptr<webrtc::SSLFingerprint> remoteFingerprint;
    std::string sslSetup;
    if (!remoteInitialSetup.fingerprints.empty()) {
      const tgcalls::signaling_4_0_0::DtlsFingerprint &fingerprint =
          remoteInitialSetup.fingerprints.front();
      remoteFingerprint = webrtc::SSLFingerprint::CreateUniqueFromRfc4572(
          fingerprint.hash,
          fingerprint.fingerprint);
      sslSetup = fingerprint.setup;
    }

    // tgcalls::NativeNetworkingImpl::setRemoteParams falls back to
    // (_isOutgoing ? SSL_CLIENT : SSL_SERVER) when the remote announces "actpass". That
    // would make us a DTLS server while we tell the peer setup=active, so both ends wait
    // for ClientHello and DTLS never starts (ready stays 0 after ICE connected). Override
    // the role here by claiming the remote is "passive" so tgcalls picks SSL_CLIENT and
    // actually sends ClientHello, consistent with the setup=active we emit upstream.
    if (isCallee && sslSetup == "actpass") {
      sslSetup = "passive";
    }

    networking->setRemoteParams(remoteIce, remoteFingerprint.get(), sslSetup);
  });
}

void DiagnosticTgCallInstance::addRemoteCandidates(
    const tgcalls::signaling_4_0_0::CandidatesMessage &remoteCandidates) {
  if (!networking_) {
    return;
  }

  networking_->perform([remoteCandidates](tgcalls::InstanceNetworking *networking) {
    std::vector<webrtc::Candidate> parsedCandidates;
    parsedCandidates.reserve(remoteCandidates.iceCandidates.size());
    for (const tgcalls::signaling_4_0_0::IceCandidate &candidate : remoteCandidates.iceCandidates) {
      std::unique_ptr<webrtc::IceCandidate> parsedCandidate =
          webrtc::IceCandidate::Create(std::string(), 0, candidate.sdpString, nullptr);
      if (!parsedCandidate) {
        OH_LOG_Print(LOG_APP, LOG_WARN, kTgCallLogDomain, kTgCallLogTag,
                     "Diagnostic Web-K remote candidate parse failed size=%{public}zu",
                     candidate.sdpString.size());
        continue;
      }
      const webrtc::Candidate &parsed = parsedCandidate->candidate();
      OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                   "Diagnostic Web-K remote candidate parsed protocol=%{public}s type=%{public}s relay=%{public}s address=%{public}s related=%{public}s bytes=%{public}zu",
                   parsed.protocol().c_str(),
                   std::string(webrtc::IceCandidateTypeToString(parsed.type())).c_str(),
                   parsed.relay_protocol().c_str(),
                   parsed.address().ToString().c_str(),
                   parsed.related_address().ToString().c_str(),
                   candidate.sdpString.size());
      parsedCandidates.push_back(parsed);
    }
    if (!parsedCandidates.empty()) {
      networking->addCandidates(parsedCandidates);
    }
  });
}

void DiagnosticTgCallInstance::emitLocalInitialSetup(
    const tgcalls::signaling_4_0_0::InitialSetupMessage &remoteInitialSetup) {
  if (!networking_ || !signalingDataEmitted_) {
    return;
  }

  networking_->perform([this, remoteInitialSetup](tgcalls::InstanceNetworking *networking) {
    tgcalls::signaling_4_0_0::InitialSetupMessage localInitialSetup;
    const tgcalls::PeerIceParameters localIce = networking->getLocalIceParameters();
    std::unique_ptr<webrtc::SSLFingerprint> localFingerprint = networking->getLocalFingerprint();
    if (!localFingerprint ||
        !BuildDiagnosticLocalInitialSetup(
            remoteInitialSetup,
            localIce,
            *localFingerprint,
            encryptionKeyIsOutgoing_,
            localInitialSetup)) {
      OH_LOG_Print(LOG_APP, LOG_WARN, kTgCallLogDomain, kTgCallLogTag,
                   "Diagnostic Web-K signaling initial setup emit skipped version=%{public}s reason=buildFailed remoteAudio=%{public}d",
                   version_.c_str(),
                   remoteInitialSetup.audio.has_value() ? 1 : 0);
      return;
    }

    tgcalls::signaling_4_0_0::Message outboundInitialSetup;
    outboundInitialSetup.data = localInitialSetup;
    uint32_t outboundSequence = 0;
    std::vector<uint8_t> encryptedPayload;
    if (!encryptWebKMessage(outboundInitialSetup, outboundSequence, encryptedPayload)) {
      return;
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                 "Diagnostic Web-K signaling emitting initial setup version=%{public}s seq=%{public}u payload=%{public}zu encrypted=%{public}zu setup=%{public}s audio=%{public}d video=%{public}d screencast=%{public}d fingerprints=%{public}zu",
                 version_.c_str(),
                 outboundSequence,
                 outboundInitialSetup.serialize().size(),
                 encryptedPayload.size(),
                 localInitialSetup.fingerprints.empty()
                     ? ""
                     : localInitialSetup.fingerprints.front().setup.c_str(),
                 localInitialSetup.audio.has_value() ? 1 : 0,
                 localInitialSetup.video.has_value() ? 1 : 0,
                 localInitialSetup.screencast.has_value() ? 1 : 0,
                 localInitialSetup.fingerprints.size());
    signalingDataEmitted_(encryptedPayload);
    {
      std::lock_guard<std::mutex> lock(webKSignalingMutex_);
      webKInitialSetupEmitted_ = true;
    }

    bool shouldStartNetworking = false;
    {
      std::lock_guard<std::mutex> lock(webKSignalingMutex_);
      if (!webKNetworkingStarted_) {
        webKNetworkingStarted_ = true;
        shouldStartNetworking = true;
      }
    }
    if (shouldStartNetworking) {
      OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                   "Diagnostic Web-K networking start version=%{public}s",
                   version_.c_str());
      networking->start();
    }
    emitMediaState();
  });
}

void DiagnosticTgCallInstance::emitLocalCandidate(const webrtc::Candidate &candidate) {
  if (!signalingDataEmitted_) {
    return;
  }

  webrtc::Candidate patchedCandidate = candidate;
  patchedCandidate.set_component(1);

  if (patchedCandidate.protocol() != webrtc::UDP_PROTOCOL_NAME) {
    OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                 "Diagnostic Web-K local candidate skipped version=%{public}s protocol=%{public}s type=%{public}s address=%{public}s",
                 version_.c_str(),
                 patchedCandidate.protocol().c_str(),
                 std::string(webrtc::IceCandidateTypeToString(patchedCandidate.type())).c_str(),
                 patchedCandidate.address().ToString().c_str());
    return;
  }

  webrtc::SocketAddress candidateAddress = patchedCandidate.address();
  const std::string originalCandidateAddress = candidateAddress.ToString();
  if (RewriteReflectorCandidateAddressForWebK(candidateAddress, rtcServers_)) {
    patchedCandidate.set_address(candidateAddress);
    OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                 "Diagnostic Web-K local reflector candidate rewritten version=%{public}s from=%{public}s to=%{public}s",
                 version_.c_str(),
                 originalCandidateAddress.c_str(),
                 candidateAddress.ToString().c_str());
  }
  webrtc::SocketAddress relatedCandidateAddress = patchedCandidate.related_address();
  const std::string originalRelatedCandidateAddress = relatedCandidateAddress.ToString();
  if (RewriteReflectorCandidateAddressForWebK(relatedCandidateAddress, rtcServers_)) {
    patchedCandidate.set_related_address(relatedCandidateAddress);
    OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                 "Diagnostic Web-K local reflector related candidate rewritten version=%{public}s from=%{public}s to=%{public}s",
                 version_.c_str(),
                 originalRelatedCandidateAddress.c_str(),
                 relatedCandidateAddress.ToString().c_str());
  }

  std::unique_ptr<webrtc::IceCandidate> iceCandidate =
      webrtc::CreateIceCandidate(std::string(), 0, patchedCandidate);
  if (!iceCandidate) {
    OH_LOG_Print(LOG_APP, LOG_WARN, kTgCallLogDomain, kTgCallLogTag,
                 "Diagnostic Web-K local candidate serialize failed");
    return;
  }
  std::string serializedCandidate = iceCandidate->ToString();
  if (serializedCandidate.empty()) {
    OH_LOG_Print(LOG_APP, LOG_WARN, kTgCallLogDomain, kTgCallLogTag,
                 "Diagnostic Web-K local candidate serialize failed");
    return;
  }
  NormalizeWebKCandidateSdp(serializedCandidate);

  tgcalls::signaling::CandidatesMessage v2Candidates;
  tgcalls::signaling::IceCandidate v2Candidate;
  v2Candidate.sdpString = serializedCandidate;
  v2Candidates.iceCandidates.push_back(std::move(v2Candidate));

  tgcalls::signaling_4_0_0::Message outboundMessage;
  outboundMessage.data = MapV2CandidatesToWebK(v2Candidates);
  uint32_t outboundSequence = 0;
  std::vector<uint8_t> encryptedPayload;
  if (!encryptWebKMessage(outboundMessage, outboundSequence, encryptedPayload)) {
    return;
  }

  OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
               "Diagnostic Web-K signaling emitting candidate version=%{public}s seq=%{public}u protocol=%{public}s type=%{public}s relay=%{public}s address=%{public}s related=%{public}s tcptype=%{public}s candidateBytes=%{public}zu encrypted=%{public}zu sdp=%{public}s",
               version_.c_str(),
               outboundSequence,
               patchedCandidate.protocol().c_str(),
               std::string(webrtc::IceCandidateTypeToString(patchedCandidate.type())).c_str(),
               patchedCandidate.relay_protocol().c_str(),
               patchedCandidate.address().ToString().c_str(),
               patchedCandidate.related_address().ToString().c_str(),
               patchedCandidate.tcptype().c_str(),
               serializedCandidate.size(),
               encryptedPayload.size(),
               serializedCandidate.c_str());
  signalingDataEmitted_(encryptedPayload);
}

void DiagnosticTgCallInstance::emitMediaState() {
  if (!signalingDataEmitted_) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(webKSignalingMutex_);
    if (webKMediaStateEmitted_) {
      return;
    }
    webKMediaStateEmitted_ = true;
  }

  tgcalls::signaling_4_0_0::MediaStateMessage mediaState;
  mediaState.isMuted = muteMicrophone_;
  mediaState.videoState = tgcalls::signaling_4_0_0::MediaStateMessage::VideoState::Inactive;
  mediaState.videoRotation = tgcalls::signaling_4_0_0::MediaStateMessage::VideoRotation::Rotation0;
  mediaState.screencastState = tgcalls::signaling_4_0_0::MediaStateMessage::VideoState::Inactive;
  mediaState.isBatteryLow = isLowBatteryLevel_;

  tgcalls::signaling_4_0_0::Message outboundMessage;
  outboundMessage.data = mediaState;
  uint32_t outboundSequence = 0;
  std::vector<uint8_t> encryptedPayload;
  if (!encryptWebKMessage(outboundMessage, outboundSequence, encryptedPayload)) {
    return;
  }

  OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
               "Diagnostic Web-K signaling emitting media state version=%{public}s seq=%{public}u encrypted=%{public}zu muted=%{public}d lowBattery=%{public}d",
               version_.c_str(),
               outboundSequence,
               encryptedPayload.size(),
               muteMicrophone_ ? 1 : 0,
               isLowBatteryLevel_ ? 1 : 0);
  signalingDataEmitted_(encryptedPayload);
}

void DiagnosticTgCallInstance::onTransportMessageReceived(const webrtc::CopyOnWriteBuffer &packet, bool isUnresolved) {
  size_t packetCount = 0;
  size_t byteCount = 0;
  size_t unresolvedCount = 0;
  {
    std::lock_guard<std::mutex> lock(mediaDiagnosticsMutex_);
    incomingRtpPacketCount_ += 1;
    incomingRtpByteCount_ += packet.size();
    if (isUnresolved) {
      unresolvedRtpPacketCount_ += 1;
    }
    packetCount = incomingRtpPacketCount_;
    byteCount = incomingRtpByteCount_;
    unresolvedCount = unresolvedRtpPacketCount_;
  }

  if (ShouldLogMediaPacketCount(packetCount)) {
    OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                 "Diagnostic media ingress RTP version=%{public}s packets=%{public}zu bytes=%{public}zu lastBytes=%{public}zu unresolved=%{public}zu",
                 version_.c_str(),
                 packetCount,
                 byteCount,
                 packet.size(),
                 unresolvedCount);
  }
}

void DiagnosticTgCallInstance::onRtcpPacketReceived(const webrtc::CopyOnWriteBuffer &packet, int64_t packetTimeUs) {
  size_t packetCount = 0;
  size_t byteCount = 0;
  {
    std::lock_guard<std::mutex> lock(mediaDiagnosticsMutex_);
    incomingRtcpPacketCount_ += 1;
    incomingRtcpByteCount_ += packet.size();
    packetCount = incomingRtcpPacketCount_;
    byteCount = incomingRtcpByteCount_;
  }

  if (ShouldLogMediaPacketCount(packetCount)) {
    OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                 "Diagnostic media ingress RTCP version=%{public}s packets=%{public}zu bytes=%{public}zu lastBytes=%{public}zu packetTimeUs=%{public}lld",
                 version_.c_str(),
                 packetCount,
                 byteCount,
                 packet.size(),
                 static_cast<long long>(packetTimeUs));
  }
}

void DiagnosticTgCallInstance::onDataChannelStateUpdated(bool isOpen) {
  {
    std::lock_guard<std::mutex> lock(mediaDiagnosticsMutex_);
    dataChannelOpen_ = isOpen;
  }

  OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
               "Diagnostic data channel state version=%{public}s open=%{public}d",
               version_.c_str(),
               isOpen ? 1 : 0);
}

void DiagnosticTgCallInstance::onDataChannelMessageReceived(const std::string &message) {
  size_t messageCount = 0;
  size_t byteCount = 0;
  {
    std::lock_guard<std::mutex> lock(mediaDiagnosticsMutex_);
    dataChannelMessageCount_ += 1;
    dataChannelMessageByteCount_ += message.size();
    messageCount = dataChannelMessageCount_;
    byteCount = dataChannelMessageByteCount_;
  }

  OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
               "Diagnostic data channel message version=%{public}s messages=%{public}zu bytes=%{public}zu lastBytes=%{public}zu",
               version_.c_str(),
               messageCount,
               byteCount,
               message.size());
}

void DiagnosticTgCallInstance::setNetworkType(tgcalls::NetworkType networkType) {
  networkType_ = networkType;
}

void DiagnosticTgCallInstance::setMuteMicrophone(bool muteMicrophone) {
  muteMicrophone_ = muteMicrophone;
}

void DiagnosticTgCallInstance::setAudioOutputGainControlEnabled(bool enabled) {
  audioOutputGainControlEnabled_ = enabled;
}

void DiagnosticTgCallInstance::setEchoCancellationStrength(int strength) {
  echoCancellationStrength_ = strength;
}

bool DiagnosticTgCallInstance::supportsVideo() {
  return false;
}

void DiagnosticTgCallInstance::setIncomingVideoOutput(
    std::weak_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
  incomingVideoOutput_ = std::move(sink);
}

void DiagnosticTgCallInstance::setAudioInputDevice(std::string id) {
  audioInputDeviceId_ = std::move(id);
}

void DiagnosticTgCallInstance::setAudioOutputDevice(std::string id) {
  audioOutputDeviceId_ = std::move(id);
}

void DiagnosticTgCallInstance::setInputVolume(float level) {
  inputVolume_ = level;
}

void DiagnosticTgCallInstance::setOutputVolume(float level) {
  outputVolume_ = level;
}

void DiagnosticTgCallInstance::setAudioOutputDuckingEnabled(bool enabled) {
  audioOutputDuckingEnabled_ = enabled;
}

void DiagnosticTgCallInstance::setIsLowBatteryLevel(bool isLowBatteryLevel) {
  isLowBatteryLevel_ = isLowBatteryLevel;
}

std::string DiagnosticTgCallInstance::getLastError() {
  return "";
}

std::string DiagnosticTgCallInstance::getDebugInfo() {
  return "diagnostic-instance version=" + version_;
}

int64_t DiagnosticTgCallInstance::getPreferredRelayId() {
  return 0;
}

tgcalls::TrafficStats DiagnosticTgCallInstance::getTrafficStats() {
  return tgcalls::TrafficStats();
}

tgcalls::PersistentState DiagnosticTgCallInstance::getPersistentState() {
  return tgcalls::PersistentState();
}

void DiagnosticTgCallInstance::receiveSignalingData(const std::vector<uint8_t> &data) {
  signalingPacketCount_ += 1;
  signalingByteCount_ += data.size();
  OH_LOG_Print(LOG_APP, LOG_DEBUG, kTgCallLogDomain, kTgCallLogTag,
               "Diagnostic tgcalls::Instance received signaling version=%{public}s packets=%{public}zu bytes=%{public}zu",
               version_.c_str(),
               signalingPacketCount_,
               signalingByteCount_);
  if (version_ != "4.0.0") {
    return;
  }
  if (!encryptionKey_) {
    OH_LOG_Print(LOG_APP, LOG_WARN, kTgCallLogDomain, kTgCallLogTag,
                 "Diagnostic Web-K signaling parse skipped version=%{public}s packet=%{public}zu reason=missingKey",
                 version_.c_str(),
                 signalingPacketCount_);
    return;
  }

  std::vector<uint8_t> decryptedPayload;
  uint32_t sequence = 0;
  if (!DecryptRawSignalingPacket(data, *encryptionKey_, encryptionKeyIsOutgoing_, decryptedPayload, sequence)) {
    OH_LOG_Print(LOG_APP, LOG_WARN, kTgCallLogDomain, kTgCallLogTag,
                 "Diagnostic Web-K signaling decrypt failed version=%{public}s packet=%{public}zu encrypted=%{public}zu keyOutgoing=%{public}d",
                 version_.c_str(),
                 signalingPacketCount_,
                 data.size(),
                 encryptionKeyIsOutgoing_ ? 1 : 0);
    return;
  }

  const absl::optional<tgcalls::signaling_4_0_0::Message> parsedMessage =
      tgcalls::signaling_4_0_0::Message::parse(decryptedPayload);
  if (!parsedMessage.has_value()) {
    OH_LOG_Print(LOG_APP, LOG_WARN, kTgCallLogDomain, kTgCallLogTag,
                 "Diagnostic Web-K signaling parse failed version=%{public}s packet=%{public}zu seq=%{public}u decrypted=%{public}zu",
                 version_.c_str(),
                 signalingPacketCount_,
                 sequence,
                 decryptedPayload.size());
    return;
  }

  LogWebKMessageDetails(parsedMessage.value(), version_, signalingPacketCount_, sequence, data.size(), decryptedPayload.size());

  const tgcalls::signaling_4_0_0::InitialSetupMessage *initialSetup =
      absl::get_if<tgcalls::signaling_4_0_0::InitialSetupMessage>(&parsedMessage.value().data);
  if (initialSetup != nullptr) {
    configureRemoteInitialSetup(*initialSetup);
    bool shouldEmitInitialSetup = false;
    bool shouldEmitMediaState = false;
    {
      std::lock_guard<std::mutex> lock(webKSignalingMutex_);
      if (!webKInitialSetupEmitScheduled_) {
        // Mark before scheduling the network-thread emit to dedupe repeated InitialSetup packets.
        webKInitialSetupEmitScheduled_ = true;
        shouldEmitInitialSetup = true;
      } else {
        shouldEmitMediaState = webKInitialSetupEmitted_;
      }
    }
    if (shouldEmitInitialSetup) {
      emitLocalInitialSetup(*initialSetup);
    } else if (shouldEmitMediaState) {
      emitMediaState();
    }
    return;
  }

  const tgcalls::signaling_4_0_0::CandidatesMessage *candidates =
      absl::get_if<tgcalls::signaling_4_0_0::CandidatesMessage>(&parsedMessage.value().data);
  if (candidates != nullptr) {
    addRemoteCandidates(*candidates);
  }
}

void DiagnosticTgCallInstance::setVideoCapture(std::shared_ptr<tgcalls::VideoCaptureInterface> videoCapture) {
  videoCapture_ = std::move(videoCapture);
}

void DiagnosticTgCallInstance::sendVideoDeviceUpdated() {
}

void DiagnosticTgCallInstance::setRequestedVideoAspect(float aspect) {
  requestedVideoAspect_ = aspect;
}

void DiagnosticTgCallInstance::stop(std::function<void(tgcalls::FinalState)> completion) {
  size_t incomingRtpPacketCount = 0;
  size_t incomingRtpByteCount = 0;
  size_t unresolvedRtpPacketCount = 0;
  size_t incomingRtcpPacketCount = 0;
  size_t incomingRtcpByteCount = 0;
  size_t dataChannelMessageCount = 0;
  size_t dataChannelMessageByteCount = 0;
  bool dataChannelOpen = false;
  {
    std::lock_guard<std::mutex> lock(mediaDiagnosticsMutex_);
    incomingRtpPacketCount = incomingRtpPacketCount_;
    incomingRtpByteCount = incomingRtpByteCount_;
    unresolvedRtpPacketCount = unresolvedRtpPacketCount_;
    incomingRtcpPacketCount = incomingRtcpPacketCount_;
    incomingRtcpByteCount = incomingRtcpByteCount_;
    dataChannelMessageCount = dataChannelMessageCount_;
    dataChannelMessageByteCount = dataChannelMessageByteCount_;
    dataChannelOpen = dataChannelOpen_;
  }

  OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
               "Diagnostic tgcalls::Instance stopping version=%{public}s signalingPackets=%{public}zu signalingBytes=%{public}zu rtpPackets=%{public}zu rtpBytes=%{public}zu unresolvedRtp=%{public}zu rtcpPackets=%{public}zu rtcpBytes=%{public}zu dataChannelOpen=%{public}d dataMessages=%{public}zu dataBytes=%{public}zu",
               version_.c_str(),
               signalingPacketCount_,
               signalingByteCount_,
               incomingRtpPacketCount,
               incomingRtpByteCount,
               unresolvedRtpPacketCount,
               incomingRtcpPacketCount,
               incomingRtcpByteCount,
               dataChannelOpen ? 1 : 0,
               dataChannelMessageCount,
               dataChannelMessageByteCount);
  if (networking_) {
    networking_->perform([](tgcalls::InstanceNetworking *networking) {
      networking->stop();
    });
    networking_.reset();
  }
  if (completion) {
    completion(tgcalls::FinalState());
  }
}

}  // namespace tgcall
