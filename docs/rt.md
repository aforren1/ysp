# ysp/rt.h design notes: event ring, clock correlation, instrumentation

This note explains why the v0.5.0 additions to `ysp/rt.h` have their
current form, and records the measurements that decided them. The manual
in the header tells you how to use them. The plan that asked for them is
section 4.1 of [rig_spec.md](rig_spec.md).

All numbers come from `examples/rt/ring_bench.c` on one machine: Windows 11
with 16 logical CPUs, built with MSVC 19.44 and MinGW-w64 gcc 16.1, and
WSL2 Ubuntu 22.04 with gcc 11.4 on the same machine (8 vCPUs), and emcc
6.0.10 under node in the `emscripten/emsdk` Docker image. Run the bench on
your rig before you trust any of them.

## The push is wait-free, and that takes two counters

The spec asks for a ring that any thread pushes into without a lock. An
audio callback and a present thread are among the producers, so a push must
never wait for another thread. A compare-and-swap loop (Vyukov's bounded
queue, the Disruptor) is lock-free but not wait-free: one producer can lose
the race any number of times.

The push uses fetch-add, which is one instruction on x86-64, on ARMv8.1 and
later, and in WebAssembly. A ticket counter alone is not enough. A producer
that finds the ring full already holds a ticket whose slot it cannot fill,
and the drainer cannot tell that hole from a producer that is still
writing. So a second counter, `used`, admits a push only while a slot is
free. A refused push undoes its increment and leaves without a ticket. Each
push is then two or three fetch-adds, a 60-byte copy and one release store,
whatever the other threads do.

The cost: with P threads inside a push at once, the ring can refuse a push
when it is within P - 1 records of full.

Alternatives we did not use:

- **One lane per producer thread** (Tracy's design) has no shared counter.
  But a thread needs a lane for every ring it pushes to, a table to find
  it, and a way to give the lane back when the thread ends. It stays the
  fallback if the shared counters ever fail their bar. They did not.
- **Overwrite the oldest record.** A producer that laps a descheduled
  producer writes the same slot, and no single-pass scheme stops a torn
  record that the drainer cannot detect. Refusing the newest record keeps
  every record whole and every gap counted.
- **A mutex.** Priority inversion against the audio thread. It is the
  baseline in the benchmark (B3 below).

## The record is one cache line

A slot is 64 bytes, aligned to 64. Adjacent tickets go to different
producers, so one record per line means two producers never write the same
line at the same moment. The drainer pulls one line per record.

The payload is 40 bytes because the largest planned record, the per-flip
record of `ysp/screen.h`, needs 36: the target time (8), the dropped count
and the mode (two u16), and six phase durations (six u32). Its `t_ns` is the
estimated onset and `aux` is the display index. A 32-byte record leaves 16
bytes of payload, which does not hold it. A 128-byte record doubles memory
and drain traffic for zones, which are most of the records in trace mode.

The publish word, `seq`, comes first, so a push copies bytes 4 to 63 in one
piece and then stores `seq` with release. A copy that included `seq` could
show the drainer a stale value before the release store. The test proves
the order matters: a copy of the header that publishes before it copies
fails the stress test in every run.

## `used` and `head` on separate cache lines

The design note put the two counters on one line, on the argument that a
push does both and the second would hit in cache. The measurement says
otherwise. Mean ns per push with 8 producers back-to-back and one drainer,
three runs each:

| Build | One line | Two lines |
|---|---|---|
| MSVC, Windows 11 | 573, 427, 546 | 405, 476, 415 |
| gcc, WSL2 | 689, 821, 785 | 588, 627, 600 |

The two-line layout also pushed more records per second at 2, 4 and 8
producers on both (MSVC 14 to 17 million against 12 to 14 million). The
drainer's once-per-batch update of `used` no longer takes the line the
ticket counter is on. The one-line layout was deleted. The handle has four
groups 128 bytes apart, so no two share a line from any 8-byte-aligned
address.

## Loss is reported in band

A full ring refuses the new record and counts it. The next drain puts a
`YRT_KIND_LOSS` record first in its output with the count since the last
drain and the total. A writer that copies every record out cannot leave the
loss out of its file. A refused push takes no ticket, so `seq` has no gaps
and a gap never means loss. The stress test checks that drained plus
refused equals pushed exactly, for 2, 4 and 8 producers.

## A stalled producer stalls the drain

Records come out in ticket order, so the drain stops at a record whose
producer took a ticket and has not yet written it. The window is a few
dozen instructions, but a normal-priority producer that the OS deschedules
inside it holds the drain until it runs again. The ring then fills, and the
next pushes are refused, from every thread. The audio thread never waits,
but its records can be the ones lost.

This is not hypothetical. In B3 (7 normal-priority producers at 1.1 million
records a second), the drain waited behind an unpublished push for up to
5.2 ms on Windows and 2.6 ms in WSL2 while the drainer polled throughout,
and the elevated producer had 4 to 684 of 5000 pushes refused per run.

B3c holds one ticket unpublished for a set time while a producer pushes at
6000 records a second into 4096 slots, the load of a full rig:

| Stall | Pushes during it | Refused (Windows) | Refused (WSL2) |
|---|---|---|---|
| 100 ms | 600 | 0 | 0 |
| 400 ms | 2400 | 0 | 0 |
| 600 ms | 3600 | 0 | 0 |
| 800 ms | 4800 | 705 | 674 |
| 1200 ms | 7200 | 3105 | 3105 |

Refusals start between 0.6 and 0.8 s, as 4096 / 6000 = 0.68 s predicts. So
the manual tells you to size the ring as capacity / rate = the longest
stall you expect, and to watch the log for LOSS records. B3b put a
below-normal producer on a machine with twice as many spinning threads as
CPUs for 10 s (Windows) and 25 s (WSL2). The elevated producer had no push
refused and no stall was seen. The below-normal producer mostly did not
run.

## Instrumentation: one record per zone, a plot for Tracy's clock

In `YRT_TRACE_RING` mode a zone is one record pushed at its end, with the
begin time and the duration. A begin and an end record would cost twice
the pushes and the memory. Nesting is time containment per thread, which is
what a Chrome trace "X" event means. A zone that an early return leaves
open is not recorded, which is safer than a begin with no end.

In `YRT_TRACY` mode the frame mark carries the ysp_rt time so a capture
can be put on the rig's clock. The spec said a message. A plot point does
the same job for less, because Tracy copies a message into its queue and
the text must be formatted first:

| Tracy v0.14.1 | Plot, ns | Message, ns |
|---|---|---|
| MSVC, on demand, not connected | 40 | 159 |
| MSVC, buffering | 60 | 264 |
| gcc WSL2, on demand, not connected | 19 | 69 |
| gcc WSL2, buffering | 43 | 119 |

The plot is what the header does. The message form was deleted.

`yrt_now_ns()` on Windows asks for the QPC frequency on every call. A zone
reads the clock twice, so we measured a version with the frequency cached.
Over five runs it was 27 against 27 ns, 32 against 34, 31 against 29, 36
against 37 and 32 against 43: no consistent gain. The clock stays as it
was.

## Correlation: the width is never 0, and edge mode is short

On Windows the ysp_rt clock ticks every 100 ns, so the two reads around the
other clock can return the same value. A width of 0 would claim an exact
match and break a 1 / width^2 weight. The width is therefore at least the
ysp_rt clock's tick. On Windows every plain correlation in B6 reported 100
ns.

Edge mode spins until the other clock changes. GetTickCount64 changes every
15.6 ms at the default Windows tick, so 16 edges would spin for 250 ms. The
default is 4 edges, which measured 62 ms per call.

The edge-mode timeout (100 ms by default) is wall-clock time. On
2026-10-22 the test's case on a 1 ms clock failed in 2 of 10 runs with
128 busy threads on 16 logical processors: the timeout ran out after 3
edges, or the 4 edges took more than its 50 ms limit. The header was
right; the test was not. That case now sets a 30 s timeout and checks a
lower bound only (4 edges of a 1 ms clock take at least 3 ms). A frozen
clock with the default timeout checks that the call gives up after at
least 100 ms. Mutants `corr-01` to `corr-03` in `tests/mutate/rt.toml`
check the edge count, the default timeout and the edge rule.
After the change: 0 of 30 runs failed under the same load (MSVC Debug,
ctest).

`yrt_ticks_to_ns()` exists so that a QPC value from another API (DXGI's
SyncQPCTime) needs no correlation at all. It is the function
`yrt_now_ns()` uses, and the test checks it against a 128-bit reference.

## Drift was for ysp/audio.h; v0.6.0 has a device clock fit

v0.5.0 had no drift fit. Nothing in it used one, and the estimator a device
clock needs is not ordinary least squares: callback stamps are late and
never early, so the fit wants a lower envelope. `ysp/audio.h` then fitted
offset and rate itself and measured the estimators. v0.6.0 adds a fit for
other devices; see [The device clock fit (v0.6.0)](#the-device-clock-fit-v060).

## The device clock fit (v0.6.0)

A response box, a microcontroller or an eye tracker stamps events with its
own clock. `docs/devices_spec.md` (section 6) asked for one fit that maps
such a clock to the ysp_rt clock, in `ysp/rt.h`, because three places
already needed one: `ysp/audio.h` has its own, the photodiode loopback had
an ad hoc line, and the response-box proposal had a third. The user
approved the home on 2026-10-08 (devices_spec section 16, default 1).
`ysp/audio.h` keeps its own fit until a measurement shows the same results
with this one.

### Decisions

| Question | Decision | Why |
|---|---|---|
| Estimators | LATE: the lower envelope. BRACKET: LATE on the bracket ends, with wide brackets refused. UNBIASED: bucket means and least squares | A USB report is late, never early: least squares would put the mean delay into the map. `ysp/audio.h` measured the envelope against least squares on callback times (207 against 359 us). BRACKET is PsychDataPixx's minimum-offset filter with Psychtoolbox's 1.3 ms acceptance bound (devices_spec section 2) |
| Which line under the points | The hull edge that spans the mean x (Moon, Skelly and Towsley, INFOCOM 1999) | It minimizes the summed gap. The test checks it against a brute force over every pair of points, within 2 ns |
| Thinning | One point per 200 ms bucket of host time: the least late pair, or the mean | Fixed memory, and the minimum per bucket keeps what the envelope needs. `ysp/audio.h` measured 200 ms |
| Window | 1024 points, fixed in the struct (about 40 KB) | The struct's size must agree in every translation unit. 205 s at 200 ms |
| Anchor | The newest point | A recent event maps over a short tick distance, so slope error matters less |
| Slope | Nominal until the points span 5 s; the offset from the last second until then | `ysp/audio.h` measured that a slope from a shorter span was worse than the drift |
| Wrap | Unwrap by the tick the map expects at the pair's host time | `micros()` wraps every 71.6 minutes of a board's uptime; a run can cross it |
| Restart | Hold a pair more than 50 ms off the map; restart when held pairs agree with each other for 1 s | A timer reset (XID's `e5`) is permanent; a burst after an OS stall arrives within milliseconds. The first version shifted the oldest held pair out after eight, so at 100 pairs a second the hold never spanned 1 s: the restart test found it, and mutant `fit-07` keeps it found |
| Refit | At each new bucket, at each pair while 16 points or fewer, and for a LATE or BRACKET pair under the line | A full refit costs 9 to 23 us. The first version also refitted UNBIASED pairs under the line, which is half of them; the benchmark found it (95,576 refits instead of 950) |
| Records | `YRT_KIND_CLOCK` per closed bucket (the existing kind, plus the epoch in `u.u64[2]`), `YRT_KIND_FIT` (new, 8) per refit at a new bucket | An offline fit of the whole session sees the points after each event too. Psychtoolbox's PsychRTBox and XDF files keep the raw pairs for the same reason |
| The map's API | `int64_t yrt_fit_map()`, 0 before the first pair | The spec's draft. 0 is never a ysp_rt time a rig sees |

### Measured on synthetic devices

Seeded devices with a known offset and rate; the error is the map minus
the true line at each new pair's tick, which is when a reader maps an
event. Four seed sets (the test's, +100, +200, +300). "From 205 s" is with
a full window.

| Scenario | Path | From 10 s, us | From 205 s, us | Rate error |
|---|---|---|---|---|
| LATE, ms timer, 100 pairs/s, +100 ppm | 1 ms USB frame, 0.1 ms exponential tail, 16 ms bursts (1%), 60 to 200 ms stalls (0.2%) | -75 to +240 | -47 to +91 | 0.3 ppm or less |
| LATE, us counter wrapping at 200 s, -80 ppm | as above | -12 to +46 | -5 to +18 | 0.05 ppm or less |
| LATE, ms timer, counter reset at 300 s | as above, no stalls | -98 to +284 (10 s after the reset excluded) | -98 to +247 | 0.9 ppm or less (the second epoch is short) |
| LATE, 300 ms stall at 200 s | as above | -70 to +214 | +4 to +113 | 0.4 ppm or less |
| LATE, a press every 2 to 4 s | a 16 ms latency timer's phase | -1359 to +8663 (from 2 min) | -1359 to +3201 | 0.6 ppm or less |
| BRACKET, a query every 100 ms | brackets of 0.4 to 3 ms, over 1.3 ms refused | -327 to +628 | -42 to +110 | 0.1 ppm or less |
| UNBIASED, us counter, -50 ppm | symmetric noise, SD 115 us | -23 to +22 | -5 to +7 | 0.02 ppm or less |
| LATE, the first 5 s (0.3 to 4.9 s), +500 ppm | 1 ms frame, 0.1 ms tail | -541 to +160 at the pair; -641 to +60 200 ms ahead | | nominal slope |

What the table says:

- Dense pairs give a map within tens of us of the truth once the window
  holds a few minutes. The early minutes are worse: the BRACKET case was
  327 us early between 10 and 20 s, from a slope fitted over a short span.
- Sparse pairs behind a coarse timer give millisecond errors for minutes:
  the envelope needs many deliveries to find the timer's floor. A box that
  reports only presses needs BRACKET probes (XID's `_e5` query) or a 1 ms
  latency timer, or both. This is the case `ysp/device.h` must handle.
- The rate converges to the truth within a ppm in every case.
- None of this is a device. The table bounds the estimator on modeled
  paths; the path's minimum delay, which LATE cannot see, needs a loopback
  (devices_spec section 12).

A counter reset restarted the fit once, 1.0 to 1.3 s after the reset (the
1 s hold plus the bucket). A 300 ms stall, whose 30 late pairs arrive
together, did not restart it.

Cost, `fit_bench.c` (a scratch program, not kept): 1000 pairs a second
with a full window, `yrt_thread_elevate()`, under the timing guard, two
runs with MinGW gcc 16.1 -O2 and one with MSVC 19.44 /O2, AC:

| Mode | Refit, p50 / p99 / max us | Pair without a refit, p99 | Refits per second |
|---|---|---|---|
| LATE | 11.1 to 22.5 / 14.9 to 28.3 / 19.7 to 49.8 | 0.1 us | 5 (one per bucket) |
| BRACKET | 11.0 to 22.2 / 14.1 to 28.0 / 19.8 to 40.1 | 0.1 us | 5 |
| UNBIASED | 9.2 to 9.7 / 13.0 / 13.4 to 36.0 | 0.1 us | 5 |

The clock ticks at 100 ns, so "0.1 us" is one tick. The first gcc run was
about 1.7 times slower than the second for every refit; the cause (a core
type, a clock speed) was not looked for.

Mutants (`tests/mutate/rt.toml`, the fit's part of the test only, about 1
s a run): 17 of 17 caught. `fit-03` (the offset from the whole window) and
`fit-15` (a slope at once) survived the first run, because no check looked
at the first 5 s; `test_fit_early` was added for them.

Not done: any real device. The first is the MCU photodiode of
`tests/loopback/screen_loopback.c`, which uses this fit from
2026-10-08 (LATE, `micros()`, 32 bits) instead of its line through the
least-late pair of the first and last quarters. It prints the old line's
distance from the fit over the edges, for the first hardware runs.

## Per-user folders (v0.7.0)

`yrt_user_dir(kind, subdir, out, cap)` gives `<base>/ysp/<subdir>`:
`YRT_DIR_CACHE` for files a program can make again (the GL program
cache), `YRT_DIR_CONFIG` for files to keep (the rig profile of
`docs/devices_spec.md` section 11.2). The coordinator asked for it on
2026-10-08, with the user's approval, so that the rig profile and the
program cache follow one set of rules.

| Question | Decision | Why |
|---|---|---|
| Home | `ysp/rt.h`, moved from `ysp/gfx.h` | The rig profile is read by the device layer and the player, which do not include `ysp/gfx.h`; every header has `ysp/rt.h` |
| `ygfx_default_cache_dir()` | A one-line wrapper: `yrt_user_dir(YRT_DIR_CACHE, "progcache", ...)`; gfx v0.10.2 | Same behavior: its test passes unchanged on Windows, WSL2 and under emcc, and its mutant `v10-05` now edits `ysp/rt.h` and is still caught by gfx's test |
| Bases | CACHE: `%LOCALAPPDATA%`, `$XDG_CACHE_HOME` or `~/.cache`, `~/Library/Caches`. CONFIG: `%APPDATA%`, `$XDG_CONFIG_HOME` or `~/.config`, `~/Library/Application Support` | The platforms' documented per-user locations. `%APPDATA%` roams with a domain profile, which a kept profile wants; a cache does not |
| Rules | gfx v0.10.1's, unchanged: absolute only, no fallback, the wide API and UTF-8 on Windows, on POSIX each existing part refused when all users can write it or another user (not root) owns it, nothing on the web, no folder made | A program cache that someone else can write can be given an entry with a correct hash. A rig profile names devices and latencies; a planted one would bind a role to the wrong device or state a false latency |
| `subdir` | NULL or "" for the ysp folder; names separated by `/`, converted to `\` on Windows; an empty name, `.`, `..`, a backslash, a colon or a control character refused | The result must stay under `ysp`; `c:x` and `\\` name other roots on Windows |
| Version | 0.7.0 | A new public function: a minor version under 1.0 |

Checked on 2026-10-08:

| Check | Result |
|---|---|
| `tests/adapt/rt_test_userdir.h` (in `rt_test.c`, also under `YRT_TEST_FIT_ONLY`) | Both kinds on Windows (MSVC, MinGW gcc 16.1: a non-ASCII user name as UTF-8, a trailing slash, UNC, relative, missing, the exact fit and one byte short, NULL and empty subdir), on WSL2 gcc 11.4 (C99, and C11 with ASan and UBSan: XDG absolute and relative, `HOME/.cache` and `HOME/.config`, `/tmp` refused as a base and as `HOME`, relative and missing `HOME`) and under emcc in Docker (both kinds refused); the argument and subdir rules everywhere |
| `ygfx_default_cache_dir()`'s test | Unchanged, passes on Windows, WSL2 and emcc |
| Mutants (`tests/mutate/rt.toml`, `dir-01` to `dir-10`) | 9 of 9 Windows mutants caught by the Windows runner. `dir-10` (a folder all users can write accepted) is POSIX code: the runner reports it as survived, as its entry expects; built by hand on WSL2 against the mutated header, the test failed its three `/tmp` checks |

Not checked: macOS (CI compiles and runs the test there; its two bases are
in the test's macOS branch).

## Signed time, unit macros and open/close (v0.8.0)

v0.8.0 applies the repository conventions (README.md, Conventions) to this
header. Three decisions:

- **`yrt_now_ns()` is `int64_t`.** Every other time in ysp was already
  signed: flip onsets, audio onsets, timeline times, deadlines. The tree
  had 123 casts of `yrt_now_ns()` to `int64_t`, and without one,
  `onset - yrt_now_ns()` wraps when the onset is past. A signed count of
  nanoseconds from the OS's zero (boot, on every platform here) lasts 292
  years. The implementation keeps its unsigned arithmetic, where a deadline
  before the clock's zero saturates, and converts at the public calls. A
  negative deadline or spin window counts as 0.
- **The unit macros live here.** `YRT_S()`, `YRT_MS()`, `YRT_US()` and
  `YRT_NS()` replace `YTL_S()`, `YAU_S()` and the like. They round to the
  nearest nanosecond, so whole numbers are exact (to 2^53 ns). The headers
  that do not include this one define the same macros under the same
  `#ifndef YRT_S` guard, so one spelling works with or without ysp/rt.h.
- **Requests to the OS return a code.** `yrt_thread_pin()`,
  `yrt_thread_set_timer_slack()`, `yrt_process_lock_memory()` and
  `yrt_timer_resolution_begin()` returned false both for "this platform
  has no such call" and for "the OS refused". They return
  `YRT_ERR_UNSUPPORTED` and `YRT_ERR_REFUSED` now, so a log can tell the
  two apart.

The worker and the pump open and close (`yrt_worker_open()`,
`yrt_pump_close()`): they make a thread from a desc, which is what open
means in every header. `yrt_fit_open()` checks its desc and leaves a
message, and `yrt_ring_close()` exists so that every open has a close.

Checked on 2026-10-10 (Windows 11, MSVC 19.44 /W4 /WX and MinGW-w64 gcc
16.1 -Werror): `tests/adapt/rt_test.c` and `tests/compile/rt.c` (C11,
C++17, `YRT_NO_THREADS`) pass. The compile check now names the clock, the
waits and the fit calls by type and checks the macros' values in a static
initializer. Mutants `conv-01` (an open that takes a zero tick) and
`conv-02` (a close that keeps the map) are caught.

## Virtual clock (v0.9.0)

The examples' `--sim` runs compared their records with a script on the
real clock. On a loaded CI runner the simulated display dropped whole
frames, a check failed, and the fix each time was a wider check (seven
examples by 2026-10-13; [screen.md](screen.md#sim-on-a-virtual-clock-v080)
lists them). v0.9.0 gives the process a virtual clock, so a `--sim` run is
a function of its seed. Decisions:

- **One clock for the process, switched at run time.** The headers read
  the clock through `yrt_now_ns()` and wait through `yrt_sleep_until()`.
  A flag in those calls is the whole seam: ysp/screen.h's 60 clock reads,
  ysp/timeline.h, ysp/response.h and the examples need no change. A
  compile-time macro per header (the test seams `YSCR__NOW` and
  `YAU__NOW`) cannot reach the examples, and it would not put two headers
  on the same virtual clock.
- **One driver thread moves it.** The thread that called
  `yrt_virtual_begin()` moves the clock when it waits, to the wait's
  deadline. A clock that any waiting thread could move would make the
  time depend on which thread the OS ran first. Other threads wait in
  real time until the driver gets there.
- **Virtual timers, not lockstep threads.** What must keep its own time
  (the null audio device's blocks, the simulated audio loop of the
  capture examples) runs as a timer on the driver, inside its waits, in
  time order with ties in slot order (a timer takes the lowest free
  slot). Lockstep with a real thread per device would also be
  deterministic, but each thread would need a handshake per tick and a
  rule for two threads due at once.
  A timer table of 16 entries, scanned per event, needs neither.
- **Real time where real time is the point.** Instrumentation zones,
  `yrt_correlate()` (a bracket around another clock's read) and the
  timeout of `yrt_pump_wait()` read the real counter. So
  `screen_flipstats --sim` still reports the header's CPU cost per frame.
- **Costs read the real clock.** Between waits the virtual clock stands
  still, so a cost read with `yrt_now_ns()` is 0, and one across a wait
  is the jump. `yrt_real_ns()` is the real counter on either clock. A
  time that feeds a plan or a record reads `yrt_now_ns()`; a cost reads
  `yrt_real_ns()`. ysp/screen.h's phases, ysp/gfx.h's program and glyph
  build times, ysp/video.h's decode and upload times and the examples'
  cost lines do. So `gfx_load`, `gfx_filtered_noise`, `gfx_text`,
  `gfx_layout` and `gfx_gallery` run on the virtual clock too and still
  print real costs (`gfx_text --sim`: 171 ms for 1728 CJK glyphs).
  `gfx_capture` stays on the real clock: its writer thread is a real
  thread, and on the virtual clock the frame loop outran it (6 of 21
  chosen frames read, 15 dropped, against 21 of 21 on the real clock), so
  the counts its check compares would depend on the host's load.

Two failure modes remain and are stated in the header: a driver that
polls for a real thread's progress with sleeps sees its timeout pass at
once, and a real thread's own progress is not paced by the clock. Both
came up. ysp/video.h's decode thread fell behind a frame loop that never
waits, so on the virtual clock a movie decodes inline (video.h v0.4.0).
miniaudio's null device is a real thread, so ysp/audio.h v0.6.0 has a
null device of its own on a virtual timer.

### Cost on the real clock

`yrt_now_ns()` and the three waits test one flag first. Measured on
2026-10-14 on the Windows 11 test laptop (Iris Xe, 16 logical CPUs, AC
power, under the shared guard with no build running), base v0.8.0 against
v0.9.0, interleaved:

| Measure | v0.8.0 | v0.9.0 |
|---|---|---|
| `yrt_now_ns()`, ns a call: median of 7 runs of 20 M calls, thread pinned to CPU 2, MinGW-w64 gcc 16.1 -O2; 6 rounds | 23.5 to 26.1 | 24.3 to 26.3 |
| `yrt_sleep_until()` lateness, 2000 waits of 1.0 to 2.8 ms, default spin, not pinned; 3 rounds: median / p99 | 0 / 2.4 to 172 us | 0 / 0.2 to 10.9 us |
| `screen_flipstats --sim --frames 1200` (real clock), header cost in `yscr_begin()`, p50 / p99, us, MSVC 19.44 Release; 3 rounds | 6.7 to 7.2 / 14.4 to 15.4 | 6.5 to 6.8 / 13.3 to 14.8 |
| The same, header cost in `yscr_flip()` | 2.0 to 2.5 / 4.3 to 4.5 | 1.8 to 2.0 / 4.3 to 4.5 |

The difference is inside the run-to-run spread on every line. Unpinned,
the clock bench's medians moved by 6 ns between runs of the same binary
(the scheduler moved it between core types), so only the pinned rounds
count. The flag is one load of a word that only `yrt_virtual_begin()` and
`yrt_virtual_end()` write.

### Tests and mutants

`tests/adapt/rt_test_virtual.h` (run by `rt_test.c`, the fit-only
mutation build too): the clock starts at `YRT_VIRTUAL_T0_NS` and stands
still between waits; the driver's waits jump it and return 0 lateness,
and a past deadline returns its lateness; timers in time order with ties
in slot order, a periodic timer, a timer that cancels another, a timer
that waits (the wait moves the clock and runs no timer), a full table;
ring stamps; a worker job that runs only once the driver passes its
deadline, with a virtual `at_ns`; a wait on the worker thread that blocks
until the driver reaches it; `yrt_virtual_end()`. The compile check names
the six calls by type and runs a timer. `yrt_real_ns()` moves 2 ms in 2 ms
of real time while the virtual clock stands still, and a 10 s virtual wait
takes under 1 s of it. Mutants vc-01 to vc-13 are caught (13 of 13).

## Windows 11 power throttling

Windows 11 can throttle a process when all its windows are minimized,
covered or otherwise invisible. Two forms matter to a rig:

- It stops honoring `timeBeginPeriod()`. The documented opt-out is
  `SetProcessInformation(ProcessPowerThrottling)` with
  `PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION`.
- It applies EcoQoS: efficiency cores at a low clock. The opt-out is
  `PROCESS_POWER_THROTTLING_EXECUTION_SPEED` for the process, or
  `THREAD_POWER_THROTTLING_EXECUTION_SPEED` for one thread.

Before v0.5.0, `ysp/rt.h` called none of these. We measured before we
changed anything.

### Method

The machine is a 13th Gen Intel Core i7-1360P laptop: 4 P-cores with 2
threads each and 8 E-cores, 16 logical CPUs. It runs Windows 11 25H2,
build 26200, with the Balanced power plan, on AC power at 99% charge. We
could not unplug it, so there is no battery result.

The probe is a program in the scratchpad, not in the repository. A console
program owns no window, so the probe creates one: a 240 x 180 window in a
corner of the screen that never takes focus. It is built as a GUI-subsystem
program. The probe tests three window states:

- Visible.
- Minimized.
- Covered by a black, topmost window from a second copy of the probe.

The probe touches no other window. After it sets the state, it waits 12 s.
A 2 s wait was too short: no state change took effect in the first
full matrix. Then it times the following:

- 150 waits of 2 ms and 150 of 1 ms on its main thread, on three paths:
  - `std`: a waitable timer without the high-resolution flag.
  - `tbp`: `timeBeginPeriod(1)` and the same timer.
  - `hires`: `yrt_sleep_until(t, 0)`.
- A deadline worker every 2 ms, with no spin and with the default spin. Each
  job does a 40,000-step computation.
- A pump with 300 messages, at normal and below-normal priority. Each
  message does a 400,000-step computation.

For each sample, the probe records whether the thread ran on an efficiency
core: `GetCurrentProcessorNumberEx()` against the `EfficiencyClass` that
`GetSystemCpuSetInformation()` gives. The opt-outs, each applied before the
measurement:

- `none`: no opt-out.
- `itr`: the process timer opt-out.
- `p0`: the process EcoQoS opt-out.
- `t0`: the thread EcoQoS opt-out on every measured thread.
- `itr_p0` and `itr_t0`: the timer opt-out with one of the EcoQoS
  opt-outs.
- `eco_t1`: EcoQoS forced on, as a positive control.

`SetProcessInformation` and `SetThreadInformation` returned success for
every mask. `GetProcessInformation` read back what was set.

### Timer resolution

Lateness of 1 ms waits in ms, p50, minimized, three runs:

| Path | No opt-out | Timer opt-out |
|---|---|---|
| `timeBeginPeriod(1)` | 14.0, 14.1, 14.0 | 0.56, 0.65, 0.54 |
| High-resolution timer (ysp_rt) | 0.51, 0.44, 0.41 | 0.41 (1 run) |
| Timer without the flag, no `timeBeginPeriod` | 14.1 | 14.2 |

Visible, `timeBeginPeriod(1)` gave 0.5 to 0.7 ms with or without the
opt-out. Minimized, it loses the raised resolution, and the opt-out keeps
it. The high-resolution timer is not affected in any state. `yrt_sleep_until()`,
the worker and the pump use the high-resolution timer, so their lateness
did not change: worker p50 was 0.27 to 0.45 ms in every state and
configuration, and 0 to 1 us with the default spin. Covering the window
throttled the timer in only 1 of 19 runs. We do not know what makes
Windows treat our cover as occlusion, so the covered result is not a
claim.

On battery (Balanced plan, no power-mode overlay, 98% charge), one run
each:

| Path | Visible | Minimized, no opt-out | Minimized, timer opt-out |
|---|---|---|---|
| `timeBeginPeriod(1)` | 0.57 | 14.1 | 0.74 |
| High-resolution timer (ysp_rt) | 0.51 | 0.62 (p99 3.4) | |

The pattern is the same as on AC. The high-resolution timer's p99 while
minimized was 3.4 ms on battery, against 0.8 to 2.2 ms on AC.

The change: `yrt_timer_resolution_begin()` also sets the timer opt-out.
That function exists for `Sleep()` and for timers without the flag, which
are the paths the throttling breaks.

### EcoQoS

Minimized, 99 to 100% of the samples on every thread ran on E-cores with no
opt-out. A single run per configuration gave a noisy result, so we used a
paired test. One thread does the 400,000-step computation 20 times with the
EcoQoS state at default, 20 times opted out per thread, and 20 times opted
out per process. It repeats this cycle 30 times, so drift on the machine
affects all three states equally. Computation time in ms, p50 / p99, with
the E-core share:

| State, run | Default | Thread opt-out | Process opt-out |
|---|---|---|---|
| Visible, 1 | 1.45 / 3.07, 40% E | 1.34 / 2.37, 4% E | 1.27 / 2.78, 6% E |
| Visible, 2 | 1.46 / 4.53, 43% E | 1.36 / 2.74, 7% E | 1.32 / 3.87, 6% E |
| Minimized, 1 | 1.60 / 3.30, 100% E | 1.22 / 2.67, 6% E | 1.21 / 2.48, 4% E |
| Minimized, 2 | 1.42 / 2.41, 99% E | 1.12 / 1.99, 1% E | 1.15 / 2.27, 2% E |

On battery, minimized, one run of 30 cycles: 2.22 / 2.58 ms at 100% E
by default, and 1.22 / 2.40 ms at 1% E with the thread opt-out.

Opted out, the same computation is 21 to 24% faster minimized and 7 to 12%
faster visible, and the p99 is lower. On battery the gain was 45%. The
thread and the process opt-out give the same result. Wake lateness did not change in any configuration.
Forcing EcoQoS on, the positive control, put 100% of samples on E-cores and
made the worker's job 2 to 4 times as slow as the default.

The change: `yrt_thread_elevate()` and the pump thread opt their own
thread out. The pump is included because below-normal priority decides
who runs first, not which core runs it, and the pump's work is
computation. We chose the thread opt-out over the process opt-out because
the two measured the same, and the thread opt-out leaves alone the threads
that `ysp/rt.h` does not own, such as an audio library's callback thread.
A worker with `no_elevate` is left to the OS, as its name says.

`yrt_describe()` reports both settings, as `timer_throttle=off` and
`ecoqos=off`. The values are `on` when Windows may still apply the
throttling, and `n/a` on other platforms.

## P-cores: a preference for elevated threads, not for the pump

A hybrid CPU has performance cores and efficiency cores. Windows 11 steers
threads with Intel Thread Director; Windows 10 does not have it. A program
can find the P-cores by the highest `EfficiencyClass` that
`GetSystemCpuSetInformation()` reports, and steer a thread to them with
`SetThreadSelectedCpuSets()`. We measured whether that helps when the
EcoQoS opt-out is already in place.

### Method

The probe from the throttling section was used again, on the same i7-1360P
with Windows 11 build 26200 on AC power. Its CPU sets show the 8 P-core
logical processors (cores 0, 2, 4 and 6, two threads each) as
EfficiencyClass 1 and the 8 E-cores as class 0.

A second, console process ran 8 spinning threads at normal priority with
no affinity, so that the P-cores were busy. The probe ran three threads:

- A frame thread, elevated with `yrt_thread_elevate()`: a 2 ms wait with
  no spin, then a 100,000-step computation.
- A deadline worker every 2 ms, with no spin, doing a 40,000-step
  computation.
- A pump at normal priority, one message every 5 ms, with a 400,000-step
  computation.

Each thread switched its setting every 200 ms, all at the same moment, for
30 s. So the three settings shared the same load and the same machine
state. The settings:

- **(a)** The EcoQoS opt-out alone, which is what v0.5.0 did first.
- **(b)** (a) plus the thread's selected CPU sets set to every P-core.
- **(c)** (b), but the P-cores split by physical core: the frame thread
  got cores 0 and 2, the worker cores 4 and 6, so the two elevated
  threads never share a core. The pump got all P-cores, as in (b).

Each state ran twice with the load, visible and minimized, and once
without the load.

### Results with the load

Wake lateness and computation time, in us:

| Run | Thread | Setting | Late p50 / p99 / max | Work p50 / p99 | E-core |
|---|---|---|---|---|---|
| Visible 1 | frame | (a) | 361 / 1123 / 117,967 | 626 / 735 | 83% |
| | | (b) | 346 / 1029 / 3755 | 556 / 790 | 0% |
| | | (c) | 351 / 1052 / 24,569 | 556 / 843 | 0% |
| | worker | (a) | 346 / 1019 / 119,956 | 241 / 313 | 83% |
| | | (b) | 345 / 1111 / 121,117 | 220 / 318 | 0% |
| | | (c) | 336 / 1128 / 6335 | 219 / 276 | 0% |
| | pump | (a) | 47 / 18,812 / 87,868 | 2440 / 3642 | 73% |
| | | (b) | 48 / 5116 / 60,280 | 2255 / 3971 | 0% |
| Visible 2 | frame | (a) | 356 / 1091 / 3294 | 602 / 725 | 82% |
| | | (b) | 344 / 932 / 8208 | 543 / 729 | 0% |
| | | (c) | 347 / 983 / 96,090 | 545 / 775 | 0% |
| | worker | (a) | 344 / 1003 / 16,678 | 241 / 312 | 82% |
| | | (b) | 332 / 928 / 2324 | 218 / 311 | 0% |
| | | (c) | 331 / 833 / 3314 | 218 / 271 | 0% |
| | pump | (a) | 46 / 209 / 5264 | 2428 / 3451 | 67% |
| | | (b) | 41 / 179 / 7742 | 2099 / 2837 | 0% |
| Minimized 1 | frame | (a) | 308 / 583 / 1156 | 286 / 628 | 81% |
| | | (b) | 289 / 567 / 1202 | 216 / 575 | 0% |
| | | (c) | 290 / 556 / 995 | 215 / 576 | 0% |
| | worker | (a) | 290 / 580 / 1050 | 115 / 241 | 90% |
| | | (b) | 270 / 557 / 772 | 86 / 226 | 0% |
| | | (c) | 272 / 551 / 823 | 86 / 223 | 0% |
| | pump | (a) | 24 / 98 / 6816 | 1094 / 2449 | 61% |
| | | (b) | 18 / 177 / 4526 | 875 / 2271 | 0% |
| Minimized 2 | frame | (a) | 286 / 742 / 9333 | 286 / 700 | 55% |
| | | (b) | 294 / 669 / 9304 | 231 / 662 | 0% |
| | | (c) | 297 / 754 / 62,213 | 231 / 692 | 0% |
| | worker | (a) | 327 / 750 / 82,826 | 115 / 294 | 77% |
| | | (b) | 309 / 727 / 16,194 | 92 / 272 | 0% |
| | | (c) | 311 / 696 / 66,268 | 93 / 253 | 0% |
| | pump | (a) | 24 / 141 / 9154 | 1030 / 2770 | 41% |
| | | (b) | 19 / 14,529 / 59,868 | 925 / 6674 | 0% |

The pump rows for (c) are the same setting as (b) and measured the same
(14,401 us p99 in minimized run 2), so they are left out.

Without the load, every thread already ran on P-cores 92 to 100% of the
time with (a). Steering changed the computation by 2 to 12% and the
lateness by no consistent amount.

### On battery

One run per state of 15 s, not 30, for the elevated threads only. The
load and the method are the same as above. Late p50 / p99 and work p50, in
us:

| State | Thread | (a) | (b) |
|---|---|---|---|
| Visible | frame | 281 / 587; 354; 65% E | 263 / 563; 232; 0% E |
| | worker | 300 / 587; 142; 59% E | 283 / 550; 93; 0% E |
| Minimized | frame | 308 / 573; 354; 71% E | 288 / 592; 232; 0% E |
| | worker | 290 / 565; 142; 74% E | 263 / 560; 93; 0% E |

On battery, steering made the computation 35% faster, against 9 to 25% on
AC. The wake p99 was lower in 3 of 4 pairs, and 3% higher in the fourth.

### What the numbers say

- With the P-cores busy, the EcoQoS opt-out does not keep an elevated
  thread on a P-core. Windows woke it on an idle E-core 55 to 90% of the
  time. Steering kept it on P-cores every time.
- For the two elevated threads, (b) made the fixed computation 9 to 25%
  faster at p50. The wake p99 was lower in 7 of 8 pairs, by 3 to 15%, and
  9% higher in the eighth (worker, visible run 1). The maximum is set by
  rare outliers in every setting and shows no pattern.
- (c) measured the same as (b). Splitting the cores gave no benefit, so it
  is not in the header.
- For the pump, (b) made the computation 8 to 20% faster. But in minimized
  run 2 its wake p99 rose from 0.14 ms to 14.5 ms. A normal-priority
  thread confined to busy P-cores waits for a quantum behind the load,
  and an idle E-core would have run it at once. The pump is not steered.

### The change

`yrt_thread_elevate()` prefers the P-cores for the calling thread on a
hybrid CPU, so the frame thread that calls it and every elevated worker
get the preference. A uniform CPU gets nothing. The preference is soft:
Windows still runs the thread elsewhere when no selected core is free.
`yrt_thread_pin()` clears it, because a pin is the explicit choice. The
test pins an elevated worker to an E-core and checks that it runs there.
`yrt_describe()` shows the preference as `cores=P` or `cores=any`.

This change was made on the Windows 11 numbers above. Windows 10 lacks
Thread Director, so the gain could be larger there, but nothing here
measured Windows 10, and that is not a claim.

### Core type of a pinned CPU

On a hybrid CPU the CPU number does not tell you the kind of core. CPU 8 is
an E-core on this machine. `yrt_cpu_core_type(cpu)` reads the kind from
the EfficiencyClass. `yrt_thread_pin()` records the pin, and the report
line shows it as `pin=8/E`.

Linux on Intel hybrid lists the kinds in `/sys/devices/cpu_core/cpus` and
`/sys/devices/cpu_atom/cpus`. WSL2 hides them: it shows 8 identical vCPUs
(4 cores with 2 threads each, every `cpu_capacity` 1024), and neither file
exists. An untested parser would be a claim we cannot check, so on Linux
`yrt_cpu_core_type()` returns `YRT_CORE_UNKNOWN`, and the manual tells
you to read those files yourself.

## Measured costs

Per push, ns, mean and 99th percentile over blocks of 64:

| Build | Stamps set | t_ns and tid 0 |
|---|---|---|
| MSVC | 15.9, 17.2 | 42.9, 46.9 |
| MinGW gcc | 14.4, 17.2 | 36.6, 53.1 |
| gcc WSL2 | 14 to 37, 45 | 76, 137 |
| emcc, node | 30.5, 92 | 214, 976 |

Under node one clock read costs 190 ns, which is most of the stamped push.

B2, per push with N producers back-to-back and one drainer, mean and p99 ns:

| Build | 2 | 4 | 8 |
|---|---|---|---|
| MSVC | 156, 213 | 239, 559 | 531, 1669 |
| MinGW gcc | 143, 177 | 287, 731 | 552, 1864 |
| gcc WSL2 | 351, 1641 | 762, 4208 | 1469, 7757 |
| emcc, node | 307, 972 | | |

In WSL2, 8 producers and a drainer are 9 busy threads on 8 vCPUs, so the
8-producer row includes preemption.

B3, one producer at the top of the ladder pushing every 1 ms while 7 others
push, per push including two clock reads, ns:

| Build | Policy | Control (no push) p99.9 | Wait-free p99.9 | Mutex p99.9 |
|---|---|---|---|---|
| MSVC, 3 runs | TIME_CRITICAL | 500 to 900 | 1500 to 2200 | 227,600 to 553,400 |
| MinGW gcc | TIME_CRITICAL | 1000 | 2000 | 115,900 |
| gcc WSL2 as root, 2 runs | DEADLINE | 680 to 1009 | 1166 to 27,961 | 10,958,556 to 11,350,073 |

The 28 us in WSL2 is one of two runs; the control run beside it had a 159 us
maximum, which is the virtual machine, not the ring.

Drain, ns per record: 1.4 to 5.7 (MSVC), 2.4 to 3.5 (MinGW), 1.6 to 2.0
(WSL2), 3.0 to 5.1 (node).

A zone pair, mean ns:

| Mode | MSVC | MinGW gcc | gcc WSL2 | emcc, node |
|---|---|---|---|---|
| Default (nothing) | empty loop | | empty loop | |
| Trace ring, attached | 66 to 72 | 95 | 52 to 55 | 471 |
| Trace ring, not attached | under 1 over empty | under 1 | under 1 | 2 |
| Tracy, on demand, not connected | 10 | | 2 | |
| Tracy, buffering | 90 | | 65 | |

One MSVC run measured 149 ns for the attached zone pair right after the
oversubscription section; three runs on their own measured 66 to 72.

B6, `yrt_correlate()` with the default tries:

| Clock | Tightest width | Call |
|---|---|---|
| yrt_now_ns itself (Windows) | 100 ns | 1.3 to 2.1 us |
| QueryInterruptTimePrecise | 100 ns | 1.2 to 1.9 us |
| GetTickCount64, edge mode | 100 to 200 ns | 62 ms |
| CLOCK_MONOTONIC_RAW (WSL2) | 41 ns | 1.1 us |
| yrt_now_ns itself (node) | 256 ns | 8.7 us |

## Against the bars

The bars were set before the measurements, against a 16.7 ms frame and a
full rig's load of about 100 records a frame.

| Bar | Result |
|---|---|
| Push mean, one thread, at most 100 ns | Met everywhere. Stamped pushes under node (214 ns) miss it, because of the clock read. |
| Push p99, one thread, at most 200 ns | Met natively. Missed under node with stamping (976 ns). |
| Push mean, 8 producers, at most 1 us | Met on Windows (0.53, 0.55 us). Missed in WSL2 (1.47 us), where the threads outnumber the vCPUs. |
| Elevated producer p99.9, at most 10 us and below the mutex | Met on Windows (1.5 to 2.2 us against 116 to 553 us). WSL2 missed it in one of two runs (28 us), with the VM's own stalls visible in the control. |
| Drain, at most 30 ns a record | Met everywhere (6 ns or less). |
| Zone, trace ring attached, at most 150 ns | Met natively (52 to 95 ns; one run 149 ns). Missed under node (471 ns). |
| Default macros cost nothing | Met: the same as an empty loop, and the compile check proves no argument is evaluated. |
| Correlation width at most 1 us, call at most 20 us | Met everywhere. |
