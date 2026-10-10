# ysp/serial.h design

Status: **v0.5.0, implemented.** Windows uses the Win32 COM API with overlapped
I/O; Linux and macOS share a termios backend. Since v0.4 the header depends on
`ysp/rt.h` for the clock, the waits, the scheduling ladder and the deadline
worker; see [Depends on ysp/rt.h](#depends-on-ysprth). Verified on Linux against a
socat pty pair, including a reader thread and a writer thread on one handle
under ThreadSanitizer and under AddressSanitizer plus UndefinedBehaviorSanitizer.
The Windows backend compiles clean under MSVC but has not run on a port: its
open-time validation and enumeration entry point run, but no byte has moved
through it, for lack of a COM device. The macOS paths compile on macOS in CI
but have never run on a device, for lack of a machine. No timing claim in the
header has been measured yet, and the async worker has only ever run at
`YSER_ASYNC_NORMAL`: the test machine grants no `CAP_SYS_NICE`, so the
`SCHED_DEADLINE` and `SCHED_FIFO` rungs of its ladder are written but
unexercised. The header's top comment is the user-facing documentation
(sokol-style); this page records why the API looks the way it does.

## Goals

- Byte I/O over serial ports for the devices used in psychophysics and
  neuroscience rigs: trigger boxes, response boxes, and DIY microcontroller
  boards.
- Same shape as `ysp/parallel.h`: one header plus the shared `ysp/rt.h`
  (since v0.4) and no other dependency, a caller-owned handle, a
  zero-initialized `desc` for defaults, and a message string in the handle
  for setup errors.
- Honest timing. The header documents what USB-serial can and cannot do, and
  the API gives the caller the clock to bracket a write.
- Windows, Linux, and macOS. Labs use all three.

## Non-goals

- Protocol layers (XID, SRBox, STK). They belong in device-specific headers or
  in the experiment code, built on `yser_read` with `YSER_READ_ALL` and
  `yser_write`.
- Async reads, callbacks, or an event loop. A listener thread that blocks in
  `yser_read` is simpler and matches how experiment code is written;
  `yser_interrupt` exists so it can be stopped.
- Custom baud rates through `termios2`/`BOTHER` on Linux. The requested rate
  goes to the driver as given and `yser_open` fails if the driver rejects it;
  on Linux that means rates without a termios `B*` constant. macOS gets
  `IOSSIOSPEED` because the TriggerBox runs at 2 Mbaud and Darwin's termios
  stops at 230400. Windows accepts any `DCB.BaudRate` the driver can do.
- Runtime reconfiguration (`yser_reconfigure`). The devices that switch baud
  after a handshake are GPS receivers (u-blox `UBX-CFG-PRT`), cellular modems
  (`AT+IPR`), HC-05 Bluetooth modules in AT mode, and some bootloaders. None
  is in the target list, and close plus reopen with a new `desc` covers the
  rare case.
- Waiting on modem lines (`yser_wait_lines`). No target device signals on
  CTS/DSR; `yser_get_lines` polls and that is enough.

## Target devices

| Device | Transport | Needs from the API |
|---|---|---|
| Brain Products TriggerBox / TriggerBox Plus | FTDI, 2 Mbaud | `yser_write_byte`, `yser_pulse`; Plus times pulses itself; `IOSSIOSPEED` on macOS |
| BioSemi USB trigger interface | FTDI | `yser_write_byte`, `yser_pulse` |
| Cedrus c-pod, StimTracker, RB-x40 (XID) | FTDI, 115200 8N1 | `yser_write` commands, `YSER_READ_ALL` for 6-byte key reports, `yser_find_ports` by vid/pid/serial, `low_latency` |
| Black Box ToolKit TTL modules | FTDI | `yser_write_byte` |
| PST Serial Response Box | RS-232 / USB-serial, 19200 | `yser_read` polling, `yser_set_dtr` (some models power the lamps from DTR) |
| Current Designs fORP (serial mode) | USB-CDC | `yser_read` polling |
| Arduino / Teensy / RP2040 DIY boxes | USB-CDC or CH340 | `yser_read`, `yser_write`, `keep_dtr_on_close`, `yser_purge` |
| Legacy RS-232 hardware | UART | parity/stop-bit options, modem lines, `yser_send_break` |

## Decisions

### Depends on ysp/rt.h

v0.4 deletes every line of timing code in this header and includes `ysp/rt.h`
instead: `yser_now_us` became `yrt_now_us` (v0.5.0 removed it), the pulse-width wait is
`yrt_sleep_until`, the reservation type is `yrt_sched_deadline`, the policy
enum is `yrt_policy`, and the async-pulse thread is a `yrt_worker`. It is a
hard dependency, in the `stb_truetype.h` / `stb_rect_pack.h` and
`sokol_gfx_imgui.h` / `sokol_gfx.h` sense: the user copies two files.

Three arguments, none of them tidiness.

- **Three copies of the same code were already drifting.** `ysp/parallel.h`,
  `ysp/serial.h` and the experiment code each had a monotonic clock, a spin,
  a sleep and an RT ladder. The header promised that a caller's
  `yser_now_us()` timestamps and the library's deadlines "share one base";
  with two copies of the conversion that was a claim about two pieces of code
  agreeing, and it is now the same function call.
- **The Windows spin bug existed only here.** This copy's blocking
  `yser_pulse` busy-spun the entire pulse width on `QueryPerformanceCounter`,
  where `ysp/parallel.h` had long spun only the last ~1.14 ms and slept the
  rest, and this copy's worker spin could not see a replacement pulse or a
  close until v0.3 found it by hand. Neither bug was findable in the other
  copies, because they were not in the other copies. One implementation is one
  place to fix and one place to measure.
- **The real-time rungs are untested everywhere.** No machine in this project
  grants `CAP_SYS_NICE`, so `SCHED_DEADLINE` and `SCHED_FIFO` have never been
  obtained by any of the copies. Three unexercised ladders are three times the
  risk for none of the coverage; `ysp/rt.h` at least has `examples/rt/jitter.c`
  pointed at it, and `yser_port.async_policy` now reports the same rung, by the
  same name, as everything else in the collection.

What the user gives up is the single file. `ysp/serial.h` alone no longer
compiles, and the STATUS block, USAGE and BUILDING all say so. That is the
whole cost, and it is paid once at copy time.

#### The writer mutex stays here

`yrt_worker` runs its callback **with its own lock released**. That is not an
oversight in `ysp/rt.h`, it is what makes the callback usable: the callback may
re-arm its own worker and may block, and a submit from another thread never
waits behind it. But this library's trailing byte is a write into a byte stream
that can block for up to `write_timeout_ns`, and the library's promise is that
exactly one thread writes the port at a time. Running the callback under the
worker's lock would buy that serialization and deadlock the first time a submit
raced a stalled write; so a straight substitution of `yrt_worker` for the old
private worker would have quietly dropped the guarantee instead.

The split: `ysp/rt.h` owns *when*, this header owns *who writes*. `yser__async`
holds one small writer mutex (a PI `pthread_mutex` where the platform has one,
a `CRITICAL_SECTION` on Windows) plus the pending trailing byte, armed with the
deadline it is due at. Every writer-role write takes the mutex; a user write
disarms the pending byte, calls `yrt_worker_cancel` and writes;
`yser_pulse_async` writes the onset, arms and submits, all under the mutex; and
the callback takes the mutex and writes only what is still armed and due.

The arm flag plus the deadline is the token, and it is checked at write time
rather than carried from submit time, because a `yrt_job_fn` receives the
same `ctx` for every job on a handle and cannot carry a per-job counter of its
own. That turns out to be the stronger test anyway. `yrt_worker_cancel`
covers the ordinary case, where the job has not left `ysp/rt.h`'s lock yet. It
cannot reach the window between the worker releasing that lock and the callback
acquiring the writer mutex, and in that window a user write may have disarmed
the byte (the callback must write nothing) or a new pulse may have armed a
different byte at a later deadline (the callback must leave it to its own
deadline). Asking "what is armed, and is it due?" answers both, and a flush
from `yrt_worker_close` writes whatever is armed at once, which is what
`yser_close` needs.

Lock order is writer mutex, then `ysp/rt.h`'s lock; the callback holds nothing
of `ysp/rt.h`'s when it takes the writer mutex, so the cycle does not close.

#### What the rename costs

`yser_sched_deadline` is a typedef of `yrt_sched_deadline` and
`yser_async_policy` of `yrt_policy`, with `YSER_ASYNC_*` and
`YSER_DEFAULT_RT_*` kept as aliases, so source that used them still compiles.
The enumerators' numeric values did change, because `yrt_policy` has a macOS
time-constraint rung between `DEADLINE` and `FIFO` that this header never had,
and macOS `async_policy` can now report it. Anything that stored those integers
has to be rebuilt rather than relinked; at v0.x, with the bindings built from
the same tree, that is a recompile and a changelog line, not a migration.

### Return codes everywhere, a message only from yser_open

`yser_open` returns `bool` and writes `yser_error()`, which its own `memset`
clears on entry, as `ypar_` does. Everything else returns an `int`: a count or
a `YSER_LINE_*` mask, 0 for done, or a negative `YSER_ERR_*` code, and never
touches the string. `yser_strerror(code)` names a code.

v0.1 split this by "setup" and "data path" and let `interrupt`, `purge`,
`send_break`, `set_dtr` and `set_rts` write the message. That was wrong, and
the header's own shutdown recipe proved it: it calls `yser_interrupt` from the
main thread while the writer thread may be inside `yser_purge(YSER_PURGE_TX)`
or `yser_send_break`, and an RX purge on the reader races a break on the
writer. Two threads formatting into one 256-byte buffer is a data race under
C11 whether or not anyone reads the result, so the buffer now belongs to the
one function that runs before any other thread exists.

The OS error number goes to the slot of the calling role: `rd_oserr` for the
reader, `wr_oserr` for the writer, `misc_oserr` for the any-role calls
(`interrupt`, `get_lines`, `set_dtr`, `set_rts`), which are documented as
one-at-a-time. `yser_get_lines` used to write `rd_oserr`, which a writer
thread calling it would have scribbled over a failing reader's error number.
`yser_purge` writes the slot of the direction it was asked to purge.

`YSER_ERR_DISCONNECTED` is distinct from timeout (0) because a USB unplug
otherwise looks like silence: macOS `read()` returns 0 (EOF), Linux `poll()`
returns `POLLHUP` immediately, and a listener with a long timeout would spin
at 100% CPU on a dead port.

### The wait call, per POSIX platform

Linux waits with `poll()`. macOS waits with `select()`, because Apple's
`poll(2)` still carries the note that it does not support devices, and every
descriptor this library waits on is one. pyserial makes the same split for the
same reason, which is the only field evidence available for a platform nobody
here can run.

The alternative considered was keeping `poll()` on both and writing a note into
the header that a Mac should be checked first. It was rejected: a note does not
make a wait wake up. The macOS backend is entirely unverified either way, so
`select()` costs no verification that existed, while `poll()` risks a listener
thread that never wakes or a poll that reports nothing on a live port, which is
a silent timing failure rather than a loud one.

What `select()` gives up is the hangup bit: there is no `POLLHUP` equivalent,
and `exceptfds` means nothing on a tty. A dead port on Darwin surfaces instead
as a readable descriptor whose `read()` returns 0, or as `EIO`/`ENXIO` out of
`read()` or `write()`, both already mapped to `YSER_ERR_DISCONNECTED`. What it
costs is `FD_SETSIZE`: `select()` cannot name a descriptor at or above it, so
`yser_open()` refuses such a port on Darwin with a message instead of writing
past an `fd_set`. Both waits sit behind one `yser__wait()` helper, so the
reader and the writer have one shape and only the wait call differs.

`read() == 0` is read as a hangup only because the wait reported the
descriptor readable first. Ungated it would be ambiguous: with `VMIN = 0` and
`VTIME = 0`, which is how this library configures the line, Linux `n_tty`
returns 0 for "nothing buffered" as well.

### Interrupt

`yser_interrupt(p)` wakes a `yser_read` blocked on another thread, which
returns `YSER_ERR_INTERRUPTED`. Every experiment shuts down, so every
listener thread needs this. A self-pipe on POSIX (added to the reader's
`poll` set) and a manual-reset event on Windows (waited on next to the
overlapped read's event). `yser_close` also interrupts, but the caller must
join the listener before closing: closing a descriptor another thread is
using is undefined on POSIX.

The wake is **latched**, not dropped. The v0.1 header said an interrupt with
nobody waiting is a no-op; implementing it that way means clearing the event
or draining the pipe at the top of every read, and then the documented
shutdown sequence deadlocks whenever the interrupt lands while the listener
is between two reads of a `YSER_READ_ALL` loop, or just before its next
`yser_read(YSER_TIMEOUT_INFINITE)`. So the wake stays pending until one read
consumes it, and several interrupts collapse into one. The cost is that a
handle interrupted while idle returns `YSER_ERR_INTERRUPTED` from its next
read, and shutdown is the only documented use of the call.

### Read model

`yser_read(p, buf, cap, timeout_ns, flags)` with `YSER_READ_ANY` returns
when at least one byte is available, with everything buffered at that moment.
`timeout_ns = 0` polls, `YSER_TIMEOUT_INFINITE` (-1) blocks. This is the pyserial
`timeout` model reduced to one call with a per-call timeout instead of
port-level state, so a frame loop can poll with 0 and a listener thread can
block, on the same handle, without reconfiguring the port.

A negative return discards the count. `YSER_READ_ALL` interrupted or
disconnected part way through a frame returns the code, and the bytes already
in the caller's buffer are neither counted nor reported: a half frame is not
usable, and `yser_interrupt` means shutdown, not "give me what you have". A
caller who wants partial input loops on `YSER_READ_ANY` instead.

On Windows the `YSER_READ_ALL` loop pays one `SetCommTimeouts` per iteration,
because the remaining time shrinks each time and `rd_timeout_applied_ms` only
elides repeated identical timeouts. That cache is there for `YSER_READ_ANY`,
which is the hot path: a frame loop polling with timeout 0, or a listener
blocking on one fixed timeout, sets the timeouts once and never again.

`YSER_READ_ALL` keeps reading until `cap` bytes or the total timeout, for
framed protocols. It is a flag rather than a separate `yser_read_exact`
because both modes share every argument and the caller's error handling; the
loop is platform-independent and sits over the per-platform `yser__read_some`
primitive. The first read always happens, so `YSER_READ_ALL` with timeout 0
still collects what is buffered.

### Windows read implementation

pyserial's `COMMTIMEOUTS` model, not `WaitCommEvent`. `EV_RXCHAR` is latched
only after `SetCommMask`, so a byte landing between "is the buffer empty?"
and the event arm is missed until the next byte, which for a one-byte
response is a full timeout late. FTDI and CH34x VCP drivers are also known to
misbehave with `WaitCommEvent` plus `CancelIo`. `{ReadIntervalTimeout =
MAXDWORD, ReadTotalTimeoutMultiplier = MAXDWORD, ReadTotalTimeoutConstant =
timeout}` gives "buffered bytes now, else the first byte within `timeout`"
in one `ReadFile`. The constant may not be `MAXDWORD`, so infinite waits loop
on `0x7FFFFFFF`. `SetCommTimeouts` is port-global, so the reader rewrites the
whole struct with the writer's constant preserved, and only when the timeout
changed (`rd_timeout_applied_ms`).

The `OVERLAPPED` structures live in the handle (`rd_ovl`, `wr_ovl`), never
on the stack: a canceled read that is not reaped with
`GetOverlappedResult(TRUE)` would complete into a dead frame.

An aborted read that carried no bytes does not end the call. `yser_interrupt`
is checked at the top of every iteration, and a vanished device is asked about
with `ClearCommError`, so an abort that is neither can only be this thread's own
`yser_purge(YSER_PURGE_RX)`. Returning 0 there would end a
`YSER_TIMEOUT_INFINITE` read with no data, no interrupt and no disconnect,
which the header promises cannot happen, and would read as a deadline to the
`YSER_READ_ALL` loop. The read instead resumes, against an absolute deadline
taken on entry so a finite timeout is not restarted by the abort.

A failed `SetCommTimeouts` maps through the same disconnect test as everything
else. It is where a reader on an unplugged port often fails first, and a
`YSER_ERR_IO` there would have hidden the unplug.

On the write side, only `ERROR_OPERATION_ABORTED` on a live port is reported as
a short count. Every other failure of `GetOverlappedResult` is an error code:
a short count is how `yser_write` signals its timeout, so a driver error
returned that way is indistinguishable from "the device is slow", and the
documented response to a short count is to send the rest again.

### Write model

`yser_write` blocks until the driver accepted every byte or `write_timeout_ns`
elapsed, and returns the count accepted; a short count is the timeout signal,
not an error code. The timeout is port-level (in `desc`) because a trigger
write should never wait on a policy decision at the call site; the 1 s
default is long enough that only a wedged device hits it. Zero in `desc`
means default, as everywhere in this collection, so the smallest configurable
write timeout is 1 ms; a fail-fast write is not a use case for any target
device. `yser_drain` is separate because draining on every write would add a
round trip to the bridge chip per trigger.

### Pulse

`yser_pulse(p, on, off, width_ns)` takes both bytes because `0x00` is the idle
byte for latching boxes (TriggerBox, BioSemi, DIY) and a protocol byte for
XID. It is blocking and spins on Windows; the header says to prefer
device-timed pulses.

### Async pulse

`yser_pulse_async(p, on, off, width_ns)` writes `on` on the calling thread and
returns; a worker thread the library owns writes `off` at an absolute deadline
taken right after the onset write. Since v0.4 that thread is a `yrt_worker`,
so the ladder, the ready handshake that makes `yser_port.async_policy` final
when `yser_open` returns, the one-pending-job rule, the deadline wait with its
spin tail and the flush on stop are all `ysp/rt.h`'s, and `yser_open` validates
`desc.sched` with `yrt_sched_normalize` before any OS call. What stays here
is the writer mutex and the armed trailing byte; see
[Depends on ysp/rt.h](#depends-on-ysprth). The shape still mirrors
`ypar_pulse_async`: the worker starts in `yser_open`, so the policy is final
before the first trial and no trial pays for thread creation.

This was left out of v0.1, on two arguments that were both wrong. "A worker
buys nothing measurable over USB" confused a floor with a budget: frame
quantization (1 ms full-speed, 125 us high-speed) is the least jitter the
transport can have, while OS scheduling and driver batching add a tail on top
of it, and on Windows that tail is reported to be much larger than a frame.
An elevated-priority thread removes the part that is ours to remove. The
second argument, that the lock would serialize the trigger thread behind a
user write, is a real cost but not a reason to have no call: the alternative
was `yser_pulse`, which does not serialize the trigger thread behind a write,
it stops it dead for the whole width. The point of the call was never
trailing-edge precision alone; it is that the caller gets its thread back.

What is genuinely different from the parallel port is that the trailing edge
is a stream write, not an `outb`, so it can block. That makes the worker a
second writer rather than an invisible helper, and it is documented as one:
every writer-role write takes the writer mutex (PI `pthread_mutex` where the
platform has one, critical section on Windows), the worker reports failures
through `wr_oserr` like any writer, and if a stalled device blocks one of them
the other waits for up to
`write_timeout_ns`. `ysp/parallel.h` does not pay that because `outb` cannot
block. The alternative, giving the worker its own queue and never blocking a
user write, would reorder bytes on a stream where order is the protocol.

Two behaviors carried over because they are about correctness, not
convenience. A plain `yser_write` / `yser_write_byte` / `yser_pulse` while a
pulse is pending cancels the pending trailing byte, because a write means
"hold this" and a stale `off` would undo it a few milliseconds later (v0.4
keeps that promise with the arm token as well as with `yrt_worker_cancel`,
because a cancel cannot reach a job the worker has already dispatched). And
`yser_close` stops the worker before it closes anything, which makes the
worker write a pending `off` at once instead of waiting out the width, so a
latching box is never left holding a trigger code. On Windows the worker
carries its own `OVERLAPPED` and event (`pa_ovl`, `pa_event`): a transfer is
reaped by the thread that started it, so it never borrows the user writer's.

That flush is not free: it runs with the worker's lock held and `yser_close`
is blocked in the join behind it, so "at once" is a statement about the
deadline, not about the call returning. It is bounded by a fixed 50 ms rather
than by `write_timeout_ns`, which a trigger sender may well have set to a
second or to `YSER_TIMEOUT_INFINITE`; a close that hangs forever on a wedged
device is worse than a trailing byte that never lands. A healthy device takes
one byte in microseconds, so the bound only ever fires on a device that is
already lost, and the loopback check of this path measures a few hundred
microseconds. The header states the 50 ms, because a promise of "at once" that
is really "up to `write_timeout_ns`" is the kind of claim this collection must
not make.

The Windows worker's sub-millisecond spin used to re-check the quit flag and a
job generation counter, a bug found and fixed here in v0.3 and nowhere else,
which is one of the reasons there is now one worker for the whole collection.
`ysp/rt.h` keeps that behavior (its loop re-evaluates after every spin) and
documents the race that remains: a submit or a cancel issued inside the final
spin window is seen after the spin, not during, so a replacement pulse can be
late by up to the spin window. That window is `YRT_DEFAULT_SPIN_NS`, wider on
Windows (about 1.2 ms) than on Linux and macOS (200 us), for the reasons
`ysp/rt.h`'s WAITS section gives.

### Timing statement

The header states the USB latency budget up front (about 1 ms host-to-device,
16 ms device-to-host on FTDI defaults) so users do not treat a serial pulse as
a parallel-port pulse. "Take your timestamp after the write" was a guess; the
header now says to bracket the call with `yrt_now_ns()` and record both
sides, and to measure the offset to the physical edge once with a scope.
Until v0.5.0 the header had its own `yser_now_us()`, a wrapper of the
`ysp/rt.h` clock in microseconds; v0.5.0 removed it, because every time in
the C API is now nanoseconds on that clock, and a second name for one clock
in a second unit is a way to mix units in a log.

### Threading

One reader thread plus one writer thread per handle, with each function's
role stated in the header (purge RX is reader-side because on Windows
`PurgeComm` aborts an in-flight overlapped read). On POSIX the promise is
free. On Windows it requires `FILE_FLAG_OVERLAPPED` with one `OVERLAPPED`
per direction; synchronous handles serialize all I/O and a blocking read
would stall a trigger write.

Nothing in the library writes shared mutable state except the three
`*_oserr` slots, one per role, and each field of the handle has exactly one
writer once `yser_open` has returned. That is the whole reason the reader and
the writer need no lock between them, and it is why the error message moved
out of every function but `yser_open`.

The async-pulse worker is the one exception, and it is a writer, not a third
role: it shares the writer's `wr_oserr` and every writer-role write is
serialized under its lock. See Async pulse. A reader is never blocked by it,
which is what matters for a listener thread.

### Disconnect mapping

A USB unplug must never look like silence. Windows: `ERROR_ACCESS_DENIED`,
`ERROR_BAD_COMMAND`, `ERROR_DEVICE_NOT_CONNECTED`, `ERROR_GEN_FAILURE`,
`ERROR_NOT_READY`, `ERROR_INVALID_HANDLE` and `ERROR_DEVICE_REMOVED` from a
started or completed overlapped I/O all become `YSER_ERR_DISCONNECTED`.
`ERROR_OPERATION_ABORTED` is ambiguous, because `yser_purge` aborts a pending
transfer with the same code, so the implementation asks `ClearCommError`
whether the port is still there. On a dead port it is a disconnect. On a live
one it was this thread's own purge, because `YSER_PURGE_RX` is a reader-role
call and `YSER_PURGE_TX` a writer-role one: the thread that could have aborted
this transfer is the thread that is in it. A read then resumes its wait rather
than reporting 0, and a write reports what got through, which is the only place
in the write path where a short count is not a timeout. An aborted transfer
still returns any bytes it moved, because `serial.sys` fills in the count even
on a canceled IRP; the interrupted path and the completed-then-aborted path
keep them on the same terms, which they did not in v0.2.

One Windows failure mode has no clean mapping: some VCP drivers never complete
a `ReadFile` that was pending when the device was unplugged. The read does not
fail, it simply never returns, and no timeout applies because the total
timeout is only armed while the driver is alive. `yser_interrupt` is the only
thing that ends such a wait, which is another reason the shutdown recipe does
not depend on the listener noticing a disconnect. POSIX: `POLLHUP`, `POLLERR`
and `POLLNVAL` from `poll`, `read() == 0`, and `EIO`, `ENXIO`, `ENODEV`,
`EPIPE`. Readability is examined before the hangup so bytes that arrived before
it are still delivered; a Linux pty discards them at master close anyway, but a
USB-serial driver need not. macOS has no hangup bit at all, because it waits
with `select()`; see the wait-call section above.

### Enumeration, per platform

Windows: SetupAPI over `GUID_DEVINTERFACE_COMPORT` (not the Ports device
class, which also holds LPT), the openable name from `PortName` in the
device's registry key, vid/pid parsed out of `SPDRP_HARDWAREID`, and the
serial number out of the device instance id. Instance ids come in three
shapes and only one of them holds a serial number: a tail containing an
ampersand is path-derived (`5&2c7b7f6&0&2`) and is not a serial, and an
`FTDIBUS` id carries the serial plus a trailing port letter, which becomes
`location` because FT2232 and FT4232 channels differ only in that letter.
Otherwise `location` is `SPDRP_LOCATION_INFORMATION`
(`Port_#0003.Hub_#0001`).

That leaves one gap on Windows: two FTDI cables with blank EEPROMs both report
serial `""` and location `"A"`, because the port letter wins the field, and
nothing in the SetupAPI properties of the COM device says which USB port they
are in. Telling them apart needs the cfgmgr32 parent walk
(`CM_Get_Parent` up to the hub, then that device's location), which is more
machinery than this is worth until a lab hits it. Until then: label the
cables, write serial numbers into the EEPROMs with FT_Prog, or unplug one and
see which port name disappears. Linux has no such gap, because the sysfs walk
reaches the real USB device either way.

Both platforms enumerate into a scratch list, sort it, and only then copy out
the first `max` entries. Filling the caller's array as ports are met and
sorting afterwards would hand a caller with a four-entry array whichever four
ports the directory or SetupAPI happened to yield first, which is neither the
first four in any order the user can predict nor stable between calls. The
sort is a natural compare on digit runs, so `COM3` precedes `COM10` and
`ttyUSB2` precedes `ttyUSB10`.

Linux: `/sys/class/tty/*` entries that have a `device` link, walking toward
the root for `idVendor`, `idProduct`, `serial`, `manufacturer` and `product`,
and taking `location` from the USB interface directory (`1-1.4:1.0`).
Entries whose `type` attribute reads 0 are dropped: the 8250 driver registers
every port the chipset could have, and a stock machine otherwise lists eight
`/dev/ttyS*` that cannot be opened. That is a narrower filter than
pyserial's, which drops everything on the `platform` bus and so also hides a
real ISA UART, and it survives the newer kernels that moved these devices to
the `serial-base` bus.

macOS: `/dev/cu.*` names only, with no USB fields. IOKit
(`kIOSerialBSDServiceValue`, walking up to `IOUSBHostDevice`) is the way to
get them, but it would put `-framework IOKit -framework CoreFoundation` on
every translation unit that defines the implementation, which is a real
dependency in exchange for a setup-time convenience, in a collection whose
rule is nothing beyond the OS. The node names carry the FTDI serial number
anyway, so `yser_port_filter.name` covers the common case. Revisit if a lab
needs vid/pid matching on a Mac.

### Discovery by filter

`yser_find_ports(filter, out, max)` takes a `yser_port_filter` whose zero or
NULL fields match anything. It keeps the out/max/return contract of
`yser_list_ports` instead of returning one match, because two identical boxes
on one rig is a real configuration and the caller should see both.
`yser_port_info.location` exists because FTDI multi-port bridges share one
serial number across ports, serial-less devices get path-derived instance ids
on Windows, and Cedrus ships the FTDI default vid/pid, so vid/pid alone
matches every FTDI cable on the rig. Serial comparison is case-insensitive
because Windows uppercases instance ids.

### Defaults and DTR

115200 8N1, no flow control, 1 s write timeout, DTR and RTS asserted on open.
These are the pyserial defaults, which every device vendor's example code
assumes. `desc.device` is the only required field because there is no sane
default port name on any platform.

`keep_dtr_on_close` (clear `HUPCL`) is the real fix for Arduino-class boxes
that reset on the DTR edge: the next open then sees no edge. `dtr_low_on_open`
cannot do that job on Linux at all, and the header now says so rather than
calling it best effort: the kernel raises DTR and RTS inside `open()` itself,
`O_NONBLOCK` included, so by the time any user code runs the edge is already
on the wire and the option only drops the line again afterwards.

`keep_dtr_on_close` ended up POSIX-only. The v0.1 plan hoped the Windows
driver would hold DTR while the handle count was nonzero, but the serial
driver drops it inside `CloseHandle` whatever `DCB.fDtrControl` says, and
there is no supported way to stop it. A Windows user who must not reset the
box between sessions has to keep the handle open. The header says so.

### Low latency

`desc.low_latency` is a request, not a requirement, and `yser_port.low_latency`
reports the outcome. Only Linux can honor it at runtime (`ASYNC_LOW_LATENCY`
through `TIOCSSERIAL`, which `ftdi_sio` maps to a 1 ms latency timer). The
header tells Windows and macOS users where the setting actually lives.

The outcome is read back rather than inferred from the ioctl. `cdc_acm`
implements `TIOCSSERIAL`, accepts the flag and does nothing with it, so a
ttyACM device would otherwise report `low_latency = true` while still holding
bytes: a false claim about timing, which is the one thing this collection must
not do. `yser_open` therefore reads
`/sys/bus/usb-serial/devices/<tty>/latency_timer` afterwards and only reports
success when the timer really is 1 ms. `<tty>` is the basename of the device
after `realpath()`, not of `desc.device`: the header tells experiments to open
the stable `/dev/serial/by-id/usb-..._-if00-port0` symlinks, whose basenames
match nothing in sysfs, so without resolving the name first the attribute never
opened and `low_latency` read false on exactly the FTDI devices whose timer
`ftdi_sio` had just programmed. A fixed `PATH_MAX` buffer, because `yser_open`
allocates nothing the caller did not ask for. An unreadable attribute still
leaves `low_latency` false, which is the honest answer for `cdc_acm` and for
every non-USB port. That file is also the supported way to
set it for good, from udev, which survives a re-plug and needs no privilege in
the experiment:

```
SUBSYSTEM=="usb-serial", DRIVER=="ftdi_sio", ATTR{latency_timer}="1"
```

### Reopening an open handle

`yser_open` starts by zeroing the handle, which is also what clears the message
buffer. On a handle that still owns a port that zeroing drops the descriptor or
`HANDLE` and orphans the worker thread, with nothing left to close either with.
One test of `is_open` before the `memset` turns that into a message and a
`false`. It is not a general safety net, because the contract is "zeroed or
closed" in both directions and an uninitialized handle has no valid flag to
read; it catches the mistake that is actually made, which is reopening a handle
the caller forgot to close. The repository's own example and loopback test had
to start zeroing their stack handles when this landed, which is the argument
for the check rather than against it.

`YSER_API` is on the definitions as well as the declarations, the sokol
convention. With the keyword on the declarations alone, `-DYSER_API=static`
does not give a caller a private copy of the library; it gives a compile error,
because the definitions are still external.

### Handle layout

Hot fields first (`is_open`, descriptor or handle, events, per-direction OS
error, write timeout), effective settings next, the two string buffers last,
so the read and write paths do not straddle 384 bytes of text. Platform
fields are public, as in `ysp/parallel.h`, for inspection rather than for
writing; the implementation needed no field the specification did not already
declare. The `yrt_worker` handle is not one of them: it sits in the
heap-allocated `yser__async` block that `yser_open` already allocates, not in
`yser_port`, so the public handle the bindings mirror does not grow half a
kilobyte of opaque OS storage and its size does not depend on whether the build
has threads. Nothing allocates on a read, write or pulse path either way.

The Linux feature-test macro is `_DEFAULT_SOURCE`, not `_POSIX_C_SOURCE`:
glibc hides `CRTSCTS`, `CMSPAR` and every `B*` rate above 38400 behind
`__USE_MISC`, and an explicit `_POSIX_C_SOURCE` switches `__USE_MISC` off for
the whole translation unit. Since `ysp/rt.h` v0.2 both headers ask for the same
macro, on their first include, and both stand down when any of
`_DEFAULT_SOURCE`, `_GNU_SOURCE`, `_POSIX_C_SOURCE` or `_XOPEN_SOURCE` is
already set. The include order between them therefore does not matter. The one
rule that remains is the rule for any system header: an implementation file
that includes other system headers first defines `_DEFAULT_SOURCE` itself.

### Nanoseconds and version macros (v0.5.0)

v0.5.0 applies the repository conventions (README.md, Conventions). Every
time a caller writes is an `int64_t` count of nanoseconds: the pulse width
(`yser_pulse()`, `yser_pulse_async()`), the read timeout (`yser_read()`),
the break duration (`yser_send_break()`) and `yser_desc.write_timeout_ns`.
The old parameters were `uint32_t` microseconds and milliseconds. The new
names say the unit, so a caller that still sets `write_timeout_ms` gets a
compile error instead of a timeout one million times too short.

Decisions:

- **The OS waits stay in milliseconds.** `COMMTIMEOUTS`, `poll()` and
  `select()` (as this header uses it) take whole milliseconds. The entry
  points convert once and round up, so a 0.5 ms timeout waits 1 ms instead
  of becoming a poll. The internal code keeps its millisecond deadlines.
- **`YSER_TIMEOUT_INFINITE` is -1.** Another negative read timeout polls, so
  a timeout computed as a deadline minus now does not block after the
  deadline. A negative `write_timeout_ns` other than -1 fails `yser_open()`
  with a message, and a negative width or break duration returns
  `YSER_ERR_ARG` before any write.
- **`yser_now_us()` is gone.** Callers bracket a write with `yrt_now_ns()`.
  The Python and MATLAB bindings keep their `now_us()`, computed from
  `yrt_now_ns()`, and keep their millisecond and microsecond arguments and
  their `TIMEOUT_INFINITE` of 4294967295: the C glue converts.
- **Return codes stay as they were.** Every call except `yser_open()` may run
  on a reader or a writer thread, so none of them writes the message (README,
  Conventions, Errors: the second-thread exception). They return a
  `YSER_ERR_*` code.

`YSER_VERSION_MAJOR`, `_MINOR`, `_PATCH` and `_STRING` are new, as in
`ysp/rt.h`. The CHANGELOG and STATUS moved to the end of the top comment, and
the USAGE block zeroes the desc and sets fields, so it builds as C99 and
C++17.

Checked on 2026-10-10 on Windows 11: `tests/compile/serial.c` (C11, C++17,
`YSER_NO_THREADS`) now names every entry point by type and calls the time
entry points on a closed handle; MSVC 19.44 (/W4 /WX) and MinGW-w64 gcc 16.1
(-Werror) build the tree. No port was opened: as before, no byte has moved
through the Windows backend.

## Tests

- Compile checks: C11 and C++17 on Windows, Linux, and macOS
  (`tests/compile/`, run by CMake and CI, warnings as errors). The `.cpp`
  wrapper should still be extended to call every public function once, so
  `extern "C"` and const-correctness are exercised and not just parsed.
- Double implementation, by hand: a translation unit that defines both
  `YSP_SERIAL_IMPLEMENTATION` and `YSP_RT_IMPLEMENTATION` compiles and links in
  either include order, as C11 and as C++17, because `ysp/rt.h`'s
  implementation block guards itself. Since `ysp/rt.h` v0.2 the `ysp/rt.h`-first
  order needs no `_DEFAULT_SOURCE` of its own: `ysp/rt.h` sets it on its first
  include, implementation or not, so `CRTSCTS` and `realpath` are still there
  when `ysp/serial.h` is compiled afterwards.
- Loopback: `tests/loopback/serial_loopback.c`, built with
  `-DYSP_BUILD_LOOPBACK=ON` and run by hand with two device names, either a
  virtual pair (`socat -d -d pty,raw,echo=0 pty,raw,echo=0`; com0com on
  Windows) or one port name twice with TX tied to RX. It checks write N then
  `YSER_READ_ALL` N and the payload, `YSER_READ_ANY` with timeout 0 on an idle
  port (0 bytes, under 2 ms, so a poll cannot quietly become a wait),
  `YSER_READ_ANY` with a 100 ms timeout on an idle port (0 bytes after 95 to
  160 ms), that `yser_open` on an already-open handle is refused and leaves
  the handle usable, `yser_available`, purge, the short count at a `YSER_READ_ALL`
  deadline (200 ms nominal, 195 to 260 ms accepted), the write timeout with a
  peer that has stopped draining (a short count after 95 to 160 ms, skipped
  with a note if the transport never blocks), `yser_interrupt` on a reader
  blocked in `YSER_TIMEOUT_INFINITE` (under 10 ms), `yser_find_ports` with a
  zero filter, a reader thread plus a writer thread on one handle, and that no
  latched interrupt is left over afterwards. Not registered with ctest: no CI
  runner has a port pair.
- Async pulse, on the same pty pair: `yser_pulse_async(0xA1, 0xA2, 2000)`
  then two `YSER_READ_ANY` reads, timestamping each byte as the reader sees it,
  which is the spacing a device would see (the worker's deadline plus one
  transport hop at each end); a plain write while a pulse is pending, which
  must cancel the trailing byte; thirty rounds of the same cancel raced against
  the worker's own dispatch, alternating a zero width (due the moment it is
  submitted) with 200 us, where either order on the wire is allowed but the
  last byte must always be the write that means "hold this"; two pulses 1 ms
  apart with different `off`
  bytes, which must yield two onsets and only the newer `off`; and
  `yser_close` on a third handle with a 1 s pulse pending, which must put the
  trailing byte on the wire within 5 ms of the close instead of after the
  width. The concurrency phase also issues an async pulse every eighth write,
  so the worker is writing into the stream while the writer thread writes and
  the reader reads: three threads on one handle.
- Two ordering details the test learned the hard way. A phase that fills the
  transmit path leaves tens of kilobytes inside socat, and a `yser_purge` does
  not stop them arriving, so the test drains both ends until the transport is
  quiet for 200 ms before any check that expects silence. And the interrupt
  that stops the hammer threads can land after the reader has already left on
  its stop flag, leaving the wake latched by design, so the test consumes it
  with one read before the disconnect check.
- Pure logic that no loopback can reach (the natural-order name compare, the
  location prefix match, the device-name length limit, and the `YSER_ERR_CLOSED`
  return of every function on a closed handle) is covered by a throwaway probe
  during development rather than by a committed test. Worth promoting to
  `tests/` if that logic grows.
- What a socat pty pair does not test: closing one slave does not hang up the
  other, because socat still holds both masters, so the disconnect check is
  informational there. Closing the master does hang up the slave, and
  `yser_read`, `yser_available` and `yser_write` then all return
  `YSER_ERR_DISCONNECTED`, which is how the POSIX disconnect path was
  verified.
- Concurrency: the same loopback binary built with `-fsanitize=thread` and
  run on a socat pair, one reader thread and one writer thread on one handle
  while the far end is fed and drained. Clean, as is the same run under
  `-fsanitize=address,undefined`. This is what validates the
  error-code split. It does not prove the split is complete, because a
  function nobody calls concurrently in the test cannot race in the test; the
  argument for `misc_oserr` and for the message buffer belonging to
  `yser_open` is the role table, not the sanitizer.
- Timing: not measured yet. A loopback round trip gives write-accept to
  first-byte latency. For trigger-onset latency use a scope or a second box
  sampling the trigger line, and record the distribution (median, 99th
  percentile), with and without `low_latency`. Add Tracy zones around the
  driver calls if the numbers need explaining.
  `tests/compare/serial_bench/` is the bench for the MATLAB binding; see
  [Compared with MATLAB serialport and IOPort](#compared-with-matlab-serialport-and-ioport).

## Compared with MATLAB serialport and IOPort

`tests/compare/serial_bench/serial_bench.m` runs the same measurements on
three MATLAB APIs: MATLAB's `serialport`, Psychtoolbox's `IOPort` and the
`ysp_serial` MEX binding. Its README tells how to run each part and how to
read the output. This section records what ran and what did not.

### What the bench measures

1. Call cost: open and close; write of 1, 8, 64 and 512 bytes; read with
   nothing waiting and with bytes waiting; the bytes-available query; the
   cheapest call into each API; port enumeration; open of a port that does
   not exist. Each API is timed with its own clock (`tic`/`toc`, `GetSecs`,
   `ysp_serial('now_us')`), and the cost of each clock is measured too.
2. Round trip: a 13-byte command `e <seq>\n` and its answer, with a blocking
   read and with a poll loop. With the echo board
   (`firmware/ysp_echo/ysp_echo.ino`), a clock fit splits the round trip into
   host to board and board to host. The fit keeps the exchange with the
   smallest path time in each 200 ms, as ysp/rt.h's BRACKET mode does, and
   assumes equal one-way delays for those exchanges. The variation of each
   one-way delay is measured; its constant part is known to plus or minus half
   the best path time, which the bench reports as `fit_unc`.
3. Receive stamps: IOPort gives one (`when`, from `Read`). serialport gives
   none from a read, and `evt.AbsTime` (a `datetime`) in a callback. ysp gives
   none; the stamp is `now_us` after the read returns. Against the echo
   board's send stamp, mapped with the fit, each API's stamp gets a latency
   distribution.
4. Stream: the board sends 1000 lines per second for 10 s. The bench counts
   missed lines and lines that arrive in one read with another line, measures
   the latency of each line against its board stamp and the MATLAB process
   CPU use, for a blocking read per line, a poll loop, and the asynchronous
   mode of each API (`configureCallback`, IOPort's background reader; the
   ysp MEX binding has none).
5. The FTDI latency timer: the loopback round trip at 16 ms and at 1 ms.

### Echo board firmware

`firmware/ysp_echo/ysp_echo.ino` is a separate sketch, not a command in
`firmware/ysp_line/`. ysp_line sends a sync line every 100 ms, and an
unsolicited line in the middle of a timed echo breaks the fixed answer
lengths that the bench reads. The sketch answers `e <seq>` with
`R <seq> <t_rx> <t_tx>`, `q <seq>` with the ysp line v1 answer
`Q <seq> <t_rx>`, and `s <hz> <n>` with n ysp line v1 sample lines
`A <t> 1 <k>`. Every number is zero-padded to 10 digits, so each kind of
line has one length. It compiles without warnings with arduino-cli 1.5.1
for `teensy:avr:teensy40` (Teensy core 1.62.0), `teensy:avr:teensy41` and
`rp2040:rp2040:rpipico` (arduino-pico 6.3.0). It has not run on a board.

### Results without hardware

Conditions: 2026-10-08, Windows 11 Enterprise 10.0.26200, Intel Core
i7-1360P, AC power, quiet machine (the shared guard waited for a CPU load of
20 % or less and no compiler). MATLAB R2023a Update 6 in `-batch` mode.
`ysp_serial.mexw64` built from this tree with `bindings/mex/build.m` and
Microsoft Visual C++ 2022. `IOPort.mexw64` and `GetSecs.mexw64` from
Psychtoolbox 3.0.19.16 (see below). No serial port exists on this machine,
so only the `clock` and `noport` parts ran. Two runs; each cell gives run 1,
then run 2. Times in microseconds. With n = 50, p99 is the maximum.

| API | Metric | n | Median | p99 | Max | Mean |
|---|---|---|---|---|---|---|
| serialport | `tic`/`toc` pair | 100000 | 0.3, 0.3 | 0.5, 0.5 | 128, 84 | 0.31, 0.28 |
| IOPort | `GetSecs` pair | 100000 | 4.5, 4.3 | 6.2, 5.7 | 96, 168 | 4.55, 4.42 |
| ysp | `now_us` pair (1 us steps) | 100000 | 1, 0 | 1, 1 | 33, 68 | 0.55, 0.50 |
| IOPort | `IOPort('Verbosity')`, cheapest call | 100000 | | | | 0.83, 0.80 |
| ysp | `ysp_serial('now_us')`, cheapest call | 100000 | | | | 0.57, 0.49 |
| serialport | `serialportlist("all")`, 0 ports | 200 | 1744, 1691 | 19400, 13130 | 181700, 146300 | 2920, 2931 |
| ysp | `ysp_serial('list')`, 0 ports | 200 | 48, 42 | 221, 90 | 293, 193 | 53, 45 |
| serialport | open of an absent port (error) | 50 | 26530, 26800 | 502300, 507800 | | 38530, 39440 |
| IOPort | open of an absent port (returns -1) | 50 | 8.3, 8.2 | 374, 348 | | 22.0, 20.3 |
| ysp | open of an absent port (error) | 50 | 118, 114 | 473, 471 | | 139, 134 |
| MATLAB | `try; error(...); catch; end` alone | 50 | 97, 93 | 306, 295 | | 109, 105 |

The cheapest-call rows are the mean of one batch of 100000 calls, so they
have no distribution. IOPort has no port enumeration. serialport has no call
without a port object.

What the table shows:

- `tic`/`toc` is the cheapest clock. `GetSecs` costs about 4.4 us per call,
  about 15 times a `toc`. The IOPort dispatch costs 0.8 us, so the rest is
  in `GetSecs` itself, not in the MEX call. `ysp_serial('now_us')` costs
  about 0.5 us but counts whole microseconds, so a call shorter than 1 us
  shows as 0 or 1; use the mean for those.
- The MEX dispatch of IOPort and of ysp_serial costs less than 1 us.
- serialport's port enumeration and its failing open cost milliseconds: the
  cost is in MATLAB's object layer, because no port exists.
- A failing open costs ysp_serial about 115 us, and MATLAB's error mechanism
  alone costs about 95 us of that. IOPort, asked for two outputs, returns -1
  and a message instead of raising an error, so its failing open is cheaper.
  Neither number is the cost of a successful open.

Not run, because each part needs a serial port or a board:

| Part | Needs |
|---|---|
| Open, close, write, read, bytes-available call costs | Any serial port (an adapter with nothing attached is enough) |
| Read with bytes waiting, round trip, FTDI latency timer | (a) a USB-serial adapter with TX connected to RX |
| One-way split, receive-stamp quality, stream, CPU use, async modes | (b) a Teensy 4.0 or 4.1, or a Pico, with `ysp_echo` |

Option (b) gives every result of option (a) except the FTDI latency timer,
so (b) is the better choice. Option (a) is only for the latency timer.

### Psychtoolbox on this machine

The Psychtoolbox on the MATLAB path is a git checkout from January 2026
(`Contents.m`: 3.0.22). Its `IOPort.mexw64` does not load: it links
`LexActivator.dll`, the license manager of Psychtoolbox 3.0.20 and later on
Windows, which is not installed. `GetSecs.mexw64` of that checkout loads.
The bench used `IOPort.mexw64` and `GetSecs.mexw64` of Psychtoolbox
3.0.19.16, the last release without a license check, downloaded from the
Psychtoolbox-3 GitHub repository into `C:\tmp\psy-work\serialbench\ptb-3.0.19.16\`.
No installation and no administrator rights were necessary. To use the
installed Psychtoolbox instead, set up its license (`PsychLicenseHandling`)
and omit `ioport_dir`.

### Hand procedure for the hardware parts

Use AC power and close other programs. Start MATLAB, then run
`cd <repo>/tests/compare/serial_bench`.

Option (a), the FTDI loopback, about 10 minutes in total (estimated):

1. Take an FTDI FT232R adapter. On the TTL-232R cable, connect TXD (orange)
   to RXD (yellow). On a DB9 RS-232 adapter, connect pin 2 to pin 3.
2. Connect it. In Device Manager, under **Ports (COM & LPT)**, find
   **USB Serial Port (COMn)**.
3. Run
   `serial_bench('port', 'COMn', 'device', 'loopback', 'ioport_dir', 'C:\tmp\psy-work\serialbench\ptb-3.0.19.16', 'label', 'FT232R 16 ms')`.
4. In Device Manager, open the port, select **Port Settings > Advanced**,
   set **Latency Timer (msec)** to 1 and select **OK**. Disconnect the
   adapter and connect it again.
5. Run step 3 again with the label `'FT232R 1 ms'`.

Option (b), the echo board, about 10 minutes in total (estimated):

1. Flash the board. Teensy 4.0: open `teensy.exe` from
   `C:\tmp\psy-work\devices\arduino\data\packages\teensy\tools\teensy-tools\1.62.0\`,
   open the HEX file
   `C:\tmp\psy-work\serialbench\fw-teensy_avr_teensy40\ysp_echo.ino.hex`
   (`fw-teensy_avr_teensy41` for a 4.1), connect the board and push its
   button. Pico: hold BOOTSEL, connect the board, and copy
   `C:\tmp\psy-work\serialbench\fw-rp2040_rp2040_rpipico\ysp_echo.ino.uf2`
   to the `RPI-RP2` drive.
2. Find its COM port in Device Manager (**USB Serial Device (COMn)**).
3. Run
   `serial_bench('port', 'COMn', 'device', 'echo', 'ioport_dir', 'C:\tmp\psy-work\serialbench\ptb-3.0.19.16', 'label', 'Teensy 4.0 echo')`.

Each run prints its result folder. Send the whole folder: `summary.txt` has
the numbers, the other CSV files have every sample.

## Deferred

- Custom baud rates on Linux (`termios2`/`BOTHER`) once a target device
  needs one.
- `yser_wait_lines` if a device that signals on CTS/DSR shows up.
- ~~A clock header (`ysp/clock.h`) if a third library needs `yser_now_us`.~~
  Done in v0.4, as `ysp/rt.h`, and it took the waits and the worker with it.

## Bindings

Done, both of them.

- `bindings/python/ysp_serial/`: distribution `ysp-serial`, module
  `ysp.serial` (a PEP 420 namespace package shared with `ysp-parallel`),
  Limited API 0x03080000, one `ysp/serial.abi3.so`. A heap-type `Port` built
  with `PyType_FromSpec`, constructed with keywords that mirror `yser_desc`
  (`device` required; `baud`, `data_bits`, `parity`, `stop_bits`, `flow`,
  `write_timeout_ms`, `exclusive`, `low_latency`, `dtr_low_on_open`,
  `rts_low_on_open`, `keep_dtr_on_close`, `rt_runtime_ns`, `rt_deadline_ns`,
  `rt_period_ns`). Methods: `read(n, timeout_ms, all=False) -> bytes`,
  `write(data) -> int`, `write_byte(value) -> int`, `pulse(on, off, usec)`,
  `pulse_async(on, off, usec)`, `available()`, `drain()`,
  `purge(rx=True, tx=False)`, `interrupt()`, `set_dtr(on)`, `set_rts(on)`,
  `get_lines()`, `send_break(ms)`, `close()`, and the context-manager pair.
  Read-only properties for the effective settings, `async_policy` and the
  three `*_oserr` slots. Module level: `list_ports()`, `find_ports(vid=0,
  pid=0, serial_number=None, location=None, description=None, name=None)`,
  and a constant per `YSER_*` enum, flag and default. Errors are `Error` with
  `Disconnected`, `Interrupted` and `Closed` derived from it, carrying the
  `yser_strerror()` text and the OS error of the failing role. `read`,
  `write`, `drain`, `pulse`, `send_break` and `pulse_async`'s onset release
  the GIL. The one-reader-one-writer rule and the interrupt-join-close
  shutdown recipe are in its README; Python threads can otherwise `close()`
  mid-read.
- `bindings/mex/ysp_serial.c`: command dispatch (`list`, `find`, `open`,
  `write`, `writebyte`, `read`, `available`, `drain`, `purge`, `interrupt`,
  `pulse`, `pulseasync`, `setdtr`, `setrts`, `lines`, `break`, `info`,
  `close`), uint64 handles in a table that `mexAtExit` drains, and negative
  codes raised as `error()` with the identifier `ysp_serial:<code name>`.
  Picked up by the existing `build.m`; no platform gate, because the header
  supports all three.
