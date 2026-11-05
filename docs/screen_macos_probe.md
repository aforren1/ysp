# macOS display-timing probe, 2026-11-04

This note describes a probe that measures what macOS, Metal and ANGLE
give a display backend. Its results decide how the macOS backend of
`ysp/screen.h` (`YSCR_BACKEND_METAL`, a stub today) is built. The plan
that asks for the backend is [rig_spec.md](rig_spec.md) section 4.2
("macOS: whatever ANGLE's Metal backend exposes, measured before it is
claimed"), section 8 (timing model, tiers) and section 13 (SDL3's EGL
path on Cocoa; ANGLE's 10-bit configurations on Metal). The Windows
measurements that the probe mirrors are in [screen.md](screen.md). The
house pattern for a probe is [screen_x11_probe.md](screen_x11_probe.md).

State: the functional part ran on 2026-11-04 and 2026-11-05 (section 5).
The timing runs have not run: other jobs compiled on the machine, and the
console session was locked the whole time, so no frame reached the panel
(section 5.1). Section 6 is the template for their numbers. Section 7
recommends a design from what is known now and names what waits.

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

## 6. Results of the timing runs (template)

Fill from `summary.txt`. Leave a cell empty when the probe did not report
it.

### 6.1 Conditions

| Item | Value |
|---|---|
| Date; macOS; load (processes.txt) | |
| Display mode and period (CVDisplayLink nominal, fit) | |
| Session unlocked, display on, AC | |

### 6.2 SDL3's EGL path

| Condition | swaps/s | swap call p50 / p99, us | return phase p50 | depth histogram | `presentedTime` minus swap p50, ms | `|presentedTime - grid|` p99, us |
|---|---|---|---|---|---|---|
| window, interval 1 | | | | | | |
| window, interval 0 | | | | | | |
| space, interval 1 | | | | | | |

### 6.3 Shared presenter

Copy the `RESULT share` lines and the `asap` rows of each run (depth,
shown).

### 6.4 Vblank sources

| Item | window | space |
|---|---|---|
| CV: period fit, |residual| p50 / p99 / max, us; missed | | |
| CV: callback minus `inNow` p50 / p99, us | | |
| CA: period fit, |residual| p99, us; missed | | |
| CA timestamp minus nearest CV `inNow`, p50, us | | |
| `presentedTime`: phase after CV grid, us; |residual| p99, us | | |
| presented handler minus `presentedTime`, p50 / p99, us | | |

### 6.5 Depth and latency per window kind

| Kind | depth histogram (asap) | presented minus commit p50 / p99, ms | handler minus `presentedTime` p50, us | pipe: `nextDrawable` p50, ms; steps | HUD says |
|---|---|---|---|---|---|
| window | | | | | |
| borderless | | | | | |
| space | | | | | |
| fullscreen | | | | | |
| exclusive 120 Hz | | | | | |
| exclusive 144 Hz | | | | | |

### 6.6 presentAtTime, late frames, holds

Copy each kind's quantization table. Then:

| Kind | ahead: largest target that held | late: on / late (injected late of 10%) and does `presentedTime` move | sleep: on / early / late | queue: on / late | mindur: steps |
|---|---|---|---|---|---|
| window | | | | | |
| space | | | | | |
| exclusive 120 Hz | | | | | |

### 6.7 Drawable count and display sync

| Condition | depth histogram | pipe: `nextDrawable` p50 | pipe: steps 0 / 1 / 2 | notes |
|---|---|---|---|---|
| window, 2 drawables | | | | |
| space, 2 drawables | | | | |
| window, sync off | | | | |
| space, sync off | | | | |

### 6.8 10-bit layers

| Format | frames shown of 30 |
|---|---|

### 6.9 Input

| Path | keys seen | NSEvent.timestamp minus post p50 / p99, us | SDL timestamp minus post p50 / p99, us | SDL minus NSEvent p50, us | sub-ms stamps |
|---|---|---|---|---|---|
| `CGEventPost` (HID) | | | | | |
| `CGEventPostToPid` | | | | | |

## 7. Recommended design for `YSCR_BACKEND_METAL`

From section 5, with the open items marked. Section 6 decides them.

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

Not verified:

- Every number of section 6: the screen was locked and other jobs ran.
- That `MTL_HUD_LOG_ENABLED=1` writes the present mode to the unified log:
  `log show` returned nothing for the probe's process here.
- The fullscreen kinds' transitions (space animation, mode change) and
  the restore of the desktop mode after `exclusive`; SDL restores it when
  the window leaves fullscreen.
- The `queue` run with real presents (it had 2 frames in the smoke run).
