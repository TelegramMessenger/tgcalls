#ifndef TGCALLS_ENCRYPTED_CONNECTION_H
#define TGCALLS_ENCRYPTED_CONNECTION_H

#include "Instance.h"
#include "Message.h"

namespace rtc {
class ByteBufferReader;
} // namespace rtc

namespace tgcalls {

class EncryptedConnection final {
public:
	enum class Type : uint8_t {
		Signaling,
		Transport,
	};
	EncryptedConnection(Type type, const EncryptionKey &key);

	struct EncryptedPacket {
		std::vector<uint8_t> bytes;
		uint32_t counter = 0;
	};
	absl::optional<EncryptedPacket> prepareForSending(const Message &message);

	struct DecryptedPacket {
		DecryptedMessage main;
		std::vector<DecryptedMessage> additional;
	};
	absl::optional<DecryptedPacket> handleIncomingPacket(const char *bytes, size_t size);

	static constexpr uint8_t kAckId = uint8_t(-1);
	static constexpr uint8_t kEmptyId = uint8_t(-2);

private:
	struct MessageForResend {
		rtc::CopyOnWriteBuffer data;
		// TODO when was last sent? for timer
	};

	bool enoughSpaceInPacket(const rtc::CopyOnWriteBuffer &buffer, size_t amount) const;
	void appendAcksToResend(rtc::CopyOnWriteBuffer &buffer);
	size_t packetLimit() const;
	size_t fullNotAckedLength() const;
	void appendAdditionalMessages(rtc::CopyOnWriteBuffer &buffer);
	EncryptedPacket encryptPrepared(const rtc::CopyOnWriteBuffer &buffer);
	bool registerIncomingCounter(uint32_t incomingCounter);
	absl::optional<DecryptedPacket> processPacket(const rtc::Buffer &fullBuffer, uint32_t packetSeq);
	void ackMyMessage(uint32_t counter);

	Type _type = Type();
	EncryptionKey _key;
	uint32_t _counter = 0;
	uint32_t _maxIncomingCounter = 0;
	std::vector<uint32_t> _largestIncomingCounters;
	std::vector<uint32_t> _ackedIncomingCounters;
	std::vector<uint32_t> _acksToResend;
	std::vector<MessageForResend> _myNotYetAckedMessages;

};

} // namespace tgcalls

#endif
