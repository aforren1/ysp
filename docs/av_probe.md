# ysp-av host probe, 2026-10-26

This page is reference and explanation. It records a probe that asked
whether a frame loop written in Python, MATLAB or Octave can drive
`ysp/screen.h` and `ysp/gfx.h` at 60 Hz, and which platform traps a
binding of the rendering stack must handle. The binding is planned as
one module, ysp-av (Python `ysp.av`, MEX `ysp_av`): screen, gfx, video,
timeline, rt and audio compiled together, with SDL3 and ANGLE shipped in
the wheel and the MEX zip. The probe is not that API.

The probe is in [tests/probe/av/](../tests/probe/av/) (build and run
steps in its README). The raw logs and per-frame CSV files are in the
worker's scratch folder, not in the repo. The numbers below are the
record.

## Summary

1. **A host-language frame loop is viable at 60 Hz on this machine.**
   With 100 stimuli, each set and drawn by its own call (206 calls into
   the module per frame), the host's work per frame between `begin()` and
   `flip_at()` was, fullscreen, p99 0.83 ms in C, 1.4 to 2.5 ms in
   Python 3.12, 1.9 ms in Python 3.14, 2.8 ms in MATLAB R2023a and 5.1 ms
   in Octave 10.1, against a 16.7 ms frame. In about 1.4 million frames
   (38 runs of 10 minutes and 5 repeats) 6 frames had host work over
   8 ms, all fullscreen with N = 100: four in Octave (13.0, 48.0, 10.9
   and 19.3 ms) and two in Python 3.12 with gc disabled (29.6 and
   11.5 ms); each was missed or late. Every other missed frame came with
   host work under 8 ms: the system's, in clusters by minute, in C too.
2. **The cost is per call.** From the fullscreen N = 100 medians against
   C: about 1.1 us per call in Python 3.12, 3.7 us in MATLAB, 8.9 us in
   Octave. ysp-av should still offer calls that take many stimuli at
   once (a draw list, a bound group), so that a scene of hundreds of
   stimuli is not hundreds of interpreter calls.
3. **Python's garbage collector did not cause a miss** with this loop
   (one small tuple per frame). A full collection of the probe's heap
   (136,000 objects) took 1.9 ms in 3.12 and 8.9 ms in 3.14;
   `gc.freeze()` after setup took it to 0. Advise `gc.freeze()` after
   setup.
4. **MATLAB's ANGLE does not clash.** MATLAB R2023a does not load its
   ANGLE into `MATLAB.exe` (batch, desktop, `-nojvm`, with figures and a
   `uifigure`), and when another ANGLE is loaded first the bundled one
   still loads by full path and is the one used. The binding must always
   pass `desc.angle_dir`.
5. **Idle between trials is the main trap.** A window whose thread pumps
   no messages for about 4.1 to 4.6 s is marked hung, and 50 ms later
   Windows puts a ghost window in front of it. Python (`time.sleep`,
   `input`), Octave (`pause`) and C (`Sleep`) all did this. After the
   gap the first 2 to 16 frames went wrong in each such host (up to 3
   vblanks dropped, or composed and occluded records at tier 3 with
   `begin()` waits of 80 ms). MATLAB pumps its thread's messages in
   `pause` and at its prompt: no hang, no ghost, the first frame on
   time. A wait that pumps every 50 ms (`yscr_poll()`) fixed it in
   Python: no hang, first frame on time, tier 1.
6. **SDL3.dll binds by name.** When another package has already loaded
   an `SDL3.dll` from another directory, the module's import binds to
   that one, not to the bundled one. Any Python package that ships its
   own `SDL3.dll` can cause this; it needs a decision (section 5).
7. **Shift+Esc and the triple-press panic work from every host**: one
   press ends the loop with abort 0x1; 3 presses while the host waits
   end the process with exit code 99, in C, Python, MATLAB and Octave.
8. **Fullscreen fared better than a window on this shared machine.**
   Windowed runs had system misses in clusters (up to 594 in 10 minutes,
   in a C run) in every host; none followed a late host frame. In
   MATLAB's 10-minute windowed runs AUTO chose DXGI_FLIP ("no
   independent flip for the composition swapchain on this output"),
   four times of four; in its short windowed runs it chose COMPOSITION.

## Setup

| Item | Value |
|---|---|
| Machine | Laptop, Intel Core i7-1360P (12 cores, 16 threads), Windows 11 Enterprise 10.0.26200 |
| GPU and display | Intel Iris Xe (driver 32.0.101.7088), internal panel 1920 x 1200 at 60 Hz (measured 59.9997 to 60.0013 Hz), one display; the NVIDIA RTX A500 is not wired to the panel |
| Power | AC, Balanced power plan |
| Screen | `ysp/screen.h` 0.9.0, AUTO backend: COMPOSITION in every run except MATLAB's four 10-minute windowed runs (DXGI_FLIP); fullscreen is a borderless window over the panel, windowed is 1280 x 720 |
| gfx | `ysp/gfx.h` 0.17.1, mid-gray background, no calibration, program cache off |
| ANGLE | 2.1.23876 (git fffbc739779a), from Docker Desktop's `frontend` directory |
| SDL | 3.4.0 (VC development package) |
| Hosts | MSVC 19.51 C (`probe_c.exe`, /O2); CPython 3.12.11 and 3.14.3 (one abi3 `.pyd` built with 3.12); MATLAB R2023a (MSVC 2022 MEX); Octave 10.1.0 (its MinGW-w64 gcc 14.2 MEX) |
| Timing | Every timing run through `bash /c/tmp/psy-guard.sh time`: one at a time, CPU load at most 20 % when it starts. Other workers used the machine between runs and sometimes during them; see "Disturbances" |
| Photodiode | None. The tiers below are what the flip records report, not a measured tier. |

The scene, the same in every host: N stimuli on an 80 px grid around the
center, in turn a gabor, a grating in a circular aperture, a field of 100
dots and a rect, each 64 px. Stimulus 0's phase follows a timeline track
that ramps 0 to 1 over 1 s and repeats (a tween that never ends), bound
with `ygfx_apply()`.

Each frame, in every host:

1. `begin()` (`yscr_begin()`; the GIL is released while it waits);
2. the frame's completed flip records, converted to seconds and kept in a
   list that grows for the whole run, as a script keeps its data;
3. `evaluate()` (`ytl_evaluate()` and `ygfx_apply()`);
4. `gfx_begin()`; for each stimulus one parameter set (its orientation,
   through `ygfx_apply()`) and one `draw()`; `gfx_end()`;
5. `flip_at(onset)` with the predicted onset from step 1 (the GIL is
   released).

So N = 100 is 2 x 100 + 6 calls into the module per frame. The C driver
makes the same calls into the core, without step 2's conversion. Before
the loop each host makes a heap like an experiment's: 100,000 small
records (Python dicts, a MATLAB struct array). The core logs, per frame,
the times of the `begin()` call and return and of the `flip_at()` call and
return on the `ysp/rt.h` clock, and every flip record (through
`yscr_on_flip()`); it writes them as CSV at the end.

"Work" in the tables is the wall time from the return of `begin()` to the
call of `flip_at()`: everything the host does for the frame. "Outside" is
the time from the return of `flip_at()` to the next `begin()` call.

## 1. Missed frames with the interpreter in the loop

38 runs of 10 minutes (about 36,000 frames each), 2026-10-26 06:09 to
13:41, the same day for every host, plus 5 repeats after (below).
"Missed" counts records with `dropped > 0` (the vblanks in brackets);
"of them, host work > 8 ms" counts misses whose own frame had host work
over half a period; "late target" is `YSCR_FLIP_LATE_TARGET` (the caller
was late); "occluded" counts records flagged OCCLUDED (another window
covered ours). Residuals are |onset - target|. Path and tier are what
the records report.

#### Fullscreen

| N | Host | Frames | Missed (vblanks) | Of them, host work > 8 ms | Late target | Occluded | Residual us p50 / p99 | Work ms p50 / p99 / max | Work > 16.7 ms | Path, tier |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | C | 35997 | 4 (4) | 0 | 0 | 0 | 0.2 / 1.5 | 0.17 / 0.44 / 3.4 | 0 | independent 35997; t1 35997 |
| 1 | Python 3.12, gc on | 36002 | 0 (0) | 0 | 0 | 0 | 0.7 / 36.0 | 0.14 / 0.31 / 2.6 | 0 | independent 36002; t1 36002 |
| 1 | Python 3.12, gc off | 35997 | 5 (5) | 0 | 0 | 0 | 0.2 / 1.6 | 0.24 / 0.58 / 2.2 | 0 | independent 35997; t1 35997 |
| 1 | Python 3.12, gc.freeze() | 35992 | 6 (6) | 0 | 0 | 1 | 0.2 / 1.7 | 0.18 / 0.48 / 2.2 | 0 | independent 35992; t1 35991, t3 1 |
| 1 | MATLAB R2023a | 35986 | 4 (8) | 0 | 0 | 1 | 0.4 / 26.4 | 0.24 / 0.57 / 6.0 | 0 | independent 35986; t1 35979, t2 6, t3 1 |
| 1 | Octave 10.1 | 35989 | 13 (13) | 0 | 1 | 0 | 0.1 / 0.6 | 0.45 / 1.15 / 3.6 | 0 | independent 35989; t1 35989 |
| 10 | C | 35819 | 29 (38) | 0 | 1 | 37 | 0.3 / 1.8 | 0.18 / 0.54 / 6.6 | 0 | independent 35786, composed 33; t1 35782, t3 37 |
| 10 | Python 3.12, gc on | 36001 | 2 (2) | 0 | 0 | 0 | 0.7 / 3.8 | 0.19 / 0.53 / 3.2 | 0 | independent 36001; t1 36001 |
| 10 | Python 3.12, gc off | 36003 | 0 (0) | 0 | 0 | 0 | 0.7 / 1.8 | 0.18 / 0.36 / 2.2 | 0 | independent 36003; t1 36003 |
| 10 | Python 3.12, gc.freeze() | 36002 | 0 (0) | 0 | 0 | 0 | 0.6 / 1.8 | 0.18 / 0.35 / 2.8 | 0 | independent 36002; t1 36002 |
| 10 | MATLAB R2023a | 36002 | 1 (1) | 0 | 0 | 0 | 0.6 / 28.1 | 0.23 / 0.44 / 2.3 | 0 | independent 36002; t1 36002 |
| 10 | Octave 10.1 | 36002 | 0 (0) | 0 | 0 | 0 | 0.7 / 1.8 | 0.39 / 0.83 / 3.6 | 0 | independent 36002; t1 36002 |
| 100 | C | 36002 | 0 (0) | 0 | 0 | 0 | 0.7 / 1.7 | 0.37 / 0.83 / 4.7 | 0 | independent 36002; t1 36002 |
| 100 | Python 3.12, gc on | 36002 | 0 (0) | 0 | 0 | 0 | 0.7 / 1.7 | 0.59 / 1.37 / 3.3 | 0 | independent 36002; t1 36002 |
| 100 | Python 3.12, gc off | 35999 | 2 (2) | 2 | 1 | 0 | 0.7 / 5.4 | 0.59 / 1.57 / 29.6 | 1 | independent 35999; t1 35999 |
| 100 | Python 3.12, gc.freeze() | 36002 | 0 (0) | 0 | 1 | 0 | 1.0 / 74.5 | 1.00 / 2.51 / 3.9 | 0 | independent 36002; t1 35985, t2 17 |
| 100 | Python 3.14, gc on | 35445 | 39 (39) | 0 | 0 | 172 | 0.7 / 2.5 | 0.80 / 1.90 / 4.3 | 0 | independent 35445; t1 35273, t3 172 |
| 100 | MATLAB R2023a | 36002 | 0 (0) | 0 | 0 | 0 | 0.6 / 55.6 | 1.11 / 2.79 / 9.5 | 0 | independent 36002; t1 36002 |
| 100 | Octave 10.1 | 35502 | 26 (26) | 2 | 9 | 116 | 0.6 / 2.4 | 2.14 / 5.10 / 48.0 | 2 | independent 35404, overlay 59, composed 39; t1 35378, t2 1, t3 123 |

#### Window 1280 x 720

| N | Host | Frames | Missed (vblanks) | Of them, host work > 8 ms | Late target | Occluded | Residual us p50 / p99 | Work ms p50 / p99 / max | Work > 16.7 ms | Path, tier |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | C | 35996 | 6 (6) | 0 | 0 | 0 | 0.7 / 2.6 | 0.14 / 0.34 / 3.4 | 0 | independent 35993, overlay 3; t1 35993, t3 3 |
| 1 | Python 3.12, gc on | 35863 | 23 (26) | 0 | 0 | 38 | 0.7 / 2.6 | 0.17 / 0.41 / 2.1 | 0 | independent 35861, overlay 2; t1 35822, t2 1, t3 40 |
| 1 | Python 3.12, gc off | 35251 | 365 (528) | 0 | 4 | 29 | 0.2 / 16665.1 | 0.25 / 0.62 / 2.4 | 0 | independent 35168, overlay 83; t1 35133, t2 6, t3 112 |
| 1 | Python 3.12, gc.freeze() | 35612 | 9 (12) | 0 | 0 | 126 | 0.7 / 2.5 | 0.19 / 0.43 / 2.0 | 0 | independent 35609, overlay 3; t1 35484, t3 128 |
| 1 | MATLAB R2023a | 35530 | 518 (714) | 0 | 0 | 0 | 0.2 / 16666.0 | 0.39 / 0.85 / 7.9 | 0 | overlay 35179, composed 260, unknown 91; t1 35179, t2 260, t3 91 |
| 1 | Octave 10.1 | 35981 | 15 (20) | 0 | 0 | 0 | 0.3 / 2.4 | 0.41 / 1.40 / 7.9 | 0 | independent 35981; t1 34012, t3 1969 |
| 10 | C | 35980 | 14 (19) | 0 | 0 | 1 | 0.7 / 2.7 | 0.19 / 0.45 / 5.9 | 0 | independent 35972, overlay 8; t1 35971, t3 9 |
| 10 | Python 3.12, gc on | 35884 | 11 (12) | 0 | 1 | 35 | 0.6 / 2.4 | 0.25 / 0.66 / 2.0 | 0 | independent 35881, overlay 3; t1 35846, t3 38 |
| 10 | Python 3.12, gc off | 36000 | 3 (3) | 0 | 0 | 0 | 0.6 / 2.3 | 0.29 / 0.62 / 4.5 | 0 | independent 36000; t1 36000 |
| 10 | Python 3.12, gc.freeze() | 35344 | 25 (30) | 0 | 0 | 208 | 0.3 / 1.9 | 0.22 / 0.54 / 2.4 | 0 | independent 35341, overlay 3; t1 35133, t3 211 |
| 10 | MATLAB R2023a | 35999 | 1 (1) | 0 | 0 | 0 | 0.7 / 2.4 | 0.35 / 0.76 / 5.3 | 0 | overlay 35915, composed 82, unknown 2; t1 35915, t2 82, t3 2 |
| 10 | Octave 10.1 | 36002 | 0 (0) | 0 | 0 | 0 | 0.7 / 2.3 | 0.63 / 1.32 / 4.8 | 0 | independent 36002; t1 36002 |
| 100 | C | 36000 | 0 (0) | 0 | 2 | 0 | 0.6 / 2.4 | 0.83 / 1.56 / 8.1 | 0 | independent 36000; t1 36000 |
| 100 | Python 3.12, gc on | 35654 | 220 (284) | 0 | 0 | 9 | 0.6 / 2.8 | 0.75 / 2.65 / 5.1 | 0 | independent 35654; t1 35645, t3 9 |
| 100 | Python 3.12, gc off | 35548 | 328 (410) | 0 | 3 | 11 | 0.6 / 3.7 | 0.68 / 2.11 / 4.8 | 0 | independent 35548; t1 35537, t3 11 |
| 100 | Python 3.12, gc.freeze() | 35862 | 141 (141) | 0 | 0 | 0 | 0.7 / 2.6 | 0.70 / 1.88 / 3.3 | 0 | independent 35862; t1 35862 |
| 100 | Python 3.14, gc on | 36002 | 0 (0) | 0 | 0 | 0 | 0.7 / 2.4 | 0.67 / 1.80 / 3.3 | 0 | independent 36002; t1 36002 |
| 100 | MATLAB R2023a | 36000 | 0 (0) | 0 | 0 | 0 | 0.6 / 2.6 | 0.67 / 1.47 / 3.9 | 0 | overlay 35922, composed 77, unknown 1; t1 35922, t2 77, t3 1 |
| 100 | Octave 10.1 | 36002 | 1 (1) | 0 | 0 | 0 | 0.7 / 2.6 | 1.25 / 3.66 / 5.9 | 0 | independent 36002; t1 36002 |

Repeated the same day (13:41 to 14:36), back to back, to see whether the
windowed misses follow the host or the hour:

| N | Host | Frames | Missed (vblanks) | Of them, host work > 8 ms | Late target | Occluded | Residual us p50 / p99 | Work ms p50 / p99 / max | Work > 16.7 ms | Path, tier |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | MATLAB R2023a | 35989 | 9 (11) | 0 | 1 | 0 | 0.2 / 1.9 | 0.40 / 0.70 / 9.2 | 0 | overlay 35905, composed 82, unknown 2; t1 35905, t2 82, t3 2 |
| 100 | C | 34765 | 594 (764) | 0 | 4 | 121 | 0.3 / 16666.3 | 0.78 / 2.10 / 4.0 | 0 | independent 34679, overlay 85, composed 1; t1 4158, t3 30607 |
| 100 | Python 3.12, gc on | 17182 | 32 (40) | 0 | 0 | 6258 | 0.1 / 2.1 | 0.74 / 2.40 / 3.9 | 0 | independent 17176, overlay 6; t1 10906, t2 13, t3 6263 |
| 100 | Python 3.12, gc off | 17306 | 0 (0) | 0 | 0 | 6233 | 0.3 / 2.5 | 0.66 / 2.11 / 6.0 | 0 | independent 17306; t1 11073, t3 6233 |
| 100 | Python 3.12, gc.freeze() | 36000 | 2 (2) | 0 | 1 | 0 | 0.3 / 1.9 | 0.74 / 2.51 / 8.0 | 0 | independent 36000; t1 36000 |

The repeats moved the misses, not the hosts: C windowed N = 100 went
from 0 misses to 594 (with 30,607 records at tier 3), Python gc.freeze()
N = 100 from 141 to 2, Python gc off N = 100 from 328 to 0. In the gc on
and gc off repeats another window covered the stimulus window for
minutes (6,258 and 6,233 OCCLUDED records; `begin()` waited, so the runs
have about 17,000 frames). MATLAB's windowed repeat chose DXGI_FLIP
again, the fourth 10-minute MATLAB windowed run in a row to do so.

### Reading the tables

- **Host work fits the frame.** The worst p99 was Octave's 5.1 ms
  (fullscreen, N = 100). Six frames in all runs had host work over 8 ms,
  all fullscreen N = 100: Octave 13.0 and 10.9 ms (1 vblank dropped
  each), 48.0 ms (followed by a frame of 16.6 ms) and 19.3 ms (both
  flagged LATE_TARGET); Python 3.12 gc off 29.6 ms (LATE_TARGET and 1
  dropped) and 11.5 ms (1 dropped).
- **The other misses are not the host's.** No other miss had host work
  over 8 ms. The misses come in clusters of minutes (389 in minute 6 of
  MATLAB's N = 1 windowed run, 184 in minute 4 of Python's gc-off
  N = 100 windowed run), in C as well as in the interpreters (C
  fullscreen N = 10: 29, with 37 occluded records; the C windowed
  N = 100 repeat: 594). The same condition at another hour had none
  (Python 3.14 windowed N = 100: 0; the first C windowed N = 100: 0).
- **Late targets** (the caller late for the vblank it asked for) came
  from three causes: host work (the frames above), `begin()`
  returning late (waits of 15 to 90 ms, the system's, C too), and frame
  0. Frame 0 was flagged in 5 runs, with host work of 0.9 to 5.3 ms:
  the first frame after setup pays first-use costs (Python 2.5 to 5.3
  ms, MATLAB 4.6 ms, C 0.9 ms) while its planned vblank is the nearest
  one.
- **Garbage collection did not show.** Python 3.12 with gc on, off and
  frozen had no miss that a slow frame caused, except the one 29.6 ms
  frame with gc *off*. The loop allocates one small tuple per frame and
  a list that grows by it, which is cheap. A full collection is the risk
  when the heap is large. Measured with the probe's heap (100,000 dicts
  and 36,000 tuples; `gccost.py` in scratch, 7 repeats, through the
  guard):

  | Python | gen0 median | full median / max | full after `gc.freeze()` |
  |---|---|---|---|
  | 3.12.11 | 0.00 ms | 1.88 / 2.27 ms | 0.00 ms |
  | 3.14.3 | 0.00 ms | 8.91 / 11.21 ms | 0.00 ms |

  A full collection grows with the tracked objects: a heap 10 times
  larger costs a frame. In the 3.12 runs one full collection happened
  per run, at setup.
- **Residual p99 of 26 to 75 us** in some runs (Python 3.12 gc on N = 1,
  MATLAB fullscreen N = 1, 10, 100, Python gc.freeze() N = 100) came
  from stretches of seconds in which the OS's vblank times jittered by
  up to 50 us around the grid with no drop (for example the last 1,700
  frames of Python gc on N = 1). They do not follow the host; C's first-set runs
  had p99 1.5 to 2.7 us at other hours.
- **The time outside the two calls** (the host's loop from `flip_at()`
  to the next `begin()`) had p99 0.002 ms in C, 0.009 to 0.024 ms in
  Python, 0.04 to 0.18 ms in MATLAB and 0.08 to 0.23 ms in Octave.
- **Paths.** COMPOSITION on independent flip at tier 1 in nearly every
  frame, fullscreen and windowed, except MATLAB's windowed runs on
  DXGI_FLIP (overlay, tier 1, with 77 to 260 composed frames at tier 2
  per run) and the occluded stretches (tier 3).

### Disturbances

The machine was shared: other workers' editors, agents and builds ran
between the runs, and the guard checks the load only when a run starts.
Several runs show another window over the stimulus window (OCCLUDED
records with `begin()` waits of 65 to 300 ms: C fullscreen N = 10,
Python 3.14 fullscreen N = 100, Octave fullscreen N = 100). Two Python
runs did not start because another process held uv's cache lock; they
were run again with the interpreter called directly. None of this
changes the host comparison (work per frame); it makes the miss counts
an upper bound for this hardware.

## 2. MATLAB's ANGLE

MATLAB R2023a ships ANGLE 2.1.18846 (git 4a65a669e11b):
`bin/win64/libEGL.dll` (437,760 bytes), `libGLESv2.dll` (6,472,192) and
`d3dcompiler_47.dll` (4,891,080). The probe listed the loaded modules
(`K32EnumProcessModules`, full paths) before and after opening the
screen. Every case passed `desc.angle_dir`: the bundle's directory,
except the one that names MATLAB's. Each run drew 2 s of frames in a
window.

| Case | MATLAB's libEGL / libGLESv2 loaded before open | After open | Open | GL_VERSION reports |
|---|---|---|---|---|
| `matlab -batch`, a `figure` and a `uifigure` drawn first | no (only `opengl32.dll` and MATLAB's `mwopengl_startup_plugin.dll`) | the bundle's | yes, 120 frames | ANGLE 2.1.23876 (the bundle's) |
| desktop session (`matlab -r`), the same figures | no | the bundle's | yes | ANGLE 2.1.23876 |
| `matlab -nojvm -batch` | no | the bundle's | yes | ANGLE 2.1.23876 |
| MATLAB's copies loaded first by full path (`LoadLibraryW`), then open with the bundle's directory | yes | both copies, side by side | yes, 120 frames | ANGLE 2.1.23876 (the bundle's) |
| `angle_dir` = MATLAB's `bin/win64` | no | MATLAB's | yes, 113 frames, independent flip, tier 1 | ANGLE 2.1.18846 (MATLAB's) |
| Python, MATLAB's copies loaded first (as PyQt5 or Kivy would load theirs) | yes | both copies | yes, 120 frames | ANGLE 2.1.23876 |

`d3dcompiler_47.dll` came from the host program's directory or from
System32, never from the bundle: MATLAB's in MATLAB, System32's in Python
and Octave, the bundle's only for `probe_c.exe` (the bundle is its program
directory).

Verdict: no clash on this machine. MATLAB does not load its ANGLE into
`MATLAB.exe` in any of the cases tried, not even after a `uifigure` drew
(which process renders it was not checked). When another ANGLE is already
loaded, the bundle's still loads and is the one used, for two reasons that
the header and ANGLE give:

- `ysp/screen.h` loads `libEGL.dll` by full path with
  `LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)` when `desc.angle_dir` is set. A load
  by full path does not match a module of the same name from another
  directory.
- Both ANGLE builds' `libEGL.dll` import only `KERNEL32.dll` (`dumpbin
  /dependents`): libEGL loads `libGLESv2.dll` itself at run time, from
  its own directory. So the bundle's libEGL gets the bundle's libGLESv2,
  not the one already loaded.

The one path that can pick the wrong ANGLE: `desc.angle_dir` NULL and
`YSCR_ANGLE_DIR` unset. The header then calls `LoadLibraryW(L"libEGL.dll")`,
and Windows returns any `libEGL.dll` already loaded in the process. A
binding must always pass `angle_dir`.

MATLAB's ANGLE works with the header (the `angle_dir` case), so it is an
option, not a need. Its program compile was slower: the context hook
(ygfx's parallel compile start) took 0.595 s, against 0.012 to 0.038 s
with ANGLE 2.1.23876.

No header change is needed for the clash. If one is wanted later: load
`libGLESv2.dll` by full path from the same directory before `libEGL.dll`,
so that the pair is fixed even with an ANGLE whose libEGL imports
libGLESv2 by name (older ANGLE builds did).

## 3. Responsiveness between trials

After 120 frames each host stopped calling the module for 10 s or 30 s
with the window open, then ran 120 more frames. A thread in the core
polled every 50 ms: `IsHungAppWindow()`, the foreground window and its
class, `IsIconic()`, and whether the window still covered the monitor.

| Host and idle | Window marked hung | Ghost window in front | Foreground |
|---|---|---|---|
| C `Sleep()` | after 4.15 to 4.31 s | at the next poll, 50 ms later (window class "Ghost") | to the ghost; back to the window within 50 ms of the next `begin()` |
| Python `time.sleep()` | after 4.30 to 4.50 s | 50 ms later | the same |
| Python `input()` | after 4.25 to 4.56 s | 50 ms later | the same |
| MATLAB `pause()` | never | never | kept |
| MATLAB idle at its prompt (desktop, a timer ends it) | never | never | kept |
| Octave `pause()` | after 4.10 to 4.31 s | 50 ms later | the same as C |
| Python `time.sleep()` after `DisableProcessWindowsGhosting()` | after 4.30 s | never | kept |

No window was minimized, and a fullscreen window kept covering the
monitor in every case: the ghost sits over it, it does not replace the
mode. Times are from the start of the gap, which was the last `begin()`
and so the last message pump. N = 10, all runs through the guard,
2026-10-26 05:53 to 06:08 (pump runs later the same day).

MATLAB pumps the Windows messages of its main thread in `pause` and at
the prompt, and the MEX file's window belongs to that thread, so the
window stays responsive. Python's and Octave's waits pump nothing.

The first frames after the idle gap (frame 120 is the first after it):

| Host, idle 30 s | Fullscreen: frame 120 | Fullscreen: bad frames, time | Window: frame 120 | Window: bad frames, time |
|---|---|---|---|---|
| C `Sleep()` | 1 vblank dropped, overlay, tier 3 | 3, 200 ms | 3 vblanks dropped, overlay, tier 3 | 2, 83 ms |
| Python `time.sleep()` | composed, ESTIMATED + OCCLUDED, tier 3 | 3, 250 ms | 1 dropped, then 2 dropped | 16, 316 ms |
| Python `input()` | 1 dropped, overlay, tier 3 | 3, 200 ms | 3 dropped, overlay, tier 3 | 16, 316 ms |
| Octave `pause()` | composed, ESTIMATED + OCCLUDED, tier 3 | 3, 250 ms | 1 dropped, overlay, tier 3 | 3, 67 ms |
| MATLAB `pause()` | on time, independent, tier 1 | 0 | on time, tier 1 | 0 |
| MATLAB at its prompt | on time, tier 1 | 0 | 1 dropped, tier 1 | 1, 33 ms |
| Python, ghosting off | 1 dropped, independent, tier 1 | 1, 33 ms | not run | |
| Python, pump every 50 ms | on time, independent, tier 1 | 0 | on time, independent, tier 1 | 0 |

The 10 s gaps gave the same picture (logs in scratch: `resp/`). "Bad
frames" are the frames before the first 10 in a row with tier 1, no drop
and no flag; "time" spans their predicted onsets. In the bad fullscreen
frames `begin()` waited 80 to 84 ms each while the ghost was taken
away.

With a ghost the first 2 or 3 frames after the gap went wrong in every
host (16 in the two 30 s windowed Python runs, the later ones flagged
GRID_UNSTABLE at tier 2): fullscreen, records of the composed or overlay
path, flagged ESTIMATED and OCCLUDED (0xa) or ONSET_PLANNED (0x100), tier
3, and `begin()` waits of 80 ms; in a window, 1 to 3 vblanks dropped.
Without a ghost (MATLAB, and Python with ghosting off) the first frame was
on time or one vblank late, then tier 1.

The pump: `run_py.py --idle-mode pump` called the probe's `pump()`
(`yscr_poll()` until SDL's queue is empty) every 50 ms for the 30 s, as a
binding's `wait()` would. The watcher saw no hang and no ghost, and the
first frame after the gap was on time at tier 1, fullscreen and in a
window. `DisableProcessWindowsGhosting()` removed the ghost but not the
hung state, and the first frame still dropped one vblank.

Abort and panic, each host (fullscreen, N = 10; keys sent with
`SendInput` from a thread in the core, only while the probe's window or
its ghost was the foreground window):

| Host | Shift+Esc during frames | 3 presses 300 ms apart while the host waits 10 s |
|---|---|---|
| C | `begin()` returned QUIT, abort 0x1, 1 press | exit code 99 |
| Python 3.12 | the same | exit code 99 |
| MATLAB R2023a (`-batch`) | the same | exit code 99 (MATLAB reported "Exit Status: 0x00000063") |
| Octave 10.1 | the same | exit code 99 |

In MATLAB the window was not hung during `pause`, so the watchdog ended a
process that was responsive. That is the header's rule (no begin()
acknowledged the abort), not a fault, but a MATLAB user who pauses
between trials and presses Shift+Esc three times loses the session.

## 4. Octave

- Build: Octave's `mkoctfile` (MinGW-w64 gcc 14.2, ld 2.43) failed on
  SDL 3.4's MSVC import library: `ld.exe: internal error: aborting at
  ldlang.c:527 in compare_section`. Linking against `SDL3.dll` itself
  (`-L<bundle> -lSDL3`, no `.lib` in that directory) works. `mkoctfile`
  refuses a `.dll` path as an input file.
- The MinGW MEX file is 4,466,170 bytes, the MSVC one 635,904. Stripped
  with MinGW `strip` it is 1,000,960 bytes (`--strip-debug`: 1,159,928).
- `octave-cli` 10.1 does not load its `SDL2.dll` (not in the module list);
  SDL2 and SDL3 have different file names, so they cannot clash by name.
- An `onCleanup` object did not run at the end of `run_mex` in the first
  smoke test, and the window stayed open until Octave exited; `run_mex`
  now closes in a try/catch. `until` is a keyword in Octave, not a
  variable name.
- Octave's `pause` lets the window go hung (section 3).

## 5. Bundling

Each host loaded `SDL3.dll`, `libEGL.dll` and `libGLESv2.dll` from the
bundle directory, with nothing on `PATH` (none of the three is on this
machine's `PATH`; module lists in every log):

- Python: the `.pyd` imports `SDL3.dll`; CPython 3.8+ loads extensions
  with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`, so the `SDL3.dll` beside it is
  found without `os.add_dll_directory()`. The core passes the module's own
  directory (`GetModuleHandleExW` on one of its functions) as
  `desc.angle_dir`.
- MATLAB and Octave: the MEX file's directory is searched for `SDL3.dll`;
  `angle_dir` is found the same way.

The trap: a module that imports `SDL3.dll` takes any `SDL3.dll` already
loaded in the process, whatever its directory. Measured in Python: a
copy of `SDL3.dll` in another directory loaded first with
`ctypes.WinDLL(path)`, then `import ysp_av_probe`; the module list then
showed only the first copy, so the module's imports were bound to it.
Any package that ships its own `SDL3.dll` and loads it before ysp-av
would give ysp-av a different SDL3 build, maybe one older than 3.2.
The other order (ysp-av first) gives that package ysp-av's SDL3. The options:

- link SDL3 statically into the module (SDL's zlib license allows it;
  the MSVC and MinGW builds each need a static SDL3); there is then no
  `SDL3.dll` to clash, but two SDL instances in one process share one
  set of window messages and hints, which is not measured;
- ship SDL3 under another file name (an SDL3 built with another output
  name, and its import library) so the loader cannot match it;
- delay-load `SDL3.dll` (MSVC `/DELAYLOAD` with a hook that loads the
  bundled copy by full path first); MinGW has no delay-load hook, so the
  Octave MEX would need one of the other two.

ANGLE has no such trap (section 2), because the header loads it by full
path and ANGLE's libEGL loads libGLESv2 from its own directory.

What a bundle carries (this probe's files; sizes in bytes):

| File | Size | Needed |
|---|---|---|
| `SDL3.dll` (3.4.0) | 2,788,352 | yes |
| `libGLESv2.dll` (ANGLE 2.1.23876) | 8,428,400 | yes |
| `libEGL.dll` (ANGLE 2.1.23876) | 503,664 | yes |
| `d3dcompiler_47.dll` | 4,917,616 | no on Windows 10 and 11: ANGLE loaded System32's or the host's copy in every host |
| the module (`.pyd`, MSVC) | 632,832 | yes |
| the MEX file, MSVC (MATLAB) | 635,904 | yes |
| the MEX file, MinGW (Octave) | 4,466,170 | yes |

So a wheel is about 12.4 MB uncompressed, a MEX zip with both MEX files
about 16.8 MB (13.4 MB with the MinGW MEX stripped). miniaudio needs no
DLL of its own: with the default device it loaded `avrt.dll`,
`MMDevApi.dll` and `AUDIOSES.DLL` from System32; the null device loads
none.

### Audio beside the screen

ysp/audio.h 0.7.0 with miniaudio 0.11.25 is compiled into every probe
module. One run per host opened the default device beside the open
screen and played a 50 ms tone at -40 dBFS:

| Host | Device | Result |
|---|---|---|
| C | null device | open, `backend=Null`, 48000 Hz, 2 ch |
| Python 3.12 | default | open, `backend=WASAPI device='Speakers (Realtek(R) Audio)'`, 48000 Hz, shared, lead 44.26 ms |
| MATLAB R2023a | default | the same, lead 42.96 ms |
| Octave 10.1 | default | the same, lead 43.01 ms |

No load or init conflict: each opened, played and closed, and the
screen ran on. These were 3 s smoke runs, not through the guard, so their
frame records are not a measurement (Python's dropped 2 frames, C's and
MATLAB's none). No audio timing was measured.

Build note: with `ysp/audio.h`'s implementation before `ysp/screen.h`'s
in one file (the order ysp/video.h asks for), MSVC warns C4005
`CLSCTX_ALL` redefined: miniaudio defines it when `combaseapi.h` is not
yet included, and `d3d11_1.h` includes it later. The repo's own
`examples/audio/av_sync.c` gives the same warning with `cl /W4` (MSVC
19.51, Windows SDK 10.0.26100); the CMake build uses `/W4 /WX`, so a
Windows build of it with miniaudio found would fail (not checked in CI's
logs). Not a blocker for the probe. A header change that should fix it
(untested): include `<objbase.h>` on Windows before `miniaudio.h` in
ysp/audio.h's implementation, so that miniaudio's `#ifndef CLSCTX_ALL`
skips its own definition.

## What this means for ysp-av

1. **Release the GIL in `begin()`, `flip_at()` and every wait**, as the
   probe did; nothing else in a frame needs it released.
2. **Offer calls that cover many stimuli**: the per-call cost (1.1 us
   Python, 3.7 us MATLAB, 8.9 us Octave) is small at 100 stimuli and
   grows with the scene. A draw list or `draw_n` over a stimulus array,
   and timeline bindings that set parameters in C, keep the host out of
   the per-stimulus work.
3. **Return records cheaply**: the probe built one tuple per record per
   frame; a structseq or a preallocated array view would be as cheap.
   Keep times in seconds with exact ns beside them, as the other
   bindings do.
4. **Advise `gc.freeze()` after setup** in Python; do not change the
   interpreter's GC from the module. `gc.disable()` showed no gain here
   (its run had the one 29.6 ms frame).
5. **Pump while idle.** Give `wait(seconds)` (and an `idle()` to call
   from a host's own loop) that pumps the window with `yscr_poll()`
   every 50 ms or less, and document that `time.sleep()`, `input()` and
   Octave's `pause()` between trials hang the window after 4 s and cost
   the next 2 or more frames. MATLAB needs nothing: its `pause` and its
   prompt pump. Do not call `DisableProcessWindowsGhosting()` from the
   module: it changes the host process for its life and still leaves the
   first frame late.
6. **Always pass `desc.angle_dir`** (the module's own directory, found
   with `GetModuleHandleExW` on a function of the module). Never rely on
   `YSCR_ANGLE_DIR` or the search path: with neither set, the header's
   `LoadLibraryW(L"libEGL.dll")` returns any libEGL already in the
   process.
7. **Decide the SDL3 packaging** (static, renamed, or delay-loaded;
   section 5). Ship `SDL3.dll` (or not), `libEGL.dll` and
   `libGLESv2.dll`; `d3dcompiler_47.dll` is not needed on Windows 10
   and 11.
8. **MATLAB specifics**: the MEX's dependent DLLs are found in the MEX
   folder; `pause` keeps the window alive; the panic watchdog can end a
   MATLAB session that is paused, not hung, so tell MATLAB users that
   three Shift+Esc presses end MATLAB.
9. **Octave specifics**: build the MEX with Octave's MinGW against the
   DLL (not SDL's MSVC import library); strip it (4.5 MB to 1.0 MB);
   close in try/catch, not `onCleanup`; Octave's `pause` hangs the
   window, so it needs the module's `wait()`.
10. **Fullscreen for timing.** In a window on this machine the system
    dropped more frames, in every host. The records show it (OCCLUDED,
    tier 3), and the binding should surface those counts per trial.

## Not run

- No photodiode: every tier here is the record's, not a measurement.
- One machine, one display (the Intel Iris Xe laptop panel at 60 Hz),
  AC power only. No second display, no 120 Hz or higher, no NVIDIA
  output, no battery.
- No Linux or macOS: their screen backends are SIM or CUSTOM.
- Python 3.8 to 3.11 were not run (the abi3 `.pyd` built with 3.12 ran
  in 3.12 and 3.14). PsychoPy's own Python was not used.
- MATLAB releases other than R2023a, and MATLAB's MinGW add-on as the
  MEX compiler, were not tried.
- The Octave GUI (`octave --gui`, which loads Qt and may load SDL2) was
  not tried; only `octave-cli`.
- MATLAB idle at its prompt was tried in the desktop only through a
  timer (`prompt_idle.m`), not with a person at the keyboard. Octave at
  its prompt was not tried (Octave has no timer to end the idle).
- The panic was tested fullscreen only (the header arms it only there).
- Audio was opened, not timed.
- Video (`ysp/video.h`) was compiled in but no movie was played.
- Two SDL3 instances in one process (static SDL3 beside a package's
  SDL3.dll) were not tried.

## Open questions

1. SDL3 packaging: static, renamed or delay-loaded (section 5)? And
   does a static SDL3 work beside another SDL3 in the same process?
2. Should the module pump on its own between frames (a timer thread
   cannot: the window belongs to the frame thread), or only through
   `wait()`? A Python input hook (as matplotlib's for Tk) could pump
   while the interpreter waits at the `>>>` prompt.
3. MATLAB's 10-minute windowed runs got DXGI_FLIP from AUTO four times
   of four ("no independent flip for the composition swapchain on this output"),
   while short MATLAB runs and the other hosts got COMPOSITION. Is that
   the hour (machine state) or something of MATLAB's process (DPI
   awareness, its own windows)? A rerun with the presentation factory's
   answer logged would tell.
4. The six frames with host work over 8 ms (Octave 13.0, 48.0, 10.9 and
   19.3 ms; Python gc off 29.6 and 11.5 ms): no GC ran in the Python
   case. A trace
   (ETW, or Tracy zones in the module) on a rerun would name them.
5. Should the panic be gated on a hung window in hosts that pump while
   idle (MATLAB), so that three presses during a `pause` do not end a
   responsive session?
