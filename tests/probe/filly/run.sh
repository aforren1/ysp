#!/bin/sh
# The timed runs of docs/filly_probe.md question 2, one at a time. Run in the
# X session on a quiet machine; each run opens a 1024 x 768 window for about
# a minute.
#
#   sh tests/probe/filly/run.sh BINDIR FILLY OUTDIR [FRAMES]
#
# BINDIR holds filly_ysp; FILLY is the filly checkout (for its sample models).
set -eu
bin=${1:?usage: run.sh BINDIR FILLY OUTDIR [FRAMES]}
filly=${2:?}
out=${3:?}
frames=${4:-3600}
mkdir -p "$out"
horse="$filly/samples/horse.glb"
wood="$filly/examples/assets/wooden_playground_horse.glb"
run() {
    name=$1; shift
    echo "== $name: $(date +%T), load $(cut -d' ' -f1-3 /proc/loadavg)"
    "$bin/filly_ysp" --frames "$frames" --csv "$out/$name.csv" "$@" > "$out/$name.log" 2>&1
    grep -v FrameInfo "$out/$name.log" | sed -n '/^mode/,/^threads/p'
    sleep 3
}
run none --mode none
for s in 512x512 1024x1024 1920x1080; do
    run "shared-$s" --mode shared --size "$s" --model "$horse"
    run "readback-$s" --mode readback --size "$s" --model "$horse"
    run "async-$s" --mode readback-async --size "$s" --model "$horse"
done
run wood-shared-1920x1080 --mode shared --size 1920x1080 --model "$wood"
run wood-async-1920x1080 --mode readback-async --size 1920x1080 --model "$wood"
run none-again --mode none
