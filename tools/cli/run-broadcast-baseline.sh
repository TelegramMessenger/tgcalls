#!/bin/bash
# Runs the broadcast testbench matrix with today's engine (tgcalls_cli --mode broadcast) and leaves one JSON report
# per run in the output directory; broadcast_report.py aggregates them into a baseline or compares them against one.
#
# Matrix: scenarios audio, tiles, unified x profiles wifi, lte, poor-lte, 3g, step-down x events none /
# notready-burst@8,resync@14,join-missing@18, plus handoff x the five profiles without events; -r repetitions each
# (default 10: the baseline is 10 runs a cell, a candidate 5; broadcast_report.py explains the calibration).
# Runs last 25 s, step-down 90 s (its steps are at 30 s and 60 s). A run whose report already exists is skipped, so
# an interrupted matrix resumes.
#
# Run i of R joins at its own phase of the server's 1 s parts: it waits until the wall clock's millisecond is
# i*1000/R. The emulated server's timeline is the wall clock, and a run's results depend on where it joins within a
# part; under -j 2 two runs start at the same instant and equal durations keep them in lockstep, so until the Plan 1
# review the two runs of a pair joined at one phase: identical frame counts in 17 of 50 tiles pairs and 26 of 50
# unified pairs, against 2 of 40 and 7 of 40 for neighbouring runs that did not run together, and audio start within
# 20 ms in 108 of 175 pairs against 16 of 140. 10 runs a cell were 5 samples. Staggered, the pairs read as their
# controls: 4 of 50 against 1 of 40, 8 of 50 against 5 of 40, 77 of 175 against 62 of 140 (audio start moves in steps
# of ~120-400 ms with the phase, so runs 100 ms apart often share one). A sleep of whole seconds would not help: the
# part period is 1 s.
#
# Run from the repository root after building //submodules/TgVoipWebrtc/tgcalls/tools/cli:tgcalls_cli:
#   submodules/TgVoipWebrtc/tgcalls/tools/cli/run-broadcast-baseline.sh -j 2 -r 10 -o /tmp/broadcast-reports
#   python3 submodules/TgVoipWebrtc/tgcalls/tools/cli/broadcast_report.py aggregate /tmp/broadcast-reports \
#       submodules/TgVoipWebrtc/tgcalls/tools/cli/broadcast-baseline
set -euo pipefail

JOBS=2
REPS=10
OUT=broadcast-reports
ONLY=""
CLI=./bazel-bin/submodules/TgVoipWebrtc/tgcalls/tools/cli/tgcalls_cli

while [ $# -gt 0 ]; do
    case "$1" in
        -j) JOBS="$2"; shift 2 ;;
        -r) REPS="$2"; shift 2 ;;
        -o) OUT="$2"; shift 2 ;;
        --only) ONLY="$2"; shift 2 ;;
        --cli) CLI="$2"; shift 2 ;;
        *) echo "usage: $0 [-j jobs] [-r repetitions] [-o dir] [--only scenario] [--cli path]" >&2; exit 2 ;;
    esac
done

if [ ! -x "$CLI" ]; then
    echo "no tgcalls_cli at $CLI: build //submodules/TgVoipWebrtc/tgcalls/tools/cli:tgcalls_cli first" >&2
    exit 1
fi
mkdir -p "$OUT"

JOB_LIST=$(mktemp)
trap 'rm -f "$JOB_LIST"' EXIT
for scenario in audio tiles unified handoff; do
    if [ -n "$ONLY" ] && [ "$ONLY" != "$scenario" ]; then
        continue
    fi
    for profile in wifi lte poor-lte 3g step-down; do
        for events in none events; do
            if [ "$scenario" = handoff ] && [ "$events" = events ]; then
                continue
            fi
            for ((rep = 0; rep < REPS; rep++)); do
                echo "$scenario $profile $events $rep" >> "$JOB_LIST"
            done
        done
    done
done

run_one() {
    local scenario=$1 profile=$2 events=$3 rep=$4
    local report="$OUT/$scenario-$profile-$events-$rep.json"
    if [ -s "$report" ]; then
        return 0
    fi
    local duration=25
    if [ "$profile" = step-down ]; then
        duration=90
    fi
    local args=(--mode broadcast --broadcast-scenario "$scenario" --broadcast-profile "$profile" --duration "$duration"
                --repetition "$rep" --broadcast-report "$report" --quiet)
    if [ "$events" = events ]; then
        args+=(--broadcast-events "notready-burst@8,resync@14,join-missing@18")
    fi
    python3 -c 'import sys, time; time.sleep(((int(sys.argv[1]) - time.time() * 1000) % 1000) / 1000)' \
        $((rep * 1000 / REPS))
    if "$CLI" "${args[@]}" > "$OUT/$scenario-$profile-$events-$rep.log" 2>&1; then
        rm -f "$OUT/$scenario-$profile-$events-$rep.log"
        echo "done   $scenario $profile $events $rep"
    else
        echo "FAILED $scenario $profile $events $rep (log kept)"
    fi
}
export -f run_one
export OUT CLI REPS

echo "$(wc -l < "$JOB_LIST" | tr -d ' ') runs, $JOBS at a time, reports in $OUT"
xargs -P "$JOBS" -L 1 bash -c 'run_one "$@"' _ < "$JOB_LIST"
echo "$(ls "$OUT"/*.json 2>/dev/null | wc -l | tr -d ' ') reports in $OUT"
