#!/bin/sh
# make_video_clips.sh - the MP4 clips ysp/video.h's Media Foundation and FFmpeg
# tests and benchmarks use, made with ffmpeg (libx264, libx265), the encoder the pack
# tool uses. Nothing is downloaded.
#
# Usage: tests/media/make_video_clips.sh OUTDIR [--long | --av]
#   OUTDIR   where the clips go (made if missing); point YVID_TEST_MEDIA at it
#   --long   also the 60 s measurement clips
#   --av     also the 10 min A/V clip with its WAV
#
# Canonical clips ("c_*") pass ysp/video.h's checks because of these flags:
#   -bf 0, x264 bframes=0           no B-frames: decode order is display order
#   -g G -keyint_min G              one GOP length
#   -sc_threshold 0, scenecut=0     no extra keyframe at a scene cut
#   open-gop=0                      every keyframe an IDR: a seek decodes exactly
#   testsrc2 rate=NUM/DEN           a constant rate; every frame on the grid
#   -video_track_timescale T        T a multiple of the rate, so each time is
#                                   an exact tick count (no 1 ms rounding)
#   -pix_fmt yuv420p, -profile high 8-bit 4:2:0
#   -color_* bt709/tv, -chroma_sample_location left, and the same in
#   x264's colorprim/transfer/colormatrix/range/chromaloc
#                                   a stated color in the stream (VUI)
#   -an                             no audio track (c_audio keeps one on purpose)
# Each frame carries its own index in 16 bars across the top 64 rows (bit k
# white when bit k of the frame number is set), so a test can read which
# frame it got. The "r_*" clips each break one rule and must be refused.
set -eu
out=${1:?usage: make_video_clips.sh OUTDIR [--long]}
long=${2:-}
mkdir -p "$out"
ff="ffmpeg -hide_banner -loglevel error -y"
bars="geq=lum='if(lt(Y,64),if(mod(floor(N/pow(2,floor(X*16/W))),2),235,16),p(X,Y))':cb='if(lt(Y,32),128,p(X,Y))':cr='if(lt(Y,32),128,p(X,Y))'"
color="-color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv -chroma_sample_location left"
# the same color in the encoder's own parameters: after a filter, ffmpeg can
# drop -color_trc and -color_primaries (seen with geq), so x264 is told too
vui="colorprim=bt709:transfer=bt709:colormatrix=bt709:range=tv:chromaloc=0"

# x264 canonical: name size rate frames gop timescale [extra input filter]
x264() {
    name=$1 size=$2 rate=$3 n=$4 g=$5 ts=$6
    $ff -f lavfi -i "testsrc2=size=$size:rate=$rate" -frames:v "$n" -vf "$bars" \
        -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -profile:v high \
        -bf 0 -g "$g" -keyint_min "$g" -sc_threshold 0 \
        -x264-params "bframes=0:scenecut=0:open-gop=0:$vui" $color \
        -video_track_timescale "$ts" -movflags +faststart -an "$out/$name.mp4"
}

x264 c_720p30    1280x720  30          300 30 30000
x264 c_1080p30   1920x1080 30          300 30 30000
x264 c_1080p60   1920x1080 60          600 60 60000
x264 c_2997      1280x720  30000/1001  150 15 30000
x264 c_23976     1280x720  24000/1001   96 24 24000
x264 c_crop      1280x718  30           60 30 30000

# A stream that crops 16 columns from the left, so a decoder's left crop is
# tested: the bars sit at the visible frame's left edge. x264 stores the crop in
# the SPS, and the encoder's input is 1280 wide.
$ff -f lavfi -i "testsrc2=size=1280x720:rate=30" -frames:v 60 \
    -vf "pad=1280:720,geq=lum='if(lt(Y,64),if(lt(X,16),16,if(mod(floor(N/pow(2,floor((X-16)*16/1264))),2),235,16)),p(X,Y))':cb='if(lt(Y,32),128,p(X,Y))':cr='if(lt(Y,32),128,p(X,Y))'" \
    -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -profile:v high -bf 0 -g 30 -keyint_min 30 -sc_threshold 0 \
    -x264-params "bframes=0:scenecut=0:open-gop=0:crop-rect=16,0,0,0:$vui" $color \
    -video_track_timescale 30000 -movflags +faststart -an "$out/c_cropleft.mp4"

# No VUI at all: nothing states the color, so an index needs it in its desc.
$ff -f lavfi -i "testsrc2=size=320x240:rate=30" -frames:v 30 -c:v libx264 -crf 18 -pix_fmt yuv420p \
    -bf 0 -g 30 -keyint_min 30 -sc_threshold 0 -x264-params "bframes=0:scenecut=0:open-gop=0" \
    -video_track_timescale 30000 -an "$out/c_novui.mp4"

# The first 3 frames as ffmpeg's own decoder gives them, raw NV12 (visible
# rows only): an independent reference for the bytes Media Foundation gives
# (H.264 and HEVC decoding are exact by the standards).
for c in c_720p30 c_1080p30 c_crop; do
    $ff -i "$out/$c.mp4" -frames:v 3 -f rawvideo -pix_fmt nv12 "$out/$c.nv12"
done

# Three different colour fields in the SPS (BT.601 matrix, sRGB transfer,
# BT.709 primaries), so a parser that mixes them up is caught.
$ff -f lavfi -i "testsrc2=size=320x240:rate=30" -frames:v 30 -c:v libx264 -crf 18 -pix_fmt yuv420p     -bf 0 -g 30 -keyint_min 30 -sc_threshold 0     -x264-params "bframes=0:scenecut=0:open-gop=0:colorprim=bt709:transfer=iec61966-2-1:colormatrix=smpte170m:range=tv"     -color_primaries bt709 -color_trc iec61966-2-1 -colorspace smpte170m -color_range tv     -video_track_timescale 30000 -an "$out/c_mixcolor.mp4"

# HEVC Main, 8-bit: refused on a machine without an HEVC decoder.
$ff -f lavfi -i "testsrc2=size=1280x720:rate=30" -frames:v 120 -vf "$bars" \
    -c:v libx265 -preset fast -crf 20 -pix_fmt yuv420p -tag:v hvc1 \
    -x265-params "bframes=0:keyint=30:min-keyint=30:scenecut=0:open-gop=0:colorprim=bt709:transfer=bt709:colormatrix=bt709:range=limited:chromaloc=0:log-level=error" \
    $color -video_track_timescale 30000 -movflags +faststart -an "$out/c_hevc.mp4"
$ff -i "$out/c_hevc.mp4" -frames:v 3 -f rawvideo -pix_fmt nv12 "$out/c_hevc.nv12"

# An AAC track beside the video: the video plays, the audio track is ignored.
$ff -f lavfi -i "testsrc2=size=1280x720:rate=30" -f lavfi -i "sine=frequency=440:sample_rate=48000" \
    -frames:v 90 -t 3 -vf "$bars" -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -profile:v high \
    -bf 0 -g 30 -keyint_min 30 -sc_threshold 0 -x264-params "bframes=0:scenecut=0:open-gop=0:$vui" $color \
    -video_track_timescale 30000 -c:a aac -movflags +faststart "$out/c_audio.mp4"

# A soundtrack for c_720p30: 10 s at 48 kHz, stereo, 16-bit, exactly 480000 frames.
$ff -f lavfi -i "sine=frequency=440:sample_rate=48000" -af "pan=stereo|c0=c0|c1=c0" -t 10 \
    -c:a pcm_s16le "$out/c_720p30.wav"

# --- refused: each breaks one rule -------------------------------------------
# B-frames
$ff -f lavfi -i "testsrc2=size=1280x720:rate=30" -frames:v 90 -vf "$bars" -c:v libx264 -crf 18 \
    -pix_fmt yuv420p -bf 2 -g 30 -keyint_min 30 -sc_threshold 0 -x264-params "bframes=2:b-adapt=0:scenecut=0" \
    $color -video_track_timescale 30000 -an "$out/r_bframes.mp4"
# Frames missing from the grid (every 7th dropped, times kept): variable rate
$ff -f lavfi -i "testsrc2=size=1280x720:rate=30" -frames:v 90 -vf "select='not(eq(mod(n\,7)\,3))',$bars" \
    -fps_mode passthrough -c:v libx264 -crf 18 -pix_fmt yuv420p -bf 0 -g 30 -keyint_min 30 -sc_threshold 0 \
    -x264-params "bframes=0:scenecut=0" $color -video_track_timescale 30000 -an "$out/r_vfr.mp4"
# A keyframe forced off the GOP
$ff -f lavfi -i "testsrc2=size=1280x720:rate=30" -frames:v 90 -vf "$bars" -c:v libx264 -crf 18 \
    -pix_fmt yuv420p -bf 0 -g 30 -keyint_min 30 -sc_threshold 0 -force_key_frames "expr:eq(n,17)" \
    -x264-params "bframes=0:scenecut=0:open-gop=0:$vui" $color -video_track_timescale 30000 -an "$out/r_gop.mp4"
# 10-bit
$ff -f lavfi -i "testsrc2=size=1280x720:rate=30" -frames:v 60 -c:v libx264 -crf 18 -pix_fmt yuv420p10le \
    -profile:v high10 -bf 0 -g 30 -keyint_min 30 -sc_threshold 0 -x264-params "bframes=0:scenecut=0" \
    $color -video_track_timescale 30000 -an "$out/r_10bit.mp4"
# 4:4:4
$ff -f lavfi -i "testsrc2=size=1280x720:rate=30" -frames:v 60 -c:v libx264 -crf 18 -pix_fmt yuv444p \
    -profile:v high444 -bf 0 -g 30 -keyint_min 30 -sc_threshold 0 -x264-params "bframes=0:scenecut=0" \
    $color -video_track_timescale 30000 -an "$out/r_444.mp4"
# Pixels not square
$ff -f lavfi -i "testsrc2=size=1280x720:rate=30" -frames:v 60 -vf "setsar=2" -c:v libx264 -crf 18 \
    -pix_fmt yuv420p -bf 0 -g 30 -keyint_min 30 -sc_threshold 0 -x264-params "bframes=0:scenecut=0" \
    $color -video_track_timescale 30000 -an "$out/r_sar.mp4"
# Interlaced
$ff -f lavfi -i "testsrc2=size=1280x720:rate=30" -frames:v 60 -c:v libx264 -crf 18 -pix_fmt yuv420p \
    -flags +ildct+ilme -bf 0 -g 30 -keyint_min 30 -sc_threshold 0 -x264-params "bframes=0:scenecut=0:tff=1" \
    $color -video_track_timescale 30000 -an "$out/r_interlace.mp4"
# MPEG-4 Part 2 in MP4
$ff -f lavfi -i "testsrc2=size=1280x720:rate=30" -frames:v 60 -c:v mpeg4 -q:v 3 -bf 0 -g 30 \
    -video_track_timescale 30000 -an "$out/r_mpeg4.mp4"
# Remuxes of a canonical clip: rotation, a 1 ms timescale at 30000/1001, a start offset
# -display_rotation is newer than ffmpeg 5.1, which refuses the option; there
# the mov muxer writes the matrix from the stream's rotate tag instead.
$ff -display_rotation 90 -i "$out/c_720p30.mp4" -c copy "$out/r_rotation.mp4" 2>/dev/null ||
    $ff -i "$out/c_720p30.mp4" -c copy -metadata:s:v:0 rotate=90 "$out/r_rotation.mp4"
$ff -i "$out/c_2997.mp4" -c copy -video_track_timescale 1000 "$out/r_ts1000.mp4"
$ff -i "$out/c_720p30.mp4" -c copy -output_ts_offset 0.1 "$out/r_offset.mp4"

if [ "$long" = "--long" ]; then
    # Measurement clips: testsrc2 with temporal noise, so the bit rate is closer
    # to a camera's than a flat pattern's (the bit rate is printed below).
    meas() {
        name=$1 size=$2 rate=$3 n=$4 g=$5 ts=$6
        $ff -f lavfi -i "testsrc2=size=$size:rate=$rate" -frames:v "$n" -vf "noise=alls=6:allf=t" \
            -c:v libx264 -preset medium -crf 20 -pix_fmt yuv420p -profile:v high \
            -bf 0 -g "$g" -keyint_min "$g" -sc_threshold 0 -x264-params "bframes=0:scenecut=0:open-gop=0:$vui" \
            $color -video_track_timescale "$ts" -movflags +faststart -an "$out/$name.mp4"
    }
    meas m_720p30   1280x720  30 1800 30 30000
    meas m_1080p30  1920x1080 30 1800 30 30000
    meas m_1080p60  1920x1080 60 3600 60 60000
    # HEVC Main at 1080p30, 10 s.
    $ff -f lavfi -i "testsrc2=size=1920x1080:rate=30" -frames:v 300 -vf "noise=alls=6:allf=t" \
        -c:v libx265 -preset fast -crf 22 -pix_fmt yuv420p -tag:v hvc1 \
        -x265-params "bframes=0:keyint=30:min-keyint=30:scenecut=0:open-gop=0:colorprim=bt709:transfer=bt709:colormatrix=bt709:range=limited:chromaloc=0:log-level=error" \
        $color -video_track_timescale 30000 -movflags +faststart -an "$out/m_hevc1080p30.mp4"
    for f in "$out"/m_*.mp4; do
        printf '%s: ' "$f"; ffprobe -v error -show_entries format=bit_rate -of default=nw=1:nk=1 "$f"
    done
fi
if [ "$long" = "--av" ]; then
    # 10 min at 30 fps and its soundtrack: 600 s at 48 kHz is exactly
    # 18000 frames x 1600 samples
    $ff -f lavfi -i "testsrc2=size=1920x1080:rate=30" -frames:v 18000 -vf "noise=alls=6:allf=t" \
        -c:v libx264 -preset medium -crf 20 -pix_fmt yuv420p -profile:v high \
        -bf 0 -g 30 -keyint_min 30 -sc_threshold 0 -x264-params "bframes=0:scenecut=0:open-gop=0:$vui" \
        $color -video_track_timescale 30000 -movflags +faststart -an "$out/m_av10min.mp4"
    $ff -f lavfi -i "sine=frequency=440:sample_rate=48000" -af "volume=0.03,pan=stereo|c0=c0|c1=c0" -t 600 \
        -c:a pcm_s16le "$out/m_av10min.wav"
fi
echo "clips in $out"
