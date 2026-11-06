# Continuous input: specification draft

Status: draft for review, 2026-11-06. Nothing in this document is
built. It proposes one header, called `ysp/sig.h` (prefix `ysig_`) here;
the name is open (section 11). Read it with
[rig_spec.md](rig_spec.md) section 6 (the player),
[experiment_format.md](experiment_format.md) D6 and D7 (no expression
language; anything else is a script) and
[devices_spec.md](devices_spec.md) section 7 (the input event).

## 1. Summary

Many tasks map a continuous input to a stimulus on every frame: a cursor
from a mouse, a gaze-contingent window from an eye tracker, a cursor
from a joystick or a force sensor, one cursor from two hands. Today each
C program writes this mapping by hand, and the experiment definition
(version 1) cannot express it at all.

This document proposes a **signal path**: a small, fixed graph of nodes
from input sources, through per-sample transforms and maps, to stimulus
properties, detectors and data streams. A cursor is one graph; a
gaze-contingent mask is another. The graph is data, so the player can
load it from the experiment definition, check it at load, run it
without a script, and record it so that a run can be replayed.

The signal path does not replace scripts. Task logic, physics with
state, and stimuli placed from the participant's behavior stay with the
script layer (section 3.2).

## 2. The problem

### 2.1 What the examples do by hand

Five example programs in `examples/response/` map input to a stimulus.
Each reimplements the same parts:

| Program | Sources | Per-sample work | Map | Frame-rate work | Detectors |
|---|---|---|---|---|---|
| `trial_mouse_tracking` | one raw mouse | integrate counts | none | cursor position | movement onset (10 px from where the pointer was at the start click); the response is a click |
| `trial_visuomotor` | one raw mouse | integrate counts | rotation about the start | cursor, or a ring for the distance only | movement onset, crossing of the target radius |
| `trial_copying` | mouse, raw mouse or pen | integrate (mouse) or absolute (pen) | none | the ink path | none per sample; scoring at the end |
| `trial_bimanual` | two raw mice (a third ignored) | integrate counts per mouse | select axes or mean, then rotation or mirror | cursor; a sum-of-sines target | leave the start, speed under a threshold for a time, target entry |
| `trial_timed_response` | keys | none | none | none | none (shown for contrast) |

The parts repeat: integrate with an exact count sum, a 2x2 map, a held
value read at the frame's onset, a crossing on a derived position, and a
per-sample trace file. Each copy is a place for an error: the first
`--auto` run of `trial_bimanual` failed because the program read the
hands at a flip record's onset before it polled that frame's input
(docs/examples_findings.md D10).

### 2.2 What the headers lack

From docs/examples_findings.md (2026-11-06):

- F19: no way to read the response trace before the window ends.
- F20: a position made from two devices takes the device and the tier of
  the hand whose report crossed; the other hand's part can be one report
  interval old.
- F21: `yrsp_cursor` clamps to [0, w] x [0, h], so a displacement needs a
  false area.
- F22: no timeline channel whose value is a function of time; a
  sum-of-sines target is computed outside the timeline.

### 2.3 What the player lacks

Version 1 of the experiment definition is data only: steps that set,
tween and show stimulus properties, and a response from keys or buttons
(D7). It has no way to bind a stimulus property to an input. D6 sends
"a computed value" to the script layer, and the script language (Luau)
is proposed, not decided. So `trial_visuomotor`, `trial_copying` and
`trial_bimanual` cannot be written as a definition today.

A built-in cursor stimulus would cover the mouse tasks only. Eye
trackers, joysticks, force sensors and streams from Lab Streaming Layer
need the same parts with other sources. The graph covers them all with
one mechanism.

### 2.4 Prior art: miniaudio's node graph

miniaudio 0.11.25 (the version `ysp/audio.h` builds on; its manual,
section 7) routes audio through a node graph. Its nodes have input and
output buses with fixed channel counts, stock nodes, custom nodes
through a vtable, flags for nodes that only observe, and starts and
stops scheduled on the graph's clock. The comparison:

| | miniaudio `ma_node_graph` | This draft |
|---|---|---|
| Data | Blocks of float frames at one sample rate, which the data sources set | Samples at irregular times, each with its own `t`; sources at different rates, with no common tick |
| Evaluation | Pull: each `ma_node_graph_read_pcm_frames()` reads the endpoint, which reads its inputs recursively | Push: a sample runs the nodes after its source (section 5.1); a frame reads held values |
| Time | A global clock and a clock per node, in frames, exact because audio is isochronous | The rig clock in ns, per sample; a frame reads at its onset |
| Two outputs into one input | Mixed (summed) without a node | Only through `sum`, `mean`, `diff` or `join`, each holding its inputs' latest values |
| One output into two inputs | A splitter node | Any later node reads any earlier node's output |
| Changes to the graph | Attach and detach at any time from any thread: spinlocks per bus, and a detach waits for the audio thread | Fixed for a trial, given as data, checked at load; one thread |
| Custom nodes | `ma_node_vtable`, with `ma_node_base` as the first member | Custom ops through a vtable (section 5.4) |
| Changes inside a run | A start and a stop per node, at a frame of the global clock | A node's inputs change with a timeline channel; its parameters do not (principle 3) |
| Nodes that only observe | `MA_NODE_FLAG_PASSTHROUGH`, `MA_NODE_FLAG_SILENT_OUTPUT` | `record` and `detect` sinks |
| Nodes with no input | `MA_NODE_FLAG_CONTINUOUS_PROCESSING` | Generators, at the frame rate |
| Two rates | `MA_NODE_FLAG_DIFFERENT_PROCESSING_RATES` (resampling) | Sample rate and frame rate, joined by the held value (section 6.2) |
| Memory | Allocation callbacks per node; the caller owns each node struct | One arena from the caller, sized by `ysig_arena_size()` |

What this draft takes from it: custom nodes through a vtable, channel
counts checked when two nodes connect, and changes at a set time inside
a run (here through a node's inputs). What it does not take, and why:

- **Mixing without a node.** Right for audio, wrong for positions: two
  hands summed by an extra edge is a silent error.
- **Pull evaluation.** An input device cannot be asked for a sample at a
  time; it delivers one. A recursive pull also computes a shared node
  once for each reader.
- **Changes to the graph during a run.** They are the cause of
  miniaudio's locks and of its rules for a node's lifetime. A graph
  fixed for a trial needs none of them, and only a fixed graph can be
  checked at load, stored in the definition and replayed.

## 3. Scope

### 3.1 In scope

Sources: every `yin_event` SAMPLE that `ysp/input.h` defines, and the
values of other nodes.

| Source | Kind and control | Typical rate |
|---|---|---|
| Raw mouse counts | MOUSE, `YIN_AXIS_DELTA` | 125 to 8000 Hz |
| Mouse or touch position | MOUSE or TOUCH, `YIN_AXIS_POSITION` | the OS's rate |
| Pen position, pressure, tilt | PEN, `YIN_AXIS_POSITION`, `YIN_AXIS_PEN + n` | the tablet's rate |
| Gamepad sticks and triggers | GAMEPAD, `YIN_AXIS_GAMEPAD + n` | the controller's rate |
| Gaze | EYE, `YIN_EYE_GAZE` (x, y window pixels, value the pupil) | the tracker's rate |
| A box's analog line, a force sensor, EMG | BOX or USER, the device's control | the device's rate |
| A Lab Streaming Layer stream | the kind `ysp/net.h` gives it | the stream's rate |

Rates in this table are typical device rates, not measurements on a ysp
rig.

Uses:

- a stimulus property from an input (a cursor, a mask, a gain on a
  contrast);
- a stimulus property from a generator (a tracking target);
- a detector that ends a step or a trial (enter a region, speed under a
  threshold for a time, a distance traveled);
- a per-sample and a per-frame record of every node a sink reads.

### 3.2 Out of scope (scripts, or another header)

| Need | Why not here | Where |
|---|---|---|
| A virtual object with dynamics (a ball in a cup, a mass on a spring) | state beyond a linear filter, and its own integration step | a custom op (section 5.4), or a script |
| A target placed from where the hand stopped | a value that depends on the trial's history, not on the signal | a script |
| Branches on performance | task logic | the definition's nodes (`if`, `loop`) or a script |
| An ink trail that grows with the pen | a growing path, not a value | `ysp/gfx.h` (F11: a segment-instanced stroke) |
| Scoring a drawn shape (Procrustes) | an analysis at the end | the program or the analysis |
| A key or button response | discrete events | `ysp/response.h`, unchanged |

## 4. Principles

1. **Samples carry time.** A per-sample node runs once for each sample,
   at the sample's `t`, in the order of that source's samples. A frame
   reads the held result; it never recomputes it.
2. **Exact where it can be.** Integration of counts keeps an int64 sum
   per axis, as `yrsp_cursor` does since response.h 0.8 (a float sum
   drifted 0.033 px off the count grid in 160 reaches at gain 0.1).
   Arithmetic inside the graph is double; a channel value is float.
3. **The graph is fixed for a trial.** Its nodes and edges are set at
   open. A node's parameters (`p[]`) change between trials, not during
   one. A node's inputs change at any sample: a value that must change
   inside a trial at a set time (a rotation that turns on in the middle
   of a reach) is an input, from a `channel` node that a timeline step
   drives. The loader checks the whole graph before the session, as
   `ytl_check_ops()` does for timelines.
4. **No allocation after open.** The caller gives the state arena. A
   frame's work has a stated cost per node and per sample (section 9).
5. **Values reach stimuli as channels.** A sink writes a float channel
   that `ygfx_apply()` reads, the same way a timeline channel does. The
   renderer does not change.
6. **The tier is the worst of the sources.** A value made from two
   sources carries the worse stamp tier of the two and the time of the
   older input (F20). A detector's result says so.
7. **Everything a sink reads is recorded.** The raw samples of each
   source and the graph's definition replay every value bit for bit,
   as `yrdk_replay()` does for dots.
8. **Deterministic under `--sim`.** The virtual clock and scripted
   samples give the same values on every host.

## 5. The model

### 5.1 Graph

A graph is an array of nodes. A node has an op, up to four inputs (other
nodes' outputs), parameters, and one output. An output is a vector of
1 to 4 lanes (x, y, z, w). Nodes come in an order where each input
precedes its user; the loader refuses a cycle, and refuses an edge whose
lane count the op does not accept.

`ysig_open()` compiles two schedules, so that no evaluation walks the
graph:

- **Per source:** for each source, the ordered list of nodes that
  depend on it. A sample runs that list and nothing else, so its cost
  is fixed at open.
- **Per frame:** the generators, then the `bind`, `detect` (frame-rate
  tests) and `record (frame)` sinks, in node order.

A node that two sources reach (a `join` of two hands) is on both lists.

Two rates:

- **Sample rate.** A node whose inputs reach back to a source runs when
  that source delivers a sample. Its output gets the sample's `t`.
- **Frame rate.** A sink reads its input's held value at the frame's
  onset (section 6.2). A generator runs at the frame rate on the
  frame's base time.

### 5.2 Node kinds

| Kind | Op | Lanes in, out | Parameters | Notes |
|---|---|---|---|---|
| Source | `source` | 0, 1 to 4 | role or device, kind, control, which fields (x, y, value) | One device, or a role the rig profile binds |
| Per sample | `integrate` | 1 to 4, same | gain per lane, origin, optional bounds | int64 count sum for DELTA sources; double otherwise (rate control: a joystick's deflection integrated over `dt`) |
| | `lowpass` | n, n | cutoff Hz, order 1 or 2 | on the sample's `dt`; refuses a rate it cannot meet |
| | `velocity` | n, n | lag s | difference over at least the lag, as `trial_visuomotor`'s 10 ms |
| | `speed` | n, 1 | lag s | the norm of `velocity` |
| | `delay` | n, n | s | a ring of the past samples, sized at open |
| | `deadzone` | n, n | radius | for sticks |
| | `rectify`, `envelope` | 1, 1 | cutoff Hz | EMG |
| Map | `affine` | 2, 2 | 2x2 matrix, offset | gain, rotation, mirror, an error-clamp projection |
| | `scale`, `offset` | n, n | per lane | |
| | `clamp` | n, n | low, high per lane | bounds on the drawn value, not the hand (F21) |
| | `curve` | 1, 1 | a table of (in, out) points | monotone, piecewise linear |
| | `units` | n, n | from, to (counts, px, cm, deg) | through the rig profile's `view` and the device's CPI |
| Combine | `select` | n, m | lane list | "x of the left mouse" |
| | `join` | 1 + 1, 2 | none | two scalars into a vector |
| | `sum`, `mean`, `diff` | k x n, n | weights | the cursor of two hands; a tracking error |
| Generator | `sines` | 0, 1 to 2 | amplitudes, frequencies, phases, ramp s | a tracking target (Yang, Cowan and Haith 2021) |
| | `noise` | 0, n | seed, cutoff Hz | seeded per trial (D8) |
| | `channel` | 0, 1 | timeline channel | a timeline value into the graph (a ramp, a gain set per trial) |
| Sink | `bind` | n, 0 | stimulus property per lane | writes the channel `ygfx_apply()` reads |
| | `detect` | n, 0 | test, level, hold s | section 7 |
| | `record` | n, 0 | stream name, `sample` or `frame` | section 8 |

Every op has a parameter table in the shared row shape
(`ytl_op_params()`, `ygfx_params()`), so the designer and the bindings
read names, types, units, bounds and defaults from one place.

A mid-reach rotation, with the angle as an input:

```
hand  = integrate(source(role "hand", MOUSE, DELTA), gain)
angle = channel(timeline channel ROT)      # a timeline step sets 30 deg
cur   = affine(hand, rotation by input angle, about start)
```

### 5.3 Examples as graphs

Visuomotor rotation (`trial_visuomotor`):

```
mouse = source(role "hand", MOUSE, DELTA)
hand  = integrate(mouse, gain 0.25 px/count, origin start)
cur   = affine(hand, rotation 30 deg about start)
bind(cur -> cursor.x, cursor.y)
detect(hand, distance from start >= 26.9 px)   -> movement onset
detect(hand, distance from start >= radius)    -> end of reach
record(mouse, sample); record(cur, frame)
```

Bimanual tracking (`trial_bimanual`, split mapping, mirror):

```
L = integrate(source(role "left", MOUSE, DELTA), gain)
R = integrate(source(role "right", MOUSE, DELTA), gain)
u = join(select(L, x), select(R, y))
c = affine(u, [[0, 1], [1, 0]])
t = sines(x: a, f, phi; y: a, f, phi; ramp 5 s)
bind(c -> cursor); bind(t -> target)
detect(diff(c, t), norm <= 0.8 cm) -> inside (per frame, a color)
record(L, R, sample); record(c, t, frame)
```

Gaze-contingent window:

```
g = lowpass(source(role "eye", EYE, GAZE), 30 Hz)
bind(g -> mask.x, mask.y)
detect(source(role "eye", EYE, PROXIMITY), value == 0) -> track loss
```

Joystick, rate control:

```
s = deadzone(source(role "stick", GAMEPAD, LEFT_X and LEFT_Y), 0.08)
p = clamp(integrate(s, gain 600 px/s), window)
bind(p -> cursor)
```

The numbers in these sketches are the examples' values or placeholders,
not recommendations.

### 5.4 Custom ops

An op the built-in set lacks (a mass with damping driven by a force
sensor, a motion-capture model) comes as a vtable, as miniaudio's
custom nodes do (section 2.4), through the extension mechanism of
rig_spec section 12:

```c
typedef struct ysig_op_vtable {
    uint32_t version;                 /* YSIG_OP_ABI */
    const char* name;                 /* the op's name in definitions */
    uint8_t lanes_in[4], lanes_out;   /* 0 = any, checked at load */
    size_t state_size;                /* bytes in the graph's arena */
    const ysig_param* params; int n_params;
    void (*reset)(void* state, const double* p, int64_t t_ns);
    void (*sample)(void* state, const double* p, const ysig_value* in, int n_in,
                   ysig_value* out);  /* at a sample; no allocation, no I/O */
    void (*frame)(void* state, const double* p, int64_t onset_ns, ysig_value* out);
} ysig_op_vtable;
```

- The graph allocates the op's state in its arena; the op allocates
  nothing.
- A custom op runs on the frame thread under the same rules as a
  built-in one, and its time per call is a zone in the trace.
- The definition names the op; the pack manifest names the module, by
  hash. A run whose module is missing does not start.
- Replay needs the same module. The run's record has the module's hash.

## 6. Time

### 6.1 Order of samples

`ysp/input.h` says that times from different sources need not rise in
arrival order. Within one source they rise. So:

- A per-sample node with one source input runs in that source's order.
- A node with inputs from two sources (`join`, `sum`, `mean`, `diff`)
  holds the latest value of each input and runs when either changes.
  Its output's `t` is the newer input's `t`; its `t_oldest` is the older
  input's `t`; its tier is the worse of the two (principle 6).
- A sample whose `t` is older than its source's last sample is counted
  and dropped. The count goes in the describe record.

### 6.2 Reading at a frame

A sink reads the held value of its input as of the frame's onset: the
result of the last sample with `t <= onset`. The frame loop polls input
before it reads (D10). With a flip record, the onset is the record's;
before it arrives, the plan's.

**Prediction** is a per-sink option, off by default: extrapolate the
value from its last samples to the onset with a stated horizon and
method. It trades error for latency, so the definition states it and
the data records it. No default horizon is proposed until a photodiode
run measures the input-to-photon latency on a rig.

### 6.3 Generators

A generator's value on a frame is its formula at the frame's base time
on the trial base (`ytl_base_time()`), so a late frame shows the value
for its own onset, not the planned one. This is the function-valued
channel of F22 and makes `ytl_set_fn()` unnecessary in `ysp/timeline.h`.

### 6.4 Trial boundaries

At a trial's start the program calls `ysig_reset()`: integrators go to
their origins, filters to their first sample, delays empty. Parameters
the trial sets (a rotation per block, a gain per condition) apply at the
reset. A source keeps its device across trials.

## 7. Detectors and ysp/response.h

A detector tests its input on every sample, with the crossing rule of
`ysp/response.h` (CROSSINGS: the first sample past the level, no
interpolation between samples). Tests: `>=`, `<=` on a lane or a norm,
inside or outside a region (circle or rectangle), and a hold (the test
true for a time, as "speed under 6.5 cm/s for 1 s"). A detector's result
is a time, the sample that met it, the tier (principle 6) and the
source of the older input.

Two ways to connect detectors to responses:

- **(a) Feed the collector.** The graph emits each detector input as a
  derived `yin_event` (a new flag, `YIN_DERIVED`; kind USER; device the
  graph's node id) and the collector's DISTANCE and crossing choices do
  the rest. RT, windows, early moves, data columns and `rsp_flags` work
  unchanged.
- **(b) Detectors in the graph.** The graph ends steps and trials itself.
  The collector keeps keys and buttons.

Proposal: (a) for anything that is a response (a movement onset, a
reach's end), so the data columns stay the collector's; (b) for events
that only drive the trial's flow (the hold before the target, a color
while the cursor is in the target). The bimanual example uses both
today. Section 11 keeps this open.

## 8. Recording and replay

- `record(node, sample)` writes one row per sample of the node:
  `t_s`, the lanes, the tier, the source's device. For a source this is
  the raw input; the replay needs only these.
- `record(node, frame)` writes one row per frame: the frame index, the
  onset `t_s`, the lanes, `t_oldest_s`.
- Rows go to `streams/<name>.csv` through `yds_stream()`
  (`ysp/dataset.h`). Times are seconds (rig_spec convention).
- `ysig_replay()` runs the graph on the recorded source rows with the
  definition's parameters and seeds, and gives the same values bit for
  bit. The player's verify step and the examples' `--sim` checks use it.
- A run also writes the graph's definition and a digest of every
  recorded node, as an `rdk` stimulus writes `<name>_digest`.

## 9. C API, sketch

```c
typedef struct ysig_node {
    uint16_t op;           /* YSIG_OP_* */
    int16_t  in[4];        /* node indices; -1 = none */
    uint8_t  lanes;        /* 1..4 */
    double   p[16];        /* parameters, by the op's table */
    const char* name;      /* for records and errors */
} ysig_node;

typedef struct ysig_desc {
    const ysig_node* nodes; int n_nodes;
    const ysig_op_vtable* const* custom; int n_custom; /* ops YSIG_OP_CUSTOM + i */
    void* arena; size_t arena_size;   /* state; ysig_arena_size() gives it */
    float* channels; int n_channels;  /* where bind sinks write */
} ysig_desc;

bool  ysig_check(const ysig_node* nodes, int n, char* err, size_t err_size);
size_t ysig_arena_size(const ysig_node* nodes, int n);
bool  ysig_open(ysig_graph* g, const ysig_desc* d);
void  ysig_reset(ysig_graph* g, int64_t t_ns);          /* trial start */
bool  ysig_set(ysig_graph* g, int node, int param, double v);  /* between trials */
void  ysig_feed(ysig_graph* g, const yin_event* e);     /* every input event */
void  ysig_frame(ysig_graph* g, const yscr_frame* f, int64_t base_ns);
int   ysig_detected(ysig_graph* g, ysig_hit* out, int cap); /* since the last call */
const ysig_param* ysig_op_params(int op, int* n);
```

- `ysig_feed()` takes every event, as `yrsp_feed()` does; a node takes
  only its source's events.
- `ysig_frame()` reads at the onset, runs the generators, and writes the
  `bind` channels. The program calls it before `ygfx_apply()`.
- One thread: the frame thread. Events come through the screen's input
  bridge as they do for the collector.
- Cost: to measure before the API is fixed. The budget is the project's
  rule that a trial's frame work fits in one 16 ms frame. The test is four
  raw mice at 8 kHz each and 32 nodes, on the Linux desktop and the
  Windows development machine, with Tracy zones per op.

## 10. Experiment definition

A template gains a `signals` object: node name to node, in the JSON
form of section 5. A stimulus property takes `{"signal": "cur.x"}`. A
step takes `{"until": "detector name"}`, and a response choice takes
`{"signal": "detector name"}` (section 7, way (a)). Parameters take
`$variables`, set at the trial's reset.

This is a change to D7 (the three-part template), so it is format
version 2 (D12). It keeps "a data-only experiment has no script": a
graph is data. The four worked examples of experiment_format.md section
2.14 do not change.

## 11. Open questions

- **The name.** `ysp/sig.h` avoids the standard `<signal.h>`; "control"
  is already the axis field of `yin_event`.
- **Detectors: way (a), (b) or both** (section 7). (a) needs a
  `YIN_DERIVED` flag and a rule for the tier in `ysp/input.h`.
- **Lanes.** Four lanes cover position, pressure and tilt. Is a 3D
  motion-capture marker plus orientation (7 values) a source to plan
  for now?
- **Units.** Where a mouse's CPI and a tablet's size live: the rig
  profile's device entry is the likely place (devices_spec 11.2), beside
  the `view` block that gives degrees.
- **Prediction** (section 6.2): which methods, and whether the designer
  shows the extra error it costs.
- **Eye tracker latency.** A tracker with a device clock gets the clock
  fit of `ysp/rt.h`; does a gaze-contingent sink also need a stated
  end-to-end latency in the data, measured per rig?
- **Generators here or in the timeline.** Section 6.3 puts them here.
  A timeline track already gives ramps; a `channel` node reads it.
- **Dynamics.** Whether a small, fixed set of linear dynamics (a mass
  with damping, driven by a force sensor) is built in, or only a custom
  op (section 5.4).
- **Custom op ABI.** The `ysig_value` type (lanes, `t`, `t_oldest`,
  tier), and whether a custom op can also be a source (a device with
  its own reader thread, as rig_spec section 12's input source).

## 12. Verification

- Rewrite `trial_visuomotor`, `trial_mouse_tracking` and
  `trial_bimanual` on the header. Their `--sim` checks must pass with
  the same data files, byte for byte where the files do not change
  columns.
- A unit test per op against a closed form (an integrator's count sum,
  a filter's step response, a sum of sines at known phases).
- `ysig_check()` and the JSON loader parse untrusted input: a fuzz
  target.
- A mutation list, as for every header.
- A cost measurement (section 9) before the API is final.
- On a rig: an input-to-photon run with a photodiode for one source
  (a raw mouse moved by a solenoid, or a uinput script with its write
  time) before any latency claim.

## 13. Order of work

1. Settle section 11's detector question and the name.
2. The header with sources, `integrate`, `affine`, `select`, `join`,
   `bind` and `record`; rewrite `trial_visuomotor` on it.
3. Filters, `speed`, `detect`, `sines`; rewrite `trial_bimanual`.
4. The loader in the player and format version 2.
5. Eye tracker and gamepad sources with a device on a rig.
