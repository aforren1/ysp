# ysp/trials.h design

Status: **v0.6.0, implemented, 2026-10-21.** v0.6.0 adds variables drawn
per trial from the trial's own stream, `weight` times `reps` in the rules
text, jitters snapped to the display's rate, and 256 columns (see "v0.6:
variables, weight x reps, the display's rate, 256 columns"). v0.5 named
every column of seconds with `_s`.
v0.4.0 adds the data writer:
one CSV row per trial with the session's columns and the caller's, read
back as written by ysp/table.h (see "Save one row per trial", "Data file
reference", "v0.4: the data writer" and "Verification and cost (v0.4)").
v0.3.0 applied the repository conventions (see "Nanosecond jitters, open
and close (v0.3.0)"). The rest of this status is v0.2.1's.

Status of v0.2.1: **implemented, 2026-10-07.** v0.2.1 adds jitter: a
duration drawn per trial, logged and replayed (see "v0.2.1: jitter in the
trial handler" and "Verification and cost (v0.2.1)"). v0.2 adds conditions files
and trial lists as tables (ysp/table.h, docs/table.md), order lists,
draws with replacement, subsets, blocked and alternating groups in
Latin-square order, units ("b always follows a"), first-order transition
balance, Latin squares and a rules text; every v0.1 desc behaves as in
v0.1.1, pinned by a test. "What v0.2 adds", the v0.2 Decisions, "Verification (v0.2)" and
"Cost (v0.2)" below record the design, the measurements behind it and
the verification. v0.1.1 status follows.
Registered in `CMakeLists.txt`, with `tests/adapt/trials_test.c` under
ctest and two examples in CI; gcc and MSVC clean, sanitizer clean. The Python binding
`ysp.trials` and the MEX function `ysp_trials` exist, and
`tests/compare/compare_trials_psychopy.py` checks the binding against
PsychoPy's TrialHandler: identical sequential order, and the block
properties of the random orders over 100 seeds per design. The weighted
sequential order differs by design (PsychoPy runs a row's copies back to
back, this header cycles the rows), and the script reports that and does
not fail on it. This page records why the API looks the way it does and
what the implementation changed; the header says what it does.

## Goals

- The layer above the adaptive methods: which trial next, and what
  happened. The method headers refuse to own the trial loop (that is a
  stated non-goal in [adapt.md](adapt.md)), so every experiment
  writes the same shuffle, interleave and bookkeeping by hand. This header
  is that code, once.
- The method of constant stimuli as the base case, not a feature:
  conditions times repetitions in some order, with tallies per condition.
- Constrained random orders, stated as rules ("no more than three of the
  same orientation in a row", "a catch trial at most once in six"), checked
  at open so an impossible design fails before the session starts.
- Interleaved adaptive tracks with no dependency on the method headers: a
  track is an opaque pointer and an is-done callback.
- The same shape as the rest of the collection: one file, no heap, a
  caller-owned handle, a zeroed desc, `open` with the one message buffer,
  `next` / `update` / `done`, deterministic from the caller's generator, a
  history that replays.

## Non-goals

- Timing, stimuli, I/O. The header never touches a clock or a device.
  `ytr_format_row` formats a line the caller writes. From v0.4 the data
  writer opens, writes and closes one file (or hands its bytes to a
  function of the caller's); nothing else in the header touches a file.
- Fitting. Per-condition proportions are the estimate; the fit is offline.
- (v0.1) Counterbalancing across sessions or subjects, and transition
  balancing within a session. v0.2 reversed both at the user's request:
  Latin and Williams squares by participant number, and exact first-order
  transition balance by an Euler circuit rather than a scored repair (see
  Decisions).
- Multi-session experiment structure (PsychoPy's ExperimentHandler). One
  handle is one session; the caller strings sessions together.

## Prior art

| Feature | PsychoPy | Here |
|---|---|---|
| Conditions x reps, sequential / random / fullRandom | `TrialHandler(trialList, nReps, method)` | `ytr_desc` with the same three orders, the same block semantics (random shuffles within each repetition) |
| Weighted conditions | `TrialHandlerExt` weights | `cond_reps[]` |
| Interleaved adaptive procedures | `MultiStairHandler(method='random'/'sequential')` | tracks with random-by-weight or round-robin interleave, plus `track_rate` to mix them with scheduled trials |
| Constraints on the order | none; users post-process `sequence` or roll their own | five rules, repaired at open, unsatisfiable sets fail at open |
| Catch trials | a condition with its own weight; spacing by hand | a condition placed by `ytr_min_gap` or `ytr_max_in_window` |
| Re-running missed trials | none built in (users append to `trialList`) | `ytr_requeue` with an optional gap |
| Practice | a separate handler | `n_practice`, flagged, untallied |
| Data | `addData`, `nextEntry`, `saveAsWideText` | a fixed-size caller record per trial, `ytr_format_row`, the history array; from v0.4 `ytr_data_set_*` and `ytr_data_write` (columns declared at open, a row written per trial) |
| Resume | `saveAsPickle` | `ytr_save` / `ytr_open_saved` byte snapshot, or `ytr_restore` from the outcomes plus the seed (a re-queue is its own outcome value so replay can tell it from an invalid response) |

Psychtoolbox has no equivalent; labs write their own. Palamedes has none.

## Save one row per trial

This procedure writes a CSV data file with one row per trial: the trial
handler's columns, then your own. `examples/trials/save_rows.c` is a
complete program that does it.

1. Put the values that you record per trial in a struct. Keep times as
   `int64_t` nanoseconds. Use `YTR_MISSING_NS` for a time that does not
   exist, for example the RT of a missed response:

   ```c
   typedef struct row {
       const char* response;   /* NULL: no response */
       bool        correct;
       int64_t     rt_ns;      /* YTR_MISSING_NS: no response */
   } row;
   ```

2. Declare the columns. `YTR_COL` reads a field of the struct, and the
   column gets the field's name. The file holds seconds, so a time
   field's `_ns` becomes `_s` in the file: field `rt_ns` is column
   `rt_s`. Each column of seconds ends in `_s`: the open refuses a
   `YTR_COL_TIME_NS` column named `rt`, and a session jitter named
   `iti` (name it `iti_s`). Give a `YTR_COL_DOUBLE` column of seconds
   the same suffix; the writer cannot see its unit. `YTR_COL_TEXT` writes the same text in
   each row. `YTR_COL_SET` is a column that you set by index for some rows
   only:

   ```c
   const ytr_col cols[] = {
       YTR_COL(row, response, YTR_COL_STRING),
       YTR_COL(row, correct, YTR_COL_BOOL),
       YTR_COL_D(row, rt_ns, YTR_COL_TIME_NS, 6),   /* column rt_s, 6 decimals */
       YTR_COL_TEXT("participant", participant),
       YTR_COL_SET("note", YTR_COL_STRING),
   };
   ```

3. Open the writer after `ytr_open()` and before the first trial. Give the
   session, the columns and a path. A sidecar file (`meta_path`) is
   optional; it records the settings that are the same for the whole
   session, for example the seed and the rig:

   ```c
   static ytr_data w;
   ytr_data_desc dd = { 0 };
   dd.path = "P07.csv";
   dd.trials = &t;
   dd.cols = cols;
   dd.n_cols = 5;
   dd.meta_path = "P07.csv.meta";
   dd.meta = "participant=P07\nseed=20261010\n";
   if (!ytr_data_open(&w, &dd)) { fprintf(stderr, "%s\n", ytr_data_error(&w)); return 1; }
   ```

   If the open fails, the message names the column or the file.

4. In the trial loop, call `ytr_data_write()` after `ytr_update()` and
   before the next `ytr_next()`. Do not call it during the frame loop of a
   trial:

   ```c
   while (ytr_next(&t, &ti) >= 0) {
       row r = run_trial(&ti);
       if (!r.response) ytr_data_set_string(&w, note_col, "no response");
       ytr_update(&t, r.response ? r.correct : YTR_INVALID, NULL);
       if (ytr_data_write(&w, &r) != 0) { fprintf(stderr, "%s\n", ytr_data_error(&w)); break; }
   }
   ```

5. Finish the writer, then close it. Finish returns `YTR_ERR_IO` if a
   write failed at any time. Close finishes a writer that is still open,
   so a cleanup path that only closes still ends the file:

   ```c
   if (ytr_data_finish(&w) < 0) fprintf(stderr, "%s\n", ytr_data_error(&w));
   ytr_data_close(&w);
   ```

6. Read the file back to check it. ysp/table.h reads it as written. Set
   `allow_empty`, because a missing value is an empty field:

   ```c
   cd.allow_empty = true;
   ytb_open_csv_file(&back, "P07.csv", &cd);   /* needs YTB_STDIO */
   ```

   pandas (`pd.read_csv`) and R (`read.csv`) read the file with their
   defaults.

For an experiment without `ytr_trials` (a staircase alone, for example),
set `dd.trials` to NULL. The file then has your columns only.

## Draw a side per trial

This procedure draws a categorical value per trial (the side of a 2AFC
target) that the data file logs and a resume gives back.
`examples/response/trial_2afc_adaptive.c` does it.

1. Give the session a variable and a seed. Set the seed before the
   generator moves it:

   ```c
   static const char* const sides[] = { "left", "right" };
   d.vars[0] = ytr_var_choice("side", sides, 2);
   d.n_vars = 1;
   d.seed = seed;
   d.rng = ytr_splitmix;
   d.rng_ctx = &seed;
   ```

   The rules text gives the same with `var side choice left right`.

2. Read the value for each trial after `ytr_next()`:

   ```c
   int side = ytr_var(&t, ti.index, 0);   /* 0 left, 1 right */
   ```

3. Log `d.seed` with the session (the sidecar's `meta`). The data line
   has a `side` column and a `trial_seed` column, and `ytr_restore()` or `ytr_open_saved()` gives
   every value again.

To snap a jitter to the display's frames, write `rate=display` in the
rules text and set `d.display_rate_num` and `d.display_rate_den` from the
screen's refresh after `ytr_rules()` and before `ytr_open()`.

## Data file reference

The header's DATA FILE section is the full reference. A summary:

| Item | Rule |
|---|---|
| Columns | The session's (`ytr_format_header()`'s names), then `desc.cols` in order. At most 256 in all (`YTR_DATA_MAX_COLS`, ysp/table.h's bound; 64 before v0.6). A name is `[A-Za-z_][A-Za-z0-9_]*`, at most 63 bytes, unique. |
| Columns of seconds | The name ends in `_s` (v0.5). The open refuses, by name, a `YTR_COL_TIME_NS` column without `_s` or `_ns` and a session whose jitter's name does not end in `_s`. A `YTR_COL_DOUBLE` or text column of seconds is the caller's to name. |
| `YTR_COL_INT`, `YTR_COL_INT64` | Decimal. `YTR_MISSING_INT` (`INT_MIN`) or `YTR_MISSING_I64` (`INT64_MIN`) gives an empty field. |
| `YTR_COL_DOUBLE` | `decimals` 0: the fewest of 15 to 17 significant digits that read back as the same double. 1 to 17: that many fixed decimals. NaN and the infinities give an empty field. |
| `YTR_COL_BOOL` | `1` or `0`. |
| `YTR_COL_STRING`, `YTR_COL_CHARS` | UTF-8 text, quoted when it holds a comma, a quote, a CR or an LF. A byte that is not valid UTF-8 becomes U+FFFD. NULL gives an empty field. |
| `YTR_COL_TIME_NS` | `int64_t` nanoseconds written as seconds with 1 to 9 decimals (0 means 9, the exact value). Rounded half away from zero. `YTR_MISSING_NS` gives an empty field. A name that ends in `_ns` is written with `_s`. A name that ends in neither `_s` nor `_ns` is refused at open (v0.5). |
| `YTR_COL_CONST` | The same text in each row. |
| Line endings, encoding | LF; UTF-8 without a BOM; no comment lines. The decimal point is `.` in every locale. |
| When a row is on disk | `YTR_FLUSH_ROW` (default): in the OS when `ytr_data_write()` returns, so a crash of the program keeps it. `YTR_FLUSH_SYNC`: on the disk, so a power cut keeps it. |
| When to write | After `ytr_update()` or `ytr_requeue()` and before the next `ytr_next()`. Else `YTR_ERR_ORDER`. |
| Sidecar | `meta_path`: `ysp_trials=<version> data=<path>`, the caller's `meta` lines, `ytr_format_meta()`'s line, and at finish `rows=N closed=1`. |

## What v0.2 adds

v0.2 adds what the v0.1 non-goals and the rig plan
([rig_spec.md](rig_spec.md) 14.1 and 14.5) asked for:

- Conditions files and fixed trial lists from CSV, as tables. The parser,
  the number parser and the table block are a separate header,
  ysp/table.h ([table.md](table.md)), because the pack stores the
  same block.
- jsPsych's sample types: with replacement, a subset without replacement,
  fixed repetitions, alternating groups and an explicit order list.
- Groups of trials in an outer order, with Latin-square group orders by
  participant.
- "b always follows a" and its variants, as units.
- First-order transition balance.
- Latin and Williams squares by participant number.
- A rules text with one statement per C call or desc field.
- The Python and MEX bindings of all of it.

Every v0.1 desc gives the same schedule, draws, history, records,
tallies, format lines (but the version token) and snapshot bytes as
v0.1.1. `tests/adapt/trials_pins.h` holds the hashes of 600 v0.1.1
sessions (30 designs x 20 seeds); the test checks them.

The v0.2 decisions follow the v0.1 ones under Decisions.

## Decisions

### Tracks are opaque

The header could have included the three method headers and offered
`ytr_add_stair(&s)`. It does not, for the convention's sake (a header
includes nothing but ysp/rt.h, and this one includes nothing) and because
the caller has to call the track's own `next` and `update` anyway: the
sequencer cannot know the level's type. A track is `{ctx, is_done, weight}`
and the adapter is three lines. The cost is that the history records which
track a trial came from but not its level; that is what the caller record
is for, and the second USAGE example puts the level there.

### Constraints are repaired, not sampled, and then held at run time

Rejection sampling of whole orders fails silently on tight constraints and
gives no diagnosis. The repair is deterministic from the seed, terminates,
and when it fails it names the first constraint still violated, so a
contradictory design is reported at open. It is not uniform over
satisfying orders; no practical method is, and the manual says so. The
five rules cover the constraints seen in practice: run length, density in
a window, minimum spacing, forbidden transitions, and the first trial.

Two things the specification got wrong, found by the implementation.
First, "swap the offending trial with a random later one" traps itself at
the end of the schedule and failed about 15 percent of easy designs
(three rows, ten repetitions, no immediate repeat); the shipped repair
draws a random start and tries up to 64 slots, wrapping, taking the first
swap that leaves both positions legal, which passes every property check
in a fraction of the time. Second, the claim that interleaved track trials
and re-queues "can only widen a gap or break a run" was false: a
re-queued trial is a trial the observer sees, and a schedule made of
identical catch trials (the second USAGE example) cannot be ordered to
satisfy any spacing rule at all. So constraints are also enforced while
the session runs: before handing out a scheduled trial, `next` checks it
against the trials already run; if it would break a rule, a track trial
runs instead while any track is live, else the first later slot that fits
is pulled forward, and if nothing fits the trial runs and its history
entry carries a violation flag. With tracks, a failed open-time repair is
therefore not an error; without tracks it still is.

### `track_rate`, not a merged schedule

Tracks end when their own stop rule fires, which the sequencer cannot
predict, so a merged schedule with fixed positions for track trials would
run out of track before or after the conditions. A rate keeps the mix
right on average and degrades gracefully: when the tracks finish, the
schedule runs alone; when the schedule finishes, the tracks run alone.
The default is 1 with no conditions and 0 with no tracks, and required
otherwise, so a mixed design states its intention.

### One generator, consumed in a fixed order

Every random choice (the shuffle, the repair, practice draws, track picks,
re-queue positions) draws from the caller's generator in a fixed order.
That is what makes `ytr_restore` work: a fresh open with the same seed
rebuilds the same schedule, and replaying the outcomes makes the same
run-time draws in the same order. The header owns no generator, as the
rest of the collection does not; `ytr_splitmix` is a step function on
state the caller owns.

### Breaks are the caller's, their trace is the header's

The header has no clock, so a break by elapsed time is the caller's check
between trials. What the header owns is the trace: `block_size` gives
scheduled boundaries and `ytr_mark_break` records an unscheduled one,
both flagged in the history, and both end a run for the constraints,
because a run of three orientations before a rest and three after is not
a run anyone perceives. `constraints_span_blocks` turns that off for a
design that wants the stricter reading.

### Warmup is a condition trial, never a track trial

`n_warmup` trials at the start of every later block, drawn from a
caller-named list of easy conditions, flagged and excluded from tallies
and constraints, are a reintroduction after a rest. They are never drawn
from a track: the header does not know what an easy level is. A caller
who wants a staircase warmed up sees the flag on a condition trial, shows
what it likes, and chooses whether the track hears about it.

### The header saves its state, not the experiment's data

The sequencer's state is small and scalar, so it saves three ways: CSV
rows for analysis, a key=value metadata line to head a data file, and a
versioned byte snapshot (`ytr_save`, `ytr_open_saved`) that resumes a
crashed session without replaying the seed. No JSON parser: a header that
parses text is the wrong header. The caller's per-trial data is
deliberately bounded to `record_size` bytes. Anything large or variable
(an eye trace, an EEG epoch, audio) belongs in its own container keyed by
the trial index, with a reference in the fixed record if one is needed.
The sequencer's table stays narrow and joins on trial index, which is
how every analysis pipeline already works.

### Fixed capacity, inline

`YTR_MAX_TRIALS` (4096) and `YTR_MAX_CONDITIONS` (1024) size the
handle at compile time: 19 bytes per trial, 91480 bytes (89 KB) at the
defaults on x86-64 in v0.1.1, 110528 bytes (108 KB) in v0.2. A session of more than 4096 trials is two handles.
The caller's per-trial record is the one variable-size
thing and lives in a buffer the caller provides, sized by
`record_size x YTR_MAX_TRIALS`, so nothing allocates.

### What PsychoPy replay can and cannot check

PsychoPy shuffles with NumPy's generator. The orders cannot match trial
for trial unless this header reproduces NumPy's Mersenne Twister and its
shuffle, which it will not. The comparison therefore checks properties
against PsychoPy: under `random`, every condition once per repetition
block and the blocks in order; under `fullRandom`, every condition exactly
`nReps` times; under `sequential`, the identical order. The constraint
rules are checked directly on generated orders, and the failure path is
checked with a design known to be impossible.

### v0.2: tables are a separate header

The conditions file, the trial list in a pack and the table a script
makes are one thing, so they have one form: the PSTB block of
ysp/table.h. It is position independent, so a pack can map it and a CSV
parse can build it in an arena; both give the same bytes and the same
hash. ysp/trials.h requires ysp/table.h, as ysp/gfx.h requires
ysp/color.h, and compiles its implementation with its own. The split
keeps a CSV parser out of the trial sequencer for a caller who only views
packed tables, and gives the parser its own test, fuzz target and
mutation list. table.md has the parser's decisions: the RFC 4180
dialect, the bounds (32767 rows, 64 columns, 4096 bytes per field), the
correctly rounded number syntax with no locale, int32 integer columns,
and errors that name the line, the row and the column.

A table's rows are the conditions and its columns the factors, a factor's
levels the column's distinct values in order of first appearance. A rule
names a level by number in C and by its text in the rules text; a numeric
column compares by value, so `0.50` finds `0.5`. A fixed trial list
played in file order is a table with SEQUENTIAL order and 1 repetition;
nothing more is needed.

### v0.2: sampling: jsPsych's types, mapped

| jsPsych `sample.type` | Here |
|---|---|
| `fixed-repetitions` | `reps` (or `cond_reps`) with FULL_RANDOM, the v0.1 order |
| `with-replacement` (size, weights) | `YTR_ORDER_WITH_REPLACEMENT`, `draws`, `weights` |
| `without-replacement` (size) | `subset` k with FULL_RANDOM and 1 repetition |
| `alternate-groups` (groups, randomize_group_order) | `groups` ALTERNATE, group order RANDOM or any other |
| `custom` (a function) | `YTR_ORDER_LIST`; the caller's function writes the list |

Two differences are deliberate. jsPsych cuts alternating groups of
unequal size to the smallest; this header refuses them and names the
sizes, because a silent cut changes the design. A subset is chosen once
at open and not again per repetition; a design that wants a new subset
per block is one handle per block.

Draws with replacement use cumulative weights and bisection. A linear
scan measured 39 ms for 10000 draws over 10000 rows; bisection measured
1 to 2 ms (indicative, not under the lock; the Cost table has the locked
numbers). The cumulative sums use the tally arrays as scratch, so the
handle did not grow for them.

### v0.2: nesting is one level

Groups are the one level of nesting. The schedule is a flat array fixed
at open, which preloading (`ytr_condition_at`) and replay need. Deeper
trees, parameters inherited down a tree, and nodes decided at run time
(jsPsych's loop and conditional functions) belong to an experiment
definition above this header. A blocked group is a block for every v0.1
purpose (first_in_block, warmups, constraint segments), so groups reuse
the block machinery and need no new run-time state.

### v0.2: units, not a repair rule

The first design treated "b always follows a" as one more rule for the
swap repair. A Python prototype of both the rule and the alternative,
units shuffled as wholes, measured this (200 or 50 seeds per design;
success count, median repair steps, maximum in brackets):

| Design | As a repaired rule | As units |
|---|---|---|
| 10a 10b 20c, a->b | 200/200, 10 (20) | 200/200, 0 |
| 40a 40b 80c, a->b | 200/200, 43 (75) | 200/200, 0 |
| 100a 100b + 4 x 50 fillers, a->b | 200/200, 123 (214) | 200/200, 0 |
| 40a 40b 80c, a->b, max run 2 | 50/50, 5984 (62656) | 50/50, 172 (298) |
| 100a 100b + 4 x 50, a->b, max run 2 | 50/50, 139 (194) | 50/50, 5 (10) |
| 40a 40b 40c 40d, a->b, max run 1 | 50/50, 176 (592) | 50/50, 26 (44) |
| 20a 30b 50c (spare b), max run of c 2 | 50/50, 43 (83) | 50/50, 46 (72) |

The repair also biases the order. Over every valid order of a small
design, the chi-square of the observed against a uniform distribution
(99.9 % critical value in brackets):

| Design | Valid orders | As a repaired rule | As units |
|---|---|---|---|
| 2a 2b 2c, a->b | 6 | 730.6 (15) | 2.7 |
| 2a 2b 3c, a->b, max run of c 2 | 7 | 1743.5 (17) | 1223.1 |
| 2a 2b 2c 2d, a->b, max run 1 | 42 | 2689.7 (69) | 270.6 |

With no other rule, units give a uniform order at no repair cost. With
other rules, the repair moves whole units and is still biased, much less
than the rule-based repair. The first unit repair swapped units of equal
length and failed 0/50 on the tight design (40a 40b 80c, max run 2): when
every one-trial unit is a c, a swap of two of them changes nothing. The
shipped repair moves a unit to another boundary and closes the gap, which
passes 50/50.

followed_by and preceded_by pair the trials by a shuffle, so spare
b-trials are free trials and fewer b- than a-trials is an error at open.
A chunk column makes longer units (a cue, a target and a probe that run
together). Two structural rules may not touch the same rows: a longer
unit is a chunk column, and overlapping pairs have no clear meaning.

At run time a unit runs back to back: no track trial, no draw, no
forward move and no block start inside it. A block_size boundary waits
for the unit's end, and the later blocks keep the block_size grid, so a
unit costs one block a few trials and does not shift the rest. A re-queue
of any trial in a unit re-queues the whole unit, because a b without its
a is not the trial the design asked for.

### v0.2: balance by Euler circuit

Transition balance (every ordered pair of levels adjacent equally often)
is exact, not scored. A balanced order is an Euler circuit of the
complete directed graph on the levels, each arc lambda times. Kandel,
Matias, Unger and Winkler (1996) draw such a circuit uniformly; Brooks
(2012) recommends it for exactly this use. The prototype checked
uniformity over every balanced order of small designs:

| Levels | lambda | Self pairs | Orders | Draws | chi-square / df |
|---|---|---|---|---|---|
| 2 | 1 | yes | 4 | 40000 | 1.8 / 3 |
| 3 | 1 | yes | 216 | 43200 | 215.3 / 215 |
| 2 | 2 | yes | 36 | 40000 | 21.2 / 35 |
| 3 | 1 | no | 18 | 40000 | 16.0 / 17 |
| 4 | 1 | no | 3072 | 614400 | 3026.3 / 3071 |

The C test repeats the 3-level cases on the header (chi-square 183.0 on
215 df with self pairs, 19.7 on 17 without).

A circuit of n^2 lambda arcs visits n^2 lambda + 1 positions, so one
level occurs once more than the others. The header puts that trial
first, as a LEAD-IN, so all transitions occur and the other trials keep
equal counts (Brooks: "each sequence contains an extra trial of one
condition"). The lead-in is on by default because a missing transition
is a silent loss and an extra trial is visible. It is a main trial (the
observer sees it and the rules count it), flagged YTR_FLAG_LEADIN, with
rep -1 and no tally, and it cannot be re-queued.
`YTR_BALANCE_NO_LEADIN` leaves it out for a short sequence that cannot
afford the trial; one transition, last to first, is then missing, and
the manual says so. The test runs both.

No field was added to `ytr_trial_info` for the lead-in. A positional
initializer of the struct in the examples broke under
`-Wmissing-field-initializers` when one was, and callers have the same
initializers; the history flag and rep -1 tell the lead-in.

A rule on the balanced factor itself is refused. Rejection of balanced
orders with a run limit fails as sessions grow. The prototype measured
the chance that a uniform balanced order (with self pairs) meets a run
limit:

| Levels | lambda | Trials | P(run <= 2) | P(run <= 3) | P(run <= 4) |
|---|---|---|---|---|---|
| 2 | 4 | 17 | 0.003 | 0.360 | 0.811 |
| 2 | 16 | 65 | 0.000 | 0.002 | 0.070 |
| 4 | 4 | 65 | 0.014 | 0.657 | 0.961 |
| 4 | 16 | 257 | 0.000 | 0.042 | 0.552 |
| 8 | 4 | 257 | 0.035 | 0.826 | 0.992 |
| 8 | 16 | 1025 | 0.000 | 0.211 | 0.856 |

A repair would break the balance, and no exact sampler gives both
properties. `ytr_balance_no_repeat` covers the common case (no level
twice in a row) exactly. Rules on other factors are repaired by swaps
between trials of the same balanced level, which keep every transition.

### v0.2: latin squares by participant

`ytr_latin(n, row, balanced, out)` gives one row of a cyclic or a
Williams (1949) square, with the row taken modulo the design's rows, so a
participant number goes in directly. It is one function, not a design
object: a row is an order list, a group order (LATIN, BALANCED_LATIN) or
a session plan, and the caller stores nothing. For odd n the Williams
design needs 2n rows (each row and its reverse); the function returns the
row count so the caller can plan recruitment.

### v0.2: the rules text

The rules text maps 1:1 onto the C calls: the verbs are the C helper
names (`max_run target 3` is `ytr_max_run`). `ytr_format_rules`
writes a session's settings in the same text, so a logged session pastes
back in, and the bindings take the same text. Columns and levels go by
name, which the C API cannot do without the table.

| Source | Form | What was taken |
|---|---|---|
| OpenSesame loop operations | `constrain col maxrep=N mindist=D`, `weight col`, `shuffle` | the three lines, with OpenSesame's meaning (mindist counts rows, so `mindist=2` is `min_gap 1`) |
| OpenSesame | `slice`, `sort`, `sortby`, `reverse`, `roll`, `shuffle_horiz`, `fullfactorial`, `setcycle` | refused by name with a reason; they edit the table, which the caller does before open |
| jsPsych `sample` | an object per timeline | the sample types, as statements (`draws`, `subset`, `groups`) |
| PsychoPy loop | `Selected rows` | `where COLUMN=VALUE`, with `@participant` for one list per participant |
| Mix (van Casteren and Davis 2006) | max repetition, min distance | already in v0.1 as max_run and min_gap |

An unknown verb that is close to a known one gets "did you mean"; a word
from another tool (maxrep, mindist) gets the statement here. The
participant number is session data, so it comes from the call, never
from the text: one rules file serves every participant.

### v0.2: snapshots: format 1 stays

A desc with no v0.2 field saves format 1, the v0.1.1 bytes, so a v0.1
snapshot loads in v0.2 and a v0.1 session saves bytes v0.1.1 can load.
A desc with any v0.2 field saves format 2, which adds the table's hash,
the v0.2 desc fields, a hash of the order list, weights and group list
(arrays the handle does not keep), and the unit and lead-in state. load()
takes the format the desc implies and names the first desc field that
differs, so a snapshot cannot resume under a changed table or list.

### v0.2: not done

| Item | Why |
|---|---|
| Second-order or higher transition balance | no exact sampler is known to be practical; first order covers the designs in the rig plan |
| Balance with tracks, groups or units | track trials and unit boundaries cut transitions |
| A rule on the balanced factor | measured above: rejection fails as sessions grow |
| A new subset per repetition | one handle per block does it |
| Trees deeper than one group level, run-time nodes | experiment definition, above this header |
| OpenSesame's table edits (slice, sort, roll) | the caller edits the table before open |
| More than 32767 rows or 256 columns (64 before v0.6) | ysp/table.h bounds; refused by name |

### v0.2: bindings

The Python binding (`ysp.trials`, version 0.2.0) takes a `Table` from
CSV text or a file, every v0.2 desc field as a keyword, the rules text,
and the helpers `followed_by`, `preceded_by`, `chunk`, `balance`,
`groups`, `latin` and `latin_rows`. The MEX function takes `desc.table`
(CSV text, or a struct with the CSV and its options), the same desc
fields and the rules text, and adds the commands `table`, `latin`,
`balance`, `chunk`, `followed_by`, `preceded_by`, `values`, `table_info`
and `format_rules`. Both builds stage ysp/table.h beside ysp/trials.h.

### v0.2.1: jitter in the trial handler

A foreperiod, an inter-trial interval or an SOA that varies from trial
to trial is a random draw like the order, so it belongs to the same
generator and the same contract (rig_spec 14.7 rank 9). In the session
it gets three things a caller's own draw does not: `ytr_restore()` and
`ytr_open_saved()` reproduce it, the data line logs it, and a table column
can set its interval per condition. The header draws and logs; the
caller waits.

Decisions, each with its reason:

- Seconds, the user's unit for definitions and data (decided
  2026-10-07), with `ns` as int64 for a clock and `frames` for a snapped
  draw. A draw is a whole number of nanoseconds; `s` is that number
  over 1e9, correctly rounded. The data line prints the seconds as the
  exact decimal of `ns` (`1.184516667`), so the logged value is the value
  waited.
- One variate per jitter per trial. No rejection loop, so the generator's
  position after a trial does not depend on the values drawn, and a
  mutant or a bug that changes a value cannot shift the later draws.
- Integer arithmetic only, from the variate's 53 bits. The first version
  mapped the exponential through the C library's `log1p` and `expm1`, and
  the libraries round those differently: over 1,000,000 draws the
  doubles' bits differed between glibc 2.35, mingw-w64 and the UCRT,
  though the nanoseconds agreed. Agreement on a sample is not a
  guarantee (a last-bit difference can flip a rounding on some draw),
  and the Python binding must regenerate a Windows session exactly, so
  the coordinator asked for ysp/rdk.h's standard: the same bits on every C
  library, compiler and flag set. Every draw is now integer arithmetic,
  with no libm call and no floating-point operation that -Ofast,
  /fp:fast or FMA contraction could change; the seconds-to-nanoseconds
  conversion reads the double's bits. Golden digests of 200,000 draws of
  each kind are checked by the test on every CI platform and by
  `tests/adapt/trials_jitter_repro.c` under -O3 -march=native and
  -Ofast.
- The exponential is the untruncated inverse CDF, E = s (-ln(1 - u)),
  folded into the interval: lo + (E mod (hi - lo)). The exponential is
  memoryless, so the folded value is exactly the truncated exponential,
  and no exp() is needed, only one logarithm. The cost is that the map is
  no longer monotone in u; nothing in the header needs it to be.
- -ln(1 - u) is a Q58 fixed-point logarithm: y = 1 - u as m 2^e, m
  times a table reciprocal of 1 + i/128 (i its next 7 bits, rounded up
  so 0 <= t < 2^-7), and ln(1 + t) by an integer Horner series of degree
  8. The 128 reciprocals and their logarithms come from mpmath at 300
  bits (`tests/compare/trials_jitter_ref.py tables`). Error by analysis
  under 2^-57; measured 0.71 x 2^-58 at worst over 1,000,000 values
  against mpmath (`trials_jitter_ref.py check` on the output of
  `tests/compare/trials_jitter_dump.c`), and the test checks 123 points
  within 1 unit of the rounded reference. The duration is then within
  0.5 ns + s x 7e-18 of s (-ln(1 - u)) for the variate's 53 bits.
- 128-bit products and quotients use the compiler's `unsigned __int128`,
  or MSVC's `_umul128` and `_udiv128`, where they exist, and 32-bit
  pieces elsewhere. Both give the same exact integers; the test and the
  digests also run with the pieces forced (`-DYTR__NO_U128`).
- The jitter draws come after all of next()'s own draws for the trial,
  and open() draws none. A desc without jitters therefore draws exactly
  as v0.2.0 did (the v0.2.0 pins below), and a desc with jitters has the
  schedule it would have without them; the test checks both by logging
  the generator.
- Three distributions. UNIFORM is the common case. CHOICE covers a list
  of SOAs or a hand-made distribution (repeat a value to weight it).
  EXPONENTIAL truncated to [lo, hi] is the non-aging foreperiod: its
  constant hazard keeps the observer's expectancy flat. Normal, log-normal
  and gamma foreperiods were left out: their hazards rise, which is the
  aging the exponential exists to avoid, and CHOICE can approximate any of
  them.
- Snapping draws the frame, not a continuous value then rounded.
  Rounding a uniform draw gives the two end frames half the weight of the
  others; drawing the frame index makes every whole frame in [lo, hi]
  equally likely. The snapped exponential is a geometric over the frames,
  exactly memoryless on the frame grid.
- A frame is inside [lo, hi] when its time rounded to the nanosecond
  is, computed in integers. In binary 0.07 x 100 is 7.000000000000001 and
  0.29 x 100 is 28.999999999999996, so a floating-point test would drop
  the frames at 0.07 s and 0.29 s; the first version needed a tolerance
  of 1e-9 frames for that, which integer arithmetic made unnecessary.
  Mutants j-01 and j-02 check both ends. No frame inside is an error at
  open that names the rate and the interval.
- Storage: 8 bytes per trial per jitter, YTR_MAX_JITTERS (4) of them,
  so the handle grew from 110528 to 243344 bytes; `-DYTR_MAX_JITTERS=1`
  makes it 143744. Storing the variate instead costs the same, and
  recomputing the value on access would run the inverse CDF on every
  read.
- The call level (`ytr_jitter_draw`, `ytr_jitter_map`) touches no
  session. A draw from the session's generator between next() and
  update() would move every later draw and `ytr_restore()` would not
  repeat it, so the manual sends replayable designs to `desc.jitters`.

The hazard of the snapped exponential, 0.5 to 2.0 s, scale 0.4 s, at 60
Hz, measured by the test on 1,000,000 draws as count(k) / count(>= k):

| Quantity | Value |
|---|---|
| Flat per-frame hazard, 1 - e^-(T/s) | 0.04081 (2.4486 /s against 1/s = 2.5 /s; 2.1 % low) |
| Truncation factor 3 s before hi, 1 / (1 - e^-3) (theory) | 1.052 |
| Measured per-frame hazard against flat, up to 0.8 s (hi - 3 s) | within 6.2 % |
| Measured against the truncated geometric's exact hazard, while at least 20,000 draws survive (to 1.75 s) | within 4.4 % (sampling noise) |
| Mass the truncation moves from beyond hi into [lo, hi], e^-(hi-lo)/s | 2.4 % |

Snapping itself adds no deviation from a flat per-frame hazard; the
deviation is the truncation's, as for the continuous distribution, and
it is under 6 % until 3 scales before hi.


### v0.4: the data writer

Two usability reviews (2026-10-10) found no data-file path:
`ytr_format_row()` cannot write the caller's columns (the RT, the
level), so every example wrote its CSV by hand, each with its own NaN
handling and quoting. The user approved a shared row writer in this
header, a record schema, and PsychoPy's `addData` without a heap.

Decisions, each with its reason:

- **The repository conventions (README.md, Conventions).** The writer was
  built beside v0.3.0 and merged after it, so it follows v0.3.0's rules:
  `ytr_data_finish()` ends the file and the sidecar and returns 0 or a
  code, as `yvid_seq_finish()` does for ysp/video.h's frame writer;
  `ytr_data_close()` returns nothing, is safe on a zeroed writer, after a
  failed open and twice, and finishes a writer left open. Every setter,
  write and finish that fails writes `ytr_data_error()`. A `TIME_NS`
  column named `x_ns` is written `x_s`, because the file holds seconds
  and a column's name keeps its unit; `ytr_data_col()` takes either name.
- **In trials.h, with stdio.** The session's columns are this header's,
  so the writer that joins them to the caller's belongs here. The file is
  opened with the C library (on Windows `_wfsopen` for a UTF-8 path), so
  the header still makes no OS call of its own but `_commit()` or
  `fsync()` for `YTR_FLUSH_SYNC`. A write function (`desc.write`) replaces
  the file for a caller with its own I/O (a pack, a socket, the tests'
  memory buffer).
- **Columns declared at open, not added per row.** PsychoPy adds a column
  the first time `addData` names it and writes the file at the end. Here
  the header line is written at open and every row as it happens, so the
  set of columns must be known first; a fixed list also means no
  allocation and a check of every name, type and field size before the
  session starts. `YTR_COL_SET` covers the values that exist on some rows
  only.
- **Bound by offset, with the field's size checked.** `YTR_COL(T, field,
  type)` stores `offsetof` and `sizeof` of the field, so `open()` refuses
  an `int` field declared `DOUBLE`, the commonest way to write garbage. The
  macros are constant expressions, so a column list can be a static array
  in C and C++.
- **A file that ysp/table.h reads.** Names follow its header rule, a
  file has at most its 64 columns, text is valid UTF-8, numbers use its
  syntax. The writer refuses a session whose factor name the reader would
  refuse, at open, by name, rather than write a file that fails later.
  Bad UTF-8 in a value becomes U+FFFD instead of an error: one response
  string from a misconfigured keyboard layout must not cost the session's
  data.
- **Missing values are empty fields.** That is what pandas, R and
  ysp/table.h (with `allow_empty`) read as NA or NaN. The sentinels are
  the values a measurement cannot take: NaN for a double, `INT64_MIN` for
  a time, `INT_MIN` for an int, NULL for a string. Infinities are written
  empty too, because ysp/table.h takes no `inf`.
- **Times as int64 nanoseconds in C, seconds in the file.** The units rule
  (2026-10-07): data files are in seconds, C APIs in int64 ns. The
  conversion is integer arithmetic with a chosen number of decimals, so
  the text is the exact decimal of the nanoseconds at 9 decimals, and no
  double rounds it.
- **Doubles in the fewest digits that read back.** 15 to 17 significant
  digits, checked through ysp/table.h's own parser, so `0.1` stays `0.1`
  and every double reads back bit for bit. The decimal point is put back
  to `.` after `snprintf`, so a German locale cannot write `0,1`.
- **A row reaches the OS per write.** The file has no stdio buffer
  (`_IONBF`), and the row goes out in one write call from the handle's
  buffer. A crash, `abort()` or the screen header's panic exit
  (`TerminateProcess`) then keeps every finished row; a normal Shift+Esc
  abort would keep them anyway, because `exit()` flushes stdio. Measured
  cost: 15 us per row (median) with gcc, against 4 us to format it
  (Verification and cost (v0.4)). A buffered mode that waits for the end
  of the session was not added: it saves about 11 us per trial, which no
  inter-trial interval needs, and loses the rows of a crashed session.
- **`YTR_FLUSH_SYNC` is opt-in.** Waiting for the disk took 1.5 to 1.7 ms
  per row (median), up to 8 ms in the first probe. That fits between
  trials but is more than half a 60 Hz frame, so it is not the default.
  It is the choice for a session where a power cut is likely.
- **One row per write, after update().** `ytr_data_write()` writes the
  last completed trial and refuses (`YTR_ERR_ORDER`) while a trial is
  pending. The first version wrote the last completed trial at any time;
  the test showed that a write before `update()` paired the pending
  trial's values with the previous trial's columns, an off-by-one that
  only the first trial of a session would flag. A second write for one
  trial is refused too. A trial without a write has no row: practice
  rows may be left out on purpose, and the index column shows the gap.
- **A sidecar, not comment lines.** The examples wrote `#` lines with the
  describe line and the seed at the top of their CSV. ysp/table.h refuses
  them, pandas needs `comment='#'` and R's `read.csv` needs
  `comment.char`. The sidecar (`meta_path`) holds those lines as
  key=value, plus `ytr_format_meta()`'s line, and ends with `rows=N
  closed=1` at finish, so a session that did not finish is visible. The
  rig plan (rig_spec 6 and 14.1; devices_spec 11.4) gives the data file
  to the player, with a header of device rows, event rows and stream
  tables. That format is not designed yet; this writer gives its trial
  table, and the sidecar can carry the device summary and the rig
  profile's hash (`yrig_hash`) until the player defines the rest.
- **Constant columns repeat per row.** `YTR_COL_TEXT` writes the
  participant or session in every row, as PsychoPy writes its session
  fields. It costs bytes and makes files from many sessions concatenate
  into one table with no join.

Not done:

| Item | Why |
|---|---|
| Append to an existing file on resume | A resumed session (`ytr_open_saved()`) opens a new file; the writer starts at the first trial completed after it opens, and the two files share the header. Checking an old file's header before appending needs a reader in the writer. |
| The Python and MEX bindings | Python has `csv` and pandas, MATLAB `writetable`; the bindings keep the sequencer only. |
| A fuzz target | The writer parses no input; the round-trip test writes 400 random strings with every special byte and reads them back. |
| Migrating the examples | A separate change; `examples/trials/mocs.c` is converted as the demonstration. |

### v0.5: columns of seconds end in `_s`

- Problem: jsPsych writes `rt` in milliseconds and PsychoPy in seconds.
  A ysp file read with the wrong habit is off by 1000, and nothing in the
  file flags it.
- Decision (the user, 2026-10-20): each data-file column that holds a
  time in seconds ends in `_s`. Experiment definitions already require
  the suffix on duration keys ([experiment_format.md](experiment_format.md),
  D4); data files now match. No user data existed, so there is no
  migration.
- Enforcement: `ytr_data_open()` refuses a `YTR_COL_TIME_NS` column whose
  name ends in neither `_s` nor `_ns` (`_ns` is written `_s`), and a
  session whose jitter's name does not end in `_s`, since the session's
  jitter columns hold seconds. The message names the column and gives
  the suffixed name. A refusal at open, not a silent rename: a silent
  `rt` to `rt_s` would make the name in the code differ from the name in
  the file, and `ytr_data_col()` and `yrsp_data_set()` find columns by
  name.
- Not enforced: a `YTR_COL_DOUBLE` or text column of seconds, a table
  column of seconds that the session copies into the row, and
  `ytr_format_header()` alone. The writer cannot see a double's unit, and
  `ytr_format_header()` returns names and cannot refuse. These are the
  caller's to name.
- Checked on 2026-10-20 with MSVC 19.44 and MinGW-w64 gcc 16.1, on Linux
  (WSL2) and under emcc: 6 new refusals (`x`, `rt`, `rts` and `rt_ms` as
  `TIME_NS`, jitters `iti` and `is`) and 3 accepted names (`rt_s`, `_s`,
  and `rt` as `DOUBLE`) in `tests/adapt/trials_test_data.h`; the C and
  C++ compile checks refuse a `TIME_NS` column named `rt`. Mutations
  `d-26` to `d-29` caught, 4 of 4, and `d-24` still caught. The examples
  that write through the data writer or by hand were renamed, and the
  CI `--sim` lists pass.

## v0.6: variables, weight x reps, the display's rate, 256 columns

v0.6.0 closes four gaps of the experiment definition
([experiment_format.md](experiment_format.md) 1.4): G3, G4, G10 and G11.

### Variables from the trial's own stream (G3)

- Problem: the 2AFC program drew its side from a second generator of its
  own. The side was not in the session's data line, `ytr_restore()` did
  not give it back, and a draw from the session's generator between
  next() and update() would have moved every later draw.
- Options: (a) draw from the session's generator after the jitters, as
  jitters do, and store the value per trial; (b) a stream per trial from
  a seed and the trial index (the experiment definition's D8).
- Decision: (b), as D8 decided. `desc.vars[]` of `ytr_var_choice(name,
  values, n)`; `desc.seed`; `ytr_var()`, `ytr_var_text()`,
  `ytr_var_index()`, `ytr_trial_seed()`; the rules statement `var NAME
  choice V V ...`. Reasons: (a) moves every jitter and schedule draw of a
  session when a variable is added, and stores 8 bytes per trial; (b)
  takes nothing from the session's generator and stores nothing per
  trial, since a value is a function of the seed and the trial index. A
  re-queued copy is a new trial and draws new values; a track trial has
  values like any other.
- The hash (pinned; the header's VARIABLES section): trial i's stream is
  splitmix64 from the state mix64((seed XOR K) + (i + 1) G), with G
  0x9E3779B97F4A7C15 and K 0x6A09E667F3BCC909 (the first 64 bits of the
  fraction of the square root of 2). Output 0 is the trial's seed, output
  1 + j variable j, whose index is floor((output >> 11) n / 2^53), the
  jitters' CHOICE mapping. K: a caller who seeds a splitmix64 session
  generator and `desc.seed` with one number would otherwise get trial
  streams whose states are that generator's outputs.
- Output 0 for the trial seed and not a variable's output: a stimulus
  generator seeded with `ytr_trial_seed()` and stepped as splitmix64
  draws mix64(seed + G), which is not any variable's output, so a noise
  field and a 2AFC side drawn for one trial are not the same bits.
- Values are texts, 1 to 63 bytes of valid UTF-8, because the data line
  writes them; the definition gives a number as its canonical text.
- The data line has a `trial_seed` column whenever trial seeds are defined
  (variables, or a nonzero seed), so a data file gives each trial's
  stimulus seed with no re-run of the hash (the user, 2026-10-21). A desc
  with neither has no column, so the pinned format lines do not move.
  The spelling is `x` and 16 lowercase hex digits (`x00f3a9c4e2b7d158`),
  chosen by measurement: ten seeds (0, 2^53 + 1, 2^63 - 1, 2^63, 2^64 - 1,
  one of all decimal digits, and others) in a file each spelling, and a
  one-row file of the all-digit seed, read with each tool's defaults
  (pandas 3.0.6, R 4.6.1 `read.csv` on WSL2, ysp/table.h 0.3.0):

  | Spelling | pandas | R `read.csv` | ysp/table.h |
  |---|---|---|---|
  | decimal | exact (uint64; int64 for one row) | numeric, rounded (81985529205302085 reads 81985529205302080) | refused: 20 significant digits |
  | `0x` + 16 hex | exact (text) | numeric, rounded: R reads hex | exact (text) |
  | 16 hex | exact (text); a one-row all-digit file reads as an integer and loses its leading zero | text; the one-row file reads as a number | text; the one-row file reads as a number |
  | `x` + 16 hex | exact (text) | exact (text) | exact (text) |

  `int(s[1:], 16)` (Python) or `strtoull(s + 1, NULL, 16)` (C) gives the
  seed back; R holds it exactly only as text, since a double has 53 bits.
- Snapshot format 5 for a desc with variables or a nonzero seed: format 4
  plus the seed and the variables. A seed alone counts, because the trial
  seeds depend on it.

### `weight` is per repetition (G4)

- Problem: `weight COLUMN` set `cond_reps` from the column, and `reps`
  beside it was ignored with no message, so the Stroop program
  multiplied the weight by `--reps` in C.
- Decision: a weight is each row's count per repetition, as OpenSesame's
  weight is per cycle, and `reps` (from the text or the desc) multiplies
  it. `cond_reps N N ...` stays each row's whole count, and the text now
  refuses `reps` beside it, the same silent loss. Breaking for a rules
  text that gave `weight` and `reps`: it now has reps times the trials.
  No logged session is affected: `ytr_format_rules()` writes `cond_reps`
  with the products, never `weight`.

### `rate=display` (G11)

- Problem: the EEG example snaps its SOA to the rig's refresh. The rules
  text could name a fixed rate only, so the player had to set `rate_num`
  and `rate_den` after `ytr_rules()` with nothing in the text to say so.
- Decision: `rate=display` is `YTR_RATE_DISPLAY` (-1) in `rate_num`;
  open() takes `desc.display_rate_num / display_rate_den` and refuses,
  naming the jitter, without it. From open() the jitter is one at that
  rate: the same draws, data line and snapshot bytes as a fixed rate of
  the same numbers, so a session resumes under either desc.
  `ytr_format_rules()` writes `rate=display`, so a logged session pastes
  onto another rig; `ytr_format_meta()` writes the rate drawn on.

### 256 columns (G10)

- Problem: a trial row of the definition (2.10) has seven columns per
  recorded stimulus beside the others, so 64 columns (ysp/table.h's
  bound, which the writer kept) is short. The session's header line was
  also built in the 8 KB row buffer.
- Options: remove the bound (column state from the caller, or in the
  rules arena's way); raise it.
- Decision: raise both to 256 (ysp/table.h 0.3.0, `YTR_DATA_MAX_COLS`).
  A removed bound needs storage the caller gives per column, a second
  API for a case no template reaches yet. The writer's names moved out of
  the per-column struct, so a row's pass reads 40 bytes per column, not
  112; the header line goes out in pieces after the file opens, so 256
  names of 63 bytes (16 KB) do not hit the row buffer. `YTR_DATA_MAX_COLS`
  may be defined lower to save 104 bytes a column.

### Verification and cost (v0.6)

Checked on 2026-10-21 (Windows 11 with MSVC 19.44 and MinGW-w64 gcc
16.1, WSL2 gcc 11.4, emcc):

- `tests/adapt/trials_test_vars.h`, part of `trials_test.c`: every value
  and trial seed of a session against a second implementation of the
  hash written from the manual, three outputs pinned by value; a session
  with variables draws the same schedule, jitters and generator position
  as without; 300,000 trials of the stream, values chi-square 4.01 on 2
  df and pairs of two variables 4.77 on 5 df; `ytr_restore()` and save
  and load (format 5) at every fifth cut give every data line again; a
  desc with another seed, another value or fewer variables is refused by
  name; 13 refusals at open; the rules statement and its round trip
  through `ytr_format_rules()`, 8 messages; weight x reps against the C
  product (the same schedule), in either order, from the desc's reps,
  with `where`, and the two refusals; `rate=display` against
  `rate=60000/1001` (the same draws and snapshot bytes), its refusals and
  the call level's; the variables in a written file read back.
- `tests/adapt/trials_test_data.h`: 256 columns of 63-byte names open,
  write a row and read it back (the header is over 8 KB); 257 with a
  session are refused; the session's columns plus the caller's up to 256
  open.
- The 600 v0.1.1 and 320 v0.2.0 pins, the jitter golden digests and the
  v0.2 digest are unchanged.

- Mutations: 21 new in `tests/mutate/trials.toml` (x-01 to x-21: the
  stream's key and outputs, the value mapping, the refusals, quoting, the
  seed in the snapshot and its format, weight x reps and its two
  refusals, the display's rate and its rules text, the writer's bound and
  its header line, the trial_seed column and its spelling), all killed;
  99 of 99 in the file, MinGW gcc 16.1.
  x-16 (one name past the bound) survived its first run: a session alone
  could pass 256 names unchecked, so the test now opens a writer on a
  247-column table. ysp/table.h: b-01 and b-02, 27 of 28 in the file, the
  survivor the known equivalent n-08.
- Fuzzing: `tests/fuzz/trials_fuzz.c` reads every variable and trial seed
  and sets a display rate; two new seeds (`var`, `rate=display`, `weight`
  with `reps`). MSVC 19.44 libFuzzer with ASan, 601 s from its 20 seeds:
  3,205,678 inputs, 8260 coverage features, 1352 corpus files, no crash;
  the corpus replayed on WSL2 gcc 11.4 under ASan and UBSan, no report.

The data writer's cost (G10), `tests/compare/trials_data_cols.c` (a session of 18 columns:
ten integers, four table texts, four jitters; then the caller's columns,
cycling int, bool, time, shortest double, fixed double, string), 4000 rows
of a FULL_RANDOM session, the same program built against the v0.5.1
header (f2fb5f3) and this one. Iris Xe laptop, AC, measurement lock held,
CPU load 0 % when it was taken (59 % at the end of the second round, from
other workers' builds; the table gives the first round). Per row, median
(p99):

| Columns | Path | gcc 16.1 -O2, v0.5.1 | gcc, v0.6.0 | MSVC 19.44 /O2, v0.5.1 | MSVC, v0.6.0 |
|---|---|---|---|---|---|
| 26 | memory | 4.8 us (8.2) | 4.8 us (7.3) | 2.9 us (4.5) | 2.8 us (4.9) |
| 64 | memory | 25.0 us (40.0) | 25.2 us (47.2) | 12.3 us (16.1) | 12.1 us (19.5) |
| 256 | memory | refused | 116.8 us (225.0) | refused | 54.9 us (92.7) |
| 26 | file, `YTR_FLUSH_ROW` | 21.4 us (63.7) | 19.2 us (49.9) | 15.1 us (35.2) | 16.7 us (42.9) |
| 64 | file | 46.0 us (124.7) | 38.0 us (90.1) | 27.0 us (81.6) | 25.4 us (50.1) |
| 256 | file | refused | 131.6 us (254.7) | refused | 77.3 us (199.6) |

- Time: the same per row at 26 and 64 columns within the spread of the
  second round (gcc 26 columns in memory: 5.6 and 5.2 us). 256 columns
  cost 0.46 us (gcc) and 0.21 us (MSVC) per column in memory, about what
  64 do: the cost is per column, between trials.
- Allocations: none in `ytr_open()`, none in the 4000 writes, one at
  `ytr_data_open()` (the C library's FILE), before and after. Counted on
  WSL2 gcc 11.4 by replacing glibc's malloc, calloc and realloc, which the
  C library's own calls go through.
- Memory: `ytr_data` 18,760 to 38,224 bytes (x64): 104 bytes per column
  of the bound, of which a row reads 40 (the names, 64 each, live apart;
  v0.5.1 read 112 per column). `ytr_trials` 243,344 to 253,376 bytes
  (variables 8 KB, the table's level pointers 1.5 KB); 251,960 under
  emcc. `ytb_csv_desc` 1072 to 4144 bytes.

## Verification (v0.2)

All on Windows 11 with MinGW-w64 gcc 16.1 (C11, C99, C++17, warnings as
errors) and MSVC 19.44 (/W4 /WX, C and C++17), and on Linux (WSL2) with
gcc 11.4 as C11 and C++17 under AddressSanitizer and
UndefinedBehaviorSanitizer, with no report. The header's STATUS block
lists every check; in summary:

- v0.1 behavior: the hashes of 600 v0.1.1 sessions (30 designs x 20
  seeds; schedule, history, tallies, records, format lines and snapshot
  bytes) match on gcc, MSVC and Linux gcc. Both v0.1 examples print what
  v0.1.1 printed but the version token.
- `tests/adapt/trials_test_v02.h`, at `YTR_MAX_TRIALS` 4096 and 256:
  each v0.2 order's properties by chi-square against its stated
  distribution (draws against weights, pairs of draws, subsets, group
  orders, unit orders, the slot of a free trial, balanced orders over all
  216 and all 18 of two small designs); exact counts where the design
  promises them (Latin and Williams squares for n = 1 to 64, the pair
  counts of 1600 balanced orders, 0 broken pairs in 200 unit sessions with
  re-queues and blocks); the rules text against the C calls; snapshot
  format 2 loaded at every third cut; and a digest of v0.2 sessions,
  `c9b59a3bb40a8f68` on every compiler and on Linux.

| Statistic | Value | df |
|---|---|---|
| Draws against weights, worst of three weight sets | 0.20 of the 99.9 % bound | 1 to 3 |
| Pairs of successive draws | 1.3 | 8 |
| 2-subsets of 5 | 13.8 | 9 |
| RANDOM group order | 3.1 | 5 |
| Units, the 6 valid orders | 7.4 | 5 |
| Balance, 3 levels with self pairs, 216 orders | 183.0 | 215 |
| Balance, 3 levels without self pairs, 18 orders | 19.7 | 17 |

- `tests/adapt/table_test.c` for the parser; docs/table.md has
  its results (10,000,084 numbers against strtod on each Windows
  compiler, 0 disagreements).
- Fuzzing, MSVC 19.44 libFuzzer with AddressSanitizer, from the seeds the
  targets write, then the corpus replayed on Linux gcc 11.4 under ASan
  and UBSan as C11 and C++17:

| Target | Time | Inputs | Coverage features | Corpus | Result |
|---|---|---|---|---|---|
| `tests/fuzz/table_fuzz.c` | 1351 s | 5,906,077 | 3208 | 958 | no crash, no timeout; replay clean |
| `tests/fuzz/trials_fuzz.c` | 1351 s | 9,734,743 | 7493 | 1383 | no crash, no timeout; replay clean |

  The trials target reads rules text against a fixed table, opens with
  a repair budget of 2000 swaps, and runs up to 300 trials with
  re-queues, breaks, the format functions and a save and load.
- Mutations (`tests/mutate/trials.toml`: 6 in the v0.1 core, which the
  pins must catch, and 23 in the v0.2 code): 29 killed. Three needed new
  checks before they were: u-01 (the unit move does not check the slot
  that closes the gap) is caught by the digest; u-05 (a chunk that joins
  rows that are not next to each other) needed the chi-square of a free
  trial's slot; and s-01 (`>=` in the draw bisection) needed draws
  with the variate fixed at 0 and 0.5, because only a variate exactly on a
  running sum tells the two apart and then picks a zero-weight row.
  `tests/mutate/table.toml`: 24 of 25 killed, the survivor equivalent
  (docs/table.md).
- Python binding: 152 tests, among them the parser against Python's `csv`
  module (40 random files) and `float()` (5000 values). MEX: the v0.2
  part of `test_mex.m` passes in MATLAB R2023a and Octave 10.1.

Not done: a run on macOS or big-endian hardware; the extra gcc 11.4
warnings and the clang 11.1 builds that v0.1.1 ran.

## Cost (v0.2)

All on the Iris Xe laptop, AC, the measurement lock held (`guard.sh time
trials`), 2026-10-07, `examples/trials/bench.c`, 21 rounds, MinGW gcc 16.1
-O2 and MSVC 19.44 /O2, built with `YTR_MAX_TRIALS` and
`YTR_MAX_CONDITIONS` at 16384 so one handle holds 10,000 trials (the
handle is then 608192 bytes). The table has 10,000 rows; open() builds
the whole schedule.

| Design | Trials | gcc median ms (min to max) | MSVC median ms (min to max) | Repair steps |
|---|---|---|---|---|
| List in file order (SEQUENTIAL) | 10000 | 0.02 (0.02 to 0.26) | 0.02 (0.02 to 0.22) | 0 |
| FULL_RANDOM | 10000 | 0.04 (0.04 to 0.04) | 0.04 (0.04 to 0.05) | 0 |
| CONSTRAINED, max_run 3 on 4 levels | 10000 | 1.82 (1.25 to 2.36) | 1.54 (1.08 to 1.90) | 159 |
| Units (followed_by) + max_run 3 | 10000 | 5.02 (4.20 to 6.19) | 7.03 (5.31 to 9.78) | 168 |
| WITH_REPLACEMENT, 10,000 draws of 10,000 rows | 10000 | 0.73 (0.73 to 0.75) | 1.33 (1.30 to 1.36) | 0 |
| Subset 5,000 of 10,000, FULL_RANDOM | 5000 | 0.14 (0.13 to 0.14) | 0.14 (0.14 to 0.15) | 0 |
| Balance, 8 levels | 9985 | 0.26 (0.26 to 0.27) | 0.30 (0.29 to 0.33) | 0 |
| BLOCKED groups (8, Williams) + max_run 3 | 10000 | 1.79 (1.61 to 2.00) | 1.73 (1.60 to 2.66) | 119 |

open() runs before the session, so these costs are outside any frame.
At run time, next() + update() over 5 runs of the units design with 5 %
re-queues (54,034 calls) took 0.04 us on average on both compilers; the
worst call was 19.0 us under gcc and 2.2 us under MSVC, the worst case a
forward move or a unit re-queue, far inside a 16 ms frame.

Two v0.2 paths changed because of indicative runs (not under the lock):
the subset's check of the remaining counts went from O(n k) to a
histogram (30 ms to 0.2 ms for 5,000 of 10,000), and draws with
replacement from a linear scan to bisection (39 ms to 1 to 2 ms).


## Verification and cost (v0.4)

All on Windows 11 with MinGW-w64 gcc 16.1 (C11 and C++17 under
`-Wall -Wextra -Wpedantic -Wshadow -Werror`) and MSVC 19.44 (/W4 /WX, C and
C++17), 2026-10-10.

- `tests/adapt/trials_test_data.h`, part of `trials_test.c`:
  - Every column type and missing value against a fixed expected text,
    then read back by `ytb_open_csv()`: the numbers equal, the texts as given.
  - `TIME_NS` at each of 1 to 9 decimals for 16 values around the
    rounding points (0, +-1, 5, 499999999, 500000000, 1499999999,
    INT64_MAX, INT64_MIN + 1) against a reference made from the digits.
  - 2000 doubles (random bit patterns, the least subnormal, the largest
    double) written shortest and read back by ysp/table.h bit for bit; at
    most 17 significant digits. Under the German locale (on Windows) the
    decimal point stays `.`, shortest and fixed.
  - 400 random strings from commas, quotes, CR, LF, CRLF, spaces, `#`,
    tabs, 2-, 3- and 4-byte UTF-8, and four invalid sequences (a lone
    0xFF, a cut 2-byte sequence, a surrogate, an overlong form), read back
    with every invalid byte as U+FFFD and every other byte equal.
  - A session (a table with a quoted cell, practice, a re-queue, a trial
    without a row, a jitter): each row's session part equals
    `ytr_format_row()` and the caller's part the values given; the write
    refused before any trial, with a trial pending, and twice for one
    trial; a writer opened mid-session writes nothing old.
  - 34 refusals (23 at open, 11 by the setters and write), each by its
    message or code; a write function that fails once:
    the row refused, every later row refused, nothing written after the
    failure, finish() YTR_ERR_IO; a 24 KB field written whole in pieces; a file
    with a UTF-8 name and its sidecar, in both flush modes, read after
    each row while the writer holds it open (the row is in the file), and
    the sidecar's first and last lines.
- `examples/trials/save_rows.c --sim` reads its own file back with
  `ytb_open_csv_file()` and checks every row; CI runs it. `trials_mocs` prints
  its CSV through the writer.
- Mutations: 25 data-writer mutants in `tests/mutate/trials.toml`,
  all 25 killed (d-23 to d-25 came with the port to the conventions:
  close finishes, the `_s` column name, a setter's message); 74 of 74
  in the file, MinGW gcc 16.1, 2026-10-10. Four survived the first run: d-01, d-03, d-04 and d-09
  broke the read-back itself, and the test skipped its checks when
  ytb_open_csv() refused the file; a refused read-back is now a failed check.
  d-18 removed one of the two guards that stop writing after a failure
  and was equivalent; it now removes both.

Cost, `examples/trials/bench.c`, the measurement lock held, the Iris Xe
laptop on AC. A 26-column row (the session's 18: ten integers, four table
texts, four jitters; the caller's 8: an int, a bool, two times, a shortest
double, a fixed double, a string, a constant), written after each trial of
a FULL_RANDOM session; per row, median (p99, max):

| Path | Rows | gcc 16.1 -O2 | MSVC 19.44 /O2 |
|---|---|---|---|
| Format only (a write function that copies to memory) | 10000 | 4.2 us (6.0, 18.3) | 1.9 us (2.6, 13.7) |
| File, `YTR_FLUSH_ROW` | 10000 | 15.2 us (26.5, 221) | 14.3 us (38.8, 570) |
| File, `YTR_FLUSH_SYNC` | 1000 | 1671 us (2456, 3160) | 1706 us (2798, 8377) |

An earlier probe of the bare calls (a 151-byte row, 3000 rows, gcc,
under the lock): `fwrite` into a stdio buffer 0.1 us, `fwrite` and
`fflush` 9.5 us (p99 36, max 154), and with `_commit` 1488 us (p99 2010,
max 8094). So the per-row flush costs about 10 us, the formatting about
4 us, and the sync to disk 1.5 to 8 ms. All of it is between trials.

## Verification and cost (v0.2.1)

All on Windows 11 with MinGW-w64 gcc 16.1 (C11, C99, C++17, warnings as
errors) and MSVC 19.44 (/W4 /WX, C and C++17), on Linux (WSL2) with gcc
11.4 as C11 and C++17 under AddressSanitizer and
UndefinedBehaviorSanitizer, and at `YTR_MAX_TRIALS` 256 and
`YTR_MAX_JITTERS` 1.

- Bit identity of the jitter draws: `tests/adapt/trials_jitter_golden.h`
  digests 200,000 draws of each of six kinds (uniform, choice and the
  truncated exponential, each continuous and snapped). The test checks
  them, so CI checks them on Linux (gcc, clang), macOS arm64, Windows MSVC
  and MinGW; CI also builds `tests/adapt/trials_jitter_repro.c` with
  -O3 -march=native (-mcpu=native on macOS), -Ofast and -Ofast native.
  By hand: MinGW gcc 16.1 at -O0, -O2, -O3 -march=native, -Ofast, -Ofast
  -march=native -ffp-contract=fast; MSVC 19.44 at /Od, /O2, /O2 /fp:fast,
  /O2 /arch:AVX2 /fp:fast; WSL gcc 11.4 at -O2, -O3 -march=native,
  -Ofast, -Ofast -march=native, with ASan and UBSan, and as C++ at -Ofast;
  each with the 128-bit types and with `-DYTR__NO_U128`: every digest
  the same. The Python binding computes the same six digests from
  `pt.splitmix` and `pt.jitter_map` in its tests.
- The fixed-point logarithm against mpmath: 0.71 x 2^-58 at worst over
  1,000,000 values; 123 points checked by the test within 1 unit of the
  rounded reference; the conversions (seconds to nanoseconds from the
  double's bits, frames to nanoseconds, the frames inside an interval)
  against exact values, including a frame at 0.5 ns that rounds outside.
- Bit identity of v0.2.0: `tests/adapt/trials_pins_v02.h` holds the
  hashes of 320 sessions (16 v0.2 designs x 20 seeds, through the v0.1 pin
  script: re-queues, marked breaks, records, a mid-session snapshot, the
  tallies and the format lines but the version token), generated by
  v0.2.0 (`trials_pin_gen.c -DPIN_V02`). They and the 600 v0.1.1 pins
  match on every build above. The v0.2 digest now leaves out the rules
  text's version line, and v0.2.0 and v0.2.1 give the same value.
- `tests/adapt/trials_test_jitter.h`: every refusal by name; values
  at fixed variates and frame ends (frames 7 and 29 for [0.07, 0.29] s at
  100 Hz); each distribution against its CDF on 200,000 draws (the
  snapped exponential on 1,000,000), worst chi-square at 0.80 of its
  99.9 % bound, and the truncated exponential's mean within 5 standard
  errors; 20,000 random intervals and rates whose snapped draws stay
  inside; `ns` against k x den x 1e9 / num in integer arithmetic; the draw
  order against a logged generator (open() draws none, each trial's own
  draws are those of the same session without jitters, then one per
  jitter; the pending trial draws none); `ytr_restore()` and
  save/load at cut points (format 3) reproduce every draw and data line;
  per-condition columns and their 9 refusals; the rules text round trip
  and 12 messages.
- Fuzzing: `tests/fuzz/trials_fuzz.c` now reads every jitter's draw
  each trial and seeds three jitter rule texts. MSVC 19.44 libFuzzer with
  AddressSanitizer, 1201 s from its 18 seeds with the first (libm)
  version: 9,003,538 inputs; then 601 s more on that corpus with the
  integer version: 4,221,162 inputs, 9353 coverage features, 2495 corpus
  files. No crash and no timeout; the corpus replayed on Linux gcc 11.4
  under ASan and UBSan, C11 and C++17, with no report.
- Mutations: 18 jitter mutants in `tests/mutate/trials.toml`, all killed
  (47 in the file, all as expected), among them both frame ends, the
  fold into the interval, the log series' sign, the ln 2 count, the
  rounding of seconds and of frames to nanoseconds, and the seconds as a
  product with 1e-9 instead of a division. j-10 (the data line without
  zero padding) survived its first run; the test now checks a 2 s draw
  prints as `2.000000000`. A mutant that skipped normalizing the frame
  scale to 63 bits survived as equivalent, and the normalization was
  removed (62 bits are kept).
- Python binding: 156 tests pass (4 for jitter: the session and its
  replay, columns, rules and errors, `jitter_map` against the maps
  written from their definitions, and the six golden digests). MEX: `test_mex.m` passes in
  MATLAB R2023a and Octave 10.1, the jitter part included.

Cost, measured on the Iris Xe laptop, AC, the measurement lock held,
2026-10-07, `examples/trials/bench.c`, 21 rounds:

| Measure | gcc 16.1 -O2 | MSVC 19.44 /O2 |
|---|---|---|
| `ytr_jitter_draw`, uniform snapped to 60000/1001 Hz, median | 66.0 ns | 53.8 ns |
| exponential snapped, median | 81.6 ns | 108.6 ns |
| exponential, median | 32.4 ns | 62.1 ns |
| choice of 4, median | 12.4 ns | 15.5 ns |
| next() + update(), units design with 5 % re-queues, mean / worst | 0.05 / 4.3 us | 0.05 / 3.0 us |
| the same with 4 jitters, mean / worst | 0.16 / 5.2 us | 0.20 / 4.8 us |

The draws include a splitmix step and the desc check that
`ytr_jitter_draw` repeats on every call; a session's draws skip it.
The snapped kinds pay for 128-bit divisions (the frames inside the
interval, then the frame's nanoseconds). Measured on the way: with the
divisions as a bit-by-bit loop the snapped draws took 580 to 680 ns
(gcc), and with MSVC's 128-bit product from 32-bit pieces the
exponential took 88 ns, so the header uses the compilers' 128-bit types
and `_umul128` / `_udiv128` where they exist. The first, libm version
took 47 / 25 ns (uniform snapped), 149 / 50 ns (exponential snapped),
118 / 34 ns (exponential) and 26 / 18 ns (choice). The larger handle costs a design
without jitters nothing measurable: the v0.2.0 bench, built against the
v0.2.0 header and against v0.2.1 (4 slots and 1) and run interleaved
twice under the lock, gave open() medians within 0.01 ms for the cheap
designs and within 0.2 ms (3 %, inside the run-to-run spread) for the
units design.

## Nanosecond jitters, open and close (v0.3.0)

v0.3.0 applies the repository conventions (README.md, Conventions):

- **Jitters are `int64_t` nanoseconds in C.** `ytr_jitter_desc` has
  `lo_ns`, `hi_ns`, `scale_ns` and `values_ns`, and `ytr_uniform()`,
  `ytr_choice()` and `ytr_exponential()` take nanoseconds
  (`YRT_MS(800)`). The names changed so that a v0.2.1 caller who writes
  `j.lo = 0.8` gets a compile error, not a duration truncated to 0. A
  bare `0.8` passed to a helper converts to 0, which MSVC (C4244) and gcc
  (`-Wfloat-conversion`, on in the CMake build) warn about.
  `ytr_jitter_value` lost `.s`: a person reads `YRT_TO_S(v.ns)`.
- **Seconds stay where people write them.** The rules text (`jitter iti
  uniform 0.8 1.2`), the table columns and the Python and MATLAB bindings
  keep seconds. They convert with the function v0.2.1 used for every
  duration (to the nearest nanosecond from the double's bits, ties up),
  so a v0.2.1 desc and its v0.3.0 translation draw the same nanoseconds.
  The draw itself was already integer arithmetic on nanoseconds; only the
  boundary moved.
- **Snapshot format 4.** The desc section of a session with jitters
  stores the durations as i64 nanoseconds where format 3 stored f64
  seconds. A format 3 snapshot is refused by its number. This breaks
  resume across the upgrade: a session saved by v0.2.x cannot be resumed
  by v0.3.0. Finish or restart a running study before you upgrade, or
  rebuild the session from its outcomes and seed with `ytr_restore`.
- **`format_rules()`** writes a duration as the exact decimal of its
  nanoseconds with trailing zeros dropped (`0.8`, `2`), where v0.2.1 wrote
  the shortest decimal of the double. A value with more than nine decimals
  (`0.333333333333`) now prints as the nanoseconds it is
  (`0.333333333`); both read back to the same session.
- **Open and close.** `ytr_load()` is `ytr_open_saved()`, and
  `ytr_close()` zeroes the handle (it owns no memory). The loop calls
  (`ytr_next()`, `ytr_update()`, `ytr_mark_break()`, `ytr_requeue()`,
  `ytr_restore()`) write `ytr_error()` when they fail; `YTR_DONE` is not a
  failure. `ytr_jitter_check()` returns 0 or `YTR_ERR_ARG`.

What was checked, on 2026-10-10 (Windows 11, MSVC 19.44 /W4 /WX as C and
C++17, and MinGW-w64 gcc 16.1 -Werror): the six golden digests of 200,000
draws each (`tests/adapt/trials_jitter_golden.h`) are unchanged, and so are
the 600 v0.1 pins and the 320 v0.2 pins. The jitter tests keep their
v0.2.1 numbers in seconds and convert them with the header's own
function, so every expected draw is the one v0.2.1 checked. The Python
binding's 157 tests pass unchanged (its `.s` is the correctly rounded
`ns / 1e9`, as the header's was), and so does `bindings/mex/test_mex.m`
under Octave 10.1 (MATLAB not run).

## Verification (v0.1)

The header's STATUS block has the numbers. In summary:

- Compile checks as C99, C11 and C++17 with warnings as errors, and MSVC
  /W4 /WX in its default C dialect and as C++17.
- `tests/adapt/trials_test.c`, at `YTR_MAX_TRIALS` 256 and at the
  default 4096: the factorial index round trip; each order's properties
  over 400 seeds; each constraint rule holding on every repaired order of
  eight designs x 200 seeds, by a checker written separately from the
  header's, and the repair's failure message on an impossible set; tracks
  interleaved with `track_rate` at 0 (rejected with conditions), 0.5 and
  1, checking the mix and that a finished track is never picked; practice
  and warmup flagged and untallied; re-queues with and without a gap, and
  760 random re-queues under a no-repeat rule with no unflagged violation;
  `ytr_mark_break` ending a run; tallies against a hand count;
  `ytr_restore` reproducing a run bit for bit, with and without tracks;
  `ytr_save` at 42 cut points then `ytr_open_saved` into a garbage-filled
  handle, identical to the uninterrupted run, snapshot bytes included;
  every load refusal and every rejected desc; the format functions against
  fixed strings.
- `examples/trials/mocs.c` (constant stimuli with a constraint, printing
  the proportions and a CSV) and `examples/trials/interleave.c` (three
  ysp/stair.h staircases and a catch condition, the second USAGE example
  run for real, printing which track each trial went to). Both exit 0
  with no hardware, and print the same bytes under gcc and MSVC.
- `tests/compare/compare_trials_psychopy.py` runs `ysp.trials` beside
  `psychopy.data.TrialHandler` and `TrialHandlerExt` on the same condition
  lists over 100 seeds: SEQUENTIAL is identical to PsychoPy's order, RANDOM
  holds each condition once per repetition block on both sides, and
  FULL_RANDOM, weighted or not, gives exact counts on both sides.

Not done: a run on macOS or on big-endian hardware, and a load of a
snapshot written by another compiler. The snapshot's portability is by
construction, not by test.
