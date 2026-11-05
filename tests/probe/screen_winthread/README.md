# The window-thread measurements

This harness measures `ysp/screen.h`'s window thread (v0.10.0) against
the frame thread's own window (`desc.window_thread` OFF, the v0.9.0
path) in C, Python, MATLAB and Octave. It gives the tables of
[docs/screen.md](../../../docs/screen.md#measured-on-v0100-the-window-thread-against-v090s-path).
It is the window-thread probe's harness (`docs/winthread_probe.md`,
branch `probe-winthread`), built against `include/` of this repository
in place of the probe's copy of the header. Windows only.

| File | What it is |
|---|---|
| `probe_core.h` | The shared core: open, the scene (N stimuli, one parameter set and one draw each per frame), the per-frame log, the idle watcher (`IsHungAppWindow`, the foreground), the Shift+Esc sender, the key latency and the window-call round trip. `desc.window_thread` comes from `YSCR_WINDOW_THREAD` in the environment: `1` ON, `0` OFF, unset AUTO. |
| `probe_c.c`, `build_c.bat` | The loop in C. |
| `py/ysp_av_probe.c`, `py/setup.py`, `py/run_py.py` | The CPython extension (abi3, cp38+) and its loop. |
| `mex/ysp_av_probe.c`, `mex/build_mex.m`, `mex/run_mex.m` | The MEX file for MATLAB and Octave, and its loop. |
| `run_long.sh` | The 10-minute frame loops: C and Python, N = 10 and 100, fullscreen and 1280 x 720, OFF then ON; MATLAB and Octave fullscreen, N = 10. |
| `run_short.sh` | Idle gaps of 5, 10 and 30 s with no pump; Shift+Esc; the panic while the host waits; MATLAB's and Octave's `pause(30)`, abort, panic and three opens; the key latency and the window-call round trip. |
| `analyze.py`, `table.py`, `idletable.py` | The tables. |

## Requirements

- MSVC (Visual Studio 2022 or later), ANGLE's `libEGL.dll` and
  `libGLESv2.dll`, and `miniaudio.h` 0.11.25.
- SDL3 3.4.0: the VC development package, or a static SDL3 (below).
- Python through `uv`; MATLAB R2023a with `mex -setup C` done; Octave
  10.1 with its MinGW-w64 gcc.
- `bash` (Git Bash) and, on the development machine, the timing guard
  `C:\tmp\psy-guard.sh`. Elsewhere set `GUARD` to a command that takes a
  run name and then the command to run, as `psy-guard.sh time NAME CMD...`
  does (a script of yours that drops the name, on a machine with nothing
  else running).

## Build a static SDL3

The measurements of docs/screen.md used a static SDL3, so each module
needs no `SDL3.dll`.

1. Get `SDL3-3.4.0.tar.gz` from
   `https://github.com/libsdl-org/SDL/releases/download/release-3.4.0/`.
   Its SHA-256 must be
   `082cbf5f429e0d80820f68dc2b507a94d4cc1b4e70817b119bbb8ec6a69584b8`.
   Extract it into an empty directory.
2. MSVC, from a shell with `vcvars64.bat` called:

   ```bat
   cmake -S SRC -B BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON ^
         -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX=PREFIX
   cmake --build BUILD
   cmake --install BUILD
   ```

   This gives `PREFIX/lib/SDL3-static.lib`.
3. For Octave: the same with Octave 10.1's `mingw64/bin` first on `PATH`
   and `-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
   -DCMAKE_RC_COMPILER=windres`. This gives `PREFIX/lib/libSDL3.a`.

## Build the bundles

A bundle is one directory with the module and `libEGL.dll` and
`libGLESv2.dll` beside it. Make three: `BIN` (C and Python), `MBIN`
(MATLAB), `OBIN` (Octave). From the repository root, through the guard's
build mode:

```sh
G="bash /c/tmp/psy-guard.sh build"
$G cmd //c "tests\probe\screen_winthread\build_c.bat" MSVC_PREFIX MINIAUDIO_DIR BIN static
cd tests/probe/screen_winthread/py
SDL3_STATIC=1 SDL3_ROOT=MSVC_PREFIX MINIAUDIO_DIR=... $G uv run --no-project --isolated \
    --with setuptools python setup.py build_ext --build-lib BIN --build-temp TMP
rm -rf build *.egg-info
cd -
$G matlab -batch "addpath('tests/probe/screen_winthread/mex'); build_mex('MBIN','MSVC_PREFIX','MINIAUDIO_DIR',true)"
$G octave-cli --eval "addpath('tests/probe/screen_winthread/mex'); build_mex('OBIN','MINGW_PREFIX','MINIAUDIO_DIR',true)"
```

Leave out `static` (and `SDL3_STATIC`, and the last `build_mex`
argument) to link `SDL3.dll` from the VC package; then copy `SDL3.dll`
into each bundle.

## Run

Every run is a timing measurement: run it through the guard's time mode,
one at a time, on a quiet machine. The two scripts do that themselves,
one guard session per run:

```sh
BIN=... MBIN=... OBIN=... OUT=out bash tests/probe/screen_winthread/run_long.sh    # about 4 h
BIN=... MBIN=... OBIN=... OUT=out bash tests/probe/screen_winthread/run_short.sh   # about 1.5 h
```

`OUT` defaults to `out/` here; git ignores it and every `*.log` and
`*.csv` here. Leave `MBIN` and `OBIN` empty to skip MATLAB and Octave.
`SEC` (default 600) sets the length of a frame loop. Each run writes
`NAME.log` and, for frame loops and idle gaps, `NAME.csv` (one row per
frame). The keys go out with `SendInput` only while the probe's window is
in front, so nothing reaches another program; leave the machine alone
while the scripts run. Fullscreen runs cover the display.

A run that another program disturbed (OCCLUDED records, a cluster of
misses with the work per frame up) goes again as a pair, OFF and ON:
docs/screen.md lists the two pairs that went again on 2026-10-27.

## The tables

```sh
cd tests/probe/screen_winthread
uv run --no-project python table.py out/c_*_wt?.csv out/py_*_wt?.csv out/m_*.csv out/o_*.csv
uv run --no-project python idletable.py out
uv run --no-project python analyze.py --gap 120 out/m_pause_*.csv out/o_pause_*.csv
```

- `table.py`: the 10-minute table (frames, misses, late targets,
  occluded, residuals, the work per frame, paths and tiers, and the window
  thread's CPU time from the log beside each CSV).
- `idletable.py`: the idle-gap table (when the window was marked hung and
  the ghost came, from the watcher in the log; frame 120 and the bad
  frames after the gap, from the CSV).
- `analyze.py --gap 120`: the first frames after MATLAB's and Octave's
  `pause`. The hung and ghost times are the watcher's lines in each log.
- The abort, panic, reopen, latency and round-trip results are lines of
  the logs: `abort:`, `panic did not end the process:`, `after the
  panic:`, `three opens done`, `avail ms`, `stamp ms`, `window thread
  command round trip`.
