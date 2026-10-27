# The grid after a gap (docs/screen.md, "The grid after a gap"): one gapdiag build,
# gaps of 5, 10, 30 and 60 s, both backends, window thread ON and OFF, files tagged _vTAG.
#   run_gaps.sh OUTDIR GAPDIAG_EXE TAG   (once per header: before and after)
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: OUTDIR GAPDIAG_EXE TAG}; EXE=${2:?usage: OUTDIR GAPDIAG_EXE TAG}; V=${3:?usage: OUTDIR GAPDIAG_EXE TAG}
G=${GUARD:-"bash /c/tmp/psy-guard.sh time"}   # one timing session per run
OM=$(cygpath -m "$OUT")
mkdir -p "$OUT"
run() { name=$1; shift; echo "== $name $(date +%T)"; $G gap-$name "$@" > $OUT/$name.log 2>&1; echo "exit=$?" >> $OUT/$name.log; }
for gap in 5 10 30 60; do
  for be in comp dxgi; do
    if [ $be = dxgi ]; then BX=--dxgi; else BX=; fi
    for wt in on off; do
      for v in $V; do
        n=${be}_fs_${gap}_${wt}_v$v
        run $n "$EXE" $BX --wt $wt --pre 2 --gap $gap --csv $OM/$n.csv
      done
    done
  done
done
for be in comp dxgi; do
  if [ $be = dxgi ]; then BX=--dxgi; else BX=; fi
  for v in $V; do
    run ${be}_win_30_on_v$v "$EXE" $BX --windowed --wt on --pre 3 --gap 30 --csv $OM/${be}_win_30_on_v$v.csv
    run ${be}_fs_30_on_v${v}_rep "$EXE" $BX --wt on --pre 2 --gap 30 --csv $OM/${be}_fs_30_on_v${v}_rep.csv
  done
done
echo done $(date +%T)
