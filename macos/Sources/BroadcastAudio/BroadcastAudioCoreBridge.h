#ifndef BROADCAST_AUDIO_CORE_BRIDGE_H
#define BROADCAST_AUDIO_CORE_BRIDGE_H

#include "BroadcastAudioCore.h"
#include <TgVoipWebrtc/BroadcastAudioCoreC.h>

#include <memory>

// C++ only: the core behind a C handle, for building playout observers (the iOS adapter, the testbench runner).
std::shared_ptr<broadcast_audio::Core> BroadcastAudioCoreShared(BroadcastAudioCoreRef _Nonnull core);

#endif
