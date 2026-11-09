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
`ysig_node_of()` sets the unused inputs to -1. A parameter's index has a
name: `YSIG_<OP>_<ROW>` is the row of that name in the op's table
([Ops](#ops)).

In C, `YSIG_NODE()` writes one node in one expression; the examples use
it. This tutorial assigns the fields one by one, which also works in C++:

```c
n[HOLD] = YSIG_NODE(YSIG_OP_DETECT, HAND, -1, .name = "hold", .p = {
    [YSIG_DETECT_LEVEL] = 12, [YSIG_DETECT_HOLD] = 0.5 });
```

```c
#define YSP_SIG_IMPLEMENTATION
#include "ysp/sig.h"

enum { MOUSE, HAND, CURSOR, BIND, N };
ysig_node n[N];
n[MOUSE] = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
n[MOUSE].lanes = 2;
n[MOUSE].p[YSIG_SOURCE_KIND] = YIN_KIND_MOUSE;
n[MOUSE].p[YSIG_SOURCE_CONTROL] = YIN_AXIS_DELTA;   /* raw counts */
n[HAND] = ysig_node_of(YSIG_OP_INTEGRATE, MOUSE, -1);
n[HAND].p[YSIG_INTEGRATE_GAIN_X] = n[HAND].p[YSIG_INTEGRATE_GAIN_Y] = 0.25;   /* px per count */
n[HAND].p[YSIG_INTEGRATE_ORIGIN_X] = 512;          /* the start */
n[HAND].p[YSIG_INTEGRATE_ORIGIN_Y] = 384;
n[CURSOR] = ysig_node_of(YSIG_OP_AFFINE, HAND, -1);
n[CURSOR].p[YSIG_AFFINE_PRE_X] = n[CURSOR].p[YSIG_AFFINE_POST_X] = 512;   /* about the start */
n[CURSOR].p[YSIG_AFFINE_PRE_Y] = n[CURSOR].p[YSIG_AFFINE_POST_Y] = 384;
n[CURSOR].p[YSIG_AFFINE_ANGLE] = -30;   /* y is down: -30 turns counterclockwise on screen */
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
ysig_frame(&g, &(ysig_frame_in){ f.index, f.onset, 0, NULL, 0, yrt_now_ns(), true });
dot.x = channels[0];
dot.y = channels[1];
```

Each raw mouse report moves the hand by its counts, and the cursor
follows at once. `ysig_frame()` writes the bound channels. Feed the
frame's input before `ysig_frame()`: a sink reads held values
([examples_findings.md](examples_findings.md), D10). The last two fields
are the poll's time and `has_poll`: the graph knows input up to the poll,
and no further. An event stamped after the poll is counted (`late_poll`,
`late_poll_max_ns`) and moves nothing, so one bad stamp cannot make the
views run to the planned onset. `ysig_frame()` refuses only bad
arguments and, on every call, a frame without a poll (`has_poll` false,
as in a zeroed `ysig_frame_in`) in a graph with a view (velocity, speed,
delay, lowpass, rate integrate): a configuration error, never the data.
A poll of 0 with `has_poll` is a valid time. On the virtual clock of `--sim`,
where the script feeds input up to the onset, the poll is the onset.

`examples/response/trial_visuomotor.c` is this graph in a whole task, with
the hold, the reach and the movement onset.

## How-to guides

### Feed a movement onset to the response collector

Use an `emit` node. It makes a derived `yin_event` of its input at each
update: kind `YIN_KIND_USER +` its `kind` parameter, device the emit
node's index, control its `control` parameter, the lanes in x, y, value and aux. Give the collector a DISTANCE
choice of that kind and device:

```c
n[EMIT] = ysig_node_of(YSIG_OP_EMIT, HAND, -1);
n[EMIT].p[YSIG_EMIT_KIND] = 7;           /* YIN_KIND_USER + 7 */
moved.kind = YIN_KIND_USER + 7;
moved.device = EMIT;
moved.control = YIN_AXIS_POSITION;
moved.cross = YRSP_CROSS_DISTANCE;
/* after each ysig_feed(): */
n = ysig_derived(&g, der, 8);
for (i = 0; i < n; i++) yrsp_feed(&rsp, &der[i]);
```

The event's `t` is the time of the report that arrived and made it (also
when the other hand's held part is newer in time, and also when it is
earlier than the emit's last event: as `ysp/input.h` says of events,
times need not rise in arrival order, and the collector reads `t`), its
`unc_us` that report's, its `stamp` the worst tier of its inputs, and its `code` that
report's source node (low 16 bits) and the oldest input's (high 16
bits). A cursor from two hands says which hand's report crossed
(`trial_bimanual.c`'s `move_hand` column). The source table's bounds do
not travel: the collector sees kind USER, so its result's `stamp_lo_ns`
and `stamp_hi_ns` are 0. Take them from the hand's own source entry.

### End a step on a hold

Use a `detect` with `hold`. "In the start circle for 0.5 s":

```c
n[HOLD] = ysig_node_of(YSIG_OP_DETECT, HAND, -1);
n[HOLD].p[YSIG_DETECT_TEST] = YSIG_TEST_LE;   /* the norm of hand - center <= level */
n[HOLD].p[YSIG_DETECT_LANE] = -1;             /* the norm, not one lane */
n[HOLD].p[YSIG_DETECT_LEVEL] = 12;            /* px */
n[HOLD].p[YSIG_DETECT_HOLD] = 0.5;            /* s */
n[HOLD].p[YSIG_DETECT_A_X] = 512;             /* the center */
n[HOLD].p[YSIG_DETECT_A_Y] = 384;
```

The hit's `t` is the hold's start plus 0.5 s. The next sample finds it
(before it is tested, so a hand that leaves after the hold ended counts as
movement), or a frame that knows the time is past it (its poll, at most
its onset), so a still hand that sends nothing completes it. A frame's
hold clock is that known time while the test is true, on any input. Read hits with `ysig_detected()`. With
`p[YSIG_DETECT_START] = 1` (start `arm`) the detect waits for
`ysig_arm(&g, node, t)`. A crossing detect (no hold) armed while its test
is true is held until the test fails (the collector's CROSSINGS rule). A
hold armed while its test is true counts from the arm with
`p[YSIG_DETECT_ENTRY] = 0` (entry `arm`, the default: a hand already on
the start at a trial's first frame), or waits for the test to fail and
come true again with `p[YSIG_DETECT_ENTRY] = 1` (entry `fresh`).

### Freeze a cursor at the sample that crossed

Use a `latch` on a `detect` with `out` set to `hit`. The latch's output is
the cursor until the detect fires, and the cursor of that sample after,
until the next reset:

```c
n[REACH] = ysig_node_of(YSIG_OP_DETECT, HAND, -1);
n[REACH].p[YSIG_DETECT_TEST] = YSIG_TEST_GE;
n[REACH].p[YSIG_DETECT_LANE] = -1;              /* the norm of hand - center */
n[REACH].p[YSIG_DETECT_LEVEL] = 300;            /* px */
n[REACH].p[YSIG_DETECT_A_X] = sx;
n[REACH].p[YSIG_DETECT_A_Y] = sy;
n[REACH].p[YSIG_DETECT_OUT] = YSIG_OUT_HIT;
n[SHOWN] = ysig_node_of(YSIG_OP_LATCH, CUR, REACH);
n[BIND] = ysig_node_of(YSIG_OP_BIND, SHOWN, -1);
```

The latch runs in the sample's pass, after the detect, so the frozen point
is the value and time of the sample that crossed, as the hit records them.
When the hit is found by a frame (a still hand completes a hold), the
output rises in that frame's pass; a latch on the same input as the detect
then holds the hit's value and time (for a hold, the value at its start and
start + hold). A latch whose trigger rises in a frame's pass and is not
such a detect holds the input's held value with that value's `t`. Trigger
on a detect's `hit` output, not its test, when the detect needs arming: the
test can rise while the detect is off.

### Choose a stimulus property with a step

Give the property a `mux` on a selector channel (D17). A timeline step
sets the channel; the frame after it shows the other input:

```c
n[SEL] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
n[SEL].p[YSIG_CHANNEL_CHANNEL] = CH_SELECT;
n[SHOW] = ysig_node_of(YSIG_OP_MUX, CURSOR, HIDDEN);
n[SHOW].in[2] = SEL;
```

The output is the chosen input, with its `t`, device, tier and source.
A sample of the input not chosen does not change the output's value or
time, but it runs the mux's users again: a `record` or `emit` after the
mux gets a repeated row or event with the chosen input's value and time.
Nonzero chooses `a`; zero and NaN choose `b`.

### Keep the hand across trials

Call `ysig_reset_trial()` at a trial's start. It resets the experiment's
nodes only: an adapter's nodes keep their state, so a relative hand does
not jump (D19). `ysig_reset()` still resets everything. With no adapters
the two calls are the same. To place a relative role, for example to bring
a cursor back to the start without moving the hand:

```c
ysig_recenter(&g, 0, start_x, start_y, t_trial);     /* role 0, in the role's units */
ysig_reset_trial(&g, t_trial);
```

`ysig_recenter()` goes back through the adapter's `scale` and `offset`
nodes to its integrator and restarts the count grid at that point, so the
role reads exactly x, y and later counts land on the same grid. The
placement gets a new ring entry at the call's time: a past read before it
still gives the old place. A rate integrator takes the point at that time,
with no jump by the time since its last sample. After the integrator,
velocity, speed, lowpass and delay restart, because the jump would spike
them; the stateless nodes take the new value; a rate integrator, peak,
latch, silent, detect and custom op keep their state and output, and a
custom op gets no `reset()`. A detect sees the new value at its next
sample, so a hold that must start from the placed point needs the
recenter before the arm or the `ysig_reset_trial()`. The call returns
false and sets `ysig_error()` for an absolute adapter, an adapter with no
integrator, another op between the integrator and the role, a point not
finite after that chain, or a point outside the integrator's bounds.

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
is still placed by its time. So is a stateless node with a generator
input (a hand plus a sum-of-sines perturbation): the generator keeps a
ring of its frames. `ysig_value_at()` is safe inside `desc.on_row`. `trial_bimanual.c` reads L, R, u and the
cursor this way.

### Run one task graph with a mouse or a tablet

Declare a role in the experiment's part and give an adapter per rig:

```c
ysig_role hand = { "hand", YSIG_TYPE_POSITION2D, YSIG_UNITS_DISPLAY_PX, true };
n[HAND] = ysig_node_of(YSIG_OP_ROLE, -1, -1);   /* p[YSIG_ROLE_ROLE] = 0: roles[0] */

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
n[SINES].p[YSIG_SINES_N] = 7;      /* components per lane */
n[SINES].data = comp;              /* 2 x 7 x (amplitude, Hz, phase rad) */
n[SINES].n_data = 42;
n[AMP] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
n[AMP].p[YSIG_CHANNEL_CHANNEL] = AMP_CHANNEL;
n[TARGET] = ysig_node_of(YSIG_OP_SCALE, SINES, AMP);
...
ytl_base_time(&tl, TRIAL, f.onset, &bt);
ysig_frame(&g, &(ysig_frame_in){ f.index, f.onset, bt, ytl_values(&tl), n_channels, yrt_now_ns(), true });
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

The digest takes every record row, hit and derived event, every bound
channel value and every frame's index and onset, a 64-bit word at a time,
each step through splitmix64's finalizer: a sign flip in two values, or
-0 for +0, changes it. Words, not bytes, so the digest does not depend on
byte order. Replay is bit for bit
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
`sig_test.c` has a mass on a damped spring. A custom op gets the frame's
onset, not the poll, so it cannot implement a view (a value at a frame
from input known up to the poll): a limit of this version.

## Reference

### Ops

p[] indices are the rows of `ysig_op_params(op, &n)`. Each has a name,
`YSIG_<OP>_<ROW>`: the op and the row's name in upper case, a dot as an
underscore (`YSIG_DETECT_LEVEL`, `YSIG_AFFINE_ANGLE`,
`YSIG_INTEGRATE_ORIGIN_X`). `speed` has `velocity`'s rows and `mean`
`sum`'s, under their own names too. Durations are seconds, as in a
definition. A zero field means the row's `def`.

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
| `detect` | 1, 1 (the test, or the hit with `out` hit; holds while disarmed or after a one-hit hit) | test (ge, le, in_rect, out_rect), lane (-1 norm), level, hysteresis, hold s, at (sample, frame), start (reset, arm), repeat (once, every), a.x a.y b.x b.y, entry (arm, fresh), out (test, hit) | The only sink with an output: another op may read it (a mux selector, an all input, a latch's trigger). With `out` hit, 1 from a hit until the detect is ready again (one hit: until the next arm or reset) |
| `record` | 1, none | per (sample, frame) | `desc.on_row`. A row has its value's true `t`: a sample stream may be out of time order when two sources feed the node, as `ysp/input.h` says of events |
| `emit` | 1, none | kind (USER + 0..7), control | `ysig_derived()`. On a frame-updated node (a mux of a channel) it emits at every frame |
| `mux` | 3 (a, b, selector), n | none | a when lane 0 of the selector is nonzero, else b; a and b the same lanes; the output's time, device, tier and source are the chosen input's. NaN counts as zero |
| `latch` | 2 (value, trigger), n | none | Passes the value until lane 0 of the trigger goes from zero to nonzero, then holds until a reset. Hit value of a detect on the same input, else the held input at the pass of the rise. Its ring holds sample-time values: a generator part of its input is not read back by `ysig_value_at()` |
| `distance` | 1 (2 or 3 lanes) or 2, 1 | point.x, point.y, point.z | The Euclidean distance to the point, or to a second input of the same lanes |
| `peak` | 1 or 2 (gate), 1 | lane (-1 norm) | The largest value since the reset or since the gate last rose; holds while the gate is zero; `t` is the peak sample's, its ring's time the later of that and the ring's last, so a late larger peak is kept. A gate that rises before any sample starts from the first sample. Needs a source upstream |
| `all` | 2 to 4, 1 | none | 1 when lane 0 of every input is nonzero |
| `silent` | 1, 1 | after s | A view: 1 once no sample has come for `after`, judged at the poll, before any sample from the reset; the same at a past read of its value, at most at the last poll. Needs a source upstream |

### Time

- A value's `t`, device, `unc_us` and source node: in a sample's pass,
  from the input that changed, along the path (the arriving report's,
  also when another hand's held part is newer; after a delay, the delayed
  sample's, so `record(delay(x))` and `record(offset(delay(x)))` agree);
  in a frame's pass and in `ysig_value_at()`, from the newest input.
  Emit and record carry that true `t`. A detect's hold clock never goes
  back: a hold starts and ends on the later of the new `t` and the
  detect's last, and the clamp is counted (`time_clamped`); a crossing's
  hit has the time of the value that met the test. A value also carries `t_oldest` (the
  oldest input's time) and its source, and the worst tier of its inputs
  (SIM, 1, 2, 3, UNKNOWN from best to worst). Before its first sample a source has no value: tier UNKNOWN,
  an empty ring.
- Order is kept per source and device: a sample older than the last of
  its device is dropped and counted (`dropped_order`), so two mice on one
  any-device source do not drop each other. Equal times pass. A raw
  mouse's counts are never dropped: a sum does not depend on order. A
  source keeps the last time of 8 devices; a ninth takes the slot of the
  least recently seen one (`order_evicted`), and that device's next late
  sample passes. Give a source a device (`YSIG_SOURCE_DEVICE`) when more than 8 share
  a kind and control.
- `ysig_frame()` runs, in node order, the generators at the frame's base
  time; velocity, speed, delay, lowpass and rate integrators as views,
  their state unchanged; every node after those; then bind, frame
  records and detects. A view uses input up to the earlier of the onset
  and `poll_ns`, as given (an event stamped after it moves nothing): a
  velocity or speed
  reads 0 only when its input was silent for the lag up to the poll. A
  sample-rate detect on a view tests at the view's time, so its hold
  completes at a poll, up to a frame after the onset that would end a
  hold on samples.
- History rings hold values at sample times; a frame's view never
  enters them. A latch's capture and a peak's restart in a frame's pass
  do. A `mux`, `all` or `distance` after a channel or another frame-updated
  input reads its inputs' rings, also a detect's: its ring holds its
  output at each value it tested. `silent` reads its input's ring and
  judges at the time asked, at most the last poll.
- Integrate, lowpass, velocity, speed, delay, peak and silent need a
  source upstream: they keep state per sample, and `ysig_check()` refuses
  them on a generator or a channel alone.
- Times saturate at the ends of int64 instead of overflowing.
- A stamp later than the poll is believed only up to the poll. At a frame
  with a poll, every time kept from a stamp and later than the poll comes
  down to it, counted in `late_poll`: a source's order slots, history
  rings and the rings of velocity, speed and delay, a detect's hold clock
  and hold start, the previous time of a low-pass and of a rate
  integrator, a `silent`'s last time. One far-future stamp then stops
  dropping that source's samples one frame later, later samples enter the
  rings, a hold can complete, and a silent reads 1 after the real silence.
  What that sample's own pass did stands: a rate integrator's step over
  its dt stays in the sum, and a hold it ended has its hit. A frame with no
  poll has no such recovery: a graph that never polls stays behind that
  stamp.

### Trials and adapters

- `ysig_reset_trial()` skips the nodes of every adapter, so their order
  slots, integrators, filters and rings keep their state; the hit and
  derived-event queues empty, as at `ysig_reset()`. Each node that reads
  an adapter starts from its current value. A graph with no adapters
  resets every node.
- `ysig_recenter()`: see [Keep the hand across trials](#keep-the-hand-across-trials).
  The point is in the role's units, at the call's time. Both calls are
  journaled.

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
- **A detect is a sink with an output.** Its output was the test's state,
  for a color; it was refused as an input, so `all` and a `mux` selector
  (D17) had nothing to read. It is now an input of another op. A sink
  otherwise has no lanes, and `ysig_check()` refuses it as an input. The
  `out` row makes the output the hit, not the test: a freeze at a control's
  hit (D18) needs a rise at the hit, also when a frame completes a hold,
  and before the arm the test must not latch.
- **A mux passes its chosen input, metadata included.** A selection is not
  a mix: `t`, device, tier and source are the chosen input's, not the
  newest of all three, or a mux on a channel selector would carry the
  frame's planned onset. `ysig_value_at()` returns 1 when the selector and
  the chosen input have the time, whatever the other input's ring holds.
- **A value at a past time for `silent`.** A silent's own ring would hold
  only sample-time zeros, so a past read comes from when its input last
  arrived, the input's ring, judged at the time asked and never past the
  last poll, as a view. Before the input's first sample in the ring the
  count runs from the reset.
- **Recenter: an inverted chain, not a transform.** The adapter's last
  node is what the task reads, but the count grid is the integrator's.
  The spec's mouse adapter ends in a `scale` (cm from counts), so the
  point goes back through the scale and the offset. An affine or a filter
  in between is refused: inverting one is a rig calibration, not a
  step's job.
- **Parameters by name, not constructors.** A graph set `p[8] = sx`; a
  reader needed the table to see that it is a detect's center
  ([examples_findings.md](examples_findings.md), F25). Each index now has
  the name of its row, `YSIG_DETECT_A_X`, made from the table by one rule
  that the test checks for every row, so a name cannot drift from the
  table. `p[]` and `ysig_set()` by number still work: the fuzz target and
  the journal use numbers. Constructors for the common shapes (a hold in
  a circle) were not added: each has five or six numbers in a fixed
  order, the same problem as `p[]`, and they would cover few of the
  graphs. The names do not carry units (`YSIG_DETECT_HOLD`, not
  `_HOLD_S`): every duration in `p[]` is seconds, and the row says so.
  `trial_visuomotor` and `trial_bimanual` set their graphs by name (57
  lines, no line more or fewer); `examples/sig/bench.c` keeps its 31
  numbers.

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

After the second review's fixes (2026-11-07), one run pinned with
`taskset` to a performance core (CPU 2) and one to an efficiency core (CPU
14); the rows before them are the first review's, three runs on the P-core
and one on the E-core. "Per
sample" is a frame's feeds and the drains of its derived events and
hits, over its 533 samples; "a frame" is 533 samples, two
`ysig_value_at()` reads and one `ysig_frame()`.

| Core | Per sample: mean, median, p99 | `ysig_frame()`: median, p99, max | Two `ysig_value_at()`: median, p99 | A frame, median |
|---|---|---|---|---|
| P-core, first review | 128.2 to 128.4 ns, 127.4 to 127.6 ns, 158.5 to 163.6 ns | 274 to 277 ns, 499 to 846 ns, 1.77 to 2.45 us | 105 ns, 163 to 181 ns | 68.3 to 68.4 us |
| E-core, first review | 249.4 ns, 248.1 ns, 293.1 ns | 538 ns, 1.40 us, 3.15 us | 152 ns, 255 ns | 132.9 us |
| P-core, second review | 142.5 ns, 141.4 ns, 175.0 ns | 276 ns, 529 ns, 1.76 us | 107 ns, 193 ns | 75.7 us |
| E-core, second review | 276.4 ns, 275.6 ns, 309.4 ns | 551 ns, 1.55 us, 3.16 us | 152 ns, 256 ns | 147.6 us |
| P-core, third review | 143.0 ns, 141.9 ns, 176.2 ns | 281 ns, 508 ns, 2.32 us | 105 ns, 203 ns | 76.0 us |
| P-core, final review | 142.5 ns, 141.6 ns, 177.0 ns | 284 ns, 593 ns, 2.43 us | 105 ns, 176 ns | 75.9 us |
| P-core, v0.1.0 again (2026-11-09) | 142.3 ns, 141.3 ns, 176.7 ns | 282 ns, 493 ns, 2.41 us | 105 ns, 189 ns | 75.7 us |
| P-core, v0.2.0 (2026-11-09) | 146.5 ns, 145.6 ns, 180.2 ns | 301 ns, 491 ns, 2.71 us | 108 ns, 168 ns | 78.0 us |

v0.2.0 costs about 4 ns a sample (3%) and 15 ns a frame over v0.1.0 on the
same graph, in two runs each (145.4 and 145.6 ns against 141.3 and 141.4 ns).
The bench graph has no new op, so the difference is in the existing path. Its
likely part is a detect now writing its value's time, tier and sources on its
output, because its output can feed other ops; this was not profiled.

The second review's cost is the digest's finalizer: built with the plain
xor-multiply step in its place, the same bench gave 130.6 and 130.8 ns a
sample (median, two runs) against 141.0 and 143.6 ns with the
finalizer, about 11 ns a sample for three digested values a sample (a
record row and two derived events). The rest (the changed-input
propagation, least-recently-used order slots) is about 3 ns.

Before the fixes the same bench gave 129.4 to 129.8 ns a sample (median)
on the P-core and 261.8 to 268.1 ns on the E-core. The fixes put derived
events, bound channels and frames into the digest: hashed a byte at a
time, that took the P-core to 156 ns a sample (18% of the time in the
digest, by `perf`); hashed a 64-bit word at a time, 127 ns. Unpinned, the
scheduler puts the run on either kind of core. The worst median, 133 us a
frame (148 us after the second review), is under 0.9% of a 16.7 ms frame
at 32,000 samples a second; the budget
(a trial's frame work in one frame) holds with room.

The bench's arena is 1,644,640 bytes (third review). Most of it is history: five rings
of 4096 values of 64 bytes (the four hands and one cursor), 1,310,720
bytes. The three velocity and speed rings of 256 values are 49,152
bytes; the 32 nodes, their values and the schedules are the rest. Not measured: the
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

- `tests/adapt/sig_test.c` (ctest `test_sig`): v0.2.0 adds `test_mux`,
  `test_latch`, `test_distance`, `test_peak`, `test_all`, `test_silent`,
  `test_detect_out`, `test_reset_trial`, `test_recenter`,
  `test_ops_refusals`, `test_ops_views` and `test_ops_replay`, each beyond a
  closed form: reports out of time order (two sources, two devices), reads
  at a planned onset ahead of the poll with no recent sample and a still
  input, `ysig_value_at()` at a past time, inside `on_row` and with a
  frame-updated input (`mux` with a `channel` selector), before any sample,
  after a reset and across `ysig_reset_trial()`, times at the ends of int64,
  a far-future stamp on another source, replay bit for bit, the digest
  moving with the output, and each refusal's message. Each op against a closed
  form, the schedules, the tier and `t_oldest` rule, the out-of-order
  drop, detectors, derived events, records, reset, replay bit for bit,
  values at a time, roles and adapters, a custom op, `ysig_set()`, a
  golden digest, 40 refusals of bad graphs, and one test per item of the
  two reviews (`test_order`, `test_views`, `test_review`, `test_still`,
  `test_review2`, `test_review3`), and `test_param_names`: every
  `YSIG_<OP>_<ROW>` against the row of that name, and one name for each
  row of every op. It passes under ASan and UBSan (`setarch -R` on the
  Linux desktop) and as C++.
- `tests/compile/sig.c` and `sig.cpp`: C99, C11, C++17 and C++20 with
  warnings as errors (C++20: GCC's `-Wdeprecated-enum-float-conversion`
  no longer fires).
- `tests/fuzz/sig_fuzz.c`: arbitrary node arrays (and roles and adapters)
  into `ysig_check()` and `ysig_open()`, then every call on a graph that
  opened, and a replay of its journal to the same digest. It also feeds
times at the ends of int64, twelve devices on a source (more than its
eight order slots) and frames with polls before, at and after the
onset. No libFuzzer here (clang is not installed on the Linux desktop):
the self-driven mode (`-DYSP_FUZZ_SELF`) ran under ASan and UBSan with
GCC 12.2 and `setarch -R`. Before the review: 20,000,000 inputs (seed
777) in 9 min 28 s, no finding. After it: 20,000,000 inputs (seed 778)
in 9 min 27 s; 14,565,670 refused by `ysig_check()`, 5,433,396 graphs
opened and ran (35.0 million events taken, 14.7 million rows, 0.97
million hits); then 2,000,000 more (seed 779) after the word-wise
digest. No sanitizer report, no graph that the check passed and the
open refused, no replay that differed. One finding during the fixes: the
default poll first came from every event fed, also those no source
took; a journal holds only the taken ones, so a replay saw another poll
and another view (fixed; mutant 68). After the second review: 20,000,000
inputs (seed 780) in 9 min 6 s, no finding. The third review's rules
(frames never refused for data) added two checks to the target: every
frame with a poll (or without one in a graph with no view) returns 0, also
after INT64_MAX and far-future stamps, and every bound channel holds its
bind's input after it. After the third review: 20,000,000 inputs (seed
781) in 9 min 33 s; 14,562,777 refused by `ysig_check()`, 5,436,289
graphs opened and ran (35.0 million events taken, 14.7 million rows,
0.97 million hits), no finding. For v0.2.0 the target builds the new ops and
calls `ysig_reset_trial()` and `ysig_recenter()` (accepted or refused, so
a replay holds across both); 20,000,000 inputs (seed 782), no finding:
14,474,726 refused by `ysig_check()`, 5,524,346 graphs opened and ran (36.4
million events taken, 14.8 million rows, 0.98 million hits).
- `tests/mutate/sig.toml`: 157 mutants and a control; 157 of 157 killed
  (v0.2.0: mutants 95 to 158 for the new ops and calls).
  Mutants 89 to 93 move a name off its row (a swap, a gap, the shared
  tables of `speed` and `mean`).
  Mutants 70 and 71 remove the saturation of time arithmetic; the test
  built with `-fsanitize=undefined` (the list's `sig_ubsan` test) catches
  them.

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
- The layout's size_t overflow check (`ysig_check()` refuses such a
  graph, so `ysig_arena_size()` is never 0 for a graph the check takes)
  is not tested: it needs a 32-bit build (a 64-bit size_t holds the
  largest graph).
- A frame-rate form of integrate, lowpass, velocity, speed, delay, peak
  and silent (on a generator or a channel alone).
- A second latch inside one trial (D18: "until a task needs it"): a
  `latch` holds until the next reset.
- `ysig_recenter()` through an `affine` or a filter in the adapter: it
  refuses them (design notes).
- Not run: MSVC, macOS, a window, a real device, a photodiode.
