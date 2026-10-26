# The ysp-av probe

A prototype, not the ysp-av API. It measures whether a frame loop in
Python, MATLAB or Octave keeps a 60 Hz display on time with ysp/screen.h,
ysp/gfx.h, ysp/timeline.h and ysp/rt.h, and finds the platform traps.
ysp/audio.h and ysp/video.h are compiled in too, as in the planned
module. The results are in [docs/av_probe.md](../../../docs/av_probe.md).
Windows only.

| File | What it is |
|---|---|
| `probe_core.h` | The shared core: open, the scene, one call per step of a frame, the per-frame log, the idle watcher, the Shift+Esc sender, the module list. |
| `probe_c.c`, `build_c.bat` | The same loop in C, the reference. |
| `py/ysp_av_probe.c`, `py/setup.py`, `py/run_py.py` | The CPython extension (Limited API, abi3, cp38+) and its loop. |
| `mex/ysp_av_probe.c`, `mex/build_mex.m`, `mex/run_mex.m`, `mex/prompt_idle.m` | The MEX file (MATLAB and Octave), its loop, and the idle-at-the-prompt test. |
| `analyze.py` | The summary table of the per-frame CSVs. |

## Requirements

- MSVC (Visual Studio 2022 or later), the SDL3 VC development package
  (3.2 or later), and ANGLE's `libEGL.dll` and `libGLESv2.dll`.
- `miniaudio.h` 0.11.25 (the file CMake's `YSP_FETCH_MINIAUDIO` gets).
- Python through `uv`; MATLAB R2023a with `mex -setup C` done; Octave 10.1.

## Make a bundle

A bundle is one directory with the module and the DLLs beside it, as a
wheel or a MEX zip carries them. Nothing goes on `PATH`.

1. Make three directories: `bin` (C and Python), `mbin` (MATLAB), `obin`
   (Octave).
2. Copy `SDL3.dll`, `libEGL.dll` and `libGLESv2.dll` into each.
3. Build:

   ```sh
   cmd //c "tests\probe\av\build_c.bat" C:\sdk\SDL3-3.4.0 MINIAUDIO_DIR BIN
   cd tests/probe/av/py
   SDL3_ROOT=C:/sdk/SDL3-3.4.0 MINIAUDIO_DIR=... uv run --no-project --isolated \
       --with setuptools python setup.py build_ext --build-lib BIN --build-temp TMP
   rm -rf build *.egg-info
   matlab -batch "addpath('tests/probe/av/mex'); build_mex('MBIN','C:/sdk/SDL3-3.4.0','MINIAUDIO_DIR')"
   octave-cli --eval "addpath('tests/probe/av/mex'); build_mex('OBIN','C:/sdk/SDL3-3.4.0','MINIAUDIO_DIR')"
   ```

   Octave's MinGW `ld` 2.43 aborts on SDL 3.4's MSVC import library, so
   `build_mex.m` links Octave's MEX against the `SDL3.dll` in `OBIN`.

## Run

Each run opens a window (or the full screen), draws N stimuli each frame
(gabor, grating, dot field of 100 dots, rect, in turn; stimulus 0's phase
follows a 1 s repeating tween), and writes one CSV row per frame. Shift+Esc
ends a run.

```sh
BIN/probe_c.exe [--windowed] --n 10 --seconds 600 --csv c.csv
uv run --no-project --python 3.12 python tests/probe/av/py/run_py.py --bundle BIN \
    [--windowed] --n 10 --seconds 600 --gc on|off|freeze --csv py.csv
matlab -batch "addpath('tests/probe/av/mex'); run_mex('MBIN','windowed',0,'n',10,'seconds',600,'csv','m.csv')"
octave-cli --eval "addpath('tests/probe/av/mex'); run_mex('OBIN','windowed',0,'n',10,'seconds',600,'csv','o.csv')"
uv run --no-project python tests/probe/av/analyze.py *.csv
```

Other options, the same in each host: `--idle S` (an idle gap after 120
frames, with the window watched; Python also takes `--idle-mode
sleep|input|pump` and `--noghost`, MATLAB `prompt_idle.m` for the idle at
its prompt), `--abort` (one Shift+Esc from a helper
thread), `--panic` (the interpreter hangs, then 3 presses; the process
must end with exit code 99), `--audio null|default`, `--angle-dir DIR`,
`--preload DLL`, `--modules`. In MATLAB and Octave the options are
name-value pairs of `run_mex`. Timing runs go through
`bash /c/tmp/psy-guard.sh time NAME CMD...` on a quiet machine.

The helper thread sends Shift+Esc only while the probe's window (or its
ghost) is the foreground window, so no other program gets the keys.
