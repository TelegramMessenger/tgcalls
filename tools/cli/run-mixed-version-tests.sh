#!/usr/bin/env bash
# Calls between two CLI builds, one side per process (--split): an OLD build against a NEW one, as an app on one WebRTC
# release calls an app on another. Every pairing is also run OLD against OLD, and its codecs on the wire are the
# reference: a codec that differs from OLD<->OLD's is a FAIL even when the call works.
#
#   p2p:   every call version below, OLD caller -> NEW callee and NEW caller -> OLD callee, with video. A side's codec
#          is the one it decoded most frames of ("Video receive codec:"), so caller->callee is the callee's. Then the
#          versions with an mtproto media transport again over it (`network_use_mtproto`; the PeerConnection engines'
#          MtProtoDtlsTransport and 13.0.0's custom one, whose framing must match across builds).
#   group: the host process (the Go SFU and its participants) OLD and the joining process NEW, then the reverse, for
#          custom<->custom, reference<->reference, custom<->reference, --e2e and --av1-screencast, with video. The
#          codecs are each sender's layers and payload types the SFU counted ("Wire:" lines, >= 30 frames).
#
#   ./submodules/TgVoipWebrtc/tgcalls/tools/cli/run-mixed-version-tests.sh OLD_CLI NEW_CLI [-o DIR] [-k p2p|group]
#       [-v "VERSION ..."] [-d P2P_SECONDS] [-g GROUP_SECONDS]
#
# One line per pairing: PASS or FAIL <reason>, and the codecs. Outputs (stdout and stderr of each process) are kept in
# DIR. The sockets live under /tmp with short names: a Unix socket path holds about 104 bytes.
set -u
if [ $# -lt 2 ]; then
    echo "usage: $0 OLD_CLI NEW_CLI [-o DIR] [-k p2p|group] [-v \"VERSION ...\"] [-d P2P_SECONDS] [-g GROUP_SECONDS]"
    exit 2
fi
OLD=$1; NEW=$2; shift 2
OUT="${TMPDIR:-/tmp}/tgcalls-mixed-$$"
KINDS="p2p group"
VERSIONS="7.0.0 8.0.0 9.0.0 10.0.0 11.0.0 12.0.0 13.0.0 14.0.0 18.0.0"
MTPROTO_VERSIONS="11.0.0 13.0.0 18.0.0"
P2P_SECONDS=10
GROUP_SECONDS=15
while [ $# -gt 0 ]; do
    case $1 in
        -o) OUT=$2; shift 2 ;;
        -k) KINDS=$2; shift 2 ;;
        -v) VERSIONS=$2; shift 2 ;;
        -d) P2P_SECONDS=$2; shift 2 ;;
        -g) GROUP_SECONDS=$2; shift 2 ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
done
for cli in "$OLD" "$NEW"; do
    if [ ! -x "$cli" ]; then echo "no CLI at $cli"; exit 2; fi
done
mkdir -p "$OUT"
echo "OLD: $OLD"
echo "NEW: $NEW"
echo "outputs in $OUT"
fail=0
passes=0
fails=0
seq_no=0

# run_pair <name> <cliA> <argsA> <cliB> <argsB> <seconds>: both processes at once on a fresh socket; sets EA, EB.
run_pair() {
    local name=$1 cliA=$2 argsA=$3 cliB=$4 argsB=$5 seconds=$6
    seq_no=$((seq_no + 1))
    local sock="/tmp/tgmv.$$.$seq_no.sock"
    rm -f "$sock"
    # shellcheck disable=SC2086
    "$cliA" $argsA --split-socket "$sock" > "$OUT/$name.a.out" 2> "$OUT/$name.a.err" &
    local pa=$!
    # shellcheck disable=SC2086
    "$cliB" $argsB --split-socket "$sock" > "$OUT/$name.b.out" 2> "$OUT/$name.b.err" &
    local pb=$!
    # A side that hangs past every timeout of its own is killed, and fails.
    ( sleep $((seconds + 120)); kill -9 $pa $pb 2>/dev/null ) &
    local wd=$!
    wait $pa; EA=$?
    wait $pb; EB=$?
    kill $wd 2>/dev/null; wait $wd 2>/dev/null
    rm -f "$sock"
}

# split_line <file>: the split role line, empty when the binary did not run split.
split_line() { grep -m1 -E '^Split: ' "$1" 2>/dev/null; }

# receive_codec <file>: the codec a p2p side decoded most frames of, "none" without one.
receive_codec() {
    local c; c=$(grep -m1 -E '^Video receive codec: ' "$1" 2>/dev/null | sed -E 's/^Video receive codec: +([^ ]+).*/\1/')
    echo "${c:-none}"
}

# wire_codecs <file>...: "P<id> pt <PT>" for every sender's media payload types that carried >= 30 frames on some
# layer, sorted. Layers are left out (how many a sender keeps up depends on the bandwidth estimate, run to run), and so
# are the RTX payload types of the group table (101, 103, 105, 107: padding probes come and go).
wire_codecs() {
    cat "$@" 2>/dev/null | grep -E '^Wire: P[0-9]+( \(screen\))? layer [0-9]+ pt [0-9]+: ' \
        | sed -E 's/^Wire: (P[0-9]+)( \(screen\))? layer ([0-9]+) pt ([0-9]+): [0-9]+ packets, ([0-9]+) frames.*/\5 \1 pt \4/' \
        | awk '$1 >= 30 && $4 != 101 && $4 != 103 && $4 != 105 && $4 != 107 { print $2 " pt " $4 }' | sort -u | paste -sd, -
}

report() { # report <line> <passed>
    if [ "$2" = 1 ]; then passes=$((passes + 1)); else fails=$((fails + 1)); fail=1; fi
    echo "$1"
}

# p2p_side <label> <caller CLI> <callee CLI> <version> [<transport>]: sets P2P_RESULT ("" when it passed, else the
# reason) and P2P_CODECS ("caller->callee X, callee->caller Y"). transport: "mtproto" for network_use_mtproto.
p2p_side() {
    local label=$1 caller=$2 callee=$3 version=$4 transport=${5:-}
    local common="--mode p2p --version $version --video --duration $P2P_SECONDS --quiet"
    local name="p2p.$version.$label"
    if [ "$transport" = mtproto ]; then
        common="$common --custom-params {\"network_use_mtproto\":true}"
        name="p2p.$version.mtproto.$label"
    fi
    run_pair "$name" "$caller" "--split caller $common" "$callee" "--split callee $common" "$P2P_SECONDS"
    local a="$OUT/$name.a.out" b="$OUT/$name.b.out"
    local forward backward
    forward=$(receive_codec "$b")
    backward=$(receive_codec "$a")
    P2P_CODECS="caller->callee $forward, callee->caller $backward"
    P2P_RESULT=""
    if [ -z "$(split_line "$a")" ] || [ -z "$(split_line "$b")" ]; then
        P2P_RESULT="no split summary (a binary without --split?)"
    elif [ "$EA" != 0 ] || [ "$EB" != 0 ]; then
        P2P_RESULT="exit codes caller $EA, callee $EB"
    elif [ "$forward" = none ] || [ "$backward" = none ]; then
        P2P_RESULT="no video codec decoded"
    fi
}

# p2p_version <version> [<transport>]: OLD->OLD as the reference, then OLD->NEW and NEW->OLD against it.
p2p_version() {
    local version=$1 transport=${2:-} title=$1
    [ -n "${2:-}" ] && title="$1 $2"
    p2p_side OLD-OLD "$OLD" "$OLD" "$version" "$transport"
    local base_result=$P2P_RESULT base_codecs=$P2P_CODECS
    if [ -z "$base_result" ]; then
        report "p2p $title OLD->OLD PASS  $base_codecs" 1
    else
        report "p2p $title OLD->OLD FAIL $base_result  ($base_codecs)" 0
    fi
    local pairing label
    for pairing in "OLD->NEW $OLD $NEW" "NEW->OLD $NEW $OLD"; do
        set -- $pairing
        label=$1; label=${label/->/-}
        p2p_side "$label" "$2" "$3" "$version" "$transport"
        if [ -z "$P2P_RESULT" ] && [ -n "$base_result" ]; then
            P2P_RESULT="no OLD->OLD reference"
        elif [ -z "$P2P_RESULT" ] && [ "$P2P_CODECS" != "$base_codecs" ]; then
            P2P_RESULT="codecs differ from OLD->OLD ($base_codecs)"
        fi
        if [ -z "$P2P_RESULT" ]; then
            report "p2p $title $1 PASS  $P2P_CODECS" 1
        else
            report "p2p $title $1 FAIL $P2P_RESULT  ($P2P_CODECS)" 0
        fi
    done
}

if [[ " $KINDS " == *" p2p "* ]]; then
    echo "== p2p"
    for version in $VERSIONS; do
        p2p_version "$version"
    done
    for version in $MTPROTO_VERSIONS; do
        [[ " $VERSIONS " == *" $version "* ]] && p2p_version "$version" mtproto
    done
fi

# group_side <label> <host CLI> <join CLI> <host args> <join args> <join id offset>: sets GROUP_RESULT and GROUP_CODECS.
group_side() {
    local label=$1 host=$2 join=$3 hostArgs=$4 joinArgs=$5 offset=$6
    local common="--mode group --video --duration $GROUP_SECONDS --quiet"
    run_pair "$label" "$host" "--split host $common $hostArgs" "$join" \
        "--split join --participant-id-offset $offset $common $joinArgs" "$GROUP_SECONDS"
    local a="$OUT/$label.a.out" b="$OUT/$label.b.out"
    GROUP_CODECS=$(wire_codecs "$a" "$b")
    GROUP_RESULT=""
    if [ -z "$(split_line "$a")" ] || [ -z "$(split_line "$b")" ]; then
        GROUP_RESULT="no split summary (a binary without --split?)"
    elif [ "$EA" != 0 ] || [ "$EB" != 0 ]; then
        GROUP_RESULT="exit codes host $EA, join $EB"
    elif [ -z "$GROUP_CODECS" ]; then
        GROUP_RESULT="no video on the wire"
    fi
}

if [[ " $KINDS " == *" group "* ]]; then
    echo "== group"
    # name | host participants | join participants | options for both | host-only options (the SFU's)
    while IFS='|' read -r name hostP joinP both hostOnly; do
        [ -z "$name" ] && continue
        offset=$(echo "$hostP" | awk '{ n = 0; for (i = 1; i < NF; i++) if ($i == "--participants" || $i == "--reference-participants") n += $(i + 1); print n }')
        group_side "group.$name.OLD-OLD" "$OLD" "$OLD" "$hostP $both $hostOnly" "$joinP $both" "$offset"
        base_result=$GROUP_RESULT; base_codecs=$GROUP_CODECS
        if [ -z "$base_result" ]; then
            report "group $name host OLD, join OLD PASS  $base_codecs" 1
        else
            report "group $name host OLD, join OLD FAIL $base_result  ($base_codecs)" 0
        fi
        for pairing in "OLD NEW $OLD $NEW" "NEW OLD $NEW $OLD"; do
            set -- $pairing
            group_side "group.$name.$1-$2" "$3" "$4" "$hostP $both $hostOnly" "$joinP $both" "$offset"
            if [ -z "$GROUP_RESULT" ] && [ -n "$base_result" ]; then
                GROUP_RESULT="no OLD<->OLD reference"
            elif [ -z "$GROUP_RESULT" ] && [ "$GROUP_CODECS" != "$base_codecs" ]; then
                GROUP_RESULT="codecs differ from OLD<->OLD ($base_codecs)"
            fi
            if [ -z "$GROUP_RESULT" ]; then
                report "group $name host $1, join $2 PASS  $GROUP_CODECS" 1
            else
                report "group $name host $1, join $2 FAIL $GROUP_RESULT  ($GROUP_CODECS)" 0
            fi
        done
    done <<'EOF'
custom|--participants 2|--participants 2||
reference|--participants 0 --reference-participants 2|--participants 0 --reference-participants 2||
custom-reference|--participants 2|--participants 0 --reference-participants 2||
e2e|--participants 1 --reference-participants 1|--participants 1 --reference-participants 1|--e2e|
av1-screencast|--participants 1 --reference-participants 1 --screencast-participants 0|--participants 1 --reference-participants 1 --screencast-participants 0|--av1-screencast|--sfu-av1
EOF
fi

echo
echo "$passes passed, $fails failed"
if [ "$fail" = 0 ]; then echo "ALL MIXED-VERSION PAIRINGS PASS"; else echo "SOME MIXED-VERSION PAIRINGS FAILED (outputs in $OUT)"; fi
exit $fail
