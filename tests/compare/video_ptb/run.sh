#!/bin/bash
# Psychtoolbox's movie playback against ysp/video.h's, the same H.264
# streams on the same display, each flip scored against the kernel's
# vblanks. README.md in this folder explains it.
#   bash run.sh OUT_DIR [rev]
# Environment (defaults in brackets):
#   B        ysp build with YVID_FFMPEG          [build/linux-ffmpeg]
#   C        folder with bbb_<RES>.mp4, its .yspvi and bbb_38074.wav [build/clips]
#   RES      resolutions                          [1080p60 2160p60]
#   CELLS    cells, in order                      [all, interleaved]
#   MOVIE_SECONDS  movie seconds per cell, 0 = to the end [0]
#   CARD, CONN  DRM card and connector of X screen 0's output [/dev/dri/card1, DP-5]
# Run from the repository root, in the X session, with nothing else running.
# touch OUT_DIR/STOP skips the cells not yet started.
set -u
out=$1
rev=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
B=${B:-build/linux-ffmpeg}
C=${C:-build/clips}
RES=${RES:-"1080p60 2160p60"}
SECONDS_=${MOVIE_SECONDS:-0}
CARD=${CARD:-/dev/dri/card1}
CONN=${CONN:-DP-5}
# ysp at -30 dB; playbin's volume is linear amplitude, so the same gain.
DB=-30
VOL=$(python3 -c "print(10 ** ($DB / 20))")
# Interleaved so a slow drift in the machine's state is not one tool's.
CELLS=${CELLS:-"ptb_default ysp_auto ptb_fast ysp_sw ptb_va_intel ysp_auto_pulse ptb_va_amd ysp_va_amd ysp_sw8"}
if [ "$rev" = rev ]; then CELLS=$(echo $CELLS | tr ' ' '\n' | tac | tr '\n' ' '); fi
mkdir -p "$out"
# PTB opens a movie by absolute path only.
out=$(cd "$out" && pwd)
C=$(cd "$C" && pwd)

# Both tools resume PulseAudio when they close; this covers a run that dies.
trap 'pactl suspend-sink 0 0 2> /dev/null' EXIT

die() { echo "run.sh: $*" >&2; exit 1; }
[ -x "$B/video_soundtrack" ] || die "no $B/video_soundtrack (build the linux-ffmpeg preset)"
command -v octave-cli > /dev/null || die "no octave-cli"
if [ -z "${VBL_LOG:-}" ]; then
    VBL_LOG=$out/vbl_log
    cc -O2 -Wall -I/usr/include/libdrm -o "$VBL_LOG" "$here/../ptb_linux/vbl_log.c" -ldrm || die "vbl_log did not build (libdrm-dev)"
fi

# The same video stream for both: PTB needs the sound in the file, so the
# canonical MP4's stream is copied, not re-encoded, into a MOV beside PCM
# (MP4 cannot hold PCM; Matroska would round the times to 1 ms).
# ysp/audio.h does not resample, so ysp's PulseAudio cell takes the WAV at
# the sink's rate: 44.1 kHz or 48 kHz here, each to the sample.
frames=38074
[ -e "$C/bbb_38074.wav" ] || die "no $C/bbb_38074.wav"
if [ ! -e "$C/bbb_38074_44k.wav" ]; then
    ffmpeg -hide_banner -loglevel error -y -i "$C/bbb_38074.wav" \
        -af "aresample=44100,apad,atrim=end_sample=$((frames * 735))" -c:a pcm_s16le "$C/bbb_38074_44k.wav" || die "44.1 kHz WAV"
fi
for r in $RES; do
    [ -e "$C/bbb_$r.mp4.yspvi" ] || die "no index for $C/bbb_$r.mp4 (README.md, Prepare)"
    if [ ! -e "$C/bbb_${r}_pcm.mov" ]; then
        ffmpeg -hide_banner -loglevel error -y -i "$C/bbb_$r.mp4" -i "$C/bbb_38074.wav" \
            -map 0:v -map 1:a -c:v copy -c:a pcm_s16le "$C/bbb_${r}_pcm.mov" || die "MOV for $r"
    fi
done

{
    date; uname -a; uptime; echo "rtprio $(ulimit -r)"
    echo "ysp $(git -C "$here" rev-parse --short HEAD) $(git -C "$here" status --short | wc -l) files changed"
    octave-cli --version | head -1
    gst-inspect-1.0 --version | sed -n 2p
    ffmpeg -version | head -1
    echo "cells: $CELLS; res: $RES; seconds: $SECONDS_"
} > "$out/conditions.txt"

quiet() {
    for _ in $(seq 1 120); do
        python3 -c "import sys; sys.exit(0 if float(open('/proc/loadavg').read().split()[0]) < 2.0 else 1)" && break
        sleep 10
    done
    echo "$(date +%T) load $(cut -d' ' -f1-3 /proc/loadavg) | top: $(ps -eo pcpu,comm --sort=-pcpu | sed -n 2,6p | tr -s ' ' | tr '\n' ';')" >> "$out/conditions.txt"
}

cell() {   # name res
    name=$1; r=$2; tag=${name}_$r
    quiet
    secs=$SECONDS_
    [ "$secs" = 0 ] && span=760 || span=$((secs + 90))
    "$VBL_LOG" "$CARD" "$CONN" "$span" "$out/${tag}_vbl.csv" 2>> "$out/vbl_log.txt" &
    vpid=$!
    sleep 1
    # A hung player ends at the vblank log's span instead of stalling the run.
    run=(timeout -k 10 "$span" python3 -I "$here/rusage.py" "$out/$tag.time")
    case $name in
    ptb_*)
        mov=$C/bbb_${r}_pcm.mov
        case $name in
        ptb_default)  pf=4; thr=-1; fl=0; rank= ;;
        # max_threads -1 everywhere: any value from 0 up hung OpenMovie on
        # this machine (README.md, "Findings while writing it").
        ptb_fast)     pf=6; thr=-1; fl=4; rank= ;;   # 4: no hardware decode
        ptb_va_intel) pf=6; thr=-1; fl=0; rank=varenderD128h264dec:MAX ;;
        ptb_va_amd)   pf=6; thr=-1; fl=0; rank=vah264dec:MAX ;;
        *) die "unknown cell $name" ;;
        esac
        echo "== $tag: PTB pixel_format $pf max_threads $thr special_flags1 $fl rank '${rank}'" >> "$out/conditions.txt"
        PTB_SKIPSPLASH=1 GST_PLUGIN_FEATURE_RANK=$rank GST_DEBUG=GST_ELEMENT_FACTORY:4 GST_DEBUG_NO_COLOR=1 \
        GST_DEBUG_FILE=$out/${tag}_gst.txt "${run[@]}" octave-cli --no-gui --eval \
            "addpath('$here'); ptb_movie('$mov', '$out/$tag.csv', $pf, $thr, $fl, $VOL, $secs)" > "$out/$tag.log" 2>&1
        ;;
    ysp_*)
        wav=$C/bbb_38074.wav
        case $name in
        ysp_auto)       a=() ;;
        ysp_auto_pulse)
            # The sink's rate follows the last stream it played (PTB's
            # 48 kHz movie moves it off 44.1 kHz), and ysp/audio.h refuses
            # a project rate that is not the device's, so read it now.
            sr=$(pactl list short sinks | awk 'NR == 1 { for (i = 1; i <= NF; i++) if ($i ~ /Hz$/) print $i + 0 }')
            if [ "$sr" = 44100 ]; then wav=$C/bbb_38074_44k.wav; else sr=48000; fi
            a=(--audio pulse --rate "$sr") ;;
        ysp_sw)         a=(--hw sw) ;;
        ysp_sw8)        a=(--hw sw --threads 8) ;;
        ysp_va_amd)     a=(--hw vaapi --device /dev/dri/renderD129) ;;
        *) die "unknown cell $name" ;;
        esac
        [ "$secs" != 0 ] && a+=(--seconds "$secs")
        echo "== $tag: ysp ${a[*]}" >> "$out/conditions.txt"
        "${run[@]}" "$B/video_soundtrack" --file "$C/bbb_$r.mp4" --wav "$wav" --fullscreen --elevate --db $DB \
            --csv "$out/$tag.csv" "${a[@]}" > "$out/$tag.log" 2>&1
        ;;
    esac
    rc=$?
    kill $vpid 2> /dev/null; wait $vpid 2> /dev/null
    pactl suspend-sink 0 0 2> /dev/null
    echo "$tag exit $rc" | tee -a "$out/conditions.txt"
}

for r in $RES; do
    for c in $CELLS; do
        [ -e "$out/STOP" ] && { echo "stopped before $c $r"; exit 0; }
        cell "$c" "$r"
    done
done
{ date; uptime; } >> "$out/conditions.txt"
echo done
