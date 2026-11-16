#ifndef TGCALLS_SCREENCAST_SEND_PARAMETERS_H
#define TGCALLS_SCREENCAST_SEND_PARAMETERS_H

#include <string>
#include <vector>

// The private-call screen-capture changes are iOS's (screen captures sent as video on 10/11, 14 and
// 18/19, and the 13.0.0 screencast channel's H264-first order and 2 Mbit/s / 60 fps caps): tgcalls is
// shared with Desktop and Android, whose screencasts stay as they were. The CLI (TGCALLS_ALLOW_TEST_KNOBS)
// builds them too, so its --video-screencast checks exercise them on the host.
#if defined(WEBRTC_IOS) || (defined(TGCALLS_ALLOW_TEST_KNOBS) && TGCALLS_ALLOW_TEST_KNOBS)
#define TGCALLS_IOS_PRIVATE_CALL_SCREENCAST 1
#else
#define TGCALLS_IOS_PRIVATE_CALL_SCREENCAST 0
#endif

namespace tgcalls {

// See TGCALLS_IOS_PRIVATE_CALL_SCREENCAST.
constexpr bool kIosPrivateCallScreencast = TGCALLS_IOS_PRIVATE_CALL_SCREENCAST != 0;

// Send parameters of one simulcast layer of the iOS screencast (the in-app
// ScreenCaptureKit capture). Plain values, applied to webrtc::RtpEncodingParameters
// by the engines.
struct ScreencastLayerParameters {
    int minBitrateBps = 0;
    int maxBitrateBps = 0;
    double scaleResolutionDownBy = 1.0;
    int maxFramerate = 0;
};

// The iOS screencast's layers, lowest first, for frames of frameWidth x frameHeight; every
// layer up to 60 fps. The screen is encoded as video, at sizes picked by pixels, not by a
// fixed factor (a phone's screen runs from 2.5 to 5.7 Mpx):
// - the top (or only) layer at most 1 Mpx, capped at 2 Mbit/s (about the camera's bits per
//   pixel, 720p at 0.9 Mbit/s and 30 fps, at 60 fps);
// - the low layer at 240 lines on its short side (240p), the middle one (3 layers) at 480;
// - never upscaled.
// The lower layers' bitrates are the ones the screencast had before. A frame size of 0 (not
// known yet) leaves the top layer unscaled and the lower ones at 1/2 (and 1/4). layerCount
// outside 1...3 gives one layer.
std::vector<ScreencastLayerParameters> iosScreencastLayerParameters(int layerCount, int frameWidth = 0, int frameHeight = 0);

// The field trials of the in-app screencast's connections on the reference engine (its PeerConnection
// factory's own FieldTrialsView): for "WebRTC-Bwe-ProbingConfiguration", periodic ALR probes stop once the
// estimate is within 10% of the cap. Empty for any other key. Both engines turn periodic ALR probing on
// for these connections (cricket::MediaConfig::video.periodic_alr_bandwidth_probing): a still screen sends
// well below its estimate, and without probes an estimate that a failed start-up probe or one delay spike
// left low stays low for the whole share. CustomImpl's field trials are the process's, so it probes at
// the cap as well (about 4-6 KB every 5 s while the screen is still). Test: screencast_probing_test.
std::string iosScreencastFieldTrial(std::string const &key);

// The iOS screencast's total send cap: the 2 Mbit/s video cap plus the audio
// headroom the group engine always added (32 kbit/s).
int iosScreencastTotalMaxBitrateBps();

// An H264 profile-level-id (six hex digits) with its level raised to 5.2, the profile and constraint
// flags kept; a higher level, or anything else, unchanged. The hardware encoder caps its frame rate by
// the level (GetMaxSampleRate(level) / frame size), and the device table's High 4.1 gives ~19 fps at
// screen size; 5.2 allows ~166.
std::string iosScreencastH264ProfileLevelId(std::string const &profileLevelId);

} // namespace tgcalls

#endif
