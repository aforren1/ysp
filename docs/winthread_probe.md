# Window-thread probe, 2026-10-26

This page is reference and explanation. It records a probe that asked
whether the window of `ysp/screen.h` can belong to a thread of the
header's own, and what that costs. The question comes from the ysp-av
host probe (`docs/av_probe.md` on branch `probe-vis`): when a host idles
between trials (Python `time.sleep()` or `input()`, Octave `pause`, C
`Sleep()`), the frame thread pumps no window messages. After 4.1 to
4.6 s Windows marks the window hung, a ghost window takes the
foreground, and the first 2 to 16 frames after the gap go wrong.

The prototype is in [tests/probe/winthread/](../tests/probe/winthread/)
(build and run steps in its README). It is a copy of `ysp/screen.h`
0.9.0 with a window thread, switched on by `YSCR_WINDOW_THREAD=1` in the
environment, so one build runs both designs and every comparison below
is the same binary. `include/ysp/screen.h` is not changed on this
branch. The raw logs and CSV files are in the worker's scratch folder;
the numbers below are the record.

## Summary

1. **Feasible.** The prototype gives the window, its message loop and
   every SDL video call to a thread of the header's own and keeps the
   D3D11 device, the swap path and ANGLE on the host's frame thread.
   DXGI_FLIP (overlay, fullscreen and windowed) and COMPOSITION
   (independent flip) present from the frame thread to a window the
   other thread owns, at tier 1.
2. **The idle trap is gone.** With idle gaps of 5, 10 and 30 s and no
   pump (C `Sleep`, Python `time.sleep`, Octave `pause`, MATLAB
   `pause`), no window was marked hung, no ghost came, and the first
   frame after the gap was on time on independent flip with no vblank
   dropped, in 16 of 16 runs. With 0.9.0 the C, Python and Octave runs
   marked the window hung after 4.20 to 4.50 s, put a ghost in front
   50 ms later, and spoiled 2 to 20 frames after the gap; MATLAB, which
   pumps in `pause`, did not hang but dropped one vblank after the gap.
3. **No cost in the frame loop.** 18 runs of 10 minutes (C and Python
   3.12, N = 10 and 100, fullscreen and windowed, one pair run twice),
   each design back to back, and 4 runs of 2 minutes on DXGI_FLIP: misses, residuals, paths, tiers and the host's work per frame
   did not differ by design. The window thread used 0.27 to 0.45 s of
   CPU in 600 s. A command to it costs a median of 10 us; none runs in a
   frame.
4. **Input is not slower.** Keys still come from SDL's raw-input thread.
   In a frame loop the delay to the frame thread is the frame's phase in
   both designs (p50 7 to 8 ms); spinning on `yscr_poll()`, the window
   thread delivered about 0.5 ms sooner.
5. **Panic: release, do not kill, while the window thread answers.** The
   prototype minimizes the windows, puts back the gamma, and reports
   abort 0x21 (KEY | the new PANIC reason) at the next `begin()`. In C,
   Python, MATLAB and Octave the process lived and the release took 8.5
   to 12.8 ms. The process still ends (exit code 99) when the window
   thread itself does not answer.
6. **Cost of the change.** The prototype adds about 250 lines and moves
   about 185; a version to merge is estimated at 400 to 500 lines and
   150 lines of manual (section 1). The main risk is code that calls SDL
   on `yscr_window()` from the host's thread (the repository's examples,
   Dear ImGui's SDL3 backend): it needs a routing call. macOS cannot use
   the design.
7. **Static SDL3 works** for every host: built from the 3.4.0 release
   tarball (SHA-256 checked) with MSVC (50 s, 13.9 MB `.lib`) and with
   Octave's MinGW gcc (4.5 min, 6.1 MB `.a`), linked with the system
   libraries `SDL3.dll` imports, no `SDL_main`, no hints. A static module is 2.63 MB larger
   than the DLL build and needs no `SDL3.dll`.
8. **Side finding for `ysp/screen.h`:** after a gap of 10 s or more
   without presents, the grid is 0.2 to 1.0 ms off the OS's vblanks and
   the next 16 records carry GRID_UNSTABLE at tier 2 with onsets up to
   1 ms late, in both designs and with the av probe's pump.

## 1. Design

### What the window thread owns

One thread per process, started by the first `yscr_open()` that makes a
window and stopped by the last `yscr_close()`. SDL has one "main
thread", the thread that starts its video subsystem (SDL 3.4.0
`src/SDL.c`, `SDL_InitSubSystem`: "We initialize video on the main
thread. On Apple platforms this is a requirement. On other platforms,
this is the definition."). SDL's video calls are documented "should
only be called on the main thread", and `SDL_PumpEvents()` asserts it in
debug builds (`src/events/SDL_events.c`, `SDL_PumpEventsInternal`). So
the window thread starts and stops SDL's video subsystem, and every SDL
video call goes to it. Several screens share it.

| Moves to the window thread | Stays on the frame thread (the host's) | Any thread, unchanged |
|---|---|---|
| `SDL_InitSubSystem(VIDEO)` and its quit; gamepad init, open, close and quit | D3D11 device and context, DXGI factory | `SDL_PushEvent` (`yscr_push_input`), `SDL_PeepEvents` |
| `SDL_CreateWindowWithProperties`, `SDL_SetWindowFullscreenMode`, `SDL_SyncWindow`, `SDL_DestroyWindow` | DXGI_FLIP: `CreateSwapChainForHwnd` on the window thread's HWND, `MakeWindowAssociation`, `Present1`, frame statistics, the latency waitable | `SDL_AddEventWatch` and the abort watch (it runs on the thread that queues the event) |
| `SDL_SetWindowIcon` and `WM_SETICON`, `SDL_RaiseWindow` with the force hint (FOREGROUND) | COMPOSITION: the DComp device, `CreateTargetForHwnd`, the visual, `IPresentationManager`, its textures and statistics | `SDL_GetWindowProperties`, `SDL_GetWindowSizeInPixels`, `SDL_GetTicksNS` (reads) |
| `SDL_HideCursor`, `SDL_ShowCursor`, `SDL_StopTextInput`, `SDL_StartTextInput`, `SDL_SetTextInputArea` | ANGLE: EGL display, context, client-buffer pbuffers, all GL | the panic watchdog (its own thread and hook), the raw-mice reader (its own message-only window), the idle-wake waiter, the trigger worker |
| the device lists at open (`SDL_GetKeyboards`, `SDL_GetMice`, ...) | `begin()`, `flip_at()`, `wait_flip()`, settling, records, the grid | SDL's raw keyboard thread (keys, stamps) |
| the message loop: `SDL_PumpEvents()` | `yscr_poll()` takes events with `SDL_PeepEvents` (no pump) | the gamma ramp (GDI, by display name) |

The window thread's loop waits in `MsgWaitForMultipleObjectsEx` on its
command event and on any message for its windows (`QS_ALLINPUT`), at most
100 ms (4 ms while gamepads are open, because SDL polls XInput in its
pump), then runs a waiting command and calls `SDL_PumpEvents()`. The
100 ms wake is a heartbeat: it tells a live thread from a hung one
(PANIC, section 4).

### Can DXGI and DirectComposition present from a thread that does not own the window?

Yes, on the paths measured (section 3.1): DXGI_FLIP on the overlay
plane and COMPOSITION on independent flip, each fullscreen and in a
window, at tier 1. DXGI_FLIP's independent-flip and composed paths and
COMPOSITION's composed path did not occur in these runs. What the
documentation says:

- DXGI ("DXGI overview", Multithread considerations): "Be careful that
  you never have the message-pump thread wait on the render thread when
  you use full-screen swap chains. [...] calling Present1 (from the
  render thread) may cause the render thread to wait on the
  message-pump thread. When a mode change occurs, this scenario is
  possible if Present1 calls ::SetWindowPos() or ::SetWindowStyle()".
  The header never makes an exclusive full-screen swapchain (fullscreen
  is a borderless window), and in the design the window thread never
  waits on the frame thread. The frame thread waits on the window thread
  only for a command (open, close, text input, gamepads), never in a
  frame.
- DirectComposition (`IDCompositionDevice::CreateTargetForHwnd`): "the
  window must belong to the calling process". Process, not thread.
- ANGLE renders to pbuffers (client buffers) only; it never touches the
  HWND.

### Input, abort and close

- **Keys** come from SDL's raw-input thread, which stamps them and queues
  them whatever any other thread does (SDL 3.4.0
  `src/video/windows/SDL_windowsrawinput.c`). That does not change.
- **Mouse, focus, close and window events** come from the window
  thread's pump, as soon as Windows posts them, not at the next
  `begin()`.
- **The frame thread** takes events with `SDL_PeepEvents(GETEVENT)`,
  which does not pump (`SDL_PollEvent` pumps, and pumping belongs to
  SDL's main thread). `begin()` no longer pumps.
- **Abort and close requests** need no change. The abort state is one
  lock-free log per process, written by SDL's event watch on the
  thread that queues the event (the raw-input thread for Shift+Esc, the
  window thread for a close request or Alt+F4) and by the watchdog's
  hook; `begin()` reads it.
- **`yscr_push_input()`** already runs on any thread.

### Open and close, in order

`yscr_open()` on the frame thread:

1. Check the desc (unchanged).
2. Start the window thread if this is the first window; wait until its
   message queue exists.
3. Window thread (one synchronous command): SDL hints, video init, the
   doorbell event type, gamepads, the display mode, the window, the mode
   switch, the icon, `SDL_SyncWindow`, `SDL_RaiseWindow`,
   `SDL_StopTextInput`, `SDL_HideCursor`, the mode obtained, the slot,
   the abort watch. A failure leaves the message in `s->error`.
4. Frame thread: the presenter's open (device, swapchain or composition
   target on the HWND, ANGLE).
5. Window thread: the foreground check and the forced raise; the device
   list.
6. Frame thread: raw mice, the display state and gamma, the panic
   watchdog, the context hook, the warm-up and settling. The window
   thread pumps the whole time.

`yscr_close()` on the frame thread: the last flip, the trigger worker,
the watchdog, the gamma, the waiter, the foreground given back, the abort
watch, then the presenter's close (the swapchain or composition target
goes before the window), then one command to the window thread (cursor,
`SDL_DestroyWindow`, icons, gamepads, video quit), then the window
thread stops if this was the last window.

Failure paths: every step of open that fails returns with the message
and calls `yscr_close()`, which undoes what exists, as in 0.9.0. A
command that fails on the window thread does not call `yscr_close()`
there (the close would wait on itself). `CreateThread` failing fails the
open with "window thread: CreateThread failed".

### The change as it would land

State: a process-wide struct (thread, id, command and done events, a
lock for one command at a time, the heartbeat, the release flags, the
user count) and one `int32_t` per screen. Functions:
`yscr__wt_acquire()`/`_release()`, `yscr__wt_run(fn, arg)` (synchronous
command), the loop, and two extracted functions,
`yscr__window_open()` and `yscr__window_close()`, which hold the window
code of 0.9.0's open and close unchanged. Calls rerouted: the
foreground, the device list, text input, gamepad hot-plug, the pump,
`yscr_poll()`.

Size, measured on the prototype: the copy differs from 0.9.0 by 437
added and 189 removed lines (`git diff -w`), of which about 185 are the
window code moved into the two functions, so about 250 new lines, 133
of them the thread itself. A version to merge needs also: a desc field
(`desc.window_thread`: AUTO, ON, OFF) in place of the environment
variable; `YRT_NO_THREADS` and the web build kept on the single-thread
path; `yscr_displays()` and `yscr_modes()` routed to the window thread
while it runs; a public `yscr_window_call(s, fn, ctx)` for code that
calls SDL on `yscr_window()` (section 5); a timeout on the command wait;
the manual (MEMORY AND THREADS, INPUT, ABORT, PANIC, FOREGROUND,
BACKENDS); `tests/adapt` and `tests/compile` updates and a mutation list
entry. Estimate: 400 to 500 lines of code and 150 lines of manual, one
header version (0.10.0).

## 2. Setup

| Item | Value |
|---|---|
| Machine | Laptop, Intel Core i7-1360P (12 cores, 16 threads), Windows 11 Enterprise 10.0.26200.9711 |
| GPU and display | Intel Iris Xe (driver 32.0.101.7088), internal panel 1920 x 1200 at 60 Hz, one display |
| Power | AC, Balanced power plan |
| Screen | the probe copy of `ysp/screen.h` 0.9.0, AUTO backend (COMPOSITION in every 10-minute run); fullscreen is a borderless window over the panel, windowed is 1280 x 720 |
| gfx, ANGLE | `ysp/gfx.h` 0.17.1; ANGLE 2.1.23876 (git fffbc739779a) |
| SDL | 3.4.0, static, built from the release tarball with MSVC 19.51 (section 6) |
| Hosts | `probe_c.exe` (MSVC 19.51, /O2 /MD); CPython 3.12.11 with the abi3 `.pyd`; MATLAB R2023a (MSVC MEX); Octave 10.1.0 (its MinGW-w64 gcc 14.2 MEX) |
| Scene | the av probe's: N stimuli (gabor, grating, 100-dot field, rect in turn), one parameter set and one draw each per frame, a 1 s repeating tween on stimulus 0 |
| Timing | every run through `bash /c/tmp/psy-guard.sh time`, one at a time, CPU load at most 20 % at its start. The user and other workers used the machine between and during runs (section 3.1, "The disturbed run") |
| Photodiode | none. Tiers are what the flip records report, not a measured tier. |

The two designs ran from one binary per host; the environment chose
the design. Each pair ran back to back, 0.9.0 first.

## 3. Measurements

### 3.1 Frame loop, 10 minutes per run

2026-10-26 16:59 to 19:46. "Missed" counts records with `dropped > 0`
(the vblanks in brackets); "late target" is `YSCR_FLIP_LATE_TARGET`;
"occluded" counts OCCLUDED records. Residuals are |onset - target|.
"Work" is the host's time from the return of `begin()` to the call of
`flip_at()`. "Window thread CPU" is the thread's kernel and user time
(`GetThreadTimes`) over the screen's life.

| Host | N | Window | Design | Frames | Missed (vblanks) | Late target | Occluded | Residual us p50 / p99 | Work ms p50 / p99 / max | Path, tier | Window thread CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| C | 10 | fullscreen | 0.9.0 | 35991 | 9 (12) | 0 | 0 | 1.0 / 83.5 | 0.20 / 0.50 / 2.1 | independent 35991; t1 35939, t2 52 | - |
| C | 10 | fullscreen | window thread | 36002 | 0 (0) | 0 | 0 | 0.7 / 62.5 | 0.24 / 0.66 / 2.4 | independent 36002; t1 35999, t2 3 | 328 ms in 600 s |
| Python 3.12 | 10 | fullscreen | 0.9.0 | 32139 (ended, see below) | 41 (46) | 2 | 129 | 0.7 / 2.7 | 0.24 / 0.58 / 3.5 | independent 32139; t1 4235, t3 27904 | - |
| Python 3.12 | 10 | fullscreen | window thread | 36002 | 0 (0) | 1 | 0 | 0.7 / 73.4 | 0.32 / 0.72 / 2.5 | independent 36002; t1 35996, t2 6 | 359 ms in 600 s |
| Python 3.12 | 10 | fullscreen | 0.9.0, rerun 19:46 | 36003 | 0 (0) | 0 | 0 | 0.8 / 2.5 | 0.27 / 0.67 / 2.2 | independent 36003; t1 36003 | - |
| Python 3.12 | 10 | fullscreen | window thread, rerun 19:56 | 36003 | 0 (0) | 0 | 0 | 1.4 / 110.6 | 0.26 / 0.72 / 2.3 | independent 36003; t1 35926, t2 77 | 266 ms in 600 s |
| C | 100 | fullscreen | 0.9.0 | 36002 | 0 (0) | 0 | 0 | 1.5 / 106.9 | 0.70 / 1.38 / 4.4 | independent 36002; t1 35960, t2 42 | - |
| C | 100 | fullscreen | window thread | 36002 | 0 (0) | 0 | 0 | 0.9 / 96.3 | 0.58 / 1.43 / 2.7 | independent 36002; t1 35958, t2 44 | 406 ms in 600 s |
| Python 3.12 | 100 | fullscreen | 0.9.0 | 36002 | 0 (0) | 0 | 0 | 0.6 / 54.2 | 0.96 / 2.36 / 3.3 | independent 36002; t1 35994, t2 8 | - |
| Python 3.12 | 100 | fullscreen | window thread | 36001 | 1 (1) | 0 | 0 | 0.7 / 67.1 | 0.82 / 2.35 / 6.9 | independent 36001; t1 35996, t2 5 | 406 ms in 600 s |
| C | 10 | 1280 x 720 | 0.9.0 | 36003 | 0 (0) | 0 | 0 | 0.8 / 2.5 | 0.23 / 0.55 / 1.3 | independent 36003; t1 36003 | - |
| C | 10 | 1280 x 720 | window thread | 36002 | 0 (0) | 0 | 0 | 0.7 / 2.5 | 0.24 / 0.56 / 1.3 | independent 36002; t1 36002 | 391 ms in 600 s |
| Python 3.12 | 10 | 1280 x 720 | 0.9.0 | 36002 | 0 (0) | 0 | 0 | 0.8 / 2.4 | 0.31 / 0.70 / 2.2 | independent 36002; t1 36002 | - |
| Python 3.12 | 10 | 1280 x 720 | window thread | 36003 | 0 (0) | 0 | 0 | 0.7 / 2.4 | 0.31 / 0.73 / 3.5 | independent 36003; t1 36003 | 453 ms in 600 s |
| C | 100 | 1280 x 720 | 0.9.0 | 36002 | 0 (0) | 0 | 0 | 0.7 / 2.4 | 0.49 / 1.45 / 2.5 | independent 36002; t1 36002 | - |
| C | 100 | 1280 x 720 | window thread | 35997 | 2 (2) | 0 | 1 | 0.7 / 2.4 | 0.47 / 1.38 / 3.1 | independent 35997; t1 35996, t3 1 | 375 ms in 600 s |
| Python 3.12 | 100 | 1280 x 720 | 0.9.0 | 36000 | 2 (2) | 0 | 0 | 0.7 / 2.4 | 0.74 / 2.36 / 3.8 | independent 36000; t1 36000 | - |
| Python 3.12 | 100 | 1280 x 720 | window thread | 36002 | 0 (0) | 0 | 0 | 0.7 / 2.4 | 0.77 / 2.27 / 2.7 | independent 36002; t1 36002 | 297 ms in 600 s |

Reading the table:

- **No difference that follows the design.** Fullscreen and windowed,
  N = 10 and 100, C and Python: 0 to 9 missed frames per 36,000 in the
  0.9.0 runs (the disturbed run aside) and 0 to 2 in the window-thread
  runs; residual p99 in the same range (54 to 111 us fullscreen, 2.4 to
  2.5 us windowed, in both); every frame on independent flip; tier 1 on
  all but 0 to 77 frames per run, in both.
- **The host's work per frame did not change**: p50 within 0.12 ms and
  p99 within 0.16 ms between the two designs of each condition, in
  either direction. `begin()` no longer pumps in the window-thread
  design, but the pump was not a visible part of the frame here (the av
  probe's loop reads no events).
- **The window thread costs 0.27 to 0.45 s of CPU in 600 s** (0.04 to
  0.08 % of one core) with the participant's input idle: the 100 ms
  heartbeat and the window's own messages.
- **The disturbed run.** Python, fullscreen, N = 10, 0.9.0: 71 s in,
  41 misses came in a cluster, then the sync guard fired
  (`YSCR_FLIP_UNSYNCED`, tier 3 from frame 4235 on) and at frame
  32,139 `begin()` failed with "the swap path freed no slot in time"
  (no slot for over 333 ms). Another program's window was over ours at
  times (129 OCCLUDED records). The user was at the machine (a
  Settings window was open). The 0.9.0 code ran, so the window thread
  is not involved; the pair was run again (row "rerun").

The residual p99 of 54 to 111 us in some fullscreen runs and the 3 to
77 tier-2 frames come from stretches in which the OS's vblank times
jittered around the grid by more than 20 us (flagged GRID_UNSTABLE, no
drop), as the av probe saw. They came in both designs: 2,359 to 7,607
records over 20 us in the 0.9.0 runs, 3,980 to 6,624 in the
window-thread runs; the 0.9.0 and the window-thread run of one pair
differed more between pairs than within them.

DXGI_FLIP named (`--dxgi`), C, N = 10, 2 minutes each, 20:07 to 20:16:

| Window | Design | Frames | Missed | Residual us p50 / p99 | Work ms p50 / p99 / max | Path, tier |
|---|---|---|---|---|---|---|
| fullscreen | 0.9.0 | 7202 | 0 | 0.7 / 2.7 | 0.48 / 0.99 / 3.3 | overlay 7202; t1 7202 |
| fullscreen | window thread | 7202 | 0 | 0.5 / 2.5 | 0.64 / 1.07 / 3.5 | overlay 7202; t1 7202 |
| 1280 x 720 | 0.9.0 | 7202 | 0 | 0.6 / 3.1 | 0.69 / 1.22 / 8.6 | overlay 7202; t1 7202 |
| 1280 x 720 | window thread | 7202 | 0 | 0.4 / 2.4 | 0.58 / 0.99 / 1.9 | overlay 7202; t1 7202 |

So a DXGI flip-model swapchain made and presented by the frame thread on
a window that the window thread owns reached the overlay plane at tier 1,
fullscreen and in a window, as in 0.9.0.

Command cost: a synchronous no-op command from the frame thread to the
window thread took a median of 10.3 us and at most 123 us (1000 round
trips, C, fullscreen, 16:58). Commands happen at open and close, for
text input on and off, and for a gamepad plugged in or out, never in a
frame.

### 3.2 Idle gaps with no pump

Each host ran 120 frames, then stopped calling the module for 5, 10 or
30 s (C `Sleep()`, Python `time.sleep()`), then ran 120 more frames.
A thread polled every 50 ms: `IsHungAppWindow()`, the foreground window
and its class, `IsIconic()`, and whether the window covered the monitor.
N = 10, 2026-10-26 16:27 to 16:57. "Bad frames" are the frames before
the first 10 in a row with tier 1, no drop and no flag (the av probe's
definition); "time" spans their predicted onsets.

| Gap | Window | Host | 0.9.0: hung, ghost | 0.9.0: frame 120 | 0.9.0: bad frames | Window thread: hung, ghost | Window thread: frame 120 | Window thread: bad frames |
|---|---|---|---|---|---|---|---|---|
| 5 s | fullscreen | C | 4.26 s, 4.31 s | composed, ESTIMATED + OCCLUDED, tier 3 | 3 (183 ms), `begin()` waits up to 83 ms | never | independent, tier 1 | 0 |
| 5 s | fullscreen | Python | 4.25 s, 4.30 s | composed, ESTIMATED + OCCLUDED, tier 3 | 2 (100 ms), waits up to 63 ms | never | independent, tier 1 | 0 |
| 5 s | window | C | 4.36 s, 4.41 s | 1 vblank dropped, overlay, tier 3 | 3 (67 ms) | never | independent, tier 1 | 0 |
| 5 s | window | Python | 4.45 s, 4.50 s | 1 dropped, overlay, tier 3 | 3 (67 ms) | never | independent, tier 1 | 0 |
| 10 s | fullscreen | C | 4.26 s, 4.31 s | 1 dropped, overlay, tier 3 | 3 (133 ms), waits up to 80 ms | never | independent, tier 1 | 0 |
| 10 s | fullscreen | Python | 4.20 s, 4.25 s | composed, ESTIMATED + OCCLUDED, tier 3 | 20 (533 ms), waits up to 82 ms | never | on time, GRID_UNSTABLE, tier 2 | 17 (283 ms), GRID_UNSTABLE only |
| 10 s | window | C | 4.24 s, 4.29 s | 1 dropped, overlay, tier 3 | 16 (283 ms) | never | on time, GRID_UNSTABLE, tier 2 | 17 (283 ms), GRID_UNSTABLE only |
| 10 s | window | Python | 4.50 s, 4.56 s | 1 dropped, overlay, tier 3 | 15 (267 ms) | never | on time, GRID_UNSTABLE, tier 2 | 17 (283 ms), GRID_UNSTABLE only |
| 30 s | fullscreen | C | 4.25 s, 4.30 s | 1 dropped, overlay, tier 3 | 19 (467 ms), waits up to 82 ms | never | on time, GRID_UNSTABLE, tier 2 | 17 (283 ms), GRID_UNSTABLE only |
| 30 s | fullscreen | Python | 4.25 s, 4.30 s | 1 dropped, overlay, tier 3 | 19 (467 ms), waits up to 84 ms | never | on time, GRID_UNSTABLE, tier 2 | 17 (283 ms), GRID_UNSTABLE only |
| 30 s | window | C | 4.50 s, 4.55 s | 1 dropped, overlay, tier 3 | 16 (283 ms) | never | on time, GRID_UNSTABLE, tier 2 | 17 (283 ms), GRID_UNSTABLE only |
| 30 s | window | Python | 4.46 s, 4.51 s | 1 dropped, overlay, tier 3 | 16 (283 ms) | never | on time, GRID_UNSTABLE, tier 2 | 17 (283 ms), GRID_UNSTABLE only |
| 30 s | fullscreen | Python, `pump()` every 50 ms | never, never | on time, independent, GRID_UNSTABLE, tier 2 | 17 (283 ms), GRID_UNSTABLE only | not run | | |
| 30 s | window | Python, `pump()` every 50 ms | never, never | on time, independent, GRID_UNSTABLE, tier 2 | 17 (283 ms), GRID_UNSTABLE only | not run | | |

With the window thread no window was marked hung, no ghost came, the
window kept the foreground, and no frame after a gap dropped a vblank
or left the independent-flip path, in all 12 runs. The same holds for
the av probe's fix (a pump every 50 ms from the host), measured the same
day.

**A finding for `ysp/screen.h`, not for the window thread: the grid
drifts during a gap.** After every gap of 10 s or more, in both designs
and with the pump, the first 16 frames after the gap carry
`YSCR_FLIP_GRID_UNSTABLE` at tier 2, and the 17th shows the grid moving
to the OS's vblank times. The OS's vblanks were earlier than the
header's grid by 0.09 to 0.16 ms after 5 s (under the 1 % of a period
that flags a record), 0.19 to 0.35 ms after 10 s and 0.37 to 0.96 ms
after 30 s, so the drift grows with the gap: the grid's period, carried
forward with no OS time to correct it, is off by some tens of ppm. While
flagged, a record's onset is the grid's vblank time (THE GRID), so those
16 onsets are up to 1 ms late against the OS's times. No frame dropped.
On COMPOSITION the idle-wake waiter already waits on every vblank while
the screen is open; feeding its vblank times to the grid would most
likely keep it in phase through a gap. Not tried here.

### 3.3 Abort and panic

Fullscreen, N = 10. Keys are sent with `SendInput` from a thread in the
probe, only while the probe's window (or its ghost) is the foreground
window; from the reruns on, the sender waits up to 5 s for the
foreground, because in some runs another program had it for a moment
and the press was skipped.

| Host | Shift+Esc during frames, 0.9.0 | Shift+Esc during frames, window thread | 3 presses 300 ms apart while the host waits 10 s, 0.9.0 | The same, window thread |
|---|---|---|---|---|
| C (`Sleep`) | `begin()` returned QUIT, abort 0x1, 1 press | the same | exit code 99 (16:5x) | process lives; window minimized 12.8 ms after the panic began; next `begin()`: QUIT, abort 0x21, 3 presses |
| Python 3.12 (`time.sleep`) | the same | the same | exit code 99 | process lives; minimized after 9.0 ms; abort 0x21, 3 presses |
| MATLAB R2023a (`pause`) | not run | the same | exit code 99 (av probe) | MATLAB lives ("MATLAB still running after the panic"); minimized after 8.5 ms; abort 0x21, 3 presses |
| Octave 10.1 (`pause`) | not run | the same | exit code 99 (av probe) | Octave lives; minimized after 11.5 ms; abort 0x21, 3 presses |

"Minimized after" is the time from the start of the panic on the
watchdog thread to the end of the release on the window thread. The
first runs of 16:5x skipped the key in three cases (another program had
the foreground for a moment; `keys_sent=0`); the table has the reruns of
20:06 to 20:25, where every press was sent.

### 3.4 Input latency

From `SendInput()` on a helper thread to the frame thread. "avail" is the
time to the `yscr_poll()` that returned the event; "stamp" is the
event's own time on the `ysp/rt.h` clock minus the send. Keys are the B
key by scan code (SDL's raw keyboard path); clicks are left clicks on the
window (the message path). "Spin": the frame thread calls `yscr_poll()`
in a loop and presents nothing. "Frames": the scene's frame loop, with
the events read after each `begin()`. C, fullscreen, N = 10, 200 probes
each, at random gaps of 20 to 50 ms, 2026-10-26 15:54 to 16:27.

| Input | Frame thread | Design | avail ms p50 / p90 / p99 / max | stamp ms p50 / p90 / p99 / max |
|---|---|---|---|---|
| key | spin | 0.9.0 | 1.21 / 1.50 / 1.77 / 1.84 | 0.86 / 1.11 / 1.50 / 1.54 |
| key | spin | window thread | 0.50 / 0.65 / 1.05 / 1.10 | 0.49 / 0.65 / 1.04 / 1.09 |
| key | frames | 0.9.0 | 7.23 / 15.26 / 17.01 / 17.34 | 0.53 / 0.72 / 1.27 / 2.13 |
| key | frames | window thread | 7.94 / 16.24 / 17.61 / 17.70 | 0.82 / 1.21 / 1.56 / 1.62 |
| click | spin | 0.9.0 | 1.07 / 1.41 / 8.65 / 13.40 | -7.11 / -0.64 / 0.64 / 10.41 |
| click | spin | window thread | 0.56 / 0.87 / 1.95 / 13.18 | -6.55 / -0.77 / 1.14 / 1.23 |
| click | frames | 0.9.0 | 8.33 / 17.25 / 27.23 / 33.91 | -5.01 / 1.29 / 8.17 / 20.17 |
| click | frames | window thread | 7.95 / 16.50 / 17.81 / 27.03 | -7.59 / -0.44 / 0.68 / 11.79 |

- In a frame loop the delay to the frame thread is the frame's phase
  (0 to 17 ms) in both designs: the frame thread reads events once per
  frame. The window thread does not shorten it.
- Spinning, the window thread delivered keys and clicks about 0.5 ms
  sooner at the median. In 0.9.0 each `yscr_poll()` pumps the window's
  messages, which costs time; with the window thread it only takes
  from SDL's queue.
- Clicks in a frame loop: 0.9.0 had a p99 of 27 ms and a max of 34 ms,
  over one frame. The most likely cause (not traced): a click that
  reaches the window after `begin()`'s pump waits for the next frame's
  pump. With the window thread the p99 was 17.8 ms, one frame.
- Key stamps (SDL's raw-input thread) differed by up to 0.3 ms at the
  median between runs, in both directions (spin: 0.9.0 later; frames:
  the window thread later). That is run-to-run spread of the raw-input
  thread's wake, not the design; the key path does not go through the
  window thread. Click stamps are the message time, which counts in the
  system tick (-15 to +1 ms against the send), as INPUT says.

### 3.5 MATLAB and Octave

The MEX caller's thread is MATLAB's (or Octave's) main thread; with the
window thread, a second thread of the MEX file owns the SDL window and
SDL's main thread is that thread. Static SDL3 MEX files (MSVC for
MATLAB, MinGW for Octave), 2026-10-26 20:16 to 20:26.

| Case | MATLAB R2023a, 0.9.0 | MATLAB R2023a, window thread | Octave 10.1, 0.9.0 | Octave 10.1, window thread |
|---|---|---|---|---|
| `pause(30)` fullscreen: hung, ghost | never | never | 4.24 s, 4.29 s | never |
| `pause(30)` fullscreen: frame 120 | 1 vblank dropped, independent, tier 2 | on time, independent, tier 2 (grid flag) | composed, ESTIMATED + OCCLUDED, tier 3, `begin()` waits up to 82 ms | on time, independent, tier 2 (grid flag) |
| `pause(30)` window: hung, ghost | never | never | 4.50 s, 4.55 s | never |
| `pause(30)` window: frame 120 | 1 vblank dropped, independent, tier 2 | on time, independent, tier 2 (grid flag) | 1 dropped, overlay, tier 3 | on time, independent, tier 2 (grid flag) |
| abort (Shift+Esc) | | QUIT, 0x1 | | QUIT, 0x1 |
| panic in `pause` | | session lives (above) | | session lives (above) |
| open, 1 s of frames, close, 3 times in one session | | each open made a new window thread, settled (2.52 to 2.53 s), drew 60 frames, closed | | the same |

Nothing broke: every open settled on COMPOSITION, every frame loop ran,
every close returned, the process exited with code 0. MATLAB's 0.9.0
runs did not hang (its `pause` pumps the main thread, as the av probe
found), but the first frame after the gap dropped a vblank; with the
window thread it did not. Octave's `pause` hung the window in 0.9.0 and
not with the window thread. The grid flag after 30 s is section 3.2's
finding.

Not tried in MATLAB: figures or App Designer windows open at the same
time, the desktop (only `-batch`), a MEX file unloaded with `clear mex`
while a screen is open.

## 4. The panic with a window thread

Today's rule (PANIC): 3 presses of the abort combination within 2 s,
with a window of this process (or its ghost) in front, and no abort
reported by a `begin()` for 10 s, end the process with exit code 99.
With a window thread the window never hangs while the host idles, so the
rule fires on a host that is only waiting: the av probe saw it end a
MATLAB session in `pause`.

### Proposal: release first, end only a hung window thread

Keep the trigger as it is; change what it does, by who answers.

1. **The window thread answers** (its heartbeat is newer than 1 s):
   *release the display.* The window thread minimizes every screen's
   window and shows the cursor; the watchdog puts back the gamma ramps,
   writes `YSCR_EV_PANIC` with `u.u32[1] = 1` (released) and pushes an
   abort with a new reason, `YSCR_ABORT_PANIC` (0x20). The process
   lives. The next `begin()` returns `YSCR_QUIT` with `f.abort =
   KEY | PANIC`, and the program decides, as for any abort.
2. **The window thread does not answer** (no heartbeat for 1 s, or the
   release did not finish within 1 s): end the process as today, exit
   code 99.

Why this rule:

- The trigger already separates a loop that answers from one that does
  not: a loop in `begin()` reports press 1 within a frame and is never
  released. A `begin()` heartbeat ("no `begin()` for X s") was
  considered and rejected when the watchdog was built, because loading
  and saving look like a hang (docs/screen.md, "The panic watchdog");
  it is no better now.
- What the panic is for is the operator's desktop: a fullscreen window
  over it, the gamma ramp and the display mode. A live window thread
  can give all of those back without ending the process, so a host that
  only waits (MATLAB `pause`, Python `input()`) loses no data and no
  session. A false release costs a minimized window and an abort the
  program sees.
- When the window thread itself is stuck (a driver call, a deadlock),
  only the end of the process takes the window down, as today.

What release does not do: a frame thread hung inside D3D11 or ANGLE stays
hung (the operator ends it in Task Manager). A switched display mode
should come back: SDL leaves fullscreen when a fullscreen window is
minimized (`SDL_OnWindowMinimized` in `src/video/SDL_video.c` calls
`SDL_UpdateFullscreenMode(window, SDL_FULLSCREEN_OP_LEAVE, ...)`). Read
in the source, not measured: the probe does not switch modes.

Prototyped in the copy (`yscr__panic()`, about 30 lines). Measured in
section 3.3.

## 5. Risks

- **SDL off the process's main thread.** SDL 3.4 on Windows has no
  rule that ties video to the process's first thread: its "main thread"
  is the thread that starts video (section 1). Rules that follow: every
  SDL video call goes to the window thread; `SDL_PollEvent()` and
  `SDL_PumpEvents()` must not run on the frame thread (they pump, and
  run SDL's main-thread callbacks; debug builds of SDL assert); a
  program that calls SDL video functions on `yscr_window()` from its own
  thread breaks the rule. The repository's examples do that
  (`SDL_SetWindowTitle`, `SDL_SetWindowMouseGrab`, `SDL_HideCursor`),
  and Dear ImGui's SDL3 backend calls SDL's cursor and text-input
  functions from the thread that runs it (not checked line by line
  here). A merged design needs `yscr_window_call(s, fn,
  ctx)` and the manual must say it; Dear ImGui on a screen's window
  needs its platform calls routed through it or the window thread off.
  `SDL_RunOnMainThread()` would also reach the window thread, but only at
  its next wake (up to 100 ms): SDL posts its wake-up message only to a
  window whose thread waits in `SDL_WaitEvent` (`SDL_SendWakeupEvent`,
  `src/events/SDL_events.c`).
- **Event memory.** SDL keeps the strings of text, IME and drop events
  in the memory of the thread that takes the event, and frees them in
  that thread's next pump (`SDL_FreeTemporaryMemory`,
  `src/events/SDL_events.c`). The frame thread never pumps, so with text
  input on, each text event's string stays until the frame thread ends
  or calls `SDL_CleanupTLS()`. Small (bytes per typed character), not
  measured. A merged design could call `SDL_CleanupTLS()` on the frame
  thread at close, or when text input stops.
- **MATLAB and Octave.** See section 3.5.
- **Fullscreen transitions.** The window thread makes the window
  fullscreen at open (borderless, in the desktop mode) and the presenter
  then makes the swapchain or composition target on it, as in 0.9.0. A
  mode switch (`desc.mode`) runs on the window thread too, before the
  presenter opens. Neither a mode switch nor a fullscreen change while
  the screen is open was tried. DXGI's deadlock rule is about exclusive
  full-screen swapchains, which the header does not make.
- **Multi-monitor.** One window thread serves every screen; each
  screen's presenter stays on its frame thread. Not tried: there is one
  display on this machine.
- **The web backend.** No threads (`YRT_NO_THREADS`, one JavaScript
  thread); the browser's event loop runs between frames. It keeps the
  single-thread path, which the copy leaves unchanged when the variable
  is off. A merged design must keep both paths and choose at open
  (`desc.window_thread`).
- **Linux X11 (stub today).** The same trap exists: a window manager
  pings its windows (`_NET_WM_PING`), and GNOME's mutter marks a window
  that does not answer as not responding (after 5 s by default, its
  `check-alive-timeout`; not checked on the Linux rig). SDL answers the
  ping in its pump (`src/video/x11/SDL_x11events.c`), so the window
  thread fixes it there too. SDL 3.4 calls `XInitThreads()` when it
  opens the display (`SDL_x11video.c`), so Xlib may be used from two
  threads on SDL's connection; a GLX backend that swaps on the frame
  thread can use that connection or its own connection to the same
  window. To be measured with the X11 probe (docs/screen_x11_probe.md).
- **macOS (not planned).** Cocoa windows belong to the process's main
  thread, and SDL asserts that video starts there on Apple platforms.
  A window thread of the header's own is not possible there; a host
  that idles on its main thread would get the spinning cursor. This
  design is Windows and Linux only.
- **The frame thread waits on the window thread** for each command.
  If the window thread is stuck (a driver call in `SDL_DestroyWindow`),
  `yscr_close()` waits forever in the copy. A merged design needs a
  timeout and a message.
- **A custom presenter** (`desc.presenter`, BACKEND_CUSTOM) gets a window
  that another thread owns; it must call SDL video functions through the
  window thread too.

## 6. Static SDL3

The user chose a static SDL3 for ysp-av (av_probe.md section 5: a module
that imports `SDL3.dll` takes any `SDL3.dll` already in the process).

Source: the release tarball
`https://github.com/libsdl-org/SDL/releases/download/release-3.4.0/SDL3-3.4.0.tar.gz`,
15,567,602 bytes, SHA-256
`082cbf5f429e0d80820f68dc2b507a94d4cc1b4e70817b119bbb8ec6a69584b8`, the
digest GitHub gives for the release asset (`gh api
repos/libsdl-org/SDL/releases/tags/release-3.4.0`). A
`tools/vendor_sdl.py` like the other vendor scripts would pin this
hash; not written here.

Build (steps in the prototype's README): CMake with
`-DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF
-DSDL_EXAMPLES=OFF`, Release, Ninja.

| Toolchain | Output | Size | Build time |
|---|---|---|---|
| MSVC 19.51 (VS 18), /MD | `SDL3-static.lib` | 13,916,450 bytes | 50 s |
| Octave 10.1's MinGW-w64 gcc 14.2 | `libSDL3.a` | 6,119,834 bytes | 4 min 33 s |

What a static SDL3 needs:

- **System libraries** that `SDL3.dll` imports itself, from SDL's
  `sdl3.pc`: `kernel32 user32 gdi32 winmm imm32 ole32 oleaut32 version
  uuid advapi32 setupapi shell32 dinput8`. The linked modules import all
  of them but `dinput8` (SDL loads DirectInput at run time).
- **No `SDL_main`.** On Windows, `SDL_main.h` makes SDL's entry point
  available, not needed; the header does not include it, and the static
  modules opened, drew and closed in C, Python, MATLAB and Octave with
  their own entry points.
- **No hint or call more** than with the DLL. SDL's main thread is the
  thread that starts video (section 1), static or not.
- **The C runtime must match.** The SDL build used CMake's default,
  `/MD`, as Python extensions and MEX files are built; a `/MT` module
  would need `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`.
- **The MinGW MEX imports `libgcc_s_seh-1.dll`**, which Octave ships
  in its `mingw64/bin`.
- **The MinGW build for Octave** links `libSDL3.a` with `-lSDL3`: no
  MSVC import library, so the `ld` 2.43 abort of av_probe.md section 4
  does not arise.

Sizes (bytes) against the DLL build of the same code:

| Module | With `SDL3.dll` (2,788,352 beside it) | Static |
|---|---|---|
| `probe_c.exe` | 650,752 | 3,278,336 |
| `ysp_av_probe.pyd` | 637,440 | 3,265,024 |
| MEX, MSVC (MATLAB) | 641,536 | 3,269,120 |
| MEX, MinGW (Octave), not stripped | 4,489,063 | 9,871,996 (4,992,000 stripped) |

A static module is about 2.63 MB larger and needs no `SDL3.dll`: about
0.16 MB less in all. `dumpbin /dependents` lists no `SDL3.dll` in any
static module.

Two SDL instances in one process (a static SDL3 in ysp-av beside another
package's SDL3) were not tried.

## 7. Verdict

**Feasible, measured on this machine.** A window thread inside
`ysp/screen.h` removes the idle trap for every host: no hung window, no
ghost, and the first frame after a gap of 5, 10 or 30 s on time at tier
1 on independent flip (or on time with the grid flag of section 3.2,
which the pump fix shows too), in C, Python, MATLAB and Octave,
fullscreen and in a window. The frame loop's misses, residuals, paths,
tiers and host work did not change over 18 runs of 10 minutes and 4 of
2 minutes. The window thread costs 0.04 to 0.08 % of a core.

**Cost.** About 400 to 500 lines in the header and 150 lines of manual
(section 1), one header version, the risks of section 5: SDL calls on
`yscr_window()` from the host's thread need a routing call; Dear ImGui on
a screen's window needs adapting or the window thread off; macOS cannot
use it.

**What it changes for the bindings.**

- `time.sleep()`, `input()`, Octave `pause` and MATLAB `pause` between
  trials are safe; ysp-av needs no `wait()` that pumps for the window's
  sake (a `wait()` may still be useful to read events).
- `begin()` no longer pumps the host thread's messages. In MATLAB, a
  MEX `begin` called from MATLAB's main thread then no longer dispatches
  MATLAB's own window messages inside the call.
- The panic releases the display in place of ending a process that
  only waits, so a MATLAB or Octave session in `pause` survives three
  Shift+Esc presses (section 3.3).
- Events reach SDL's queue while the host idles; the binding reads them
  at the next `poll()`, each with its own time. Keys did so in 0.9.0
  too (SDL's raw-input thread); clicks and window events now do as
  well, where 0.9.0 left them in the hung window's message queue.
- The bindings must not call SDL directly on the window.

## Not run

- No photodiode: every tier here is the record's.
- One machine, one display (the Iris Xe panel at 60 Hz), AC only. No
  second display, no other refresh rate, no NVIDIA output.
- The 10-minute runs used COMPOSITION (AUTO); DXGI_FLIP ran 2 minutes
  per condition only. MATLAB and Octave ran the idle, abort, panic and
  reopen cases, not the 10-minute loop.
- No display-mode switch (`desc.mode`), no fullscreen change while
  open, no gamepads, no text input, no raw mice, no triggers, no
  `desc.on_context` other than ysp/gfx.h's, with the window thread.
- The panic's release with a hung frame thread inside D3D11 or ANGLE
  (the probe's frame thread sleeps), and the kill path with a hung
  window thread, were not provoked.
- Dear ImGui on a screen's window with the window thread; SDL calls on
  `yscr_window()` from the frame thread.
- The event-memory growth with text input on (section 5).
- Linux, macOS and the web: the copy changes the Windows path only.
- Two SDL3 instances in one process (static SDL3 beside another
  package's SDL3); Python other than 3.12.11.
- The machine was in use during the runs (section 3.1, "The disturbed
  run"); every run went through the guard.

## Open questions

1. Default: should `desc.window_thread` be on by default on Windows (the
   bindings need it; C programs that call SDL on the window must route
   their calls), or off by default with the bindings turning it on?
2. Should `yscr_window_call()` be public API, or should the header
   offer the few window calls the examples use (title, mouse grab,
   cursor) itself?
3. Dear ImGui's SDL3 backend on a screen's window: route its platform
   calls, or keep the operator console in its own window (the
   configuration docs/imgui_probe.md recommends), which the window
   thread could own too?
4. The panic's release: should a released screen offer a way back
   (restore the window, settle again), or should a released screen only
   close?
5. The grid drift through a gap (section 3.2): feed the idle-wake
   waiter's vblank times to the grid on COMPOSITION, and what on
   DXGI_FLIP?
6. A timeout on commands to the window thread, and what `yscr_close()`
   does when the window thread does not answer.
