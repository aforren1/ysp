# ysp.response Python binding

A CPython extension for the
[ysp/response.h](../../../include/ysp/response.h) single-header library:
the response of a trial from any input device. You define the rules (the
valid keys or buttons, the window, the minimum RT), give the stimulus
onset and the input events, and get the response, the RT from the onset,
and the timing quality of both. It uses the plain CPython C API and has
no dependencies. It is built against the **Limited API / stable ABI**
(`Py_LIMITED_API = 0x03080000`), so one `ysp/response.abi3.so` works on
CPython 3.8 and later.

The distribution is `ysp-response` and the module is `ysp.response`. `ysp`
is a [PEP 420](https://peps.python.org/pep-0420/) implicit namespace
package (no `__init__.py`), so `ysp-response` installs alone or next to
the other `ysp-*` distributions.

Every time is in seconds, as a `float`. The exact integer nanoseconds are
beside each absolute time (`t_ns`, `t_response_ns`, `t_onset_ns`) and
beside the RT (`rt_ns`). The collector has no clock: give it every time on
one clock. In PsychoPy, that is the clock of `win.flip()` and of the
keyboard's `tDown`. The events of `ysp.box` and `ysp.device` are on ysp's
clock (`QueryPerformanceCounter` on Windows, `CLOCK_MONOTONIC` on Linux
and macOS), so give the onset on that clock too.

## How to install

From this directory:

```sh
uv pip install .                    # build and install an abi3 wheel
```

In a development tree, setup.py finds `ysp/response.h` in the repository's
`include/` directory. An isolated build (an sdist or a cibuildwheel run)
sees only this directory. Copy the header and the `ysp/input.h` it
includes into `include/ysp/` next to `setup.py` first:

```sh
mkdir -p include/ysp && cp ../../../include/ysp/response.h ../../../include/ysp/input.h include/ysp/
uv build --sdist
```

## How to collect a keyboard response in PsychoPy

```python
import ysp.response as yr

rsp = yr.Collector(["f", "j"], duration=1.5, min_rt=0.1)   # once, before the trials

t_fix = win.flip()                     # the fixation
rsp.arm(t_fix)
...
t_stim = win.flip()                    # the frame that shows the stimulus
rsp.set_onset(t_stim, src="flip")
while True:
    t = win.flip()
    for k in kb.getKeys(waitRelease=False, clear=True):
        rsp.key(k.tDown, k.name)                          # the press
        if k.duration is not None:
            rsp.key(k.tDown + k.duration, k.name, down=False)
    if rsp.update(t) == yr.ENDED:
        break
r = rsp.finish()
thisExp.addData("response", r.response)
thisExp.addData("rt_s", r.rt)          # None when there is no response
for name, value in rsp.data_row().items():
    thisExp.addData(name, value)       # the header's data columns
```

`"f"` is the physical key at the F position of a US keyboard (the header's
CHOICES section lists the names). A press before the onset is EARLY, a
press before `min_rt` is an ANTICIPATION, and a press after `duration` is
LATE. Each is logged and counted (`entries()`, `r.n_early`,
`r.n_anticipations`), and none of them is a response.

## How to feed events from a device

`feed(event)` takes one `Event`, and `feed_many(events)` takes an iterable
of them, so one call per frame gives the collector every event a reader
queued. An `Event` has these fields, in this order (`ysp/input.h`'s
`yin_event`):

| Field | Meaning |
|---|---|
| `t`, `t_ns` | The time, in seconds and in exact ns. `t_ns` wins when it is an `int` |
| `device`, `control`, `code`, `mods` | The instance, the key or button or axis, the keycode, the modifiers |
| `kind`, `type`, `flags`, `stamp` | `KIND_*`, `PRESS`/`RELEASE`/`SAMPLE`/`PROXIMITY`, `REPEAT` and others, the stamp's `TIER_*` |
| `unc` | This event's uncertainty in seconds (`unc_us` / 1e6; 0.065535 means 65.5 ms or more) |
| `x`, `y`, `value`, `aux`, `ticks` | Position, axis value, yours, the device clock's low 32 bits |

`feed()` takes any 16-tuple in that order, so the `Event` of `ysp.box` and
`ysp.device` goes in as it is. It also takes any object with those
attributes; a missing attribute is 0. `yr.event(t, kind=..., ...)` makes
one, with a key name as `control` for a keyboard. Neither form allocates
for each event it reads (the tests check it with `tracemalloc`): an
object's instance dict is read first, so a field it lacks raises no
`AttributeError` inside the binding. A field that only a property or a
slot gives goes through `getattr`.

```python
box = yr.Collector([{"name": "left", "kind": "box", "control": 1},
                    {"name": "right", "kind": "box", "control": 2}],
                   duration=2.0,
                   sources=[{"kind": "box", "tier": yr.TIER_1, "lo": -0.0002, "hi": 0.0002}])
box.arm(t_fix)
box.set_onset(t_stim, src="flip", tier=yr.TIER_1)
box.feed_many(events)          # each frame
r = box.finish()               # r.stamp_tier, r.stamp_lo, r.stamp_hi from the source
```

A choice is a key name, or a dict with the header's choice fields: `key`,
`name`, `kind`, `match` (`"scancode"`, `"keycode"`), `cross` (`"rising"`,
`"falling"`, `"distance"`), `device` (0 = any), `control`, `code`,
`level`, `hysteresis`. An enum takes its name or its number. A field the
header does not have is refused.

## How to rescore a logged trial

`score(events, onset, arm=None, end=None)` runs one trial from a logged
event list and returns its `Result`. Events stamped before `arm` (default:
the onset) at the start of the list are fed before the arm, so a key held
from before the window stays held; the rest are fed after the arm and the
onset. `end` calls `update(end)` before `finish()`.

```python
for min_rt in (0.1, 0.15):
    r = yr.Collector(["f", "j"], duration=1.5, min_rt=min_rt).score(log, t_onset, arm=t_fix)
```

## Reference

`Collector(choices=None, *, mode=None, kinds=None, min_rt=0, duration=None,
persist=False, max_responses=0, allow_held_key=False,
wait_for_key_release=False, max_hold=0, sources=None, trace_cap=0,
trace_kinds=None)`: `yrsp_open()`. `duration` None, 0 or `math.inf` is no
deadline. `kinds` and `trace_kinds` take a bit mask, `"all"` (every kind
but SYNC) or a list of kinds. `trace_cap` allocates the trace array once,
here.

| Method | Does |
|---|---|
| `arm(t)` | Starts a window; keeps which keys are down |
| `set_onset(t, *, final=True, src="other", tier=0, residual=0, uncertainty=0, frame=None, t_ns=None)` | The onset RT runs from (`src`: `"flip"`, `"audio"`, `"plan"`, `"other"`). A plan (`final=False`) does not replace a final onset. Returns the status |
| `feed(event)`, `feed_many(events)` | Input events. Return the status |
| `key(t, key, down=True, *, device=0, repeat=False, stamp=0, code=0, t_ns=None)` | One keyboard event |
| `update(now)` | The deadline and `max_hold`. Returns `IDLE`, `OPEN` or `ENDED` |
| `finish()` | The `Result`. Call it again after a late release, onset or report |
| `entries()` | Every press of the window, as `Entry` structseqs |
| `trace()` | The traced inputs up to the last `finish()` |
| `set_sent(sent, total=None)` | Codes sent at presses: `(t, kind, device, control, choice, written)` each; `finish()` sets `R_SENT_MISMATCH` where they and the responses disagree |
| `score(events, onset, ...)` | One trial from a log |
| `data_row()` | The header's data columns (`DATA_COLS`) of the last `finish()`, `None` for an empty field |
| `close()` | Later calls raise `StateError`. A `Collector` is a context manager |

Properties: `error`, `plan_frame`, `closed`, `choices`. Module functions:
`event()`, `scancode(name)`, `key_name(scancode)`, `version()`.

`Result` has the header's result in seconds: `rt`, `rt_key_duration`,
`response` (the choice's name, the data's `response` column), `choice`
(its index), `entry`, `t_response`, the response's `kind`, `device`,
`control`, `code`, `stamp_tier`, `stamp_partial`, `stamp_lo`,
`stamp_hi`; the onset's `t_onset`, `onset_src`, `onset_tier`,
`onset_residual`, `onset_uncertainty`, `onset_frame`; `plan_residual`;
`flags` (`R_*`); and the counts. "None" in the header (`YRSP_NO_TIME`,
-1, a zero time) is `None`.

Errors: `Error`; `ArgumentError` (also a `ValueError`) for a bad argument
or a desc the header refuses, with the header's message; `StateError` for
a closed collector.

## What is not bound, and why

- The frame-loop and data helpers (`yrsp_update_frame()`,
  `yrsp_feed_screen()`, `yrsp_set_landing()`, `yrsp_onset_flip()`,
  `yrsp_onset_audio()`, `YRSP_DATA_COLS` with `yrsp_data_set()`): they
  read `ysp/screen.h`, `ysp/audio.h`, `ysp/timeline.h` and `ysp/trials.h`
  structs, which no Python package gives. `set_onset()` takes the same
  values, and `data_row()` gives the same columns.
- The response-trigger glue (`yrsp_rule_*`): it needs `ysp/device.h` in
  the same translation unit. `set_sent()` takes the fires instead.
- `yrsp_cursor` and the SDL and raw-mouse adapters: a PsychoPy or analysis
  user has no SDL events or raw mouse reports.
- `yrsp_params()`: it is for the experiment loader and the designer, in
  ns.
