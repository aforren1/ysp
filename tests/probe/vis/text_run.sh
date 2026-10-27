#!/usr/bin/env bash
# One measurement run of the text loop (text_loop.py, text_loop_mex.m) or the
# layout cost (text_cost.py, text_cost_mex.m; README.md). Run it through the
# timing guard on a quiet machine:
#   bash /c/tmp/psy-guard.sh time NAME bash tests/probe/vis/text_run.sh HOST MODE N OUT [SECONDS] [window]
# HOST: py (PY = the python of a venv with ysp-vis), matlab, octave (OCTAVE =
# octave-cli.exe); MODE: words, page, label, or cost (the layout cost; N is
# then the repetitions). Fullscreen unless the sixth argument is "window".
# Writes OUT.csv (the loop) or OUT.txt (the cost).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
MEX=${MEXDIR:-$(cd "$HERE/../../../bindings/mex" && pwd)}   # MEXDIR: a frozen copy of the build
HOST=$1; MODE=$2; N=$3; OUT=$4; SEC=${5:-60}; WIN=0
[ "${6:-}" = window ] && WIN=1
CSV=$(cygpath -m "$OUT.csv")
M="addpath('$(cygpath -m "$MEX")'); addpath('$(cygpath -m "$HERE")');"
if [ "$MODE" = cost ]; then
  case $HOST in
    py) "${PY:-python}" "$HERE/text_cost.py" --reps "$N" > "$OUT.txt" ;;
    matlab) matlab -batch "$M text_cost_mex($N)" > "$OUT.txt" ;;
    octave) "${OCTAVE:-octave-cli}" --eval "$M text_cost_mex($N)" > "$OUT.txt" ;;
    *) echo "HOST: py, matlab or octave"; exit 2 ;;
  esac
  exit
fi
case $HOST in
  py)
    ARGS=(--mode "$MODE" --n "$N" --seconds "$SEC" --csv "$CSV")
    [ "$WIN" = 1 ] && ARGS+=(--windowed)
    "${PY:-python}" "$HERE/text_loop.py" "${ARGS[@]}" ;;
  matlab)
    matlab -batch "$M text_loop_mex('mode', '$MODE', 'n', $N, 'seconds', $SEC, 'windowed', $WIN, 'csv', '$CSV')" ;;
  octave)
    "${OCTAVE:-octave-cli}" --eval "$M text_loop_mex('mode', '$MODE', 'n', $N, 'seconds', $SEC, 'windowed', $WIN, 'csv', '$CSV')" ;;
  *) echo "HOST: py, matlab or octave"; exit 2 ;;
esac
