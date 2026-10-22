# Build

This page is a how-to guide: build the examples, tests and tools of this
repository, or use the headers from CMake. To use one header in your own
project, see the [Quick start](../README.md#quick-start).

Every header compiles as C99, C11 or C++17, and in the C dialect MSVC
compiles by default.

To build the examples and compile checks in this repository:

```sh
cmake -B build/default
cmake --build build/default
ctest --test-dir build/default
```

`CMakePresets.json` names the configurations in use: `msvc` (Visual Studio
2022, examples and tests), `msvc-full` (`msvc` with SDL3, ANGLE, the layout
library, the loopback tests and both downloads), `mingw`, and on Linux the CI
sets `linux`, `clang`, `sanitizers` and `nothreads-cxx`. Each preset builds
into `build/<preset>/`: `cmake --preset msvc`, then
`cmake --build --preset msvc` and `ctest --preset msvc`. `msvc-full` reads
`SDL3_DIR` and `YSP_ANGLE_DIR` from the environment; to keep paths for one
machine, write a `CMakeUserPresets.json` (git ignores it) that inherits a
preset.

Three options need files that are fetched, not committed. Run each fetch
once from the repository root:

| Option | Builds | Fetch |
|---|---|---|
| `-DYSP_BUILD_PACK=ON` | the pack tool `ypak` (core: every kind but glyph runs) | `uv run --no-project python tools/vendor_pack.py --write` (lodepng) |
| `-DYSP_BUILD_LAYOUT=ON` | the layout library; with `YSP_BUILD_PACK`, glyph runs in `ypak` | `uv run --no-project python tools/vendor_layout.py --write` (about 8 MB) |
| none (ysp/net.h's tests) | the liblsl ABI check and the loopback test | `uv run --no-project python tools/vendor_lsl.py --write` |

Or without CMake, from the repository root:

```sh
cc -O2 -pthread -Iinclude -o parallel_trigger examples/parallel/trigger.c   # Linux
cl /O2 /Iinclude examples\parallel\trigger.c                                # Windows (MSVC)
```

CMake consumers can `add_subdirectory(ysp)` and link the group that holds the
headers a program uses. Each group adds the include path, the groups that its
headers include, and the libraries an implementation file needs:

- `ysp::core` holds `ysp/rt.h`, `ysp/timeline.h`, `ysp/table.h`,
  `ysp/json.h` and `ysp/pack.h`, and links pthreads on POSIX and `libm` on
  Linux.
- `ysp::methods` holds `ysp/stair.h`, `ysp/quest.h`, `ysp/aep.h`,
  `ysp/trials.h` and `ysp/dataset.h`, and links `ysp::core`.
- `ysp::devices` holds `ysp/serial.h`, `ysp/parallel.h`, `ysp/input.h`,
  `ysp/response.h`, `ysp/box.h`, `ysp/device.h`, `ysp/rigfile.h` and
  `ysp/net.h`, and links `ysp::core`, `setupapi` on Windows and `libdl`
  (ysp/net.h loads liblsl).
- `ysp::rig` holds `ysp/screen.h`, `ysp/gfx.h`, `ysp/color.h`,
  `ysp/outline.h`, `ysp/rdk.h`, `ysp/video.h` and `ysp/audio.h`, and links
  `ysp::core`, `ysp::devices`, `libdl`, and the SDL3, miniaudio and pl_mpeg
  that exist when it is added (from `find_package(SDL3)`,
  `YSP_MINIAUDIO_DIR` and `YSP_PL_MPEG_DIR`).
- `ysp::ysp` links all four groups.
