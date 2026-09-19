#pragma once

#include <set>
#include <string>

#include "group_participant.h"

int runGroupMode(int customParticipants, int referenceParticipants, int duration, bool quiet, bool video, const std::string& networkScenario = "", const std::set<int>& mutedParticipants = {}, bool earlyVideoRequest = false, bool videoSinkChurn = false, bool e2e = false, VideoFeed videoFeed = VideoFeed::Source, bool requestOwnVideo = false, int unmuteAfterSeconds = 0, bool videoRerequest = false);
