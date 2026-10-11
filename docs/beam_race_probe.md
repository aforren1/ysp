# Beam-racing probe (Windows), 2026-10-09

This note describes a probe that measures whether a Windows machine can
"race the beam": present several frames per refresh, each one timed so
that its tear line lands on a chosen scanline. Its results decide the
form of an opt-in beam-racing present mode in `ysp/screen.h`. The idea is
the one of Blur Busters' Tearline Jedi demo and WinUAE's lagless vsync;
the probe takes no code from them.

Code: `tests/probe/beam_race/`

- `beam_race_probe.c`: the probe, one C file. D3D11 and DXGI directly
  (no ANGLE, no SDL); `ysp/rt.h` for the clock, the waits and thread
  elevation.
- `CMakeLists.txt`: a standalone project. The main `CMakeLists.txt` does
  not build the probe.
- `run.sh`: one command that builds the probe and runs every section into
  a dated folder.

The probe is Windows only. It builds without warnings with MSVC 19.44
(`/W4 /WX`, C11) and MinGW-w64 gcc 16.1 (`-Wall -Wextra -Werror`). WSL gcc
cannot build it: there is no `windows.h`, and the file stops with
`#error` on other systems.

## 1. Why

A motor task with real-time feedback (a cursor, a hand position, a
target that follows a tracker) wants the shortest time from the input
sample to light. With vsync, a frame shows at a vblank and the scanout
then takes most of a refresh to reach the bottom row. The late frame
start of [gfx.md](gfx.md) (Next, item 12) moves the input sample close to
the vblank, but a row near the bottom still shows about 16 ms after it.

Beam racing presents N slices per refresh, each with tearing. A slice's
tear line lands just above the rows that the raster scans next, so the
rows below it show an input sample that is at most one slice old. It also
changes what an onset is: a row's onset is the time the raster reaches
that row after the tear, not the vblank. So a beam-racing mode needs
records per slice and an onset that depends on the row.

## 2. How to run it

WARNING: the probe shows full-screen bands and, with `--patch`, patches
that flicker at 30 Hz. Do not run it where a person with photosensitive
epilepsy can see the screen. `--no-flicker` uses low-contrast grays only.
The probe prints the warning and waits 5 s (`--yes` skips the wait).
Esc stops a run.

From the repo root, in Git Bash, on AC power, with nothing else running:

```sh
sh tests/probe/beam_race/run.sh [parent folder] --no-flicker
```

The script builds into `build/beam_race/` (set `BUILD_DIR` to change it)
and writes `ysp_beam_race_<date>_<time>/` in the parent folder (default:
the current folder). A full run takes about 3 minutes and uses the whole
screen. Options after the folder go to the probe:

| Option | Effect |
|---|---|
| `--only LIST` | Sections, comma-separated: `path`, `recipes`, `raster`, `psr`, `tear`, `cost` (default all; the latency comes from `cost`) |
| `--slices LIST` | Slices per refresh for `tear` (default `2,4,10`) |
| `--seconds S` | Seconds per tear or cost pass (default 5) |
| `--drift S` | Seconds of raster sampling for the drift (default 60) |
| `--static-check` | Each slice draws one fixed picture in its own rows (plus a guard band) and a wrong color elsewhere. A tear line where planned is invisible; a miss shows as a stripe of the wrong color |
| `--guard ROWS` | Rows of the picture beyond each slice edge in `--static-check` (default 8). The guard wraps between refreshes: the last slice also draws rows 0 to ROWS, slice 0 also draws the last ROWS rows |
| `--seam-lines N` | Slice 0 aims N lines before the first active line, inside the vertical blanking (default 0: at line 0). See 3.6 |
| `--sweep-lead A:B:STEP` | Tear section only: one pass per lead from A to B us in steps of STEP, `--seconds` each, with the lead shown large on the screen. No calibration pass. See 3.6 |
| `--ruler` | Ticks at the right edge every 8 rows, from 64 rows above to 128 rows below each slice's target row. Long tick at the target row, medium tick every 32 rows |
| `--patch` | Three 48 x 48 photodiode patches at the left edge, at 1/10, 1/2 and 9/10 of the height, drawn by the slice that covers them, light on even refreshes (30 Hz). Their planned times go to `patch_*.csv` |
| `--draw-lead US` | Wake this long before a present to read input and draw (default 1000) |
| `--lead US` | Present this long before the raster reaches the target line; skips the calibration pass |
| `--cost-slices N` | Slices per refresh in `cost` (default 4) |
| `--monitor N` | The Nth DXGI output over all adapters (default: the primary) |
| `--buffers N` | Swapchain buffers (default 2) |
| `--no-flush` | Leave the draw in the command buffer until Present flushes it |
| `--vary-clear` | Clear each frame to its slice's color instead of one fixed color (see 4.2) |
| `--exclusive` | Exclusive fullscreen and `Present(0, 0)` instead of a borderless window and the tearing flag |

Output files:

| File | Content |
|---|---|
| `summary.txt` | Everything the console shows: machine, power, mode, each section's numbers, and the display and session state over the run |
| `build.log` | The build |
| `raster_vblanks.csv` | Every vblank entry the sampler saw, with its bracket |
| `raster_reads_fit.csv` | Every 4th scanline read of the 2-s fit window, with the model's line |
| `raster_drift.csv` | The 2-s model's error per second over the drift run |
| `tear_N<n>_lead0.csv`, `tear_N<n>_cal.csv`, `tear_N<n>_sweep<i>_lead<us>us.csv` | One row per present: planned and actual times, the scanline after Present, GPU begin and end, DXGI's statistics |
| `cost_N<n>_<light,slice,full>.csv` | The same for the cost passes |
| `patch_*.csv` | With `--patch`: per patch and refresh, the time the raster reaches the patch row, on the ysp_rt clock |

Times in the CSV files are ns since the run's time zero; `summary.txt`
gives time zero on the ysp_rt (QPC) clock. The patch files use the
ysp_rt clock directly.

## 3. What each section measures

### 3.1 Setup

The probe makes a borderless window that covers the output, a D3D11
device on the adapter that owns that output, and a flip-discard
swapchain with `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` when
`CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)` says yes, as
`ysp/screen.h` does for DXGI_FLIP (without its ANGLE layer). It reads the
mode timing from the display configuration (`QueryDisplayConfig`:
active and total size, pixel rate, line rate, refresh) and opens the
output with `D3DKMTOpenAdapterFromGdiDisplayName` for
`D3DKMTGetScanLine`. A second thread logs the console display state and
the session lock, as in the v0.4.3 investigation of
[screen.md](screen.md).

### 3.2 path

Presents with `Present(1, 0)`, then 240 presents with
`Present(0, ALLOW_TEARING)` 4 ms apart, about 4 per refresh. It reports
the presentation path from `GetFrameStatisticsMedia`, the duration of
the Present call, and DXGI's count of presents shown per refresh. A
flip that tears in shows about 4 per refresh and returns at once; a flip
held to the vblank shows 1 and blocks for up to a refresh. It runs this
twice: once with a full clear to a new color each frame, once with one
clear color and a half-screen ClearView that changes the frame (see 4.2).

### 3.3 recipes

24 kinds of frame, 120 tearing presents each, 4 ms apart: full clears,
ClearViews, a full-screen shader, and combinations, with the clear color
fixed or changing. It reports presents shown per refresh for each. This
section exists because the path test found that the frame's content
decides whether a tearing present tears in on this GPU.

### 3.4 raster

A time-critical thread polls `D3DKMTGetScanLine` in a loop (about 1
million reads per second) and stamps each read at the middle of the
call. Every change of `InVerticalBlank` from 0 to 1 is a vblank entry.
From the first 2 s the probe fits:

- the vblank grid: entry time = T0 + n P, by least squares;
- the line model: line = a + b (t minus the last entry), from the reads
  in the active area. b is the line rate; b P is the vertical total; -a/b
  is the time from the vblank entry to line 0.

It reports the model's prediction error over the 2 s (in lines and us),
then the 2-s model's error per second over the next 60 s (the drift),
and a fit over all 62 s. Presents with `Present(1)` run beside it, with
frames that differ by one code value (invisible).

### 3.5 psr

What software can see of Panel Self Refresh: the connector type, the
adapter's registry values whose names contain PSR, DRRS, LRR, self
refresh or FeatureTestControl (read without admin), and the raster over
5 s with a static image (no presents), 5 s with a changing image, and 5 s
static again. For each: vblank intervals, reads where the scanline did
not move for two line times, and the error against the raster model.

### 3.6 tear

For each N: the slice k covers rows [kH/N, (k+1)H/N). Its present is
planned for the time the model says the raster reaches row kH/N, minus a
lead. The loop sleeps until the plan minus the draw lead, reads the
input (the cursor), draws, flushes, sleeps until the plan, calls
`Present(0, ALLOW_TEARING)`, and reads the scanline at once. A pass with
lead 0 measures the lateness; a second pass uses its median as the lead.
For each pass: the scanline after Present minus the target line, the
Present call's duration, presents shown per refresh, the GPU time per
slice (D3D11 timestamp queries, mapped to QPC by a calibration at the
start and end of each pass), and the GPU's end against the time the
raster reaches the slice. The model's T0 and P are refitted from 1 s of
reads before each pass.

The scanline after Present is not the tear line. The flip happens after
Present returns, when the GPU work and the kernel's flip are done. DXGI
stamps its statistics at vblanks, also for these flips (section 4.4), so
no software source gives the time of a mid-frame flip. The static check
and a photodiode can (section 6).

**The seam between refreshes.** Slice 0 starts a new refresh. If its
tear lands below row 0, rows 0 to the tear show the frame of the last
slice of the refresh before. Before 2026-10-10 the guard did not wrap, so
those rows showed the wrong color at any guard (section 4.9). Now the last
slice also draws rows 0 to the guard, and slice 0 draws the last guard
rows (for an early tear, which lands in the bottom rows of the refresh
before). `--seam-lines N` moves slice 0's target N lines before line 0,
into the vertical blanking (35 lines here). There, a tear that lands up
to N lines late changes no visible row. The target line in the CSV files
is then negative (-N), and the scanline read is in the blanking
(1200 to 1234 here: the same line modulo the vertical total). A value
larger than the blanking aims into the bottom rows of the refresh
before; the probe prints a note.

**The lead sweep.** `--sweep-lead A:B:STEP` runs one pass per lead, each
for `--seconds`, and refits the model for 1 s before each step (a plain
gray screen between steps). The lead shows in us as large seven-segment
digits at a third of the width and three eighths of the height, the same
in every frame of the step, so a tear does not cut them. Each step's
start goes to the console and `summary.txt` with the seconds since time
zero and the local clock time, and a table at the end gives the scanline
offsets per step. Use it with `--static-check` to find by eye the lead at
which the stripes go.

**The ruler.** `--ruler` draws the same ticks in every frame, so the
ticks show the rows of a stripe: count from the long tick at the target
row (8 rows per tick, a medium tick every 32 rows). The ruler and the
digits are one ClearView with a list of rects. In one A/B pair (N = 4,
lead 20 us, 3 s) the GPU time per slice was p50 246 us without the ruler
and 317 us with it (254 us in a third run with it). The GPU was done
273 us or more before the raster reached the slice at p01 in all three.

### 3.7 cost

The GPU time of four draws on an offscreen target of the screen's size,
one at a time on an idle GPU: a full clear, a ClearView of one slice's
rows, a shader on one slice's rows (scissor), and the shader on the full
screen. The shader is a grating under a Gaussian, 8 sines and an exp per
pixel. Then three tear passes at `--cost-slices` with those draws per
slice.

### 3.8 latency

In software terms only: from the input read to the time the raster
reaches each row, over all rows. Three loops at the same refresh:

- classic: input read just after the previous vblank, flip at the vblank;
- late frame start: input read one budget before the vblank, where the
  budget is the p99 of a full-screen frame's submit-to-done time plus
  500 us;
- beam racing: measured from the tear passes, where each row shows the
  newest present whose tear (the later of the Present return and the
  GPU's end) came before the raster reached the row. This is a lower
  bound: the true tear is later by the flip's own latency. Also a model
  for N = 2, 4, 10 with the measured slice budget.

None of these include the panel's response or input delays (USB
polling, the OS).

## 4. Results on the development laptop

### 4.1 Conditions

Windows 11 25H2 (build 26200), Intel Iris Xe driving the built-in 1920 x
1200 panel at 60 Hz (eDP; the display configuration reports the
connector as "internal"), an RTX A500 with no output. AC power, Balanced
plan, machine otherwise quiet (runs under the measurement guard, load at
the start 0 to 20 %). Display on and session unlocked from the start to
the end of every run. Thread policy TIME_CRITICAL, P-cores.
Mode: active 1920 x 1200, total 2080 x 1235 (35 blanking lines), pixel
rate 154.130 MHz, line time 13.495 us, refresh 60.000779 Hz. The numbers
come from two full runs at 18:45 and 18:48 (`--no-flicker`, the second
with `--static-check`) and from the runs that found the clear-color rule
(17:48 to 18:41).

### 4.2 Tearing presents tear in only with an unchanged clear color

`DXGI_FEATURE_PRESENT_ALLOW_TEARING` is supported, and every present
went to an overlay plane ("overlay" from `GetFrameStatisticsMedia`; no
present was composed). But a tearing present is not always flipped at
once. The first runs presented frames that each started with a full
clear to the slice's color: every present waited for the next vblank
(the Present call took up to 16.7 ms, DXGI showed 1.00 present per
refresh, and its time stamps were on the vblanks). More buffers
(`--buffers 3`) and exclusive fullscreen (`--exclusive`, which this
system ran composed) did not change that.

The recipes section isolated the cause. Presents shown per refresh, 4
expected when the flips tear in, 120 presents 4 ms apart, two runs:

| Frame | Shown per refresh | Present call p50 |
|---|---|---|
| Full clear to a new color each frame (2 grays one code apart, or 2 colors) | 1.00 | 12.5 ms |
| Full clear to a new color, then a ClearView of 1 pixel, of half the screen, or a full-screen shader | 1.00 | 12.5 ms |
| ClearView of the whole surface to a new color, no full clear | 1.00 | 12.5 ms |
| Full clear to one fixed color, frames equal | 3.73 | 24 us |
| Full clear to one fixed color, then a ClearView that changes half or a quarter of the screen | 3.73 | 23 us |
| Full clear to one fixed color, then a ClearView of all rows but one, to a new color | 3.73 | 24 us |
| Full clear to one fixed color, then a full-screen shader that changes every frame | 3.73 | 23 us |
| Full-screen shader only, after buffers cleared to one fixed color | 3.73 | 24 us |
| Full-screen shader only, after buffers cleared to two alternating colors | 1.00 | 12.5 ms |

(3.73 and not 4: 4 ms presents, 16.67 ms refresh.)

The rule that fits every row: a tearing present flips at once only when
its buffer was last cleared (by a full clear, or by a ClearView that
covers the whole surface) to the same color as the buffer on the screen.
Otherwise it waits for the vblank. A likely cause is Intel's render
compression with a stored fast-clear color: if the display engine takes
the clear color only at a vblank, a flip between buffers with different
clear colors cannot be done mid-frame. That cause is not verified; the
rule is measured, on this driver only.

All later runs start every frame with a full clear to one fixed color
and draw the slice's content over it. `--vary-clear` brings the old
behavior back.

### 4.3 The raster model

| Measure | Result |
|---|---|
| Scanline reads | 0.85 to 1.03 million per second; call width p50 0.9 to 1.1 us, p99 1.0 to 1.9 us; no read failed |
| Values | 0 to 1199 in the active area, 1200 to 1234 with InVerticalBlank set |
| Vertical total | 1235.0 lines from the fit, equal to the display configuration's total |
| Line time | 13.495 us, equal to the display configuration's line rate |
| Vblank entry to line 0 | 479 to 481 us (35.5 lines) |
| Prediction error, fit window | p50 0.25 lines (3.4 us), p99 0.50 to 0.59 lines (6.7 to 7.9 us), max 0.82 lines (11 us) in every run: the 1-line quantization of the reads, not the model |
| Vblank entries against a 62-s fit | residual p99 2.3 to 8.4 us, max 12 to 15 us |
| Period from a 2-s fit against a 62-s fit | -1.87 to +0.07 ppm over 5 runs; the 2-s model drifted -111 to +5 us per minute |
| Refits before passes | moved the prediction by 0.2 to 147 us (147 us after 3 minutes without one) |

The scanout is steady against QPC within the read's resolution over a
minute: the model's error is set by the period estimate, not by a
wandering display clock. A 2-s fit is not good enough for more than a
few seconds; a mode in the header must keep refitting.

### 4.4 Tear placement

With the fixed clear color, every pass showed N presents per refresh.
The scanline read right after Present returned, minus the target line,
after the lead calibration, both full runs:

| N | Slice period | Lead (lead 0 lateness) | p01 | p50 | p99 | max abs, lines (us) | Present call p50, p99 |
|---|---|---|---|---|---|---|---|
| 2 | 8.33 ms | 40 to 54 us | -2 to -1 | -1 to 0 | +3 | 68 to 73 (920 to 990) | 28 us, 69 us |
| 4 | 4.17 ms | 13.5 to 27 us | -2 to -1 | -1 to 0 | +1 to +3 | 3 to 8 (40 to 108) | 25 us, 58 us |
| 10 | 1.67 ms | 13.5 to 27 us | -1 to 0 | 0 to +1 | +1 to +2 | 5 to 13 (68 to 175) | 19 to 22 us, 41 us |

So the present lands within 1 to 3 lines (13 to 40 us) of its target at
p99. A few presents per pass land far off (up to 73 lines); the likely
cause is a preempted thread, not checked. DXGI's
`SyncQPCTime` was at a vblank for every present (0 of about 9600 per
run mid-frame), so DXGI does not report when a tearing flip happened. The
true tear line is below the scanline read by the flip's latency, which
software cannot see.

Without the lead (lead 0) the read landed 1 to 4 lines after the target:
the Present call plus the read.

### 4.5 Slice cost

Offscreen, one draw on an idle GPU, 200 each, four runs (18:35 to 18:48):

| Draw | GPU p50 | GPU p99 | Submit to done p99 |
|---|---|---|---|
| Full clear | 27 to 29 us | 28 to 30 us | 232 to 334 us |
| ClearView of 300 rows | 66 to 98 us | 98 us | 263 to 409 us |
| Shader on 300 rows | 110 to 166 us | 112 to 167 us | 248 to 444 us |
| Shader on the full screen | 138 to 618 us | 310 to 620 us | 424 to 864 us |

The full-screen shader's time changed by up to 4.5x between runs and
within one run (p50 138 us, p99 412 us), likely the GPU's clock; the
slice draws changed by 1.5x at most. Submit to done has a floor of about
200 us (the flush and the event poll).

On the screen, N = 4, per slice including the fixed full clear:

| Draw per slice | GPU p50, p99 | GPU done before the raster reached the slice's first row |
|---|---|---|
| Clears (fixed clear, ClearView of the slice's rows) | 139 to 233 us, 153 to 344 us | yes; margin p01 +325 to +410 us |
| Shader on the slice's rows | 212 to 297 us, 322 to 416 us | yes; margin p01 +258 to +365 us |
| Shader on the full screen | 665 to 747 us, 763 to 773 us | no; p50 30 to 219 us late; GPU still busy at the Present call on 810 to 1175 of 1196 |

With a draw lead of 1 ms, a slice that draws only its own rows is done
0.26 to 0.41 ms before the raster reaches it. A slice that redraws the
full screen is late. Its present still tore in (4 per refresh), so its
tear came after the target by the GPU's lateness. A beam-racing mode
draws the slice's rows only, or takes a longer draw lead.

The GPU timestamp clock (19.2 MHz) ran within 10 ppm of QPC over 5 to 20
s; the calibration bracket was 77 to 236 us wide.

### 4.6 Panel Self Refresh

| Sign | Result |
|---|---|
| Connector | internal (eDP: PSR is possible) |
| Registry (display class key, no admin) | `PSR2Disable = 0`, `Psr2DrrsEnable = 1`, `FeatureTestControl = 0x1200` |
| Intel's control library (`ControlLib.dll`, power optimization caps per output) | no data: every output returned "not initialized" (scratch tool, not in the repo) |
| Vblank intervals, static against changing image | p50 16666.4 us in both; 0 to 2 of 300 more than 1% off in either, from the sampler (a missed entry gives a 33.3 ms interval, a late stamp a pair at 16.33 and 17.00 ms) |
| Scanline frozen for two line times | 0 of about 850,000 read pairs in every phase |
| Raster model error, static against changing | the same within 1 us (only the 2-s fit's drift changes it) |

The source's timing generator runs on with a static image. So
`D3DKMTGetScanLine` reports the GPU's scanout, not the panel's. If the
panel enters PSR, it refreshes from its own memory, and the software
cannot see that. The registry says PSR2 is not disabled. Only a
photodiode at two or three screen heights can show whether a row's light
follows the GPU's raster during beam racing (section 6.2).

### 4.7 Latency, software terms

Input read to the raster reaching the row, over all rows, final runs:

| Loop | p50 | p99 | max |
|---|---|---|---|
| Classic: input after the previous vblank | 25.2 ms | 33.1 ms | 33.3 ms |
| Late frame start, budget 1.08 to 1.21 ms | 9.6 to 9.7 ms | 17.5 to 17.7 ms | 17.8 ms |
| Beam racing, N = 2, measured (lower bound) | 5.0 ms | 9.0 ms | 17.6 ms |
| Beam racing, N = 4, measured (lower bound) | 3.0 ms | 5.0 ms | 17.6 ms |
| Beam racing, N = 4, full-screen shader per slice, measured | 3.1 to 3.2 ms | 5.5 to 5.6 ms | 17.8 ms |
| Beam racing, N = 10, measured (lower bound) | 1.8 ms | 2.5 to 2.6 ms | 17.6 ms |
| Beam racing, N = 10, model with the slice budget | 1.3 ms | 2.1 ms | 2.1 ms |

The max of 17.6 ms comes from the 1 to 3 rows at the top: slice 0's tear
lands just below row 0, so those rows show the previous refresh's last
slice. Planning slice 0's tear inside the vblank removes it. Against the
late frame start, beam racing at N = 4 cuts the p50 by about 6.5 ms and
the p99 by about 12.5 ms; at N = 10, by about 8 and 15 ms. The cost is a
thread that wakes N times per refresh, and the GPU budget per slice.

### 4.8 Is this panel usable for beam racing?

In software, yes, with one condition: every frame keeps one fixed clear
color. Then presents tear in at N up to 10, the scanline after Present
lands within 1 to 3 lines of the target at p99, and slices that draw
their own rows finish 0.3 ms early. Whether the light follows is not
known: the panel is eDP with PSR2 enabled, and a panel that buffers a
frame in self refresh would show each row at its own time, not at the
GPU raster's. The photodiode test in 6.2 decides it. Until then, any
row-dependent onset on this panel is a model, not a measurement.

### 4.9 Eye test of the static check (2026-10-10)

The user watched `--only tear --slices 4 --static-check` (magenta as the
wrong color) on the built-in panel, PSR2 enabled, with the calibrated
lead, before the guard wrapped:

| Guard | What showed |
|---|---|
| 8 rows | Steady magenta stripes at every slice edge |
| 32 rows | 4 stripes all the time. The top stripe is thick, the others thinner |
| 64 rows | The top stripe thick and all the time; the bottom stripe some of the time; the middle stripes gone |

In the same conditions the scanline read after Present was within 1 to 3
lines of the target at p99 (4.4). Interpretation, not measured: the real
tear lands about 50 to 70 rows (0.7 to 0.9 ms) after that read, with a
spread of some rows. A stripe shows the rows between the target plus the
guard and the real tear, so guard 64 hides most of them. The top stripe is
the slice-0 seam: rows 0 to the tear show the last slice's frame, which
did not draw the top rows, so no guard hid it. The guard now wraps, and
`--seam-lines` aims slice 0 into the blanking (3.6). The scanline read is
a lower bound of the tear, as 3.6 says; on this panel it is short by
about 0.8 ms. If the photodiode test (6.2) confirms it, a mode in the
header cannot learn its lead from the scanline read alone.

Check runs of the new options (2026-10-09, 22:14 to 22:20, quiet
machine, `--no-flicker`, nobody watching): a sweep of 0, 200 and 400 us
(2 s each) gave 3.99 presents shown per refresh in each step, no late
slice, and scanline offsets of p50 +5, -11 and -25 lines (14.8 lines
per 200 us, as the line time predicts). With `--seam-lines 20 --lead 20`
slice 0's scanline read was in the blanking on 177 of 716 presents (the
slice-0 share at N = 4), its miss p50 +1 p99 +4 lines against -20, and
3.99 (N = 4) and 9.99 (N = 10) presents shown per refresh. The new
options do not change the path: every present still went to an overlay
plane.

Lead test by eye (2026-10-10, guard 8, before the guard wrapped, magenta):

| Lead | Top stripe (slice-0 seam) | Other edges |
|---|---|---|
| 600 us | Thin, always on | Flicker, off more than on |
| 800 us | Flickers in and out | Always on, width changes |
| 1000 us | Mostly gone | Always on |

The scanline read put the 800 us presents 54 to 58 lines before their
target rows (p50 -57 lines). Interpretation, not measured: on this panel
the light lags the scanline read by about 600 us (about 44 lines) at
the middle edges, with a spread of roughly 8 rows either way, since at
600 us those stripes come and go at guard 8 and at 800 us they show all
the time at changing widths (the tear now lands early). The seam needs
about 400 us more lead than the middle edges, which is what
`--seam-lines` gives slice 0 alone. Next: a sweep of 400 to 700 us in
steps of 50 with `--ruler` to read the offset in rows; then the best
lead with the wrapped guard and `--seam-lines 30`; then the same on an
external monitor to tell PSR2 from a pipeline delay.

## 5. What this decides for ysp/screen.h

An opt-in beam-racing present mode, `DXGI_FLIP` only:

1. **Swapchain.** Created with `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` when
   the mode is asked for and `CheckFeatureSupport` allows it, fullscreen
   borderless on an overlay or independent-flip path. A composed path
   cannot tear, and `COMPOSITION` presents at a target time on a vblank:
   the mode refuses both and says why.
2. **The clear-color rule.** Each slice frame starts with a full clear to
   one fixed color (the presenter's, not the caller's), and no draw clears
   the whole surface to another color. Through ANGLE this needs its own
   check: whether `glClear` maps to a D3D11 fast clear, and whether a
   frame drawn by ANGLE keeps the rule (not measured).
3. **The raster model in the header.** `D3DKMTGetScanLine` reads give the
   vblank grid and the line rate; the model refits all the time (a 2-s
   period estimate drifted up to 111 us per minute) and re-anchors on the
   scanline read after each present. The vertical total and line rate
   from `QueryDisplayConfig` check the fit.
4. **Plan per slice.** Present at the model's time for the slice's first
   row minus a learned lead, flush the draw at once, draw only the
   slice's rows, wake one draw budget early. Slice 0 aims inside the
   vblank. The lead from the scanline read is 13.5 to 54 us here, but the
   eye test (4.9) puts the real tear about 0.8 ms later: the lead must
   come from light (a photodiode, or the static check by eye), not from
   the scanline read alone.
5. **Record per slice.** Present call and return times, the scanline and
   vblank flag read after Present, the rows the slice covers, the target
   line and lead, the GPU's end (timestamp queries) when available, the
   slice and refresh index, and DXGI's present count, from which
   "presents shown per refresh" detects slices that did not tear in.
6. **Onsets and tiers.** A row's onset is the model's time for that row
   after the slice's tear: tier 2 (a model), since no software source
   stamps a mid-frame flip and the panel's light is not known. Tier 1
   needs a photodiode run at three heights on that panel (6.2). A
   present that waited for the vblank (Present blocks, or fewer than N
   shown per refresh) gets the vblank onset and a flag.
7. **The sync guard.** In this mode the guard's evidence is the design:
   several flips per refresh, flips off the grid. The guard stays off
   for beam-racing presents and checks the opposite: slices that should
   tear in but wait for vblanks (a driver setting that forces vsync, or a
   broken clear-color rule).
8. **Not built yet.** The decision to build it waits for the photodiode
   test, the external-monitor run, and the ANGLE check of item 2.

## 6. Hand tests for the user

### 6.1 Static check, by eye

Without `--no-flicker` the wrong color is magenta; read the warning. The
first results are in 4.9. Record what you see (stripes, their place and
height in ruler ticks) with the `summary.txt` of each run.

1. **The wrap.** Run
   `beam_race_probe --only tear --slices 4 --seconds 30 --static-check --guard 64 --ruler`.
   Before the guard wrapped, the top stripe stayed at guard 64. If 4.9 is
   right, the top stripe now goes like the middle ones.
2. **The lead sweep, guard 8.** Run
   `beam_race_probe --only tear --slices 4 --seconds 5 --static-check --guard 8 --ruler --sweep-lead 0:1500:100`
   (about 100 s). Write down the lead on the screen when the stripes go,
   and when stripes come back above the long ticks (the tear is then
   early). If 4.9 is right, they go near 700 to 900 us.
3. **The seam, on and off.** With the best lead L from step 2, run
   `beam_race_probe --only tear --slices 4 --seconds 30 --static-check --guard 8 --ruler --lead L`,
   then the same with `--seam-lines 20`. Without the seam, the top edge
   shows a stripe when slice 0's tear is late by more than 8 rows. With
   it, a tear up to 20 lines late lands in the blanking.
4. Run step 2 with `--slices 10` and with `--vary-clear`.

### 6.2 Photodiode at three heights

Needs the MCU photodiode board (`firmware/ysp_line/`, see
[device.md](device.md), "How-to: run the photodiode board").

1. Tape the photodiode over the top patch: the left edge, 120 rows down
   (1/10 of 1200).
2. In one console start `photodiode_check --key <key> --monitor 80 > edges_top.txt`.
3. In another, run
   `beam_race_probe --only tear --slices 4 --seconds 60 --patch --static-check --yes`
   (the patches flicker at 30 Hz).
4. Repeat for the middle patch (600 rows down) and the bottom patch (1080
   rows down).
5. For each light edge, find the patch row's `t_row_abs_ns` in
   `patch_tear_N4_cal.csv` for that refresh (both are on the ysp_rt
   clock) and compute edge minus `t_row_abs_ns`.
6. Compare the three heights. If the per-row onset model holds, edge
   minus model is the same at all three (the panel's response). A value
   that grows with height, or edges that come only at vblank times, means
   the panel does not follow the GPU's raster (a buffered or self-refresh
   panel), and beam racing gains nothing on it.
7. For reference, run `photodiode_check --key <key>` in window mode on the
   top-left corner: edge minus flip onset with vsync.

### 6.3 External monitor

The laptop has no external display now. Connect one to a port the Iris
Xe drives (HDMI or USB-C), then run
`sh tests/probe/beam_race/run.sh . --monitor 1 --no-flicker` (use the
output number the probe lists). An external monitor has no PSR, so it
separates the panel from the GPU: compare path, recipes, tear and the
photodiode test with section 4.

### 6.4 The Intel PSR setting

If Intel Graphics Software offers a Panel Self-Refresh switch, turn it
off, run `--only path,recipes,psr,tear` and the photodiode test again,
then turn it back on. Record whether the clear-color rule and the light
change.

## 7. Not measured

- The tear line itself: no software source stamps a mid-frame flip.
- Light: no photodiode was attached.
- Any GPU but the Iris Xe; any display but the built-in panel; any rate
  but 60 Hz; Windows 10.
- ANGLE: the probe draws with D3D11. Whether frames drawn through ANGLE
  keep the clear-color rule.
- Load: all runs were on a quiet machine. A busy CPU delays the slice's
  wake; a busy GPU delays its end.
- Input: the latency covers the software path from the input read only.
