# The designer

Status: the skeleton is built (step 3 of "Order of work", 2026-11-18, branch
`designer-skeleton`): a window, open and save of a project, the player's
check on each change with its refusals linked to their places in the
text, and redraw on input only. Step 4 is partly done (2026-11-19,
branch `designer-step4`): the state chart, the tables and the property
panels are built and merged, and the items left are in "The
integration". Steps 5 to 7 are not started. This document
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
- Work waits for a frame: a check, a scroll to restore, a question to
  open.
- A panel animates (none in step 3).

The check runs in the frame of the edit, after the editor and before the
issues panel. The caret does not blink: a blinking caret is an animation.
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
     and the session's load on a thread of their own) comes first. Nothing
     is built.
6. Build and Run.
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
- The check runs in the frame of any change of any buffer.
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
accepted its decisions the same day ("Decisions of the plan"). Nothing is
built. This section answers, from the code at `98de716`, what the preview
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
| The pack tool | `pack/layout.c` (HarfBuzz version pointer, same value each time), `pack/colr.c` and `layout.c` (Windows QPC frequency, same value) | the check thread only | Benign: each write stores the same value |
| ysp/video.h (FFmpeg's `dlopen` once, a heap counter) | `video.h` | the check thread | `pthread_once` and an atomic: safe |
| Skribidi's font collection id (`static uint32_t id`, `++id`) | `skb_font_collection.c` 328 | the UI once in `skt_init()`; the pack tool at each build | Safe: the thread starts after `skt_init()`, and the UI makes no other collection. A second UI collection later would race (a duplicate id, which only keys that collection's layout cache) |
| Skribidi's attribute collection id; `skb_script_from_iso15924_tag()`'s lazy sort | `skb_attribute_collection.c` 36, `skb_layout.c` 145 | the check thread only; no caller | Safe |
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
| `designer/core/preview.h`, the rest of `dsg.h`, `platform.c` (`dsg_remove_tree()`), `views.h`, `ui.h`, `ui.cpp` (the AUX slot, the preview issue in the Issues panel), `main.cpp` (the key hook, framebuffer 0 before the clear), `views.cpp`, `designer/CMakeLists.txt`; stubs of parts A, B and C | Shared (S): frozen during step 5, after T |
| `designer/core/preview.c`; `tests/designer/preview_test.c`, `preview_bench.c` | A (core) |
| `designer/app/view_preview.cpp`; `tests/designer/te_preview.cpp`; `test_designer_ui`'s preview part | B (pane) |
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
  bytes with a direct open.
- **One attach call cannot be split.** A large image's upload is one GL
  call; its cost sets the floor of an adoption's frame. Part D measures
  the largest call on the example packs.
- **Threads.** Until part T the designer has one thread. The player's
  statics and gfx's first open are the known races; ThreadSanitizer is
  the check for the ones not known.
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

- Nothing is built. The check's cost is measured (branch
  `designer-check-cost`); the session's load, attach and step costs are
  not.
- Windows and macOS are not considered beyond what CI builds.

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
- Does the check stay in the frame of the edit? Measured 2026-11-20
  ("Evidence: the check's cost"; i5-14500, gcc 12.2, Release, one
  P-core). Each project in the repository fits 16 ms: a stroop text
  keystroke 7.1 ms (the build 6.4 ms), a bimanual keystroke 9.1 ms
  median, 12.9 ms max (`pl_check()`). A generated project with 200 texts
  in four scripts does not: 41 ms a text keystroke, and with 10,000 more
  texts from its tables 201 ms a text keystroke and 37 ms each other
  keystroke. Not decided. The options, none built:
  - A worker thread for the whole `dsg_check()`, the newest text wins.
    No keystroke waits for a check. It costs a copy of the text and the
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
