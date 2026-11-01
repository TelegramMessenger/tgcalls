# Go/Pion SFU

The group call test mode uses a Go-based SFU (Selective Forwarding Unit) built with [Pion WebRTC](https://github.com/pion/webrtc), linked into the C++ `tgcalls_cli` binary via CGo.

## Build Integration
- `MODULE.bazel` — `rules_go` 0.60.0 + Go SDK 1.24.2 + `gazelle` 0.43.0; Pion dependencies managed via `go_deps` Gazelle extension
- `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/BUILD` — `go_binary` with `linkmode = "c-archive"` produces a static archive + CGo header, exposes `CcInfo` to C++ targets
- `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/go.mod` / `go.sum` — Pion dependency declarations (pion/ice, pion/dtls, pion/srtp, pion/sctp)
- `submodules/TgVoipWebrtc/tgcalls/tools/cli/BUILD` — depends on `//submodules/TgVoipWebrtc/tgcalls/tools/go_sfu` to link the Go archive
- The CGo-generated header is included as `#include "submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/go_sfu.h"` in C++ code

## How It Works
The `go_binary` with `linkmode = "c-archive"` compiles Go code (including the Go runtime) into a `.a` static archive. Functions annotated with `//export` in Go become C-callable symbols. Bazel's `rules_go` automatically provides `CcInfo`, so `cc_binary` targets can depend on the Go archive via `deps` — no manual linkopts needed.

The Go runtime (GC, goroutine scheduler) runs inside the C++ process. This adds ~10MB memory overhead. `GoSfu_Init()` must be called before any other Go functions.

## Key Files
- `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/sfu.go` — SFU core: participant registry, join/leave/response flow, audio+video RTP forwarding, SSRC registry (audio/video/video-rtx with layer index), Colibri `ReceiverVideoConstraints`/`SenderVideoConstraints` handling, PLI/FIR forwarding, `ActiveVideoSsrcs` broadcasting, `//export` C bindings (`GoSfu_Init`, `GoSfu_Create`, `GoSfu_Destroy`, `GoSfu_Join`, `GoSfu_Leave`, `GoSfu_QuerySsrc`, `GoSfu_QueryVideoSsrcs`, `GoSfu_QueryRequestedLayer` — the layer a receiver asked the SFU to forward from a sender, -1 if none; with receiver == sender it tells the harness whether an engine let a request for its OWN video through — `GoSfu_Free`, `GoSfu_Shutdown`)
- `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/participant.go` — per-participant transport stack (ICE agent, DTLS conn, SRTP session, SRTCP contexts for manual RTCP decrypt/encrypt, SCTP association, data channel send/receive, per-receiver video layer selection)
- `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/mux.go` — packet demuxer: three-way split of ICE traffic into DTLS handshake, SRTP (RTP), and SRTCP (RTCP) channels per RFC 7983 + RFC 5761
- `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/go.mod` / `go.sum` — Go module with Pion dependencies
- `submodules/TgVoipWebrtc/tgcalls/tools/cli/group_mode.cpp` — C++ side that drives the group join flow and calls into Go SFU
- `submodules/TgVoipWebrtc/tgcalls/tools/cli/group_mode.h` — header for group mode entry point
- `submodules/TgVoipWebrtc/tgcalls/tools/cli/group_participant.h/.cpp` — shared participant lifecycle helpers (`createParticipant`, `stopParticipant`, `validateGroupState`, `printGroupSummary`), `ParticipantState` struct, audio helpers
- `submodules/TgVoipWebrtc/tgcalls/tools/cli/group_churn_mode.h/.cpp` — group-churn stress test: base group + rapid join/leave cycling

## SFU Bandwidth Adaptation

The SFU implements REMB-based bandwidth-adaptive simulcast layer selection for video. Per receiver, it maintains an EWMA-smoothed bandwidth estimate from REMB RTCP feedback and uses a `LayerSelector` state machine per (receiver, sender) pair to decide which simulcast layer to forward.

### State Machine
- **STABLE**: forwarding current layer. Checks for upswitch opportunity (REMB > threshold × 1.2) or downswitch need (REMB < threshold × 0.7).
- **PROBING_UP**: ramping RTX padding from 0 to the gap between current and target layer bitrate over 2 seconds. Aborts if REMB drops; succeeds if REMB sustains.
- **GRACE_DOWN**: REMB below downswitch threshold. Waits 500ms, then downswitches if not recovered. 5-second cooldown after any switch.

### Layer Thresholds
| Layer | Nominal | Upswitch When | Downswitch When |
|-------|---------|--------------|-----------------|
| 0 | 60 kbps | (start) | (never) |
| 1 | 110 kbps | BW > 132 kbps | BW < 77 kbps |
| 2 | 900 kbps | BW > 1,080 kbps | BW < 630 kbps |

### Requested heights → layers (the protocol's scale)
`heightToLayer` reads a receiver's `maxHeight` on the protocol's layer scale, `layerHeights = {180, 360, 720}`: the thresholds at which a CustomImpl sender enables its layers and the heights CustomImpl receivers ask for (thumbnail 180, medium 360, full 720). `SenderVideoConstraints.idealHeight` is emitted on the same scale. A positive `maxHeight` below 180 maps to **no layer** and is logged as a warning: the SFU forwards nothing for it. That is deliberately strict. It models the production failure where `GroupInstanceReferenceImpl` asked for `maxHeight 90` on thumbnail tiles and those endpoints received no RTP (device log 2026-09-22), so a client sending an off-scale height fails the run rather than being rounded up. Until 2026-09-22 the scale here was ReferenceImpl's own (90/180/360), which is why no CLI run could catch that.

A request that maps to no layer (off-scale, a `maxHeight` of 0, or an endpoint dropped from the constraints and covered by `defaultConstraints: {maxHeight: 0}`) also **stops and removes the pair's `LayerSelector`** and clears `selectedLayer`. Before, forwarding kept running on the surviving selector, because `forwardRTP` prefers `selectedLayer` over `requestedLayer`, so dropping an endpoint never stopped its video at the SFU.

### Layer Selection and SSRC Rewriting
The SFU forwards exactly one simulcast layer per (receiver, sender) pair. Before `ReceiverVideoConstraints` arrives, the SFU uses `requestedLayer` as the cap and forwards at `maxActiveLayer` (the highest layer the encoder actually produces). After constraints arrive, `ensureLayerSelector` sets `selectedLayer` clamped to `maxActiveLayer`.

When forwarding a non-base layer, the SFU rewrites the RTP SSRC to the primary (layer 0) SSRC. This is necessary because `IncomingVideoChannel` in CustomImpl attaches its `VideoSinkImpl` to `_mainVideoSsrc` (the first SSRC in the SIM group, i.e., layer 0). Without SSRC rewriting, packets from higher layers are delivered to the wrong receive stream and never decoded. RTX SSRCs are similarly rewritten to the layer 0 FID SSRC.

### Testing on Localhost
Use `--network-scenario step-down-up` to exercise the full adaptation path via per-client network simulation (replaces the old REMB-override `--bw-scenario`).

```bash
# Network scenario test (30s, 4 phases: uncapped → 80k egress → 200k → uncapped)
./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli --mode group --participants 2 --video --duration 30 --network-scenario step-down-up
```

Unit tests drive the `LayerSelector` state machine directly via mocked callbacks. Run from `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/`:

```bash
go test -run TestLayerSelector -v -timeout 60s
```

Covers upswitch L0→L1→L2, downswitch L2→L1→L0, grace-down recovery on transient dips, stale-BW idle behavior, the `OnMaxActiveLayerIncreased` fallback used when clients don't send REMB, and `maxLayer` enforcement.

### REMB-free Fallback

Real tgcalls clients negotiate `goog-remb` but use transport-cc as the primary BWE signal, so no REMB actually arrives at the SFU. This means the REMB-driven state machine never enters `PROBING_UP` in live runs. `LayerSelector.OnMaxActiveLayerIncreased(maxActive)` is the fallback: when the sender starts producing a higher simulcast layer than previously seen AND the BW estimate is stale, the SFU immediately upshifts to the highest available layer (clamped by `maxLayer`). Called from `sfu.go`'s packet-forwarding path whenever `maxActiveLayer[senderID]` is bumped.

### Key Files
- `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/bandwidth.go` — `BandwidthEstimator`, `LayerSelector`, `RtxRingBuffer`, `OnMaxActiveLayerIncreased`
- `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/bandwidth_test.go` — unit tests for `LayerSelector` up/down transitions
- `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/participant.go` — REMB parsing in `readRTCPLoop()`, `selectedLayers`
- `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/sfu.go` — layer-filtered forwarding, `ensureLayerSelector`, SSRC rewriting, `maxActiveLayer` tracking

## SFU Transport-CC Feedback

The SFU generates RTCP transport-cc feedback (type 205, FMT 15) per sender every 100ms. This provides the sender's GCC (Google Congestion Control) with packet arrival data, enabling BWE ramp-up so the encoder produces higher simulcast layers.

The feedback reflects actual (or simulated) packet arrivals — if ingress network simulation drops packets, the feedback reports them as missing, causing the sender's GCC to reduce bitrate.

### How It Works
1. Each incoming RTP packet is parsed for the transport-wide sequence number (header extension ID 3, one-byte RFC 5285 format)
2. `TransportCCGenerator.RecordArrival(twccSeq)` records the arrival time
3. Every 100ms, `emitFeedback()` builds an `rtcp.TransportLayerCC` packet with `PacketChunks` (run-length or status-vector encoding) and `RecvDeltas` (250µs units)
4. The feedback is marshalled, encrypted via SRTCP, and sent to the sender
5. The sender's `Call::Receiver::DeliverRtcpPacket()` feeds it to the GCC via `GroupNetworkManager::OnRtcpPacketReceived_n` → `_call->Receiver()->DeliverRtcpPacket()`

### Current Status
Transport-cc feedback is working: the SFU records ~60-70 arrivals per first 100ms tick, generates feedback packets (32-128 bytes), and the sender receives them. The GCC ramps from the 400kbps start bitrate to produce layer 1 (640x360). Full ramp to layer 2 (1280x720, needs ~1Mbps) requires further investigation — the GCC may need probing support or the `adjustBitratePreferences` max_bitrate_bps of 1052kbps may be a bottleneck.

### Key Files
- `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/twcc.go` — `TransportCCGenerator`, `parseTWCCSeq` (RTP header extension parser)

## SFU Network Simulation

Per-client network simulation with independent ingress (from client) and egress (to client) simulators. Each direction has: delay, jitter, packet loss, and bandwidth cap (token bucket).

```bash
# Configure via CGo: GoSfu_SetNetworkParams(handle, participantID, direction, delayMs, jitterMs, dropRate, bandwidthBps)
# direction: 0 = ingress, 1 = egress

# Network scenario test (4 phases: uncapped -> 80k -> 200k -> uncapped)
./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli --mode group --participants 2 --video --duration 30 --network-scenario step-down-up
```

### Key Files
- `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/network_sim.go` — `NetworkSimulator` (token bucket, delay, jitter, drop)
- `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/participant.go` — `ingressSim`, `egressSim` on each `Participant`

## Broadcast server

The SFU also emulates Telegram's **broadcast server** (2026-11-01): what a broadcast listener (an RTMP stream's, or
a large chat's listener that gets 1-second parts instead of WebRTC) is answered when it asks for parts, the server
time or a stream's newest part. `tgcalls_cli --mode broadcast` drives it (see `tools/cli/CLAUDE.md`, "Architecture
(Broadcast)"). Files: `broadcast_server.go` (rules, link, events, stats), `broadcast_audio.go` (listener audio from
live RTP), `broadcast_ogg.go` (the Ogg Opus part writer), `broadcast_fixtures.go` (video and unified parts),
`broadcast_libopus.go` (cgo libopus), `broadcast_exports.go` (`GoSfu_Broadcast*`). Formats are what tgcalls' own part
decoders read (`group/AudioStreamingPart*`, `group/VideoStreamingPart.cpp`); `tools/cli/broadcast_part_format_test`
decodes the server's parts with them.

- **Rules**: parts are 1000 ms on the Unix-millisecond grid. A part `[T, T+1000)` is available from
  `T + 1000 + availabilityDelayMs` (default 300; the real server's delay is unknown, device parity calibrates it):
  earlier is NotReady, off the grid or older than the retained window (30 s) is ResyncNeeded. A request is decided when
  it reaches the server. Scripted events per client: `notready-burst@S` (2 s of NotReady), `resync@S`,
  `join-missing@S` (once each), timed from `GoSfu_BroadcastAddClient` on the precise clock.
- **Link**: each client's responses go through `DebugNetworkShaper`'s model (telegram-ios
  `submodules/TelegramCore/Sources/Network/DebugNetworkShaper.swift`) with its profiles: a response's bytes start one
  round trip after the request, or when the link is done with the responses before it, at the profile's bandwidth.
  `GoSfu_BroadcastFetch` blocks for that time. Not the packet `NetworkSimulator`: parts are whole responses.
- **Listener audio**: the group engine sends 60 ms Opus frames, which cannot be cut into 1-second parts, so every
  speaker's audio RTP (tapped in `processIncomingRTP`) is decoded with libopus onto a 48 kHz timeline taken from its
  RTP timestamps (anchored at the first packet's arrival), and each part's 20 ms slots are re-encoded per channel
  (fresh encoder per part, 32 kbit/s). A part is cached once built. Channels: speakers in join order, at most 8 (the
  old decoder ends a part with more); a leaver keeps its channel, empty, until the next part; a second nobody spoke in
  is a part with one silent channel listing none (NotReady there would stall the old engine for good). Empty slots
  carry a CELT silence frame (`F8 FF FE`), not a zero-length frame (libopus conceals that as a loss).
- **Ogg Opus layout**: family 255, one uncoupled stream per channel; `TG_META` (channel → ssrc), `ACTIVE_MASK` and
  `ENDPOINTS` (video channels) as OpusTags comments. **The head's mapping table undoes a reorder**: FFmpeg's libopus
  wrapper (`libavcodec/libopusdec.c`, what tgcalls decodes with) reorders 3-8 channels from Vorbis to FFmpeg order
  whatever the family, and the old client's channel index is that decoder's output order, so stream `c` is placed at
  `mapping[ff_vorbis_channel_layout_offsets[N-1][c]]`. FFmpeg's native opus decoder (the one Homebrew's `ffmpeg`
  uses) does not reorder, so a part looks swapped there and right in tgcalls; a new decoder of these parts has to
  reproduce the libopus wrapper's order.
- **Video and unified parts** come from fixture slices (`testdata/broadcast/`, made by `make-broadcast-fixtures.sh`
  with Homebrew `ffmpeg`, committed so builds never run it): 10 s of `testsrc2` at 320x180, 640x360, 1280x720 (H.264
  baseline, 150/500/1500 kbit/s), one 1 s GOP per file, AAC 1000 Hz tone in the `unified_*` ones, a 16-bit frame
  counter burned into the top 16 rows. Part T shows slice `(T / 1000) % 10` behind the video-part header (signature
  `0xa12e810d`, TL string `"mp4"`, one event: endpoint, rotation 0). Video channel `c + 1` is the endpoint of the
  participant on audio channel `c` of the same second. Unified parts name endpoint `""` (tgcalls shows unified frames
  on its `"unified"` sink whatever the part names). Fixtures rather than the speakers' live H.264: stable part sizes
  per quality (the quality-selection comparison is otherwise the senders' bandwidth-estimation noise), whole GOPs, and
  a readable media time. (Live layer 2 does occur on the host: a 30 s two-participant video run produced 720p SSRCs.)
- **libopus under Bazel only**: `broadcast_libopus.go` builds with the go tag `tgcalls_vendored_opus` and
  `cdeps = ["//third-party/opus:opus"]` (the library webrtc links, one copy in the CLI); `go test` builds without the
  tag (`broadcast_libopus_none.go`) and injects a fake codec. Tests: `go test ./...` here, and the CLI's
  `broadcast_part_format_test` / `broadcast_client_test` under Bazel.

