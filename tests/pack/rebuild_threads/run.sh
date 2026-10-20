#!/usr/bin/env sh
# Hand test (docs/pack.md 14): a pack whose ffmpeg step ran on the build
# machine's thread count rebuilds byte for byte on another machine that has
# the same ffmpeg version, with the recorded count.
#
#   run.sh build   YPAK FFMPEG MOVIE DIR   on the first machine: three packs
#                                          (mpeg1video, libx264, libx265) of
#                                          MOVIE into DIR, with the sources
#   run.sh rebuild YPAK FFMPEG DIR         on the second machine, after DIR
#                                          is copied there: `ypak rebuild`
#                                          of each pack; exits 0 only when
#                                          all three print `identical`
#
# MOVIE is PsychoPy's psychopy/demos/coder/stimuli/jwpIntro.mp4 (the clip
# of docs/pack.md 9.1). The x265 description sets info=0: x265's info
# message holds the build's OS, compiler and CPU flags (9.1, 2026-10-20).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
case ${1:-} in
build)
    [ $# -eq 5 ] || { echo "usage: run.sh build YPAK FFMPEG MOVIE DIR" >&2; exit 2; }
    ypak=$2; ff=$3; dir=$5
    mkdir -p "$dir"
    cp "$4" "$dir/jwpIntro.mp4"
    "$ff" -version | head -n 1 > "$dir/ffmpeg_version.txt"
    for e in mpeg1 x264 x265; do
        cp "$here/$e.json" "$dir/$e.json"
        "$ypak" build "$dir/$e.json" -o "$dir/$e.ysppak" --ffmpeg "$ff"
        echo "$e: built, $(grep -a -o '"threads": [0-9]*' "$dir/$e.ysppak" | head -n 1)"
    done
    ;;
rebuild)
    [ $# -eq 4 ] || { echo "usage: run.sh rebuild YPAK FFMPEG DIR" >&2; exit 2; }
    ypak=$2; ff=$3; dir=$4
    echo "this ffmpeg:  $("$ff" -version | head -n 1)"
    echo "the packs':   $(cat "$dir/ffmpeg_version.txt")"
    echo "this machine: $(nproc 2>/dev/null || echo '?') logical processors; $(grep -m 1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2)"
    echo "AVX-512:      $(grep -q -m 1 avx512f /proc/cpuinfo 2>/dev/null && echo yes || echo no)"
    fail=0
    for e in mpeg1 x264 x265; do
        echo "== $e"
        "$ypak" rebuild "$dir/$e.ysppak" --sources "$dir" --ffmpeg "$ff" -o "$dir/$e.rebuilt.ysppak" || fail=1
    done
    exit $fail
    ;;
*)
    echo "usage: run.sh build YPAK FFMPEG MOVIE DIR | run.sh rebuild YPAK FFMPEG DIR" >&2
    exit 2
    ;;
esac
