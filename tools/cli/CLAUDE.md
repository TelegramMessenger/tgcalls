# tgcalls CLI Test Tool

In-process test harness for tgcalls. See the root `CLAUDE.md` for build instructions, top-level CLI usage, and the CLI options reference.

## Supported Versions

| Version | Implementation | Notes |
|---|---|---|
| `14.0.0` | `InstanceV2CompatImpl` | WebRTC PeerConnection speaking V2Impl's channel protocol (rewritten 2026-10-11, see `submodules/TgVoipWebrtc/CLAUDE.md`) |
| `compat:<version>` | `InstanceV2CompatImpl` | CompatImpl constructed under that call version, e.g. `compat:8.0.0` against an unchanged `8.0.0` peer: the registry maps 7.0.0–13.0.0 to `InstanceV2Impl`, as the app does |
| `13.0.0` (default) | `InstanceV2Impl` | Also: 7.0.0, 8.0.0, 9.0.0, 12.0.0 |
| `11.0.0` | `InstanceV2ReferenceImpl` | Also: 10.0.0. Uses WebRTC PeerConnection |
| `5.0.0` | `InstanceImpl` (v1) | Also: 2.7.7. Legacy |

## Architecture (P2P/Reflector)
- Two `tgcalls::Instance` objects (caller + callee) created via `Meta::Create(version, ...)`
- Signaling bridged via `SignalingBridge` with configurable drop rate and delay
- Group participant logs (`/tmp/tgcalls_group_p<id>_<pid>.log`) are written at stop and unlinked after validation; set `TGCALLS_CLI_KEEP_LOGS=1` to keep them (added 2026-09-17).
- `--video` in p2p/reflector mode attaches a `tgcalls::VideoCaptureInterface::Create(threads, ...)` to BOTH descriptors (`descriptor.videoCapture`, the iOS join-time path: camera already on when the call is created). Added 2026-09-17 to reproduce the 18/19 start-glare renegotiation loop; a healthy video call shows 2 `Calling SetLocalDescription` in the `--log-file` for 11.0.0 (one per side since the 2026-10-12 one-round negotiation; 4 for 18/19), no `SetLocalDescription failed`, one `Creating data channel`. Combine with `--delay 100-300` for field-like signaling latency.
- `run-parity-tests.sh` (2026-10-27): `InstanceV2CompatImpl` under a legacy version against `InstanceV2Impl` under the same version, "no worse than V ↔ V" as numbers. Eight scenarios (delay, mid-call cameras, mute, 30% signaling loss) × three pairings (V-V, CompatImpl caller, CompatImpl callee), 528 calls, ~6 min; `parity_report.py` prints failure counts with a one-sided Fisher p where CompatImpl is worse, and p50/p90 of connection and first audio/video per side. Default version 8.0.0.
- `--video-late-caller-ms N` / `--video-late-callee-ms N` attach that side's capture N ms after the instances are created (the other side gets none unless it has its own value): the Android app's shape, where the camera reaches the engine while the first offer is in flight. `run-negotiation-tests.sh` (2026-10-12) uses them with `network_reference_immediate_capture_renegotiation` to reproduce the late-camera DTLS failure and pins the one-round negotiation (see the tgcalls `CLAUDE.md`).
- `--video-late` attaches the same captures mid-call via `setVideoCapture` (callee at 3 s, caller at 5 s). With either flag both instances get a `FakeVideoSink` (`setIncomingVideoOutput`) and the summary prints the decoded frames each side received; `remoteMediaStateUpdated` is counted per side. Both are informational, not pass criteria.
- `FakeAudioDeviceModule` with `SineRecorder` (440Hz tone, amplitude 3000) and, since 2026-10-27, `ToneRenderer`, which counts the 10 ms playout frames whose peak reaches 1000 — the peer's tone; silence and loss concealment stay far below — and when the first played. The summary prints `Audio heard: caller N frames (first at Ts), callee …` (-1 = nothing heard); with `--video*` also `Video first frame: caller at Ts, callee at Ts`. Informational, like the video counts; the exit code still rests on connection + BWE. With both sides muted the counts are 0, which is how the renderer was validated (a first version read past the buffer and "heard" ~500 frames regardless: the fake ADM's `num_samples` is already the interleaved total).
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
- SSRC discovery: video SSRCs are broadcast via `ActiveVideoSsrcs` (the test app's `dataChannelMessageReceived` callback consumes them and calls `setRequestedVideoChannels`). Audio SSRCs are discovered by `GroupInstanceReferenceImpl` from the RTP packets WebRTC's demuxer drops (`OnUnDemuxableRtpPacket` seam; mid=0 is sendonly and payload-type demuxing is off, so an SSRC without its own m-line is always dropped) — same shape CustomImpl uses (`receiveUnknownSsrcPacket` → `_requestMediaChannelDescriptions`). The legacy `ActiveAudioSsrcs` data-channel message has been removed.
- Video SSRC groups: parsed from join payload `"ssrc-groups"` field (SIM + FID semantics), stored per participant
- Colibri video constraints: SFU parses `ReceiverVideoConstraints` from receivers, sends `SenderVideoConstraints` back to senders with `idealHeight`, and sends proactive PLI to trigger keyframes when a receiver first requests video
- RTCP feedback: SFU demuxes SRTCP from the shared ICE transport (RFC 5761: byte[1] >= 200 && < 224), decrypts with per-participant SRTCP contexts, parses PLI/FIR, and forwards as new PLI to the sender. NACK is terminated (not forwarded).
- Audio validation: `audioLevelsUpdated` callback tracks remote audio levels; success requires every participant to receive audio from at least one other participant (remote SSRC != 0, level > 0.05). The 440Hz sine tone arrives at ~0.126 level after SFU forwarding.
- Video validation: `FakeVideoSink` (implements `rtc::VideoSinkInterface<VideoFrame>`) counts decoded frames per remote endpoint; success requires every participant to receive ≥1 frame from every other
- Video signaling flow: SFU broadcasts `ActiveVideoSsrcs` over data channel → `dataChannelMessageReceived` callback fires in the app → app calls `setRequestedVideoChannels` → CustomImpl creates `IncomingVideoChannel` / ReferenceImpl adds recvonly video transceiver → both send `ReceiverVideoConstraints` → SFU sends `SenderVideoConstraints` + proactive PLI → sender produces keyframe → receiver decodes. `requestVideoFromEndpoints()` (group_participant.cpp) is the single request path: it accumulates the FULL requested set in `ParticipantState::requestedVideoChannels` and resends all of it on every call, because both engines treat the list as the complete set and drop endpoints missing from it. With `--early-video-request` the same helper runs right after `emitJoinPayload` for every already-joined participant (SSRCs via `GoSfu_QueryVideoSsrcs`), reproducing the real app's order — request before join response, before the data channel — which the `ActiveVideoSsrcs`-driven path never exercises (the broadcast fires 500 ms after the data channel is up)
- `dataChannelMessageReceived` callback: added to `GroupInstanceDescriptor`, forwards all incoming Colibri data channel messages to the application. Used by the CLI test tool to react to `ActiveVideoSsrcs` and dynamically set up video channels — mirrors the real Telegram app's reactive flow
- `FakeAudioDeviceModule` with `SineRecorder` (440Hz tone) and `NoOpRenderer` (audio discarded; levels come from `audioLevelsUpdated`)
- `FakeInterface` (`tgcalls/platform/fake/FakeInterface.cpp`) wraps the builtin video encoder/decoder factories so `GetSupportedFormats()` comes back in the iOS platform order — two H264 entries (packetization-mode 1, the constrained-baseline one kept), VP8, VP9 profile 0. PeerConnection assigns dynamic payload types by walking that list (96, 98, 100, ... with RTX at +1), so the ORDER decides which codec sits on which number; the raw builtin host order lists five H264 profiles and lands one on PT 104 by coincidence, which is exactly the number the group-call convention uses for H264 and hid a ReferenceImpl bug for the device (where PT 104 was H265). `--builtin-codec-order` restores the raw order for comparison
- `churnVideoSinks()` (group_participant.cpp, `--video-sink-churn`) replaces every participant's incoming sinks via `addIncomingVideoOutput` and releases the old ones, mirroring the app recreating tile views on quality changes; the engine must only ever hold weak references (a raw sink pointer left on the track crashes on the next frame)
- `dropAllVideoRequests()` / `rerequestAllVideo()` (group_participant.cpp, `--video-rerequest`): after frames flow, every participant's SFU egress gets a 150 ms delay (`GoSfu_SetNetworkParams`), then five rounds of `setRequestedVideoChannels({})` followed a second later by the full accumulated set again, sinks kept throughout. Validation requires every sink's frame count to grow past the value recorded at the last drop ("Video after re-request: N/M stalled", must be 0). The delay is load-bearing: on the in-process SFU the streams stop before the renegotiation lands, whereas on a real network the packets forwarded before the SFU read the empty `ReceiverVideoConstraints` are still arriving while the receive streams are torn down; that in-flight packet is what used to create a default receive stream on the abandoned transceiver's channel and freeze the tile on re-request. The window is sub-millisecond at the harness's packet rates, so the reproduction needs `--reference-participants 6` (both unfixed runs failed 1/30; three participants passed by luck).
- `FakeVideoTrackSource` generates 1280x720 I420 frames at 30fps with per-participant color tint and frame counter (720p needed for 3 simulcast layers; 640x360 only allows 2 per WebRTC's `kSimulcastFormats`)
- Two ways a participant feeds outgoing video (`VideoFeed` in group_participant.h): `Source` — `descriptor.getVideoSource` returning the `FakeVideoTrackSource` above (the CLI's original shortcut, no app uses it); `CaptureAtJoin` / `CaptureLate` (`--video-via-capture` / `--video-via-capture-late`) — a real `tgcalls::VideoCaptureInterface::Create(threads, "<id>")`, handed over as `descriptor.videoCapture` or via `setVideoCapture()` after the join response, exactly as the iOS wrapper does. For that to produce frames on the host, the fake platform implements `makeVideoSource` (a `FakePlatformVideoSource`: plain adapted source, pushed into) and `makeVideoCapturer` (`FakeVideoCapturer`: 1280x720 @ 30 fps, tint from `deviceId`, a sweeping bar for motion, also feeds the uncropped preview sink) in `tgcalls/platform/fake/FakeVideoCapturer.{h,cpp}`. Both used to return nullptr, which is why `setVideoCapture` could be a no-op in the reference engine without any test noticing.
- `--request-own-video`: `requestVideoFromEndpoints()` appends the participant's own endpoint to every request (the SFU's `ActiveVideoSsrcs` never names the receiver, but the app's roster includes the local participant and is handed to the engine unfiltered) and registers a sink for it. Validation reads `GoSfu_QueryRequestedLayer(receiver, receiver)` before the SFU is destroyed (`ParticipantState::sfuSelfRequestedLayer`) — any value ≥ 0 means the engine let a self-request through — and, for Capture* feeds, requires the own-endpoint sink to have received preview frames (`videoViaCapture` outlives the released `videoCapture`). The own-endpoint sink is never counted as a received pair.
- Group mode source: `submodules/TgVoipWebrtc/tgcalls/tools/cli/group_mode.cpp`

## Architecture (Broadcast)

`--mode broadcast` (2026-11-01) measures **today's engine as a broadcast listener**: `GroupInstanceCustomImpl` in
`GroupConnectionModeBroadcast`, fetching 1-second parts from the Go SFU's emulated broadcast server (see the go_sfu
`CLAUDE.md`, "Broadcast server"). It exists to record the old engine's behaviour as the baseline the app-side broadcast
player (telegram-ios `docs/superpowers/specs/2026-11-01-broadcast-playback-out-of-tgcalls-design.md`) must meet before
broadcast leaves tgcalls; the baseline is `broadcast-baseline/`.

- **Scenarios** (`broadcast_mode.cpp`): three CustomImpl speakers in WebRTC (`createParticipant` with a
  `broadcast::SpeakerTone`): speaker 0 the time-coded marker, 1 and 2 fixed 2300 / 2900 Hz, endpoints `ep-0..ep-2`.
  The listener (`createBroadcastListener`, id 100, server client 1) joins once every speaker's audio has reached the
  SFU plus 3 s, so its first parts are not from before anyone spoke. `audio`: listener audio only. `tiles`: tiles
  ep-0 thumbnail, ep-1 medium, ep-2 full; at 10 s ep-0 and ep-2 swap (the engine's in-place quality update). `unified`:
  no speakers, an RTMP stream's listener (unified parts, sink `"unified"`). `handoff`: at 12 s
  `promoteListenerToSpeaker` in `PresentationGroupCall`'s order (`None` keeping broadcast, join payload, SFU join,
  `Rtc`, join response). `--duration` counts from the listener entering broadcast.
- **The marker** (`broadcast_signals`): its pitch steps every 100 ms of the sender's wall clock through 8 frequencies
  (500-1900 Hz), each step a different pitch, any 3 consecutive differences unique per 343 steps (de Bruijn B(7,3) of
  differences). `MarkerDecoder` turns the listener's playout back into playout-minus-generation time: the distance from
  live, and at a handoff which copy is heard (the broadcast and WebRTC copies carry the same Opus payloads, so they can
  only be told apart by when they were generated). `RecordingRenderer` only believes a decoded latency once two
  consecutive steps agree within 20 ms (triples spanning a handoff decode to garbage).
- **Real-time pacing**: `FakeAudioDeviceModule` sleeps the returned wait after each 10 ms frame, so a fixed 10 ms wait
  runs a microphone or a playout slow by each frame's cost. `RealTimePacer` (deadline-based) paces `GroupSineRecorder`
  (every mode's speakers), the listener's silent recorder and `RecordingRenderer`. Without it a speaker's RTP clock
  drifted ~3 % behind the server's and parts were cut short (12 s of stalls in a 25 s run).
- **Measurements** (`broadcast_metrics`, per 10 ms block / per frame): audio start (first block with an expected tone),
  stalls (≥ 3 consecutive silent blocks after start, outside the handoff window), latency (median/max, before any
  promotion), tones heard per frequency; per tile: first frame, stalls (inter-frame gap > 100 ms), widths (which
  quality was shown), and from the fixture's burned-in frame counter the frames placed in media time (below): latency,
  distinct `frames`, `repeatedFrames`, `outOfOrderFrames`, `droppedFrames`, `unplacedFrames`; levels per speaker
  (`speaker0..2`: SSRCs differ between runs) as mean level and fraction with voice; the handoff's gap (blocks with no
  marker between the promotion and the first block at WebRTC latency, below 1000 ms) and overlap (two markers near full
  amplitude); the server's requests and bytes. Times are wall-clock ms since the listener entered broadcast. Report:
  `--broadcast-report PATH` (schema 1).
- **Placing frames in media time**: a counter names a slice of the 10 s fixture, and the server shows slices in an
  order where three consecutive parts locate a part modulo 162 s (go_sfu guide, "Video and unified parts").
  `CodedFrameSink::report` splits a tile's frames into runs of one slice, locates run i from runs i-2..i
  (`locateFixturePart`), and places the four runs two consecutive agreeing windows cover; latency is arrival minus
  media time modulo 162 s. Until the Plan 1 review latency was `(arrival − counter·1000/30) mod 10 s`, which read the
  old engine's tiles after the step-down, 13–18 s behind live, as 3–8 s. The media frame index separates frames
  shown twice (the old engine's quality update re-fetches buffered segments at the last channel's id,
  `StreamingMediaContext::requestPendingVideoQualityUpdate`, so after the 10 s swap ep-2 shows two segments twice and
  ep-0 drops them) from distinct ones, which a correct player is held to. A run no agreeing windows place (interleaved
  repeats, a skip landing on the same slice) counts as `unplacedFrames`, and the frames it hides show up as dropped.
- **Request callbacks** (`broadcast_client`): one thread per part fetch through the blocking `GoSfu_BroadcastFetch`;
  a cancelled task or a torn-down listener (`ParticipantState::broadcastAlive`, cleared by `stopParticipant` before the
  engine stops) never gets a completion; `responseTimestamp` is seconds, the server's answer time; a join-missing
  answer rejoins in `PresentationGroupCall.requestCall`'s order (`runBroadcastRejoin`): connection mode `None` at once,
  `Broadcast` two profile round trips later (getCurrentGroupCall, then the join). Until the Plan 1 review the engine
  kept playing for one round trip and then switched `None`/`Broadcast` back to back, which understated every events
  cell's stall.
- **The baseline**: `run-broadcast-baseline.sh -j 2 -r 10 -o DIR` (350 runs, ~2.4 h; resumes by skipping existing
  reports; run i of R joins when the wall clock's millisecond is i·1000/R: a run's results depend on where it joins
  within the server's 1 s parts, and until the Plan 1 review the two runs `-j 2` starts together joined at one phase,
  so 10 runs a cell were 5 samples), then `broadcast_report.py aggregate DIR tools/cli/broadcast-baseline` (median, p10, p90 per metric per
  cell); the raw reports are kept in `broadcast-baseline/reports/`, so the rules can be recalibrated without a
  re-run (`broadcast_report.py calibrate`). `broadcast_report.py compare BASELINE CANDIDATE [--metrics prefix,…] [--confirm CANDIDATE2]` is the bar for a
  new implementation, run with 5 repetitions: lower-is-better metrics (start, stalls, latency, frame order, handoff
  gap/overlap) at most p90 + margin, heard blocks, frames and mean level at least p10 − margin, the margin being the
  baseline's p90 − p10 and at least the metric's slack (50 ms, 5 blocks, 2 frames, 1 stall). **Why this shape**: with
  5 runs a side, the planned "median at most p90" failed a candidate drawn from the baseline's own runs on ~13% of
  checks (~44 of the matrix's 500; measured on Task 8's baseline, whose 5 runs a cell were ~2.5 independent samples).
  `broadcast_report.py calibrate broadcast-baseline/reports --matrices 10000` simulates compare as it is used (the
  recorded baseline fixed, each candidate's 5 runs drawn from the cell's recorded runs): a candidate as good as the
  old engine fails 0.18 of the matrix's 620 checks on average, and with `--confirm` 19 checks in 10000 matrices
  (99.8% pass); one worse by max(3 sd, 2 slack) fails 53% of its checks, 42% with `--confirm`, which is what
  `--confirm` costs. `--confirm` counts a check only if it also fails against a second candidate run (unconfirmed ones
  are listed). A metric the baseline has in
  every run fails if any candidate run lacks it (a run that never started). **Informational**: heard blocks of a tone
  the scenario does not play (the detector "hears" 1000 Hz in listener cells, 0–355 blocks a run, and the marker in
  unified cells, 9–79, 9–28 in the 25 s runs: leakage from neighbouring frequencies), the speaking fraction (webrtc's
  VAD judges a steady sine speech in one of two modes picked per run: a speaker reads 0.0 in some runs and 0.1–0.4 in
  others, so it carries
  no signal; the audio-core plan must check the VAD port per update instead), `unplacedFrames`, server counts.
- **Libraries for the new player's runner** (telegram-ios `Tests/BroadcastRunner`, which plays the same scenarios with the
  app-side player and is held to this baseline): `broadcast_signals`, `broadcast_metrics`, `broadcast_report` (the report
  writer, `broadcastReportJson`, which broadcast mode also uses) and `group_participant` (the speakers) are public. Keep
  their outputs identical for both: the report's keys are what `broadcast_report.py compare` matches.
  `CodedFrameSink::onFrameAt(width, counter, arrival)` and `readFrameCounter(y, stride, width, height)` take frames from a
  player that does not hand over `webrtc::VideoFrame`s. `aggregate --prefix player` names a candidate's summary.
- Tests (host, `bazel test //submodules/TgVoipWebrtc/tgcalls/tools/cli:all`): `broadcast_signals_test` (marker round
  trip, with Opus; the slice order locates every part), `broadcast_part_format_test` (the server's parts read by
  tgcalls' own `AudioStreamingPart` / `VideoStreamingPart`: the old decoders are the format's specification; the
  server's slice order is the metrics'), `broadcast_client_test` (cancellation, teardown, time sources, join-missing,
  the rejoin order), `broadcast_metrics_test` (latency beyond the loop, repeats, drops); `python3 -m unittest discover
  -s tools/cli -p 'broadcast_report_test.py'` (the rules, their calibration on synthetic checks, `calibrate`).

