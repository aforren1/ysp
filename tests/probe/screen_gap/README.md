# The grid after a gap

`gapdiag` measures `ysp/screen.h`'s vblank grid across an idle gap: the
cause of the GRID_UNSTABLE records after a wait between trials, the
vblank sources the system has during a gap, and the fix of v0.10.1
before and after. It gives the tables of
[docs/screen.md](../../../docs/screen.md#the-grid-after-a-gap-v0101).
Windows only.

| File | What it is |
|---|---|
| `gapdiag.c`, `build.bat` | The program: open a screen, run frames (GL clears) for `--pre` seconds, sleep `--gap` seconds with no call, run `--post` frames. It wraps the screen's presenter, so each OS vblank time is logged with the grid's state before the core used it (the nearest grid vblank and the offset). `--wake` logs the idle-wake waiter's vblank wakes (COMPOSITION), `--dwm` DWM's vblank time every 20 ms. `--dxgi` names DXGI_FLIP (else AUTO), `--windowed` a 1280 x 720 window, `--wt on|off` the window thread. |
| `run_cause.sh` | 2 s or 60 s of frames before a gap of 10 or 30 s, both backends: the period's error and the offset after the gap. |
| `run_sources.sh` | A 30 s gap with the waiter's wakes and DWM's time logged: fullscreen and windowed, both backends. |
| `run_gaps.sh` | Gaps of 5, 10, 30 and 60 s, both backends, window thread ON and OFF, fullscreen; a 30 s gap in a window and once more fullscreen. Run it once per header: before and after a change. |
| `gapan.py` | One summary per CSV: the grid's period before the gap against a least-squares fit of the OS's times, the offsets after the gap, the waiter's wakes and DWM's times against the OS's times. |
| `seg.py` | Least-squares periods over windows of one run (the wander table). |
| `d3table.py` | The before-and-after table of a directory of `run_gaps.sh` runs. |

## Build

MSVC, the SDL3 VC development package and ANGLE's DLLs. One build per
header: build the header before a change into one directory and the
header after it into another (check out each version of
`include/ysp/screen.h`, or copy it). Through the guard's build mode:

```sh
bash /c/tmp/psy-guard.sh build cmd //c "tests\probe\screen_gap\build.bat" C:\sdk\SDL3-3.4.0 ANGLE_DIR OUT_DIR [NAME]
```

## Run

Every run is a timing measurement. The scripts run each one through
`psy-guard.sh time` (set `GUARD` to a command that takes a run name and
then the command, elsewhere). Fullscreen runs cover the display for the
gap.

```sh
bash tests/probe/screen_gap/run_cause.sh OUT BEFORE/gapdiag.exe 2        # about 10 min
bash tests/probe/screen_gap/run_sources.sh OUT BEFORE/gapdiag.exe 2      # about 5 min
bash tests/probe/screen_gap/run_gaps.sh OUT BEFORE/gapdiag.exe 2         # about 30 min
bash tests/probe/screen_gap/run_gaps.sh OUT AFTER/gapdiag.exe 4          # about 30 min
```

The last argument tags the files (`..._v2`, `..._v4`) and is the header
column of the table. Git ignores `out/` and every `*.log` and `*.csv`
here; put `OUT` there or outside the repository.

## The tables

```sh
cd tests/probe/screen_gap
uv run --no-project python gapan.py OUT/comp_fs_pre2_gap30_v2.csv ...   # the cause, the sources
uv run --no-project python seg.py OUT/comp_fs_pre60_gap30_v2.csv        # the wander
uv run --no-project python d3table.py OUT                               # before and after
```

In docs/screen.md the cause table is `gapan.py` on `run_cause.sh`'s
CSVs (period, first OS time after the gap) with the GRID_UNSTABLE count
of each log; the source paragraph is `gapan.py` on `run_sources.sh`'s
CSVs (its "waiter wakes" and "DWM" lines); the before-and-after table is
`d3table.py` on the `run_gaps.sh` runs of both builds in one directory.
The 2026-10-27 table has two before builds (tags 2 and 3, the second a
first fix that never anchored) and the after build (tag 4).
