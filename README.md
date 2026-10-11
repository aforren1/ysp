# ysp

Single-header, public-domain C libraries for psychophysics and
neuroscience rigs, in the style of [stb](https://github.com/nothings/stb)
and [sokol](https://github.com/floooh/sokol). Each header compiles as C99,
C11 or C++17, and its top comment is its manual.

## Headers

| Header | Prefix | Purpose | Version | Status |
|---|---|---|---|---|
| [rt](docs/rt.md) | `yrt_` | Clock, deadline waits, worker, pump, event ring, clock fit | 0.8.0 | 1 machine |
| [parallel](docs/parallel.md) | `ypar_` | Parallel port trigger output and line input | 0.4.0 | no device |
| [serial](docs/serial.md) | `yser_` | Serial port byte I/O for trigger and response boxes | 0.5.0 | no device |
| [trials](docs/trials.md) | `ytr_` | Trial orders, blocks, adaptive tracks, jitter, replay, a CSV row per trial | 0.4.0 | tested |
| [table](docs/table.md) | `ytb_` | Typed tables and a bounded CSV parser | 0.2.0 | tested |
| [pack](docs/pack.md) | `ypak_` | Read and verify ysp packs (store-only zip) | 0.2.0 | tested |
| [json](docs/json.md) | `yjs_` | Strict JSON reader and canonical writer | 0.1.1 | tested |
| [stair](docs/adapt.md) | `yst_` | Adaptive staircases | 0.2.0 | tested |
| [quest](docs/adapt.md) | `yqst_` | QUEST+, QUEST, Psi and Psi-marginal on a grid | 0.6.0 | tested |
| [aep](docs/adapt.md) | `yaep_` | Gaussian-process adaptive estimation (AEPsych features) | 0.15.0 | tested |
| [timeline](docs/timeline.md) | `ytl_` | Events, tracks and keyframes on the clock | 0.5.0 | tested |
| [input](docs/input.md) | `yin_` | The input event that every device shares | 0.4.0 | 1 machine |
| [response](docs/response.md) | `yrsp_` | Trial responses and RT from any input device | 0.3.0 | 1 machine |
| [box](docs/box.md) | `ybox_` | Response and trigger box protocols, bytes to events | 0.3.0 | no device |
| [device](docs/device.md) | `ydev_` | Boxes and boards on the rig clock, with outputs | 0.3.0 | no device |
| [rigfile](docs/device.md#how-to-keep-a-rigs-devices-and-latencies-in-its-rig-profile) | `yrig_` | Rig profile: devices per role, loopback results | 0.3.0 | no device |
| [net](docs/net.md) | `ynet_` | Lab Streaming Layer markers and streams | 0.3.0 | no device |
| [color](docs/color.md) | `ycol_` | Display calibration and color space conversions | 0.2.0 | tested |
| [rdk](docs/rdk.md) | `yrdk_` | Random dot kinematograms, reproducible bit for bit | 0.2.0 | tested |
| [outline](docs/outline.md) | `yol_` | Paths, font glyphs and SVG into curve sets | 0.3.1 | tested |
| [screen](docs/screen.md) | `yscr_` | Stimulus window, flip at a time, flip records | 0.6.0 | 1 machine |
| [audio](docs/audio.md) | `yau_` | Sound at a time on the rig clock | 0.4.0 | 1 machine |
| [gfx](docs/gfx.md) | `ygfx_` | GL ES 3.0 stimuli: shapes, gratings, gabors, text, frame capture | 0.14.0 | 1 machine |
| [video](docs/video.md) | `yvid_` | Movies as stimuli, frame by predicted onset | 0.3.0 | 1 machine |

Each header is `include/ysp/<name>.h`; macros use the uppercase prefix
(`YRT_`). Status: **tested** is pure computation, checked by tests;
**1 machine** ran on one Windows 11 laptop only; **no device** never ran
on the hardware it drives.
[docs/status.md](docs/status.md) has the platforms, the dependencies, the
measurements and the hardware for each header.

## Quick start

1. Copy the header and the files in its row of
   [Files to copy](docs/status.md#files-to-copy) into a `ysp/` folder on
   your include path.
2. In exactly one `.c` or `.cpp` file, define the implementation macro
   before the include. Include the header normally everywhere else.

   ```c
   #define YSP_STAIR_IMPLEMENTATION
   #include "ysp/stair.h"

   yst_desc d = { 0 };                 /* zero it, then set fields */
   d.start = 0.5; d.n_up = 1; d.n_down = 3;
   d.steps[0] = 0.05; d.n_steps = 1; d.stop_reversals = 10;
   static yst_stair s;                 /* about 28 KB: not on the stack */
   if (!yst_open(&s, &d)) { fputs(yst_error(&s), stderr); return 1; }
   while (!yst_done(&s)) {
       double level = yst_next(&s);
       yst_update(&s, level, run_trial(level));   /* 1 = correct */
   }
   double threshold = yst_estimate(&s, YST_EST_REVERSALS);
   yst_close(&s);
   ```

3. Read the header's top comment: usage, platform setup, timing, build
   flags and the API.

## Build

To build the examples and tests in this repository:

```sh
cmake --preset msvc          # or msvc-full, mingw, linux, clang, sanitizers, nothreads-cxx
cmake --build --preset msvc
ctest --preset msvc
uv run --no-project python tools/vendor_pack.py --write     # lodepng, for -DYSP_BUILD_PACK=ON
uv run --no-project python tools/vendor_layout.py --write   # HarfBuzz, Skribidi, for -DYSP_BUILD_LAYOUT=ON
```

`msvc-full` adds SDL3 and ANGLE. [docs/build.md](docs/build.md) has the
presets, the liblsl fetch, a build without CMake and the CMake groups.

## Start here

1. Tutorial: [run your first trial](docs/response.md#tutorial-a-keyboard-trial-with-rt-from-the-flip-onset)
   (build with `msvc-full` first).
2. How-to: [choose a trigger path](docs/triggers.md), then the how-to
   guides in [response](docs/response.md), [device](docs/device.md),
   [net](docs/net.md) and [json](docs/json.md).
3. Reference: the top comment of each header, and
   [docs/status.md](docs/status.md).
4. Explanation: `docs/<name>.md`, the design decisions and measurements of
   each header.
5. Before you rely on a timing number, read
   [Verified on hardware](docs/status.md#verified-on-hardware).

## Conventions

The rules every header follows (dependencies, names, lifecycle verbs,
return values, errors, units, threads) are in
[docs/conventions.md](docs/conventions.md).

## More

- [docs/bindings.md](docs/bindings.md): Python wheels and MATLAB/Octave
  MEX functions.
- [docs/contributing.md](docs/contributing.md): repository layout, how to
  add a library, how to change a header.
- [examples/README.md](examples/README.md): the example index.

## License

MIT-0 (public domain equivalent). See [LICENSE](LICENSE) and the end of each
header.
