# The experiment definition

Status: design decided 2026-10-14 (proposed 2026-10-13). Nothing is
built. This page designs the file that the player interprets (rig_spec
6): the EXPERIMENT entry of a pack. Each decision gives the options, the
evidence and the choice. The user decided D1 to D15 on 2026-10-14; D4
changed from the proposal. Part 1 is
explanation; part 2 is reference, with four worked examples checked
against the C examples. Where this page and rig_spec 6 or 14.5 to 14.6
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
  errors in hand edits. ysp/json.h keeps a number's text and writes it
  back unchanged, so `0.50` and `0.5` hash differently (gap G7). The rig
  profile writes seconds as the exact decimal of the nanoseconds.
- Decision: (a). A duration is read with `yjs_fixed(v, 9, &ns)`
  and written as the exact decimal of its nanoseconds. Another real is
  written as the shortest exact decimal of its source text (no exponent,
  no trailing zeros, `-0` as `0`). A key at its default is left out. The
  defaults are the format's, listed in this page per version, not the
  headers': the loader fills every desc field, so a header that changes
  a default does not change an experiment. The experiment ID is the
  SHA-256 of the canonical bytes. The pack tool writes the canonical
  bytes into the pack and records `"op": "canonical"` in the entry's
  derivation, so the entry's SHA-256 in the manifest is the experiment
  ID.
- Prerequisite: gap G7. ysp/json.h has no canonical form for a
  non-integer number other than fixed point, so the experiment hash
  cannot be computed as decided until G7 is closed.

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
  geometry, never from the pack, as the calibration does (gap G2).

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
  variable by default (`save_timeline_variables`). The writer takes at
  most 64 columns (gap G10).
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

**D15. Web runs. DECIDED (2026-10-14).**
- Evidence: rig_spec 6: on a participant's machine timing is measured
  and excluded, never controlled, and the exclusion rule is written
  before data collection. Every web frame is tier WEB.
- Decision: a `web` object: `allowed` (default false) and
  `exclusion`, the rule as data (for example a refresh range and a
  maximum fraction of dropped frames). The player records it and the
  self-test's values; the analysis applies it. `min_tier` does not apply
  to a web run.

### 1.4 Gaps in the C APIs

Found while mapping the four examples (2.14). The size is the change in
the header.

| Id | Gap | Header | Needed by | Size |
|---|---|---|---|---|
| G1 | CLOSED in ysp/timeline.h v0.8.0: a TRIGGER op's `ch` is its trigger channel and the fired event's `target` (`YTL_TRIGGER_CH(ch, code)`, `ytl_trigger_ch()`); `ytl_trigger()` and `YTL_TRIGGER()` give channel 0. Was: `ytl_trigger()` and `YTL_OP_TRIGGER` took a code only; the builder wrote target 0, so an op table could not say which trigger channel (role) fires. | ysp/timeline.h | EEG example, two output roles | Small: TRIGGER takes `ch` as its target |
| G2 | The rig profile has no viewing geometry (`ygfx_view`: `distance_mm`, `width_mm`). The 2AFC program hard-codes `PPD_DISTANCE_MM` and `PPD_MM_PER_PX`. | ysp/rigfile.h | Every `deg` definition | Small |
| G3 | No logged and replayed categorical draw per trial: jitters are durations only, and the 2AFC side comes from the program's own generator. | ysp/trials.h (or player code, D8) | `vars` with `choice` | Small |
| G4 | `weight COLUMN` sets `cond_reps` from the column; nothing multiplies it by `reps`. The Stroop program computes weight x reps in C. | ysp/trials.h | Stroop | Small |
| G5 | No call maps a Michelson contrast in light to ysp/gfx.h's contrast and back to the contrast the output codes show (about 20 lines in trial_2afc_adaptive.c with `ycol_output_code()`). | ysp/color.h | `{"michelson": ...}` | Small |
| G6 | Parameter tables (rig_spec 1, principle 8) exist for audio, color, gfx, outline, rdk, screen and video only. None for stair, quest or aep. Closed for the timeline: `ytl_op_params(kind, &n)` gives the rows of the fields each op kind reads (`YTL_OP_NONE` gives the `op` row, whose enum values name every kind), and `ytl_desc_params()` the desc's plain fields (ysp/timeline.h 0.8.0). Closed for response: `yrsp_params()` (ysp/response.h 0.6.0). Closed for the device families: `ydev_params(family, &n)`, the fields each family reads with its defaults (ysp/device.h 0.4.0). Closed for gfx's desc enums: `ygfx_enum_params()` lists `place`, `anchor`, `shape`, `aperture`, `edge`, `dist` and the rest with their kinds and values, and `ygfx_enum_set()` applies one (ysp/gfx.h 0.16.0). `ygfx_desc_params()` (the gfx's own desc) keeps its enum values in its doc text: `yscr_param` has no `values`. | those headers | The loader's key check and the designer's panels from one source | Medium |
| G7 | The canonical writer writes a number's source text; only `yjs_new_fixed()` and `yjs_new_int()` make canonical numbers. No canonical decimal for other reals. The prerequisite for the experiment hash (D2). | ysp/json.h | D2 | Small |
| G8 | A response-driven output waits for the frame loop to see the response, up to a frame after the press. No callback on the input's thread. | ysp/response.h, ysp/device.h | Response triggers (EEG) | Medium; measure the lag first |
| G9 | Glyph-run blocks are listed by hand in the source description. Nothing derives them from a definition's texts and a table's levels. | pack tool | D14; Stroop | Small to medium |
| G10 | A trial row has at most 64 columns (ysp/table.h's bound, kept by the writer). | ysp/trials.h, ysp/table.h | Templates with many recorded stimuli | Small; `record` limits columns meanwhile |
| G11 | The jitter `rate=` takes a fixed number. Snapping to the rig's refresh needs the player to set `rate_num` and `rate_den` after `ytr_rules()`, and the text cannot say so. | ysp/trials.h | EEG example (`rate=display`) | Small |
| G12 | ysp/stair.h has no save and open-saved (quest, aep and trials have them). A resume replays the staircase's updates meanwhile. | ysp/stair.h | Resume | Small |
| G13 | The player's data set (D9) is not built. The trials writer's sidecar is key=value lines. | player | D9 | Medium |

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
| `{"by": VALUE, "map": {KEY: VALUE, ...}}` | A lookup. KEY is the text of `by`'s value; a bool is `"true"` or `"false"`. The loader checks that `map` has every level that `by` can take (table levels, `choice` lists, outcomes) |
| `{"linspace": [lo, hi, n]}` | n values from lo to hi (`yqst_linspace()`) |
| `{"gamut": "stimulus.property"}` | The largest value of the property inside the display's gamut (`ycol_max_scale()`) |
| `{"michelson": VALUE}` | Only for `contrast`: a Michelson contrast in light through the calibration; defines `$contrast_shown` (G5) |

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
| `trial_seed` | trial start | the trial stream's 64-bit seed (D8) |
| `method`, `level`, `estimate` | trial start (`estimate`: after the update) | the track's name, its proposal in stimulus units, its estimate |
| `response`, `rt_s`, `responded` | `after` | `yrsp_result`: the choice's key, `rt_ns` in seconds, `YRSP_R_RESPONDED` |
| `correct` | `after` | the response's key equals `key_answer` |
| `outcome` | `after` | `"correct"`, `"wrong"` or `"timeout"` |
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
| `vars` | NAME to a value, or `{"choice": [v, ...]}` drawn per trial | the trial stream (D8; G3) |
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
| `outcome` | `"correct"` or a value | `ytr_update()`'s outcome; `YTR_INVALID` when no response |
| `method_value` | value | what the method's update hears; default `$level` |

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
| `response_ends_trial` | true (default): `after` starts at the response; false: at the window's end |
| `persist`, `max_responses`, `allow_held_key`, `wait_for_key_release` | the `yrsp_desc` fields |
| `trigger` | `{"role": R, "code": VALUE}`: `ydev_out_pulse()` when the response is seen (G8) |

### 2.7 Stimuli

| `kind` | Lowers to | Resource keys |
|---|---|---|
| `shape`, `grating`, `gabor`, `noise` | `ygfx_shape()`, `ygfx_grating()`, `ygfx_gabor()`, `ygfx_noise()` | none |
| `image` | `ygfx_image()` | `texture`: a TEXTURE entry, or a variable whose levels are entries |
| `text` | `ygfx_crun()` per run of a GLYPHRUNS block (D14) | `font`: a FONT entry; `text`; `size`; `width` |
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
| other keys | the fields of `yst_desc`, `yqst_desc` or `yaep_desc`; enums in lower case without the prefix (`"step_type": "log"`, `"pf": "gumbel"`); a `yqst_fixed(v)` grid is the number v |
| `scale` | `"linear"` (default) or `"log10"`: the method works in log10 of the stimulus value; `$level` and `method_value` are in stimulus units, and the player converts both ways. Grids and bounds are in the method's domain; a `gamut` value in them is converted to it |
| `dims` | `aep` only: names for the dimensions, which become variables |
| `estimate` | which estimate fills `$estimate` (`"reversals"`, `"trials"`, `"mean"`, `"mode"`) |

A method's stop fields end its track (`yst_done()`, `yqst_done()`).
QUEST+ and AEP may compute the next proposal on the pump
(`YQST_ASYNC`, `YAEP_ASYNC`); that is the player's choice, not the
definition's. A resume restores the trials with `ytr_open_saved()`,
QUEST+ and AEP with their saved states, and a staircase by replaying
its updates (G12).

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
| `session.json` | format `"ysp-session"` 1; experiment ID (stored and migrated, D12), pack ID, rig profile hash (`yrig_hash()`), device rows (devices_spec 11.4), describe lines of the screen, color context and audio, session fields, the seed, the player's and each header's version, start date (UTC), applied live changes, `closed` | the player, at start and at close |
| `trials.csv` | one row per trial (below) | `ytr_data_open()` with `dd.trials` NULL and the player's columns, since a tree has several `ytr_trials` |
| `frames.csv` | one row per `yscr_record`: index, target_s, planned_s, onset_s, residual_s, dropped, path, tier, flags, phase durations (`_s`) | the player, from `f.done` |
| `events.csv` | timeline landings (kind, stimulus, planned_s, frame, onset_s, residual_s), responses, device events, outputs (`YDEV_REC_OUT`: code, deadline_s, write start and end in `_s`, flags), marks | the player, from the event ring |
| `streams/` | one table per stream and per `rdk` stimulus | the player |
| `clock.csv` | thinned clock pairs and every fit | the player |

Times are seconds on the ysp_rt clock (rig_spec 1, principle 1), in
columns whose names end in `_s` (D4; conventions.md, Units); RT is
relative to its onset. A trial row has, in order:

1. Session fields, as constant columns (the trials writer's rule).
2. `node`, `trial_type` (the template), `trial_index`, `time_elapsed_s`,
   and the `ytr_trial_info` fields.
3. Every table column of the row, the `vars`, the jitters (`soa_s`) and the
   inherited `data`.
4. For each method trial: `method`, `level`, `estimate`.
5. For each stimulus in `record`: `<s>_onset_s`, `<s>_offset_s`,
   `<s>_frames` (frames shown), `<s>_residual_s`, `<s>_tier` (worst),
   `<s>_dropped` (summed `yscr_record.dropped`), `<s>_flags` (OR of
   `YSCR_FLIP_*`).
6. Over the trial's frames: `tier_worst`, `frames_dropped`,
   `flip_flags`.
7. For each output role: `<role>_n`, `<role>_failed`, `<role>_lag_max_s`
   (write start minus deadline).
8. `YRSP_DATA_COLS` (`yrsp_data_set()`), `correct`, `outcome`.
9. The template's `columns`.

### 2.11 Versions

Version 1 is this page as decided on 2026-10-14. 1.3 D12 gives the
rules. The player's describe line names the newest version it reads.

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
- The data set as a zip through the trials writer's `desc.write`, posted
  over the runner protocol or downloaded.
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
| `display.units` `deg` | `ppd` from two constants: G2 |
| `calibration` `required` | `--cal`, else a nominal calibration flagged NOMINAL. The definition refuses instead (the rig's `.yspcal`, devices_spec 11.2) |
| `fix`, `gabor` fields | Same: `ygfx_shape_desc`, `ygfx_gabor_desc` |
| `x` by `$side`; `vars.side` | `gabor.x = (side ? 5 : -5) * ppd`, side from `rng_side`: G3 |
| `{"michelson": "$level"}`, `$contrast_shown` | `shown_contrast()`: G5 |
| `methods.stair` | Same `yst_desc` fields; `{"gamut": ...}` is `ycol_max_scale()` / `bg_scale` |
| `methods.quest` | Same `yqst_desc`; `scale` `log10` is the program's `pow(10, ...)` and `log10(x)` |
| `tracks`, `interleave` | Same: `ytr_track()`, `YTR_INTERLEAVE_RANDOM` |
| `events`, `after` | Same ops; `after` at `max(bt, stim_end)` as the program's `ytl_at()` |
| `response` | Same `yrsp_desc` (`choices`, `duration_ns`); `from` is `yrsp_set_landing(..., GABOR_ON)` |
| `method_value` | `yst_update(&stair, x, correct)`, `yqst_update_values()` with the shown contrast |
| Row | The program's columns are 2.10 items 4, 8 and 9; `code_peak` and `code_trough` come with G5 |

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
(D14).

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
      "rules": ["order constrained", "weight weight", "max_run congruent=1 3", "practice 4"] }
  ]
}
```

| Field | In the program |
|---|---|
| `timeline_variables` | `ytb_open_csv()` on the same text; the pack stores the same PSTB bytes (table.md) |
| `rules` | Same text as the program's `rules` plus `weight weight`; the program multiplies the weight by `--reps` 3 in C: G4 |
| `word.text`, `word.color` | The program upper-cases the ink names and copies `ink_rgb[ink]` into the runs; the lookups do both |
| Text stimuli | `ylay_layout()` at setup and `ygfx_crun()` per run: D14, G9. The font is a pack FONT entry, not a system path |
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
| `response.trigger` | `ydev_out_pulse()` when the frame loop sees the response: G8 |
| `light` | `photodiode_check.c`: edges of a line board on the input bridge, recorded in `events.csv` for the analysis to match to flips |
| `rate=display` | G11 |
| `min_tier` 1 | `yscr_desc.min_tier`; below it, frames carry `YSCR_FLIP_BELOW_TIER` (D11) |
