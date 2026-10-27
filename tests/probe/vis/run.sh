#!/usr/bin/env bash
# One measurement run of tests/probe/vis (README.md). Run it through the
# timing guard on a quiet machine:
#   bash /c/tmp/psy-guard.sh time NAME bash tests/probe/vis/run.sh HOST KIND OUT [SECONDS]
# HOST: py (PY = the python of a venv with ysp-vis), matlab, octave (OCTAVE =
# octave-cli.exe); KIND: full (fullscreen loop), window (1280 x 720 loop),
# idle (120 frames, 30 s without a call, 120 frames; fullscreen, watched).
# Writes OUT.csv and, for idle, OUT.watch.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
MEX=$(cd "$HERE/../../../bindings/mex" && pwd)
HOST=$1; KIND=$2; OUT=$3; SEC=${4:-600}
IDLE=0; WIN=0
case $KIND in
  full) ;;
  window) WIN=1 ;;
  idle) IDLE=30; SEC=4 ;;
  *) echo "KIND: full, window or idle"; exit 2 ;;
esac
CSV=$(cygpath -m "$OUT.csv")
if [ "$IDLE" != 0 ]; then
  "${PY:-python}" "$HERE/watch_hung.py" --title "ysp-vis probe" --seconds 90 --log "$OUT.watch" &
  WATCH=$!
fi
case $HOST in
  py)
    ARGS=(--seconds "$SEC" --idle "$IDLE" --csv "$CSV")
    [ "$WIN" = 1 ] && ARGS+=(--windowed)
    "${PY:-python}" "$HERE/loop.py" "${ARGS[@]}" ;;
  matlab)
    matlab -batch "addpath('$(cygpath -m "$MEX")'); addpath('$(cygpath -m "$HERE")'); loop_mex('seconds', $SEC, 'windowed', $WIN, 'idle', $IDLE, 'csv', '$CSV')" ;;
  octave)
    "${OCTAVE:-octave-cli}" --eval "addpath('$(cygpath -m "$MEX")'); addpath('$(cygpath -m "$HERE")'); loop_mex('seconds', $SEC, 'windowed', $WIN, 'idle', $IDLE, 'csv', '$CSV')" ;;
  *) echo "HOST: py, matlab or octave"; exit 2 ;;
esac
RC=$?
[ "$IDLE" != 0 ] && wait "$WATCH"
exit $RC
