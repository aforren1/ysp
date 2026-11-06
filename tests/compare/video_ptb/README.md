# Psychtoolbox and ysp/video.h on Linux: movie playback

This folder compares movie playback in Psychtoolbox-3 (PTB) with
`ysp/video.h` on one Linux machine. PTB plays through GStreamer under GNU
Octave. ysp plays through FFmpeg in `examples/video/soundtrack.c`
(`video_soundtrack`). Both tools play the same H.264 stream on the same
display. Both are scored against the same reference: the kernel's vblank
times, from `../ptb_linux/vbl_log.c`.

This test does not use a photodiode or a line-in. It measures which movie
frame each flip showed and on which vblank the flip occurred. It does not
measure light, sound, or the delay between them.

| File | What it does |
|---|---|
| `ptb_movie.m` | PTB's playback loop (as `PlayMoviesDemo.m`): `GetMovieImage`, `DrawTexture` at 1:1, `Flip`. Writes one line per flip |
| `run.sh` | Prepares the media, then runs the cells in turn, each with `vbl_log` beside it |
| `analyze.py` | Maps each flip to a kernel vblank and prints one table row per cell |
| `rusage.py` | Runs one command and writes its wall time, CPU time and peak RSS |

## Cells

| Cell | Tool | Decoder | Frame upload | Sound output |
|---|---|---|---|---|
| `ptb_default` | PTB | GStreamer's choice: `avdec_h264` (software; `vah264dec` has rank 0 here) | RGBA8 (`pixel_format` 4, PTB's default) | `autoaudiosink`, which picks PulseAudio |
| `ptb_fast` | PTB | `avdec_h264`, hardware decode off (`special_flags1` 4) | I420 planes, converted by a shader (`pixel_format` 6) | PulseAudio |
| `ptb_va_intel` | PTB | `varenderD128h264dec`: VA-API on the Intel UHD | I420 (6) | PulseAudio |
| `ptb_va_amd` | PTB | `vah264dec`: VA-API on the RX 6600 | I420 (6) | PulseAudio |
| `ysp_auto` | ysp | AUTO: VAAPI on FFmpeg's default device (the Intel UHD), read back | NV12 planes, shader | ALSA exclusive (AUTO) |
| `ysp_auto_pulse` | ysp | As `ysp_auto` | NV12 | PulseAudio |
| `ysp_sw` | ysp | FFmpeg software decode, 4 threads (the default) | NV12 | ALSA exclusive |
| `ysp_sw8` | ysp | FFmpeg software decode, 8 threads | NV12 | ALSA exclusive |
| `ysp_va_amd` | ysp | VAAPI on the RX 6600, read back | NV12 | ALSA exclusive |

Each cell runs at 1080p60 and at 2160p60. `run.sh` forces a GStreamer
decoder with `GST_PLUGIN_FEATURE_RANK` and records each element that
GStreamer made in `<cell>_gst.txt`. GStreamer's VA debug log confirmed
the device of each VA decoder on this machine: `vah264dec` opened the
RX 6600 (renderD129) and `varenderD128h264dec` opened the Intel UHD.

Pairs to compare:

- **Defaults:** `ptb_default` and `ysp_auto`.
- **Software decode:** `ptb_fast` and `ysp_sw`, `ysp_sw8`.
- **Same decoder hardware and the same sound path:** `ptb_va_intel` and `ysp_auto_pulse`.
- **VA-API on the display GPU:** `ptb_va_amd` and `ysp_va_amd`.

### The same for all cells

- **Display:** fullscreen, 1920 x 1080 at 60 Hz.
- **Frame thread:** SCHED_FIFO for both. PTB uses `Priority(MaxPriority(w))`. ysp uses `--elevate`.
- **Drawing:** the frame is drawn 1:1 at the center with nearest filtering. Both tools crop a 4K frame to the center 1920 x 1080.
- **Gain:** -30 dB. PTB's `soundvolume` is a linear amplitude, so the value is 10^(-30/20).
- **Video stream:** PTB reads a MOV. The MOV holds the MP4's video stream, copied bit for bit, and the WAV as PCM. ysp reads the MP4, its `.yspvi` index and the WAV.

### Different by design

Each tool selects frames in its own way. In PTB, GStreamer releases each
frame on the pipeline's clock (the audio sink's clock). The loop then
flips that frame at the next vblank. In ysp, `yvid_update()` selects the
frame that is due at the next vblank on the audio device's clock
(`yvid_follow_audio()`).

## Run it

Run this in the X session on a quiet machine. The full set takes about
3.5 hours: 18 cells of 10.6 minutes each. During the run, each cell takes
the display, and the sound plays at -30 dB.

1. Prepare the media once (see [Prepare](#prepare)).
2. From the repository root, start the run:

   ```sh
   bash tests/compare/video_ptb/run.sh build/ptb_video_run1
   bash tests/compare/video_ptb/run.sh build/ptb_video_run2 rev     # optional: opposite order
   ```

3. Print the results:

   ```sh
   uv run --no-project python tests/compare/video_ptb/analyze.py build/ptb_video_run1 build/ptb_video_run2
   ```

To run a short version, set `MOVIE_SECONDS=180`. This takes about
1 hour and includes the hard part of the movie at 151 to 153 s. Use
`RES` and `CELLS` to select a subset:

```sh
RES=2160p60 CELLS="ptb_fast ysp_sw ysp_sw8" bash tests/compare/video_ptb/run.sh build/ptb_sw4k
```

To skip the cells that have not started, run `touch OUT_DIR/STOP`. To end
the current cell, press Esc for PTB or Shift+Esc for ysp. Each cell ends
at the vblank log's span (the movie plus 90 to 126 s), so a player that
hangs cannot stop the run. If a run dies, `pactl suspend-sink 0 0` brings
back the desktop's sound.

## Prepare

You need these:

- **PTB:** `octave-cli` with PTB-3 on its path, and the `psychtoolbox` group's `rtprio` grant.
- **GStreamer:** GStreamer 1.x with gst-libav and gst-plugins-bad (the `va` plugin).
- **Tools:** `libdrm-dev` (for `vbl_log`) and `ffmpeg`.
- **ysp:** a `linux-ffmpeg` build of `video_soundtrack`.
- **DRM output:** the card and connector of X screen 0's output. The defaults are `/dev/dri/card1` and `DP-5`. Find yours in `/sys/class/drm/card*-*/status`.

The movie is Big Buck Bunny at 60 fps
(`bbb_sunflower_2160p_60fps_normal.mp4`). The source has B-frames, so
`ysp/video.h` refuses it. Re-encode it in the canonical form of
`tests/media/make_video_clips.sh`. The source does not state its color, so
these commands state BT.709. That is an assumption.

```sh
S=bbb_sunflower_2160p_60fps_normal.mp4
V=colorprim=bt709:transfer=bt709:colormatrix=bt709:range=tv:chromaloc=0
K="-color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv -chroma_sample_location left"
X="-c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -profile:v high -bf 0 -g 60 -keyint_min 60 \
   -sc_threshold 0 -x264-params bframes=0:scenecut=0:open-gop=0:$V $K -video_track_timescale 60000 \
   -movflags +faststart -an"
ffmpeg -i $S -map 0:v:0 $X build/clips/bbb_2160p60.mp4
ffmpeg -i $S -map 0:v:0 -vf scale=1920:1080:flags=lanczos $X build/clips/bbb_1080p60.mp4
# The soundtrack: 48 kHz, 16-bit stereo, as long as the 38074 frames to the sample.
ffmpeg -i $S -map 0:a:0 -ac 2 -ar 48000 -af "apad,atrim=end_sample=30459200" -c:a pcm_s16le build/clips/bbb_38074.wav
# The index beside each MP4 (each command plays 1 s in a window).
for r in 1080p60 2160p60; do
    build/linux-ffmpeg/video_play --file build/clips/bbb_$r.mp4 --index --seconds 1 --window 640x360
done
```

On the i5-14500, the 2160p60 encode took 817 s and its index took 253 s.
`run.sh` makes the MOVs and a 44.1 kHz WAV the first time it runs.

## Read the table

`analyze.py` uses only the flips that showed a new movie frame. It maps
each flip to the nearest kernel vblank by the onset that the tool
reported. Then it counts from the kernel's vblank numbers:

| Column | Meaning |
|---|---|
| Shown | New frames that were flipped, with the first and last frame numbers |
| Unshown | Frames between the first and the last shown frame that were never shown |
| Repeats | Vblanks on which the previous frame stayed on the screen |
| Slips | Changes of (vblank number minus frame number). Each repeat or unshown frame changes it. 0 means that each frame stayed for exactly one refresh |
| Stamp | Reported onset minus the kernel's vblank time, in us (median, and the maximum absolute value) |
| Tool's own count | PTB: flips with `missed` > 0, and the dropped frames that `PlayMovie` returns. ysp: late flips, and the repeated and dropped counts in its log |
| CPU per frame | The whole process's user and system time, divided by the frames shown. It includes startup and Octave |
| Peak RSS | The whole process. PTB's value includes Octave |
| Notes | PTB: the time from the frame's release to its flip (p1, p50). ysp: the lowest number of decoded frames ready in the queue (full is 7), without the first 10 and last 8 records |

PTB's times are `GetSecs` values, which are CLOCK_REALTIME on Linux.
`vbl_log`'s third column maps them to CLOCK_MONOTONIC. ysp's times are
CLOCK_MONOTONIC.

## Findings while writing it

These come from PTB 3.0.19.14, Octave 7.3.0, GStreamer 1.22.0, Debian 12,
an i5-14500 and an RX 6600, on 2026-11-05.

- **PTB's thread setting:** `Screen('OpenMovie')` hung with `max_threads`
  set to 0, which the PTB manual recommends for high-resolution movies. It
  also hung with 1, 4 and 8, with pixel format 4 and 6, and with hardware
  decode on and off. Only -1, the codec's own default, opened the movie.
  For `avdec_h264` that default is its automatic thread count. So all PTB
  cells use -1.
- **PulseAudio's rate:** the sink's rate follows the last stream that
  played. PTB's 48 kHz movie moved it from 44.1 kHz to 48 kHz.
  `ysp/audio.h` refuses a project rate that differs from the device's
  rate, so `run.sh` reads the sink's rate just before `ysp_auto_pulse`.
- **A 4 s check at 2160p60, one run:** `ptb_default` did not show 3
  frames, and PTB reported 3 dropped frames and 1 missed flip.
  `ptb_va_amd` showed all 241 frames. All 1080p60 cells showed every
  frame in 4 s.
