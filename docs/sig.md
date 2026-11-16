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
movement), or a frame whose onset is at or after it, so a still hand that
sends nothing completes it. On a frame-updated input (a view, a mux or a
latch of a channel) a frame's hold clock is the earlier of its poll and
its onset, while the test is true: a still input's value keeps its old
`t`, and the hold would never end. Read hits with `ysig_detected()`. With
`p[YSIG_DETECT_START] = 1` (start `arm`) the detect waits for
`ysig_arm(&g, node, t)`. A crossing detect (no hold) armed while its test
is true is held until the test fails (the collector's CROSSINGS rule). A
hold armed while its test is true counts from the arm with
`p[YSIG_DETECT_ENTRY] = 0` (entry `arm`, the default: a hand already on
the start at a trial's first frame), or waits for the test to fail and
come true again with `p[YSIG_DETECT_ENTRY] = 1` (entry `fresh`).
A crossing detect with `p[YSIG_DETECT_ENTRY] = YSIG_ENTRY_NOW` (2, from
v0.3.0) takes a test that is true at the arm as a hit at the arm's time,
with the input's held value: a cursor already in the target starts
tracking at once (experiment_format.md D35 (f), gap G16). At frame rate
the hit is at the next onset. With a hold, `now` counts from the arm, as
`arm` does.

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
ysig_recenter(&g, 0, start_x, start_y, poll_ns);     /* role 0, in the role's units */
ysig_reset_trial(&g, t_trial);
```

Give `ysig_recenter()` a time no later than the input already fed,
normally the poll's time. A planned onset is ahead of samples not yet fed:
a ring would skip them, and a past read would disagree with the live value.

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
| `detect` | 1, 1 (the test, or the hit with `out` hit; holds while disarmed or after a one-hit hit) | test (ge, le, in_rect, out_rect), lane (-1 norm), level, hysteresis, hold s, at (sample, frame), start (reset, arm), repeat (once, every), a.x a.y b.x b.y, entry (arm, fresh, now), out (test, hit) | The only sink with an output: another op may read it (a mux selector, an all input, a latch's trigger). With `out` hit, 1 from a hit until the detect is ready again (one hit: until the next arm or reset) |
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
- A poll a little behind the newest sample is normal and changes
  nothing; it is counted in `late_poll`. A stamp later than the poll by
  more than `YSIG_FUTURE_NS` (1 s) is a bad stamp: at a frame with a poll,
  an order slot's last time and a `silent`'s last time past that come down
  to the poll. Later samples then flow again one frame later, and a silent
  recovers.
- Known limit: that stamp's effect on rings, filters, velocity, delay, a
  rate integrator (it takes one step over the bad time, then stays frozen)
  and a hold's clock stays until the node's next reset (`ysig_reset_trial()` for
  the experiment's nodes, `ysig_reset()` for an adapter's). `late_poll` is
  the alarm. A frame with no poll has no recovery of the order slots
  either: the source drops its later samples until a reset.

### An arm in the past: the semantics

Status: the semantics of 0.6.0 as built in the header and the player
(0.10.0). An independent reviewer wrote an oracle from this section and
the graph's other rules alone (`tests/adapt/sig_arm_ref.c`, with
`sig_arm_spec_cases.c`; both in ctest): it runs the reference sequence
of the same calls on a graph with no window. The header gives that
sequence's result exactly within the bound of "Exactness" below, and
counts every arm outside it. The last subsection lists where 0.6.0
before it differed. Where the design note "An arm in the past (v0.6.0)"
below disagrees, this section is the rule.

#### Terms

- **Reads.** Node m reads node n when n is one of m's inputs (`in[0]` to
  `in[3]`, a mux's selector too). A role node reads the last node of its
  adapter.
- **Downstream set** `D(n)`: n, and every node that reads a node in
  `D(n)`.
- **Input call**: `ysig_feed()` of an event that a source takes and does
  not drop (sig.h SOURCES; a sample older than the last of its device is
  dropped), `ysig_frame()`, `ysig_settle()`. A feed that no source takes,
  or that every source taking it drops, changes no node: it is not an
  input call (its counts are kept, as in any order).
- **State call**: `ysig_set()`, `ysig_set_data()`, `ysig_disarm()`,
  `ysig_recenter()`, `ysig_arm()`.
- **Barrier**: `ysig_reset()`, `ysig_reset_trial()`, `ysig_flush()`
  (new, below; it resets nothing).
- **Read**: `ysig_value_of()`, `ysig_value_at()`, `ysig_detected()`,
  `ysig_derived()`, `ysig_get_stats()`. A read is not a call of the
  sequence. It sees the state after the calls made before it.
- **Input time** `N`: after a reset, the reset's time (`ysig_flush()`
  leaves `N`); after an input call, the larger of `N` and that call's
  time.

#### The time of each call

| Call | Time |
|---|---|
| `ysig_feed(e)` | `e.t` |
| `ysig_frame(f)` | `f.onset_ns` |
| `ysig_settle(t)` | `t` |
| `ysig_arm(d, t)` | `t` |
| `ysig_reset(t)`, `ysig_reset_trial(t)` | `t` |
| `ysig_flush()` | `N` at its call |
| A state call in a group (below) | the group's time |
| Any other state call (`ysig_recenter()` too) | `N` at its call |

A `ysig_recenter()` outside a group acts at its call: its `t` stamps
the new value and the integrator's grid. It does not run input again.

#### Groups

A **gap** is a run of state calls with no input call or barrier between
them. Each gap is cut after each `ysig_arm()`: a **group** is the state
calls from the gap's start, or from the call after the gap's previous
arm, up to and including an arm. The group's time `τ` is its arm's `t`.
State calls after a gap's last arm are in no group.

Reason: a caller makes the calls of one moment with no input between
them, and the arm states that moment's time. The player makes a take's
sets, disarms and recenters, then the take's arm, all at the take's
time (format 3.12).

A group is **past** when `τ < N` at its arm's call, **present** when
`τ = N`, and **ahead** when `τ > N`.

#### Footprint

The footprint `F(c)` of a call is the set of nodes it can change:

| Call | Footprint |
|---|---|
| Feed | the union of `D(s)` over the sources s that take the event and do not drop it |
| Frame, settle | every node |
| `ysig_arm(d)`, `ysig_disarm(d)` | `D(d)` |
| `ysig_set(n)`, `ysig_set_data(n)` | `D(n)` |
| `ysig_recenter(r)` | `D(i)`, i the integrator of role r's adapter |
| A group | the union of its calls' footprints |

Two calls with disjoint footprints commute: in either order every node
ends with the same value, ring and state. Only the order of rows, hits
and derived events of different nodes, and the digest, change.

#### The reference sequence R

R is built call by call, in call order:

1. An input call, a barrier, a state call in no group, and a present
   group are added at the end of R.
2. A past group G is put into R as one block, its calls in call order,
   right after the last call c of R that must come before it:
   - c is a barrier; or
   - c is an input call, a state call or a group call whose time is at
     or before `τ` and whose footprint meets `F(G)`.

   Every call of R after that place has a time later than `τ` or a
   footprint disjoint from `F(G)`. The test is per call, so G can go
   between the calls of an earlier group's block. That is allowed and
   harmless: every call G passes shares no node with it or is later
   than `τ`, and calls that share no node commute.
3. An ahead group waits, with or without a window. It is added at the
   end of R right before the first later call, an input call or a state
   call, whose time is later than `τ` and whose footprint meets `F(G)`,
   or right before a barrier. For this test a state call's time is `N`
   at its call (input that does not meet `F(G)` can move `N` past `τ`;
   in a past group, the group's `τ`, below; in 0.7.0 `N(F(c))`, and a past group's `τ`, "Late input", S1'),
   and an arm call's is the smaller of its `t` and `N`: a present arm
   lets in an earlier waiting group it meets (it is at `N`), a past arm
   only one earlier than its own time (E's example: the arm at 40 ms
   does not let in the one waiting at 60 ms). An ahead arm lets in
   nothing at its call. It goes into R when its group goes in, and lets
   in then, at its `τ`. Reason: the ahead arm is not in R at its call.
   Letting a group in there put that group's calls into R before input
   that must come before them (the reviewer's H12: a waiting arm's set
   made at a later ahead arm's call, before input reached the waiting
   arm's time). An arm call lets in
   a waiting group it meets whose `τ` is at or before that time: at
   equal times rule 2 puts the earlier call first (S3). A state call's
   letting in takes effect only when the call itself goes into R: a
   state call in an ahead group lets in nothing until that group goes
   in, so nothing in the group, its triggers included, is made before
   (H9; sets, set_data, disarms, recenters and channel index sets
   alike). The calls of a past group go into R at the group's place
   (rule 2), at the group's `τ`. A waiting group that one of them lets
   in goes in right before that call, at the group's place, in its
   block; the group's own calls alone set that place. A waiting group
   that such a call let in at its call, at `N`, and that `τ` does not
   let in, goes back to waiting. Reason: this is H9's principle (a call
   lets in where it goes into R), and it keeps the waiting group in time
   order. At the call's position in call order the waiting group could
   follow input later than the past group's `τ`, and so pin that group
   (the reviewer's H13).
   Waiting groups go in by `τ`, then call order. A group that never
   meets such a call is never made. A state call with a time before
   `τ`, or at `τ` and made before G's arm, goes before the group: a
   `ysig_set()` of its detect then applies to the waiting arm. A disarm cancels a waiting arm:
   - a `ysig_disarm(d)`, in a group or in no group, whose time is at or
     before `τ` cancels the waiting arms of d made before it. One whose
     time is later than `τ` goes after the arm, in time order (the arm
     goes in first, by the test above). The cancel is decided when the
     disarm goes into R: at its group's arm, at the end of its gap (the
     next input call or barrier), or, in a waiting group, when that group
     goes in. The final hits are the same either way;
   - a barrier cancels every waiting arm. The other calls of a waiting
     group go into R right before the barrier, without the arm.

   A cancelled arm is not made and is counted in `arm_ahead_cancelled`.
   The other calls of its group still go in by this rule, and their
   footprint for the test is the union of theirs alone. Reason: a
   disarm ends what the caller armed d for. A caller that leaves a state
   before the input reaches the time it armed that state's control at
   (the player arms at a mark's onset, which can be ahead of the poll)
   disarms the control on exit; without the cancel, the waiting arm
   would arm d later for a state that has ended. A barrier ends the
   trial the arm was for. Two arms with no disarm between them (E) stay
   as above: both are made, by `τ`.
4. Input calls keep their call order in R. A barrier is never crossed.

Without a window an ahead group's set, set_data and recenter calls
cannot be taken back once made: they stay where made, and the arm waits
(counted in `arm_past_off`; with a window whose log did not keep one of
them, in `arm_past_full`). A disarm can wait: it takes effect at the end
of its gap (below), so in an ahead group it waits with the arm, with or
without a window. At most 32 arms and 64 group calls wait; an arm with
no room is made at once on the input known, counted in `arm_past_full`.

A read in an open gap (`ysig_value_of()`, `ysig_value_at()`,
`ysig_detected()`, `ysig_derived()`, `ysig_get_stats()`) returns the
state after the gap's calls, made at `N` as a present group's are: its
sets applied, and the waiting groups they let in at `N` made. A disarm
takes effect at the gap's end (below), so its detect still reads as
armed. Reason: until an arm closes the gap, the gap is a present group
or no group, and R puts both at `N`. A read cannot know that an arm
follows, and to hold every state call back until its gap ends would
delay the sets that no arm follows. When an arm then makes the group
past or ahead, the run again changes the state, and a hit handed out
that R does not have is counted (`arm_hits_retracted`).

Equal times: a call at `τ` that must come before G (rule 2) does. So an
input at `τ`, a frame with onset `τ`, a settle at `τ`, and a group at
`τ` made before G come before G; a group at `τ` made after G comes
after it.

Which input restricts a group's place (A): only input whose footprint
meets the group's. Reason: other input commutes with the group, so
every node sees time order whichever side of the group it goes.

A late report (A, the second case): a report at or before `τ` from a
device that does not feed the group's nodes has a disjoint footprint.
The group passes it, so the input later than `τ` before it does not pin
the group.

**Pinned.** One case stays out of time order: an input with time at or
before `τ` whose footprint meets `F(G)`, made after an input later than
`τ` whose footprint meets `F(G)` (a late report from a second device on
a source or sum the group reads). Rule 2 puts G after the late report,
so after the later input too: G's nodes see that later input before G.
Input keeps call order, since a late report is late for every node, so
R cannot put the late report first. G is **pinned** when an input call
after the last barrier and before G's place in R has a time later than
`τ` and a footprint that meets `F(G)`. A state call cannot pin a group:
outside a group its time is `N` at its call, so after an input later
than `τ` it is later than `τ` too. A pinned group is counted in
`arm_past_pinned`, a fallback: the player's guard must see it. Before
the window's oldest checkpoint the graph keeps no input's footprint:
when an input there is later than `τ` (and no barrier is between), the
group counts as pinned whatever that input's footprint (the reviewer's
H1: frames older than the log).

In 0.7.0 no group is pinned (the coordinator, 2026-11-15): ruling 8's
clamp puts such a group after the feeds at the barrier's time, by key
(an input at the last barrier's time comes before a group whose `τ` is
before that time). A place before the window falls back as
`arm_past_far`. `arm_past_pinned` stays in the stats and reads 0.

Arms ahead and an earlier arm (E): an ahead arm waits for input past its
time, so a later arm with an earlier time goes before it in R. For
`ysig_arm(d, 60 ms)`, frames at 16 and 33 ms, a crossing at 41 ms, then
`ysig_arm(d, 40 ms)`: R is the two frames, the arm at 40 ms, the
crossing (a hit at 41 ms), and the arm at 60 ms before the first input
later than 60 ms. Reason: this is time order for the detect and every
node after it. An arm made at once ahead of its input ended the earlier
arm early (its hits between `N` and `τ` were lost) and showed users the
output's fall before the input up to `τ`.

#### What an arm does at its place in R

At its place, `ysig_arm(d, τ)`:

1. Meets d's hold up to `τ`: when d holds under its earlier arm and the
   hold's start plus its length is at or before `τ`, that is the earlier
   arm's hit, at start plus length. Any detect: on a view and at frame
   rate too. Reason: at its place in R, input later than `τ` that d reads
   exists (past: already; ahead: the call that places it), so input up
   to `τ` is in.
2. Arms d at `τ`: its test on its input's held value at that place, then
   the entry rule (`YSIG_DETECT_ENTRY`): arm, fresh or now, as
   documented. A sample earlier than `τ` that d takes after this is
   before the arm (no hit, no hold start; a failing value breaks a
   hold and makes d ready again, as any failing value does: for a
   fresh entry, it is the failure the arm waits for, the reviewer's
   O3).
3. Changes d's output (a hit output): a fall to 0 at `τ` when a hit
   stood, then a rise to 1 at `τ` for an entry-now hit.

A present arm does the same (`τ = N`).

A disarm at its time (its group's, else `N` at its call) first meets
its detect's hold up to that time, as rule 1 of the arm, then ends the
arm. Reason: the exit that a hit causes disarms the control at the
hit's time; a hold that ended then is the hit that caused the exit, and
must stand. A disarm takes effect at the end of its gap: at its group's
arm, at the next input call, or at a barrier (in a waiting group, when
the group goes in). Until then the detect stays armed. Reason: its time
is known only then, and a graph with no window and one with a window
must agree (a disarm made at its call with `N` would meet a hold that
its group's earlier time does not).

#### A change at an arm reaches every node (D)

A fall or a rise that an arm makes is a new value of the detect at `τ`.
Each goes through the graph as a sample's value does: every node in
`D(d)` other than d, in the graph's order, takes it as a new value of its
input at `τ`, as in a sample's pass. A delay, velocity, speed or lowpass
takes it as a sample at `τ` (two values at one time as for samples); a
custom op's `sample()` runs; a record writes a row; an emit emits; a
latch or peak gates on it; a detect tests it. A fall and a rise at one
arm are two passes, the fall first. A node that runs only at frames
takes the change at its next frame, as any sample's change. An
entry-now hit at a reset is the same.

Reason: an arm's change is input to the detect's users like any other.
Their rules for a value at a time then apply unchanged, and no node kind
needs a rule of its own. This removes the named exception (a detect on
another detect's hit output) and the second review's D (a latch after a
`delay(hit, 0)`, velocity, speed, lowpass, custom ops).

#### Hits, rows and derived events

The queue holds R's hits in R's order. A hit the caller already had is
not queued again, except the hits of a past group's detect from `τ` on in
R: those are the new arm's, also when equal to one handed out. A hit
handed out that R's result does not have (matched by node, `t` and the
whole value) is counted:

- `arm_hits_superseded`: a hit later than `τ` of a detect that the
  group arms or disarms (its earlier arm's: a caller that ends that arm
  at `τ` took none of its later hits);
- `arm_hits_retracted`: any other, another detect's at any time too (the
  caller may have taken it with the hit it acted on).

`on_row` and `ysig_derived()` get R's rows and events once. When they
get them depends on the desc:

- **At once** (the default; always with no window): at the call that
  makes them. When a past group changes ones already handed out, in any
  column, the arm is counted in `arm_rows_stale` or `arm_derived_stale`.
  They are not handed out again.
- **Final only** (`desc.rows_final`, `desc.derived_final`, each with a
  window): a row or a derived event is held until it is final, then
  handed out, in R's order per node; the order across nodes of calls
  with disjoint footprints is unspecified. It is final when the call that made it is
  older than the oldest call the window can still run again: the
  oldest checkpoint's, or the log's oldest after the logs ran full. A
  later past group can then not change it: its place would be before
  the window, so it falls back (`arm_past_far`, `arm_past_full`). A
  barrier makes everything before it final, and `ysig_flush()` (new: a
  barrier that resets nothing) does the same between trials or at the
  end of a session. Final derived events go to the 64-event queue as it
  has room; the rest wait in the held buffer, final, and each
  `ysig_derived()` call refills the queue, so a caller that drains until
  0 loses none (a barrier can make hundreds final at once). Reason: no
  more arena than the held buffer; the queue's 64 stay the limit between
  drains, as for events made at once. In this mode `arm_rows_stale` (or
  `arm_derived_stale`) cannot occur, and every row (or event) handed
  out is R's. Latency: up to `catchup_frames` frames, as the oldest
  checkpoint is that many frames old (with the logs full, less). The
  held rows take arena room: `catchup_rows` rows (new desc field;
  default 4 × `catchup_calls`). When it is full, the oldest held row is
  handed out at once and the window drops the checkpoints older than
  its call, so the row is final (a shorter window, still exact). An arm
  whose place a dropped checkpoint held falls back as after any log
  that ran full (`arm_past_full`, not `arm_past_far`: the reviewer's
  H10). Reason: the cause is the buffer's size, a knob, not the
  window's length.

#### Counts and fallbacks

| Count | When |
|---|---|
| `arm_caught_up` | a past group placed before calls already made (the window runs them again) |
| `arm_past_pinned` | a past group pinned (above); placed by rule 2 |
| `arm_past_far` | a past group whose place is before the window's oldest checkpoint: its calls are made at their call positions, the arm as described above (input later than `τ` already seen) |
| `arm_past_full` | the same after the window's logs ran full |
| `arm_past_off` | the same with no window (an oracle's graph counts this; it is not compared) |
| `arm_ahead_cancelled` | a waiting arm cancelled by a disarm or a barrier (rule 3); not a fallback |
| `arm_hits_superseded`, `arm_hits_retracted` | above |
| `arm_rows_stale`, `arm_derived_stale` | above; never in final-only mode |
| `arm_reentry`, `call_reentry` | a call from a callback during another call: refused, not in R |
| `arm_unverified` | an arm whose group is outside the exact shapes (below); R is made as usual |

A fallback is not R's result. The player's guard must stop on
`arm_past_pinned`, `arm_past_far`, `arm_past_full`, `arm_hits_retracted`,
`arm_rows_stale` and `arm_derived_stale` (with a window, `arm_past_off`
does not occur).

#### Exactness: `arm_unverified`

R's result is defined for every call sequence. The header gives R's
result for an arm exactly when the arm's group G meets the conditions
below (the shapes the independent oracle covers). When G does not meet
them, the header counts the arm once in `arm_unverified` and makes R as
usual. A count means "not verified", not "wrong".

Terms:

- `τ` is G's time and `F(G)` its footprint.
- `F_a(G)` is the footprint of G's arm and its disarms. For a cancelled
  arm, it is the footprint of the disarms only.
- `N(G)` is the latest input time in R before G's place, after the last
  barrier. A reset and a trial reset set it to their `t`; a flush keeps
  it. In 0.7.0 only an input whose footprint meets `F(G)` counts (a
  frame or a settle meets every footprint; ruling 4 below).
- `L(G)` is the first call after G in R that lets G in and whose
  footprint meets `F(G)`: an input later than `τ`, a frame or a settle at
  `τ`, or a `ysig_disarm_at` with `t` at or after `τ` (X1). (R1,
  2026-11-15.)

G is exact when all of these are true:

1. G is not inside another group's block: no other group has calls both
   before and after G in R.
2. `N(G) ≥ τ`. Or, when G waits at its place (`N(G) < τ`), all of these:
   - (a) `L(G)` is in R, it meets `F_a(G)`, and no barrier is between G
     and `L(G)`.
   - (b) Between G and `L(G)`, no state call of another group, or of no
     group, meets `F(G)`. The exception is the arm or a disarm of a group
     H that holds its arm and disarms only (no set, set_data or
     recenter), that is later than G by (`τ`, call order), that waits at
     its place too, and whose own L is `L(G)`: `L(G)` lets H in, and no
     input between H and `L(G)` does. Reason: R makes H's disarms and arm
     at `L(G)`, after G, as a graph with no window does in call order. An
     input between them that let H in would make H before G there (the
     reviewer's 489). The player makes this shape when a state exits at
     the time its control was armed for (the arm, then the exit's
     disarm, at one time). The exception also needs G's arm to go into
     R before H's calls. At `L(G)`, the groups at G's `τ` that wait for
     `L(G)` go in by call order. Each arm lets in, right before itself,
     the waiting groups at its time that are earlier by call order and
     meet it, and right after itself, every waiting group at its time
     that meets it (rule 3, H16). So a group K at `τ`, made before G,
     whose arm meets H, directly or through a chain of such arms, lets
     H in before G. When H disarms G's detect, R then cancels G's arm
     (the oracle's case 1061 of seed 777016: K arms the detect that H
     disarms). The header orders up to 32 groups at one `τ` for one
     `L(G)`; with more, it counts G. Groups at an earlier time go in
     before G whatever they let in, and their arms let in no group at
     G's `τ`.
     A `ysig_disarm_at(d, t)` with `t` at or after `τ` is not counted
     here, whichever control it disarms: it lets G in right before
     itself, so a graph with no window makes G there, as R has it, and
     every call after it comes after G in both (X1).
   - (c) When a disarm or a barrier cancelled G's arm, G has no disarm.
     This one holds for any `N(G)` too: when a disarm or a barrier
     cancelled G's arm and G has a disarm, G is not exact unless
     `N(G) = τ`. Reason: a graph with no window makes the remaining
     disarm when the group goes in, where `N` is past `τ` (the reviewer's
     490 and 1821; the header acts at `τ`, as S1 says).
3. When G waits at its place (`N(G) < τ`): while G's arm is its detect's
   newest arm in R, no input whose footprint meets `F(G)` and whose time
   is earlier than `τ` goes into R after G (a late report). A group with
   `N(G) ≥ τ` is exact by condition 2 whatever comes after it: input up
   to `τ` was in before it, so a later report earlier than `τ` is late
   for every node, as for any call (a slow device's late sample after an
   arm at a frame's onset). An input at `τ` is not a late report: a feed
   at `τ` goes before G by key (L3); a frame or a settle at `τ` goes by
   call order. With a window this condition no longer fires: a report
   earlier than `τ` goes before G by key, to its place. It applies only
   where a report cannot be placed (no window, a place before the
   window, or the log full); those cases count `late_off`, `late_far` or
   `late_full` (the coordinator, 2026-11-15).

4. No disarm of another group, or of no group, at a time other than
   `τ`, lies in the gap that G's arm closes with no window: between G's
   arm and the last input, barrier, arm or `ysig_disarm_at` before it in
   R (ruling 1 below). Reason: with no window that disarm
   joins G's gap and takes G's time (the reviewer's 883 and 404: a
   disarm in no group, then G's calls; 1137: a present group's disarm,
   then the group its arm lets in right before itself). A disarm in that
   gap at G's own `τ` is not counted: taking G's time does not move it
   (the header's `ysig__c4()`, and the oracle's B-cannot reason 9 and
   predicate, skip it too). The condition
   covers disarms only: the reviewer tested every state call there,
   which flagged more cases and found no other wrong one.

Condition 1 includes a group let in inside an open gap that an arm then
closes: what a state call of the gap, or the arm itself, lets in goes
after the gap's earlier calls and before the arm, inside that group's
block (the reviewer's 1137).

When the header tests:

- At G's going-in: conditions 1 and 2. When `L(G)` is not in R yet, the
  header counts G then. This covers reads made while G waits. The
  exception is a **pending** group (0.6.1, below). `N(G)` is read in R's
  key order (ruling 4): an input of another device later than `τ`, made
  before `L(G)`, goes after G and does not make `N(G)` reach `τ` (the
  oracle's case 218 of seed 20261121, `OFF=l`: the header read it as
  `N(G)` and did not test 2 (a)).
- At G's going-in as a late call (a waiting group whose key is smaller
  than a meeting call already in R, W1): 2 (b) again at G's place, over
  the calls G goes before, since `L(G)` comes after them (the oracle's
  cases 297 and 616 of seed 20261121, `OFF=l`: a present group's set made
  while G waited, at an `N(F(c))` that did not let G in). The 2 (b)
  exception is not ordered there: such a call counts G. With no window
  the header cannot place G and counts `late_off`; it counts G by 2 (b)
  too, unless `N(G)` is `τ` (the coordinator's ruling of 2026-11-15:
  `late_off` alone did not count G; `sig_test` `test_v2_n_tau` and
  `test_v2_present_key`). A past group's
  call is never among them: one that meets G with a greater key lets G
  in at its `τ`, so G goes in at that group's place, right before the
  call (H13), and is tested there; `L(G)` is not an input then, and 2 (a)
  counts G (the oracle's case 899 family: a past group's set at 54.187
  ms, G waiting at 49.937 ms; `sig_test` `test_v2_past_letin`).
- At each later past group's place between G and `L(G)`: condition 2 (b)
  again, over every call between them, since a later group can be the
  link of a chain that lets an earlier H in before G.
- (0.7.0, "Late input") At each late call that takes a place
  between G and `L(G)`: a state call that meets `F(G)` retests 2 (b)
  (U2), except a `ysig_disarm_at` with `t` at or after `τ` (it lets G in
  before itself, X1): that one becomes G's `L` (R2), 2 (a) and 2 (b)
  again with it, so a later call placed after it is outside G's span (the
  player's 60 Hz seed 102788: a late report after the trial's end); an
  input later than `τ` that meets `F(G)` becomes G's `L` and
  retests 2 (a) and 2 (b) (L5). In the retest with the disarm_at as `L`,
  2 (b)'s exception holds as at G's going-in: a group H between G and the
  disarm_at that the disarm_at lets in too (X1), after G, is not counted
  (the player's 60 Hz seed 102401: an until's arm at `τ`, the next
  until's arm of the same control at `τ`, then the state's disarm_at at
  `τ`; `sig_test` `test_v2_two_untils`); one that meets an exception group there
  retests 2 (b) with R's order at `L` (L6). With an input as the new `L`
  (L5) the exception holds the same way: 2 (b) is one condition (the
  reviewer, 2026-11-15). A group H between G and the input, that the
  input lets in too and no input between H and it does, is not counted
  (`sig_test` `test_v2_l5_exc`). Without it the retest only counted more.
  R's order at the new `L` reads every group that `L` lets in, so the
  header gives each of them the new `L` before it retests any (the
  reviewer's op-order gap: a group K made before G but retested after
  it was left out of G's order, and G went uncounted; `sig_test`
  `test_v2_op_order`).
- Condition 3 (a late report of an arm in R): met by the placement
  code. A late report goes to its place by key and the window runs
  again, or it falls back counted (`late_off`, `late_far`, `late_full`).
  0.6.x's table of armed detects (`w->lr`) went in 0.7.0: it had no
  reader, and on its overflow at 32 armed detects it counted an
  `arm_unverified` that no shape needed.
- Condition 4: at G's place (a past group), at the trigger that lets G
  in, or at G's arm at the end of R. At the place, a group that the past
  arm lets into its block is tested there too (`sig_test`
  `test_v2_c4_past`); a disarm at G's own `τ` is skipped there as in
  `ysig__c4()` (V2SIG8: the place's test counted it).
  Where the log no longer holds G's gap (its calls left with the
  oldest checkpoint, or the logs ran full and keep nothing until the
  next frame), the header keeps the key times of the disarms it lost
  since the last barrier. When one of them can lie between the gap's
  end found in the log (or the barrier) and G's key, G is counted: the
  header cannot read the gap, as before the window in 0.6.x (H1). (The
  oracle's case 1928 of seed 20261124, `SIG_ORACLE_PAST_GROUPS=1`,
  `SIG_ORACLE_PRED_TARGET=1`: the 64-call log ran full at call 54; a
  disarm in no group at 27.458 ms, device 1 at 31.561 ms, then a group's
  arm at 30.561 ms that a settle let in. By key the input is after the
  arm, so the disarm is in its gap. `sig_test` `test_v2_c4_lost`.) Cost:
  0.3% to 1.1% more counts in right cases (the oracle at ctest's
  switches, seed 20261115: 2,788 to 2,799 of 31,205 made groups, late
  generator on; 4,017 to 4,063 of 43,438 with `OFF=l`).
- (R2, 2026-11-15) At each late disarm whose place is in the gap a
  group's arm closes in R (back to the last input, barrier, arm or
  `ysig_disarm_at`): condition 4 for that group.
- At a cancel by key of an arm already in R (a disarm with a smaller key
  than the arm's group): condition 2 (c) for that group, at the disarm's
  call. (The reviewer's text gap, 2026-11-15: the oracle's reason-8
  misses, case 1378 of seed 20261202 and 1050 of 20261207; spec case 10,
  "2c-at-cancel-by-key", in `sig_disarm_at_cases.c`; `sig_test`
  `test_v2_c2_cancel`.)
- At a cancel by key that takes an arm out of R: condition 4 again for
  the group whose arm comes next after it in R. That group's gap now
  reaches back past the removed arm, so it can hold a disarm the arm
  bounded before. (The reviewer's case 2107 of seed 20261124, late on,
  `SIG_ORACLE_PRED_TARGET=1`: G0 = [disarm d, arm d at 2.193], then a
  past group G3 = [arm e at 2.934]; a late `ysig_disarm_at(d, 0.000)`
  cancels G0's arm, and G0's disarm at 2.193 is now in G3's gap.
  `sig_test` `test_v2_c4_cancel`.)
- At the arm call of a group J that waits (with a window): a waiting
  group H with the smaller key whose footprint J's sets, set_data or
  recenters meet is counted, and J is counted when a waiting H with the
  greater key has such calls that meet J. A graph with no window, given
  R, makes those calls at once, so it cannot give R's order if R lets
  the two in that way (the oracle's B-cannot reason 4). The test is
  conservative: at J's arm the header cannot know which goes in first.
  It compares keys: before the last barrier, call order (V2SIG10; by raw
  `τ` it counted H only at its going-in, after the call B cannot
  follow, and counted J where R makes H's calls first anyway. `sig_test`
  `test_v2_wk_reason4`).

**Pending groups (0.6.1).** This case does not arise in 0.7.0 (the
coordinator, 2026-11-15); the paragraph is kept for 0.6.x. G is pending when it holds its arm and
disarms only, `N(G)` is earlier than `τ`, `L(G)` is not in R yet, and no
state call after G in R meets `F(G)`. R makes G at its place, and so does
the header. A graph with no window, given R, keeps G's arm and disarms
waiting until a later call that meets `F(G)` lets them in (rule 3).
Until then the two can differ only in what a read sees of G, so the
header does not count G at its going-in. It decides G at the first of
these:

- **G's own call.** When G's arm or disarms change, at that call, a value
  or a ring entry of a node in `F(G)`, or the hits (an entry-now hit, a
  hit output's fall, a hold that the arm completes for the earlier arm),
  a read differs from the graph with no window: counted then.
- **An input later than `τ` that meets `F(G)`** (`L(G)`, a frame and a
  settle too: their footprint is every node): it lets G in, after every
  call between, none of which met `F(G)`. Decided exact; condition 3
  stays on G (below).
- **An input that does not meet `F(G)` but lets in another group whose
  footprint meets it** (a pending group or a waiting one): the graph
  with no window makes that group first and G later, against R's order
  (the oracle's "two waiting arms let in out of R's order"). Counted.
- **An input at or before `τ` that meets `F(G)`:** R puts it after G's
  arm, the graph with no window before it, so the arm's entry reads it
  there. Counted (one earlier than `τ` is condition 3's late report).
- **A state call at the input time that meets `F(G)`:** a call in no
  group, or a call of a present group. Its time is the input time,
  later than `τ`, so it lets G in first (rule 3), as R has G before it.
  Decided exact. This covers a disarm of G's detect when the caller ends
  G's state: the disarm's time is later than `τ`, so it makes the arm
  and ends it; it does not cancel it.
- **A call of another group that waits too and meets `F(G)`:** a past
  group placed after G, or an ahead group. A graph with no window makes
  that group's set, set_data and recenter at once, before G's arm, and
  the order of two waiting arms at `L(G)` needs `L(G)` to know (2 (b)'s
  exception). Counted then.
- **A barrier** (`ysig_reset()`, `ysig_reset_trial()`, `ysig_flush()`):
  counted. The graph with no window cancels the waiting arm there (rule
  3), while R has it made at its place: for a flush, which resets
  nothing, the detect stays armed in R and not in the other graph. The
  oracle cannot check either result (its "a barrier while an arm waits
  in B"), so the header counts G. The player ends each trial with a
  flush, so a G still pending at a trial's end is counted there, and the
  player's guard reads the counts after that flush too. A pending G
  never decides itself: a caller that makes no more calls keeps it
  pending, which no read can tell from R.

A G decided exact stays an open group with the call that let it in as
its `L`: a later past group placed between G and that call is tested by
2 (b), as for any G whose `L` is in R (the oracle's 890 of seed 778001: a
group at an earlier time placed after G, before the frame that let G
in).

The window does not decide a pending G. When the log drops G's calls
before `L(G)` (the second hand silent for longer than the window: 8 ms
at 500 Hz), G stays pending, and the calls above still decide it: none
of them needs G's calls in the log. A later read-visible change from G's
arm needs an input or a call that meets `F(G)`, and each of those decides
G first.

Reason: two hands, each a device. The player arms both hands' controls
at the time of a hit that a sample of the first hand found. The second
hand's sample at that time is not fed yet, so the second hand's control
has no `L` in R. Before 0.6.1 each such entry counted one
`arm_unverified` and the guard stopped the trial (the bimanual
definition of 2026-11-14, two relative mice at 1 kHz). With 0.6.1 the
next frame or the second hand's next sample decides it, exact. An
earlier draft (not committed in the header) made such a group wait in
the header too; the oracle showed that is not R: R makes the arm at its
place, and a report at or before `τ` that comes after it is before the
arm (O3), where a waiting arm would read it.

Condition 2 (c) counts the reviewer's reason 8. A barrier (a flush) lets
in a cancelled group whose disarm completes a hold at a `τ` later than
any input. The hit is handed out at a time that no input has reached.
That cannot be exact: the disarm meets the hold up to `τ` as if input up
to `τ` were in, and none is. An input earlier than `τ` that fails the
test, made after the barrier, would have broken the hold in time order.

#### What the caller can rely on

After each call returns, every node's value, ring and state, the bound
channels, and every count but the window's own (the `arm_*` counts,
`call_reentry`, `derived_lost`) and the digest are those of R's calls so
far on a graph with no window. A waiting ahead group is not yet in R.
One known exception, not fixed: `history_skipped` (a value older than
its ring's newest, not kept) can differ by a few. In the reviewer's
`residual_history_skipped_1336.c` (the header's calls) against
`residual_history_skipped_1336_r.c` (R's calls on a graph with no
window), every node's value agrees, and so does every entry's time in
every node's ring at the end (all entries of all rings printed and
compared, 2026-11-14), but the graph with no window evaluates node 9
with its stale value twice more: 18 skipped pushes against the run
again's 16. The count is a diagnostic; nothing reads it. The same holds
for `time_clamped` (a detect's value whose time went back, kept at the
detect's last time): in `residual_530_a.c` against `residual_530_r.c`
every value agrees, and the count is 1 against 2 after call 19, where
ahead groups with a recenter of role 0 at 0.377 ms go in at the input
at 8.953 ms.
Every hit of R's result is handed out once; a hit handed out that R does
not have is counted, as above. Rows and derived events: as above. With
no fallback count, no retracted hit and no stale row or event, the
caller's view is exact time order with input in call order.

The journal holds the calls in call order, not R. `ysig_replay()` makes
the same calls, so it builds the same R.

#### Memory (F)

A window's arena is the graph's plus:

- `catchup_frames` checkpoint images, each the graph's handle, nodes,
  values, op states, data, and both queues;
- `catchup_calls` logged calls (144 bytes each) and a hit log of as
  many hits;
- `catchup_writes` ring writes (72 bytes each);
- room in the live hit queue for `catchup_calls` hits beyond 64 (88 bytes
  each).

0.6.0 as built gives each image the same queue room (64 plus
`catchup_calls` hits and their sequence numbers, 96 bytes each). The
second review measured (GCC 12.2, a 3-node graph):

| Window (frames, calls) | 0.5.0 (c7dc717) | 0.6.0 as built |
|---|---|---|
| 4, 1024 | 1.06 MB | 1.55 MB |
| 4, 16384 | 6.2 MB | 14.1 MB |
| 8, 2^20 | 353 MB | 1.26 GB |

The live queue's room stays: a run again may queue up to
`catchup_calls` hits beyond 64 (more count in `hits_lost`), and the hit
log holds `catchup_calls` hits, so more room could not be matched
anyway. The images' room is cut. An image keeps 64 hits, as the queue holds
outside a run again. A checkpoint that a run again takes with more than
64 hits queued is not kept: a later arm uses an older checkpoint
(exact, a longer run again) or counts `arm_past_far`. That saves
`catchup_frames` × `catchup_calls` × 96 bytes: 0.39 MB at the player's
window (4 frames, 1024 calls; 1.55 MB for the 3-node graph, 9.21 MB for
a 27-node graph with 4,096-value rings), 6.3 MB at 4 and 16384.

#### What this changes for the player

- Rows: the player opens its graphs with `rows_final`. Its only row
  consumer is the sample traces (`v2_on_row`), which it keeps for the
  trial and writes at the trial's end, so a latency of `catchup_frames`
  frames costs nothing; its traces are then always R's, and a take's
  recenter that a past group moves back (format 3.12: recenter, then
  the until's arm) cannot make a stale row. Two player changes come
  with it: `v2_on_row` takes a trial's rows until the barrier that
  follows the trial (it now drops rows that arrive between trials), and
  the player writes a trial's traces after `ysig_flush()` at the trial's
  end.
- Derived events: the player feeds them (a movement's emit) to the
  response collector at each sample, and the collector's responses
  drive the states in that frame. A latency of four frames (67 ms at
  60 Hz) would move responses and the frames shown, so the player keeps
  them at once. A past group changes one only through a node it moves
  (a recenter or a set upstream of the movement's emit) or through an
  arm's change that reaches an emit. That is counted in
  `arm_derived_stale` and the guard stops. Nothing reads them early as
  a value: frame traces and drawing use `ysig_value_of()` and
  `ysig_value_at()`, which read R's state and are exact; latches and
  peaks (`freeze_at`, the peak gate) are graph nodes, so in R.
- The player drops an ended state's hits by `arm_gen`. With waiting arms
  it disarms, when a state ends, each of the state's controls whose
  latest arm is still ahead of the input, so that waiting arm is
  cancelled (rule 3). It disarms no other control: a disarm in no group
  takes the input time, and the next state's arm of the same control,
  made after a settle at an earlier time, would go before it in R and be
  ended by it (the model's seeds 62160, 63378, 65482, 68666, and with
  `GEN_MULTI` 24301, 25201, 26962, 27583 showed it).
- An ahead arm waits for input past its time: an entry-now hit at an
  arm after the frame's onset comes with the next input past that time.
  The model's GAPs `future-arm-hit` and `future-arm-hold` take that
  reading.
- `arm_past_pinned` is new; the guard stops on it. To measure how often
  the harness's multi-device cases pin.

Measured on the player at 4a0584d (2026-11-13, the Linux desktop,
Release, a scratch build with counters in sig.h's public calls, not
committed; `N` counted as above). The harness: seeds 42001 to 62000
(19,999 cases) and `GEN_MULTI=1` 19001 to 24000 (4,999); a line per
player run that opened a graph and exited (10,791 and 2,500). Its
generator makes no recenter and one pointing role, so the recenter and
pin rows are 0 by construction there; `player_test.py` (56 cases, its
recenter shapes too) and the two 3.18 examples cover them.

| Count | Harness single | Harness multi | `player_test.py` | Visuomotor, bimanual |
|---|---|---|---|---|
| Arms | 309,893 | 141,938 | 598 | 800, 148 |
| Past / present / ahead | 230,877 / 64,506 / 14,510 | 86,638 / 36,533 / 18,767 | 229 / 356 / 13 | 320 / 480 / 0, 16 / 132 / 0 |
| Ahead by: up to 1 ms / 5 ms / 17 ms / more | 2,325 / 4,334 / 7,851 / 0 | 4,507 / 1,643 / 12,617 / 0 | 2 / 7 / 4 / 0 | 0 |
| Arms at a mark's onset (recenter, then the until) / of them ahead | 0 / 0 | 0 / 0 | 3 / 0 | 0 / 0 |
| A recenter in a past group (it would move back) | 0 | 0 | 0 | 0 |
| A set or disarm in a past group | 0 | 0 | 0 | 0 |
| An arm made with an earlier time while an ahead arm waits (an exit before `τ`; same detect) | 0 (0) | 0 (0) | 0 (0) | 0 |
| Arms still waiting at the next barrier (cancelled by rule 3) | 108 in 61 runs | 10 in 8 runs | 0 | 0 |
| Past arms that would pin | 0 | 0 | 0 | 0 |

So, on these runs: no take moves a recenter back, no group carries a
set, no exit comes before a waiting arm's time, and nothing pins. The
change the player meets is the waiting itself: 33,277 ahead arms in the
harness and 13 in `player_test.py`, every one at most 17 ms ahead, and
118 in the harness that a barrier would cancel.

#### Late input (0.7.0)

Status: built in 0.7.0 (2026-11-15). This subsection changes R. It was
written for the reviewer's oracle before the header. The fourth draft
(2026-11-15) answers the reviewer's findings U1 to U5 on the third draft
(7a17176); the third answered T1 to T7 on the second (25837ee), which
answered L1 to L11 on the first (d906e19) and the user's decisions of
2026-11-15. Three tables at the end map each finding to its answer. The
rulings of 2026-11-15 below are in the header too. Where this subsection
and the rules above disagree, this subsection is the rule. The release
gates' results are in "Tests, fuzzing and mutations".

The user's choice (2026-11-14): the header places a late input at its
time, as it places a past arm, and the window bounds the lateness that it
can absorb. Rejected: the player waiting for complete input (drawing
would lag by the lateness), and keeping the guard's stop.

Reason: in 0.6.x a report that comes after a call with a later time
stays after that call in R (rule 4). A past arm at the time of a hit
goes after the late report, so later input reaches the arm's nodes
before it (pinned), or a waiting group sees a late report after its
place (condition 3). The player's harness (player.md, "A 500 Hz
display") stopped on the guard in 24, 41 and 58 of 1,000 cases at
500 Hz for samples 5, 10 and 20 ms late, and the window's length did not
change that.

**Terms.**

- **Input time of a footprint** `N(F)`: the latest time of an input in
  R whose footprint meets `F`, since the last barrier (a frame or a
  settle meets every footprint). After a reset, the reset's `t`, for
  every `F`. A flush changes no `N(F)`: it resets nothing. `N`, the
  input time of every node, is `N(all)`.
- **Classes, per footprint.** A group G is past when `N(F(G))` is later
  than `τ` at its arm's call, present when it equals `τ`, and ahead when
  it is earlier. In 0.6.x the classes used `N`: input of another device
  that does not meet G made G past although nothing G reads had reached
  `τ` (the two-hand entry of "Pending groups"). Such a group is now
  ahead and waits (rule 3); pending groups no longer arise.
- **The time of a call**, for the order below: an input's time (a feed's
  `e.t`, a frame's onset, a settle's `t`); a group's `τ` for each of its
  calls; `ysig_disarm_at()`'s `t` (new, below); a plain `ysig_disarm()`
  outside a group, `N` at its call, as in 0.6.x (the ruling below); and
  for any other state call outside a group, `N(F(c))` at its call. A
  barrier's: as in "The time of each call".
- **The key of a call**: (its time, its class, its call order). Classes,
  first to last at one time: a feed; then a frame, a settle, a state call
  or a group, by call order. Reason: a feed at a time carries the input
  at that time, which every other call at that time reads (L3, T7). A
  frame, a settle and a state call at one time are the caller's own
  moments, in the caller's order, as in 0.6.x. The player ends a state at
  a frame's onset and arms the next state there before it makes the
  frame, and the frame must see them (the 500 Hz tie, R1, 2026-11-15).
  A group's calls share one key, its arm's call order (T6: as S3 and H16
  order groups at one time), and stay one block in their own call
  order.
- **Late call**: a call c such that R already holds a call whose
  footprint meets c's and whose key is greater than c's (T2). Since c is
  the newest call, that means a later time, or the same time with c a
  feed and the other call not a feed. Lateness is the largest such difference in time. A call that is
  not late goes at the end of R, as before. A late call is an input
  (the case this subsection is for), a `ysig_disarm_at()` with an earlier
  `t`, a state call outside a group whose `N(F(c))` is earlier than a
  meeting call already in R (T3), or a group whose key is smaller than a
  meeting call already in R (a present group after a `ysig_disarm_at()`
  with a later `t`, or after groups that X1 or R1 let in).

**R as an order by key (replaces rules 2 and 4).** R holds the calls
since the last barrier (a raw mouse's count from before it is not in R;
it goes into its integrators, "Per device"). Of two calls whose footprints meet, the one with
the smaller key comes first. Calls whose footprints do not meet can be in
either order: they commute, so R is defined up to that freedom. The
order is acyclic: it is the order by key, restricted to pairs that meet.
Which calls R holds is decided as before: a waiting group's arm is in R
once something lets it in, and a cancelled arm never is (rule 3, with
the H9, H12, H13 and H16 readings).

Rule 3's let-in, in this order (T1): a call that lets a waiting group W
in also lets in, right before W, every waiting group whose footprint
meets W's and whose key is smaller, and so on for those, transitively.
So the groups a call lets in go in by key. The reviewer's case: W7 =
[arm d1 at 10] and W8 = [set k2, arm d2 at 10], made later, k2 reading
d1, both ahead; dev2@12 meets W8 only. 0.6.x let W8 in alone at dev2@12
and W7 at dev1@13. Now dev2@12 lets in W8 and, before it, W7 (they meet,
and W7's key is smaller). A report of dev1 at or before 10 that comes
later is a late input: it goes before W7, and the window runs again.
The key is the one of ruling 2 extended: two groups whose times are
before the last barrier both key at the barrier's time, and call order
decides between them. (Before V2SIG9 the header's closure compared raw
times and could put the group made later first; W1 then found the other
late and placed it by key, and counted it. `sig_test`
`test_v2_closure_key`.) The same key orders every other pick among
waiting groups: the groups one call lets in, those a past group's block
lets in, a present group's ties (S1' (b)), and the calls of the groups a
barrier cancels. Two groups before the last barrier go in by call order,
not by raw `τ`. (Before V2SIG10 these went by raw `τ`: with footprints
that do not meet, the hits came in the other order; at a barrier, the
set of the group made later lost. `sig_test` `test_v2_wk_fire`,
`test_v2_wk_barrier`, `test_v2_wk_blk`, `test_v2_wk_tie`.)

Rule 3's let-in time for a state call (S1', the coordinator's ruling of
2026-11-15, from the reviewer; it replaces S1 of the same day): a state
call of a present or an ahead group lets in at `N(F(c))` at its call; a
state call of a past group at the group's `τ`; a state call in no group
at `N(F(c))`; a plain disarm in no group at `N`. The readings
(reviewer):

- (a) The calls of an open gap let in at their call until an arm closes
  the gap. When the arm makes a past group, its calls let in at `τ` and
  the window runs again to correct them: a waiting group that `τ` lets
  in goes in right before the call, and one that a call let in at its
  call and `τ` does not goes back to waiting. With no window the gap's
  calls are made already: a group that `τ` lets in is made after them, a
  late call (`late_off`).
- (b) A let-in by a group's state call is inclusive by key: it lets in
  a waiting group whose `τ'` equals the let-in time `t` when that group's
  key is smaller. When `t` is earlier than the group's `τ` (a present or
  an ahead group's call at `N(F(c))`), that is every waiting group at
  `t`; when `t` is the group's `τ` (a past group), only one whose arm
  call is earlier. A state call in no group lets in strictly, as an
  input does. A call of an open gap does not know yet which it is: it
  lets in strictly at its call, and the header notes the waiting groups
  at its `t` that it meets. When an arm makes the gap a present group,
  those go in right before the call: the window runs again from the
  gap's start; with no window they are made after the arm, late calls
  (`late_off`).
- (c) With no window, a group made where R does not have it yet counts
  `late_off`.

Reason: `N(F(c))` never lets a group in on input that is not complete.
S1 (a group's `τ` for every class) did: a waiting group went in on
input of another device before the input of its own device was
complete, and a later report of that device voided a hit already handed
out (`late_hits_voided`, a guard stop). S1's own reason ("the only
reading under which a graph with a window and one without agree") is
false: with no window both readings count `late_off`; with a window both
are exact after a run again. The deciding case: d's arm at 19 waits
(d's device's last report is at 10); a present group [set d's level, arm
e at 24] follows; then d's device reports 15 with a value that makes d's
test false. Under S1 the set let d in at 24, and its hit at 19 was
handed out, then voided by the report at 15. Under S1' the set lets in
at `N(F(c))` = 10: d still waits, and nothing is voided. d's device's
report at 26 lets d in, by key before the set already made: a run again
with a window, `late_off` with none.

A late call goes before every call of R that it must precede and after
every call it must follow. When a call X already in R must follow the
late call but sits before a call Y the late call must follow, X moves
after the late call, and so do the calls that must follow X, by the
same rule (the reviewer's L2).

What this order says at one time:

- A feed at a time at or before `τ` comes before G, whatever its call
  order (L3): G's arm tests the held value at `τ`, and a report at `τ` is
  part of it. A frame or a settle at `τ` goes by call order: one made
  before G's arm comes before G; one made after comes after G and lets G
  in right before itself when G waits.
- A feed at a frame's onset comes before the frame, whatever its call
  order (T7): the frame's pass reads input at or before its onset. A feed
  at the onset fed after the frame call is late; the window runs again
  for the frame.
- A settle at `t` comes after the feeds at `t`. Among the frames,
  settles, state calls and groups at `t`, call order decides.

**State calls (T3, T4).** A state call outside a group goes into R by
the same order, at its time. Most are at R's end: `N(F(c))` is the time
of the newest input that meets c. One that a call already in R must
follow (a group made at a later `τ` that it meets, the reviewer's T3
case) is a late call: it goes before that group, and the window runs
again. With no window it stays at R's end (counted, `late_off`).
A late state call in an open gap (one that no arm, input, barrier or
`ysig_disarm_at` has ended yet) takes its place by key when the gap
ends: with its group's time when an arm closes the gap, else at its own
time. Until then a read sees it at R's end, made at its call, as "A read
in an open gap" says. A hit handed out that its placement voids is
decided and counted at the gap's end (`arm_hits_superseded` or
`arm_hits_retracted`, the split for a late state call); with no window
the call stays where made (`late_off`). A barrier ends a gap, so a gap
that the caller leaves open is decided at the trial's flush at the
latest.

`ysig_disarm_at(g, d, t)` (new): a disarm with the caller's time. It is
a state call of time `t`. It ends d's arm at `t`: an arm of d at or
before `t` goes into R before it, made, and is then ended (its hold met
by `t` is that arm's hit, as for any disarm); an arm of d with a `τ`
later than `t` is cancelled (below). So a disarm at a state's exit time
never cancels what R gives at or before that time (the coordinator's
requirement, T4).

X1 (the coordinator's ruling, 2026-11-15, from the reviewer's analysis):
`ysig_disarm_at(d, t)` lets in, right before itself, every waiting group
whose footprint meets `D(d)` and whose `τ` is at or before `t` (inclusive,
as an arm call: S3), by key; an arm of d among them is made, then ended
at `t`. Reason: the sentence above says an arm at or before `t` is made
and then ended, but rule 3's strict "earlier than" left a group at
`τ = t` waiting. The header does this with a window and without one.

A frame or a settle at `t` lets in, right before itself, every waiting
group whose `τ` is at or before `t` (inclusive, as an arm call: S3), by
key, with T1's closure. Reason: such a group has the smaller key; left
waiting, it would go in later, before a frame already drawn (R1,
2026-11-15).

**The cancel, by key (U1, U5; replaces rule 3's "at or before `τ`").** A
disarm of d, plain or with a time, cancels an arm of d made before it
(in call order) exactly when the disarm's key is smaller than the key of
the arm's group: the disarm then comes first in R, and the arm comes
after it, not made. That holds whether the arm still waits, or a call
already let it in (a closure of T1, or a past group's place): in R it
goes after the disarm all the same, so it is cancelled there too, and a
run again takes it out (U5: T1's W7 and W8 let in by the closure, then
`ysig_disarm_at(d1, 8)` cancels W7's arm at 10). An arm whose group key
is smaller than the disarm's is made first and then ended. At one time
the call order decides, and the disarm, made later, is the greater key:
an arm at `τ` and a disarm at `τ` give the arm, then its end (U1: dev1 at
100, dev2's last report at 95, `ysig_arm(C2, 100)` with entry now, then a
plain `ysig_disarm(C2)` at `N` = 100: the entry-now hit at 100 stands).
For a plain disarm at `N` this reads: it cancels an arm with `τ` later
than `N`, and makes and ends one at or before `N`.

A disarm also ends an arm of d made after it (in call order) whose key
is smaller than the disarm's (W2): in R that arm comes before the
disarm, so it is made and then ended there, as an arm made before it
would be. For example `ysig_disarm_at(d, 100)`, then `ysig_arm(d, 95)`:
R makes the arm at 95 and ends it at 100. The general rule: in R a
disarm of d ends every arm of d with a smaller key and cancels every arm
of d with a greater key that was made before it; an arm made after it
with a greater key is untouched. So a caller must not arm a state's
control at a time earlier than the time it disarmed the ended state's
control at: the player asserts that (`v2_arm_exit()`), since its next
state's arms are at the exit time or later.

A disarm at exactly a waiting arm's `τ` (W1): with a window, the arm
goes in later (when its group is let in), and the disarm, by key, moves
after it, with a run again. With no window the header cannot move a
made disarm: it counts `late_off`, so the no-window B is not the
reference for that shape (the oracle's spec cases are). The same holds
for U1's plain disarm at `N` when the arm at `N` still waits.

A declared change from 0.6.x (W3): a past group's disarm now cancels a
later arm of the same detect that is already in R, when the disarm's
key is smaller (its group's `τ` before the arm's). In 0.6.x the cancel
looked only at arms that still waited.

Ruling (the coordinator, 2026-11-15): a plain `ysig_disarm()` keeps
0.6.x's time, `N` at its call (the input time of every node), not
`N(F(c))`. So a caller that passes no time never cancels an arm that R
makes by `N`. With `N(F(c))` it could: T4's two-hand case, C2 armed at
100, the second hand's last report at 95 and the first hand's at 100 or
later, then the state ends; a disarm at 95 would cancel C2's entry-now
hit at 100, and one at `N` (100 or later) makes C2 and ends it.
`ysig_disarm_at()` is for callers that know the time. Keyed at `N`, the
disarm fits the order by key as any state call (U1 above).

A plain disarm at `N` also makes later reports of a lagging device late
(U3): its key is `N`, and a report with a time before `N` that meets the
detect, fed after it, must precede it. The window runs again for such a
report, and the report now reaches the detect before the disarm (in
0.6.x it came after the disarm and moved nothing of the ended arm). The
reviewer counted 199 such inputs in 180 of 6,000 cases and estimates
about 120 us a disarm with two 1 kHz devices and 10 ms of lag. The
player uses `ysig_disarm_at(exit time)`, which is not later than the
reports it has, so this does not touch it. The build measures the cost.

The player ends a state with `ysig_disarm_at(exit time)` for each of the
state's controls, made or waiting. That replaces 0.6.x's rule (disarm
only the controls whose latest arm is ahead of the input, ca9bc48): a
disarm at the exit time comes before the next state's arms at that time
by call order, so it no longer ends the next state's arm of the same
control (the shape ca9bc48 fixed). "What this changes for the player"
changes with it when built.

**Frames and settles (L1, L8).** A frame goes into R at its onset, by
key, and a settle at its `t`. A frame's pass reads every node as of its
place in R: the inputs at or before its onset, and no later one (views as
now: input up to the earlier of the onset and the poll). In the player
nothing changes for a frame made in time: it feeds the reports up to a
frame's onset before the frame call (`v2_feed()`), so the frame is at R's
end. A report later than the onset that a caller feeds before the frame
call goes after the frame in R, and the window runs again for the frame.
A frame made after state calls or groups at its onset comes after them
in R (R1).

What the display already used: the bound channels that `ysig_frame()`
wrote at the frame's call, from R as it was then. A late call placed
before an earlier frame changes what R gives at that frame. That frame
was drawn; its channels are not drawn again. The header counts each such
frame once in `late_frames`, not a fallback: the difference is the
input's lateness, which R makes visible, not an error. After every call
the bound channels hold R's values for the newest frame in R (a run
again writes them again, as in 0.6.x).

**Per device (T6).** A sample older than its device's last sample is
dropped and counted (`dropped_order`), as now: per device, time order is
the device's own, and a report that goes back is a device or driver
fault. The one exception is a raw mouse's counts, which are never
dropped (a sum does not depend on order): a count with an earlier stamp
than one of its own device already in R is a late input, placed by its
stamp like any other. The first draft's "never late against its own
device" holds for every source but a raw mouse.

A raw mouse's count stamped before the last barrier (the coordinator's
ruling H1, 2026-11-16, on the reviewer's `bar.c`) is not in R either, but
it is not dropped: the header adds it to each counts integrator of each
source that takes it, with no run again, and counts it in `late_barrier`.
A sum does not depend on the order of its terms, so the integrator's sum
is the sum of every count of the device. Every other input before the
last barrier is dropped, as before. Exactly:

- The count goes into the integrator's sum and position (with the
  integrator's bounds, at the time it is added), not into its ring, and no
  node after the integrator runs: they take the new sum at the device's
  next count. No hit, row or derived event comes from it.
- With a window, the count goes into every checkpoint image too (each is
  after the last barrier), so a run again from any image keeps it. A batch
  (`ysig_feed_n()`) does the same; such a count writes no log, so the
  batch's fit bound holds unchanged.
- An integrator reset or recentered after the count's stamp does not take
  it: the count is before that sum's origin. `ysig_reset_trial()` keeps an
  adapter's integrator (D19), so the adapter's integrator takes a count
  from before the trial's barrier; `ysig_reset()` does not.
- `stats.fed` does not count it (it is not one of R's calls).

`sig_test` `test_v2_bar_counts`.

**The batch, `ysig_feed_n()` (L7).** `ysig_feed_n(g, events, n)` is
`ysig_feed()` of each event in array order, with the window run again
once at the end, from the checkpoint at or before the earliest place the
batch needed, instead of once a late event. Per-device order
(`dropped_order`) and equal-time call order are the array's order. A late
event fed alone with `ysig_feed()` runs the window again at once.

The run again comes earlier, before the next event of the batch, in
three cases. Each is a point where per-event feeding has done its run
again and the batch would otherwise lose or merge it:

- The next event could end the window: a log has less room than one
  event can use (one call; one history write per source; per scheduled
  node two ring writes and one hit, derived event or row). Without this,
  a batch whose later events fill the logs loses the checkpoint of its
  run again and counts `late_full`, where per-event feeding does not.
  Why the bound holds: a source's schedule runs once a feed, and each
  node in it once. A detect makes at most one hit an evaluation (with a
  hold it returns before the crossing test, and with hold 0 there is no
  held hit to complete). A delay writes the one new sample to its ring and
  reads one entry; it does not release a queue. Only ring and history
  writes take undo entries; other op state is in the checkpoint's image.
  A build with `YSIG_DEBUG_FEED_BOUND` defined (the fuzz target sets it)
  counts each log's use in each such feed of a batch and aborts if a
  feed uses more than its bound.
- The next event could let in a waiting group (a waiting arm before the
  event's time), or a gap is open. Such an event can run the window
  again itself, so its log use has no bound.
- The pending run again is for late state calls only (an open gap's,
  placed at the batch's first input). Merged with a late event's run
  again, the hits it voids would count as `late_hits_voided`; fed alone,
  they count as superseded or retracted.

What batching changes. Values, rings, bound channels and the hits that
stand are those of the same events fed one call each; only the reads
between them are not possible. Rows and derived events are those of
per-event feeding only when they are final-only (`rows_final`,
`derived_final`). At once, per-event feeding hands out rows and derived
events between a batch's events, and a later event can change them; a
batch does not hand them out there. So `arm_rows_stale`,
`arm_derived_stale` and the at-once lists of rows and derived events are
lower or equal batched. For that reason the oracle's check below does not
compare at-once rows and derived events. Some counts are lower: fed one
by one, a report's run again can void a hit or change a drawn frame that
the next report's run again restores. The batch does not see that
short-lived state, so `late_hits_voided` and `late_frames` are lower or
equal, and so are the hits handed out (fed one by one, the restored hit
is handed out again). A batch never falls back where
per-event feeding does not (`late_full` 0 in the check below). The sum of
the voided-hit counts (`late_hits_voided`, `arm_hits_superseded`,
`arm_hits_retracted`) is lower or equal, but one of them can be higher: a
hit that per-event feeding voids as late and makes again before the
caller drains it, and that a later arm or disarm then voids, counts in
`late_hits_voided` fed one by one and in `arm_hits_superseded` batched
(1 case in 6,000 of the check below).

The player feeds a poll's reports with one `ysig_feed_n()`, drains the
hits, and takes the exits by time: format 3.11 rule 4, DECIDED
2026-11-15 ("allow, fix overflow"). Checked (2026-11-15, 77e070b,
Linux desktop, i5-14500, GCC 12.2, no other load):

- The reviewer's oracle, `SIG_ORACLE_FEEDN=1` (one graph fed per event,
  one in batches of 1 to 8, compared after each batch and each other
  call), seeds 20261115, 20261123, 20261124, the late generator on and
  off, 1,000 cases each: no batch that did not fit (131 cases at
  2dc806c), and in 14 cases a first difference, each a count, batched
  lower (`late_hits_voided` 8, `late_frames` 6). A variant that reports
  every count at every compare: where the void counts differ, the sum is
  never higher; the case above (20261123, case 110) is the only one where
  a void count is higher. After a count differs, the per-event graph has
  handed out one hit more in 7 cases, each a hit it voided and handed
  out again. ctest runs seed 20261115
  with the generator on (`test_sig_arm_ref_feed_n`, 30 s).
- A scratch search (two-node graph, random feeds, arms, disarms, sets,
  `ysig_disarm_at()` and batches): 5,000,000 sequences, no count of a
  fallback, `late_frames`, the voided-hit sum or `arm_ahead_cancelled`
  higher batched.
- The model harness (`tests/model/diff.py --extra=--test-feed-each`
  against batches), 1,000 cases each at 60 and 500 Hz, 0 and 10 ms late:
  `trials.csv`, `events.csv`, `frames.csv`, the streams and
  `session.json` equal in every case that ran (836 or 837; the rest the
  loader refuses), the `sig_arms` counts equal. Its cases have one role,
  equally late, so no report is late to sig.h (`late_input` 0): this
  checks the player's order, not the window's run again.

**What the header does.** With a window, a late call whose place is in
the window goes into the log at its place, the calls that move (above)
move, and the window runs again from the newest checkpoint at or before
the earliest place that changed. Hits, rows and derived events follow
"Hits, rows and derived events": a hit handed out that R no longer has
is counted (`arm_hits_superseded` for a hit later than the late call's
time of a detect whose input it changed, else `arm_hits_retracted`), and
so is a row or an event changed in at-once mode (`arm_rows_stale`,
`arm_derived_stale`).

**When R cannot be met** (new counts; renamed from `input_late_*` in the
second draft, since a state call can be late too):

| Count | When | What the header does | Guard |
|---|---|---|---|
| `late_far` | a late call's place is before the window's oldest checkpoint | makes it at the end of R, as 0.6.x does | stops |
| `late_full` | the window's logs ran full, or the batch did not fit | the same | stops |
| `late_barrier` | an input's time is before the last barrier's time | drops it: in R too it is not there, the trial it belongs to ended. A raw mouse's count is the exception (H1): it goes into each counts integrator of its source, with no run again (below, "Per device") | does not stop |
| `late_off` | no window (a zeroed desc) | makes it at the end of R; an oracle's graph counts this, not compared | n/a |
| `late_frames` | a frame already made whose bound channels R now gives otherwise | nothing: the frame was drawn | does not stop |
| `late_hits_voided` | a hit handed out that a late input's run again does not give (below) | nothing more: the caller had the hit | stops |

Ruling (the coordinator, 2026-11-15, on the builder's question): a hit
handed out that a late input voids counts in `late_hits_voided`, whatever
its time, and the guard stops on it. Reason: 0.6.x's
`arm_hits_superseded` covered a hit of an arm the caller itself ended or
replaced, which is harmless; a late input voids a hit with no caller
action, and the player takes hits at once to drive exits, so the trial
can hold an exit that R does not have. A late input is a feed, a frame or
a settle. A late state call (a disarm, `ysig_disarm_at()`, a set, a
recenter, a group let in late) keeps the 0.6.x split
(`arm_hits_superseded`, `arm_hits_retracted`). When one run again holds
both kinds, all of its voided hits count in `late_hits_voided`.

`late_frames` goes in the data: a trial's count is a column of
trials.csv, and the session's total is in session.json (the
coordinator's choice, 2026-11-15; player.md's data reference when
built).

**Holding hits and derived events (0.7.0; the player's wait for
complete input, docs/player.md).** The reviewer's text (M3):

`ysig_detected_to(g, out, cap, t)` (new) hands out, in R's order among
them, at most `cap` queued hits whose time is at or before `t`, and
returns their number. The queued hits later than `t` stay queued and are
not handed out. A queued hit is the caller's only once handed out: a run
again that no longer gives a queued hit removes it, with no count; a run
again that gives it again leaves it queued once; `arm_hits_superseded`,
`arm_hits_retracted` and `late_hits_voided` count only hits handed out.
`ysig_detected()` is `ysig_detected_to()` with no limit. Across calls
with a rising `t`, a hit held by an earlier call comes out after hits
that R has after it. Within one call, two hits come out in R's order
when R fixes their order: the calls that found them meet (always for
two nodes on one path, one downstream of the other), or a frame or a
settle found one of them. R does not fix the order of other hits: the
calls that found them do not meet, so they commute ("Footprint"), and
the hits can come out in either order. Take hits by their time and by
the caller's own order, not by the order of the hand-out. The player
does this (case 453, the coordinator's ruling of 2026-11-15). The queue
holds `YSIG_QUEUE_CAP` plus `catchup_calls` hits; a hit that finds it full is
counted in `hits_lost`, which a caller that holds hits treats as a guard
stop. A reset or a trial reset empties the queue, held hits too.

`ysig_derived_to(g, out, cap, t)` (new) is the same for derived events:
it hands out, in R's order among them, at most `cap` queued events whose
time is at or before `t`, and returns their number; later ones stay
queued and are not handed out. A queued event is the caller's only once
handed out: a run again that no longer gives it, or gives it otherwise,
removes it with no count and queues the new one; `arm_derived_stale`
counts only events handed out. `ysig_derived()` is `ysig_derived_to()`
with no limit. The queue holds `YSIG_QUEUE_CAP` plus `catchup_calls`
events; an event that finds it full is counted in `derived_lost`, a
guard stop for a caller that holds events. A reset or a trial reset
empties it. The same is true for derived events of different nodes. For
the events of one node, the order is R's within one call. Across calls,
an event that an earlier call held comes out after events that R has
after it.

**Rows and derived events (L9).** Final-only mode still holds: a row or
an event is final when the call that made it is older than the oldest
call the window can still run again, and a late call whose place is
before that falls back (`late_far`) and changes nothing handed out. A
final row's latency is the window's span, `catchup_ns`. In at-once mode
a late call that changes a row or an event already handed out, a frame
row too, counts `arm_rows_stale` or `arm_derived_stale`, and the guard
stops on them. The player opens its graphs final-only for rows, so its
frame rows never stop it; its derived events stay at once (a movement's
emit feeds the response collector), so a late report that changes an
emitted event stops it.

**The window in time.** `desc.catchup_ns` (new; the user's choice of
2026-11-15: a knob with a 50 ms default) is the window's span in input
time. The header keeps up to 8 checkpoint images, spaced in input time:
a new checkpoint at a frame or a barrier when the newest is at least
`catchup_ns / 8` older, and the oldest dropped when the newest is
`catchup_ns` past it. Memory for images then does not grow with the
display's rate, and a run again starts at most `catchup_ns / 8` plus one
frame before the place it needs. `catchup_frames` stays as the count of
images (1 to 8). `catchup_calls` and `catchup_writes` size the logs for
`catchup_ns` of calls; the player computes them from its roles' report
rates.

Sizing: the lateness to absorb is the frame loop's own (a report stamped
before a frame's onset that a poll reads after the frame call: up to one
frame period plus the frame's work), plus the transport: a raw mouse or
a pen up to its report period (1 ms at 1 kHz); a USB serial device with
HOST stamps behind an FTDI latency timer, 16 ms by default
(devices_spec.md); LSL streams, per-setup offsets of 6 to 20 ms that a
setup must measure (devices_spec.md, Kothe et al. 2025); an eye
tracker's delivery, which devices_spec.md lists only as a vendor's claim
(EyeLink 1.4 ms at 2 kHz). No device here has a measured lateness
(devices_spec.md: "A latency is never taken from a manual"), so the
default, 50 ms, covers a 16 ms FTDI timer, a 20 ms LSL offset and one
frame at 60 Hz. The player sets `catchup_ns` from the rig profile when a
role's device has a measured input lateness: that lateness plus two
frame periods.

**What a run again costs.** A run again costs the calls since its
checkpoint (measured above: about 0.76 us a call, 202.7 us for half a
frame of four 8 kHz mice). A device with a steady lateness makes every
one of its reports late; one run again per report is not affordable at
500 Hz, hence the batch. With one `ysig_feed_n()` a poll, a frame makes
at most one run again for late input, plus one per past arm or late
state call it makes. One run again replays at most the calls of
`catchup_ns / 8` plus the lateness before the newest call: with
`catchup_ns` 50 ms and a lateness of 10 ms, about 16 ms of calls; with
four 8 kHz mice about 512 calls, about 0.4 ms on the P-core; at 1 kHz
and two roles about 32 calls, 25 us. Estimates from the 0.6.0
measurements; the build measures them (a frame's work at 60 and 500 Hz,
the examples, and `--test-sample-late` 5, 10 and 20 ms).

**Exactness for late calls.** A graph with no window given R in order
places nothing: every call of R is in key order for the nodes it
reaches, so the oracle's B runs R as a graph that gets its calls on
time. The header gives R's result for a late call when it meets the
conditions below; else it counts the call once in `late_unverified`
(new; renamed from `input_unverified`; the guard stops on it) and makes
R as usual.

1. Its place is in the window and after the newest barrier.
2. When it takes a place between a group G with `N(G) < τ` and `L(G)`,
   meets `F(G)` and is later than `τ`, it becomes G's `L`: the header
   tests 2 (a) and 2 (b) again for G with that `L` (the new `L` must meet
   `F_a(G)`; L5), and counts G if one fails.
3. When it takes a place inside such a span and meets the footprint of a
   group H there that 2 (b)'s exception admitted, it can let H in before
   G (the reviewer's 489 shape; L6): the header tests 2 (b) again, with
   R's order at `L`, for every G whose span holds it.
4. When a late state call takes a place inside a waiting group G's span
   (from G to `L(G)`) and meets `F(G)`, the header tests 2 (b) again for
   G (U2: G = [arm d at 10] waiting, `L` = dev1@20, then a state call at
   time 15 that meets `D(d)`), except a `ysig_disarm_at` whose `t` is at
   or after `τ` (2 (b)'s exception, X1). This joins "When the header tests": a
   late call between G and `L(G)` is one more call that goes into R
   between them.

The oracle for `ysig_disarm_at()` (U4). The reviewer's B is the 0.6.1
header with no window, which has no `ysig_disarm_at()`. So the build
needs, for this call only: hand-derived spec cases in
`tests/adapt/sig_arm_spec_cases.c`, written by the reviewer, not by the
header's author; and a second B, the 0.7.0 header with a zeroed window,
used only for cases that call `ysig_disarm_at()`. That B is less
independent: it shares the header's code. It is trusted for this call
because with no window the header places nothing: `ysig_disarm_at()`
there is a disarm at `t` made at its call, with the cancel test above
against the waiting arms and the arm's end at `t`, the same two steps a
plain disarm takes in 0.6.1, only with `t` given. The spec cases check
that path against hand-derived results.

Every group condition of "Exactness" stays, tested at a group's going-in
as now, with `N(G)` from R's order.

**What the caller can rely on (0.7.0, in place of the 0.6.x
paragraph).** After each call, every node's value, ring and state, and
every count but the window's own, are those of R's calls so far on a
graph with no window, given in R's order. The exceptions: the bound
channels hold R's values for the newest frame only; a frame already
drawn keeps what the caller read at its call (`late_frames` counts the
frames where R now differs); and `history_skipped` and `time_clamped`,
as before. While a gap is open, its late state calls are at R's end, not
at their place by key (State calls, above); R's order holds for them from
the gap's end.

**Exceptions: refused or counted, and why.**

- A sample older than its device's last stays dropped (`dropped_order`),
  but for a raw mouse's counts (T6). In a batch, the array's order
  decides (L7).
- A report with a stamp later than the poll by more than
  `YSIG_FUTURE_NS` stays a bad stamp (the frame's recovery, as now).
- An input before the last barrier is dropped (`late_barrier`), but a
  raw mouse's count goes into its integrators with no run again ("Per
  device", H1).
- A late call before the window falls back (`late_far`).
- With no window nothing is placed (`late_off`), as with arms.
- `history_skipped` and `time_clamped` stay diagnostics that a run again
  can change by a few (as for arms).

**The reviewer's findings on the third draft (7a17176), and the
answers.** No high finding; every T answer held, and every order
difference on cases with nothing late is now declared.

| Finding | Answer here |
|---|---|
| U1 (medium): at `τ` = `N` a plain disarm cancelled what the ruling promises to make (7 of 6,000) | The cancel is by key: only a disarm whose key is smaller than the arm's group key cancels; at one time the arm comes first and is then ended |
| U2 (medium): a late state call inside a waiting G's span that meets `F(G)` (34 of 6,000) | Exactness condition 4: 2 (b) again for G; added to the calls that go in between G and `L(G)` |
| U3 (low): a plain disarm at `N` makes a lagging device's later reports late | Stated: they run the window again and now reach the detect before the disarm; the reviewer's estimate given; the player uses `ysig_disarm_at()`; the build measures it |
| U4 (low): the oracle's B has no `ysig_disarm_at()` | Spec cases by the reviewer, and a second B, the 0.7.0 header with no window, for that call only; why that path is simple enough to trust |
| U5 (medium): an arm already let in by a closure, then a disarm with a smaller key (2 of 6,000) | The cancel covers every arm of d made before the disarm with a greater group key, waiting or let in; a run again takes it out |

**The reviewer's findings on the second draft (25837ee), and the
answers.** The reviewer compared 0.6.x's R with the time order on cases
with nothing late: 658 of 1,500 differ, most in changes the text
declares (T5).

| Finding | Answer here |
|---|---|
| T1 (high): rule 3's let-in is not an instance of the time order (W7, W8; 47 of 1,500) | A call that lets a group in lets in, right before it, every waiting group that meets it with a smaller key, transitively; groups go in by key |
| T2 (high): "late" by time, the order's ties by "input first" (459 + 412) | Late is defined by the key (time, class, call order): R holds a meeting call with a greater key |
| T3 (medium): a state call at `N(F(c))` before a group already made | A state call goes by the same order; one that must precede a call in R is a late call, with a run again (`late_off` with no window) |
| T4 (medium): a plain disarm at `N(F) ≤ τ` cancels what 0.6.1 made and ended (the two-hand entry-now hit) | `ysig_disarm_at(g, d, t)`: an arm at or before `t` is made, then ended; only an arm later than `t` is cancelled. The player disarms each ended state's controls at the exit time, made or waiting; the 0.6.x rule (only arms ahead of the input) goes, since a disarm at the exit time comes before the next state's arms by call order. A plain `ysig_disarm()` keeps `N` at its call (the coordinator's ruling, 2026-11-15) |
| T5: the declared changes, measured (equal-time input first 459, a state call at `N(F)` 412, pins resolved by moving 9 and 267, a frame or a settle at its onset 1,255 and 845, late feeds 754) | No change: each is declared above. The frame and settle numbers are large because the oracle's generator feeds past a frame's onset before the frame call; the player feeds up to the onset first (`v2_feed()`) |
| T6 (low): which call orders a group's block; one device's reports reordered by stamp | A group's key uses its arm's call order (S3, H16); "Per device": only a raw mouse's counts are placed by stamp against their own device, every other source drops a sample that goes back |
| T7 (low): an input at the onset, fed after the frame call | Class order at equal times: a feed comes before a frame or a settle; such a feed is late and the frame runs again |

**The reviewer's findings on the first draft (d906e19), and the answers (second draft; the counts' names were `input_late_*` there).**

| Finding | Answer here |
|---|---|
| L1 (high): a frame at R's end breaks the place rule; dev2@18 after a frame with onset 16 went after dev1@20 | A frame and a settle go at their time among all calls (time order); the frame's pass reads input at or before its onset; the display's channels at the call are not given again (`input_late_frames`, no stop). The 0.6.x pin through a frame no longer arises |
| L2 (high): the last call the input must follow comes after a call it must precede | R is a time order over meeting pairs; a call that must follow the input moves after it, with the calls that must follow it |
| L3 (medium): an input at `τ` after a waiting G | At equal times a feed comes before a group; a feed at or before `τ` comes before G whatever its call order (R1: a frame or a settle at `τ` goes by call order) |
| L4 (low): condition 2 never applies; "right before" against "at the end of R" | Condition 2 deleted; the time order fixes the wording |
| L5 (medium): condition 3 must retest 2 (a) | Condition 2 (was 3) tests 2 (a) and 2 (b) with the new `L` |
| L6 (medium): a late input that meets only an exception group H | Condition 3: 2 (b) again, with R's order at `L`, for every G whose span holds the input |
| L7 (medium): `ysig_feed_n()`'s order | Array order, as one `ysig_feed()` each; results equal per-event feeding |
| L8 (medium): bound channels given again or not | Given again for the newest frame (as 0.6.x); a drawn frame is not; stated in "What the caller can rely on" |
| L9 (low): at-once frame rows | They count `arm_rows_stale` and stop the guard; final-only's latency is `catchup_ns`; the player is final-only for rows |
| L10 (low): `input_late_barrier`'s exemption against R | Such an input is dropped in R and in the header, so they agree; the exemption stays |
| L11 (low): `N(F)` after a flush; a settle's place; "must follow" | A flush changes no `N(F)`; a settle goes at its `t` as a frame does; the definition says "must precede" |

**Rulings while building (the coordinator, 2026-11-15, on the builder's
questions).**

1. `ysig_disarm_at()` is in no group. It ends the open gap as an input
   does, and keeps its own time `t` (W2). State calls before it in the
   gap are in no group; an arm after it closes a gap that starts after
   it.
2. A frame or a settle with a time before the last barrier goes right
   after the barrier; it is not dropped. The display already used the
   frame, and the reviewer's R does the same. `late_barrier`'s drop
   applies to feeds only, and not to a raw mouse's count (H1, "Per
   device") (ruling 8 extends this to every call's key).
   Such a frame or settle counts nothing of its
   own: it is a late call (every call since the barrier has a greater
   key), placed with a run again, and `late_frames` counts a frame it
   changes, as for any late call.
3. The oldest checkpoint goes only when the next oldest is already
   `catchup_ns` behind the newest, so the window covers at least
   `catchup_ns` of input time. A new checkpoint is taken at a frame when
   the newest is at least `catchup_ns / 8` older (a barrier's always).
   Memory bound: at most `catchup_frames` images (1 to 8), each the size
   given in "Memory (F)", whatever the display's rate. With fewer than 8
   images the window covers less than `catchup_ns` when the frames are
   closer than `catchup_ns / 8`; the player sets `catchup_frames` 8.
4. R's order is the key order, total and canonical, for every reading
   that depends on order: `N(G)` (Exactness), the gap that condition 4
   reads, and `L(G)`. "Up to commuting calls" stays true for the results
   (values, rings, states, counts), but these readings use the key order.
   So `N(G)` is `τ` when an input at `τ` that meets `F(G)` is in R, and
   earlier than `τ` otherwise: every input later than `τ` comes after G
   in the key order, whatever its footprint, and condition 2 applies to
   most past groups. An input at `τ` that does not meet `F(G)` does not
   make `N(G)` reach `τ` (0b4410e; accepted by the coordinator,
   2026-11-15, on the second reviewer's review): it does not let G in, so
   a graph with no window, given R, keeps G waiting as the classes by
   footprint say, and conditions 2 (a) and 2 (b) decide G. Read over
   every footprint, such an input made G exact by condition 2 while that
   graph could not give R. The oracle's B-cannot cases 169, 204, 1915 and
   2600 of seed 20261115 (late generator on) and 237, 1185 and 1425
   (`OFF=l`) were counted by none of the header's tests: a set, an arm or
   a plain disarm that R makes after a waiting G, and that graph before
   it, with an input of another device at G's `τ`. `sig_test`
   `test_v2_n_tau`. (Cases 1094 and 1444 were oracle artifacts, not
   evidence: the reviewer's oracle after be18843 no longer flags them.)
   With this reading, 2 (b)'s exception ("H waits at its place") can
   admit more groups H: an H whose only input at its time is of another
   device now waits at its place. This is sound: the graph with no window
   keeps H waiting by footprint too, so it makes H at `L(G)` as R does.
   The input at `τ` must come before G in R: one logged after G (a frame
   or a settle at `τ`, G's `L` by R1) does not make `N(G)` reach `τ`
   (the second reviewer's `c2_frame`: at a cancel by key, 2 (c) went
   uncounted; `sig_test` `test_v2_c2_cancel`). A graph with no window
   reads `N(G)` the same way, from its nodes' input times (it keeps no
   log; an input at `τ` that a later input of the same node hides reads
   as none, so G is counted, never missed): before, it read the newest
   input of every device and made G exact by 2 where the text tests 2 (a)
   and 2 (b) (the reviewer's `nowin.c`; `sig_test` `test_v2_n_tau` with
   a zeroed window).
5. (R1, R2; the coordinator's rulings of 2026-11-15, from the reviewer's
   analysis.) At one time a feed comes first; frames, settles, state
   calls and groups then go by call order, as in 0.6.x. A frame or a
   settle lets in the waiting groups at or before its time, and is
   `L(G)` for a group at its time. A `ysig_disarm_at` with `t` at or
   after `τ` that lets G in is G's `L` for 2 (a); conditions 1, 2 (b),
   2 (c) and 4 still apply (this replaces the builder's exception at a
   group's going-in, d38539c). Condition 4's gap ends at a
   `ysig_disarm_at` too, and a late disarm placed in a group's gap counts
   the group.
6. (The coordinator, 2026-11-15.) A disarm's gap ends at the next input
   call or barrier in call order ("Groups" defines gaps in call order;
   ruling 1 adds `ysig_disarm_at()`), even when R places that input before
   the disarm. Until then the disarm has not taken effect, and a read sees
   its detect still armed. The same reading in the other direction (S2,
   the coordinator's ruling of 2026-11-15): a call made before the disarm
   does not end its gap, even when R places it after the disarm (a
   `ysig_disarm_at()` with a later `t`); only an input, a barrier, an arm
   or a `ysig_disarm_at()` made after it does. The reviewer's cases
   379, 477, 557, 438, 462 and 690 at 5feeb5b end with such a disarm: the
   header keeps the hit until an input ends the gap, then counts it
   (`arm_hits_superseded`, a late state call).
7. (S1', the coordinator's ruling of 2026-11-15, from the reviewer; S1
   of the same day is withdrawn.) A state call of a present or an ahead
   group lets in at `N(F(c))` at its call; a past group's state calls at
   its `τ`; a state call in no group at `N(F(c))`; a plain disarm in no
   group at `N`. An open gap's calls let in at their call until the arm
   closes the gap; for a past group then at `τ`, and the run again
   corrects them ("Rule 3's let-in time for a state call" above).
8. (Ruling 2 extended, the coordinator, 2026-11-15, from the reviewer's
   case 350 of seed 20261115.) Any call whose time is before the last
   barrier takes the barrier's time as its key time; call order decides
   among them. This covers state calls, groups and `ysig_disarm_at()`,
   not frames and settles alone. The clamp applies to three readings:
   the key; the cancel by key (a disarm's time against an arm's `τ`);
   and the let-in tests (a waiting group's `τ` against the let-in time,
   and the bound of S1' (b)). What a call does at its time is unchanged:
   an arm tests its input at its `τ`, a `ysig_disarm_at()` ends its arm
   at its `t`. The case: after a reset at 19.254 ms, arms at 12.873 ms,
   then `ysig_disarm_at()` of the same detect at 3.828 ms. Unclamped, the
   disarm's key (3.828) is smaller than the arm's (12.873) and cancels
   it, voiding a hit already handed out; clamped, both key at 19.254 and
   the arm, made first, is the smaller: it goes in, and the disarm ends
   it. The header clamps with the last barrier's time (`ysig__kt()`):
   every call in its log and every waiting arm is after that barrier.

Measured effect of ruling 4 (the reviewer's oracle at 99addca, mode 4
with A's checks, seed 20261115, 3,000 cases, the Linux desktop):
`arm_unverified` in right cases rose from about 2.7% of made groups
(`N(G)` read in the header's log order) to about 7% (key order), and the
B-cannot cases other than reasons 6 and 8 are all counted.

Measured effect of ruling 4 by footprint (the oracle at ctest's
switches, seed 20261115, 3,000 cases, the Linux desktop, two oracle runs
at once): B-cannot counted 778 of 784 before and 784 of 784 after with
the late generator on, 789 of 792 and 792 of 792 with it off (`OFF=l`);
wrong cases with no count 0 before and after. `arm_unverified` in right
cases: 2,471 to 2,675 counts over 30,783 made groups (8.0% to 8.7%), and
3,383 to 3,706 over 40,691 (8.3% to 9.1%) with `OFF=l` (0b4410e). With
2 (b)'s exception in the L5 retest and 2 (c) at a cancel by key too
(40e34a9; ctest, `-j 6`, load about 2): 2,693 and 3,728. Seeds 20261121
and 20261122, 1,000 cases each, late generator on and `OFF=l`: B-cannot
counted 246 of 246, 258 of 258, 250 of 251 and 263 of 266 at de9a98a,
and all of them (246, 258, 251, 266) at 40e34a9; 0 wrong with no count;
right-case counts 810, 1,290, 951, 1,120 before, 886, 1,401, 1,039,
1,195 after (made groups 10,467, 14,692, 11,146, 13,942).

**Open questions for this text.**

1. The default `catchup_ns` until a rig's devices have a measured input
   lateness.

(The note for format 3.11 rule 4 is DECIDED, 2026-11-15; its checks are
above, under "The batch".)

#### Where 0.6.0 before this section differed

1. No groups: a set, set_data, disarm or recenter made before an arm
   always comes before it, so an arm after one of them made after input
   later than `t` stays after that input, with nothing counted (A, r11).
2. Every input at or before `t` restricts the place, whatever its
   footprint (A, the late report from another device).
3. A state call in no group restricts the place whatever its time.
4. No `arm_past_pinned`.
5. An ahead arm is made at its call, and meets the earlier arm's hold
   only up to `N` (E, r15).
6. An arm's fall and rise reach latches, peaks and stateless nodes only
   (D, r14; the named exception).
7. Each image reserves the queue's full room (F, r16).
8. `N` after a barrier is the lowest time, not the barrier's time, so an
   arm at the reset's time with no input yet is ahead, made at once.
9. No waiting arms, so nothing to cancel (`arm_ahead_cancelled` is new).
10. Rows and derived events are handed out at once only; no
    `rows_final`, `derived_final`, `catchup_rows` or `ysig_flush()`.

### Trials and adapters

- `ysig_reset_trial()` skips the nodes of every adapter, so their order
  slots, integrators, filters and rings keep their state; the hit and
  derived-event queues empty, as at `ysig_reset()`. Each node that reads
  an adapter starts from its current value. A graph with no adapters
  resets every node.
- `ysig_recenter()`: see [Keep the hand across trials](#keep-the-hand-across-trials).
  The point is in the role's units, at the call's time, no later than the
  input already fed. Both calls are journaled.

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
  a block change the held cursor kept the old block's P until the next
  report, while `c` is computed at the onset with the new P. From v0.4.0
  `ysig_set()` recomputes a stateless node and its stateless users at
  once (G17), so the held cursor has the new P too; the program still
  reads `c`, and its files do not change.
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
- **`ysig_set()` recomputes stateless nodes (v0.4.0, G17).** A trial's
  parameters are set between trials, and a still hand sends nothing, so
  the held value of an `affine` kept the old block's map until the next
  report: a frame test (`ready`, `inside`) or a bind read the old map at
  a block's first frames (experiment_format.md 3.18, the last finding of
  `trial_bimanual`). The set node and the stateless nodes after it now
  recompute from their inputs' held values at the call, in node order.
  A stateful node (integrate, a filter, velocity, peak, latch, a detect)
  keeps its value: its value comes from its state, and its next input
  updates it. Replay runs the journaled set, so it agrees. v0.4.1 (the
  review): the recompute changes the lanes only, since no sample came (a
  sum's `t` had moved to its newest input's); and a node with a ring gets
  an entry at the set's time, so `ysig_value_at()` and `ysig_value_of()`
  agree after it. The set has no time argument and the graph no clock, so
  its time is the newest the graph knows: the later of the newest event
  fed and the last frame's onset. A read at or after it gives the new
  value, one before it the old: a region hit's value read at the hit
  (format 3.15) after a block change is the cursor the detect tested. A
  node computed from its inputs at a past time (two sources, a frame
  input) has no ring of its own and uses its parameters now at any time.
- **Entry `now` (v0.3.0, G16).** The crossing rule holds a test that is
  true at the arm until it fails. That is right for a response (a hand
  already past the onset is no movement), but a state's exit on "in the
  target" must fire when the cursor is there when the state starts
  (experiment_format.md D35 (f): `trial_bimanual` starts tracking on the
  first frame that has the cursor in the target, also when it was there
  before the wait). The hit's time is the arm's: nothing happened later,
  and the arm is the state's entry time. A frame-rate detect gives every
  hit at an onset, so armed between frames it tests at the next one, as
  its other hits do. A rise of the output at the arm comes after any
  fall, so a latch or a peak gate sees a rise. With a hold, `now` is
  `arm`: a hold already counts from the arm, and a hit at the arm would
  skip the hold. v0.4.1 (the review): a reset that arms and hits raises
  the output after every node's reset, so a latch freezes and a peak's
  gate restarts as at `ysig_arm()` (in v0.3.0 the latch reset saw the
  output already 1, no rise). An input with no sample since its reset
  (tier UNKNOWN: zeros, no hand) gives no hit at the arm; the detect is
  ready, and its first value is tested as a crossing, so the hit is at
  the first sample that meets the test (D35 (f)'s "already true" needs a
  value to be true of).

### An arm in the past (v0.6.0)

The player arms a state's controls at the state's entry time. That time
is often before input the graph already holds: a hit that a frame's pass
found is before the frame's onset, and a response's end is before the
samples of its frame. The experiment format's 3.11 rules 2 and 4 then
need the result of an arm made at its time, in order with the input.

0.5.0 caught up on the detect's input ring. A review found it right only
for one source with rising times and a ring that reaches back: it read a
two-source node's held value as if no input came after the arm, lost
equal-time samples (the ring keeps the last), tested ring values that a
view input or a `ysig_set()` never showed the live detect, and collapsed
the detect's output to one rise at the call, so a latch, a peak's gate or
a mux saw the wrong values. 0.6.0 replaces it.

**The guarantee.** An arm at time t gives what the same calls give with
the arm made at its place in time order; input stays in call order (a
source defines out-of-order input by call order: drops, per-device order,
ring skips). The place is after every earlier call that must come first:

- input at or before t: a sample (a late report from another device
  too), a frame's onset, a `ysig_settle()`;
- `ysig_set()`, `ysig_set_data()`, `ysig_disarm()` and a reset, made
  before it (they have no time: call order);
- a `ysig_recenter()` or a `ysig_arm()` at or before t.

Input, recenters and arms later than t go after it. For time-ordered
input that is the calls sorted by (time, call order).

At its place an arm first meets its detect's hold up to t (any detect:
one on a view or at frame rate too): that hold ended under the earlier
arm, so it is that arm's hit. An arm made after input later than t
knows input up to t is in, so it meets the hold up to t wherever the run
again puts it; that keeps a hit the caller took at or before t (found by
a later call) and starts the new hold at t. An arm ahead of the newest
input (t later than it) meets the hold only up to that input, not on
unfed time; input fed later with a time before t comes after it.

A hit output's rise is at the hit's time, with the sources of the input
that met the test (the hit's value), however the hit was found (a
sample, a frame, a settle, an arm). A later evaluation of a standing hit
is at its own time, with its input's sources, as any value: a hold on
the output then completes from samples, and the ring takes each entry
(the fix round had kept the hit's time on every evaluation; the second
review's r13b and r13, now `test_hit_out_time`).

What the guarantee does not cover, counted by name:

- a hit handed out that the new order does not give: the armed detect's
  own later than the arm's time in `arm_hits_superseded` (its earlier
  arm's: a caller re-arming at t took none; the player drops them by an
  arm count); any other in `arm_hits_retracted`, another detect's at any
  time too (the caller may have drained it with the hit it acted on and
  cannot tell a voided one from one that stands; the player stops on
  it; the second review's r12, now `test_arm_review2`). What the armed detect finds from the new arm on is handed
  out again, also when its earlier arm handed out an equal hit: a caller
  that keeps arms apart needs it (the reference model's s21418);
- rows and derived events that `on_row` and `ysig_derived()` already had:
  a change in any column is counted (`arm_rows_stale`,
  `arm_derived_stale`); the run again's lost derived events are not;
- an arm whose place is before the window, or after its logs ran full
  (below), and a graph with no window;
- a detect on another detect's hit output does not see a rise and a fall
  that an arm makes at one time (an arm's change reaches latches, peaks
  and pure nodes, not detects). Accepted for now: the player builds no
  such graph, and the hit comes out retracted (counted in
  `arm_hits_retracted`), so the player's guard stops;
- `ysig_arm()` from a callback (`on_row`, the journal, a custom op)
  during another call: refused, counted in `arm_reentry`. Every other
  call that changes the graph (`ysig_feed()`, `ysig_frame()`,
  `ysig_set()`, `ysig_set_data()`, `ysig_recenter()`, `ysig_reset()`,
  `ysig_reset_trial()`, `ysig_settle()`, `ysig_disarm()`,
  `ysig_replay()`): the same, in `call_reentry`. Such a call would run
  inside the other call but be logged and journaled after it, so a run
  again and a replay would run it in another order. Reads
  (`ysig_value_of()`, `ysig_value_at()`) and the queues
  (`ysig_detected()`, `ysig_derived()`) stay allowed;
- a custom op with effects outside its state runs them again.

**How.** With `desc.catchup_frames` above 0 the graph keeps a window:

- a checkpoint of its state after each frame and each reset, the last
  `catchup_frames` of them (the handle, the nodes and their parameters,
  values, states, data, and both queues; a ring's slots are not copied);
- every call since the oldest checkpoint (`catchup_calls`), with a
  frame's channel values copied, as the caller may change them (so
  `ysig_set()` of a channel node to a channel above those its channel
  nodes read at the open is refused, with or without a window);
- the old value of every ring slot those calls wrote (`catchup_writes`).

An arm in the past goes into the call log at its place. The graph goes
back to the newest checkpoint at or before it, puts back the ring slots
written since, and runs the logged calls again: the same code on the same
calls, so no op needs a rule of its own. A run again can queue up to
`catchup_calls` hits beyond the queue's 64 (the caller drains after the
arm). A reset is a barrier: an arm after it is never placed before it.

**Where it falls back.** An arm before the oldest checkpoint, or after the
logs ran full, tests the newest value as 0.4.1 did, counted in
`arm_past_far` and `arm_past_full`. A zeroed desc has no window (the
examples' fixed arenas stay as they were): such arms are counted in
`arm_past_off`.

**Cost, and the knob.** An arm in the past costs the calls since the
checkpoint it goes back to, run again: at most `catchup_calls` calls. A
frame adds a copy of the state. Measured on the Linux desktop (i5-14500,
GCC 12.2, `-O2`, pinned to CPU 2, 1-minute load 0.45, 2026-11-12), a
graph of 27 nodes (a pen with a 4,096 ring, an affine cursor, a speed,
8 detects each with a latch and a peak, every node with a 4,096 ring),
window of 4 frames:

| Input | `ysig_frame()` median, no window / window | One arm half a frame back | One arm 2.5 frames back | 8 arms half a frame back |
|---|---|---|---|---|
| 1 device, 17 samples a frame (1 kHz) | 0.2 / 0.6 us | 8.7 us | 25.1 us | 69.6 us |
| 4 devices, 133 samples a frame each (8 kHz) | 0.2 / 1.0 us | 202.7 us | 612.3 us | 1,621 us |

The bound per frame is the arms in the past a frame makes times the calls
in their windows. The player arms a few controls a frame at 1 kHz
(about 10 us each); with four 8 kHz mice, 8 arms half a frame back take
1.6 ms of a 16.7 ms frame. Fewer `catchup_frames` or `catchup_calls`
lower the bound; an arm beyond them falls back by name. The arena grows
by the checkpoints (about 30 KB each for 27 nodes), the calls (about 200
bytes each) and the ring writes (72 bytes each).

**Checked.** `test_arm_window`, `test_arm_review` (one case per finding
of the third review), and two randomized differentials:

- `tests/adapt/sig_arm_oracle.c`, the time-order oracle: B builds the
  guarantee's order from the calls and A's hand-outs alone (its rule is
  in prose at the top of the file) on a graph with no window. B makes
  one internal call, `ysig__settle_det()`, for an arm A made in the past:
  the arm's own hold completion, which no public call gives alone. After
  every call from A's first arm in the past on at which both have run
  the same calls, and at the end, A must give B's values, rings, op
  state, stats (but the digest and the window's counts) and bound
  channels. At the end, also B's hits (extras counted, a hit a
  time-order caller could have taken as retracted), rows and derived
  events (in every column, or counted stale). Shapes: several detects
  (one on `all()` of detects, one on a latch of a hit, views, frame
  rate), emit, records, a channel node and its `ysig_set()`, a bind,
  set, disarm and recenter before an arm, arms ahead of the input, late
  reports from a second device, equal-time samples, `ysig_reset()` and
  `ysig_reset_trial()`, bursts of arms, random drains, small windows.
  Frozen before the fix (2113774); each later change is its own commit
  with its reason. The oracle as of 550dd2b on 0.6.0 as reviewed
  (c7dc717): 3,408 of 3,592 compared cases wrong (5,000 cases, seed 7,
  GCC 12.2 `-O2`).
- `tests/adapt/sig_arm_diff.c`, the first differential (one detect, the
  arm moved by the rule), retired from ctest (below); the file stays as a
  diagnostic.

Results after the decision on fix 1 (2026-11-13, the Linux desktop,
GCC 12.2 with ASan and UBSan, `setarch -R`, 8 processes, nothing else
running):

| Run | Cases | Compared | Fell back | Wrong |
|---|---|---|---|---|
| Oracle, seeds 97001 to 97008, 7 min 38 s | 1,040,000 | 854,359 | 185,641 | 0 |
| Oracle compared after every call, with `ysig_reset_trial()` (550dd2b), seeds 99001 to 99008, 9 min 22 s | 1,040,000 | 854,938 | 185,062 | 0 |
| First differential, seeds 98001 to 98008, 19 min 20 s | 1,200,000 | 1,197,037 | 2,963 | 3,038 |

No sanitizer report in either. The first differential's 3,038 come from
its B, not from the window. That B moves only the case's last arm to its
place; an earlier arm that the window placed in the past stays at its
call position in B, so B is not in time order. The diagnostic
`SIG_ARM_CLASSIFY=1` runs each wrong case again against the oracle's
order (every arm placed by the rule, the hold met up to t for an arm
made in the past), and compares values, rings, op state and stats after
every call at which both have run the same calls, and at the end. Seeds
98001 to 98003, 40,000 cases each: 102, 107 and 111 wrong (62 of the
first seed's 102 a ring's length, 21 op state, 17 stats, 1 a value, 1 a
ring entry), 25 of the 320 on a detect on a view. A agrees with the
oracle's order in all 320. `test_sig_arm_diff` is therefore retired from
ctest; the oracle covers its shapes and compares after every call too.
`sig_fuzz` (10,000,000 inputs, seeds 8501 to 8508,
39 s): no report, every replay equal.

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
| P-core, v0.1.0 again (2026-11-09) | 142.5 ns, 141.4 ns, 177.7 ns | 279 ns, 612 ns, 2.32 us | 105 ns, 178 ns | 75.7 us |
| P-core, v0.2.0 (2026-11-09) | 145.4 ns, 144.4 ns, 179.0 ns | 287 ns, 492 ns, 2.70 us | 107 ns, 170 ns | 77.4 us |

v0.2.0 costs about 3 ns a sample (2%) and 8 to 15 ns a frame over v0.1.0 on
the same graph, in two runs each (144.4 and 144.5 ns against 141.4 and 141.2 ns).
The bench's digest is the same for both.
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

The release gates of 0.7.0 (V2SIG10, 2026-11-16, Linux desktop,
i5-14500, GCC 12.2): every pick among waiting groups by key, condition 4
where the log lost the gap. The oracle (the reviewer's rev2e copy) runs
with ctest's switches (late mode 4 with A's checks, R1, the tie, the
disarm_at and exit shapes, the horizons) and `SIG_ORACLE_PRED_TARGET=1`,
3,000 cases a seed, with the late generator on and off (`OFF=l`), four
runs at a time (load average 2.6 to 3.5):

| Seed | Generator | B cannot run (counted) | Wrong (by `arm_unverified`; by another count; none) | Right cases with a count (counts over made groups) | Time |
|---|---|---|---|---|---|
| 20261115 | on | 747 (747) | 1,398 (1,000; 398; 0) | 709 of 855 (2,799 of 31,205) | 64 s |
| 20261115 | off | 754 (754) | 1,059 (784; 275; 0) | 1,013 of 1,187 (4,063 of 43,438) | 55 s |
| 20261123 | on | 724 (724) | 1,429 (1,062; 367; 0) | 725 of 847 (2,901 of 31,024) | 80 s |
| 20261123 | off | 701 (701) | 1,078 (806; 272; 0) | 1,049 of 1,221 (4,177 of 45,887) | 62 s |
| 20261124 | on | 757 (757) | 1,358 (1,029; 329; 0) | 738 of 885 (2,947 of 32,435) | 67 s |
| 20261124 | off | 698 (698) | 1,137 (856; 281; 0) | 979 of 1,165 (4,144 of 43,673) | 60 s |

Both targets hold on every run, and no made group that the predicate
flags by condition 1 or 4 is without a count. Against V2SIG9's table as
many cases are wrong and B cannot run as many; 31 to 60 wrong cases a
run move from another count to `arm_unverified`, and right cases get 11
to 46 more counts (the arm's reason-4 tests by key, and condition 4 from
the disarms the log lost).

With `SIG_ORACLE_PAST_GROUPS=1` (past groups as often as the others) and
`PRED_TARGET`, the same switches, the same seeds:

| Seed | Generator | B cannot run (counted) | Wrong (by `arm_unverified`; by another count; none) | Right cases with a count (counts over made groups) |
|---|---|---|---|---|
| 20261115 | on | 966 (966) | 1,301 (970; 331; 0) | 634 of 733 (2,802 of 25,575) |
| 20261115 | off | 981 (981) | 1,061 (835; 226; 0) | 829 of 958 (3,713 of 33,759) |
| 20261123 | on | 992 (992) | 1,240 (940; 300; 0) | 665 of 768 (2,883 of 25,616) |
| 20261123 | off | 973 (973) | 1,066 (841; 225; 0) | 837 of 961 (3,767 of 34,829) |
| 20261124 | on | 996 (996) | 1,281 (969; 312; 0) | 642 of 723 (2,755 of 25,251) |
| 20261124 | off | 968 (967) | 1,035 (815; 220; 0) | 880 of 997 (3,739 of 34,767) |

Seed 20261125 (both modes) holds too. The one B-cannot case not
counted, 1318 of 20261124 `OFF=l` (reason 9), is an oracle artifact:
the oracle tests reason 9 on an R in which a disarm whose gap is still
open sits in no group at `N` (17.000 ms) before group 0's arm (20.647
ms); the next call's arm closes that gap into its own past group at
3.000 ms, which is in no gap of group 0. By the text a disarm goes into
R at the end of its gap. The reviewer's switch for this shape
(`SIG_ORACLE_R9PEND_CASE`, case 1444, "not for ctest") gives 967 of 967
with 1318 and 943 of 943 with every case. ctest runs this mode once
(`test_sig_arm_ref_past`: seed 20261115, late generator on, 1,000
cases), reading the oracle's lines for all three targets.

The V2SIG8 round's table (load average 6.9 to 7.4, other builders on
the machine) had the same numbers but for the one case above. Against
V2SIG7's table: the same cases
are wrong and B cannot run the same ones; a few wrong cases move from
`arm_unverified` to another count, and 9 to 19 fewer counts in right
cases, from condition 4 at a past group's place, which no longer counts
a disarm at the group's own time (the text's condition 4). Case 2005 of
seed 20261124 with `OFF=l` (V2SIG7's one bare case) is now read at the
late call that made its row wrong (the reviewer's rev2d): counted
(`late_full`). Diagnosed in V2SIG7: its window was empty (the 64-call
log ran full at call 70), a row of `all()` was final at once at the arm
(call 76), and call 77, device 2 at the row's time, is late by key and
counted `late_full` there.

The oracle's feed_n mode (`SIG_ORACLE_FEEDN=1`, the same switches,
1,000 cases, the three seeds, both modes, six at once, 25 to 33 s
each): no batch that did not fit, and 14 cases with a first difference,
each a count, batched lower (`late_hits_voided` 8, `late_frames` 6), as
in V2SIG7 and V2SIG8.

The V2SIG10 round's other gates (one at a time, load average 1.3 to
4.7):

- ctest Release 128 of 128, 69 s at `-j 6`. Sanitizers tree under
  `setarch -R`: build 36 s at `-j 6` (incremental), ctest 127 of 127,
  510 s at `-j 4`, no sanitizer report. `sig_test` as C99 and as
  C++17, at `-O2` and `-O3` (`-Wall -Wextra -Werror`): passed.
- `sig_fuzz` (`YSIG_DEBUG_FEED_BOUND` on): 10,000,000 inputs under ASan
  and UBSan (`setarch -R`, 4 processes, seeds 10001 to 10004), 78 s: no
  abort, no report. 1,495,391 graphs opened, 4,503,339 late events,
  1,460,529 batches, 731,199 disarm_at calls, 11,678 arms run again;
  3,218,622 quiet feeds checked, the largest use 1.000 of the need in
  each log (at most 14 undo entries in one feed).
- The oracle's feed_n mode (`SIG_ORACLE_FEEDN=1`, 1,000 cases, the three
  seeds, both modes, six at once, 25 to 30 s each): no batch that did
  not fit, 14 cases with a first difference, each a count, batched
  lower (`late_hits_voided` 8, `late_frames` 6); the same as V2SIG9.
- `player_test` 78 of 78 at 60 Hz (13 s) and at 500 Hz (18 s).
- The model's harness (`--no-shrink --jobs 6`), 1,000 cases a run,
  60 Hz seeds 102001 to 103000, 500 Hz seeds 100001 to 101000:
  mismatches 6 at 60 Hz and 4 at 500 Hz with no lateness, all at a GAP;
  0 at 10 ms at both rates. No guard stop. The same as V2SIG9.
- Mutants: sig.toml 358 mutants (380 to 387 new); the whole list at
  `-j 4`, 687 s (load average 0.4 to 3.6): 357 of 358 as expected. The
  one off, 379 (T1's closure by raw time), survived because
  `test_v2_closure_key`'s counts no longer tell it (the arm's reason-4
  test counts G there too); `test_v2_closure_key_arm` kills it. With it,
  and 285 and 300 killed by `test_v2_h9_first` and
  `test_v2_inside_waiting`: killed 346, survived 12 (00, 189, 192, 215,
  237, 287, 291, 296, 313, 316, 326, 361), all as expected
  (`--only` 00, 285, 300, 379 to 385: 10 of 10, 24 s).

The V2SIG9 round's other gates (one at a time, load average 0.7 to 4.8):

- ctest Release 127 of 127, 68 s at `-j 6`. Sanitizers tree under
  `setarch -R`: build 36 s at `-j 6` (incremental), ctest 126 of 126,
  383 s at `-j 4`, no sanitizer report. `sig_test` as C99 and as
  C++17, at `-O2` and `-O3` (`-Wall -Wextra -Werror`): passed.
- `sig_fuzz` (`YSIG_DEBUG_FEED_BOUND` on): 10,000,000 inputs under ASan
  and UBSan (`setarch -R`, 4 processes, seeds 9001 to 9004), 70 s: no
  abort, no report. 1,494,568 graphs opened, 4,495,188 late events,
  1,455,285 batches, 729,532 disarm_at calls, 12,026 arms run again;
  3,211,947 quiet feeds checked, the largest use 1.000 of the need in
  each log (at most 11 undo entries in one feed).
- `player_test` 78 of 78 at 60 Hz (13 s) and at 500 Hz (19 s).
- The model's harness (`--no-shrink --jobs 6`), 1,000 cases a run,
  60 Hz seeds 102001 to 103000, 500 Hz seeds 100001 to 101000:
  mismatches 6 at 60 Hz and 4 at 500 Hz with no lateness, all at a GAP;
  0 at 10 ms at both rates. No guard stop. The same as the player's
  0.12.0 gates.
- Mutants: sig.toml 350 mutants; the whole list at `-j 4`, 774 s (load
  average 0.8 at the start, 2.4 at the end), then the changed notes
  with `--only` (192 s): all as expected, killed 336, survived 14. New
  survivors: 285 (since condition 4 at a cancel by key, the oracle
  counts its B-cannot cases) and 287 (equivalent with the closure by
  key); 300's equivalence argument is withdrawn (the mutant drops one
  count in a right case at seed 20261115 with `PAST_GROUPS`). Their
  notes say why; 285 and 300 were open (V2SIG10 kills both).

The V2SIG8 round (2026-11-15, Linux desktop, i5-14500, GCC 12.2, one
gate at a time, load average 2.4 to 7.8, with other builders on the
machine):

- `ysig_feed_n()`'s bound on one feed's log use ("The batch") is
  checked: `sig_fuzz` sets `YSIG_DEBUG_FEED_BOUND`, and each quiet feed
  of a batch aborts if it uses a log past its need. 10,000,000 inputs
  under ASan and UBSan (`setarch -R`, 4 processes, seeds 8901 to 8904),
  93 s: no abort, no report. 3,227,249 feeds checked; the largest use
  is 1.000 of the need in each of the six logs (the bound holds and is
  reached; at most 14 undo entries in one feed). 1,498,018 graphs
  opened, 4,516,417 late events, 1,462,512 batches, 732,735 disarm_at
  calls, 12,062 arms run again. Controls: the need of one log set to 0
  (calls, hits, derived, row buffer), or the undo need halved, aborts
  within 2,000,000 inputs. The derived buffer's control was not run.
- The nine open mutants: 287, 297, 314, 315, 318 and 356 are killed by
  new `sig_test` cases, each with a control (`test_v2_h12_before`,
  `test_v2_c4_past`, `test_v2_c1_c4_exact`, `test_v2_n_tau_image`); 300,
  313 and 326 are equivalent (their notes say why). The reviewer's
  `SIG_ORACLE_PRED_TARGET=1` (a made group that the text's predicate
  flags by condition 1 or 4 must have a count) tells none of 300, 314,
  315 or 318 apart at seeds 20261115, 20261123 and 20261125 (2,000 to
  3,000 cases, both modes, also with `SIG_ORACLE_PAST_GROUPS=1`): the
  conditions those count are counted by condition 2 too unless
  `N(G) = τ`, which the oracle's generator rarely makes. It is in
  sig.toml as `sig_ref_pt` (seed 20261115, late on, 3,000 cases). The
  unmutated header failed it at seed 20261124 late on, once in 3,000
  cases (case 2107: a group flagged by condition 4 at a `disarm_at` at
  0.000 after the reset). V2SIG9 found the cause, a text gap: condition 4
  was not tested again at a cancel by key ("When the header tests").
- Mutants: sig.toml 347 of 347 as expected (killed 335, survived 12:
  the control 00 and 11 that their notes expect to survive, 300, 313
  and 326 among them), 807 s, 4 jobs (load average 5.9 at
  the start, 2.9 at the end).
- ctest Release 127 of 127, 79 s at `-j 4`. `sig_test` as C99 and as
  C++17, at `-O2` and `-O3` (`-Wall -Wextra -Werror`): passed.
- Sanitizers: the `sanitizers` tree (layout, pack and player on),
  configured and built under `setarch -R` (204 s at `-j 6`, from
  clean), ctest 126 of 126, 467 s at `-j 4`, no sanitizer report (load
  average 2.2 at the start, 2.1 at the end).

The V2SIG7 round (2026-11-15, Linux desktop, i5-14500, GCC 12.2, one
gate at a time, load average 1.1 to 4.4): `ysig_feed_n()`'s early run
again (format 3.11 rule 4, "The batch"), "N(G) earlier than tau" as a
flag, condition 3's table removed.

- The oracle at ctest's switches, the six runs above at once (78 s):
  every number in the table is the same.
- The oracle's feed_n mode (`test_sig_arm_ref_feed_n`, new in ctest):
  see "The batch".
- `sig_fuzz` arms at `INT64_MIN` about one arm in 24 (the 501fea5
  header gives a UBSan report in under 1 s). 10,200,000 inputs under
  ASan and UBSan (`setarch -R`, 6 processes, seeds 8701 to 8706), 75 s:
  no report. 1,525,804 graphs opened, 4,601,481 late events, 1,490,905
  batches, 748,060 disarm_at calls, 12,175 arms run again.
- ctest Release 127 of 127, 71 s at `-j 6`. The sanitizers tree under
  `setarch -R`: build 30 s at `-j 8` (incremental), ctest 126 of 126,
  415 s at `-j 6`, no sanitizer report. `player_test` 72 cases at 60 Hz
  and 72 at 500 Hz, no failure.
- New `sig_test` cases, each with a control: the batch that fills the
  window after a late event, the state-call run again then a late
  input in one batch, `test_arm_int64_min` (at the barrier's time, with
  and without an input at tau).

- Fuzz: `sig_fuzz` now feeds late events (1, 5 and 20 ms), batches
  through `ysig_feed_n()`, `ysig_disarm_at()` before and after the newest
  call, and reads with a limit, among resets, flushes, frames and
  settles. 10,200,000 inputs under ASan and UBSan (`setarch -R`, 6
  processes, seeds 8601 to 8606), 74 s: no report. 1,528,341 graphs
  opened, 4,594,718 late events, 1,488,668 batches, 744,395 disarm_at
  calls, 12,109 arms run again. The bind check after a frame now runs
  only when the frame is at R's end: a late frame's channels hold R's
  values at its place.
- A defect the gates found and fixed: the checkpoint spacing by input
  time (`ysig__ck_take()`) subtracted times with no saturation; an input
  at `INT64_MAX` after a negative one overflowed (UBSan, the fuzz's seed
  3). It now uses `ysig__sub()` ("Times saturate at the ends of
  int64"); `sig_test` `test_v2_time_ends`, with a control.
- Sanitizers: the `sanitizers` tree, built and run under `setarch -R`
  (build 42 s at `-j 8`, incremental): ctest 125 of 125, 456 s at
  `-j 6`, no sanitizer report; the oracle there 401 s (generator on)
  and 362 s (off). ctest Release 126 of 126, 80 s at `-j 6` (the two
  ran at the same time; load average 2.3 to 5.0).
- Compile checks: C11 and C++17 with the 0.7.0 calls in the API table
  (`ysig_feed_n()`, `ysig_disarm_at()`, `ysig_detected_to()`,
  `ysig_derived_to()`), in the Linux, sanitizers and `nothreads-cxx`
  trees.
- Mutants (V2SIG7): sig.toml 338 of 347 as expected (killed 329,
  survived 18; 679 s, 4 jobs, load average 2.7 at the start, 2.8 at the
  end). 312 went with condition 3's table; 283 and 285 are killed by a
  new test, `sig_ref_23off` (the oracle at seed 20261123 with `OFF=l`,
  3,000 cases: 283 leaves case 493 wrong with no count, 285 one case B
  cannot run uncounted); 361 is equivalent (its note says why). Open:
  9 survive every test (287, 297, 300, 313 to 315, 318, 326, 356);
  none is killed by the oracle at seeds 20261123 and 20261124, on and
  off. Before V2SIG7: 335 of 348 as expected (killed 327, survived 21;
  628 s, 4 jobs, load average 0.3 at the start, 2.7 at the end). 47
  anchors had gone stale with the 0.7.0 code: 35 follow the code; 12
  are removed with their code: 246 and 270 (no `derived_lost` restore,
  no checkpoint skip at 64 hits), 261 (the log no longer pins a group),
  298 and 299 (`ysig__ux_input()` went: a late report goes to its
  place), 324, 325 and 327 to 331 (pending groups went). 340 to 376 are
  new: one or more for each 0.7.0 rule, and 376 for the overflow fix.
  The oracle test of the list runs ctest's 0.7.0 switches (seed
  20261115); `sig_spec` and `sig_disarm_at` are new tests (the
  reviewer's cases). New `sig_test` cases, each with a control:
  `test_late_no_window` (a late report reaches the detect's
  before-the-arm branch only with no window; mutants 42, 88, 293),
  `test_v2_l_at_tau` (345, 348, 358), `test_v2_n_tau_nowin` (355),
  `test_v2_s1b_nowin` (351), H8's barrier 3 (280: the queue of 64 +
  `catchup_calls` held all 240 events), the next gap of
  `test_v2_gap_late` (365), the ring after a `ysig_feed_n()` batch (371).
  The oracle kills 240, 350 and 363. 291 and 296 are equivalent (their
  notes say why). Then 13 survived every test; V2SIG7 left 9 (above).
  Most are 0.6.x's counts (conditions 1, 4 and 2 (c)) and gap mechanics
  (H12, 1650) that 0.7.0's classes by footprint and its other count
  sites now reach first.

The reviewer's oracle (2026-11-14, final round). `tests/adapt/sig_arm_ref.c`
and `sig_arm_spec_cases.c` are by an independent reviewer, written from
"An arm in the past: the semantics" and the graph's other rules, without
the header's internals; their headers keep the change notes. A is the
graph with a window, called as a caller calls it; B is the same graph with
no window, given R through the public API. ctest runs the spec cases in
full (`test_sig_arm_spec`) and the oracle at 3,000 cases, seed 20261114
(`test_sig_arm_ref`), passing on the two targets of "Exactness": every
case B cannot run is counted in `arm_unverified`, and every wrong case has
a count. Results (Linux desktop, i5-14500, GCC 12.2; the 20,000 under ASan
and UBSan, `setarch -R`, 7 min 39 s, load average 2.9 at the start and 1.3 at the end):

| Run | Cases | B cannot run (counted) | Wrong (by `arm_unverified`; by another count; none) | Right cases with a count |
|---|---|---|---|---|
| ctest, seed 20261114 | 3,000 | 972 (972) | 3 (3; 0; 0) | 1,549 of 2,025 (4,563 of 74,361 arms) |
| ASan and UBSan, seed 20261114 | 20,000 | 6,406 (6,406) | 16 (14; 2; 0) | 10,504 of 13,578 (30,212 of 495,147 arms) |

No sanitizer report. The count is a bound, not a verdict: about one arm in
16 of the right cases is outside the shapes the oracle verifies and is
counted. B cannot run R in 6,406 cases, by its reasons: a barrier while an
arm waits in B 782; a meeting call not past `τ` while it waits (an arm 384,
a past group's call 285, a present or ahead group's call 314); two waiting
arms let in out of R's order 1,194; a late input at or before a waiting
arm's `τ` 437; a cancelled group's disarm at a `τ` not B's `N` 1,104; a
disarm of another group or none in the gap an arm closes 1,906.

The final verification (2026-11-14, head after the oracle's commit): ctest
Release 124 of 124, the sanitizers tree (`setarch -R`) 123 of 123, with the
C and C++ compile checks and the Windows-macros ones; `sig_fuzz` under ASan
and UBSan, 10,000,000 inputs, seeds 8501 to 8508, 8 processes, 35 s: no
report, 1,500,470 graphs opened, 2,745 arms run again, 94 fell back.
Mutants: sig.toml 311 of 311 as expected (325 s, 4 jobs); 213, 214 and 232
are killed by the oracle (test `sig_ref`); 316 survives it (no case
places a cancelled waiting group with a disarm inside a past group's
block, also at 20,000 cases). The retired `sig_arm_oracle.c` and
`sig_arm_diff.c` are removed; the paragraphs below keep their history.

The check round (2026-11-14, the same machine, Release, 8 processes).
The oracle at 3,000 cases on seeds 777001 to 777016 and 20261114, and at
20,000 cases on seed 31415, before and after the round's two fixes:

| Seeds | Cases | B cannot run (counted) | Wrong (by `arm_unverified`; by another count; none) | Right cases with a count, before | After |
|---|---|---|---|---|---|
| 20261114 and 777001 to 777016 | 17 × 3,000 | 16,815 (16,815) | 67 (58; 8; 1) | 26,324 of 34,118 (75,050 of 1,238,987 arms, 6.06 %) | 26,612 of 34,118 (77,067 arms, 6.22 %) |
| 31415 | 20,000 | 6,403 (6,403) | 31 (25; 6; 0) | 10,486 of 13,567 (30,302 of 494,120 arms, 6.13 %) | 10,605 of 13,566 (31,125 of 493,870 arms, 6.30 %) |

The first target holds on every seed. The second missed three cases
before the round:

- Case 1061 of seed 777016: a hole in 2 (b)'s exception (fixed in the
  text first, then in the header). K arms e, G arms d, H disarms e and d
  and arms d, all at one `τ`, all waiting for one L. R lets K in first,
  K's arm lets H in, and H's disarm cancels G's arm; the header made G
  at its place and handed out a hit that R does not have, with no count.
  The header now orders the groups at G's `τ` that wait for L as R does.
- Case 3578 of seed 31415: a header defect, with the text right (S1). A
  waiting group made its disarm before its recenters, so a latch on the
  disarmed detect froze before the recenters reached it. The log had the
  fix since 2192; the waiting group's copy of the gap did not.
- Case 2201 of seed 777001, wrong at call 176 with `arm_past_pinned`
  counted at that call. "Counts and fallbacks" lists `arm_past_pinned`
  as a fallback that the guard stops on, so the case has a count. The
  oracle's tally of other counts (`sig_arm_ref.c`, line 1584) leaves
  `arm_past_pinned` out and prints the case as one with none. Not
  changed: the oracle is the reviewer's.

The round's 2 (b) counts 2.7 % more arms in right cases (6.06 % to
6.22 % of the made groups over the 17 seeds). Two first forms linked K
to H by footprints (at any time: 21 % more on seed 20261114; through
arms at G's `τ`: 15 % more); R's order at L counts 2.7 % more there.

Gates after the round: ctest Release 124 of 124, the sanitizers tree
(`setarch -R`) 123 of 123. `sig_test` adds 2 (b)'s exception with K and
the chain K, J, H (each with a control), the ahead group of
`test_disarm_at_gap_end`, and `test_hit_whole_value`. Mutants 320 to
323 are new and killed. In the full run, sig.toml gave 314 of 315 as
expected (336 s, 4 jobs): 232 (a hit matched on its lanes only)
survived the oracle, because the round's 2 (b) counts all 15 wrong cases
it makes at seed 20261114, where case 1382 had none before.
`test_hit_whole_value` (the oracle's case 286, reduced to a mux that a
past arm switches to another device) now kills it: 315 of 315.

Pending groups (0.6.1, 2026-11-14, the same machine, Release, 8
processes). The oracle with 0.6.0's header (v2-fast, a1c2717) and with
0.6.1's, on the same cases:

| Seed | Cases | B cannot run (counted), both | Wrong, both (0.6.1: by `arm_unverified`; another count; none) | Right cases with a count, 0.6.0 | 0.6.1 |
|---|---|---|---|---|---|
| 20261114 | 3,000 | 972 (972) | 3 (2; 1; 0) | 1,565 of 2,025 (4,686 arms) | 1,352 (3,183) |
| 778001 | 3,000 | 978 (978) | 1 (1; 0; 0) | 1,563 of 2,021 (4,500) | 1,320 (3,073) |
| 778002 | 3,000 | 999 (999) | 8 (4; 4; 0) | 1,542 of 1,993 (4,458) | 1,294 (3,053) |
| 778003 | 3,000 | 959 (959) | 4 (2; 2; 0) | 1,605 of 2,037 (4,745) | 1,375 (3,232) |
| 778004 | 3,000 | 995 (995) | 5 (4; 1; 0) | 1,549 of 2,000 (4,534) | 1,322 (3,097) |
| 61441 | 20,000 | 6,547 (6,547) | 20 (11; 9; 0) | 10,563 of 13,433 (30,819) | 9,005 (20,842) |

Both targets hold on every seed. The wrong cases are the same cases with
both headers; 0.6.1 counts some of them by a fallback or a retracted hit
where 0.6.0 counted `arm_unverified` too. Over the six runs the arms
counted in right cases fall from 53,742 to 36,480 of 851,475 made groups
(6.31 % to 4.28 %, 32 % fewer). The first build (pending decided by L,
counted by calls of waiting groups, no more) left 9 B-cannot cases with
no count on four seeds (reasons 2, 3 and 6 of the oracle); the two rules
of 20b8b93 cover them.

ctest Release 124 of 124, the sanitizers tree (`setarch -R`) 123 of 123,
no sanitizer report. `test_pending` covers L, a frame, a change at the
call, inputs at and before `τ`, a disarm in no group, a present, an
ahead and a later past group, a flush, a trial reset, and a log that
drops G's calls before L. sig.toml 323 of 323 as expected (352 s, 4
jobs): 324 to 329 killed by `test_pending`, 331 by the oracle at seed
778001 (a second `sig_ref` test, `sig_ref_778001`), 330 kept as a guard
(not reached; its note says why); 210's anchor follows the code. After
the reviewer's tally fix (the B-cannot tally accepts a fallback), 778001's
two cases for 331 count by their fallback, so `test_pending` has a case
of its own for it (`pg_reason6`: a pending group, an ahead group that
meets it, and an input that lets in the ahead one only; no fallback in
the case), which kills 331.

- `tests/adapt/sig_test.c` (ctest `test_sig`): v0.6.0 adds
  `test_arm_window`: no window (counted), two crossings after the arm as
  two hits with a latch frozen at the first and the ring's rise and fall,
  a re-arm before handed-out hits (not queued twice), a replay to the same
  digest, a hold a frame covers, `ysig_settle()` (a hold met, one not yet
  met, a frame-rate detect it leaves), a retracted hit, an arm past the
  window, logs that ran full and the window working again after a frame,
  a reset as a barrier, rows changed (counted), rows changed in their
  tier and `unc_us` only (counted), a frame's channel values
  kept with the call, and the desc's limits.
  `tests/adapt/sig_arm_diff.c` (ctest `test_sig_arm_diff`, 3,000 cases,
  until 0.6.0's fix round retired it; see "Results after the decision on
  fix 1"): the randomized differential of an arm in the past. At scale
  (2026-11-12, Linux desktop, GCC 12.2 with ASan and UBSan, `setarch -R`,
  8 processes, seeds 92001 to 92008, 16 min 7 s, after the arm completes
  its hold met by its time): 1,200,000 cases, 1,155,408 exact, 10,726 fell
  back by name (an arm past the window of 8 frames), 32,801 with a
  superseded hit and 1,065 with a retracted one (counted), 0 wrong, no
  sanitizer report. Most retracted ones are on a view (speed), whose hold
  only a frame completes. A hit that the arm's time ends and that B's arm
  finds is compared without its `t_found` (A found it by later input). Each category the review named:

  | Category | Cases | Exact | Fell back | Superseded | Retracted | Wrong |
  |---|---|---|---|---|---|---|
  | Detect on a two-source sum (late reports) | 159,660 | 154,435 | 626 | 4,559 | 40 | 0 |
  | One source, two devices out of order | 159,561 | 154,134 | 604 | 4,791 | 32 | 0 |
  | Equal-time samples | 262,806 | 251,988 | 966 | 9,617 | 235 | 0 |
  | Out-of-order samples dropped | 408,949 | 390,932 | 3,694 | 13,851 | 472 | 0 |
  | Detect on a view (speed) | 160,076 | 155,346 | 635 | 3,255 | 840 | 0 |
  | `ysig_set()` in the window | 211,473 | 200,826 | 3,383 | 7,029 | 235 | 0 |
  | `ysig_recenter()` in the window | 110,914 | 100,324 | 6,947 | 3,643 | 0 | 0 |
  | A reset or `ysig_reset_trial()` before B's place | 784,272 | 758,299 | 4,371 | 20,830 | 772 | 0 |
  | A re-arm earlier than a found hit | 142,183 | 105,783 | 3,574 | 32,798 | 28 | 0 |
  | Entry arm / fresh / now | 399,772 / 399,891 / 400,337 | 382,900 / 389,215 / 383,293 | 3,582 / 3,564 / 3,580 | 12,852 / 6,970 / 12,979 | 438 / 142 / 485 | 0 / 0 / 0 |
  | Hold above 0 | 700,728 | 661,799 | 6,210 | 31,884 | 835 | 0 |
  | Repeat every | 600,384 | 577,135 | 5,401 | 17,272 | 576 | 0 |
  | Hysteresis | 600,447 | 578,529 | 5,265 | 16,083 | 570 | 0 |
  | Norm or rectangle test | 799,351 | 769,384 | 7,152 | 22,125 | 690 | 0 |
  | `ysig_settle()` in the window | 366,739 | 343,643 | 10,382 | 12,420 | 294 | 0 |

  A 19th category, an arm at exactly the end of the detect's old hold (or
  of the hit it found), came with the player's s19383: 400,000 cases
  under ASan and UBSan (seeds 93001 to 93008, 5 min 1 s), 17,608 of them
  in it, 0 wrong overall (386,812 exact, 3,343 fell back, 9,507
  superseded, 338 retracted).

  After the past arm hands out its own hits again (0b20b3c), the
  differential compares the cases with a superseded hit too: A hands out
  B's hits, and before its arm the earlier arm's; B's hits of the detect
  after t and after B's arm come from A's after A's arm. 400,000 cases
  under ASan and UBSan (seeds 94001 to 94008, 4 min 56 s): 353,528 exact,
  42,895 with a superseded hit (compared), 3,262 fell back, 315 retracted,
  0 wrong.

  Users of the detect's output in every case: a latch, a peak gated by
  it, a mux it selects, an all and a delay of it. B is defined only when
  the arm commutes with the calls it moves past: another arm of the same
  detect between B's place and A's call is no call there (two arms at one
  place go in call order). v0.4.1 adds
  `test_review_fixes`: a latch and a peak gate on a reset-armed hit
  through an adapter, no hit with no sample and the hit at the first
  sample, `ysig_value_at()` after a set at, after and before its time,
  a two-input node's `t` and sources across a set, and replays.
  v0.4.0 adds
  `test_set_recompute` (G17): a 90-degree turn of a still pen's cursor
  reaches its held value, a stateless user, a frame-rate test and a bind
  with no new sample; the ring keeps the sample's value; a stateful
  integrator and a peak keep their values and times; a stateless node
  after a stateful one recomputes; a replay to the same digest and values.
  v0.3.0 adds
  `test_entry_now` (G16): a hit at the arm with the held value and the
  arm's time, one per arm, a fall and a rise at a re-arm, a latch frozen
  by the rise, no hit when the test is false at the arm and a hit at the
  next value inside, a reset that arms, a frame-rate detect hit at the
  next onset, `arm` unchanged, a hold under `now`, a replay to the same
  digest, and the refusal of entry 3. v0.2.0 adds `test_mux`,
  `test_latch`, `test_distance`, `test_peak`, `test_all`, `test_silent`,
  `test_detect_out`, `test_reset_trial`, `test_recenter`,
  `test_ops_refusals`, `test_ops_views`, `test_ops_replay`,
  `test_recenter_time`, `test_recenter_chain`, `test_peak_order`,
  `test_late_stamp`, `test_rearm`, `test_hold_frame_input` and `test_u`, each
  beyond a closed form: reports out of time order (two sources, two devices), reads
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
  no longer fires). v0.2.1: MSVC `/W4 /WX` stopped on C4724 ("potential
  mod by 0") in the ring push, where an inlined path has `n == cap == 0`.
  A ring never has capacity 0; the push now returns on one, so MSVC can
  see that the mod is safe.
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
inputs (seed 780) in 9 min 6 s, no finding. v0.5.0 (the target arms up
to 6 ms in the past, so the catch-up runs): 10,000,000 inputs (seeds
8101 to 8108, 8 processes) in 1 min 34 s, 2,226,585 graphs opened and
ran, 1,994,616 hits, no sanitizer report and no replay that differed.
v0.6.0 (the input's bytes size the window, 0 to 4 frames, and its logs;
an arm picks a detect in most inputs and a settle can follow it):
10,000,000 inputs (seeds 8301 to 8308, 8 processes, 39 s, after the arm
completes its hold); 8,500,402 refused, 1,499,270 graphs opened and ran
(16.5 million events taken, 969,892 hits), 6,832 arms in the past ran
again and 150 fell back by name. No sanitizer report, no replay that
differed. One finding before that run:
the window logged feeds that no source took, the journal does not, so a
replay's run again saw other calls (fixed: the window logs the taken
feeds only).
The third review's rules
(frames never refused for data) added two checks to the target: every
frame with a poll (or without one in a graph with no view) returns 0, also
after INT64_MAX and far-future stamps, and every bound channel holds its
bind's input after it. After the third review: 20,000,000 inputs (seed
781) in 9 min 33 s; 14,562,777 refused by `ysig_check()`, 5,436,289
graphs opened and ran (35.0 million events taken, 14.7 million rows,
0.97 million hits), no finding. For v0.2.0 the target builds the new ops and
calls `ysig_reset_trial()` and `ysig_recenter()` (accepted or refused, so
a replay holds across both), and has seeds of valid adapter graphs: `gcov`
shows role resolution, accepted recenters and a latch's hit branch run. The
earlier seeds set the adapter bit in a byte that also holds the node count,
so no adapter graph opened. 20,000,000 inputs (seed 784) in about 37 min, no
finding: 14,504,259 refused by `ysig_check()`, 5,494,950 graphs opened and
ran (640.5 million events taken, 373.5 million rows, 10.17 million hits).
- `tests/mutate/sig.toml`: 184 mutants and a control; 183 killed. Mutant
  189 removes the early exit of the late-stamp walk and survives by design:
  it changes only the cost (v0.2.0: mutants 95 to 191 for the new ops, the
  calls and the reviews' fixes).
  Mutants 89 to 93 move a name off its row (a swap, a gap, the shared
  tables of `speed` and `mean`).
  Mutants 70 and 71 remove the saturation of time arithmetic; the test
  built with `-fsanitize=undefined` (the list's `sig_ubsan` test) catches
  them.
  v0.6.0: mutants 210 to 235 for the window (the run again, the arm's
  place, equal times, the undo log, the checkpoint chosen, hits not
  queued twice, retracted hits, rows, the journal, resets, frame values,
  the logs' capacity, the counts, `ysig_settle()`, the desc's limits, the
  hit's whole value, superseded against retracted hits, the arm's hold
  met by its time); the `sig_oracle` test runs the oracle (20,000 cases,
  its default seed). 215 and 222 are kept with notes (a cost only, or not
  reached). The oracle reaches 247 (the rows' metadata) about once in
  23,500 cases; `test_arm_window` kills it with two samples at one time
  on one device that differ only in tier and `unc_us`. 23, 31, 65, 125, 161, 172, 190, 191 and
  198 follow the code. The second review of the fix round: 177 and 178
  (a hit output's rise at the found time) are killed again; 252 is that
  round's regression B and 253 the rise's sources (`test_hit_out_time`);
  254 to 259 (C, calls from callbacks, `test_arm_review2`). The
  semantics of an arm in the past: 260 to 281 (`test_arm_spec`,
  `test_arm_spec2`, `test_arm_repro`). 213, 214 and 232 are noted for the
  reviewer's oracle (no unit test reaches them since
  `test_sig_arm_oracle` left ctest). The list: 273 of 273 as expected
  (2026-11-13, f2f0e0d, 217 s at 4 jobs).

### Zones

The header has no profiling zones: it does not include ysp/rt.h, and a
zone in it would make ysp/rt.h a dependency. A program times its own
calls. The player (0.3.0) puts `sig.samples` around its feeds and
`sig.frame` around `ysig_frame()` (docs/player.md, "How-to: profile a
run"). On the Linux desktop (i5-14500, GCC 12.2, Release, pinned to CPU
2, the trace ring attached, 2026-11-11), `--sim` on the 3.18 examples:
`sig.frame` median 0.51 us (visuomotor, 57 nodes) and 0.56 us (bimanual,
53 nodes); `sig.samples` median 0.02 us and 15.10 us a frame (bimanual
feeds two mice into two graphs). A zone per op stays not done: it needs
the dependency, and a zone pair with the trace ring attached costs 51 ns
on this machine (`rt_ring_bench` B5, the same day), against 9 ns a node
in that `ysig_frame()` (0.51 us over 57 nodes).

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
- A re-arm stamped before a hit detect's last ring entry cannot push an
  entry, so `ysig_value_at()` after it reads the old value (live reads
  are correct). An arm at or after the last entry is correct.
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
- Not run: macOS, a window, a real device, a photodiode. MSVC: the
  compile checks and the test in CI only.
