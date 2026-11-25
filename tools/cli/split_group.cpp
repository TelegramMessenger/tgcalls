#include "split_group.h"

#include <chrono>

#include "third-party/json11.hpp"

namespace {

constexpr int kHelloTimeoutMs = 60000;
constexpr int kJoinTimeoutMs = 30000;
constexpr int kRequestTimeoutMs = 15000;

json11::Json infoToJson(const SplitParticipantInfo& info) {
    return json11::Json::object{
        {"id", info.id},
        {"isReference", info.isReference},
        {"audioSsrc", (double)info.audioSsrc},
        {"muted", info.muted},
        {"sharesScreen", info.sharesScreen},
        {"sendsVideo", info.sendsVideo},
        {"endpointId", info.endpointId},
    };
}

SplitParticipantInfo infoFromJson(const json11::Json& json) {
    SplitParticipantInfo info;
    info.id = json["id"].int_value();
    info.isReference = json["isReference"].bool_value();
    info.audioSsrc = (uint32_t)(int64_t)json["audioSsrc"].number_value();
    info.muted = json["muted"].bool_value();
    info.sharesScreen = json["sharesScreen"].bool_value();
    info.sendsVideo = json["sendsVideo"].bool_value();
    info.endpointId = json["endpointId"].string_value();
    return info;
}

json11::Json parse(const std::string& text) {
    std::string error;
    auto json = json11::Json::parse(text, error);
    return error.empty() ? json : json11::Json();
}

} // namespace

// ---------------------------------------------------------------------------
// SplitGroupHost
// ---------------------------------------------------------------------------

SplitGroupHost::SplitGroupHost(std::shared_ptr<SplitChannel> channel, GoInt sfuHandle,
                               std::vector<SplitParticipantInfo> participants)
    : _channel(std::move(channel)), _sfuHandle(sfuHandle), _participants(std::move(participants)) {
    _thread = std::thread([this] { serve(); });
}

SplitGroupHost::~SplitGroupHost() {
    _channel->shutdown();
    if (_thread.joinable()) {
        _thread.join();
    }
}

bool SplitGroupHost::waitReady(int timeoutMs) {
    std::unique_lock<std::mutex> lock(_mutex);
    _cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return _ready || _gone; });
    if (!_ready && _error.empty()) {
        _error = _gone ? "the joining process went away before it was ready" : "the joining process was not ready in time";
    }
    return _ready;
}

bool SplitGroupHost::waitDone(int timeoutMs) {
    std::unique_lock<std::mutex> lock(_mutex);
    _cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return _done || _gone; });
    if (!_done && _error.empty()) {
        _error = _gone ? "the joining process went away before it finished" : "the joining process did not finish in time";
    }
    return _done;
}

std::string SplitGroupHost::error() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _error;
}

void SplitGroupHost::serve() {
    while (true) {
        std::string request;
        if (!_channel->receive(request, -1)) {
            break;
        }
        if (!_channel->send(handle(request))) {
            break;
        }
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _gone = true;
    _cv.notify_all();
}

std::string SplitGroupHost::handle(const std::string& text) {
    const auto request = parse(text);
    const std::string op = request["op"].string_value();
    json11::Json::object reply;
    if (op == "hello") {
        const int protocol = request["protocol"].int_value();
        reply["protocol"] = kSplitProtocolVersion;
        if (protocol != kSplitProtocolVersion) {
            std::lock_guard<std::mutex> lock(_mutex);
            _error = "the joining process speaks split protocol " + std::to_string(protocol) + ", this one " +
                std::to_string(kSplitProtocolVersion);
            reply["error"] = _error;
        } else {
            json11::Json::array participants;
            for (const auto& info : _participants) {
                participants.push_back(infoToJson(info));
            }
            reply["participants"] = participants;
            groupLog("Split", "the joining process said hello");
        }
    } else if (op == "join") {
        const auto info = infoFromJson(request["info"]);
        const std::string response = sfuJoin(_sfuHandle, request["id"].int_value(), request["payload"].string_value(),
                                             request["iceControlling"].bool_value(), info);
        if (!response.empty()) {
            addRemoteParticipant(info);
            groupLog("Split", "P%d joined from the other process (ssrc=%u, %s)", info.id, info.audioSsrc,
                     info.isReference ? "reference" : "custom");
        }
        reply["response"] = response;
    } else if (op == "querySsrc") {
        reply["value"] = (double)sfuQuerySsrc(_sfuHandle, (uint32_t)(int64_t)request["ssrc"].number_value());
    } else if (op == "queryVideoSsrcs") {
        reply["value"] = sfuQueryVideoSsrcs(_sfuHandle, request["id"].int_value());
    } else if (op == "queryRequestedLayer") {
        reply["value"] = sfuQueryRequestedLayer(_sfuHandle, request["receiver"].int_value(), request["sender"].int_value());
    } else if (op == "querySsrcPackets") {
        reply["value"] = (double)sfuQuerySsrcPackets(_sfuHandle, (uint32_t)(int64_t)request["ssrc"].number_value());
    } else if (op == "queryPayloadTypeStats") {
        reply["value"] = sfuQueryPayloadTypeStats(_sfuHandle, request["sender"].int_value());
    } else if (op == "setNetworkParams") {
        sfuSetNetworkParams(_sfuHandle, request["id"].int_value(), request["direction"].int_value(),
                            request["delayMs"].int_value(), request["jitterMs"].int_value(),
                            request["dropRate"].number_value(), (int64_t)request["bandwidthBps"].number_value());
    } else if (op == "ready") {
        std::lock_guard<std::mutex> lock(_mutex);
        _ready = true;
        _cv.notify_all();
        groupLog("Split", "the joining process is ready");
    } else if (op == "done") {
        std::lock_guard<std::mutex> lock(_mutex);
        _done = true;
        _cv.notify_all();
        groupLog("Split", "the joining process is done");
    } else {
        reply["error"] = "unknown request " + op;
    }
    return json11::Json(reply).dump();
}

// ---------------------------------------------------------------------------
// SplitGroupClient
// ---------------------------------------------------------------------------

SplitGroupClient::SplitGroupClient(std::shared_ptr<SplitChannel> channel) : _channel(std::move(channel)) {}

std::string SplitGroupClient::call(const std::string& request, int timeoutMs) {
    std::lock_guard<std::mutex> lock(_callMutex);
    std::string reply;
    if (!_channel->send(request) || !_channel->receive(reply, timeoutMs)) {
        if (!_failed.exchange(true)) {
            fprintf(stderr, "Error: --split join: no answer from the host to %s\n", request.substr(0, 80).c_str());
        }
        return std::string();
    }
    return reply;
}

bool SplitGroupClient::hello(std::vector<SplitParticipantInfo>& hostParticipants, std::string& error) {
    const std::string text = call(json11::Json(json11::Json::object{{"op", "hello"}, {"protocol", kSplitProtocolVersion}}).dump(),
                                  kHelloTimeoutMs);
    if (text.empty()) {
        error = "no answer to hello";
        return false;
    }
    const auto reply = parse(text);
    if (reply["protocol"].int_value() != kSplitProtocolVersion || !reply["error"].string_value().empty()) {
        error = "the host speaks split protocol " + std::to_string(reply["protocol"].int_value()) + ", this process " +
            std::to_string(kSplitProtocolVersion) + " " + reply["error"].string_value();
        return false;
    }
    for (const auto& entry : reply["participants"].array_items()) {
        hostParticipants.push_back(infoFromJson(entry));
    }
    return true;
}

bool SplitGroupClient::ready() {
    return !call(json11::Json(json11::Json::object{{"op", "ready"}}).dump(), kRequestTimeoutMs).empty();
}

bool SplitGroupClient::done() {
    return !call(json11::Json(json11::Json::object{{"op", "done"}}).dump(), kRequestTimeoutMs).empty();
}

std::string SplitGroupClient::join(int participantId, const std::string& payload, bool iceControlling,
                                   const SplitParticipantInfo& info) {
    const auto reply = parse(call(json11::Json(json11::Json::object{
        {"op", "join"}, {"id", participantId}, {"payload", payload}, {"iceControlling", iceControlling},
        {"info", infoToJson(info)}}).dump(), kJoinTimeoutMs));
    return reply["response"].string_value();
}

int64_t SplitGroupClient::querySsrc(uint32_t ssrc) {
    const auto reply = parse(call(json11::Json(json11::Json::object{{"op", "querySsrc"}, {"ssrc", (double)ssrc}}).dump(),
                                  kRequestTimeoutMs));
    return reply["value"].is_number() ? (int64_t)reply["value"].number_value() : -1;
}

std::string SplitGroupClient::queryVideoSsrcs(int participantId) {
    const auto reply = parse(call(json11::Json(json11::Json::object{{"op", "queryVideoSsrcs"}, {"id", participantId}}).dump(),
                                  kRequestTimeoutMs));
    return reply["value"].string_value();
}

int SplitGroupClient::queryRequestedLayer(int receiverId, int senderId) {
    const auto reply = parse(call(json11::Json(json11::Json::object{
        {"op", "queryRequestedLayer"}, {"receiver", receiverId}, {"sender", senderId}}).dump(), kRequestTimeoutMs));
    return reply["value"].is_number() ? reply["value"].int_value() : -1;
}

int64_t SplitGroupClient::querySsrcPackets(uint32_t ssrc) {
    const auto reply = parse(call(json11::Json(json11::Json::object{{"op", "querySsrcPackets"}, {"ssrc", (double)ssrc}}).dump(),
                                  kRequestTimeoutMs));
    return reply["value"].is_number() ? (int64_t)reply["value"].number_value() : -1;
}

std::string SplitGroupClient::queryPayloadTypeStats(int senderId) {
    const auto reply = parse(call(json11::Json(json11::Json::object{{"op", "queryPayloadTypeStats"}, {"sender", senderId}}).dump(),
                                  kRequestTimeoutMs));
    return reply["value"].string_value();
}

void SplitGroupClient::setNetworkParams(int participantId, int direction, int delayMs, int jitterMs, double dropRate,
                                        int64_t bandwidthBps) {
    call(json11::Json(json11::Json::object{
        {"op", "setNetworkParams"}, {"id", participantId}, {"direction", direction}, {"delayMs", delayMs},
        {"jitterMs", jitterMs}, {"dropRate", dropRate}, {"bandwidthBps", (double)bandwidthBps}}).dump(), kRequestTimeoutMs);
}
