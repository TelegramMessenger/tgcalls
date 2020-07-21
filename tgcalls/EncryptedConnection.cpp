#include "EncryptedConnection.h"

#include "CryptoHelper.h"
#include "rtc_base/logging.h"
#include "rtc_base/byte_buffer.h"

namespace tgcalls {
namespace {

constexpr auto kSingleMessagePacketSeqBit = (uint32_t(1) << 31);
constexpr auto kMessageRequiresAckSeqBit = (uint32_t(1) << 30);
constexpr auto kMaxAllowedCounter = std::numeric_limits<uint32_t>::max()
	& ~kSingleMessagePacketSeqBit
	& ~kMessageRequiresAckSeqBit;

static_assert(kMaxAllowedCounter < kSingleMessagePacketSeqBit, "bad");
static_assert(kMaxAllowedCounter < kMessageRequiresAckSeqBit, "bad");

constexpr auto kAckSerializedSize = sizeof(uint32_t) + sizeof(uint8_t);
constexpr auto kNotAckedMessagesLimit = 1024 * 1024; // some insane number
constexpr auto kMaxIncomingPacketSize = 128 * 1024; // don't try decrypting more
constexpr auto kKeepIncomingCountersCount = 64;

void AppendSeq(rtc::CopyOnWriteBuffer &buffer, uint32_t seq) {
	const auto bytes = rtc::HostToNetwork32(seq);
	buffer.AppendData(reinterpret_cast<const char*>(&bytes), sizeof(bytes));
}

void WriteSeq(void *bytes, uint32_t seq) {
	*reinterpret_cast<uint32_t*>(bytes) = rtc::HostToNetwork32(seq);
}

uint32_t ReadSeq(const void *bytes) {
	return rtc::NetworkToHost32(*reinterpret_cast<const uint32_t*>(bytes));
}

uint32_t CounterFromSeq(uint32_t seq) {
	return seq & ~kSingleMessagePacketSeqBit & ~kMessageRequiresAckSeqBit;
}

absl::nullopt_t LogError(
		const char *message,
		const std::string &additional = std::string()) {
	RTC_LOG(LS_ERROR) << "ERROR! " << message << additional;
	return absl::nullopt;
}

} // namespace

EncryptedConnection::EncryptedConnection(Type type, const EncryptionKey &key) :
_type(type),
_key(key) {
	assert(_key.value != nullptr);
}

auto EncryptedConnection::prepareForSending(const Message &message)
-> absl::optional<EncryptedPacket> {
	const auto messageRequiresAck = absl::visit([](const auto &data) {
		return std::decay_t<decltype(data)>::kRequiresAck;
	}, message.data);

	// If message requires ack, then we can't serialize it as a single
	// message packet, because later it may be sent as a part of big packet.
	const auto singleMessagePacket = _myNotYetAckedMessages.empty()
		&& _acksToResend.empty()
		&& !messageRequiresAck;

	if (messageRequiresAck && _myNotYetAckedMessages.size() >= kNotAckedMessagesLimit) {
		return LogError("Too many not acked messages.");
	} else if (_counter == kMaxAllowedCounter) {
		return LogError("Outgoing packet limit reached.");
	}

	const auto seq = (++_counter)
		| (singleMessagePacket ? kSingleMessagePacketSeqBit : 0)
		| (messageRequiresAck ? kMessageRequiresAckSeqBit : 0);
	auto serialized = SerializeMessageWithSeq(message, seq, singleMessagePacket);
	if (!enoughSpaceInPacket(serialized, 0)) {
		return LogError("Too large packet: ", std::to_string(serialized.size()));
	}

	const auto notYetAckedCopy = messageRequiresAck
		? serialized
		: rtc::CopyOnWriteBuffer();
	appendAdditionalMessages(serialized);
	if (messageRequiresAck) {
		_myNotYetAckedMessages.push_back({ notYetAckedCopy });
	}
	return encryptPrepared(serialized);
}

size_t EncryptedConnection::packetLimit() const {
	return 1400; // TODO
}

bool EncryptedConnection::enoughSpaceInPacket(const rtc::CopyOnWriteBuffer &buffer, size_t amount) const {
	const auto limit = packetLimit();
	return (amount < limit)
		&& (16 + buffer.size() + amount <= limit);
}

void EncryptedConnection::appendAcksToResend(rtc::CopyOnWriteBuffer &buffer) {
	auto i = _acksToResend.begin();
	while ((i != _acksToResend.end())
		&& enoughSpaceInPacket(
			buffer,
			kAckSerializedSize)) {
		AppendSeq(buffer, *i);
		buffer.AppendData(&kAckId, 1);
		++i;
	}
	_acksToResend.erase(_acksToResend.begin(), i);
}

size_t EncryptedConnection::fullNotAckedLength() const {
	assert(_myNotYetAckedMessages.size() < kNotAckedMessagesLimit);

	auto result = size_t();
	for (const auto &message : _myNotYetAckedMessages) {
		result += message.data.size();
	}
	return result;
}

void EncryptedConnection::appendAdditionalMessages(rtc::CopyOnWriteBuffer &buffer) {
	appendAcksToResend(buffer);
	if (enoughSpaceInPacket(buffer, fullNotAckedLength())) {
		for (const auto &resending : _myNotYetAckedMessages) {
			buffer.AppendData(resending.data);
		}
	} else {
		// TODO schedule empty sending with additionals
	}
}

auto EncryptedConnection::encryptPrepared(const rtc::CopyOnWriteBuffer &buffer)
-> EncryptedPacket {
	auto result = EncryptedPacket();
	result.counter = CounterFromSeq(ReadSeq(buffer.data()));
	result.bytes.resize(16 + buffer.size());

	const auto x = (_key.isOutgoing ? 0 : 8) + (_type == Type::Signaling ? 128 : 0);
	const auto key = _key.value->data();

	const auto msgKeyLarge = ConcatSHA256(
		MemorySpan{ key + 88 + x, 32 },
		MemorySpan{ buffer.data(), buffer.size() });
	const auto msgKey = result.bytes.data();
	memcpy(msgKey, msgKeyLarge.data() + 8, 16);

	auto aesKeyIv = PrepareAesKeyIv(key, msgKey, x);

	AesProcessCtr(
		MemorySpan{ buffer.data(), buffer.size() },
		result.bytes.data() + 16,
		std::move(aesKeyIv));

	return result;
}

bool EncryptedConnection::registerIncomingCounter(uint32_t incomingCounter) {
	auto &list = _largestIncomingCounters;

	const auto position = std::lower_bound(list.begin(), list.end(), incomingCounter);
	const auto largest = list.empty() ? 0 : list.back();
	if (position != list.end() && *position == incomingCounter) {
		// The packet is in the list already.
		return false;
	} else if (incomingCounter + kKeepIncomingCountersCount <= largest) {
		// The packet is too old.
		return false;
	}
	const auto eraseTill = std::find_if(list.begin(), list.end(), [&](uint32_t counter) {
		return (counter + kKeepIncomingCountersCount <= incomingCounter);
	});
	const auto eraseCount = eraseTill - list.begin();
	const auto positionIndex = (position - list.begin()) - eraseCount;
	list.erase(list.begin(), eraseTill);

	assert(positionIndex >= 0 && positionIndex <= list.size());
	list.insert(list.begin() + positionIndex, incomingCounter);
	return true;
}

auto EncryptedConnection::handleIncomingPacket(const char *bytes, size_t size)
-> absl::optional<DecryptedPacket> {
	if (size < 21 || size > kMaxIncomingPacketSize) {
		return LogError("Bad incoming packet size: ", std::to_string(size));
	}

	const auto x = (_key.isOutgoing ? 8 : 0) + (_type == Type::Signaling ? 128 : 0);
	const auto key = _key.value->data();
	const auto msgKey = reinterpret_cast<const uint8_t*>(bytes);
	const auto encryptedData = msgKey + 16;
	const auto dataSize = size - 16;

	auto aesKeyIv = PrepareAesKeyIv(key, msgKey, x);

	auto decryptionBuffer = rtc::Buffer(dataSize);
	AesProcessCtr(
		MemorySpan{ encryptedData, dataSize },
		decryptionBuffer.data(),
		std::move(aesKeyIv));

	const auto msgKeyLarge = ConcatSHA256(
		MemorySpan{ key + 88 + x, 32 },
		MemorySpan{ decryptionBuffer.data(), decryptionBuffer.size() });
	if (memcmp(msgKeyLarge.data() + 8, msgKey, 16)) {
		return LogError("Bad incoming data hash.");
	}

	const auto incomingSeq = ReadSeq(decryptionBuffer.data());
	const auto messageRequiresAck = ((incomingSeq & kMessageRequiresAckSeqBit) != 0);
	const auto incomingCounter = CounterFromSeq(incomingSeq);
	if (!registerIncomingCounter(incomingCounter)) {
		// We've received that packet already.
		return LogError("Already handled packet received.", std::to_string(incomingCounter));
	}

	if (messageRequiresAck) {
		_acksToResend.push_back(incomingSeq); // TODO schedule send
	}
	return processPacket(decryptionBuffer, incomingSeq);
}

auto EncryptedConnection::processPacket(
	const rtc::Buffer &fullBuffer,
	uint32_t packetSeq)
-> absl::optional<DecryptedPacket> {
	assert(fullBuffer.size() >= 5);

	auto index = 0;
	auto currentSeq = packetSeq;
	rtc::ByteBufferReader reader(
		reinterpret_cast<const char*>(fullBuffer.data() + 4), // Skip seq.
		fullBuffer.size() - 4);

	auto result = absl::optional<DecryptedPacket>();
	while (true) {
		const auto type = uint8_t(*reader.Data());
		const auto singleMessagePacket = ((currentSeq & kSingleMessagePacketSeqBit) != 0);
		if (singleMessagePacket && index > 0) {
			return LogError("Single message packet bit in not first message.");
		}
		if (type == kEmptyId) {
			reader.Consume(1);
		} else if (type == kAckId) {
			ackMyMessage(currentSeq);
			reader.Consume(1);
		} else if (auto message = DeserializeMessage(reader, singleMessagePacket)) {
			auto decrypted = DecryptedMessage{
				std::move(*message),
				CounterFromSeq(currentSeq)
			};
			if (result) {
				result->additional.push_back(std::move(decrypted));
			} else {
				result = DecryptedPacket{ std::move(decrypted) };
			}
		} else {
			return LogError("Could not parse message from packet, type: ", std::to_string(type));
		}
		if (!reader.Length()) {
			break;
		} else if (singleMessagePacket) {
			return LogError("Single message didn't fill the entire packet.");
		} else if (reader.Length() < 5) {
			return LogError("Bad remaining data size: ", std::to_string(reader.Length()));
		}
		const auto success = reader.ReadUInt32(&currentSeq);
		++index;
		assert(success);
	}
	return result;
}

void EncryptedConnection::ackMyMessage(uint32_t seq) {
	auto &list = _myNotYetAckedMessages;
	for (auto i = list.begin(), e = list.end(); i != e; ++i) {
		if (ReadSeq(i->data.cdata()) == seq) {
			list.erase(i);
			return;
		}
	}
}

} // namespace tgcalls
