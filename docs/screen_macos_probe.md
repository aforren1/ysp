# macOS display-timing probe, 2026-11-04

This note describes a probe that measures what macOS, Metal and ANGLE
give a display backend. Its results decide how the macOS backend of
`ysp/screen.h` (`YSCR_BACKEND_METAL`, built in v0.11.0) is built. The plan
that asks for the backend is [rig_spec.md](rig_spec.md) section 4.2
("macOS: whatever ANGLE's Metal backend exposes, measured before it is
claimed"), section 8 (timing model, tiers) and section 13 (SDL3's EGL
path on Cocoa; ANGLE's 10-bit configurations on Metal). The Windows
measurements that the probe mirrors are in [screen.md](screen.md). The
house pattern for a probe is [screen_x11_probe.md](screen_x11_probe.md).

State: the functional part ran on 2026-11-04 and 2026-11-05 with the
session locked (section 5). Two timing sessions ran on 2026-11-05,
unlocked: a clean one, the primary data, and an earlier one with a Screen
Sharing session open (section 6). Section 7 is the design, built in
`ysp/screen.h` v0.11.0, set from the first session in v0.12.0 and checked
on the clean one in v0.14.1.

Code: `tests/probe/screen_macos/`

| File | Contents |
|---|---|
| `main.m` | Options, the section switch |
| `common.m` | Time, output, statistics, the vblank grid fit, CVDisplayLink, EGL and GL loaders, windows |
| `env.m` | Section `env` |
| `egl.m` | Section `egl` (question 1a) |
| `metal.m`, `mtl.h` | The probe's CAMetalLayer presenter and the per-run analysis |
| `share.m` | Section `share` (question 1b) |
| `links.m` | Section `links` (question 2) |
| `present.m` | Section `present` (questions 3 and 4) |
| `bits.m` | Section `bits` (question 5) |
| `input.m` | Section `input` (question 6) |
| `link.m` | Section `link`: CAMetalDisplayLink's pacing (4.8) |
| `build.sh` | Builds with the system clang; links SDL3, loads ANGLE at run time |
| `run.sh` | One command per condition |

CMake does not build the probe. It is macOS only.

## 1. Run it

### 1.1 Before you start

- Unlock the screen and keep the display on. With the screen locked,
  every window is occluded, `presentedTime` is 0 for every frame, and the
  window sections refuse to run (exit code 3).
- Close other programs. Keep the machine on AC. Run nothing else while a
  timing condition runs (CLAUDE.md, "Timing measurements run one at a
  time").
- Do not touch the mouse or the keyboard. Escape stops a section.
- Give the terminal program the Accessibility permission (System
  Settings, Privacy and Security, Accessibility) before the `input`
  condition, or that condition measures nothing (section 5.7). The probe
  does not ask for the permission.

### 1.2 One command per condition

From the repository root:

```sh
sh tests/probe/screen_macos/run.sh CONDITION [results folder]
```

Give every condition of one session the same results folder. The script
builds the probe into `<folder>/bin` when a source file is newer than the
binary, records the system once into `<folder>/system`, and appends every
section's report to `<folder>/summary.txt`.

Durations at 60 Hz on this machine (estimates from the frame counts, not
measured):

| Condition | What it runs | Duration | Tables |
|---|---|---|---|
| `env` | Clocks, displays, modes, Metal, ANGLE configs and extensions | 15 s | 6.1 |
| `egl` | SDL3's EGL path: window at swap interval 1 and 0, a fullscreen space at interval 1, 600 frames each; a 10-bit request | 50 s | 6.2 |
| `share` | 12 runs of 300 frames (3 targets x 4 syncs), then the same with a 400-iteration shader | 2.5 min | 6.3 |
| `links-window` | CVDisplayLink, CADisplayLink and presentedTime, 800 x 600 window, 60 s | 65 s | 6.4 |
| `links-space` | The same in a fullscreen space | 70 s | 6.4 |
| `present-window` | asap, pipe, quant, ahead, late, sleep, queue, mindur in an 800 x 600 window | 2.3 min | 6.5, 6.6 |
| `present-borderless` | The same without ahead, display-sized borderless window above the menu bar | 1.8 min | 6.5, 6.6 |
| `present-space` | The same in a fullscreen space (SDL, `SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES=1`) | 1.9 min | 6.5, 6.6 |
| `present-fullscreen` | The same in SDL's fullscreen without a space | 1.9 min | 6.5, 6.6 |
| `present-exclusive-120` | The same at 1920 x 1080, 120 Hz (SDL mode change; restored at the end) | 1.1 min | 6.5, 6.6 |
| `present-exclusive-144` | The same at 144 Hz | 1 min | 6.5, 6.6 |
| `present-drawables2` | asap, pipe, queue at `maximumDrawableCount` 2, window and space | 50 s | 6.7 |
| `present-syncoff` | asap, pipe at `displaySyncEnabled = NO`, window and space | 45 s | 6.7 |
| `bits` | 10-bit and half-float targets through ANGLE, five CAMetalLayer formats | 15 s | 6.8 |
| `input` | 200 F19 presses by `CGEventPost` and 200 by `CGEventPostToPid` | 25 s | 6.9 |
| `all` | Every condition above, in that order | 18 min | all |
| `link-window` | CAMetalDisplayLink (4.8), 800 x 600 window: `free` 300 frames and `target_wait` 20 trials (`target_at` is skipped: 6.11), each at preferredFrameLatency 1, 2 and the default | 45 s | 6.11 |
| `link-space` | The same in a fullscreen space | 50 s | 6.11 |
| `link-window-heavy`, `link-space-heavy` | The same with `--heavy 400` (about 4.5 ms of GPU time a frame at 800 x 600; more fullscreen) | 50 s each | 6.11 |
| `link` | The four `link-` conditions, in that order (not in `all`) | 3.5 min | 6.11 |

With `HUD=1` in front of the command, the script turns on Metal's
performance HUD (`MTL_HUD_ENABLED=1`), which shows "Direct" or
"Composited" on the window, and records the unified log of the probe
during the run (`log_<tag>.txt`). Whether the HUD writes its present mode
to the log is not verified (section 5.4); read the HUD by eye otherwise.
Do not use `HUD=1` for the timing runs themselves: the HUD is a layer
that macOS composes.

`ANGLE_DIR` and `SDL3_PREFIX` override the default dependency folders
(`$HOME/ysp-deps/angle-electron-43.8.3`, `$HOME/sdl3`).

### 1.3 The probe without the script

```sh
sh tests/probe/screen_macos/build.sh       # makes tests/probe/screen_macos/build/screen_macos_probe
tests/probe/screen_macos/build/screen_macos_probe --help
```

| Option | Effect |
|---|---|
| `--out DIR` | Output folder |
| `--angle DIR` | Folder with ANGLE's `libEGL.dylib` and `libGLESv2.dylib` (or `YSP_ANGLE_DIR`) |
| `--tag NAME` | Suffix of this condition's CSV files |
| `--kind K` | `window` (800 x 600, floating level), `borderless` (display size, above the menu bar), `space`, `fullscreen` (SDL without a space), `exclusive` |
| `--hz HZ` | `exclusive`: the refresh rate of the mode at the desktop size |
| `--runs a,b` | `present`: the sub-runs (default all but `ahead`) |
| `--frames N` | Frames per run, default 300 |
| `--reps N` | `present quant`: presents per cell, default 20; `queue`: rounds |
| `--seconds S` | `links`: length, default 20 |
| `--drawables N` | `maximumDrawableCount`, 2 or 3 (default 3) |
| `--sync-off` | `displaySyncEnabled = NO` |
| `--interval N` | `egl`: swap interval, default 1 |
| `--bits N` | `egl`: 8, 10 or 16 bits per channel |
| `--backend B` | `egl`: ANGLE back end, `metal` (default), `gl`, `default` |
| `--heavy N` | `egl`, `share`: N dependent shader iterations per pixel |
| `--keys N` | `input`: key presses per pass, default 200 |
| `--allow-locked` | Open windows on a locked screen (functional checks only) |

## 2. Time base

Every time the probe records is `mach_absolute_time()` scaled by
`mach_timebase_info` (125/3 on the M2): "mach ns". This is the clock of
`ysp/rt.h` v0.10.0 on macOS. Apple's display and event times are on the
same base and need no correlation:

| Source | Unit | Conversion |
|---|---|---|
| `MTLDrawable.presentedTime`, `CACurrentMediaTime`, `GPUStartTime`, `CADisplayLink.timestamp`, `NSEvent.timestamp` | s (`CFTimeInterval`) | x 1e9 |
| `CVTimeStamp.hostTime` | mach ticks | x numer / denom |
| `SDL_GetTicksNS` | ns since `SDL_Init` | plus an offset, from a correlation |

The `env` section checks each of these against `mach_absolute_time()`
instead of assuming it (section 5.2).

## 3. Output

| File | Contents |
|---|---|
| `summary.txt` | Every section's report, appended |
| `env_modes.csv` | Every display mode CoreGraphics lists, with the low-resolution duplicates |
| `present_<tag>.csv`, `share_<tag>.csv`, `links_frames_<tag>.csv` | One row per frame (columns below) |
| `present_cv_<tag>.csv`, `links_cv_<tag>.csv`, `egl_cv_<tag>.csv` | One row per CVDisplayLink callback |
| `links_ca_<tag>.csv` | One row per CADisplayLink callback |
| `egl_frames_<tag>.csv` | One row per `eglSwapBuffers` |
| `input_hid_<tag>.csv`, `input_pid_<tag>.csv` | One row per key press |
| `system/` | `sw_vers`, `system_profiler`, `pmset`, the process list, ANGLE's SHA-256 |
| `bin/` | The binary and its sources |

Frame columns: `t_req`, `t_got` (around `nextDrawable`), `t_commit`,
`target` (presentAtTime), `planned` (the planned vblank's time),
`live_vblank` (the newest CVDisplayLink vblank at the commit), `sched`
and `done` (handler arrivals), `gpu_start`, `gpu_end`, `presented`
(`presentedTime`, 0 if not shown), `pres_arrive` (presented handler
arrival), `drawable_id`, `readback` and `expect` (share), `v_commit`,
`v_planned`, `v_shown` (vblank indices), `resid_us`.

### 3.1 The vblank grid and the vblank index

The probe fits a straight line of CVDisplayLink's `inNow.hostTime`
against the vblank index (each time's index from the nominal period, so a
missed callback keeps its place). `presentedTime` need not sit on that
grid, so a shown frame's index comes from the grid moved by the circular
mean of the run's `presentedTime` residuals; the summary prints that
phase. The depth of a frame presented at once is its shown index minus
the index of the last vblank before the commit returned, as in
[screen.md](screen.md#depth).

## 4. What each section measures

### 4.1 `env`

Machine, OS, SDL version; the smallest step and the offset and rate
against mach ns of `CACurrentMediaTime`, `systemUptime`,
`CLOCK_UPTIME_RAW`, `mach_continuous_time`, `CLOCK_MONOTONIC`,
`CLOCK_MONOTONIC_RAW`, `SDL_GetTicksNS` and `CGEventGetTimestamp`; the
displays (CoreGraphics modes, NSScreen refresh properties, CVDisplayLink
nominal period, EDR headroom, depth); SDL's displays and fullscreen
modes; Metal devices; for each ANGLE back end (default, Metal, OpenGL)
the EGL version, extensions, configs, GL strings, the Metal device ANGLE
uses, and whether ANGLE takes the probe's own MTLDevice.

### 4.2 `egl` (question 1a)

SDL3's own path: `SDL_HINT_EGL_LIBRARY`, `SDL_HINT_OPENGL_LIBRARY`,
`SDL_HINT_VIDEO_FORCE_EGL`, `SDL_HINT_OPENGL_ES_DRIVER`, a GL ES 3.0
profile, and `SDL_GL_EGL_PLATFORM` with a platform attribute callback that
asks for ANGLE's Metal back end. It reports the context, the config SDL
chose, the layers under the window's content view (ANGLE's CAMetalLayer
and what ANGLE set on it), and each timing extension with one call. Then
it swaps 600 frames and records the call and return of each swap.

To see when ANGLE's frames reach the panel, the probe swizzles
`-[CAMetalLayer nextDrawable]` in its own process: each drawable ANGLE
takes gets a presented handler. This is a measurement tool, not a design
option.

### 4.3 `share` (question 1b)

A CAMetalLayer on the SDL window (`SDL_Metal_CreateView`), on the
MTLDevice that ANGLE's Metal display uses (`EGL_METAL_DEVICE_ANGLE`
through `EGL_EXT_device_query`), with the probe's own command queue. Each
frame ANGLE clears to its own gray level (and runs a heavy shader with
`--heavy`), and the probe presents. Targets:

- `drawable_ios`: ANGLE draws into the drawable: an
  `EGL_ANGLE_iosurface_client_buffer` pbuffer on
  `drawable.texture.iosurface`, cached per IOSurface.
- `drawable_image`: the same through an EGLImage of the drawable's
  MTLTexture (`EGL_ANGLE_metal_texture_client_buffer`) bound to a GL
  framebuffer, cached per texture.
- `iosurface`: ANGLE draws into the probe's own IOSurface; the probe
  blits it into the drawable.

Syncs between ANGLE's queue and the probe's: none; `glFinish`; an
`MTLSharedEvent` that ANGLE signals (`EGL_ANGLE_metal_shared_event_sync`,
`eglCreateSync` with the event and a value, then `glFlush`) and the
probe's command buffer waits on (`encodeWaitForEvent`); and
`eglWaitUntilWorkScheduledANGLE`. The probe's command buffer reads the
drawable's center pixel before it presents. A pixel that differs from the
frame's gray level is a frame read before ANGLE finished.

### 4.4 `links` (question 2)

For 60 s: CVDisplayLink on its own thread, CADisplayLink from the
window's NSView on the main run loop (`-[NSView displayLinkWithTarget:]`,
macOS 14 and later), and frames presented at once, one in flight. It
reports per source the count, the fitted period, missed callbacks, the
residual about the fit, the callback's delay after its own timestamp, and
the agreement between sources (CADisplayLink timestamp minus the nearest
CVDisplayLink `inNow`, grid phase, period difference in ppm).

### 4.5 `present` (questions 3 and 4)

On the probe's CAMetalLayer, from a thread that is not the main thread,
while the main thread pumps SDL:

| Run | What it does | Question |
|---|---|---|
| `asap` | Present at once, one in flight, after a random wait of up to one period | Depth, and its dependence on the commit's phase; latency from commit to `presentedTime`; arrival of the presented handler after `presentedTime` |
| `pipe` | Present at once, no wait but `nextDrawable` | The queue the layer allows; `nextDrawable` blocking |
| `quant` | `presentDrawable:atTime:` at V + frac x period, V 1, 2, 3 or 5 vblanks ahead, frac -0.5 to +0.5 | How a target between two vblanks is quantized: nearest, next, tie |
| `ahead` | Targets 0.1 to 5 s ahead | How far ahead a target holds |
| `late` | Targets 2 vblanks ahead; every 10th frame commits 0.3 period after its target vblank | What a late present does; does `presentedTime` move with it (an observation) or stay on the plan |
| `sleep` | Sleep until 1 ms after vblank (V - depth), then present at once | The hold DXGI_FLIP uses, as a fallback if presentAtTime is not exact |
| `queue` | `maximumDrawableCount - 1` presents in flight, each for its own vblank | Whether queued presents with targets land |
| `mindur` | `presentDrawable:afterMinimumDuration:` of 2 periods | Whether a minimum duration holds frames |

The window kinds of question 4 are the conditions `present-window`,
`-borderless`, `-space`, `-fullscreen` and `-exclusive-*`. macOS has no
public API that reports whether a window is direct to display. The probe
compares the kinds by depth and latency (one vblank less is the expected
sign of a direct path) and reads the HUD by eye with `HUD=1`.

### 4.6 `bits` (question 5)

EGL configurations of 10/10/10/2 and half float on ANGLE's Metal back
end; then a 1024-step ramp drawn into a GL texture (RGBA8, RGB10_A2,
RGBA16F), an EGLImage of an MTLTexture (BGRA8Unorm, RGB10A2Unorm,
BGR10A2Unorm, RGBA16Float) and an IOSurface pbuffer ('BGRA', 'l10r',
'RGhA'), each read back; then five CAMetalLayer pixel formats with 30
frames each.

### 4.7 `input` (question 6)

F19 key presses at known mach times, 40 to 50 ms apart, by
`CGEventPost(kCGHIDEventTap)` and by `CGEventPostToPid` to the probe
itself. Each press is read as the `NSEvent.timestamp` (a local monitor
sees it before SDL), as SDL's event timestamp moved to mach ns through a
correlation of `SDL_GetTicksNS`, and as its arrival in the program. The
HID pass runs only when the probe is the active app and the screen is
unlocked: a HID key goes to the app with the keyboard focus.

### 4.8 `link`: CAMetalDisplayLink (macOS 14 and later)

The question. Apple's newer pacing model drives each frame from
`CAMetalDisplayLink`'s update callback, which hands over a drawable, a
`targetTimestamp` (the deadline to commit by) and a
`targetPresentationTimestamp` (when the frame will be shown). The
PsychMetal developer reported that it schedules about 3 refreshes ahead
(2 with Game Mode), against about 1.7 when presenting inline
(psychtoolbox.discourse.group, thread 5811). `ysp/screen.h`'s
`presentDrawable:atTime:` path measured depth 2 (6.5). The knob nobody has
measured is `preferredFrameLatency` (frames; its default read 2.0 on this
machine).

The condition. Frames are driven from the update callback on a thread of
the probe's own, whose run loop the link is added to, and each frame is
drawn the way `ysp/screen.h` draws: ANGLE into the update's drawable
through a pbuffer kept per IOSurface, ended with an
`EGL_ANGLE_metal_shared_event_sync` signal that the probe's command buffer
waits on, then the center pixel read back and the drawable presented. Each
run is made at preferredFrameLatency 1, 2 and the default:

| Run | What it does |
|---|---|
| `free` | Draw in every update and present its drawable: 300 frames |
| `target_wait` | Ask for a vblank 3 updates ahead (from the first update's targetPresentationTimestamp and the period of their steps), then draw only in the update whose targetPresentationTimestamp is that vblank: 20 trials, each after the trial before was shown. This is how the model targets a vblank: it has no call for a chosen vblank, only a choice of which update to draw in |
| `target_at` | Draw in a trial's first update and present its drawable with `presentDrawable:atTime:` the vblank 3 updates ahead; the link refuses it with an exception that aborts the process (6.11), so the probe skips this run |

Per frame (`link_<tag>.csv`): the update's arrival, `targetTimestamp`,
`targetPresentationTimestamp`, the commit, the vblank asked (target runs),
`presentedTime`, the presented handler's arrival, the read-back pixel.
Summary per run: shown and not shown, vblank steps between shown frames,
presented minus commit, presented minus targetPresentationTimestamp (does
the link's promise hold), the update to presentedTime in periods, the
targets' hits (on, early, late), read-back mismatches; `--heavy` gives the
drops under GPU load.

The decision rule. Switch `ysp/screen.h`'s METAL backend to the link only
if, at the same rates and kinds, its depth (update or commit to the shown
vblank) is at most 2, it drops no more frames than `present`'s `asap` and
two-in-flight runs, and `target_wait` (or `target_at`) hits its vblank at
least as often as `quant` (6.6). Otherwise keep `presentDrawable:atTime:`.

Run it at the Mac, unlocked, with no Screen Sharing session, nothing else
running:

```sh
sh tests/probe/screen_macos/run.sh link build/macos/screen_probe_link_<date>
```

Built and checked on 2026-11-05 with the session locked and the display
asleep, only for a clean exit: the link gave a few updates (the period
of their steps, 16680.375 us, and 3 drawables in the first run), then none
once its drawables were held unshown; every run ended at its time limit
with no frame shown, and the process exited 0. No timing run.

## 5. Results so far: functional, 2026-11-04 and 2026-11-05

Machine: Apple M2 Mac mini (Mac14,3), macOS 27.0.1 (26A434), Apple clang
21, arm64. One display: M27Q over HDMI, desktop 1920 x 1080 at "60 Hz".
SDL 3.4.0. ANGLE from Electron 43.8.3 (SHA-256 of `libEGL.dylib`
`c52abfc5...10d4`, of `libGLESv2.dylib` `1ca7d2ee...817e`); EGL_VERSION
`1.5 (ANGLE 2.1 git hash: unknown hash)`, GL_RENDERER on Metal
`ANGLE (Apple, ANGLE Metal Renderer: Apple M2, Version 27.0.1 (Build
26A434))`, GL_VERSION `OpenGL ES 3.0 (ANGLE 2.1 git hash: unknown hash)`.
Other agents compiled on the machine during every run. Costs below are
from that loaded machine and are not timing results.

### 5.1 The screen was locked

`CGSessionCopyCurrentDictionary` reported `CGSSessionScreenIsLocked = 1`
for the whole session; the display was on (`CGDisplayIsAsleep` 0). With
the screen locked:

- every window's `occlusionState` lacks `Visible`, and every frame's
  `presentedTime` is 0, in a window, a borderless window and a fullscreen
  space;
- in the first run (a window at normal level) 53 of 60 presented handlers
  did not fire within the drain and `nextDrawable` blocked up to 1.03 s
  (its timeout); with the window at floating level and joined to all
  spaces, every handler fired at once with `presentedTime` 0 and
  `nextDrawable` took 8 ms. Which of the two changes made the difference
  was not isolated;
- SDL's EGL path swapped 124 frames per second at swap interval 1: no
  vblank throttle.

A backend must therefore treat `presentedTime == 0` as "not shown"
(screen.md's ESTIMATED and OCCLUDED), not block on `nextDrawable`
without a timeout, and report a locked session. The probe now refuses to
open a window on a locked screen.

### 5.2 Clocks (`env`)

| Clock | Smallest step | Offset from mach ns | Rate against mach ns |
|---|---|---|---|
| `mach_absolute_time` | 41 ns (24 MHz, timebase 125/3) | 0 | 0 |
| `CACurrentMediaTime`, `systemUptime`, `CLOCK_UPTIME_RAW` | 41 ns | 0.000 to 0.001 us | 0.000 ppm |
| `mach_continuous_time`, `CLOCK_MONOTONIC_RAW` | 41 ns | +112 935.7 s (time asleep since boot) | 0.001 ppm |
| `CLOCK_MONOTONIC` (rt.h before v0.10.0) | 1000 ns | +112 931.5 s | +0.58 ppm over 1 s (frequency-adjusted) |
| `SDL_GetTicksNS` | 41 ns | minus the uptime at `SDL_Init` | 0.000 ppm |
| `CGEventGetTimestamp` of `CGEventCreate(NULL)` | did not change in 20 ms | about 1 s | the value did not move: not a clock |

So `CACurrentMediaTime`, `NSProcessInfo.systemUptime` and SDL's ticks are
mach time; SDL's ticks need only a constant offset. `CLOCK_MONOTONIC`
counts sleep, steps in whole microseconds and runs 0.58 ppm off mach time,
which is why rt.h v0.10.0 moves to mach time. The timestamp of a key
event from `CGEventCreateKeyboardEvent` is measured by `input`, not here.

### 5.3 Displays and modes (`env`)

- CoreGraphics lists 127 modes (69 in SDL). At 1920 x 1080 points and
  pixels: 144, 120, 100, 60, 50 and 24 Hz; also 2560 x 1440 at 144, 120
  and 60 Hz. The desktop is 1920 x 1080 at 60.000 Hz by CoreGraphics and
  SDL (60/1).
- CVDisplayLink's nominal period is 400330 / 24000000 s = 16.680417 ms
  (59.9505 Hz), and `NSScreen.minimumRefreshInterval` and
  `displayUpdateGranularity` are 16.680416 ms. So the "60 Hz" mode runs at
  about 59.95 Hz, and the CoreGraphics and SDL refresh rate (60.000) is a
  rounded label. A backend must take the period from the display link or
  the flips, not from the mode.
- NSScreen: 8 bits per sample, EDR headroom 1.0 (now and potential), no
  variable refresh (`minimumRefreshInterval` equals
  `maximumRefreshInterval`).
- Metal: one device, "Apple M2", unified memory.

### 5.4 ANGLE back ends and extensions (`env`)

| Item | Default (no type) | Metal | OpenGL |
|---|---|---|---|
| GL_RENDERER | `ANGLE (Apple, Apple M2, OpenGL 4.1 Metal - 91.7)` | `ANGLE Metal Renderer: Apple M2` | as default |
| Configs | 1 (RGBA8, D24S8) | 10, all RGBA8 (depth and stencil and 4x MSAA variants) | 1 |
| `EGL_CHROMIUM_sync_control`, `EGL_ANGLE_sync_control_rate` | no | no | no |
| `EGL_ANDROID_get_frame_timestamps`, `EGL_ANDROID_presentation_time` | no | no | no |
| `EGL_ANGLE_metal_texture_client_buffer` | no | yes (EGLImage only: a pbuffer on it gives `EGL_BAD_PARAMETER`) | no |
| `EGL_ANGLE_iosurface_client_buffer` | yes | yes (pbuffer, bind target `EGL_TEXTURE_2D`) | yes (bind target rectangle) |
| `EGL_ANGLE_metal_shared_event_sync`, `_commands_scheduled_sync` | no | yes | no |
| `EGL_ANGLE_wait_until_work_scheduled` | yes | yes | yes |
| `EGL_KHR_fence_sync`, `EGL_KHR_wait_sync` | no | yes | no |
| `EGL_KHR_gl_colorspace` and the colorspace extensions | no | no | no |
| `EGL_METAL_DEVICE_ANGLE` (device extension `EGL_ANGLE_device_metal`) | `EGL_BAD_ATTRIBUTE` | the default MTLDevice, the same object as `MTLCreateSystemDefaultDevice()` | `EGL_BAD_ATTRIBUTE` |

- **This build's default back end on macOS is OpenGL (CGL), not Metal.**
  A backend must ask for `EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE`, and SDL's
  EGL path needs `SDL_GL_EGL_PLATFORM` and a platform attribute callback
  to get it. SDL frees the array the callback returns, so it must come
  from `SDL_malloc` (a static array aborted the probe).
- `eglCreateDeviceANGLE(EGL_METAL_DEVICE_ANGLE, device)` fails with
  `EGL_BAD_ACCESS`: ANGLE does not take a caller's MTLDevice that way.
  `EGL_PLATFORM_ANGLE_DEVICE_ID_HIGH/LOW_ANGLE` with the device's
  `registryID` initializes. On a one-GPU Mac both routes end on the same
  device; the query is enough.
- ANGLE does not take a caller's command queue. Sharing means one device,
  two queues, and a sync between them (5.6).

### 5.5 SDL3's EGL path (`egl`, question 1a)

| Item | Result |
|---|---|
| Context | Works with the Metal platform callback: `RESULT sdl_egl_context OK`, GL ES 3.0 on ANGLE Metal |
| Config | ANGLE config 2: RGBA 8/8/8/8, depth 24, window and pbuffer |
| Layers | SDL's `NSViewBackingLayer`, with ANGLE's own `CAMetalLayer` as a sublayer: pixel format 80 (BGRA8Unorm), `maximumDrawableCount` 3, `displaySyncEnabled` 1 at swap interval 1, `presentsWithTransaction` 0, `allowsNextDrawableTimeout` 1 |
| Timing extensions | None: no sync control, no frame timestamps, no presentation time, no MSC rate |
| Swizzle | Saw ANGLE's drawable on 60 of 60 frames |
| 10-bit request (`--bits 10`) | `SDL_CreateWindow`: "Could not create GLES window surface": no 10-bit config |
| `eglSwapBuffers` blocking, depth, `presentedTime` | Pending: the screen was locked (124 swaps per second, every `presentedTime` 0) |

So SDL's path gives a context and a window surface, but no time of any
flip, no present-at-time and no depth control. ANGLE owns the layer. Only
an in-process swizzle sees when a frame is shown. This differs from the
Windows answer in its reason (there, ANGLE's surface was a blt-model
swapchain) but leads to the same structure: SDL makes the window, the
header owns the presentation.

### 5.6 A header-owned CAMetalLayer (`share`, question 1b)

Window 800 x 600, 120 frames per run, screen locked (so presents were not
shown), `--heavy 400` (a fragment loop of about 4.5 ms of GPU time per
frame). Loaded machine.

| Target | Sync | Readback mismatches | Distinct surfaces (made in, us) | GL draw + sync, p50 / p99, us | make-current p50, us |
|---|---|---|---|---|---|
| `drawable_ios` | none | **5 of 120** | 3 (16.9) | 27 / 192 | 1.9 |
| `drawable_ios` | `glFinish` | 0 | 3 | 4600 / 6637 | 2.0 |
| `drawable_ios` | shared event | 0 | 3 | 194 / 337 | 18.2 |
| `drawable_ios` | work scheduled | 0 | 3 | 363 / 707 | 14.5 |
| `drawable_image` | none | **2 of 120** | 3 (102.2) | 56 / 308 | 3.8 |
| `drawable_image` | `glFinish` | 0 | 3 | 4558 / 5051 | 1.4 |
| `drawable_image` | shared event | 0 | 3 | 86 / 239 | 4.9 |
| `drawable_image` | work scheduled | 0 | 4 | 419 / 658 | 8.7 |
| `iosurface` + blit | none | **3 of 120** | - | 47 / 323 | 2.8 |
| `iosurface` + blit | `glFinish` | 0 | - | 4884 / 6142 | 7.4 |
| `iosurface` + blit | shared event | 0 | - | 166 / 418 | 6.8 |
| `iosurface` + blit | work scheduled | 0 | - | 346 / 564 | 7.3 |

- ANGLE and the probe share one MTLDevice: the one ANGLE reports. They
  do not share a queue.
- Without a sync, the probe's command buffer read frames that ANGLE had
  not finished (2 to 5 of 120 under GPU load). A sync is needed.
- `EGL_ANGLE_metal_shared_event_sync` works with the constants used here
  (`EGL_SYNC_METAL_SHARED_EVENT_ANGLE` 0x34D8, object 0x34D9, value
  0x34DA/0x34DB): 0 mismatches and 0 failed syncs, and the CPU does not
  wait for the GPU (0.2 ms against 4.6 ms for `glFinish` at this load).
  `eglWaitUntilWorkScheduledANGLE` also gave 0 mismatches, but "scheduled"
  does not order two queues; it is not a correct sync, only a lucky one.
- CAMetalLayer cycled 3 drawables (4 once). `drawable.texture.iosurface`
  is not NULL, so ANGLE can draw straight into the drawable through an
  IOSurface pbuffer cached per IOSurface, as the Windows backend's
  pbuffer on buffer 0 does. The EGLImage route works too.
- Pending: whether each route keeps the depth of a plain Metal present
  (the `asap` rows of `share` against `present-window`), and the costs on
  a quiet machine.

### 5.7 10-bit (`bits`, question 5)

| Target | Distinct levels of a 1024-step ramp |
|---|---|
| EGL config RGB10_A2 or RGBA16F on ANGLE Metal | none exists (`eglChooseConfig` gives 0) |
| GL texture RGBA8 / RGB10_A2 / RGBA16F (framebuffer object) | 256 / 1024 / 1024 |
| EGLImage of MTLTexture BGRA8Unorm / RGB10A2Unorm / BGR10A2Unorm / RGBA16Float | 256 / 1024 / 1024 / 1024 |
| IOSurface pbuffer 'BGRA' / 'l10r' (RGB10_A2) / 'RGhA' (RGBA half) | 256 / 1024 / 1024 |
| CAMetalLayer pixel format BGRA8Unorm, BGR10A2Unorm, RGB10A2Unorm, RGBA16Float (extended linear sRGB, EDR), BGR10_XR | the layer took each format; frames shown: pending (locked) |

So ANGLE's Metal back end renders 10 bits and half float, but not through
an EGL config: SDL's EGL path cannot get a 10-bit window, and a backend
gets one only by owning the layer and drawing through an IOSurface or
EGLImage of a 10-bit drawable. The display reports 8 bits per sample in
this mode; what reaches the panel needs a photometer.

### 5.8 Input (`input`, question 6)

`AXIsProcessTrusted` 0, `CGPreflightPostEventAccess` 0,
`CGPreflightListenEventAccess` 0: the process that ran the probe may not
post events. The HID pass was skipped (screen locked, app not active).
The `CGEventPostToPid` pass posted 20 keys; none reached the NSEvent
monitor or SDL. Not worked around, as asked. The run needs the
Accessibility permission for the terminal and an unlocked screen.

### 5.9 Vblank sources while locked (`links --allow-locked`, 3 s)

Not timing results, but they show what to look for:

- CVDisplayLink: 180 callbacks, fitted period 16.680373 ms; `inNow`
  residuals about the line p50 215 us, max 786 us (an earlier 2 s run:
  p50 537 us, max 1470 us); `inOutputTime - inNow` 23.0 ms (1.38
  periods); callbacks 1 to 18 us after `inNow`'s time.
- CADisplayLink: 180 callbacks; fitted period 16.680375 ms; residual
  0.00 us: its timestamps are computed on a fixed grid, not stamped per
  vblank; `targetTimestamp - timestamp` = `duration` = 16.680 ms; its
  timestamps sit 7.3 ms after CVDisplayLink's `inNow`.

Whether either sits on the scanout's vblank is what `links-window` and
`links-space` measure, against `presentedTime`.

## 6. Results of the timing sessions, 2026-11-05

Two sessions ran `sh tests/probe/screen_macos/run.sh all` from the
mac-integration worktree. The clean session is the primary data. The
first session had a macOS Screen Sharing session open; its results are
kept as the "Screen Sharing" condition, because Screen Sharing changed
some of them. Depth and grid numbers are on the presentedTime grid,
computed from the CSVs (section 3.1's depth is on the CVDisplayLink grid
and reads one less for half the commits).

| Session | Results folder (in `.claude/worktrees/mac-integration/build/macos/`) |
|---|---|
| Clean | `screen_probe_clean/` |
| Screen Sharing | `screen_probe_20261105/` |

### 6.1 Conditions

| Item | Clean | Screen Sharing |
|---|---|---|
| Time | 09:33 to 09:50 | 05:24 to 05:38 |
| macOS | 27.0.1 (26A434) | same |
| Machine | Mac mini (Mac14,3), Apple M2, AC (no battery) | same |
| Display | M27Q over HDMI; desktop 1920 x 1080, labeled 60 Hz; CVDisplayLink 400330/24000000 s = 16.680417 ms (59.9505 Hz); 120 and 144 Hz in the `exclusive` conditions | same |
| Session | Unlocked, display on, no `screensharingd` in the process list (`replayd` at 0% CPU), `HUD=1` | Unlocked, display on, **a Screen Sharing session open** (`screensharingd` 4.4% CPU at the start; WindowServer 15.9 to 30%) |
| ANGLE, SDL | Electron 43.8.3's (SHA-256 as in section 5), SDL 3.4.0 | same |
| Other load | None | Other agents' builds had stopped |

`HUD=1` was meant to record the Metal HUD's present mode (direct or
composited) through the unified log. Every `log_*.txt` holds one line:
"log: Must be admin to run 'stream' command". So the HUD's log was not
captured, and no session has a record of the present mode.

### 6.2 SDL3's EGL path

The swap times come from the swizzled drawables (section 4.2).

| Condition | swaps/s | depth (vblanks after the one before the call) | presentedTime minus swap call p50, ms | shown / 600 |
|---|---|---|---|---|
| window, interval 1 | 60.6 | 3: 591 of 591 | 48.5 | 591 |
| window, interval 0 | 267 | <=0: 71, 1: 218, 2: 23, 3: 24, 4: 6 (no vsync) | 4.1 | 342 |
| space, interval 1 | 60.4 | **2: 592 of 592** | **32.8** | 592 |
| window, 10 bits | `SDL_CreateWindow`: "Could not create GLES window surface" | | | |
| Screen Sharing: window, interval 1 | 70.5 | 3: 509 of 509 | 48.2 | 509 |
| Screen Sharing: space, interval 1 | 62.4 | 3: 576 of 576 | 48.6 | 576 |

`eglSwapBuffers` does not block on the vblank; the loop is paced by
ANGLE's `nextDrawable`, three drawables deep. In a window every frame
shows three vblanks after its swap; in a fullscreen space two, without
Screen Sharing (6.5 has the same difference on the probe's own layer).
No extension exposes any of it. The 10-bit failure is the missing
config (6.8).

### 6.3 Shared presenter (question 1b)

Clean: every `share` run, light and heavy (`--heavy 400`), showed 294 to
300 of 300 frames (the first run of each process 294, every other run
300), with 3 to 5 distinct drawable surfaces and no readback mismatch but
1 of 300 in `iosurface_event` (light and heavy). The light condition ran
right after `egl_space_i1`, as in the Screen Sharing session.

With Screen Sharing, `share_heavy` showed 300 of 300 in every run but the
first (`drawable_ios_none`, 2 of 8 shown), and the light `share` showed
no frame at all: the drawable runs stopped at 8 distinct surfaces with 0
of 8 shown, and the `iosurface` runs showed 0 of 300, with `nextDrawable`
waiting its 1 s timeout at p99. Two reruns of `egl --kind space` followed
by `share` did the same (0 shown in every run); `share` alone, twice, did
not. So the blank windows came with Screen Sharing open (3 of 3 runs
after a fullscreen space), and not without it (1 of 1). That is one clean
run; Screen Sharing is the one difference found between the sessions,
not a cause shown by a test that switches only it.

Screen Sharing session, heavy, 300 frames per run (window):

| Run (heavy) | mismatches | GL draw + sync p50 / p99, us | depth 2 of 300 | presented minus commit p50, ms |
|---|---|---|---|---|
| drawable_ios_finish | 0 | 5062 / 5703 | 289 | 26.4 |
| drawable_ios_event | 0 | 142 / 257 | 300 | 31.3 |
| drawable_ios_scheduled | 0 | 252 / 389 | 300 | 31.3 |
| drawable_image_none | **3** | 119 / 223 | 300 | 31.5 |
| drawable_image_event | 0 | 145 / 245 | 300 | 31.3 |
| iosurface_event | 0 | 175 / 256 | 300 | 31.1 |

In the blank state (Screen Sharing):

- Core Animation hands out a new drawable, on a new IOSurface, for nearly
  every frame (8 distinct in 8 frames), because none is consumed. The
  header's 6-slot pbuffer cache would then make a pbuffer each frame (3.6
  to 34 us each, kept to 6): bounded, and only while no frame is shown,
  whose records are estimated anyway.
- `iosurface_event` had 28 readback mismatches in 300 (1 in the clean
  session): in the `iosurface` target one IOSurface is drawn by ANGLE and
  read by the probe's blit, and nothing orders ANGLE's next frame after
  the probe's blit of the frame before (a write after read). The header
  does not use that target: ANGLE draws into the drawable itself.
- Presented handlers came after the probe's 1 s drain for 11 to 21 of 300
  frames, and wrote into the run's frame array after the probe freed it:
  the exit 139 at the end of the condition. Fixed in the probe (`a79e04c`):
  frame arrays are never freed.

### 6.4 Vblank sources (question 2)

| Item | window | space | Screen Sharing: window / space |
|---|---|---|---|
| CVDisplayLink: period fit; \|residual\| p50 / p99 / max, us | 16.680368 ms; 210 / 788 / 807 | 16.680375 ms; 210 / 767 / 782 | 16.680380 / 16.680368 ms; p99 668 / 692 |
| CV callback minus `inNow` p50 / p99, us | 3.8 / 7.7 | 3.7 / 6.8 | 4.4 / 12.4, 4.6 / 10.0 |
| CV `inOutputTime` minus `inNow` | 24.0 ms | 24.0 ms | 24.0 ms |
| CADisplayLink: period fit; \|residual\| p99 | 16.680375 ms; 0.00 us | 16.680375 ms; 0.00 us | same |
| CADisplayLink callback minus timestamp p50 / p99, us | 69 / 98 | 62 / 90 | 55 / 97, 47 / 87 |
| CADisplayLink grid minus CV grid | +7511 us | +7529 us | +7592 / +7569 us |
| presentedTime: period fit; \|residual\| p99 / max | 16.680372 ms; 0.32 / 0.44 us | 16.680371 ms; 0.49 / 0.54 us | 0.23 / 0.42, 0.29 / 0.37 us |
| presented handler minus presentedTime p50 / p99, us | 523 / 600 | 489 / 599 | 567 / 653, 576 / 678 |
| frame loop waiting for each handler: frames in 60 s; steps of 2 vblanks | 1819; 1777 (41 steps of 1) | 1798; 1797 | 1896; 1701 / 1801; 1797 |

- presentedTime and CADisplayLink's timestamp lie on one grid (0.1 us
  apart), computed (residual under 1 us everywhere; under 0.5 us over
  every clean `present` run with display sync). CVDisplayLink's `inNow`
  lies 0.42 to 0.45 period before it at 59.95 Hz (6.94 to 7.59 ms by run
  and session) and 0.31 to 0.40 period at 120 and 144 Hz, and jitters by
  up to 0.9 ms. Which one is the scanout's vblank, or where the light is,
  only a photodiode says.
- The handler always came after presentedTime (0.30 to 0.76 ms over every
  run, all rates, both sessions).
- A frame loop that waits for each frame's handler shows every other
  vblank. In the clean window 41 of 1818 steps were one vblank: a frame
  committed just after a vblank showed on the next one (also 2 of 297 in
  the clean window's `asap`, 6.5).

### 6.5 Depth and latency per window kind

Depth: the shown vblank minus the last presentedTime vblank before the
commit returned. `asap` commits one frame at a random phase, one in
flight; `pipe` keeps three drawables queued (no wait but `nextDrawable`);
`mindur` presents with a minimum duration of 2 periods. Clean session
unless named.

| Kind (rate) | asap: depth 2 / 3 (frames 4 on) | asap: latest commit that missed, a period plus (ms) | asap presented minus commit min / p50, ms | pipe: presented minus commit p50, ms (periods) | mindur p50, ms | Screen Sharing: pipe p50, ms |
|---|---|---|---|---|---|---|
| window (59.95 Hz) | 278 / 16 (1: 2) | 0.80 | 15.88 / 25.69 | 48.2 (3) | 81.8 | 47.4 |
| borderless | 275 / 21 | 1.17 | 17.92 / 26.25 | 48.7 (3) | 82.1 | 48.1 |
| space | 284 / 12 | 1.02 | 17.49 / 25.78 | **32.6 (2)** | **65.9** | 48.4 |
| fullscreen (a space too, 6.10) | 287 / 9 | 0.53 | 17.39 / 25.76 | **32.6 (2)** | **65.9** | 48.5 |
| exclusive 120 Hz | 263 / 33 | 1.03 | 9.54 / 14.15 | 23.7 (2.84) | 40.4 | 23.8 |
| exclusive 144 Hz | 261 / 35 | 1.00 | 8.01 / 11.92 | 19.5 (2.81) | 33.5 | 19.6 |
| window, 2 drawables | 281 / 14 | 1.00 | 17.75 / 26.05 | 31.5 (2) | | 31.2 |
| space, 2 drawables | 287 / 9 | 0.68 | 17.28 / 25.62 | 32.6 (2) | | 31.9 |

Frames 0 to 3 of a run are left out: frame 2 or 3 missed with a commit up
to a period plus 8.2 ms before the vblank in four kinds, as the window
settles.

- **One commit: the same deadline in every kind.** No commit under a
  period plus 0.3 ms before a vblank showed on it; the latest that missed
  came a period plus 0.53 to 1.17 ms before it (clean), up to 1.56 ms at
  120 Hz with Screen Sharing; every earlier commit made it. A space did
  not take a later commit than a window. In the clean window 2 of 296
  frames, committed 0.7 to 0.8 ms after a vblank, showed on the next vblank
  (15.9 ms after the commit): earlier than the rule, never later. The
  header passes a target half a period before its vblank, so such a frame
  cannot show early.
- **A queue: one drawable fewer in a fullscreen space.** With three
  drawables queued, a frame showed 3 periods after its commit in a window,
  borderless and exclusive, and 2 in a space or fullscreen; `mindur` and
  SDL3's EGL path (6.2) show the same difference. With 2 drawables, window
  and space both showed 2. Nothing public says why. A direct path, where
  the display holds the drawable on screen and the compositor holds no
  copy, would give this; it is the only sign of a direct path in the data,
  and the HUD's log was not captured (6.1).
- **Screen Sharing added the frame back.** With Screen Sharing, a space
  and fullscreen queued as a window (48.4 and 48.5 ms), and the one-commit
  numbers did not change.
- `asap`'s steps of 2 and 3 vblanks are its loop (one frame in flight and
  a random wait of up to a period after the handler), not drops: 297 or
  298 of 300 shown in every kind.

### 6.6 presentAtTime, late frames, holds

The probe plans targets on the CVDisplayLink grid. A target "V + frac" is
(frac - 0.416) period from presentedTime's vblank V at 59.95 Hz in the
clean session (0.45 with Screen Sharing), (frac - 0.31) at 120 Hz and
(frac - 0.32) at 144 Hz. Quantization, clean, shown vblank minus V for
leads 2, 3 and 5 (20 presents per cell, so 60 per kind and column):

| frac (from presentedTime's V, 59.95 Hz) | -0.5 (-0.92) | -0.45 (-0.87) | -0.25 (-0.67) | 0 (-0.42) | 0.25 (-0.17) | 0.45 (+0.03) | 0.5 (+0.08) |
|---|---|---|---|---|---|---|---|
| window | 0 (60 of 60) | 0 | 0 | 0 | 0 | 0 in 23, +1 in 37 | +1 (60 of 60) |
| borderless | 0 | 0 | 0 | 0 | 0 | 0 in 19, +1 in 41 | +1 |
| space | 0 | 0 | 0 | 0 | 0 | 0 in 24, +1 in 36 | +1 |
| fullscreen | 0 | 0 | 0 | 0 | 0 | 0 in 25, +1 in 35 | +1 |
| 120 Hz (from V: -0.81 ... +0.14, +0.19) | 0 | 0 | 0 | 0 | 0 | +1 | +1 |
| 144 Hz (-0.82 ... +0.13, +0.18) | 0 | 0 | 0 | 0 | 0 | +1 | +1 |

Lead 1 (V the next CV vblank, so V is 0.42 to 1.42 periods after the
commit) showed a vblank late in 140 of 140 per kind and rate: the commit
deadline of 6.5. So Core Animation shows a drawable at the first
presentedTime vblank at or after its target, less 0 to 0.6 ms (a target
0.56 ms after V showed on V in 91 of 240), and not at the nearest vblank
(a target 0.92 period before V showed on V). With Screen Sharing the
column 0.45 sat at +0.0 period and showed on V, 60 of 60 per kind. The
header's target is the planned vblank minus half a period (7).

| Run | window | borderless | space | fullscreen | 120 Hz | 144 Hz |
|---|---|---|---|---|---|---|
| late: shown; the 30 injected late frames late | 0 of 300 (below) | 300; 30 of 30 | 300; 30 of 30 | 300; 30 of 30 | 300; 30 of 30 | 300; 30 of 30 |
| queue (2 in flight, own vblanks): on target | 40 of 40 | 40 of 40 | 40 of 40 | 40 of 40 | 40 of 40 | 40 of 40 |
| mindur (2 periods): steps of 2 | 299 of 299 | 299 | 299 | 299 | 299 | 299 |
| ahead (window only) | 0.1 to 1.0 s: on target, 1 of 1 each; 2 and 5 s: not shown | | | | | |

The same in both sessions.

- **late.** A frame committed 0.3 period after its target's CV vblank
  (target already past) showed at the first vblank it could make, one
  later than planned, and its presentedTime moved with it: 30 of 30 in
  every kind and rate. A past target behaves as no target.
- **The window's late and ahead.** `ahead` runs only in the window
  condition. Its 2 s target was not shown; every later present in that
  layer (the rest of `ahead`, all 300 of `late`, the first 121 of `sleep`)
  was dropped (presentedTime 0, the handler at once) until about 6.5 s
  after the 5 s present, then `sleep` showed again (179 of 300). It
  happened in both sessions. So the window's `late` says nothing about
  past targets; the other kinds' `late` does. The header holds a target
  more than 0.5 s ahead until it is 0.5 s ahead.
- **sleep** is a probe artefact for "early": it planned on the CV grid and
  slept to a depth measured on the CV grid; every frame showed at depth 2
  on the presentedTime grid.

### 6.7 Drawable count and display sync

| Condition (clean) | asap depth 2 / 3, shown | pipe: periods, latency p50 | notes |
|---|---|---|---|
| window, 3 drawables | 278 / 16, 297 of 300 | 3, 48.2 ms | |
| window, 2 drawables | 281 / 14, 297 of 300 | 2, 31.5 ms | `nextDrawable` p50 16.4 ms in pipe |
| space, 2 drawables | 287 / 9, 298 of 300 | 2, 32.6 ms | `nextDrawable` p50 33.1 ms in pipe |
| window, sync off | depth 1 or less (CV grid) in 298 | 163 of 300 shown | 119 frames shared a vblank |
| space, sync off | depth 1 or less (CV grid) in 299 | 205 of 300 shown | 178 shared a vblank |

With one frame in flight the drawable count changed nothing. With
display sync off, frames share vblanks and many are not shown: not a
timed configuration.

### 6.8 10 bits

`bits` repeated section 5.7: no 10-bit or half-float EGL config; GL
textures, EGLImages and 'l10r' / 'RGhA' IOSurfaces keep 1024 levels. The
layer took every format. Frames shown in the layer test, clean (Screen
Sharing in brackets): BGRA8 16 of 30 (22), BGR10A2 25 (0), RGB10A2 23
(4), RGBA16Float 0 (0), BGR10_XR 11 (10). The test presents 30 frames at
once right after the window opens, so the counts do not compare the
formats. A BGR10A2 layer's frames reach the compositor; whether 10 bits
reach the panel needs a photometer. NSScreen: 8 bits per sample.

### 6.9 Input

In both timing sessions `AXIsProcessTrusted` and
`CGPreflightPostEventAccess` were 0 and no key arrived. On macOS 27 the
Accessibility list showed no entry until a process asked for access
(`AXIsProcessTrustedWithOptions` with the prompt option, or
`CGRequestPostEventAccess`). VS Code was restarted after the grant; this
run does not show whether the restart was necessary.

Run 2026-11-05 11:33, clean session, `--kind window`, 200 keys per pass,
all three permissions 1. Every key reached the NSEvent monitor and SDL in
both passes. Times in us; "post" is the mach time before the
`CGEventPost` call.

| Pass | NSEvent.timestamp minus post, p50 / p99 / max | SDL timestamp minus post, min / p50 / p99 / max | SDL timestamp minus NSEvent.timestamp, p50 / max | SDL event read minus post, p50 / p99 |
|---|---|---|---|---|
| `CGEventPost(kCGHIDEventTap)` | 108 / 4567 / 40490 | 371 / 424 / 4883 / 44263 | 315 / 3773 | 1783 / 8126 |
| `CGEventPostToPid(self)` | 111 / 3226 / 4902 | 262 / 318 / 3432 / 5109 | 207 / 316 | 1596 / 4443 |

- SDL 3.4 adds an offset to every key's `NSEvent.timestamp`
  (`Cocoa_GetEventTimestamp`, `src/video/cocoa/SDL_cocoaevents.m`). A
  static variable holds the offset. The first event sets it to that
  event's read delay (SDL's clock at the read minus the event's stamp).
  Later events make it smaller only when it would put a stamp after the
  read. Measured: 3773 us on the process's first key, 501 us on the
  third, 316 us from the fourth, then 288, 278, 245, 238 and 207 us in the
  second pass. Every SDL key stamp in a process is late by the shortest
  read delay seen so far: never less, and not constant. A first event
  read after a long frame starts the offset at that frame's length.
- `NSEvent.timestamp` has no such offset: its clock is mach time.
- One HID key was stamped 40 ms after the post, during a
  `CGEventPost` call that took 26 ms. With `CGEventPostToPid` the largest
  value was 4.9 ms.
- `CGEventGetTimestamp` on the posted event read 0 for every key: the
  caller's event copy gets no stamp. The probe's row for it has no
  meaning.
- These are host times for synthetic keys. They leave out the
  keyboard's scan, debounce and USB poll.
- `ysp/screen.h` v0.15.0 gives keys, modifier keys and mouse buttons
  `NSEvent.timestamp` in place of SDL's stamp (docs/screen.md, "Keys and
  mouse buttons: NSEvent.timestamp").

### 6.10 Other findings

- `--kind fullscreen` made a fullscreen space as well (style mask 0x400f):
  SDL's `SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES` "0" set before the window
  did not take. Only `exclusive` (a mode change, level 25) and
  `borderless` (level 25) avoided a space. In the clean session the two
  space kinds queued one drawable fewer than the others (6.5).
- The mode change to 120 and 144 Hz worked through SDL, and the desktop
  mode came back.
- `ysp/screen.h`'s own test (`tests/adapt/screen_metal_test.c`, run after
  the clean session): the process's first open took the period from
  CVDisplayLink, the later opens from CADisplayLink. The link's callbacks
  did come in the first open; its first timestamp was off the grid (15.8
  to 25.2 ms before the second, 5 of 5 processes; docs/screen.md, "The
  first open's period"). Fixed in `ysp/screen.h` v0.14.1.

### 6.11 CAMetalDisplayLink (`link`)

Clean session, the second pass (09:48 to 09:50; in the first pass, at 09:47,
each condition ended at its first `target_at`). Each run: `free` 300 frames, `target_wait` 20
trials.

| Kind, load | preferredFrameLatency | free: shown of 300; steps 1 / 2 | free: presented minus commit p50, ms | free: presented minus targetPresentationTimestamp p50 / p99, us | update to presented, periods p50 | target_wait: on / early / late of 20 |
|---|---|---|---|---|---|---|
| window | 1 | 298; 296 / 0 (one step of 3) | 49.35 | -0.83 / -0.17 (one frame -33.4 ms) | 2.994 | 20 / 0 / 0 |
| window | 2 | 300; 299 / 0 | 49.36 | -0.83 / +0.04 | 2.994 | 20 / 0 / 0 |
| window | default (2.0) | 300; 299 / 0 | 49.36 | -0.75 / -0.08 | 2.994 | 20 / 0 / 0 |
| space | 1 | 298; 296 / 1 | 32.33 | +8.0 / +8.8 (frames up to a period off both ways) | 1.970 | 20 / 0 / 0 |
| space | 2 | 300; 299 / 0 | 32.37 | -1.04 / -0.21 | 1.971 | 20 / 0 / 0 |
| space | default | 300; 299 / 0 | 32.37 | -1.08 / -0.21 | 1.971 | 20 / 0 / 0 |
| window, heavy | 1 | 297; 296 / 0 | 49.29 | -0.92 / -0.17 | 2.993 | 20 / 0 / 0 |
| window, heavy | 2 | 300; 299 / 0 | 49.31 | -0.92 / -0.17 | 2.993 | 20 / 0 / 0 |
| window, heavy | default | 300; 299 / 0 | 49.32 | -1.00 / -0.13 | 2.994 | 20 / 0 / 0 |
| space, heavy | 1 | 293; 285 / 7 | 32.31 | +7.8 / +8.7 (frames up to a period off both ways) | 1.970 | **5 / 0 / 15** |
| space, heavy | 2 | 295; 289 / 5 | 32.29 | -0.88 / -0.13 | 1.970 | **4 / 0 / 16** |
| space, heavy | default | 298; 295 / 2 | 32.32 | -0.63 / +0.17 | 1.970 | **4 / 0 / 16** |

- **`target_at`.** `presentDrawable:atTime:` on a link's drawable throws
  `CAMetalDrawableInvalidOperation` ("-presentAtTime should not be called
  when using CAMetalDisplayLink", as the coordinator read it from the
  first pass's console; the text is not in the results folder) and the
  process aborts. The probe skips the run from `bfe8023`. The link has no
  way to ask for a time: a frame is placed only by choosing the update to
  draw in.
- **Depth.** The link's promise, targetPresentationTimestamp, was 3
  periods after the update in a window (49.9 ms) and 2 in a space (33.3
  ms), at every preferredFrameLatency: the knob changed neither, and at 1
  it dropped more frames (2 to 7 of 300, against 0 to 5 at 2 and the default). These are the
  queue depths of `pipe` (6.5): the link keeps the queue full.
- **The promise.** Idle, presentedTime came within 2 us of
  targetPresentationTimestamp (-2.0 to +0.2 us p01 to p99) in every run
  but the space at latency 1 (+7 to +9 us, and some frames a period off).
  Under load in a space, `target_wait` showed a vblank later than promised
  in 15 or 16 of 20 per latency.
- **The rule of 4.8.** Depth from the update: 3 in a window, above the
  header's 2. Drops: 0 to 7 of 300, against `asap`'s 2 or 3. Hits:
  `target_wait` 4 or 5 of 20 in a loaded space, against `quant`'s 60 of
  60 per cell 0.17 period or more before the vblank (6.6). `target_at`
  cannot run. So `ysp/screen.h` keeps `presentDrawable:atTime:`.

## 7. Recommended design for `YSCR_BACKEND_METAL`

Built in `ysp/screen.h` v0.11.0 (docs/screen.md, "macOS: the header owns
the CAMetalLayer"); its constants set from the Screen Sharing session in
v0.12.0 and checked on the clean session in v0.14.1, which changed none:

| Constant | Value | Rows it rests on |
|---|---|---|
| `YSCR__MTL_TARGET_LEAD` (item 3) | 0.5 period before the planned vblank | 6.6: clean, targets from 0.92 to 0.17 period before a vblank showed on it, 1200 of 1200 at 59.95 Hz, and from 0.81 to 0.06 period before it, 300 of 300 at each of 120 and 144 Hz; 0.03 period after it, on it in 91 of 240; 0.08 period and more after it, a vblank late in every case. With Screen Sharing, 2155 of 2160 from 0.95 period before up to the vblank. 0.5 is the window's middle |
| `YSCR__MTL_IN_FLIGHT` (item 5) | 2 | 6.4, 6.5: the handler comes 0.30 to 0.76 ms after presentedTime and a commit needs a period plus up to 1.17 ms (1.56 with Screen Sharing); waiting for the frame before showed every other vblank (clean: 1777 of 1818 steps of 2) |
| depth (item 5) | 2 in every kind (the composed path's table) | 6.5: one commit's deadline was the same in every kind (the latest miss a period plus 0.53 to 1.17 ms clean). A fullscreen space queued one drawable fewer (three queued: 2 periods against a window's 3), which two frames in flight never reach; the header's own loop in a space showed 119 of 119 frames a vblank apart (docs/screen.md) |
| `YSCR__MTL_MARGIN_NS` (item 5) | 2 ms (`caps.margin_ns`) | 6.5: the deadline's excess over a period (1.17 ms clean, 1.56 ms with Screen Sharing) plus the present call (encode and commit p99 0.28 to 0.35 ms clean, 0.45 ms with Screen Sharing) |
| `YSCR__MTL_DRAWABLES` | 3 | 6.7: 2 or 3 changed nothing with one frame in flight; with two in flight and one drawn, 3 keep one free |
| `YSCR__MTL_AHEAD_MAX_NS` | 0.5 s | 6.6: 0.1 to 1.0 s ahead on target (1 of 1 each), 2 and 5 s not shown and the layer's presents dropped for about 6.5 s, in both sessions |
| `yscr__mtl_tier()` (item 2) | 2 for a shown flip, every kind | 6.4, 6.6: presentedTime moved with 30 of 30 late frames per kind and rate, came before its handler every time, and sits on its grid to under 1 us; that is an OS time for the vblank shown, DXGI_FLIP's composed tier. Tier 1 needs a photodiode (CVDisplayLink disagrees by 0.31 to 0.45 period) |
| `presentDrawable:atTime:`, not CAMetalDisplayLink | | 6.11: the link's drawables refuse a time (an exception), its depth was 3 in a window, and a loaded space missed 15 or 16 of 20 targets |

Items 2 to 5 below are the plan as written before section 6.

1. **Path: the header owns the CAMetalLayer** (question 1b). SDL3 makes
   the window; the header makes the layer with `SDL_Metal_CreateView`,
   an ANGLE display with `EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE` (the
   default back end is OpenGL), and sets `layer.device` to the device that
   `EGL_METAL_DEVICE_ANGLE` reports. ANGLE draws into the drawable through
   an IOSurface pbuffer on `drawable.texture.iosurface`, cached per
   IOSurface (3 to 4), and its frame ends with an
   `EGL_ANGLE_metal_shared_event_sync` signal that the header's command
   buffer waits on before `presentDrawable`. Not SDL's EGL path: it has no
   flip time, no present time and no depth control (5.5). The shape is the
   Windows backend's: SDL owns the window, the header owns presentation,
   ANGLE draws into the header's buffer.
2. **Onset: `MTLDrawable.presentedTime`** from the presented handler, on
   the rt clock with no correlation (rt.h v0.10.0 is mach time). 0 means
   not shown: the record is ESTIMATED with the planned vblank as its onset
   (5.1). **Tier: pending.** Tier 1 needs, per kind: `presentedTime` on a
   steady vblank grid, moving with a late frame (`late`), and the handler
   arriving after `presentedTime` (an observation, not a plan, as
   screen.md's condition C asks). A kind that fails the last test is tier
   3, as COMPOSITION's composed path. The direct path cannot be read per
   frame from a public API, so the tier per kind comes from these runs and
   the describe line names the kind.
3. **flip_at: `presentDrawable:atTime:`** with the planned vblank's own
   time, after the timeline's snap (nearest vblank, lead 0.5), if `quant`
   shows that Core Animation shows a frame at the vblank nearest its target
   (the COMPOSITION result). If it shows "the first vblank at or after",
   the header passes the planned vblank minus half a period instead, so
   the snap stays the timeline's. If neither holds exactly, the `sleep`
   hold is the fallback. **Pending: `quant`, `sleep`.**
4. **Grid: from the flips and CVDisplayLink**, not from the mode's
   refresh rate (60.000 by label, 59.9505 Hz by period). CADisplayLink's
   timestamps are a computed grid; use them as a wake, not as a vblank
   time. **Pending: which source sits on `presentedTime`'s grid (`links`).**
5. **Depth: from the kind**, as screen.md's "Depth from the path", with
   `maximumDrawableCount` 2 if `present-drawables2` shows no drop at depth
   1, else 3, and one frame in flight. **Pending: `asap` per kind.**
6. **Capability kind: fixed grid** (rig_spec 4.2) on this display: no
   variable refresh here. A ProMotion display needs its own run.
7. **Display modes:** enumerate with CoreGraphics (127 modes) or SDL (69);
   take the period from the display link after a change. **Pending: a
   non-60 Hz run (`present-exclusive-120`, `-144`).**
8. **10-bit:** a BGR10A2Unorm (or RGBA16Float with extended linear sRGB)
   layer, ANGLE drawing through an 'l10r' or 'RGhA' IOSurface or an
   EGLImage of the drawable; there is no 10-bit EGL config.
9. **Threads:** Cocoa keeps the window on the main thread; the probe
   presented from another thread with the main thread pumping events. The
   header's window thread (screen.md, v0.10.0) maps onto this.
10. **Locked session and occlusion:** report them; never block on
    `nextDrawable` without its timeout.
11. **Input:** take `NSEvent.timestamp` (mach seconds, so rt time with no
    correlation) through a local monitor, or SDL's timestamp plus its
    constant offset, whichever `input` shows to be closer to the post
    time. **Pending: `input` with the permission.**

## 8. Open questions only a photodiode answers

- The light's delay after `presentedTime` per window kind, and whether
  `presentedTime` is the vblank's time, the start of scanout or the
  compositor's plan. Its offset from the light at the patch (top left).
- Whether a frame that macOS reports shown on vblank V is lit on V, on a
  direct path and on a composed one.
- Whether the 59.95 Hz period that the display link reports is the panel's
  (HDMI 1080p59.94 or a rounded 60) over minutes.
- Whether a mode change to 120 or 144 Hz and back changes the light's
  delay, and whether the panel adds frame interpolation or overdrive
  delay at those rates.
- Whether 10-bit output reaches the panel (photometer steps).

## 9. Checked, and not verified

Checked on 2026-11-04 and 2026-11-05 on the machine of section 5:

- Builds without warnings with `-Wall -Wextra -Wshadow`.
- Every section ran to its end: `env`, `egl`, `share`, `bits` fully;
  `links`, `present` with `--allow-locked` and short runs; `input`.
- The share readback check catches unsynchronized frames (5.6).

Checked in the timing sessions (section 6): every section on a shown
screen at 59.95 Hz, and `present` at 120 and 144 Hz, with and without
Screen Sharing; `link` without it.

Not verified:

- A timing run of the header's own frame loop. Its functional test ran
  on the shown screen: 120 of 120 frames a vblank apart, 0 dropped, tier 2
  (docs/screen.md). The held far target did not run.
- That Screen Sharing causes the blank first windows of 6.3: they came
  with it (3 of 3) and not without it (1 of 1), but no run switched only
  Screen Sharing.
- Whether a fullscreen space is a direct path: its shorter queue (6.5) is
  the only sign. The HUD's present mode was not recorded: `log stream`
  needs an administrator here (6.1).
- SDL's fullscreen without a space (`--kind fullscreen` made a space).
