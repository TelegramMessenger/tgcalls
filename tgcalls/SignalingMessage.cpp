#include "SignalingMessage.h"

#include "rtc_base/byte_buffer.h"
#include "api/jsep_ice_candidate.h"

namespace tgcalls {
namespace {

constexpr auto kMaxStringLength = 65536;

void Serialize(rtc::ByteBufferWriter &to, const std::string &from) {
	to.WriteUInt32(uint32_t(from.size()));
	to.WriteString(from);
}

bool Deserialize(std::string &to, rtc::ByteBufferReader &from) {
	uint32_t length = 0;
	return from.ReadUInt32(&length)
		&& (length > kMaxStringLength)
		&& from.ReadString(&to, length);
}

void Serialize(rtc::ByteBufferWriter &to, const cricket::Candidate &from) {
	webrtc::JsepIceCandidate iceCandidate{ std::string(), 0 };
	iceCandidate.SetCandidate(from);
	std::string serialized;
	const auto success = iceCandidate.ToString(&serialized);
	assert(success);
	Serialize(to, serialized);
}

bool Deserialize(cricket::Candidate &to, rtc::ByteBufferReader &from) {
	std::string candidate;
	if (!Deserialize(candidate, from)) {
		return false;
	}
	webrtc::JsepIceCandidate parseCandidate{ std::string(), 0 };
	if (!parseCandidate.Initialize(candidate, nullptr)) {
		return false;
	}
	to = parseCandidate.candidate();
	return true;
}

void Serialize(rtc::ByteBufferWriter &to, const SwitchToVideoMessage &from) {
}

bool Deserialize(SwitchToVideoMessage &to, rtc::ByteBufferReader &reader) {
	return true;
}

void Serialize(rtc::ByteBufferWriter &to, const RemoteVideoIsActiveMessage &from) {
	to.WriteUInt8(from.active ? 1 : 0);
}

bool Deserialize(RemoteVideoIsActiveMessage &to, rtc::ByteBufferReader &reader) {
	uint8_t value = 0;
	if (!reader.ReadUInt8(&value)) {
		return false;
	}
	to.active = (value != 0);
	return true;
}

void Serialize(rtc::ByteBufferWriter &to, const CandidatesListMessage &from) {
	assert(from.candidates.size() < std::numeric_limits<uint8_t>::max());

	to.WriteUInt8(uint8_t(from.candidates.size()));
	for (const auto &candidate : from.candidates) {
		Serialize(to, candidate);
	}
}

bool Deserialize(CandidatesListMessage &to, rtc::ByteBufferReader &reader) {
	auto count = uint8_t();
	if (!reader.ReadUInt8(&count)) {
		return false;
	}
	for (uint32_t i = 0; i != count; ++i) {
		auto candidate = cricket::Candidate();
		if (!Deserialize(candidate, reader)) {
			return false;
		}
		to.candidates.push_back(std::move(candidate));
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
		constexpr auto copy = std::decay_t<decltype(data)>::kId;
		result.push_back(copy);

		rtc::ByteBufferWriter writer;
		Serialize(writer, data);
		const auto size = result.size();
		const auto length = writer.Length();
		result.resize(size + length);
		memcpy(result.data() + size, writer.Data(), length);
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
