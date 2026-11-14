# The player

Status: v0.10.0, 2026-11-13. It loads a pack, checks its experiment
definition, lowers the definition onto the ysp headers and runs it: in a
real window with the participant's keyboard and the rig's devices, or on
the simulated display (`--sim`). It is a program (`player/`, target
`ysp_player`, option `YSP_BUILD_PLAYER`), not a single header. It reads
format versions 1 and 2 ([experiment_format.md](experiment_format.md), D1
to D15 and part 3). Version 2 runs on `--sim` only when it has a signal
role: the rig profile has no adapter block yet (G18), so a real run refuses
the role by name. Real runs are measured on the Linux X11 desktop only
(Results). The runner protocol and `ysp/app.h`'s loop (rig_spec 4.2.1) are
not built.

## How-to: run a session

Build the pack with `ypak build`. Run the player from a folder that is not
the source tree (it writes a new data set folder, `DIR`, which must not
exist):

```sh
ysp_player PACK.ysppak --out DIR --seed 7 --field participant=1
```

1. The player loads the rig profile: `--rig FILE` (a path) or `--rig NAME`
   (a profile in the user's rig folder, ysp/rigfile.h "WHERE IT LIVES").
   With no `--rig`, it takes the folder's `profile.json` when there is one.
2. It binds each role of the definition (2.9). An input role that the rig
   does not bind and that lists the family `keyboard` takes the keyboard.
   A required role with no device and no keyboard stops the start, with
   the role's name.
3. It checks each bound role's `min_tier` (2.9), before any device or the
   display opens, so a refusal never sets full screen. A role's tier comes
   from the rig profile: tier 1 for an input with a checked bounds
   loopback in the profile, else UNKNOWN, which is below every
   `min_tier`. An output role is UNKNOWN: ysp/rigfile.h gives no output a
   tier yet. A role below its `min_tier` stops the start, with the role's
   name and tiers. `--allow-below-tier ROLE[,ROLE]` starts anyway for the
   roles it names; every other role keeps its check. `session.json` then
   has `"allow_below_tier"` (the list) and each such role `"below_tier":
   true`. A value that starts with `-`, an empty entry (`trig,`) or a
   name that is not a role of the definition is a bad argument: exit 2,
   in `--sim` too. In the same step it checks the response rules of the
   bound device roles (the same choices, codes and fires a trial on one
   role; see "Explanation: what lowering decided"), so that refusal sends
   no byte to a trigger box either.
4. It opens each bound device and waits up to 3 s for it to run. A device
   that does not answer as its family must stops the start.
5. It opens the display that `YSCR_BACKEND_AUTO` picks (GLX_OML on X11),
   full screen, with the panic watchdog armed, and compiles the programs
   into the user's program cache (ysp/gfx.h, PROGRAM CACHE).
6. It shows 30 blank frames (about 0.5 s). If the definition sets
   `display.min_tier` and these frames are of a worse tier, the start stops
   here, before the data set folder exists.
7. It prints `ysp_player: running; Shift+Esc stops the session` and runs
   the session. `session.json` names the rig profile file it used
   (`rig_profile`; `null` with none).

To stop the session, press Shift+Esc. The trial that runs has no row; each
finished trial keeps its row; `session.json` gets `"aborted"`; the exit
code is 3. If the program hangs, press Shift+Esc three times within 2 s:
the watchdog puts the display's mode and gamma ramp back and ends the
process (exit code 99), and `panic.json` beside `session.json` says which
trial ran.

`--windowed` runs in a window, for development: a window's flips are tier
2 at best, so a definition with `display.min_tier` 1 does not start in
one.

Linux: key times come from the kernel's evdev time when the user can read
the keyboard's node (`tools/linux/70-ysp-input.rules`, docs/screen.md
"Kernel key times"); else from the X server's millisecond clock (tier 3).

## How-to: give a keyboard a tier

You cannot yet. A role's tier comes only from the rig profile (rig_spec
8; ysp/rigfile.h, TIER), never from the kind of stamp, a polling rate or
a product name. A keyboard role is unbound in the rig profile, so its
tier is UNKNOWN, and a keyboard role with a `min_tier` needs
`--allow-below-tier`. These are missing:

1. The rig profile cannot bind a keyboard: a role's `family` must be a
   ysp/box.h family or `lsl` (`yrig__family_id()`). It needs a family
   such as `keyboard`, with a key that names one keyboard (its evdev
   `by-id` name on Linux, a USB vid:pid and serial number elsewhere).
2. ysp/rigfile.h gives tier 1 only to a family that maps a device clock
   (xid, line, photo, lsl). A keyboard has no device clock: its times are
   the host's (the kernel's evdev time). It needs a rule for a host-stamped
   input whose bounds a loopback checked.
3. The loopback. `examples/rigfile/bounds.c` measures an input against a
   line board's output pin, but only for a ysp/device.h role. For a
   keyboard it needs: a line board (`firmware/ysp_line/`) whose output
   drives the key, through a relay or an optocoupler across the key
   switch or a solenoid on the keycap; and a mode that reads the key's
   time from the screen's input (`yscr_poll()`, the evdev time) and
   writes the same `format ysp-in-bounds 1` file (`n`, `lo_s`, `hi_s`,
   `date`) into the profile's `loopback/` folder, its SHA-256 in
   `roles.<role>.bounds`.

With those three, the keyboard role would get its tier from
`yrig_source_role()`, as the player already asks.

An output role has the same gap. ysp/rigfile.h's rule (`yrig__tier()`,
TIER) gives tier 1 only to an input family with a device clock and a
checked `bounds` file; it reads an output role's `latency` file, and
checks its SHA-256, but gives no output a tier (devices_spec.md 11.2: "No
output has a tier"). The measurement and the file exist:
`examples/device/out_latency.c` writes `format ysp-out-latency 1` (n, p5,
median, p95, max of write start to edge) and the profile stores it in
`roles.<role>.latency`. What is missing is the rule: devices_spec.md 8
proposes tier 2 with bounds for a host write after a write-to-edge
loopback, and 1 for a video-locked output (VPixx) after one. That is a
decision and a change to ysp/rigfile.h, so an output role stays UNKNOWN
here, and `--allow-below-tier` names it.

## How-to: run a pack on the simulated display

```sh
ysp_player PACK.ysppak --out DIR --sim --seed 7 --field participant=1
```

`--sim` selects the simulated display, the virtual clock, the null audio
device and a scripted participant. The same inputs give byte-identical
data.

A definition in `deg` units or with `"calibration": "required"` needs a
rig profile with a `view` and a calibration: `--rig FILE` (and `--cal FILE`,
or a calibration named in the profile). Test profiles come from
`player_make_rig DIR`.

## How-to: run a version 2 definition on the simulated display

A definition with signal roles (3.4) needs a scripted participant for each
role. Give the player a samples file and a scale per role:

```sh
ysp_player PACK.ysppak --out DIR --sim --seed 3 --field participant=1 \
    --rig RIG/profile.json --samples samples.csv --sim-gain hand=0.25
```

1. Write the samples file as CSV with the header `t_s,role,x,y`. Each row
   is one report of a raw mouse (`x` and `y` are counts) or of a pen (`x`
   and `y` are positions). `t_s` is seconds from the first frame's onset.
   Rows are in time order.
2. Give `--sim-gain ROLE=UNITS` for each relative role
   (`position2d_relative`). The `--sim` adapter integrates the counts at
   that many role units a count. An absolute role multiplies the position
   by its gain (1 by default). A relative role with no gain is a bad
   argument (exit code 2). Reading F32 of experiment.md.
3. For `display_cm` or `cm` units, give a rig profile with a `view` (its
   width in mm). `player_make_rig DIR --view 1024 320` writes one: 1024 px
   over 320 mm.

With no samples file, each role stays at its origin. A definition with no
signal role (states and exits only) needs no samples.

To compare with a C program, the C program must see the same draws and
the same samples:

1. `ysp_player PACK --out DIR --sim ... --test-draws draws.csv` writes every
   leaf's trials and variables, and runs no frame.
2. The C program reads them (`--test-order draws.csv`) and writes the
   samples it fed its own graph (`--test-samples-out samples.csv`).
   `trial_bimanual` also takes `--design-timing`: the definition's 1 s
   inter-trial interval and graph speed.
3. `ysp_player PACK --out DIR2 --sim ... --samples samples.csv` runs the
   definition on those samples.

`tests/player/compare.py` does these steps for `visuomotor` and
`bimanual`. The options are for tests only.

Two more test options, `--sim` only, make the simulated display's flip
records late: `--test-record-late S` adds S seconds to every record's
onset, and `--test-record-delay N` (0 to 8) hands each record to the player
N frames later. `player_test.py` uses them to check the time columns and
the frame traces against records that are not the plan.

## How-to: profile a run

Use a build with the trace ring. The build adds zones to the player and
the examples; the default build has none.

1. Configure and build:

   ```sh
   cmake --preset linux-trace-ring -DYSP_BUILD_PLAYER=ON -DYSP_BUILD_PACK=ON
   cmake --build build/linux-trace-ring --target ysp_player
   ```

   The preset sets `YSP_TRACE_RING=ON`. `YSP_TRACY=ON` (preset
   `linux-tracy`) builds Tracy's client instead, on demand: it sends
   nothing until the Tracy profiler connects. One build has one mode;
   CMake refuses both options together.
2. Run with `--profile` (and `--measure` for the largest frame):

   ```sh
   ysp_player PACK --out DIR --sim ... --profile --measure
   ```

   The player writes `DIR/profile.csv`: `frame,zone,t_ns,dur_ns,value`, one
   row a zone. A build with no trace ring refuses `--profile` (exit code 2).
3. Summarize it:

   ```sh
   uv run --no-project python tools/profile_summary.py DIR/profile.csv
   ```

   The tool prints, per zone, the count, median, p99 and largest time in
   us. Then it prints the zones in the largest frame (`pl.frame`) and in
   the largest trial start (`pl.trial_start`), with their totals and
   shares. A zone's time includes the zones in it.

The zones:

| Zone | What it times |
|---|---|
| `pl.frame` | `pl_frame()`, all the frame's work |
| `pl.devices_drain`, `pl.flip_records` | The device records; the flip records (`value`: the records) |
| `pl.trial_start` | The trial's start. In it: `pl.leaf_open` (a leaf opens), `pl.trial_next` (the trial's draws), `pl.stim_begin`, `pl.timeline_reset`, `pl.build_ops` (version 1's lowering) or `pl.v2_trial_start` (with `pl.v2_params` and `pl.v2_enter`, the first state's lowering) |
| `pl.feed_screen` | `yrsp_feed_screen()` (a real run) |
| `pl.v2_input`, `sig.samples` | Version 2: the samples to the graphs |
| `pl.update_frame`, `pl.evaluate`, `pl.landing` | `yrsp_update_frame()`, `ytl_evaluate()` (`value`: the events), what landed |
| `pl.v2_frame`, `sig.frame`, `pl.v2_frame_rows` | Version 2: hits and exits; `ysig_frame()` (`value`: the nodes); the frame traces |
| `pl.trial_end` | The trial's end. In it: `pl.v2_trial_end`, `pl.row_fill`, `pl.v2_trial_rows` (the sample traces), `pl.trial_update` |
| `pl.rule_window`, `pl.apply`, `pl.draw` | The rule's window; the channels to the stimuli; the draw |
| `pl.write_inplace` | `--sim` only: one write to the data set (a real run gives it to the writer) |

ysp/screen.h and ysp/gfx.h add their own zones (`yscr.*`, `ygfx.*`).
ysp/sig.h has none: it does not include ysp/rt.h. The player's `sig.*`
zones time its calls.

`--profile` writes the file on the frame thread after each frame, outside
`pl.frame`. On a real rig, the write can make the next frame late. It is
a debug option: do not use it for a timing run.

## How-to: choose the participant

`--participant SPEC` gives the scripted participant of `--sim`. It is
outside the definition, so the definition does not change for a real run.
A real run refuses `--participant` and `--rt` (exit code 2).

| SPEC | Effect |
|---|---|
| `correct` (default) | Presses `key_answer` on every trial, `--rt` seconds (default 0.3) after the `from` stimulus's flip |
| `silent` | Never presses |
| `p=0.8` | Presses `key_answer` with that probability, else the next choice (drawn from a stream of the session seed) |
| `script:FILE` | A CSV of `trial_index,response,rt_s,hold_s` (a response name or an empty field). A trial with no row is silent; a file with only the header makes every trial silent. Several presses a trial (to test `max_responses` above 1): consecutive rows with one trial index, in time order, each `rt_s` from the same `from` onset and at least the row before's `rt_s` + `hold_s`, compared in ns after rounding each value (a key's press follows its release); the player refuses an overlap by name. An empty `response` is a silent trial and that trial's only row |

## Reference: data

The data set is format 2.10. Columns not fixed by the format:
`first_in_block`, `index` (the leaf's trial number; `trial_index` is the
session's, since it joins `frames.csv` and `events.csv`), and `time_elapsed_s`
(the end frame's onset minus the session's first frame's onset).
`rsp_flags` is the result's `YRSP_R_*` flags (bits 0 to 14, ysp/response.h)
and the player's own in the bits the header leaves to its caller (15 to
31): 0x8000 (`PL_RSP_AFTER_LIVE`), `after` was built from another result
than the row's.

`session.json` also has `node_seeds` (each leaf run's path, iteration and
seed, for the first 65,536 leaf runs) and `node_seeds_unlogged` (the leaf
runs after those), `devices` (each role and what it is bound to; `sim` in `--sim`), the
audio describe line and every header's version.

A real run's data set has the same files and columns as `--sim`'s. These
differ:

| Item | Real run | `--sim` |
|---|---|---|
| `events.csv` `out` rows | From the device's own record of each write (`YDEV_REC_OUT`): `t_s` the write's start, `end_s` its end, `deadline_s` the flip's vblank (a code at a flip), or the press (a response's code), `flags` the `YDEV_OUT_*` bits. One row a code: a pulse's trailing edge is in the device's log only | The deadline as the write's time, flags 1 for a response's code |
| `<role>_lag_max_s` | The largest write start minus deadline of the trial's codes: exactly the largest of its `out` rows in `events.csv` | 0 |
| `<role>_n` | The codes the trial asked for, its rule's fires included | The codes asked for |
| `<role>_failed` | The codes whose write failed, and the codes whose device record never came (a trigger whose flip was not shown, a full ring) | The codes the screen refused |
| When the row is written | When its last frame's flip record and every one of its codes' device records are in, on every output channel, or 30 frames (0.5 s at 60 Hz) after its last frame, with the codes still owed counted in `_failed`. A record that comes after that has `flags` 0x4000 (`PL_OUT_AFTER_ROW`) in `events.csv`: its row counted it failed. At the session's end the player shows 8 more frames, and up to 32 (`PL_OUT_WAIT_FRAMES` + 2: the record that passes a row's bound comes up to 2 frames after its flip) while a row still waits for a code's record | When its last frame's flip record is in; 8 frames at the end |
| `session.json` `devices` | `bound` (a family, `keyboard` or `none`), `key`, `port`, `ident`, `tier` (every bound role; an output's is 0, UNKNOWN), `min_tier` and `below_tier` where they apply, and `rule` (the output a device role's rule writes to) | `bound` only (`sim` or the rig's family) |
| `session.json` | `rig_profile` (the file, or `null`), `allow_below_tier`, `gfx` (the program cache's hits), `view_distance_mm` and `view_width_mm` (deg units), `start_utc` (the time), `aborted` after Shift+Esc | `start_utc` fixed |
| `frames.csv` phase columns | The CPU costs | Empty: they would differ between two runs |
| `panic.json` | After a panic only | Never |

Not done in the data set, and why: `clock.csv` is empty (no device clock
fit is logged yet, in either mode); a device's input events other than the
response are not in `events.csv` (as in `--sim`).

### Version 2 data

A version 2 definition adds these to the data set (3.15):

| Item | Content |
|---|---|
| `events.csv` rows with `kind` `state` | One row a state visit: `text` the state, `frame` and `t_s` its entry frame and that frame's onset (the flip record's), `planned_s`, `residual_s`, `end_s` the visit's end (before `t_s` when the visit ends before its entry frame shows: an `over` row, a response's end ns before the frame), `code` the exit's index (-1 the steps ended, -2 `over` after `max_visits`; reading F20). An events template has none (F27) |
| `streams/<trace>.csv` | A sample trace: `trial_index`, `t_s`, a column a lane (`<item>_x`, `<item>_y`), `tier`, `device`; the rows of the trace's window only. A frame trace (`rate: frame`): `trial_index`, `frame`, `planned_s`, `t_s` (the flip record's onset), a column a lane, and `flags` (0.4.0), bits: 1 when `t_s` is empty (the record did not come within 30 frames, or came with no onset, or the session ended first), 2 when the next trial's start came before the record (the values are read at `planned_s`). So 0, 1, 2 or 3. F29 |
| `trials.csv` | The template's `columns` (`signal`, `hit`, `time`, `when` forms), a flag a column (0 or 1), `rsp_role` for a signal response, the five columns of a walk (`W_x`, `W_y`, `W_from_x`, `W_from_y`, `W_deg`) and a uniform's column |
| `session.json` | See below |

Changes to `session.json` from the player 0.2.0 (0.3.0, and later where
it says), for each version:

| Field | Version 1 | Version 2 |
|---|---|---|
| `player_version` | `0.10.0` | `0.10.0` |
| `format_version` | 1, the definition's (it was the player's constant 1) | 2 |
| `headers.sig` | Not written | ysp/sig.h's version |
| `headers.trials`, `headers.gfx`, `headers.experiment` | The new versions (0.8.1, 0.20.0; experiment 0.5.0 from 0.7.0) | The same |
| `signal_roles` | Not written | One object a signal role: `name`, `adapter` (`sim raw mouse` or `sim positions`), `device` (`--samples` or `none`), `type`, `units`, `per_count` (the `--sim-gain`), `calibration_sha256` (empty) |
| `recenter_skipped` | Not written | The `recenter` steps that a graph did not apply (D40) |
| `sig_arms` | Not written | ysp/sig.h's counts of arms in the past, summed over the session's graphs: `caught_up` (run again, exact), `past_off`, `past_far`, `past_full` (fell back to the newest value: inexact), `hits_superseded`, `hits_retracted`, `rows_stale`, `derived_stale`, `past_pinned` (a fallback), `ahead_cancelled` (waiting arms a disarm or a barrier cancelled), `late_input` (input calls with a time before an arm that the player made with the graph's input at or past its time, any footprint: sig.h's reference order puts them after that arm), `unverified` (arms outside the shapes whose result sig.h verifies: docs/sig.md, "Exactness") (docs/sig.md, "An arm in the past: the semantics"). A run stops at a retracted hit, a stale row or event, any fallback (`past_off`, `past_far`, `past_full`, `past_pinned`), or an unverified arm, so those are 0 in a finished session |

No other field changes. A version 1 data set keeps every other byte
(Results).


## Explanation: what lowering decided

Each of these is a place where the format was silent, or where a choice was
forced. The Report section lists them with the evidence.

| Point | Decision |
|---|---|
| D8's node hash | SHA-256 of `ysp-node-seed\0`, `S` (little-endian), the path, `\0`, then the iteration of each node of the chain as u32 little-endian; the first 8 bytes, little-endian. Iteration = starts of that node within one run of its parent, so a skipped node counts and moves nothing else |
| Which node instance a method is | One per session. Two leaves that name one method continue one staircase |
| A channel's value at a trial's start | Every visibility 0; every driven property at the trial's evaluated value (`ytl_open()` with `desc.initial`). A trial cannot inherit the last one's tween |
| A property that depends on the response | Applied when `after` is built (`pl_value_after()`), per 2.7 |
| A stimulus key that varies per trial on `rdk`: `count`, `w`, `aperture`, ... | Refused: the field is opened at load |
| `mark` named `end` | An ordinary mark; the trial's end uses the code `0x7fffffff`, so no name collides |
| `visible` as a key | Ignored: a stimulus that no step shows is never drawn; a shown one's channel owns its visibility |
| A stimulus's keys at a trial start | Only the stimuli the running template touches, since a key can name a variable of another template's leaf only |
| `edge` on `rdk` | `yrdk_desc.edge`; gfx's dots keep their default edge |
| A leaf with tracks and a table, whose template reads a column on a track trial | The run stops at that trial with a named error: a track trial has no row. A finding: the format does not forbid it |
| `$outcome` with no `key_answer` | `responded` (ysp/experiment.h 0.1.3 checks lookups against it); `correct` empty; `ytr_update()` gets the choice index |
| A `0` to a log10 method | `method_value` is floored at `1e-4` before the log, in a log domain (`scale log10` or stair `step_type` log or db), as the C program does. The format has no key for this |
| `loop.until_accuracy` | Correct over all trials of the iteration (timeouts count against it), `0` when none ran |
| Trigger codes | `0..255`, the width of a port byte; another value stops the run |
| A stimulus shown more than once in a trial | `_onset_s` is the first onset, `_offset_s` the last offset, and `_frames`, `_dropped`, `_tier`, `_flags` cover every shown frame. D10 was silent |
| A key that depends on the response, on a property a step drives | Stops at load with a named error: the channel writes the field every frame, so the key would have no effect |
| A `from` that an `if` leaves unshown on a trial | A named error at that trial's start, because the response could never end |
| A tree where no leaf can run (every leaf under an `if` or `unless` that a literal or a session field makes false, or zero repetitions) | Refused at load |
| A loop or repetition over leaves that a run skips | Tree steps are spread over frames (4096 a frame); the walk has no step limit, and each yield frame draws nothing |
| Leaf runs in one session | No limit. `node_seeds` lists the first 65,536 leaf runs and `node_seeds_unlogged` counts the rest, so the log's memory is fixed. A seed the log does not list re-derives from the session seed, the node's path and its iterations (D8) |
| An RDK's steps file | No limit on a trial's length, a response with no deadline included. Each RDK has a buffer of 64 steps (about 1 s at 60 Hz); a full buffer goes to `streams/<name>.csv` in the frame it fills and the trial goes on, numbering its steps on. A run whose buffer flushes every 7 steps writes the same stream and `_digest`, `_updates` and `_seed` as one that never flushes (`player_test.py`, two 300 s trials). A flush costs about 0.70 us per step, so about 45 us per RDK at 64 steps (measured below) |
| Column text | Each text column holds the longest value known at load (at least 64 bytes); a value longer than 4,096 bytes is refused at load |
| An array in `vars` or `data` | Written to its column as one JSON array, numbers in canonical form: `[1,0,0.5]`. A stimulus key reads it as its value |
| A key held past its trial's end | The release goes to the collector that saw the press, in any later trial or template, so the next press counts |
| A frame with no trial (before the first trial, between trials, a yield frame) | Draws nothing |
| A stimulus left on at a trial's end | `<s>_offset_s` is the onset of the trial's last frame |
| One node with the same name in `vars` and `data` | Refused at load |
| `requeue: no_response` on a trial that never gets a response | The trial requeues until ysp/trials.h's history of 4,096 trials is full, then the session stops with a named error that gives the session trial, the leaf's trial and the number of requeues. A real session would need about 4,000 timeouts in a row on one leaf; the operator can stop it sooner. The rows written before the stop stay in the data set |
| A leaf `loop` pass that gives no trials (its track is done) | Ends the loop: no accuracy can change without trials. Each reopen of a looping leaf also counts as a leaf open in the per-frame walk budget, so a frame yields as the walk does |
| Container starts in one session | The loader bounds them from repetitions, table rows and `loop.max`: each start is a walk step, 4096 a frame. A tree that can start its containers more than 10,000,000 times (about 40 s of blank frames at 60 Hz) is refused at load |
| A table column in one leaf and a `vars` name in another | One column, as for a jitter in one leaf and a text in another: each leaf's row writes its own value, and a column whose leaves give different types holds text (a duration as its exact decimal) |
| A channel's trace | `--trace-channels FILE` writes every channel per frame: a test's view, off by default |
| A trial row's timing columns | Written when their frames' flip records arrive (a pending-row ring), so `<s>_offset_s` of a stimulus turned off at the trial's last frame is known |
| `--sim` triggers | The screen's trigger channels are real (`yscr_trigger`, worker thread); the sink counts firings, and the data takes the deadline (the planned vblank) as the write's time, because a real write's start would differ between two runs |
| A required role unbound in `--sim` | The participant and the sink stand in for the device (the check runs against a real run's rig) |
| `display.min_tier` on the simulated display | Flags frames (`BELOW_TIER`); the display is not refused |
| `display.min_tier` in a real run | Refuses the start when the 30 blank frames before the data set opens are of a worse tier (`caps.worst_tier`; open's settle frames are judged but not counted, so the player shows its own). Every flip of the session is flagged `BELOW_TIER` as in `--sim` |
| An input role that the rig does not bind | The keyboard when it lists the family `keyboard`; else unbound (a required one refuses the start). The rig's families have no keyboard, so a keyboard role needs no binding |
| A role's `min_tier` (decided 2026-11-10) | A real start is refused, naming each role, when a bound role's tier is worse than its `min_tier`; UNKNOWN counts as worse. The check runs before any device or the display opens. The tier is the rig profile's rule (`yrig_source_role()`), so a keyboard and every output role are UNKNOWN today (How-to: give a keyboard a tier). `--allow-below-tier ROLE[,ROLE]` starts anyway for those roles only and `session.json` says so. `--sim` writes a line on stderr only |
| The response kind in a real run | The binding's: a device role's choices are its `buttons` (box events, control = the button's code); a keyboard role's are key names. `--sim` keeps the first family's kind |
| An any-key response in a real run | Hears its role's kind only (`yrsp_desc.kinds`): a click that brings the window to the front is no response |
| A response's `trigger` on a device role | A `ydev_rule` on the role's reader thread, its output the trigger role's device. Its codes are fixed when the device opens: the code must be a number or a lookup by `$response` of numbers, and every template that responds on the role with a trigger must give the same choices and codes, in any order, and the same fires a trial (`response_ends_trial`, `max_responses`); else the start stops, naming both, before any device or the screen opens. A template on the role with no `trigger` runs with the rule disarmed, so its presses send nothing; the rule is disarmed at each trial's end. A rule's record goes to the trial whose arm its press follows. Measured on a pty pair: write start minus the press, p50 66.5 us, max 93.9 us (Results) |
| A response's `trigger` on a keyboard role | `ydev_out_mark()` from the frame loop when it sees the response, up to a frame after the press (2.6). The write runs on the frame thread: 2.7 to 8.9 us on the pty TriggerBox (28 writes); ysp/device.h says an XID output can wait up to 2 ms for a timer query, which a 16.7 ms frame holds. No XID was measured |
| When a response ends | Only once the landing frame's flip record is in, shown or not (`yrsp_set_wait_final()`, ysp/response.h 0.9.0), for every response; a safety net of 1 s from the onset if no record comes. The row's response columns are the final result's. If the live result `after` was built from named another response (a box's report read late with an earlier stamp, or the safety net), `rsp_flags` has 0x8000 (`PL_RSP_AFTER_LIVE`, the player's bit) |
| The collectors | One array, one collector per template with a response; `yrsp_feed_screen()` gives every one every input, between trials too, so none misses a release |
| Where a real run's data is written | A writer thread (`yrt_worker`, SCHED_OTHER) takes each frame's records, events, stream steps and rows from a queue of 8192 writes, every 2 ms. The frame thread only stores the queue's end: it takes no lock of the writer's, so a SCHED_FIFO frame thread cannot wait on a SCHED_OTHER writer. On ext4 a row's `write()` from the frame thread blocked one frame for 17.8 ms while the kernel wrote the file back (Results); none on tmpfs. `--sim` writes in place, so its files are the same bytes as before. A row reaches the OS within 2 ms of being final |
| A real run's first frame | The 30 blank frames come first, off the data set: a frame after a long setup is often late (ysp/gfx.h, PRIME) |

The variable `$trial_index` is the leaf's trial number (2.3 reads it from
`ytr_trial_info`); the column `trial_index` is the session's.

These are not lowered, and the player refuses each by name: `aep` methods,
`artwork` stimuli, `video` stimuli, and a `quest` with more than one
stimulus axis (`$level` and `method_value` carry one value; the fuzz target
found the update reading past it, 2026-11-10). No worked example needs them, and no
C program gives a comparison for them. A version 2 definition with a
signal role is refused in a real run (G18); on `--sim` it runs.
`session.json`'s `format_version` is the definition's (1 or 2). In `--sim` a response's `trigger` goes
out from the frame loop, as for a keyboard role; a real run's device role
has the rule.

## Explanation: how version 2 lowers

A version 2 template lowers to these parts (`player/v2.c`):

| Format | Lowering |
|---|---|
| Signal roles (3.4) | One ysp/sig.h graph a template. Every sample goes to every graph, so an adapter's state (an integrated mouse) stays continuous across templates. `--sim` adapters: a relative role is a raw mouse's counts through an integrate node at `--sim-gain`; an absolute role is a pen's positions through a scale node (F32) |
| Controls (3.7): `cursor`, `region`, `distance`, `movement`, `peak`, `moving_target`, `trace`, `signal_lost` | Nodes of the graph (`v2_ctl_build()`). A region with no `stay_s` arms with entry `now` (ysp/sig.h 0.3.0, G16); a held one waits. `freeze_at` latches the cursor at the hit. A cursor's `transform` is an affine node whose angle and matrix `ysig_set()` changes per trial (G17) |
| Stimulus keys that take a signal (`{"signal": ...}`, `when`, `test`, `hit`) | A selector channel per key: the graph's value goes to a bind slot after the timeline's values, and the stimulus's bind reads that slot (`v2_forms_graph()`) |
| `states`, `exits`, `until` (3.11, D20, D35) | Runtime states with exits of four kinds: a hit, the response, `after_s`, and an instant `if`. A state's steps split at each `until` into one runtime state a part (F3); its `after_s` exits count from the visit's entry frame in every part, an `until`'s timeout from the wait's start. Either is taken no earlier than its state's entry, the wait's for a timeout (F41, decided 2026-11-13, "clamp to entry"): a between-frames entry can sit on a frame whose onset is before it. Each part's steps are one timeline op table from its entry time (`v2_enter()`); an `at_s` there counts from that entry (3.11 rule 6; after an `until`, from the until's hit, since rule 9 makes the steps after it the next state), an events template's from the trial's start. The steps end at the cursor (rule 7): a step that a backward `at_s` put after it never lands. Exits are taken in rule 4's order (`v2_hits()`, `v2_timed()`). A timed exit (`after_s`, an until's `timeout_s`) is never taken on its state's entry frame, the wait's own for a timeout (F40, decided): it keeps its time and lands on the later of its lead-rule frame and the next frame. A hit that an arm makes at once is taken before the frame's evaluate, whatever started the state (a timed exit, a steps' end, the response's end, a hit), so at the hit's time the exit wins (F38). A state's exits count hits from the visit's entry, in every part; an arm drops the hits of the node's earlier arm that a drain still holds. An `until` on a control that the state's exits also name arms a detect of its own, so the exits' hold is not restarted (F3). An until's timeout has the code after the state's own, shared and added exits (F28). The response (F34, decided 2026-11-12, "wait like version 1"): the exit that `response_ends_trial` adds does not keep a state with no written exits from going on at its steps' end; a press or the deadline takes the `on: "response"` exits (own, shared, the added one), the deadline with outcome `timeout` unless the exit gives one; states that reach `end` while the window is open (its `from` shown in this trial, or still due by any visit's step, and the response not ended) wait for the response's end, then the trial ends. A `from` never shown opens no window. The response's end is the entry that ended the window (the press with `response_ends_trial`, the `max_responses`-th press or crossing; its release with `wait_for_key_release`), else the window's end, and the exit is taken at that time (`v2_resp_end_time()`). That end goes to whichever state is current at its time: a key response's collector runs before the frame's samples, and `pl_v2_response()` feeds the samples up to the end itself, so its hits, step ends, timed exits and chains through empty or instant states at or before that time go first, in time order, and at one time in rule 4's list order (`v2_hits()`, `v2_feed()`). One state takes the end, once; a pressed response is never scored `timeout`. A response whose `minimum_valid_rt_s` is at or past its `duration_s` leaves no time (ysp/response.h refuses the window): refused at load for literals and one variable's known levels, and before the trial for two variables or a session field, both versions (a reading of 2.6). A key response that nothing ends (no deadline, no count, a press does not end it) is refused at load (F37, decided); a variable `duration_s` that can be 0 is refused at load, and a session field's 0 at the trial's start, before the trial runs, by name, in both versions (`pl_window_check()`). F37 applies to key responses only (decided): a signal-choice response's window with no deadline closes when the trial ends, since the trial's own steps end it. A signal choice's window takes the trial's `duration_s` and `minimum_valid_rt_s` at every arm. A response's end and an `after_s` exit at one time go by the list order of rule 4: the timed exits run after the response's collector in the frame. A cycle with no SURE wait (every wait a region with `stay_s` rounding to 0 ns, a `stay_s` not known above 0 at load such as a session field, or a `signal_lost`) is refused at load (F26, decided; experiment.h 0.5.0). The player checks nothing at the trial's start for F26; its stop on state changes at one time is an internal guard sized from the template (runtime states, `over` rows and 8), which no loaded file reaches, so a chain of 70 states at one time runs. A `stay_s` from a variable chooses the detect's entry at each trial's start: 0 is `now`, as 3.7 says. Rule 8 (reading F38): an exit at time t removes the events of every part of the visit that have not fired and are due at or after t; the ones due before t land in the frame's evaluate, and a flag among them holds for the next state at once. A tween still running stops at the value it last wrote (the last evaluated frame's), and a tween of the next table on that channel starts from that value. The waiting rows of frame traces are sized at load (64 a frame trace), and so are a visit's tweens and the state lists (no limit on states); more stream names (traces and rdk stimuli) than the data set's 16 are refused at load by name: a legal definition does not stop mid-session on these bounds |
| An events template (no `states`) | Its steps split at each `until` into parts, as a state's do, but a part's change is no exit: the events of earlier parts that have not fired and their running tweens go on, as version 1's `to` runs on into `after`. An `until`'s timeout ends the parts with outcome `timeout`, and removes nothing either. When the parts are done, `after` follows them and the response's end, as version 1 (2.5). The response is waited for when its `from` has landed or a part's table shows it (it can land in the frame the parts end); an `until`'s timeout before any table shows `from` does not wait (F35) |
| A property both tweened and set by a constant in a template's step lists | Refused at load, naming it: a timeline channel is driven by a track or by events, never both, so version 1 refuses it in one table; across states it would stop the session at the second state's entry (reading F36) |
| Time columns (`{"time": A, "from": B}`) | A stimulus's, a state's and the end's times are their frames' flip record onsets, as the state rows' are; the row waits for them as it waits for its last frame's record |
| The trial's end, flags, `recenter` | Timeline marks with reserved codes; the end is handled after the frame's `ysig_frame()`. A flag a state's steps set by their end holds for the next state's `if` and table (3.12). A state whose first step is `recenter` arms its exits at the recenter's call (3.12); a recenter at a state's head runs on every visit but the trial's first (3.6 runs that one before the reset) |
| `max_visits`, `over` | A visit of no time past the limit, code -2, then `over`'s `to` and `outcome` (F20) |
| Outcomes (D36, F16), `requeue_outcomes` | The latest outcome set wins; with none set, the response's levels. An outcome in `requeue_outcomes` requeues the trial |
| `uniform`, `walk2d` (3.14) | ysp/trials.h variables. A leaf draws only what its template reads |
| `cm` units | ysp/gfx.h's `YGFX_CM` (0.20.0, G14), from the rig profile's `view` |
| `columns` | Each form read at the row's fill; a `time` column waits for its flip record |

The frame's order (3.10): a key response's collector (its end feeds the
samples up to it, `pl_v2_response()`); the samples up to the frame's
onset go to the graphs and their hits are taken; the timed exits, then
the hits their states' arms made at once; the timeline; `ysig_frame()`
and its hits; a second `ytl_evaluate()` for the states that hits started
on this frame (rule 5); the draw. The stop on state changes at one time
is an internal guard sized from the template (F26 refuses the cycles
that could move without end).

3.11 rules 2 and 4 for an arm made after input the graph already has
(the user's decisions of 2026-11-12): each template's graph keeps
ysp/sig.h 0.6.0's window (4 frames, 1,024 calls, 16,384 ring writes; a
response's end can be final two frames after its time). An arm at a
state's entry time before input already fed runs the graph again from a
checkpoint with the arm where it would have been made (sig.md, "An arm in
the past"), so a hold that a frame's hit started and that ends before the
frame's onset is a hit at the arm's call and the next state starts on
that frame (the model's `refound-in-frame`), and a response's chain whose
next sample came 1 ns later arms on the hand at the response's time (the
model's s4548). Rows of a frame trace that the window had passed to the
writer are not written again (counted by sig.h). An arm past the window
(a response's end that only the 1 s safety net made final) falls back to
the newest value: inexact, so the session counts it (`sig_arms` in
`session.json`) and says so on stderr at its end. Before the frame's
timed changes and evaluate, the player calls `ysig_settle()` at the
frame's onset, and before a response's end at that time: input is fed up
to it, so a hold the hand met by then is a hit in rule 4's time order with
the timed exits and the steps' ends (`v2_hits()` takes an earlier one
first), and rule 8 drops the steps at or after it. `ysig_frame()` found
such a hold only after the evaluate had landed those steps. At a hold
that ends at a response's end, rule 4's list order decides.

An arm in the past changes only what comes after its time: the player
takes changes in time order and arms at a take's time. What a run again
can change is the re-armed detect's earlier arm's hits after that time,
which `arm_gen` drops, and the hit at that time that ended the earlier
arm (sig.h's `arm_hits_superseded`). A hit of another detect, a row or a
derived event that a run again changes (`arm_hits_retracted`,
`arm_rows_stale`, `arm_derived_stale`) would be one the player acted on or
wrote; a fallback (`arm_past_pinned`, `_far`, `_full`, `_off`) is not
sig.h's reference order. The player stops on any of them by name
("internal guard: a past arm of ... changed a hit already taken, or a
row or event already written, or fell back"). `session.json` has the
session's sums in `sig_arms` (version 2 only).

A trial's start resets its graph (`ysig_reset_trial()` at the onset) and
arms the controls that no exit names at that onset. The scripted samples
up to that onset go into every graph before it, with the frame's other
input: fed after, they were input later than the arms they precede
(`late_input`; sig.h keeps input in call order, so an arm at the onset
did not see them). `--sim` has every sample in hand; in a real run a
sample that arrives after the onset's arms is real late input.

Three more parts follow sig.h's semantics (docs/sig.md): the graphs take
`rows_final`, so a sample trace's rows come once no arm can change them
(up to four frames late); `v2_on_row()` takes a trial's rows from its
reset to the `ysig_flush()` at its end, and the traces are written after
that flush. When a state ends, each of its controls whose latest arm is
still ahead of the input (it waits in sig.h) is disarmed, so that arm is
cancelled, not made for a state that ended; no other control is
disarmed (a disarm with no arm in its group takes the input time and
would end the next state's arm of the control made at an earlier time).

A trace's rows go through the data set's writer, as the other rows do: a
real run's frame thread does not call `write()`. A trace row is wider than
the writer's job, so it waits in a side buffer of the queue (2.4 MB, only
for a definition with traces).

## Explanation: a window with no deadline, and the wait for the flip record

Decided 2026-11-10 ("wait for the record"). The loader (ysp/experiment.h
0.1.2) takes a response with `duration_s` 0 as one that always ends with
a press: `$rt_s` reads in a number key, a lookup by `$response` needs no
`""` key, and a lookup by `$outcome` no `"timeout"` key. The player keeps
that guarantee.

The problem it had. ysp/response.h ends a window at the first press that
is valid against the onset it has when the press comes. Until the
stimulus's flip record arrives (one or two frames after the landing),
that onset is the plan. When the record's onset is later than the plan,
a press between `plan + min_rt` and `record + min_rt` became EARLY or an
anticipation after the window had ended: `YRSP_R_ENDED_EARLY`, no
response, and with no deadline, a trial with no valid press. The frame
loop feeds a press before it applies that frame's record (ysp/response.h,
"The order in a frame"), so with `min_rt` 0 the gap is the onset's
residual (under 20 us here) or a whole frame when the landing frame is
late.

The decision. Every response's window now waits for the record
(`yrsp_set_wait_final()`, ysp/response.h 0.9.0, opt-in in the header):
a press or the deadline ends the window only once the final onset is in.
A press the record makes early leaves the window open, as it would have
with exact timing. The landing frame's record settles the wait, shown or
not: ysp/screen.h gives every frame a record, a never-shown one (SKIPPED,
CANCELED) as a plan. The cost: every press fed before that record waits
for it, so the window ends, and `after` starts, at the record's arrival,
one or two frames after the onset, not at the press.

The safety net. If no record comes within 1 s of the onset, the onset
stands as it is. A record on GLX_OML comes 1 to 2 frames after its
vblank, also for a frame shown frames late (a stall such as a first
texture upload), so 1 s is 60 frames past any record the screen gives;
it fires only when records stop (a frame loop that does not call
`yrsp_update_frame()`). If it fires and a record then moves the onset,
ENDED_EARLY can come back: `player_test.py` `t_r5_wait_final` makes it
fire with a plan 1.2 s early, and the row is then a timeout with
ENDED_EARLY. With no deadline, the response is then empty, and an
`after` lookup by `$response` with no `""` key stops the session: "a
lookup has no entry for the value ''" (exit 1; the trial's row is not
written). A plan 1.2 or 1.5 s early in `--sim` shows it
(`--test-plan-early`, a scripted press 5 ms before the final onset).
On a real rig it needs the records to stop for 1 s. Give such a lookup
a `""` key, or a deadline, where that matters. A first version of this bound was 4 frames, from a wrong
reading that a never-shown frame has no record; a landing frame 3 or
more frames late then passed it before its record (review, 2026-11-10).

The evidence. A 60 Hz frame loop in the player's order, duration 0
(`ended_early.c`, a probe of ysp/response.h; the header's test has the
same cases):

| Case | Without the wait | With the wait |
|---|---|---|
| `min_rt` 0, the landing a frame late, press 5 ms after the plan | Ends on the press; no response, ENDED_EARLY | The press is EARLY; the window stays open; a press 300 ms after the onset is the response |
| `min_rt` 0, onset 40 us after the plan, press 20 us after it | No response, ENDED_EARLY | As above |
| `min_rt` 100 ms, landing 2 frames late, press 110 ms after the plan | The record comes first: an anticipation; the window open | The same |
| `min_rt` 0, onset at the plan, press 5 ms after it | A response, RT 5 ms | A response, RT 5 ms |

In the player (`player_test.py` `t_r5_wait_final`, `--sim` with
`--test-plan-early`, a plan one frame before the record's onset, and a
scripted press 5 ms before the record's onset; also with the plan 50 and
100 ms early, a landing 3 and 6 frames late): with a deadline of 1 s,
the press is EARLY, the row is a timeout with no ENDED_EARLY, and the
`after` table's first mark comes 1.0 s after the final onset, not at the
press; with no deadline, the window stays open after the press; a press
5 ms after the final onset is the response. Without the wait, the same
runs gave `rsp_flags` 132 (ENDED_EARLY), `after` 16.7 ms after the onset,
and a no-deadline trial that ended with no response.

The row and `after`. `after` is built from the result when the window
ends (the live result); the row's response columns from the final result
at the trial's end, so the data never claims a response that the final
onset makes early. They can still name another response in two cases: a
box's press read after the window ended, stamped before the press that
ended it (the final result takes the earlier stamp; `real_test.py`
`lateread`), and the safety net. Such a row has `rsp_flags` 0x8000
(`PL_RSP_AFTER_LIVE`), a bit ysp/response.h leaves to its caller (bits 15
to 31). `--sim`'s files did not change.

## Results

2026-11-09, the Linux desktop (Debian 12, gcc 12.2, 20 threads), `--sim`
on the virtual clock, unless the row says otherwise.

| Check | Result |
|---|---|
| The four worked examples against their C programs (`tests/player/compare.py`) | RDK, 2AFC: every shared column exact, the dots' digest and the steps file included (the program's trial seed from `--trial-seeds`, the 2AFC's timing from `--design-timing`, both test-only). Stroop: exact except columns the format derives differently (`block` text, `too_slow`). Oddball (`trigger_flip.c`): not comparable column for column; the shared mechanism checked instead. The report lists each column |
| `tests/player/player_test.py` | 41 cases of unusual shapes and of each fix of both reviews (round 2: `to` and `then` and the trial's end, arrays in `vars` and `data`, a key held into another template's trial, yield frames, the load-time bounds, a stimulus left on, the definitions experiment.h 0.1.2 accepts; a lookup in every key type, nesting, `if` on a node and on a step, `loop`, `requeue`, a track that ends early, density, key re-application, stale RDK data, timeouts, any-key responses, a stimulus shown twice, `vars` and `data` of one name, repeated sounds, a backward `at_s`, the walk's budget, the data set): pass |
| `tests/player/eval_test.c` | 52 checks: pass |
| ASan and UBSan (`setarch -R`): the four examples, the unit test, the 40 cases, the comparison, and two review scripts of 31 definitions (the 20-minute RDK trial and 70,000 leaf runs among them) run to their ends | Clean. Each heavy job ran in a 6 GB memory scope; the system tests now stream their data files (peak 18 MB) |
| Fuzz, ASan and UBSan: `tests/fuzz/player_fuzz.c` over the corpus of `tests/fuzz/player_corpus.py` (1,015 seeds, every key of ysp/experiment.h's table in a valid definition, arrays in `vars` and `data`, the values 0.1.2 accepts) and the four examples; token swap, byte flip, key insertion, key deletion; a quarter of the inputs with no trial cap; 8 processes | Final code (round 4, `setarch -R` for the build and the run, a 6 GB memory scope, 8 processes, 418 s; 1,017 seeds, with a leaf loop over a track that finishes and an RDK trial past its buffer): 200,000 inputs, 27,316 loaded, 26,441 ran to the end, no report. Round 3: 200,000 inputs, 27,076 loaded, 26,222 ran to the end, no report. The run before it, on the code before the last fix, found one: a `quest` with three stimulus axes read past the one value the player passes (now refused at load) |
| gcov, `-O0`, the seeds and 30,000 mutated inputs | Final code: `lower.c` 92.2 %, `run.c` 95.1 %, `host.c` 94.4 %, `eval.c` 84.6 % of lines. Reached: a leaf loop ended by a pass with no trials (10 times), a mid-trial RDK flush (29 times). Not reached: the yield on a leaf reopen (a guard, see the mutants) and the full requeue history (the system tests reach it). Not reached in `eval.c`: the AEP dims branch (AEP is refused), defensive cases of value kinds the loader does not produce (`YEXP_V_NONE`, a linspace or choice as a single value, unknown kinds) and errors for types the loader refuses (`no value where a number goes`, a non-contrast gamut, a gamut with no calibration) |
| Mutants (`tests/mutate/player_{lower,run,eval,host}.toml`, built with the system tests) | Round 4, every list in full: 79 of 79 as expected: 75 killed, 4 kept as survivors with a note (a full row ring, the weight of a leaf open in the per-frame yield, a value's text behind the load's text bound, and the yield on a leaf loop's reopen, a second guard behind the empty-pass rule). `experiment.toml` (round 3): 171 of 171. 2026-11-10, the real window: every `--sim` mutant again, 81 of 81 as expected (r-33 and r-34 moved to the writer's hand-off; r-46, the writer's events in `--sim`, killed); 20 new mutants of the real path (test `real`: `tests/player/mut_real.c` runs `real_test.py`'s oddball, rule, abort, panic and refusal cases with the mutated player, one at a time on the display, 1,883 s): 20 of 20 as expected, 18 killed, 2 kept with a note (rr-08, the rule's codes not told to the collector, needs a press the rule fires on and the collector refuses; rr-10, the writer not drained before `session.json`, is a race the writer wins) |
| Fix round, 2026-11-10 (the review's findings and decisions A and B) | Mutants: every `--sim` mutant, 82 of 82 as expected (r-47, the wait for the flip record, killed by `t_r5_wait_final`); the real path's, 30 of 30 as expected (each runs only its cases, `YPL_REAL_CASES`, 1,361 s and a rerun of three): 25 killed, 5 kept with a note (rr-08, rr-10, rr-12, rr-16, rr-18: guards that this rig cannot reach). rr-05 and rr-14 survived the first run and each got a stronger check (a code's deadline; a lost code whose next trial has the same code). ysp/response.h 0.9.0: 89 of 89. ASan and UBSan: the `--sim` tests (43 cases, the comparison, the four examples with three participants, the unit tests) and the real cases (the four examples, `noafter`, `lost`, `rule`, `rule2`, `fewer`, the abort, the panic, the refusals; 10 trials each): no report. Fuzz (ASan and UBSan, 4 processes, 152 s): 40,000 inputs, 5,475 loaded, 5,308 ran to the end, no report. ctest 119 of 119 |
| Fix round 2, 2026-11-10 (the second review's items) | Mutants: every `--sim` mutant, 83 of 83 as expected (run 48, lower 25, host 4, eval 6); the real path's, 34 of 34 as expected (1,576 s, and rr-12 again after its anchor moved, 57 s): 30 killed, 4 kept with a note (rr-08, rr-10, rr-12, rr-16). rr-18 (the row's flag when `after` heard another result), rr-19 (the row keeps the live result), rl-12 (rules with other fires a trial), rl-13 (the override names its roles) and rl-14 (the role check after the screen opens) killed. ysp/response.h 0.9.0: 93 of 93; ysp/device.h `rule-12` to `rule-19`: 8 of 8. ASan and UBSan (`setarch -R`): the `--sim` tests (43 cases, the comparison, the four examples with three participants, the unit tests, 21,292 checks of `response_test`) and the real cases (the four examples, `noafter`, `lost`, `rule`, `rule2`, `lateread`, `fewer`, `maxfires`, the abort, the panic, the refusals; 10 trials each, 238 s): no report; the `--sim` files the same bytes as the Release build. Fuzz (ASan and UBSan, 4 processes, 153 s): 40,000 inputs, 5,475 loaded, 5,308 ran to the end, no report. ctest 119 of 119 |
| Allocations in `pl_frame` (`player_alloc_test`) | 0 on all four examples; 0 on the frame thread in a real run (`--real`, below) |
| The comparison (`tests/player/compare.py`) | No exact column differs; timeouts now in the RDK and 2AFC scripts |
| Full ctest, Linux, `DISPLAY` unset | 120 of 120; the player and experiment tests of the sanitizer build, 9 of 9. 2026-11-10 (this build's set, the real window's code included): 119 of 119 |
| `--sim` byte identity, 2026-11-10 (v0.2.0 against main's v0.1.0): the 42 cases of `player_test.py`, `compare.py`, and the four examples with three participants each | 459 files: 369 identical; 90 `session.json` files differ in `player_version` only (and in `rig_profile_sha256` where the test made its rig profile again, whose `written` date differs). The same on the ASan and UBSan build. After the fix round: the same 369 identical, the 90 differ in `player_version` and `headers.response` (0.9.0) only. After fix round 2: the same, on the Release and the ASan and UBSan build |

Frame work (`--measure`, Release, i5-14500, no other job of mine running,
desktop open, load average 1.4 to 2.9, three runs of each of the four
examples, after round 2): largest frame 29 to 67 us, largest trial start 17
to 58 us, 0 frames over 16 ms. Before round 2 (load average about 2): 20 to
50 us and 14 to 32 us. The spread between runs of one example (2AFC: 50 to
67 us) is as large as the change, so this is no measured regression. The simulated display has no
GPU cost, so this is the CPU part of a frame only.

RDK flush cost (`--measure`, Release, i5-14500, load average 0.2 to 1.2,
desktop open, no other job running, 2026-11-10): four RDKs of 200 dots
shown together for three 120 s trials (21,604 frames), so all four flush
in the same frame. With a 4,096-step buffer the largest frame's work was
11.5 ms in each of three runs (4 x 4,096 steps: about 0.70 us a step).
With the 64-step buffer it was 219 to 373 us. The two runs' streams are
identical. A 4,096-step buffer would put a 16 ms frame at risk with five
or six RDKs; 64 steps keep a flush under 50 us an RDK.

### Format version 2 (2026-11-11)

The Linux desktop (i5-14500, Debian 12, liquorix 6.11, gcc 12.2, Release),
`--sim` on the virtual clock.

The comparison with the C programs (`tests/player/compare.py`; the C
program takes the player's draws and writes its samples, and the player
runs on those samples):

| Example | Result |
|---|---|
| Visuomotor rotation (`trial_visuomotor`, 160 trials) | Every `trials.csv` column exact but two: `rsp_tier` against `move_tier`, 1 of 160 (the program writes the stamp tier 0 with no response, F33), and the cursor's end angle, 1 of 160 at 4 decimals (F25). `streams/path.csv` against `--traj`, 42,958 rows: `trial_index`, `t_s`, `hand_x`, `hand_y` exact; `cursor_x` 765 rows and `cursor_y` 726 rows round the other way at the C file's 3 decimals (the program turns the cursor in float about the window's center, the player in double about the origin, F25) |
| Two-mouse tracking and reaching (`trial_bimanual --design-timing`, 120 reaches, 12 tracking trials) | Every reach column exact (`rsp_role` after mapping `L` and `R` to the role names). `streams/frames.csv` against `--track`, 33,120 rows: every column exact |
| The four version 1 examples | As before (the table above): no exact column differs |

Version 1 identity (this branch against main, 7078e4d): of the 439 CSV
files of the version 1 runs of `player_test.py` and `compare.py`, 438 are
byte-identical. The other is the Stroop C program's file (`stroop_c.csv`),
whose comment lines name ysp/trials.h's version (0.8.1). `session.json` differs in
`player_version`, `headers.trials` (0.8.1), `headers.gfx` (0.20.0),
`pack_id` (the pack records the headers' versions, so its bytes change
with them) and `rig_profile_sha256` where a test wrote its rig profile on
another day (4 files). `format_version` is 1 in both (94 files).

Allocations: 0 heap calls in `pl_frame()` on both version 2 examples
(`player_alloc_test --samples`).

Frame work (`--measure`, the default build, no zones; pinned to CPU 2, a
performance core, with `taskset`; nothing else running; three runs):

| Example | Frames | Largest frame | Largest trial start | Frames over 16 ms |
|---|---|---|---|---|
| Visuomotor | 19,370 | 638 to 642 us | 36 to 37 us | 0 |
| Bimanual | 48,422 | 1,889 to 1,954 us | 38 to 42 us | 0 |

Without pinning, a run on an efficiency core measured up to 3,041 us
(bimanual), so a timing run states its core.

The largest frame is the trial's end. With the trace ring
(`--profile`, three runs, the same core), 95 to 97 % of it is the sample
trace's rows written in place: 271 rows in 644 us (visuomotor) and 1,878
rows in 1,893 us (bimanual), about 1 to 2.4 us a row. On `--sim` the frame
thread writes them; a real run copies each row (296 bytes) to the writer's
queue instead, which is not measured. The knob is a trace's rows a trial:
at 1 us a row, 16 ms holds about 16,000 rows on `--sim`.

Per zone (`tools/profile_summary.py`, run 3, us):

| Zone | Visuomotor median, p99, max | Bimanual median, p99, max |
|---|---|---|
| `pl.frame` | 2.16, 17.12, 661.70 | 20.51, 26.36, 1,983.95 |
| `pl.trial_end` | 601.77, 647.24, 659.22 | 852.46, 1,953.78, 1,981.07 |
| `pl.v2_trial_rows` | 598.84, 644.29, 656.33 | 849.38, 1,950.26, 1,977.49 |
| `pl.trial_start` | 7.29, 20.39, 40.34 | 13.22, 41.71, 48.72 |
| `pl.v2_input` (`sig.samples` in it) | 0.07, 7.83, 10.61 | 15.15, 20.30, 30.12 |
| `sig.frame` | 0.51, 0.70, 3.19 | 0.56, 0.71, 8.12 |
| `pl.flip_records` | 0.54, 2.94, 8.99 | 3.77, 5.62, 24.16 |
| `pl.draw` | 0.50, 0.59, 6.84 | 0.51, 0.59, 5.09 |

The largest trial start (the session's first, frame 0): visuomotor 40.3
us, of which `pl.leaf_open` 14.8, `pl.timeline_reset` 4.7, `pl.v2_enter`
6.0, `pl.v2_params` 4.1, `pl.trial_next` 3.7, `pl.stim_begin` 2.6;
bimanual 48.7 us, of which `pl.leaf_open` 21.7, `pl.v2_enter` 8.6,
`pl.v2_params` 5.9, `pl.timeline_reset` 4.9, `pl.trial_next` 3.2.

The cost of the zones (pinned, eight runs of each build, mean user time
of the whole run):

| Build | Visuomotor | Bimanual |
|---|---|---|
| Default (no zones) | 0.211 s (sd 0.002) | 1.142 s (sd 0.007) |
| `YSP_TRACE_RING`, no `--profile` (no ring attached) | 0.209 s (sd 0.003) | 1.148 s (sd 0.007) |
| `YSP_TRACE_RING`, `--profile` | 0.291 s (sd 0.003) | 1.351 s (sd 0.008) |

With no ring attached the difference is within the spread. With
`--profile`, the run costs 4.1 us (visuomotor) and 4.3 us (bimanual) more
a frame, most of it the file's write after the frame. In the frame's work
it is about 1.4 us: 27.2 and 26.3 zones a frame at 51 ns a zone pair with
the ring attached (`rt_ring_bench` B5, the same machine and day). The
largest frame with `--profile`: 646 to 729 us and 1,975 to 2,005 us.

`YSP_TRACY` (on demand, no profiler connected), three runs on CPUs 2, 4
and 6: largest frame 648 to 654 us and 1,980 to 2,003 us, against 638 to
642 us and 1,877 to 1,900 us for the default build on the same CPUs.
Pinned to one CPU, Tracy's own threads took it: 2,378 to 2,456 us
(visuomotor), and once a 2,111 us trial start.

The data of every build and option above (`--measure`, `--profile`, both
instrumentation modes) is byte-identical to the default build's, apart
from `profile.csv`.

Tests:

| Check | Result |
|---|---|
| `tests/player/player_test.py` | 49 cases, 0 failures. Version 2: `t_v2_states` (states, exits, flags, instant and shared exits, `max_visits` and `over`, outcomes, `requeue_outcomes`), `t_v2_until` (an events template's `until` and timeout, the G18 refusal), `t_v2_hand` (hits with a hold, a movement response and `rsp_role`, the column forms, `freeze_at`, a sample and a frame trace and the trace's window, a walk and a uniform, value forms, cm units), `t_v2_rule5` (five states on one frame; `--profile`), `t_v2_corpus` (the loader's `v2_*.json` files: `v2_instant_shared` stops by name, F26) |
| ctest, Release (`build/linux`, `DISPLAY` unset) | 122 of 122, `compile_windows_macros_c` and `_cxx` among them |
| ctest, ASan and UBSan (`sanitizers` preset, built and run under `setarch -R`) | 121 of 121 |
| Fuzz, `tests/fuzz/player_fuzz.c` over the version 2 seeds (the loader's 8 `v2_*.json` files against the visuomotor pack's table, and the two 3.18 packs); each signal role gets a gain and 20 s of scripted samples; 8 processes | Release: 5,000,000 inputs (about 2 h), 767,844 loaded, 294,751 ran to the end, 1,886,114 with signal roles; no crash. ASan and UBSan (`setarch -R`): 40,000 inputs, 6,130 loaded, 2,395 ran to the end, no report. The version 1 corpus was not run again |
| Mutants, `tests/mutate/player_{v2,run,lower,host,eval}.toml` (`--sim` test) | `player_v2.toml` 11 of 11 killed. Every other `--sim` mutant as expected: run 45 killed and 3 kept survivors, lower 25 killed, host 4 killed and `h-v2-01` now kept (a guard: the loader refuses a version above 2 first), eval 5 killed and 1 kept. The 35 real-path mutants (test `real`) need a display and were not run |

### The phase 2 review's fix round (0.4.0, 2026-11-11)

The same machine. Each of the review's 19 items has a case in
`t_v2_review2` (the reviewer's reproducers) or in `mut_test.c`
(`case_v2_outcome`), and a mutant where the branch is new.

| Check | Result |
|---|---|
| `player_test.py` | 50 cases, 0 failures |
| `compare.py`, the six examples | No exact column differs; the format classes within their bounds (F25: 765 and 726 path rows, 1 angle; F33: 1 row); the blocks and the tracking rows' `block`, `block_trial`, `perturbation` and `mapping` now compared, exact; every other C column listed as not comparable, with its reason |
| ctest | Release 122 of 122; ASan and UBSan (`setarch -R`) 121 of 121 |
| Mutants (`--sim` test) | `player_v2.toml` 24 of 24 killed (v2-12 to v2-24 new; v2-18 survived the first run and `t_v2_review2` got a check); `player_run.toml` r-v2-01 to r-v2-03 killed (r-v2-02 and r-v2-03 survived the first run, then the hold and the late response cases); the other `--sim` mutants as before. `experiment.toml` v031-01 and v031-02 killed |
| Fuzz, Release, 8 processes, the version 2 seeds and `tests/fuzz/player_v2_seeds` (role-only columns, when tests), one input in 16 with a bad samples line | 5,000,000 inputs, 722,464 loaded, 435,355 ran to the end, 1,761,306 with signal roles; no crash. ASan and UBSan: 40,000 inputs, 5,750 loaded, 3,465 ran to the end, no report |
| Version 1 identity against main (7078e4d) | 438 of 439 CSV files byte-identical (the other the Stroop C program's, its trials.h version line); `session.json`: `player_version`, `headers.trials`, `headers.gfx`, `headers.experiment`, `pack_id` (94 files) and `rig_profile_sha256` (4) |
| Version 2 data against 0.3.0 | `target_frames` differs on 159 of 160 visuomotor rows and `reach_target_frames` on 4 bimanual rows (the landing frame now counted, item 7); `streams/frames.csv` has the `flags` column, all 0; every other cell equal |
| Frame work, pinned to CPU 2, six runs each, mean user time | Visuomotor 0.207 s (sd 0.003) against 0.3.0's 0.212 s; bimanual 1.133 s (sd 0.010) against 1.139 s: no growth. `--measure`, four runs: largest frame 628 to 633 us against 638 to 642 us, and 1,907 to 1,920 us against 1,891 to 1,907 us, one run 2,853 us at frame 24,487 (0.3.0 showed 2,803 us at that frame once) |

### The re-review, F34 and F26 (0.4.1 and 0.5.0, 2026-11-12)

The same machine. The re-review's items B, D to I have cases in
`t_v2_review3`, F34's in `t_v2_f34` (the reviewer's `v1r.py` cases, the
window open at the states' end, states that outlast it, the deadline's
outcome and the latest set, a `from` never shown); F26's in the loader's
corpus.

| Check | Result |
|---|---|
| `player_test.py` | 52 cases, 0 failures |
| `compare.py`, the six examples | No exact column differs; the format classes within their bounds |
| ctest | Release 122 of 122; ASan and UBSan (`setarch -R`) 121 of 121 |
| Mutants (`--sim` test) | `player_v2.toml` 41: 39 killed, 2 kept as guards (v2-35, a frame row whose record never comes; v2-41, the 64-change stop F26 leaves unreachable). v2-37 survived its first run; `deadline_latest_wins` then killed it. `player_lower.toml` l-v2-01 kept (a guard). `experiment.toml` f26-01 to f26-04 killed. The other lists as before |
| Fuzz, Release, 8 processes, the version 2 seeds | 5,000,000 inputs, 723,855 loaded, 437,963 ran to the end, 1,764,089 with signal roles; no crash. ASan and UBSan: 40,000 inputs, 5,725 loaded, 3,473 ran to the end, no report |
| Version 1 identity against main (7078e4d) | 438 of 439 CSV files byte-identical (the other the Stroop C program's trials.h version line); `session.json`: versions, `pack_id` (94) and `rig_profile_sha256` (4) |
| Version 2 data against 0.3.0, the two 3.18 examples | `target_frames` (159 of 160) and `reach_target_frames` (4 of 132) only, as in 0.4.0 |
| Frame work, CPU 2, six runs each, mean user time | Visuomotor 0.209 s (sd 0.003) against 0.3.0's 0.211 s; bimanual 1.128 s (sd 0.010) against 1.146 s. Largest frame 637 to 641 us against 636 to 646 us, and 1,900 to 1,924 us (one run 2,069 us) against 1,880 to 1,906 us (one run 2,045 us) |

### The final verification (0.10.0, 2026-11-14)

The Linux desktop (i5-14500, GCC 12.2, Release), sig.h 0.6.0 with the
reviewer's oracle in ctest.

| Check | Result |
|---|---|
| ctest | Release 124 of 124; the sanitizers tree (ASan and UBSan, `setarch -R`) 123 of 123 |
| `player_test.py` | 56 cases, 0 failures; `sig_arms.unverified` 0 in every session |
| `compare.py`, the six examples | No exact column differs; `unverified` 0 |
| Version 1 identity against main (7078e4d) | 438 of 439 CSV files byte-identical (the other the Stroop C program's version line); `session.json`: versions and `pack_id` (94 files) and `rig_profile_sha256` (4) |
| The model's harness, new seeds 82001 to 92000 (9,999 cases) | 1,446 refused (agreed), 429 hangs agreed, 53 mismatches: 51 at a GAP, 2 not (below); no guard stop; `unverified` 0 in all 7,405 sessions run (the shrinker's included) |
| `GEN_MULTI=1`, 34001 to 39000 (4,999 cases) | 654 refused (agreed), 221 hangs agreed, 25 mismatches, all at a GAP; `unverified` 0 in all 3,682 sessions |
| Mutants (`--sim` test) | `player_v2.toml` 89 of 89, `player_run.toml` 64 of 64, `player_lower.toml` 26 of 26, `player_eval.toml` 6 of 6, `player_host.toml` 5 of 5 as expected; the 35 real-path ones need a display and did not run |
| Frame work, CPU 2, six runs each, load average 0.84 at the start and 1.02 at the end | Visuomotor 0.237 s (sd 0.002) against 0.9.0's 0.212 s (sd 0.002); bimanual 1.291 s (sd 0.008) against 1.135 s (sd 0.006): 12 % and 14 % more (0.10.0 on 2026-11-13: 10 %). Largest frame 626 to 645 us against 629 to 632 us, and 1,857 to 1,886 us against 1,891 to 1,909 us; no frame over 16 ms |
| Arena of a template's graph (4 frames, 1,024 calls, 16,384 writes) | Visuomotor `reach`, 55 nodes: 11,049,136 bytes (8,718,192 with no window); bimanual `reach`, 49 nodes: 10,799,344 (8,471,632); `track`, 38 nodes: 8,926,576 (6,615,760) |

The two mismatches outside a GAP were there before this round (the same
on the players built 2026-11-13). Both are a tie at one time between a
key response's end and a region entry from a sample at that time. Seed
84233: the response exit is listed first and the player takes it (code
0), the model the region (1). Seed 89320: the region exit is listed
first, the response exit is the one the player adds, and the player
takes the response (2), the model the region (0). By rule 4's list
order the second looks like the player's, the first like the model's.
Not decided in this round.

### sig.h's `arm_unverified` and the guard (2026-11-14)

The same machine. sig.h counts an arm outside the shapes whose result is
verified (docs/sig.md, "Exactness"); `sig_arms.unverified` sums it, and
the guard stops on it, after each arm and at the end of each frame (a
late report or a group that an input lets in is counted at an input).

| Check | Result |
|---|---|
| `player_test.py`, before the guard | 56 cases; `unverified` 0 in every session (342 arms run again) |
| `compare.py`, the six examples, before the guard | `unverified` 0 (visuomotor and bimanual: 176 arms run again) |
| The model's harness, before the guard (seeds 72001 to 82000; `GEN_MULTI=1`, 29001 to 34000) | 57 and 22 mismatches, all at a GAP, as at 085bbb6. `unverified` above 0 in 4 of 4,878 sessions (41 arms) and 3 of 2,394 (16): one shape, condition 2 (b). A waiting arm at `τ`, then at the same `τ` a later waiting group whose disarm meets it (the state exits at the time its control was armed for), and one input lets both in |
| The same, with the guard, after the reviewer's next round (2 (b)'s exception takes a group of an arm and disarms; condition 3 for groups that wait only) | 57 and 22 mismatches, all at a GAP; `unverified` 0 in all 7,272 sessions; no guard stop. `player_test.py` and the six examples: 0 |

### The samples before a trial's start (2026-11-14)

The same machine. The player fed a frame's samples after the trial
start's reset and arms at the onset (`run.c`, `pl_start_trial()` before
`pl_v2_frame_input()`). A count of late input in a scratch ysp/sig.h
(an input call with a time before an arm already in the reference order
whose footprint meets the arm's, made after it) found 16 in the
bimanual example (both hands' samples, timed 16.5 to 16.7 ms before a
trial's first onset: d13, a frame-rate hold on the start position, and
d11, a frame-rate entry-now distance test), 17 in `player_test.py`
(`v2_hand` 16; a model case, seed 4588, 1, a sample 1 ns before the onset
from the frame grid's rounding) and 0 in the visuomotor example. With the
samples first:

| Check | Result |
|---|---|
| The count, scratch build | `player_test.py` 0 of 64,776 input calls; visuomotor 0 of 300,144; bimanual 0 of 3,241,997 |
| `sig_arms.late_input` (the player's count, any footprint) | `v2_hand` 0, 16 with the old order (its new check fails on it; mutant r-v2-16 killed); `vm_settle_tie` 0 |
| `compare.py`, the six examples | Every CSV byte-identical to the old order's run |
| `player_test.py` | 56 cases, 0 failures. Of 1,508 CSV files one differs: `vm_rows_final`'s `streams/path.csv`: its rows at the trial's onset are 6 with the hand's tier and device (were 7 with tier 0 and device 0, the peak's value after the reset); the other 57 rows are unchanged |
| Version 1 identity | Every version 1 CSV file byte-identical (`session.json` differs in `rig_profile_sha256` only, a rig profile written again) |

### sig.h 0.6.0's window and the settle at the onset (0.10.0, 2026-11-13)

The same machine. ysp/sig.h 0.6.0 (an arm in the past runs the graph
again; an arm completes its detect's hold met by its time), the settle at
the frame's onset and at a response's end, `sig_arms` in `session.json`,
and the guard on a changed past.

| Check | Result |
|---|---|
| The model's harness, the tree's model (v2-model at 43e1172) | Seeds 0 to 19,999: 131 mismatches (30 at a GAP); `GEN_MULTI=1`, seeds 0 to 4,999: 33 (10 at a GAP). The model takes a timed exit (phase T) before a hold that ended by the frame's onset, which it finds only after the evaluate (phase F); the player takes the hold first (rule 4). With the window and no settle: 3 mismatches, all at a GAP |
| The same, with the settle reading in the model (in `resolve()`: phases T and F add holds met by the onset as candidates, phase P holds met by a response's time) | 20,000 cases: 5 mismatches, all at a GAP (`arm-order`, `tie-steps-end`, `future-arm-hit`, `future-arm-hold`); 5,000 multi-press cases: 0. The fixed set: 50 cases, 0 failures |
| `sig_arms` over the harness (25,000 cases, the settle reading) | 18,325 arms run again, 66 superseded hits, 0 retracted, 0 rows or derived events stale, 0 fallbacks, no guard stop. Before the arm completed its hold (sig.h 2ca1cf6), 13 of the 20,000 cases had a retracted hit: an `until`'s re-arm at a state's entry went before the onset that had found the hit which ended the state before |
| `player_test.py` | 56 cases, 0 failures (`vm_settle_tie` new); its v2 sessions: 197 arms run again, 1 superseded, all else 0 |
| `compare.py`, the six examples | No exact column differs. Against 0.9.0's run, the visuomotor example's `landing_residual_s` is 10 ns in 159 of 160 trials (was 0): the target is due at the hold's hit, 10 ns before the onset that shows it, and the settle finds that hit before the frame's first evaluate. Its v2 sessions: 176 arms run again, all else 0 |
| ctest | Release 123 of 123; ASan and UBSan (`setarch -R`) 122 of 122 |
| Mutants (`--sim` test) | `player_v2.toml` 74 of 74 as expected (v2-73 to v2-77 new; v2-61 now equivalent: the settle's drain does its work); `player_run.toml` 62 of 62 of the `--sim` ones (r-v2-02 not reached now, kept with a note; the 19 real-path ones need a display and did not run) |
| The model's s19383 (14ec8af) | s1 goes back to itself by `after_s` at the time its hold ends (the exit wins by list order). sig.h's re-arm at that time completes the old hold and queues its hit; the player drained it with the new arm's generation, so the next visit ended at its own entry. An arm's hits are at or after its time, after it for a hold: a hit before is the earlier arm's (`vm_rearm_tie`; mutants v2-78 to v2-80 killed). Then v2-model at 9779055 (its `diff.py`): seeds 1 to 19,999, 4 mismatches, all at a GAP (851, 12130, 13283, 16049); `GEN_MULTI=1`, seeds 1 to 4,999, 0; the fixed set 50 of 50. ctest Release 123 of 123, sanitizers 122 of 122; `player_test.py` 56 cases, 0 failures |
| The model's s21418 and seven more (0b20b3c) | An arm made at a frame with a past time (an F40 timed exit taken a frame late) ran the graph again and found the crossing after its time, but sig.h did not hand that hit out again: the detect's earlier arm had handed out an equal one, which the player dropped as that arm's. sig.h now hands out the armed detect's hits from the new arm on (`vm_rearm_found`; sig mutants 236 and 237, and 210, 217, 235: 5 of 5). `player_test.py` and `compare.py` check the reference model's state-row invariant (entry times and frames never go back) on every v2 run; the v2 fuzz seeds keep it. v2-model at 1d0ea00 (its `diff.py`): seeds 1 to 21,999, 4 mismatches, all at a GAP (851, 7516, 13283, 16049), and 12 rows that end before their entry in both (an `after_s` exit counted from the entry frame's onset: the GAP after-before-entry; 315, 1496, 2418, 3647, 6673, 9485, 11368, 12353, 15993, 19685, 19818, 20380); `GEN_MULTI=1`, seeds 1 to 4,999, 1 mismatch at a GAP (313) and 4 such rows in the player (313, 1496, 3230, 4265; the model has 4190 too); the fixed set 50 of 50. ctest Release 123 of 123, sanitizers 122 of 122; `player_test.py` 56 cases, 0 failures; `compare.py` no exact column differs |
| F41, "clamp to entry" (decided 2026-11-13) | A timed exit's time is the later of its entry frame's onset plus the duration and the entry (`v2_after_t()`; for an until's timeout, the wait's entry). `vm_after_clamp`, `vm_timeout_clamp`; mutants v2-81 and v2-82 killed, v2-15 and v2-48 follow the code |
| Fuzz, Release, 8 processes (2 h 22 min) | 5,000,000 inputs, 725,751 loaded, 439,487 ran to the end (as 0.9.0); 88,776,145 arms run again, 0 fallbacks, 0 superseded, retracted or stale, no guard stop. ASan and UBSan: 40,000 inputs, 687,745 arms run again, all else 0, no report |
| Version 1 identity against main (7078e4d) | 438 of 439 CSV files byte-identical (the other the Stroop C program's version line) |
| Frame work, CPU 2, six runs each, load average 2.20 at the start and 1.94 at the end (5-minute 3.67) | Visuomotor 0.233 s (sd 0.003) against 0.3.0's 0.212 s; bimanual 1.255 s (sd 0.006) against 1.145 s (sd 0.008): about 10 % more than 0.9.0 (0.205 s and 1.133 s), the window's checkpoint at each frame and the settle. Largest frame 642 to 653 us against 636 to 646 us, and 1,915 to 1,997 us against 1,884 to 1,938 us (one 0.3.0 run at 2,791 us) |

### Rule 2 through sig.h 0.5.0, the model's later findings (0.9.0, 2026-11-12)

The same machine. ysp/sig.h 0.5.0's arm in the past (the user's decision
on `refound-in-frame`), the model's runs against 9b82be2 (s4548, the
script's overlap check in ns, a window with no time), `t_v2_findings`
(renamed from `t_v2_model`: the v2-model branch adds a function of that
name).

| Check | Result |
|---|---|
| The model's harness, v2-model at bd10c2e | Seeds 0 to 19,999: 20,000 cases, 2,628 refusals agreed, 4 mismatches, all at a GAP (1 `refound-in-frame`, 2 `future-arm-hit`, 1 `arm-order`); was 26 with 21 `refound-in-frame`. The one left: the model lands a visit's steps on rule 5's second evaluate before it takes that visit's arm-made hit; the player takes the hit at the arm, before the evaluate (item 2's one rule), so the step does not land. `GEN_MULTI=1`, seeds 0 to 4,999: 0 mismatches. The fixed set (`--fixed`): 50 cases, 0 failures |
| `player_test.py` | 55 cases, 0 failures |
| `compare.py`, the six examples | No exact column differs; every CSV byte-identical to 0.8.0's run, the 3.18 examples included |
| ctest | Release 122 of 122; ASan and UBSan (`setarch -R`) 121 of 121 |
| Mutants (`--sim` test) | `player_v2.toml` 69 of 69 as expected; `player_run.toml` 62 of 62 (r-v2-13 and r-v2-14 new). Two drains of the arms' hits before the next sample (v2-73, v2-74) survived: sig.h's catch-up already gives the right value, so they are not in the code |
| Fuzz, Release, 8 processes | 5,000,000 inputs, 725,751 loaded, 439,487 ran to the end; no input with every deadline given, no states and no open until stopped at the budget. ASan and UBSan: 40,000 inputs, no report |
| Version 1 identity against main (7078e4d) | 438 of 439 CSV files byte-identical (the other the Stroop C program's version line) |
| Frame work, CPU 2, six runs each, load average 0.10 at the start and 0.37 at the end (5-minute 1.49) | Visuomotor 0.205 s (sd 0.004) against 0.3.0's 0.213 s; bimanual 1.133 s (sd 0.007) against 1.147 s (sd 0.006). Largest frame 629 to 639 us against 639 to 643 us, and 1,890 to 1,918 us against 1,879 to 1,915 us. A first set, at a 5-minute load of 4.45, had two visuomotor runs at 776 and 857 us; the quiet set did not repeat them |

### The reference model's findings and F40 (0.8.0, 2026-11-12)

The same machine. An independent model of version 2 timing (branch
`v2-model`, `tests/model/`) generated the cases; `t_v2_findings` has one
test a finding.

| Check | Result |
|---|---|
| The model's harness (`diff.py`), the v2-model branch at c6a2a38 | Seeds 0 to 1999: 2,000 cases, 268 refusals agreed, 91 hangs agreed, 5 mismatches, all at a GAP (4 `refound-in-frame`, 1 `future-arm-hit`); none outside the GAP list. Seeds 0 to 19,999: 26 mismatches, all at a GAP (21 `refound-in-frame`). The first run of the round, with only items 1 and 3 in: 70 of 2,000, 36 outside the GAP list |
| `player_test.py` | 55 cases, 0 failures (`t_v2_findings` new; `t_v2_rule5` follows F40) |
| `compare.py`, the six examples | No exact column differs |
| ctest | Release 122 of 122; ASan and UBSan (`setarch -R`) 121 of 121 |
| Mutants (`--sim` test) | `player_v2.toml` 69 of 69 as expected (v2-60 to v2-72 new, less two found equivalent and removed with their code; v2-10 now an equivalent guard); `player_run.toml` 60 of 60 (r-v2-09 to r-v2-12 new) |
| Fuzz, Release, 8 processes | 5,000,000 inputs, 725,751 loaded, 439,487 ran to the end; no input with every deadline given, no states and no open until stopped at the budget. ASan and UBSan: 40,000 inputs, no report |
| Version 1 identity against main (7078e4d) | 438 of 439 CSV files byte-identical (the other the Stroop C program's version line) |
| Frame work, CPU 2, six runs each, load average 0.49 at the start and 0.71 at the end (5-minute 4.32) | Visuomotor 0.210 s (sd 0.003) against 0.3.0's 0.211 s; bimanual 1.137 s (sd 0.011) against 1.149 s (sd 0.007). Largest frame 632 to 639 us against 638 to 645 us, and 1,893 to 1,930 us against 1,883 to 1,952 us |

### The final review (0.7.0, 2026-11-12)

The same machine. Cases in `t_v2_review5` (the reviewer's `n*.py`,
pressed and silent where a response is in them). The reviewer's 0.6.0 build fails 14 of its
checks.

| Check | Result |
|---|---|
| `player_test.py` | 54 cases, 0 failures |
| `compare.py`, the six examples | No exact column differs; the format classes within their bounds |
| ctest | Release 122 of 122; ASan and UBSan (`setarch -R`) 121 of 121 |
| Mutants (`--sim` test) | `player_v2.toml` 59: 57 killed, 2 kept guards (v2-35; v2-41, the guard on changes at one time); v2-50 to v2-59 new, v2-44 and v2-47 replaced. `player_run.toml` 56 of 56 as expected (r-v2-05 to r-v2-08 new). `experiment.toml` 310 of 310 as expected |
| Fuzz, Release, 8 processes | 5,000,000 inputs, 725,751 loaded, 439,487 ran to the end; 248,426 budget stops, none with every deadline given, no states and no open until. ASan and UBSan: 40,000 inputs, 5,775 loaded, no report |
| Version 1 identity against main (7078e4d) | 438 of 439 CSV files byte-identical (the other the Stroop C program's version line); `session.json`: versions, `pack_id` (94), `rig_profile_sha256` (4) |
| Frame work, CPU 2, six runs each, load average 0.36 at the start and 0.61 at the end (the 5-minute average 3.98, from the jobs before) | Visuomotor 0.212 s (sd 0.005) against 0.3.0's 0.212 s (sd 0.004); bimanual 1.138 s (sd 0.006) against 1.145 s (sd 0.009). Largest frame 629 to 638 us against 639 to 646 us, and 1,881 to 1,897 us against 1,882 to 1,921 us. `trials.csv` against 0.3.0: `target_frames` (159 of 160) and `reach_target_frames` (4 of 132) only, as before |

A state row's `end_s` can be before its `t_s`: `t_s` is the entry frame's
flip onset, `end_s` the visit's end in time. A visit that ends before its
first frame shows (an `over` row ends at its entry; a response's end a
few ns before the next frame) has the earlier `end_s`. The two come from
two clocks, not from one time rounded two ways: frame onsets step by the
period in whole ns (16,666,667 ns at 60 Hz), and an `after_s` or a press
counts from an onset in exact ns.

### The third re-review and F37 (0.6.0, 2026-11-12)

The same machine. Cases in `t_v2_review4` (the reviewer's `r*.py`,
pressed and silent where a response is in them).

| Check | Result |
|---|---|
| `player_test.py` | 53 cases, 0 failures |
| `compare.py`, the six examples | No exact column differs; the format classes within their bounds |
| ctest | Release 122 of 122; ASan and UBSan (`setarch -R`) 121 of 121 |
| Mutants (`--sim` test) | `player_v2.toml` 49: 47 killed, 2 kept guards; `player_run.toml` r-v2-04 killed; `experiment.toml` 300 of 300 as expected (f37-01, f37-02, f26-05 to f26-07 new, killed) |
| Fuzz, Release, 8 processes | 5,000,000 inputs, 723,839 loaded, 438,162 ran to the end; 248,039 budget stops, 1 of them with every deadline given, no states and no open until: a trial of 61 s against the 60 s budget, not a hang. ASan and UBSan: 40,000 inputs, no report |
| Version 1 identity against main (7078e4d) | 438 of 439 CSV files byte-identical (the other the Stroop C program's version line); `session.json`: versions, `pack_id` (94), `rig_profile_sha256` (4). F37 refuses no version 1 file in the repository |
| Frame work, CPU 2, six runs each, load average 0.33 at the start and 0.48 at the end | Visuomotor 0.210 s (sd 0.003) against 0.3.0's 0.213 s; bimanual 1.151 s (sd 0.010) against 1.143 s (sd 0.009): within the spread. Largest frame 629 to 661 us (one run 1,658 us) against 640 to 649 us, and 1,881 to 1,895 us (one run 2,195 us) against 1,894 to 1,968 us |

### A real window (2026-11-10)

The machine: the Linux desktop (Debian 12, kernel 6.11.5-liquorix, Xorg
21.1, Mesa 22.3.6 on an AMD RX 6600, one 1920 x 1080 output at 60 Hz,
Xfwm4 with its compositor off, SDL 3.4.18, gcc 12.2, i5-14500, 20
threads), Release, GLX_OML full screen, the desktop open, no other job of
mine running, one run at a time. `tests/player/real_test.py` played the
participant: a uinput keyboard (or an emulated LINE board), pressing the
correct key 0.35 to 0.6 s after the expected onset, silent on every
seventh trial; the oddball's trigger role a TriggerBox on a pty pair.

| Example | Trials, run time | Load average (1 min), before / after | Frames | Dropped | Residual, \|max\| / \|p99\| | Tier | Largest frame work (`--measure`) | Responses checked |
|---|---|---|---|---|---|---|---|---|
| RDK | 30, 60.8 s | 1.22 / 1.12 | 3,494 | 0 | 3.2 / 1.8 us | 1 | 222 us; trial start 39 us | 26 of 26 |
| 2AFC (staircase, QUEST+) | 30, 55.8 s | 1.03 / 1.01 | 3,194 | 0 | 8.2 / 1.8 us | 1 | 242 us; trial start 84 us | 26 of 26 |
| Stroop | 36, 66.0 s | 0.93 / 1.02 | 3,807 | 0 | 3.2 / 1.8 us | 1 | 368 us; trial start 40 us | 31 of 31 |
| Oddball (keyboard, TriggerBox) | 16, 31.2 s | 0.93 / 0.96 | 1,713 | 0 | 2.8 / 1.8 us | 1 | 251 us; trial start 49 us | 14 of 14 |
| Oddball (LINE board, rule) | 16, about 31 s | 0.88 / 0.89 | 1,712 | 0 | 2.2 / 1.8 us | 1 | 113 us; trial start 55 us | 14 of 14 |

After the fix round (2026-11-10, 08:36 to 08:45, the same machine and
conditions; the wait for the flip record, the outputs counted at the
row's write, the tier rule):

| Example | Trials, run time | Load average (1 min), before / after | Frames | Dropped | Residual, \|max\| / \|p99\| | Tier | Largest frame work | Responses checked |
|---|---|---|---|---|---|---|---|---|
| RDK | 30, 60.8 s | 0.65 / 0.84 | 3,494 | 0 | 7.2 / 2.2 us | 1 | 323 us; trial start 32 us | 26 of 26 |
| 2AFC | 30, 55.9 s | 0.78 / 1.01 | 3,194 | 0 | 7.2 / 1.8 us | 1 | 217 us; trial start 71 us | 26 of 26 |
| Stroop | 36, 66.1 s | 0.93 / 0.98 | 3,807 | 0 | 8.2 / 1.8 us | 1 | 225 us; trial start 48 us | 31 of 31 |
| Oddball (keyboard, `--allow-below-tier`) | 16, 31.2 s | 0.90 / 0.94 | 1,712 | 0 | 7.8 / 2.2 us | 1 | 230 us; trial start 34 us | 14 of 14 |
| Oddball (LINE board, rule, tier 1 from a bounds fixture) | 16, about 31 s | 0.81 / 0.91 | 1,712 | 0 | 16.9 / 1.8 us | 1 | 77 us; trial start 50 us | 14 of 14 |

After fix round 2 (2026-11-10, 10:03 to 10:13, the same machine and
conditions; the landing frame's record settles the wait, the row from the
final result, owed per channel, the role check before the screen):

| Example | Trials, run time | Load average (1 min), before / after | Frames | Dropped | Residual, \|max\| / \|p99\| | Tier | Largest frame work | Responses checked |
|---|---|---|---|---|---|---|---|---|
| RDK | 30, 60.8 s | 1.17 / 1.02 | 3,494 | 0 | 13.2 / 1.8 us | 1 | 219 us; trial start 36 us | 26 of 26 |
| 2AFC | 30, 55.8 s | 0.93 / 0.95 | 3,194 | 0 | 14.2 / 1.8 us | 1 | 403 us; trial start 99 us | 26 of 26 |
| Stroop | 36, 66.0 s | 0.87 / 1.07 | 3,807 | 0 | 7.2 / 1.8 us | 1 | 225 us; trial start 65 us | 31 of 31 |
| Oddball (keyboard, `--allow-below-tier resp`) | 16, 31.2 s | 0.98 / 0.99 | 1,713 | 0 | 3.2 / 1.8 us | 1 | 240 us; trial start 38 us | 14 of 14 |
| Oddball (LINE board, rule) | 16, about 31 s | 0.85 / 0.79 | 1,712 | 0 | 2.2 / 1.2 us | 1 | 84 us; trial start 37 us | 14 of 14 |

Codes at the flip: write start minus the vblank p50 1.5 to 1.7 us, at
most 3.3 us. The rule's codes: write start minus the press p50 37.1 us,
max 90.1 us. `lateread` (a press stamped before the flip record, read
after it): 7 trials, each row the final result. `maxfires` (templates
with 1 and 2 fires a trial): refused at the start naming both. The
allocation count: 0 on the frame thread, all four examples. The 2AFC's
403 us frame was frame 93, early in the run, as before.

The review's cases, each 16 trials: `noafter` (the oddball with no
`after`): every row's `trig_lag_max_s` is its out rows' largest lag to
the nanosecond (it was the flip's 2 us; the frame loop's code, 0.4 to
13 ms, was lost); `lost` (one code at a flip never written, the next
trial's code the same): that trial `trig_failed` 1, every other code in
its own trial; `rule2` (a template with no response trigger on the LINE
role): 34 bytes on the pty, no response code in its trials (before: a
101 or 102 in each); `fewer` (two templates with other choices): refused
at the start naming both (before: it ran and sent 102 for a template
with only `left`).

- A response checked: the response column is the key sent, `rsp_tier`
  is 0 (the kernel's evdev time), the response's time lies inside the
  `write()` to uinput that sent the key (to its 1 us truncation), the RT
  is inside the window, and `correct` is true. A silent trial is a
  timeout. The columns of `trials.csv`, `frames.csv` and `events.csv` are
  those of a `--sim` run of the same definition.
- The largest frame was an early one of each run (index 93 to 183), and
  SDL's pump with the collectors took most of it (197 to 279 us of it).
- Codes at the flip (TriggerBox): 16 of 16 per run on the pty, the
  write's start 1.8 to 1.9 us (p50) after the planned vblank, at most 2.5
  us. A response's code from the frame loop (keyboard role): about 6 ms
  after the press. From the rule (LINE board): write start minus the
  board's press p50 79.8 us, max 110.4 us; this includes the pty pair's
  copy in Python. The bytes on the pty were the codes `events.csv` lists,
  each followed by its pulse's 0, with the box's 0 at open and 0xFF at
  close: 62 bytes.
- Before the writer thread: the 2AFC's frame 933 took 17.9 ms in each of
  three runs (one late frame, `residual_s` 16.7 ms). `strace -T` put it in
  a `write()` of `frames.csv` (17.8 ms); it repeated every 5 s under
  strace, the kernel's writeback period (`dirty_writeback_centisecs` 500).
  The same run with the data set on tmpfs: largest frame 248 us. With the
  writer thread, on ext4: 242 to 368 us, above.
- Allocations: `player_alloc_test --real` (full screen, keys and pty codes
  included) counted 0 heap calls on the frame thread inside `pl_frame()`,
  on all four examples, 11 keys each. Other threads (SDL's, the GL
  driver's, the writer, the device readers) are not counted.
- Shift+Esc in the 2AFC's trial 6: exit 3, rows 0 to 5 complete,
  `session.json` `"aborted": {"presses": 1, "reasons": ["key"],
  "trial_index": 6, "trials_written": 6, ...}` and `"closed": true`. The
  test first set the ramp to `xrandr --gamma 0.9:0.9:0.9`; the full-screen
  screen set the identity, and after the run the ramp was 0.9 again. The
  mode does not change (the player uses the desktop mode).
- The panic: the frame loop stopped in trial 6 (`--hang-at-trial 6`), then
  Shift+Esc three times 0.3 s apart: exit 99, rows 0 to 5 on disk,
  `session.json` `"closed": false`, `panic.json` with `trial_index` 6, and
  the ramp back at 0.9.
- Refused, each before the data set folder exists: `display.min_tier` 1
  in a window (its flips are tier 2), a required role the rig profile does
  not bind (the loader names it), a required role with no rig profile at
  all (the player names it), a rig file that does not exist, and
  `--participant` without `--sim` (exit 2).
- ASan and UBSan (`setarch -R` for the build and the run): the real cases
  above (12 trials each, the rule case included), the abort, the panic and
  the refusals: no report. The `--sim` tests: the unit test, the 42 cases,
  the comparison and the four examples with three participants each, no
  report, and the same bytes as the Release build.
- Fuzz, after the change (ASan and UBSan, `setarch -R`, a 6 GB scope, 4
  processes, 233 s): 60,000 inputs over the 1,017 seeds and the four
  examples, 8,197 loaded, 7,945 ran to the end, no report. The target
  stays `--sim`.

## What a real-window run needs next

- A tier for a keyboard role: the rig profile's family, a rule for a
  host-stamped input, and a loopback (How-to: give a keyboard a tier).
- A tier for an output role: ysp/rigfile.h has no tier rule for an output yet (devices_spec.md section 8 proposes one after a write-to-edge loopback).
- A real window on Windows (COMPOSITION, DXGI_FLIP) and macOS (METAL): CI
  builds the player there and runs `--sim`; no real run was made.
- A photodiode run for the display's onsets and a line-in run for a
  sound: the tiers in the data are the screen's software times. A sound
  in a real run uses `YAU_BACKEND_AUTO` (ALSA exclusive on Linux); no
  worked example has one, and no real run played one.
- A real response box (XID) and a real trigger box: the devices here were
  a TriggerBox and a LINE board emulated on pty pairs.
- `clock.csv`: the devices' clock fits are not logged.
- The loop as ysp/app.h's `yscr_run` when rig_spec 4.2.1 exists: `pl_run`
  and `pl_frame` are its two parts.
- Version 2 with a signal role: the rig profile's adapter block (G18), so
  a role binds a real device (a raw mouse, a pen, an eye tracker) with its
  units and calibration; `yexp_host.sigrole()` against it; the real run's
  input path (the screen's queue and the devices' events into the graphs,
  in place of `--samples`); a photodiode run for the cursor's latency.
- The role samples as input rows of `events.csv` (F30).

## Not verified

- A real run on Windows or macOS, and the MSVC, MinGW and macOS builds
  themselves until CI runs them (the CI lines are new).
- A photodiode or line-in measurement: no tier here is a measured one.
- A real XID box, parallel port or trigger box; a role with direction
  `both` (it opens as an output; its events go to the input bridge; no
  rule).
- A real run with a sound stimulus.
- A panic keeps the rows the writer wrote; a trial that ended in the 2 ms
  before a hang, or whose last flip record or codes' records never came,
  can lose its row. Measured only with the test hang (`--hang-at-trial`).
- The wait for the flip record on a real display: no landing frame was
  late in these runs, so the wait was never longer than the record's
  usual arrival; the `--sim` test and the header's test show the late
  case.
- A rule's codes from two input roles into one output: the arms are kept
  per output and the press time picks one; no rig here has two boxes.
- `aep`, `artwork`, `video`: refused.
- Version 2 in a real run: refused with a signal role (G18). The trace
  rows' path through the writer's side buffer (`pl_wq_push_row()`) runs
  only in a real run, so no test reaches it; `--sim` writes the rows in
  place.
- A real run's trial-end cost with traces: the copy to the writer is not
  measured (Results, "Format version 2").
- Format version 2 on Windows, macOS and emcc.
