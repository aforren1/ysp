# ysp_layout: text layout for the pack tool and the player

Status: v0.3.4, 2026-11-19 (v0.1.0, 2026-10-07: the first build; v0.2.0:
font fallback past the script, [Patch 0003](#patch-0003-font-fallback-past-the-script);
v0.3.0: emoji and color glyphs, [Emoji](#emoji), [Color glyphs](#color-glyphs) and
[Patch 0004](#patch-0004-a-run-whose-family-has-no-font); v0.3.1: two
editing faults with Devanagari,
[Patches 0005 and 0006](#patches-0005-and-0006-editing-devanagari);
v0.3.2: cluster values out of order after a right-to-left override,
[Patch 0007](#patch-0007-cluster-values-out-of-order); v0.3.3: faults of
Skribidi's editor on empty texts and at a text's ends,
[Patch 0008](#patch-0008-the-editor-at-its-texts-edges); v0.3.4: End past
the LF on each line but the first,
[Patch 0009](#patch-0009-end-on-each-line-but-the-first)).
The decision record is
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
  editor. It is pinned at a commit with eight local patches. The sources are
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
| Skribidi | `dee63d6ba76aeddd49dea6d1b2508cf9aa391f46` (2026-08-17) | MIT | `patches/skribidi-0001-bidi-run-merge.patch`, `patches/skribidi-0002-l1-trailing-whitespace.patch`, `patches/skribidi-0003-font-fallback.patch`, `patches/skribidi-0004-emoji-family-fallback.patch`, `patches/skribidi-0005-text-grapheme-breaks.patch`, `patches/skribidi-0006-caret-split-grapheme.patch`, `patches/skribidi-0007-monotone-clusters.patch`, in that order |
| HarfBuzz | tag 14.6.0, `c7a7457b7385` (2026-10-05) | Old MIT; `src/ms-use` MIT | none |
| SheenBidi | `83f77108a2873600283f6da4b326a2dca7a3a7a6` | Apache-2.0 | none |
| libunibreak | tag `libunibreak_6_1`, `304585d8e2d6` | zlib | none |
| budouxc | `a044d49afc654117fac7623fff15bec15943270c` | MIT; its models are BudouX's, Apache-2.0 | none |

- 463 files, 7725840 bytes: HarfBuzz 5167534, Skribidi 902317, libunibreak
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

Nine local patches, applied in this order. 0001, 0002, 0004, 0006 and
0007 change `src/skb_layout.c`; 0003 changes it, `src/skb_font_collection.c`
and `include/skribidi/skb_font_collection.h`; 0005 changes
`src/skb_text.c`; 0008 changes `src/skb_editor.c`, `src/skb_text.c` and
`src/skb_rich_layout.c`; 0009 changes `src/skb_editor.c`.

| Patch | SHA-256 | What it fixes |
|---|---|---|
| `skribidi-0001-bidi-run-merge.patch` | `ff058faa3c16aa62ec0af21ce30e2db6eee9bce380fec3d51970ceb582164788` | `skb__line_append_shaping_run()` merged a shaping run into the previous layout run without comparing their bidi levels, and merged RTL runs (layout_probe.md 2.3). It adds the two conditions. |
| `skribidi-0002-l1-trailing-whitespace.patch` | `36ce7aec18d9f2b1256ee4374522ef36e87b6850be8bab4ba0bd720c162fb1b1` | Unicode's rule L1 was not applied per line, so a line-end space in an RTL run of an LTR paragraph stayed inside the line (Known issues, fixed). |
| `skribidi-0003-font-fallback.patch` | `bf6296af91c73737b18637e57f86f95fa443fad83848013cb427a5f47a2ca225` | A script run took its fonts only from the fonts that support its script, so a digit or punctuation mark in Hebrew or Arabic text got glyph 0 when the script's font lacked it, though another font in the list had it ([Patch 0003](#patch-0003-font-fallback-past-the-script)). |
| `skribidi-0004-emoji-family-fallback.patch` | `ce9aa9eea3585162bb2aff62725f883465f63e54e2ea3d3e4bc21c847add4ab2` | A run whose font family had no font was skipped: with no emoji font, an emoji vanished with no glyph 0, and with emoji fonts only, text did ([Patch 0004](#patch-0004-a-run-whose-family-has-no-font)). |
| `skribidi-0005-text-grapheme-breaks.patch` | `11081a2421b97f0111040b4934d98bfc349471a5b1647f5a34a07552c65ab69c` | An edit of `skb_text` set grapheme breaks for the new codepoints only, as if they were a separate text, so typed Devanagari got breaks inside a grapheme ([Patches 0005 and 0006](#patches-0005-and-0006-editing-devanagari)). |
| `skribidi-0006-caret-split-grapheme.patch` | `dfa7cdbd1cc8e05ffb4c93a7135a348d193a9084cc31b3cd1aff0065d3ca12d5` | The caret iterator took each cluster as whole graphemes; where HarfBuzz splits a grapheme into two clusters, the first got no caret stop and its width was lost ([Patches 0005 and 0006](#patches-0005-and-0006-editing-devanagari)). |
| `skribidi-0007-monotone-clusters.patch` | `f8459c8b13e50157995ca0769329e7698cbd4ce82a33666f0aebcd93eacf191c` | `skb__shape_run()` took each cluster's text range from the cluster values next to it and assumed them in order; HarfBuzz gave them out of order for Devanagari after U+202E, and a cluster got a negative range ([Patch 0007](#patch-0007-cluster-values-out-of-order)). |
| `skribidi-0008-editor-empty-text.patch` | `4b08c2781fbacaca9b7b0988cd1a6c30e4728e9cb340bb34b715d84c232dac21` | The editor read paragraph 0 of none in an editor that never had text, read text properties past the text (Ctrl+Right) and before it (a double click at offset 0), and gave `memcpy()` NULL for each empty paragraph ([Patch 0008](#patch-0008-the-editor-at-its-texts-edges)). |
| `skribidi-0009-line-end-offset.patch` | `f0c97fa5dfa25375d7f997bdc398931fea71ca27f3b866af79aaec11976bcccb` | `skb_editor_get_line_end_at()` gave the step that keeps the caret before a line's last control character a global offset, which that step compares with an offset in the paragraph; End stopped past the LF on each paragraph but the first ([Patch 0009](#patch-0009-end-on-each-line-but-the-first)). |

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

CMake hashes the patches and checks each source file a patch changes
(`skb_layout.c`, `skb_font_collection.c`, `skb_text.c`, `skb_editor.c`,
`skb_rich_layout.c`; `skb_layout.c` only before v0.3.3) against the
manifest; a tree that differs still builds, and its logged patch field
ends in `,tree-differs`, which the test refuses. The logged field lists
the hashes in the order the patches apply: `sha256:ff058faa...,sha256:36ce7aec...,` and so on.
The pack manifest's `tool.libraries.layout` line carries the same field,
so a pack records which patches laid out its glyph runs (rig_spec 5.2).

### Patch 0003: font fallback past the script

Found 2026-11-18 on the Linux desktop (GCC 12) with `ypak build`. Skribidi
splits a script run by font, but takes the candidates only from the fonts
that support the run's script (`skb_font_collection_match_fonts()`).
Digits and punctuation (script Common) take the script of the text next to
them. So "שלום 123" with the fonts [Noto Sans Hebrew, Noto Sans], in either
order, was one Hebrew run in Noto Sans Hebrew, which has no digits and none
of `: , . %`: those characters got glyph 0, and Noto Sans, which has them,
was never a candidate. "123" alone and "abc 123 שלום" got Noto Sans's
digits; DejaVu Sans alone, which has Hebrew and digits, worked. The pack
tool then refused the pack and told the author to add a font that was
already in the list.

Upstream: Skribidi's `main` is at the pin, `dee63d6` (checked 2026-11-18);
no open pull request changes this rule. Not reported upstream; the user
decides that.

The rule the patch adds, in `skb__itemize()`:

- When none of the fonts of the run's script has a codepoint, its grapheme
  cluster comes from the first font of the family, in the order added, that
  has every codepoint of the cluster, as browsers do; if no font has the
  whole cluster, from the first font that has the codepoint. The new
  `skb_font_collection_match_fallback_fonts()` lists the candidates with
  the same style matching as `skb_font_collection_match_fonts()` (weight,
  style, stretch), with the codepoint in place of the script as the hard
  constraint. It does not call the font fallback callback, which is keyed
  by script.
- The run's font keeps everything it has: after a fallback cluster, the
  next codepoint is checked against the script's fonts first, and a
  codepoint they lack is looked up again, so it comes from the first font
  that has it, not from the previous fallback font.
- Default ignorable codepoints (ZWJ, ZWNJ, variation selectors and the rest
  of HarfBuzz's `is_default_ignorable()` set) never fall back. HarfBuzz
  hides them, so they need no glyph; moved to another font, a ZWJ would
  split the Arabic run around it and break the joining. The ZWJ check of
  `test_layout` part 7 guards this.
- 135 added and 14 removed lines in three files, in Skribidi's style.

Decisions, and why:

| Question | Choice | Why |
|---|---|---|
| Common and Inherited only, or every missing cluster | Every cluster none of the script's fonts has | A letter the script's font lacks (a rare letter, a font made for a test) is the same fault: glyph 0 while a font in the list has it. Browsers fall back for any character. The pack tool's message can then say "no font in the list maps" and be true. Text the script's fonts cover is not touched, so the rule changes no output that had no glyph 0 |
| The unit | The grapheme cluster, else the codepoint | A mark stays with its base: shaped in another font than its base, a mark is placed without the base. If the base is in the run's font and only a fallback font has the mark, the whole cluster moves to that font (the run is split at the cluster's start). If no font has the whole cluster, the codepoint alone moves, as Skribidi already does between two fonts of one script |
| Where to split | At the cluster's ends, inside the script run and the bidi run | The fallback run is a shaping run like any other font split: it keeps the script run's script, direction and bidi level, so bidi reordering and the cluster map are unchanged. Digits in RTL text already have their own bidi level and run; punctuation at level 1 (", " in "שלום 12:30, 4.5%") is shaped RTL in the fallback font, so HarfBuzz mirrors what needs it |
| Line metrics | Each run's own font, as before | Skribidi takes a line's ascender, descender and line height as the extremes of its runs' fonts. A line that uses a fallback font can be taller than the script's font alone (in the test, 90 and 30 units against 80 and 20); a line that uses none keeps its metrics. Browsers do the same for `line-height: normal`. Noto Sans and Noto Sans Hebrew have the same metrics, so the reported texts do not move |
| Baseline | Shared | Every run sits on the layout's baseline (alphabetic by default), as before; the fallback run's baseline set is the fallback font's for the run's script |
| Cost | A lookup only for codepoints the script's fonts lack | The lookup walks every font of the collection. The helper is kept out of line: inlined, it made the layout of text that needs no fallback 0.5 to 0.8 % slower (GCC 12, Costs) |

Outputs that changed: none of text with no glyph 0. With the Linux fonts
of this machine (Arial, DejaVu Sans, Noto Sans and the Noto fonts of each
script, Droid Sans Fallback), the corpus and editor texts and seven more
(numbers and punctuation in Hebrew and Arabic, a ZWJ, a ZWNJ and a
variation selector in Arabic, an English label), each in up to nine font
lists, at widths 0, 250 and 600: all 492 layouts that had no glyph 0 are
byte-identical (every item's glyph, position and cluster, every line, run
and box); of the 228 that had glyph 0, 72 have none now, and in the 156
left no font of the list has the character (Devanagari with Arial alone,
the Chinese of `page4` with Noto Sans Arabic and Arial, and so on). The
harness (a dump of every block, and a comparison) ran from a scratch
folder and is not in the repository: it needs this machine's fonts. The
corpus against Chromium and the editing checks need the Windows fonts and
did not run here.

### Patch 0004: a run whose family has no font

Found 2026-11-18 (Known issues, "emoji were
dropped without a trace"). Skribidi lays out a run of emoji
(`skb_emoji_run_iterator`: emoji presentation, VS16, ZWJ, keycap and flag
sequences) with the fonts of `SKB_FONT_FAMILY_EMOJI` only, and any other
run with the fonts of its text family. When the run's family had no font
that matched and no default font, `skb__itemize()` skipped the run. With
the layout glue's fonts all in the default family, "a 😀 b" gave the
items of "a  b", `n_missing` 0, and the pack tool built the pack.

The rule the patch adds:

- Where the run would be skipped, it is laid out in the other family: an
  emoji run in its text family, a text run in the emoji family. The
  codepoints then get that family's glyphs, or glyph 0, which
  `n_missing` counts and `ylay_missing_chars()` names. A run that was not
  skipped is itemized as before.
- The fallback of patch 0003 (a codepoint none of the run's fonts has)
  looks in the run's family, then in the other family. An emoji that the
  emoji fonts lack (U+2764 with VS16, when only a text font has U+2764)
  comes from the text font; a character with text presentation that no
  text font has (U+2603 SNOWMAN alone) from the first emoji font that has
  it, as a browser does.
- In each family the first font, in the order added, that has the
  cluster wins, so the result does not depend on anything but the list.
- 32 added and 3 changed lines in one file.

Outputs that changed: only runs Skribidi skipped before, and codepoints
that had glyph 0 because their family lacked them. A collection without a
COLR font gives the same layouts as with patch 0003 alone, except that a
skipped emoji run now shows as glyph 0. `test_layout` part 8 checks it.

### Patches 0005 and 0006: editing Devanagari

Found 2026-11-18 in the designer's spike
([designer_ui_probe.md](designer_ui_probe.md) part 2, section 3). Both
patches change only code that Skribidi's editor calls: the grapheme
breaks of `skb_text`, and the caret, hit test and selection functions of
a layout. Glyphs, clusters and line breaks do not change, and the pack
tool and the player do not call the changed functions.

Patch 0005, select all and Backspace left a vowel sign:

- Each insert, append or remove of `skb_text` set the grapheme-break
  flags only for the new codepoints, as if they were a separate text. The
  codepoint before the edit kept its old flag, and at the end of a text
  that flag is always a break. An edit in the middle moved the codepoints
  but not their flags.
- Typed one codepoint at a time, "नमस्ते" got a break after ta (U+0924).
  The layout breaks the whole paragraph again and finds three graphemes,
  the last "स्ते" (UAX #29 rule GB9c). Select all ended at that
  grapheme's start, from the layout; the removal took the next break from
  the text's flags, one codepoint short, and U+0947 stayed. Text set as
  one string had the right flags.
- The patch recomputes the flags after each edit with libunibreak, from
  the start of the grapheme that holds the codepoint before the edit to
  the end of the text. An append costs the new codepoints and one
  grapheme; an edit in the middle costs the rest of the paragraph, which
  the layout breaks again in any case.

Patch 0006, the caret after "नमस्ते" was drawn after "नम":

- HarfBuzz gives the clusters [na] [ma] [sa virama] [ta e]: the last
  grapheme spans two clusters. Skribidi's caret iterator took each
  cluster as whole graphemes, so [sa virama], which has no grapheme break,
  got no caret stop and an advance of 0. With Noto Sans Devanagari at
  16 px, the end caret was at x 27.57 of a 33.79 px word; hit tests gave
  an offset inside the grapheme, and a selection of the grapheme divided
  by a grapheme count of 0 (NaN, no rectangle).
- The patch groups neighboring clusters of one run until the group starts
  and ends at grapheme boundaries, steps the caret iterator past the
  group, and selects a cluster with no break whole. Clusters that end at
  grapheme boundaries give the same stops as before.

`test_layout_editor` (`tests/layout/editor_test.c`) checks both with box
fonts it writes, so it runs on every platform, and again with the Debian
Noto fonts when `YSP_NOTO_DIR` or `YSP_EDITOR_TEST_FONTS` names them. On
the Linux desktop with the Noto fonts, 20 of its 68 checks fail with
patches 0001 to 0004 only, 10 with 0005 added, and none with both. Its
part 3 shows that patch 0003 also covers the editor's path: a digit in
Hebrew text comes from the Latin font. The probe's box for U+2192 was
missing coverage (none of its four Noto files maps it), not a fallback
fault.

### Patch 0007: cluster values out of order

The fault, as reproduced 2026-11-18 (Linux gcc 12, HarfBuzz 14.6.0):

- Text: U+202E (RIGHT-TO-LEFT OVERRIDE), then U+094D U+200C U+094E
  (virama, ZWNJ, the pre-base vowel sign prishthamatra), with a font
  that has no U+25CC. The override makes the Devanagari an RTL run.
- HarfBuzz shapes a script against its direction by reversing the
  graphemes, shaping in the script's direction, and reversing back
  (`hb_ensure_native_direction()`). The Indic shaper's reordering then
  moves a glyph across two of the reversed graphemes. The RTL run's
  cluster values come back as 2, 1, 2. HarfBuzz alone gives the same
  values. With a font that has U+25CC, HarfBuzz inserts a dotted circle,
  the syllables differ, and the values are in order.
- `skb__shape_run()` takes the text range of a cluster from the values of
  the glyphs next to it, so it expects the values in order (descending in
  RTL). The last cluster got the range [2, 1).
- Debug build: `assert(text_end >= text_start)` in `skb__shape_run()`
  fails. Release build: the cluster's text count is clamped to 0, so its
  glyph maps to no codepoint, carets and hit tests are wrong there, and
  code that reads a cluster's last codepoint
  (`text_offset + text_count - 1`) reads the codepoint before the
  cluster. Under ASan with asserts off, no memory error came from it.

The patch adds `skb__merge_non_monotone_clusters()`, which runs on
HarfBuzz's output before the cluster loop:

- When the values are in order, one pass and no allocation: the clusters
  do not change. Compared with and without the patch (Release; glyph ids,
  offsets, advances, clusters and runs), 220,006 random layouts of bidi
  controls with Devanagari, Arabic, Hebrew and Latin (the Noto fonts, and
  box fonts) differ in 22, each one whose clusters were wrong without it.
- Else, it merges the glyphs into the fewest clusters whose values are in
  order in the run's direction, each with the lowest value in it, as
  HarfBuzz merges clusters. It allocates one `uint32_t` per glyph from the
  layout's temporary allocator. The case above becomes one cluster of
  U+094D U+200C U+094E with its 3 glyphs.
- The function does not look at the direction of the script, so a left-
  to-right override (U+202D) before Arabic or Hebrew gets the same
  treatment. HarfBuzz gave no values out of order for 8,000,000 random
  Arabic and Hebrew runs forced to LTR with box fonts, and 600,000 with
  Noto Sans Arabic and Hebrew; for Devanagari forced to RTL it gave 4 in
  2,000,000 with a box font and 0 in 300,000 with Noto Sans Devanagari.

Checks:

- `test_layout_editor` part 5 (box fonts the test writes, none with
  U+25CC): each bidi control (U+202A to U+202E, U+2066 to U+2069, U+200E,
  U+200F), before and after the first codepoint of 6 texts (Devanagari,
  Hebrew, Arabic, mixed), at the 3 base directions: 414 layouts. Every
  codepoint must be in exactly one cluster and no cluster may be empty;
  Hebrew and Arabic keep one cluster per codepoint; the fault's text gives
  the clusters [U+202E] [U+094D U+200C U+094E]. The editor types U+202E
  U+094D U+200C U+094E, a space and ka, moves the caret, and removes the
  text with Backspace, with the same check after each edit. Without the
  patch, Release: 6 of 85 checks fail (12 of the 414 layouts, all with
  U+202E, and 5 of the editor's layouts); Debug: the assert stops the
  test. With it: 81 of 81 pass (the Noto fonts present), also under ASan
  and UBSan.
- `tests/fuzz/layout_fuzz.c` (CMake target `layout_fuzz`, self-driven):
  text with bidi controls, Devanagari, Arabic and Hebrew through
  `ylay_layout()`, through Skribidi's layout with the same cluster check,
  and through Skribidi's editor with key presses. Its 6 seeds hold the
  fault; ctest `layout_fuzz_seeds` runs them. Without the patch the first
  seed fails (Release: the cluster check; Debug: the assert). With the
  patch, 4 runs of 300,006 inputs (2,076,458 editor keys) in the
  sanitizers tree (Debug, ASan and UBSan, `setarch -R`): no report.
- Hand mutants of the new function (Release, `test_layout_editor`): 6 of
  7 killed. The survivor takes the last value as the bound of the glyphs
  before it, not the lowest so far (RTL) or the highest (LTR); it differs
  only for values such as 5, 2, 4, 3, 1, which no input here made
  HarfBuzz give.

The cost of the extra pass was not measured.

### Patch 0008: the editor at its text's edges

Found 2026-11-18 by `tests/fuzz/layout_fuzz.c` (Known issues), and
2026-11-19 by commands on small texts: 12 texts (none, empty, one
character, Hebrew, a space, LF, U+2029, two LFs, text with a separator at
either end, two letters), each key with each modifier set, select all,
pastes, double and triple clicks and an IME composition, from each caret
position, in both behaviors. Linux gcc 12, Debug with ASan and UBSan, and
Release. The pack tool and the player do not call the editor; the
designer's text fields do. The faults and their causes:

1. An editor with no paragraph. `skb_editor_create()` and
   `skb_editor_reset()` leave the editor with no paragraph until text is
   set or inserted, and an undo of the first edit leaves none again.
   `skb_editor_select_all()`, the mouse click and the caret's position
   check for this; other paths read paragraph 0 of none: an assert in a
   Debug build, a read through NULL (a crash) in a Release one.
   - A caret move: Left, Right, Up, Down, Home, End, with any modifier
     (`skb_rich_layout_get_direction()` or `skb_rich_layout_get_layout()`).
     A new designer text field reaches it with its first arrow key.
   - `skb_editor_get_line_index_at()` and `skb_editor_get_column_index_at()`.
   - The first IME composition, `skb_editor_set_composition_utf32()`,
     through `skb_editor_get_selection_ordered_start()`.
   - `skb_editor_iterate_text_range_bounds()`, the selection's
     rectangles: it read paragraph -1 (UBSan; ASan after an undo).
2. Word boundaries read text properties outside the text.
   - `skb__advance_word_forward()` (Ctrl+Right, and Ctrl+Left in RTL text,
     in the default behavior) read the property after the last grapheme
     when the last word ends at the text's end: "ab", Ctrl+Right (ASan).
   - `skb_editor_get_word_start_at()` (a double click) read the property
     before offset 0 (ASan).
3. `skb_text_append_utf32_with_payload()` called `memcpy()` with a NULL
   destination and a size of 0. A `skb_text` that never held a codepoint
   has no buffer, and Skribidi appends an empty text to each empty
   paragraph it makes: Enter, text set or inserted that ends in LF or
   starts with a separator, and `skb_editor_set_text_utf8()` with "" (UBSan;
   the copy does nothing, but a compiler may assume the pointer is not
   NULL).

The fixes, one patch because each is one to four lines in the editor's
edge cases, found and checked together:

- No paragraph: `skb_editor_process_key_pressed()` ignores a caret move;
  Backspace, Delete and Enter run as before (Enter makes the first two
  paragraphs). The line and column index are 0. The two ordered-selection
  functions give the range's own start and end, so the first composition
  starts at offset 0. The selection bounds call back nothing, as
  `skb_rich_layout_get_caret_info_at()` already returns early.
- Word boundaries: the forward move stops at the text's end, and the
  word-start loop stops at offset 0. Both give the offsets they gave
  before; they no longer read outside the text.
- The empty append skips the copy, as `skb_text_insert_utf32_with_payload()`
  already does.

Checks:

- `test_layout_editor` part 6 (box fonts the test writes): the moves of an
  editor that never had text, Enter in it, Ctrl+Right in "ab" and "a b",
  Ctrl+Left in Hebrew, a double click on each letter of "ab", text that
  ends in LF, Enter after "a", U+2029 and a letter inserted before "a";
  then 6,478 sessions of 79 commands. Each
  session runs a command twice and requires a paragraph per separator and
  one more, the selection inside the text, clusters that tile each
  paragraph, a caret that is a number, readable selection bounds, and an
  unchanged text after a move.
- `tests/fuzz/layout_fuzz.c`: the skips are gone. The editor step inserts
  the whole text, or none (input bit 5: the text comes only from a paste
  command), in either behavior (bit 4), and runs keys with Shift, Ctrl,
  Option and Command, select all, pastes, undo, double clicks and IME
  compositions. 6 new seeds hold the faults.
- Each fix reversed alone, the others in place (sanitizers tree, Debug,
  ASan and UBSan with `halt_on_error=1`): no paragraph: the assert stops
  `test_layout_editor` and `layout_fuzz 0` (in Release both crash); the
  forward word move: ASan stops both in `skb__advance_word_forward()`;
  the empty append: UBSan stops both in
  `skb_text_append_utf32_with_payload()`; the word start: ASan stops both
  in `skb_editor_get_word_start_at()`. With the patch: 95 of 95 checks
  pass (Release, and under ASan and UBSan), and the fuzz target ran
  8 processes, 5,200,096 inputs and 16,821,438 editor commands, with no
  report (sanitizers tree, `setarch -R`, `halt_on_error=1`).
- Hand mutants of the patch (sanitizers tree): 6 of 7 killed. The
  survivor makes the ordered start of an editor with no paragraph give
  the range's end; its selection is empty there, so the two are equal.
- Without a sanitizer, the second and third faults change no result: the
  word move gives the same offset, and the copy is of nothing. Their
  checks fail only under ASan and UBSan. CI's sanitizer job does not build
  the layout, so only a local sanitizer run catches them.

### Patch 0009: End on each line but the first

Found 2026-11-19 by hand in the designer, whose editor holds a JSON
definition as one paragraph per line, each line ending in LF. End put the
caret at the end of the first line, and one character past the end of
each later line: after the LF. The pack tool and the player do not call
the editor.

The cause. `skb_editor_get_line_end_at()` makes the caret at the line's
last grapheme as a global offset (the paragraph's start plus
`line->last_grapheme_offset`) and gives it to
`skb__caret_prune_control_eol()`. That function keeps a caret off a
line's last control character (the LF or U+2029 that ends a paragraph):
it compares the caret's offset with `line->last_grapheme_offset`, an
offset in the paragraph, and moves the caret to the character's
trailing edge. The two offsets are equal in paragraph 0 only. In each
later paragraph the caret stayed on the LF's leading edge, after it.
Effects on each line but the first:

- End: the caret was drawn one LF advance past the line's end (box fonts
  at 16 px: x 48 for "cd", not 32), and a letter typed there went to the
  start of the next line.
- Shift+End, Command+Right and Shift+Command+Right on macOS, a triple
  click and a drag by lines call the same function: they selected the LF
  too.

Callers that pass an offset in the paragraph were right: the hit test
(a click past a line's end, Down and Up into a shorter line) and
`skb_layout_get_line_end_at()`. Home does not prune. Lines that do not end
in a control character (the first parts of a soft-wrapped paragraph, the
last paragraph) need no prune.

The fix: the function gives the prune the offset in the paragraph, then
adds the paragraph's start. Paragraph 0 and lines that do not end in a
control character get the caret they got before.

Checks (Linux gcc 12):

- `test_layout_editor` part 7 (box fonts the test writes): "ab\ncd\n";
  Hebrew lines with LF in paragraphs 0 and 2 around an LTR line, and a
  last Hebrew line with none; and "ab cd ef\nab cd ef\n" wrapped at 40
  px (3 lines a paragraph). On each of their 14 lines, in both behaviors:
  End from the line's start gives the end of the visible text, the caret
  at the outer edge of the last visible character (x and baseline), and a
  letter typed there at that offset; Shift+End selects the visible text;
  Home from before the last character goes to the line's start and its x;
  a click past the end (left of an RTL line) and a triple click give the
  end and the line. Then Command+Right and Command+Left on macOS, a triple
  click and a drag by lines over two lines, and Down and Up through lines
  of 4, 2, 4, 1, 0 and 3 characters, in both behaviors.
- Without the patch 7 of 110 checks fail: End's offset, its caret, the
  typed letter, Shift+End and the triple click, each on 8 of 28 lines (the
  lines that end in LF in paragraphs after the first); Command+Right; the
  drag by lines. With it, 110 of 110 pass, in Release and under ASan and
  UBSan. Home, the click past the end, and Down and Up pass both ways.
- Hand mutants of the patch: 5 of 5 killed (the old code; the
  paragraph's start added before the prune too; no prune; `=` for `+=`;
  the prune's offset taken without its affinity).
- The fuzz target cannot find this fault: the caret stays inside the
  text, nothing reads out of bounds, and the fuzz target has no oracle of
  where End must go. No seeds were added.

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
| `ylay_add_font(L, bytes, n, face, name)` | A font, in fallback order; any face of a collection. Opens it with HarfBuzz and `ysp/outline.h`, hashes it, makes an empty curve set. A font `ysp/outline.h` refuses (CFF2, no outlines) is refused by name. A COLR font goes in the emoji family (v0.3.0, [Emoji](#emoji)); a font whose color glyphs are CBDT, sbix or SVG with no COLR is refused (`YLAY_ERR_REFUSED`) |
| `ylay_layout(L, text, n, style, spans, n_spans, block)` | Lays out UTF-8 text: size, width (0: no wrapping), line height, direction, alignment, language, a palette color; spans change size, color and language |
| `ylay_take_new_glyphs(L, font, &glyphs)` | The glyphs built since the last call, for `ygfx_cset_add()` |
| `ylay_from_skb(L, layout, x, y, color, append, block)` | The same conversion for a Skribidi layout the caller made (the editor's paragraphs) |
| `ylay_stamp(L, block, buf, cap)` | The versions and font hashes as one line |
| `ylay_missing_chars(L, out, cap)` | The characters of the last call's glyph-0 items, in text order, with their runs' ISO 15924 scripts; `in_a_font` tells a character no font maps from one a font maps (v0.2.0) |
| `ylay_skb_fonts`, `_attributes`, `_temp` | Skribidi's objects, for its editor |
| `ylay_color_glyph(L, font, glyph, &layers)` | A color glyph's layers (set glyph, paint texel index), or `YLAY_ERR_REFUSED` with the reason (v0.3.0) |
| `ylay_color_paints(L, font, &texels)` | The font's paint records, the first texels of its set (v0.3.0) |
| `ylay_open_set`, `ylay_build_glyphs` | A set built as the library builds the font's: paints first, then font and layer glyphs; the pack tool's CURVESET (v0.3.0) |

### Font fallback

Fonts are tried in the order added. For each run of one script, the fonts
that support the script, and the first that maps the codepoint
(Skribidi's rule). For a codepoint none of them maps (digits and
punctuation take the script of the text around them), the first font of
all that maps its whole grapheme cluster, else the codepoint (patch 0003).
So [Noto Sans Hebrew, Noto Sans] and [Noto Sans, Noto Sans Hebrew] both
give "שלום 12:30, 4.5%" the Hebrew font's letters and spaces and Noto
Sans's digits and punctuation.

Skribidi gives a font a script only when a range of the font's cmap starts
or ends with a character of that script (it samples the ends of each
range). Real fonts pass; a font made for a test needs a range that starts
or ends with a letter (see `tests/layout/layout_test.c`). The fallback
looks at the cmap itself, so a font whose scripts the sampling misses is
still found for the codepoints it has.

### Emoji

A font with a COLR table (version 0 or 1, as HarfBuzz reads it) goes in
Skribidi's emoji family (v0.3.0); every other font stays in the default
family. `ylay_font_info.color` says which COLR a font has (1: version 0
layers, 2: version 1 paints).

- An emoji run (emoji presentation, a VS16 sequence, ZWJ, keycap and flag
  sequences, as Skribidi's emoji iterator finds them) takes the COLR
  fonts in the order added; a codepoint they lack, the text fonts
  (patch 0004). A ZWJ sequence is shaped in one font, so the emoji
  font's GSUB makes its ligature.
- A text run takes the text fonts as before; a codepoint they all lack,
  the COLR fonts in the order added. A character with text presentation
  (U+2603 SNOWMAN without VS16) that only the emoji font has is drawn from
  it.
- No emoji font: an emoji is glyph 0, counted in `n_missing` and named by
  `ylay_missing_chars()`. Emoji fonts only: text is glyph 0 the same way.
- Refused by name (`YLAY_ERR_REFUSED`): a font whose color glyphs are CBDT
  or sbix bitmaps or SVG documents and that has no COLR (Noto Color Emoji's
  CBDT build, Apple Color Emoji), with the advice to use Twemoji. A font
  with COLR and CBDT or SVG (the Fluent builds) is accepted; its COLR is
  used. A COLR table that HarfBuzz rejects is refused (`YLAY_ERR_FORMAT`).

### Color glyphs

`ylay_add_font()` flattens every color glyph of a COLR font (v0.3.0), so
a layout gives an emoji as layers that `ysp/gfx.h` draws, and the pack
stores them (pack.md 18). The decisions are the user's of 2026-11-18
([emoji_colr_survey.md](emoji_colr_survey.md), "Decisions").

- Each glyph that COLR has (version 1 paint, or version 0 layers) is
  painted once with `hb_font_paint_glyph()`, palette 0, at a scale of
  units per em, so every coordinate is in font units. Callbacks keep a
  stack of transforms, clips and groups and collect the fills.
- A fill becomes a **layer**: a clip outline and a paint.
  - The outline is the clip glyph under the transform in force at the
    clip, baked into its points (an affine map of a quadratic is exact),
    in em with y down. With no transform it is the font's own glyph; else
    a **layer glyph**, an id past the font's glyphs in the font's set,
    one per distinct glyph, transform and cut. A clip box (the ClipList
    rectangle at the root, under an axis-aligned transform) that cuts the
    outline's control-point box is cut into it: each contour clipped to
    the four half-planes, the parts outside replaced by the boundary
    line, then resolved; a box that does not cut adds nothing.
  - The paint is a **paint record** of texels (gfx.md, "Paint records"):
    a solid; a linear gradient as an affine function t of the em point
    (COLRv1's p0, p1 and the normal to p2 - p0, through the inverse of
    the gradient's own transform); a radial gradient as a 2x3 matrix to
    the space where the end circle is the unit circle about the start
    point, and the start point's offset d, so t solves
    |q - t d| = t. Up to 16 stops, sorted stably by offset (equal offsets
    are a hard edge), pad. Colors are CPAL's sRGB codes decoded to linear
    light (IEC 61966-2-1) and premultiplied, as gfx.h blends, in f32.
- Group opacity (nanoemoji's SVG opacity: PaintComposite SRC_IN over a
  backdrop of one solid with no glyph clip) becomes the solid's alpha on
  each layer of the source, when no two of those layers overlap: each
  pair's areas against the area of their union, resolved (overlap above
  1e-7 of the two areas refuses the glyph). SRC_OVER composites draw in
  order.
- Alpha reaches the flattening as HarfBuzz's 8 bits (`hb_color_t`): a
  PaintSolid alpha of 0.5 (F2DOT14 8192) is 128/255 = 0.50196. Within
  1/510 of the font's value.
- Records are deduplicated by their bytes, layer glyphs by glyph,
  transform and cut. Every record is written into the font's set at open,
  first, in glyph id order, so the indexes depend on the font alone: the
  pack tool's set (`ylay_open_set()`) has the same ones.
- A second, independent walk of the COLR table counts each glyph's paint
  graph the way HarfBuzz recurses (an edge per paint, depth from the
  root), and refuses a glyph that reaches 64 levels or 16,384 edges,
  where HarfBuzz stops without an error; it also finds the PaintVar
  formats. HarfBuzz's sanitizer already refuses a chain of more than 64
  paints (the whole COLR, and the font with it); a chain through the
  LayerList passes it, and the walk catches that.
- A gradient that COLR calls ill-formed (p1 or p2 at p0, p0p2 parallel
  to p0p1 within 1e-6 of the sine, flattened by a singular transform, or
  a radial with both radii 0) "must not be rendered": its layer is left
  out and the glyph's other layers draw (`test_layout` part 9's p2 = p0
  case).
- What the layers cannot draw exactly is refused per glyph; a layout that
  uses the glyph fails with `YLAY_ERR_REFUSED`, naming the font, the
  cluster's codepoints, the glyph, the feature and what to use instead.
  The list is in [pack.md](pack.md#what-the-tool-refuses) 18.
- A layout puts each color glyph's layers, in order, in a run of their
  own (`ylay_run.flags` `YLAY_RUN_PAINT`): one per font and size, so a
  page of emoji is one draw. The items' `color` is the record's texel
  index; draw the run with `YGFX_I_PAINT`.

### The default emoji font

Twemoji Mozilla v0.7.0 (the user's decision, 2026-11-18): COLR version 0,
3,689 color glyphs, no gradients and no transforms, so every glyph draws
exactly (0 refused; 33,179 layers, no layer glyph of its own), and the
smallest pack of the fonts measured (pack.md 18). "Default" means:

- `tools/fetch_twemoji.py` fetches the font and its `LICENSE.md` at tag
  v0.7.0 and writes them only when their SHA-256 values match the ones in
  the script (`6d90152e...` for the font). The repository does not hold
  the font: CC BY 4.0 would allow it, but the repository keeps third-party
  binaries out, as `third_party/` does.
- `examples/pack/emoji/` is a pack source that uses it.
- The refusal messages and these docs name it as the font to use.
- Nothing picks it without being asked: a font list holds what the caller
  gives, and a missing emoji is glyph 0, counted.

A pack that embeds it carries the attribution CC BY 4.0 asks for: who
made the art (Twemoji, Twitter, Inc and other contributors), the license
and its link, and whether it was changed. `examples/pack/emoji/ATTRIBUTION.txt`
is one such text: the art unchanged, the pack's curve set a conversion of
the glyphs for drawing; with the font's `LICENSE.md` (Mozilla's build
code, Apache-2.0) as a FILE entry beside it.

Blending: layers are drawn in linear light, as everything in ysp/gfx.h.
A browser blends sRGB-encoded values, so translucent layers and the
antialiased edges between layers come out lighter or darker than in
Chrome or Firefox; opaque interiors and the stops' own colors match
(gfx.h PAINT RECORDS).

A character that no font in the list maps still gets glyph 0.
`ylay_missing_chars()` names the characters of the last call's glyph-0
items, each with its run's script, so the pack tool can say which
characters are missing and from which text.

## Checks

`tests/layout/layout_test.c` (ctest `test_layout`):

1. The logged versions: the Skribidi commit, the hashes (and no
   `tree-differs`) of the nine patches, HarfBuzz 14.6.0.
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
7. Font fallback (v0.2.0), with fonts the test writes, one script each, so
   no system font is needed: "שלום 123", "123 שלום", "שלום 12:30, 4.5%"
   and "مرحبا 123، 4.5%" with [script font, Latin font] in both orders give
   no glyph 0, the letters, spaces and U+060C in the script's font and
   their glyph ids, the digits and Latin punctuation in the Latin font and
   theirs, one baseline, and the line with the taller Latin font's ascent
   and descent; the first two keep the bidi order of part 2; Hebrew alone
   keeps the Hebrew font's metrics. A numeral and an acute, and a numeral
   and a Hebrew point the run's font has, move whole to the first font
   with both, and the numeral after them comes from the first font that
   has it; an alef whose acute only another font has moves whole when
   that font has the alef too, else only the acute moves; a ZWJ between
   Arabic letters stays in the Arabic font. A snowman in Hebrew and in
   Latin text, and an acute no font has, are glyph 0, and
   `ylay_missing_chars()` names them with "Hebr" or "Latn"; the list
   empties with the next layout.

   Without patch 0003 (its three files reverted, 2026-11-18, Linux gcc 12):
   36 of the 81 checks failed. Every text of the first group had glyph 0
   in both font orders (3, 3, 10 and 7 glyphs; the line kept the script
   font's 80 and 20), the numeral and alef clusters stayed in the run's
   font with glyph 0, and "שלום ☃ 12" had 3 glyphs 0 and 3 characters
   listed, the digits with the snowman. The ZWJ check passed: it guards
   against a fallback that splits the run, a fault of the patch, not of
   Skribidi.

8. Emoji (v0.3.0), with fonts the test writes
   (`tests/adapt/colr_font_build.h`: a Latin box font and a COLR version
   0 emoji font with a GSUB ligature for a ZWJ sequence), so no system
   font is needed. In both font orders: "a😀b" takes the grin from the
   emoji font and the letters from the text font, with no glyph 0; "a☃️b"
   (VS16) and "a☃b" (text presentation; only the emoji font has it) take
   the snowman from the emoji font; "x👨‍👩" is the ligature glyph. With no
   emoji font, the grin is glyph 0, `n_missing` 1, and
   `ylay_missing_chars()` names U+1F600 as in no font; with the emoji font
   only, both letters are glyph 0 and counted. With two emoji fonts the
   first added gives the grin, in either order. "a❤️" with a text font that
   has U+2764 and an emoji font that lacks it takes the heart from the text
   font. Two layouts of one text give the same items. A font with a CBDT,
   sbix or SVG table and no COLR is refused with a message that names
   Twemoji; the COLR font's `color` is 1.

   Without the change (fonts in the default family, patch 0004 reverted
   and the refusal off; 2026-11-18, Linux gcc 12): 14 of the 99 checks
   failed: the emoji vanished ("a😀b" gave 2 items, `n_missing` 0), the
   ZWJ sequence was 1 item of glyph 3, text with only the emoji font gave
   no items, the two-font order and the heart checks failed, and the three
   refusals were accepted.

9. Color glyphs (v0.3.0), with a COLR version 0 font and a COLR version
   1 font of 21 cases, both written by the test. Version 0: the grin's
   two layers, its yellow record (kind 1, linear-light values of
   0xFFCC22, alpha 1) and the snowman's half alpha, premultiplied; no
   layer glyph. Version 1: the solid case (alpha 128/255); a linear
   gradient's t at three points; a three-stop radial's t and its middle
   stop; a two-point radial at its focal point and on its end circle; an
   elliptical radial (the gradient scaled 2 x 1 inside an unscaled box)
   at four points; group opacity over two boxes apart as alphas 128/255
   and 204/255 of that; the set (`ylay_open_set()`, every glyph) passes
   `ygfx_cset_check()` and starts with the paints; a translated box, a
   box turned by COLR's 30 degrees (2731/16384 half turns) and a box cut
   by its clip box are layer glyphs with the boxes they should have
   (within 1e-6 em); a second flattening gives the same records and
   layers; a layout of an emoji gives one paint run and builds its layer
   glyph. The twelve refused cases each name their feature, and a
   layout of the soft light case names U+E010 and glyph 16. Two more
   cases: ill-formed gradients (p2 = p0, both radii 0) leave their layers
   out and keep the glyph's disc, and a radial under a skewed affine is
   right at its mapped center and circle.

   Hand mutants (`tests/probe/colr/hand_mutants.py`; the mutation runner
   cannot build the layout library or the tool), 2026-11-18, Linux gcc 12:
   11 of pack/colr.c (the overlap test, the em transform's signs, the clip
   box cut, the walk's depth, alpha, the PaintVar check, the radial map's
   off-diagonal terms, both radii 0, ill-formed linear gradients, the
   group-opacity backdrop, a composite mode), 3 of pack/layout.c (the
   emoji family, the refusal in a layout, the paint records at the set's
   start) and 6 of the tool (GLYPHRUNS version 2, pack format 2, dropping
   tables, refusing CBDT, the run's paint flag, layer glyphs in a used
   set): 20 of 20 killed, two (the radial map, both radii 0) after the
   test gained the last two cases.

   The fuzz target `tests/fuzz/colr_fuzz.c` (CMake target `colr_fuzz`,
   self-driven: mutations of the two written fonts, a third of them
   inside the COLR table) checks every accepted font's layers, records
   and set (2026-11-18, Linux gcc 12): 300,002 inputs under ASan and
   UBSan (the sanitizers tree, Debug, `setarch -R`), no report, 461,031
   color glyphs flattened and 77,537 sets built and checked; 300,002 more
   in Release with a layout each. Its first run found a layer whose clip
   glyph is past the font's glyphs (HarfBuzz passes COLR's glyph ids on
   unchecked), which gave a layer glyph past the set; such a glyph is now
   refused as a damaged font.

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

### Open: at a soft wrap, the caret after End counts as on the next line

Found 2026-11-19 with patch 0009, the same in each paragraph (not the
fault of patch 0009). On a soft-wrapped line, End puts the caret at the
line's last grapheme with EOL affinity ("ab cd ef" at 40 px: offset 2,
the space, drawn at x 48 on line 0). The editor resolves that caret with
its affinity to offset 3, the next line's start: Home then goes to the
start of line 1 (offset 3), not line 0; a second End goes to the end of
line 1; `skb_editor_get_line_index_at()` says line 1. Down and Up, and
the caret's drawing, take it as on line 0. The same holds for a caret on
the leading edge of a wrapped line's last character. Not patched: it comes
from the caret's affinity, not from its offset, and a fix changes which
line Skribidi takes a caret to be on, which other paths read. A text
field that does not wrap does not reach it; this repository does not
hold the designer's editor, so whether it wraps is not checked here.

### Open: an editor with no paragraph draws no caret and no composition

Found 2026-11-19 with patch 0008. An editor that never had text (and one
whose first edit was undone) has no paragraph, so
`skb_editor_get_caret_info_at()` gives a caret of height 0 (ascender and
descender 0, box fonts at 16 px) and an IME composition is not laid out
(no paragraph holds it). After `skb_editor_set_text_utf8(editor, temp,
"", 0)` the editor has one empty paragraph: the caret has the font's
height (ascender -12.80, descender 3.20 px) and every command works. A
text field should set its empty text after `skb_editor_create()`.

### Open: `skb_rich_text_append_paragraph()` does not end the last paragraph

Its comment says it ends the current last paragraph with LF, but it
appends 0 codepoints, so two paragraphs follow with no separator between
them. The editor calls it only on an empty rich text (`skb_editor_set_text_utf8()`,
the scratch text of an input), where it has no effect; a caller that
builds a rich text of several paragraphs with it gets them joined. Not
patched: no path of the designer or the pack tool reaches it.

### Fixed by patch 0009: End past the LF on each line but the first

Found 2026-11-19 by hand in the designer: End was right on the first line
of a definition and one character past the end on each later line.
Patch 0009 fixes it, and Shift+End, Command+Right, triple clicks and
drags by lines, which had the same cause
([Patch 0009](#patch-0009-end-on-each-line-but-the-first)).

### Fixed by patch 0008: faults of Skribidi's editor on empty texts and at a text's ends

Found 2026-11-18 by `tests/fuzz/layout_fuzz.c` and confirmed on plain
text (pinned Skribidi with patches 0001 to 0007): Ctrl+Right read one past
the text's properties ("ab"; ASan), a key press in an editor that never
had text read paragraph 0 of none (an assert in a Debug build, a crash in
a Release one), and Enter passed NULL to `memcpy()` (UBSan). Patch 0008
fixes them and four faults of the same kinds that commands on small texts
found ([Patch 0008](#patch-0008-the-editor-at-its-texts-edges)).

### Fixed by patch 0007: a right-to-left override before Devanagari

Recorded 2026-11-18 as: pinned Skribidi hangs or crashes on text that
starts with U+202E followed by Devanagari, and with asserts on,
`skb__line_append_shaping_run()`'s assert fails. The fault reproduced
here asserts in `skb__shape_run()` instead
([Patch 0007](#patch-0007-cluster-values-out-of-order)); no hang, crash
or `skb__line_append_shaping_run()` assert came up in about 800,000
random layouts (bidi controls with Devanagari, Arabic, Hebrew and Latin;
box fonts, the Debian Noto fonts, and DejaVu Sans first in the list;
Debug, and ASan with asserts off) and 18,000 random editor sessions. If
the recorded symptom had another input, that input is not known.

### Fixed in v0.3.0: emoji were dropped without a trace

Skribidi lays out a run of emoji with the fonts of its emoji family
(`SKB_FONT_FAMILY_EMOJI`) only, and when there was none it skipped the run.
`ylay_add_font()` added every font to the default family, so "a 😀 b" gave
the items of "a  b": no glyph 0, `n_missing` 0, and the pack tool built
the pack. Seen 2026-11-18 with DejaVu Sans, and with Noto Sans and Noto
Color Emoji (CBDT, which `ysp/outline.h` refused as bitmap only). v0.3.0
puts COLR fonts in the emoji family and refuses CBDT, sbix and SVG fonts
without COLR by name ([Emoji](#emoji)); patch 0004 lays a run whose family
has no font out in the other family, so an emoji no font has is glyph 0
and counted.

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

### The fallback's cost (v0.2.0)

Measured 2026-11-18 on the Linux desktop (i5-14500, 6 P-cores and 8
E-cores; gcc 12.2, the Release build with `-O3`), each program pinned to
P-core CPU 4 with `taskset` and holding `/tmp/psy-measure.lock`, nothing
else running; load average (1 min) 1.19 to 1.44, falling from a build. The
unpinned runs were bimodal: the scheduler put whole rounds on an E-core
(the English label 7.2 us there, 4.3 us on a P-core), so only pinned runs
count. Three runs of v0.1.0 (HEAD's glue and Skribidi with patches 0001
and 0002) and v0.2.0, interleaved; each number is one run's median of 3
rounds, as layout_bench prints it. `layout_bench` with Arial as
`segoeui.ttf` and Droid Sans Fallback as `msyh.ttc` (none of its texts
needs a fallback); the Hebrew rows from a program with the same method,
fonts Noto Sans Hebrew and Noto Sans, or DejaVu Sans alone.

| Item | v0.1.0, us | v0.2.0, us |
|---|---|---|
| Label, English | 4.3, 4.3, 4.3 | 4.3, 4.4, 4.3 |
| Label, Arabic | 4.9, 4.9, 4.9 | 4.9, 4.9, 4.9 |
| Paragraph, English, 600 px | 59.5, 59.9, 59.8 | 59.1, 59.4, 59.2 |
| Paragraph, Arabic with a Latin sentence | 94.5, 94.5, 94.5 | 94.3, 94.3, 94.2 |
| Keystroke at 500, English | 69.7, 69.6, 69.9 | 68.8, 69.1, 68.8 |
| Keystroke at 500, Arabic | 106.1, 106.1, 105.9 | 106.1, 106.4, 105.6 |
| Label "שלום שלום שלום", [Hebrew, Latin] | 3.12, 3.10, 3.10 | 3.13, 3.13, 3.13 |
| 516 codepoints of Hebrew with numbers, DejaVu Sans | 88.06, 87.49, 87.75 | 87.36, 88.16, 87.68 |
| Label "שלום 12:30, 4.5%", [Hebrew, Latin]: 10 fallback codepoints | 4.47, 4.47, 4.46 (glyph 0) | 4.99, 5.02, 5.02 |
| The 516 codepoints, [Hebrew, Latin]: 84 fallback codepoints | 98.63, 98.07, 98.48 (glyph 0) | 112.86, 113.52, 113.22 |

- Text that needs no fallback: no change outside the run-to-run spread,
  but the Hebrew label, 0.01 to 0.03 us (up to 1 %) slower in each pair.
  By instruction count (`perf stat`, 20,000 layouts): 137 more a label
  (0.24 %) and 2886 more for the 516 codepoints (0.18 %).
- The first build of the patch inlined the fallback helper into the font
  split loop: the DejaVu paragraph then took 0.4 to 0.7 us (0.5 to 0.8 %) more,
  in every pair. Kept out of line it is within the spread above. The glue
  first noted glyph-0 characters inside its conversion loop, 1 % of the
  same paragraph; it now does a second pass only when a glyph is 0.
- Text that needs the fallback: 0.05 to 0.18 us more for each codepoint that
  falls back (each looks up every font of the collection and adds a run
  to shape). Before, that text had glyph 0 and the pack tool refused it.

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

- Vertical text (Skribidi has none), justification. Explicit bidi
  controls (U+202A to U+2069) are checked for whole clusters only
  (patch 0007's checks), not for their order against a browser's.
- Variable-font instances and CFF2 through HarfBuzz's draw API into
  `ysp/outline.h`'s builder (layout_probe.md 8.4): a font `ysp/outline.h`
  refuses is refused here.
- Decorations (underline, the IME composition's underline), Skribidi's
  icons and inline objects: the glue converts text runs only.
- Color glyphs the layers cannot draw exactly (Color glyphs): offscreen
  groups (SOFT_LIGHT, group opacity over overlapping layers) are a later
  addition; sweep gradients, extend repeat and reflect, the foreground
  color, variable COLR and PaintColrGlyph are refused by name.
- A damaged cmap or GSUB can give a shaped glyph id above 0xFFFF:
  Skribidi asserts on it in a Debug build (`skb__shape_run()`, found by
  `tests/fuzz/colr_fuzz.c` with its layout step) and keeps the low 16
  bits in a Release build; the glue drops an id past the font's set, so
  the result is a wrong glyph or none.
- Serializing blocks into the pack: the pack tool's job, next.
- The designer's path (Skribidi in a browser page with composition events
  and a hidden text input) and the editor's IME with a real input method:
  `examples/layout/gfx_layout.c` drives the editor's composition calls with a
  script in `--sim`; a person with an IME has not run it.
- Untrusted-font robustness of HarfBuzz and Skribidi (no fuzzing here;
  `ysp/outline.h`'s reader is fuzzed).
