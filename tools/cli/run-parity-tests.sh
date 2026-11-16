#!/usr/bin/env zsh
# Parity of InstanceV2CompatImpl under a legacy call version against InstanceV2Impl under the
# same version: is compat+V no worse than V+V? Every scenario runs for three pairings on the
# in-process p2p harness:
#   V-V    the baseline, InstanceV2Impl on both ends
#   cV-V   CompatImpl as the caller (--version compat:V)
#   V-cV   CompatImpl as the callee (--version2 compat:V)
# -c SPEC gives CompatImpl's ends another --version: `-c 14.0.0` runs the pairing the server
# serves (14.0.0 against 8.0.0), which must measure the same as compat:8.0.0.
# and parity_report.py prints, per scenario and pairing, the failure counts (no connection,
# a side that hears nothing of the other's tone, a side that gets no video) with a one-sided
# Fisher p wherever a CompatImpl pairing is worse than the baseline, and p50/p90 of the
# connection time and of each side's first audio and first video.
#
#   ./submodules/TgVoipWebrtc/tgcalls/tools/cli/run-parity-tests.sh [-v VERSION] [-c SPEC] [-j PARALLEL] [-o DIR] [-l] [CLI] [SCENARIO...]
#
# -l keeps each call's engine log next to its summary (DIR/<scenario>.<pairing>.<n>.log), which
# is what a failure has to be traced from. VERSION defaults to 8.0.0, PARALLEL to 16: every call
# encodes and decodes on both sides, and an oversubscribed host delays CompatImpl's answers (two
# posted tasks) more than InstanceV2Impl's (synchronous), which biases exactly what is measured.
# CLI defaults to the bazel-bin binary. With no SCENARIO all run (528 calls, ~10 min at -j 16):
#   audio-delay, video-delay, videolate-delay   100-300 ms signaling delay, 16 runs each
#   mute-caller, mute-callee                    muted at start, unmuted at 4 s, 8 runs each
#   audio-loss30, video-loss30                  30% signaling loss, 32 runs each
#   videolate-loss30                            30% loss, cameras at 3 s (callee) / 5 s (caller),
#                                               48 runs: the glare stress case
# At these sample sizes a rate difference shows direction, not equivalence.
set -u
VERSION=8.0.0
COMPAT=""
PAR=16
LOGS=0
OUT="${TMPDIR:-/tmp}/tgcalls-parity-$$"
while getopts "v:c:j:o:l" opt; do
    case $opt in
        v) VERSION=$OPTARG ;;
        c) COMPAT=$OPTARG ;;
        j) PAR=$OPTARG ;;
        o) OUT=$OPTARG ;;
        l) LOGS=1 ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ -z "$COMPAT" ] && COMPAT="compat:$VERSION"
CLI="${1:-./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli}"
(( $# > 0 )) && shift
if [ ! -x "$CLI" ]; then echo "no CLI at $CLI"; exit 2; fi
CLI=${CLI:A}
mkdir -p "$OUT/jobs"
rm -f "$OUT"/jobs/*.sh(N)

typeset -A ARGS RUNS
ARGS[audio-delay]="--duration 10 --delay 100-300";                                         RUNS[audio-delay]=16
ARGS[video-delay]="--duration 10 --video --delay 100-300";                                 RUNS[video-delay]=16
ARGS[videolate-delay]="--duration 12 --video-late --delay 100-300";                        RUNS[videolate-delay]=16
ARGS[mute-caller]="--duration 10 --mute-participants 0 --unmute-after 4 --delay 100-300";  RUNS[mute-caller]=8
ARGS[mute-callee]="--duration 10 --mute-participants 1 --unmute-after 4 --delay 100-300";  RUNS[mute-callee]=8
ARGS[audio-loss30]="--duration 20 --drop-rate 0.3 --delay 50-200";                         RUNS[audio-loss30]=32
ARGS[video-loss30]="--duration 20 --video --drop-rate 0.3 --delay 50-200";                 RUNS[video-loss30]=32
ARGS[videolate-loss30]="--duration 30 --video-late --drop-rate 0.3 --delay 50-200";        RUNS[videolate-loss30]=48

scenarios=("$@")
(( ${#scenarios} == 0 )) && scenarios=(${(k)ARGS})
for sc in $scenarios; do
    if [ -z "${ARGS[$sc]:-}" ]; then echo "unknown scenario $sc"; exit 2; fi
    for pair in "V-V $VERSION $VERSION" "cV-V $COMPAT $VERSION" "V-cV $VERSION $COMPAT"; do
        set -- ${=pair}
        for r in $(seq 1 ${RUNS[$sc]}); do
            log=""
            (( LOGS )) && log="--log-file '$OUT/$sc.$1.$r.log'"
            echo "'$CLI' --mode p2p --version $2 --version2 $3 ${ARGS[$sc]} $log --quiet > '$OUT/$sc.$1.$r.out' 2>/dev/null" > "$OUT/jobs/$sc.$1.$r.sh"
        done
    done
done
echo "running $(ls "$OUT"/jobs/*.sh | wc -l | tr -d ' ') calls, $PAR at a time, into $OUT"
# Shuffled so every pairing sees the same load.
ls "$OUT"/jobs/*.sh | sort -R | xargs -P "$PAR" -n 1 zsh
python3 -I "${0:A:h}/parity_report.py" "$OUT"
