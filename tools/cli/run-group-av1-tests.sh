#!/usr/bin/env bash
# AV1 in group calls: the in-app screen share in AV1, sent by either engine (a GroupInstanceCustomImpl sharer, then a
# GroupInstanceReferenceImpl one) and received by both, on the in-process group harness: both of two layers in AV1
# (--av1-screencast-layers every, the app's mode), one AV1 layer (single) and the top of two layers (top). Each
# scenario runs one call and asserts on:
#   the CLI's "Wire: P<id>[ (screen)] layer <L> pt <PT>: N packets, N frames, F fps" lines (what the SFU received from
#   each sender's layers, by payload type: 104/105 H264 and its RTX, 106/107 AV1 and its RTX), its summary, and
#   the engines' log ("GroupAv1: screen share layers: …" / "GroupAv1: not sending AV1: <reason>"); every engine of a
#   run writes to every log file of that run, so the lines are counted in one of them.
#
#   ./submodules/TgVoipWebrtc/tgcalls/tools/cli/run-group-av1-tests.sh [path/to/tgcalls_cli]
set -u
CLI="${1:-./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli}"
if [ ! -x "$CLI" ]; then echo "no CLI at $CLI"; exit 2; fi
TMP="${TMPDIR:-/tmp}/tgcalls-group-av1-$$"; mkdir -p "$TMP"
fail=0

count() { grep -c -F -- "$1" "$2" 2>/dev/null || true; }
check() { # check <label> <actual> <expected>
    if [ "$2" = "$3" ]; then echo "    ok   $1 = $2"; else echo "    FAIL $1 = $2 (expected $3)"; fail=1; fi
}
check_ge() { # check_ge <label> <actual> <minimum>
    if [ "$2" -ge "$3" ] 2>/dev/null; then echo "    ok   $1 = $2 (>= $3)"; else echo "    FAIL $1 = $2 (expected >= $3)"; fail=1; fi
}
check_le() { # check_le <label> <actual> <maximum>
    if [ "$2" -le "$3" ] 2>/dev/null; then echo "    ok   $1 = $2 (<= $3)"; else echo "    FAIL $1 = $2 (expected <= $3)"; fail=1; fi
}
wire_frames() { # wire_frames <participant> <layer> <pt>: frames the SFU received, 0 if none
    local n; n=$(grep -E "^Wire: P$1( \(screen\))? layer $2 pt $3:" "$OUT" | sed -E 's/.* ([0-9]+) frames.*/\1/' | head -1)
    echo "${n:-0}"
}
wire_fps() { # wire_fps <participant> <layer> <pt>: whole fps, 0 if none
    local n; n=$(grep -E "^Wire: P$1( \(screen\))? layer $2 pt $3:" "$OUT" | sed -E 's/.* ([0-9.]+) fps$/\1/' | cut -d. -f1 | head -1)
    echo "${n:-0}"
}
av1_anywhere() { grep -c -E '^Wire: .* pt 106:' "$OUT" || true; }
wire_packets() { # wire_packets <participant> <layer> <pt>: packets the SFU received, 0 if none
    local n; n=$(grep -E "^Wire: P$1( \(screen\))? layer $2 pt $3:" "$OUT" | sed -E 's/.*: ([0-9]+) packets.*/\1/' | head -1)
    echo "${n:-0}"
}
wire_dd() { # wire_dd <participant> <layer> <pt>: those packets carrying the dependency descriptor at the SFU's ID
    local n; n=$(grep -E "^Wire: P$1( \(screen\))? layer $2 pt $3 dependency descriptor:" "$OUT" | sed -E 's/.*: ([0-9]+) packets.*/\1/' | head -1)
    echo "${n:-0}"
}
full() { # full <summary label>: 1 if "<label>: N/N" with N > 0
    local line; line=$(grep -m1 -E "^$1: +[0-9]+/[0-9]+" "$OUT" | sed -E 's/.*: +([0-9]+)\/([0-9]+).*/\1 \2/')
    set -- $line
    if [ -n "${1:-}" ] && [ "$1" = "${2:-x}" ] && [ "$1" -gt 0 ]; then echo 1; else echo 0; fi
}

EXPECT_EXIT=0
run() { # run <name> <cli args...>; EXPECT_EXIT=any skips the exit-code check
    local name="$1"; shift
    OUT="$TMP/$name.out"
    echo "== $name"
    TGCALLS_CLI_KEEP_LOGS=1 "$CLI" --mode group --video --quiet "$@" > "$OUT" 2>&1 &
    local pid=$!
    wait $pid
    local rc=$?
    if [ "$EXPECT_EXIT" = any ]; then echo "    info exit code = $rc"; else check "exit code" "$rc" "$EXPECT_EXIT"; fi
    LOG="$TMP/$name.log"
    cat /tmp/tgcalls_group_p0_${pid}.log > "$LOG" 2>/dev/null
    rm -f /tmp/tgcalls_group_p*_${pid}.log
    check "every pair got video" "$(full 'Video received')" "1"
}

# A custom-engine sharer, three receivers (two custom, one reference); the SFU lists AV1 and the switch is on.
SHARE="--participants 3 --reference-participants 1 --screencast-participants 0"
TOP="--av1-screencast-layers top"
SINGLE="--av1-screencast-layers single"

# One layer, AV1 as the sender's codec: no second layer, no H264 from the sharer.
run av1-single $SHARE $SINGLE --sfu-av1 --av1-screencast --duration 15
check_ge "AV1 frames" "$(wire_frames 0 0 106)" 100
check_le "AV1 frame rate (fps)" "$(wire_fps 0 0 106)" 31
check "H264 frames" "$(wire_frames 0 0 104)" "0"
check "second layer" "$(grep -c -E '^Wire: P0 \(screen\) layer 1 ' "$OUT" || true)" "0"
check "every peer heard every sender, the screen audio too" "$(full 'Audio levels heard')" "1"
check "decision" "$(grep -c -E 'GroupAv1: screen share layers: AV1$' "$LOG" || true)" "1"
check "one AV1 stream" "$(count 'Av1TopLayer: the share is one AV1 stream' "$LOG")" "1"
check "dependency descriptor on every AV1 packet (default ID 11)" "$(wire_dd 0 0 106)" "$(wire_packets 0 0 106)"
check "dependency descriptor ID" "$(count 'GroupAv1: dependency descriptor as header extension 11' "$LOG")" "1"

# The SFU lists the dependency descriptor (at 12): the share sends it there.
run av1-single-dd-listed $SHARE $SINGLE --sfu-av1 --sfu-dd-id 12 --av1-screencast --duration 10
check_ge "AV1 frames" "$(wire_frames 0 0 106)" 100
check "dependency descriptor on every AV1 packet (ID 12)" "$(wire_dd 0 0 106)" "$(wire_packets 0 0 106)"
check "dependency descriptor ID" "$(count 'GroupAv1: dependency descriptor as header extension 12' "$LOG")" "1"

# One layer, the server without AV1: that layer in H264.
run av1-single-server-no-av1 $SHARE $SINGLE --av1-screencast --duration 10
check "AV1 on the wire" "$(av1_anywhere)" "0"
check_ge "H264 frames" "$(wire_frames 0 0 104)" 100
check "second layer" "$(grep -c -E '^Wire: P0 \(screen\) layer 1 ' "$OUT" || true)" "0"
check "reason" "$(count 'GroupAv1: not sending AV1: server did not list AV1' "$LOG")" "1"
check "dependency descriptor without AV1" "$(wire_dd 0 0 104)" "0"

# One layer, a failing AV1 encoder: WebRTC switches the sender to H264 (the factory's selector), never to VP8.
run av1-single-encoder-fails $SHARE $SINGLE --sfu-av1 --av1-screencast --av1-test-encoder-fails --duration 12
check "AV1 frames" "$(wire_frames 0 0 106)" "0"
check_ge "H264 frames" "$(wire_frames 0 0 104)" 100
check "VP8 frames" "$(wire_frames 0 0 100)" "0"
check "fallback" "$(count 'GroupAv1: the AV1 encoder failed; the share goes on in H264' "$LOG")" "1"

# Both layers AV1 (--av1-screencast-layers every, the app's mode and the CLI's default): WebRTC's AV1 simulcast, L1T3
# on each encoding, an AV1 encoder per stream, the dependency descriptor on every AV1 packet of both layers.
# This one runs without --av1-screencast-layers: the default is the app's mode.
EVERY="--av1-screencast-layers every"
run av1-every $SHARE --sfu-av1 --av1-screencast --duration 15
check_ge "low layer AV1 frames" "$(wire_frames 0 0 106)" 100
check_ge "top layer AV1 frames" "$(wire_frames 0 1 106)" 100
check_le "low layer AV1 frame rate (fps)" "$(wire_fps 0 0 106)" 31
check_le "top layer AV1 frame rate (fps)" "$(wire_fps 0 1 106)" 31
check "H264 frames" "$(( $(wire_frames 0 0 104) + $(wire_frames 0 1 104) ))" "0"
check "dependency descriptor on every low layer AV1 packet" "$(wire_dd 0 0 106)" "$(wire_packets 0 0 106)"
check "dependency descriptor on every top layer AV1 packet" "$(wire_dd 0 1 106)" "$(wire_packets 0 1 106)"
check "decision" "$(grep -c -E 'GroupAv1: screen share layers: AV1, AV1$' "$LOG" || true)" "1"
check "AV1 simulcast" "$(count 'Av1TopLayer: the share is AV1 simulcast, an AV1 encoder per stream' "$LOG")" "1"
check_ge "L1T3 on both encodings" "$(grep -c -E 'Screencast send parameters for .*L1T3\}.*L1T3\}' "$LOG" || true)" 1

# Receivers alternate between the two AV1 layers.
run av1-every-quality-switch $SHARE $EVERY --sfu-av1 --av1-screencast --video-quality-switch 3000 --duration 21
check "video after every quality switch" "$(grep -m1 -E '^Video after quality switch:' "$OUT" | sed -E 's/.*: ([0-9]+)\/([0-9]+) stalled.*/\1 of \2/' | awk '{ if ($3 > 0) print $1; else print "no pairs" }')" "0"
check_ge "top layer AV1 frames" "$(wire_frames 0 1 106)" 100

# A failing AV1 encoder: WebRTC switches the sender to H264 on both layers (the factory's selector), never to VP8, and
# the scalability modes go.
run av1-every-encoder-fails $SHARE $EVERY --sfu-av1 --av1-screencast --av1-test-encoder-fails --duration 12
check "AV1 frames" "$(( $(wire_frames 0 0 106) + $(wire_frames 0 1 106) ))" "0"
check_ge "low layer H264 frames" "$(wire_frames 0 0 104)" 100
check_ge "top layer H264 frames" "$(wire_frames 0 1 104)" 100
check "VP8 frames" "$(( $(wire_frames 0 0 100) + $(wire_frames 0 1 100) ))" "0"
check "fallback" "$(count 'GroupAv1: the AV1 encoder failed; the share goes on in H264' "$LOG")" "1"

# The app's mode (the default), the server without AV1: both layers in H264, no scalability mode.
run av1-every-server-no-av1 $SHARE --av1-screencast --duration 10
check "AV1 on the wire" "$(av1_anywhere)" "0"
check_ge "low layer H264 frames" "$(wire_frames 0 0 104)" 100
check_ge "top layer H264 frames" "$(wire_frames 0 1 104)" 100
check "reason" "$(count 'GroupAv1: not sending AV1: server did not list AV1' "$LOG")" "1"
check "scalability modes" "$(grep -c -E 'Screencast send parameters for .*, L1T' "$LOG" || true)" "0"

run av1-custom-sharer $SHARE $TOP --sfu-av1 --av1-screencast --duration 15
check_ge "top layer AV1 frames" "$(wire_frames 0 1 106)" 100
check "top layer H264 frames" "$(wire_frames 0 1 104)" "0"
check_ge "low layer H264 frames" "$(wire_frames 0 0 104)" 100
check "low layer AV1 frames" "$(wire_frames 0 0 106)" "0"
check_le "top layer AV1 frame rate (fps)" "$(wire_fps 0 1 106)" 31
check_ge "low layer H264 frame rate (fps)" "$(wire_fps 0 0 104)" 45
check "AV1 from the camera senders" "$(grep -c -E '^Wire: P[1-3] .* pt 106:' "$OUT" || true)" "0"
check "every peer heard every sender, the screen audio too" "$(full 'Audio levels heard')" "1"
check "decision" "$(count 'GroupAv1: screen share layers: H264, AV1' "$LOG")" "1"
check "AV1 encoder fell back" "$(count 'GroupAv1: the AV1 encoder failed' "$LOG")" "0"
check_ge "SVT-AV1 encoders initialised" "$(count 'SvtAv1Encoder: initialized' "$LOG")" 1

# Receivers alternate between the sharer's low (H264) and top (AV1) layers: each receive stream switches decoder.
run av1-quality-switch $SHARE $TOP --sfu-av1 --av1-screencast --video-quality-switch 3000 --duration 21
check "video after every quality switch" "$(grep -m1 -E '^Video after quality switch:' "$OUT" | sed -E 's/.*: ([0-9]+)\/([0-9]+) stalled.*/\1 of \2/' | awk '{ if ($3 > 0) print $1; else print "no pairs" }')" "0"
check_ge "top layer AV1 frames" "$(wire_frames 0 1 106)" 100

run av1-server-no-av1 $SHARE $TOP --av1-screencast --duration 10
check "AV1 on the wire" "$(av1_anywhere)" "0"
check_ge "top layer H264 frames" "$(wire_frames 0 1 104)" 100
check "reason" "$(count 'GroupAv1: not sending AV1: server did not list AV1' "$LOG")" "1"

run av1-switch-off $SHARE --sfu-av1 --duration 10
check "AV1 on the wire" "$(av1_anywhere)" "0"
check_ge "top layer H264 frames" "$(wire_frames 0 1 104)" 100
check "decision lines" "$(count 'GroupAv1:' "$LOG")" "0"

# Encrypted: no AV1 (the frame transformer encrypts the OBU headers), and the screen audio's level reaches every peer
# through the frame trailer, which carries the share connection's own level (ExternalAudioRecorder measures it).
run av1-e2e $SHARE --sfu-av1 --av1-screencast --e2e --duration 15
check "every participant got audio" "$(full 'Audio received')" "1"
check "every peer heard every sender, the screen audio too" "$(full 'Audio levels heard')" "1"
check "AV1 on the wire" "$(av1_anywhere)" "0"
check "reason" "$(count 'GroupAv1: not sending AV1: encrypted' "$LOG")" "1"

run av1-camera --participants 2 --reference-participants 1 --sfu-av1 --av1-screencast --duration 10
check "AV1 on the wire" "$(av1_anywhere)" "0"
check_ge "reason (one per custom camera sender)" "$(count 'GroupAv1: not sending AV1: not the in-app screen share' "$LOG")" 2

run av1-encoder-fails $SHARE $TOP --sfu-av1 --av1-screencast --av1-test-encoder-fails --duration 12
check "top layer AV1 frames" "$(wire_frames 0 1 106)" "0"
check_ge "top layer H264 frames" "$(wire_frames 0 1 104)" 100
check_ge "top layer frame rate after the fallback (fps)" "$(wire_fps 0 1 104)" 40
check "fallback" "$(count 'GroupAv1: the AV1 encoder failed; the layer goes on in H264' "$LOG")" "1"

run av1-ingress-loss $SHARE $TOP --sfu-av1 --av1-screencast --sharer-ingress-loss 0.05 --duration 15
check_ge "top layer AV1 frames" "$(wire_frames 0 1 106)" 100
echo "    info AV1 RTX (107) packets = $(grep -E '^Wire: P0 \(screen\) layer 1 pt 107:' "$OUT" | sed -E 's/.*: ([0-9]+) packets.*/\1/' | head -1)"

# The same share from a reference-engine sharer (GroupInstanceReferenceImpl: the app's in-app share on the reference
# engine), participant 2; receivers 0 and 1 custom, 3 reference.
REF_SHARE="--participants 2 --reference-participants 2 --screencast-participants 2"

# Both layers AV1, the default: WebRTC's AV1 simulcast through PeerConnection, L1T3 on each encoding, the descriptor
# on every AV1 packet.
run av1-ref-every $REF_SHARE --sfu-av1 --av1-screencast --duration 15
check_ge "low layer AV1 frames" "$(wire_frames 2 0 106)" 100
check_ge "top layer AV1 frames" "$(wire_frames 2 1 106)" 100
check_le "low layer AV1 frame rate (fps)" "$(wire_fps 2 0 106)" 31
check_le "top layer AV1 frame rate (fps)" "$(wire_fps 2 1 106)" 31
check "H264 frames" "$(( $(wire_frames 2 0 104) + $(wire_frames 2 1 104) ))" "0"
check "dependency descriptor on every low layer AV1 packet" "$(wire_dd 2 0 106)" "$(wire_packets 2 0 106)"
check "dependency descriptor on every top layer AV1 packet" "$(wire_dd 2 1 106)" "$(wire_packets 2 1 106)"
check "decision" "$(grep -c -E 'GroupAv1: screen share layers: AV1, AV1$' "$LOG" || true)" "1"
check "AV1 simulcast" "$(count 'Av1TopLayer: the share is AV1 simulcast, an AV1 encoder per stream' "$LOG")" "1"
check_ge "L1T3 on both encodings" "$(grep -c -E 'Screencast send parameters for .*L1T3\}.*L1T3\}' "$LOG" || true)" 1
check "every peer heard every sender, the screen audio too" "$(full 'Audio levels heard')" "1"

run av1-ref-every-quality-switch $REF_SHARE $EVERY --sfu-av1 --av1-screencast --video-quality-switch 3000 --duration 21
check "video after every quality switch" "$(grep -m1 -E '^Video after quality switch:' "$OUT" | sed -E 's/.*: ([0-9]+)\/([0-9]+) stalled.*/\1 of \2/' | awk '{ if ($3 > 0) print $1; else print "no pairs" }')" "0"
check_ge "top layer AV1 frames" "$(wire_frames 2 1 106)" 100

run av1-ref-every-encoder-fails $REF_SHARE $EVERY --sfu-av1 --av1-screencast --av1-test-encoder-fails --duration 12
check "AV1 frames" "$(( $(wire_frames 2 0 106) + $(wire_frames 2 1 106) ))" "0"
check_ge "low layer H264 frames" "$(wire_frames 2 0 104)" 100
check_ge "top layer H264 frames" "$(wire_frames 2 1 104)" 100
check "VP8 frames" "$(( $(wire_frames 2 0 100) + $(wire_frames 2 1 100) ))" "0"
check "fallback" "$(count 'GroupAv1: the AV1 encoder failed; the share goes on in H264' "$LOG")" "1"

run av1-ref-server-no-av1 $REF_SHARE --av1-screencast --duration 10
check "AV1 on the wire" "$(av1_anywhere)" "0"
check_ge "low layer H264 frames" "$(wire_frames 2 0 104)" 100
check_ge "top layer H264 frames" "$(wire_frames 2 1 104)" 100
check "reason" "$(count 'GroupAv1: not sending AV1: server did not list AV1' "$LOG")" "1"
check "scalability modes" "$(grep -c -E 'Screencast send parameters for .*, L1T' "$LOG" || true)" "0"

run av1-ref-single $REF_SHARE $SINGLE --sfu-av1 --av1-screencast --duration 15
check_ge "AV1 frames" "$(wire_frames 2 0 106)" 100
check_le "AV1 frame rate (fps)" "$(wire_fps 2 0 106)" 31
check "H264 frames" "$(wire_frames 2 0 104)" "0"
check "second layer" "$(grep -c -E '^Wire: P2 \(screen\) layer 1 ' "$OUT" || true)" "0"
check "decision" "$(grep -c -E 'GroupAv1: screen share layers: AV1$' "$LOG" || true)" "1"
check "one AV1 stream" "$(count 'Av1TopLayer: the share is one AV1 stream' "$LOG")" "1"
check "dependency descriptor on every AV1 packet (default ID 11)" "$(wire_dd 2 0 106)" "$(wire_packets 2 0 106)"
check "dependency descriptor ID" "$(count 'GroupAv1: dependency descriptor as header extension 11' "$LOG")" "1"
check "every peer heard every sender, the screen audio too" "$(full 'Audio levels heard')" "1"

# The SFU lists the descriptor at 12: the share sends it there.
run av1-ref-single-dd-listed $REF_SHARE $SINGLE --sfu-av1 --sfu-dd-id 12 --av1-screencast --duration 10
check_ge "AV1 frames" "$(wire_frames 2 0 106)" 100
check "dependency descriptor on every AV1 packet (ID 12)" "$(wire_dd 2 0 106)" "$(wire_packets 2 0 106)"
check "dependency descriptor ID" "$(count 'GroupAv1: dependency descriptor as header extension 12' "$LOG")" "1"

# Encrypted, a reference-engine sharer: no AV1, and its screen audio's level reaches the custom peers through the frame
# trailer.
run av1-ref-e2e $REF_SHARE --sfu-av1 --av1-screencast --e2e --duration 15
check "AV1 on the wire" "$(av1_anywhere)" "0"
check "reason" "$(count 'GroupAv1: not sending AV1: encrypted' "$LOG")" "1"
check "every peer heard every sender, the screen audio too" "$(full 'Audio levels heard')" "1"

run av1-ref-top $REF_SHARE $TOP --sfu-av1 --av1-screencast --duration 15
check_ge "top layer AV1 frames" "$(wire_frames 2 1 106)" 100
check "top layer H264 frames" "$(wire_frames 2 1 104)" "0"
check_ge "low layer H264 frames" "$(wire_frames 2 0 104)" 100
check "low layer AV1 frames" "$(wire_frames 2 0 106)" "0"
check_le "top layer AV1 frame rate (fps)" "$(wire_fps 2 1 106)" 31
check "decision" "$(count 'GroupAv1: screen share layers: H264, AV1' "$LOG")" "1"

if [ "$fail" -eq 0 ]; then
    echo; echo "all group AV1 scenarios passed (outputs in $TMP)"
else
    echo; echo "SOME GROUP AV1 SCENARIOS FAILED (outputs in $TMP)"
fi
exit "$fail"
