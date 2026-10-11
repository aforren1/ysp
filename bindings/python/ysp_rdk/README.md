# ysp.rdk Python binding

A CPython extension for the [ysp/rdk.h](../../../include/ysp/rdk.h) single-header
library: random dot kinematograms whose dots regenerate bit for bit from
a seed and the logged inputs of each update. Use it in an analysis to get
back the exact dots a participant saw, for reverse correlation, motion
energy or a check of the stimulus. The header's state is integers and its
random words are Philox4x32-10 of (seed, stream, dot, update, slot), so
the rig (C, MSVC) and the analysis (Python, any platform) compute the same
bits.

The extension uses the **Limited API / stable ABI**
(`Py_LIMITED_API = 0x03080000`), so one `ysp/rdk.abi3.so` works on
CPython 3.8 and later. It needs NumPy at run time: the outputs are NumPy
arrays (`numpy.frombuffer` over bytes that the C code fills, with no
Python work per dot).

The distribution is `ysp-rdk` and the module is `ysp.rdk`. `ysp` is a
[PEP 420](https://peps.python.org/pep-0420/) implicit namespace package (no
`__init__.py`), so `ysp-rdk` installs alone or next to the other `ysp-*`
distributions.

## How to install

From this directory:

```sh
uv pip install .                    # build and install an abi3 wheel
```

In a development tree, setup.py finds `ysp/rdk.h` in the repository's
`include/` directory.
An isolated build (an sdist or a cibuildwheel run) sees only this
directory. Copy the header into `include/ysp/` next to `setup.py` first:

```sh
mkdir -p include/ysp && cp ../../../include/ysp/rdk.h include/ysp/
uv build --sdist
```

## How to regenerate a logged trial

The rig logs what the header's manual lists (REPRODUCIBILITY): the
version, the desc, the seed, the time of `yrdk_start()`, and `rdk.last`
after each update (t, coherence, direction, speed). The C header logs its
times as integer ns; give them to the binding as seconds, `ns / 1e9`.
Open a field with the same desc as keyword arguments, then replay:

```python
import ysp.rdk as rdk

f = rdk.Field(algorithm="MN", w=10, count=100, coherence=0.256, speed=5)
steps = [(t_ns / 1e9, c, d, s) for t_ns, c, d, s in log]   # rdk.last of each update
xy, direction, signal = f.trajectory(seed, t0_ns / 1e9, steps)
# xy: (updates, n, 2) float32, units from the center, y down
# direction: (updates, n) float32, degrees, clockwise on the screen
# signal: (updates, n) uint8, 1 for a signal dot on that update
assert f.digest() == logged_digest              # the same trial
```

`trajectory()` runs the whole trial in one C loop. `replay(seed, t0,
steps)` does the same without the arrays. A step is any row of at least
four values: a tuple, a `Field.last` record (`Step`), or a row of a NumPy
structured array. A row of six values, (t, coherence, direction, speed,
set, ns), as `Field.last` gives it, uses its `ns`; its `t` must then be
`ns / 1e9`, or the row is refused.

## How to run a field step by step

```python
f = rdk.Field(w=10, count=200, coherence=0.5, direction=0, speed=7.5)
f.start(seed, t0)                    # seconds; every dot placed from the seed
for t in onsets:                     # seconds, the frames' predicted onsets
    f.coherence = 0.5                # read at each update, as float32
    f.update(t)
    xy = f.xy()                      # (n, 2) float32
    log.append(f.last)               # Step(t, coherence, direction, speed, set, ns)
snap = f.snapshot()                  # bytes: resume later with restore()
```

`directions()`, `ages()`, `signals()` and `events()` give the other outputs
of the shown set. `digest()` is the header's FNV-1a 64 of the state.

## Times

Every time is seconds, as a `float`: `start(seed, t)`, `update(t)`, the
`t0` and the step rows of `replay()` and `trajectory()`, `Field.last.t`,
and the desc's `lifetime` and `frame_period` (the header's `frame_ns`).
`ages()` is seconds too. The header counts integer ns, and a replay is the
same trial only if every ns is the same, so the binding converts each time
once, to the nearest ns of the exact product `t x 1e9`.

This conversion is exact: a time given as `ns / 1e9` comes back as `ns`
for every `ns` below 2^23 s (8,388,608 s, about 97 days of a clock's
count). Below that bound the double nearest `ns / 1e9` is within 0.47 ns
of it. A double holds every integer ns to 2^53 ns (about 104 days), but
its seconds lose that sooner. Rounding `t x 1e9` in floating point, as a
plain `round(t * 1e9)` does, misses by 1 ns on about 3 % of the values
between 2^51 and 2^52 ns (26 to 52 days); the binding uses `fma()` to
round the exact product instead. A time at or above the bound, a negative
time and NaN raise `ArgumentError`. So does an old ns integer passed as
seconds: 10^9 seconds is 31 years.

`Field.last` gives the seconds and the exact integer beside them:
`Step(t, coherence, direction, speed, set, ns)`, with `t == ns / 1e9`.
Log both. A row with `ns` replays from it at any magnitude.

## Reference

| Name | What it is |
|---|---|
| `Field(**desc)` | `yrdk_desc`'s fields as keyword arguments. An enum takes its name, in any case, or its value: `algorithm` "custom", "WN", "MN", "LL", "BM"; `aperture` "circle", "rect"; `signal` "same", "different", "least_recent"; `select` "exact", "bernoulli"; `noise` "direction", "position", "walk"; `edge` "wrap", "replot"; `clock` "onset", "frames". A float is rounded to float32, as a C float literal is. `lifetime` and `frame_period` are seconds (`frame_period` sets `frame_ns`). |
| `start(seed, t)` | `yrdk_start()`; `t` in seconds. |
| `update(t)` | `yrdk_update()`; `t` in seconds; returns the dots shown. |
| `replay(seed, t0, steps)` | `yrdk_replay()`; `t0` and each step's `t` in seconds. |
| `trajectory(seed, t0, steps)` | the replay, with the shown set's x, y, direction and signal after each step. |
| `xy()`, `directions()`, `ages()`, `signals()`, `events()` | `yrdk_write()` of the shown set, as NumPy arrays. |
| `digest()`, `snapshot()`, `restore(bytes)` | the header's calls of the same names. |
| `coherence`, `direction`, `speed` | the field's floats, read at each update. |
| `n`, `total`, `last`, `stats` | dots shown, dots in the field, the last update's inputs (`Step`, with `t` in seconds and its exact `ns`), counts since start. |
| `philox(seed, ctr)`, `angle(deg)`, `sincos(angle)` | the exact pieces of REPRODUCIBILITY. |
| `Error`, `ArgumentError`, `OrderError` | the header's codes; `ArgumentError` and `OrderError` are also `ValueError`. |

## Tests

```sh
uv build --wheel -o dist
uv run --no-project --with pytest --with numpy --with dist/ysp_rdk-0.2.0.1-cp38-abi3-<platform>.whl pytest tests
```

The tests check the 8 golden digests of `tests/adapt/rdk_test.c` and
`tests/compare/rdk_ref.py`, a trial logged by the C header
(`tests/c_trial.bin`, written by `tests/make_c_trial.c`) replayed into
equal arrays bit for bit, frozen noise across coherence levels,
snapshots, the outputs and the refusals. The seconds tests convert about
6,600 ns values from 0 to the bound, near 10^14 ns among them, and replay
a logged trial through the seconds API at 10^9 and 10^14 ns. CI runs them
on Linux, macOS arm64 and Windows.

## Version

0.2.0.1 is `ysp/rdk.h` v0.2.0 with the change to seconds (2026-10-11).
The first three numbers are the header's version; a fourth counts
changes to the binding alone and goes away when the header's version
changes. `rdk.__version__` gives the same string, and `rdk.version()`
gives the header's.

0.2.0.1 breaks the API of 0.2.0: `start()`, `update()`, `replay()` and
`trajectory()` take seconds where they took integer ns, `Field.last.t` is
seconds and `Field.last.ns` is new, and `frame_ns=` is `frame_period=` in
seconds.
