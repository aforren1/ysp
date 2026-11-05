# ysp.rt Python binding

A CPython extension for the [ysp/rt.h](../../../include/ysp/rt.h)
single-header library: the clock that every ysp stamp is on, the deadline
waits, the clock correlation, the device clock fit, the virtual clock and the
per-user folders. It uses the plain CPython C API and has no dependencies.
It is built against the **Limited API / stable ABI**
(`Py_LIMITED_API = 0x030A0000`), so one `ysp/rt.abi3.so` works on CPython
3.10 and later.

The distribution is `ysp-rt` and the module is `ysp.rt`. `ysp` is a
[PEP 420](https://peps.python.org/pep-0420/) implicit namespace package (no
`__init__.py`), so `ysp-rt` installs alone or next to the other `ysp-*`
distributions. Every time is seconds as a `float`.

## How to install

From this directory:

```sh
uv pip install .                    # build and install an abi3 wheel
# or, in place for development:
python setup.py build_ext --inplace
```

In a development tree, setup.py finds `ysp/rt.h` in the repository's
`include/` directory. An isolated build (an sdist or a cibuildwheel run) sees
only this directory. Copy the header into `include/ysp/` next to `setup.py`
first:

```sh
mkdir -p include/ysp && cp ../../../include/ysp/rt.h include/ysp/
uv build --sdist
```

## How to put your own stamps on the ysp clock

`ysp.rt.now()` reads the clock of `ysp.serial.now()`, `ysp.parallel.now()`
and every time in a ysp data file. Each ysp extension compiles its own copy
of `ysp/rt.h`, but all copies read one counter (QueryPerformanceCounter on
Windows, `CLOCK_MONOTONIC` on Linux and macOS); the tests check that a read
of `ysp.serial.now()` lies between two reads of `ysp.rt.now()`.

```python
import ysp.rt as rt

t_cue = rt.now()                  # a stamp you can subtract from a ysp stamp
t_ns = rt.now_ns()                # the same, as an exact int of ns
print(rt.describe())              # log this line beside the timing data
```

## How to wait for a deadline

```python
t = rt.now()
for frame in range(100):
    t += 0.010                         # a running deadline: lateness does not add up
    late = rt.sleep_until(t)           # seconds late; the GIL is released
late = rt.sleep_until(t, spin=0)       # the OS wait only, no spin
late = rt.sleep(0.002)                 # relative
late = rt.spin_until(rt.now() + 0.001) # spin the whole time (a full core)
```

`sleep_until()` gives the OS the wait up to `spin` seconds before the
deadline and spins the rest. `DEFAULT_SPIN` is 1.2 ms on Windows and 0.2 ms
elsewhere. A wait longer than 200 ms goes to the OS in 100 ms slices, and
the signal handlers run between them, so Ctrl+C ends it. A thread that ends
after it waited can call `thread_cleanup()` to release its Windows timer.

## How to map another clock

`correlate(read)` reads another clock between two reads of this one and
keeps the tightest bracket. `read()` returns an `int` (a counter in the
clock's own units) or a `float` (seconds):

```python
import time
c = rt.correlate(time.perf_counter)    # Corr(t, other, width, tries, t_ns)
offset = c.t - c.other                 # a perf_counter stamp s is at s + offset
c = rt.correlate(read_box_ms, edge=True)   # a coarse clock: time a change
```

The true ysp time of `c.other` is within `c.width / 2` of `c.t`. The width
includes the cost of the Python call: about 1 us for `time.perf_counter`.

## How to fit a device's clock

```python
fit = rt.Fit("late", tick=1e-6, bits=32)   # a board's micros(), 32 bits
fit.add(device_us, rt.now())               # per sync report: its counter, when it came
t = fit.map(event_us)                      # an event's time on the ysp clock
ts = fit.map(list_of_event_us)             # many at once; map_ns() for exact ns
print(fit.info())                          # ppm, spread, points, epoch, ...
```

Modes: `"late"` (report times are late, never early), `"bracket"` (the
device's time was read between `t - width` and `t`: `fit.add(ticks, t1,
t1 - t0)`), `"unbiased"` (stamps scatter both ways). The keyword arguments
`bucket`, `slope_span`, `restart`, `restart_hold` and `max_width` are the
desc's times in seconds. A counter wraps at `2**bits`; `unwrap()` gives the
unwrapped value. `map()` raises `ysp.rt.Error` before the first pair. A fit
belongs to one thread.

## How to run a test on the virtual clock

```python
t0 = rt.virtual_begin()          # 1000.0: this thread now drives the clock
rt.sleep_until(t0 + 10.0)        # returns 0.0 at once; now() is t0 + 10
cost = rt.real_now()             # real time, for a cost
rt.virtual_end()
```

The virtual clock is in this module's copy of `ysp/rt.h` only:
`ysp.serial.now()` and the other modules keep the real clock.

## Reference

| Name | What |
|---|---|
| `now()`, `now_ns()` | the ysp clock in seconds, and in exact int ns |
| `real_now()` | the real clock, also on the virtual clock |
| `sleep_until(t, spin=DEFAULT_SPIN)`, `sleep(duration, spin=...)`, `spin_until(t)` | waits; return the lateness in seconds |
| `thread_cleanup()` | release the calling thread's wait timer (Windows) |
| `clock_info()` | `{'resolution': s, 'hires_timer': bool}` |
| `describe()` | the `yrt_describe()` line for the calling thread |
| `correlate(read, *, edge=False, tries=0, timeout=0.1)` | `Corr(t, other, width, tries, t_ns)` |
| `Fit(mode, *, tick, bits=0, ...)` | `add()`, `map()`, `map_ns()`, `unwrap()`, `info()` |
| `virtual_begin(t0=0)`, `virtual_end()`, `is_virtual()` | the virtual clock |
| `user_dir(kind, subdir=None)` | `<base>/ysp/<subdir>` for `"cache"` or `"config"`, or `None` |
| `DEFAULT_SPIN`, `VIRTUAL_T0`, `FIT_*` | constants; `FIT_*` are `Fit.add()`'s bits |
| `Error` | a refusal from the header |

What is not bound, and why, is in [docs/rt.md](../../../docs/rt.md#bindings).

## Tests

```sh
uv run --no-project --isolated --with . --with pytest python -m pytest -q tests
```

Add `--with ../ysp_serial --with ../ysp_parallel` to run the checks that the
clocks agree; without those packages they skip.
