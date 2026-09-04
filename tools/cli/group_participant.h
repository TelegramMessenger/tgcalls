#pragma once

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "group/GroupInstanceCustomImpl.h"
#include "group/GroupInstanceImpl.h"
#include "group/GroupInstanceReferenceImpl.h"
#include "FakeAudioDeviceModule.h"
#include "StaticThreads.h"
#include "AudioFrame.h"
#include "fake_video_source.h"
#include "fake_video_sink.h"

// CGo header
#include "submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/go_sfu.h"

// ---------------------------------------------------------------------------
// Logging helpers
// ---------------------------------------------------------------------------

extern std::chrono::steady_clock::time_point gGroupStartTime;
extern std::atomic<bool> gGroupQuiet;

double groupElapsed();
void groupLog(const char* tag, const char* fmt, ...);

// ---------------------------------------------------------------------------
// GroupSineRecorder - generates 440 Hz sine tone
// ---------------------------------------------------------------------------

class GroupSineRecorder : public tgcalls::FakeAudioDeviceModule::Recorder {
public:
    GroupSineRecorder();
    tgcalls::AudioFrame Record() override;
    int32_t WaitForUs() override;

private:
    static constexpr size_t kSampleRate = 48000;
    static constexpr size_t kChannels = 2;
    static constexpr size_t kFrameSamples = 480;
    static constexpr double kFrequency = 440.0;
    static constexpr double kAmplitude = 3000.0;

    std::vector<int16_t> buffer_;
    uint64_t phase_ = 0;
};

// ---------------------------------------------------------------------------
// GroupNoOpRenderer - discards received audio
// ---------------------------------------------------------------------------

class GroupNoOpRenderer : public tgcalls::FakeAudioDeviceModule::Renderer {
public:
    bool Render(const tgcalls::AudioFrame&) override;
};

// ---------------------------------------------------------------------------
// SimpleRequestMediaChannelDescriptionTask
// ---------------------------------------------------------------------------

class SimpleRequestMediaChannelDescriptionTask : public tgcalls::RequestMediaChannelDescriptionTask {
public:
    void cancel() override;
};

// ---------------------------------------------------------------------------
// ParticipantState
// ---------------------------------------------------------------------------

struct ParticipantState {
    int id;
    bool isReference;
    bool muted{false};
    std::unique_ptr<tgcalls::GroupInstanceInterface> instance;
    std::atomic<bool> connected{false};
    std::atomic<bool> wasConnected{false};
    std::atomic<bool> receivedAudio{false};
    uint32_t audioSsrc{0};
    std::string logPath;

    // Per-source-SSRC max audio level observed via audioLevelsUpdated.
    std::mutex audioLevelsMutex;
    std::map<uint32_t, float> maxAudioLevelPerSsrc;

    // Video fields
    std::string endpointId;
    rtc::scoped_refptr<FakeVideoTrackSource> videoSource;
    std::mutex videoSinksMutex;
    std::map<std::string, std::shared_ptr<FakeVideoSink>> videoSinks;
    // Every endpoint this participant has requested video from, in request
    // order. `setRequestedVideoChannels` takes the FULL set (both engines drop
    // endpoints missing from the list), so each call resends this accumulated
    // list rather than only the newly discovered entries. Guarded by
    // `videoSinksMutex`.
    std::vector<tgcalls::VideoChannelDescription> requestedVideoChannels;
};

// ---------------------------------------------------------------------------
// GroupValidationResult
// ---------------------------------------------------------------------------

struct GroupValidationResult {
    int totalParticipants;
    int connectedCount;
    int audioReceivedCount;
    int videoReceivedPairs;
    int videoExpectedPairs;
    bool success;
};

// ---------------------------------------------------------------------------
// Participant lifecycle functions
// ---------------------------------------------------------------------------

// Creates a fully initialized participant: builds descriptor, creates instance,
// joins SFU, sets join response, unmutes (unless `muted=true`). Returns nullptr on failure.
//
// With `earlyVideoRequest`, video from every already-joined participant is
// requested right after the join payload is requested — before the join
// response is applied and before the data channel opens. That is the real
// app's call order: `PresentationGroupCall` calls `setRequestedVideoChannels`
// for the participants it already knows about immediately after the context
// issues `emitJoinPayload`. Without the flag, video is requested only once the
// SFU announces `ActiveVideoSsrcs` over the (already open) data channel.
std::unique_ptr<ParticipantState> createParticipant(
    int id,
    bool isReference,
    GoInt sfuHandle,
    std::shared_ptr<tgcalls::Threads> threads,
    bool quiet,
    bool video,
    std::vector<std::unique_ptr<ParticipantState>>* allStates,
    bool muted = false,
    bool earlyVideoRequest = false
);

// Replaces every participant's incoming video sinks: for each endpoint a
// fresh FakeVideoSink is registered via addIncomingVideoOutput and the old
// one is released. That is what the app does on a quality switch (the tile's
// view — which owns the sink — is recreated), so an engine that keeps a raw
// pointer to a dead sink crashes on the next decoded frame. Validation then
// runs against the NEW sinks, which must still receive frames.
void churnVideoSinks(const std::vector<std::unique_ptr<ParticipantState>>& states);

// Requests video from `endpointIds` (endpoints already requested are skipped):
// registers a FakeVideoSink per new endpoint, resolves the sender's simulcast
// SSRC groups from the SFU registry, and resends the accumulated full request
// set via `setRequestedVideoChannels`.
void requestVideoFromEndpoints(
    ParticipantState* state,
    GoInt sfuHandle,
    const std::vector<std::string>& endpointIds
);

// Clean teardown: GoSfu_Leave, stop video, stop instance, reset.
void stopParticipant(ParticipantState* state, GoInt sfuHandle);

// Validates group state: connection, audio, video. Returns result struct.
GroupValidationResult validateGroupState(
    const std::vector<std::unique_ptr<ParticipantState>>& states,
    bool video
);

// Prints a group call summary to stdout. Returns the success boolean.
bool printGroupSummary(
    int customParticipants,
    int referenceParticipants,
    int duration,
    bool video,
    const GroupValidationResult& result,
    bool anyFailed
);
