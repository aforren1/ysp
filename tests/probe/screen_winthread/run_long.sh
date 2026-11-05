# Part 1 measurements on ysp/screen.h 0.10.0 (the real header), window thread
# OFF (0) and ON (1) from one binary per host, back to back per condition.
# One guard session per run (docs/screen.md, "Measured on v0.10.0").
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
power() { powershell.exe -NoProfile -Command "(Get-CimInstance -Namespace root/cimv2 -ClassName Win32_Battery | Select-Object -ExpandProperty BatteryStatus)" 2>/dev/null | tr -d '\r'; }
echo "start $(date) battery_status=$(power) (2 = on AC)"
# 1. the frame loop, 10 minutes per run
for mode in fs win; do
  if [ $mode = win ]; then CW=--windowed; MW=1; else CW=; MW=0; fi
  for n in 10 100; do
    for wt in 0 1; do
      t=${mode}_${n}_wt$wt
      run c_$t $wt $BIN/probe_c.exe $CW --n $n --seconds $SEC --csv $OM/c_$t.csv
      run py_$t $wt uv run --no-project --no-cache --python 3.12 python $PYR --bundle $BM $CW --n $n --seconds $SEC --gc on --csv $OM/py_$t.csv
    done
  done
done
# 2. MATLAB R2023a and Octave 10.1, 10 minutes, fullscreen, N = 10
for wt in 0 1; do
  [ -n "$MBIN" ] || break
  run m_fs_10_wt$wt $wt matlab -batch "addpath('$HM/mex'); run_mex('$(cygpath -m "$MBIN")','n',10,'seconds',$SEC,'csv','$OM/m_fs_10_wt$wt.csv')"
  run o_fs_10_wt$wt $wt "$OCT" --no-gui --eval "addpath('$HM/mex'); run_mex('$(cygpath -m "$OBIN")','n',10,'seconds',$SEC,'csv','$OM/o_fs_10_wt$wt.csv')"
done
echo "done $(date) battery_status=$(power)"
