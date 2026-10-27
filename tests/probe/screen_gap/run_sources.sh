# DWM's vblank time and the idle-wake waiter's wakes across a 30 s gap, beside the
# flip statistics (docs/screen.md, "What the system knows during a gap").
#   run_sources.sh OUTDIR GAPDIAG_EXE TAG
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: OUTDIR GAPDIAG_EXE TAG}; EXE=${2:?usage: OUTDIR GAPDIAG_EXE TAG}; V=${3:?usage: OUTDIR GAPDIAG_EXE TAG}
G=${GUARD:-"bash /c/tmp/psy-guard.sh time"}   # one timing session per run
OM=$(cygpath -m "$OUT")
mkdir -p "$OUT"
run() { name=$1; shift; echo "== $name $(date +%T)"; $G gap-$name "$@" > $OUT/$name.log 2>&1; echo "exit=$?" >> $OUT/$name.log; }
run comp_fs_gap30_v$V "$EXE" --wt on --pre 2 --gap 30 --wake --dwm --csv $OM/comp_fs_gap30_v$V.csv
run dxgi_fs_gap30_v$V "$EXE" --dxgi --wt on --pre 2 --gap 30 --dwm --csv $OM/dxgi_fs_gap30_v$V.csv
run comp_win_gap30_v$V "$EXE" --windowed --wt on --pre 3 --gap 30 --wake --dwm --csv $OM/comp_win_gap30_v$V.csv
run dxgi_win_gap30_v$V "$EXE" --dxgi --windowed --wt on --pre 3 --gap 30 --dwm --csv $OM/dxgi_win_gap30_v$V.csv
echo done $(date +%T)
