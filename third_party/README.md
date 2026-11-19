# third_party: the vendored layout stack

Skribidi and its dependencies, for `pack/layout` (docs/layout.md).
Built only with the CMake option `YSP_BUILD_LAYOUT` (default OFF); no
`ysp/*.h` header uses anything here.

The trees are not in git (decided 2026-10-08, to keep the repository
small). Git tracks the recipe: this README, `MANIFEST.sha256`, `patches/`,
`CMakeLists.txt` and `.gitattributes`. Fetch the trees once before you
configure with `YSP_BUILD_LAYOUT=ON`:

```sh
uv run --no-project python tools/vendor_layout.py --write
```

The script downloads each pin by its full commit SHA, applies the patches
and checks every file against `MANIFEST.sha256`, so the trees are the same
bytes on every machine. Without them, configure stops and prints that
command. CI runs it with a tarball cache keyed on the manifest. Decision record:
[docs/layout_probe.md](../docs/layout_probe.md).

## Components

| Directory | Upstream | Pin | License files kept | Local patches |
|---|---|---|---|---|
| `skribidi/` | https://github.com/memononen/skribidi | `dee63d6ba76aeddd49dea6d1b2508cf9aa391f46` (2026-08-17) | `LICENSE` (MIT) | `patches/skribidi-0001-bidi-run-merge.patch`, `patches/skribidi-0002-l1-trailing-whitespace.patch`, `patches/skribidi-0003-font-fallback.patch`, `patches/skribidi-0004-emoji-family-fallback.patch`, `patches/skribidi-0005-text-grapheme-breaks.patch`, `patches/skribidi-0006-caret-split-grapheme.patch`, `patches/skribidi-0007-monotone-clusters.patch`, `patches/skribidi-0008-editor-empty-text.patch`, `patches/skribidi-0009-line-end-offset.patch`, in that order |
| `harfbuzz/` | https://github.com/harfbuzz/harfbuzz | tag `14.6.0`, `c7a7457b7385f33178e8cf87615ca077a810bbe7` | `COPYING` (Old MIT), `AUTHORS`, `src/ms-use/COPYING` (MIT; the USE table is generated from its data) | none |
| `sheenbidi/` | https://github.com/Tehreer/SheenBidi | `83f77108a2873600283f6da4b326a2dca7a3a7a6` (2025-06-06) | `LICENSE` (Apache-2.0) | none |
| `libunibreak/` | https://github.com/adah1972/libunibreak | tag `libunibreak_6_1`, `304585d8e2d63187507368d612c3d5fff1486368` | `LICENCE` (zlib), `AUTHORS` | none |
| `budouxc/` | https://github.com/memononen/budouxc | `a044d49afc654117fac7623fff15bec15943270c` (2025-03-29) | `LICENSE` (MIT); `LICENSE-BudouX` (Apache-2.0) | none |

- `budouxc/LICENSE-BudouX` is google/budoux's `LICENSE` at
  `898b6a11bccc66117f7de8b3e5db65e9cfdfb8ed` (2024-01-15). budouxc's
  `src/model_*.h` are converted from BudouX's models, and budouxc ships no
  license for them; its `models/ja.json` equals google/budoux's at that
  commit byte for byte.
- Apache-2.0 notices: neither SheenBidi nor google/budoux has a NOTICE
  file, so section 4(d) adds nothing to carry; section 4(a) is met by the
  two license files above. Neither source is modified.
- Skribidi's pins of the others are HarfBuzz 11.0.0, SheenBidi `83f7710`,
  libunibreak 6.1 and budouxc `a044d49`. ysp pins HarfBuzz 14.6.0 instead
  (docs/layout.md, Vendoring).

## What is kept, what is trimmed

Kept: the license files above, and the include closure of the sources that
`CMakeLists.txt` here compiles (gcc's `-MM` output; MSVC's `/showIncludes`
gives the same HarfBuzz list). 463 files, 7725840 bytes:

| Directory | Files | Bytes | Kept |
|---|---|---|---|
| `harfbuzz/` | 332 | 5167534 | `src/harfbuzz.cc` and the 328 `src/*.h`, `*.hh`, `*.cc` and `src/OT/` files it includes |
| `skribidi/` | 33 | 902317 | 11 `src/*.c` (layout, editor, rich text and what they need), `emoji_presentation_scanner.c` (included by `skb_common.c`), their internal headers, 12 of 15 `include/skribidi/*.h` |
| `libunibreak/` | 27 | 760171 | the 25 `src/` files of its `Makefile.gcc` build |
| `sheenbidi/` | 63 | 609024 | `Source/` and `Headers/SheenBidi/` as `SheenBidi.c` includes them |
| `budouxc/` | 8 | 286794 | `src/budoux.c`, `src/model_*.h`, `include/budoux.h` |

Trimmed, for every component: tests, fuzz corpora, fonts, documentation,
examples, screenshots, CI files, upstream build files (CMake, meson,
autotools, Makefiles), generators and their Unicode input data. In
particular:

- `skribidi/`: `example/`, `test/`, `extern/glad/`, `screenshots/`,
  `convert_emoji_data.py`, `emoji-data.txt`, `readme.md`, `CMakeLists.txt`,
  `src/CMakeLists.txt`, `src/skribidi.natvis`, and the canvas, rasterizer,
  image atlas and layout cache (`skb_canvas.c`, `skb_rasterizer.c/.h`,
  `skb_image_atlas.c/.h`, `skb_layout_cache.c/.h`). `skb_canvas.h` is kept:
  `skb_icon_collection.h` includes it.
- `harfbuzz/`: `test/`, `perf/`, `docs/`, `util/`, `subprojects/`, `.ci/`,
  `src/` files outside the amalgamation's closure (`hb-subset*`, `hb-gpu*`,
  `src/rust/`, `src/wasm/`, `src/graph/`, `gen-*.py`, `*.rl`, `test-*.cc`,
  `meson.build`), and the `src/ms-use/` data files.
- `sheenbidi/`: `Tests/`, `Tools/`, build files.
- `libunibreak/`: `tools/`, the `generate_*.py` and `*.sed` generators, the
  `*Test.txt` data, `tests.c`, build files.
- `budouxc/`: `models/*.json` (the headers hold the same data),
  `convert.py`, `example/`, build files.

## The patches

All but 0005, 0008 and 0009 change `src/skb_layout.c`; 0003 also changes
`src/skb_font_collection.c` and `include/skribidi/skb_font_collection.h`,
0005 changes `src/skb_text.c` only, 0008 changes `src/skb_editor.c`,
`src/skb_text.c` and `src/skb_rich_layout.c`, and 0009 changes
`src/skb_editor.c` only. They apply in this order:

1. `patches/skribidi-0001-bidi-run-merge.patch` changes the merge
   condition in `skb__line_append_shaping_run()`: runs merge only in LTR
   and at the same bidi level. SHA-256
   `ff058faa3c16aa62ec0af21ce30e2db6eee9bce380fec3d51970ceb582164788`.
   Why: docs/layout_probe.md 2.3.
2. `patches/skribidi-0002-l1-trailing-whitespace.patch` applies Unicode's
   bidi rule L1 to each line in `skb__finalize_line()` (new
   `skb__apply_rule_l1()`), and lets the line breaker leave trailing
   whitespace out of the fit in either direction. SHA-256
   `36ce7aec18d9f2b1256ee4374522ef36e87b6850be8bab4ba0bd720c162fb1b1`.
   Why: docs/layout.md, "Known issues" and "The patches".
3. `patches/skribidi-0003-font-fallback.patch` lets `skb__itemize()` take
   a codepoint that none of the fonts of its run's script has from the
   first font of the collection that has its grapheme cluster (new
   `skb_font_collection_match_fallback_fonts()`). SHA-256
   `bf6296af91c73737b18637e57f86f95fa443fad83848013cb427a5f47a2ca225`.
   Why: docs/layout.md, "Patch 0003: font fallback past the script".
4. `patches/skribidi-0004-emoji-family-fallback.patch` lays a run whose
   font family has no font (an emoji run with no emoji font, a text run
   with emoji fonts only) out in the other family instead of skipping it,
   and lets the fallback of 0003 look in the other family after the run's.
   SHA-256 `ce9aa9eea3585162bb2aff62725f883465f63e54e2ea3d3e4bc21c847add4ab2`.
   Why: docs/layout.md, "Patch 0004: a run whose family has no font".
5. `patches/skribidi-0005-text-grapheme-breaks.patch` makes each edit of
   `skb_text` (insert, append, remove) recompute the grapheme breaks from
   the start of the grapheme before the edit to the end of the text (new
   `skb__update_grapheme_breaks()` in `src/skb_text.c`). SHA-256
   `11081a2421b97f0111040b4934d98bfc349471a5b1647f5a34a07552c65ab69c`.
   Why: docs/layout.md, "Patches 0005 and 0006: editing Devanagari".
6. `patches/skribidi-0006-caret-split-grapheme.patch` groups neighboring
   clusters of a run in the caret iterator until the group starts and ends
   at grapheme boundaries, and selects a cluster with no break whole
   (`src/skb_layout.c`). SHA-256
   `dfa7cdbd1cc8e05ffb4c93a7135a348d193a9084cc31b3cd1aff0065d3ca12d5`.
   Why: docs/layout.md, "Patches 0005 and 0006: editing Devanagari".
7. `patches/skribidi-0007-monotone-clusters.patch` merges glyphs whose
   HarfBuzz cluster values are out of order in the run's direction into
   one cluster before `skb__shape_run()` takes the clusters' text ranges
   (new `skb__merge_non_monotone_clusters()` in `src/skb_layout.c`).
   SHA-256
   `f8459c8b13e50157995ca0769329e7698cbd4ce82a33666f0aebcd93eacf191c`.
   Why: docs/layout.md, "Patch 0007: cluster values out of order".
8. `patches/skribidi-0008-editor-empty-text.patch` fixes faults of the
   editor on empty texts and at a text's ends: moves, line and column
   queries, the first IME composition and the selection bounds in an
   editor with no paragraph (`src/skb_editor.c`,
   `src/skb_rich_layout.c`), reads of text properties past and before the
   text in the word functions (`src/skb_editor.c`), and `memcpy()` with
   NULL for an empty append (`src/skb_text.c`). SHA-256
   `4b08c2781fbacaca9b7b0988cd1a6c30e4728e9cb340bb34b715d84c232dac21`.
   Why: docs/layout.md, "Patch 0008: the editor at its text's edges".
9. `patches/skribidi-0009-line-end-offset.patch` makes
   `skb_editor_get_line_end_at()` (`src/skb_editor.c`) give the step that
   keeps the caret before a line's last control character an offset in
   the paragraph, as that step compares it with one. End no longer stops
   after the LF on each paragraph but the first. SHA-256
   `f0c97fa5dfa25375d7f997bdc398931fea71ca27f3b866af79aaec11976bcccb`.
   Why: docs/layout.md, "Patch 0009: End on each line but the first".

The tree here holds the patched source; the script applies the patches,
in order, to a fresh download.

## Recreate and verify

`tools/vendor_layout.py`, standard library only, needs `git` (for
`git apply`) and network:

```sh
uv run --no-project python tools/vendor_layout.py              # verify: download, patch, compare byte for byte
uv run --no-project python tools/vendor_layout.py --write      # rewrite the trees here from upstream
uv run --no-project python tools/vendor_layout.py --cache DIR  # keep the tarballs between runs
```

Verify compares every file with the download and with `MANIFEST.sha256`,
and reports a file here that the manifest does not list. On 2026-10-07 it
reported `ok: 463 files, 7719645 bytes` (both patches applied); on
2026-11-18, with the third, `ok: 463 files, 7725840 bytes`; with the
fourth, `ok: 463 files, 7727613 bytes`; with the seventh,
`ok: 463 files, 7732502 bytes`; with the eighth, `ok: 463 files, 7734365
bytes`; with the ninth, `ok: 463 files, 7734516 bytes`.

`.gitattributes` here turns off line-ending conversion, so a Windows
checkout with `core.autocrlf=true` keeps the files as upstream has them.

## Update a pin

1. Change the commit in `PINS` in `tools/vendor_layout.py` and in
   `pack/layout.c`, and here.
2. `uv run --no-project python tools/vendor_layout.py --extract DIR`: the
   whole upstream trees, Skribidi patched (the patches must still apply).
3. `uv run --no-project python tools/vendor_layout.py --closure DIR > list.txt`
   (needs gcc and g++ on PATH): the file list.
4. `uv run --no-project python tools/vendor_layout.py --write --list list.txt`:
   the trees and a new manifest.
5. Build with `YSP_BUILD_LAYOUT=ON` and run `test_layout`; regenerate
   `tests/layout/corpus_ref.txt` only if Chromium's version is the reason.
   Every difference in glyph ids, order or positions goes in
   docs/layout.md (policy point 4).

## Add a patch

1. Make the change in `skribidi/` here (the earlier patches applied) and
   write it as `patches/skribidi-NNNN-name.patch`: a git-style diff with
   `a/` and `b/` paths relative to `skribidi/`, LF line endings, and a
   header that says what it fixes and which patches it applies after.
2. Add the file name to `PATCHES` in `tools/vendor_layout.py` and to the
   `foreach` in `CMakeLists.txt` here, in the order they apply.
3. Put the new SHA-256 of each file it changes in `MANIFEST.sha256`.
4. `uv run --no-project python tools/vendor_layout.py`: the patches must
   apply to a fresh download and give the tree here (`ok`).
5. Configure again (CMake hashes the patches at configure time), build,
   and run `test_layout`; the logged patch field lists one hash more.

## lodepng, for the pack tool

`third_party/lodepng/` holds lodepng (zlib license) at commit
`e584a8490db1b76b81bf298c11c09d3b55d9c270` (2026-10-06): `lodepng.cpp`,
`lodepng.h` and `LICENSE`, unchanged. The pack tool (CMake option
`YSP_BUILD_PACK`, docs/pack.md) decodes PNG textures with it; the layout
library does not need it. The core pack tool (`YSP_BUILD_PACK=ON` without
`YSP_BUILD_LAYOUT`) needs only this file set, not the layout trees above. Like the trees above, it is fetched, not
committed:

```sh
uv run --no-project python tools/vendor_pack.py --write   # fetch and verify
uv run --no-project python tools/vendor_pack.py           # verify only
```

`tools/vendor_pack.py` downloads each file at the full commit SHA and
compares its SHA-256 with the value in the script; nothing else is
accepted. CMake compiles `lodepng.cpp` as C from a copy named
`lodepng.c`, with its warnings off, and records the commit in each
manifest, with `,tree-differs` when the file's SHA-256 is not the pinned
one. To update the pin, change `COMMIT` and the three hashes in the script,
the pin and hash in `pack/CMakeLists.txt`, and this section.
