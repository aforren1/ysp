# ysp.timeline Python binding

A CPython extension for the [ysp/timeline.h](../../../include/ysp/timeline.h)
single-header library: events, tracks and tweens on a frame clock. Use it
in a PsychoPy experiment that draws its own stimuli. Build a trial (events,
channels, keyframes, tweens with eases, sequences), then on each frame
evaluate it at the frame's predicted onset and read the channel values,
the fields and the events that landed on the frame, with their landing
records.

The extension uses the **Limited API / stable ABI**
(`Py_LIMITED_API = 0x03080000`), so one `ysp/timeline.abi3.so` works on
CPython 3.8 and later. It has no run-time dependencies.

The distribution is `ysp-timeline` and the module is `ysp.timeline`. `ysp`
is a [PEP 420](https://peps.python.org/pep-0420/) implicit namespace
package (no `__init__.py`), so `ysp-timeline` installs alone or next to the
other `ysp-*` distributions.

## How to install

From this directory:

```sh
uv pip install .                    # build and install an abi3 wheel
```

In a development tree, setup.py finds `ysp/timeline.h` in the repository's
`include/` directory. An isolated build (an sdist or a cibuildwheel run)
sees only this directory. Copy the header into `include/ysp/` next to
`setup.py` first:

```sh
mkdir -p include/ysp && cp ../../../include/ysp/timeline.h include/ysp/
uv build --sdist
```

## How to run a trial in a frame loop

```python
import ysp.timeline as ytl

TRIAL = 1                                          # a base: the trial clock
tl = ytl.Timeline(["fix", "grating", "contrast"])
dot = tl.field((0.0, 0.0))                         # a position tweens write

q = tl.seq(TRIAL)                                  # a cursor at trial time 0
q.on("fix").wait(0.5).off("fix").on("grating").trigger(12)
q.to("contrast", 0.5, duration=0.1, ease="cosine")         # the cursor stays
q.to(dot, (300, 0), duration=0.6, ease="power2.out")
q.wait(0.6).then("contrast", 0.0, duration=0.1, ease="cosine").off("grating")

period = win.monitorFramePeriod
next_onset = win.flip() + period
tl.anchor(TRIAL, next_onset)                       # trial time 0 is this frame
for frame in range(80):
    fired = tl.evaluate(next_onset, period, frame)
    fix, grating, contrast = tl.values             # float32, written in place
    x, y = dot.value
    ...                                            # set the stimuli, draw
    for e in fired:
        if e.kind == ytl.TRIGGER:
            port.write(e.code)                     # e.target: the trigger channel
        log(e.id, e.frame, e.residual_ns)          # where it landed, exact ns
    next_onset = win.flip() + period
```

Every time is seconds. The timeline reads no clock: give it the predicted
onset of the frame you are about to draw, on the clock your flips are
stamped with. Each landing record (`Event`) gives `time`, `onset` and
`residual` in seconds and `time_ns`, `onset_ns` and `residual_ns` as exact
integers.

To run the same trial again, anchor its base at time 0 again:
`tl.anchor(TRIAL, next_onset)`. Every event fires again and every tween
replays, bit for bit. To start a trial at a later point without the events
before it, use `tl.skip(TRIAL, next_onset, t)`. Between trials,
`tl.clear(TRIAL)` and build the next one.

## Reference

`Timeline(channels=0, *, initial=None, events=1024, keys=1024, fields=64,
tweens=256, lead=0.0)`: `channels` is a count or a list of names (at most
`MAX_CHANNELS`, 1024 in this build), `initial` a list or a dict by name.
`events`, `keys`, `fields` and `tweens` size the pools the header uses.
`lead` is the fraction of a period an event may land early: 0 is the
header's default, half a frame (the nearest frame); `"none"` never lands
early. A channel argument is an int or a name.

| Call | What it does |
|---|---|
| `evaluate(onset, period=0, index=None)` | The frame. Returns the fired `Event`s in RT order (the empty tuple when none). `index` defaults to the count of evaluates. |
| `values` | A float32 memoryview of the channel values (no copy). `numpy.frombuffer(tl.values, numpy.float32)` is a live view. |
| `value(ch)`, `channel(name)`, `channels` | One value, a name's index, the names. |
| `anchor(base, rt, bt=0)`, `pause`, `resume`, `stop`, `skip(base, rt, bt)` | Base clocks: run, freeze, seek without firing. |
| `rate(base, rt, num, den=1)`, `get_rate(base)` | An exact rational rate from `rt` on. |
| `base_time`, `rt_time`, `window`, `next_due`, `horizon`, `clamped`, `lead` | Queries; `None` where the header answers false. |
| `add(time, kind="mark", *, base, target, value, code, user)`, `add_many([dict])` | Raw events; `kind` is a name or an int (16 to 255 are yours). |
| `remove(id)`, `prune(base)`, `clear(base)` | Delete events (and tracks and target tweens for `clear`). |
| `events(base)`, `find(id)`, `peek(base, start, end)` | The record; the pending events of an RT window (for sound). |
| `set_keys(ch, base, keys)` | Keys are `(t, v)` or `(t, v, ease)`; an ease of `(x1, y1, x2, y2)` is CSS `cubic-bezier()`. |
| `set_track(ch, base, *, keys, samples, t0, rate, interp, period, repeats)` | A keyed or sampled track (samples: float32 buffer or numbers), with a repeat. |
| `tween(ch, to, *, base, duration, start, delay, from_, ease, cycles, yoyo, keep_velocity)` | A channel tween from "now", taking over from the channel's value. |
| `seq(base, now=False)` | A `Seq`. |
| `check_ops(base, ops)` | Check an op table as `Seq.run()` would, changing nothing. |
| `field(value=0, *, n, int=False)`, `fields(count, value)` | A `Field` (1 to 4 float32 or int32 components); `fields` makes adjacent ones for a stagger. A `Field` has `value`, `index`, `n`, `kill()` and `free()`. |
| `field_values` | A float32 memoryview of every field slot, 4 words each. |
| `kill(field)`, `kill_tween(id)`, `tween_count()` | GSAP's `killTweensOf()` and `kill()`. |
| `bind(obj, attr, source, component=-1)`, `unbind(obj=None)` | After each evaluate, `setattr(obj, attr, value)` from a channel or a field. |
| `close()`, `with Timeline(...) as tl:` | End its use; drops callbacks and bindings. |

`Seq` (every method returns the sequence, for chaining; the first failing
call raises with `index`, the call's number, and the calls before it stay):

| Method | GSAP |
|---|---|
| `on(ch)`, `off(ch)`, `set(ch, v)` | `.set()` of a channel: ONSET, OFFSET, SET events |
| `trigger(code, ch=0)`, `mark(code, user=0)` | `.call()`: a TRIGGER or a MARK |
| `wait(dt)`, `at(t)`; `t`, `prev_start`, `prev_end` (and `_ns`) | `"+=dt"`, a position, labels, `"<"` and `">"` |
| `to(target, v, **options)` | `.to(target, vars, "<")`: the cursor stays |
| `then(target, v, **options)` | `.to(target, vars)`: the cursor moves to its end |
| `from_(field, v, **options)`, `from_to(target, a, b, **options)` | `gsap.from()`, `gsap.fromTo()` |
| `set(field, v)` | `.set()` of a field; it leaves a `TARGET_SET` record whose `user` is the field's index |
| `keyframes(ch, values, offsets, ease=None)`, `on_n(chs, stagger)` | `keyframes: [...]`, `stagger` of onsets |
| `run(ops)` | An op table, all or none: `("on", ch)`, `("wait", 0.5)`, `("to", ch, v, {"duration": 0.1})` |
| `cancel()` | `tl.kill()` of what the sequence added |

The tween options are `duration`, `delay`, `ease`, `from_`, `cycles` (an
int or `"forever"`), `yoyo` and `keep_velocity`, and for a field also
`immediate`, `overwrite`, `on_start`, `on_complete`, `user` and `stagger`
(with a list of adjacent fields). An ease is a name (`"linear"`, `"step"`,
`"cosine"`, `"quad_in"`, `"quad_out"`, `"quad_in_out"`, `"cubic_in"`,
`"cubic_out"`, `"cubic_in_out"`, `"log"`), a GSAP name (`"power1.out"`,
`"power2.inOut"`, `"sine.inOut"`, `"none"`), an `EASE_*` value, or
`(x1, y1, x2, y2)`. A channel takes one tween at a time in a sequence; a
field takes any number, and the latest start drives it.

`on_start` and `on_complete` take a callable, called with no arguments
after the frame's fields are written, or `True`, which only adds the
`CALL_START` or `CALL_COMPLETE` event to the report. If a callback raises,
`evaluate()` raises it after the frame is evaluated; the record holds the
frame.

## Explanation: what is not bound, and why

- `ytl_sample()`: it does not validate its track, so a bad track reads out
  of bounds. `set_track()` on a spare `Timeline` and `evaluate()` give the
  same values with the checks.
- `ytl_op_params()` and `ytl_desc_params()`: they serve a player's loader
  and a designer, not an experiment.
- Negative times: the binding refuses them (docs/bindings.md), so
  `wait(-0.1)` and a negative `stagger` are refused. Use `at(q.t - 0.1)`.
- A Python object as a mark's payload: `user` is a 64-bit integer, as in
  C. Keep a dict from `code` or `user` to the object.

The Timeline keeps every `Field` it made until `field.free()` or
`close()`, so a field whose variable goes out of scope keeps its tweens.
`free()` kills its tweens and gives the slot back. `field_values` reads
every slot as float32; read an `int=True` field with `.value`.

## Tests

```sh
uv run --no-project --isolated --with . --with pytest python -m pytest -q tests
```

`tests/test_timeline.py` (25 tests) checks the manual's trial by calls and as an op
table, the landing records against the quantization rule, eases against
their formulas, a rewind and a seek that replay a trial bit for bit, two
timelines given the same calls, fields, callbacks, tracks, the times in
seconds, the refusals, and that a kept `values` or `field_values` view stays valid after `close()` or the Timeline's end. [docs/timeline.md](../../../docs/timeline.md#bindings)
has the per-frame cost.
