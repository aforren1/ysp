# ysp.serial Python binding

A small CPython extension wrapping the
[ysp/serial.h](../../../include/ysp/serial.h) single-header library, which builds on
the shared [ysp/rt.h](../../../include/ysp/rt.h). It uses the plain CPython C API (no
nanobind/pybind/Cython) to stay dependency-free, and is built against the
**Limited API / stable ABI** (`Py_LIMITED_API =
0x03080000`), so a single `ysp/serial.abi3.so` works on CPython 3.8+ without a
rebuild per version.

The distribution is `ysp-serial` and the module is `ysp.serial`. `ysp` is a
[PEP 420](https://peps.python.org/pep-0420/) implicit namespace package (no
`__init__.py`), so `ysp-serial` and `ysp-parallel` install independently or
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
directory, so copy `ysp/serial.h` and `ysp/rt.h` into `include/ysp/` next to
`setup.py` first:

```sh
mkdir -p include/ysp && cp ../../../include/ysp/serial.h ../../../include/ysp/rt.h include/ysp/
```

Both copies are gitignored, and the repository root still wins when it is
there, so a stale copy cannot shadow a newer header.

The implementation of `ysp/serial.h`, and of the `ysp/rt.h` it includes, is
compiled directly into the extension. On Linux/macOS the build links
`-pthread` for the async-pulse worker; on Windows it links `setupapi` and
`advapi32` for port enumeration.

## Use

```python
import ysp.serial as ps

for info in ps.list_ports():            # enumerate without opening
    print(info)   # {'name': '/dev/ttyUSB0', 'description': ..., 'vid': 1027, ...}

# Find a box by USB id instead of hard-coding the port name.
match = ps.find_ports(vid=0x0403, pid=0x6001)     # FTDI FT232R
device = match[0]["name"] if match else "/dev/ttyUSB0"

with ps.Port(device, baud=115200) as port:
    port.write_byte(0x55)                   # raise a trigger
    port.pulse(0x55, 0x00, 0.002)           # 0x55, hold 2 ms, then 0x00
    port.pulse_async(0x55, 0x00, 0.002)     # same, without blocking
    frame = port.read(6, 0.5, all=True)     # a 6-byte frame within 0.5 s
    pending = port.read(64, 0)              # poll: whatever is buffered now
```

`read` returns `b""` on timeout. A disconnect, an `interrupt()` or a closed
port raise instead, so a listener with a long timeout cannot spin on a dead
port.

## Units

Every time is a `float` (or `int`) in seconds, as in every ysp binding. The
binding converts each one to the `int64_t` nanoseconds of the C API, rounded
to the nearest nanosecond.

| Value | Meaning |
|---|---|
| `0` | read: poll. `write_timeout`: refused (`ValueError`), because a write cannot poll; omit the argument for `DEFAULT_WRITE_TIMEOUT` (1 s). A width or a break of 0 is allowed. |
| `math.inf` (`TIMEOUT_INFINITE`) | read and `write_timeout`: wait forever. Refused for a width, a break and the `rt_*` values. |
| a negative value, NaN | refused (`ValueError`) before the library is called |
| 9223372036 s or more | refused (`OverflowError`): it does not fit in `int64_t` nanoseconds |

`None` is not a timeout: it raises `TypeError`. The OS waits in whole
milliseconds, so a positive timeout rounds up to the next millisecond and
never becomes a poll.

Timestamp a trigger against the library's own clock, so your numbers and its
deadlines share one base:

```python
t0 = ps.now()                   # monotonic seconds, a float
port.write_byte(0x55)
t1 = ps.now()
```

The physical onset lies after `t0` and, over USB, after the next frame
boundary; `t1 - t0` is the syscall cost. Neither is the onset, they bound it.
See TIMING in [ysp/serial.h](../../../include/ysp/serial.h). `ysp.parallel.now()`
reads the same clock: both libraries build on `ysp/rt.h`, so one time base
covers the whole rig. A `float` resolves this clock to 1.9 ns or better for
the first 194 days of uptime.

See [example.py](example.py).

## Threading

The header's rule holds for Python threads: **one reader thread and one writer
thread** may use a `Port` at the same time, and nothing else.

| Role | Calls |
|---|---|
| reader | `read`, `available`, `purge(rx=True)` |
| writer | `write`, `write_byte`, `drain`, `pulse`, `pulse_async`, `send_break`, `purge(tx=True)` |
| any (one at a time) | `interrupt`, `get_lines`, `set_dtr`, `set_rts` |
| neither | the constructor, `close` (only after both roles returned) |

`read`, `write`, `drain`, `pulse`, `send_break` and `pulse_async`'s onset write
release the GIL, so a listener thread does not stall the interpreter.

Shut a listener down with interrupt, join, close. Do **not** call `close()`
from another thread while a `read()` is in flight: closing a descriptor a
second thread is still using is undefined on POSIX.

```python
stop_reading = False

def listener(port):
    while not stop_reading:
        try:
            data = port.read(64, ps.TIMEOUT_INFINITE)
        except ps.Interrupted:
            return                      # shutdown, not an error
        except ps.Disconnected:
            return                      # the box was unplugged
        handle(data)

t = threading.Thread(target=listener, args=(port,))
t.start()
...
stop_reading = True
port.interrupt()        # wakes the blocked read
t.join()                # the reader has returned
port.close()            # now it is safe
```

## API

| | |
|---|---|
| `ps.now() -> float` | monotonic seconds from the clock the library times its own deadlines against |
| `ps.list_ports() -> list[dict]` | enumerate ports (`name`, `description`, `serial_number`, `location`, `vid`, `pid`) |
| `ps.find_ports(vid=0, pid=0, serial_number=None, location=None, description=None, name=None) -> list[dict]` | the matching subset; zero/`None` matches anything |
| `ps.Port(device, baud=0, data_bits=0, parity=PARITY_NONE, stop_bits=STOP_BITS_1, flow=FLOW_NONE, write_timeout=DEFAULT_WRITE_TIMEOUT, exclusive=False, low_latency=False, dtr_low_on_open=False, rts_low_on_open=False, keep_dtr_on_close=False, rt_runtime=0, rt_deadline=0, rt_period=0)` | open a port; `device` is required, times are seconds, a zero field means the library default |
| `port.read(n, timeout, all=False) -> bytes` | read; `timeout` in seconds, 0 polls, `math.inf` waits forever; `all=True` waits for the full `n` |
| `port.write(data) -> int` | write a bytes-like object, or any iterable of ints 0..255; returns the count accepted |
| `port.write_byte(value) -> int` | write one byte; returns 1 |
| `port.pulse(on, off, width) -> int` | blocking pulse, `width` in seconds; returns 2 |
| `port.pulse_async(on, off, width) -> int` | non-blocking pulse via the RT worker thread; returns 1 |
| `port.available() -> int` | bytes buffered by the driver |
| `port.drain()` | wait until the transmit buffer is empty |
| `port.purge(rx=True, tx=False)` | discard buffered bytes |
| `port.interrupt()` | wake a `read()` blocked on another thread |
| `port.set_dtr(on)` / `port.set_rts(on)` | drive the output lines |
| `port.get_lines() -> int` | input lines as a `LINE_*` mask |
| `port.send_break(duration)` | hold TX in the break state for `duration` seconds |
| `port.close()`, `port.is_open` | lifecycle; `Port` is a context manager |

Read-only properties: `is_open`, `device`, `baud`, `data_bits`, `parity`,
`stop_bits`, `flow`, `write_timeout` (seconds, `math.inf` for none),
`low_latency`, `keep_dtr_on_close`, `sched` (the worker's effective
`{runtime, deadline, period}` in seconds), `async_policy`, `rd_oserr`,
`wr_oserr`, `misc_oserr`. The effective settings
are what the driver accepted, not what you asked for. Log `async_policy`
(`ASYNC_*`) next to timing data: `SCHED_DEADLINE` is refused without
`CAP_SYS_NICE` or under CPU pinning, and the library falls back silently. The
`ASYNC_*` values come from `ysp/rt.h` and mean the same thing in
`ysp.parallel`.

Tune the async-pulse worker's real-time reservation (Linux
[SCHED_DEADLINE](https://www.kernel.org/doc/html/latest/scheduler/sched-deadline.html); seconds):

```python
port = ps.Port(device, rt_runtime=0.001, rt_deadline=0.010, rt_period=0.010)
```

The rule is `0 < runtime <= deadline <= period`; a zero period means "same as
deadline". Invalid combinations raise `ps.Error` at construction.

## Errors

| Exception | Raised when |
|---|---|
| `ps.Error` | base class; also a failed open, a bad argument or an OS error |
| `ps.Disconnected` | the device went away (USB unplug); close the port |
| `ps.Interrupted` | `interrupt()` or `close()` woke a blocked `read()` |
| `ps.Closed` | the port is not open |

The message is the library's `yser_strerror()` text plus the OS error number
from the slot that belongs to the failing role (`rd_oserr`, `wr_oserr` or
`misc_oserr`). Constants mirror the header: `PARITY_*`, `STOP_BITS_*`,
`FLOW_*`, `LINE_*`, `PURGE_*`, `READ_*`, `ERR_*`, `ASYNC_*`,
`TIMEOUT_INFINITE` (`math.inf`), `DEFAULT_BAUD`, and in seconds
`DEFAULT_WRITE_TIMEOUT` and `DEFAULT_RT_{RUNTIME,DEADLINE,PERIOD}`.

## Version

`ysp-serial` 0.5.0.1 is `ysp/serial.h` v0.5.0 with binding revision 1, the
change to seconds. The first three numbers are the header's version; a fourth
counts changes to the binding alone and goes away when the header's version
changes. `ps.__version__` gives the same string.

0.5.0.1 breaks the API of 0.5.0. Replace `now_us()` with `now()` (seconds),
`timeout_ms` with `timeout`, `write_timeout_ms` with `write_timeout`,
`usec` with `width`, `send_break(ms)` with `send_break(duration)`, and the
`rt_*_ns` arguments with `rt_*`, all in seconds. `TIMEOUT_INFINITE` is
`math.inf`, not 4294967295.

## Tests

The tests need no port:

```sh
uv build --wheel -o dist
uv run --no-project --with pytest --with dist/ysp_serial-0.5.0.1-cp38-abi3-<platform>.whl pytest tests
```

Device naming, USB latency, DTR-driven board resets and what a serial trigger
is worth in timing terms are documented in the header:
[ysp/serial.h](../../../include/ysp/serial.h).
