#!/bin/sh
# Builds the web demos into a static site (docs/web.md).
#
#   sh examples/web/build.sh OUTDIR
#
# Run from the repository root with emcc on PATH (emsdk 6.0.12, the
# version .github/workflows/pages.yml pins). Copies examples/web/site,
# fetches coi-serviceworker.js at a pinned commit and checks its hash,
# and compiles each demo to OUTDIR/<name>.js and .wasm.
set -eu
out=${1:?usage: sh examples/web/build.sh OUTDIR}
mkdir -p "$out"
cp examples/web/site/*.html examples/web/site/*.css examples/web/site/*.js "$out/"

# The service worker that makes a page cross-origin isolated where the host
# sets no headers (GitHub Pages sets none). MIT; its license goes beside it.
coi=https://raw.githubusercontent.com/gzuidhof/coi-serviceworker/7b1d2a092d0d2dd2b7270b6f12f13605de26f214
curl -fsSL -o "$out/coi-serviceworker.js" "$coi/coi-serviceworker.js"
curl -fsSL -o "$out/coi-serviceworker.LICENSE.txt" "$coi/LICENSE"
check() {
    if command -v sha256sum > /dev/null; then got=$(sha256sum "$1" | cut -d' ' -f1)
    else got=$(shasum -a 256 "$1" | cut -d' ' -f1); fi
    if [ "$got" != "$2" ]; then echo "build.sh: $1: sha256 $got, expected $2" >&2; exit 1; fi
}
check "$out/coi-serviceworker.js" e97bbac6017322d48aa54a5bc7ce473c725a4b7376ff245002e0ba71c3dcdd7e
check "$out/coi-serviceworker.LICENSE.txt" 3ee2c88cec9f3036fd4b0cba94e94485bb8b4673be7b8e0f3b1496c5baff75cb

# One build per demo without sound, no threads: those demos use no
# worker, so a threaded build buys them nothing, and the same build runs
# isolated (finer clock) and not. ASYNCIFY, not JSPI: it runs in every
# WebGL2 browser, and measured on Chrome it cost 15 to 20 us more per
# frame (docs/web.md).
common="-O2 -Wall -Wextra -Wpedantic -Wshadow -Wfloat-conversion -Werror -Iinclude
 -sUSE_SDL=3 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sASYNCIFY -sASYNCIFY_STACK_SIZE=65536
 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1MB -sENVIRONMENT=web
 -sEXPORTED_FUNCTIONS=_main,_yscr_request_abort -sEXPORTED_RUNTIME_METHODS=callMain"
flags="$common -std=c11 -DYRT_NO_THREADS"
size() { echo "$1: $(wc -c < "$out/$1.wasm") bytes wasm, $(wc -c < "$out/$1.js") bytes js"; }
for demo in tutorial:examples/web/tutorial.c rdk_staircase:examples/web/rdk_staircase.c \
            gallery:examples/gfx/gallery.c selftest:examples/web/selftest.c; do
    name=${demo%%:*}
    src=${demo#*:}
    # shellcheck disable=SC2086
    emcc $flags -o "$out/$name.js" "$src"
    size "$name"
done

# The sound demo, twice (docs/web.md, "Audio"): av_sync_mt with threads and
# miniaudio's AudioWorklet path, for a cross-origin isolated page (it needs
# SharedArrayBuffer), and av_sync without threads on miniaudio's
# ScriptProcessorNode path for every other page; the page picks one by
# crossOriginIsolated. miniaudio 0.11.25, the version ysp/audio.h pins,
# fetched and checked as CMakeLists.txt does; its EM_ASM needs -std=gnu11.
ma=$(mktemp -d)
curl -fsSL -o "$ma/miniaudio.h" https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h
check "$ma/miniaudio.h" ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89
# shellcheck disable=SC2086
emcc $common -std=gnu11 -DYRT_NO_THREADS -I"$ma" -o "$out/av_sync.js" examples/web/av_sync.c
size av_sync
# shellcheck disable=SC2086
emcc $common -std=gnu11 -I"$ma" -pthread -Wno-pthreads-mem-growth -DMA_ENABLE_AUDIO_WORKLETS \
     -sAUDIO_WORKLET=1 -sWASM_WORKERS=1 -o "$out/av_sync_mt.js" examples/web/av_sync.c
size av_sync_mt
rm -rf "$ma"
