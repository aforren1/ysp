# A reference model of version 2 trial timing

`v2model.py` computes, from a version 2 definition and a scripted
participant, the rows that `ysp_player --sim` must write. `gen.py` makes
random small definitions. `diff.py` runs the player on them and reports
each difference from the model. The model uses only
[experiment_format.md](../../docs/experiment_format.md) (part 2, part 3,
D16 to D41) and the readings F1 to F38 of
[experiment.md](../../docs/experiment.md). It does not use the player's
source.

## How to run it

1. Build the player and the pack tool (`YSP_BUILD_PLAYER`,
   `YSP_BUILD_PACK`) in a build folder, for example `build/linux`.
2. Run the harness from the repository root. Use a work folder outside
   the source tree:

   ```sh
   uv run --no-project python tests/model/diff.py --bin build/linux \
       --seeds 0:2000 --work /tmp/v2model --jobs 8 --log /tmp/v2model.jsonl
   ```

3. Read the output. The last line gives the count of cases, of refusals
   that the model and the loader agree on, and of mismatches. Each
   mismatch gives its seed, the GAPs that its trial reached, the first
   different row, and the folder of the smallest case that still fails
   (`min/`, with `exp.json`, `part.csv`, `samples.csv`, `expect.json` and
   the player's `out/`).

To see the expected rows of one definition:

```sh
uv run --no-project python tests/model/v2model.py exp.json \
    --participant part.csv --samples samples.csv
```

To see one generated case: `uv run --no-project python tests/model/gen.py SEED`.

Options:

- `--no-shrink` does not shrink failing cases.
- `GEN_AVOID=state_at,until_timeout,release` leaves those features out
  of the generated cases, so that a known difference does not hide others.

## What the model covers

- One leaf with `reps N`, one template, no variables. The simulated
  display: 60 Hz, frame k at 1000 s + (k + 7) x 16,666,667 ns (measured).
- States: the steps `on`, `off`, `wait_s`, `at_s`, `mark`, `flag`, `set`
  (a constant), `if` and `unless` on a flag, and `until` with and without
  `timeout_s`.
- Exits: `after_s`, `on` a region, `on: "response"`, instant exits with and
  without `if`, shared exits with `in`, `max_visits` and `over`, exits to
  a state or to `end`, and exit outcomes.
- The key response: `choices`, `from`, `duration_s`,
  `minimum_valid_rt_s`, `response_ends_trial`, `max_responses`,
  `key_answer`, `wait_for_key_release`, and the three rules of F34.
- Frames: the lead rule for timed exits and step ends, F40 (a timed exit
  is never taken on its state's entry frame), "the first frame at
  or after" for hits, the list order of D20 rule 4 at equal times, F38,
  and the wait for the landing frame's flip record (3.11 rule 11).
- An events template with `until`, `after` and the response (2.5, 2.6,
  F35).
- Outcomes (D36, F16, F34 rule 2).
- Regions: one absolute 2D role `hand` (pen positions, gain 1), a circle,
  `stay_s` literal, `rate` sample.
- The loader's refusals that the generator can make: D20 rule 6 with F5
  and F26, the rule for a state whose exits are all instant, F37, a
  response whose `from` no step shows, and `at_s` after an `until` in
  `events`.

The harness compares, per trial: the state rows of `events.csv` (text,
frame, `t_s`, `end_s`, code), the landing rows (`onset`, `offset`, `mark`,
`set`: kind, name, frame), and `time_elapsed_s`, `outcome`, `response`,
`rt_s` and the flags of `trials.csv`.

## Not covered

Variables and tables, requeue, `to` and `then` tweens, triggers, the
stagger, `recenter`, controls other than a region, `rate` frame, relative
roles, signal-choice responses, columns, traces, `allow_held_key`, more
than one press a trial, and real (not `--sim`) runs.

## Decided points

These were GAPs; the user or the coordinator decided them on 2026-11-12,
and the model follows the decision: F40 (a timed exit, `after_s` or an
until's `timeout_s`, lands on the later of its lead-rule frame and the
frame after the entry frame); an until's timeout code is the count of the
state's own, shared and added exits (F28); an until on a control that the
state's exits name has a detect of its own (F3); a hit that an arm makes is
taken before the frame's evaluate (F38).

## GAPs

A GAP is a point that the format does not decide. The model marks each in
`v2model.py` with `GAP:` and records which GAPs a trial reached. A
difference at a GAP is a question for the format, not a player bug.

| GAP | Reading in the model |
|---|---|
| The simulated display's onsets; when a flip record arrives | Measured: the grid above; frame L's record is in at frame L + 1 |
| `arm-order`: an arm made at a frame has a time that is not the input's place (a timed exit, a response that became final, a hold that a later sample found) | Not decided; the generator moves samples out of these windows |
| `future-arm-hit`: an arm after the frame's onset whose test is true at once | The hit is taken in that frame |
| `refound-in-frame`: a hold armed by a state that a frame's hit started, which ends before that frame's onset | The frame finds it, and its next state starts on the same frame |
| `response-before-entry`: a response that becomes final in a state entered after the response's time | The exit's time is the later of the response's time and the state's entry (a reading of 2026-11-12) |
| `at-after-until`: `at_s` after an `until` in a state | From the until's hit (rule 9: the steps after it are the next state), not the state's entry (rule 6) |
| `tie-steps-end`: a steps' end and an exit at one time | The last steps' end of a state with no authored exits first; else the exit first |
| `release-time`, `release-after-deadline`: a response that ends at its key's release | The release is the exit's time; the deadline waits for it |
| F40 for an until's timeout: "the state's entry frame" | The wait's entry frame (the wait is a state of its own, 3.11 rule 9) |
| The outcome column with no response in an events template | `done` |
| A sample and a press at one time | The sample first |
| The deadline of F34 rule 2 | A deadline with no response sets `timeout` |
