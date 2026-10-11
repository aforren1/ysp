# ysp/outline.h design notes

Status: v0.3.1, 2026-10-10 (v0.2.1, 2026-10-07, for the measurements). v0.1.0 built the font reader, the sweep,
exact alpha, strokes and curve sets; v0.2.0 adds the SVG subset reader and
cheaper strokes; v0.2.1 places TrueType glyphs where HarfBuzz places them. The design was reviewed and approved by the coordinator
on 2026-10-06; "Decisions" records the answers. The manual in the header
says what the calls do; this note says why, and holds the measurements.

## Why an outline builder

Text and artwork reach ysp/gfx.h as curve sets: quadratic Bezier curves
and bands for Lengyel's Slug algorithm (JCGT 6(2), 2017), and, for
value-exact text, exact alpha images drawn on the pixel grid
(docs/text_probe.md, rig_spec 5.2). Something must turn font outlines and
paths into those forms: the pack tool for whole fonts and artwork, and
the player for a glyph that a pack lacks. That is this header. It is
pure computation, so the pack tool, the player and a program without a
pack use the same code.

The probe of 2026-10-06 left four jobs open, and this header does them:

1. Curves in the Slug form from fonts and paths, cubics converted within
   a stated tolerance.
2. Stroke expansion (outlined text, faux bold) without the probe's fold:
   137 wrong pixel centers of 122,000 at 0.08 em, where the curvature
   radius is below the half width.
3. An exact-area rasterizer. Skribidi's is off by 0.24 to 0.32 of full
   scale; the probe's exact one took 0.3 to 6 ms per glyph.
4. Overlap removal, which none of the above had: overlapping contours
   (variable fonts, composites, strokes) break exact-area accumulation,
   and make Slug add two partial crossings near an outer edge.

## Decisions

| Question | Decision | Why |
|---|---|---|
| Font reader | Our own, bounds-checked | stb_truetype stores vertices as `short` and casts CFF coordinates and composite transforms to integers, so fractional CFF points move by up to half a unit; it says "NO SECURITY GUARANTEE"; the player reads fonts from other labs' packs. |
| One core | A sweep over pieces monotone in x and y, with algebraic crossings | It removes overlaps, it feeds the rasterizer, and strokes need it. |
| Overlap removal | `yol_resolve()`, on by default for curve sets (`keep_overlaps` turns it off) | Coordinator, 2026-10-06. It makes exact-area coverage exact for every glyph and lets ysp/gfx.h trust the RESOLVED flag. |
| RESOLVED flag | Glyph flag bit 2 (`YGFX_CSET_RESOLVED`), set on every glyph the builder resolves, never with `keep_overlaps` | Assigned by the gfx worker on 2026-10-06. ysp/gfx.h does not verify it. |
| Exact alpha | Resolve once, then closed-form signed-area accumulation per pixel cell | Measured in phase 1: equal to a direct exact sweep to 3.8e-13 of full scale on 25,000 glyphs, and 3 to 4 times cheaper per size. |
| Stroke fold | Nehab (2020): evolute elements where the curvature radius is below the half width | Measured: 0 wrong pixel centers on 400 random curves, against 186 on 30 curves for half bands alone (phase 1). |
| Cubics | Uniform split with a proven bound | A fitted conversion cut Source Han Sans JP's curves by 16 percent at best, under the 20 percent the coordinator set, and built sets 1.4 to 11 times slower (Cost). Deleted. |
| No-pack layout | cmap and hmtx lookups only; examples/outline/font.c lays out a Latin label by advances | Coordinator, 2026-10-06. Real text needs shaping, kerning and bidi: Skribidi. |
| Variable fonts | The default instance; an instance is refused by name | Coordinator, 2026-10-06. gvar is about 300 lines, when an experiment needs it. |
| New code | `YOL_ERR_NUMERIC` = -12 | -11 is ysp/video.h's NOT_IMPLEMENTED. |
| SVG subset | Built in v0.2.0 (SVG below) | After the font path, the tests, the bars and the stroke knobs, as planned. |
| Round joins and caps | The sector of the disk that no band covers, and band end lines split at the segment's ends | Exact for the round stroke, and 2.8 to 4.6 times fewer positions and crossings in the sweep (Cost). |
| TrueType placement | HarfBuzz's rule (v0.2.1): the outline moves in x by the hmtx left side bearing minus the glyf xMin, or by the USE_MY_METRICS component's difference; CFF never moves | Coordinator, 2026-10-07, after the layout probe (docs/layout_probe.md 4.1). Skribidi positions glyphs with HarfBuzz, so an outline that HarfBuzz moves must move the same distance, or the glyph sits off its pen position. HarfBuzz follows the TrueType rasterizer's phantom points; fontTools does not. Placement against HarfBuzz, below, has the numbers. |
| Sweep order | Sorted in each strip, as in v0.1 | A persistent order (Bentley-Ottmann with binary-search placement) computed more positions on Latin strokes and was 1.2 to 1.3 times slower there, and no faster on CJK (Cost). Deleted. |

## The sweep

Every segment is split at its x and y extrema into pieces monotone in
both, with the extremum copied into the neighbors' control points so each
piece is monotone in floating point too. Piece end heights within 1e-13 of
the path's extent are merged into one line: an arc that ends one ulp off
its line (a rounded rectangle) would otherwise leave a piece 2e-17 tall,
and the first test run failed to close on exactly that.

Between two consecutive event lines (piece ends and crossings), the live
pieces are sorted by x. No two pieces cross inside a strip, so the order
at the strip's top decides, then at its bottom, then at its middle (only
for a tie). A crossing is found once per pair, the first time the pair is
adjacent (the Bentley-Ottmann rule), from the quartic that Q's implicit
form gives along P, in Bernstein form, isolated by Descartes' rule and
refined by the Illinois method to the double's resolution. Two straight
pieces cross where their x difference, linear in y, changes sign. A piece
pair whose x ranges over their common heights do not meet is skipped.

The winding is a prefix sum along each strip. Pieces within 1e-9 of each
other at the strip's top, bottom and middle act as one edge with their
summed direction, so shared and opposite edges cancel (composites,
abutting shapes, stroke elements). Boundary runs become output curves;
the horizontal boundary on each line is the symmetric difference of the
inside intervals just above and just below it. Points within 1e-9 on one
line are one point, and the output is chained into contours by exact
endpoint match. A boundary that does not close is `YOL_ERR_NUMERIC`
with the point named, never a silent open contour.

On each strip's bottom line, two neighbors that are out of order, or
closer than either one's x moves when the line moves by 1e-13 of the
extent, take one x: the x of the piece that moves less, and of the piece
that goes on past the line before one that ends on it (v0.3.1, below).

The sweep's work is bounded (`YOL_SWEEP_WORK`, 2^27 piece positions,
about 2 s): 600 random chords, about 90,000 crossings, are refused.

## Exact alpha

Each monotone piece of the resolved path is walked cell by cell; each
part inside a pixel adds `dy - A` to its cell and `A` to the next, with
`A` the closed-form integral of `(x - i) dy` along the quadratic part:
`x0 d1 / 2 + (x0 d2 + 2 x1 d1 + 2 x1 d2 + x2 d1) / 6 + x2 d2 / 2`. A
row's prefix sum is the coverage. The walk crosses each grid line once.
The prefix sum leaves 1e-16 of residue past the last edge; values within
1e-13 of 0 or 1 are stored as 0 or 1.

## Strokes

Nehab's construction, as the manual describes it. These things made it
fast enough to measure and correct enough to keep:

- Every element is oriented positive by its own area, so the nonzero union
  never cancels a folded region.
- At a smooth join (tangents within 1e-9) both segments offset along one
  shared normal, so the line that ends one band and starts the next is the
  same bits in both and cancels; at a fold the segment's two copies are
  computed once. Exact reverse pairs cancel before the sweep (Nehab writes
  his stroke as this sum).
- The approximation of offsets and evolutes has a call budget and a depth
  limit: the phase-1 prototype hung on Segoe UI glyph 2522; v0.1 builds it
  in 4.5 ms, and a tolerance of 1e-300 returns `YOL_ERR_NUMERIC`.
- v0.2.0: a round join is the sector of its disk between the two outer
  normals, not the whole disk. A point within half the width of a vertex
  whose nearest point on the path is the vertex lies in that sector (the
  vertex's normal cone); every other point within half the width is in a
  segment's band. So bands plus sectors are the round stroke exactly, and
  the sector is 1 to 3 quadratics where the disk was 9 to 12. A round cap
  is the half disk.
- v0.2.0: each band's end line passes through the segment's end, and the
  bands, the sectors' spokes, the miter and bevel wedges and the inner
  triangles compute those points with the same products. The halves that
  meet from two sides are reverses bit for bit and cancel before the
  sweep, which then sees the outer offsets, the arcs and the inner side
  only.

The inner join (the triangle between the two inner offsets at a vertex)
is painted for miter and bevel joins: with segments shorter than the half
width the bands leave it uncovered. The test checks a point in it.

Each segment is also split where its tangent has turned by each 30
degrees. A near cusp (curvature radius 1.7e-9 in one of the test's random
curves) turns almost 180 degrees within a tiny range of t; samples spaced
in t did not see it, and the stroke covered 2 pixel centers it should not
have (21 at 50 px). The curve is a regression case now.

A reference can be wrong too: the first stroke reference sampled the
foot-point function at 64 points and missed two roots 1e-3 apart. The
reference now solves the cubic (B(t) - p) . B'(t) = 0 exactly.

## Verification

`tests/adapt/outline_test.c` needs no font file: it writes a TrueType
font (simple glyphs with repeats, short vectors and instructions, an
all-off-curve contour, overlapping contours, composites with a 2 x 2 and
an x-y scale, a 40,000-point glyph, and glyphs that hit each bound), a
CFF font and a CID-keyed CFF font (every path operator, hints, local and
global subroutines, a 16.16 coordinate, and glyphs that hit each bound),
a collection and a variable-flagged font. Its references are independent
of the header: winding from the roots of each quadratic, and coverage and
area by adaptive Gauss-Kronrod over y of the exact nonzero measure of a
pixel column, with breakpoints at segment ends, extrema, column crossings
and crossings found by subdivision (not the header's algebra).

| Check | Result |
|---|---|
| Format v1 test vector (10 texels, 60 words) | equal, keep_overlaps; the resolved glyph carries RESOLVED, the other does not |
| ygfx_cset_check() on every set | OK |
| Band membership and order, recomputed from the texels | equal |
| Winding against ygfx_cset_winding(), resolved glyphs | equal at every sampled point |
| Cubics, 900 random, three tolerances | worst 0.998 of the tolerance, parametric |
| Ellipse under a rotation and a scale | within 1.05e-5 of the exact ellipse at tol 1e-5 |
| Font reader: points, implied midpoints, composites, every CFF operator, cmap 4 and 12, hmtx, collections | exact |
| Placement: a simple glyph (xMin 100, lsb 70), a composite (xMin 40, lsb 45) and a USE_MY_METRICS composite, through the path, a curve set and exact alpha; CFF glyphs with lsb different from xMin | moved by lsb - xMin, or by the component's difference; CFF not moved |
| Resolve, grids of 2 bars and 2 stems whose starts are rounded to float and other corners not (100; 400 with --long; v0.3.1) | every grid resolved, area within 2.8e-16 of the reference (1.3e-15 for the 400 of --long; gcc 11.4 and clang 11.1, -O2 and -O3); v0.3.0 refused 92 of 100 and had 5 of the other 8 off by up to 9.3e-9 |
| Resolve, random shapes (60; 300 with --long) | area within 2e-13 of the reference (300 shapes); winding 0 or 1 and equal to the input's fill at every sampled point; a second resolve keeps area and curve count |
| Exact alpha against the reference | worst 1.8e-13 (10,698 pixels with --long); the same image without a border; coverage sums to the area; U8, U16, F32 codes exact |
| Strokes, 400 random quadratics, every pixel center at 20 px per unit, against the exact band (the roots of a cubic) | 0 wrong of 1,254,027 |
| The prototype's 3 fold curves and 2 unclosed strokes, and a near cusp, at 50 px | 0 wrong, closed |
| Glyph outlines, round joins, against distance <= 0.04 em | 0 wrong |
| Joins, caps, miter limit, bold, inset, dots, by area | within 1e-8 |
| 30,000 mutated fonts (bit flips, extreme words, truncation, random bytes) | no crash, every result OK or a code; also under ASan and UBSan (gcc 11.4, Linux) |
| SVG: shapes, path grammar (relative, implicit, packed numbers and flags, S and T reflection, arcs), transforms and their order, fill-rule, inheritance, fill then stroke, a stroke under an unequal scale, caps and joins, colors, opacity | areas exact or within the tolerance times the perimeter; reflection and relative commands by exact-alpha images equal within 1e-9 |
| SVG refusals: 33 features, each by name; 13 malformed files; the depth and attribute bounds; a full layer array | the named code and the name in the message |
| 4000 mutated SVG files (20,000 with --long) | no crash, every result layers or a code; also under ASan and UBSan |
| Allocations after warm-up | none (counting allocator); a fixed arena refuses with the size needed |
| Windows fonts, when present: Segoe UI, Segoe UI Light, Bahnschrift, Arial, Nirmala UI, Microsoft YaHei, Source Han Sans JP | every glyph built and set (YaHei and Source Han Sans every 13th), sets valid, coverage sums equal the area within 1e-12 |

Builds: MSVC 19.44 /W4 /WX as C11 and C++17; MinGW-w64 gcc 16.1 -Wall
-Wextra -Wpedantic -Wshadow -Werror as C99, C11 and C++17; gcc 11.4 on
Linux under ASan and UBSan (12 s). The test takes 8 s (Release) and has
`--long` for the full counts, and `--quick` or the environment variable
`YOL_TEST_QUICK` to skip the Windows fonts.
The first reference integrator split rounding noise without end where a
curve is nearly horizontal (1e-16 in y is 1e-12 in x there); a floor of
1e-10 per unit height on its error estimate stopped that.

### Placement against HarfBuzz

`tests/compare/outline_hb/` (its README says how to run it on any font;
a local check, not CI) draws every glyph with HarfBuzz 14.6.0 (uharfbuzz
0.56.3 through uv, `hb_font_draw_glyph` at the font's units per em),
puts each outline through ysp/outline.h's path builder in em units with y
down, and compares it with `yol_font_glyph()`. It also builds two curve
sets: one
with `yol_cset_add_font()`, one with `yol_cset_add()` from HarfBuzz's
outlines. Segments are compared bit for bit as cycles, so a contour may
start at another point. Zero-length segments and contours of one repeated
point are skipped: HarfBuzz passes them on, ysp/outline.h drops them, and
neither has an area (Arial glyph 4621 ends on a repeated point; Times New
Roman glyphs 3022 to 3025 are one point).

| Font | Glyphs | xMin differs from lsb | Equal, v0.2.0 | Equal, v0.2.1 | Curve sets, v0.2.0 | Curve sets, v0.2.1 |
|---|---|---|---|---|---|---|
| Arial 7.06 | 4651 | 13 | 4650; glyph 396 (`napostrophe`) off by 1 unit (4.9e-4 em) | 4651 | differ | equal, 590432 words and 127874 texels |
| Times New Roman | 4731 | 19 | 4730; glyph 327, a simple glyph, off by 190 units (0.0928 em) | 4731 | differ | equal, 745016 words and 170869 texels |

MinGW gcc 16.1 on Windows and gcc 11.4 on Linux under ASan and UBSan gave
the same result. Each font has one glyph that moves. The other mismatched
glyphs are composites with a USE_MY_METRICS component whose own xMin and
lsb agree, so HarfBuzz does not move them. Without that rule, 12 more
Arial glyphs and 18 more Times New Roman glyphs would move where HarfBuzz
does not move them (mutant 67).

### ysp/gfx.h's winding on curves that are not monotone

This test found that `ygfx_cset_winding()` counted two crossings that
did not cancel where a ray passes between a curve's extremum and its
control point (a negative discriminant clamped to 0). The gfx worker's
own mutation run found the same; ysp/gfx.h v0.6.0 has the fix (no
crossing when the discriminant is negative in a two-root row), and the
test compares every glyph, resolved or not, with the fixed function.

### Mutations

`tests/mutate/outline.toml` holds the mutants for the repository's runner
(`uv run tests/mutate/mutate.py outline.toml -j 4`; tests/mutate/README.md).
72 mutants, run on v0.3.1 (2026-10-11): 69 killed, 3 survived as
expected, 72 of 72 as expected, 6588 s with 4 at a time (paused while
other timing ran). On v0.2.1: 67 mutants, 64 killed, 574 s. The 65 of v0.2.0 caught 62 and the 51 of
v0.1 caught 48 the same way.

The three that survive change no result, and the list marks them:

| Mutant | Why no test sees it |
|---|---|
| 06: a crossing on Q's conic but off its arc is kept | An extra event line in a strip changes nothing but the strip count: cost only. |
| 07: the order in a strip by its top x only, with no bottom-x step before the middle | The middle x decides the same ties; the bottom-x step saves computing it: cost only. |
| 39: the stroke approximation's call budget removed | The depth limit (30) ends every runaway first; the budget caps the total work should a path need many pieces at every depth. |

The list's `why` fields say what each fault would break. v0.2.1 added 2:
the placement shift dropped (66) and USE_MY_METRICS ignored (67). v0.2.0
added 14:
the round sector on the wrong side and too coarse, and twelve in the SVG
reader (S and T without reflection, a relative arc end, m's pairs taken
as absolute, the transform order, no default fill, clip-path ignored,
opacity dropped, #rgb times 16, the depth and attribute bounds, entities
passed). The first run left S, T and the relative arc alive: the test
compared areas, and a quadratic segment's area does not depend on its
control point's x. The test now compares exact-alpha images, and kills
them.

Two steps the v0.1 run showed no test could see were deleted: the
extremum copied into a piece's neighbors (the clamp after the end
heights merge already keeps a piece monotone in y) and a clamp of each
run's control point.

## Cost

Measured on the Iris Xe laptop (the probe's machine), on AC power for
the whole run, under the shared measurement lock taken by the shared
guard (no compiler or test process, CPU load at most 20 percent), rows
interleaved, the median of 3 rounds; `examples/outline/bench.c`. Two
numbers are gcc 16.1 -O2 and MSVC 19.44 /O2, in that order, run one after
the other. v0.2.0 tables; v0.1.0's are kept where they changed.

### Curve sets

| Font | Glyphs | Resolved, s | Resolved, us/glyph | keep_overlaps, us/glyph |
|---|---|---|---|---|
| Segoe UI | 5394 | 0.164, 0.136 | 30, 25 | 4.9, 6.3 |
| Bahnschrift | 961 | 0.018, 0.018 | 19, 19 | 3.9, 3.9 |
| Nirmala UI | 5025 | 0.43, 0.38 | 85, 77 | 15, 15 |
| Microsoft YaHei | 30209 | 1.67, 1.86 | 55, 62 | 12, 15 |
| Source Han Sans JP (CFF) | 17934 | 1.68, 1.85 | 94, 103 | 21, 23 |

v0.2.0 did not change the set path; the v0.1.0 run gave 25 to 119 us a
glyph on the same rows, which shows the run-to-run spread. Bars: a whole
YaHei font in 2.5 s (met); 66 us a glyph, the probe's top (met for Segoe
UI, Bahnschrift and YaHei; missed for Nirmala UI by 1.2 to 1.3 times and
Source Han Sans by 1.4 to 1.6 times). The resolve is 70 to 85 percent of
it. The probe's 19 to 66 us did not resolve.

#### A fitted cubic conversion, tried and deleted

Source Han Sans JP has 39.5 lines and 17.7 cubics a glyph; at a cubic
tolerance of 1e-4 em the uniform split gives 88.2 curves a glyph (133.6
at 1e-5), so the floor of one quadratic a cubic is 57.3. The coordinator's
condition: keep a fit only if it cuts the curves by 20 percent at 1e-4 em,
with its error measured against the exact cubic. Measured (every glyph;
set build per glyph under the lock, gcc, 3 interleaved rounds):

| Conversion | Curves a glyph | Cut | Set build, us/glyph |
|---|---|---|---|
| uniform split, n from the proven bound (v0.1, kept) | 88.24 | | 77 to 96 |
| fewest uniform pieces whose measured distance is within tol, midpoint control | 75.07 | 14.9 % | 133 to 141 |
| the same, the tangents' intersection as control (cu2qu's choice) | 75.17 | 14.8 % | not timed |
| greedy longest pieces within tol (bisection on each end), midpoint control | 73.94 | 16.2 % | 777 to 840 |
| the same, tangent control | 74.03 | 16.1 % | not timed |

The distance was measured both ways at 7 points a piece, by Newton on
the exact cubic and on the quadratic. None reaches 20 percent, and each
costs more build time than the curves it saves; the code is deleted.

### Exact alpha

| Row | Time | Bar |
|---|---|---|
| 3000 YaHei glyphs at 32 px | 0.40, 0.37 s | 1 s |
| the same at 24, 32 and 48 px, resolved once | 0.33, 0.34 s | 2 s |
| every Segoe UI glyph at 24 px | 39, 42 us a glyph | 50 us |

All met. The probe's exact rasterizer took 0.3 to 6 ms a glyph.

### Strokes, 0.08 em, round joins

| Font | Whole font | Per glyph | Slowest glyph | Bar |
|---|---|---|---|---|
| Segoe UI, v0.2.0 | 1.56, 1.63 s | 289, 303 us | 26, 38 ms (glyph 5106, 526 segments) | 200 us: missed by 1.4 to 1.5 times |
| Microsoft YaHei, v0.2.0 | 22.0, 24.0 s | 727, 795 us | 8.4, 7.6 ms | 1 ms: met |
| Segoe UI, v0.1.0 | 2.8, 3.7 s | 520, 685 us | 60, 107 ms | |
| Microsoft YaHei, v0.1.0 | 80, 100 s | 2.7, 3.3 ms | 32, 41 ms | |

At pack time a whole Latin font stroked at 0.08 em takes under 2 s and a
whole CJK font under half a minute.

The v0.1 knobs, each tried as the coordinator asked and measured under
the lock (gcc, AC, 3 interleaved rounds, mean per glyph, every 7th Segoe
UI glyph and every 40th YaHei glyph; before the 30-degree split, which
added about 10 percent):

| Variant | Segoe UI | YaHei |
|---|---|---|
| every adjacent pair tested every strip, 64-quadratic disks, no cancelling | 1517 us | 8.7 ms |
| + each pair's crossings once (Bentley-Ottmann) | 946 us | 5.0 ms |
| + disks sized to the tolerance | 666 us | 2.3 ms |
| + exact reverse edges cancelled, shared normals at smooth joins | 474 us | 2.3 ms |

The v0.2.0 knobs, in the coordinator's order, measured the same way
(every variant has the 30-degree split; gcc; 3 interleaved rounds; mean,
then median, per glyph). Work counts per glyph (positions computed,
crossing tests) are exact and come from an instrumented copy, every 23rd
Segoe UI glyph and every 301st YaHei glyph:

| Variant | Segoe UI mean | median | YaHei mean | median | positions, Segoe UI | YaHei |
|---|---|---|---|---|---|---|
| v0.1.0 | 500 to 531 us | 386 to 407 us | 2.33 to 2.36 ms | 2.10 to 2.15 ms | 12,020 | 43,602 |
| v0.1.0 + persistent order | 603 to 625 us | 419 to 425 us | 2.26 to 2.30 ms | 2.07 to 2.10 ms | 16,848 | 21,429 |
| + round joins as sectors | 318 to 328 us | 242 to 247 us | 0.96 to 1.02 ms | 0.88 to 0.92 ms | 7,253 | 14,812 |
| + band end lines through the segment's ends (spokes cancel) | 271 to 279 us | 222 to 231 us | 0.74 to 0.85 ms | 0.69 to 0.81 ms | 4,255 | 9,442 |
| + persistent order | 318 to 350 us | 236 to 259 us | 0.76 to 0.82 ms | 0.73 to 0.78 ms | 6,782 | 5,501 |

The persistent order keeps the live pieces sorted from strip to strip
and places only the pieces that start, cross or tie, by binary search.
On strokes nearly every strip has such a piece, and each placement
probes pieces whose positions no earlier strip computed, so it computes
more positions on Latin glyphs and saves too few on CJK ones to show in
time. It also needed two fixes for Bentley and Ottmann's degenerate
cases (more than two curves through one point; pieces that meet at a
line and part slower than the rounding), which the test found. It is
deleted; the sectors and the cancelling end lines are kept.

### Phase 1 numbers

The phase-1 design's timings were taken on battery, under load, without
the lock. They are void; the tables above replace them.

## SVG subset

```c
typedef struct yol_svg_layer {
    yol_path path;          /* resolved: a fill, or a stroke expanded to one  */
    uint32_t   rgba;          /* the file's sRGB color and opacity, unconverted */
    int32_t    source;        /* YOL_SVG_FILL or YOL_SVG_STROKE             */
    int32_t    element;       /* the element's index in document order          */
} yol_svg_layer;
typedef struct yol_svg_desc {
    double tol;               /* user units; 0: 1e-4 of the viewBox's larger side */
    yol_svg_layer* layers;  int max_layers;
} yol_svg_desc;
/* UTF-8 SVG text to layers, bottom to top: the count, or a code. */
int yol_svg(yol_ctx* c, const char* text, size_t n, const yol_svg_desc* d,
              double viewbox[4], char* err, size_t cap);
```

Accepted: `svg` (viewBox, width, height), `g`, `path` (every command,
arcs by `yol_arc_to()`), `rect` (rx, ry), `circle`, `ellipse`, `line`,
`polyline`, `polygon`; `fill`, `fill-rule`, `fill-opacity`, `stroke`,
`stroke-width`, `stroke-linejoin`, `stroke-linecap`, `stroke-miterlimit`,
`stroke-opacity`, `opacity` on a shape, `transform` (all six forms),
inherited through `g`; colors `#rgb`, `#rrggbb`, `rgb()`, `none` and the
17 CSS 2 names. Refused by name: `filter`, `mask`, `clip-path`, `text`,
`tspan`, `style` and `class` (CSS), gradients and patterns, `url(...)`
paints, `image`, `use`, `symbol`, `marker`, `foreignObject`,
`stroke-dasharray`, group `opacity`, `vector-effect`, DOCTYPE internal
subsets, and any element not listed. A fill and a stroke on one element
are two layers, fill first.

As built in v0.2.0, the manual's SVG section is the reference. Decisions
the build made:

- A stroke is expanded in the shape's own coordinates and then
  transformed, so a skewed or unequally scaled pen is drawn as SVG says;
  the tolerance in the shape's coordinates is the tolerance over the
  transform's largest stretch. Every layer is resolved after the
  transform (a mirror reverses the winding).
- opacity on a shape with fill and stroke is refused, as on a group:
  separate layers with alpha would composite the overlap of fill and
  stroke twice, and SVG fades the group as one.
- Attributes that cannot change the picture (id, xmlns, version, an
  editor's namespaced ones) are passed over; the ones that can and are
  not read are refused (the manual lists them). Unknown elements are
  refused, so an editor's `sodipodi:namedview` is refused: export plain
  SVG.
- No `strtod`: numbers are parsed by hand, as in the CFF reader, so the
  locale cannot change them.

## Open, close and messages (v0.3.0)

v0.3.0 applies the repository conventions (README.md, Conventions) to this
header. Four decisions:

- **The context opens and closes.** `yol_open()` checks its desc (an
  arena and an allocator both given, an arena off 16 bytes), so it is an
  open, and it returns `bool`. `yol_close()` frees the scratch buffers. A
  zeroed context still works as `yol_open(c, NULL)` leaves it, because the
  pack tool and the tests keep contexts in static storage.
- **The path builder's close is `yol_close_path()`.** It closes a contour
  (SVG's Z). With `yol_close()` taken by the context, the old name would
  close the wrong thing.
- **A font is a view.** `yol_font_view()` keeps pointers into the
  caller's bytes and allocates nothing, so it has no close; its message
  goes to the caller's buffer, as for every call without a handle.
- **A curve set opens on a context.** `yol_cset_open()` returns `bool` and
  writes its message into the context it grows on. A set in the caller's
  storage (context NULL) has no context, so it returns false with no
  message; the caps are the only thing that can fail there.

Every call on a context that fails now writes `yol_error()`. v0.2 returned
some codes bare: a NULL argument, and the fixed store of a set full. A
wrapper on each public call compares a counter of written messages before
and after the call and writes "<call>: <code's text>" only when the call
wrote nothing, so a specific message (the glyph, the byte of an SVG) is
never replaced. It costs one compare per call; snprintf runs only on
failure.

Checked on 2026-10-10 (Windows 11, MSVC 19.44 /W4 /WX, and MinGW-w64 gcc
-std=c99 and -std=c++17 -pedantic for USAGE): `tests/adapt/outline_test.c`
passes with every v0.2.1 check unchanged, plus close on zeroed handles,
after a failed open and twice, and the generic and specific messages.
`tests/compile/outline.c` names the lifecycle by type. Mutants 68 (no
generic message) and 69 (a set close that keeps its pointers) are caught.

## v0.3.1: edges a few ulps from horizontal

### What failed

On Ubuntu 22.04's gcc 11.4 (WSL2, the `linux` preset, Release, -O3),
`test_gfx` failed: "outline.h set: resolve: the boundary does not close
at (0.27, -0.64999998271465298)". clang 11.1 on the same machine, MSVC
19.44, MinGW-w64 gcc 16.1 and CI's gcc passed.

Measured, with the test's glyph set alone in a small program
(`resolve` of the 12 test glyphs, as `cs_ol_build()` builds them):

| gcc 11.4 flags | Result |
|---|---|
| -O0, -O1, -O2 | resolved |
| -O3 | the error above |
| -O3 -ffp-contract=off | the error |
| -O3 -fno-strict-aliasing | the error |
| -O3 -fno-tree-loop-vectorize | the error |
| -O3 -fno-tree-slp-vectorize | resolved |
| -O3 -fno-inline | resolved |
| -O0 and -O3, -fsanitize=address,undefined | resolved, no report |
| clang 11.1 -O0, -O3 | resolved |

The difference is in the path, before the resolve. `cs_ol_build()`
passes `(float)` casts of each point to `yol_move()` and `yol_line()`.
At -O3, gcc 11.4 inlines `yol_line()` and, with the SLP vectorizer, loads
the line's end as a vector of two doubles (`movapd`) and uses it without
the `cvtsd2ss`/`cvtss2sd` pair that the cast needs; the start, through
`yol_move()`, is rounded. A rectangle path from 4 corners then held 8 of
11 points that are not floats, and every contour closed with an extra
segment about 1e-8 long. The program did not reduce to a case without
ysp/outline.h, so a fault in the caller is not excluded; the casts are in
test code, and the header cannot keep a compiler from dropping them.

The header's fault: that path is valid, and the resolve refused it on
every compiler. Fed directly (start rounded to float, other corners
not), the test glyph failed the same way under gcc 11.4 -O0 and -O2 and
clang 11.1 -O3.

### Why the boundary did not close

A bar's top edge from (0.05f, -0.65f) to (0.85, -0.65) is 1.7e-8 tall and
0.8 wide: |dx/dy| about 5e7. Where a stem at x = 0.27 crosses it, the
strip line's y is the crossing's, rounded; the edge's x on that line is
then off by |dx/dy| times that rounding, 8.6e-10 here, just past the
eps_x merge (8.5e-10). Two faults follow, and both appeared in the
random grids:

1. The edge's x falls on the wrong side of the stem's. The xs on the line
   are then out of the order the windings came from, and the runs and
   the horizontal pieces on the line miss each other.
2. The edge's x falls short of the stem's on the right side. The two
   keep their order into the next strip, cross inside it, and the
   crossing is never seen (each pair's crossings are found once).

The test grids also showed a third case: a crossing within eps_y of a
line (a stem against an edge 8e-11 tall) is not a strip of its own, so
the pieces reach the line already crossed, by 4.8e-9, with both xs exact.

### The fix

`yol__order_xb()` runs on each strip's bottom line, in the strip's order.
Two neighbors meet when they are out of order, or when their gap is
within the larger of the two x uncertainties, each about the distance
the piece's x moves when the line moves by eps_y: |dx/dy| at the
piece's point on the line times eps_y, 0 for a vertical piece, at most
its width; for a quadratic that ends on the line, where a horizontal
tangent makes |dx/dy| infinite, its position at y - eps_y. Both take
one x, and the next strip's top orders them by
their bottoms, so the crossing swaps them. Which x:

| Tried | Result on the 100 grids |
|---|---|
| Out-of-order pairs only, the x of the smaller \|dx/dy\| on the line (0 at a piece's end) | none refused; 92 with the wrong area, worst 0.19 (fault 2 left) |
| Also pairs in order within \|dx/dy\| eps_y | none refused; 37 with the wrong area, worst 1.2e-9 (the third case: the stem moved to the edge's end) |
| Only pairs within \|dx/dy\| eps_y, in either order; wider inversions kept | 54 refused (the third case: \|dx/dy\| is 0 at the edge's end) |
| Kept: any inversion, and pairs within the x uncertainty (first from positions at y +- eps_y, then as above); the piece that goes on keeps its x, else the smaller uncertainty | none refused, area within 2.8e-16 |

A piece that ends on the line moves only its end: a jog of no area along
the line, which the horizontal boundary there absorbs. Moving the piece
that goes on moves its next run's start, and in the third case left a
sliver 4.8e-9 wide along a whole stem (area 1.2e-9). Earlier pairs that
are now above the new x take it too, so the xs never decrease.

`tests/adapt/outline_test.c` has the grids (T7): the test glyph itself
and 99 random ones, 400 with --long, each resolved and its area checked
against the reference. v0.3.0 fails it on every compiler. Mutants 70
(no reconciling), 71 (pairs in order not met) and 72 (the piece that goes
on moves) are caught.

### Cost

Work counts, stroking every Segoe UI glyph at 0.08 em: the same
crossings (571,913), strips (784,550) and piece positions (11.5 million)
with and without the fix; 312,746 pairs met. The first fix computed both
uncertainties for every adjacent pair in order, 7.9 million pairs a font;
a bound per piece, computed once (`yol__xt()`: a monotone quadratic's
|dx/dy| is largest at an end), leaves 0.53 million.

`examples/outline/bench.c`, MinGW-w64 gcc 16.1 -O2, the Windows 11 laptop
on AC under the guard, 3 rounds a run, runs interleaved, us a glyph:

| Row | v0.3.0 | v0.3.1 | v0.3.1 without the fix |
|---|---|---|---|
| Segoe UI set, resolved | 22.9, 22.9 | 24.1, 24.5 | 35.7, 22.9 |
| Nirmala UI set, resolved | 84.8, 92.5 | 94.5, 103.4 | 90.9, 87.1 |
| Microsoft YaHei set, resolved | 57.6, 60.9 | 62.0, 63.1 | 58.7, 60.3 |
| Source Han Sans JP set, resolved | 89.5, 113.0 | 105.7, 97.1 | 88.3, 103.4 |
| Segoe UI stroke, 0.08 em | 296, 291 | 332, 297 | 278, 291 |
| Microsoft YaHei stroke, 0.08 em | 768, 776 | 845, 812 | 747, 752 |

The set rows move by 10 percent between runs of one build (Segoe UI
without the fix: 35.7, then 22.9). The strokes are 2 to 12 percent
slower; the CJK stroke stays under its 1 ms bar, and the Latin stroke was
over its 200 us bar before. With both uncertainties for every pair, the
strokes were 12 to 21 percent slower (Segoe UI 336, 351; YaHei 928, 962
against 301, 302 and 791, 793 in the same session).

## Next

1. The Latin stroke bar (Cost): the sweep's per-strip positions are what
   is left; a cheaper sort key (x at the strip's top, carried, with the
   bottom computed only for ties) is the next thing to measure.
2. The CFF set bar: the resolve dominates; the same sweep work.
3. gvar instances when an experiment names one.
