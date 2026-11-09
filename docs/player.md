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

`session.json` adds `node_seeds`: each leaf run's path, iteration and seed.
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
| `visible` as a key | Honored only when no step drives the stimulus's channel |
| `edge` on `rdk` | `yrdk_desc.edge`; gfx's dots keep their default edge |
| A leaf with tracks and a table, whose template reads a column on a track trial | The run stops at that trial with a named error: a track trial has no row. A finding: the format does not forbid it |
| `$outcome` with no `key_answer` | `responded`; `correct` empty; `ytr_update()` gets the choice index |
| A `0` to a log10 method | `method_value` is floored at `1e-4` before the log, in a log domain (`scale log10` or stair `step_type` log or db), as the C program does. The format has no key for this |
| `loop.until_accuracy` | Correct over all trials of the iteration (timeouts count against it), `0` when none ran |
| Trigger codes | `0..255`, the width of a port byte; another value stops the run |
| A trial row's timing columns | Written when their frames' flip records arrive (a pending-row ring), so `<s>_offset_s` of a stimulus turned off at the trial's last frame is known |
| `--sim` triggers | The screen's trigger channels are real (`yscr_trigger`, worker thread); the sink counts firings, and the data takes the deadline (the planned vblank) as the write's time, because a real write's start would differ between two runs |
| A required role unbound in `--sim` | The participant and the sink stand in for the device (the check runs against a real run's rig) |
| `display.min_tier` on the simulated display | Flags frames (`BELOW_TIER`); the display is not refused |

Not lowered: `aep` methods, `artwork` and `video` stimuli are refused by
name. No worked example needs them and no comparison with a C program
exists for them. A `yexp_response.trigger` is a frame-loop `yscr`-free code
in `--sim`; its device rule (`ydev_rule`) needs a real role.

## Results

2026-11-09, the Linux desktop (Debian 12, gcc 12.2, i7-1360P, 20 threads),
virtual clock. All in `tests/player/`.

| Check | Result |
|---|---|
| `compare.py`, the four worked examples against their C programs | Below |
| `player_test.py` | 14 cases, 0 failures |
| `eval_test.c` | 52 checks, 0 failures, ASan and UBSan clean (`setarch -R`) |
| The four examples under ASan and UBSan | Clean |
| Determinism | `trials.csv`, `events.csv`, `frames.csv` and `session.json` byte-identical across two runs (`t_seeds`, `compare.py`) |
| Frame work (`--measure`, the 2AFC session) | Max frame 44.5 us, max trial start 35.5 us, no allocation in the frame loop (see below) |

Frame work was measured while nothing else ran.

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
