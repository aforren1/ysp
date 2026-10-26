# The window-thread probe

A prototype, not a change to `ysp/screen.h`. It answers whether the
window of `ysp/screen.h` can belong to a thread of the header's own, so
that a host that idles between frames (Python `time.sleep()`, `input()`,
Octave `pause`) never leaves the window without a message pump. The
results are in [docs/winthread_probe.md](../../../docs/winthread_probe.md).
Windows only.

| File | What it is |
|---|---|
| `include/ysp/screen.h` | A copy of `ysp/screen.h` 0.9.0 with the window thread. `YSCR_WINDOW_THREAD=1` in the environment at open turns it on; else the code is 0.9.0's. The build puts this directory before the repository's `include/`, so `ysp/gfx.h` and `ysp/video.h` get the copy. |
| `probe_core.h`, `probe_c.c`, `py/`, `mex/`, `analyze.py` | The ysp-av probe of branch `probe-vis` (docs/av_probe.md there), with `pv_wt_state()` (the window thread's state and CPU time), `pv_latency()` (input latency) and a panic test that expects the process to live. |
| `build_c.bat` | Builds `probe_c.exe` with MSVC, against SDL3's DLL or a static SDL3. |

## Requirements

- MSVC (Visual Studio 2022 or later), ANGLE's `libEGL.dll` and
  `libGLESv2.dll`, and `miniaudio.h` 0.11.25.
- SDL3 3.4.0: the VC development package, or a static SDL3 built from
  the release tarball (below).
- Python through `uv`; MATLAB R2023a with `mex -setup C` done; Octave 10.1.

## Build a static SDL3

1. Get `SDL3-3.4.0.tar.gz` from
   `https://github.com/libsdl-org/SDL/releases/download/release-3.4.0/`.
   Its SHA-256 must be
   `082cbf5f429e0d80820f68dc2b507a94d4cc1b4e70817b119bbb8ec6a69584b8`
   (the digest GitHub gives for the release asset). Extract it into an
   empty directory.
2. MSVC, from a shell with `vcvars64.bat` called (Ninja comes with
   Visual Studio's CMake):

   ```bat
   cmake -S SRC -B BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON ^
         -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX=PREFIX
   cmake --build BUILD
   cmake --install BUILD
   ```

   This gives `PREFIX/lib/SDL3-static.lib`.
3. MinGW-w64, with Octave 10.1's `mingw64/bin` first on `PATH`: the same
   command with `-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
   -DCMAKE_RC_COMPILER=windres`. This gives `PREFIX/lib/libSDL3.a`.
4. Link the system libraries that `SDL3.dll` imports itself:
   `user32 gdi32 winmm imm32 ole32 oleaut32 version uuid advapi32
   setupapi shell32 dinput8` (SDL's `sdl3.pc` lists them).

No `SDL_main` is needed: SDL 3.4 needs it on Windows only for its own
`SDL_main.h` entry point, which `ysp/screen.h` does not include.

## Build the probe

A bundle is one directory with the module and `libEGL.dll` and
`libGLESv2.dll` beside it (and `SDL3.dll` when SDL3 is not static).

```sh
cmd //c "tests\probe\winthread\build_c.bat" SDL_PREFIX MINIAUDIO_DIR BIN static
cd tests/probe/winthread/py
SDL3_STATIC=1 SDL3_ROOT=SDL_PREFIX MINIAUDIO_DIR=... uv run --no-project --isolated \
    --with setuptools python setup.py build_ext --build-lib BIN --build-temp TMP
rm -rf build *.egg-info
matlab -batch "addpath('tests/probe/winthread/mex'); build_mex('MBIN','MSVC_PREFIX','MINIAUDIO_DIR',true)"
octave-cli --eval "addpath('tests/probe/winthread/mex'); build_mex('OBIN','MINGW_PREFIX','MINIAUDIO_DIR',true)"
```

Leave out `static` (and `SDL3_STATIC`, and the last `build_mex`
argument) to link `SDL3.dll` from the VC development package, as the
`probe-vis` probe did.

## Run

The options are those of the `probe-vis` probe (its README), plus:

- `YSCR_WINDOW_THREAD=1` in the environment: the window thread. Without
  it the screen runs as `ysp/screen.h` 0.9.0 does. The describe output
  ends with `window_thread=0` or `1`.
- `probe_c --latency key|click [--lat-frames] [--lat-n N]`: N key presses
  (scan code, SDL's raw path) or left clicks (the message path) from a
  helper thread, sent only while the probe's window is in front; prints
  the time from `SendInput()` to the poll that returned the event
  ("avail") and to the event's own time ("stamp"). `--lat-frames` polls
  once per frame after `begin()`; without it the frame thread spins on
  `yscr_poll()`.
- `probe_c --dxgi`: the DXGI_FLIP backend in place of AUTO.
- `--panic`: with the window thread, the process must live. It prints
  the window thread's state (`released=1 iconic=1`) and then the abort
  the next `begin()` reports (`0x21` is the key and the panic's release).

Timing runs go through `bash /c/tmp/psy-guard.sh time NAME CMD...` on a
quiet machine.
