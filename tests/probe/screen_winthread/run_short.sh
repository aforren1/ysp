# Part 1: idle gaps, abort, panic, MATLAB and Octave cases on ysp/screen.h
# 0.10.0, window thread OFF (0) and ON (1). One guard session per run.
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/out}            # logs and CSVs (git ignores out/)
BIN=${BIN:?BIN: the C and Python bundle (probe_c.exe, ysp_av_probe.pyd, ANGLE)}
MBIN=${MBIN:-}; OBIN=${OBIN:-}    # the MATLAB and Octave bundles; empty skips them
OCT=${OCT:-"/c/Program Files/GNU Octave/Octave-10.1.0/mingw64/bin/octave-cli.exe"}
G=${GUARD:-"bash /c/tmp/psy-guard.sh time"}   # one timing session per run
HM=$(cygpath -m "$HERE"); BM=$(cygpath -m "$BIN"); OM=$(cygpath -m "$OUT")
PYR=$HM/py/run_py.py
mkdir -p "$OUT"
run() { name=$1; wt=$2; shift 2; echo "== $name $(date +%T)"; YSCR_WINDOW_THREAD=$wt $G winthread-$name "$@" > $OUT/$name.log 2>&1; echo "exit=$?" >> $OUT/$name.log; tail -1 $OUT/$name.log; }
echo "start $(date)"
# idle gaps with no pump: C Sleep, Python time.sleep
for gap in 5 10 30; do
  for mode in fs win; do
    if [ $mode = win ]; then CW=--windowed; else CW=; fi
    for wt in 0 1; do
      t=${gap}_${mode}_wt$wt
      run idle_c_$t $wt $BIN/probe_c.exe $CW --n 10 --idle $gap --csv $OM/idle_c_$t.csv
      run idle_py_$t $wt uv run --no-project --no-cache --python 3.12 python $PYR --bundle $BM $CW --n 10 --idle $gap --heap 10 --csv $OM/idle_py_$t.csv
    done
  done
done
# abort (Shift+Esc in frames) and the panic (3 presses while the host waits 10 s)
for wt in 0 1; do
  run abort_c_wt$wt $wt $BIN/probe_c.exe --n 10 --abort
  run abort_py_wt$wt $wt uv run --no-project --no-cache --python 3.12 python $PYR --bundle $BM --n 10 --abort --heap 10
done
run panic_c_wt1 1 $BIN/probe_c.exe --n 10 --panic
run panic_py_wt1 1 uv run --no-project --no-cache --python 3.12 python $PYR --bundle $BM --n 10 --panic --heap 10
# MATLAB and Octave: pause(30) fullscreen and in a window, abort, panic, three opens
if [ -n "$MBIN" ] && [ -n "$OBIN" ]; then
for mode in fs win; do
  if [ $mode = win ]; then MW=1; else MW=0; fi
  for wt in 0 1; do
    run m_pause_${mode}_30_wt$wt $wt matlab -batch "addpath('$HM/mex'); run_mex('$(cygpath -m "$MBIN")','windowed',$MW,'n',10,'idle',30,'csv','$OM/m_pause_${mode}_30_wt$wt.csv')"
    run o_pause_${mode}_30_wt$wt $wt "$OCT" --no-gui --eval "addpath('$HM/mex'); run_mex('$(cygpath -m "$OBIN")','windowed',$MW,'n',10,'idle',30,'csv','$OM/o_pause_${mode}_30_wt$wt.csv')"
  done
done
run m_abort_wt1 1 matlab -batch "addpath('$HM/mex'); run_mex('$(cygpath -m "$MBIN")','n',10,'abort',1)"
run o_abort_wt1 1 "$OCT" --no-gui --eval "addpath('$HM/mex'); run_mex('$(cygpath -m "$OBIN")','n',10,'abort',1)"
run m_panic_wt1 1 matlab -batch "addpath('$HM/mex'); run_mex('$(cygpath -m "$MBIN")','n',10,'panic',1); disp('MATLAB still running after the panic')"
run o_panic_wt1 1 "$OCT" --no-gui --eval "addpath('$HM/mex'); run_mex('$(cygpath -m "$OBIN")','n',10,'panic',1); disp('Octave still running after the panic')"
run m_reopen_wt1 1 matlab -batch "addpath('$HM/mex'); for k=1:3, run_mex('$(cygpath -m "$MBIN")','windowed',1,'n',10,'seconds',1,'heap',10); end; disp('three opens done')"
run o_reopen_wt1 1 "$OCT" --no-gui --eval "addpath('$HM/mex'); for k=1:3, run_mex('$(cygpath -m "$OBIN")','windowed',1,'n',10,'seconds',1,'heap',10); end; disp('three opens done')"
fi
# the window-thread command round trip and key latency (C, fullscreen)
run rt_c_wt1 1 $BIN/probe_c.exe --n 10 --latency key --lat-n 50
echo "done $(date)"
