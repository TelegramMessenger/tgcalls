# v2wasm — pump-boundary call core (native + WASM)

A refactor of `InstanceV2ReferenceImpl`'s call-control logic into a swappable
"core" behind a C ABI, so control-plane variants can eventually be delivered as
runtime-loaded WASM modules and A/B-tested without shipping app builds. The
fixed harness owns everything platform/real-time (PeerConnection, audio,
signaling crypto); the core owns negotiation policy and the signaling wire
protocol. Wire-compatible with stock `InstanceV2ReferenceImpl` versions
`10.0.0`/`11.0.0` — a pump peer interoperates with unmodified stock peers.

Status: Phase 1 (native pump), Phase 2 (WASM module + WAMR runtime),
Phase 2.5 (PeerConnection-projection ABI rewrite + `variant-core-abi1.wasm`
demo module), and Phase 2.6 (protocol substrate — core-owned signaling
framing, N-channel data-channel substrate, audio/ICE config knobs) are
complete and CLI-validated. Phase 3 (iOS app integration behind a flag) is
not started. Design/plan/validation records live in the parent repo under
`docs/superpowers/{specs,plans}/2026-07-0*-tgcalls-wasm-core-*`.

## Files

| File | Role |
|---|---|
| `CallCoreABI.h` | **The frozen contract (ABI v1), a PeerConnection-projection contract.** C form (3 functions + emit callback) and "module form" (WASM exports/imports), plus the full JSON event/command schema — commands (`pc_create_offer`, `pc_set_local_description`, `pc_add_transceiver`, `pc_set_parameters`, `pc_restart_ice`, `pc_get_stats`, …) and events (`pc_description_created`, `pc_track`, `pc_connection_state`, the curated `stats` reduction, …) that mirror the WebRTC-native PC API — plus the normative rules (reentrancy, buffer ownership, `nowMs` clock, unknown-type tolerance, abiVersion refusal). Since Phase 2.6 also carries the **raw signaling packet boundary** (`signaling_send_packet {packetB64}` / inbound plaintext packet — seq framing is core-owned, the host only seals/sends), the **N-channel data-channel surface** (`pc_create_data_channel {label,...}`, `dc_send {label, data|dataB64}`, `dc_state`/`dc_message`/`dc_buffered`/`dc_channel` events, text or binary payloads), and the **audio/ICE config knobs** (`set_audio_processing`, `pc_set_configuration {iceServers, iceTransportsType, candidatePoolSize}`). Read this first. |
| `ReferenceCallCore.{h,cpp}` | The core: perfect-negotiation state machine, `{"@type": offer/answer/candidate}` wire protocol, MediaState, signal-bars heuristic, stats-log shaping, and (Phase 2.6) the V1/V2 signaling framing + data-channel + config-knob command handling. **Compiled twice**: into the native library AND into the `.wasm` module — same source, no fork. Include discipline: only `CallCoreABI.h` + `third-party/json11.hpp` + `v2wasm/CoreGzip.h`/`CoreBase64.h` + C++17 std (this is the WASM-compilability guarantee; compression crosses the ABI as the host_deflate/host_inflate service). Exposes five `protected virtual` hooks (`mungeLocalDescription`, `mungeOutgoingSignalingMessage`, `onStats`, `onIceState`, `onDataChannelEvent`) plus a protected `sendSignalingKeepalive()` helper for derived cores. |
| `SignalingFraming.{h,cpp}` | Core-side signaling framing component (Phase 2.6): V1 (wire `10.0.0`) faithfully ports `EncryptedConnection`'s reliability layer (seq flag bits, message packing, ack bookkeeping, resend timers, service packets); V2 (wire `11.0.0`) gzip's JSON bodies via `CoreGzip`. Produces/consumes the plaintext packet (`seq(4, network order) \|\| body`) the host seals. Same include discipline as the core headers. |
| `CoreGzip.{h,cpp}` | gzip-format compress/decompress for V2 signaling bodies, backed by the **host compression service** (`tgcalls_host_deflate`/`tgcalls_host_inflate` in `CallCoreABI.h`; module form: `env.host_deflate`/`env.host_inflate` imports). Pure mechanism — whether/what to compress stays core policy; compressed bytes now match stock zlib exactly. Mirrors `utils/gzip.h` semantics incl. the zip-bomb `sizeLimit` (= `outCap`). WASM discipline: std + the ABI header only. |
| `HostCompressionService.cpp` | Native implementation of the host compression service over `utils/gzip` (zlib). Harness-side ONLY — never in the wasm modules (there the same C symbols are the `env.*` imports; `WamrCoreBackend` routes them here via `"(*~*~)i"`-validated trampolines). |
| `CoreBase64.h` | Dependency-free base64 encode/decode (`std::vector<uint8_t>` overloads), shared by the data-channel binary path (A) and signaling framing (C). std-only, header-only. |
| `CoreFactory.h` | `createModuleCore(config, emit) -> unique_ptr<ReferenceCallCore>` — the seam a WASM module's entry point calls to pick which core class it instantiates. Same include discipline as the core headers. |
| `reference_core_factory.cpp` / `variant_core_factory.cpp` | Per-module factory TUs implementing `createModuleCore`, one instantiating `ReferenceCallCore` and the other `VariantCallCore`. **WASM-only** — each is compiled into exactly one module genrule, never into the native library. |
| `wasm_module_entry.cpp` | WASM-only entry points (`core_init`/`core_on_event`/`rt_alloc`/`rt_free` exports, `env.host_emit` import) — generic, calls `createModuleCore` rather than naming a core class. Compiled ONLY by the module genrules — never in native source lists. |
| `VariantCallCore.{h,cpp}` | Demo variant core (derives `ReferenceCallCore`): Opus fmtp munge, BWE-driven bitrate-cap loop, periodic ICE restart, RTT/loss-driven signal bars, `variant: …` marker log lines, and (Phase 2.6) signaling padding, a V1 keepalive demo, an `exp0` data-channel ping/pong, and one-shot APM/config-knob demos. **Ships only as `variant-core-abi1.wasm`** — joins no native source list; that's the demonstration that behavior experiments need no harness/CLI rebuild. |
| `CallCoreHost.{h,cpp}` | The fixed harness: PeerConnection/ADM/native `EncryptedConnection`-key-sealing/SCTP-or-external transport routing/timers/`GetStats`; executes core commands (queued, never re-entering the core), forwards platform callbacks as events, stamps `nowMs`. Since Phase 2.6 the host no longer parses/frames signaling JSON itself — it hands the core's raw plaintext packet to the AEAD seal/open and routes bytes; `_isSignalingV2` and the old gzip/JSON framing code were removed. Media-thread only. |
| `CallCoreBackend.h`, `NativeCoreBackend.*`, `WamrCoreBackend.*` | Backend seam: native = linked C ABI (default); WAMR = per-call module instance in the vendored interpreter (`third-party/wamr` in the parent repo). Selection: `Descriptor.config.customParameters` JSON key `wasm_core_path`. |
| `InstanceV2PumpImpl.{h,cpp}` | `Instance` shell (mirrors stock's `ThreadLocalObject` pattern), registered as versions `10.0.0-pump` / `11.0.0-pump`. |

## Building & running

The module is built inside the Bazel graph (hermetic wasi-sdk 33.0 pinned in
the parent repo's `MODULE.bazel`; genrule `reference_core_wasm` in
`submodules/TgVoipWebrtc/BUILD`):

```bash
# builds the CLI AND both modules (data deps):
./build-input/bazel-8.4.2-darwin-arm64 build //submodules/TgVoipWebrtc/tgcalls/tools/cli:tgcalls_cli
# modules land at: bazel-bin/submodules/TgVoipWebrtc/reference-core-abi1.wasm
#                   bazel-bin/submodules/TgVoipWebrtc/variant-core-abi1.wasm

WASM=bazel-bin/submodules/TgVoipWebrtc/reference-core-abi1.wasm
VARIANT=bazel-bin/submodules/TgVoipWebrtc/variant-core-abi1.wasm
CLI=./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli

# wasm-core caller vs stock callee:
$CLI --mode p2p --version 11.0.0-pump --version2 11.0.0 --wasm-core $WASM --wasm-core2 NONE --duration 10
# wasm <-> wasm (callee inherits --wasm-core):
$CLI --mode p2p --version 11.0.0-pump --version2 11.0.0-pump --wasm-core $WASM --duration 10
# wasm-backend vs native-backend (same version, different substrate):
$CLI --mode p2p --version 11.0.0-pump --version2 11.0.0-pump --wasm-core $WASM --wasm-core2 NONE --duration 10
# variant module (demo behavior changes) vs stock callee — --log-file to observe
# the `variant: …` marker lines (RTC_LOG sinks are process-global, so one flag
# captures both sides' logs; markers don't otherwise reach stdout in quiet mode):
$CLI --mode p2p --version 11.0.0-pump --version2 11.0.0 --wasm-core $VARIANT --wasm-core2 NONE --log-file /tmp/variant_rtc.log --duration 20
```

`--wasm-core PATH` sets the caller's module; `--wasm-core2` the callee's
(defaults to the caller's value; sentinel `NONE` forces the native backend).
No `--wasm-core` at all = native backend both sides (Phase-1 behavior).

The P1/P2 wire-parity diffs (see "Invariants & gotchas" below) are
re-runnable via `tools/parity/p1_diff.py RUN_A.log RUN_B.log` (offer/answer
JSON-layer diff) and `tools/parity/p2_framing.py LOG` (V1 framing
invariants), fed by `--log-file` captures.

To experiment with a variant core: subclass `ReferenceCallCore` (override the
protected hooks — see `VariantCallCore.{h,cpp}`), add a `*_core_factory.cpp`
returning your class, and add a genrule in `submodules/TgVoipWebrtc/BUILD`
mirroring `variant_core_wasm` with your sources in its `extra_srcs`. Never
modify `ReferenceCallCore` itself — it is the stock-parity baseline. Harness,
ABI, and entry file stay fixed; pass the new `.wasm` via `--wasm-core`.

The five hooks available to a derived core (`VariantCallCore` uses all of
them): `mungeLocalDescription` (SDP munge point), `mungeOutgoingSignalingMessage`
(edit the outbound `{"@type": …}` JSON object in place — e.g. signaling
padding — before it's framed/sealed), `onStats` (periodic stats tick — bitrate
cap, ICE restart cadence, one-shot APM/config demos), `onIceState`, and
`onDataChannelEvent` (data-channel open/message/buffered-amount events — e.g.
the `exp0` ping/pong demo); plus the protected `sendSignalingKeepalive()`
helper (emits a V1 empty/service packet on demand, used by the keepalive
demo on `10.0.0`).

## Invariants & gotchas

- **Wire parity is frozen.** Signaling JSON keys/values are copied verbatim
  from stock (`InstanceV2ReferenceImpl.cpp` / `Signaling.cpp`); both sides use
  json11 (std::map-backed → key-sorted dump), so identical keys ⇒ identical
  bytes. Never rename a wire key.
- **Core discipline**: `ReferenceCallCore.*`, `VariantCallCore.*`,
  `SignalingFraming.*`, `CoreFactory.h`, the `*_core_factory.cpp` TUs, and
  `wasm_module_entry.cpp` may include only the ABI header, json11,
  `CoreGzip.h`/`CoreBase64.h`, and std. No third-party code beyond json11:
  compression is a host service (`env.host_deflate`/`env.host_inflate`,
  post-2.6 amendment — the vendored miniz was removed). No webrtc, no absl,
  no other tgcalls headers, no exceptions (`-fno-exceptions` in the module
  build).
- **The reference core is the parity baseline** — behavior experiments go in
  variant cores (new `*CallCore` subclasses + a factory TU + a genrule),
  never in `ReferenceCallCore` itself. `ReferenceCallCore` must keep
  reproducing stock `InstanceV2ReferenceImpl` wire-for-wire; that's what P1
  (the wire-log diff) checks.
- **The signaling framing constants and byte layout in `SignalingFraming.cpp`
  are wire-frozen** — copied verbatim from `EncryptedConnection.cpp` (seq
  flag bits, message-packing/ack/resend constants, service-packet shape).
  Never change them; P2 (the V1 wire-log diff + framing invariant check)
  depends on byte-for-byte fidelity.
- **The `[core] signaling out: …` / `sendSignalingMessage: …` log strings are
  load-bearing** — P1/P2 (`p1_diff.py`) grep them to extract the outbound
  offer/answer JSON from pump and stock logs respectively. Don't rename or
  reformat either string without updating the diff script.
- **The pump never re-enters the core**: commands emitted during
  `create`/`onEvent` are queued and executed after the call returns; async
  results come back as events. Events raised mid-drain are deferred via
  PostTask.
- **WAMR auto-invokes `_initialize`** during `wasm_runtime_instantiate` for
  WASI-importing modules — calling it again traps wasi-libc's once-guard.
- **Trust boundary** (WAMR backend): `host_emit` buffers are range-validated
  by WAMR (`"(*~)"` signature); module stdio is sandboxed to `/dev/null`; no
  preopened dirs/env/args; traps and load failures fail the call fast
  (`disableCoreWithFailure`). NOT defended in Phase 2: CPU/memory exhaustion
  (an infinite loop in the module wedges the media thread) — defense is module
  provenance until the Phase-4 watchdog/metering.
- **No signal handlers**: the vendored WAMR builds with
  `WASM_DISABLE_HW_BOUND_CHECK=1` and `WASM_ENABLE_THREAD_MGR=0`; re-audit
  before flipping either define (see the SAFETY comment in
  `third-party/wamr/BUILD`).
- Stock `v2/InstanceV2ReferenceImpl.{h,cpp}` is the read-only fidelity
  reference; intentional deviations are commented at their sites.
