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
MATLAB or Octave and prints `PASS`. Without a port it checks `ysp_serial`'s
arguments and handles only; with the environment variable
`YSP_TEST_ECHO_PORT` set to the port of an echo board (`firmware/ysp_echo`)
it also runs the async reader on a 2000-line stream.
[test_mex_color.m](test_mex_color.m) does
the same for `ysp_color` (compiles nothing but ysp/color.h and
`ysp_mex_util.h`); `tests/compare/color_ptb.m` compares it with
Psychtoolbox's colorimetric code.

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
`jitters`. `order` also
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
draws to whole frames.

```matlab
d.jitters = [ysp_trials('exponential', 'fp', 0.5, 2.0, 0.4, [60000 1001]), ...
             ysp_trials('uniform', 'iti', 0.8, 1.2)];
h = ysp_trials('open', d);
ti = ysp_trials('next', h);
fp = ysp_trials('jitter', h, ti.index, 'fp');   % fp.s, fp.ns (int64), fp.frames
```

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
| `'uniform'`, `'choice'`, `'exponential'`, `'jitter_map', j, u` | struct | a jitter for `desc.jitters`; a variate mapped with no session (v0.2.1) |
| `'values', h, c` | struct | condition `c`'s table row, one field per column (v0.2) |
| `'table_info', h`, `'table', csv` | struct | `columns`, `types`, `n_rows`, `n_skipped`, `levels`, `values` (a cell), `hash` (v0.2) |
| `'latin', n, row [, balanced]` | `[items, rows]` | row `row` (0-based, taken modulo `rows`) of a cyclic or Williams square, 1-based items (v0.2) |
| `'splitmix', state` | `[u, state]` | one step of the seed generator |
| `'version'`, `'strerror', code` | string | |

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
the first field that differs. The same steps apply to `ysp_aep` and
`ysp_trials`. With a function-handle generator, save and restore its state
yourself.

A `ysp_trials` session with tracks also needs the state of each track.
`ysp_stair` has no snapshot: open each staircase again with its desc, and
replay its `history` (`shown`, `response`) through `update`. A staircase is
deterministic, so the replay restores it exactly. `ysp_quest` and `ysp_aep`
tracks have their own `save`.

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
