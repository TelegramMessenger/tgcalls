# tgcalls CLI Test Tool

In-process test harness for tgcalls. See the root `CLAUDE.md` for build instructions, top-level CLI usage, and the CLI options reference.

## Supported Versions

| Version | Implementation | Notes |
|---|---|---|
| `14.0.0` | `InstanceV2CompatImpl` | WebRTC PeerConnection + V2Impl signaling. Cross-version interop with 7.0.0–13.0.0 |
| `13.0.0` (default) | `InstanceV2Impl` | Also: 7.0.0, 8.0.0, 9.0.0, 12.0.0 |
| `11.0.0` | `InstanceV2ReferenceImpl` | Also: 10.0.0. Uses WebRTC PeerConnection |
| `5.0.0` | `InstanceImpl` (v1) | Also: 2.7.7. Legacy |

## Architecture (P2P/Reflector)
- Two `tgcalls::Instance` objects (caller + callee) created via `Meta::Create(version, ...)`
- Signaling bridged via `SignalingBridge` with configurable drop rate and delay
- Group participant logs (`/tmp/tgcalls_group_p<id>_<pid>.log`) are written at stop and unlinked after validation; set `TGCALLS_CLI_KEEP_LOGS=1` to keep them (added 2026-09-17).
- `--video` in p2p/reflector mode attaches a `tgcalls::VideoCaptureInterface::Create(threads, ...)` to BOTH descriptors (`descriptor.videoCapture`, the iOS join-time path: camera already on when the call is created). Added 2026-09-17 to reproduce the 18/19 start-glare renegotiation loop; a healthy video call shows 4 `SetLocalDescription` in the `--log-file`, no `SetLocalDescription failed`, one `Creating data channel`. Combine with `--delay 100-300` for field-like signaling latency.
- `FakeAudioDeviceModule` with `SineRecorder` (440Hz tone) and `NoOpRenderer` (audio discarded; validation via BWE)
- `FakeInterface` platform implementation (pure C++, no iOS/ObjC deps)
- Stats log validation: both caller and callee write `config.statsLogPath` with bitrate records; non-empty log with at least one non-zero BWE value is a success condition
- On failure, full tgcalls internal logs (caller + callee) are dumped to stdout via `config.logPath`

## Architecture (Group)
- N participants using `GroupInstanceCustomImpl` and/or `GroupInstanceReferenceImpl` connect to an in-process Go SFU
- SFU uses Pion's low-level APIs (pion/ice, pion/dtls, pion/srtp, pion/sctp) — NOT PeerConnection
- ICE: lite mode, loopback-only, UDP host candidates on 127.0.0.1. SFU uses `Dial` (controlling) for CustomImpl clients and `Accept` (controlled) for PeerConnection clients
- DTLS: SFU acts as DTLS client (setup=active); GroupNetworkManager hardcodes SSL_SERVER for the tgcalls client
- SRTP: AES-256-GCM (negotiated via DTLS-SRTP; GroupNetworkManager requires GCM suites)
- SCTP: over DTLS, accepts data channel from client, reads Colibri messages, sends `ActiveVideoSsrcs` notifications
- RTP forwarding: audio RTP forwarded to all others unconditionally; video RTP forwarded only to receivers that have requested video from that sender (via `ReceiverVideoConstraints`)
- SSRC tracking: SFU maintains `ssrcRegistry map[uint32]ssrcInfo` with kind (audio/video/video-rtx) and simulcast layer index, exposed via `GoSfu_QuerySsrc` and `GoSfu_QueryVideoSsrcs` CGo exports
- SSRC discovery: video SSRCs are broadcast via `ActiveVideoSsrcs` (the test app's `dataChannelMessageReceived` callback consumes them and calls `setRequestedVideoChannels`). Audio SSRCs are discovered by `GroupInstanceReferenceImpl`'s per-receiver `GRAudioFrameTransformer` directly from incoming RTP — same shape CustomImpl uses (`receiveUnknownSsrcPacket` → `_requestMediaChannelDescriptions`). The legacy `ActiveAudioSsrcs` data-channel message has been removed.
- Video SSRC groups: parsed from join payload `"ssrc-groups"` field (SIM + FID semantics), stored per participant
- Colibri video constraints: SFU parses `ReceiverVideoConstraints` from receivers, sends `SenderVideoConstraints` back to senders with `idealHeight`, and sends proactive PLI to trigger keyframes when a receiver first requests video
- RTCP feedback: SFU demuxes SRTCP from the shared ICE transport (RFC 5761: byte[1] >= 200 && < 224), decrypts with per-participant SRTCP contexts, parses PLI/FIR, and forwards as new PLI to the sender. NACK is terminated (not forwarded).
- Audio validation: `audioLevelsUpdated` callback tracks remote audio levels; success requires every participant to receive audio from at least one other participant (remote SSRC != 0, level > 0.05). The 440Hz sine tone arrives at ~0.126 level after SFU forwarding.
- Video validation: `FakeVideoSink` (implements `rtc::VideoSinkInterface<VideoFrame>`) counts decoded frames per remote endpoint; success requires every participant to receive ≥1 frame from every other
- Video signaling flow: SFU broadcasts `ActiveVideoSsrcs` over data channel → `dataChannelMessageReceived` callback fires in the app → app calls `setRequestedVideoChannels` → CustomImpl creates `IncomingVideoChannel` / ReferenceImpl adds recvonly video transceiver → both send `ReceiverVideoConstraints` → SFU sends `SenderVideoConstraints` + proactive PLI → sender produces keyframe → receiver decodes. `requestVideoFromEndpoints()` (group_participant.cpp) is the single request path: it accumulates the FULL requested set in `ParticipantState::requestedVideoChannels` and resends all of it on every call, because both engines treat the list as the complete set and drop endpoints missing from it. With `--early-video-request` the same helper runs right after `emitJoinPayload` for every already-joined participant (SSRCs via `GoSfu_QueryVideoSsrcs`), reproducing the real app's order — request before join response, before the data channel — which the `ActiveVideoSsrcs`-driven path never exercises (the broadcast fires 500 ms after the data channel is up)
- `dataChannelMessageReceived` callback: added to `GroupInstanceDescriptor`, forwards all incoming Colibri data channel messages to the application. Used by the CLI test tool to react to `ActiveVideoSsrcs` and dynamically set up video channels — mirrors the real Telegram app's reactive flow
- `FakeAudioDeviceModule` with `SineRecorder` (440Hz tone) and `NoOpRenderer` — same as P2P mode
- `FakeInterface` (`tgcalls/platform/fake/FakeInterface.cpp`) wraps the builtin video encoder/decoder factories so `GetSupportedFormats()` comes back in the iOS platform order — two H264 entries (packetization-mode 1, the constrained-baseline one kept), VP8, VP9 profile 0. PeerConnection assigns dynamic payload types by walking that list (96, 98, 100, ... with RTX at +1), so the ORDER decides which codec sits on which number; the raw builtin host order lists five H264 profiles and lands one on PT 104 by coincidence, which is exactly the number the group-call convention uses for H264 and hid a ReferenceImpl bug for the device (where PT 104 was H265). `--builtin-codec-order` restores the raw order for comparison
- `churnVideoSinks()` (group_participant.cpp, `--video-sink-churn`) replaces every participant's incoming sinks via `addIncomingVideoOutput` and releases the old ones, mirroring the app recreating tile views on quality changes; the engine must only ever hold weak references (a raw sink pointer left on the track crashes on the next frame)
- `FakeVideoTrackSource` generates 1280x720 I420 frames at 30fps with per-participant color tint and frame counter (720p needed for 3 simulcast layers; 640x360 only allows 2 per WebRTC's `kSimulcastFormats`)
- Two ways a participant feeds outgoing video (`VideoFeed` in group_participant.h): `Source` — `descriptor.getVideoSource` returning the `FakeVideoTrackSource` above (the CLI's original shortcut, no app uses it); `CaptureAtJoin` / `CaptureLate` (`--video-via-capture` / `--video-via-capture-late`) — a real `tgcalls::VideoCaptureInterface::Create(threads, "<id>")`, handed over as `descriptor.videoCapture` or via `setVideoCapture()` after the join response, exactly as the iOS wrapper does. For that to produce frames on the host, the fake platform implements `makeVideoSource` (a `FakePlatformVideoSource`: plain adapted source, pushed into) and `makeVideoCapturer` (`FakeVideoCapturer`: 1280x720 @ 30 fps, tint from `deviceId`, a sweeping bar for motion, also feeds the uncropped preview sink) in `tgcalls/platform/fake/FakeVideoCapturer.{h,cpp}`. Both used to return nullptr, which is why `setVideoCapture` could be a no-op in the reference engine without any test noticing.
- `--request-own-video`: `requestVideoFromEndpoints()` appends the participant's own endpoint to every request (the SFU's `ActiveVideoSsrcs` never names the receiver, but the app's roster includes the local participant and is handed to the engine unfiltered) and registers a sink for it. Validation reads `GoSfu_QueryRequestedLayer(receiver, receiver)` before the SFU is destroyed (`ParticipantState::sfuSelfRequestedLayer`) — any value ≥ 0 means the engine let a self-request through — and, for Capture* feeds, requires the own-endpoint sink to have received preview frames (`videoViaCapture` outlives the released `videoCapture`). The own-endpoint sink is never counted as a received pair.
- Group mode source: `submodules/TgVoipWebrtc/tgcalls/tools/cli/group_mode.cpp`
