# ysp.parallel Python binding

A small CPython extension wrapping the
[ysp/parallel.h](../../../include/ysp/parallel.h) single-header library, which builds
on the shared [ysp/rt.h](../../../include/ysp/rt.h). It uses the plain CPython C API
(no nanobind/pybind/Cython) to stay dependency-free, and is built against the
**Limited API / stable ABI** (`Py_LIMITED_API =
0x03080000`), so a single `ysp/parallel.abi3.so` works on CPython 3.8+ without
a rebuild per version.

The distribution is `ysp-parallel` and the module is `ysp.parallel`. `ysp` is a
[PEP 420](https://peps.python.org/pep-0420/) implicit namespace package (no
`__init__.py`), so `ysp-parallel` and `ysp-serial` install independently or
side by side.

## Build / install

From this directory:

```sh
pip install .                       # build + install an abi3 wheel
# or, in place for development:
python setup.py build_ext --inplace
```

The in-place build drops the extension into `ysp/`, an otherwise empty
directory kept in git for exactly that purpose (the `ysp` namespace has no
source of its own).

setup.py finds the headers in the repository's `include/` directory in a
development tree. A
build outside the tree, such as an sdist or a cibuildwheel run, sees only this
directory, so copy `ysp/parallel.h` and `ysp/rt.h` into `include/ysp/` next to
`setup.py` first:

```sh
mkdir -p include/ysp && cp ../../../include/ysp/parallel.h ../../../include/ysp/rt.h include/ysp/
```

Both copies are gitignored, and the repository root still wins when it is
there, so a stale copy cannot shadow a newer header.

The implementation of `ysp/parallel.h`, and of the `ysp/rt.h` it includes, is
compiled directly into the extension. On Linux/macOS the build links
`-pthread` for the async-pulse worker.

## Use

```python
import ysp.parallel as pp

for info in pp.list_ports():            # enumerate without opening
    print(info)                         # {'name': '/dev/parport0', 'backend': 1, 'base_addr': 0}

# Defaults: ppdev /dev/parport0 (Linux), inpout @ LPT1 (Windows).
with pp.Port() as port:                 # or pp.Port(device="/dev/parport1")
    port.write_data(0x55)               # latch a trigger byte
    port.pulse(0x55, width=0.002)       # blocking: 0x55 for 2 ms, then 0
    port.pulse_async(0x55, width=0.002) # returns immediately; worker drops it
    s = port.read_status()
    print(hex(s), port.get_status_bit(pp.STATUS_BUSY))
```

Other backends:

```python
pp.Port(backend=pp.BACKEND_DIRECT, base_addr=pp.LPT1)   # Linux raw I/O (root)
pp.Port(backend=pp.BACKEND_INPOUT, base_addr=0x378)     # Windows inpout
```

Tune the async-pulse worker's real-time reservation (Linux
[SCHED_DEADLINE](https://www.kernel.org/doc/html/latest/scheduler/sched-deadline.html); seconds):

```python
port = pp.Port(rt_runtime=0.0005, rt_deadline=0.002, rt_period=0.002)
print(port.sched)   # {'runtime': 0.0005, 'deadline': 0.002, 'period': 0.002}
```

Omit `rt_period` (or leave it 0) and it defaults to the deadline. Leave all
three unset for the library defaults. Invalid combinations (the rule is
`0 < runtime <= deadline <= period`) raise `pp.Error` at construction.

Timestamp a trigger against the library's own clock, so your numbers and its
deadlines share one base:

```python
t0 = pp.now()                   # monotonic seconds, a float
port.write_data(0x55)
t1 = pp.now()                   # the onset lies after t0; t1 - t0 bounds it
```

`ysp.serial.now()` reads the same clock: both libraries build on
`ysp/rt.h`, so one time base covers the whole rig.

## Units

Every time is a `float` (or `int`) in seconds, as in every ysp binding: the
pulse widths, the `rt_*` reservation, `sched` and `now()`. The binding
converts each one to the `int64_t` nanoseconds of the C API, rounded to the
nearest nanosecond. A width of 0 is allowed. A negative, NaN or infinite
value raises `ValueError` before the library is called, and 9223372036 s or
more raises `OverflowError`. A positive `rt_*` value never rounds to 0, which
would mean "library default".

See [example.py](example.py).

## Threading

A `Port` is safe to share between Python threads. Every method holds a
per-`Port` lock for the duration of its library call, so only one thread is
inside the library on a given handle at a time. Two `pulse()` calls on one port
therefore serialize: the second waits for the first to finish its width.

The lock is needed because `ysp/parallel.h` guards only its data writes against
its own async worker. Every call that returns a bool clears and rewrites one
shared error buffer in the handle, and `pulse()` and `pulse_async()` drop the
GIL, so the GIL alone would not keep a second thread out. A thread waiting for
the lock releases the GIL, so a 2 ms `pulse()` never stalls unrelated Python
code.

What the lock cannot fix is `BACKEND_DIRECT`: `ioperm`/`iopl` grant I/O
permission to the calling thread only, so a direct-backend `Port` is usable
from the thread that opened it (plus the library's own worker) and no other.
See DIRECT IS PER-THREAD in [ysp/parallel.h](../../../include/ysp/parallel.h).

## API

| | |
|---|---|
| `pp.now() -> float` | monotonic seconds from the clock the library times its own deadlines against |
| `pp.list_ports() -> list[dict]` | enumerate ports (`name`, `backend`, `base_addr`) |
| `pp.Port(device=None, backend=BACKEND_DEFAULT, base_addr=0, exclusive=False, rt_runtime=0, rt_deadline=0, rt_period=0)` | open a port; the `rt_*` times are seconds |
| `port.write_data(value)` / `port.read_data()` | data register |
| `port.set_data_dir(input)` | data-line direction (bidirectional ports) |
| `port.pulse(value, width)` | blocking pulse, `width` in seconds (releases the GIL while waiting) |
| `port.pulse_async(value, width)` | non-blocking pulse via the RT worker thread |
| `port.read_status()` / `port.get_status_bit(mask)` | status register |
| `port.read_control()` / `port.write_control(value)` / `port.set_control_bit(mask, on)` | control register |
| `port.close()`, `port.is_open`, `port.base_addr`, `port.backend`, `port.sched`, `port.async_policy` | lifecycle / info; `sched` is the worker's effective `{runtime, deadline, period}` in seconds; `async_policy` is the policy the worker obtained (`ASYNC_*`), log it with timing data |

The `ASYNC_*` values come from `ysp/rt.h` and mean the same thing in
`ysp.serial`. `ASYNC_TIME_CONSTRAINT` is the macOS rung, which this library
never returns.

Errors raise `pp.Error`. Constants: `BACKEND_*`, `ASYNC_*`, `LPT1/2/3`, `STATUS_*`,
`CONTROL_*`, `DEFAULT_RT_{RUNTIME,DEADLINE,PERIOD}` (seconds). `Port` is a
context manager (`with pp.Port() as port:`).

## Version

`ysp-parallel` 0.4.0.1 is `ysp/parallel.h` v0.4.0 with binding revision 1,
the change to seconds. The first three numbers are the header's version; a
fourth counts changes to the binding alone and goes away when the header's
version changes. `pp.__version__` gives the same string.

0.4.0.1 breaks the API of 0.4.0. Replace `now_us()` with `now()` (seconds),
`usec` with `width`, and the `rt_*_ns` arguments with `rt_*`, all in
seconds. The `sched` keys and the `DEFAULT_RT_*` constants lost their `_ns`
and hold seconds.

## Tests

The tests need no port:

```sh
uv build --wheel -o dist
uv run --no-project --with pytest --with dist/ysp_parallel-0.4.0.1-cp38-abi3-<platform>.whl pytest tests
```

Platform setup (ppdev permissions, the Windows inpout DLL) is documented in
the header: [ysp/parallel.h](../../../include/ysp/parallel.h).
