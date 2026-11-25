#pragma once

#include <set>
#include <string>

#include "group_participant.h"

// A group call split over two processes (--split, split_group.h): `role` "host" runs the Go SFU and its participants,
// "join" only participants (ids from `participantIdOffset`), whose SFU calls go to the host over `socketPath`. Empty
// `role`: the whole call in this process.
struct GroupSplitOptions {
    std::string role;
    std::string socketPath;
    int participantIdOffset = 0;
};

int runGroupMode(int customParticipants, int referenceParticipants, int duration, bool quiet, bool video, const std::string& networkScenario = "", const std::set<int>& mutedParticipants = {}, bool earlyVideoRequest = false, bool videoSinkChurn = false, bool e2e = false, VideoFeed videoFeed = VideoFeed::Source, bool requestOwnVideo = false, int unmuteAfterSeconds = 0, bool videoRerequest = false, const GroupSplitOptions& split = GroupSplitOptions());
