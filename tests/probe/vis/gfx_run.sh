#!/usr/bin/env bash
# One measurement run of the gfx loop (gfx_loop.py, gfx_loop_mex.m; README.md).
# Run it through the timing guard on a quiet machine:
#   bash /c/tmp/psy-guard.sh time NAME bash tests/probe/vis/gfx_run.sh HOST MODE N OUT [SECONDS] [window]
# HOST: py (PY = the python of a venv with ysp-vis), matlab, octave (OCTAVE =
# octave-cli.exe); MODE: list or instances; N: the gabors. Fullscreen unless
# the sixth argument is "window". Writes OUT.csv.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
MEX=${MEXDIR:-$(cd "$HERE/../../../bindings/mex" && pwd)}   # MEXDIR: a frozen copy of the build
HOST=$1; MODE=$2; N=$3; OUT=$4; SEC=${5:-600}; WIN=0
[ "${6:-}" = window ] && WIN=1
CSV=$(cygpath -m "$OUT.csv")
case $HOST in
  py)
    ARGS=(--n "$N" --mode "$MODE" --seconds "$SEC" --csv "$CSV")
    [ "$WIN" = 1 ] && ARGS+=(--windowed)
    "${PY:-python}" "$HERE/gfx_loop.py" "${ARGS[@]}" ;;
  matlab)
    matlab -batch "addpath('$(cygpath -m "$MEX")'); addpath('$(cygpath -m "$HERE")'); gfx_loop_mex('n', $N, 'mode', '$MODE', 'seconds', $SEC, 'windowed', $WIN, 'csv', '$CSV')" ;;
  octave)
    "${OCTAVE:-octave-cli}" --eval "addpath('$(cygpath -m "$MEX")'); addpath('$(cygpath -m "$HERE")'); gfx_loop_mex('n', $N, 'mode', '$MODE', 'seconds', $SEC, 'windowed', $WIN, 'csv', '$CSV')" ;;
  *) echo "HOST: py, matlab or octave"; exit 2 ;;
esac
