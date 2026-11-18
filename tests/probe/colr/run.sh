#!/bin/sh
# Builds the COLR survey against the pinned HarfBuzz and runs it on the
# fonts given. From the repo root, after tools/vendor_layout.py --write:
#   sh tests/probe/colr/run.sh font.ttf [font.ttf ...]
# BUILD_DIR overrides the build folder (default build/colr_survey).
# docs/emoji_colr_survey.md explains the output.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
hb="$root/third_party/harfbuzz/src"
[ -f "$hb/harfbuzz.cc" ] || {
    echo "no HarfBuzz: uv run --no-project python tools/vendor_layout.py --write"; exit 1; }
bin=${BUILD_DIR:-"$root/build/colr_survey"}
mkdir -p "$bin"
CXX=${CXX:-g++}
CC=${CC:-gcc}
[ -f "$bin/harfbuzz.o" ] || "$CXX" -std=c++17 -O2 -fno-exceptions -fno-rtti -w \
    -c "$hb/harfbuzz.cc" -o "$bin/harfbuzz.o"
"$CC" -std=c99 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -I"$hb" -c "$here/colr_survey.c" \
    -o "$bin/colr_survey.o"
"$CXX" "$bin/colr_survey.o" "$bin/harfbuzz.o" -lm -o "$bin/colr_survey"
"$bin/colr_survey" "$@"
