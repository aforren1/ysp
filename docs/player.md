# The player

Status: v0.2.0, 2026-11-10. It loads a pack, checks its experiment
definition, lowers the definition onto the ysp headers and runs it: in a
real window with the participant's keyboard and the rig's devices, or on
the simulated display (`--sim`). It is a program (`player/`, target
`ysp_player`, option `YSP_BUILD_PLAYER`), not a single header. It reads
format version 1 ([experiment_format.md](experiment_format.md), D1 to D15).
Real runs are measured on the Linux X11 desktop only (Results). The runner
protocol, `ysp/app.h`'s loop (rig_spec 4.2.1) and format version 2 are not
built.

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
   in `--sim` too.
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

## How-to: choose the participant

`--participant SPEC` gives the scripted participant of `--sim`. It is
outside the definition, so the definition does not change for a real run.
A real run refuses `--participant` and `--rt` (exit code 2).

| SPEC | Effect |
|---|---|
| `correct` (default) | Presses `key_answer` on every trial, `--rt` seconds (default 0.3) after the `from` stimulus's flip |
| `silent` | Never presses |
| `p=0.8` | Presses `key_answer` with that probability, else the next choice (drawn from a stream of the session seed) |
| `script:FILE` | A CSV of `trial_index,response,rt_s,hold_s`, one row per session trial (a response name or an empty field); a trial with no row is silent |

## Reference: data

The data set is format 2.10. Columns not fixed by the format:
`first_in_block`, `index` (the leaf's trial number; `trial_index` is the
session's, since it joins `frames.csv` and `events.csv`), and `time_elapsed_s`
(the end frame's onset minus the session's first frame's onset).

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
| A response's `trigger` on a device role | A `ydev_rule` on the role's reader thread, its output the trigger role's device. Its codes are fixed when the device opens: the code must be a number or a lookup by `$response` of numbers, and every template that responds on the role with a trigger must give the same choices and codes, in any order; else the start stops, naming both. A template on the role with no `trigger` runs with the rule disarmed, so its presses send nothing; the rule is disarmed at each trial's end. A rule's record goes to the trial whose arm its press follows. Measured on a pty pair: write start minus the press, p50 66.5 us, max 93.9 us (Results) |
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
C program gives a comparison for them. In `--sim` a response's `trigger` goes
out from the frame loop, as for a keyboard role; a real run's device role
has the rule.

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
(`PL_RSP_AFTER_LIVE`). `--sim`'s files did not change.

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
