# The cause (docs/screen.md, "The cause"): the grid's period after 2 s and after
# 60 s of frames, and where the OS's times fall after a gap.
#   run_cause.sh OUTDIR GAPDIAG_EXE TAG   (a build of the header before v0.10.1)
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: OUTDIR GAPDIAG_EXE TAG}; EXE=${2:?usage: OUTDIR GAPDIAG_EXE TAG}; V=${3:?usage: OUTDIR GAPDIAG_EXE TAG}
G=${GUARD:-"bash /c/tmp/psy-guard.sh time"}   # one timing session per run
OM=$(cygpath -m "$OUT")
mkdir -p "$OUT"
run() { name=$1; shift; echo "== $name $(date +%T)"; $G gap-$name "$@" > $OUT/$name.log 2>&1; echo "exit=$?" >> $OUT/$name.log; }
run comp_fs_pre2_gap30_v$V "$EXE" --wt on --pre 2 --gap 30 --wake --csv $OM/comp_fs_pre2_gap30_v$V.csv
run comp_fs_pre60_gap30_v$V "$EXE" --wt on --pre 60 --gap 30 --wake --csv $OM/comp_fs_pre60_gap30_v$V.csv
run comp_fs_pre2_gap10_v$V "$EXE" --wt on --pre 2 --gap 10 --wake --csv $OM/comp_fs_pre2_gap10_v$V.csv
run dxgi_fs_pre2_gap30_v$V "$EXE" --dxgi --wt on --pre 2 --gap 30 --csv $OM/dxgi_fs_pre2_gap30_v$V.csv
run dxgi_fs_pre60_gap30_v$V "$EXE" --dxgi --wt on --pre 60 --gap 30 --csv $OM/dxgi_fs_pre60_gap30_v$V.csv
echo done $(date +%T)
