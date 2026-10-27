# ysp_layout: text layout for the pack tool and the player

Status: v0.1.0, 2026-10-07. The decision record is
[layout_probe.md](layout_probe.md); this note is the manual of what was
built on it, with the checks and the costs. The plan is
[rig_spec.md](rig_spec.md) 5.2 (Font row) and 11 (item 6).

## What it is

`pack/ysp/layout.h` and `pack/layout.c` (prefix `ylay_`) lay out
UTF-8 text with Skribidi and give the glyphs as `ysp/gfx.h` curve-run items
(`ygfx_citem`) over `ysp/outline.h` curve sets built from the same font
bytes. It is a normal C11 library in the pack tool's tree, not a single
header. The `ysp/*.h` headers stay free of Skribidi and HarfBuzz.

- Skribidi does bidi, script and font runs, shaping (HarfBuzz), line
  breaking (libunibreak, BudouX for Japanese, Chinese and Thai) and the
  editor. It is pinned at a commit with two local patches. The sources are
  fetched into `third_party/` by `tools/vendor_layout.py` and checked
  against `third_party/MANIFEST.sha256`; git tracks only that recipe.
- The glue converts Skribidi's layout runs into one group of items per
  font and size, so each group is one `ygfx_crun()`. It builds the glyphs
  a layout uses into that font's curve set (resolved, backward lists), on
  demand, and lists them for the caller's `ygfx_cset_add()`.
- Every block carries the versions that made it: the Skribidi commit, the
  patch hash, the HarfBuzz version, and the SHA-256 of each font it used.
  `ylay_stamp()` writes them as one line for a log or the pack manifest
  (rig_spec 5.1).
- The glue makes no GL call, so the pack tool runs without a GPU. Uploading
  curve sets is the caller's; `examples/layout/gfx_layout.c` shows the 30 lines.

## Build

The CMake option `YSP_BUILD_LAYOUT` (default OFF) adds `third_party/` and
`pack/`:

```sh
uv run --no-project python tools/vendor_layout.py --write   # once: fetch, patch, verify
cmake -B build -DYSP_BUILD_LAYOUT=ON -DSDL3_DIR=...   # SDL3 only for gfx_layout
cmake --build build --target test_layout gfx_layout
ctest --test-dir build -R layout
```

- No network at configure or build time: no FetchContent. The one fetch is
  the explicit script run above; configure stops with that command if the
  sources are missing.
- The upstream code compiles with its warnings off (`/W0`, `-w`) and its
  include directories are SYSTEM, so our `/W4 /WX` and `-Werror` builds do
  not see its warnings. The upstream sources are not changed beyond the
  patch.
- HarfBuzz compiles from its amalgamation (`harfbuzz.cc`) as C++17 with no
  exceptions and no RTTI; SheenBidi as one unit (`SB_CONFIG_UNITY`);
  Skribidi as C17 with extensions, as upstream sets it. Programs that link
  the glue link with the C++ library (CMake does it; with emcc, link with
  `em++`).
- The option is OFF by default because a header user needs none of it, and
  it adds about 8 MB of sources and a 40 s (MSVC) to 95 s (MinGW, Ninja)
  build of HarfBuzz.

Built and run on 2026-10-07: MSVC 19.44 (`/W4 /WX`), MinGW-w64 gcc 16.1
(`-Wall -Wextra -Wpedantic -Wshadow -Werror`) and emcc 6.0.10 (Docker
`emscripten/emsdk:6.0.10`, the test under node). The test passes on all
three, with the same corpus result.

## Vendoring

| Component | Pin | License | Local change |
|---|---|---|---|
| Skribidi | `dee63d6ba76aeddd49dea6d1b2508cf9aa391f46` (2026-08-17) | MIT | `patches/skribidi-0001-bidi-run-merge.patch`, then `patches/skribidi-0002-l1-trailing-whitespace.patch` |
| HarfBuzz | tag 14.6.0, `c7a7457b7385` (2026-10-05) | Old MIT; `src/ms-use` MIT | none |
| SheenBidi | `83f77108a2873600283f6da4b326a2dca7a3a7a6` | Apache-2.0 | none |
| libunibreak | tag `libunibreak_6_1`, `304585d8e2d6` | zlib | none |
| budouxc | `a044d49afc654117fac7623fff15bec15943270c` | MIT; its models are BudouX's, Apache-2.0 | none |

- 463 files, 7719645 bytes: HarfBuzz 5167534, Skribidi 896122, libunibreak
  760171, SheenBidi 609024, budouxc 286794.
- The files are the include closure of the sources `third_party/CMakeLists.txt`
  compiles (as gcc's dependency output gives it; MSVC's gives the same
  HarfBuzz list), plus the license files. Everything else is trimmed.
- `tools/vendor_layout.py` downloads each pin as a GitHub tarball by full
  commit SHA, takes the files `third_party/MANIFEST.sha256` lists, applies
  the patches in order, and compares every file with the tree byte for byte and with
  the manifest. `third_party/README.md` has the commands and the update
  procedure.
- HarfBuzz 14.6.0, not Skribidi's own 11.0.0: `ysp/outline.h` v0.2.1 places
  glyphs as 14.6.0 does and was checked against it bit for bit; Skribidi
  on 14.6.0 matched Chromium on the corpus (below); 11.0.0 stops emcc on two
  `-Warray-bounds` errors.

## The patches

Two local patches, applied in this order to `src/skb_layout.c`:

| Patch | SHA-256 | What it fixes |
|---|---|---|
| `skribidi-0001-bidi-run-merge.patch` | `ff058faa3c16aa62ec0af21ce30e2db6eee9bce380fec3d51970ceb582164788` | `skb__line_append_shaping_run()` merged a shaping run into the previous layout run without comparing their bidi levels, and merged RTL runs (layout_probe.md 2.3). It adds the two conditions. |
| `skribidi-0002-l1-trailing-whitespace.patch` | `36ce7aec18d9f2b1256ee4374522ef36e87b6850be8bab4ba0bd720c162fb1b1` | Unicode's rule L1 was not applied per line, so a line-end space in an RTL run of an LTR paragraph stayed inside the line (Known issues, fixed). |

Patch 0002, and why it sits where it does:

- Skribidi resolves bidi levels once per paragraph, before it breaks lines,
  and keeps each layout run's level. `skb__finalize_line()` gets each line
  while it is still in logical order, just before `skb__reorder_runs()`
  sorts it by level; that is the one place where a line's extent is known
  and its levels are not yet used. The new `skb__apply_rule_l1()` runs
  there: walking the line back from its logical end, whitespace and
  isolate formatting characters (bidi types WS, LRI, RLI, FSI, PDI) at the
  line end and before a segment or paragraph separator (S, B), and the
  separators, take the paragraph level, as UAX #9 L1 states. A layout run
  that is partly reset is split in two (the line's runs are the last in
  the array, so the split moves only them). The split part keeps its
  shaping direction: its glyphs are whitespace, so their order inside the
  part does not show.
- The line breaker left trailing whitespace out of the fit only when its
  run had the paragraph's direction. With L1 the whitespace sits at the
  line end in either direction, so the condition goes.
- 79 added and 4 changed lines in one file. The editor's caret and
  selection code is unchanged: it reads the levels and runs this leaves
  behind (the wrapped selection and caret checks below agree with Edge).
- The paragraph level is the layout's resolved direction, as Skribidi's
  own alignment and whitespace trimming use. Boundary neutrals that X9
  removes are not looked through.
- Skribidi's own unit tests pass with both patches (MSVC, MinGW gcc 16.1,
  Linux gcc 13.3).

CMake hashes the patches and checks the patched `skb_layout.c` against the
manifest; a tree that differs still builds, and its logged patch field
ends in `,tree-differs`, which the test refuses. The logged field lists
both hashes in order: `sha256:ff058faa...,sha256:36ce7aec...`.

## Pinning and update policy

From layout_probe.md 7, unchanged:

1. Skribidi's sources at a commit, the bidi fix as a named patch file, no
   FetchContent, the same build on MSVC, MinGW and emcc.
2. HarfBuzz pinned by tag, independent of Skribidi's pin. SheenBidi,
   libunibreak and budouxc by commit or tag.
3. The pack manifest records the Skribidi commit, the patch hash, the
   HarfBuzz version and the fonts' hashes (`ylay_stamp()`), because static
   text in a pack is laid-out glyph runs and the player lays out dynamic
   text.
4. Update only for a fix or a feature ysp needs, never on a schedule. An
   update passes `tests/layout/`: glyph ids, visual order and positions for
   every corpus item against Chromium and against the previous pin. Any
   difference goes in the change log; a difference in static text needs a
   pack rebuild, not a player change.
5. Send the two bug reports and the patch upstream; drop the patch when
   upstream has a fix that passes the corpus check.

## API

The header is the reference. In short:

| Call | What it does |
|---|---|
| `ylay_create`, `ylay_destroy` | A library: Skribidi's font and attribute collections, a temp allocator, one reused layout object. One thread, with its `yol_ctx` |
| `ylay_add_font(L, bytes, n, face, name)` | A font, in fallback order; any face of a collection. Opens it with HarfBuzz and `ysp/outline.h`, hashes it, makes an empty curve set. A font `ysp/outline.h` refuses (CFF2, no outlines) is refused by name |
| `ylay_layout(L, text, n, style, spans, n_spans, block)` | Lays out UTF-8 text: size, width (0: no wrapping), line height, direction, alignment, language, a palette color; spans change size, color and language |
| `ylay_take_new_glyphs(L, font, &glyphs)` | The glyphs built since the last call, for `ygfx_cset_add()` |
| `ylay_from_skb(L, layout, x, y, color, append, block)` | The same conversion for a Skribidi layout the caller made (the editor's paragraphs) |
| `ylay_stamp(L, block, buf, cap)` | The versions and font hashes as one line |
| `ylay_skb_fonts`, `_attributes`, `_temp` | Skribidi's objects, for its editor |

Font fallback is Skribidi's rule: for each run of one script, the fonts
that support the script in the order added, and the first that maps the
codepoint. Skribidi gives a font a script only when a run of the font's
cmap starts or ends with a character of that script (it samples the
ends of each range). Real fonts pass; a font made for a test needs a range
that starts with a letter (see `tests/layout/layout_test.c`).

## Checks

`tests/layout/layout_test.c` (ctest `test_layout`):

1. The logged versions: the Skribidi commit, the patch hash (and no
   `tree-differs`) of both patches, HarfBuzz 14.6.0.
2. Visual order on every platform, with a TrueType font the test writes (a
   box for every ASCII, Hebrew and Arabic codepoint): the two bug strings
   and the editor's caret string. The items of each line, by x, must visit
   the bidi segments in the order the Unicode bidi algorithm gives, each
   segment's clusters ascending (LTR) or descending (RTL), and every
   non-space codepoint must have a glyph.
3. The corpus against Chromium (below), for fonts whose SHA-256 equals the
   reference's; another font version is skipped with a message.
4. `ygfx_cset_check()` on every curve set the glue built.
5. A second layout of the same text into the same block grows nothing and
   builds no glyph; spans give two runs and their colors; wrapping gives
   more lines when narrower; the stamp line.
6. Editing against Edge (below): line starts, run order and ink extents at
   10 widths, and selection rectangles and carets on one line and wrapped.
   Every count of differences must be 0.

### The corpus against Chromium

The probe's corpus texts and its Chromium reference were lost with a
session scratchpad. They were regenerated on 2026-10-07 by the probe's
method and committed:

- `tests/layout/corpus.txt`: 15 items, new sentences: the 14 horizontal
  items of layout_probe.md 2.2 (same scripts, fonts and features) and the
  second bug string. The first bug string is item `arabic_in_ltr`.
- `tests/layout/layout_ref.py` (uv, Playwright with the installed Edge,
  pypdf): Edge prints a page that loads each font through `@font-face`, one
  item a page at 100 px per em; a content-stream interpreter reads every
  glyph id (Identity-H CID) and position back.
- `tests/layout/corpus_ref.txt`: Edge 154.0.4258.62, each item's font hash,
  glyph ids in visual order and positions in font units.

Result, patched Skribidi, the same on MSVC, MinGW and emcc (node):

| Item | Font | Glyphs | Ids | d, font units |
|---|---|---|---|---|
| Latin, kerning and ligatures | Segoe UI | 98 | same as Chromium | 0.00 / 0.00 |
| Latin | Calibri | 80 | same | 0.00 / 0.00 |
| Latin | Arial | 92 | same | 0.00 / 0.00 |
| Arabic with marks, Latin and numbers, RTL paragraph | Arial | 72 | same | 1.12 / 0.00 |
| the same | Segoe UI | 72 | same | 0.52 / 0.00 |
| Arabic phrase and number in an LTR paragraph (bug string 1) | Arial | 54 | same | 0.68 / 0.00 |
| Hebrew with points, Latin and numbers | Arial | 55 | same | 0.48 / 0.00 |
| the same | Segoe UI | 55 | same | 0.36 / 0.00 |
| Devanagari: reph, i-matra, conjuncts | Nirmala UI | 45 | same | 0.00 / 0.00 |
| Bengali conjuncts and split vowels | Nirmala UI | 41 | same | 0.00 / 0.00 |
| Tamil split vowels, ஸ்ரீ | Nirmala UI | 51 | same | 0.00 / 0.00 |
| Thai with stacked marks | Leelawadee UI | 53 | same | 0.00 / 0.00 |
| Chinese with punctuation | Microsoft YaHei | 27 | same | 0.00 / 0.00 |
| Japanese with punctuation | Yu Gothic | 41 | same | 0.00 / 0.00 |
| RTL paragraph, Latin word, waw, number (bug string 2) | Arial | 16 | same | 0.44 / 0.00 |

"d" is the largest position difference along the line / across it, after
the first glyph's offset is removed. Lines of one direction are exact.
Bidi lines differ by up to 1.12 font units (0.055 px at 100 px per em):
Chromium puts each bidi run on its 1/64 px grid, 0.32 font units here, and
a line of several runs adds a few of those. The test allows 2 font units.

### Without the patch

One run with the patch reversed (`git apply -R` in a copy of the tree,
MSVC, 2026-10-07; repeated with the final tree, same result). CMake
warned that `skb_layout.c` differs from the manifest, the logged patch
field ended in `,tree-differs`, and the test failed that check too.

- The written-font order checks failed on all three strings: in bug string
  1 and the caret string the Arabic word came after the last LTR word
  (23 and 6 items out of order); in bug string 2 one item ran against its
  segment's direction.
- 9 of 15 corpus items equal Chromium. The 6 that differ: both Arabic RTL
  items (66 glyphs, Chromium 72), `arabic_in_ltr` (the glyph at visual
  position 13 is 80, Chromium 993), both Hebrew items (38 glyphs, Chromium
  55), and `rtl_latin_waw` (15 glyphs, Chromium 16: the waw is gone).

### Editing against Edge

Made 2026-10-08 after a user tried `gfx_layout`'s editor with
`abc مرحبا 123 def.` and reported a split highlight with a possible gap at
the space between `123` and `مرحبا`, and unexpected wrapping.

Method:

- `tests/layout/editor_items.txt`: 8 items. `user_bidi` is the user's line,
  `page4` is page 4's text (Segoe UI, then Microsoft YaHei), and the other
  6 are the mixed-direction lines of the corpus.
- `tests/layout/editor_ref.py` (uv, Playwright, the installed Edge
  154.0.4258.62) lays each item out in a div that loads the item's font
  files through `@font-face`, at 100 px per em, `white-space: pre-wrap`,
  and reads `Range.getClientRects()`. It writes
  `tests/layout/editor_ref.txt` with the font hashes. Two models:
  - `layout`: a plain div with `overflow-wrap: normal`, against
    `ylay_layout()` (Skribidi's `SKB_WRAP_WORD`);
  - `editor`: a `contenteditable` div with `overflow-wrap: break-word`, the
    model of a textarea, against Skribidi's editor (`SKB_WRAP_WORD_CHAR`).
    A textarea itself gives no Range rectangles, so the div stands in for
    it.
- Wrapping: 10 widths per item, from the widest word to the whole line.
  Compared: each line's first logical offset; each line's words in visual
  order; and, for the editor, each line's ink extent (the outer edges of
  its characters other than spaces and combining marks).
- Selection, on one line: every range `[s, e)` of `user_bidi` (171), and
  every one-grapheme range of the other bidi lines. A range that ends
  inside a grapheme (a letter without its marks) is skipped: an editor
  never selects one, Edge returns the whole grapheme and Skribidi returns
  nothing. Compared: the x intervals the rectangles cover per line, after
  touching rectangles are merged, within 4 font units (Edge puts each box
  on a 1/64 px grid, 0.32 font units here).
- Caret: at every logical offset, Skribidi's caret with either affinity
  against Edge's collapsed Range, within 4 font units. Vertical extents are
  not compared (line boxes differ by design).
- Wrapped, added with patch 0002: `user_bidi` in the editor model at two
  widths that break it (280.30 px, 4 lines, and 468.28 px, 2 lines, the
  first ending `مرحبا `): all 171 selections each, compared per line, and
  every caret, compared with its line.

Result with both patches, the same on MSVC, MinGW gcc 16.1, Linux gcc
13.3 and emcc 6.0.10; the corpus still equals Chromium on all 15 lines:

| Check | Compared | Differ, patch 0001 only | Differ, patches 0001 and 0002 |
|---|---|---|---|
| Line starts | 160 layouts | 5 | 0 |
| Run order on the same lines | 155 (160 with 0002) | 0 | 0 |
| Ink extent of each line, editor, on the same lines and order | 77 (80 with 0002) | 10 | 0 |
| Selections, one line | 347 (160 inside a grapheme skipped) | 0 | 0 |
| Carets, one line | 361 | 0 (92 with Skribidi's other affinity) | 0 (the same 92) |
| Selections, wrapped | 342 | 160 | 0 |
| Carets, wrapped | 38 | 12 | 0 |

The first table of this comparison (before patch 0002) found the five
early breaks and ten misplaced spaces; the wrapped checks were added with
the patch and, run once against a tree without it, found 160 selections and
12 carets that differ, all on the lines with the misplaced space (a caret
at offset 10 at 468.28 px, after `مرحبا `, was at x 3753 on line 0 or at the
start of line 1; Edge's is at x 8200 on line 0).

- The highlight the user saw is the logical selection, and its geometry is
  Edge's: for all 171 ranges of `abc مرحبا 123 def.`, Skribidi's rectangles
  cover the same intervals as Edge's within 4 font units. The space between
  `123` and `مرحبا` (offset 9) belongs to the RTL run and sits visually
  between the two words; a range that stops before it leaves it unselected
  in both.
- Skribidi gives most selections (304 of 347) as rectangles that touch or
  overlap: it reports each run's rectangle and then the same rectangle grown
  by the next run, so the range `[3, 14)` comes as 4 rectangles from one
  left edge (46.76 to 54.98, 103.49, 176.85 and 185.07 px). `gfx_layout`
  drew each one with its 1 px soft edge. It now merges the rectangles of a
  line that touch or overlap and draws one band, as a browser does. In the
  screenshot of that range before the change no seam showed (the
  rectangles overlap rather than abut); the change removes the case where
  two soft edges would meet inside a band.
- Carets: 92 of 361 match only with Skribidi's other affinity, at run
  boundaries, where Edge picks the other side. Both positions are valid
  carets for that offset.

## Known issues

### Fixed by patch 0002: Skribidi kept a line-end space of an RTL run inside the line

In an LTR paragraph, when a line ends with an RTL word and the space after
it, Skribidi counts the space in the line-breaking width and draws it
between the text before the RTL word and the word, instead of letting it
hang at the line end. Unicode's rule L1 resets whitespace at each line end
to the paragraph level; Skribidi runs the bidi algorithm once per
paragraph (`skb_layout.c` line 351), before it breaks lines, and its line
breaker leaves trailing whitespace out of the width only when the run has
the paragraph's direction (`skb_layout.c` line 2205).

Effect, measured against Edge with `abc مرحبا 123 def.` (Segoe UI, 100 px
per em): at 405.62 px Skribidi makes `abc ` / `مرحبا ` / `123 def.` where Edge
makes `abc مرحبا ` / `123 def.` (the two words need 400.4 px, the space 27.4
px more); where both break alike, Skribidi's RTL word sits one space width
(561 font units) to the right of Edge's. 5 of 160 layouts break early and
10 of 77 lines put the space inside; RTL paragraphs are not affected. This
is the "interesting wrapping" a user can see on page 4 when an Arabic word
ends a line.

Removing only the direction test would fix the break but leave the space
drawn inside the line, which would then overflow by a space. Patch 0002
(The patches, above) applies L1 per line and removes the direction test;
with it every check of "Editing against Edge" equals Edge, and the test
requires that. The upstream issue draft carries the patch; drop the patch
when upstream has a fix that passes the same checks (policy point 5).

## Costs

`tests/layout/layout_bench.c` (target `layout_bench`), patched Skribidi,
on the probe's laptop (i7-1360P, AC), under the shared measurement lock at
11 % load, 2026-10-07. Medians of 3 interleaved rounds, each the median of
at least 0.3 s of calls; microseconds per call. "Layout" is one
`ylay_layout()` call: Skribidi from UTF-8 and the glue's conversion into
items, with every glyph already built. A keystroke is one Skribidi editor
edit (insert a letter or Backspace at the end) and `ylay_from_skb()` on
its paragraph.

| Item | Codepoints | MSVC 19.44 `/O2`: median (p90) | MinGW gcc 16.1 `-O2`: median (p90) |
|---|---|---|---|
| Label, English, 32 px | 26 | 5.3 (6.3) | 5.1 (5.2) |
| Label, Arabic, 32 px | 14 | 7.0 (8.0) | 6.5 (6.6) |
| Paragraph, English, 16 px at 600 px | 516 | 66.0 (81.9) | 51.2 (64.3) |
| Paragraph, Arabic with a Latin sentence | 540 | 120.9 (152.5) | 98.6 (118.1) |
| Keystroke at 500, English | 516 | 94.0 (97.6) | 61.9 (77.4) |
| Keystroke at 500, Arabic | 540 | 163.6 (169.3) | 111.2 (132.7) |

The largest single call in the last round: 89 to 149 us for a label, 242
to 408 us for a paragraph, 158 to 603 us for a keystroke.

One-time costs:

| What | MSVC | MinGW |
|---|---|---|
| `ylay_add_font()`, Segoe UI (0.96 MB): HarfBuzz and `ysp/outline.h` open it, Skribidi collects its codepoints and scripts, the glue hashes it | 3.8 ms | 3.9 ms |
| the same, Microsoft YaHei (19.7 MB) | 61.5 ms | 67.9 ms |
| first build of a glyph into the curve set, resolved: Latin (32 glyphs) | 15.6 us | 17.5 us |
| the same, CJK (30 glyphs) | 35.7 us | 41.0 us |

What this means for the player (rig_spec 6):

- Static text: laid out once, at setup or between trials; the frame loop
  draws the block's items and allocates nothing (`examples/layout/gfx_layout.c`
  does this). The glue's own arrays grow only when a block needs more
  room; the test checks that a second layout of the same text grows
  nothing and builds no glyph.
- Dynamic text (a feedback label with a number, a typed response) is
  laid out again when it changes: 5 to 8 us for a label, 0.1 ms for a
  keystroke in a 500-codepoint box, then one `ygfx_crun()` per font
  (no allocation). A glyph not yet in the set costs 16 to 41 us to build
  and an upload between frames (`ygfx_cset_add()`); a pack built from
  the experiment's texts holds them already (rig_spec 5.2). Skribidi
  allocates inside each layout (layout_probe.md 5.2: 9 to 21 allocator
  calls for a label, unpatched; not counted again here).
- A 500-codepoint page: 0.05 to 0.12 ms here, worst call 0.4 ms. Lay
  instruction pages out between trials anyway, as rig_spec 5.2 plans.
- Against the probe's unpatched numbers (layout_probe.md 5.1, MSVC):
  English 500 codepoints 66.0 against 124.5 us, Arabic 120.9 against
  202.3 us, labels 5.3 to 7.0 against 13.1 to 24.3 us. The bench differs
  (this one reuses a layout and a block, and draws the paragraph from a
  repeated sentence), so this is not a measure of the patch's cost. The
  patch makes more layout runs in RTL text; this table puts RTL text at
  1.8 times Latin, as the probe's did (1.6).

In `examples/layout/gfx_layout.c` (MSVC, `--sim`, two runs, not under the lock):
setting up all four pages (seven paragraphs, three bidi lines, four
phrase-breaking blocks, the editor, every label) lays everything out and
builds 286 glyphs of the main font list in 14 to 27 ms; a scripted undo in
the editor costs 64 to 74 us, the rebuild of its runs included.

## In the bindings

`ysp-vis` (Python `ysp.gfx`, MEX `ysp_gfx`, 2026-10-27) links this
library and the vendored stack as static libraries:
`tools/vendor_vis.py --layout msvc` (or `mingw`) builds them with
`tools/vis_layout/CMakeLists.txt` from the trees `tools/vendor_layout.py`
checks. A binding's text is a `ylay_layout()` block per text and a
`ylay_lib` per fallback list, its glyphs uploaded with `ygfx_cset_add()`
as `examples/layout/gfx_layout.c` does. Its manual is
[gfx.md](gfx.md#text), with the costs measured through each host.

## Not done

- Vertical text (Skribidi has none), justification, explicit bidi
  controls (U+202A to U+2069; untested).
- Variable-font instances and CFF2 through HarfBuzz's draw API into
  `ysp/outline.h`'s builder (layout_probe.md 8.4): a font `ysp/outline.h`
  refuses is refused here.
- Color glyphs, decorations (underline, the IME composition's underline),
  Skribidi's icons and inline objects: the glue converts text runs only.
- Serializing blocks into the pack: the pack tool's job, next.
- The designer's path (Skribidi in a browser page with composition events
  and a hidden text input) and the editor's IME with a real input method:
  `examples/layout/gfx_layout.c` drives the editor's composition calls with a
  script in `--sim`; a person with an IME has not run it.
- Untrusted-font robustness of HarfBuzz and Skribidi (no fuzzing here;
  `ysp/outline.h`'s reader is fuzzed).
