# The player

Status: v0.1.0, 2026-11-09. The first player. It loads a pack, checks its
experiment definition, lowers the definition onto the ysp headers and
runs it on the simulated display. It is a program (`player/`, target
`ysp_player`, option `YSP_BUILD_PLAYER`), not a single header. It reads
format version 1 ([experiment_format.md](experiment_format.md), D1 to D15).
A real window, a real device, the runner protocol and format version 2 are
not built.

## How-to: run a pack

Build the pack with `ypak build`. Then run it from a folder that is not the
source tree (the player writes a new data set folder):

```sh
ysp_player PACK.ysppak --out DIR --sim --seed 7 --field participant=1
```

`--sim` is required in this version. It selects the simulated display, the
virtual clock, the null audio device and a scripted participant. The same
inputs give byte-identical data. `DIR` must not exist.

A definition in `deg` units or with `"calibration": "required"` needs a
rig profile with a `view` and a calibration: `--rig FILE` (and `--cal FILE`,
or a calibration named in the profile). Test profiles come from
`player_make_rig DIR`.

## How-to: choose the participant

`--participant SPEC` gives the scripted participant. It is outside the
definition, so the definition does not change for a real run.

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
seed), `devices` (each role and what it is bound to; `sim` in `--sim`), the
audio describe line and every header's version.

Not done in the data set, and why: `clock.csv` is empty (`--sim` has no device
clock fits to log); `events.csv`'s `out` rows take the deadline as the write's
time (a real write's start needs a real output); device rows have no ports
or serial numbers (there is no device in `--sim`).
Under `--sim`, `start_utc` is fixed and `frames.csv`'s phase columns are
empty (they are real CPU costs, and would differ between two runs).

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
| Leaf runs in one session | The loader bounds them from the tree (repetitions, table rows, `loop.max`) and sizes `node_seeds` from that bound. A tree that can run more than 10,000,000 leaves is refused at load |
| An RDK's steps file | The step buffer is sized at load from the longest trial each template that shows the RDK can run (its events, the response's deadline, `after`). A template with an RDK and no reachable end (a response with no deadline) or one longer than 7,200 s is refused at load, so no trial stops with 0 rows |
| Column text | Each text column holds the longest value known at load (at least 64 bytes); a value longer than 4,096 bytes is refused at load |
| An array in `vars` or `data` | Written to its column as one JSON array, numbers in canonical form: `[1,0,0.5]`. A stimulus key reads it as its value |
| A key held past its trial's end | The release goes to the collector that saw the press, in any later trial or template, so the next press counts |
| A frame with no trial (before the first trial, between trials, a yield frame) | Draws nothing |
| A stimulus left on at a trial's end | `<s>_offset_s` is the onset of the trial's last frame |
| One node with the same name in `vars` and `data` | Refused at load |
| A table column in one leaf and a `vars` name in another | One column, as for a jitter in one leaf and a text in another: each leaf's row writes its own value, and a column whose leaves give different types holds text (a duration as its exact decimal) |
| A channel's trace | `--trace-channels FILE` writes every channel per frame: a test's view, off by default |
| A trial row's timing columns | Written when their frames' flip records arrive (a pending-row ring), so `<s>_offset_s` of a stimulus turned off at the trial's last frame is known |
| `--sim` triggers | The screen's trigger channels are real (`yscr_trigger`, worker thread); the sink counts firings, and the data takes the deadline (the planned vblank) as the write's time, because a real write's start would differ between two runs |
| A required role unbound in `--sim` | The participant and the sink stand in for the device (the check runs against a real run's rig) |
| `display.min_tier` on the simulated display | Flags frames (`BELOW_TIER`); the display is not refused |

The variable `$trial_index` is the leaf's trial number (2.3 reads it from
`ytr_trial_info`); the column `trial_index` is the session's.

These are not lowered, and the player refuses each by name: `aep` methods,
`artwork` stimuli and `video` stimuli. No worked example needs them, and no
C program gives a comparison for them. In `--sim` a response's `trigger` goes
out from the frame loop, as for a keyboard role. Its device rule (`ydev_rule`)
needs a real role.

## Results

2026-11-09, the Linux desktop (Debian 12, gcc 12.2, 20 threads), `--sim`
on the virtual clock, unless the row says otherwise.

| Check | Result |
|---|---|
| The four worked examples against their C programs (`tests/player/compare.py`) | RDK, 2AFC: every shared column exact, the dots' digest and the steps file included (the program's trial seed from `--trial-seeds`, the 2AFC's timing from `--design-timing`, both test-only). Stroop: exact except columns the format derives differently (`block` text, `too_slow`). Oddball (`trigger_flip.c`): not comparable column for column; the shared mechanism checked instead. The report lists each column |
| `tests/player/player_test.py` | 40 cases of unusual shapes and of each fix of both reviews (round 2: `to` and `then` and the trial's end, arrays in `vars` and `data`, a key held into another template's trial, yield frames, the load-time bounds, a stimulus left on, the definitions experiment.h 0.1.2 accepts; a lookup in every key type, nesting, `if` on a node and on a step, `loop`, `requeue`, a track that ends early, density, key re-application, stale RDK data, timeouts, any-key responses, a stimulus shown twice, `vars` and `data` of one name, repeated sounds, a backward `at_s`, the walk's budget, the data set): pass |
| `tests/player/eval_test.c` | 52 checks: pass |
| ASan and UBSan (`setarch -R`): the four examples, the unit test, the 40 cases, the comparison, and two review scripts of 31 definitions (the 20-minute RDK trial and 70,000 leaf runs among them) run to their ends | Clean. Each heavy job ran in a 6 GB memory scope; the system tests now stream their data files (peak 18 MB) |
| Fuzz, ASan and UBSan: `tests/fuzz/player_fuzz.c` over the corpus of `tests/fuzz/player_corpus.py` (1,015 seeds, every key of ysp/experiment.h's table in a valid definition, arrays in `vars` and `data`, the values 0.1.2 accepts) and the four examples; token swap, byte flip, key insertion, key deletion; a quarter of the inputs with no trial cap; 8 processes | 200,000 inputs on the final code, 27,131 loaded, 26,338 ran to the end, no report |
| gcov, `-O0`, the seeds and 30,000 mutated inputs | `lower.c` 92.1 %, `run.c` 95.5 %, `host.c` 95.4 %, `eval.c` 85.6 % of lines. Not reached in `eval.c`: the AEP dims branch (AEP is refused), defensive cases of value kinds the loader does not produce (`YEXP_V_NONE`, a linspace or choice as a single value, unknown kinds) and errors for types the loader refuses (`no value where a number goes`, a non-contrast gamut, a gamut with no calibration) |
| Mutants (`tests/mutate/player_{lower,run,eval,host}.toml`, built with the system tests) | 76 of 76 as expected: 71 killed, 5 kept as survivors with a note (a full row ring, the weight of a leaf open in the per-frame yield, and three guards behind a load-time bound: the RDK buffer, the seed log, a value's text). `experiment.toml`: 169 of 169 |
| Allocations in `pl_frame` (`player_alloc_test`) | 0 on all four examples |
| The comparison (`tests/player/compare.py`) | No exact column differs; timeouts now in the RDK and 2AFC scripts |
| Full ctest, Linux, `DISPLAY` unset | 120 of 120; the player and experiment tests of the sanitizer build, 9 of 9 |

Frame work (`--measure`, Release, i5-14500, no other job of mine running,
desktop open, load average 1.4 to 2.9, three runs of each of the four
examples, after round 2): largest frame 29 to 67 us, largest trial start 17
to 58 us, 0 frames over 16 ms. Before round 2 (load average about 2): 20 to
50 us and 14 to 32 us. The spread between runs of one example (2AFC: 50 to
67 us) is as large as the change, so this is no measured regression. The simulated display has no
GPU cost, so this is the CPU part of a frame only.

## What a real-window run needs next

`--sim` covers neither a real screen, a real input source, nor a real
output. A real run needs: `yrsp_sdl_source()` and `yrsp_feed_screen()`;
`ygfx_open_screen()` with a real backend, the view from the actual mode and
a program cache; `display.min_tier` refusing a start; bound roles and
`yrig_open_device()`; a `ydev_rule` on a response trigger; a `ydev` output
role behind `ydev_trigger_channel()`; and the frame loop as `yscr_run`
when 4.2.1 exists.

## Not verified

- No run on a real display, an input device or an output device.
- No run on Windows or macOS.
- `aep`, `artwork`, `video`: refused.
