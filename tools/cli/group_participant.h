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
#include <thread>
#include <vector>

#if TGCALLS_ENABLE_CUSTOM_GROUP_ENGINE
#include "group/GroupInstanceCustomImpl.h"
#endif
#include "group/GroupInstanceImpl.h"
#include "group/GroupInstanceReferenceImpl.h"
#include "group/GroupFrameTransformer.h"
#include "FakeAudioDeviceModule.h"
#include "StaticThreads.h"
#include "VideoCaptureInterface.h"
#include "AudioFrame.h"
#include "fake_video_source.h"
#include "fake_video_sink.h"
#include "broadcast_signals.h"

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
// GroupSineRecorder - generates a sine tone: 440 Hz by default, or a broadcast testbench speaker's tone (fixed or
// the time-coded marker, whose pitch follows the wall clock at each sample)
// ---------------------------------------------------------------------------

class GroupSineRecorder : public tgcalls::FakeAudioDeviceModule::Recorder {
public:
    explicit GroupSineRecorder(broadcast::SpeakerTone tone = broadcast::SpeakerTone::fixed(440.0));
    tgcalls::AudioFrame Record() override;
    int32_t WaitForUs() override;

private:
    static constexpr size_t kSampleRate = 48000;
    static constexpr size_t kChannels = 2;
    static constexpr size_t kFrameSamples = 480;
    static constexpr double kAmplitude = 3000.0;

    broadcast::SpeakerTone tone_;
    broadcast::RealTimePacer pacer_;
    std::vector<int16_t> buffer_;
    double phase_ = 0.0;
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
// Fake end-to-end crypto (--e2e)
// ---------------------------------------------------------------------------

// Reversible stand-in for the app's conference crypto.
//
// Keyed per user id: encrypt uses our own `ownUserId` key (the engines
// always pass userId 0 on encrypt, exactly as the app does), decrypt uses the
// key of the userId the engine resolved for that SSRC. An engine that resolves
// the wrong userId therefore fails the tag check and the frame is dropped,
// which is what puts the ssrc->userId plumbing genuinely under test.
//
// The nonce is random per call so ciphertext differs between attempts, which is
// what makes GroupFrameTransformer's four-attempt validation retry meaningful.
// Participant id -> the user id the engines see. Offset by one so that no real
// participant maps to 0, which both engines use to mean "sender not yet known"
// (CustomImpl passes int64_t() for its encryptors, and the reference engine's
// registry returns 0 for an unresolved SSRC). Without the offset, participant 0's
// key would decrypt frames whose owner the engine failed to resolve, and the
// --e2e run would pass while the ssrc->userId plumbing was broken.
//
// Production has the same property for free: Telegram peer ids are never 0.
inline int64_t cliUserId(int participantId) { return (int64_t)participantId + 1; }

tgcalls::GroupEncryptDecryptFunction makeFakeE2eTransform(int64_t ownUserId);

// ---------------------------------------------------------------------------
// VideoFeed — how a participant hands its outgoing video to the engine
// ---------------------------------------------------------------------------

enum class VideoFeed {
    // descriptor.getVideoSource: the CLI's original shortcut. No app uses it.
    Source,
    // descriptor.videoCapture: the iOS wrapper's join-time path (the camera is
    // already on when the call context is created).
    CaptureAtJoin,
    // setVideoCapture() after the join response: the iOS wrapper's
    // requestVideo: path (the camera is switched on mid-call).
    CaptureLate,
};

// ---------------------------------------------------------------------------
// ParticipantState
// ---------------------------------------------------------------------------

// Group screen sharing in the harness (main.cpp sets it before any participant exists).
struct GroupScreencastOptions {
    // --screencast-participants: these participants share a screen (the in-app ScreenCaptureKit share's instance:
    // VideoContentType::Screencast, iosInAppScreencast, a 60 fps source, screen audio through addExternalAudioSamples)
    // instead of sending a camera.
    std::set<int> sharers;
    // --sfu-av1: the SFU's join responses list AV1 (GoSfu_SetListAv1).
    bool sfuListsAv1 = false;
    // --sfu-dd-id ID: the SFU's join responses list the AV1 Dependency Descriptor at ID (GoSfu_SetDependencyDescriptorId).
    int sfuDependencyDescriptorId = 0;
    // --av1-screencast: GroupInstanceDescriptor::allowAv1ScreencastSending on every participant.
    bool allowAv1 = false;
    // --av1-screencast-layers single|top|every: GroupInstanceDescriptor::av1ScreencastLayers (every, as in the app).
    tgcalls::GroupAv1ScreencastLayers av1Layers = tgcalls::GroupAv1ScreencastLayers::Every;
    // --av1-test-encoder-fails: the sharers' AV1 encoder fails to initialise (av1TestEncoderFails).
    bool av1TestEncoderFails = false;
    // --video-quality-switch MS: every MS each receiver alternates its request for the sharers between thumbnail and
    // full (their low H264 layer and their top layer); validation requires frames after every switch.
    int qualitySwitchMs = 0;
    // --sharer-ingress-loss RATE: packets from the sharers dropped on the way into the SFU.
    double sharerIngressLoss = 0.0;
};
extern GroupScreencastOptions gGroupScreencast;

struct ParticipantState {
    ~ParticipantState();

    int id;
    bool isReference;
    bool muted{false};
    // --unmute-after: this participant joined muted and was unmuted mid-call,
    // after the other engines had already renegotiated for the SSRCs they
    // discovered at join. `muted` is cleared at that point; this stays set so
    // validation can require every peer to actually hear it.
    bool unmutedLate{false};
    std::unique_ptr<tgcalls::GroupInstanceInterface> instance;
    std::atomic<bool> connected{false};
    std::atomic<bool> wasConnected{false};
    std::atomic<bool> receivedAudio{false};
    uint32_t audioSsrc{0};
    // Read from the SFU before teardown: audio RTP packets it received on
    // `audioSsrc`, -1 if never queried (group-churn). A participant that
    // stayed muted for the whole run must have sent none — mute has to stop
    // the stream, not merely silence it.
    std::atomic<int64_t> sfuAudioPacketsReceived{-1};
    std::string logPath;

    // Per-source-SSRC max audio level observed via audioLevelsUpdated.
    std::mutex audioLevelsMutex;
    std::map<uint32_t, float> maxAudioLevelPerSsrc;

    // Video fields
    bool sendsVideo{false};
    // --screencast-participants: this participant shares a screen. Its screen audio (a 440 Hz tone) goes in through
    // addExternalAudioSamples from `externalAudioThread` until stopScreencastAudio.
    bool sharesScreen{false};
    std::atomic<bool> externalAudioRunning{false};
    std::thread externalAudioThread;
    std::string endpointId;
    // Exactly one of these is set for a video participant, per VideoFeed.
    webrtc::scoped_refptr<FakeVideoTrackSource> videoSource;
    std::shared_ptr<tgcalls::VideoCaptureInterface> videoCapture;
    // True for the Capture* feeds; outlives `videoCapture`, which teardown
    // releases before validation runs.
    bool videoViaCapture{false};
    // --request-own-video: include this participant's own endpoint in the
    // requested set, as the app does (its local tile asks like any other).
    bool requestOwnVideo{false};
    // Read from the SFU before teardown: the layer this participant asked the
    // SFU to forward from ITSELF, -1 if it never did. Must stay -1.
    std::atomic<int> sfuSelfRequestedLayer{-1};
    std::mutex videoSinksMutex;
    std::map<std::string, std::shared_ptr<FakeVideoSink>> videoSinks;
    // Every endpoint this participant has requested video from, in request
    // order. `setRequestedVideoChannels` takes the FULL set (both engines drop
    // endpoints missing from the list), so each call resends this accumulated
    // list rather than only the newly discovered entries. Guarded by
    // `videoSinksMutex`.
    std::vector<tgcalls::VideoChannelDescription> requestedVideoChannels;
    // --video-rerequest: frame count of every incoming sink just before the
    // dropped endpoints were requested again (a second after the drop, sinks
    // quiet). Validation requires each sink to have grown past this on the
    // same PeerConnection. Guarded by `videoSinksMutex`.
    std::map<std::string, int> videoFramesAtRerequest;
    bool videoRerequested{false};

    // Broadcast testbench listener: false once the listener is torn down, so fetches still in flight never call
    // into its engine (stopParticipant clears it before the instance stops).
    std::shared_ptr<std::atomic<bool>> broadcastAlive;
};

// ---------------------------------------------------------------------------
// The SFU, here or in the other process of a split call (--split)
// ---------------------------------------------------------------------------

// A participant of the other process of a split group call: what this process's SSRC lookups and validation need to
// know about it.
struct SplitParticipantInfo {
    int id = 0;
    bool isReference = false;
    uint32_t audioSsrc = 0;
    bool muted = false;
    bool sharesScreen = false;
    bool sendsVideo = false;
    std::string endpointId;
};

// The other process's participants: the host adds the joiner's as they join, the joiner the host's from its hello.
// Empty when the call runs in one process.
void addRemoteParticipant(const SplitParticipantInfo& info);
std::vector<SplitParticipantInfo> remoteParticipants();

// The host process's SFU, for a joining process (--split join): every SFU call below goes there instead of to the
// in-process Go SFU once it is set.
class RemoteSfu {
public:
    virtual ~RemoteSfu() = default;
    // The join response, empty on failure. `info` tells the host who joined.
    virtual std::string join(int participantId, const std::string& payload, bool iceControlling,
                             const SplitParticipantInfo& info) = 0;
    virtual int64_t querySsrc(uint32_t ssrc) = 0;
    virtual std::string queryVideoSsrcs(int participantId) = 0;
    virtual int queryRequestedLayer(int receiverId, int senderId) = 0;
    virtual int64_t querySsrcPackets(uint32_t ssrc) = 0;
    virtual std::string queryPayloadTypeStats(int senderId) = 0;
    virtual void setNetworkParams(int participantId, int direction, int delayMs, int jitterMs, double dropRate,
                                  int64_t bandwidthBps) = 0;
};
void setRemoteSfu(std::shared_ptr<RemoteSfu> sfu);

// The SFU calls group mode and the participants make: to the in-process Go SFU (`handle`), or to the remote one.
// Strings are empty where the Go SFU returns null.
std::string sfuJoin(GoInt handle, int participantId, const std::string& payload, bool iceControlling,
                    const SplitParticipantInfo& info);
int64_t sfuQuerySsrc(GoInt handle, uint32_t ssrc);
std::string sfuQueryVideoSsrcs(GoInt handle, int participantId);
int sfuQueryRequestedLayer(GoInt handle, int receiverId, int senderId);
int64_t sfuQuerySsrcPackets(GoInt handle, uint32_t ssrc);
std::string sfuQueryPayloadTypeStats(GoInt handle, int senderId);
void sfuSetNetworkParams(GoInt handle, int participantId, int direction, int delayMs, int jitterMs, double dropRate,
                         int64_t bandwidthBps);

// This process's participant as the other process of a split call sees it.
SplitParticipantInfo splitParticipantInfo(const ParticipantState& state);

// ---------------------------------------------------------------------------
// GroupValidationResult
// ---------------------------------------------------------------------------

struct GroupValidationResult {
    int totalParticipants;
    int connectedCount;
    // Participants that heard audio from someone, over those expected to: every participant but a screen sharer (the
    // app's share connection plays no audio and the app reads no levels from it: PresentationGroupCall takes them from
    // the main connection; the reference engine's levels come from playout, which a share connection has none of).
    int audioReceivedCount;
    int audioListeners;
    int videoReceivedPairs;
    int videoExpectedPairs;
    // Participants the SFU saw requesting their own video (must be 0).
    int selfVideoRequests;
    // Participants feeding video through VideoCaptureInterface whose sink for
    // their OWN endpoint received no frames (must be 0): the engine has to
    // serve that sink from the camera preview, the way the app's local tile
    // expects, not from a network channel.
    int ownPreviewMissing;
    // --unmute-after: (peer, late unmuter) pairs where the peer reported a
    // non-trivial level for the late unmuter's SSRC, over the pairs expected.
    int lateUnmuteHeardPairs;
    int lateUnmuteExpectedPairs;
    // --video-rerequest: (receiver, endpoint) pairs that were dropped and
    // requested again, and how many of them received no frame afterwards
    // (must be 0).
    int videoRerequestExpectedPairs;
    int videoRerequestStalledPairs;
    // Every (peer, unmuted sender) pair, and how many of them the peer reported
    // at a non-trivial level. A sender that is heard but never reaches its
    // peers' level reports is the "audible but never shown speaking" defect.
    int audioLevelHeardPairs;
    int audioLevelExpectedPairs;
    // "Creating unsignaled receive stream" lines in the engine log (must be 0):
    // the reference engine must receive audio only through m-lines that signal
    // the SSRC, never through a catch-all stream that WebRTC created on its own.
    int unsignaledAudioStreams;
    // Participants that stayed muted for the whole run.
    int mutedParticipants;
    // Of those, the ones the SFU still received audio RTP from (must be 0).
    int mutedAudioLeaks;
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
//
// `videoFeed` picks how outgoing video reaches the engine (see VideoFeed); the
// two Capture* feeds go through the platform's VideoCaptureInterface exactly
// as the iOS wrapper does. `requestOwnVideo` puts the participant's own
// endpoint in every requested-video set, as the app does.
std::unique_ptr<ParticipantState> createParticipant(
    int id,
    bool isReference,
    GoInt sfuHandle,
    std::shared_ptr<tgcalls::Threads> threads,
    bool quiet,
    bool video,
    std::vector<std::unique_ptr<ParticipantState>>* allStates,
    bool muted = false,
    bool earlyVideoRequest = false,
    bool e2e = false,
    VideoFeed videoFeed = VideoFeed::Source,
    bool requestOwnVideo = false,
    broadcast::SpeakerTone tone = broadcast::SpeakerTone::fixed(440.0)
);

// Replaces every participant's incoming video sinks: for each endpoint a
// fresh FakeVideoSink is registered via addIncomingVideoOutput and the old
// one is released. That is what the app does on a quality switch (the tile's
// view — which owns the sink — is recreated), so an engine that keeps a raw
// pointer to a dead sink crashes on the next decoded frame. Validation then
// runs against the NEW sinks, which must still receive frames.
void churnVideoSinks(const std::vector<std::unique_ptr<ParticipantState>>& states);

// --video-rerequest, step 1: every participant asks the engine for NO video
// (`setRequestedVideoChannels({})`) while keeping its sinks, the way the app
// does when the call UI leaves the video grid.
void dropAllVideoRequests(const std::vector<std::unique_ptr<ParticipantState>>& states);

// --video-rerequest, step 2: every participant records each sink's frame
// count and requests the full accumulated set again, on the same
// PeerConnection. Validation then requires every re-requested sink to receive
// frames past that count.
void rerequestAllVideo(const std::vector<std::unique_ptr<ParticipantState>>& states);

// --video-quality: the maxQuality every participant requests, with minQuality
// Thumbnail, which is the shape the app always sends
// (VideoChatParticipantsComponent: `minQuality: .thumbnail`, a grid tile's
// max usually `.thumbnail` too). Not calling it keeps the historical Full/Full
// request, which never exercised the lower qualities: ReferenceImpl mapped a
// thumbnail to a height below the SFU's lowest layer and no run caught it.
void setRequestedVideoMaxQuality(tgcalls::VideoChannelDescription::Quality quality);

// --video-quality-switch: every receiver's request for these endpoints gets `quality` as its max (min thumbnail), and
// the full requested set is sent again.
void setRequestedVideoQualityFor(
    const std::vector<std::unique_ptr<ParticipantState>>& states,
    const std::set<std::string>& endpoints,
    tgcalls::VideoChannelDescription::Quality quality);

// Stops a sharer's screen-audio thread; before its instance stops.
void stopScreencastAudio(ParticipantState* state);

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

// Validates group state: connection, audio, video. Returns result struct. In a split call (--split) the peers are this
// process's participants and the senders every participant of the call, the other process's (remoteParticipants())
// included.
GroupValidationResult validateGroupState(
    const std::vector<std::unique_ptr<ParticipantState>>& states,
    bool video
);

// Prints a group call summary to stdout. Returns the success boolean.
// `split` is the "Split: ..." line's text for a split call (--split), empty otherwise.
bool printGroupSummary(
    int customParticipants,
    int referenceParticipants,
    int duration,
    bool video,
    const GroupValidationResult& result,
    bool anyFailed,
    const std::string& split = std::string()
);
