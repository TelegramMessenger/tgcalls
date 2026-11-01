#pragma once

// A broadcast listener's three request callbacks (GroupInstanceDescriptor::requestCurrentTime,
// requestAudioBroadcastPart, requestVideoBroadcastPart), served by the Go SFU's emulated broadcast server.
//
// Each part fetch runs on its own thread through the blocking GoSfu_BroadcastFetch (the server holds it for the
// listener's link time). A fetch's completion runs only if its task was not cancelled and the listener is still
// alive (`alive`, which the listener's teardown sets false before the engine stops). A join-missing answer calls
// `onRejoinNeeded` instead of the completion, as the app's NetworkBroadcastPartSource does.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "group/GroupInstanceImpl.h"

#include "submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/go_sfu.h"

struct BroadcastRequestCallbacks {
    std::function<std::shared_ptr<tgcalls::BroadcastPartTask>(std::function<void(int64_t)>)> requestCurrentTime;
    std::function<std::shared_ptr<tgcalls::BroadcastPartTask>(int64_t, int64_t, std::function<void(tgcalls::BroadcastPart &&)>)> requestAudioBroadcastPart;
    std::function<std::shared_ptr<tgcalls::BroadcastPartTask>(int64_t, int64_t, int32_t, tgcalls::VideoChannelDescription::Quality, std::function<void(tgcalls::BroadcastPart &&)>)> requestVideoBroadcastPart;
};

// `unified`: an RTMP stream's listener: its time is the newest available part (getGroupCallStreamChannels'
// last_timestamp_ms) and every video request is the unified part at the requested quality. Otherwise the time is
// the server's clock and video requests name channels of the listener-audio parts.
BroadcastRequestCallbacks makeBroadcastRequestCallbacks(GoInt sfuHandle, int clientId, bool unified,
                                                        std::shared_ptr<std::atomic<bool>> alive,
                                                        std::function<void()> onRejoinNeeded);

// The rejoin after a join-missing answer, on the caller's thread, in PresentationGroupCall.requestCall's order:
// `stop` (connection mode None) at once, `resume` (Broadcast again) two round trips later (getCurrentGroupCall, then
// the join). Either is skipped once `alive` is cleared.
void runBroadcastRejoin(std::function<void()> const &stop, std::function<void()> const &resume, int roundTripMs,
                        std::shared_ptr<std::atomic<bool>> const &alive);
