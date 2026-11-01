#pragma once

// The broadcast testbench's listener: today's engine (GroupInstanceCustomImpl) in broadcast mode, fetching parts
// from the Go SFU's emulated broadcast server, and its promotion to speaker in the app's order.

#include <functional>
#include <memory>

#include "group_participant.h"

struct BroadcastListenerOptions {
    int clientId = 0;
    // An RTMP stream's listener (unified parts, no microphone), as opposed to a large chat's.
    bool unified = false;
    std::shared_ptr<tgcalls::FakeAudioDeviceModule::Renderer> renderer;
    // A part request answered "join missing": the mode rejoins as the app does.
    std::function<void()> onRejoinNeeded;
    // Every audio level update the engine reports.
    std::function<void(tgcalls::GroupLevelsUpdate const &)> onLevels;
};

// Creates the listener without joining the SFU: muted, the request callbacks bound to the server (client
// `options.clientId`, registered by the caller with GoSfu_BroadcastAddClient), audio levels recorded in the state,
// then setConnectionMode(Broadcast, false, options.unified).
std::unique_ptr<ParticipantState> createBroadcastListener(int id, GoInt sfuHandle, std::shared_ptr<tgcalls::Threads> threads,
                                                          BroadcastListenerOptions options);

// Promotes a listener to speaker as PresentationGroupCall does (requestCall(movingFromBroadcastToRtc: true)):
// setConnectionMode(None, keep broadcast) -> emitJoinPayload -> SFU join -> setConnectionMode(Rtc) ->
// setJoinResponsePayload. The listener stays muted. Returns false if the join failed.
bool promoteListenerToSpeaker(ParticipantState *listener, GoInt sfuHandle);

// What the app does on a join-missing answer while in broadcast (PresentationGroupCall.requestCall): leave broadcast
// (connection mode None), and once it has joined again, enter it again. runBroadcastRejoin orders the two.
void stopBroadcast(ParticipantState *listener);
void resumeBroadcast(ParticipantState *listener);
