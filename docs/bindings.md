# Bindings

This page is reference: the language bindings of the headers.

- **Python**: one distribution per library under
  [bindings/python/](../bindings/python/), each a dependency-free CPython
  extension on the Limited API (one abi3 wheel per platform for CPython
  3.8+). Each one compiles its header into the extension, with the headers
  it includes. They share the `ysp` namespace (PEP 420, no `__init__.py`),
  so `pip install ysp-parallel ysp-serial` gives `import ysp.parallel` and
  `import ysp.serial`, and either installs alone. The adaptive headers have
  the same: `ysp-stair`, `ysp-quest`, `ysp-aep` and `ysp-trials` give
  `ysp.stair`, `ysp.quest`, `ysp.aep` and `ysp.trials`, with an `Async`
  class in quest and aep that runs inference on a C thread the interpreter
  never sees, and `ysp.trials` taking any of the three as a track.
  `ysp-color` gives `ysp.color` (calibration, conversions, gamut, a
  participant's luminance). `ysp-rdk` gives `ysp.rdk` (the dots of a trial
  again, bit for bit, from its seed and log); it needs NumPy at run time.
  The bindings of 2026-10-25 are `ysp-rt`, `ysp-box`, `ysp-device`,
  `ysp-rigfile`, `ysp-response`, `ysp-timeline`, `ysp-dataset` and
  `ysp-pack` (sections below), and `ysp-audio` is of 2026-10-26.
  `tests/compare/` runs the adaptive bindings
  beside PsychoPy, questplus and AEPsych on one response stream. CI builds
  the wheels with cibuildwheel on Linux, Windows and macOS, except
  `ysp-parallel` on macOS, which its header does not support.
- **MATLAB / Octave**: [bindings/mex/](../bindings/mex/), one MEX function
  per library (`ysp_parallel`, `ysp_serial`, `ysp_stair`, `ysp_quest`,
  `ysp_aep`, `ysp_trials`, `ysp_color`, `ysp_rdk`, `ysp_rt`, `ysp_box`,
  `ysp_device`, `ysp_rigfile`, `ysp_response`, `ysp_timeline`,
  `ysp_dataset`, `ysp_pack`, `ysp_audio`) with ppdev-mex-style command dispatch, built
  and tested in MATLAB R2023a and Octave 10. `tests/compare/` runs
  `ysp_quest` beside mQUESTPlus and Palamedes' PAL_AMPM, and `ysp_stair`
  beside PAL_AMUD, on one response stream; the MEX README has the results.
  `tests/compare/color_ptb.m` runs `ysp_color` beside Psychtoolbox's
  colorimetric code (docs/color.md). `ysp_serial` has an async reader
  (`async_start`, `async_read`, `async_stop`): a C thread that stamps each
  line when it arrives. `tests/compare/serial_bench/` runs it beside
  MATLAB's `serialport` and Psychtoolbox's `IOPort` (docs/serial.md). The
  Python `ysp.serial` has the same reader since 0.5.0.2 (`async_start`,
  `async_read`, `async_wait`, `async_stop`, `async_stats`): a C thread that
  never takes the GIL.

Not bound, by decision: `ysp/table.h` and `ysp/json.h` (Python and MATLAB
read CSV and JSON; `ysp.trials` already takes a table), `ysp/net.h`
(pylsl and liblsl-Matlab exist) and `ysp/outline.h` (an offline asset
tool). Not bound yet: `ysp/screen.h`, `ysp/gfx.h` and `ysp/video.h`, which
are planned as one module, `ysp-vis` (Python `ysp.screen`, `ysp.gfx` and
`ysp.video` from one extension; MEX `ysp_screen`, `ysp_gfx`, `ysp_video`).
It takes audio and the timeline from `ysp.audio` and `ysp.timeline`
through their tables (Sharing one implementation across modules, below).
Its host probe (frame loops in Python, MATLAB and Octave, DLL loading,
idle windows) is [av_probe.md](av_probe.md).

### The input event

`ysp.box`, `ysp.device` and `ysp.response` and their MEX functions give
`ysp/input.h`'s `yin_event` in one layout: in Python a structseq `Event`,
in MATLAB and Octave a 1 x N struct array, with the fields `t`, `t_ns`,
`device`, `control`, `code`, `mods`, `kind`, `type`, `flags`, `stamp`,
`unc`, `x`, `y`, `value`, `aux`, `ticks`. `t` and `unc` are seconds; `t_ns`
is the exact ns (an `int64` in MATLAB). `ysp.response` takes any 16-tuple
in that order or any object with those attributes, so the other packages'
events need no import. The constants have the C name without `YIN_`
(`KIND_BOX`, `PRESS`, `REPEAT`, `TIER_1`).

### Clock: ysp.rt, ysp_rt

The clock of every ysp stamp, deadline waits that release the GIL, the
clock correlation and the device clock fit, so a PsychoPy or Psychtoolbox
experiment can put its own stamps on the ysp clock and analysis code can
map a device's clock to it. [docs/rt.md](rt.md#bindings) has what is not
bound (worker, pump, ring, the scheduling ladder) and why.

### Boxes: ysp.box, ysp_box; ysp_rdk

A response box's or a line board's captured bytes to events and clock
pairs, without the device layer, and the bytes of queries and trigger
codes (docs/box.md, "Bindings"). The MEX function `ysp_rdk` has the surface
of `ysp.rdk` and replays a C-logged trial bit for bit (docs/rdk.md,
"Bindings").

### Devices: ysp.device, ysp.rigfile and their MEX

Two distributions: `ysp-device` ([ysp/device.h](../include/ysp/device.h):
boxes, trigger interfaces and boards, their events on the ysp_rt clock,
outputs and response rules) and `ysp-rigfile`
([ysp/rigfile.h](../include/ysp/rigfile.h): the rig profile).
`Profile.desc(role)` gives the keyword arguments of `ysp.device.Device`, so
a role opens with `Device(**profile.desc(role))` and neither extension
links the other; in MEX, `ysp_device('open', ysp_rigfile('desc', p,
role))`. The reader threads never take the GIL: events wait in a C ring
that `Device.poll()` and `Device.wait()` empty. Both test on simulated
boards (`ysp.device.Sim`), so CI needs no hardware.
[docs/device.md](device.md#bindings) lists what is bound and what is not.

### Responses: ysp.response, ysp_response

A trial's response rules (`ysp/response.h`), fed with input events from a
PsychoPy or Psychtoolbox key loop, from `ysp.box` or `ysp.device`, or from
a log, giving the response, the RT from the stimulus onset and the timing
quality of both. Feeding an event allocates nothing.
[docs/response.md](response.md#bindings) has what is bound and what is not.

### Timeline: ysp.timeline, ysp_timeline

For an experiment that draws its own stimuli and takes its timing from
`ysp/timeline.h`: a trial built as a sequence (events, channel tweens,
keyframes, op tables) and fields that target tweens write, evaluated at
each frame's predicted onset. The binding owns the fields' memory; the
frame loop reads the channel values and the fields as float32 views
(Python; a view keeps its memory alive) or matrices (MEX), with the landing
record of every event that fired. Both compile the header with
`YTL_MAX_CHANNELS` 1024 ([timeline.md](timeline.md#bindings)).

### Audio: ysp.audio, ysp_audio

Sound at a time on the ysp_rt clock (`ysp/audio.h`, miniaudio 0.11.25
compiled in): open a device by a part of its name (`devices()` lists the
names) or miniaudio's null device for tests; buffers from tones, noise,
clicks, host data (NumPy, `array.array`, a MATLAB matrix) or a WAV file;
play at a time; cancel, stop with a ramp, change the gain; and the record
of every sound: its onset (the device-clock fit's time, never an observed
time), the residual, the stream frame and the tier. `wait()` blocks
without the GIL. Streams fed from the host (`Stream.write()`) and WAV files
streamed (`Wav.feed()`) are voices like buffers; an input on the same
clock (`Capture`) gives its frames with their times. `ysp.audio` and
`ysp_audio` own the one audio implementation that `ysp-vis` shares
([audio.md](audio.md#bindings)).

### Data sets: ysp.dataset, ysp_dataset

A PsychoPy or Psychtoolbox session written as a ysp data set
(`session.json`, the trial, frame, event, clock and stream tables, the
zip), so the player's analysis reads it. A row is a `dict` or keywords (a
struct or name-value pairs in MEX); `None`, `[]` and `NaN` are an empty
cell. An exception through `with`, `abandon()` (`'abandon'`) and the
teardown of a data set never closed leave `"closed": false`
([dataset.md](dataset.md#bindings)).

### Packs: ysp.pack, ysp_pack

Open a pack (a file, mapped, or bytes in memory), list its entries, read
an entry or a range of it as a copy (bytes, or a `uint8` column), read the
manifest and the experiment definition, and verify the whole pack or one
entry. A failure raises the header's message (`ysp.pack.CorruptError`,
`ysp_pack:corrupt`, ...). The Python calls that hash or copy entry data
release the GIL. [docs/pack.md](pack.md#17-bindings-2026-10-25) has what is
not bound and why.

## Sharing one implementation across modules

This section is reference and explanation. `ysp/audio.h` and
`ysp/timeline.h` keep state that more than one module must reach: a movie
in `ysp-vis` plays its soundtrack on the `ysp.audio.Audio` of the
experiment, and its clock follows a `ysp.timeline.Timeline` built in
Python. A second copy of either implementation in `ysp-vis` would be a
second device and a second timeline. So each header is compiled into one
module, its owner (`ysp.audio` and `ysp_audio`; `ysp.timeline` and
`ysp_timeline`), and the owner exports a table that other modules call
through. This is NumPy's C-API pattern. nanobind was considered and not
used: a C++ dependency, a stable ABI only from Python 3.12, the same
version coupling, and no help for MEX.

The files are in [bindings/share/](../bindings/share/):
`ysp_share.h` (the table head, the checks, the MEX loader) and one file per
owner, `ysp_audio_api.h` and `ysp_timeline_api.h`. Each lists its
functions once, as an X-macro of the header's own signatures; that one list
makes the table's struct, fills it in the owner, and makes the consumer's
forwarders (the shims).

### The table

| Field | What it is |
|---|---|
| `head.abi` | `YSP_SHARE_ABI` (1). An entry is only appended; a change to an entry raises it |
| `head.size` | `sizeof` the owner's table; a consumer refuses a smaller one |
| `head.host` | `YSP_SHARE_HOST_PYTHON` or `YSP_SHARE_HOST_MEX`: what `acquire()` takes |
| `head.library`, `head.version` | `"audio"` and the header's version string as compiled (`YAU_VERSION_STRING`); a consumer refuses another major.minor and accepts another patch ([Versions of a table](#versions-of-a-table)) |
| `head.acquire(ref, err, cap)` | a host object to its C pointer, pinned (Lifetime); NULL with a message in `err` for an object of another type, of another module, or closed |
| `head.release(ptr)` | drops one pin |
| `max_channels`, `max_voices` / `max_bases` | the configuration values that change a layout: `YAU_MAX_CHANNELS`, `YAU_MAX_VOICES`; `YTL_MAX_CHANNELS` (1024 in both timeline owners), `YTL_MAX_BASES` |
| `sizeof_<struct>` | of every struct a consumer shares with the owner: `yau_audio`, `yau_buf`, `yau_play_desc`, `yau_onset`, `yau_caps`, `yau_stream`, `yau_stream_desc`, `yau_stream_info`, `yau_wav`, `yau_wav_desc`, `yau_reader`; `ytl_timeline`, `ytl_frame`, `ytl_event` |
| the functions | audio: 36, the output, stream and WAV functions (ysp/video.h calls 16 of them). Timeline: 24, the frame-time functions (ysp/video.h calls 9, a frame loop `ytl_evaluate` and `ytl_values`) |

Not in the tables: open and close (the owner's), the arena and the event
storage (the owner's memory: a consumer gives a stream or WAV ring its own
memory), building a timeline, capture, the detectors and the WAV writer.

`ysp_audio_api_check()` and `ysp_timeline_api_check()` run the checks: the
ABI, the host, the size, the major and minor numbers of the version, each
configuration value and each struct size. A configuration macro such as
`YTL_MAX_CHANNELS` changes a struct without changing the version, so both
are checked. A refusal names what differs (`ysp_timeline:
sizeof(ytl_timeline) is ... in the owner and ... here: the two were built
with other settings`; `ysp_audio: the owner compiled ysp/audio.h 0.8.2,
this module was built against 0.7.2: the major.minor versions must match
(a patch may differ); install matching versions`).

### Versions of a table

A consumer accepts an owner whose header has the same major and minor
version and another patch number: `ysp-vis` built against ysp/audio.h
0.7.2 loads `ysp.audio` 0.7.3. This relies on one rule for every header
that a table carries (CLAUDE.md, "Changing a header"):

- A change to the layout of a struct, or to a function that a table
  carries (its signature or what it does with its arguments), bumps at
  least the minor version.
- A patch changes neither: a fix inside a function, a comment, a test.

The struct sizes and configuration values are checked too, so a layout
change that broke the rule and changed a size is still refused. A layout
change that keeps every size (two fields swapped) is not found: the rule
alone covers it. Before 2026-10-27 the check asked for the exact version
string, so every patch of an owner needed a new build of each consumer.

### How a host finds the table

- **Python**: the owner module has an attribute `_C_API`, a capsule named
  `ysp.audio._C_API` (`ysp.timeline._C_API`).
  `ysp_audio_api_import_py()` (with `YSP_SHARE_PYTHON`) calls
  `PyCapsule_Import()`, runs the checks, and raises `ImportError` with the
  reason.
- **MATLAB and Octave**: the owner MEX file exports
  `ysp_audio_api_table()` (`ysp_timeline_api_table()`) besides
  `mexFunction`. `ysp_audio_api_import_mex()` (with `YSP_SHARE_MEX`) finds
  its own file's path (`GetModuleHandleEx`, `dladdr`), loads
  `ysp_audio.<its own extension>` from that directory (`LoadLibraryExW`,
  `dlopen`), and calls the export. For a path already loaded, Windows and
  `dlopen()` return that module, so the consumer reaches the handle table
  of the owner's own commands: verified in MATLAB R2023a and Octave 10.1 on
  Windows 11 and in Octave 6.4 on Ubuntu 22.04 (WSL2), where a handle
  opened by `ysp_audio('open')` is pinned and played through the consumer.
  An owner on the MATLAB path in another directory than the consumer is
  another module; its handles are refused. MinGW exports every symbol only
  when none is marked for export, so the owners mark `mexFunction` too
  (`YSP_SHARE_MEX_ENTRY`); MATLAB on Linux and macOS exports `mexFunction`
  alone unless `build.m` gives the linker the list (not run here: CI's
  MATLAB job does).

### Lifetime

`acquire()` pins the object; the pointer stays valid until the consumer
calls `release()` with it.

- Python: `acquire()` takes a strong reference to the owner's object, so
  the object outlives its last Python name. While pinned, `close()` raises
  `BusyError`. `release()` drops the reference; the last one frees the
  object.
- MEX: a handle is a counter in the owner's table, so the consumer keeps
  the pointer, not the handle. While pinned, `'close'` raises
  `ysp_<name>:busy`. `clear ysp_audio` (or `clear mex`, or the exit) runs
  the owner's cleanup: a pinned device or timeline stays open, out of the
  handle table, and the last `release()` closes and frees it. The consumer
  keeps its `LoadLibrary`/`dlopen` reference until it has released every
  pin (`ysp_share_mex_unload()` in its `mexAtExit` hook), so the owner's
  code stays mapped. The handles of each owner start at a number of its
  own (`0x4155 * 2^32 + 1` for audio, `0x544C * 2^32 + 1` for the
  timeline), so `acquire()` does not take a handle of another module.

### Threads

The table's functions keep the header's thread rules: a consumer calls
them on the thread that uses the handle, and the stream and WAV producer
calls from one other thread. `acquire()` and `release()` run with the GIL
held (Python) or on the MATLAB thread. In Python, the timeline table's
`ytl_evaluate` is the binding's evaluate: tween callbacks run, it takes the
GIL itself, `Timeline.bind()` targets are written, and a callback that
raised returns `YSP_SHARE_ERR_HOST` (-1000) with the exception set for a
caller that holds the GIL. ysp.rt's state (the virtual clock) is per
module: each module compiles `ysp/rt.h`, and the real clock reads the same
counter in all of them.

### The consumer's build

One translation unit defines `YSP_AUDIO_API_SHIMS` and
`YSP_TIMELINE_API_SHIMS` after the headers' declarations, without their
implementations (and defines `YSP_TIMELINE_IMPLEMENTATION_GUARD`, so that
`ysp/video.h`'s implementation does not pull timeline.h's in). Each shim is
`r name(params) { return api->name(args); }`; a signature that drifts from
the header's does not compile, and a function the tables do not forward
does not link. [tests/compile/share_shims.c](../tests/compile/share_shims.c)
links `ysp/video.h`'s and `ysp/screen.h`'s implementations against the
shims, as C11 and C++17 with warnings as errors; CMake builds it
(`compile_share_shims_c`, `_cxx`) wherever ysp/video.h builds. It found one
header limit: `ysp/video.h` 0.5.0 called `YAU__NOW()` and
`YAU__SLEEP_UNTIL()` in `yvid__snd_close`, private macros of audio.h's
implementation, and the check defined them itself. ysp/video.h 0.5.1 uses
its own clock and sleep there, and the check defines nothing. The test
consumers are
[bindings/python/share_test/](../bindings/python/share_test/) (23 tests) and
[bindings/mex/tests/share_consumer.c](../bindings/mex/tests/share_consumer.c)
([test_mex_share.m](../bindings/mex/test_mex_share.m), 26 checks).

### Cost

The i7-1360P laptop of docs/audio.md, Windows 11, on AC, the measurement
lock held, 21 runs of 3 x 2,000,000 calls of `ytl_value(tl, 0 or 1)` from
the test consumer, medians of the best of 3:

| Host | Through the shim | Through the table pointer | The read it replaces |
|---|---|---|---|
| Python 3.14 (MSVC 19.44 /O2) | 1.24 ns | 1.03 ns | 0.51 ns |
| MATLAB R2023a (MSVC 19.44) | 1.03 ns | 1.25 ns | 0.52 ns |
| Octave 10.1 (MinGW-w64 gcc) | 1.27 ns | 1.06 ns | 0.53 ns |

So a forwarded call costs about 0.5 to 0.8 ns over the inline read: one
indirect call. The shim and the table pointer do not differ beyond the
run-to-run spread; the shim is in the consumer's own unit, where the
compiler may inline it into its caller.

## Units

Every time in a binding is seconds, as a `double`: the arguments, the
returned values, the constants and the clock read (`ysp.serial.now()`,
`ysp.parallel.now()`, `ysp_serial('now')`, `ysp_parallel('now')`). No binding takes or returns
milliseconds or microseconds, and a name does not carry the unit. The
binding rounds each time to the nearest nanosecond for the C call and
refuses a negative or NaN time before the call. "Wait forever" is
`math.inf` in Python and `Inf` in MATLAB and Octave; 0 polls where the C
call can poll. [Conventions](conventions.md) lists the exceptions.

| Binding | Times in seconds |
|---|---|
| `ysp.serial` | `now()`, `read(n, timeout)`, `Port(write_timeout=, rt_runtime=, rt_deadline=, rt_period=)`, `pulse(on, off, width)`, `pulse_async(on, off, width)`, `send_break(duration)`, `async_wait(timeout)`, the `async_read()` stamps, `write_timeout`, `sched`, `TIMEOUT_INFINITE` (`math.inf`), `DEFAULT_WRITE_TIMEOUT`, `DEFAULT_RT_*` |
| `ysp.parallel` | `now()`, `Port(rt_runtime=, rt_deadline=, rt_period=)`, `pulse(value, width)`, `pulse_async(value, width)`, `sched`, `DEFAULT_RT_*` |
| `ysp_serial` (MEX) | `'now'`, the `'read'` timeout, the `'pulse'` and `'pulseasync'` widths, the `'break'` duration, the `open` struct (`write_timeout`, `min_hold`, `rt_runtime`, `rt_deadline`, `rt_period`) and the `'async_read'` stamps |
| `ysp_parallel` (MEX) | `'now'`, `'pulse'` and `'pulseasync'` widths, the RT struct and `'sched'` (`runtime`, `deadline`, `period`) |
| `ysp.quest`, `ysp.aep` and their MEX | `Async.wait(seq, timeout_s)` |
| `ysp.trials` and its MEX | the jitter bounds; a drawn jitter gives `s`, and its exact `ns` beside it |
| `ysp.box` | the read time `t` of `decode()`, `Event.t` (with the exact `t_ns` beside it), `Event.unc`, the `width` of `encode()`, `fixed_pulse()`, `tick()`, `SLOW_WRITE`, `UNC_MAX` |
| `ysp_box` (MEX) | the `'decode'` read time, the events' `t` (with `t_ns`, int64) and `unc`, the `'encode'` width, `'family'` (`fixed_pulse`, `tick`), `'constants'` (`SLOW_WRITE`, `UNC_MAX`) |
| `ysp_rdk` (MEX) | as `ysp.rdk`: `'start'`, `'update'`, the `t0` and steps of `'replay'` and `'trajectory'`, `'get'` (`last.t`, with `last.ns` beside it), the desc's `lifetime` and `frame_period`, `'ages'` |
| `ysp.pack` and its MEX | none: sizes and offsets are bytes |
| `ysp.rt` and `ysp_rt` (MEX) | `now()` (and `now_ns()`, exact), `real_now()`, `sleep_until(t, spin)`, `sleep(duration, spin)`, `spin_until(t)` and their lateness, `DEFAULT_SPIN`, `VIRTUAL_T0`, `virtual_begin(t0)`, `clock_info()` resolution, `correlate()`'s `t`, `width`, `timeout` (and `other` when the clock returns seconds; `t_ns` exact), `Fit(tick=, bucket=, slope_span=, restart=, restart_hold=, max_width=)`, `Fit.add(ticks, t, width)`, `Fit.map()` (and `map_ns()`, exact), `Fit.info()` times; exact to the ns below 2^23 s (about 97 days) |
| `ysp.dataset` and its MEX | every `time` column of trials.csv and of a stream, and every `_s` column of frames.csv, events.csv and clock.csv; rounded to the nearest ns, so ns -> s -> ns holds below 2^23 s. A data value may be negative (a residual, an SOA), and `NaN` is missing, not refused |
| `ysp.rdk` | `start(seed, t)`, `update(t)`, the `t0` and step rows of `replay()` and `trajectory()`, `Field.last` (`t`, and its exact `ns` beside it), `Field(lifetime=, frame_period=)`, `ages()`; exact to the ns below 2^23 s (about 97 days) |
| `ysp.device` | `now()`, the events' `t` and `unc` (`t_ns` exact beside `t`), `Device(pulse=, min_hold=, probe=, retry=, silent=, max_width=)` (`probe` and `silent` take `math.inf` for none), `wait_running(timeout)`, `wait(timeout)`, `pulse(code, width)`, `last_out()`, `stats()` (`last_rx` and the fit's times), `source()` (`lo`, `hi`), `Rule(width=)`, `Rule.arm(start, until)` and `window()` (`math.inf`: no end), `Rule.info()` and `fires()`, `Sim(frame=, tail=)`, `Sim.edge(t)`, `press(t, duration)`, `unplug(start, stop)`, the `writes()` and `lines()` stamps |
| `ysp.rigfile` | a role's `pulse`, `ftdi_latency`, `latency` (`p5`, `median`, `p95`, `max`) and `bounds` (`lo`, `hi`; `lo_ns` and `hi_ns` exact), `display` (`onset_offset`, `onset_offset_ns` exact), `source()` (`lo`, `hi`), `desc()['pulse']`; lengths in mm |
| `ysp_device`, `ysp_rigfile` (MEX) | the same, with `Inf` for `math.inf`; an event's `t_ns` is `int64` |
| `ysp.response` and its MEX | `arm(t)`, `set_onset(t, residual=, uncertainty=)`, `update(now)`, `key(t, ...)`, `score(events, onset, arm=, end=)`, the desc's `min_rt`, `duration` (`math.inf`/`Inf`: no deadline) and `max_hold`, a source's `lo` and `hi`, the event's `t` and `unc` (from `unc_us`), and every time of the result and the entries; the exact ns beside each absolute time and the RT (`t_ns`, `rt_ns`, `t_onset_ns`, `t_response_ns`); exact to the ns below 2^23 s |
| `ysp.audio` and `ysp_audio` (MEX) | `now()` (`now_ns()` exact), every time argument (`play_at(t)`, `play(at=)`, `stop(at=, ramp=)`, `gain(at=, ramp=)`, `wait(timeout)`, the synthesis `duration`, `ramp`, `period`, `onset_offset`, a ring's and a preroll's length, `frame_at(t)`, `sample_at(t)`, a capture's `frame_at(t)` and `offset`) and result (`lead` with `lead_ns`, `caps()` and `info()` times, `time_of()` as `(s, ns)`); a record gives `target`, `onset`, `residual` and `rendered_at` in seconds and their `_ns` exactly. Frame and sample counts stay integers |
| `ysp.timeline` and `ysp_timeline` (MEX) | every time argument (`evaluate(onset, period)`, `anchor`, `skip`, `rate`, `wait`, `at`, the tween options `duration`, `delay`, `start`, `stagger`, key and event times, track `t0` and `period`, `peek`'s window) and result (`base_time`, `rt_time`, `window`, `next_due`, `horizon`, a sequence's cursor); a landing record gives `time`, `onset` and `residual` in seconds and `time_ns`, `onset_ns` and `residual_ns` exactly |

## Versions

A Python distribution's version is its header's version. A change to the
binding alone adds a fourth number, which counts such changes and goes away
when the header's version moves: `ysp-serial` 0.5.0.1 is `ysp/serial.h`
v0.5.0 with the change to seconds, and `ysp-parallel` 0.4.0.1 is
`ysp/parallel.h` v0.4.0 with the same change, and so was `ysp-rdk`
0.2.0.1 for `ysp/rdk.h` v0.2.0 (now 0.3.0, the header's v0.3.0). `ysp-serial` 0.5.0.2 is the second
binding-only change to `ysp/serial.h` v0.5.0: the async reader. `ysp-serial`
0.5.1 is `ysp/serial.h` v0.5.1 (a comment change) without a fourth number,
and has the API of 0.5.0.2; 0.6.0 is `ysp/serial.h` v0.6.0 (`pulse_async`
without I/O, the width from the onset write's start, `min_hold`), and
0.6.1 is `ysp/serial.h` v0.6.1 (`Port.driver`, `Port.rts_via_dtr`, the
`driver` key of `list_ports`); 0.6.2 is `ysp/serial.h` v0.6.2 (a pulse
width near `INT64_MAX` saturates). `ysp-parallel` is 0.4.1, `ysp/parallel.h`
v0.4.1, without the fourth number. PEP 440 accepts the fourth
number as a release segment, and 0.5.0.1 sorts after 0.5.0 and before
0.5.1. A post-release (0.5.0.post1) was not used: it means "no code
changed", and this change breaks the API. Each package's tests check that
`__version__` equals the version in `pyproject.toml`.

A MEX function that reports a version has its own number, apart from
its header's. `ysp_serial('version')` returns 2.2.0 (2.0.0, 2026-10-11:
seconds and the async reader; v1 took milliseconds and microseconds and had
`now_us`; 2.1.0, 2026-10-12: the `min_hold` setting; 2.2.0, 2026-10-12:
`driver` and `rts_via_dtr` in `'info'`, `driver` in `'list'` and
`'find'`), and its second output is the version of `ysp/serial.h` in the
build. The MEX functions of 2026-10-25 (`ysp_rt`, `ysp_box`, `ysp_rdk`,
`ysp_device`, `ysp_rigfile`, `ysp_response`, `ysp_timeline`,
`ysp_dataset`, `ysp_pack`) do the same: `'version'` returns 1.0.0, and
its second output is the version of the header. `ysp_pack` is 1.1.0 since
2026-10-26: `'verify'` of one entry hashes it under `'none'` too
(`ypak_verify_entry()`, ysp/pack.h 0.4.0). `ysp_audio` is 1.0.1 since
2026-10-27: `Inf` as the timeout of `'wait'` is `INT64_MAX` ns (ysp/audio.h
0.7.2; 1.0.0 passed 2^62 ns). `ysp_device` is 1.0.1 since 2026-10-27:
`Inf` as the timeout of `'wait_running'` is `INT64_MAX` ns (ysp/device.h
0.5.1; 1.0.0 passed `INT64_MAX / 2`). `ysp_timeline` is 1.1.0 since 2026-10-26: the shared table,
`'pins'`, `ysp_timeline:busy`, and handles from `0x544C * 2^32 + 1`. The
Python `ysp-timeline` of the same change is 0.8.1.1 (ysp/timeline.h
v0.8.1), and `ysp-audio` is 0.7.2, the header's. The adaptive MEX
functions and `ysp_color` have no number of their own: `'version'`
returns the header's.
