#!/bin/sh
# Psychtoolbox's Flip against ysp/screen.h's GLX_OML, each against the
# kernel's vblank times. README.md in this folder explains it.
#   sh run.sh OUT_DIR [rev]
# Needs: octave-cli with Psychtoolbox 3 on its path, a flipstats build
# (FLIPSTATS, default screen_flipstats on PATH), vbl_log (VBL_LOG), the
# card and connector of X screen 0's output (CARD, CONN). Run nothing else.
set -eu
out=$1
rev=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
FLIPSTATS=${FLIPSTATS:-screen_flipstats}
VBL_LOG=${VBL_LOG:-vbl_log}
CARD=${CARD:-/dev/dri/card1}
CONN=${CONN:-DP-5}
N=${N:-600}
mkdir -p "$out"
{ date; uname -a; uptime; ulimit -r; } > "$out/conditions.txt"

load_on() {
    pids=""
    i=0
    while [ $i -lt "$(nproc)" ]; do
        ( while :; do :; done ) &
        pids="$pids $!"
        i=$((i + 1))
    done
}
load_off() { kill $pids 2> /dev/null || true; wait 2> /dev/null || true; pids=""; }

cell() {   # name tool hold load
    name=$1; tool=$2; hold=$3; load=$4
    [ "$load" = 1 ] && load_on
    "$VBL_LOG" "$CARD" "$CONN" 300 "$out/${name}_vbl.csv" 2>> "$out/vbl_log.txt" &
    vpid=$!
    sleep 1
    if [ "$tool" = ptb ]; then
        octave-cli --eval "addpath('$here'); ptb_flips('$out/${name}.csv', $N, $hold, 0)" > "$out/${name}.log" 2>&1
    else
        h=""
        [ "$hold" = 1 ] && h=--hold
        "$FLIPSTATS" --backend glx --frames "$N" $h --csv "$out/${name}.csv" > "$out/${name}.log" 2>&1
    fi
    kill $vpid 2> /dev/null || true
    wait $vpid 2> /dev/null || true
    [ "$load" = 1 ] && load_off
    echo "$name done"
}

cells="ptb_next:ptb:0:0 ysp_next:ysp:0:0 ptb_hold:ptb:1:0 ysp_hold:ysp:1:0 ptb_next_load:ptb:0:1 ysp_next_load:ysp:0:1 ptb_hold_load:ptb:1:1 ysp_hold_load:ysp:1:1"
if [ "$rev" = rev ]; then
    cells=$(echo "$cells" | tr ' ' '\n' | awk '{a[NR]=$0} END{for(i=NR;i>0;i--) print a[i]}' | tr '\n' ' ')
fi
for c in $cells; do
    IFS=: read -r name tool hold load << EOF
$c
EOF
    cell "$name" "$tool" "$hold" "$load"
done
{ date; uptime; } >> "$out/conditions.txt"
