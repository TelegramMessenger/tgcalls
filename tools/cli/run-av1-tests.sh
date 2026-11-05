#!/usr/bin/env bash
# AV1 in InstanceV2ReferenceImpl (10.0.0 / 11.0.0) on the in-process p2p harness. Each scenario runs one call and
# asserts on the engine log, whose AV1 lines name the side:
#   "InstanceV2ReferenceImpl: AV1 offered, encoding allowed: <0|1> (<caller|callee>)" (or "AV1 not offered")
#   "InstanceV2ReferenceImpl: video send codec pinned to <codec> (<caller|callee>)"
#
#   ./submodules/TgVoipWebrtc/tgcalls/tools/cli/run-av1-tests.sh [path/to/tgcalls_cli]
#
# The fake platform has no H.265, so a side that may not send AV1 pins H264.
set -u
CLI="${1:-./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli}"
if [ ! -x "$CLI" ]; then echo "no CLI at $CLI"; exit 2; fi
TMP="${TMPDIR:-/tmp}/tgcalls-av1-$$"; mkdir -p "$TMP"
fail=0

count() { grep -c -F -- "$1" "$2" 2>/dev/null || true; }
check() { # check <label> <actual> <expected>
    if [ "$2" = "$3" ]; then echo "    ok   $1 = $2"; else echo "    FAIL $1 = $2 (expected $3)"; fail=1; fi
}
check_ge() { # check_ge <label> <actual> <minimum>
    if [ "$2" -ge "$3" ] 2>/dev/null; then echo "    ok   $1 = $2 (>= $3)"; else echo "    FAIL $1 = $2 (expected >= $3)"; fail=1; fi
}
pins() { # pins <side>: the side's pinned codecs in order, space-separated
    grep -o -- "video send codec pinned to [A-Za-z0-9]* ($1)" "$LOG" | awk '{print $6}' | tr '\n' ' ' | sed 's/ $//'
}
last_pin() { pins "$1" | awk '{print $NF}'; }
send_fps() { # send_fps <side>: the side's best send frame rate over its "video stats" intervals, whole fps
    grep -o -E "video stats \($1\): send [A-Za-z0-9]+ \([A-Za-z0-9]+\) [0-9]+x[0-9]+, [0-9.]+ fps" "$LOG" \
        | sed -E 's/.* ([0-9.]+) fps$/\1/' | sort -n | tail -1 | cut -d. -f1
}
frames() { # frames <caller|callee>: decoded frames that side received
    local line; line=$(grep -m1 'Video frames:' "$OUT")
    if [ "$1" = caller ]; then echo "$line" | sed -E 's/.*caller received ([0-9]+).*/\1/'; else echo "$line" | sed -E 's/.*callee received ([0-9]+).*/\1/'; fi
}

run() { # run <name> <cli args...>
    local name="$1"; shift
    LOG="$TMP/$name.log"; OUT="$TMP/$name.out"
    echo "== $name"
    "$CLI" --mode p2p --duration 8 --log-file "$LOG" "$@" > "$OUT" 2>&1
    check "exit code" "$?" "0"
}

both_frames() {
    check_ge "frames received by the caller" "$(frames caller)" 1
    check_ge "frames received by the callee" "$(frames callee)" 1
}

# 12 s: the first "video stats" line comes 10 s in (a sample every 5 s, a line per interval).
run av1-both --version 11.0.0 --video --duration 12
check "caller's last pin" "$(last_pin caller)" "AV1"
check "callee's last pin" "$(last_pin callee)" "AV1"
check_ge "SVT-AV1 encoders initialised" "$(count 'SvtAv1Encoder: initialized' "$LOG")" 2
check "encoding refused" "$(count 'encoding refused' "$LOG")" "0"
check "SetLocalDescription calls" "$(count 'Calling SetLocalDescription' "$LOG")" "2"
check_ge "caller's stats: sends AV1, receives AV1 with dav1d" \
    "$(grep -c -E 'video stats \(caller\): send AV1 \(SvtAv1\) [0-9]+x[0-9]+, .*; receive AV1 \(dav1d\) [0-9]+x[0-9]+, .*; process CPU [0-9]+%' "$LOG")" 1
check_ge "callee's stats: sends AV1, receives AV1 with dav1d" \
    "$(grep -c -E 'video stats \(callee\): send AV1 \(SvtAv1\) [0-9]+x[0-9]+, .*; receive AV1 \(dav1d\) [0-9]+x[0-9]+, .*; process CPU [0-9]+%' "$LOG")" 1
# The fake camera runs at 30 fps and the capture interface caps a call's video at 30 (the cap was 25).
check_ge "caller's send frame rate (fps)" "$(send_fps caller)" 28
check_ge "callee's send frame rate (fps)" "$(send_fps callee)" 28
both_frames

run av1-incapable-callee --version 11.0.0 --video --custom-params2 '{"video_av1_test_device_incapable":true}'
check "callee may not encode AV1" "$(count 'AV1 offered, encoding allowed: 0 (callee)' "$LOG")" "1"
check "caller's last pin" "$(last_pin caller)" "AV1"
check "callee's last pin" "$(last_pin callee)" "H264"
check "callee pinned AV1" "$(count 'pinned to AV1 (callee)' "$LOG")" "0"
# The encoder's log line does not name its side; with the callee never pinned to AV1, these are the caller's.
check_ge "SVT-AV1 encoders initialised" "$(count 'SvtAv1Encoder: initialized' "$LOG")" 1
both_frames

run av1-incapable-answerer-av1-first --version 11.0.0 --video \
    --custom-params '{"video_av1_test_offer_av1_first":true}' --custom-params2 '{"video_av1_test_device_incapable":true}'
check "callee may not encode AV1" "$(count 'AV1 offered, encoding allowed: 0 (callee)' "$LOG")" "1"
check "callee pinned AV1" "$(count 'pinned to AV1 (callee)' "$LOG")" "0"
check "callee's last pin" "$(last_pin callee)" "H264"
both_frames
echo "    info encoding refused = $(count 'encoding refused' "$LOG") (0: the pin alone held)"

run av1-killswitch-offer --version 11.0.0 --video --custom-params '{"video_av1":false}' --custom-params2 '{"video_av1":false}'
check "AV1 not offered" "$(count 'AV1 not offered' "$LOG")" "2"
check "AV1 pins" "$(count 'pinned to AV1' "$LOG")" "0"
check "SVT-AV1 encoders initialised" "$(count 'SvtAv1Encoder: initialized' "$LOG")" "0"
both_frames

# A peer without AV1 gets today's call: AV1 is not negotiated, so neither side pins a codec.
run av1-peer-without-av1 --version 11.0.0 --video --custom-params '{"video_av1":false}'
check "AV1 not offered" "$(count 'AV1 not offered (caller)' "$LOG")" "1"
check "callee offers AV1" "$(count 'AV1 offered, encoding allowed: 1 (callee)' "$LOG")" "1"
check "pins" "$(count 'video send codec pinned to' "$LOG")" "0"
check "SVT-AV1 encoders initialised" "$(count 'SvtAv1Encoder: initialized' "$LOG")" "0"
both_frames

run av1-killswitch-send --version 11.0.0 --video --custom-params '{"video_av1_send":false}'
check "caller may not encode AV1" "$(count 'AV1 offered, encoding allowed: 0 (caller)' "$LOG")" "1"
check "caller's last pin" "$(last_pin caller)" "H264"
check "callee's last pin" "$(last_pin callee)" "AV1"
both_frames

run av1-v10 --version 10.0.0 --video
check "caller's last pin" "$(last_pin caller)" "AV1"
check "callee's last pin" "$(last_pin callee)" "AV1"
both_frames

run av1-late-camera --version 11.0.0 --video-late --duration 10
check "caller's last pin" "$(last_pin caller)" "AV1"
check "callee's last pin" "$(last_pin callee)" "AV1"
# Once a side has pinned AV1 it stays on AV1 through the other side's later camera offer.
for side in caller callee; do
    after=$(pins "$side" | sed -E 's/^[^A]*AV1//')
    check "$side pins after its first AV1 are all AV1" "$(echo "$after" | tr ' ' '\n' | grep -v -c -e '^AV1$' -e '^$' || true)" "0"
done
both_frames

if [ "${AV1_TESTS_STAGE:-}" != "engine-only" ]; then
    run av1-power-flip --version 11.0.0 --video --duration 10 --av1-power-off-ms caller:3000 --av1-power-on-ms caller:6000
    check "caller's pins" "$(pins caller | tr ' ' '\n' | uniq | tr '\n' ' ' | sed 's/ $//')" "AV1 H264 AV1"
    check "SetLocalDescription calls" "$(count 'Calling SetLocalDescription' "$LOG")" "2"
    check_ge "frames received by the callee" "$(frames callee)" 200

    run av1-power-off-before-camera --version 11.0.0 --av1-power-off-ms caller:300 --video-late-caller-ms 2000
    check "caller pinned AV1" "$(count 'pinned to AV1 (caller)' "$LOG")" "0"
    check "caller's first pin" "$(pins caller | awk '{print $1}')" "H264"
    check_ge "frames received by the callee" "$(frames callee)" 1
fi

echo
if [ "$fail" = 0 ]; then echo "all AV1 scenarios passed (logs in $TMP)"; else echo "AV1 scenarios FAILED (logs in $TMP)"; fi
exit "$fail"
