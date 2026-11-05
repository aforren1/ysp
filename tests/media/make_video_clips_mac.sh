#!/bin/sh
# make_video_clips_mac.sh - the clips of make_video_clips.sh on macOS without
# ffmpeg: builds make_video_clips_mac.m (VideoToolbox and AVAssetWriter,
# Command Line Tools are enough) and runs it. Nothing is downloaded.
#
# Usage: tests/media/make_video_clips_mac.sh OUTDIR [--long | --av]
#   OUTDIR   where the clips go (made if missing); point YVID_TEST_MEDIA at it
#   --long   also the 60 s measurement clips (about 1 GB, 45 s on an M2)
#   --av     also the 10 min A/V clip with its WAV
set -eu
out=${1:?usage: make_video_clips_mac.sh OUTDIR [--long | --av]}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
tool="$out/make_video_clips_mac"
clang -fobjc-arc -O2 -Wall -Wextra -Wno-unused-parameter -o "$tool" "$here/make_video_clips_mac.m" \
    -framework Foundation -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox
"$tool" "$@"
