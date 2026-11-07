# ysp/sig.h

`ysp/sig.h` is the signal path: a small graph, fixed at open, from input
sources through per-sample transforms and maps to stimulus channels,
detectors, derived events and records. The design is
[signal_spec.md](signal_spec.md); this page is the manual. The header
comment is the full reference.

The header needs `ysp/input.h` and nothing else. It allocates nothing,
calls no OS function, has no thread and does no file I/O. The caller gives
it one arena.

## Tutorial: a cursor from a raw mouse

This tutorial builds the cursor of a visuomotor rotation task: the hand is
the mouse's counts integrated at 0.25 px per count, and the cursor is the
hand turned 30 deg about the start. At the end you read the cursor from a
channel, as `ygfx_apply()` does.

### 1. Write the graph

A graph is an array of nodes. Each input comes before its user.
`ysig_node_of()` sets the unused inputs to -1.

```c
#define YSP_SIG_IMPLEMENTATION
#include "ysp/sig.h"

enum { MOUSE, HAND, CURSOR, BIND, N };
ysig_node n[N];
n[MOUSE] = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
n[MOUSE].lanes = 2;
n[MOUSE].p[0] = YIN_KIND_MOUSE;      /* kind */
n[MOUSE].p[1] = YIN_AXIS_DELTA;      /* control: raw counts */
n[HAND] = ysig_node_of(YSIG_OP_INTEGRATE, MOUSE, -1);
n[HAND].p[0] = n[HAND].p[1] = 0.25;  /* px per count */
n[HAND].p[4] = 512;                  /* origin: the start */
n[HAND].p[5] = 384;
n[CURSOR] = ysig_node_of(YSIG_OP_AFFINE, HAND, -1);
n[CURSOR].p[4] = n[CURSOR].p[6] = 512;   /* about the start */
n[CURSOR].p[5] = n[CURSOR].p[7] = 384;
n[CURSOR].p[8] = -30;                /* y is down: -30 turns counterclockwise on screen */
n[BIND] = ysig_node_of(YSIG_OP_BIND, CURSOR, -1);   /* channels 0 and 1 */
```

### 2. Open it

```c
static double arena[4096];
static float channels[2];
ysig_graph g;
ysig_desc d = { 0 };
d.nodes = n;
d.n_nodes = N;
d.channels = channels;
d.n_channels = 2;
d.arena = arena;
d.arena_size = ysig_arena_size(&d);          /* 0: a bad graph */
if (d.arena_size > sizeof arena || !ysig_open(&g, &d)) {
    fprintf(stderr, "%s\n", ysig_error(&g));
    return 1;
}
ysig_reset(&g, t_start);
```

`ysig_open()` refuses a graph with a cycle, a lane count that an op does
not take, an unknown op or a parameter outside its op's table, and says
why.

### 3. Feed it and read it at each frame

```c
/* every frame, after yscr_begin() */
while (yscr_poll(&scr, &ev, NULL))
    if (yscr_event_input(&scr, &ev, &in)) ysig_feed(&g, &in);
ysig_frame(&g, &(ysig_frame_in){ f.index, f.onset, 0, NULL, 0, yrt_now_ns() });
dot.x = channels[0];
dot.y = channels[1];
```

Each raw mouse report moves the hand by its counts, and the cursor
follows at once. `ysig_frame()` writes the bound channels. Feed the
frame's input before `ysig_frame()`: a sink reads held values
([examples_findings.md](examples_findings.md), D10). The last field is
the poll's time: the graph knows input up to there, and no further.

`examples/response/trial_visuomotor.c` is this graph in a whole task, with
the hold, the reach and the movement onset.

## How-to guides

### Feed a movement onset to the response collector

Use an `emit` node. It makes a derived `yin_event` of its input at each
update: kind `YIN_KIND_USER + p0`, device the emit node's index, control
`p1`, the lanes in x, y, value and aux. Give the collector a DISTANCE
choice of that kind and device:

```c
n[EMIT] = ysig_node_of(YSIG_OP_EMIT, HAND, -1);
n[EMIT].p[0] = 7;                        /* YIN_KIND_USER + 7 */
moved.kind = YIN_KIND_USER + 7;
moved.device = EMIT;
moved.control = YIN_AXIS_POSITION;
moved.cross = YRSP_CROSS_DISTANCE;
/* after each ysig_feed(): */
n = ysig_derived(&g, der, 8);
for (i = 0; i < n; i++) yrsp_feed(&rsp, &der[i]);
```

The event's `t` is the time of the report that arrived and made it (also
when the other hand's held part is newer in time), its `unc_us` that
report's, its `stamp` the worst tier of its inputs, and its `code` that
report's source node (low 16 bits) and the oldest input's (high 16
bits). A cursor from two hands says which hand's report crossed
(`trial_bimanual.c`'s `move_hand` column). The source table's bounds do
not travel: the collector sees kind USER, so its result's `stamp_lo_ns`
and `stamp_hi_ns` are 0. Take them from the hand's own source entry.

### End a step on a hold

Use a `detect` with `hold`. "In the start circle for 0.5 s":

```c
n[HOLD] = ysig_node_of(YSIG_OP_DETECT, HAND, -1);
n[HOLD].p[0] = YSIG_TEST_LE;   /* the norm of hand - center <= level */
n[HOLD].p[1] = -1;             /* the norm, not one lane */
n[HOLD].p[2] = 12;             /* px */
n[HOLD].p[4] = 0.5;            /* s */
n[HOLD].p[8] = 512;            /* the center */
n[HOLD].p[9] = 384;
```

The hit's `t` is the hold's start plus 0.5 s. The next sample finds it
(before it is tested, so a hand that leaves after the hold ended counts as
movement), or a frame whose onset is at or after it, so a still hand that
sends nothing completes it. Read hits with `ysig_detected()`. With
`p[6] = 1` the detect waits for `ysig_arm(&g, node, t)`. A crossing
detect (no hold) armed while its test is true is held until the test
fails (the collector's CROSSINGS rule). A hold armed while its test is
true counts from the arm with `p[12] = 0` (entry `arm`, the default: a
hand already on the start at a trial's first frame), or waits for the
test to fail and come true again with `p[12] = 1` (entry `fresh`).

### Read a value at a flip record's onset

The flip record of a frame arrives a frame later. Give the node a
history, and read it at the record's onset:

```c
n[LEFT].history = 4096;            /* past values kept */
...
ysig_value v;
if (ysig_value_at(&g, LEFT, rec.onset, &v) < 1) ring_miss++;
```

A stateless node after two sources (a `join` or `mean` of two hands, an
`affine` of that) keeps no ring of its own: its value at t is computed
from its inputs' values at t, so a report that arrives late from one hand
is still placed by its time. `trial_bimanual.c` reads L, R, u and the
cursor this way.

### Run one task graph with a mouse or a tablet

Declare a role in the experiment's part and give an adapter per rig:

```c
ysig_role hand = { "hand", YSIG_TYPE_POSITION2D, YSIG_UNITS_DISPLAY_PX, true };
n[HAND] = ysig_node_of(YSIG_OP_ROLE, -1, -1);   /* p[0] = 0: roles[0] */

/* mouse rig */
ysig_adapter ad = { "hand", "raw mouse", YSIG_TYPE_POSITION2D_RELATIVE, YSIG_UNITS_DISPLAY_PX, mouse_nodes, 2 };
/* tablet rig */
ysig_adapter ad = { "hand", "tablet pen", YSIG_TYPE_POSITION2D, YSIG_UNITS_DISPLAY_PX, pen_nodes, 1 };
```

`ysig_open()` refuses an adapter of other units or another type. A
relative device (a mouse) under a `position2d` role needs
`allow_relative`, because the task must design the recentering. The
experiment's nodes keep their indices; `ysig_adapter_node()` gives an
adapter's. `trial_visuomotor --sim --pen` runs the task through a tablet
adapter: its data rows and trajectory equal the raw mouse run's.

### Draw a tracking target from a sum of sines

Use `sines` at the frame's base time, times a timeline channel for the
ramp:

```c
n[SINES] = ysig_node_of(YSIG_OP_SINES, -1, -1);
n[SINES].lanes = 2;
n[SINES].p[0] = 7;                 /* components per lane */
n[SINES].data = comp;              /* 2 x 7 x (amplitude, Hz, phase rad) */
n[SINES].n_data = 42;
n[AMP] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
n[AMP].p[0] = AMP_CHANNEL;
n[TARGET] = ysig_node_of(YSIG_OP_SCALE, SINES, AMP);
...
ytl_base_time(&tl, TRIAL, f.onset, &bt);
ysig_frame(&g, &(ysig_frame_in){ f.index, f.onset, bt, ytl_values(&tl), n_channels, yrt_now_ns() });
```

A late frame shows the value for its own base time (F22). Set the phases
of a new block with `ysig_set_data()`.

### Check a run by replay

Set `desc.journal`. It gets every call that changes the graph: reset, set,
set_data, arm, disarm, each event a source takes or drops, each frame with
its channel values (copy them: they are valid during the call). Open a
second graph from the same desc and give it the journal:

```c
if (ysig_replay(&g2, journal, n_entries) != n_entries) ...
ysig_get_stats(&g, &a);
ysig_get_stats(&g2, &b);
/* a.digest == b.digest: every row and hit, bit for bit */
```

The digest is FNV-1a over every record row, hit and derived event, every
bound channel value and every frame's index and onset. Replay is bit for bit
on one build. Across compilers and platforms, `cos`, `exp` and fused
multiply-adds can differ in the last bit ([rdk.md](rdk.md), "Why the state
is integers"); a graph of counts and exact gains (0.25 px per count) gives
the same digest everywhere (`sig_test`'s golden digest).

### Add a custom op

Fill a `ysig_op_vtable` and pass it in `desc.custom`; op
`YSIG_OP_CUSTOM + i` is `custom[i]`. The graph gives the op `state_size`
bytes of its arena, checks the lanes and the parameters by the op's
table, and fills the output's time, tier and sources before `sample()`.
An op without inputs is a generator: its `frame()` runs at each frame.
`sig_test.c` has a mass on a damped spring.

## Reference

### Ops

p[] indices are the rows of `ysig_op_params(op, &n)`. Durations are
seconds, as in a definition. A zero field means the row's `def`.

| Op | Inputs, lanes | p[] | Notes |
|---|---|---|---|
| `source` | none, 1 to 4 | kind, control, device (0 any), fields (x 1, y 2, value 4, aux 8), tier (for stamp 0), type (0 sample) | Without fields: x, y, value, aux by lanes; 1 lane is value |
| `role` | none (its adapter), the type's | role index | |
| `integrate` | 1, n | gain[4] (0 = 1), origin[4], low[4], high[4] | A raw mouse's DELTA source (kind MOUSE): int64 count sum per lane, held inside the bounds. Else rate: gain x held value x dt. Needs a source upstream |
| `lowpass` | 1, n | cutoff Hz, order (1 or 2), rate Hz (for the Nyquist check) | First-order sections on each sample's dt |
| `velocity`, `speed` | 1, n (speed: 1) | lag s, capacity (256) | Difference over at least the lag; at a frame, 0 once the input was silent for the lag up to the poll |
| `delay` | 1, n | delay s, capacity (1024) | The input as it was |
| `deadzone` | 1, n | radius | Radial; past it the norm shrinks by the radius |
| `affine` | 1 or 2 (angle), 2 | m.xx m.xy m.yx m.yy (all 0 = identity), pre.x/y, post.x/y, angle deg | out = Rot(angle + input) M (in - pre) + post |
| `scale` | 1 or 2 (factor), n | k[4] (0 = 1) | |
| `offset` | 1, n | d[4] | |
| `clamp` | 1, n | low[4], high[4] (both 0 = none) | |
| `select` | 1, lanes | lane per output lane | |
| `join` | 1 to 4, sum of lanes (at most 4) | none | |
| `sum`, `mean` | 1 to 4 of n, n | weights (0 = 1) | mean divides by the weights' sum |
| `diff` | 2 of n, n | none | |
| `sines` | none, 1 or 2 | n (components), ramp s; data | At the frame's base time |
| `noise` | none, 1 to 4 | seed, sd, cutoff Hz | Set again at each reset |
| `channel` | none, 1 | timeline channel | From the frame's values |
| `bind` | 1, none | first channel | Writes float at each frame |
| `detect` | 1, 1 (the test) | test (ge, le, in_rect, out_rect), lane (-1 norm), level, hysteresis, hold s, at (sample, frame), start (reset, arm), repeat (once, every), a.x a.y b.x b.y, entry (arm, fresh) | |
| `record` | 1, none | per (sample, frame) | `desc.on_row` |
| `emit` | 1, none | kind (USER + 0..7), control | `ysig_derived()` |

### Time

- A value made at a sample carries that sample's `t`, device, `unc_us`
  and source node, `t_oldest` (the oldest input's time) and its source,
  and the worst tier of its inputs (SIM, 1, 2, 3, UNKNOWN from best to
  worst). Before its first sample a source has no value: tier UNKNOWN,
  an empty ring.
- Order is kept per source and device: a sample older than the last of
  its device is dropped and counted (`dropped_order`), so two mice on one
  any-device source do not drop each other. Equal times pass. A raw
  mouse's counts are never dropped: a sum does not depend on order.
- `ysig_frame()` runs, in node order, the generators at the frame's base
  time; velocity, speed, delay, lowpass and rate integrators as views,
  their state unchanged; every node after those; then bind, frame
  records and detects. A view uses input up to `poll_ns` (0: the newest
  event a source took) and never past it to the planned onset: a
  velocity or speed reads 0 only when its input was silent for the lag
  up to the poll.
- History rings hold values at sample times; a frame's view never
  enters them.
- Integrate, lowpass, velocity, speed and delay need a source upstream:
  they keep state per sample, and `ysig_check()` refuses them on a
  generator or a channel alone.
- Times saturate at the ends of int64 instead of overflowing.

### Limits

512 nodes with the adapters', 4 lanes, 16 parameters, 4096 data values a
node, 64 hits and 64 derived events between drains (the rest counted),
16 adapters, 256 roles, a ring of at most 2^20 values. The arena must be
8-byte aligned. One thread.

## Design notes

### Why the header has these parts

The spec (section 5) lists the parts. The ports of `trial_visuomotor` and
`trial_bimanual` decided the details. Each was ported until its CI data
files came out byte-identical.

- **One output per node, held.** A node reads its inputs' held values. A
  node that two sources reach runs at each sample of either, with the
  other's held value. This makes F20 visible: the value's `t_oldest` is
  the older part's time.
- **Two schedules, compiled at open.** A sample runs its source's list,
  nothing else. The cost of a sample is fixed at open.
- **Values at a past time.** `trial_bimanual` reads each tracking frame's
  hands at the flip record's onset, a frame after the frame. A frame sink
  cannot do this alone: the record comes later. Rings per node, opt-in,
  answer it. A stateless node after two sources is computed from its
  inputs' rings, so arrival order across sources does not matter (the
  test feeds a late report and checks it).
- **Views up to the poll.** A mouse sends nothing while it is still. A
  velocity or a filter that changed only at samples would hold the last
  moving value forever. These nodes also give a view at a frame, and keep
  their state, so the per-sample values do not depend on when frames ran.
  The first build ran the view to the frame's planned onset, a frame past
  the input: a hand moving at 1000 px/s read as slower, and a "speed
  under 50" detect hit 29 times in 30 frames while the hand moved (the
  review's P8). The view now runs to the poll's time, and a velocity
  reads 0 only after the input was silent for the lag up to the poll.
- **The sign of zero.** An affine skips exact zero terms. A mirror
  (`[[0, 1], [1, 0]]`) then gives `y` exactly, -0 included, as the old
  `trial_bimanual` did; without the skip, `0 + -0` is `+0` and its track
  file printed `0.0000` for `-0.0000` (mutants 07 and 08).
- **The target's color in `trial_bimanual`.** It reads the frame's cursor
  `c`, as before the port, not a detect on the graph's held cursor: after
  a block change the held cursor keeps the old block's P until the next
  report, while `c` is computed at the onset with the new P.
- **Derived events, no new flag.** The emit node's events use a USER kind
  and the emit node's index as the device. The collector needs nothing
  more. A consumer that logs every event cannot tell a derived event from
  a USER device's. `ysp/input.h` has no flag for it: see "Not done".

### Byte-identical ports

Both examples run on the header. Their CI `--sim` runs give the same data
files as before, byte for byte (the Linux desktop, GCC 12.2, Release):

| Run | Files | Result |
|---|---|---|
| `trial_visuomotor --sim` | data, trajectory | identical |
| `trial_visuomotor --sim --sdl-mouse --gradual --endpoint-only` | data, trajectory | identical |
| `trial_bimanual --sim`, 4 mappings and perturbations | data, reaches, track, freq (16 files) | identical |

At the default gain (0.25 px per count) and the nominal 32 px per cm,
every count lands on a value a float holds exactly, so the old float path
and the new double path agree. At other gains they differ in the last
printed digit: the old programs rounded each hand to float (a
`yrsp_cursor`), the graph keeps double. For example `trial_visuomotor
--sim --gain 0.3` changed 2 of 161 data rows (`cursor_end_deg`
3.8179 to 3.8178) and 348 of 42,959 trajectory rows (a cursor's last
digit); `trial_bimanual --sim --gain 0.3` changed values in the fourth
decimal (a hand of 0.0094 cm, which was 0.0094 + 6e-5 off the count
grid). The graph's values are the ones on the count grid. Both programs'
own checks pass at both gains.

`trial_bimanual --sim` (14 virtual minutes, about 1.7 million hand
reports) took 424 to 426 ms of wall time on the Linux desktop's P-core,
336 to 337 ms before the port (three runs each): about 50 ns a report
for the graph over the hand-written arithmetic.

The ports changed no column. Each prints its graph on stdout, not in the
data file, so the files keep their bytes.

### Cost

On the Linux desktop (i5-14500, Debian 12, liquorix 6.11, GCC 12.2 at
`-O3`, Release, nothing else running, 2026-11-07), `examples/sig/bench.c`
runs 60 s of input: four raw
mice at 8 kHz each into a 32-node graph (the visuomotor and bimanual
graphs side by side, widened: four integrators, four hands in cm with
rings of 4096, a split mapping and a mean, two cursors, two speeds, a
second-order low-pass, a velocity, two emits, a sample record, 7 + 7
sines times a channel, a tracking error with a frame-rate detect, a
distance detect, a bind).

Three runs each, pinned with `taskset` to a performance core (CPU 2) and
to an efficiency core (CPU 14). "Per sample" is a frame's feeds and the
drains of its derived events and hits, over its 533 samples; "a frame" is
533 samples, two `ysig_value_at()` reads and one `ysig_frame()`.

| Core | Per sample: mean, median, p99 | `ysig_frame()`: median, p99, max | Two `ysig_value_at()`: median, p99 | A frame, median |
|---|---|---|---|---|
| P-core | 130.3 to 130.7 ns, 129.4 to 129.8 ns, 159.4 to 163.2 ns | 292 to 297 ns, 515 to 867 ns, 1.96 to 2.39 us | 102 ns, 146 to 163 ns | 69.4 to 69.6 us |
| E-core | 262.5 to 268.8 ns, 261.8 to 268.1 ns, 289.1 to 296.9 ns | 582 to 599 ns, 1.44 to 1.61 us, 2.69 to 53.1 us | 150 to 155 ns, 235 to 264 ns | 140.3 to 143.7 us |

Unpinned, the scheduler put the run on either kind of core (one run at
130 ns a sample, two at 250 ns). The worst median, 144 us a frame, is
0.9% of a 16.7 ms frame at 32,000 samples a second; the budget (a
trial's frame work in one frame) holds with room. Not measured: the
Windows development machine, Tracy zones per op.

The first build scanned a velocity's ring linearly from the newest value:
at 16 kHz into one node and a 10 ms lag, 160 values a sample. `perf` put
73% of the time in `ysig__run`. The ring is in time order, so a bisection
finds the same value. On the P-core, three runs each of the same bench:
454.3 to 456.7 ns a sample (mean) with the scan, 130.3 to 130.5 ns with
the bisection. The ring now keeps a value
only when it is not older than its newest (two sources out of order), so
the bisection stays correct.

No allocation after open: the test renames `malloc`, `calloc`, `realloc`
and `free` before the implementation, so a call fails the link; the bench
counts this program's allocator calls during the 60 s run (glibc).

### Tests, fuzzing and mutations

- `tests/adapt/sig_test.c` (ctest `test_sig`): each op against a closed
  form, the schedules, the tier and `t_oldest` rule, the out-of-order
  drop, detectors, derived events, records, reset, replay bit for bit,
  values at a time, roles and adapters, a custom op, `ysig_set()`, a
  golden digest, and 25 refusals. It passes under ASan and UBSan
  (`setarch -R` on the Linux desktop) and as C++.
- `tests/compile/sig.c` and `sig.cpp`: C99, C11 and C++17 with warnings
  as errors.
- `tests/fuzz/sig_fuzz.c`: arbitrary node arrays (and roles and adapters)
  into `ysig_check()` and `ysig_open()`, then every call on a graph that
  opened, and a replay of its journal to the same digest. No libFuzzer
here (clang is not installed on the Linux desktop): the self-driven
mode (`-DYSP_FUZZ_SELF`) ran 20,000,000 inputs (seed 777: random
bytes, mutated seeds, and random graphs in the decoder's form) under
ASan and UBSan with GCC 12.2 and `setarch -R`, in 9 min 28 s.
14,537,934 were refused by `ysig_check()`; 5,461,054 graphs opened and
ran, with 50.7 million events taken, 24.0 million rows and 2.5 million
hits. No sanitizer report, no graph that the check passed and the open
refused, no replay that differed.
- `tests/mutate/sig.toml`: MUTANT_RESULT

## Not done

- **A derived-event flag.** `ysp/input.h` would need
  `#define YIN_DERIVED 0x20u /* made by a signal path from other events:
  device is the node, code its sources */` in its flags (0x20 is free),
  a row in THE EVENT, and a version bump. Then the emit node sets it, and
  a log or the collector's ALL mode can skip or keep derived events.
- The ops `rectify`, `envelope`, `curve` and `units`; the `pose` type;
  prediction; a stream-sample source (`ysig_stream_sample` is defined, not
  read); the JSON loader; the rig profile's adapter block; a Tracy zone per
  op.
- A dataset glue: a record row goes to `desc.on_row`, and the program
  writes it (to `yds_stream()` or its own file).
- Bindings: no Python or MEX binding yet.
- The layout's size_t overflow check is not tested: it needs a 32-bit
  build (a 64-bit size_t holds the largest graph).
- A frame-rate form of integrate, lowpass, velocity, speed and delay (on
  a generator or a channel alone).
- Not run: MSVC, macOS, a window, a real device, a photodiode.
