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
| `skribidi/` | https://github.com/memononen/skribidi | `dee63d6ba76aeddd49dea6d1b2508cf9aa391f46` (2026-08-17) | `LICENSE` (MIT) | `patches/skribidi-0001-bidi-run-merge.patch`, then `patches/skribidi-0002-l1-trailing-whitespace.patch` |
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
gives the same HarfBuzz list). 463 files, 7719645 bytes:

| Directory | Files | Bytes | Kept |
|---|---|---|---|
| `harfbuzz/` | 332 | 5167534 | `src/harfbuzz.cc` and the 328 `src/*.h`, `*.hh`, `*.cc` and `src/OT/` files it includes |
| `skribidi/` | 33 | 896122 | 11 `src/*.c` (layout, editor, rich text and what they need), `emoji_presentation_scanner.c` (included by `skb_common.c`), their internal headers, 12 of 15 `include/skribidi/*.h` |
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

Both change `src/skb_layout.c` and apply in this order:

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
reported `ok: 463 files, 7719645 bytes` (both patches applied).

`.gitattributes` here turns off line-ending conversion, so a Windows
checkout with `core.autocrlf=true` keeps the files as upstream has them.

## Update a pin

1. Change the commit in `PINS` in `tools/vendor_layout.py` and in
   `pack/layout.c`, and here.
2. `uv run --no-project python tools/vendor_layout.py --extract DIR`: the
   whole upstream trees, Skribidi patched (the patch must still apply).
3. `uv run --no-project python tools/vendor_layout.py --closure DIR > list.txt`
   (needs gcc and g++ on PATH): the file list.
4. `uv run --no-project python tools/vendor_layout.py --write --list list.txt`:
   the trees and a new manifest.
5. Build with `YSP_BUILD_LAYOUT=ON` and run `test_layout`; regenerate
   `tests/layout/corpus_ref.txt` only if Chromium's version is the reason.
   Every difference in glyph ids, order or positions goes in
   docs/layout.md (policy point 4).

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
