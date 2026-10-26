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

@@SUMMARY@@

## Setup

| Item | Value |
|---|---|
| Machine | Laptop, Intel Core i7-1360P (12 cores, 16 threads), Windows 11 Enterprise 10.0.26200 |
| GPU and display | Intel Iris Xe (driver 32.0.101.7088), internal panel 1920 x 1200 at 60 Hz (measured 59.9997 to 60.0013 Hz), one display; the NVIDIA RTX A500 is not wired to the panel |
| Power | AC, Balanced power plan |
| Screen | `ysp/screen.h` 0.9.0, AUTO backend: COMPOSITION in every run; fullscreen is a borderless window over the panel, windowed is 1280 x 720 |
| gfx | `ysp/gfx.h` 0.17.1, mid-gray background, no calibration, program cache off |
| ANGLE | 2.1.23876 (git fffbc739779a), from Docker Desktop's `frontend` directory |
| SDL | 3.4.0 (VC development package) |
| Hosts | MSVC 19.51 C (`probe_c.exe`, /O2); CPython 3.12.11 and 3.14.3 (one abi3 `.pyd` built with 3.12); MATLAB R2023a (MSVC 2022 MEX); Octave 10.1.0 (its MinGW-w64 gcc 14.2 MEX) |
| Timing | Every timing run through `bash /c/tmp/psy-guard.sh time`: one at a time, CPU load at most 20 % at its start |
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
makes the same calls. Before the loop each host makes a heap like an
experiment's: 100,000 small records (Python dicts, a MATLAB struct array).
The core logs, per frame, the times of the `begin()` call and return and
of the `flip_at()` call and return on the `ysp/rt.h` clock, and every
flip record (through `yscr_on_flip()`); it writes them as CSV at the end.

"Work" in the tables is the wall time from the return of `begin()` to the
call of `flip_at()`: everything the host does for the frame. "Outside" is
the time from the return of `flip_at()` to the next `begin()` call.

## 1. Missed frames with the interpreter in the loop

@@CAMPAIGN@@

## 2. MATLAB's ANGLE

MATLAB R2023a ships ANGLE 2.1.18846 (git 4a65a669e11b):
`bin/win64/libEGL.dll` (437,760 bytes), `libGLESv2.dll` (6,472,192) and
`d3dcompiler_47.dll` (4,891,080). The probe listed the loaded modules
(`K32EnumProcessModules`, full paths) before and after opening the
screen.

| Case | MATLAB's libEGL / libGLESv2 loaded before open | After open | Open | GL_VERSION reports |
|---|---|---|---|---|
| (a) `matlab -batch`, a `figure` and a `uifigure` drawn first | no (only `opengl32.dll` and MATLAB's `mwopengl_startup_plugin.dll`) | the bundle's | yes, 120 frames | ANGLE 2.1.23876 (the bundle's) |
| (a) desktop session (`matlab -r`), the same figures | no | the bundle's | yes | ANGLE 2.1.23876 |
| (b) `matlab -nojvm -batch` | no | the bundle's | yes | ANGLE 2.1.23876 |
| (c) MATLAB's copies loaded first by full path (`LoadLibraryW`), then open with the bundle's directory | yes | both copies, side by side | yes, 120 frames | ANGLE 2.1.23876 (the bundle's) |
| (e) `angle_dir` = MATLAB's `bin/win64` | no | MATLAB's | yes, 113 frames, independent flip, tier 1 | ANGLE 2.1.18846 (MATLAB's) |
| Python, MATLAB's copies loaded first (as PyQt5 or Kivy would load theirs) | yes | both copies | yes, 120 frames | ANGLE 2.1.23876 |

`d3dcompiler_47.dll` came from the host program's directory or from
System32, never from the bundle: MATLAB's in MATLAB, System32's in Python
and Octave, the bundle's only for `probe_c.exe` (the bundle is its program
directory).

Verdict: no clash on this machine. MATLAB does not load its ANGLE into
`MATLAB.exe` in any of the cases tried; the `uifigure` renders in another
process. When another ANGLE is already loaded, the bundle's still loads
and is the one used, for two reasons that the header and ANGLE give:

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

MATLAB's ANGLE works with the header (case e), so it is an option, not a
need. Its program compile was slower: the context hook (ygfx's parallel
compile start) took 0.595 s, against 0.012 to 0.038 s with ANGLE
2.1.23876.

No header change is needed for the clash. If one is wanted later: load
`libGLESv2.dll` by full path from the same directory before `libEGL.dll`,
so that the pair is fixed even with an ANGLE whose libEGL imports
libGLESv2 by name (older ANGLE builds did).

## 3. Responsiveness between trials

After 120 frames each host stopped calling the module for 10 s or 30 s
with the window open, then ran 120 more frames. A thread in the core
polled every 50 ms: `IsHungAppWindow()`, the foreground window and its
class, `IsIconic()`, and whether the window still covered the monitor.

| Host and idle | Window marked hung | Ghost window in front | Lost fullscreen or focus |
|---|---|---|---|
| C `Sleep()` | after 4.05 to 4.30 s | 50 ms later (class "Ghost", owned by dwm) | no: covers=1 throughout; focus back 50 ms after the next `begin()` |
| Python `time.sleep()` | after 4.30 to 4.50 s | 50 ms later | no |
| Python `input()` | after 4.25 to 4.55 s | 50 ms later | no |
| MATLAB `pause()` | never | never | no |
| MATLAB idle at its prompt (desktop, a timer ends it) | never | never | no |
| Octave `pause()` | after 4.10 to 4.30 s | 50 ms later | no |
| Python `time.sleep()` after `DisableProcessWindowsGhosting()` | after 4.30 s | never | no |

MATLAB pumps the Windows messages of its main thread in `pause` and at
the prompt, and the MEX file's window belongs to that thread, so the
window stays responsive. Python's and Octave's waits pump nothing.

The first frames after the idle gap (frame 120 is the first; "recovered"
is the first of 10 frames in a row with tier 1, no drop and no flag):

@@AFTER@@

With a ghost the first 2 or 3 frames after the gap went wrong in every
host: fullscreen, records of the composed or overlay path, flagged
ESTIMATED and OCCLUDED (0xa) or ONSET_PLANNED (0x100), tier 3, and
`begin()` waits of 80 ms; in a window, 1 to 3 vblanks dropped. Without a
ghost (MATLAB, and Python with ghosting off) the first frame was on time
or one vblank late, then tier 1.

@@PUMP@@

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
- The MinGW MEX file is 4,466,170 bytes, the MSVC one 635,904 (@@STRIP@@).
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

@@DECOY@@

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
about 16.8 MB. miniaudio needs no DLL of its own: with the default
device it loaded `avrt.dll`, `MMDevApi.dll` and `AUDIOSES.DLL` from
System32; the null device loads none.

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

No load or init conflict, and the frames that followed had no drop in
the 3 s smoke runs. No audio timing was measured.

Build note: with `ysp/audio.h`'s implementation before `ysp/screen.h`'s
in one file (the order ysp/video.h asks for), MSVC warns C4005
`CLSCTX_ALL` redefined: miniaudio defines it when `combaseapi.h` is not
yet included, and `d3d11_1.h` includes it later. @@C4005@@

## What this means for ysp-av

@@MEANS@@

## Not run

@@NOTRUN@@

## Open questions

@@OPEN@@
