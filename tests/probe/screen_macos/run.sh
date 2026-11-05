#!/bin/sh
# One command per condition of the macOS display-timing probe:
#   sh tests/probe/screen_macos/run.sh CONDITION [results folder]
# The results folder defaults to ./ysp_macos_probe_<date>; give the same
# folder to every condition of one session so they share summary.txt.
# ANGLE_DIR (default $HOME/ysp-deps/angle-electron-43.8.3) holds libEGL.dylib
# and libGLESv2.dylib; SDL3_PREFIX (default $HOME/sdl3) holds SDL3.
# HUD=1 turns on the Metal performance HUD (it shows "Direct" or
# "Composited") and records the probe's unified log while it runs.
# docs/screen_macos_probe.md lists the conditions, their durations and what
# each table needs. Run nothing else while a timing condition runs, keep the
# machine on AC with the screen unlocked, and do not touch the mouse or keys.
set -u
here=$(cd "$(dirname "$0")" && pwd)
cond=${1:-}
if [ -z "$cond" ]; then
    sed -n '2,12p' "$0"
    echo "conditions: env egl share links-window links-space present-window present-borderless"
    echo "            present-space present-fullscreen present-exclusive-120 present-exclusive-144"
    echo "            present-drawables2 present-syncoff bits input all"
    exit 2
fi
out=${2:-"./ysp_macos_probe_$(date +%Y%m%d)"}
mkdir -p "$out/bin" "$out/system"
out=$(cd "$out" && pwd)
angle=${ANGLE_DIR:-"$HOME/ysp-deps/angle-electron-43.8.3"}
bin="$out/bin/screen_macos_probe"

if [ ! -x "$bin" ] || [ -n "$(find "$here" -name '*.[mh]' -newer "$bin")" ]; then
    if ! sh "$here/build.sh" "$out/bin" > "$out/build.log" 2>&1; then
        echo "build failed; the end of $out/build.log:"
        tail -20 "$out/build.log"
        exit 1
    fi
    cp "$here"/*.m "$here"/*.h "$out/bin/"
fi
if [ ! -f "$out/system/sw_vers.txt" ]; then
    sw_vers > "$out/system/sw_vers.txt" 2>&1
    uname -a > "$out/system/uname.txt" 2>&1
    system_profiler SPDisplaysDataType SPHardwareDataType > "$out/system/system_profiler.txt" 2>&1
    pmset -g > "$out/system/pmset.txt" 2>&1
    pmset -g assertions > "$out/system/pmset_assertions.txt" 2>&1
    ps -Ao pid,pcpu,comm > "$out/system/processes.txt" 2>&1
    shasum -a 256 "$angle/libEGL.dylib" "$angle/libGLESv2.dylib" > "$out/system/angle_sha256.txt" 2>&1
fi

run() {
    tag=$1
    shift
    echo "== $tag: $*"
    if [ "${HUD:-0}" = 1 ]; then
        log stream --style compact --predicate 'process == "screen_macos_probe"' > "$out/log_$tag.txt" 2>&1 &
        logpid=$!
        MTL_HUD_ENABLED=1 MTL_HUD_LOG_ENABLED=1 "$bin" "$@" --angle "$angle" --out "$out" --tag "$tag"
        rc=$?
        kill "$logpid" 2> /dev/null
    else
        "$bin" "$@" --angle "$angle" --out "$out" --tag "$tag"
        rc=$?
    fi
    [ "$rc" = 0 ] || echo "   exit $rc"
}

case "$cond" in
env)                run env env ;;
egl)                run egl_window_i1 egl --kind window --interval 1 --frames 600
                    run egl_window_i0 egl --kind window --interval 0 --frames 600
                    run egl_space_i1 egl --kind space --interval 1 --frames 600
                    run egl_window_10bit egl --kind window --bits 10 --frames 60 ;;
share)              run share share --frames 300
                    run share_heavy share --frames 300 --heavy 400 ;;
links-window)       run links_window links --kind window --seconds 60 ;;
links-space)        run links_space links --kind space --seconds 60 ;;
present-window)     run present_window present --kind window --runs asap,pipe,quant,ahead,late,sleep,queue,mindur ;;
present-borderless) run present_borderless present --kind borderless ;;
present-space)      run present_space present --kind space ;;
present-fullscreen) run present_fullscreen present --kind fullscreen ;;
present-exclusive-120) run present_exclusive_120 present --kind exclusive --hz 120 ;;
present-exclusive-144) run present_exclusive_144 present --kind exclusive --hz 144 ;;
present-drawables2) run present_window_d2 present --kind window --drawables 2 --runs asap,pipe,queue
                    run present_space_d2 present --kind space --drawables 2 --runs asap,pipe,queue ;;
present-syncoff)    run present_window_syncoff present --kind window --sync-off --runs asap,pipe
                    run present_space_syncoff present --kind space --sync-off --runs asap,pipe ;;
bits)               run bits bits ;;
input)              run input input --kind window --keys 200 ;;
all)                for c in env egl share links-window links-space present-window present-borderless present-space \
                             present-fullscreen present-exclusive-120 present-exclusive-144 present-drawables2 \
                             present-syncoff bits input; do
                        sh "$0" "$c" "$out" || exit $?
                    done ;;
*)                  echo "unknown condition $cond"; exit 2 ;;
esac
echo "summary: $out/summary.txt"
