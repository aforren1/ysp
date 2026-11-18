# Color emoji font survey: what COLRv0 and COLRv1 fonts use, 2026-11-18

This note records a survey of six freely licensed color emoji fonts. It
asks which COLRv0 and COLRv1 operations the fonts use, and how many
glyphs need each one, to size the work for color emoji in ysp. The plan
that it sizes is not built: the pack tool flattens each color glyph with
HarfBuzz's paint API into layers (a clip outline, a transform and a
paint), `ysp/gfx.h` draws each layer as a curve run with a fill, and
bitmap (CBDT, sbix) and SVG color fonts are refused when the pack is
built. `ysp/outline.h` stays monochrome. What gfx draws today:
`include/ysp/gfx.h`, CURVE RUNS (one solid palette color per item; paint
is refused on a run). The Font row of [rig_spec.md](rig_spec.md) section
5.2 and [layout.md](layout.md) ("Not done": color glyphs) are the
context.

The tool is `tests/probe/colr/colr_survey.c` (a C program against the
pinned HarfBuzz) with `tests/probe/colr/run.sh`. The fonts are not in the
repository; the URLs and hashes below get the same bytes.

## Recommendation

1. **Build the layer model with these paints, ranked by the glyphs that
   need them** (counts are glyphs; the fonts are Noto Color Emoji
   COLRv1, Twemoji Mozilla, OpenMoji COLRv0 and COLRv1, Fluent Emoji
   Flat and Fluent Emoji Color; sections 1 to 5 have the per-font tables):

   | Rank | Feature | Glyphs that need it | How |
   |---|---|---|---|
   | 1 | Solid fill of one glyph outline, SRC_OVER | every glyph of every font (22,981) | a curve run item, as today |
   | 2 | Affine transform of an outline | 11,870 (OM1 3,800, FC 3,092, Noto 2,776, FF 2,202) | bake into the outline in the pack tool |
   | 3 | Alpha below 1 in a solid or a stop | 5,419 (FC 3,082, Noto 2,198, FF 77, Tw 46, OM and OM1 8 each) | per-layer RGBA |
   | 4 | Linear gradient | 4,859 (FC 3,122, Noto 1,735, FF 2) | gradient fill on a run item |
   | 5 | Radial gradient | 4,844 (FC 3,038, Noto 1,805, FF 1) | gradient fill |
   | 6 | A gradient with its own affine (elliptical radial) | 4,559 (FC 3,022, Noto 1,537) | a 2x3 matrix per gradient |
   | 7 | SOFT_LIGHT blend of an SRC_IN-masked gradient | 262 (Noto, flags only) | offscreen group plus a blend that reads the backdrop |
   | 8 | Group opacity (PaintComposite SRC_IN over one unclipped solid) | 74 (FF 34, Noto 32, FC 8) | per-layer alpha where layers do not overlap; else an offscreen group |
   | 9 | A clip rectangle that cuts the outline | 16 (FC 13 by up to 5.8 units, FF 2 by up to 10.5, Noto 1 by 0.9) | a per-emoji clip box |
   | 10 | Two-point radial (start radius 0, start circle inside the end circle) | 15 (Noto) | the two-point form in the radial shader |

   Abbreviations: Noto (Noto Color Emoji COLRv1), Tw (Twemoji Mozilla),
   OM and OM1 (OpenMoji COLRv0 and COLRv1), FF (Fluent Emoji Flat), FC
   (Fluent Emoji Color).
2. **Every row but 7, with group opacity flattened to per-layer alpha,
   draws three fonts exactly and the other three except for few
   glyphs.** Twemoji and both OpenMoji builds need rows 1 to 3 at most
   (Twemoji and OpenMoji COLRv0 need no transforms) and draw exactly.
   Fluent Flat needs 6 gradients in 3 glyphs and 36 group-opacity
   composites, 35 of them with disjoint layer boxes, where
   per-layer alpha is exact: at most 1 glyph is not exact. Fluent Color:
   at most 10 composites in at most 8 glyphs. Noto: the 262 flags, and
   at most 29 composites in at most 32 other glyphs. "At most" because
   the overlap test uses layer boxes, an upper bound. An offscreen group
   (a target per composite) makes all of these exact except the flags'
   soft light, which also needs a shader that reads the target.
3. **The minimum gradient feature set is small.** Every gradient in
   every font uses the pad extend: no repeat, no reflect. No font has a
   sweep gradient. Color stops: median 2 per gradient in every font with
   gradients, max 13 (Noto), 11 (FC), 4 (FF); a fixed array of 16 stops
   holds all 160,442 gradients. No stop lies outside [0, 1]. Radials are
   concentric with start radius 0 except 15 in Noto (focal point inside
   the end circle). The COLR specification asks for interpolation in
   linear light with premultiplied alpha (CPAL, "Interpolation of
   colors"), which is how gfx already works.
4. **Bake transforms into the outlines in the pack tool.** A quadratic
   Bezier curve maps exactly under an affine map, so baking is exact, and
   the baked glyphs stay unturned, so gfx's exact-area coverage applies
   (a turned item falls back to the rays). The cost is storage: one
   outline per distinct (glyph, transform) pair. As bounds, the segments
   of the font's distinct clip glyphs against the segments of every use:
   Noto 0.78 M to 2.38 M, Fluent Color 0.42 M to 2.51 M, OpenMoji COLRv1
   0.31 M to 1.00 M. Linear gradients bake exactly too (an affine map
   keeps lines of equal color parallel). Radial gradients under a
   non-similarity transform become ellipses (90,656 of 93,707 radials in
   Fluent Color, 7,693 of 9,384 in Noto), so each gradient carries its
   own 2x3 matrix. A gradient's transform differs from its outline's in
   93,594 of 142,875 Fluent Color gradients and 8,118 of 17,561 Noto
   gradients: the layer needs the outline and the paint in separate
   spaces.
5. **Do not build these: no surveyed font uses them.** Sweep gradients;
   repeat and reflect; the foreground color (palette entry 0xFFFF);
   palettes other than palette 0 (every font has one palette); COLR
   variations (no `fvar`, no ItemVariationStore, no DeltaSetIndexMap);
   PaintColrGlyph; a fill under two or more glyph clips; a clip
   rectangle under a turn; composite modes other than SRC_OVER, SRC_IN
   and SOFT_LIGHT; cubic outlines (every font is `glyf`, 0 cubics).
   Refuse them by name in the pack tool, as the artwork subset refuses
   SVG features, so a font that uses one is not drawn wrong.
6. **Noto's flags are the only blend mode, and they can be left out.**
   All 262 SOFT_LIGHT composites are in the flag glyphs (glyph ids 3763
   to 4024, reached by regional-indicator and tag sequences). Each flag
   draws its layers, then draws them again as the backdrop of an SRC_IN
   that masks a linear gradient to the flag's shape, and soft-lights the
   result onto the first copy. Soft light needs the backdrop's color at
   each pixel, which a layer with fixed-function blending does not have.
   Three options: an offscreen group (gfx has targets; a target per
   composite and a shader that reads it), approximate soft light, or
   refuse the flags. Noto publishes `Noto-COLRv1-noflags.ttf` beside the
   full font, which suggests the third: draw flags from another source,
   or from Noto with the soft light left out, named as an approximation.
7. **Size the draw path for Fluent Color and Noto, not for Twemoji.**
   Per emoji, median and p99: layers 50 and 150 (FC), 22 and 281
   (Noto; 22 and 76 without flags), 8 and 27 (Tw); overdraw (the layers'
   boxes summed over the ink box) 5.4x and 17x (FC), 2.1x and 7.5x
   (Noto), 1.7x and 3.9x (Tw). The largest glyphs: Noto 3,285 layers and
   50,162 outline segments (a flag), Fluent Color 857 layers and 8,904
   segments. A frame budget for emoji is therefore per glyph, not per
   font: measure the gfx cost of a 50-layer and a 857-layer emoji when
   gradient runs exist.
8. **Refuse a glyph that HarfBuzz stops early.** HarfBuzz walks at most
   64 levels and 16,384 paint-graph edges per glyph
   (`HB_MAX_NESTING_LEVEL`, `HB_MAX_GRAPH_EDGE_COUNT`) and then returns
   without an error. The deepest surveyed glyph had 8 levels and the
   largest made 10,045 callbacks, so none came near; a hostile font can.
   The pack tool should count edges itself, or compare against a
   separate walk, and refuse a glyph that reaches a limit.

## Decisions (2026-11-18)

- Scope of the first build: the minimum feature set above (solid fills
  in glyph clips, transforms baked into the outlines, per-layer alpha,
  linear and radial gradients with their own 2x3 matrix, the clip box, and
  two-point radials). The pack tool refuses everything else by name.
  Offscreen groups (SOFT_LIGHT for Noto's flags, group opacity over
  overlapping layers) are a later addition; until then, use Noto's build
  without flags or another font.
- Blending: linear light, as the rest of `ysp/gfx.h`. The manual states
  that translucent layers and edges differ from a browser, which blends
  on sRGB-encoded values.
- Default emoji font: Twemoji (Mozilla's COLRv0 build; art CC BY 4.0).

## Conditions

- Linux desktop: Intel Core i5-14500, Debian 12, kernel
  6.11.5-1-liquorix-amd64. gcc and g++ 12.2.0, `-O2`; HarfBuzz compiled
  with `-std=c++17 -fno-exceptions -fno-rtti`, as `third_party/` does.
- HarfBuzz 14.6.0 (`c7a7457b7385f33178e8cf87615ca077a810bbe7`), the
  repository's pin, fetched by `tools/vendor_layout.py --write`. It has
  the paint API (`hb_font_paint_glyph`, since 7.0.0) and the newer
  `fill_glyph`, `push_group_for` and `color_glyph` callbacks.
- Single-threaded, one font at a time. Each font took 0.03 to 0.16 s.
  This is not a timing measurement; no time below depends on it.
- The tool also ran under ASan and UBSan (`setarch -R`) on all six
  fonts: no report, and the same output as the optimized build.

### Fonts

| Font | Source | Version | File | Bytes | sha256 | License |
|---|---|---|---|---|---|---|
| Noto Color Emoji, COLRv1 | https://raw.githubusercontent.com/googlefonts/noto-emoji/1ffdd21391dd1f25c081fa93a9dea0c7c029442b/2D/fonts/Noto-COLRv1.ttf | commit `1ffdd21` (2026-09-17, "Add Emoji 18 fonts") | `Noto-COLRv1.ttf` | 5,038,236 | `b8e25ea68db82f9e4d0aee921f4420be2be39887bd5c893a2ad98710531f9d0c` | OFL 1.1 (`2D/fonts/LICENSE`) |
| Twemoji Mozilla (COLRv0) | https://github.com/mozilla/twemoji-colr/releases/download/v0.7.0/Twemoji.Mozilla.ttf | v0.7.0, `c0460fd` | `Twemoji.Mozilla.ttf` | 1,474,284 | `6d90152ee0d29e82fe2a87793af5aa4b7ad13e6538360889e141e81ed299ee8e` | art CC BY 4.0, code Apache-2.0 (`LICENSE.md`) |
| OpenMoji, COLRv0 | `OpenMoji-color-glyf_colr_0/` in https://github.com/hfg-gmuend/openmoji/releases/download/17.0.0/openmoji-font.zip | 17.0.0, `f9fc506` | `OpenMoji-color-glyf_colr_0.ttf` | 2,681,320 | `8376fee074649ece235faba0d157d851f3d93632f0540ad1165feba1aba9ba37` | CC BY-SA 4.0 (`LICENSE.txt`) |
| OpenMoji, COLRv1 | same zip, `OpenMoji-color-glyf_colr_1/` | 17.0.0 | `OpenMoji-color-glyf_colr_1.ttf` | 2,401,396 | `b6c86e3d95f557dae9a7d59a993b5a6f2274c401d6d975747d43612c6fab236d` | CC BY-SA 4.0 |
| Fluent Emoji Flat | https://raw.githubusercontent.com/tetunori/fluent-emoji-webfont/v0.8.5/dist/FluentEmojiFlat.ttf | v0.8.5, `b7fc5ad` | `FluentEmojiFlat.ttf` | 26,888,932 | `951da552e63bd9e6b4ee375fe711a0cf523553568dd9c203888a43f872ab0dbb` | MIT (the build), MIT (Microsoft's fluentui-emoji art) |
| Fluent Emoji Color | same tag, `dist/FluentEmojiColor.ttf` | v0.8.5 | `FluentEmojiColor.ttf` | 87,758,820 | `6292f3ff969d800d55486e01b293248ccd9ee71a17e4f9770b4abea13d9cad25` | MIT, MIT |

- The OpenMoji zip (`1de61d8dcb38d0bebbdd13c7057a65f5d957ead36ce4b61bd635f84e6f3cadba`,
  56,844,019 bytes) also holds CBDT, sbix, SVG (`picosvgz`,
  `untouchedsvgz`), `colr0_svg` and `colr1_svg` builds; only the two
  `glyf_colr` builds were surveyed.
- The Fluent fonts are third-party builds (nanoemoji) of Microsoft's
  MIT-licensed fluentui-emoji art, not Microsoft's own fonts. Each file
  holds COLRv1 plus CBDT and SVG: of Fluent Color's 87.8 MB, COLR is
  5.3 MB, `glyf` 2.2 MB, CBDT 38.8 MB and SVG 41.0 MB. The survey reads
  COLR only; a pack built from these fonts must drop the CBDT and SVG
  tables, not refuse the font.
- Not on this machine, from their documentation:
  - Segoe UI Emoji (Windows 11) is a hybrid: a COLRv1 set (the "3D"
    Fluent emoji, over 3,000 glyphs moved to COLRv1 with nanoemoji, per
    Microsoft's design team), the COLRv0 2D set from Windows 8.1, and a
    monochrome set. A renderer without COLRv1 falls back to COLRv0.
    Sources: https://microsoft.design/articles/bringing-new-emoji-to-windows-11/
    and https://forest.watch.impress.co.jp/docs/news/1572154.html.
  - Apple Color Emoji is an `sbix` font (PNG bitmaps per size), which
    the plan refuses. Source: Apple's TrueType Reference Manual, `sbix`
    table.

## Method

`colr_survey` opens each font with the scale set to units per em, so
HarfBuzz's font transform is the identity and every coordinate is in font
units. For every glyph with a COLRv1 paint (`hb_ot_color_glyph_has_paint`)
or COLRv0 layers (`hb_ot_color_glyph_get_layers`), it calls
`hb_font_paint_glyph` with palette 0 and a set of callbacks that keep one
stack of transforms, clips and groups and count:

- Fills, the layers of the flattened model: `color`, `linear_gradient`,
  `radial_gradient`, `sweep_gradient`, `image`, and `fill_glyph`.
  HarfBuzz 14 calls `fill_glyph` (a solid in one glyph clip) for every
  COLRv0 layer and for every COLRv1 PaintGlyph whose paint is solid, so
  most solids arrive that way.
- Clips: `push_clip_glyph`, `push_clip_rectangle` (the ClipList box at
  the root, and for PaintColrGlyph), and `color_glyph` (PaintColrGlyph).
- Transforms: each `push_transform` matrix classified as identity,
  translate, uniform or non-uniform scale, rotate, rotate with uniform
  scale (or a mirrored similarity), skew, or general (tolerance 1e-4
  relative). HarfBuzz calls `push_transform` once per Paint transform,
  with the around-center forms folded into one matrix, plus identity
  font transforms around every PaintGlyph. The tables count the
  non-identity ones.
- Groups: `push_group_for(mode)`. HarfBuzz 14 makes groups only for
  PaintComposite: an outer SRC_OVER group around the backdrop, and an
  inner group with the composite mode around the source. Each inner group
  with a mode other than SRC_OVER is classified at its pop by its
  backdrop and source: "group opacity" (SRC_IN, the backdrop one
  unclipped solid, no nested composite), "mask" (SRC_IN, the source one
  unclipped fill), "blend of a source that holds a nested composite", or
  other. For group opacity it also tests whether the source's fills have
  overlapping boxes (a box is the intersection of a fill's clip-glyph
  boxes; overlap of boxes is an upper bound on overlap of shapes).
- Gradients: extend mode, stop count, stops outside [0, 1], stops with
  alpha below 1, foreground stops; radials as concentric (start radius 0
  or above) or two-point; sweeps as full or partial and under a mirror;
  the cumulative transform at the fill, whether it is a similarity, and
  whether it differs from the innermost clip glyph's.
- Per fill: the number of glyph clips over it, whether any clip
  rectangle cuts the clip glyph's box (the outline's exact extremes under
  the cumulative transform, not its control points; tolerance 0.5 font
  units), foreground and alpha.
- Per glyph: layers; nesting depth (the stack's non-identity entries);
  outline segments of every clip glyph use, from `hb_font_draw_glyph`
  (lines, quadratics and cubics), summed over the uses and over the
  distinct glyphs; overdraw, the fills' boxes summed over the box of
  them all.
- Per font: table sizes, the COLR header's v1 offsets (ClipList,
  DeltaSetIndexMap, ItemVariationStore), CPAL palettes and entries, and
  `fvar`.

`--list` writes one line per glyph; `--dump GID` prints a glyph's calls
nested by the stack (the Noto flag and glasses structures in
Recommendation 6 and sections 3.1 and 3.2 come from it).

Run, from the repository root:

```sh
uv run --no-project python tools/vendor_layout.py --write
sh tests/probe/colr/run.sh --list out.tsv Noto-COLRv1.ttf Twemoji.Mozilla.ttf \
   OpenMoji-color-glyf_colr_0.ttf OpenMoji-color-glyf_colr_1.ttf \
   FluentEmojiFlat.ttf FluentEmojiColor.ttf
```

Checks: the COLRv0 fonts' fills equal their COLR layer records (Twemoji
33,179 and 33,179; OpenMoji 62,305 and 62,305), and OpenMoji's COLRv1
build gives the same 62,305 fills and the same layer and segment
percentiles as its COLRv0 build of the same art. Every glyph's stack was
balanced and never passed 256 entries.

## 1. The fonts

| | Noto | Tw | OM (v0) | OM1 (v1) | FF | FC |
|---|---|---|---|---|---|---|
| File bytes | 5,038,236 | 1,474,284 | 2,681,320 | 2,401,396 | 26,888,932 | 87,758,820 |
| COLR version | 1 | 0 | 0 | 1 | 1 | 1 |
| COLR bytes | 1,093,851 | 154,864 | 276,204 | 601,613 | 398,384 | 5,268,928 |
| `glyf` bytes | 3,456,424 | 912,704 | 2,114,902 | 1,624,572 | 810,179 | 2,195,210 |
| Other color tables | none | none | none | none | CBDT, SVG | CBDT, SVG |
| Color glyphs | 4,012 | 3,689 | 4,495 | 4,495 | 3,145 | 3,145 |
| All glyphs | 42,330 | 13,723 | 41,369 | 22,208 | 13,248 | 33,351 |
| Units per em | 1024 | 512 | 1024 | 1024 | 1024 | 1024 |
| CPAL palettes, entries | 1, 6,396 | 1, 1,063 | 1, 37 | 1, 28 | 1, 259 | 1, 52,680 |
| ClipList boxes | 2,006 | | | 2,907 | 1,887 | 2,243 |
| Variations (`fvar`, ItemVariationStore) | no | no | no | no | no | no |

## 2. Operations

Counts of operations over all color glyphs of a font.

| Operation | Noto | Tw | OM | OM1 | FF | FC |
|---|---|---|---|---|---|---|
| Fills (layers) | 131,247 | 33,179 | 62,305 | 62,305 | 48,883 | 174,846 |
| Solid | 113,686 | 33,179 | 62,305 | 62,305 | 48,877 | 31,971 |
| of which `fill_glyph` | 113,635 | 33,179 | 62,305 | 62,305 | 48,841 | 31,959 |
| Linear gradient | 8,177 | 0 | 0 | 0 | 5 | 49,168 |
| Radial gradient | 9,384 | 0 | 0 | 0 | 1 | 93,707 |
| Sweep gradient | 0 | 0 | 0 | 0 | 0 | 0 |
| Image | 0 | 0 | 0 | 0 | 0 | 0 |
| Glyph clip (with `fill_glyph`) | 130,934 | 33,179 | 62,305 | 62,305 | 48,847 | 174,834 |
| Clip rectangle | 4,012 | 0 | 0 | 4,495 | 3,145 | 3,145 |
| PaintColrGlyph | 0 | 0 | 0 | 0 | 0 | 0 |
| PaintComposite | 575 | 0 | 0 | 0 | 36 | 12 |
| Transform: translate | 11,088 | 0 | 0 | 8,411 | 3,162 | 40,896 |
| Transform: uniform scale | 7,223 | 0 | 0 | 6,092 | 6,526 | 17,915 |
| Transform: non-uniform scale | 2,744 | 0 | 0 | 2,013 | 356 | 7,910 |
| Transform: rotate | 528 | 0 | 0 | 509 | 69 | 2,033 |
| Transform: rotate and uniform scale | 4,551 | 0 | 0 | 1,508 | 2,312 | 23,708 |
| Transform: skew | 0 | 0 | 0 | 0 | 0 | 2 |
| Transform: general | 9,129 | 0 | 0 | 628 | 261 | 89,674 |

Fills with no glyph clip occur only inside composites (Noto 313, FF 36,
FC 12: the unclipped solid of group opacity and the masked gradient of
the flags). No fill is under two or more glyph clips, in any font.

## 3. Composite modes

| Mode and shape | Noto | Tw | OM | OM1 | FF | FC |
|---|---|---|---|---|---|---|
| SRC_IN, group opacity, source solid only | 33 | | | | 36 | 2 |
| SRC_IN, group opacity, source with gradients | 18 | | | | | 10 |
| SRC_IN, mask: one unclipped gradient over the flag's layers | 262 | | | | | |
| SOFT_LIGHT, source holds the SRC_IN mask | 262 | | | | | |
| Group opacity: source fills' boxes disjoint | 22 | | | | 35 | 2 |
| Group opacity: source fills' boxes overlap | 29 | | | | 1 | 10 |
| Glyphs with SRC_IN only | 32 | 0 | 0 | 0 | 34 | 8 |
| Glyphs with SOFT_LIGHT | 262 | 0 | 0 | 0 | 0 | 0 |

No other mode occurs. Fills inside a group with a mode other than
SRC_OVER: Noto 21,765 (522 gradients; nearly all the flags' second copy),
FF 75 (0), FC 48 (44).

### 3.1 Group opacity

nanoemoji writes an SVG group's `opacity` as PaintComposite SRC_IN with a
backdrop of one solid at that alpha and no glyph clip. Noto's glasses
(U+1F453), from `--dump`:

```
clip rect 64 128 1216 576
  group SRC_OVER
    solid #000000cc                 (backdrop: alpha 0.8, unclipped)
    group SRC_IN
      fill glyph 9604 solid #f9ca90ff
      fill glyph 9605 solid #f9ca90ff
  ...
```

The result is the source group at 0.8 alpha. Where the source's layers
do not overlap, this equals each layer at its alpha times 0.8 under
SRC_OVER, which the layer model draws. Where they overlap, the lower
layer shows through the upper one, which a group drawn opaque and then
faded does not do; exact drawing needs the group in a target.

### 3.2 Noto's flags

Each of the 262 flag glyphs is a SOFT_LIGHT PaintComposite whose
backdrop is the flag's layers and whose source is an SRC_IN composite:
the same layers again as its backdrop, and one unclipped linear gradient
as its source. Glyph 3830, a flag with 3,285 layers:

```
clip rect 32 -160 1248 864
  group SRC_OVER
    (1,642 layers of the flag)
    group SOFT_LIGHT
      group SRC_OVER
        (the same 1,642 layers)
        group SRC_IN
          linear (47 790) (890 -342) (-1085 -53) stops 3 extend 0
```

So the flags double their layers and outline segments: per flag, median
21 layers and 314 segments, p99 1,501 layers and 34,702 segments.

## 4. Gradients

| | Noto | FF | FC |
|---|---|---|---|
| Gradients | 17,561 | 6 | 142,875 |
| Extend pad, repeat, reflect | 17,561, 0, 0 | 6, 0, 0 | 142,875, 0, 0 |
| Stops: median, p99, max | 2, 7, 13 | 2, 4, 4 | 2, 4, 11 |
| Stops outside [0, 1] | 0 | 0 | 0 |
| Gradients with a stop of alpha below 1 | 8,718 | 3 | 96,250 |
| Foreground stops | 0 | 0 | 0 |
| Radial: concentric, start radius 0 | 9,369 | 1 | 93,707 |
| Radial: concentric, start radius above 0 | 0 | 0 | 0 |
| Radial: two-point (all start radius 0, start circle inside end) | 15 | 0 | 0 |
| Radial under a non-similarity transform (an ellipse) | 7,693 | 0 | 90,656 |
| Gradient transform differs from its clip glyph's | 8,118 | 1 | 93,594 |
| Gradient transform: identity | 7,129 | 4 | 24,380 |
| Gradient transform: translate | 1,379 | 1 | 11,879 |
| Gradient transform: uniform scale | 889 | 0 | 4,502 |
| Gradient transform: non-uniform scale | 2,167 | 0 | 7,690 |
| Gradient transform: rotate, or rotate and scale | 397 | 1 | 9,018 |
| Gradient transform: general | 5,600 | 0 | 85,406 |

Twemoji and both OpenMoji builds have no gradients.

## 5. Per glyph: layers, depth, outline cost, overdraw

Median, p99 and max over the color glyphs. Outline segments count every
line and quadratic of every clip glyph use (from `hb_font_draw_glyph`);
"distinct" counts each glyph once per emoji. No font has a cubic.

| | Noto | Noto, no flags | Tw | OM | OM1 | FF | FC |
|---|---|---|---|---|---|---|---|
| Glyphs | 4,012 | 3,750 | 3,689 | 4,495 | 4,495 | 3,145 | 3,145 |
| Layers | 22, 281, 3,285 | 22, 76, 230 | 8, 27, 44 | 12, 40, 80 | 12, 40, 80 | 15, 40, 83 | 50, 150, 857 |
| Nesting depth | 3, 6, 8 | 3, 6, 8 | 0, 0, 0 | 0, 0, 0 | 3, 3, 3 | 3, 3, 6 | 6, 6, 8 |
| Outline segments | 376, 6,412, 50,162 | 378, 1,103, 11,092 | 134, 437, 1,448 | 210, 578, 1,341 | 210, 578, 1,341 | 206, 491, 1,612 | 709, 2,575, 8,904 |
| Distinct segments | 336, 3,206, 24,541 | 342, 1,040, 11,080 | 134, 437, 1,448 | 208, 578, 1,341 | 190, 526, 1,341 | 187, 491, 1,589 | 260, 704, 7,201 |
| Overdraw, % | 211, 746, 5,290 | 204, 456, 5,290 | 167, 387, 552 | 205, 463, 724 | 205, 463, 724 | 144, 325, 612 | 543, 1,722, 4,975 |
| Segments, all uses, font total | 2,382,875 | | 572,863 | 999,848 | 999,848 | 684,134 | 2,514,143 |
| Segments, distinct clip glyphs, font total | 776,958 (lines 175,061, quadratics 601,897) | | 207,074 | 520,090 | 307,812 | 172,682 | 423,542 |

The depth counts transforms, clips and groups that are not the identity.
The largest glyphs: Noto glyph 3979 (a flag, 3,067 layers, 50,162
segments) and, without flags, the lizard U+1F98E (230 layers, 11,092
segments, a group-opacity composite); Fluent Color glyph 451 (857 layers,
8,904 segments).

## 6. What the layered model cannot express exactly

The model: per emoji, an optional clip rectangle; per layer, one glyph
outline (its transform baked), an RGBA solid or a gradient (linear or
radial, with its own 2x3 matrix, up to 16 premultiplied linear-light
stops, pad), composited SRC_OVER.

| Case | Noto | FF | FC | What exact drawing needs |
|---|---|---|---|---|
| SOFT_LIGHT (flags) | 262 composites, 262 glyphs | | | the backdrop's color at each pixel: an offscreen group and a blend shader |
| SRC_IN mask of a gradient by many layers (flags) | 262 | | | the union of the backdrop's outlines as one clip, or an offscreen group |
| Group opacity over overlapping layers | at most 29 composites | at most 1 | at most 10 | an offscreen group, faded |
| Clip rectangle that cuts an outline | 1 glyph, 0.9 units (rounding) | 2 glyphs, 10.5 units | 13 glyphs, 5.8 units | a per-emoji clip box (or the pack tool cuts the outline) |
| Radial under a non-similarity transform | 7,693 | 0 | 90,656 | the gradient's own matrix (in the model) |
| Two-point radial | 15 | 0 | 0 | the two-point form in the shader (in the model if built) |

Twemoji and both OpenMoji builds have nothing in this table.

One more difference is not a font feature: the COLR specification blends
layers as W3C Compositing and Blending Level 1 says, and browsers do that
on sRGB-encoded values, while gfx blends in linear light. Edges and
translucent layers will differ from a browser's rendering; gradients
match the specification, which asks for linear light.

## Not checked

- Rendering. Nothing was drawn; the survey counts what the fonts ask
  for. Whether the layer model's output matches a reference renderer
  (FreeType with COLRv1, Skia, a browser) pixel for pixel is not checked.
- The exact overlap of group-opacity layers: box overlap is an upper
  bound. The pack tool can test it exactly with the outline builder.
- HarfBuzz's edge budget: the survey trusts HarfBuzz's walk. The COLRv0
  fonts and OpenMoji's COLRv1 build agree with their layer records, but
  no COLRv1 glyph was walked by a second reader.
- The GPU cost of the layers and of gradient fills on a curve run
  (gradient runs do not exist yet), and the cost of an offscreen group.
- Whether ANGLE exposes `KHR_blend_equation_advanced` (which has a soft
  light equation) on the rig's back ends.
- Variable COLRv1 fonts: no surveyed font is one. Nabla and other
  variable COLRv1 display fonts exist but are not emoji fonts.
- Segoe UI Emoji and Apple Color Emoji (not on this machine), Noto's
  `Noto-COLRv1-noflags.ttf` and `-emojicompat` builds, Noto's 3D release,
  OpenMoji's `colr0_svg` and `colr1_svg` builds, Fluent Emoji High
  Contrast, and Blobmoji (CBDT).
- Palettes other than 0, and the foreground color: no font has them.
