# Continuous input: specification draft

Status: draft for review, 2026-11-06. Steps 2 to 4 of section 13 (the C
side) are built as `ysp/sig.h` v0.1.0 (prefix `ysig_`, 2026-11-07;
[sig.md](sig.md) is its manual). Section 14 lists where the header
differs from this draft and why; the sections below say so where they
changed. Section 15 tests the designer-level components on paper. Read it with
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
without a script, and record it so that a run can be replayed. The
experiment's graph reads typed roles in physical units; the rig profile
binds each role to a device through an adapter, so one definition runs
with a mouse, a tablet or a motion-capture system (section 5.5).

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

### 2.5 Prior art: Unity's Input System

Unity's Input System package (manual for version 1.11) separates what a
game wants from the device that gives it. Section 5.5 takes the same
split. From the manual:

- An **action** is what the game reads ("look", "move"). Its type is
  `Value`, `Button` or `PassThrough`.
- A **binding** connects an action to a control by a path, as
  `"<Mouse>/delta"` or `"<Gamepad>/leftStick"`.
- A **composite** "source[s] values from other Bindings ... and then
  synthesize[s] input on the fly from those values": `2DVector` makes a
  Vector2 from four keys.
- A **processor** changes a value: Clamp, Invert, Normalize, Scale (each
  also for Vector2 and Vector3), AxisDeadzone, StickDeadzone. "Processors
  must be stateless." A processor goes on a binding, on an action or on
  a control; the binding's runs first.
- An **interaction** finds a pattern: Press, Hold, Tap, SlowTap and
  MultiTap, with `duration` and `pressPoint`. Interactions can keep
  state.
- A **control scheme** groups the bindings for one kind of device
  ("Keyboard&Mouse", "Gamepad"), so one action map serves both.

| Unity | This draft |
|---|---|
| Action, with a type | Role, with a type and a unit (section 5.5) |
| Binding path | An adapter's source node |
| Composite | `join`, `select`, `sum`, `mean` |
| Processor on a binding | A node in the rig's adapter |
| Processor on an action | A node in the experiment's graph |
| Interaction | `detect` |
| Control scheme | An adapter in the rig profile |

Where this draft differs, and why:

- **State in the transforms.** Unity's processors are stateless, so an
  integrator, a filter or a delay has no place in the chain. A
  psychophysics rig needs all three, run on each sample.
- **Units and calibration.** Unity's values are in device units: pixels,
  or -1 to 1 for a stick. A role here has physical units, and the
  rig's adapter gives them, so a threshold in cm means the same on every
  device.
- **Who binds.** In Unity the game's asset holds the bindings, and the
  player can rebind them. Here the experiment declares roles and the rig
  profile binds them, because a device's calibration belongs to the rig,
  not to the experiment or the participant.
- **Time.** Each value here keeps the time and tier of the sample that
  made it (principle 1, principle 6). The Unity pages read for this
  section do not say whether an action's value keeps each event's time;
  they name three update modes (dynamic, fixed, manual).

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
| A motion-capture marker (3D position, or position and orientation) | a stream sample (section 5.5), through LSL or an input-source extension | 120 to 240 Hz |

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
   open. The player sets a node's parameters (`p[]`) at trial
   boundaries, not during a trial; the C API applies a set at once and
   journals it, so a replay sees it where it came. A node's inputs change
   at any sample: a value that must change
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
| Map | `affine` | 2 (+ 1, an angle), 2 | 2x2 matrix, center (pre), offset (post), angle | gain, rotation, mirror, an error-clamp projection; out = Rot(angle) M (in - center) + offset; built: the angle input is a timeline channel |
| | `scale`, `offset` | n (+ 1, a factor), n | per lane | built: scale's second input multiplies every lane (a ramp channel) |
| | `clamp` | n, n | low, high per lane | bounds on the drawn value, not the hand (F21) |
| | `curve` | 1, 1 | a table of (in, out) points | monotone, piecewise linear |
| | `units` | n, n | from, to (counts, px, cm, deg) | through the rig profile's `view` and the device's CPI |
| Combine | `select` | n, m | lane list | "x of the left mouse" |
| | `join` | 1 to 4 inputs, their lanes (at most 4) | none | two scalars into a vector; built for any lanes (a hand and a cursor into one record) |
| | `sum`, `mean`, `diff` | k x n, n | weights | the cursor of two hands; a tracking error |
| Generator | `sines` | 0, 1 to 2 | amplitudes, frequencies, phases, ramp s | a tracking target (Yang, Cowan and Haith 2021) |
| | `noise` | 0, n | seed, cutoff Hz | seeded per trial (D8) |
| | `channel` | 0, 1 | timeline channel | a timeline value into the graph (a ramp, a gain set per trial) |
| Sink | `bind` | n, 0 | stimulus property per lane | writes the channel `ygfx_apply()` reads |
| | `detect` | n, 1 | test, level, hysteresis, hold s, sample or frame rate, armed at reset or by a call, once or every | section 7; built with one output lane, the test's state, for a color while the cursor is inside |
| | `record` | n, 0 | stream name, `sample` or `frame` | section 8 |
| | `emit` | n, 0 | kind, control | built: section 7's way (a), a derived `yin_event` per update |

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

### 5.5 Roles, types and adapters

Aim: one definition runs on a mouse rig and on a motion-capture rig.
Only the rig profile changes.

The graph has two parts:

- **The experiment's part** reads roles. It holds the design: the
  display gain, the rotation, the targets, the thresholds in cm, the
  detectors and the records.
- **The rig's part** is one adapter per role. It reads a device and
  makes the role's value. It holds what the rig knows: the device, its
  counts per inch or its size, a calibration, a filter.

**A role** has a name, a type and a unit space, declared in the
definition:

| Type | Lanes | Meaning |
|---|---|---|
| `position2d` | x, y | an absolute position in the workspace plane |
| `position2d_relative` | x, y | a position integrated from motion; lifting the device moves the hand without the cursor |
| `position3d` | x, y, z | an absolute position in the workspace |
| `pose` | position and orientation | a rigid body |
| `scalar` | 1 | a force, a pressure, an EMG envelope |
| `gaze` | x, y | a point of regard in the display plane |

Unit spaces: `workspace_cm` (the plane the hand moves in, origin and axes
from the rig's calibration), `display_cm` and `display_deg` (from the
profile's `view` block), `display_px` (window pixels; added in ysp/sig.h
because the examples' tasks are designed in pixels, and a role in pixels
keeps their data files byte-identical), `device` (no conversion; the
definition then depends on the device and says so).

**An adapter** is a subgraph in the rig profile (devices_spec section
11.2) that ends in the role's value, in the role's type and units:

```
# mouse rig: role "hand", position2d_relative, workspace_cm
hand = scale(integrate(source(MOUSE, DELTA)), 2.54 / cpi)    # cpi from the profile

# tablet rig: role "hand", position2d, workspace_cm
hand = affine(source(PEN, POSITION), tablet_px_to_cm)        # from the tablet's size

# motion-capture rig: role "hand", position2d, workspace_cm
m    = source(stream "markers", marker 3)
hand = lowpass(select(affine3(m, calibration), x, y), 30 Hz)  # calibration from the profile
```

The experiment's part of a visuomotor task is then the same on all
three rigs:

```
cur = affine(scale(role("hand"), display_gain), rotation 30 deg about start)
bind(cur -> cursor)              # display_cm to px through the view block
detect(role("hand"), distance from start >= 1 cm) -> movement onset
```

Rules:

- The loader refuses a run where an adapter's type or units differ from
  the role's. A role of type `position2d` accepts a `position2d_relative`
  adapter only when the definition allows it, because a relative device
  needs a step that the task must design (move the cursor back to the
  start without the hand; recenter).
- A calibration is measured, not typed in: a procedure (touch known
  points, fit an affine or a rigid transform) writes it to the profile
  with its date, its residual in mm and the points it used, as the
  `view` block is measured. A run records the calibration's hash.
- The data records, per role: the adapter, the device and its identity,
  the calibration's hash, and the raw samples in device units (for
  replay). The derived columns are in the role's units, so the columns
  of a mouse run and of a motion-capture run are the same.
- A source with more values than `yin_event` holds (3D position,
  orientation, many markers) comes as a **stream sample**: `t`, a
  stream id, a sample index, the stamp's tier and N doubles, posted by
  `ysp/net.h` or by an input-source extension (rig_spec section 12).
  `yin_event` stays 48 bytes; its cache-line layout in ysp/screen.h's
  input slot does not change.

## 6. Time

### 6.1 Order of samples

`ysp/input.h` says that times from different sources need not rise in
arrival order. Within one source they rise. So:

- A per-sample node with one source input runs in that source's order.
- A node with inputs from two sources (`join`, `sum`, `mean`, `diff`)
  holds the latest value of each input and runs when either changes.
  Its output's `t` is the newer input's `t`; its `t_oldest` is the older
  input's `t`; its tier is the worse of the two (principle 6). As built:
  in a sample's pass the output's `t`, device and source come from the
  input that changed, along the path: the arriving sample's (the report
  that made the value, the one that crossed), even when the other
  input's held part is newer in time, and after a delay the delayed
  sample's; in a frame's pass and in a read at a past time, from the
  newest input; `t_oldest` stays the oldest input's. Emit and record
  carry that true `t`, which need not rise in arrival order; a detect's
  hold clock never goes back (the later of the new and its last,
  counted), and a crossing's hit has the time of the value that met it.
- A sample whose `t` is older than its source's last sample is counted
  and dropped. The count goes in the describe record. As built: order is
  kept per source and device (two mice on one any-device source do not
  drop each other; 8 devices a source, the least recently seen slot
  reused), and a raw mouse's counts are never dropped, because a sum
  does not depend on the order of its terms.
- As built: a source has no value before its first sample (tier
  UNKNOWN, nothing in its ring), and times saturate at the ends of int64.

### 6.2 Reading at a frame

A sink reads the held value of its input as of the frame's onset: the
result of the last sample with `t <= onset`. The frame loop polls input
before it reads (D10). With a flip record, the onset is the record's;
before it arrives, the plan's. As built: `ysig_frame()` reads held values
(the caller feeds the frame's input first), and the record's onset, which
arrives a frame later, is read with `ysig_value_at()` from rings a node
keeps on request; a stateless node after two sources is computed from its
inputs' rings, so a late report of one source is placed by its time.
Velocity, speed, delay, low-pass and rate integration give a view at a
frame without changing their state: a still mouse sends nothing, and its
speed must still fall to 0. A view never infers stillness from the gap
between the last sample and a planned onset, which is in the future when
the frame is prepared: the frame gives the poll's time (the latest time
input is known up to, with a flag that it is set); a view uses input up
to the earlier of the onset and that poll, as given (an event stamped
after it is counted as a late poll and moves nothing); and a velocity or
speed is 0 only when its input was silent for the lag up to it. A frame
is refused only for bad arguments and for a frame without a poll in a
graph with a view, on every call; a poll of 0 is a valid time; a reset
does not change the newest event's time. A custom op gets the onset, not
the poll, so it cannot implement a view. A stateless node with a generator input is read at a past time
from its inputs' rings, as one after two sources. History rings hold values at sample times;
a view never enters them. These five ops keep state per sample, so the
loader refuses one with no source upstream (on a generator or a channel
alone); a frame-rate form can come later.

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
reset. A source keeps its device across trials. As built: `ysig_set()`
takes effect at once and keeps the state, because a center-out task keeps
its hand across trials (the hand returns to the start; a reset would move
it there); a parameter that sizes the arena or a schedule is refused.

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
  As built: it runs a journal of every call that changed the graph
  (resets, sets, arms, the events a source took or dropped, the frames
  with their channel values), and gives the same rows, hits and digest
  on the same build. Across builds, libm and fused multiply-adds can
  change a last bit ([rdk.md](rdk.md)).
- As built, a record row goes to a callback (`desc.on_row`): the header
  does no I/O, and the program writes the row (to `yds_stream()` or its
  own file).
- A run also writes the graph's definition and a digest of every
  recorded node, as an `rdk` stimulus writes `<name>_digest`.

## 9. C API, sketch

As built (ysp/sig.h v0.1.0), the sketch below changed: `ysig_node` has a
`history` (past values kept for `ysig_value_at()`) and a `data` array
(sines' components do not fit in 16 parameters); `ysig_check()` and
`ysig_arena_size()` take the whole desc (roles, adapters, custom ops and
the channel count are part of the check and the size); `ysig_frame()`
takes a `ysig_frame_in` (index, onset, base time, the timeline's values,
the poll's time)
instead of a `yscr_frame`, so the header needs `ysp/input.h` only; and
`ysig_value_at()`, `ysig_derived()`, `ysig_arm()`, `ysig_set_data()` and
`ysig_replay()` are added.

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
  Windows development machine, with Tracy zones per op. Measured on the
  Linux desktop: [sig.md](sig.md), "Cost" (no Tracy zones yet; the
  Windows machine not run).

## 10. Experiment definition

A template gains a `signals` object: node name to node, in the JSON
form of section 5. A stimulus property takes `{"signal": "cur.x"}`. A
step takes `{"until": "detector name"}`, and a response choice takes
`{"signal": "detector name"}` (section 7, way (a)). Parameters take
`$variables`, set at the trial's reset.

The definition's top level gains `roles`: name to type and unit space
(section 5.5), for example `"hand": {"type": "position2d", "units":
"workspace_cm"}`. The graph reads a role with `{"role": "hand"}`. The
definition has no device nodes unless a role's units are `device`; the
rig profile supplies the adapters.

Authors work in components that expand to these nodes (section 15), and
the graph is the advanced view.

This is a change to D7 (the three-part template), so it is format
version 2 (D12). It keeps "a data-only experiment has no script": a
graph is data. The four worked examples of experiment_format.md section
2.14 do not change.

## 11. Open questions

Decided for the build of 2026-11-07: the name `ysp/sig.h`; detectors both
ways ((b) `detect` sinks for flow events, (a) an `emit` node's derived
events for responses, without a new flag: section 14); 4 lanes, `pose`
left out; prediction left out; roles and adapters on the C side, the rig
profile's block not built; the stream sample's struct defined, not read;
custom ops through a vtable. The questions below stay open where they
were not decided.

- **The name.** `ysp/sig.h` avoids the standard `<signal.h>`; "control"
  is already the axis field of `yin_event`.
- **Detectors: way (a), (b) or both** (section 7). (a) needs a
  `YIN_DERIVED` flag and a rule for the tier in `ysp/input.h`.
- **Lanes.** Four lanes cover position, pressure and tilt. A `pose`
  role (section 5.5) needs 7 (position and a quaternion): a wider value,
  or a pose as two linked values?
- **The rig profile's adapter block** (section 5.5). Its schema in
  devices_spec section 11.2: the device, the adapter's nodes, the
  counts per inch or the tablet's size, the calibration. The profile's
  hash changes with it, as with the `view` block.
- **The calibration procedure**: which points, which fit (affine or
  rigid), the residual that fails it, and how often a rig repeats it.
- **The role types.** Is the list in section 5.5 enough? A
  `position2d_relative` role and the recentering it needs: does the
  definition design the recentering step, or does the adapter give a
  standard one?
- **The stream sample** (section 5.5): its struct, its ring, and
  whether `ysp/net.h`'s LSL inlets post it.
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

Steps 2 to 4 are built on the C side (ysp/sig.h v0.1.0): step 4 without
the rig profile's adapter block, with a mouse and a tablet adapter in
`trial_visuomotor` (`--pen`).

1. Settle section 11's detector question and the name.
2. The header with sources, `integrate`, `affine`, `select`, `join`,
   `bind` and `record`; rewrite `trial_visuomotor` on it.
3. Filters, `speed`, `detect`, `sines`; rewrite `trial_bimanual`.
4. Roles and adapters (section 5.5): the rig profile's adapter block,
   a mouse and a tablet adapter, and `trial_visuomotor` run with each
   from one definition.
5. The loader in the player and format version 2.
6. The stream sample and a motion-capture adapter with its calibration
   procedure, on a rig with a tracker.
7. Eye tracker and gamepad sources with a device on a rig.

## 14. As built: ysp/sig.h v0.1.0

Where the header differs from the sections above, and why. Each was
needed by a ported example or found by a measurement.

| Section | Draft | Built | Why |
|---|---|---|---|
| 5.2 | `join` takes 1 + 1 | 1 to 4 inputs of any lanes, at most 4 out | `trial_visuomotor` records the hand and the cursor as one row |
| 5.2 | `detect` has no output | One lane, the test's state | `trial_bimanual`'s target is yellow while the cursor is inside: a state, not an event |
| 5.2, 7 | Way (a) needs `YIN_DERIVED` | An `emit` node; kind USER + p0, device the node's index, code the sources of the newest and oldest input; no flag | The collector needs no flag; ysp/input.h is unchanged. The flag stays proposed ([sig.md](sig.md), "Not done") |
| 5.2 | `affine` has a matrix and an offset | A center subtracted first (pre), an offset added after (post), an angle in degrees, and an optional angle input | A rotation about the start; the angle input is principle 3's mid-trial rotation |
| 5.2 | `scale` per lane | A second input, a factor for every lane | The tracking target's ramp is a timeline tween: sines times a channel |
| 5.2 | Parameters in 16 doubles | Also a data array per node, copied at open | 7 + 7 sines need 42 numbers |
| 5.5 | Four unit spaces | `display_px` added | The examples are designed in window pixels |
| 6.1 | The worse tier | SIM, 1, 2, 3, UNKNOWN from best to worst; a source's tier for stamp 0 is a parameter (the source table's) | ysp/input.h orders no tiers; an unmeasured stamp is the worst |
| 6.1 | A value's `t` is the newer input's | In a sample's pass the changed input's (the arriving sample's) `t`, device and source; in a frame's pass the newest input's; emit and record carry it as is; a detect's hold clock never goes back; `t_oldest` the oldest input's | The derived event must name the report that crossed, at its time; a frame must not move a value's time back; a hold must not end early on a late report |
| 6.1 | Order per source | Per source and device; raw mouse counts never dropped | Two mice on one any-device source dropped each other's counts |
| 6.2 | A sink reads at the onset | Held values at `ysig_frame()`; `ysig_value_at()` from rings of sample-time values for a later onset; views for velocity, speed, delay, low-pass, rate integration up to the poll's time | The flip record comes a frame later (D10); a still mouse sends nothing; a planned onset is in the future |
| 6.2 | | Stateful sample-rate ops need a source upstream | They would never run; a frame-rate form is not built |
| 7 | A hold | A hold armed while its test is true counts from the arm (default) or waits for a fresh entry (a parameter) | The reach task's first hold starts with the hand on the start |
| 6.4 | Parameters apply at the reset | `ysig_set()` applies at once, state kept | A reach task keeps its hand across trials |
| 8 | Replay from the source rows | A journal of every call; bit for bit on one build | Frames, channel values, arms and sets change values too |
| 8 | Rows to `yds_stream()` | A callback | No I/O in the header |
| 9 | `ysig_check(nodes, n)`, `ysig_arena_size(nodes, n)`, `ysig_frame(g, yscr_frame*, base)` | Both take the desc; `ysig_frame(g, ysig_frame_in*)` | Roles, adapters, custom ops and channels are part of the check and the size; ysp/input.h is the only dependency |

## 15. Paper test: four tasks as definitions

Status: a design exercise, 2026-11-08. No code reads these definitions.
The aim is to find what a designer user must set for a continuous task,
and what the format cannot express yet. Designer authoring is the
baseline; the C and Python surfaces come after it.

### 15.1 The proposal under test

A template gains `controls`: name to component. The designer shows
components. Each one expands to graph nodes at load, as a step expands
to timeline ops. The graph of section 5 stays as the advanced view.

| Component | The author sets | Expands to |
|---|---|---|
| `cursor` | the role or roles, a transform, the stimulus | `role`, `select`, `join` or `mean`, `affine`, `bind` |
| `region` | what it tests, a shape, `stay_s` | `detect` |
| `movement` | what moves, the onset distance | `detect`, `emit` |
| `moving_target` | the sines, the stimulus | `sines`, `scale`, `bind` |
| `trace` | what to record, at which rate | `join`, `record` |

Steps gain `{"until": CONTROL}`: the sequence waits for that control's
hit. Components that a task needs and this list lacks are findings
(section 15.7). The definitions below use them and name them in
**bold** in section 15.6's table. They show only the keys that change;
stimuli and tables are as in experiment_format.md.

### 15.2 Visuomotor rotation (`trial_visuomotor`)

```json
{
  "roles": { "hand": { "type": "position2d", "units": "workspace_cm", "allow_relative": true } },
  "trials": { "reach": {
    "controls": {
      "cursor":      { "control": "cursor", "role": "hand", "stimulus": "dot",
                       "rotation_deg": "$rotation_deg", "about": [0, 0] },
      "near_start":  { "control": "region", "of": "hand", "center": [0, 0], "radius": 2.7 },
      "at_start":    { "control": "region", "of": "hand", "center": [0, 0], "radius": 0.5, "stay_s": 0.5 },
      "past_target": { "control": "region", "of": "hand", "center": [0, 0], "outside": 10.8 },
      "aid":         { "control": "distance", "of": "hand", "from": [0, 0], "stimulus": "ring", "property": "size", "scale": 2 },
      "moved":       { "control": "movement", "of": "hand", "onset": 1.0, "from": "at_start" },
      "path":        { "control": "trace", "of": ["hand", "cursor"], "rate": "sample", "from": "at_start", "to": "past_target" }
    },
    "events": [
      { "set": "dot", "props": { "visible": { "signal": "near_start" } } },
      { "on": "ring" }, { "until": "at_start" }, { "off": "ring" },
      { "set": "dot", "props": { "visible": { "by": "$feedback", "map": { "online": true, "endpoint": false, "none": false } } } },
      { "on": "target" }, { "until": "past_target" }
    ],
    "response": { "choices": [{ "signal": "moved" }], "from": "target", "response_ends_trial": false },
    "after": [
      { "freeze": "cursor" },
      { "set": "dot", "props": { "visible": { "by": "$feedback", "map": { "online": true, "endpoint": true, "none": false } } } },
      { "wait_s": 0.5 }, { "off": "target" }
    ],
    "columns": { "hand_end_x": { "hit": "past_target", "lane": "x" }, "hand_end_y": { "hit": "past_target", "lane": "y" } }
  } },
  "timeline": [
    { "name": "baseline", "timeline_variables": "targets", "rules": ["order random", "reps 5"], "trial": "reach",
      "vars": { "rotation_deg": 0, "feedback": "online" } },
    { "name": "rotation", "timeline_variables": "targets", "rules": ["order random", "reps 10"], "trial": "reach",
      "vars": { "rotation_deg": 30, "feedback": "online" } },
    { "name": "washout", "timeline_variables": "targets", "rules": ["order random", "reps 5"], "trial": "reach",
      "vars": { "rotation_deg": 0, "feedback": "none" } }
  ]
}
```

The hand angles (at a third of the radius, at peak speed) are not
columns. They are arithmetic on the trace, so they belong to the
analysis. The hand keeps its place across trials (finding 12).

### 15.3 Two-mouse tracking and reaching (`trial_bimanual`)

```json
{
  "roles": { "left": { "type": "position2d", "units": "workspace_cm", "allow_relative": true },
             "right": { "type": "position2d", "units": "workspace_cm", "allow_relative": true } },
  "trials": {
    "track": {
      "controls": {
        "cursor": { "control": "cursor", "x": "left.x", "y": "right.y", "stimulus": "dot",
                    "transform": { "by": "$perturb", "map": { "none": "identity", "rotation": { "rotation_deg": -90 }, "mirror": "swap_xy" } } },
        "goal":   { "control": "moving_target", "stimulus": "target", "amplitude": 0,
                    "x": { "amps_cm": [2.31, 2.31, 2.31, 1.76, 1.30, 0.97, 0.73], "freqs_hz": [0.10, 0.25, 0.55, 0.85, 1.15, 1.55, 2.05] },
                    "y": { "amps_cm": [2.31, 2.31, 2.31, 1.58, 1.03, 0.81, 0.70], "freqs_hz": [0.15, 0.35, 0.65, 0.95, 1.45, 1.85, 2.15] },
                    "phase": { "random": [-3.14159265, 3.14159265], "per": "block" } },
        "inside": { "control": "region", "of": "cursor", "center": { "signal": "goal" }, "radius": 0.8 },
        "ready":  { "control": "region", "of": "cursor", "center": [0, 0], "radius": 0.8, "stay_s": 0.2, "speed_below": 6.5 },
        "frames": { "control": "trace", "of": ["goal", "cursor", "left", "right"], "rate": "frame" }
      },
      "events": [
        { "on": ["target", "dot"] }, { "until": "ready" },
        { "set": "target", "props": { "color": { "when": "inside", "then": [1, 1, 0], "else": [0.5, 0.5, 0.5] } } },
        { "to": "goal", "props": { "amplitude": 1 }, "duration_s": 5 }, { "wait_s": 41 }
      ]
    },
    "reach": {
      "controls": {
        "cursor": { "control": "cursor", "x": "left.x", "y": "right.y", "stimulus": "dot",
                    "transform": { "by": "$perturb", "map": { "none": "identity", "rotation": { "rotation_deg": -90 }, "mirror": "swap_xy" } } },
        "left_start": { "control": "movement", "of": "cursor", "onset": 1.0, "from": "target" },
        "top_speed":  { "control": "peak", "of": "cursor", "measure": "speed", "from": "left_start" },
        "arrived":    { "control": "region", "of": "cursor", "center": ["$goal_x", "$goal_y"], "radius": 1.0, "stay_s": 1, "speed_below": 6.5 },
        "path":       { "control": "trace", "of": ["cursor", "left", "right"], "rate": "sample" }
      },
      "events": [
        { "wait_s": 0.5 }, { "on": "target" }, { "until": "arrived" },
        { "set": "target", "props": { "color": { "when": { "signal": "top_speed", "above": 39 }, "then": [1, 1, 0], "else": [0, 0, 1] } } }
      ],
      "response": { "choices": [{ "signal": "left_start" }], "from": "target", "response_ends_trial": false },
      "columns": { "peak_speed_cm_s": { "signal": "top_speed" } }
    }
  },
  "timeline": [
    { "assign_roles": ["left", "right"], "by": "click" },
    { "name": "baseline_track", "trial": "track", "rules": ["reps 2"], "vars": { "perturb": "none" } },
    { "name": "baseline_reach", "trial": "reach", "rules": ["reps 30"],
      "vars": { "perturb": "none", "goal": { "walk2d": { "step_cm": 12, "box_cm": [20, 20] } } } }
  ]
}
```

The default block list repeats those two nodes with `perturb` set. The
frequency analysis and the alignment matrix use the `frames` stream, in
the analysis.

### 15.4 Mouse tracking (`trial_mouse_tracking`)

```json
{
  "roles": { "pointer": { "type": "position2d", "units": "display_px" } },
  "trials": { "choose": {
    "controls": {
      "moved": { "control": "movement", "of": "pointer", "onset": 10, "from": "start_click" },
      "path":  { "control": "trace", "of": ["pointer"], "rate": "sample", "from": "start_click" }
    },
    "events": [
      { "on": ["start_button", "box_left", "box_right"] },
      { "until": { "click_in": "start_button", "name": "start_click" } },
      { "off": "start_button" }, { "on": "disc" }
    ],
    "response": { "role": "pointer", "from": "disc",
                  "choices": [{ "click_in": "box_left", "name": "red" }, { "click_in": "box_right", "name": "blue" }],
                  "key_answer": "$color" },
    "after": [{ "off": ["disc", "box_left", "box_right"] }, { "wait_s": 1 }],
    "columns": { "init_time_s": { "signal": "moved", "from": "disc" } }
  } }
}
```

`max_deviation` is arithmetic on the trace: the analysis.

### 15.5 Gaze-contingent window (`gfx/gaze_contingent`)

```json
{
  "roles": { "eye": { "type": "gaze", "units": "display_px" } },
  "trials": { "view": {
    "controls": {
      "window": { "control": "cursor", "role": "eye", "stimulus": "sharp", "lowpass_hz": 30, "also": ["source_x", "source_y"] },
      "lost":   { "control": "signal_lost", "of": "eye", "after_s": 0.1 },
      "gaze":   { "control": "trace", "of": ["eye"], "rate": "sample" }
    },
    "events": [
      { "on": ["blurred", "sharp"] },
      { "set": "sharp", "props": { "visible": { "when": "lost", "then": false, "else": true } } },
      { "wait_s": 10 }
    ]
  } }
}
```

### 15.6 What an author sets

| Task | Templates | Controls | Values set in controls | Steps | Components not in the list |
|---|---|---|---|---|---|
| Visuomotor rotation | 1 | 7 | 27 | 11 | **`distance`** |
| Two-mouse tracking and reaching | 2 | 10 | 51 | 9 | **`peak`** |
| Mouse tracking | 1 | 2 | 6 | 6 | none |
| Gaze-contingent window | 1 | 3 | 9 | 3 | **`signal_lost`** |

A numeric list (a point, a color, the amplitudes) counts as one value.

Bold marks a component or key that is not in section 15.1's list.

### 15.7 Findings

The five components carry the signal path of all four tasks. The missing
parts are mostly trial state. None of the four tasks needs a script.

| # | Need | Seen in | Proposal | Lands in |
|---|---|---|---|---|
| 1 | A boolean signal as a stimulus property, set by a step | visuomotor (`visible`), gaze | `{"signal": NAME}` as a value for a step's `set`, which replaces the property's source until the next `set` | format |
| 2 | A property chosen per frame from a boolean | tracking (`color`), reach, gaze | `{"when": TEST, "then": V, "else": V}` | format; a `select`-like op |
| 3 | Freeze a cursor at a detector's value | visuomotor (endpoint feedback) | `{"freeze": CONTROL}` step, and the hit's value in place of the live value | format; an op or a `bind` mode |
| 4 | Distance from a point as a value | visuomotor (the return ring) | **`distance`** component; a `distance` op (now only inside `detect`, lane -1) | sig.h |
| 5 | The largest value over a window | bimanual (peak speed) | **`peak`** component and op, with a reset at a control's hit | sig.h |
| 6 | Two tests at once | bimanual (in the target and slow) | `speed_below` on `region`; an `all` op | sig.h |
| 7 | A region centered on a moving signal | tracking (`inside`) | `center` takes `{"signal": NAME}`; expands to `diff` and `detect` | component |
| 8 | Random values drawn per block | tracking (phases; F23) | `{"random": [lo, hi], "per": "block"}` in a control, or vars per block | trials.h, format |
| 9 | A target chained from the last target | bimanual reach | `{"walk2d": ...}` draw in `vars`. It depends on the last target, not on the hand, so it is known before the trial and is not a script | trials.h, format |
| 10 | A press inside a stimulus | mouse tracking (start button, boxes) | `{"click_in": STIMULUS}` as a choice and in `until`; the hit test with a press's position | format; response.h or the player |
| 11 | A setup step that binds devices to roles | bimanual (click each mouse) | `{"assign_roles": [...], "by": "click"}` node; a rig profile may bind by device instead | format, rig profile |
| 12 | A role that keeps its state across trials | visuomotor, bimanual (the hand stays where it is) | `"keep": true` on a role: no reset of its adapter between trials | format; `ysig_reset` |
| 13 | Loss of a signal | gaze (track loss) | **`signal_lost`** component: no sample for a time, or the eye's PROXIMITY | sig.h |
| 14 | Units in cm on the display | visuomotor, bimanual | `display.units` gains `cm`, from the rig's `view` | format |
| 15 | A column from a control's hit or value | visuomotor, bimanual, mouse tracking | `{"hit": NAME, "lane": L}`, `{"signal": NAME}` as a column value | format |

Findings about the test itself:

- **The count does not fall.** Visuomotor's controls set 27 values, the
  same as the 27 parameters its C graph sets, and they also cover the
  return ring and the trace, which the C program does outside the graph.
  The gain is in the words an author uses, not in how many values.
- **Repeated controls.** `trial_bimanual`'s two templates repeat the
  same `cursor` (7 values). A definition needs named controls shared by
  templates, as `stimuli` are.
- **Section 3.2 stands.** "A target placed from where the hand stopped"
  is still a script. `trial_bimanual`'s chain is another case: it depends
  only on the last target (finding 9).
- **`from` names three kinds of thing:** a control (`at_start`), a
  stimulus's onset (`target`) and a named click (`start_click`). The
  loader must resolve them in one namespace, and the designer must offer
  them in one list.
- **A control as a step's target.** `{"to": "goal", ...}` tweens a
  control's parameter, so a step must address controls as it addresses
  stimuli.
- **A response whose choices are signals has no role.** Section 2.6 of
  the format requires one; it becomes optional for these choices.
- **Arithmetic stays in the analysis.** Hand angles, the frequency
  response and `max_deviation` do not need the definition, unless a
  task gives feedback from them. A task that scores a reach by its angle
  online needs a `heading` op or a script. None of these four does.
- **Words a designer user must learn:** role, control, `until`, `from`,
  and the difference between a control's hit and its value. The op vocabulary (`affine` with `pre_x`,
  `emit` kind, `detect` lane -1, `join` for a trace) does not appear.

### 15.8 What this changes in the order of work

Before format version 2, decide findings 1 to 3 and 12 with the player's
sequence model: they change what a step can do, so they are the part of
D7 that this work reopens. Findings 4 to 7 and 13 are small additions to
`ysp/sig.h`. Findings 8 and 9 belong with F23 in `ysp/trials.h`.
