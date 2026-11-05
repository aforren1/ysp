# Queue-ahead probe, 2026-10-27 and 2026-10-28

This page is reference and explanation. It records a probe of the open
question "A queue-ahead mode on COMPOSITION" in
[rig_spec.md](rig_spec.md) section 13: can a host draw K frames ahead,
present each one for its own vblank, and then sleep, on `ysp/screen.h`'s
COMPOSITION backend?

The prototype is in [tests/probe/queue/](../tests/probe/queue/) (build
and run steps in its README). It is a copy of `ysp/screen.h` 0.10.3 with
a queue mode, a DLL with the probe's calls, and a Python driver that
calls the DLL through `ctypes`, so the host that sleeps is a real Python
interpreter. `include/ysp/screen.h` is not changed on this branch.

## Summary

1. **Feasible within 31 buffers.** On independent flip, fullscreen and
   in a window, queues of K = 2, 4, 8, 16 and 28 frames, each frame
   drawn into its own buffer and presented for its own vblank, landed on
   the vblank asked for: 580 of 580 queued frames fullscreen and 580
   of 580 windowed, every one tier 1, against 300 of 300 for one held
   frame. Targets up to 600 vblanks (10 s) ahead landed, 100 of 100.
2. **Hard limit: 31 presentation buffers per manager, so a depth of
   28.** Microsoft documents the 31. Runs past it (62 to 122 buffers)
   stopped the laptop twice with bugcheck 0x133 (section 1). Within it,
   every run completed.
3. **Make the buffers at open.** Buffers added to a running manager put
   3 frames off tier 1, and a queue right after the add lost its first
   frame and the statistics of the rest. After 64 frames one in flight,
   every queue was clean.
4. **Cancel works, with two fixes.** An abort (Shift+Esc) cancels the
   queue 0.34 to 1.7 ms after the key, on the thread that saw it; the
   call takes 9 to 154 us (297 to 818 us with a black frame). The
   frame committed for the next vblank still shows (7.7 to 15.2 ms
   after the cancel), and nothing after it. A
   black frame follows on the vblank after (28.9 to 31.1 ms). When the
   abort came while the frame thread was still submitting, a present
   made just before the cancel escaped it in 3 of 40 runs (shown 225 to
   309 ms later); a second cancel on the frame thread, when `begin()`
   reports the abort, left 0 of 40.
5. **The panic's release cancels too.** With the cancel only at the
   release, it ran on the window thread 5 ms after the third press, and
   the release ended 9.8 ms after it. The window was minimized and not
   hung; `begin()` then reported 0x21.
6. **The host can sleep.** A 28-frame RSVP, then `time.sleep(1.5)` or
   `time.sleep(6)`: 112 of 112 frames on their vblank at tier 1, no
   window marked hung, no ghost, all records read in 0.6 to 1.5 ms after
   the sleep, and the first 120 frames after the queue on time.
7. **Costs.** 8.8 MB per buffer at 1920 x 1200 RGBA8, 17.6 MB in
   RGBA16F (measured: 31 buffers +276 MB and +568 MB of DXGI local
   memory). Drawing and submitting a queued frame took 228 to 487 us
   (p50) of CPU, so 28 frames took 7.2 ms (p50), 12.6 ms at most.
8. **Recommendation:** build it in `ysp/screen.h` as an opt-in on
   COMPOSITION, at most 28 frames deep, with the buffers made at open,
   the cancel at abort and at the release, and the fixes above
   (section 8).

## 1. Limits and history

Microsoft documents two limits of the composition swapchain
(`IPresentationManager`):

- At most 31 presentation buffers per presentation manager
  (`AddBufferFromResource`).
- A present statistics queue of 512 to 1024 entries. Here each present
  gave two (a status and an independent-flip statistic), so the queue
  holds 4 to 8 s at 60 Hz. The host must read statistics before the
  queue fills, or the oldest are lost.

It documents no limit on the number of presents in flight or on how far
ahead a target time can be.

The first runs of this probe did not respect the buffer limit.
`yscr_queue_reserve(s, 60)` registered 62 buffers with the screen's
manager, and the `cost` mode's allocation test registered 62 more with a
second manager. Twice the laptop stopped with bugcheck 0x133
(DPC_WATCHDOG_VIOLATION, arguments 0, 0x500, 0x500, the same address
both times):

| When | What ran | Buffers per manager |
|---|---|---|
| 2026-10-27, about 23:58 | `cost` (allocation test of 62 RGBA8 and 62 RGBA16F buffers, then reserves up to 62), then the start of `many` | 62 to 63 |
| 2026-10-28, about 05:04 | `many --ks 60,120`, seconds after the start | 62 to 122 |

Before the first bugcheck, runs with 62 and 63 buffers per manager
(queues of K = 60) completed several times. So a breach of the limit
does not fail at once; it can end in a bugcheck later. The minidumps
(`C:\Windows\Minidump\102726-20640-01.dmp`, `102826-23062-01.dmp`) need
an administrator to read, so the faulting driver is not named. This
laptop had one earlier bugcheck, 0x10E (VIDEO_MEMORY_MANAGEMENT_INTERNAL),
on 2026-09-24, before this probe.

From 2026-10-28 the prototype refuses more than 31 buffers per manager:
`yscr_queue_reserve(s, n)` makes n + 2 buffers and one black buffer, so
n is at most 28. The `cost` mode registers at most 31, and the `many`
mode (queues of 60 and 120) is removed. Every number on
this page from a queue deeper than 28 is from the runs of 2026-10-27,
and is marked so; the tables of section 4 are from the runs of
2026-10-28 within the limit.

## 2. Design of the prototype

The copy changes only the COMPOSITION presenter and the limits of the
core that assume one frame in flight. The host's frame loop does not
change: `begin()`, draw, `flip_at(t)`.

- **Depth.** `yscr_queue_reserve(s, n)` adds buffers until the screen
  has n + 2 (one shown, one drawn, n in flight) and one black buffer,
  and lets n presents be in flight. `acquire` (inside `begin()`) waits
  only while n presents have no statistic that settles them, then for a
  free buffer. With room in the queue, `begin()` returns at once and
  `f.onset` is the vblank after the last one planned, so K pairs of
  `begin()` and `flip_at()` put K frames on K vblanks in a row. Depth 1
  is 0.10.3's one frame in flight. The probe's `pq_queue()` raises the
  depth for the K frames it submits and puts it back to 1 after, so the
  frames that follow a queue wait as 0.10.3's do.
- **Records.** Each present keeps its slot in an id map of 256 (8 in
  0.10.3) and its target, for the OCCLUDED rule: the oldest present in
  flight that has no statistic 3 periods after its target completes as
  OCCLUDED. `YSCR__MAX_PEND` is 160 (8), and `yscr__drain()` reads
  every completion, not 4. `wait_flip()` waits until 8 periods and
  200 ms after the last planned vblank in flight, not after the call.
  The records are 0.10.3's `yscr_record`: no field was added.
- **Cancel.** `yscr_queue_cancel(s, blank)` calls
  `IPresentationManager::CancelPresentsFrom` with the id of the oldest
  present in flight. With `blank`, it then presents the black buffer
  with no target (the next vblank). It runs on any thread: a critical
  section guards the manager against the frame thread's `Present` and
  statistics reads. `yscr_queue_cancel_on(bits)` calls it from the
  abort log (bit 1: on the thread that saw the abort) and from the
  panic's release (bit 2: on the window thread); bit 4 adds the black
  buffer.
- **A frame drawn during a cancel.** The frame thread may be between
  `begin()` and `flip_at()` when another thread cancels. That frame's
  present is not made: `acquire` keeps the cancel count, and `present`
  compares it under the lock and completes the frame as CANCELED, after
  the presents before it have settled (section 4.3). With
  `PQ_RECANCEL=1` in the environment, the probe cancels again on the
  frame thread when `begin()` reports the abort.
- **Limits.** At most 31 buffers per manager (section 1), so the depth
  is at most 28.

## 3. Setup

- Laptop: Intel Core i7-1360P, 31.7 GB, Windows 11 Enterprise 26200, on
  AC. The panel (1920 x 1200, 60.0008 Hz measured, `\\.\DISPLAY1`) is
  driven by the Intel Iris Xe Graphics, driver 32.0.101.7088
  (2026-06-16). The NVIDIA RTX A500 (driver 32.0.15.9658) does not drive
  the panel and was not used.
- ANGLE 2.1.23876 (Docker Desktop's Electron build), SDL3 3.4.0 (DLL).
- Python 3.14.3 through `uv`, `ctypes`, MSVC 2026 (`/O2`).
- COMPOSITION, window thread on, foreground forced. Fullscreen: the
  panel's mode. Windowed: 1280 x 800, in front. Both reached independent
  flip (tier 1) after settling.
- Every run through `bash /c/tmp/psy-guard.sh time NAME ...`, one at a
  time on a quiet machine. No photodiode: the tiers are the records'.
- The frames: a gray clear and a white 100 x 100 square that moves with
  the frame index. The GPU work per frame is small.

## 4. Results

Every table is from 2026-10-28, within 31 buffers, unless it says so.
The raw logs and JSON files are in `C:\tmp\psy-work\probe-queue\`.
"On" means shown on the planned vblank: not SKIPPED, CANCELED, late or
EARLY. |res| is |onset - target|, where the target is the frame's
`f.onset`.

### 4.1 Several queued target times in a row (`land`)

10 queues per K, back to back, each frame for the next vblank; 64
frames one in flight after each reserve (`--prime 64`); between queues,
10 frames one in flight. The baseline is one frame held 0 to 4 vblanks
ahead with one frame in flight, as in docs/screen.md ("1800 of 1800").

| K | Fullscreen: on / frames | tier 1 | \|res\| p99, max (us) | Windowed: on / frames | tier 1 | \|res\| p99, max (us) |
|---|---|---|---|---|---|---|
| 1 held (0.10.3) | 300 / 300 | 300 | 6.5, 11.6 | 300 / 300 | 300 | 9.2, 10.2 |
| 2 | 20 / 20 | 20 | 1.7, 1.7 | 20 / 20 | 20 | 2.9, 2.9 |
| 4 | 40 / 40 | 40 | 1.9, 1.9 | 40 / 40 | 40 | 5.5, 5.5 |
| 8 | 80 / 80 | 80 | 11.3, 12.6 | 80 / 80 | 80 | 4.2, 4.2 |
| 16 | 160 / 160 | 160 | 20.1, 20.5 | 160 / 160 | 160 | 14.5, 15.6 |
| 28 | 280 / 280 | 280 | 23.8, 29.1 | 280 / 280 | 280 | 13.7, 19.7 |

Every queued frame was an independent flip with its own statistic, so
each frame has its own observed onset. |res| grows with the lookahead
because `f.onset` is extrapolated from an older vblank time (section 5),
not because a frame landed off its vblank. On 2026-10-27, past the
buffer limit, K = 30 and 60 gave the same picture: 270 of 270 and 540 of
540 on, tier 1, |res| up to 38.9 and 65.2 us.

**The first queue after a reserve.** Without the 64 frames after each
reserve (`land_fs_noprime`), the first queue after each reserve lost
its first frame (SKIPPED) and got no statistic for the rest (ESTIMATED):
for K = 2, 4, 8, 16 and 28, 0 of 2, 4, 8, 16 and 28 frames at tier 1.
The 9 queues after it were clean (522 of 522 on, tier 1). On 2026-10-27
the same first queues went to the overlay path (ONSET_PLANNED, tier 3)
or were SKIPPED for up to 8 frames. With one frame in flight after a
reserve, 3 of the 64 frames were off tier 1: fullscreen ESTIMATED and
OCCLUDED (no statistic), windowed overlay frames, one of them a vblank
late. The cause is the add of buffers to a manager that is presenting,
not the depth: the reserve of 2 did the same as the reserve of 28.

### 4.2 How far ahead (`far`)

Fullscreen, K = 4 frames whose first target was L vblanks ahead, 5
queues each.

| L (vblanks) | Time ahead | On / frames | tier 1 | \|res\| max (us) |
|---|---|---|---|---|
| 2 | 0.03 s | 20 / 20 | 20 | 6.1 |
| 10 | 0.17 s | 20 / 20 | 20 | 14.7 |
| 60 | 1 s | 20 / 20 | 20 | 69.8 |
| 300 | 5 s | 20 / 20 | 20 | 31.3 |
| 600 | 10 s | 20 / 20 | 20 | 70.4 |

The manager accepted every target and held the presents until their
vblank. The depth limit is the 31 buffers, not the time.

### 4.3 Cancel at an abort (`cancel`)

28 frames queued (467 ms), Shift+Esc by `SendInput` from a helper thread
300 ms after the start (the host sleeps), or 5 ms after the start (the
frame thread still submits). The abort log calls the cancel on the
thread that saw the key: SDL's raw keyboard thread in every run.
"Committed" is the last shown frame's onset minus the end of the cancel
call.

| Run | Runs | Key to cancel | Cancel call | Shown / canceled | Shown after the cancel | Committed | Black frame after the cancel |
|---|---|---|---|---|---|---|---|
| Fullscreen, host sleeps | 10 | 0.38 to 1.22 ms | 39 to 139 us | 19 / 9 | 1 (the committed one) | 12.7 to 15.2 ms | (no black) |
| Fullscreen, black frame | 5 | 0.49 to 1.01 ms | 345 to 667 us | 19 / 9 | 1 | 12.2 to 14.4 ms | 28.9 to 31.0 ms |
| Windowed, black frame | 5 | 0.52 to 1.13 ms | 297 to 818 us | 19 / 9 | 1 | 13.0 to 14.4 ms | 29.6 to 31.1 ms |
| Fullscreen, abort during submission | 20 | 0.35 to 1.66 ms | 39 to 154 us | 1 / rest | 1 in 19 runs; 2 in 1 run (225 ms after) | 8.1 to 9.8 ms | (no black) |
| Same, second cancel on the frame thread | 20 | 0.34 to 1.35 ms | 9 to 84 us | 1 / rest | 1 in 20 runs | 7.8 to 10.0 ms | (no black) |

- The present for the next vblank cannot be canceled: it shows. Every
  later present came back CANCELED (with the black frame, SKIPPED: the
  black present replaced them first), and none showed.
- **Abort during submission.** The abort reached `begin()` after 12 to
  28 frames; the frame being drawn at the cancel was not presented (1 to
  4 per 20 runs). In a first run of 20 (before the fix of the next
  point), 2 more runs showed a frame 242 and 309 ms after the cancel.
  So 3 of 40 runs without the second cancel, 0 of 40 with it (two runs
  of 20). A present made just before the cancel, and not yet in the
  manager's queue, can escape `CancelPresentsFrom`.
- **A probe bug that the runs found.** The record of a frame dropped at
  the cancel first completed before the queued presents before it. The
  core read that as "the older presents were skipped" and estimated
  them as shown (ESTIMATED) in 7 of 40 runs. The fixed copy holds that
  record until the presents before it settle; the two runs of 20 after
  the fix had no ESTIMATED record.

### 4.4 The panic's release (`panic`)

Fullscreen, panic armed with a grace of 1 ms (so a sleeping host looks
hung), 28 frames queued, 3 presses of Shift+Esc, the host sleeps 3 s.

| Cancel at | Presses at | First cancel | Release | Shown / canceled | Shown after the cancel | Window |
|---|---|---|---|---|---|---|
| each abort and the release (bits 3) | 300, 420, 540 ms | 1.0 ms after press 1 (SDL's raw keyboard thread) | 9.1 ms after press 3 | 19 / 9 | 0 after the release's cancel; 1 committed after the first | minimized, not hung, no ghost |
| the release only (bits 2) | 100, 170, 240 ms | at the release, 5.0 ms after press 3 (window thread) | 9.8 ms after press 3 | 15 / 13 | 1 (committed, 3.2 ms after) | minimized, not hung, no ghost |

In both runs the process lived and the next `begin()` returned
YSCR_QUIT with abort 0x21 (KEY and PANIC) and 3 presses. The window
thread's heartbeat never went more than 107 ms without a beat.

### 4.5 The host sleeps (`sleep`)

A 28-frame RSVP queued, then `time.sleep(S)` in Python, a watcher
polling `IsHungAppWindow` and the foreground every 50 ms, then
`wait_flip()`, then 120 frames one in flight.

| Run | S | Submit 28 frames | Queued frames on | Records during the sleep | Read after the sleep | Hung or ghost polls | Next 120 frames on | First `begin()` after |
|---|---|---|---|---|---|---|---|---|
| Fullscreen | 1.5 s | 5.7 ms | 28 / 28, tier 1 | 0 | 1.3 ms | 0 of 31 | 120 / 120 | 0.07 ms |
| Fullscreen | 6 s | 14.6 ms | 28 / 28, tier 1 | 0 | 0.6 ms | 0 of 119 | 120 / 120 | 0.02 ms |
| Windowed | 1.5 s | 9.5 ms | 28 / 28, tier 1 | 0 | 1.5 ms | 0 of 31 | 120 / 120 | 0.10 ms |
| Windowed | 6 s | 6.2 ms | 28 / 28, tier 1 | 0 | 1.5 ms | 0 of 119 | 120 / 120 | 0.03 ms |

The records complete only when the host reads the statistics, so they
arrive after the sleep (p50 1.3 s and 5.8 s after their onsets). The
manager kept every statistic across 6 s. On 2026-10-27 a 60-frame RSVP
(past the buffer limit) gave the same: 120 of 120 on, tier 1, no hang.

### 4.6 Costs (`cost`)

**Memory.** Displayable shared buffers at the screen's size, each
cleared once so the memory is committed, registered with a second
presentation manager; DXGI's local segment usage before and after (an
integrated GPU: shared system memory; budget 15,452 MB).

| Buffers | 1920 x 1200 RGBA8 | 1920 x 1200 RGBA16F | 1280 x 800 RGBA8 | 1280 x 800 RGBA16F |
|---|---|---|---|---|
| 8 | +73.5 MB | +128.1 MB | +32.5 MB | +16.2 MB |
| 16 | +147.0 MB | +293.0 MB | +44.7 MB | +130.0 MB |
| 31 | +275.7 MB | +567.7 MB | +101.6 MB | +251.9 MB |

That is 8.8 to 8.9 MB per RGBA8 buffer and 16 to 18 MB per RGBA16F
buffer at 1920 x 1200, as w x h x 4 and w x h x 8 predict. The windowed
rows at 8 and 16 buffers are below the prediction: the driver had not
committed all of them when it was read. The process's private bytes
did not change (the memory is the driver's). Making 31 buffers took 4.7
to 15.4 ms. A reserve of 28 (26 more buffers) took 1.1 to 2.8 ms.

**CPU.** Thread CPU time from `QueryThreadCycleTime`, per frame from
`begin()` to the return of `flip_at()`; the frame draws two clears and a
square. 5 queues per K after 64 frames one in flight.

| Run | One in flight: CPU p50, p99 | Queued, K = 2, 8, 16, 28: CPU p50 | Wait in `begin()` p99 (queued) | Submit all K: p50, max |
|---|---|---|---|---|
| Fullscreen | 808, 1471 us | 424, 263, 253, 239 us | 27 to 74 us | 1.0, 2.4, 4.2, 7.3 ms; max 12.6 ms |
| Windowed | 820, 1500 us | 487, 239, 308, 228 us | 25 to 67 us | 1.3, 2.7, 6.4, 7.2 ms; max 9.7 ms |

A queued frame costs less CPU than a frame one in flight, because
`begin()` does not wait for the previous frame's statistic. The frames
after a queue drains are one in flight again: in 4.5, 480 of 480 were on
time and the first `begin()` returned in 0.02 to 0.10 ms.

## 5. Mapping records and timeline events to frames drawn ahead

- **Frame index and onset.** Each queued frame is an ordinary frame:
  `begin()` gives it the next index and `f.onset`, the grid's time of
  the vblank after the last one planned. The host evaluates its
  timeline at `f.onset` and draws, as in a frame loop. For K frames
  back to back, `f.onset` of frame k is t0 + k periods on the grid.
  `flip_at(f.onset)` (or a time that snaps to it) keeps that plan.
- **The prediction error grows with the lookahead.** The grid is
  anchored on the newest OS vblank time, which is up to K frames old for
  the last frame of a queue. The OS's vblank times wander by tens of
  microseconds over a second (docs/screen.md, "The grid after a gap").
  So the last frames of a queue had |residual| (onset minus target, and
  target is `f.onset`) of 15 to 29 us at K = 28, and up to 65 us at
  K = 60 (2026-10-27), against 10 to 14 us for one held frame. That is
  far from a vblank's width: every frame landed on the vblank it was
  planned for.
- **Records arrive late.** A record completes when the host next reads
  the statistics: in `f.done` of the next `begin()`, in `wait_flip()`,
  or in `records()` (ysp-vis). After an RSVP and a sleep, all K records
  come at once. `f.done` holds 16 (YSCR_MAX_DONE): a queue deeper than
  16 must read `records()` or the ring, or `f.done_lost` counts the
  rest. The presentation manager kept every statistic across a 6 s
  sleep; its queue holds 512 to 1024 entries, so a host that presents
  continuously must read within about 4 s.
- **Timeline events.** An event whose time snaps to frame k's vblank is
  reported by `ytl_evaluate()` for frame k when the host evaluates it,
  K frames before it shows. The event's measured onset is frame k's
  record onset, joined by index (`ytl_event.frame`, the ring's
  `u.u32[9]`). Triggers armed at a flip (`yscr_trigger`) fire from the
  record as before.
- **What input can and cannot change.** Input that arrives while frames
  are queued cannot change a queued frame. It can only cancel the queue
  (an abort, or the host's own cancel) or change the frames drawn after
  the queue drains. A response is stamped on the ysp/rt.h clock as
  before; its latency from a stimulus uses the stimulus frame's record
  onset, which arrives after the sleep. A design that must react within
  a stream (a gaze-contingent change, a step within the stream) needs
  one frame in flight for that part.

## 6. The API it suggests

**ysp/screen.h.**

- `desc.queue_max` (0 or 1: one frame in flight, as now; at most 28 on
  COMPOSITION; refused on the other backends). `open()` makes
  `queue_max + 2` buffers and the black buffer before settling, so the
  settle frames present every buffer once. A reserve after open put 3
  frames off tier 1, and the first queue right after a reserve lost its
  first frame and the statistics of the rest (section 4.1).
- `yscr_queue(s, n)`: the depth for the frames that follow (1 to
  `queue_max`). `begin()` waits only while n presents are in flight.
  `n = 1` returns to one frame in flight; the next `begin()` then waits
  until every queued present is settled.
- `f.ahead` in `yscr_frame`: the presents in flight when `begin()`
  returned, so a host knows how far ahead it draws.
- `yscr_queue_cancel(s)`: cancels every present not yet shown, from any
  thread, and drops the present of a frame being drawn. The header calls
  it at each abort and at the panic's release (on by default when
  `queue_max > 1`), on the thread that saw it, and again on the frame
  thread when `begin()` reports the abort (section 4.3).
  `desc.queue_blank` shows the black buffer at the vblank after the
  frame already committed.
- `YSCR_MAX_DONE` stays 16; the manual tells a host to read the ring or
  `records()` after a queue deeper than 16.

**ysp-vis.** The frame loop does not change. `Screen(queue=28)` sets
`queue_max`; `s.queue(n)` sets the depth; `s.cancel()`; `Frame.ahead`.
An RSVP is then:

```python
s.queue(len(items))
for item in items:
    f = s.begin()
    tl.evaluate(f.onset, f.period)
    g.frame(scene)
    s.flip()
s.queue(1)
time.sleep(len(items) * f.period + 0.5)   # the host may do anything here
s.wait_flip()
records = s.records()
```

The calls per frame stay (about 1.1 us each in Python, rig_spec
section 12), small against the draw. A call that draws K frames from
one list would save K - 1 interpreter round trips; no measurement here
asks for it.

## 7. Risks

- **Driver stability.** Two bugchecks 0x133 on this laptop when a
  manager held 62 to 122 buffers (section 1). Within 31 buffers, every
  run of 2026-10-28 completed. The header must refuse more than 31
  buffers per manager. The depth of 28 is a hard limit, not a default.
- **Memory.** Each buffer is one back buffer: 8.8 MB at 1920 x 1200
  RGBA8 (17.6 MB in RGBA16F). 31 buffers are 272 MB in RGBA8 against a
  DXGI budget of 15.1 GB here (shared memory). On a discrete GPU with
  2 to 4 GB the same depth is a larger share, and video textures
  compete for it.
- **A reserve during a run.** Adding buffers to a running manager put 3
  frames off tier 1 and spoiled the first queue after it. Buffers must
  be made at open.
- **Thread safety of the cancel.** The cancel calls the manager from
  another thread while the frame thread may draw with ANGLE on the same
  D3D11 device, which has no multithread protection unless
  `d3d11_video` is set. No error came in the runs, which proves nothing
  about a race. A header version must hold the manager's lock across
  the frame thread's whole present, as the probe does, and leave every
  D3D11 context call to the frame thread.
- **A present just before the cancel.** When the abort came during
  submission, a frame presented just before the cancel was sometimes
  still shown later (section 4.3).
- **Input.** A queued frame cannot react to input. Designs that react
  within a stream must not queue that part.
- **Tiers.** Only independent flip was measured. A composed or overlay
  path gives tier 3 onsets with or without a queue.

## 8. Verdict

**Feasible, and worth building, as an opt-in with a hard depth of 28
on COMPOSITION.**

- It does what section 13 asked: K frames drawn ahead land on their
  vblanks as exactly as one held frame, at tier 1, fullscreen and in a
  window; the host can sleep through them; the records come afterwards;
  the window does not hang.
- It removes the host from the timing of a fixed sequence: the
  interpreter pauses of docs/av_probe.md (10 to 48 ms) cannot drop a
  frame that is already queued.
- Build it with: the buffers made at open (`desc.queue_max`, at most
  28: 31 buffers per manager with the black buffer); a refusal of more;
  the cancel at each abort and at the panic's release; a second cancel
  on the frame thread when `begin()` reports the abort; the drop of a
  present that a cancel overtakes; that frame's record after the
  presents before it; the manager's lock around every manager call.
- Keep one frame in flight as the default. A queue gives up input
  during the stream and adds 8.8 MB per frame of depth at 1920 x 1200.
- Not on DXGI_FLIP: it has no present time.

## Not run

- No photodiode: every onset is the OS's (tier 1 independent flip). A
  queued frame's light was not measured.
- One machine (Iris Xe, 60 Hz). No other GPU, no discrete GPU, no other
  refresh rate, no Windows 10 (COMPOSITION needs 11), no second display.
- Composed and overlay paths with a queue (a window that is covered or
  not in front): not run.
- Heavy GPU work per frame (`--load`): not run. A frame whose GPU work
  is late in a queue was not measured; with targets, the manager may
  skip it rather than shift the frames after it.
- Long runs: the longest queue run was about 1 minute. No 10-minute run.
- MATLAB, Octave and the ysp-vis binding: the driver is Python through
  `ctypes` only.
- The minidumps of the two bugchecks were not analyzed (they need an
  administrator).
- Depths above 28 within the rules: none exist. Several managers per
  screen (to pass 31 buffers) were not tried and are not proposed.

## Open questions

- Which driver caused the bugchecks 0x133? An elevated `!analyze -v` of
  the two minidumps would name it. Is the cause the buffer count, the
  memory, or the number of presents in flight?
- Does a late frame inside a queue (GPU work over a period) skip, or
  shift the frames after it? The `--load` option exists for this run.
- Does the queue hold on a composed or overlay path, where the depth is
  2 and the onsets are tier 3?
- Should the black frame after a cancel be the default? It shows on the
  second vblank after the abort; without it, the committed frame stays
  until the program draws.
