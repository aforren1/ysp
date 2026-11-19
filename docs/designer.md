# The designer

Status: planned, not built (2026-11-18). This document gives the
designer's decisions, its first version's scope, its architecture and
its order of work. [rig_spec.md](rig_spec.md) section 7 gives the
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
    +-- ysp_pack_tool, ysp_layout (Skribidi, HarfBuzz)
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
     of height 0 and shows no IME composition; set its text to "" after
     `skb_editor_create()` ([layout.md](layout.md), Known issues).
3. A skeleton: the window, open and save of a project, the loader's
   checks with refusals linked to fields, and redraw on input only.
4. The state chart, the tables and the panels from parameter tables.
5. The preview.
6. Build and Run.
7. The web build, with the text glue.

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
  section 6). The designer needs a fallback.
- The glyph atlas uses `RegisterUserTexture`, an ImGui API marked
  internal and experimental. The ImGui version is pinned, and each
  upgrade checks it.
- UI tests: the Dear ImGui Test Engine, as
  [lsl-viewer](https://github.com/aforren1/lsl-viewer) uses. Check its
  license before taking it.
