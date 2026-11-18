#!/bin/sh
# Builds the filly web probe into SITE: ysp's module (filly_web.js and .wasm),
# the page, filly's web release files and its sample horse.
#
#   FILLY_WEB=<unzipped filly-web-0.5.0.zip> FILLY=<filly checkout> \
#   sh tests/probe/filly/web/build.sh SITE
#
# Run from the repository root with emcc on PATH. The flags are those of
# examples/web/build.sh, plus the GL library for the hooks.
set -eu
site=${1:?usage: build.sh SITE}
: "${FILLY_WEB:?}" "${FILLY:?}"
mkdir -p "$site/filly"
cp tests/probe/filly/web/index.html "$site/"
cp -r "$FILLY_WEB"/. "$site/filly/"
cp "$FILLY/samples/horse.glb" "$site/"
emcc -O2 -Wall -std=gnu11 -Iinclude -DYRT_NO_THREADS \
    -sUSE_SDL=3 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sASYNCIFY -sASYNCIFY_STACK_SIZE=65536 \
    -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1MB -sENVIRONMENT=web \
    -sEXPORTED_FUNCTIONS=_main,_yscr_request_abort -sEXPORTED_RUNTIME_METHODS=callMain \
    -o "$site/filly_web.js" tests/probe/filly/web/filly_web.c
ls -l "$site"/filly_web.* "$site"/filly/filly-core.*
