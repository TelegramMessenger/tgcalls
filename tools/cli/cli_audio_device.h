#pragma once

#include "FakeAudioDeviceModule.h"

// The fake audio device the CLI's p2p sides and group participants record and play through: 48 kHz stereo. (A screen
// sharer has none: its engine builds its own device for the screen audio.)
//
// It reports echo cancellation, AGC and noise suppression as built in, so WebRTC's voice engine runs without its software
// ones, as in the app, whose shared call audio device reports all three built in (and whose WEBRTC_IOS build keeps echo
// cancellation and AGC off). The host builds of WebRTC (macOS and Linux) would otherwise turn the three on, and noise
// suppression removes the CLI's steady tones (no frame heard at all), so the audio checks would measure WebRTC's
// processing instead of the call. A custom engine's explicit noise suppression request goes to the "built-in" one, as
// in the app.
inline tgcalls::FakeAudioDeviceModule::Options cliAudioDeviceOptions() {
    tgcalls::FakeAudioDeviceModule::Options options;
    options.samples_per_sec = 48000;
    options.num_channels = 2;
    options.reports_builtin_processing = true;
    return options;
}
