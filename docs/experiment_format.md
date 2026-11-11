# The experiment definition

Status: design decided 2026-10-14 (proposed 2026-10-13). Nothing is
built. This page designs the file that the player interprets (rig_spec
6): the EXPERIMENT entry of a pack. Each decision gives the options, the
evidence and the choice. The user decided D1 to D15 on 2026-10-14; D4
changed from the proposal. The user decided D16 to D19, for continuous
tasks, on 2026-11-09. Nothing is built for them. The user decided D20 on the same day. Part 1 is
explanation; part 2 is reference, with four worked examples checked
against the C examples. Part 3 (2026-11-11) is the proposed reference for
format version 2, with two worked examples that are the acceptance tests
of its loader and player; its open points are D21 to D41 (1.5), proposed,
not decided. Where this page and rig_spec 6 or 14.5 to 14.6
disagree about the definition, this page is newer.

## Part 1: Explanation

### 1.1 Decided, and cited here

| Decision | Source |
|---|---|
| Durations in definitions, scripts and data files are seconds. C APIs take `int64_t` ns. (2026-10-07) | rig_spec 14.6; conventions.md, Units |
| An experiment names roles. Only the rig profile names ports, paths and serial numbers, and the profile and the display calibration stay on the rig. | devices_spec 3 (principle 7), 11.1, 11.2; pack.md 10, question 1 |
| ysp/json.h is the strict reader and the canonical writer: unknown and duplicate keys refused, errors with a line and a column, keys in bytewise order, two spaces, LF. | json.md; devices_spec 11.2 |
| The definition is the pack's one EXPERIMENT entry. The manifest records its SHA-256, so the pack ID commits to it. Resources are in their canonical forms. | rig_spec 5; pack.md 2.8, 2.11, 3, 4.12 |
| The tree is fixed at load. No node is added or removed at run time. | rig_spec 14.6 |
| A data-only experiment has no script. Scripts (Luau, proposed) live in the player. | rig_spec 6 |

### 1.2 The shape

A definition is one JSON object with five named sets and one tree:

- `stimuli`: what the trial can draw or play, with values from the
  parameter tables (rig_spec 1, principle 8).
- `trials`: templates. A template has `events` (steps planned at the
  trial's first frame), a `response`, and `after` (steps planned when the
  response ends).
- `methods`: staircases, QUEST+ and AEP, by name.
- `devices`: roles (devices_spec 11.1).
- `timeline`: the tree. A leaf node runs one template over the rows of a
  pack table, the tracks of methods, or both.

Each part lowers to a C API that exists. The definition adds no timing
of its own:

```
node            --ytr_desc, ytr_rules()-->  ytr_next()                 ysp/trials.h
template steps  --variables filled-->      ytl_op[], ytl_run()         ysp/timeline.h
response        -->                        yrsp_desc, yrsp_set_landing ysp/response.h
end MARK fires  -->  yrsp_finish(), method update, ytr_update(), ytr_data_write()
```

The three C trials that this page checks (2AFC, RDK, Stroop) have the
same structure: a sequence from the trial's first frame, a response
window from one stimulus's flip, and a second sequence from the later
of the response's end and the first sequence's end. The format names
that structure; it does not invent a new one.

### 1.3 Decisions

**D1. Syntax. DECIDED (2026-10-14): JSON, with a `note` key for comments.**
- Options: JSON; TOML; YAML; JSON with comments (JSONC, JSON5).
- Evidence: ysp/json.h is decided as the reader and writer, and the
  manifest, the pack source description and the rig profile are JSON.
  devices_spec 11.2 refused YAML (implicit typing changes `no` and
  `1e3`; a large parser) and said the profile follows the definition if
  it goes to TOML. A second syntax would need a second strict parser,
  fuzz target and canonical writer. The designer writes most
  definitions; comments matter for hand edits only. ysp/json.h refuses
  comments, and a comment that the canonical form drops cannot be kept.
- Decision: JSON. Any object may have `"note": "text"`. A note is
  part of the hash, as any value is.

**D2. Canonical form, numbers and the hash. DECIDED (2026-10-14).**
- Options for numbers: (a) JSON numbers in a canonical decimal form;
  (b) reals as decimal strings, as in the manifest (`"1e-4"`).
- Evidence: the manifest uses strings because its tool refuses a
  fraction, and people rarely edit it. A definition has many reals
  (contrast, colors, durations); `"contrast": "0.5"` invites type
  errors in hand edits. ysp/json.h v0.1 kept a number's text and wrote it
  back unchanged, so `0.50` and `0.5` hashed differently (gap G7, closed
  in v0.2.0). The rig profile writes seconds as the exact decimal of the
  nanoseconds.
- Decision: (a). A duration is read with `yjs_fixed(v, 9, &ns)`
  and written as the exact decimal of its nanoseconds. Another real is
  written as the exact decimal of its source text, in ysp/json.h's
  canonical spelling (json.md, Canonical numbers): no trailing zeros,
  `-0` as `0`, no exponent from 10^-18 to below 10^21, `d.ddde+E` or
  `d.ddde-E` outside that range. A key at its default is left out. The
  defaults are the format's, listed in this page per version, not the
  headers': the loader fills every desc field, so a header that changes
  a default does not change an experiment. The experiment ID is the
  SHA-256 of the canonical bytes. The pack tool writes the canonical
  bytes into the pack and records `"op": "canonical"` in the entry's
  derivation, so the entry's SHA-256 in the manifest is the experiment
  ID.
- Prerequisite: gap G7, closed by ysp/json.h v0.2.0 (2026-10-21). Its
  writer writes every number as its exact value in one spelling, so
  `0.50`, `0.5` and `5e-1` give the same bytes, and every accessor reads
  the canonical bytes as it read the source. `yjs_new_double()` gives the
  shortest decimal that reads back to a double, the same bytes on every
  compiler. Open: two texts of one double that differ past its 17th
  digit (`0.1` and `0.1000000000000000055`) still hash differently. A
  loader that writes each real back through `yjs_double()` and
  `yjs_new_double()` makes them one; the experiment ID then depends on
  the double, not the text.
- Built (pack tool v0.7.0, 2026-10-22; pack.md 16): the tool stores a
  definition as ysp/json.h's canonical bytes with one LF after the value,
  as the manifest and the rig profile end, and records `"op":
  "canonical"`. The source's SHA-256 in the manifest is the canonical
  bytes' too, so two sources that differ only in whitespace, key order,
  number spelling or escapes give one entry, one experiment ID and, at
  one source path, one pack ID. The pack does not keep the source text:
  JSON has no comments (D1), so the canonical form drops no value.
  Built (pack tool v0.8.0, 2026-11-09): "a key at its default is left
  out", through ysp/experiment.h and its table of defaults.

**D3. Named sets are objects; order is in arrays. DECIDED (2026-10-14).**
- Evidence: the canonical form sorts keys, so order in an object is
  lost; arrays keep it. The parser refuses a duplicate key, which gives
  the duplicate-name check for free. devices_spec 11.1's YAML used a
  list of roles.
- Decision: `stimuli`, `trials`, `methods`, `devices`, `vars` and
  `columns` are objects keyed by name. Steps, children, choices and
  rules are arrays. Draw order is `layer` (an integer, default 0), then
  the name.

**D4. Units and names. DECIDED (2026-10-14).**
- Options (rig_spec 14.6 safeguards): a duration as a number or as text
  with a unit (`"250 ms"`); a `_s` suffix on duration keys, as in the
  rig profile; a warning above 60 s.
- Evidence: one spelling per value keeps the hash stable. A unit string
  accepted and rewritten by the loader makes the stored bytes differ
  from the source. Without a suffix, a time reads like a size or a count
  (`duration` beside `count`, `w`, `reps`). The rig profile already
  writes `pulse_s` and `onset_offset_s` (devices_spec 11.2). The
  bindings carry no unit in a name (conventions.md, Units), and
  jsPsych's names (`minimum_valid_rt`) carry none; the definition
  differs from both on purpose.
- Decision (the user changed the proposal, which had no suffix): a
  duration is a bare number in seconds, or a variable that holds
  seconds; any other string where a duration goes is refused with a
  message that says "seconds". Every key and every variable that holds
  a duration ends in `_s`: `wait_s`, `at_s`, `duration_s`, `delay_s`,
  `stagger_s`, `minimum_valid_rt_s`, `pulse_s`, a jitter named `soa_s`,
  the response variable `rt_s`. Other quantities keep plain names (`w`,
  `sf`, `count`, `speed`). A C field `x_ns` is the key `x_s`. The loader
  refuses a duration key without the suffix and names the suffixed key.
  Data columns follow the same rule (the user, 2026-10-20): a column
  that holds a time in seconds ends in `_s` (`YRSP_DATA_COLS` writes
  `rt_s`, `onset_residual_s`), because jsPsych writes `rt` in ms and
  PsychoPy in s, and a file read with the wrong habit is off by 1000
  with nothing to flag it. `ysp/trials.h` v0.5.0 refuses a time column
  or a jitter without the suffix.
  The designer shows the unit beside each field and warns above 60 s;
  the loader's lint warns too, and does not refuse. A jsPsych importer
  converts ms to s. Spatial units are `px` or `deg` for the whole
  display (`ygfx_desc.units`); the degrees come from the rig's viewing
  geometry, never from the pack, as the calibration does (the rig
  profile's `view`, ysp/rigfile.h v0.5.0; gap G2, closed).

**D5. Order and sampling: the rules text. DECIDED (2026-10-14).**
- Options: (a) jsPsych keys (`randomize_order`, `repetitions`,
  `sample`) that map to `ytr_desc`; (b) ysp/trials.h's rules text,
  one statement per array element; (c) both.
- Evidence: the rules text already sets every order, sampling,
  constraint, group, Latin-square, practice, warmup and jitter field of
  `ytr_desc`, by column and level names, with errors at a line and a
  column (trials.h GRAMMAR), and `ytr_format_rules()` writes a session's
  settings back in the same text. jsPsych's keys cover a subset and
  would need constraints added anyway. Two syntaxes for one desc give
  two places to be wrong.
- Decision: (b), `"rules": ["order constrained", "max_run
  congruent=1 3"]`. The designer and a jsPsych importer show and write
  rules. jsPsych's names stay for the tree (`timeline`,
  `timeline_variables`, `name`, `data`, `record_data`) and the response
  (`choices`, `key_answer`, `minimum_valid_rt_s` with D4's suffix,
  `response_ends_trial`).

**D6. Values: references and maps, no expression language. DECIDED (2026-10-14).**
- Options: (a) references (`"$side"`) and lookups (`by` and `map`), with
  a few fixed forms; (b) a small expression grammar of our own;
  (c) Luau expressions.
- Evidence: the four worked examples need only references, lookups,
  boolean variables (`$practice`, `$first_in_block`) and the response's
  correctness. A grammar of our own is a second language to specify,
  fuzz and teach. Luau is proposed for scripts, not decided.
- Decision: (a) in version 1. A computed value, a branch on an
  arithmetic test, or a custom flow is a script; when the script layer
  exists, a template takes `"script": "name"` and a value takes
  `{"lua": "..."}`, which version 1 refuses by name.

**D7. The trial template: events, response, after. DECIDED (2026-10-14).**
- Options: (a) the three-part template; (b) a general state machine of
  routines (PsychoPy Builder); (c) only scripts for anything with a
  response.
- Evidence: the 2AFC, RDK and Stroop programs each build one sequence at
  the trial's first frame, arm the response, and on its end build a
  second sequence at `max(response end, end of the first)` with
  `ytl_at()`, ending in a MARK. (b) needs transitions, conditions and a
  scheduler in the format. (c) contradicts "a data-only experiment has
  no script".
- Decision: (a). Anything else is a script (D6).

**D8. Random streams. DECIDED (2026-10-14).**
- Evidence: trials.md: "a draw from the session's generator between
  next() and update() would move every later draw and `ytr_restore()`
  would not repeat it". The RDK program seeds each field with
  `seed * 1000003 + index`, and the 2AFC program draws the side from a
  second generator of its own: two ad hoc schemes.
- Decision: one session seed `S` (the operator gives it, or the
  player draws it from the OS at start), logged. Each node's generator
  is seeded from a fixed hash of `S`, the node's path and its iteration,
  so skipping a node or looping it does not shift any other node's
  draws. `ytr_trials` draws from the node's generator only. A second
  stream per trial, from the node's seed and the trial index, serves
  `vars` draws and `$trial_seed` (a stimulus generator's seed). Version 1
  pins the hash.
- Built (2026-10-21): ysp/trials.h v0.6.0 has the second stream
  (`desc.seed`, `desc.vars[]`, `ytr_trial_seed()`; the header's VARIABLES
  section). Trial i's stream is splitmix64 from the state
  mix64((seed XOR 0x6A09E667F3BCC909) + (i + 1) x 0x9E3779B97F4A7C15);
  output 0 is `$trial_seed` and output 1 + j is variable j. A variable
  takes nothing from the node's generator, so adding one moves no other
  draw. The data line records each trial's seed in a `trial_seed` column
  (`x` and 16 hex digits, text in pandas, R and ysp/table.h; trials.md,
  v0.6), so a data file gives it without a re-run of the hash.

**D9. The data set. DECIDED (2026-10-14): a folder of CSV files and `session.json`.**
- Options: (a) a folder: `trials.csv`, `frames.csv`, `events.csv`,
  `streams/`, `clock.csv` and `session.json`; (b) the same in one zip,
  written at the end; (c) SQLite or HDF5.
- Evidence: the trials writer (trials.md, v0.4) writes each row to the
  OS when it happens, so a crash keeps every finished row; pandas, R and
  ysp/table.h read its CSV with their defaults. (b) loses a crashed
  session unless it also writes (a). (c) adds a dependency and a reader.
  devices_spec 11.4 lists the content: device rows, event rows, stream
  tables, clock pairs and fits.
- Decision: (a) on a rig; a zip of (a) on the web (2.13).
  `session.json` is canonical JSON written at start and again at close
  with `"closed": true`, so an unfinished session is visible, as the
  trials writer's sidecar does with `closed=1`.

**D10. What a trial row records. DECIDED (2026-10-14).**
- Evidence: the examples write the response's columns
  (`YRSP_DATA_COLS`: onset source, tier, residuals, response tier,
  flags), a count of dropped vblanks over the motion frames (RDK), the
  RDK seed, update count and digest, and the method's level, the
  contrast shown and the estimate (2AFC). jsPsych writes every timeline
  variable by default (`save_timeline_variables`). The writer takes up
  to 256 columns (ysp/trials.h v0.6.0; was 64, gap G10).
- Decision: 2.10 lists the columns. Per-stimulus landing columns
  are written for the stimuli in the template's `record` list (default:
  the response's `from` and any stimulus with a trigger at its onset);
  every other landing is in `events.csv` and every flip in `frames.csv`.

**D11. Flagged frames: record, do not act. DECIDED (2026-10-14).**
- Options: (a) record the tier and flags of every flip and the worst of
  them per stimulus and per trial, and do nothing else; (b) also allow
  `requeue` of a trial whose named stimulus had a dropped or flagged
  frame (`ytr_requeue()`).
- Evidence: rig_spec 8 (proposal of 2026-10-05): "The analysis, not the
  experiment definition, decides what to do with flagged frames." A run
  can require a minimum tier: `yscr_desc.min_tier` sets
  `YSCR_FLIP_BELOW_TIER`. Psychtoolbox users often repeat a trial with
  a missed flip; a requeue changes the trial counts that the analysis
  sees.
- Decision: (a). No automatic requeue. `display.min_tier` refuses a
  start on a path below it and flags frames below it in the data. If a
  requeue is ever added, it is an explicit rule that the definition
  states (a node key that names the stimulus and the flag), never a
  default; the flagged row then stays in the data, marked.

**D12. Versions and migration. DECIDED (2026-10-14).**
- Options: refuse every old version; migrate old versions at load;
  migrate only in the designer.
- Evidence: the pack reader refuses a version it does not know
  (rig_spec 1, principle 4). ysp/json.h refuses unknown keys, so an older
  player meets a newer key as "unknown key", not as a version message.
  A methods section cites the pack ID, so an old pack must still run.
- Decision: `"version"` is one integer, raised for every added
  key or changed meaning. The player refuses a newer version with "this
  player reads up to N". It migrates an older version forward at load,
  one pure step per version, and records the stored bytes' SHA-256 and
  the migrated bytes' SHA-256. Each step has a test that pins a corpus
  of old files to their migrated canonical bytes. A step that cannot
  keep the meaning refuses and names the key.

**D13. Designer state stays out of the file. DECIDED (2026-10-14).**
- Evidence: the hash covers every byte (D2). Node positions in an editor
  or a collapsed panel would change the experiment ID.
- Decision: the designer keeps its view state in the project
  (local storage), not in the definition. `note` is the only free text.

**D14. Text from tables is laid out at pack build. DECIDED (2026-10-14).**
- Evidence: rig_spec 5.2: static text is stored as laid-out glyph runs;
  layout belongs to the pack tool and the player. The values of a table
  column are known at build: they are the column's levels. The Stroop
  program lays out every word once at setup and allocates nothing in the
  frame loop.
- Decision: the pack tool lays out every literal text and every
  value a `map` or a table column can give a text stimulus, keyed by
  text and style (gap G9). The player looks up runs; it lays out text at
  run time only for a script.
- Built (pack tool v0.6.0, 2026-10-21; G9): the style is the font,
  face, size, width, line height, alignment, direction and language;
  two stimuli of one style that show one text share one block. The
  player builds the key with the tool's function, `ypak_runs_key()` in
  ysp/pack.h (pack.md 4.5, "The block key").

**D15. Web runs. DECIDED (2026-10-14).**
- Evidence: rig_spec 6: on a participant's machine timing is measured
  and excluded, never controlled, and the exclusion rule is written
  before data collection. Every web frame is tier WEB.
- Decision: a `web` object: `allowed` (default false) and
  `exclusion`, the rule as data (for example a refresh range and a
  maximum fraction of dropped frames). The player records it and the
  self-test's values; the analysis applies it. `min_tier` does not apply
  to a web run.

**D16. Waiting for a control. DECIDED (2026-11-09).**
- Options: (a) a step `{"until": CONTROL}` that splits a template into
  segments; (b) a general state machine of routines (D7's option (b));
  (c) a script.
- Evidence: each of the four tasks of signal_spec.md section 15 waits
  for a hit one to three times in a trial, always in a straight line.
  None branches. D7's `after` already builds a second op table when the
  response ends. `until` is that mechanism, used more than once.
- Decision: (a). The steps split at each `until` into segments. A
  segment is one op table, built when it starts, at the first frame whose
  onset is at or after the hit's time, with `ytl_at()`. An optional
  `timeout_s` on the step ends the trial with the outcome `"timeout"`.
  A wait with no timeout is valid: the operator can stop a session.
  The designer warns about one. D7 still holds for everything else.
  D20 amends this.

**D17. A stimulus property that follows a signal. DECIDED (2026-11-09).**
- Options: (a) a property is bound to a signal for the whole trial;
  (b) the signal and the timeline both write the property, and the later
  write wins; (c) one writer: a `mux` node chooses the property's source.
- Evidence: in `trial_visuomotor` the dot's `visible` follows a signal
  during the return and is a constant during the reach, so (a) fails.
  ysp/timeline.h's rule for a channel is "never both": one driver, so
  (b) would add the case that header refuses. Two tasks choose a color or
  a visibility per frame from a boolean.
- Decision: (c). Two value forms: `{"signal": NAME}`, and `{"when": TEST,
  "then": V, "else": V}`, where TEST is a control's name (its state) or
  `{"signal": NAME, "above": LEVEL}`. A property that any step gives one
  of these forms gets a selector channel on the timeline, a `mux` node
  and one bind row, so a `set` takes effect at its time on the timeline,
  as every step does. Constants stay timeline channels. This needs a new
  `mux` op in ysp/sig.h.

  Note (2026-11-11): "a control's name (its state)" now has a second
  meaning, since D20 calls a node of a trial's chart a state; part 3
  calls a control's boolean its *test* (D27). D17 does not say whether
  `then` and `else` take a `when`: the bimanual reach target needs three
  colors from two tests, so D27 proposes nesting. `above` lowers to
  `detect` `ge`, so a value equal to the level is above.

**D18. Freezing a cursor at a crossing. DECIDED (2026-11-09).**
- Options: (a) a step `{"freeze": CONTROL}`, as signal_spec.md section
  15.2 wrote it; (b) a key on the control, `"freeze_at": CONTROL`.
- Evidence: a step runs on the first frame after the hit, so the frozen
  point would be where the cursor was up to a frame later, at peak speed.
  `trial_visuomotor` shows the point of the sample that crossed, and its
  data files record that point. The shown endpoint and the recorded one
  must be the same.
- Decision: (b). A `latch` op in ysp/sig.h captures its input on the
  sample where the control's test is met and holds it until the next
  trial starts. A second reach inside one trial is out of scope until a
  task needs it.

**D19. What keeps its state across trials. DECIDED (2026-11-09).**
- Options: (a) everything resets at a trial's start, and a role can set
  `"keep": true` (signal_spec.md section 15, finding 12); (b) a role's
  adapter never resets at a trial boundary, the controls do, and a step
  recenters a relative role; (c) a setting per control.
- Evidence: a hand does not jump between trials. A reset of a relative
  adapter moves the cursor while the hand stays. Neither ported program
  resets per trial: `trial_visuomotor` calls `ysig_reset()` for the first
  trial only and `trial_bimanual` when both mice are placed.
- Decision: (b). A new step `{"recenter": ROLE, "to": POINT}` places a
  relative role; the loader refuses it on an absolute role. This needs
  two calls in ysp/sig.h, both journaled: a reset of the experiment's
  nodes only, and a recenter of one role. It replaces finding 12's
  `"keep"`.

  Note (2026-11-11): the loader does not know the adapter. A
  `position2d` role with `allow_relative` can get an absolute adapter (a
  tablet). D21 and part 3 (3.5) propose: the loader refuses `recenter` on
  a role that cannot be relative; the player skips it on an absolute
  adapter and counts the skip in `session.json`.

**D20. A trial as named states with exits. DECIDED (2026-11-09). It amends D16.**
- Options: (a) D16's straight line of segments; (b) named states, each
  with exits, where `until` is the shorthand for "exit to the next
  state"; (c) full statecharts, with nested and parallel states, as in
  Stateflow; (d) a script.
- Evidence: D16's `timeout_s` is already a branch. The four tasks of
  signal_spec.md section 15 do not branch, but they were chosen for their
  signal paths. Common tasks do: a fixation break that aborts or retries,
  and a trial that ends on an early press (examples_findings F3). A
  designer draws a trial as a chart. D7 refused a state machine because it
  needs transitions, conditions and a scheduler; D16 already adds the
  scheduler, and the conditions here are only hits and elapsed times.
  Nothing has parsed format version 2 yet, so nothing needs migration.
- Decision: (b).

  ```json
  "states": [
    { "name": "fixate", "steps": [{ "on": "fix" }],
      "exits": [{ "on": "held", "to": "cue" },
                { "after_s": 5, "to": "end", "outcome": "timeout" }] },
    { "name": "cue", "steps": [{ "on": "cue" }],
      "exits": [{ "on": "broke", "to": "fixate" },
                { "after_s": 0.5, "to": "go" }] },
    { "name": "go", "steps": [{ "on": "target" }, { "until": "arrived" }] }
  ]
  ```

  1. A state runs its steps. A state with no `exits` goes to the next
     listed state when its steps end, and the last one ends the trial.
     `{"until": C}` inside the steps is D16's form: the shorthand for a
     split into two states joined by one exit.
  2. An exit is `on` a control's hit, or `after_s` (above 0) from the
     state's first frame's onset, or instant: it has neither, and may have
     `"if": "$flag"`. `to` is a state's name or `"end"`. `outcome` sets the
     trial's outcome.
  3. A state arms the controls its exits name, at its entry. A hit from
     before the entry does not count.
  4. A frame tests the exits. When more than one `on` or `after_s` exit
     has fired, the earliest time wins, then the order in the list.
     Instant exits are tested when the state starts, in list order, before
     it waits for anything, and the first that is true is taken.
  5. A frame can pass through several states. The state after a timed
     exit starts on the first frame whose onset is at or after the exit's
     time (D16). If that state has no step that waits (`wait_s`, `until`,
     `then`, an `at_s` above 0) and an instant exit is true, or it has no
     exits, the next state starts on the same frame. Their steps land on
     that frame in order, and a later `set` of a property wins, by the
     timeline's rule for a channel.
  6. A cycle is valid: a retry needs one. Every cycle must contain an exit
     that waits (`on` or `after_s`), and the loader refuses a cycle of only
     instant exits, also when they have an `if`. So a chain in one frame
     ends within the number of states, and there is no limit to set. The
     designer warns about a cycle that has no timeout.
  7. The response stays at the template's level (2.6): it spans states.
     An exit can name it: `{"on": "response"}`. An exit on an early press
     needs F3 in ysp/response.h.
  8. A run records each state's name, entry onset, the exit taken and that
     exit's time, as one stream.
  9. Names: stimuli, controls, states, named clicks and flags share one
     namespace per template, and a clash is refused at load. Variables
     stay as D6 has them: flat and read-only, known at the trial's start.
  10. Flags are the one thing a trial can change. A template lists them
     in `"flags"`. Each is a boolean, false at the trial's start. The step
     `{"flag": NAME, "value": BOOL}` sets one, and `$NAME` reads it in
     later states. Each flag is a column of the trial's row. A flag has no
     scope but its trial. Any other value that changes during a trial,
     a counter among them, is a script.

  ```json
  { "flags": ["broke_once"],
    "states": [
      { "name": "note_break", "steps": [{ "flag": "broke_once", "value": true }],
        "exits": [{ "if": "$give_warning", "to": "warn" }, { "to": "fixate" }] }
    ] }
  ```

  11. Shared exits. The template's `"exits"` are exits that several states
     have, so a task does not repeat one. Each has `"in"`, the list of
     states; it is required. A shared exit is one of each listed state's
     exits and arms with them. It comes after the state's own exits in the
     list order of rule 4. It is `on` or `after_s`: an instant shared exit
     is refused.
  12. `"max_visits": N` on a state, with `"over": {"to": STATE, "outcome":
     VALUE}`, limits how many times one trial enters that state. Entry
     N + 1 runs none of the state's steps and goes to `over` instead, on
     the same frame. A cycle still needs an exit that waits (rule 6).

  ```json
  { "exits": [{ "on": "broke", "to": "fixate", "in": ["cue", "delay"] }],
    "states": [
      { "name": "fixate", "max_visits": 3, "over": { "to": "end", "outcome": "too_many_breaks" },
        "steps": [{ "on": "fix" }], "exits": [{ "on": "held", "to": "cue" }] }
    ] }
  ```

  Nested states and parallel states wait for a task that needs them. The response, the graph and the stimuli
  already run side by side.

  Note (2026-11-11): part 3 found six points that these rules leave
  open, and proposes each:
  - Rule 1: as the shorthand of two states joined by one exit, `until`
    arms its control at the first state's entry, so a hit during the
    steps before `until` ends them; D16 says the sequence waits at
    `until`. D35 (h): the wait is a state of its own.
  - Rule 3, "at its entry": a state has an entry time (the exit's time)
    and an entry frame. `trial_visuomotor` arms at the hit's time,
    between frames. D35 (b).
  - Rule 4, "A frame tests the exits": an arm at a hit's time needs the
    exit taken when the hit is drained, before the next sample. A hit
    that a frame finds needs a second `ytl_evaluate()` for the next
    state's steps to land on that frame (rule 5). D35 (d), 3.11.
  - Rule 7: `after` and `response_ends_trial` have no meaning among
    states as written. D34 (e).
  - Rule 9: the namespace also needs the signal roles and the signal
    nodes, and the reserved names `end` and `response`; a flag is read as
    `$NAME`, so it cannot have a variable's name. 3.9.
  - Rule 2: an exit's `outcome` has no default. D36.

D16 to D20 add keys and change what a step can
do, so they are format version 2 (D12).

### 1.4 Gaps in the C APIs

Found while mapping the four examples (2.14). The size is the change in
the header.

| Id | Gap | Header | Needed by | Size |
|---|---|---|---|---|
| G1 | CLOSED in ysp/timeline.h v0.8.0: a TRIGGER op's `ch` is its trigger channel and the fired event's `target` (`YTL_TRIGGER_CH(ch, code)`, `ytl_trigger_ch()`); `ytl_trigger()` and `YTL_TRIGGER()` give channel 0. Was: `ytl_trigger()` and `YTL_OP_TRIGGER` took a code only; the builder wrote target 0, so an op table could not say which trigger channel (role) fires. | ysp/timeline.h | EEG example, two output roles | Small: TRIGGER takes `ch` as its target |
| G2 | CLOSED in ysp/rigfile.h v0.5.0 (2026-10-21): the profile's `view` (`distance_mm`, `width_mm`, `width_px`: the eye to the screen's center, the lit image's width and the pixels across it) and `yrig_ppd()`, the pixels per degree at the center by ysp/gfx.h's YGFX_DEG expression. A `ygfx_view` for a drawable w pixels wide has `width_mm` = w * `width_mm` / `width_px`, so a window gets the degrees of the full screen. A profile without `view` writes and hashes as before; adding or changing one changes `yrig_hash()`. `rigfile_profile --view` writes it. The 2AFC program reads it (`--rig NAME`, `--rig-dir DIR`) and falls back to nominal geometry flagged in its meta. Was: the rig profile had no viewing geometry, and the 2AFC program hard-coded `PPD_DISTANCE_MM` and `PPD_MM_PER_PX`. | ysp/rigfile.h | Every `deg` definition | Done |
| G3 | CLOSED in ysp/trials.h v0.6.0 (2026-10-21): `desc.vars[]` of `ytr_var_choice(name, values, n)` draws one of n texts per trial from the trial's own stream of `desc.seed` and the trial index (D8; the hash is pinned and checked against a second implementation), writes it in the data line, and `ytr_restore()` and a snapshot (format 5) give it again; `ytr_var()`, `ytr_var_text()`, `ytr_trial_seed()`; rules text `var NAME choice V V ...`. A variable takes no draw from the session's generator, so no other draw moves. The 2AFC program draws its side this way. Was: jitters were durations only, and the 2AFC side came from the program's own generator. | ysp/trials.h | `vars` with `choice` | Done |
| G4 | CLOSED in ysp/trials.h v0.6.0 (2026-10-21): `weight COLUMN` is each row's count per repetition, times `reps` (the text's or the desc's), as OpenSesame's weight is per cycle; `cond_reps` beside `reps` is refused, since reps was ignored there. The Stroop program's rules say `weight weight` and `reps N`, and its C product is gone. Was: nothing multiplied the weight by `reps`. | ysp/trials.h | Stroop | Done |
| G5 | CLOSED in ysp/color.h v0.4.0 (2026-10-21): `ycol_michelson(cx, m, dir, bits, &r)` gives ysp/gfx.h's contrast for a Michelson contrast in light (`r.scene`), the largest one in gamut (`r.max`), the output codes of the peak and the trough (`r.peak`, `r.trough`) and the Michelson contrast of their light (`r.shown`). A contrast past the gamut is reported (`r.gamut`), not clipped. Luminance comes from the readings alone, so a photometer's calibration is enough. The 2AFC program uses it; against its old arithmetic, 0 of 2406 codes differ and every value is within 1.1e-16. Was: no call did this; the 2AFC program had about 20 lines with `ycol_output_code()`. | ysp/color.h | `{"michelson": ...}` | Done |
| G6 | CLOSED (2026-10-21). Every header that has a desc a definition sets exports its table, and every table has one row shape: `name, type, min, max, def, unit, doc, values`, where `values` is the "name=value" pairs of an `enum` or `enum[]` row (lower-case C suffixes, in value order) and NULL for any other type. The methods: `yst_params()` (ysp/stair.h 0.3.0), `yqst_params()` (ysp/quest.h 0.7.0), `yaep_params()` (ysp/aep.h 0.16.0). The timeline: `ytl_op_params(kind, &n)` and `ytl_desc_params()` (ysp/timeline.h 0.8.0). Response: `yrsp_params()` (ysp/response.h 0.6.0). The device families: `ydev_params(family, &n)` (ysp/device.h 0.4.0). gfx's stimulus fields: `ygfx_params()`, `ygfx_enum_params()` and the others (ysp/gfx.h 0.16.0). The rest gained `values` on 2026-10-21: `yscr_params()` (ysp/screen.h 0.9.0), `yau_params()` (ysp/audio.h 0.7.0), `ycol_desc_params()` (ysp/color.h 0.3.0), `yol_*_params()` (ysp/outline.h 0.4.0), `yvid_params()` (ysp/video.h 0.5.0), `yrdk_params()` (ysp/rdk.h 0.3.0) and `ygfx_desc_params()` (ysp/gfx.h 0.17.0). Each header's test checks its rows against its desc and its C enums; `tests/adapt/param_tables_test.c` checks every table against the shared rule (conventions.md, "Parameter tables"). | those headers | The loader's key check and the designer's panels from one source | Done |
| G7 | CLOSED in ysp/json.h v0.2.0 (2026-10-21). Was: the canonical writer wrote a number's source text; only `yjs_new_fixed()` and `yjs_new_int()` made canonical numbers. Now every number is written as its exact value in one spelling, and `yjs_format_double()` gives a double's shortest round-trip decimal, the same bytes on MSVC, MinGW, gcc and emcc (json.md). | ysp/json.h | D2 | Done |
| G8 | CLOSED in ysp/device.h v0.5.0 and ysp/response.h v0.7.0 (2026-10-21): a response rule (`ydev_rule`, the box's `desc.rule`) writes the response's code on the input's reader thread, its window and choices from the collector (`yrsp_rule_arm()`, `yrsp_rule_window()`, `yrsp_rule_choices()`); the record has `YDEV_OUT_RULE` and the press's t. A code that the final response does not match (an anticipation once the final onset came, a late press, another choice, a press the collector refused, a response without its code) sets `YRSP_R_SENT_MISMATCH` in the row's `rsp_flags` (`yrsp_rule_sent()`). Measured (`examples/device/response_lag.c`, in-process box and output, Windows 11, i7-1360P, the timing guard): press stamp to write start p50 8.8 ms, max 16.7 ms through a 60 Hz frame loop before; p50 5.2 us, p99 19.3 us, max 38.7 us through the rule (600 presses each). A real box adds its USB delivery and write on both paths. Was: a response-driven output waited for the frame loop to see the response, up to a frame after the press. | ysp/response.h, ysp/device.h | Response triggers (EEG) | Done |
| G9 | CLOSED in the pack tool v0.6.0 (2026-10-21). The tool reads the experiment entry; when it is a version 1 definition, every text its `text` stimuli can show goes into one GLYPHRUNS entry named after the experiment entry (`experiment/main.json` gives `experiment/main.ysprun`), one block per text and style (font, size, width, line height, alignment, direction, language), keyed by both through `ypak_runs_key()` in ysp/pack.h, which the player calls too; stimuli of one style that show one text share the block. A text is a literal, the levels of a string column of any table a node names, a node's `vars` value or choices, or every value of a lookup's map. A glyph runs resource of the same name adds its blocks and fallback fonts. A text known only at run time (a built-in or a session field used directly, a script, a number column, a layout key from a variable) is refused at build by name (pack.md 4.5 and 15). Was: glyph-run blocks were listed by hand in the source description; nothing derived them. | pack tool | D14; Stroop | Done |
| G10 | CLOSED in ysp/table.h v0.3.0 and ysp/trials.h v0.6.0 (2026-10-21): a table and a data file hold 256 columns (`YTB_MAX_COLUMNS`, `YTR_DATA_MAX_COLS`); the header line goes out in pieces, so long names do not hit the row buffer. No allocation in a write, as before; the writer is 38,224 bytes (was 18,760) and a row costs the same per column (docs/trials.md, v0.6). Was: at most 64 columns. | ysp/trials.h, ysp/table.h | Templates with many recorded stimuli | Done |
| G11 | CLOSED in ysp/trials.h v0.6.0 (2026-10-21): `rate=display` in the rules text (`YTR_RATE_DISPLAY` in C) snaps a jitter to `desc.display_rate_num / display_rate_den`, which the player sets from the screen after `ytr_rules()`; open() refuses the session, naming the jitter, without it. `ytr_format_rules()` writes `rate=display` back; the meta line gives the rate drawn on. Was: `rate=` took a fixed number only. | ysp/trials.h | EEG example (`rate=display`) | Done |
| G12 | Closed (2026-10-21): ysp/stair.h 0.3.0 has `yst_save()` and `yst_open_saved()`, in the form of quest's, aep's and trials' (versioned, checked, a mismatched desc refused, the error in the handle), so a resume no longer replays the staircase's updates. | ysp/stair.h | Resume | Small; closed |
| G13 | CLOSED in ysp/dataset.h v0.1.0 (2026-10-21; docs/dataset.md). The folder, `session.json` (canonical, `"closed": false` until the finish, each rewrite atomic), the four tables and `streams/` through the trials writer, adapters from flip records, inputs, timeline events and clock fits, and the zip for the web. Left to the player: what it puts in `session.json` and which rows it drains from the event ring and the device records. Was: the player's data set (D9) was not built; the trials writer's sidecar is key=value lines. | ysp/dataset.h | D9 | Done |
| G14 | OPEN (2026-11-11): ysp/gfx.h has `YGFX_PX` and `YGFX_DEG` only. Proposed: `YGFX_CM`, one scale factor, px per cm = drawable width x 10 / `view.width_mm`, refused without `view.width_mm` | ysp/gfx.h | D23; `trial_bimanual` | Small |
| G15 | OPEN (2026-11-11): no variable depends on the last trial's value. Proposed: `ytr_var_walk2d(name, step, lo_x, hi_x, lo_y, hi_y, x0, y0)` and rules text `var NAME walk2d STEP LO_X HI_X LO_Y HI_Y X0 Y0`: trial i draws angles from a sub-stream of its trial stream until the previous point plus `step` (cos a, -sin a) is inside the box; a leaf's first trial starts at (x0, y0); the snapshot (`ytr_save()`) and `ytr_restore()` keep the last point; the data line gets NAME_x, NAME_y, NAME_from_x, NAME_from_y and NAME_deg | ysp/trials.h | D30; `trial_bimanual` | Medium |
| G16 | OPEN (2026-11-11): a `detect` with `hold` 0 that is armed while its test is true gives no hit until the test fails (the CROSSINGS rule). Proposed: `entry` value `now` (2): a test true at the arm is a hit at the arm's time | ysp/sig.h | D35; `trial_bimanual`'s `ready` | Small |
| G17 | OPEN (2026-11-11): `ysig_set()` on a stateless node leaves its held value, and its stateless users', at the old parameters until the next sample (docs/sig.md, "The target's color in trial_bimanual"). Proposed: the call recomputes them from their inputs' held values, journaled as now | ysp/sig.h | 3.6; `trial_bimanual`'s block changes | Small |
| G18 | OPEN (2026-11-11): the rig profile has no adapter block (signal_spec 11). Proposed by devices_spec 11.2: per signal role, the device, the adapter's type and units, and its scale per count or its map | ysp/rigfile.h | D21; both acceptance tasks | Medium |
| G19 | OPEN, not needed by a task (2026-11-11): `ysig_frame_in` has one base time for every generator. Proposed when a task needs two generators with different `from`: a time origin per generator | ysp/sig.h | D39 | Small |
| G20 | OPEN, version 3 (2026-11-11): a press choice has no region, and a window cannot end on an early press (F3). Proposed: a rectangle or a circle on `yrsp_choice`; `desc.end_on_early`, `YRSP_R_EARLY_END`, `yrsp_ended_by()` | ysp/response.h | D31; mouse tracking; D20 rule 7 | Small each |

### 1.5 Proposed for version 2 (2026-11-11)

Part 3 is the reference for version 2. Where D16 to D20 do not decide a
point that the reference needs, an entry below proposes it. Each entry is
PROPOSED, not DECIDED. Each says whether the two acceptance tasks
(`trial_visuomotor`, `trial_bimanual`) need it, or whether it can wait for
version 3. "A designer learns" names the words and fields that a designer
user must learn for it (signal_spec 15.7, "Words a designer user must
learn").

**D21. Signal roles. PROPOSED (2026-11-11).**
- Options: (a) a top-level `roles` beside `devices`, as signal_spec 10
  says; (b) one `devices` set, where a role with `type` is a signal role;
  (c) signal roles inside each template.
- Evidence: the two kinds lower to different structs (a `ydev_desc` and
  a `ysig_role`), and the rig binds them differently (a device, or an
  adapter that ends in the role's value). The rig binds a role once per
  session (devices_spec 11.1), so (c) would bind one hand per template.
  No task has a role that is both an event role and a signal role. Both
  acceptance tasks bind mice as signal roles only.
  `trial_visuomotor` and `trial_bimanual` integrate counts inside bounds
  (the window, or the window's size from the start); their scripts never
  reach the bounds. A designer learns one word, role, if the designer
  shows both sets in one panel.
- Proposal: (a), with one namespace for the names of `devices` and
  `roles`. Keys, adapters and checks: 3.5. A relative role reads [0, 0]
  at the session's start. A role has no bound unless its adapter has
  one. A value that drives a stimulus must be in `display.units`; a
  `workspace_cm` role needs a gain to the display, which version 2 does
  not have. The acceptance tasks use display units, as their C programs
  do. Needed by both acceptance tasks. The gain waits for a task that
  needs workspace units.

**D22. One coordinate frame, and angles on the screen. PROPOSED (2026-11-11).**
- Options: (a) version 1's frame for every unit (the origin at the
  display's center, x right, y down), with every angle that a designer
  sets counterclockwise on the screen; (b) y up for `cm` and `deg`, as in
  PsychoPy; (c) each control states its frame.
- Evidence: version 1's `px` and `deg` are y down: 2.14's Stroop banner
  at `"y": -30` is above the center. (b) changes the meaning of every
  version 1 `y`, and its migration must rewrite values. The C programs
  differ: `trial_visuomotor` uses window pixels from the top-left, y down;
  `trial_bimanual` uses cm, y up. An `affine` angle turns
  counterclockwise on the screen only with y up: `trial_visuomotor`
  writes `-rot_deg` for a counterclockwise turn (start_trial). A mirror
  written as "swap x and y" is a different mirror in a y-down frame.
  Reach studies give a rotation counterclockwise on the screen.
- Proposal: (a). The loader converts a screen angle into the frame's
  (3.4). `trial_bimanual`'s y columns then map with a sign change, and
  `trial_visuomotor`'s x and y with the center's offset (3.18). A
  designer learns nothing new: the panel shows the direction. Needed by
  both acceptance tasks.

**D23. `cm` as display units (finding 14). PROPOSED (2026-11-11).**
- Options: (a) `display.units` `"cm"`, from the rig's `view`; (b) cm in
  signal roles only, with stimuli in px; (c) a unit per stimulus.
- Evidence: `trial_bimanual` gives every size in cm (target radius 0.8,
  reach 12, workspace 20 x 20). The rig profile's `view` gives `width_mm`
  and `width_px` (G2). ysp/gfx.h has `YGFX_PX` and `YGFX_DEG`, each one
  scale factor; cm is one more. Without `--px-per-cm` the C program draws
  at a NOMINAL 32 px per cm and says so in its files; the format has no
  nominal geometry (D4). (b) puts two unit systems in one task.
- Proposal: (a), lowered to `YGFX_CM` (G14). A run without `view` is
  refused, as for `deg`. The acceptance run's rig has `width_px` 1024 and
  `width_mm` 320: 32 px per cm, the C program's NOMINAL value. A designer
  learns one more value of a field already shown. Needed by
  `trial_bimanual`.

**D24. The components and their keys. PROPOSED (2026-11-11).**
- Options: (a) the five components of signal_spec 15.1, with `distance`,
  `peak`, `signal_lost` and `speed_below` of 15.7; (b) fewer, broader
  components (one `test` with a list of conditions); (c) the graph only.
- Evidence: 15.6 counts what an author sets: the components carry the
  signal path of all four paper tasks, and the gain is in the words, not
  in the count. The acceptance tasks need `cursor`, `region` (with
  `outside`, `speed_below` and `rate`), `movement`, `moving_target`,
  `trace`, `distance` (the return ring) and `peak` (the peak speed).
  Neither needs `signal_lost` or the cursor's `lowpass_hz` and `also`
  (the gaze task). Three details of the C programs force keys:
  `trial_bimanual` tests its holds once a frame (`rate`) and starts
  tracking when the cursor is already in the target (a hit at the arm,
  G16); `trial_visuomotor` measures its movement onset from where the
  hand was at the hold's end, and `trial_bimanual` from the start's
  center (`origin`).
- Proposal: (a), with the keys of 3.7. A `movement` is a response choice
  only: its hit is the response. `signal_lost` is specified for the gaze
  task and can wait. `lowpass_hz` and `also` wait for a gaze task. A
  designer learns eight component names, and for each control what it
  gives: a value, a test, hits (3.7). Needed by both acceptance tasks,
  except `signal_lost`.

**D25. Shared controls. PROPOSED (2026-11-11).**
- Options: (a) a top-level `controls`, as `stimuli` are top-level; (b)
  a control repeated in each template; (c) a template that includes
  another's controls.
- Evidence: `trial_bimanual`'s two templates repeat one `cursor` with 7
  values (15.7, "Repeated controls"). A control's variables are read per
  trial, as a stimulus's are, so a shared control costs nothing at run
  time: each template's graph gets its own copy of the nodes.
- Proposal: (a). A template's control cannot have a top-level control's
  name. A designer learns that a control, as a stimulus, can sit at the
  top level. Needed by `trial_bimanual` (or it repeats the cursor).

**D26. The graph view, `signals`. PROPOSED (2026-11-11).**
- Options: (a) `signals` in version 2, restricted to the ops that a
  definition can write; (b) wait for a task that the components cannot
  write; (c) every op, sources included.
- Evidence: neither acceptance task needs it. Without it, a need that no
  component has becomes a script or a new component. signal_spec 10 and
  15.1 put the graph under the components as the advanced view. Sources,
  binds, records and emits are what the components and the rig already
  write; a source in a definition ties it to a device (5.5).
- Proposal: (a), 3.8. It can wait for version 3: adding it then is one
  version step and no migration. A designer learns nothing until they
  open the graph view.

**D27. Where the value forms go. PROPOSED (2026-11-11).**
- Options: for `{"signal": ...}` and `{"when": ...}` beyond a step's
  `set` (D17): (a) also in `columns` and in a region's `center`; (b) in
  every key; (c) only in `set`. For `then` and `else`: (d) a `when`
  inside them; (e) constants only. For a lane: (f) `NAME.x`; (g) a
  `lane` key.
- Evidence: finding 15 needs a value and a test in a column
  (`peak_speed_cm_s`, `fast`). Finding 7 needs a center that moves. The
  bimanual reach target is white, then yellow or blue: three values from
  two tests, so (e) cannot draw it. Section 10 already writes
  `{"signal": "cur.x"}`. D17 says a control's "state"; D20 uses "state"
  for a node of a trial's chart, so one word has two meanings for a
  designer user. (b) puts a per-frame value into keys that are read once
  (a texture, a method's grid).
- Proposal: (a), (d), (f), with an element of a point that may be a
  variable; the reference calls a control's boolean its *test* (3.7,
  3.10). A designer learns "follows" (a signal), "when ... then ... else",
  and the word test. Needed by both acceptance tasks.

**D28. A region centered on a signal (finding 7). PROPOSED (2026-11-11).**
- Options: (a) `distance` with two inputs, then a `detect` on its lane;
  (b) a `detect` whose center is a second input (a ysp/sig.h change);
  (c) `diff`, then a `detect` on the norm.
- Evidence: ysp/sig.h v0.2.0's `distance` takes a second input of the
  same lanes. (a) and (c) need no header change. (b) makes `a.x` and
  `a.y` mean two things. A hit's value must be the tested position, not
  the distance; it comes from the ring of `of` at the hit's time.
- Proposal: (a), with no header change. Needed by `trial_bimanual`
  (`inside`).

**D29. Random values per block (finding 8, F23). PROPOSED (2026-11-11).**
- Options: (a) a `vars` draw per run of a node, `{"uniform": [lo, hi],
  "n": N, "per": "node"}`, drawn by the player from the node's seed; (b)
  per-block variables in ysp/trials.h (F23's proposal); (c) a table
  column with values fixed at pack build.
- Evidence: `trial_bimanual` draws 14 phases per block, the same for
  each trial of the block. In the definition each block is one leaf, so
  a draw per run of the node is a draw per block. D8 seeds each node's
  generator from S, the node's path and its iteration; a separate stream
  from that seed moves no trial's draw. (b) is needed only when one leaf
  holds several blocks (`block_size`). (c) gives every participant the
  same phases.
- Proposal: (a), 3.14. (b) waits for a task with blocks inside one leaf;
  its change is `desc.vars[]` entries with a block scope in ysp/trials.h,
  drawn at a block's first trial and restored by `ytr_restore()`. A
  designer learns "uniform" and "once per block". Needed by
  `trial_bimanual`.

**D30. A chained target, `walk2d` (finding 9). PROPOSED (2026-11-11).**
- Options: (a) a variable kind in ysp/trials.h; (b) a draw of the
  player's own; (c) a script.
- Evidence: each reach of `trial_bimanual` starts at the last target
  (new_target). The value depends on the previous trial, so a resume
  must give it again: `ytr_restore()` and a snapshot carry the trial
  variables (G3), and a draw that the player makes outside them would
  not resume. A draw repeats until the point is inside the workspace,
  so it takes a variable number of outputs. It depends on the last
  target only, not on the hand (15.7), so it is no script.
- Proposal: (a), G15, with the format's form in 3.14. A designer learns
  "each target a step from the last, inside a box". Needed by
  `trial_bimanual`.

**D31. A press inside a stimulus, `click_in` (finding 10). PROPOSED (2026-11-11).**
- Options: (a) a choice `{"click_in": STIMULUS, "name": N}`, with the hit
  test in ysp/response.h; (b) the hit test in the player.
- Evidence: only the mouse-tracking task needs it. A press carries its
  x and y; the collector already filters a choice by kind, device and
  control.
- Proposal: (a), in version 3. The change: `yrsp_choice` gains a
  rectangle or a circle in window coordinates, and a press outside it is
  not that choice. F3 goes with it (`desc.end_on_early`,
  `YRSP_R_EARLY_END`, `yrsp_ended_by()`), for D20 rule 7's exit on an
  early press. Not needed by the acceptance tasks.

**D32. Roles bound by a click (finding 11). PROPOSED (2026-11-11).**
- Options: (a) a node `{"assign_roles": [...], "by": "click"}`; (b) the
  rig profile binds each mouse by its device path; (c) the operator
  binds in the runner.
- Evidence: `trial_bimanual` asks for a click of each mouse. Two mice of
  one model have one vendor and product id, but each has a stable path
  on Linux (evdev `by-path`). No data column depends on how a role was
  bound, and `--sim` feeds each role.
- Proposal: (b) in version 2; (a) waits for version 3 and needs F24
  (`yscr_device_name()`). Not needed by the acceptance tasks.

**D33. Columns from controls (finding 15). PROPOSED (2026-11-11).**
- Options: (a) four column forms: `{"hit": ...}`, `{"time": ..., "from":
  ...}`, `{"signal": ...}`, `{"when": ...}`; (b) traces only, and every
  value in the analysis.
- Evidence: the acceptance programs write a crossing's time, the hand at
  the crossing, a peak speed, a fast flag and the hand whose report
  crossed. Each is one value that the graph or the collector holds.
  Angles, the MSE, the alignment matrix and frequency responses are
  arithmetic over a trace (15.7: arithmetic stays in the analysis).
- Proposal: (a), 3.15, and the column `rsp_role` for a response with
  signal choices. A designer learns that a column can show a control's
  time, value or test. Needed by both acceptance tasks.

**D34. A response whose choices are signals. PROPOSED (2026-11-11).**
- Options: for the role: (a) no `role`, as each choice's `movement`
  names its input; (b) `role` required. For when the window opens: (c)
  at the movement's `from`; (d) at the trial's first frame. For
  `response_ends_trial` with `states`: (e) a shared exit to `end`; (f)
  refused.
- Evidence: 15.7: "A response whose choices are signals has no role".
  `trial_visuomotor` arms its collector at the hold's end and measures
  the onset distance from the hand there; `trial_bimanual` arms at the
  target's onset and measures from the start's center. D20 rule 7 keeps
  the response at the template's level, and `after` has no place among
  states. jsPsych users know `response_ends_trial`.
- Proposal: (a), (c), (e), 3.13. A designer learns that a movement can
  be a response. Needed by both acceptance tasks.

**D35. When a state arms, and when an exit is taken. PROPOSED (2026-11-11); it specifies D20 rules 1, 3, 4 and 5.**
- Options: for the arm: (a) at the entry frame's onset; (b) at the time
  of the exit that led to the state. For the exit: (c) at a frame (D20
  rule 4 as written); (d) when the player drains the hit, after each
  event it feeds and after `ysig_frame()`. For a region whose test is
  already true at the arm: (e) ysp/response.h's CROSSINGS rule (no hit
  until the test fails); (f) a hit at the arm. For `until`: (g) the
  steps before it and the wait are one state; (h) the wait is a state of
  its own.
- Evidence: `trial_visuomotor` arms its crossing detect and its collector
  at the hold's hit time, between frames. One frame's poll holds many
  samples (8 kHz at 60 Hz: 133), and a detect armed at the next frame
  would not test those between the hit and the frame. `trial_bimanual`
  starts tracking on the first frame that has the cursor in the target
  and still, also when it was there before the wait began. ysp/sig.h's
  detect keeps a crossing that is true at the arm from being a hit (G16).
  D16 says the sequence "waits" at `until`; with (g) the control is
  armed at the state's entry, before the steps that precede `until`, and
  a hit during those steps ends them early.
- Proposal: (b), (d), (f) for a region named by an exit, and (h); 3.11.
  A designer learns nothing new: the chart behaves as drawn. Needed by
  both acceptance tasks.

**D36. A trial's outcome with states. PROPOSED (2026-11-11).**
- Options: (a) the outcome of the exit that reached `end`, else version
  1's rule from the response, else `"done"`; (b) an outcome required on
  every exit to `end`; (c) `"done"` always.
- Evidence: D16 and D20 give an exit an `outcome` but give no default.
  A trial of either acceptance task ends through its steps, with no
  outcome. `trial_visuomotor` passes 1 to `ytr_update()` for every
  trial; version 1 passes `YTR_INVALID` for a trial with no response,
  which a leaf with no tracks and no `requeue` does not run again. An
  `outcome` on an exit to a state other than `end` has no effect unless
  a later exit keeps it, which the chart does not show.
- Proposal: (a), 3.11; `ytr_update()` gets the template's `outcome` (2.5),
  whose default with `states` and no `key_answer` is 1; an `outcome` on
  an exit whose `to` is not `end` is refused. Needed by both acceptance
  tasks (a default is forced).

**D37. The state stream (D20 rule 8). PROPOSED (2026-11-11).**
- Options: (a) rows of `events.csv` with `kind` `"state"`; (b) a file
  `streams/states.csv`; (c) columns of `trials.csv`, per state.
- Evidence: ysp/dataset.h's event row already says that its `text` holds
  "a state", and its `frame` joins `frames.csv`: no header change. (c)
  cannot hold a cycle (a retry enters a state many times). (b) takes a
  name that a trace can have.
- Proposal: (a), 3.15. Needed by D20 rule 8; both acceptance tasks have
  states.

**D38. Traces. PROPOSED (2026-11-11).**
- Options: for a frame row: (a) the held value at `ysig_frame()` (a
  `record` per frame); (b) each value at the frame's flip record's onset
  (`ysig_value_at()`). For the rows kept: (c) a window from `from` to
  `to`; (d) the whole trial.
- Evidence: `trial_bimanual` reads each frame's hands at its flip
  record's onset, a frame later, from rings (sig.md, "Read a value at a
  flip record's onset"); its frequency analysis needs the onset that was
  shown. `trial_visuomotor` writes the samples from the hold's end to the
  crossing, both included. A `join` takes at most 4 lanes;
  `trial_bimanual`'s frame rows have 9.
- Proposal: (b) and (c), 3.7 and 3.15. A sample trace is one `join` and
  one `record`, at most 4 lanes; a frame trace has no lane limit but the
  data writer's 256 columns. Needed by both acceptance tasks.

**D39. A generator's time, and steps on a control. PROPOSED (2026-11-11).**
- Options: for a generator's time 0: (a) the trial's first frame, the
  base time that ysp/sig.h reads; (b) a state's entry, `from`; (c) a step
  that restarts it. For the control keys that a step drives: (d) every
  number key; (e) a listed few.
- Evidence: `trial_bimanual` anchors its timeline at the first moving
  frame, so the target's sum starts there; the definition's trial starts
  at the wait, an unknown time earlier. `ysig_frame_in` has one base time
  for every generator. Principle 3: a parameter changes at a trial
  boundary, an input at any time. The tracking ramp is a tween on an
  input (`scale`'s factor).
- Proposal: (b): the player anchors a second timeline base at that
  state's entry frame and gives its base time to `ysig_frame()`; two
  generators with a different `from` in one template are refused
  (G19 lifts that). (e): only `moving_target.amplitude` in version 2.
  Needed by `trial_bimanual`.

**D40. Signal roles' samples in the data set. PROPOSED (2026-11-11).**
- Options: (a) input rows in `events.csv` (`kind` the device's kind,
  `role` the role); (b) a stream per role; (c) none (the journal only).
- Evidence: principle 7: the raw samples replay every value. signal_spec
  5.5: the data records, per role, the adapter, the device and its
  identity, the calibration's hash and the raw samples. ysp/dataset.h
  already writes input rows (`yds_event_from_input()`). Size:
  `trial_bimanual --sim` feeds about 1.7 million reports in 14 virtual
  minutes.
- Proposal: (a), and per signal role in `session.json` (3.15). No
  acceptance column needs it; replay does. It can wait.

**D41. The version 1 gaps, settled at version 2. PROPOSED (2026-11-11).**
1. The `end` mark. Options: reserve it; leave it a label. Evidence: the
   player ends a trial with the code `0x7fffffff`, so no mark name
   collides; in version 2 `"to": "end"` is an exit's target, not a mark.
   Proposal: a mark's name is a label in `events.csv`, outside the
   template's namespace (3.9); `end` stays a valid mark name.
2. The `visible` stimulus key. Options: keep it at its table default;
   drop it. Evidence: the player ignores it (player.md, "`visible` as a
   key"); `on`, `off` and D17's forms drive the property. Proposal:
   version 2 refuses it on a stimulus, and the migration from version 1
   leaves it out (3.2).
3. `dot_edge` on an `rdk`. Options: add `dot_edge` for gfx's dot edge
   now; settle `edge` and wait. Evidence: the loader and the player give
   `edge` to `yrdk_desc`; no task sets gfx's dot edge. Proposal: `edge`
   on an `rdk` is yrdk's; gfx's dot edge keeps its default and has no key
   until a task needs one, and `dot_edge` is its name then.
4. `$contrast_shown` per stimulus. Options: one variable per stimulus;
   keep one. Evidence: no task shows two stimuli with `{"michelson":
   ...}` in one trial. Proposal: keep one variable and the refusal of two
   such stimuli in one trial.
5. The defaults table. Options: in this page; in experiment.md.
   Evidence: D2 says the defaults are the format's, listed in this page
   per version; version 1's table is experiment.md's (2.11). Proposal:
   part 3 gives a default for every version 2 key (3.17), and
   `yexp_defaults(2)` lists the same rows, checked by the loader's test.
Needed by the acceptance tasks: item 5 only.

## Part 2: Reference

Examples on this page are compact. The canonical form (D2) sorts keys
and puts one member on each line.

### 2.1 The file

| Key | Value | Default |
|---|---|---|
| `format` | `"ysp-experiment"` | required |
| `version` | integer, 1 | required |
| `name` | string, `[A-Za-z_][A-Za-z0-9_]*` | `""` |
| `session` | `{"fields": {NAME: {"type": "integer" \| "string", "required": bool}}}`: values the operator gives at start (the web: the URL query) | `participant`, integer, required |
| `display` | `units` (`"px"`, `"deg"`), `background` ([r, g, b], linear device RGB), `calibration` (`"required"`, `"optional"`), `min_tier` (1 to 3) | `px`, [0.5, 0.5, 0.5], `optional`, none |
| `devices` | object, role name to role (2.9) | `{}` |
| `stimuli` | object, name to stimulus (2.7) | `{}` |
| `methods` | object, name to method (2.8) | `{}` |
| `trials` | object, name to template (2.5) | required |
| `timeline` | array of nodes (2.4) | required |
| `web` | `allowed` (bool), `exclusion` (object, recorded only) | `false` |
| `note` | string, on any object | none |

Names of stimuli, templates, methods, roles, nodes, variables and
columns follow `[A-Za-z_][A-Za-z0-9_]*`, at most 63 bytes (the data
writer's column rule). A pack entry is named by its exact pack name
(pack.md 2.7), for example `"faces/f01.ysptex"`.

### 2.2 Values

| Form | Meaning |
|---|---|
| A number, a string, a bool, an array of numbers | The value |
| `"$name"` | A variable (2.3). A literal text that starts with `$` is written `"$$..."` |
| `{"by": VALUE, "map": {KEY: VALUE, ...}}` | A lookup. KEY is the text of `by`'s value; a bool is `"true"` or `"false"`; a number is its canonical text (`0.50` is `"0.5"`). The loader checks that `map` has every level that `by` can take (table levels, `choice` lists, outcomes). The pack tool does it at build (v0.7.0, pack.md 16), in each leaf that reaches the lookup: a level without a key is an error that names the stimulus, the key and the levels; a key no level reaches is a warning. A boolean built-in takes only the values the leaf's rules can make: `practice` is `"true"` only with `practice N`, `warmup` with `warmup N`, `after_break` with `block_size N` or blocked groups; otherwise `"false"` only, and a `"true"` key is a key no level reaches |
| `{"linspace": [lo, hi, n]}` | n values from lo to hi (`yqst_linspace()`) |
| `{"gamut": "stimulus.property"}` | The largest value of the property inside the display's gamut (`ycol_max_scale()`; for a `michelson` contrast, `ycol_michelson()`'s `max`) |
| `{"michelson": VALUE}` | Only for `contrast`: a Michelson contrast in light through the calibration; defines `$contrast_shown` (`ycol_michelson()`: `scene` is the stimulus's contrast, `shown` is `$contrast_shown`) |

Durations are seconds (1.1), and their keys and variables end in `_s` (D4). A value that a node or a template gives
can reference any variable known at that point. A step value is filled
when its sequence is built (2.5).

### 2.3 Variables

| Name | Known from | Value |
|---|---|---|
| a table column | trial start | the row's value |
| a `vars` name | trial start | the node's value or draw |
| a jitter name | trial start | the drawn duration, seconds |
| a session field | session start | the operator's value |
| `trial_index`, `block`, `rep`, `condition` | trial start | `ytr_trial_info` |
| `practice`, `warmup`, `first_in_block`, `after_break` | trial start | `ytr_trial_info` flags |
| `trial_seed` | trial start | the trial stream's 64-bit seed (D8): `ytr_trial_seed()` |
| `method`, `level`, `estimate` | trial start (`estimate`: after the update) | the track's name, its proposal in stimulus units, its estimate |
| `response`, `rt_s`, `responded` | `after` | `yrsp_result`: the choice's key (with no `choices`, the key's name), `rt_ns` in seconds, `YRSP_R_RESPONDED`. On a timeout `response` is the empty string and `rt_s` is empty: a lookup by `$response` that a timeout can reach needs a `""` key, and `$rt_s` is refused in a number key that a timeout can reach (ysp/experiment.h v0.1.1). Only a press reaches `outcome`, `method_value`, the steps of `{"if": "$responded"}` and a lookup's entry that only a press selects (v0.1.2) |
| `correct` | `after` | the response's key equals `key_answer` |
| `outcome` | `after` | `"correct"` or `"wrong"` with a `key_answer`, `"responded"` without one, or `"timeout"` (ysp/experiment.h 0.1.3) |
| `contrast_shown` | trial start | with `{"michelson": ...}` only |

A table column, a `vars` name or a jitter with a built-in name is
refused at load.

### 2.4 Nodes

A node has `timeline` (a container) or `trial` (a leaf), not both.

| Key | Value | Lowers to |
|---|---|---|
| `name` | unique among siblings; the path `a/b` names the node in data and seeds | `node` column; D8 |
| `timeline` | array of nodes, run in order | the player's loop |
| `trial` | a template name | 2.5 |
| `timeline_variables` | a pack TABLE entry | `ytb_open_view()`, `ytr_desc.table`; a container runs its children once per row, and the row's values are inherited |
| `rules` | array of rules statements, joined with LF | `ytr_rules()`, the node's participant from the session field `participant`; errors name the element and the column |
| `tracks` | array of method names | `ytr_desc.tracks[]` = `ytr_track(&method, done)` |
| `interleave`, `track_rate` | `"random"`, `"round_robin"`; a number | `ytr_desc.interleave`, `track_rate` |
| `vars` | NAME to a value, or `{"choice": [v, ...]}` drawn per trial | a value: a constant column; a choice: `ytr_desc.vars[]` = `ytr_var_choice()`, values as their canonical text, `desc.seed` = the node's seed (D8) |
| `data` | column to value: constant columns, inherited by children | trial row columns |
| `repetitions` | containers: run the children N times | the player's loop; iteration in the seed |
| `if`, `unless` | `"$bool"`, read when the node starts | jsPsych's `conditional_function` |
| `loop` | `{"until_accuracy": p, "max": n}`: run again until the last iteration's proportion correct reaches p, at most n times | jsPsych's `loop_function` for "practice until criterion" |
| `requeue` | `"no_response"` | `ytr_requeue()` instead of `ytr_update()` for a timeout |
| `record_data` | bool | no `ytr_data_write()` for its trials |

A leaf with neither a table nor tracks has one condition
(`n_conditions` 1, `reps` from its rules, default 1). Each leaf opens one
`ytr_trials` with `rng = ytr_splitmix` on its node seed. A track trial
whose response timed out gives `YTR_INVALID` and is not told to its
method, so the track runs it again (as in the 2AFC program).

### 2.5 Templates

| Key | Value | Lowers to |
|---|---|---|
| `events` | array of steps, from trial time 0, the trial's first frame | one `ytl_op` table, `ytl_run()` on the trial base after `ytl_clear()` and `ytl_anchor(&tl, TRIAL, f.onset, 0)` |
| `response` | 2.6 | `yrsp_desc`, `yrsp_arm()` at the first frame |
| `after` | array of steps, from `max(end, events end)`; `end` is the response's end, the window's end when `response_ends_trial` is false, or 0 with no response | a second op table after `ytl_at(&q, max(bt, events_end))`, `bt` from `ytl_base_time()`; then `YTL_MARK` end |
| `record` | array of stimulus names | per-stimulus columns (2.10) |
| `columns` | column to value | `ytr_col` entries |
| `outcome` | `"correct"`, or an integer or boolean value (ysp/experiment.h v0.1.1 refuses text and other numbers) | `ytr_update()`'s outcome; `YTR_INVALID` when no response. Default: `"correct"` when the template has a response; empty when it has none |
| `method_value` | value | what the method's update hears; default `$level`. Ignored when no leaf with tracks runs the template: a written one is a warning, is not in the checked form, and is left out of the canonical bytes, so it does not change the experiment ID |

Steps:

| Step | Lowers to |
|---|---|
| `{"on": S}`, `{"off": S}`; S a name or an array (with `"stagger_s": dt`) | `YTL_ON` / `YTL_OFF` on S's `visible` channel (`ytl_on_n()` with a stagger); a sound: `ytl_peek()` and `yau_play_at()` (rig_spec 4.5.1) |
| `{"wait_s": dt}` | `YTL_WAIT` |
| `{"at_s": t}` | `YTL_AT` |
| `{"set": S, "props": {P: v}}` | `YTL_SET_VALUE` on S.P's channel |
| `{"to": S, "props": {P: v}, "duration_s": d, "ease": e, "delay_s": d}` | `YTL_TO` per property, `ytl_tween_desc` |
| `{"then": ...}` | `YTL_THEN`: as `to`, then the cursor moves to its end |
| `{"trigger": CODE, "role": R}` | `YTL_TRIGGER_CH(channel(R), CODE)`; when it fires, `yscr_trigger(&scr, e.target, e.code)` writes at the flip's planned vblank (G1, closed in ysp/timeline.h v0.8.0) |
| `{"mark": NAME}` | `YTL_MARK`, code = NAME's index |
| `{"if": "$bool", "steps": [...]}`, `{"unless": ...}` | the steps go in the table when the variable is true (false) as the table is built |

Note: `at_s` is trial time in `after` as in `events`, not time from
`after`'s start. A step in `after` that follows `{"at_s": t}` with `t`
before `after`'s start goes at `t`. On a template with no response,
`after` starts at the events' end, so such a step can land while `events`
still runs (the player, 2026-11-10; the format does not refuse it).

Eases are ysp/timeline.h's names in lower case (`"linear"`,
`"cosine"`, `"cubic_out"`, ...). The player gives each stimulus
property that a step drives a channel and an `ygfx_bind` row, and runs
`ytl_check_ops()` on each table at load with every variable at its
extremes (a jitter's `lo` and `hi`), so a bad op fails before the
session.

### 2.6 Responses

| Key | Lowers to |
|---|---|
| `role` | the role's source: a keyboard role is `yrsp_sdl_source()`; a device role's events come through the screen's input bridge (`yrsp_feed_screen()`) |
| `choices` | `yrsp_desc.choices`: `"left"` or `{"key": "r", "name": "red"}`; for a button role, the role's button names |
| `from` | the stimulus whose onset starts RT: `yrsp_set_landing()` on its channel, then its flip record (`yrsp_update_frame()`), or its audio onset |
| `duration_s` | `duration_ns`, from the `from` onset; 0 means no deadline |
| `minimum_valid_rt_s` | `min_rt_ns` |
| `key_answer` | `$correct`: the response's key equals this value |
| `response_ends_trial` | true (default): `after` starts at the response; false: at the window's end. The player sets `yrsp_desc.persist` from it |
| `max_responses`, `allow_held_key`, `wait_for_key_release` | the `yrsp_desc` fields |
| `trigger` | `{"role": R, "code": VALUE}`: a response rule on the role's output (`ydev_rule`; G8, closed): `yrsp_rule_choices()` with a code per choice, the rule in the response role's `desc.rule`, `yrsp_rule_arm()` and `yrsp_rule_window()` each trial, `yrsp_rule_sent()` before `yrsp_finish()`: `rsp_flags` has `YRSP_R_SENT_MISMATCH` (0x400) when a code went out that the final response does not match (an anticipation after the final onset, a late press, another choice, a press the collector refused) or a response has no code. A keyboard role has no reader thread of ysp/device.h: `ydev_out_pulse()` when the frame loop sees the response |

### 2.7 Stimuli

| `kind` | Lowers to | Resource keys |
|---|---|---|
| `shape`, `grating`, `gabor`, `noise` | `ygfx_shape()`, `ygfx_grating()`, `ygfx_gabor()`, `ygfx_noise()` | none |
| `image` | `ygfx_image()` | `texture`: a TEXTURE entry, or a variable whose levels are entries |
| `text` | `ygfx_crun()` per run of the block whose key `ypak_runs_key()` gives for the stimulus's style and text, in the experiment's GLYPHRUNS entry (D14; pack.md 4.5) | `font`: a FONT entry; `text`; `size`; `width`; `line_height`, `align`, `dir`, `lang` as in a glyph-run block. Each is a literal, not a variable: the pack tool lays the text out |
| `artwork` | `ygfx_crun()` over an ARTWORK entry | `artwork` |
| `rdk` | `yrdk_open()` and `ygfx_dots()`; `yrdk_start(&f, $trial_seed, 0)` on its first visible frame, `yrdk_update(&f, onset - first onset)` each frame | none |
| `sound` | `yau_wav_load()`, `yau_play_at()` | `audio`: an AUDIO entry |
| `video` | `yvid_*` on a VIDEO entry | `video` |

Every other key is a property: a name from the kind's parameter table
(`ygfx_params()`, `yrdk_params()`, ...) in the table's unit, or a desc
field (`shape`, `edge`, `aperture`, `place`, `signal`, `noise`, in lower
case; gfx's are the rows of `ygfx_enum_params()` that read the kind, and
their values are the names in the row's `values`). `color` is `[r, g, b]` for `color_r`, `color_g`, `color_b`.
`layer` sets the draw order (D3). A property that references a variable
is set when its variable is known: at trial start (as the C programs
set `gabor.contrast` and `rdk.coherence` before the first frame), or
when `after` is built. An `rdk` stimulus records `<name>_seed`,
`<name>_updates` and `<name>_digest` (`yrdk_digest()`) in the row and its
steps in `streams/<name>.csv`, so `yrdk_replay()` regenerates every dot.

### 2.8 Methods

| Key | Value |
|---|---|
| `type` | `"stair"`, `"quest"`, `"aep"` |
| other keys | the fields of `yst_desc`, `yqst_desc` or `yaep_desc`, as the rows of `yst_params()`, `yqst_params()` and `yaep_params()` name them; enums in lower case without the prefix, as the rows' values list them (`"step_type": "log"`, `"pf": "gumbel"`); a `yqst_fixed(v)` grid is the number v |
| `scale` | `"linear"` (default) or `"log10"`: the method works in log10 of the stimulus value; `$level` and `method_value` are in stimulus units, and the player converts both ways. Grids and bounds are in the method's domain; a `gamut` value in them is converted to it |
| `dims` | `aep` only: names for the dimensions, which become variables |
| `estimate` | which estimate fills `$estimate` (`"reversals"`, `"trials"`, `"mean"`, `"mode"`) |

A method's stop fields end its track (`yst_done()`, `yqst_done()`).
QUEST+ and AEP may compute the next proposal on the pump
(`YQST_ASYNC`, `YAEP_ASYNC`); that is the player's choice, not the
definition's. A resume restores the trials with `ytr_open_saved()`,
and each method with its saved state: `yst_open_saved()`,
`yqst_open_saved()`, `yaep_open_saved()`.

### 2.9 Devices

A role has the keys of devices_spec 11.1: `direction`, `family` (an
array; the first that the rig has binds), `required`, `min_tier`,
`buttons`, `pulse_s` (seconds), `kind`, `map`. `ydev_params(family, &n)`
lists the desc fields each family reads, with its defaults, for the
role's keys and the profile's options. The player binds roles from
the rig profile (`yrig_load()`, `yrig_open_device()`) and refuses to
start when a required role is unbound, fails identify, or is below
`min_tier`, naming the role. An output role becomes a screen trigger
channel (`ydev_trigger_channel()` in `yscr_desc.triggers`), in the
order of the role names.

### 2.10 Data

| File | Content | Written by |
|---|---|---|
| `session.json` | format `"ysp-session"` 1; experiment ID (stored and migrated, D12), pack ID, rig profile hash (`yrig_hash()`), device rows (devices_spec 11.4), describe lines of the screen, color context and audio, session fields, the seed, the player's and each header's version, start date (UTC), applied live changes, `closed` | the player, at start and at close (`yds_open()`, `yds_session_push()`, `yds_finish()`) |
| `trials.csv` | one row per trial (below) | `ytr_data_open()` with `dd.trials` NULL and the player's columns, since a tree has several `ytr_trials` |
| `frames.csv` | one row per `yscr_record`: frame (the record's index; the name joins `events.csv`'s `frame`), trial_index, target_s, planned_s, onset_s, residual_s, dropped, path, tier, flags, code_risk, phase durations (`_s`) | the player, from `f.done` (`yds_frame_from_screen()`) |
| `events.csv` | timeline landings (kind, stimulus, planned_s, frame, onset as t_s, residual_s), responses, device events, outputs (`YDEV_REC_OUT`: code, deadline_s, write start as t_s and end as end_s, flags), marks; one table with a `kind` column (ysp/dataset.h, THE TABLES) | the player, from the event ring |
| `streams/` | one table per stream and per `rdk` stimulus | the player (`yds_stream()`) |
| `clock.csv` | thinned clock pairs and every fit | the player (`yds_clock_from_fit()`) |

Every file is ysp/dataset.h's (gap G13); the trial rows go through
`yds_trials_writer()`, which is the `ytr_data` above.

Times are seconds on the ysp_rt clock (rig_spec 1, principle 1), in
columns whose names end in `_s` (D4; conventions.md, Units); RT is
relative to its onset. A trial row has, in order:

1. Session fields, as constant columns (the trials writer's rule).
2. `node`, `trial_type` (the template), `trial_index`, `time_elapsed_s`,
   and the `ytr_trial_info` fields.
3. Every table column of the row, the `vars`, the jitters (`soa_s`),
   `trial_seed` (`x` and 16 hex digits, from `ytr_format_row()`) and the
   inherited `data`.
4. For each method trial: `method`, `level`, `estimate`.
5. For each stimulus in `record`: `<s>_onset_s`, `<s>_offset_s`,
   `<s>_frames` (frames shown), `<s>_residual_s`, `<s>_tier` (worst),
   `<s>_dropped` (summed `yscr_record.dropped`), `<s>_flags` (OR of
   `YSCR_FLIP_*`). A stimulus shown more than once in a trial: the first
   onset, the last offset, and frames, tier, dropped and flags over every
   frame it was shown (the player, 2026-11-09; D10 did not say).
6. Over the trial's frames: `tier_worst`, `frames_dropped`,
   `flip_flags`.
7. For each output role: `<role>_n`, `<role>_failed`, `<role>_lag_max_s`
   (write start minus deadline).
8. `YRSP_DATA_COLS` (`yrsp_data_set()`), `correct`, `outcome`. With a
   response `trigger`, `rsp_flags` & 0x400 marks a trial whose code and
   final response disagree (2.6).
9. The template's `columns`.

### 2.11 Versions

Version 1 is this page as decided on 2026-10-14. 1.3 D12 gives the
rules. The player's describe line names the newest version it reads.

Note (2026-11-09): this page does not list every default of version 1
that D2 asks for. The defaults table of ysp/experiment.h
(`yexp_defaults()`, [experiment.md](experiment.md), "the format's
defaults") lists them, and the gaps found in this page. That table is
version 1's; a later version may change it.

Note (2026-11-11): the AEP method's keys `hyper.*`, `hyper_min.*` and
`hyper_max.*` are now `hyperparams.*`, `hyperparams_min.*` and
`hyperparams_max.*` (ysp/experiment.h 0.2.0, ysp/aep.h 0.17.0). This
change is inside version 1, and there is no migration: no AEP definition
existed outside this repository. Reason: the format uses the names of the
C fields, and the Windows SDK defines `hyper` as a macro (`__int64`), so a
C file that included `<windows.h>` before ysp/aep.h did not compile.

Version 2 (proposed 2026-11-11) is part 3. 3.2 gives the migration step
from version 1.

### 2.12 What the designer edits

The designer edits the definition and the pack source description; it
never edits the rig profile. Panels come from the parameter tables
(G6). On each change it runs the loader in WebAssembly: the strict read,
`ytr_rules()` and a dry `ytr_open()` per node (an impossible constraint
fails here), `ytl_check_ops()` per template, `yrsp_desc` checks, the
lookups' levels, and every pack entry name. It writes canonical bytes
(D2) and keeps its own view state elsewhere (D13). Live edits go over
the runner protocol, apply between trials, and are logged in
`session.json` with the new experiment ID.

### 2.13 What the web player needs

- The same loader, built with emcc. The experiment entry is read whole;
  other entries by HTTP range (rig_spec 5).
- `web.allowed`. Without it, a web run is refused.
- Only keyboard, mouse and touch roles bind. A required role of another
  family refuses the run; a web-capable definition makes such roles
  optional.
- `deg` units need the size procedure (rig_spec 14.7, rank 10). Until it
  exists, a `deg` definition is refused on the web.
- Session fields from the URL query.
- The data set as a zip, posted over the runner protocol or downloaded:
  ysp/dataset.h writes the folder in Emscripten's in-memory file system
  and `yds_zip()` gives its stored zip to a write function (2026-10-21;
  was: through the trials writer's `desc.write`).
- The self-test values (refresh, clock resolution, cross-origin
  isolation, fullscreen) in `session.json`; every visibility change in
  `events.csv`; tier WEB on every frame.

### 2.14 Worked examples

Each example names the C program it was checked against. "Same" means
the field maps one to one to the call or field in the program; gaps are
named.

#### A 2AFC staircase (`examples/response/trial_2afc_adaptive.c`)

```json
{
  "format": "ysp-experiment", "version": 1, "name": "contrast_2afc",
  "display": { "units": "deg", "background": [0.5, 0.5, 0.5], "calibration": "required", "min_tier": 2 },
  "devices": { "resp": { "direction": "in", "family": ["keyboard"], "required": true } },
  "stimuli": {
    "fix": { "kind": "shape", "shape": "cross", "w": 0.4, "h": 0.4, "shape_p0": 0.06,
             "edge": "cosine", "edge_width": 1, "color": [0.15, 0.15, 0.15], "layer": 1 },
    "gabor": { "kind": "gabor", "sf": 2, "sigma": 0.4,
               "x": { "by": "$side", "map": { "left": -5, "right": 5 } },
               "contrast": { "michelson": "$level" } }
  },
  "methods": {
    "stair": { "type": "stair", "start": 0.2, "n_up": 1, "n_down": 3, "step_type": "log",
               "steps": [0.2, 0.1, 0.05], "min": 0.002, "max": { "gamut": "gabor.contrast" },
               "initial_rule": true, "stop_trials": 60, "estimate": "reversals" },
    "quest": { "type": "quest", "scale": "log10", "pf": "gumbel", "stop_trials": 60, "estimate": "mean",
               "stim": [{ "linspace": [-2.5, { "gamut": "gabor.contrast" }, 51] }],
               "param": [{ "linspace": [-2.5, { "gamut": "gabor.contrast" }, 51] },
                         { "linspace": [1, 7, 13] }, 0.5, 0.02] }
  },
  "trials": {
    "detect": {
      "events": [{ "on": "fix" }, { "wait_s": 0.5 }, { "on": "gabor" }, { "wait_s": 0.2 }, { "off": "gabor" }],
      "response": { "role": "resp", "choices": ["left", "right"], "from": "gabor",
                    "duration_s": 2.5, "key_answer": "$side" },
      "after": [{ "off": "fix" }, { "wait_s": 0.5 }],
      "method_value": "$contrast_shown",
      "columns": { "stimulus": "$side", "contrast_shown": "$contrast_shown" }
    }
  },
  "timeline": [
    { "name": "main", "tracks": ["stair", "quest"], "interleave": "random",
      "vars": { "side": { "choice": ["left", "right"] } }, "trial": "detect" }
  ]
}
```

| Field | In the program |
|---|---|
| `display.units` `deg` | `ppd` is `yrig_ppd()` of the rig profile's `view` (`--rig`, `--rig-dir`); without one, nominal geometry flagged NOMINAL. The definition has no nominal geometry: its degrees need the rig's `view` (D4) |
| `calibration` `required` | `--cal`, else a nominal calibration flagged NOMINAL. The definition refuses instead (the rig's `.yspcal`, devices_spec 11.2) |
| `fix`, `gabor` fields | Same: `ygfx_shape_desc`, `ygfx_gabor_desc` |
| `x` by `$side`; `vars.side` | Same: `ytr_var_choice("side", ...)` and `ytr_var()`, `gabor.x = (side ? 5 : -5) * ppd` (G3, closed in ysp/trials.h v0.6.0) |
| `{"michelson": "$level"}`, `$contrast_shown` | Same: `ycol_michelson()`; `scene` to `gabor.contrast`, `shown` to the methods |
| `methods.stair` | Same `yst_desc` fields; `{"gamut": ...}` is `ycol_michelson()`'s `max` |
| `methods.quest` | Same `yqst_desc`; `scale` `log10` is the program's `pow(10, ...)` and `log10(x)` |
| `tracks`, `interleave` | Same: `ytr_track()`, `YTR_INTERLEAVE_RANDOM` |
| `events`, `after` | Same ops; `after` at `max(bt, stim_end)` as the program's `ytl_at()` |
| `response` | Same `yrsp_desc` (`choices`, `duration_ns`); `from` is `yrsp_set_landing(..., GABOR_ON)` |
| `method_value` | `yst_update(&stair, x, correct)`, `yqst_update_values()` with the shown contrast |
| Row | The program's columns are 2.10 items 4, 8 and 9, and `code_peak` and `code_trough` (`ycol_michelson()`'s `peak[0]` and `trough[0]`), which no variable of the definition names yet |

#### An RDK (`examples/rdk/trial_rdk.c`)

`conditions.pstb` is the program's CSV (`coherence`,
`coherent_direction`, `key_answer`; 8 rows).

```json
{
  "format": "ysp-experiment", "version": 1, "name": "rdk_direction",
  "devices": { "resp": { "direction": "in", "family": ["keyboard"], "required": true } },
  "stimuli": {
    "fix": { "kind": "shape", "shape": "cross", "w": 16, "h": 16, "shape_p0": 3,
             "edge": "cosine", "edge_width": 1, "color": [0.2, 0.2, 0.2] },
    "dots": { "kind": "rdk", "aperture": "circle", "w": 400, "count": 200, "speed": 200,
              "dot_size": 4, "color": [1, 1, 1], "layer": 1,
              "coherence": "$coherence", "direction": "$coherent_direction" }
  },
  "trials": {
    "rdk": {
      "events": [{ "on": "fix" }, { "wait_s": 0.5 }, { "on": "dots" }],
      "response": { "role": "resp", "choices": ["left", "right"], "from": "dots", "duration_s": 1.5,
                    "minimum_valid_rt_s": 0.1, "key_answer": "$key_answer" },
      "after": [{ "off": ["dots", "fix"] }, { "wait_s": 0.8 }],
      "record": ["dots"]
    }
  },
  "timeline": [
    { "name": "main", "timeline_variables": "conditions.pstb",
      "rules": ["order random", "reps 5"], "trial": "rdk" }
  ]
}
```

| Field | In the program |
|---|---|
| `dots` | Same `yrdk_desc` (`w`, `count`, `speed`) and `ygfx_dots_desc` (`dot_size`, `aperture`, `w`, `color`) |
| `coherence`, `direction` | Same: `rdk.coherence`, `rdk.direction` from the row before the first frame |
| `$trial_seed` | `seed * 1000003 + index`: replaced by D8 |
| `rules` | Same: `YTR_ORDER_RANDOM`, `reps` 5 |
| `minimum_valid_rt_s` | Same: `min_rt_ns` |
| `after` | Same: off at the response frame's base time, wait 0.8, MARK |
| Row | `rdk_seed`, `rdk_updates`, `rdk_digest` are `dots_seed`, `dots_updates`, `dots_digest`; `frames_dropped` is `dots_dropped`; the steps file is `streams/dots.csv` |

#### A Stroop task from a CSV (`examples/response/trial_stroop.c`)

`stroop.pstb` is the program's CSV (`word`, `ink`, `congruent`,
`answer`, `weight`; 9 rows). Every text is in the pack's glyph runs
(D14). The definition, the CSV and the pack's source description are
`examples/pack/stroop/`; the description lists no glyph runs, and the
pack tool derives the 9 blocks from the four text stimuli (G9).

```json
{
  "format": "ysp-experiment", "version": 1, "name": "stroop",
  "display": { "background": [0.05, 0.05, 0.05] },
  "devices": { "resp": { "direction": "in", "family": ["keyboard"], "required": true } },
  "stimuli": {
    "fix": { "kind": "shape", "shape": "cross", "w": 24, "h": 24, "shape_p0": 3,
             "edge": "cosine", "edge_width": 1, "color": [0.6, 0.6, 0.6] },
    "word": { "kind": "text", "font": "fonts/NotoSans-Regular.ttf", "size": 72, "width": 1000,
              "text": { "by": "$word", "map": { "red": "RED", "green": "GREEN", "blue": "BLUE" } },
              "color": { "by": "$ink", "map": { "red": [0.7, 0.04, 0.03], "green": [0.04, 0.42, 0.05],
                                                "blue": [0.07, 0.15, 0.85] } } },
    "banner": { "kind": "text", "font": "fonts/NotoSans-Regular.ttf", "size": 30, "width": 1000, "y": -30,
                "color": [0.75, 0.75, 0.75],
                "text": { "by": "$practice", "map": { "true": "Practice. Name the ink color, not the word.",
                                                      "false": "Test. No feedback from here on." } } },
    "keys": { "kind": "text", "font": "fonts/NotoSans-Regular.ttf", "size": 22, "width": 1000, "y": 30,
              "color": [0.75, 0.75, 0.75], "text": "R red     G green     B blue" },
    "feedback": { "kind": "text", "font": "fonts/NotoSans-Regular.ttf", "size": 32, "width": 1000,
                  "color": [0.75, 0.75, 0.75],
                  "text": { "by": "$outcome", "map": { "correct": "Correct", "wrong": "Wrong",
                                                       "timeout": "Too slow" } } }
  },
  "trials": {
    "stroop": {
      "events": [
        { "if": "$first_in_block", "steps": [{ "on": ["banner", "keys"] }, { "wait_s": 1.5 },
                                             { "off": ["banner", "keys"] }] },
        { "on": "fix" }, { "wait_s": 0.5 }, { "off": "fix" }, { "on": "word" }],
      "response": { "role": "resp", "from": "word", "duration_s": 2, "minimum_valid_rt_s": 0.1,
                    "choices": [{ "key": "r", "name": "red" }, { "key": "g", "name": "green" },
                                { "key": "b", "name": "blue" }],
                    "key_answer": "$answer" },
      "after": [
        { "off": "word" },
        { "if": "$practice", "steps": [{ "on": "feedback" }, { "wait_s": 0.6 }, { "off": "feedback" }] },
        { "wait_s": 0.4 }],
      "columns": { "feedback": { "by": "$practice", "map": { "true": "$outcome", "false": "" } } }
    }
  },
  "timeline": [
    { "name": "main", "timeline_variables": "stroop.pstb", "trial": "stroop",
      "rules": ["order constrained", "weight weight", "reps 3", "max_run congruent=1 3", "practice 4"] }
  ]
}
```

| Field | In the program |
|---|---|
| `timeline_variables` | `ytb_open_csv()` on the same text; the pack stores the same PSTB bytes (table.md) |
| `rules` | Same text as the program's rules with `--reps` 3: `weight weight` and `reps 3` give each row weight x 3 trials (G4, closed in ysp/trials.h v0.6.0) |
| `word.text`, `word.color` | The program upper-cases the ink names and copies `ink_rgb[ink]` into the runs; the lookups do both |
| Text stimuli | `ylay_layout()` at setup and `ygfx_crun()` per run: D14, G9 (closed: the pack tool lays out the same 9 texts into `experiment/stroop.ysprun`). The font is a pack FONT entry, not a system path |
| `if: $first_in_block`, `if: $practice` | Same: `ti.first_in_block`, `ti.practice` |
| `key_answer` against the key | Same: `strcmp(keys[r].key, answer)` |
| `feedback` from `$outcome` | Same: correct, wrong, too slow |
| Row | The program writes its CSV by hand with `#` lines, before the trials writer; the definition's row is 2.10, with the same columns |

#### An EEG-trigger trial (`examples/device/trigger_flip.c`)

An oddball: an image, a trigger code from the table at its onset, a
button box response with its own code. `oddball.pstb` has `image` (a
TEXTURE entry name), `type`, `code` and `answer`.

```json
{
  "format": "ysp-experiment", "version": 1, "name": "oddball_eeg",
  "display": { "units": "deg", "background": [0.5, 0.5, 0.5], "min_tier": 1 },
  "devices": {
    "resp": { "direction": "in", "family": ["xid", "keyboard"], "buttons": { "1": "left", "2": "right" },
              "required": true, "min_tier": 2 },
    "trig": { "direction": "out", "family": ["parallel", "triggerbox", "xid"], "pulse_s": 0.002,
              "required": true },
    "light": { "direction": "in", "family": ["line"], "kind": "sync", "required": false }
  },
  "stimuli": {
    "fix": { "kind": "shape", "shape": "cross", "w": 0.4, "h": 0.4, "shape_p0": 0.06,
             "color": [0.15, 0.15, 0.15], "layer": 1 },
    "pic": { "kind": "image", "texture": "$image", "w": 6, "h": 6 }
  },
  "trials": {
    "oddball": {
      "events": [{ "on": "fix" }, { "wait_s": "$soa_s" }, { "on": "pic" },
                 { "trigger": "$code", "role": "trig" }, { "wait_s": 0.1 }, { "off": "pic" }],
      "response": { "role": "resp", "choices": ["left", "right"], "from": "pic", "duration_s": 1,
                    "key_answer": "$answer",
                    "trigger": { "role": "trig", "code": { "by": "$response", "map": { "left": 101, "right": 102 } } } },
      "after": [{ "wait_s": 0.2 }],
      "record": ["pic"]
    }
  },
  "timeline": [
    { "name": "main", "timeline_variables": "oddball.pstb",
      "rules": ["order constrained", "reps 4", "max_run type=target 1", "jitter soa_s uniform 0.8 1.2 rate=display"],
      "trial": "oddball" }
  ]
}
```

| Field | In the C examples |
|---|---|
| `trig` role | `ydev_desc.role = "trig"`, `pulse_ns`; the rig profile gives family and key (`yrig_open_device()`) |
| Trigger step | `ydev_trigger_channel()` in `yscr_desc.triggers`; `yscr_trigger(&scr, 0, code)` between begin and flip writes at the planned vblank plus `onset_offset_ns`. The step names the role; its channel is the TRIGGER op's `ch` and the fired event's `target` (G1, closed in ysp/timeline.h v0.8.0) |
| `<role>_lag_max`, `events.csv` | The program's table from `YDEV_REC_OUT` ring records: deadline, write start, write length, trailing edge |
| `resp` with `buttons` | devices_spec 11.1; device events reach `yrsp_feed_screen()` through the input bridge |
| `response.trigger` | A response rule on `trig` in `resp`'s `desc.rule` (ysp/device.h RESPONSE RULES; G8, closed): the code leaves on the box's reader thread. The example's keys come from SDL, so it sends from the frame loop |
| `light` | `photodiode_check.c`: edges of a line board on the input bridge, recorded in `events.csv` for the analysis to match to flips |
| `rate=display` | `YTR_RATE_DISPLAY`: the player sets `desc.display_rate_num` and `display_rate_den` from the screen after `ytr_rules()` (G11, closed in ysp/trials.h v0.6.0) |
| `min_tier` 1 | `yscr_desc.min_tier`; below it, frames carry `YSCR_FLIP_BELOW_TIER` (D11) |

## Part 3: Reference, version 2

Status: proposed 2026-11-11. Nothing parses version 2. D16 to D20 are
decided; every other rule in this part comes from a proposed decision,
D21 to D41 (1.5), and names it. A version 2 definition is a version 1
definition (part 2) plus the keys of this part: a key that this part does
not name keeps its part 2 meaning, type and default.

### 3.1 What version 2 adds

| Addition | Where | Decision |
|---|---|---|
| `version` 2, and version 1 read under it | file | D12; 3.2 |
| `roles`: signal roles with a type and a unit space | file | D21 |
| `display.units` `"cm"` | file | D23 |
| One coordinate frame for every unit; angles on the screen | everywhere | D22 |
| `controls`: components, per template and shared at the top level | file, template | D24, D25 |
| `signals`: the graph as nodes | template | D26 |
| `states`, `exits`, `flags`, `max_visits`, `until`, `after_s` | template | D16, D20; D35, D36 |
| The value forms `{"signal": ...}` and `{"when": ...}` | steps, columns, a region's center | D17; D27 |
| `freeze_at` | cursor, peak | D18; D24 |
| The step `recenter` | steps | D19 |
| Steps on a control's key | steps | D39 |
| Response choices that are signals | response | D34 |
| Columns from controls; `rsp_role` | template, data | D33 |
| `vars`: `uniform` per node, `walk2d` | node | D29, D30 |
| State rows and role samples in `events.csv`; trace streams | data | D37, D38, D40 |
| A defaults table | 3.17 | D41 |

### 3.2 A version 1 definition under version 2

The loader reads version 1 and version 2, and refuses a higher version
with "this player reads up to 2". It migrates a version 1 definition in
one step (D12):

1. `version` becomes 2.
2. Each stimulus's `visible` key is left out (D41, item 2). The player
   ignores that key in version 1, so the meaning stays the same.
3. Nothing else changes. Every key of part 2 means in version 2 what it
   means in version 1. A version 1 template has `events`, `response` and
   `after`, and no `states`, so it runs as D7 says.

The player records the stored bytes' SHA-256 and the migrated bytes'
SHA-256 (D12). The step's test pins a corpus to its migrated canonical
bytes: the four worked examples of 2.14, `examples/pack/stroop/` and the
loader's corpus (`tests/experiment/`) each give their version 1 bytes
with `"version": 2` and no `visible`.

### 3.3 The file

Keys that version 2 adds to 2.1, or changes:

| Key | Value | Default |
|---|---|---|
| `version` | integer, 1 or 2 | required |
| `roles` | object, signal role name to signal role (3.5) | `{}` |
| `controls` | object, control name to control (3.7), in every template's namespace (D25) | `{}` |
| `display.units` | `"px"`, `"deg"`, `"cm"` | `"px"` |

`devices` keeps its meaning (2.9): its roles are event roles (keys,
buttons, triggers). A name is a key of `devices` or of `roles`, not of
both (D21).

### 3.4 Coordinates and units (D22, D23)

- **One frame.** For every unit the origin is the display's center, x is
  right and y is down: version 1's frame, `ygfx`'s default `place`.
- **`cm`.** One scale factor from the rig profile's `view`: px per cm is
  `width_px` / `width_mm` x 10. It lowers to `ygfx_desc.units`
  `YGFX_CM` (G14). Without a `view` the session does not start, as for
  `deg`.
- **Angles that a designer sets** turn counterclockwise on the screen
  from rightward: `rotation_deg`, `mirror_deg`, and `walk2d`'s `_deg`
  variable. In the y-down frame the loader writes `affine`'s `angle` as
  minus `rotation_deg`. A mirror about the line at A degrees is the
  matrix [[cos 2A, -sin 2A], [-sin 2A, -cos 2A]]; the loader takes the
  cosine and sine of a multiple of 90 degrees as exact (0, 1, -1), so a
  mirror gives exact values and keeps the sign of zero (sig.md, "The
  sign of zero").
- **Signal roles in display units** (`display_px`, `display_cm`,
  `display_deg`) give values in this frame. A value that drives a
  stimulus's `x`, `y`, `w` or `h` is in `display.units`: the loader
  refuses a cursor or a moving target whose role's units are not the
  display's (`display_px` for `px`, `display_cm` for `cm`,
  `display_deg` for `deg`).
- **A control's numbers** (a radius, an onset distance, a speed) are in
  the units of what it tests (`of`); a speed is those units per second.

### 3.5 Signal roles (D21)

| Key | Value | Default | Lowers to |
|---|---|---|---|
| `type` | `"position2d"`, `"position2d_relative"`, `"position3d"`, `"scalar"`, `"gaze"` | required | `ysig_role.type` |
| `units` | `"display_px"`, `"display_cm"`, `"display_deg"`, `"workspace_cm"`, `"device"` | required | `ysig_role.units` |
| `allow_relative` | bool; only with `type` `position2d` | `false` | `ysig_role.allow_relative` |
| `required` | bool | `true` | the player refuses a start with a required role unbound, naming it |
| `note` | string | none | |

Lanes: `position2d`, `position2d_relative` and `gaze` have x and y;
`position3d` has x, y and z; `scalar` has x. `pose` is not a type
(ysp/sig.h has no 7-lane value).

- **Adapters.** The rig supplies one adapter per signal role (signal_spec
  5.5): nodes that read a device and end in the role's value, in the
  role's type and units (`ysig_adapter`). The profile's adapter block is
  not built (G18): it must give, per signal role, the device and, for a
  relative device, its scale per count in the role's units, or for an
  absolute one the map from its coordinates. `--sim` gives each role a
  scripted source in the role's units (3.19).
- **Start and bounds.** A relative role reads [0, 0] at the session's
  start. Its adapter never resets at a trial boundary (D19): the player
  calls `ysig_reset_trial()`, not `ysig_reset()`. A role has no bound
  unless its adapter has one.
- **Checks without a rig** (the loader alone): the names, types and
  units; the lanes of each control's `of`; the units of a value that
  drives a stimulus (3.4); `recenter` only on a `position2d_relative`
  role or a `position2d` role with `allow_relative`.
- **Checks with a rig** (the host's `role()`, at the player's start):
  each signal role has an adapter; `ysig_open()` accepts its type and
  units (it refuses another type or units, and a relative adapter under
  `position2d` without `allow_relative`); its device opens. A `recenter`
  on a role whose adapter is absolute (a tablet) is skipped at run time
  and counted in `session.json` (D19's note).

### 3.6 Templates

| Key | Value | Default | Lowers to |
|---|---|---|---|
| `events` | array of steps, with `until` (D16) | `[]` | 2.5; `until` splits it into states (3.11) |
| `after` | array of steps; only with `events` | `[]` | 2.5 |
| `states` | array of states (3.11); not with `events` or `after` | none | one op table per entered state |
| `exits` | array of shared exits (D20 rule 11); only with `states` | `[]` | each listed state's exits |
| `flags` | array of names | `[]` | the player's booleans; columns (3.15) |
| `controls` | object, control name to control (3.7) | `{}` | `ysig_node[]` |
| `signals` | object, node name to node (3.8) | `{}` | `ysig_node[]` |
| `response` | 2.6 and 3.13 | none | `yrsp_desc` |
| `columns` | column to value, also the forms of 3.15 | `{}` | `ytr_col` entries |
| `record`, `method_value` | as 2.5 | as 2.5 | |
| `outcome` | as 2.5; its default with `states`: 3.11 | as 2.5 | `ytr_update()`'s outcome |

The graph. A template that has controls or signal nodes, or names a
top-level control, has one `ysig_graph`, opened at load from those nodes,
the nodes that the forms of 3.10 add, and the signal roles' adapters. At
a trial's start the player sets each node parameter that a variable gives
(`ysig_set()`, `ysig_set_data()`), then calls `ysig_reset_trial(g, t)`
with the first frame's onset (D19). It feeds the graph every input event
(`ysig_feed()`), drains its hits (`ysig_detected()`) and derived events
(`ysig_derived()`) after each one, and calls `ysig_frame()` once a frame
after `ytl_evaluate()`, with the timeline's values, the base time (3.7,
`moving_target`) and the poll's time (sig.md, "Feed it and read it at
each frame").

### 3.7 Controls (D24)

A control is an object with `"control": KIND`. It can be in a template's
`controls` or in the top-level `controls` (D25). A number or a point in a
control is a literal, a variable or a lookup, set at the trial's start
with `ysig_set()`; an element of a point can be a variable (D27).

What a control gives. Three things, and the designer shows them by these
words:

- a **value**: lanes that follow the input, read by `{"signal": NAME}`,
  a trace and a column;
- a **test**: true or false, read by `{"when": NAME}` and a trace;
- **hits**: times at which something happened, read by an exit's `on`,
  `until`, `from`, `to`, `freeze_at` and a column's `time`.

| Kind | Value | Test | Hits | Acceptance tasks |
|---|---|---|---|---|
| `cursor` | 2 lanes | no | no | both |
| `region` | its test as 1 lane | inside (and slow) | yes | both |
| `distance` | 1 lane | no | no | visuomotor |
| `movement` | no | no | the response's | both |
| `peak` | 1 lane | no | no | both |
| `moving_target` | 2 lanes | no | no | bimanual |
| `trace` | no (it writes a stream) | no | no | both |
| `signal_lost` | its test as 1 lane | no sample for a time | yes | neither (gaze) |

Keys that several kinds share: `of` (a role, a control with a value or a
test, or a signal node: 3.9), `from` and `to` (a name with a time: 3.9),
`freeze_at` (a control with hits), `note`.

A control whose hits an exit names arms at its state's entry (3.11).
Every other control arms at the trial's start.

#### cursor

| Key | Value | Default | Lowers to |
|---|---|---|---|
| `role` | a role of 2 lanes | exactly one of `role`, `x` with `y`, `mean` | `role` |
| `x`, `y` | each `"ROLE.x"` or `"ROLE.y"`: one lane of a role | | `select` per lane, then `join` |
| `mean` | array of 2 to 4 roles of one type | | `mean` |
| `transform` | `"identity"`, `{"rotation_deg": A}`, `{"mirror_deg": A}`, or a lookup that gives one of them | `"identity"` | `affine`: `angle` -A, or `m` (3.4), set per trial |
| `about` | point: the rotation's or the mirror's center | `[0, 0]` | `affine` `pre` and `post` |
| `stimulus` | a stimulus whose `x` and `y` follow the value for the whole trial | none | `bind` to its `x` and `y` channels |
| `freeze_at` | a control with hits: from that hit's sample to the trial's end the value holds (D18) | none | `latch` on the hit's `detect` (`out` hit) |

A step cannot give `x` or `y` of a cursor's `stimulus` another value; to
switch what a stimulus follows, leave out `stimulus` and use `set` steps
(3.10).

#### region

| Key | Value | Default | Lowers to |
|---|---|---|---|
| `of` | a name with a value of 2 or 3 lanes | required | the detect's input |
| `center` | point, or `{"signal": NAME}` of the lanes of `of` (D28) | `[0, 0]` | `detect` `a.x`, `a.y`; a signal: `distance(of, NAME)` and a `detect` on its lane |
| `radius` | number above 0 | required | `detect` `level` |
| `outside` | bool: the test is "outside the circle" | `false` | `detect` `test` `ge`, else `le`; `lane` -1 (the norm) |
| `stay_s` | seconds, 0 or more: how long the test holds for a hit | `0` | `detect` `hold` |
| `speed_below` | number above 0: the test also needs the speed of `of` under it | none | `speed(of)`, a `detect` `le`, `all` of the two tests |
| `speed_lag_s` | seconds above 0 | `0.01` | `speed` `lag` |
| `rate` | `"sample"` or `"frame"` | `"sample"` | `detect` `at` |

- Test: inside (or outside) the circle, and slow with `speed_below`. The
  test does not include `stay_s`.
- Hits: a hit when the test has been true for `stay_s`, counted from the
  arm or later. A test that is already true at the arm counts from the
  arm, also with `stay_s` 0 (D35; `entry` `arm` with a stay, `entry`
  `now` without, G16).
- A hit's value (a column's `hit`): the value of `of` at the hit's
  sample.
- Nodes. For the test: a `detect` with `out` test, `start` reset,
  `repeat` every. For the hits: a `detect` with `out` hit, `start` arm
  (or reset: 3.7's arming rule), `repeat` once. With `speed_below`, the
  two tests feed `all`, and the hit `detect` (`ge` 0.5, `hold`
  `stay_s`) reads `all`. The loader makes only the nodes that something
  reads.

#### distance

| Key | Value | Default | Lowers to |
|---|---|---|---|
| `of` | a name with a value of 2 or 3 lanes | required | `distance` input |
| `center` | point | `[0, 0]` | `distance` `point` |
| `scale` | number | `1` | `scale` |

Value: 1 lane, the distance times `scale` (`trial_visuomotor`'s ring has
a diameter of twice the distance).

#### movement

| Key | Value | Default | Lowers to |
|---|---|---|---|
| `of` | a name with a value of 2 lanes | required | `emit` (kind `YIN_KIND_USER + 7`) |
| `onset` | number above 0: the distance from `origin` | required | the collector's choice: `cross` DISTANCE, `level` |
| `from` | a name with a time: the window opens there | the trial's first frame | `yrsp_arm(c, t)` |
| `origin` | point | the value of `of` at the arm | `yrsp_set_origin(c, i, x, y)` |

A movement is a response choice only (3.13). Its hit is the response:
an exit and a `from` name it as `response`. The loader refuses a
movement that no response names.

#### peak

| Key | Value | Default | Lowers to |
|---|---|---|---|
| `of` | a name with a value | required | |
| `measure` | `"speed"` | required | `speed(of, lag_s)` |
| `lag_s` | seconds above 0 | `0.01` | `speed` `lag` |
| `from` | a name with a time: the peak starts again at each of its times | the trial's start | `peak`'s gate: the hit `detect` of a control, the `visible` channel of a stimulus (`channel`), or a selector channel that a state's entry sets |
| `freeze_at` | a control with hits | none | `latch` |

Value: 1 lane, the largest speed since `from` (since the trial's start
with no `from`).

#### moving_target

| Key | Value | Default | Lowers to |
|---|---|---|---|
| `stimulus` | a stimulus whose `x` and `y` follow the value | none | `bind` |
| `x`, `y` | `{"amplitudes": [...], "freqs_hz": [...], "phases": [...]}`: arrays of one length n, 1 to 64, the same for both lanes; phases in radians, a literal array or an array variable | required | `sines` `n` and `data` (lanes x n x 3) |
| `amplitude` | a factor on both lanes; a step can set or tween it (D39) | `1` | `scale` by a `channel` |
| `from` | a state: its entry frame's onset is time 0 | the trial's first frame | the frame's base time on a second base anchored there (D39) |

Value: 2 lanes, `amplitude` x sum of a cos(2 pi f t + phase), at each
frame's base time t from `from`. Before `from`'s entry, t is 0.

#### trace

| Key | Value | Default | Lowers to |
|---|---|---|---|
| `of` | array of names: roles, controls with a value or a test, signal nodes | required | |
| `rate` | `"sample"` or `"frame"` | `"sample"` | 3.15 |
| `from` | a name with a time: rows from it | the trial's first frame | |
| `to` | a name with a time, or `"end"`: rows up to it | `"end"` | |

- Rows kept: from the latest time of `from`; up to `to`. A row at a
  control's hit is in; a row at a state's entry frame or at the end
  frame is out (D38).
- `rate` `"sample"`: one `join` of the items and one `record` `per`
  sample: a row for each sample that changes the join. At most 4 lanes
  in all (`join`).
- `rate` `"frame"`: one row a frame, each item read at the frame's flip
  record's onset with `ysig_value_at()`, written when the record
  arrives. The loader gives each item's node a history of 4096 values
  (`trial_bimanual`'s ring). No lane limit but the data writer's 256
  columns.
- The stream is `streams/<trace name>.csv` (3.15).

#### signal_lost (the gaze task; not needed by the acceptance tasks)

| Key | Value | Default | Lowers to |
|---|---|---|---|
| `of` | a role | required | `silent` input |
| `after_s` | seconds above 0 | required | `silent` `after` |

Test: no sample for `after_s`. Hits: when the test becomes true (a
`detect` `ge` 0.5 on the `silent` node).

### 3.8 The graph: `signals` (D26)

`signals` is the graph of signal_spec 5 as JSON: node name to node.

| Key | Value | Lowers to |
|---|---|---|
| `op` | an op of `ysig_op_params(YSIG_OP_NONE)`'s values, except `source`, `role`, `channel`, `bind`, `record`, `emit`, and custom ops | `ysig_node.op` |
| `in` | array of 1 to 4 names: roles, controls with a value or a test, signal nodes; no lane suffix (use `select`) | `ysig_node.in[]` |
| a row's name | a row of `ysig_op_params(op)`, with its dot (`"gain.x"`, `"m.xy"`); a row whose unit is `s` is the key `<row>_s` (`"lag_s"`, `"hold_s"`, `"after_s"`, `"delay_s"`, `"ramp_s"`, D4); an `enum` row takes a name of its `values` (`"test": "le"`); a literal, a variable or a lookup | `ysig_node.p[]`; `ysig_set()` per trial |
| `lanes` | 1 to 4: `select`'s output, a generator's | `ysig_node.lanes` |
| `data` | `sines` only: an array of lanes x n x 3 numbers (amplitude, Hz, phase in radians) | `ysig_node.data` |

- The loader sets a `detect` node's `start`, `repeat` and `out` by its
  use, as for a region (3.7); those three keys are refused. It also sets
  `history` (D38).
- Order: an object has no order (D3). The loader sorts the nodes so that
  each input comes before its user, and refuses a cycle, naming its
  nodes. `ysig_check()` then checks the lanes and the rows.
- Names: a signal node is in the template's namespace (3.9). A control's
  `of` and `center`, a value form, an exit's `on` (a `detect` node), a
  trace's `of` and a column can name a signal node. A signal node's `in`
  can name a control: its value, or a region's test. The nodes of a
  control's expansion have no names in the file.
- What a designer shows: controls as cards in the template's panel. The
  graph view shows each control's expansion (the nodes of 3.7) under the
  control's name, read only, and the signal nodes, which the user can
  edit. "Edit as graph" writes a control's expansion as signal nodes and
  removes the control. The fields come from `ysig_op_params()` (G6).

### 3.9 Names and references

**One namespace per template** (D20 rule 9, completed by D21 and D26):
the stimuli, the top-level controls, the template's controls and signal
nodes, its states, its flags and named clicks, and the signal roles. A
name that two of these share is refused. `end` and `response` are
reserved. An event role (`devices`) is named only by a `role` key. A mark
is a label in `events.csv`, outside the namespace (D41). A flag is read
as `$NAME`, so a flag cannot have a variable's name.

**Lanes.** `NAME.x`, `NAME.y`, `NAME.z`, `NAME.w` name one lane of a
value. A name alone is every lane; a name alone in a one-lane place is
refused when its value has more than one lane.

What each key can name:

| Key | Can name | Means |
|---|---|---|
| `of`, a trace's `of`, `in` | a role, a control with a value or a test, a signal node | its value; a region's test as 0 or 1 |
| `{"signal": N}` | a role, a control with a value, a signal node | its value |
| `when`'s test | a control with a test, a `detect` signal node, or `{"signal": N, "above": L}` | its test |
| an exit's `on`, `until`, `freeze_at` | a control with hits, a `detect` signal node; `on` also `response` | its hit |
| `from`, `to`, a column's `time` and `from` | a stimulus, a control with hits, a state, `response`; `to` also `end` | its time |

The time of a name, in the running trial:

| Name | Time |
|---|---|
| a stimulus | its first onset (the flip record's onset; the plan until the record comes) |
| a control with hits | its latest hit's time |
| a state | its latest entry frame's onset |
| `response` | `yrsp_result.t_response` |
| `end` | the trial's end frame's onset |

A `from` that starts something again (a peak's gate) takes each time of
its name. A `from` that opens a window (a movement, a trace) takes the
latest time before the window's use. A column takes the latest time in
the trial; it is empty when the name has none.

### 3.10 Value forms (D17, D27)

| Form | Value | Allowed in |
|---|---|---|
| `{"signal": N}` | N's value (one lane in a one-lane place) | a `set` step's `props`; a column; a region's `center` |
| `{"when": TEST, "then": V, "else": V}` | V when the test is true, else the other; V is a literal, a variable, a lookup, or a `when` | a `set` step's `props`; a column |
| TEST | the name of a control with a test or of a `detect` node, or `{"signal": N, "above": L}`: N's one-lane value at or above L | inside `when` |

The loader refuses the forms everywhere else: in a stimulus's keys, in
`to` and `then` steps, and in methods, `vars`, `devices`, `roles` and
control keys other than `center`.

Lowering. For each stimulus property P (one float field: `color` is
three, `color_r`, `color_g`, `color_b`) that any step gives a form:

- Let F1 to Fk be the distinct forms that steps give P, by their
  canonical JSON. P gets k selector channels S1 to Sk on the timeline
  and k `mux` nodes: P = mux(S1, F1, mux(S2, F2, ... mux(Sk, Fk,
  channel(C)))), where C is P's own timeline channel (constants, `on`,
  `off`, tweens). One `bind` row writes P (D17).
- A `set` of Fj lands as `YTL_SET_VALUE` ops: Sj to 1, every other S to
  0. A constant step on P (`set`, `to`, `then`, and `on` and `off` for
  `visible`) sets every S to 0 and writes C. A later `set` of P at the
  same time wins, by the timeline's rule for a channel.
- `{"signal": "N.x"}`: N's node, through `select` for one lane.
- `{"when": T, "then": A, "else": B}`: mux(T, A, B). A literal, a
  variable or a lookup in A or B is a timeline channel that the step
  sets when it lands, read by a `channel` node.
- `{"signal": N, "above": L}`: a `detect` `ge` L (`out` test, `start`
  reset, `repeat` every) on N: a value equal to L is above.
- In a column, a form is read at the trial's end frame, after its
  `ysig_frame()`.

### 3.11 States and exits (D16, D20, D35, D36)

A state:

| Key | Value | Default | Lowers to |
|---|---|---|---|
| `name` | a name (3.9) | required | the state rows' `text` (3.15) |
| `steps` | array of steps (2.5, 3.12) | `[]` | one op table at the entry frame: `ytl_at(&q, base time of the entry frame)` |
| `exits` | array of exits | `[]` | the player's exit test |
| `max_visits` | integer, 1 or more | none | a count per trial (D20 rule 12) |
| `over` | `{"to": STATE or "end", "outcome": V}`; only with `max_visits` | none | |

An exit:

| Key | Value | Default | Lowers to |
|---|---|---|---|
| `on` | a name with hits, or `"response"` (3.9) | one of `on`, `after_s`, or neither (instant) | a hit from `ysig_detected()`; the collector's response |
| `after_s` | seconds above 0, from the entry frame's onset | | that time |
| `if` | a boolean variable or `"$flag"`; instant exits only | none | read when the state starts |
| `to` | a state or `"end"` | required | |
| `outcome` | as 2.5's `outcome`; only when `to` is `end` (D36) | none | the trial's outcome |
| `in` | shared exits only: array of states | required in a shared exit | |

D20's rules 1 to 12 hold as written. These rules complete them (D35):

1. **Entry time and entry frame.** The first state's entry time is the
   trial's first frame's onset. A later state's entry time is the time
   of the exit that led to it: a hit's time, an `after_s` time, or the
   time its steps ended. Its entry frame is the first frame whose onset
   is at or after the entry time (D16). Its steps land from the entry
   frame; `after_s` counts from the entry frame's onset (D20 rule 2).
2. **Arms.** A state arms the controls that its exits name at its entry
   time (`ysig_arm(g, node, t)`). A hit before that time does not count
   (D20 rule 3).
3. **When an exit is taken.** The player drains the graph's hits and the
   collector's response after each event it feeds, and after
   `ysig_frame()`. Of the state's pending exits, the earliest time wins,
   then the list order (D20 rule 4). An `after_s` time T is pending once
   a drained hit is at or after T, or a frame's onset is at or after T.
   So the next state's controls arm before the next sample is fed.
4. **A hit that a frame finds** (a `rate` frame test, a hold that a still
   input completes) starts the next state on the same frame: the player
   adds its op table and calls `ytl_evaluate()` again for the frame (an
   equal onset fires only what was added since, ysp/timeline.h
   EVALUATE). An input of the graph that this second evaluate changes (a
   selector, a step on a control's key) reaches the graph at the next
   `ysig_frame()`, a frame later.
5. **When steps end.** At the end of the state's op table's cursor:
   `wait_s`, `then` and `at_s` move it; `to` does not.
6. **`until`.** `{"until": C, "timeout_s": t}` in `events` or in a
   state's steps splits the steps there: the steps before it are one
   state with no exits; the wait is a state with no steps and the exits
   `{"on": C, "to": NEXT}` and, with `timeout_s`, `{"after_s": t, "to":
   "end", "outcome": "timeout"}`; the steps after it are the next state.
   So C arms when the steps before `until` end (D35 (h)).
7. **The end.** A trial ends at an exit or `over` to `end`, or when the
   last state's steps end with no exit taken. The end lands as version
   1's end MARK; the next trial starts on the next frame. A shared exit
   on the response may end it (3.13).

Outcome (D36): the `outcome` column is the `outcome` of the exit or
`over` that reached `end`; else, with a response, version 1's value
(`correct`, `wrong`, `responded`, or `timeout` when no response came
before the trial's end); else `"done"`. `ytr_update()` gets the
template's `outcome` (2.5), whose default with `states` and no
`key_answer` is 1.

### 3.12 Steps

Steps that version 2 adds or changes (2.5 holds for the rest):

| Step | Lowers to |
|---|---|
| `{"until": C, "timeout_s": t}` | 3.11 rule 6 (D16) |
| `{"set": S, "props": {P: FORM}}` | 3.10: `YTL_SET_VALUE` on P's selector channels |
| `{"set": C, "props": {"amplitude": v}}`, `{"to": C, ...}`, `{"then": C, ...}` | the control key's channel: `YTL_SET_VALUE`, `YTL_TO`, `YTL_THEN` (D39; only `moving_target`'s `amplitude`) |
| `{"recenter": R, "to": [x, y]}` | at its landing frame, before `ysig_frame()`, `ysig_recenter(g, role, x, y, poll)` with the frame's poll time (D19). A state whose first step is `recenter` arms its controls at that call's time |
| `{"flag": F, "value": B}` | the player sets F when the step lands; `$F` reads it when a later state's table is built |
| `{"mark": NAME}` | as 2.5; NAME is a label (D41) |

### 3.13 Responses with signal choices (D34)

A response's choices are key or button choices (2.6), or signal choices,
not both.

| Key | With signal choices | Lowers to |
|---|---|---|
| `role` | refused: each movement's `of` gives its input | |
| `choices` | `[{"signal": MOVEMENT}, ...]` | per movement, a `yrsp_choice`: `name` the control's name, `kind` `YRSP_KIND_USER + 7`, `device` its `emit` node's index, `control` `YRSP_AXIS_POSITION`, `cross` `YRSP_CROSS_DISTANCE`, `level` its `onset` |
| `from` | required: a stimulus | `yrsp_set_landing()` on its channel (2.6) |
| the arm | each movement's `from`; movements with different `from` are refused | `yrsp_arm(c, t)` at that time, then `yrsp_set_origin()` for each movement with `origin` |
| `duration_s`, `minimum_valid_rt_s`, `max_responses` | as 2.6 | |
| `response_ends_trial` | `true`: with `events`, as 2.6; with `states`, a shared exit `{"on": "response", "to": "end"}` in every state, after their own exits. `false`: no such exit | `yrsp_desc.persist` is its negation |
| `key_answer`, `trigger`, `allow_held_key`, `wait_for_key_release` | refused | |

The collector's events are the emit nodes' derived events, fed after
each `ysig_feed()` (sig.md, "Feed a movement onset to the response
collector"). `$response` is the movement's name; `$rt_s` is from the
`from` stimulus's onset. The row adds `rsp_role` (3.15). A key response
in a template with `states` arms at the trial's first frame, as in 2.5.
An exit on an early press needs F3 in ysp/response.h (D20 rule 7; D31).

### 3.14 Variables

Version 2 adds two `vars` forms. Both are leaf keys.

| Form | Value | Lowers to |
|---|---|---|
| `{"uniform": [lo, hi], "n": N, "per": "node"}` | N values, uniform in [lo, hi), drawn once per run of the node: a number when N is 1, else an array. `n` 1 to 64, default 1; `per` required, only `"node"` (D29) | the player: stream splitmix64 from mix64((node seed XOR 0xBB67AE8584CAA73B) + (j + 1) x 0x9E3779B97F4A7C15) for the node's j-th such variable, one output per value, lo + (hi - lo) x (output >> 11) x 2^-53 |
| `{"walk2d": {"step": s, "box": [[x0, y0], [x1, y1]], "start": [x, y]}}` | per trial, a point `step` from the previous trial's point, inside the box; the leaf's first trial starts at `start` (default [0, 0]) (D30) | `ytr_var_walk2d()` (G15) |

`walk2d` named NAME defines `$NAME_x`, `$NAME_y`, `$NAME_from_x`,
`$NAME_from_y` and `$NAME_deg` (the direction from the previous point,
counterclockwise on the screen, in (-180, 180]), each a column. The box
must hold `start`, and `step` must be above 0 and at most half the box's
diagonal, so a point inside the box can always take a step.

### 3.15 Data

**Column forms** (D33), in a template's `columns`:

| Form | Value | Lowers to |
|---|---|---|
| `{"hit": "C.x"}` | the value of C's `of` at C's latest hit's sample, one lane; empty with no hit | `ysig_hit.value`; with a signal `center`, `ysig_value_at(of, hit t)` |
| `{"time": N, "from": M}` | N's time minus M's time (3.9), seconds; empty when either has none. The column's name ends in `_s` (D4) | |
| `{"signal": "N.x"}` | N's value at the trial's end frame | `ysig_value_of()` after that frame's `ysig_frame()` |
| `{"when": ...}` | 3.10, at the trial's end frame | |

**`trials.csv`.** Version 2 adds to 2.10's row:

- in item 8, after `YRSP_DATA_COLS`, `rsp_role` for a response with
  signal choices: the signal role whose sample crossed (the derived
  event's `code`, low 16 bits, is a node of that role's adapter);
- item 10: each flag, in the order of `flags`, written as `correct` is.

**`events.csv` state rows** (D37). One row per visit of a state, written
when the visit ends and its entry frame's flip record is in:

| Column | Value |
|---|---|
| `kind` | `"state"` |
| `t_s` | the entry frame's onset (its flip record's) |
| `planned_s`, `residual_s`, `frame` | the entry frame's plan, residual and index |
| `text` | the state's name |
| `end_s` | the exit's time |
| `code` | the exit's index: the state's own exits, then the shared ones, in order; -1 when the steps ended with no exit; -2 for `over` |

The next row of the trial names the state that the exit entered; the
last row's exit went to `end`.

**`streams/<trace>.csv`** (D38). Lane columns are `<item>_<lane>`
(`hand_x`, `cursor_y`), or `<item>` for a one-lane item (`inside`):

| `rate` | Columns |
|---|---|
| `"sample"` | `trial_index`, `t_s` (the row's sample time), the lanes, `tier`, `device` |
| `"frame"` | `trial_index`, `frame`, `planned_s`, `t_s` (the flip record's onset), the lanes |

Two templates' traces of one name must have the same columns.

**Signal roles** (D40). Each source sample that a signal role's adapter
takes is an input row of `events.csv` (`yds_event_from_input()`, `role`
the role's name, `x`, `y`, `value` in device units). `session.json` has,
per signal role, the adapter's name, the device and its identity, the
type, the units, the calibration's hash, and the count of skipped
`recenter` steps.

### 3.16 The loader's refusals

Each refusal names the key and its line and column, as in version 1.

| Area | Refused |
|---|---|
| File | `version` above 2; a name in both `devices` and `roles`; `display.units` `cm` at a start whose rig has no `view` (the player) |
| Roles | an unknown `type` or `units`; `allow_relative` on a type other than `position2d` |
| Templates | `states` with `events` or `after`; `after` or `exits` with `states` missing; a template control with a top-level control's name |
| Names | a clash in the namespace (3.9); `end` or `response` as a name; a flag with a variable's name; a lane that the value does not have; a name alone with more than one lane in a one-lane place |
| Controls | an unknown kind; a key that the kind does not have; a missing required key; `of` naming a trace, a movement or nothing; `role` with `x`, `y` or `mean`, or none of them; a `transform` other than the three forms; `freeze_at` naming a control with no hits; a cursor or moving target whose role's units are not the display's; `radius`, `onset`, `speed_below`, `lag_s` or `after_s` not above 0; `stay_s` below 0; a moving target's lanes of different lengths, or more than 64 components; two generators with a different `from` in one template (D39); a sample trace of more than 4 lanes; a movement that no response names |
| Signals | an unknown or refused op; an unknown row; `start`, `repeat` or `out` on a `detect`; a cycle; whatever `ysig_check()` refuses (lanes, ranges), with its message |
| Value forms | a form outside the places of 3.10; a test naming a name with no test; `above` on a value of more than one lane; a `set` of `x` or `y` of a control's `stimulus` |
| States and exits | a duplicate state; `to` naming no state; an exit with both `on` and `after_s`; `if` on an `on` or `after_s` exit; `after_s` not above 0; `outcome` on an exit whose `to` is not `end`; a shared exit with no `in`, an `in` naming no state, or an instant shared exit; `max_visits` below 1, or one of `max_visits` and `over` without the other; a cycle of instant exits only (D20 rule 6) |
| Steps | `until` naming a name with no hits; `timeout_s` not above 0; `flag` naming no flag, or a `value` that is not a bool; a step on a control's key other than `amplitude`; `recenter` on a role that cannot be relative, or a `to` that is not a point |
| Responses | signal choices with `role`, `key_answer`, `trigger`, `allow_held_key` or `wait_for_key_release`; signal and key choices in one response; a signal choice naming no movement; movements with different `from` |
| Columns | a `time` column whose name does not end in `_s`; `hit` of a name with no hits |
| Variables | `uniform` with `lo` not below `hi`, `n` outside 1 to 64, or `per` other than `"node"`; `walk2d` whose box does not hold `start`, or whose `step` is not above 0 or is above half the box's diagonal |
| Stimuli | `visible` as a key (D41) |

### 3.17 Defaults (D41)

Version 2's keys at their defaults, which the canonical form leaves out
(D2). Version 1's keys keep the defaults of experiment.md's table.

| Object | Key | Default |
|---|---|---|
| file | `roles`, `controls` | `{}` |
| signal role | `type`, `units` | required |
| signal role | `allow_relative` | `false` |
| signal role | `required` | `true` |
| template | `states`, `signals`, `controls` | none, `{}`, `{}` |
| template | `exits`, `flags` | `[]` |
| template | `outcome` with `states` and no `key_answer` | `1` |
| state | `steps`, `exits` | `[]` |
| state | `max_visits`, `over` | none |
| exit | `on`, `after_s`, `if`, `outcome` | none |
| control | `control` | required |
| cursor | `transform`, `about` | `"identity"`, `[0, 0]` |
| cursor | `stimulus`, `freeze_at` | none |
| region | `of`, `radius` | required |
| region | `center`, `outside`, `stay_s` | `[0, 0]`, `false`, `0` |
| region | `speed_below` | none |
| region | `speed_lag_s`, `rate` | `0.01`, `"sample"` |
| distance | `center`, `scale` | `[0, 0]`, `1` |
| movement | `of`, `onset` | required |
| movement | `from`, `origin` | the trial's first frame, the value at the arm |
| peak | `of`, `measure` | required |
| peak | `lag_s` | `0.01` |
| peak | `from`, `freeze_at` | none (the trial's start), none |
| moving_target | `x`, `y` | required |
| moving_target | `amplitude`, `from`, `stimulus` | `1`, none (the trial's first frame), none |
| trace | `of` | required |
| trace | `rate`, `from`, `to` | `"sample"`, none (the trial's first frame), `"end"` |
| signal_lost | `of`, `after_s` | required |
| step `until` | `timeout_s` | none |
| `uniform` | `n` | `1` |
| `walk2d` | `start` | `[0, 0]` |

### 3.18 Worked examples

Each example names the C program that it is checked against, as 2.14
does. These are the acceptance tests of the version 2 loader and of the
player's lowering and `--sim` run. "Same" means that the column has the
same value. A C column that the definition cannot write is a finding,
listed under each table.

#### Visuomotor rotation (`examples/response/trial_visuomotor.c`)

The C program's defaults: a 1024 x 768 window, target radius 0.35 x 768
= 268.8 px, start circle 12 px, the cursor shown within 0.25 x 268.8 =
67.2 px of the start, movement onset at 0.1 x 268.8 = 26.88 px, 0.25 px
per count, schedule 40, 80, 40 at 30 degrees. `targets.pstb` has
`target_deg` and the target's center in this page's frame: `target_x` =
268.8 cos(deg), `target_y` = -268.8 sin(deg), 8 rows, written by the
pack's source. The rig's adapter for `hand` integrates a raw mouse at
0.25 `display_px` per count.

```json
{
  "format": "ysp-experiment", "version": 2, "name": "visuomotor_rotation",
  "display": { "background": [0.3, 0.3, 0.3] },
  "roles": { "hand": { "type": "position2d", "units": "display_px", "allow_relative": true } },
  "stimuli": {
    "start":  { "kind": "shape", "shape": "circle", "w": 24, "h": 24, "color": [0.9, 0.9, 0.9] },
    "target": { "kind": "shape", "shape": "circle", "w": 24, "h": 24, "color": [0.1, 0.8, 0.2],
                "x": "$target_x", "y": "$target_y", "layer": 1 },
    "ring":   { "kind": "shape", "shape": "circle", "w": 10, "h": 10, "stroke": 2, "color": [1, 1, 0.2], "layer": 2 },
    "dot":    { "kind": "shape", "shape": "circle", "w": 8, "h": 8, "color": [1, 1, 0.2], "layer": 3 }
  },
  "trials": {
    "reach": {
      "controls": {
        "cursor":      { "control": "cursor", "role": "hand", "transform": { "rotation_deg": "$rotation_deg" },
                         "freeze_at": "past_target" },
        "near_start":  { "control": "region", "of": "hand", "radius": 67.2, "rate": "frame" },
        "at_start":    { "control": "region", "of": "hand", "radius": 12, "stay_s": 0.5 },
        "past_target": { "control": "region", "of": "hand", "radius": 268.8, "outside": true },
        "aid":         { "control": "distance", "of": "hand", "scale": 2 },
        "moved":       { "control": "movement", "of": "hand", "onset": 26.88, "from": "at_start" },
        "top_speed":   { "control": "peak", "of": "hand", "measure": "speed", "from": "at_start",
                         "freeze_at": "past_target" },
        "path":        { "control": "trace", "of": ["hand", "cursor"], "from": "at_start", "to": "past_target" }
      },
      "states": [
        { "name": "return",
          "steps": [{ "on": "start" },
                    { "set": "dot", "props": { "x": { "signal": "hand.x" }, "y": { "signal": "hand.y" },
                                               "visible": { "when": "near_start", "then": 1, "else": 0 } } },
                    { "set": "ring", "props": { "w": { "signal": "aid" }, "h": { "signal": "aid" },
                                                "visible": { "when": "near_start", "then": 0, "else": 1 } } }],
          "exits": [{ "on": "at_start", "to": "reach" }] },
        { "name": "reach",
          "steps": [{ "on": "target" }, { "off": "ring" },
                    { "set": "dot", "props": { "x": { "signal": "cursor.x" }, "y": { "signal": "cursor.y" },
                                               "visible": { "by": "$feedback", "map": { "online": 1, "none": 0 } } } }],
          "exits": [{ "on": "past_target", "to": "feedback" }] },
        { "name": "feedback", "steps": [{ "wait_s": 0.5 }, { "off": "target" }] }
      ],
      "response": { "choices": [{ "signal": "moved" }], "from": "target", "response_ends_trial": false },
      "columns": {
        "cross_s": { "time": "past_target", "from": "target" },
        "mt_s": { "time": "past_target", "from": "response" },
        "hand_end_x": { "hit": "past_target.x" }, "hand_end_y": { "hit": "past_target.y" },
        "cursor_end_x": { "signal": "cursor.x" }, "cursor_end_y": { "signal": "cursor.y" },
        "peak_speed_px_s": { "signal": "top_speed" }
      }
    }
  },
  "timeline": [
    { "name": "baseline", "timeline_variables": "targets.pstb", "rules": ["order random", "reps 5"],
      "vars": { "rotation_deg": 0, "feedback": "online" }, "trial": "reach" },
    { "name": "rotation", "timeline_variables": "targets.pstb", "rules": ["order random", "reps 10"],
      "vars": { "rotation_deg": 30, "feedback": "online" }, "trial": "reach" },
    { "name": "washout", "timeline_variables": "targets.pstb", "rules": ["order random", "reps 5"],
      "vars": { "rotation_deg": 0, "feedback": "none" }, "trial": "reach" }
  ]
}
```

The C program's `--endpoint-only` is a third `feedback` value: add
`"endpoint": 0` to the map and a `set` of `dot`'s `visible` to 1 in
`feedback`. The lookup by `$feedback` has a key that each leaf does not
reach: the loader warns once per leaf (2.2).

| Field | In the program |
|---|---|
| `hand` role, adapter | Same: `ysig_role` "hand", `position2d`, `display_px`, `allow_relative`; the raw-mouse adapter's integrator at 0.25 px per count (G18 for the profile's block). The program's origin is the window's top-left; this frame's is the center (D22) |
| States `return`, `reach`, `feedback` | The program's phases RETURN (HOLD on the first trial), REACH, FEEDBACK |
| `at_start` | Same: `N_HOLD`, `le` 12 on the norm, `hold` 0.5, armed at the trial's start (the program: at the last trial's end frame, one frame earlier) |
| `past_target` | Same: `N_REACH`, `ge` on the norm, armed at the hold's hit time (D35) |
| `cursor`, `freeze_at` | Same `affine` (`angle` -30 about the start); the program freezes by copying the cursor at the crossing, the definition with `latch` (D18): the same sample |
| `dot`, `ring` from `near_start` and `aid` | Same as `place_cursor()`: the hand within 67.2 px, else the ring of diameter 2 d; in REACH the cursor, shown with online feedback; in FEEDBACK the frozen point |
| `moved` and the response | Same: `emit` on the hand, a DISTANCE choice of 26.88 px from the hand at the hold's hit, `persist`, the target's landing |
| `top_speed` | Speed over 10 ms, as `measure_reach()`, and its peak from the hold's end to the crossing |
| `path` | The `--traj` rows: every hand sample from the hold's hit to the crossing, both included |
| Timeline | Same blocks and rotations; the order differs (3.19, item 2) |

The data columns (`--out`, then `--traj`):

| C column | Definition | Note |
|---|---|---|
| `trial_index` | `trial_index` | Same |
| `block` | `node` | The node's name |
| `cycle` | none | Analysis: `rep` plus 0, 5 or 15 by `node` |
| `target_deg`, `rotation_deg`, `feedback` | table column, `vars` | Same |
| `rt_s` | `rt_s` | Same when `n_early` is 0; the program leaves it empty after any early crossing, the collector gives a later valid crossing |
| `mt_s`, `cross_s` | `mt_s`, `cross_s` | Same |
| `hand_third_deg`, `hand_pv_deg` | none | Analysis on `streams/path.csv` |
| `hand_end_deg`, `cursor_end_deg` | `hand_end_x`, `hand_end_y`, `cursor_end_x`, `cursor_end_y` | Analysis: atan2(-y, x) minus `target_deg` |
| `peak_speed_px_s` | `peak_speed_px_s` | Same rule; the program's lag sample is one of its trace rows, the graph's can be before the hold's end, so they differ only when the peak is in the first 10 ms |
| `n_samples` | none | Analysis: the rows of `streams/path.csv` per trial |
| `input` | none | `session.json`, the role's adapter (D40) |
| `onset_tier`, `onset_src`, `rsp_flags` | same names | Same (`rsp_flags` bits 0 to 14) |
| `move_tier` | `rsp_tier` | Same |
| `early_move` | none | Analysis: `n_early` above 0 |
| `--traj` `trial_index`, `t_s` | `streams/path.csv` `trial_index`, `t_s` | `t_s` minus the row's `target_onset_s` |
| `--traj` `hand_x`, `hand_y`, `cursor_x`, `cursor_y` | `hand_x`, `hand_y`, `cursor_x`, `cursor_y` | Plus 512 and 384 (the window's center) |

Findings. No C column lacks a source: `cycle`, `n_samples`, the angles
and `early_move` are arithmetic on columns and streams, and `input` is
in `session.json`. The program's levels are floats (268.8f, 26.88f), the
definition's doubles, so a norm between the two decides differently.
When a frame finds the hold's hit (a still hand), the `reach` state's
`set` of `dot` reaches the graph a frame later (3.11 rule 4): for one
frame the dot shows the hand, not the cursor; the data do not change.

#### Two-mouse tracking and reaching (`examples/response/trial_bimanual.c`)

The C program's defaults: `--mapping split`, `--perturb rotation` at -90
degrees, blocks `T,R,T*,R*,T*,R*,T*,R*,T*,T`, 2 tracking trials and 30
reaches a block, 46 s of tracking, 0.25 px per count at 32 px per cm.
The rig's adapters for `left` and `right` integrate one raw mouse each at
0.25 / 32 = 0.0078125 `display_cm` per count, an exact binary fraction,
as the program's `affine` gives; its `view` is 1024 px over 320 mm.

```json
{
  "format": "ysp-experiment", "version": 2, "name": "bimanual_tracking",
  "display": { "units": "cm", "background": [0.05, 0.05, 0.05] },
  "roles": {
    "left":  { "type": "position2d_relative", "units": "display_cm" },
    "right": { "type": "position2d_relative", "units": "display_cm" }
  },
  "stimuli": {
    "track_target": { "kind": "shape", "shape": "circle", "w": 1.6, "h": 1.6, "color": [0.35, 0.35, 0.35] },
    "start_circle": { "kind": "shape", "shape": "circle", "w": 2, "h": 2, "color": [0.35, 0.35, 0.35],
                      "x": "$goal_from_x", "y": "$goal_from_y" },
    "reach_target": { "kind": "shape", "shape": "circle", "w": 2, "h": 2, "x": "$goal_x", "y": "$goal_y", "layer": 1 },
    "dot":          { "kind": "shape", "shape": "circle", "w": 0.5, "h": 0.5, "color": [1, 1, 1], "layer": 2 }
  },
  "controls": {
    "cursor": { "control": "cursor", "x": "left.x", "y": "right.y", "stimulus": "dot",
                "transform": { "by": "$perturb", "map": { "none": "identity", "rotation": { "rotation_deg": -90 } } } }
  },
  "trials": {
    "track": {
      "controls": {
        "goal":   { "control": "moving_target", "stimulus": "track_target", "from": "move", "amplitude": 0,
                    "x": { "amplitudes": [2.31, 2.31, 2.31, 1.76, 1.3, 0.97, 0.73],
                           "freqs_hz": [0.1, 0.25, 0.55, 0.85, 1.15, 1.55, 2.05], "phases": "$phase_x" },
                    "y": { "amplitudes": [2.31, 2.31, 2.31, 1.58, 1.03, 0.81, 0.7],
                           "freqs_hz": [0.15, 0.35, 0.65, 0.95, 1.45, 1.85, 2.15], "phases": "$phase_y" } },
        "inside": { "control": "region", "of": "cursor", "center": { "signal": "goal" }, "radius": 0.8, "rate": "frame" },
        "ready":  { "control": "region", "of": "cursor", "radius": 0.8, "speed_below": 6.5, "rate": "frame" },
        "frames": { "control": "trace", "of": ["goal", "cursor", "left", "right", "inside"], "rate": "frame",
                    "from": "move" }
      },
      "states": [
        { "name": "iti",
          "steps": [{ "on": ["track_target", "dot"] },
                    { "set": "track_target", "props": { "color": { "when": "inside", "then": [1, 0.85, 0],
                                                                   "else": [0.35, 0.35, 0.35] } } },
                    { "wait_s": 1 }] },
        { "name": "wait", "exits": [{ "on": "ready", "to": "move" }] },
        { "name": "move",
          "steps": [{ "to": "goal", "props": { "amplitude": 1 }, "duration_s": 5 }, { "wait_s": 46 },
                    { "set": "goal", "props": { "amplitude": 0 } }] }
      ]
    },
    "reach": {
      "controls": {
        "at_start":  { "control": "region", "of": "cursor", "radius": 1, "stay_s": 0.5, "rate": "frame" },
        "moved":     { "control": "movement", "of": "cursor", "onset": 1, "from": "reach_target",
                       "origin": ["$goal_from_x", "$goal_from_y"] },
        "top_speed": { "control": "peak", "of": "cursor", "measure": "speed", "from": "reach_target" },
        "arrived":   { "control": "region", "of": "cursor", "center": ["$goal_x", "$goal_y"], "radius": 1,
                       "stay_s": 1, "speed_below": 6.5, "rate": "frame" },
        "path":      { "control": "trace", "of": ["cursor"], "from": "reach_target", "to": "arrived" }
      },
      "states": [
        { "name": "begin", "exits": [{ "if": "$first_in_block", "to": "hold_start" }, { "to": "reach" }] },
        { "name": "hold_start", "steps": [{ "on": ["start_circle", "dot"] }],
          "exits": [{ "on": "at_start", "to": "reach" }] },
        { "name": "reach",
          "steps": [{ "on": ["start_circle", "reach_target", "dot"] },
                    { "set": "reach_target", "props": { "color": {
                        "when": "arrived",
                        "then": { "when": { "signal": "top_speed", "above": 39 }, "then": [1, 0.85, 0], "else": [0.1, 0.35, 1] },
                        "else": [0.9, 0.9, 0.9] } } }],
          "exits": [{ "on": "arrived", "to": "end" }] }
      ],
      "response": { "choices": [{ "signal": "moved" }], "from": "reach_target", "response_ends_trial": false },
      "columns": {
        "peak_speed_cm_s": { "signal": "top_speed" },
        "fast": { "when": { "signal": "top_speed", "above": 39 }, "then": 1, "else": 0 },
        "arrived_s": { "time": "arrived", "from": "response" }
      }
    }
  },
  "timeline": [
    { "name": "blocks", "data": { "mapping": "split" }, "timeline": [
      { "name": "b0_track", "trial": "track", "rules": ["reps 2"], "vars": { "perturb": "none",
        "phase_x": { "uniform": [-3.141592653589793, 3.141592653589793], "n": 7, "per": "node" },
        "phase_y": { "uniform": [-3.141592653589793, 3.141592653589793], "n": 7, "per": "node" } } },
      { "name": "b1_reach", "trial": "reach", "rules": ["reps 30"], "vars": { "perturb": "none",
        "goal": { "walk2d": { "step": 12, "box": [[-10, -10], [10, 10]] } } } },
      { "name": "b2_track", "trial": "track", "rules": ["reps 2"], "vars": { "perturb": "rotation",
        "phase_x": { "uniform": [-3.141592653589793, 3.141592653589793], "n": 7, "per": "node" },
        "phase_y": { "uniform": [-3.141592653589793, 3.141592653589793], "n": 7, "per": "node" } } },
      { "name": "b3_reach", "trial": "reach", "rules": ["reps 30"], "vars": { "perturb": "rotation",
        "goal": { "walk2d": { "step": 12, "box": [[-10, -10], [10, 10]] } } } },
      { "name": "b4_track", "trial": "track", "rules": ["reps 2"], "vars": { "perturb": "rotation",
        "phase_x": { "uniform": [-3.141592653589793, 3.141592653589793], "n": 7, "per": "node" },
        "phase_y": { "uniform": [-3.141592653589793, 3.141592653589793], "n": 7, "per": "node" } } },
      { "name": "b5_reach", "trial": "reach", "rules": ["reps 30"], "vars": { "perturb": "rotation",
        "goal": { "walk2d": { "step": 12, "box": [[-10, -10], [10, 10]] } } } },
      { "name": "b6_track", "trial": "track", "rules": ["reps 2"], "vars": { "perturb": "rotation",
        "phase_x": { "uniform": [-3.141592653589793, 3.141592653589793], "n": 7, "per": "node" },
        "phase_y": { "uniform": [-3.141592653589793, 3.141592653589793], "n": 7, "per": "node" } } },
      { "name": "b7_reach", "trial": "reach", "rules": ["reps 30"], "vars": { "perturb": "rotation",
        "goal": { "walk2d": { "step": 12, "box": [[-10, -10], [10, 10]] } } } },
      { "name": "b8_track", "trial": "track", "rules": ["reps 2"], "vars": { "perturb": "rotation",
        "phase_x": { "uniform": [-3.141592653589793, 3.141592653589793], "n": 7, "per": "node" },
        "phase_y": { "uniform": [-3.141592653589793, 3.141592653589793], "n": 7, "per": "node" } } },
      { "name": "b9_track", "trial": "track", "rules": ["reps 2"], "vars": { "perturb": "none",
        "phase_x": { "uniform": [-3.141592653589793, 3.141592653589793], "n": 7, "per": "node" },
        "phase_y": { "uniform": [-3.141592653589793, 3.141592653589793], "n": 7, "per": "node" } } }
    ] }
  ]
}
```

`--perturb mirror` is `"mirror": { "mirror_deg": 45 }` in the cursor's
map: the program's swap of x and y in its y-up frame. `--mapping mean`
is `"mean": ["left", "right"]` in place of `x` and `y`.

| Field | In the program |
|---|---|
| `left`, `right`, adapters | Same counts: `B_SRC_L`, `B_SRC_R` and the integrators; the program's `affine` to cm (1 / 32, y up) is the adapter's scale (y down, D22) |
| `cursor` | Same: `select` x of L and y of R, `join`, then `affine` by block (`set_graph_block()`); `rotation_deg` -90 is the program's `angle` -90 in y up (D22) |
| `goal` | Same: `sines` of 7 components per lane times the channel `AMP`, tweened 0 to 1 over 5 s from the first moving frame; the program draws the phases per block from its own generator, the definition per node (D29) |
| `iti`, `wait`, `ready` | `S_T_WAIT`: the cursor within 0.8 cm of the center and slower than 6.5 cm/s, tested once a frame, at least 1 s after the last tracking trial's end |
| `inside` and the target's color | Same test, once a frame: the cursor within 0.8 cm of the target |
| `move` | `S_T_MOVE`: the ramp, then 46 s, then `END_OF_TRACK` |
| `frames` | The `--track` rows, the values at each moving frame's flip record's onset (`finalize()`) |
| `begin`, `hold_start`, `at_start` | `S_R_START`: the first reach of a block waits 0.5 s in the start circle, tested once a frame |
| `goal` (`walk2d`) | `new_target()`: 12 cm from the last target, inside 20 x 20 cm |
| `moved` and the response | Same: `emit` on the cursor, a DISTANCE choice of 1 cm from the start's center (`yrsp_set_origin()`), armed at the target's frame |
| `top_speed` | `peak_run`: the largest `B_SPEED` (10 ms) since the target appeared |
| `arrived` and the color | `S_R_REACH`: within 1 cm of the target and slower than 6.5 cm/s for 1 s, once a frame; white until the hold starts, then yellow above 39 cm/s, else blue |

The data columns (`--reaches-out`, `--out`, `--track`; y columns of the
definition are y down, D22):

| C column | Definition | Note |
|---|---|---|
| `reach_index` | none | Analysis: the rank of the row among `trial_type` `reach` |
| `block` | `node` | `blocks/b1_reach` for block 1 |
| `block_reach`, `block_trial` | `index` | Same |
| `perturbation` | `perturb` | Same |
| `mapping` | `mapping` | Same (`data`) |
| `start_x_cm`, `start_y_cm` | `goal_from_x`, `goal_from_y` | y with a sign change |
| `target_x_cm`, `target_y_cm`, `target_deg` | `goal_x`, `goal_y`, `goal_deg` | y with a sign change; the draws differ (3.19, item 2) |
| `rt_s`, `onset_tier`, `onset_src`, `rsp_flags` | same names | Same |
| `move_tier` | `rsp_tier` | Same |
| `move_hand` | `rsp_role` | `left` and `right` for L and R |
| `init_err_deg` | none | Analysis on `streams/path.csv`: the program reads the collector's trace, the same cursor samples |
| `peak_speed_cm_s` | `peak_speed_cm_s` | The program reads it at the last hold's start, the definition at the trial's end; they agree while the hold's speed stays under the peak |
| `fast` | `fast` | `above` is at or above 39; the program's is above |
| `mt_s` | `arrived_s` | Analysis: `arrived_s` minus 1 (`stay_s`): the program measures to the hold's start |
| tracking `trial_index` | none | Analysis: the rank of the row among `trial_type` `track` (the definition's `trial_index` is the session's) |
| `mse_cm2` to `r_theta_deg` | none | Analysis on `streams/frames.csv` |
| `n_frames`, `n_missed` | none | Analysis: the rows of `streams/frames.csv`, joined to `frames.csv` for `dropped` |
| `n_norecord` | none | Cannot be written: the player waits for every flip record (player.md) |
| `n_other` | none | Cannot be written: an adapter takes only its own device, and nothing counts the others |
| tracking `onset_tier`, `onset_src` | none | Analysis: the `move` state row's `frame` (3.15), joined to `frames.csv` |
| `--track` `t_s` | `t_s` | Minus the first row's `t_s` of the trial |
| `--track` `target_x`, `target_y`, `cursor_x`, `cursor_y`, `l_x`, `l_y`, `r_x`, `r_y`, `inside` | `goal_x`, `goal_y`, `cursor_x`, `cursor_y`, `left_x`, `left_y`, `right_x`, `right_y`, `inside` | y with a sign change |
| `--freq` | none | Analysis on `streams/frames.csv` |

Findings:

- Two C columns cannot be written: `n_norecord` and `n_other`. The rest
  that have no column are arithmetic on columns and streams.
- The program tests "still" with the cursor's change from one frame's
  onset to the next; the definition's `speed_below` uses ysp/sig.h's
  `speed` over 10 ms. Frames near 6.5 cm/s can decide differently, and
  then the tracking start, the reach's end and `mt_s` differ. Proposal:
  the program moves to the graph's `speed`, as `trial_visuomotor` moved
  its hold to a `detect`; no header changes.
- The program waits 1 s from the last tracking trial's end, and not after
  a reach block; the definition waits 1 s from each tracking trial's
  start, one frame after the last one's end.
- The next reach target comes one frame later than in the program: a
  trial starts on the frame after the last trial's end frame (3.11 rule
  7).
- At a block's first trial the cursor's new `transform` reaches the held
  cursor at the hand's next report (G17); the program reads its frame
  cursor with `ysig_value_at()`, which applies the new block's map. With
  a hand that does not move, `ready` and `inside` can read the old map.

### 3.19 What the acceptance runs need besides the format

1. **A scripted participant for signal roles.** `--participant` takes key
   presses only (player.md). The runs need each role's samples: a file
   of `t_s, role, x, y` in device units, which the C program writes from
   its own model with a test-only option, so both read the same samples.
2. **The same order and draws.** The definition draws its target order,
   its phases and its walk from D8's streams; the C programs draw from
   their own generators. As for the RDK (player.md, Results:
   `--trial-seeds`), each C program needs a test-only option that takes
   the player's order and draws, or the comparison sorts rows by block
   and target.
3. **A test rig profile with adapters**: `view` 1024 px over 320 mm,
   and the scales of 3.18 (G18, or a `--sim`-only adapter).
4. **The header changes**: G14 (`YGFX_CM`), G15 (`walk2d`) and G16
   (`entry` `now`) for `trial_bimanual`; G17 removes a difference at
   block changes. `trial_visuomotor` needs none of them.
