#!/bin/sh
# Builds the macOS display-timing probe with the system clang.
#   sh tests/probe/screen_macos/build.sh [output folder]   default: tests/probe/screen_macos/build
# SDL3_PREFIX (default $HOME/sdl3) holds include/SDL3 and lib/libSDL3.dylib.
# ANGLE is loaded at run time, not linked.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-"$here/build"}
mkdir -p "$out"
sdl=${SDL3_PREFIX:-"$HOME/sdl3"}
cc=${CC:-clang}
set -x
"$cc" -std=gnu11 -O2 -g -fobjc-arc -DSDL_USE_BUILTIN_OPENGL_DEFINITIONS -Wall -Wextra -Wshadow -Wno-unused-parameter \
    -I"$sdl/include" -I"$here" \
    "$here/main.m" "$here/common.m" "$here/env.m" "$here/egl.m" "$here/metal.m" "$here/share.m" \
    "$here/links.m" "$here/present.m" "$here/bits.m" "$here/input.m" \
    -L"$sdl/lib" -lSDL3 -Wl,-rpath,"$sdl/lib" \
    -framework Cocoa -framework Metal -framework QuartzCore -framework CoreVideo \
    -framework IOSurface -framework ApplicationServices \
    -o "$out/screen_macos_probe"
