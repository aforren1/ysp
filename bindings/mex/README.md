# ysp MATLAB / Octave (MEX) bindings

One MEX function per header, in the command-dispatch style of widmann's
[ppdev-mex](https://github.com/widmann/ppdev-mex): the first argument is a
command string, and `open` returns an opaque `uint64` handle that you pass
back to every other command. A handle is a counter, not an address, and is
never reused, so a handle kept past its `close` always raises
`ysp_<name>:handle` instead of reaching whichever port was opened next.

| Source | Wraps | Example |
|---|---|---|
| `ysp_parallel.c` | [ysp/parallel.h](../../include/ysp/parallel.h) | [example_parallel.m](example_parallel.m) |
| `ysp_serial.c` | [ysp/serial.h](../../include/ysp/serial.h) | [example_serial.m](example_serial.m) |
| `ysp_stair.c` | [ysp/stair.h](../../include/ysp/stair.h) | [example_stair.m](example_stair.m) |
| `ysp_quest.c` | [ysp/quest.h](../../include/ysp/quest.h) | [example_quest.m](example_quest.m) |
| `ysp_aep.c` | [ysp/aep.h](../../include/ysp/aep.h) | [example_aep.m](example_aep.m) |
| `ysp_trials.c` | [ysp/trials.h](../../include/ysp/trials.h) | [example_trials.m](example_trials.m) |
| `ysp_color.c` | [ysp/color.h](../../include/ysp/color.h) | [test_mex_color.m](test_mex_color.m); the command reference is in [docs/color.md](../../docs/color.md) |
| `ysp_box.c` | [ysp/box.h](../../include/ysp/box.h) | [example_box.m](example_box.m), [test_mex_box.m](test_mex_box.m) |
| `ysp_rdk.c` | [ysp/rdk.h](../../include/ysp/rdk.h) | [example_rdk.m](example_rdk.m), [test_mex_rdk.m](test_mex_rdk.m) |
| `ysp_pack.c` | [ysp/pack.h](../../include/ysp/pack.h) | [example_pack.m](example_pack.m), [test_mex_pack.m](test_mex_pack.m) |
| `ysp_rt.c` | [ysp/rt.h](../../include/ysp/rt.h) | [example_rt.m](example_rt.m) |
| `ysp_device.c` | [ysp/device.h](../../include/ysp/device.h) | [example_device.m](example_device.m) |
| `ysp_rigfile.c` | [ysp/rigfile.h](../../include/ysp/rigfile.h) | [example_rigfile.m](example_rigfile.m) |
| `ysp_response.c` | [ysp/response.h](../../include/ysp/response.h) | [example_response.m](example_response.m) |
| `ysp_timeline.c` | [ysp/timeline.h](../../include/ysp/timeline.h) | [example_timeline.m](example_timeline.m) |
| `ysp_dataset.c` | [ysp/dataset.h](../../include/ysp/dataset.h) | [example_dataset.m](example_dataset.m) |
| `ysp_audio.c` | [ysp/audio.h](../../include/ysp/audio.h) | [example_audio.m](example_audio.m), [test_mex_audio.m](test_mex_audio.m) |
| `ysp_vis.c` (the core), `ysp_screen.c`, `ysp_gfx.c`, `ysp_video.c` | [ysp/screen.h](../../include/ysp/screen.h), [ysp/gfx.h](../../include/ysp/gfx.h), [ysp/video.h](../../include/ysp/video.h) (ysp-vis, Windows only) | [example_screen.m](example_screen.m), [example_gfx_2afc.m](example_gfx_2afc.m), [example_gfx_rsvp.m](example_gfx_rsvp.m), [example_gfx_stroop.m](example_gfx_stroop.m), [example_gfx_reading.m](example_gfx_reading.m), [test_mex_vis.m](test_mex_vis.m), [test_mex_vis_gfx.m](test_mex_vis_gfx.m), [test_mex_vis_text.m](test_mex_vis_text.m), [test_mex_vis_window.m](test_mex_vis_window.m) |

The two transport headers build on the shared [ysp/rt.h](../../include/ysp/rt.h), so
both MEX functions compile it too. `ysp_quest.c` and `ysp_aep.c` define
`YQST_ASYNC` and `YAEP_ASYNC`, so they also compile `ysp/rt.h`, for the
async inference thread. `ysp_stair.c` compiles nothing but its header, and
`ysp_trials.c` compiles ysp/trials.h and the [ysp/table.h](../../include/ysp/table.h)
it requires. The four adaptive-method sources also include
`ysp_mex_util.h`, a binding-internal header with the handle table and the
argument helpers; a copy of one of those sources needs that file beside it.

[test_mex.m](test_mex.m) runs the key checks of the four adaptive modules and
of `ysp_serial` and the time units of `ysp_parallel` (no port needed) in
MATLAB or Octave and prints `PASS` with the number of checks. Without a port it checks `ysp_serial`'s
arguments and handles only; with the environment variable
`YSP_TEST_ECHO_PORT` set to the port of an echo board (`firmware/ysp_echo`)
it also runs the async reader on a 2000-line stream.
[test_mex_color.m](test_mex_color.m) does
the same for `ysp_color` (compiles nothing but ysp/color.h and
`ysp_mex_util.h`); `tests/compare/color_ptb.m` compares it with
Psychtoolbox's colorimetric code.
[test_mex_rt.m](test_mex_rt.m) does the same for `ysp_rt` (compiles
ysp/rt.h and `ysp_mex_util.h`); it needs no device.
[test_mex_device.m](test_mex_device.m) checks `ysp_device` and
`ysp_rigfile` together on simulated boards (no hardware) and prints `PASS`
with the number of checks.
[test_mex_dataset.m](test_mex_dataset.m) does the same for `ysp_dataset`
(compiles ysp/dataset.h with the ysp/trials.h, ysp/table.h and
ysp/json.h it includes, and `ysp_mex_util.h`). Its crash check starts a
second MATLAB or Octave, which writes rows and is then killed.

## Build

In MATLAB or Octave, from this directory:

```matlab
run build.m            % builds every ysp_*.c
build('parallel')      % builds one
```

- **Octave** needs the development package that provides `mex`/`mkoctfile`
  (`sudo apt install octave-dev`, or `liboctave-dev` on older releases).
- **MATLAB** needs a configured compiler (`mex -setup`).

The build adds `-I` for include/ under the repository root, where the headers live. That
one path covers the transport header and the `ysp/rt.h` it includes. A
standalone build outside this repository must keep `ysp/rt.h` in the same
`ysp/` directory as the transport header and put the parent of that `ysp/`
directory on the include path with `-I`. The build also links
`-lpthread` on Linux/macOS for the async-pulse workers. Windows uses
Win32 threads and needs no thread library. `ysp_serial` needs `setupapi` and
`advapi32` for port enumeration on Windows, and `build.m` passes
`-lsetupapi -ladvapi32` there: the header's `#pragma comment(lib)` reaches
MSVC only, so a MinGW toolchain (MinGW Octave, MATLAB's MinGW-w64 add-on)
needs the libraries on the command line. `mex` accepts that spelling on both
toolchains. The adaptive-method sources need no library but the thread
library, which `ysp_quest.c` and `ysp_aep.c` use on Linux/macOS.

`ysp_audio.c` compiles miniaudio 0.11.25 (one public-domain file, the pin
of CMakeLists.txt's `YSP_FETCH_MINIAUDIO`). Put `miniaudio.h` in
`bindings/mex/miniaudio/` (gitignored) or name its directory in the
environment variable `YSP_MINIAUDIO_DIR`; without it, `build.m` skips
`ysp_audio.c` with a message. It links `-ldl -lm` on Linux and nothing on
Windows. `ysp_audio.c` and `ysp_timeline.c` also include the shared tables
of `bindings/share/` (`build.m` adds `-I` for it) and export one function
besides `mexFunction`; with MATLAB on Linux and macOS, `build.m` gives the
linker a list of both.

## ysp_parallel

```matlab
ports = ysp_parallel('list');                 % struct array: name/backend/base_addr

h = ysp_parallel('open');                      % platform defaults
% h = ysp_parallel('open', '/dev/parport1');   % ppdev device (Linux)
% h = ysp_parallel('open', 'direct', 888);     % raw x86 I/O @ base (root)
% h = ysp_parallel('open', 'inpout', 888);     % Windows inpout @ base

ysp_parallel('write',      h, 85);             % data register (0..255)
d = ysp_parallel('read',   h);                 % read data register (uint8)
ysp_parallel('setdir',     h, true);           % data direction: input
ysp_parallel('pulse',      h, 85, 0.002);      % blocking 2 ms pulse (seconds)
ysp_parallel('pulseasync', h, 85, 0.002);      % non-blocking pulse
s = ysp_parallel('status', h);                 % status register
c = ysp_parallel('control', h);                % read control register
ysp_parallel('control',    h, 4);              % write control register
ysp_parallel('close',      h);
```

Tune the async-pulse worker's real-time reservation (Linux
[SCHED_DEADLINE](https://www.kernel.org/doc/html/latest/scheduler/sched-deadline.html))
by passing a trailing **struct** (fields in seconds) to `open`:

```matlab
rt = struct('runtime', 5e-4, 'deadline', 2e-3, 'period', 2e-3);
h  = ysp_parallel('open', rt);                  % default device + RT params
h  = ysp_parallel('open', '/dev/parport0', rt); % with an explicit device
cur = ysp_parallel('sched', h);                 % struct of effective params
```

Any subset of fields is accepted. Missing fields are 0, and a 0 `period`
means "same as `deadline`". Leave the struct off for the library defaults.
Invalid combinations (the rule is `0 < runtime <= deadline <= period`) raise
an error at `open`. A field of the old nanosecond form (`runtime_ns`,
`deadline_ns`, `period_ns`) raises `ysp_parallel:arg`: read as seconds, its
value would be days.

Bracket a trigger with the library's own clock so your timestamps and the
library's deadlines share one base:

```matlab
t0 = ysp_parallel('now');        % monotonic seconds, no handle needed
ysp_parallel('write', h, 85);
t1 = ysp_parallel('now');        % the onset lies after t0; t1 - t0 bounds it
```

`ysp_serial('now')` reads the same clock: both headers build on
`ysp/rt.h`, so one time base covers the whole rig.

Every time is seconds: the pulse widths, the RT struct, `'sched'` and
`'now'`. The MEX function converts each one to the `int64_t` nanoseconds of
the C API, rounded to the nearest nanosecond. A width of 0 is allowed. A
negative, NaN or infinite time raises `ysp_parallel:arg` before the library
is called. Until 2026-10-11 the widths were microseconds, the RT struct
nanoseconds and the clock command `'now_us'`.

### Commands

| Command | Returns | Notes |
|---|---|---|
| `'list'` | struct array | `name`, `backend`, `base_addr`; opens nothing |
| `'open' [, dev \| 'direct' \| 'inpout' [, base]] [, rtStruct]` | `uint64` handle | defaults per platform; optional trailing RT struct (seconds) |
| `'write', h, value` | none | data register, 0..255 |
| `'read', h` | `uint8` | data register |
| `'setdir', h, isInput` | none | bidirectional-port direction |
| `'pulse', h, value, width` | none | blocking; `width` in seconds |
| `'pulseasync', h, value, width` | none | non-blocking (RT worker); `width` in seconds |
| `'status', h` | `uint8` | status register (read-only) |
| `'control', h [, value]` | `uint8` / none | read or write control register |
| `'sched', h` | struct | effective RT params `runtime`/`deadline`/`period` in seconds and `policy` (`deadline`, `fifo`, `time_critical`, `normal`, `none`), the policy the worker actually obtained |
| `'now'` | `double` | monotonic seconds from the library clock, the same clock as `ysp_serial('now')`; opens nothing |
| `'close', h` | none | release the port |

Errors are raised through `error()` with id `ysp_parallel:*`. Every open port
is closed automatically on `clear ysp_parallel` and at interpreter exit
(a `mexAtExit` hook), so a forgotten `close` cannot leave the async worker
thread running against a freed port. Calling `close` yourself is still good
form.

Platform setup (ppdev permissions, the Windows inpout DLL) is documented in
the header: [ysp/parallel.h](../../include/ysp/parallel.h).

## ysp_serial

```matlab
ports = ysp_serial('list');                    % struct array, one per port
% name / description / serial_number / location / driver / vid / pid
match = ysp_serial('find', struct('vid', 1027, 'pid', 24577));   % FTDI FT232R

h = ysp_serial('open', '/dev/ttyUSB0');        % 115200 8N1 by default
% h = ysp_serial('open', 'COM3', struct('baud', 9600, 'parity', 'even'));

n    = ysp_serial('write',     h, uint8([1 2 3]));  % or [1 2 3]
n    = ysp_serial('writebyte', h, 85);              % trigger workhorse
data = ysp_serial('read',      h, 6, 0.5, 'all');   % 6-byte frame in 0.5 s
data = ysp_serial('read',      h, 64, 0);           % poll: [] if nothing
ysp_serial('pulse',      h, 85, 0, 0.002);     % blocking: 85, 2 ms, then 0
ysp_serial('pulseasync', h, 85, 0, 0.002);     % non-blocking (RT worker)
info = ysp_serial('info', h);                  % effective settings
ysp_serial('async_start', h, 10);              % reader thread, lines end in LF
[lines, t] = ysp_serial('async_read', h);      % lines since the last call
st = ysp_serial('async_stop', h);              % counts: dropped, overlong, ...
ysp_serial('close', h);
```

Every time that `ysp_serial` takes or returns is a `double` in seconds, as in
the other bindings: timeouts, pulse widths, the break duration, the RT
reservation, `now` and the async stamps. The C library under it counts
`int64` nanoseconds; the binding converts at the call and rounds to the
nearest nanosecond. A timeout is 0 (poll), a positive number, or `Inf`
(forever). `NaN` and negative values raise `ysp_serial:arg`. Binding
v2.0.0 made this change: v1 took milliseconds and microseconds and had
`now_us`, which is gone. `open` refuses the v1 field names, so an old
script fails at once instead of opening with a default.

Bracket a trigger with the library's own clock so your timestamps and the
library's deadlines share one base (see TIMING in the header):

```matlab
t0 = ysp_serial('now');          % monotonic seconds, no handle needed
ysp_serial('writebyte', h, 85);
t1 = ysp_serial('now');          % the onset lies after t0; t1 - t0 bounds it
```

`read` returns a `uint8` row vector, empty on timeout. A disconnect, an
`interrupt` or a closed port raise instead, so a listener with a long timeout
cannot spin on a dead port.

`open` takes an optional struct of settings. Any subset is accepted; a missing
field is the library default (115200 baud, 8 data bits, no parity, 1 stop bit,
no flow control, 1 s write timeout). A field that `open` does not know
raises `ysp_serial:arg`.

| Field | Values |
|---|---|
| `baud` | any rate the driver accepts, e.g. `9600`, `115200` |
| `data_bits` | `5`..`8` |
| `parity` | `'none'`, `'odd'`, `'even'`, `'mark'`, `'space'` (the last two are rejected on macOS) |
| `stop_bits` | `1` or `2` |
| `flow` | `'none'`, `'rtscts'`, `'xonxoff'` |
| `write_timeout` | seconds, rounded up to whole milliseconds by the library; `0` is the default (1 s), `Inf` blocks |
| `min_hold` | seconds; the least time between a pulse's onset write returning and its trailing write; `0` is the default (0.25 ms) |
| `exclusive`, `low_latency` | logical |
| `dtr_low_on_open`, `rts_low_on_open`, `keep_dtr_on_close` | logical; see DTR AND DEVICE RESET in the header |
| `rt_runtime`, `rt_deadline`, `rt_period` | async-worker RT reservation (Linux SCHED_DEADLINE), seconds |

The RT rule is `0 < runtime <= deadline <= period`; a zero period means "same
as deadline". Invalid combinations raise an error at `open`. `info` reports the
policy the worker actually obtained, which is not always the one requested.

### Commands

| Command | Returns | Notes |
|---|---|---|
| `'list'` | struct array | `name`, `description`, `serial_number`, `location`, `driver` (Windows: the driver service, `'usbser'`, `'FTDIBUS'`, ...; `''` elsewhere), `vid`, `pid`; opens nothing |
| `'find' [, filterStruct]` | struct array | filter fields `vid`, `pid`, `serial_number`, `location`, `description`, `name`; absent or 0/`''` matches anything |
| `'open', device [, settingsStruct]` | `uint64` handle | `device` is required |
| `'write', h, data` | count (optional) | `uint8` array, or a `double` array of integers 0..255 |
| `'writebyte', h, value` | count (optional) | one byte, 0..255 |
| `'read', h, n, timeout [, 'all']` | `uint8` row vector | `timeout` in seconds, `0` polls; empty on timeout; `'all'` waits for the full `n` |
| `'available', h` | count | bytes buffered by the driver |
| `'drain', h` | none | wait until the transmit buffer is empty |
| `'purge', h [, 'rx' \| 'tx' \| 'both']` | none | discard buffered bytes; default `'rx'` |
| `'interrupt', h` | none | latch a wake that the next `read` consumes; it cannot reach a read already in flight (see below) |
| `'now'` | `double` | monotonic seconds from the library clock, the clock of `ysp_parallel` too; opens nothing |
| `'pulse', h, on, off, width` | count (optional) | blocking; `width` in seconds from the start of the onset write; returns 2 |
| `'pulseasync', h, on, off, width` | count (optional) | non-blocking: queues the pulse for the RT worker, which writes both bytes; `width` in seconds; returns 1 when queued, 0 when the queue is full or an earlier async write timed out |
| `'setdtr', h, on` / `'setrts', h, on` | none | drive the output lines |
| `'lines', h` | struct | `mask` plus `cts`, `dsr`, `ri`, `dcd` logicals |
| `'break', h, duration` | none | hold TX in the break state for `duration` seconds |
| `'info', h` | struct | effective settings, `async_policy` as a string, `rd_oserr` / `wr_oserr` / `misc_oserr`, `driver`, and `rts_via_dtr` (`setrts` also sends a DTR request, because Windows' `usbser.sys` sends an RTS change only with one) |
| `'async_start', h [, terminator [, lines [, max_len]]]` | none | start the reader thread; defaults `10` (LF), `8192` lines, `256` bytes per line; see below |
| `'async_read', h [, max_lines]` | `lines` [, `t`, `t_first`] | the complete lines since the last call: a cell column of `uint8` rows (terminator included) and two `double` columns of stamps in seconds |
| `'async_stop', h` | struct (optional) | stop the reader thread; fields `pending` (lines not read, discarded), `dropped`, `overlong`, `bytes`, `reads`, `error` |
| `'version'` | string [, string] | the binding version (`2.2.0`), and as a second output the version of ysp/serial.h in this build; opens nothing |
| `'close', h` | none | release the port; stops a running reader first |

`write`, `writebyte`, `pulse` and `pulseasync` return their count only when
you ask for it (`n = ysp_serial('write', ...)`), so a bare call prints
nothing. The count matters: a write that hits `write_timeout` returns fewer
bytes than you passed and is not an error.

Errors are raised through `error()` with an identifier that names the library
code, so a script can branch on it:

| Identifier | Meaning |
|---|---|
| `ysp_serial:io` | OS error; the number is in `info.rd_oserr` / `wr_oserr` / `misc_oserr` |
| `ysp_serial:disconnected` | the device went away (USB unplug); close the handle |
| `ysp_serial:closed` | the port is not open |
| `ysp_serial:interrupted` | `interrupt` woke a blocked `read` |
| `ysp_serial:async` | an async call with no reader running, a second `async_start`, or a reader-role call (`read`, `available`, `interrupt`, `purge` of `'rx'` or `'both'`) while the reader runs |
| `ysp_serial:arg` | bad argument |
| `ysp_serial:open`, `ysp_serial:handle`, `ysp_serial:usage` | open failure, bad handle, bad call |

Every open port is closed automatically on `clear ysp_serial` and at
interpreter exit (a `mexAtExit` hook), so a forgotten `close` cannot leave the
async worker thread running against a freed port. Calling `close` yourself is
still good form.

### Blocking reads: poll, do not wait forever

MATLAB and Octave run this binding on one thread, so the header's
one-reader-one-writer rule collapses to "one call at a time". A MEX call owns
the interpreter thread until it returns. Timer callbacks, the event loop and
Ctrl-C are all dispatched on that same thread, so **nothing can interrupt a
blocked `read`**: a `timer` whose callback calls `interrupt` never runs while
the read is in flight.

Never pass `Inf` (forever) from MATLAB or Octave. Poll in short slices
instead, so the interpreter gets the thread back between them:

```matlab
% Wait up to 5 s for a 6-byte frame, staying responsive to Ctrl-C.
frame = uint8([]);
t0 = tic;
while toc(t0) < 5
    frame = [frame ysp_serial('read', h, 6 - numel(frame), 0.05)];  % 50 ms slice
    if numel(frame) == 6, break; end
    drawnow limitrate;    % run timers, Ctrl-C and the GUI
end
```

`interrupt` latches a wake that the *next* `read` consumes, so on one thread it
only makes that read return immediately with `ysp_serial:interrupted`. It
exists for symmetry with the C API. To receive while the script does other
work, use the async reader below.

### Asynchronous read: lines stamped when they arrive

A `read` can stamp its bytes only when the script gets round to calling it.
`async_start` starts a C thread that does what ysp/device.h's reader does: it
blocks in `yser_read`, takes `now` when each read returns, cuts the bytes
into lines at the terminator and keeps the lines in a ring. The script
collects them with `async_read` whenever it likes, for example once per frame:

```matlab
ysp_serial('async_start', h, 10);             % LF ends a line
ysp_serial('write', h, uint8(sprintf('s 1000 500\n')));   % writes stay allowed
for frame = 1:60
    % ... draw, flip ...
    [lines, t, t_first] = ysp_serial('async_read', h);
    for i = 1:numel(lines)
        fprintf('%s arrived at %.6f s\n', char(lines{i}(1:end-1)), t(i));
    end
end
st = ysp_serial('async_stop', h);             % st.dropped == 0: nothing lost
```

- `t` is `now` when the read that brought the line's terminator
  returned; `t_first` is the same for the line's first byte. Both are
  seconds on the `now` clock.
- The thread owns the reader role of the header's THREADING rule while it
  runs. `read`, `available`, `interrupt` and an RX purge raise
  `ysp_serial:async`; `write`, `writebyte`, the pulses, the modem lines and
  a TX purge stay allowed.
- The ring and the line buffer are allocated by `async_start` (`lines` times
  `max_len` bytes); the thread allocates nothing after that. A full ring
  drops the newest complete line and counts it in `dropped`; it never
  overwrites a line that `async_read` has not returned. A line longer than
  `max_len` is cut and counted in `overlong`.
- The thread raises its own priority with `yrt_thread_elevate`, as
  ysp/device.h does. If the port fails (an unplug), the thread stops;
  `async_read` returns the lines it has, then raises the error.
- `async_stop` wakes the thread with `interrupt` and joins it; `close` and
  `clear ysp_serial` do the same. After `async_stop` a plain `read` works
  again.

What it measured on a Teensy 4.0 against MATLAB's callback and IOPort's
background reader is in
[docs/serial.md](../../docs/serial.md#results-with-the-teensy-40-echo-board).
The Python binding has no async reader yet.

Device naming, USB latency, DTR-driven board resets and what a serial trigger
is worth in timing terms are documented in the header:
[ysp/serial.h](../../include/ysp/serial.h).

## ysp_rt

```matlab
t    = ysp_rt('now');                          % seconds on the ysp clock
tn   = ysp_rt('now_ns');                       % the same, exact int64 ns
late = ysp_rt('sleep_until', t + 0.5);         % seconds late
late = ysp_rt('sleep_until', t + 0.5, 0);      % no spin: the OS wait only
c    = ysp_rt('correlate', @GetSecs);          % GetSecs on the ysp clock

h = ysp_rt('fit_open', struct('mode', 'late', 'tick', 1e-6, 'bits', 32));
flags = ysp_rt('fit_add', h, us, t);           % pairs: counters and their times
[s, ns] = ysp_rt('fit_map', h, event_us);      % events on the ysp clock
info = ysp_rt('fit_info', h);                  % ppm, spread, points, epoch, ...
ysp_rt('fit_close', h);
```

`ysp_rt` wraps the clock that every ysp time is on. `ysp_rt('now')`,
`ysp_serial('now')` and `ysp_parallel('now')` read one counter: each MEX
file compiles its own copy of `ysp/rt.h`, and `test_mex_rt.m` checks that a
read of the other two lies between two reads of `ysp_rt('now_ns')`. On the
Windows development machine, Psychtoolbox's `GetSecs` read between two
reads of `ysp_rt('now')` in every try, so it is the same counter there; on
another machine, check with `'correlate'`.

A wait blocks MATLAB or Octave for its whole length, and Ctrl+C does not
end it. `'sleep_until'` gives the OS the wait up to the spin window before
the deadline and spins the rest. The default window is the `default_spin`
field of `ysp_rt('constants')`: 1.2 ms on Windows, 0.2 ms elsewhere.

`'correlate'` calls the clock function between two reads of the ysp clock
and keeps the tightest bracket: the true ysp time of `c.other` is within
`c.width / 2` of `c.t`. A function that returns an integer class is a
counter, and `c.other` is a `uint64`; one that returns a double is seconds.
The width includes the cost of the call back into MATLAB: 16 us for
`GetSecs` on the development machine. An error in the function is raised
with its own identifier after the header's loop ends.

`'fit_add'`, `'fit_map'` and `'fit_unwrap'` take vectors and keep their
shape. Counters are `uint64`, or doubles that hold integers below 2^53.
`'fit_add'` checks every value before the first pair goes in.

The virtual clock (`'virtual_begin'`) is in `ysp_rt`'s copy of `ysp/rt.h`
only: `ysp_serial('now')` keeps the real clock.
## ysp_dataset

Writes a session's data set from a Psychtoolbox script: the folder of
`session.json`, `trials.csv`, `frames.csv`, `events.csv`, `clock.csv` and
`streams/<name>.csv` that the ysp player writes, and a zip of it, so that
the same analysis reads a Psychtoolbox session and a player session. The
header's manual (THE FOLDER, SESSION.JSON, THE TABLES, THE ZIP) and
[docs/dataset.md](../../docs/dataset.md) are the reference for the files.

### How to write a session

```matlab
d.dir = fullfile('data', 'P07_20261021T140312Z');      % must not exist; its parent must
d.trial_cols = {'participant', 'const', 'P07';           % the same text on every row
                'trial_index', 'int',    [];
                'response',    'string', [];
                'correct',     'bool',   [];
                'rt_s',        'time',   []};             % seconds; the name ends in _s
d.session = struct('experiment', 'gabor-2afc', 'seed', 20261021);
h = ysp_dataset('open', d);
for trial = 0:n - 1
    for k = 1:n_frames
        vbl = Screen('Flip', win);
        ysp_dataset('write_frame', h, 'frame', frame, 'trial_index', trial, 'onset_s', vbl);
    end
    [t_key, key] = wait_key();                           % yours, on GetSecs
    ysp_dataset('write_event', h, 't_s', t_key, 'trial_index', trial, 'kind', 'keyboard', 'text', key);
    ysp_dataset('write_trial', h, struct('trial_index', trial, 'response', key, ...
                'correct', strcmp(key, answer), 'rt_s', t_key - t_onset));
end
rows = ysp_dataset('finish', h);                         % session.json: "closed": true
ysp_dataset('zip', h, [d.dir '.zip']);                   % or z = ysp_dataset('zip', h)
ysp_dataset('close', h);
```

A row is a 1x1 struct of column names and values, or name-value pairs. A
column that the row does not name is an empty cell, and so are `[]` and
`NaN`. Times are seconds on one clock of yours (`GetSecs`); the binding
rounds each to the nearest nanosecond, and the file holds it as seconds
with 9 decimals. A time may be negative (a residual, an SOA). The binding
checks every value of a row before it sets one, so a refused row writes
nothing.

`trials.csv` has no part that the sequencer writes itself (the header
opens it with `desc.trials` NULL), and two MEX files share no C state. To
log `ysp_trials` columns, declare them as your own and fill them from
`ysp_trials('format_row', ...)`, `'var'` and `'trial_seed'`;
`ysp_dataset('seed_text', seed)` writes a seed as `x` and 16 hex digits,
as the trials writer does. The test checks that the rows equal
`ysp_trials`' data line, text for text.

`'finish'` and `'close'` mark the session `"closed": true`, the mark of
a normal end. A session that fails is abandoned instead: every table file
closes, every row written stays, and `session.json` keeps
`"closed": false` and no row counts, as after a crash. Call `'abandon'`
from your own catch block:

```matlab
h = ysp_dataset('open', d);
try
    run_experiment(h);
catch err
    ysp_dataset('abandon', h);              % "closed": false; the handle goes
    rethrow(err);
end
ysp_dataset('finish', h);                   % "closed": true
ysp_dataset('close', h);
```

A handle that is still open when the MEX file is cleared (`clear mex`,
`clear ysp_dataset`) or when MATLAB or Octave exits is abandoned too, so a
script that stopped on an error and was never closed does not read as a
normal end. A process that is killed leaves the same files.

### Open

`h = ysp_dataset('open', d)`; the struct `d` has:

| Field | Values |
|---|---|
| `dir` | the folder, a char row (UTF-8 on every OS); required. It must not exist; its parent must |
| `trial_cols` | an N x 2 or N x 3 cell, one row per column: `{name, type}`, `{name, type, decimals}` or `{name, 'const', text}`; required |
| `session` | a 1x1 struct of your keys for `session.json`; not `format`, `version`, `closed`, `rows` or `write_errors` |
| `sync` | `true`: every row of every table also waits for the disk (milliseconds a row). Default `false`: each row reaches the OS when the call returns |

| Column type | Value | Missing |
|---|---|---|
| `'int'`, `'int64'` | an integer-valued number; `int64` and `uint64` exactly | `[]`, `NaN` |
| `'double'` | a number; decimals 0 (the shortest exact) or 1..17 | `[]`, `NaN` |
| `'bool'` (`'logical'`) | `true`, `false`, 0 or 1 | `[]`, `NaN` |
| `'string'` (`'char'`) | a char row, or a string scalar | `[]`, `''` |
| `'time'` | seconds; decimals 0 (9, exact) or 1..9; the name ends in `_s` | `[]`, `NaN` |
| `'const'` | none: the text is the third cell | |

A `session` value (and a `'session_set'` value) becomes JSON: a 1x1 struct
an object, a struct array or a cell an array, a char row a string, a
numeric or logical scalar a number or a bool, a vector an array, a matrix
an array of its rows, `[]` null. JSON has no NaN or Inf.

### Commands

| Command | Returns | Notes |
|---|---|---|
| `'version'` | binding version, and the header's as a second output | `'1.0.0'`, `'0.9.1'` |
| `'now'`, `'now_ns'` | seconds; `int64` ns | the ysp clock |
| `'real_now'` | seconds | the real clock, also on the virtual clock |
| `'sleep_until', t [, spin]` | lateness, seconds | `spin` defaults to `default_spin` |
| `'sleep', duration [, spin]` | lateness, seconds | the deadline is taken on entry |
| `'spin_until', t` | lateness, seconds | spins the whole time |
| `'clock_info'` | struct | `resolution` (seconds), `hires_timer` |
| `'constants'` | struct | `default_spin`, `virtual_t0`, `fit_points`, the `fit_*` flag bits |
| `'describe'` | char | the `yrt_describe()` line; log it beside timing data |
| `'virtual_begin' [, t0]`, `'virtual_end'`, `'is_virtual'` | now; none; logical | `ysp_rt:state` when already virtual |
| `'correlate', fn [, struct('edge', 'tries', 'timeout')]` | struct `t`, `other`, `width`, `tries`, `t_ns` | `ysp_rt:timeout` when an edge never came |
| `'user_dir', 'cache' or 'config' [, subdir]` | char | `''` means "use no folder" |
| `'fit_open', struct` | `uint64` handle | fields `mode` (`'late'`, `'bracket'`, `'unbiased'`), `tick` (seconds, required), `bits`, `bucket`, `slope_span`, `restart`, `restart_hold`, `max_width` |
| `'fit_add', h, ticks, t [, width]` | flags | `width` is one value or one per pair |
| `'fit_map', h, ticks` | seconds, and `int64` ns | `ysp_rt:state` before the first pair |
| `'fit_unwrap', h, ticks` | `uint64` | |
| `'fit_info', h` | struct | `t0`, `t0_ns`, `ticks0`, `tick`, `ppm`, `spread`, `width`, `span`, `points`, `pairs`, `epoch`, `rejected`, `pending`, `ready`, `slope` |
| `'fit_close', h` | none | |

Not bound, and why: [docs/rt.md](../../docs/rt.md#bindings).
## ysp_device and ysp_rigfile

`ysp_device` wraps [ysp/device.h](../../include/ysp/device.h): response
boxes, trigger boxes and boards as events on the ysp_rt clock, and outputs
for trigger codes. `ysp_rigfile` wraps
[ysp/rigfile.h](../../include/ysp/rigfile.h): the rig profile, which says
the device of each role, what the rig's loopbacks measured, and the
viewing geometry. `ysp_rigfile('desc', p, role)` gives the struct that
`ysp_device('open', ...)` takes, so the two MEX files stay apart, as the
headers' Python distributions do. Both compile `ysp/serial.h` and the
headers under it, and include `ysp_mex_util.h`. `ysp_device.c` also
includes `../python/ysp_device/ysp_device_bind.h` (the event queue and the
simulated boards, shared with the Python binding) and, except on macOS,
compiles `ysp/parallel.h` for the parallel-port transport.

### How-to: read a box and send triggers from the rig profile

```matlab
p = ysp_rigfile('load');                      % the user's profile.json
info = ysp_rigfile('info', p);
for i = 1:numel(info.notes), fprintf('rig profile: %s\n', info.notes{i}); end
box = ysp_device('open', ysp_rigfile('desc', p, 'resp'));
trig = ysp_device('open', ysp_rigfile('desc', p, 'trig'));
if ~ysp_device('wait_running', box, 3), error(ysp_device('error', box)); end
if ~ysp_device('wait_running', trig, 3), error(ysp_device('error', trig)); end
src = ysp_rigfile('source', p, ysp_device('info', box));   % tier 1 only when checked

ysp_device('pulse', trig, 12, 0.002);         % 12 for 2 ms, then 0
onset = ysp_device('now');
ev = ysp_device('wait', box, 2.0);            % struct array, oldest first
if ~isempty(ev), fprintf('button %d, rt %.4f s\n', ev(1).control, ev(1).t - onset); end
ysp_device('close', box); ysp_device('close', trig); ysp_rigfile('close', p);
```

Without a profile, give the desc yourself:
`struct('role', 'resp', 'family', 'xid', 'key', 'serial:0403:6001:FT4ABC12:')`.
`ysp_device('list')` gives each serial port's match key.

### ysp_device commands

| Command | Returns | Notes |
|---|---|---|
| `'open', desc` | handle | `desc`: `role`, `family` (a name or a number), `key` (required); `device`, `kind`, `baud`, `pulse`, `latched`, `min_hold`, `probe`, `retry`, `silent`, `max_width`, `no_identify`, `no_elevate`, `queue`, `rule`, `sim`. Times in seconds; `probe` and `silent` take `Inf` for none. An unknown field raises `ysp_device:arg`. `device` 0 takes the role's index from the module's role table |
| `'wait_running', h, timeout` | logical | Call it before the first output. `Inf` waits without end (1.0.1). `ysp_device:failed` when the device ends FAILED |
| `'poll', h [, max]`, `'wait', h, timeout [, max]` | struct array | Events, oldest first: `t` (s), `t_ns` (int64), `device`, `control`, `code`, `mods`, `kind`, `type`, `flags`, `stamp`, `unc` (s), `x`, `y`, `value`, `aux`, `ticks`. A 1x0 struct with these fields when there is none |
| `'pending', h`, `'clear', h` | double | The events queued; drop them |
| `'set', h, code`, `'pulse', h, code, width`, `'mark', h, code [, text]` | none | `ysp_device:state` before RUNNING, `:arg` for a code or width the family cannot give, `:family` without outputs, `:io` |
| `'last_out', h` | struct | `t_before`, `t_after`, `width`, `code`, `seq`, `flags`, `result` |
| `'stats', h`, `'state', h`, `'source', h`, `'info', h`, `'error', h` | struct, `[state, name]`, struct, struct, string | `info` has `role`, `family` and the rest of the opened desc, for `ysp_rigfile('source', ...)` |
| `'close', h` | none | `ysp_device:order` while an open rule writes to the device |
| `'rule_open', out, choices [, opts]` | handle | `choices`: a struct array with `kind` (default 5, a box), `device`, `control`, `code`. `opts`: `width` (s), `max_fires` (`Inf`: no limit; default 1) |
| `'rule_arm', r, start [, until]`, `'rule_window', ...`, `'rule_disarm', r` | none | The window `[start, until)` in seconds on `'now'`; `until` defaults to `Inf` |
| `'rule_info', r`, `'rule_fires', r`, `'rule_close', r` | struct, struct array, none | `choice` is 1-based, `[]` before the first fire |
| `'sim_open', family [, opts]` | handle | A simulated board for `desc.sim`: `ppm`, `frame`, `tail`, `key`, `identify` |
| `'sim_edge', s, t, level [, ch]`, `'sim_press', s, t, duration [, ch]`, `'sim_unplug', s, start, stop` | none | The board's input and faults, at times on `'now'` |
| `'sim_writes', s`, `'sim_lines', s`, `'sim_key', s`, `'sim_opens', s`, `'sim_close', s` | | What the host wrote; the board's key |
| `'now'`, `'list'`, `'key_match', p, k`, `'family', f`, `'roles'`, `'constants'`, `'version'` | | `constants` has the families, the states, `ERR_*`, `OUT_*`, `KIND_*`, `PRESS`, `RELEASE`, the flags and `TIER_*` |

Close in this order: the input that a rule watches, the rule, then the
output. `clear ysp_device` and interpreter exit close the rest in that
order. The reader threads never call into MATLAB or Octave. The events
wait in a C ring of `queue` events (default 1024); a full ring drops new
events and counts them (`stats.queue_dropped`). A MEX call owns the
interpreter, so Ctrl+C cannot interrupt a `'wait'`: give a finite timeout
and loop.

### ysp_rigfile commands

| Command | Returns | Notes |
|---|---|---|
| `'load' [, name]`, `'load_file', path`, `'parse', text [, dir]`, `'new' [, rig]` | handle | `ysp_rigfile:missing` (no file), `:folder` (no rig folder, or one that another user can write), `:json`, `:format` (the message names the field) |
| `'info', p` | struct | `rig`, `written`, `dir`, `notes` (cell), `hash`, `ppd`, `view`, `display`, `roles` (a struct array; times in seconds, `[]` for none) |
| `'role', p, name`, `'desc', p, role` | struct | `desc` is the struct for `ysp_device('open', ...)`. It raises `:format` for an unbound or `lsl` role |
| `'source', p, role_or_info` | struct | The tier rule by role name, or for `ysp_device('info', h)` |
| `'text', p`, `'hash', p` | string | The canonical JSON and its SHA-256 |
| `'bind', p, role, family, key`, `'remove', p, role`, `'set_view', p, dist_mm, width_mm, width_px`, `'clear_view', p` | none, logical | Changes in memory |
| `'save', p [, name]`, `'save_file', p, path`, `'close', p` | none | |
| `'dir'`, `'path' [, name]`, `'check_name', c`, `'version'` | string | |

Under Octave, `clear` runs the exit hook of a MEX file but keeps the
library loaded. Since 2026-10-25 `ysp_mex_util.h` hooks again after a
clear. Before, the handles opened after a clear stayed open, and their
threads crashed Octave at exit.
| `'open', d` | `uint64` handle | makes the folder; `session.json` says `"closed": false` |
| `'write_trial', h, row` | none | one row of `trials.csv`; `row` is a struct or name-value pairs |
| `'write_frame', h, row` | none | one row of `frames.csv` (`'columns', 'frames'`) |
| `'write_event', h, row` | none | one row of `events.csv` (`'columns', 'events'`) |
| `'write_clock', h, row` | none | one row of `clock.csv` (`'columns', 'clock'`) |
| `'stream', h, name, cols` | id (1-based) | makes `streams/<name>.csv`; `cols` as `trial_cols` |
| `'write_stream', h, id or name, row` | none | one row of a stream |
| `'session_set', h, key, value` | none | `session.json`'s key; one atomic rewrite (milliseconds: between trials) |
| `'session_push', h, key, value` | none | appends to the array at key (made when absent); one atomic rewrite |
| `'rows', h` | struct | `trials`, `frames`, `events`, `clock`, `streams.<name>`: rows written |
| `'finish', h` | struct | as `'rows'`; closes the tables, `session.json` says `"closed": true` |
| `'zip', h [, path]` | `uint8` row, or none | the finished folder as one stored zip |
| `'info', h` | struct | `dir`, `is_open`, `finished`, `streams`, `trial_cols` |
| `'close', h` | none | finishes a data set that is open, then frees the handle |
| `'abandon', h` | none | an abnormal end: closes the files, leaves `session.json` `"closed": false`, frees the handle |
| `'columns', 'frames' \| 'events' \| 'clock'` | cell of names | the fixed tables' columns |
| `'seed_text', seed` | char | a `uint64` seed as `x` and 16 hex digits |
| `'version'` | char | the MEX function's version (1.0.0); the second output is ysp/dataset.h's |

Error ids: `ysp_dataset:arg`, `:exists` (the folder is there), `:io` (a
file could not be made, written or renamed), `:closed`, `:order` (`'zip'`
before the finish), `:full` (16 streams), `:range` (a zip of 4 GiB or
more), `:memory`, `:usage`, `:handle`.

A row costs one write call, about 12 us in C (docs/dataset.md); the
binding allocates nothing per row once its text buffer has grown to the
longest row. Not bound: the header's adapters (`yds_frame_from_screen()`
and the others take structs of headers with no MEX binding; the fields of
`'write_frame'`, `'write_event'` and `'write_clock'` are theirs), struct
field (bound) and `CHARS` columns (every column is set per row, which also
gives a missing `bool`), and a reader (the header has none; use
`readtable`).

## Adaptive methods: common rules

`ysp_stair`, `ysp_quest`, `ysp_aep` and `ysp_trials` share these rules.

- **Descs are structs.** `open` takes one 1x1 struct whose fields are the
  C desc's fields, with the same names. A missing field, or `[]`, is the
  library default, as a zero is in C. An unknown field name raises
  `ysp_<name>:arg`, so a typo cannot leave a default in force. An enum field
  is a string, and case does not matter: `'log'`, `'gumbel'`, `'eavc'`.
- **Indices are 1-based.** Stimuli, parameter axes, candidates, conditions,
  factors, levels, tracks, trials and reversals are numbered from 1. Where C
  uses -1 for "none", the binding uses 0. `rep` and `block` follow the same
  rule: 0 is no repetition, or a practice trial.
- **Outcomes are values.** A response keeps the header's numbering: 1 is
  correct or yes, 0 is incorrect or no, and 0..K-1 in general.
  `ysp_trials` keeps -1 (no valid response) and -2 (re-queued).
- **Histories are structs of columns.** `history` returns one struct with
  one column per field of the C trial record. `struct2table(history)` makes
  a MATLAB table from it.
- **Snapshots are bytes.** `save` returns a `uint8` row vector. The
  module command `load(bytes, desc)` returns a new handle. See
  [How to save and resume a session](#how-to-save-and-resume-a-session).
- **Generators.** `desc.rng` is a function handle that returns a uniform
  number in [0, 1), or an integer seed (a `uint64`, or a double below
  2^53). A seed drives a splitmix64 generator that the module keeps, the
  same generator as `ytr_splitmix()`. `rng_state` reads and writes its
  state. A seed never calls the interpreter, so the async layers accept it.
- **Errors.** Every header code becomes an error identifier
  `ysp_<name>:<code>`: `arg`, `closed`, `full`, `memory`, and per module
  `order` (trials), `nocross` and `numeric` (aep), `busy` and `timeout`
  (quest, aep). The binding adds `handle` (a closed or unknown handle),
  `usage` (a bad command or argument count), `open` and `load` (the header
  refused the desc or the snapshot; the message is the header's),
  `callback` (a MATLAB callback returned the wrong shape) and `async` (the
  async thread did not start). An error in a MATLAB callback keeps the
  callback's own identifier.
- **Cleanup.** `close` frees a handle. `clear ysp_<name>`, `clear mex` and
  interpreter exit free every open handle and stop every async thread.

### Explanation: MATLAB callbacks and the header

A header calls a callback (a `pf_batch`, an `rng`, a track's done test)
from inside its own loops, and it cannot stop there. A MEX error at that
point would leave the handle half updated. So the binding traps every
callback error, gives the header a neutral value for the rest of that call,
and raises the error when the header call has returned. The handle stays
consistent and usable.

In MATLAB the trap is `mexCallMATLABWithTrap`. Octave's version of that
function replaces the callback's error with `Octave:MEX`. So under Octave
the binding runs the callback inside `cellfun` with an `ErrorHandler`, which
keeps the original identifier and message.

A callback must not call the handle that it runs under. That raises
`ysp_<name>:busy`.

## ysp_stair

```matlab
h = ysp_stair('open', struct('start', 0.5, 'n_down', 3, 'step_type', 'log', ...
              'steps', [0.3 0.15 0.075], 'min', 0.001, 'max', 1, 'stop_reversals', 10));
while ~ysp_stair('done', h)
    x = ysp_stair('next', h);                  % the proposed level
    ysp_stair('update', h, x, run_trial(x));   % the level shown, and 1 or 0
end
thr = ysp_stair('estimate', h, 'reversals');
ysp_stair('close', h);
```

Desc fields: `start`, `rule` (`'updown'`, `'asa'`), `n_up`, `n_down`,
`step_type` (`'lin'`, `'log'`, `'db'`), `steps` (a vector of up to 16: the
schedule), `step_down_scale`, `target_p`, `min`, `max`, `use_limits`,
`harder_is_up`, `initial_rule`, `stop_reversals`, `stop_trials`,
`stop_at_limit`, `est_reversals`, `est_trials`. The manual of
[ysp/stair.h](../../include/ysp/stair.h) gives the semantics.

| Command | Returns | Notes |
|---|---|---|
| `'open', desc` | handle | |
| `'next', h` | double | the proposed level; the same until `update` |
| `'update', h, level, response` | event mask (optional) | bit 1 step, bit 2 reversal, bit 4 done |
| `'done', h` | logical | |
| `'stop_reason', h` | string | `'none'`, `'reversals'`, `'trials'`, `'limit'`, `'full'` |
| `'estimate', h [, how]` | double | `how`: `'reversals'` (default), `'trials'`, `'median_rev'`, `'last'`; NaN when the window is empty |
| `'estimate_count', h [, how]` | double | the number of values the estimate averages |
| `'n_trials', h`, `'n_reversals', h` | double | |
| `'history', h` | struct of n x 1 columns | `proposed`, `shown`, `response`, `reversal`, `direction`, `step_index` (1-based into `steps`) |
| `'reversal_level', h [, i]` | double or column | the level of reversal `i`, or of every recorded reversal |
| `'reversal_trial', h [, i]` | double or column | the 1-based trial of reversal `i`, or of every one |
| `'save', h` | `uint8` row | |
| `'load', bytes, desc` | handle | resumes without replaying the updates |
| `'close', h` | none | |
| `'weighted_scale', p` | double | `(1 - p) / p` |
| `'convergence_p', n_up, n_down [, scale]` | double | NaN when both counts are above 1 |
| `'simulate_response', p, u` | 0 or 1 | 1 when `u < p` |
| `'version'`, `'strerror', code` | string | |

## ysp_quest

```matlab
d = struct();
d.stim  = {{-3, 0, 31}};                       % one axis: a linspace
d.param = {{-3, 0, 61}, {0.5, 6, 12}, 0.5, 0.02};
d.stop_trials = 60;
h = ysp_quest('open', d);
while ~ysp_quest('done', h)
    [i, x] = ysp_quest('next', h);             % the grid index and its value
    ysp_quest('update', h, i, run_trial(10 ^ x));
end
est = ysp_quest('estimate', h, 'mean');
ysp_quest('close', h);
```

An axis is a numeric vector of explicit values (a scalar is a fixed
parameter), or a cell `{lo, hi, n}` for a linspace. `stim` and `param` are
cell arrays with one axis each; a bare numeric vector is one axis. So one
linspace stimulus axis is `{{lo, hi, n}}`.

Desc fields: `stim`, `param`, `prior` (a cell with one weight vector per
parameter axis, `[]` for uniform), `nuisance` (a 0/1 vector, one entry per
parameter axis), `joint_prior` (P weights in the shape that `posterior`
returns), `pf` (`'gumbel'`, `'weibull'`, `'logistic'`, `'normal'`,
`'hypsec'`, `'custom'`), `pf_batch`, `n_outcomes`, `select` (`'entropy'`,
`'quantile'`, `'mean'`, `'mode'`), `select_param` (1-based),
`select_quantile`, `tiebreak` (`'lowest'`, `'nearest'`, `'alternate'`,
`'random'`), `tie_tolerance`, `rng`, `subset_size`, `stop_trials`,
`stop_entropy`, `stop_sd`, `stop_sd_param` (1-based), `no_table`. The
binding does not expose `desc.memory`.

For Psi-marginal, give the nuisance axes a few points and flag them:

```matlab
d.param = {{-3, 0, 61}, {0.5, 6, 12}, 0.5, [0 0.02 0.04 0.06]};
d.nuisance = [0 0 0 1];
```

For QUEST, use a Gaussian prior on the threshold and quantile placement:

```matlab
d.param = {{-3, 0, 61}, 3.5, 0.5, 0.01};
d.prior = {ysp_quest('prior_normal', {-3, 0, 61}, -1.5, 0.5), [], [], []};
d.select = 'quantile';
```

### A custom model: pf_batch

`pf_batch` is a function handle `p = f(stim, P)`. `stim` is a 1 x n_stim
row. `P` is a P x n_param matrix with one parameter point per row. The
function returns a P x K matrix: one row per row of `P`, and each row sums
to 1 within 1e-6.

```matlab
d.pf_batch = @(s, P) [1 - pc(s(1), P), pc(s(1), P)];   % K = 2
```

The header calls it once per stimulus at `open` (S calls), and once for
each off-grid evaluation: `update_values`, `p`, `p_values` and `simulate`.
The rows of `P` are the header's walk of the grid, which is permuted when a
nuisance axis is not last, so treat them as a list of points. The binding
converts the grid's matrix once and uses it again. `pf_batch` is slow only
under `no_table`: then the header calls it for every stimulus on every
`next`, which moves the whole table through the interpreter on each trial.
Do not do that. When `pf_batch` is set, `pf` is `'custom'` and `n_outcomes`
defaults to 2. The binding does not expose the per-cell `pf_fn`.

### Commands

| Command | Returns | Notes |
|---|---|---|
| `'open', desc` | handle | builds the table: S*P evaluations |
| `'next', h` | `[i, stim]` | the 1-based stimulus index and its values |
| `'next_subset', h, indices` | `[i, stim]` | only the stimuli in `indices` |
| `'expected_entropy', h [, indices]` | column | bits; every stimulus when `indices` is not given |
| `'update', h, i, outcome` | none | |
| `'update_values', h, stim, outcome` | none | a stimulus off the grid; recorded with index 0 |
| `'done', h`, `'stop_reason', h` | logical, string | `'none'`, `'trials'`, `'entropy'`, `'sd'`, `'full'` |
| `'estimate', h [, how]` | row, one value per parameter | `'mean'` (default), `'mode'`, `'median'` |
| `'quantile', h, axis, p`, `'sd', h, axis` | double | the marginal of one parameter |
| `'marginal', h, axis` | column | |
| `'posterior', h` | n_1 x n_2 x ... array | element `(i1, i2, ...)` is the mass at those axis indices, in the caller's axis order |
| `'entropy', h` | double | bits, over the axes that are not nuisance |
| `'p', h, i, params`, `'p_values', h, stim, params` | row of K | the model at any parameter values |
| `'simulate', h, i, params, u` | outcome | `u` in [0, 1) from your generator |
| `'stim_value', h, i [, axis]`, `'stim_values', h, i` | double, row | |
| `'stim_index', h, subs`, `'stim_nearest', h, values` | index | |
| `'param_value', h, t [, axis]`, `'param_values', h, t`, `'param_index', h, subs` | | the same for the parameter grid |
| `'n_stim', h`, `'n_param', h`, `'n_outcomes', h` | double | S, P, K |
| `'stim_shape', h`, `'param_shape', h` | row | points per axis |
| `'n_trials', h`, `'history', h` | double, struct | `stim` (n x n_stim), `stim_index`, `proposed_index`, `outcome` |
| `'save', h` | `uint8` row | |
| `'load', bytes, desc` | handle | rebuilds the table: the cost of one `open` |
| `'rng_state', h [, state]` | `uint64` | seed generators only |
| `'close', h` | none | stops a running async session first |
| `'memory_size', desc` | double | arena bytes; 0 for a desc that `open` refuses; calls no callback |
| `'prior_normal', axis, mean, sd` | row | Gaussian weights over the points of an axis |
| `'version'`, `'strerror', code` | string | |

Async commands (see
[How to run the inference on a thread](#how-to-run-the-inference-on-a-thread)):

| Command | Returns | Notes |
|---|---|---|
| `'async_start', h [, opts]` | none | `opts`: `estimator`, `below_normal`, `pin_cpu`, `queue_depth` |
| `'async_submit', h, i, outcome` | seq | `ysp_quest:busy` when the queue is full |
| `'async_submit_values', h, stim, outcome` | seq | refused with a MATLAB `pf_batch` |
| `'async_poll', h` | snapshot struct | `seq`, `proposed` (1-based), `stim`, `update_rc`, `n_trials`, `done`, `stop`, `estimate`, `entropy`, `sd` (of parameter 1) |
| `'async_wait', h, seq [, timeout_s]` | snapshot struct | the default timeout is 1 s; `ysp_quest:timeout` |
| `'async_pending', h` | double | queued responses; takes the queue mutex |
| `'async_policy', h` | string | `'normal'`, `'below_normal'`, `'none'` |
| `'async_running', h` | logical | |
| `'async_stop', h` | none | drains the queue and joins the thread |

## ysp_aep

```matlab
d = struct('lo', [0 -3], 'hi', [1.5 0], 'intensity_dim', 2, 'acq', 'eavc', ...
           'target_p', 0.75, 'grid', [15 25], 'n_init', 10, ...
           'fit', true, 'fit_every', 10, 'stop_trials', 150);
h = ysp_aep('open', d);
while ~ysp_aep('done', h)
    x = ysp_aep('next', h);                     % 1 x n_dims
    ysp_aep('update', h, x, run_trial(x));      % the stimulus shown
end
[thr, lo, hi] = ysp_aep('threshold', h, 0.7);   % at log10 SF 0.7
ysp_aep('close', h);
```

The desc fields are those of `yaep_desc`. `n_dims` is `numel(lo)`.

| Field | Value |
|---|---|
| `lo`, `hi` | vectors, one entry per dimension |
| `intensity_dim` | 1-based; the default is 1 |
| `lik` | `'bernoulli'` (default), `'ordinal'`, `'categorical'`, `'gaussian'`, `'pairwise'` |
| `model`, `kernel`, `link` | `'gp'` or `'psychometric'`; `'rbf'` or `'semip'`; `'probit'` or `'logit'` |
| `n_outcomes`, `target_outcome` | K; the outcome value that the level set is about |
| `guess`, `lapse`, `target_p`, `target_value`, `acq_beta` | doubles |
| `hyper`, `hyper_min`, `hyper_max` | structs with `lengthscale` (vector), `outputscale`, `mean`, `lengthscale_b`, `outputscale_b`, `cutpoint` (vector), `noise_sd`, `lengthscale_g`, `outputscale_g`, `mean_g`; a missing field is 0 |
| `priors` | a struct with `lengthscale`, `outputscale`, `outputscale_b`, `outputscale_g`, `mean`, `mean_g`, `noise_sd`; each is `[center sd]`, `[center sd ceiling]` or a struct with those fields |
| `fit`, `fit_every`, `fit_max_evals`, `fit_tol`, `fit_pcg`, `no_hyper_prior`, `refit_every`, `jitter`, `pcg_threshold` | fit and solver settings |
| `acq` | `'lse'` (default), `'eavc'`, `'localmi'`, `'balv'`, `'bald'`, `'random'`, `'ucb'`, `'ei'`, `'thompson'` |
| `minimize` | logical |
| `n_init`, `rng` | Halton trials first; a generator |
| `candidates` | an M x n_dims matrix; the binding keeps a copy |
| `n_candidates`, `grid` | a Halton set of M points; or points per dimension |
| `refine_steps` | golden-section refinement of the grid winner |
| `dim_kind`, `dim_levels` | a cell of `'continuous'`, `'integer'`, `'categorical'`; level counts |
| `monotone_dims` | a vector of 1-based dimensions |
| `stop_trials`, `stop_threshold_sd`, `stop_context`, `max_trials` | stop rules and the trial ceiling |

The binding does not expose `desc.memory`.

| Command | Returns | Notes |
|---|---|---|
| `'next', h` | `[x, i]` | `i` is the 1-based candidate, 0 for a Halton, rng or refined point |
| `'next_subset', h, indices` | `[x, i]` | skips the init phase |
| `'acq_score', h [, indices]` | column | NaN in the init phase; every candidate when `indices` is not given |
| `'update', h, x, outcome`, `'update_real', h, x, y` | rc (optional) | see the note on `numeric` below |
| `'next_pair', h` | `[x1, x2]` | `'pairwise'` |
| `'update_pair', h, x1, x2, outcome` | rc (optional) | 1 when `x1` was preferred |
| `'predict_pair', h, x1, x2` | double | P(`x1` is preferred) |
| `'done', h`, `'stop_reason', h` | logical, string | `'none'`, `'trials'`, `'threshold_sd'`, `'full'`. `done` runs a threshold search when `stop_threshold_sd` is set |
| `'predict_f', h, x [, latent]` | `[mu, sd]` | `latent` is 1-based: a categorical class, or 1 = threshold and 2 = log slope under `'psychometric'` |
| `'predict_p', h, x`, `'predict_p_var', h, x` | double | the target quantity and its variance |
| `'predict_outcomes', h, x` | row of K | |
| `'predict_p_many', h, X` | column | `X` is n x n_dims, one point per row |
| `'predict_f_many', h, X [, latent]` | `[mu, sd]` columns | |
| `'threshold', h, ctx [, target]` | `[x, lo, hi]` | `ctx` has n_dims - 1 values; leave it out for 1-D; `ysp_aep:nocross` |
| `'threshold_multi_cross', h` | logical | |
| `'argmax', h` | `[x, value, i]` | |
| `'fit', h` | logical | true when the fit converged |
| `'fit_step', h` | logical | true while another step helps |
| `'fit_delta', h`, `'refit', h`, `'log_marginal', h` | | |
| `'get_hyper', h`, `'get_priors', h` | struct | |
| `'n_candidates', h`, `'candidate', h [, i]` | double, row or M x n_dims | |
| `'n_trials', h`, `'n_dims', h` | double | |
| `'history', h` | struct | `x` (n x n_dims), `y`, `proposed`, `init`, and `x2` under `'pairwise'` |
| `'save', h`, `'load', bytes, desc` | `uint8` row, handle | |
| `'rng_state', h [, state]` | `uint64` | seed generators only |
| `'close', h` | none | |
| `'simulate_outcome', p, u` | outcome | the smallest k with cumulative p > u |
| `'memory_size', desc`, `'version'`, `'strerror', code` | | |

`update` records the trial also when the Cholesky fails
(`YAEP_ERR_NUMERIC`), and the previous posterior stays in force. With an
output (`rc = ysp_aep('update', ...)`) the binding returns that code, -6.
Without an output it raises `ysp_aep:numeric`, so the failure cannot pass
unnoticed.

The async commands are those of `ysp_quest`, with these differences.
`async_start` takes the `opts` fields `context` (n_dims - 1 values),
`target`, `fit_in_idle`, `below_normal` and `pin_cpu`. The submits are
`async_submit(h, x, outcome)` and `async_submit_real(h, x, y)`. The
snapshot has `seq`, `x`, `proposed`, `update_rc`, `n_trials`, `done`,
`stop`, `threshold`, `threshold_lo`, `threshold_hi`, `threshold_rc`,
`multi_cross`, `hyper`, `log_marginal`, `numeric` and `fitting`.
`async_start` refuses a `'pairwise'` GP.

## ysp_trials

```matlab
d = struct('reps', 20, 'order', 'constrained', 'rng', uint64(20260923));
d.factors = {{'orientation', 2}, {'contrast', 5}};
d.constraints = ysp_trials('max_run', 'orientation', 'any', 3);
h = ysp_trials('open', d);
ti = ysp_trials('next', h);
while ~isempty(ti)
    ysp_trials('update', h, run_trial(ti.levels));   % levels: 1-based, per factor
    ti = ysp_trials('next', h);
end
ysp_trials('close', h);
```

Desc fields: `n_conditions`, `factors` (a cell of `{name, n_levels}` pairs,
or an n x 2 cell), `reps`, `cond_reps`, `order` (`'sequential'`,
`'random'`, `'full_random'`, `'constrained'`), `constraints`, `max_swaps`,
`tracks`, `track_weights`, `interleave` (`'random'`, `'round_robin'`),
`track_rate`, `block_size`, `constraints_span_blocks`, `n_practice`,
`n_warmup`, `warmup_conditions` (1-based), `requeue_gap`, `rng`,
`record_size`, and from v0.2: `table`, `order_list` (1-based), `draws`,
`weights`, `subset`, `groups`, `rules` and `participant`; from v0.2.1,
`jitters`; from v0.6, `seed`, `vars` and `display_rate`. `order` also
takes `'list'` and `'with_replacement'`.

**Tables and rules (v0.2).** `desc.table` is CSV text, or
`struct('csv', text, 'types', struct('n', 'integer'), 'delimiter', ';',
'allow_empty', true)`. Its rows are the conditions and its columns the
factors, so a constraint names a column and a level by its value:

```matlab
csv = fileread('conditions.csv');          % ysp/table.h reads no files here
d = struct('table', csv, 'participant', 7, 'rng', uint64(20261007));
d.rules = sprintf(['order constrained\nreps 4\nwhere list=@participant\n' ...
                   'max_run target 3\nfollowed_by cue valid probe\n']);
h = ysp_trials('open', d);
ti = ysp_trials('next', h);
row = ysp_trials('values', h, ti.condition);  % struct: one field per column
```

`desc.rules` is the header's rules text (ysp/trials.h, GRAMMAR), applied
after the other fields. `desc.participant` is the participant number for
`@participant`, `latin` and `balanced_latin`. `desc.groups` is
`struct('factor', 'block', 'mode', 'blocked', 'order', 'balanced_latin')`;
`mode` is `'blocked'` or `'alternate'`, `order` is `'sequential'`,
`'random'`, `'latin'`, `'balanced_latin'` or `'list'` (then `list` holds
1-based levels or a cell of values). MATLAB holds text as UTF-16, and the
binding converts it to UTF-8 before the parse; Octave's text is UTF-8
already.

**Jitter (v0.2.1).** `desc.jitters` is a struct array from
`ysp_trials('uniform', name, lo, hi [, rate])`,
`ysp_trials('choice', name, values [, rate])` and
`ysp_trials('exponential', name, lo, hi, scale [, rate])`: a duration in
seconds drawn per trial from the session's generator, logged in
`format_row` and reproduced by `restore` and `load`. `lo`, `hi` and `scale`
may be a table column's name; `rate` is `NUM` or `[NUM DEN]` Hz and snaps the
draws to whole frames. A jitter's column holds seconds, so its name ends
in `_s`, as ysp/trials.h's data writer requires.

To snap a jitter to the rig's refresh, give `rate` as `'display'` (rules
text: `rate=display`), and set `desc.display_rate` to the rate that the
screen reports, `NUM` or `[NUM DEN]` Hz, for example `[60000 1001]`. Without
`display_rate` the session does not open. `format_rules` keeps
`rate=display`; `format_meta` gives the rate of the draws (v0.6).

```matlab
d.jitters = [ysp_trials('exponential', 'fp_s', 0.5, 2.0, 0.4, [60000 1001]), ...
             ysp_trials('uniform', 'iti_s', 0.8, 1.2)];
h = ysp_trials('open', d);
ti = ysp_trials('next', h);
fp = ysp_trials('jitter', h, ti.index, 'fp_s'); % fp.s, fp.ns (int64), fp.frames
```

**Variables (v0.6).** A variable is a category drawn per trial, for
example the side of a 2AFC target:

```matlab
d.seed = uint64(20261021);                  % the trial streams' seed
d.vars = ysp_trials('var_choice', 'side', {'left', 'right'});
h = ysp_trials('open', d);
ti = ysp_trials('next', h);
side = ysp_trials('var', h, ti.index, 'side');     % 'left' or 'right'
noise_seed = ysp_trials('trial_seed', h, ti.index); % uint64, for a stimulus generator
```

A value comes from the trial's own stream, which `seed` and the trial index
make. It draws nothing from `rng`, so a variable that you add or remove does
not change the schedule or the jitters. `restore` and `load` give every
value again. Each value is equally likely; repeat a value to weight it.
`desc.vars` is a struct array (`[v1 v2]`) of at most 4 variables, each with
1 to 32 values of 1 to 63 bytes. The rules text takes `var side choice left
right`. `format_row` writes one column per variable, after the jitters, and
then `trial_seed` when the session has variables or a nonzero `seed`: the
seed as `x` and 16 lowercase hex digits (`x00f3a9c4e2b7d158`). That column
is text, so all 64 bits read back; `ysp_trials('table', ...)` reads it as a
string column. `seed` is a `uint64`, or a double that holds an integer
below 2^53.
The binding converts the values and the rules text to UTF-8, as it does a
table. Text that is not ASCII (values, table levels, rules, error messages)
comes back intact in Octave 10.1 and in MATLAB R2023a, also with MATLAB's
`DefaultCharacterSet` set to windows-1252 (2026-10-22, Windows).

**Constraints** come from module commands. Put them in `desc.constraints`
as a struct array (`[c1 c2]`) or as a cell. A factor is a 1-based index, a
factor name, or `'condition'` for the condition row. A level is 1-based, or
`'any'`.

| Command | Constraint |
|---|---|
| `'max_run', factor, level, n` | at most `n` consecutive trials with that level |
| `'max_in_window', factor, level, window, n` | at most `n` in any `window` consecutive trials |
| `'min_gap', factor, level, gap` | at least `gap` other trials between two |
| `'no_transition', factor, from, to` | `from` is never directly followed by `to` |
| `'first_not', factor, level` | the first main trial does not have that level |
| `'followed_by', factor, level, next_level` | every trial with `level` is directly followed by one with `next_level` (v0.2) |
| `'preceded_by', factor, level, prev_level` | every trial with `level` is directly preceded by one with `prev_level` (v0.2) |
| `'chunk', factor` | each run of rows with one value of `factor` runs as a unit (v0.2) |
| `'balance', factor [, 'no_repeat'] [, 'no_leadin']` | every ordered pair of levels adjacent equally often (v0.2) |

**Tracks.** A track is `{'stair', h}`, `{'quest', h}` or `{'aep', h}`, a
handle of one of the other modules, or a function handle that returns true
when the track is done. The header asks each live track whether it is done
from inside `next` and `done`. The binding answers with
`ysp_stair('done', h)` (or quest, aep). A closed track handle raises that
module's error, for example `ysp_stair:handle`, from `next`.

Why a call through the interpreter: the modules are separate shared
libraries, each with its own handle table, so `ysp_trials` cannot read the
table of `ysp_stair` in C. A shared table needs symbols exported across MEX
files, and neither MATLAB nor Octave supports that portably. The call is
safe, because the done test runs on the interpreter thread, inside the call
that the script made. It costs one MEX call per live track per trial.

**Records.** With `record_size` > 0 the binding owns a buffer of
`record_size` x 4096 bytes. `update(h, outcome, rec)` copies the raw bytes of
any numeric array of at most `record_size` bytes, and fills the rest with
zeros. `record(h, i)` returns them as `uint8`, and `typecast` gets the value
back:

```matlab
ysp_trials('update', h, r, level);                       % a double: 8 bytes
level = typecast(ysp_trials('record', h, i), 'double');
```

| Command | Returns | Notes |
|---|---|---|
| `'open', desc` | handle | an impossible design fails here with `ysp_trials:open` |
| `'next', h` | struct, or `[]` when done | `index`, `condition` (0 for a track trial), `track` (0 for a condition trial), `rep`, `block` (0 for practice), `levels` (1-based, per factor), `first_in_block`, `after_break`, `practice`, `warmup`, `requeued`. The same trial until `update` or `requeue` |
| `'update', h, outcome [, record]` | none | `outcome` >= 0, or -1 for no valid response |
| `'requeue', h` | none | instead of `update`: schedule the condition again |
| `'mark_break', h` | none | a break was taken |
| `'done', h` | logical | asks the tracks |
| `'level', h, c, f`, `'levels', h, c` | double, row | 1-based |
| `'condition_from_levels', h, levels` | double | |
| `'condition_at', h, slot`, `'schedule', h`, `'n_scheduled', h` | | the schedule, for preloading stimuli |
| `'n_valid', h, c`, `'count', h, c [, outcome]`, `'proportion', h, c [, outcome]` | double | tallies; `outcome` defaults to 1 |
| `'history', h` | struct of columns | `condition`, `track`, `rep`, `block`, `outcome`, `flags`, and the logicals `practice`, `requeued`, `done`, `warmup`, `after_break`, `first_in_block`, `violation`, `leadin` |
| `'record', h, i` | `uint8` row | |
| `'format_header', h`, `'format_row', h, i`, `'format_meta', h` | string with a newline | the header's CSV, in the header's own numbering (0-based, -1 for none) |
| `'save', h`, `'load', bytes, desc` | `uint8` row, handle | |
| `'restore', h, outcomes [, records]` | none | replay; `records` is an n x record_size `uint8` matrix |
| `'rng_state', h [, state]` | `uint64` | seed generators only |
| `'n_conditions'`, `'n_factors'`, `'n_run'`, `'n_done'`, `'swaps'`, `'record_size'` (each with `h`) | double | |
| `'close', h` | none | |
| `'format_rules', h` | string | the session's settings as rules text (v0.2) |
| `'jitter', h, i, j`, `'jitter_names', h` | struct, cell | trial `i`'s draw of jitter `j` (1-based, or a name): `s`, `ns` (int64), `frames` (-1 unless snapped); the names (v0.2.1) |
| `'uniform'`, `'choice'`, `'exponential'`, `'jitter_map', j, u` | struct | a jitter for `desc.jitters`; a variate mapped with no session (v0.2.1). `rate` may be `'display'` (v0.6); `jitter_map` refuses it, because only a session has a display rate |
| `'var', h, i, j`, `'var_index', h, i, j`, `'var_names', h` | string, double, cell | trial `i`'s value of variable `j` (1-based, or a name); the 1-based index of that value in the variable's values; the names (v0.6) |
| `'trial_seed', h, i` | `uint64` | trial `i`'s 64-bit seed, a function of `desc.seed` and `i` only, for a stimulus generator of the trial's own (v0.6) |
| `'var_choice', name, values` | struct | a variable for `desc.vars`; `values` is a cell of strings (v0.6) |
| `'limits'` | struct | the build's bounds: `max_trials`, `max_conditions`, `max_factors`, `max_constraints`, `max_tracks`, `max_jitters`, `max_jitter_values`, `max_vars`, `max_var_values`, `max_var_text`, `max_columns` (256 from v0.6) |
| `'values', h, c` | struct | condition `c`'s table row, one field per column (v0.2) |
| `'table_info', h`, `'table', csv` | struct | `columns`, `types`, `n_rows`, `n_skipped`, `levels`, `values` (a cell), `hash` (v0.2). At most 256 columns (64 before v0.6) |
| `'latin', n, row [, balanced]` | `[items, rows]` | row `row` (0-based, taken modulo `rows`) of a cyclic or Williams square, 1-based items (v0.2) |
| `'splitmix', state` | `[u, state]` | one step of the seed generator |
| `'version'`, `'strerror', code` | string | |

## ysp_timeline

Events, tracks and tweens on a frame clock, for a Psychtoolbox script that
draws its own stimuli: build a trial, then on each frame evaluate it at the
frame's predicted onset (`vbl + ifi`) and read the channel values, the
fields and the events that landed. The timeline reads no clock. Every time
is seconds; a landing record also gives the exact ns as `int64`.
[example_timeline.m](example_timeline.m) runs a trial and a rewind;
[test_mex_timeline.m](test_mex_timeline.m) prints `PASS` with the number
of checks. [docs/timeline.md](../../docs/timeline.md#bindings) has the
per-frame cost.

```matlab
h = ysp_timeline('open', struct('channels', 3));        % FIX, GRATING, CONTRAST
q = ysp_timeline('seq', h, 1);                           % base 1, cursor at 0
ysp_timeline('on', q, 1); ysp_timeline('wait', q, 0.5); ysp_timeline('off', q, 1);
ysp_timeline('on', q, 2); ysp_timeline('trigger', q, 12);
ysp_timeline('to', q, 3, 0.5, 'duration', 0.1, 'ease', 'cosine');
ysp_timeline('anchor', h, 1, vbl + ifi);                 % trial time 0
[v, ev] = ysp_timeline('evaluate', h, vbl + ifi, ifi);   % each frame
```

Channels and fields are 1-based. A base keeps the header's number (0 is
the RT clock, 1 to 7 are yours), and a trigger channel keeps its value (0
by default), because it names one of your outputs. A sequence is a handle
of its own; `close` of the timeline also closes its sequences.

| Command | Returns | Notes |
|---|---|---|
| `'open' [, opts]` | `uint64` handle | `channels`, `initial`, `events` (1024), `keys` (1024), `fields` (64), `tweens` (256), `lead` (a fraction of a period, or `'none'`) |
| `'evaluate', h, onset [, period [, index]]` | `[values, events, fields]` | `values` 1 x channels; `events` a struct column (`id`, `kind`, `kind_name`, `base`, `time`, `target`, `value`, `code`, `user`, `flags`, `frame`, `onset`, `residual`, `time_ns`, `onset_ns`, `residual_ns`); `fields` 4 x slots. `index` defaults to the count of evaluates |
| `'values', h`, `'field_values', h` | matrix | the last evaluate's |
| `'anchor', h, base, rt [, bt]`, `'pause'`/`'resume', h, base, rt`, `'stop', h, base` | none | base clocks; an anchor at or before the furthest evaluated time rewinds |
| `'skip', h, base, rt, bt` | count | a seek; the events passed over never fire |
| `'rate', h, base, rt, num [, den]`, `'get_rate', h, base` | none, `[num den]` | an exact rational rate |
| `'base_time'`, `'rt_time'`, `'window'`, `'next_due'`, `'horizon'`, `'clamped'`, `'lead'` | seconds, or `[]` | queries |
| `'add', h, events` | first id | a struct (array), all or none: `time`, `kind` (name or number), `base`, `target`, `value`, `code`, `user` |
| `'remove', h, id`, `'prune', h [, base]`, `'clear', h [, base]` | none, count | |
| `'events', h [, base]`, `'find', h, id`, `'peek', h, base, from, to` | struct column | the record; the pending events of an RT window |
| `'set_keys', h, ch, base, K [, curves]` | none | `K` is N x 2..4 `[t value ease curve]`, `curve` a 1-based row of `curves` (M x 4) for BEZIER; `[]` removes |
| `'set_track', h, ch, base [, track]` | none | `keys`, `curves`, `samples`, `t0`, `rate`, `interp`, `period`, `repeats`; none removes |
| `'tween', h, ch, base, to, name, value, ...` | none | a channel tween from "now": `duration`, `start`, `delay`, `from`, `ease`, `curve`, `cycles`, `yoyo`, `keep_velocity` |
| `'seq', h, base [, 'now']` | `uint64` sequence | a cursor on the base |
| `'on'`, `'off', q, ch`; `'set', q, ch, v`; `'trigger', q, code [, ch]`; `'mark', q, code [, user]` | none | events at the cursor |
| `'wait', q, dt`, `'at', q, t`, `'cursor', q` | none, struct | `cursor`: `t`, `prev_start`, `prev_end`, `t_ns`, `base`, `err`, `err_call` |
| `'to'`, `'then', q, ch, value, name, value, ...` | end time | a channel tween; `then` also moves the cursor to its end |
| `'keyframes', q, ch, values, offsets [, ease]`, `'on_n', q, chs [, stagger]` | none | |
| `'run', q, ops`, `'check_ops', h, base, ops` | none | an op table, all or none: `{{'on', 1}, {'wait', 0.5}, {'to', 3, 0.5, 'duration', 0.1}}`; an error names the op (1-based) |
| `'cancel', q`, `'release', q` | count, none | undo what the sequence added; drop the sequence handle |
| `'field', h [, value [, 'int']]`, `'fields', h, count [, value [, 'int']]` | index, indices | a value target tweens write, 1 to 4 components; `fields` makes adjacent ones for a stagger |
| `'field_value', h, f [, value]`, `'field_free', h, f` | value | |
| `'tween_to', q, f, to, dur [, ease]`, `'tween_from'`, `'tween_from_to', q, f, from, to, dur [, ease]`, `'tween_set', q, f, v` | tween id | GSAP's `to()`, `from()`, `fromTo()`, `set()` |
| `'animate', q, f, name, value, ...` | tween id | `to`, `from`, `to_current`, `duration`, `delay`, `stagger`, `ease`, `curve`, `cycles`, `yoyo`, `keep_velocity`, `immediate`, `overwrite`, `on_start`, `on_complete` (logical: the event goes in the report), `user` |
| `'kill', h, f`, `'kill_tween', h, id`, `'tween_count', h` | count | |
| `'error', h`, `'version'` | string | `version`: the binding's (1.1.0), and the header's as a second output |
| `'pins', h` | count | objects of other MEX files that hold the timeline (SHARING) |
| `'close', h` | none | `ysp_timeline:busy` while another MEX file holds it |

Since 1.1.0 the MEX file exports `ysp_timeline_api_table()` (see SHARING
in the `ysp_audio` section), and a timeline handle is a number from
`0x544C * 2^32 + 1` up.

An ease is a name (`'linear'`, `'step'`, `'cosine'`, `'quad_in'`,
`'quad_out'`, `'quad_in_out'`, `'cubic_in'`, `'cubic_out'`,
`'cubic_in_out'`, `'log'`, `'bezier'` with `'curve'`), its number, or
`[x1 y1 x2 y2]`. Errors are `ysp_timeline:arg`, `:closed`, `:order`,
`:full`, `:bound`, `:not_found`, `:handle` and `:usage`, with the
header's message.

Not bound: a MATLAB function as `on_start` or `on_complete` (the events in
the report serve the frame loop, and a call into the interpreter from
inside the evaluate would cost more than the frame's work), `ytl_sample()`
(no validation), the parameter tables (for a player's loader), and
negative times (refused, as in every binding).

## How to save and resume a session

1. Save the session. With an integer-seed `rng`, also save the generator
   state:

   ```matlab
   snap = ysp_quest('save', h);
   state = ysp_quest('rng_state', h);          % only with an integer-seed rng
   save('session.mat', 'snap', 'state', 'desc');
   ```

2. Later, load it with the same desc. Put the saved generator state in
   `desc.rng`:

   ```matlab
   load('session.mat');
   desc.rng = state;                           % only with an integer-seed rng
   h = ysp_quest('load', snap, desc);
   ```

The resumed session makes the same proposals and has the same posterior as
the session that was not interrupted. This is also true when a proposal was
pending at the save. The desc must agree with the snapshot on every number.
A mismatch raises `ysp_quest:load` with the header's message, which names
the first field that differs. The same steps apply to `ysp_stair`,
`ysp_aep` and `ysp_trials`. A staircase has no generator. With a
function-handle generator, save and restore its state yourself.

A `ysp_trials` session with tracks also needs the state of each track:
`ysp_stair`, `ysp_quest` and `ysp_aep` tracks have their own `save`.

With an async session, stop it first (`async_stop`), then save.

## How to run the inference on a thread

`ysp_quest` and `ysp_aep` can run the update and the selection (and for aep,
the threshold search) on a C thread. The frame loop then only polls.

1. Open the handle as usual, then start the thread:

   ```matlab
   h = ysp_aep('open', desc);
   ysp_aep('async_start', h, struct('below_normal', true));
   snap = ysp_aep('async_poll', h);              % seq 0: the first stimulus
   ```

2. For each trial, submit the response. Then poll once per frame until the
   snapshot includes it:

   ```matlab
   while ~snap.done
       y = run_trial_frames(snap.x);
       seq = ysp_aep('async_submit', h, snap.x, y);
       snap = ysp_aep('async_poll', h);
       while snap.seq < seq
           draw_one_frame();                    % Screen('Flip', ...) and so on
           snap = ysp_aep('async_poll', h);
       end
   end
   ```

3. Stop the thread. The handle is yours again:

   ```matlab
   ysp_aep('async_stop', h);
   [thr, lo, hi] = ysp_aep('threshold', h, 0.5);
   ```

Between `async_start` and `async_stop` the thread owns the handle, and every
other command on it raises `ysp_<name>:busy`. Read the snapshot instead.
`async_poll` is one atomic load and a struct copy. `async_wait(h, seq,
timeout_s)` blocks for up to `timeout_s` seconds (default 1) and then raises
`ysp_<name>:timeout`. Nothing in the interpreter runs while it blocks, not
even Ctrl-C, so keep the timeout short. `async_submit` raises
`ysp_<name>:busy` when the queue is full. Nothing was queued, so submit the
same response again on the next frame.

The thread is C and never calls the interpreter. So `async_start` refuses a
handle whose `rng` is a function handle (use an integer seed), and
`ysp_quest` also refuses a MATLAB `pf_batch` under `no_table`.
`async_submit_values` refuses a MATLAB `pf_batch`. A MATLAB `pf_batch` with
a table is permitted, because the table is built before the thread starts.

## ysp_box

Decode a response box's, a trigger box's or a line board's captured bytes
into events and clock pairs, without a port or a thread; and get the bytes
of queries and trigger codes. It compiles `ysp/box.h` and `ysp/input.h`.

```matlab
c = ysp_box('constants');                         % KIND_BOX, PRESS, DEVTICKS, ...
h = ysp_box('open', 'xid', c.KIND_BOX, 1);        % family, kind, device
[ev, pairs, info] = ysp_box('decode', h, bytes, t_read);   % bytes: uint8, t_read: s
[ev, pairs] = ysp_box('decode', 'line', fread(fid, Inf, '*uint8'));  % one shot
s = ysp_box('stats', h);                          % frames, garbage, pending
ysp_box('reset', h);  ysp_box('close', h);

b = ysp_box('encode', 'triggerbox', 'set', 12);   % uint8 row: write it to the port
b = ysp_box('encode', 'xid', 'pulse', 3, 0.002);  % the box ends the pulse
q = ysp_box('probe', 'line', 7);  i = ysp_box('identify', 'xid');
f = ysp_box('family', 'mmbts');                   % name, caps, code_max, fixed_pulse, baud, tick, slow_write
```

`ev` is a 1 x N struct array (as in `ysp_device` and `ysp_response`). Its fields are those of `yin_event`, in the
order every ysp binding uses: `t` (seconds), `t_ns` (int64), `device`,
`control`, `code`, `mods`, `kind`, `type`, `flags`, `stamp`, `unc`
(seconds), `x`, `y`, `value`, `aux`, `ticks`. Each `t` is the read time
given; `ticks` is the low 32 bits of the device's clock. `pairs` has the
columns `ticks` (uint64, the whole device time) and `probe` (0, or the
number of the timer query answered). `info` has `garbage` (bytes that fit
no frame), `text` (a cell of identity lines) and `outputs` (columns
`ticks` and `code`, a line board's `O` reports). A frame split across two
calls waits in the decoder. A family is a name (`'xid'`, `'line'`,
`'photo'`, `'triggerbox'`, `'biosemi'`, `'mmbts'`, `'lines'`,
`'parallel'`) or its number. A negative, NaN or Inf time, a value out of
range, a fraction where an integer is needed and bytes that are not uint8
raise `ysp_box:arg`. [docs/box.md](../../docs/box.md) has the tests.

## ysp_rdk

The analysis side of `ysp/rdk.h`, as the Python `ysp.rdk`: open a field
with the rig's desc, replay a logged trial, and get the dots a participant
saw, bit for bit.

```matlab
h = ysp_rdk('open', struct('algorithm', 'LL', 'aperture', 'rect', 'w', 8, 'h', 6, ...
            'count', 60, 'lifetime', 0.3, 'coherence', 0.5, 'speed', 4));
[xy, dir, sig] = ysp_rdk('trajectory', h, seed, t0, steps);
%   xy n x 2 x K single, dir n x K single (deg), sig n x K uint8
ysp_rdk('replay', h, seed, t0, steps);  d = ysp_rdk('digest', h);   % uint64

ysp_rdk('start', h, seed, t0);                    % a live run
ysp_rdk('set', h, 'coherence', 0.25, 'direction', 90);
n = ysp_rdk('update', h, t);
s = ysp_rdk('get', h);                            % coherence ... last (t, ns), stats
xy = ysp_rdk('xy', h);                            % also 'directions', 'ages', 'signals', 'events'
b = ysp_rdk('snapshot', h);  ysp_rdk('restore', h, b);
ysp_rdk('close', h);
```

The desc fields are the keyword names of `ysp.rdk.Field`; an enum takes
its name or its value. `seed` is a uint64 (a double only below 2^53).
`steps` is a K x 4 double matrix `[t coherence direction speed]`, or a
struct array such as the `last` of `'get'` after each update; a row with
`ns` replays from it. Every time is seconds, below 2^23 s (about 97 days):
the conversion is exact there, as in `ysp.rdk` ([docs/rdk.md](../../docs/rdk.md),
"Bindings"). `'philox'`, `'angle'` and `'sincos'` are the exact pieces.
Errors: `ysp_rdk:arg`, `ysp_rdk:order` (a time before the last update, or
an output before `'start'`), `ysp_rdk:handle`, `ysp_rdk:usage`.
## ysp_pack

Reads and verifies a ysp pack ([docs/pack.md](../../docs/pack.md)): one
store-only zip file, entries aligned to 4096 bytes, XXH64 hashes over
1 MiB chunks, a SHA-256 manifest. `ysp_pack.c` compiles ysp/pack.h and the
[ysp/json.h](../../include/ysp/json.h) it requires, and needs no library.
A pack is untrusted input: the header checks every byte before it uses
it, and each refusal raises `ysp_pack:<what>` with the header's message,
which names the rule, the entry and the chunk.

```matlab
h = ysp_pack('open', 'study.ysppak');        % mapped read-only
L = ysp_pack('list', h);                      % one struct per entry
t = ysp_pack('read', h, 'conditions.pstb');   % uint8 column, hashed first
d = jsondecode(ysp_pack('definition', h));    % the experiment definition
[ok, msg] = ysp_pack('verify', h);            % every entry, now
ysp_pack('close', h);
```

### Commands

| Command | Returns | Notes |
|---|---|---|
| `'open', path [, verify]` | `uint64` handle | `path` is a char (UTF-8 on every platform), or the pack's bytes as `uint8` (copied once). `verify`: `'use'` (default: an entry is hashed when it is first read), `'open'` (every entry at open) or `'none'` |
| `'close', h` | none | unmaps the file; results already returned stay valid |
| `'info', h` | struct | `id` (`'sha256:...'`, the pack ID), `format`, `entries`, `chunks`, `file_size`, `zip_start`, `experiment` (the EXPERIMENT entry's name, or `''`), `policy`, `hashed_bytes`, `describe` (one line for a log) |
| `'list', h` | N x 1 struct | `name`, `index` (1-based), `kind` (`'table'`, ...), `size`, `stream`, `flags`, `header_off`, `data_off` (0-based byte offsets in the file), `xxh64` (`uint64`), `crc32` (`uint32`); the manifest is first |
| `'entry', h, name` | struct | one entry, the fields of `'list'` |
| `'has', h, name` | logical | names are case-sensitive |
| `'read', h, name [, offset, n]` | `uint8` column | the entry, or `n` bytes (`Inf`: to the end) from the 0-based byte `offset`; a range hashes only the 1 MiB chunks it touches |
| `'verify', h [, name]` | none, or `[ok, msg]` | every entry, or one, whatever the policy. With no output a mismatch raises `ysp_pack:corrupt`; with outputs it returns `false` and the message |
| `'manifest', h` | char | `ysp/manifest.json`; `jsondecode` reads it |
| `'definition', h` | char | the EXPERIMENT entry (a definition's canonical bytes), or `''` |
| `'xxh64', bytes [, seed]` | `uint64` | opens nothing |
| `'entry_hash', bytes` | `uint64` | the pack's entry hash, the manifest's `"xxh64"`; opens nothing |
| `'version'` | char, char | this MEX function's version (1.1.0; 1.0.0 refused `'verify'` of one entry under `'none'`) and ysp/pack.h's |

Error ids: `ysp_pack:arg`, `:io` (the file cannot be opened or mapped),
`:format`, `:corrupt`, `:version` (another pack format), `:not_found`,
`:nomem`, `:handle`, `:usage`.

`'read'` copies the entry once, from the mapping into the new array, so
the array stays valid after `'close'`. The text commands decode UTF-8 into
MATLAB's UTF-16 `char`; Octave's `char` holds UTF-8, so there the bytes
are copied. A byte that starts no valid UTF-8 sequence (a damaged or
hostile manifest) becomes U+FFFD in MATLAB.

[test_mex_pack.m](test_mex_pack.m) reads
`tests/pack/fixtures/small.ysppak`, which the pack tool built from
`tests/pack/fixtures/small/`, and damaged copies of it: a flipped data
byte under each policy, truncations, and a flipped byte every 7 bytes. It
prints `PASS` with the number of checks.
## ysp_response

The response of a trial from any input device (ysp/response.h, with the
ysp/input.h it includes; nothing else is compiled). Every time is
seconds; Psychtoolbox's `GetSecs`, `Screen('Flip')` and
`KbQueueGetEvent` times go in as they are.

```matlab
h = ysp_response('open', struct('choices', {{'f', 'j'}}, 'duration', 1.5, 'min_rt', 0.1));
ysp_response('arm', h, tFixation);
ysp_response('set_onset', h, tStim, struct('src', 'flip'));    % the stimulus's flip
% each frame:
[evt, n] = KbQueueGetEvent;                                     % or a ysp_box / ysp_device read
if n > 0, ysp_response('key', h, evt.Time, KbName(evt.Keycode), evt.Pressed); end
if ysp_response('update', h, GetSecs) == 2, ... end            % 2: ended
[r, row] = ysp_response('finish', h);   % r.response, r.rt (NaN: none), r.onset_tier; row: the data columns
ysp_response('close', h);
```

KbName's names are not all the header's (`'LeftArrow'` is `'left'`
here). To skip the names, use Psychtoolbox's key codes on both sides:
a choice `struct('name', 'left', 'control', KbName('f'))` and events whose
`control` is the event's `Keycode`. The collector compares the numbers.

**Desc** (`open`): `choices` (a cell of key names, or a struct array with
the header's choice fields `key`, `name`, `kind`, `match`, `cross`,
`device`, `control`, `code`, `level`, `hysteresis`), `mode` (`'all'`,
`'list'`, `'none'`), `kinds` (a mask, `'all'`, or a cell of kinds),
`min_rt`, `duration` (`Inf`: no deadline), `persist`, `max_responses`,
`allow_held_key`, `wait_for_key_release`, `max_hold`, `sources` (a struct
array: `kind`, `device`, `tier`, `partial`, `lo`, `hi`, `note`),
`trace_cap`, `trace_kinds`. An enum takes its name or its number. An
unknown field raises `ysp_response:arg`.

**Events** are struct arrays with the fields of `yin_event`: `t`
(seconds), `t_ns` (int64), `device`, `control`, `code`, `mods`, `kind`,
`type`, `flags`, `stamp`, `unc` (seconds), `x`, `y`, `value`, `aux`,
`ticks`, the layout of the `ysp_box` and `ysp_device` events. `'feed'`
reads a missing field as 0, `t_ns` before `t`, and a key name as a
keyboard's `control`. Give it every event of a frame in one call: it reads
them in place.

### Commands

| Command | Does |
|---|---|
| `h = ysp_response('open', desc)` | `yrsp_open()` |
| `ysp_response('close', h)` | |
| `ysp_response('arm', h, t)` | Starts a window |
| `st = ysp_response('set_onset', h, t, [opts])` | `opts`: `final` (default true), `src` (`'flip'`, `'audio'`, `'plan'`, `'other'`), `tier`, `residual`, `uncertainty`, `frame`. `t` may be an int64 ns |
| `st = ysp_response('feed', h, events)` | Input events, in order |
| `st = ysp_response('key', h, t, key, [down], [device])` | One keyboard event; `key` is a name or a scancode |
| `st = ysp_response('update', h, now)` | The deadline; 0 idle, 1 open, 2 ended |
| `[r, row] = ysp_response('finish', h)` | The result and its data columns (`YRSP_DATA_COLS`, NaN or `''` for an empty field) |
| `[r, row] = ysp_response('score', h, events, onset, [opts])` | Offline: one logged trial. `opts` also takes `arm` (default: the onset) and `end`. Events stamped before `arm` at the start are fed before the arm |
| `e = ysp_response('entries', h)` | Every press of the window |
| `ev = ysp_response('trace', h)` | The traced events up to the last finish |
| `ysp_response('set_sent', h, sent, [total])` | Codes sent at presses: a struct array (`t`, `kind`, `device`, `control`, `choice`, `written`) |
| `row = ysp_response('data_row', h)` | The last finish's data columns |
| `k = ysp_response('plan_frame', h)` | `[]` when the onset is final or not tied to a frame |
| `msg = ysp_response('error', h)` | The last failure's message |
| `ev = ysp_response('event', s)` | A full event struct from some of its fields (type PRESS by default) |
| `ysp_response('scancode', name)`, `ysp_response('key_name', sc)` | Key names |
| `K = ysp_response('constants')` | `KIND_*`, `PRESS`, `TIER_*`, `E_*`, `R_*` and the others |
| `[v, vh] = ysp_response('version')` | the MEX function's version (1.0.0) and ysp/response.h's |

The result's `choice` and `entry`, and an entry's `choice`, are 1-based,
0 for none. A missing time is `NaN` in seconds and `intmin('int64')`
(`YRSP_NO_TIME`) in its `_ns` field. [test_mex_response.m](test_mex_response.m)
runs the key checks in MATLAB or Octave and prints `PASS` with the number
of checks. The binding's scope and what is left out are in
[docs/response.md](../../docs/response.md#bindings).

## ysp_audio

Sound at a time on the ysp_rt clock for a Psychtoolbox experiment, with the
record of where each sound landed: its onset (the device-clock fit's time,
never an observed time), the residual from the time asked for, the stream
frame, and a tier. [example_audio.m](example_audio.m) plays five tones;
[test_mex_audio.m](test_mex_audio.m) runs on miniaudio's null device (no
sound hardware) and prints `PASS` with the number of checks.
[docs/audio.md](../../docs/audio.md#bindings) has what is not bound and
the call costs.

```matlab
h = ysp_audio('open');                                    % the OS default output
b = ysp_audio('tone', h, struct('hz', 1000, 'duration', 0.1, 'peak', ysp_audio('db', -30), 'ramp', 0.005));
id = ysp_audio('play_at', h, b, t);                      % t: seconds, ysp_rt clock
r = ysp_audio('wait', h, id);                            % r.onset, r.residual_ns, r.tier, r.flags
ysp_audio('close', h);
```

Every time is seconds; a record gives the exact ns as `int64` beside each
time (`onset_ns`, `target_ns`, `residual_ns`, `rendered_at_ns`). Samples
are a frames x channels matrix (double or single), as `sound()` and
`audiowrite()` take them; PsychPortAudio's channels x frames is the
transpose. A row vector is one channel. Output channels in `'channels'`
are 1-based; frame and sample numbers keep the header's (0 is the first).
To put Screen's flips on this clock, use `ysp_rt('correlate', @GetSecs)`.

| Command | Returns | Notes |
|---|---|---|
| `'version'`, `'now'`, `'db', x` | string, seconds (and `int64` ns), gain | `version`: the binding's (1.0.1) and the header's |
| `'devices' [, backend [, capture]]` | struct row (`name`, `default`, `backend`) | the names `'open'` matches |
| `'wav_info', path` | struct | `rate`, `channels`, `format`, `frames`, `duration`, `rf64`, without a device |
| `'open' [, opts]` | `uint64` handle | `device` (a part of a name), `backend` (`'auto'`, `'wasapi'`, `'coreaudio'`, `'alsa'`, `'pulse'`, `'null'`), `rate` (48000), `channels` (2), `sample`, `exclusive`, `period` (s), `voices`, `queue`, `min_tier`, `onset_offset` (s), `arena_bytes`, `device_index`. Refuses any format or rate conversion |
| `'caps', h`, `'describe', h`, `'lead', h`, `'is_open', h`, `'error', h`, `'pins', h` | struct, string, seconds, ... | |
| `'frame_at', h, t`, `'time_of', h, frame` | frame, `[s, ns]` | from the fit; `[]` before it |
| `'tone', h, s` | buffer handle | `hz`, `duration`, `peak`, `ramp`, `phase`, `channels`, `id` |
| `'noise', h, s`, `'click', h, s` | buffer handle | `duration`, `rms`, `ramp`, `seed`, `dist` (`'gauss'`, `'uniform'`), `channels`, `id`; `peak`, `duration`, `id` |
| `'buffer', h, samples [, id]`, `'load_wav', h, path [, id]` | buffer handle | copied into the device's arena |
| `'samples', b` | single matrix | a copy |
| `'arena_reset', h` | none | frees every buffer; `ysp_audio:busy` while one plays |
| `'play_at', h, b, t` | id | 0: as soon as it can |
| `'play', h, sound [, opts]` | id | `at`, `db`, `channels`, `offset`, `frames`, `loops` (-1: until stopped); `sound` is a buffer, stream or WAV handle |
| `'cancel', h, id`, `'stop', h [, id [, at [, ramp]]]`, `'gain', h, id, db [, at [, ramp]]` | none | id 0 stops every sound |
| `'update', h`, `'result', h, id`, `'wait', h, id [, timeout]` | count, record, record | `timeout` in seconds, `Inf` (the default) waits without end; the record: `id`, `target`, `onset`, `residual`, their `_ns`, `start_frame`, `end_frame`, `device_pos`, `rendered_at`, `buffer_id`, `flags`, `tier`, `sample`, `gap_frames`, `pending` |
| `'stream_open', h [, opts]` | stream handle | `channels`, `ring` (s), `preroll` (s, or `'none'`), `first`, `id` |
| `'stream_write', s, samples`, `'stream_end', s`, `'stream_reset', s [, first]`, `'stream_info', s`, `'stream_close', s` | frames taken, ... | |
| `'sample_at', s, t`, `'stream_time_of', s, sample` | sample, `[s, ns]` | once the stream has started; `[]` before |
| `'wav_open', h, path [, opts]` | WAV handle | `loops`, `start`, `ring` (s), `id`; a WAV handle is also a stream handle |
| `'wav_feed', w [, max]`, `'wav_seek', w, sample`, `'wav_wants', w`, `'wav_close', w` | frames, ... | |
| `'capture_open' [, opts]` | capture handle | `device`, `backend`, `rate`, `channels` (0: the device's), `sample`, `exclusive`, `period`, `ring` (s), `offset` (s), `device_index` |
| `'capture_read', c [, max]` | `[samples, first]` | single frames x channels, and the capture frame of row 1 |
| `'capture_time_of', c, frame`, `'capture_frame_at', c, t`, `'capture_info', c`, `'capture_describe', c`, `'capture_close', c` | | |
| `'close', h` | none | `ysp_audio:busy` while another MEX file holds it |

Errors are `ysp_audio:arg`, `:closed`, `:full`, `:format`, `:not_found`,
`:lost`, `:busy`, `:timeout`, `:io`, `:open`, `:handle` and `:usage`, with
the header's message. `'wait'` blocks MATLAB, and Ctrl+C does not end it.

### SHARING: one implementation for every MEX file

`ysp_audio` and `ysp_timeline` each export a table of their header's
functions (`ysp_audio_api_table()`, `ysp_timeline_api_table()`), so a MEX
file of another binding (ysp-vis: a movie's soundtrack, a timeline that
drives the movie clock) uses the device and timeline handles that this
MATLAB or Octave session opened. The consumer loads the owner's MEX file
from its own directory; Windows and `dlopen()` return the copy MATLAB or
Octave already loaded, so there is one handle table. A device or timeline
that another MEX file holds refuses `'close'` (`:busy`), and
`clear ysp_audio` leaves it open until that file lets it go. The handles of
`ysp_audio` start at `0x4155 * 2^32 + 1`, so a handle of another module is
not taken for one. [docs/bindings.md](../../docs/bindings.md#sharing-one-implementation-across-modules)
has the rules; [test_mex_share.m](test_mex_share.m) builds the test
consumer (`tests/share_consumer.c`) and prints `PASS` with the number of
checks.

## ysp_screen, ysp_gfx, ysp_video (ysp-vis)

The display, its frame loop and the record of every flip
([ysp/screen.h](../../include/ysp/screen.h)), and the stimuli, draw
lists, groups, instances and timeline bindings of
[ysp/gfx.h](../../include/ysp/gfx.h) (phase B, 2026-10-27), and text laid
out by pack/layout (phase C, MEX 1.2.0; Text in ysp_gfx, below). `ysp_video` plays
movies (Movies: ysp_video, below; [example_video.m](example_video.m)). Windows only. [example_screen.m](example_screen.m) runs a
3 s frame loop in a window; [example_gfx_2afc.m](example_gfx_2afc.m) a
two-alternative grating trial; [example_gfx_rsvp.m](example_gfx_rsvp.m)
an RSVP stream from an atlas; [example_gfx_stroop.m](example_gfx_stroop.m)
a Stroop block; [example_gfx_reading.m](example_gfx_reading.m) an RSVP
sentence and its word boxes. [test_mex_vis.m](test_mex_vis.m),
[test_mex_vis_gfx.m](test_mex_vis_gfx.m) and
[test_mex_vis_text.m](test_mex_vis_text.m) run on the simulated display (no
window) and print `PASS` with the number of checks;
[test_mex_vis_window.m](test_mex_vis_window.m) opens a real window for
about 25 s (local only) and checks five stimuli and two texts against the
C path, bit for bit.

```matlab
h = ysp_screen('open', 'gfx', struct('background', 0.5));   % fullscreen; the gfx compiles while it settles
g = ysp_gfx('open', h);
ga = ysp_gfx('gabor', g, 'sf', 1/32, 'sigma', 24, 'x', -200);
fx = ysp_gfx('shape', g, 'circle', 'w', 8, 'color', 1);
L = ysp_gfx('list', g, [ga fx]);                 % drawn in order by one call
tl = ysp_timeline('open', struct('channels', 1));
ysp_gfx('bind', g, ga, 'contrast', tl, 1);       % channel 1, written in C each frame
ysp_gfx('timeline', g, tl, true);                % evaluated at each frame's onset
while true
    [f, quit] = ysp_screen('begin', h);          % f.onset: when this frame shows
    if ~isempty(quit), break; end                % Shift+Esc, the close button, Alt+F4
    ysp_gfx('frame', g, L);                      % begin, bindings, draws, end
    ysp_screen('flip', h);                       % at f.onset; 'flip_at', h, t for a later vblank
end
r = ysp_screen('close', h);                      % the records not read yet; the gfx goes too
```

**Build.** Make the natives once from the repository root:
`uv run --no-project python tools/vendor_layout.py --write`, then
`uv run --no-project python tools/vendor_vis.py --write --angle --pl-mpeg --sdl msvc --layout msvc`
for MATLAB, and `--sdl mingw --layout mingw --mingw-bin "<Octave>/mingw64/bin"` for
Octave (the static SDL3 and the text layout must come from Octave's own
gcc). `build.m` then links `ysp_vis.c` against them, strips the Octave
file (`-s`), and copies ANGLE's `libEGL.dll` and `libGLESv2.dll` and the
license notices of SDL3, ANGLE, pl_mpeg and the text layout (Skribidi,
HarfBuzz, SheenBidi, libunibreak, budouxc) into this directory. Without the natives it skips `ysp_vis.c`
with a message. `ysp_audio` and `ysp_timeline` must be built here too.

**Layout and lifetime.** `ysp_vis.<mexext>` is the core: the three
headers, the static SDL3, the one handle table and every command. The thin
`ysp_screen`, `ysp_gfx` and `ysp_video` load it from this directory and
hand it each call, so a screen handle from `ysp_screen` opens a gfx in
`ysp_gfx`. A clear of one thin file leaves every object open; the clear of
the last one (or `clear mex`, or the exit) closes every gfx and screen.
`ysp_vis('info')` gives the thin files attached, the open objects, the
directory ANGLE is loaded from and the SDL version. Handles start at
`0x5653 * 2^32 + 1`.

| Command | Returns | Notes |
|---|---|---|
| `ysp_screen('version')` | `'1.2.1'`, and the header's | the same for `ysp_gfx`, `ysp_video`, `ysp_vis`; 1.1.0 added `ysp_gfx`'s stimuli and `ysp_video`'s movies, 1.2.0 the text, 1.2.1 `ysp_video`'s UTF-8 paths and error ids |
| `'displays'`, `'modes' [, display]`, `'mode_multiple', num, den, [w h], tol_ppm, display` | struct rows; `[k, mode, err_ppm]` | `'open'` takes a display's `id` and a mode struct |
| `'options'`, `'constants'` | struct | the options with a line each; `TIER_*`, `PATH_*`, `FLIP_*`, `ABORT_*`, `KIND_*`, `PRESS` ... |
| `'open' [, opts]` | `uint64` handle | a struct or name-value pairs: `windowed`, `size`, `display`, `mode`, `backend` (`'auto'`, `'dxgi_flip'`, `'composition'`, `'sim'`), `window_thread`, `lead`, `onset_offset` (s), `depth`, `depth_learn`, `settle`, `settle_flips`, `settle_min`, `settle_max` (s), `foreground`, `panic`, `panic_presses`, `panic_window`, `panic_grace` (s), `patch`, `patch_size`, `patch_corner`, `show_cursor`, `keep_os_gamma`, `key_dedup` (s), `gamepads`, `min_tier`, `sim_period` (s). Settles: 0.5 to 0.8 s fullscreen, 2.5 s in a window. Two options of the binding: `gfx` (`true` or a struct of `ysp_gfx('open')` options: the gfx starts while the screen settles, and `ysp_gfx('open', h)` finishes it) and `frame` (`'struct'`, the default, or `'row'`: see `'begin'`) |
| `[f, quit, done] = ('begin', h)` | frame | `f`: `onset`, `onset_ns`, `period`, `period_ns`, `index`, `vblank`, `n_done`, `done_lost` (`_ns` as `int64`); `done`: the records completed since the last begin, only when asked for (a third output). With `'frame', 'row'`: `f = [onset period index vblank n_done done_lost]` and `done` an n x 15 matrix (below). An abort raises `ysp_screen:abort`; `[f, quit] = ...` returns it in `quit` (`reasons`, `presses`, `t`, `t_ns`, `panic`) with `f` empty |
| `'flip', h`, `'flip_at', h, t` | none | at the predicted onset, or at the vblank `t` snaps to |
| `'wait_flip', h`, `'records', h` | record, record row | `records`: every record since the last call (4096 kept; the second output counts the overflow); an n x 15 matrix on a `'frame', 'row'` screen: `index target planned onset residual dropped path tier flags` and the six phases (seconds; `NaN` unknown) |
| `'mark', h, phase` | none | `'evaluate'`, `'script'`, `'draw'`, `'upload'` |
| `'poll', h [, max]` | event row | the shared event layout of `ysp_device` and `ysp_response` |
| `'title', h, text`, `'cursor', h, show`, `'grab', h, on` | none | through the window thread; `:not_implemented` on `'sim'` |
| `'describe', h`, `'caps', h`, `'settle', h`, `'sync', h`, `'set_patch', h, v` | | |
| `'request_abort'`, `'input_stats'`, `'now'`, `'now_ns'` | | |
| `'close', h` | record row | lets the last flip complete; the gfx on it closes too |
| `ysp_gfx('open', h, name, value, ...)` | gfx handle | `background`, `units` (`'px'`, `'deg'`), `view` (`[distance_mm width_mm]`), `dither`, `seed`, `cones`, `cal` and `lum` (the bytes of .yspcal and .ysplum files, uint8), `max_draws`, `max_instances`, `cache` (true: the per-user program cache, the default; false; or a folder), `prime`. One gfx per screen: its output stage writes every pixel. `ysp_gfx('open', h, 0.5)` (a background) works as in phase A |
| `ysp_gfx(...)` | | the stimuli and the frame: [ysp_gfx](#ysp_gfx), below |
| `ysp_video('shared')` | struct | loads `ysp_audio`'s and `ysp_timeline`'s tables and gives their versions |

A record has `index`, `target`, `planned`, `onset`, `residual` (each with
`_ns`, `int64`), `dropped`, `missed`, `late` (`FLIP_LATE_TARGET`), `path`,
`tier`, `flags`, `phases` (1 x 6 seconds, `NaN` for the GPU phase) and
`code_risk`. A frame number starts at 0, as the header's. Errors are
`ysp_screen:arg`, `:closed`, `:order`, `:timeout`, `:lost`, `:refused`,
`:not_implemented`, `:abort`, `:open`, `:handle` and `:usage` (and the same
under `ysp_gfx:`). `'begin'` and `'wait_flip'` block MATLAB, and Ctrl+C
does not end them; Shift+Esc does.

### ysp_gfx

Stimuli, draw lists, groups and textures are handles of the same table.
Their parameters go by ysp/gfx.h's names (`ysp_gfx('params', kind)` lists
them, with unit, range and default) as name-value pairs or a struct.
Closing the gfx, or its screen, frees them and their handles: a handle
kept after that gives `ysp_gfx:handle`. Element, channel and list indices
are 1-based.

| Command | Returns | Notes |
|---|---|---|
| `'shape', g, shape, ...` | handle | `'rect'` (`radius`), `'circle'`, `'annulus'` (`inner`), `'line'` (`width`), `'polygon'` (`vertices`, n x 2), `'cross'` (`width`), `'rrect'` (`radii`), `'arc'` (`sweep`, `thickness`, `start`), `'pie'`, `'capsule'` (`radii`), `'ngon'` (`sides`, `rounding`), `'star'` (`points`, `inner`, `rounding`), `'ellipse'`, `'qbezier'`, `'polyline'` (`path`, n x 2; `width`, `join`, `cap`); `color`, `opacity`, `edge`, `edge_width`, `stroke`, `stroke_align`, `offset`, `dash`, `gap`, `trim`, `blend`, ... |
| `'grating'`, `'gabor'`, `'noise', g, ...` | handle | `sf`, `phase`, `sigma`, `size` (gabor; 0 = 8 sigma), `aperture`, `dist`, `check`, `seed`, `contrast`, `dir` |
| `'texture', g, pixels, 'format', f, 'linear', l, 'srgb', s` | handle | H x W uint8 (1 to 4 channels), uint16, single or double; `'update', T, pixels [, [x y]]` between frames |
| `'image', g, T, ...`, `'dots', g, 'n', n, 'positions', XY, ...` | handle | `src` (a sprite of an atlas), `modulation`, `tint`; dots: `'set', d, 'positions', XY` (n x 2) between frames |
| `'instances', g, template, n, 'fields', {'xy', 'ori', ...}, 'palette', P` | handle | n elements in one draw; `'elements', I, name, values [, first]` sets a field (`x`, `y`, `xy` (n x 2), `ori`, `phase`, `contrast`, `scale`, `gate`, `color`); `E = ('elements', I)` gives them, 8 x n single |
| `'list', g, [s1 s2 ...]`, `'add'`, `'remove'`, `'clear'`, `'count'` | handle | stimuli drawn in order by one call |
| `'group', g, ...` | handle | `place`, `anchor`, `x`, `y`, `w`, `h`, `ori`, `scale`, `opacity`, `visible`; a stimulus joins with `'group', G` |
| `'set', obj, name, value, ...`, `v = ('get', obj, name)`, `s = ('get', stim)` | | a stimulus, group or list; a list takes n values (n x k for a color) or one for all, and `'get'` gives n x k. `'space', 'dkl'` converts `color` and `dir` through the calibration; `'color', '#ff8000'` is sRGB |
| `'copy', s`, `'free', obj` | handle, none | `'free'` refuses (`ysp_gfx:busy`) what a list or a stimulus still uses; a binding goes with its object |
| `'begin', g`, `'draw', g, handles...`, `'end', g`, `'frame', g, handles...` | | `'frame'`: begin, the bindings, the draws and end in one call |
| `'bind', g, target, name, tl, channels [, element]` | | a `ysp_timeline` channel writes the parameter in each `'begin'`: a stimulus (`color`: 3 channels), a list (one channel per item, or one), a group, or instances' elements from `element` |
| `'timeline', g, tl, evaluate`, `n = ('unbind', g, target, name, tl)` | | evaluate `tl` at each frame's onset in `'begin'`; `[]` or `''` match all. The gfx pins each timeline once (`ysp_timeline('close')` gives `busy`) until its last binding goes |
| `'hit', obj, x, y`, `'bounds', s`, `'resolve', s [, element]`, `'length', s` | | screen px from the top-left: a stimulus gives true or false, instances the element (0: none), a list `[item element]` |
| `'color', g, space, [a b c]`, `'dir', ...` | `[r g b]` | through the calibration's ysp/color.h context |
| `'capture_start', g, ...`, `'capture_mark'`, `'capture_pump'`, `'capture_stop'`, `'capture_stats'` | stats | chosen frames to PPM files off the frame thread |
| `'prime', g`, `'read_scene', g, [x y w h]`, `'read_output', ...`, `'describe'`, `'info'`, `'params' [, kind]` | | the reads wait for the GPU: tests |

Not bound: compounds, effects, paint, glyph runs, curve sets and runs as
handles of their own (a text holds its own), user shaders, render
targets, blur and video textures (ysp_video's); no image file reading
(ysp/gfx.h has none: `imread`, then `'texture'`).

### Text in ysp_gfx

A text is laid out by pack/layout: Skribidi with HarfBuzz, pinned and
patched ([docs/layout.md](../../docs/layout.md)), so it is shaped for its
script, put in order by the Unicode bidirectional algorithm, kerned, and
broken into lines; ysp/gfx.h draws its glyphs as curve runs from curve
sets built from the same font bytes. A text handle is a stimulus handle:
`'set'`, `'get'`, `'list'`, `'bind'`, `'hit'`, `'bounds'`, `'draw'` and
`'frame'` take it. [example_gfx_stroop.m](example_gfx_stroop.m) changes one
text's word and ink between trials.

```matlab
f = ysp_gfx('font', g, {fullfile(getenv('WINDIR'), 'Fonts', 'segoeui.ttf'), ...
                        fullfile(getenv('WINDIR'), 'Fonts', 'msyh.ttc')});   % a fallback list
t = ysp_gfx('text', g, 'RED', 'font', f, 'size', 64, 'color', [0.6 0.05 0.05]);
ysp_gfx('set', t, 'text', 'GREEN', 'color', [0.05 0.45 0.05]);   % a new layout: between frames
[W, words] = ysp_gfx('words', t);        % [first last line x0 y0 x1 y1], screen px; the strings
```

| Command | Returns | Notes |
|---|---|---|
| `'font', g, src, 'face', k, 'name', s` | handle | `src` a path, the uint8 bytes of a font file, or a cell of them: a fallback list, tried in order for each run of one script. `'face'`: one, or one per file (a .ttc's face) |
| `'font_info', f` | struct row | per file `name`, `face`, `sha256`, `units_per_em`, `glyphs`, `ascender`, `descender`, `line_gap` (em), `built` (glyphs in its curve set), `build_time` (s) |
| `'text', g, str, name, value, ...` | handle | `font` (a font handle, a path or a cell of paths, made once per gfx; none: Segoe UI, else Arial), `size` (units per em; 32 px or 1 deg), `width` (wrap width; 0 none), `align` (`'start'`, `'center'` (the default), `'end'`), `line_height` (times the fonts' line; 0 = 1), `dir` (`'auto'`, `'ltr'`, `'rtl'`), `lang` (BCP 47; `'ja'`, `'zh-Hans'`, `'zh-Hant'`, `'th'` break at BudouX phrases), `spans` (k x 6 `[first last r g b size]`, char indices, last included; `NaN` color: the text's, size 0: the text's; at most 15 colors), and any stimulus parameter: `place`, `anchor`, `x`, `y`, `ori`, `color`, `space`, `opacity`, `visible`, `gate`, `group`, `blend`, `rays`, `aliased` |
| `'set', t, name, value, ...` | none | the text's own parameters (one layout; between frames, else `ysp_gfx:order`; a refused change leaves the text as it was) and the stimulus's |
| `v = ('get', t, name)` | | the above, and `box` (`[w h]` units), `n_lines`, `n_glyphs`, `n_runs`, `n_chars`, `missing`, `resolved_dir`, `layout_time`, `build_time`, `upload_time` (s), `new_glyphs`, `stamp` (the layout's versions and the fonts' hashes, for a data file) |
| `G = ('glyphs', t)` | n x 6 | `[char line x0 y0 x1 y1]`: each glyph's character, line and ink box (a space: its pen point) |
| `[W, w] = ('words', t)` | n x 7, cell | `[first last line x0 y0 x1 y1]`: the runs of characters between white space, their glyphs' advances across and their line's box down; `w{i}` is `str(first:last)` |
| `L = ('lines', t)` | n x 6 | `[first last x0 y0 x1 y1]`; a line's trailing space is in its box |
| `'hit', t, x, y` | char index | the glyph's character there, 0 none; a list gives `[item char]` |
| `'free', f` | none | `ysp_gfx:busy` while a text uses the font |

Boxes are screen px from the top-left, axis-aligned (a turned text gives
the bounds of the turned boxes). A text's box is its laid-out block (as
wide as its widest line, or `width` when it wraps), placed by `place`
and `anchor` as any stimulus's: centered on the screen by default.
**Strings.** MATLAB's char is UTF-16: a non-ASCII string goes in and comes
back as it was, and a character index is a MATLAB index (a character
past U+FFFF, an emoji, takes two). Octave's char holds UTF-8 bytes, so
there an index is a byte index; `strfind` gives the same numbers in both.
Paths and `lang` go through the same conversion. A text takes 3 of the
gfx's `max_draws` blocks a frame per run: past about 340 texts a frame,
open the gfx with a larger `'max_draws'`. Not bound: OpenType
feature switches, a language per span, vertical text, justification,
outlines, glow (blur) and color glyphs.
### Movies: ysp_video

A movie on a gfx ([ysp/video.h](../../include/ysp/video.h)): each display
frame shows the frame due at the frame's predicted onset, and gets a record.
[example_video.m](example_video.m) plays a movie it writes, with a
soundtrack on the default output; [test_mex_video.m](test_mex_video.m)
runs on the simulated display and the null audio device and prints `PASS`
with the number of checks.

```matlab
m = ysp_video('open', g, 'clip.mp4');           % clip.mp4.yspvi beside it
ysp_video('soundtrack', m, a, 'clip.wav');      % optional: a ysp_audio handle; the clock follows the sound
ysp_video('play', m);
while true
    [f, quit] = ysp_screen('begin', h);
    if ~isempty(quit), break; end
    playing = ysp_video('update', m);           % after the screen's begin, before the gfx's
    ysp_gfx('begin', g); ysp_video('draw', m); ysp_gfx('end', g);
    ysp_screen('flip', h);
    if ~playing, break; end                     % the first frame past the end
end
ysp_screen('wait_flip', h);                     % the last flips complete the records
r = ysp_video('records', m);                    % one per display frame
ysp_video('close', m);                          % gives the ysp_audio handle back
```

| Command | Returns | Notes |
|---|---|---|
| `'open', g, path [, opts]` | `uint64` handle | name-value pairs or a struct: `data` (uint8, with path `''`), `index` (a path or uint8; none = path + `.yspvi`), `timeline` (a `ysp_timeline` handle) with `base`, `records` (FRAME records kept, 36000), `loop`, `ahead`, `preroll`, `lead`, `light`, `chroma`, `backend`, `hw_decode`, `strict_cadence`, `inline_decode`, `inline_budget`, `refresh_num`, `refresh_den`, `movie_index`, `min_tier`, `pin_cpu` (`'options'` lists them) |
| `'update', m` | logical | true while playing; `:order` out of turn or twice in a frame |
| `'draw', m [, [x y w h], ori, opacity, linear]` | none | between the gfx's begin and end; `w`, `h` 0 = the frame's pixels |
| `'play', m [, t]`, `'pause', m [, t]` | id | `t` on the ysp_rt clock; none or `[]` as soon as possible |
| `'seek', m, t [, resume]`, `'seek_frame', m, i [, resume]` | id | resume `[]`, `'paused'` or a time |
| `'show', m, i` | none | manual: this display frame shows frame `i`; before `'update'` |
| `'result', m, id`, `'last', m` | struct or `[]` | the PLAY, PAUSE or SEEK record; the newest update's record |
| `'records', m`, `'events', m` | struct row, cell row | the FRAME records since the last call (the second output counts the overflow); the other records as structs with `kind` |
| `'soundtrack', m, audio, wav [, opts]` | none | wav a path or uint8; `db`, `channels`, `ramp`, `ring` (s), `follow` (true) |
| `'follow_audio', m, audio` | none | `[]` stops it |
| `'info', m`, `'describe', m`, `'movie_time', m, t`, `'close', m` | | `info`: size, rate, frames, `duration` (s, `duration_ns` int64), form, cadence, counts |
| `'probe', path [, index]`, `'make_index', media, index [, opts]` | struct, frames | `make_index`: `''` writes `media.yspvi`; `matrix`, `range`, `transfer`, `primaries`, `siting`, `backend`, `hw`, `no_seek_check` |
| `'write_seq', path, frames, fps [, 'qoi']` | frames | `[h w n]` or `[h w 4 n]`: uint8 (R8, RGBA8), uint16 (R16), single (R32F, RGBA32F); fps a number or `[num den]` |
| `'options'`, `'constants'`, `'shared'` | struct | `F_*` flags; the owners' tables |

A record has `display`, `frame`, `onset`, `due`, `residual` (`onset -
due`: with the audio clock the frame's A/V error), `movie_t` (each with
`_ns`, `int64`), `decision` (`'shown'`, `'repeated'`), `why` (`'due'`,
`'cadence'`, `'display_late'`, `'decode_late'`, `'drift'`, `'seek'`,
`'loop'`, `'manual'`), `tier`, `path`, `flags`, `ahead` and `shows`. A
dropped frame has a `drop` event. Errors are `ysp_video:arg`, `:format`,
`:closed`, `:order`, `:refused`, `:io`, `:not_found`, `:decoder`,
`:not_implemented`, `:handle`, `:shared` and `:usage`, by the header's
code: since 1.2.1 (ysp/video.h 0.6.0) `'open'` raises the code of its
failure (`:not_found` for a file that is not there, `:format` for one not
in the canonical form), not `:open`. Paths are UTF-8 for the header:
MATLAB's char (UTF-16) is converted, Octave's char bytes go as they are,
so any folder name works. The `ysp_audio` handle of a soundtrack
and a `ysp_timeline` handle are pinned through their owners' tables until
the movie closes: `ysp_audio('close', a)` is `ysp_audio:busy` meanwhile.
A closed gfx or screen closes its movies first; `'records'` still reads
them, and `'close'` frees the handle.

Between trials, `pause` is fine in MATLAB and in Octave: the window
belongs to ysp/screen.h's window thread and does not hang. With `'panic'`
on (fullscreen; off by default), three Shift+Esc presses in 2 s while the
frame loop does not call `'begin'` release the display: the next `'begin'` reports the abort with `panic`
true, and only `'close'` is left. Close in `try`/`catch`, not in an
`onCleanup`: Octave did not run one in the av probe
([docs/av_probe.md](../../docs/av_probe.md), section 4).

## Verification

[tests/compare/compare_quest_mquestplus.m](../../tests/compare/compare_quest_mquestplus.m)
runs `ysp_quest` and mQUESTPlus on the examples of Watson (2017) figures 2,
3 and 4 and on the mQUESTPlus marginalization demo. It compares the
selection and the posterior on every trial.
[tests/compare/compare_stair_palamedes.m](../../tests/compare/compare_stair_palamedes.m)
replays fixed response sequences through `ysp_stair` and Palamedes
`PAL_AMUD`, and runs `ysp_quest` beside `PAL_AMPM` (Psi and Psi-marginal).
Both scripts print a table and raise an error on a disagreement beyond
tolerance. The results in MATLAB R2023a:

- mQUESTPlus: 0 of 424 selections differ. 32 selections are ties within
  6.9e-10 bits, and in each of them both picked the same stimulus. The
  posteriors agree within 4.3e-7, which is the float likelihood table of
  `ysp/quest.h`.
- `PAL_AMPM`: identical selections over 60 trials each for Psi and
  Psi-marginal, and posteriors within 5.7e-8.
- `PAL_AMUD`: eight configurations are identical, level for level and
  reversal for reversal. Two differences are Palamedes' design choices,
  and the `ysp/stair.h` manual states both: `PAL_AMUD` keeps the
  opposite-direction counter when a response causes no step, and a step
  that a caller changes after the reversing update applies one trial later
  than a schedule does.

Neither reference is in this repository: mQUESTPlus is on GitHub
(BrainardLab/mQUESTPlus), and Palamedes is at palamedestoolbox.org, whose
terms do not permit redistribution.
