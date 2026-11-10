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
3. It opens each bound device and waits up to 3 s for it to run. A device
   that does not answer as its family must stops the start.
4. It opens the display that `YSCR_BACKEND_AUTO` picks (GLX_OML on X11),
   full screen, with the panic watchdog armed, and compiles the programs
   into the user's program cache (ysp/gfx.h, PROGRAM CACHE).
5. It shows 30 blank frames (about 0.5 s). If the definition sets
   `display.min_tier` and these frames are of a worse tier, the start stops
   here, before the data set folder exists.
6. It prints `ysp_player: running; Shift+Esc stops the session` and runs
   the session.

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
| `<role>_lag_max_s` | The largest write start minus deadline of the trial's codes | 0 |
| `<role>_n` | The codes asked for, and the rule's codes from their records | The codes asked for |
| `session.json` `devices` | `bound` (a family, `keyboard` or `none`), `key`, `port`, `ident`, an input's `tier`, and `rule` (the output a device role's rule writes to) | `bound` only (`sim` or the rig's family) |
| `session.json` | `gfx` (the program cache's hits), `view_distance_mm` and `view_width_mm` (deg units), `start_utc` (the time), `aborted` after Shift+Esc | `start_utc` fixed |
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
| A role's `min_tier` | Not checked. A keyboard's events are tier 0 (UNKNOWN, the kernel's time) or 3 (the X server's), so the oddball's `resp` (`min_tier` 2) on a keyboard would refuse every start. `session.json` gives each input role's tier. A finding: see "What a real-window run needs next" |
| The response kind in a real run | The binding's: a device role's choices are its `buttons` (box events, control = the button's code); a keyboard role's are key names. `--sim` keeps the first family's kind |
| An any-key response in a real run | Hears its role's kind only (`yrsp_desc.kinds`): a click that brings the window to the front is no response |
| A response's `trigger` on a device role | A `ydev_rule` on the role's reader thread, its output the trigger role's device. Its codes are fixed when the device opens: the code must be a number or a lookup by `$response` of numbers, and every template that responds on the role must give the same codes; else the start stops, naming both. Measured on a pty pair: write start minus the press, p50 66.5 us, max 93.9 us (Results) |
| A response's `trigger` on a keyboard role | `ydev_out_mark()` from the frame loop when it sees the response, up to a frame after the press (2.6) |
| The collectors | One array, one collector per template with a response; `yrsp_feed_screen()` gives every one every input, between trials too, so none misses a release |
| Where a real run's data is written | A writer thread (`yrt_worker`, SCHED_OTHER) takes each frame's records, events, stream steps and rows from a queue of 8192 writes. On ext4 a row's `write()` from the frame thread blocked one frame for 17.8 ms while the kernel wrote the file back (Results); none on tmpfs. `--sim` writes in place, so its files are the same bytes as before. A row reaches the OS within a frame of its last flip record |
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

## Explanation: a window with no deadline can end with no response

This is a format question, open. The player does not decide it.

The loader (ysp/experiment.h 0.1.2) takes a response with `duration_s` 0
as one that always ends with a press: `$rt_s` reads in a number key, a
lookup by `$response` needs no `""` key, and a lookup by `$outcome` needs
no `"timeout"` key.

On a real display that is not true. ysp/response.h ends a window at the
first press that is valid against the onset it has when the press comes.
Until the stimulus's flip record arrives (one or two frames after the
landing), that onset is the plan. When the record's onset is later than
the plan, a press between `plan + min_rt` and `record + min_rt` is an
anticipation (EARLY when `min_rt` is 0). The window stays ended, and the
result has `YRSP_R_ENDED_EARLY` and no `YRSP_R_RESPONDED`. With no
deadline, nothing else ends the window, so the trial ends with no valid
press.

The evidence. A run of ysp/response.h 0.8.1 with the frame loop's order
(the plan, the press, then the record; duration 0):

| min_rt | Record minus plan | Press minus plan | Result |
|---|---|---|---|
| 0 | 16.7 ms (one frame late) | 5 ms | ENDED_EARLY, not responded, `rt_ns` none |
| 0 | 40 us | 20 us | ENDED_EARLY, not responded |
| 100 ms | 33.3 ms (two frames late) | 110 ms | ENDED_EARLY, not responded |
| 0 | 0 | 5 ms | responded, RT 5 ms |

When it happens in the player: the frame loop feeds a press before it
applies the frame's flip record (ysp/response.h, "The order in a frame").
On GLX_OML the landing frame's record comes at the next frame or the one
after. So a press stamped after the plan and before the actual onset, and
fed before the record, ends the window and is then reclassified. With
`min_rt` 0 (the default) and an on-time frame, that gap is the onset's
residual (under 20 us here); with a late landing frame, a whole frame.
With `min_rt` of two frames or more, the record is in before a valid press
can be fed. No real run here showed it: no landing frame was late in
16,000 frames (Results).

What the player does now: the row has an empty `response`, empty `rt_s`,
`rsp_flags` with 0x4 (ENDED_EARLY), and `outcome` `timeout`. A definition
that reads `$rt_s` in a number key, or looks up `$response` with no `""`
entry or `$outcome` with no `"timeout"` entry, stops the session with an
evaluation error at that trial; the rows before it are on disk. Also, the
`after` table was built from the live result (a press), so it showed what
a press shows, and the row says there was none. That second part applies
to any response, with a deadline too.

The options, with their effect:

| Option | `$rt_s` in a number key | Lookups by `$response`, `$outcome` | Existing definitions | Cost |
|---|---|---|---|---|
| A. The loader takes every response as one that can time out (drop 0.1.2's exception for `duration_s` 0) | Refused at load, as with a deadline | Need `""` and `"timeout"` entries | Valid 0.1.2 definitions with `duration_s` 0 that use the exception are refused; the worked examples all have deadlines | A format change (the loader's levels); the trial with no valid press is a timeout, as the row already says |
| B. The player re-opens the window after ENDED_EARLY when there is no deadline, and builds `after` only from a final onset | Always a press | No change | None | `after` starts up to two frames later after an early press; ysp/response.h has no call that re-opens a window and keeps the reclassified press's counts (`yrsp_arm()` clears them): a header change, or the counts are lost |
| C. With no deadline, the press that ended the window is `$response` and `$rt_s` (an RT below `min_rt`, or negative), and the row keeps ysp/response.h's result | Always a number | No change | None | `$correct` and the method hear an anticipation as a response; the variables and the row disagree on such a trial |
| D. Keep the loader, and stop the session at such a trial with a named error | Unchanged | Unchanged | None | A rare event (an anticipation within a late frame) ends a session |

`requeue: no_response` does not help on its own: the row's values are
evaluated before the requeue, so the lookups fail first.


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
| Allocations in `pl_frame` (`player_alloc_test`) | 0 on all four examples; 0 on the frame thread in a real run (`--real`, below) |
| The comparison (`tests/player/compare.py`) | No exact column differs; timeouts now in the RDK and 2AFC scripts |
| Full ctest, Linux, `DISPLAY` unset | 120 of 120; the player and experiment tests of the sanitizer build, 9 of 9. 2026-11-10 (this build's set, the real window's code included): 119 of 119 |
| `--sim` byte identity, 2026-11-10 (v0.2.0 against main's v0.1.0): the 42 cases of `player_test.py`, `compare.py`, and the four examples with three participants each | 459 files: 369 identical; 90 `session.json` files differ in `player_version` only (and in `rig_profile_sha256` where the test made its rig profile again, whose `written` date differs). The same on the ASan and UBSan build |

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

- A decision on the window with no deadline (the format question above).
- A decision on a role's `min_tier` (2.9): a keyboard is tier 0 (UNKNOWN)
  or 3, so enforcing it refuses every keyboard role that sets one, the
  oddball's `resp` included.
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
- A panic keeps the rows already handed to the writer; a trial that ended
  in the frame before a hang can lose its row (its last flip record never
  comes). Measured only with the test hang (`--hang-at-trial`).
- `aep`, `artwork`, `video`: refused.
