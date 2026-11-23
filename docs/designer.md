# The designer

Status: the skeleton is built (step 3 of "Order of work", 2026-11-18, branch
`designer-skeleton`): a window, open and save of a project, the player's
check on each change with its refusals linked to their places in the
text, and redraw on input only. Step 4 is partly done (2026-11-19,
branch `designer-step4`): the state chart, the tables and the property
panels are built and merged, and the items left are in "The
integration". Step 5 is built on branch `step5-int2`. Step 6 is
built on branch `step6-int` ("Parts B, G, R and V integrated"), but for
part M, the measurement with the user on the display; step 7 is not
started. This document
gives the designer's decisions, its first version's scope, its architecture
and its order of work. [rig_spec.md](rig_spec.md) section 7 gives the
designer's place in the rig. The measurements behind the UI decision are
in [designer_ui_probe.md](designer_ui_probe.md).

The designer is a program that edits an experiment definition and its
tables, checks them with the player's own loader, shows a preview, and
builds the pack. Most definitions are written in it
([experiment_format.md](experiment_format.md) 1.3), and authoring of
continuous-input tasks in it is the baseline that format version 2 and
`ysp/sig.h` are judged by.

## Decisions

**UI: Dear ImGui on SDL3, native and WebAssembly. DECIDED (2026-11-18).**
- Evidence: [designer_ui_probe.md](designer_ui_probe.md). On the Linux
  desktop, with a state chart, a 600,000-point trace, a 10,000-row table
  and a `ysp/gfx.h` preview in one window, the UI took 1.2 to 2.1 ms a
  frame natively and in Chromium and Firefox, and the frame interval
  stayed at the display's period. Idle cost was 0.00 % of one core
  natively with redraw on input only. `ysp/gfx.h` draws on ImGui's GL
  context with no change to the header.
- Decision: one C++ program, built per platform and with emscripten from
  the same source. It needs a C++ compiler and no C++ runtime
  ([imgui_probe.md](imgui_probe.md), finding 6). The headers stay C.
- Rejected: a TypeScript web app. It gets text editing and accessibility
  from the browser, but it needs the player in WebAssembly for every
  preview, a second toolchain (Node, a bundler), and custom canvas code
  for the chart, the trace and the timeline. A native webview was
  rejected too: three browser engines to test, and nothing that a
  browser tab and the runner protocol do not give.

**User text: Skribidi. DECIDED (2026-11-18).**
- Evidence: ImGui draws one glyph per codepoint, left to right, with no
  shaping. The repo's pinned Skribidi in ImGui showed joined Arabic,
  Devanagari conjuncts and mixed lines in bidirectional order, in an edit
  field, a table column and the state names, natively and in both
  browsers (probe part 2).
- Decision: all user text (names, table cells, edit fields, labels from
  the definition) is laid out and edited with Skribidi (`skb_editor`).
  ImGui's own fonts draw only the designer's own UI text. The designer,
  the pack tool and the player use the same Skribidi commit, so text is
  the same in all three.

**Accessibility: deferred. DECIDED (2026-11-18).**
- Decision: not in the first version. Every widget keeps a stable ImGui
  ID, so an accessibility tree can be added later. AccessKit is the
  native path; when it is built, it is the accepted exception to the
  rule of no Rust dependencies. AccessKit has no web adapter, so the
  browser build would need a hidden DOM tree of its own.

**Format versions.** The designer writes format version 2 only. It opens
a version 1 definition by the one-step migration of
[experiment_format.md](experiment_format.md) 3.2 (D12: migration happens
only in the designer), and saves version 2.

### Decisions of step 3 (2026-11-18)

These were chosen while the skeleton was built. Each can change when a
later step needs it.

**The project folder.** A project is a folder with a pack source
description ([pack.md](pack.md) 5.2), normally `pack.json`, the definition
that its `experiment.source` names, and the other sources that it lists.
The folders `tests/player/rdk/` and `tests/player/stroop/` are projects.
The designer adds one folder, `.designer/`:

| Path | What | Keep it? |
|---|---|---|
| `.designer/view.json` | The view state (D13): the caret's line and column, the editor's scroll, ImGui's window settings. Canonical JSON, `"format": "ysp-designer-view"`, version 1. | Optional. A missing or bad file gives an empty view; open notes a bad one and goes on. |
| `.designer/check/experiment.json` | The text of the last check pack build. | No: a cache. |
| `.designer/check/check.ysppak` | The check pack (below). | No: a cache. |

- Why one folder: deleting it loses the view and a cache, never the
  experiment. A project under git can ignore it.
- Open takes the folder or its description file. New makes a folder with
  `pack.json` and `experiment.json`, a version 2 definition with one trial
  that checks clean; it does not overwrite either file.

**The check pack.** `pl_check()` needs a pack for the tables, fonts and
other entries. The designer builds one with the linked pack tool, from the
project's description with the definition replaced by the editor's text,
and keeps it in memory.
- It builds the pack at the first check whose text `yexp_load()` reads,
  and again when the set of texts that the definition's text stimuli can
  show changes (`ypt_derive_texts()`). The glyph runs are the only entries
  that the definition decides; any other change is checked against the
  pack in memory. A failed build is not tried again for the same text.
- Until a pack exists, a check gives `yexp_load()`'s diagnostics with no
  host and a note that the other checks wait.
- One implementation of each header, with SDL and miniaudio.
  `ysp_designer_core` links `ysp_player_lib`, `ysp_pack_tool_code` and
  `ysp_player_impl` (`player/impl.c`). It does not link `ysp_pack_impl`
  (`pack/pack_impl.c`), the pack tool's copy of the headers that it
  shares with the player, compiled without SDL and miniaudio for ypak.
  `ysp_pack_tool_code` has the implementations of the headers that only
  the tool uses (`pack/tool_impl.c`: ysp/video.h, ysp/outline.h), so the
  designer's pack tool is ypak's but for the SDL and miniaudio backends,
  which the tool does not call. The headers' declarations do not change
  with `YSCR_NO_SDL` or `YAU_NO_MINIAUDIO`. A header in `pack_impl.c` must
  also be in `player/impl.c` ([pack.md](pack.md) 5.1); the designer's link
  fails if it is not.
- Until 2026-11-19 the designer compiled both libraries' sources again
  against its own implementation file (`designer/core/engine_impl.c`).
  Each new source file of either library then also had to go in the
  designer's list. That file and the lists are gone.

**Evidence: the check's cost.** `bench_designer_check`
(`tests/designer/check_bench.c`) times the real code per edit. It compiles
`check.c` into itself to reach `build()` and `texts_key()`, so it changes
no designer code.
- Conditions: Linux desktop, Intel Core i5-14500, Linux 6.11.5 (Liquorix),
  gcc 12.2, Release, pinned to CPU 2 (a P-core, governor `performance`),
  2026-11-20. 30 rounds of each part that builds, 200 of the others.
  `bench_designer_check WORK 30 200`, four passes, one at a time under the
  measuring lock. Load average (1 min) 0.13 before the first pass, 0.97
  after the last. The table gives the pass on the home folder
  (`~/.cache`, ext4 on NVMe, `/dev/nvme0n1p2`). `/tmp` is on the same
  ext4 file system; in its two passes the medians of the builds and the
  checks were within 1 % of these.
- Projects: the six of `tests/player/` (only stroop has text stimuli), and
  two that the bench generates. "many": 200 text stimuli, 50 each in
  Latin, Hebrew, Arabic and Devanagari, each in its script's Noto Sans
  with a curve set, and 10,000 table rows that the timeline runs. The
  player refuses a node's table above `YTR_MAX_CONDITIONS` (1,024) rows,
  so the rows are 10 tables of 1,000, one per node. "many+words": the
  same, and one more stimulus shows the tables' word column, which gives
  10,000 more texts.
- Parts: "Build" is `ypt_build_text()` with the description made from the
  project. The pack tool writes the pack while it builds, so "Write" is
  `dsg_write_file()` of the same bytes alone, and "Read" is the read-back.
  "Same texts" is `dsg_check()` after a keystroke that changes no text
  (no build). "Text changed" is `dsg_check()` after a keystroke in a text
  literal (one build). "First check" is `dsg_check()` with no pack (at
  open).

Milliseconds: median, p95, max ("Write", "Read" and "First check":
median).

| Project | Pack | Texts | Build | Write; Read | `texts_key()` | Same texts | Text changed | First check |
|---|---|---|---|---|---|---|---|---|
| stroop | 599 KB | 9 | 6.36, 6.40, 6.41 | 0.20; 0.03 | 0.04, 0.04, 0.07 | 0.47, 0.49, 0.78 | 7.10, 7.25, 7.26 | 7.07 |
| oddball | 25 KB | 0 | 0.14, 0.15, 0.15 | 0.02; 0.002 | 0.02, 0.02, 0.03 | 0.26, 0.27, 0.38 | none | 0.51 |
| rdk | 17 KB | 0 | 0.09, 0.10, 0.10 | 0.02; 0.002 | 0.01, 0.01, 0.02 | 0.22, 0.24, 0.35 | none | 0.40 |
| bimanual | 21 KB | 0 | 0.36, 0.37, 0.37 | 0.02; 0.002 | 0.02, 0.02, 0.03 | 9.12, 9.41, 12.92 | none | 9.84 |
| visuomotor | 21 KB | 0 | 0.27, 0.29, 0.29 | 0.02; 0.002 | 0.02, 0.02, 0.05 | 1.61, 1.74, 1.93 | none | 2.20 |
| afc2 | 13 KB | 0 | 0.10, 0.11, 0.11 | 0.02; 0.001 | 0.01, 0.01, 0.02 | 1.56, 1.59, 1.68 | none | 1.80 |
| many | 2.5 MB | 200 | 33.9, 34.0, 34.1 | 0.80; 0.14 | 0.74, 0.76, 0.79 | 5.26, 5.30, 5.94 | 40.8, 41.5, 41.6 | 40.9 |
| many+words | 12.6 MB | 10,200 | 143.2, 144.3, 162.6 | 4.15; 1.04 | 19.5, 19.6, 19.7 | 37.2, 37.8, 39.4 | 201.2, 204.1, 207.5 | 183.2 |

- In the repository's projects the check fits a 16 ms frame. The largest
  cost per keystroke is bimanual's `pl_check()` (9.1 ms median, 12.9 ms
  max), not the pack. A keystroke in a stroop text costs 7.1 ms, of which
  the build is 6.4 ms.
- In the generated projects it does not fit. A keystroke in a text costs
  41 ms ("many") and 201 ms ("many+words"). In "many+words" each other
  keystroke costs 37 ms: `texts_key()` derives the 10,000 table texts
  again on each check (19.5 ms), and the rest (17.7 ms) is `pl_check()`
  with a 12.6 MB pack.
- The file system is not the cost. The pack's write and read-back are
  2.6 % of a text keystroke in "many+words" (5.2 ms of 201 ms). On tmpfs
  (`/dev/shm`) the build was 1 % to 2 % faster than on ext4. The time is
  in the pack tool's layout and curve sets.
- Step 5, part T, moved the check to a thread of its own. The UI frame
  now pays only the post and the take of a result: at most 25 us a
  keystroke on bimanual and "many". The check's latency did not change
  ("Plan of step 5", "Part T as built").

**A refusal's link, while the definition is text.** Step 3 edits the
definition as JSON text in a multi-line Skribidi editor. Each issue has a
line and a column in that text; a click on the issue puts the caret
there.
- Tier 1 (`yexp_load()`) and the pack tool give a line and a column.
- Tier 2 gives an object (`PL_OBJ_*`) and its name. The designer finds its
  key: `stimuli`, `trials`, `methods`, `roles` or `session.fields` by
  name; a node by its path, name by name down `timeline` (a node with no
  name matches an empty part). If the key is not there, the link goes to
  the set's key, then to the parent node. This reimplements no rule: it
  only finds a name in the text.
- Tier 3 (what a real run needs) links to the key that asks for it:
  `display.calibration`, `display.units`, `display.min_tier`, the role in
  `devices` or `roles`, the session field. The default `participant` field
  has no key, so it has no link.
- Step 4 replaces the text with panels; the same object and name then
  select the panel's field.

**Save.** Save writes the definition's file through a temporary file.
- When `yexp_load()` reads the text, Save writes `yexp_migrated()`'s bytes:
  canonical, format version 2 (D2, D12). The editor then shows those
  bytes, with the caret at the same line and column.
- When it does not, Save writes the text as typed and says so: no work is
  lost, and the next Save of a fixed text writes canonical bytes.
- A version 1 file opens as its migrated version 2 bytes. It shows as
  changed until Save.
- Opening another project or quitting with changes asks: Save, Discard or
  Cancel.

**The file dialog's fallback: a typed path.** Open... calls
`SDL_ShowOpenFolderDialog()`. Where it fails (a Linux desktop with no
xdg-desktop-portal and no zenity, probe part 1, section 6), the status line
says so and the path field takes the keyboard: type the folder and press
Enter. A path on the command line and a folder or file dropped on the
window also open a project. The dialog's answer comes back through SDL's
event queue, because SDL can call the callback on another thread.

**The redraw rule.** The loop blocks in `SDL_WaitEvent()` and draws only
when one of these is true:
- An event came in the last `DSG_SETTLE_FRAMES` (3) frames: ImGui settles
  hover and focus over two frames.
- ImGui's input queue holds events: ImGui applies one key or button change
  a frame.
- Work waits for a frame: a check to post, a scroll to restore, a
  question to open. A check that runs on the check thread is not work for
  a frame: its result wakes the loop (step 5, "Part T as built").
- A panel animates (none in step 3).

Through step 4, the check ran in the frame of the edit, after the editor
and before the issues panel. Since step 5 (part T) it runs on the check
thread: a frame that changes the document posts a check job at its end,
and the frame after the thread's wake-up takes the result at its start.
So the issues show one or more frames after the edit. The caret does not
blink: a blinking caret is an animation.
The rule is `dsg_redraw_*()` in `designer/core/redraw.c`.

### Decisions of step 4

#### The panels

Built 2026-11-19 (branch `designer-step4-panels`, part C of "Plan of step
4"). The side column, `Properties`, shows the keys of the selected object.
The code is `designer/core/schema.c` (C, no window: the fields, the object
at a path, the writes, the renames) and `designer/app/view_panels.cpp`
(the rows).

**What shows.** The selection is a JSON Pointer. `dsg_schema_chain()` finds
the object that it is in, and the objects that hold that object as far as
its template. An exit shows three panels: the exit, its state and its
template. A stimulus shows one panel. Each panel is a collapsing header
with the object kind, its name and its kind ("stimulus dots (rdk)").

| Path | Object | Section of `yexp_defaults(2)` |
|---|---|---|
| `""` | the file | file |
| `/display`, `/session`, `/session/fields/F`, `/web` | display, session, session field, web | the same names |
| `/devices/R`, `/roles/R` | event role, signal role | role, signal role |
| `/stimuli/S` | stimulus | stimulus, kind from `kind` |
| `/methods/M` | method | stair, quest or aep method, from `type` |
| `/controls/C`, `/trials/T/controls/C` | control | control, kind from `control` |
| `/trials/T` | template | template; kind 2 with `states` and no `key_answer`, else 1 |
| `/trials/T/response`, `.../trigger`, `.../choices/I` | response, response trigger, choice | response, or response with signal choices; a signal choice has no keys |
| `/trials/T/states/I`, `.../over`, `.../exits/J` | state, over, exit | state, over, exit (kind 1) |
| `/trials/T/exits/J` | shared exit | exit (kind 2) |
| `.../events/I`, `.../after/I`, `.../steps/I` | step | step, kind from the key that names the op |
| `/trials/T/columns/N` | data column | none: a value |
| `/timeline/I` (and nested), `.../loop` | node, loop | node, loop |
| `.../vars/N`, `.../data/N` | variable, node data column | uniform, walk2d (its keys under `walk2d`), else a value |

- A path under an object names one of its keys (its focus). The row of
  that key has a highlight, and a reveal (a click on an issue, a selection
  in the chart) scrolls to it and opens the panel.
- Every key that `yexp_defaults(2)` has for the section and the kind shows,
  in the table's order. A stimulus's `visible` does not: in format 2 it is a
  step's property (D41). A state's and a node's `name` is the name row.
- A key that is not in the text shows its default in grey. Reset removes
  the key. The commit writes a key at its default, and Save drops it (D2),
  as "The model: the bytes are the truth" says.

**Units, ranges and text from the headers (G6).** `schema.c` joins each
format row to a header's row by name, at the first call:

| Section | Table |
|---|---|
| stimulus | `yrdk_params()` for an rdk, then `ygfx_params()` and `ygfx_enum_params()`; `color` takes `color_r`'s row |
| response, response with signal choices, choice | `yrsp_params()`: `duration_s` is `duration_ns`, `minimum_valid_rt_s` is `min_rt_ns`, `response_ends_trial` is the negation of `persist`, a choice's keys are `choices.*` |
| control | `ysig_op_params()` of the op that the key lowers to (format 3.7): a region's `radius` and `speed_below` are `detect`'s `level`, `stay_s` its `hold`, `rate` its `at`, `speed_lag_s` and a peak's `lag_s` are `speed`'s `lag`, a moving target's `ramp_s` is `sines`'s `ramp`, a trace's `rate` is `record`'s `per`, signal_lost's `after_s` is `silent`'s `after`, a distance's `scale` is `scale`'s `k.x`, a cursor's `transform` is `affine`'s `angle`; a movement's `onset` is `yrsp_params()`'s `choices.level` |
| stair, quest, aep method | `yst_params()`, `yqst_params()`, `yaep_params()` (`hyperparams.mean` is a key with a dot, as the loader reads it) |

- A format row whose kinds take different header rows is split into one
  field per group of kinds (`rate`: a region's and a trace's).
- A duration's unit is `s` whatever the header's unit is (D4); a range in
  ns is converted to seconds.
- The tooltip of a key gives its text, the range, the default, and the
  source. The text is the format row's (ysp/experiment.h 0.6.0, P2); the
  row's text is empty only for a stimulus or method key that a header's
  table has, and then the header's text shows. A key that only the format
  has (a node's `repetitions`, a state's `max_visits`) takes the row's unit
  too. A gfx row joins only the kinds it reads, so a sound's `offset` is
  the format's, not a shape's.

**Widgets.** By the row's type, when the value has that type:

| Type | Widget |
|---|---|
| number, integer, duration | ImGui's text field (a number is ASCII). Drag the key's name to change the value |
| bool | a checkbox |
| enum | a combo of the row's values; `(none)` for an optional key with no default |
| a name (`to`, `on`, `of`, `from`, `trial`, `role`, ...) | a combo of the names that the key can take there (format 3.9): states and `end` for an exit's `to`, controls with hits and `response` for its `on`, roles with their lanes (`left.x`) for a cursor's `x`, the templates for a node's `trial` |
| text, string, resource | a Skribidi field (`skf.cpp`): user text in any script |
| an object with a panel of its own (`response`, `loop`, `over`, `display`) | Select, or Add when absent |
| a set or a list of objects (`states`, `stimuli`, `timeline`) | the count; the chart and the text view edit them |
| any other list, map or form, and a value that does not fit its widget (a variable, a lookup) | a Skribidi field with the value's compact JSON. Text opens the text view at the key |

A Skribidi field of a row out of view is not made: the row keeps its
place and its ID, so a scroll or the Test Engine still reaches it (an aep
method has 83 keys). At most 48 fields show at once.

**Writes.** `dsg_schema_write()` makes one undo step "Set KEY":
- A duration: the typed decimal through `yjs_parse_fixed(..., 9)`, written
  with `dsg_def_set_seconds_ns()`. `0.50` is `0.5`; `1.0000000004` is `1`.
- A number: `dsg_def_set_number_text()`, so the canonical writer spells it
  (`5e-1` is `0.5`). An integer: `dsg_def_set_int()`.
- Text that starts with `$` is a variable, and text that starts with `{`
  or `[` is JSON, for every type that is not text. Text keeps its spaces.
  A field that showed JSON also reads `"..."` as JSON.
- A choice that is a string (`"left"`) becomes `{"key": "left"}` before a
  second key goes in.
- A value out of a header's range is written. The check reports it: the
  loader names the key and the header's range at its place (a coherence of
  1.5: "outside ysp/rdk.h's 0 to 1"). The panels validate nothing.
- Text that does not read as the type (`abc` for a number, `1 s` for a
  duration) writes nothing, and the status line says why.

**Commits.**
- Each edit is queued while the panels draw and made after them: an edit
  parses the tree again, and the rows of the frame read the tree they were
  drawn from.
- A text field commits on Enter, when it loses the focus, and when the
  selection changes. Escape gives the field its value back. While a field
  has the keyboard, Ctrl+Z is the field's own undo (Skribidi's or
  ImGui's); after the commit, Ctrl+Z takes the whole edit back from the
  document's stack.
- A drag of a number's name shows the value as it moves and writes it when
  the mouse button comes up: one step and one check for the drag. The
  value moves by the header's range over 500 px, or by 0.5 % of the value
  when the range is open, and keeps the step's decimals.
- When the selection changes, every field with typed text writes it and
  lets the keyboard go, before the frame reads the tree. A field that kept
  the focus would take the next Ctrl+Z from the document's stack.

**Renames.** The name row of a set member (a stimulus, a control, a role, a
template, a method, a session field, a variable, a column), a state and a
node renames it in one step "Rename NAME" with `dsg_schema_rename()`:
- It changes the keys whose schema type names that kind of thing (format
  3.9): steps' `on`, `off`, `set`, `to`, `then`; controls' `of`, `x`, `y`,
  `mean`, `stimulus`, `freeze_at`, `from`, `to`; exits' `on`, `to`, `in`;
  `over`'s `to`; responses' `role` and `from`; nodes' `trial` and `tracks`;
  and the forms `{"signal": N}`, `{"when": N}`, `{"hit": "N.x"}` and
  `{"time": N, "from": M}`. A lane stays (`left.x` becomes `hand_l.x`).
- A state and a template's control are renamed within their template; the
  other names are global (3.9). A state's rename tells the rename listeners,
  so the chart's position follows it.
- A variable's `"$NAME"` changes only when no other node defines NAME:
  another node's references are not this one's.
- The check reports what the walk does not find. A `walk2d` variable `aim`
  defines `$aim_x` and four more names; renaming `aim` changes none of
  them, and the check names `$aim_x`.
- The selection follows a set member to its new key in the same step.

**Issues.** After each check, a key whose path holds an issue's value
(`ui_issue_path()`) has a mark beside its name, colored by severity. Its
tooltip is the message; a click goes to the issue, as a click in the
issues panel does. An issue at the object itself (a tier 2 refusal that
names a stimulus) marks the panel.

**The view file.** The part `panels` is `{"collapsed": [KIND, ...]}`: the
object kinds whose panels are collapsed ("stimulus", "template"). A
selection keeps a collapsed panel collapsed; a reveal opens it.

**The Skribidi field** (`skf.h`, `skf.cpp`; the existing API did not change):
- One field has the keyboard. Before, a click in a second field kept the
  first one focused too (a click on an item keeps the focus), and the first
  one drawn took the characters: the definition editor before the panels.
- `skf_take_events()` gives a one-line field's Enter and Escape and the
  loss of its focus; `skf_blur()` lets the keyboard go.
- A one-line field lays its text out at the width it is drawn at. It makes
  its editor again when the width changes, because `skb_editor_reset()`
  with new params copies the attributes from the editor's own array while
  it grows that array. Skribidi's undo of that field starts again then.

**Measured.** Linux desktop, Intel Core i5-14500, gcc 12.2, Release,
pinned to CPU 2 (a P-core), load average 0.87 during the runs (`uptime`),
two runs within 3 % at the median. Microseconds, median and p99.

| Work | bimanual (5.7 KB) | its templates copied to 100 KB | to 1 MB |
|---|---|---|---|
| A frame's schema work for an exit: the chain, each object's fields, each value | 2.5, 2.8 | 2.5, 4.8 | 2.8, 11.5 |
| The same for a stimulus | 3.2, 3.5 | 3.2, 4.5 | 3.2, 6.2 |
| The candidates of an exit's `on` (a combo opens) | 0.2, 0.5 | 0.2, 1.8 | 0.2, 1.8 |
| Rename a stimulus: the whole file (4, 72, 732 references) | 80.5, 87.8 | 907.8, 963.5 | 8,612.8, 9,041.0 |
| Rename a state: its template (3 references) | 42.5, 49.5 | 328.8, 359.5 | 3,000.5, 3,062.5 |

| A whole frame of the UI, with no window (1400 x 900, the rdk project, the text view shown), 2,000 frames | Median | p99 |
|---|---|---|
| Nothing selected | 31.6 | 34.2 |
| The file | 41.5 | 43.7 |
| Stimulus `dots` (rdk) | 52.4 | 58.4 |
| Template `rdk` | 42.6 | 48.5 |
| An aep method (83 keys) | 68.2 | 75.5 |

- The panels cost 10 to 37 us of a drawn frame. The designer draws on
  input only, and the check after an edit costs 0.2 to 7 ms.
- A rename at 1 MB is under one 16 ms frame. Its cost is the commit's write
  of the whole text (3.7 ms in "The model: the bytes are the truth") and
  732 sets. The first walk found each value's object again from the root:
  51 ms at 1 MB, in one run with no pinning. The walk now goes down the
  objects with their sections and kinds (8.6 ms).
- `test_designer_panels WORK --bench` gives the first table. The second
  came from a harness in the session's scratch folder that draws frames as
  `test_designer_ui` does; it is not kept.

**Tests.** `test_designer_panels` (209 checks) and nine tests on the
Test Engine (`te_panels.cpp`):

| Test | What it checks | Hand mutants killed |
|---|---|---|
| `test_designer_panels` 1, 2 | The rows' types and flags (every `_s` key is a duration in seconds, every row with values is an enum, the flags); every row's kinds covered once by the split fields; the joins: every stimulus and method key that a header's table has takes its row, and the response, choice and control joins; every field has a text; a format-only key takes the row's unit and text (0.6.0) | no gfx join; no join of a trace's `rate`; no split; the row's text not used; the row's unit not used; a gfx row joined to every kind; the header's text first |
| 3, 4 | The object at a path for each object kind and each kind of stimulus, control, method, step, exit, template and variable; the chain; every key of each section and kind shows once for its object | a step's property shown; `walk2d`'s keys at the variable; the template's kind always 1 |
| 5, 6 | Each type writes the canonical value in one step; bad text writes nothing; Reset; a string choice; a value out of range written and named by the check at its key | a duration written as typed; text trimmed; Reset that keeps the key; a string choice kept; a number as text; a write clamped to the range |
| 7, 8 | Candidates; renames of a state, a stimulus, controls, a role, a template, variables and a node, each one step, the check clean after each, undo to the same bytes, the listener; a shared variable; a state in one of two templates; refusals | lanes dropped; no forms in `props`; a state's rename global; a variable counted twice; a shared variable renamed; candidates not filtered |
| `panels/follows_text`, `contrast`, `issue_mark`, `hebrew`, `long_panel`, `drag`, `select_commits`, `rename`, `collapsed` | The caret selects the panel; a click in a field takes the keyboard from the editor; a contrast typed over its lookup and a number typed in a number field, each taken back by Ctrl+Z; Escape; Reset; an out-of-range value's mark and its jump; Hebrew typed with the engine's keys; Ctrl+Z in a focused field; the last keys of an aep method's 83; a drag over six frames as one step and one check; a commit on a selection change within one object; a rename through the name row with the selection; the collapsed part through a reopen, and a reveal | focus not exclusive; Enter ignored; Escape commits; Escape ignored; no marks; no commit on a selection change; a drag that commits each frame; no view part; the selection not following a rename; a reveal that does not open; a number field not committed; Ctrl+Z in a field going to the document; a row out of view with no ID |

31 of 31 hand mutants failed a test (18 of the core, 13 of the UI) and
were reverted; they are not in a mutation list. Without mutants, every
test passed with `YSP_DESIGNER_TEST_ENGINE` ON and OFF (Linux, gcc 12.2,
Release).

**Not done.**
- The window was not opened: no person used the panels. The Skribidi
  field's width change was not looked at.
- The Windows (MSVC) build was not compiled. The code was compiled with
  gcc 12's `-Wconversion` and gave no warning.
- No ASan or UBSan run.
- Variables have no candidates: `if` and `unless` are text fields.
- The panels add and remove no list items (a state, a step, an exit) and
  no set members.
- `view_panels.cpp` calls `ui_show_view()`, `ui_editor()` and
  `ui_jump_to_issue()` from `ui.h` (to open the text view at a key and to
  go to an issue). `views.h` did not list them for a view; it does since
  the integration.
- Prerequisites P1 and P2 did not block anything. Both are made in
  ysp/experiment.h 0.6.0 (2026-11-20): `schema.h` uses the header's
  `YEXP_T_*` and `YEXP_R_*` with no mirror, and a key that only the format
  has shows the row's unit and text.

#### The chart

Built 2026-11-19 (part A, branch `designer-step4-chart`). The chart view
(`designer/app/view_chart.cpp`) shows the states of one template and edits
them through the document. The model, the commands and the view state are
in `designer/core/chart.h` and `chart.c`. They need no window, so
`test_designer_chart` tests them headless.

**What the chart shows.** The list above the chart picks the template. By
default it is the first template with `states`, in byte order.

| Item | How it shows |
|---|---|
| A state | A box with its name (Skribidi), its number of steps and exits, and `max_visits` when it is set. An arrow points into the box of the first state: the trial starts there. |
| The end | A circle with the name `end`. |
| An exit | A solid arrow from its state to its `to`. The label tells what the exit waits for: `on X`, `after T s`, `instant`, `if C`, `outcome O`. |
| A shared exit | A blue arrow from each state in its `in` to its `to`, with the same label. |
| The next state (format 3.11 rule 10) | A dashed gray arrow with the label `steps end`. It goes from a state that has no exits and is in no `in` of a shared exit, to the next state in the list, or to the end from the last state. This is the player's rule (`player/v2.c`): a state in a shared exit has exits. |
| `over` | A purple arrow with the label `over N visits`. |
| A `to` that names no state | A short red arrow to nowhere. |
| An issue | A red border (error) or a yellow border (warning) on the box or the arrow that `ui_issue_path()` puts it in. A badge on the box shows the number of issues in the state. |
| The selection | A yellow border or arrow. |
| The state that the preview runs (step 5) | A green ring outside the box, or around the end. An events template rings each of its parts while its events run, and the end while `after` runs ("Part C as built"). |

Two arrows between the same two nodes bow to opposite sides. An arrow
from a state to the same state is a loop above the box.

A template that uses `events` shows read-only, with a blue banner across
the view that says so. Its boxes are
the parts that `until` splits it into (format 3.11 rule 9): the steps
before an `until`, the wait, and the steps after it. To edit such a
template, use the text view.

**What a person does.**

| To | Do this |
|---|---|
| Select a state, an exit or the template | Click the box, the arrow or its label, or empty space. The side column shows the panel of the selection. |
| Move a state or the end | Drag its box. |
| Pan | Drag empty space with the left or the middle button. |
| Zoom | Turn the mouse wheel over the chart. The point under the mouse stays where it is. |
| Connect | Drag from the round port on the right edge of a box to a box (the same box too) or to the end. |
| Rename | Double-click the box, press F2, or click Rename. Type the name, then press Enter or click OK. Escape or Cancel stops the rename. |
| Delete | Press Delete, or click Delete. |
| Set the first state | Click Set first. |
| Add a state | Click Add state. The box goes to the middle of the chart, with the name `state`, `state2` and so on. |
| Show the template that the preview runs (step 5) | Select Follow. A pick in the list, or a jump to another template, clears it. |
| Preview the template shown (step 5) | Click Preview this template. The preview pins the first leaf of the timeline that runs it, any row, and the pane shows. |

**The commands.** Each command is one transaction, so one undo step. Each
command selects what it made or changed, so undo and redo put the
selection back.

| Command | What it changes | What it selects |
|---|---|---|
| Add state | Appends `{"name": N}`. Makes `states` when the template has neither `states` nor `events`. | the state |
| Connect | Appends the exit `{"to": T}` to the state. | the exit |
| Delete state | Removes the state; each exit and shared exit whose `to` is the state; the name of the state in each `in` (and a shared exit whose `in` becomes empty); `over` and its `max_visits` where `over` goes to the state; and each `exits` array that becomes empty. | the template |
| Delete exit | Removes the exit, the shared exit, or `over` and its `max_visits`, and an `exits` array that becomes empty. | the template |
| Rename state | Sets the name, and each reference to it in the template: the `to` of an exit, a shared exit and `over`; the `in` of a shared exit; and the `from`, `to` and `time` of the controls and the columns of the template (format 3.9). Notes the rename for the view state. | the state |
| Set first | Moves the state to the start of `states`. | the state |

- **Connect makes an instant exit.** An exit with no `on` and no
  `after_s` is taken when the state starts. No default for `on` or
  `after_s` is correct for every chart, and an instant exit is valid. The
  label says `instant` until a panel gives the exit its condition.
- **Why a rename changes references.** `dsg_def_rename()` changes a key,
  not the names that point to it. A template has one namespace (format
  3.9), so a string at those keys that is equal to the old name can only
  be the state. The bimanual definition shows why the controls count: its
  controls `goal` and `frames` start `from` the state `move`. The rename
  does not change the top-level controls, because they serve each
  template. The check reports a reference that the rename did not change.
- **Names that the chart refuses.** An empty name, `end`, `response`, and
  the name of another state of the template. Two states with one name
  would share their references and their positions. The other rules for
  a name (an ASCII identifier of 63 bytes or less, no stimulus or control
  with the same name) are the loader's rules: the chart writes the name
  and the check reports it. A refusal makes no step and changes no byte.
  `dsg_chart_error()` gives the reason, and the status line shows it.
- **A command takes effect at the end of the frame.** A rename moves
  positions through the rename listener, so the chart does a command
  after it draws the frame. The next frame makes the model again.
- **The name field exists only during a rename.** It is a Skribidi field
  (`skf_create()`). The rename makes it and destroys it at the end. A
  field that stays keeps the keyboard focus, and then Ctrl+Z goes to the
  undo of the field, not to the undo of the document.

**Positions are view state.** The part `chart` of the view file holds
the template shown and, for each template, the pan, the zoom and the top
left corner of each box, by the name of the state (`end` for the end).
The definition never holds them (format D13). `test_designer_chart`
checks the bytes of the definition for them.
- A state that has no position gets one on a grid of four columns. The
  chart keeps it, so the box stays where it is when other states come
  and go.
- A rename of a state or of a template moves its entry through
  `dsg_doc_listen()`: the rename, its undo and its redo.
- The position of a deleted state stays in the part, so an undo puts the
  box back where it was.
- The file holds positions and the pan in whole units, and the zoom with
  3 decimals. The chart ignores an entry that has the wrong shape.
- An events template keeps no positions: its parts have no names in the
  definition.

**Measured.** `bench_designer_chart` (`tests/designer/chart_bench.cpp`)
draws the chart with no window, as `test_designer_ui` does.
Linux desktop, Intel Core i5-14500, gcc 12.2, Release, pinned to CPU 2 (a
P-core), 1,000 frames each, two runs; load average 0.91 during the runs
(`uptime`). Microseconds, median and p99 of run 1; run 2 was within 1 % at
the median. The made-up charts have an exit from each state to the next,
an `after_s` exit to the end from every third state, one `over`, and a
shared exit in every state.

| Chart | States, edges | `draw()` of the view | Whole frame | Model build |
|---|---|---|---|---|
| `v2_retry`, the largest in the repository, zoom 1 | 8, 18 | 27.0, 31.8 | 44.0, 47.5 | 2.4, 3.2 |
| made up, zoom 1 | 50, 117 | 65.6, 70.4 | 77.6, 81.4 | 21.8, 23.2 |
| made up, zoom 0.25 (all on the canvas) | 50, 117 | 94.7, 98.7 | 111.1, 113.6 | 23.3, 24.5 |
| made up, zoom 1 | 200, 467 | 237.8, 242.0 | 251.9, 256.0 | 198.1, 201.5 |
| made up, zoom 0.25 | 200, 467 | 240.8, 256.0 | 251.5, 267.9 | 220.6, 227.3 |

- The largest chart in the repository costs 27 us a frame: 0.2 % of a
  16 ms frame. A chart of 200 states costs 0.24 ms.
- Boxes and arrows off the canvas still go into ImGui's draw list; only
  their glyphs are skipped. At 200 states, zoom 1 costs as much as zoom
  0.25. Culling them is an option if a chart grows past a few hundred
  states.
- The model build grows with the square of the states (each `to` is
  found by a scan of the names). It runs only when the text changes, and
  the check after each change costs 0.2 to 7 ms, so it does not count at
  this size.

**Tests.** `test_designer_chart` has 159 checks: the models of
bimanual, visuomotor and `v2_retry` (each kind of edge, the labels, the
bows) and of an events template; the places of paths; each command as one
step that undo and redo take to the same bytes, with the player's check
clean after it where the definition allows; the refusals; the positions
through a rename, its undo and its redo; and the part `chart` through the
view file. `test_designer_te` (optional) has eight chart tests:

| Test | What it does |
|---|---|
| `chart/drag` | Drags a box by (60, 45). The box moves, the bytes of the definition do not change, and Save writes the position to the view file. |
| `chart/connect` | Drags from the port of `hold_start` to the box of `begin`: a new exit, one undo step, selected. A drop on empty space makes nothing. |
| `chart/issue` | Gives a state `max_visits` with no `over`, then clicks the issue in the text view. The chart shows, the state is selected, and its box is in the middle of the canvas. |
| `chart/delete` | Clicks a box (the selection is its path), presses Delete, then Ctrl+Z: the same bytes and the same selection. |
| `chart/rename` | Double-clicks a box, types a name with the keys of the engine and presses Enter. The state and the two exits to it have the new name, the box stays where it was, and the check is clean. Then Ctrl+Z goes to the document. |
| `chart/add` | Clicks Add state twice. The new states are `state` and `state2`, the first one is selected and centered on the canvas, and each click is one undo step. Two Ctrl+Z give the same bytes. |
| `chart/view` | Turns the wheel over a point of a box: the box grows by 1.15 and the point stays under the mouse. Then drags on empty space by (50, -30): the box moves by the same amount. The definition's bytes do not change. |
| `chart/events` | Opens oddball, whose template uses `events`. The banner shows, Add state is disabled, the part has no port, and a drag does not move its box. A template with states shows no banner. |

86 hand mutants of `chart.c` (76) and `view_chart.cpp` (10) each fail
`test_designer_chart` or a `chart/` test of `test_designer_te`. They were
reverted and are not in a mutation list. The 159 checks of a run come
from 129 lines with a check. For 13 of these lines no mutant of part A's
code fails the check: the projects' clean check before any command, and
the checks of the shared document's undo, redo, selection and listeners.

The last three tests came after a hand test (2026-11-20). In it, a person
opened oddball, could not drag or add states, and did not see the gray
note that said the chart was read-only. The note is now the banner. 4 of 5
hand mutants of `view_chart.cpp` each fail one of the three tests (no
banner, Add state enabled, zoom not about the mouse, no pan); they were
reverted. The fifth, a box drag allowed in an events template, is
equivalent: `resolve_positions()` puts the boxes of an events template back
at their default positions each frame.

**Not done.**
- The window was not opened: no person used the chart. No test checks
  the issue marks, the zoom with the wheel, the pan or the look of the
  arrows.
- The Windows (MSVC) and macOS builds were not compiled.
- The chart cannot add, rename or delete a template. The text view does
  that, and the panels will.

#### The tables

Part B, branch `designer-step4-tables` (2026-11-19). The Tables view
edits the project's tables as a grid. The code is in
`designer/core/csv.h`, `csv.c` and `check.c`, and
`designer/app/view_tables.cpp`.

**Which tables.** Each resource of `"kind": "table"` in `pack.json` is one
table, once for each `source`. The view loads them when a project opens.
- A CSV or TSV source is a buffer of the document (`dsg_doc_add_buffer()`),
  with the buffer's name equal to its `source`. Its `delimiter` key gives
  the delimiter: `","` (the default), `";"` or `"\t"`.
- An Excel source (`.xlsx`, `.xlsm`, with its `sheet` key) shows
  read-only, through the pack tool's own reader (`ypt_xlsx_read()`). It is
  not a buffer: its bytes are not CSV, and the designer writes no
  workbook.

**The bytes are the truth.** The grid is `dsg_csv`, an index of each
cell's byte range in the buffer. `dsg_csv_sync()` makes it again when the
buffer's revision (`dsg_doc_rev()`) moves. The index follows ysp/table.h's
dialect, so it has the rows and cells that `ytb_open_csv()` reads: a
record of empty fields (`,,` or `"",""`) is not a row, and a record longer
than the header is an error, also when its fields are empty.
- The index is made again after each change, not shifted. The plan
  measured 0.35 ms for a cell edit with its index at 10,000 rows and
  1.2 ms at 32,767 ("Plan of step 4", Evidence), and a check follows each
  change. No measurement asks for an incremental index.

**Edits.** Each edit is one undo step of the document, with the edited
cell selected in the step, so undo and redo select it again.
- A cell: one splice. The new text is quoted only when it holds the
  delimiter, a quote, a CR or an LF; a quote is doubled. The other bytes
  stay as they are: the file's line ends, its BOM, the quotes of other
  cells and the records that ysp/table.h skips.
- Add row: a copy of the selected row below it, or of the last row at the
  end. An empty record is not a row to ysp/table.h, so a new row in a
  table with no rows has `0` in its first cell. A row takes the line end
  of the file's first record (LF or CRLF). A file whose last record has
  no line end keeps it so.
- Remove row: the record and its line end.
- Add column: an empty column after the selected one, named `column_N`
  (the first N that no column has). Remove column: the cell and one
  delimiter of each record. It refuses the only column. It also removes
  the records that ysp/table.h skips, because with one column fewer they
  are longer than the header, which ysp/table.h refuses.
- The column edits change every record, so they replace the buffer once
  (`dsg_doc_replace()`, which keeps only the bytes that differ). One splice
  for each record would move the rest of the buffer each time.
- Rename: a double-click on a column's name. The name must be a name to
  ysp/table.h (a letter or `_`, then letters, digits or `_`, at most 63
  bytes) and not another column's. The step notes the rename in the scope
  `#SOURCE/columns`, so the column's width follows the rename, its undo
  and its redo.

**The keys.** Click a cell to select it. Double-click, Enter or F2 edits
it in a single-line Skribidi field (`skf.h`) with the cell's text; a
typed character on a selected cell starts an edit with that character.
Enter or a click outside the field keeps the text, as one step; Escape
drops it. While the field has the keyboard, Ctrl+Z is Skribidi's undo of
the typing; after the edit it is the document's. The field is destroyed
at the end of each edit, so it gives the keyboard back. The arrow keys
move the selection, and Delete empties the cell. A click on a row number
or a column name selects the row or the column, for Remove row and
Remove column.

**Drawing.** The grid is an ImGui table. `ImGuiListClipper` gives the
visible rows, and a column scrolled out of view lays out nothing. Each
cell's text is a Skribidi layout, drawn as in the issues panel;
right-to-left text starts at the cell's right edge.
- The cells' layouts come from the shared cache (`skt_layout_n()`), which
  frees a layout 100 frames after its last use ("The layout cache"). Part
  B built the view with a cache of its own, because the shared one did not
  evict then; the integration removed it.
- At 10,000 rows with the window of the tests (1400 x 900 px), a frame lays
  out 126 to 133 cells (at most 17 or 18 rows and the header).

**The view file.** The part `tables` holds the open table, each table's
column widths by column name, and its scroll as the first row and column
shown: `{"open": SOURCE, "tables": {SOURCE: {"widths": {COLUMN: px},
"scroll": [row, column]}}}`. ImGui's own table settings are off for the
grid, because ImGui keeps widths by column index, which an inserted
column moves.

**The check sees unsaved tables.** Before step 4, the check pack came
from the tables on disk, so a table edit was checked only after Save.
- `ui.cpp` gives the check the document (`dsg_check()`'s argument
  `tables`). Each build writes each dirty table's bytes to
  `.designer/check/tableN.csv` (N is the buffer) and sets that resource's
  `source` in the check description to it. A clean table comes from its
  file.
- The check builds the pack again when the set of dirty tables or the
  revision of one changes, as it does for the texts. An undo back to the
  saved bytes after a failed build needs no build: the last pack that was
  built is of those bytes.
- A failed build is not tried again for the same text and the same table
  revisions.
- Part B could not change `dsg_check()`'s signature (`dsg.h` was
  shared), so its check read one document for the process, set by the
  tables view. The integration made the document an argument and deleted
  that global.

**Errors in cells.** The pack tool names a table's error as `'ENTRY':
ysp_table: line L, row R, column 'NAME' (C): ...`; the player and the
pack tool's rules name a column as `column 'NAME'`. `dsg_csv_place()`
reads the entry, the row and the column from the message. The view marks
the cell, the row number or the column's name in red, and lists the
issues of the open table above the grid; a click on one selects its cell.
The check gives these issues no place in the definition.

**Measured.** `test_designer_table_ui --bench 3`: the whole UI frame
(`ImGui::NewFrame()` to `ImGui::Render()`, with the skt atlas, no GL and
no window) while the grid scrolls 3 rows a frame through 10,000 rows of 8
columns (a label in mixed scripts as the second column), 3,328 frames a
pass. Linux desktop, Intel Core i5-14500, gcc 12.2, Release, pinned to
CPU 2 (a P-core), under the measuring lock; load average 1.11 and 1.10
(`uptime`), two runs. Milliseconds, median, p99, max:

| Run | First pass (cache cold) | Passes 2 and 3 |
|---|---|---|
| 1 | 0.113, 0.126, 0.313 | 0.113, 0.128, 0.372 |
| 2 | 0.112, 0.126, 0.200 | 0.112, 0.124, 0.388 |

- Each frame laid out 126 to 133 cells. The two passes cost the same
  because the view's cache evicts a layout after 100 frames unused, and a
  pass is longer than that.
- This is the CPU of a frame without GL. The probe's native table took
  1.39 to 1.45 ms of UI a frame (part 2, section 4), with its trace and
  chart in the same frame. A frame here is far inside 16 ms.

**Tests.**
- `test_designer_table` (headless, C), 236 checks: the index against
  `ytb_open_csv()` on the 11 CSV files in `tests/` and `examples/` and 5
  made-up tables (quotes, CRLF, BOM, `;`, tabs, skipped records); each
  edit as one step that ytb reads as intended, with undo and redo to the
  same bytes, on LF, CRLF and open-ended files; quoting; refusals with no
  step; the rename's scope through undo and redo; places in messages; the
  check of `tests/player/oddball` with an unsaved table (an edit builds a
  clean pack; the rule `max_run type=target 1` with the level `target`
  removed in the document only is reported, the file on disk unchanged;
  a byte that is not UTF-8 is reported at its row and column).
- `test_designer_table_ui` (headless, ImGui, no window): a table of
  10,000 rows and 8 columns in the Tables view; the layouts in a frame at
  the top, the middle and the end of the table and in every frame of a
  scroll through it are at most the visible rows' cells and the header's.
  With `--bench PASSES` it times the frames of a scroll.
- Test Engine, `tables/*`: `shows`; `scroll` (10,000 rows, the engine's
  wheel and scrolls to the end and back); `edit` (a double-click, Arabic
  text with a comma through the engine's keys, Enter: one step, quoted;
  Ctrl+Z); `add_row` (a click on a row number, Add row, Ctrl+Z);
  `widths` (the engine drags a column's border; the width is in the view
  file and comes back after another project was open; a rename through
  the header keeps it, and Ctrl+Z moves it back).

**Hand mutants.** 25, each reverted, each failed its test: the quotes
for the delimiter, for a quote (not doubled) and for an LF; `""` read as
a value; a long empty record taken; LF rows in a CRLF file; a new last
row with no line end before it; a removed last row that leaves the line
end of an open file; the skipped records kept when a column goes; the
rename not noted; a taken name accepted; the selection not in the step;
a message's row off by one; an edit with a stale index; the check's key
without revisions; the check description's source not changed; a failed
build keyed by the text only (`test_designer_table`); every row drawn
(`test_designer_table_ui`); Enter does not commit; the field keeps the
keyboard after the edit; the width does not follow a rename; no widths in
the view file; widths not read from it; one ImGui table for every
project; Add row adds an empty row at the end (`tables/*`).

**Not done.**
- No person used the view: the window was not opened.
- Four items of this list were fixed at the integration ("The
  integration"): `test_designer_table_ui` is in `designer/CMakeLists.txt`;
  a click on a table's issue in the Issues panel selects its cell; a table
  edit marks the project unsaved; and the cell's field lays out its text
  at the cell's width, not at 200 px (part C's change to `skf.cpp`; read
  in the code, not seen in a window).
- An Excel source was not opened in a test: the repository has no
  workbook fixture.
- An edit open while another main view shows ends at the next frame that
  draws the tables; until then Ctrl+Z is the field's.
- IME composition in a cell, MSVC and macOS builds, ASan and UBSan runs.

#### The layout cache

Built at the integration (2026-11-19). `skt_layout()` and `skt_layout_n()`
(`designer/app/skt.cpp`) keep one Skribidi layout for each text and size,
for the issues panel, the chart and the tables. `skt_frame_begin()` frees
each layout that was not used for 100 frames.

- **Why not Skribidi's cache.** `skb_layout_cache_compact()` (skribidi
  `dee63d6ba76a`, the designer's rasterizer commit) removes an evicted
  entry from its table and does not call `skb_layout_destroy()`. So the
  memory of each evicted layout stays allocated: row A of the table below.
- **How.** A hash table (open addressing, linear probing, at most half
  full, removal by backward shift, so no tombstones) keyed by an FNV-1a
  hash of the text and the size; each entry keeps its text, so two texts
  with one hash are two entries. A list keeps the entries from the most
  recently used to the least. The compaction walks the list from its
  least recent end and stops at the first entry still in use, so a frame
  pays only for what it frees.
- **100 frames** is Skribidi's own value. The designer draws on input
  only, so 100 frames are 100 inputs or animation frames, not a time. A
  layout stays valid until the next `skt_frame_begin()` at least; a view
  keeps no layout across frames.

**Measured.** `test_designer_table_ui WORK --bench 3`: the 10,000-row,
8-column table of the test, opened, scrolled as the test does, then
scrolled through 3 times at 3 rows a frame (3,328 frames a pass). Linux
desktop, Intel Core i5-14500, gcc 12.2, Release, pinned to CPU 2 (a
P-core), under the measuring lock, load average 0.95 over the last minute
(`uptime`), two runs. Resident memory in MB (`/proc/self/statm`); frames
in ms, median and p99, of run 1. Run 2 was within 0.2 MB and 0.002 ms at
the median.

| Cells' layouts | After open | Before the passes | After pass 1 | After pass 3 | Layouts after pass 3 | Frame, pass 1 | Frame, passes 2 and 3 |
|---|---|---|---|---|---|---|---|
| A: the tables view's own cache (Skribidi's), as part B built it | 29.2 | 50.9 | 91.5 | 172.6 | not counted | 0.112, 0.126 | 0.112, 0.124 |
| B: the shared cache with no eviction | 29.4 | 51.9 | 70.3 | 70.3 | 20,015 | 0.092, 0.116 | 0.080, 0.088 |
| C: the shared cache with eviction (built) | 29.3 | 35.2 | 35.2 | 35.2 | 653 | 0.103, 0.113 | 0.103, 0.115 |

- A keeps 40.6 MB more after each pass: the leak. B is bounded only by
  the table's distinct texts: this table has about 20,000 (its labels and row
  numbers are unique; its other columns repeat), about 2 KB a layout. A
  table of 80,000 distinct cells would keep about 160 MB. C keeps the
  rows of the last 100 frames.
- The compaction: `skt_frame_begin()` (the atlas's compaction and the
  layouts' eviction) took 0.001 ms median, 0.001 ms p99 and 0.29 ms in
  the worst of 9,984 frames with eviction (C), against 0.000 ms median,
  p99 and worst with none (B). A scroll at 3 rows a frame frees about 24
  layouts a frame.
- C's frames cost 0.01 to 0.02 ms more than B's: B's passes found most
  layouts in the cache (the test's own scroll had made 11,400 of them
  before pass 1), and C makes again what it freed. C costs 0.009 ms a
  frame less than A.
- **Decision:** the tables view has no cache of its own. It uses the
  shared one, which costs less a frame than the view's own (A) and keeps
  no memory after a scroll.

#### The integration

The three parts were merged on `designer-step4` on 2026-11-19, in the
order panels (C), chart (A), tables (B), each with `git merge --no-ff`.

- **Conflicts:** only `docs/designer.md` (each part added a "Decisions of
  step 4" heading: one heading is kept, with the parts in the order
  panels, chart, tables) and `docs/status.md` (all entries kept). The
  Test Engine's registration lists did not conflict: each part registers
  its tests in its own file.
- **CMake:** `bench_designer_chart` (not a test; not on Windows),
  `test_designer_table_ui` (with `dsg_test()`), and `view_tables.h` in the
  UI sources.
- After each merge, `ctest -R designer` passed with the Test Engine ON:
  7, 7 and 8 tests, and 15, 19 and 23 tests in `test_designer_te`.
- `bench_designer_chart` after the merges, with the panels in the side
  column (as in "The chart": CPU 2, 1,000 frames, two runs; load average
  0.80): `v2_retry` 24.6 us for `draw()` and 41.1 us for the whole frame;
  200 states at zoom 1, 192.2 and 207.1 us (medians of run 1; run 2 within
  2 %). These are at or below the part's own numbers.

**Fixes of shared code that the parts asked for.** The shared files were
frozen for the parts, so each part reported its need and the integration
made the change. Each fix has a test that failed without it, and hand
mutants that fail the test (reverted; not in a mutation list).

| Fix | Change | The test, and its run without the fix | Hand mutants killed |
|---|---|---|---|
| A table edit marks the project unsaved | `ui.cpp` reads the unsaved state again when `dsg_doc_revision()` (any buffer) moves: after the views draw (before the title), and in Open, New, Quit and `ui_dirty()`. Before, only a change of the definition did, so after a table edit the title had no `*` and Open and Quit did not ask. | `test_designer_table_ui` part 5: a cell edit; `ui_dirty()`, the `*` in the title, Quit asks. Without the fix (`refresh_dirty()` empty), 3 checks failed. | No refresh before the title (1 check failed) |
| A click on a table's issue in the Issues panel selects its cell | A view hook, `issue_sel` (`views.h`): the selection that an issue names in the view's own data. `ui_jump_to_issue()` takes the first view's answer before the issue's path in the definition, and reveals it (claims, then `reveal`). The tables view answers for a CSV table's cell, row or column. | `test_designer_table_ui` part 6: a byte that is not UTF-8 in row 5,000; the jump to its issue shows the Tables view with the cell selected and in view. Without the fix (no view asked), 2 checks failed: the text view stayed, and rows 9,960 to 9,976 showed. | The answer selected but not revealed (2) |
| The check takes the document | `dsg_check()` has an argument `tables`, the document whose table buffers it builds from (NULL: the tables on disk). `dsg_check_use_tables()` and its process-wide pointer are deleted. | `test_designer_table`, two documents: two copies of a project, each with its document and checker, one with an unsaved edit; each check reads its own document. Ported to the old call (the pointer set when a project opens, as the tables view did) and run on the merge commit `94657e4`: 2 checks failed, the edited project's check read the other's tables. | The pack's key without the document (3); the check description without it (4); `ui.cpp` gives NULL (3) |
| The shared layout cache evicts | "The layout cache", above | `test_designer_table_ui` part 4: after a scroll through the table and 101 frames at rest, the cache keeps the cells in view; the rows in view stay hits while the rest goes; a layout used each frame is never made again; one unused for 101 frames goes. Without eviction, 3 checks failed (11,399 layouts at rest). | A hit that does not renew its frame (1); a removal that leaves a hole in the table (1); a hit that keeps its place in the list (3) |
| `views.h` says what a view may call | `views.h` lists the three calls of `ui.h` that the panels make: `ui_show_view()`, `ui_editor()` and `ui_jump_to_issue()`. | A comment: no test. | - |

**Verified on the integrated branch** (Linux desktop, gcc 12.2):
- The vendored sources match their hashes (`tools/vendor_layout.py`,
  `tools/vendor_pack.py`, `tools/vendor_designer.py --test-engine`).
- A clean configure and a build of every target, with
  `YSP_BUILD_EXAMPLES=ON`: 628 steps with `YSP_DESIGNER_TEST_ENGINE` ON
  and 603 with it OFF, Release, no compiler warning.
- `ctest` from the build tree with `DISPLAY` and `WAYLAND_DISPLAY` unset:
  138 of 138 passed with the Test Engine ON and 137 of 137 with it OFF
  (8 and 7 designer tests). `test_designer_te` passed 23 of 23 tests.
- ASan and UBSan (Debug, built and run under `setarch -R`):
  `test_designer_core` (82 checks), `_doc` (115), `_chart` (159), `_table`
  (241), `_panels` (207), `_ui` (39) and `_table_ui` (33) passed with leak
  checks on and no report; `test_designer_te` (23 tests) passed with leak
  checks off and no report. With the tables view's old cache (row A
  above), LeakSanitizer reported 22.2 MB in 113,600 allocations from
  `skb_layout_cache_get_utf8()`.
- `-Wconversion -Wsign-conversion` on the 22 designer sources and tests of
  step 4 (`designer/core`, `designer/app`, their tests): no warning, after
  one cast in `skt_frame_end()` (a line of step 3).

**Not done.**
- No person opened the window: nobody used the chart, the tables, the
  panels or the fixes above.
- No Excel workbook fixture: an `.xlsx` source was not opened in a test.
- The panels add and remove no list items (a state, a step, an exit) and
  no set members.
- Variables have no candidate lists: `if` and `unless` are text fields.
- The chart does not cull shapes off the canvas; at 200 states, zoom 1
  costs as much as zoom 0.25.
- IME composition in a table cell is not tested.
- The Windows (MSVC) and macOS builds were not compiled here; CI's job
  `designer` builds `designer_tests` on both and has not run on this
  branch.
- The Test Engine ran in its fast mode only.

### Decisions of step 5

#### Part A as built (2026-11-21)

Branch `step5-a-core`, from `step5-s` (`813e40b`). Part A fills
`designer/core/preview.c` behind S's frozen `preview.h`. No frozen file
changed; `designer/CMakeLists.txt` only gains the test and the bench.

**What it does.**
- **The session ops** (check thread). The load makes the run's folder,
  calls `pl_sim_load()` with the check's pack, the built-in rig (`rig`
  and `cal` `NULL`), the settings and `main.cpp`'s first gfx (`gl_caps`,
  `share`, `cache`, `gl_proc`), and fast-forwards to the pin. The close
  calls `pl_close()` and removes the data sets that are done with.
- **The settle.** The tick posts a session job when the document has not
  changed for 300 ms and a check accepted its current revision. A check
  of an older revision posts nothing. The first job after an open does
  not wait: nothing changed.
- **Adoption.** A session that is not stale starts to attach. The tick
  calls `pl_sim_attach()` with a budget of 4 ms a frame. The shown
  session steps in each of these frames. In the frame that the attach
  ends, the old session stops, detaches and goes back to the worker, and
  the new one shows; its first step is in the next frame. A newer session
  that arrives during an attach replaces it.
- **Steps.** The frames due are those whose onsets are at or before
  `speed` x (real time since the anchor). A tick steps at most 16, and
  only the last one draws. Past 16, the rest are dropped, `info.slow` is
  true, and the anchor moves to the last frame stepped, so the session
  does not race to catch up. Pause, a hidden pane and a run after a pause
  move the anchor too.
- **Idle.** While paused, ended or failed, `animating` and `work` are
  false. Work is an attach in progress, a step asked for, a restart that
  can post, or an accepted text that waits for its settle.
- **Input.** Keys and choices go into a queue of 64. At the next step
  they reach the session at that frame's onset less 1 ns. While paused,
  a key or a choice is dropped, but the up of a down that the session
  took always goes to it. The pointer sends one sample a step, only when
  it moved, to each `position2d` role that the preview drives.
- **State.** `where_now`, the outputs ring (the last 64 codes, times from
  the first frame), the roles not driven, the choices, the leaves and the
  info all read the shown session.

**Decisions, and the reasons.**
- **The load numbers the run, not the UI.** The UI sends the runs'
  folder, `.designer/preview`, in `settings.out_dir`. The load picks
  `run-N` and writes the folder back. So two loads never race for a
  name, and a folder that is already there is skipped.
- **A file keeps the runs' range.** `.designer/preview/runs` holds
  "LO HI": the lowest run folder that may be left and the highest made.
  Only the session ops read and write it, on the check thread. The close
  removes `run-LO` to the run below the one that the UI keeps, then moves
  LO. Why: `platform.c` cannot list a folder, `dsg.h` is frozen, and a
  later process must find what an earlier one left. A removal that fails
  (an open file on Windows) leaves LO, so the next close tries again.
- **The first session of an open** has no old session to carry that
  removal, so the preview hands the worker a session with no player
  (`dsg_worker_close()`), whose close only removes.
- **What the preview keeps.** At a replacement, the old run goes. At a
  project's close, the shown run stays, with its `session.json`, so the
  person can read it. A stale, failed or refused session's run goes at
  its close.
- **Stale.** A session is stale when its revision is not the newest
  job's, the document changed after that job, its settings differ from
  the preview's settings now, or a newer job waits or runs. A stale
  session never attaches.
- **Restart with a refused text** posts the shown session's own text,
  pack and revision: the last good text again.
- **A refusal at the load or in the fast forward** comes back as a
  session that holds the refusal, not as a failed load. Its place in the
  text is found on the check thread. So the preview issue names the
  object and its place, and the shown session stays. Only a load that
  cannot run (no folder, no memory) is a failed load, with no place.
- **A refusal at run time** stops the session; the image stays. The
  preview issue gets its message, object and place in the text that the
  session loaded. The place is found once, in that tick.
- **The issue goes** when a session that loaded well is adopted.
- **Paused when a new session shows:** one drawn frame, so the pane shows
  the new definition. Then it stays paused.
- **Who answers.** With no pin, `live()` gives every trial to the person.
  With a pin, the scripted participant answers until the pin.
- **The fast forward stops** at the pin, when the walk leaves the pinned
  leaf without the row ("row 5 was not drawn in this run of the leaf:
  seed 7"), at a trial with a signal role that lasts 30 s of session time
  ("waits for input that the script cannot give"), or after 200,000
  frames. The session goes to the UI where it stopped, with the note.
- **The pointer's scale** is gfx's: px per cm is width x 10 / width_mm,
  px per deg is width / width_mm x distance x tan(1 deg). The view is the
  one the load gave the gfx, or the built-in rig's (0.27 mm a px,
  570 mm). The origin is the display's center, y down (D22).
- **Without a thread**, the preview runs its session job at the post
  (`dsg_worker_wait()`), as `ui.cpp` does for a check.

**Measured** (Linux desktop, Intel Core i5-14500, gcc 12.2, Release,
`bench_designer_preview WORK 1000`, pinned to CPUs 2 and 4, under the
measuring lock; two passes, load average (1 min) 0.36 before the first
and 0.84 after the second). gfx's null backend: no GL work is in these
numbers; part D measures the GL side. Microseconds, median, p99 and max
of pass 1. Pass 2's medians are within 3 % or 0.1 us of pass 1's; its
maxima differ where the table says.

| Project | Tick, 60 Hz display (1 step, drawn) | Tick, 500 Hz display (8 or 9 steps) | A step, no draw (500 Hz) | The frame that adopts |
|---|---|---|---|---|
| bimanual | 2.0, 4.1, 59 | 13.0, 17.2, 70 | 1.4, 3.0, 54 | 5.3 |
| stroop | 0.3, 1.6, 33 | 1.5, 4.7, 32 (pass 2: max 245) | 0.1, 1.0, 28 | 2.6 |
| many (200 text stimuli on) | 37.3, 68, 173 | 42.0, 67, 175 | 0.7, 2.1, 167 | 4.7 |

| Project | Load (check thread), ms | Close (check thread), ms | Load and fast forward to a pin, ms |
|---|---|---|---|
| bimanual | 10.4, 30.5, 30.5 (pass 2: max 10.5) | 14.8, 16.5, 16.5 | 13.8 (stops at trial 0: a signal role) |
| stroop | 3.0, 3.3, 3.3 | 8.1, 8.2, 8.2 | |
| many | 6.9, 7.4, 7.4 | 8.1, 13.2, 13.2 | 748 (stops at the 200,000-frame cap, before leaf `b9`) |

- The UI thread's part of a frame is at most 0.25 ms in every case, far
  under the 16 ms frame. The drawn frame is the most of a tick: 37 us
  median with 200 text stimuli on.
- The close is the largest cost on the check thread for small projects:
  `pl_close()` runs the end's frames and the data set waits on the disk.
  An earlier pass, at a load average of 0.9 to 1.2, gave stroop's close
  41 ms median and 83 ms max: the disk, not the code.
- The fast forward of the generated project stops at its cap after
  0.75 s on the check thread (3.7 us a frame). A pin past about 3,300
  trials of 60 frames is not reached. The knob is `PV_FF_MAX_FRAMES`.

**Tests.** `test_designer_preview` (189 checks; headless, null gfx, the
test's clock, the worker inline). (1) The first session at 1920 x 1080
and 60 Hz, its data set and writer; one step a UI frame; the cap of 16
with one drawn frame and no catch-up; speed 2 and 0.25; paused: no step
and no frame wanted in 10 s of frames; step: one drawn frame, then
paused; a run after 5 s paused and a hidden pane: no catch-up; the end
after 3 trials. (2) No job 33 ms after an edit or before 300 ms; one
after; one job after a burst of edits; none for an older revision's
check. (3) A refused text keeps the session running, stale with its
errors; restart reloads the last good text; the fixed text loads. (4)
Stale: an edit after the post, other settings, a newer job waiting.
(5) With `pl_sim_attach()` held by `--wrap`: the 4 ms budget, the old
session steps while the new one attaches, a newer session replaces the
attach, no step in the attach's frame. (6) A run-time refusal as the
issue (template `win`, its line in the loaded text), cleared by an
accepted edit; the fast forward's refusal; a failed load (the runs'
folder is a file). (7) The pin: 2 of 6 rows found under `subset 2`, 4
with the note; leaf `b` any row; "Preview this template"; the stall on a
signal role. (8) Keys through the claimed keyboard, Escape, the up after
the keyboard went back, rt on the grid less 1 ns; oddball's choice, its
trigger codes (101 last), the sync line not driven; visuomotor's
relative role not driven. (9) The pointer at a region's center in px,
cm and deg ends the state; 3 px off does not; leaving the display sends
nothing. (10) One run folder after restarts; after the close the shown
one stays; a second "process" numbers on and leaves one. (11) No heap
call in 1,500 running ticks with keys and the pointer (`--wrap`). (12)
The real thread: bursts of restarts and edits, a reopen.

**Sanitizers** (gcc 12, Debug, built and run under `setarch -R`,
`DISPLAY` unset). ASan with UBSan (`halt_on_error`) and ThreadSanitizer:
the 11 designer tests, no report. ThreadSanitizer: 10 more runs of
`test_designer_preview` and 3 of `test_designer_te`, no report. The UI
and Test Engine tests now load real sessions on the check thread.

**Hand mutants** (each reverted; each fails `test_designer_preview`):

| Mutant | Fails |
|---|---|
| pv-01 settle: no wait after a change | a session job 33 ms after the edit |
| pv-02 newest session: an attach in progress keeps its older session | the shown run is not the newest job's |
| pv-03 stale drop: a stale session is adopted | an edit's stale session attached |
| pv-04 step cap: no cap | a 1 s frame stepped 59, not 16 |
| pv-05 pause idle: a paused session animates | paused, 600 of 600 frames wanted a redraw |
| pv-06 run folders: the close removes no older run | the second process left 2 folders |
| pv-07 draw: every step draws | 16 frames, 16 drawn |
| pv-08 input time: half a period before the onset | rt is not a frame onset less 1 ns |
| pv-09 hidden pane: steps | hidden, stepped 60 |
| pv-10 pointer: deg at cm's scale | at the region's center the state is `wait` |
| pv-11 pin: the fast forward goes past a leaf left without the row | row 0, no session |
| pv-12 refusal: the run-time refusal is no issue | no preview issue |

**Freeze issue.** `test_designer_ui` part 8 (`ui_test.cpp`, S's) ends
with "no session: nowhere" (line 405). It held with S's stub. With part
A the pane shows a project that the check accepts, so a session loads,
and the check fails. It is the only failure in ctest (141 of 142,
Release, examples and the Test Engine on, `DISPLAY` unset). The check
needs a change in S's file: for example, read `ui_preview_where()` before
the pane first shows, or after the project closes. Fixed at the
integration ("Parts A, B and C integrated").

**Not done.**
- No GL context: the attach's GL work, the texture and the drawn frame
  on a GPU are part D's.
- Windows and macOS were not compiled. The test's `--wrap` parts (the
  held attach, the heap count, the drawn frames) skip there.
- A session that ends keeps its data set open until it is replaced: its
  `session.json` is written at the close.

#### Part C as built (2026-11-21)

Branch `step5-c-chart`, from `step5-s` (`813e40b`). Part C adds the
preview's hooks to the state chart (`designer/app/view_chart.cpp`): a
ring on the running state, Follow, and "Preview this template" ("The
chart" lists what a person does). No frozen file changed.

**The ring.** A green ring outside the box of the state that
`ui_preview_where()` names, when the chart shows the running template. It
is outside the box, so the selection's border and an issue's border do not
hide it. `dsg_chart_running()` (`chart.c`) gives the nodes:

| Template | Place | Ring |
|---|---|---|
| `states` | a state | that state |
| `states` | no state | the end |
| `states` | a state that the chart does not have (the text changed after the load) | none |
| `events` | the events run, or the response waits | each part |
| `events` | `after` runs | the end |
| any | between trials, another template, no session | none |

- **An events template rings each part.** `pl_sim_where` (player 0.17.0)
  and `dsg_preview_where` give a state for version 2 and only the phase
  for an events template, not the step. So the chart cannot tell which part
  runs. To ring one part, both structs need the part (or the step): a
  change to the player and to the frozen `preview.h`. Not made; reported.
  The common case has one part (no `until`), where the ring is exact.

**Follow.** A check box in the chart's toolbar. While it is on, the chart
shows the template that the preview runs, and changes with it.
- Off by default: a chart that jumps while a person edits it would lose
  their place.
- Between trials, with no session, or when the text no longer has the
  running template, the chart stays where it is (`dsg_chart_follow()`).
- A pick in the template list, or a jump to a state of another template
  (an issue, a panel), clears Follow. Else the next frame would take the
  chart back.
- It is view state: the part `chart` holds `"follow": true`, written only
  when on, so a project that never follows keeps its part's bytes. Any
  other value reads as off.

**"Preview this template".** A button in the toolbar. It calls
`dsg_preview_pin_template()` with the shown template. When the preview
pins a leaf, the AUX pane shows; when no leaf runs the template, the status
line says "Not pinned: no leaf of the timeline runs 'T'." With no session
(the pane was never shown), the preview has no leaves to look in: the pane
shows, the status line says "The preview loads; then it pins 'T'.", and the
chart pins when the first session shows (added at the integration). A read-only chart
(an events template, or a text that does not parse) pins too: the pin
edits no definition. The pin runs at the end of the chart's frame, after
the last read of the place, because a pin restarts the preview and the
place's strings are valid only until then (`preview.h`, "Pointers").

**The seam.** `designer/app/view_chart.h` (new): the chart reads the
place and pins only through two hooks, `ui_preview_where()` and
`dsg_preview_pin_template()` by default. `ui_chart_set_preview_hooks()`
puts in a test double; NULL puts the real calls back. The double gives
each place at once, with no session to load and step, and it works on S's
stub of `preview.c`, which runs no session. The logic that needs no window
is in `chart.c`, so `test_designer_chart` tests it headless.

**Cost.** The ring is two integers a frame and one `AddRect()` or
`AddCircle()` into the draw list, which keeps its buffers between frames:
no allocation per frame (from the code; not counted with an allocator
hook). Follow copies the template's name only when it changes.
`bench_designer_chart` gains a line `ring`: the view's `draw()` with the
place (a test double) on the middle state and Follow on, against `draw`,
the same with no place. i5-14500, gcc 12.2, Release, pinned to CPU 2,
1,000 frames, two runs. The machine was not quiet: load average 8.8 to
11.6 from other jobs. Microseconds, median and p99 of run 1; run 2 was
within 0.6 us at the median.

| Chart | `draw` | `ring` |
|---|---|---|
| `v2_retry`, zoom 1 | 24.6, 25.8 | 24.7, 25.9 |
| 50 states, zoom 1 | 54.3, 55.9 | 54.5, 57.5 |
| 50 states, zoom 0.25 | 82.8, 85.0 | 83.2, 85.0 |
| 200 states, zoom 1 | 195.5, 322.1 | 195.6, 198.7 |
| 200 states, zoom 0.25 | 199.3, 205.1 | 199.8, 204.1 |

The ring and Follow add at most 0.6 us a frame, within the noise of this
load.

**Tests** (Linux desktop, i5-14500, Debian 12, gcc 12.2):
- `test_designer_chart` part 5 (now 184 checks, 25 new): the ring's nodes
  for each row of the table above, on a states template, an events
  template of three parts and one of one part; Follow's template (the
  definition's own key, none for a missing one, between trials, no session
  or no definition); `"follow"` through the part (absent when off, read
  back when on, off for a value of another type, off after
  `dsg_chart_view_free()`).
- `test_designer_te`, five new chart tests. The first three use the test
  double and find the ring by its color in the chart's draw list:

| Test | What it does |
|---|---|
| `chart/ring` | bimanual: no ring with no place; the ring on `hold_start`, and nowhere else, also with the box selected; on `go`; on the end with no state; none for `track`, between trials or with no session. oddball: the one part while the events run, the end in `after`; after an `until` is set into the text, each of the three parts. |
| `chart/follow` | Off at first: the chart stays on `reach` while `track` runs. On: it shows `track` with the ring on `wait`, then `reach` when that runs; it stays between trials, with no session and for a template that the text lacks. Save writes `"follow":true`. A jump to `track` clears it, and so does a pick of `track` in the list. |
| `chart/pin` | The button pins `reach`; a refusal shows the status and not the pane; a pin shows the pane. oddball's read-only chart pins `oddball`. |
| `chart/pin_real` | With the real `preview.c`: the pin is the first leaf of the definition's timeline that runs `reach`, row any, and the pane shows. |
| `chart/ring_real` | With the real `preview.c`: Follow on, the pane shown, a session of bimanual runs until it is in a state, then paused; the ring is on that state and nowhere else. |

- **Re-run after part A merges:** `chart/pin_real` and `chart/ring_real`.
  On S's stub they log "skipped" and pass: a stub's
  `dsg_preview_session_ops()` gives no `load`, and the tests check that.
  With part A they run, and they were not run here. Also re-run the other
  `chart/` tests and `test_designer_chart`, which do not depend on A. Done
  at the integration: both run and pass on A.
- ctest from the Release build tree, examples and the Test Engine on,
  `DISPLAY` unset: 141 of 141. `test_designer_te` 32 of 32, three runs.
- ASan with UBSan (gcc 12, Debug, built and run under `setarch -R`):
  the ten designer tests, `halt_on_error`, no report.

**Hand mutants** (each reverted; each fails a test):

| Mutant | Fails |
|---|---|
| c-01: another template rings | `chart`: another template: no ring |
| c-02: no state rings nothing, not the end | `chart`: no state: the end; `chart/ring` |
| c-03: an events template's `after` rings the parts | `chart`: after: the end; `chart/ring` |
| c-04: an events template rings its first part only | `chart`: every part |
| c-05: the between phase rings | `chart`: the between phase: no ring |
| c-06: Follow takes a template that the text does not have | `chart`: Follow's three checks |
| c-07: `"follow"` not written | `chart`: on: written; `chart/follow` (Save) |
| c-08: `"follow"` written when off | `chart`: off: no key |
| c-09: `"follow"` not read | `chart`: on: read back |
| v-01: the ring on a box not drawn | `chart/ring`, `chart/follow` |
| v-02: the end never ringed | `chart/ring` |
| v-03: only the first node of the range ringed | `chart/ring` (three parts) |
| v-04: Follow ignored | `chart/follow` |
| v-05: a pick in the list keeps Follow | `chart/follow` |
| v-06: a jump to another template keeps Follow | `chart/follow` |
| v-07: a pin does not show the pane | `chart/pin` |
| v-08: a refused pin says nothing | `chart/pin` |
| v-09: the pin names no template | `chart/pin` |
| v-10: a read-only chart does not pin | `chart/pin` |

An earlier form of v-02 (no circle around the end) survived: the rectangle
branch then rang the end. That is only a shape; the mutant was replaced by
one that rings no end.

**Not done.**
- `chart/pin_real` and `chart/ring_real` did not run (S's stub).
- The window was not opened: no person saw the ring or used Follow.
- An events template's ring cannot name one part (above).
- MSVC, clang and macOS were not compiled. Locals that a call fills are
  initialized (C4701); no narrowing conversion was added (C4244).

#### Part B as built (2026-11-21)

Branch `step5-b-pane`, from `step5-s` (`813e40b`). Part B is the
preview's pane, `designer/app/view_preview.cpp`, in the AUX slot. Part A
(`preview.c`) was built at the same time, so the pane was tested on a test
double of `preview.h` (below). No frozen file of part S changed, and B
needed no change in them.

**What the pane shows, from the top.**
- The controls: Run, Pause, Step, Restart, New seed, Speed (0.25x, 0.5x,
  1x, 2x), and Fit or 1:1. A button that cannot act now is disabled: Run
  only while paused, Pause only while running, Step while running or
  paused. New seed draws a 63-bit seed (splitmix64 of the clock and the old
  seed) and restarts through `dsg_preview_set()`.
- The status: waiting, attaching, running, paused, ended ("Session over: N
  trials"), failed, with the preview issue's text under it; "Slower than
  real time" when the last tick dropped frames; a line when a newer session
  loads or attaches while the shown one runs.
- The banner, in a blue band: "Faithful in logic, not in timing: a
  simulated N Hz display paced by this window; input times are frame
  times; no sound." N is the shown session's rate, else the settings'.
- The stale flag, in a red band: "Shows the definition as of HH:MM:SS; the
  text has N errors." The time is local, from `shown_unix_s`. The fast
  forward's note, when there is one, in a brown band.
- Where the session is: the trial, the leaf and its row, the template, the
  state, the frame and the time; then the last trial's outcome, response
  and rt.
- The display, then the choice strip, then four sections: Settings
  (collapsed at first), Roles not driven (only when there are some),
  Outputs and Data set.

**The display.**
- Fit: the scale `s` is the largest that fits the room below the
  controls, less the measured height of the rows that are always there
  (the status, its note row, two rows of where, the keys row, the
  "Faithful" banner) and 7 rows for the details, at most 1. The status's note row and the two rows of where the
  session is are always there, empty or "Last trial: none yet." when
  there is nothing to show. So the display keeps its size while the
  session runs; a stale banner, a fast forward's note or an issue moves it
  down. A first version reserved 5 widget heights for the rows instead of
  their measured height; at 1:1 the image then overflowed its child
  window by 6 px in `test_designer_preview_gl`. Before the 2026-11-21
  hand test the room was what the rows above left, and the display shrank
  after the first trial (403.6 to 368 points in `preview/steady`). 1:1: `s` = 1 in a child window with scroll bars. The size in
  points is `W x s / io.DisplayFramebufferScale`, at a position snapped to a
  whole framebuffer px.
- The texture is drawn with `ImDrawList::AddImage()`, not
  `ImGui::Image()`: a white tint and no style alpha, so ImGui's blend does
  not mix the device codes with the background. The rows are bottom up: uv
  (0, 1) at the top-left corner.
- At 1:1, `platform_io.DrawCallback_SetSamplerNearest` before the image and
  `DrawCallback_SetSamplerLinear` after it (ImGui 1.93; the GL backend sets
  them). The pane makes no GL call. At Fit, no callback: ImGui's own linear
  sampler. Scaled, the pane says "Scaled: fine patterns alias; use 1:1".
- `UiPlatform.gl_caps` NULL (the headless tests): a gray box at the
  display's aspect, with "No image: the session draws with no GL." The
  cursor and the click work on the box as on an image. With caps and no
  texture yet: a dark box, "No frame yet."

**Input.**
- The cursor over the display goes to `dsg_preview_pointer()` in
  participant px from the top-left corner: `(mouse - corner) x W / width
  in points`. Only a change is sent, so a still cursor over a paused
  display gives no input. Leaving the display sends `inside` false once.
- A press on the display claims the keyboard (`dsg_preview_set_keyboard()`)
  and draws an amber frame around it. Escape gives it back (in
  `dsg_preview_key()`), and so does a click outside the pane: the pane
  gives it back when its window and its children lose ImGui's focus.
- A choice button sends `dsg_preview_choice(i, true)` when pressed and
  `false` when released, so a hold is a hold. Its label is the choice's
  name through Skribidi.

**Settings.** Display (px), Rate (Hz), Seed, Participant, Its rt (s), the
trial (the pin: "Any: from the start", or a leaf with its template, and a
row when the leaf has a table), and the session fields that the check's
needs name (`DSG_FROM_RUN`, `PL_NEED_FIELD`), empty for the check's
stand-in. A field takes effect when the person leaves it
(`IsItemDeactivatedAfterEdit()`), so typing does not restart the session
at each key. The bounds are the view file's (`preview_part.c`): sides 1 to
16,384 px, 1 to 1,000 Hz, rt over 0 and at most 60 s. A participant other
than `correct`, `silent` or `p=` and a probability from 0 to 1 is not taken,
and the status line says what to type.

**Outputs and the data set.** Outputs: a table of the codes sent, oldest
first (time in seconds, role, code), that keeps the newest in view unless
the person scrolled up. Data set: the run's folder; "Open trials.csv"
(`ui_open_file()`; no view opens a file yet, so the status line names the
folder) and "Copy path".

**User text.** Leaf, template, state, response, role and choice names, the
issue's text and the folder go through Skribidi (`skt_layout()`), from the
shared layout cache. Each also goes to ImGui's log (`ImGui::LogText()`, a
no-op while the log is off), so a test reads them as ImGui's own text.

**The test double.** `tests/designer/preview_double.c` defines every call
of `preview.h`. It is in the sources of `test_designer_ui` and
`test_designer_te`, so the linker does not take `preview.c`'s object from
`ysp_designer_core`. Off (the default), it acts as S's stub, so the other
tests see no change. On (`pvd_enable()`), it posts real session jobs to the
worker after a 300 ms settle and an accepted check of the current
revision, gets them back through `ui.cpp`'s take and adopt, attaches at the
next tick, and steps a frame grid at the rate and speed, at most 16 steps a
tick. A key or a choice ends the trial with that response; a trial with
none ends after 2 s. The test scripts the choices, the roles not driven,
the leaves, the output role, a texture name, an end and a run-time refusal.
`DSG_PREVIEW_DOUBLE` 0 compiles it to nothing.

**After A merges.** `test_designer_te` must run on A's `preview.c`: take
`preview_double.c` out of its sources and define `DSG_PREVIEW_DOUBLE` 0 at
the top of `te_preview.cpp`. Then re-run all five `preview/*` tests:
`key_space` (with A it also reads the row from the run's `trials.csv`),
`pause`, `edit_restart`, `choice` and `stale`. They are written to
`preview.h`'s contract: each waits for the response window
(`DSG_PV_PHASE_RESPONSE`) before it answers, and detects a restart by a
new data set folder. `test_designer_ui` parts 9 and 10 keep the double:
they need an end, a run-time refusal and a texture with fake GL caps on
demand. Done at the integration: the five pass on A after two changes to
the tests ("Parts A, B and C integrated").

**Tests** (Linux desktop, i5-14500, Debian 12, gcc 12.2):
- `test_designer_ui` (now 113 checks). Part 9, no GL caps: no session job
  before the settle and one after it; the session runs; the display is a
  gray box and the texture is never drawn; the banner at the session's
  rate (75 Hz); the roles not driven, each with why; the choice strip; the
  outputs' code and role after a choice; the data set's folder; `animating`
  while running, not while paused, not while the pane is hidden (the
  preview pauses then), and again after Run; no frame steps while paused; a
  text that does not parse keeps the session (no new adoption) and shows
  "Shows the definition as of HH:MM:SS; the text has N errors."; Ctrl+Z
  gives a new session after the settle; the end and a run-time refusal say
  so. Part 10, `ui_init()` again with a fake caps pointer: at 1:1 the image
  is 800 x 600 px at a whole px, uv (0, 1) at the top-left corner, the
  nearest callback just before it and the linear one just after it; the
  cursor at (100, 50) px gives (100, 50), once, and leaving gives `inside`
  false; at Fit, smaller at the same aspect, no sampler callback, the alias
  note, and the middle gives (400, 300) within a px.
- `test_designer_te`, `preview/*`, 5 tests on the double (list above), and
  32 of 32 in all.
- ctest from the Release build tree, examples and the Test Engine on,
  `DISPLAY` unset: 141 of 141. The Release build gives no compiler
  warning; B's four files give none with `-Wconversion -Wsign-conversion`
  either, but two `snprintf()` truncation notes in the double.
- ASan with UBSan (gcc 12, Debug, built and run under `setarch -R`,
  `halt_on_error`): the eleven designer tests, no report
  (`test_designer_te` with leak checks off, as in step 4).
- With `DSG_PREVIEW_DOUBLE` 0, the tests link S's stub: `test_designer_ui`
  passes its 66 checks of parts 1 to 8, and the five `preview/*` tests
  fail, as they must with no session.

**Hand mutants** (each reverted; each fails its test):

| Mutant | Fails |
|---|---|
| b-01: the texture drawn with no GL caps | part 9: no GL caps, the texture is not drawn; a gray box in its place |
| b-02: no nearest sampler at 1:1 | part 10: the nearest sampler before the image |
| b-03: rows not flipped (uv 0 to 1) | part 10: the top-left corner's uv is (0, 1) |
| b-04: the sampler callbacks at Fit too | part 10: Fit, no sampler change |
| b-05: the cursor not scaled to participant px | part 10: Fit, the middle is (400, 300) px |
| b-06: a still cursor sent each frame | part 10: a still cursor sends nothing more |
| b-07: leaving the display not sent | part 10: the cursor left the display |
| b-08: no faithful banner | part 9: the banner, at the session's rate |
| b-09: no stale banner | part 9: the stale banner; `preview/stale` |
| b-10: the roles not driven not listed | part 9: the roles not driven are listed |
| b-11: no `animating` hook | part 9: running, the view animates; `preview/pause` |
| b-12: Pause runs | `preview/pause` |
| b-13: a press on the display claims no keyboard | `preview/key_space` |
| b-14: the keyboard kept when the pane loses the focus | `preview/key_space` |
| b-15: a choice button sends the next choice | `preview/choice` |
| b-16: the outputs not listed | part 9: the outputs list the code with its role |
| b-17: Step runs | `preview/pause` |
| b-18: the banner at a fixed 60 Hz | part 9: the banner, at the session's rate |
| b-19: no gray box headless | part 9: a gray box in its place |

**Not done.**
- The window was not opened: the pane, the sampler callbacks and the key
  hook were not seen on a display, and no person used them. Part D's GL
  test reads the bytes.
- The pane's own cost per frame is not measured (part D measures the
  whole UI frame).
- A's real core was not linked: the five `preview/*` tests must run again
  after A merges.
- MSVC, clang and macOS were not compiled. Locals that a call fills are
  initialized, and the narrowing conversions are cast.

#### Parts A, B and C integrated (2026-11-21)

Branch `step5-int2`, from `step5-s` (`813e40b`): `step5-a-core` (A),
`step5-c-chart` (C) and `step5-b-pane` (B), merged in that order with
`--no-ff`.

**Conflicts.** Each part added the heading "Decisions of step 5" and its
subsection at the same place in this file, and an entry at the top of
`docs/status.md`. One heading keeps the three subsections, and
`status.md` keeps the three entries. Git merged the CMake files and the
code itself.

**Changes to the tests.**
- `test_designer_ui` part 8 ended with "no session: nowhere". With part
  A a session loads while the pane shows. The check now runs before the
  pane first shows (a hidden pane posts no session job), and the end of
  part 8 checks that `ui_preview_where()` gives a place exactly when a
  session shows (its data set's folder).
- `test_designer_ui_real` (new): `ui_test.cpp` with `DSG_PREVIEW_DOUBLE`
  0 and no double, so parts 1 to 8 run on A's `preview.c` with real
  sessions on the check thread (68 checks; it also checks that a session
  loaded). Parts 9 and 10 keep the double in `test_designer_ui`.
- `test_designer_te` runs on A's `preview.c`: `preview_double.c` is out
  of its sources, and `te_preview.cpp` defines `DSG_PREVIEW_DOUBLE` 0.
  `preview/choice` read the choices before a trial ran: A gives the
  running template's choices, so 0 between trials (`preview.h`). It reads
  them in the response window now. `preview/key_space` and
  `preview/choice` answered as soon as the window opened. rdk's collector
  waits from the trial's start, but its rt counts from the dots, 0.5 s
  in, so the press was early and the trial timed out (`n_early` 1 in
  `trials.csv`). The test now answers 0.8 s into the next trial's window.

**Faults found and fixed.**

| Part | Fault | Fix | Test that fails without the fix |
|---|---|---|---|
| A | A session that the preview drops as stale, because the text changed after its post, left `info.loading` true. When the new text does not check, no job follows, so the pane said "A new session loads" (or "Waiting") until the next accepted edit. A's own test (4a) relied on the flag. | `dsg_preview_adopt()` clears `waiting` when it drops the newest job's session and no job waits. | `test_designer_preview` (4d): "a session still loads, but no job waits or runs" |
| C (with A) | "Preview this template" with the pane never shown said "Not pinned: no leaf of the timeline runs 'reach'". A gives the leaves of the shown session only (`preview.h` says "the last accepted definition"), and a hidden pane loads none. This is the button's first use in a project. | The chart shows the pane and pins when the first session shows (above). | `chart/pin_real` and `chart/pin` (the pane does not show) |

After the hand test (2026-11-21): the display rescaled after the first
trial, because the "Last trial" row came in above it and Fit took the room
the rows left. The rows now keep their places ("The display", above), and
`preview/steady` checks the display's size across the first trial's end
and the stale banner. With the old layout it fails (403.6 to 368 points).

`preview.h` is frozen, so its comment on `dsg_preview_leaves()` still says
"the last accepted definition"; A's preview gives the shown session's.

**Tests** (Linux desktop, i5-14500, Debian 12, gcc 12.2):
- ctest from the Release build tree, examples and the Test Engine on,
  `DISPLAY` unset: 143 of 143. `test_designer_preview` 197 checks,
  `test_designer_ui` 114, `test_designer_ui_real` 68, `test_designer_te`
  37 of 37 (`chart/pin_real` and `chart/ring_real` run on A).
- ASan with UBSan (Debug, built and run under `setarch -R`,
  `halt_on_error`): the 12 designer tests (`test_designer_te` with leak
  checks off), `test_player_sim` and `test_gfx` with llvmpipe: no report
  but Mesa's known 336 bytes in 6 blocks in `test_gfx`.
- ThreadSanitizer (the same): the 12 designer tests, `test_player_sim`
  and `test_gfx`, with `race:swrast_dri.so`; then 5 more runs each of
  `test_designer_te` and `test_designer_preview`: no report.
- `-Wconversion -Wsign-conversion` on the designer's sources and tests:
  no warning. The merged diff was read for locals that a call fills
  (C4701, `-Wsometimes-uninitialized`) and narrowing conversions (C4244):
  none found.
- Hand mutants (each reverted): A's fix undone fails (4d); C's fix undone
  fails `chart/pin` and `chart/pin_real`; `ui_preview_where()` that gives a
  place with no session fails part 8 in both UI tests.

#### Part D as built (2026-11-21)

Branch `step5-int2`. Part D adds `tests/designer/preview_gl_test.cpp`: the
test `test_designer_preview_gl`, and with `--bench` the measurements of
"Redraw and cost" for the UI thread on a GL context.

**The test.** Linux only. It makes a pbuffer on EGL's device platform, with
no window, on the device whose `GL_RENDERER` contains `YSP_GL_DEVICE`
(default `llvmpipe`). It runs the UI as `main.cpp` does: the first gfx on
the context (64 x 64, its own output texture), `ui_init()` with its caps,
`ui_frame()`, then ImGui's OpenGL3 backend into framebuffer 0. A test
clock gives 16.7 ms a frame. CMake adds the test when it finds `libEGL`.
The test exits 77 (ctest: skipped) when EGL has no such device or the
stroop font is missing.

1. A copy of stroop at 1100 x 360 px, 60 Hz, 1:1, paused before the first
   session. The session loads on the check thread and attaches on the
   context. Then Step goes to frame 130: the first trial's word is on
   from frame 120 (banner 1.5 s, fixation 0.5 s).
2. After one ImGui frame, `glReadPixels()` of framebuffer 0 at the
   image's rectangle gives the texture's bytes, which the test reads
   through a framebuffer of its own: 0 of 1,584,000 bytes differ, and
   every alpha is 255. The image is 1100 x 360 px at a whole px, inside
   the clip rectangle of the display's child window.
3. The word is at its place, in its row's ink. Stroop's text box is
   1000 px wide at the display's center, and a text with no `align` is
   centered in it (ysp/experiment.h 0.7.0, 2026-11-23; format 2.11). So
   the word's ink is from column 469 to 633, its middle at 551 of 1100
   (the side bearings move it 1 px). Its middle row is 182 of 360. Its
   strongest channel is its row's ink (red). Before 0.7.0 its lines
   started at the box's left edge (`start`), and the word started at
   57 px.
4. Restart, the same seed, stepped again to frame 130 with no ImGui frame
   drawn between the steps: gfx gives the same bytes as with an ImGui
   frame before each step.

The pbuffer is 3800 x 1200 px, so the pane (45 % of what the side column
leaves) holds the 1100 px display at 1:1. Before ysp/experiment.h 0.7.0
the word started at the 1000 px box's left edge, so a smaller display
lost it: at 480 px it started at -260 px and the check found nothing.

Results (Linux desktop, i5-14500, Debian 12, Mesa 22.3.6, gcc 12.2,
Release): 19 of 19 checks on llvmpipe (LLVM 15.0.6), on the RX 6600
(radeonsi) and on the UHD 770 (iris); 1.2 s on llvmpipe. ASan with UBSan
and ThreadSanitizer (`setarch -R`): no report.

| Hand mutant (each reverted) | Result |
|---|---|
| d-01: the image's tint alpha 250, not 255 | fails 2: 1,187,795 bytes differ |
| d-02: rows not flipped (uv 0 at the top) | fails 2: 7,542 bytes differ |
| d-03: no `ygfx_reset_state()` before a drawn frame (`player/sim.c`) | survives. ImGui's OpenGL3 backend saves the GL state that it changes and puts it back, so gfx finds its own state. `test_player_sim` case (10) sets foreign state itself and keeps the reset tested. |
| d-04: the image 0.4 px off the px grid | fails 2: not at a whole px |
| d-05: no nearest sampler at 1:1 | survives. At a whole px each fragment samples a texel's center, where linear and nearest give the same value. The sampler matters only off the grid, which d-04 catches. |

**A fault found by the test** (not in A, B or C). `skt_shutdown()`
(`designer/app/skt.cpp`, step 3) deleted its glyph pages after it
unregistered them, but ImGui rebuilds the renderer's texture list only at
a frame's end. `ImGui_ImplOpenGL3_Shutdown()`, which `main.cpp` calls after
`ui_shutdown()`, then walked the list and read the freed pages (ASan:
heap use after free at the program's exit). `skt_shutdown()` now takes each
page out of that list too.

**The bench.** `test_designer_preview_gl --bench PROJECT HZ FRAMES
[hidden | WxH]`: the project opens with the pane shown at the default
display (1920 x 1080, Fit) at HZ, in a 1920 x 1080 window (a full
1080p monitor). The test clock gives 16.7 ms a frame, and the frames run
back to back. GNU ld's `--wrap` times `dsg_preview_tick()` and
`pl_sim_attach()`. Each frame's parts:

- tick: `dsg_preview_tick()`: the attach and the steps, only the last
  drawn.
- views: the rest of `ui_frame()`: every view, the pane too. With the pane
  hidden ("pane hidden" rows) no session runs; the difference is the
  pane's draw, about 0.03 to 0.08 ms on the RX 6600 (the main view is wider
  when the pane is hidden, so this is near, not exact).
- render: `ImGui::Render()`, the clear and `ImGui_ImplOpenGL3_RenderDrawData()`.
- finish: `glFinish()`, what a swap waits for, less the vsync.

Conditions: Linux desktop, Intel Core i5-14500 (`lscpu`: 2.6 GHz max),
Debian 12, kernel 6.11.5-liquorix, Mesa 22.3.6, gcc 12.2, Release. EGL's
device platform, no window. One run at a time under the measuring lock,
pinned with `taskset` to CPUs 2 and 4 (two P-cores), `LP_NUM_THREADS=2`
for llvmpipe. The 1-minute load average before each run was 0.40 to 0.99
(a run that began at 1.03 was repeated); after a run it was up to 1.0 on
the RX 6600 and up to 3.0 on llvmpipe, from the run's own threads. Mesa's
shader cache warm unless the row says cold. 1,000 frames after 60 warm-up
frames (300 for the generated case where the table says), two runs.
Milliseconds.

| Device | Project | Display | tick | views | render | finish | total: median, p99, max | run 2: median |
|---|---|---|---|---|---|---|---|---|
| RX 6600 | bimanual | 60 Hz | 0.04 | 0.16 | 0.02 | 0.57 | 0.82, 0.91, 0.92 | 0.81 |
| RX 6600 | bimanual | 500 Hz | 0.09 | 0.15 | 0.02 | 0.57 | 0.83, 0.92, 0.94 | 0.83 |
| RX 6600 | bimanual | pane hidden | 0.00 | 0.13 | 0.02 | 0.21 | 0.36, 0.38, 0.52 | 0.39 |
| RX 6600 | stroop | 60 Hz | 0.02 | 0.13 | 0.02 | 0.50 | 0.66, 0.90, 0.96 | 0.64 |
| RX 6600 | stroop | 500 Hz | 0.03 | 0.10 | 0.02 | 0.50 | 0.66, 0.90, 1.14 | 0.66 |
| RX 6600 | stroop | pane hidden | 0.00 | 0.05 | 0.01 | 0.14 | 0.20, 0.21, 0.25 | 0.20 |
| RX 6600 | oddball | 60 Hz | 0.02 | 0.11 | 0.02 | 0.50 | 0.65, 0.88, 0.93 | 0.66 |
| RX 6600 | oddball | 500 Hz | 0.03 | 0.11 | 0.02 | 0.50 | 0.65, 0.87, 0.94 | 0.65 |
| RX 6600 | oddball | pane hidden | 0.00 | 0.06 | 0.01 | 0.15 | 0.23, 0.27, 0.29 | 0.23 |
| RX 6600 | many | 60 Hz | 0.17 | 0.09 | 0.02 | 1.84 | 2.15, 2.58, 2.84 | 2.14 |
| RX 6600 | many | 500 Hz | 0.18 | 0.09 | 0.02 | 1.84 | 2.15, 2.67, 2.98 | 2.14 |
| RX 6600 | many | pane hidden | 0.00 | 0.06 | 0.01 | 0.14 | 0.22, 0.23, 0.87 | 0.22 |
| llvmpipe | bimanual | 60 Hz | 1.79 | 0.20 | 28.73 | 1.36 | 32.08, 32.83, 37.13 | 32.03 |
| llvmpipe | bimanual | 500 Hz | 1.81 | 0.19 | 28.70 | 1.36 | 32.06, 32.52, 35.52 | 32.05 |
| llvmpipe | bimanual | pane hidden | 0.00 | 0.14 | 0.35 | 11.64 | 12.12, 12.38, 15.24 | 12.11 |
| llvmpipe | stroop | 60 Hz | 2.12 | 0.13 | 28.05 | 1.07 | 31.37, 33.06, 35.66 | 31.40 |
| llvmpipe | stroop | 500 Hz | 2.10 | 0.13 | 28.04 | 1.07 | 31.35, 32.88, 33.19 | 31.35 |
| llvmpipe | stroop | pane hidden | 0.00 | 0.06 | 0.17 | 10.50 | 10.73, 10.78, 10.82 | 10.73 |
| llvmpipe | oddball | 60 Hz | 1.69 | 0.16 | 28.06 | 1.27 | 31.18, 32.83, 37.03 | 31.20 |
| llvmpipe | oddball | 500 Hz | 1.71 | 0.15 | 28.06 | 1.27 | 31.19, 32.81, 38.53 | 31.17 |
| llvmpipe | oddball | pane hidden | 0.00 | 0.07 | 0.19 | 10.58 | 10.84, 10.89, 10.93 | 10.84 |
| llvmpipe | many | 60 Hz | 456 | 0.20 | 28.10 | 1.08 | 486, 495, 496 | 486 |
| llvmpipe | many | 500 Hz | 456 | 0.20 | 28.09 | 1.08 | 486, 495, 496 | 486 |
| llvmpipe | many | pane hidden | 0.00 | 0.07 | 0.18 | 10.54 | 10.79, 10.88, 11.00 | 10.79 |

llvmpipe with more cores, and with a smaller display (the same conditions
otherwise; total median, p99, max of run 1; run 2's median):

| llvmpipe | Project | Display | total | run 2: median |
|---|---|---|---|---|
| CPUs 2, 4, 6, 8, 10, 4 threads | bimanual | 60 Hz / 500 Hz | 16.61, 16.85, 17.20 / 16.61, 16.87, 16.97 | 16.61 / 16.61 |
| CPUs 2, 4, 6, 8, 10, 4 threads | stroop | 60 Hz / 500 Hz | 8.18, 8.54, 8.66 / 8.20, 8.56, 8.63 | 8.17 / 8.18 |
| CPUs 2, 4, 6, 8, 10, 4 threads | oddball | 60 Hz / 500 Hz | 16.13, 16.93, 17.00 / 16.14, 16.95, 17.18 | 16.14 / 16.12 |
| CPUs 2, 4, 6, 8, 10, 4 threads | many (300 frames) | 60 Hz / 500 Hz | 246, 247, 248 / 246, 247, 249 | 246 / 246 |
| CPUs 2 and 4, 800 x 600 px | stroop | 60 Hz | 15.51, 16.73, 17.30 | 15.52 |
| CPUs 2 and 4, 800 x 600 px | many (300 frames) | 60 Hz | 303, 304, 309 | 303 |

The frames of the first adoption: those with attach work and the three
after them. The largest frame's total, and the largest single
`pl_sim_attach()` call, in two runs (ms). Cold: `MESA_SHADER_CACHE_DISABLE`.

| Device | Cache | bimanual | stroop | oddball | many |
|---|---|---|---|---|---|
| RX 6600 | warm | 5.79, 5.74 (0.68, 0.78) | 6.31, 6.83 (1.66, 1.88) | 6.65, 6.55 (1.78, 1.72) | 7.39, 7.20 (2.12, 2.14) |
| RX 6600 | cold | 6.14, 5.99 (0.78, 0.79) | 71.6, 71.0 (31.0, 31.1) | 37.8, 38.2 (16.7, 17.8) | 71.9, 71.6 (30.3, 30.1) |
| llvmpipe | warm | 33.5, 33.9 (6.81, 6.96) | 17.6, 17.6 (6.60, 6.49) | 31.9, 31.8 (6.97, 6.15) | 31.8, 31.7 (6.79, 6.75) |
| llvmpipe | cold | 260, 262 (248, 250) | 259, 259 (248, 249) | 261, 261 (250, 250) | 261, 261 (250, 250) |

On the RX 6600 a warm adoption takes 4 frames; the largest one's other
part is ImGui's render or the finish (4.2 to 4.7 ms), the GPU's first use
of the new texture and programs. On llvmpipe a cold adoption takes 18 to
20 frames with attach work, 1.2 to 1.4 s in all, and one call of 248 to
250 ms: one program's compile.

**Against the 16 ms frame.**

- **RX 6600: every case fits.** The running preview costs 0.6 to 2.2 ms a
  frame (p99 at most 2.7 ms, max 3.0 ms), the generated case the most: its
  GPU time (finish, 1.84 ms). A 500 Hz display costs what a 60 Hz one
  does, except for the tick's extra steps (0.05 ms in bimanual). A warm adoption's
  largest frame is 7.4 ms. No knob needs a turn.
- **RX 6600, cold driver cache: the adoption does not fit** once: 38 to
  72 ms, 2 to 4 frames lost, then the cache is warm. One compile is one
  call, so `PV_ATTACH_BUDGET_MS` cannot split it. The knob is a program
  cache: `dsg_preview_gl.cache`, which `ui.cpp` leaves NULL (gfx's
  `ygfx_file_cache`). Not measured here.
- **llvmpipe on two cores: nothing fits while the preview runs** (31 to
  32 ms; with the pane hidden, 10.7 to 12.1 ms). The cost is the session's
  drawn frame: ImGui's render waits for it (28 ms). The knob is the
  display's size: stroop at 800 x 600 takes 15.5 ms (p99 16.7 ms), at the
  edge. More cores also help: with four llvmpipe threads on five cores
  stroop takes 8.2 ms, bimanual and oddball 16.1 to 16.6 ms (at the edge).
- **The generated case on llvmpipe does not fit with any measured knob:** 486 ms
  on two cores, 246 ms on five, 303 ms at 800 x 600. Its 200 text stimuli
  are the same size in px on any display, so a smaller display cuts little.
  It needs a knob that the plan does not have, such as a session frame
  drawn less often than the UI's frame. That is a design choice; it is
  not made.
- **llvmpipe, adoption:** warm, attach calls stay under 7 ms; cold, one
  call of 250 ms. The same program cache is the knob.

**Not done.**
- The window was not opened: the test and the bench use a pbuffer.
  `main.cpp`'s swap and vsync are not in the numbers.
- gfx's program cache in the designer is not wired or measured.
- The UHD 770 ran the test, not the bench.
- MSVC, clang and macOS were not compiled. The test is Linux only.

### Decisions of step 6

#### Part P as built (2026-11-22)

Branch `step6-p`, from `designer-step6` (`55f27ea`). The player 0.18.0
([player.md](player.md), "With a rig profile" and Results). Only P's files
changed: `player/main.c`, `player.h`, `lower.c`, `run.c`, the player's
tests and mutation lists, `docs/player.md`, `docs/status.md`.

**What it gives the other parts.**
- `ysp_player --version`: one line, `ysp_player VERSION` with
  `PL_VERSION_STRING`, exit 0, for `dsg_player_version()` (part R,
  decision 5). It is read before any other argument.
- `--sim` and SIGTERM (decision 8, "Stop"): exit 3, `"aborted"` with
  reason `request`, the finished trials' rows. The stop line is
  `ysp_player: stopped by the operator; N trials written to DIR (trial K
  has no row)`, or `(no trial was running)` when the stop comes between
  trials; through 0.17.0 that case printed `trial -1`. The handler only
  sets `pl_stop_requested` (`volatile sig_atomic_t`); `pl_run()` reads it
  before each frame and calls `yscr_request_abort()`, so the stop takes
  the path of every other abort. It is installed before the load, so a
  Stop during the load ends the session at its first frame with no
  trial. A real run does not install it (SDL makes SIGTERM a quit event,
  reason `quit`). **Windows**: no process can send SIGTERM to another, and
  a `--sim` run has no window for `WM_CLOSE`. So on Windows the designer's
  Stop does nothing to a `--sim` run; it ends at its end, or by Kill
  (`TerminateProcess()`: no `"aborted"`, the data set not closed). Part R
  should show Kill at once for a simulated run on Windows, or not offer
  Stop there.
- `pl_check_desc.rig` (decision 13): the profile that `dsg_rig_load()`
  loads, and in `cal` the profile's calibration when it names one. The
  needs go as the plan's table says, with two readings:
  - A role tier need that stays is always at UNKNOWN: ysp/rigfile.h gives
    a role tier 1 or UNKNOWN, and an output UNKNOWN. So `pl_need` gained no
    field for "the profile's tier"; the Run view can say "the profile
    gives it no tier" for each `PL_NEED_ROLE_TIER` when a profile is
    chosen.
  - The plan's "rules and buttons checked against the bound family (for
    the bound roles)" was already true: since 0.15.0 the check runs them
    for every role that a rig can bind, and a profile can bind only a
    family that the role lists, never the keyboard. The profile adds and
    drops no rule check.
  - The calibration need goes only when the profile's calibration passed
    its check and `cal` is given. The check reads no file, so part G must
    read the calibration with the profile and pass it.
  - Profiles bind roles by name. The test's first profile bound `resp` to
    an xid, and every keyboard example (rdk, stroop, afc2, all with a
    keyboard role `resp`) was refused: `role 'resp': the rig binds it to
    family 'xid', which the role does not list`. A real start with that
    profile refuses them too, so this is right, but a person who keeps one
    `profile.json` for one rig sees it on every other project. Part V may
    want the check's issue to name the profile.

**P2: the stepped session takes the profile** (decision 14, accepted
2026-11-22; the plan had given this player change to no part).
`pl_sim_load()` with `pl_sim_desc.rig` runs the same rules as the check:
the gates in `lower.c` key on "a stand-in load with a profile"
(`p->stand_in && p->have_rig`), not on `p->checking`, so there is one copy
of each rule. Its needs and refusals equal `pl_check()`'s with the same
profile and with `pl_sim_desc.cal` as `cal`. With a profile, a given
calibration meets a required one only when the profile names a
calibration whose check is OK; with no profile, as in 0.16.0, a given one
meets it. A relative signal role whose signal is in the profile takes the
profile's adapter, so **a sample's meaning changes**: through
`pl_sim_sample()`, a relative role's x and y are a raw mouse's counts at
the profile's `per_count` (converted through its view, with its sign and
bounds), not role units at gain 1. Part G or V must turn the pane's
mouse movement into counts at that scale when a profile is chosen (for
example, divide the role-unit motion by the profile's scale), or the
preview's hand moves by the profile's factor. player.md, "Time and
input", has the table. With no profile, the stepped session's bytes are
0.17.0's (248 files).

**Found wrong or missing in the plan.**
- Decision 14 needed a player change that no part owned; built as P2,
  above.
- The brief's list of git-ignored folders to copy missed `sheenbidi`,
  `skribidi`, `skribidi-extra` and `stb`; `YSP_BUILD_LAYOUT` does not
  configure without them.

**Tests.** `test_player_check` `case_rig`: profiles parsed against a rig
folder that the test writes (a bounds loopback for an xid role, the
nominal calibration, `player_make_rig`'s view, signals for hand, left and
right), not `player_make_rig` itself, so the mutation runner's build of
the test needs no other program. `player_test.py` `t_p18_version` and
`t_p18_sigterm` (skipped on Windows). `--only` takes a list, and
`mut_test.c` passes `YPL_SHAPES` to it, so `player_main.toml`'s mutants run
two cases, not the suite. Numbers in player.md, Results.

**Not done.**
- No real run: the real run's Stop through SDL is still the plan's small
  test program, not a run.
- MSVC, MinGW, clang and macOS were not compiled; clang 24's
  `-fsyntax-only` of the changed C files gave no new warning.
- The designer suite ran with the Test Engine on only.

#### Part K as built (2026-11-22)

Branch `step6-k`, from `designer-step6` (`55f27ea`). Part K is the pack
tool 0.12.0 ([pack.md](pack.md), section 19): decisions 4 and 17. No
designer file changed.

**The API that parts B and V use.**

```c
ypt_options o;                       /* zero it first, as before         */
o.cancel = build_canceled;           /* int fn(void* ctx): nonzero stops */
o.cancel_ctx = builder;              /* read an atomic flag in fn        */
o.cache = ".../PROJECT/.designer/cache/entries";   /* made when missing */
o.cache_max = 2LL << 30;             /* part B's knob; 0: no bound       */
rc = ypt_build(desc, out, &o, err, sizeof err);    /* YPT_ERR_CANCELED  */
```

- The check pack and Build pass the same folder; two builds at once, in
  threads or processes, are safe (a temporary file and a rename; a
  record that does not check out is a miss).
- `ypt_cache_trim(dir, 0, NULL, err, cap)` empties the folder (a "Clear
  the cache" command, if part V wants one).
- With `o.log`, an entry from the cache ends its line with `(cache)`.

**What the cache holds**: textures, converted or resampled audio, and
each video with its index (a copied video: its index). The other kinds
cost less than their source's hash or depend on other entries. The key
is the source's SHA-256, the resource as written, the project's audio,
the tool's and libraries' versions, and for ffmpeg its whole `-version`
text and the command.

**Measured** (pack.md 19 has the conditions and the tables): the 10 s 1080p video case builds in
3.48 s without a cache, 3.50 s with a cold one (the 27.1 MB record
written) and 0.236 s with a warm one (i5-14500, CPUs 2 and 4, load 0.25
before, median of 20). On the repository's projects the cache changes
nothing measurable (0.09 to 6.3 ms either way): only oddball has keyed
entries, two small textures.

**Decided by measurement: hash the source every build.** The manifest
needs each source's SHA-256, so the build hashes every source anyway; a
size-and-time check could only save that hash by writing a remembered
one into the manifest, and a source changed within one time step at the
same size would then give a wrong pack.

**Where the plan was wrong or short.**
- "No cancel gives the same bytes as 0.11.0" cannot hold for the
  manifest: it records the tool's version, so every 0.12.0 pack has
  another manifest and pack ID. Every other entry is byte-identical (the
  corpus pin is unchanged); the test is that pin.
- Decision 4 reads the cancel "before an ffmpeg step". That read is the
  one before the step's resource; no test could tell them apart, so the
  tool reads before each entry, before the glyph runs' derivation, and
  after an ffmpeg step. A video's index (it decodes every frame) cannot
  be canceled either: seconds for a copied 1080p MPEG-1 file.
- Decision 17's key ("the source's bytes, the derivation's options and
  the tool's version") is short: the libraries' versions, the project's
  audio and, for video, ffmpeg's whole `-version` text are inputs too.
- A warm cache does not make the check pack of a media project cheap:
  a hit on the video case still costs 236 ms (the source's
SHA-256 87 ms, `ffmpeg -version` 33 ms, the entry's hashes and the
pack's write), at each check pack build. Part B may want to skip the check pack's build when no
  source and no resource changed, or build it without the media.

**Tests**: `test_pack_tool` 473 checks (63 new), Release, ASan with
UBSan, and ThreadSanitizer; the fuzz target `cache_fuzz` (ctest
`cache_fuzz_short`); 37 of 37 hand mutants killed (pack.md 19).

**Part K2: the ffmpeg step's child** (2026-11-22, after part X found it).
`run_program()` gave ffmpeg every inheritable handle on Windows and every
descriptor without close-on-exec on POSIX, so a Build's ffmpeg could
hold the player's log open. Now: on Windows a handle list of the null
device and the pipe; on POSIX `addclosefrom_np(3)` (glibc 2.34 and
later; macOS `POSIX_SPAWN_CLOEXEC_DEFAULT`), a close-on-exec pipe, the
default signal actions and SCHED_OTHER. Before, a SCHED_FIFO build
thread would have passed its policy to ffmpeg; no build thread is
elevated, and now the child is SCHED_OTHER regardless. Tested on Linux
with the test program as a fake ffmpeg that lists `/proc/self/fd` (3
checks, 3 of 3 mutants killed); the Windows branch only syntax-checked
with clang 24 against stubs (pack.md 19, "The ffmpeg step's child").

**Open.** Windows and macOS not compiled or run here (CI); the hit could
skip the entry's SHA-256 by storing it in the record (pack.md 19,
"Left").

#### Part X as built (2026-11-22)

Branch `step6-x`, from `designer-step6` (`55f27ea`). Part X adds the
process layer `dsg_proc_*` to `designer/core/platform.c` and the plan's
declarations to `dsg.h`, with one change: `dsg_proc_stop()` returns
`int` (decision 7, below). The version of `dsg.h` stays 0.3.0: part S
makes it 0.4.0. Part X also
changed `designer/CMakeLists.txt` (S's file): the target `test_child`,
and its path as the second argument of `test_designer_platform`. The
plan gives `test_child` to S, but X merges before S, and X's test needs
it.

**What it does.**
- **Start, POSIX.** `posix_spawn()` of `argv[0]` (no PATH search). The
  log is made with `O_CREAT|O_EXCL`, so an old log is never written over.
  File actions: `/dev/null` on descriptor 0, the log on 1 and 2,
  `posix_spawn_file_actions_addclosefrom_np(3)` (glibc 2.34 and later;
  macOS: `POSIX_SPAWN_CLOEXEC_DEFAULT`), and the working folder
  (`addchdir_np`). Attributes: SCHED_OTHER with priority 0
  (`POSIX_SPAWN_SETSCHEDULER`, where the system has it; macOS does not),
  every signal but SIGKILL and SIGSTOP at its default action, an empty
  mask, and a process group of its own. When the spawn fails, the log is
  removed.
- **Start, Windows.** `CreateProcessW()` with the program's path as
  `lpApplicationName` and a command line that the C runtime splits back
  into the same argv (the pack tool's rules: quote an argument with a
  space, a tab, a quote or nothing in it; in it, double the backslashes
  before a quote and at the end, and escape the quote). UTF-8 to UTF-16
  with `MB_ERR_INVALID_CHARS`: an argument that is not UTF-8 is refused.
  The log is made with `CREATE_NEW`. `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`
  has two handles, NUL and the log (one handle serves standard output
  and standard error), so no other inheritable handle of the designer's
  goes to the child. `NORMAL_PRIORITY_CLASS`, `CREATE_NO_WINDOW` (a
  console program gets no console window, and the designer's console's
  Ctrl+C does not reach it), then `AllowSetForegroundWindow(pid)`.
- **The watch thread.** One per run, made with
  `PTHREAD_EXPLICIT_SCHED` and SCHED_OTHER. It blocks in `poll()` on a
  stop pipe and, on Linux 5.3 and later, a pidfd of the child, with a
  timeout of 250 ms (Windows: `WaitForMultipleObjects()` on a stop event
  and the process handle). At each return it reaps the child if it ended
  (`waitpid(WNOHANG)`), reads the sizes of `out_path` and of each watched
  file (`stat()`), and calls `wake` when a size changed or the child
  ended. It ends after the exit's wake. `out_path` is always watched.
- **Stop, Kill, poll.** SIGTERM and SIGKILL; on Windows, `WM_CLOSE` to
  each top-level window of the child (`EnumWindows()`). Stop gives 1
  when the request went out, and 0 when the child has ended or, on
  Windows, has no top-level window. Kill is `TerminateProcess()` on
  Windows, with the exit code -9, so that `dsg_proc_poll()` gives -9 on
  each system. The reap and the signals are under one lock: a pid is
  never signaled after its reap, when another process can have it.
  `dsg_proc_poll()` reaps too, so its answer does not wait for the watch
  thread.
- **Free.** When the child ended: the stop pipe, then a join. When it
  runs: the child is left running, and the watch thread is detached; it
  blocks in `waitpid()`, so no zombie stays, then frees the struct.
  `wake` is called under a second lock with a "gone" flag, so it is
  never called after `dsg_proc_free()` returns. Windows keeps no zombie:
  free joins and closes the handles.
- **WebAssembly.** `dsg_proc_start()` fails with "no processes in a
  browser"; the other calls do nothing (`dsg_proc_poll()` gives 1 and
  -1).

**Decisions taken here.** S, or the user, can reverse each one.
1. **The watch: a pidfd and a 250 ms tick, not inotify.** The UI draws a
   run's progress at most 2 times a second (`RUN_REDRAW_HZ`), so a wake
   per write gives it nothing. inotify wakes at each write of the log
   and of `trials.csv`, and `trials.csv` is in a folder that the player
   makes after the start, so it needs a watch on the parent folder too.
   macOS and Windows have no same call. The tick reads 1 + N sizes;
   measured below, the whole process uses 91 us of CPU a second while a
   child runs. The pidfd costs one descriptor and makes the exit's
   wake 0.034 ms (median) in place of up to 250 ms.
2. **Every signal at its default, not only SIGTERM, SIGINT and SIGPIPE**
   (the plan's list). Any signal that the designer ignores stays ignored
   across `exec()`: SIGHUP, SIGCHLD, SIGQUIT too.
3. **A process group of its own** (not in the plan). A Ctrl+C in the
   designer's terminal sends SIGINT to the terminal's foreground group;
   SDL turns it into `SDL_EVENT_QUIT`, which aborts the session. Windows
   gets the same from `CREATE_NO_WINDOW`.
4. **With `cwd`, `argv[0]` must be absolute.** `posix_spawn()` looks for
   a relative path after the change of folder, `CreateProcessW()` from
   the designer's folder. The designer passes an absolute path
   (`SDL_GetBasePath()`), so the refusal costs it nothing.
5. **No job object on Windows.** A job with
   `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` ends the player when the designer
   ends, which "Leave it running" refuses. Without that limit a job adds
   nothing for one process that starts no other: `TerminateProcess()`
   ends it.
6. **Free of a running child detaches the watch thread** (the plan says
   free joins it). With a join and no thread left, the player that ends
   later stays a zombie until the designer ends.
7. **`dsg_proc_stop()` returns `int`** (the plan: `void`). Part P found
   that `ysp_player --sim` opens no window, so on Windows `WM_CLOSE`
   reaches nothing and Stop of a simulated run does nothing. A 0 tells
   part R to offer Kill at once, not after 5 s. A real run's window can
   be not yet made in its first moments; Stop then also gives 0.

**Measured.** Linux desktop (i5-14500, Debian 12, kernel
6.11.5-liquorix, governor `performance`), gcc 12.2, Release (Ninja),
2026-11-22, from `ce0e7b2`. One run under the measuring lock, pinned with
`taskset -c 2,4`, `DISPLAY` and `WAYLAND_DISPLAY` unset, the files on
`/tmp` (ext4 on NVMe). Load average 0.95 before, 0.26 after.
`test_designer_platform WORK CHILD --measure`. Median, p95 and max.

| What | Samples | Result |
|---|---|---|
| `dsg_proc_start()`, the caller's time (the log, the spawn, the pidfd, the thread) | 200 | 88.5, 134.9, 171.6 us |
| The child's last write before its exit, to the wake (`test_child lines 0 0`) | 200 | 0.034, 0.045, 0.072 ms |
| A watched file's write, to the wake (random phase of the tick) | 100 | 128, 231, 240 ms |
| CPU of the whole process while a child sleeps (the UI thread asleep) | 10 s | 91 us a second |

The growth's wake is the tick: from 0 to 250 ms, as expected, under the
plan's 300 ms. A first run rewrote the file with text of the same length,
and most writes gave no wake: the watch sees sizes, not writes. The
player only appends, so that is enough. The start's call is the plan's `posix_spawn()` (42 us
median) plus the log, the pidfd and the thread.

**Tests.** `test_designer_platform` gets `test_child`'s path as its
second argument; without it the process part is skipped. 106 checks
on Linux (47 before). Each child that the test starts is `test_child`;
none spins, and each wait ends by itself after 30 s.
- The exit code (7); "out line" and "err line" in the log.
- argv arrives whole: spaces, an empty argument, quotes, backslashes
  before a quote and at the end, a UNC-like path with a trailing
  backslash, a tab, UTF-8 (the child prints each argument's bytes in
  hex; Windows: it reads `GetCommandLineW()` back with
  `CommandLineToArgvW()`).
- Descriptors: with a descriptor and a pipe of the test's open without
  close-on-exec, and a pipe on the test's standard input, the child has
  0, 1 and 2 only, its standard input is `/dev/null`, and it leads a
  process group of its own. Windows: an inheritable event of the test is
  not inherited.
- Policy (Linux, with an `rtprio` grant; skipped without): the test's
  thread at SCHED_FIFO 1 starts the child, then goes back. The child and
  the watch thread are SCHED_OTHER. Windows: the test at
  `BELOW_NORMAL_PRIORITY_CLASS` starts a child of the class NORMAL.
- Signals (POSIX): with SIGPIPE ignored and SIGTERM, SIGINT and SIGPIPE
  blocked in the test, the child has SIGPIPE at its default and none
  blocked.
- The working folder; a relative program with one is refused.
- Stop ends a child that takes SIGTERM (Windows: `WM_CLOSE` to its
  hidden window) with its code (5), and gives 1; Stop of the ended child
  gives 0. A child that ignores it runs 300 ms after Stop; Kill ends it
  with -9. Windows: Stop of a child with no window gives 0.
- The wake: each of 4 log lines, 330 ms apart, wakes within 300 ms of
  the child's write (the child prints its monotonic time); a file in
  `watch` that the test writes wakes within 300 ms; the exit wakes within
  50 ms on Linux (the pidfd), within 300 ms elsewhere. A child that
  writes nothing wakes at its exit.
- No spin: while a child sleeps 1 s, the process uses less than 50 ms of
  CPU.
- A log that exists is refused, and its bytes stay; a program that does
  not start leaves no log; a NULL description is refused.
- No zombie: after the frees, `waitid(WNOWAIT)` finds no child to reap.
  A child freed while it runs still runs; after the test ends it, it is
  reaped (`kill(pid, 0)` gives ESRCH), and `wake` was not called after
  the free, though a watched file grew.

Release: 20 of 20 runs pass at load 8 (other jobs). ASan with UBSan and
ThreadSanitizer (gcc 12, Debug, built and run under `setarch -R`,
`halt_on_error`): 3 of 3 runs each, no report. ctest from the Release
tree (examples and the Test Engine on, `DISPLAY` unset): 144 of 144.

**Hand mutants** (each reverted; each fails `test_designer_platform`):

| Mutant | Fails |
|---|---|
| x-01: no `addclosefrom_np` | fds: `fds 0 1 2 3 4 5 6` |
| x-02: no `POSIX_SPAWN_SETSCHEDULER` | policy: the child is FIFO |
| x-03: the watch thread inherits the policy | policy: the watch thread is FIFO |
| x-04: no `POSIX_SPAWN_SETSIGDEF` | signals: `sigpipe ignored` |
| x-05: no `POSIX_SPAWN_SETSIGMASK` | signals: 3 blocked |
| x-06: no `POSIX_SPAWN_SETPGROUP` | fds: `pgrp parent` |
| x-07: standard input not `/dev/null` | fds: `stdin other` |
| x-08: standard error not the log | exit: no "err line" |
| x-09: `O_TRUNC` in place of `O_EXCL` | exists: not refused, the old log gone |
| x-10: a failed start keeps its log | missing: the log is left |
| x-11: the raw wait status | exit: 1792 |
| x-12: a signal as a positive status | kill: 9 |
| x-13: Stop sends SIGKILL | stop: -9, no "got term" |
| x-14: Kill sends SIGTERM | kill: the child does not end in 5 s |
| x-15: a 1000 ms tick | watch: 671 ms for a line, 551 ms for a file |
| x-16: no look at the sizes | watch: lines and file late |
| x-17: no pidfd | watch: the exit after 181 ms |
| x-18: a `poll()` with no timeout (a spin) | block: 998 ms of CPU in 1 s |
| x-19: free of a running child leaves the thread as it was | left: 2 wakes after the free |
| x-20: the detached thread does not reap | left: a zombie |
| x-21: the status read with `WNOWAIT` (no reap) | zombie: one after the frees |
| x-22: no change of folder | cwd: the test's folder |
| x-23: a relative program with a working folder accepted | cwd: not refused as "absolute" |
| x-24: no wake at the exit | quiet: no wake |
| x-25: Stop always gives 1 | stop: an ended child gets nothing |

The "gone" flag guards a race (a tick's wake that starts just as free
returns); no single mutant of it fails a test.

**Checked only in part, or only by CI.**
- Windows: not compiled with the SDK, and not run. clang 24
  (`--target=x86_64-pc-windows-gnu`, `-fsyntax-only`) compiled the three
  files against hand-written stubs of the Win32 declarations: the
  syntax, the types and `-Wsometimes-uninitialized` only. The quoting
  function, taken out of `platform.c`, gave back 97,656 argument lists
  (every string of up to 7 of `a`, space, tab, quote and backslash,
  beside a fixed one and an empty one) through a parser of the C
  runtime's documented rules. CI runs the test on Windows first.
- macOS: not compiled. `addchdir_np` is macOS 10.15 and later; if the SDK
  marks it deprecated, `-Werror` fails there.
- glibc before 2.34, and musl: no `addclosefrom_np`, so a descriptor
  without close-on-exec reaches the child, and the fds check fails.
- The pack tool's `run_program()` (ffmpeg) inherits every inheritable
  handle on Windows. If the build thread starts ffmpeg while the
  designer starts the player, ffmpeg can get the log's handle (it is
  inheritable from its creation to the end of `dsg_proc_start()`). The
  fix is the pack tool's: a handle list in `run_program()` (part K or a
  later pack tool release).
- clang 24 for Linux (`-fsyntax-only`, glibc's headers,
  `-Wsometimes-uninitialized`) and emcc (the WebAssembly stubs): no
  warning.

#### Part S as built (2026-11-22)

Branch `step6-s`, from `designer-step6` (`2d367ba`: the plan, P, K and X
merged). Part S gives parts B, R, G and V frozen interfaces and stubs, as
part S of step 5 did. With the stubs the designer runs as before step 6:
Build and Run are in the toolbar, and each says that it is not built yet;
the Run view says the same; the check and the preview run with no rig
profile.

**Decided for part S** (by the coordinator, 2026-11-22):
- Part X's deviations are accepted as built: `int dsg_proc_stop()` (0:
  nothing went out, so Kill shows at once, for example `--sim` on
  Windows); a free of a running child detaches its watch thread; every
  signal at its default; a process group of its own; with `cwd`, `argv[0]`
  must be absolute; no job object on Windows.
- The findings of parts P and K go into the interfaces and into the list
  of what each part of wave 2 does (below).

**The frozen interfaces.** After part S, parts B, R, G and V change none
of these files. A part that needs a change stops and reports it.

| File | What it gives | Who calls it |
|---|---|---|
| `designer/core/dsg.h` (0.4.0) | `DSG_FROM_BUILD`, `DSG_FROM_PLAYER_RUN`, `DSG_FROM_RIG`; `dsg_issue.rig`, `dsg_report.rig`; `DSG_RIG_NAME`, `DSG_CACHE_DIR`, `DSG_BUILD_DIR`; the rig, Build and Run sections (below); `dsg_checker_set_rig()`; `dsg_worker_check(..., rig)`; `dsg_session_settings.rig` | B, R and G implement them; V and `ui.cpp` call them |
| `designer/core/run_part.c` | `dsg_run_settings_default()`, `dsg_run_part_read()`, `dsg_run_part_write()`: the view file's part `run` | `ui.cpp` only |
| `designer/core/preview_part.c`, `preview.h` | the part `preview` keeps `"rig"` (`session.rig`); `dsg_preview_set()` restarts on a change of it (G) | `ui.cpp`; G in `preview.c` |
| `designer/core/worker.c` | the check job carries the rig's name and calls `dsg_checker_set_rig()` before each check | the UI |
| `designer/app/views.h` | `ui_aux_view_open()`, `ui_show_aux_view()`; `ui_build()`, `ui_can_build()`, `ui_build_busy()`, `ui_build_last()`, `ui_build_stale()`, `ui_can_run()`, `ui_run_start()`, `ui_run_stop()`, `ui_run_kill()`, `ui_run()`, `ui_run_settings()`, `ui_check_again()` | V |
| `designer/app/ui.h` | `UiPlatform.player_path`, `UiPlatform.wake_run`; `ui_run_active()`, `ui_run_wait_ms()`, `ui_wait_builds()`; `ui_report()` is the merged report | `main.cpp`, the tests |
| `designer/app/ui.cpp`, `main.cpp`, `views.cpp` | below | |
| `designer/CMakeLists.txt` | the four new core files, `view_run.cpp`; `ysp_player` copied beside `ysp_designer` | |
| `tests/designer/build_run_double.h`, `.c` | a double of `build.c` and `run.c` for `test_designer_ui` | S's tests |

`dsg.h`, the rig section (part G, `rig.c`). A `dsg_rig` is one thread's:
the checker keeps one on the check thread, a session's load one, the Run
view one on the UI thread.
```c
int  dsg_rig_list(char (*names)[DSG_RIG_NAME], int cap, char* err, size_t err_cap);
int  dsg_rig_load(const char* name, dsg_rig** out, char* err, size_t cap);  /* and its calibration */
void dsg_rig_free(dsg_rig* r);
const char* dsg_rig_name(const dsg_rig* r);
const char* dsg_rig_path(const dsg_rig* r);
const struct yrig_profile* dsg_rig_profile(const dsg_rig* r);
const struct ycol_cal* dsg_rig_cal(const dsg_rig* r);    /* NULL unless its check is OK */
bool dsg_rig_changed(const dsg_rig* r);                   /* size or time, a stat each */
int  dsg_rig_summary(const dsg_rig* r, char* out, size_t cap);   /* the banner's line */
bool dsg_rig_counts_per_unit(const dsg_rig* r, const char* role, int role_units, double out[2]);
void dsg_checker_set_rig(dsg_checker* c, const char* name);       /* check.c */
```

`dsg.h`, the Build section (part B, `build.c`):
```c
dsg_builder* dsg_builder_start(const dsg_builder_desc* d, char* err, size_t cap);  /* wake, ctx, no_thread */
void dsg_builder_stop(dsg_builder* b);
int  dsg_build_path(const dsg_project* p, char* out, size_t cap);     /* PROJECT/build/NAME.ysppak */
int  dsg_build_post(dsg_builder* b, const dsg_project* p, const char* out_path, uint64_t revision, char* err, size_t cap);
void dsg_build_cancel(dsg_builder* b);
bool dsg_build_busy(const dsg_builder* b);
bool dsg_build_take(dsg_builder* b, const dsg_build_result** out);
int  dsg_build_wait(dsg_builder* b, int64_t timeout_ns);
bool dsg_build_stale(const dsg_project* p, const dsg_build_result* r, uint64_t revision);
int  dsg_build_check_pack(const dsg_project* p, const char* desc, size_t desc_len, const char* out_path, char* err, size_t cap);
```
`dsg_build_result`: revision, status (`DSG_BUILD_NONE`, `_OK`, `_FAILED`,
`_CANCELED`), a `dsg_report` of `DSG_FROM_BUILD` issues, path, size,
`pack_id[72]` (`ypak_id()`'s form), ms, `done_unix_s`, and the stamp
(`dsg_build_source`: absolute path, size, modification time).

`dsg.h`, the Run section (part R, `run.c`; S's `run_part.c`):
```c
typedef struct dsg_run_settings {     /* the view file's part "run", but out_dir */
    char rig[DSG_RIG_NAME]; bool sim; char participant[32];
    int32_t n_fields; dsg_session_field fields[DSG_SESSION_FIELDS];
    char seed[24]; int32_t n_below; char allow_below_tier[DSG_RUN_ROLES][32];
    bool windowed; double max_seconds; int32_t n_gains; dsg_run_gain gains[DSG_RUN_ROLES];
    char out_dir[DSG_PATH_MAX];
} dsg_run_settings;
int  dsg_run_args(const dsg_run_settings* s, const char* player, const char* pack, const char* out_dir,
                  dsg_run_argv* out, char* err, size_t cap);
int  dsg_run_out_dir(const char* folder, const dsg_run_settings* s, int64_t now_unix_s, char* out, size_t cap);
const char* dsg_run_exit_text(int status, char* buf, size_t cap);
int  dsg_player_version(const char* player, const char* tmp_dir, char* out, size_t cap, char* err, size_t err_cap);
int  dsg_run_start(dsg_run** out, const dsg_run_desc* d, char* err, size_t cap);
int  dsg_run_stop(dsg_run* r, int64_t now_ns);     /* 0: nothing went out, Kill now */
void dsg_run_kill(dsg_run* r);
bool dsg_run_tick(dsg_run* r, int64_t now_ns);     /* the UI thread, a frame */
void dsg_run_get_info(const dsg_run* r, dsg_run_info* out);
bool dsg_run_issue(const dsg_run* r, dsg_issue* out);
void dsg_run_free(dsg_run* r);                     /* a running player is left running */
```
`dsg_run_desc`: argv, cwd, out_dir, tmp_dir, sim, wake, ctx, now_ns.
`dsg_run_info`: state (`DSG_RUN_STARTING`, `_RUNNING`, `_STOPPING`,
`_ENDED`), elapsed, trials, the last row's response, outcome and rt, the
log's last `DSG_RUN_LOG_LINES` (200) lines, the exit status and its text,
`kill_offered`, the output folder, the log's path, the command line. The
knobs: `DSG_RUN_MAX_SECONDS` (600), `DSG_RUN_REDRAW_HZ` (2),
`DSG_RUN_KILL_AFTER_NS` (5 s), `DSG_RUN_ROLES` (16), `DSG_RUN_ARGV` (96).

**What `ui.cpp` and `main.cpp` do, frozen.**
- **Toolbar.** After Redo, in the ID scope `tool`: Build, Run, and while a
  run goes Stop, and Kill when `kill_offered`. A disabled button's tooltip
  says why. Build is disabled with no project, while a Build goes (or one
  that Run posted) and while a run goes. Run is disabled as Build is, and
  until the check of the document's current revision came back with no
  error. Then one check box per AUX view, in the ID scope `aux`
  (`//##designer/aux/Preview`, `//##designer/aux/Run`). In a browser there
  is no Build, Run or Run view (decision 16).
- **The AUX pane** shows while one AUX view is ticked; the ticked views
  stack in the views' order. `ui_aux_open()` and `ui_show_aux()` are now
  the Preview's own box. The part `preview` keeps the Preview's `open`, the
  part `run` the Run view's.
- **Build.** `ui_build()` saves when the document is dirty (decision 2),
  then posts to `dsg_build_path()` with the document's revision. The
  build thread's wake is the check thread's event. Each frame takes the
  result; one of another project (below the floor) is dropped.
- **Run.** `ui_run_start()` builds first when the document is dirty or
  `dsg_build_stale()` says so, and starts the run at that Build's result
  when it is OK. The start: the output folder (`settings.out_dir`, else
  `dsg_run_out_dir()` with the time now) must not exist; `dsg_run_args()`;
  `PROJECT/.designer` as `tmp_dir`; `dsg_run_start()`. A real run (not
  `sim`) closes the preview's session (decision 9) and keeps its settings;
  at the run's end the preview opens again and a check is posted, so a
  session follows. Each frame calls `dsg_run_tick()` and, at the end, says
  how it ended in the status.
- **The rig of the check** is `settings.rig` of the Run view; a change of
  it posts a check at the next frame.
- **The merged report.** `ui_report()`: the check's issues, then the last
  Build's, then the run's (`dsg_run_issue()`, copied). `status`, `built`,
  `have_pack` and `rig` are the check's; `n_errors` counts all.
  `ui_issue_path()`, the marks of the views and a click follow that order,
  so the tables view's `issue_sel` maps a Build issue of a table to its cell
  with no new code. The Issues panel's severity column says `build`, `run`
  or `rig` for those sources, and an issue with `rig` set shows as "rig
  profile 'NAME': message" (the check's rig; a run issue's: the run's).
- **Quit, Open or New while a run goes** asks "A run goes": Stop the run
  (the action goes on at the run's end; Kill shows in the toolbar),
  Leave it running (`dsg_run_free()`), or Cancel. Then the unsaved
  changes' question, as before.
- **The redraw rate.** `dsg_run_desc.wake` is `ui.cpp`'s: it counts the
  wake-up (an atomic) and calls `UiPlatform.wake_run`. `main.cpp` pushes
  its own event type for it, which is not input: no settle frames.
  `ui_run_wait_ms()` is -1 with no new wake-up, else the time to the last
  frame plus 1/`DSG_RUN_REDRAW_HZ`; the loop waits that long
  (`SDL_WaitEventTimeout()`) and then draws. Every frame takes the progress.
- **`main.cpp`**: `UiPlatform.player_path` is `SDL_GetBasePath()` and
  `ysp_player` (`.exe` on Windows); CMake copies `ysp_player` there at
  every build (`copy_if_different`).

**Changes from the plan's interface, and why.**
- `dsg_proc_stop()` returns `int` (part X), and `dsg_run_stop()` passes it
  on.
- `DSG_FROM_RIG`, `dsg_issue.rig` and `dsg_report.rig` are new (part P's
  finding): a refusal that the profile causes, for example `role 'resp':
  the rig binds it to family 'xid'`, names the profile in the Issues panel,
  and the profile's own load errors and notes have a source.
- `dsg_checker_set_rig()` is new: the worker passes the job's rig to the
  checker before each check, so `dsg_check_tables()` keeps its signature
  and its callers.
- `dsg_build_check_pack()` is new (part K's finding): `check.c`'s build of
  the check pack calls it in place of its own `ypt_build_text()`, so part B
  owns the check pack's cache and any skip of it in `build.c`, and changes
  no line of `check.c`. Only part G edits `check.c`.
- `dsg_rig_counts_per_unit()` is new (part P2's finding), for part G.
- `dsg_run_issue()` is new: a failed run's line as an issue, which
  `ui.cpp` puts after Build's.
- `dsg_player_version()` takes a `tmp_dir`: `dsg_proc_start()` writes
  the output to a file that must not exist. `dsg_run_start()` makes the
  version check itself (decision 5), so `ui.cpp` does not.
- `dsg_build_post()` and `dsg_run_args()` take `err`; `dsg_run_args()`
  takes the player and the output folder and fills a `dsg_run_argv` (no
  heap); `dsg_build_busy()` and `dsg_build_wait()` are new (the toolbar;
  the tests). `dsg_build_path()` is new (decision 1's name, in one place).
- `views.h` is not "unchanged": the Run view needs Build and Run from
  `ui.cpp`, which owns them (`ui_build()` and the rest above), and each
  AUX view has its own check box (`ui_aux_view_open()`).
- `ui.h` gains `UiPlatform.wake_run` and `ui_run_wait_ms()`: the plan's
  `RUN_REDRAW_HZ` needs a wake-up that is not input (a check's wake brings
  three settle frames) and a loop that waits with a timeout.
- The part `run` also keeps `participant` (for `--participant`) and
  `gains`; `seed` is a string of digits (`""`: the player draws one).
  `rig` is never a path: the player reads `--rig X` as a file when X has a
  slash or ends in `.json`.
- `dsg_session_settings.rig` `""` is the built-in simulated rig; the
  preview has no "no profile" of its own, because `pl_sim_desc.rig` NULL
  is the built-in rig.
- `ysp_player` is copied beside the designer by a target of its own
  (`ysp_player_beside_designer`), not written there, so the player's own
  folder and tests do not change.

**What each part of wave 2 does.**

Part B (`build.c`, `tests/designer/build_test.c`):
- Every call of the Build section. The thread is a `yrt_pump` as the
  check thread's; `dsg_build_post()` is newest wins; `dsg_builder_stop()`
  cancels through `ypt_options.cancel`.
- `ypt_options.cache` is `PROJECT/` `DSG_CACHE_DIR` for Build and for the
  check pack (`dsg_build_check_pack()`), and B picks `cache_max` (a knob
  in `build.c`, with its measurement).
- Part K's finding: a warm cache still costs 236 ms a check pack in the
  1080p video case. B may skip the check pack's build when no source or
  resource changed (the hook is `dsg_build_check_pack()`; the check thread
  calls it, one at a time). `check.c` rebuilds the check pack only when
  the texts or the dirty tables change, and a skip must not keep a pack
  whose glyph runs are another text's.
- `dsg_build_result.report`'s issues: places from "line N, column M" as
  `check.c`'s `place_in_message()` gives them (with the offset), a table's
  as `dsg_csv_place()` reads them.
- `build.c` defines only the Build section's functions as external
  symbols: `build_run_double.c` stands in for the whole file in
  `test_designer_ui`.
- Part X's open point: on Windows the pack tool's `run_program()` (ffmpeg)
  inherits every inheritable handle, so a Build that starts ffmpeg while
  the designer starts the player can give ffmpeg the log's handle. The fix
  is in the pack tool (a handle list), not in B.

Part R (`run.c`, `tests/designer/run_test.c`, `tests/designer/fake_player.c`):
- Every call of the Run section but `run_part.c`'s. `dsg_run_args()`
  refuses a real run when `YSP_DESIGNER_TEST` is set.
- Part P's finding: on Windows a `--sim` run cannot be stopped, only
  killed. `dsg_run_stop()` gives `dsg_proc_stop()`'s 0, and
  `kill_offered` is then true at once.
- `rig` `""` gives no `--rig`; a real run then takes `profile.json` when
  there is one (player/lower.c). Say so in the info, or V offers no "No
  profile" for a real run when `profile.json` exists (below).
- `dsg_run_issue()`: `rig` true when the run had `--rig` and the line
  names the rig.
- `run.c` defines only the Run section's functions, but
  `dsg_run_settings_default()` and the part's two (`run_part.c`), as
  external symbols.

Part G (`rig.c`, the rig's parts of `check.c` and `preview.c`,
`tests/designer/rig_test.c`):
- Every call of the rig section, and `dsg_checker_set_rig()` in `check.c`
  (load at the next check, again on a new name or `dsg_rig_changed()`;
  `pl_check_desc.rig` and `.cal`; `dsg_report.rig`).
- Part P's finding: the calibration need goes only when the profile's
  calibration passed its check and the caller gives it, so
  `dsg_rig_load()` reads both, and `dsg_rig_cal()` is NULL unless the
  check is OK.
- Part P's finding: a profile that binds a role by name refuses every
  keyboard project with that role name. Set `dsg_issue.rig` on a refusal
  that the profile causes, for example by a second `pl_check()` with no
  profile when the first refuses: the Issues panel then names the profile.
  The profile's own load error or note is `DSG_FROM_RIG` with `rig` true;
  the panel puts "rig profile 'NAME': " before the message, so the message
  does not repeat the name.
- The session (`preview.c`, decision 14): `pv_load()` reads
  `s->settings.rig` (`""`: the built-in rig), loads it with
  `dsg_rig_load()`, and gives `pl_sim_desc.rig` and `.cal`; `preview.c`'s
  `same_settings()` compares `rig`, so a change restarts.
- Part P2's finding, decided here: the pointer's conversion to counts is
  G's, in `preview.c`, not V's: `dsg_preview_pointer()` already turns the
  pane's px into role units there. The preview does not drive a relative
  role today ("relative: the preview does not capture the mouse"). If G
  drives one with a profile that has its signal, a sample is the pointer's
  motion in role units times `dsg_rig_counts_per_unit()`; never role
  units as counts.

Part V (`view_run.cpp`, the rig's banner in `view_preview.cpp`,
`tests/designer/te_run.cpp`, `test_designer_ui`'s Run part):
- The Run view as "The views" says, through `views.h`'s calls and
  `ui_run_settings()`. V keeps its own `dsg_rig` for the profile's view
  and summary, and looks at `dsg_rig_changed()` once a second while the
  view shows; on a change it calls `ui_check_again()` and
  `dsg_preview_restart()`.
- Part P's finding: on Windows a `--sim` run shows Kill at once
  (`kill_offered`), with no Stop that does nothing.
- Part P's finding: for each `PL_NEED_ROLE_TIER` with a profile chosen,
  say "the profile gives it no tier" (the need's tier is always UNKNOWN).
- "No profile (keyboard only)" for a real run when `profile.json` exists
  would still load it (no `--rig`); do not offer it there, or say so.
- The preview's combo: the profiles and "Built-in simulated rig" (`""`);
  the banner from `dsg_rig_summary()`.
- `test_designer_ui`'s Run part: idle frames 0 while a run goes and
  nothing changes; at most `DSG_RUN_REDRAW_HZ` frames a second while
  `trials.csv` grows, through `ui_run_wait_ms()` as `main.cpp` uses it.

**Tests** (Linux desktop, i5-14500, Debian 12, gcc 12.2, Release):
- `test_designer_ui` (182 checks, 68 new), part 0: with no project the
  toolbar's Build and Run are disabled (ImGui's own word: a mouse sweep
  along the toolbar reads `HoveredIdIsDisabled` for each item), there is
  no Stop, and there is a check box for each AUX view, in the views'
  order. Part 11: with a clean check Build and Run are enabled; a click on
  Run's check box shows the Run view alone, then under the Preview, and a
  second click hides it; the part `run` round-trips every setting and each
  view's `open`, and a part by hand with values out of range (a rig path,
  a negative seed, a role with a comma, `max_seconds` 0, gains 0 and
  1e9) reads as the defaults; a change of the rig posts a check. On
  `build_run_double.c`: Build saves first (the file's bytes are the
  document's) and posts the saved revision to `PROJECT/build/rdk3.ysppak`;
  the merged report is the check's issues, then Build's error, then its
  warning; `n_errors` counts all; Build's error has the path of its place,
  and a click puts the caret there; Run with a stale pack posts a Build,
  starts at its result with the player, the Build's pack and a folder
  under `data/`; while it builds and while a run goes, Build and Run are
  disabled and Stop shows, Kill does not before a Stop; a real run closes
  the preview's session and it loads none until the run ends, then it runs
  again; a simulated run leaves it alone; a wake-up just after a frame is
  due in 500 ms, and a frame takes it; the run's issue comes last; Quit
  while a run goes asks, Cancel keeps the project and the run, "Stop the
  run" stops it, Kill shows, and the quit comes at the run's end.
- `test_designer_ui_real` (93 checks) runs parts 0 to 8 and the parts of
  11 that need no double on the library's `build.c` and `run.c`.
- ctest from the Release build tree, examples and the Test Engine on,
  `DISPLAY` and `WAYLAND_DISPLAY` unset: 145 of 145; with the Test Engine
  off: 144 of 144.
- ASan with UBSan (gcc 12, Debug, built and run under `setarch -R`,
  `halt_on_error`): the 13 designer tests, no report.
- clang 24 (`-fsyntax-only`, `-Wsometimes-uninitialized`, `-Wconversion`,
  `-Wshorten-64-to-32`) on every changed C and C++ file: no new warning.
  emcc (the emsdk's, `-fsyntax-only`) on the designer's core and app
  files, `main.cpp` too: no warning.

**Hand mutants** (each reverted; each fails `test_designer_ui` at the
check named):

| Mutant | Fails |
|---|---|
| u6-01: Build never disabled | no project: Build disabled |
| u6-02: Run never disabled | no project: Run disabled |
| u6-03: one AUX check box (step 5's `break`) | a check box for each AUX view |
| u6-04: Run disabled when the check is clean | Build and Run enabled |
| u6-05: Run's check box ticks the Preview | the Run view alone |
| u6-06: every AUX view stacks, ticked or not | the Run view alone |
| u6-07: the part `run` not written | the part holds every setting |
| u6-08: the part `run` not read at an open | reopened: both views and the settings |
| u6-09: no check on a change of the rig | a change of the rig posts a check |
| u6-10: Build's issues before the check's | the check's, then Build's |
| u6-11: the paths of the check's issues only | Build's error has the path of its place |
| u6-12: Build does not save first | Build saved first |
| u6-13: `n_errors` the check's only | every error counted |
| u6-14: Run never builds first | Run posted a Build first |
| u6-15: a real run keeps the preview | a real run closed the preview's session |
| u6-16: no preview after the run | the preview's session runs again |
| u6-17: no wait between run frames | a wake-up just after a frame: due in 500 ms |
| u6-18: a frame does not take the wake-up | a frame took it |
| u6-19: the run's issue not merged | the run's issue last |
| u6-20: Quit does not ask with a run | Quit does not quit at once |
| u6-21: "Stop the run" goes on at once | the designer waits for the end |
| u6-22: no going on at the run's end | it quits at the run's end |
| u6-23: Kill before a Stop | no Kill before a Stop |
| u6-24: no Stop button | Stop there |
| u6-25: a simulated run closes the preview | a simulated run leaves the preview alone |
| u6-26: a Build that goes does not disable Build and Run | while it builds |
| r-01: a rig name ending in `.json` kept | not kept |
| r-02: `max_seconds` 0 read | out of range: the defaults |
| r-03: a role with a comma or `=` read | out of range: the defaults |
| r-04: a seed with a sign read | out of range: the defaults |
| r-05: `seed` not written | the part holds every setting |
| r-06: `gains` not written | the part holds every setting |

**Not done.**
- The window was not opened: the toolbar, the question, the wake-up's
  event and the loop's timeout were not seen on a display. `main.cpp`'s
  changes are compiled only.
- No process starts in S's tests: the double stands in for `run.c`.
  The rig's prefix in the Issues panel is drawn by Skribidi, which ImGui's
  log does not see; no test checks its text.
- MSVC, MinGW and macOS were not compiled. Locals that a call fills are
  initialized (C4701).
- No ThreadSanitizer run: S adds an atomic counter that the watch thread
  writes and the UI thread reads, and no thread.

#### Part B as built (2026-11-22)

Branch `step6-b`, from `designer-step6` (`1110636`). Part B replaces S's
stub `designer/core/build.c` and adds `tests/designer/build_test.c`
(`test_designer_build`). No interface changed. One addition outside B's
files: the test's target in `designer/CMakeLists.txt` (S's file).

**What it does.**
- **The build thread.** A `yrt_pump`, as the check thread's: SCHED_OTHER
  with explicit attributes, never elevated, blocked when idle. The job is
  a slot, not a queue: a post while one waits replaces it (newest wins); a
  post while one runs waits for it. `dsg_build_busy()` is true from the
  post to the result. `desc.no_thread` (and a build without threads) runs
  the job in `dsg_build_wait()`.
- **Cancel.** Each post gets a number; `dsg_build_cancel()` sets an atomic
  "canceled up to" to the last number and drops the waiting job (it gives
  no result). `ypt_options.cancel` reads the atomic, so a running Build
  stops before its next entry, or after an ffmpeg step that runs, with
  `DSG_BUILD_CANCELED`, no pack and no temporary file. A post after a
  cancel has a higher number and runs. `dsg_builder_stop()` cancels, then
  joins.
- **One Build.** The stamp first: the description, the definition and each
  resource's `source` and `from` (a curveset's font), each an absolute
  path with its size and modification time (size -1: missing). Then
  `ypt_build(desc_path, PROJECT/build/NAME.ysppak)` with the entry cache in
  `DSG_CACHE_DIR` and the warnings on `tmpfile()` (a file in `.designer`
  when `tmpfile()` fails). Then the pack's ID (`ypak_open()` with
  `YPAK_VERIFY_OPEN`, `ypak_id()`) and size. A source that changes during
  the build makes the result stale, never the reverse. A folder given as
  `.` gets its real name (`.../build/NAME.ysppak`, not `..ysppak`).
- **Issues** (`DSG_FROM_BUILD`): the tool's error line (severity error)
  and each `warning:` line (severity warning, without the prefix), errors
  first. The tool's `ypak: ` prefix goes. A definition's place comes from
  "line N, column M" as the check reads it, with the offset in the saved
  file; the source description's and the manifest's lines get no place. A
  table's message keeps its form (`'conditions.pstb': ysp_table: line 3,
  row 2: ...`), which `dsg_csv_place()` and the tables view read, so a
  click selects the cell with no new code. A message that starts with a
  resource's name gets its source: `(source 'conditions.csv')`.
- **Stale** (`dsg_build_stale()`): no result, one not OK, another
  revision, a result of another path than `dsg_build_path(p)`, a pack
  that is gone or has another size, or a source whose size or time is not
  the stamp's. 2 us for the video case's 4 sources (a stat each).
- `build.c` defines only the Build section's symbols. Its file stamps and
  absolute path (`stat()` and `realpath()`; `GetFileAttributesExW()` and
  `GetFullPathNameW()`) are static in it, because frozen `dsg.h` has no
  such call in `platform.c`. So `platform.c` is no longer the only file
  with OS calls.

**The entry cache's bound** (decision 17's knob). After each build (Build
or check pack, OK or not), `ypt_cache_trim()` to twice the size of the
pack at the output path, never below `DSG_BUILD_CACHE_FLOOR` (1 GiB).
- The bound must hold the pack's own records, or each build after a change
  misses: a record larger than the bound goes right after it is written
  (pack.md 19), and a miss costs the derivation again (the video case:
  3,553 ms cold against 238 ms warm, below).
- The records are about the pack's media, so twice the pack holds the new
  records beside the old ones of a changed source until the next trim; the
  floor keeps old versions (an option tried and undone) in a small
  project.
- B trims after the build, not through `ypt_options.cache_max`: the tool's
  trim runs before the new pack exists, so a bound from the last pack
  would remove the records of media added past it (a first build with
  more than 512 MB of new media would derive them all again at the next
  build).
- The trim runs after every build: whether a build wrote a record cannot
  be told cheaply (the folder's time moves in steps of 1 to 4 ms here, and
  a small project's build takes 0.15 ms). A trim costs about 0.9 us a
  record (below).

**The check pack's skip** (`dsg_build_check_pack()`; decided by
measurement: yes). The check thread builds the check pack when there is
none (the first check after an open or a forget) and when the texts or
the dirty tables change. Only the first case can repeat the last inputs.
The skip key is the SHA-256 of the description's bytes and, for each
source it names, the bytes of the designer's own copies under `.designer`
(the text under check and the unsaved tables: `check.c` writes them again
before each build, so their time says nothing) or the size and time of
the project's files. The definition's bytes are in the key, so a kept pack
never has another text's glyph runs. After a build the key and the pack's
size and time go to `check.ysppak.key`; a pack that another build replaced
or that is gone is built again. A size and a time miss a file written
again in the same clock step at the same size, so no key is kept when a
source's time is within 2 s (FAT's step, the coarsest) of the pack's, as
git treats its index; the skip then waits for a later build. Build itself
never skips (decision 3). Run's staleness keeps the plan's size-and-time
rule, with that small window open: a write by another program within one
clock step after the stamp, at the same size.

**Measured.** Linux desktop, i5-14500, Debian 12, kernel 6.11.5-liquorix,
gcc 12.2, Release (Ninja), ffmpeg 5.1.6, 2026-11-22. Four blocks, each under
the measuring lock and pinned with `taskset` to CPUs 2 and 4, `DISPLAY` and
`WAYLAND_DISPLAY` unset; each waited for a 1-minute load below 1.0: 0.65 to
0.88 before, 0.65 to 1.44 after (other workers). Files on `/tmp` (ext4,
NVMe), in the page cache. A harness (`bench_b`, not kept) calls the
designer's functions in process. Milliseconds: median, p95, max. The video
case is part K's: rdk and a 10 s 1080p clip (testsrc2, MJPEG q 3, 31.2 MB)
through ffmpeg to MPEG-1 (`-q:v 3 -g 15 -bf 0`, 2 threads).

| What | Runs | Time |
|---|---|---|
| Build, warm cache (the builder's `ms`) | 20 | 238.1, 244.7, 265.4 |
| Build, cold cache | 3 | 3,553, 3,588, 3,588 |
| Check pack, cold cache | 3 | 3,580, 3,585, 3,585 |
| Check pack, warm cache, no skip (`ypt_build_text()` alone) | 20 | 237.8, 247.4, 250.5 |
| Check pack, warm cache, a miss of the skip (key, build, key file, trim) | 20 | 235.2, 244.4, 245.0 |
| Check pack, a skip | 200 | 0.010, 0.010, 0.019 |
| The first check after an open (`dsg_check()`, a new checker), skipped | 50 | 11.7, 12.7, 13.2 |
| The same, the key file removed (a build) | 20 | 242.3, 256.4, 276.2 |

The skip elsewhere: a skip (median), a miss, and `ypt_build_text()` alone
with the same cache; the difference is the key, its file and the trim.

| Project | Skip | Miss | Build alone |
|---|---|---|---|
| oddball (2 records) | 0.013 | 0.174, 0.217, 0.310 | 0.148, 0.203, 0.251 |
| stroop | 0.017 | 6.374, 6.529, 6.793 | 6.346, 6.443, 6.578 |
| many (200 texts) | 0.184 | 34.3, 35.0, 35.4 | 34.0, 34.9, 35.4 |
| many+words | 0.190 | 145.3, 152.9, 152.9 | 146.4, 199.2, 199.2 |

The first check after an open: stroop 1.16 ms skipped, 7.80 ms built;
many+words 42.2 ms skipped, 179.3 ms built. So a miss costs 0.02 to 0.03
ms more than a build, within the spread of the larger projects, and a skip
saves the whole build: 230 ms of the video case's first check, 137 ms of
many+words'. `ypt_cache_trim()` of a folder of 10, 100 and 1,000 records
of 64 KB: 0.009, 0.082 and 0.915 ms median; of oddball's 2 records 0.003.

**ThreadSanitizer** (gcc 12, RelWithDebInfo, built and run under
`setarch -R`): `test_designer_build` passes (153 checks); part 9 runs a
Build of stroop on the build thread and its check pack on the test's
thread at once, into one cold cache, four times. TSan reports three races,
all in the layout library that each build's glyph runs create
(`ylay_create()`), none in `build.c`:
- `skb_font_collection_create()` (`skb_font_collection.c` 333, `++id` on
  a static): the race the plan named. Two collections can get one id.
- `skb_attribute_collection_create()` (`skb_attribute_collection.c` 44):
  the same `++id` pattern, not in part T's table.
- `ylay_get_versions()` (`pack/layout.c` 178): writes HarfBuzz's version
  pointer into a static at each call. Part T's table calls it benign (the
  same value each time); in C's terms it is a data race.

Each round's check pack was byte-equal to a build alone, and the Build's
pack ID was `ypak`'s. A fix (atomic ids in a Skribidi patch, the versions
filled once) is a pack/layout patch release: reported, not made.

**Tests**: `test_designer_build` (153 checks; ctest, with `ypak`'s path):
1. Each of afc2, bimanual, oddball, rdk, stroop (with the test font) and
   visuomotor: Build's pack ID equals that of the `ypak build` process of
   the same description; the size is the file's; the stamp has the
   description, then the definition, then each source, absolute; up to
   date just after. oddball: its records are in `DSG_CACHE_DIR`; an old
   1.5 GiB record (sparse; Linux) goes at the next Build and the Build's
   own stay. A folder `.` has its real name.
2. A table row with one field too many: one error from Build that
   `dsg_csv_place()` maps to `conditions.pstb`, row 1, with its source and
   without `ypak: `.
3. An unknown column in the definition: an error at its line and column,
   with the offset of that place in the saved file. A 120 s wait: a
   warning (severity warning, no `warning: `) at its place; visuomotor's
   two warnings do not fail its Build. A description that does not parse:
   no place in the definition, and a stamp of the description alone.
4. After `dsg_project_save()` of an edit, the pack's definition is the
   saved file's bytes.
5. Newest wins: with the build thread held in its wake, two posts give
   one build, the last; a cancel drops a waiting Build (no result, not
   busy); a post after a cancel builds.
6. POSIX: a stand-in `ffmpeg` (a shell script first on the `PATH`) holds
   the Build in its step; a cancel there gives `DSG_BUILD_CANCELED`, no
   error, no pack, no temporary file, and a stale result.
7. Stale: by revision; by a source's time at the same size; by its size;
   with the pack gone; by the description's time.
8. The check pack: the cache folder; skipped for the same inputs; built
   for another text, another description (the same sources), a source's
   time or bytes, a font's time (stroop), the pack gone, another pack of
   the same size, a pack of another size at the same time, and a source
   written now and again at the same size and time (the 2 s window); each
   pack equal to a fresh `ypt_build_text()` of the same inputs, stroop's
   glyph runs too. Through `dsg_check()`: after a forget, a pack and no
   error, and the pack file not built again.
9. Two builds at once (above).

Hand mutants, 28 of 28 killed (each reverted):

| Mutant | Fails |
|---|---|
| b-01: a waiting job not replaced | the newest ran |
| b-02: a cancel keeps the waiting job | the cancel dropped it |
| b-03: no `ypt_options.cancel` | canceled, no error |
| b-04: the cancel misses the newest job | canceled, no error |
| b-05: busy ignores the running job | the build is in its ffmpeg step |
| b-06: stale ignores the revision | another revision: stale |
| b-07: stale ignores a source's time | a source's time: stale |
| b-08: stale ignores the pack | the pack gone: stale |
| b-09: the stamp without the description | the stamp's sources |
| b-10: the stamp without a curveset's font | the stamp's sources (stroop) |
| b-11: a description's line as the definition's | no place in the definition |
| b-12: no offset | its offset is its line and column's |
| b-13: no warnings | a warning does not fail it |
| b-14: warnings as errors | visuomotor built |
| b-15: the `warning: ` prefix kept | the severity says it |
| b-16: the `ypak: ` prefix kept | the message names the source |
| b-17: no source for a resource's message | the message names the source |
| b-18: Build without the cache | the Build's records are in the cache |
| b-19: the check pack without the cache | the entry cache is there |
| b-20: no trim after a Build | an old record past the bound went |
| b-21: the check pack never skipped | the same inputs: skipped |
| b-22: the designer's copies keyed by time | another text: built, fresh |
| b-23: the key without the description | another description: built, fresh |
| b-24: no 2 s window | the same size at the same time: built, fresh |
| b-25: the key file without the pack's time | another pack in its place |
| b-26: the key file without the pack's size | a pack of another size at its time |
| b-27: a folder `.` named `.` | a folder '.' |
| b-28: a trim's bound below the pack | the Build's records are in the cache |

Also: `test_designer_build` 5 runs in a row, 0 failed; ctest from the
Release tree with the Test Engine and the examples on, `DISPLAY` and
`WAYLAND_DISPLAY` unset: 146 of 146 (`test_designer_ui_real` now runs on
B's `build.c`). clang 24 (`-fsyntax-only`, `-Wsometimes-uninitialized`,
`-Wconversion`, `-Wshorten-64-to-32`) on `build.c` and `build_test.c`: no
warning; emcc on `build.c`: no warning; the Windows functions of both
(file stamp, absolute path, file identity, `_utime64()`) with clang 24
`--target=x86_64-pc-windows-gnu` against hand-written stubs: no warning.
Locals that a call fills are initialized (C4701).

**For the other parts.**
- V: `dsg_build_stale()` stats each source; call it at most once a second
  (as `dsg_rig_changed()`), not every frame, for a project with many
  sources.
- The check pack's build has no cancel (`dsg_build_check_pack()` takes
  none): a Quit during a cold check pack of the video case waits up to
  3.6 s for the check thread. With the skip and a warm cache this is rare.

**Not done.**
- Windows and macOS not compiled or run; part 6 (the stand-in ffmpeg) and
  the sparse record are POSIX and Linux only.
- The three layout races above (a pack/layout patch release).
- The UI was not opened. Build's issues in the Issues panel and a click
  to a table cell were checked in the core only (`dsg_csv_place()`); part
  V's `te_run.cpp` checks the click.

#### Part G as built (2026-11-22)

Branch `step6-g`, from `designer-step6` (`1110636`). Part G fills the rig
section of `dsg.h` 0.4.0 (`designer/core/rig.c`, which replaces S's stub),
the rig's parts of `check.c` and `preview.c`, and
`tests/designer/rig_test.c`. `designer/CMakeLists.txt` (S's file) gains
only the target `test_designer_rig`. `worker.c` needed nothing: S's
worker already gives each check job's rig to `dsg_checker_set_rig()`. No
frozen interface changed.

**`rig.c`.**
- **The list.** `yrig_dir()`'s regular files `NAME.json`, NAME a profile
  name of ysp/rigfile.h (1 to 63 of A-Z a-z 0-9 _ -): `profile` first when
  `profile.json` exists, then the others in byte order. Other files (a
  save's `profile.json.tmp`, `a.b.json`, a folder `x.json`) are not
  listed. A rig folder that does not exist yet gives 0 profiles. A
  folder that `yrig_dir()` refuses gives -1 and `yrig_load()`'s own words
  ("no rig folder: the config folder is missing, or another user can write
  it"), so the list, a load and the player say one thing.
- **The load.** `yrig_load()`, so the folder rule and the loopback and
  calibration checks are the player's. The calibration file that the
  profile names is read with it: its bytes, with their size and magic.
  `dsg_rig_cal()` is not NULL only when they are there and the file's
  check is OK (part P): the SHA-256 that the profile names, so the bytes
  are the ones its tool wrote. `ycol_cal_load()`'s whole check (the CRC,
  and the derived fields against the readings) costs 23 ms ("Measured",
  below), so the load does not run it: only the checker does, once per
  load (`dsg_rig__cal_error()`); a failure is an error and takes the
  checker's `dsg_rig_cal()` away. A session's load and the Run view get
  the bytes unchecked. A file whose SHA-256 matches but that is not a
  calibration gets the check DIFFERS in the `dsg_rig`'s copy of the
  profile (ysp/rigfile.h: "the file is another kind"), so `pl_check()`
  keeps the calibration need. A `dsg_rig` is 119 KB on the heap (the
  profile 48 KB, the calibration 62.5 KB, two paths).
- `dsg_rig_load("")` (or NULL) loads `profile.json`, as `yrig_load()`
  does: the profile that a real run with no `--rig` loads. The checker's
  and the preview's `""` is no profile (the checker) or the built-in rig
  (the preview), as `dsg.h` says; to check what a real run with `""`
  will use, a caller passes `"profile"` when `dsg_rig_list()` lists it.
- **`dsg_rig_changed()`**: two `stat()` calls (the profile and its
  calibration file): size, modification time in ns (Windows: 100 ns
  ticks), and whether the file is there. The stamp is taken before the
  read, so a write between the two shows as a change.
- **`dsg_rig_summary()`**: `Rig: NAME (view 570 mm, 1920 px over 518.4
  mm)`, `Rig: NAME (no view)`; NAME is the profile's file name, as the
  combo lists it.
- **`dsg_rig_counts_per_unit()`**: `1 / (per_count x
  yrig_units_factor(signal units, role units))` for each lane, with its
  sign, as `player/v2.c` `v2_rig_signals()` builds the adapter; false
  (and 1, 1) with no signal, an adapter that is not `raw_mouse`, or units
  that do not convert.
- Two functions for `check.c` that are not in `dsg.h`:
  `dsg_rig__cal_error()` (why the named calibration file does not load)
  and `dsg_rig__file_key()` (the folder's refusal, and the profile file's
  size and time, without a parse). `dsg.h` has no way to say either, and
  part S froze it.

**The check (`check.c`).**
- **Load once per choice or change.** At each check the checker compares
  the job's rig name with the one it holds and calls `dsg_rig_changed()`
  (two stats); it loads again only on a new name or a change. A load that
  failed is tried again only when `dsg_rig__file_key()` changes, so a
  broken profile is not parsed at each keystroke. The test proves both:
  a file rewritten with other bytes but its size and time put back is not
  read again.
- `pl_check_desc.rig` is the profile; `.cal` is `dsg_rig_cal()` when it
  is given, else the stand-in (so the calibration need stays, part P).
- **The profile's own issues**, each `DSG_FROM_RIG` with `rig` true and
  no profile name in the message (the Issues panel puts "rig profile
  'NAME': " before it): a load that failed is an error, and the check runs
  with no profile; a calibration file that a real start cannot read or
  load is an error ("a real run refuses to start": `player/lower.c`
  `pl_cal_load()` reads the profile's calibration at every real or
  `--sim` start, whatever its check says, and fails when it does not
  load); each note of `yrig_load()`'s checks is a note (a loopback file
  missing or changed, a calibration changed).
- **A refusal that the profile causes.** When the check with a profile
  does not pass, a second `pl_check()` runs with no profile. Each loader
  or player error or warning of the first that the second does not give
  gets `rig` true. The profile's refusal of a role comes from the loader
  (`DSG_FROM_LOADER`, status DEFINITION, part P's case), not from the
  player, so both sources are compared. Only a failed check pays for the
  second one.
- **`dsg_report.rig` is the chosen profile's name, also when its load
  failed.** `dsg.h` says "the profile that the check ran with"; with a
  failed load that is none, and the Issues panel's prefix would then read
  "rig profile '?'". The load's error says that the check ran with no
  profile. This is a reading of the frozen comment, not a change of it;
  part V may prefer the other reading.
- A folder that others can write is refused at a load. A profile that
  loaded stays in use until its name or files change; the checker does
  not call `yrig_dir()` at each check.

**The preview (`preview.c`).**
- `same_settings()` compares `rig`, so a change restarts the session and a
  session of the old rig is stale.
- `pv_load()` loads `settings.rig` with `dsg_rig_load()` on the check
  thread at each session load (a `dsg_rig` is one thread's, and the
  session ops have no state of their own to keep one between loads), and
  gives `pl_sim_desc.rig` and `.cal`. The player copies both, so the
  `dsg_rig` is freed after the load. A profile that does not load fails
  the session's load with "the rig profile 'NAME' does not load: ..."
  (the preview's issue; `rig` is not set, because the preview's rig can
  differ from the check's). A file change takes effect at the next
  session load: part V calls `dsg_preview_restart()` on
  `dsg_rig_changed()`.
- **The pointer's scale**: from the view that the load gave the gfx (the
  profile's, for a definition in deg or cm), else the profile's view (a
  definition in px with a signal role in cm or deg), else the built-in
  rig's.
- **The relative role (part P2), decided here: driven with a profile that
  has its signal.** The sample is the pointer's motion since the last
  sample in role units times `dsg_rig_counts_per_unit()`: counts at the
  profile's scale, so the hand moves with the pointer, one role unit per
  role unit. There is no sample at the first place after a new session,
  after the pointer left the display or during a pause, so the hand does
  not jump across a gap. The preview still does not capture the mouse:
  the hand stops where the pointer leaves the pane. With no profile the
  role stays undriven ("relative: the preview does not capture the
  mouse"), as in step 5, because its samples would be role units at the
  stand-in gain 1, which no real run has. With a profile that has no
  signal for it: "relative: the rig profile has no signal for it".

**Tests** (`test_designer_rig`, 118 checks; Linux desktop, i5-14500,
Debian 12, gcc 12.2, Release). The user's config folder is the test's:
`XDG_CONFIG_HOME` (Windows: `APPDATA`; macOS: `HOME`) at `WORK/cfg`. The
build tree here is under `/tmp`, which `yrt_user_dir()` refuses (all
users can write it), so the test then makes
`~/.cache/ysp_designer_rig_test-PID` and removes it at its end, as
`tests/adapt/rigfile_test.c` does. It first checks that its rig folder is
not the user's own.
1. The list: an empty folder; `profile` first, then byte order; not
   listed: a bad name, `x.b.json`, `notes.txt`, `profile.json.tmp`, a
   folder `dir.json`; the cap; no `profile.json`.
2. A load: the name, the path in the test's folder, the profile, the
   calibration's bytes, the banner, not changed at once; the counts for
   px, cm, and deg from cm with a negative y; no signal; units that do
   not convert; a missing profile; a bad name; NULL.
3. A loopback file changed after its run: the load's note
   (`roles.resp.bounds ... UNKNOWN`), the check's `DSG_FROM_RIG` note with
   `rig` set, and the role tier need back.
4. (POSIX) The rig folder at mode 0777: the list and the load refuse it
   with the player's words; the check's error, and the check runs with no
   profile; at 0700 again, no issue.
5. The check with a profile: part P's table through the designer
   (oddball with no profile, `full` and `part`; afc2 with no profile and
   `room`; visuomotor and bimanual with `full`, bimanual with `part`);
   `""` is no profile while `profile.json` exists and would refuse every
   project; the profile's refusal (`resp` bound to `line`) has `rig` set
   and no profile name, and is gone with no profile; a definition error
   with a profile and a check pack is not marked; rdk's issues equal with
   no profile and with `room`; the calibration changed (the need stays, a
   note, `dsg_rig_cal()` NULL), missing (an error and a note), of the
   right hash but not a calibration (an error, the need stays), and of the
   right size, magic and hash with a byte changed after its CRC (the
   checker's whole check: an error, the need stays).
6. Loads once per choice or change: other bytes with the size and time
   put back are not read; a new time is; a new size is; a broken profile
   (an error, again at the next check, the name not in the message); a
   good one of the broken one's size and time is not parsed; a fix with a
   new size is; a new name is.
7. The preview: the pointer at (5, -3) deg at the profile's 83.8 px a
   degree (view 1000 mm, 400 mm over 1920 px) ends a region's state, in
   a deg definition and in a px one; a change of the rig restarts the
   session, and the built-in rig's scale does not end it; a relative role
   in deg with a signal in cm (per_count 0.01, -0.02): undriven with the
   built-in rig, driven with the profile, no motion at the first place
   (whose distance from the last session's last place would end the
   state), none across a gap out of the display (likewise), half way stays,
   (4, 2) deg ends it; a profile with no signal for it; the file changed
   and a restart takes the new view, at 36.8 px a degree; a profile that
   does not load is the preview's issue with its name.
8. The same deg session on the check thread, with checks of two profiles
   posted while it runs (for ThreadSanitizer).

ThreadSanitizer (gcc 12, RelWithDebInfo, built and run under
`setarch -R`): `test_designer_rig`, `test_designer_preview` and
`test_designer_worker`, no report. ctest from the Release build tree,
examples and the Test Engine on, `DISPLAY` and `WAYLAND_DISPLAY` unset:
146 of 146 (S's 145 and `test_designer_rig`). clang 24 `-fsyntax-only`
(`-Wsometimes-uninitialized`, `-Wconversion`, `-Wshorten-64-to-32`) on
the four changed C files: no warning.

**Hand mutants** (each reverted; each fails `test_designer_rig` at the
check named):

| Mutant | Fails |
|---|---|
| g-01: `profile` not put first | the list's order |
| g-02: any name listed | the list (a bad name) |
| g-03: a folder `x.json` listed | the list |
| g-04: the calibration's bytes not kept | the calibration's bytes; cal crc |
| g-05: `dsg_rig_cal()` whatever the check | `dsg_rig_cal()` of a changed file |
| g-06: `dsg_rig_changed()` sees no change of the profile | a changed file is loaded again |
| g-07: `dsg_rig_changed()` always true | not changed at once; no load with the files unchanged |
| g-08: the banner in cm | the banner |
| g-09: the counts without the sign | the counts; the relative hand |
| g-10: the counts without the units' factor | the counts; the relative hand |
| g-11: an unreadable calibration is no error | cal missing |
| g-12: no DIFFERS for a file that is not a calibration | cal junk; cal crc |
| g-13: a refused folder lists nothing, no error | the folder's list |
| g-14: `yrig_load_file()` (no folder rule) | a bad name; the folder |
| g-15: the list not sorted | the list's order |
| g-16: the list past the cap | cap 2 |
| g-17: no whole check of the calibration | cal crc |
| c-01: `pl_check_desc.rig` not given | part P's needs |
| c-02: no profile issues | the note; the folder; the calibration |
| c-03: the profile's issues without `rig` | the note's `rig` |
| c-04: no second check | the profile's refusal is marked |
| c-05: every error marked | a definition error is not marked |
| c-06: a load at every check | no load with the files unchanged |
| c-07: a failed load parsed at every check | a failed profile not parsed again |
| c-08: `dsg_report.rig` empty | the report's rig |
| c-09: the load's error names the profile | the name not in the message |
| c-10: no error for the calibration | cal missing; cal junk |
| c-11: no notes | the note; cal changed |
| c-12: `""` loads `profile.json` | `""` is no profile |
| p-01: `same_settings()` ignores the rig | a change of the rig restarts |
| p-02: `pl_sim_desc.rig` not given | the deg session at the profile's scale |
| p-03: a relative role never driven | driven with the profile's signal |
| p-04: role units as counts | the relative hand at (4, 2) deg |
| p-05: no reset when the pointer leaves | no motion across a gap |
| p-06: a sample at the first place | no motion at the first place |
| p-07: no profile's view for a px definition | the px session |
| p-08: the session's error without the name | the preview's issue names it |
| p-09: one reason for every undriven relative role | no signal for it |
| p-10: y's motion reversed | the relative hand |

39 of 39 killed.

**Measured** (Linux desktop, i5-14500, Debian 12, gcc 12.2, Release, one
P-core by `taskset -c 2`, under the measurement lock, load average 0.97
before and 1.06 after; a first run with the load at 1.1 to 2.1 gave the
same rig and check numbers, and its session loads drifted with the load;
median, p95 in parentheses). A bench program in the scratch folder, not
in the repository.

| What | Time |
|---|---|
| `dsg_rig_load()`: a profile with 2 roles, a loopback, a calibration, a view and 2 signals | 191 us (196) |
| `ycol_cal_load()`'s whole check of the calibration (the checker, once a load) | 23.2 ms (23.3) |
| `dsg_rig_changed()`: two stats | 0.6 us (0.6) |
| `dsg_rig_list()`: 3 profiles | 4.6 us (5.6) |
| A check, the texts unchanged: rdk, no profile / `room` | 574 / 588 us |
| oddball, no profile / `full` | 627 / 640 us |
| oddball, `wrong` (refused: the second `pl_check()`) | 755 us |
| bimanual, no profile / `full` | 5.20 / 5.19 ms |
| A session's load: oddball, no profile / `full` | 2.94 / 4.08 ms |
| bimanual, no profile / `full` | 8.27 / 9.52 ms |

- A profile adds 14 to 16 us to a check (two stats and the player's copy
  of the 48 KB profile); a refusal with a profile adds the second check,
  128 us on oddball.
- A session's load with a profile costs 1.1 to 1.2 ms more: 0.19 ms is
  `dsg_rig_load()`; the rest is the player's load with a profile and a
  calibration, not split here (with a given calibration `pl_cal_load()`
  opens the color context even for a definition that does not require
  one). Both are on the check thread.
- The plan's 176 us a load is `yrig_load()` with the calibration's
  SHA-256; `ycol_cal_load()` adds 23 ms, which is why the load reads only
  the bytes. Part V's own `dsg_rig_load()` on the UI thread costs 0.19
  ms, once per choice or change.

**Not done.**
- No window and no display: the preview ran on gfx's null backend.
- The Windows branches of `rig.c` (`FindFirstFileW()`,
  `GetFileAttributesExW()`) and macOS were not compiled; clang 24's
  `-fsyntax-only` checked the POSIX code of the changed files.
- `dsg.h`'s comment on `dsg_report.rig` (above) is read as the chosen
  profile; S or V may want it said in `dsg.h`.

#### Part R as built (2026-11-22)

Branch `step6-r`, from `designer-step6` (`1110636`). Part R replaces S's
stub `designer/core/run.c` and adds `tests/designer/run_test.c`
(`test_designer_run`) and `tests/designer/fake_player.c`. S left no CMake
lines for them; part R added them to `designer/CMakeLists.txt` (the
targets `fake_player` and `test_designer_run`, its environment
`YSP_DESIGNER_TEST=1`). No frozen interface changed. `run.c` defines only
the Run section's functions as external symbols.

**What it does.**
- **`dsg_run_args()`**: the plan's order, `player PACK --out DIR [--rig
  NAME] [--seed N] [--field NAME=VALUE]... [--allow-below-tier R,R]
  [--windowed] [--sim --max-seconds S [--sim-gain ROLE=UNITS]...
  [--participant SPEC]]`. With `sim`, no `--windowed` (the player refuses
  it with `--sim`, exit 2); without `sim`, no `--max-seconds`, gain or
  participant. `--max-seconds` is whole seconds rounded up (the player
  reads it with `atoll()`). A gain is the shortest text that reads back as
  the same double (`0.1`, not `0.10000000000000001`). Refused, with the
  argv empty: a real run while `YSP_DESIGNER_TEST` is set; a rig that the
  player would read as a path (a slash, a backslash, `.json` at the end)
  or that starts with `-`; a seed that is not 0 to 2^64 - 1 (`strtoull()`
  would give 2^64 - 1 for a larger one); a field with no name or an `=` in
  it; a role (below tier, gain) that is empty, starts with `-` or holds
  `,` or `=`; with `sim`, `max_seconds` not in (0, 172800] and a gain not
  above 0; a string with no NUL in its array; a command line over
  `DSG_RUN_ARGV` - 1 arguments or the 32 KB buffer.
- **`dsg_run_out_dir()`**: `FOLDER/data/WHO_YYYYMMDD-HHMMSS`, UTC by
  H. Hinnant's civil-date algorithm (no `gmtime()`). WHO is the field
  `participant`, its bytes other than `A-Z a-z 0-9 - _ .` (and a `.` at
  its start) made `_`, at most 64 bytes; `session` when there is none.
- **`dsg_run_start()`**: refuses an output folder that exists, then runs
  the version check (decision 5): `player --version` through
  `dsg_proc_start()`, its answer in `TMP/player_version.log` (removed
  after), a wait of at most 5 s that sleeps 0.2 ms between polls; a
  version other than `PL_VERSION_STRING`, an exit other than 0 (a player
  before 0.18.0 gives 2) or no answer refuses the run, and nothing
  starts. Then it makes the missing folders above `DIR` and starts the
  player with its output in `DIR.log` and `DIR/trials.csv` watched.
- **`dsg_run_tick()`**: reads only after the watch thread's wake-up (an
  atomic count that `run.c`'s own wake raises before it calls the UI's),
  or 1 s after the last read. Then: `dsg_proc_poll()` first, so that once
  the player has ended the reads see all it wrote; the log's and
  `trials.csv`'s new bytes, from `FILE`s held open at their ends
  (`clearerr()`, then `fread()` of 64 KB at a time); at the end the
  `dsg_proc` is freed (a short join: the watch thread ends at the exit's
  wake). The tick allocates nothing: the run's buffers are one `calloc()`
  at the start (about 350 KB).
- **`trials.csv`**: a byte-at-a-time state machine (RFC 4180 quoting,
  ysp/trials.h's), so a row in two reads needs no buffer and no byte is
  read twice. The header gives the columns `response`, `outcome` and
  `rt_s` by name; a row counts at its line break outside quotes. The last
  row's three fields are kept (128 bytes each); an empty `rt_s` is
  `last_rt_s` -1.
- **The log**: its last `DSG_RUN_LOG_LINES` (200) lines, each cut at
  1,024 bytes, as one text; the oldest lines go with one `memmove()` a
  read.
- **States**: STARTING, RUNNING when `trials.csv` opens or the log has
  `ysp_player: running;` (a real run's start line), STOPPING after a Stop
  that went out, ENDED.
- **Stop and Kill**: `dsg_run_stop()` gives `dsg_proc_stop()`'s value.
  1: STOPPING, and `kill_offered` from `DSG_RUN_KILL_AFTER_NS` after the
  first Stop. 0 while the player runs (a `--sim` run on Windows, part P):
  the state stays RUNNING and `kill_offered` is true at once.
  `dsg_run_kill()` does nothing before a Stop.
- **The exit's text** (`dsg_run_info.exit_text`): 0 `Done: N trials in
  DIR.`; 1 `Refused or failed: ` and the log's last line without
  `ysp_player: `; 3 `Stopped: N trials written; trial K has no row.` (K
  from the player's stop line) or `...; no trial was running.`; 2, 99 and
  the others as `dsg_run_exit_text()`. A signal: `Ended by signal S
  (NAME): the data set is not closed.`, and on Linux, for a run that had
  a display (`dsg_run_desc.sim` false), the `xrandr` line of "What the Run
  view shows". Windows: -9 is `Killed`, another negative status is shown
  as its NTSTATUS in hex.
- **`dsg_run_issue()`**: exit 1 only: the log's last line without
  `ysp_player: `, an error of `DSG_FROM_PLAYER_RUN`, no place. `rig` is
  true when the argv had `--rig` and the line has the word `rig` (not
  `trigger`, not `rigid`): the player's refusals that a profile causes say
  "the rig profile" or "the rig binds it".
- **`fake_player`**: takes the player's command line and reads PACK as a
  script (`header`, `row`, `rows`, `raw`, `out`, `err`, `outs`,
  `outlong`, `sleep`, `wait FILE`, `term ignore`, `term exit CODE`,
  `raise SIG`, `exit CODE`; the list is in the file). `--version` prints
  `YSP_FAKE_PLAYER_VERSION` (`YSP_FAKE_PLAYER_OLD`: the usage and exit 2,
  as a player before 0.18.0). It opens no window, so on Windows Stop
  reaches it as little as it reaches `ysp_player --sim`. Part V's
  `te_run.cpp` can start it with `UiPlatform.player_path` and the
  environment variable set to `PL_VERSION_STRING`.

**Decisions taken here.** The coordinator, or the user, can reverse each
one.
1. **An output folder that exists is refused in `dsg_run_start()`, not
   in `dsg_run_out_dir()`.** The frozen header says that
   `dsg_run_out_dir()` fails only when the name does not fit, and
   `ui.cpp` says "the path is too long" for its failure and checks the
   folder itself after. `dsg_run_start()` also refuses before the
   version check, so a refused start makes no file. Two Runs in one
   second get one name, and the second is refused ("choose another"): a
   `--sim` run of rdk takes 64 ms, so a fast second click can see it.
2. **The version check runs at every start, not once.** It costs
   0.45 ms median with `ysp_player` in the caller (below), and a player rebuilt between two runs
   is seen.
3. **The tick reads on a wake-up, not on each call.** `ui.cpp` calls
   `dsg_run_tick()` on every frame, input frames too; without a wake-up
   the tick is an atomic load (0.01 us). The 1 s read without one
   covers a lost wake-up.
4. **Files held open, not opened at each read.** No seek (so no 2 GB
   limit of `long` on Windows), and no `fopen()` per read. The files
   close at the end.
5. **`run.c` opens files by their UTF-8 path itself on Windows**
   (`MultiByteToWideChar()` and `_wfsopen(..., _SH_DENYNO)`, `_wremove()`):
   `dsg.h` has no reader of a growing file and is frozen, and `fopen()`
   takes the ANSI code page there. This is the one OS call outside
   `platform.c`. A later `dsg.h` can move it there.
6. **The `xrandr` line only for a run with a display**, and not on
   Windows or macOS, where the command does not exist.

**Found in the frozen interfaces** (none stops an item; nothing changed):
- The brief and part S ask the info to say that a real run with rig `""`
  takes `profile.json` when there is one. `dsg_run_info` has no field for
  it; the player's `session.json` records `rig_profile`. Part V must say
  it, or not offer "No profile" for a real run when `profile.json` exists.
- `dsg_proc_poll()`'s status -1 means both "not known" (a child reaped by
  someone else) and SIGHUP (-signal). The text says "its status is not
  known" for -1.
- `ui.cpp`'s message when `dsg_run_out_dir()` fails still says "or Run is
  not built" (S's stub). Harmless; S's file.

**Measured.** Linux desktop (i5-14500, Debian 12, kernel
6.11.5-liquorix, governor `performance`), gcc 12.2, Release (Ninja),
2026-11-22, from this branch. One run under the measuring lock, pinned with
`taskset -c 2,4`, `DISPLAY` and `WAYLAND_DISPLAY` unset, the files on
`/tmp` (ext4 on NVMe). Load average 0.95 before, 0.65 after.
`test_designer_run WORK FAKE PLAYER --measure`: the test appends rows of
33 columns (about 103 bytes) to a run's `trials.csv` while `fake_player`
waits, waits for the watch thread's wake-up, then times one
`dsg_run_tick()`. Median, p95 and max.

| What | Samples | Result |
|---|---|---|
| `dsg_player_version()` of `fake_player` | 20 | 250, 374, 374 us |
| `dsg_player_version()` of `ysp_player` | 20 | 453, 502, 502 us |
| `dsg_run_start()` of `fake_player`, its version check in it | 20 | 348, 543, 543 us |
| A tick with no wake-up (no read) | 2,000 | 0.01, 0.01, 0.02 us |
| A tick that reads one new row, `trials.csv` of 40 rows | 40 | 12.8, 32.1, 38.4 us |
| The same, 10,040 rows | 40 | 15.3, 31.6, 34.6 us |
| The same, 100,040 rows | 40 | 11.5, 30.5, 34.2 us |
| One tick's first read of 9,960 rows (1.0 MB) | 1 | 1.67 ms |
| One tick's first read of 89,960 rows (9.0 MB) | 1 | 15.2 ms |

A tick that reads costs the same with 40 rows as with 100,040: it reads
only the new bytes. Most of its 12 us are the poll and two `read()`
calls. The parse runs at about 600 MB/s, so a tick that read the whole
file each time would cost 15 ms at 100,000 rows, a whole 60 Hz frame of
the UI thread; the generated "many" project writes 10,000 trials. A start
with `ysp_player` was not timed as one; its version check is 0.45 ms of
it, so the UI thread pays about 0.5 ms at Run.

**Tests.** `test_designer_run WORK FAKE PLAYER` (ctest passes the build
tree's `fake_player` and `ysp_player`): 150 checks on Linux, about
3 s.
- `dsg_run_args()`: the exact argv of the defaults, of a real run with
  every setting (no `--sim` options), of a simulated run with every
  setting (no `--windowed`), `--max-seconds` 12.25 as 13 and 172800, a
  gain of 2.5e-7, 1/3 reading back as the same double, the largest seed;
  22 refusals, each with an empty argv; `YSP_DESIGNER_TEST`: a real run
  refused, a simulated one not.
- `dsg_run_out_dir()`: 2026-11-22 13:45:07, a leap day, 2000-03-01, 1970
  and the second before it, a participant with `..`, a space, `:`, `/`
  and UTF-8, none, an empty one, a folder with a trailing separator; a
  name that does not fit.
- `dsg_run_exit_text()`: 0, 1, 2, 3, 99, 7, -9, -15, -10 (no name), -1,
  the `xrandr` line, a small buffer.
- Refusals that start nothing: an output folder that exists (no log), a
  log that exists (its bytes stay), another version (`0.0.1`: no log, no
  folder), a player before `--version` (exit 2), no player, no folder for
  the answer; the answer's file is removed each time.
- `fake_player` runs: rows and the last row by name with the columns out
  of the player's order, a quoted field with a comma, quotes and a line
  break before them, names that start as `response` and `rt_s` do, a row
  with an empty response and `rt_s`, a row in two reads with a line break
  in a quoted field; standard error in the log; the end line; `Done: 4
  trials in DIR.`. No read without a wake-up (3 writes that the tick did
  not see, then saw after the wake-up, with the clock held). The log's
  last 200 lines, a 3,000-byte line cut at 1,024. Exit 1, five lines: the
  issue, its `rig` with and without `--rig`, `trigger` and `rigid`. Stop:
  1, STOPPING, exit 3, `Stopped: 2 trials written; trial 2 has no row.`.
  Kill: refused before a Stop; after a Stop that the player ignores, no
  Kill at 5 s - 1 ns, Kill at 5 s; a second Stop; -9 with no `xrandr`
  (`--sim`). SIGINT of a run with a display: `signal 2 (SIGINT)` and the
  `xrandr` line. A free leaves the player running (it writes its log
  after). The output folder's missing parents are made.
- `ysp_player --sim` of rdk (`ypt_build()` of `tests/player/rdk`, seed
  7, `--field participant=17`): `--version` gives `PL_VERSION_STRING`;
  exit 0, 40 rows, the last row's response, outcome and rt, the log's one
  line `ysp_player: 40 trials, DIR, seed 7, experiment ...`, `Done: 40
  trials in DIR.`, `session.json` there.
- ThreadSanitizer (gcc 12, Debug, built and run under `setarch -R`,
  `halt_on_error`): 3 of 3 runs, no report.
- ctest from the Release tree (examples and the Test Engine on, `DISPLAY`
  and `WAYLAND_DISPLAY` unset): 146 of 146 (145 before, and
  `test_designer_run`). clang 24 `-fsyntax-only` (`-Wconversion`,
  `-Wshorten-64-to-32`, `-Wsometimes-uninitialized`) and emcc
  `-fsyntax-only` of `run.c`: no warning.

**Hand mutants** (each reverted; each fails `test_designer_run` at the
check named):

| Mutant | Fails |
|---|---|
| r-01: no `YSP_DESIGNER_TEST` refusal | a real run refused under it |
| r-02: `--windowed` with `--sim` | a simulated run, every setting |
| r-03: `--rig` with rig `""` | the defaults |
| r-04: `--max-seconds` cut, not rounded up | 12.25 s rounds up |
| r-05: a gain at 17 digits | a simulated run, every setting (`0.1`) |
| r-06: a seed above 2^64 - 1 accepted | its refusal |
| r-07: a rig ending in `.json` accepted | its refusal |
| r-08: the participant not made a name | the folder of `../a b:c/` |
| r-09: the year of January and February | the leap day |
| r-10: a time before 1970 truncated | 1969-12-31 23:59:59 |
| r-11: no text for 99 | 99 |
| r-12: no `xrandr` line | a signal: how to put the display back |
| r-13: the `xrandr` line for `--sim` too | Kill of a simulated run |
| r-14: quotes not read | two rows |
| r-15: columns by place | the last row by name |
| r-16: a column name by its prefix | the last row by name |
| r-17: the header counted as a row | two rows |
| r-18: the log keeps its first lines | 200 lines |
| r-19: a long line not cut | cut at 1024 |
| r-20: the issue keeps `ysp_player: ` | exit 1, case 0 |
| r-21: `rig` as a part of a word | exit 1, `trigger` |
| r-22: the issue's `rig` without `--rig` | exit 1, case 2 |
| r-23: no version check | another version is refused |
| r-24: an output folder that exists not refused | its refusal |
| r-25: Kill offered at the Stop | Stop went out, no Kill |
| r-26: Kill after more than 5 s | the tick at 5 s |
| r-27: Kill without a Stop | Kill before a Stop does nothing |
| r-28: every tick reads | no wake-up, no read |
| r-29: the end before the last read | a row in two reads |
| r-30: exit 1 gives no issue | exit 1, the issue |
| r-31: no parents made | the folder that is not there |
| r-32: the log in the data set's name (`DIR-log`) | a log that exists |
| r-33: an empty `rt_s` as 0 | a row with no response |
| r-34: never RUNNING | two rows: the state |
| r-35: a free kills the player | the player went on after the free |
| r-36: an old player's exit 2 accepted | a player before `--version` |
| r-37: the answer's file left | its answer's file is gone |
| r-38: elapsed always 0 | elapsed |
| r-39: the stop line's trial not read | stopped |
| r-40: a doubled quote kept double | a row in two reads |

40 of 40 killed.

**Not done, or only by CI.**
- Windows: the `_WIN32` branches (`_wfsopen()`, the exit texts, the
  quoting of the command line, the test's Stop that gives 0 and Kill at
  once) were only syntax-checked: clang 24 `-fsyntax-only` with `_WIN32`
  defined against hand-written stubs of the Win32 declarations. CI runs
  them first. Stop's 0 and `kill_offered` at once run only there.
- macOS: not compiled.
- No real run (`--sim` only, as the rules say). A real run's
  `running;` line, Stop through SDL and the `xrandr` advice are from the
  code.

#### Part V as built (2026-11-22)

Branch `step6-v`, from `designer-step6` (`1110636`: the plan, P, K, X and
S merged). Part V is the Run view (`designer/app/view_run.cpp`), the rig's
band and combo in the preview's pane (`view_preview.cpp`), the Test Engine
tests `run/*` (`tests/designer/te_run.cpp`) and part 12 of
`test_designer_ui`. Parts B, R and G were built at the same time, so the
tests ran on test doubles (below). No frozen interface changed: `dsg.h`
0.4.0, `views.h` and `ui.h` are as part S left them.

**The Run view** (`//##designer/Run`, in `UI_SLOT_AUX`, under the Preview
when both are ticked). From the top:
- Run, Stop and Kill, and one line: the state, the elapsed time and the
  trials written. Run is disabled as `ui_can_run()` says; its tooltip
  says why (no project, a Build or a run goes, the check has errors, the
  check has not come back).
- **Rig.** The combo of the rig folder's profiles (`dsg_rig_list()`,
  at most once a second while the view draws, and at each open of the
  combo): "profile (default)" first, then each other name. "No profile
  (keyboard only)" is offered for a simulated run, and for a real run
  only while the folder has no `profile.json`. A folder that the list
  refuses shows its reason. The chosen profile's line
  (`dsg_rig_summary()`), its check notes' count, "Copy the folder's
  path", and "Profile details": the rig's name and `written`, the file,
  each role (family, key, options, `latency` and `bounds` with their
  check: OK, MISSING, CHANGED, DIFFERS), the display (onset offset, the
  photodiode latency, the calibration file and its check, and a warning
  when the check is OK and `dsg_rig_cal()` gave none), the view (distance,
  px over mm, `yrig_ppd()`), the signals, and each note of the load.
- **Build.** The pack's path (`dsg_build_path()`), "Building ..." while
  one goes, then the last result: the pack ID with "Copy ID", the size,
  the time and the hour; "Up to date" or "Stale" (`ui_build_stale()`, at
  most once a second, and at a new result or revision); a failed Build's
  error count, which the Issues panel lists; Build.
- **Run settings.** Simulated (`--sim`); with it the simulated
  participant (correct, silent, `p=` a probability), Max seconds and a
  gain for each `PL_NEED_SIM_GAIN` role; without it Window
  (development). The seed (digits, or empty). A row for each session
  field that a `PL_NEED_FIELD` names, marked "required" while empty. For
  each `PL_NEED_ROLE_TIER` role: an "Allow ... below its tier" box
  (`--allow-below-tier`) and, with a profile chosen, "The profile gives it
  no tier: it has no checked loopback for this role." (Part P: the need's
  tier is always UNKNOWN). The output folder: empty is decision 11's,
  and the view names the folder that the next run writes and says when it
  exists. The bounds are `run_part.c`'s, so a value typed here reads back
  the same; a field takes effect when the person leaves it.
- **Output.** "The preview stops while a real run goes." (decision 9);
  the last trial's outcome, response and rt; at the end the exit's text
  and what to do next ("What the Run view shows"): exit 1 points at the
  Issues panel, exit 2 shows the command line with a copy button, exit 3
  names `session.json`'s record, 99 names `panic.json`, a signal says the
  data set was not closed. The folder with "Copy path", "Open trials.csv"
  after an end with trials, the log's path, and the log's last lines (a
  list clipper over at most `DSG_RUN_LOG_LINES` lines, the newest in view
  unless the person scrolled up; the line starts in a static array, no
  allocation).

User text (profile names, roles, keys, notes, paths, the log, the last
row) goes through Skribidi and into ImGui's log, as the preview's does.

**Decisions taken here** (the plan gave 9, 12, 14 and 15 to part V).
1. **No frame of its own** (decision 9). The view has no `animating`
   hook, and its once-a-second work (the folder's list, the profile's
   stat, the pack's stat) runs only in frames that something else brings:
   input, or a run's wake-up at most `DSG_RUN_REDRAW_HZ` times a second.
   So a profile edited in a text editor shows at the next such frame, not
   by itself. `views.h` has no "draw again in 1 s" hook; a timer frame
   once a second would cost frames while the player runs.
2. **"No profile" and `profile.json`** (decision 12; part S's item). A
   real run with no `--rig` takes `profile.json` (`player/lower.c`), so
   the check with no profile would not be the run's. While the folder has
   `profile.json` and Simulated is off, the view sets the rig to
   `profile`: the check takes it (ui.cpp posts a check on the change) and
   the run gets `--rig profile`, the same file. This happens only in a
   frame that draws the Run view: see "Interface findings", 1.
3. **"Show" copies the folder's path.** The plan opens the folder with
   `SDL_OpenURL()`; the UI layer has no SDL and `UiPlatform` has no hook
   for it, so the view copies the path to the clipboard.
4. **The preview's rig** (decision 14). The preview's Settings get a
   "Rig" combo: "Built-in simulated rig" (`""`) and the folder's
   profiles; a choice goes through `dsg_preview_set()`, which restarts.
   The Run view's choice sets the preview's too ("No profile" is the
   built-in rig there, which also has the keyboard only); the preview's
   combo can then differ. A band above "Faithful in logic" names the rig:
   `dsg_rig_summary()`, "Built-in simulated rig: view 570 mm, 1920 px over
   518.4 mm; no devices, no signals.", or in red the profile's load error.
   The pane keeps its own `dsg_rig` for it; when its file changes (a stat,
   once a second) it loads it again and restarts the session. The Run
   view, on a change of its profile's file, loads it again and calls
   `ui_check_again()` and `dsg_preview_restart()`, as part S says; when
   both use one profile the session restarts twice in a frame, which costs
   nothing more than one restart.
5. **Windows and `--sim`** (Part P). Stop cannot reach a simulated run on
   Windows, so the view shows Kill and no Stop for one. Its Kill calls
   `ui_run_stop()` first, which gives 0 there, then `ui_run_kill()`: Kill
   still comes only after a Stop, as `dsg_run_kill()` asks. On POSIX, Kill
   shows when the run offers it (`kill_offered`); Stop is disabled while
   the run stops.
6. **The display after a Kill.** When a real run ends by a signal, a red
   band says how to put the display back: on Linux the command `xrandr
   --output OUT --mode 1920x1080 --rate 60 --gamma 1:1:1` with a copy
   button, and to use the output's name, mode and rate in place of OUT,
   1920x1080 and 60; on Windows and macOS, to set the resolution and rate
   again in the system's display settings or restart the computer. A
   simulated run's end has no band: it has no display.
7. **Simulated or real** is the run's command line (`--sim` in
   `dsg_run_info.command`); with no command (part S's double) the
   settings at the first frame that shows the run. A run that goes after
   an end is a new run even at the old one's address and folder (the
   allocator gives one, and the double's folder has a 1 s resolution).
8. The sections' headers are `Rig##section`, `Build##section` and so on:
   the Build header and the Build button had one ID, and a click on the
   button toggled the header.

**Interface findings** (reported, not changed):
1. `ui_run_settings()->rig` `""` while `profile.json` exists: only the Run
   view moves it to `profile`, and only when it draws. With the view
   hidden, a check runs with no profile while the toolbar's Run gives the
   player none and it takes `profile.json`. A fix is `ui.cpp`'s (at the
   open, and when the list changes) or `dsg_run_args()`'s (`--rig
   profile` for a real run when the file exists). Part S's text allowed
   "say so"; the view does both where it can.
2. `yrt_user_dir()` refuses every folder under one that all users can
   write, and `/tmp` is 1777: the real `rig.c` lists nothing for a test
   folder in a build tree under `/tmp`, as this machine's scratch trees
   are. The double does not make that check. At the integration, run
   `test_designer_te` with a work folder outside `/tmp` (CI's build tree
   is), or the rig tests check only that the view shows the refusal.
3. `dsg_run_info` has no `sim`: the view reads `--sim` from `command`.
4. `views.h` has no timed redraw and `UiPlatform` no "open a folder";
   decisions 1 and 3 above.

**The test double** (`tests/designer/run_view_double.h`, `.c`, part V's).
Part S's `build_run_double.c` gives no trials, log, exit text, table
place, `kill_offered` timing or rig, which the view's tests read. The new
double defines every external symbol of `build.c`, `run.c` and `rig.c`
and is in `test_designer_te`'s sources only: a Build's result as
scripted (pack ID, size, ms, an error with a definition's place or a
table's message, a warning); a run with no process whose progress, end
and Stop are scripted, `kill_offered` 5 s after a Stop on the UI's clock,
the plan's exit texts, and `dsg_run_args()` refusing a real run under
`YSP_DESIGNER_TEST`; a `--sim` run's Stop gives 0 on Windows; the
profiles of `$XDG_CONFIG_HOME/ysp/rig` (Windows `%APPDATA%\ysp\rig`)
through `yrig_load_file()`, with `dsg_rig_changed()` by size and time.
`test_designer_ui` keeps part S's double.

**Tests** (Linux desktop, i5-14500, Debian 12, gcc 12.2, Release, `DISPLAY`
and `WAYLAND_DISPLAY` unset):
- `test_designer_te`, `run/*`, 9 tests on the double: `build_id` (the
  view's Build, its pack ID and "Up to date", then "Stale" after an
  edit), `build_table` (a Build error that names `conditions.pstb`'s row
  2, column 2; a click on it in the Issues panel selects that cell in
  Tables), `sim_end` (the trials, the last row, the log, the end's text,
  the folder, Open trials.csv, no display band), `stop` ("Stopped"),
  `kill` (Kill at once when Stop gives 0; after a Stop that goes out,
  Stop disabled and no Kill until it is offered), `disabled` (Run
  disabled while the check has errors, enabled after Ctrl+Z), `rig_combo`
  (the list, `profile` for a real run, "No profile" only simulated, the
  choice posts a check and is the preview's, the preview's band and its
  own combo back to the built-in rig, the Run view's profile kept),
  `rig_changed` (a new view in the file shows within 90 frames and posts
  a check), `needs` (oddball: the field row writes `--field
  participant=s07` and the folder `data/s07_...`; the tier box; "The
  profile gives it no tier" only with a profile). The process's
  `XDG_CONFIG_HOME` (Windows `APPDATA`) is `WORK/run_xdg`, set before any
  frame, and `YSP_DESIGNER_TEST` is set. 47 of 47 tests in all.
- `test_designer_ui` 198 checks (16 new, part 12). The loop of `main.cpp`
  played on a virtual clock (`UiPlatform.now_ns`; a drawn frame costs one
  60 Hz refresh), with a simulated run going and the Run view shown: 0
  frames in 5 s with nothing new; 8 frames in 4 s with a wake-up every
  50 ms (at most `DSG_RUN_REDRAW_HZ` a second), and no check posted by
  them; 1 frame in the 3 s after the wake-ups stop. A killed simulated
  run's end has no display band; a real run (the double starts no
  process) says that the preview stops, and its Kill shows the xrandr
  command. `XDG_CONFIG_HOME` is `WORK/xdg`, so the view never lists the
  user's own rig folder. `test_designer_ui_real` 93 checks: part 12 needs
  the double.
- ctest from the Release build tree, examples on: 145 of 145 with the
  Test Engine on, 144 of 144 with it off.
- clang 24 (`-fsyntax-only`, `-Wsometimes-uninitialized`, `-Wconversion`,
  `-Wshorten-64-to-32`) on the changed files, `te_run.cpp` also with
  `DSG_RUN_VIEW_DOUBLE` 0: no warning in them. em++ on `view_run.cpp`,
  `view_preview.cpp` and `ui.cpp`: no warning.

**Hand mutants** (each reverted; each fails the test named):

| Mutant | Fails |
|---|---|
| v-01: the pack ID not shown | `build_id` |
| v-02: "Stale" not computed again at a new revision | `build_id` |
| v-03: the pack's path not shown | `build_id` |
| v-04: no "The last Build failed" line | `build_table` |
| v-05: no trials in the top line | `sim_end` |
| v-06: no last response | `sim_end` |
| v-07: no log | `sim_end` |
| v-08: no end text | `sim_end`, `stop`, `kill`; part 12 |
| v-09: no Open trials.csv | `sim_end` |
| v-10: the display band for a simulated run too | `kill`; part 12 |
| v-11: a run after an end at the same address not seen as new | part 12 |
| v-12: Kill not shown when offered | `kill` |
| v-13: Stop not disabled while the run stops | `kill` |
| v-14: Kill shown while the run goes | `sim_end` |
| v-15: no word on exit 3's record | `stop` |
| v-16: no "did not close its data set" | `kill`; part 12 |
| v-17: Run never disabled | `disabled` |
| v-18: no `profile` for a real run | `rig_combo` |
| v-19: "No profile" always offered | `rig_combo` |
| v-20: the Run view's choice not the preview's | `rig_combo` |
| v-21: no profile line | `rig_combo` |
| v-22: no profile details | `rig_combo` |
| v-23: a changed file not loaded again | `rig_changed` |
| v-24: no check after a changed file | `rig_changed` |
| v-25: "no tier" with no profile too | `needs` |
| v-26: "no tier" never | `needs` |
| v-27: the tier box does nothing | `needs` |
| v-28: a field's value not kept | `needs` |
| v-29: the next run's folder not named | `needs` |
| v-30: no "No run since the project opened." | `sim_end` |
| v-31: every run taken as real | `sim_end` |
| v-32: the Run view animates | part 12 |
| v-33: the Run view posts a check each frame | part 12 |
| v-34: the preview has no rig band | `rig_combo` |
| v-35: the band always says built-in | `rig_combo` |
| v-36: the preview's combo does not apply | `rig_combo` |
| u-01: `ui_run_wait_ms()` always 0 (`ui.cpp`) | part 12 and part 11 |
| u-02: a frame does not take the wake-up (`ui.cpp`) | part 12 and part 11 |

**The integration (wave 3): the exact steps.**
1. `tests/designer/te_run.cpp`: put `#define DSG_RUN_VIEW_DOUBLE 0` at
   its top, above its `#ifndef`.
2. `designer/CMakeLists.txt`, `test_designer_te`: remove
   `${_te_config}/run_view_double.h ${_te_config}/run_view_double.c` from
   its sources; add
   `target_compile_definitions(test_designer_te PRIVATE YSP_TE_FAKE_PLAYER="$<TARGET_FILE:fake_player>")`
   and `add_dependencies(test_designer_te fake_player)` (part R's
   target). Then the two double files can go.
3. Check `te_run.cpp`'s scripts against part R's `fake_player` as merged.
   They assume what R's branch had: PACK read as a script (`header`,
   `row`, `rows N MS TEXT`, `out`, `sleep`, `term exit CODE`, `term
   ignore`, `exit`), and the version from `YSP_FAKE_PLAYER_VERSION`, which
   the test sets to `PL_VERSION_STRING`. The test builds with the view's
   Build, then writes the script over the built pack and presses Run (the
   pack is no source, so the Build stays up to date).
4. Check the names that R's `trials.csv` parse reads: the scripts' header
   is `trial,response,rt_s,outcome`, and `sim_end` wants "rt 0.412 s";
   `stop` wants R's exit text for 3 to hold "Stopped".
5. `build_table` with B writes `conditions.csv` with an unclosed quote in
   its second row; B's message must name a row that `dsg_csv_place()`
   reads (the test takes the row from the message, and only the double's
   case pins row 1, column 1).
6. Run `test_designer_te` with its work folder outside `/tmp` on this
   machine (finding 2); under `/tmp` the rig tests check only the
   refusal's text. ctest's work folder is the build tree's.
7. `test_designer_ui` keeps part S's double; nothing changes there.
8. Run the whole `run/*` group and part 12 again, and the hand mutants
   v-01 to v-36 that the real parts can reach.

**Not done.**
- The window was not opened: the view, the bands and the combo were not
  seen on a display, and no person used them. No real or fake player
  started in part V's tests.
- The real `build.c`, `run.c` and `rig.c` were not linked (step 6's wave
  3). `te_run.cpp`'s real-part branch is compiled (clang 24
  `-fsyntax-only`), not run.
- The data folder's size ("Risks": 871 MB a run) is not shown: `dsg.h`
  has no call for it.
- MSVC, MinGW and macOS were not compiled; the Windows branches (Kill
  without Stop, the restore text) are only read. Locals that a call fills
  are initialized, and narrowing conversions are cast.
- No ASan or ThreadSanitizer run: part V adds no thread.

#### Parts B, G, R and V integrated (2026-11-22)

Branch `step6-int`, from `designer-step6` (`1110636`: the plan, P, K, X
and S). `step6-b` (B, `828cc0d`), `step6-g` (G, `5f829d6`), `step6-r`
(R, `19cf8cc`) and `step6-v` (V, `2a04b9d`), merged in that order with
`--no-ff`. After each merge the build and the designer's, the player's
and the pack tool's tests passed, but after V's: its double and G's
`rig.c` both define the rig section, so `test_designer_te` did not link
until the first fix below.

**Conflicts.** Each part added its subsection under "Decisions of step 6"
at the same place in this file and an entry at the top of
`docs/status.md`. One heading keeps the four subsections in merge order,
and `status.md` keeps every entry, the newest first. In
`designer/CMakeLists.txt`, B's `test_designer_build` and R's
`fake_player` and `test_designer_run` were added at the same lines: both
are kept. Git merged the code and G's CMake lines.

**Part V's integration steps** (its subsection, "The integration (wave
3)"):
1. `te_run.cpp` defines `DSG_RUN_VIEW_DOUBLE` 0; `run_view_double.c` is
   out of `test_designer_te`'s sources, which get `YSP_TE_FAKE_PLAYER`
   and a dependency on `fake_player`. The double's two files stay, for a
   run of `run/*` on it.
2. The scripts match R's `fake_player` as merged, and the header
   `trial,response,rt_s,outcome` gives R's parse its columns: "rt 0.412
   s" shows. R's exit text for 3 starts with "Stopped".
3. What the real parts needed of the test:
   - The pack is a source of `dsg_build_stale()`: a pack of another size
     is another pack. The script that the test wrote over the pack made
     the Build stale, Run built again over the script, and
     `fake_player` read a pack (exit 2). `fake_script()` pads the script
     with comment lines to the pack's size.
   - R's tick reads after a wake-up of the watch thread, which polls
     every 250 ms, and the engine's fast frames take almost no time. The
     waits are on wall time (at most 10 s), not on counts of frames.
   - `run/build_table`: B's message for a quote that does not close
     names `conditions.pstb`, row 2, which `dsg_csv_place()` reads as row
     1. But the designer cannot index such a table (`csv.c` refuses what
     ysp/table.h refuses), so the Issues panel has no cell to select. The
     test keeps that case for the place only, and the click case is a
     cell of 20 significant digits, which the designer indexes and the
     pack tool refuses: row 1, column 1, as on the double.
   - The rig folder. `yrt_user_dir()` refuses a folder under `/tmp`, and
     it ignores a relative `XDG_CONFIG_HOME` and takes `HOME/.config`,
     the user's own: the first run here, with a relative work folder,
     listed `~/.config/ysp/rig`. The test now takes the work folder's
     absolute path; when `yrig_dir()` refuses it, the test makes
     `HOME/.cache/ysp_designer_te-PID` and `te_run_cleanup()` removes it
     after the tests. It fails when its rig folder is the user's. On
     macOS it sets `HOME`, as `test_designer_rig` does.
4. Part V's hand mutants that the real parts reach, run again on B, R
   and G (33; v-11, v-32, v-33, u-01 and u-02 are part 12 of
   `test_designer_ui`, on part S's double, unchanged): 31 killed. v-16
   ("did not close its data set" not shown) and v-31 (every run taken as
   real) were killed only on the double's branch. The real branch now
   checks, after the Kill of a simulated run, "Ended by signal 9", "did
   not close its data set" and no display band, and in `run/sim_end`
   that the command has `--sim` and the view says "(simulated)". Both
   are killed now, and so is v-10.

**Faults found and fixed.** Each fix has a test and hand mutants that
fail it (each reverted).

| Found by | Fault | Fix | Test; mutants |
|---|---|---|---|
| V and R | With no profile chosen (rig `""`), a real run starts the player with no `--rig`, which then loads `profile.json`, but the check ran with no profile. The Run view moved the setting to `profile` only in a frame that drew it. | `ui.cpp` resolves one name for the check and Run (`run_rig_name()`): `""` is `profile` for a real run while `dsg_rig_list()` lists it, and stays `""` for `--sim`. The check posts that name, a change of it posts a check, and Run passes `--rig` with it. The folder is listed at the open, at each Run and at most once a second otherwise. | `test_designer_ui` part 13 (9 checks): the check and a real run take `profile`; a simulated run and its check none; `profile.json` gone, none again. i2-01 to i2-04 |
| R | R's folder name has whole seconds: two Runs in one second got one name, and the second was refused. | `dsg_run_out_dir()` keeps its frozen signature. `ui.cpp` takes its name, or the name with `-2` to `-99` after it, the first whose folder and `DIR.log` are both free. The Run view names the same folder through the new `views.h` call `ui_run_next_out_dir()`. A folder that the person typed is still refused when it exists. | `test_designer_ui` part 14: with the name and the `-2`'s log taken, the next folder and the Run's `--out` end in `-3`; with every name up to `-99` taken, Run is refused and says so. i3-01, i3-02, i3-06 |
| R (S's text) | The status when `dsg_run_out_dir()` failed said "or Run is not built". | "the output folder's path is too long", or "NAME and the names -2 to -99 after it exist". | part 14; i3-03 |
| This integration | A project opened by a relative path (`ysp_designer myproject`) gave the player a relative pack and output folder, with the project's folder as its working folder: the paths did not hold there (exit 2). | The player starts in the project's folder only when its path is absolute; otherwise in the designer's own. | part 14 (`build_run_double.c` records `desc.cwd`); `run/*` with a relative work folder passes. i3-04, i3-05 |
| B | ThreadSanitizer: three races in the layout library when a Build and a check pack run at once. | pack/layout 0.3.5, Skribidi patch 0010 ([layout.md](layout.md#patch-0010-collection-ids-on-two-threads)). | `test_layout` part 10; ThreadSanitizer on `test_layout` and `test_designer_build` |

The preview has no "same as Run" rig choice: it keeps its own (`""` is
its built-in simulated rig), and the Run view copies its choice to it
when the person chooses.

**pack/layout 0.3.5** (the layout and the API do not change). Patch 0010
makes the id counters of `skb_font_collection_create()` and
`skb_attribute_collection_create()` atomic; `ylay_get_versions()` gives a
constant struct (`HB_VERSION_STRING`); on Windows the performance
counter's frequency is not kept in a static. `test_layout` part 10: four
threads make 2,000 collections of each kind at once; without the patch
387 to 1,092 got another's id in each of three runs, with it none. With
the old `ylay_get_versions()` ThreadSanitizer reports it in `test_layout`
and in `test_designer_build`; with the two Skribidi files reverted, it
reports both counters in `test_designer_build`. The stamp lists ten
patch hashes; ysp-vis 0.20.0.7 (pytest on Linux, its layout built with
`tools/vendor_vis.py --layout gcc`: 82 passed, 14 skipped), MEX 1.6.5
(version strings; `gcc -fsyntax-only` only).
`tools/vendor_layout.py`: `ok: 463 files, 7735220 bytes`.

**Tests** (Linux desktop, i5-14500, Debian 12, gcc 12.2):
- ctest from the Release build tree, examples on, `DISPLAY` and
  `WAYLAND_DISPLAY` unset: 148 of 148 with the Test Engine, 147 of
  147 without. `test_designer_te` 47 of 47 (`run/*` 9 of 9 on B, R and G),
  `test_designer_ui` 229 checks (part 13 and part 14 new),
  `test_designer_ui_real` 93, `test_designer_build` 153,
  `test_designer_run` 150, `test_designer_rig` 118, `test_layout` 151.
- ASan with UBSan (Debug, built and run under `setarch -R`,
  `halt_on_error`): the 16 designer tests (`test_designer_te` with leak
  checks off), `test_player_sim`, `test_player_check`, `test_pack_tool`
  and `test_layout`: no report.
- ThreadSanitizer (the same, `race:swrast_dri.so`): the same 20 tests,
  no report; then `test_designer_build` and `test_layout` three more
  times each, and `test_designer_te`, `test_designer_run`,
  `test_designer_build`, `test_designer_ui` and `test_layout` three more
  times each: no report.
- `-Wconversion -Wsign-conversion` (gcc) on the changed designer files:
  no warning. clang 24 from the emsdk (`-fsyntax-only`,
  `-Wsometimes-uninitialized`, `-Wconversion`, `-Wshorten-64-to-32`) on
  every C and C++ file changed since `1110636`, `run_view_double.c` too:
  no new warning (one in `layout_test.c` line 1011 is older). emcc
  `-fsyntax-only` on the changed designer core and app files and
  `pack/layout.c` and `pack/colr.c`, and emcc builds of the two patched
  Skribidi files (with and without `-pthread`): no warning.
- Mutation lists, `--check`: `player_eval`, `player_host`,
  `player_lower`, `player_main`, `player_run`, `player_sim`, `player_v2`,
  `gfx`, `pack` and `table`: 0 anchors not matching. The layout library
  has no list.

**Only CI can check.** MSVC (C4701, C4244), MinGW and macOS builds of the
merged code; patch 0010's `_InterlockedIncrement()` branch and the
Windows `QueryPerformanceFrequency()` change; the `_WIN32` branches of
`build.c`, `run.c` and `rig.c` and Windows' `--sim` Kill with no Stop;
`te_run.cpp`'s config folder on Windows (`APPDATA`) and macOS (`HOME`);
`test_designer_te` on the three systems (CI's build trees are not under
`/tmp`, so the test uses the work folder there); the wheels and the MEX
builds.

**Open.**
- Part M, with the user on the display (below).
- B: the check pack's build has no cancel; a Quit during a cold check
  pack of the video case waits up to 3.6 s.
- X and B: on Windows the pack tool's `run_program()` (ffmpeg) inherits
  every inheritable handle (a fix in the pack tool).
- R: `dsg_proc_poll()`'s -1 is both "not known" and SIGHUP.
- V: no "draw again in 1 s" hook in `views.h` and no "open a folder" in
  `UiPlatform`; the data folder's size is not shown.
- G: `dsg.h`'s comment on `dsg_report.rig` (the chosen profile, also
  when its load failed) is G's reading, not said in `dsg.h`.
- Decision 14 makes the built-in rig the preview's default "when no
  profile exists". The preview's `""` is the built-in rig whether or not
  `profile.json` exists; only the Run view's choice moves it.

**Part M** (the measurement with the user on the display; nothing of it
ran here: no real player, no window). It should measure and see:
- The player's dropped frames (`frames.csv`) for a whole stroop session
  at 60 Hz on this machine's display: the designer idle; the Run view
  shown with `trials.csv` growing (redraws at `DSG_RUN_REDRAW_HZ`, 2 a
  second); and a designer that redraws every frame (the preview of a
  second project, since a real run closes the preview's session). If
  2 Hz costs frames, the knob is 0 while the player has the foreground.
- Stop on the real player: SIGTERM through SDL 3.4.18's
  `SDL_EVENT_QUIT`, exit 3, `session.json`'s `"aborted"` with reason
  `quit`, and the Run view's "Stopped" text; Kill after 5 s and the
  `xrandr` line, and whether the mode and ramp need it.
- The player's foreground and its first frames beside the designer
  (`posix_spawn()` with SCHED_OTHER; the player elevates its own frame
  thread).
- `--rig profile` on a real run with no profile chosen (`session.json`'s
  `rig_profile`), and a refusal of a profile with a role that the check
  marks (`rig profile 'NAME': ...`).
- The first use of the window: the toolbar's Build, Run, Stop and Kill,
  the Run view, the rig combo and the preview's band, the "A run goes"
  question, and the loop's wait with `ui_run_wait_ms()`.

## First version

In:
- A project: a folder with the definition, its tables (CSV) and the pack
  source description. The designer writes canonical bytes (D2) and keeps
  its view state (node positions, open panels) in the project, never in
  the definition (D13).
- Editing of everything in a format 2 definition
  ([experiment_format.md](experiment_format.md) part 3): the tree and its
  nodes, templates, states and exits drawn as a chart, controls,
  responses with signal choices, variables, data columns and roles.
  Panels come from the headers' parameter tables (G6). Each field shows
  its unit; durations are seconds.
- Tables: a grid editor with shaped text. Only visible rows are laid
  out; a layout costs at most about 10 us a cell (probe part 2, section 4).
- Checks on each change, with the player's loader: the list in
  [experiment_format.md](experiment_format.md) 2.12. Each refusal links
  to its field.
- Preview: a template's states on the player's simulated display, in a
  panel. Mouse input in the panel drives the signal roles. The preview is
  faithful in logic and not in timing, and the panel says so.
- Build: the pack tool, linked in. Its messages show with the entry
  selected.
- Run, native only: the designer starts the command-line player with the
  built pack, a rig profile and an output folder, and shows its output.

Out of the first version, in rough order:
- Run over the runner protocol, hot reload and the operator console
  (rig_spec 6).
- Import as normalization, with its choices exposed (rig_spec 7). The
  first version builds with the pack tool's defaults.
- Scripts (Luau), the eject path and build jobs.
- Format version 3's `signals` graph (experiment_format 3.8).
- Accessibility.

## Architecture

```
ysp_designer (C++, Dear ImGui, SDL3, GL ES 3.0)
    +-- player core (C library: load, check, lower, simulated run)
    +-- ysp_pack_tool_code, ysp_layout (Skribidi, HarfBuzz)
    +-- ysp_player_impl (the header implementations, once)
    +-- ysp/gfx.h on ImGui's GL context (preview)
    |
    +-- native: starts ysp_player for a real run
    +-- web: same source with emscripten; no Run
```

- **The player's loader is the only validator.** The designer links the
  player's load, check and lower code as a library and does not
  reimplement any rule. Two validators would drift apart.
- **Preview runs the player's simulated session** (`--sim`) in-process.
  It draws through `ysp/gfx.h` into a texture that an ImGui panel shows.
- **Redraw on input only.** The loop waits on SDL events. It also draws
  while ImGui's input queue holds events, because ImGui applies one key or
  button change a frame (probe part 1, section 2). Animation (a running
  preview) draws every frame.
- **Code.** `designer/core/` (C11, `dsg.h`, `doc.h`) holds every part
  with no window: the project, the document and its undo stack, the
  check, the places of issues and the redraw rule. `designer/app/`
  (C++20) holds the UI: `ui.cpp` (the window and the text view, no SDL or
  GL), one file per other view (`views.h`), `skf.cpp` (the Skribidi edit
  field), `skt.cpp` (Skribidi text in ImGui) and `main.cpp` (SDL3, GL ES
  3.0, the loop). Only
  `designer/core/platform.c` calls the OS (files and folders); its POSIX
  branch also serves emscripten.
- **Web build.** A hidden-textarea glue gives text input, IME
  composition and the system clipboard, which SDL's web backend does not
  (probe part 2, section 1). A dropped file arrives through SDL; a save is
  a download. The project lives in the browser's storage.

## Order of work

1. Split the player into a library and `main.c`, with no change in
   behavior: the player's tests and mutation lists pass unchanged.
   - Done before this plan: `player/CMakeLists.txt` builds the library
     `ysp_player_lib`, and `main.c` only reads the arguments.
   - Added 2026-11-18: `pl_check()`, the entry point the designer calls
     on each change ([player.md](player.md), "Reference: pl_check()"). It
     takes the definition's bytes and a pack, in memory or through a
     range reader, and runs the player's load with no side effects
     outside memory. It gives the loader's diagnostics with lines and
     columns, the player's first refusal with the node, stimulus,
     template or method that it names, and what a real run needs of the
     rig and the session. `pl_load()` and `pl_check()` run one load
     function. The command line's behavior and output bytes did not
     change, apart from the version (player 0.14.0).
2. Fix the text stack's faults from probe part 2: the two Skribidi
   editing faults with Devanagari, and a font list per script that has
   digits and punctuation.
   - Done 2026-11-18: Skribidi patches 0005 and 0006 (pack/layout 0.3.1;
     [layout.md](layout.md), "Patches 0005 and 0006: editing
     Devanagari"). `test_layout_editor` checks both without a window.
   - The font list: patch 0003 gives a Hebrew run its digits from the
     Latin font on the editor's path too. U+2192 was a box because none
     of the probe's four Noto fonts has it; the designer's list needs a
     symbols font (Noto Sans Symbols) after the script fonts.
   - 2026-11-18: patch 0007 (pack/layout 0.3.2) fixes U+202E before
     Devanagari.
   - 2026-11-19: patch 0008 (pack/layout 0.3.3) fixes the three faults of
     Skribidi's editor that a text field reached (an arrow key in a field
     that never had text read NULL, Ctrl+Right read past the text, Enter
     and text that ends in LF gave `memcpy()` NULL) and four more of the
     same kinds (the line and column index, the first IME composition and
     the selection bounds of a field with no text; a double click at
     offset 0); [layout.md](layout.md), "Patch 0008: the editor at its
     text's edges". A UBSan run of the designer no longer needs to accept
     `skb_text.c` line 628. Open: a field that never had text has a caret
     of height 0 and shows no IME composition, so `skf_create()` sets its
     text to "" ([layout.md](layout.md), Known issues).
   - 2026-11-19: patch 0009 (pack/layout 0.3.4) fixes End in the
     definition editor: it was right on the first line and put the caret
     after the LF on each later line (seen by hand in the designer), so a
     letter typed there went to the next line. Shift+End, Command+Right,
     triple clicks and drags by lines had the same cause;
     [layout.md](layout.md), "Patch 0009: End on each line but the
     first". Open: on a soft-wrapped line, Home after End goes to the
     next line's start ([layout.md](layout.md), Known issues).
3. A skeleton: the window, open and save of a project, the loader's
   checks with refusals linked to fields, and redraw on input only.
   - Done 2026-11-18 (branch `designer-skeleton`), with the decisions in
     "Decisions of step 3". `test_designer_core` (82 checks) and
     `test_designer_ui` (27 checks, ImGui with no window) run in ctest; 11
     hand-made mutants of the core and the UI each fail one of them. Both
     tests pass under ASan and UBSan (gcc 12, `setarch -R`), with one
     UBSan report in Skribidi suppressed (below, "Open questions").
   - Not done: the window was built and never opened, because the session
     had no free display; no person has used it. The web build, the IME on the
     multi-line editor, and Windows and macOS builds are not tried.
   - Added 2026-11-19: the CI job `designer` builds the three tests on
     Linux, Windows (MSVC) and macOS and runs them ("How to build and run
     the skeleton"). It has not run yet.
   - Added 2026-11-19: `test_designer_te`, three UI tests on the Dear ImGui
     Test Engine, optional and off by default ("UI tests with the Test
     Engine", below).
4. The state chart, the tables and the panels from parameter tables.
   - Planned 2026-11-19 (branch `designer-step4`): "Plan of step 4",
     below. The document that every view edits, the view registry and the
     view file's parts are built; the three parts are stubs.
   - Partly done 2026-11-19 (branch `designer-step4`): parts A, B and C
     are built and merged ("Decisions of step 4"). The items left are in
     "The integration", "Not done".
5. The preview.
   - Planned 2026-11-20 (branch `designer-step5-plan`): "Plan of step 5",
     below, with its decisions accepted the same day. Part T (the check
     and the session's load on a thread of their own) comes first.
   - Partly done 2026-11-20 (branch `step5-int`): wave 1, parts P3
     (ysp/gfx.h 0.23.0), P4 (the player's stepped session, 0.17.0) and T
     (the check thread), built and merged ("Wave 1 integrated"). The
     preview itself (parts S, A, B, C, D) is not built.
6. Build and Run.
   - Planned 2026-11-21 (branch `designer-step6-plan`): "Plan of step 6",
     below. Wave 1 is parts P (the player 0.18.0), K (the pack tool
     0.12.0) and X (the process). The user accepted decisions 1 to 17 on
     2026-11-22.
   - Partly done 2026-11-22: wave 1 and part S (the frozen interfaces)
     merged into `designer-step6`; wave 2, parts B (Build), G (rig
     profiles), R (Run) and V (the Run view), built and integrated on
     `step6-int`, with pack/layout 0.3.5 ("Parts B, G, R and V
     integrated"). Left: part M, the player's frames beside the designer
     and the first use of the window, with the user on the display.
7. The web build, with the text glue.

## Plan of step 4

Status: planned 2026-11-19 on branch `designer-step4`, and built the same
day: parts A (the chart), B (the tables) and C (the panels) are merged,
with the shared fixes that they asked for ("The integration"). This
section gives the model that all three parts edit, the shared
interfaces, the files that each part owns, the tests, the order of
merges and the risks.

### The model: the bytes are the truth

**Decision.** The designer keeps the project's files as buffers of bytes
in one document (`designer/core/doc.h`). Buffer 0 is the definition; the
other buffers are the CSV tables. The bytes are the source of truth.
Every change from every view is a splice of a buffer's bytes, and one
undo stack holds all the splices.

- The parsed definition (a `ysp/json.h` tree) is a cache of buffer 0. The
  document parses it again when the bytes change. A table's grid (the
  byte range of each cell, `dsg_csv`) is a cache of its buffer too.
- The text view gives the document the editor's text after each change
  (`dsg_doc_replace()` with `DSG_EDIT_MERGE`). The document keeps only the
  bytes that differ, and typing at one place is one undo step.
- A structured edit (the chart, a panel) works on a copy of the tree in a
  transaction (`dsg_doc_begin()`, `dsg_def_*()`, `dsg_doc_commit()`). The
  commit writes the tree in the canonical layout (`YJS_WRITE_PRETTY`, one
  LF after the value) and splices the difference into buffer 0. So one
  chart or panel action is one undo step, and the text view shows it at
  once, with the caret at the change.
- When the text does not parse, the text view is the only editor. The
  structured edits refuse with the parse error's line and column. The
  other views show the last tree that parsed, read-only, and say so
  (`dsg_doc_shown()` gives `stale`). No typed text is lost.
- The commit writes keys at their defaults; Save drops them
  (`yexp_migrated()`, format D2), as in step 3. A panel shows a key that
  is not in the text as its default, and "Reset" removes the key.
- The canonical layout of a structured edit is `yexp_canonical()`'s:
  `test_designer_doc` checks that a no-op edit of the bimanual
  definition's canonical bytes changes no byte and makes no step.

**Evidence.** `bench_designer_doc` (tests/designer/doc_bench.c) times the
real code. Linux desktop, Intel Core i5-14500, gcc 12.2, Release, pinned
to CPU 2 (a P-core), 200 edits each, two runs; load average 0.18 to 0.36
during the runs (`uptime`). Microseconds, median and p99 of run 1; run 2
was within 2 % at the median.

| Definition | Bytes | yjs parse | One structured edit | Parse again | One typed character | yexp load and canonical |
|---|---|---|---|---|---|---|
| kitchen_sink (the largest in the repository) | 5,933 | 10.1, 18.8 | 31.7, 72.2 | 11.7, 12.8 | 2.3, 2.5 | 151.7, 269.4 |
| bimanual | 5,670 | 9.4, 12.1 | 29.2, 41.1 | 11.3, 11.9 | 2.3, 3.7 | 181.6, 260.7 |
| bimanual, its templates copied to 100 KB | 101,130 | 118.4, 125.5 | 317.3, 353.8 | 121.5, 136.3 | 27.1, 31.9 | 2,136.8, 2,461.9 |
| bimanual, its templates copied to 1 MB | 1,001,866 | 1,392.4, 1,422.2 | 3,699.3, 3,815.0 | 1,193.5, 1,474.7 | 273.3, 337.3 | 11,470.6, 15,646.4 |

| Table (8 columns) | Bytes | Index the whole table | One cell edit (splice, index again) |
|---|---|---|---|
| 10,000 rows (the probe's table) | 449,114 | 355.4, 584.0 | 354.6, 357.3 |
| 32,767 rows (ysp/table.h's limit) | 1,547,115 | 1,207.3, 1,240.1 | 1,207.7, 1,212.1 |

- A structured edit of the largest definition in the repository costs
  about 32 us, and the parse for the views 12 us. The check that follows
  every change costs 0.2 to 7 ms (player.md, Measured), and its loader
  part alone 150 to 180 us. So the write of the whole text is not the
  cost of an edit; the check is.
- At 1 MB (170 times the largest example), an edit and its parse take
  about 5 ms, inside a 16 ms frame; the loader alone takes 11.5 ms. If
  definitions grow that large, the check is the first thing to move off
  the frame, not the model.
- A cell edit with a full index costs 0.35 ms at 10,000 rows and 1.2 ms at
  the 32,767-row limit. The index is 8 bytes a cell (2.1 MB at the limit).
  An incremental index (shift the offsets after the cell) is an option
  for part B if a measurement asks for it.

**Rejected.**
- A parsed model as the truth, with the text made from it. A text that
  does not parse has no model, so either the typed text is lost or the
  text view needs a second store and a second undo stack. Step 3 decided
  that Save keeps a text that does not parse ("Decisions of step 3").
- `yexp_def` (the loader's checked form) as the model. It exists only
  when the loader accepts the definition. A chart edit must work on a
  definition with errors: an exit to a state that is not there yet is
  normal while a person builds a chart. `yexp_canonical()` also drops a
  key at its default, so a panel that sets a value equal to its default
  would see it go at once.
- Splices of single values in the user's layout, to keep a text as the
  person typed it. `ysp/json.h` gives a value's start and not its end, and
  an insert must find its place in sorted keys and indent. The gain is
  only for text that is not canonical, and Save makes it canonical.

### The shared interfaces

The workers use these and do not change them. A part that needs a change
stops and reports it; the change goes into this branch first.

`designer/core/doc.h`, the document (C, `doc.c`):
- Buffers: `dsg_doc_reset()`, `dsg_doc_add_buffer()` (a table),
  `dsg_doc_bytes()`, `dsg_doc_rev()` (a view's cache key),
  `dsg_doc_revision()` (any change: the check runs), `dsg_doc_dirty()`,
  `dsg_doc_set_saved()`, `dsg_doc_mark_saved()`.
- Changes: `dsg_doc_splice()`, `dsg_doc_replace()`; transactions
  `dsg_doc_begin()`, `dsg_doc_commit()`, `dsg_doc_cancel()` (nested;
  the outermost makes one step with its label).
- Undo: `dsg_doc_undo()`, `dsg_doc_redo()`, their labels, and
  `dsg_doc_last_change()` (where the text view puts the caret). The stack
  keeps at most 4,096 steps or 64 MiB of old and new bytes.
- Selection, shared by every view: `dsg_sel` (a buffer, a JSON Pointer
  into the definition, or a table's row and column), `dsg_doc_select()`,
  `dsg_doc_selection()`, `dsg_doc_selection_rev()`. A step keeps the
  selection from before and after it; undo and redo put it back.
- Renames: `dsg_doc_listen()` and `dsg_doc_note_rename()`. View state that
  a name keys (a state's position, a column's width) follows a rename,
  its undo and its redo through the listener. The scope is the JSON
  Pointer of the set (`/trials/reach/states`, `/stimuli`), or
  `#NAME/columns` for the table in buffer NAME.
- The tree: `dsg_doc_root()`, `dsg_doc_parse_error()`, `dsg_doc_shown()`.
  Views call one of them once a frame and keep no pointer into the tree
  across frames.
- Structured edits: `dsg_def_get()`, `dsg_def_set()` (JSON text),
  `dsg_def_set_string()`, `_int()`, `_bool()`, `_seconds_ns()` (a
  duration as the exact decimal of its nanoseconds, D2 and D4),
  `_number_text()` (a number as typed), `dsg_def_remove()`,
  `dsg_def_insert()`, `dsg_def_move()`, `dsg_def_rename()`.
- Paths are JSON Pointers (RFC 6901): `dsg_path_key()`,
  `dsg_path_index()`, `dsg_path_get()`, `dsg_path_split()`,
  `dsg_path_within()`, `dsg_path_at()` (a byte offset to the value there)
  and `dsg_path_offset()` (a value to its offset).

`designer/core/dsg.h`, the view file's parts: `dsg_view` gains
`main_view`, `selection` and up to 8 parts. A part is a top-level key of
`.designer/view.json` whose value is an object, kept as compact JSON
(`dsg_view_part_get()`, `dsg_view_part_set()`). The view that owns a key
reads it in its `opened` hook and writes it in `save_view`. The file
keeps a part that no view reads. The format stays version 1: every key
is optional, and a reader skips a key it does not know.

| Key | Owner | Value |
|---|---|---|
| `view` | ui.cpp | the main view shown: `"text"`, `"chart"`, `"tables"` |
| `selection` | ui.cpp | the selection's JSON Pointer in the definition |
| `chart` | part A | `{"template": NAME, "templates": {NAME: {"pan": [x, y], "zoom": z, "states": {STATE: [x, y]}}}}`; `end` is the end node (a state cannot have that name, format 3.9) |
| `tables` | part B | `{"open": SOURCE, "tables": {SOURCE: {"widths": {COLUMN: px}, "scroll": [row, column]}}}`, SOURCE the CSV's path in the project |
| `panels` | part C | `{"collapsed": [SECTION, ...]}` |

`designer/app/views.h`, the view registry (C++): a view is one `UiView`:
an id (its part key), a label (the switch's label and its child window's
name, so a test finds it as `//##designer/LABEL`), a slot (the main area
or the side column) and hooks, each optional: `init`, `shutdown`,
`opened`, `closed`, `save_view`, `draw`, `claims`, `reveal`,
`animating`, and `issue_sel` (added at the integration: the cell that a
table's issue names). `views.cpp` lists the three step 4 views already, so
no part edits it. A view may call `ui_doc()`, `ui_project()`,
`ui_report()`, `ui_issue_path()`, `ui_reveal()`, `ui_set_status()` and
`ui_text_size()`, and from `ui.h` `ui_show_view()`, `ui_editor()` and
`ui_jump_to_issue()` (added at the integration: the panels use them).

`designer/app/ui.cpp` (shared, built): the toolbar with Undo and Redo,
the switch (Text, Chart, Tables), the main view's child window, the side
column (the panels), the issues, open, save and the view file.
- Ctrl+Z, Ctrl+Shift+Z and Ctrl+Y go to the document's stack, unless
  ImGui's path field or a Skribidi field that keeps its own undo has the
  keyboard. The definition editor gives its undo to the document
  (`skf_set_own_undo()`).
- A change of any buffer posts a check job (through step 4 the check ran
  in that frame; since step 5 it runs on the check thread, "Part T as
  built").
- After each check, `ui_issue_path(i)` gives the JSON Pointer of the value
  at issue i's place (`dsg_path_at()`). Tier 2 and 3 issues have places
  from `locate.c`, so they get a path too.
- A click on an issue selects its path and calls `ui_reveal()`: the main
  view that claims the selection most shows (the text claims 1 for any
  value in the definition; the chart claims 2 for a state or an exit;
  the tables claim 2 for a table cell); the side views get `reveal` too.
  The caret goes to the issue's place, as in step 3.
- The caret in the text view selects the value it is in, so the panels
  follow the text.
- Save writes the definition as in step 3 (its canonical form is an undo
  step of its own) and each dirty table's bytes as they are.

`designer/app/skf.h`: `skf_move_caret()` (no focus change),
`skf_set_own_undo()`, `skf_owns_undo()`, `skf_focus_field()`.

**Parameter-table metadata at run time.** Part C reads two sources in
the program, with no generated table:
- The format's own table, `yexp_defaults(2)` (ysp/experiment.h): each key
  of each section, its type, its default, its enum values and whether it
  is required. This is the list of keys a panel shows.
- The headers' parameter tables (G6): the unit, the range and the text of
  a key that a header's desc also has. `schema.c` joins stimulus keys
  with `yrdk_params()` and `ygfx_params()` by name, as the player's param
  host does (player/host.c). Part C adds the others it needs:
  `ysig_params()` for controls, `yrsp_params()` for responses,
  `yst_params()`, `yqst_params()` and `yaep_params()` for methods.
- A key that ends in `_s` is a duration in seconds (D4). A panel shows it
  in seconds and writes it with `dsg_def_set_seconds_ns()` from the
  decimal text that the person typed (`yjs_parse_fixed()`), so no binary
  fraction comes between.
- The section and the value type of a format row are public in
  ysp/experiment.h 0.6.0 (`yexp_section`, `yexp_row_type`, `YEXP_R_*`;
  P1). `schema.h` uses them; in 0.5.3 they were internal, and `schema.h`
  had a mirror.

### Who owns which file

| Files | Owner |
|---|---|
| `designer/core/doc.h`, `doc.c`, `dsg.h`, `project.c`, `locate.c`, `platform.c`, `redraw.c`; `designer/app/views.h`, `views.cpp`, `ui.cpp`, `ui.h`, `main.cpp`, `skt.*`; `designer/CMakeLists.txt`; `tests/designer/doc_test.c`, `doc_bench.c`, `core_test.c`, `ui_test.cpp`, `te_test.cpp`, `te_common.h`, `imconfig_designer_te.h` | Shared: frozen during step 4 |
| `designer/core/chart.h`, `chart.c`; `designer/app/view_chart.cpp`; `tests/designer/chart_test.c`, `te_chart.cpp` | A |
| `designer/core/csv.h`, `csv.c`; `designer/app/view_tables.cpp`; `tests/designer/table_test.c`, `te_tables.cpp`; `designer/core/check.c` (the check pack with unsaved tables) | B |
| `designer/core/schema.h`, `schema.c`; `designer/app/view_panels.cpp`; `designer/app/skf.h`, `skf.cpp` (a field's width, commit on Enter); `tests/designer/panels_test.c`, `te_panels.cpp` | C |
| `docs/designer.md`: one subsection each under "Decisions of step 4" (A: "The chart", B: "The tables", C: "The panels"); `docs/status.md`: one entry each | Each part its own subsection and entry |

- A part adds files only under its own names (`chart_*.c`,
  `view_chart_*.cpp`, and so on) and lists them in the core library or
  the UI sources through the integrator: `designer/CMakeLists.txt` is
  shared, so a new file goes in at merge time, in one line.
- A part does not include another part's header. The chart and the
  tables do not draw panels: they select, and the side column shows the
  panel of the selection.

### Tests

Every part keeps `test_designer_core`, `test_designer_doc`,
`test_designer_ui` and its own test passing in ctest from the build tree
with `DISPLAY` unset, with `YSP_DESIGNER_TEST_ENGINE` off and on. Each new
check of a part's test has a hand mutant that fails it, as in step 3.

| Part | Headless (default, `test_designer_<part>`) | Test Engine (optional, `te_<part>.cpp`) |
|---|---|---|
| Shared (done) | `test_designer_doc`, 115 checks: paths, buffers, undo and redo, nested transactions, cancel, typing merges, structured edits as one step in the canonical layout, refusal while the text does not parse, selection and renames through undo and redo, an issue's place to its key, the view file's parts. `test_designer_ui` part 7: a structured edit shows in the text; each main view draws; Ctrl+Z with the chart shown takes the edit back | `designer/undo_views`: the same through the engine's keys |
| A | The chart of each test project with states (bimanual, visuomotor): states, edges of each kind (exit, shared exit per `in` state, next state, `over`), the end. Add, connect, delete (with the exits to it and its `in` entries), rename (with every `to`, `in` and `over.to`), set first: each one undo step, each undone to the same bytes, the check clean after each where the definition allows. Positions: a rename moves the position; undo moves it back; the part round-trips through the view file; the definition's bytes never hold a position | Drag a state: its position changes, the definition's bytes do not. Drag from a state to another: an exit. Select a state: the panel shows it. Delete key on a selected state. A click on an issue of a state shows the chart with the state selected |
| B | The index against ysp/table.h on every CSV in tests/ (same rows, columns and cell texts as `ytb_open_csv()`); a cell edit, a row and a column added and removed, a column renamed: each one step, the bytes read by `ytb_open_csv()` as intended, quoting where a cell has a delimiter, a quote or a line end, the file's line ends kept; the check pack built with an unsaved table (a level the definition names, then removed: the check says so). Only visible rows laid out: count the layouts per frame with 10,000 rows | Scroll a 10,000-row table; edit a cell with Arabic text through the engine's keys; add a row; Ctrl+Z; column widths in the view file after a reopen |
| C | The schema: every key that `yexp_defaults(2)` has for a section shows for an object of that section; a unit for every duration and for every key a header's table has; enum values from the table. The object at a path (a node, a stimulus by kind, a template, a state, an exit, a control by kind, a response, a variable, a data column, a role). An edit of each type writes the canonical value (a duration typed `0.50` is `0.5`); Reset removes the key; a refused value (out of a header's range) still writes, and the check reports it, because the loader is the only validator | Select a stimulus; type a contrast; the text shows it; Ctrl+Z. An issue's field shows marked in its panel. A field with Hebrew text edited with the engine's keys |

### Order of merges

1. This branch, `designer-step4`, into `main` (the shared code and the
   stubs), when the user says so.
2. Each part works on a branch from `designer-step4` (`designer-step4-chart`,
   `-tables`, `-panels`) in its own worktree.
3. The parts share no code, so any order works. Suggested: C, then A,
   then B. The side column is where a selection in A or B shows, so C
   first lets the others see their selections. B goes last because its
   change to `check.c` changes the check that every part's tests run; the
   others then pass against the check they were built with. Each merge
   runs the whole designer suite with the option off and on.
4. The integrator adds a part's new files to `designer/CMakeLists.txt`
   and resolves `docs/designer.md` and `docs/status.md`, the only files
   that two parts touch.

### Prerequisites outside designer/

- **P1** (done in ysp/experiment.h 0.6.0, 2026-11-20): ysp/experiment.h makes the format table's
  section numbers and value types public (`yexp_row.section` and
  `yexp_row.type` as named enums), so `schema.h` drops its mirror. New
  public names, no layout change: 0.5.3 to 0.6.0, with docs/experiment.md,
  the README row, docs/status.md, the header's test and its mutation list.
- **P2** (done in ysp/experiment.h 0.6.0, 2026-11-20): the format table's rows have no `unit` or
  `doc` (G6's row shape has them). A panel shows a format-only key (a
  node's `repeat`, a state's `max_visits`) with its name and its default
  only. Adding `unit` and `doc` changes `yexp_row`'s layout: a minor
  version (0.6.0, with P1).
- Nothing in the player. `pl_check()` serves as it is.

### Risks

- **The check is the cost of an edit**, not the model: 0.2 to 7 ms on the
  examples and 11.5 ms for the loader alone at 1 MB. A panel's drag (a
  slider) gives an edit each frame. Part C commits a drag at its end, or
  merges a drag's steps, so the stack does not hold one step a frame.
- **Undo inside a Skribidi field.** A panel's field keeps Skribidi's own
  undo while it has the keyboard, then commits one step. A field that
  keeps the focus after a click in another view (skf.cpp keeps it when an
  item is hovered) takes the next Ctrl+Z. Part C commits and drops the
  focus when the selection changes.
- **The whole text moves after an edit of a text that was not canonical.**
  The first structured edit writes the canonical layout, so a person's
  layout goes at once, not at Save. The text view puts the caret at the
  change.
- **References after a rename.** `dsg_def_rename()` changes a key, not
  the names that point at it. Part A renames a state and its `to`, `in`
  and `over.to`; part C renames a set member and the references it can
  find by the schema's name types. The check reports the ones missed.
- **The tables' check.** Until part B changes `check.c`, the check pack
  is built from the tables on disk, so a table edit is checked only after
  Save.
- **Excel sources** (`"sheet"` in pack.json) are not CSV. Part B shows
  them read-only.
- **Parallel work in shared files.** If a part finds that it needs a
  shared change, it stops and reports. Two parts that each change
  `ui.cpp` would conflict at merge.

### Not done in the plan

- The window was not opened: no person used the switch or the side
  column.
- The Test Engine tests ran with the engine's fast mode only, 1 run.
- No ASan or UBSan run of the new code.
- The Windows build (MSVC with C4701 and C4244 as errors) was not
  compiled; `bench_designer_doc` is not built on Windows (it uses
  `clock_gettime()`).

## Plan of step 5

Status: planned 2026-11-20 on branch `designer-step5-plan`; the user
accepted its decisions the same day ("Decisions of the plan"). Wave 1
(parts P3, P4 and T) is built and merged on branch `step5-int` the same
day ("Wave 1 integrated"). Part S (the shared interfaces and the stubs)
is built on branch `step5-s` ("Part S as built"). Parts A, B and C are
built and merged on branch `step5-int2`, and part D is built there
("Decisions of step 5"). This
section answers, from the code at `98de716`, what the preview
needs from the player, from `ysp/gfx.h` and from the check, and gives
the decisions, the shared interfaces, the parts, the tests, the
measurements and the order of merges.

The goal ("First version"): a template's states on the player's simulated
display, in a panel. Input in the panel drives the response roles and the
signal roles. The preview is faithful in logic and not in timing, and the
panel says so.

### Decisions of the plan (2026-11-20)

The user accepted these on 2026-11-20. Each one's reason is in the
subsection that it names.

1. **Display.** The default simulated display is 1920 x 1080 at 60 Hz
   when the project names no display ("The built-in simulated rig").
2. **Sound.** No audible sound in the first version. A sound follows its
   plan silently: its onset is its scheduled time. Audible sound, off by
   default, comes later ("Input").
3. **Relative roles.** No mouse capture. A `position2d_relative` role,
   and a `position2d` role with `allow_relative`, is listed as "not
   driven" ("Input").
4. **Edits.** The preview restarts after 300 ms with no change. Hot
   reload is outside the first version ("Edits while the preview
   runs").
5. **Placement.** The preview is a pane beside the main view
   (`UI_SLOT_AUX`) ("The designer's shared interfaces").
6. **Scaling.** No mipmaps: linear sampling for Fit, nearest for 1:1
   ("The GL context").
7. **Data.** The preview writes a data set to `.designer/preview/` and
   replaces it on each run, because a check of the columns that a
   definition writes is a core authoring task ("The data set").
8. **Rig.** The preview uses rig profiles when step 6 adds them. Until
   then it uses a built-in simulated rig ("The built-in simulated
   rig").
9. **The load is off the UI thread.** The session's load and lower run
   on the check thread (part T). The UI thread only adopts a session
   that is ready ("The check thread and the session's load").

### What the code allows today

The preview needs five operations: start a session, step it one frame,
give it input, stop it, and start it again after an edit. The player
library at 0.15.0 has none of them in a form that a UI loop can use.

| Need | What the code does now | Where |
|---|---|---|
| Step one frame | `pl_run()` owns the loop. It blocks in `yscr_begin()`. On the real clock the SIM screen sleeps to each vblank, so one simulated frame holds the UI thread for up to one period, on top of the designer's own vsync. `pl_frame()` is the frame's body and takes a `yscr_frame`, so a host can call it. | `player/run.c` `pl_run()`, `pl_frame()`; `ysp/screen.h` SIM presenter |
| No process-wide clock | `--sim` calls `yrt_virtual_begin(0)` in `main.c`. That puts the whole process on the virtual clock: every `yrt_now_ns()` in the designer reads it, and a wait on another thread blocks until the frame thread moves it. | `player/main.c`; `ysp/rt.h` VIRTUAL CLOCK |
| No other process-global state | `p_row_cap` is a file-scope static ("one session per process"). `h_rules()` and `h_check_ops()` keep static scratch (253 KB of `ytr_trials`, a `ytl_timeline`). `pl_v2_sink` (lower.c) and the raw mouse `yrsp_source` (v2.c) are statics. `pl_loop_active` is a global for a test. ysp/gfx.h builds its NOISE GAUSSIAN table at the first `ygfx_open()` in the process and says that this is not thread-safe before that first call. With part T, a session loads on the check thread while the UI thread steps another one. `p_row_cap` is then a race (the load sets it, the other session's frames read it), and so is `pl_v2_sink` (both threads' frames write it). The host's scratch is a race only for loads on two threads (the tests, or a second worker later). | `run.c` line 1210; `host.c` lines 147 and 274; `lower.c` line 2730; `v2.c` line 1871; `run.c` line 664; `gfx.h` `ygfx__gauss_table()` |
| No allocation per frame | `pl_frame()` allocates nothing; `player_alloc_test` checks it. `pl_run()` allocates its buffers before the loop (events, the seed log of 1.5 MB, the rows' text, the frame visibility ring). A host must get them at the load. | `run.c` `pl_run()` |
| No wait on the UI thread | `--sim` writes its data set in place on the frame thread and hands each row to the OS (`YTR_FLUSH_ROW`): one `write()` a frame for `frames.csv`. A real run queues its writes to a writer thread, because on ext4 one append blocked a frame for 18 ms (player.md). A definition with a sound opens `YAU_BACKEND_NULL`, which on the real clock is miniaudio's null device, a real-time thread; `pl_check()` opens a device that never runs. | `run.c` `pl_wq_push()`, `pl_writer_open()`; `lower.c` the audio open |
| GL on the right thread | `--sim` and `pl_check()` draw on gfx's null backend. Nothing opens the player's gfx on a context that a host gives, and every gfx call must come from the thread that owns the context (gfx.h, MEMORY AND THREADS), while the load makes textures, glyph sets and buffers. | `lower.c` `pl_screen_open()`, `pl_stim_make()` |
| Input from a host | `--sim` feeds a scripted participant (`pl_participant_plan()`, `--samples`). A real run reads the screen's queue. No call takes a key, a choice or a sample from the caller. | `run.c` lines 2340 to 2420; `v2.c` `v2_feed()` |

The frame path itself reads no clock: ysp/gfx.h ("NOTHING READS A
CLOCK"), ysp/timeline.h, ysp/response.h, ysp/sig.h and ysp/trials.h take
their times from the frame. Only the audio device, the data set's writer
thread (for its own schedule) and a real run's input poll read the clock.
So the session can run on its own frame grid, on the real clock, with no
virtual clock. The frame path's gfx calls are `ygfx_apply()`,
`ygfx_begin()`, `ygfx_draw()`, `ygfx_buffer_update()` and `ygfx_end()`
only (run.c, v2.c); a frame that does not draw makes no GL call.

### The player's stepped session

**Decision.** The player library gets a session that its caller steps
(player 0.16.0). It is the `--sim` session with the built-in simulated
rig, a data set written by the writer thread of a real run, and a gfx
that the load records and the GL thread makes. `ysp_player --sim` does
not change: its output bytes stay the same (`player_test.py`,
`compare.py`).

```c
/* player/player.h, 0.16.0: the simulated session, stepped by its caller
 * (the designer's preview). pl_sim_load() runs on any thread and makes no
 * GL call. pl_sim_attach(), pl_sim_frame() with draw and pl_sim_detach()
 * run on the thread whose GL context is current. The other calls run on
 * the thread that owns the session at the time (see the check thread). */
typedef struct pl_sim_where {
    int         trial;          /* the session trial; -1 before the first  */
    const char* leaf;           /* the leaf node's path, or NULL            */
    int         condition;      /* ytr_trial_info.condition: the table row,
                                 * -1 for a track trial                     */
    const char* tmpl;           /* the template, or NULL between trials     */
    const char* state;          /* version 2: the current state; NULL: the
                                 * end, or an events template               */
    int         phase;          /* PL_SIM_EVENTS, _RESPONSE, _AFTER, _BETWEEN */
    int64_t     frame;          /* the next frame's index                   */
    int64_t     t_ns;           /* its onset from the session's first frame */
    const char* outcome;        /* the last trial's, or NULL                */
    const char* response; int64_t rt_ns;   /* the last trial's          */
} pl_sim_where;

typedef struct pl_sim_desc {
    ypak_desc   pack;           /* in memory; the caller keeps it until pl_close() */
    const char* def_text; size_t def_len;   /* NULL: the pack's experiment */
    uint64_t    seed;
    const yrig_profile* rig;    /* NULL: the built-in simulated rig          */
    const ycol_cal* cal;        /* the rig's calibration, or NULL           */
    int         n_fields; pl_field_arg fields[PL_MAX_FIELDS];
    const char* out_dir;        /* the data set's folder; must not exist    */
    const char* participant;    /* the scripted participant of the trials
                                 * that live() leaves to it: "correct",
                                 * "p=0.8", "silent"                        */
    double      rt_s;
    /* the gfx (ysp/gfx.h 0.23.0, prerequisite P3): recorded at the load,
     * made at pl_sim_attach(); caps from a gfx the GL thread opened.
     * NULL: the null backend (headless tests) */
    const ygfx_gl_caps* gl_caps;
    const ygfx_cache* cache;    /* compiled programs, kept across sessions  */
    /* hooks, each optional */
    bool (*live)(void* ctx, const pl_sim_where* w);   /* at a trial's start:
                                 * true, the caller answers this trial;
                                 * false, the scripted participant does    */
    void (*on_code)(void* ctx, const char* role, uint32_t code, int64_t t_ns);
    void*       ctx;
} pl_sim_desc;

/* The load and lower: pl_check()'s with the rig and the data set. r as
 * pl_check()'s. No GL call. */
int  pl_sim_load(pl_player* p, const pl_sim_desc* d, pl_check_result* r);
/* Makes the recorded GL objects on the current context, for at most
 * budget_ns (0: all). 1 when done, 0 when more is left, < 0 an error.
 * Then the session's first drawn frame primes the programs. */
int  pl_sim_attach(pl_player* p, int64_t budget_ns);
/* One frame: its onset is the last one's plus the period. draw false: the
 * frame's logic and no GL call (a fast forward, before or after attach).
 * draw true: after attach only. 0 to go on, 1 the session is over, < 0 a
 * refusal: p->error, and the object in p->fail_obj and p->fail_name.
 * Allocates nothing. */
int  pl_sim_frame(pl_player* p, bool draw);
/* Input, at a time after the last frame's onset and at or before the next
 * one's; it reaches the session at the next pl_sim_frame(). */
int  pl_sim_key(pl_player* p, int64_t t_ns, int scancode, bool down);
int  pl_sim_choice(pl_player* p, int64_t t_ns, int choice, bool down);
int  pl_sim_sample(pl_player* p, int64_t t_ns, int role, float x, float y);
int64_t pl_sim_next_onset(const pl_player* p);
void pl_sim_where_now(const pl_player* p, pl_sim_where* w);
/* The display's texture: its GL name (GL_RGBA8, rows bottom-up). */
bool pl_sim_texture(const pl_player* p, ygfx_native* out);
/* Stops a session before its end: the data set records it as stopped
 * ("aborted", reason "request"). */
void pl_sim_stop(pl_player* p);
/* Frees the GL objects and closes the gfx (the GL thread). After it,
 * pl_close() makes no GL call and may run on any thread. */
void pl_sim_detach(pl_player* p);
```

**What changes in the player, by file.**

- `player.h`: the declarations above. `pl_player` gains `sim` (the
  desc's hooks, the frame grid, a fixed input queue of 256 entries),
  `row_cap` (was `p_row_cap`), `rules_scratch` and `ops_scratch` (were
  the statics of `host.c`), `v2_sink`, and `stand_in`.
- `lower.c`: `pl_load_pack()` splits `p->checking` into two flags.
  `stand_in` (no rig file, no devices, the never-running audio device,
  the tier 3 stand-ins) is set by `pl_check()` and `pl_sim_load()`;
  `checking` (the null gfx, no warm-up draw, no data set, the
  device-button check of 0.15.0) by `pl_check()` only. Of the 15 uses of
  `checking`, each goes to one of the two. The rig is the desc's or the
  built-in one. `pl_screen_open()` gets a branch for the stepped session:
  no `yscr_open()`; `ygfx_open()` deferred (P3) at the rig's display
  size, with its view and calibration, or `pl_gfx_open_null()`. The
  text programs' warm-up and `ygfx_prime()` move to the first drawn
  frame after attach. The `pl_v2_sink` static moves into the player.
- `run.c`: `pl_run()` splits into `pl_run_prepare()` (the allocations),
  `pl_run_frame()` (the stats reset, `pl_frame()`, the blank frame at the
  end), `pl_run_drain()` and `pl_run_finish()`. `pl_run()` calls them
  around `yscr_begin()` and `yscr_flip()`; `pl_sim_frame()` calls them
  with a frame it makes. `pl_draw()` gets the draw flag (the RDK still
  steps when it is false). `pl_writer_open()` starts the writer for a
  stepped session too. A trigger fires to the sink and `on_code` when no
  screen is open. `pl_participant_plan()` asks `live()`. A sound's onset
  is its plan (decision 2): `pl_sounds()` and `pl_from_sound()` take the
  scheduled time, final, and call no audio device.
- `v2.c`: `pl_sim_sample()` puts samples in the queue that a real run's
  raw mouse samples use (`S->live`), read by `v2_feed()` for a stepped
  session too. The `yrsp_source` static moves into the session.
  `pl_sim_where_now()` reads `def_cur`.
- `host.c`: the two scratch areas come from `pl_alloc()` at the first
  use in a load, not from statics. In the designer every load runs on
  the check thread, so this is not a race there; `test_player_sim` loads
  on two threads, and a second worker would.
- New `player/sim.c`: the `pl_sim_*` functions.

**The frame grid.** Frame k's onset is `YRT_VIRTUAL_T0_NS + (k + 1) x
period`, and its flip record arrives with frame k + 1, with onset equal
to the plan, tier SIM, no drop: what the SIM screen gives on the virtual
clock. The equality test (below) checks that the grid gives `--sim`'s
data.

**Rejected.**
- `yrt_virtual_begin()` in the designer. It is process-wide (rt.h): the
  designer's other users of `yrt_now_ns()` and the session's writer
  thread would read a clock that stands still between waits.
- `pl_run()` on a thread of its own. The SIM screen would still sleep
  each period on the real clock, and its gfx calls must come from the
  GL thread.
- A custom presenter (`YSCR_BACKEND_CUSTOM`) under `pl_run()`. The screen
  plans each frame from `yrt_now_ns()`, so a stall of the UI would drop
  frames and change the session's logic.
- Sharing the check's `pl_player`. The check closes it after each check
  and has no rig, data set or GL.

**As built (P4, player 0.16.0, 2026-11-20; branch `step5-p4-player`).**
The API above, with these changes. The reference is
[player.md](player.md#reference-the-stepped-session).
- `pl_sim_desc` gains `width`, `height` and `period_ns` (0: 1920 x 1080
  at 60 Hz). A rig profile has no display size or rate, and the panel
  chooses them. It also gains `gl_proc` and `gl_ctx`, which P3's
  `ygfx_attach()` takes.
- No `pl_sim_rig()`: `rig` `NULL` is the built-in rig, which is
  `pl_check()`'s stand-ins. `cal` is the display's calibration; `NULL`
  gives the nominal one to a definition that requires one.
- The grid starts at vblank 7, not 1: frame k's onset is
  `YRT_VIRTUAL_T0_NS + (7 + k) x period`. `--sim`'s SIM screen shows 6
  warm-up frames at its open, and with the same start the stepped data
  equal `--sim`'s byte for byte. With a sound, `--sim`'s null device
  moves the virtual clock 0.55 s at its open, so there `--sim`'s times are
  0.55 s later.
- `pl_sim_frame()` gives 1 when the end's frames are done.
  `pl_close()` then writes the last rows and `session.json`, so the
  writer's join and the disk wait are on the thread that closes.
- The text programs' first draws and `ygfx_prime()` are at the end of
  `pl_sim_attach()`, not in the first drawn frame: they may allocate, and
  a frame allocates nothing. Changed at the integration (player 0.17.0,
  "Wave 1 integrated"): the text programs are made at the load, and the
  attach primes.
- `live()` is asked at every trial's start, also for a template with no
  response, so a pin can stop at any leaf.

### The built-in simulated rig

Until step 6 adds rig profiles (decision 8), each session gets one rig
that the player makes in memory (`pl_sim_rig()`), as `player_make_rig`
makes the test profiles:
- the display: 1920 x 1080 at 60 Hz (decision 1). The panel can choose
  1280 x 1024, 2560 x 1440 or 800 x 600 (`--sim`'s) and a rate of 60,
  120, 144, 240 or 500 Hz; the view file keeps the choice.
- the view: 570 mm away, 1920 px over 518.4 mm (the check's stand-in,
  scaled to the width as `pl_gd_units()` does);
- the calibration: the nominal one of `pl_check_nominal_cal()` (sRGB
  primaries, D65 white at 100 cd/m2, gamma 2.2), given only to a
  definition with `"calibration": "required"`; any other gets identity,
  as on a rig with no calibration;
- no devices and no signals: every input role is the person or the
  scripted participant, every output role the sink.
- Why 1920 x 1080: a stimulus placed in px is where a typical rig shows
  it. The equality test with `--sim` gives the session 800 x 600.
- The panel says "Built-in simulated rig: 1920 x 1080, 60 Hz, nominal
  view" and names the stand-ins in use (the check's tier 3 needs).

### The data set

Decision 7: each session writes a data set, as `ysp_player --sim` does:
`session.json`, `trials.csv`, `frames.csv`, `events.csv` and the streams.
- The folder is `.designer/preview/run-N/`, N one more than the last
  session's. When a session is adopted (below), the old one is stopped
  and closed, and the check thread removes every other `run-*` folder.
  So `.designer/preview/` holds the current run, and for a moment the one
  before it: each run replaces the last. A folder must not exist before
  `yds_open()`, which is why the name changes.
- The writes go through the player's writer thread, as in a real run, not
  in place: no `write()` on the UI thread. The writer is a `yrt_worker`
  with `no_elevate`, started at the load. Its queue has one producer at a
  time: the check thread during the fast forward, then the UI thread.
  The handover goes through SDL's event queue, whose lock orders the
  two.
- A session stopped by a restart has `"aborted"` with the reason
  `"request"` in its `session.json`; its finished trials keep their rows.
- The panel shows the folder's path, the last trial's response, rt and
  outcome (`pl_sim_where_now()`), and a button that opens `trials.csv` in
  the Tables view, read-only.
- WebAssembly: the folder is in the browser's storage; without threads
  the writes are in place. Step 7 decides.

### The check thread and the session's load

The check's cost was measured on 2026-11-20 (branch
`designer-check-cost`, commit `003a186`; Intel Core i5-14500, one
P-core, gcc 12.2, Release; median and max in ms):

| Project | A keystroke, the texts unchanged | A keystroke in a text literal |
|---|---|---|
| bimanual | 9.1, 12.9 (all of it `pl_check()`) | |
| stroop | 0.5 | 7.1 (the pack's build alone 6.4) |
| the other projects in the repository | 0.2 to 1.9 | |
| generated: 200 text stimuli in 4 scripts | 5.3 | 41 |
| the same, and a stimulus that shows 10,000 words from a table | 37 | 201 |

A session's load repeats the check's load and adds the data set and the
recorded gfx, so it costs at least as much: up to 12.9 ms on bimanual.
That does not fit in a 16 ms frame beside the UI's own 1.2 to 2.1 ms and
a step (decision 9).

**Part T, the check thread** (a prerequisite of step 5). One worker
thread runs two kinds of job, each newest-wins: a job that arrives while
one of its kind waits replaces it.
- A **check job**: the text, a copy of the dirty tables (60 KB in the
  measured case) and the revision. It runs `dsg_check()` and posts the
  result.
- A **session job**: an accepted text, its pack reference, the
  revision, and a copy of the preview's settings (rig, seed, fields,
  participant, pin). It runs `pl_sim_load()`, then the fast forward to
  the pin (`pl_sim_frame()` with `draw` false; no GL), and posts the
  session.
- A waiting check runs before a waiting session job: the check gives the
  issues, and a session job's text was checked already.
- Results go to the UI thread through SDL's event queue, which also
  wakes the loop's wait, as the folder dialog's answer does. Each result
  carries its revision.
- Before T starts its thread, `main.cpp` opens one GL gfx on the window's
  context. That builds gfx's NOISE table on the UI thread (not
  thread-safe before the first open) and gives the GL caps
  (`ygfx_gl_caps_get()`) that each deferred open takes. The headless
  tests open a null gfx first, for the same table.
- WebAssembly without threads (no cross-origin isolation) runs both jobs
  in the frame. Step 7 decides.

**Part T as built** (2026-11-20, branch `step5-t-worker`;
`designer/core/worker.c`, `check.c`, `dsg.h`; `ui.cpp`, `main.cpp`).
- **The thread is a `yrt_pump` (ysp/rt.h), not C11 threads.** The core had
  no thread code. glibc has `<threads.h>`, macOS has none, and MSVC has it
  only from Visual Studio 2022 17.8. The pump is in the player's header
  implementations already and runs on the three CI systems. It starts its
  thread with `PTHREAD_EXPLICIT_SCHED` and `SCHED_OTHER` (Windows:
  `CreateThread()`, normal priority), never elevates, and blocks on a
  condition variable when it has no job. So the thread does not inherit
  SCHED_FIFO and does not spin.
- **The atomics** (`dsg_atomic_add()`, `dsg_atomic_load()`) are the
  compilers' intrinsics in `platform.c`: `<stdatomic.h>` needs
  `/experimental:c11atomics` on MSVC.
- **Newest wins.** The pump's messages are only wake-ups. Each kind of job
  has one slot, and a post into a full slot replaces the job in it. The
  pump's publish lock guards the slots; no job runs under it. A wake-up
  that finds no job returns.
- **No allocation per post at steady state.** A check job copies the
  text, the project's description and the dirty tables (name, revision,
  bytes) into buffers of its slot. Three slots rotate: the poster's, the
  waiting job's and the running job's. A post allocates only when a
  buffer must grow. `dsg_check()` takes the tables as a document;
  `dsg_check_tables()` takes the copy, so the thread never reads the
  document.
- **Results.** The thread copies the checker's report into storage of the
  result, because the checker reuses its own at the next check. Three
  results rotate: the thread's, the ready one and the UI's. The UI's
  report stays valid until its next take.
- **Order on the thread:** a waiting check, then the sessions handed back
  to close, then a waiting session job.
- **Revision marks.** Each result carries the revision of its job. The UI
  takes the newest check result at the start of each frame and posts a
  check at the end of a frame that changed the document.
  `ui_work_pending()` is false while a job runs: the wake-up
  (`UiPlatform.wake`, an SDL event that `main.cpp` pushes) brings the
  frame that takes the result. A result from before the last open is
  dropped: the document's revision does not go back at a reset, so the
  UI keeps the revision of the open as a floor.
- **Sessions: a stub behind a seam.** `dsg_session_ops` (load and close,
  both on the thread) is where part A puts `pl_sim_load()` and
  `pl_close()`. With no `load` the stub succeeds and loads nothing. A
  session that finishes while a newer session job waits is closed on the
  thread and never posted. A session that is posted and never taken is
  closed on the thread when a newer one arrives. A failed load posts its
  error, and the thread closes what the load made.
- **gfx's NOISE table.** `dsg_worker_start()` calls
  `ygfx_noise_value(0, 0, 0, YGFX_GAUSSIAN)` on the calling thread before
  the thread exists. That builds the table, so no two threads build it
  at once. This covers `main.cpp` and every headless test in one place,
  in any order of calls. The plan's GL gfx in `main.cpp` (before
  `ui_init()`) is for the GL caps, which need P3's `ygfx_gl_caps_get()`:
  it comes with part A. `main.cpp` registers the wake-up's SDL event type
  before `ui_init()`, and `ui_init()` starts the thread after
  `skt_init()` (Skribidi's counter, below).
- **No threads.** `desc.no_thread`, `YRT_NO_THREADS` or WebAssembly
  without pthreads run the jobs in `dsg_worker_wait()`, on the caller's
  thread. The UI then runs the check after its post, and takes the result
  in the next frame.
- **The tests wait.** `ui_wait_checks()` blocks until every posted check
  ran. The frames of `test_designer_ui`, `test_designer_table_ui`,
  `test_designer_te` and `bench_designer_chart` call it first, so a check
  shows one frame after its edit. `te_test.cpp` yields one more frame
  after an open and after three edits whose result the test reads.

**Process globals on the check path** (2026-11-20, from the code; the UI
thread and the check thread at once):

| Global | Where | Threads | Verdict |
|---|---|---|---|
| NOISE GAUSSIAN table (`ygfx__gauss_tab`, `_ready`, the scratch `x[]`) | `gfx.h` `ygfx__gauss_table()` | the check thread (`pl_check()`'s null gfx); later the UI's GL gfx | A race at the first build. Fixed: built in `dsg_worker_start()` before the thread |
| `h_rules()` scratch (253 KB `ytr_trials`), `h_check_ops()` `ytl_timeline` | `player/host.c` 147, 274 | the check thread only: the UI's `yexp_load()` calls pass no host | Safe with one check thread. P4 moves them into the player |
| `pl_v2_sink`, the raw mouse `yrsp_source` | `player/lower.c` 2730, `v2.c` 1871 | the check thread only, until part A steps a session on the UI thread | Safe now. P4 moves them |
| `p_row_cap`, `pl_loop_active` | `player/run.c` 1210, 664 | `pl_run()` only; `pl_check()` does not touch them | Safe now. P4 moves `p_row_cap` |
| `yexp_load()` and `ysp/json.h` | `experiment.h`, `json.h` | both threads | No mutable statics: safe |
| The pack tool | `pack/layout.c` (HarfBuzz version pointer, same value each time), `pack/colr.c` and `layout.c` (Windows QPC frequency, same value) | the check thread only | Benign: each write stores the same value. Gone in pack/layout 0.3.5 (step 6's integration): the versions are constant and the frequency is read at each call |
| ysp/video.h (FFmpeg's `dlopen` once, a heap counter) | `video.h` | the check thread | `pthread_once` and an atomic: safe |
| Skribidi's font collection id (`static uint32_t id`, `++id`) | `skb_font_collection.c` 328 | the UI once in `skt_init()`; the pack tool at each build | Safe: the thread starts after `skt_init()`, and the UI makes no other collection. A second UI collection later would race (a duplicate id, which only keys that collection's layout cache). Step 6's build thread made it a race; Skribidi patch 0010 (pack/layout 0.3.5) makes the add atomic |
| Skribidi's attribute collection id; `skb_script_from_iso15924_tag()`'s lazy sort | `skb_attribute_collection.c` 36, `skb_layout.c` 145 | the check thread only; no caller | Safe. The id: as the font collection's (patch 0010) |
| HarfBuzz's lazy loaders (Unicode functions, shapers, fonts) and the language list | `hb-ucd.cc`, `hb-shaper.cc`, `hb-common.cc` | both threads | Atomic compare and exchange (`HB_NO_MT` is not set); the threads share no face or font: safe |
| ysp/rt.h clock state | `rt.h` | both | Atomic; only `ysp_player --sim` writes the virtual clock: safe |
| The audio device | `lower.c` | the check thread | `pl_check()` opens a custom stand-in with no thread; miniaudio is not reached |
| `schema.c` `g_fields`, `chart.c` `chart_err`, `skt.cpp`, `skf.cpp` | designer | the UI only | Safe |

No call on the check path raises a thread's priority. `pl_check()` starts
no writer thread (`args.sim`). A pack with a video can start one FFmpeg
thread, made with `PTHREAD_EXPLICIT_SCHED` and `SCHED_OTHER`, or an
`ffmpeg` process.

**Measured** (Linux desktop, Intel Core i5-14500, gcc 12.2, Release,
`bench_designer_check WORK 30 200 PROJECT worker` pinned to CPUs 2 and 4,
two P-cores, under the measuring lock, two passes; load average (1 min)
0.57 before the first pass and 0.74 after the second, the bench's own two
threads included). Microseconds, median, p95 and max of each pass; 200
keystrokes, 30 for a changed text. "Before" is `dsg_check()` in the frame,
as the UI did it. "Post" and "take" are what the UI frame pays per
keystroke now. "Latency" is from the post to the thread's wake-up, with
the result ready.

| Project | Before: the check in the frame | Post | Post with a dirty table (18.9 KB) | Take | Latency |
|---|---|---|---|---|---|
| bimanual, a keystroke | 4,781, 5,213, 10,714; 4,788, 5,257, 10,745 | 3.4, 4.7, 11.5; 3.4, 4.7, 11.2 | no table | 0.3, 0.5, 1.6; 0.2, 0.5, 1.7 | 4,795, 4,984, 8,786; 4,789, 5,225, 8,880 |
| many, a keystroke, the texts unchanged | 5,772, 5,935, 6,209; 5,793, 5,883, 6,160 | 10.3, 16.9, 23.9; 10.0, 13.9, 18.9 | 12.1, 19.6, 25.2; 11.9, 13.2, 24.6 | 0.2, 0.8, 4.1; 0.2, 0.3, 0.9 | 5,327, 6,020, 8,348; 5,288, 5,358, 8,064 |
| many, a keystroke in a text | 40,837, 41,709, 42,237; 40,977, 41,583, 41,739 | | | | 40,976, 41,683, 41,895; 40,935, 41,662, 42,122 |

- The UI frame's cost per keystroke falls from the whole check (4.8 to 41
  ms median) to the post and the take: at most 25 us. The post is the
  copy of the text (60 KB in "many") and of the dirty tables into kept
  buffers; the take is a swap under the lock.
- The latency is the check's own time: the hand-over adds nothing that
  the passes resolve. The result shows in the frame after the wake-up, so
  up to one more frame (16.7 ms at 60 Hz).
- **glibc's trim.** The first two passes, without `mallopt()`, gave a
  "many" latency 1.4 to 1.7 ms above the check in the frame (7.2 ms
  against 5.8 ms median). On CPU 2 alone the gap stayed (6.9 against
  5.8 ms), so it was not the move between cores. With
  `MALLOC_ARENA_MAX=1` it went (5.6 ms), and with
  `MALLOC_TRIM_THRESHOLD_=67108864` the check took 5.1 ms on either
  thread. glibc gives a second thread an arena of its own, and gives that
  arena's freed top back to the kernel after each check, so the next
  check faults its pages in again. `dsg_worker_start()` now sets
  `M_TRIM_THRESHOLD` to 64 MB (glibc only): the numbers above. The cost
  is up to 64 MB of freed memory that each arena keeps. Not measured on
  Windows or macOS, whose allocators differ.
- bimanual's check measured 9.1 ms median at CPU 2 alone earlier on
  2026-11-20 ("Evidence: the check's cost") and 4.8 ms here; the code of
  the check is the same, the CPUs and the time are not.

**Hand mutants** of the thread (each reverted; each fails
`test_designer_worker`):

| Mutant | Fails |
|---|---|
| Queue: a waiting session job runs before a waiting check | the check before session 21 |
| Newest wins: a check posted while one waits does not replace it | the result is the last text's |
| Newest wins: the oldest waiting session job runs | 301 and the newest, 305, loaded |
| Newest wins: a replaced session job keeps its pack reference | every pack freed at the stop |
| Reference count: retain does not count | aborts (the pack freed under a session) |
| Reference count: release frees one reference early | crashes (the same) |
| Reference count: a rebuild frees the bytes, not its reference | the old pack's bytes are whole |
| Stale drop: a session is posted though a newer job waits | session 20 was never posted |
| Stale drop: a session never taken is not closed | 402, never taken, was closed |
| Revision: the result is not marked | the result's revision |

**Tests and sanitizers.** `test_designer_worker` (62 checks) runs the
thread with the stub session: the gates in its hooks hold the thread, so
the jobs wait in a known order. 30 runs in Release and 30 under
ThreadSanitizer passed. The designer's 9 tests pass in Release, under
ASan with UBSan (`halt_on_error`) and under ThreadSanitizer (gcc 12,
`setarch -R`, `halt_on_error`), with `DISPLAY` unset;
`test_designer_ui` and `test_designer_te` passed 20 times in a row in
Release and 5 times under ThreadSanitizer. The whole ctest (139 tests)
passed in Release.

**What the thread hands over.**
- **Ownership.** The session job allocates the `pl_player` (1.6 MB,
  heap). The job owns it until it posts it; from the event's delivery
  the UI thread owns it. No two threads touch one `pl_player` at a time.
- **Lifetime.** The session and every pointer in it (the definition's
  arena, the pack's bytes, the data set) live until `pl_close()`. A
  session that the UI thread drops is detached on the UI thread
  (`pl_sim_detach()`, the GL objects) and then closed on the check
  thread (`pl_close()`: the writer's last writes, `session.json`, the
  memory), so the join and the disk wait are never on the UI thread. A
  session that was never attached has no GL objects, and either thread
  may close it.
- **The pack.** The checker hands out its pack with an atomic reference
  count (`dsg_checker_pack()`, retain, release). Each check result holds
  a reference to the pack it ran against. `dsg_worker_session()` adds the
  job's own reference when the UI posts the job (as built: not when the
  thread takes it, because the result's reference can go first); the
  reference goes with the session; the thread that closes the session
  releases it after `pl_close()`. A rebuild drops only the checker's own reference, so the
  bytes live while a session uses them. No copy: a pack with images or
  videos can be large.
- **A session that is ready and stale.** When the event arrives, the UI
  thread compares the session's revision and settings with the
  document's revision and the preview's settings now. If they differ, or
  a newer session job waits or runs, it drops the session (above) and
  keeps the one it shows. The check thread also drops a finished session
  itself, before it posts it, when a newer session job waits. So a stale
  session never shows and never makes a GL object.
- **A session that fails its load** (a refusal that the check did not
  give, or a data set folder that cannot be made) posts its error; the
  preview shows it as a preview issue and keeps the session it shows.

**How the UI thread adopts a session.** In the frame the event arrives,
the preview takes the session and calls `pl_sim_attach()` with a budget
of `PV_ATTACH_BUDGET_MS` (4 ms) a frame, until it gives 1. The old
session keeps running meanwhile. In the frame that attach completes, the
preview stops the old session (`pl_sim_stop()`), detaches it, hands it
to the check thread to close, and shows the new one; the new one's first
step is the next frame. So no UI frame does a load, and a frame does the
attach's budget or a step's work, not both.

**Rejected.**
- The load on the UI thread, in the frame after the result. bimanual's
  `pl_check()` alone takes up to 12.9 ms.
- A second GL context on the check thread, shared with the window's.
  Framebuffers and vertex arrays are not shared between contexts, gfx
  makes both, and WebGL has no shared contexts.
- A load against the null gfx and a player-side remake of each texture
  and glyph set on the UI thread. The player's structs keep gfx handles
  (`ygfx_stim`, the text runs), so every handle would need a new value.
  The deferred gfx (P3) keeps the handles.

### The GL context

**Prerequisite P3, ysp/gfx.h 0.23.0.** Two features:
- **An output texture.** gfx writes its output stage into framebuffer 0
  (target 0 in `ygfx__gl_pass_begin()`), which in the designer is the
  window. With `ygfx_desc.output_texture` (appended; headless only), gfx
  makes a GL_RGBA8 texture of `width` x `height` and a framebuffer for
  it, and the output stage writes it in place of framebuffer 0.
  `ygfx_read_output()` and CAPTURE read it. `ygfx_output_native(g,
  &nat)` gives its GL name (the `ygfx_native` of v0.21). `end()` leaves
  framebuffer 0 bound, and its cache says so. The probe's copy from
  framebuffer 0 (`glCopyTexSubImage2D`) needed a window at least the
  display's size and a fixed order of draws, and cost a copy a frame.
- **A deferred open.** With `ygfx_desc.defer` and the caps of a gfx that
  the GL thread opened (`ygfx_gl_caps`), `ygfx_open()` makes no GL call:
  it checks the calibration, builds its tables and records each backend
  call (programs, textures with a copy of their texels, buffers, glyph
  sets) with the handle the GL backend will give it. `ygfx_attach(g,
  gl_proc, ctx, budget_ns)` on the GL thread makes them in order, for at
  most the budget a call, frees the copies, and from then on the gfx is
  a GL gfx. Draws before the attach are refused. The handles do not
  change, so the player's structs stay valid. The calibration's check,
  20 to 33 ms in `pl_check()` before it dropped the gfx's calibration
  (player.md, "Measured"), is then on the check thread.
- gfx makes the texture and replays the calls itself, so its state cache
  stays right, and the host makes no GL call of its own (the reason for
  v0.21's shared textures). A struct grows, so the version is minor.
  ysp-vis carries `ygfx_desc` and reports `GFX_VERSION`: its
  `pyproject.toml` and `YV_VERSION` move too.

**P3 as built (branch `step5-p3-gfx`, 2026-11-20).** The numbers and the
tests are in [gfx.md](gfx.md#v0230-the-output-texture-the-deferred-open-shared-programs).
- **Measured before the build.** Each backend call of a session's load
  was timed on the RX 6600 and on llvmpipe (EGL, no window). With Mesa's
  shader cache warm, a session's GL work was 6.1 ms or more on the RX
  6600 and 24 ms or more on llvmpipe. Without the cache it was 215 ms or
  more, with single compiles of up to 65 ms. `ygfx_open()` also spent 24 ms
  of CPU in the calibration's check. None of this is far under the 4 ms
  budget, so the deferred open was built. The user then asked for three
  more things (below).
- **The calibration's check, once a process.** `ycol_cal_check()` was 24
  of the open's 29 ms (perf). gfx keeps the hashes of the calibrations that
  passed. A deferred open now takes 1.3 to 1.4 ms on the check thread, and
  the first open of a calibration in a process takes 24.6 ms. The NOISE
  table's first build takes a lock, so the first open no longer has to be
  on the UI thread for the table's sake (T may keep it, for the caps).
- **Shared programs.** `desc.share` names a gfx open on the same context,
  in practice `main.cpp`'s first gfx. A session's gfx then compiles none of
  the built-in programs. A program that only a session needs (text,
  images bottom first) is compiled once, by the first session that needs
  it, and later sessions find it in the pool. Each program counts its users,
  so the gfx close in any order. A deferred gfx joins the pool at its
  attach, not at its open: the check thread never reads `main.cpp`'s gfx.
  Rule: `desc.share` must be open and attached when the attach starts.
- **The attach primes without waiting.** `ygfx_attach()` draws every
  program once, a program a step, and skips the prime's read. The attach
  of a player project then takes one call of 0.16 to 0.29 ms on the RX 6600
  (shared programs, Mesa's cache warm or not), and 2.3 to 5.6 ms in one or
  two calls on llvmpipe. The images case takes 8 calls on the RX 6600 and 15
  to 17 on llvmpipe. Uploads over 4 MB go in bands. One call can pass the
  budget: the llvmpipe storage of a 4096 x 4096 texture takes 17 to 22 ms,
  and a cold compile up to 65 ms. The budget is a target.
- **What P4 changes** (done at the integration, player 0.17.0, "Wave 1
  integrated"). `pl_sim_attach()` must not call `ygfx_prime()`
  after `ygfx_attach()` returns 1, because the attach has primed every
  program, and `ygfx_prime()` would add its read's wait (0.6 ms on the RX
  6600, 9.5 ms on llvmpipe). `pl_gfx_warm()`'s `ygfx_crun()` calls make
  the text programs and the per-set state. They can run at the load,
  before the attach (recorded), so the attach makes and primes them. The
  stepped session's open should set `desc.share` to the gfx that
  `main.cpp` opens, which T or S hands to the session job, and
  `desc.gl_caps` from `ygfx_gl_caps_get()` of that gfx. The names are the
  plan's: `desc.defer`, `desc.gl_caps` (`const struct ygfx_gl_caps*`),
  `desc.output_texture`, `ygfx_attach(g, gl_proc, gl_ctx, budget_ns)`
  (1 attached, 0 more to do, < 0 error), `ygfx_output_native()`,
  `ygfx_reset_state()`.

**State.** Read from the code of gfx.h 0.22.0 and of the vendored
`imgui_impl_opengl3.cpp`:

| State | ysp/gfx.h | ImGui's OpenGL3 backend |
|---|---|---|
| Program, VAO, array buffer, texture unit 0, active unit, viewport, blend (enable, equations, functions) | Sets them where its cache says they changed; leaves them | Saves and puts back each |
| Scissor, depth, stencil and cull tests | Assumes all off; never sets them | Saves the enables, sets its own, puts them back |
| Sampler object on unit 0 | Assumes none | Binds its own while it draws, puts the old one back. Its sampler overrides the texture's own filter |
| Textures on units 1 to 5, uniform buffer ranges 0 and 1, clear color, color mask | Sets the first three; the color mask only at a clear | Does not touch |
| Framebuffer | Binds its targets; today leaves the last one bound | Draws into the bound one; does not bind |
| Unpack alignment and row length | Sets 1 and 0 at an upload | Saves and puts back (since 2026-07-15) |

So: `pl_sim_frame()` with `draw` true and `pl_sim_attach()` call
`ygfx_reset_state()` first, because ImGui changed the program, the
textures and the blend since gfx's last call. With 0.23.0, `end()`
leaves framebuffer 0 bound; `main.cpp` also binds framebuffer 0 before
its clear, so no later framebuffer user can make ImGui draw into the
preview's texture. The designer enables no scissor, depth or stencil
test outside ImGui; a GL test checks that gfx draws the same bytes after
an ImGui frame as with none.

**Where in the frame.** The preview's view steps the session in its
`draw` hook, inside `ui_frame()`, then calls `ImGui::Image()` with the
texture's name. gfx's draw is in the command stream before
`ImGui_ImplOpenGL3_RenderDrawData()`, on one context, so no fence is
needed.

**Color.** The texture is GL_RGBA8 (not GL_SRGB8_ALPHA8) and holds the
output stage's 8-bit device codes. ImGui samples them and writes them
into the window's framebuffer, which has no sRGB encoding (GL ES and
WebGL have no `GL_FRAMEBUFFER_SRGB`). So the designer's monitor gets the
codes that a rig's output stage sends. With the built-in rig's nominal
calibration, a monitor near sRGB shows the intended luminance. The
output alpha must be 255, and the image is drawn with a white tint and
no style alpha, or ImGui's blend mixes it with the background: the GL
test checks both.

**Size and pixel density.**
- The panel shows the texture at a scale `s` in the window's framebuffer
  px: "Fit" (the largest `s` that fits, at most 1) or "1:1" (`s` = 1,
  with scroll bars). ImGui's size in points is `W x s /
  io.DisplayFramebufferScale`, at a position snapped to a whole
  framebuffer px. At 1:1, one participant px is one physical px of the
  designer's monitor, on a high-density display too.
- Decision 6: at 1:1 the image draws with ImGui's nearest sampler (its
  draw callback), so the bytes are exact; at Fit, with ImGui's linear
  sampler. No mipmaps. Scaled, fine gratings alias, and the panel says
  "Scaled: fine patterns alias; use 1:1".
- Rows: the texture is bottom-up (GL's order), so `ImGui::Image()` takes
  `uv0 = (0, 1)`, `uv1 = (1, 0)`.

### Input

The first version gives the session these inputs, while the panel has
the keyboard focus or the mouse is over the display:

| Input | To the session | How |
|---|---|---|
| A key | `YRSP_KIND_KEYBOARD`, the SDL scancode, to every collector, as a real run does | `main.cpp` passes each key event's scancode to `ui_preview_key()`; ImGui's keys follow the keyboard layout, and the player's choices are physical keys (scancodes). Esc gives the keyboard back to the designer and does not reach the session. |
| A choice button | The choice's own kind and control, as the scripted participant presses it (`pl_sim_choice()`) | A strip under the display has one button per choice of the running template. It covers choices that a keyboard cannot give: button box, XID and other device roles. |
| The mouse over the display | A `position2d` role's sample (`pl_sim_sample()`) | The cursor's place in the role's units, with the origin at the display's center, x right, y down (format D22), at the gfx's px per unit. No capture (decision 3). |
| A trigger code | Not an input: each code shows in an outputs list with its role and time (`on_code`) | |

**Not driven in the first version.** A `position2d_relative` role and a
`position2d` role with `allow_relative` (decision 3: with no capture, the
mouse's motion stops at the panel's edge); a role of type `position3d`,
`scalar` or `gaze`; units `workspace_cm` or `device`; a sync input (a
`line` role of kind `sync`). The panel lists each such role by name with
"not driven": the role stays at its origin, as with `--sim` and no
samples. Format 3's `signals` graph (experiment_format 3.8) and
`docs/signal_spec.md` are drafts and out.

**Sound** (decision 2): silent. A sound's onset is its scheduled time,
so a response window `from` a sound opens as planned. `--sim`'s virtual
null device starts a sound on a sample, so an onset can differ from it by
less than one sample period; the equality test measures this.

**Times.** An input's time is the onset of the first frame that the UI
frame steps after it arrives, less 1 ns, so it lands in that frame. The
resolution is the UI frame (16.7 ms at 60 Hz). A real run stamps keys
from the kernel. Input that arrives while the session is paused is
dropped.

**The panel says so**, in one line under the display: "Faithful in
logic, not in timing: a simulated 60 Hz display paced by this window;
input times are frame times; no sound."

### Time

**Two clocks.** The session's time is its frame grid. The UI's clock is
`SDL_GetTicksNS()`, which `main.cpp` gives to the core each frame (the
tests give their own).
- **Running.** At each UI frame, the preview steps the frames whose
  onsets are due by `speed x (real time since the start, less the time
  paused)`, at most `PV_MAX_STEPS` (16) a UI frame. Only the last of
  them draws (`draw` true). When more are due, the rest are dropped and
  the panel says "slower than real time". At a 60 Hz UI this covers a
  500 Hz display (8 steps) with room. Speed is 0.25, 0.5, 1 or 2.
- **Paused.** No steps. The texture keeps the last frame. The view's
  `animating` hook gives false, so the window waits for input
  (`dsg_redraw_*()`).
- **Step.** One frame, drawn. Then paused.
- **Restart.** A session job with the same seed and pin. "New seed"
  draws one.
- When the window is minimized or hidden, or the pane is closed, the
  preview pauses.

**Which trial.** A session runs from the start of the timeline, as
`--sim` does, so the variables and the table rows are the ones the walk
gives at the seed. The person picks the trial by a pin: a leaf node
(from the definition's timeline, listed with its template) and a row of
its table, or "any".
- With a pin, the session job fast-forwards on the check thread: it
  steps with `draw` false and the scripted participant (`live()` gives
  false) until the walk starts a trial of that leaf with that row. Then
  `live()` gives true for that trial and the ones after it, and the
  session goes to the UI thread there. Changing the pin is a restart.
- The fast forward stops when the walk leaves the leaf without that row
  ("row 5 was not drawn in this run of the leaf: seed 7"), or when a
  trial waits for input that the script cannot give (a signal role). The
  session is handed over where it stopped, with that note.
- The chart offers "Preview this template": the pin of the first leaf
  that runs it, row any.
- Session fields: the check's stand-ins (integer 1, text `x`), shown in a
  table that the person can edit.
- Rejected: a player call that starts the walk at a node. The
  ancestors' rows, loops and methods give the leaf its variables, and a
  second walk would have to rebuild them; the fast forward uses the
  player's own walk.

**Where the trial is.** `pl_sim_where_now()` gives the trial, the leaf,
the row, the template, the state (version 2) or the phase (version 1),
the frame and the time. The panel shows them in one line. The chart
draws a ring around the running state when it shows the running
template; with "Follow" on, it changes to the running template.

### Edits while the preview runs

The preview loads only text that the check accepted (`DSG_CHECK_OK`),
with the check pack of that check. The check's results come from the
check thread one or more frames after the edit.

| Change | What the preview does | Why |
|---|---|---|
| The definition or a table changes, and the check accepts it | Posts a session job when both are true: no change for `PV_SETTLE_MS` (300 ms, decision 4), and a check result with `DSG_CHECK_OK` for the document's current revision has come. Same seed and pin, running or paused as before. The old session goes on until the new one is adopted. A result for an older revision posts nothing. | A restart per keystroke would reset the trial at each character, and costs a load each time. |
| The text does not parse, or the check refuses it | Keeps the last good session, running or paused as it was. A banner: "Shows the definition as of HH:MM:SS; the text has N errors." | Typing passes through texts that do not load. Stopping would lose the trial being looked at. |
| The check pack rebuilds (a text stimulus's texts, a table) | As an accepted change. The old session keeps its reference to the old pack until it closes. | The session points into the pack's bytes. |
| A change while a session job runs or waits | The newest job wins; a session that is ready and stale is dropped ("The check thread and the session's load"). | |
| The session refuses at run time (`pl_sim_frame()` < 0) | Stops; the image stays. The refusal goes to the Issues panel as a preview issue, linked by its object as a tier 2 refusal is. The next accepted change restarts. | These are the checks that `pl_check()` cannot reach (player.md, "What it does not do"): the preview finds them. |
| The session ends | "Session over: N trials", and the data set's folder. Restart. | |

**Rejected: hot reload** in the first version (decision 4; rig_spec 6: an
update applies at once in preview and keeps the session's state). It
needs the runner protocol, which is out of the first version. A restart
at the pin shows the same trial; a staircase's state and the trials
before the pin start again.

### Redraw and cost

Each UI frame stays within 16 ms. On the UI thread the preview adds a
step's work while it runs, and the attach's budget while a session is
adopted. The load, the fast forward, the data set's writes and the old
session's close are on other threads.

Part D measured the UI thread's frames on the RX 6600 and on llvmpipe
instead of the UHD 770 ("Part D as built"); part A measured the core's
side headless ("Part A as built").

**Measure** (Linux desktop, Intel Core i5-14500, Intel UHD 770 with Mesa
iris, gcc 12.2, Release, pinned to CPU 2, one run at a time on a quiet
machine, the load average stated; medians, p99 and max of 1,000 frames,
two runs):

| What | Thread | Why | Knob if over budget |
|---|---|---|---|
| `pl_sim_frame(draw false)` on each example (rdk, stroop, afc2, oddball, sound, visuomotor, bimanual) | UI | Up to 16 of them run in one UI frame | `PV_MAX_STEPS` |
| `pl_sim_frame(draw true)`, CPU, at 800 x 600 and 1920 x 1080 | UI | The drawn frame | Display size |
| `pl_sim_attach()`: the whole, and its largest single call, with and without the program cache | UI | One call cannot be split: a large texture's upload is the floor | `PV_ATTACH_BUDGET_MS` |
| The whole UI frame with the chart and the preview running (bimanual), the frame of an adoption, and the frame interval | UI | The budget itself | |
| `pl_sim_load()` and the fast forward to a pin | check | How late a restart shows; the checks that wait behind it | Session jobs after checks |
| `pl_close()` of a session, with its writer's last writes | check | The same | |
| Idle while paused: frames drawn in 10 s with no input, and CPU | UI | Must be 0 frames after the settle frames | |

**Kept idle.** The view's `animating` hook is true only while the
session runs, the pane is open, and the window is not minimized. A step,
an attach in progress or an arriving result sets `ui_work_pending()`. A
job on the check thread draws no frame until its event arrives. Mouse
motion over a paused display is input like any other: it gives the
settle frames and no step.

**Allocation.** `pl_sim_frame()` allocates nothing (the player's test
with `--wrap`). The preview's input queue and its strings are fixed
arrays. The load allocates on the check thread; `pl_sim_attach()` makes
GL objects and frees the recorded copies.

### The designer's shared interfaces

The parts use these and do not change them; a part that needs a change
stops and reports it, as in step 4.

From part T, `designer/core/dsg.h` and a new `designer/core/worker.c`
(as built; each call takes the worker's handle, `dsg_worker*`, which
`ui.cpp` keeps):
- `dsg_worker_start(desc, err, cap)`, `dsg_worker_stop(w)`: the thread.
  `desc.wake(ctx)` runs on the thread when a result is ready (main.cpp:
  `SDL_PushEvent()`, `UiPlatform.wake`). `desc.session` is the seam to the
  session's load and close (`dsg_session_ops`). `desc.no_thread` runs the
  jobs in `dsg_worker_wait()` (part A's tests).
- `dsg_worker_check(w, project, text, n, doc, revision)`: a check job.
  `dsg_worker_forget(w)`: the checker forgets its pack before the next
  check. `dsg_worker_session(w, text, n, pack, revision, settings)`: a
  session job (`dsg_session_settings`: display, rate, seed, participant,
  rt, pin, fields, the data set's folder); the worker adds its own
  reference to the pack.
- `dsg_worker_take_check(w, &result)` and `dsg_worker_take_session(w,
  &result)`: on the UI thread, the newest result of each kind, with its
  revision (`dsg_check_result`: the report and the pack it ran against;
  `dsg_session_result`: a `dsg_session` or the load's error).
  `dsg_worker_session_queued(w)`: a session job waits or runs.
- `dsg_worker_close(w, session)`: a session to close on the thread.
- `dsg_worker_wait(w, timeout_ns)`: until every job posted so far ran (the
  tests; `ui_wait_checks()` in the UI tests). `dsg_worker_get_stats()`.
- `dsg_checker_pack()`, `dsg_pack_retain()`, `dsg_pack_release()`
  (atomic), `dsg_pack_bytes()`, `dsg_pack_live()`.

`designer/core/preview.h` (C, no SDL; `preview.c`, part A):
- `dsg_preview_new(gl_caps)`, `dsg_preview_free()`.
- `dsg_preview_checked(pv, report, revision, pack, now_ns)`: a check
  result arrived; the preview decides whether to post a session job.
  `dsg_preview_adopt(pv, session, revision, now_ns)`: a session arrived.
  `dsg_preview_tick(pv, now_ns)`: once a UI frame; attaches, steps,
  posts after the settle. `dsg_preview_animating()`,
  `dsg_preview_work()`.
- Controls: `dsg_preview_run()`, `_pause()`, `_step()`, `_restart()`,
  `_set()` (display, rate, speed, seed, participant, pin, fields).
- Input: `dsg_preview_key(scancode, down)`, `_choice(i, down)`,
  `_pointer(x_px, y_px, inside)` in participant px.
- State: `dsg_preview_where()`, `dsg_preview_status()` (none, waiting
  for a session, attaching, running, paused, ended, failed, stale), the
  preview issue, the outputs list, the data set's folder, the roles not
  driven, `dsg_preview_texture()`.

`designer/core/dsg.h` (S): `DSG_FROM_PREVIEW`; `dsg_remove_tree()` in
`platform.c` (the old `run-*` folders, called on the check thread).

`designer/app/views.h` (S): a third slot, `UI_SLOT_AUX` (decision 5): a
pane at the right of the main area, shown or hidden by a toolbar button,
beside whichever main view shows, so the chart and the preview show at
once. `ui.h` (S): `UiPlatform.gl_caps` (NULL in the headless tests,
where the session draws on gfx's null backend and the pane shows a gray
box), `ui_preview_key()`, and `ui_preview_where()` for the chart.

The view file's part `preview`: `{"open": bool, "display": [w, h],
"hz": n, "speed": s, "seed": n, "participant": "...", "pin": {"leaf":
PATH, "row": n}, "fields": {...}, "scale": "fit"}`.

### Who owns which file

| Files | Owner |
|---|---|
| `include/ysp/gfx.h`, `docs/gfx.md`, its README row and `docs/status.md` entry, its test and compile checks, `tests/mutate/gfx.toml`; ysp-vis's `pyproject.toml` and `YV_VERSION` | P3 |
| `player/player.h`, `sim.c` (new), `run.c`, `lower.c`, `v2.c`, `host.c`, `player/CMakeLists.txt`; `tests/player/sim_test.c` (new); `tests/mutate/player_sim.toml` (new) and the entries of `player_run`, `player_lower`, `player_v2`, `player_host`; `docs/player.md` ("Reference: the stepped session"); `docs/status.md` | P4 |
| `designer/core/worker.c` (new), `check.c`, the worker's and the check's parts of `dsg.h`, the check's call in `ui.cpp`, the job event and the first gfx open in `main.cpp`; `tests/designer/worker_test.c` (new) | T (the check thread) |
| `designer/core/preview.h`, the rest of `dsg.h`, `platform.c` (`dsg_remove_tree()`), `views.h`, `ui.h`, `ui.cpp` (the AUX slot, the preview issue in the Issues panel), `main.cpp` (the key hook, framebuffer 0 before the clear), `views.cpp`, `designer/CMakeLists.txt`; stubs of parts A, B and C. As built, also `designer/core/preview_part.c` (the view file's part), `tests/designer/platform_test.c`, `te_common.h`, `te_test.cpp`, `ui_test.cpp` parts 1 to 8 | Shared (S): frozen during step 5, after T |
| `designer/core/preview.c`; `tests/designer/preview_test.c`, `preview_bench.c` | A (core) |
| `designer/app/view_preview.cpp`; `tests/designer/te_preview.cpp`; `test_designer_ui`'s preview part (as built: part 9 and later of `ui_test.cpp`) | B (pane) |
| `designer/app/view_chart.cpp` (the ring, Follow, "Preview this template"); `tests/designer/te_chart.cpp` (added tests) | C (chart) |
| `tests/designer/preview_gl_test.cpp` (new); the measurements | D |
| `docs/designer.md`: a subsection each under "Decisions of step 5"; `docs/status.md`: an entry each | Each part its own |

### Tests

Every part keeps the designer suite passing in ctest from the build tree
with `DISPLAY` unset, with `YSP_DESIGNER_TEST_ENGINE` off and on, and the
player's tests and mutation lists. Each new check has a hand mutant that
fails it. The threaded tests also run under ThreadSanitizer (gcc 12,
`setarch -R`).

| Part | Tests |
|---|---|
| P3 | `test_gfx` part "v0.23" (EGL, llvmpipe, headless). Output texture: `ygfx_read_output()` gives the same bytes as the framebuffer 0 path for the same frames (gratings, text runs, a calibration); the native name is GL_RGBA8 at the size; framebuffer 0 is bound after `end()`; after foreign GL calls and `ygfx_reset_state()`, the same bytes. Deferred open: opened on one thread, attached on another (the context's), the same bytes as a direct open; the handles unchanged; attach in steps of a small budget gives the same; a draw before attach refused; a texture larger than the caps refused at the open. Mutants: write framebuffer 0; leave the texture's framebuffer bound; replay out of order; free a copy before its call. |
| P4 | `test_player_sim`: (1) at 800 x 600 with the scripted participant, the same `trials.csv`, `events.csv` and streams as `ysp_player --sim` (run in the same test on the virtual clock, which is then ended) for each example pack, and for visuomotor and bimanual with the samples through `pl_sim_sample()`; `frames.csv` the same but for its measured phase columns; a sound's onset within the tolerance the test measures. (2) No allocation in `pl_sim_frame()` (`--wrap`, Linux). (3) Load and fast forward on one thread, attach and frames on another; no clock change (`yrt_is_virtual()` false). (4) Two sessions stepped in turn, with a `pl_check()` on a third thread at the same time, give the data of each alone. (5) `pl_sim_key()` at a time gives that response and `rt = t - onset`; a choice; a sample that ends a reach. (6) The states of `pl_sim_where_now()` follow `--sim`'s state rows (bimanual). (7) A refusal at run time gives its object. (8) `live()`: the scripted trials and the person's trials. (9) A stopped session's `session.json` says so. Mutants in `player_sim.toml`. |
| T | `test_designer_worker`: newest-wins for each kind (ten texts in a burst give one result per check, the last for the last text); a check before a waiting session job; a result marked with its revision; a session dropped by the thread when a newer job waits; a pack valid while a session holds it after a rebuild (ASan); a session closed on the thread after it was detached on another. `test_designer_ui` and the `te_*` tests pass with the check one frame late. Built: "Part T as built". |
| A | `test_designer_preview` (headless, null gfx, a test clock, the worker run inline): the steps a UI frame for given times (cap, speed, pause, step); a session job posted after the settle and only for the current revision; a ready and stale session dropped; the attach's budget over frames, with the old session running until it ends; last good on a refusal; a run-time refusal as an issue; the pin's two stops; the cursor to role units for px, cm and deg; the `run-*` folders, one left; idle frames 0 while paused. `bench_designer_preview` (not on Windows): the numbers of "Redraw and cost". |
| B | `test_designer_ui`: the pane draws with `gl_caps` NULL; `animating` while running, not while paused; the banner texts; the roles not driven listed. `te_preview.cpp`: Space through the key hook gives a row with the response; the Pause button; an edit in the panels restarts the preview after the settle; a choice button; a text that does not parse shows the banner and keeps the session. |
| C | `te_chart.cpp`: the ring on the running state; Follow changes the template; "Preview this template" sets the pin. |
| D | `test_designer_preview_gl` (EGL, llvmpipe, headless; not in the default ctest when EGL is missing): a session loaded on the worker and attached on the context; one ImGui frame with the preview at 1:1 and `glReadPixels()` of framebuffer 0 gives the texture's bytes (alpha 255, no tint); the stroop word at its place; gfx's bytes the same with and without an ImGui frame before. Then the measurements, one at a time. |

### Order of merges

1. Wave 1, in parallel: P3, P4 and T on branches from `main`
   (`gfx-output-texture`, `player-sim`, `designer-check-thread`). P4
   builds against P3's branch for the deferred open and works with the
   null gfx until P3 merges. T needs no step 5 code; its session jobs
   use P4's `pl_sim_load()` and start as a stub.
2. Into `designer-step5`: P3, then P4, then T, then S. Each merge runs
   the player's, gfx's and the designer's suites and the mutation lists.
3. Wave 2, in parallel, each on a branch from `designer-step5`: A, B, C.
   Merge A first (B calls its API), then C, then B.
4. Wave 3: D, then the integration: the measurements in "Redraw and
   cost", "Decisions of step 5", and the README and status rows.
5. `designer-step5` into `main` only when the user says so.

### Risks

- **The frame grid against `--sim`.** If a stepped session does not give
  `--sim`'s data, the preview is not faithful in logic. The equality
  test is the check, and it comes first in P4.
- **The deferred open is the largest new piece** (P3). It must replay
  every backend call in order with the same handles. Its test compares
  bytes with a direct open (built: 0 frames differ, handles equal;
  gfx.md, "v0.23.0").
- **One attach call cannot be split.** P3 splits uploads over 4 MB into
  bands. A texture's storage, one compile and one first draw stay single
  calls. Measured by P3: up to 4.3 ms on the RX 6600 with Mesa's cache warm,
  17 to 22 ms on llvmpipe (a 4096 x 4096 texture's storage), and 31 ms for a
  cold text program on the RX 6600. Part D measures the designer's own frames.
- **Threads.** Part T added the check thread. P4 moved the player's
  statics into each load, P3 made the NOISE table's first build take a
  lock, and T builds it before its thread. ThreadSanitizer is the check
  for the races not known.
- **A restart shows late** when the check thread is busy: a session job
  waits behind a check (up to 201 ms on the generated project's text
  keystroke). The old session runs meanwhile.
- **The fast forward can stall** on a trial that waits for signal input
  the script does not give. It hands over there and says so.
- **GL state.** A later change to ImGui's backend or the designer's own
  GL can break the assumptions in the table. The GL test checks the
  bytes after an ImGui frame.
- **Disk.** Each session writes a data set (`frames.csv` grows a row a
  frame). Only the current run and the one before it are kept.
- **The web build** (step 7) is not tried: WebGL2 headless gfx worked in
  the probe; the player library in WebAssembly, its writer thread and
  part T's thread are not built.
- **Memory.** Up to three 1.6 MB players (the check's, the shown
  session's, one loading), each session with its 1.5 MB seed log, and a
  deferred gfx's copies of its texels until the attach.

### Not done in the plan

- At the plan, nothing was built. Wave 1 is built ("Wave 1
  integrated"). The check's cost is measured (branch
  `designer-check-cost`), and P3 measured the deferred open and the
  attach (gfx.md, "v0.23.0"); the session's step costs and the designer's
  own frames are not measured (part D).
- Windows and macOS are not considered beyond what CI builds.

### Wave 1 integrated (2026-11-20)

Branch `step5-int`, from `integrate-1120`: `step5-p3-gfx` (P3,
ysp/gfx.h 0.23.0), `step5-p4-player` (P4, player 0.16.0) and
`step5-t-worker` (T, the check thread), merged in that order with
`--no-ff`. Git merged every file itself: the parts' sections of this
file, `docs/status.md`, the README and the CMake files did not overlap.

**P4 on P3's API** (player 0.17.0; [player.md](player.md#the-gfx-on-a-gl-context)).
P4's GL path had been written against the plan's names behind
`PL_GFX_DEFER` and never compiled against 0.23.0.
- `pl_sim_desc` gains `share`, passed to `ygfx_desc.share`. Part A sets it,
  and `gl_caps`, from `main.cpp`'s first gfx.
- `pl_gfx_warm()` (the text programs and each glyph set's draw state)
  runs at the load, so the deferred gfx records it and the attach makes
  it. `pl_sim_attach()` no longer calls `ygfx_prime()` after the attach:
  the attach primes without the read's wait.
- The guard is gone: the player needs gfx.h 0.23.0, and a desc with
  `gl_caps` is never refused for the header's version.
- `pl_sim_attach(p, 0)` is "all", as documented: gfx's own 0 is one step.
- Found by the GL test: `pl_sim_texture()` said false when the texture was
  there (`YGFX_OK` is 0).

**Tests** (Linux desktop, i5-14500, Debian 12, gcc 12.2):
- `test_player_sim` case (10): a session loaded on a second thread with
  the caps and `share` of a gfx open on an EGL context (llvmpipe, no
  window), attached and drawn on the context's thread, and read through
  `pl_sim_texture()`, equals the same session on a direct GL gfx on 360 of
  360 drawn frames, for stroop and oddball. The test leaves foreign GL
  state before each drawn frame, as ImGui may. No program is made in a
  frame, and no heap call (53,923 frames).
- ctest from the Release build tree, examples and the Test Engine on,
  `DISPLAY` unset: 140 of 140.
- Sanitizers, built and run under `setarch -R`: ASan and UBSan: `test_player_sim` (677 checks), the nine designer tests and `test_gfx` with llvmpipe (`YGFX_TEST_DEVICES=mesa`) give no ASan or UBSan report; LeakSanitizer reports only Mesa's known 336 bytes in 6 blocks in `test_gfx`, each a `calloc()` from the unloaded llvmpipe, which no suppression can name (gfx.md, "Sanitizers"). ThreadSanitizer: `test_player_sim`, the nine designer tests and ten more runs of `test_designer_worker` give no report; `test_gfx` with llvmpipe gave 34, every one in Mesa's own mutex and condition variable teardown in `swrast_dri.so`, and with the one suppression `race:swrast_dri.so` none.
- Mutants: 68 of 68 killed on the merged tree: `player_sim.toml` 20 (4 new: s-17 to s-20), `player_run.toml` rs-01 to rs-11, `player_v2.toml` vs-01 to vs-05, `player_lower.toml` 14 (ls-01 to ls-10, ls-09 and ls-10 new; ck-07, ck-09, ck-10, ck-12), `player_host.toml` h-04 (it needs `YPL_BIN`, the build folder), and `gfx.toml` ot-01 to ot-08, df-01 to df-06, sh-01 to sh-03; `--check` of every list, no anchor missing.
- ysp-vis 0.20.0.6 in a scratch virtual environment (CPython 3.12):
  82 passed, 14 skipped (the window tests).
- A `-Wconversion` build of the designer, the player and their tests: no
  warning in `designer/` or in the code that the three parts added; the
  warnings that remain (`v2.c` line 2374, `gfx.h` lines 7319, 12154 to
  12185, and the vendored headers) are older and only sign conversions.
- The MSVC and clang warnings that CI treats as errors (C4701, C4244,
  `-Wsometimes-uninitialized`) were looked for by reading the merged
  diff, because no MSVC or clang is here: none found. MSVC and macOS were
  not compiled.

**Not done.** Parts S, A, B, C and D. T's ten hand mutants are not in a
list and were not run again.

### Part S as built (2026-11-21)

Branch `step5-s`, from `step5-int` (`82c0561`). Part S gives parts A, B
and C frozen interfaces and stubs, as the shared step of step 4 did. With
the stubs the designer runs as before step 5: the preview never posts a
session job, and the pane says "Preview not built."

**The frozen interfaces.** After S, parts A, B and C change none of these
files. A part that needs a change stops and reports it.

| File | What it gives | Who calls it |
|---|---|---|
| `designer/core/dsg.h` (0.3.0) | `DSG_FROM_PREVIEW`; `dsg_remove_tree(root, rel, err, cap)` | A (the old `run-*` folders, on the check thread) |
| `designer/core/preview.h` | The whole preview API (below), `dsg_preview_gl`, `dsg_preview_settings` | A implements it in `preview.c`; B and C call it; `ui.cpp` drives it |
| `designer/core/preview_part.c` | `dsg_preview_settings_default()`, `dsg_preview_part_read()`, `dsg_preview_part_write()`: the view file's part `preview` | `ui.cpp` only |
| `designer/app/views.h` | `UI_SLOT_AUX`; `UiView.open_file`; `ui_preview()`, `ui_preview_gl()`, `ui_now_ns()`, `ui_preview_where()`, `ui_aux_open()`, `ui_show_aux()`, `ui_open_file()` | B and C |
| `designer/app/ui.h` | `UiPlatform.now_ns`, `gl_caps`, `gl_share`, `gl_proc`, `gl_proc_ctx`; `ui_preview_key()`, `ui_preview_has_keyboard()`, `ui_set_window_shown()` | `main.cpp`, the tests |
| `tests/designer/te_common.h` | `te_register_preview()`, which `te_test.cpp` calls | B, in `te_preview.cpp` |

`preview.h`, by caller:
- `ui.cpp`: `dsg_preview_session_ops()`, `_new()`, `_free()`, `_open()`,
  `_close()`, `_checked()`, `_adopt()`, `_tick()`, `_animating()`,
  `_work()`, `_get()`, `_set()`, `_key()`, `_keyboard()`,
  `_set_keyboard()`, `_issue()`, `_where_now()`.
- The pane (B): the controls `dsg_preview_run()`, `_pause()`, `_step()`,
  `_restart()`, `_set()`, `_get()`; the input `_set_keyboard()`,
  `_choice()`, `_pointer()`; the state `_get_info()`, `_texture()`,
  `_outputs()`, `_output_at()`, `_undriven()`, `_undriven_role()`,
  `_choices()`, `_choice_label()`, `_leaves()`, `_leaf_at()`. The
  session fields' needs come from `ui_report()` (`DSG_FROM_RUN`,
  `PL_NEED_FIELD`). For 1:1, ImGui 1.93's
  `platform_io.DrawCallback_SetSamplerNearest` gives the nearest sampler
  with no GL call in the pane.
- The chart (C): `ui_preview_where()` for the ring and Follow,
  `dsg_preview_pin_template()` for "Preview this template", and
  `ui_show_aux(true)` to show the pane.
- The check thread (A's session ops): `dsg_worker_session_queued()`,
  `dsg_pack_bytes()`, `dsg_remove_tree()`, `dsg_locate_object()`, and the
  player's `pl_sim_*` calls, unchanged from player 0.17.0.

**Changes from the plan.**
- `dsg_preview_new(gl, worker, doc)`, not `(gl_caps)`. `pl_sim_desc` also
  takes `share`, `gl_proc` and `gl_ctx` (player 0.17.0), so the preview
  gets a `dsg_preview_gl` with all of them, and the cache. The preview
  posts jobs to the worker and reads the document's revision and text.
- `dsg_preview_open()` and `_close()` are new. The open gives the folder
  of the runs and the floor revision: a session of a lower revision is
  another project's.
- `dsg_preview_checked(pv, result, now)` takes the check result.
  `dsg_preview_adopt(pv, session_result, now)` takes the whole result,
  not a session and a revision, because a failed load is a preview issue.
- `ui.cpp` calls `dsg_preview_tick(pv, now, shown)` once a frame before
  the views draw, not the pane's draw hook. `shown` is the pane open and
  the window not minimized or hidden (`ui_set_window_shown()`). So the
  pane only shows and controls, and a hidden pane pauses the preview.
- Names: `dsg_preview_where_now()`, `_output_at()` and `_leaf_at()`. C
  does not allow a function and a type of the same name
  (`dsg_preview_where`).
- The state is one struct, `dsg_preview_info`. "Stale" is a flag, not a
  status, because a stale session still runs or pauses.
- The keyboard: the pane claims it (`dsg_preview_set_keyboard()`).
  `dsg_preview_key()` gives true when it takes a key, and Escape gives
  the keyboard back. `main.cpp` then keeps a taken key down and text
  input from ImGui. It gives every key up to ImGui, so a key that ImGui
  saw go down does not stay down there.
- `ui_preview_key(scancode, down, repeat)`: repeats are taken while the
  keyboard is claimed and do not reach the session.
- The clock: `UiPlatform.now_ns` (`SDL_GetTicksNS()`). With none, the UI
  uses ImGui's time, which the tests drive with `io.DeltaTime`. So
  `te_test.cpp` and `ui_test.cpp` need no clock of their own.
- `ui_preview_where()` is in `views.h`, not `ui.h`, because the chart
  includes `views.h`. Each call reads the place again.
- The view file's part is `ui.cpp`'s, not the pane's: `ui.cpp` reads it at
  an open into `dsg_preview_set()` and writes `dsg_preview_get()` with the
  view file. Every key is written, defaults too. It gains `rt_s` (the
  scripted participant's rt, which the session settings have). A value
  out of range reads as its default.
- `UiView.open_file` and `ui_open_file()` are new: the pane's button that
  opens `trials.csv` in the Tables view. No view has the hook yet, so
  `ui_open_file()` gives false. The Tables view's read-only file is not
  built.
- `dsg_preview_texture()` gives the GL name and the size as integers, so
  `preview.h` needs no ysp header.

**The session seam.** The real `pl_sim_load()` call goes in `preview.c`,
not in `worker.c`. `preview.h` declares `dsg_preview_session_ops(gl,
&ops)`, and `ui_init()` gives its result to `dsg_worker_desc.session`.
The stub gives a NULL `load`, which is T's stub: it loads nothing and
succeeds. Why:
- Part A then changes only `preview.c`. T's `worker.c` stays as T built
  and tested it, with no player call of the stepped session.
- The ops' `ctx` is the `dsg_preview_gl` that `ui.cpp` keeps. It is fixed
  before the thread starts and never written after, so the two threads
  share no mutable state through it. The settings, the pack and the text
  come in the `dsg_session`, as T designed.
- The load and the adoption share the session's private struct (the
  `pl_player`, the text copy, the hooks' state, the run folder) in one
  file. No other file needs its layout.

**The first gfx** (`main.cpp`). After the GL context is current and before
`ui_init()` starts the check thread, `main.cpp` opens one gfx on the
window's context: 64 x 64 into its own output texture, so it never
writes framebuffer 0. `ygfx_gl_caps_get()` of it goes to
`UiPlatform.gl_caps`, the gfx itself to `gl_share` (`pl_sim_desc.share`),
and `SDL_GL_GetProcAddress()` to `gl_proc`. It is the process's first use
of ysp/gfx.h, so it builds gfx's NOISE table on the UI thread before any
other thread exists; `dsg_worker_start()` builds it again, a no-op then,
so T's order still holds for the headless tests. It closes after
`ui_shutdown()`, with the context current. If it does not open, the
designer says so on stderr and the sessions draw on the null backend.
`main.cpp` also binds framebuffer 0 before its clear.

**The AUX pane.** A check box in the toolbar (the AUX view's label,
"Preview") shows the pane at the right of the main area, between the
main view and the side column, beside any main view. Its width is 45 % of
the room the side column leaves, at least 320 px. AUX views get `reveal`
while the pane shows. An AUX view's `animating` counts only while the
pane shows and the window is not hidden. A hidden pane drops the
keyboard claim. The Issues panel lists the preview issue after the
check's issues as "preview", and a click on it selects the value at its
place and puts the caret there.

**Tests** (Linux desktop, i5-14500, Debian 12, gcc 12.2):
- `test_designer_platform` (new, 47 checks): `dsg_remove_tree()` in a
  sandbox under the build tree's work folder (refuses a relative WORK).
  It refuses NULL, an empty path, `.`, `a/.`, `..`, `../sibling`,
  `a/../..`, `a//sub`, `a/`, an absolute path, `C:/outside`, a
  backslash, an empty root, a link out of the root as the name and on
  the way, and a link inside the root; after each, nothing is gone. It
  removes a file, a folder below another, and a tree with links in it
  (the links go, their targets stay), and leaves the root's other
  children, its links and its siblings. A missing name, a missing folder
  on the way and a file on the way remove nothing and are not errors.
  Each refusal, if it did not hold, would remove only inside the
  sandbox. The test cleans with `nftw(FTW_PHYS)`, not with the code
  under test. The link cases are POSIX only; Windows runs the rest in CI.
- `test_designer_ui` part 8 (now 66 checks): the pane hidden at first;
  shown, its child draws beside the text view, the side column and the
  chart; hidden, it does not. The settings and the pane round-trip
  through the view file, a seed above `INT64_MAX` too; a project with no
  part gets the defaults; a part with values out of range reads as the
  defaults; a row with no leaf is written as "any". The keyboard: no key
  is taken until the pane claims it; a claimed key, its repeat and its
  release are taken; Escape gives the keyboard back; hiding the pane
  drops the claim. No session: `ui_preview_where()` is NULL.
- `test_designer_te`: `designer/aux_pane`, a click on the check box
  shows the pane beside the text and the chart, and another hides it.
- ctest from the Release build tree, examples and the Test Engine on,
  `DISPLAY` unset: 141 of 141.
- ASan with UBSan (gcc 12, Debug, built and run under `setarch -R`,
  `halt_on_error`): the ten designer tests, no report.

**Hand mutants** (each reverted; each fails its test):

| Mutant | Fails |
|---|---|
| rm-01: an empty name accepted | `a//sub` gave 0 |
| rm-02: `..` accepted | `..` removed something |
| rm-03: `.` accepted | `.` removed something |
| rm-04: a link as the last name removed | `link_out` gave 0 |
| rm-05: a link on the way followed (no check, no `O_NOFOLLOW`) | `link_out/keep` gave 0 |
| rm-06: a link inside the tree followed | removing `a` failed at `esc` |
| rm-07: a drive or a backslash accepted | `C:/outside` gave 0 |
| rm-08: an empty root accepted | an empty root is refused |
| u-01: the view file without the part | the part holds the pane and the settings |
| u-02: an open reads no part | the other project: pane hidden, defaults |
| u-03: the AUX pane never drawn | shown, the pane draws |
| u-04: a hidden pane keeps the keyboard | hiding the pane drops the claim |
| u-05: repeats reach ImGui | a key, its repeat and its release |
| p-01: a seed above `INT64_MAX` dropped | the part holds the settings |
| p-02: any speed read | out of range: the defaults |
| p-03: a display of 0 px read | out of range: the defaults |
| p-04: the fields not written | the part holds the settings |
| p-05: the pin's row kept without a leaf | a row with no leaf is written as any |

**Not done.**
- The window was not opened: the first gfx, the key hook and the pane
  were not seen on a display. `main.cpp`'s changes are compiled only.
- MSVC, clang and macOS were not compiled. The Windows branch of
  `dsg_remove_tree()` (`GetFileAttributesW()`, name-surrogate reparse
  tags, `RemoveDirectoryW()`, `DeleteFileW()`) runs first in CI; its
  test has no link cases there, because a symbolic link needs a
  privilege on Windows. Locals that a call fills are initialized for
  C4701.
- The web build's `gl_proc` (`SDL_GL_GetProcAddress()` under
  emscripten) is not tried.
- No ThreadSanitizer run: S adds no thread code.

## Plan of step 6

Status: planned 2026-11-21 on branch `designer-step6-plan`, from
`step5-int2` at `f8908c7`. The user accepted decisions 1 to 17 on
2026-11-22 (17 was added that day). Nothing is built yet. This section answers, from the code at `f8908c7`, what Build and
Run need from the pack tool, the player, ysp/rigfile.h and the OS, and
gives the decisions, the shared interfaces, the parts, the tests, the
measurements and the order of merges.

The goal ("First version"):
- Build: the pack tool, linked in. Its messages show with the entry
  selected.
- Run, native only: the designer starts the command-line player with the
  built pack, a rig profile and an output folder, and shows its output.
- Rig profiles replace the preview's built-in simulated rig (step 5,
  decision 8).

Out of step 6, as "First version" says: the runner protocol, hot reload,
the operator console, binding by activity (devices_spec 11.3, which needs
the runner protocol), import as normalization, build jobs.

### Decisions for the user

Each one's evidence is in the subsection that it names. The numbers are
from "Measured", below.

1. **Where Build writes.** `PROJECT/build/NAME.ysppak`, NAME the project
   folder's name, through the tool's temporary file and rename. Not in
   `.designer/`: that folder is a cache that a person may delete, and the
   pack is the artifact that goes to the rig and whose ID a methods
   section cites. The Run view shows the path and the pack ID ("Build").
2. **Build saves first, and builds from the files.** Build is Save, then
   `ypt_build()` of the project's own description. It does not reuse the
   check pack: the check pack's manifest names
   `.designer/check/experiment.json` as the experiment's source, so its
   pack ID differs from a build of the project's files, and `ypak rebuild`
   of it fails against the project. Measured: for stroop and rdk the two
   packs differ only in that `path` line of the manifest; every entry is
   the same ("Build").
3. **Build has a thread of its own.** Not the check thread: a project
   with a video builds for seconds (ffmpeg's step), and the checks would
   wait behind it. Each Build is a full build: measured in a process of
   its own, the repository's projects take 0.7 to 7.1 ms median, the
   generated worst cases 36 and 146 ms, and a 10 s 1080p video 3.6 s
   ("Build").
4. **A Build can be canceled between entries.** The pack tool 0.12.0
   gets `ypt_options.cancel`, read before each resource and before an
   ffmpeg step; an ffmpeg encode that runs goes to its end. Without it a
   Quit during a video build waits for the build (part K). Recommended:
   yes.
5. **The player is the one beside the designer, of the same version.**
   CMake writes `ysp_player` into the designer's folder. At the first Run
   the designer runs `ysp_player --version` (new, player 0.18.0) and
   refuses another version than the player library it links: its check
   must be the run's check ("Run").
6. **The player writes its output to a file, not to a pipe.** Its
   standard output and standard error go to `DIR.log` beside the data set
   folder `DIR`. The designer reads the file. With a pipe, a designer that
   quits or ends would end the session: the player's next write to the
   closed pipe ends it with SIGPIPE. Measured: `ysp_player --sim` into a
   closed pipe ended with status 141 (SIGPIPE). A real run writes such
   lines during the session (the signal roles' "click the mouse" lines,
   v2.c) ("Run").
7. **Progress comes from `trials.csv`.** The player prints nothing
   between its start line and its end line. The designer reads the rows
   that the player's writer thread appends (trial count, the last row's
   response, rt and outcome). No player change, and nothing more on the
   player's frame thread ("Run").
8. **Stop asks; Kill forces.** Stop sends SIGTERM (Windows: WM_CLOSE to
   the player's windows). A real run's SDL 3.4.18 turns SIGTERM into
   `SDL_EVENT_QUIT`, which ysp/screen.h makes an abort: the data set
   closes, `session.json` gets `"aborted"`, exit code 3. Kill (SIGKILL,
   `TerminateProcess()`) comes only after Stop, with a warning: the
   display's mode and gamma ramp may stay changed. The player 0.18.0 also
   stops `--sim` on SIGTERM as an operator stop: measured, `--sim` ended
   by SIGTERM with status -15 and a `session.json` without `"aborted"`
   ("Stop").
9. **While a real run goes, the designer steps back.** The preview stops
   and closes its session, Build and Run are disabled, edits and checks go
   on. The designer draws on input only, and at most 2 times a second for
   the Run view's progress (knob `RUN_REDRAW_HZ`). It starts the player
   with SCHED_OTHER set explicitly. Whether a designer frame costs the
   player a frame is not measured: that needs the real player on the
   display ("The designer's own frames during a run").
10. **Run can be simulated.** The Run view has a "Simulated (--sim)"
    check box: the same spawn path, the whole session on the virtual
    clock, its data set in seconds. The designer passes `--max-seconds`
    (default 600) and each relative signal role's `--sim-gain`, because a
    version 2 definition with no samples runs to the player's 48 h budget:
    measured, bimanual 50 s and 872 MB, visuomotor 45 s and 871 MB, exit
    1 ("Run").
11. **The output folder.** `PROJECT/data/PARTICIPANT_YYYYMMDD-HHMMSS`
    (the `participant` field, UTC), editable before the run; the player
    refuses a folder that exists. The Run view checks first and names the
    folder.
12. **Rig profiles: choose and view; no edit in step 6.** A combo lists
    the profiles of the user's rig folder (`yrig_dir()`: `profile.json`
    and `NAME.json`), "No profile (keyboard only)" and, for the preview
    only, "Built-in simulated rig". A view shows the chosen profile:
    roles and their devices, view, signals, the calibration file, and each
    check note of `yrig_load()` (a loopback file missing or changed). No
    edit: the profile is written by tools (the loopback runs, a binder by
    activity), a tier must come from a checked loopback, and binding by
    activity needs the runner protocol (devices_spec 11.3). A person edits
    the view or signals in a text editor; the designer loads the file
    again when its size or time changes ("Rig profiles").
13. **The check takes the chosen profile.** `pl_check_desc` gains `rig`
    (player 0.18.0, a layout change). With a profile, the loader's role
    and signal checks against a rig run (today `r->report->skipped`), and
    a tier 3 need that the profile meets is not listed: a bound device, a
    view, a calibration, a signal's adapter, a role's tier from a checked
    loopback. So a refusal that a real start would give shows while the
    person edits, not as exit code 1 after a Build. Measured: a
    profile's load is 176 us; the designer loads it once per choice or
    file change, not per check ("Rig profiles").
14. **The preview takes the chosen profile** (step 5, decision 8):
    `pl_sim_desc.rig` and `.cal` from it, so deg and cm units, the
    calibration and the signals' adapters are the rig's. Devices never
    open in the preview (`stand_in`). The display's size and rate stay
    the pane's choice: a profile has neither. "Built-in simulated rig"
    stays as a choice and is the default when no profile exists.
15. **Placement.** A view "Run" in `UI_SLOT_AUX`, with its own toolbar
    check box, stacked under the preview when both show (the slot stacks
    its views already). The toolbar gets Build, Run and Stop, so they
    work with the pane hidden. Build's messages go into the Issues panel
    (`DSG_FROM_BUILD`); a click selects the entry, as a check's issue does
    ("The views").
16. **Web.** Run is not built for WebAssembly. Build in the browser (a
    download of the pack) goes to step 7 with the rest of the web build;
    step 6 keeps the web build compiling with stubs.
17. **An entry cache for derived media** (added 2026-11-22). The check
    pack re-encodes a video at each text change: 3.6 s with no check
    result for the 1080p case ("The check pack and a video"). The pack
    tool gets an entry cache (part K): a derived entry is reused when its
    key (the source's bytes, the derivation's options and the tool's
    version) matches. The designer keeps it in `.designer/cache/entries`,
    shared by the check pack and Build (part B). Part K measures the key's
    cost (hash of the source, or size and time first) and the hit's cost
    on the 1080p case, and the pack bytes with and without the cache must
    be equal.

### What the code allows today

| Need | What the code does now | Where |
|---|---|---|
| Build a pack in process | `ypt_build()` and `ypt_build_text()`; messages to `FILE*` (`ypt_options.log`, `.warn`) and one error line; no cancel; a failed build leaves no pack (temporary file and rename). The ffmpeg step blocks in `run_program()` until ffmpeg ends | `pack/ysp/pack_tool.h`, `pack/pack_tool.c` |
| The check pack | Built on the check thread from the project's description with the experiment's source replaced by `.designer/check/experiment.json` and each dirty table by a copy; default options; kept in memory, shared by reference count | `designer/core/check.c` `build()` |
| Start a process | Only the pack tool's `run_program()`: `posix_spawnp()` or `CreateProcessW()`, output into a buffer, a blocking wait, no kill, every inheritable handle inherited on Windows. The designer has no process code | `pack/pack_tool.c` |
| The player's command line | `PACK --out DIR [--sim] [--seed N] [--field NAME=VALUE]... [--rig FILE|NAME] [--cal FILE] [--windowed] [--allow-below-tier ROLE[,ROLE]] [--max-trials N]`; `--sim` adds `--participant`, `--rt`, `--max-seconds`, `--samples`, `--sim-gain`, `--sim-hz`. No `--version` | `player/main.c` |
| Exit codes | 0; 1 refused or failed (one line on stderr); 2 a bad argument; 3 the operator stopped it (`"aborted"`); 99 the panic watchdog (`panic.json`). A signal otherwise | `player/main.c`, player.md |
| Output | A real run prints `running; Shift+Esc stops the session` at its start, the signal roles' click lines during the session, and one end line; `--sim` prints only the end line | `run.c` 2915, `v2.c` 3867 to 3924, `main.c` |
| The rig | `--rig NAME` reads `NAME.json` from `yrig_dir()`; with no `--rig` a real run takes `profile.json` there; `--sim` takes no profile unless named. `pl_sim_desc.rig` exists; `pl_check_desc` has no rig | `player/lower.c` `pl_rig_load()` |
| Stop | Shift+Esc; the window's close; SDL's quit. A real run opens SDL video, and SDL 3.4.18 turns SIGTERM into `SDL_EVENT_QUIT` (measured, `sdlterm`); `--sim` has no handler | `ysp/screen.h` the abort watch |
| The UI | Toolbar (New, Open, Save, Undo, Redo, the AUX check box, the path); the main view; the AUX pane (stacks its views; the check box shows the first); the side panels; the Issues panel. `ui_report()` is the check's report, which the tables view's `issue_sel` maps to cells | `designer/app/ui.cpp`, `view_tables.cpp` |

### Build

**What Build does.** On the UI thread: Save (the definition and every
dirty table; as Ctrl+S), then a build job with the project's paths and
the document's revision. On the build thread: `ypt_build(desc_path,
out_path, &o)` with `o.log` and `o.warn` on `tmpfile()` (C standard, also
in WebAssembly), then the pack ID (ysp/pack.h's reader), then the result: the
status, the pack's path, size and ID, the time, the revision, the issues
(`DSG_FROM_BUILD`: the error line and each `warning:` line), and the
stamp of the sources (each source's size and modification time, from the
description's resources). The result goes to the UI through the event that
the check thread uses (`UiPlatform.wake`).

**Its messages, with the entry selected.** The pack tool's lines name the
place in the forms that the check already reads: "line N, column M" of the
definition (`place_in_message()` in `check.c`), and a table's entry, row
and column (`dsg_csv_place()`, which the tables view's `issue_sel` reads).
`ui_report()` becomes the check's issues followed by the last Build's,
so the Issues panel, its click and the tables view's marks take Build's
issues with no new code in the views. A message that names another
resource (a font, an image, a video) shows its name and source path; the
designer has no editor for those.

**What the check pack gives, and what it does not.** The check pack is
built from the editor's text and the dirty tables, before Save, with the
same tool and options. Its entries equal Build's when the text and the
tables are saved; its manifest does not (decision 2). So Build reuses the
check's code path (the description, the tool, the place of a message),
not its bytes.

**Full builds.** The pack tool has no entry cache: each build derives
every entry again. A full build of each repository project costs 0.7 to
7.1 ms in its own process ("Measured"), the generated worst case 36 and
146 ms, so no cache pays there. The 1080p video case costs 3.6 s, nearly
all ffmpeg's encode on 2 threads. Run builds only when the pack is stale:
the document changed since the last Build, or a source's size or time
differs from Build's stamp.

**The check pack and a video** (found by this plan, not new). The check
pack is built from the whole description, so a project with a video
re-encodes the video at each check pack build: at the first check and at
each keystroke that changes a text. On the check thread that is seconds
(3.6 s for the 1080p case here) in which no check result comes. The
fix is an entry cache keyed by the source's hash and the derivation
(decision 17, parts K and B). A check pack that leaves out the video
entries does not serve: the preview draws the videos from it.

**The build thread.** A second `yrt_pump`, as part T's: SCHED_OTHER with
explicit attributes, never elevated, blocks when idle. One job at a time;
a Build posted while one runs waits, and a newer one replaces it (newest
wins). Two pack tool builds can then run at once, the check pack's and
Build's. Part T's table of process globals says the pack tool's statics
are benign (each write stores the same value). Skribidi's font collection
counter (`skb_font_collection.c` line 328, `++id`) is a race between two
builds: two collections can get one id, which only keys that collection's
layout cache. Part B runs two builds at once under ThreadSanitizer; if it
reports the counter, a Skribidi patch makes it atomic (pack/layout patch
release).

### Run

**The command line.** The designer composes the arguments from the Run
view's settings (part R, `dsg_run_args()`, a pure function):

```
ysp_player PACK --out DIR [--rig NAME|FILE] [--seed N] [--field NAME=VALUE]...
           [--allow-below-tier ROLE[,ROLE]] [--windowed]
           [--sim --max-seconds S [--sim-gain ROLE=UNITS]... [--participant SPEC]]
```

- PACK: decision 1's file. Run builds first when it is stale.
- `--rig`: the chosen profile's name (`--rig NAME`), or no `--rig` for "No
  profile". The player reads the profile itself; the designer reads it
  only for the check and the preview.
- `--field`: the session fields (the preview's fields are the defaults;
  each required field must have a value, as the check's
  `PL_NEED_FIELD` says).
- `--allow-below-tier`: the roles that the person ticks, each with the
  check's `PL_NEED_ROLE_TIER` text beside it; `session.json` records them.
- `--windowed`: "Window (development)", off by default; a window is tier 2
  at best.
- `--seed`: empty by default (the player draws one and records it).
- The working folder is the project's folder.

**The process.** `dsg_proc_start()` (part X, `platform.c`):
- POSIX: `posix_spawn()`, not `fork()`. Its file actions put `/dev/null`
  on standard input and the log file (decision 6, opened `O_CREAT|O_EXCL`)
  on standard output and error, and close every other descriptor
  (`posix_spawn_file_actions_addclosefrom_np()`, glibc 2.34 and later;
  macOS: `POSIX_SPAWN_CLOEXEC_DEFAULT`). Its attributes set SCHED_OTHER
  (`POSIX_SPAWN_SETSCHEDULER`) and the default action of SIGTERM, SIGINT
  and SIGPIPE (`POSIX_SPAWN_SETSIGDEF`), with an empty signal mask. So the
  player never inherits a designer thread's policy (rt.md, "Inherited
  policy": inherited SCHED_FIFO froze this machine), a descriptor of the
  designer's, or an ignored signal.
- Windows: `CreateProcessW()` with a quoted UTF-16 command line (the
  pack tool's quoting), `NORMAL_PRIORITY_CLASS`, an explicit list of the
  three handles to inherit (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`; the pack
  tool's `run_program()` inherits every inheritable handle), and then
  `AllowSetForegroundWindow(pid)`: the designer holds the foreground when
  the person clicks Run, and the player needs it (rig_spec 6, "The runner
  gives the player the foreground").
- macOS: as POSIX. The player raises its own window (`desc.foreground`).
- A watch thread per run (SCHED_OTHER, never elevated) waits for the
  exit (`waitpid()` with `WNOHANG` every 250 ms; Windows
  `WaitForSingleObject()` with 250 ms) and, at each wait, looks at the
  log's and `trials.csv`'s sizes; when one grew or the process ended, it
  wakes the UI. So the UI reads at most 4 times a second and blocks on
  nothing.
- Measured: `posix_spawn()` holds the caller 42 us median (37 us with
  1 GB of the caller's memory touched); `fork()` 17 us, but 8.5 ms median
  and 29.5 ms max with the 1 GB. The player's start to its exit with a
  usage error is 0.3 ms. So the UI thread may call `posix_spawn()`
  whatever the designer's size, and `fork()` is rejected ("Measured").

**What the Run view shows.** The state (building, starting, running,
stopping, ended), the elapsed time, the trials written and the last
trial's response, rt and outcome (from `trials.csv`), the log's last
lines, and at the end the exit code's meaning:

| Exit | The Run view says | Then |
|---|---|---|
| 0 | Done: N trials in DIR | "Open trials.csv" (read-only, as the preview's) |
| 1 | Refused or failed: the log's last line | The message as a Run issue in the Issues panel; the check with the same profile should have said it first (decision 13) |
| 2 | A bad argument: the designer's fault | The command line, for a report |
| 3 | Stopped: N trials written; trial K has no row | `session.json`'s `aborted` |
| 99 | The panic watchdog ended it; the display was put back | `panic.json`'s trial |
| a signal | Ended by signal S | X11: "if the display's mode or ramp is wrong: `xrandr --output OUT --mode 1920x1080 --rate 60 --gamma 1:1:1`"; the data set is not closed |

**Quit during a run.** The designer asks: Stop the run, Leave it running,
or Cancel. "Leave it running" frees the watch thread and leaves the
player alone: its output goes to its log file, so the designer's end does
not touch it (decision 6).

### Stop

- POSIX: `kill(pid, SIGTERM)`. A real run has SDL's video subsystem open;
  SDL 3.4.18 turns SIGTERM into `SDL_EVENT_QUIT` (measured with a test
  program, `SDL_Init(SDL_INIT_EVENTS)`, no window), ysp/screen.h's watch
  makes that `YSCR_ABORT_QUIT`, and `pl_run()` stops as for Shift+Esc:
  exit 3, `"aborted"` with reason `quit`. Not run with the real player
  (no display was used for this plan).
- Windows: `PostMessageW(WM_CLOSE)` to each top-level window of the
  player's process (`EnumWindows()`), which SDL gives as
  `SDL_EVENT_WINDOW_CLOSE_REQUESTED`: reason `close`, exit 3. A console
  control event would need a shared console.
- `--sim`: no SDL video, so SIGTERM ends the process. Measured: status -15
  after 1 s of the generated project's 12.9 s session; `session.json` was
  the one written at the start. Part P adds a handler (an atomic flag
  that the frame loop reads) so that `--sim` stops as a real run does:
  exit 3, reason `request`.
- Kill: shown 5 s after Stop when the player has not ended. SIGKILL and
  `TerminateProcess()` skip the player's restore of the display's mode
  and ramp (`atexit()` does not run), so the button says so.

### The designer's own frames during a run

The player wants the display and, on Linux, SCHED_FIFO for its frame
thread under an `rtprio` grant (rt.md, "Linux real-time priority"). The
designer runs on the same machine and the same GPU.
- No designer thread elevates: the UI thread, the check thread, the build
  thread and the watch thread are SCHED_OTHER. The player is started with
  SCHED_OTHER set explicitly, and elevates its own frame thread.
- The preview stops and closes its session at Run: a running preview draws
  every frame, 0.6 to 2.2 ms of a UI frame on the RX 6600 (step 5, part D),
  most of it GPU time. With the pane hidden a frame costs 0.20 to 0.39 ms.
- The redraw rule: a run adds no animation. The watch thread's wake-ups
  bring a frame at most `RUN_REDRAW_HZ` (2) times a second; a person's
  input brings frames as before (an operator's second monitor).
- Edits and checks go on: the check thread is SCHED_OTHER, and the
  player's frame thread at FIFO takes its CPU first. A check costs up to
  41 ms of one core on the generated project (step 3's evidence).
- Not measured, and needed before step 6 closes: the player's dropped
  frames (`frames.csv`) on this machine's display with the designer idle,
  redrawing at 2 Hz, and redrawing every frame with the preview, each for
  a whole stroop session at 60 Hz. That needs the real player on the
  user's display, which this plan did not use. If 2 Hz costs frames, the
  knob is 0 (draw only on input) while the player's window has the
  foreground.

### Rig profiles

**Choose.** The Run view's combo lists `yrig_dir()`'s `*.json` files
(`profile.json` first, as "profile (default)"), "No profile (keyboard
only)", and, in the preview only, "Built-in simulated rig". The view
file's part `run` keeps the choice by name. A folder that `yrig_dir()`
refuses (other users can write it) shows its reason, as the player gives
it.

**View.** Read-only: the rig's name and `written`; each role (family,
key, options, `latency` and `bounds` with their check: OK, MISSING,
CHANGED, DIFFERS); `display.onset_offset_s` and the calibration file and
its check; `view` (distance, width, px, and the px per degree,
`yrig_ppd()`); `signals`; the notes of the load. A button opens the
folder in the OS's file manager (`SDL_OpenURL()` of a `file://` URL).

**Load.** `yrig_load()` on the check thread, when the choice changes or
the file's size or time changes (the UI looks at them once a second while
the Run view shows). 176 us median for a profile with a calibration
(`yrig_profile` is 48 KB, so the checker keeps one, on the heap). The
calibration file (62,528 bytes) is read with it, for `pl_check_desc.cal`
and `pl_sim_desc.cal`.

**What the check needs of the rig** (decision 13). Today `pl_check()`
runs the player's load with stand-ins for every rig need and lists each
one (`PL_NEED_*`). With `pl_check_desc.rig`:

| Need today | With a profile |
|---|---|
| `PL_NEED_DEVICE` (a role the keyboard cannot take) | Bound in the profile: no need, and the role's response rules and buttons are checked against the bound family, as a real start does (`pl_rules_check()` for the bound roles). Not bound: the need stays |
| `PL_NEED_ROLE_TIER` | The tier from `yrig_source_role()` (a checked loopback): met, no need; not met, the need stays, with the profile's tier, and the Run view offers `--allow-below-tier` |
| `PL_NEED_VIEW` | The profile's `view`: met; the stand-in view is replaced by it, so deg sizes are the rig's |
| `PL_NEED_CALIBRATION` | The profile's calibration (its check OK): met, and it is the check's calibration |
| `PL_NEED_SIGNAL`, `PL_NEED_SIM_GAIN` | The profile's `signals`: met, and the loader's signal checks against the rig run |
| `PL_NEED_DISPLAY_TIER` | Stays: only a run's lead-in frames give it |
| `PL_NEED_FIELD` | Stays: the Run view's fields give it |

The check still opens no device and no display (`stand_in`); a device
that is in the profile and not plugged in is the player's refusal at the
start (exit 1), not the check's.

### The views

- **Toolbar** (`ui.cpp`): Build, Run, Stop after Redo. Build is disabled
  with no project or while a build or a run goes; Run is disabled while
  the last check of the current revision has errors, or while a build or
  a run goes; Stop shows only while a run goes. One check box per AUX
  view (today the first only).
- **The view "Run"** (`view_run.cpp`, `UI_SLOT_AUX`, test path
  `//##designer/Run`): sections Rig (the combo, the profile's summary,
  "Show"), Build (path, pack ID, size, time, stale or up to date; Build),
  Run (Simulated, participant and the other fields, seed, the roles below
  tier, Window, the output folder; Run, Stop, Kill), Output (state,
  trials, the last trial, the log's last 200 lines, the exit's meaning,
  Open trials.csv, Open the folder).
- **The preview pane** gets the rig combo's choice through its settings;
  its banner names the profile in use: "Rig: NAME (view 570 mm, 1920 px
  over 518.4 mm)" in place of "Built-in simulated rig: ...".
- **The Issues panel** shows Build's issues after the check's (from
  "Build"), and a failed Run's line (from "Run").

### The designer's shared interfaces

Frozen by part S after wave 1; a part that needs a change stops and
reports it.

`designer/core/dsg.h` (0.4.0):
- `DSG_FROM_BUILD`, `DSG_FROM_PLAYER_RUN`.
- The process (`platform.c`, part X):
  ```c
  typedef struct dsg_proc dsg_proc;
  typedef struct dsg_proc_desc {
      const char* const* argv;      /* argv[0]: the program's path          */
      const char* cwd;              /* NULL: the designer's                 */
      const char* out_path;         /* stdout and stderr; made, must not exist */
      const char* const* watch;     /* files whose growth wakes (NULL-ended) */
      void (*wake)(void* ctx);      /* on the watch thread                  */
      void* ctx;
  } dsg_proc_desc;
  int  dsg_proc_start(dsg_proc** out, const dsg_proc_desc* d, char* err, size_t cap);
  void dsg_proc_stop(dsg_proc* p);           /* SIGTERM; Windows WM_CLOSE */
  void dsg_proc_kill(dsg_proc* p);           /* SIGKILL; TerminateProcess */
  /* 1 ended (status: exit code >= 0, or -signal), 0 running */
  int  dsg_proc_poll(dsg_proc* p, int* status);
  /* Joins the watch thread. A process still running is left running. */
  void dsg_proc_free(dsg_proc* p);
  ```
  WebAssembly: `dsg_proc_start()` fails with "no processes in a browser".
- Build (`build.c`, part B): `dsg_builder_start(desc)`,
  `dsg_builder_stop()`, `dsg_build_post(b, project, out_path, revision)`,
  `dsg_build_cancel(b)`, `dsg_build_take(b, &result)` (`dsg_build_result`:
  revision, status, `dsg_report` of `DSG_FROM_BUILD` issues, path, size,
  pack ID, ms, the sources' stamp), `dsg_build_stale(project, result,
  revision)`.
- Run (`run.c`, part R): `dsg_run_settings` (rig name, sim, participant
  and fields, seed, allow-below-tier roles, windowed, max_seconds, gains,
  out_dir); `dsg_run_args(settings, pack, argv_out, buf, cap)`;
  `dsg_run_out_dir(project, settings, now, out, cap)`;
  `dsg_run_start()`, `_stop()`, `_kill()`, `_tick(now)` (reads the log and
  `trials.csv`), `dsg_run_info` (state, trials, last row, log lines,
  exit, its text); `dsg_run_exit_text(status)`; `dsg_player_version(path,
  out, cap)`.
- Rig (`rig.c`, part G): `dsg_rig_list(names, cap)`; `dsg_rig_load(name,
  &rig, err, cap)` (the profile and its calibration; `dsg_rig` owns both);
  `dsg_rig_changed(rig)` (size or time); `dsg_rig_summary(rig, lines)`.
- The check job and the session settings carry the rig's name:
  `dsg_worker_check()` gains `rig` (the checker loads and keeps it);
  `dsg_session_settings` gains `char rig[64]` ("": the built-in rig).

`designer/app/views.h`: the slots do not change; the check box per AUX
view is `ui.cpp`'s. `ui.h`: `UiPlatform.player_path` (main.cpp:
`SDL_GetBasePath()` + `ysp_player`), `ui_run_active()`. `ui_report()`
returns the merged report.

The view file's part `run`: `{"open": bool, "rig": NAME, "sim": bool,
"fields": {...}, "seed": "", "allow_below_tier": [ROLE], "windowed":
bool, "max_seconds": s}`.

### Who owns which file

| Files | Owner |
|---|---|
| `player/main.c` (`--version`, the `--sim` signal handler), `player/player.h` and `lower.c` (`pl_check_desc.rig`), `run.c` (the stop flag), `tests/player/check_test.c`, `player_test.py`; the player's mutation lists; `docs/player.md`; `docs/status.md` | P (player 0.18.0) |
| `pack/ysp/pack_tool.h`, `pack/pack_tool.c` (`ypt_options.cancel`, the entry cache of decision 17), `tests/pack/tool_test.c`, `docs/pack.md`, `docs/status.md` | K (pack tool 0.12.0) |
| `designer/core/platform.c` (`dsg_proc_*`), `tests/designer/platform_test.c` (the process part), `tests/designer/test_child.c` (new: a child that prints, waits, exits with a code, ignores or takes SIGTERM, lists its descriptors and its policy) | X (the process) |
| `designer/core/dsg.h`, `views.h`, `ui.h`, `ui.cpp` (toolbar, AUX check boxes, the merged report, the Run state), `main.cpp` (the player's path, the quit question), `views.cpp`, `designer/CMakeLists.txt` (`ysp_player` beside the designer; `test_child`); stubs of B, R, G and V; `preview_part.c`'s `run` sibling (`run_part.c`) | Shared (S): frozen during step 6, after wave 1 |
| `designer/core/build.c` (new), the check pack's use of the entry cache in `check.c` (decision 17), `tests/designer/build_test.c` (new) | B (Build) |
| `designer/core/run.c` (new), `tests/designer/run_test.c` (new), `tests/designer/fake_player.c` (new) | R (Run) |
| `designer/core/rig.c` (new), the rig's parts of `check.c`, `worker.c` and `preview.c`, `tests/designer/rig_test.c` (new) | G (rig profiles) |
| `designer/app/view_run.cpp` (new), `designer/app/view_preview.cpp` (the rig's banner), `tests/designer/te_run.cpp` (new), `test_designer_ui`'s Run part | V (the views) |
| `docs/designer.md`: a subsection each under "Decisions of step 6"; `docs/status.md`: an entry each | Each part its own |
| The measurement of "The designer's own frames during a run" | M (with the user, on the display) |

### Tests

Every part keeps the designer suite passing in ctest from the build tree
with `DISPLAY` and `WAYLAND_DISPLAY` unset, with
`YSP_DESIGNER_TEST_ENGINE` off and on, and the player's and the pack
tool's tests and mutation lists. Each new check has a hand mutant that
fails it. The threaded tests also run under ThreadSanitizer (gcc 12,
`setarch -R`). **No test starts the player without `--sim`**: the tests
that start a process start `test_child`, `fake_player` or `ysp_player
--sim`, and `dsg_run_args()` refuses a real run when the environment
variable `YSP_DESIGNER_TEST` is set.

| Part | Tests |
|---|---|
| P | `test_player_check`: with a profile from `player_make_rig` that binds a role, has a view, a calibration and a signal, the matching needs are gone and the others stay; a profile whose role's family the definition does not list gives the start's refusal by name; a profile changes no result of a definition that needs nothing of it. `player_test.py`: `--version`; `--sim` stopped by SIGTERM exits 3 with `"aborted"` (reason `request`) and the finished trials' rows; `--sim`'s output bytes otherwise unchanged. Mutants in `player_lower.toml` and `player_main`. |
| K | `test_pack_tool`: a cancel before the second resource gives the new `YPT_ERR_CANCELED`, no pack and no temporary file; a cancel set during an ffmpeg step stops after it; no cancel gives the same bytes as 0.11.0. |
| X | `test_designer_platform` (process part, POSIX and Windows): `test_child` gives its exit code; its output is in the log file; argv with spaces, quotes, a trailing backslash and UTF-8 arrive whole (Windows quoting); the child sees only descriptors 0, 1 and 2; its policy is SCHED_OTHER when the parent thread is SCHED_FIFO (with an `rtprio` grant; skipped without); SIGPIPE is default in the child when the parent ignores it; Stop ends a child that takes SIGTERM with its code, and Kill one that ignores it; the watch thread wakes on a log's growth and at the exit, within 300 ms; no zombie after `dsg_proc_free()`; a log path that exists is refused. |
| S | The designer runs as before with the stubs; `test_designer_ui`: the toolbar's Build and Run disabled with no project; the AUX check boxes; the merged report's order. |
| B | `test_designer_build`: each `tests/player` project's Build has the pack ID of `ypak build` of the same description; a table with a bad cell gives a `DSG_FROM_BUILD` issue that the tables view maps to the cell; a definition error has its line and column; a warning is an issue of severity warning; Build saves first (the file's bytes are the document's); newest wins; cancel; a stale check by revision and by a source's time; two builds at once (a check pack and a Build) under ThreadSanitizer. |
| R | `test_designer_run`: `dsg_run_args()` for every setting (exact argv); the output folder's name and its refusal when it exists; each exit code's text; with `fake_player` (writes `trials.csv` rows and log lines on a schedule, exits with a given code, takes or ignores SIGTERM): the trials and last row, the log's lines, Stop, Kill after 5 s; with `ysp_player --sim` of rdk through the real spawn: exit 0, 40 rows, the log's end line; the version check refuses another version. |
| G | `test_designer_rig`: with `XDG_CONFIG_HOME` (Windows: `APPDATA`) at a test folder, the list, a load, a changed loopback file's note, a folder that others can write refused; the check with a profile (the needs as in P); a preview session with the profile's view: a stimulus in deg at `yrig_ppd()`'s scale; the file's change reloads it. |
| V | `te_run.cpp`: Build gives the pack ID; Build with a table error shows the issue, and a click selects the cell in Tables; Run (Simulated, `fake_player`) shows the trials and the end; Stop gives "Stopped"; Run disabled while the check has errors; the rig combo lists the test folder's profiles and the preview's banner names the chosen one. `test_designer_ui`: idle frames 0 while a run goes and nothing changes; at most `RUN_REDRAW_HZ` frames a second while `trials.csv` grows. |

### Order of merges

1. Wave 1, in parallel, each on a branch from the merged step 5
   (`step5-int2` or its successor): P, K and X. None needs designer step
   6 code.
2. Into `designer-step6`: P, then K, then X, then S. Each merge runs the
   player's, the pack tool's and the designer's suites and the mutation
   lists.
3. Wave 2, in parallel, each on a branch from `designer-step6`: B, R, G,
   V (V on S's stubs). Merge B, then G, then R, then V.
4. Wave 3: the integration (V's tests on the real B, R and G), then M:
   the measurement of "The designer's own frames during a run" on the
   display, with the user.
5. `designer-step6` into `main` only when the user says so.

### Risks

- **The real player is not run by this plan or by the tests.** Stop's
  path through SDL, the foreground, the player's frames beside the
  designer: each is from the code and a small test program, not from a
  run. Part M is the check, with the user.
- **The check pack re-encodes videos** ("Build"): seconds of no check
  results for a project with a video. Not step 6's to fix; it needs a
  decision and a measurement of its own.
- **Two pack tool builds at once** (the check thread and the build
  thread). Skribidi's collection counter is a known race; others may be
  unknown. ThreadSanitizer in part B is the check.
- **Process code on three systems.** The Windows branch (quoting, the
  handle list, WM_CLOSE, the foreground) and the macOS branch are built
  and tested only by CI. CI's `designer` job has not run yet.
- **A Kill leaves the display changed** (mode, ramp). The Run view shows
  how to put it back; a test cannot check it without the display.
- **Disk.** A `--sim` run of a version 2 definition with no samples wrote
  871 MB before its budget ended it; decision 10's `--max-seconds` caps
  that, and the Run view shows the folder's size.
- **Version drift.** A `ysp_player` of another version would check
  differently from the designer's linked library; decision 5 refuses it.
- **`trials.csv` as progress** depends on the data set's format; a change
  of its columns changes the Run view's last row (the parse reads columns
  by name, not by place).

### Not done in the plan

- Nothing is built. The real player and the display were not used.
- Windows and macOS were not measured or compiled; their process code
  is from the documentation of the calls.
- The check pack's video cost is a finding, not a plan.

### Measured

Conditions: Linux desktop, Intel Core i5-14500, Debian 12, kernel
6.11.5-liquorix, governor `performance`, gcc 12.2, Release (Ninja), SDL
3.4.18, ffmpeg 5.1.6, 2026-11-21, from `f8908c7`. One block at a time
under the measuring lock, pinned with `taskset` to CPUs 2 and 4 (two
P-cores), `DISPLAY` and `WAYLAND_DISPLAY` unset. Each block waited for a
1-minute load average below 1.0: it was 0.83 to 0.95 before each block
and at most 1.20 after one (the video build's own two ffmpeg threads).
The files were on `/tmp`, ext4 on NVMe. A harness (`spawnm`, not kept)
starts each process with `posix_spawn()` (or `fork()` and `execv()`),
times the call and the exit, and takes the child's peak resident memory
from `wait4()`. Milliseconds unless the column says otherwise; median,
p95 and max.

**Build: `ypak build` of the project, a process each** (the same
`ypt_build()` that Build calls; a process that only prints its usage takes
0.46 ms). Peak memory is the process's; for the video case it is the
largest of `ypak` and its ffmpeg child.

| Project | Runs | Pack | Time | Peak memory |
|---|---|---|---|---|
| rdk | 20 | 17 KB | 0.72, 0.87, 0.87 | 4.4 MB |
| oddball | 20 | 25 KB | 0.80, 0.93, 0.93 | 4.6 MB |
| visuomotor | 20 | 21 KB | 0.92, 1.01, 1.01 | 4.7 MB |
| bimanual | 20 | 21 KB | 1.00, 1.08, 1.08 | 4.7 MB |
| stroop | 20 | 599 KB | 7.06, 7.41, 7.41 | 6.6 MB |
| many (generated, 200 texts in 4 scripts) | 10 | 2.5 MB | 35.7, 36.2, 36.2 | 10.5 MB |
| many+words (and 10,000 texts from tables) | 5 | 12.6 MB | 146, 163, 163 | 40.4 MB |
| rdk with a 10 s 1080p video (MJPEG source, MPEG-1 at q 3, `"threads": 2`) | 3 | 27.0 MB | 3,567, 3,574, 3,574 | 103.8 MB |

The in-process build times of step 3 ("Evidence: the check's cost") agree
for the shared projects (stroop 6.36 ms, many 33.9 ms, many+words 143 ms):
the process adds about 0.5 ms.

**The check pack against Build's pack.** For stroop and rdk, a build of
the project's description and one with the experiment's source at
`.designer/check/experiment.json` (as `check.c` makes it): different
pack IDs; the manifests differ in one line (the source's `path`); the
experiment entries have the same SHA-256.

**Starting a process** (`ysp_player` with no arguments: usage, exit 2;
200 runs). "Call" is the time the caller spends in `posix_spawn()` or
`fork()`, which the UI thread would pay.

| Caller | Call, us | Start to exit |
|---|---|---|
| `posix_spawn()` | 41.8, 303, 435 | 0.32, 0.42, 0.49 |
| `posix_spawn()`, 1 GB of the caller's memory touched | 36.7, 51.5, 123 | 0.30, 0.34, 0.63 |
| `fork()` and `execv()` | 16.8, 30.1, 119 | 0.34, 0.40, 0.54 |
| `fork()` and `execv()`, 1 GB touched | 8,542, 10,384, 29,513 | 20.1, 23.4, 49.8 |

**A whole `--sim` session as a process** (10 runs each, with a
`player_make_rig` profile): rdk 64.1, 67.8, 67.8 (40 trials); oddball
59.9, 61.8, 61.8 (16 trials); stroop 61.8, 61.9, 61.9 (40 trials); peak
memory 7.6 to 8.0 MB. Single runs, not pinned, load about 3: the
generated "many" 12.9 s and 317 MB of data (10,000 trials); bimanual and
visuomotor with a gain and no samples ran to the 172,800 s budget, 50.0 s
and 45.4 s, 872 MB and 871 MB of data, exit 1.

**Stop and the pipe** (single runs, not timed):
- `sdlterm` (a test program: `SDL_Init(SDL_INIT_EVENTS)`, `raise(SIGTERM)`,
  then `SDL_PollEvent()`): SDL 3.4.18 gave `SDL_EVENT_QUIT`. The player's
  real run inits SDL's video, which inits its events.
- `ysp_player --sim` of "many", SIGTERM after 1 s: ended by signal 15;
  `session.json` was the one of the start, with no `"aborted"`; the
  writes stopped where they were.
- `ysp_player --sim` of rdk with its standard output to a pipe that was
  closed: status 141 (SIGPIPE) at its end line, after the data set was
  done. A real run's earlier lines would end it mid-session.

**A rig profile's load** (`yrig_load_file()` of `player_make_rig`'s
profile with its 62,528-byte calibration file, 1,000 loads, CPU 2):
175.6 us median, 178.2 us p95, 230.7 us max. `yrig_profile` is 48,072
bytes.

## How to build and run the skeleton

From the repository root, with SDL 3.4.18 or later:

```sh
uv run --no-project python tools/vendor_layout.py --write
uv run --no-project python tools/vendor_pack.py --write
uv run --no-project python tools/vendor_designer.py --write
cmake -B build/designer -DCMAKE_BUILD_TYPE=Release -DYSP_BUILD_DESIGNER=ON -DYSP_BUILD_PLAYER=ON \
      -DYSP_BUILD_PACK=ON -DYSP_BUILD_LAYOUT=ON -DYSP_FETCH_MINIAUDIO=ON -DYSP_FETCH_PL_MPEG=ON \
      -DCMAKE_PREFIX_PATH=/path/to/sdl3
cmake --build build/designer --target ysp_designer designer_tests
ctest --test-dir build/designer -R designer
cp -r tests/player/rdk /tmp/rdk_project
build/designer/designer/ysp_designer /tmp/rdk_project
```

The build copies the fonts that `YSP_DESIGNER_FONTS` lists (Debian's Noto
and DejaVu paths by default) to `designer/fonts/` beside the program, and
the program reads them from there. Open a copy of a project, not the
repository's: Save and the check pack write into the folder. On Windows
and macOS, set `YSP_DESIGNER_FONTS` to fonts of that system, for example
`C:/Windows/Fonts/arial.ttf`.

CI: the job `designer` in `.github/workflows/ci.yml` builds the target
`designer_tests` (every designer test; each registers itself with
`dsg_test()` in `designer/CMakeLists.txt`) on Linux, Windows (MSVC) and
macOS, and runs them with `ctest -R designer`. On Linux
it also builds `ysp_designer` and does not run it. Each OS gives its own
fonts to `YSP_DESIGNER_FONTS` and `YSP_DESIGNER_TEST_FONT`.

## UI tests with the Test Engine

`test_designer_te` drives the UI with the
[Dear ImGui Test Engine](https://github.com/ocornut/imgui_test_engine). The
engine moves the mouse, clicks and types through ImGui's input queue, as a
person does. The test is optional and off by default. `test_designer_core`
and `test_designer_ui` stay the default tests, and a build of ysp never
needs the engine.

### The license: read it first

The engine is not MIT. Its folder `imgui_test_engine/` has the Dear ImGui
Test Engine License v1.04 (`LICENSE.txt` in that folder). Read the license
before you fetch the engine. In summary:

- You get a free license if one of these is true: you are a natural
  person; you are not a legal entity, or you are a not-for-profit one; you
  use it for education; you use it to make software that you release
  publicly under an OSI open-source license (ysp is MIT-0); your legal
  entity's turnover was less than USD 2 million in its last fiscal year.
- Other users get a trial of 45 days at no charge. After the trial, they
  must buy a paid license from DISCO HELLO.
- Only license holders can distribute the engine or software made from it.

For these reasons, the repository has no copy of the engine. The vendor
script fetches it only with `--test-engine`, into
`third_party/imgui_test_engine/`, which git ignores (the rule
`/third_party/*/`). Do not distribute those files or `test_designer_te`
unless you hold a license.

### How to run the tests

From the repository root, after the steps in "How to build and run the
skeleton":

```sh
uv run --no-project python tools/vendor_designer.py --write --test-engine
cmake -B build/designer -DYSP_DESIGNER_TEST_ENGINE=ON
cmake --build build/designer --target test_designer_te
ctest --test-dir build/designer -R test_designer_te
```

- To run one test, give its name as a filter:
  `build/designer/designer/test_designer_te WORK edit_jump`. `WORK` is a
  folder for the test's project copies.
- To check the fetched files against their hashes, run
  `tools/vendor_designer.py --test-engine` with no `--write`.
- When the option is ON and the files are missing, CMake stops and shows
  the fetch command.
- The test opens no window and needs no display or GL.
- CI runs the test (job `designer`). The license permits this: ysp is
  released under MIT-0, an OSI license, and the job keeps no artifact of
  the engine or of `test_designer_te`, so it distributes neither.

### What was taken

- Tag `v1.93.0` of ocornut/imgui_test_engine, commit `16b6efbb393f`. The
  engine's author made this tag on 2026-10-14, the day of ImGui's
  `v1.93.0`, which is the designer's ImGui (`c1aaac65`). ImGui did not
  change.
- 20 files: the 8 sources and 10 headers of `imgui_test_engine/`, its
  `LICENSE.txt`, and `thirdparty/Str/Str.h` (public domain).
- Not taken: `stb_image_write` (screen capture is off), the engine's test
  suite and app helpers (MIT), and ImPlot.

### The tests

| Test | What it does | The hand mutant that fails it |
|---|---|---|
| `path_field` | The folder dialog fails. The path field takes the keyboard. The engine types the path and Enter with no click, and the project opens. | `ui_dialog_failed()` does not give the path field the focus. |
| `edit_jump` | The engine opens a project through the path field and clicks Discard on the question. Its Ctrl+click focuses the Skribidi editor. It types a note with Arabic text, then an `x` before `"stimuli"`; the test puts the caret before each with `skf_set_caret()`. The check finds the error at the `x`. The engine moves the caret with Up and Home, then clicks issue 0: the caret goes to the `x` as a codepoint offset. Ctrl+Z removes the `x`. | `ui_jump_to_issue()` gives the byte offset (caret 177, not 172). Also: `skf.cpp` drops characters above U+007F (the note is not in the text). |
| `unsaved` | The engine types into the editor, then types another folder in the path field and presses Enter. The question opens and the project stays. Cancel keeps the project, the text and the unsaved state. The Open path button asks again, and Discard opens the other project. | `ask_or_run()` does not ask. |
| `undo_views` (step 4) | A structured edit through the document shows in the text. With the chart shown, the engine's Ctrl+Z takes it back and Ctrl+Shift+Z puts it in again. | `undo_keys_free()` in `ui.cpp` always false; also: `sync_editor()` does not show the document's change. |
| `chart/*` (8), `tables/*` (5), `panels/*` (9) (step 4) | The parts' tests: "The chart", "The tables" and "The panels" give each test and its mutants. | - |
| `preview/*` (6) (step 5), `run/*` (9) (step 6) | The parts' tests: "Part B as built" of step 5 and "Part V as built" of step 6 give each test and its mutants. | - |

Each mutant of the first three tests (step 3) failed its test, and the
other tests passed. The mutants were reverted; they are not in a mutation
list. Without mutants, 3 of 3 tests passed in each of 30 runs (Linux
desktop, gcc 12.2, Release). After the step 4 merges, the runner has 23
tests ("The integration").

### How the runner works

- **Hooks.** ImGui calls the engine only when it is compiled with
  `IMGUI_ENABLE_TEST_ENGINE`. `tests/designer/imconfig_designer_te.h`
  includes `designer/app/imconfig_designer.h`, then the engine's
  `imgui_te_imconfig.h`, which defines it. Only `test_designer_te` uses
  this file: it compiles `ui.cpp`, `skf.cpp`, `skt.cpp` and ImGui again,
  with the designer's flags. `ysp_designer_ui` and `ysp_designer` do not
  change. With the option ON, their compile commands and object files
  were the same, byte for byte, as in a build of `main` in the same build
  folder (Linux, gcc 12.2, Release). With the option OFF, the build had
  the same targets and the same tests (132) as `main`.
- **No window.** The runner draws frames as `test_designer_ui` does: a
  1400 x 900 display, textures accepted with no renderer, and a fixed
  frame of 1/60 s. After each frame it calls
  `ImGuiTestEngine_PostSwap()`. It stops when the engine's queue is
  empty. The engine runs in its fast mode: the mouse moves at once and no
  frame waits. The engine's own null app (`shared/imgui_app.cpp`) does
  the same, so the runner does not take it.
- **Coroutines.** The engine runs each test function as a coroutine on a
  second thread, with its `std::thread` implementation. Only one of the
  two threads runs at a time: the test function runs while the frame loop
  waits. Thus a test can call `ui_*()` directly.
- **Item paths.** The engine's item paths do not go into child windows.
  `WindowInfo()` finds the `Definition` and `Issues/##issue_list` child
  windows, and their IDs are the seeds of the item IDs.
- **The Skribidi fields.** They need no glue and no change.
  `skf_draw()` calls `ItemAdd()` and `ItemHoverable()`, so the engine finds
  the field and aims at it. The engine's click and Ctrl+click go through
  the field's own mouse code. `KeyChars()` puts characters in
  `io.InputQueueCharacters`, which the field reads, and Arabic arrives
  whole (`IMGUI_USE_WCHAR32`). Keys from `KeyPress()` reach the field
  through `ImGui::IsKeyPressed()`.

### What it needs

- The C++ runtime (libstdc++ on Linux) and threads, for `std::thread`,
  `std::mutex`, `std::condition_variable` and `std::chrono`. Only the
  test needs them; the designer still does not.
- No exceptions and no RTTI: the engine builds and runs with
  `-fno-exceptions -fno-rtti`. The engine keeps thread-safe statics,
  because the coroutine thread can reach its function-local statics.
- The engine compiles with its warnings off (`-w`), as Skribidi's
  rasterizer does.

### Not done

- A run on Windows (MSVC) and macOS. CI's job `designer` builds and runs
  the test there; it has not run yet.
- ASan and UBSan runs of the test.
- The other parts of `test_designer_ui` (Ctrl+S, the view file, idle
  frames) are not ported; `test_designer_ui` keeps them.

## Open questions

- IME composition (fcitx5 with Mozc, tested by hand 2026-11-18 on the
  Linux desktop; logs in the session scratchpad, not kept). Committed text
  arrived natively and in Firefox. Two faults:
  - Web, fixed on branch `spike-ime-web` (2026-11-18), not yet checked
    with a real IME. The glue read the caret from the textarea's
    selection in `compositionupdate`, before the browser updates the
    textarea. Firefox (128 and 159) then gave the previous composition's
    caret, one step behind (the hand test's x 217.2 where 203.6 is
    right), and lost caret moves with no text change, which fire no
    `compositionupdate`. Chromium 159 gave 0, the composition's start,
    at every step. In both browsers the selection is the IME's caret by
    the `input` event, so the glue now reads it there and clamps it to
    the composition's length. A second fault: committed text overtook
    keys still in SDL's queue (Backspace, then a commit of "X", deleted
    the X; 5 of 5 rounds in Chromium); `spike_text` now runs those keys
    first. `designer/spike/tools/ime_check.py` drives a real composition
    (Chromium through `Input.imeSetComposition`, Firefox through
    Marionette and `nsITextInputProcessor`): the old glue fails it in all
    three browsers, the new one passes.
  - Native, cause found (2026-11-18). On X11, SDL 3.4.18 takes the IME
    through XIM only; its D-Bus clients (fcitx, IBus) are called only by
    the Wayland driver, so `SDL_IM_MODULE` does nothing on X11. fcitx5
    5.0.21's XIM server offers no preedit-callback style by default
    (`UseOnTheSpot` false), so it draws the composition in its own window
    and sends only the committed text: `SDL_EVENT_TEXT_INPUT` and no
    `SDL_EVENT_TEXT_EDITING`. The hint value `composition` is right.
    Firefox goes through GTK's fcitx5 module over D-Bus (inferred from
    the module cache, not observed). Ways out: a local SDL patch that
    calls SDL's IME clients from the X11 driver, as the Wayland driver
    does (preferred: fcitx and IBus, code SDL keeps up); or the spike's
    own fcitx5 D-Bus client (branch `spike-ime-native`, `--ime=dbus`;
    fcitx only, built and not yet typed into).
  - Native, the skeleton (hand test, 2026-11-19, fcitx5 with Mozc, with
    `XMODIFIERS=@im=fcitx`): committed text arrived in the definition
    editor, the composition showed only in fcitx's own window, and that
    window was at the caret. So the editor works with an IME today; inline
    composition still needs one of the two ways out above.
- A file dialog on a Linux desktop with no xdg-desktop-portal and no
  zenity: `SDL_ShowOpenFileDialog()` fails at once there (probe part 1,
  section 6). Answered in step 3 with a typed path ("Decisions of step
  3"); a file browser of the designer's own is not built.
- The glyph atlas uses `RegisterUserTexture`, an ImGui API marked
  internal and experimental. The ImGui version is pinned, and each
  upgrade checks it.
- UI tests: answered 2026-11-19. The Dear ImGui Test Engine, as
  [lsl-viewer](https://github.com/aforren1/lsl-viewer) uses, is an
  optional test layer, off by default, because its license is not MIT
  ("UI tests with the Test Engine").
- Does the check stay in the frame of the edit? Answered 2026-11-20: no.
  Step 5's part T moved it to a thread of its own (the first option
  below; "Plan of step 5", "Part T as built"). Measured 2026-11-20
  ("Evidence: the check's cost"; i5-14500, gcc 12.2, Release, one
  P-core). Each project in the repository fits 16 ms: a stroop text
  keystroke 7.1 ms (the build 6.4 ms), a bimanual keystroke 9.1 ms
  median, 12.9 ms max (`pl_check()`). A generated project with 200 texts
  in four scripts does not: 41 ms a text keystroke, and with 10,000 more
  texts from its tables 201 ms a text keystroke and 37 ms each other
  keystroke. The options:
  - Built (part T): a worker thread for the whole `dsg_check()`, the
    newest text wins. No keystroke waits for a check. It costs a copy of the text and the
    dirty tables per check (60 KB here), a report one check late, and
    one checker thread only (`pl_check()` has static scratch). The
    WebAssembly build needs threads (cross-origin isolation headers) or
    keeps the check in the frame.
  - A debounce of the build only (build after an idle time, check against
    the old pack until then). Typing stops paying for the build, but the
    build still stops one frame when it runs (201 ms above). Until then
    the old pack has no glyph run for the new text; if `pl_check()`
    refuses that (not tried), those issues must be held back.
  - A `texts_key()` that keeps the texts from the tables until a table
    changes. At best it removes the 19.5 ms of "many+words", and its
    other keystrokes then cost 17.7 ms, still above 16 ms.
- The program links libstdc++ for one symbol: the vendored HarfBuzz
  amalgamation refers to `std::__throw_system_error`. The designer's own
  C++ and ImGui need no C++ runtime (built with `-fno-exceptions
  -fno-rtti -fno-threadsafe-statics`). `ypak` links it for the same reason.
- The issues panel lays out each message through the shared layout cache.
  Until step 4 the cache never evicted, so a long session of edits kept
  every message it had shown; it now frees a layout 100 frames after its
  last use ("The layout cache").
- `platform.c`'s Windows branch (UTF-16 paths, `MoveFileExW`) is compiled
  and tested only by CI's job `designer` (`test_designer_core` on Windows),
  which has not run yet.
- Skribidi's `skb_common.h` tests `__cplusplus__`, not `__cplusplus`, so
  in C++ its `SKB_LITERAL()` makes a C compound literal, which MSVC
  refuses (C4576). `skf.cpp` and `skt.cpp` define it again after the
  Skribidi includes, as a C++20 designated initializer. A patch to Skribidi
  would remove the need.
