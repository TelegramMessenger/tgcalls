#!/usr/bin/env bash
# Negotiation regression tests for InstanceV2ReferenceImpl (11.0.0) on the in-process
# p2p harness. Each scenario runs one call and asserts on the engine log.
#
#   ./submodules/TgVoipWebrtc/tgcalls/tools/cli/run-negotiation-tests.sh [path/to/tgcalls_cli]
#
# Scenarios:
#   audio-one-round   a plain call negotiates in ONE offer/answer: 2 SetLocalDescription in the
#                     whole log (one per side), 1 offer, the callee's answer sends audio.
#   video-one-round   the same with both cameras on at start.
#   android-late-cam  the Android app's shape: the caller's camera reaches the engine while its
#                     first offer is in flight and the engine offers immediately
#                     (network_reference_immediate_capture_renegotiation). The call must still
#                     connect, with no DTLS failure.
#   android-glare     as above, and the callee's camera lands right after it answered, so the
#                     callee re-offers while the caller's answer is being rejected and the caller
#                     has to answer the colliding offer without a DTLS role of its own.
set -u
CLI="${1:-./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli}"
if [ ! -x "$CLI" ]; then echo "no CLI at $CLI"; exit 2; fi
TMP="${TMPDIR:-/tmp}/tgcalls-negotiation-$$"; mkdir -p "$TMP"
fail=0

count() { grep -c -F -- "$1" "$2" 2>/dev/null || true; }
check() { # check <label> <actual> <expected>
    if [ "$2" = "$3" ]; then echo "    ok   $1 = $2"; else echo "    FAIL $1 = $2 (expected $3)"; fail=1; fi
}

run() { # run <name> <expected-exit> <cli args...>
    local name="$1" wantExit="$2"; shift 2
    local log="$TMP/$name.log" out="$TMP/$name.out"
    echo "== $name"
    "$CLI" --mode p2p --version 11.0.0 --duration 6 --log-file "$log" "$@" > "$out" 2>&1
    local rc=$?
    check "exit code" "$rc" "$wantExit"
    LOG="$log"; OUT="$out"
}

IMMEDIATE='{"network_reference_immediate_capture_renegotiation":true}'

run audio-one-round 0
check "SetLocalDescription calls" "$(count 'Calling SetLocalDescription' "$LOG")" 2
check "offers sent" "$(count 'sendSignalingMessage: {"@type": "offer"' "$LOG")" 1
check "callee answer sends audio" "$(grep -F 'sendSignalingMessage: {"@type": "answer"' "$LOG" | grep -c 'a=sendrecv')" 1

run video-one-round 0 --video
check "SetLocalDescription calls" "$(count 'Calling SetLocalDescription' "$LOG")" 2
check "offers sent" "$(count 'sendSignalingMessage: {"@type": "offer"' "$LOG")" 1
check "SetLocalDescription failed" "$(count 'SetLocalDescription failed' "$LOG")" 0
check "video frames both ways" "$(grep -c 'Video frames:      caller received [1-9][0-9]*, callee received [1-9]' "$OUT")" 1

run android-late-cam 0 --video-late-caller-ms 500 --delay 400-600 --custom-params "$IMMEDIATE"
check "DTLS transport error" "$(count 'DTLS transport error' "$LOG")" 0
check "Failed to set SSL role" "$(count 'Failed to set SSL role' "$LOG")" 0
check "callee received video" "$(grep -c 'Video frames:      caller received [0-9]*, callee received [1-9]' "$OUT")" 1

# Timing (fixed 500 ms per signaling packet, so the SCTP handshake puts the first offer at the
# callee at ~2.5 s): the callee answers it at once and becomes the DTLS client; its camera at
# 2.8 s makes it re-offer before the caller's second offer (sent at 2.6 s, arriving ~3.1 s)
# lands, so that one is ignored as glare; the caller gets the stale answer (rejected, no role)
# and then the callee's offer, which it must answer as the DTLS server.
run android-glare 0 --duration 7 --video-late-caller-ms 2600 --video-late-callee-ms 2800 --delay 500-500 --custom-params "$IMMEDIATE" --custom-params2 "$IMMEDIATE"
check "DTLS transport error" "$(count 'DTLS transport error' "$LOG")" 0
check "Failed to set SSL role" "$(count 'Failed to set SSL role' "$LOG")" 0
check "video frames both ways" "$(grep -c 'Video frames:      caller received [1-9][0-9]*, callee received [1-9]' "$OUT")" 1

if [ $fail = 0 ]; then echo "ALL PASS"; rm -rf "$TMP"; else echo "FAILURES (logs in $TMP)"; fi
exit $fail
