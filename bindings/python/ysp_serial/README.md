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
| reader | `read`, `available`, `purge(rx=True)`; the async reader's thread while it runs |
| writer | `write`, `write_byte`, `drain`, `pulse`, `pulse_async`, `send_break`, `purge(tx=True)` |
| any (one at a time) | `interrupt`, `get_lines`, `set_dtr`, `set_rts` |
| neither | the constructor, `close` (only after both roles returned; it stops the async reader itself) |

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

## Async reader

A Python thread blocked in `read()` gets its bytes only when the interpreter
runs it, so `ps.now()` after the read is late by the GIL and by whatever
else the program does. The async reader (0.5.0.2) is a C thread that reads
the port, stamps each read with the `ps.now()` clock when the read returns,
cuts the bytes into records and keeps them in a ring. It never takes the
GIL. The MATLAB binding (`ysp_serial('async_start', ...)`) and
`ysp/device.h` use the same design.

```python
port.async_start(terminator=b"\n", capacity=4096)   # one byte; or record_size=n
port.write(b"go\n")                                  # writes stay allowed
while running:
    if port.async_wait(0.5):                         # releases the GIL
        for line, t in port.async_read():            # oldest first
            handle(line, t)                          # t: ps.now() seconds
stats = port.async_stop()                            # close() also stops it
```

| Call | Does |
|---|---|
| `async_start(terminator=b"\n", capacity=4096, max_len=256, record_size=0)` | Start the thread. A record ends with `terminator` (one byte, as `bytes` or `int`); a longer record is cut at `max_len` bytes. `record_size > 0` gives records of that many bytes instead. `terminator=None` without `record_size` gives one record per read. The ring holds `capacity` records and is allocated here, so the thread allocates nothing. |
| `async_read(max=0, first=False) -> list` | Take the records in the ring, at most `max` (0: all), without waiting. Each is `(data, t)`: the bytes and the time of the read that brought the last byte. `first=True` adds the time of the read that brought the first byte. |
| `async_wait(timeout) -> int` | Wait up to `timeout` seconds (0 polls, `math.inf` waits forever) for a record. Returns the count in the ring, 0 on timeout. Ctrl+C reaches it within 50 ms. |
| `async_stop() -> dict` | Stop and join the thread and free the ring. Records not read are lost. Returns `async_stats`, or `None` when no reader was running. |
| `async_stats` | `running`, `capacity`, `max_len`, `pending`, `max_used` (the largest ring fill), `lines` (records cut), `overflows`, `overlong`, `bytes`, `reads`, `error` and `policy` (`ASYNC_*`, what the thread's elevation obtained). `None` before the first `async_start()`; kept after `async_stop()`. |

Rules:

- When the ring is full, the thread drops the new record and counts it in
  `overflows`. It never waits for Python. Size `capacity` for the longest
  time your program does not call `async_read()`.
- The thread owns the reader role. `read`, `available`, `interrupt` and
  `purge(rx=True)` raise `ps.Error` until `async_stop()`. The writer role
  stays yours: `write`, `pulse` and the others work while the thread runs.
- If the device goes away, the thread stops. `async_read()` returns the
  records it collected, then raises the error (`ps.Disconnected`).
- `async_stop()` or `close()` from another thread ends an `async_wait()` in
  progress with `ps.Interrupted`. One thread at a time may call
  `async_read()`; a second one at the same time raises `ps.Error`.
- `async_start()` on a running reader raises `ps.Error`. `async_stop()` is
  idempotent.

What it measured on a Teensy 4.0 echo board at 1000 lines per second
(Windows 11, two runs, docs/serial.md): the stamp was 146 to 214 us after
the board sent the line (median), p99 338 to 406 us, no line lost, 3 to 6 %
of one core. The MATLAB reader measured 153 us median, p99 350 to 380 us. A
write beside the running reader cost within 34 us of a write without one.

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
| `port.async_start(terminator=b"\n", capacity=4096, max_len=256, record_size=0)` | start the async reader (see [Async reader](#async-reader)) |
| `port.async_read(max=0, first=False) -> list` | take `(data, t)` records from its ring without waiting |
| `port.async_wait(timeout) -> int` | wait for a record; releases the GIL |
| `port.async_stop() -> dict` | stop it; returns `async_stats` |
| `port.close()`, `port.is_open` | lifecycle; `Port` is a context manager |

Read-only properties: `is_open`, `device`, `baud`, `data_bits`, `parity`,
`stop_bits`, `flow`, `write_timeout` (seconds, `math.inf` for none),
`low_latency`, `keep_dtr_on_close`, `async_stats`, `sched` (the worker's effective
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
| `ps.Interrupted` | `interrupt()` or `close()` woke a blocked `read()`; `async_stop()` or `close()` ended an `async_wait()` |
| `ps.Closed` | the port is not open |

The message is the library's `yser_strerror()` text plus the OS error number
from the slot that belongs to the failing role (`rd_oserr`, `wr_oserr` or
`misc_oserr`). Constants mirror the header: `PARITY_*`, `STOP_BITS_*`,
`FLOW_*`, `LINE_*`, `PURGE_*`, `READ_*`, `ERR_*`, `ASYNC_*`,
`TIMEOUT_INFINITE` (`math.inf`), `DEFAULT_BAUD`, and in seconds
`DEFAULT_WRITE_TIMEOUT` and `DEFAULT_RT_{RUNTIME,DEADLINE,PERIOD}`.

## Version

`ysp-serial` 0.5.1 is `ysp/serial.h` v0.5.1. The header changed only its
TIMING comment, so 0.5.1 has the API of 0.5.0.2. The first three numbers are
the header's version; a fourth counts changes to the binding alone and goes
away when the header's version changes: 0.5.0.1 changed every time to
seconds, 0.5.0.2 added the async reader. `ps.__version__` gives the same
string. 0.5.0.2 adds calls and does not change the API of 0.5.0.1.

0.5.0.1 breaks the API of 0.5.0. Replace `now_us()` with `now()` (seconds),
`timeout_ms` with `timeout`, `write_timeout_ms` with `write_timeout`,
`usec` with `width`, `send_break(ms)` with `send_break(duration)`, and the
`rt_*_ns` arguments with `rt_*`, all in seconds. `TIMEOUT_INFINITE` is
`math.inf`, not 4294967295.

## Tests

Most tests need no port:

```sh
uv build --wheel -o dist
uv run --no-project --with pytest --with dist/ysp_serial-0.5.1-cp38-abi3-<platform>.whl pytest tests
```

On Linux, `tests/test_async_pty.py` runs the async reader on a
pseudo-terminal pair. `tests/test_async_device.py` needs an echo board
(`firmware/ysp_echo`, a Teensy 4.0 or a Pico) and runs only when
`YSP_SERIAL_PORT` names its port. It checks echo round trips with the
reader on, a stream of 1000 lines per second for 10 s with no line lost, the
latency of the reader's stamps against the board's own stamps, and the cost
of a write while the reader runs. Add `-s` to see the numbers;
`YSP_SERIAL_RESULTS=<dir>` also writes them to `<dir>/async_device.json`.

```sh
YSP_SERIAL_PORT=COM3 uv run --no-project --with pytest --with dist/ysp_serial-0.5.1-cp38-abi3-<platform>.whl pytest tests -s
```

Device naming, USB latency, DTR-driven board resets and what a serial trigger
is worth in timing terms are documented in the header:
[ysp/serial.h](../../../include/ysp/serial.h).
