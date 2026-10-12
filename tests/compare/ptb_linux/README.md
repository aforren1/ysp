# Psychtoolbox and ysp/screen.h on Linux X11

This folder compares Psychtoolbox-3 (PTB) with `ysp/screen.h`'s GLX_OML
backend on one Linux machine. Each side's flip stamps are checked against
the kernel's own vblank times, and PTB's KbQueue stamps against the
kernel's evdev time of a uinput keyboard. No photodiode: the reference is
the kernel, not the light.

| File | What it does |
|---|---|
| `vbl_log.c` | Logs every vblank of one CRTC from the kernel (`drmWaitVBlank`), with CLOCK_REALTIME minus CLOCK_MONOTONIC beside each |
| `ptb_flips.m` | PTB's Flip, back to back or held 1 to 5 frames ahead (`when = vbl + (k - 0.5) * ifi`), under `Priority(MaxPriority(w))` |
| `run.sh` | Eight cells (PTB and ysp; next vblank and held; idle and under one spinning process per hardware thread), each with `vbl_log` beside it |
| `compare.py` | Maps each reported onset to the nearest kernel vblank: stamp error, on target, late, early, and whether the tool flagged its own late flips |
| `uinput_keys.c` | A uinput keyboard that taps F14 and logs the `write()` in both clocks and the kernel's evdev time |
| `ptb_keys.m` | A KbQueue on that keyboard alone |
| `keys_stats.py` | KbQueue's stamp minus the kernel's time |

## Run it

In the X session, on a quiet machine. Find the card and connector of X
screen 0's output in `/sys/class/drm/card*-*/status` (`xrandr`'s
DisplayPort-4 was the kernel's `card1-DP-5` here).

```sh
cc -O2 -Wall -I/usr/include/libdrm -o vbl_log vbl_log.c -ldrm
cc -O2 -Wall -o uinput_keys uinput_keys.c
FLIPSTATS=<build>/screen_flipstats VBL_LOG=./vbl_log CARD=/dev/dri/card1 CONN=DP-5 sh run.sh out1
FLIPSTATS=<build>/screen_flipstats VBL_LOG=./vbl_log CARD=/dev/dri/card1 CONN=DP-5 sh run.sh out2 rev
uv run --no-project python compare.py out1 out2
octave-cli --eval "addpath(pwd); ptb_keys('./uinput_keys', 200, 'keys.csv', 'keys_log.csv')"
uv run --no-project python keys_stats.py keys.csv keys_log.csv idle
```

The key test needs write access to `/dev/uinput` and read access to the
new keyboard's node (`tools/linux/70-ysp-input.rules`, and `sudo modprobe
uinput`).

## Results, 2026-10-12

Conditions: Debian 12, kernel 6.11.5-liquorix (PDS, HZ=1000), Intel Core
i5-14500, AMD RX 6600, Mesa 22.3.6, Xorg with Xfwm4 (compositor off), one
1920 x 1080 output at 60 Hz (1125 lines a frame). PTB 3.0.19.14 under GNU
Octave 7.3.0; ysp/screen.h v0.8.0 (`examples/screen/flipstats.c`, SDL
3.4.18). Both frame threads on SCHED_FIFO 1 under the `psychtoolbox`
group's `rtprio` 50. Load: one `while :` shell loop per hardware thread
(20). Files in `results/2026-10-12/`.

Flips, 600 per cell, two runs in opposite orders (9600 flips):

| Cell | Stamp minus kernel vblank, us: median / p99 of abs / max abs | On target | Late or early |
|---|---|---|---|
| PTB, next vblank, idle | -0.04, -0.01 / 0.39, 0.43 / 0.54, 0.52 | 599, 599 of 599 | 0 |
| ysp, next vblank, idle | 0 / 0 / 0 | 600, 600 | 0 |
| PTB, held, idle | -0.03, +0.02 / 0.42, 0.41 / 0.54, 0.52 | 599, 599 of 599 | 0 |
| ysp, held, idle | 0 / 0 / 0 | 600, 600 | 0 |
| PTB, next vblank, load | -0.02, +0.02 / 0.35, 0.37 / 0.38, 2.03 | 599, 599 of 599 | 0 |
| ysp, next vblank, load | 0 / 0 / 0 | 600, 600 | 0 |
| PTB, held, load | -0.02, +0.00 / 0.35, 0.36 / 0.37, 0.42 | 599, 599 of 599 | 0 |
| ysp, held, load | 0 / 0 / 0 | 600, 600 | 0 |

PTB's first flip has no vblank before it to count from, so it is not
scored. PTB's errors come from its seconds as doubles (about 0.24 us near
1.8e9 s) and its mapping of the kernel's clock to GetSecs (one flip of
4800 off by 2.0 us); ysp's stamps are the kernel's own numbers.

- Both put every flip on the vblank asked for, idle and under load, and
  both report the kernel's vblank time. On this machine they tie on flip
  timing.
- The kernel's vblank time is in the future when the flip completes:
  idle, PTB's Flip returned 0.15 to 0.41 ms (median 0.36) before the
  vblank time it reported. DRM's high-precision vblank timestamps mark the end of the
  vertical blank, the start of the new frame's scanout; the blank is 45
  lines, 0.67 ms, here.
- PTB's GetSecs is CLOCK_REALTIME on Linux (it read epoch seconds), so its
  stamps move if the system clock is stepped. ysp's are CLOCK_MONOTONIC.
- Opening, 3 runs each, with `PTB_SKIPSPLASH=1`: PTB's
  `Screen('OpenWindow')` took 1.50 to 1.54 s with its default sync tests
  (`SkipSyncTests` 0), 1.52 to 1.55 s at level 1 and 0.65 to 0.69 s with
  the tests skipped (level 2), so the tests cost about 0.85 s here: 50
  valid refresh samples, all valid at the first try. A whole one-frame
  `flipstats` run (process, SDL, GL, the settle check of 22 flips in
  0.48 s, close) took 0.55 s.
- Without `PTB_SKIPSPLASH` every open took 10 s more (11.5 to 11.7 s), at
  every level: PTB holds its welcome splash for 10 s on the first open
  after a Screen build is installed (`PsychWindowSupport.c`, marker file
  `screen_buildnr_<N>` in `PsychtoolboxConfigDir`), and `sca` deletes the
  marker again (`PsychJavaSwingCleanup.m`) while
  `PsychtoolboxConfigDir/welcomemsgdone` does not exist, which it did not
  for this user. The cell runs above include those 10 s; they do not
  touch the flips.
- The refresh interval: the kernel's vblanks averaged 16.66683 ms; PTB
  measured 16.66682 to 16.66684 ms at open; ysp's describe line at close
  said 59.9994 Hz (+10 ppm) and at open the nominal 60.0000 Hz.

Keys, 200 taps per run through a uinput keyboard, KbQueue on that
keyboard alone:

| Run | KbQueue stamp minus the kernel's evdev time, ms: min / mean / p50 / p99 / max |
|---|---|
| Idle | 0.034 / 0.110 / 0.109 / 0.179 / 0.404 |
| 20 spinning processes | 0.038 / 0.066 / 0.045 / 0.298 / 2.007 |

KbQueue stamps a key when its thread receives the XInput 2 event: 0.03 to
0.4 ms after the kernel, idle, with a 2 ms tail under load. ysp/screen.h
gives a key the kernel's time when the user can read the keyboard's node
(0 error, `test_screen_glx` part 11), and the X server's time otherwise
(1 to 2 ms early; docs/screen.md, "Kernel key times"). The key test did not
use PTB's KbCheck, its default `KbQueueCreate` on all keyboards, or a key
pressed by hand.
