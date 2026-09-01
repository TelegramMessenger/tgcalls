# CLAUDE.md

This is a testbench repository for the tgcalls VoIP library (from Telegram). It contains the full Telegram iOS source tree as a build dependency, but the focus is on testing and debugging tgcalls.

## Build

Requires Bazel 8.4.2 (download to `build-input/` if not present):

```bash
# One-time setup: create build configuration stub
mkdir -p build-input/configuration-repository/provisioning
# Then populate MODULE.bazel, BUILD, variables.bzl, provisioning/BUILD
# (see build-input/configuration-repository/ for existing stubs)

# Build the CLI test tool
./build-input/bazel-8.4.2 build //submodules/TgVoipWebrtc/tgcalls/tools/cli:tgcalls_cli
```

The system-installed Bazel (v9) is NOT compatible with this codebase.

## Linux Build

Prerequisites (Ubuntu/Debian):
```bash
apt install gcc g++ cmake meson ninja-build nasm make autoconf automake libtool pkg-config zlib1g-dev libbz2-dev
```

Download the Linux Bazel 8.4.2 binary to `build-input/`:
```bash
curl -fL "https://github.com/bazelbuild/bazel/releases/download/8.4.2/bazel-8.4.2-linux-arm64" -o build-input/bazel-8.4.2-linux
chmod +x build-input/bazel-8.4.2-linux
```

Build the CLI test tool:
```bash
./build-input/bazel-8.4.2-linux build //submodules/TgVoipWebrtc/tgcalls/tools/cli:tgcalls_cli
```

The same Bazel 8.4.2 version is required. The build uses the system GCC toolchain and system-installed cmake/meson/ninja for third-party library compilation.

## Docker Build

Build a minimal Linux container image from macOS (or any Docker host):

```bash
# Build (uses BuildKit cache — first build ~5 min, rebuilds seconds)
docker build -t tgcalls-test .

# Run locally
docker run --rm tgcalls-test --mode p2p --duration 5 --quiet
docker run --rm tgcalls-test --mode reflector --reflector 91.108.13.2:598 --duration 10 --quiet

# Push to ECR for AWS deployment
docker tag tgcalls-test 654654616143.dkr.ecr.eu-west-1.amazonaws.com/tgcalls-test:latest
docker push 654654616143.dkr.ecr.eu-west-1.amazonaws.com/tgcalls-test:latest
```

The Dockerfile uses a multi-stage build: full build environment in stage 1, minimal runtime image (~50MB) in stage 2. Bazel's build cache is preserved across `docker build` invocations via `--mount=type=cache`. The image is built for ARM64 (matches Apple Silicon and Fargate ARM).

## Testing

### Local Mass Testing

Run large-scale P2P tests locally using `run-local-test.sh`. Launches N parallel processes, each running a single call, and aggregates results.

```bash
# 1000 calls, 150 parallel, 30% loss (default settings)
./submodules/TgVoipWebrtc/tgcalls/tools/cli/run-local-test.sh -n 1000

# Custom parallelism and duration
./submodules/TgVoipWebrtc/tgcalls/tools/cli/run-local-test.sh -n 500 -j 100 -d 30

# Custom loss parameters
./submodules/TgVoipWebrtc/tgcalls/tools/cli/run-local-test.sh -n 1000 --drop-rate 0.5 --delay 100-300
```

Options: `-n NUM` (count), `-j PARALLEL` (default 150), `-d DURATION` (default 15s), `--drop-rate RATE` (default 0.3), `--delay MIN-MAX` (default 50-200), `--mode MODE` (default p2p), `--version VER` (default 13.0.0).

Typical results: 100% success rate at 30% loss on Apple Silicon (16 cores).

### AWS Mass Testing

Run large-scale reflector tests on ECS Fargate (ARM64). Infrastructure is pre-configured in eu-west-1. Requires Docker push first.

```bash
# Launch 1000 tasks across all Telegram reflectors, 30s each
./submodules/TgVoipWebrtc/tgcalls/tools/cli/run-test.sh -n 1000 -d 30

# Collect results
./submodules/TgVoipWebrtc/tgcalls/tools/cli/run-test.sh --results
```

The script fetches the reflector list from `https://core.telegram.org/getReflectorList`, embeds the IPs as a `--reflector-list` argument (each task picks a random IP + random port 596-599), and launches in waves of 500 (Fargate concurrent task limit). Results are collected from CloudWatch Logs with automatic retry for delayed log delivery.

**AWS resources** (eu-west-1, account 654654616143):
- ECR: `tgcalls-test`
- ECS cluster: `tgcalls-test`
- Task definition: `tgcalls-test` (ARM64 Fargate, 0.25 vCPU, 512MB)
- CloudWatch log group: `/ecs/tgcalls-test`
- Subnets: `subnet-0292f49f3b4885428`, `subnet-09b8edab6eb20b837`, `subnet-0f464b5c62c9a6d1a`
- Security group: `sg-0d87a1f19be76c160`

**Cost**: ~$0.01 per 100 tasks (~$0.10 per 1000-task run).

## tgcalls CLI Test Tool

Located at `submodules/TgVoipWebrtc/tgcalls/tools/cli/`. Runs tgcalls instances in-process with emulated signaling and validates audio/media flow.

```bash
# P2P mode (direct loopback, no network)
./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli --mode p2p --duration 10

# Reflector mode (routes through a real Telegram UDP reflector)
./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli --mode reflector --reflector 91.108.13.2:596 --duration 10

# Random reflector from a list (picks one at random, randomizes port 596-599 if no port given)
./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli --reflector-list "91.108.13.2,91.108.13.3,91.108.9.1" --duration 10

# Simulate lossy signaling (30% drop, 50-200ms random delay)
./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli --mode p2p --duration 30 --drop-rate 0.3 --delay 50-200

# Quiet mode (summary only, full tgcalls logs dumped on failure)
./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli --mode p2p --duration 5 --quiet

# Group mode (in-process SFU with N participants)
./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli --mode group --participants 3 --duration 10

# Mixed group mode (CustomImpl + ReferenceImpl participants)
./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli --mode group --participants 2 --reference-participants 2 --duration 15

# Group mode with video (H264 simulcast, pattern generator)
./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli --mode group --participants 2 --video --duration 15

# Mixed group with video (both CustomImpl and ReferenceImpl send/receive video)
./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli --mode group --participants 2 --reference-participants 2 --video --duration 15

# ReferenceImpl-only video
./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli --mode group --participants 0 --reference-participants 3 --video --duration 15

# Group churn stress test (100 join/leave cycles, then validate base group)
./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli --mode group-churn --participants 3 --duration 10

# Group churn with video
./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli --mode group-churn --participants 3 --video --churn-cycles 100 --duration 10

# Mixed implementations churn
./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli --mode group-churn --participants 2 --reference-participants 1 --video --duration 10
```

`--mode` is required (`p2p`, `reflector`, `group`, or `group-churn`) unless `--reflector-list` is used (implies reflector mode). Exit code 0 = success. Exit code 1 = failure.

For p2p/reflector: success = call established, stats logs non-empty, BWE non-zero for both sides.
For group (audio): success = all N participants report `isConnected = true` AND all participants receive remote audio (non-zero SSRC with level > 0.05 via `audioLevelsUpdated`). Remote 440Hz sine tone typically arrives at ~0.126 level.
For group (video): audio criteria plus every participant receives ≥1 decoded video frame from every other participant via `FakeVideoSink` frame counting.
For group-churn: success = all churn cycles complete without crash/hang AND base group passes group validation (all connected, all receiving audio, and if video, all receiving video from all other base participants).

### CLI Options
- `--mode p2p|reflector|group|group-churn` — call mode (required unless `--reflector-list` used)
- `--reflector host:port` — single reflector address
- `--reflector-list addr,addr,...` — comma-separated list, one picked at random
- `--version VER` — caller tgcalls protocol version (default: `13.0.0`)
- `--version2 VER` — callee tgcalls protocol version (default: same as `--version`). Enables cross-version interop testing.
- `--wasm-core PATH` — CLI-only override: run the caller's pump core from a module file instead of the version-derived default (`NONE` forces native). Mainly for variant modules; version `19.0.0` already uses the module embedded in the binary. Exists only because the CLI target defines `TGCALLS_ALLOW_EXTERNAL_WASM_CORE` — the app has no such loader.
- `--wasm-core2 PATH` — same for the callee (defaults to `--wasm-core`; `NONE` forces native)
- `--custom-params JSON` — sets the caller's engine `customParameters` as a JSON object (merged
  with the `wasm_core_path` entry the CLI synthesises from `--wasm-core`). This is how to
  exercise the three new default-off flags without a server-side rollout: e.g.
  `network_disable_stun_when_unconfigured`, `network_reflector_resolve_remote_candidate_ip`,
  `network_reference_use_addtrack`.
- `--custom-params2 JSON` — same for the callee. Independent of `--custom-params` — unlike
  `--wasm-core2` there is no fallback to the caller's value — so `--custom-params2` **alone**
  gives the callee different engine parameters than the caller, enabling a within-call A/B on
  one flag. See `--log-file` below to capture evidence of what a flag actually did:
  `SetLogToStderr(false)` is set unconditionally by the engines and no file sink exists without
  it, and `RTC_LOG` is process-global, so one `--log-file` captures both endpoints' output
  interleaved.
- `--participants N` — number of CustomImpl participants in group mode (default: 3)
- `--reference-participants N` — number of ReferenceImpl (PeerConnection-based) participants in group mode (default: 0). Total = `--participants` + `--reference-participants`.
- `--duration N` — test duration in seconds (default: 10)
- `--drop-rate 0.0-1.0` — signaling packet drop probability
- `--delay min-max` — signaling delay range in ms (e.g., `50-200`)
- `--video` — enable H264 video with simulcast in group mode (both CustomImpl and ReferenceImpl participants)
- `--churn-cycles N` — number of join/leave cycles in group-churn mode (default: 100)
- `--network-scenario NAME` — network simulation test scenario (e.g., `step-down-up`). Group mode only.
- `--quiet` — summary output only
- `--log-file PATH` — write RTC_LOG output to a file — log sinks are process-global, so one flag captures both sides; needed to observe `[core]` marker lines

### Modes
- **P2P**: Direct loopback, `enableP2P=true`, no servers configured
- **Reflector**: Routes through a Telegram UDP reflector, `enableP2P=false`, configures `RtcServer` with `login="reflector"` and random peer tags (16 bytes, byte 0 = `0x00` for caller, `0x01` for callee)
- **Group**: In-process SFU with N participants using `GroupInstanceCustomImpl` and/or `GroupInstanceReferenceImpl`. The SFU is implemented in Go using Pion's low-level ICE/DTLS/SRTP/SCTP APIs (not PeerConnection), linked into the same process via CGo c-archive. Each participant gets a full ICE + DTLS + SRTP + SCTP transport stack over localhost UDP. Audio RTP is selectively forwarded between all participants. With `--video`, H264 video with 3-layer simulcast is enabled. Mixed-implementation groups (CustomImpl + ReferenceImpl) are supported via `--reference-participants`.
- **Group Churn**: Stress test for participant join/leave dynamics. Creates a base group of N participants, then rapidly cycles an additional participant in and out `--churn-cycles` times (default 100). After churn, validates that the base group is healthy: all connected, all receiving audio, and if `--video` is enabled, all receiving video. Alternates between CustomImpl and ReferenceImpl for the cycling participant. The `--duration` controls the stabilization wait after churn completes.

## Project Structure

- `submodules/TgVoipWebrtc/tgcalls/tools/cli/` — CLI test tool (main.cpp, group_mode.cpp, group_participant.h/.cpp, group_churn_mode.h/.cpp, fake_video_source.h/.cpp, fake_video_sink.h, run-test.sh, run-local-test.sh, BUILD)
- `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/` — Go/Pion SFU library (sfu.go, participant.go, mux.go, go.mod/go.sum), built as c-archive via rules_go + Gazelle, linked into tgcalls_cli
- `submodules/TgVoipWebrtc/tgcalls/tgcalls/` — tgcalls library source
- `submodules/TgVoipWebrtc/tgcalls/tgcalls/group/` — group call implementations (GroupInstanceCustomImpl, GroupInstanceReferenceImpl, GroupNetworkManager, GroupJoinPayloadInternal)
- `submodules/TgVoipWebrtc/tgcalls/tgcalls/v2/` — v2 implementation (InstanceV2Impl, InstanceV2ReferenceImpl, InstanceV2CompatImpl, NativeNetworkingImpl, SignalingSctpConnection, SignalingTranslator)
- `submodules/TgVoipWebrtc/tgcalls/tgcalls/v2wasm/` — pump-boundary call core: `CallCoreABI.h` (C ABI v1, a PeerConnection-projection contract since Phase 2.5; since Phase 2.6 also the raw signaling-packet boundary, N-channel data-channel surface, and audio/ICE config knobs), `ReferenceCallCore` (portable control logic, the parity baseline; since Phase 2.6 owns signaling framing via `SignalingFraming`/`CoreGzip`/`CoreBase64`), `CallCoreHost` (harness — since Phase 2.6 seals/routes core-framed packets rather than parsing signaling JSON itself), `InstanceV2PumpImpl` (**versions `18.0.0` = native core and `19.0.0` = embedded wasm core**, both wire-compatible with stock 11.0.0, for a server-side A/B on the substrate); Phase 2 adds the `reference-core-abi1.wasm` module + WAMR backend; Phase 2.5 adds a second module, `variant-core-abi1.wasm` (`VariantCallCore`, wasm-only), demonstrating behavior changes (SDP munge, adaptive bitrate cap, periodic ICE restart) with no harness/CLI rebuild; Phase 2.6 extends the variant demo with signaling padding, an `exp0` data-channel ping/pong, and APM/config-knob demos; Phase 3 embeds the reference module in the binary, compiles the external-module loader out of the app, and deletes the V1 (wire 10.0.0) framing
- `submodules/TgVoipWebrtc/BUILD` — contains `tgcalls_core` target (C++ only, macOS-native) and `TgVoipWebrtc` target (iOS, ObjC)
- `third-party/webrtc/` — WebRTC source and BUILD
- `third-party/webrtc/webrtc/net/dcsctp/` — dc-sctp (SCTP implementation)
- `third-party/webrtc/webrtc/media/sctp/dcsctp_transport.cc` — WebRTC SCTP wrapper
- `third-party/` — other dependencies (opus, libvpx, ffmpeg, boringssl, etc.)

## Code Style
- **Naming**: PascalCase for types, camelCase for variables/methods
- **Language**: C++17 for tgcalls code
- **Formatting**: Standard C++ formatting

## Engine audit findings (July 2026)

> **Re-analysed 2026-08-19 — read this before acting on the audit doc.** An adversarial
> re-analysis re-verified every claim in `docs/engine-audit-2026-07.md` against the tree and
> **refuted six of them.** The refutations, and the fixes that followed, are recorded in that
> doc's own header and in "Fixes landed 2026-08-19" below. Do not act on a refuted section.

[`docs/engine-audit-2026-07.md`](docs/engine-audit-2026-07.md) records two investigations: why
Cloudflare TURN-only underperformed Telegram reflectors in an `InstanceV2Impl` A/B, and an audit
of `InstanceV2ReferenceImpl` + its custom PeerConnection networking stack. Read it before
touching relay/ICE code or drawing a conclusion from a relay-backend A/B. The load-bearing points:

- **`ReflectorPort::CreateConnection` keys signalled connections differently from inbound
  packets on UDP** (`SetResolvedIP` is gated on `PROTO_TCP` at `ReflectorPort.cpp:541`, while
  `HandleIncomingPacket:765` applies it unconditionally). Measured: 749 signalled relay
  connections sent 15,985 STUN pings and received **0** responses; every response lands on a
  peer-reflexive twin. **This affects production (13.0.0).** Do not read the prflx connections as
  a reflector advantage — 93% of them duplicate an already-signalled address.
- **`InstanceV2ReferenceImpl` and 18.0.0/19.0.0 are test-only**, so their defects are
  *measurement* defects, not user-facing ones. Rank arm-asymmetric ones first.
- **Two arms are not measurable against each other today** — `packet_overhead_bytes` reads 8 vs
  28 and the route line's remote `turn:` reads 0 vs 1 for structural reasons unrelated to call
  quality. (The `"network": []` problem is FIXED — see below.) Never key an A/B metric on
  `remote_candidate().is_relay()`. A separate `relay_backend` field is not needed: `ReflectorPort`
  mints its local candidate as `reflector-<id>-<tag>.reflector` and that string already lands in
  `network[].network.local.address`, so reflector routes are identifiable in data you already have.
- **The injected port allocator loses no configuration vs the default** —
  `InitializePortAllocator_n` covers injected allocators too. That hypothesis is dead; don't
  re-open it. The real delta is *what the allocator points at*. NOTE: the audit's claim that the
  ICE-server mapping loop "silently drops hostname servers" is **refuted** — `IsComplete()` is
  `!IPIsAny(ip_) && port_ != 0` and `IPIsAny` returns false for `AF_UNSPEC`, so a hostname with a
  non-zero port passes. Only TCP servers are dropped there.
- **REJECTED** (2026-08-19 re-analysis) — ~~A directional 1:1 transceiver design needs
  `AddTrack` + `SetDirectionWithError(kSendOnly)` and an offerer-only pre-declared recvonly
  slot; pre-creating a recvonly transceiver on the *answerer* is actively harmful.~~ The
  2026-08-19 re-analysis rejected this directional recipe outright. Do not implement it — see
  "Do not implement" below.

### Fixes landed 2026-08-19

Unflagged (always on): the `~ReflectorPort` write-after-free and `Close()` iterator invalidation;
the incoming-video-sink use-after-free in all three PeerConnection engines; swallowed
`CreatePeerConnectionOrError` failure and its unguarded dereferences; unread SDP-observer errors
(which could deadlock renegotiation); the duplicate startup offer (every outgoing call offered
twice — 15.3% of outbound signalling bytes); the missing baseline network record (~77% of failing
11.0.0 calls uploaded `"network": []`); and the wasm cores' state semantics, which now match
11.0.0 (ICE `failed` non-terminal, 20s watchdog, 2s disconnect debounce, throttled ICE restart).

**Three experiment flags, all default-off**, read from the server-supplied `customParameters`:

| flag | reaches | effect |
|---|---|---|
| `network_reflector_resolve_remote_candidate_ip` | `NativeNetworkingImpl` → `InstanceV2Impl` → versions 7/8/9/12/13 | signalled relay connections can receive binding responses |
| `network_disable_stun_when_unconfigured` | 11/14/18/19 | stops sending unparseable STUN to reflectors |
| `network_reference_use_addtrack` | 11.0.0 | removes an extra offer/answer round trip |

Exercise them with the CLI's `--custom-params '<json>'` (caller) / `--custom-params2` (callee).

**Before enabling `network_reflector_resolve_remote_candidate_ip`** — the connection key collapses
per *candidate*, not per *peer*, and one peer publishes several: `OngoingCallContext.swift` emits
the v4 and v6 entries of one reflector with the **same `reflectorId` and port**, and there is one
relay port per network with a fresh random tag. `SocketAddress::operator<` stops comparing
`hostname_` once `ip_` is set, so with the flag ON they collapse to one key and the collision guard
refuses the second — dropping a dual-stack peer's relay paths from two to one. (The related packet
misattribution is *already* today's behaviour on the peer-reflexive path, flag or not.) Only
"stop stamping the resolved IP on the inbound address, so both sides key on hostname" actually
removes the collapse, and it uniquely also fixes the pre-existing one — but it touches the
always-on path and needs its own window. Also note the flag flips the TCP path's collision policy
from destructive-replace to refuse, and that under `network_standalone_reflectors` the original bug
does not exist while the collapse still does.

**Readout for that flag is relay-remote connections receiving binding responses** — *not* "prflx
count goes to zero", which the audit specified and which is wrong: peer-reflexive pairs are still
minted whenever the peer pings first.

**Call version 12.0.0 — release coordination.** The hardcoded 12.0.0 TCP reflector injection was
removed app-side. Those entries carried `id: 123456` and were appended *before* `reflectorIdList`
is built and sorted, so they shifted every real reflector's mapped id — and that id goes on the
wire as `reflector-<serverId>-<tag>.reflector`, which the peer prefix-matches. **A 12.0.0 call
between an updated and a not-yet-updated client gets zero signalled relay connections.** Do not
serve 12.0.0 until the build is fully rolled out.

### Do not implement

Each was proposed by the audit and is wrong: the `WebRTC-UseTurnServerAsStunServer/Disabled/`
one-liner (`BasicPortAllocator` bypasses the trial when the STUN set is empty — exactly the
reflector case); resolving the **local** relay candidate's IP (the peer-addressing tag travels only
inside the synthetic hostname, so this kills relay entirely); the sendonly/recvonly directional
transceiver recipe; `AddTrack` in `InstanceV2CompatImpl` (its `SignalingTranslator` synthesises
every callee-side remote offer section as `kSendOnly`, so the edit is inert); a Swift `return []`
credential filter; and re-anchoring `stop()`'s `baseTimestamp` (production 13.0.0 shares the
construct and emits no version key to key the discontinuity on).


## mtproto transport on the PeerConnection engines (11.0.0, 18/19)

`network_use_mtproto` works on `InstanceV2ReferenceImpl` (11.0.0) and
`CallCoreHost` (18/19), producing the same wire bytes as 13.0.0. Default off.

**Shape.** Two changes, both gated on the flag:

1. `PeerConnectionFactoryInterface::Options::disable_encryption = true`, set on
   the factory *before* `CreatePeerConnectionOrError` (that is where
   `DtlsEnabled()` is read). This makes `JsepTransportController` build a plain
   `RtpTransport` instead of a `DtlsSrtpTransport`, and stops DTLS entirely.
2. `PeerConnectionDependencies::ice_transport_factory =
   MtProtoIceTransportFactory`, which wraps a real `P2PTransportChannel` in
   `MtProtoIceTransport` and applies `EncryptedConnection` above ICE.

Result: `MtProtoIceTransport -> DtlsTransport (inactive passthrough) ->
RtpTransport (no SRTP)`. Media is `mtproto(RTP)` and the data channel is
`mtproto(SCTP)`, with **no DTLS handshake at all** - the same code in the same
position as 13.0.0's `MtProtoPacketTransport`. Verified on p2p calls: DTLS
handshake log lines go 45 -> 0, and `Creating UnencryptedRtpTransport` replaces
`Creating DtlsSrtpTransport`.

This requires the "Allow SCTP without DTLS" patch in the vendored webrtc fork -
see `submodules/TgVoipWebrtc/CLAUDE.md`. Without it `disable_encryption` also
kills the data channel and no call can connect.

**Non-obvious invariants**, each of which cost a failed run:

- **`disable_encryption` is not cosmetic, and it is not optional.** It selects
  the transport class. Without it you get a `DtlsSrtpTransport`, and there is no
  pass-through mode: `SrtpTransport` refuses to send (`srtp_transport.cc:43-47`)
  and drops on receive (`:124-128`) when SRTP is inactive. So the only two
  outcomes are double encryption or a dead call. Nothing ends up unencrypted -
  mtproto replaces DTLS-SRTP, and the shared key makes DTLS redundant.
- **Both ends must have the flag.** `disable_encryption` removes the SDP
  fingerprint, and the mtproto framing is itself asymmetric, so a one-sided flag
  fails to connect. Safe in production because `phoneCall.custom_parameters` is
  delivered identically to both participants.
- **`flags` cannot carry the SCTP/RTP distinction.** 13.0.0 selects its
  `0xdcdcdcdc` prefix from `flags`, but the inactive `DtlsTransport` drops that
  argument on send (`dtls_transport.cc:433`) and `RTC_DCHECK(flags == 0)` on
  receive (`:599`). `MtProtoIceTransport` therefore infers the type with
  `InferRtpPacketType` and **always re-emits with `flags == 0`**; a non-zero
  value aborts a `-c dbg` build. The prefix buys wire parity only, not demux -
  upstream demux is mutual filtering (`RtpTransport` drops non-RTP,
  `rtp_transport.cc:266-270`; SCTP validates its own).
- **The 11 signal/callback bridges in `installBridges()` are not
  compiler-enforced.** Omit one and it fails silently at runtime - miss
  `SignalCandidateGathered` and candidates never trickle, so the call simply
  never connects. All of them live in that one method deliberately. The four
  callback setters are non-virtual, so each is bridged by a lambda on the inner
  transport reading our own inherited protected member. Every bridge re-emits
  with `this`, because the controller keys transports by pointer.
  `MtProtoIceTransportTest.cpp` covers all 11; it is the reason this approach is
  safe, so do not weaken it.
- **Do NOT override `SetIceCredentials` / `SetRemoteIceCredentials`.** They are
  virtual but not pure, and their base implementations already delegate to
  `SetIceParameters` / `SetRemoteIceParameters`, which the decorator forwards.

**A route that looks right and is not:** subclassing `DtlsSrtpTransport` to skip
SRTP while leaving DTLS enabled. It works and keeps SCTP, but it defeats the
purpose - mtproto already carries a shared key, so the DTLS handshake and record
framing are exactly the overhead worth removing, and it leaves the data channel
as `mtproto(DTLS(SCTP))` rather than `mtproto(SCTP)`. If you try it anyway, two
traps: `IsSrtpActive()` must not be forced to `true` (`GetSrtpOverhead` and
`GetRtpAuthParams` are non-virtual and `RTC_CHECK(send_session_)`, which fires
in release too), and `SrtpTransport` stores `field_trials` **by reference**
(`srtp_transport.h:169`), so a temporary `FieldTrialBasedConfig` dangles and
segfaults mid-handshake.

**Also dead, for the record:** subclassing `P2PTransportChannel` (its
`OnReadPacket` is private and non-virtual, so inbound cannot be intercepted);
socket- or PortAllocator-level mtproto (would encrypt STUN, which reflectors
must parse); and injecting a fake DTLS transport via `dtls_transport_factory`
(hits the same SCTP gate, so the injection choice was never the blocker).

## Further Context

When working in these areas, additional `CLAUDE.md` files load automatically:
- `submodules/TgVoipWebrtc/tgcalls/tools/cli/CLAUDE.md` — CLI test tool architecture (P2P/Reflector, Group), supported version matrix
- `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/CLAUDE.md` — Go SFU internals: build integration, bandwidth adaptation, transport-cc feedback, network simulation
- `submodules/TgVoipWebrtc/tgcalls/tgcalls/v2wasm/CLAUDE.md` — pump-boundary call core (native + WASM): the ABI, backends, `--wasm-core` usage, invariants
- `submodules/TgVoipWebrtc/CLAUDE.md` — tgcalls library internals: macOS/Linux build patches, SCTP signaling, InstanceV2CompatImpl, GroupInstanceCustomImpl/ReferenceImpl, video pitfalls, known issues
