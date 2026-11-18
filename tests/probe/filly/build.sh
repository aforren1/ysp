#!/bin/sh
# Builds the native filly probe (docs/filly_probe.md, "How to rebuild").
#
#   FILLY=<filly checkout> FILLY_BUILD=<its build tree> FILAMENT=<SDK> \
#   SDL3=<SDL 3.4.18 prefix> CXX=<clang++> sh tests/probe/filly/build.sh OUTDIR
#
# Run from the repository root. FILLY_BUILD is a CMake build of filly's root
# with -DFILLY_PYTHON=OFF (it holds core/libfilly_core.a); FILAMENT is the SDK
# that filly's tools/build_filament_linux.py stages. CXX must be the Clang
# that built them.
set -eu
out=${1:?usage: build.sh OUTDIR}
: "${FILLY:?}" "${FILLY_BUILD:?}" "${FILAMENT:?}" "${SDL3:?}" "${CXX:?}"
here=tests/probe/filly
mkdir -p "$out"

cc -O2 -g -Wall "$here/share_test.c" -o "$out/share_test" -lGL -lEGL -lX11

# ysp as C with gcc; the C++ file sees declarations only.
cc -std=c11 -O2 -g -Iinclude -I"$SDL3/include" -c "$here/ysp_impl.c" -o "$out/ysp_impl.o"

fil="$FILAMENT/lib/x86_64"
libs=""
for l in gltfio_core filament-iblprefilter filament backend filabridge filaflat geometry ibl utils \
         bluegl smol-v stb dracodec meshoptimizer mikktspace uberzlib zstd basis_transcoder \
         ktxreader image abseil; do
    libs="$libs $fil/lib$l.a"
done
# shellcheck disable=SC2086
"$CXX" -std=c++20 -O2 -g -Iinclude -I"$SDL3/include" -I"$FILLY/core" \
    "$here/filly_ysp.cpp" "$out/ysp_impl.o" -o "$out/filly_ysp" \
    "$FILLY_BUILD/core/libfilly_core.a" "$FILLY_BUILD/core/libfilly_material_archive.a" \
    "$FILLY_BUILD/core/libfilly_output_materials.a" "$FILLY_BUILD/_deps/libwebp-build/libwebpdecoder.a" \
    -Wl,--start-group $libs -Wl,--end-group \
    -L"$SDL3/lib" -Wl,-rpath,"$SDL3/lib" -lSDL3 -lGL -lEGL -lX11 -lpthread -ldl -lm
ls -l "$out/filly_ysp" "$out/share_test"

# Route (b): gles_share against a second Filament build tree (FILAMENT_GLES_BUILD)
# configured with FILAMENT_SUPPORTS_EGL_ON_LINUX and FILAMENT_LINUX_IS_MOBILE
# (docs/filly_probe.md, "How to rebuild"). Skipped when it is not set.
if [ -n "${FILAMENT_GLES_BUILD:-}" ]; then
    g="$FILAMENT_GLES_BUILD"
    cc -std=c11 -O2 -g -Iinclude -DYSCR_NO_SDL -c "$here/ysp_impl.c" -o "$out/ysp_impl_nosdl.o"
    "$CXX" -std=c++20 -O2 -g -Iinclude -I"$SDL3/include" -I"$FILAMENT/include" -I"$g/libs/gltfio" \
        -DYSCR_NO_SDL "$here/gles_share.cpp" "$out/ysp_impl_nosdl.o" -o "$out/gles_share" \
        -Wl,--start-group "$g/libs/gltfio/libgltfio_core.a" "$g/libs/gltfio/libuberarchive.a" \
        "$g/filament/libfilament.a" "$g/filament/backend/libbackend.a" "$g/libs/filabridge/libfilabridge.a" \
        "$g/libs/filaflat/libfilaflat.a" "$g/libs/geometry/libgeometry_combined.a" "$g/libs/ibl/libibl.a" \
        "$g/libs/utils/libutils.a" "$g/libs/uberz/libuberzlib.a" "$g/libs/ktxreader/libktxreader.a" \
        "$g/libs/image/libimage.a" "$g/third_party/mikktspace/libmikktspace.a" \
        "$g/third_party/abseil/tnt/libabseil_combined.a" "$g/third_party/smol-v/tnt/libsmol-v.a" \
        "$g/third_party/zstd/tnt/libzstd.a" "$g/third_party/draco/tnt/libdracodec.a" \
        "$g/third_party/stb/tnt/libstb.a" "$g/third_party/meshoptimizer/tnt/libmeshoptimizer.a" \
        "$g/third_party/basisu/tnt/libbasis_transcoder.a" -Wl,--end-group \
        -lEGL -lGLESv2 -lpthread -ldl -lm
    ls -l "$out/gles_share"
fi
