#ifndef TGCALLS_SIGNALING_MESSAGE_H
#define TGCALLS_SIGNALING_MESSAGE_H

#include "api/candidate.h"
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

struct SignalingMessage {
	absl::variant<
		SwitchToVideoMessage,
		RemoteVideoIsActiveMessage,
		CandidatesListMessage> data;
};

std::vector<uint8_t> SerializeMessage(const SignalingMessage &message);
absl::optional<SignalingMessage> DeserializeMessage(const std::vector<uint8_t> &data);

} // namespace tgcalls

#endif
