#include "SignalingMessage.h"

#include "rtc_base/byte_buffer.h"
#include "api/jsep_ice_candidate.h"

namespace tgcalls {
namespace {

constexpr auto kMaxStringLength = 65536;

void Serialize(rtc::ByteBufferWriter &to, const std::string &from) {
	assert(from.size() < kMaxStringLength);

	to.WriteUInt32(uint32_t(from.size()));
	to.WriteString(from);
}

bool Deserialize(std::string &to, rtc::ByteBufferReader &from) {
	uint32_t length = 0;
	if (!from.ReadUInt32(&length)) {
		RTC_LOG(LS_ERROR) << "Could not read string length.";
		return false;
	} else if (length >= kMaxStringLength) {
		RTC_LOG(LS_ERROR) << "Invalid string length: " << length;
		return false;
	} else if (!from.ReadString(&to, length)) {
		RTC_LOG(LS_ERROR) << "Could not read string data.";
		return false;
	}
	return true;
}

void Serialize(rtc::ByteBufferWriter &to, const webrtc::SdpVideoFormat &from) {
	assert(from.parameters.size() < std::numeric_limits<uint8_t>::max());

	Serialize(to, from.name);
	to.WriteUInt8(uint8_t(from.parameters.size()));
	for (const auto &pair : from.parameters) {
		Serialize(to, pair.first);
		Serialize(to, pair.second);
	}
}

bool Deserialize(webrtc::SdpVideoFormat &to, rtc::ByteBufferReader &from) {
	if (!Deserialize(to.name, from)) {
		RTC_LOG(LS_ERROR) << "Could not read video format name.";
		return false;
	}
	auto count = uint8_t();
	if (!from.ReadUInt8(&count)) {
		RTC_LOG(LS_ERROR) << "Could not read video format parameters count.";
		return false;
	}
	for (uint32_t i = 0; i != count; ++i) {
		auto key = std::string();
		auto value = std::string();
		if (!Deserialize(key, from)) {
			RTC_LOG(LS_ERROR) << "Could not read video format parameter key.";
			return false;
		} else if (!Deserialize(value, from)) {
			RTC_LOG(LS_ERROR) << "Could not read video format parameter value.";
			return false;
		}
		to.parameters.emplace(key, value);
	}
	return true;
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
		RTC_LOG(LS_ERROR) << "Could not read candidate string.";
		return false;
	}
	webrtc::JsepIceCandidate parseCandidate{ std::string(), 0 };
	if (!parseCandidate.Initialize(candidate, nullptr)) {
		RTC_LOG(LS_ERROR) << "Could not parse candidate: " << candidate;
		return false;
	}
	to = parseCandidate.candidate();
	return true;
}

void Serialize(rtc::ByteBufferWriter &to, const RequestVideoMessage &from) {
}

bool Deserialize(RequestVideoMessage &to, rtc::ByteBufferReader &reader) {
	return true;
}

void Serialize(rtc::ByteBufferWriter &to, const AcceptVideoMessage &from) {
}

bool Deserialize(AcceptVideoMessage &to, rtc::ByteBufferReader &reader) {
    return true;
}

void Serialize(rtc::ByteBufferWriter &to, const RemoteVideoIsActiveMessage &from) {
	to.WriteUInt8(from.active ? 1 : 0);
}

bool Deserialize(RemoteVideoIsActiveMessage &to, rtc::ByteBufferReader &reader) {
	uint8_t value = 0;
	if (!reader.ReadUInt8(&value)) {
		RTC_LOG(LS_ERROR) << "Could not read videoIsActive.";
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
		RTC_LOG(LS_ERROR) << "Could not read candidates count.";
		return false;
	}
	for (uint32_t i = 0; i != count; ++i) {
		auto candidate = cricket::Candidate();
		if (!Deserialize(candidate, reader)) {
			RTC_LOG(LS_ERROR) << "Could not read candidate.";
			return false;
		}
		to.candidates.push_back(std::move(candidate));
	}
	return true;
}

void Serialize(rtc::ByteBufferWriter &to, const VideoFormatsMessage &from) {
	assert(from.formats.size() < std::numeric_limits<uint8_t>::max());
	assert(from.encodersCount <= from.formats.size());

	to.WriteUInt8(uint8_t(from.formats.size()));
	for (const auto &format : from.formats) {
		Serialize(to, format);
	}
	to.WriteUInt8(uint8_t(from.encodersCount));
}

bool Deserialize(VideoFormatsMessage &to, rtc::ByteBufferReader &from) {
	auto count = uint8_t();
	if (!from.ReadUInt8(&count)) {
		RTC_LOG(LS_ERROR) << "Could not read video formats count.";
		return false;
	}
	for (uint32_t i = 0; i != count; ++i) {
		auto format = webrtc::SdpVideoFormat(std::string());
		if (!Deserialize(format, from)) {
			RTC_LOG(LS_ERROR) << "Could not read video format.";
			return false;
		}
		to.formats.push_back(std::move(format));
	}
	auto encoders = uint8_t();
	if (!from.ReadUInt8(&encoders)) {
		RTC_LOG(LS_ERROR) << "Could not read encoders count.";
		return false;
	} else if (encoders > to.formats.size()) {
		RTC_LOG(LS_ERROR) << "Invalid encoders count: " << encoders << ", full formats count: " << to.formats.size();
		return false;
	}
	to.encodersCount = encoders;
	return true;
}

template <typename T>
bool TryDeserialize(absl::optional<SignalingMessage> &to, const rtc::CopyOnWriteBuffer &from) {
	assert(from.size() != 0);

	constexpr auto id = T::kId;
	if (from[0] != id) {
		return false;
	}
	auto parsed = T();
	rtc::ByteBufferReader reader((const char *)from.data() + 1, from.size());
	if (!Deserialize(parsed, reader)) {
		RTC_LOG(LS_ERROR) << "Could not read message with kId: " << id;
		return false;
	}
	to = SignalingMessage{ parsed };
	return true;
}

template <typename ...Types>
struct TryDeserializeNext;

template <>
struct TryDeserializeNext<> {
	static bool Call(absl::optional<SignalingMessage> &to, const rtc::CopyOnWriteBuffer &from) {
		return false;
	}
};

template <typename T, typename ...Other>
struct TryDeserializeNext<T, Other...> {
	static bool Call(absl::optional<SignalingMessage> &to, const rtc::CopyOnWriteBuffer &from) {
		return TryDeserialize<T>(to, from)
			|| TryDeserializeNext<Other...>::Call(to, from);
	}
};

template <typename ...Types>
bool TryDeserializeRecursive(
		absl::optional<SignalingMessage> &to,
		const rtc::CopyOnWriteBuffer &from,
		absl::variant<Types...> *) {
	return TryDeserializeNext<Types...>::Call(to, from);
}

} // namespace

rtc::CopyOnWriteBuffer SerializeMessage(const SignalingMessage &message) {
	rtc::ByteBufferWriter writer;
	absl::visit([&](const auto &data) {
		writer.WriteUInt8(std::decay_t<decltype(data)>::kId);
		Serialize(writer, data);
	}, message.data);

    auto result = rtc::CopyOnWriteBuffer();
    result.AppendData(writer.Data(), writer.Length());

	return result;
}

absl::optional<SignalingMessage> DeserializeMessage(const rtc::CopyOnWriteBuffer &data) {
    if (data.size() == 0) {
		return absl::nullopt;
	}
	using Variant = decltype(std::declval<SignalingMessage>().data);
	auto result = absl::make_optional<SignalingMessage>();
	return TryDeserializeRecursive(result, data, (Variant*)nullptr)
		? result
		: absl::nullopt;
}

} // namespace tgcalls
