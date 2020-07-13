#ifndef TGCALLS_SIGNALING_MESSAGE_H
#define TGCALLS_SIGNALING_MESSAGE_H

#include "api/candidate.h"
#include "api/video_codecs/sdp_video_format.h"
#include "absl/types/variant.h"
#include "absl/types/optional.h"

#include <vector>

namespace tgcalls {

struct SwitchToVideoMessage {
	static constexpr uint8_t kId = 1;
};

struct RemoteVideoIsActiveMessage {
	static constexpr uint8_t kId = 2;

	bool active = false;
};

struct CandidatesListMessage {
	static constexpr uint8_t kId = 3;

	std::vector<cricket::Candidate> candidates;
};

struct VideoFormatsMessage {
	static constexpr uint8_t kId = 4;

	std::vector<webrtc::SdpVideoFormat> formats;
	int encodersCount = 0;
};

// To add a new message you should:
// 1. Add the message struct.
// 2. Add the message to the variant in SignalingMessage struct.
// 3. Add Serialize/Deserialize methods in SignalingMessage module.
// 4. Add TryDeserialize call in DeserializeMessage method.

struct SignalingMessage {
	absl::variant<
		SwitchToVideoMessage,
		RemoteVideoIsActiveMessage,
		CandidatesListMessage,
		VideoFormatsMessage> data;
};

std::vector<uint8_t> SerializeMessage(const SignalingMessage &message);
absl::optional<SignalingMessage> DeserializeMessage(const std::vector<uint8_t> &data);

} // namespace tgcalls

#endif
