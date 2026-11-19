# The designer

Status: the skeleton is built (step 3 of "Order of work", 2026-11-18, branch
`designer-skeleton`): a window, open and save of a project, the player's
check on each change with its refusals linked to their places in the
text, and redraw on input only. Steps 4 to 7 are not started. This document
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
- **Code.** `designer/core/` (C11, `dsg.h`) holds every part with no
  window: the project, the check, the places of issues and the redraw
  rule. `designer/app/` (C++20) holds the UI: `ui.cpp` (the panels, no SDL
  or GL), `skf.cpp` (the Skribidi edit field), `skt.cpp` (Skribidi text in
  ImGui) and `main.cpp` (SDL3, GL ES 3.0, the loop). Only
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
     Devanagari. Open, on any text: three faults of pinned Skribidi's
     editor that a text field can reach (Ctrl+Right reads past the text,
     a key in a field that never had text dereferences NULL, Enter passes
     NULL to `memcpy`); [layout.md](layout.md), "Open: three faults of
     Skribidi's editor".
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
4. The state chart, the tables and the panels from parameter tables.
5. The preview.
6. Build and Run.
7. The web build, with the text glue.

## How to build and run the skeleton

From the repository root, with SDL 3.4.18 or later:

```sh
uv run --no-project python tools/vendor_layout.py --write
uv run --no-project python tools/vendor_pack.py --write
uv run --no-project python tools/vendor_designer.py --write
cmake -B build/designer -DCMAKE_BUILD_TYPE=Release -DYSP_BUILD_DESIGNER=ON -DYSP_BUILD_PLAYER=ON \
      -DYSP_BUILD_PACK=ON -DYSP_BUILD_LAYOUT=ON -DYSP_FETCH_MINIAUDIO=ON -DYSP_FETCH_PL_MPEG=ON \
      -DCMAKE_PREFIX_PATH=/path/to/sdl3
cmake --build build/designer --target ysp_designer test_designer_core test_designer_ui
ctest --test-dir build/designer -R designer
cp -r tests/player/rdk /tmp/rdk_project
build/designer/designer/ysp_designer /tmp/rdk_project
```

The build copies the fonts that `YSP_DESIGNER_FONTS` lists (Debian's Noto
and DejaVu paths by default) to `designer/fonts/` beside the program, and
the program reads them from there. Open a copy of a project, not the
repository's: Save and the check pack write into the folder.

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
- A file dialog on a Linux desktop with no xdg-desktop-portal and no
  zenity: `SDL_ShowOpenFileDialog()` fails at once there (probe part 1,
  section 6). Answered in step 3 with a typed path ("Decisions of step
  3"); a file browser of the designer's own is not built.
- The glyph atlas uses `RegisterUserTexture`, an ImGui API marked
  internal and experimental. The ImGui version is pinned, and each
  upgrade checks it.
- UI tests: the Dear ImGui Test Engine, as
  [lsl-viewer](https://github.com/aforren1/lsl-viewer) uses. Check its
  license before taking it.
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
- `platform.c`'s Windows branch (UTF-16 paths, `MoveFileExW`) is not
  compiled or tested.
- Skribidi at dee63d6 passes a NULL buffer to `memcpy()` with a length of
  0 when a paragraph is empty (`skb_text.c` line 628, UBSan
  `nonnull-attribute`); every text that ends with LF has one, so every
  canonical definition does. It is harmless on the compilers used, and not
  patched here.
