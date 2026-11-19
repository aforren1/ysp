# The designer

Status: the skeleton is built (step 3 of "Order of work", 2026-11-18, branch
`designer-skeleton`): a window, open and save of a project, the player's
check on each change with its refusals linked to their places in the
text, and redraw on input only. Step 4 is planned, and its shared code is
built (2026-11-19, branch `designer-step4`, "Plan of step 4"). Steps 5 to
7 are not started. This document
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
- The tooltip of a key gives the header's text, the range, the default,
  and the source. A key that only the format has (a node's `repetitions`, a
  state's `max_visits`) shows its name and its default only (prerequisite
  P2 adds a unit and a text to the format's rows).

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

**Tests.** `test_designer_panels` (207 checks) and nine tests on the
Test Engine (`te_panels.cpp`):

| Test | What it checks | Hand mutants killed |
|---|---|---|
| `test_designer_panels` 1, 2 | The type mirror where the rows show it (every `_s` key is a duration in seconds, every row with values is an enum, the flags); every row's kinds covered once by the split fields; the joins: every stimulus and method key that a header's table has takes its row, and the response, choice and control joins | no gfx join; no join of a trace's `rate`; no split |
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
  go to an issue), which `views.h` does not list for a view.
- Prerequisites P1 and P2 did not block anything. Without P1, `schema.h`
  mirrors the type enum and the row flags, and the test checks the mirror
  where the rows show it. Without P2, a key that only the format has shows
  no unit or text.

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
5. The preview.
6. Build and Run.
7. The web build, with the text glue.

## Plan of step 4

Status: planned 2026-11-19 on branch `designer-step4`. The shared code
(the document, the view registry, the view file's parts and their tests)
is built and tested. Parts A (the chart), B (the tables) and C (the
panels) are stubs that build, so three workers can build them at the same
time. This section gives the model that all three parts edit, the shared
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
`animating`. `views.cpp` lists the three step 4 views already, so no part
edits it. A view may call `ui_doc()`, `ui_project()`, `ui_report()`,
`ui_issue_path()`, `ui_reveal()`, `ui_set_status()` and
`ui_text_size()`.

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
- The section and the value type of a format row are internal numbers in
  ysp/experiment.h 0.5.3. `schema.h` mirrors the type enum, and
  `test_designer_panels` checks the mirror where the rows show it (every
  `_s` key is a duration, every row with enum values is an enum).
  Prerequisite P1 removes the mirror.

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

- **P1** (for C, not blocking): ysp/experiment.h makes the format table's
  section numbers and value types public (`yexp_row.section` and
  `yexp_row.type` as named enums), so `schema.h` drops its mirror. New
  public names, no layout change: 0.5.3 to 0.6.0, with docs/experiment.md,
  the README row, docs/status.md, the header's test and its mutation list.
- **P2** (for C, not blocking): the format table's rows have no `unit` or
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

## How to build and run the skeleton

From the repository root, with SDL 3.4.18 or later:

```sh
uv run --no-project python tools/vendor_layout.py --write
uv run --no-project python tools/vendor_pack.py --write
uv run --no-project python tools/vendor_designer.py --write
cmake -B build/designer -DCMAKE_BUILD_TYPE=Release -DYSP_BUILD_DESIGNER=ON -DYSP_BUILD_PLAYER=ON \
      -DYSP_BUILD_PACK=ON -DYSP_BUILD_LAYOUT=ON -DYSP_FETCH_MINIAUDIO=ON -DYSP_FETCH_PL_MPEG=ON \
      -DCMAKE_PREFIX_PATH=/path/to/sdl3
cmake --build build/designer --target ysp_designer test_designer_core test_designer_ui test_designer_doc \
      test_designer_chart test_designer_table test_designer_panels
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

CI: the job `designer` in `.github/workflows/ci.yml` builds
`test_designer_core`, `test_designer_ui` and `test_designer_te` on Linux,
Windows (MSVC) and macOS, and runs them with `ctest -R designer`. On Linux
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
| `chart/shows`, `tables/shows`, `panels/follows_text` (step 4 stubs) | The switch shows the chart and the tables; the caret in the text selects `/stimuli`, which the panels show. Parts A, B and C replace them. | - |

Each mutant of the first three tests failed its test, and the other two tests passed. The mutants
were reverted; they are not in a mutation list. Without mutants, 3 of 3
tests passed in each of 30 runs (Linux desktop, gcc 12.2, Release).

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
- The check pack's build cost is not measured. It runs at open and on each
  edit that changes the texts of text stimuli (typing in a text literal
  builds once a keystroke), in the frame of the edit. In
  `test_designer_core` the whole test, with four builds of the stroop
  pack, took 0.05 s on the Linux desktop; that is not a measurement.
- The program links libstdc++ for one symbol: the vendored HarfBuzz
  amalgamation refers to `std::__throw_system_error`. The designer's own
  C++ and ImGui need no C++ runtime (built with `-fno-exceptions
  -fno-rtti -fno-threadsafe-statics`). `ypak` links it for the same reason.
- The issues panel lays out each message through the shared layout cache,
  which never evicts. A long session of edits keeps every message it has
  shown.
- `platform.c`'s Windows branch (UTF-16 paths, `MoveFileExW`) is compiled
  and tested only by CI's job `designer` (`test_designer_core` on Windows),
  which has not run yet.
- Skribidi's `skb_common.h` tests `__cplusplus__`, not `__cplusplus`, so
  in C++ its `SKB_LITERAL()` makes a C compound literal, which MSVC
  refuses (C4576). `skf.cpp` and `skt.cpp` define it again after the
  Skribidi includes, as a C++20 designated initializer. A patch to Skribidi
  would remove the need.
