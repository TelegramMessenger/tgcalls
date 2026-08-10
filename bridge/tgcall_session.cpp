#include "tgcall_session.h"

#include "base64_util.h"
#include "tgcall_logging.h"
#include "third-party/json11.hpp"

#include <hilog/log.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

namespace tgcall {
namespace {

uint64_t ParseUnsignedId(const std::string &value) {
  if (value.empty()) {
    return 0;
  }
  uint64_t result = 0;
  for (char character : value) {
    if (character < '0' || character > '9') {
      return 0;
    }
    result = result * 10 + static_cast<uint64_t>(character - '0');
  }
  return result;
}

std::string ReadJsonString(const json11::Json::object &object, const std::string &key) {
  const auto item = object.find(key);
  if (item == object.end() || !item->second.is_string()) {
    return "";
  }
  return item->second.string_value();
}

std::string ReadJsonStringFallback(
    const json11::Json::object &object,
    const std::string &primaryKey,
    const std::string &fallbackKey) {
  std::string result = ReadJsonString(object, primaryKey);
  if (!result.empty()) {
    return result;
  }
  return ReadJsonString(object, fallbackKey);
}

int ReadJsonInt(const json11::Json::object &object, const std::string &key) {
  const auto item = object.find(key);
  if (item == object.end() || !item->second.is_number()) {
    return 0;
  }
  return item->second.int_value();
}

double ReadJsonNumber(const json11::Json::object &object, const std::string &key, double defaultValue) {
  const auto item = object.find(key);
  if (item == object.end() || !item->second.is_number()) {
    return defaultValue;
  }
  return item->second.number_value();
}

std::vector<std::string> ReadJsonStringArray(const json11::Json::object &object, const std::string &key) {
  std::vector<std::string> result;
  const auto item = object.find(key);
  if (item == object.end() || !item->second.is_array()) {
    return result;
  }
  for (const json11::Json &entry : item->second.array_items()) {
    if (entry.is_string()) {
      result.push_back(entry.string_value());
    }
  }
  return result;
}

bool ReadJsonBool(const json11::Json::object &object, const std::string &key) {
  const auto item = object.find(key);
  if (item == object.end() || !item->second.is_bool()) {
    return false;
  }
  return item->second.bool_value();
}

bool ReadJsonBoolFallback(
    const json11::Json::object &object,
    const std::string &primaryKey,
    const std::string &fallbackKey) {
  if (ReadJsonBool(object, primaryKey)) {
    return true;
  }
  return ReadJsonBool(object, fallbackKey);
}

std::string ReadServerType(const json11::Json::object &object) {
  const auto item = object.find("type");
  if (item == object.end()) {
    return "";
  }
  if (item->second.is_string()) {
    return item->second.string_value();
  }
  if (!item->second.is_object()) {
    return "";
  }
  return ReadJsonString(item->second.object_items(), "@type");
}

std::string BytesToHex(const std::vector<uint8_t> &bytes) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (uint8_t byte : bytes) {
    stream << std::setw(2) << static_cast<int>(byte);
  }
  return stream.str();
}

std::vector<uint8_t> DecodePeerTagBytes(const std::string &peerTag) {
  std::vector<uint8_t> result;
  if (peerTag.empty()) {
    return result;
  }
  if (DecodeBase64(peerTag, result) && !result.empty()) {
    return result;
  }

  result.clear();
  result.reserve(peerTag.size());
  for (char character : peerTag) {
    result.push_back(static_cast<uint8_t>(character));
  }
  return result;
}

std::string DecodePeerTagPassword(const std::string &peerTag) {
  return BytesToHex(DecodePeerTagBytes(peerTag));
}

std::map<uint64_t, uint8_t> BuildReflectorIdMap(const json11::Json::array &servers) {
  std::vector<uint64_t> reflectorIds;
  for (const json11::Json &server : servers) {
    if (!server.is_object()) {
      continue;
    }
    const json11::Json::object &object = server.object_items();
    const std::string type = ReadJsonString(object, "type");
    if (type != "callServerTypeTelegramReflector") {
      continue;
    }
    const uint64_t serverId = ParseUnsignedId(ReadJsonString(object, "id"));
    if (serverId > 0) {
      reflectorIds.push_back(serverId);
    }
  }
  std::sort(reflectorIds.begin(), reflectorIds.end());
  reflectorIds.erase(std::unique(reflectorIds.begin(), reflectorIds.end()), reflectorIds.end());

  std::map<uint64_t, uint8_t> result;
  for (size_t index = 0; index < reflectorIds.size() && index < 255; index += 1) {
    result.emplace(reflectorIds[index], static_cast<uint8_t>(index + 1));
  }
  return result;
}

struct ParsedServerMapping {
  std::vector<tgcalls::RtcServer> rtcServers;
  std::vector<tgcalls::Endpoint> endpoints;
};

ParsedServerMapping ParseServers(const std::string &serversJson) {
  ParsedServerMapping result;
  if (serversJson.empty()) {
    return result;
  }

  std::string parseError;
  const json11::Json parsed = json11::Json::parse(serversJson, parseError);
  if (!parseError.empty() || !parsed.is_array()) {
    OH_LOG_Print(LOG_APP, LOG_WARN, kTgCallLogDomain, kTgCallLogTag,
                 "Failed to parse tgcall servers json error=%{public}s len=%{public}zu",
                 parseError.c_str(),
                 serversJson.size());
    return result;
  }

  const json11::Json::array &servers = parsed.array_items();
  const std::map<uint64_t, uint8_t> reflectorIdMap = BuildReflectorIdMap(servers);
  for (const json11::Json &server : servers) {
    if (!server.is_object()) {
      continue;
    }
    const json11::Json::object &object = server.object_items();
    const std::string type = ReadServerType(object);
    const std::string serverIdString = ReadJsonString(object, "id");
    const int port = ReadJsonInt(object, "port");
    if (port <= 0 || port > 65535) {
      OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                   "Skipped tgcall server with invalid port type=%{public}s id=%{public}s port=%{public}d",
                   type.c_str(),
                   serverIdString.c_str(),
                   port);
      continue;
    }

    tgcalls::RtcServer rtcServer;
    const uint64_t serverId = ParseUnsignedId(serverIdString);
    const auto reflectorId = reflectorIdMap.find(serverId);
    rtcServer.id = reflectorId == reflectorIdMap.end() ? 0 : reflectorId->second;
    rtcServer.host = ReadJsonStringFallback(object, "ipAddress", "ip_address");
    if (rtcServer.host.empty()) {
      rtcServer.host = ReadJsonStringFallback(object, "ipv6Address", "ipv6_address");
    }
    rtcServer.port = static_cast<uint16_t>(port);

    if (type == "callServerTypeTelegramReflector") {
      rtcServer.login = "reflector";
      rtcServer.password = DecodePeerTagPassword(ReadJsonStringFallback(object, "peerTag", "peer_tag"));
      rtcServer.isTurn = true;
      rtcServer.isTcp = false;
      if (!rtcServer.host.empty() && !rtcServer.password.empty()) {
        result.rtcServers.push_back(rtcServer);

        tgcalls::Endpoint endpoint;
        endpoint.endpointId = static_cast<int64_t>(serverId);
        endpoint.host = tgcalls::EndpointHost{
            ReadJsonStringFallback(object, "ipAddress", "ip_address"),
            ReadJsonStringFallback(object, "ipv6Address", "ipv6_address")};
        endpoint.port = static_cast<uint16_t>(port);
        endpoint.type = tgcalls::EndpointType::UdpRelay;
        const std::vector<uint8_t> peerTagBytes = DecodePeerTagBytes(ReadJsonStringFallback(object, "peerTag", "peer_tag"));
        const size_t peerTagLength = std::min(peerTagBytes.size(), sizeof(endpoint.peerTag));
        if (peerTagLength > 0) {
          std::memcpy(endpoint.peerTag, peerTagBytes.data(), peerTagLength);
        }
        result.endpoints.push_back(std::move(endpoint));
      } else {
        OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                     "Skipped Telegram reflector server id=%{public}s hostLen=%{public}zu peerTagLen=%{public}zu",
                     serverIdString.c_str(),
                     rtcServer.host.size(),
                     rtcServer.password.size());
      }
      continue;
    }

    if (type == "callServerTypeWebrtc") {
      const std::string username = ReadJsonString(object, "username");
      const std::string password = ReadJsonString(object, "password");
      const bool supportsTurn = ReadJsonBoolFallback(object, "supportsTurn", "supports_turn");
      const bool supportsStun = ReadJsonBoolFallback(object, "supportsStun", "supports_stun");
      OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                   "Mapped WebRTC tgcall server id=%{public}s hostLen=%{public}zu port=%{public}d turn=%{public}d stun=%{public}d userLen=%{public}zu passLen=%{public}zu",
                   serverIdString.c_str(),
                   rtcServer.host.size(),
                   port,
                   supportsTurn ? 1 : 0,
                   supportsStun ? 1 : 0,
                   username.size(),
                   password.size());
      // #65 E4 diagnostics: list the JSON keys that actually crossed the ArkTS->native
      // boundary for this server (key names only — never credential values) so a missing
      // field is attributable to the producer side. Remove with task 4.4 C1 cleanup.
      {
        std::string keyList;
        for (const auto &entry : object) {
          if (!keyList.empty()) {
            keyList += ",";
          }
          keyList += entry.first;
        }
        OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                     "WebRTC server diag id=%{public}s keys=%{public}s",
                     serverIdString.c_str(),
                     keyList.c_str());
      }
      if (!rtcServer.host.empty()) {
        rtcServer.login = username;
        rtcServer.password = password;
        rtcServer.isTurn = supportsTurn;
        rtcServer.isTcp = false;
        result.rtcServers.push_back(std::move(rtcServer));
      }
      continue;
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                 "Skipped unsupported tgcall server type=%{public}s id=%{public}s",
                 type.c_str(),
                 serverIdString.c_str());
  }

  return result;
}

}  // namespace

std::shared_ptr<const std::array<uint8_t, kTgCallEncryptionKeySize>> DecodeEncryptionKeyBase64(
    const std::string &keyBase64) {
  std::vector<uint8_t> decoded;
  if (!DecodeBase64(keyBase64, decoded) || decoded.size() != kTgCallEncryptionKeySize) {
    return nullptr;
  }

  std::shared_ptr<std::array<uint8_t, kTgCallEncryptionKeySize>> result =
      std::make_shared<std::array<uint8_t, kTgCallEncryptionKeySize>>();
  std::memcpy(result->data(), decoded.data(), kTgCallEncryptionKeySize);
  return result;
}

tgcalls::Descriptor BuildDescriptor(
    const TgCallSession &session,
    std::shared_ptr<const std::array<uint8_t, kTgCallEncryptionKeySize>> encryptionKey,
    TgCallSignalingDataCallback signalingDataCallback,
    TgCallAudioLevelCallback audioLevelCallback,
    TgCallStateCallback stateCallback,
    TgCallRemoteMediaStateCallback remoteMediaStateCallback,
    TgCallRemoteBatteryCallback remoteBatteryCallback,
    TgCallRemoteAspectRatioCallback remoteAspectRatioCallback,
    TgCallSignalBarsCallback signalBarsCallback) {
  tgcalls::Descriptor descriptor{
      session.protocolVersion,
      tgcalls::Config(),
      tgcalls::PersistentState(),
      std::vector<tgcalls::Endpoint>(),
      nullptr,
      std::vector<tgcalls::RtcServer>(),
      tgcalls::NetworkType::WiFi,
      tgcalls::EncryptionKey(std::move(encryptionKey), session.isOutgoing)};

  // ---- Config：来自 configJson（空串 = 默认 + 兼容旧参数） ----
  std::string configParseError;
  json11::Json configJson = json11::Json::object();
  if (!session.configJson.empty()) {
    configJson = json11::Json::parse(session.configJson, configParseError);
    if (!configParseError.empty() || !configJson.is_object()) {
      OH_LOG_Print(LOG_APP, LOG_WARN, kTgCallLogDomain, kTgCallLogTag,
                   "Failed to parse tgcall configJson error=%{public}s len=%{public}zu",
                   configParseError.c_str(),
                   session.configJson.size());
      configJson = json11::Json::object();
    }
  }
  const json11::Json::object &configObject = configJson.object_items();
  tgcalls::Config &config = descriptor.config;
  config.initializationTimeout = ReadJsonNumber(configObject, "initializationTimeout", 15.0);
  config.receiveTimeout = ReadJsonNumber(configObject, "receiveTimeout", 20.0);
  config.enableP2P = ReadJsonBool(configObject, "enableP2P") || session.allowP2p;
  config.allowTCP = ReadJsonBool(configObject, "allowTCP") || true;
  config.enableStunMarking = ReadJsonBool(configObject, "enableStunMarking");
  config.enableAEC = ReadJsonBool(configObject, "enableAEC") || true;
  config.enableNS = ReadJsonBool(configObject, "enableNS") || true;
  config.enableAGC = ReadJsonBool(configObject, "enableAGC") || true;
  config.enableCallUpgrade = ReadJsonBool(configObject, "enableCallUpgrade");
  config.enableVolumeControl = ReadJsonBool(configObject, "enableVolumeControl");
  config.enableHighBitrateVideo = ReadJsonBool(configObject, "enableHighBitrateVideo");
  config.maxApiLayer = ReadJsonInt(configObject, "maxApiLayer");
  if (config.maxApiLayer <= 0) {
    config.maxApiLayer = 92;
  }
  const int dataSaving = ReadJsonInt(configObject, "dataSaving");
  if (dataSaving >= 0 && dataSaving <= 2) {
    config.dataSaving = static_cast<tgcalls::DataSaving>(dataSaving);
  }
  config.logPath.data = ReadJsonString(configObject, "logPath");
  config.statsLogPath.data = ReadJsonString(configObject, "statsLogPath");
  config.preferredVideoCodecs = ReadJsonStringArray(configObject, "preferredVideoCodecs");
  const std::string configProtocolVersion = ReadJsonString(configObject, "protocolVersion");
  if (configProtocolVersion == "2.7.7") {
    config.protocolVersion = tgcalls::ProtocolVersion::V0;
  } else if (configProtocolVersion == "5.0.0") {
    config.protocolVersion = tgcalls::ProtocolVersion::V1;
  }
  config.customParameters = ReadJsonString(configObject, "customParameters");
  if (config.customParameters.empty()) {
    config.customParameters = session.customParameters;
  }

  // ---- Proxy：来自 proxyJson（空串 = 无代理） ----
  if (!session.proxyJson.empty()) {
    std::string proxyParseError;
    const json11::Json proxyJson = json11::Json::parse(session.proxyJson, proxyParseError);
    if (proxyParseError.empty() && proxyJson.is_object()) {
      const json11::Json::object &proxyObject = proxyJson.object_items();
      std::unique_ptr<tgcalls::Proxy> proxy = std::make_unique<tgcalls::Proxy>();
      proxy->host = ReadJsonString(proxyObject, "host");
      proxy->port = static_cast<uint16_t>(ReadJsonInt(proxyObject, "port"));
      proxy->login = ReadJsonString(proxyObject, "login");
      proxy->password = ReadJsonString(proxyObject, "password");
      if (!proxy->host.empty()) {
        descriptor.proxy = std::move(proxy);
      }
    } else {
      OH_LOG_Print(LOG_APP, LOG_WARN, kTgCallLogDomain, kTgCallLogTag,
                   "Failed to parse tgcall proxyJson error=%{public}s",
                   proxyParseError.c_str());
    }
  }

  // ---- PersistentState：来自 persistentStateBase64（可空） ----
  if (!session.persistentStateBase64.empty()) {
    std::vector<uint8_t> persistentStateBytes;
    if (DecodeBase64(session.persistentStateBase64, persistentStateBytes)) {
      descriptor.persistentState.value = std::move(persistentStateBytes);
    } else {
      OH_LOG_Print(LOG_APP, LOG_WARN, kTgCallLogDomain, kTgCallLogTag,
                   "Failed to decode tgcall persistentStateBase64");
    }
  }

  // ---- MediaDevicesConfig：来自 mediaDevicesConfigJson（可空） ----
  if (!session.mediaDevicesConfigJson.empty()) {
    std::string devicesParseError;
    const json11::Json devicesJson = json11::Json::parse(session.mediaDevicesConfigJson, devicesParseError);
    if (devicesParseError.empty() && devicesJson.is_object()) {
      const json11::Json::object &devicesObject = devicesJson.object_items();
      descriptor.mediaDevicesConfig.audioInputId = ReadJsonString(devicesObject, "audioInputId");
      descriptor.mediaDevicesConfig.audioOutputId = ReadJsonString(devicesObject, "audioOutputId");
      descriptor.mediaDevicesConfig.inputVolume = static_cast<float>(ReadJsonNumber(devicesObject, "inputVolume", 1.0));
      descriptor.mediaDevicesConfig.outputVolume = static_cast<float>(ReadJsonNumber(devicesObject, "outputVolume", 1.0));
    }
  }

  // ---- NetworkType：来自 initialNetworkType（0-11） ----
  if (session.initialNetworkType >= 0 && session.initialNetworkType <= 11) {
    descriptor.initialNetworkType = static_cast<tgcalls::NetworkType>(session.initialNetworkType);
  }

  ParsedServerMapping serverMapping = ParseServers(session.serversJson);
  descriptor.rtcServers = std::move(serverMapping.rtcServers);
  descriptor.endpoints = std::move(serverMapping.endpoints);
  OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
               "Mapped tgcall servers jsonLen=%{public}zu rtcServers=%{public}zu endpoints=%{public}zu allowP2p=%{public}d netType=%{public}d",
               session.serversJson.size(),
               descriptor.rtcServers.size(),
               descriptor.endpoints.size(),
               config.enableP2P ? 1 : 0,
               static_cast<int>(descriptor.initialNetworkType));
  descriptor.stateUpdated = [version = session.protocolVersion, callback = std::move(stateCallback)](tgcalls::State state) {
    OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                 "TgCall instance state version=%{public}s state=%{public}d",
                 version.c_str(),
                 static_cast<int>(state));
    if (callback) {
      callback(state);
    }
  };
  descriptor.signalBarsUpdated = [callback = std::move(signalBarsCallback)](int signalBars) {
    OH_LOG_Print(LOG_APP, LOG_DEBUG, kTgCallLogDomain, kTgCallLogTag,
                 "TgCall signal bars=%{public}d",
                 signalBars);
    if (callback) {
      callback(signalBars);
    }
  };
  descriptor.audioLevelUpdated = [callback = std::move(audioLevelCallback)](float level) {
    if (callback) {
      callback(level);
    }
  };
  descriptor.remoteBatteryLevelIsLowUpdated = [callback = std::move(remoteBatteryCallback)](bool isLow) {
    OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                 "TgCall remote low battery=%{public}d",
                 isLow ? 1 : 0);
    if (callback) {
      callback(isLow);
    }
  };
  descriptor.remoteMediaStateUpdated = [callback = std::move(remoteMediaStateCallback)](
      tgcalls::AudioState audioState, tgcalls::VideoState videoState) {
    OH_LOG_Print(LOG_APP, LOG_INFO, kTgCallLogDomain, kTgCallLogTag,
                 "TgCall remote media state audio=%{public}d video=%{public}d",
                 static_cast<int>(audioState),
                 static_cast<int>(videoState));
    if (callback) {
      callback(audioState, videoState);
    }
  };
  descriptor.remotePrefferedAspectRatioUpdated = [callback = std::move(remoteAspectRatioCallback)](float ratio) {
    if (callback) {
      callback(ratio);
    }
  };
  descriptor.signalingDataEmitted = [callback = std::move(signalingDataCallback)](const std::vector<uint8_t> &data) {
    OH_LOG_Print(LOG_APP, LOG_DEBUG, kTgCallLogDomain, kTgCallLogTag,
                 "TgCall emitted signaling bytes=%{public}zu",
                 data.size());
    if (callback) {
      callback(data);
    }
  };
  return descriptor;
}

int StateToInt(tgcalls::State state) {
  return static_cast<int>(state);
}

int AudioStateToInt(tgcalls::AudioState state) {
  return static_cast<int>(state);
}

int VideoStateToInt(tgcalls::VideoState state) {
  return static_cast<int>(state);
}

std::string TrafficStatsToJson(const tgcalls::TrafficStats &stats) {
  return "{"
         "\"bytesSentWifi\":" + std::to_string(stats.bytesSentWifi) +
         ",\"bytesReceivedWifi\":" + std::to_string(stats.bytesReceivedWifi) +
         ",\"bytesSentMobile\":" + std::to_string(stats.bytesSentMobile) +
         ",\"bytesReceivedMobile\":" + std::to_string(stats.bytesReceivedMobile) +
         "}";
}

std::string FinalStateToJson(const tgcalls::FinalState &finalState) {
  std::string persistentStateBase64 = EncodeBase64(finalState.persistentState.value);
  return "{"
         "\"isRatingSuggested\":" + std::string(finalState.isRatingSuggested ? "true" : "false") +
         ",\"debugLogLen\":" + std::to_string(finalState.debugLog.size()) +
         ",\"persistentState\":\"" + persistentStateBase64 + "\"" +
         ",\"trafficStats\":" + TrafficStatsToJson(finalState.trafficStats) +
         "}";
}

}  // namespace tgcall
