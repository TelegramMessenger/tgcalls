#include "SignalingMessage.h"

#include "rtc_base/byte_buffer.h"
#include "api/jsep_ice_candidate.h"

namespace tgcalls {
namespace {

constexpr auto kMaxCandidates = 128;
constexpr auto kMaxCandidateLength = 256;

void Serialize(std::vector<uint8_t> &to, const SwitchToVideoMessage &from) {
}

bool Deserialize(SwitchToVideoMessage &to, rtc::ByteBufferReader &reader) {
	return true;
}

void Serialize(std::vector<uint8_t> &to, const RemoteVideoIsActiveMessage &from) {
	to.push_back(from.active ? 1 : 0);
}

bool Deserialize(RemoteVideoIsActiveMessage &to, rtc::ByteBufferReader &reader) {
	uint8_t value = 0;
	if (!reader.ReadUInt8(&value)) {
		return false;
	}
	to.active = (value != 0);
	return true;
}

void Serialize(std::vector<uint8_t> &to, const CandidatesListMessage &from) {
	rtc::ByteBufferWriter writer;
	writer.WriteUInt32((uint32_t)from.candidates.size());
	for (const auto &candidate : from.candidates) {
		webrtc::JsepIceCandidate iceCandidate("", 0);
		iceCandidate.SetCandidate(candidate);
		std::string serializedCandidate;
		if (!iceCandidate.ToString(&serializedCandidate)) {
			to.clear(); // error
			return;
		}
		writer.WriteUInt32((uint32_t)serializedCandidate.size());
		writer.WriteString(serializedCandidate);
	}
	const auto size = to.size();
	const auto length = writer.Length();
	to.resize(size + length);
	memcpy(to.data() + size, writer.Data(), length);
}

bool Deserialize(CandidatesListMessage &to, rtc::ByteBufferReader &reader) {
	uint32_t candidateCount = 0;
	if (!reader.ReadUInt32(&candidateCount) || candidateCount > kMaxCandidates) {
		return false;
	}
	for (uint32_t i = 0; i < candidateCount; i++) {
		uint32_t candidateLength = 0;
		if (!reader.ReadUInt32(&candidateLength) || candidateLength > kMaxCandidateLength) {
			return false;
		}
		std::string candidate;
		if (!reader.ReadString(&candidate, candidateLength)) {
			return false;
		}
		webrtc::JsepIceCandidate parseCandidate("", 0);
		if (!parseCandidate.Initialize(candidate, nullptr)) {
			return false;
		}
		to.candidates.push_back(parseCandidate.candidate());
	}
	return true;
}

template <typename T>
bool TryDeserialize(absl::optional<SignalingMessage> &to, const std::vector<uint8_t> &from) {
	assert(!from.empty());

	if (from[0] != T::kId) {
		return false;
	}
	auto parsed = T();
	rtc::ByteBufferReader reader((const char *)from.data() + 1, from.size());
	if (!Deserialize(parsed, reader)) {
		return false;
	}
	to = SignalingMessage{ parsed };
	return true;
}

} // namespace

std::vector<uint8_t> SerializeMessage(const SignalingMessage &message) {
	auto result = std::vector<uint8_t>();
	absl::visit([&](const auto &data) {
		result.push_back(data.kId);
		Serialize(result, data);
	}, message.data);
	return result;
}

absl::optional<SignalingMessage> DeserializeMessage(const std::vector<uint8_t> &data) {
	if (data.empty()) {
		return absl::nullopt;
	}
	auto result = absl::make_optional<SignalingMessage>();
	return (TryDeserialize<SwitchToVideoMessage>(result, data)
		|| TryDeserialize<RemoteVideoIsActiveMessage>(result, data)
		|| TryDeserialize<CandidatesListMessage>(result, data))
		? result
		: absl::nullopt;
}

} // namespace tgcalls
