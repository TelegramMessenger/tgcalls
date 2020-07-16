#ifndef TGCALLS_SIGNALING_MESSAGE_H
#define TGCALLS_SIGNALING_MESSAGE_H

#include "api/candidate.h"
#include "api/video_codecs/sdp_video_format.h"
#include "absl/types/variant.h"
#include "absl/types/optional.h"
#include "rtc_base/copy_on_write_buffer.h"

#include <vector>

namespace tgcalls {

struct CandidatesListMessage {
	static constexpr uint8_t kId = 1;

	std::vector<cricket::Candidate> candidates;
};

struct VideoFormatsMessage {
	static constexpr uint8_t kId = 2;

	std::vector<webrtc::SdpVideoFormat> formats;
	int encodersCount = 0;
};

struct RequestVideoMessage {
    static constexpr uint8_t kId = 3;
};

struct RemoteVideoIsActiveMessage {
    static constexpr uint8_t kId = 4;

    bool active = false;
};

// To add a new message you should:
// 1. Add the message struct.
// 2. Add the message to the variant in SignalingMessage struct.
// 3. Add Serialize/Deserialize methods in SignalingMessage module.

struct SignalingMessage {
	absl::variant<
		CandidatesListMessage,
		VideoFormatsMessage,
        RequestVideoMessage,
        RemoteVideoIsActiveMessage> data;
};

rtc::CopyOnWriteBuffer SerializeMessage(const SignalingMessage &message);
absl::optional<SignalingMessage> DeserializeMessage(const rtc::CopyOnWriteBuffer &data);

} // namespace tgcalls

#endif
