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

## Further Context

When working in these areas, additional `CLAUDE.md` files load automatically:
- `submodules/TgVoipWebrtc/tgcalls/tools/cli/CLAUDE.md` — CLI test tool architecture (P2P/Reflector, Group), supported version matrix
- `submodules/TgVoipWebrtc/tgcalls/tools/go_sfu/CLAUDE.md` — Go SFU internals: build integration, bandwidth adaptation, transport-cc feedback, network simulation
- `submodules/TgVoipWebrtc/tgcalls/tgcalls/v2wasm/CLAUDE.md` — pump-boundary call core (native + WASM): the ABI, backends, `--wasm-core` usage, invariants
- `submodules/TgVoipWebrtc/CLAUDE.md` — tgcalls library internals: macOS/Linux build patches, SCTP signaling, InstanceV2CompatImpl, GroupInstanceCustomImpl/ReferenceImpl, video pitfalls, known issues
