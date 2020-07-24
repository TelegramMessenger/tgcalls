#ifndef TGCALLS_MESSAGE_H
#define TGCALLS_MESSAGE_H

#include "api/candidate.h"
#include "api/video_codecs/sdp_video_format.h"
#include "absl/types/variant.h"
#include "absl/types/optional.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/byte_buffer.h"

#include <vector>

namespace tgcalls {

struct CandidatesListMessage {
	static constexpr uint8_t kId = 1;
	static constexpr bool kRequiresAck = true;

	std::vector<cricket::Candidate> candidates;
};

struct VideoFormatsMessage {
	static constexpr uint8_t kId = 2;
	static constexpr bool kRequiresAck = true;

	std::vector<webrtc::SdpVideoFormat> formats;
	int encodersCount = 0;
};

struct RequestVideoMessage {
	static constexpr uint8_t kId = 3;
	static constexpr bool kRequiresAck = true;
};

struct RemoteVideoIsActiveMessage {
	static constexpr uint8_t kId = 4;
	static constexpr bool kRequiresAck = true;

	bool active = false;
};

struct AudioDataMessage {
	static constexpr uint8_t kId = 5;
	static constexpr bool kRequiresAck = false;

	rtc::CopyOnWriteBuffer data;
};

struct VideoDataMessage {
	static constexpr uint8_t kId = 6;
	static constexpr bool kRequiresAck = false;

	rtc::CopyOnWriteBuffer data;
};

struct UnstructuredDataMessage {
    static constexpr uint8_t kId = 7;
    static constexpr bool kRequiresAck = true;

    rtc::CopyOnWriteBuffer data;
};

// To add a new message you should:
// 1. Add the message struct.
// 2. Add the message to the variant in Message struct.
// 3. Add Serialize/Deserialize methods in Message module.

struct Message {
	absl::variant<
		CandidatesListMessage,
		VideoFormatsMessage,
        RequestVideoMessage,
        RemoteVideoIsActiveMessage,
		AudioDataMessage,
		VideoDataMessage,
        UnstructuredDataMessage> data;
};

rtc::CopyOnWriteBuffer SerializeMessageWithSeq(
	const Message &message,
	uint32_t seq,
	bool singleMessagePacket);
absl::optional<Message> DeserializeMessage(
	rtc::ByteBufferReader &reader,
	bool singleMessagePacket);

struct DecryptedMessage {
	Message message;
	uint32_t counter = 0;
};

} // namespace tgcalls

#endif
