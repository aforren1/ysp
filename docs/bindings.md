# Bindings

This page is reference: the language bindings of the headers.

- **Python**: one distribution per library under
  [bindings/python/](../bindings/python/), each a dependency-free CPython
  extension on the Limited API (one abi3 wheel per platform for CPython
  3.8+). Each one compiles its header into the extension, plus `ysp/rt.h`
  where the header uses it. They share the `ysp` namespace (PEP 420, no `__init__.py`), so
  `pip install ysp-parallel ysp-serial` gives `import ysp.parallel` and
  `import ysp.serial`, and either installs alone. The adaptive headers have
  the same: `ysp-stair`, `ysp-quest`, `ysp-aep` and `ysp-trials` give
  `ysp.stair`, `ysp.quest`, `ysp.aep` and `ysp.trials`, with an `Async`
  class in quest and aep that runs inference on a C thread the interpreter
  never sees, and `ysp.trials` taking any of the three as a track.
  `ysp-color` gives `ysp.color` (calibration, conversions, gamut, a
  participant's luminance). `ysp-rdk` gives `ysp.rdk` (the dots of a trial
  again, bit for bit, from its seed and log); it needs NumPy at run time. `tests/compare/`
  runs them beside PsychoPy, questplus and AEPsych on one response stream.
  CI builds the wheels with cibuildwheel on Linux, Windows and macOS, except
  `ysp-parallel` on macOS, which its header does not support.
- **MATLAB / Octave**: [bindings/mex/](../bindings/mex/), one MEX function per
  library (`ysp_parallel`, `ysp_serial`, `ysp_stair`, `ysp_quest`,
  `ysp_aep`, `ysp_trials`, `ysp_color`, `ysp_response`) with ppdev-mex-style command dispatch, built and
  tested in MATLAB R2023a and Octave 10. `tests/compare/` runs `ysp_quest`
  beside mQUESTPlus and Palamedes' PAL_AMPM, and `ysp_stair` beside
  PAL_AMUD, on one response stream; the MEX README has the results.
  `tests/compare/color_ptb.m` runs `ysp_color` beside Psychtoolbox's
  colorimetric code (docs/color.md). `ysp_serial` has an async reader
  (`async_start`, `async_read`, `async_stop`): a C thread that stamps each
  line when it arrives. `tests/compare/serial_bench/` runs it beside MATLAB's
  `serialport` and Psychtoolbox's `IOPort` (docs/serial.md). The Python
  `ysp.serial` has the same reader since 0.5.0.2 (`async_start`,
  `async_read`, `async_wait`, `async_stop`, `async_stats`): a C thread that
  never takes the GIL.
- **Response collection**: `ysp-response` gives `ysp.response`, and the MEX
  `ysp_response` the same commands: a trial's response rules
  (`ysp/response.h`), fed with input events from a PsychoPy or
  Psychtoolbox key loop, from `ysp.box` or `ysp.device`, or from a log,
  giving the response, the RT from the stimulus onset and the timing
  quality of both. The input event (`ysp/input.h`'s `yin_event`) is one
  layout in the three bindings: in Python a structseq `Event` (`t`, `t_ns`,
  `device`, `control`, `code`, `mods`, `kind`, `type`, `flags`, `stamp`,
  `unc`, `x`, `y`, `value`, `aux`, `ticks`), and `ysp.response` takes any
  16-tuple in that order or any object with those attributes, so the other
  packages' events need no import; in MATLAB and Octave a struct array
  with the same field names. Feeding an event allocates nothing.
  [docs/response.md](response.md#bindings) has what is bound and what is
  not.

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
| `ysp.rdk` | `start(seed, t)`, `update(t)`, the `t0` and step rows of `replay()` and `trajectory()`, `Field.last` (`t`, and its exact `ns` beside it), `Field(lifetime=, frame_period=)`, `ages()`; exact to the ns below 2^23 s (about 97 days) |
| `ysp.response` and its MEX | `arm(t)`, `set_onset(t, residual=, uncertainty=)`, `update(now)`, `key(t, ...)`, `score(events, onset, arm=, end=)`, the desc's `min_rt`, `duration` (`math.inf`/`Inf`: no deadline) and `max_hold`, a source's `lo` and `hi`, the event's `t` and `unc` (from `unc_us`), and every time of the result and the entries; the exact ns beside each absolute time and the RT (`t_ns`, `rt_ns`, `t_onset_ns`, `t_response_ns`); exact to the ns below 2^23 s |

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
`driver` key of `list_ports`). PEP 440 accepts the fourth
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
build.
