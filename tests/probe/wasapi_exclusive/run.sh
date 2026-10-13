#!/bin/sh
# Builds the WASAPI exclusive-mode probe and runs the cave-audio setup and
# the raw loop of docs/audio.md, at the minimum and the default device
# period, 30 s each, on the default render endpoint. Git Bash or MSYS2,
# from the repo root:
#   sh tests/probe/wasapi_exclusive/run.sh [probe options...]
# Other options go to every run (for example --device NAME). BUILD_DIR
# overrides the build folder (default build/wasapi_exclusive). Every other
# program loses the device while a run is in progress. Run nothing else
# while it measures. docs/audio.md, "Exclusive mode compared with
# cave-audio", explains the output.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
bin=${BUILD_DIR:-"$root/build/wasapi_exclusive"}
mkdir -p "$bin"
if ! { cmake -S "$here" -B "$bin" && cmake --build "$bin" --config Release; } > "$bin.log" 2>&1; then
    echo "build failed; the end of $bin.log:"
    tail -20 "$bin.log"
    exit 1
fi
exe="$bin/Release/wasapi_exclusive_probe.exe"
[ -f "$exe" ] || exe="$bin/wasapi_exclusive_probe.exe"
for preset in cave raw; do
    for period in min default; do
        echo "== --preset $preset --period $period"
        "$exe" --seconds 30 --preset "$preset" --period "$period" "$@"
    done
done
