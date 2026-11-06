# ysp/screen.h design notes

This note explains why `ysp/screen.h` has its current form and records
what was measured. The manual in the header tells you how to use it. The
plan that asked for the header is section 4.2 of
[rig_spec.md](rig_spec.md).

The Windows numbers come from one machine: a Windows 11 25H2 laptop (build 26200)
with an Intel Iris Xe that drives the internal 1920 x 1200 panel and an
NVIDIA RTX A500 with no outputs. The panel has only 60 Hz modes; SDL3
reports the refresh as 15413000/256880 Hz, 60.0008 Hz, and the vblank
times agree with that to 1 ppm. ANGLE is the copy that ships with Docker
Desktop's Electron front end, `2.1.23876 git hash: fffbc739779a`, not a
pinned build. SDL3 is 3.4.0. The frame thread ran at
`THREAD_PRIORITY_TIME_CRITICAL`. No photodiode was attached, so every
onset below is a software timestamp: the vblank time DXGI reports, not
light on the panel. The Linux X11 numbers come from a second machine,
described in [Linux X11: GLX_OML](#linux-x11-glx_oml-v0170).

## Windows: the header owns the swapchain

The spec first said "EGL through ANGLE via SDL3". That path was measured
and dropped.

With `SDL_HINT_OPENGL_ES_DRIVER` and `SDL_HINT_VIDEO_FORCE_EGL`, SDL3 makes
an ES 3.0 context through ANGLE on the window. ANGLE then makes the
window surface's swapchain itself, and its source (`NativeWindow11Win32.cpp`)
shows that an HWND surface is `DXGI_SWAP_EFFECT_SEQUENTIAL` with one
buffer, a blt-model swapchain. A blt-model swapchain has no frame
statistics in a window, no frame latency control, and can never get an
overlay plane or independent flip. Measured, fullscreen:

| | Result |
|---|---|
| `eglGetSyncValuesCHROMIUM` (advertised) | failed on 180 of 180 frames |
| 180 swaps, swap interval 1 | 3.6 s, about 50 Hz |
| one swap call | 0.4 to 54 ms |

So on Windows SDL3 makes only the window. The header makes a D3D11 device
on the adapter whose output shows the window, a
`DXGI_SWAP_EFFECT_FLIP_DISCARD` swapchain with two buffers, a frame latency
waitable object and `SetMaximumFrameLatency(1)`, and an ANGLE display on
that device (`EGL_ANGLE_device_creation_d3d11`). ANGLE draws into a pbuffer
made from swapchain buffer 0 (`EGL_ANGLE_d3d_texture_client_buffer`). In
D3D11's flip model only buffer 0 can be named, and it always aliases the
current back buffer; asking for buffer 1 gives `EGL_BAD_PARAMETER`. A
readback of the composed desktop with GDI after each flip found the frame's
own colors and the patch's own state on 40 of 40 frames, so the pbuffer
follows the rotation. (GDI read the levels scaled by about 0.55 by the
desktop's color path; the check compares states, not levels.)

The adapter matters on this laptop: the NVIDIA part has no output, so a
device on it would mean a copy between adapters and a composed path. The
header picks the adapter whose output holds the window's monitor.

DXGI's vblank time, `SyncQPCTime`, is a QueryPerformanceCounter value, the
counter `yrt_now_ns()` reads. `yrt_ticks_to_ns()` converts it with the
same arithmetic, so a flip's onset is on the ysp_rt clock with no
correlation.

The Windows 11 composition swapchain, the one native present-at-a-time
path, reports `IsPresentationSupported() = 1` and
`IsPresentationSupportedWithIndependentFlip() = 1` on this GPU. v0.2.0
adds it as `YSCR_BACKEND_COMPOSITION`; see
[The composition swapchain](#the-composition-swapchain-v020).

## flip_at does not wait for the flip

A flip's onset is known only after its vblank. Psychtoolbox's Flip blocks
until then, which puts the CPU work of the next frame after the scanout of
this one. `yscr_flip_at()` returns after the present call, and the record
completes in a later `yscr_begin()`, with `f.last` and the ring.
`yscr_wait_flip()` is the blocking form.

## The snap is the timeline's

`yscr_flip_at(t)` snaps `t` to the first vblank at or after
`t - lead x period`, with lead 0.5 by default: the nearest vblank. The frame
loop passes the predicted onset as `t`, and a predicted onset carries
noise. Under "the first vblank at or after `t`", a `t` one microsecond
after a vblank goes a frame late; [timeline.md](timeline.md)
measured 12% of frame-aligned targets a frame late at 2 us of noise for the
same rule, which is why the timeline dropped it. With one rule in both
headers, a flip and a timeline event at one time land on one frame.

## Depth

A present shows `depth` vblanks after the vblank before the call. Measured:
1 on the overlay path (fullscreen), 2 on the composed path (a window). The
header learns it from the flips. The first version learned it only from
presents that went out at once, and a window that had just gone fullscreen
was composed for its first frames, so open() learned 2 and the first held
frames on the overlay path showed a vblank early (2 to 5 of the first
frames of a run). Two changes fixed it: every present without a target
counts (DXGI shows a frame at the first vblank it can, held or not), and
open() presents black frames until three flips in a row agree, up to 40.
After the change: 0 early flips in 1200 held fullscreen frames. In
October 2026 every version from v0.2.0 to v0.4.2 showed held frames early
again on this machine, for another cause; see
[Held frames a vblank early](#held-frames-a-vblank-early-v043).
Since v0.4.4 the depth after open comes from the path, never from
misses, and the learning described below is opt-in; see
[Depth from the path](#depth-from-the-path-v044).

## Holding a frame for a later vblank

Two ways were built and measured, the same frame loop asking for a vblank
0 to 4 frames ahead with the target up to 0.45 frame off the grid:

| Method | Path | Held frames checked | On the vblank asked | Early | Late | Header CPU in flip_at, p50 |
|---|---|---|---|---|---|---|
| Sleep on ysp_rt until just after vblank (planned - depth), then `Present(1)` | composed (window) | 2998 | 2998 | 0 | 0 | 3.3 us |
| same | overlay (fullscreen) | 1200 | 1200 | 0 | 0 | 2.8 us |
| `Present(SyncInterval = planned - last shown)` at once | composed | 2906 | 2711 | 143 | 52 | 2.7 us |
| same | overlay | 1800 | 605 | 795 | 400 | 1.8 us |

DXGI's sync interval holds the frame being presented, not the one on the
screen, so it cannot move a frame later. The sleep is the one kept; the
other was deleted. (The sleep stopped being enough on this machine by
October 2026, because the grid it wakes on followed DXGI times that were
off the vblanks; fixed in v0.4.3, see
[Held frames a vblank early](#held-frames-a-vblank-early-v043).)

## DXGI reports only the newest flip

`GetFrameStatistics` returns the statistics of the newest flip only, and
on the composed path it lags the vblank by about 1.8 ms. A flip that is
followed by another before the next read is never reported. In a hold run
in a window this left 358 of 3000 records without a statistic (12%). The
header now reads the statistics before each present when a flip is due,
and once per vblank, 2.5 ms after it, while it waits to hold a frame. That
brought the hold run to 2 of 3000. A frame the caller made late by 20 ms
or more still hides the flip before it on the composed path (23 of 900
records in those runs); on the overlay path none was lost. Such a record
is flagged `YSCR_FLIP_ESTIMATED` and gets its planned vblank as onset.

## The patch is a ClearView

The first patch was a scissored `glClear` through ANGLE, with the GL state
it touches read and restored. Measured: 6.3 us p50 for the reads and
8.5 us for the clear, 15.2 us per frame. On DXGI the patch is now
`ID3D11DeviceContext1::ClearView` with one rectangle on the back buffer,
after ANGLE's flush, inside the present: 0.1 us of header time, no GL
state touched. The GL path stays for presenters that do not paint the
patch themselves.

## Input timestamps

`SDL_GetTicksNS()` against `yrt_now_ns()`: the tightest of 16 reads has a
width of 0.1 us (the QPC tick) in 1000 of 1000 calls, 0.9 us per call. SDL3
counts in QPC on Windows, so the restamp is exact to 0.1 us.

The event's own timestamp is not exact. SendInput reaches only the
foreground window, and Windows does not give the foreground to a program
started from the background. It does after the program sends one input
event, so `examples/screen/input.c` first sends a zero-size mouse move
(the pointer does not move) and calls `SetForegroundWindow()`. Then it
sends F24 with SendInput at known ysp_rt times, first through SDL's
message loop and then on SDL's raw keyboard path:

| Path | Keys | Restamped minus sent, ms: mean | p50 | p99 | min | max |
|---|---|---|---|---|---|---|
| raw input | 199 of 200 | +0.39 | +0.37 | +0.65 | +0.18 | +0.67 |
| raw input, `timeBeginPeriod(1)` | 199 of 200 | +0.50 | +0.43 | +1.21 | +0.21 | +2.86 |
| message loop | 199 of 200 | +2.77 | +2.97 | +10.99 | -6.16 | +12.05 |
| message loop, `timeBeginPeriod(1)` | 199 of 200 | +2.42 | +2.24 | +11.13 | -6.91 | +11.14 |
| posted `WM_KEYDOWN` (v0.1) | 199 of 200 | -6.98 | | | -14.84 | +0.49 |

SDL takes a message event's time from the message time, which counts in
the system tick (about 15.6 ms), so a key can be stamped before it was
sent. On the raw path SDL reads Raw Input on its own thread and stamps
each key as it reads it; its sub-millisecond parts spread over the whole
millisecond, so the stamp is not tick-bound. So the header turns on the
raw keyboard path (`SDL_HINT_WINDOWS_RAW_KEYBOARD`, at default priority,
so the caller's own setting wins) and stops SDL text input. The times
above leave out the keyboard itself. Use `ysp/serial.h` response boxes
for reaction times.

### Text input (v0.3.2)

A text box (a typed response, the designer's editor in the player) needs
SDL text input: `SDL_EVENT_TEXT_INPUT` for committed text and
`SDL_EVENT_TEXT_EDITING` for an input method's composition. Open stops it,
because SDL routes keys through the message path while it runs, and keys
on that path carry message times (the table above). `yscr_text_input()`
turns it on and off for one screen and sets the IME area. The header does
not hide the cost; it records it:

- each change of on or off is a `YSCR_EV_TEXT_INPUT` ring record (the
  call's time, on or off, the rectangle);
- each flip planned while it is on has `YSCR_FLIP_TEXT_INPUT` in its
  record, so an analysis can drop or mark the key times of those frames.

A caret move calls it again with the new rectangle and writes no record:
the log needs the intervals, not every keystroke. The flag is not in the
ring's mode word, which holds 10 flag bits; the ring records give the same
intervals. Default: off at open, as before v0.3.2.

Checked: the scripted-path test (records, the flag on exactly the flips
planned while on, no record for a moved rectangle or a repeated off, the
argument and closed-screen errors) on MSVC and MinGW gcc 16.1, and two
mutants (`text-00`, `text-01` in `tests/mutate/screen.toml`), both caught.
The SDL calls ran in `examples/layout/gfx_layout.c`'s editor page. Not measured:
how much later keys are stamped with text input on than on the raw path
beyond the message-loop rows above.

## Measurements

`examples/screen/flipstats.c` made every table here, built with MSVC
19.4 /O2 in Release. It defines `YRT_TRACE_RING`, so the header's zones
give the cost breakdown. The runs before the last three changes to the
header (DXGI times through `yrt_ticks_to_ns()` instead of a correlation
with a 0.1 us width, no second event pump after `yscr_poll()`, and the
GPU timer query taken out) were checked with one more fullscreen run of
the final build (last row of the first table).

Prediction: the onset DXGI reported minus the onset `yscr_begin()`
predicted, for frames the caller did not make late. Bar: 50 us at p99.

| Run | Path | Frames | p50 us | p99 us | max us | Dropped | Late target | Estimated |
|---|---|---|---|---|---|---|---|---|
| fullscreen, idle, patch on, 2 min | overlay | 7200 | 0.46 | 2.33 | 3.69 | 0 | 0 | 0 |
| fullscreen, 8 threads spinning, 1 min | overlay | 3600 | 0.68 | 2.04 | 6.08 | 0 | 0 | 0 |
| window 800 x 600, idle, 10 min | composed | 36000 | 1.30 | 5.72 | 22.52 | 0 | 0 | 1 |
| fullscreen, final build, 30 s | overlay | 1800 | 0.32 | 2.56 | 6.58 | 0 | 1 | 0 |

The one late target in the last run was the frame loop's own: a
`SDL_PumpEvents()` call took 2.7 ms on that frame.

Holds (see above): every held frame on the vblank asked for, 1200 of 1200
fullscreen and 2998 of 2998 in a window.

Overruns: a busy wait of the given length before the flip in a random 1 of
30 frames. Bar: every overrun past the deadline flagged on its own frame,
with the right phase charged; no false flag.

| Overrun | Path | Frames | Injected | Flagged late target | Draw phase charged | Missed without a flag | Other frames flagged |
|---|---|---|---|---|---|---|---|
| 16 ms | composed | 900 | 24 | 24 | 24 | 0 | 0 |
| 16.5 ms | composed | 900 | 24 | 24 | 24 | 0 | 0 |
| 17 ms | composed | 900 | 24 | 24 | 24 | 0 | 0 |
| 20 ms | composed | 900 | 24 | 24 | 24 | 0 | 0 |
| 40 ms | composed | 900 | 24 | 24 | 24 | 0 | 0 |
| 20 ms | overlay | 900 | 24 | 24 | 24 | 0 | 0 |

A first 20 ms run in a window had one dropped flip and one more late
target on frames with no overrun; the rerun had none. Those were the
machine's, and the record named them.

The header's CPU time per frame, fullscreen, idle, 7200 frames. "begin" is
`yscr_begin()` minus its wait for the swap path; "flip" is
`yscr_flip_at()` minus the present call. Bar: 20 us mean and 50 us p99
for the two together.

| Part | mean us | p50 us | p99 us |
|---|---|---|---|
| begin, all | 22.2 | 20.3 | 60.4 |
| of which `SDL_PumpEvents()` | 7.8 | 7.0 | 24.7 |
| of which the statistics read and record completion | 11.8 | 10.7 | 39.8 |
| flip, all | 1.8 | 1.8 | 4.1 |
| of which the patch | 0.1 | 0.1 | 0.2 |
| the present call itself (DXGI and the driver, not counted) | 341.5 | 325.3 | 705.7 |

The bar is missed: 24 us mean and about 64 us p99. The header's own work
is about 4.5 us; the rest is the two OS calls. A repeated
`GetFrameStatisticsMedia()` costs 0.5 us, so the 11 us is the first read
after a flip. A frame that reads its events with `yscr_poll()` skips
the second pump.

GPU time: a `GL_EXT_disjoint_timer_query` around each frame tracked the
GPU work (54 us for a clear, 3067 us p50 with 50 full-window clears) but
added about 140 us to begin and 115 us to flip at p50 through ANGLE, against
a bar of 5 us. It was taken out; `_GPU` is always unknown in v0.1.

Allocations: no C runtime heap call in 570 frames of an MSVC debug build
(`_CrtSetAllocHook`); with one `malloc` per frame added as a control, the
hook counted 90 of 90.

Two windows as a group, 3 minutes: 10800 flips on each, one late target
on each (the same moment), one drop and three estimated records on the
second. Both windows are on one panel, so their onsets were equal on
10799 of 10800 frames.

Builds, with warnings as errors: MSVC 19.4 (C11 and C++17, Release, and a
debug build), MinGW-w64 gcc 14.2 (C11, C99 and C++17), gcc 11.4 on WSL2
(C11, C99 -O3, C++17, ASan and UBSan), clang through emcc 6.0.10 (C11 run
in node, C++17 compile only). `ctest` passed 32 of 32 in the MSVC build.
The core test caught each of eight deliberate mutations of the header:
the snap without its lead, depth taken on one vote, drops not counted,
estimated records not flagged, the prediction one vblank late, the hold
waking a vblank early, late targets not flagged, a path change not
adopted at once.

## The composition swapchain (v0.2.0)

`YSCR_BACKEND_COMPOSITION` presents through the Windows 11
presentation manager (`IPresentationManager`) and a DirectComposition
visual. Each present carries a target time. The measurements in this
section come from two programs:

- a scratch probe in C++ with the SDK headers, which logs every present
  and every statistic;
- `examples/screen/flipstats.c` on the header's own C implementation.

All runs used the machine described at the top of this note. Most of
them ran in a 640 x 480 or 800 x 600 window kept on top, because a
window gets independent flip here too.

### Prior art

I read two projects before chasing the anomalies:

- [xzn/ntrviewer-hr](https://github.com/xzn/ntrviewer-hr),
  `ui_compositor_csc.c`. It is MIT-licensed, so I learned from it and
  copied no code. The headers are MIT-0, and MIT asks for its notice in
  every copy.
- FoggyBytes/StreamLight, `d3d11composition.cpp`. It is GPL-3.0, so I
  only used the research agent's notes on it.

Four of their practices agree with what we found and the header does the
same:

- set the surface's source rectangle;
- wait for a buffer through its available event;
- call `SetTargetTime` before every present (the target persists across
  presents);
- read a displayed time only from an independent-flip statistic.

ntrviewer-hr also declares `GetDisplayedTime` with an output pointer, as
our header does. Neither project checked a target time against the
hardware.

### The anomalies of the phase 1 probe

The first probe found three anomalies. Each one now has a measured
cause.

| Anomaly | Cause | Evidence |
|---|---|---|
| 217 of 630 presents CANCELED | Two queued presents whose targets were the same vblank. The older one is canceled on the independent-flip path and SKIPPED on the composed path. In the first probe, a counter that counted a present twice let two presents be in flight | Pairs of presents aimed at one vblank, 150 pairs: the older present was canceled in 147 and skipped in 3 (independent path), and skipped in 150 of 150 (composed path). The newer present was shown in all 300 pairs |
| 215 of 630 presents with no statistic in one run, 0 in the next | The window was covered: a program started from the background does not get the foreground, and a covered window gets PresentStatus Queued but no display statistic | 300 asap presents with one in flight: 266 had only a Queued status when the window was behind others (34 shown), and 300 of 300 were shown with the window on top |
| Half the presents SKIPPED with target 0 and 3 buffers | With no throttle, the loop presented every 13.9 ms, so some vblanks had two presents ready, and the system skips all but the latest | 136 of 300 presents skipped |

So the header keeps one present in flight. It also completes a present
that gets no statistic within 3 periods as ESTIMATED and OCCLUDED, so a
covered window cannot stall the frame loop.

### Clock

The SDK calls the target time and the displayed time `SystemInterruptTime`.
On this machine both are QPC time in 100 ns units, not
`QueryInterruptTimePrecise()`. Interrupt time (biased or unbiased) was a
constant 20.03 ms behind QPC, and QPC runs at 10 MHz here.

- Targets passed as interrupt time put every frame 1.2 periods early.
  That is the 20 ms difference, and it is what made the phase 1 table
  inconsistent.
- Displayed times read as interrupt time sat 3.4 ms off the vblank. Read
  as QPC, they sit on it.

The header therefore passes `yrt_now_ns()` time divided by 100. On a
machine whose QPC does not run at 10 MHz this is unverified; the manual
says so in STATUS.

### The independent vblank source

D3DKMTWaitForVerticalBlankEvent returned early now and then, which put
extra wakes in the record. The probe instead polls `D3DKMTGetScanLine` on
a time-critical thread and stamps QPC when the scanout enters vertical
blank. Two numbers show how steady that stamp is:

- the median wake comes 0.5 to 2 us after the 10th-percentile wake;
- 4910 of 4910 stamps sat on a 16666.425 us grid.

### Quantization

Each cell targets vblank V + frac x period, with V at least `lead`
periods ahead, one present in flight, and 20 presents per cell. The
table gives where the frame was shown.

| Path, lead | frac -0.50 | -0.45 to +0.45 | +0.50 |
|---|---|---|---|
| independent, lead 0 | V 19/19 | V, 380 of 380 | V 18/20 |
| independent, leads 1, 2, 3, 5 | V-1 in 70 of 78 | V, 1520 of 1520 | V in 72 of 80 |
| composed, lead 0 | V+1 20/20 | V+1, 380 of 380 | V+1 20/20 |
| composed, leads 1, 2, 3, 5 | V-1 in 52 of 60 (lead 1: V) | V, 1520 of 1520 | V in 72 of 80 |

So the system shows a frame on the vblank nearest its target, and a
tie goes to the earlier vblank. That is ysp/timeline.h's lead-0.5 rule.
The quarter-period offset proposed in the design is therefore not used:
the header passes the planned vblank's own time.

The shortest lead that still met the target:

- on independent flip, the next vblank;
- on the composed path, the vblank after next.

### Queue depth

With 1, 2, 4 and 7 presents in flight, each aimed at the next free
vblank, 120 of 120 frames were shown on their target at every depth.
The queue accepts presents several frames ahead. The header keeps one in
flight anyway, and no `queue_ahead` mode was added: one in flight
dropped nothing in these runs, so queueing showed no benefit to measure.

### Is the onset an observation? (condition C)

| Path | Reported time minus the scanline vblank | Statistic arrival minus reported time | Late present (0.3 period after its target vblank) |
|---|---|---|---|
| independent flip: DisplayedTime | p50 +12.7 us, p99 +76.7 us (sweep, 2097 frames) | p50 +270 us, p99 +577 us | shown 1 or 2 vblanks late, 60 of 60, and the reported time moved with it |
| composed: DComp presentedStats.time | p50 +25.8 us, p99 +92.6 us (2100 frames) | p50 -15.3 ms, p99 -13.9 ms | shown 1 to 2 vblanks late, 60 of 60, and the reported time moved with it |

- **Independent flip.** The displayed time sits on the scanout's own
  vblank. It arrives a quarter of a millisecond after it and follows a
  late frame. It is an observation: tier 1.
- **Composed path.** The time also sits on a hardware vblank and follows
  a late frame. But it is reported about 15 ms before that vblank comes,
  so it is DWM's plan for the vblank it composed the frame for. It is not
  an observation of the scanout.
- **DWM load.** A second window cleared a 4096 x 4096 float target 8 and
  then 40 times per frame. DWM's own counters (`cFramesMissed`,
  `cFramesDropped`, `cFramesLate`) stayed at 0, so no DWM miss could be
  produced to see whether the record shows one.
- **Result.** A composed onset on COMPOSITION is DWM's claim. It is
  recorded with YSCR_FLIP_ONSET_PLANNED, tier 3.

### A finding: the panel stretches frames

On the independent-flip path, a frame presented late was followed by
frames whose PresentDuration was 20.0, 19.4, 18.8 and so on, down to 16.67
ms over about ten frames. Over those frames the displayed time moved up
to 4.9 ms off the vblank grid, while the scanline's vblank entries stayed
on it. The panel held the vblank longer, as variable refresh does, which
the header does not enable. The record flags these flips GRID_UNSTABLE,
and their tier is 2.

Correction (v0.4.3, 2026-10-09): the scanline stayed on the grid here,
so what moved was the reported time, not a vblank of the scanout. The
probe of [Held frames a vblank early](#held-frames-a-vblank-early-v043)
found the same on DXGI_FLIP: times up to half a period off, the
scanout's vblanks steady, each frame flipped on the vblank nearest its
time. Whether the panel's light moves is not known without a photodiode.

### Path reports on DXGI_FLIP

`IDXGISwapChainMedia::GetFrameStatisticsMedia` is documented for media
swapchains, but the header calls it on an ordinary flip swapchain. Two
observations support its answer there:

- The path it reports changes when the conditions change. A window in
  the background was composed, and the same window taken to the
  foreground went to an overlay plane.
- The depth the header learns independently agrees with it: 2 vblanks
  on composed, 1 on overlay in fullscreen.

A small window put over ours did not force composition: the system kept
our frames on an overlay plane and composed only the other window. So
the forced-composed runs of COMPOSITION use the probe's own controls
instead, a premultiplied-alpha or rotated surface, which DWM composes.

### Depth on a backend that holds frames

This is the rule of v0.2.0 to v0.4.3; since v0.4.4 it runs only with
`desc.depth_learn` (see [Depth from the path](#depth-from-the-path-v044)).

The first COMPOSITION batch showed a fault in the core. A present with a
target waits for that target, so a flip that is on time shows only that
the depth it was given was enough. After one miss raised the depth to 2,
every flip "measured" 2, and with one frame in flight the loop ran at
half rate: 21.0 ms mean frame time in a window, and 27.4 ms after
overruns, where 16.7 ms was possible. DXGI_FLIP has no such fault,
because a DXGI present shows at the first vblank it can.

**First fix, replaced: a try at one less.** After 60 on-time flips the
header lowered the depth by one; a try that missed put it back and
doubled the wait, up to 3840 flips. A try that fails drops a frame on
purpose. In the two COMPOSITION runs with a CSV (window kept on top,
2 minutes and 10 minutes), every try worked: 7 tries, 0 failed, so 0 of
the 54 drops were tries. But a display that really needs depth 2 would
lose a frame at every try (the core test showed 2 in 280 flips), and the
slow phases before each try cost 123 and 468 vblanks with no new frame.
The 10-minute run also opened at depth 4: open() learned the depth from
frames whose path changed back and forth between composed and
independent flip.

**The rule now: lower only on evidence.** For each path the header
counts the slack of every flip, on time and missed, in bins of a 32nd of
a period. The slack is the planned vblank's time minus the return of the
present call. A present at one depth less would have had one period less
slack. The header lowers the depth when, on 4 flips in a row, that
slack has been seen on time at least 8 times as often as a miss was
seen with as much slack or more. Three misses in a row raise the depth
by one. So a short run of system misses (the case that caused the half
rate) is undone in about 4 flips, from the evidence of the flips before
it, and no frame is put at risk to find out.

The evidence can be wrong in one case: the system's deadline grew after
the evidence was taken, at the same time as the misses that raised the
depth. Then the first flip at the lower depth misses. A miss within 4
flips of a lowering clears that path's on-time counts, so the same
evidence cannot cost a second frame, and the depth stays up until new
on-time evidence at that slack comes in. Counts halve every 4096 flips.
Each path keeps its last depth, so a window that moves between composed
and independent flip does not learn it again. On these backends open()
does not learn the depth: it starts at 1.

The core test runs three cases on a scripted display that holds frames
to their target, on a virtual clock, with a seeded random wait of up to
a quarter period before each flip:

| Case | Drops after the 3 misses | Frames that waited 2 vblanks |
|---|---|---|
| 3 system misses at depth 1, then the display is as before | 0 | 7 or 14 |
| the display needs depth 2 from the misses on, buffer free at the flip | 1 (the miss that cleared the evidence) | all, as one frame in flight allows |
| the same, buffer free a vblank early (as on DXGI composed) | 1 | 0 |

On the panel, `screen_flipstats --miss 300` gives 3 frames in a row
about 25 ms of GPU work every 300 frames: they miss their vblank with
the present on time, which is the system's lateness that raised the
depth. COMPOSITION, window kept on top, 7200 frames each, on battery:

| Rule | Heavy frames dropped | Other frames dropped | Other frames 2 or more vblanks after the one before |
|---|---|---|---|
| try at one less (the first fix) | 70 of 72 | 1 | 1355 |
| evidence (now) | 72 of 72 | 1 | 69 |

The heavy frames drop by design. With the evidence rule each of the 24
episodes cost about 3 slow frames, not 60, and no frame outside them
dropped because of the rule.


### COMPOSITION against DXGI_FLIP

All runs below are after the depth fix unless the row says otherwise.
Prediction is the reported onset minus the predicted onset, for flips the
caller did not make late. In a window, "kept on top" means
`HWND_TOPMOST` and the foreground.

| Run | Backend | Frames | Paths | p50 us | p99 us | max us | Dropped | Late target | Early | No statistic |
|---|---|---|---|---|---|---|---|---|---|---|
| window on top, idle, 10 min | COMPOSITION | 36000 | independent 36000 | 0.53 | 2.27 | 8.07 | 12 | 0 | 0 | 0 |
| window on top, patch, 2 min | COMPOSITION | 7200 | independent 7200 | 0.43 | 2.57 | 19.37 | 42 | 1 | 0 | 4 |
| window on top, patch, 1 min, pair 1 | DXGI_FLIP | 3600 | composed 1943, overlay 1560 | (1) | (1) | (1) | 67 | 10 | 15 | 96 |
| same, pair 1 | COMPOSITION | 3600 | independent 3600 | 0.33 | 2.12 | 10.68 | 24 | 0 | 0 | 15 |
| same, pair 2 | DXGI_FLIP | 3600 | composed 1824, overlay 1743 | 0.60 | 5.35 | 16668 | 38 | 9 | 14 | 33 |
| same, pair 2 | COMPOSITION | 3600 | independent 3600 | 0.61 | 2.49 | 4.95 | 5 | 0 | 0 | 0 |
| same, pair 3 | DXGI_FLIP | 3600 | composed 552, overlay 3035 | 0.66 | 4.50 | 16667 | 27 | 1 | 9 | 13 |
| same, pair 3 | COMPOSITION | 3600 | independent 3600 | 0.64 | 2.20 | 4.48 | 0 | 0 | 0 | 0 |
| fullscreen, patch, 2 min (2) | COMPOSITION | 7200 | independent 7200 | 0.54 | 82.07 | 135.88 | 29 | 0 | 0 | 5 |
| fullscreen, patch, 2 min (2) | DXGI_FLIP | 7200 | overlay 7200 | 0.65 | 75.31 | 16668 | 21 | 6 | 4 | 0 |
| fullscreen, patch, 1 min | COMPOSITION | 3600 | independent 3600 | 1.73 | 110.19 | 260.50 | 16 | 0 | 0 | 8 |

(1) A swap-path timeout in this run put the example's prediction table
one frame out of step after it; the example now keeps them in step. The
counts are right.
(2) Before the depth fix, while another program was building on the
machine. Both runs ended at depth 1.

The windowed DXGI_FLIP runs moved between the composed and the overlay
path every 5 to 11 s. Each change to the overlay path showed a few flips
a vblank early, at the depth learned on the composed path, and each is
flagged EARLY. The v0.1 runs in a window not kept on top stayed composed.
Why the path moved was not found. Twice in these runs DXGI's waitable
object freed no slot for 333 ms, and `yscr_begin()` returned
`YSCR_ERR_TIMEOUT`; `screen_flipstats` now counts that and goes on.

The fullscreen rows were 30 to 50 times worse at p99 than the v0.1
fullscreen runs (2.3 us). They ran while another agent ran load on the
laptop; on the quiet machine both backends are back at 2.3 us (see
[The fullscreen regression](#the-fullscreen-regression)).

Holds and overruns on COMPOSITION:

| Run | Frames | Result |
|---|---|---|
| hold 0 to 4 vblanks, window on top | 3000 | 2979 of 2998 on the vblank asked for, 19 late, 0 early |
| hold 0 to 4 vblanks, fullscreen, 30 s (2) | 1800 | 1800 of 1800 on the vblank asked for |
| overrun 20 ms in 1 of 30, window on top | 1800 | 60 of 60 flagged LATE_TARGET, draw phase charged on 60, 0 missed without a flag; 14 other flips dropped or late |
| overrun 17 ms in 1 of 30, window on top (2) | 900 | 24 of 24 flagged, 0 missed without a flag |

The header's CPU time per frame on COMPOSITION, window on top, 10 min:
`yscr_begin()` without its wait 21.8 us mean and 75.1 us p99, of which
`SDL_PumpEvents()` 11.0 us and the statistics 7.9 us; `yscr_flip_at()`
without the present call 1.8 us. That misses the 20 us bar, as on
DXGI_FLIP, on the two OS calls. The present call (flush, patch,
`SetBuffer`, `SetTargetTime`, `Present`) took 530 to 1230 us mean in the
alternating runs, against 380 to 730 us for DXGI_FLIP's flush, patch and
`Present`.

Allocations, MSVC debug build, window on top: the C runtime heap hook
counted 2 calls in the frame loop, in runs of 270 and of 570 frames, so
they do not repeat per frame. Both came from the debug runtime making its
per-thread data while an OS component on the frame thread raised and
caught a C++ exception (the stacks end in `RtlRaiseException` and
`_CxxFrameHandler3`). DXGI_FLIP had 0 in 570 frames. The control run
with one `malloc` per frame counted 572.

With one frame in flight the next frame waits for the statistic of the
last one, which comes about 0.3 ms after its vblank on independent flip.
In the 10-minute window run the loop used about 99% of the vblanks
(16.85 ms mean frame time).

### Latency

Latency here is from the return of the present call to the onset the
system reports: the start of scanout of the frame's vblank, NOT light.
The panel's own delay to light is not measured (no photodiode). "First
vblank" is the share of frames shown on the first vblank after the
present call returned. `screen_flipstats` takes the return time just
after `yscr_flip_at()` returns, a few us later than the call itself.
Fullscreen runs: 1 minute each, idle, patch on, Balanced power plan.

| Power | Backend | Path | Frames | First vblank | p50 ms | p99 ms | max ms |
|---|---|---|---|---|---|---|---|
| AC | DXGI_FLIP (v0.1 control) | overlay | 3600 | 100% | 15.93 | 16.36 | 16.45 |
| AC | DXGI_FLIP | overlay | 3600 | 100% | 15.96 | 16.38 | 16.53 |
| AC | COMPOSITION | independent | 3600 | 100% | 15.63 | 16.19 | 16.39 |
| battery | DXGI_FLIP (v0.1 control) | overlay | 3600 | 100% | 16.01 | 16.33 | 16.44 |
| battery, window on top | DXGI_FLIP | overlay | 7192 | 100% | 16.02 | 16.32 | 16.45 |
| battery, window on top | DXGI_FLIP | composed | 5 | 3 of 5 | 15.68 | 45.39 | 45.39 |
| battery, window on top | COMPOSITION | independent | 7198 | 99.99% | 15.54 | 16.01 | 27.33 |
| battery, window on top | COMPOSITION | overlay | 2 | 0 of 2 | 26.85 | 26.85 | 26.85 |

So on the overlay and independent-flip paths both backends show a frame
on the next vblank after the present, and the time to it is set by when
in the frame the present comes, not by the backend. COMPOSITION's p50 is
0.3 to 0.5 ms lower only because its present call returns later (it
costs more). The composed path costs a vblank more: DXGI_FLIP composed
fullscreen (a window over ours, battery run below) took 32.61 ms p50,
0 of 251 frames on the first vblank. The windowed runs before the
latency columns existed (`r1`, `r4`, the alternating pairs) did not
record the return time, so they give no latency.

The two v0.2 fullscreen runs on battery did not hold the foreground:
DXGI_FLIP ran on the composed path with 3349 of 3600 flips without a
statistic, and COMPOSITION had no statistic on any flip (OCCLUDED). The
v0.1 control just before them was on the overlay path. What covered the
window was not found. The fullscreen limit for that power state was used
up, so the AC set takes the foreground (`--topmost`) for all three.

### The fullscreen regression

The fullscreen p99 of 75 to 110 us in the first v0.2 batch was the
machine's load, not the header. The same binaries on the quiet machine:

| Power | Build | Backend | Path | Frames | p50 us | p99 us | max us | Dropped |
|---|---|---|---|---|---|---|---|---|
| AC | v0.1 header, older ysp/rt.h | DXGI_FLIP | overlay | 3600 | 0.39 | 2.32 | 3.65 | 0 |
| AC | v0.2 | DXGI_FLIP | overlay | 3600 | 0.48 | 2.33 | 4.15 | 0 |
| AC | v0.2 | COMPOSITION | independent | 3600 | 0.45 | 2.36 | 8.36 | 0 |
| battery | v0.1 header, older ysp/rt.h | DXGI_FLIP | overlay | 3600 | 0.51 | 3.13 | 12.85 | 0 |
| not recorded | v0.2 (probe rule) | DXGI_FLIP | overlay | 3600 | 0.63 | 3.55 | 25.85 | 0 |
| not recorded | v0.2 (probe rule) | COMPOSITION | independent | 3600 | 0.56 | 2.29 | 4.09 | 0 |

The v0.1 control is today's `screen_flipstats.c` with the tier lines
taken out, built on the v0.1 header and the ysp/rt.h from before the
P-core steering and the EcoQoS and timer opt-outs. On AC all three agree
to 0.04 us at p99, so neither the v0.2 depth changes nor the ysp/rt.h
changes moved the prediction. The first batch ran while another agent
ran spinning load threads and throttle probes on this laptop. Both the
vblank time and the prediction come from the OS's vblank times, so load
can move the error only by delaying the vblank timestamp itself
(interrupt and DPC latency under load); that was not measured
separately. The "not recorded" rows ran before the power state was
logged, on the quiet machine.

### AUTO

Since v0.4.4 AUTO picks COMPOSITION where it opens with independent
flip; see [AUTO, decided](#auto-decided-v044). Before it, AUTO stayed
DXGI_FLIP, for these reasons. COMPOSITION was better in a window on every
measure but the cost of the present call: fewer drops, no early flips,
fewer flips without a statistic, a lower p99. In fullscreen on the
quiet machine it did as well, not better (2.36 against 2.33 us p99, no
drop on either, the same latency to the next vblank), its present call
costs more everywhere, it needs
Windows 11, and its composed onset is DWM's plan (tier 3) where
DXGI_FLIP's composed onset is tier 2. A task that runs in a window on
Windows 11 can ask for COMPOSITION by name.

v0.4.3 measured both in fullscreen with load, misses and holds:
COMPOSITION dropped fewer heavy frames, showed none early and had no
off-grid time. The proposal in
[AUTO, decided](#auto-decided-v044) applies it, after the 10-minute runs.

### Tiers

| Tier | Backend and path | Why, from the measurements |
|---|---|---|
| 1 | DXGI_FLIP overlay or independent flip | DXGI's vblank time on a flip the OS made; v0.1 prediction p99 2.3 us |
| 1 | COMPOSITION independent flip | DisplayedTime 13 us after the scanline vblank at p50, 77 us at p99, and it moves with a late frame |
| 2 | DXGI_FLIP composed | DWM's vblank time; not checked against another source |
| 2 | any flip whose OS time is off the vblank grid (GRID_UNSTABLE); its onset is the grid's vblank | DXGI's times of held frames up to half a period off the scanout's vblanks, which stayed on the grid (v0.4.3) |
| 3 | COMPOSITION composed or overlay frame (ONSET_PLANNED) | DWM reports the time about 15 ms before the vblank |
| 3 | any ESTIMATED flip | no OS time at all: the planned vblank |
| SIM | the simulated display | no display |

Where the scheme does not fit: tier 1 on COMPOSITION rests on 2097
frames against the scanline on one machine, and its p99 of 77 us is
larger than DXGI_FLIP's prediction error, so "tier 1" does not mean the
same error bound on both backends. `desc.min_tier` flags flips; it does
not stop a run, because a path can change in the middle of a trial and
only the caller knows what to do with that trial.


## Flip hooks (v0.3.0)

The design note (`screen_hooks_design.md`) was not kept; this section is
the record of what was built and measured. All runs: one laptop, on
battery, Balanced power plan, `screen_flipstats` built with MSVC 19.4
/O2, 2026-10-05, under the shared measurement lock.

### What VPixx devices read from the frame

| Code | Pixels | Where | Bits | Source |
|---|---|---|---|---|
| Pixel Mode | 1 | the top-left pixel of each frame | digital out 0 to 7 from red, 8 to 15 green, 16 to 23 blue; modes RGB, GB, B | [VPixx, Sending Triggers with Pixel Mode](https://docs.vpixx.com/vocal/sending-triggers-with-pixel-mode), [Datapixx('EnablePixelMode')](http://psychtoolbox.org/docs/Datapixx-EnablePixelMode) |
| Pixel sync | "at least 8" recommended, caller-defined RGB | a raster line set with `DPxSetVidPsyncRasterLine`; Psychtoolbox draws it on scanline 0 from x = 10 and blanks that line | all 24 bits of each pixel compared | [VPixx, Example: sending a digital trigger on Pixel Sync](https://docs.vpixx.com/vocal/example-sending-a-digital-trigger-on-pixel-sync), [libdpx](https://www.vpixx.com/manuals/libdpx/html/libdpx.html), [PsychDataPixx.m](https://raw.githubusercontent.com/Psychtoolbox-3/Psychtoolbox-3/beta/Psychtoolbox/PsychHardware/DatapixxToolbox/DatapixxBasic/PsychDataPixx.m) |

VPixx also documents a "stereo blue line" for the VIEWPixx /3D
([Setting up a 3D display](https://docs.vpixx.com/matlab/setting-up-a-3d-display));
its pixels were not found in the pages read, so there is no preset. The
pages read do not say in which DATAPixx video modes Pixel Mode works or
whether pixel sync searches a whole line. Both presets
(`yscr_slot_pixel_mode`, `yscr_slot_psync`) follow the sources above.

### Codes

| Run | Backend, path | Frames | Self test | Read back | Differed | Codes zone, mean / p50 / p99 us | Code risk |
|---|---|---|---|---|---|---|---|
| ROW by ClearView per pixel, fullscreen, 1 min | DXGI_FLIP, overlay | 3600 | pass | 720 | 0 | 3.82 / 3.10 / 18.3 | 0x0002 (advanced color) |
| same | COMPOSITION, independent | 3600 | pass | 720 | 0 | 4.24 / 3.20 / 14.8 | 0x0002 |
| ROW by UpdateSubresource, fullscreen, 30 s | DXGI_FLIP, overlay | 1800 | pass | 360 | 0 | 5.31 / 3.90 / 22.5 | 0x0002 |

Each frame drew a 1-pixel Pixel Mode code and an 8-pixel pixel-sync row,
and read them back every 10 flips. The zone is the CPU time of the D3D11
calls, including the staging copy on read-back frames. One ClearView per
pixel was cheaper for 8 pixels, so rows of up to 8 pixels use it and
longer ones use UpdateSubresource; the longer case was not timed.

The read-back and the self test see the back buffer only. The describe
line on this laptop says `color=wcg`: Windows runs auto color management
on the internal panel, so DWM and the display kernel convert colors, and
every code here carries `CODE_RISK_ADVANCED_COLOR`. No device has read a
code from this laptop.

### Triggers

One trigger per frame on a TIME_CRITICAL worker; the callback did
nothing. "Moved" counts triggers armed or re-armed for another vblank;
"GPU moved" those moved by the fence check, with how many of those
frames were in fact late.

| Run | Backend, path | Frames | Fired minus deadline, p50 / p99 / max us | Moved | GPU moved (frame late) | Fired early | Fired late | Flip cost, mean us |
|---|---|---|---|---|---|---|---|---|
| fullscreen, codes, 1 min | DXGI_FLIP, overlay | 3600 | 1.57 / 58.7 / 3200 | 0 | off | 0 | 0 | 12.0 |
| fullscreen, codes, 1 min | COMPOSITION, independent | 3600 | 2.05 / 102 / 646 | 0 | off | 0 | 0 | 12.0 |
| window, `--miss 300`, no fence | DXGI_FLIP | 7200 | 1.98 / 238 / 7933 | 62 | off | 72 | 0 | 12.6 |
| window, `--miss 300`, fence | DXGI_FLIP | 7200 | 0.98 / 215 / 3240 | 137 | 71 (70) | 0 | 0 | 12.4 |
| window, `--miss 300`, fence | COMPOSITION | 7200 | 0.97 / 271 / 3655 | 70 | 70 (70) | 1 | 0 | 11.4 |
| window, idle, fence | COMPOSITION | 7200 | 1.16 / 300 / 2103 | 0 | 0 | 1 | 0 | 14.4 |
| fullscreen, idle, fence, 1 min | DXGI_FLIP, overlay | 3600 | 0.99 / 113 / 838 | 0 | 0 | 0 | 0 | 13.6 |

- `--miss 300` gives 3 frames in a row 25 ms of GPU work every 300
  frames. Without the fence every one of their triggers fired a frame
  early (72 of 72); the after-flip hook and the ring said so. With the
  fence, 140 of 142 were moved before they fired; the other 2 were frames
  whose drop the fence could not see.
- The 62 moves on DXGI_FLIP without the fence are the window's path
  changes: a flip shown a vblank early (EARLY) completes before its
  trigger fires, and the trigger moves to it.
- No false move: no trigger fired after its frame (fired late 0), and no
  GPU move in 10800 idle frames. The fence stays off by default because
  this is one GPU.
- The flip cost rose from 4.5 us (codes, no triggers) to 12 us with a
  trigger: arming takes the trigger lock and wakes the worker
  (`yrt_worker_submit`). Begin and flip together miss the 20 us bar.
- A trigger whose record completed before it fired is reported PENDING
  by the after-flip hook: 0 to 3 in 3600 fullscreen frames, up to 131 in
  7200 windowed frames with path changes.
- The table above uses one word for two things. In it, "fired late" is
  a mismatch (YSCR_TRIG_FIRED_LATE: the trigger fired, then its frame
  was shown a vblank EARLIER), not the worker's lateness. The worker's
  lateness is the "fired minus deadline" column. Later tables call the
  mismatch "fired a vblank late".

### The trigger tail (v0.3.0, 2026-10-05)

Work notes, kept as the work goes, so a restart loses nothing. All runs
on AC power (`power: AC` in the flipstats output), Balanced plan, the
laptop panel only, 800 x 600 window unless the row says fullscreen.
"Late" in these notes means fired minus deadline over 20 us.

The split. `yscr_trigger_info` has `woke_ns` (the clock as ysp/rt.h's
worker entered the job, after its spin) and `lock_ns` (the wait for the
trigger lock). Fired minus deadline = wake (ysp/rt.h's own lateness) +
lock + dispatch.

| Run (DXGI_FLIP, window, 1200 frames) | Fired - deadline p50 / p99 / max us | Wake p99 / max | Lock p99 / max | Dispatch p99 / max |
|---|---|---|---|---|
| default | 0.76 / 18.4 / 356 | 17.7 / 356 | 0.4 / 1.2 | 1.4 / 7.8 |
| channel offset 4 ms (deadline off the vblank) | 0.52 / 1.74 / 32.6 | 0.40 / 31.7 | 0.3 / 9.9 | 0.8 / 8.7 |
| spin window 3 ms (default 1.2 ms) | 1.10 / 48.7 / 215 | 47.3 / 215 | 0.5 / 2.4 | 1.6 / 4.8 |

- The lock is not the cause: its wait is at most 13 us in every run.
  The whole tail is in the wake.
- The spin window is not the cause: 3 ms does no better than 1.2 ms.
- The tail is tied to the vblank: with the deadline 4 ms after it, p99
  falls from 18 to 50 us to 1.7 us.
- Repeated default runs vary widely: p99 18 to 240 us, max 0.36 to 23
  ms, in 20 to 30 s windows minutes apart.

Where the time goes. A probe (now removed) armed ysp/rt.h's worker 300
us early and spun to the deadline inside the job, logging every gap over
5 us between two clock reads, with the CPU before and after and the
thread's cycle count (QueryThreadCycleTime) across the gap. In 1800
frames: 962 gaps over 20 us, 740 of them starting 50 to 100 us before
the vblank time, 643 crossing the deadline, 50 to 400 us long. Across a
gap the thread ran almost no cycles and stayed on its CPU (2 of 962
moved). The thread was stopped on its CPU while something else ran
there: an interrupt or a DPC, or a thread of higher priority.

- Not a thread of priority 16 (dwm.exe has 3, csrss.exe 2): with the
  worker raised to 18 by MMCSS ("Pro Audio", critical), the tail stayed
  (p99 166 and 241 us against 108 and 223 us without, alternated).
- A DPC: `% DPC Time` per CPU (typeperf, 1 s samples) shows 2 to 3% of
  one P-core (330 to 500 us per frame at 60 Hz) during a run, on the CPU
  the worker spun on (CPUs 0 to 2 in that run). Pinned to E-core 10, the
  DPC ran on P-cores 2 and 4 and the worker fired at p99 5.4 us.
- Pinned per CPU (900 frames each): CPU 0 p50 31 us, CPU 1 p50 35 us
  (the same core), CPU 2 p99 292 us, CPU 3 p99 206 us, CPU 4 p99 120 us,
  CPU 6 p99 4.2 us, CPU 10 p99 6.1 us, CPU 15 p99 7.2 us. A pinned
  thread cannot move off a CPU that is taken: max 0.4 to 48 ms.

So the cause: ysp/rt.h's worker prefers the P-cores, and on this laptop
the display's vblank DPC runs on a P-core, often the one the worker is
spinning on. Raising the priority cannot help against a DPC.

Placement trials (window, 1200 frames each, with the self-arming below):

- A soft CPU set (SetThreadSelectedCpuSets) of the E-cores did not keep
  the worker there: Windows still ran it on P-cores 0 to 6, p99 37 and
  132 us.
- A hard mask of the 8 E-cores: p99 2.1 and 2.6 us, 6 and 7 triggers over
  20 us late. A mask of 4 E-cores did as well (1.3 and 2.9 us); CPUs
  12 to 15 once gave p99 2.8 ms. The gap probe with the 8 E-core mask: 4
  and 0 gaps over 20 us before the deadline, none crossing it.
- Pinned to E-core 10: p99 1.0 and 1.7 us, but a pin cannot leave its CPU.

The fix: on Windows on a hybrid CPU, the worker's on_start sets a hard
affinity mask of the E-cores (all logical CPUs below the highest
EfficiencyClass) and drops ysp/rt.h's P-core CPU sets. `trigger_cpu = -1`
keeps ysp/rt.h's placement; `k > 0` still pins.

Arming cost. `yscr.trigarm` was 3.8 us mean on AC (8 us on battery),
of which `yscr.trigsubmit` (yrt_worker_submit: the worker's lock, an
event and a condition variable) was 3.3 us. The trigger lock itself was
uncontended. So the arming cost is the wake-up, not the lock, and a
lock-free job slot would not remove it. The fix: after a flip trigger
fires, the worker arms itself 50 us before the next vblank's deadline
(`YSCR__SPEC_EARLY_NS`), and the frame thread submits only when its
wake is earlier than the one armed. In steady state the frame thread
never submits; the worker wakes 50 us early, finds nothing due, and
re-arms itself for the real deadline. With no trigger in the next frame
the worker spins one window for nothing. The race guarantees are
unchanged: every state change is still under the one lock, a FIRING job
cannot move, and the test checks it.

Lock waits. `yscr__lock` now tries the lock first and records a
`yscr.lockwait` zone only when it has to wait: 0 to 15 waits in 1200 to
3600 frames, at most 2.1 us.

### Before and after (v0.3.0, 2026-10-05, AC)

"Before" is the header as it was at the start of this work (worker on
the P-cores, a submit per trigger), built from a copy; "after" is the
current header. One trigger per frame, a no-op callback, the laptop panel,
Balanced plan, `power: AC` on every run. "Over 20" and "over 200" count
triggers that fired that many us or more after their deadline.

Fullscreen, 3600 frames each:

| Run | Fired - deadline p50 / p99 / max us | Over 20 / over 200 | Wake p99 | trigarm mean us | flip_at mean us | Drops |
|---|---|---|---|---|---|---|
| DXGI_FLIP, before | 0.97 / 31.5 / 369 | 152 / 2 | 30.7 | 3.83 | 5.03 | 0 |
| DXGI_FLIP, after | 0.46 / 1.66 / 406 | 4 / 1 | 0.56 | 0.94 | 2.56 | 0 |
| DXGI_FLIP, after, again | 0.45 / 2.55 / 2108 | 11 / 4 | 0.35 | 0.73 | 2.01 | 0 |
| COMPOSITION, before | 0.99 / 29.2 / 1627 | 122 / 2 | 28.2 | 4.08 | 5.28 | 0 |
| COMPOSITION, after | 0.47 / 1.76 / 9239 | 6 / 6 | 0.59 | 1.16 | 3.04 | 3 |
| COMPOSITION, after, again | 0.48 / 1.60 / 703 | 7 / 3 | 0.45 | 0.64 | 1.84 | 5 |

The fullscreen runs used the 6-minute budget. They ran between 19:15 and
19:47, inside the hour in which another agent ran GPU pixel tests and
started and stopped Docker Desktop outside the measurement lock, so their
tails may carry that load. They were not repeated in fullscreen.

Window, the final batch, repeated on a quiet machine (no docker, cl,
gcc or gfx test process in `tasklist` before each run, GPU 3D engine at
0.2%, under the lock, 19:52 to 20:00), 3600 frames each, alternated:

| Run | Fired - deadline p50 / p99 / max us | Over 20 / over 200 | trigarm mean us | flip_at mean us | Drops |
|---|---|---|---|---|---|
| DXGI_FLIP, before | 2.66 / 169 / 12563 | 1116 / 22 | 11.7 | 15.8 | 1 |
| DXGI_FLIP, after | 0.66 / 3.15 / 4460 | 18 / 8 | 1.46 | 4.34 | 1 |
| DXGI_FLIP, before | 2.37 / 148 / 4857 | 1023 / 19 | 7.88 | 10.7 | 2 |
| DXGI_FLIP, after | 0.48 / 2.40 / 31268 | 18 / 15 | 1.17 | 3.49 | 1 |
| COMPOSITION on top, before | 2.56 / 140 / 2460 | 877 / 18 | 10.7 | 13.9 | 13 |
| COMPOSITION on top, after | 0.46 / 1.08 / 861 | 12 / 2 | 0.62 | 1.71 | 0 |
| COMPOSITION on top, before | 0.98 / 38.8 / 669 | 227 / 1 | 4.03 | 5.18 | 0 |
| COMPOSITION on top, after | 0.46 / 0.96 / 347 | 6 / 1 | 0.69 | 1.87 | 0 |

The first runs of this batch had a slow frame thread in both builds
(trigarm 8 to 12 us before); the after build is faster in each pair.
An earlier window batch (19:37 to 19:43, possibly under the other agent's
load) agreed: p99 32 to 94 us before, 1.3 to 3.0 us after; maxima 0.88
to 1.9 ms before, 0.55 to 43 ms after.

The OS tail. examples/rt/jitter.c, 1800 waits of 16.667 ms per spin
window, no display involved, AC, under the lock:

| Placement | Spin 1.2 ms (default): p50 / p99 / max us | Spin 0: p50 / p99 / max us |
|---|---|---|
| E-cores (`start /affinity FF00`), 19:26 | 0.0 / 0.2 / 2603 | 448 / 902 / 2646 |
| P-cores (default), 19:28 | 0.0 / 21.2 / 2996 | 377 / 844 / 3689 |
| E-cores, quiet, 20:00 | 0.0 / 0.1 / 32559 | 338 / 663 / 1049 |
| P-cores, quiet, 20:02 | 0.0 / 0.0 / 20.5 | 319 / 634 / 746 |

On the simulated display (no GPU, no window), 7200 triggers on the
E-cores: p99 1.68 us, max 2232 us, 2 over 200 us.

What this says:

- The p99 target (20 us) is met in every after run; the before runs
  missed it in every run, by the vblank DPC.
- The max target (200 us) is missed in every run, before and after. The
  waits that make it are rare (1 to 15 a minute after, 1 to 22 before)
  and come from the OS: rt_jitter without a display, on the same cores
  and the same spin window, woke 2.6 ms and 32.6 ms late in two of four
  runs. A wider spin window covers a late timer (spin 0 shows the timer
  alone up to 3.7 ms late), not a thread kept off its CPU, at the cost
  of a spinning core.
- The E-cores carry their own rare stall: the 32.6 ms rt_jitter wake and
  the 31 and 43 ms trigger waits were on E-cores. P-cores had stalls of
  12 to 48 ms in the earlier pinned and default runs. The data do not
  show which placement has fewer such stalls; they do show the E-cores
  remove the DPC tail. `trigger_cpu = -1` restores the P-cores.
- begin() cost the frame thread more in some after runs (16 to 32 us
  mean against 14 to 24 us before, in runs minutes apart); with the
  worker pinned to E-core 10 and on the P-cores in the same minutes it
  was the same, and lock waits were at most 2.1 us, so the change is not
  the header's; whether the DPC moved to the frame thread's CPU was not
  measured.
- COMPOSITION fullscreen dropped 3 and 5 frames a minute after and 0
  before, in the window of possible outside load; in the quiet window
  batch the after runs dropped 0 and 0. Not resolved with these few runs.

Mutations. The core test catches each of these 24 changes to the header
(`gcc -std=c11 -O1` on WSL, one build per change): the snap without its
lead, the depth on one vote, drops not counted, ESTIMATED not set, the
prediction a vblank late, the hold a vblank early, LATE_TARGET not set, a
path change not adopted at once, the native depth raised on one miss,
EARLY not set, an estimated flip keeping its tier, a trigger not moved
after a late present, a taken trigger moved, a taken trigger fired again,
an armed trigger not moved to the vblank shown, the fence never moving,
the mismatch direction swapped, the channel offset ignored, the
trigger_at offset ignored, FLUSHED not set, triggers fired out of deadline
order, the frame thread submitting on every change, no self-arming, and
self-arming after the deadline. The native depth and EARLY changes were
first missed; `test_one_miss_and_early` now catches them (one drop on a
backend that holds frames leaves the depth at 1; a DXGI-like path whose
depth falls from 2 to 1 shows a flip a vblank early, and EARLY is set on
that flip and on no other). The previous agent's list of 20 was lost;
this list replaces it.

### Where the trigger worker runs: the rule and this machine

The rule is: keep the trigger worker off the CPUs that service the
display's vblank DPC. A DPC preempts every thread at any priority, so a
worker that spins on that CPU at the vblank waits until the DPC ends.

The E-cores are this machine's answer to that rule, not the rule. The
measurements that justify it here (i7-1360P, Iris Xe, Windows 11,
Balanced, AC; all above):

- The gap probe: in 1800 frames, 740 gaps over 20 us started 50 to 100
  us before the vblank, 643 crossed the deadline, 50 to 400 us long, with
  the worker's thread running almost no cycles and not changing CPU.
- `% DPC Time` per processor during a run: 2 to 3% of one P-core (330
  to 500 us per frame), on the P-cores the worker ran on; with the worker
  pinned to E-core 10, the DPC ran on P-cores 2 and 4 and the worker
  fired at p99 5.4 us.
- MMCSS priority 18 did not remove the tail, so it is not a thread.
- A hard mask of the E-cores: p99 1 to 3 us in every run after.

On another machine this can differ. A CPU of one core type has no
E-cores; the default is then ysp/rt.h's placement, which can share a CPU
with the DPC. Another GPU or driver can route its interrupt elsewhere, an
administrator can set the device's MSI interrupt affinity policy, and
Windows can steer the interrupt toward a CPU that is awake. On such a
machine the E-cores may be the busy ones. To check a rig: run
`screen_flipstats --trigger` (it prints the triggers over 20 us late per
CPU) and `typeperf "\Processor(*)\% DPC Time"` during the run, and set
`desc.trigger_cpu` from what they show.

Later (not built): a diagnostic at open that samples the DPC count or DPC
time of each CPU for about 0.5 s while the swap path flips, and places
the worker on the CPUs with the least. The documented way is PDH (the
counters `\Processor(*)\DPCs Queued/sec` and `% DPC Time`, as typeperf
reads them), which needs no privilege. The undocumented ways are
NtQuerySystemInformation with SystemProcessorPerformanceInformation
(per-CPU DPC and interrupt time in fields the SDK calls Reserved1) or
SystemInterruptInformation (per-CPU DPC counts). The cost of the sample
at open, and whether 0.5 s finds the vblank DPC's CPU every time, are
not measured.

### The OS gamma ramp

`examples/screen/gamma.c`, without changing what is shown: the laptop's
ramp is the 8-bit identity; setting the same ramp again and reading it
back was exact; GetDeviceGammaRamp took 0.8 to 20 ms (the first call is
slow), SetDeviceGammaRamp 4.6 to 18 ms. Because the ramp was already the
identity, no fullscreen run set one, and the identity path of
`yscr_open` did not run on hardware. Microsoft documents that
SetDeviceGammaRamp can report success without setting a ramp, that other
programs and the OS may overwrite it, that display events reset it, and
that it is undefined under HDR
([SetDeviceGammaRamp](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-setdevicegammaramp)).
No primary source says whether it acts on independent-flip or overlay
frames, or how it combines with Night Light, which uses the GPU color
pipeline ([display calibration pipeline](https://learn.microsoft.com/en-us/windows/win32/wcs/display-calibration-mhc));
that needs a capture card or light, and is not claimed.

### Two frames planned for one vblank (v0.3.0, 2026-10-05)

The video agent's report, windowed DXGI_FLIP (composed, 640 x 360, AC):
begin() sometimes planned two frames in a row for one vblank (their
predicted onsets 136 ns apart, after flips flagged EARLY), and f.vblank
sometimes rose by 2 before a drop was reported.

Reproduced on the virtual clock (`test_flap_prediction`): a DXGI-like
scripted presenter whose path moves between composed (depth 2) and
overlay (depth 1) every 9 presents, with a random wait of up to a third
of a period per frame. 300 frames: 16 frames planned on the vblank of the
frame before, 48 early flips in runs of 3.

The cause, in two parts:

1. A flip planned at depth 2 when the depth has fallen to 1 shows a
   vblank early and completes. begin() kept "one frame per vblank" only
   against flips still in flight and the newest vblank shown, so the next
   frame was planned for the vblank the early frame had been planned for.
   The 136 ns is the grid's period refit between the two begin() calls,
   not a different vblank.
2. After a path change the header adopts a new depth on one flip. The
   first flip on the new path was shown on the vblank after the flip
   before it: it had waited behind that flip in the queue, so its count
   matched the old depth (2). That match closed the one-flip window, and
   three flips in a row had to vote the depth down: 3 early flips per fall.

The fix: the screen keeps the vblank flip_at() last planned, and begin()
and flip_at() never plan at or before it. A flip shown on the vblank right
after the flip before it no longer closes a path change's window (for up
to 3 such flips). On the virtual clock: 0 frames on the vblank of the
frame before, 16 early flips, each alone. The price: the early frame
stays on the screen for the vblank it was planned for, so that vblank
shows it a second time, where before a second frame was planned there.
The prediction stays monotone, which ysp/timeline.h and ysp/video.h
assume.

The rise by 2 is not a fault. When the depth rises, the flip planned at
the old depth shows late (a drop), and the frame after it is planned 2
vblanks on; its begin() runs before the drop's record completes, because
one frame is in flight. On the virtual clock every vblank that no frame
was planned for followed an early flip or preceded a late one (the test
checks it). On a backend that holds frames (COMPOSITION), the flapping
script ran at half rate on the overlay path, because the depth there is
lowered only on evidence (DEPTH); that is the depth rule, not this bug,
and was not changed.

Windowed check (DXGI_FLIP, `--windowed --topmost`, AC, under the lock,
Docker Desktop running; flipstats counts frames planned at or before the
vblank of the frame before):

| Build | Frames | Planned at or before the frame before | Early | Dropped |
|---|---|---|---|---|
| fixes reverted | 3600 | 1 | 3 | 2 |
| fixed | 5400 | 0 | 3 | 4 |

The window stayed on the overlay path except at start (6 to 8 composed
frames), so these runs saw few depth falls; the virtual-clock test is the
evidence, the window runs only confirm that nothing else broke.

### ysp/video.h's requests (v0.3.0)

- `yscr_frame.done`, `n_done`, `done_lost`: every record completed
  since the begin() before, in ring order, including those completed in
  flip_at() during a hold and the ESTIMATED ones. In the windowed runs
  above, f.done carried 3599 of 3600 and 5399 of 5400 records (the last
  completes in close()).
- `yscr_native()`: the D3D11 device and immediate context, ANGLE's EGL
  display, the adapter LUID. Borrowed pointers, valid until close(); the
  device is free-threaded, the context is the frame thread's and ANGLE's.
  In the windowed runs all three were set and the LUID was
  00000000:0001e8e6. v0.3.1 adds the device with VIDEO_SUPPORT and
  multithread protection (`desc.d3d11_video`, below).

Mutations now caught: 29 (the 24 above, plus begin() and flip_at()
planning the last planned vblank again, a queued flip closing the
window, completed records not listed, and the list not cleared per
begin()).

## Aborts, the panic watchdog, the icon, the video device (v0.3.1)

Measured on 2026-10-06, on AC. Keys were sent with SendInput only while
the test's own window (or its ghost) was the foreground window.

### The abort combination

Psychtoolbox has no abort key by default. PsychoPy ends on Esc alone,
which participants press by accident. v0.3.0 also ended on Esc alone.
The default is now Shift+Esc, which the user chose: an operator presses
it with one hand, and a participant does not press it by accident. The
other reserved Esc combinations are taken by Windows: Ctrl+Esc opens
Start, Alt+Esc switches windows, and Ctrl+Shift+Esc opens Task Manager.

An abort is reported once (an edge), not while it lasts. The caller can
ask the operator and then continue, and `while (yscr_begin(...) ==
YSCR_OK)` still ends at the first abort.

v0.3.0 peeked at SDL's queue in begin(). A loop that read its events
with `yscr_poll()` before begin() removed the Esc before begin() saw
it. v0.3.1 uses `SDL_AddEventWatch`, which SDL calls for each event as it
is queued, so the header sees every event. The watch can run on SDL's
raw-input thread, so the abort state is a lock-free log of 16 entries
with atomic sequence numbers. begin() reads it with one atomic load when
nothing is new.

Found while testing: with `SDL_HINT_WINDOWS_RAW_KEYBOARD` on, SDL 3.4
reports each key twice. One report comes from its raw-input thread and
one from the message loop, 0.1 to 11 ms apart. A probe with 8 F24 keys
gave 8 key-downs from each thread and 16 from `SDL_PollEvent`. With the
hint off, the probe gave 8. The tree's v0.3.0 header behaves the same,
so `examples/screen/input.c`'s raw-path table matched the first report
of each key. The header counts a press only after a key-up of the same
key, and treats reports within 30 ms (50 ms from v0.3.3) as one press.
v0.3.3 found the cause: the probe sent keys by virtual-key code, and only
such keys (no scan code) come twice. See "Input for ysp/response.h
(v0.3.3)".

`examples/screen/abort.c` (a child process with a 320 x 200 window that
reads its events with `yscr_poll()` before begin()):

| Case | Result |
|---|---|
| Esc alone | no abort |
| Shift+Esc | one abort; stamped 0.12 to 0.70 ms after SendInput (5 runs) |
| Shift+Esc held with 8 repeats | one abort |
| 3 presses after the loop reported press 1 | 3 more aborts, 1 press each; no panic; exit 0 |

Alt+F4 was not sent: if the focus moved, it would close another program.
The core test checks it through the feed. The header sets
`SDL_HINT_WINDOWS_CLOSE_ON_ALT_F4` to "0", so SDL does not turn Alt+F4
into a close request. The header reports it as its own reason, and the
caller can ignore it.

### The panic watchdog

The user chose the gesture: Shift+Esc three times within 2 s, which is
what people do when a program seems stuck. The abort and the panic use
one combination.

The watchdog uses a low-level keyboard hook (`WH_KEYBOARD_LL`) on its
own thread, not `RegisterHotKey`. RegisterHotKey takes the combination
for the whole system, hides it from SDL (so press 1 would not be an
abort), fails when another program holds it, and fires whatever window
has the focus. The hook sees every key, passes every key on, and
consumes nothing. It counts a press only while a window of this process
is in front. The thread sleeps in GetMessage; the hook does a few
compares and posts the panic to its own thread.

Two of the design's open questions were decided by measurement
(`probe.c` in the worker's scratch, not in the tree):

- **Ghost windows.** A window that pumps no messages for 4.5 s is
  marked hung. At 5.0 s the foreground window became a window of class
  "Ghost" owned by another process (dwm). A foreground check by process
  alone would then miss every press. The hook also accepts a "Ghost"
  window while one of the screens' windows is hung. The header does not
  call `DisableProcessWindowsGhosting()`, which would change the process
  for its whole life.
- **Hook removal.** `LowLevelHooksTimeout` is not set on this machine
  (the default applies). A test hook that stalled for 1.5 s on one key
  was still called for the next two keys. The header does not reinstall
  the hook.

The rule against data loss: no panic while a begin() has reported an
abort within the last 10 s. A begin() heartbeat was considered: it would
kill a program that is outside its frame loop (loading, saving) at the
wrong moment, and that program is not hung. The acknowledgement shows
whether the program answers the abort itself.

The display mode: ChangeDisplaySettingsExW from the watchdog thread
blocked for more than 1 s. A display change sends to every window, and
the hung window does not answer. The desktop mode came back 1.21 s
after press 3 with that call, and 1.16 and 1.18 s after press 3 without
it. In both cases Windows put the mode back while the process was ending
(the end itself takes about 1.2 s with a D3D11 device). The call was
taken out.

| Case (`screen_abort.c`) | Result |
|---|---|
| hung window, 3 presses 300 ms apart | exit code 99, 28 to 85 ms after press 3 (5 runs); panic_fn ran; the gamma entry was put back |
| hung 7 s (ghost window in front), 3 presses | the same, 35 to 79 ms (4 runs) |
| hung, Shift+Esc held with 8 repeats | no panic; then 3 presses: exit 99 |
| fullscreen 1680 x 1050 on the 1920 x 1200 desktop, hung, 3 presses | exit 99; desktop mode back 1.16 and 1.18 s after press 3 (2 runs of this build, 1.21 s with the removed call; about 3 s of a dark gray screen each) |

The gamma entry was the display's current ramp (the identity), so the
restore changed nothing that could be seen. The ramp of a real switch
was not set on hardware (see "The OS gamma ramp").

Keys on SDL's raw path with the hook armed and not armed, interleaved,
`screen_input.c --panic`, 199 F24 keys each (restamped minus sent, ms):

| Run | Hook | mean | p50 | p99 | max |
|---|---|---|---|---|---|
| 1 | off | 0.492 | 0.459 | 1.019 | 2.441 |
| 1 | armed | 0.310 | 0.303 | 0.696 | 0.730 |
| 2 | off | 0.365 | 0.347 | 1.074 | 1.075 |
| 2 | armed | 0.326 | 0.311 | 0.555 | 0.625 |
| 3 | off | 0.464 | 0.367 | 1.826 | 3.197 |
| 3 | armed | 0.327 | 0.305 | 0.669 | 0.851 |

The hook did not make keys later. They were earlier and tighter with it,
which this test cannot explain; the hook thread may keep the input path
busy.

### The video device

`desc.d3d11_video` follows the ysp_video worker's design:
`D3D11_CREATE_DEVICE_VIDEO_SUPPORT` on both device creations, and
`SetMultithreadProtected(TRUE)` through `ID3D11Multithread`
(ID3D10Multithread has the same IID). `yscr_native()` reads
`video` and `mt_protected` from the device, not from the desc. With the
flag, both were 1 on DXGI_FLIP and on COMPOSITION; without it, both were
0.

The header's CPU time per frame, under the measurement lock,
`screen_flipstats --windowed --topmost --frames 1800`, AC. Three rounds,
interleaved: flag off, flag on, and the v0.3.0 header (its peek at the
queue instead of the watch). "begin" is begin() minus its wait; the
window was on the overlay path:

| Round | Build | begin mean us | p50 | p99 | flip mean us | pump zone mean us |
|---|---|---|---|---|---|---|
| 1 | off | 25.2 | 23.8 | 62.8 | 1.96 | 9.2 |
| 1 | on | 26.3 | 23.8 | 51.6 | 2.01 | 10.2 |
| 1 | v0.3.0 | 29.5 | 27.5 | 53.2 | 2.23 | 11.6 |
| 2 | off | 22.5 | 20.3 | 92.5 | 1.71 | 8.9 |
| 2 | on | 24.5 | 19.2 | 38.3 | 1.54 | 12.0 |
| 2 | v0.3.0 | 21.6 | 20.0 | 40.7 | 1.54 | 8.0 |
| 3 | off | 23.5 | 21.7 | 41.9 | 1.72 | 9.2 |
| 3 | on | 19.6 | 18.5 | 37.8 | 1.54 | 7.2 |
| 3 | v0.3.0 | 20.0 | 18.1 | 43.0 | 1.41 | 7.9 |

Other workers' compilers ran during round 1 (each run waited for none to
be running at its start). No difference shows beyond the spread between
rounds. The ysp_video worker's own interleaved runs agree: begin 21.0 to
22.0 us off against 21.2 to 21.9 us on, p99 43 to 45 us both. With no
decoder on the device, the protection costs nothing measurable. The
header still misses its 20 us bar, on the OS calls, as before. The case
with a decoder on the device belongs to ysp/video.h's measurements.

### The window icon

The icon is Escher's impossible cube, which the user picked over a brain
and a Penrose tribar. The 48 pixel size is ray-cast from 3D square beams
with three face shades, and one pair of beams overlaps the wrong way
round. The 16 and 32 pixel sizes are drawn as line art, because the
ray-cast was a dark blob at 16. The palette has 5 colors, stored as runs
of 4-bit indices: 1292 bytes of runs and 20 of palette.

SDL 3.4's `SDL_SetWindowIcon` with alternate images gave the window one
32 pixel icon for both ICON_SMALL and ICON_BIG (read back with
WM_GETICON at 96 dpi), so Windows would shrink it for the title bar. On
Windows the header sets ICON_SMALL and ICON_BIG itself: the smallest of
its sizes at least as large as the window's icon metric at its dpi. Read
back, ICON_SMALL was the 16 pixel art and ICON_BIG the 32, every pixel
equal to the header's.

### Builds (v0.3.1, 2026-10-06)

gcc 13.3 in WSL2: the core test at -O2 and under ASan and UBSan. gcc 13.3
and clang 18.1 in an Ubuntu 24.04 container with SDL3 3.4.0: the compile
checks, the COM check, the test, the examples and the loopback test,
with the CI jobs' flags; the examples on the simulated display give
their documented exit codes. MinGW-w64 gcc 16.2 (MSYS2): the Screen
job's MinGW step. All pass. emcc 6.0.10 compiles it only inside
ysp/gfx.h's and ysp/video.h's compile checks (`YSCR_NO_SDL`), which
pass under node; its own test was not run under emcc.

## Input for ysp/response.h (v0.3.3)

Measured on 2026-10-07, on AC, with `examples/screen/input.c --reports`:
F24 sent with SendInput while the test's window had the keyboard focus,
on SDL's raw keyboard path. Five runs (20, 50, 50, 199 and 199 presses
per row, 518 taps in all).

### The double key report has one cause

v0.3.1 found that SDL 3.4 reports each key twice with the raw path on.
That probe injected keys by virtual-key code only (`wVk`, scan code 0).
SDL 3.4.0's source (`SDL_windowsevents.c`) sends a key from `WM_KEYDOWN`
only when the raw keyboard is off, text input is on, or the message has
no scan code. So the second report belongs to keys without a scan code.

| Injection | Hold | Key-downs per press | Second report after the first |
|---|---|---|---|
| Virtual key (`wVk`, scan code 0) | tap (down and up back to back) | 2, each with its key-up | -0.1 to 34.2 ms; 517 of 518 within 17.1 ms |
| Virtual key | 100 ms | 1, then 1 marked as a repeat | |
| Scan code (`KEYEVENTF_SCANCODE`) | tap | 1 | |
| Scan code | 100 ms | 1 | |

For the 100 ms hold, SDL's own key state marks the second down as a
repeat and drops the second up, because both paths report as hardware
keys. Only a press shorter than the gap gives two full presses.

Keys from a keyboard carry scan codes, so they most likely report once.
That was not measured: no key was pressed by hand here.
`screen_input --hand [s]` logs keys pressed by hand for s seconds (30 by
default): the reports of each press and their path (`which` is the
keyboard's device id on the raw path and 0 on the message path). It
prints `RESULT: one report per press` or `RESULT: some keys reported
twice`.

The abort merge was 30 ms, shorter than the longest measured gap (34.2
ms, 1 of 518). Remote-desktop and assistive tools inject by virtual key,
so two deliberate presses of Shift+Esc through such a tool could count as
four and meet the panic rule (3 presses), which ends the program. v0.3.3
merges reports of one abort press within 50 ms, as `ysp/response.h` does.
Two presses by hand cannot fall within 50 ms, so no press is lost. The
core test sends two presses reported twice, 34.2 ms apart, to an armed
watchdog: no panic (mutant `input-02`, with 30 ms, is caught).

v0.4.1 drops the second reports in `yscr_event_input()` for every
consumer ("Second key reports").

### caps.raw_keyboard

`yscr_get_caps()` reads it at each call: true on Windows with a window,
`SDL_HINT_WINDOWS_RAW_KEYBOARD` on and text input off. `ysp/response.h`
uses it to label key stamps: the raw path's host part was measured (0.16
to 0.67 ms after SendInput); the message path's is tick-quantized (-6.2
to +12.1 ms).

### Text input that another library starts

`docs/imgui_probe.md` (finding 3) found that Dear ImGui's stock SDL3
backend starts SDL text input on whichever window has the focus. On the
stimulus window, keys then left the raw path and were stamped 11.4 ms
after they were sent (p50), and v0.3.2 recorded nothing: only
`yscr_text_input()` wrote the ring record and set the flip flag.

v0.3.3 observes the state. `flip_at()` reads `SDL_TextInputActive()` for
the window, and when it differs from the last logged state, writes the
same `YSCR_EV_TEXT_INPUT` record with `u.u32[5]` = 1 (external; the
rectangle 0) before it sets `YSCR_FLIP_TEXT_INPUT`. Records from
`yscr_text_input()` have `u.u32[5]` = 0. A change between two flips is
seen at the next flip, so the flag can be one frame late.
`caps.raw_keyboard` reads the state when it is called.

Measured on 2026-10-07 (AC, measurement lock, a 320 x 200 window on
DXGI_FLIP), with `SDL_StartTextInput()` called directly at frame 30 and
`SDL_StopTextInput()` at frame 80 of 120:

| Check | Result |
|---|---|
| Flips with `YSCR_FLIP_TEXT_INPUT` | 50: frames 30 to 79 |
| Ring records | on, external; off, external |
| `caps.raw_keyboard` | 0 while on, 1 after off |
| `SDL_TextInputActive()` | 21.1 ns a call (best of 21 rounds of 100,000) |
| `yscr_get_caps()` | 182.1 ns a call (it reads the raw-keyboard hint) |

The core test models the other library through a seam
(`YSCR__TEXT_INPUT_ACTIVE`): on at frame 3, off at frame 7, two external
records and the flag on frames 3 to 6. Mutants `input-03` (state not
observed) and `input-04` (not marked external) are caught; `text-01` was
re-anchored on the shared logging function.

### Devices (v0.3.4)

SDL 3.4 on Windows: a key on the raw path carries the Raw Input handle of
its keyboard (`which`), so keyboards are told apart. A key on the message
path, and a key from SendInput, carries 0. Raw mouse input is on only in
SDL's relative mode (`WIN_SetRelativeMouseMode`); in the absolute mode
that `ysp/screen.h` uses, every mouse arrives as mouse 0 through window
messages, with the message time. The abort and panic keys count every
keyboard on purpose: the watchdog's low-level hook sees the system's
keys, not devices.

With `desc.ring`, a screen with a window writes a `YSCR_EV_DEVICE`
record for each keyboard, mouse and touch device SDL lists at open (and
each gamepad with `desc.gamepads`), and one for each later ADDED or
REMOVED event that passes through `yscr_poll()`. Pens, and touch
devices SDL did not list, get a record when first used. A table of 32
devices per screen keeps it to one record per change. Names are cut at
23 bytes in the record. The ids are Windows handles: a device that is
plugged in again gets a new id.

`screen_input --devices` on this laptop (no keys pressed):

| Kind | Change | Id | Name in the record |
|---|---|---|---|
| keyboard | present | 65659 | Computer Corp Dell Univ (a USB receiver) |
| keyboard | present | 65601 | Standard PS/2 Keyboard (the built-in one) |
| keyboard | present | 65599 | HID Keyboard Device (0x |
| mouse | present | 65661 | Computer Corp Dell Univ |
| mouse | present | 65610 | TrackPoint Device |
| mouse | present | 65608 | Microsoft HIDI2C Device |
| touch | present | 18446744073709551614 | pen_input (SDL's touch device for the pen, `SDL_PEN_TOUCHID`) |

It also prints SDL's full names, and the device of each key and click
pressed in its window. Checked: the core test calls the logging function
directly (present, a repeated ADDED dropped, the same id under another
kind, a removal and a repeated one, a 30-byte name cut to 23, a 64-bit
touch id, 40 gamepads past the 32-entry table all logged); mutants
`dev-00` to `dev-02`, caught; the whole list, 22 of 22.

### The input bridge (v0.4.0)

One loop for every input. A producer on any thread (the raw mouse
reader, a response-box reader, an eye tracker) calls
`yscr_push_input(&e)` with a `ysp/input.h` event stamped on the ysp_rt
clock. The header stores the event and pushes one SDL event of a
registered type, a doorbell, whose `user.code` is the record's sequence
number. `yscr_poll()` returns doorbells in SDL's stream; ImGui and
other consumers see the stream unchanged. `yscr_event_input()` turns a
doorbell into its record and SDL's own keyboard, mouse, touch, pen and
gamepad events into `yin_event`s through `ysp/input.h`'s adapter.

Decisions (the user approved the design with one change: no second poll
call):

| Question | Decision | Why |
|---|---|---|
| One poll or two | One: `yscr_poll()` plus a decode call | ImGui and the rest see one stream. |
| How a doorbell finds its record | The sequence in `user.code`; the record in slot `seq % size`, checked on decode | A doorbell decoded late, out of order or never does not shift any other. |
| A full store | The newest record overwrites the oldest; counted; its doorbell then decodes as lost | A doorbell the caller never decodes would otherwise hold its slot forever. |
| SDL's queue full (65,535, measured) | The doorbell is counted and pushed again, oldest first, when SDL's queue is empty | The event arrives late, but it arrives. |
| Timestamps | The producer's ysp_rt stamp, kept as given | SDL's push time says when it was queued, not when it happened. |
| Raw mice | Over the bridge; `yscr_poll_mouse()` deprecated, reads the store for one more version | One loop. |
| ysp/screen.h requires ysp/input.h | Yes, from v0.4.0 | The store holds `yin_event`s. |

Measured on 2026-10-08 (AC, the timing guard, a 320 x 200 window, two
runs; `yscr_push_input()` timed one call at a time):

| Check | Result |
|---|---|
| 4 producer threads at about 1 kHz each, 8000 events | each decoded once, in each producer's order; none overwritten, refused or lost |
| `yscr_push_input()` with a window | 1.9 to 2.6 us (SDL_PushEvent runs the event watches and wakes the queue); 184 to 345 ns in the prototype without a window |
| `yscr_event_input()` on a doorbell | 74 to 81 ns |
| A doorbell's poll and decode | 410 to 424 ns (SDL_PollEvent pumps the window's messages) |
| 70,000 pushes without a poll | SDL refused 4465 doorbells; the store kept the newest 4096 records, all decoded over the next 3 frames; 65,535 doorbells of overwritten records counted as lost |
| Doorbells decoded out of order and twice | each gave its own record; the second decode gave nothing |
| Raw mice through the bridge | 30 of 30 injected moves, with SDL's keys in the same loop |

The core test (no SDL) rings its own doorbells through a seam
(`YSCR__DOORBELL`): in order, skipped and late decodes, a decode
twice, a record overwritten and its doorbell lost, refused doorbells
rung again oldest first, 4 producer threads of 1000 events, the ring
record once per change, and the deprecated `yscr_poll_mouse()` over
the store. Mutants `br-00` to `br-05`, caught; the whole list, 33 of
33. Two needed a second look: removing the first sequence check left a
second one that catches the same fault (the mutant now removes both),
and the loss record had a redundant early return (removed).

### Second key reports (v0.4.1)

SDL 3.4 reports a key with no scan code two times ("The double key report
has one cause"). Up to v0.4.0, only `ysp/response.h`'s collector dropped
the second report (its rule R3). A program that read keys for any other
purpose got both: `examples/response/trial_adjustment.c` stepped its bar
two times for one virtual-key tap. From v0.4.1, `yscr_event_input()`
drops them, so every consumer of the input bridge gets one press per key.
The user approved the move on 2026-10-09.

| Question | Decision | Why |
|---|---|---|
| Where | In `yscr_event_input()`, on keyboard events from SDL (raw and message paths) and from doorbells | The one call that every consumer of the bridge makes. SDL's own event stream is not changed: Dear ImGui and the abort watch read it. |
| The rule | A key-down of the same scancode within 50 ms of the last key-down kept, before or after it, from the same keyboard or with either device 0 | The rule of `ysp/response.h` v0.1 (R3): the largest measured gap was 34.2 ms in 518 taps, and the message path's stamp can come first (-0.1 ms). |
| The key-up | After a drop, the first key-up that finds no kept key-down still down is dropped too | A consumer sees one key-down and one key-up per press. The kept press's key-up passes, whichever report comes first. |
| OS repeats | Pass | SDL marks the second report of a held key (100 ms) as a repeat; a consumer's held-key rule reads the flag. |
| One event decoded twice | Passes (same time, device and key) | Two calls on one SDL event must not drop the event as its own second report. |
| State | One table of the 16 keys pressed last, for the process | SDL's queue is one stream for every window, like the bridge's store. |
| Measurable | `yscr_input_stats.key_doubles` and `key_double_ups`; a `YSCR_EV_KEY_DOUBLE` ring record per dropped key-down, with the gap and both devices | The data shows how often the filter acted, and the gaps show whether 50 ms still covers them. |
| Opt-out | `desc.key_dedup_ns`: 0 is 50 ms, < 0 is off for that screen's calls, at most 1 s | A program that studies the reports themselves through the bridge needs them all (`screen_input --reports` reads SDL's events, so it needs no opt-out). |
| Events from elsewhere | `yscr_key_filter()` applies the same rule and table | A simulated participant, or a program's own SDL adapter. |

Measured (MinGW-w64 gcc 16.1 `-O2`, AC, under the timing guard): 9.6 ns
per keyboard event at p50 (9.1 to 11.0 ns, 15 runs of 1,000,000 events,
each key-down scanning a full table). Other events pay one compare.

The core test (no SDL) feeds `yscr__key_keep()` synthetic reports: doubled
taps at 0 (the raw report with a keyboard id, the message report with
0), 10, 49 and 49.999999 ms are one press, at 50, 51 and 200 ms two; the
message report stamped 5 ms first; down, down, up, up; distinct keys 2 ms
apart; two keyboards on one key 5 ms apart; a held key with OS repeats 9
ms apart; one report decoded twice; a mouse click twice in 5 ms; more keys
than the table; the counters; the ring record; `desc.key_dedup_ns` of 10
ms, 0 (50 ms), off, and over 1 s (refused). Mutants `kd-01` to `kd-15`,
caught; the whole list, 48 of 48 (MinGW gcc 16.1). Not measured: keys
injected through the filter in a window, and keys pressed by hand.
`examples/response/trial_adjustment.c --sim` and `trial_keyboard.c
--sim` send a doubled tap through `yscr_key_filter()` and check that it
counts once.

#### A key-up taken by another keyboard (fixed in v0.4.2)

`examples/response/trial_two_keyboards.c --sim` found a fault in the
v0.4.1 rule. Player 1's press came twice (the raw report with the
keyboard's id, then a second report with device 0), and player 2 held
the same key. The second report's key-up (device 0) found player 2's
press still down, matched it as device 0 matches every keyboard, and
passed. Then player 2's own key-up found no press down and passed too.
So `key_double_ups` stayed 0, and the consumer saw two key-ups for
player 2.

The fix has two parts:

- A key-up with device 0 first goes to a press kept with a keyboard's id
  that has a dropped report pending. The kept press's own key-up has
  that id, so a device-0 key-up there is the second report's, and it is
  dropped. Where the kept report itself has device 0, its key-up passes
  as before.
- A key-up pairs with a kept press of its own device before one with
  device 0. Before, the first match in the table won, so the table's
  order could give a device-0 key-up to another keyboard's older press.

The core test plays both: two keyboards on one key, the second report's
key-up while the other keyboard holds the key, in both orders of the
first two key-ups; and the kept report with device 0 while another
keyboard's older press sits earlier in the table. `trial_two_keyboards
--sim` now also checks `key_double_ups` per trial (1 in trial 3, 0
elsewhere). Mutants `kd-16` to `kd-18`, caught.

### Raw mice (v0.3.5)

`desc.raw_mice` (off by default, Windows, a window needed) starts one
reader thread per process. It registers Raw Input for mice (usage page 1,
usage 2) for a message-only window of its own, flags 0, runs at
time-critical priority, and stamps each report when
`GetRawInputBuffer()` returns. `yscr_poll_mouse()` hands the reports
out from a queue of 1024 (single producer, single consumer, no lock;
reports that do not fit are dropped and counted).

Decisions:

| Question | Decision | Why |
|---|---|---|
| Own reader or SDL's relative mode | Own reader | Relative mode hides the cursor and keeps it in the window, which traps the operator's mouse and ImGui (design of 2026-10-07, approved). |
| `RIDEV_INPUTSINK` | Not used | It worked in the prototype, but it takes clicks made on other programs. Without it, reports come only while this process is in front; an operator window of this process keeps it in front. |
| Device filter | SDL's list; others flagged | Windows lists 3 mice here, the same 3 SDL lists (an earlier count of 31 came from a probe bug). Device 0 is system-synthesized: a precision touchpad's cursor, injected input, remote desktop. A report from an unlisted device gets `YSCR_MOUSE_UNLISTED` and one `YSCR_EV_DEVICE` record ("raw, not in SDL's list"). |
| Relative mode | Refused at open; suspended and renewed later | Measured: relative mode takes the registration and its end removes it for the process. |
| Guard cost | Relative mode every frame, the registration on a change and every 30th frame | `SDL_GetWindowRelativeMouseMode()` 23 ns; `GetRegisteredRawInputDevices()` 0.4 to 11 us. 291 ns per frame on average. |
| Stamp | One stamp per `GetRawInputBuffer()` call | SDL interpolates across a call; an interpolated time is a guess. |

Measured on 2026-10-08 with injected input (SendInput moves of +3 and -3
pixels while the window had the focus, a scratch probe, three runs on
battery and AC):

| Check | Result |
|---|---|
| Delivery, raw mice on | every injected move, in order (40 of 40, net 0, in the run with the focus held) |
| Stamp - SendInput | means 0.15 to 0.40 ms |
| SDL relative mode on | no raw reports; one SUSPENDED record |
| Relative mode off | REGISTERED record; reports again, 40 of 40 |
| SDL's raw keyboard meanwhile | every injected key (6 of 6 per phase) |
| A second screen with raw_mice | opens and shares the reader |
| raw_mice while a window is in relative mode | refused with a message |

Not verified: two physical mice coming apart (injected input has device
0). `screen_input --mice` is the hand test: it prints each click and
wheel notch with its mouse and ends with one line per mouse. Not
measured: a physical mouse's stamp, its USB poll.

The core test checks the decoder on synthetic RAWMOUSE data (every
button bit, both wheels, absolute flags), the queue (order, 1024 kept,
losses counted), the unlisted flag and its one record, the guard's
transitions, the ring record and the refusal on the simulated display.
Mutants `rm-00` to `rm-06`, caught; the whole list, 29 of 29.

### desc.gamepads

Off by default. On a screen with a window, open() starts
`SDL_INIT_GAMEPAD` and opens each connected gamepad; `yscr_poll()`
opens a gamepad on `SDL_EVENT_GAMEPAD_ADDED` and closes it on
`SDL_EVENT_GAMEPAD_REMOVED`; close() closes them and stops the
subsystem. At most 8. Not run with a gamepad: none was attached. The
stamps of gamepad events depend on SDL's driver for the pad and were not
measured.

Checked: the core test (no SDL) sees `gamepads` in the parameter table,
off by default, and `caps.raw_keyboard` false on a screen with no window;
five mutants (`input-00` to `input-04` in `tests/mutate/screen.toml`),
all caught; the whole list, 19 of 19.

## Driver-forced vsync off: the sync guard (v0.4.2)

A driver panel can force vsync off for every program: AMD Software "Wait
for Vertical Refresh: Always off", NVIDIA Control Panel "Vertical sync:
Off", Intel Graphics "Vertical Sync: Speed", and Mesa's `vblank_mode=0`
on Linux. If such a setting overrides the header's flip-model
`Present(1)`, frames tear or are discarded, and the flip records still
look like measurements. `docs/rig_spec.md` section 13 asked for a guard.
Prior art: the SDL frame pacing sample (TylerGlaiel/SDL-Frame-Pacing-Sample)
detects "not actually vsynced" from the drift between measured and
snapped frame times. Read for the idea only; no code is copied. The
header already has a record per flip with an OS time and count, so the
guard reads those.

| Question | Decision | Why |
|---|---|---|
| Input | Each flip completion with an OS time, after open's black frames; not while the window is occluded | An estimate has no time to test. A hidden window's presents may not be paced. open() may run composed while a window goes fullscreen. |
| Evidence | Same refresh: the count is not after the previous flip's, or the time is less than half a period after it. No wait: the count is not after the count at the present call, and the time is before the call. Torn: the time is more than a quarter period before the vblank its count names, on the grid the flip was planned with | Under vsync each is impossible: a vblank shows one new frame, and a frame presented in a refresh flips at a later vblank. |
| Why not every off-grid flip | Only flips a quarter period early | Legitimate OS times go off the grid: DXGI's times of held frames, up to half a period either way, with the scanout's vblanks on the grid (409 of 3600 flips in the stress run below; see [Held frames a vblank early](#held-frames-a-vblank-early-v043)). |
| Why no wait needs the time too | Count and time | With `screen_flipstats --hold` on DXGI_FLIP fullscreen, a rule on the count alone had 165 hits in 899 flips (2026-10-09): while the vblanks stretch, the grid's count at the call is one too high. With the time as well: 8 in 1800. The core test's legit case 3 plays stretched vblanks and catches the count-only rule (mutant `sg-03`). |
| Fire (FLIPS) | 32 of the last 64 flips with evidence | A stale count or a jittered stamp is one flip; a forced-off driver gives evidence on nearly every flip. |
| Fire (RATE) | Over a block of at least 64 refreshes between flips with a time, more presents completed (shown, estimated or skipped) than the refreshes plus 8 | A driver that discards frames without tearing leaves their statistics missing, so they complete as ESTIMATED and look shown. 8 is the frames in flight that begin() can estimate before they show (legit case 5 needs the slack; mutant `sg-13`). |
| Effect | Once per open, until close: every record from the firing flip on gets YSCR_FLIP_UNSYNCED and tier 3; a YSCR_EV_UNSYNCED ring record; WARNING=not-vsynced in the describe line; `yscr_sync_check()` returns the rule, the frame, the evidence and a message that names the settings | The onsets are not measurements. The run goes on, as for `desc.min_tier`: only the caller knows what to do with its trials. |
| Opt-out | None | The header never presents with vsync off on purpose, and a legitimate path gives no evidence. |

The flags field of the ring's mode word has 10 bits, so YSCR_FLIP_UNSYNCED
(0x1000) is not in it, as for CODE_AT_RISK and TEXT_INPUT; the tier
there is 3, and the YSCR_EV_UNSYNCED record marks the flip.

### What the core test plays

`tests/adapt/screen_test.c` plays a driver with vsync forced off four
ways on its virtual clock. Each fires the guard once, by the rule its
evidence feeds:

| Scripted driver | Fires | Evidence |
|---|---|---|
| 1: each present flips at once, reported at the vblank of its own refresh (DXGI's vblank-stamped statistics) | FLIPS at frame 31 | no wait on every flip |
| 2: each present flips 0 to 3/4 of a period after the call, as the GPU finishes, reported at that time with the next vblank's count | FLIPS | torn, then same refresh and no wait as the grid follows the tears |
| 3: a backend that holds frames to their target, with the target ignored: 3 presents a refresh, each reported | FLIPS | same refresh |
| 4: as 3, only the newest present of a refresh reported (the others discarded) | RATE | under 32 of 64 per flip |

Then the legitimate paths, each 600 frames: reported times that stretch
after each late frame; a composed path with drops, held frames,
preemption and missing statistics; a backend that holds frames to their
target, with planned onsets, skipped presents and a path and depth that
flap; vblanks that themselves stretch, with held frames; a timestamp 30%
of a period early on one flip in 3 (evidence on a third of the flips,
under the threshold: mutant `sg-06` lowers it to 16 and fires); two
flips in flight with a statistic for one present in 3. None fires, and
all but the jittered one give no evidence at all. The SIM backend in
real time gives none. A counter on the fire hook proves that no other
case in the file fired it. Mutants `sg-01` to `sg-13`, caught.

### On this laptop

Windows 11, Iris Xe, 60 Hz, AC, no driver setting changed, 2026-10-09.
`screen_flipstats --frames 3600 --load 4 --overrun 25 --miss 120 --hold`
(load threads, overruns in 1 frame of 30, heavy GPU frames, held frames
0 to 4 vblanks ahead), 3600 flips each:

| Backend and window | Path | Drops | Off grid | Evidence: same refresh, no wait, torn | Most in one window of 64 |
|---|---|---|---|---|---|
| DXGI_FLIP fullscreen | overlay | 42 | 409 | 0, 3, 8 | 2 |
| DXGI_FLIP window (kept on top: overlay) | overlay | 40 | 433 | 0, 5, 13 | 3 |
| DXGI_FLIP fullscreen, a small window over a corner from frame 300 | overlay | 41 | 429 | 0, 2, 18 | 3 |
| COMPOSITION fullscreen | independent flip | 17 | 0 | 0, 0, 0 | 0 |
| COMPOSITION window | independent flip | 16 | 0 | 0, 0, 0 | 0 |
| COMPOSITION, covered corner, on top | independent flip | 16 | 0 | 0, 0, 0 | 0 |

`screen_sync_check --no-flicker --seconds 5`: DXGI_FLIP windowed
(composed, 11 path changes, 165 estimated flips), DXGI_FLIP fullscreen
(overlay) and COMPOSITION fullscreen (independent flip): 1.00 flips per
refresh, no evidence. The guard never fired on this machine. On
DXGI_FLIP the evidence came with the held frames, which v0.4.2 recorded
off the grid and a vblank early (EARLY on 285 to 313 flips a run). The
vblanks were not off the grid; DXGI's times were, and v0.4.2's grid
followed them. With v0.4.3 the same stress runs gave no guard evidence
(see [Held frames a vblank early](#held-frames-a-vblank-early-v043)).

### Not known

- Whether any driver panel overrides a flip-model `Present(1)`, on
  DXGI_FLIP or COMPOSITION. No panel setting was changed: the Intel
  setting is a hand test (below); AMD and NVIDIA need their hardware
  (the RTX A500 here has no output); Mesa needs the X11 backend.
- A driver that tears but reports the next vblank's count and time gives
  no evidence. `examples/screen/sync_check.c` shows the tear to the eye.
- Whether the Intel "Speed" mode (application-unaware triple buffering)
  discards frames under the header's paced loop. On DXGI_FLIP the core
  holds each present until its vblank's slot, so no two presents meet in
  one refresh, and nothing is lost.

### Hand test: the Intel setting

1. Run `screen_sync_check --no-flicker` (or without `--no-flicker` if no
   one with photosensitive epilepsy can see the screen). Expect "verdict:
   synced", an even pattern and a bar in one piece.
2. In Intel Graphics Software, set Vertical Sync for
   `screen_sync_check.exe` (or globally) to Off, then to Speed. Run step 1
   again for each, also with `--backend composition` and `--windowed`.
3. Record: the verdict and the guard's evidence, and whether the bar
   broke or bands crossed the screen. A torn screen with "verdict: synced"
   is the limit above.
4. Set Vertical Sync back to the application's choice.

## Held frames a vblank early (v0.4.3)

The v0.4.2 stress runs (previous section) showed held frames on DXGI_FLIP
off the grid and a vblank early, where v0.1 had measured 0 early in 1200
(see [Depth](#depth)). All runs below: this laptop, fullscreen unless
stated, AC power, `screen_flipstats --hold` (0 to 4 vblanks ahead,
targets up to 0.45 period off the grid), 1200 frames, idle unless stated,
2026-10-09.

### Regression or stress

Neither: idle holds alone give it, and so does every version in git.
The same loop, built on each header with that version's own
`screen_flipstats.c`:

| Header | Flips flagged EARLY | Held frames more than half a period before the vblank asked | Off-grid times |
|---|---|---|---|
| v0.2.0 (f71a1e4) | 226 | 72 | 347 |
| v0.3.0 (58232d8) | 229 | 41 | 282 |
| v0.3.1 (6d788ba) | 259 | 62 | 327 |
| v0.4.0 (3f975bb) | 242 | 58 | 334 |
| v0.4.2 | 187 | 26 | 219 |
| v0.4.2, no holds | 0 | (no holds) | 0 |

v0.1 is not in git, so it could not be built again. What changed since
its 0 in 1200 (the driver, the OS or the state of the machine) is not
known.

### Which stress element

With the probe below, v0.4.2, 1200 frames each:

| Condition | Early (by the scanout) | Off-grid times |
|---|---|---|
| 4 load threads, no holds | 0 | 0 |
| a 25 ms overrun in 1 frame of 30, no holds | 0 | 0 |
| 3 heavy GPU frames every 120, no holds | 24 | 0 |
| holds, idle | 63 | 177 |
| holds, window kept on top | 32 | 77 |
| holds, load, overruns and heavy frames together | 13 | 17 |

Holds give the off-grid times and most early flips. The heavy GPU frames
give early flips by another path, the depth rule (below).

### The probe

A copy of v0.4.2 that logged every DXGI statistic, every present (the
planned vblank, the depth, the wake time, the call and return times, the
grid at the wake) and every completion, and a thread that polled
`D3DKMTGetScanLine` and stamped each entry into vertical blank (the
independent vblank source of the COMPOSITION probe). Results of one idle
hold run (3730 scanout vblanks, 1200 flips):

- The scanout's vblanks were steady: intervals 16.608 to 16.725 ms, none
  more than 1% of a period off. No refresh was stretched.
- DXGI's `SyncQPCTime` was more than 0.5 ms off the nearest scanout
  vblank on 177 of 1199 statistics, by up to 8.3 ms (half a period),
  either way, in runs of 1 to 14 flips. Some were later than the time
  the statistic was read. In a run they sometimes drift back to the grid
  in small steps (4.89, 4.88, 4.81, 4.51 ms and so on), which looks like
  a stretched refresh but is not one.
- DXGI's `SyncRefreshCount` lost 146 of 3730 vblanks (about 1 in 25),
  always across a held frame.
- On all 1199, the scanout vblank nearest DXGI's time was the first one
  after the present call returned: the frame flipped where expected.

So the off-grid times and the lost counts are DXGI's errors, not the
display's. v0.4.2 anchored its grid on every reported time and count.
Against the header's own plan (its planned vblank, mapped to the nearest
scanout vblank):

| Outcome | Flips |
|---|---|
| Shown on the planned vblank, not flagged | 990 |
| Shown on the planned vblank, flagged EARLY (the count had lost a vblank) | 145 |
| Shown a vblank early, flagged EARLY | 63 |
| Shown a vblank early, not flagged | 0 |

On all 63 early flips the grid at the wake was more than 0.45 ms before
the scanout's vblank (planned - depth): the grid had moved to an
off-grid time, the hold woke before the real vblank, and the present
made it. The header's planning put them early, not DXGI's presentation.

Display and session: every probe run from then on logged the console
display state (`GUID_CONSOLE_DISPLAY_STATE`) and the session lock
(`WTSRegisterSessionNotification` and `WTSQuerySessionInformation`). The
display was on and the session unlocked from the start to the end of
every logged run, and the early flips came all the same. The runs before
the logging had a steady scanout throughout, which a display that is off
does not have. `--topmost` sends one zero-size mouse move at the start
to take the foreground.

### The fix

A flip's vblank is now the grid's nearest to the OS's time, not the OS's
count. A time within 1% of a period of the grid moves it; a time off it
is flagged GRID_UNSTABLE (tier 2), its onset is the grid's vblank time,
and the grid stays. 16 off-grid times in a row that agree on a new phase
move the grid there (DXGI's agreed on at most 5 in a row in five runs),
so a display whose timing changes is followed. When 48 of the last 64
times are off the grid, the grid follows each time with the OS's count,
as before (DXGI's held frames: at most 34 of 64). The sync guard gets
the OS's count in the grid's numbers.

`tests/adapt/screen_test.c` (`test_held_off_grid`) plays the hardware's
sequence on the virtual clock: lost counts, off-grid times up to 0.45
period either way, 3 in a row that agree, with holds; then a display
whose vblanks move 0.3 period at once. Mutants `eg-01` to `eg-09`, all
caught; `eg-02` (anchor on every time) is v0.4.2's behavior.

After the fix, probe on v0.4.3, scanout as reference:

| Run | Backend | Frames | On the vblank asked | Early | Dropped | Off-grid times (tier 2) | Guard evidence |
|---|---|---|---|---|---|---|---|
| holds, idle | DXGI_FLIP | 1200 | 1200 | 0 | 0 | 163 | 0 |
| holds, window kept on top | DXGI_FLIP | 1200 | 1196 | 5 (first 6 frames) | 0 | 191 | 0 |
| holds, load, overruns, heavy frames | DXGI_FLIP | 3600 | 3534 of 3581 | 3 | 44 (heavy frames) | 105 | 0 |
| same, window kept on top | DXGI_FLIP | 3600 | 3533 of 3580 | 5 | 42 (heavy frames) | 134 | 0 |
| holds, idle | COMPOSITION | 1200 | 1200 | 0 | 0 | 0 | 0 |
| holds, load, overruns, heavy frames | COMPOSITION | 3600 | 3565 of 3582 | 0 | 17 (heavy frames) | 0 | 0 |

The early flips left come from the depth rule, not the grid:

- In a window, the first frames run on the composed path at depth 2
  while the window goes to the overlay path ("Two frames planned for one
  vblank").
- After 3 heavy GPU frames in a row, each shown a vblank late, DXGI_FLIP
  takes depth 2 (three flips in a row at a new depth), and the next 3
  frames show a vblank early until three votes bring it back. It came
  once in 30 heavy episodes per run here. The GPU, not the swap path,
  made those 3 frames late. Fixed in v0.4.4: misses no longer change
  the depth (see [Depth from the path](#depth-from-the-path-v044)).

### AUTO, decided (v0.4.4)

Decided 2026-10-09 (user): AUTO picks COMPOSITION where it opens and the
presentation factory reports independent flip for the window's output,
else DXGI_FLIP, unless the 10-minute runs showed COMPOSITION worse on
prediction p99, drops, early or late flips, or tiers. They did not.

10 minutes each (36000 frames), idle, no holds, kept on top, AC, v0.4.4's
depth policy, the console display on and the session unlocked from
start to end, one after the other in one quiet session, 2026-10-09.
Prediction is the reported onset minus the predicted one, for frames
not late and not dropped. "Present call" is the `yscr.present` zone
(flush, patch and the present).

| Run | Backend | Paths | Prediction p50 / p99 / max, us | Dropped | Early | Late targets | Tier 1 / 2 / 3 | Present call mean / p99, us | Path changes after the first report | Depth changes after open |
|---|---|---|---|---|---|---|---|---|---|---|
| fullscreen | DXGI_FLIP | overlay 36000 | 0.62 / 1.88 / 14.05 | 0 | 0 | 0 | 36000 / 0 / 0 | 222 / 603 | 0 | 0 |
| fullscreen | COMPOSITION | independent 35998, none 2 | 0.62 / 1.92 / 4849.6 | 0 | 0 | 0 | 35998 / 0 / 2 | 463 / 1286 | 0 | 0 |
| window 800 x 600 | DXGI_FLIP | overlay 35991, composed 6, none 3 | 0.67 / 1.78 / 16666.8 | 1 | 4 | 0 | 35991 / 6 / 3 | 218 / 587 | 1 | 1 |
| window 800 x 600 | COMPOSITION | independent 35998, overlay 2 | 0.68 / 1.82 / 23.25 | 1 | 0 | 0 | 35998 / 0 / 2 | 464 / 1195 | 3 | 4 |

- Every record worse than tier 1, every early flip and every path change
  is in the first 10 frames of its run. COMPOSITION's 2 fullscreen
  records with no statistic are frames 0 and 1, while another window
  covered ours (the example takes the foreground after open); DXGI_FLIP
  gets statistics for a covered window, COMPOSITION does not. COMPOSITION's
  4.85 ms maximum is one frame of 35998.
- The p99 differs by 0.04 us, as much as two runs of one backend did
  before ([The fullscreen regression](#the-fullscreen-regression)).
- The present call costs COMPOSITION twice as much: 463 against 222 us
  mean, 1.2 to 1.3 ms against 0.6 ms at p99.

The DXGI_FLIP window run predates the early-flip rule of
[Depth from the path](#depth-from-the-path-v044); its 4 early flips are
the window's move to the overlay at the start.

Caveats that remain:

- The present call costs about twice as much (above), from the frame's
  16.7 ms.
- Windows 11 x64 only; elsewhere AUTO is DXGI_FLIP, and the describe
  line says why.
- On COMPOSITION a composed or overlay frame's onset is DWM's plan (tier
  3), where DXGI_FLIP's composed onset is tier 2. AUTO takes COMPOSITION
  only where independent flip is supported, but the path can still move.
- A covered window gets no statistic on COMPOSITION (ESTIMATED,
  OCCLUDED, tier 3).
- One machine: the Iris Xe at 60 Hz. No photodiode run: every onset here
  is the OS's time, not light.

### Display off and a locked session

Not measured: what DXGI_FLIP and COMPOSITION do while the display is off
or the session is locked (presents shown or not, vblank times simulated,
OCCLUDED). A rig can blank its display in the middle of a session.
Proposal: the header registers for `GUID_CONSOLE_DISPLAY_STATE` and
session lock notices on its window, writes a ring record at each change,
and gives the flips planned meanwhile a flag and tier 3.
Decided 2026-10-09 (user): not built. A rig runs a session on an awake
machine; the case matters only to unattended measurement runs like the
ones above, which log the display and lock state themselves.

### Hand test: display off and lock

1. Set the display to turn off after 1 minute on AC.
2. Run `screen_flipstats --backend dxgi --frames 7200 --hold --csv a.csv`
   and do not touch the machine; the display turns off after 1 minute.
3. Run it again and lock the session (Windows+L) after 30 s, unlock after
   30 s.
4. Record the paths, flags, tiers and drops of the records over the off
   or locked interval, against the time of the change.

## Depth from the path (v0.4.4)

Decided by the user on 2026-10-09: a depth that rises after misses adds a
vblank of latency from input to photon in the middle of a task, and the
task did not ask for it. The depth now comes from the path, never from
misses. All runs below: this laptop, AC, fullscreen unless stated, the
console display on and the session unlocked from start to end of every
run (logged with `GUID_CONSOLE_DISPLAY_STATE` and the WTS session state),
2026-10-09.

### The policy

| Case | Depth |
|---|---|
| open, DXGI_FLIP | measured: black frames until three flips in a row agree (as before), but never above the smallest depth that 2 of them showed on the last path |
| open, COMPOSITION | the path's (v0.4.3: 1 on every path) |
| a path change | the new path's: the depth measured at open on that path, else independent flip 1, DXGI overlay 1, COMPOSITION overlay 2, composed 2; an unknown path keeps it |
| a miss | none: a drop, flagged and counted, its tier as before |
| DXGI_FLIP: a flip not held, on the grid, shown early | the depth it showed (lower only); the path keeps at least its table depth |
| `desc.depth = N` | N from open on: no measurement, no path following |
| `desc.depth_learn` | v0.4.3's rule (opt-in, not with a pin): see below |

Each change writes a `YSCR_EV_DEPTH` ring record (old, new, reason: open,
path, pin, learner or early; the path; the frame whose record brought it), and
the describe line reads `depth=2(path,3 changes)`.

**A pin on a path that needs more.** Every flip shows a vblank after the
one planned: each record has `dropped` 1 and `f.onset` is a period early,
so the timeline is a frame off until the pin changes. On DXGI_FLIP the
loop still runs at one frame per vblank; on COMPOSITION, where the slot
frees at the flip, it can run at half rate (the core test's case 1). **A
pin above the path's depth** adds that latency and shows every frame on
time: on DXGI_FLIP the hold wakes at the path's depth, not the pin, and a
trigger is armed by the path's depth.

**COMPOSITION.** v0.4.3 opened at 1 on every path and let three misses
raise it; once at 2 on the independent-flip path, the loop ran at half
rate until slack evidence lowered it (see
[Depth on a backend that holds frames](#depth-on-a-backend-that-holds-frames)).
Now no miss changes the depth and the independent-flip path is always 1,
so that fault cannot come back. On the composed path (2) the loop runs at
full rate, because DWM's statistic for a composed frame arrives about
15 ms before its vblank (see
[Is the onset an observation?](#is-the-onset-an-observation-condition-c)).
The overlay entry (2) rests on 2 of 2 COMPOSITION overlay frames that
showed on the vblank after next at depth 1 (see [Latency](#latency)). On
a backend that holds frames a depth too large costs latency only and one
too small drops every frame, so 2 is the safe side. In the windowed
COMPOSITION runs below, the path went to overlay for 2 or 3 frames at the
start and came back; the depth followed it both ways, with no drop.

### Early flips lower the depth (DXGI_FLIP)

The video worker ran this policy in 39 runs (docs/video.md, "Rerun: the
early flips"), scanout as reference. In a window in the foreground the
system moved the window from the composed path to an overlay plane 1.7
to 1.9 s after open; presents then showed at depth 1, but
`GetFrameStatisticsMedia` reported "composed" for 5 more flips, so the
path rule waited for the report: 6 early flips per run (v0.4.3's vote: 3).
In 3 of 9 windowed runs open() learned 3 on the composed path, and
nothing lowered it until the path changed: 108 to 116 early flips.

DXGI shows a present at the first vblank it can, so a flip that was not
held and showed early proves the depth too high. Such a flip, at an OS
time on the grid (an off-grid DXGI time can map to the vblank before),
now lowers the depth to what it showed. It only lowers, so it adds no
latency. The path's remembered depth keeps at least its table depth,
because the report may lag the flip. open() takes the smallest depth that
2 of its flips showed on the last path: a flip that waited behind the one
before it shows "this depth or less", and three such in a row gave 3.

Windowed DXGI_FLIP after the change, kept on top, scanout as reference:

| Run | Runs | Open's depth | Flips flagged EARLY per run | Where |
|---|---|---|---|---|
| no holds, 600 frames | 6 | 2 in 6 | 1, 1, 1, 1, 0, 1 | frames 6 to 8, the first after the move; then the depth fell |
| holds, 1200 frames | 3 | 2 in 3 | 2, 2, 0 | frames 3 and 4 (held flips do not lower it) |
| holds, load, overruns, heavy frames, 3600 frames | 1 | 2 | 3 | frames 3 to 5 |

open() did not learn 3 in these 10 runs, so the open rule is shown only
on the virtual clock (`test_depth_early`, case 2); case 1 plays an open
at 3 on a display at 2: 1 or 2 early flips, not one per frame. The
scanout analysis also marks frames 0 to 6 early without a flag while the
window is still composed; it assumes a frame shows on the first vblank
after its present, which a composed frame does not.

### The learner: kept, opt-in

The user's rule was to keep the miss-driven rule only if a measurement
showed a case where it beats the default. `screen_flipstats --gpu N`
gives every frame N full-screen clears (steady GPU work); 900 and 1100
clears put the work near one period here. 600 frames per run, 3 runs per
cell (1 for 700), interleaved:

| Backend | GPU work | Default: dropped | Learner: dropped (+ early) | `desc.depth = 2`: dropped |
|---|---|---|---|---|
| DXGI_FLIP | 700 clears | 4 | 5 (+5) | 2 |
| DXGI_FLIP | 900 clears | 11, 46, 3 | 2 (+4), 4 (+3), 1 (+6) | 2, 0, 5 |
| DXGI_FLIP | 1100 clears | 5, 8, 4 (+4) | 3 (+4), 4 (+4), 2 (+4) | 1, 2, 2 |
| COMPOSITION | 700 clears | 3 | 3 | 0 |
| COMPOSITION | 900 clears | 6, 57, 49 | 3, 2, 3 | 46, 0, 0 |
| COMPOSITION | 1100 clears | 50, 28, 5 | 3, 6, 3 | 0, 36, 2 |

So it does beat the default there: with GPU work close to a period, the
default dropped up to 1 frame in 10 and the learner, at depth 2, 1 in 100
or fewer. A pin of 2 did as well in 12 of 14 runs, but dropped 36 and 46
in two COMPOSITION runs, where the learner (at 2) dropped at most 6. On
DXGI_FLIP the learner's early flips are at the start: after the window's
path change, it adopted 3 or 4 from one flip.

Where it costs: short runs of misses. `--miss 120` (3 frames of about
25 ms of GPU work every 120), 1200 frames, and the v0.4.3 stress run
(holds, 4 load threads, a 25 ms overrun in 1 of 30, `--miss 120`), 3600
frames:

| Run | Backend | Rule | Heavy frames dropped | Other frames dropped | Early | Depth changes | Other frames 2 or more vblanks after the one before |
|---|---|---|---|---|---|---|---|
| misses | DXGI_FLIP | default | 26 of 30 | 0 | 0 | 0 | 0 |
| misses | DXGI_FLIP | learner | 30 of 30 | 0 | 30 | 20 | 10 |
| misses | COMPOSITION | default | 28 of 30 | 0 | 0 | 0 | 0 |
| misses | COMPOSITION | learner | 30 of 30 | 0 | 0 | 19 | 29 |
| stress | DXGI_FLIP | default | 42 of 90 | 0 | 0 | 0 | (holds) |
| stress | DXGI_FLIP | learner | 42 of 90 | 0 | 4 | 3 | (holds) |
| stress | COMPOSITION | default | 16 of 90 | 0 | 0 | 0 | (holds) |
| stress | COMPOSITION | learner | 17 of 90 | 0 | 0 | 0 | (holds) |

The learner is kept as `desc.depth_learn`, off by default: it is the
choice for a loop whose GPU work per frame is close to a period and that
accepts a depth (and latency) that moves; a loop that knows it needs 2
can pin it, with the two COMPOSITION outliers above as the caveat.

### The stress runs again

v0.4.3's runs (see [Held frames a vblank early](#held-frames-a-vblank-early-v043))
on v0.4.4 before the early-flip rule above (the windowed DXGI_FLIP
runs after it are in that section), the same probe build with the
scanout source. "Early" is by
the scanout's own vblanks (D3DKMTGetScanLine) on DXGI_FLIP and by the
records on COMPOSITION, whose present waits for its target. "Window" is
800 x 600 kept on top. Before: v0.4.3's table; its depth changes from
the probe's logs.

| Run | Backend | Frames | On the vblank asked, before / after | Early, before / after | Dropped, before / after | Off-grid (tier 2), before / after | Depth changes after open, before / after |
|---|---|---|---|---|---|---|---|
| holds, idle | DXGI_FLIP | 1200 | 1200 / 1200 | 0 / 0 | 0 / 0 | 163 / 171 | 0 / 0 |
| holds, window | DXGI_FLIP | 1200 | 1196 / 1195 | 5 / 4 | 0 / 0 | 191 / 265 | 1 / 1 |
| holds, load, overruns, heavy frames | DXGI_FLIP | 3600 | 3534 / 3537 | 3 / 1 | 44 / 43 | 105 / 186 | 2 / 1 |
| same, window | DXGI_FLIP | 3600 | 3533 / 3534 | 5 / 4 | 42 / 42 | 134 / 120 | 3 / 1 |
| holds, idle | COMPOSITION | 1200 | 1200 / 1198 of 1198 | 0 / 0 | 0 / 0 | 0 / 65 | 0 / 0 |
| holds, load, overruns, heavy frames | COMPOSITION | 3600 | 3565 / 3563 | 0 / 0 | 17 / 17 | 0 / 64 | 0 / 0 |
| holds, window | COMPOSITION | 1200 | not run / 1199 | - / 0 | - / 1 | - / 16 | - / 4 |
| holds, load, overruns, heavy frames, window | COMPOSITION | 3600 | not run / 3565 | - / 0 | - / 16 | - / 0 | - / 4 |

- Every early flip after v0.4.4 is in the first 6 frames, while the
  window moves from the composed path to the overlay (open's path change);
  the v0.4.3 runs had 3 more after the heavy frames (frames 2520 to 2522),
  from the depth rule. One record in the windowed DXGI stress run was
  flagged EARLY on an off-grid DXGI time where the scanout shows it on
  time.
- Every depth change after open is a path change at the start (DXGI:
  composed to overlay; COMPOSITION in a window: independent flip to
  overlay and back, twice).
- The off-grid records on COMPOSITION fullscreen are frames 3 to 66. The
  window was covered at open in these runs (the program takes the
  foreground after open), the first 2 statistics did not come, and the
  displayed times that followed were 4.7 ms after the scanout's vblank,
  coming back to it over about 3 s. The header flagged them; see
  [No OS time at open](#no-os-time-at-open-v044).

### No OS time at open (v0.4.4)

In the first learner batch, run right after the session was unlocked,
every COMPOSITION run got no statistic during open's black frames (0.7
s). open() then guessed the grid, and v0.4.3 kept the guess: every record
was tier 3 and the sync guard fired on flips that were on time (same
refresh 334 to 968 times). The same binary 10 minutes later, and v0.4.3
beside it, gave tier 1 and no evidence. Now the first OS time replaces the
guess, numbered as the guessed vblank nearest it, so frames planned on
the guess keep their numbers, and that one flip gives the guard no
evidence. `test_late_statistics` plays it on the virtual clock on both
kinds of backend; mutants `dp-15` to `dp-17`.

## Settling at open (v0.5.0)

Psychtoolbox's `OpenWindow` collects at least 50 valid flip intervals (up
to 5 s) and requires a small spread before it trusts the refresh. Up to
v0.4.4, `yscr_open()` presented black frames only until three flips
agreed on the depth (DXGI_FLIP, at most 40). The measurements below show
that the start of a run is not settled then. Decided 2026-10-09 (user):
`yscr_open()` settles the display, as described in SETTLE in the header.

### What the start of a run does

All runs: this laptop, AC, the guard held, the console display on and the
session unlocked in every run (logged per run), 2026-10-09. The probe
(`C:\tmp\psy-work\settle\settle.c`, not in the repository) is v0.4.4 with
two log hooks. It logs every OS statistic, every completed present with
its plan, every record, the grid at each `yscr_begin()`, the ring's path
and depth records, and the scanout's vblanks (`D3DKMTGetScanLine` on its
own thread). One open per process, then 5 s of frames at `f.onset`, idle,
no holds. "In front": the program sent one input event before the open, so
the window was in the foreground from its creation (20 opens per
configuration, interleaved; 5 more per configuration not kept on top gave
the same results). "Covered": started from the background, so the window
was under another window during open and came to the front after it (9
opens per configuration).

Time from the `yscr_open()` call to the moment each condition starts and
stays true to the end of the 5 s, in front, ms, median / p95 / max:

| Configuration | `yscr_open()` | Depth fixed | Path fixed | All flips tier 1 | No early, late or estimated record |
|---|---|---|---|---|---|
| DXGI_FLIP fullscreen | 322 / 419 / 469 | at the end of open | 205 / 302 / 352 | 255 / 352 / 402 | at the end of open |
| DXGI_FLIP window 800 x 600 | 306 / 456 / 562 | 504 / 582 / 627 | 554 / 631 / 661 | 554 / 631 / 661 | 537 / 615 / 661 |
| COMPOSITION fullscreen | 395 / 505 / 538 | at the end of open | 328 / 438 / 470 | 328 / 438 / 470 | 328 / 438 / 470 |
| COMPOSITION window | 460 / 531 / 532 | 566 / 647 / 648 | 566 / 647 / 648 | 477 / 564 / 565 | 469 / 547 / 548 |
| AUTO fullscreen (COMPOSITION) | 394 / 438 / 505 | at the end of open | 327 / 371 / 438 | 327 / 371 / 438 | 327 / 371 / 438 |

- The grid was anchored on an OS time during open in every run, within
  about 0.1 ms of the scanout (the scanout poll's own noise).
- DXGI_FLIP in a window: the system moved the window from the composed
  path to an overlay plane at 0.50 to 0.66 s. Each run had 1 EARLY, 1
  dropped and 3 ESTIMATED records in its first frames. The move at 1.7 to
  1.9 s of docs/video.md (decode load) did not come in 34 idle window
  opens.
- COMPOSITION in a window: after each independent-flip statistic, a
  second statistic for the same present came, as an overlay frame planned
  for the next vblank. v0.4.4 changed the path and the depth on each: 9 to
  13 depth changes and 13 to 17 path changes per open, on frames 0 to 5.
  The records stayed clean.
- Covered, COMPOSITION: no statistic during open, so open presented its
  cap of 40 black frames (2.2 s). Then the OS times were up to 7.6 ms off
  the scanout's vblanks and stayed GRID_UNSTABLE past 7 s in 4 of 9
  fullscreen runs. DXGI_FLIP gets statistics for a covered window: its
  covered runs settled as the ones in front did.

The spread of the vblank intervals (OS times, each interval divided by the
vblanks it spans), from the first OS time, over 100 opens in front:

| Intervals | SD, us, median / p95 / max | Mean minus the scanout's period, us, median / max |
|---|---|---|
| 5 | 4.1 / 13.1 / 16.6 | 2.09 / 14.36 |
| 10 | 6.0 / 12.6 / 16.6 | 1.48 / 9.62 |
| 20 | 5.5 / 10.1 / 12.6 | 1.64 / 5.36 |
| 50 | 4.6 / 7.2 / 8.4 | 0.80 / 2.74 |
| 200 | 2.7 / 3.9 / 4.4 | 0.29 / 0.60 |

Once settled, the SD of 50 intervals is about 1 us (p99 1.1 us). The 20
opens not kept on top had at most 13.5 us and 6.7 us at 20 intervals.
After a covered COMPOSITION open the SD of 50 intervals reached 170 to
1800 us. Psychtoolbox's default limit, 1 ms, would pass all of these; the
interval count is what costs time. So the rule takes 20 intervals (about
0.33 s at 60 Hz) and limits of 50 us and 10 us.

### The rule

| Condition | Value | Why |
|---|---|---|
| An OS vblank time | the grid is not a guess | A guessed grid made every record tier 3 (v0.4.4). |
| Clean flips in a row | 6 (`desc.settle_flips`): an OS statistic, on the grid, not early, not late, tier 1 fullscreen or 2 in a window, one path, one depth | Simulated on every run: 3 in a row settled 14 of 20 COMPOSITION window opens before a path flap; 6 or more never settled before a bad flip. |
| Spread | the last 20 intervals: SD at most 50 us, mean within 10 us of the mode's period | The table above: 4 times the largest SD in front, and well under the drift after a covered open. The mean catches a mode whose period is not the panel's. |
| Minimum time | 2.5 s in a window, none fullscreen (`desc.settle_min_ns`) | The move to an overlay at 1.7 to 1.9 s under decode load (docs/video.md). |
| Cap | 3 s fullscreen, 4 s in a window (`desc.settle_max_ns`), from the `yscr_open()` call | Fullscreen settled by 0.8 s in every simulated open in front. A window's cap is 1.5 s above its minimum, and above the slowest window seen (2.95 s, covered). |
| At the cap | AUTO: open fails fullscreen (STRICT), opens with a warning in a window (WARN); `desc.settle`, `YSP_SETTLE` | Decided 2026-10-09 (user). |

Every outcome is recorded, so a skipped or failed settling cannot pass
unseen (Psychtoolbox's `SkipSyncTests` is easy to leave on): the
describe line (`settle=0.66s(24 flips)`, `settle=SKIPPED(env
YSP_SETTLE=off)`, `settle=FAILED(grid,flip41,2/6-clean,warn)` with
`WARNING=not-settled`, `settle=ABORTED(...)`), one `YSCR_EV_SETTLE` ring
record, and `yscr_settle_check()`. The describe line goes into the header
of each example's data file. Settle frames are not records. The SIM
backend and a presenter without OS vblank times skip settling.

A statistic for a present that already completed is stale and changes no
path: the COMPOSITION window flap is gone (`test_stale_statistics`).

Simulated on the 120 opens in front, the rule settled every one and no
bad flip came after it: fullscreen DXGI_FLIP 555 / 652 / 702 ms (median /
p95 / max), COMPOSITION 661 / 772 / 804 ms, windows 1.01 s with the old
1 s minimum. Covered, COMPOSITION: 21 of 27 settled at 2.7 to 3.0 s, 6
reached the cap, and the rule named the spread or the grid.

### Tests

`tests/adapt/screen_test.c` plays each condition on the virtual clock:
a clean display (fullscreen settles at once; a window after 2.5 s; the
composed path settles a window and fails fullscreen on the tier),
statistics missing, drops, off-grid times, a flapping path, a period 15
us longer than the mode's (on the grid, so only the spread's mean sees
it), a driver that forces vsync off (the sync guard fires during
settling); each fails a STRICT open with an error that names it and opens
with WARN. Also a path change late in the black frames, YSP_SETTLE over
the desc and a bad value, the desc's limits, an abort during settling
(open succeeds, the first `yscr_begin()` reports it), SIM, and the stale
statistics. Mutants `st-01` to `st-20`, all caught.

### After: v0.5.0 on the same probe

The same probe on v0.5.0 (the log hooks copied into it again), the same
loop, 2026-10-09. 20 opens in front and 5 covered per configuration,
interleaved. "After open" counts the caller's records and the ring's path
and depth records from the end of `yscr_open()` to 5 s later.

| Configuration | `yscr_open()` in front, ms, median / p95 / max (v0.4.4) | Settled | After open: records early, late, estimated or off the grid | Path and depth changes after open |
|---|---|---|---|---|
| DXGI_FLIP fullscreen | 562 / 664 / 684 (322 / 419 / 469) | 20 of 20, 24 presents | 0 of 6020 (v0.4.4: 0) | 0 (v0.4.4: 0) |
| DXGI_FLIP window | 2508 / 2515 / 2516 (306 / 456 / 562) | 20 of 20, 128 to 139 presents | 0 of 6020 (v0.4.4: 96 in 20 opens) | 0 (v0.4.4: 1 and 1 per open) |
| COMPOSITION fullscreen | 673 / 826 / 845 (395 / 505 / 538) | 20 of 20, 25 presents | 0 of 6020 (0) | 0 (0) |
| COMPOSITION window | 2508 / 2516 / 2516 (460 / 531 / 532) | 20 of 20, 127 to 143 presents | 0 of 6020 (0) | 0 (v0.4.4: 8 to 12 of each per open) |
| AUTO fullscreen | 671 / 755 / 791 (394 / 438 / 505) | 20 of 20, 25 presents | 0 of 6020 (0) | 0 (0) |

Covered at open (started from the background; the window came to the
front only after `yscr_open()` returned):

| Configuration | Result |
|---|---|
| DXGI_FLIP fullscreen | 5 of 5 failed: "statistics", no flip had a statistic in 3 s |
| COMPOSITION and AUTO fullscreen | 8 of 8 covered opens failed: "statistics"; the 2 opens that were in front settled at 0.65 s |
| DXGI_FLIP window | 5 of 5 settled at 2.5 s (DXGI reports a covered window); when the window came to the front, 1 path and 1 depth change and 2 bad records per open |
| COMPOSITION window | 4 of 4 covered opens reached the 4 s cap (WARN, "statistics") and opened |

So a fullscreen program that starts with its window covered now fails
`yscr_open()` with an error that names the missing statistics and asks
whether the window is covered. v0.4.4 opened it and gave seconds of onsets
off the grid (COMPOSITION) or without statistics. A program started by a
person from a terminal in front gets the foreground; a scheduler, a remote
shell or a test runner does not. Such a launcher can give it with one input
event and `AllowSetForegroundWindow(ASFW_ANY)` before it starts the
program, as the examples with `--topmost` now do for themselves; or set
`YSP_SETTLE=warn`.

v0.5.1 forces the foreground of a fullscreen screen by default ([The
foreground](#the-foreground-v051)).

## The foreground (v0.5.1)

Windows gives the foreground, and with it the keyboard, only to some
programs: the foreground program and the programs it starts, the program
that got the last input, and a few more cases. Up to v0.5.0,
`yscr_open()` called `SDL_RaiseWindow()` once and did not check the
result. Decided 2026-10-10 (user): `desc.foreground` AUTO forces the raise
in fullscreen and honors the OS policy in a window; HONOR and FORCE set
it. The describe line and the settle record say how the window got the
foreground.

### Launches measured

All runs: this laptop, AC, the guard held, the session unlocked and the
display on in every run, the user idle for 0.6 to 1.3 s (the probe's own
input). The probe (`C:\tmp\psy-work\v051\fg.c`, not in the repository)
opens a fullscreen screen with the default settling, runs 150 frames and
closes. A second program (`victim.c`) has the foreground and a text box
with the keyboard focus before each open. 1 s after the open, one F24 key
goes to the foreground window, and one more after the close. "Forced" is
`SDL_HINT_FORCE_RAISEWINDOW`, set by the probe (phase 1) or by the header
(v0.5.1). The foreground lock time-out is the system's (2147483647 ms).

| Launch | Default policy | Forced |
|---|---|---|
| A program in front starts it (a terminal) | granted 5 of 5 | granted 5 of 5 |
| A background process starts it (the shell of a tool) | refused 6 of 7 | foreground 9 of 9 |
| A detached process (parent gone) starts it | refused 7 of 7 | foreground 9 of 9 |
| Task Scheduler, `/it`, a program without a console | refused 7 of 7 | foreground 7 of 7 |
| Task Scheduler, a console program, or a `.cmd` file | granted 10 of 10 | granted 10 of 10 |

The scheduler's console window took the foreground first, so the program
it started was granted. Over all launches from the background without a
console: refused 20 of 21 (phase 1 and v0.5.1 HONOR), and forcing got the
foreground 25 of 25.

When the OS refused:

- Settling passed in every run (0.49 to 0.57 s, 22 presents), every flip
  was an independent flip, tier 1, and no record was bad. So settling does
  not detect it.
- The other program's window and the taskbar were above the screen's
  window in the z-order.
- The F24 key went to the other program in all 20. The screen got no key
  event, so Shift+Esc would not reach it either.

When the raise was forced:

- The key went to the screen.
- After the close, the foreground went to a third program (here the code
  editor), not to the program that had it, in 9 of 9 phase-1 runs from the
  background. That program's text box kept no focus.
- With `SetForegroundWindow()` to the previous window at the close, while
  the screen still had the foreground, it went back in 15 of 15 runs, and
  the key reached it.

A window of 800 x 600 behaved the same (2 runs per cell): refused from
the background, settled at 2.5 s, the key to the other program.

### The rule

- AUTO: FORCE fullscreen, HONOR in a window. A fullscreen screen is the
  participant's whole display; a program that started it from a scheduler
  or a remote tool wants it in front. A window shares the desktop with
  the operator, so the OS policy stands.
- The header asks once with SDL's default. If the OS refuses and the mode
  is FORCE, it sets the hint, raises again and puts the hint back as it
  was (the caller's value, or unset). So "forced" in the record means the
  OS refused first.
- `close()` gives the foreground back to the window that had it before the
  open, but only after a forced raise, while the screen still has it, and
  while that window exists.
- A refused foreground does not fail `yscr_open()`. The describe line adds
  `WARNING=not-foreground`; settling decides whether the display is fit.

Checked again on v0.5.1, 24 launches of the matrix above with
`desc.foreground` HONOR and FORCE: every record said granted, forced or
refused as the OS did, and the foreground went back to the previous
window 24 of 24. AUTO from the background: 3 forced, 1 granted
fullscreen; refused in a window.

### Tests

`test_foreground` plays each outcome through a test seam
(`YSCR__FOREGROUND`): the describe token, the warning, `u.u32[8]` and
`[9]` of the settle record, `yscr_settle_check()`, the mode AUTO applies
and a bad `desc.foreground`. Mutants `fg-01` to `fg-06`, all caught. The
raise itself needs a display: hand tests below.

### Hand tests

1. Start `screen_flipstats` from a terminal in front: `foreground=granted`.
2. Start it with `schtasks /create /tn t /tr "<path>\screen_flipstats.exe
   --frames 300" /sc once /st 23:59 /it` and `schtasks /run /tn t` while
   another window is in front: `foreground=granted` (its console takes the
   foreground first).
3. Start it from a background process, for example
   `Start-Process -WindowStyle Hidden` in a script that a scheduler runs, or
   over a remote shell into the console session: `foreground=forced`; at
   the end the window that was in front has the keyboard again.
4. The same with `--windowed`: `foreground=refused` and
   `WARNING=not-foreground`.

## The first frame after a gap (v0.5.1)

ysp/gfx.h v0.11 found the first frame after a setup of 2 s or more late
or dropped in 21 of 22 runs (docs/gfx.md). The cause is not the program
or the GPU. It is the composition swapchain after a time without a
present.

### The probe

`C:\tmp\psy-work\v051\gap.c` (not in the repository) opens a fullscreen
screen (settled), then repeats items in a random order: 30 frames, a gap
of G seconds without a present, then frame M0 and 9 more. The scanout's
own vblanks are polled with `D3DKMTGetScanLine` on a thread. Modes of the
gap: sleep; the frame thread busy (as a shader compile); GPU work into an
offscreen target with `glFinish()` every 4 ms; black frames on every
vblank; one black frame every 100 or 250 ms; 1 or 2 black frames just
before M0; M0 targeted one vblank later; a thread in
`D3DKMTWaitForVerticalBlankEvent` during the gap. This laptop, AC,
2026-10-09 and 10, 4 to 6 repetitions per item.

M0 late or dropped:

| Gap, sleep | COMPOSITION | DXGI_FLIP |
|---|---|---|
| 0 to 50 ms | 0 of 26 | 0 of 17 |
| 67 ms | 6 of 6 | 0 of 3 |
| 83 ms to 3 s | 66 of 66 | 0 of 51 |

Other modes, COMPOSITION, gap 0.5 and 2.5 s:

| During the gap | M0 late or dropped |
|---|---|
| frame thread busy | 7 of 8 |
| GPU work, no present | 8 of 8 |
| a black frame every 250 ms / 100 ms | 7 of 8 / 8 of 8 |
| a black frame on every vblank | 0 of 8 |
| 1 or 2 black frames before M0 | 0 of 22 (the first black frame dropped instead) |
| M0 targeted one vblank later | 0 of 12 |
| a thread waiting on the vblank event | 0 of 12 |

Every late M0 was one vblank late (dropped 1), on independent flip, tier
1, with 10 to 16 ms between the present call's return and the planned
vblank; the present call took 0.4 to 0.9 ms, as on other frames. The
scanout's vblank intervals stayed within 1 to 50 us of the period through
every gap, and the vblanks after a gap were within 4 us of the grid of
those before it: the panel did not stop or shift (no self-refresh exit, no
refresh change), and the grid needs no new anchor. CPU and GPU load do not
matter; only the time without a present does, from about 4 vblanks. A
waiter on the display's vblank event during the gap removes it; so the
likely cause is that the vblank event (its interrupt) goes idle when
nothing waits on it, and the first present after that misses its target.
That cause is inferred, not observed. DXGI_FLIP waits on its own
frame-latency object, which may explain why it is not affected.

### The mitigation

| Option | Effect | Cost |
|---|---|---|
| Keep-alive frames | fixes it if every vblank | the caller's frame loop must run through setup |
| A warm frame before the first real one | fixes M0, the warm frame drops | a frame of black; the header cannot know the content |
| Plan the first frame after a gap one vblank later | fixes it | a frame of latency after each gap; a threshold that may differ elsewhere |
| A thread waiting on the vblank event | fixes it | a thread that wakes 60 times a second while the screen is open |

Kept: the waiter, on COMPOSITION only (IDLE WAKE in the header). It
changes no frame and no prediction. With it, on v0.5.1: 0 of 36 M0 late
or dropped after gaps of 67 ms to 2 s, 0 of 30 with the other modes. A
failed wait (display off) backs off 4 ms; `close()` waits 250 ms for the
thread and otherwise leaves it its state.

### A hang after settling

While checking this on the original case, every program that called
`ygfx_prime()` right after `ygfx_open()` hung in its `glReadPixels()` on
COMPOSITION (gfx_hello, gfx_trial and the probe; 20 of 20, with the v0.5.0
header too). Settling ended after a flip, so the back buffer bound at the
end of `yscr_open()` was the one just presented; the prime's output pass
drew into it and the GPU waited for the presentation to release it,
which no later present did. v0.4.4's warm-up ended holding a free slot,
which is why earlier probes did not hang. v0.5.1 holds a slot at the end
of settling too (`fg-07`). Then the original case, a cold program cache
(2.2 s of compiles after `yscr_open()`): frame 0 late or dropped in 0 of 8
gfxwarm runs with and without the prime (21 of 22 in v0.5.0's runs), and
in 2 of 25 runs of the compile probe (1 dropped, 1 a late target; with
and without the waiter alike, 1 of 12 each).

## Compiles during settling (v0.5.2)

`yscr_open()` settles for 0.5 to 0.8 s fullscreen and 2.5 s in a window;
then `ygfx_open()` compiles its 14 programs (2.2 s with a cold cache on
AC, 2.8 to 3.2 s on battery). ANGLE exposes `GL_KHR_parallel_shader_compile`
here and compiles on its own threads: `ygfx_open()` submits every program
before it asks for a status. A scratch probe of v0.5.1
(`C:\tmp\psy-work\v051\par.c`) submitted gfx's programs at the first
settle frame, let settling go on, and asked for the statuses after
`yscr_open()`. No program cache, 10 opens each, interleaved, AC, ms from
the `yscr_open()` call:

| | Settled | gfx ready, median (range) |
|---|---|---|
| Fullscreen, one after the other | 667 to 782 | 2900 (2854 to 3022; 1 run 5404) |
| Fullscreen, compiles during settling | 648 to 808 | 2700 (2604 to 3523; 1 run 5257) |
| Window, one after the other | 2503 to 2516 | 4683 (4628 to 4702; 1 run 7148) |
| Window, compiles during settling | 2505 to 2516 | 2730 (2662 to 3361; 1 run 5237) |

Decided 2026-10-10 (user): build it, for fullscreen and windows.

### The hook

`desc.on_context` (a function and its context pointer) is the whole API in
this header. `yscr_open()` calls it once, when the GL context exists and
before the warm-up's first present. ysp/gfx.h v0.11.1's
`ygfx_open_start()` goes in it: it submits every program and returns, and
`ygfx_open()` after `yscr_open()` waits for the statuses.

- A callback in each settle frame was the other candidate. Nothing has to
  run per frame (the compiles run on ANGLE's threads), so a call once is
  enough, and a per-frame call would let a caller's work change the load
  that settling measures.
- Settling as a state (the sketch below) would also give the time, but it
  moves the strict failure into the frame loop and every example must wait
  for it. The hook keeps "`yscr_open()` returned, so the display is
  settled".
- The hook's time is added to the cap. Without that, a hook that holds the
  thread (a compile without parallel compile, 2 s or more) would fail a
  strict fullscreen open on time it did not spend settling. The minimum
  stays from the `yscr_open()` call: the move of a window to an overlay
  counts from the window's creation.
- After the hook, `yscr_open()` binds the default framebuffer:
  `ygfx_open_start()` leaves its scene target bound, and the black frames
  of the warm-up and settling would clear that target and present a back
  buffer that nobody cleared.
- `yscr_open()` can fail after the hook (strict settling). Then the
  context is gone with every object the hook made: ysp/gfx.h's
  `ygfx_close()` frees its memory and deletes nothing, and `ygfx_open()`
  on the started handle fails and says so.

### Before or after the warm-up

The warm-up (6 or more black frames, about 0.2 s) measures the depth and
anchors the grid. All runs in this section: this laptop (Iris Xe, Docker's
ANGLE 2.1.23876), on battery (the guard logged it; AC was not available),
the guard held, the session unlocked and the console display on in every
run (logged by the probe), the window in front (one input event before the
open), 2026-10-10. The probe (`C:\tmp\psy-work\pc\ovl.c`, not in the
repository) opens a screen with the given backend, opens the gfx one after
the other or through the hook, primes it, and draws 120 to 180 frames of a
drifting gabor, counting records that are early, late, estimated, off the
grid or dropped. Cold cache: an empty file cache folder before each open.

Hook before against after the warm-up, cold cache, 5 opens each, ms from
the `yscr_open()` call, median (range):

| Configuration | Hook at, before / after | gfx ready, before | gfx ready, after |
|---|---|---|---|
| COMPOSITION fullscreen | 225 to 256 / 464 to 596 | 3484 (3297 to 3576) | 3703 (3563 to 3883) |
| COMPOSITION window | 212 to 333 / 438 to 463 | 3423 (3366 to 4042) | 3778 (3589 to 3929) |
| DXGI_FLIP fullscreen | 229 to 323 / 399 to 430 | 3645 (3357 to 3811) | 3537 (3494 to 3705) |
| DXGI_FLIP window | 214 to 263 / 368 to 563 | 3432 (3321 to 3647) | 3696 (3548 to 3758) |

All 40 settled; each configuration ended on one path (independent flip on
COMPOSITION, overlay on DXGI_FLIP) at depth 1, before as after. Kept:
before the warm-up (0.2 s more for the compiles; DXGI_FLIP fullscreen's
difference is within its spread).

### Before and after v0.5.2

10 opens per row, interleaved; "one after the other" is `yscr_open()` then
`ygfx_open()`; "hook" is `ygfx_open_start()` in `desc.on_context`. Warm
cache: the folder a cold open filled. ms from the `yscr_open()` call,
median (range):

| Configuration | Cache | Settled at, one after the other | Settled at, hook | gfx ready, one after the other | gfx ready, hook |
|---|---|---|---|---|---|
| COMPOSITION fullscreen | cold | 782 (743 to 870) | 798 (760 to 844) | 3852 (3794 to 3971) | 3468 (3335 to 3636) |
| COMPOSITION fullscreen | warm | 781 (741 to 876) | 781 (762 to 828) | 805 (759 to 905) | 781 (762 to 828) |
| COMPOSITION window | cold | 2525 (2519 to 2527) | 2520 (2516 to 2532) | 5374 (5184 to 5614) | 3405 (3266 to 3588) |
| COMPOSITION window | warm | 2521 (2517 to 2530) | 2520 (2517 to 2530) | 2546 (2540 to 2554) | 2520 (2517 to 2530) |
| DXGI_FLIP fullscreen | cold | 708 (652 to 756) | 664 (653 to 750) | 3887 (3629 to 4145) | 3459 (3241 to 3626) |
| DXGI_FLIP fullscreen | warm | 695 (641 to 813) | 666 (648 to 741) | 717 (671 to 853) | 666 (648 to 741) |
| DXGI_FLIP window | cold | 2527 (2518 to 2532) | 2526 (2517 to 2530) | 5396 (5263 to 5609) | 3407 (3301 to 3685) |
| DXGI_FLIP window | warm | 2528 (2523 to 2532) | 2524 (2517 to 2532) | 2553 (2549 to 2560) | 2524 (2517 to 2532) |

- Settling: 160 of 160 settled. The presents per open were the same with
  and without the compiles (fullscreen 25 on COMPOSITION and 24 on
  DXGI_FLIP in every open; windows 126 to 138), so were the clean flips in
  a row at the end (16 and 14 fullscreen; 115 to 125 in a window), the
  path and the depth (one per configuration, depth 1). The spread's SD at
  the end: at most 11.3 us one after the other and 25.1 us with the
  compiles (1 DXGI_FLIP fullscreen open; the next largest 16.7 us); the
  limit is 50 us.
- Cold cache: the compiles take 3.1 s on battery, so in a window they end
  0.9 s after the 2.5 s minimum (2.0 s gained) and fullscreen 0.4 s is
  hidden. On AC (below) they end within 0.04 s of the minimum.
- Warm cache: the 14 loads (16 to 40 ms) run in the hook, before the
  warm-up; `ygfx_open()` then returns at once. 24 to 51 ms gained.
- `ygfx_prime()` after it: 49 to 107 ms in every condition.
- After the open: frame 0 was dropped (one vblank late) in 3 of 80 hook
  opens and 8 of 80 opens one after the other, on both backends, with a
  cold and a warm cache; these 11 were the only bad records of 28640.
  Not studied further here.

On AC (the laptop was plugged in later the same morning), cold cache, 5
opens per row, interleaved, the same probe:

| Configuration | Settled at, one after the other | Settled at, hook | gfx ready, one after the other | gfx ready, hook |
|---|---|---|---|---|
| COMPOSITION fullscreen | 662 (651 to 696) | 680 (668 to 685) | 2908 (2901 to 3257) | 2466 (2447 to 2638) |
| COMPOSITION window | 2524 (2516 to 2525) | 2528 (2517 to 2531) | 4832 (4712 to 5027) | 2567 (2561 to 2572) |
| DXGI_FLIP fullscreen | 570 (550 to 610) | 568 (564 to 584) | 2865 (2774 to 2989) | 2459 (2383 to 2619) |
| DXGI_FLIP window | 2527 (2522 to 2532) | 2528 (2520 to 2532) | 4787 (4733 to 4815) | 2567 (2561 to 2572) |

All 40 settled, with the same presents per configuration (24 or 25
fullscreen, 135 to 143 in a window). After the open: frame 0 dropped or
late in 1 hook open and 2 opens one after the other; in 1 DXGI_FLIP window
open one after the other, frames 0 to 16 were off the grid (tier 2) after
the 2.3 s without a present. With the hook, a window's `ygfx_open()`
returns 34 to 52 ms after `yscr_open()`, so that gap is gone.

### Without parallel compile

`ygfx_open_start()` submits only when the backend reports
`caps.parallel_compile` (the extension in the GL context's list).
Otherwise it makes nothing, and `ygfx_open()` does the whole open after
settling. A test seam hides the extension; COMPOSITION, cold cache, 5
opens each, ms from the `yscr_open()` call, median (range):

| | gfx ready, one after the other | gfx ready, hook without the extension |
|---|---|---|
| Fullscreen | 3802 (3735 to 3871) | 4073 (3752 to 4256) |
| Window | 5348 (5270 to 5583) | 5297 (5278 to 5354) |

The ranges overlap. The describe line says
`open_start=serial(no GL_KHR_parallel_shader_compile)`, and the settle
token shows `hook=0.000s`.

### Tests

`test_context_hook` (`tests/adapt/screen_test.c`, virtual clock): called
once, with the screen open, before the first present; a hook of 2.95 s
still settles fullscreen (the cap is 5.95 s); in a window the minimum
stays 2.5 s from the call; an open that fails at the cap after the hook
closes the presenter; settle OFF and SIM call it; an open refused before
the context does not; the describe token and `hook_ns`. ysp/gfx.h's GL
test `v0.11.1 start` reopens its headless screen with the hook and checks
the default framebuffer is bound after `yscr_open()`. Mutants `ch-01` to
`ch-07`, all caught.

### Settling as a state (sketch; not built)

`yscr_open()` would return after the warm-up; `yscr_begin()` would hand
out frames with `f.settling` set until the rule holds, and then report the
outcome (a strict failure as an error from `yscr_begin()`). The caller
compiles, uploads and loads the pack, and may draw a loading screen,
meanwhile.

- For callers: every loop must check `f.settling` and not start a trial;
  `yscr_open()` no longer proves the display fit; the strict failure moves
  from open to the frame loop; the describe line is final only after
  settling. Settle frames are not records, so `f.index` must not count
  them, and `f.done` must hide them, as now.
- For the examples: each one's first trial must wait for settling, and
  the data header written at open must be written after it.
- Risks: content drawn during settling changes the GPU load that settling
  measures (a heavy loading screen could fail it or pass it under load
  that the trials do not have); a caller that ignores the flag runs trials
  on an unsettled display; ysp/timeline.h anchors must not use settle
  frames; the abort path and the panic watchdog run in a new state.

## A message for every failure, ns in the desc (v0.6.0)

v0.6.0 applies the repository conventions (README.md, Conventions) to this
header. Three decisions:

- **Every failing call on a screen writes `yscr_error()`.** Before,
  only `yscr_open()` and `yscr_text_input()` wrote it. `yscr_begin()`
  returned `YSCR_ERR_TIMEOUT` or `YSCR_ERR_LOST` from the swap path with
  an empty message, and about 10 examples print `yscr_error()` after a
  failed begin. Each public call now runs inside a wrapper: it hides the
  first byte of the message, runs the call, and then either keeps the
  message the call wrote, writes "ysp_screen: <call>: <code's text>", or
  puts the byte back after a success. The cost is two byte writes per
  call, also in the frame loop; `snprintf()` runs only on failure. The
  group calls write into the screen at fault, or into the first screen
  when no screen is at fault. `yscr_native()` now takes a non-const
  screen so that it can write. No call on a screen is for another
  thread (MEMORY AND THREADS), so no call is an exception to the rule.
- **The panic times are `int64_t` ns.** `desc.panic_window_ms` and
  `panic_grace_ms` were `int32_t` ms. They are now `panic_window_ns` and
  `panic_grace_ns`, so old code that sets the ms names fails to compile
  and does not wait a million times shorter. `desc.trigger_spin_ns` was
  `uint32_t`; it is `int64_t`, as `yrt_worker_desc.spin_ns` is.
- **Kept.** `yscr_restamp()`'s `sdl_ticks_ns` is SDL's clock, not the
  ysp_rt clock, and keeps its `uint64_t`. `yscr_record.phase_ns` is a
  `uint32_t` that the caller only reads (a phase of one frame, with
  `YSCR_PHASE_UNKNOWN` as its sentinel): changing it would change the
  record's size and the ring's layout for no caller benefit.
  `desc.lead` is a fraction of a period, not a time.

Checked on 2026-10-10 (Windows 11, MSVC 19.44 /W4 /WX, and MinGW-w64 gcc
-std=c99 and -std=c++17 -pedantic for USAGE): `tests/adapt/screen_test.c`
checks the message after each failing begin, flip, wait_flip, code,
trigger_at and begin_group, that a success keeps the last failure's
message, and the refusal of a negative `panic_grace_ns`.
`tests/compile/screen.c` names the calls that write the message by type.
Mutants cv-01 (a failure that writes nothing) and cv-02 (a success that
drops the message) are caught.

## Enum values in the parameter table (v0.9.0)

Every ysp parameter table has one row shape: `name`, `type`, `min`, `max`, `def`, `unit`, `doc`, `values` ([conventions.md](conventions.md), "Parameter tables"; gap G6 of [experiment_format.md](experiment_format.md)). `yscr_param` gained `values` as its last field. ysp/gfx.h's
`ygfx_desc_params()` and ysp/video.h's `yvid_params()` return
`yscr_param` too, so their enum rows have values from gfx v0.17.0 and
video v0.5.0.

| Row | values |
|---|---|
| `backend` | `auto=0,dxgi_flip=1,composition=2,glx_oml=3,wayland=4,metal=5,web=6,sim=7,custom=8` |
| `patch.corner` | `top_left=0,top_right=1,bottom_left=2,bottom_right=3` |
| `settle` | `auto=0,strict=1,warn=2,off=3` |

The doc text of `backend` listed 0, 1, 2, 7 and 8, and its range said 0
to 8, so a reader could not tell whether 3 to 6 exist. They do: the C
enum has them, and `open()` refuses the stubs (at v0.9.0 GLX_OML, WAYLAND
and METAL; GLX_OML is built since v0.17.0, numbered v0.11.0 before the merge with v0.16.1) with a message and WEB outside Emscripten. `values` lists the C enum,
not what opens here; the doc says which are stubs. `patch.corner` and
`settle` are `YSCR_` macros, not a C enum, so the test's names of them
need a case by review when a macro is added.

`key_dedup_ns`'s default changed from 0 to 5e7: a zero field filters a
key's second report within 50 ms, and a designer that showed 0 suggested
no filter. The other zero defaults are the zero itself or a sentinel the
doc names (`display` 0 is the primary display, `depth` 0 the path's).

The test (`test_params()` in `tests/adapt/screen_test.c`) opens a SIM
screen with a zero desc and compares each default with what the screen
holds (`lead`, the patch size, the abort key and modifiers, the settle
flips, minimum and cap, the SIM period) or with the constant open puts
in where the screen keeps none (`window_w` 800, `window_h` 600,
`refresh_den` 1, the panic presses, window and grace, the key filter's
`YSCR__KEY_DEDUP_NS`). 4 of 4 new mutants killed; 2 more, run against `tests/adapt/param_tables_test.c`, show that the cross-header check sees a name twice and a default that is not a value (`tests/mutate/screen.toml`, pv-).

## SIM on a virtual clock (v0.8.0)

`--sim` self-checks failed on loaded CI runners (macOS most often): the
SIM presenter sleeps to a vblank grid on the real clock, so a slow host
dropped whole frames, and a check that compared with the script failed
or was widened to allow the drop. Ten examples were widened that way:
`trial_adjustment`, `trial_srt`, `trial_stroop`, `trial_rdk`,
`trial_same_different`, `trial_sternberg`, `trial_mouse_tracking`,
`web_av_sync`, `audio_av_sync` and `audio_schedule`.

The note that stood here (v0.5.1) judged the change not contained: the
core reads its clock at about 60 places through one compile-time macro,
and three examples compare audio onsets from miniaudio's real-time null
device with flip onsets. The built design does not touch those 60
places. It moves the clock under them:

- **ysp/rt.h v0.9.0 owns the virtual clock.** `yrt_virtual_begin()`
  makes `yrt_now_ns()` virtual for the whole process. The thread that
  called it is the driver: its `yrt_sleep_until()` jumps the clock to the
  deadline at once. Other threads wait in real time until the driver gets
  there. Virtual timers run on the driver, in time order, inside its
  waits. rt.h's VIRTUAL CLOCK has the rules.
- **The SIM presenter needs no change.** Its sleep to the vblank is a
  `yrt_sleep_until()`, which on the virtual clock is the jump. So the
  core's prediction, `flip_at()`, the waits and the ring stamps all read
  one clock, and no frame can drop. A CUSTOM presenter that times itself
  with `yrt_now_ns()` (`screen_sync_check --sim-vsync-off`) runs on it
  too.
- **The screen refuses a real backend on the virtual clock.** A display
  flips on real time; its records against a virtual prediction would be
  nonsense. The describe line says `clock=virtual`, so a log of a dry run
  cannot pass for a measurement.
- **The audio null device is virtual too** (ysp/audio.h v0.6.0). On the
  virtual clock `YAU_BACKEND_NULL` is the header's own device: a virtual
  timer renders one block a period at the block's own time. The fit is
  exact, and an onset is on its plan within half a sample.
- **Video decodes inline** (ysp/video.h v0.4.0). A decode thread on real
  time could not keep up with a frame loop that never waits: `video_play
  --sim` showed 6 frames and dropped 54, a different count each run.
- **Costs stay real.** A record's phases (DRAW, UPLOAD, SWAP) are costs:
  they read `yrt_real_ns()`, the real counter on either clock, through
  the seam `YSCR__COST_NOW()`. Through `yrt_now_ns()` they read 0 for
  DRAW and the 16.7 ms jump for SWAP. The instrumentation zones are real
  too, so `screen_flipstats --sim` reports the header's CPU cost and the
  draw phase in real microseconds. Times that feed a plan or a record
  (onsets, the prediction, the flip's call and return) stay on
  `yrt_now_ns()`.

The examples call `yrt_virtual_begin(0)` right after they parse `--sim`.
The checks that allowed whole frames are exact again: an onset is the
plan, an RT is the script's to the nanosecond (or within 1 us where the
script times from a plan that is off the 16666667 ns grid), and an audio
onset is on its plan within half a sample. `trial_mouse_tracking
--raw-mice` (CI run 38410242460: init 0.2247 s against 0.2913 s without
`--raw-mice`) now gives the init of the sample that passes 10 px, exactly,
with and without raw counts.

Determinism, measured on 2026-10-14 on the Windows 11 test laptop (Iris
Xe, 16 logical CPUs): the CI list of 49 `--sim` and `--null` runs.

| Build | Runs | Exit codes as CI expects | Output of the virtual-clock runs |
|---|---|---|---|
| MSVC 19.44 Release | 2 idle, 1 beside 24 CPU-bound processes (`burn.exe`) | 49 of 49 in each | data files and standard output identical in all 3 |
| MinGW-w64 gcc 16.1 Release | 1 idle | 49 of 49 | data files and standard output the same as MSVC's (paths aside) |
| WSL2 Ubuntu 22.04, gcc 11.4, SDL 3.4.0 console | 2 idle | 49 of 49 in each | identical between the 2 (paths aside) |

The lines that differ between runs print a real cost (the phases and
header costs of `screen_flipstats`, the build, draw, decode and upload
times of the gfx and video examples, the battery level), and the frames
of `gfx_capture`, which stays on the real clock: its writer thread is
real, and on the virtual clock the frame loop outran it (6 of 21 chosen
frames read, against 21 of 21 on the real clock). Every other run but
`trials_save_rows` (it reads no clock) is on the virtual clock. The MSVC
list took 25 s idle and 29 s under the load.

Tests: `test_sim_virtual` in `tests/adapt/screen_test.c` runs 120 frames
of SIM on the virtual clock through the header's own clock calls: every
onset on the grid and on the prediction, no drop, no late target, a real
backend refused. Mutants sv-01 (a real backend opens) and sv-02 (the
describe line hides the clock) are caught.

## The web backend (v0.7.0)

`YSCR_BACKEND_WEB` is the presenter for a browser: SDL3's Emscripten
canvas, a WebGL2 context the header makes, frames paced by awaited
`requestAnimationFrame` callbacks. [web.md](web.md) has the how-to guides,
the measurements and what a browser can claim; this section has the
design decisions that touch the core.

- **Tier and path.** A browser observes no flip, so every web record is
  `YSCR_TIER_WEB` (5), below the simulated display, with
  `YSCR_PATH_BROWSER` and `ONSET_PLANNED`. Two new enum values; nothing
  else in the record changes.
- **The onset.** The first callback time after both the callback that
  drew the frame and the present call (`yscr__web_shown()`). The callback
  time alone was 1.3 ms before the present call after a task that ran past
  a vsync (headless Chrome 154).
- **No sleeps.** The presenter sets `native_target`, so `flip_at()` never
  waits; a wait on the browser's main thread is a busy loop that stops
  rendering. It cannot hold a drawn frame either (the drawing buffer is not
  preserved), so a later target is recorded `EARLY`.
- **Planning.** On the web the next frame is planned from `yscr_begin()`'s
  own prediction, not a vblank after the last plan: a frame planned late
  shows at the next browser frame all the same, and the rule "never the
  vblank the last frame was planned for" then kept every later frame a
  vblank ahead (598 of 600 records `EARLY` in one headless run).
- **Stale callbacks.** `acquire()` and `bind()` skip a callback that ran
  more than half a period ago: a frame drawn there was replaced by the next
  callback's draw before it was composited.
- **Settling.** The presenter's own rule on the callback intervals, warn
  only; the core's rule needs OS vblank statistics, which a browser has
  none of.
- **Tests.** `yscr__web_shown()` and `yscr__web_steady()` compile on every
  platform, so `tests/adapt/screen_test.c` checks them; mutants wb-01 to
  wb-04. The presenter itself runs only in a page: the demos ran in Chrome
  154 (headless and headed), Edge 155 and Firefox 154 to 157 (headless).
- **Input stamps (v0.7.1).** `yscr_event_input()` on the web backend
  replaces SDL's handler time of a key, mouse button, touch or pen press
  or release with the DOM event's `timeStamp` where a capture listener
  recorded it. The match is by the time the listener ran, which is in the
  same task as SDL's handler and just before it, so it needs no key or
  button mapping between the DOM and SDL. `yscr_poll()`'s time stays
  SDL's: a program that reads SDL's events itself (the gallery) keeps what
  SDL gives. The matching is JavaScript and has no native test or mutant;
  `selftest.html` reports, per run, how many events got the DOM's time
  ([web.md](web.md), "Input: touch, the mouse and the DOM's time").
- **Fullscreen (v0.7.2).** The page owns fullscreen; the header never asks
  and now keeps SDL from leaving it. SDL 3.4.2's port applies a window's
  flags at create, destroy and a show, and its driver calls
  `emscripten_exit_fullscreen()` whenever the document is fullscreen and
  the window is not flagged fullscreen. Flagging the window fullscreen was
  not tried: SDL's resize handler sizes a fullscreen window's canvas to
  the page (`Emscripten_HandleResize()`), and the demos keep the drawing
  buffer they opened with. So the document's `exitFullscreen`
  is a no-op during `SDL_CreateWindowWithProperties()` and
  `SDL_DestroyWindow()`, and SDL's `visibilitychange` handler is removed
  after the create. Evidence and the page's side are in
  [web.md](web.md#fullscreen).

## The window thread (v0.10.0)

A host that waits between trials leaves the window without a message
pump: Python's `time.sleep()` and `input()`, Octave's `pause`, C's
`Sleep()`. Windows marks such a window hung after 4.2 to 4.6 s and puts a
"Ghost" window in front of it 50 ms later, and the first 2 to 20 frames
after the wait go wrong (`docs/av_probe.md`). The window-thread probe
(`docs/winthread_probe.md`, branch `probe-winthread`) gave the window, its
messages and every SDL video call to a thread of the header's own, kept
the D3D11 device, the swap path and ANGLE on the frame thread, and found
no hung window, no ghost, and no cost in the frame loop. v0.10.0 lands it.
The user decided the shape on 2026-10-26: a desc field with AUTO on,
a public window call, and a panic that releases the display while the
window thread answers.

### What the window thread owns

| On the window thread | On the frame thread | Any thread |
|---|---|---|
| `SDL_InitSubSystem(VIDEO)` and its quit, gamepads, the window, the mode switch, the icon, the raise, the cursor, text input, the device lists at open, gamepad hot-plug, `yscr_displays()` and `yscr_modes()` | the D3D11 device, DXGI_FLIP's swapchain or COMPOSITION's target on the window's HWND, ANGLE, `begin()`, `flip_at()`, settling, records, the grid | SDL's queue (`SDL_PeepEvents`, `SDL_PushEvent`), the abort watch (it runs on the thread that queues the event), the panic watchdog, the raw mouse reader, the idle-wake waiter, the trigger worker, SDL's raw keyboard thread |
| the pump: `SDL_PumpEvents()` | `yscr_poll()` with `SDL_PeepEvents()`: no pump | the gamma ramp (GDI, by the display's name) |
| the program's window calls (`yscr_window_call()`) | two window flags read each frame (below) | |

The thread waits in `MsgWaitForMultipleObjectsEx` on its command event
and on any message for its windows (`QS_ALLINPUT`), at most 100 ms (4 ms
while a screen has gamepads, because SDL polls XInput in its pump), then
runs a waiting command, a waiting release, and `SDL_PumpEvents()`. The
100 ms wake is the heartbeat the panic reads. When the thread ends it
calls `SDL_CleanupTLS()`, so a thread per session leaves no SDL memory.

### Decisions

Each item below was open after the probe (its section 5 and its open
questions); the reason is the one that decided it.

1. **`desc.window_thread`: AUTO (0), ON, OFF.** AUTO is ON for DXGI_FLIP
   and COMPOSITION (user, 2026-10-26: the bindings need it, and C
   programs route their few SDL calls). AUTO is OFF where the thread
   cannot run or should not: the web and `YRT_NO_THREADS` (one thread), a
   custom presenter (it may call SDL on the window from the frame
   thread), and a program that started SDL's video on its own thread
   before the first open (SDL made that thread its main thread, and
   `SDL_IsMainThread()` would be false on ours). ON fails `open()` with a
   message where it cannot run. OFF is v0.9.0's path, line for line: the
   window code moved into two functions that both paths call.
2. **One mode per process, while screens with a window are open.** SDL
   has one main thread. An OFF window beside the window thread would be
   pumped by no thread, and an ON window beside an OFF one would put SDL's
   video calls on two threads. So `open()` refuses an ON or OFF that
   differs from the open screens', and AUTO takes theirs, except that a
   custom presenter's AUTO beside the window thread fails (AUTO would be
   OFF for it; the message says to set ON and route its SDL calls).
3. **`yscr_window_call(s, fn, ctx)` is public** (user, 2026-10-26). It is
   the one door for SDL calls on `yscr_window()`, so the header does not
   grow a wrapper per SDL call (title, cursor, grab, a console window,
   Dear ImGui). Without a window thread it runs `fn` at once, so code
   written for the thread runs on every backend and platform. A call
   from inside `fn` runs at once (the window thread would wait on
   itself). Any thread may call it; calls run one at a time under one
   lock.
4. **The time limit on a command.** A command the window thread has not
   taken in 5 s is withdrawn before it runs, and the call fails
   (`YSCR_ERR_TIMEOUT`). Why 5 s: Windows calls a window hung after 5 s
   without a message read, so a window thread that takes no command in
   that time is hung by the OS's own rule. Once taken:
   - `open()`'s command and a window call run to their end. Both use the
     caller's memory (the desc, the handle, `ctx`); returning while the
     window thread still writes there would corrupt it. A driver call
     that never returns hangs v0.9.0's `open()` the same way.
   - `close()`'s command works on a copy of what it needs (the window,
     the icons, the gamepads, the flags), held by the header. `close()`
     waits at most 5 s more and then returns: the window stays until the
     process ends, `yscr_error()` says "the window thread did not answer
     in 5 s", and the thread is left behind. Why 5 s: the destroy of a
     fullscreen window in a switched mode put the desktop mode back in 1.2
     s (measured, v0.3.1). Close must not hang the host.
   - After a thread is left behind, `open()` refuses a window: SDL's
     video belongs to that thread, and no other thread may take it.
5. **The display lists run on the window thread while it runs.**
   `yscr_displays()`, `yscr_modes()` and `yscr_mode_multiple()` call
   SDL's video functions; without a window thread they bring SDL's video
   up for the call on the calling thread, as before. Measured with the
   thread: the same displays and modes as before the open.
6. **`begin()` no longer pumps; `yscr_poll()` takes events with
   `SDL_PeepEvents()`**, which any thread may call. `SDL_PollEvent()`
   pumps, and SDL 3.4.0 asserts in debug builds that the pump runs on
   its main thread (`SDL_PumpEventsInternal`). Events come into SDL's
   queue as the window thread reads messages, also while the host waits.
7. **SDL's strings for the frame thread.** SDL keeps the string of a
   text, IME, drop or clipboard event in the memory of the thread that
   takes the event, and frees it at that thread's next pump
   (`SDL_FreeTemporaryMemory()` in `SDL_PumpEventsInternal`, SDL 3.4.0
   `src/events/SDL_events.c`). The frame thread never pumps, so nothing
   freed them. Measured with `tests/adapt/screen_window_test.c --only
   text` (500 characters typed with `SendInput` as UTF-16 into a window
   with text input on, each polled; SDL's allocations counted through
   `SDL_SetMemoryFunctions`): without a fix 2.00 SDL allocations stayed
   per character (the string and its list entry), with the fix 0.01.
   The fix: `yscr_poll()` notes when it returned such an event, and the
   next `begin()` or `close()` calls `SDL_CleanupTLS()` on that thread.
   It also frees the strings SDL returned before on that thread
   (`SDL_GetKeyName()` and the like); the manual says to copy them. A
   rewrite of the event's pointer in an event watch, to a copy the
   header holds, was considered and not taken: it relies on SDL linking
   the memory by pointer value, and a full copy store needs a size limit
   that a long drop of files can exceed.
8. **One window thread per process, with a user count.** The first
   `open()` that needs it starts it, the last `close()` stops it; each
   screen's window is on it. A per-screen thread would put SDL's video
   calls on several threads.
9. **Two flags stay on the frame thread.** `flip_at()` reads
   `SDL_TextInputActive()` and the raw mouse guard reads
   `SDL_GetWindowRelativeMouseMode()` on every frame. Both are field
   reads after SDL's object check in SDL 3.4.0 (`SDL_video.c`); a window
   call cost 6.5 and 11.4 us at the median and 27 and 44 us at p99 in two
   runs of 1000 (`screen_window_test --only call`, a 480 x 300 window),
   too much for each frame. The HWND and the window's pixel size (read
   at open with `GetClientRect` inside SDL) also stay. Everything that
   changes a window goes to the window thread.

### The panic's release

Before v0.10.0, three presses with no abort reported for the grace time
ended the process (exit code 99). With the window thread a host that
only waits never hangs its window, so the rule would end a MATLAB
session in `pause` (the av probe saw it). Decided (user, 2026-10-26):
keep the trigger, change what it does by who answers.

- **The window thread answers** (its heartbeat is newer than 1 s): the
  watchdog asks it for a release and waits up to 5 s (v0.10.1; 1 s in v0.10.0, see below). The window thread
  minimizes every window it owns and shows the cursor; the watchdog puts
  back the gamma ramps, writes `YSCR_EV_PANIC` with `u.u32[1]` 1 and adds
  an abort with the new reason `YSCR_ABORT_PANIC` (0x20). The next
  `begin()` reports it with the presses (0x21), and every `begin()` after
  that returns `YSCR_QUIT` with `YSCR_ABORT_PANIC`. The program may only
  close; `open()` refuses a window until every screen has closed.
  `panic_fn` does not run: the process lives and saves after its next
  `begin()`.
- **The window thread does not answer** (no heartbeat for 1 s, or the
  release not begun in 1 s or not done in 5 s): the process ends as
  before.

No call gets a released session back (user, 2026-10-26): a released
window would have to be restored, made fullscreen again and settled, and
a program that wants that can close and open.

Measured (`examples/screen/abort.c`, keys by `SendInput`, a 320 x 200
window with the watchdog armed through its test seam, AC): with the
window thread ON and the host in `Sleep(4000)`, the release ran 7.7 ms
after the panic began, the window was minimized, the child lived, its
next `begin()` returned 0x21 with 3 presses and the following ones 0x20,
and it exited 0 after close. With the window thread blocked by an 8 s
window call, the third press ended the child (exit code 99) after 1.1 s:
the heartbeat was 0.9 s old at the panic, so the watchdog asked for a
release, waited the 1 s and then ended the process. With the thread OFF,
"hang" and "ghost" ended the child 51 to 52 ms after the third press, as
in v0.3.1. `tests/adapt/screen_window_test.c` calls the panic as the
watchdog does: release 9.9 ms, and a child whose window thread is blocked
ended with exit code 99 after 2.4 s.

### Measured on v0.10.0: the window thread against v0.9.0's path

The harness is [tests/probe/screen_winthread/](../tests/probe/screen_winthread/)
(its README says how to build, run and make these tables): the
window-thread probe's `probe_c`, abi3 module for CPython 3.12.11 and MEX
file, built against this header in place of the probe's copy, with
`desc.window_thread` from `YSCR_WINDOW_THREAD` in the environment, so
each comparison is one binary per host; static SDL3 3.4.0 (MSVC 19.51
`/MD` for C, Python and MATLAB; Octave 10.1's MinGW gcc 14.2 for
Octave). The laptop and panel of the top of this note (Iris Xe, 1920 x
1200 at 60.0008 Hz, one display), AUTO backend (COMPOSITION in every
run), AC (the battery status read 2 at the start and end of the series),
the Balanced plan. Every run went through `psy-guard.sh time`, one at a
time; other workers built between runs. No photodiode: every tier is the
record's. The scene is the av probe's: N stimuli, one parameter set and
one draw each per frame. OFF ran first in each pair.

**The frame loop, 10 minutes per run** (2026-10-26 21:55 to 2026-10-27
02:39, and the four reruns of 05:11 to 05:53). Columns as in
`docs/winthread_probe.md` section 3.1.

| Host | N | Window | Design | Frames | Missed (vblanks) | Late target | Occluded | Residual us p50 / p99 | Work ms p50 / p99 / max | Path, tier | Window thread CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|
| C | 10 | fullscreen | OFF | 36002 | 0 (0) | 0 | 0 | 0.7 / 2.5 | 0.24 / 0.61 / 1.0 | independent 36002; t1 36002 | - |
| C | 10 | fullscreen | ON | 36002 | 0 (0) | 0 | 0 | 0.8 / 2.1 | 0.21 / 0.59 / 2.2 | independent 36002; t1 36002 | 266 ms in 600 s |
| C | 100 | fullscreen | OFF | 35929 | 16 (20) | 0 | 18 | 0.7 / 60.1 | 0.47 / 1.69 / 2.8 | independent 35926, overlay 3; t1 35906, t2 2, t3 21 | - |
| C | 100 | fullscreen | ON | 36001 | 1 (1) | 0 | 0 | 0.7 / 25.5 | 0.42 / 1.33 / 3.3 | independent 36001; t1 36001 | 297 ms in 600 s |
| C | 10 | 1280 x 720 | OFF | 36002 | 0 (0) | 0 | 0 | 0.7 / 1.9 | 0.20 / 0.53 / 1.3 | independent 36002; t1 36002 | - |
| C | 10 | 1280 x 720 | ON | 36001 | 1 (1) | 0 | 0 | 0.7 / 2.2 | 0.23 / 0.59 / 2.4 | independent 36001; t1 36001 | 359 ms in 600 s |
| C | 100 | 1280 x 720 | OFF | 36000 | 2 (2) | 0 | 0 | 0.7 / 2.2 | 0.49 / 1.55 / 3.1 | independent 36000; t1 36000 | - |
| C | 100 | 1280 x 720 | ON | 36002 | 0 (0) | 0 | 0 | 0.7 / 2.5 | 0.66 / 1.79 / 3.4 | independent 36002; t1 36002 | 484 ms in 600 s |
| Python 3.12 | 10 | fullscreen | OFF | 35998 | 1 (1) | 0 | 1 | 0.7 / 2.2 | 0.28 / 0.71 / 2.3 | independent 35998; t1 35997, t3 1 | - |
| Python 3.12 | 10 | fullscreen | ON | 36003 | 0 (0) | 0 | 0 | 1.0 / 86.3 | 0.28 / 0.74 / 1.3 | independent 36003; t1 35995, t2 8 | 406 ms in 600 s |
| Python 3.12 | 100 | fullscreen | OFF | 36000 | 3 (3) | 0 | 0 | 0.7 / 2.0 | 0.73 / 2.34 / 6.0 | independent 36000; t1 36000 | - |
| Python 3.12 | 100 | fullscreen | ON | 35939 | 44 (64) | 0 | 0 | 0.6 / 2.2 | 0.77 / 2.38 / 3.6 | independent 35939; t1 35939 | 469 ms in 600 s |
| Python 3.12 | 10 | 1280 x 720 | OFF | 36000 | 2 (2) | 0 | 0 | 0.6 / 2.1 | 0.29 / 0.75 / 2.1 | independent 36000; t1 36000 | - |
| Python 3.12 | 10 | 1280 x 720 | ON | 35999 | 3 (3) | 0 | 0 | 0.7 / 2.1 | 0.29 / 0.74 / 2.5 | independent 35999; t1 35999 | 391 ms in 600 s |
| Python 3.12 | 100 | 1280 x 720 | OFF | 36002 | 0 (0) | 0 | 0 | 0.8 / 2.4 | 1.24 / 2.67 / 5.1 | independent 36002; t1 36002 | - |
| Python 3.12 | 100 | 1280 x 720 | ON | 36002 | 0 (0) | 1 | 0 | 0.7 / 2.4 | 1.29 / 2.65 / 5.7 | independent 36002; t1 36002 | 531 ms in 600 s |
| MATLAB R2023a | 10 | fullscreen | OFF | 36002 | 0 (0) | 0 | 0 | 1.0 / 95.8 | 0.51 / 1.00 / 5.8 | independent 36002; t1 35990, t2 12 | - |
| MATLAB R2023a | 10 | fullscreen | ON | 36002 | 0 (0) | 1 | 0 | 1.2 / 104.2 | 0.50 / 0.99 / 3.2 | independent 36002; t1 35979, t2 23 | - |
| Octave 10.1 | 10 | fullscreen | OFF | 36002 | 0 (0) | 1 | 0 | 1.4 / 106.1 | 0.79 / 1.59 / 3.4 | independent 36002; t1 35944, t2 58 | - |
| Octave 10.1 | 10 | fullscreen | ON | 36002 | 0 (0) | 0 | 0 | 1.2 / 104.1 | 0.84 / 1.60 / 4.4 | independent 36002; t1 35973, t2 29 | - |

Reruns of the fullscreen N = 100 pairs: the first C OFF run had 18
OCCLUDED records (another window over ours for a while) and the first
Python ON run 37 of its 44 misses between 529 and 578 s, with the work per
frame up from 0.8 to 1.2 to 1.7 ms in the same stretch (the machine, not
the screen). Again, 05:11 to 05:53:

| C | 100 | fullscreen | OFF | 36001 | 1 (1) | 0 | 0 | 1.2 / 103.7 | 0.68 / 1.57 / 2.8 | independent 36001; t1 35962, t2 39 | - |
| C | 100 | fullscreen | ON | 36003 | 0 (0) | 0 | 0 | 1.0 / 95.2 | 0.77 / 1.58 / 2.8 | independent 36003; t1 35972, t2 31 | - |
| Python 3.12 | 100 | fullscreen | OFF | 36002 | 0 (0) | 0 | 0 | 0.9 / 88.0 | 1.22 / 2.61 / 7.1 | independent 36002; t1 35978, t2 24 | - |
| Python 3.12 | 100 | fullscreen | ON | 36001 | 1 (1) | 1 | 0 | 1.3 / 108.0 | 1.33 / 2.63 / 5.4 | independent 36001; t1 35954, t2 47 | - |

(Window thread CPU in the reruns: 328 and 344 ms in 600 s.) Reading the
tables: no difference follows the design. Away from the two disturbed
runs, 0 to 3 missed frames per 36,000 in either design; residual p99 2 to
2.5 us in a window and 2 to 108 us fullscreen in both (the fullscreen
tails are the grid-jitter stretches the probe described); every frame on
independent flip; the host's work per frame within 0.2 ms at p50 and p99
between the designs of a pair. MATLAB and Octave, 10 minutes each: no
missed frame in either design. The window thread used 0.27 to 0.53 s of
CPU in 600 s.

**Idle gaps with no pump** (2026-10-27 03:24 to 04:57): 120
frames, then no call for the gap (C `Sleep()`, Python `time.sleep()`),
then 120 frames; N = 10; a thread polled `IsHungAppWindow()` and the
foreground every 50 ms. "Bad" frames are those before the first 10 in a
row with tier 1, no drop and no flag.

| Gap | Window | Host | OFF: hung, ghost | OFF: frame 120; bad frames | ON: hung, ghost | ON: frame 120; bad frames |
|---|---|---|---|---|---|---|
| 5 s | fullscreen | C | 4.21 s, 4.26 s | frame 120: drop 1, overlay, tier 3, flags 0x100; bad 3 (133 ms), max begin wait 81.1 ms, flags 0x10a, drops 1 | never, never | frame 120: drop 0, independent, tier 1, flags 0x4; bad 1 (17 ms), max begin wait 0.1 ms, flags 0x4, drops 0 |
| 5 s | fullscreen | Python | 4.26 s, 4.31 s | frame 120: drop 1, overlay, tier 3, flags 0x100; bad 3 (133 ms), max begin wait 80.1 ms, flags 0x10a, drops 1 | never, never | frame 120: drop 0, independent, tier 1, flags 0x0; bad 0 (0 ms), max begin wait 0.0 ms, flags 0x0, drops 0 |
| 5 s | 1280 x 720 | C | 4.05 s, 4.10 s | frame 120: drop 1, overlay, tier 3, flags 0x100; bad 2 (50 ms), max begin wait 14.2 ms, flags 0x100, drops 1 | never, never | frame 120: drop 0, independent, tier 1, flags 0x0; bad 0 (0 ms), max begin wait 0.0 ms, flags 0x0, drops 0 |
| 5 s | 1280 x 720 | Python | 4.46 s, 4.51 s | frame 120: drop 1, overlay, tier 3, flags 0x100; bad 3 (67 ms), max begin wait 16.7 ms, flags 0x100, drops 1 | never, never | frame 120: drop 0, independent, tier 2, flags 0x10; bad 17 (283 ms), max begin wait 15.8 ms, flags 0x10, drops 0 |
| 10 s | fullscreen | C | 4.30 s, 4.36 s | frame 120: drop 1, overlay, tier 3, flags 0x100; bad 3 (133 ms), max begin wait 80.0 ms, flags 0x10a, drops 1 | never, never | frame 120: drop 0, independent, tier 2, flags 0x10; bad 17 (283 ms), max begin wait 16.3 ms, flags 0x10, drops 0 |
| 10 s | fullscreen | Python | 4.26 s, 4.31 s | frame 120: drop 0, composed, tier 3, flags 0xe; bad 19 (467 ms), max begin wait 83.0 ms, flags 0x1e, drops 0 | never, never | frame 120: drop 0, independent, tier 2, flags 0x10; bad 17 (283 ms), max begin wait 16.8 ms, flags 0x10, drops 0 |
| 10 s | 1280 x 720 | C | 4.04 s, 4.09 s | frame 120: drop 1, overlay, tier 3, flags 0x100; bad 3 (67 ms), max begin wait 17.7 ms, flags 0x100, drops 1 | never, never | frame 120: drop 0, independent, tier 1, flags 0x0; bad 0 (0 ms), max begin wait 0.0 ms, flags 0x0, drops 0 |
| 10 s | 1280 x 720 | Python | 4.21 s, 4.26 s | frame 120: drop 1, overlay, tier 3, flags 0x110; bad 10 (183 ms), max begin wait 30.9 ms, flags 0x110, drops 1 | never, never | frame 120: drop 0, independent, tier 2, flags 0x10; bad 17 (283 ms), max begin wait 16.4 ms, flags 0x10, drops 0 |
| 30 s | fullscreen | C | 4.31 s, 4.36 s | frame 120: drop 1, overlay, tier 3, flags 0x110; bad 19 (467 ms), max begin wait 81.8 ms, flags 0x11a, drops 1 | never, never | frame 120: drop 0, independent, tier 2, flags 0x10; bad 17 (283 ms), max begin wait 16.5 ms, flags 0x10, drops 0 |
| 30 s | fullscreen | Python | 4.31 s, 4.36 s | frame 120: drop 0, composed, tier 3, flags 0xa; bad 19 (450 ms), max begin wait 81.5 ms, flags 0x1a, drops 0 | never, never | frame 120: drop 0, independent, tier 2, flags 0x10; bad 17 (283 ms), max begin wait 16.2 ms, flags 0x10, drops 0 |
| 30 s | 1280 x 720 | C | 4.46 s, 4.51 s | frame 120: drop 1, overlay, tier 3, flags 0x110; bad 15 (267 ms), max begin wait 29.9 ms, flags 0x110, drops 1 | never, never | frame 120: drop 1, independent, tier 2, flags 0x10; bad 17 (300 ms), max begin wait 17.0 ms, flags 0x10, drops 1 |
| 30 s | 1280 x 720 | Python | 4.41 s, 4.46 s | frame 120: drop 1, overlay, tier 3, flags 0x114; bad 16 (300 ms), max begin wait 28.7 ms, flags 0x114, drops 1 | never, never | frame 120: drop 0, independent, tier 2, flags 0x10; bad 17 (283 ms), max begin wait 16.5 ms, flags 0x10, drops 0 |

With the thread OFF every window was marked hung after 4.04 to 4.46 s
and a ghost took the foreground 50 ms later; the first frame after the
gap dropped a vblank or went composed at tier 3, and `begin()` waited up
to 82 ms. With the thread ON: never hung, no ghost, the first frame on
independent flip with no drop (one 30 s window run dropped one vblank).
The 17 bad frames ON after gaps of 10 s and more are GRID_UNSTABLE only:
the grid after a gap, below (v0.10.1).

**MATLAB R2023a and Octave 10.1** (MEX, static SDL3; `pause(30)` with no
pump):

| Case | MATLAB OFF | MATLAB ON | Octave OFF | Octave ON |
|---|---|---|---|---|
| hung, ghost (fullscreen; window) | never; never | never; never | 4.21 s, 4.26 s; 4.41 s, 4.46 s | never; never |
| frame 120, fullscreen | dropped 1, independent, GRID_UNSTABLE | on time, independent, GRID_UNSTABLE | composed, tier 3, `begin()` waits up to 82 ms | on time, independent, GRID_UNSTABLE |
| frame 120, window | dropped 1, independent | on time, independent | 2 dropped, overlay, tier 3 | on time, independent |
| Shift+Esc in frames | | QUIT, 0x1 | | QUIT, 0x1 |
| 3 presses during `pause(10)` | | lives; minimized; release 8.1 ms; next `begin()` 0x21, 3 presses | | lives; minimized; release 10.7 ms; 0x21, 3 presses |
| open, 1 s of frames, close, 3 times in one session | | done, exit 0 | | done, exit 0 |

MATLAB's `pause` pumps its main thread, so OFF did not hang there either
(as the av probe found). C and Python with the thread ON: Shift+Esc in
frames gave QUIT 0x1 with 1 press in both designs; 3 presses while the host
slept 10 s released the display (8.7 and 9.5 ms) and the next `begin()`
returned 0x21 with 3 presses.

**Input and the command cost** (C, fullscreen, ON): 50 keys by scan
code while the frame thread spun on `yscr_poll()`: available 0.48 ms
after `SendInput()` at p50 and 0.90 ms at p99, stamped 0.48 and 0.89 ms;
an empty window call: 6.3 us median, 30 us max over 1000.

### Dear ImGui

Dear ImGui's SDL3 backend calls SDL's window, mouse, cursor, gamepad,
text input and clipboard functions from the thread that runs it, and
ImGui calls the backend's platform callbacks (`Platform_SetImeDataFn`,
the clipboard, `Platform_OpenInShellFn`) from `ImGui::EndFrame()` and its
text widgets. With the window thread all of them must run there. The
pattern, in C through cimgui:

```c
static yscr_screen scr;
static SDL_Event im_ev[256];
static int im_n;

/* the frame's events and NewFrame, in one call per frame */
static void im_frame(void* u) {
    (void)u;
    for (int i = 0; i < im_n; i++) ImGui_ImplSDL3_ProcessEvent(&im_ev[i]);
    im_n = 0;
    ImGui_ImplSDL3_NewFrame();
}

/* each platform callback through the window call */
static void (*ime_fn)(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData*);
typedef struct { ImGuiContext* c; ImGuiViewport* v; ImGuiPlatformImeData* d; } ime_arg;
static void ime_cb(void* p) { ime_arg* a = p; ime_fn(a->c, a->v, a->d); }
static void ime_routed(ImGuiContext* c, ImGuiViewport* v, ImGuiPlatformImeData* d) {
    ime_arg a = { c, v, d };
    yscr_window_call(&scr, ime_cb, &a);
}
/* the same for Platform_GetClipboardTextFn, _SetClipboardTextFn, _OpenInShellFn */

/* at setup, after ImGui_ImplSDL3_InitForOther() ran in a window call: */
ImGuiPlatformIO* pio = igGetPlatformIO_Nil();
ime_fn = pio->Platform_SetImeDataFn;
pio->Platform_SetImeDataFn = ime_routed;

/* each frame */
while (yscr_poll(&scr, &ev, NULL)) if (im_n < 256) im_ev[im_n++] = ev;
ImGui_ImplOpenGL3_NewFrame();          /* GL: the frame thread */
yscr_window_call(&scr, im_frame, NULL);
igNewFrame();
/* ... the panels ... */
igRender();                            /* may call ime_routed */
```

`ImGui_ImplSDL3_InitForOther()` and `ImGui_ImplSDL3_Shutdown()` go
through the window call too. An operator console in a second SDL window
(docs/imgui_probe.md, configuration a) is made and destroyed in a window
call, so the window thread reads its messages; its D3D11 device,
swapchain and ANGLE context stay on the frame thread. The GL renderer
(`ImGui_ImplOpenGL3_*`) stays on the frame thread with the context.

The ImGui probe of docs/imgui_probe.md (`C:\tmp\psy-work\imgui`, not in
the repository) was ported to the ysp names and routed this way
(scratch copy of the window-thread worker, `imgui-wt`). On this laptop,
COMPOSITION, a 800 x 600 timed window, AC, 2026-10-27 03:2x, each run
through the timing guard (ON first in each pair):

| Mode | Window thread | Frames | Long intervals | ImGui frame on the frame thread, mean / p99 us | Window calls |
|---|---|---|---|---|---|
| console, 12 s | ON | 839 | 0 of 718 | 275 / 712 | 843 |
| console, 12 s | OFF | 839 | 0 of 718 | 217 / 522 | 843 (inline) |
| overlay, 8 s | ON | 599 | 0 of 478 | 288 / 645 | 604 |
| overlay, 8 s | OFF | 599 | 0 of 478 | 256 / 533 | 604 (inline) |

The ImGui frame (events, NewFrame, the panels, Render) took 32 to 58 us
more at the mean with the window thread (80 to 90 us in a first pair of
runs on 2026-10-26 outside the timing guard): two window calls a frame (`im_frame` and
the IME callback in `Render()`), each a wake of the window thread. One
call a frame, with the IME data applied at the next frame's call, would
save one. The probe's input test (console mode, F24 by `SendInput`)
gave the same routing as on v0.3.2: with the stock IME callback, an
active ImGui text field turned on text input on the timed window once it
had the focus (keys off the raw path); with the probe's routed IME
callback it stayed off. Both with the window thread ON and OFF.

### Tests

- `tests/adapt/screen_test.c`, `test_window_thread()`, without SDL: the
  mode rule in every case (AUTO on, off for each reason, AUTO following
  the open screens, ON and OFF refused), `desc.window_thread`'s range,
  the window call without a thread (inline; a NULL `fn` fails with a
  message), and a released screen: the release's abort once (0x21), then
  0x20 from every `begin()`, a group with a released member begins no
  member, and a screen the thread does not own sees the abort once.
- `tests/adapt/screen_window_test.c` (new; Windows, a display; CMake
  builds `test_screen_window` and does not run it, because CI has no
  display) runs the thread on real windows: `idle` (5 s of `Sleep()`
  with no pump: not hung, no ghost, AUTO's describe token), `call` (a
  title through the window call, a nested call, 600 calls from 3 threads
  each run once, the round trip, the thread gone after close),
  `displays`, `mixed` (one mode per process), `text` (above), `release`
  (the panic called as the watchdog calls it while the host sleeps),
  `kill` (a child with a blocked window thread: exit code 99), `two`
  (two screens, a group, one closed first), `cycles` (10 opens and
  closes OFF, then ON: handles, threads, SDL allocations), `fullscreen`
  (fullscreen off and on again through window calls while frames run),
  `gamepads` (beside a screen without), `mice` (raw mice: 20 moves by
  `SendInput`, 20 events), `triggers` (60 of 60 fired), and with
  `--mode` a display-mode switch at open (1920 x 1200 to 1680 x 1050 and
  back after close). 43 of 43 checks passed on 2026-10-26, `--mode`
  included. Later cases: `sequence` (v0.10.2) and `between` (v0.10.3).
  From v0.10.3 the checks that need the window in front (the typed
  text, the raw mice, `sequence`) skip with a message when the OS did not
  give the process the foreground, and the last line counts the skips;
  with another program that took the foreground every 50 ms, `text`,
  `mice` and `sequence` skipped and none failed. 81 of 81 checks passed
  on 2026-10-27, from a launcher in front and from a background shell.
- `examples/screen/abort.c` sends the keys: `live` (AUTO, so the window
  thread), `hang`, `ghost` and `held` with the thread OFF (the process
  ends, as in v0.3.1), `release` and `kill` with it ON, and with
  `--mode-test` the mode case OFF and ON.
- Mutants `wt-01` to `wt-09` (the core test) and `wt-20` to `wt-29`
  (`tests/adapt/screen_window_test.c`, one case each through
  `YSCR_WINDOW_TEST_ONLY`): 19 of 19 killed. The whole list, 164 with
  v0.10.1's, ran again with `-j 3`: 162 killed, 1 timeout (st-11), and
  `wt-25` survived because its baseline failed (another test's window
  had the foreground, so the typed keys missed); alone it was killed. Run
  the window mutants one at a time.

### Findings that are not the window thread's

- **5 handles per open and close.** `screen_window_test --only cycles`:
  from the second cycle to the tenth the process gained 40 handles, 5 a
  cycle, with the window thread OFF and the same with it ON
  (COMPOSITION, 480 x 300 window, settling off). No thread and no SDL
  allocation was left. The handles are not the window thread's; whose
  they are was not traced (ANGLE, D3D11 or DComp are candidates).
  Fixed in v0.10.4: 5 events of COMPOSITION's open
  ([Handles, no free buffer, display ids](#handles-no-free-buffer-display-ids-v0104)).
- **SDL's display ids change with each start of SDL's video.** A
  `yscr_displays()` before the first open returned id 1 for the panel;
  with a screen open, id 3. SDL numbers displays anew at each video
  init, and `yscr_displays()` without a screen starts and stops video
  for the call. So a `desc.display` taken from `yscr_displays()` before
  the first open does not name a display at that open (v0.9.0 too; one
  display here, so `desc.display` 0 was used throughout). With the
  window thread running, the ids stay valid between opens. Fixed in
  v0.10.4: an id that is no longer current is matched by name and bounds
  ([Handles, no free buffer, display ids](#handles-no-free-buffer-display-ids-v0104)).

## The grid after a gap (v0.10.1)

The window-thread probe (`docs/winthread_probe.md` section 3.2) found,
in both designs and with the av probe's pump: after a gap of 10 s or more
without presents, the first 16 records carry `YSCR_FLIP_GRID_UNSTABLE` at
tier 2, the 17th shows the grid moving to the OS's vblank times, and the
onsets of the flagged records are up to 1 ms late against the OS's times.

### The cause

`gapdiag.c` ([tests/probe/screen_gap/](../tests/probe/screen_gap/); its
README says how to build and run it and make these tables) wraps a
screen's presenter, so each OS vblank time is logged with the
grid's state before the core used it. It runs frames for a time, sleeps
the gap with no call, and runs 120 frames. COMPOSITION, fullscreen,
window thread ON, the laptop of this note, AC, 2026-10-26 23:2x to
2026-10-27 01:0x, through the timing guard:

| Frames before the gap | Gap | The grid's period before the gap, against the mode | The first OS time after the gap, off the grid | GRID_UNSTABLE after |
|---|---|---|---|---|
| 2 s | 30 s | -17.1 ppm | +530 us | 17 |
| 2 s | 10 s | +14.4 ppm | -154 us | 0 (within 1% of a period) |
| 60 s | 30 s | -0.4 ppm | +3 us | 0 |
| 2 s, DXGI_FLIP | 30 s | -12.0 ppm | +336 us | 17 |
| 60 s, DXGI_FLIP | 30 s | -0.2 ppm | -16 us | 0 |

The offset after the gap is the period's error times the gap. The grid
fits its period from the OS's times since its anchor (THE GRID); over the
first 2 s that fit was 1 to 41 ppm off the long-run rate, of either sign,
in 60 runs. The OS's times themselves wander: least-squares periods over
windows of the 60 s run (residuals 2 to 5 us within 1 s windows, 37 to 56
us within 10 s windows):

| Window of the run | Period against the mode |
|---|---|
| 0.0 to 1.0 s | -35.5 ppm |
| 1.0 to 2.0 s | -5.2 ppm |
| 2.0 to 3.0 s | +2.6 ppm |
| 3.0 to 4.0 s | +50.7 ppm |
| 4.0 to 5.0 s | +20.2 ppm |
| 5.0 to 7.5 s | -14.9 ppm |
| 7.5 to 10.0 s | +6.4 ppm |
| 10.0 to 20.0 s | -3.1 ppm |
| 20.0 to 30.0 s | -0.1 ppm |
| 30.0 to 40.0 s | +4.0 ppm |
| 40.0 to 50.0 s | +6.7 ppm |
| 50.0 to 60.0 s | +3.2 ppm |

So no fit over a few seconds predicts the vblanks 30 s later to the 1%
of a period (167 us) that keeps a time on the grid, and the rule that
protects the grid from DXGI's off-grid times (16 in a row before it
moves, v0.4.3) then flags 16 records and moves on the 17th.

### What the system knows during a gap

The same runs logged two other vblank sources every 20 ms or at each
vblank, from threads of the probe:

- The idle-wake waiter's wakes (COMPOSITION's `D3DKMTWaitForVerticalBlankEvent`):
  they track the vblanks through the gap, but each wake comes 7 to 173 us
  after the vblank (p10 to p90 in frames) and with a different spread
  while the host idles; a phase from them would need a latency model.
- DWM's vblank time (`DwmGetCompositionTimingInfo(NULL)`, `qpcVBlank`):
  within 4 us of the flip statistics' times before and after the gap on
  both backends (p10 to p90: -4.3 to +4.7 us fullscreen, -19 to +20 us in
  a window while the window was still composed, -8 to +7 us after), never
  older than 2 periods in 6,682 samples, and moving with the vblanks
  through the gap (the last sample of a gap within 1 to 19 us of the line
  of the OS's times after it). `qpcVBlank` is the next vblank, up to a
  period after the call (measured: 1 to 16 ms ahead), so the header takes
  it back one `qpcRefreshPeriod`.

### The fix

The candidate (feed the idle-wake waiter's times into the grid during the
gap) was not taken: the wakes carry a latency of tens of us that varies
with the CPU's load, they exist on COMPOSITION only, and the grid would
need a lock with the waiter's thread. DWM's time has no latency, works on
both backends, and is read once, on the frame thread:

- In `begin()`, after the completions and before the prediction, when the
  grid's newest OS time is 0.5 s old or more (a gap: within 0.5 s the
  grid holds to tens of us), the header reads DWM's vblank once for that
  gap.
- It takes it as an OS time (the anchor moves there, and the period's fit
  gains the gap as baseline) when it is newer than the grid's newest,
  at most 2 periods old, and within 1% of a period plus 100 ppm of the
  gap of the grid's vblank, at most a quarter period. Else it counts as
  refused and the grid extrapolates as before.
- DWM's timing is the primary display's: a screen on another display, or
  a refresh 0.5% off DWM's, does not ask. Two displays were not tried.
- `gap_anchors=N(max ...us, refused M)` in the describe line.

### Before and after

`gapdiag` again, 2 s of frames, then the gap, then 120 frames; fullscreen
unless stated; the before runs twice (0.10.0, and "(b)" from a build of
the first fix, which never anchored: it took DWM's next vblank, ahead of
now, as stale); 2026-10-27 02:4x to 06:09, through the timing guard, AC.

| Backend | Window | Gap s | Window thread | Header | Period before the gap, ppm | First OS time after the gap, us off the grid | GRID_UNSTABLE records after | Largest onset error, us | Gap anchors (move) |
|---|---|---|---|---|---|---|---|---|---|
| COMPOSITION | fullscreen | 5 | OFF | 0.10.0 | +28.14 | -151 | 0 | 0 | 0 |
| COMPOSITION | fullscreen | 5 | OFF | 0.10.0 (b) | +22.49 | -135 | 0 | 0 | 0 |
| COMPOSITION | fullscreen | 5 | OFF | 0.10.1 | +41.04 | -0 | 0 | 0 | 1 (334 us) |
| COMPOSITION | fullscreen | 5 | ON | 0.10.0 | +33.76 | -211 | 17 | 212 | 0 |
| COMPOSITION | fullscreen | 5 | ON | 0.10.0 (b) | +35.20 | -210 | 17 | 219 | 0 |
| COMPOSITION | fullscreen | 5 | ON | 0.10.1 | +24.96 | -0 | 0 | 0 | 1 (227 us) |
| COMPOSITION | fullscreen | 10 | OFF | 0.10.0 | +19.17 | -239 | 17 | 268 | 0 |
| COMPOSITION | fullscreen | 10 | OFF | 0.10.0 (b) | +18.92 | -197 | 17 | 197 | 0 |
| COMPOSITION | fullscreen | 10 | OFF | 0.10.1 | +18.66 | -1 | 0 | 0 | 1 (201 us) |
| COMPOSITION | fullscreen | 10 | ON | 0.10.0 | +22.02 | -245 | 17 | 253 | 0 |
| COMPOSITION | fullscreen | 10 | ON | 0.10.0 (b) | +37.10 | -392 | 17 | 407 | 0 |
| COMPOSITION | fullscreen | 10 | ON | 0.10.1 | +21.98 | -1 | 0 | 0 | 1 (220 us) |
| COMPOSITION | fullscreen | 30 | OFF | 0.10.0 | +26.87 | -862 | 17 | 871 | 0 |
| COMPOSITION | fullscreen | 30 | OFF | 0.10.0 (b) | +15.13 | -499 | 17 | 501 | 0 |
| COMPOSITION | fullscreen | 30 | OFF | 0.10.1 | +28.44 | -0 | 0 | 0 | 1 (895 us) |
| COMPOSITION | fullscreen | 30 | ON | 0.10.0 | +28.70 | -896 | 17 | 901 | 0 |
| COMPOSITION | fullscreen | 30 | ON | 0.10.0 (b) | +19.72 | -675 | 17 | 682 | 0 |
| COMPOSITION | fullscreen | 30 | ON | 0.10.1 | +15.64 | -1 | 0 | 0 | 1 (480 us) |
| COMPOSITION | fullscreen | 30 (again) | ON | 0.10.0 | +8.74 | -274 | 17 | 291 | 0 |
| COMPOSITION | fullscreen | 30 (again) | ON | 0.10.0 (b) | -1.43 | 15 | 0 | 0 | 0 |
| COMPOSITION | fullscreen | 30 (again) | ON | 0.10.1 | +22.28 | 2 | 0 | 0 | 1 (726 us) |
| COMPOSITION | fullscreen | 60 | OFF | 0.10.0 | +23.34 | -1495 | 17 | 1501 | 0 |
| COMPOSITION | fullscreen | 60 | OFF | 0.10.0 (b) | +12.38 | -727 | 17 | 731 | 0 |
| COMPOSITION | fullscreen | 60 | OFF | 0.10.1 | +16.02 | -0 | 0 | 0 | 1 (1036 us) |
| COMPOSITION | fullscreen | 60 | ON | 0.10.0 | +15.89 | -1013 | 17 | 1024 | 0 |
| COMPOSITION | fullscreen | 60 | ON | 0.10.0 (b) | +30.06 | -1904 | 17 | 1921 | 0 |
| COMPOSITION | fullscreen | 60 | ON | 0.10.1 | +15.04 | 0 | 0 | 0 | 1 (991 us) |
| COMPOSITION | 1280 x 720 | 30 | ON | 0.10.0 | +12.67 | -382 | 17 | 391 | 0 |
| COMPOSITION | 1280 x 720 | 30 | ON | 0.10.0 (b) | +20.06 | -659 | 17 | 660 | 0 |
| COMPOSITION | 1280 x 720 | 30 | ON | 0.10.1 | +5.93 | -0 | 0 | 0 | 1 (208 us) |
| DXGI_FLIP | fullscreen | 5 | OFF | 0.10.0 | +24.08 | -132 | 0 | 0 | 0 |
| DXGI_FLIP | fullscreen | 5 | OFF | 0.10.0 (b) | +33.93 | -214 | 17 | 225 | 0 |
| DXGI_FLIP | fullscreen | 5 | OFF | 0.10.1 | +19.35 | 1 | 0 | 0 | 1 (125 us) |
| DXGI_FLIP | fullscreen | 5 | ON | 0.10.0 | +20.45 | -126 | 0 | 0 | 0 |
| DXGI_FLIP | fullscreen | 5 | ON | 0.10.0 (b) | +27.38 | -140 | 0 | 0 | 0 |
| DXGI_FLIP | fullscreen | 5 | ON | 0.10.1 | +15.85 | 1 | 0 | 0 | 1 (89 us) |
| DXGI_FLIP | fullscreen | 10 | OFF | 0.10.0 | +22.86 | -252 | 17 | 255 | 0 |
| DXGI_FLIP | fullscreen | 10 | OFF | 0.10.0 (b) | +7.77 | -72 | 0 | 0 | 0 |
| DXGI_FLIP | fullscreen | 10 | OFF | 0.10.1 | +13.10 | -1 | 0 | 0 | 1 (153 us) |
| DXGI_FLIP | fullscreen | 10 | ON | 0.10.0 | +13.66 | -119 | 0 | 0 | 0 |
| DXGI_FLIP | fullscreen | 10 | ON | 0.10.0 (b) | +25.31 | -328 | 17 | 331 | 0 |
| DXGI_FLIP | fullscreen | 10 | ON | 0.10.1 | +28.78 | 0 | 0 | 0 | 1 (286 us) |
| DXGI_FLIP | fullscreen | 30 | OFF | 0.10.0 | +19.86 | -594 | 17 | 595 | 0 |
| DXGI_FLIP | fullscreen | 30 | OFF | 0.10.0 (b) | +24.97 | -766 | 17 | 774 | 0 |
| DXGI_FLIP | fullscreen | 30 | OFF | 0.10.1 | +12.04 | 1 | 0 | 0 | 1 (412 us) |
| DXGI_FLIP | fullscreen | 30 | ON | 0.10.0 | +37.52 | -1164 | 17 | 1184 | 0 |
| DXGI_FLIP | fullscreen | 30 | ON | 0.10.0 (b) | +28.56 | -949 | 17 | 956 | 0 |
| DXGI_FLIP | fullscreen | 30 | ON | 0.10.1 | +25.86 | -0 | 0 | 0 | 1 (766 us) |
| DXGI_FLIP | fullscreen | 30 (again) | ON | 0.10.0 | -5.14 | 156 | 0 | 0 | 0 |
| DXGI_FLIP | fullscreen | 30 (again) | ON | 0.10.0 (b) | -6.55 | 247 | 17 | 248 | 0 |
| DXGI_FLIP | fullscreen | 30 (again) | ON | 0.10.1 | +30.89 | -1 | 0 | 0 | 1 (968 us) |
| DXGI_FLIP | fullscreen | 60 | OFF | 0.10.0 | -23.96 | 1334 | 17 | 1334 | 0 |
| DXGI_FLIP | fullscreen | 60 | OFF | 0.10.0 (b) | -4.64 | 228 | 17 | 228 | 0 |
| DXGI_FLIP | fullscreen | 60 | OFF | 0.10.1 | +29.49 | -1 | 0 | 0 | 1 (1864 us) |
| DXGI_FLIP | fullscreen | 60 | ON | 0.10.0 | +20.11 | -1213 | 17 | 1219 | 0 |
| DXGI_FLIP | fullscreen | 60 | ON | 0.10.0 (b) | +30.72 | -1961 | 17 | 1981 | 0 |
| DXGI_FLIP | fullscreen | 60 | ON | 0.10.1 | +23.58 | -2 | 0 | 0 | 1 (1431 us) |
| DXGI_FLIP | 1280 x 720 | 30 | ON | 0.10.0 | +4.57 | -129 | 0 | 0 | 0 |
| DXGI_FLIP | 1280 x 720 | 30 | ON | 0.10.0 (b) | -1.84 | 75 | 0 | 0 | 0 |
| DXGI_FLIP | 1280 x 720 | 30 | ON | 0.10.1 | +8.61 | 1 | 0 | 0 | 1 (282 us) |

Before: 17 GRID_UNSTABLE records after the gap in 29 of 40 runs, each
run whose drift passed 167 us (3 of 8 at 5 s, 6 of 8 at 10 s, 12 of 16
at 30 s, 8 of 8 at 60 s), onsets up to 1.98 ms late. After: none in 20 of 20; the first OS time
after the gap 0 to 2 us off the grid; DWM's time moved the grid 89 us to
1.86 ms. The fix lands.

The core test plays it on its scripted display (`test_gap_anchor()`):
the vblanks move 0.5 ms during a 30 s gap; without the system's vblank
16 to 17 records are flagged, with it none and every onset a vblank; a
system vblank 2 ms off is refused once; a 0.3 s gap does not ask. Mutants
`gp-01` to `gp-04`, all killed.

Not run: a second display (DWM's timing is the primary's), another
refresh, a gap longer than 60 s (at 30 ppm the drift passes a quarter
period after about 140 s and the anchor is refused), a display that
turned off during the gap.

### The release in a switched mode (fixed in v0.10.1)

`examples/screen/abort.c --mode-test` on v0.10.0: fullscreen at 1680 x
1050 on the 1920 x 1200 desktop, the window thread ON, the host in
`Sleep()`: the panic ended the process (exit code 99) instead of the
release. Minimizing a fullscreen window in a switched mode puts the
desktop mode back first; that took 1.2 to 1.4 s, and the watchdog waited
1 s for the release. Now it waits 1 s for the window thread to begin the
release and 5 s for it to end, and reads the hook's messages meanwhile
(the hook runs on the watchdog's thread). After, in two runs: the release
took 1.43 and 1.49 s on the window thread, the child lived and exited 0,
and the desktop mode was back 1.23 and 1.29 s after the third press. The `kill` case (a
blocked window thread) still ended the process, 1.1 s after the third
press: the blocked thread never began the release.

## The next screen's raise (v0.10.2)

Found by the ysp-vis real-window tests (2026-10-27, main a0d3dcc): one
Python process opened a 1280 x 720 window three times, one after
another. With `window_thread` ON, the first window got
`foreground=granted`, the second and the third `foreground=refused`, and
each of their 120 records was `PATH_UNKNOWN`, `ESTIMATED|OCCLUDED`,
tier 3. With OFF, all three were granted.

### The cause

Windows lets a thread that has the foreground window give the foreground
to its next window. With the window thread OFF, the caller's thread
makes every window, so the right goes from one screen to the next. With
ON, the window thread ends at the last close, and the next open's window
belongs to a new thread, which has no right of its own. The raise was
refused, the window opened behind the other programs, and DWM gave its
flips no statistics because another window covered it. Fullscreen
(AUTO is FORCE there) the second and the third raise were refused too,
and the forced raise took the foreground: `foreground=forced`, the
records good.

Two fixes, each measured with `seq.c` (not in the repository; three
1280 x 720 windows, 120 frames each, the window thread ON), two runs
each on this laptop, AC:

| Header | Screen 1 | Screens 2 and 3 |
|---|---|---|
| v0.10.1 | granted | refused, 119 of 119 records OCCLUDED and PATH_UNKNOWN |
| A: the window thread lives on after the last close | granted | granted, 0 OCCLUDED |
| B: close() calls `AllowSetForegroundWindow` for its own process while its window has the foreground | granted | granted, 0 OCCLUDED |

B is v0.10.2. A keeps a thread and a hidden window after the last
screen, and a MEX file that a host unloads would have to stop it; B
keeps no state, and the right belongs to the process, so it holds across
a module unload. The right goes only from a screen that has the
foreground: when the user gave input to another program, or a forced
raise gave the foreground back at close (FOREGROUND), the next screen in
a window is refused as before.

### Before and after

All runs on this laptop, AC, through the timing guard. Only runs whose
first screen got the foreground count: the process has the right only
when a program in front started it and no input went to another program
since. Some runs came from the shell of the tool, which had it; the
others from a launcher (not in the repository) that took the foreground
and then started the program, as a terminal in front does. C: `seq.c`
(three 1280 x 720 windows, 120 frames each) and `fgdbg.c` (four, 60
frames each). Python: the ysp-vis repro (three 1280 x 720 windows with a
gfx, 120 frames each), 0.10.1.1 from main against 0.10.2 from this
branch, CPython 3.14.3.

| Program | Header | Window thread | Runs | Screens 2 and later |
|---|---|---|---|---|
| C | v0.10.1 | ON | 3 | refused in 7 of 7; every record OCCLUDED (119 of 119, 59 of 59) |
| C | v0.10.2 | ON | 3 | granted in 8 of 8; no OCCLUDED record |
| C | v0.10.1 | OFF | 3 | granted in 7 of 7 |
| C | v0.10.2 | OFF | 2 | granted in 5 of 5 (13 of 59 records OCCLUDED in one screen, which lost the foreground to another program's window during the run) |
| Python | 0.10.1.1 | ON | 2 | refused in 4 of 4; 120 of 120 records PATH_UNKNOWN in 3 of them |
| Python | 0.10.2 | ON | 3 | granted in 6 of 6; no PATH_UNKNOWN record |
| Python | 0.10.1.1 | OFF | 5 | granted in 10 of 10 |
| Python | 0.10.2 | OFF | 6 | granted in 8 of 12; refused, with no PATH_UNKNOWN record, in the other 4 (2 runs) |

The two Python runs with OFF and refusals ran while another worker's
window tests ran outside the guard; the three later runs, two from the
launcher and one from the shell, and one before them were granted in every screen. The C
runs with OFF and v0.10.2 do not show it.

In MATLAB R2023a (`test_mex_vis_window.m`, ysp-vis MEX 1.1.0 on
v0.10.2, from the launcher) the second window got `foreground=granted`
and its records path 3, tier 1.

Fullscreen with v0.10.1: screens 2 and 3 were `forced`, the records good;
with v0.10.2 `granted`.

### Tests

`tests/adapt/screen_window_test.c --only sequence`: three screens in a
window and three fullscreen, one after another, then two at once and one
more after both closed; each screen is in front, not refused, and has no
OCCLUDED record in 60 frames. The case is skipped when the OS refused the
process the foreground at the first screen (a launch from the
background, or input to another program). Mutant `fg-08` (the call
removed) fails 11 of its 17 checks from the launcher. `bindings/mex/test_mex_vis_window.m`
checks that its second window gets the foreground as the first did.

## Between frames (v0.10.3)

Found 2026-10-27 by the ysp-vis text work: `ygfx_prime()` called after
`flip()` and before the next `begin()` or `wait_flip()` blocked for 18 s
to more than 5 minutes. The binding called `yscr_wait_flip()` first as a
workaround.

### The cause

On COMPOSITION, `begin()` binds a free presentation buffer as
framebuffer 0, and `flip()` presents it. The buffer then stayed bound. A
buffer on the screen is released only when a newer present replaces it
or when another window covers ours. `ygfx_prime()` draws its output pass
into framebuffer 0 and then reads a pixel, so the GPU waited for the
release. A read alone did not wait; a write did. DXGI_FLIP draws into
its own texture and copies it at the present, so it did not wait. Not
the foreground (v0.10.2): a window that was refused and covered did not
wait, and a window in front did.

On v0.10.2, this laptop, AC, `prime_c.c` (not in the repository: one
frame, then the prime; 800 x 600 or fullscreen; in front means raised
with FORCE and still in front at the prime):

| Before the prime | In front, COMPOSITION | Other |
|---|---|---|
| nothing (at once), 1 s of sleep, or 60 more frames | more than 90 s in 4 of 4 (window and fullscreen, window thread ON and OFF); 47 and 56 s in 2 more, until another window covered ours | covered: 45 ms to 1.8 s (40 runs); DXGI_FLIP: 100 and 137 ms |
| `yscr_wait_flip()` | 64 ms | |

`ygfx_read_scene()`, `ygfx_read_output()` and `glFinish()` at the same
point took 1.4 to 2.8 ms. The text worker's Python repro (ysp-vis
0.10.2): more than 90 s in 3 of 3 runs in front, 4 to 6 ms in 5 covered
ones.

### The fix

`flip()` binds a buffer that no present holds right after the present:
the first free one of the other two, by `IsAvailable`, as `begin()`
does. `begin()` takes that buffer first when it is still free, so it is
current already. When no buffer is free, no surface is current until
`yscr_bind()` or `begin()` finds one, and draws into framebuffer 0 then
do nothing. Over 7200 presents in front (4 runs of 1800 frames) and all
the runs below, a buffer was free at every present.

- **What a draw between frames shows:** nothing. The buffer is not
  presented until a `begin()` takes it, and a frame draws every pixel.
- **The reads.** `ygfx_read_output()` reads framebuffer 0 after `end()`
  and before `flip()`, as documented; after `flip()` it reads the parked
  buffer, which holds an older frame on COMPOSITION and the last frame on
  DXGI_FLIP. GL STATE now says that framebuffer 0 holds nothing defined
  after `flip()`. `ygfx_read_scene()` reads the gfx's own target, and
  `ygfx_prime()` draws into the parked buffer and reads its own target.
  Capture copies in `end()`, before the flip.
- **Not changed:** DXGI_FLIP, the web backend and SIM. The code is in the
  COMPOSITION presenter only.

Rejected: binding a free buffer only in `yscr_bind()`, which
`ygfx_prime()` calls. That costs nothing in the frame, but GL from the
caller between frames without `yscr_bind()` would still wait for
minutes.

### Cost

The park, timed with QPC around it (`flipstats --backend composition`,
1800 frames a run): p50 13.2 to 16.2 us, p99 42 to 97 us, max 165 to
252 us, in 4 runs in a window in front, 4 fullscreen and 2 covered. It
runs after the present call has returned, so the frame is queued
already. In 4 runs of 1800 frames each, before and after: no drop, every
record tier 1; the present zone p50 554 to 632 us before and 588 to
616 us after; the header's own cost at p50 inside the run-to-run spread
(in `begin()` 10.5 to 12.3 us before, 10.3 to 11.7 after; in `flip()`
5.3 to 6.2 before, 5.1 to 5.9 after).

### After

On v0.10.3, the same machine:

- C, `prime_c.c` in front: 38.7 to 101.7 ms in 7 of 7 primes after a
  flip (window and fullscreen, ON and OFF; at once and after 60 frames).
- `tests/adapt/screen_window_test.c --only between`: a clear and a pixel
  read of framebuffer 0 between `flip()` and `begin()` in front, at once,
  after 1 s and after `wait_flip()`, window and fullscreen, ON and OFF:
  0.7 to 1.8 ms in 24 of 24 on COMPOSITION, 0.9 to 17.0 ms in 12 of 12
  on DXGI_FLIP (two runs). With the park removed (mutant `bf-01`) the first one did not
  return in 10 s, and the test's watchdog ended it with a FAIL line.
- Python, ysp-vis 0.10.3, CPython 3.14.3, in front: the text worker's
  repro 3 of 3 (prime 2 to 3 ms) and `prime_repro3.py` 6 of 6 (window and
  fullscreen, ON and OFF; 1 to 3 ms). The ysp-vis window tests: 87
  passed, with the text binding's `yscr_wait_flip()` before its prime
  removed.
- MATLAB R2023a: `test_mex_vis_window.m` primes right after a flip in
  front, without the `wait_flip` it had: under 1 s, 17 checks pass.

## Handles, no free buffer, display ids (v0.10.4)

Three open findings of v0.10.0 and v0.10.3, and a CI failure on 32-bit
Windows. Laptop of the measurements above (Iris Xe, Windows 11, AC), 2026-10-27.

### 5 handles an open and close

The cause: COMPOSITION's `open()` gets 5 event handles from DComp's
presentation API: `GetPresentStatisticsAvailableEvent` and `GetLostEvent`
of the presentation manager, and `GetAvailableEvent` of each of the 3
buffers. Each call returns a new handle that the caller owns. Release of
the manager and the buffers does not close them, and `close()` did not.
DXGI_FLIP closes its one waitable object, and had no leak.

`screen_window_test --only cycles` now lists the process's handles by
type (`NtQueryInformationProcess`, ProcessHandleInformation, and
`NtQueryObject` for the type name) after the second cycle and after the
tenth, and prints the new ones. 10 opens and closes of a 480 x 300
window a backend, settling off:

| | COMPOSITION OFF | COMPOSITION ON | DXGI_FLIP OFF | DXGI_FLIP ON |
|---|---|---|---|---|
| v0.10.3, cycles 2 to 10 | +40 (Event 40) | +40 (Event 40) | +0 | +0 |
| v0.10.4 | +0 | +0 | +0 | +0 |

The fix: `close()` closes the 5 events after it releases the objects.
The test checks that the count grows by less than 0.5 a cycle on both
backends, with the window thread ON and OFF. It checks the threads only
with the window thread ON: with it OFF, the thread count of the process
was 1 higher once at the end (COMPOSITION OFF, before and after the
fix), with no thread handle among the new ones. That is the OS's thread
pool, not the header. Mutants `lk-01` and `lk-02` (one close each removed).

### No free buffer after a present

v0.10.3 binds a free buffer after each present. With none free, no
surface is current until `yscr_bind()` or `begin()` finds one. That case
never came in the runs. The window test now makes it come:
`YSCR__COMP_IS_AVAILABLE(c, k, ok)` is the buffer query, and the test
defines it before it includes the header, to report every buffer busy
while a flag is set. The header's default is `IsAvailable`; nothing else
changes.

`screen_window_test --only nofree`, COMPOSITION in front, window thread
OFF and ON: a frame whose present finds no buffer free (the flag is set
after `begin()`: set before it, `begin()` itself finds none and waits),
then:

- framebuffer 0 is `GL_FRAMEBUFFER_UNDEFINED`: no surface is current;
- a clear and a pixel read return at once (0.0 ms; a 10 s watchdog ends
  the test with a FAIL line otherwise), the read sets
  `GL_INVALID_FRAMEBUFFER_OPERATION`, and the pixel buffer is unchanged;
- `yscr_bind()` while every buffer is busy leaves no surface current;
  with the flag cleared, `yscr_bind()` binds a buffer
  (`GL_FRAMEBUFFER_COMPLETE`);
- a `begin()` right after a present with none free returns `YSCR_OK`
  with a complete framebuffer 0, and 30 more frames and `wait_flip()`
  run.

16 of 16 checks pass. Mutants: `bf-02` (with none free, the shown buffer
stays bound) and `bf-03` (`yscr_bind()` does not look again).

### Display ids

`yscr_displays()` without a screen starts SDL's video for the call and
stops it after. SDL numbers the displays anew at each start (here id 1,
then 4 with the window thread), so an id from that list named no display
at the next `yscr_modes()` or `open()`. Two fixes were possible: document
0 (the primary display) as the only stable id, or match an id that is
no longer current. 0 alone does not let a program pick a second display
by a list, which is what the list is for. So the header matches:

- SDL never gives an id out twice in one process (`SDL_GetNextObjectID`),
  so a current id is used as it is, and a stale one cannot name another
  display by accident.
- `yscr_displays()` keeps the name and bounds of each id it gives out
  (the last 16).
- `open()`, `yscr_modes()` and `yscr_mode_multiple()` map an id that is
  no longer current to the current display with the same name and
  bounds, else to the only one with the same bounds, else to the only one
  with the same name. With no match, the id goes to SDL as before, and
  `open()` fails with SDL's message.

A custom presenter still gets `desc.display` as given
(`yscr_presenter_open.display`). `screen_window_test --only displays`:
an id listed with no screen gives the same mode list after SDL's video
started again, opens a screen, and gives the same list while the window
thread runs. Mutant `ds-01` (the match removed). One display here: the
match among two or more displays was not run.

### 32-bit Windows

CI's 32-bit MSVC build failed: MSVC has no `InterlockedExchange64` for
x86. The 64-bit stores (the abort state, the window thread's heartbeat
and release time) now go through one helper that uses
`InterlockedCompareExchange64` in a loop on x86 and
`InterlockedExchange64` elsewhere. COMPOSITION now refuses x86 as it
refuses ARM64: its C declarations of the COM methods that return a
struct by a hidden pointer were checked on x64 only
(`tests/compile/screen_com.cpp`), and AUTO falls back to DXGI_FLIP.
Checked with MSVC 19.51 x86 (vcvars32), `/W4 /WX`: `tests/compile/screen.c`
as C11 and with `YRT_NO_THREADS`, `screen.cpp` as C++17, and
`tests/adapt/screen_test.c` built and run (all checks pass). No x86
program opened a window: there is no x86 ANGLE here.

### Tests and mutants

On v0.10.4, this laptop: `screen_window_test` 110 of 110 checks (every
default case, launched in front, through `psy-guard.sh time`); MSVC
full ctest 97 of 97; MinGW ctest 80 of 80 (no SDL3 there) and the MinGW
gcc 16.1 `-Werror` compile checks with SDL3's headers; WSL2 gcc 11.4
`screen_test` with ASan and UBSan.

The window-thread, foreground and gap mutants and the 5 new ones, 37 in
all, ran through `tests/mutate/mutate.py`: the 20 on the core test
(`wt-01` to `wt-09`, `fg-01` to `fg-07`, `gp-01` to `gp-04`) with `-j 4`,
and the 17 on the window test (`wt-20` to `wt-29`, `fg-08`, `bf-01`,
`bf-02`, `bf-03`, `lk-01`, `lk-02`, `ds-01`) one at a time with
`--guard` (each run under `psy-guard.sh time`; see
[tests/mutate/README.md](../tests/mutate/README.md)). 37 of 37 killed.
`bf-02` (the shown buffer left bound when none is free) made the draw
between frames wait until the test's 10 s watchdog ended it: the case
the fallback exists for.

## macOS: the header owns the CAMetalLayer (v0.11.0, v0.12.0)

`YSCR_BACKEND_METAL` is the macOS swap path, and AUTO there. It needs
macOS 14 (Sonoma) or later, ysp's floor ([The macOS 14
floor](#the-macos-14-floor-v0140)). The probe of
[screen_macos_probe.md](screen_macos_probe.md) chose its shape; this note
says what was built from the probe's functional results, what waits on
its timing runs, and why the header calls Objective-C as it does.

All runs below: Apple M2 Mac mini, macOS 27.0.1, SDL 3.4.0, ANGLE from
Electron 43.8.3 (`EGL_VERSION` "1.5 (ANGLE 2.1 git hash: unknown hash)"),
2026-11-05. **The console session was locked for every run**, so no
frame reached the panel: every `presentedTime` was 0. Other agents
compiled on the machine at the same time. Nothing below is a timing
result.

### The path

SDL's EGL path on Cocoa gives a context, but ANGLE then owns the layer and
exposes no flip time, no present time and no depth control (probe,
section 5.5). So, as on Windows, SDL makes only the window:

1. The window has `SDL_PROP_WINDOW_CREATE_METAL_BOOLEAN`, and the header
   makes its CAMetalLayer with `SDL_Metal_CreateView`.
2. ANGLE's display is asked for the Metal back end by name
   (`EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE`): this ANGLE's default on macOS
   is OpenGL. The layer's device is the one `EGL_METAL_DEVICE_ANGLE`
   reports, so one device serves ANGLE's queue and the header's. ANGLE
   does not take a caller's device (`eglCreateDeviceANGLE` gives
   `EGL_BAD_ACCESS`) or queue.
3. Each frame, `acquire()` takes the next drawable and makes current a
   pbuffer on the drawable's own IOSurface
   (`EGL_ANGLE_iosurface_client_buffer`), one per IOSurface, kept in a
   cache of 6. The layer cycled 2 IOSurfaces per run here, and 3 or 4 in
   the probe's `share` runs.
4. `present()` ends ANGLE's frame with an `eglCreateSync` of type
   `EGL_SYNC_METAL_SHARED_EVENT_ANGLE` on the header's MTLSharedEvent, with
   the present id as the value, and `glFlush`. The header's command buffer
   waits on that value (`encodeWaitForEvent:value:`), draws the patch and
   the codes as blits from a staging buffer, copies code rows to a
   read-back buffer when asked, and presents with
   `presentDrawable:atTime:`.
5. The presented handler reports `presentedTime`. A time of 0 is a
   drawable not shown: the core estimates the flip, and it is OCCLUDED when
   the window's `occlusionState` lacks Visible or the session is locked.

GL's row 0 lands on IOSurface row 0, the top of the screen (checked with a
scissored clear read back from the IOSurface; ANGLE's Metal display has no
`EGL_ANGLE_surface_orientation`). So `rows_top_down` is 1 on METAL, as on
DXGI_FLIP and COMPOSITION, and ysp/gfx.h v0.17.3 takes its origin from it.

### The sync, checked by read-back

`tests/adapt/screen_metal_test.c` draws each frame with a GL clear and a
400-iteration fragment loop over the window, both writing the frame's
own value, and the header's command buffer copies the center pixel after
its wait (a test seam, `YSCR__MTL_SYNC_PIXEL`). The read-back needs no
shown frame, so it ran on the locked screen:

| Run | Frames read back | Not their own frame's |
|---|---|---|
| 8 bits, 800 x 600 window | 120 of 120 | 0 |
| 10 bits (`desc.color_depth` 10) | 30 of 30 | 0 |
| 8 bits, the wait removed (mutant mt-08) | 120 of 120 | 21, each the frame two before |
| 10 bits, the wait removed | 30 of 30 | 2, each the frame two before |

The seam waits for the frame before at each present, so every frame is
read; without the seam, a read-back still running at the next verify is
skipped, never waited for.

The code self test (all 256 values of each channel through the blits, in
the layer's format, into a texture and back) passed; 150 to 158 codes read
back in a 60-frame fullscreen run, none differing.

### Objective-C from C

The header must compile as C99 and as C++ on every platform, and its
implementation is one translation unit the caller picks. Two ways were
open: require that translation unit to be Objective-C on macOS, or call
the Objective-C runtime from C. The header calls the runtime from C:
`objc_msgSend` cast to each method's exact prototype (the ABI needs the
exact one: a `double` return, `MTLSize` and `MTLOrigin` passed by value),
with libobjc, CoreFoundation, CoreGraphics, CoreVideo and ANGLE loaded at
run time and no Apple header but `TargetConditionals.h`, `dlfcn.h` and
`mach/mach_time.h`. Reasons:

- ysp/video.h's AVFoundation backend, built at the same time by another
  agent, made the same choice (its worktree, `include/ysp/video.h`,
  "AVFoundation backend"): one rule for every Apple backend, and a caller
  builds every ysp header the same way.
- An Objective-C translation unit would change how the caller builds, and
  the bindings (`ysp-vis`, MEX) compile the headers as C.
- Apple's framework headers bring ApplicationServices and CarbonCore's
  globals into the caller's translation unit (ysp/video.h found `pi` and
  `Rect` there).

The cost is a Block: Metal's presented handler takes one. The header
builds global Block literals by hand to the Block ABI (`isa`
`_NSConcreteGlobalBlock`, flags IS_GLOBAL and HAS_SIGNATURE, signature
`v16@?0@8`), one per slot of a process-wide table of 64, so Metal's
`Block_copy` is a no-op and a handler that comes after its screen closed
finds its slot orphaned and frees it. A handler touches only that table.
Nothing measured contradicts the choice; the read-backs above and the
code self test went through it.

### The constants, measured (v0.12.0)

v0.11.0 shipped three unmeasured constants. The probe's timing session of
2026-11-05 ([screen_macos_probe.md](screen_macos_probe.md), section 6:
the M27Q at 59.9505, 120 and 144 Hz, unlocked, AC, **a Screen Sharing
session open**) set them, and found two more. The numbers below are the
probe's, on its own CAMetalLayer. This header's own frame loop then ran
in `tests/adapt/screen_metal_test.c` on the unlocked screen (800 x 600
window at 59.95 Hz, about 4.5 ms of GPU time a frame): 120 of 120 frames
shown, 119 of 119 a vblank after the one before, 0 dropped, tier 2; at 10
bits 30 of 30 and 29 of 29. That is a functional test, not a timing run.

| Item | Where | v0.11.0 | v0.12.0 | The rows |
|---|---|---|---|---|
| (a) the target passed to `presentDrawable:atTime:` | `YSCR__MTL_TARGET_LEAD` | planned vblank minus 0.25 period | minus 0.5 period | A drawable shows at the first presentedTime vblank at or after its target, less about 0.4 ms, not at the nearest: targets from 0.9 or 0.95 period before a vblank up to it showed on it (2155 of 2160, every kind and rate), 0.05 to 0.1 period after it a vblank late (360 of 360). Half a period is that window's middle. The tie that made "minus half a period" a risk is a rule of COMPOSITION's, not Core Animation's |
| (b) frames in flight | `YSCR__MTL_IN_FLIGHT`, `caps.max_in_flight` | 1 | 2 | The presented handler comes 0.30 to 0.76 ms after presentedTime, and a commit needs a period plus 0.65 to 1.18 ms before its vblank. So waiting for the frame before showed every other vblank (the probe's `links`: 1896 frames in 60 s, 1701 of 1896 steps of 2 vblanks); `begin()` now waits for the frame two before |
| (b) depth | `yscr__path_table` (COMPOSED) | 2 | 2 | Every commit up to phase 0.907 to 0.940 of a period showed two vblanks later, every kind and rate |
| (b) margin before a vblank | `YSCR__MTL_MARGIN_NS`, new `caps.margin_ns` | the core's, at most 1 ms | 2 ms | The deadline's excess over a period (up to 1.18 ms) plus the present call (encode and commit p99 0.45 ms, max 0.93 ms). `caps.margin_ns` is new in `yscr_caps`: a presenter states its own, and the core takes the larger |
| (b) drawables | `YSCR__MTL_DRAWABLES` | 3 | 3 | 2 or 3 changed nothing with one frame in flight (300 of 300 shown both); with two in flight and one drawn, 3 keep one free |
| a target far ahead | `YSCR__MTL_AHEAD_MAX_NS` | none | held until 0.5 s ahead | 0.1 to 1.0 s ahead showed on target (1 of 1 each); 2 and 5 s ahead did not show, and every later present in that layer was dropped for 6.5 s. `flip_at()` sleeps until such a target is 0.5 s ahead (`held_far=`) |
| (c) the tier | `yscr__mtl_tier()` | 3 | 2 for a shown flip, every kind | presentedTime moved with every late frame (30 of 30 per kind and rate), always came before its handler, and sits on its own grid to under 1 us (0.64 us max): an OS time for the vblank the frame showed on, not a plan. That is DXGI_FLIP's composed tier ([Tiers](#tiers)). Not tier 1: no photodiode has run, CVDisplayLink's `inNow` puts the vblank 0.40 to 0.45 period earlier (one of the two is not the scanout's vblank, or neither is), and the window kinds measured alike, so no path is known. An estimated flip (presentedTime 0) stays tier 3 |

The clean session (no Screen Sharing, v0.14.1) checked every row and
changed none ([Window kinds and Screen Sharing](#window-kinds-and-screen-sharing-v0141)).

A planned vblank is on presentedTime's grid: the header's grid comes from
the onsets it records, which are presentedTime. CADisplayLink's timestamp
lies on the same grid (0.1 us apart); CVDisplayLink's `inNow` does not.

macOS has no public call that says a window goes direct to the display,
so every Metal flip's path is COMPOSED and the describe line names the
window kind instead. The probe's runs decide each row: `present`'s
`quant`, `asap` and `late` runs per kind, and the photodiode for the tier
(probe, sections 6 and 8).

### Other choices

- **The period.** Never the mode's rate, which is a rounded label: the
  "60 Hz" HDMI mode measured 16.680417 ms nominal, 59.9505 Hz, where
  CoreGraphics and SDL say 60.000. v0.11.0 took CVDisplayLink's nominal
  period at open; from v0.13.1 it comes from CADisplayLink
  ([below](#the-period-from-cadisplaylink-v0131)). The flips refine it as
  on Windows.
- **The clock.** `presentedTime` is mach time. ysp/rt.h from v0.10.0 counts
  mach time on macOS; before, it read `CLOCK_MONOTONIC`, which counts
  sleep and is slewed (+0.58 ppm against mach time, measured). The
  presenter correlates the two at open and every 10 s, so it is right
  with either; with v0.10.0 the offset is 0 to the bracket.
- **Threads.** Cocoa keeps windows on the main thread and SDL's video
  lives there, so open(), the frame loop and close() run on the main
  thread; open() refuses another. There is no window thread on macOS.
- **nextDrawable** gives up after its 1 s timeout
  (`allowsNextDrawableTimeout`, set explicitly). A frame without a drawable
  draws into a 1 x 1 pbuffer and is estimated. With the screen locked the
  first probe run waited the full second on most frames in a window at
  normal level; in these runs every drawable came at once.
- **10 bits.** ANGLE's Metal display has no 10-bit or half-float config, so
  `desc.color_depth` 10 gives a BGR10A2Unorm layer and pbuffers on its
  'l10r' IOSurfaces with an RGBA8 config: a pbuffer's format is the
  IOSurface's. 8-bit codes are spread over 10 bits (`v << 2 | v >> 6`).
  `color_depth` 10 on another backend is refused, not drawn in 8 bits.
- **Input.** SDL's event times are restamped onto ysp_rt as on the other
  backends (SDL's ticks are mach time plus a constant, measured). SDL 3.4's
  Cocoa key and button times are late by a per-process offset; from
  v0.15.0 `yscr_event_input()` gives them `NSEvent.timestamp` (see "Keys
  and mouse buttons: NSEvent.timestamp").

### The period from CADisplayLink (v0.13.1)

CVDisplayLink is deprecated in macOS 15 and does not exist on iOS. METAL
used it only for the period at open. From v0.13.1 the period comes from
`CADisplayLink` (`-[NSView displayLinkWithTarget:selector:]`). From
v0.14.0 CVDisplayLink stays only for a CADisplayLink that gives no
callback ([The macOS 14 floor](#the-macos-14-floor-v0140)).

Which number: the probe's `links` run gave three candidates for the M27Q
at its "60 Hz" (docs/screen_macos_probe.md, 6.4):

| Source | Value | Note |
|---|---|---|
| CADisplayLink `duration`, `targetTimestamp - timestamp` | 16680417 ns (+-1) | the nominal timing, as CVDisplayLink's nominal period |
| CADisplayLink `timestamp` step | 16680375 ns (+-1) | 3597 steps; on presentedTime's grid, 0.1 us apart |
| presentedTime fit | 16680370 to 16680380 ns | what the flips show |

So the period is the step of the timestamps: the span of up to 6 callbacks
over the whole periods in it, counted on the duration, so a missed
callback keeps its count. There is no read without a callback: before its
first callback the link's numbers are 0. Waiting for callbacks inside
open(), which runs on the main thread: the link is added to the main run
loop's common modes, and open() runs the run loop in its default mode in
10 ms steps, pumping SDL's events (the events stay in SDL's queue), for
up to 6 callbacks or 250 ms. Nothing waits on another thread, so it
cannot deadlock against SDL's pump. With fewer than 2 callbacks it falls
back to CVDisplayLink: with the display asleep (the session locked),
CADisplayLink gave no callback in 2 s while CVDisplayLink went on.

The describe line names the source and, from v0.14.0, why:
`period=16680375ns(cadisplaylink)`, `(cvdisplaylink-no-callback)` or
`(mode-no-display-link)`.

**The phase.** CADisplayLink's timestamps also lie on presentedTime's
grid, so they could anchor the grid at open, before the first flip, where
the backend now takes it from the first presentedTimes. Not done in this
change.

**Not run with a callback.** During this change the display was asleep,
so every real open here took the fallback: `period=16680417ns(cvdisplaylink)`
(`cvdisplaylink-no-callback` from v0.14.0).
`tests/adapt/screen_metal_test.c` injects the callbacks' timestamps
(`YSCR__MTL_LINK_TIMESTAMPS`): six a 16680375 ns step apart with a
duration of 16.680417 ms give `period=16680375ns(cadisplaylink)`, and none
gives `(cvdisplaylink)`. The core test checks the rule with the measured
numbers, a missed callback, one callback and a step off the duration.
Mutants `mt-17` and `mt-18`, 2 of 2 killed.

### The macOS 14 floor (v0.14.0)

macOS 14 (Sonoma) is ysp's macOS floor ([rig_spec.md, section
9](rig_spec.md#9-platform-matrix)). Up to v0.13.1, METAL ran on macOS 12
and 13 untested: it asked `respondsToSelector:` before CADisplayLink
(macOS 14) and before NSScreen's refresh intervals (macOS 12), and took
CVDisplayLink's nominal period where CADisplayLink was missing. v0.14.0
removes those branches. `open()` asks
`-[NSProcessInfo isOperatingSystemAtLeastVersion:]` for 14.0, once per
process and before ANGLE loads, and on an older system fails with:

```
ysp_screen: METAL needs macOS 14 (Sonoma) or later, ysp's floor; this system is older. Update macOS.
```

That call reports the real version to a binary built with any SDK, where
the system's version file can say 10.16 to an old one. An open that
succeeded on macOS 12 or 13 now fails, so the version is a minor bump.

**The case with no callback.** CADisplayLink gives no callback with the
display asleep, so a period source is still needed for it. The candidates
were measured with the display asleep (2026-11-05, Apple M2 Mac mini,
macOS 27.0.1, the M27Q on HDMI at its "60 Hz", whose period is 16680375
ns; the session locked; a 320 x 240 SDL window; 1 s each):

| Source | Asleep | Period it gives |
|---|---|---|
| CADisplayLink | 0 callbacks | none |
| CAMetalDisplayLink, drawables dropped | 59 updates | `targetPresentationTimestamp` steps exactly 16666667 ns: a 60.000 Hz grid, not the display's, 0.8 ms a second off |
| CAMetalDisplayLink, drawables presented | 3 updates | too few to read |
| CVDisplayLink | callbacks go on | nominal 16680417 ns, 2.5 ppm off |

CAMetalDisplayLink would remove the deprecated API, but asleep it gives a
period that is wrong by 0.08%, and nothing in its updates says so. So
CVDisplayLink stays, only for this case, and the header's comment says
why. The mode's rate stays the last resort, when neither link exists.

**Tests.** No system older than 14 was at hand.
`tests/adapt/screen_metal_test.c` acts as one through a seam
(`YSCR__MTL_OS_14`): the open fails with the message above, and then
the real system opens. Mutants `mt-19` (the check skipped), `mt-20` (the
check asks for 99.0, so only the real call can kill it) and `mt-21` (the
describe line drops the reason), 3 of 3 killed.

### The first open's period (v0.14.1)

The clean timing session's run of `tests/adapt/screen_metal_test.c` (25
of 25 passed) showed the process's first open with
`period=16680417ns(cvdisplaylink-no-callback)` and every later open with
`period=16680375ns(cadisplaylink)`, on an unlocked, awake screen.

The reason was measured, not guessed. A scratch copy of the header logged
each callback of the open's CADisplayLink (2026-11-05, the Mac of the
sessions, unlocked, no Screen Sharing, the M27Q at "60 Hz"; 7 processes,
two opens each). The first open did get its 6 callbacks, within 124 to
141 ms of the link's start in the three processes that logged it; the window was visible and the app not yet
active. But the first callback's timestamp was off the grid:

| Open | Step from the first timestamp to the second | Steps after it | Source before v0.14.1 |
|---|---|---|---|
| A process's first, windowed (5 processes) | 15.8, 19.8, 24.9, 21.1, 25.2 ms | 16680375 ns (+-1), every one | cvdisplaylink (5 of 5) |
| A process's second, windowed (5) | 16680375 ns | 16680375 ns | cadisplaylink (5 of 5) |
| A process's first and second, fullscreen (2 processes) | 16680375 ns | 16680375 ns | cadisplaylink (4 of 4) |

With the first step 1 to 5 ms off, the span of the 6 timestamps was no
whole number of periods within 1%, so the rule returned no period, and
the open fell back as if no callback had come. The fix leaves the first
timestamp out: the period is the span of the callbacks after the first
over the whole periods in it (at least 3 callbacks). Of the options:

- Wait after the window is shown and pumped: the callbacks already came
  in time (6 in 141 ms); the timing of the window was not the fault.
- Keep the link alive and read it once frames run: a second run-loop
  source for the life of the screen, for a number that 5 timestamps give
  at open.
- Take the period from presentedTime and drop the open's source: the
  flips refine the period already (`measured=` in the describe line), but
  the open needs a period before the first flip.

Leaving one timestamp out costs nothing and adds no thread or wait. A
CADisplayLink whose callbacks came but whose steps are off its duration
is now named: `(cvdisplaylink-link-off-grid)`, apart from
`(cvdisplaylink-no-callback)`.

After the fix, on the same screen: `tests/adapt/screen_metal_test.c` 33 of
33, its first check the process's first open,
`period=16680375ns(cadisplaylink)`. The test injects the measured case
(the first timestamp 15.8355 ms before the second: `cadisplaylink`) and a
late step of 25 ms (`cvdisplaylink-link-off-grid`); the core test checks
the rule with the first steps of 15.8 and 25.2 ms. Mutants `mt-22` (the
first timestamp kept) and `mt-23` (the off-grid case named as no
callback), 2 of 2 killed.

### Window kinds and Screen Sharing (v0.14.1)

The probe's clean session (docs/screen_macos_probe.md, section 6; the
Mac above, unlocked, AC, no Screen Sharing) repeated the first session,
which had a Screen Sharing session open. Per kind, on presentedTime's
grid:

| Kind | One commit: the latest that missed its vblank, a period plus | Three queued: commit to presentedTime p50 | Same, with Screen Sharing |
|---|---|---|---|
| window | 0.80 ms | 48.2 ms (3 periods) | 47.4 ms |
| borderless | 1.17 ms | 48.7 ms (3) | 48.1 ms |
| fullscreen space | 1.02 ms | **32.6 ms (2)** | 48.4 ms |
| fullscreen (a space too) | 0.53 ms | **32.6 ms (2)** | 48.5 ms |
| exclusive 120 Hz | 1.03 ms | 23.7 ms (2.84) | 23.8 ms |
| exclusive 144 Hz | 1.00 ms | 19.5 ms (2.81) | 19.6 ms |

The depth stays 2 in every kind, from the kind of evidence each option
needs:

- **Depth 1 in a space would plan wrong.** A depth of 1 says that a frame
  committed less than a period before a vblank shows on it. In a space,
  as in a window, no commit under a period plus 0.3 ms before a vblank
  showed on it, and `presentAtTime` one vblank ahead showed a vblank late
  in 140 of 140 per kind.
- **The space's shorter queue is the queue, not the deadline.** With three
  drawables queued, a space showed each frame two periods after its
  commit and a window three; with two drawables both showed two. The
  header keeps two frames in flight (`begin()` waits for the frame two
  before), so its queue never holds three. Its own loop in a space
  (`tests/adapt/screen_metal_test.c`, `fullscreen`, 1920 x 1080, `kind=space`):
  120 of 120 shown, 119 of 119 a vblank apart, 0 dropped, at depth 2.
  The first try, with the window's per-pixel load at 4.3 times the
  pixels, dropped 24: the GPU's load, not the depth.
- **No learning from presentedTime.** The core already corrects the grid
  from the flips, and a frame shown early cannot happen with a target
  half a period before its vblank. A learner would add a state with
  nothing measured for it to find.

The space's shorter queue is the one sign of a direct path: a display
that holds the drawable on screen while the compositor holds no copy
would give it. Nothing public says so, and the Metal HUD's present mode
was not recorded (`log stream` needs an administrator here). The tier
stays 2.

**Screen Sharing changes the queue.** With a Screen Sharing session open,
a space queued as a window (three periods), and SDL3's EGL path showed
48.6 ms in a space against 32.8 ms without it. The one-commit deadline
and the `presentAtTime` rule did not change. A window opened right after
another process left a fullscreen space showed no frame with Screen
Sharing (3 of 3) and every frame but the first 6 without it (1 of 1).
For timing work, close Screen Sharing. The header does not detect it: no
public call that says the screen is being captured was found.

### Keys and mouse buttons: NSEvent.timestamp (v0.15.0)

SDL 3.4 on Cocoa stamps a key or mouse event with `NSEvent.timestamp`
plus a process-wide offset: the shortest read delay seen so far (3.8 ms
on a process's first key, then down to 0.2 ms; probe, 6.9). It stamps a
modifier key when it reads it. Its `Cocoa_GetEventTimestamp` stamps keys
(also the key IME holds), mouse buttons, motion and the wheel, touch,
pinches and pen events. v0.15.0 corrects keys, modifier keys and mouse
buttons: the presses and releases a response collector reads.

**The monitor.** The first METAL open in a process adds a local NSEvent
monitor (`+[NSEvent addLocalMonitorForEventsMatchingMask:handler:]`) for
key down and up, flags changed and the mouse buttons; the last close
removes it. The handler is a global Block built by hand in C, as the
presented handler is (`isa` `_NSConcreteGlobalBlock`, flags IS_GLOBAL and
HAS_SIGNATURE, signature `@16@?0@8` for `NSEvent* (^)(NSEvent*)`), in
static storage, so it lives as long as the process. It returns the event,
so SDL still gets it. It writes the event's class and
`trunc(NSEvent.timestamp x 1e9)` into a ring of 64 entries of 16 bytes:
no allocation per event. It runs on the main thread inside SDL's pump.
`yscr_event_input()` may run on another thread, so it copies the ring and
keeps an entry only when the entry's sequence number was the same before
and after the copy.

**The pairing key: SDL's own arithmetic.** SDL's stamp minus
`trunc(NSEvent.timestamp x 1e9)` is SDL's offset, the same for every key
and button until SDL lowers it. So the right entry gives that offset to
the nanosecond, and an entry SDL did not deliver gives a larger one (it
is older). A field of the event would not do: a key's scan code comes
from SDL's private key-code table, and a button misses SDL's remapping (with
SDL's hint, a control-click is the right button). The rule
(`yscr__nsm_pick`):

- An entry pairs only within its class: key down, key up, button down,
  button up, and each of the 8 modifier keys apart.
- Within a class an SDL event takes the oldest entry not taken whose
  NSEvent came at most 50 us after SDL's stamp (on the ysp_rt clock) and
  at most 1 s before it.
- Once an offset is known, the entry's offset must be no larger than the
  last match's plus 1 us: SDL's offset never grows. An older entry that
  fails is an event SDL did not deliver (a click on the title bar, a key
  IME took, an event posted to the process that SDL drops) and is passed
  over.
- A modifier key has no offset (SDL stamps it at the read): it takes the
  oldest entry of its own key within the window.
- An SDL event with no entry keeps SDL's stamp, and the adapter gives it
  tier 3: SDL's own key-ups at a focus loss, a release outside the window.

Key repeats have NSEvents of their own and pair as key downs. A second
METAL screen shares the monitor; the ring is per process, as SDL's queue
is. The describe line has `os_stamps=found/tried(passed n, bias us)`, the
bias being SDL's lateness at the last match. `caps.os_input_stamps` says
whether the monitor was made; pass it to `yin_sdl_ctx.os_stamps` for the
source entry (ysp/input.h v0.5.0). `yscr_poll()`'s time is still SDL's.

**Measured** through the rig's path, `tests/adapt/screen_metal_test.c`'s
`input` case with `YSCR_TEST_INPUT_N=200`:

Conditions: 2026-11-05 11:58, Apple M2 Mac mini, macOS 27.0.1, the M27Q
on HDMI at "60 Hz", AC, the session unlocked, no Screen Sharing, nothing
else running; Accessibility granted to the terminal's app. An 800 x 600
METAL window ran a frame loop that read its events once a frame with
`yscr_poll()` and `yscr_event_input()`, the path a rig uses. A thread
posted F19, the left button at the window's center, and (to the process
only) the left shift key, 40 to 50 ms apart at a random phase to the
frames, 200 of each per pass. "post" is the mach time just before the
post call. The test's own local NSEvent monitor gave the reference
NSEvent.timestamp. Times in us.

| Pass, kind | n | SDL's stamp kept | \|stamp - NSEvent\| max | stamp - post: min / p50 / p99 / max | SDL's stamp - post: min / p50 / p99 / max |
|---|---|---|---|---|---|
| `CGEventPostToPid`, key down | 200 | 0 | 0 | 68.8 / 113.7 / 3781 / 44567 | 585.5 / 782.4 / 6598 / 52827 |
| `CGEventPostToPid`, key up | 200 | 0 | 0 | 49.3 / 115.3 / 3139 / 15397 | 566.0 / 772.1 / 4311 / 23657 |
| `CGEventPostToPid`, left shift down | 200 | 0 | 0 | 68.1 / 113.6 / 2578 / 2803 | 546.0 / 8411 / 16940 / 17150 |
| `CGEventPostToPid`, left shift up | 200 | 0 | 0 | 50.7 / 112.3 / 2788 / 3211 | 499.5 / 8996 / 16837 / 17049 |
| `CGEventPostToPid`, left button down and up | 200 each | SDL delivered none | | | |
| `CGEventPost(kCGHIDEventTap)`, key down | 200 | 0 | 0 | 75.8 / 135.7 / 3724 / 3743 | 592.4 / 652.4 / 4240 / 4259 |
| `CGEventPost(kCGHIDEventTap)`, key up | 200 | 0 | 0 | 73.2 / 125.8 / 2933 / 3531 | 589.9 / 642.4 / 3450 / 4048 |
| `CGEventPost(kCGHIDEventTap)`, left button down | 200 | 0 | 0 | 74.4 / 136.0 / 2917 / 3582 | 591.0 / 652.6 / 3433 / 4099 |
| `CGEventPost(kCGHIDEventTap)`, left button up | 200 | 0 | 0 | 64.2 / 122.5 / 3194 / 3725 | 580.9 / 639.1 / 3711 / 4242 |

Every pair was exact (1600 of 1600). The details are in
[input.md](input.md#macos-nsevent-stamps-v050). With the test's
default of 20 events per kind it is one of the Metal test's checks, and
it runs only where posting is allowed (`CGPreflightPostEventAccess`) and
the session is unlocked.

**Not verified:** real keys and mice (scan, debounce, USB poll, and a
real event's path to the window server); IME composition; a second
window or screen open at the same time; events while another library's
NSEvent monitor eats events; touch, pinch, pen, motion and wheel times
(SDL's stamps, unchanged).

### Variable refresh refused (v0.13.0)

A ProMotion MacBook, or an external display in an Adaptive-Sync mode,
varies the time between frames on its own. The core's grid assumes a
fixed period, so every planned onset, every snap and every record would
be wrong, and no flag would say so; Psychtoolbox's users on 120 Hz
ProMotion MacBooks missed 341 to 401 of 600 deadlines
(psychtoolbox.discourse.group, thread 5767).

The header already refuses variable refresh (VARIABLE REFRESH):
`desc.vrr` fails the open until the panel has passed the photodiode
interval sweep and the luminance-versus-interval sweep. A display that is
in a variable mode is that same variable refresh, chosen by the system
instead of the caller, so METAL refuses it the same way. The other
options were weighed against the existing rules:

- Open with every record flagged and tier 3: the header does that only for
  what it finds while running (the sync guard, off-grid times), never for a
  condition it knows at open; and a run whose every onset is wrong in a
  known way is not a run to make.
- Report `caps.vrr_capable` and open: the field says what a panel could
  do; it does not make the fixed grid right.

So `open()` reads the window's `NSScreen.minimumRefreshInterval` and
`maximumRefreshInterval`, and when they are more than 0.1% apart it fails
with:

```
ysp_screen: METAL: the display is in a variable refresh mode (8.333 to 33.333 ms a frame), refused as desc.vrr is. Set a fixed rate: System Settings > Displays > Refresh Rate, not ProMotion; on an external display, its variable refresh off
```

The intervals are in the describe line as `refresh_interval=min..maxns`;
the M27Q at its "60 Hz" reads `refresh_interval=16680416..16680416ns`
(measured, 2026-11-05). The display link's nominal period is no help: it
is a single number. A mode change or a move to another screen while the
screen is open is not watched (the backend tracks neither).

**Not tested on a variable display.** No ProMotion or variable-mode display
was attached. `tests/adapt/screen_metal_test.c` injects the intervals
through a test seam (`YSCR__MTL_SCREEN_INTERVALS`): 8.333 to 33.333 ms is
refused with the message above, 8.333 and 8.333 ms (ProMotion set to a
fixed 120 Hz) opens, and the real screen's intervals are equal. The core
test checks the rule and that the message fits the screen's error buffer
(255 characters) at the widest numbers. Mutants `mt-14` to `mt-16`, 3 of
3 killed. Whether a ProMotion panel at a fixed rate in System Settings
reports equal intervals is how Apple documents the properties, not
something measured here.

### The panic, raw mice, the gamma table and the code risk (v0.16.0)

Before v0.16.0 four features were Windows only: the panic watchdog, raw
mice, the OS gamma ramp and the code risk state (METAL left
`CODE_RISK_STATE_UNKNOWN` set). v0.16.0 brings each to METAL with the
records, flags, events and describe-line words of Windows.

Conditions of every run below: Apple M2 Mac mini, macOS 27.0.1, SDL
3.4.0, ANGLE from Electron 43.8.3, the Dell M27Q on HDMI in its 1920 x
1080 mode at "60 Hz", AC, 2026-11-05. The coordinator said the machine
was quiet; the load average before each run was 0.7 to 1.4. The session
was locked for the first runs and unlocked for the rest; each table says
which.

#### Permissions

Read with the probe in the worker's scratch (`CGPreflight*`,
`IOHIDCheckAccess`), with Accessibility and Input Monitoring granted to
VS Code, which runs the shell:

| Call | Result | What it needs |
|---|---|---|
| `CGEventTapCreate`, listen-only, `kCGAnnotatedSessionEventTap` | made | Input Monitoring (`CGPreflightListenEventAccess` 1) |
| `IOHIDManagerOpen`, mice | `kIOReturnSuccess`, the Dell MS116 listed | Input Monitoring (`IOHIDCheckAccess` granted) |
| `CGGetDisplayTransferByTable`, `CGSetDisplayTransferByTable` | read and set | none |
| a screen capture (`CGPreflightScreenCaptureAccess`) | 0 | Screen Recording, not granted |

Both permissions were granted here, so which one Apple checks for the tap
was not separated: Apple documents listen-only taps under
`CGPreflightListenEventAccess` (Input Monitoring). `open()` names Input
Monitoring when the tap or the HID manager is refused. No run was made
without the permission.

#### The panic

The frame loop owns the main thread on macOS, so a hung loop stops the
process's events. The candidates for a key while the main thread hangs:

| Candidate | Thread | Permission | Decision |
|---|---|---|---|
| `NSEvent` global or local monitor | main | Accessibility for global | no: it needs the main thread |
| `IOHIDManager` on the keyboards | its own | Input Monitoring | no: it does not say which process the key goes to |
| `CGEventTap`, listen-only, at the annotated session tap | its own | Input Monitoring | yes |

The tap sees each key after the window server chose its target, and
`kCGEventTargetUnixProcessID` says which process gets it. That is the
counterpart of Windows' foreground check: a key that another program in
front takes does not count. Measured: posted presses reached the tap
with the hung child's pid, after 1.5 s and after 7 s of a hang (the
beach ball). macOS has no "Ghost" window: the hung app keeps its keys.

"Release the display" on macOS. The main thread owns every window, so no
other thread can order a window out or leave fullscreen safely. Windows'
rule is kept with the main run loop in the window thread's place: an
observer on the main run loop (entry, before waiting, exit) stamps a
heartbeat, and the release is a function on the main queue
(`dispatch_async_f`), which the main thread runs only while it pumps.
While the heartbeat is newer than 1 s the watchdog asks for a release
(each METAL window leaves fullscreen and is minimized; the cursor
shows), else the process ends. A host that waits on the main thread
(`input()`, MATLAB's `pause`) does not pump, so on macOS a panic there
ends the process; on Windows the window thread keeps answering. The
gamma tables and the display mode need no release: the window server
puts back the tables and takes down the windows of a process that ends.

`_exit(99)`, not `exit()`: atexit handlers can wait on locks the hung
thread holds. `panic_fn` runs on its own thread for at most 1 s first.

`tests/adapt/screen_metal_test.c` runs a child that opens a screen with
`desc.panic` (a window through the test seam `YSCR__PANIC_WINDOWED`, or
a fullscreen space), runs 60 frames, then sleeps on its main thread. The
parent waits on the child's exit with `kqueue` (`NOTE_EXIT`). "post":
Shift+Esc posted with `CGEventPost(kCGHIDEventTap)`, 3 presses 300 ms
apart. "seam": the same presses given to the tap's function
(`yscr__wd_feed`). Unlocked; ms from press 3 to the child's exit:

| Case | Runs | Exit code | Press 3 to exit, ms |
|---|---|---|---|
| post, window, hang 1.5 s | 6 | 99, panic_fn ran | 17.0 to 25.5 |
| post, fullscreen space, hang 1.5 s | 6 | 99, panic_fn ran | 24.1 to 29.6 |
| post, window, hang 7 s | 3 | 99 | 23.4 to 25.1 |
| post, fullscreen space, hang 7 s | 3 | 99 | 26.8 to 29.6 |
| seam, window (locked and unlocked) | 3 | 99 | 17.8 to 23.9 |
| seam, fullscreen space (locked and unlocked) | 7 | 99 | 23.4 to 30.6 |
| seam, presses for another pid | 2 | 3 (lived) | no panic |

Windows' table for comparison: 28 to 85 ms in a window, and 1.16 s for a
switched mode to come back.

Release, the child pumping SDL (`SDL_PumpEvents()` every 10 ms) without
`begin()` and `panic_grace_ns` 1 s: the release ran 1.7 to 10.6 ms after
the panic began (the 9 runs that printed it, window and space, locked and unlocked), the
child's next `begin()` returned `YSCR_QUIT` with 0x21 and the one after
with 0x20, and it exited 0 after close. In one locked run the fullscreen
window was not minimized at the first ask (a minimize during the space's
exit); `begin()` on a released METAL screen asks again.

Not measured: keys pressed by hand (posted keys only), the injected flag
on a hardware key (`kCGEventSourceUnixProcessID` 0 is assumed), a
switched display mode (METAL opens in the desktop's mode unless
`desc.mode` names one).

#### Raw mice

| Candidate | Per device | Unaccelerated | The device's rate | Permission |
|---|---|---|---|---|
| `NSEvent` deltas | no (one pointer) | no: `deltaX` is after acceleration | no: coalesced to the run loop | none |
| `IOHIDManager` values on a thread of its own | yes (one `IOHIDDevice` each) | yes: the report's counts | yes: one callback per report | Input Monitoring |

The header uses `IOHIDManager`. IOKit parses each report, so the header
parses no device bytes (no fuzz target): it groups the values that share
a device and `IOHIDValueGetTimeStamp` into one report, and closes the
last one when its run loop goes idle (an observer, before waiting). The
stamp is the time the HID driver took the report, in mach time, which is
the ysp_rt clock from ysp/rt.h v0.10.0. The device id is the low 32 bits
of the IORegistry entry id; a `YSCR_EV_DEVICE` record per mouse (present
at open, added and removed at the next `begin()`), so the reports carry
no `YIN_UNLISTED`. Measured (locked and unlocked): `open()` with
`desc.raw_mice` made one `YSCR_EV_RAW_MICE` ON record and a
`YSCR_EV_DEVICE` record for "Dell MS116 USB Optical Mouse" (the name cut
at 23 bytes).

The measured numbers come from the hand run of v0.16.1's reader
(`YSCR_TEST_MOUSE_S=5 test_screen_metal rawmice`, 2026-11-05 17:19:47, a
person moving the Dell MS116 behind a USB hub, unlocked, a quiet
machine; 6 of 6 checks):

| Quantity | min | p50 | p99 | max |
|---|---|---|---|---|
| Report interval, ms (626 reports) | 7.926 | 8.000 | 8.055 | 8.074 |
| Callback minus the value's stamp, us (1159 values kept) | 55.3 | 122.3 | 266.2 | 485.7 |

The mouse reports at 125 Hz. The reader's callback comes 55 us after
the HID driver's stamp at the least, 122 us at the median, 266 us at p99
and 486 us at most. Of 1878 values, 1159 were kept, 719 were empty (a
relative 0) and none stale; no report was unlisted and no event was
earlier than the one before from its device. The USB poll comes before
the stamp and is in none of these numbers.

The runs before it are the history of the bug below. The first (v0.16.0's
reader, 16:49, the same mouse and conditions):

| Quantity | min | p50 | p99 | max |
|---|---|---|---|---|
| Report interval, ms (850 reports) | 7.926 | 8.000 | 8.069 | 256.015 |
| Callback minus the value's stamp, us (1749 values) | 63.2 | 257.9 | 43,544,192.6 | 43,960,241.9 |

The maximum interval is a pause of the hand. The median callback time
of that run (258 us) and its p99 and maximum include the stale values;
the clean numbers are the table above. No report was unlisted.

**A bug, fixed in v0.16.1: stale values.** At least 1% of the values had
stamps 43.5 to 44.0 s old: from before the open. `IOHIDDeviceGetValue`
shows that IOKit keeps each input element's last value with the stamp
of the report that set it (read on the MS116 at rest: X and Y 95 s old,
button 1 130 s, the wheel 155 s, buttons 2 and 3 hours). With no hand on
the mouse a probe got no value at all in 2 s after `IOHIDManagerOpen`:
the old values come with real reports. The reader grouped them by their
stamps into reports of the past. No event in the run had such a time
(the smallest interval was 7.9 ms, not negative).

The first fix (a value dated before the reader's start, or before the
newest stamp taken from its device, is stale) went to two more hand runs
(17:07:54 and 17:08:10, the same mouse, 5 s each), which printed every
value more than 1 s old or dropped. Run 2: 1089 values; the old ones
were all relative elements at 0: the wheel's 0 (page 1, usage 0x38) with
every report of a move, dated the wheel's last real change (12.6 to 30.1
s back, the stamp fixed, so the age grew by 8 ms a report), and X's or
Y's 0 dated the report before (8 to 192 ms back). So IOKit calls back
with a relative element's 0 after a report that moved it, with the
element's previous stamp. The stamp rule dropped most of them (531
stale), but a 0 whose stamp was the newest taken (the last report before
a pause) was kept and arrived up to a pause later: the kept values'
callback minus stamp had a p99 of 8.16 ms (one report) and a maximum of
72.2 ms. Such a 0 opened a report of its own with no event in it (no
event went back in time; the test now checks that per device) and closed
the report it arrived in early.

The fix, both in v0.16.1:

- **Empty values.** A relative element at 0 (X, Y, the wheel, AC Pan) is
  dropped before any other rule and counted as empty: it adds nothing to
  a report. An absolute axis at 0 is a position and is kept.
- **Stale values.** A value dated before the reader's start (taken just
  before `IOHIDManagerOpen`) or before the newest stamp the reader took
  from its device is dropped and counted (the values of one report share
  a stamp, and a device's reports come in order). With the empty rule
  first, this catches a cached button value.

The describe line says `raw_mice=reports(inactive n, stale n, empty n)`.
The other option was `IOHIDDeviceRegisterInputReportWithTimeStampCallback`,
which gives each report once with its time. It was not taken: the header
would then parse the report bytes against the device's report descriptor
itself, a parser of untrusted input that needs a fuzz target, where now
IOKit parses them. The rules are three compares a value.

The rawmice case checks the rules without a hand on the mouse, and
whatever the mouse does meanwhile: it gives the reader's own function,
with an accumulator and a device id of the test's own, a button value
dated 44 s back, a fresh one, one older than the fresh one, and a
relative X of 0; only the fresh press and its release may reach
`yscr_event_input()`. (In the 17:08 run that check failed only because it
counted the reader's real drops while the mouse moved: 48, not 2. It now
counts the injected values by their stamps.) The hand run checks that no
event of a device is earlier than the one before, and that every kept
value came within 8 ms (one report) of its stamp. The hand run of these
fixes (17:19:47) passed 6 of 6: the table at the top of this subsection.
Its describe line said `raw_mice=946(inactive 0, stale 2, empty 1082)`;
the 2 stale are the injected values. A run at 17:15:37 got no report
because nobody moved the mouse; it is not a result. To run it again:

```
YSCR_ANGLE_DIR=/Users/brainworks/ysp-deps/angle-electron-43.8.3 YSCR_TEST_MOUSE_S=5 ./build/macos/test_screen_metal rawmice
```

#### The gamma table

A probe in the worker's scratch, the M27Q, 1024 entries a channel (its capacity):

| Call | Locked: min / p50 / p99 / max, us | Unlocked |
|---|---|---|
| `CGGetDisplayTransferByTable`, 1024 (200) | 36.4 / 137.9 / 250.6 / 866.1 | 30.8 / 125.8 / 221.3 / 222.0 |
| the same, 256 (200) | 36.2 / 38.5 / 59.1 / 1593.2 | 31.7 / 43.2 / 64.4 / 64.9 |
| `CGSetDisplayTransferByTable`, the same table (50) | 37.5 / 41.1 / 47.7 / 3276.4 | 42.3 / 49.0 / 60.2 / 3048.8 |
| set and read back (50) | 71.7 / 77.3 / 104.7 / 284.1 | 86.5 / 92.2 / 100.6 / 103.7 |
| `CGDisplayRestoreColorSyncSettings` (20) | 23.1 / 24.9 / 26.2 / 37.4 | 27.6 / 29.3 / 30.3 / 46.6 |

The first read of a process took 221 to 243 us. A read at open is cheap
next to Windows' 0.8 ms, but the table is still read at open only.

A child that set a table one entry off the identity and ended with
`_exit` or with SIGKILL: the parent read the identity again as soon as
`waitpid` returned (2 of 2). So a crash does not leave the table set on
macOS, unlike Windows' ramp.

`tests/adapt/screen_metal_test.c gamma` runs the set path on hardware:
a table one entry off (0.5 to 0.52 at entry 512 of 1024) before a
fullscreen open gives `os_gamma=set`, the identity read back while open,
and the table of before after close; a refused set (test seam
`YSCR__MAC_GAMMA_SET`) gives `refused` and `CODE_RISK_GAMMA`;
`keep_os_gamma` gives `not-identity(kept)`. Passed locked and unlocked.

Whether macOS applies the table on HDMI to the light was not measured. A
read-back says the window server kept the table, not that the panel
shows it. A capture needs Screen Recording, and a capture is before the
table in any case; the proof is a photodiode on a gray step.

#### The code risk state

What the layer used: SDL 3.4.0's `CAMetalLayer` read
`colorspace` = sRGB (`kCGColorSpaceSRGB`) at open, on a display whose
profile is "M27Q colorspace". ColorSync therefore converted every frame
from sRGB to the display's profile, in v0.11.0 to v0.15.0. A bare
`[CAMetalLayer layer]` reads nil, so the tag comes from SDL's view or
AppKit, not from the class. `CAMetalLayer.h` says: "If nil, no
colormatching occurs. If non-nil, the rendered content will be
colormatched to the colorspace of the context containing this layer
(typically the display's colorspace)." v0.16.0 sets it to nil at open
and reads it back for the risk (`layer_cs=untagged`). The display's own
colorspace would also mean no conversion, but it changes with the
profile the user picks; nil does not. Not verified with light or a
capture (no Screen Recording permission).

EDR. `NSScreen.maximumExtendedDynamicRangeColorComponentValue` (now) and
`maximumPotentialExtendedDynamicRangeColorComponentValue` (the display
can) read 1.00 and 1.00 on the M27Q; the layer's
`wantsExtendedDynamicRangeContent` is NO. Either value above 1 sets
`CODE_RISK_ADVANCED_COLOR`: an HDR mode or an XDR panel maps SDR values
into its range. No such display was attached; the test seam
`YSCR__MAC_COLOR` gives 16.0 and checks the flag.

Scaled. The current `CGDisplayMode` is 1920 x 1080 pixels; the modes
with the native flag (`kDisplayModeNativeFlag`, 0x02000000) are 2560 x
1440 (1x and HiDPI "looks like 1280 x 720", at 60, 120 and 144 Hz). So a
fullscreen screen on this display is `CODE_RISK_SCALED`: the panel
scales 1920 x 1080 up. The window's pixels are compared with the mode's
too: the compositor scales a window whose pixels differ. A window is not
checked, as on Windows. `STATE_UNKNOWN` is set only when the screen, the
layer, the mode or a native mode could not be read; on the M27Q every
check ran (window and fullscreen).

What the self test sees: the drawable as the header's blits wrote it,
read back on the GPU, so the pixel format (BGRA8, BGR10A2) and the
blits. It does not see what comes after the drawable: ColorSync, the
gamma table, scaling, EDR mapping, Night Shift and True Tone (no public
call reads them; `nightshift=unknown`), accessibility filters, and the
panel.

#### yscr_poll()'s time

`yscr_poll()` now gives a key, a modifier key or a mouse button the same
NSEvent time as `yscr_event_input()`. The pairing takes its entry, so
the pair is made once: the last 16 events are kept with their result
(keyed by SDL's stamp, type and key or button), and
`yscr_event_input()` on the same event finds it there. The input case
checks it: 160 of 160 events with `yscr_poll()`'s time equal to the
event's.

#### Tests and mutants

The core test checks the rules (key codes, modifiers, the 8-bit
identity, the risk, the value grouping, with 20,000 random values).
`tests/adapt/screen_metal_test.c` gained `state`, `gamma`, `rawmice`,
`panic` and `panic-post` (also selected by `YSCR_TEST_CASES`), and
passed 73 of 73 unlocked. The fullscreen space's settling ends
`FAILED(tier,...)` with `warn` there, as it did on v0.15.0 (checked on
the same machine the same day): not a change of v0.16.0. `mt-28` to `mt-45`: see the mutation list;
the results are in [status.md](status.md).

### Tests and mutants

- The core test (`tests/adapt/screen_test.c`): the rules without a Mac
  (target, period, completion, pixels, tier, in flight, margin, variable
  refresh) and, from
  v0.12.0, a scripted display that behaves as the timing session measured:
  the first vblank at or after the target, a commit deadline of a period
  plus 0.275 period, the handler 0.125 period after the vblank; 60 frames
  with targets a third of a period off the grid, work that ends late in a
  period on every 10th frame, and 3 hidden frames: every shown frame on
  its planned vblank, tier 2, nearly all one vblank apart.
- `tests/adapt/screen_metal_test.c`: the real presenter (above); built on
  macOS as `test_screen_metal`, not registered with ctest (it needs a
  window server session and ANGLE). A shown flip must be tier 2 and an
  estimated one tier 3; the last frame's read-back is checked at close.
- 13 mutants (`mt-01` to `mt-13`), 13 killed: 10 by the core test, 3 by
  the Metal test (no wait on the signal, codes compared raw, the pbuffer
  cache off). The far-target hold has no mutant: no test can show a frame
  far ahead without a shown screen.
- `mt-14` to `mt-27` (v0.13.0 to v0.15.0): variable refresh, the period's
  source, the macOS 14 floor, the first open's period and the NSEvent
  stamps, each in its section above.
- From v0.15.0 `tests/adapt/screen_metal_test.c`'s `input` case posts
  keys, the shift key and clicks and checks each stamp against an NSEvent
  monitor of its own (20 per kind; `YSCR_TEST_INPUT_N` for more). It
  needs the Accessibility permission and an unlocked session, and says so
  when it skips.
- From v0.14.1 `tests/adapt/screen_metal_test.c` also runs 120 frames in
  a fullscreen space and checks one frame a vblank there.

### Not done

A timing run of this header's own frame loop (only the functional test
above ran), the light (tier 1), direct against composed (one sign only:
the space's shorter queue), the panel's 10 bits, keys pressed by hand
(only posted keys were timed, v0.15.0), and a run that
switches only Screen Sharing to show that it causes the blank windows of
the probe's 6.3. From v0.16.0 the panic watchdog, raw mice, the OS gamma
ramp and the code risk state run on macOS too ([above](#the-panic-raw-mice-the-gamma-table-and-the-code-risk-v0160));
the raw mouse rate is not measured. Not exercised by a test: a presented handler given up 1 s
after its target (`late_handler=`; no way to withhold one was found), a
frame with no drawable (`no_drawable=`; every drawable came at once
here), a failed shared-event sync (the `glFinish` fallback), a window
resized while open (new IOSurfaces, the cache's eviction), and the
handler table full (64 slots across screens).

## Bindings

`ysp/screen.h` is bound in `ysp-vis` with `ysp/gfx.h` and `ysp/video.h`:
Python `ysp.screen` (one extension, `ysp._vis`) and the MEX `ysp_screen`
on the core `ysp_vis` ([bindings.md](bindings.md#display-stimuli-movies-ysp-vis)
has the packaging, the natives and the lifetime rule). Windows only, phase
A, 2026-10-27.

Bound: the display and mode lists, `yscr_mode_multiple()`, open with every
desc field a host sets (the option table of `bindings/share/ysp_vis_core.h`,
one for both hosts, times in seconds), begin (the frame with its completed
records), flip, flip at a time, wait_flip, marks, a ring of the last 4096
records (`records()`), the input events of `yscr_event_input()` in the
shared event layout (drained into a ring of 1024 at each begin, so a loop
that never polls does not fill SDL's queue), aborts (Python `Abort`, MEX
`ysp_screen:abort` or `[f, quit]`), the window's title, cursor and mouse
grab through `yscr_window_call()`, describe, caps, settle, sync, the patch,
`yscr_request_abort()`, the input bridge's counters. The window thread is
on (AUTO). `desc.angle_dir` is always the module's own directory.

Not bound, and why: triggers, `yscr_on_flip()`, `yscr_on_present()` and
`desc.panic_fn` run a callback on a C thread or inside the header's frame,
and no binding runs a host function there (rig_spec.md, section 12);
codes, text input, raw mice, groups, the native handles and
`desc.abort_keys`: not needed by a phase A frame loop. `desc.on_context`
is not a host callback either: since phase B the binding sets it itself,
to a C hook that starts the gfx while the screen settles (the open option
`gfx`; [gfx.md](gfx.md#bindings)).

Tests: 19 Python tests on the simulated display (CPython 3.12.11 and
3.14.3, fresh `--no-cache` builds), `test_window.py` with a real window
(both Pythons, local); 91 checks of `test_mex_vis.m` and 6 of
`test_mex_vis_window.m` in MATLAB R2023a and Octave 10.1.

### Measured on the binding: the minimal frame for 10 minutes

The laptop of [av_probe.md](av_probe.md) (i7-1360P, Iris Xe, internal
panel 1920 x 1200 at 60 Hz, Windows 11 26200), AUTO (COMPOSITION),
fullscreen, ANGLE 2.1.25848 from the wheel or the MEX directory, static
SDL3 3.4.0; every run through the timing guard; no photodiode, so every
tier is the record's. Each frame: begin, gfx begin, one rect, gfx end,
flip, and the frame's records kept as a script keeps its data
(`tests/probe/vis/`). Work is the DRAW phase: from the return of
`yscr_begin()` to the call of `yscr_flip()`, everything the host does in
the frame through the binding. 2026-10-27, 07:34 to 08:30.

| Host | Records | Missed (vblanks) | Late target | Occluded | Residual us p50 / p99 | Work ms p50 / p99 / max | Work > 8 ms | Path; tier |
|---|---|---|---|---|---|---|---|---|
| Python 3.12, AC | 36001 | 0 (0) | 1 | 0 | 3.9 / 116.4 | 0.25 / 0.57 / 4.78 | 0 | independent 36001; t1 35940, t2 61 |
| Python 3.12, battery, final build | 35997 | 2 (3) | 2 | 0 | 0.8 / 79.7 | 0.24 / 0.69 / 22.42 | 1 | independent 35997; t1 35986, t2 11 |
| MATLAB R2023a, AC | 36001 | 1 (1) | 0 | 0 | 2.0 / 113.0 | 0.49 / 1.01 / 6.25 | 0 | independent 36001; t1 35951, t2 50 |
| Octave 10.1, AC | 36002 | 0 (0) | 0 | 0 | 0.8 / 85.1 | 0.98 / 1.92 / 5.36 | 0 | independent 36002; t1 35988, t2 14 |

- **Against the probe** (av_probe.md, fullscreen, N = 1: work p50 / p99
  0.14 to 0.24 / 0.31 to 0.58 ms in Python 3.12, 0.24 / 0.57 ms in MATLAB,
  0.45 / 1.15 ms in Octave): Python's work is the probe's, MATLAB's and
  Octave's about twice it, all under 2 ms at p99 against a 16.7 ms frame.
  The difference is the frame the binding returns: a struct with its
  records (`done` and `last`), where the probe returned two numbers.
  Measured per call on the simulated display (battery, guard): a record
  costs 0.35 us more than a bare call in Python (0.42 against 0.07 us), 4.5
  us in MATLAB (6.1 against 1.7) and 45 us in Octave (49 against 4.4).
- **Misses.** MATLAB's one miss was frame 0 (work 6.3 ms: first-use costs
  while its planned vblank is the nearest, as in the probe); Python's late
  target on AC was frame 0 too. The battery run (the laptop was off AC
  for it) had 2 misses with work under 0.7 ms (the system's)
  and one frame of 22.4 ms host work at 105 s (`gc.freeze()` was on; the
  cause was not traced), flagged LATE_TARGET with no vblank dropped.
- **Residual p99 of 80 to 116 us** come from stretches flagged
  GRID_UNSTABLE (11 to 61 records a run), the OS's vblank jitter that the
  probes saw too; the p50 differed by the hour (0.8 to 3.9 us).
- The Python rows before and after the final build differ by one Record
  object per frame (frame.last shared with done's last) and the power
  source; the work did not change beyond the run-to-run spread.

### Measured on the binding: 30 s without a call

120 frames fullscreen, then 30 s with no call into the module (Python
`time.sleep`, MATLAB and Octave `pause`), then 120 frames; a watcher process
polled `IsHungAppWindow()` and the foreground window's class every 50 ms
(about 674 polls a run).

| Host | Hung | Ghost | Frame 120 (the first after the gap) | The 20 frames after it |
|---|---|---|---|---|
| Python 3.12 | never | never | on time, independent, tier 1, no flag | the same |
| Octave 10.1 | never | never | on time, independent, tier 1, no flag | the same |
| MATLAB R2023a | never | never | on time, independent, tier 1, no flag | the same |

With the window thread no host needs a pumping wait, and with v0.10.1's
anchor after a gap no record after it carried GRID_UNSTABLE (v0.10.0's
17 flagged records, [The grid after a gap](#the-grid-after-a-gap-v0101)).

### The frame in MATLAB and Octave (MEX 1.1.0)

Phase A's `'begin'` returned the frame as a struct with its records
(`done`) and the newest record again (`last`). A record is a struct of 17
fields, and each field is an array that MATLAB and Octave allocate. Since
1.1.0 `'begin'` returns the frame without records; the records come as
a third output when asked for (`[f, quit, done] = ...`) or from
`'records'` once per trial. A screen opened with `'frame', 'row'` gives
the frame as one row, `[onset period index vblank n_done done_lost]`, and
the records (the third output and `'records'`) as one n x 15 double
matrix. The exact ns are then not in the row or the matrix; seconds as
doubles hold every ns below 2^53 ns (104 days of a clock that counts from
boot).

Measured per call with the hidden command `'_frame'` (the last frame
converted by each form, no begin), a frame with one record, on the
simulated display: the i7-1360P laptop, Windows 11, AC, the measurement
lock held, 2026-10-27; best of 5 rounds of 20000 calls, the median of 3
runs. `'now'` is a bare call for the scale.

| us per call | `'now'` | phase A frame (`done`, `last`) | frame struct (1.1.0) | frame row | records struct | records matrix |
|---|---|---|---|---|---|---|
| MATLAB R2023a | 3.7 | 22.5 | 9.3 | 3.8 | 13.6 | 4.1 |
| Octave 10.1 | 3.6 | 191.0 | 55.0 | 13.5 | 104.3 | 13.1 |

So the default frame costs 2.4 (MATLAB) and 3.5 (Octave) times less than
phase A's with one record a frame, and the row costs about a bare call
in MATLAB and 10 us more in Octave. A first run the same day, under the
lock too, gave MATLAB numbers 3.3 to 5.8 times smaller (`'now'` 0.67 us,
phase A's frame 6.3, the struct 2.8, the row 0.65) and Octave numbers 1.1
to 2.7 times smaller (phase A's frame 120, the struct 22, the row 5.1);
the order of the forms held. The cause of the difference between the runs
was not found. Python's `Frame` is
unchanged: a record costs it 0.35 us (above).

## Linux X11: GLX_OML (v0.17.0)

The header is v0.17.0: v0.11.0 after the first merge, and v0.17.0 after the
merge over main's v0.11.0 to v0.16.1 (macOS). The numbers in this section
were measured as v0.8.0, the branch's number,
on the code before main's v0.8.0 to v0.10.4. After that merge (2026-11-04,
the same machine), `test_screen_glx` passed and the 32 `glx-` mutants gave
the expected results. The window thread is off on X11. No timing run was
repeated.

The design comes from the X11 probe ([screen_x11_probe.md](screen_x11_probe.md),
section 5.6). This section records what the backend itself does and
what was measured with it. The measurements ran on the backend's code
before screen.h's v0.5 to v0.7.2 changes (settling, the foreground, the
messages, the web backend) were merged under it. After the merge with
v0.7.2 (2026-10-13), `test_screen_glx` passed and the 31 `glx-` mutants
gave the expected results (30 killed; `glx-21` survives, as its entry
says), with SDL 3.4.18. With SDL before 3.4.18 the test ended on an X
error (BadWindow in X_TranslateCoords) in its first part, and the mutant
runner's baseline failed.

The machine: Debian 12, kernel 6.11.5-liquorix, Xorg 21.1 with the
amdgpu DDX, Mesa 22.3.6 (radeonsi) on an AMD RX 6600, one 1920 x 1080
output at 60.000000 Hz (dot clock), Xfwm4 with its compositor off,
SDL 3.4.18, gcc 12.2. The frame thread ran at normal priority
(`yrt_thread_elevate()` was not granted). No photodiode was attached.

### SDL makes the window, the header makes the context

As on Windows, SDL3 owns the window, and the header owns the swap path.
SDL's own GL context would work, but the header needs GL ES 3.0, a
GLXWindow and the OML entry points on a display connection that SDL's
event pump does not read. So the header opens a second Xlib connection
to the same display, makes the context there on a GLXWindow over SDL's
window, and opens a third connection, through xcb, that selects the
window's PresentCompleteNotify events. The X server sends a copy of each
event to every client that selects it, so Mesa's own queue is not
touched. The probe had verified this with its own window; the backend
does it on SDL's.

The window is SDL's fullscreen window, which the window manager makes
fullscreen (`_NET_WM_STATE_FULLSCREEN`). The probe measured only an
override-redirect window. With Xfwm4, every frame of the SDL window was
a Present flip.

### Measurements

`examples/screen/flipstats.c --backend glx`, one run at a time, nothing
else on the machine:

| Run | Frames | Path | Late target | Dropped | Early | Estimated | Prediction p50 / p99 / max, us |
|---|---|---|---|---|---|---|---|
| fullscreen, patch, idle | 3600 | flip 3600 | 0 | 0 | 0 | 0 | 0.15 / 2.15 / 17.15 |
| fullscreen, patch, 20 spinning threads | 3600 | flip 3600 | 0 | 0 | 0 | 0 | 0.15 / 1.16 / 16.15 |
| fullscreen, patch, `--miss 300` | 3600 | flip 3600 | 0 | 0 | 0 | 0 | 0.15 / 1.85 / 17.15 |
| fullscreen, `--hold` (0 to 4 vblanks ahead) | 600 | flip 600 | 0 | 0 | 0 | 0 | 600 of 600 on the vblank asked for |
| fullscreen, `--overrun 20` | 600 | flip 600 | 17 | 0 | 0 | 0 | 0.15 / 2.15 / 2.86 |
| 800 x 600 window | 600 | copy 600 | 0 | 0 | 0 | 0 | 0.15 / 2.86 / 3.15 |

- Every flip of the fullscreen runs is tier 1; every copy of the window
  run is tier 2.
- The 17 overruns of 20 ms were all flagged LATE_TARGET and charged to
  the draw phase; no other frame was late.
- `--miss 300` adds 1500 clears to 3 frames in 300. That was not enough
  to make this GPU miss a vblank, so no frame dropped, and the path that
  raises the depth after misses did not run.
- The header's own CPU time per frame (idle run): `yscr_begin()` without
  its wait 4.5 us p50, 14.8 us p99; `yscr_flip_at()` without the swap
  call 5.5 us p50, 17.0 us p99. The swap call itself
  (`glXSwapBuffersMscOML`) took 36 us p50.
- The present return to onset was 17.0 ms p50: one frame in flight, the
  record of a flip completes as its vblank begins, and the next frame is
  planned for the vblank after. flipstats' "on the first vblank after
  the return" counted 1 of 3600 idle: the event of the flip that just
  completed comes about 0.4 ms before that flip's UST, the next present
  returns within that time, so the UST of the flip already on the screen
  counts as the first vblank after the return. Under load the frame
  thread woke later, after that UST, in 21.9% of frames.

`tests/adapt/screen_glx_test.c` checks the same on a display in about
6 s: 120 frames on time, 120 held frames on the vblank asked for, every
record observed, independent and tier 1, the onset on the ysp_rt clock,
`_VARIABLE_REFRESH` 0, and a refusal under `vblank_mode=0`. ctest does
not run it. Its six mutants (`tests/mutate/screen.toml`, `glx-`) were all
killed.

### SDL before 3.4.18 ends the process on an X error

With SDL 3.4.0, CI's version, 7 of 14 short GLX runs ended at open with
an X error: `BadWindow` on `X_TranslateCoords`, for a window of another
client. A bare SDL program with no ysp code, a fullscreen window open for
0.6 s, ended the same way in 2 of 10 runs. SDL's ConfigureNotify handler
asks the server for the window's parent (`XQueryTree`) and then
translates coordinates against it (`XTranslateCoordinates`), with no X
error handler. When the window manager destroys its frame window between
the two calls, Xlib's default handler ends the process. SDL 3.4.18 puts
a handler around the calls (3.4.16 does not): 0 of 20 bare runs and 0 of
20 GLX runs failed. The describe line warns when SDL is older than
3.4.18. CI pins 3.4.18 since v0.17.0 (v0.11.0 on its branch).

### Mesa sets _VARIABLE_REFRESH at the first swap

The probe found `_VARIABLE_REFRESH = 1` on each fullscreen window. Mesa
sets it when its driconf option `adaptive_sync` is on, which is the
default. The first backend set the property to 0 after making the
context current, and the describe line read 1 at the end of the run:
Mesa sets it at the first swap, not at drawable creation. The backend
now sets it to 0 after open's test swap; it read 0 at the end of every
run since. On this monitor (`vrr_capable` 0) the property changes
nothing; on a variable-refresh monitor with the DDX's `VariableRefresh`
option on, it would let the refresh vary.

### X11 key times

SDL 3 stamps an X11 key event when it reads the event from the X
socket, inside `SDL_PumpEvents()`. A frame loop pumps once per frame,
in `yscr_begin()`, so a key's stamp is the next pump, not the key.
`examples/screen/input.c` on Linux injects keycode 192 through XTEST
(the X server's test extension, the analog of SendInput) at a random
point in each frame, in a 400 x 300 window with the focus, and compares
the stamp with the injection time:

| Stamp | Keys | Stamp minus injection, ms: min / mean / max |
|---|---|---|
| SDL's (`SDL_GetTicksNS()` at the pump) | 199 | 8.59 / 12.67 / 16.63 |
| The X event's server time, as ms of CLOCK_MONOTONIC | 199 | -1.97 / -1.40 / -0.97 |
| `yscr_poll()` from v0.17.0 (the server time) | 200 | -1.96 / -1.47 / -0.99 |

The server time sits 1 to 2 ms before the injection, always: Xorg's
millisecond clock lags by up to a tick (HZ=1000 here) and the
milliseconds truncate. If the server counted another clock, the second
row would be off by years. So the header takes SDL's X11 event hook,
queues each KeyPress and KeyRelease with its server time, and
`yscr_poll()` writes that time into the SDL key event it returns. A held
key's release and press, which SDL folds into one repeat, are skipped in
the queue. A time more than 1 s from now is not used.

XTEST keys do not come from a device, so they keep the server's time.
Keys from a keyboard get the kernel's time when the user can read the
keyboard's evdev node (next section). `test_screen_glx` checks 20 XTEST
keys; mutants `glx-06` and `glx-07`, killed.

### Kernel key times

The server's time is not the kernel's. `tests/probe/input_evdev/keys_probe.c`
makes a keyboard through uinput, sends 300 presses of F24 at random gaps
of 20 to 50 ms, and records for each press: the time before and after
the `write()` to uinput, the kernel's evdev time (asked for on
CLOCK_MONOTONIC), when a reader thread's `read()` returned it, and the
X server's time of the XInput 2 raw event. Same machine, idle, then with
20 spinning threads (one for each hardware thread) at normal priority:

| Interval, ms: min / mean / p99 / max | Idle | 20 spinning threads |
|---|---|---|
| Kernel's time minus the start of `write()` | 0.001 / 0.009 / 0.023 / 0.029 | 0.000 / 0.002 / 0.006 / 0.007 |
| evdev `read()` returned, minus the kernel's time | 0.006 / 0.021 / 0.038 / 0.042 | 0.005 / 0.021 / 0.039 / 0.058 |
| X server's time minus the kernel's time | -1.979 / -1.453 / -0.980 / -0.973 | -1.989 / -1.491 / -1.005 / -1.000 |
| Xlib returned the event, minus the kernel's time | 0.025 / 0.080 / 0.137 / 0.320 | 0.017 / 0.034 / 0.071 / 2.080 |

Xorg read each key within 0.3 ms (2.1 ms once under load), but it
stamps the key with its own millisecond clock, not with the kernel's time:
0.97 to 1.99 ms before the kernel's time, so that clock runs about 1 ms
behind CLOCK_MONOTONIC before the ms truncates (a coarse clock one tick
behind would do that, and this kernel's tick is 1 ms; Xorg's source was
not checked). A keyboard's key gets the same 1 to 2 ms early stamp as an
XTEST key, and the kernel's microsecond time is lost. With the reader on SCHED_FIFO 1 under the same
load the X column was -1.990 / -1.460 / -0.637 / -0.334.

From v0.17.0 the X11 key hook reads the keyboards' evdev nodes itself.
Every reader of a node gets every event with the kernel's time, buffered
until it reads, and the kernel queues an event for all readers before
Xorg can read it. So when SDL's X11 hook sees a KeyPress or KeyRelease,
the header reads its nodes (non-blocking; nothing to wait for) and takes
the newest kernel event of that key (keycode minus 8) and direction from
3 ms after the server's stamp to 100 ms before it. Older events of the
key are keys another window had and are dropped. No thread: the kernel's
time does not change with when the header reads it. With no match (an
XTEST key, a node the user cannot read, the server more than 100 ms
late) the key keeps the server's time.

The nodes are opened at the first GLX_OML open: every readable
`/dev/input/event*` with keys below `BTN_MISC` (mice have buttons only),
up to 16, read-only and not grabbed, so the desktop and other programs
keep their keys. inotify on `/dev/input` (IN_CREATE, and IN_ATTRIB
because udev sets the ACL after it makes the node) opens keyboards
plugged in later, checked on every X event: Xorg adds a new keyboard
about a second after its node appears, and SDL then gets the hierarchy
events. The describe line counts what came from where:
`key_time=kernel:n,x-server:n,unmatched:n,off-clock:n evdev_keyboards=n`.

Access. By default only root and the `input` group read evdev nodes.
Membership in `input` gives every program of that user every key, at all
times, from any session. The rule in `tools/linux/70-ysp-input.rules`
adds the `uaccess` tag to keyboards instead, so systemd-logind gives the
user at the seat (the one logged in at the rig's screen, not over SSH) an
ACL on keyboard nodes only, removed at logout:

```sh
sudo cp tools/linux/70-ysp-input.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
getfacl /dev/input/by-id/*-event-kbd   # user:<you>:rw- on each keyboard
```

Without it, nothing fails: keys keep the server's time, and the describe
line says `evdev_keyboards=0`. The rule's second line lets the same user
make uinput devices, which only `test_screen_glx` needs (with
`sudo modprobe uinput`).

`test_screen_glx` part 11: two uinput keyboards, one made before open and
one while the screen is open, send keycode 192 (KEY_F14) 10 times each.
Each press must come back from `yscr_poll()` with a time inside the
`write()` to uinput, which the server's time never is. 10 of 10 and 10 of
10, in 9 runs. Mutants `glx-18` to `glx-20` and `glx-22` to `glx-24`,
killed: the kernel time not used, the evdev clock not set to
CLOCK_MONOTONIC, no hotplug, a match window that ends at the server's
stamp, the evdev code taken as the X keycode, and no device accepted as a
keyboard. `glx-21` (no IN_ATTRIB)
survives on purpose: udev finished the node about 4 ms after the kernel
made it (`udevadm monitor`), and the hook reads inotify only on X events,
which for a new keyboard come about a second later. IN_ATTRIB matters only
when some other X event falls in those 4 ms, which a test cannot place.

A keyboard pressed by hand: a Dell wireless keyboard on its USB receiver
(`usbhid`, full speed), 300 presses, `keys_probe --keyboard
/dev/input/by-id/usb-Dell_Computer_Corp_Dell_Universal_Receiver-event-kbd
--hand --n 300`, same machine, idle:

| Interval, ms: min / mean / p50 / p99 / max | Dell receiver, by hand |
|---|---|
| evdev `read()` returned, minus the kernel's time | 0.007 / 0.023 / 0.022 / 0.042 / 0.046 |
| X server's time minus the kernel's time | -1.976 / -1.537 / -1.931 / -0.963 / -0.956 |
| Xlib returned the event, minus the kernel's time | 0.039 / 0.108 / 0.103 / 0.205 / 0.222 |

The same 1 to 2 ms early stamp as the uinput keyboard, so the kernel's
time corrects a real keyboard's keys too. The median sits near the -2 ms
end, unlike the uinput run's: most likely the receiver's 1 ms polling held
a phase against the kernel's tick (not checked).

Not measured: the delay from the key to the kernel's time, which is when
the USB host controller's interrupt delivered the report: the keyboard's
scan and debounce, the receiver's radio link and its polling interval. No
host stamp sees it; a photodiode, a microphone on the key or a solenoid
does.

### GPU-late frames and the depth

`examples/gfx/stress.c --kind noise` on this machine: full-screen layers
of gradient noise, 120 frames a level, doubled from 16, then bisected.

Before the merge under v0.4.4's rule ("Depth from the path"), the core
raised the depth after three misses in a row and lowered it only on
slack evidence. At 64 layers the GPU missed, the depth went to 2, and it
never came down: 16 layers, which had run at one frame per vblank, then
ran at half the rate for the rest of the run, with no frame counted as
dropped. Slack measures when the present call returns, not when the GPU
finishes, so at depth 2 the evidence for depth 1 never came.

With the depth from the path (v0.17.0 on GLX_OML):

| Layers | Dropped | Slow frames (2+ vblanks after the one before) | Depth |
|---|---|---|---|
| 16, 32 | 0 | 0 | 1 |
| 64 | 120 | 119 | 1 |
| 48 | 120 | 119 | 1 |
| 40 | 97 | 96 | 1 |
| 36, 38, 39 | 0 | 0 | 1 |
| 16 again, 600 frames | 0 | 0 | 1 |

A GPU-late frame is a drop and nothing more, the screen is back to one
frame per vblank as soon as the load is, and the knee is real: 39 layers
in 120 frames dropped nothing, 40 dropped 97.

A window is the copy path, which the depth table gave 2, from DWM's
composed path on Windows: a GLX window then ran 33.7 ms from present to
onset with no drop. X11's copy shows on the next vblank (the probe's
window runs), so GLX_OML's copy path is depth 1: 17.0 ms, 600 of 600 on
time. `test_screen_glx` checks the window's depth; mutant `glx-08`,
killed.

### A compositing manager

Xfwm4's compositor switched on (`use_compositing`, with
`unredirect_overlays` on, its default), 600 frames each, then switched
off again:

| Window | Present path | Tier | Dropped |
|---|---|---|---|
| SDL fullscreen (the window manager's fullscreen) | copy 600 | 3 on all 600 | 0 |
| 800 x 600 window | copy 600 | 3 on all 600 | 0 |
| SDL fullscreen as an override-redirect window (`SDL_X11_FORCE_OVERRIDE_REDIRECT=1`) | flip 600 | 1 on all 600 | 0 |

Xfwm4 kept SDL's fullscreen window redirected, although SDL sets
`_NET_WM_BYPASS_COMPOSITOR`: it unredirects override-redirect windows
only. A copy into a redirected window completes when the server copies,
not when the compositor shows the frame, so the backend gives every
frame tier 3 when a compositing manager owned the screen at open. The
records were right; the timing claim was not available.

An override-redirect window gets no keyboard focus from the window
manager: in `test_screen_glx` no injected key reached it. With an
explicit `XSetInputFocus()` on the window after open, all 20 keys
arrived with the X server's time.

On v0.17.0, settling (v0.5.0, SETTLE) is strict in fullscreen and needs
tier 1, so the default fullscreen window under Xfwm4's compositor does
not open: "the display did not settle in 3.0 s (fullscreen, strict):
tier ...". `desc.x11_override_redirect` (opt-in) asks SDL for an
override-redirect window for this screen only (SDL's hint is set around
the window's creation and put back) and gives it the keyboard focus. With
the compositor on: 300 of 300 frames Present flips, tier 1, settled in
0.61 s. The cost is the window manager's help: it neither stacks nor
focuses the window, so another program that takes the focus (a dialog, a
notification that asks for input) takes the keys, the abort keys too.
`test_screen_glx` part 7 checks the window and the 20 keys; mutants
`glx-09` (no focus) and `glx-10` (no hint), killed.

### X11: gamma, mode and panic

Measured on the Linux rig, both X screens, with `xrandr` and scratch
xcb programs:

- Both CRTCs' ramps (1024 entries) were exactly i * 65535 / (n - 1),
  which `xrandr --gamma 1:1:1` also sets: that is the identity.
- A ramp `xrandr` set stayed after `xrandr` exited (5369 counts from
  identity).
- A RandR mode an SDL program switched to (59.94 Hz) stayed after the
  program was killed with SIGKILL. On Windows the system puts a mode back
  when the process ends (PANIC); X does not.
- A mode set resets the CRTC's ramp to identity, but after it returns: a
  ramp set at once read back right, and was identity 50 ms later. Set 10
  ms after the mode set, it was lost; set 20, 34, 50 or 100 ms after, it
  stayed. SDL's fullscreen window in the desktop mode did not reset it.

So on GLX_OML (v0.17.0) the header does what OS GAMMA does on Windows,
through RandR on a short-lived connection of its own, and more: before a
`desc.mode` switch it keeps every active CRTC's mode and ramp; close,
`exit()` and the panic put the modes back, wait 150 ms, put the ramps
back and read them back 60 ms after each set. The panic watchdog reads
XInput 2's raw key events on X screen 0's root window on its own
connection, keeps its own modifier state, and counts a press while one of
the armed screens' windows has the input focus.

`test_screen_glx`:

| Part | What | Result |
|---|---|---|
| 8 | A child opens fullscreen with `desc.panic` and a 59.94 Hz mode over a dimmed ramp, then stops calling `begin()`; Shift+Esc x 3 through XTEST | exit 99, 0.34 to 0.95 s after the third press (5 runs); the dimmed ramp and 60 Hz back |
| 9 | A clean close over a dimmed ramp, in the desktop mode and after a 59.94 Hz switch | `os_gamma=set`, then identity after the switch; the dimmed ramp and the mode back after both closes |

The first version set each ramp right after its mode set and checked it
at once: it read back right and was then reset, so the user's ramp was
lost. The read-back now waits. Mutants `glx-11` to `glx-15` (the raw
keycode's offset, the wait and read-back, the snapshot's ramps, the ramp
to identity, the focus check), all killed. A test leaves the CRTC as it
found it, so that a failed run does not change the next one's start.

### Two X screens

The rig's two monitors as two X screens (ZaphodHeads, `:0.0` and `:0.1`,
one output each). The probe's two-screen phases (each screen's own
connection and thread, `oml_k1`, 600 swaps a phase): each screen alone,
both timed at once, and screen 0 timed beside screen 1 swapping as fast
as it goes (28,410 swaps a second) all gave 600 of 600 on target with no
late frame; beside the untimed screen, screen 0's grid residual rose to
8.9 us max (1.9 alone). Its cross-screen offset line (p50 0.0 us, min
-50,000 us, drift -99.6 ppm) is not explained.

SDL 3.4.18 lists the displays of both X screens, but puts every window on
$DISPLAY's screen: asked for the display on screen 1 from `:0.0`, the
window was on screen 0 (GLX and EGL). With `DISPLAY=:0.1` the window and
its context were on screen 1. So `desc.x_display` (":0.1") sets DISPLAY
before SDL's video starts, and one process drives one X screen; a second
screen asking for another X screen in that process is refused.

Not taken, measured: a window the header makes on screen 1, wrapped by
SDL on screen 0 (`SDL_PROP_WINDOW_CREATE_X11_WINDOW_NUMBER`). SDL
delivered its keys, 19 of 20 (the first key after the wrap was lost, 3
runs of 3), but reported the window on screen 0's display, and SDL's mode
switching would act on the wrong output. It is the way to an operator
window in the same process, if one is needed.

`examples/screen/flipstats.c --x-display :0.1` from a process on `:0.0`:
600 of 600 tier 1 on DisplayPort-5. Two processes, one per X screen,
1200 frames each at once, three times: tier 1 on every frame; one frame
dropped per run, on whichever process was running when the other opened.
Timed: the running screen dropped a frame when a GL program opened on the
other X screen (1 vblank) and when it closed (2 vblanks). Between those,
no drop. So start the operator program before the run.
`test_screen_glx` part 10; mutants `glx-16` and `glx-17`, killed.

### vblank_mode=0

Under `vblank_mode=0`, Mesa makes the swap interval 0, and Present then
flips without waiting for the vblank. The backend asks for interval 1 and
reads it back: with `vblank_mode=0` in the environment it read 0, and
open() refused with a message that names the variable. This answers the
X11 part of the open question in [rig_spec.md](rig_spec.md) section 13
for the environment variable. A driconf file setting was not tried.

### Raw mice on Linux

`desc.raw_mice` was Windows only (Raw Input). On Linux X11 the reader is
evdev, as the kernel key times are: one thread per process, started
SCHED_OTHER, reading every mouse node the user can read (the uaccess
rule's mouse line, `tools/linux/70-ysp-input.rules`) and new ones by
inotify. A report is everything the kernel sent up to a `SYN_REPORT`,
stamped with the kernel's time of it. The reports go over the input
bridge as on Windows (`yin_from_mouse()`), so `yscr_poll()` and
`yscr_event_input()` deliver them.

Three things differ from Raw Input:

- The device id. SDL 3's mouse ids on X11 are XInput 2 device ids (SDL
  3.4.18 listed ids 2, 4, 9, 10, 12 and 13 here, as `xinput list` does).
  The reader asks X for each device's "Device Node" property and gives a
  report the id of the device whose node it read, so a raw report and
  SDL's event name a mouse the same way; a node X does not list gets
  `0x45560000` plus its node number and `YSCR_MOUSE_UNLISTED`.
- The focus. Raw Input without RIDEV_INPUTSINK gives a process its mice
  only while it is in front; evdev gives everything. `begin()` passes the
  reader whether SDL's keyboard focus is in a window of this process, and
  the reader drops reports while it is not. A focus change takes effect at
  the next `begin()`.
- No guard: SDL's relative mode does not take evdev.

`test_screen_glx` part 12: a uinput mouse, made before open so X has
added it, sends 20 clicks with a move of (3, -2) while the window has the
focus: 20 presses, each with a time inside the `write()` (the kernel's),
SDL's id of that mouse (21) and the move, none flagged unlisted. With the
X focus moved to the root window and one frame later, 5 more clicks gave
0 events. Injected in the same frame as the focus change, before a
`begin()` had passed it on, 10 events got through: the gate lags the
focus by up to a frame.
A screen with no window is refused. Mutants `glx-25` to `glx-30` and
`rm-07`, killed: no focus gate, no id match, the read time, the default
clock, the left button as the right, the focus never passed.

Not measured: two physical mice by hand (`examples/screen/input.c
--mice`), the delay from a click to the kernel, tablets and touchpads
(absolute devices are not read).

### Compared with Psychtoolbox

On this machine, Psychtoolbox 3.0.19.14 under Octave beside
`screen_flipstats`, both checked against every vblank the kernel reported
for the output (`drmWaitVBlank` in a separate process).
`tests/compare/ptb_linux/README.md` has the method, the tables and the
commands.

- Flips: 9600, 4800 each, for the next vblank and held 1 to 5 frames,
  idle and with 20 spinning processes. Both landed every flip on the
  vblank asked for, and both report the kernel's vblank time: ysp's
  records exactly, PTB's within 2 us (its seconds are a double of
  CLOCK_REALTIME). On this driver the two tie on flip timing.
- The kernel's vblank time is the end of the vertical blank: PTB's Flip
  returned 0.15 to 0.41 ms before the time it reported (the blank is 45
  lines, 0.67 ms), so an onset stamp here is the start of the new frame's
  scanout.
- Open: PTB's OpenWindow 1.5 s with its default sync tests, 0.65 s
  without; a whole `flipstats` process 0.55 s, its settle 0.48 s.
- Keys, 200 uinput taps: PTB's KbQueue stamped them 0.03 to 0.40 ms after
  the kernel's time idle, with a tail to 2.0 ms under load; ysp gives the
  kernel's time ("Kernel key times").

## Not measured

The open findings of v0.10.0 (5 handles an open and close, SDL's
display ids) are fixed in v0.10.4
([Handles, no free buffer, display ids](#handles-no-free-buffer-display-ids-v0104)).

- The display-id match with two or more displays (one display here), and
  after a display is added, removed or moved between the list and the
  open.
- The window thread (v0.10.0) with two displays, a display that turns
  off while a screen is open, MATLAB's desktop (only `-batch` ran), a MEX
  file cleared with `clear mex` while a screen is open, a static SDL3
  beside another package's SDL3 in one process, a gamepad (none was
  attached), a custom presenter with ON, and Windows 10. The grid after a
  gap (v0.10.1) on a second display (DWM's timing is the primary's).
- Light. No photodiode was attached. `tests/loopback/screen_loopback.c`
  is built, not run. Every onset here is DXGI's vblank time.
- Any refresh other than 60 Hz; any GPU other than the Iris Xe; Windows 10.
- Wayland: a stub. macOS (METAL, v0.11.0): run only with the
  session locked; its constants set in v0.12.0 from the probe's timing
  session, not from a run of the header itself; see
  [macOS: the header owns the CAMetalLayer](#macos-the-header-owns-the-cametallayer-v0110-v0120).
  The web: no light measured (web.md).
- On X11: a compositing manager other than Xfwm4's, one window per
  output of one X screen, two X screens in one process, XWayland, NVIDIA's driver, a real
  keyboard's key times, and whether a ramp RandR reads back is the one
  the panel shows (no light or capture card).
- A DWM miss on the composed path of COMPOSITION: a second window loading
  the GPU did not cause one, so whether the record shows it is unknown.
- COMPOSITION on a machine whose QPC does not run at 10 MHz, on ARM64
  (refused), and on Windows 10 (not available).
- Variable refresh: refused (`desc.vrr`; on METAL also a display in a
  variable mode, v0.13.0, checked only through a test seam).
- Two physical displays. The group calls ran on two windows on one display.
- The keyboard itself: its scan, USB polling and the HID stack are not in
  the input times above.
- Any code read by a device (VPixx or Bits#), and codes on a display
  without auto color management.
- The OS gamma ramp set to identity on hardware (the ramp here was
  identity), and its effect on independent-flip and overlay frames.
- How late the after-flip hook runs, per backend (derived only, in the
  design note).
- Alt+F4 sent to a screen's window (not sent: it would close another
  program if the focus moved).
- The panic on COMPOSITION in fullscreen, on Windows 10, and with a real
  non-identity gamma ramp set.
- `desc.d3d11_video` with a decoder on the device (ysp/video.h measures
  it).
- The icons at a display scale above 100%.
- Early flips when a window leaves the composed path (open finding, from
  the ysp_video worker, 2026-10-06; cause found 2026-10-09, not fixed).
  ysp/video.h's runs, done again on v0.4.3 with the scanout as reference
  (docs/video.md, "Rerun: the early flips"): not the grid. At 1.7 to 1.9 s
  the system moves a foreground window from the composed path to an
  overlay plane; presents then show at depth 1, but
  `GetFrameStatisticsMedia` reports the composed path for 5 more flips,
  so no path change opens the one-flip window of DEPTH. v0.4.3's learner
  needs 3 flips in a row: 3 early flips in each of 4 runs. With the
  in-progress rule where the path sets the depth: 6 early flips (6 runs
  of 9), and in 3 runs of 9 open() learned depth 3 on the composed path,
  which nothing lowers before the path changes, so about 105 composed
  flips were early. Proposal: on DXGI_FLIP a flip not held that shows
  before its planned vblank proves a smaller depth (DXGI shows a present
  on the first vblank it can), so lower the depth on that one flip.
  The late flips of 2026-10-06 did not come again in 39 runs. Also seen:
  a window in the background got no statistic for 93% of its flips on
  DXGI_FLIP, and none on COMPOSITION, where it ran one frame each 4
  vblanks.
  v0.5.0: `yscr_open()` settles a window for 2.5 s; in 20 idle DXGI_FLIP
  window opens no early flip, path change or depth change came after open
  ([Settling at open](#settling-at-open-v050)). Under decode load, not
  measured again.

## Left out on purpose

- **RegisterHotKey for the panic** (v0.3.1): it takes the combination
  for the whole system and hides it from SDL. The low-level hook
  observes it instead (see above).
- **Escape held for a time as the abort**: slow for an operator under
  stress, and it needs state polled every frame. The user chose
  Shift+Esc.
- **The watchdog's own display-mode restore**: measured, it blocked for
  more than 1 s and the mode came back no sooner (see above).
- **Reinstalling the keyboard hook** and **DisableProcessWindowsGhosting**:
  measured, neither is needed on this machine (see above).

- **Partial presents** (EGL_KHR_swap_buffers_with_damage,
  EGL_KHR_partial_update, DXGI Present1 dirty rectangles). Decided
  2026-10-06. The Windows backends do not swap through EGL, so the EGL
  extensions do not apply there. Dirty rectangles reduce only the
  compositor's work on a composed path; a fullscreen independent flip or
  overlay frame gains nothing, and onset timing does not change. ysp_gfx
  redraws every pixel every frame (the output stage and its noise dither
  run on the full screen), so there is no small damage region to report.
  A partial update also needs the previous frame's pixels to survive, and
  the swapchains here are flip-discard. One wrong rectangle would leave
  stale stimulus pixels on the screen with no error and no record. A
  later Wayland backend reports the full frame as damaged, which equals a
  plain swap.
