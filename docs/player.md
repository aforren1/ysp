# The player

Status: v0.13.2, 2026-11-17. It loads a pack, checks its experiment
definition, lowers the definition onto the ysp headers and runs it: in a
real window with the participant's keyboard, mice and the rig's devices,
or on the simulated display (`--sim`). It is a program (`player/`, target
`ysp_player`, option `YSP_BUILD_PLAYER`), not a single header. It reads
format versions 1 and 2 ([experiment_format.md](experiment_format.md), D1
to D15 and part 3). A version 2 signal role takes its adapter from the rig
profile's `signals` (G18; raw mice only). Real runs are measured on the
Linux X11 desktop only (Results). The runner protocol and `ysp/app.h`'s
loop (rig_spec 4.2.1) are not built.

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

## How-to: run a version 2 definition with signal roles on a rig

A signal role (experiment_format.md 3.5) takes its adapter from the rig
profile's `signals` block (devices_spec.md 11.2, "The signal block"). The
first adapter is a raw mouse.

1. Give each signal role a signal in the rig profile:
   `rigfile_profile --signal left display_cm 0.0078125 click` (docs/
   device.md, "How-to: set a signal role's adapter"). Display units need
   the profile's `view`.
2. Run the player with that profile: `ysp_player PACK --out DIR --rig
   NAME ...`. At load it checks each signal role against its signal: a
   role with no signal (unless `required` is false), a `position2d` role
   without `allow_relative`, and units that do not convert stop the start,
   with the role's name.
3. For each role with `device` `click`, the console asks for a click:
   `player: click the mouse for signal role 'left' (left button)`. Click
   the left button of that role's mouse in the player's window. The first
   trial starts after the last role's click; counts that reach the player
   before that click are not used (a count that comes later is, even if
   it is stamped before the click). A click from a mouse that is bound already, or with device 0
   (injected input, a touchpad on Windows), is refused, and the console
   asks again.
4. A role with `device` `any` takes every mouse's counts and needs no
   click. It must be the only raw mouse signal in the profile.
5. If a bound mouse is removed, the console says so and asks for a click
   at once. The trial in progress gets no input from that role meanwhile;
   no trial starts until the click comes.

The window must have the input focus: on Linux and Windows, reports come
only while the player's window is in front. `session.json` has each
role's adapter and mouse (`signal_roles`), every click and removal with
its time (`signal_bindings`), and the counts the player did not use
(`raw_mice`).

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
   Rows are in time order. An optional fifth column, `arr_s` (0.13.2), is
   when the report reaches the player, at or after `t_s` (tests: one late
   report among on-time ones, as two mice of a `device` `any` role give).
2. Give `--sim-gain ROLE=UNITS` for each relative role
   (`position2d_relative`). The `--sim` adapter integrates the counts at
   that many role units a count. An absolute role multiplies the position
   by its gain (1 by default). A relative role with no gain, and no signal
   in the rig profile, is a bad argument (exit code 2). Reading F32 of
   experiment.md. A role that `--sim-gain` does not name and the rig
   profile's `signals` has takes the profile's adapter: its scale, bounds
   and `late_s`, as a real run does; `device` `click` binds the scripted
   participant with no click.
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

Two more test options, `--sim` only:

- `--sim-hz HZ` sets the simulated display's rate (default 60). The
  period is `1e9 / HZ` ns, rounded to whole ns; 60 gives ysp/screen.h's
  own 16,666,667 ns, so a 60 Hz run is byte-identical to one without the
  option. The five C programs that `compare.py` runs take the same option,
  and `compare.py`, `player_test.py` and the model's harness take `--hz`.
- `--test-sample-late ROLE=S` gives that role's samples to the player S
  seconds after their `t_s`, the time they keep. It is a device whose
  reports come late (a USB transport, an FTDI latency timer). It replaces
  the role's `arr_s` values.
- `--test-live-cap N` (a real run) makes one poll hold N raw mouse
  samples, not 8,192, so a test can fill it: the session stops.

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

   Under `--sim`, `--measure` keeps the in-place writes out of the frame
   work and the trial start, and prints their largest frame separately: a
   real run queues each write to its writer thread, and `--sim` does the
   write on the frame thread. On ext4 one such write blocked for 17 ms
   (below, "The 17 ms frame").
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
| When the row is written | When its last frame's flip record and every one of its codes' device records are in, on every output channel, or 0.5 s (`PL_OUT_WAIT_NS`, 30 frames at 60 Hz) after its last frame, with the codes still owed counted in `_failed`. A record that comes after that has `flags` 0x4000 (`PL_OUT_AFTER_ROW`) in `events.csv`: its row counted it failed. At the session's end the player shows 133 ms more frames (`PL_END_DRAIN_NS`, 8 at 60 Hz), and up to 0.5 s plus 33 ms (`PL_RECORD_LAG_NS`: the record that passes a row's bound comes up to 2 frames at 60 Hz after its flip; 32 frames at 60 Hz) while a row still waits for a code's record. Each time is a whole number of frames of the display, rounded up | When its last frame's flip record is in; 133 ms of frames at the end |
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
| `signal_bindings` (0.13.0) | Not written | A real run with signal roles: one object a click that bound a mouse or a removal, in order: `role`, `event` (`click` or `removed`), `mouse` (SDL's id), `name` (the mouse's name), `t_s` (from the session's first frame) |
| `raw_mice` (0.13.0) | Not written | A real run with signal roles: the raw mouse reports that no role used. `other_mice` (a mouse that no role has), `device0` (no device: injected input), `absolute` (positions: a tablet, a virtual machine), `wheel` (scroll reports, 0.13.1: 0.13.0 counted them in `absolute`; no adapter reads the wheel), `refused_clicks`, `samples_lost` (a role's samples over the poll's buffer of 8,192; 0.13.2 stops the session at the first, so 0 in a session that ends), `inputs_lost` (other inputs over the collectors' buffer of 1,024) |

No other field changes. A version 1 data set keeps every other byte
(Results).

## Reference: pl_check()

`pl_check()` runs the player's load on a definition and a pack, and keeps
all its results in memory. It is for the designer, which checks the
definition on each change with the player's own loader
([designer.md](designer.md), "Architecture"). It is in the player library
(`ysp_player_lib`; declared in `player/player.h`, built in
`player/lower.c`).

```c
int  pl_check(pl_player* p, const pl_check_desc* d, pl_check_result* r);
void pl_close(pl_player* p);
```

Call `pl_close(p)` after you read `r`, and before the next `pl_check()` on
the same `p`. Every pointer in `r` points into `p`. `pl_player` is large
(1.6 MB with GCC 12 on x86-64 Linux): give it static or heap storage.

### Inputs

| Field of `pl_check_desc` | Meaning |
|---|---|
| `pack` | A `ypak_desc`: the pack in memory (`data`, `size`, 8-byte aligned) or a range reader (`reader`, `reader_ctx`). With a reader, the player reads each entry that it uses whole, and the reader's cursor verifies each chunk. A `path` also works; the player maps the file read-only. |
| `def_text`, `def_len` | The definition's bytes. `NULL`: the pack's experiment entry. The tables, textures, fonts and other entries come from the pack. |
| `seed` | The session seed (D8). The same inputs give the same result. |

### The result

`pl_check()` returns `r->status`:

| Status | Tier | Meaning |
|---|---|---|
| `PL_CHECK_DEFINITION` | 1 | `yexp_load()` refused the definition. `r->report` holds its diagnostics, errors and warnings, each with a line and a column (`YEXP_MAX_DIAGS`, 32, are kept; `n_errors` counts all). |
| `PL_CHECK_REFUSED` | 2 | The player refused the definition after `yexp_load()` accepted it, or the pack did not open. `r->message` is the refusal, the same text the command line prints. The check stops at the first refusal, as the command line does. |
| `PL_CHECK_OK` | | The definition loads and lowers. |

`r->object` and `r->object_name` give the object that a tier 2 refusal names, when the player knows it:

| `r->object` | `r->object_name` | Refusals that give it |
|---|---|---|
| `PL_OBJ_NODE` | the node's path | a leaf's rules and variables (`pl_leaf_prepare()`), the node checks (`pl_check_nodes()`: a name in both `vars` and `data`, an `if` field that is not 0 or 1), `@participant` with a text field |
| `PL_OBJ_STIMULUS` | its name | a stimulus's lowering (`pl_stim_make()`: rdk, text, sound, image, the kinds the player does not lower), its static keys, its textures, a channel of one of its properties |
| `PL_OBJ_TEMPLATE` | its name | the response's collector (`pl_tmpl_open()`), a version 2 template's graph, controls, forms and exits (`pl_v2_load()`), the steps' `if` fields |
| `PL_OBJ_METHOD` | its name | a method's open (`pl_method_open()`) |
| `PL_OBJ_SIGNAL_ROLE` | its name | a signal role's adapter (`v2_rig_signals()`) |
| `PL_OBJ_FIELD` | its name | a session field's value (`pl_fields_init()`) |
| `PL_OBJ_NONE` | `NULL` | every other refusal: the pack, the format version, the timeline's size, the data set's columns, and refusals inside `yexp_load()`'s host checks (those are tier 1) |

The object comes from the step of the load that is running when the
refusal happens, not from the message text. No `pl_fail()` site changed.

`r->needs` (tier 3) lists what a real run asks of the rig or the session.
The check does not refuse them. It continues with a stand-in for each,
and lists each in the order the load meets it. After a tier 2 refusal,
the list holds the needs met before the refusal; after a tier 1 refusal it
is empty.

| Need | `name`, `value` | What a real run asks | The check's stand-in |
|---|---|---|---|
| `PL_NEED_FIELD` | the field; 1 text, 0 integer | a required session field, `--field NAME=VALUE` | the integer 1, or the text `x` |
| `PL_NEED_CALIBRATION` | | `display.calibration` `"required"`: `--rig` with a calibration, or `--cal FILE` | the nominal calibration of `player_make_rig`: sRGB primaries, D65 white at 100 cd/m2, gamma 2.2 |
| `PL_NEED_VIEW` | 1 deg, 2 cm | `display.units` `deg` or `cm`: a rig profile with a view | the view of `player_make_rig`: 570 mm away, 1920 px over 518.4 mm |
| `PL_NEED_DISPLAY_TIER` | the tier | `display.min_tier`: a display that gives that tier | not checked, as in `--sim` |
| `PL_NEED_DEVICE` | the role; its direction (0 in, 1 out, 2 both) | a required role that the keyboard cannot take (an output, or an input that does not list `keyboard`): a device in the rig profile | no device, as in `--sim`: an input role is the scripted participant, an output role the in-process sink |
| `PL_NEED_ROLE_TIER` | the role; the tier | a role's `min_tier`: a checked loopback in the rig profile, or `--allow-below-tier` | not checked. `--sim` prints a line to stderr here; `pl_check()` prints nothing |
| `PL_NEED_SIGNAL` | the signal role; 1 when required | a signal role's adapter in the rig profile's `signals` | the `--sim` adapter |
| `PL_NEED_SIM_GAIN` | the signal role | a relative signal role: `--sim` needs `--sim-gain` or the rig's signal | gain 1: one role unit a count |

These stand-ins apply always, with no need listed:

- The display: none. ysp/gfx.h opens on its null backend at the simulated
  display's size, 800 x 600 px, and at 60 Hz. The null backend makes every
  texture, buffer, glyph set and program, and uploads and draws nothing.
  A texture is still decoded, and a glyph run's curve set is still read.
- The audio device: a device that opens at the pack's format and never
  starts a thread. A sound's audio entry is still loaded.
- The roles: unbound, as in `--sim`. A role whose first family is
  `keyboard` takes key choices; any other role takes box buttons.
- No rig profile. The loader's role and signal role checks against a rig
  do not run (`r->report->skipped` says so).

### What it does not do

- It writes no file and makes no folder. It reads no file other than the
  pack's path when you give one. It does not read `/dev/urandom`.
- It opens no device, window, GL context, audio device or data set, and
  starts no thread. `test_player_check` counts the process's threads
  during a check with a sound stimulus: one.
- It does not change the clock. It does not call `yrt_virtual_begin()`
  and needs no virtual clock. `--sim` runs on the virtual clock (from
  `main.c`, before the load) so that two runs give the same bytes. In the
  load, two parts use the clock: the simulated display, whose vblank grid
  runs on ysp/rt.h's clock, and the null audio device (on the virtual
  clock ysp/audio.h's own virtual device; on the real clock miniaudio's
  null device, a real-time thread). The check opens neither, so it runs
  on either clock. The same check on the real clock and on the virtual
  clock gives the same result (`test_player_check`).
- It writes nothing to stdout or stderr.
- These checks of a real run do not run: a response trigger's rule on a
  device role (`pl_rules_check()`, which needs a role bound to a device),
  the display's tier in the lead-in frames, and the data set's checks of
  stream names and trace columns (`yds_stream()` needs an open data set).
  The data set's checks of the `trials.csv` columns run, through a writer
  that discards the bytes.

Two calls must not run at the same time in one process. The loader's
host checks use static scratch (`h_rules()` and `h_check_ops()` in
`player/host.c`).

### Measured

PLACEHOLDER

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

## Explanation: late input, and exits that wait for it (0.12.0)

Status: built in 0.12.0 (2026-11-15), from the second draft of the
design, after the reviewer's findings H1, M1 to M3, L1 and L2 on the
first (3f6d21c; table at the end). The release's gates are in "Results
at 0.12.0", below. The user's decision of
2026-11-15: "wait for complete input". ysp/sig.h 0.7.0 places a late
report at its time (docs/sig.md, "Late input"). The player takes a hit
only when the input is complete up to the hit's time for every role that
feeds the hit's control. Exits and state changes lag by the lateness. The
drawing does not lag. Every recorded time stays exact. The format's note:
docs/experiment_format.md, D20, "DECIDED (2026-11-15, the user)".

Why: a hit that the player takes before the reports up to its time are in
can be wrong. A frame's pass, a settle at the onset or an entry-now arm
finds a hit, and a late report with a time at or before the hit then
moves it or removes it (`late_hits_voided`). With 0.7.0 and hits taken at
once (601f7c0, 2026-11-15, Linux desktop, i5-14500, the model's harness,
1,000 cases a run), the guard stopped more sessions than 0.11.0 at every
lateness. Guard stops per 1,000 sessions at 0, 5, 10, 20, 40 and 60 ms
late: at 60 Hz 23, 30, 31, 68, 92 and 337 (0.11.0: 1, 5, 9, 43, 69 and
76); at 500 Hz 6, 72, 96, 124, 105 and 204 (0.11.0: 0, 24, 41, 58, 58
and 57). The bimanual example stopped at 10 ms late.

### Terms

- **Lateness bound** `B(r)` of a signal role r: the longest time from a
  report's stamp to the poll that reads it.
- **Poll time** `P`: the time of the newest poll whose reports the player
  fed. In `--sim` it is the frame's onset: the player feeds the samples
  that arrived by the onset (`v2_feed()`).
- **Newest stamp** `S(d)`: the latest stamp of a report of device d that
  the player fed in this trial (since the trial's reset). sig.h keeps
  time order per device (M1), not per role.
- **Complete to**, per device: `C(d) = max(S(d) - 1 ns, P - B(d))`. Every
  report of d with a stamp at or before `C(d)` is fed. The two parts:
  - `P - B(d)`: a report stamped at or before it reached a poll by `P`.
  - `S(d) - 1 ns`: a device does not go back (sig.h drops a sample older
    than its device's last, `dropped_order`), so after a report stamped
    `S(d)` no report of d stamped before it comes. A second report at
    the same stamp can come, so `S(d)` itself is not complete. A raw
    mouse's counts are the exception sig.h makes (T6: placed, not
    dropped); a source whose stamps can go back uses `P - B(d)` only.
    That the sources of ysp/input.h stamp in order is an assumption
    (the final review's L3): it is not measured.
- **Complete to, per role**: `C(r)`, the least `C(d)` over the role's
  devices. A device of the role not yet seen in the trial gives
  `P - B(d)`. In `--sim` a role has one device. As built
  (`v2_horizon()`), the player keeps one newest stamp per role and uses
  `max(S(r) - 1 ns, P - B(r))`, this rule for a role of one device (the
  final review's L4). A `device` `any` role (0.13.2) takes `P - B(r)`
  and no newest stamp: a mouse not heard from yet can send a report
  stamped before another mouse's newest, within its bound. 0.13.0 and
  0.13.1 took each seen mouse's `max(S(d) - 1 ns, P - B)` and counted a
  mouse not seen yet as still, and a timed exit after such a report went
  first (the review of 0.13.1; `t_v2_any_unseen`).
- **A device with no reports** (a still relative mouse, a pen out of
  range): `S(d)` does not move, so `C(d) = P - B(d)`. For a source that
  the kernel stamps when it queues a report and that one poll reads to
  its end (evdev raw mice and pens, keyboards), `B(r)` is 0: no report
  stamped before the poll's read can come later. So a still device of
  that kind holds nothing back. `B(r)` 0 for evdev was an assumption from
  how the kernel stamps and queues (L3). Measured in 0.13.0 with `P` the
  poll's start: 0 idle, 1.6 ms with every core busy, 15 to 36 ms at twice
  that ("Signal roles on a rig"); a real run's raw mouse has 4 ms by
  default.
- **The roles of a control** `R(c)`: the signal roles with a path to the
  control's detect in the graph. A detect that reads only timeline
  channels has none.
- **The horizon** `G` of a template's graph: the least `C(r)` over the
  roles that feed any of the template's controls or movements, and `P`
  when there is none (no role, or every such role with `B(r)` 0 and so
  `C(r) = P`).

### Where each bound comes from

1. `--sim`: `--test-sample-late ROLE=S` gives `B(ROLE) = S`; every other
   role has 0.
2. The rig profile's `late_s` for the role's signal (G18; ysp/rigfile.h
   0.6.0), a measured lateness. In `--sim` too, for a role that takes
   the profile's adapter.
3. The default, with no measured value: 4 ms for a raw mouse in a real
   run (0.13.0: "Signal roles on a rig", below). In a real run `P` is
   when the frame's poll started, not the onset, so this is the time
   from a report's stamp to the queue the poll drains, not a frame. None
   for a source with its own clock or a transport (a serial device with
   host stamps behind an FTDI timer, LSL, an eye tracker): the player
   will refuse such a role without a measured bound, by name, when an
   adapter for one exists (only raw mice have one).
4. Keys and buttons that feed a key response: 0 only when the OS stamps
   them on arrival (evdev, the OS's key events). A response box stamped
   behind a transport (a serial box with host stamps) needs a measured
   bound, else the player refuses it, as in item 3 (L1). A key role's
   bound counts in `G` only when an exit compares its response with
   hits.

sig.h's window: `catchup_ns` is the larger of 50 ms and
`ceil((max B(r) + 3 periods) * 8 / 7)`. An exit taken at the horizon
arms the next state at the exit's time, up to `max B(r)` plus two frame
periods before the newest input: the window must reach that far back.
The window keeps only about 7/8 of `catchup_ns`, because sig.h takes a
checkpoint each `catchup_ns / 8`, and the newest input can be up to
`catchup_ns / 8` plus a period past the newest checkpoint.

### Which hits wait

Every hit, by one rule: a hit at t is final when `t <= G`. A hit whose
roles all have `B(r)` 0 is final at the poll that found it (its time is
at or before `P`, and `G = P`), so in a template with no late role
nothing waits and the player behaves as 0.11.0. One horizon per graph,
not one per control: the exit test compares hits of several controls
and timed exits by time (below), and that needs one bound under which
every candidate is known. Cost: in a template with one late role, an
exit on a control that reads only an on-time role waits too.

Holding a hit or a derived event needs two sig.h calls (0.7.0; their
text goes into docs/sig.md, "Late input"). Reason: sig.h counts
`late_hits_voided` for every hit it handed out, and `arm_derived_stale`
for every derived event it handed out, used or not (H1). So the player
leaves what is not final in sig.h's queues.

- `ysig_detected_to(g, out, cap, t)` (the reviewer's text, M3): hands
  out, in R's order among them, at most `cap` queued hits whose time is
  at or before `t`, and returns their number. The queued hits later than
  `t` stay queued and are not handed out. A queued hit is the caller's
  only once handed out: a run again that no longer gives a queued hit
  removes it, with no count; a run again that gives it again leaves it
  queued once; `arm_hits_superseded`, `arm_hits_retracted` and
  `late_hits_voided` count only hits handed out. `ysig_detected()` is
  `ysig_detected_to()` with no limit. Across calls with a rising `t`, a
  hit held by an earlier call comes out after hits that R has after it;
  within one call, the order is R's. The queue holds `YSIG_QUEUE_CAP`
  plus `catchup_calls` hits; a hit that finds it full is counted in
  `hits_lost`, which a caller that holds hits treats as a guard stop. A
  reset or a trial reset empties the queue, held hits too.
- `ysig_derived_to(g, out, cap, t)`: the same for derived events. It
  hands out, in R's order among them, at most `cap` queued events whose
  time is at or before `t`; later ones stay queued and are not handed
  out. A queued event is the caller's only once handed out: a run again
  that no longer gives it, or gives it otherwise, removes it with no
  count, and gives the new one queued; `arm_derived_stale` counts only
  events handed out. `ysig_derived()` is `ysig_derived_to()` with no
  limit. The queue holds `YSIG_QUEUE_CAP` plus `catchup_calls` events;
  an event that finds it full is counted in `derived_lost`, a guard stop
  for a caller that holds events. A reset or a trial reset empties it.

The queues' size (M2; L5 of the final review, 2026-11-16): the player
computes the count it needs and sets `catchup_calls` to at least that.
Between two drains each detect and each emit queues at most one hit or
event an evaluation: one a sample of a feed part (at most
`YSIG_QUEUE_CAP`, 64, samples), one at the frame, and what a run again
gives from `max B` plus two periods back (as far back as an exit's arm
reaches), at `V2_WINDOW_HZ` per role. So with this size neither queue
fills at any bound, 0 included (under the `V2_WINDOW_HZ` assumption that
the log size makes too), and `hits_lost` and `derived_lost` stop the
guard at any bound. 0.12.0 as first built sized for `max B` plus one
period and counted them only while a bound was above 0.

### The order of exits (D20 rule 4)

At each pass of the exit test (after each part of a poll, after
`ysig_frame()`, after a key response's end), the candidates are: the
hits drained up to `G`, the `after_s` times, an until's `timeout_s`, the
steps' end, and the response's end. The player takes the earliest
candidate whose time is at or before `G`; at one time, the list order.
A timed candidate (an `after_s`, a timeout, the steps' end, a response's
end) that the frame's window reaches but that is later than `G` waits
for `G`, as a hit does. So no candidate is taken while a hit at or
before its time can still come: a hit that becomes final late cannot
lose to a timed exit that is later in time. With every bound 0, a timed
candidate is taken by the lead rule as in 0.11.0, and a hit later than
the poll can lose to it, as in 0.11.0 (R6, 2026-11-15). After a take at
t, the next
state arms at t (a past arm; sig.h runs the window again), and its hits
up to `G` are drained in the same pass, so a chain of states at one
frame runs as in 0.11.0.

### Frames, times and steps

- **Entry time**: the exit's time, exact, as 3.11 rule 1. The next
  state's controls arm at it, `at_s` counts from it, and F41's clamp
  holds.
- **Entry frame** (amends the hit clause of the D20 amendment and 3.11
  rule 2): the first frame at or after both the exit's time and the
  time the exit became final, that is, the frame whose poll moved `G`
  past it. A timed exit lands on the later of its lead-rule frame and
  that frame. F40 holds with this entry frame. `after_s` counts from
  the entry frame's onset (D20 rule 2), so it counts from what the
  participant saw.
- **Leaving a state** (3.11 rule 8): the old state's events that landed
  between the exit's time and its take were shown and stay in
  `events.csv`; `ytl_remove()` takes the ones that have not fired.
- **The trial's end**: the exit to `end` is taken when `G` reaches its
  time. The flush and the next trial's reset are barriers: sig.h drops a
  report stamped before a barrier that arrives after it (`late_barrier`;
  R drops it too), and the held values (an adapter's state) carry over
  the reset, so the next trial would read the value before that report.
  So the player waits for complete input up to each barrier (the
  coordinator's ruling C2, 2026-11-15, after the user's decision):
  - The flush's barrier is the graph's newest input or frame (the end
    frame's onset, or later). The flush and the trial's held rows wait
    until `G` over every signal role (fed by the template or not: a
    barrier drops a report of any device) reaches the barrier minus
    1 ns. The next trial does not start before the flush: frames between
    draw no trial. In `--sim` with one role whose bound is its lateness
    `L`, the next trial starts on the first frame after the end frame
    whose onset is at or after the end frame's onset plus `L` minus 1 ns.
  - The reset cannot wait: at the trial's first onset `T` the input is
    complete only to `G`, which is before `T` while a bound is above 0.
    So the reset's barrier goes at `G + 1 ns` (`G` over every role, as
    above), not at `T`. The trial's arms at `T` are then ahead of the
    input and wait in sig.h for input past them (an arm ahead of the
    input). The experiment's nodes see the input from `G + 1 ns` on;
    detects are not armed before `T`, so a control's hits do not change.
  - With every bound 0, `G` is the poll's time: nothing waits, and the
    reset goes at `T`, as in 0.11.0.

### The response window

- **Keys** (a key response; arrival-stamped, `B` 0): the collector is
  unchanged and arms at the trial's first frame (3.13). Its end is a
  candidate at its time, taken when final (the flip record, 3.11 rule
  11) and at or before `G`.
- **Signal choices** (3.13): the collector's input is the movements'
  derived events. The player takes them with `ysig_derived_to(..., G)`,
  so the later ones stay in sig.h's queue (H1). It arms the collector
  (a `from` hit, a state's entry) when it takes that event, at its time
  t: the events before t go in before the arm, the rest after it,
  as 3.11 rule 4's order per event. The collector's clock (`duration_s`,
  the deadline) is `G`, not the frame's onset, so a deadline is not
  judged on input that is not in. Its end lags as an exit does.
- The arm (`yrsp_arm()`) clears the collector's onset, but `$rt_s` and
  the deadline count from the `from` stimulus's onset (3.13). When that
  stimulus lands before the movements' `from` (for example at the
  trial's first frame, with the hold later), the player gives the onset
  back after the arm. Before (0.11.0 too), the window never got an onset
  again, and a state that waits on the response never ended
  (`player_test` `t_v2_signal_from_first`, with `from` after the arm as
  its control). A `from` that is never shown (a state left at its entry,
  so its steps do not land) still opens no window: a state whose only
  exit is on the response then waits until the session's budget, in
  0.11.0 too. That is the definition's, not the wait's.
- A derived event that a run again changes after the player took it
  (at or before `G`) counts `arm_derived_stale`, and the guard stops: a
  report came later than its bound.

### What the data shows

- Exact: hit times, the `time` and `hit` columns, the sample traces, the
  response's times, and the `end_s` of a state row that a hit or a
  response ends.
- Later by up to the lateness plus a frame: a state's entry frame (the
  state rows' `frame`, `t_s`, `planned_s`) and what it shows. An until's
  wait has an entry frame too (rule 9 makes it a state, with no row of
  its own): the steps before it end at a time that the horizon must pass,
  and an until met at its arm is a hit that waits, so the steps after it
  land later too.
- Later by the lag of the entry frame it counts from: the `end_s` of a
  row that an `after_s` or an until's `timeout_s` ends (it counts from the
  row's entry frame or from the until's wait's), so a row whose own entry
  frame did not lag can end later. From there on the trial runs on a
  later clock: a timed exit can now lose to a hit or a press that it beat
  with no late role, or win against one, and the trial's next states,
  outcome and response can differ. That is the cost of counting from what
  the participant saw (D20 rule 2).
- Later by up to the lateness plus a frame: `trials.csv`
  `time_elapsed_s`, since the trial's end lags as an exit does.
- The lag of the exit that started a visit (L2): the poll time at which
  the player took it, minus its time (the visit's entry time). On a state
  row of `events.csv` it is `t_s` minus the `end_s` of the visit before,
  so the file has no column for it. A later `after_s` exit counts from
  the entry frame, so this lag moves it too. With `B` 0 it is under a
  frame, as in 0.11.0.
- Later by up to the lateness plus a frame, beyond the end's own lag:
  the next trial's first frame, when the flush waits (above, "The
  trial's end"). Before that wait, a report stamped before a barrier
  that arrived after it was dropped, and the next trial read a stale
  held value (the model's harness, 60 Hz, 10 ms late, seed 102928: the
  hand left the region 10 ms after trial 0's exit, and trial 1's entry
  arm found it still in the region, a hit the hand did not make).
  `player_test` `t_v2_barrier` holds the shape: at 60 Hz the reset at
  the horizon is the part that matters, at 500 Hz the flush's wait.
- In a real run (0.13.0), each "plus a frame" above is plus two frames.
  The player polls at the frame's start, up to a frame before the onset,
  and the input is complete to that poll's time `P` minus `B`.
- Exact: the frame traces. A frame row reads its items at its frame's
  record onset (3.7), and it waits until the input is complete to that
  time (`G` over every signal role reaches it). Before (27d52a1), the
  row read at the record, a frame after the onset, and missed the
  reports that were still late: at 500 Hz and 10 ms late, the five rows
  after a step of the hand read the place before it. `player_test`
  `t_v2_frame_rows_late` holds this, with the same samples on time as its
  control. A row can wait the largest bound in frames more; the queue is
  sized for it at load.
- Input before the trial's onset (the reset at `G + 1 ns`). The
  experiment's nodes take the reports stamped between `G + 1 ns` and the
  onset `T`; with every bound 0 they start at `T` from the held values.
  What reads them (measured 2026-11-15 at 27d52a1, 60 and 500 Hz, 10 ms
  late, a sample 5 ms before `T` against one 30 ms before `T` and against
  the same samples on time):
  - No change: the sample traces (their rows are cut at `T`), the frame
    traces (after the fix above), and the stateless controls (`cursor`,
    `distance`, a `region`'s test without `speed_below`): their value at
    `T` and later is the held value.
  - Changes, open: a node with time state restarts at `G + 1 ns`, not at
    `T`. A `speed` (a `peak`, a `region` with `speed_below`) takes the
    step before `T` into its first `lag_s` after `T`, and a `peak` with
    no `from` keeps it (`peak` 10000 against 0). A `signal_lost` counts
    the silence from the last report before `T` (500 Hz: its hit 4 ms
    and 8 ms earlier than on time). The player cannot restart the
    experiment's nodes at `T` without a barrier there, and a barrier at
    `T` drops the late reports before it from the adapters (the stale
    hand of seed 102928). A fix needs ysp/sig.h: a trial reset whose
    barrier and whose restart of the experiment's nodes have two times.
  - A movement's origin (no `origin`): the format's is the last position
    fed before the arm (3.11, 3.13). With a bound above 0 the reports
    before `T` reach the collector through the trial's graph, so the
    origin is the hand's place at `T`. With every bound 0 the derived
    events of the polls between two trials go to no collector
    (`v2_feed_part()`: no trial's graph), and in 0.11.0 and up to 6ace730
    the origin was the place at the last trial's end: a hand that moved
    between trials made a response at the trial's first sample (60 Hz, a
    step 5 ms before `T`: `go` at `rt_s` 0.0003; 500 Hz, 1 ms before:
    0.001). Now, when the reset is at `T`, the trial's start gives the
    collector the place that each movement's `of` holds at `T`, as the
    emit would have, before the arm (`pl_v2_trial_start()`).
    `player_test` `t_v2_move_origin`: a step 0.4 periods before trial 1's
    onset makes no response; its control, the step 0.4 periods after,
    makes one.
- Exact: a `{"signal": N}` column. With a bound above 0 it is read at the
  end frame's onset from its node's ring when the flush's wait is done
  (the input complete to the barrier), and the trial's row waits for it
  (`pl_v2_row_waits()`). Before (to 5ead8ef) it read the value at the end
  frame, with the input complete only to `G`: a report between `G` and
  the onset was not in it (60 and 500 Hz, 10 ms late, a step 5 ms before
  the end frame: 0 against 100 on time; `player_test`
  `t_v2_signal_column_late`, with the samples on time and a step after
  the onset as its controls). With every bound 0 it is read at the end
  frame, as 0.11.0.
- Not exact, open: a `when` column's test (and a signal inside a `when`)
  still reads the value at the end frame with the input complete to `G`.
- New in `trials.csv`: `exit_lag_max_s`, the trial's largest exit lag;
  `late_frames` (sig.h's count of the trial's drawn frames that a late
  report changed).
- New in `session.json`: `exit_lag` (`n`, `mean_s`, `max_s`) over the
  session's exits; `late_beyond_bound` per role (reports whose stamp was
  at or before the role's `C(r)` when fed: the bound is too small); the
  late counts in `sig_arms`.

### What still stops the guard

- `late_hits_voided`: now only a hit the player took (final by the
  bound) that a later report voids, which means a report came later than
  its role's bound. A graph that is not the trial's (between trials, or
  another template's) gets every poll too, and no one takes its hits and
  derived events, but sig.h counts one handed out that a late report then
  voids. So the player hands them out only up to `G` over every role
  (`v2_feed_part()`). Before (156d2b5), it took all of them: a hit of
  the ended trial's control, held past `G` at the trial's end, came out
  between trials, and the hand's late report voided it (the model's
  harness, 500 Hz: seed 100308 at 20 ms late, 100162, 100601 and 100823
  at 40 ms, those and 100045 and 100572 at 60 ms; `player_test`
  `t_v2_ended_hits`).
- `late_far` (a report later than the window, `catchup_ns`), `late_full`,
  `late_off`, `late_unverified`.
- `hits_lost` and `derived_lost`, when a bound is above 0 (M2).
- A hit or a derived event that comes out at or before a horizon that
  the player already used (N1, R5), counted in `late_beyond_bound`: only
  in a template that waits (a role that feeds it has a bound above 0).
  With every bound 0 such a hit is an arm ahead of the input, and it
  loses by rule 4 as in 0.11.0 (R6); 9109ecd stopped on it (the model's
  harness, 500 Hz, no lateness, seed 100619; `t_v2_beyond_bound0`).
- The arm counts, as before: `hits_retracted`, `rows_stale`,
  `derived_stale`, and the fallbacks `past_pinned`, `past_far`,
  `past_full`, `past_off`, and `unverified`.
- Not: `late_frames`, `late_barrier`, and the per-role
  `late_beyond_bound` of reports fed (counted, in the data).

### Format 3.11 rule 4

Rule 4 feeds each event, drains its hits and takes an exit before the
next event. Under the wait the player takes exits at `G`: it feeds a
poll with `ysig_feed_n()` in parts, drains the hits up to `G` and takes
them by time. With every bound 0, `G` is the poll's time, and the two
orders must give the same results (the check of the proposed 3.11 rule 4
note: the oracle and the model harness fed per event against batched).
With a bound above 0 they differ by design: rule 4's "a report that
arrives in a later poll with a time before a taken exit does not undo
it" no longer arises, since no exit is taken before its time is
complete.

Measured (2026-11-15, 2d52faa, Linux desktop, i5-14500, `--sim`): each
example with `--test-feed-each` against batches, 60 and 500 Hz, with 0
and 10 ms late (`--test-sample-late` on every role). `trials.csv`,
`events.csv`, `frames.csv`, the streams and `session.json` (less its
timing fields) are equal in 7 of the 8 runs. In the bimanual example at
60 Hz, 10 ms late, only `late_frames` differs: 2 of its trials count 2
frames more fed one by one (2797 against 2795, 2792 against 2790), and
`session.json` `sig_arms.late_frames` 4 more (39534 against 39530). Every
exit, time, outcome and value is equal. `late_frames` is sig.h's count
of drawn frames that a run again changed; likely (not verified), fed one by one, a frame can be
changed by one report's run again and changed back by the next, which
one part's run again does not see. It is a diagnostic count, not a value
the trial uses; to make it equal needs ysp/sig.h (count a frame by its
value after the part, not after each run again). Open.

### The 17 ms frame (bimanual, 500 Hz, 10 ms late)

The bimanual example at 500 Hz, 10 ms late, showed one frame of 16.9 to
17.7 ms in some runs, at a different frame each time (294,695 at
601f7c0; 292,946 and 293,469 at 27d52a1 and 2d52faa, 1 run in 2 or 3).
Tracy (2d52faa, the player at CPU 2, the capture at CPU 4, load 1.5):
the frame of 16.95 ms (frame 246,048) was 16.90 ms in one
`pl.write_inplace` under `pl.flip_records`. `--sim` writes its rows on
the frame thread, and the output folder is on ext4 (95 % full): the
`write()` waited for the kernel's write-back, as the 2AFC's frame 933
did before the writer thread (Results). A real run queues the write to
its writer thread, so the frame thread does not wait. The same holds for
the trial start's 1.9 ms at frame 281,055 (every run): 1,877 in-place
writes of the last trial's sample trace rows at the flush (2.0 ms in
Tracy).

The fix is in the measurement, not the frame: `--measure` takes the
in-place writes out of the frame work and the trial start, and prints
their largest frame. Linux desktop, i5-14500, CPU 2, 2d52faa plus this
change, load 0.8 to 0.9, three runs: frame work max 1,657, 131 and 766
us (frames 238,528, 290,968 and 87,724), trial start max 94 to 97 us,
p99.9 29 to 37 us, no frame over the 2 ms period; in-place writes max
1,863, 16,892 and 1,876 us in a frame. The largest frame work left moves
from run to run, has no zone of its own in the profile and is not
explained (not verified: preemption on CPU 2 is the likely cause).
Tracy's own cost makes about 900 frames over 0.4 ms in a captured run,
so the capture cannot place a frame of that size.

### Results at 8c3cb20 (2026-11-15)

Linux desktop, i5-14500, GCC 12.2, Release, `--sim`.

The model's harness (`tests/model/diff.py --no-shrink --jobs 6`, exact
compare), 1,000 cases a run: 60 Hz seeds 102001 to 103000, 500 Hz seeds
100001 to 101000. Mismatches (at a GAP) and guard stops by counter,
against 0.11.0 (`b_`) and 43e2d10 (`n2_`), whose runs used that day's
`diff.py`:

| Hz | Late | 8c3cb20 mismatches | 8c3cb20 guard stops | 0.11.0 mismatches, stops | 43e2d10 mismatches, stops |
|---|---|---|---|---|---|
| 60 | 0 | 6 (6) | 0 | 7 (7), 1 unverified | 6 (6), 0 |
| 60 | 5 ms | 1 (0) | 1 late_unverified (102932) | 45 (22), 5 | 329 (46), 4 late_unverified |
| 60 | 10 ms | 1 (1) | 1 late_unverified (102788) | 51 (23), 9 | 372 (45), 1 late_unverified |
| 60 | 20 ms | 4 (0) | 4 late_unverified (102032, 102139, 102587, 102932) | 104 (38), 43 | 391 (44), 7 late_unverified |
| 60 | 40 ms | 1 (0) | 1 late_unverified (102932) | 137 (52), 69 | 392 (41), 3 late_unverified |
| 60 | 60 ms | 1 (0) | 1 late_unverified (102478) | 150 (55), 76 | 395 (41), 1 late_unverified |
| 500 | 0 | 4 (4) | 0 | 4 (4), 0 | 5 (4), 1 unverified |
| 500 | 5 ms | 0 | 0 | 101 (38), 24 | 395 (42), 1 late_unverified |
| 500 | 10 ms | 0 | 0 | 127 (41), 41 | 400 (42), 0 |
| 500 | 20 ms | 1 (0) | 1 late_hits_voided (100308) | 153 (48), 58 | 408 (43), 0 |
| 500 | 40 ms | 3 (0) | 3 late_hits_voided (100162, 100601, 100823) | 154 (49), 58 | 426 (43), 0 |
| 500 | 60 ms | 5 (0) | 5 late_hits_voided (100045, 100162, 100572, 100601, 100823) | 159 (48), 57 | 469 (62), 308 (286 late_far) |

0.11.0's stops are `pinned` and `unverified`; every mismatch off a GAP
at 8c3cb20 is a stopped case. Open: the `late_unverified` stops at 60 Hz
(102788 is with the reviewer) and the `late_hits_voided` stops at 500 Hz
from 20 ms. A hit the player took as final was voided by a report that
the bound covers, so the player's horizon or sig.h's placement is wrong
in those cases; not looked into.

Exit lag over the harness's sessions (`session.json` `exit_lag`), mean
and max: 60 Hz: 0 ms 2.16 and 16.67 ms; 5 ms 14.36 and 21.67; 10 ms
17.81 and 26.33; 20 ms 30.26 and 36.33; 40 ms 48.37 and 56.33; 60 ms
67.66 and 76.33. 500 Hz: 0 ms 0.05 and 2.00; 5 ms 5.81 and 7.00; 10 ms
10.44 and 12.00; 20 ms 20.46 and 22.00; 40 ms 40.45 and 42.00; 60 ms
60.44 and 62.00. The lag is at most the lateness plus a frame, as the
design says ("What the data shows").

The examples at 10 ms late (2d52faa, the data the same at 8c3cb20):
every session finishes (visuomotor 160 trials, bimanual 132), no guard
count, `late_beyond_bound` 0. Exit lag mean and max: visuomotor 16.69
and 25.00 ms at 60 Hz, 10.00 and 11.00 ms at 500 Hz; bimanual 16.67 and
16.67 ms at 60 Hz, 10.00 and 10.00 ms at 500 Hz. `late_frames` (sum of
the trials, and sig.h's count): visuomotor 6,515 at 60 Hz and 54,880 at
500 Hz; bimanual 39,528 (39,530) at 60 Hz and 329,433 (329,442) at 500
Hz. sig.h's count also has the frames between trials.

Frame work (`--measure`, in-place writes left out, the pack and samples
of `scratchpad/fast/time/in`, CPU 2, one run at a time, load 0.56 to
0.81 at each start, three runs each; max with its frame, p99, p99.9, in
us):

| Example | Hz | Late | Max | p99 | p99.9 | Writes in place, max in a frame |
|---|---|---|---|---|---|---|
| visuomotor | 500 | 0 | 68, 66, 64 (frame 0) | 3.7 to 3.8 | 15.1 to 15.3 | 618 to 627 |
| visuomotor | 500 | 10 ms | 65, 65, 77 | 19.4 to 19.6 | 27.0 to 27.2 | 621 to 764 |
| bimanual | 500 | 0 | 83, 1,874 (frame 395,935), 75 | 6.0 | 7.0 to 22.9 | 1,860 to 1,866 |
| bimanual | 500 | 10 ms | 115, 101, 1,719 (frame 361,176) | 27.6 to 27.8 | 28.7 to 38.1 | 1,941 to 17,508 |
| visuomotor | 60 | 0 | 62, 64, 82 | 16.0 to 16.6 | 26.0 to 26.3 | 630 to 641 |
| bimanual | 60 | 0 | 80, 80, 88 | 30.0 to 30.2 | 46.8 to 49.1 | 1,849 to 1,857 |
| 0.11.0 visuomotor | 60 | 0 | 649 to 658 | 17.6 to 18.8 | 628 to 632 | (in the frame work) |
| 0.11.0 bimanual | 60 | 0 | 1,901 to 1,958 (frame 33,866) | 29.9 to 30.0 | 1,030 to 1,039 | (in the frame work) |

0.11.0's largest frames are its in-place writes: 8c3cb20's writes at
the same frame (33,866) take 1,849 to 1,857 us. With the writes left
out, 8c3cb20 at 60 Hz has p99 within 3 us of 0.11.0's. At 500 Hz no
frame is over the 2 ms period; one frame in 400,000 of 1.7 to 1.9 ms
comes in some runs, at a different frame each time, with no late input
too (not explained; above, "The 17 ms frame"). 10 ms late costs about
22 us at p99 in the bimanual example and 16 us in the visuomotor
(sig.h's runs again).

### Results at cfb3856 (2026-11-15)

Linux desktop, i5-14500, GCC 12.2, Release, `--sim`; the model's
harness as above (1,000 cases a run, `--jobs 6`, exact compare). The
load was 2.0 to 7.9 during the runs (the reviewer's oracle jobs); none
of these numbers is a timing.

| Hz | Late | 156d2b5 mismatches (at a GAP), guard stops | cfb3856 mismatches (at a GAP), guard stops |
|---|---|---|---|
| 60 | 0 | 6 (6), 1 late_unverified (102401) | 6 (6), 0 |
| 60 | 5 ms | 1 (1), 1 late_unverified (102401) | 0, 0 |
| 60 | 10 ms | 1 (1), 1 late_unverified (102401) | 0, 0 |
| 60 | 20, 40, 60 ms | 0, 0 | 0, 0 |
| 500 | 0, 5, 10 ms | 4 (4), 0, 0 at 0 ms; 0 and 0 at 5 and 10 ms | 4 (4) at 0 ms; 0 at 5 and 10 ms; 0 stops |
| 500 | 20 ms | 1 (0), 1 late_hits_voided (100308) | 0, 0 |
| 500 | 40 ms | 3 (0), 3 late_hits_voided (100162, 100601, 100823) | 0, 0 |
| 500 | 60 ms | 5 (0), 5 late_hits_voided (100045, 100162, 100572, 100601, 100823) | 0, 0 |

The causes: every `late_hits_voided` stop was a hit of a graph that was
not the trial's, handed out past `G` between trials ("What still stops
the guard"); 102401's `late_unverified` was sig.h's 2 (b) retest with a
late disarm_at as `L` (docs/sig.md, "When the header tests").

`player_test` 72/0 at 60 and 500 Hz. `compare.py` rc 0 at both rates. The
60 Hz identity against 0.11.0 (`late5/ident.py` over `player_test`'s
data) is the same at 156d2b5 and cfb3856: 975 files identical, 342
differ, in the declared columns only. Feed-each against batches, both
examples, 500 Hz, 10 ms late: the data and `session.json` (less its
timing fields) equal; and equal to 156d2b5's batch data.

### Results at 0.12.0 (2026-11-15)

The release's gates. Linux desktop, i5-14500, GCC 12.2, `--sim`. The load
is given with each group; only the frame work is a timing.

Mutants (`tests/mutate/player_*.toml`, `-j 4`, load 4 to 12 from other
jobs): every `--sim` mutant of the five lists, 228 of 228 as expected.

| List | Mutants | Killed | Kept |
|---|---|---|---|
| `player_v2.toml` | 127 | 116 | 11 |
| `player_run.toml` | 64 | 60 | 4 |
| `player_lower.toml` | 26 | 25 | 1 |
| `player_eval.toml` | 6 | 5 | 1 |
| `player_host.toml` | 5 | 4 | 1 |

- 14 anchors of `player_v2.toml` had gone stale; 12 keep their intent at
  the new code. v2-91 and v2-92 are removed with their code: 0.12.0
  disarms every armed control of a state at the exit's time
  (`ysig_disarm_at()`), not only the arms ahead of the input. v2-85 (an
  until's next part disarms the state's exits) is now killed.
- 36 new mutants, v2-97 to v2-132, one or more for each rule of 0.12.0:
  the horizon (per-role bound, `C(d)`'s parts, the poll's claim, which
  roles feed `G`), the drains to `G`, timed candidates that wait, the
  entry frame, `catchup_ns`, the queues' size, `hits_lost` and
  `derived_lost`, N1 and R5, the trial's barriers (C2), frame rows that
  wait, the signal column, the movement's origin, the signal choice's
  `from` onset, `v2_resp_pend_first()`, `v2_arms()`, the drain of a graph
  that is not the trial's, and `ysig_disarm_at()` at a state's end. 31 are
  killed; v2-104, v2-107 and v2-119 only at 500 Hz (`YPL_HZ`, which
  `tests/player/mut_test.c` gives `player_test.py` as `--hz`).
- New `player_test` cases for the mutants the cases did not see, each
  with a control: `t_v2_newest_stamp`, `t_v2_cross_roles`,
  `t_v2_entry_frame_after`, `t_v2_window_reach`, `t_v2_frame_rows_long`,
  `t_v2_resp_pend_hit`, and `vm_guard_arm_full` in `t_v2_findings`
  (v2-89: since 0.12.0 a state's end disarms at the exit's time, and that
  call met the full log first, so the old case stopped on a late count,
  not on the arm's own).
- Kept, with the reason in the list: v2-100 and v2-128 are equivalent;
  v2-108 (`catchup_ns` from two periods) is not reached in `--sim`, where
  an exit arms less than `B` plus one period back. Open: v2-110 and
  v2-111 (the queues' size, `hits_lost` and `derived_lost`) are not
  reached. Six movements of an 8 kHz hand at 200 ms late, and the same at
  16 kHz, lost no derived event, so the test held fewer events than the
  size assumes; not looked into.
- Not run: the 35 mutants of the real path (test `real`), which need a
  display.

ASan and UBSan (the `sanitizers` preset, Debug, `setarch -R` for the
build and the runs, `UBSAN_OPTIONS=halt_on_error=1`, a 6 GB memory scope,
load 5 to 10): `player_test` 78 of 78 at 60 Hz and at 500 Hz (the 500 Hz
run is more than the gate asks), `vm_guard_arm_full` again after it was
added; the model's harness at 60 Hz, seeds 102001 to 102200, `--jobs 4`:
no lateness, 200 cases, 1 mismatch (at a GAP), no guard stop; 10 ms late,
200 cases, no mismatch, no guard stop. No sanitizer report in any run.

The other gates (Release, load 6 to 13 from other jobs, none a timing):

- ctest, `DISPLAY` unset: 127 of 127.
- `player_test`: 78 of 78 at 60 Hz and at 500 Hz. `compare.py`: exit 0 at
  60 and 500 Hz, no exact column differs.
- The 60 Hz identity against 0.11.0 (b995055's player, built in scratch,
  `player_test.py --player`, over the same cases): 1,053 data files
  identical, 420 differ. 22 cases differ beyond the declared fields
  (`player_version`, `headers.sig`, `exit_lag`, `late_beyond_bound`,
  sig.h's new late counts, `trials.csv` `exit_lag_max_s` and
  `late_frames`): 14 cases with a lateness bound above 0, which wait by
  design; with every bound 0, `t_v2_move_origin` and
  `t_v2_signal_from_first` (the two fixes, "What the data shows" and "The
  response window"), and 6 cases in sig.h's diagnostic counts only
  (`sig_arms` `caught_up`, `ahead_cancelled`, `hits_superseded`: sig.h
  0.7.0 against 0.6.1, and one arm per detect in
  `t_v2_two_exits_one_ctl`). 0.11.0 fails 24 checks of the 0.12.0 cases.
- The model's harness (`--no-shrink --jobs 6`, exact compare), 1,000
  cases a run, 60 Hz seeds 102001 to 103000, 500 Hz seeds 100001 to
  101000. Mismatches at 60 Hz: 6 at no lateness, all at a GAP, 0 at 5,
  10, 20, 40 and 60 ms; at 500 Hz: 4 at no lateness, all at a GAP, 0 at
  every lateness. No guard stop in any of the 12 runs: the same as at
  cfb3856. Exit lag (`session.json` `exit_lag`, now in the harness's
  log), mean and max over the sessions: 60 Hz 2.16 and 16.67 ms at no
  lateness, 14.36 and 21.67 at 5 ms, 17.81 and 26.33 at 10 ms, 30.26 and
  36.33 at 20 ms, 48.37 and 56.33 at 40 ms, 67.64 and 76.33 at 60 ms;
  500 Hz 0.05 and 2.00, 5.81 and 7.00, 10.44 and 12.00, 20.46 and 22.00,
  40.45 and 42.00, 60.44 and 62.00. The lag stays under the lateness
  plus a frame.
- Feed-each against batches, both examples, 500 Hz, 10 ms late on every
  role (the C programs' samples from `compare.py --hz 500`): every file
  byte-identical, `session.json` too. Visuomotor: 160 trials, exit lag
  mean 10.00 and max 11.00 ms over 480 exits, `late_frames` 54,880;
  bimanual: 132 trials, 10.00 and 10.00 ms over 160 exits, `late_frames`
  329,442; `late_beyond_bound` 0, no guard count.

Frame work at 500 Hz (`--measure`, in-place writes left out, CPU 2, one
run at a time, nothing else of mine running, load 1.71 to 1.98 at each
start; the pack and the C programs' samples of `compare.py --hz 500`,
which may not be the inputs of 8c3cb20's runs). Three runs each; max with
its frame, p99, p99.9, in us:

| Example | Late | Max | p99 | p99.9 | Trial start max | 8c3cb20: max; p99; p99.9 |
|---|---|---|---|---|---|---|
| visuomotor | 0 | 377 (frame 108,719), 70, 73 | 3.7 to 3.8 | 15.3 to 15.5 | 54 to 56 | 64 to 68; 3.7 to 3.8; 15.1 to 15.3 |
| visuomotor | 10 ms | 67, 78, 64 | 20.3 to 20.5 | 27.0 to 27.5 | 49 to 52 | 65 to 77; 19.4 to 19.6; 27.0 to 27.2 |
| bimanual | 0 | 87, 596 (frame 187,688), 90 | 5.9 to 6.0 | 7.6 to 10.4 | 61 to 70 | 75 to 1,874; 6.0; 7.0 to 22.9 |
| bimanual | 10 ms | 666, 749, 1,206 (frames 311,825, 281,012, 147,833) | 28.9 to 29.0 | 30.5 to 38.7 | 97 to 190 | 101 to 1,719; 27.6 to 27.8; 28.7 to 38.1 |

No frame was over the 2 ms period. The bimanual example at 10 ms late,
three runs more at load 0.97 to 1.06: max 145, 824 (frame 279,263) and
109 us, p99 28.9 to 29.0, p99.9 30.0 to 34.8, trial start max 102 to 109
us. So the single large frame comes in some runs at a different frame
each time, as at 8c3cb20 ("The 17 ms frame"); at the higher load it came
in each of the three runs. p99 at 10 ms late is about 1 us (4 to 5 %)
above 8c3cb20's in both examples; with no lateness it is the same. The
in-place writes, left out: 632 to 790 us (visuomotor) and 1,871 to
16,931 us (bimanual) in a frame, the 17 ms write-back wait again.

The final review's round (2026-11-16, Linux desktop, i5-14500, GCC 12.2,
Release, `--sim`; with ysp/sig.h 0.7.0's H1 and the player's L5):

- `player_test` 78 of 78 at 60 Hz (11 s) and at 500 Hz (13 s), load 1.2
  to 1.3. `compare.py` exit 0 at 60 and 500 Hz, no exact column differs
  (load 2.8).
- The model's harness (`--no-shrink --jobs 6`; seeds 102001:103000 and
  100001:101000 are 999 cases each), load 1.9 to 4.7: 60 Hz, 6
  mismatches at no lateness, all at a GAP, 0 at 10 ms; 500 Hz, 4 at no
  lateness, all at a GAP, 0 at 10 ms. No guard stop in the four runs,
  with `hits_lost` and `derived_lost` now in the guard at every bound.
  As many as at V2SIG10 (which seeds was not compared).
- Mutants: `player_v2.toml` 129 (v2-111 at the new guard lines; v2-133
  and v2-134 for L5), `-j 4`, 1,029 s (load 2.1 to 4.0): 129 of 129 as
  expected, killed 116, survived 13. The mutants of the other player
  lists were not run in this round.
- Feed-each at 60 Hz: "Open in 0.12.0" below.

Open in 0.12.0 (each above, in "What the data shows" and "Results at
8c3cb20"):

- Input before the trial's onset for nodes with time state (`speed`,
  `peak`, `signal_lost`): the reset at `G + 1 ns` restarts them there,
  not at `T`. A fix needs a ysp/sig.h trial reset with separate barrier
  and restart times.
- A `when` column's test (and a signal inside a `when`) reads the value
  at the end frame with the input complete only to `G`.
- At 500 Hz, one frame in about 400,000 of 1.7 to 1.9 ms, with no late
  input too, at a different frame each run: not explained.
- `--sim` writes its rows in place on the frame thread; `--measure` takes
  them out of the frame work. A real run queues them to the writer.
- No test fills sig.h's queues: mutants v2-110 (the queues' size) and
  v2-111 (`hits_lost` and `derived_lost` do not stop the guard) survive,
  so nothing tests that a lost hit or event stops the guard. Since the
  final review (L5) the size covers every bound and they stop it at any
  bound; still not reached by a test.
- v2-108 (`catchup_ns` from two periods) is not reached in `--sim`: an
  exit there arms less than `B` plus one period back.
- The 35 mutants of the real path (test `real`) are not run: they need a
  display.
- Fed one event at a time (`--test-feed-each`), the bimanual example at
  60 Hz and 10 ms late counts more `late_frames` than fed in parts (2d52faa,
  "What still stops the guard"): 2 trials 2 frames more each (2,797
  against 2,795 and 2,792 against 2,790), `session.json`
  `sig_arms.late_frames` 39,534 against 39,530. The gate of 0.12.0 ran
  feed-each at 500 Hz only. Run again at 60 Hz, 10 ms late on every role,
  after the final review's fixes (2026-11-16, Linux desktop, i5-14500,
  load 2.5, `--sim`, the C programs' samples of `compare.py`): the
  visuomotor example equal in every file; the bimanual example the same
  difference, `late_frames` only, every exit, time, outcome and value
  equal. A diagnostic count; open.

### The reviewer's findings on the first draft (3f6d21c), and the answers

The reviewer confirmed one `G` per graph, the order of exits, the entry
frame, F40, F41, 3.11 rule 8 and the trial's end.

| Finding | Answer here |
|---|---|
| H1 (high): holding derived events in the player does not prevent `arm_derived_stale`; sig.h counts any event `ysig_derived()` handed out (a bimanual midpoint's emit at a frame at 100, hand 2's report at 97 arriving at 105, `G` at most 90) | `ysig_derived_to()`, built as `ysig_detected_to()`; the collector takes events from it up to `G` |
| M1 (medium): `S(r)` assumes one device a role; sig.h drops by device | Complete-to per device, `C(r)` the least over the role's devices; an unseen device gives `P - B(d)`; device 0 ("any") gives `P - B(r)` |
| M2 (medium): the queues' size | A count the player computes from its controls and report rates over `max B` plus a frame; `hits_lost` and `derived_lost` stop the guard while a bound is above 0 |
| M3 (medium): the text of `ysig_detected_to()` | The reviewer's text, verbatim; `ysig_derived_to()` adapted from it |
| L1 (low): keys and buttons are 0 only when stamped on arrival | A response box behind a transport needs a measured bound or is refused |
| L2 (low): the lag per state row | A state row's `t_s` minus the `end_s` of the visit before (no column: the coordinator's decision, 2026-11-15); `trials.csv` `exit_lag_max_s` |

## Results

### The review of 0.13.1 (0.13.2, 2026-11-17)

A reviewer read the late-input changes of 0.13.0 with no account of
them but the code and the contracts, and reproduced findings in `--sim`.
Linux desktop (i5-14500, GCC 12.2, Release).

| Finding | Fix | Test |
|---|---|---|
| A `device` `any` role counted a mouse not heard from yet as still, so its first report, late within the bound, could come after a horizon the player used: a timed exit after it went first, silently | `P - B` for an `any` role, no newest stamp (Explanation, "Signal roles on a rig") | `t_v2_any_unseen`: a second mouse's report 3 ms late, a timed exit 1 ms after it; the hit wins. Its control (the newest stamp's claim) gives `timed`. Mutant `v2-152` killed |
| `bounds` with `device` `any`: two mice's counts reach the bounded integrator out of time order, and the clip depends on the order (`end_y` 2 for 1.21875) | Refused at load, by name | `t_v2_rig_signals` (`anybounds`); mutant `v2-154` killed |
| A feed with a limit (a key response's end) stopped at its first sample past the limit and claimed input complete to the limit; a sample queued behind it, stamped before the limit, came out under a horizon already used: a guard stop for a report within its bound (0.12.0 on, `--sim` too) | The claim ends before the earliest stamp still queued (`v2_fed_to()`); the response waits for the whole feed | `t_v2_limited_feed` (the reviewer's case); mutant `v2-153` killed with the reviewer's guard message |
| A full poll buffer dropped a role's counts and only counted them | The session stops, by name | `real_test.py livecap` (`--test-live-cap 16`, 200 reports in a frame); mutant `v2-155` killed |
| `late_seen_s` missed the samples that a feed with a limit left for the frame's whole feed | Measured as each sample is polled | None: a real run's poll and a key response in one frame |

The samples file got an optional arrival column (`arr_s`) for the first
test; `--sim` now runs a `device` `any` role. Two mutants' anchors moved
(`v2-98`, `v2-99`; killed again). `player_test.py`: 82 of 82 at 60 and
500 Hz. The model's harness, seeds 0 to 299, 10 ms late: 0 mismatches and
0 guard stops at 60 and 500 Hz. The real cases `signals`,
`signals_examples` and `livecap` pass (the mutant runner's baseline).
Not fixed: a removal that SDL's event does not name ("What a
real-window run needs next").

### Hand-moved mice and the wheel count (0.13.1, 2026-11-17)

Linux desktop (i5-14500, GCC 12.2, X11 at 1920x1080 60 Hz, SDL 3.4.18).
Two USB mice moved by hand: a Dell Universal Receiver and an HP USB
Optical Mouse. The profiles are the real cases' (0.25 px a count for
visuomotor, 0.0078125 cm a count for bimanual), not a measurement of
these mice.

| Run | Result |
|---|---|
| bimanual, two roles bound by click; the receiver unplugged in trial 0 and bound again by a click 13.8 s later | The bindings in order: left, right, left removed, left. In the trace, left changed in 252 of 332 frames before the removal, in 0 of 825 while unbound (right in 191 of them), and in 451 of 697 after the click. Stopped with Shift+Esc 31 s into the first trial's 46 s of tracking |
| visuomotor, `device` `any` | Both mice moved the cursor, with no click. The reach ended at (-5.5, 270.75) px for a target at (0, 268.8) |
| visuomotor, `device` `click` | Only the bound mouse moved the cursor; the other's 268 reports went to `other_mice`. Both reaches ended on their targets |
| Each run | 0 frames dropped, `late_seen_s` 0, 0 reports past the bound, 0 samples or inputs lost, no guard stop. Largest `evaluate_s` 1.68 ms (bimanual) |

Found: 0.13.0 counted scroll reports (`YIN_AXIS_WHEEL`) in `raw_mice`'s
`absolute`, which is for tablets and virtual machines (12 and 2 in two
runs). 0.13.1 counts them in `wheel`. `real_test.py signals` sends 3
scroll reports and checks `wheel` 3 and `absolute` 0; mutant `v2-151`
(the wheel's branch removed) killed. `player_test.py`: 80 of 80 at 60
and 500 Hz. One baseline run of the mutant harness (`signals` and
`signals_examples`, 8 trials) had 1 failing check, and the runner
deleted its log; the failure did not come back in 4 later runs of
`signals` and 5 of `signals_examples`. Not reached by hand: a bimanual
trial's end, the reach blocks and visuomotor's rotation.

### Signal roles on a rig (0.13.0, 2026-11-16)

Linux desktop (i5-14500, GCC 12.2, Debian 12, X11 at 1920x1080 60 Hz,
SDL 3.4.18), uinput mice. Explanation: "Signal roles on a rig".

| Check | Result |
|---|---|
| `player_test.py` (`t_v2_rig_signals`, `t_v2_two_templates_late`, the two-template check in `t_v2_frame_rows_late`) | 80 of 80 at 60 and 500 Hz. The profile's 0.25 px a count at 32 px a cm gives the data of `--sim-gain 0.0078125`; a negative y scale with bounds y in [-2, 3] cm gives the data of the positive one with the counts negated (the bound holds y at 3); `--sim-gain` wins over the profile; `late_s` 0.004 from the profile; refusals by name (a `position2d` role, `workspace_cm` against `display_cm`, display units with no view, no signal and no `--sim-gain`). Bimanual with both hands 2 and 3 ms late at 500 reports a second: no guard stop (0.12.0: "5 logs full") |
| The model's harness (`tests/model/diff.py`, seeds 0 to 299, 10 ms late) | 60 and 500 Hz: 0 mismatches, 0 guard stops |
| `real_test.py signals` | Two mice bound by click (the first one's second click refused), 4 trials on their counts, the left mouse removed in trial 3 (which waits on it) and a third bound by a click then; counts moved before the last click not in trial 0's hit (2.03125 cm, on the grid from 0); the bindings in `session.json` in order |
| `real_test.py signals_examples` | bimanual (two mice by click) and visuomotor (one role, `device` `any`, two mice moving) for 15 s on circles at 500 reports a second a mouse, then Shift+Esc: exit 3, no guard stop, 0 or 1 frame dropped, under 60 frames of no trial between trials. 12 runs in a row after the last fix, the last 4 without the sort of each poll's reports by stamp that the first 8 had (it measured no better, and went); before the fix, 1 failure in 3 to 4 (the flush and the idle graphs, above) |
| Other real cases (`refuse`, `afc2`, `oddball`, 10 trials) | Pass |
| Mutants | player_v2.toml: 13 new on `--sim` killed, 6 new on the real path killed (`v2-143` to `v2-148`), 2 recorded survivors (`v2-147`: no output shows sig.h's poll time; `v2-150`: no case reaches a waiting frame row across an idle graph's reset). `rr-01` (run.c, its anchor moved): killed |

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

### Two hands and sig.h 0.6.1 (0.11.0, 2026-11-14)

A plain two-mouse definition stopped on the guard at 60 Hz with 1 kHz
samples, one `arm_unverified` at each entry of its state b (the
coordinator's `repro_unv`): roles lh and rh, both relative; state a exits
on a hold region of lh (`stay_s` 0.1155), and b exits on `far` (lh), on
`farr` (rh) and on `after_s`. The hold ends between samples, so the lh
sample after it finds the hit, and the player arms both hands' controls
at the hit's time. rh's sample at the same time is not fed yet, so
`farr`'s past group had no `L` in R, and sig.h 0.6.0 counted it at once
(docs/sig.md, "When the header tests"). sig.h 0.6.1 makes it pending:
made at its place as before, decided by rh's next sample or the next
frame (exact), and counted only on what a graph with no window would make
differently (docs/sig.md, "Pending groups").

The player changes in one place: its guard reads sig.h's counts after a
trial's flush too, since a group still pending at the flush is counted
there (`v2_unv_guard()`). It disarms an ended state's controls as before:
a pending arm is past, and the disarm at the input time makes it and ends
it (rule 3). `player_test.py`'s `t_v2_two_hands` runs the repro's shape
(two trials, `done`) and one where b exits at its entry while `farr`'s arm
is still pending.

The Linux desktop (i5-14500, GCC 12.2, Release), against v2-fast (a1c2717,
sig.h 0.6.0):

| Check | 60 Hz | 500 Hz |
|---|---|---|
| `repro_unv` | 2 trials, `unverified` 0 (0.6.0: the guard stops the first) | the same |
| `player_test.py` | 59 cases, 0 failures; `unverified` 0 in 127 sessions | 59, 0 failures; 0 in 121 |
| `compare.py`, the six examples | No exact column differs; 70 of 70 CSV files byte-identical to a1c2717's | the same, 70 of 70 |
| The model's harness, 1,000 cases (106001 to 107000; 107001 to 108000 at 500 Hz) | 112 refused (agreed), 50 hangs agreed, 4 mismatches, all at a GAP; no guard stop | 132 refused, 35 hangs, 7 mismatches, all at a GAP; no guard stop |
| A two-hand sample (below), seeds 108001 to 110500 | 1,271 cases ran: 1 guard stop (0.6.0: 2) | 1,275: 1 (0.6.0: 2) |

The model's generator makes one hand, so the harness never made this
shape. The two-hand sample is a scratch script, not in the repository:
it takes the generator's cases, puts every other region on a second
absolute hand with samples at the same times at another point, and runs
the player alone (the model has one hand, so no rows are compared). The
seeds with no region (1,024 of 2,500 at 60 Hz, 1,022 at 500 Hz) and
those the pack refuses do not run. 0.6.1 removes one stop at each rate (seed 108318). The one left at
each rate is the same on 0.6.0: at 60 Hz condition 1 (a group
let in inside another's block, seed 109772), at 500 Hz 2 (c) (a cancelled
group's disarm, seed 109726).

### A 500 Hz display on `--sim` (0.11.0, 2026-11-14)

The Linux desktop (i5-14500, Debian 12, liquorix 6.11, GCC 12.2, Release),
`--sim --sim-hz 500` (2 ms frames) against `--sim` at 60 Hz. A `--sim` run
gives no timing tier: a 500 Hz display's tier needs a photodiode run on it.

#### Limits counted in frames

At 500 Hz a frame is 8.3 times shorter. A limit counted in frames that
covers a span of time covers 8.3 times less.

| Limit | 60 Hz | 500 Hz in 0.10.0 | What runs short | 0.11.0 |
|---|---|---|---|---|
| The sound's scheduling lead (`run.c`, `pl_sounds()`, 2 frames) | 33 ms | 4 ms | A sound starts late, with no error: ysp/audio.h's record says LATE. The shortest safe lead measured at the defaults is 40 ms (docs/audio.md, "Shortest safe lead"; ALSA exclusive, 480-frame periods: 39.86 ms) | Two frames or `PL_SOUND_LEAD_NS` (40 ms) past the next frame, whichever is further: 56.7 ms at 60 Hz, 42 ms at 500 Hz |
| A row's wait for its codes' device records (`PL_OUT_WAIT_FRAMES`, 30) | 500 ms | 60 ms | The row is written, the codes still owed counted failed; a frame trace's row is written with no `t_s` | `PL_OUT_WAIT_NS`, 0.5 s: 30 frames at 60 Hz, 250 at 500 Hz. The frame traces' waiting rows are sized from it |
| The session's last flips (8 frames; 32 while a code is owed) | 133 ms, 533 ms | 16 ms, 64 ms | Owed codes counted failed; later device records lost | `PL_END_DRAIN_NS` 133 ms and `PL_OUT_WAIT_NS` plus `PL_RECORD_LAG_NS` (33 ms): 8 and 32 frames at 60 Hz, 67 and 267 at 500 Hz |
| A real run's lead-in (`PL_LEAD_IN`, 30 frames) | 500 ms | 60 ms | Less settle time before the first trial; `display.min_tier` judged on fewer records | `PL_LEAD_IN_NS`, 0.5 s |
| sig.h's catch-up window (4 frames) | 67 ms | 8 ms | An arm before the oldest checkpoint falls back (`arm_past_far`) and the guard stops | Not changed: no arm ran past it (below) |
| The window's logs (1,024 calls, 16,384 ring writes) | | | Two raw mice at 8 kHz put 1,067 calls in four 60 Hz frames: the log runs full, the window is cleared, and a past arm right after falls back (`arm_past_full`). At 60 Hz too | Every role, and at least two, at 8 kHz over five frames, plus 16 calls a frame; each report's ring writes in its role's downstream set. 1,414 calls at 60 Hz, the floor of 1,024 at 500 Hz |

Counted in frames on both sides (a flip record's lag), so not changed:
`V2_STATE_ROWS`, `PL_PENDING_ROWS`, `PL_FRAME_RING` (256 frames, 512 ms at
500 Hz), `PL_RDK_STEPS` (a flush every 128 ms). A per-frame capacity gets
easier: `PL_DEV_RING`, `PL_EVENT_QUEUE`, `PL_WALK_PER_FRAME`.

In ysp/screen.h (not changed in this round; a real run only):

- GLX_OML gives up on a swap's event 4 periods past its target
  (`yscr__glx_acquire()`): 67 ms at 60 Hz, 8 ms at 500 Hz. A later event's
  frame completes as OCCLUDED with an estimated time.
- `YSCR_MAX_DONE` (16 records a `begin()`): a stall of 267 ms at 60 Hz, 32
  ms at 500 Hz, drops the oldest records. ysp/screen.h counts them
  (`f.done_lost`, since the open); the player does not read the count.
- `YSCR_MAX_JOBS` (16 trigger jobs in flight): 32 ms of codes on every frame
  at 500 Hz. A job with no room is dropped and counted in the screen's
  `trig_lost`, which no call returns; `yscr_trigger()` had returned OK. A
  bound device then sends no record, so the row counts the code failed after
  `PL_OUT_WAIT_NS`; an unbound role's sink counts the firings.
- The sync guard's window of 64 flips: 1.07 s at 60 Hz, 128 ms at 500 Hz.

#### Measured

The model's harness (`tests/model/diff.py`; mismatches after shrinking).
"Guard" is a stop on sig.h's counts; every stop below lists its counter.

| Run | Cases | Refused (agreed) | Hangs agreed | Mismatches (at a GAP) | Guard stops |
|---|---|---|---|---|---|
| 0.10.0 at 500 Hz, seeds 100001 to 102000 | 2,000 | 254 (254) | 83 | 7 (7) | 0 |
| 0.10.0 at 500 Hz, `GEN_MULTI=1`, 41001 to 42000 | 1,000 | 130 (130) | 30 | 7 (7) | 0 |
| 0.10.0 at 60 Hz, 102001 to 103000 | 1,000 | 132 (132) | 53 | 7 (7) | 1 (`unverified`, seed 102590) |
| 0.10.0 at 500 Hz, `--test-record-delay` 2, 4, 8; 100001 to 100500 | 500 each | 72 | 24 | context only | 1, 1, 2 (`unverified`) |
| 0.11.0 at 500 Hz, 103001 to 105000 | 2,000 | 300 (300) | 63 | 16 (16) | 0 |
| 0.11.0 at 500 Hz, `GEN_MULTI=1`, 42001 to 43000 | 1,000 | 125 (125) | 46 | 4 (4) | 0 |
| 0.11.0 at 60 Hz, 105001 to 106000 | 1,000 | 161 (161) | 33 | 8 (8) | 0 |
| 0.11.0 at 60 Hz, `GEN_MULTI=1`, 43001 to 43500 | 500 | 61 (61) | 22 | 2 (2) | 0 |

No arm ran past the 4-frame window at 500 Hz (`past_far` and `past_full`
0 in every session). Fewer arms are in the past at 500 Hz: the examples
run 1 (visuomotor) and 0 (bimanual) arms again, against 160 and 16 at
60 Hz.

Late samples (`--late`, 0.10.0, seeds 100001 to 101000 at 500 Hz and
102001 to 102500 at 60 Hz; the model has no arrival time, so its
mismatches are not counted):

| Lateness | 500 Hz guard stops (pinned, unverified) | 60 Hz guard stops (pinned, unverified) |
|---|---|---|
| 5 ms | 24 of 1,000 (6, 18) | 3 of 500 (2, 1) |
| 10 ms | 41 of 1,000 (14, 27) | 5 of 500 (3, 2) |
| 20 ms | 58 of 1,000 (27, 31) | 21 of 500 (8, 13) |

`past_far` and `past_full` were 0 in every run. The same 20 ms run with a
window of 8 frames (16 ms) and of 34 frames (68 ms; a scratch build with
sig.h's limit of 8 raised) stopped on the same 58 cases with the same
counts. So the window's length does not decide these stops. A late
report comes after the frame calls whose onsets are later than its time.
A frame's footprint is every node, so a past arm at the hit's time (the
report's) goes after the report, and after those frames: it is pinned
(sig.md, "Pinned"). The `unverified` stops are "Exactness" condition 3: a
late report after a waiting group. Any lateness that spans a frame call
does this, at any rate; at 500 Hz any lateness over 2 ms spans one. A
longer window helps only if sig.h places a late report at its time, as it
places a past arm. That change is not made (it changes sig.h's reference
order).

The window's logs with two raw mice at 8 kHz (60 Hz; `t_v2_window_8k`,
the hold's end at 0.199 s just after the 1,024-call log ran full in that
frame's reports): 0.10.0 stops on the guard (1 `logs full`); 0.11.0 runs
the 20 trials with 19 arms run again and no fallback. Window use, measured
in a scratch sig.h: 1,081 calls and 5,364 ring writes at most (that case),
1,080 and 10,776 (the bimanual example's samples split to 8 kHz), 546 and
7,572 (visuomotor's).

Arena of a template's graph (0.10.0 at 60 and 500 Hz, then 0.11.0):

| Graph | 0.10.0 (1,024 calls, 16,384 writes) | 0.11.0 at 60 Hz | 0.11.0 at 500 Hz |
|---|---|---|---|
| Visuomotor `reach`, 55 nodes | 11,049,136 bytes | 13,090,624 (1,414 calls, 39,538 writes) | 11,049,136 (the floor) |
| Bimanual `reach`, 49 nodes | 10,799,344 | 12,535,408 (1,414, 35,296) | 10,799,344 |
| Bimanual `track`, 38 nodes | 8,926,576 | 10,034,512 (1,414, 26,572) | 8,926,576 |

Frame work (`--measure`, the default build, `taskset -c 2`, six runs each,
interleaved; 0.10.0 at ca1ec57; load average 0.52 at the start and 0.71 at
the end):

| Example | Largest frame | p99 | p99.9 | Frames over the period |
|---|---|---|---|---|
| Visuomotor, 60 Hz | 640 to 764 us | 17.7 to 18.2 us | 620 to 626 us | 0 |
| Visuomotor, 500 Hz | 632 to 835 us | 4.2 us | 100 to 200 us | 0 |
| Bimanual, 60 Hz | 1,916 to 1,972 us | 29.0 to 30.0 us | 1,035 to 1,043 us | 0 |
| Bimanual, 500 Hz | 1,920 to 2,769 us | 9.5 to 9.6 us | 11.3 to 12.4 us | 1 in 3 of 6 runs |

The same after the changes (0.11.0; load average 0.19 at the start and
0.55 at the end):

| Example | Largest frame | p99 | p99.9 | Frames over the period |
|---|---|---|---|---|
| Visuomotor, 60 Hz | 644 to 658 us | 17.7 to 17.9 us | 619 to 624 us | 0 |
| Visuomotor, 500 Hz | 632 to 875 us | 4.2 to 4.4 us | 55 to 253 us | 0 |
| Bimanual, 60 Hz | 1,893 to 1,915 us | 29.3 to 29.9 us | 1,021 to 1,035 us | 0 |
| Bimanual, 500 Hz | 1,901 to 1,916 us | 9.5 to 9.7 us | 11.5 to 25.9 us | 0 |

The largest frame at both rates is a trial's end. Tracy (v0.14.1,
`tracy-capture` and `tracy-csvexport -u`; a scratch build with zones on 16
of sig.h's functions, about 1.5 us a frame; player on CPUs 2 and 4),
bimanual at 500 Hz, self time:

| Frames | `pl.write_inplace` | `pl.v2_trial_rows` | sig.h's graph work (feed, frame, settle) | Checkpoints | Exactness and waiting arms | Call log |
|---|---|---|---|---|---|---|
| The 5 largest (1,953 to 2,070 us) | 1,928 us (96 %) | 67 us | 0.85 us | 0.88 us | under 0.1 us | under 0.1 us |
| p99 to p99.9 (15.1 us a frame) | 6.18 us | | 4.51 us | 0.43 us | 0.45 us | 0.17 us |
| Every frame (8.3 us) | 3.15 us (38 %) | | 2.51 us (30 %) | 0.35 us (4.2 %) | 0.25 us (3.0 %) | 0.06 us (0.8 %) |

The largest frame writes the trial's 1,878 sample-trace rows in place,
about 1.03 us a row: `--sim` has no writer thread. A real run copies each
row into the writer's queue (`pl_wq_push_row()`): 1,878 rows of 296 bytes
in 20.4 to 20.6 us (best of 200, three runs, CPU 2), 10.9 ns a row, 102.5
us for 8,000 rows; the worst of each 200 was 142 to 178 us. So the
real trial end is the row loop (`pl.v2_trial_rows`, 67 us with the zones)
and the copy, about 0.1 ms, which fits a 2 ms frame. sig.h's catch-up
bookkeeping is about 0.66 us of a typical frame's 8.3 us; the ring writes'
undo notes are in the feed and not split out.

Cuts, not made (the `--sim` frame only): write the rows through the queue
on `--sim` too, and drain it after the frame (about 0.1 ms in the frame); or
write a trial's rows over its next frames, at 256 rows a frame (about 0.26
ms a frame at 1 us a row).

Checks of 0.11.0 (the same machine):

| Check | Result |
|---|---|
| ctest | Release 124 of 124 (`test_sig_arm_ref`, the C and C++ compile checks among them); the sanitizers tree (ASan and UBSan, built and run under `setarch -R`) 123 of 123 |
| `player_test.py` | 58 cases, 0 failures at 60 Hz and at `--hz 500` (`t_v2_window_8k` and `vm_guard_pinned` new) |
| `compare.py`, the six examples | No exact column differs at 60 Hz or at `--hz 500`. At 500 Hz the bimanual `frames.csv` has 276,000 rows (33,120 at 60 Hz); `goal_x` 10 and `goal_y` 12 of them round the other way at the C file's 4th decimal, each a value at a half (the program's float, the player's double, F25): the format class. `unverified` 0 |
| 60 Hz identity against 0.10.0 (ca1ec57) | Every data CSV file of `player_test.py` (1,455) and `compare.py` (70) byte-identical; `session.json` differs in `player_version` and `rig_profile_sha256` only. The one other difference is `vm_dense.csv`, the case's own input |
| Mutants (`--sim` test, 4 jobs) | `player_v2.toml` 93 of 93 as expected (v2-95 killed; v2-96 kept, a guard), `player_run.toml` 64 of 64 of the `--sim` ones, `player_lower.toml` 26 of 26; the real-path ones (19 and 15) need a display and did not run |


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
takes the response (2), the model the region (0). The check round below
fixes both by rule 4's list order.

### The check round: ties of a response's end and a hit (0.10.0, 2026-11-14)

The times in both cases are equal to the nanosecond: the press is the
trial's first onset plus a whole number of frames (`rt_s` 0.400000008
and 0.500000010), and so are the sample and, in 89320, the state's entry.
So D20 rule 4 decides: the earliest time, then the list order.

- 89320 was the player's. Its response's end fed the sample at its time,
  which entered the region; s4's `after_s` exit, 1 ns earlier, came first,
  and s4's next visit armed the region at that exit's time. sig.h runs the
  arm again over the sample, so the hit, at the response's time, came
  after the batch in which the player took the response. The player now
  drains the hits that an exit of the batch queued before it takes the
  response. Writing the case found a second path: a state entered on the
  way to the response's time (a timed exit at that time) drained its
  entry hits with no response offered, so a region won over an own
  response exit listed first. The response now stays offered until it is
  taken (`v2_resp_states()`).
- 84233 was the model's. It resolved the sample's hit before it fed the
  press at the same time. It now feeds every input at one time (not two
  samples) and resolves their exits together (D20 rule 4, cited in
  `v2model.py`).

`player_test.py`'s `t_v2_ties` has three list orders at one exit's time
and 89320's path; the player built at 1f61b9f fails two of the four (the own
response first, and 89320's path). Mutants v2-93 and v2-94.

The same machine, sig.h 0.6.0 after its check round:

| Check | Result |
|---|---|
| ctest | Release 124 of 124; the sanitizers tree (`setarch -R`) 123 of 123 |
| `player_test.py` | 57 cases, 0 failures; `sig_arms.unverified` 0 in every session |
| The model's harness, seeds 84233 and 89320 | 0 mismatches |
| New seeds 92001 to 94000 (2,000 cases) | 284 refused (agreed), 83 hangs agreed, 9 mismatches, all at a GAP; no guard stop; `unverified` 0 in all 1,382 sessions (the shrinker's included) |
| `GEN_MULTI=1`, 39001 to 40000 (1,000 cases) | 139 refused (agreed), 48 hangs agreed, 7 mismatches, all at a GAP; `unverified` 0 in all 967 sessions |
| The guard's rate on the harness (the coordinator's sample at db38205, seeds 96001 to 99000, 3,000 single-press cases) | 1 case of 3,000 stops on the guard: seed 96549, one `arm_unverified`, the same with 1f61b9f's sig.h (so not from this round); the other 25 mismatches are all at a GAP |
| `compare.py`, the six examples | No exact column differs; `unverified` 0 (visuomotor and bimanual: 176 arms run again) |
| Mutants (`--sim` test) | `player_v2.toml` 90 of 91 as expected (796 s, 4 jobs); v2-93 and v2-94 killed. v2-74 (no settle before a response's end) survived: `v2_resp_states()` now runs `v2_timed_to()` to the response's time with the response offered, which settles at that time and drains the hits too, so the mutant is equivalent. It is kept as a guard, expected to survive, with a note: 91 of 91. The other player lists were not run (their files did not change) |
| Frame work, CPU 2, six runs each, load average 1.82 at the start (falling from the mutant run; nothing else ran) and 1.54 at the end | Visuomotor 0.239 s (sd 0.003) against 1f61b9f's 0.235 s (sd 0.003) and 0.9.0's 0.211 s; bimanual 1.308 s (sd 0.006) against 1.291 s (sd 0.007) and 1.133 s: 1 % to 2 % above 1f61b9f, 13 % and 15 % above 0.9.0. Largest frame 639 to 693 us (1f61b9f 627 to 749 us), and 1,884 to 1,895 us (1,858 to 1,883 us); no frame over 16 ms |

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

## Explanation: signal roles on a rig (0.13.0)

G18's player part (devices_spec.md 11.2, "The signal block", build order
step 2). Decided by the user on 2026-11-16: a mouse is bound by a click,
an adapter is a named preset, display units convert through the
profile's `view`, and a removed mouse is replaced by a click.

| Question | Decision | Why |
|---|---|---|
| Where the checks run | The host's `sigrole()` (experiment.h), at load: a signal for each required role, the type by `ysig_open()`'s rule, the units by `yrig_units_factor()`; a real run with signal roles and no rig profile stops there too | Before the window or a device opens, with the role's name, as the device roles' checks |
| The lowering | The `--sim` adapter's source and integrator, at `per_count` times the units' factor, with the bounds times the factor. A lane with a negative scale runs mirrored (gain `-q`, bounds `[-hi, -lo]`) and a scale node of -1 turns it back | sig.h bounds a lane only with a positive gain ("flip with affine") |
| The input | Once a frame, at the frame's start, the player drains the screen's queue (`pl_v2_poll_screen()`). A role's raw mouse samples go to its graphs, their device made the role's (as `--sim`'s); every other input waits for the collectors, which get it where `yrsp_feed_screen()` fed them | The trial's reset at this onset must come after the samples polled before it. A role's samples go to no collector: at 1 kHz a mouse they filled the collectors' buffer in long frames (226 inputs lost under 40 busy loops on 20 cores) |
| `P` | When the poll started (`yrt_now_ns()`), for the horizon and for `ysig_frame_in.poll_ns` | sig.h's contract: input is known up to the poll. A real run polls before the onset; with the onset as `P`, every report between the two was late input |
| A click | Binds the first role that waits (definition order), refused for device 0 or a bound mouse, at any frame | A trial that waits on a role whose mouse was removed ends only on that role's input: the first build bound only between trials and hung |
| `device` `any` | The role is complete to `P - B`; no newest stamp raises it (0.13.2) | A mouse not heard from yet can send a report stamped before another mouse's newest, within its bound. 0.13.0 took the least of each seen mouse's `max(S(d) - 1, P - B)` and counted a mouse not seen yet as still: a timed exit after such a report went first, silently (the review of 0.13.1) |
| `device` `any` with `bounds` | Refused at load (0.13.2) | Every mouse feeds one integrator. A report late within its bound reaches it after another mouse's later counts; a sum does not depend on that order, a bound's clip does (the review of 0.13.1: `end_y` 2 for 1.21875) |
| The trial's flush (a real run) | Waits for the input to the barrier as it was at the trial's end, not to the graph's newest input | The graph still takes every sample, so its newest input moves on. A report after the trial's end that comes after the flush goes into its integrator (sig.h's H1): exact, as an integrator with no bounds sums in any order and a bounded one has one mouse `--sim` keeps the newest, which a pen's held value needs (seed 102928) |
| A graph with no trial (any bound above 0) | Reset (`ysig_reset_trial()`) at the horizon over every role after each poll, unless its trial's flush waits or its frame trace rows wait for their records | It takes every sample (its adapters) but no frame, so it takes no checkpoint: its logs filled, a late report fell back, and the guard stopped bimanual (v0.12.0 too, in `--sim` with 2 and 3 ms late hands at 500 reports a second). A trial reset clears an experiment node's ring, which a waiting frame row reads |
| `late_frames` in a real run | Counts nearly every frame with motion (bimanual 901 of 971, visuomotor 847 of 956) | A frame shows the input to its poll; the reports between the poll and the onset arrive at the next poll and change what it would have shown. In a real run it measures motion, not lateness |

A raw mouse's lateness: the player's `late_seen_s` per role (`session.json`)
is the largest `P' - t` of a sample, where `P'` is the poll before the one
that read it. Linux desktop (i5-14500, 20 threads, GCC 12.2, 60 Hz), two
uinput mice at 1 kHz each for 60 s, 2026-11-16:

| Load | `late_seen_s` | Reports past the 4 ms bound |
|---|---|---|
| none | 0 (58,000 samples a mouse) | 0 |
| a busy loop on each of the 20 cores | 1.6 ms | 0 |
| 40 busy loops | 15 to 36 ms | 59 to 395 a mouse; 1 of 3 runs stopped on the guard (reports before sig.h's window) |

So 4 ms: the loaded case with a margin. The raw mouse reader runs
SCHED_OTHER; a rig that oversubscribes its cores gets stops, not wrong data.

Frame work (`--measure`, the same machine, two mice at 1 kHz each, 30 s,
idle): p50 183 us, p99 215 us, max 270 us; no frame over the period.

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
- A removed mouse is seen only through SDL's `SDL_EVENT_MOUSE_REMOVED`,
  whose id is the raw reader's on Linux X11 only for a mouse X lists. On
  macOS (IORegistry ids) and for a node X does not list, the removal is
  not seen: no click is asked for, and a trial that waits on the role
  waits until Shift+Esc. The fix is to read the screen's `YSCR_EV_DEVICE`
  records, which use the reader's ids (a screen ring the player drains).
- Version 2 signal roles from other devices: a pen (ysp/screen.h delivers
  no pen events), an absolute pointer, a stream sample, an eye tracker;
  a stable mouse key for unattended sessions; the calibration procedure
  of signal_spec 5.5; a photodiode run for the cursor's latency.
- The role samples as input rows of `events.csv` (F30).

## Not verified

- A display faster than 60 Hz: the 500 Hz numbers are `--sim`'s. No 500 Hz
  display, photodiode run, GLX_OML swap events at 2 ms or flip-record lag in
  frames was measured. `PL_RECORD_LAG_NS` keeps 60 Hz's 2 frames as a time.
- Signal roles on real hardware mice: the tests use uinput mice
  (tests/player/real_test.py, `signals` and `signals_examples`), on Linux
  X11 only. Three short runs with hand-moved mice (Results, 0.13.1) did
  not reach a bimanual trial's end, the reach blocks or visuomotor's
  rotation. No Windows or macOS run was made.
- `pl_v2_poll_screen()` under player_alloc_test: the alloc case runs the
  four examples without signal roles. A click prints to the console
  (stdio).
- The sound lead on a real device at 500 Hz: `--sim` has the null device,
  and no test sees when a sound is scheduled.

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
- Version 2 in a real run: the trace rows' path through the writer's side
  buffer (`pl_wq_push_row()`) runs only in a real run; `signals_examples`
  reaches it with bimanual's and visuomotor's traces, but no check reads
  those rows. `--sim` writes the rows in place.
- A real run's trial-end cost with traces: the copy to the writer is not
  measured (Results, "Format version 2").
- Format version 2 on Windows, macOS and emcc.
