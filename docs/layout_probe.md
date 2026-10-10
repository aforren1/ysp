# Text layout probe, 2026-10-07

This note records a probe that compared three text layout candidates for
the pack tool, the player (native and WebAssembly) and the designer
(browser, with text editing): Skribidi, kb_text_shape, and HarfBuzz with
our own layout code. It also records a probe of HarfBuzz's GPU glyph
renderer, hb-gpu, against `ysp/gfx.h`'s curve runs (section 8). The plan
that asked for both is [rig_spec.md](rig_spec.md) section 5.2 (Font row),
section 6 and section 7. The probe code was in a session scratchpad and is
not kept; the numbers below are the record. The corpus texts and the
Chromium reference were lost with it; on 2026-10-07 a new corpus of the
same items and a new Edge reference were made by the same method and kept
in `tests/layout/` (docs/layout.md, Checks).

## Recommendation

1. **Use Skribidi, pinned to a commit, with a one-line local patch.** At
   commit `dee63d6` it has two bidi bugs that Chromium does not have: it
   puts a run on the wrong side of the line, and it drops the glyphs of an
   RTL word between an LTR word and a number. Both come from one merge
   condition in `skb_layout.c`. With the patch (section 2.3), Skribidi gives
   the same glyph ids as Chromium on every horizontal line of the corpus,
   with positions within 0.7 font units, and Skribidi's own unit tests
   pass. It is the only candidate with paragraph layout, Thai and Japanese
   phrase breaking, and an editor (caret, selection in bidi text, IME
   composition, undo) in C. It builds as C17 with MSVC, MinGW gcc and emcc,
   and gives the same output bit for bit on all three.
2. **Do not use kb_text_shape.** Its shaping equals HarfBuzz's on every
   script tested, but it has no paragraph layout, no editing, no embedding
   levels in its run output, no explicit bidi controls, and no Thai or
   CJK dictionary. Its own header says "DO NOT use it on untrusted font
   files", and packs come from other labs. It keeps a full copy of each
   font per context (20 MB for Microsoft YaHei). On 500-codepoint
   paragraphs it is 1.9 to 6.2 times slower than the HarfBuzz glue (MSVC).
3. **Keep "HarfBuzz alone" as the fallback.** Our glue (about 200 lines) on
   HarfBuzz 14.6, SheenBidi and libunibreak matched Chromium on every
   shaped line and on every line break that needs no dictionary. It is
   the fastest candidate. It has no Thai or Japanese phrase breaking, no
   justification and no editing; Skribidi's editor and rich text are about
   6600 lines.
4. **Keep `ysp/gfx.h`'s renderer. Do not adopt hb-gpu. Use HarfBuzz's draw
   API as a second outline source in the pack tool** (section 8.4).

## Conditions

- Windows 11 laptop, Intel Core i7-1360P, 32 GB, Intel Iris Xe, AC power.
- Compilers: MSVC 19.44.35214 (`/O2 /MT`), MinGW-w64 gcc 16.2.0 (`-O2`),
  emcc 6.0.10 (Docker image `emscripten/emsdk:6.0.10`, `-O2` and `-Os`).
  WebAssembly ran in node 24.14.0 on the same machine.
- Chromium: Microsoft Edge 154.0.4258.62, headless, through Playwright.
- Fonts from `C:/Windows/Fonts`: Segoe UI 5.71, Arial 7.06, Calibri 6.27,
  Nirmala UI 1.46 (face 0 of `Nirmala.ttc`), Leelawadee UI 6.00,
  Microsoft YaHei 6.31 (face 0 of `msyh.ttc`), Yu Gothic Regular 1.95
  (face 0 of `YuGothR.ttc`), Source Han Sans JP Normal (CFF), Bahnschrift
  and Segoe UI Variable (variable TrueType). Every font of the layout
  corpus has 2048 units per em.
- Every timing run held the shared measurement lock (`guard.sh time`), with
  no compiler or test process running and a CPU load of 20 % or less at
  the start. Builds and correctness runs used `guard.sh build`.

## Candidates

| Candidate | Version tested | Language | Dependencies | Licenses |
|---|---|---|---|---|
| Skribidi | `dee63d6` (2026-08-17), no tags or releases | C17 (`C_EXTENSIONS ON`); headers usable from C++ | HarfBuzz 11.0.0, SheenBidi `83f7710`, libunibreak 6.1, budouxc `a044d49` | Skribidi MIT; HarfBuzz "Old MIT" (one subfolder, `ms-use`, MIT); SheenBidi Apache-2.0; libunibreak zlib; budouxc MIT, its models are Google BudouX's (Apache-2.0); Unicode data under the Unicode license |
| kb_text_shape | 2.28e (`cc63806`, 2026-10-02), `github.com/JimmyLefevre/kb` | C or C++, one header (31603 lines); declare-anywhere C | none | zlib |
| HarfBuzz alone | 14.6.0 (2026-10-05), with our glue | C++11 library, C API | SheenBidi and libunibreak in our glue | as above |

HarfBuzz is 3 major versions ahead of the one Skribidi pins. Skribidi
compiled against HarfBuzz 14.6.0 with no change and gave the same output,
glyph for glyph and line for line, on the whole corpus.

kb_text_shape's coverage claims: its header says that all OpenType
shapers are supported, except Zawgyi, the Syriac abbreviation mark and
(probably) Egyptian hieroglyphs; that Indic1 (`beng`, not `bng2`) fonts,
Windows 3.1 Arabic fonts and Thai PUA fonts are not supported; that word
breaking for languages that need a dictionary (CJK) is not supported; and
that explicit direction controls (U+202A to U+202E, U+2066 to U+2069) are
not supported. The probe confirmed the shaping claim for Latin, Arabic,
Hebrew, Devanagari, Bengali, Tamil, Thai and CJK (section 2) and found no
Thai dictionary (section 3). It did not test the other claims.

## 1. Build

Sizes are the linked probe program minus the same program with no layout
library (MSVC 176640 bytes, MinGW 1644544 bytes, wasm 41593 bytes at
`-O2`, 40416 at `-Os`), with unused sections removed (`/OPT:REF`,
`--gc-sections`). Skribidi's size has its layout, not its editor.

| | HarfBuzz 14.6 + glue | Skribidi + HarfBuzz 11.0 | kb_text_shape + glue |
|---|---|---|---|
| MSVC, bytes | 1060864 | 1293312 | 507392 |
| MinGW gcc, bytes | 1375232 | 1625088 | 610816 |
| wasm `-O2`, bytes (gzip -9) | 934811 (292625) | 1176728 (371330) | 425910 (91676) |
| wasm `-Os`, bytes (gzip -9) | 691239 (240923) | 943088 (322961) | 380371 (83557) |

The designer needs the editor too. A wasm program with Skribidi's layout
and editor (`-O2`) is 1271378 bytes, 411556 with gzip -9.

Build friction:

- Skribidi's own CMake build fetches its four dependencies by
  `FetchContent` (network at configure time) and turns warnings into
  errors for every target. With MinGW gcc and Ninja it built in 66 s
  (configure) plus 85 s, and its tests passed. With MSVC its tests passed
  through the Ninja generator. The Visual Studio generator failed in this
  scratch folder: MSBuild hit the Windows path length limit in a deep
  build path, not a Skribidi fault.
- Under emcc 6.0.10, Skribidi's CMake build stops on two
  `-Warray-bounds` errors inside HarfBuzz 11.0.0 (`hb-ot-layout-gsubgpos.hh`
  lines 2278 and 3501). `cmake --compile-no-warning-as-error` builds it.
  The tests then fail under node because the font files are not in the
  WebAssembly file system; the repository has no WebAssembly build of its
  tests or examples (issue #25, "WASM example", open).
- Without CMake, the probe built every library with a short script:
  HarfBuzz from its one-file amalgamation (`harfbuzz.cc`), SheenBidi from
  `SheenBidi.c` with `SB_CONFIG_UNITY`, 14 libunibreak files, `budoux.c`
  and 15 Skribidi files. All libraries for all three candidates built in
  26 s (MinGW, parallel), 62 s (MSVC, serial) and 45 to 47 s (emcc). Do
  not compile `emoji_presentation_scanner.c`: `skb_common.c` includes it.
- kb_text_shape gives no warning with MSVC `/W3` (C4244, C4267, C4305
  and C4996 off for every library) and a set of
  `-Wstringop-overflow` warnings in `kbts_PlaceBlob` with gcc 16.2 `-O2`.
- HarfBuzz 14.6 gives 4 `C4146` warnings with MSVC.
- Only HarfBuzz needs C++ (C++11; the probe used C++17 with no exceptions
  and no RTTI). Skribidi, SheenBidi, libunibreak and budouxc are C.
- The three toolchains give the same output for each candidate, bit for
  bit, on the shaping and paragraph corpora.

## 2. Shaping correctness

### 2.1 Method

Each candidate laid out each corpus line with no wrapping at 100 px per
em, and wrote each glyph's font, glyph id and position. Positions are
compared in font units, after a common x offset is removed (alignment is
not shaping). Glyphs are put in visual order by x, then y, then id, and
matched with a sequence diff.

The Chromium reference is Edge's print-to-PDF output of a page that loads
the same font files through `@font-face`. Skia's PDF backend writes each
glyph id as an Identity-H CID with `CIDToGIDMap /Identity`, and writes the
positions as text-space moves with the font's own widths in `/W`. A small
PDF content-stream interpreter in the probe read every glyph id and
position back. This was the most reliable method found:

- Canvas `measureText` gives widths of whole strings, not glyph ids.
- Rasterizing and diffing does not identify a glyph, and Edge's rasterizer
  is not exact (section 4.2).
- Edge places text with linear advances, not hinted ones: the width of a
  Segoe UI sentence at 11, 13, 16, 24 and 100 px differed from HarfBuzz's
  by 0 to 0.013 px, the 1/64 px rounding of Chromium's layout units, at
  device pixel ratio 1 and 2.

Edge trims the space of adjacent CJK punctuation by default
(`text-spacing-trim: normal`), so the reference pages set `space-all`.
Without it, Japanese positions differed by 1024 units (half an em) at
"。「".

### 2.2 Results

"same ids" means the same glyph ids in the same visual order. "d" is the
largest position difference in font units, along the line / across it.

| Item | Font | Glyphs | HarfBuzz + glue | Skribidi `dee63d6` | Skribidi, patched | kb_text_shape + glue |
|---|---|---|---|---|---|---|
| Latin, kerning and ligatures | Segoe UI | 83 | same ids, d 0 / 0 | same ids, d 0 / 0 | same ids, d 0 / 0 | same ids, d 0 / 0 |
| Latin | Calibri | 74 | same, 0 / 0 | same, 0 / 0 | same, 0 / 0 | same, 0 / 0 |
| Latin | Arial | 83 | same, 0 / 0 | same, 0 / 0 | same, 0 / 0 | same, 0 / 0 |
| Arabic with marks, Latin and numbers, RTL paragraph | Arial | 88 | same, 0.68 / 0 | 1 glyph missing | same, 0.68 / 0 | same, 0.68 / 0 |
| the same | Segoe UI | 88 | same, 0.68 / 0 | 1 glyph missing | same, 0.68 / 0 | same, 0.68 / 0 |
| Arabic phrase and number in an LTR paragraph | Arial | 54 | same, 0.52 / 0 | 19 glyphs in the wrong place | same, 0.52 / 0 | 3 glyphs in the wrong place |
| Hebrew with points, Latin and numbers | Arial | 64 | same, 0.62 / 0 | 1 glyph missing, 1 out of order | same, 0.62 / 0 | same, 0.62 / 0 |
| the same | Segoe UI | 64 | same, 0.46 / 0 | 1 glyph missing, 1 out of order | same, 0.46 / 0 | same, 0.46 / 0 |
| Devanagari: reph, i-matra, conjuncts | Nirmala UI | 62 | same, 0 / 0 | same, 0 / 0 | same, 0 / 0 | same, 0 / 0 |
| Bengali conjuncts and split vowels | Nirmala UI | 41 | same, 0 / 0 | same, 0 / 0 | same, 0 / 0 | same, 0 / 0 |
| Tamil split vowels, ஸ்ரீ | Nirmala UI | 31 | same, 0 / 0 | same, 0 / 0 | same, 0 / 0 | same, 0 / 0 |
| Thai with stacked marks | Leelawadee UI | 70 | same, 0 / 0 | same, 0 / 0 | same, 0 / 0 | same, 0 / 0 |
| Chinese with punctuation | Microsoft YaHei | 35 | same, 0 / 0 | same, 0 / 0 | same, 0 / 0 | same, 0 / 0 |
| Japanese with punctuation | Yu Gothic | 34 | same, 0 / 0 | same, 0 / 0 | same, 0 / 0 | same, 0 / 0 |
| Japanese, vertical (`vert`, `vrt2`) | Yu Gothic | 15 | same ids (positions not compared) | no vertical layout | no vertical layout | no vertical layout |

- HarfBuzz against Chromium: the largest difference is 0.68 font units
  (0.033 px at 100 px), on bidi lines only. Chromium puts each bidi run on
  its 1/64 px grid.
- Skribidi against HarfBuzz is exact where it is correct: it shapes at the
  font's units per em and scales in float.
- kb_text_shape gives the same ids and positions as HarfBuzz once the glue
  orders its runs (2.4). The one difference is a bidi case its run output
  cannot express.
- Vertical text: HarfBuzz with `HB_DIRECTION_TTB` gives the same 15 glyph
  ids as Edge's `writing-mode: vertical-rl`, with the same vertical
  alternates. The probe did not compare vertical positions (the PDF gives
  horizontal-origin positions). Skribidi and kb_text_shape have no
  vertical layout.

### 2.3 The Skribidi bugs and the patch

Both bugs are in `skb__line_append_shaping_run()` (`skb_layout.c` line
1033 at `dee63d6`; this note first named it `skb__add_layout_run()`). It merges a shaping run into the previous layout run when
their direction, font and content run are equal. It does not compare the
bidi level, and it merges RTL runs.

- A level-2 LTR run (a number after Arabic) merges with a following
  level-0 LTR run. The reorder then moves "123 means hello world." with the
  number. Example: "The word مَرْحَبًا بِالْعَالَمِ 123 means hello world."
  shows the Arabic phrase after "world.".
- In an RTL paragraph, a space after a Latin word becomes its own RTL
  shaping run (script Latin). It merges with the next RTL run, "و ". Each
  shaping run's glyphs are in visual order, but the merged run's glyph
  range is computed as if the merged clusters were one visual run, and
  glyphs fall out of the merged layout run. Example: "مع English و 123"
  loses و. "مع English وا 123" loses both letters. The bug needs an RTL
  paragraph, an LTR word, an RTL word, then more text.

The patch, tested against Chromium and against Skribidi's own tests:

```c
// skb_layout.c, skb__line_append_shaping_run(), the merge condition
if (shaping_run->direction == cur_layout_run->direction
    && !skb_is_rtl(shaping_run->direction)
    && shaping_run->bidi_level == cur_layout_run->bidi_level
    && shaping_run->font_handle == cur_layout_run->font_handle
    && shaping_run->content_run_idx == cur_layout_run->content_run_idx) {
```

With it, every horizontal item above is the same as Chromium, the editor
places the caret in the right visual order (section 6), and Skribidi's
unit tests pass with MinGW gcc. Not merging RTL runs makes more layout
runs in RTL text; its cost was not measured. Report both bugs upstream
before or with the pin.

### 2.4 Glue that kb_text_shape needs

kb_text_shape's context API returns runs in logical order, with the glyphs
of each run in visual order, and gives each run a direction but not an
embedding level. The glue reorders runs as kb's own editor (refpad,
`refpad_editor.c`) does: consecutive runs of one direction form a block,
an RTL block's runs are reversed, and in an RTL paragraph the blocks are
reversed. This is enough for two levels. It fails on a number after Arabic
in an LTR paragraph (level 2 inside level 1), the item with 3 glyphs in
the wrong place. Before the glue, 47 of 88 Arabic glyphs and 32 of 64
Hebrew glyphs were out of place.

## 3. Paragraph layout

### 3.1 Line breaks against Chromium

UDHR article 1 in each language, cut or repeated to 600 codepoints, at 16
px per em, wrapped at 320 px and at 211 px; the Arabic and Hebrew
paragraphs have a Latin and number sentence inside. The Chromium reference
reads the line start of each codepoint from `Range.getClientRects()`.
`white-space: normal`, `text-align: start`, `dir=auto`. "same" means the
same line start for every line.

| Paragraph | Lines (Edge) | HarfBuzz + libunibreak | Skribidi | kb_text_shape |
|---|---|---|---|---|
| English, 320 / 211 px | 15 / 24 | same / same | same / same | same / same |
| Arabic with bidi sentence | 11 / 18 | same / same | same / same | same / same |
| Hebrew with bidi sentence | 14 / 21 | same / same | same / same | same / same |
| Hindi | 13 / 20 | same / same | same / same | same / same |
| Bengali | 16 / 23 | same / same | same / same | same / same |
| Tamil | 23 / 41 | same / same | same / same | same / same |
| Thai, `lang=th` | 13 / 20 | 14 / 14 lines, 5 / 9 overflow | 13 / 20 lines, differ from line 2 / 3 | 14 / 14 lines, 5 / 9 overflow |
| Thai, `lang=en` | 13 | 14, 5 overflow | 14, 5 overflow | 14, 5 overflow |
| Japanese, `lang=ja` | 31 / 47 | same / same | 35 / 57: same as Edge's `word-break: auto-phrase` | same / same |
| Chinese, `lang=zh-Hans` | 31 / 47 | same / same | 31 / 49, differ from line 2 | same / same |
| Mixed Latin, Hindi, Thai, Chinese, 4 fonts, two orders | 9 / 9 | same / same | same / same | same / same |

- Skribidi's Japanese lines equal Edge's `word-break: auto-phrase` line for
  line (35 and 57 lines). Both use BudouX. Edge's default breaks between
  any two ideographs, as UAX 14 does.
- Font fallback: all three candidates pick fonts per codepoint as CSS
  `font-family` does, first font in the list that has the glyph, and
  match Edge with the list in both orders. A first version of the glue
  kept the current font while it had the codepoint (so spaces and digits
  stayed in the Thai or Chinese font); it broke line 3 differently from
  Edge. Per-codepoint first-in-list matches.
- Justification: none of the three has it. Chromium has `text-align:
  justify`. ysp would write it: spread the line's spare width over its
  spaces, or between characters for Thai and CJK.

### 3.2 Breaks inside Thai, Japanese and Chinese words

Each line start was checked against the word boundaries of
`Intl.Segmenter` in Edge (ICU, dictionary-based for these languages). For
Japanese and Chinese, a break between ideographs is legal (UAX 14), so a
break that is not at a word boundary is a style choice, not an error.

| Paragraph | Edge default | Edge `auto-phrase` | HarfBuzz + libunibreak | Skribidi | kb_text_shape |
|---|---|---|---|---|---|
| Thai 320 px | 13 lines, 0 not at a word boundary | 13, 0 | 14, 0, 5 lines overflow | 13, 1 | 14, 0, 5 overflow |
| Thai 211 px | 20, 0 | 20, 0 | 14, 0, 9 overflow | 20, 1 | 14, 0, 9 overflow |
| Japanese 320 px | 31, 11 | 35, 0 | 31, 11 | 35, 0 | 31, 11 |
| Japanese 211 px | 47, 23 | 57, 7 | 47, 23 | 57, 7 | 47, 23 |
| Chinese 320 px | 31, 10 | 31, 10 | 31, 10 | 31, 5 | 31, 10 |
| Chinese 211 px | 47, 18 | 47, 18 | 47, 18 | 49, 7 | 47, 18 |

- libunibreak 6.1 and kb_text_shape have no Thai dictionary: Thai breaks
  only at spaces, and lines run past the box.
- Skribidi breaks Thai with BudouX's Thai model when the language is `th`
  (not otherwise): it fits the box, and 1 of 12 or 19 breaks is not at an
  ICU word boundary.

## 4. Glyph ids and `ysp/outline.h`

### 4.1 The same glyph for the same id

For every glyph of every font, the probe built the outline two ways, in em
units with y down, and compared the segments: `ysp/outline.h`'s own reader
(`yol_font_glyph`), and HarfBuzz's `hb_font_draw_glyph` fed through
`ysp/outline.h`'s path builder. HarfBuzz is the shaper of Skribidi and of
the glue, so its glyph ids index these outlines.

| Font | Glyphs | Equal bit for bit | Differ |
|---|---|---|---|
| Segoe UI | 5394 | 5394 | 0 |
| Arial | 4651 | 4650 | 1: `napostrophe` (396), 1 font unit in x |
| Nirmala UI | 5025 | 5025 | 0 |
| Leelawadee UI | 1055 | 1055 | 0 |
| Microsoft YaHei | 30209 | 30209 | 0 |
| Yu Gothic Regular | 24411 | 24411 | 0 |
| Calibri | 7048 | 7041 | 7: within 3e-8 em (HarfBuzz's float callbacks on scaled composites) |
| Source Han Sans JP (CFF) | 17934 | 17934 | 0 |

`napostrophe` in Arial is a composite whose `glyf` xMin (-38) differs from
its `hmtx` left side bearing (-39). HarfBuzz moves the outline by the
difference, as a TrueType rasterizer places its phantom points;
`ysp/outline.h` and fontTools do not. Arial has 13 composites with this
mismatch, and Times New Roman 19 glyphs (18 composites); in Arial only
this one gave a different outline.
`ysp/outline.h` may want HarfBuzz's rule; this probe did not decide it.

### 4.2 End to end, against Edge's rendering

Each shaping item's first line (glyph ids and pens from the HarfBuzz
glue) was drawn by `yol_raster()` (exact area) at 96 px per em, and
compared with Edge's screenshot of the same text and font at the same
size, after the best integer shift. A control replaced one glyph id per
line with the next id.

| Item | Correlation | Mean abs difference on ink | Pixels off by more than 0.5 | Control: correlation, pixels off by more than 0.5 |
|---|---|---|---|---|
| Latin, Segoe UI | 0.974 | 0.031 | 64 | 0.870, 1772 |
| Latin, Calibri | 0.926 | 0.047 | 1121 | 0.919, 1228 |
| Latin, Arial | 0.963 | 0.032 | 198 | 0.862, 1932 |
| Arabic, Arial | 0.912 | 0.055 | 1185 | 0.875, 1750 |
| Arabic, Segoe UI | 0.951 | 0.040 | 408 | 0.919, 934 |
| Arabic in LTR | 0.952 | 0.038 | 271 | 0.757, 2360 |
| Hebrew, Arial | 0.919 | 0.044 | 979 | 0.873, 1529 |
| Hebrew, Segoe UI | 0.928 | 0.048 | 971 | 0.886, 1497 |
| Devanagari | 0.932 | 0.039 | 976 | 0.861, 2163 |
| Bengali | 0.897 | 0.051 | 1299 | 0.776, 2966 |
| Tamil | 0.862 | 0.058 | 1459 | 0.738, 3308 |
| Thai | 0.903 | 0.065 | 1470 | 0.846, 2395 |
| Chinese | 0.991 | 0.019 | 0 | 0.856, 2524 |
| Japanese | 0.919 | 0.068 | 1203 | 0.790, 3412 |

Every line lands in place and the control is worse on every line, but this
check is weak: Edge's rasterizer is not exact coverage (it hints and
applies its own contrast), so a correct line still has up to 1470 pixels
off by more than 0.5. On Calibri the control's change was small (1121 to
1228). The exact checks are 2.2 (ids and positions against Edge) and 4.1
(ids against outlines). The probe did not draw through
`yol_cset_add_font()` and `ygfx_crun()`; section 8 does that for single
glyphs.

## 5. Cost

### 5.1 Layout time

One call lays out the whole text from UTF-8: bidi, script and font runs,
shaping, line breaking, and the glyph list, into objects reused from call
to call (Skribidi: one `skb_layout_t` and one temp allocator; the glue: one
`hb_buffer_t`; kb: one context). Labels are one line at 32 px; the
500-codepoint paragraph wraps at 600 px; the page is 6 such paragraphs
(3005 codepoints) at 1200 px. Microseconds per call, the median of each
round's median, 3 interleaved rounds (each round ran every toolchain and
candidate in turn), at least 30 calls and 0.4 s per item.

| Item | Codepoints | MSVC: HarfBuzz / Skribidi / kb | MinGW gcc | wasm, node 24 |
|---|---|---|---|---|
| Label, English | 18 | 2.9 / 13.1 / 3.8 | 2.6 / 13.5 / 3.1 | 3.2 / 6.3 / 4.1 |
| 500, English | 500 | 40.6 / 124.5 / 252.7 | 37.6 / 112.8 / 200.8 | 49.1 / 86.6 / 253.3 |
| Page, English | 3005 | 234.9 / 566.1 / 1510.7 | 221.0 / 522.8 / 1201.7 | 287.5 / 500.4 / 1510.1 |
| Label, Arabic | 20 | 6.8 / 24.3 / 5.3 | 5.7 / 24.8 / 4.4 | 7.3 / 9.9 / 6.3 |
| 500, Arabic | 500 | 118.1 / 202.3 / 328.1 | 110.3 / 196.9 / 288.2 | 133.2 / 156.0 / 372.2 |
| Page, Arabic | 3005 | 694.8 / 1127.5 / 1941.8 | 657.9 / 1049.1 / 1655.4 | 777.8 / 922.9 / 2193.4 |
| Label, Hebrew | 19 | 14.0 / 30.1 / 14.8 | 11.6 / 29.9 / 10.8 | 16.8 / 16.4 / 12.6 |
| 500, Hebrew | 500 | 283.4 / 331.8 / 812.3 | 252.3 / 282.2 / 590.6 | 362.9 / 306.6 / 653.1 |
| Page, Hebrew | 3005 | 1822.8 / 1892.3 / 5128.2 | 1521.7 / 1756.6 / 3590.1 | 2158.5 / 1835.8 / 3961.5 |
| Label, Hindi | 18 | 11.4 / 19.9 / 10.8 | 13.9 / 17.6 / 8.1 | 11.9 / 11.6 / 10.8 |
| 500, Hindi | 500 | 321.6 / 290.5 / 678.1 | 315.0 / 250.6 / 473.8 | 335.7 / 275.2 / 625.4 |
| Page, Hindi | 3005 | 1801.6 / 1800.0 / 5323.7 | 1778.1 / 1476.5 / 2847.0 | 1876.8 / 1635.9 / 3675.9 |
| Label, Bengali | 21 | 12.4 / 19.6 / 16.4 | 12.1 / 20.3 / 8.3 | 14.3 / 12.7 / 11.7 |
| 500, Bengali | 500 | 254.3 / 270.7 / 606.5 | 240.6 / 255.4 / 410.7 | 289.4 / 244.0 / 565.3 |
| Page, Bengali | 3005 | 1443.1 / 1535.2 / 3731.0 | 2157.4 / 1368.1 / 2438.4 | 1668.8 / 1446.9 / 3488.3 |
| Label, Tamil | 15 | 5.2 / 15.7 / 5.8 | 4.8 / 14.3 / 4.6 | 5.6 / 7.2 / 6.4 |
| 500, Tamil | 500 | 190.1 / 248.0 / 549.0 | 168.1 / 218.0 / 409.3 | 200.2 / 179.1 / 564.1 |
| Page, Tamil | 3005 | 1081.7 / 1288.9 / 3144.6 | 1074.1 / 990.3 / 2431.2 | 1132.8 / 1051.7 / 3358.1 |
| Label, Thai | 24 | 9.8 / 26.4 / 7.2 | 8.0 / 21.1 / 6.1 | 9.9 / 17.4 / 8.0 |
| 500, Thai | 500 | 183.4 / 344.6 / 357.0 | 149.7 / 382.8 / 318.0 | 184.0 / 305.5 / 380.5 |
| Page, Thai | 3005 | 1137.5 / 1951.1 / 2124.5 | 904.6 / 1877.5 / 1993.8 | 1088.9 / 1806.8 / 2284.0 |
| Label, Japanese | 9 | 4.1 / 14.9 / 2.9 | 2.8 / 13.8 / 2.4 | 3.4 / 6.7 / 3.5 |
| 500, Japanese | 500 | 206.9 / 395.4 / 387.1 | 151.1 / 311.2 / 322.4 | 168.8 / 311.2 / 453.2 |
| Page, Japanese | 3005 | 1183.0 / 1954.4 / 2286.1 | 900.6 / 1605.0 / 1915.5 | 1006.3 / 1835.0 / 2699.3 |
| Label, Chinese | 8 | 2.1 / 14.2 / 1.7 | 1.7 / 12.7 / 1.5 | 2.1 / 5.6 / 2.2 |
| 500, Chinese | 500 | 60.1 / 264.3 / 247.7 | 46.4 / 205.2 / 211.1 | 65.4 / 207.6 / 301.5 |
| Page, Chinese | 3005 | 339.1 / 1367.2 / 1561.8 | 257.2 / 1014.7 / 1273.7 | 366.4 / 1236.3 / 2623.8 |
| 440 codepoints, 4 scripts, 4 fonts | 440 | 208.1 / 353.3 / 562.2 | 155.8 / 335.8 / 274.8 | 212.5 / 190.4 / 372.1 |

The largest per-call time in any round, any script:

| | Labels, p90 / max, us | 500 codepoints, p90 / max | Page, p90 / max |
|---|---|---|---|
| HarfBuzz, MSVC / gcc / wasm | 24 / 807; 26 / 1929; 26 / 865 | 681 / 2119; 583 / 4563; 766 / 2326 | 3537 / 5397; 3584 / 6255; 4786 / 5472 |
| Skribidi | 57 / 1782; 40 / 2324; 32 / 1914 | 674 / 1710; 704 / 1729; 637 / 2240 | 3785 / 7084; 4035 / 5795; 3651 / 4265 |
| kb_text_shape | 21 / 677; 16 / 2037; 23 / 2579 | 1416 / 3198; 1169 / 2753; 1490 / 8542 | 7263 / 8939; 5944 / 10296; 7543 / 9125 |

- The HarfBuzz glue and the kb glue shape each paragraph twice (once for
  the line breaker's advances, once per line). Skribidi shapes once.
- Skribidi's labels cost 12.7 to 30.1 us natively and 5.6 to 17.4 us in
  wasm. The cause of the native gap was not investigated.
- The patched Skribidi was not timed.

For the player: a participant's typed response or a feedback line with a
number is a label. Its median is 30.1 us or less with every candidate, and
the slowest single call seen was 2.6 ms (kb, wasm). Laying it out inside
a 16.7 ms frame fits. A 500-codepoint paragraph fits at the median (0.4 ms
or less with Skribidi), but its worst calls reached 1.7 to 2.2 ms; lay out
instructions pages between trials, as rig_spec 5.2 plans (static text is
stored as laid-out glyph runs).

### 5.2 Memory

A counting allocator under each library (the C library's `malloc` family
by a forced include; HarfBuzz through `hb_malloc_impl`), MinGW gcc. Calls
and bytes per call after the first call, and the peak of live bytes the
call added. Fonts are memory-mapped by HarfBuzz (Skribidi and the glue),
so their bytes are not counted.

| Item | HarfBuzz + glue: calls, bytes, peak | Skribidi | kb_text_shape |
|---|---|---|---|
| Labels (9 scripts) | 5; 494 to 1022 B; 418 to 850 B | 9 to 21; 2606 to 3275 B; 1520 to 1540 B | 0 |
| 500 codepoints | 13 to 31; 15310 to 17874 B; 3662 to 4142 B | 9 to 200; 16862 to 48006 B; 12082 to 26860 B | 0 to 4; up to 393446 B (Japanese) |
| Page (3005 codepoints) | 49 to 97; 101524 to 103348 B; 6654 to 9132 B | 29 to 1175; 102537 to 150921 B; 16257 to 26860 B | 0 |
| Live after all items | 456856 B | 2567390 B | 85225314 B |

- kb_text_shape copies and expands each font into its context: the first
  Chinese label allocated 20173891 bytes (the YaHei file is 19704352
  bytes), the first Japanese label 17267058 (Yu Gothic, 13835976), and the
  4-font context 29990550. Two contexts with one font hold two copies.
- Skribidi's Arabic and Bengali pages make 919 and 1175 allocator calls
  per layout. The other pages make 29 to 47.
- The glue's 5 calls per label are SheenBidi's objects.

## 6. Editing

Skribidi has an editor (`skb_editor.h`, about 6600 lines with rich text
and the rules) that the designer can use as it is. Checked in a probe
program, natively and in wasm with the same results:

- Caret: the arrow keys move in logical order, one grapheme at a time.
  The default mode (`SKB_CARET_MODE_SKRIBIDI`) adds a stop where the
  direction changes; `SIMPLE` does not. On "abc مرحبا 123 def", the caret
  visits offsets 0 to 16 in order and its x follows the visual order of
  each run. Before the patch of 2.3, the visual order of this text was
  wrong (the run-merge bug); with it, the caret positions match the
  corrected layout.
- Selection: a selection from inside "abc" to inside "123" gives one
  rectangle per visual run (4 at `dee63d6`, 5 with the patch). Shift+arrow
  grows the selection.
- IME: `skb_editor_set_composition_utf32()` shows composition text at the
  caret without putting it in the text; `commit` inserts it; `clear`
  drops it. Composing かな and committing 仮名 gave the right text. Undo
  restored the text before the commit. The browser's composition events
  must reach these calls; the designer needs a hidden text input for IME
  and screen readers, and the probe did not build it.
- Mouse: click, drag, and double and triple click (with a time stamp).
- Cost of one keystroke (insert one codepoint and lay out again) in a
  500-codepoint paragraph, 600 px wide, 3 rounds under the lock: English
  90.1 to 90.4 us (gcc), 111.4 to 125.4 us (wasm); Arabic 171.6 to 172.2
  us (gcc), 207.3 to 230.7 us (wasm).

kb_text_shape gives grapheme, word and line breaks and nothing else for
editing. HarfBuzz gives cluster values only. With either, ysp would write
caret positions from clusters, grapheme boundaries (libunibreak for the
glue), hit testing, selection rectangles across bidi runs, IME
composition, undo, and rich text: the work that Skribidi's editor files
already do.

## 7. Pinning and churn

Skribidi's history at `dee63d6`:

| Period | Commits | Marked "[API breaking]" |
|---|---|---|
| 2025-06 (first commit 2025-06-05) | 68 | 0 |
| 2025-07 to 2025-11 | 124 | 32 |
| 2025-12 to 2026-08 | 18 | 1 (2026-08-17, headers moved to `include/skribidi/`) |

- 210 commits, 172 by Mikko Mononen; 7 other contributors. No tags and no
  releases. The README says the API is likely to change.
- 15 open issues on 2026-10-07; the last commit was 2026-08-17.
- The breaking changes of 2025 renamed and reshaped attributes, rich text,
  the editor and decorations. The probe's own use (font collection, layout,
  lines, runs, glyphs) needed about 100 lines and would have followed most
  of them.

Proposed policy:

1. Vendor Skribidi's `src` and `include` at a commit, with the bidi patch
   as a separate, named patch file. Do not use `FetchContent` at build
   time. Build it with the probe's script method (no network, no
   warnings-as-errors), the same way on MSVC, MinGW and emcc.
2. Pin HarfBuzz by release tag, independent of Skribidi's pin (Skribidi on
   HarfBuzz 14.6.0 gave identical output). Pin SheenBidi, libunibreak and
   budouxc by commit or tag.
3. Record the Skribidi commit, the patch hash and the HarfBuzz version in
   the pack manifest (rig_spec 5.1, tool version), because static text in a
   pack is laid-out glyph runs and the player lays out dynamic text.
4. Update only for a fix or a feature ysp needs, never on a schedule. Each
   update must pass a corpus check like this probe's: glyph ids, positions
   and line starts for every item, against the previous pin and against
   Chromium. Any difference is listed in the change log; a difference in
   static text needs a pack rebuild, not a player change.
5. Send the two bug reports and the patch upstream. Drop the local patch
   when upstream has a fix that passes the corpus check.

## 8. hb-gpu, HarfBuzz's GPU glyph renderer

HarfBuzz 14.0 added hb-gpu (`hb-gpu.h`, `hb-gpu.cc`, `hb-gpu-draw.cc`,
`hb-gpu-paint.cc`, shaders in GLSL ES 3.00, HLSL, MSL and WGSL), by Behdad
Esfahbod. Its fragment shader credits Eric Lengyel's Slug reference and
carries his 2017 copyright line under HarfBuzz's license. A glyph blob is
RGBA16I texels: each coordinate is an int16 at 4 steps per font unit
(range about +/-8000 units), with bands and two rays per pixel. Below 16
pixels per em it blends in a 4-sample average (MSAA-style), and
`HB_GPU_NO_MSAA` turns that off. Stem darkening is a separate function
that `hb_gpu_draw()` does not call, so it was off in every run here.

The probe ran hb-gpu's own GLSL (`hb_gpu_shader_source()` and
`hb_gpu_draw_shader_source()`, `HB_GPU_ATLAS_2D`) with a minimal instanced
vertex shader (one quad per glyph, 1.5 px margin, no dilation), in the
same ANGLE context as `ysp/gfx.h`, through `tests/adapt/gfx_headless.h`
on the Iris Xe (ANGLE from Docker Desktop's folder, as in gfx.md, on
D3D11). `ysp/gfx.h`'s curve sets came
from `yol_cset_add_font()` (resolved, backward lists), drawn by
`ygfx_crun()`, one run per glyph, turned by the run's `ori`.

### 8.1 Coverage error

Reference: `yol_raster()` exact box coverage in double of the same
outline, at the same pen (a subpixel offset of 0.37, 0.61 px) and turn.
Glyph sets: 20 Latin glyphs of Segoe UI, 8 CJK glyphs of Microsoft YaHei,
and 5 CJK and 3 Latin glyphs of Source Han Sans JP (CFF). Read back from
RGBA32F. Mean error on edge pixels (reference strictly between 0 and 1),
the range over the three sets, and the largest error on any pixel.
`ysp/gfx.h` takes the rays by itself on a turned run, so its exact-area
column is the rays at 15 and 45 degrees.

| px per em | Turn, degrees | `ysp/gfx.h` exact area | `ysp/gfx.h` `.rays` | hb-gpu | hb-gpu, `HB_GPU_NO_MSAA` |
|---|---|---|---|---|---|
| 8 | 0 | 1.2e-5 to 1.8e-5 (max 1.1e-4) | 0.036 to 0.058 (max 0.46) | 0.087 to 0.097 (max 0.33) | 0.043 to 0.060 (max 0.38) |
| 8 | 15 | rays | 0.038 to 0.059 (max 0.36) | 0.12 to 0.13 (max 0.42) | 0.059 to 0.088 (max 0.36) |
| 8 | 45 | rays | 0.031 to 0.052 (max 0.57) | 0.14 to 0.15 (max 0.56) | 0.075 to 0.11 (max 0.40) |
| 12 | 0 | 2.6e-5 to 3.6e-5 (max 1.1e-4) | 0.021 to 0.040 (max 0.38) | 0.039 to 0.050 (max 0.21) | 0.032 to 0.044 (max 0.31) |
| 12 | 15 | rays | 0.024 to 0.034 (max 0.37) | 0.12 to 0.13 (max 0.35) | 0.049 to 0.072 (max 0.32) |
| 12 | 45 | rays | 0.023 to 0.032 (max 0.30) | 0.17 (max 0.54) | 0.075 to 0.10 (max 0.36) |
| 24 | 0 | 5.3e-6 to 2.1e-5 (max 7.3e-5) | 0.013 to 0.017 (max 0.40) | 0.023 to 0.024 (max 0.36) | the same |
| 24 | 15 | rays | 0.012 to 0.015 (max 0.43) | 0.039 to 0.050 (max 0.28) | the same |
| 24 | 45 | rays | 0.011 to 0.013 (max 0.44) | 0.061 to 0.080 (max 0.49) | the same |
| 48 | 0 | 2.1e-5 to 3.9e-5 (max 1.5e-4) | 0.0062 to 0.0083 (max 0.44) | 0.014 to 0.020 (max 0.27) | the same |
| 48 | 15 | rays | 0.0062 to 0.0075 (max 0.41) | 0.037 to 0.046 (max 0.31) | the same |
| 48 | 45 | rays | 0.0056 to 0.0072 (max 0.43) | 0.062 to 0.074 (max 0.38) | the same |
| 200 | 0 | 1.9e-5 to 4.8e-5 (max 2.5e-4) | 0.0016 to 0.0019 (max 0.45) | 0.011 to 0.019 (max 0.27) | the same |
| 200 | 15 | rays | 0.0015 to 0.0018 (max 0.44) | 0.036 to 0.043 (max 0.25) | the same |
| 200 | 45 | rays | 0.0014 to 0.0015 (max 0.37) | 0.063 to 0.078 (max 0.26) | the same |

- hb-gpu's mean edge error divided by the rays', per glyph set: 1.04 to
  1.20 at 8 px with no turn and `HB_GPU_NO_MSAA`; 1.04 to 3.3 at 8 to 48
  px with no turn; 5.6 to 11 at 200 px with no turn; 1.5 to 11 turned at
  8 to 48 px; 24 to 51 turned at 200 px (computed from the per-set rows
  of this run). The turned error does not fall with size. Each ray's
  coverage is a ramp along the ray (Lengyel's form, `ysp/gfx.h`'s "V0" in
  [gfx.md](gfx.md)), not the half-plane coverage of the crossing
  ("V1"), so a slanted edge is wrong at every size; and its window comes
  from `fwidth()`, which by its definition (|dx| + |dy|) is 1.41 times too
  wide at 45 degrees. Its signed mean error on the YaHei set at 200 px and
  0 degrees was -6.9e-5, so it has no offset.
- Its 4-sample average below 16 px per em makes the mean edge error 1.6
  to 2.0 times larger at 8 px per glyph set (0.087 to 0.097 against 0.043
  to 0.060), and biases it: the signed mean at 8 px is -0.007 to -0.014
  with it and 0.0004 to 0.018 without it.
- Quantization: TrueType points at the font's units per em are integers
  or halves, so the 0.25-unit grid holds them exactly; the CFF set's
  error at 200 px (0.0156 at 0 degrees) was within the TrueType sets'
  range (0.011 to 0.019). The grid is 0.25 / 1000 em for a 1000-unit font,
  0.05 px at 200 px; this was derived, not measured. A glyph past +/-8000
  units does not encode.
- `ysp/gfx.h`'s exact area stays within 2.5e-4 here (the 1.8e-5 of
  gfx.md was measured nearer the origin; f32 positions near 1000 px
  have a larger step).

### 8.2 GPU cost

The pages of gfx.md's text cost table, laid out as `gfx_bench` lays
them out (Segoe UI's 94 printable ASCII glyphs at 12 px per em, 14 px
leading, 26205 glyphs; 7000 YaHei glyphs from U+4E00 at 16 px, 20 px
leading, 7140 glyphs), 1920 x 1200. 15 interleaved rounds of 20 frames
between two `glFinish()` calls, under the lock. `ysp/gfx.h` frames are
`ygfx_begin()` to `ygfx_end()` (RGBA16F scene and output pass), less
its empty frame; hb-gpu frames are a clear of an RGBA16F target and one
instanced draw (blend ONE, ONE_MINUS_SRC_ALPHA), less the clear alone.

| Workload | GPU ms per frame, median (range) |
|---|---|
| `ysp/gfx.h` 12 px Latin page, exact area | 7.91 (7.78 to 8.32) |
| `ysp/gfx.h` 12 px Latin page, `.rays` | 5.70 (5.62 to 5.82) |
| `ysp/gfx.h` 16 px CJK page, exact area | 10.00 (9.77 to 10.37) |
| hb-gpu 12 px Latin page | 20.46 (19.98 to 21.54) |
| hb-gpu 16 px CJK page | 16.19 (15.87 to 17.08) |
| hb-gpu 12 px Latin page, `HB_GPU_NO_MSAA` | 4.31 (4.23 to 4.83) |
| hb-gpu 16 px CJK page, `HB_GPU_NO_MSAA` | 5.88 (5.76 to 6.26) |

| Data for the CJK page's 7000 glyphs | Size |
|---|---|
| hb-gpu blobs | 4432913 texels of 8 bytes, 33.8 MB |
| `ysp/outline.h` curve set | 437032 texels of 16 bytes (6.7 MB) and 3320860 words (12.7 MB), 19.4 MB |

Encoding cost on the CPU: hb-gpu 12.3 to 15.2 us a Latin glyph and 32.2
to 33.0 us a CJK glyph; the curve set (resolved) 17.9 to 18.1 and 31.0 us
(8.3).

- With its 4-sample average, hb-gpu is 2.6 and 1.6 times slower than the
  exact area. Without it, hb-gpu is 1.3 times faster than `.rays` on the
  Latin page and 1.7 times faster than the exact area on the CJK page, at
  the error of 8.1.

### 8.3 What hb-gpu and HarfBuzz offer that ysp does not

- Color glyphs: `hb-gpu-paint` encodes COLRv0 and COLRv1 paint graphs
  (layers, gradients, transforms) for its own fragment shader.
  `ysp/gfx.h` draws solid layers from a palette only. Not measured here;
  emoji were out of the corpus.
- Variable fonts and CFF2: `ysp/outline.h` builds the default instance only
  and refuses CFF2 by name. HarfBuzz draws any instance of TrueType and
  CFF2 variable fonts.

HarfBuzz as an outline source for the pack tool, with no header depending
on it: `hb_font_draw_glyph()` with variations set, fed to `yol_move`,
`yol_line`, `yol_quad`, `yol_cubic` and `yol_close_path` (em units, y
down, tolerance 1e-4 em), then `yol_cset_add()`. On static fonts this
path gives the outlines of 4.1, and the same curve set word for word as
`yol_cset_add_font()` over the first 3000 glyphs of Segoe UI, Microsoft
YaHei and Source Han Sans JP (CFF); Arial differs at `napostrophe`. Cost
per glyph, median of 3 rounds of 3000 glyphs, MinGW gcc, under the lock:

| Font | `ysp/outline.h` reader | HarfBuzz draw into the builder | Reader + resolved curve-set record | HarfBuzz + record |
|---|---|---|---|---|
| Segoe UI | 0.56 us | 0.62 us | 18.09 us | 17.92 us |
| Microsoft YaHei | 0.93 | 0.93 | 30.95 | 31.03 |
| Source Han Sans JP (CFF) | 2.73 | 3.11 | 53.23 | 53.14 |
| Arial | 0.52 | 0.56 | 16.79 | 17.57 |

Instances (every glyph drawn by HarfBuzz into a resolved curve-set record,
none refused):

| Font, instance | Glyphs | us a glyph | Against an independent instance |
|---|---|---|---|
| Bahnschrift, wght 300 wdth 75 | 961 | 37.9 | fontTools' variable glyph set at the same location: 0 font units, area within 4e-8. fontTools' static instance read by `ysp/outline.h`: within 0.5 units (its integer rounding), area within 0.001 |
| Bahnschrift, wght 700 wdth 100 | 961 | 40.9 | static instance: within 0.5 units, area within 0.001 |
| Segoe UI Variable, wght 700 | 2530 | 21.7 | static instance: within 0.5 units, area within 0.001 |
| AdobeVFPrototype-Subset (CFF2, HarfBuzz test font), wght 700 | 3 | 127.9 | fontTools' variable glyph set: within 0.0015 units, area within 1e-5 |
| TestCFF2VF (CFF2), wght 650 | 5 | 67.5 | fontTools' variable glyph set: 0 units, area within 2e-5 |

fontTools' static CFF2-to-CFF instance of AdobeVFPrototype differed from
both by up to 98 units; its converter, not HarfBuzz, is the outlier.

### 8.4 Recommendation for rendering

Keep `ysp/gfx.h`'s renderer. By the numbers of 8.1 and 8.2:

- Its exact area has a mean edge error of 5.3e-6 to 4.8e-5 (largest
  2.5e-4) where hb-gpu's is 0.011 to 0.097 at 0 degrees; hb-gpu has no
  counterpart of `ysp/gfx.h`'s value-exact text.
- Its rays have a smaller mean edge error than hb-gpu in every condition
  tested: by 1.04 to 1.20 times at 8 px with no turn (hb-gpu without its
  4-sample average), up to 51 times turned at 200 px; hb-gpu's turned
  error does not shrink with size.
- hb-gpu is faster only without its 4-sample average (4.31 against 5.70
  ms on the Latin page, 5.88 against 10.00 ms on the CJK page), at 0.043
  to 0.11 mean error at 8 px, against 0.031 to 0.059.
- Its CJK data is 1.7 times larger (33.8 against 19.4 MB for 7000 glyphs).
- `ysp/gfx.h` stays a public-domain header with f32 data and documented
  exactness in linear light.

Use HarfBuzz as an outline source in the pack tool only: for variable-font
instances and CFF2, draw with HarfBuzz into `ysp/outline.h`'s builder and
write the same curve-set format. Its outlines equal `ysp/outline.h`'s
reader on static fonts (except one Arial composite), at the same cost,
and no header depends on HarfBuzz. hb-gpu's COLR paint encoder is a
candidate to study if color emoji ever enter the plan.

## Not tested

- Shaping of scripts outside the corpus: Myanmar, Khmer, Sinhala, Syriac,
  Mongolian, Tibetan, Hangul, and emoji (off by request).
- Positions of vertical text; vertical layout in Skribidi or kb (they have
  none).
- Explicit bidi controls (U+202A to U+2069) in any candidate.
- Justification (no candidate has it).
- Font fallback to fonts outside the given list (Chromium's system
  fallback was avoided by the corpus).
- Fonts with face indices other than 0 in a collection.
- Skribidi's layout cache (`skb_layout_cache.h`), its rasterizer, icons and
  canvas.
- The patched Skribidi's timing, and its tests with MSVC and emcc.
- The designer's real path: Skribidi in a browser page with composition
  events, a hidden text input and screen readers. The probe ran wasm in
  node.
- Untrusted-font robustness of any candidate (no fuzzing).
- hb-gpu on renderers other than the Iris Xe, on WebGPU, Metal or D3D;
  hb-gpu's dilation vertex helper; its stem darkening; its paint encoder.
- `ysp/gfx.h` curve runs for whole laid-out lines against Edge (8.1 drew
  single glyphs).
- libunibreak 7.0 and 8.0, SheenBidi 3.0.0 and budouxc's newer commits.
