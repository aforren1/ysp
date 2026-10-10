# Contributing

This page is for people who change ysp: where files are, how to add a
library, and what to update when you change a header.

## Layout

```
include/ysp/rt.h          the shared base: clock, waits, scheduling, worker, pump, event ring, trace macros
include/ysp/<name>.h      one library per header; the header is the documentation
examples/<lib>/<name>.c   runnable demos, one or more per library, built as <lib>_<name>;
                          examples/README.md is the index
tests/compile/            per-header compile checks (C11, C++17, no-threads, async)
tests/adapt/              self-checking tests for ysp/rt.h's pump, the adaptive-method
                          headers, ysp/trials.h, ysp/table.h and ysp/timeline.h, run by ctest
tests/compare/            side-by-side runs against PsychoPy, questplus, AEPsych,
                          mQUESTPlus and Palamedes; by hand, not CI
tests/fuzz/               libFuzzer targets for ysp/table.h, ysp/trials.h, ysp/json.h and ysp/pack.h;
                          by hand, not CI
tests/loopback/           opt-in hardware tests (-DYSP_BUILD_LOOPBACK=ON), run by hand
docs/status.md            per-header status, hardware verified, files to copy
docs/build.md             presets, fetches, a build without CMake, the CMake groups
docs/conventions.md       names, handles, errors, threads, units, documentation
docs/contributing.md      this page: layout, adding a library, changing a header
docs/bindings.md          the Python and MATLAB/Octave bindings
docs/serial.md            design notes for ysp/serial.h
docs/adapt.md             design notes and verification for ysp/stair.h, ysp/quest.h, ysp/aep.h
docs/adapt_comparison.md  the adaptive methods compared on published test problems
docs/trials.md            design notes and verification for ysp/trials.h
docs/table.md             design notes, number-parser verification and costs for ysp/table.h
docs/timeline.md          design notes and measured costs for ysp/timeline.h
docs/input.md             why the input event is its own header, and its checks
docs/response.md          tutorial, how-to guides and decisions for ysp/response.h
docs/box.md               the box protocols: XID checked against pyxid2, the ysp line protocol, checks
docs/device.md            the device layer: decisions, simulated and pty-pair results, what needs hardware
docs/net.md               ysp/net.h: how-to guides, the LabStreamer, results against the real liblsl
docs/json.md              ysp/json.h: how-to guides, the decisions and the checks
docs/pack.md              the pack format, ysp/pack.h, the pack tool (ypak), costs
docs/triggers.md          how-to: choose a trigger path (device.h, net.h, parallel.h, serial.h)
docs/rig_spec.md          the plan: display, audio, video, pack, player, designer
docs/devices_spec.md      the plan for specialized devices: decomposition, headers, order of work
firmware/ysp_line/        the photodiode board (Teensy 4.x, Raspberry Pi Pico) on the ysp line protocol
docs/screen.md            design notes and the swap-path measurements for ysp/screen.h
docs/audio.md             design notes and the clock and loopback measurements for ysp/audio.h
docs/gfx.md               design notes, pixel errors per renderer and frame costs for ysp/gfx.h
docs/color.md             design notes, references, Psychtoolbox comparison and costs for ysp/color.h
docs/rdk.md               design notes, statistics, mutations and costs for ysp/rdk.h
docs/outline.md           design notes, references, mutations and build costs for ysp/outline.h
docs/video.md             design notes, decode and upload costs for ysp/video.h
docs/rt.md                design notes and measured costs for ysp/rt.h's event ring, correlation, macros,
                          the device clock fit and the per-user folders
pack/ysp/layout.h, layout.c  text layout for the pack tool and the player (Skribidi); not a header;
                          -DYSP_BUILD_LAYOUT=ON builds it, tests/layout/ and examples/layout/gfx_layout.c
pack/ysp/pack_tool.h, pack_tool.c, ypak.c  the pack tool library and its CLI; -DYSP_BUILD_PACK=ON
                          builds it (glyph runs need -DYSP_BUILD_LAYOUT=ON too; docs/pack.md 5.1)
third_party/              Skribidi, HarfBuzz, SheenBidi, libunibreak, budouxc for pack/layout, pinned
                          and verified byte for byte by tools/vendor_layout.py; lodepng for the pack
                          tool (tools/vendor_pack.py); liblsl for net.h's tests (tools/vendor_lsl.py)
docs/*_probe.md           probe results: beam race, ImGui, layout, text, the X11 display
docs/layout.md            the layout library: pins, the patch, checks against Chromium, costs
bindings/python/<name>/   one pip package per library
bindings/mex/             ysp_parallel.c, ysp_serial.c, ysp_stair.c, ysp_quest.c, ysp_aep.c,
                          ysp_trials.c, ysp_color.c, ysp_mex_util.h, build.m, test_mex.m,
                          test_mex_color.m, example_<name>.m
CMakeLists.txt            builds examples, compile checks and tests; registers libraries
```

## Add a library

1. Write `ysp/<name>.h` with the top-comment manual, the API reference on
   each declaration, and the MIT-0 block, following [conventions.md](conventions.md).
   Build on `ysp/rt.h` for the clock, the waits, the thread scheduling and
   the deadline worker. Do not write those again.
2. Add `tests/compile/<name>.c` and `.cpp` (copy an existing pair and
   change the name).
3. Register it in `CMakeLists.txt`: append to `YSP_LIBS`, set
   `YSP_PREFIX_<name>`, `YSP_GROUP_<name>`, `YSP_PLATFORMS_<name>`, and
   `YSP_THREADS_<name>`,
   and `YSP_ASYNC_<name>` if it has an opt-in async layer. Compile checks,
   examples, a `tests/adapt/<name>_test.c` and a
   `tests/loopback/<name>_loopback.c` if there are ones are found by name
   from there.
4. Add at least one `examples/<name>/<example>.c` (built as
   `<name>_<example>`) and its line in `examples/README.md`. It must exit with a documented
   code when no hardware is present; add that expectation to the "Run
   examples" step in `.github/workflows/ci.yml`.
5. Add a row to the Headers table in [README.md](../README.md#headers)
   and an entry to [status.md](status.md).
6. Bindings are optional: `bindings/python/<name>/` (add it to the CI wheel
   matrix) and `bindings/mex/ysp_<name>.c` (add a platform gate to
   `build.m` if the header does not support every OS).

CI builds every registered header on Windows, Linux, and macOS, so step 3 is
what puts a header under test.

## Change a header

Bump the version in the header's first line, its `*_VERSION` macro and
its changelog; update `docs/<name>.md`, the header's row in the README
Headers table (version and status word) and its entry in
[status.md](status.md), the tests, the compile checks, and its mutation
list (`uv run tests/mutate/mutate.py tests/mutate/<name>.toml --check`,
then run the mutants you add). Parsers of untrusted input get a fuzz
target. CI is `.github/workflows/ci.yml`.
