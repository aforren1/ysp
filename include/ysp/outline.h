/* ysp/outline.h - v0.3.1 - public domain single-header outline builder:
 *   paths, font outlines, overlap removal, strokes, exact alpha and curve
 *   sets for ysp/gfx.h
 *
 *   The CPU side of text and artwork. It reads glyph outlines from
 *   TrueType and CFF fonts, builds paths of quadratic Bezier curves from C
 *   calls, removes overlaps, expands strokes into fills, computes the exact
 *   box-filter coverage of each pixel (value-exact text), and writes curve
 *   sets: the curves and bands ysp/gfx.h draws by Lengyel's Slug algorithm
 *   (format v1, docs/gfx.md "Curve sets: the format"). The pack tool
 *   calls it; a program without a pack can call it at load to build text
 *   from a font file.
 *
 *   Written in the single-header style of the stb / sokol libraries. Pure
 *   computation: no OS calls, no threads, no I/O. Needs nothing but libm.
 *   Not a transport header, so it does not include ysp/rt.h, and it does
 *   not include ysp/gfx.h. C99 is the floor: it builds as C99, C11 and
 *   C++17, and in the C dialect MSVC compiles by default.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Do this:
 *
 *       #define YSP_OUTLINE_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header to create the
 *   implementation. Every other file just includes the header normally.
 *
 *       yol_ctx cx;                       // one per thread
 *       if (!yol_open(&cx, NULL)) fail(yol_error(&cx));
 *       yol_font font;                    // a view of the bytes, shareable
 *       char err[200];
 *       if (yol_font_view(&font, bytes, n, 0, err, sizeof err) < 0) fail(err);
 *
 *       // a whole font into a curve set: em units, y down, resolved
 *       yol_cset set;
 *       yol_cset_desc cd = { 0 };
 *       cd.n_glyphs = (uint32_t)font.n_glyphs;
 *       if (!yol_cset_open(&set, &cx, &cd)) fail(yol_error(&cx));
 *       if (yol_cset_add_font(&set, &font, NULL, 0, 0) < 0) fail(yol_error(&cx));
 *       // set.texels, set.n_texels, set.words, set.n_words: ygfx_cset_make()
 *
 *       // one glyph outlined at 0.08 em (CSS -webkit-text-stroke: 0.08em)
 *       yol_path g, o;
 *       yol_path_init(&g, &cx); yol_path_init(&o, &cx);
 *       yol_font_glyph(&cx, &font, gid, &g, NULL);
 *       yol_stroke_desc sd = { 0 };
 *       sd.width = 0.08;                  // faux bold: sd.mode = YOL_BOLD;
 *       yol_stroke(&cx, &g, &o, &sd);     // thinning: YOL_INSET
 *
 *       // value-exact alpha at 24 px (canvas fillText, but exact)
 *       yol_box b;
 *       yol_raster_desc rd = { 0 };
 *       rd.scale = 24;
 *       yol_raster(&cx, &g, &rd, &b);     // the box only
 *       rd.x = -b.x0; rd.y = -b.y0;
 *       rd.out = img; rd.w = rd.stride = b.x1 - b.x0; rd.h = b.y1 - b.y0;
 *       yol_raster(&cx, &g, &rd, NULL);
 *
 *       // artwork by hand (ctx.rect(); ctx.ellipse(); ctx.fill('evenodd'))
 *       yol_path a;
 *       yol_path_init(&a, &cx);
 *       a.rule = YOL_EVENODD;
 *       yol_rect(&a, 0, 0, 100, 60, 8, 8);
 *       yol_ellipse(&a, 50, 30, 20, 20);
 *       if (yol_path_end(&a) < 0) fail(yol_error(&cx));
 *
 *       yol_path_free(&a); yol_path_free(&o); yol_path_free(&g);
 *       yol_cset_close(&set);
 *       yol_close(&cx);
 *
 *   A zero desc field means its default. In C99 and C++20 a compound
 *   literal with designated initializers does the same:
 *   yol_stroke(&cx, &g, &o, &(yol_stroke_desc){ .width = 0.08 }).
 *
 *   ERRORS
 *     yol_open() and yol_cset_open() return false on failure; every other
 *     call that can fail returns a negative YOL_ERR_* code. A call on a
 *     context (or on a path or a curve set made on one) writes
 *     yol_error() before it returns a code. A path or a set in the
 *     caller's storage (context NULL) has nowhere to write: it returns the
 *     code only. yol_font_view() and yol_svg() also write the caller's
 *     buffer (err, cap; may be NULL). A context is for one thread, so no
 *     call races on its message.
 *
 *   ---------------------------------------------------------------------
 *   MODEL
 *   ---------------------------------------------------------------------
 *   PATHS
 *     A path is contours of quadratic Bezier segments in double. A line is
 *     a segment whose control point equals its start, bit for bit (the
 *     curve-set convention). The builder (yol_move, _line, _quad, _cubic,
 *     _arc_to, _close_path, _rect, _ellipse) puts every point through the path's
 *     transform. A contour without yol_close_path() stays open: fills close it
 *     with a line (SVG), strokes give it caps. The first builder error
 *     sticks in p->status; yol_path_end() returns it.
 *   COORDINATES
 *     Glyphs come in em units times desc.size, y down (ysp/gfx.h's
 *     convention; the reader flips the font's y), from the pen at desc.x,
 *     desc.y on the baseline: an ascender has negative y. Artwork is in its
 *     own units. Clockwise on the screen is the positive direction:
 *     yol_path_area() is positive for it, and ygfx_cset_winding()
 *     counts +1 inside it. TrueType outer contours come out counterclockwise
 *     on the screen (winding -1); the nonzero rule does not care, so the
 *     reader does not reverse them.
 *   FILL RULES
 *     path.rule: YOL_NONZERO (fonts) or YOL_EVENODD (SVG
 *     fill-rule="evenodd").
 *   RESOLVED
 *     yol_resolve() returns the region a path fills as closed contours,
 *     clockwise on the screen, whose winding is 0 or 1 everywhere: no
 *     overlaps, no self-crossings, every vertex shared bit for bit, and
 *     flags YOL_PATH_RESOLVED. yol_stroke() and yol_raster() resolve;
 *     a curve set resolves every glyph unless desc.keep_overlaps, and then
 *     sets the glyph's RESOLVED flag (bit 2, YGFX_CSET_RESOLVED), which
 *     ysp/gfx.h's exact-area paths rely on. A glyph written with
 *     keep_overlaps never has the flag.
 *
 *   ---------------------------------------------------------------------
 *   FONTS
 *   ---------------------------------------------------------------------
 *   yol_font_view() reads a TrueType or OpenType font or one face of a
 *   collection (.ttc): head, maxp, hhea, hmtx, cmap (format 4 and 12), and
 *   glyf with loca, or CFF (Type 2 charstrings, local and global
 *   subroutines, CID-keyed fonts with FDArray and FDSelect format 0 or 3).
 *   yol_font_glyph() adds one glyph's outline to a path. Glyph indices
 *   come from layout (Skribidi in the pack tool and the player); this
 *   header does no shaping, kerning or bidi. yol_font_glyph_index() and
 *   yol_font_hmetrics() are cmap and hmtx lookups for simple labels.
 *   Font bytes can come from another lab's pack, so the reader treats them
 *   as hostile. Every offset and length is checked before its read, and
 *   every loop and recursion has a bound:
 *       composite depth          8
 *       composite components     4096 visits per glyph
 *       CFF operand stack        48
 *       CFF subroutine depth     10
 *       CFF charstring tokens    65536 per glyph
 *       CFF stems                96
 *       outline points           YOL_GLYPH_POINTS (2^17) per glyph
 *   A broken font gives YOL_ERR_FORMAT with the glyph named. Refused by
 *   name (YOL_ERR_REFUSED): CFF2 outlines, a font with neither glyf nor
 *   CFF, a point-matched composite component, a seac accent, Type 2
 *   arithmetic operators, a CFF FontMatrix other than 1 / unitsPerEm, and
 *   a variation instance (desc.axes): v0.1 builds the default instance of
 *   a variable font (flag YOL_FONT_VARIABLE). TrueType hinting is not
 *   run: value-exact text is exact area, not hinted.
 *   Placement: a TrueType glyph sits where HarfBuzz puts it (HarfBuzz
 *   14.6.0, src/OT/glyf/Glyph.hh, Glyph::get_points). If its glyf xMin
 *   differs from its hmtx left side bearing (lsb), the outline moves in x
 *   by lsb - xMin, as a TrueType rasterizer places its phantom points. A
 *   composite uses the shift of its last component with the
 *   USE_MY_METRICS flag, if it has one. Simple, composite and empty glyphs
 *   get the shift; CFF glyphs do not, because CFF has no phantom points.
 *   The layout engine gives glyph positions from HarfBuzz, so the outline
 *   must agree with HarfBuzz; fontTools does not shift. Arial and Times New
 *   Roman each have one glyph that moves (1 and 190 font units).
 *
 *   ---------------------------------------------------------------------
 *   CUBICS
 *   ---------------------------------------------------------------------
 *   A cubic (CFF, yol_cubic, SVG) becomes n quadratics on a uniform split
 *   of its parameter, each with the same ends and the control point
 *   (3 (C1 + C2) - (A + B)) / 4 of its piece. The distance between the
 *   cubic and its quadratics is at most sqrt(3) / 36 |P3 - 3 P2 + 3 P1 - P0|
 *   / n^3 at every parameter, so n = ceil(cbrt(sqrt(3) / 36 |...| / tol))
 *   keeps them within tol: path.tol after the transform (0: 1e-4), or for a
 *   glyph desc.tol in em (0: 1e-4 em). Arcs and ellipses use quadratics
 *   whose largest radial error, r (1 - cos(a/2))^2 / (2 cos(a/2)) for an
 *   arc piece of angle a, is within tol after the transform.
 *
 *   ---------------------------------------------------------------------
 *   STROKES
 *   ---------------------------------------------------------------------
 *   yol_stroke() follows Nehab (2020, "Converting stroked primitives to
 *   filled primitives", ACM TOG 39(4)). Each segment is split where its
 *   curvature radius equals the half width h and at its vertex. Where the
 *   radius is at least h, the segment adds a band between its two offsets.
 *   Where it is below h, the inner offset folds back; the segment then adds
 *   its outer half band, the region between itself and its evolute (the
 *   centers of curvature), and the region between the evolute and the
 *   inner offset. Joins and caps complete it: round is the sector of the
 *   disk between the two normals on the outer side (a point within half
 *   the width of a vertex whose nearest point is the vertex lies there; any
 *   other point within it is in a band), miter (with miter_limit) and bevel
 *   are the outer wedge plus the inner triangle the bands leave; caps are
 *   butt, round (a half disk) or square; a zero-length subpath is a dot. Every
 *   piece is oriented positive, so their union under the nonzero rule is
 *   the stroke with no folded hole; edges that are exact reverses cancel,
 *   and yol_resolve()'s sweep removes the rest of the overlaps. Offsets
 *   and evolutes are quadratics within tol (5 samples of each must lie
 *   within tol); a piece that does not converge within the call's budget
 *   gives YOL_ERR_NUMERIC with the point named, never a hang. mode BOLD
 *   is the fill and the stroke, INSET the fill minus the stroke.
 *
 *   ---------------------------------------------------------------------
 *   SVG
 *   ---------------------------------------------------------------------
 *   yol_svg() reads artwork (rig_spec 5.2, Artwork) into solid-color
 *   layers, each a resolved path in viewBox units (y down, as SVG has it)
 *   for yol_cset_add(), bottom to top: a shape's fill, then its stroke.
 *   Read: <svg> (viewBox, or width and height in px), <g>, <path> (every
 *   command, arcs included), <rect> (rx, ry), <circle>, <ellipse>, <line>,
 *   <polyline>, <polygon>, an empty <defs>, and <title>, <desc> and
 *   <metadata> (skipped); the attributes transform (matrix, translate,
 *   scale, rotate, skewX, skewY), fill, fill-rule, fill-opacity, stroke,
 *   stroke-width, stroke-linejoin, stroke-linecap, stroke-miterlimit,
 *   stroke-opacity, and opacity on a shape with one paint, inherited
 *   through <g> as SVG does; colors #rgb, #rrggbb, rgb() and the 17 CSS 2
 *   names, and none. A stroke is expanded in the shape's own coordinates,
 *   then transformed, so a skewed pen is drawn as the file says. The
 *   tolerance is desc.tol, or 1e-4 of the viewBox's larger side.
 *   Refused by name (YOL_ERR_REFUSED, the message names it): every other
 *   element (filter, mask, clipPath, text, style, gradients, pattern,
 *   image, use, symbol, marker, foreignObject, a nested svg, anything
 *   unknown), the attributes style, class, clip-path, mask, filter,
 *   stroke-dasharray, vector-effect, marker*, display, visibility,
 *   paint-order, transform-origin, requiredFeatures, systemLanguage, a
 *   url() paint, currentColor and other color syntax, units other than
 *   px, opacity on a group or on a shape with fill and stroke (it needs a
 *   compositing group), a transform on <svg>, entity references, a
 *   DOCTYPE internal subset and CDATA. Other attributes (id, xmlns,
 *   version, an editor's namespaced ones) change nothing and are passed
 *   over. Malformed XML or path data is YOL_ERR_FORMAT with the byte.
 *   Bounds: element depth 64, 64 attributes an element.
 *
 *   ---------------------------------------------------------------------
 *   EXACTNESS
 *   ---------------------------------------------------------------------
 *   yol_raster() resolves the path, then sums for every pixel the exact
 *   signed area under each curve inside it: each monotone piece is split
 *   where it crosses a pixel row or column, and each part adds a closed
 *   form integral of x dy. That is the box-filter coverage of the pixel
 *   square in double, linear in area, with no gamma (ysp/gfx.h's linear
 *   output stage). Values within 1e-13 of 0 or 1 are stored as 0 or 1.
 *   U8 stores floor(255 c + 0.5), U16 floor(65535 c + 0.5). Draw the image
 *   on the pixel grid only: off the grid it is not exact.
 *   The resolve's sweep is exact up to rounding: output points are on the
 *   input curves, crossings are roots of a quartic refined to the double's
 *   resolution, end heights within 1e-13 of the path's extent are merged,
 *   and points within 1e-9 of it on one line are one point.
 *   The sweep's work is bounded by YOL_SWEEP_WORK piece positions (2^27,
 *   about 2 s): a more complex outline is refused (YOL_ERR_REFUSED).
 *
 *   ---------------------------------------------------------------------
 *   CURVE SETS
 *   ---------------------------------------------------------------------
 *   yol_cset_open() writes the header and an empty glyph table of
 *   desc.n_glyphs entries; yol_cset_add() appends one glyph: each vertex
 *   rounded to f32 once (so curves share their ends bit for bit and a
 *   contour closes on its first texel), lines with p1 = p0, the bbox from
 *   the stored f32 values, nh = nv = clamp(round(sqrt(curves)), 1, 16)
 *   unless desc.nh, desc.nv say otherwise, each band's list sorted as the
 *   format requires with ties by ascending texel (the format's test
 *   vector), backward lists with desc.backward, then the table entry last,
 *   so ygfx_cset_add() never sees half a glyph. The arrays grow on the
 *   context's allocator, or stay in the caller's storage (ctx NULL at
 *   init; growth past the caps is YOL_ERR_FULL).
 *
 *   ---------------------------------------------------------------------
 *   MEMORY
 *   ---------------------------------------------------------------------
 *   The context holds the allocator (desc.realloc, or the C library's, or
 *   a fixed arena in desc.mem: no heap at all) and scratch buffers that
 *   grow to the largest glyph seen and are reused. After the first glyphs
 *   of a font, building, resolving and rasterizing glyphs make no
 *   allocator call (the test counts them). A context is for one thread;
 *   fonts are read-only and threads may share one.
 *
 *   ---------------------------------------------------------------------
 *   COST
 *   ---------------------------------------------------------------------
 *   Measured on the Iris Xe laptop on AC power, under the shared
 *   measurement lock, medians of 3 interleaved rounds, gcc 16.1 -O2 to
 *   MSVC 19.44 /O2 (examples/outline/bench.c measures your machine):
 *       a whole font into a set, resolved   Segoe UI 25 to 30 us a glyph;
 *                                           Microsoft YaHei (30209 glyphs)
 *                                           1.7 to 1.9 s; Source Han Sans
 *                                           JP (CFF) 94 to 103 us a glyph
 *       the same with keep_overlaps         4 to 23 us a glyph
 *       exact alpha                         Latin at 24 px 39 to 42 us a
 *                                           glyph; 3000 CJK glyphs at 32 px
 *                                           0.37 to 0.40 s
 *       a stroke, 0.08 em, round joins      Latin 289 to 303 us a glyph
 *                                           (Segoe UI: 1.6 s); CJK 727 to
 *                                           795 us (YaHei: 22 to 24 s)
 *   The sweep's cost grows with the curves and their crossings; a stroke
 *   has 5 to 10 times its glyph's curves before the resolve.
 *
 *   ---------------------------------------------------------------------
 *   LEFT OUT
 *   ---------------------------------------------------------------------
 *   Layout, shaping, bidi and kerning (Skribidi). Hinting. Font variations
 *   (gvar, CFF2), color fonts (COLR, SVG, sbix, CBDT) and bitmap fonts.
 *   Dashes. Atlas packing (the pack tool places the alpha images). TrueType
 *   hinting. In SVG: everything SVG lists as refused.
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   Link libm on Linux. Define YOL_API to override the default `extern`
 *   linkage; -DYOL_API=static needs a translation unit that calls every
 *   function, or -Wno-unused-function. YOL_SWEEP_WORK and
 *   YOL_GLYPH_POINTS may be defined before the implementation to change
 *   those bounds.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.3.1 - Fixed: yol_resolve() failed "the boundary does not close" on
 *          strokes that cross an edge a few float ulps from horizontal (a
 *          rectangle whose start is rounded to float and whose other
 *          corners are not). Such an edge's x on a strip line is off by
 *          its |dx/dy| times the line's rounding, past the 1e-9 merge.
 *          Neighbors on a strip's bottom line that are out of order, or
 *          closer than either one's x moves in eps_y of y, now meet at the
 *          x of the piece that moves less. Found by gfx_test under gcc
 *          11.4 -O3 (Ubuntu 22.04), which drops the caller's (float) casts
 *          before the inlined yol_line() (docs/outline.md, "v0.3.1").
 *   v0.3.0 -The ysp conventions (README.md, Conventions); breaks the API.
 *          The context opens and closes: yol_init() is bool yol_open(),
 *          yol_free() is yol_close(). The path builder's yol_close() (SVG's
 *          Z) is yol_close_path(). yol_font_open() is yol_font_view(): it
 *          keeps pointers into the caller's bytes and has no close.
 *          yol_cset_init() is bool yol_cset_open(), yol_cset_free() is
 *          yol_cset_close(); the message is in yol_error() of the set's
 *          context. Every call on a context that fails leaves a message,
 *          also where v0.2 returned a bare code (a NULL argument, a full
 *          fixed store). USAGE builds as C99 and C++17. No output
 *          changed.
 *   v0.2.1 - TrueType glyphs sit where HarfBuzz puts them: an outline whose
 *          glyf xMin differs from its hmtx left side bearing moves by the
 *          difference (FONTS, "Placement"). Output changes for such glyphs
 *          only: one in Arial, one in Times New Roman.
 *   v0.2.0 - the SVG subset reader (yol_svg, SVG). Strokes: a round join or
 *          cap is the sector of its disk that no band covers, not the whole
 *          disk, and the bands' end lines pass through the segment's ends so
 *          they cancel against the joins' spokes: 2.8 to 4.6 times fewer
 *          positions and crossings in the sweep (docs/outline.md). Tried
 *          and deleted, by measurement: a persistent strip order in the
 *          sweep, and a fitted cubic conversion.
 *   v0.1.0 - first version. Paths, transforms, cubics and arcs; a bounded
 *          TrueType (glyf) and CFF (Type 2, CID-keyed included) reader;
 *          overlap removal; strokes with Nehab's (2020) evolute elements;
 *          exact alpha; curve sets with the RESOLVED flag (docs/outline.md).
 *
 *   STATUS: v0.3.0: the v0.2.1 checks below pass unchanged, and the test
 *   checks open, close and the messages of v0.3. v0.2.1: built and run with MSVC 19.44 (/W4 /WX, C11 and C++17),
 *   MinGW-w64 gcc 16.1 (C99, C11, C++17) and gcc 11.4 on Linux under ASan
 *   and UBSan; the desc literals as C++20. tests/adapt/outline_test.c
 *   writes its own TrueType, CFF and CID-keyed CFF fonts and SVG files and
 *   checks, against references that do not share the header's code:
 *   - curve sets: the format's test vector word for word, ygfx_cset_check()
 *     on every set, band membership and order recomputed from the texels,
 *     the winding equal to ygfx_cset_winding() at every sampled point;
 *   - the reader: every point, implied midpoints, composites, every CFF
 *     operator, cmap 4 and 12, hmtx, collections, every bound and refusal,
 *     the placement shift of a simple glyph, a composite and a
 *     USE_MY_METRICS composite, and no shift for CFF;
 *     30000 mutated fonts without a crash;
 *   - cubics within 0.998 of the tolerance (900 random), ellipses within
 *     1.05e-5 at tol 1e-5;
 *   - resolve: 300 random shapes, area within 2e-13 of an adaptive
 *     Gauss-Kronrod reference, winding 0 or 1 and equal to the input's fill;
 *   - exact alpha within 1.8e-13 of that reference (10698 pixels);
 *   - strokes: 400 random quadratics, 0 wrong of 1254027 pixel centers
 *     against the exact band (the probe's half bands: 186 wrong on 30 of
 *     400); joins, caps, bold and inset by area within 1e-8;
 *   - SVG: shapes, path grammar, transforms, styles and colors against
 *     built paths and exact-alpha images; 33 refusals by name; 20000
 *     mutated files without a crash;
 *   - no allocator call after warm-up.
 *   On the Windows fonts (Segoe UI, Bahnschrift, Arial, Nirmala UI,
 *   Microsoft YaHei, Source Han Sans JP) every glyph tried builds a valid
 *   set. Against HarfBuzz 14.6.0 (uharfbuzz 0.56.3), every glyph of Arial
 *   (4651) and Times New Roman (4731) gives the same segments bit for bit,
 *   and the curve sets from yol_cset_add_font() and from HarfBuzz's
 *   outlines are the same word for word. tests/mutate/outline.toml: 67
 *   deliberate faults, 64 caught, the 3
 *   that survive change no result (docs/outline.md). Missed bars: a
 *   Latin stroke (289 to 303 us a glyph against 200) and the sets of the
 *   two heaviest fonts (Nirmala UI 77 to 85, Source Han Sans 94 to 103 us
 *   a glyph against 66). COST has the rest.
 */
#ifndef YSP_OUTLINE_H_INCLUDED
#define YSP_OUTLINE_H_INCLUDED

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef YOL_API
#define YOL_API extern
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define YOL_VERSION_MAJOR  0
#define YOL_VERSION_MINOR  3
#define YOL_VERSION_PATCH  1
#define YOL_VERSION_STRING "0.3.1"

/* YOL_VERSION_STRING, as compiled into the implementation. */
YOL_API const char* yol_version(void);

/* Return codes. The values equal ysp/gfx.h's and ysp/color.h's for the same
 * meaning. */
#define YOL_OK            0
#define YOL_ERR_ARG     (-1)   /* NULL, not finite, a bad enum               */
#define YOL_ERR_FULL    (-4)   /* a fixed buffer or arena is full            */
#define YOL_ERR_REFUSED (-7)   /* a feature this version does not do, named  */
#define YOL_ERR_FORMAT  (-8)   /* malformed font or SVG bytes, named         */
#define YOL_ERR_RANGE   (-9)   /* a value out of range                       */
#define YOL_ERR_NUMERIC (-12)  /* a computation did not converge or did not
                                  * close; the message names the point. Never
                                  * silent (EXACTNESS)                         */

YOL_API const char* yol_strerror(int code);

/* --- context ------------------------------------------------------------- */

/* realloc(user, p, n): grow or shrink p to n bytes; n 0 frees p and returns
 * NULL. Must return memory aligned for double. */
typedef void* (*yol_realloc_fn)(void* user, void* p, size_t n);

typedef struct yol_ctx_desc {
    yol_realloc_fn realloc;  /* NULL: the C library's realloc and free      */
    void*            user;
    void*            mem;      /* or a fixed arena: no heap at all; realloc
                                * must be NULL. 16-byte aligned              */
    size_t           mem_size;
} yol_ctx_desc;

#define YOL__NSCRATCH 28

/* One per thread: the allocator, the last error message and the scratch
 * buffers every call reuses. yol_open() fills it; a zeroed context works
 * as yol_open(c, NULL) leaves it. */
typedef struct yol_ctx {
    yol_realloc_fn realloc_fn;
    void*            user;
    uint8_t*         arena;
    size_t           arena_size, arena_used, arena_last;
    uint64_t         n_alloc;              /* allocator calls so far (tests)  */
    void*            scratch[YOL__NSCRATCH];
    size_t           scratch_cap[YOL__NSCRATCH];
    char             err[256];
    int32_t          inited;
    uint32_t         n_err;                /* messages written (a call checks it
                                            * to keep a more specific one)    */
} yol_ctx;

/* d NULL: the C library's allocator. False, with the reason in
 * yol_error(c), for realloc and an arena both given or an arena that is
 * not 16-byte aligned. */
YOL_API bool yol_open(yol_ctx* c, const yol_ctx_desc* d);
/* Frees the scratch buffers. Safe on a zeroed context, after a failed open
 * and twice. Paths and sets made on c must be freed first. */
YOL_API void yol_close(yol_ctx* c);
/* The message of the last call that failed on this context ("" if none). */
YOL_API const char* yol_error(const yol_ctx* c);

/* --- paths ----------------------------------------------------------------- */

#define YOL_NONZERO 0
#define YOL_EVENODD 1

#define YOL_PATH_RESOLVED 0x1u    /* winding 0 or 1, +1 clockwise on screen */
#define YOL_OPEN 0x80000000u      /* contours[k]: the contour is open       */

/* A 2 x 3 affine transform: x' = a x + c y + e, y' = b x + d y + f (SVG's
 * matrix(a, b, c, d, e, f)). */
typedef struct yol_xform { double a, b, c, d, e, f; } yol_xform;

/* Contours of quadratic Bezier segments in double. Contour k's points are
 * pts[2 i], pts[2 i + 1] for i from first(k) = contours[k] & ~YOL_OPEN to
 * first(k + 1) - 1: its start, then (control, end) per segment. A line has
 * its control point equal to its start, bit for bit (the curve-set
 * convention). A contour without YOL_OPEN is closed: its last point is its
 * first, bit for bit. Fills treat an open contour as closed by a line. */
typedef struct yol_path {
    double*     pts;          /* x, y pairs                                      */
    uint32_t*   contours;     /* first point index | YOL_OPEN                  */
    int32_t     n_pts, cap_pts, n_contours, cap_contours;
    int32_t     rule;         /* YOL_NONZERO or YOL_EVENODD                  */
    uint32_t    flags;        /* YOL_PATH_RESOLVED                             */
    yol_ctx*  ctx;          /* the allocator; NULL: fixed storage (caps given) */
    /* builder state */
    yol_xform xf;           /* applied to every point the builder adds          */
    double      tol;          /* cubic and arc tolerance, path units; 0: 1e-4     */
    double      ux, uy;       /* current point, before the transform              */
    double      usx, usy;     /* the open contour's start, before the transform   */
    int32_t     status;       /* the first builder error, sticky                  */
    int32_t     in_contour;
} yol_path;

/* An empty path on c's allocator (c NULL: give pts, contours and the caps
 * yourself, and growth past them is YOL_ERR_FULL). */
YOL_API void yol_path_init(yol_path* p, yol_ctx* c);
YOL_API void yol_path_free(yol_path* p);
/* Empties the path; keeps its storage, transform and tolerance. */
YOL_API void yol_path_clear(yol_path* p);
/* Copies src's contours, rule and flags into dst. */
YOL_API int  yol_path_copy(yol_path* dst, const yol_path* src);

YOL_API yol_xform yol_xf_identity(void);
YOL_API yol_xform yol_xf_mul(yol_xform l, yol_xform r);      /* l after r  */
YOL_API yol_xform yol_xf_translate(double x, double y);
YOL_API yol_xform yol_xf_scale(double sx, double sy);
YOL_API yol_xform yol_xf_rotate(double deg);    /* clockwise on screen (y down) */
YOL_API yol_xform yol_xf_skew(double xdeg, double ydeg);

/* The builder. Points go through the path's transform. An error (not
 * finite, full) sticks in p->status and stops the builder until
 * yol_path_end() returns it. */
YOL_API void yol_move (yol_path* p, double x, double y);
YOL_API void yol_line (yol_path* p, double x, double y);
YOL_API void yol_quad (yol_path* p, double cx, double cy, double x, double y);
/* Converted to quadratics within p->tol after the transform (CUBICS). */
YOL_API void yol_cubic(yol_path* p, double c1x, double c1y, double c2x, double c2y,
                           double x, double y);
/* Closes the contour (SVG's Z). */
YOL_API void yol_close_path(yol_path* p);
/* SVG's elliptical arc command 'A' from the current point; quadratics
 * within p->tol. */
YOL_API void yol_arc_to(yol_path* p, double rx, double ry, double rot_deg,
                            int large, int sweep, double x, double y);
/* Closed shapes, clockwise on screen, each its own contour. rx, ry round
 * the rectangle's corners (0: square corners). */
YOL_API void yol_rect   (yol_path* p, double x, double y, double w, double h,
                             double rx, double ry);
YOL_API void yol_ellipse(yol_path* p, double cx, double cy, double rx, double ry);
YOL_API void yol_set_xform(yol_path* p, yol_xform t);
YOL_API void yol_set_tol(yol_path* p, double tol);
/* Ends the open contour (left open) and returns the sticky status, then
 * clears it. The builder wrote the message into yol_error(p->ctx), if the
 * path has a context, when the error happened. */
YOL_API int  yol_path_end(yol_path* p);
/* Green's theorem: the signed area, open contours closed by a line;
 * positive for clockwise on screen. */
YOL_API double yol_path_area(const yol_path* p);

/* --- fonts ----------------------------------------------------------------- */

#define YOL_FONT_GLYF     1u      /* TrueType outlines                       */
#define YOL_FONT_CFF      2u      /* CFF (Type 2 charstrings) outlines        */
#define YOL_FONT_VARIABLE 0x100u  /* has fvar: only the default instance     */
#define YOL_FONT_CID      0x200u  /* CID-keyed CFF                           */

/* A view of the caller's font bytes; read-only after open, so threads may
 * share one. The bytes must outlive it. */
typedef struct yol_font {
    const uint8_t* data;
    size_t         size;
    int32_t        units_per_em, n_glyphs;
    int32_t        ascender, descender, line_gap;   /* hhea, font units          */
    uint32_t       flags;                           /* YOL_FONT_*              */
    /* private: table offsets from data */
    uint32_t loca_, loca_len_, glyf_, glyf_len_, hmtx_, hmtx_len_, n_hmetrics_;
    uint32_t cmap_, cmap_fmt_, loca_long_, hmtx_nb_;
    uint32_t cff_, cff_len_, cs_, gsubr_, lsubr_, fdarray_, fdselect_;
} yol_font;

/* A view: f keeps pointers into data, so the bytes must outlive it, and
 * there is nothing to close. index: the face in a collection (.ttc), else
 * 0. Refuses CFF2 outlines and a font without outlines, by name, in err
 * (may be NULL). */
YOL_API int yol_font_view(yol_font* f, const void* data, size_t size, int index,
                              char* err, size_t cap);
/* 1 for a single font, the face count for a collection, or a code. */
YOL_API int yol_font_faces(const void* data, size_t size);
/* cmap format 4 or 12; 0 (.notdef) when the font does not map it. */
YOL_API uint32_t yol_font_glyph_index(const yol_font* f, uint32_t codepoint);
/* hmtx: the advance width and left side bearing in em. */
YOL_API int yol_font_hmetrics(const yol_font* f, uint32_t glyph, double* advance_em,
                                  double* lsb_em);

typedef struct yol_glyph_desc {
    double        size;       /* path units per em; 0: 1 (em units)             */
    double        x, y;       /* the pen position, path units                    */
    bool          y_up;       /* false (the default): y down, ysp_gfx's way      */
    double        tol;        /* CFF cubic tolerance in em; 0: 1e-4              */
    const double* axes;       /* a variation instance: refused in v0.1           */
    int           n_axes;
} yol_glyph_desc;

/* Appends glyph `glyph` to p as closed contours (nonzero, as fonts are), in
 * em times size from the pen, through p's transform. An empty glyph adds
 * nothing. Refuses by name: a point-matched component, a composite deeper
 * than 8, a seac accent, Type 2 arithmetic operators, an instance. */
YOL_API int yol_font_glyph(yol_ctx* c, const yol_font* f, uint32_t glyph,
                               yol_path* p, const yol_glyph_desc* d);

/* --- overlap removal ------------------------------------------------------- */

/* out gets the region `in` fills under in->rule as closed contours whose
 * winding is 0 or 1 everywhere, +1 clockwise on screen, every vertex shared
 * bit for bit, flags YOL_PATH_RESOLVED. Open contours are filled closed.
 * in may equal out. YOL_ERR_NUMERIC if the output would not close. */
YOL_API int yol_resolve(yol_ctx* c, const yol_path* in, yol_path* out);

/* --- strokes --------------------------------------------------------------- */

typedef enum yol_join { YOL_JOIN_ROUND = 0, YOL_JOIN_MITER, YOL_JOIN_BEVEL } yol_join;
typedef enum yol_cap  { YOL_CAP_BUTT = 0, YOL_CAP_ROUND, YOL_CAP_SQUARE } yol_cap;
typedef enum yol_stroke_mode {
    YOL_STROKE = 0,   /* the stroke alone: outlined text, an SVG stroke      */
    YOL_BOLD,         /* the fill and the stroke: faux bold                   */
    YOL_INSET         /* the fill minus the stroke                            */
} yol_stroke_mode;

typedef struct yol_stroke_desc {
    double width;        /* path units, > 0                                     */
    int    join;         /* yol_join; 0: ROUND                                */
    double miter_limit;  /* 0: 4 (SVG's); past it a miter becomes a bevel       */
    int    cap;          /* yol_cap, for open contours; 0: BUTT               */
    int    mode;         /* yol_stroke_mode                                   */
    double tol;          /* path units; 0: in->tol, or 1e-4                      */
} yol_stroke_desc;

/* out (not in) gets the stroked region, resolved (winding 0 or 1). */
YOL_API int yol_stroke(yol_ctx* c, const yol_path* in, yol_path* out,
                           const yol_stroke_desc* d);

/* --- exact alpha ----------------------------------------------------------- */

typedef enum yol_alpha {
    YOL_ALPHA_U8 = 0,   /* floor(255 c + 0.5)                                */
    YOL_ALPHA_U16,      /* floor(65535 c + 0.5)                              */
    YOL_ALPHA_F32,
    YOL_ALPHA_F64
} yol_alpha;

typedef struct yol_raster_desc {
    double scale;         /* pixels per path unit (px per em for a glyph), > 0  */
    double x, y;          /* where path point (0, 0) lands, px from the image's
                           * top-left corner                                    */
    int    format;        /* yol_alpha                                        */
    void*  out;           /* w x h values, rows stride bytes apart; NULL: only
                           * the box                                            */
    int    w, h, stride;
} yol_raster_desc;

/* Pixels [x0, x1) x [y0, y1) in image coordinates. */
typedef struct yol_box { int32_t x0, y0, x1, y1; } yol_box;

/* The exact box-filter coverage of each pixel of out, computed in double
 * (EXACTNESS), then stored as `format`. Resolves first unless in has
 * YOL_PATH_RESOLVED. box (may be NULL) gets the pixels the path can touch. */
YOL_API int yol_raster(yol_ctx* c, const yol_path* in, const yol_raster_desc* d,
                           yol_box* box);

/* --- curve sets, format v1 (docs/gfx.md, "Curve sets: the format") ------ */

typedef struct yol_cset_desc {
    uint32_t n_glyphs;      /* G, the glyph table size, < 2^24                  */
    int      nh, nv;        /* bands per glyph, 1..65535; 0: clamp(round(sqrt(
                             * curves)), 1, 16)                                 */
    bool     backward;      /* backward lists (flag bit 1)                      */
    bool     keep_overlaps; /* false (the default): resolve every glyph first   */
} yol_cset_desc;

typedef struct yol_cset {
    float*          texels;   uint32_t n_texels, cap_texels;   /* 4 floats each */
    uint32_t*       words;    uint32_t n_words,  cap_words;
    yol_cset_desc desc;
    yol_ctx*      ctx;
} yol_cset;

/* The header and an empty glyph table of d->n_glyphs entries. On c's
 * allocator, or (c NULL) in the caller's texels, words and caps, which
 * open keeps. False, with the reason in yol_error(c) when c is not NULL,
 * for a bad desc or no room. */
YOL_API bool yol_cset_open(yol_cset* s, yol_ctx* c, const yol_cset_desc* d);
/* Frees what open and add grew on the context; the caller's storage is the
 * caller's. Safe on a zeroed set, after a failed open and twice. */
YOL_API void yol_cset_close(yol_cset* s);
/* Adds glyph `glyph`, absent before, from p: vertices rounded to f32 once,
 * contours, the record, the bands and their lists, then the table entry
 * last. An empty or zero-area p gives an empty record. */
YOL_API int  yol_cset_add(yol_cset* s, uint32_t glyph, const yol_path* p);
/* Font glyphs in em, y down, pen at 0, 0. glyphs NULL: every glyph 0 .. n - 1
 * (n 0: the font's count). tol: the CFF cubic tolerance in em (0: 1e-4). */
YOL_API int  yol_cset_add_font(yol_cset* s, const yol_font* f, const uint32_t* glyphs,
                                   uint32_t n, double tol);

/* --- SVG subset (SVG) --------------------------------------------------------- */

#define YOL_SVG_FILL   1
#define YOL_SVG_STROKE 2

/* One solid-color layer: one curve-set glyph. */
typedef struct yol_svg_layer {
    yol_path path;          /* the region, resolved, in viewBox units, y down;
                               * yol_svg() inits it on the context: free it   */
    uint32_t   rgba;          /* sRGB 0xRRGGBBAA as the file gave it, the alpha
                               * from fill-opacity or stroke-opacity times the
                               * shape's opacity; not converted (ysp/color.h)   */
    int32_t    source;        /* YOL_SVG_FILL or YOL_SVG_STROKE             */
    int32_t    element;       /* the shape's index in document order            */
} yol_svg_layer;

typedef struct yol_svg_desc {
    double           tol;     /* viewBox units; 0: 1e-4 of the viewBox's larger
                               * side                                           */
    yol_svg_layer* layers;  /* the caller's array                              */
    int              max_layers;
} yol_svg_desc;

/* UTF-8 SVG text to layers, bottom to top (a shape's fill, then its
 * stroke). Returns the layer count, or a code: YOL_ERR_REFUSED names what
 * the subset leaves out, YOL_ERR_FORMAT names malformed XML or path data,
 * YOL_ERR_FULL when max_layers is short. viewbox (may be NULL) gets x, y,
 * width, height. err may be NULL. */
YOL_API int yol_svg(yol_ctx* c, const char* text, size_t n, const yol_svg_desc* d,
                        double viewbox[4], char* err, size_t cap);

/* --- parameter tables (rig_spec 1.8) --------------------------------------- */

typedef struct yol_param {
    const char* name;         /* the desc field                                  */
    const char* type;         /* "f64", "i32", "bool", "enum", "u32"             */
    double      min, max;     /* inclusive                                       */
    double      def;          /* the value a zero field means                    */
    const char* unit;
    const char* doc;
    uint32_t    offset;       /* in the desc                                     */
} yol_param;

YOL_API const yol_param* yol_stroke_params(int* n);
YOL_API const yol_param* yol_raster_params(int* n);
YOL_API const yol_param* yol_cset_params(int* n);
YOL_API const yol_param* yol_glyph_params(int* n);

#ifdef __cplusplus
}
#endif

#endif /* YSP_OUTLINE_H_INCLUDED */

/* ======================================================================= *
 *                            IMPLEMENTATION                               *
 * ======================================================================= */
#ifdef YSP_OUTLINE_IMPLEMENTATION
#ifndef YSP_OUTLINE_IMPLEMENTATION_GUARD
#define YSP_OUTLINE_IMPLEMENTATION_GUARD

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)   /* vsnprintf is fine here */
#endif

#define YOL__PI 3.14159265358979323846

YOL_API const char* yol_version(void) { return YOL_VERSION_STRING; }

YOL_API const char* yol_strerror(int code) {
    switch (code) {
    case YOL_OK: return "ok";
    case YOL_ERR_ARG: return "bad argument";
    case YOL_ERR_FULL: return "full";
    case YOL_ERR_REFUSED: return "refused";
    case YOL_ERR_FORMAT: return "malformed input";
    case YOL_ERR_RANGE: return "out of range";
    case YOL_ERR_NUMERIC: return "numeric failure";
    default: return "unknown code";
    }
}

static int yol__err(yol_ctx* c, int code, const char* fmt, ...) {
    va_list ap;
    if (!c) return code;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
    c->n_err++;
    return code;
}

/* A public call's last word: a failure that wrote no message since n0 gets
 * a generic one, so yol_error() always belongs to the last failure. */
static int yol__done(yol_ctx* c, uint32_t n0, int rc, const char* what) {
    if (c && rc < 0 && c->n_err == n0) yol__err(c, rc, "%s: %s", what, yol_strerror(rc));
    return rc;
}

static int yol__finite(double v) { return v == v && v - v == 0.0; }

/* --- allocator ------------------------------------------------------------- */

/* A fixed arena is a bump allocator. A block stores its size in front; the
 * last block grows and shrinks in place, any other moves. */
#define YOL__AHDR 16u

static void* yol__arena_realloc(yol_ctx* c, void* p, size_t n) {
    size_t old = 0, off;
    if (p) {
        off = (size_t)((uint8_t*)p - c->arena) - YOL__AHDR;
        memcpy(&old, c->arena + off, sizeof old);
        if (off == c->arena_last) {
            if (n == 0) { c->arena_used = off; return NULL; }
            if (off + YOL__AHDR + n <= c->arena_size) {
                memcpy(c->arena + off, &n, sizeof n);
                c->arena_used = off + YOL__AHDR + ((n + 15u) & ~(size_t)15u);
                if (c->arena_used > c->arena_size) c->arena_used = c->arena_size;
                return p;
            }
            return NULL;
        }
        if (n == 0) return NULL;   /* freed in place: the space is lost */
    }
    if (n == 0) return NULL;
    {
        size_t need = YOL__AHDR + ((n + 15u) & ~(size_t)15u);
        uint8_t* q;
        if (c->arena_used > c->arena_size || need > c->arena_size - c->arena_used) return NULL;
        off = c->arena_used;
        q = c->arena + off;
        memcpy(q, &n, sizeof n);
        c->arena_last = off;
        c->arena_used += need;
        if (p) memcpy(q + YOL__AHDR, p, old < n ? old : n);
        return q + YOL__AHDR;
    }
}

static void* yol__realloc(yol_ctx* c, void* p, size_t n) {
    c->n_alloc++;
    if (c->arena) return yol__arena_realloc(c, p, n);
    if (c->realloc_fn) return c->realloc_fn(c->user, p, n);
    if (n == 0) { free(p); return NULL; }
    return realloc(p, n);
}

/* Grows *pp to hold `need` bytes (doubling); YOL_ERR_FULL with a message. */
static int yol__grow(yol_ctx* c, void** pp, size_t* cap, size_t need) {
    size_t n;
    void* q;
    if (need <= *cap) return YOL_OK;
    if (!c) return YOL_ERR_FULL;
    n = *cap ? *cap : 256;
    while (n < need) {
        if (n > ((size_t)-1) / 2) return yol__err(c, YOL_ERR_FULL, "a buffer of %lu bytes is too large", (unsigned long)need);
        n *= 2;
    }
    q = yol__realloc(c, *pp, n);
    if (!q) return yol__err(c, YOL_ERR_FULL, "out of memory: %lu bytes needed%s", (unsigned long)n,
                              c->arena ? " in the fixed arena" : "");
    *pp = q;
    *cap = n;
    return YOL_OK;
}

/* Scratch slot k with room for `bytes`, or NULL (message set). */
static void* yol__scratch(yol_ctx* c, int k, size_t bytes) {
    if (yol__grow(c, &c->scratch[k], &c->scratch_cap[k], bytes ? bytes : 1) < 0) return NULL;
    return c->scratch[k];
}

enum {
    YOL__S_PIECE, YOL__S_KV, YOL__S_KV2, YOL__S_EVENT, YOL__S_HEAP, YOL__S_LIVE,
    YOL__S_POS, YOL__S_PAIR, YOL__S_RUN, YOL__S_SEG, YOL__S_IVA, YOL__S_IVB,
    YOL__S_ACC, YOL__S_FONT, YOL__S_TP_PTS, YOL__S_TP_CON, YOL__S_TQ_PTS, YOL__S_TQ_CON,
    YOL__S_ORDER, YOL__S_USED, YOL__S_GROUP, YOL__S_CSET, YOL__S_CSET2, YOL__S_MISC,
    YOL__S_STROKE, YOL__S_STROKE2
};

YOL_API bool yol_open(yol_ctx* c, const yol_ctx_desc* d) {
    if (!c) return false;
    memset(c, 0, sizeof *c);
    if (d) {
        if (d->mem && d->realloc) { yol__err(c, YOL_ERR_ARG, "give realloc or a fixed arena, not both"); return false; }
        if (d->mem && (((uintptr_t)d->mem) & 15u)) { yol__err(c, YOL_ERR_ARG, "the arena must be 16-byte aligned"); return false; }
        c->realloc_fn = d->realloc;
        c->user = d->user;
        c->arena = (uint8_t*)d->mem;
        c->arena_size = d->mem ? d->mem_size : 0;
    }
    c->arena_last = (size_t)-1;
    c->inited = 1;
    return true;
}

YOL_API void yol_close(yol_ctx* c) {
    int k;
    if (!c) return;
    if (!c->arena) for (k = 0; k < YOL__NSCRATCH; k++) if (c->scratch[k]) yol__realloc(c, c->scratch[k], 0);
    memset(c, 0, sizeof *c);
}

YOL_API const char* yol_error(const yol_ctx* c) { return c ? c->err : "no context"; }

/* --- transforms ------------------------------------------------------------ */

YOL_API yol_xform yol_xf_identity(void) { yol_xform t = { 1, 0, 0, 1, 0, 0 }; return t; }
YOL_API yol_xform yol_xf_mul(yol_xform l, yol_xform r) {
    yol_xform t;
    t.a = l.a * r.a + l.c * r.b;  t.b = l.b * r.a + l.d * r.b;
    t.c = l.a * r.c + l.c * r.d;  t.d = l.b * r.c + l.d * r.d;
    t.e = l.a * r.e + l.c * r.f + l.e;  t.f = l.b * r.e + l.d * r.f + l.f;
    return t;
}
YOL_API yol_xform yol_xf_translate(double x, double y) { yol_xform t = { 1, 0, 0, 1, 0, 0 }; t.e = x; t.f = y; return t; }
YOL_API yol_xform yol_xf_scale(double sx, double sy) { yol_xform t = { 1, 0, 0, 1, 0, 0 }; t.a = sx; t.d = sy; return t; }
YOL_API yol_xform yol_xf_rotate(double deg) {
    /* exact for multiples of 90 degrees, so a rotated rectangle stays exact */
    yol_xform t = { 1, 0, 0, 1, 0, 0 };
    double r = fmod(deg, 360.0), s, co;
    if (r < 0) r += 360.0;
    if (r == 0) { s = 0; co = 1; } else if (r == 90) { s = 1; co = 0; }
    else if (r == 180) { s = 0; co = -1; } else if (r == 270) { s = -1; co = 0; }
    else { s = sin(r * YOL__PI / 180.0); co = cos(r * YOL__PI / 180.0); }
    t.a = co; t.b = s; t.c = -s; t.d = co;
    return t;
}
YOL_API yol_xform yol_xf_skew(double xdeg, double ydeg) {
    yol_xform t = { 1, 0, 0, 1, 0, 0 };
    t.c = tan(xdeg * YOL__PI / 180.0);
    t.b = tan(ydeg * YOL__PI / 180.0);
    return t;
}
/* The largest singular value of the linear part: how far the transform can
 * stretch an error. */
static double yol__xf_smax(yol_xform t) {
    double s = 0.5 * (t.a * t.a + t.b * t.b + t.c * t.c + t.d * t.d), det = t.a * t.d - t.b * t.c, q = s * s - det * det;
    return sqrt(s + sqrt(q > 0 ? q : 0));
}

/* --- paths ----------------------------------------------------------------- */

YOL_API void yol_path_init(yol_path* p, yol_ctx* c) {
    if (!p) return;
    if (c) memset(p, 0, sizeof *p);
    else { p->n_pts = 0; p->n_contours = 0; p->rule = 0; p->flags = 0; p->status = 0; p->in_contour = 0; p->ctx = NULL; }
    p->ctx = c;
    p->xf = yol_xf_identity();
    p->tol = 0;
}

YOL_API void yol_path_free(yol_path* p) {
    if (!p) return;
    if (p->ctx) {
        if (p->pts) yol__realloc(p->ctx, p->pts, 0);
        if (p->contours) yol__realloc(p->ctx, p->contours, 0);
        memset(p, 0, sizeof *p);
    }
}

YOL_API void yol_path_clear(yol_path* p) {
    if (!p) return;
    p->n_pts = 0; p->n_contours = 0; p->flags = 0; p->status = 0; p->in_contour = 0;
}

static int yol__path_reserve(yol_path* p, int32_t pts, int32_t cons) {
    int rc;
    if (pts > 0x3fffffff - p->n_pts || cons > 0x3fffffff - p->n_contours) {
        return yol__err(p->ctx, YOL_ERR_FULL, "a path of more than 2^30 points");
    }
    if (p->n_pts + pts > p->cap_pts) {
        size_t cap = (size_t)p->cap_pts * 2 * sizeof(double);
        void* q = p->pts;
        if (!p->ctx) return YOL_ERR_FULL;
        rc = yol__grow(p->ctx, &q, &cap, (size_t)(p->n_pts + pts) * 2 * sizeof(double));
        if (rc < 0) return rc;
        p->pts = (double*)q;
        p->cap_pts = (int32_t)(cap / (2 * sizeof(double)) > 0x3fffffff ? 0x3fffffff : cap / (2 * sizeof(double)));
    }
    if (p->n_contours + cons > p->cap_contours) {
        size_t cap = (size_t)p->cap_contours * sizeof(uint32_t);
        void* q = p->contours;
        if (!p->ctx) return YOL_ERR_FULL;
        rc = yol__grow(p->ctx, &q, &cap, (size_t)(p->n_contours + cons) * sizeof(uint32_t));
        if (rc < 0) return rc;
        p->contours = (uint32_t*)q;
        p->cap_contours = (int32_t)(cap / sizeof(uint32_t) > 0x3fffffff ? 0x3fffffff : cap / sizeof(uint32_t));
    }
    return YOL_OK;
}

static int32_t yol__cfirst(const yol_path* p, int32_t k) { return (int32_t)(p->contours[k] & ~YOL_OPEN); }
static int32_t yol__cend(const yol_path* p, int32_t k) { return k + 1 < p->n_contours ? yol__cfirst(p, k + 1) : p->n_pts; }

YOL_API int yol_path_copy(yol_path* dst, const yol_path* src) {
    int rc;
    if (!dst || !src) return YOL_ERR_ARG;
    if (dst == src) return YOL_OK;
    dst->n_pts = 0; dst->n_contours = 0; dst->in_contour = 0;
    rc = yol__path_reserve(dst, src->n_pts, src->n_contours);
    if (rc < 0) return rc;
    if (src->n_pts) memcpy(dst->pts, src->pts, (size_t)src->n_pts * 2 * sizeof(double));
    if (src->n_contours) memcpy(dst->contours, src->contours, (size_t)src->n_contours * sizeof(uint32_t));
    dst->n_pts = src->n_pts; dst->n_contours = src->n_contours;
    dst->rule = src->rule; dst->flags = src->flags;
    return YOL_OK;
}

static void yol__fail(yol_path* p, int code) { if (p->status == 0) p->status = code; }

/* Drops a contour that has only its start point. */
static void yol__end_contour(yol_path* p) {
    if (p->in_contour && p->n_contours > 0 && p->n_pts - yol__cfirst(p, p->n_contours - 1) < 3) {
        p->n_pts = yol__cfirst(p, p->n_contours - 1);
        p->n_contours--;
    }
    p->in_contour = 0;
}

static void yol__start(yol_path* p, double x, double y) {
    double X = p->xf.a * x + p->xf.c * y + p->xf.e, Y = p->xf.b * x + p->xf.d * y + p->xf.f;
    yol__end_contour(p);
    if (!yol__finite(X) || !yol__finite(Y)) { yol__fail(p, yol__err(p->ctx, YOL_ERR_ARG, "a point is not finite")); return; }
    if (yol__path_reserve(p, 1, 1) < 0) { yol__fail(p, YOL_ERR_FULL); return; }
    p->contours[p->n_contours++] = (uint32_t)p->n_pts | YOL_OPEN;
    p->pts[2 * p->n_pts] = X; p->pts[2 * p->n_pts + 1] = Y; p->n_pts++;
    p->ux = p->usx = x; p->uy = p->usy = y;
    p->in_contour = 1;
    p->flags &= ~YOL_PATH_RESOLVED;
}

YOL_API void yol_move(yol_path* p, double x, double y) {
    if (!p || p->status) return;
    yol__start(p, x, y);
}

/* Appends a segment in path (transformed) coordinates; line when cx, cy is
 * NULL: its control is a copy of the start's bits. */
static void yol__seg_raw(yol_path* p, const double* ctrl, double X, double Y) {
    double* q;
    if (!yol__finite(X) || !yol__finite(Y) || (ctrl && (!yol__finite(ctrl[0]) || !yol__finite(ctrl[1])))) {
        yol__fail(p, yol__err(p->ctx, YOL_ERR_ARG, "a point is not finite")); return;
    }
    if (yol__path_reserve(p, 2, 0) < 0) { yol__fail(p, YOL_ERR_FULL); return; }
    q = p->pts + 2 * p->n_pts;
    if (ctrl) { q[0] = ctrl[0]; q[1] = ctrl[1]; } else { q[0] = q[-2]; q[1] = q[-1]; }
    q[2] = X; q[3] = Y;
    p->n_pts += 2;
}

static void yol__ensure(yol_path* p) { if (!p->in_contour) yol__start(p, p->ux, p->uy); }

YOL_API void yol_line(yol_path* p, double x, double y) {
    if (!p || p->status) return;
    yol__ensure(p);
    if (p->status) return;
    yol__seg_raw(p, NULL, p->xf.a * x + p->xf.c * y + p->xf.e, p->xf.b * x + p->xf.d * y + p->xf.f);
    p->ux = x; p->uy = y;
}

YOL_API void yol_quad(yol_path* p, double cx, double cy, double x, double y) {
    double c2[2];
    if (!p || p->status) return;
    yol__ensure(p);
    if (p->status) return;
    c2[0] = p->xf.a * cx + p->xf.c * cy + p->xf.e; c2[1] = p->xf.b * cx + p->xf.d * cy + p->xf.f;
    yol__seg_raw(p, c2, p->xf.a * x + p->xf.c * y + p->xf.e, p->xf.b * x + p->xf.d * y + p->xf.f);
    p->ux = x; p->uy = y;
}

static double yol__tol(const yol_path* p) { return p->tol > 0 ? p->tol : 1e-4; }

/* CUBICS: n uniform pieces, each the quadratic with the same ends and the
 * control point (3 (C1 + C2) - (A + B)) / 4 of its cubic A C1 C2 B. Its
 * distance from the cubic is at most sqrt(3) / 36 |B - 3 C2 + 3 C1 - A|,
 * and that third difference is the whole cubic's divided by n^3. */
#define YOL__CUBIC_MAX 4096
static int yol__cubic_n(double d3, double tol) {
    double n = ceil(cbrt(sqrt(3.0) / 36.0 * d3 / tol));
    if (!(n >= 1)) n = 1;
    return n > YOL__CUBIC_MAX ? -1 : (int)n;
}

static void yol__cubic_raw(yol_path* p, const double P[8], double tol) {
    double d3x = P[6] - 3 * P[4] + 3 * P[2] - P[0], d3y = P[7] - 3 * P[5] + 3 * P[3] - P[1];
    int n = yol__cubic_n(sqrt(d3x * d3x + d3y * d3y), tol), i;
    if (n < 0) { yol__fail(p, yol__err(p->ctx, YOL_ERR_RANGE, "a cubic needs more than %d quadratics at tolerance %g", YOL__CUBIC_MAX, tol)); return; }
    if (P[0] == P[2] && P[1] == P[3] && P[4] == P[6] && P[5] == P[7]) { yol__seg_raw(p, NULL, P[6], P[7]); return; }
    for (i = 0; i < n; i++) {
        /* the sub-cubic on [t0, t1] by blossoming */
        double t0 = (double)i / n, t1 = (double)(i + 1) / n, A[2], C1[2], C2[2], B[2], ctrl[2];
        int k;
        for (k = 0; k < 2; k++) {
            double p0 = P[k], p1 = P[2 + k], p2 = P[4 + k], p3 = P[6 + k];
#define YOL__BL3(u, v, w) ((1 - (u)) * (1 - (v)) * (1 - (w)) * p0 + \
            ((u) * (1 - (v)) * (1 - (w)) + (1 - (u)) * (v) * (1 - (w)) + (1 - (u)) * (1 - (v)) * (w)) * p1 + \
            ((u) * (v) * (1 - (w)) + (u) * (1 - (v)) * (w) + (1 - (u)) * (v) * (w)) * p2 + (u) * (v) * (w) * p3)
            A[k] = YOL__BL3(t0, t0, t0);
            C1[k] = YOL__BL3(t0, t0, t1);
            C2[k] = YOL__BL3(t0, t1, t1);
            B[k] = YOL__BL3(t1, t1, t1);
#undef YOL__BL3
        }
        if (i == n - 1) { B[0] = P[6]; B[1] = P[7]; }
        (void)A;
        /* A is the previous end; the control uses the blossom values */
        ctrl[0] = (3 * (C1[0] + C2[0]) - (A[0] + B[0])) * 0.25;
        ctrl[1] = (3 * (C1[1] + C2[1]) - (A[1] + B[1])) * 0.25;
        yol__seg_raw(p, ctrl, B[0], B[1]);
        if (p->status) return;
    }
}

YOL_API void yol_cubic(yol_path* p, double c1x, double c1y, double c2x, double c2y, double x, double y) {
    double P[8];
    yol_xform t;
    if (!p || p->status) return;
    yol__ensure(p);
    if (p->status) return;
    t = p->xf;
    P[0] = p->pts[2 * p->n_pts - 2]; P[1] = p->pts[2 * p->n_pts - 1];
    P[2] = t.a * c1x + t.c * c1y + t.e; P[3] = t.b * c1x + t.d * c1y + t.f;
    P[4] = t.a * c2x + t.c * c2y + t.e; P[5] = t.b * c2x + t.d * c2y + t.f;
    P[6] = t.a * x + t.c * y + t.e;     P[7] = t.b * x + t.d * y + t.f;
    if (!yol__finite(P[2]) || !yol__finite(P[3]) || !yol__finite(P[4]) || !yol__finite(P[5])) {
        yol__fail(p, yol__err(p->ctx, YOL_ERR_ARG, "a point is not finite")); return;
    }
    yol__cubic_raw(p, P, yol__tol(p));
    p->ux = x; p->uy = y;
}

YOL_API void yol_close_path(yol_path* p) {
    int32_t f;
    if (!p || p->status || !p->in_contour) return;
    f = yol__cfirst(p, p->n_contours - 1);
    if (p->n_pts - f >= 3) {
        if (p->pts[2 * p->n_pts - 2] != p->pts[2 * f] || p->pts[2 * p->n_pts - 1] != p->pts[2 * f + 1]) {
            double X = p->pts[2 * f], Y = p->pts[2 * f + 1];
            yol__seg_raw(p, NULL, X, Y);
            if (p->status) return;
        }
        p->contours[p->n_contours - 1] &= ~YOL_OPEN;
    }
    yol__end_contour(p);
    p->ux = p->usx; p->uy = p->usy;
}

YOL_API void yol_set_xform(yol_path* p, yol_xform t) { if (p) p->xf = t; }
YOL_API void yol_set_tol(yol_path* p, double tol) { if (p) p->tol = tol > 0 ? tol : 0; }

YOL_API int yol_path_end(yol_path* p) {
    int rc;
    if (!p) return YOL_ERR_ARG;
    yol__end_contour(p);
    rc = p->status;
    p->status = 0;
    return rc;
}

/* --- arcs ------------------------------------------------------------------ */

/* The largest angle of one quadratic per circle arc segment within relative
 * error e: the quadratic through the ends with the tangents' intersection as
 * control is off the arc by r (1 - cos(a/2))^2 / (2 cos(a/2)) at its middle,
 * its largest. */
static double yol__arc_step(double e) {
    double c = (1 + e) - sqrt((1 + e) * (1 + e) - 1);
    if (c > 1) c = 1;
    if (c < 0.5) c = 0.5;   /* at most 120 degrees a piece */
    return 2 * acos(c);
}

/* cos and sin, exact at multiples of 90 degrees, so a quarter arc ends
 * exactly where the next line of a rounded rectangle starts */
static void yol__cossin(double a, double* c, double* s) {
    double q = a / (0.5 * YOL__PI), r = floor(q + 0.5);
    if (fabs(q - r) < 1e-12) {
        int k = (int)fmod(fmod(r, 4.0) + 4.0, 4.0);
        *c = k == 0 ? 1 : (k == 2 ? -1 : 0);
        *s = k == 1 ? 1 : (k == 3 ? -1 : 0);
    } else { *c = cos(a); *s = sin(a); }
}

/* An ellipse arc (center cx, cy, radii, rotation phi, from angle t0 by dt)
 * in user coordinates, from the current point, which is its start. */
static void yol__ell_arc(yol_path* p, double cx, double cy, double rx, double ry, double phi, double t0, double dt) {
    yol_xform m = p->xf, l;
    double cp = cos(phi), sp = sin(phi), smax, step;
    int n, i;
    l.a = m.a * cp * rx + m.c * sp * rx; l.b = m.b * cp * rx + m.d * sp * rx;
    l.c = -m.a * sp * ry + m.c * cp * ry; l.d = -m.b * sp * ry + m.d * cp * ry;
    l.e = l.f = 0;
    smax = yol__xf_smax(l);
    if (!(smax > 0)) return;
    step = yol__arc_step(yol__tol(p) / smax);
    n = (int)ceil(fabs(dt) / step);
    if (n < 1) n = 1;
    if (n > 4096) { yol__fail(p, yol__err(p->ctx, YOL_ERR_RANGE, "an arc needs more than 4096 quadratics")); return; }
    for (i = 0; i < n; i++) {
        double a0 = t0 + dt * i / n, a1 = t0 + dt * (i + 1) / n, am = 0.5 * (a0 + a1), k = 1.0 / cos(0.5 * (a1 - a0)), c1, s1, cm, sm, ux, uy, ex, ey;
        yol__cossin(a1, &c1, &s1); yol__cossin(am, &cm, &sm);
        ux = rx * cm * k; uy = ry * sm * k; ex = rx * c1; ey = ry * s1;
        double qx = cx + cp * ux - sp * uy, qy = cy + sp * ux + cp * uy;
        double px = cx + cp * ex - sp * ey, py = cy + sp * ex + cp * ey;
        yol_quad(p, qx, qy, px, py);
        if (p->status) return;
    }
}

YOL_API void yol_arc_to(yol_path* p, double rx, double ry, double rot_deg, int large, int sweep, double x, double y) {
    /* SVG 1.1 F.6.5: endpoint to center parameterization */
    double x1, y1, phi, cp, sp, dx, dy, x1p, y1p, lam, num, den, co, cxp, cyp, cx, cy, ux, uy, vx, vy, t0, dt;
    if (!p || p->status) return;
    yol__ensure(p);
    if (p->status) return;
    x1 = p->ux; y1 = p->uy;
    if (x1 == x && y1 == y) return;
    rx = fabs(rx); ry = fabs(ry);
    if (rx == 0 || ry == 0) { yol_line(p, x, y); return; }
    phi = rot_deg * YOL__PI / 180.0; cp = cos(phi); sp = sin(phi);
    dx = 0.5 * (x1 - x); dy = 0.5 * (y1 - y);
    x1p = cp * dx + sp * dy; y1p = -sp * dx + cp * dy;
    lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if (lam > 1) { lam = sqrt(lam); rx *= lam; ry *= lam; }
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    co = (den > 0 && num > 0) ? sqrt(num / den) : 0;
    if ((large != 0) == (sweep != 0)) co = -co;
    cxp = co * rx * y1p / ry; cyp = -co * ry * x1p / rx;
    cx = cp * cxp - sp * cyp + 0.5 * (x1 + x); cy = sp * cxp + cp * cyp + 0.5 * (y1 + y);
    ux = (x1p - cxp) / rx; uy = (y1p - cyp) / ry; vx = (-x1p - cxp) / rx; vy = (-y1p - cyp) / ry;
    t0 = atan2(uy, ux);
    dt = atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    if (!sweep && dt > 0) dt -= 2 * YOL__PI;
    else if (sweep && dt < 0) dt += 2 * YOL__PI;
    yol__ell_arc(p, cx, cy, rx, ry, phi, t0, dt);
    if (p->status) return;
    /* the exact end point, not the last arc sample's rounding */
    {
        double X = p->xf.a * x + p->xf.c * y + p->xf.e, Y = p->xf.b * x + p->xf.d * y + p->xf.f;
        p->pts[2 * p->n_pts - 2] = X; p->pts[2 * p->n_pts - 1] = Y;
    }
    p->ux = x; p->uy = y;
}

YOL_API void yol_ellipse(yol_path* p, double cx, double cy, double rx, double ry) {
    if (!p || p->status) return;
    rx = fabs(rx); ry = fabs(ry);
    if (rx == 0 || ry == 0) return;
    yol_move(p, cx + rx, cy);
    yol__ell_arc(p, cx, cy, rx, ry, 0, 0, 2 * YOL__PI);
    if (p->status) return;
    yol_close_path(p);
}

YOL_API void yol_rect(yol_path* p, double x, double y, double w, double h, double rx, double ry) {
    if (!p || p->status) return;
    if (!(w > 0) || !(h > 0)) return;
    rx = fabs(rx); ry = fabs(ry);
    if (rx > 0 && ry == 0) ry = rx;
    if (ry > 0 && rx == 0) rx = ry;
    if (rx > 0.5 * w) rx = 0.5 * w;
    if (ry > 0.5 * h) ry = 0.5 * h;
    if (rx == 0 || ry == 0) {
        yol_move(p, x, y); yol_line(p, x + w, y); yol_line(p, x + w, y + h); yol_line(p, x, y + h); yol_close_path(p);
        return;
    }
    yol_move(p, x + rx, y);
    yol_line(p, x + w - rx, y);
    yol__ell_arc(p, x + w - rx, y + ry, rx, ry, 0, -0.5 * YOL__PI, 0.5 * YOL__PI);
    yol_line(p, x + w, y + h - ry);
    yol__ell_arc(p, x + w - rx, y + h - ry, rx, ry, 0, 0, 0.5 * YOL__PI);
    yol_line(p, x + rx, y + h);
    yol__ell_arc(p, x + rx, y + h - ry, rx, ry, 0, 0.5 * YOL__PI, 0.5 * YOL__PI);
    yol_line(p, x, y + ry);
    yol__ell_arc(p, x + rx, y + ry, rx, ry, 0, YOL__PI, 0.5 * YOL__PI);
    yol_close_path(p);
}

/* integral of x dy along a quadratic */
static double yol__xdy(double x0, double y0, double x1, double y1, double x2, double y2) {
    double d1 = y1 - y0, d2 = y2 - y1;
    return 0.5 * x0 * d1 + (x0 * d2 + 2 * x1 * d1 + 2 * x1 * d2 + x2 * d1) / 6.0 + 0.5 * x2 * d2;
}

YOL_API double yol_path_area(const yol_path* p) {
    double s = 0;
    int32_t k, i;
    if (!p) return 0;
    for (k = 0; k < p->n_contours; k++) {
        int32_t f = yol__cfirst(p, k), e = yol__cend(p, k);
        const double* q = p->pts;
        for (i = f; i + 2 < e; i += 2)
            s += yol__xdy(q[2 * i], q[2 * i + 1], q[2 * i + 2], q[2 * i + 3], q[2 * i + 4], q[2 * i + 5]);
        if (e - f >= 3) {
            double lx = q[2 * (e - 1)], ly = q[2 * (e - 1) + 1];
            if (lx != q[2 * f] || ly != q[2 * f + 1]) s += 0.5 * (lx + q[2 * f]) * (q[2 * f + 1] - ly);
        }
    }
    return s;
}

/* --- fonts ------------------------------------------------------------------ */
/* FONTS: every offset and length is checked before its read; every loop and
 * recursion has a bound (composite depth 8 and 4096 component visits, CFF
 * stack 48, subroutine depth 10, 65536 charstring tokens per glyph). */

#define YOL__COMPOSITE_DEPTH 8
#define YOL__COMPOSITE_VISITS 4096
#define YOL__CFF_STACK 48
#define YOL__CFF_CALLS 10
#define YOL__CFF_TOKENS 65536
#define YOL__CFF_STEMS 96
#ifndef YOL_GLYPH_POINTS
#define YOL_GLYPH_POINTS (1 << 17)    /* path points one glyph may add */
#endif
#define YOL__GLYPH_POINTS YOL_GLYPH_POINTS

static int yol__in(size_t size, uint32_t off, uint32_t len) { return (size_t)off <= size && (size_t)len <= size - (size_t)off; }
static uint32_t yol__u8(const uint8_t* p) { return p[0]; }
static uint32_t yol__u16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static int32_t  yol__s16(const uint8_t* p) { return (int32_t)(int16_t)(uint16_t)yol__u16(p); }
static uint32_t yol__u32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int yol__ferr(char* err, size_t cap, int code, const char* fmt, ...) {
    va_list ap;
    if (!err || cap == 0) return code;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return code;
}

YOL_API int yol_font_faces(const void* data, size_t size) {
    const uint8_t* d = (const uint8_t*)data;
    uint32_t v;
    if (!d || size < 12) return YOL_ERR_FORMAT;
    if (memcmp(d, "ttcf", 4) == 0) {
        uint32_t n = yol__u32(d + 8);
        if (n == 0 || n > 65536 || !yol__in(size, 12, 4 * n)) return YOL_ERR_FORMAT;
        return (int)n;
    }
    v = yol__u32(d);
    return (v == 0x00010000u || v == 0x4F54544Fu || v == 0x74727565u) ? 1 : YOL_ERR_FORMAT;
}

/* A CFF INDEX at absolute offset off: count, offset size, data base. */
typedef struct yol__idx { uint32_t off, count, osz, data, end; } yol__idx;

static int yol__index(const yol_font* f, uint32_t off, yol__idx* ix) {
    uint32_t last, i;
    const uint8_t* d = f->data;
    memset(ix, 0, sizeof *ix);
    ix->off = off;
    if (!yol__in(f->size, off, 2)) return -1;
    ix->count = yol__u16(d + off);
    if (ix->count == 0) { ix->end = off + 2; return 0; }
    if (!yol__in(f->size, off, 3)) return -1;
    ix->osz = yol__u8(d + off + 2);
    if (ix->osz < 1 || ix->osz > 4) return -1;
    if (!yol__in(f->size, off + 3, (ix->count + 1) * ix->osz)) return -1;
    ix->data = off + 3 + (ix->count + 1) * ix->osz - 1;
    for (last = 0, i = 0; i < ix->osz; i++) last = last << 8 | d[off + 3 + ix->count * ix->osz + i];
    if (last < 1 || !yol__in(f->size, ix->data, last)) return -1;
    ix->end = ix->data + last;
    return 0;
}

static int yol__index_item(const yol_font* f, const yol__idx* ix, uint32_t i, uint32_t* start, uint32_t* len) {
    uint32_t a = 0, b = 0, k;
    const uint8_t* q;
    if (i >= ix->count) return -1;
    q = f->data + ix->off + 3 + i * ix->osz;
    for (k = 0; k < ix->osz; k++) { a = a << 8 | q[k]; b = b << 8 | q[ix->osz + k]; }
    if (a < 1 || b < a || ix->data + b > ix->end) return -1;
    *start = ix->data + a;
    *len = b - a;
    return 0;
}

/* A CFF DICT number at d[*i] (b0 already checked as an operand). */
static int yol__dict_num(const uint8_t* d, uint32_t* i, uint32_t end, double* v) {
    uint32_t b0 = d[*i];
    if (b0 >= 32 && b0 <= 246) { *v = (double)b0 - 139; *i += 1; return 0; }
    if (b0 >= 247 && b0 <= 250) { if (*i + 2 > end) return -1; *v = ((double)b0 - 247) * 256 + d[*i + 1] + 108; *i += 2; return 0; }
    if (b0 >= 251 && b0 <= 254) { if (*i + 2 > end) return -1; *v = -((double)b0 - 251) * 256 - d[*i + 1] - 108; *i += 2; return 0; }
    if (b0 == 28) { if (*i + 3 > end) return -1; *v = yol__s16(d + *i + 1); *i += 3; return 0; }
    if (b0 == 29) { if (*i + 5 > end) return -1; *v = (double)(int32_t)yol__u32(d + *i + 1); *i += 5; return 0; }
    if (b0 == 30) {
        /* a real: nibbles, parsed by hand (strtod follows the locale) */
        double m = 0, scale = 1; int exp = 0, esign = 1, neg = 0, state = 0, done = 0, digits = 0;
        *i += 1;
        while (!done) {
            uint32_t byte, h;
            int k;
            if (*i >= end || digits > 64) return -1;
            byte = d[(*i)++];
            for (k = 0; k < 2 && !done; k++) {
                h = k == 0 ? byte >> 4 : byte & 15u;
                digits++;
                if (h <= 9) {
                    if (state == 0) m = m * 10 + h;
                    else if (state == 1) { scale *= 0.1; m += h * scale; }
                    else exp = exp * 10 + (int)h;
                    if (exp > 400) exp = 400;
                } else if (h == 0xa) state = 1;
                else if (h == 0xb) { state = 2; esign = 1; }
                else if (h == 0xc) { state = 2; esign = -1; }
                else if (h == 0xe) neg = 1;
                else if (h == 0xf) done = 1;
                else return -1;
            }
        }
        *v = (neg ? -m : m) * pow(10.0, esign * exp);
        return 0;
    }
    return -1;
}

/* The operands of operator `op` (12 x is 1200 + x) in a DICT; their count,
 * 0 when absent, -1 when malformed. */
static int yol__dict_get(const yol_font* f, uint32_t start, uint32_t len, int op, double* vals, int maxv) {
    const uint8_t* d = f->data;
    uint32_t i = start, end = start + len;
    double st[YOL__CFF_STACK];
    int n = 0, k;
    if (!yol__in(f->size, start, len)) return -1;
    while (i < end) {
        uint32_t b0 = d[i];
        if (b0 <= 21) {
            int o = (int)b0;
            if (b0 == 12) { if (i + 1 >= end) return -1; o = 1200 + d[i + 1]; i += 2; } else i += 1;
            if (o == op) { for (k = 0; k < n && k < maxv; k++) vals[k] = st[k]; return n; }
            n = 0;
        } else {
            double v;
            if (b0 == 31 || b0 == 255 || (b0 >= 22 && b0 <= 27)) return -1;
            if (yol__dict_num(d, &i, end, &v) < 0) return -1;
            if (n >= YOL__CFF_STACK) return -1;
            st[n++] = v;
        }
    }
    return 0;
}

/* The local Subrs INDEX offset of a Private DICT given as (size, offset). */
static int yol__private_subrs(const yol_font* f, double size, double off, uint32_t* subrs) {
    double v[1];
    int n;
    *subrs = 0;
    if (!(size >= 0) || !(off >= 0) || size > (double)f->cff_len_ || off > (double)f->cff_len_) return -1;
    if (!yol__in(f->size, f->cff_ + (uint32_t)off, (uint32_t)size)) return -1;
    n = yol__dict_get(f, f->cff_ + (uint32_t)off, (uint32_t)size, 19, v, 1);
    if (n < 0) return -1;
    if (n >= 1) {
        if (!(v[0] >= 0) || off + v[0] > (double)f->cff_len_) return -1;
        *subrs = f->cff_ + (uint32_t)off + (uint32_t)v[0];
    }
    return 0;
}

static int yol__open_cff(yol_font* f, char* err, size_t cap) {
    const uint8_t* d = f->data;
    uint32_t c0 = f->cff_, pos, ts, tl;
    yol__idx names, top, strs, gs, cs;
    double v[8];
    int n;
    if (f->cff_len_ < 4 || yol__u8(d + c0) != 1) return yol__ferr(err, cap, YOL_ERR_FORMAT, "CFF: not version 1");
    pos = c0 + yol__u8(d + c0 + 2);
    if (yol__index(f, pos, &names) < 0) return yol__ferr(err, cap, YOL_ERR_FORMAT, "CFF: the Name INDEX");
    if (yol__index(f, names.end, &top) < 0 || top.count < 1) return yol__ferr(err, cap, YOL_ERR_FORMAT, "CFF: the Top DICT INDEX");
    if (yol__index(f, top.end, &strs) < 0) return yol__ferr(err, cap, YOL_ERR_FORMAT, "CFF: the String INDEX");
    if (yol__index(f, strs.end, &gs) < 0) return yol__ferr(err, cap, YOL_ERR_FORMAT, "CFF: the Global Subr INDEX");
    f->gsubr_ = gs.off;
    if (yol__index_item(f, &top, 0, &ts, &tl) < 0) return yol__ferr(err, cap, YOL_ERR_FORMAT, "CFF: the Top DICT");
    n = yol__dict_get(f, ts, tl, 1206, v, 1);
    if (n < 0 || (n >= 1 && v[0] != 2)) return yol__ferr(err, cap, YOL_ERR_REFUSED, "CFF: charstring type other than 2");
    n = yol__dict_get(f, ts, tl, 1207, v, 6);
    if (n < 0) return yol__ferr(err, cap, YOL_ERR_FORMAT, "CFF: the FontMatrix");
    if (n == 6 && (fabs(v[0] * f->units_per_em - 1) > 1e-9 || fabs(v[3] * f->units_per_em - 1) > 1e-9 || v[1] != 0 || v[2] != 0 || v[4] != 0 || v[5] != 0))
        return yol__ferr(err, cap, YOL_ERR_REFUSED, "CFF: a FontMatrix other than 1/unitsPerEm");
    n = yol__dict_get(f, ts, tl, 17, v, 1);
    if (n < 1 || !(v[0] > 0) || v[0] >= (double)f->cff_len_) return yol__ferr(err, cap, YOL_ERR_FORMAT, "CFF: no CharStrings");
    f->cs_ = c0 + (uint32_t)v[0];
    if (yol__index(f, f->cs_, &cs) < 0) return yol__ferr(err, cap, YOL_ERR_FORMAT, "CFF: the CharStrings INDEX");
    if ((int32_t)cs.count < f->n_glyphs) f->n_glyphs = (int32_t)cs.count;
    n = yol__dict_get(f, ts, tl, 1230, v, 3);
    if (n < 0) return yol__ferr(err, cap, YOL_ERR_FORMAT, "CFF: the ROS");
    if (n >= 1) {
        yol__idx fa;
        f->flags |= YOL_FONT_CID;
        n = yol__dict_get(f, ts, tl, 1236, v, 1);
        if (n < 1 || !(v[0] > 0) || v[0] >= (double)f->cff_len_) return yol__ferr(err, cap, YOL_ERR_FORMAT, "CFF: a CID font without FDArray");
        f->fdarray_ = c0 + (uint32_t)v[0];
        if (yol__index(f, f->fdarray_, &fa) < 0 || fa.count < 1) return yol__ferr(err, cap, YOL_ERR_FORMAT, "CFF: the FDArray INDEX");
        n = yol__dict_get(f, ts, tl, 1237, v, 1);
        if (n < 1 || !(v[0] > 0) || v[0] >= (double)f->cff_len_) return yol__ferr(err, cap, YOL_ERR_FORMAT, "CFF: a CID font without FDSelect");
        f->fdselect_ = c0 + (uint32_t)v[0];
        if (!yol__in(f->size, f->fdselect_, 1)) return yol__ferr(err, cap, YOL_ERR_FORMAT, "CFF: FDSelect");
        {
            uint32_t fmt = d[f->fdselect_];
            if (fmt != 0 && fmt != 3) return yol__ferr(err, cap, YOL_ERR_REFUSED, "CFF: FDSelect format %u", (unsigned)fmt);
        }
    } else {
        n = yol__dict_get(f, ts, tl, 18, v, 2);
        if (n < 0) return yol__ferr(err, cap, YOL_ERR_FORMAT, "CFF: the Private entry");
        if (n >= 2 && yol__private_subrs(f, v[0], v[1], &f->lsubr_) < 0) return yol__ferr(err, cap, YOL_ERR_FORMAT, "CFF: the Private DICT");
    }
    return YOL_OK;
}

static void yol__open_cmap(yol_font* f, uint32_t off, uint32_t len) {
    const uint8_t* d = f->data;
    uint32_t n, i, best = 0, best_fmt = 0;
    int best_rank = 0;
    if (len < 4 || !yol__in(f->size, off, len)) return;
    n = yol__u16(d + off + 2);
    if (!yol__in(f->size, off + 4, n * 8) || 4 + n * 8 > len) return;
    for (i = 0; i < n; i++) {
        uint32_t pid = yol__u16(d + off + 4 + 8 * i), eid = yol__u16(d + off + 6 + 8 * i), so = yol__u32(d + off + 8 + 8 * i), fmt;
        int rank = 0;
        if (so >= len || !yol__in(f->size, off + so, 2)) continue;
        fmt = yol__u16(d + off + so);
        if (fmt == 12 && ((pid == 3 && eid == 10) || pid == 0)) rank = 4;
        else if (fmt == 4 && pid == 3 && eid == 1) rank = 3;
        else if (fmt == 4 && pid == 0) rank = 2;
        if (rank > best_rank) { best_rank = rank; best = off + so; best_fmt = fmt; }
    }
    if (!best_rank) return;
    if (best_fmt == 4) {
        uint32_t L, seg2;
        if (!yol__in(f->size, best, 14)) return;
        L = yol__u16(d + best + 2); seg2 = yol__u16(d + best + 6);
        if (seg2 == 0 || (seg2 & 1) || !yol__in(f->size, best, L) || 16 + 4 * seg2 > L) return;
    } else {
        uint32_t L, ng;
        if (!yol__in(f->size, best, 16)) return;
        L = yol__u32(d + best + 4); ng = yol__u32(d + best + 12);
        if (ng > (L - 16) / 12 || L < 16 || !yol__in(f->size, best, L)) return;
    }
    f->cmap_ = best; f->cmap_fmt_ = best_fmt;
}

YOL_API int yol_font_view(yol_font* f, const void* data, size_t size, int index, char* err, size_t cap) {
    const uint8_t* d = (const uint8_t*)data;
    uint32_t base = 0, nt, i, head = 0, head_n = 0, maxp = 0, maxp_n = 0, hhea = 0, hhea_n = 0, cff2 = 0, cmap = 0, cmap_n = 0;
    int faces;
    if (!f || !d) return yol__ferr(err, cap, YOL_ERR_ARG, "no font or no bytes");
    memset(f, 0, sizeof *f);
    if (err && cap) err[0] = 0;
    if (size > 0xffffffffu) return yol__ferr(err, cap, YOL_ERR_RANGE, "a font of 4 GB or more");
    faces = yol_font_faces(d, size);
    if (faces < 0) return yol__ferr(err, cap, YOL_ERR_FORMAT, "not a TrueType or OpenType font");
    if (index < 0 || index >= faces) return yol__ferr(err, cap, YOL_ERR_RANGE, "face %d of %d", index, faces);
    if (memcmp(d, "ttcf", 4) == 0) base = yol__u32(d + 12 + 4 * (uint32_t)index);
    if (!yol__in(size, base, 12)) return yol__ferr(err, cap, YOL_ERR_FORMAT, "the table directory");
    f->data = d; f->size = size;
    nt = yol__u16(d + base + 4);
    if (!yol__in(size, base + 12, nt * 16)) return yol__ferr(err, cap, YOL_ERR_FORMAT, "the table directory");
    for (i = 0; i < nt; i++) {
        const uint8_t* r = d + base + 12 + 16 * i;
        uint32_t o = yol__u32(r + 8), l = yol__u32(r + 12);
        if (!yol__in(size, o, l)) return yol__ferr(err, cap, YOL_ERR_FORMAT, "table %.4s past the end", (const char*)r);
        if (!memcmp(r, "head", 4)) { head = o; head_n = l; }
        else if (!memcmp(r, "maxp", 4)) { maxp = o; maxp_n = l; }
        else if (!memcmp(r, "hhea", 4)) { hhea = o; hhea_n = l; }
        else if (!memcmp(r, "hmtx", 4)) { f->hmtx_ = o; f->hmtx_len_ = l; }
        else if (!memcmp(r, "loca", 4)) { f->loca_ = o; f->loca_len_ = l; }
        else if (!memcmp(r, "glyf", 4)) { f->glyf_ = o; f->glyf_len_ = l; }
        else if (!memcmp(r, "cmap", 4)) { cmap = o; cmap_n = l; }
        else if (!memcmp(r, "CFF ", 4)) { f->cff_ = o; f->cff_len_ = l; }
        else if (!memcmp(r, "CFF2", 4)) cff2 = 1;
        else if (!memcmp(r, "fvar", 4)) f->flags |= YOL_FONT_VARIABLE;
    }
    if (head_n < 54 || maxp_n < 6) return yol__ferr(err, cap, YOL_ERR_FORMAT, "no head or maxp table");
    f->units_per_em = (int32_t)yol__u16(d + head + 18);
    if (f->units_per_em < 16 || f->units_per_em > 16384) return yol__ferr(err, cap, YOL_ERR_FORMAT, "unitsPerEm %d", f->units_per_em);
    f->loca_long_ = yol__s16(d + head + 50) ? 1u : 0u;
    f->n_glyphs = (int32_t)yol__u16(d + maxp + 4);
    if (hhea_n >= 36) {
        f->ascender = yol__s16(d + hhea + 4); f->descender = yol__s16(d + hhea + 6); f->line_gap = yol__s16(d + hhea + 8);
        f->n_hmetrics_ = yol__u16(d + hhea + 34);
        if ((uint64_t)f->n_hmetrics_ * 4 > f->hmtx_len_) f->n_hmetrics_ = f->hmtx_len_ / 4;
    }
    /* how many glyphs hmtx gives a left side bearing, by HarfBuzz's rule
     * (hb-ot-hmtx-table.hh 14.6.0): the long metrics, then as many short
     * bearings as numGlyphs asks and the table holds; none without long
     * metrics */
    {
        uint32_t nlm = f->n_hmetrics_, len = f->hmtx_len_ - 4 * nlm, nb = (uint32_t)f->n_glyphs;
        if (nb < nlm) nb = nlm;
        if ((uint64_t)(nb - nlm) * 2 > len) nb = nlm + len / 2;
        if (nlm == 0) nb = 0;
        f->hmtx_nb_ = nb;
    }
    if (f->glyf_ && f->loca_) {
        if ((uint64_t)(f->n_glyphs + 1) * (f->loca_long_ ? 4u : 2u) > f->loca_len_) return yol__ferr(err, cap, YOL_ERR_FORMAT, "loca shorter than numGlyphs");
        f->flags |= YOL_FONT_GLYF;
    } else if (f->cff_) {
        int rc = yol__open_cff(f, err, cap);
        if (rc < 0) return rc;
        f->flags |= YOL_FONT_CFF;
    } else if (cff2) return yol__ferr(err, cap, YOL_ERR_REFUSED, "CFF2 outlines");
    else return yol__ferr(err, cap, YOL_ERR_REFUSED, "a font without glyf or CFF outlines (bitmap only)");
    if (cmap) yol__open_cmap(f, cmap, cmap_n);
    return YOL_OK;
}

YOL_API uint32_t yol_font_glyph_index(const yol_font* f, uint32_t cp) {
    const uint8_t* d;
    if (!f || !f->cmap_) return 0;
    d = f->data;
    if (f->cmap_fmt_ == 4) {
        uint32_t o = f->cmap_, seg2 = yol__u16(d + o + 6), lo = 0, hi = seg2 / 2, ends = o + 14, starts = o + 16 + seg2;
        if (cp > 0xffff) return 0;
        while (lo < hi) {   /* the first segment whose endCode >= cp */
            uint32_t mid = (lo + hi) / 2;
            if (yol__u16(d + ends + 2 * mid) < cp) lo = mid + 1; else hi = mid;
        }
        if (lo >= seg2 / 2) return 0;
        {
            uint32_t start = yol__u16(d + starts + 2 * lo), delta = yol__u16(d + starts + seg2 + 2 * lo);
            uint32_t rpos = starts + 2 * seg2 + 2 * lo, ro = yol__u16(d + rpos), g;
            if (cp < start) return 0;
            if (ro == 0) return (cp + delta) & 0xffffu;
            if (!yol__in(f->size, rpos + ro + 2 * (cp - start), 2)) return 0;
            g = yol__u16(d + rpos + ro + 2 * (cp - start));
            return g ? (g + delta) & 0xffffu : 0;
        }
    } else {
        uint32_t o = f->cmap_, ng = yol__u32(d + o + 12), lo = 0, hi = ng;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2, g = o + 16 + 12 * mid;
            uint32_t s = yol__u32(d + g), e = yol__u32(d + g + 4);
            if (cp < s) hi = mid; else if (cp > e) lo = mid + 1;
            else { uint32_t r = yol__u32(d + g + 8) + (cp - s); return r < (uint32_t)f->n_glyphs ? r : 0; }
        }
        return 0;
    }
}

YOL_API int yol_font_hmetrics(const yol_font* f, uint32_t glyph, double* advance_em, double* lsb_em) {
    uint32_t adv, n;
    int32_t lsb;
    if (!f || !f->data) return YOL_ERR_ARG;
    n = f->n_hmetrics_;
    if (glyph >= (uint32_t)f->n_glyphs || n == 0) return YOL_ERR_RANGE;
    if (glyph < n) { adv = yol__u16(f->data + f->hmtx_ + 4 * glyph); lsb = yol__s16(f->data + f->hmtx_ + 4 * glyph + 2); }
    else {
        uint32_t lo = 4 * n + 2 * (glyph - n);
        adv = yol__u16(f->data + f->hmtx_ + 4 * (n - 1));
        lsb = (lo + 2 <= f->hmtx_len_) ? yol__s16(f->data + f->hmtx_ + lo) : 0;
    }
    if (advance_em) *advance_em = (double)adv / f->units_per_em;
    if (lsb_em) *lsb_em = (double)lsb / f->units_per_em;
    return YOL_OK;
}

/* Glyph space to the caller's: a composite matrix m (font units), then the
 * em scale k, the pen and the y flip. */
typedef struct yol__gmap { double k, px, py, sy; int32_t pts0, pad; } yol__gmap;
static double yol__mx(const yol__gmap* g, const double m[6], double x, double y) { return g->px + g->k * (m[0] * x + m[2] * y + m[4]); }
/* py - v, not py + (-1) v: a 0 stays +0, so equal points keep equal bits */
static double yol__my(const yol__gmap* g, const double m[6], double x, double y) {
    double v = g->k * (m[1] * x + m[3] * y + m[5]);
    return g->sy > 0 ? g->py + v : g->py - v;
}

/* hmtx's left side bearing of a glyph, font units; 0 where hmtx has none
 * (HarfBuzz 14.6.0, hb-ot-hmtx-table.hh, get_leading_bearing_without_var_unscaled) */
static int32_t yol__lsb(const yol_font* f, uint32_t gid) {
    if (gid < f->n_hmetrics_) return yol__s16(f->data + f->hmtx_ + 4 * gid + 2);
    if (gid >= f->hmtx_nb_) return 0;
    return yol__s16(f->data + f->hmtx_ + 4 * f->n_hmetrics_ + 2 * (gid - f->n_hmetrics_));
}

/* The left phantom point's x of a glyf glyph, font units: its xMin minus
 * its hmtx left side bearing; for a composite, the phantom of the last
 * component flagged USE_MY_METRICS, untransformed, if any. HarfBuzz 14.6.0
 * (src/OT/glyf/Glyph.hh, Glyph::get_points) shifts the whole outline by
 * minus this at the top level, as a TrueType rasterizer places its phantom
 * points ("undocumented rasterizer behavior"); glyph positions come from
 * HarfBuzz, so outlines sit where it puts them. CFF has no phantom points.
 * Malformed data gives 0: the reader refuses it anyway. */
static int32_t yol__phantom_x(const yol_font* f, uint32_t gid, int depth) {
    const uint8_t* d = f->data;
    uint32_t a, b, o, end, q, fl;
    int32_t x;
    if (depth > YOL__COMPOSITE_DEPTH || gid >= (uint32_t)f->n_glyphs) return 0;
    if (f->loca_long_) { a = yol__u32(d + f->loca_ + 4 * gid); b = yol__u32(d + f->loca_ + 4 * gid + 4); }
    else { a = 2 * yol__u16(d + f->loca_ + 2 * gid); b = 2 * yol__u16(d + f->loca_ + 2 * gid + 2); }
    /* an empty glyph has a zero header: xMin 0 */
    if (a == b) return -yol__lsb(f, gid);
    if (b < a || b > f->glyf_len_ || b - a < 10) return 0;
    o = f->glyf_ + a; end = f->glyf_ + b;
    x = yol__s16(d + o + 2) - yol__lsb(f, gid);
    if (yol__s16(d + o) >= 0) return x;
    for (q = o + 10;;) {
        uint32_t cg;
        if (q + 4 > end) return 0;
        fl = yol__u16(d + q); cg = yol__u16(d + q + 2);
        q += 4 + ((fl & 1) ? 4u : 2u) + ((fl & 8) ? 2u : ((fl & 0x40) ? 4u : ((fl & 0x80) ? 8u : 0u)));
        if (fl & 0x200) x = yol__phantom_x(f, cg, depth + 1);
        if (!(fl & 0x20)) return x;
    }
}

static int yol__glyf(yol_ctx* c, const yol_font* f, uint32_t gid, const double m[6], yol_path* p,
                       const yol__gmap* g, int depth, int* visits) {
    const uint8_t* d = f->data;
    uint32_t a, b, o, end;
    int32_t nc;
    if (++*visits > YOL__COMPOSITE_VISITS) return yol__err(c, YOL_ERR_REFUSED, "glyph %u: more than %d composite components", gid, YOL__COMPOSITE_VISITS);
    if (gid >= (uint32_t)f->n_glyphs) return yol__err(c, YOL_ERR_FORMAT, "a component names glyph %u of %d", gid, f->n_glyphs);
    if (f->loca_long_) { a = yol__u32(d + f->loca_ + 4 * gid); b = yol__u32(d + f->loca_ + 4 * gid + 4); }
    else { a = 2 * yol__u16(d + f->loca_ + 2 * gid); b = 2 * yol__u16(d + f->loca_ + 2 * gid + 2); }
    if (a == b) return YOL_OK;
    if (b < a || b > f->glyf_len_ || b - a < 10) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: loca range %u..%u", gid, a, b);
    o = f->glyf_ + a; end = f->glyf_ + b;
    nc = yol__s16(d + o);
    if (nc >= 0) {
        uint32_t np, i, q, ilen, s;
        uint8_t* flags;
        int32_t *xs, *ys, x = 0, y = 0, k;
        if (nc == 0) return YOL_OK;
        if (o + 10 + 2 * (uint32_t)nc + 2 > end) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: contour table past the glyph", gid);
        for (k = 1; k < nc; k++) if (yol__u16(d + o + 10 + 2 * k) < yol__u16(d + o + 8 + 2 * k)) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: contour ends decrease", gid);
        np = yol__u16(d + o + 10 + 2 * (nc - 1)) + 1;
        if ((int64_t)(p->n_pts - g->pts0) + 2 * (int64_t)np + 2 * (int64_t)nc > YOL__GLYPH_POINTS)
            return yol__err(c, YOL_ERR_REFUSED, "glyph %u: more than %d outline points", gid, YOL__GLYPH_POINTS);
        ilen = yol__u16(d + o + 10 + 2 * nc);
        q = o + 12 + 2 * (uint32_t)nc + ilen;
        if (q > end) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: instructions past the glyph", gid);
        flags = (uint8_t*)yol__scratch(c, YOL__S_FONT, np * (1 + 2 * sizeof(int32_t)) + 16);
        if (!flags) return YOL_ERR_FULL;
        xs = (int32_t*)(void*)(flags + ((np + 7u) & ~7u));
        ys = xs + np;
        for (i = 0; i < np;) {
            uint32_t v, r;
            if (q >= end) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: flags past the glyph", gid);
            v = d[q++]; flags[i++] = (uint8_t)v;
            if (v & 8) {
                if (q >= end) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: flags past the glyph", gid);
                r = d[q++];
                while (r-- && i < np) flags[i++] = (uint8_t)v;
            }
        }
        for (i = 0; i < np; i++) {
            uint32_t v = flags[i];
            if (v & 2) { if (q + 1 > end) goto past; x += (v & 16) ? (int32_t)d[q] : -(int32_t)d[q]; q += 1; }
            else if (!(v & 16)) { if (q + 2 > end) goto past; x += yol__s16(d + q); q += 2; }
            xs[i] = x;
        }
        for (i = 0; i < np; i++) {
            uint32_t v = flags[i];
            if (v & 4) { if (q + 1 > end) goto past; y += (v & 32) ? (int32_t)d[q] : -(int32_t)d[q]; q += 1; }
            else if (!(v & 32)) { if (q + 2 > end) goto past; y += yol__s16(d + q); q += 2; }
            ys[i] = y;
        }
        for (k = 0, s = 0; k < nc; k++) {
            uint32_t e = yol__u16(d + o + 10 + 2 * k), n = e - s + 1, st, j;
            double sx, sy, px, py, cx = 0, cy = 0;
            int have = 0;
            if (e < s || n < 2) { s = e + 1; continue; }
            /* start at an on-curve point, or at the midpoint of two off-curve ones */
            for (st = 0; st < n && !(flags[s + st] & 1); st++) {}
            if (st == n) { sx = 0.5 * ((double)xs[s] + xs[s + 1]); sy = 0.5 * ((double)ys[s] + ys[s + 1]); st = 1; }
            else { sx = xs[s + st]; sy = ys[s + st]; st = st + 1; }
            yol_move(p, yol__mx(g, m, sx, sy), yol__my(g, m, sx, sy));
            px = sx; py = sy;
            for (j = 0; j < n; j++) {
                uint32_t idx = s + (st + j) % n;
                double qx = xs[idx], qy = ys[idx];
                if (flags[idx] & 1) {
                    if (have) yol_quad(p, yol__mx(g, m, cx, cy), yol__my(g, m, cx, cy), yol__mx(g, m, qx, qy), yol__my(g, m, qx, qy));
                    else if (qx != px || qy != py) yol_line(p, yol__mx(g, m, qx, qy), yol__my(g, m, qx, qy));
                    have = 0; px = qx; py = qy;
                } else {
                    if (have) {
                        double mx = 0.5 * (cx + qx), my = 0.5 * (cy + qy);
                        yol_quad(p, yol__mx(g, m, cx, cy), yol__my(g, m, cx, cy), yol__mx(g, m, mx, my), yol__my(g, m, mx, my));
                        px = mx; py = my;
                    }
                    cx = qx; cy = qy; have = 1;
                }
            }
            if (have) yol_quad(p, yol__mx(g, m, cx, cy), yol__my(g, m, cx, cy), yol__mx(g, m, sx, sy), yol__my(g, m, sx, sy));
            yol_close_path(p);
            if (p->status) return p->status;
            s = e + 1;
        }
        return YOL_OK;
past:
        return yol__err(c, YOL_ERR_FORMAT, "glyph %u: coordinates past the glyph", gid);
    } else {
        uint32_t q = o + 10, fl;
        if (depth >= YOL__COMPOSITE_DEPTH) return yol__err(c, YOL_ERR_REFUSED, "glyph %u: composite deeper than %d", gid, YOL__COMPOSITE_DEPTH);
        do {
            double t[6] = { 1, 0, 0, 1, 0, 0 }, mm[6];
            uint32_t cg;
            int rc;
            if (q + 4 > end) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: component past the glyph", gid);
            fl = yol__u16(d + q); cg = yol__u16(d + q + 2); q += 4;
            if (!(fl & 2)) return yol__err(c, YOL_ERR_REFUSED, "glyph %u: a point-matched composite component", gid);
            if (fl & 1) { if (q + 4 > end) goto cpast; t[4] = yol__s16(d + q); t[5] = yol__s16(d + q + 2); q += 4; }
            else { if (q + 2 > end) goto cpast; t[4] = (int8_t)d[q]; t[5] = (int8_t)d[q + 1]; q += 2; }
            if (fl & 8) { if (q + 2 > end) goto cpast; t[0] = t[3] = yol__s16(d + q) / 16384.0; q += 2; }
            else if (fl & 0x40) { if (q + 4 > end) goto cpast; t[0] = yol__s16(d + q) / 16384.0; t[3] = yol__s16(d + q + 2) / 16384.0; q += 4; }
            else if (fl & 0x80) {
                if (q + 8 > end) goto cpast;
                t[0] = yol__s16(d + q) / 16384.0; t[1] = yol__s16(d + q + 2) / 16384.0;
                t[2] = yol__s16(d + q + 4) / 16384.0; t[3] = yol__s16(d + q + 6) / 16384.0; q += 8;
            }
            if ((fl & 0x800) && !(fl & 0x1000)) {   /* Apple's scaled offset */
                double ox = t[4], oy = t[5];
                t[4] = t[0] * ox + t[2] * oy; t[5] = t[1] * ox + t[3] * oy;
            }
            mm[0] = m[0] * t[0] + m[2] * t[1]; mm[1] = m[1] * t[0] + m[3] * t[1];
            mm[2] = m[0] * t[2] + m[2] * t[3]; mm[3] = m[1] * t[2] + m[3] * t[3];
            mm[4] = m[0] * t[4] + m[2] * t[5] + m[4]; mm[5] = m[1] * t[4] + m[3] * t[5] + m[5];
            rc = yol__glyf(c, f, cg, mm, p, g, depth + 1, visits);
            if (rc < 0) return rc;
        } while (fl & 0x20);
        return YOL_OK;
cpast:
        return yol__err(c, YOL_ERR_FORMAT, "glyph %u: component past the glyph", gid);
    }
}

/* --- CFF charstrings ---------------------------------------------------------- */

typedef struct yol__cs {
    yol_ctx* c; const yol_font* f; yol_path* p; const yol__gmap* g;
    double st[YOL__CFF_STACK]; int n;
    double x, y;
    int open, nstems, width_done, tokens;
    uint32_t gid;
} yol__cs;

static const double yol__id6[6] = { 1, 0, 0, 1, 0, 0 };

static void yol__cs_move(yol__cs* s, double dx, double dy) {
    if (s->open) yol_close_path(s->p);
    s->x += dx; s->y += dy;
    yol_move(s->p, yol__mx(s->g, yol__id6, s->x, s->y), yol__my(s->g, yol__id6, s->x, s->y));
    s->open = 1;
}
static void yol__cs_line(yol__cs* s, double dx, double dy) {
    if (!s->open) yol__cs_move(s, 0, 0);
    s->x += dx; s->y += dy;
    yol_line(s->p, yol__mx(s->g, yol__id6, s->x, s->y), yol__my(s->g, yol__id6, s->x, s->y));
}
static void yol__cs_curve(yol__cs* s, double d1x, double d1y, double d2x, double d2y, double d3x, double d3y) {
    double x1, y1, x2, y2;
    if (!s->open) yol__cs_move(s, 0, 0);
    x1 = s->x + d1x; y1 = s->y + d1y; x2 = x1 + d2x; y2 = y1 + d2y;
    s->x = x2 + d3x; s->y = y2 + d3y;
    yol_cubic(s->p, yol__mx(s->g, yol__id6, x1, y1), yol__my(s->g, yol__id6, x1, y1),
                yol__mx(s->g, yol__id6, x2, y2), yol__my(s->g, yol__id6, x2, y2),
                yol__mx(s->g, yol__id6, s->x, s->y), yol__my(s->g, yol__id6, s->x, s->y));
}

static uint32_t yol__bias(uint32_t count) { return count < 1240 ? 107u : (count < 33900 ? 1131u : 32768u); }

/* The first stack-clearing operator may carry the advance width first. */
static void yol__cs_width(yol__cs* s, int extra) {
    if (!s->width_done && extra && s->n > 0) { memmove(s->st, s->st + 1, (size_t)(s->n - 1) * sizeof(double)); s->n--; }
    s->width_done = 1;
}

static int yol__cs_run(yol__cs* s, uint32_t lsubr) {
    const yol_font* f = s->f;
    const uint8_t* d = f->data;
    struct { uint32_t pos, end; } stack[YOL__CFF_CALLS + 1];
    int depth = 0, i;
    yol__idx cs, gs, ls;
    uint32_t start, len;
    yol_ctx* c = s->c;
    if (yol__index(f, f->cs_, &cs) < 0 || yol__index_item(f, &cs, s->gid, &start, &len) < 0)
        return yol__err(c, YOL_ERR_FORMAT, "glyph %u: the CharStrings entry", s->gid);
    if (yol__index(f, f->gsubr_, &gs) < 0) return yol__err(c, YOL_ERR_FORMAT, "CFF: the Global Subr INDEX");
    memset(&ls, 0, sizeof ls);
    if (lsubr && yol__index(f, lsubr, &ls) < 0) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: the local Subrs INDEX", s->gid);
    stack[0].pos = start; stack[0].end = start + len;
    for (;;) {
        uint32_t pos = stack[depth].pos, end = stack[depth].end, b0;
        if (pos >= end) {
            /* a subroutine may end without return; the charstring may not */
            if (depth == 0) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: charstring without endchar", s->gid);
            depth--; continue;
        }
        if (++s->tokens > YOL__CFF_TOKENS) return yol__err(c, YOL_ERR_REFUSED, "glyph %u: more than %d charstring tokens", s->gid, YOL__CFF_TOKENS);
        b0 = d[pos];
        if (b0 >= 32 || b0 == 28) {
            double v;
            if (b0 == 28) { if (pos + 3 > end) goto past; v = yol__s16(d + pos + 1); pos += 3; }
            else if (b0 <= 246) { v = (double)b0 - 139; pos += 1; }
            else if (b0 <= 250) { if (pos + 2 > end) goto past; v = ((double)b0 - 247) * 256 + d[pos + 1] + 108; pos += 2; }
            else if (b0 <= 254) { if (pos + 2 > end) goto past; v = -((double)b0 - 251) * 256 - d[pos + 1] - 108; pos += 2; }
            else { if (pos + 5 > end) goto past; v = (double)(int32_t)yol__u32(d + pos + 1) / 65536.0; pos += 5; }
            if (s->n >= YOL__CFF_STACK) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: charstring stack overflow", s->gid);
            s->st[s->n++] = v;
            stack[depth].pos = pos;
            continue;
        }
        pos += 1;
        stack[depth].pos = pos;
        switch (b0) {
        case 1: case 3: case 18: case 23:          /* stems: count them, for hintmask */
            yol__cs_width(s, s->n & 1);
            s->nstems += s->n / 2;
            if (s->nstems > YOL__CFF_STEMS) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: more than %d stems", s->gid, YOL__CFF_STEMS);
            s->n = 0; break;
        case 19: case 20: {                          /* hintmask, cntrmask */
            uint32_t bytes;
            yol__cs_width(s, s->n & 1);
            s->nstems += s->n / 2;
            if (s->nstems > YOL__CFF_STEMS) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: more than %d stems", s->gid, YOL__CFF_STEMS);
            s->n = 0;
            bytes = (uint32_t)(s->nstems + 7) / 8;
            if (pos + bytes > end) goto past;
            stack[depth].pos = pos + bytes;
            break;
        }
        case 21: yol__cs_width(s, s->n > 2); if (s->n < 2) goto few; yol__cs_move(s, s->st[0], s->st[1]); s->n = 0; break;
        case 22: yol__cs_width(s, s->n > 1); if (s->n < 1) goto few; yol__cs_move(s, s->st[0], 0); s->n = 0; break;
        case 4:  yol__cs_width(s, s->n > 1); if (s->n < 1) goto few; yol__cs_move(s, 0, s->st[0]); s->n = 0; break;
        case 5: for (i = 0; i + 2 <= s->n; i += 2) yol__cs_line(s, s->st[i], s->st[i + 1]); s->n = 0; break;
        case 6: case 7: {
            int h = b0 == 6;
            for (i = 0; i < s->n; i++, h = !h) yol__cs_line(s, h ? s->st[i] : 0, h ? 0 : s->st[i]);
            s->n = 0; break;
        }
        case 8: for (i = 0; i + 6 <= s->n; i += 6) yol__cs_curve(s, s->st[i], s->st[i + 1], s->st[i + 2], s->st[i + 3], s->st[i + 4], s->st[i + 5]); s->n = 0; break;
        case 27: {   /* hhcurveto */
            double dy1 = 0;
            i = 0;
            if (s->n & 1) { dy1 = s->st[0]; i = 1; }
            for (; i + 4 <= s->n; i += 4) { yol__cs_curve(s, s->st[i], dy1, s->st[i + 1], s->st[i + 2], s->st[i + 3], 0); dy1 = 0; }
            s->n = 0; break;
        }
        case 26: {   /* vvcurveto */
            double dx1 = 0;
            i = 0;
            if (s->n & 1) { dx1 = s->st[0]; i = 1; }
            for (; i + 4 <= s->n; i += 4) { yol__cs_curve(s, dx1, s->st[i], s->st[i + 1], s->st[i + 2], 0, s->st[i + 3]); dx1 = 0; }
            s->n = 0; break;
        }
        case 30: case 31: {   /* vhcurveto, hvcurveto */
            int h = b0 == 31;
            for (i = 0; i + 4 <= s->n; h = !h) {
                int last = (s->n - i == 5);
                double e = last ? s->st[i + 4] : 0;
                if (h) yol__cs_curve(s, s->st[i], 0, s->st[i + 1], s->st[i + 2], e, s->st[i + 3]);
                else   yol__cs_curve(s, 0, s->st[i], s->st[i + 1], s->st[i + 2], s->st[i + 3], e);
                i += 4 + last;
            }
            s->n = 0; break;
        }
        case 24:   /* rcurveline */
            for (i = 0; i + 8 <= s->n; i += 6) yol__cs_curve(s, s->st[i], s->st[i + 1], s->st[i + 2], s->st[i + 3], s->st[i + 4], s->st[i + 5]);
            if (i + 2 <= s->n) yol__cs_line(s, s->st[i], s->st[i + 1]);
            s->n = 0; break;
        case 25:   /* rlinecurve */
            for (i = 0; i + 8 <= s->n; i += 2) yol__cs_line(s, s->st[i], s->st[i + 1]);
            if (i + 6 <= s->n) yol__cs_curve(s, s->st[i], s->st[i + 1], s->st[i + 2], s->st[i + 3], s->st[i + 4], s->st[i + 5]);
            s->n = 0; break;
        case 10: case 29: {   /* callsubr, callgsubr */
            const yol__idx* ix = b0 == 10 ? &ls : &gs;
            double v;
            int64_t k;
            if (s->n < 1) goto few;
            if (b0 == 10 && !lsubr) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: callsubr without local Subrs", s->gid);
            v = s->st[--s->n];
            k = (int64_t)v + yol__bias(ix->count);
            if (v != floor(v) || k < 0 || k >= (int64_t)ix->count) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: subroutine %g out of range", s->gid, v);
            if (depth >= YOL__CFF_CALLS) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: subroutines nested deeper than %d", s->gid, YOL__CFF_CALLS);
            if (yol__index_item(f, ix, (uint32_t)k, &start, &len) < 0) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: a subroutine past its INDEX", s->gid);
            depth++;
            stack[depth].pos = start; stack[depth].end = start + len;
            break;
        }
        case 11:
            if (depth == 0) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: return outside a subroutine", s->gid);
            depth--; break;
        case 14:
            if (!s->width_done && (s->n == 1 || s->n == 5)) yol__cs_width(s, 1);
            if (s->n >= 4) return yol__err(c, YOL_ERR_REFUSED, "glyph %u: a seac accent (endchar with 4 arguments)", s->gid);
            if (s->open) yol_close_path(s->p);
            s->open = 0;
            return s->p->status;
        case 12: {
            uint32_t b1;
            double* a = s->st;
            if (pos >= end) goto past;
            b1 = d[pos];
            stack[depth].pos = pos + 1;
            if (b1 == 35) {          /* flex */
                if (s->n < 13) goto few;
                yol__cs_curve(s, a[0], a[1], a[2], a[3], a[4], a[5]);
                yol__cs_curve(s, a[6], a[7], a[8], a[9], a[10], a[11]);
            } else if (b1 == 34) {   /* hflex */
                if (s->n < 7) goto few;
                yol__cs_curve(s, a[0], 0, a[1], a[2], a[3], 0);
                yol__cs_curve(s, a[4], 0, a[5], -a[2], a[6], 0);
            } else if (b1 == 36) {   /* hflex1 */
                if (s->n < 9) goto few;
                yol__cs_curve(s, a[0], a[1], a[2], a[3], a[4], 0);
                yol__cs_curve(s, a[5], 0, a[6], a[7], a[8], -(a[1] + a[3] + a[7]));
            } else if (b1 == 37) {   /* flex1 */
                double dx = a[0] + a[2] + a[4] + a[6] + a[8], dy = a[1] + a[3] + a[5] + a[7] + a[9];
                if (s->n < 11) goto few;
                yol__cs_curve(s, a[0], a[1], a[2], a[3], a[4], a[5]);
                if (fabs(dx) > fabs(dy)) yol__cs_curve(s, a[6], a[7], a[8], a[9], a[10], -dy);
                else yol__cs_curve(s, a[6], a[7], a[8], a[9], -dx, a[10]);
            } else if (b1 == 0) {    /* dotsection, deprecated: nothing */
            } else return yol__err(c, YOL_ERR_REFUSED, "glyph %u: Type 2 arithmetic operator 12 %u", s->gid, (unsigned)b1);
            s->n = 0; break;
        }
        default:
            return yol__err(c, YOL_ERR_FORMAT, "glyph %u: charstring operator %u", s->gid, (unsigned)b0);
        }
        if (s->p->status) return s->p->status;
        if (s->p->n_pts - s->g->pts0 > YOL__GLYPH_POINTS) return yol__err(c, YOL_ERR_REFUSED, "glyph %u: more than %d outline points", s->gid, YOL__GLYPH_POINTS);
    }
past:
    return yol__err(c, YOL_ERR_FORMAT, "glyph %u: a charstring number past its end", s->gid);
few:
    return yol__err(c, YOL_ERR_FORMAT, "glyph %u: an operator without its arguments", s->gid);
}

static int yol__cff_glyph(yol_ctx* c, const yol_font* f, uint32_t gid, yol_path* p, const yol__gmap* g) {
    yol__cs s;
    uint32_t lsubr = f->lsubr_;
    if (f->flags & YOL_FONT_CID) {
        const uint8_t* d = f->data;
        uint32_t fs = f->fdselect_, fd = 0, ds, dl;
        yol__idx fa;
        double v[2];
        int n;
        if (d[fs] == 0) {
            if (!yol__in(f->size, fs + 1 + gid, 1)) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: FDSelect past the end", gid);
            fd = d[fs + 1 + gid];
        } else {
            uint32_t nr, r, found = 0;
            if (!yol__in(f->size, fs + 1, 2)) return yol__err(c, YOL_ERR_FORMAT, "CFF: FDSelect");
            nr = yol__u16(d + fs + 1);
            if (!yol__in(f->size, fs + 3, nr * 3 + 2)) return yol__err(c, YOL_ERR_FORMAT, "CFF: FDSelect ranges");
            for (r = 0; r < nr; r++) {
                uint32_t first = yol__u16(d + fs + 3 + 3 * r), next = yol__u16(d + fs + 3 + 3 * r + 3);
                if (gid >= first && gid < next) { fd = d[fs + 3 + 3 * r + 2]; found = 1; break; }
            }
            if (!found) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: not in FDSelect", gid);
        }
        if (yol__index(f, f->fdarray_, &fa) < 0 || yol__index_item(f, &fa, fd, &ds, &dl) < 0)
            return yol__err(c, YOL_ERR_FORMAT, "glyph %u: Font DICT %u", gid, fd);
        n = yol__dict_get(f, ds, dl, 18, v, 2);
        if (n < 0) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: Font DICT %u", gid, fd);
        lsubr = 0;
        if (n >= 2 && yol__private_subrs(f, v[0], v[1], &lsubr) < 0) return yol__err(c, YOL_ERR_FORMAT, "glyph %u: its Private DICT", gid);
    }
    memset(&s, 0, sizeof s);
    s.c = c; s.f = f; s.p = p; s.g = g; s.gid = gid;
    return yol__cs_run(&s, lsubr);
}

static int yol__font_glyph(yol_ctx* c, const yol_font* f, uint32_t glyph, yol_path* p, const yol_glyph_desc* d) {
    yol_glyph_desc dd;
    yol__gmap g;
    double tol0;
    int rc, visits = 0;
    if (!c || !f || !f->data || !p) return YOL_ERR_ARG;
    if (d) dd = *d; else memset(&dd, 0, sizeof dd);
    if (dd.n_axes > 0) return yol__err(c, YOL_ERR_REFUSED, "font variation instances (gvar, CFF2): v0.1 builds the default instance");
    if (glyph >= (uint32_t)f->n_glyphs) return yol__err(c, YOL_ERR_RANGE, "glyph %u of %d", glyph, f->n_glyphs);
    if (!yol__finite(dd.size) || !yol__finite(dd.x) || !yol__finite(dd.y) || !yol__finite(dd.tol) || dd.size < 0 || dd.tol < 0)
        return yol__err(c, YOL_ERR_ARG, "a glyph desc value is not finite or negative");
    if (p->status) return p->status;
    yol__end_contour(p);
    g.k = (dd.size > 0 ? dd.size : 1.0) / f->units_per_em;
    g.px = dd.x; g.py = dd.y; g.sy = dd.y_up ? 1.0 : -1.0; g.pts0 = p->n_pts; g.pad = 0;
    tol0 = p->tol;
    p->tol = (dd.tol > 0 ? dd.tol : 1e-4) * (dd.size > 0 ? dd.size : 1.0) * yol__xf_smax(p->xf);
    if (f->flags & YOL_FONT_GLYF) {
        /* HarfBuzz's placement (FONTS, "Placement") */
        double m0[6] = { 1, 0, 0, 1, 0, 0 };
        m0[4] = (double)-yol__phantom_x(f, glyph, 0);
        rc = yol__glyf(c, f, glyph, m0, p, &g, 0, &visits);
    }
    else rc = yol__cff_glyph(c, f, glyph, p, &g);
    p->tol = tol0;
    yol__end_contour(p);
    if (rc >= 0 && p->status) rc = p->status;
    p->status = 0;
    return rc < 0 ? rc : YOL_OK;
}

/* --- sorting (heapsort: no allocation, deterministic ties) ------------------ */

typedef struct yol__kv { double k; int32_t v; int32_t pad; } yol__kv;

static int yol__kv_lt(const yol__kv* a, const yol__kv* b) { return a->k < b->k || (a->k == b->k && a->v < b->v); }

static void yol__kv_sift(yol__kv* a, size_t i, size_t n) {
    yol__kv t = a[i];
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && yol__kv_lt(&a[c], &a[c + 1])) c++;
        if (!yol__kv_lt(&t, &a[c])) break;
        a[i] = a[c]; i = c;
    }
    a[i] = t;
}
static void yol__kv_sort(yol__kv* a, size_t n) {
    size_t i;
    if (n < 2) return;
    for (i = n / 2; i-- > 0;) yol__kv_sift(a, i, n);
    for (i = n - 1; i > 0; i--) { yol__kv t = a[0]; a[0] = a[i]; a[i] = t; yol__kv_sift(a, 0, i); }
}

/* --- pieces ------------------------------------------------------------------- */

/* A piece: monotone in x and y, running toward +y (y0 < y2). dir is the
 * original direction in y; set is the operand (0 or 1); line marks a piece
 * of a straight segment, so its sub-runs keep control == start. */
typedef struct yol__pc {
    double x0, y0, x1, y1, x2, y2;
    int8_t dir; uint8_t set, line, pad0;
    float xt;                   /* at most how far x moves for eps_y of y      */
} yol__pc;

/* Per piece sweep state: the position at the last strip line, and the open
 * output run. */
typedef struct yol__ps {
    double y, t, x;            /* where the piece was at the last line          */
    double rt, rx, ry;         /* the open run's start                          */
    int32_t rdir, pad;
} yol__ps;

typedef struct yol__act {
    int32_t i, d0, d1, lead;
    double ta, tb, xa, xb, xm;
} yol__act;

typedef struct yol__seg { double x0, y0, cx, cy, x1, y1; int32_t line, pad; } yol__seg;

typedef struct yol__sw {
    yol_ctx* c;
    yol__pc* pc; int32_t npc, cappc;
    double ext, eps_y, eps_x;
} yol__sw;

static double yol__qx(const yol__pc* q, double t) { double u = 1 - t; return u * u * q->x0 + 2 * t * u * q->x1 + t * t * q->x2; }
static double yol__qy(const yol__pc* q, double t) { double u = 1 - t; return u * u * q->y0 + 2 * t * u * q->y1 + t * t * q->y2; }

/* t in [0, 1] where a monotone quadratic coordinate (a, b, c) equals v. */
static double yol__solve_mono(double a, double b, double c, double v) {
    double A = a - 2 * b + c, B = 2 * (b - a), C = a - v, t;
    if ((a <= c && v <= a) || (a > c && v >= a)) return 0;
    if ((a <= c && v >= c) || (a > c && v <= c)) return 1;
    if (fabs(A) <= 1e-14 * fabs(B)) t = -C / B;
    else {
        double disc = B * B - 4 * A * C, s, q;
        if (disc < 0) disc = 0;
        s = sqrt(disc);
        q = -0.5 * (B + (B >= 0 ? s : -s));
        t = (q != 0) ? C / q : -B / (2 * A);
        if (!(t >= 0 && t <= 1)) t = q / A;
    }
    return t < 0 ? 0 : (t > 1 ? 1 : t);
}

/* t and x where the piece is at y (y0 <= y <= y2). A line's x is the
 * linear interpolation, no root; its t is only a fraction, which the run
 * output does not use (a line keeps control == start). */
static void yol__pos(const yol__pc* q, double y, double* t, double* x) {
    if (y <= q->y0) { *t = 0; *x = q->x0; return; }
    if (y >= q->y2) { *t = 1; *x = q->x2; return; }
    if (q->line) { *t = (y - q->y0) / (q->y2 - q->y0); *x = q->x0 + (q->x2 - q->x0) * *t; return; }
    *t = yol__solve_mono(q->y0, q->y1, q->y2, y);
    *x = yol__qx(q, *t);
}

static int yol__add_piece(yol__sw* w, const double* q, int8_t dir, int set, int line) {
    yol__pc* p;
    if (w->npc == w->cappc) {
        size_t cap = (size_t)w->cappc * sizeof(yol__pc);
        void* v = w->c->scratch[YOL__S_PIECE];
        cap = w->c->scratch_cap[YOL__S_PIECE];
        if (yol__grow(w->c, &v, &cap, ((size_t)w->npc + 64) * sizeof(yol__pc)) < 0) return YOL_ERR_FULL;
        w->c->scratch[YOL__S_PIECE] = v; w->c->scratch_cap[YOL__S_PIECE] = cap;
        w->pc = (yol__pc*)v; w->cappc = (int32_t)(cap / sizeof(yol__pc));
    }
    p = &w->pc[w->npc++];
    if (dir > 0) { p->x0 = q[0]; p->y0 = q[1]; p->x1 = q[2]; p->y1 = q[3]; p->x2 = q[4]; p->y2 = q[5]; }
    else { p->x0 = q[4]; p->y0 = q[5]; p->x1 = q[2]; p->y1 = q[3]; p->x2 = q[0]; p->y2 = q[1]; }
    p->dir = dir; p->set = (uint8_t)set; p->line = (uint8_t)line; p->pad0 = 0; p->xt = 0;
    return YOL_OK;
}

static double yol__ext_t(double a, double b, double c) {
    double d = a - 2 * b + c, t;
    if (d == 0) return -1;
    t = (a - b) / d;
    return (t > 0 && t < 1) ? t : -1;
}

/* One segment into pieces: split at its x and y extrema. A control point
 * an ulp outside its piece's height is clamped after the end heights merge.
 * Horizontal pieces change no winding: dropped. */
static int yol__seg_pieces(yol__sw* w, const double* s, int set, int line) {
    double ts[2], cur[6], done = 0, tx, ty;
    int n = 0, i, rc;
    if (line) {
        if (s[1] == s[5]) return YOL_OK;
        cur[0] = s[0]; cur[1] = s[1]; cur[2] = s[0]; cur[3] = s[1]; cur[4] = s[4]; cur[5] = s[5];
        return yol__add_piece(w, cur, (int8_t)(s[5] > s[1] ? 1 : -1), set, 1);
    }
    tx = yol__ext_t(s[0], s[2], s[4]); ty = yol__ext_t(s[1], s[3], s[5]);
    if (tx > 0) ts[n++] = tx;
    if (ty > 0) { if (n && ty < ts[0]) { ts[1] = ts[0]; ts[0] = ty; n++; } else if (!n || ty != ts[0]) ts[n++] = ty; }
    memcpy(cur, s, sizeof cur);
    for (i = 0; i <= n; i++) {
        double a[6], b[6];
        if (i < n) {
            double t = (ts[i] - done) / (1 - done);
            double mx0 = cur[0] + t * (cur[2] - cur[0]), my0 = cur[1] + t * (cur[3] - cur[1]);
            double mx1 = cur[2] + t * (cur[4] - cur[2]), my1 = cur[3] + t * (cur[5] - cur[3]);
            double px = mx0 + t * (mx1 - mx0), py = my0 + t * (my1 - my0);
            a[0] = cur[0]; a[1] = cur[1]; a[2] = mx0; a[3] = my0; a[4] = px; a[5] = py;
            b[0] = px; b[1] = py; b[2] = mx1; b[3] = my1; b[4] = cur[4]; b[5] = cur[5];
            done = ts[i];
        } else memcpy(a, cur, sizeof a);
        if (a[1] != a[5]) {
            rc = yol__add_piece(w, a, (int8_t)(a[5] > a[1] ? 1 : -1), set, 0);
            if (rc < 0) return rc;
        }
        if (i < n) memcpy(cur, b, sizeof cur);
    }
    return YOL_OK;
}

/* A stroke's private path of loose edges: a balanced edge set, not
 * contours, so nothing closes them. */
#define YOL__PATH_EDGES 0x80000000u

/* Every segment of p (open contours closed by a line) into pieces of set. */
static int yol__path_pieces(yol__sw* w, const yol_path* p, int set) {
    int32_t k, i;
    for (k = 0; k < p->n_contours; k++) {
        int32_t f = yol__cfirst(p, k), e = yol__cend(p, k);
        const double* q = p->pts;
        if (e - f < 3) continue;
        for (i = f; i + 2 < e; i += 2) {
            const double* s = q + 2 * i;
            int line = s[2] == s[0] && s[3] == s[1];
            int rc = yol__seg_pieces(w, s, set, line);
            if (rc < 0) return rc;
        }
        if ((q[2 * (e - 1)] != q[2 * f] || q[2 * (e - 1) + 1] != q[2 * f + 1]) && !(p->flags & YOL__PATH_EDGES)) {
            double s[6];
            int rc;
            s[0] = q[2 * (e - 1)]; s[1] = q[2 * (e - 1) + 1]; s[2] = s[0]; s[3] = s[1]; s[4] = q[2 * f]; s[5] = q[2 * f + 1];
            rc = yol__seg_pieces(w, s, set, 1);
            if (rc < 0) return rc;
        }
    }
    return YOL_OK;
}

/* --- crossings ----------------------------------------------------------------- */

/* Bernstein degree-4 coefficients of Q's implicit form along the piece P
 * (control points X): v^2 - 4 u w in barycentric areas of Q's control
 * triangle, or the signed distance to Q's chord over its length when Q is
 * flat. Both are dimensionless. 0 when Q is a point. */
static int yol__implicit(const yol__pc* Q, const double X[6], double c[5]) {
    double ax = Q->x0, ay = Q->y0, bx = Q->x1, by = Q->y1, cx = Q->x2, cy = Q->y2;
    double area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    double L2 = (cx - ax) * (cx - ax) + (cy - ay) * (cy - ay);
    int k;
    if (L2 == 0) return 0;
    if (fabs(area) <= 1e-12 * L2) {
        double l[3];
        for (k = 0; k < 3; k++) l[k] = ((cx - ax) * (X[2 * k + 1] - ay) - (cy - ay) * (X[2 * k] - ax)) / L2;
        c[0] = l[0]; c[1] = 0.5 * (l[0] + l[1]); c[2] = (l[0] + 4 * l[1] + l[2]) / 6.0; c[3] = 0.5 * (l[1] + l[2]); c[4] = l[2];
    } else {
        double U[3], V[3], W[3], vv[5], uw[5], inv = 1.0 / area;
        for (k = 0; k < 3; k++) {
            double x = X[2 * k], y = X[2 * k + 1];
            U[k] = ((bx - x) * (cy - y) - (by - y) * (cx - x)) * inv;
            V[k] = ((x - ax) * (cy - ay) - (y - ay) * (cx - ax)) * inv;
            W[k] = ((bx - ax) * (y - ay) - (by - ay) * (x - ax)) * inv;
        }
#define YOL__PROD(A, B, o) do { o[0] = A[0]*B[0]; o[1] = 0.5*(A[0]*B[1] + A[1]*B[0]); \
        o[2] = (A[0]*B[2] + 4*A[1]*B[1] + A[2]*B[0]) / 6.0; o[3] = 0.5*(A[1]*B[2] + A[2]*B[1]); o[4] = A[2]*B[2]; } while (0)
        YOL__PROD(V, V, vv); YOL__PROD(U, W, uw);
#undef YOL__PROD
        for (k = 0; k < 5; k++) c[k] = vv[k] - 4 * uw[k];
    }
    return 1;
}

static double yol__bern4(const double c[5], double s) {
    double b[5];
    int i, j;
    memcpy(b, c, sizeof b);
    for (j = 1; j < 5; j++) for (i = 0; i < 5 - j; i++) b[i] += s * (b[i + 1] - b[i]);
    return b[0];
}

/* Sign changes of a Bernstein quartic in (0, 1), ascending, by Descartes'
 * rule with subdivision. The children's sign variations never add up to
 * more than the parent's, so at most 4 intervals live per level, and depth
 * is bounded: no runaway. A tangency (no sign change) is not a crossing. */
static int yol__roots4(const double c[5], double s0, double s1, double tol, double* out, int nout, int depth) {
    int i, ch = 0, last = 0;
    double m = 0;
    if (nout <= 0) return 0;
    for (i = 0; i < 5; i++) m = fmax(m, fabs(c[i]));
    if (m <= tol) return 0;
    for (i = 0; i < 5; i++) {
        int sg = c[i] > tol ? 1 : (c[i] < -tol ? -1 : 0);
        if (sg) { if (last && sg != last) ch++; last = sg; }
    }
    if (ch == 0) return 0;
    if (ch == 1 && c[0] * c[4] < 0) {
        /* one root: the Illinois variant of regula falsi, bounded */
        double a = 0, b = 1, fa = c[0], fb = c[4], r = 0.5;
        int it, side = 0;
        for (it = 0; it < 100 && b - a > 4.5e-16 * b; it++) {
            double fm;
            r = (a * fb - b * fa) / (fb - fa);
            if (!(r > a && r < b)) r = 0.5 * (a + b);
            fm = yol__bern4(c, r);
            if (fm == 0) break;
            if ((fm < 0) == (fa < 0)) { a = r; fa = fm; if (side < 0) fb *= 0.5; side = -1; }
            else { b = r; fb = fm; if (side > 0) fa *= 0.5; side = 1; }
        }
        out[0] = s0 + r * (s1 - s0);
        return 1;
    }
    if (depth >= 48) return 0;
    {
        double L[5], R[5], b[5];
        int j, n1;
        memcpy(b, c, sizeof b);
        L[0] = b[0]; R[4] = b[4];
        for (j = 1; j < 5; j++) { for (i = 0; i < 5 - j; i++) b[i] = 0.5 * (b[i] + b[i + 1]); L[j] = b[0]; R[4 - j] = b[4 - j]; }
        n1 = yol__roots4(L, s0, 0.5 * (s0 + s1), tol, out, nout, depth + 1);
        return n1 + yol__roots4(R, 0.5 * (s0 + s1), s1, tol, out + n1, nout - n1, depth + 1);
    }
}

/* Every crossing y of pieces P and Q strictly inside both y ranges. */
static int yol__crossings(const yol__sw* w, const yol__pc* P, const yol__pc* Q, double* ys) {
    double X[6], c[5], r[8], m = 0, ylo, yhi;
    int n, i, k = 0;
    /* both pieces are monotone: over their common y range each spans the x
     * between its two ends there, and two lines cross where their x
     * difference, linear in y, changes sign */
    {
        double t, pa, pb, qa, qb;
        ylo = fmax(P->y0, Q->y0); yhi = fmin(P->y2, Q->y2);
        if (!(yhi - ylo > 2 * w->eps_y)) return 0;
        yol__pos(P, ylo, &t, &pa); yol__pos(P, yhi, &t, &pb);
        yol__pos(Q, ylo, &t, &qa); yol__pos(Q, yhi, &t, &qb);
        if (fmax(pa, pb) < fmin(qa, qb) - w->eps_x || fmax(qa, qb) < fmin(pa, pb) - w->eps_x) return 0;
        if (P->line && Q->line) {
            double da = pa - qa, db = pb - qb, y;
            if (da == 0 || db == 0 || (da > 0) == (db > 0)) return 0;
            y = ylo + (yhi - ylo) * (da / (da - db));
            if (!(y > ylo + w->eps_y && y < yhi - w->eps_y)) return 0;
            ys[0] = y;
            return 1;
        }
    }
    X[0] = P->x0; X[1] = P->y0; X[2] = P->x1; X[3] = P->y1; X[4] = P->x2; X[5] = P->y2;
    if (!yol__implicit(Q, X, c)) return 0;
    for (i = 0; i < 5; i++) m = fmax(m, fabs(c[i]));
    n = yol__roots4(c, 0, 1, fmax(1e-13, 1e-13 * m), r, 8, 0);
    ylo = fmax(P->y0, Q->y0) + w->eps_y; yhi = fmin(P->y2, Q->y2) - w->eps_y;
    for (i = 0; i < n && i < 8; i++) {
        double y = yol__qy(P, r[i]), x = yol__qx(P, r[i]), u;
        if (!(y > ylo && y < yhi)) continue;
        u = yol__solve_mono(Q->y0, Q->y1, Q->y2, y);
        if (fabs(yol__qx(Q, u) - x) > 1e-7 * w->ext) continue;   /* on the conic, off the arc */
        ys[k++] = y;
    }
    return k;
}

/* --- pair set (each adjacent pair's crossings are computed once) -------------- */

static uint64_t yol__hash64(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33; return x; }

/* 1 if the pair was new (and is now in the set), 0 if seen, <0 on error. */
static int yol__pair_add(yol_ctx* c, uint64_t** tab, size_t* cap, size_t* count, uint32_t i, uint32_t j) {
    uint64_t key = ((uint64_t)(i < j ? i : j) << 32 | (i < j ? j : i)) + 1, h;
    size_t mask, k;
    if ((*count + 1) * 2 > *cap) {
        size_t ncap = *cap ? *cap * 2 : 1024, n;
        uint64_t* old = *tab;
        size_t ocap = *cap;
        /* the old table is copied to slot MISC first, since the slot grows in place */
        uint64_t* tmp = (uint64_t*)yol__scratch(c, YOL__S_MISC, ocap * sizeof(uint64_t));
        uint64_t* nt;
        if (ocap && !tmp) return YOL_ERR_FULL;
        if (ocap) memcpy(tmp, old, ocap * sizeof(uint64_t));
        nt = (uint64_t*)yol__scratch(c, YOL__S_PAIR, ncap * sizeof(uint64_t));
        if (!nt) return YOL_ERR_FULL;
        memset(nt, 0, ncap * sizeof(uint64_t));
        for (n = 0; n < ocap; n++) if (tmp[n]) {
            size_t q = (size_t)yol__hash64(tmp[n]) & (ncap - 1);
            while (nt[q]) q = (q + 1) & (ncap - 1);
            nt[q] = tmp[n];
        }
        *tab = nt; *cap = ncap;
    }
    mask = *cap - 1;
    h = yol__hash64(key);
    for (k = (size_t)h & mask;; k = (k + 1) & mask) {
        if ((*tab)[k] == key) return 0;
        if ((*tab)[k] == 0) { (*tab)[k] = key; (*count)++; return 1; }
    }
}

/* --- heap of crossing ys ------------------------------------------------------- */

static int yol__heap_push(yol_ctx* c, double** h, size_t* n, size_t* cap, double y) {
    size_t i;
    if (*n == *cap) {
        size_t bytes = c->scratch_cap[YOL__S_HEAP];
        void* v = c->scratch[YOL__S_HEAP];
        if (yol__grow(c, &v, &bytes, (*n + 64) * sizeof(double)) < 0) return YOL_ERR_FULL;
        c->scratch[YOL__S_HEAP] = v; c->scratch_cap[YOL__S_HEAP] = bytes;
        *h = (double*)v; *cap = bytes / sizeof(double);
    }
    i = (*n)++;
    while (i > 0 && (*h)[(i - 1) / 2] > y) { (*h)[i] = (*h)[(i - 1) / 2]; i = (i - 1) / 2; }
    (*h)[i] = y;
    return YOL_OK;
}
static void yol__heap_pop(double* h, size_t* n) {
    double last = h[--*n];
    size_t i = 0;
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= *n) break;
        if (c + 1 < *n && h[c + 1] < h[c]) c++;
        if (h[c] >= last) break;
        h[i] = h[c]; i = c;
    }
    if (*n) h[i] = last;
}

/* --- the sweep, and the resolve on it ----------------------------------------- */

/* The sweep's work bound: piece positions computed, about 2 s. A real glyph
 * or stroke needs below 10^6; an adversarial outline is refused. */
#ifndef YOL_SWEEP_WORK
#define YOL_SWEEP_WORK (1 << 27)
#endif
#define YOL__SWEEP_WORK YOL_SWEEP_WORK

#define YOL__OP_A    0   /* inside = A under its rule                           */
#define YOL__OP_OR   1   /* A or B (B nonzero)                                  */
#define YOL__OP_DIFF 2   /* A and not B                                         */

typedef struct yol__out { yol__seg* s; size_t n, cap; } yol__out;

static int yol__seg_push(yol_ctx* c, yol__out* o, double x0, double y0, double cx, double cy, double x1, double y1, int line) {
    yol__seg* s;
    if (o->n == o->cap) {
        size_t bytes = c->scratch_cap[YOL__S_SEG];
        void* v = c->scratch[YOL__S_SEG];
        if (yol__grow(c, &v, &bytes, (o->n + 64) * sizeof(yol__seg)) < 0) return YOL_ERR_FULL;
        c->scratch[YOL__S_SEG] = v; c->scratch_cap[YOL__S_SEG] = bytes;
        o->s = (yol__seg*)v; o->cap = bytes / sizeof(yol__seg);
    }
    s = &o->s[o->n++];
    s->x0 = x0; s->y0 = y0; s->x1 = x1; s->y1 = y1;
    if (line) { s->cx = x0; s->cy = y0; } else { s->cx = cx; s->cy = cy; }
    s->line = line; s->pad = 0;
    return YOL_OK;
}

/* A piece's boundary run [t0, t1] as an output segment; d < 0 runs toward
 * -y (the region on its right), d > 0 toward +y. */
static int yol__emit_run(yol_ctx* c, yol__out* o, const yol__pc* q, yol__ps* st, double t1, double x1, double y1) {
    double t0 = st->rt, cx, cy, a = (1 - t0) * (1 - t1), b = (1 - t0) * t1 + t0 * (1 - t1), e = t0 * t1;
    int rc;
    cx = a * q->x0 + b * q->x1 + e * q->x2; cy = a * q->y0 + b * q->y1 + e * q->y2;
    if (st->rdir < 0) rc = yol__seg_push(c, o, x1, y1, cx, cy, st->rx, st->ry, q->line);
    else rc = yol__seg_push(c, o, st->rx, st->ry, cx, cy, x1, y1, q->line);
    st->rdir = 0;
    return rc;
}

/* Horizontal boundary on line y: where inside-above (A) and inside-below (B)
 * differ. A and B are sorted boundary xs of interval sets. Inside below
 * runs +x (a top edge), inside above runs -x. */
static int yol__horiz(yol_ctx* c, yol__out* o, double y, const double* A, size_t na, const double* B, size_t nb) {
    size_t i = 0, j = 0;
    int ia = 0, ib = 0, open = 0, odir = 0, rc;
    double xs = 0;
    while (i < na || j < nb) {
        double x;
        int st, dir;
        if (j >= nb || (i < na && A[i] <= B[j])) { x = A[i++]; ia ^= 1; } else { x = B[j++]; ib ^= 1; }
        st = ia != ib; dir = ib ? 1 : -1;
        if (open && (!st || dir != odir)) {
            if (x > xs) {
                rc = odir > 0 ? yol__seg_push(c, o, xs, y, 0, 0, x, y, 1) : yol__seg_push(c, o, x, y, 0, 0, xs, y, 1);
                if (rc < 0) return rc;
            }
            open = 0;
        }
        if (st && !open) { open = 1; xs = x; odir = dir; }
    }
    return YOL_OK;
}

static void yol__dsort(double* a, size_t n) {
    size_t i;
    for (i = 1; i < n; i++) { double t = a[i]; size_t k = i; while (k > 0 && a[k - 1] > t) { a[k] = a[k - 1]; k--; } a[k] = t; }
}

static int yol__inside(int rule, int op, int w0, int w1) {
    int a = rule == YOL_EVENODD ? (w0 & 1) : (w0 != 0);
    if (op == YOL__OP_OR) return a || w1 != 0;
    if (op == YOL__OP_DIFF) return a && w1 == 0;
    return a;
}

/* --- chaining -------------------------------------------------------------- */

typedef struct yol__pt { double x, y; } yol__pt;

static int yol__ptkv_lt(const yol__seg* s, int32_t a, int32_t b) {
    if (s[a].y0 != s[b].y0) return s[a].y0 < s[b].y0;
    if (s[a].x0 != s[b].x0) return s[a].x0 < s[b].x0;
    return a < b;
}
static void yol__seg_order(const yol__seg* s, int32_t* ix, size_t n) {
    size_t i, k;
    /* heapsort of indices by start (y, x) */
    for (k = n / 2; k-- > 0;) {
        size_t r = k;
        int32_t t = ix[r];
        for (;;) { size_t ch = 2 * r + 1; if (ch >= n) break; if (ch + 1 < n && yol__ptkv_lt(s, ix[ch], ix[ch + 1])) ch++; if (!yol__ptkv_lt(s, t, ix[ch])) break; ix[r] = ix[ch]; r = ch; }
        ix[r] = t;
    }
    for (i = n; i-- > 1;) {
        int32_t t = ix[i];
        size_t r = 0;
        ix[i] = ix[0];
        for (;;) { size_t ch = 2 * r + 1; if (ch >= i) break; if (ch + 1 < i && yol__ptkv_lt(s, ix[ch], ix[ch + 1])) ch++; if (!yol__ptkv_lt(s, t, ix[ch])) break; ix[r] = ix[ch]; r = ch; }
        ix[r] = t;
    }
}

/* Points on one line within eps_x are one point (two pieces crossing at y
 * give x values a few ulps apart); zero-length segments go; the rest are
 * chained by exact endpoint match into closed contours. */
static int yol__chain(yol__sw* w, yol__out* o, yol_path* out) {
    yol_ctx* c = w->c;
    size_t n = o->n, i, m;
    yol__kv* kv;
    int32_t* ix;
    uint8_t* used;
    int rc;
    out->n_pts = 0; out->n_contours = 0; out->in_contour = 0; out->status = 0;
    out->rule = YOL_NONZERO; out->flags = YOL_PATH_RESOLVED;
    if (n == 0) return YOL_OK;
    kv = (yol__kv*)yol__scratch(c, YOL__S_KV, 2 * n * sizeof(yol__kv));
    if (!kv) return YOL_ERR_FULL;
    /* sort endpoint refs by (y, x): key y, then x by a second stable pass */
    for (i = 0; i < 2 * n; i++) { kv[i].k = (i & 1) ? o->s[i / 2].x1 : o->s[i / 2].x0; kv[i].v = (int32_t)i; kv[i].pad = 0; }
    yol__kv_sort(kv, 2 * n);
    {
        /* group by y exactly, within a group the x order is kv's */
        yol__kv* k2 = (yol__kv*)yol__scratch(c, YOL__S_KV2, 2 * n * sizeof(yol__kv));
        size_t a, b;
        if (!k2) return YOL_ERR_FULL;
        for (i = 0; i < 2 * n; i++) { int32_t r = kv[i].v; k2[i].k = (r & 1) ? o->s[r / 2].y1 : o->s[r / 2].y0; k2[i].v = (int32_t)i; k2[i].pad = 0; }
        yol__kv_sort(k2, 2 * n);    /* by y, ties by x order */
        for (a = 0; a < 2 * n; a = b) {
            double xr, yv = k2[a].k;
            int32_t r0 = kv[k2[a].v].v;
            xr = (r0 & 1) ? o->s[r0 / 2].x1 : o->s[r0 / 2].x0;
            for (b = a; b < 2 * n && k2[b].k == yv; b++) {
                int32_t r = kv[k2[b].v].v;
                double* px = (r & 1) ? &o->s[r / 2].x1 : &o->s[r / 2].x0;
                if (*px - xr > w->eps_x) xr = *px;
                else *px = xr;
            }
        }
    }
    /* drop zero-length segments; fix line controls after the moves */
    for (i = 0, m = 0; i < n; i++) {
        yol__seg s = o->s[i];
        if (s.x0 == s.x1 && s.y0 == s.y1) continue;
        if (s.line) { s.cx = s.x0; s.cy = s.y0; }
        o->s[m++] = s;
    }
    n = o->n = m;
    if (n == 0) return YOL_OK;
    ix = (int32_t*)yol__scratch(c, YOL__S_ORDER, n * sizeof(int32_t));
    used = (uint8_t*)yol__scratch(c, YOL__S_USED, n);
    if (!ix || !used) return YOL_ERR_FULL;
    for (i = 0; i < n; i++) ix[i] = (int32_t)i;
    yol__seg_order(o->s, ix, n);
    memset(used, 0, n);
    rc = yol__path_reserve(out, (int32_t)(2 * n + n), (int32_t)n);
    if (rc < 0) return rc;
    for (i = 0; i < n; i++) {
        int32_t cur = ix[i];
        double sx, sy;
        size_t steps = 0;
        if (used[cur]) continue;
        sx = o->s[cur].x0; sy = o->s[cur].y0;
        out->contours[out->n_contours++] = (uint32_t)out->n_pts;
        out->pts[2 * out->n_pts] = sx; out->pts[2 * out->n_pts + 1] = sy; out->n_pts++;
        for (;;) {
            const yol__seg* s = &o->s[cur];
            size_t lo = 0, hi = n;
            int32_t next = -1;
            used[cur] = 1;
            out->pts[2 * out->n_pts] = s->cx; out->pts[2 * out->n_pts + 1] = s->cy;
            out->pts[2 * out->n_pts + 2] = s->x1; out->pts[2 * out->n_pts + 3] = s->y1;
            out->n_pts += 2;
            if (s->x1 == sx && s->y1 == sy) break;
            if (++steps > n) return yol__err(c, YOL_ERR_NUMERIC, "resolve: a contour does not close");
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                const yol__seg* t = &o->s[ix[mid]];
                if (t->y0 < s->y1 || (t->y0 == s->y1 && t->x0 < s->x1)) lo = mid + 1; else hi = mid;
            }
            for (; lo < n && o->s[ix[lo]].y0 == s->y1 && o->s[ix[lo]].x0 == s->x1; lo++)
                if (!used[ix[lo]]) { next = ix[lo]; break; }
            if (next < 0) return yol__err(c, YOL_ERR_NUMERIC, "resolve: the boundary does not close at (%.17g, %.17g)", s->x1, s->y1);
            cur = next;
        }
    }
    return YOL_OK;
}

static double yol__act_xm(const yol__sw* w, yol__act* a, double ym) {
    if (a->xm == HUGE_VAL) { double t; yol__pos(&w->pc[a->i], ym, &t, &a->xm); }
    return a->xm;
}
/* a after b in the strip's order */
static int yol__act_after(const yol__sw* w, yol__act* a, yol__act* b, double ym) {
    double xa, xb;
    if (fabs(a->xa - b->xa) > w->eps_x) return a->xa > b->xa;
    if (fabs(a->xb - b->xb) > w->eps_x) return a->xb > b->xb;
    xa = yol__act_xm(w, a, ym); xb = yol__act_xm(w, b, ym);
    if (xa != xb) return xa > xb;
    return a->i > b->i;
}

/* How far a piece's x on the line yb moves when yb moves by eps_y: the
 * sweep's uncertainty of that x. |dx/dy| eps_y, 0 for a vertical piece, at
 * most the piece's width. A quadratic's |dx/dy| at its t on the line,
 * except at its end, where a horizontal tangent makes it infinite: there
 * one position at yb - eps_y. Two positions for every pair (the first
 * version) made Segoe UI's resolve 30 to 60 percent slower. */
static double yol__xtol(const yol__sw* w, const yol__act* a, double yb) {
    const yol__pc* q = &w->pc[a->i];
    double t = a->tb, dx, dy, width = fabs(q->x2 - q->x0);
    if (q->line) { dx = q->x2 - q->x0; dy = q->y2 - q->y0; }
    else if (t >= 1) {
        double x;
        yol__pos(q, yb - w->eps_y, &t, &x);
        return fabs(x - a->xb);
    } else {
        dx = (1 - t) * (q->x1 - q->x0) + t * (q->x2 - q->x1);
        dy = (1 - t) * (q->y1 - q->y0) + t * (q->y2 - q->y1);
    }
    return dy > 0 ? fmin(fabs(dx) / dy * w->eps_y, width) : width;
}

/* yol__xtol()'s bound for a whole piece, once: a monotone quadratic's
 * |dx/dy| is largest at an end. order_xb() skips a pair farther apart than
 * both bounds without the divisions: stroking every Segoe UI glyph, 7.9
 * million pairs reached them (0.5 million with the bound); the stroke
 * took 12 to 21 percent longer than v0.3.0 with them, 2 to 12 with the
 * bound (docs/outline.md, "v0.3.1"). */
static double yol__xt(const yol__sw* w, const yol__pc* q) {
    double width = fabs(q->x2 - q->x0), d0, d1;
    if (q->line) return fmin(width / (q->y2 - q->y0) * w->eps_y, width) * 1.0001;
    d0 = q->y1 > q->y0 ? fabs(q->x1 - q->x0) / (q->y1 - q->y0) : HUGE_VAL;
    d1 = q->y2 > q->y1 ? fabs(q->x2 - q->x1) / (q->y2 - q->y1) : HUGE_VAL;
    return fmin(fmax(d0, d1) * w->eps_y, width) * 1.0001;
}

/* The xs on a strip's bottom line yb, in the strip's order. A nearly
 * horizontal piece's x there is off by its |dx/dy| times the error of yb:
 * at a crossing with a stem, 8.6e-10 for an edge 1.7e-8 tall and 0.8
 * wide, past eps_x (8.5e-10 there). A crossing within eps_y of the line is not a strip of its own, so
 * two pieces can also reach the line already crossed. Then the xs are out
 * of the order that the windings came from, or the two pieces keep their
 * order into the next strip and cross inside it unseen; either way their
 * runs miss each other and the boundary does not close (v0.3.0). So two
 * neighbors out of order, or closer than either one's x moves in eps_y of
 * y, meet: both take the x of the one that moves less if moved. A piece
 * that ends on the line moves only its end, a jog of no area along the
 * line; a piece that goes on moves its next run's start. Then the xs never
 * decrease in the strip's order. */
static void yol__order_xb(const yol__sw* w, yol__act* act, size_t n, double yb) {
    size_t i, k;
    for (i = 1; i < n; i++) {
        yol__act *a = &act[i - 1], *b = &act[i];
        double gap = b->xb - a->xb, ta = -1, tb = -1, v;
        int ea, eb;
        if (gap == 0) continue;
        if (gap > 0) {
            if (gap > fmax((double)w->pc[a->i].xt, (double)w->pc[b->i].xt)) continue;
            ta = yol__xtol(w, a, yb);
            tb = yol__xtol(w, b, yb);
            if (gap > fmax(ta, tb)) continue;
        }
        ea = a->tb >= 1; eb = b->tb >= 1;
        if (ea != eb) v = ea ? b->xb : a->xb;
        else {
            if (ta < 0) { ta = yol__xtol(w, a, yb); tb = yol__xtol(w, b, yb); }
            v = tb <= ta ? b->xb : a->xb;
        }
        a->xb = b->xb = v;
        for (k = i - 1; k-- > 0 && act[k].xb > v;) act[k].xb = v;
    }
}

/* The sweep: the region of A (rule) op B as closed contours in out. */
static int yol__sweep(yol_ctx* c, const yol_path* A, int rule, const yol_path* B, int op, yol_path* out) {
    yol__sw w;
    yol__out o;
    yol__ps* ps;
    yol__act* act;
    yol__kv* order;
    double *ev, *heap = NULL, *iva, *ivb;
    uint64_t* pairs = NULL;
    size_t nord = 0, nev = 0, nheap = 0, capheap = 0, npairs = 0, cappairs = 0, nprev = 0, ei = 0, head = 0, i;
    int32_t* live;
    size_t nlive = 0, guard = 0, maxguard, work;
    double ycur;
    int rc;

    memset(&w, 0, sizeof w);
    w.c = c;
    w.pc = (yol__pc*)c->scratch[YOL__S_PIECE];
    w.cappc = (int32_t)(c->scratch_cap[YOL__S_PIECE] / sizeof(yol__pc));
    rc = yol__path_pieces(&w, A, 0);
    if (rc >= 0 && B) rc = yol__path_pieces(&w, B, 1);
    if (rc < 0) return rc;
    /* the scale for the tolerances */
    w.ext = 0;
    for (i = 0; i < (size_t)w.npc; i++) {
        const yol__pc* q = &w.pc[i];
        w.ext = fmax(w.ext, fmax(fabs(q->x0), fmax(fabs(q->x1), fmax(fabs(q->x2), fmax(fabs(q->y0), fmax(fabs(q->y1), fabs(q->y2)))))));
    }
    if (!(w.ext > 0)) w.ext = 1;
    if (!yol__finite(w.ext)) return yol__err(c, YOL_ERR_ARG, "resolve: a point is not finite");
    memset(&o, 0, sizeof o);
    o.s = (yol__seg*)c->scratch[YOL__S_SEG];
    o.cap = c->scratch_cap[YOL__S_SEG] / sizeof(yol__seg);
    if (w.npc == 0) return yol__chain(&w, &o, out);
    w.eps_y = 1e-13 * w.ext; w.eps_x = 1e-9 * w.ext;
    /* End ys within eps_y of each other are one line: a nearly horizontal
     * piece (an arc's end a few ulps off its line) becomes horizontal and
     * goes, and no strip is thinner than eps_y. */
    {
        yol__kv* yk = (yol__kv*)yol__scratch(c, YOL__S_KV2, 2 * (size_t)w.npc * sizeof(yol__kv));
        size_t a, m;
        if (!yk) return YOL_ERR_FULL;
        for (i = 0; i < (size_t)w.npc; i++) { yk[2 * i].k = w.pc[i].y0; yk[2 * i].v = (int32_t)(2 * i); yk[2 * i + 1].k = w.pc[i].y2; yk[2 * i + 1].v = (int32_t)(2 * i + 1); yk[2 * i].pad = yk[2 * i + 1].pad = 0; }
        yol__kv_sort(yk, 2 * (size_t)w.npc);
        for (a = 0; a < 2 * (size_t)w.npc;) {
            double rep = yk[a].k, last = rep;
            size_t b = a;
            while (b < 2 * (size_t)w.npc && yk[b].k - last <= w.eps_y) {
                yol__pc* q = &w.pc[yk[b].v / 2];
                last = yk[b].k;
                if (yk[b].v & 1) q->y2 = rep; else q->y0 = rep;
                b++;
            }
            a = b;
        }
        /* a piece that became horizontal stays in place with dir 0, so the
         * sorted ends above remain the events and the start order */
        for (i = 0, m = 0; i < (size_t)w.npc; i++) {
            yol__pc* q = &w.pc[i];
            if (q->y0 == q->y2) { q->dir = 0; continue; }
            if (q->y1 < q->y0) q->y1 = q->y0;
            if (q->y1 > q->y2) q->y1 = q->y2;
            q->xt = (float)yol__xt(&w, q);
            m++;
        }
        if (m == 0) return yol__chain(&w, &o, out);
    }

    ps = (yol__ps*)yol__scratch(c, YOL__S_POS, (size_t)w.npc * sizeof(yol__ps));
    act = (yol__act*)yol__scratch(c, YOL__S_GROUP, (size_t)w.npc * sizeof(yol__act));
    live = (int32_t*)yol__scratch(c, YOL__S_LIVE, (size_t)w.npc * sizeof(int32_t));
    order = (yol__kv*)yol__scratch(c, YOL__S_KV, (size_t)w.npc * sizeof(yol__kv));
    ev = (double*)yol__scratch(c, YOL__S_EVENT, 2 * (size_t)w.npc * sizeof(double));
    iva = (double*)yol__scratch(c, YOL__S_IVA, (size_t)w.npc * sizeof(double));
    ivb = (double*)yol__scratch(c, YOL__S_IVB, (size_t)w.npc * sizeof(double));
    if (!ps || !act || !live || !order || !ev || !iva || !ivb) return YOL_ERR_FULL;
    heap = (double*)c->scratch[YOL__S_HEAP]; capheap = c->scratch_cap[YOL__S_HEAP] / sizeof(double);
    memset(ps, 0, (size_t)w.npc * sizeof(yol__ps));
    for (i = 0; i < (size_t)w.npc; i++) ps[i].y = -HUGE_VAL;
    {
        /* the events and the start order from the ends sorted for the snap */
        const yol__kv* yk = (const yol__kv*)c->scratch[YOL__S_KV2];
        for (i = 0; i < 2 * (size_t)w.npc; i++) {
            const yol__pc* q = &w.pc[yk[i].v / 2];
            if (!q->dir) continue;
            if (nev == 0 || yk[i].k != ev[nev - 1]) ev[nev++] = yk[i].k;
            if (!(yk[i].v & 1)) { order[nord].k = yk[i].k; order[nord].v = yk[i].v / 2; order[nord].pad = 0; nord++; }
        }
    }
    /* each strip ends at an event or a crossing; at most 4 crossings a pair
     * and one pair per adjacency: a generous bound that never binds */
    maxguard = 64 * (size_t)w.npc * (size_t)w.npc + 1024;
    work = 0;
    ycur = ev[0];
    for (;;) {
        double yb, ym;
        size_t k, it;
        int w0, w1;
        if (++guard > maxguard) return yol__err(c, YOL_ERR_NUMERIC, "resolve: the sweep did not finish");
        while (ei < nev && ev[ei] <= ycur) ei++;
        while (nheap && heap[0] <= ycur + w.eps_y) yol__heap_pop(heap, &nheap);
        if (ei >= nev && !nheap) break;
        yb = ei < nev ? ev[ei] : HUGE_VAL;
        if (nheap && heap[0] < yb - w.eps_y) yb = heap[0];
        /* the live set: drop finished pieces in order, append new ones */
        for (k = 0, i = 0; i < nlive; i++) if (w.pc[live[i]].y2 > ycur) live[k++] = live[i];
        nlive = k;
        while (head < nord && w.pc[order[head].v].y0 <= ycur) live[nlive++] = order[head++].v;
        for (it = 0;; it++) {
            int shrunk = 0;
            ym = 0.5 * (ycur + yb);
            work += nlive + 1;
            if (work > YOL__SWEEP_WORK) return yol__err(c, YOL_ERR_REFUSED, "resolve: more than %d piece positions (%d pieces): an outline this complex is refused", YOL__SWEEP_WORK, w.npc);
            for (i = 0; i < nlive; i++) {
                yol__act* a = &act[i];
                const yol__pc* q = &w.pc[live[i]];
                yol__ps* s = &ps[live[i]];
                a->i = live[i];
                if (q->y0 >= ycur) { a->ta = 0; a->xa = q->x0; }
                else if (s->y == ycur) { a->ta = s->t; a->xa = s->x; }
                else yol__pos(q, ycur, &a->ta, &a->xa);
                yol__pos(q, yb, &a->tb, &a->xb);
                a->xm = HUGE_VAL;   /* computed only for a tie */
            }
            /* Insertion sort; live keeps the order between strips. No two
             * pieces cross inside a strip, so the order at its top decides,
             * then at its bottom, then at its middle. */
            for (i = 1; i < nlive; i++) {
                yol__act t = act[i];
                size_t m = i;
                while (m > 0 && yol__act_after(&w, &act[m - 1], &t, ym)) { act[m] = act[m - 1]; m--; }
                act[m] = t;
            }
            for (i = 0; i < nlive; i++) live[i] = act[i].i;
            /* crossings of pairs that are adjacent for the first time */
            for (i = 0; i + 1 < nlive; i++) {
                double ys[8];
                int nc, j;
                const yol__pc *P = &w.pc[act[i].i], *Q = &w.pc[act[i + 1].i];
                if (fmax(act[i].xa, act[i].xb) < fmin(act[i + 1].xa, act[i + 1].xb) - w.eps_x &&
                    fmax(P->x0, P->x2) < fmin(Q->x0, Q->x2)) continue;   /* apart for both whole pieces */
                rc = yol__pair_add(c, &pairs, &cappairs, &npairs, (uint32_t)act[i].i, (uint32_t)act[i + 1].i);
                if (rc < 0) return rc;
                if (rc == 0) continue;
                nc = yol__crossings(&w, P, Q, ys);
                for (j = 0; j < nc; j++) if (ys[j] > ycur + w.eps_y) {
                    rc = yol__heap_push(c, &heap, &nheap, &capheap, ys[j]);
                    if (rc < 0) return rc;
                    if (ys[j] < yb - w.eps_y) { yb = ys[j]; shrunk = 1; }
                }
            }
            if (!shrunk || it > nlive + 2) break;
        }
        yol__order_xb(&w, act, nlive, yb);
        /* coincident pieces act as one edge with their summed directions */
        for (i = 0; i < nlive;) {
            size_t g = i + 1, m;
            int32_t d0 = 0, d1 = 0;
            while (g < nlive && fabs(act[g].xa - act[i].xa) <= w.eps_x && fabs(act[g].xb - act[i].xb) <= w.eps_x &&
                   fabs(yol__act_xm(&w, &act[g], ym) - yol__act_xm(&w, &act[i], ym)) <= w.eps_x) g++;
            for (m = i; m < g; m++) {
                const yol__pc* q = &w.pc[act[m].i];
                if (q->set) d1 += q->dir; else d0 += q->dir;
                act[m].lead = 0; act[m].d0 = act[m].d1 = 0;
            }
            act[i].lead = 1; act[i].d0 = d0; act[i].d1 = d1;
            i = g;
        }
        /* boundary runs and the horizontal boundary at ycur */
        {
            size_t ncur = 0;
            w0 = w1 = 0;
            for (i = 0; i < nlive; i++) {
                yol__act* a = &act[i];
                yol__ps* s = &ps[a->i];
                int in0 = yol__inside(rule, op, w0, w1), in1, d = 0;
                w0 += a->d0; w1 += a->d1;
                in1 = yol__inside(rule, op, w0, w1);
                if (a->lead && in0 != in1) d = in1 ? -1 : 1;
                if (s->rdir && s->rdir != d) { rc = yol__emit_run(c, &o, &w.pc[a->i], s, a->ta, a->xa, ycur); if (rc < 0) return rc; }
                if (d && !s->rdir) { s->rdir = d; s->rt = a->ta; s->rx = a->xa; s->ry = ycur; }
                if (d) iva[ncur++] = a->xa;
            }
            yol__dsort(iva, ncur);
            rc = yol__horiz(c, &o, ycur, ivb, nprev, iva, ncur);
            if (rc < 0) return rc;
            nprev = 0;
            for (i = 0; i < nlive; i++) {
                yol__act* a = &act[i];
                yol__ps* s = &ps[a->i];
                if (s->rdir) ivb[nprev++] = a->xb;
                s->y = yb; s->t = a->tb; s->x = a->xb;
                if (s->rdir && a->tb == 1) { rc = yol__emit_run(c, &o, &w.pc[a->i], s, 1, a->xb, yb); if (rc < 0) return rc; }
            }
            yol__dsort(ivb, nprev);
        }
        ycur = yb;
    }
    rc = yol__horiz(c, &o, ycur, ivb, nprev, NULL, 0);
    if (rc < 0) return rc;
    return yol__chain(&w, &o, out);
}

static int yol__resolve(yol_ctx* c, const yol_path* in, yol_path* out) {
    int rc;
    if (!c || !in || !out) return YOL_ERR_ARG;
    if (in->status) return yol__err(c, in->status, "resolve: the path has a builder error");
    if (in != out && !out->ctx && out->cap_pts == 0) return yol__err(c, YOL_ERR_ARG, "resolve: out has no storage");
    rc = yol__sweep(c, in, in->rule, NULL, YOL__OP_A, out);
    if (rc < 0) { out->n_pts = 0; out->n_contours = 0; return rc; }
    return YOL_OK;
}

/* --- scratch paths ----------------------------------------------------------- */

/* A path whose storage lives in two scratch slots, so a call reuses it. */
static void yol__tmp_get(yol_ctx* c, yol_path* t, int sp, int sc) {
    memset(t, 0, sizeof *t);
    t->ctx = c;
    t->xf = yol_xf_identity();
    t->pts = (double*)c->scratch[sp];
    t->cap_pts = (int32_t)(c->scratch_cap[sp] / (2 * sizeof(double)) > 0x3fffffff ? 0x3fffffff : c->scratch_cap[sp] / (2 * sizeof(double)));
    t->contours = (uint32_t*)c->scratch[sc];
    t->cap_contours = (int32_t)(c->scratch_cap[sc] / sizeof(uint32_t) > 0x3fffffff ? 0x3fffffff : c->scratch_cap[sc] / sizeof(uint32_t));
}
static void yol__tmp_put(yol_ctx* c, yol_path* t, int sp, int sc) {
    c->scratch[sp] = t->pts; c->scratch_cap[sp] = (size_t)t->cap_pts * 2 * sizeof(double);
    c->scratch[sc] = t->contours; c->scratch_cap[sc] = (size_t)t->cap_contours * sizeof(uint32_t);
}

/* --- exact alpha -------------------------------------------------------------- */

/* One monotone quadratic (px) into the accumulator, cell by cell. Cell
 * (i, j) gets dy - A and cell i + 1 gets A, A = integral of (x - i) dy over
 * the part inside the cell; a row's prefix sum is then the winding-weighted
 * area (EXACTNESS). The walk crosses each grid line once: at most
 * |dx| + |dy| + 2 steps. */
static void yol__acc_mono(double* acc, int w, int h, const double q[6]) {
    double x0 = q[0], y0 = q[1], x2 = q[4], y2 = q[5], t = 0, px = x0, py = y0, nbx, nby;
    int sx = x2 > x0 ? 1 : (x2 < x0 ? -1 : 0), sy = y2 > y0 ? 1 : -1;
    double cx, cy;
    long steps = 0, maxsteps;
    yol__pc pc;
    if (y0 == y2) return;
    if ((y0 <= 0 && y2 <= 0) || (y0 >= h && y2 >= h) || (x0 >= w && x2 >= w)) return;
    pc.x0 = q[0]; pc.y0 = q[1]; pc.x1 = q[2]; pc.y1 = q[3]; pc.x2 = q[4]; pc.y2 = q[5];
    cx = sx >= 0 ? floor(x0) : ceil(x0) - 1;
    cy = sy > 0 ? floor(y0) : ceil(y0) - 1;
    nbx = sx > 0 ? cx + 1 : cx;
    nby = sy > 0 ? cy + 1 : cy;
    maxsteps = (long)(fabs(x2 - x0) + fabs(y2 - y0)) + 8;
    for (;;) {
        double tx = 2, ty = 2, tn, ex, ey, ccx, ccy, a, b, e;
        int hitx = 0, hity = 0;
        if (sx > 0 ? nbx < x2 : (sx < 0 ? nbx > x2 : 0)) tx = yol__solve_mono(x0, q[2], x2, nbx);
        if (sy > 0 ? nby < y2 : nby > y2) ty = yol__solve_mono(y0, q[3], y2, nby);
        tn = tx < ty ? tx : ty;
        if (tn >= 1) { tn = 1; ex = x2; ey = y2; }
        else {
            if (tn < t) tn = t;
            hitx = tx <= ty; hity = ty <= tx;
            ex = hitx ? nbx : yol__qx(&pc, tn);
            ey = hity ? nby : yol__qy(&pc, tn);
        }
        a = (1 - t) * (1 - tn); b = (1 - t) * tn + t * (1 - tn); e = t * tn;
        ccx = a * x0 + b * q[2] + e * x2; ccy = a * y0 + b * q[3] + e * y2;
        if (cy >= 0 && cy < h) {
            double* row = acc + (size_t)cy * (size_t)(w + 2), dy = ey - py;
            if (cx < 0) row[0] += dy;
            else if (cx < w) {
                double A = yol__xdy(px - cx, py, ccx - cx, ccy, ex - cx, ey);
                row[(size_t)cx] += dy - A; row[(size_t)cx + 1] += A;
            }
        }
        if (tn >= 1 || ++steps > maxsteps) break;
        if (hitx) { cx += sx; nbx += sx; }
        if (hity) { cy += sy; nby += sy; }
        t = tn; px = ex; py = ey;
    }
}

/* A quadratic (px) split at its x and y extrema, each part accumulated. */
static void yol__acc_quad(double* acc, int w, int h, const double s[6]) {
    double ts[2], cur[6], done = 0, tx = yol__ext_t(s[0], s[2], s[4]), ty = yol__ext_t(s[1], s[3], s[5]);
    int n = 0, i;
    if (tx > 0) ts[n++] = tx;
    if (ty > 0) { if (n && ty < ts[0]) { ts[1] = ts[0]; ts[0] = ty; n++; } else if (!n || ty != ts[0]) ts[n++] = ty; }
    memcpy(cur, s, sizeof cur);
    for (i = 0; i <= n; i++) {
        double a[6];
        if (i < n) {
            double t = (ts[i] - done) / (1 - done);
            double mx0 = cur[0] + t * (cur[2] - cur[0]), my0 = cur[1] + t * (cur[3] - cur[1]);
            double mx1 = cur[2] + t * (cur[4] - cur[2]), my1 = cur[3] + t * (cur[5] - cur[3]);
            double px = mx0 + t * (mx1 - mx0), py = my0 + t * (my1 - my0);
            a[0] = cur[0]; a[1] = cur[1]; a[2] = mx0; a[3] = my0; a[4] = px; a[5] = py;
            cur[0] = px; cur[1] = py; cur[2] = mx1; cur[3] = my1;
            if (ts[i] == tx) { a[2] = a[4]; cur[2] = cur[0]; }
            if (ts[i] == ty) { a[3] = a[5]; cur[3] = cur[1]; }
            done = ts[i];
        } else memcpy(a, cur, sizeof a);
        yol__acc_mono(acc, w, h, a);
    }
}

static int yol__raster(yol_ctx* c, const yol_path* in, const yol_raster_desc* d, yol_box* box) {
    yol_path tmp;
    const yol_path* r = in;
    double sc, ox, oy, minx = HUGE_VAL, miny = HUGE_VAL, maxx = -HUGE_VAL, maxy = -HUGE_VAL;
    int rc = YOL_OK, bpp, used_tmp = 0;
    int32_t k, i;
    if (!c || !in || !d) return YOL_ERR_ARG;
    sc = d->scale; ox = d->x; oy = d->y;
    if (!(sc > 0) || !yol__finite(sc) || !yol__finite(ox) || !yol__finite(oy)) return yol__err(c, YOL_ERR_ARG, "raster: scale must be > 0 and finite, x and y finite");
    if (d->format < YOL_ALPHA_U8 || d->format > YOL_ALPHA_F64) return yol__err(c, YOL_ERR_ARG, "raster: format %d", d->format);
    bpp = d->format == YOL_ALPHA_U8 ? 1 : (d->format == YOL_ALPHA_U16 ? 2 : (d->format == YOL_ALPHA_F32 ? 4 : 8));
    if (d->out && (d->w <= 0 || d->h <= 0 || d->stride < d->w * bpp || d->w > (1 << 20) || d->h > (1 << 20)))
        return yol__err(c, YOL_ERR_ARG, "raster: w, h > 0 and stride >= w times the value size");
    if (in->status) return yol__err(c, in->status, "raster: the path has a builder error");
    if (!(in->flags & YOL_PATH_RESOLVED)) {
        yol__tmp_get(c, &tmp, YOL__S_TP_PTS, YOL__S_TP_CON);
        rc = yol__sweep(c, in, in->rule, NULL, YOL__OP_A, &tmp);
        yol__tmp_put(c, &tmp, YOL__S_TP_PTS, YOL__S_TP_CON);
        if (rc < 0) return rc;
        r = &tmp; used_tmp = 1;
    }
    (void)used_tmp;
    for (i = 0; i < r->n_pts; i++) {
        double x = r->pts[2 * i] * sc + ox, y = r->pts[2 * i + 1] * sc + oy;
        minx = fmin(minx, x); maxx = fmax(maxx, x); miny = fmin(miny, y); maxy = fmax(maxy, y);
    }
    if (box) {
        if (r->n_pts == 0) { box->x0 = box->y0 = box->x1 = box->y1 = 0; }
        else {
            if (fabs(minx) > 1e9 || fabs(maxx) > 1e9 || fabs(miny) > 1e9 || fabs(maxy) > 1e9) return yol__err(c, YOL_ERR_RANGE, "raster: the box is beyond 1e9 px");
            box->x0 = (int32_t)floor(minx); box->y0 = (int32_t)floor(miny);
            box->x1 = (int32_t)ceil(maxx); box->y1 = (int32_t)ceil(maxy);
            if (box->x1 == box->x0) box->x1++;
            if (box->y1 == box->y0) box->y1++;
        }
    }
    if (!d->out) return YOL_OK;
    {
        int W = d->w, H = d->h, x, y;
        size_t rowlen = (size_t)W + 2;
        double* acc = (double*)yol__scratch(c, YOL__S_ACC, rowlen * (size_t)H * sizeof(double));
        if (!acc) return YOL_ERR_FULL;
        memset(acc, 0, rowlen * (size_t)H * sizeof(double));
        for (k = 0; k < r->n_contours; k++) {
            int32_t f = yol__cfirst(r, k), e = yol__cend(r, k);
            const double* q = r->pts;
            double s[6];
            if (e - f < 3) continue;
            for (i = f; i + 2 < e; i += 2) {
                int j;
                for (j = 0; j < 3; j++) { s[2 * j] = q[2 * (i + j)] * sc + ox; s[2 * j + 1] = q[2 * (i + j) + 1] * sc + oy; }
                yol__acc_quad(acc, W, H, s);
            }
            if (q[2 * (e - 1)] != q[2 * f] || q[2 * (e - 1) + 1] != q[2 * f + 1]) {
                s[0] = q[2 * (e - 1)] * sc + ox; s[1] = q[2 * (e - 1) + 1] * sc + oy;
                s[4] = q[2 * f] * sc + ox; s[5] = q[2 * f + 1] * sc + oy;
                s[2] = s[0]; s[3] = s[1];
                yol__acc_quad(acc, W, H, s);
            }
        }
        for (y = 0; y < H; y++) {
            const double* row = acc + (size_t)y * rowlen;
            uint8_t* o = (uint8_t*)d->out + (size_t)y * (size_t)d->stride;
            double sum = 0;
            for (x = 0; x < W; x++) {
                double v;
                sum += row[x];
                v = -sum;   /* +1 clockwise on screen accumulates as -1 */
                /* the prefix sum leaves rounding residue (1e-16) past the last
                 * edge and inside: snapped, so empty is 0 and full is 1 */
                if (v < 1e-13) v = 0;
                if (v > 1 - 1e-13) v = 1;
                switch (d->format) {
                case YOL_ALPHA_U8: o[x] = (uint8_t)floor(v * 255.0 + 0.5); break;
                case YOL_ALPHA_U16: { uint16_t u = (uint16_t)floor(v * 65535.0 + 0.5); memcpy(o + 2 * x, &u, 2); break; }
                case YOL_ALPHA_F32: { float fv = (float)v; memcpy(o + 4 * x, &fv, 4); break; }
                default: memcpy(o + 8 * x, &v, 8); break;
                }
            }
        }
    }
    return rc;
}

/* --- strokes (Nehab 2020) ------------------------------------------------------ */

typedef struct yol__sk {
    yol_ctx* c;
    yol_path* e;        /* the elements: closed contours, each positive       */
    double h, tol;
    long budget;          /* approximation calls left                           */
} yol__sk;

/* Reverses contour k in place; a line keeps its control on its start. */
static void yol__contour_reverse(yol_path* p, int32_t k) {
    int32_t f = yol__cfirst(p, k), e = yol__cend(p, k), i, j;
    double* q = p->pts;
    for (i = f, j = e - 1; i < j; i++, j--) {
        double tx = q[2 * i], ty = q[2 * i + 1];
        q[2 * i] = q[2 * j]; q[2 * i + 1] = q[2 * j + 1];
        q[2 * j] = tx; q[2 * j + 1] = ty;
    }
    for (i = f; i + 2 < e; i += 2) {
        /* the segment's old start is q[i + 2]; a line had its control there */
        if (q[2 * (i + 1)] == q[2 * (i + 2)] && q[2 * (i + 1) + 1] == q[2 * (i + 2) + 1]) {
            q[2 * (i + 1)] = q[2 * i]; q[2 * (i + 1) + 1] = q[2 * i + 1];
        }
    }
}

static double yol__contour_area(const yol_path* p, int32_t k) {
    int32_t f = yol__cfirst(p, k), e = yol__cend(p, k), i;
    const double* q = p->pts;
    double s = 0;
    for (i = f; i + 2 < e; i += 2) s += yol__xdy(q[2 * i], q[2 * i + 1], q[2 * i + 2], q[2 * i + 3], q[2 * i + 4], q[2 * i + 5]);
    return s;
}

/* Ends the element contour just built and makes it positive. */
static int yol__elem_end(yol__sk* s) {
    yol_path* e = s->e;
    yol_close_path(e);
    if (e->status) return e->status;
    if (e->n_contours > 0 && yol__contour_area(e, e->n_contours - 1) < 0) yol__contour_reverse(e, e->n_contours - 1);
    return YOL_OK;
}

/* The curves an element follows: the offset by s along N, or the evolute. */
typedef struct yol__cv {
    double x0, y0, x1, y1, x2, y2, s;
    double n0x, n0y, n1x, n1y;     /* shared unit normals at t = 0 and t = 1 */
    int evolute, has0, has1;
} yol__cv;

/* A point of the segment; its ends exactly, so every element that meets
 * there names the same bits. */
static void yol__bz(const double* q, double t, double* x, double* y) {
    double u = 1 - t;
    if (t == 0) { *x = q[0]; *y = q[1]; return; }
    if (t == 1) { *x = q[4]; *y = q[5]; return; }
    *x = u * u * q[0] + 2 * t * u * q[2] + t * t * q[4];
    *y = u * u * q[1] + 2 * t * u * q[3] + t * t * q[5];
}

static void yol__cv_eval(const yol__cv* v, double t, double* px, double* py, double* tx, double* ty) {
    double u = 1 - t, bx, by;
    double dx = 2 * (u * (v->x1 - v->x0) + t * (v->x2 - v->x1)), dy = 2 * (u * (v->y1 - v->y0) + t * (v->y2 - v->y1));
    yol__bz(&v->x0, t, &bx, &by);
    double L = sqrt(dx * dx + dy * dy), nx, ny, r;
    if (L == 0) { *px = bx; *py = by; *tx = v->x2 - v->x0; *ty = v->y2 - v->y0; return; }
    nx = -dy / L; ny = dx / L;
    /* at a smooth join both segments offset along one normal, so their
     * bands' end lines are the same bits and cancel (yol__cancel) */
    if (t == 0 && v->has0 && !v->evolute) { nx = v->n0x; ny = v->n0y; }
    else if (t == 1 && v->has1 && !v->evolute) { nx = v->n1x; ny = v->n1y; }
    if (v->evolute) {
        double ddx = 2 * (v->x2 - 2 * v->x1 + v->x0), ddy = 2 * (v->y2 - 2 * v->y1 + v->y0), cr = dx * ddy - dy * ddx;
        r = L * L * L / cr;
        *px = bx + r * nx; *py = by + r * ny; *tx = nx; *ty = ny;
    } else {
        *px = bx + v->s * nx; *py = by + v->s * ny; *tx = dx; *ty = dy;
    }
}

/* Distance from (px, py) to the quadratic Q, by Newton from u. An
 * overestimate when Newton stops early, which only splits more. */
static double yol__dist_quad(const double Q[6], double px, double py, double u) {
    int it;
    double best = HUGE_VAL;
    for (it = 0; it < 8; it++) {
        double w = 1 - u;
        double x = w * w * Q[0] + 2 * u * w * Q[2] + u * u * Q[4], y = w * w * Q[1] + 2 * u * w * Q[3] + u * u * Q[5];
        double dx = 2 * (w * (Q[2] - Q[0]) + u * (Q[4] - Q[2])), dy = 2 * (w * (Q[3] - Q[1]) + u * (Q[5] - Q[3]));
        double ddx = 2 * (Q[4] - 2 * Q[2] + Q[0]), ddy = 2 * (Q[5] - 2 * Q[3] + Q[1]);
        double f = (x - px) * dx + (y - py) * dy, fp = dx * dx + dy * dy + (x - px) * ddx + (y - py) * ddy;
        double dd = sqrt((x - px) * (x - px) + (y - py) * (y - py));
        if (dd < best) best = dd;
        if (!(fp > 0)) break;
        u -= f / fp;
        if (u < 0) u = 0;
        if (u > 1) u = 1;
    }
    return best;
}

/* Quadratics within tol along the curve from t = a to t = b, appended to the
 * element (its current point is the curve at a). Accepts the quadratic
 * through the ends whose control is the tangents' intersection when five
 * samples of the curve lie within tol of it; else splits. Bounded by depth
 * and by the stroke's call budget: YOL_ERR_NUMERIC, never a hang. */
static int yol__approx(yol__sk* s, const yol__cv* v, double a, double b, int depth) {
    double ax, ay, atx, aty, bx, by, btx, bty, chord, den, Q[6];
    int ok = 0, k;
    if (--s->budget < 0 || depth > 30) {
        yol__cv_eval(v, a, &ax, &ay, &atx, &aty);
        return yol__err(s->c, YOL_ERR_NUMERIC, "stroke: an %s did not converge near (%.9g, %.9g)", v->evolute ? "evolute" : "offset", ax, ay);
    }
    yol__cv_eval(v, a, &ax, &ay, &atx, &aty);
    yol__cv_eval(v, b, &bx, &by, &btx, &bty);
    chord = sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
    if (chord <= 1e-12 * (fabs(ax) + fabs(ay) + s->h)) { yol_line(s->e, bx, by); return s->e->status; }
    den = atx * bty - aty * btx;
    Q[0] = ax; Q[1] = ay; Q[4] = bx; Q[5] = by;
    if (fabs(den) > 1e-9 * sqrt((atx * atx + aty * aty) * (btx * btx + bty * bty))) {
        double u = ((bx - ax) * bty - (by - ay) * btx) / den;
        Q[2] = ax + u * atx; Q[3] = ay + u * aty;
        ok = sqrt((Q[2] - 0.5 * (ax + bx)) * (Q[2] - 0.5 * (ax + bx)) + (Q[3] - 0.5 * (ay + by)) * (Q[3] - 0.5 * (ay + by))) <= 2 * chord;
    } else { Q[2] = ax; Q[3] = ay; ok = 1; }   /* parallel ends: a line, if the samples agree */
    if (ok) {
        double err = 0;
        for (k = 1; k <= 5 && err <= s->tol; k++) {
            double px, py, tx, ty;
            yol__cv_eval(v, a + (b - a) * k / 6.0, &px, &py, &tx, &ty);
            err = fmax(err, yol__dist_quad(Q, px, py, k / 6.0));
        }
        if (err <= s->tol) {
            if (Q[2] == ax && Q[3] == ay) yol_line(s->e, bx, by);
            else yol_quad(s->e, Q[2], Q[3], bx, by);
            return s->e->status;
        }
    }
    {
        int rc = yol__approx(s, v, a, 0.5 * (a + b), depth + 1);
        if (rc < 0) return rc;
        return yol__approx(s, v, 0.5 * (a + b), b, depth + 1);
    }
}

static int yol__disk(yol__sk* s, double cx, double cy) {
    int n, i;
    double r = s->h;
    yol_path* e = s->e;
    n = (int)ceil(2 * YOL__PI / yol__arc_step(s->tol / r));
    if (n < 4) n = 4;
    yol_move(e, cx + r, cy);
    for (i = 0; i < n; i++) {
        double a0 = 2 * YOL__PI * i / n, a1 = 2 * YOL__PI * (i + 1) / n, am = 0.5 * (a0 + a1), k = r / cos(0.5 * (a1 - a0));
        if (i == n - 1) yol_quad(e, cx + k * cos(am), cy + k * sin(am), cx + r, cy);
        else yol_quad(e, cx + k * cos(am), cy + k * sin(am), cx + r * cos(a1), cy + r * sin(a1));
    }
    return yol__elem_end(s);
}

/* The part of the disk at (cx, cy) that no band covers: the sector between
 * the unit normals a and b that contains the direction m (the vertex's
 * normal cone). A point within h of the vertex whose nearest point on the
 * path is the vertex lies in it; every other point within h of the path is
 * in a band or another vertex's sector, so bands and sectors are the round
 * stroke exactly. */
static int yol__sector(yol__sk* s, double cx, double cy, double ax, double ay, double bx, double by, double mx, double my) {
    double r = s->h, a0 = atan2(ay, ax), dt = atan2(ax * by - ay * bx, ax * bx + ay * by), am, step;
    int n, i;
    yol_path* e = s->e;
    am = a0 + 0.5 * dt;
    if (cos(am) * mx + sin(am) * my < 0) dt = dt > 0 ? dt - 2 * YOL__PI : dt + 2 * YOL__PI;
    if (fabs(dt) < 1e-12) return YOL_OK;
    step = yol__arc_step(s->tol / r);
    n = (int)ceil(fabs(dt) / step);
    if (n < 1) n = 1;
    yol_move(e, cx, cy);
    yol_line(e, cx + r * ax, cy + r * ay);
    for (i = 0; i < n; i++) {
        double a1 = a0 + dt * (i + 1) / n, ah = a0 + dt * (i + 0.5) / n, k = r / cos(0.5 * dt / n);
        if (i == n - 1) yol_quad(e, cx + k * cos(ah), cy + k * sin(ah), cx + r * bx, cy + r * by);
        else yol_quad(e, cx + k * cos(ah), cy + k * sin(ah), cx + r * cos(a1), cy + r * sin(a1));
    }
    return yol__elem_end(s);
}

static int yol__poly(yol__sk* s, const double* xy, int n) {
    int i;
    yol_move(s->e, xy[0], xy[1]);
    for (i = 1; i < n; i++) yol_line(s->e, xy[2 * i], xy[2 * i + 1]);
    return yol__elem_end(s);
}

/* The band of a straight segment: a rectangle; n0, n1 (may be NULL) are
 * shared unit normals at its ends. */
static int yol__stroke_line(yol__sk* s, double x0, double y0, double x1, double y1, const double* n0, const double* n1) {
    double dx = x1 - x0, dy = y1 - y0, L = sqrt(dx * dx + dy * dy), ax, ay, bx, by, r[12];
    if (L == 0) return YOL_OK;
    ax = bx = -dy / L; ay = by = dx / L;
    if (n0) { ax = n0[0]; ay = n0[1]; }
    if (n1) { bx = n1[0]; by = n1[1]; }
    ax *= s->h; ay *= s->h; bx *= s->h; by *= s->h;
    /* the end lines pass through the segment's ends, where they meet a
     * join's spokes or the next band's end line, the same bits reversed */
    r[0] = x0 + ax; r[1] = y0 + ay; r[2] = x1 + bx; r[3] = y1 + by; r[4] = x1; r[5] = y1;
    r[6] = x1 - bx; r[7] = y1 - by; r[8] = x0 - ax; r[9] = y0 - ay; r[10] = x0; r[11] = y0;
    return yol__poly(s, r, 6);
}

/* The elements of one quadratic (EXACTNESS, STROKES): split where the
 * curvature radius is h and at the vertex; where the radius is at least h a
 * full band (two offsets), else the outer half band, the segment-to-evolute
 * part and the evolute-to-offset part, each positive. */
static int yol__stroke_quad(yol__sk* s, const double* q, const double* n0, const double* n1) {
    double d0x = 2 * (q[2] - q[0]), d0y = 2 * (q[3] - q[1]), ddx = 2 * (q[4] - 2 * q[2] + q[0]), ddy = 2 * (q[5] - 2 * q[3] + q[1]);
    double cr = d0x * ddy - d0y * ddx, ts[16], A, B, C, h = s->h;
    int nt = 0, i, rc, side;
    yol__cv po, pi, ev;
    A = ddx * ddx + ddy * ddy;
    ts[nt++] = 0;
    {
        double K = pow(h * fabs(cr), 2.0 / 3.0), disc;
        B = 2 * (d0x * ddx + d0y * ddy); C = d0x * d0x + d0y * d0y - K;
        disc = B * B - 4 * A * C;
        if (A > 0 && disc > 0) {
            double sq = sqrt(disc), r1 = (-B - sq) / (2 * A), r2 = (-B + sq) / (2 * A);
            if (r1 > 0 && r1 < 1) ts[nt++] = r1;
            if (r2 > 0 && r2 < 1) ts[nt++] = r2;
        }
        if (A > 0) { double tv = -(d0x * ddx + d0y * ddy) / A; if (tv > 0 && tv < 1) ts[nt++] = tv; }
    }
    /* where the tangent has turned by each 30 degrees: no approximated piece
     * turns more, so five samples see its shape. A near cusp turns almost
     * 180 degrees in a tiny range of t, which samples spaced in t miss. */
    {
        double d1x = d0x + ddx, d1y = d0y + ddy, a0 = atan2(d0y, d0x), turn = atan2(d0x * d1y - d0y * d1x, d0x * d1x + d0y * d1y);
        int kk, nk = (int)ceil(fabs(turn) / (YOL__PI / 6));
        for (kk = 1; kk < nk && nt < 14; kk++) {
            double th = a0 + turn * kk / nk, ux = cos(th), uy = sin(th), den = ddx * uy - ddy * ux;
            if (den != 0) { double t = -(d0x * uy - d0y * ux) / den; if (t > 0 && t < 1) ts[nt++] = t; }
        }
    }
    ts[nt++] = 1;
    for (i = 1; i < nt; i++) { double t = ts[i]; int k = i; while (k > 0 && ts[k - 1] > t) { ts[k] = ts[k - 1]; k--; } ts[k] = t; }
    side = cr > 0 ? 1 : -1;      /* the center of curvature is at B + rho N, on this side */
    memset(&po, 0, sizeof po);
    po.x0 = q[0]; po.y0 = q[1]; po.x1 = q[2]; po.y1 = q[3]; po.x2 = q[4]; po.y2 = q[5];
    if (n0) { po.has0 = 1; po.n0x = n0[0]; po.n0y = n0[1]; }
    if (n1) { po.has1 = 1; po.n1x = n1[0]; po.n1y = n1[1]; }
    pi = po; ev = po;
    po.s = -side * h; pi.s = side * h; ev.evolute = 1;
    for (i = 0; i + 1 < nt; i++) {
        double a = ts[i], b = ts[i + 1], m = 0.5 * (a + b), px, py, tx, ty, L, rho;
        if (b - a <= 1e-12) continue;
        tx = d0x + m * ddx; ty = d0y + m * ddy;
        L = sqrt(tx * tx + ty * ty);
        rho = L * L * L / fabs(cr);
        if (rho >= h) {
            /* full band: outer offset a->b, inner offset b->a; the end
             * lines through the segment's points, as the line band's */
            double ex, ey;
            yol__cv_eval(&po, a, &px, &py, &tx, &ty); yol_move(s->e, px, py);
            rc = yol__approx(s, &po, a, b, 0); if (rc < 0) return rc;
            yol__bz(q, b, &ex, &ey); yol_line(s->e, ex, ey);
            yol__cv_eval(&pi, b, &px, &py, &tx, &ty); yol_line(s->e, px, py);
            rc = yol__approx(s, &pi, b, a, 0); if (rc < 0) return rc;
            yol__bz(q, a, &ex, &ey); yol_line(s->e, ex, ey);
            rc = yol__elem_end(s); if (rc < 0) return rc;
        } else {
            /* the segment's ends and control on [a, b], computed once, so
             * its two copies (b->a, a->b) are reverses bit for bit */
            double ax, ay, bx, by, cu = (1 - a) * (1 - b), cv = (1 - a) * b + a * (1 - b), cw = a * b;
            double ccx = cu * q[0] + cv * q[2] + cw * q[4], ccy = cu * q[1] + cv * q[3] + cw * q[5];
            yol__bz(q, a, &ax, &ay); yol__bz(q, b, &bx, &by);
            /* outer half band: outer offset a->b, the segment b->a */
            yol__cv_eval(&po, a, &px, &py, &tx, &ty); yol_move(s->e, px, py);
            rc = yol__approx(s, &po, a, b, 0); if (rc < 0) return rc;
            yol_line(s->e, bx, by);
            yol_quad(s->e, ccx, ccy, ax, ay);
            rc = yol__elem_end(s); if (rc < 0) return rc;
            /* the segment a->b, the evolute b->a */
            yol_move(s->e, ax, ay);
            yol_quad(s->e, ccx, ccy, bx, by);
            yol__cv_eval(&ev, b, &px, &py, &tx, &ty); yol_line(s->e, px, py);
            rc = yol__approx(s, &ev, b, a, 0); if (rc < 0) return rc;
            rc = yol__elem_end(s); if (rc < 0) return rc;
            /* the evolute a->b, the inner offset b->a */
            yol__cv_eval(&ev, a, &px, &py, &tx, &ty); yol_move(s->e, px, py);
            rc = yol__approx(s, &ev, a, b, 0); if (rc < 0) return rc;
            yol__cv_eval(&pi, b, &px, &py, &tx, &ty); yol_line(s->e, px, py);
            rc = yol__approx(s, &pi, b, a, 0); if (rc < 0) return rc;
            rc = yol__elem_end(s); if (rc < 0) return rc;
        }
    }
    return YOL_OK;
}

/* A join at (vx, vy) between unit tangents t1 (in) and t2 (out). */
static int yol__join(yol__sk* s, const yol_stroke_desc* d, double vx, double vy, double t1x, double t1y, double t2x, double t2y) {
    double cr = t1x * t2y - t1y * t2x, dot = t1x * t2x + t1y * t2y, h = s->h, so, n1x, n1y, n2x, n2y, r[8];
    int rc;
    if (fabs(cr) <= 1e-12 && dot > 0) return YOL_OK;
    n1x = -t1y; n1y = t1x; n2x = -t2y; n2y = t2x;
    so = cr > 0 ? -1.0 : 1.0;      /* the outer side */
    if (d->join == YOL_JOIN_ROUND)
        return yol__sector(s, vx, vy, so * n1x, so * n1y, so * n2x, so * n2y, t1x - t2x, t1y - t2y);
    /* the inner join: the triangle the two bands leave between them */
    r[0] = vx; r[1] = vy; r[2] = vx - (so * h) * n1x; r[3] = vy - (so * h) * n1y; r[4] = vx - (so * h) * n2x; r[5] = vy - (so * h) * n2y;
    if (fabs(cr) > 1e-12) { rc = yol__poly(s, r, 3); if (rc < 0) return rc; }
    r[2] = vx + (so * h) * n1x; r[3] = vy + (so * h) * n1y; r[4] = vx + (so * h) * n2x; r[5] = vy + (so * h) * n2y;
    if (d->join == YOL_JOIN_MITER && 1 + dot > 1e-12) {
        double ratio = sqrt(2.0 / (1 + dot)), lim = d->miter_limit > 0 ? d->miter_limit : 4.0;
        if (ratio <= lim) {
            double mx = vx + so * h * (n1x + n2x) / (1 + dot), my = vy + so * h * (n1y + n2y) / (1 + dot);
            r[6] = r[4]; r[7] = r[5]; r[4] = mx; r[5] = my;
            return yol__poly(s, r, 4);
        }
    }
    if (fabs(cr) <= 1e-12) return YOL_OK;    /* a 180-degree bevel has no area */
    return yol__poly(s, r, 3);
}

/* A cap at (x, y) with the unit tangent pointing out of the path. */
static int yol__cap(yol__sk* s, int cap, double x, double y, double tx, double ty) {
    double h = s->h, nx = -ty * h, ny = tx * h, r[10];
    if (cap == YOL_CAP_ROUND) return yol__sector(s, x, y, -ty, tx, ty, -tx, tx, ty);
    if (cap != YOL_CAP_SQUARE) return YOL_OK;
    /* through the end point, where the band's end line is split */
    r[0] = x + nx; r[1] = y + ny; r[2] = x + nx + h * tx; r[3] = y + ny + h * ty;
    r[4] = x - nx + h * tx; r[5] = y - ny + h * ty; r[6] = x - nx; r[7] = y - ny; r[8] = x; r[9] = y;
    return yol__poly(s, r, 5);
}

static void yol__unit(double* x, double* y) { double L = sqrt(*x * *x + *y * *y); if (L > 0) { *x /= L; *y /= L; } }

/* A segment's unit tangents at its start and end. 0 for a point. */
static int yol__tangents(const double* q, double* t0x, double* t0y, double* t1x, double* t1y) {
    *t0x = q[2] - q[0]; *t0y = q[3] - q[1];
    if (*t0x == 0 && *t0y == 0) { *t0x = q[4] - q[0]; *t0y = q[5] - q[1]; }
    *t1x = q[4] - q[2]; *t1y = q[5] - q[3];
    if (*t1x == 0 && *t1y == 0) { *t1x = q[4] - q[0]; *t1y = q[5] - q[1]; }
    if ((*t0x == 0 && *t0y == 0) || (*t1x == 0 && *t1y == 0)) return 0;
    yol__unit(t0x, t0y); yol__unit(t1x, t1y);
    return 1;
}

/* A kept segment of a contour: its points, unit tangents at both ends, and
 * whether it is a line. */
typedef struct yol__ks { double q[6], t0x, t0y, t1x, t1y; int line, pad; } yol__ks;

static int yol__smooth(double ax, double ay, double bx, double by) { return fabs(ax * by - ay * bx) <= 1e-9 && ax * bx + ay * by > 0; }

static void yol__ks_set(yol__ks* k, const double* q, int line) {
    memcpy(k->q, q, 6 * sizeof(double));
    yol__tangents(q, &k->t0x, &k->t0y, &k->t1x, &k->t1y);
    k->line = line; k->pad = 0;
}

static int yol__stroke_contour(yol__sk* s, const yol_stroke_desc* d, const yol_path* p, int32_t k) {
    int32_t f = yol__cfirst(p, k), e = yol__cend(p, k), i;
    int open = (p->contours[k] & YOL_OPEN) != 0, rc;
    const double* q = p->pts;
    int32_t nraw = (e - f - 1) / 2 + 1, n = 0, j;
    yol__ks* ks;
    if (e - f < 1) return YOL_OK;
    /* pass 1: the segments that are not points; a straight quadratic that
     * turns back becomes two lines through its turning point */
    ks = (yol__ks*)yol__scratch(s->c, YOL__S_STROKE, (size_t)(2 * nraw + 2) * sizeof(yol__ks));
    if (!ks) return YOL_ERR_FULL;
    for (i = 0; i < nraw; i++) {
        double sg[6], t0x, t0y, t1x, t1y, cr, scale;
        const double* sp;
        if (i < nraw - 1) sp = q + 2 * (f + 2 * i);
        else {
            /* a closed contour's last point is its first; an open contour has
             * no closing segment */
            if (open) break;
            if (q[2 * (e - 1)] == q[2 * f] && q[2 * (e - 1) + 1] == q[2 * f + 1]) break;
            sg[0] = q[2 * (e - 1)]; sg[1] = q[2 * (e - 1) + 1]; sg[2] = sg[0]; sg[3] = sg[1]; sg[4] = q[2 * f]; sg[5] = q[2 * f + 1];
            sp = sg;
        }
        if (!yol__tangents(sp, &t0x, &t0y, &t1x, &t1y)) continue;
        cr = (sp[2] - sp[0]) * (sp[5] - sp[3]) - (sp[3] - sp[1]) * (sp[4] - sp[2]);
        scale = fabs(sp[4] - sp[0]) + fabs(sp[5] - sp[1]) + fabs(sp[2] - sp[0]) + fabs(sp[3] - sp[1]);
        if (!(sp[2] == sp[0] && sp[3] == sp[1]) && fabs(cr) <= 1e-12 * scale * scale) {
            double ax = sp[2] - sp[0], ay = sp[3] - sp[1], bx = sp[4] - 2 * sp[2] + sp[0], by = sp[5] - 2 * sp[3] + sp[1];
            double bb = bx * bx + by * by, tt = bb > 0 ? -(ax * bx + ay * by) / bb : -1, a2[6];
            if (tt > 0 && tt < 1) {
                double mx = (1 - tt) * (1 - tt) * sp[0] + 2 * tt * (1 - tt) * sp[2] + tt * tt * sp[4];
                double my = (1 - tt) * (1 - tt) * sp[1] + 2 * tt * (1 - tt) * sp[3] + tt * tt * sp[5];
                a2[0] = sp[0]; a2[1] = sp[1]; a2[2] = sp[0]; a2[3] = sp[1]; a2[4] = mx; a2[5] = my;
                if (mx != sp[0] || my != sp[1]) yol__ks_set(&ks[n++], a2, 1);
                a2[0] = mx; a2[1] = my; a2[2] = mx; a2[3] = my; a2[4] = sp[4]; a2[5] = sp[5];
                if (mx != sp[4] || my != sp[5]) yol__ks_set(&ks[n++], a2, 1);
            } else {
                a2[0] = sp[0]; a2[1] = sp[1]; a2[2] = sp[0]; a2[3] = sp[1]; a2[4] = sp[4]; a2[5] = sp[5];
                yol__ks_set(&ks[n++], a2, 1);
            }
            continue;
        }
        yol__ks_set(&ks[n++], sp, sp[2] == sp[0] && sp[3] == sp[1]);
    }
    if (n == 0) {
        /* a zero-length subpath: a dot with round or square caps (SVG 2) */
        double x = q[2 * f], y = q[2 * f + 1];
        if (d->cap == YOL_CAP_ROUND) return yol__disk(s, x, y);
        if (d->cap == YOL_CAP_SQUARE) return yol__cap(s, YOL_CAP_SQUARE, x - 0.5 * s->h, y, 1, 0);
        return YOL_OK;
    }
    /* pass 2: bands with shared normals at smooth joins, joins elsewhere */
    for (j = 0; j < n; j++) {
        const yol__ks* a = &ks[j];
        const yol__ks* pv = j > 0 ? &ks[j - 1] : (open ? NULL : &ks[n - 1]);
        const yol__ks* nx = j + 1 < n ? &ks[j + 1] : (open ? NULL : &ks[0]);
        double n0[2], n1[2];
        const double *pn0 = NULL, *pn1 = NULL;
        int sm0 = 0;
        /* the bands' end normals: shared at a smooth join; at a corner each
         * segment's own, the bits the join's spokes use too */
        n0[0] = -a->t0y; n0[1] = a->t0x; n1[0] = -a->t1y; n1[1] = a->t1x;
        pn0 = n0; pn1 = n1;
        if (pv && yol__smooth(pv->t1x, pv->t1y, a->t0x, a->t0y)) {
            double sx = pv->t1x + a->t0x, sy = pv->t1y + a->t0y, L = sqrt(sx * sx + sy * sy);
            n0[0] = -sy / L; n0[1] = sx / L; sm0 = 1;
        }
        if (nx && yol__smooth(a->t1x, a->t1y, nx->t0x, nx->t0y)) {
            double sx = a->t1x + nx->t0x, sy = a->t1y + nx->t0y, L = sqrt(sx * sx + sy * sy);
            n1[0] = -sy / L; n1[1] = sx / L;
        }
        if (j > 0 && !sm0) { rc = yol__join(s, d, a->q[0], a->q[1], pv->t1x, pv->t1y, a->t0x, a->t0y); if (rc < 0) return rc; }
        rc = a->line ? yol__stroke_line(s, a->q[0], a->q[1], a->q[4], a->q[5], pn0, pn1) : yol__stroke_quad(s, a->q, pn0, pn1);
        if (rc < 0) return rc;
    }
    if (open) {
        rc = yol__cap(s, d->cap, ks[0].q[0], ks[0].q[1], -ks[0].t0x, -ks[0].t0y);
        if (rc < 0) return rc;
        return yol__cap(s, d->cap, ks[n - 1].q[4], ks[n - 1].q[5], ks[n - 1].t1x, ks[n - 1].t1y);
    }
    if (yol__smooth(ks[n - 1].t1x, ks[n - 1].t1y, ks[0].t0x, ks[0].t0y)) return YOL_OK;
    return yol__join(s, d, ks[0].q[0], ks[0].q[1], ks[n - 1].t1x, ks[n - 1].t1y, ks[0].t0x, ks[0].t0y);
}

/* Element edges that are exact reverses of each other cancel: the sum of
 * the elements' boundaries keeps its winding everywhere (Nehab 2020 writes
 * his stroke as this sum) and gives the sweep fewer curves. What is left is
 * a balanced set of loose edges. */
typedef struct yol__er { double k[6]; int32_t line, dir; } yol__er;

static int yol__er_lt(const yol__er* a, const yol__er* b) {
    int i;
    if (a->line != b->line) return a->line < b->line;
    for (i = 0; i < 6; i++) if (a->k[i] != b->k[i]) return a->k[i] < b->k[i];
    return 0;
}

static int yol__cancel(yol_ctx* c, yol_path* e) {
    int32_t k, i, n = 0, m;
    yol__er* r;
    int32_t* ix;
    int rc;
    for (k = 0; k < e->n_contours; k++) n += (yol__cend(e, k) - yol__cfirst(e, k) - 1) / 2;
    r = (yol__er*)yol__scratch(c, YOL__S_STROKE2, (size_t)n * (sizeof(yol__er) + sizeof(int32_t)) + 16);
    if (!r) return YOL_ERR_FULL;
    ix = (int32_t*)(void*)(r + n);
    for (k = 0, m = 0; k < e->n_contours; k++) {
        int32_t f = yol__cfirst(e, k), en = yol__cend(e, k);
        for (i = f; i + 2 < en; i += 2) {
            const double* q = e->pts + 2 * i;
            int line = q[2] == q[0] && q[3] == q[1];
            int fwd = q[0] < q[4] || (q[0] == q[4] && q[1] <= q[5]);
            yol__er* t = &r[m];
            /* the key: the lower end first; a line's control is not in it */
            t->k[0] = fwd ? q[0] : q[4]; t->k[1] = fwd ? q[1] : q[5];
            t->k[2] = line ? 0 : q[2]; t->k[3] = line ? 0 : q[3];
            t->k[4] = fwd ? q[4] : q[0]; t->k[5] = fwd ? q[5] : q[1];
            t->line = line; t->dir = fwd ? 1 : -1;
            ix[m] = m; m++;
        }
    }
    {   /* heapsort of the indices by key */
        int32_t a2, root, ch, t;
        for (a2 = m / 2; a2-- > 0;) {
            root = a2; t = ix[root];
            for (;;) { ch = 2 * root + 1; if (ch >= m) break; if (ch + 1 < m && yol__er_lt(&r[ix[ch]], &r[ix[ch + 1]])) ch++; if (!yol__er_lt(&r[t], &r[ix[ch]])) break; ix[root] = ix[ch]; root = ch; }
            ix[root] = t;
        }
        for (a2 = m; a2-- > 1;) {
            t = ix[a2]; ix[a2] = ix[0]; root = 0;
            for (;;) { ch = 2 * root + 1; if (ch >= a2) break; if (ch + 1 < a2 && yol__er_lt(&r[ix[ch]], &r[ix[ch + 1]])) ch++; if (!yol__er_lt(&r[t], &r[ix[ch]])) break; ix[root] = ix[ch]; root = ch; }
            ix[root] = t;
        }
    }
    /* runs of equal keys: their summed direction survives */
    e->n_pts = 0; e->n_contours = 0; e->in_contour = 0;
    for (i = 0; i < m;) {
        int32_t j = i, sum = 0, cnt;
        while (j < m && !yol__er_lt(&r[ix[i]], &r[ix[j]]) && !yol__er_lt(&r[ix[j]], &r[ix[i]])) { sum += r[ix[j]].dir; j++; }
        for (cnt = 0; cnt < (sum < 0 ? -sum : sum); cnt++) {
            const yol__er* t = &r[ix[i]];
            double* o;
            rc = yol__path_reserve(e, 3, 1);
            if (rc < 0) return rc;
            e->contours[e->n_contours++] = (uint32_t)e->n_pts;
            o = e->pts + 2 * e->n_pts;
            if (sum > 0) { o[0] = t->k[0]; o[1] = t->k[1]; o[4] = t->k[4]; o[5] = t->k[5]; }
            else { o[0] = t->k[4]; o[1] = t->k[5]; o[4] = t->k[0]; o[5] = t->k[1]; }
            if (t->line) { o[2] = o[0]; o[3] = o[1]; } else { o[2] = t->k[2]; o[3] = t->k[3]; }
            e->n_pts += 3;
        }
        i = j;
    }
    e->flags |= YOL__PATH_EDGES;
    return YOL_OK;
}

static int yol__stroke(yol_ctx* c, const yol_path* in, yol_path* out, const yol_stroke_desc* d) {
    yol__sk s;
    yol_path e;
    int32_t k;
    int rc = YOL_OK;
    if (!c || !in || !out || !d) return YOL_ERR_ARG;
    if (in == out) return yol__err(c, YOL_ERR_ARG, "stroke: out must not be in");
    if (!(d->width > 0) || !yol__finite(d->width) || !yol__finite(d->tol) || d->tol < 0 || !yol__finite(d->miter_limit) || d->miter_limit < 0)
        return yol__err(c, YOL_ERR_ARG, "stroke: width must be > 0, tol and miter_limit >= 0, all finite");
    if (d->miter_limit > 0 && d->miter_limit < 1) return yol__err(c, YOL_ERR_RANGE, "stroke: miter_limit below 1");
    if (d->join < YOL_JOIN_ROUND || d->join > YOL_JOIN_BEVEL || d->cap < YOL_CAP_BUTT || d->cap > YOL_CAP_SQUARE ||
        d->mode < YOL_STROKE || d->mode > YOL_INSET) return yol__err(c, YOL_ERR_ARG, "stroke: join, cap or mode out of range");
    if (in->status) return yol__err(c, in->status, "stroke: the path has a builder error");
    yol__tmp_get(c, &e, YOL__S_TQ_PTS, YOL__S_TQ_CON);
    memset(&s, 0, sizeof s);
    s.c = c; s.e = &e; s.h = 0.5 * d->width;
    s.tol = d->tol > 0 ? d->tol : (in->tol > 0 ? in->tol : 1e-4);
    if (s.tol > 0.25 * s.h) s.tol = 0.25 * s.h;
    s.budget = 65536 + 4096L * (long)in->n_pts;
    for (k = 0; k < in->n_contours && rc >= 0; k++) rc = yol__stroke_contour(&s, d, in, k);
    if (rc >= 0) rc = yol_path_end(&e);
    if (rc >= 0) rc = yol__cancel(c, &e);
    if (rc >= 0) {
        if (d->mode == YOL_BOLD) rc = yol__sweep(c, in, in->rule, &e, YOL__OP_OR, out);
        else if (d->mode == YOL_INSET) rc = yol__sweep(c, in, in->rule, &e, YOL__OP_DIFF, out);
        else rc = yol__sweep(c, &e, YOL_NONZERO, NULL, YOL__OP_A, out);
    }
    yol__tmp_put(c, &e, YOL__S_TQ_PTS, YOL__S_TQ_CON);
    if (rc < 0) { out->n_pts = 0; out->n_contours = 0; }
    return rc < 0 ? rc : YOL_OK;
}

/* --- curve sets, format v1 ------------------------------------------------------ */

#define YOL__CSET_MAGIC 0x43505359u
#define YOL__CSET_EVENODD  0x1u
#define YOL__CSET_BACKWARD 0x2u
#define YOL__CSET_RESOLVED 0x4u

static int yol__cset_words(yol_cset* s, uint32_t n) {
    if (s->n_words + (uint64_t)n > 0xfffffff0u) return yol__err(s->ctx, YOL_ERR_FULL, "curve set: more than 2^32 words");
    if (s->n_words + n > s->cap_words) {
        size_t cap = (size_t)s->cap_words * 4;
        void* q = s->words;
        int rc;
        if (!s->ctx) return YOL_ERR_FULL;
        rc = yol__grow(s->ctx, &q, &cap, ((size_t)s->n_words + n) * 4);
        if (rc < 0) return rc;
        s->words = (uint32_t*)q;
        s->cap_words = (uint32_t)(cap / 4 > 0xffffffffu ? 0xffffffffu : cap / 4);
    }
    return YOL_OK;
}
static int yol__cset_texels(yol_cset* s, uint32_t n) {
    if (s->n_texels + (uint64_t)n > 0x0ffffff0u) return yol__err(s->ctx, YOL_ERR_FULL, "curve set: more than 2^28 texels");
    if (s->n_texels + n > s->cap_texels) {
        size_t cap = (size_t)s->cap_texels * 16;
        void* q = s->texels;
        int rc;
        if (!s->ctx) return YOL_ERR_FULL;
        rc = yol__grow(s->ctx, &q, &cap, ((size_t)s->n_texels + n) * 16);
        if (rc < 0) return rc;
        s->texels = (float*)q;
        s->cap_texels = (uint32_t)(cap / 16);
    }
    return YOL_OK;
}

YOL_API bool yol_cset_open(yol_cset* s, yol_ctx* c, const yol_cset_desc* d) {
    uint32_t i, n0 = c ? c->n_err : 0;
    if (!s || !d) { yol__err(c, YOL_ERR_ARG, "curve set: no set or no desc"); return false; }
    if (c) { memset(s, 0, sizeof *s); s->ctx = c; }
    else { s->n_texels = 0; s->n_words = 0; s->ctx = NULL; }
    s->desc = *d;
    if (d->n_glyphs >= (1u << 24)) {
        yol__err(c, YOL_ERR_RANGE, "curve set: %u glyphs; the format allows below 2^24", d->n_glyphs);
        return false;
    }
    if (d->nh < 0 || d->nh > 65535 || d->nv < 0 || d->nv > 65535) {
        yol__err(c, YOL_ERR_RANGE, "curve set: band counts 0..65535");
        return false;
    }
    if (yol__done(c, n0, yol__cset_words(s, 8 + d->n_glyphs + 4), "curve set") < 0) return false;
    s->words[0] = YOL__CSET_MAGIC; s->words[1] = 1; s->words[2] = d->n_glyphs;
    for (i = 3; i < 8 + d->n_glyphs; i++) s->words[i] = 0;
    s->n_words = 8 + d->n_glyphs;
    while (s->n_words % 4) s->words[s->n_words++] = 0;
    return true;
}

YOL_API void yol_cset_close(yol_cset* s) {
    if (!s) return;
    if (s->ctx) {
        if (s->texels) yol__realloc(s->ctx, s->texels, 0);
        if (s->words) yol__realloc(s->ctx, s->words, 0);
        memset(s, 0, sizeof *s);
    }
}

typedef struct yol__cc { double xmin, xmax, ymin, ymax; uint32_t ref, pad; } yol__cc;

static uint32_t yol__f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* round(sqrt(n)) clamped to 1..16, the default band count */
static int yol__bands_auto(uint32_t n) {
    int b = (int)floor(sqrt((double)n) + 0.5);
    return b < 1 ? 1 : (b > 16 ? 16 : b);
}

static int yol__cset_add(yol_cset* s, uint32_t glyph, const yol_path* p) {
    yol_ctx* c;
    yol_path tmp;
    const yol_path* src = p;
    int resolved = 0, rc, nh, nv, b;
    uint32_t first, ncurves = 0, o, k, nlists;
    int32_t ci, i;
    yol__cc* cc;
    float bb[4] = { 0, 0, 0, 0 };
    if (!s || !p || !s->words) return YOL_ERR_ARG;
    c = s->ctx;
    if (glyph >= s->desc.n_glyphs) return yol__err(c, YOL_ERR_RANGE, "curve set: glyph %u of %u", glyph, s->desc.n_glyphs);
    if (s->words[8 + glyph] != 0) return yol__err(c, YOL_ERR_ARG, "curve set: glyph %u is in the set already", glyph);
    if (p->status) return yol__err(c, p->status, "curve set: glyph %u's path has a builder error", glyph);
    if (!c) c = p->ctx;
    if (!s->desc.keep_overlaps) {
        if (!(p->flags & YOL_PATH_RESOLVED)) {
            if (!c) return YOL_ERR_ARG;
            yol__tmp_get(c, &tmp, YOL__S_TP_PTS, YOL__S_TP_CON);
            rc = yol__sweep(c, p, p->rule, NULL, YOL__OP_A, &tmp);
            yol__tmp_put(c, &tmp, YOL__S_TP_PTS, YOL__S_TP_CON);
            if (rc < 0) return rc;
            src = &tmp;
        }
        resolved = 1;
    }
    /* texels: each vertex rounded to f32 once; a curve that is a point in
     * f32 goes; the contour's start closes it. refs collects each curve's
     * first texel. */
    first = s->n_texels;
    {
        uint32_t* refs = NULL;
        size_t refcap = 0;
        for (ci = 0; ci < src->n_contours; ci++) {
            int32_t f = yol__cfirst(src, ci), e = yol__cend(src, ci);
            const double* q = src->pts;
            uint32_t t0 = s->n_texels, kept = 0;
            float sx, sy;
            int closing;
            if (e - f < 3) continue;
            closing = q[2 * (e - 1)] != q[2 * f] || q[2 * (e - 1) + 1] != q[2 * f + 1];
            rc = yol__cset_texels(s, (uint32_t)((e - f) / 2 + 3));
            if (rc < 0) return rc;
            sx = (float)q[2 * f]; sy = (float)q[2 * f + 1];
            for (i = f; i + 2 < e + (closing ? 2 : 0); i += 2) {
                float x0, y0, x1, y1, x2, y2;
                float* t;
                if (i + 2 < e) { x0 = (float)q[2 * i]; y0 = (float)q[2 * i + 1]; x1 = (float)q[2 * i + 2]; y1 = (float)q[2 * i + 3]; x2 = (float)q[2 * i + 4]; y2 = (float)q[2 * i + 5]; }
                else { x0 = (float)q[2 * (e - 1)]; y0 = (float)q[2 * (e - 1) + 1]; x1 = x0; y1 = y0; x2 = sx; y2 = sy; }
                if (x0 == x1 && y0 == y1 && x0 == x2 && y0 == y2) continue;
                if (!kept) { t = s->texels + 4 * s->n_texels++; t[0] = x0; t[1] = y0; }
                t = s->texels + 4 * (s->n_texels - 1);
                t[2] = x1; t[3] = y1;
                if (!c) return YOL_ERR_ARG;
                rc = yol__grow(c, &c->scratch[YOL__S_CSET], &c->scratch_cap[YOL__S_CSET], ((size_t)ncurves + kept + 1) * sizeof(uint32_t));
                if (rc < 0) return rc;
                refs = (uint32_t*)c->scratch[YOL__S_CSET]; refcap = c->scratch_cap[YOL__S_CSET];
                refs[ncurves + kept] = s->n_texels - 1;
                t = s->texels + 4 * s->n_texels++;
                t[0] = x2; t[1] = y2; t[2] = 0; t[3] = 0;
                kept++;
            }
            if (kept == 0) { s->n_texels = t0; continue; }
            ncurves += kept;
        }
        (void)refcap;
        cc = NULL;
        if (ncurves) {
            /* the ranges from the stored f32 values; refs moves to the end */
            uint32_t* rcopy;
            size_t need = (size_t)ncurves * (sizeof(yol__cc) + sizeof(uint32_t));
            cc = (yol__cc*)yol__scratch(c, YOL__S_MISC, need);
            if (!cc) return YOL_ERR_FULL;
            rcopy = (uint32_t*)(void*)(cc + ncurves);
            memcpy(rcopy, c->scratch[YOL__S_CSET], (size_t)ncurves * sizeof(uint32_t));
            for (k = 0; k < ncurves; k++) {
                const float* t = s->texels + 4 * rcopy[k];
                double xa = t[0], ya = t[1], xb = t[2], yb = t[3], xc = t[4], yc = t[5];
                cc[k].xmin = fmin(xa, fmin(xb, xc)); cc[k].xmax = fmax(xa, fmax(xb, xc));
                cc[k].ymin = fmin(ya, fmin(yb, yc)); cc[k].ymax = fmax(ya, fmax(yb, yc));
                cc[k].ref = rcopy[k]; cc[k].pad = 0;
            }
            bb[0] = (float)cc[0].xmin; bb[1] = (float)cc[0].ymin; bb[2] = (float)cc[0].xmax; bb[3] = (float)cc[0].ymax;
            for (k = 1; k < ncurves; k++) {
                if ((float)cc[k].xmin < bb[0]) bb[0] = (float)cc[k].xmin;
                if ((float)cc[k].ymin < bb[1]) bb[1] = (float)cc[k].ymin;
                if ((float)cc[k].xmax > bb[2]) bb[2] = (float)cc[k].xmax;
                if ((float)cc[k].ymax > bb[3]) bb[3] = (float)cc[k].ymax;
            }
        }
    }
    if (ncurves == 0 || !(bb[2] > bb[0]) || !(bb[3] > bb[1])) { s->n_texels = first; ncurves = 0; nh = nv = 0; }
    else {
        nh = s->desc.nh ? s->desc.nh : yol__bands_auto(ncurves);
        nv = s->desc.nv ? s->desc.nv : yol__bands_auto(ncurves);
    }
    /* the record, its descriptors, then the lists */
    nlists = (uint32_t)(nh + nv) * (s->desc.backward ? 2u : 1u);
    while (s->n_words % 4) { rc = yol__cset_words(s, 1); if (rc < 0) return rc; s->words[s->n_words++] = 0; }
    o = s->n_words;
    rc = yol__cset_words(s, 8 + 4 * (uint32_t)(nh + nv) + nlists * ncurves + 4);
    if (rc < 0) return rc;
    s->words[o + 0] = yol__f2u(bb[0]); s->words[o + 1] = yol__f2u(bb[1]);
    s->words[o + 2] = yol__f2u(bb[2]); s->words[o + 3] = yol__f2u(bb[3]);
    s->words[o + 4] = (uint32_t)nh | (uint32_t)nv << 16;
    s->words[o + 5] = (nh && src->rule == YOL_EVENODD ? YOL__CSET_EVENODD : 0u) |
                      (nh && s->desc.backward ? YOL__CSET_BACKWARD : 0u) | (nh && resolved ? YOL__CSET_RESOLVED : 0u);
    s->words[o + 6] = first;
    s->words[o + 7] = s->n_texels - first;
    s->n_words = o + 8 + 4 * (uint32_t)(nh + nv);
    for (k = o + 8; k < s->n_words; k++) s->words[k] = 0;
    {
        yol__kv* kv = ncurves ? (yol__kv*)yol__scratch(c, YOL__S_CSET2, (size_t)ncurves * sizeof(yol__kv)) : NULL;
        int dir;
        if (ncurves && !kv) return YOL_ERR_FULL;
        for (b = 0; b < nh + nv; b++) {
            int vert = b >= nh, kb = vert ? b - nh : b, nb = vert ? nv : nh;
            double lo0 = vert ? bb[0] : bb[1], hi0 = vert ? bb[2] : bb[3], hb = (hi0 - lo0) / nb;
            double lo = lo0 + kb * hb - hb / 256, hi = lo0 + kb * hb + hb + hb / 256;
            for (dir = 0; dir < (s->desc.backward ? 2 : 1); dir++) {
                uint32_t n = 0, start = s->n_words, j;
                for (k = 0; k < ncurves; k++) {
                    double mn = vert ? cc[k].xmin : cc[k].ymin, mx = vert ? cc[k].xmax : cc[k].ymax;
                    if (mx < lo || mn > hi) continue;
                    /* forward: descending max of the other axis; backward:
                     * ascending min; ties by ascending reference */
                    kv[n].k = dir == 0 ? -(vert ? cc[k].ymax : cc[k].xmax) : (vert ? cc[k].ymin : cc[k].xmin);
                    kv[n].v = (int32_t)cc[k].ref; kv[n].pad = 0;
                    n++;
                }
                yol__kv_sort(kv, n);
                for (j = 0; j < n; j++) s->words[s->n_words++] = (uint32_t)kv[j].v;
                s->words[o + 8 + 4 * (uint32_t)b + 2 * (uint32_t)dir] = start;
                s->words[o + 8 + 4 * (uint32_t)b + 2 * (uint32_t)dir + 1] = n;
            }
        }
    }
    while (s->n_words % 4) s->words[s->n_words++] = 0;
    s->words[8 + glyph] = o;
    return YOL_OK;
}

/* --- SVG subset ----------------------------------------------------------------- */
/* SVG: a bounded reader of a small subset. Anything outside it is refused by
 * name, never ignored: an ignored filter or clip would draw a different
 * stimulus than the file shows. Bounds: element depth 64, 64 attributes an
 * element. */

#define YOL__SVG_DEPTH 64
#define YOL__SVG_ATTRS 64

typedef struct yol__sa { const char* name; size_t nlen; const char* val; size_t vlen; } yol__sa;

typedef struct yol__ss {       /* inherited style and the transform */
    yol_xform xf;
    uint32_t fill, stroke;       /* 0xRRGGBB00                                 */
    int fill_on, stroke_on, rule, join, cap;
    double fill_op, stroke_op, sw, miter;
} yol__ss;

typedef struct yol__sv {
    yol_ctx* c;
    const char* s;
    size_t n, i;
    char* err; size_t cap;
    const yol_svg_desc* d;
    int nl, element, have_vb;
    double tol, vb[4];
    yol__sa at[YOL__SVG_ATTRS];
    int nat;
} yol__sv;

static int yol__sverr(yol__sv* v, int code, const char* fmt, ...) {
    va_list ap;
    char msg[200];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    yol__err(v->c, code, "SVG: %s (byte %lu)", msg, (unsigned long)v->i);
    if (v->err && v->cap) snprintf(v->err, v->cap, "%s", v->c->err);
    return code;
}

static int yol__sws(int ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; }
static int yol__salpha(int ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
static int yol__sname(int ch) { return yol__salpha(ch) || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == ':'; }
static int yol__seq(const char* a, size_t n, const char* lit) { size_t k = strlen(lit); return n == k && memcmp(a, lit, k) == 0; }

/* A number of SVG's grammar at s[*i], parsed by hand (strtod follows the
 * locale); 0 when there is none. */
static int yol__snum(const char* s, size_t n, size_t* i, double* out) {
    size_t k = *i;
    double m = 0, frac = 0, scale = 1;
    int neg = 0, digits = 0, e = 0, eneg = 0;
    if (k < n && (s[k] == '+' || s[k] == '-')) { neg = s[k] == '-'; k++; }
    while (k < n && s[k] >= '0' && s[k] <= '9') { m = m * 10 + (s[k] - '0'); k++; digits++; }
    if (k < n && s[k] == '.') {
        k++;
        while (k < n && s[k] >= '0' && s[k] <= '9') { scale *= 0.1; frac += (s[k] - '0') * scale; k++; digits++; }
    }
    if (!digits) return 0;
    if (k + 1 < n && (s[k] == 'e' || s[k] == 'E') && ((s[k + 1] >= '0' && s[k + 1] <= '9') ||
        ((s[k + 1] == '+' || s[k + 1] == '-') && k + 2 < n && s[k + 2] >= '0' && s[k + 2] <= '9'))) {
        k++;
        if (s[k] == '+' || s[k] == '-') { eneg = s[k] == '-'; k++; }
        while (k < n && s[k] >= '0' && s[k] <= '9') { if (e < 400) e = e * 10 + (s[k] - '0'); k++; }
    }
    m += frac;
    if (e) m *= pow(10.0, eneg ? -e : e);
    *out = neg ? -m : m;
    *i = k;
    return yol__finite(*out);
}

static void yol__sskip(const char* s, size_t n, size_t* i) { while (*i < n && (yol__sws(s[*i]) || s[*i] == ',')) (*i)++; }

/* A length: a number, then nothing or px. */
static int yol__slen(yol__sv* v, const char* s, size_t n, const char* what, double* out) {
    size_t i = 0;
    while (i < n && yol__sws(s[i])) i++;
    if (!yol__snum(s, n, &i, out)) return yol__sverr(v, YOL_ERR_FORMAT, "%s is not a number", what);
    if (i + 2 <= n && s[i] == 'p' && s[i + 1] == 'x') i += 2;
    while (i < n && yol__sws(s[i])) i++;
    if (i < n) return yol__sverr(v, YOL_ERR_REFUSED, "%s: a length in units other than px is not supported", what);
    return YOL_OK;
}

static const yol__sa* yol__sattr(const yol__sv* v, const char* name) {
    int k;
    for (k = 0; k < v->nat; k++) if (yol__seq(v->at[k].name, v->at[k].nlen, name)) return &v->at[k];
    return NULL;
}

/* The 17 CSS 2 color names. */
static const struct { const char* name; uint32_t rgb; } yol__scolors[17] = {
    { "black", 0x000000 }, { "silver", 0xc0c0c0 }, { "gray", 0x808080 }, { "white", 0xffffff }, { "maroon", 0x800000 },
    { "red", 0xff0000 }, { "purple", 0x800080 }, { "fuchsia", 0xff00ff }, { "green", 0x008000 }, { "lime", 0x00ff00 },
    { "olive", 0x808000 }, { "yellow", 0xffff00 }, { "navy", 0x000080 }, { "blue", 0x0000ff }, { "teal", 0x008080 },
    { "aqua", 0x00ffff }, { "orange", 0xffa500 } };

static int yol__shex(int ch) { return ch >= '0' && ch <= '9' ? ch - '0' : (ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : (ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1)); }

/* A paint: *on 0 for none; *rgb 0xRRGGBB. 1 for inherit (unchanged). */
static int yol__spaint(yol__sv* v, const yol__sa* a, int* on, uint32_t* rgb) {
    const char* s = a->val;
    size_t n = a->vlen, i = 0, k;
    while (n && yol__sws(s[n - 1])) n--;
    while (i < n && yol__sws(s[i])) i++;
    s += i; n -= i;
    if (yol__seq(s, n, "inherit")) return 1;
    if (yol__seq(s, n, "none")) { *on = 0; return 0; }
    if (n >= 4 && memcmp(s, "url(", 4) == 0) return yol__sverr(v, YOL_ERR_REFUSED, "a url() paint (gradient or pattern) is not supported");
    if (yol__seq(s, n, "currentColor")) return yol__sverr(v, YOL_ERR_REFUSED, "currentColor is not supported");
    if (n == 4 || n == 7) if (s[0] == '#') {
        int h[6], ok = 1;
        for (k = 1; k < n; k++) { h[k - 1] = yol__shex(s[k]); if (h[k - 1] < 0) ok = 0; }
        if (ok) {
            *rgb = n == 4 ? (uint32_t)(h[0] * 17) << 16 | (uint32_t)(h[1] * 17) << 8 | (uint32_t)(h[2] * 17)
                          : (uint32_t)(h[0] * 16 + h[1]) << 16 | (uint32_t)(h[2] * 16 + h[3]) << 8 | (uint32_t)(h[4] * 16 + h[5]);
            *on = 1;
            return 0;
        }
    }
    if (n > 5 && memcmp(s, "rgb(", 4) == 0 && s[n - 1] == ')') {
        size_t j = 4;
        uint32_t c3[3];
        int q;
        for (q = 0; q < 3; q++) {
            double x;
            yol__sskip(s, n - 1, &j);
            if (!yol__snum(s, n - 1, &j, &x)) break;
            if (j < n - 1 && s[j] == '%') { x = x * 255.0 / 100.0; j++; }
            if (x < 0) x = 0;
            if (x > 255) x = 255;
            c3[q] = (uint32_t)floor(x + 0.5);
        }
        yol__sskip(s, n - 1, &j);
        if (q == 3 && j == n - 1) { *rgb = c3[0] << 16 | c3[1] << 8 | c3[2]; *on = 1; return 0; }
    }
    for (k = 0; k < 17; k++) if (yol__seq(s, n, yol__scolors[k].name)) { *rgb = yol__scolors[k].rgb; *on = 1; return 0; }
    return yol__sverr(v, YOL_ERR_REFUSED, "the color '%.*s' is not supported", (int)(n > 40 ? 40 : n), s);
}

static int yol__snumattr(yol__sv* v, const yol__sa* a, double lo, double hi, double* out) {
    size_t i = 0;
    while (i < a->vlen && yol__sws(a->val[i])) i++;
    if (yol__seq(a->val + i, a->vlen - i, "inherit")) return 1;
    if (!yol__snum(a->val, a->vlen, &i, out) || *out < lo || *out > hi) return yol__sverr(v, YOL_ERR_FORMAT, "%.*s is not a number in range", (int)a->nlen, a->name);
    return 0;
}

/* transform="...": the functions composed left to right after t. */
static int yol__sxform(yol__sv* v, const yol__sa* a, yol_xform* t) {
    const char* s = a->val;
    size_t n = a->vlen, i = 0;
    for (;;) {
        double p[6];
        int np = 0;
        size_t nb;
        yol_xform m;
        yol__sskip(s, n, &i);
        if (i >= n) return YOL_OK;
        nb = i;
        while (i < n && yol__salpha(s[i])) i++;
        yol__sskip(s, n, &i);
        if (i >= n || s[i] != '(') return yol__sverr(v, YOL_ERR_FORMAT, "a transform without its '('");
        i++;
        for (;;) {
            yol__sskip(s, n, &i);
            if (i < n && s[i] == ')') { i++; break; }
            if (np == 6 || !yol__snum(s, n, &i, &p[np])) return yol__sverr(v, YOL_ERR_FORMAT, "a transform's arguments");
            np++;
        }
#define YOL__TF(nm) (yol__seq(s + nb, (size_t)(i - nb), nm))
        {
            size_t ne = nb;
            while (ne < n && yol__salpha(s[ne])) ne++;
            if (yol__seq(s + nb, ne - nb, "matrix") && np == 6) { m.a = p[0]; m.b = p[1]; m.c = p[2]; m.d = p[3]; m.e = p[4]; m.f = p[5]; }
            else if (yol__seq(s + nb, ne - nb, "translate") && (np == 1 || np == 2)) m = yol_xf_translate(p[0], np == 2 ? p[1] : 0);
            else if (yol__seq(s + nb, ne - nb, "scale") && (np == 1 || np == 2)) m = yol_xf_scale(p[0], np == 2 ? p[1] : p[0]);
            else if (yol__seq(s + nb, ne - nb, "rotate") && (np == 1 || np == 3)) {
                m = yol_xf_rotate(p[0]);
                if (np == 3) m = yol_xf_mul(yol_xf_translate(p[1], p[2]), yol_xf_mul(m, yol_xf_translate(-p[1], -p[2])));
            }
            else if (yol__seq(s + nb, ne - nb, "skewX") && np == 1) m = yol_xf_skew(p[0], 0);
            else if (yol__seq(s + nb, ne - nb, "skewY") && np == 1) m = yol_xf_skew(0, p[0]);
            else return yol__sverr(v, YOL_ERR_FORMAT, "the transform '%.*s' with %d arguments", (int)(ne - nb > 20 ? 20 : ne - nb), s + nb, np);
        }
#undef YOL__TF
        *t = yol_xf_mul(*t, m);
    }
}

/* The element's attributes into the style; refuses the ones that would
 * change the picture in a way this reader cannot draw. */
static int yol__sstyle(yol__sv* v, yol__ss* st, double* opacity, int shape) {
    static const char* const refused[] = { "style", "class", "clip-path", "mask", "filter", "stroke-dasharray", "vector-effect",
                                           "marker", "marker-start", "marker-mid", "marker-end", "display", "visibility",
                                           "paint-order", "transform-origin", "requiredFeatures", "systemLanguage" };
    int k, rc;
    size_t q;
    *opacity = 1;
    for (k = 0; k < v->nat; k++) {
        const yol__sa* a = &v->at[k];
        for (q = 0; q < sizeof refused / sizeof refused[0]; q++)
            if (yol__seq(a->name, a->nlen, refused[q])) return yol__sverr(v, YOL_ERR_REFUSED, "the attribute %s is not supported", refused[q]);
        if (yol__seq(a->name, a->nlen, "fill")) { rc = yol__spaint(v, a, &st->fill_on, &st->fill); if (rc < 0) return rc; }
        else if (yol__seq(a->name, a->nlen, "stroke")) { rc = yol__spaint(v, a, &st->stroke_on, &st->stroke); if (rc < 0) return rc; }
        else if (yol__seq(a->name, a->nlen, "fill-rule")) {
            if (yol__seq(a->val, a->vlen, "evenodd")) st->rule = YOL_EVENODD;
            else if (yol__seq(a->val, a->vlen, "nonzero")) st->rule = YOL_NONZERO;
            else if (!yol__seq(a->val, a->vlen, "inherit")) return yol__sverr(v, YOL_ERR_FORMAT, "fill-rule '%.*s'", (int)(a->vlen > 20 ? 20 : a->vlen), a->val);
        }
        else if (yol__seq(a->name, a->nlen, "stroke-linejoin")) {
            if (yol__seq(a->val, a->vlen, "miter")) st->join = YOL_JOIN_MITER;
            else if (yol__seq(a->val, a->vlen, "round")) st->join = YOL_JOIN_ROUND;
            else if (yol__seq(a->val, a->vlen, "bevel")) st->join = YOL_JOIN_BEVEL;
            else if (!yol__seq(a->val, a->vlen, "inherit")) return yol__sverr(v, YOL_ERR_REFUSED, "stroke-linejoin '%.*s' is not supported", (int)(a->vlen > 20 ? 20 : a->vlen), a->val);
        }
        else if (yol__seq(a->name, a->nlen, "stroke-linecap")) {
            if (yol__seq(a->val, a->vlen, "butt")) st->cap = YOL_CAP_BUTT;
            else if (yol__seq(a->val, a->vlen, "round")) st->cap = YOL_CAP_ROUND;
            else if (yol__seq(a->val, a->vlen, "square")) st->cap = YOL_CAP_SQUARE;
            else if (!yol__seq(a->val, a->vlen, "inherit")) return yol__sverr(v, YOL_ERR_FORMAT, "stroke-linecap '%.*s'", (int)(a->vlen > 20 ? 20 : a->vlen), a->val);
        }
        else if (yol__seq(a->name, a->nlen, "stroke-width")) { rc = yol__slen(v, a->val, a->vlen, "stroke-width", &st->sw); if (rc < 0) return rc; if (st->sw < 0) return yol__sverr(v, YOL_ERR_FORMAT, "a negative stroke-width"); }
        else if (yol__seq(a->name, a->nlen, "stroke-miterlimit")) { rc = yol__snumattr(v, a, 1, 1e9, &st->miter); if (rc < 0) return rc; }
        else if (yol__seq(a->name, a->nlen, "fill-opacity")) { rc = yol__snumattr(v, a, -1e9, 1e9, &st->fill_op); if (rc < 0) return rc; st->fill_op = st->fill_op < 0 ? 0 : (st->fill_op > 1 ? 1 : st->fill_op); }
        else if (yol__seq(a->name, a->nlen, "stroke-opacity")) { rc = yol__snumattr(v, a, -1e9, 1e9, &st->stroke_op); if (rc < 0) return rc; st->stroke_op = st->stroke_op < 0 ? 0 : (st->stroke_op > 1 ? 1 : st->stroke_op); }
        else if (yol__seq(a->name, a->nlen, "opacity")) { rc = yol__snumattr(v, a, -1e9, 1e9, opacity); if (rc < 0) return rc; *opacity = *opacity < 0 ? 0 : (*opacity > 1 ? 1 : *opacity); }
        else if (yol__seq(a->name, a->nlen, "transform")) { rc = yol__sxform(v, a, &st->xf); if (rc < 0) return rc; }
    }
    (void)shape;
    return YOL_OK;
}

/* The tag at s[i] == '<': name, attributes, self-closing. 0 for a start tag,
 * 1 for an end tag, 2 for something skipped. */
static int yol__stag(yol__sv* v, const char** name, size_t* nlen, int* selfclose) {
    const char* s = v->s;
    size_t n = v->n, i = v->i + 1;
    v->nat = 0;
    if (i < n && s[i] == '?') {
        for (; i + 1 < n && !(s[i] == '?' && s[i + 1] == '>'); i++) {}
        if (i + 1 >= n) return yol__sverr(v, YOL_ERR_FORMAT, "an unclosed <?");
        v->i = i + 2; return 2;
    }
    if (i + 2 < n && memcmp(s + i, "!--", 3) == 0) {
        for (i += 3; i + 2 < n && memcmp(s + i, "-->", 3) != 0; i++) {}
        if (i + 2 >= n) return yol__sverr(v, YOL_ERR_FORMAT, "an unclosed comment");
        v->i = i + 3; return 2;
    }
    if (i + 7 < n && memcmp(s + i, "![CDATA[", 8) == 0) return yol__sverr(v, YOL_ERR_REFUSED, "CDATA sections are not supported");
    if (i < n && s[i] == '!') {
        for (; i < n && s[i] != '>'; i++) if (s[i] == '[') return yol__sverr(v, YOL_ERR_REFUSED, "a DOCTYPE internal subset (entities) is not supported");
        if (i >= n) return yol__sverr(v, YOL_ERR_FORMAT, "an unclosed <!");
        v->i = i + 1; return 2;
    }
    if (i < n && s[i] == '/') {
        size_t nb = ++i;
        while (i < n && yol__sname(s[i])) i++;
        *name = s + nb; *nlen = i - nb;
        while (i < n && yol__sws(s[i])) i++;
        if (i >= n || s[i] != '>' || *nlen == 0) return yol__sverr(v, YOL_ERR_FORMAT, "a malformed end tag");
        v->i = i + 1; return 1;
    }
    {
        size_t nb = i;
        if (i >= n || !yol__salpha(s[i])) return yol__sverr(v, YOL_ERR_FORMAT, "a malformed tag");
        while (i < n && yol__sname(s[i])) i++;
        *name = s + nb; *nlen = i - nb;
    }
    for (;;) {
        size_t an, ae;
        char qch;
        while (i < n && yol__sws(s[i])) i++;
        if (i >= n) return yol__sverr(v, YOL_ERR_FORMAT, "an unclosed tag");
        if (s[i] == '>') { *selfclose = 0; v->i = i + 1; return 0; }
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '>') { *selfclose = 1; v->i = i + 2; return 0; }
        if (!yol__salpha(s[i])) return yol__sverr(v, YOL_ERR_FORMAT, "a malformed attribute");
        an = i;
        while (i < n && yol__sname(s[i])) i++;
        ae = i;
        while (i < n && yol__sws(s[i])) i++;
        if (i >= n || s[i] != '=') return yol__sverr(v, YOL_ERR_FORMAT, "an attribute without a value");
        i++;
        while (i < n && yol__sws(s[i])) i++;
        if (i >= n || (s[i] != '"' && s[i] != '\'')) return yol__sverr(v, YOL_ERR_FORMAT, "an unquoted attribute value");
        qch = s[i++];
        if (v->nat == YOL__SVG_ATTRS) return yol__sverr(v, YOL_ERR_FORMAT, "more than %d attributes on one element", YOL__SVG_ATTRS);
        v->at[v->nat].name = s + an; v->at[v->nat].nlen = ae - an; v->at[v->nat].val = s + i;
        while (i < n && s[i] != qch) {
            if (s[i] == '&' || s[i] == '<') return yol__sverr(v, YOL_ERR_REFUSED, "entity references in attribute values are not supported");
            i++;
        }
        if (i >= n) return yol__sverr(v, YOL_ERR_FORMAT, "an unclosed attribute value");
        v->at[v->nat].vlen = (size_t)(s + i - v->at[v->nat].val);
        v->nat++;
        i++;
    }
}

/* Path data into p (local coordinates). */
static int yol__spathd(yol__sv* v, const yol__sa* a, yol_path* p) {
    const char* s = a->val;
    size_t n = a->vlen, i = 0;
    char cmd = 0, prev = 0;
    double cx = 0, cy = 0, sx = 0, sy = 0, lx = 0, ly = 0;
    for (;;) {
        double q[7];
        int k, need, rel;
        char up;
        yol__sskip(s, n, &i);
        if (i >= n) break;
        if (yol__salpha(s[i])) { cmd = s[i++]; }
        else if (!cmd) return yol__sverr(v, YOL_ERR_FORMAT, "path data must start with a command");
        up = (char)(cmd >= 'a' ? cmd - 32 : cmd);
        rel = cmd >= 'a';
        switch (up) {
        case 'Z': need = 0; break;
        case 'M': case 'L': case 'T': need = 2; break;
        case 'H': case 'V': need = 1; break;
        case 'C': need = 6; break;
        case 'S': case 'Q': need = 4; break;
        case 'A': need = 7; break;
        default: return yol__sverr(v, YOL_ERR_FORMAT, "the path command '%c'", cmd);
        }
        for (k = 0; k < need; k++) {
            yol__sskip(s, n, &i);
            if (up == 'A' && (k == 3 || k == 4)) {
                if (i < n && (s[i] == '0' || s[i] == '1')) { q[k] = s[i] - '0'; i++; continue; }
                return yol__sverr(v, YOL_ERR_FORMAT, "an arc flag");
            }
            if (!yol__snum(s, n, &i, &q[k])) return yol__sverr(v, YOL_ERR_FORMAT, "path data: '%c' without its numbers", cmd);
        }
        switch (up) {
        case 'Z': yol_close_path(p); cx = sx; cy = sy; cmd = 0; break;
        case 'M':
            if (rel) { q[0] += cx; q[1] += cy; }
            yol_move(p, q[0], q[1]); cx = sx = q[0]; cy = sy = q[1];
            cmd = rel ? 'l' : 'L';
            break;
        case 'L': if (rel) { q[0] += cx; q[1] += cy; } yol_line(p, q[0], q[1]); cx = q[0]; cy = q[1]; break;
        case 'H': if (rel) q[0] += cx; yol_line(p, q[0], cy); cx = q[0]; break;
        case 'V': if (rel) q[0] += cy; yol_line(p, cx, q[0]); cy = q[0]; break;
        case 'C':
            if (rel) for (k = 0; k < 6; k += 2) { q[k] += cx; q[k + 1] += cy; }
            yol_cubic(p, q[0], q[1], q[2], q[3], q[4], q[5]); lx = q[2]; ly = q[3]; cx = q[4]; cy = q[5];
            break;
        case 'S': {
            double c1x = cx, c1y = cy;
            if (prev == 'C' || prev == 'S') { c1x = 2 * cx - lx; c1y = 2 * cy - ly; }
            if (rel) for (k = 0; k < 4; k += 2) { q[k] += cx; q[k + 1] += cy; }
            yol_cubic(p, c1x, c1y, q[0], q[1], q[2], q[3]); lx = q[0]; ly = q[1]; cx = q[2]; cy = q[3];
            break;
        }
        case 'Q':
            if (rel) for (k = 0; k < 4; k += 2) { q[k] += cx; q[k + 1] += cy; }
            yol_quad(p, q[0], q[1], q[2], q[3]); lx = q[0]; ly = q[1]; cx = q[2]; cy = q[3];
            break;
        case 'T': {
            double qx = cx, qy = cy;
            if (prev == 'Q' || prev == 'T') { qx = 2 * cx - lx; qy = 2 * cy - ly; }
            if (rel) { q[0] += cx; q[1] += cy; }
            yol_quad(p, qx, qy, q[0], q[1]); lx = qx; ly = qy; cx = q[0]; cy = q[1];
            break;
        }
        default:   /* A */
            if (rel) { q[5] += cx; q[6] += cy; }
            yol_arc_to(p, q[0], q[1], q[2], (int)q[3], (int)q[4], q[5], q[6]); cx = q[5]; cy = q[6];
            break;
        }
        prev = up;
        if (p->status) return yol__sverr(v, p->status, "path data: %s", yol_error(v->c));
    }
    return YOL_OK;
}

static int yol__spoints(yol__sv* v, const yol__sa* a, yol_path* p, int close) {
    const char* s = a->val;
    size_t n = a->vlen, i = 0;
    int k = 0;
    for (;;) {
        double x, y;
        yol__sskip(s, n, &i);
        if (i >= n) break;
        if (!yol__snum(s, n, &i, &x)) return yol__sverr(v, YOL_ERR_FORMAT, "points: not a number");
        yol__sskip(s, n, &i);
        if (!yol__snum(s, n, &i, &y)) return yol__sverr(v, YOL_ERR_FORMAT, "points: an odd count");
        if (k++ == 0) yol_move(p, x, y); else yol_line(p, x, y);
    }
    if (close) yol_close_path(p);
    return YOL_OK;
}

/* x' = t x for every point of p; a mirror reverses the winding, which the
 * resolve after it does not mind. */
static void yol__sapply(yol_path* p, yol_xform t) {
    int32_t k;
    for (k = 0; k < p->n_pts; k++) {
        double x = p->pts[2 * k], y = p->pts[2 * k + 1];
        p->pts[2 * k] = t.a * x + t.c * y + t.e;
        p->pts[2 * k + 1] = t.b * x + t.d * y + t.f;
    }
}

static int yol__slayer(yol__sv* v, yol_path* src, uint32_t rgb, double alpha, int source) {
    yol_svg_layer* L;
    int rc;
    if (v->nl >= v->d->max_layers) return yol__sverr(v, YOL_ERR_FULL, "more than %d layers", v->d->max_layers);
    L = &v->d->layers[v->nl];
    yol_path_init(&L->path, v->c);
    rc = yol_resolve(v->c, src, &L->path);
    if (rc < 0) { yol_path_free(&L->path); return yol__sverr(v, rc, "%s", yol_error(v->c)); }
    if (L->path.n_contours == 0) { yol_path_free(&L->path); return YOL_OK; }
    L->rgba = rgb << 8 | (uint32_t)floor(alpha * 255 + 0.5);
    L->source = source;
    L->element = v->element;
    v->nl++;
    return YOL_OK;
}

/* One shape: built in its own coordinates, then fill and stroke. */
static int yol__sshape(yol__sv* v, const char* name, size_t nlen, const yol__ss* st, double opacity) {
    yol_path lp, tp;
    double smax = yol__xf_smax(st->xf), g[6] = { 0, 0, 0, 0, 0, 0 };
    int rc = YOL_OK, k;
    static const char* const gn[6] = { "x", "y", "width", "height", "rx", "ry" };
    if (!(smax > 0)) return YOL_OK;      /* a degenerate transform draws nothing */
    if (opacity < 1 && st->fill_on && st->stroke_on)
        return yol__sverr(v, YOL_ERR_REFUSED, "opacity on a shape with both fill and stroke (a compositing group) is not supported");
    yol__tmp_get(v->c, &lp, YOL__S_TP_PTS, YOL__S_TP_CON);
    yol_set_tol(&lp, v->tol / smax);
    if (yol__seq(name, nlen, "path")) {
        const yol__sa* a = yol__sattr(v, "d");
        if (a) rc = yol__spathd(v, a, &lp);
    } else if (yol__seq(name, nlen, "polyline") || yol__seq(name, nlen, "polygon")) {
        const yol__sa* a = yol__sattr(v, "points");
        if (a) rc = yol__spoints(v, a, &lp, yol__seq(name, nlen, "polygon"));
    } else {
        /* rect: x y width height rx ry; circle: cx cy r; ellipse: cx cy rx ry; line: x1 y1 x2 y2 */
        const char* const* names = gn;
        static const char* const cn[3] = { "cx", "cy", "r" }, * const en[4] = { "cx", "cy", "rx", "ry" }, * const ln[4] = { "x1", "y1", "x2", "y2" };
        int m = 6;
        if (yol__seq(name, nlen, "circle")) { names = cn; m = 3; }
        else if (yol__seq(name, nlen, "ellipse")) { names = en; m = 4; }
        else if (yol__seq(name, nlen, "line")) { names = ln; m = 4; }
        for (k = 0; k < m && rc >= 0; k++) {
            const yol__sa* a = yol__sattr(v, names[k]);
            g[k] = (k >= 4 && names == gn) ? -1 : 0;
            if (a && !(names == gn && k >= 4 && yol__seq(a->val, a->vlen, "auto"))) rc = yol__slen(v, a->val, a->vlen, names[k], &g[k]);
        }
        if (rc >= 0) {
            if (names == gn) {
                if (g[4] < 0 && g[5] < 0) g[4] = g[5] = 0;
                else if (g[4] < 0) g[4] = g[5];
                else if (g[5] < 0) g[5] = g[4];
                if (g[2] > 0 && g[3] > 0) yol_rect(&lp, g[0], g[1], g[2], g[3], g[4], g[5]);
            } else if (names == cn) { if (g[2] > 0) yol_ellipse(&lp, g[0], g[1], g[2], g[2]); }
            else if (names == en) { if (g[2] > 0 && g[3] > 0) yol_ellipse(&lp, g[0], g[1], g[2], g[3]); }
            else { yol_move(&lp, g[0], g[1]); yol_line(&lp, g[2], g[3]); }
        }
    }
    if (rc >= 0) { rc = yol_path_end(&lp); if (rc < 0) rc = yol__sverr(v, rc, "%s", yol_error(v->c)); }
    yol__tmp_put(v->c, &lp, YOL__S_TP_PTS, YOL__S_TP_CON);
    if (rc < 0 || lp.n_contours == 0) return rc;
    lp.rule = st->rule;
    yol__tmp_get(v->c, &tp, YOL__S_TQ_PTS, YOL__S_TQ_CON);
    if (st->fill_on && st->fill_op * opacity > 0) {
        rc = yol_path_copy(&tp, &lp);
        if (rc >= 0) { yol__sapply(&tp, st->xf); tp.flags = 0; rc = yol__slayer(v, &tp, st->fill, st->fill_op * opacity, YOL_SVG_FILL); }
    }
    yol__tmp_put(v->c, &tp, YOL__S_TQ_PTS, YOL__S_TQ_CON);
    if (rc >= 0 && st->stroke_on && st->sw > 0 && st->stroke_op * opacity > 0) {
        /* stroked in the shape's own coordinates, then transformed: a skew
         * or an unequal scale draws the pen as the file says */
        yol_stroke_desc sd;
        yol_path sp;
        memset(&sd, 0, sizeof sd);
        sd.width = st->sw; sd.join = st->join; sd.cap = st->cap; sd.miter_limit = st->miter; sd.tol = v->tol / smax;
        yol_path_init(&sp, v->c);
        rc = yol_stroke(v->c, &lp, &sp, &sd);
        if (rc < 0) rc = yol__sverr(v, rc, "%s", yol_error(v->c));
        else { yol__sapply(&sp, st->xf); sp.flags = 0; rc = yol__slayer(v, &sp, st->stroke, st->stroke_op * opacity, YOL_SVG_STROKE); }
        yol_path_free(&sp);
    }
    v->element++;
    return rc;
}

/* Skips an element's content to its end tag (title, desc, metadata). */
static int yol__sskipel(yol__sv* v) {
    int depth = 1;
    while (v->i < v->n) {
        const char* nm;
        size_t nl;
        int sc = 0, t;
        if (v->s[v->i] != '<') { v->i++; continue; }
        t = yol__stag(v, &nm, &nl, &sc);
        if (t < 0) return t;
        if (t == 0 && !sc) { if (++depth > YOL__SVG_DEPTH) return yol__sverr(v, YOL_ERR_FORMAT, "elements nested deeper than %d", YOL__SVG_DEPTH); }
        else if (t == 1 && --depth == 0) return YOL_OK;
    }
    return yol__sverr(v, YOL_ERR_FORMAT, "an unclosed element");
}

static int yol__sisshape(const char* nm, size_t nl) {
    return yol__seq(nm, nl, "path") || yol__seq(nm, nl, "rect") || yol__seq(nm, nl, "circle") || yol__seq(nm, nl, "ellipse") ||
           yol__seq(nm, nl, "line") || yol__seq(nm, nl, "polyline") || yol__seq(nm, nl, "polygon");
}

/* The content of an element: children until the end tag. kind 0: g or svg,
 * 1: a shape (only title and desc inside), 2: defs (nothing inside). */
static int yol__scontent(yol__sv* v, const yol__ss* st, int depth, int kind) {
    while (v->i < v->n) {
        const char* nm;
        size_t nl;
        int sc = 0, t, rc;
        if (v->s[v->i] != '<') { v->i++; continue; }
        t = yol__stag(v, &nm, &nl, &sc);
        if (t < 0) return t;
        if (t == 2) continue;
        if (t == 1) return YOL_OK;
        if (yol__seq(nm, nl, "title") || yol__seq(nm, nl, "desc") || yol__seq(nm, nl, "metadata")) {
            if (!sc) { rc = yol__sskipel(v); if (rc < 0) return rc; }
            continue;
        }
        if (kind == 2) return yol__sverr(v, YOL_ERR_REFUSED, "<%.*s> inside <defs> is not supported", (int)(nl > 30 ? 30 : nl), nm);
        if (kind == 1) return yol__sverr(v, YOL_ERR_REFUSED, "<%.*s> inside a shape is not supported", (int)(nl > 30 ? 30 : nl), nm);
        if (depth >= YOL__SVG_DEPTH) return yol__sverr(v, YOL_ERR_FORMAT, "elements nested deeper than %d", YOL__SVG_DEPTH);
        if (yol__seq(nm, nl, "g") || yol__sisshape(nm, nl)) {
            yol__ss cs = *st;
            double op;
            int shape = yol__sisshape(nm, nl);
            rc = yol__sstyle(v, &cs, &op, shape);
            if (rc < 0) return rc;
            if (!shape && op < 1) return yol__sverr(v, YOL_ERR_REFUSED, "opacity on a group (a compositing group) is not supported");
            if (shape) { rc = yol__sshape(v, nm, nl, &cs, op); if (rc < 0) return rc; }
            if (!sc) { rc = yol__scontent(v, &cs, depth + 1, shape ? 1 : 0); if (rc < 0) return rc; }
        } else if (yol__seq(nm, nl, "defs")) {
            if (!sc) { rc = yol__scontent(v, st, depth + 1, 2); if (rc < 0) return rc; }
        } else if (yol__seq(nm, nl, "svg")) {
            return yol__sverr(v, YOL_ERR_REFUSED, "a nested <svg> is not supported");
        } else return yol__sverr(v, YOL_ERR_REFUSED, "<%.*s> is not supported", (int)(nl > 30 ? 30 : nl), nm);
    }
    return yol__sverr(v, YOL_ERR_FORMAT, "an unclosed element");
}

static int yol__svg(yol_ctx* c, const char* text, size_t n, const yol_svg_desc* d, double viewbox[4], char* err, size_t cap) {
    yol__sv v;
    yol__ss st;
    int rc, k;
    if (err && cap) err[0] = 0;
    if (!c || !text || !d || (!d->layers && d->max_layers > 0) || d->max_layers < 0 || !yol__finite(d->tol) || d->tol < 0)
        return yol__ferr(err, cap, YOL_ERR_ARG, "SVG: no context, text or desc, or a bad desc");
    memset(&v, 0, sizeof v);
    v.c = c; v.s = text; v.n = n; v.err = err; v.cap = cap; v.d = d;
    memset(&st, 0, sizeof st);
    st.xf = yol_xf_identity();
    st.fill_on = 1; st.fill = 0; st.stroke_on = 0; st.fill_op = st.stroke_op = 1; st.sw = 1; st.miter = 4;
    st.join = YOL_JOIN_MITER; st.cap = YOL_CAP_BUTT; st.rule = YOL_NONZERO;
    /* the root: prolog, comments and doctype, then <svg> */
    for (;;) {
        const char* nm;
        size_t nl;
        int sc = 0, t;
        while (v.i < n && yol__sws(text[v.i])) v.i++;
        if (v.i + 3 <= n && (unsigned char)text[v.i] == 0xef && (unsigned char)text[v.i + 1] == 0xbb && (unsigned char)text[v.i + 2] == 0xbf) { v.i += 3; continue; }
        if (v.i >= n || text[v.i] != '<') return yol__sverr(&v, YOL_ERR_FORMAT, "no <svg> element");
        t = yol__stag(&v, &nm, &nl, &sc);
        if (t < 0) return t;
        if (t == 2) continue;
        if (t == 1 || !yol__seq(nm, nl, "svg")) return yol__sverr(&v, YOL_ERR_FORMAT, "the root element is not <svg>");
        {
            const yol__sa* vb = yol__sattr(&v, "viewBox");
            double op;
            if (yol__sattr(&v, "transform")) return yol__sverr(&v, YOL_ERR_REFUSED, "transform on <svg> is not supported");
            if (vb) {
                size_t i = 0;
                for (k = 0; k < 4; k++) {
                    yol__sskip(vb->val, vb->vlen, &i);
                    if (!yol__snum(vb->val, vb->vlen, &i, &v.vb[k])) return yol__sverr(&v, YOL_ERR_FORMAT, "viewBox needs four numbers");
                }
                if (!(v.vb[2] > 0 && v.vb[3] > 0)) return yol__sverr(&v, YOL_ERR_FORMAT, "viewBox width and height must be > 0");
            } else {
                const yol__sa* wa = yol__sattr(&v, "width");
                const yol__sa* ha = yol__sattr(&v, "height");
                if (!wa || !ha) return yol__sverr(&v, YOL_ERR_FORMAT, "an <svg> without viewBox, or width and height");
                rc = yol__slen(&v, wa->val, wa->vlen, "width", &v.vb[2]);
                if (rc >= 0) rc = yol__slen(&v, ha->val, ha->vlen, "height", &v.vb[3]);
                if (rc < 0) return rc;
                if (!(v.vb[2] > 0 && v.vb[3] > 0)) return yol__sverr(&v, YOL_ERR_FORMAT, "width and height must be > 0");
            }
            v.tol = d->tol > 0 ? d->tol : 1e-4 * fmax(v.vb[2], v.vb[3]);
            rc = yol__sstyle(&v, &st, &op, 0);
            if (rc < 0) return rc;
            if (op < 1) return yol__sverr(&v, YOL_ERR_REFUSED, "opacity on <svg> (a compositing group) is not supported");
            if (viewbox) for (k = 0; k < 4; k++) viewbox[k] = v.vb[k];
            if (!sc) { rc = yol__scontent(&v, &st, 1, 0); if (rc < 0) goto fail; }
            break;
        }
    }
    return v.nl;
fail:
    for (k = 0; k < v.nl; k++) yol_path_free(&d->layers[k].path);
    return rc;
}

static int yol__cset_add_font(yol_cset* s, const yol_font* f, const uint32_t* glyphs, uint32_t n, double tol) {
    yol_ctx* c;
    yol_path g;
    yol_glyph_desc gd;
    uint32_t i;
    int rc = YOL_OK;
    if (!s || !f || !s->ctx) return YOL_ERR_ARG;
    c = s->ctx;
    if (!glyphs && n == 0) n = (uint32_t)f->n_glyphs;
    memset(&gd, 0, sizeof gd);
    gd.tol = tol;
    yol__tmp_get(c, &g, YOL__S_TQ_PTS, YOL__S_TQ_CON);
    for (i = 0; i < n && rc >= 0; i++) {
        uint32_t id = glyphs ? glyphs[i] : i;
        yol_path_clear(&g);
        rc = yol_font_glyph(c, f, id, &g, &gd);
        if (rc >= 0) rc = yol_cset_add(s, id, &g);
    }
    yol__tmp_put(c, &g, YOL__S_TQ_PTS, YOL__S_TQ_CON);
    return rc;
}

/* --- parameter tables -------------------------------------------------------- */

static const yol_param yol__stroke_params[] = {
    { "width", "f64", 0, 1e9, 0, "path units", "stroke width, > 0 (a glyph path is in em)", (uint32_t)offsetof(yol_stroke_desc, width) },
    { "join", "enum", 0, 2, 0, "", "0 round, 1 miter, 2 bevel", (uint32_t)offsetof(yol_stroke_desc, join) },
    { "miter_limit", "f64", 1, 1e9, 4, "", "miter length over width past which a miter is a bevel", (uint32_t)offsetof(yol_stroke_desc, miter_limit) },
    { "cap", "enum", 0, 2, 0, "", "open contours: 0 butt, 1 round, 2 square", (uint32_t)offsetof(yol_stroke_desc, cap) },
    { "mode", "enum", 0, 2, 0, "", "0 the stroke, 1 fill and stroke (faux bold), 2 fill minus stroke", (uint32_t)offsetof(yol_stroke_desc, mode) },
    { "tol", "f64", 0, 1e9, 1e-4, "path units", "largest distance of the expanded outline from the exact one", (uint32_t)offsetof(yol_stroke_desc, tol) },
};
static const yol_param yol__raster_params[] = {
    { "scale", "f64", 0, 1e6, 0, "px per path unit", "the font size in px per em for a glyph; > 0", (uint32_t)offsetof(yol_raster_desc, scale) },
    { "x", "f64", -1e9, 1e9, 0, "px", "where path x = 0 lands in the image", (uint32_t)offsetof(yol_raster_desc, x) },
    { "y", "f64", -1e9, 1e9, 0, "px", "where path y = 0 lands in the image", (uint32_t)offsetof(yol_raster_desc, y) },
    { "format", "enum", 0, 3, 0, "", "0 u8, 1 u16, 2 f32, 3 f64", (uint32_t)offsetof(yol_raster_desc, format) },
};
static const yol_param yol__cset_params[] = {
    { "n_glyphs", "u32", 0, 16777215, 0, "", "the glyph table size", (uint32_t)offsetof(yol_cset_desc, n_glyphs) },
    { "nh", "i32", 0, 65535, 0, "", "horizontal bands per glyph; 0: clamp(round(sqrt(curves)), 1, 16)", (uint32_t)offsetof(yol_cset_desc, nh) },
    { "nv", "i32", 0, 65535, 0, "", "vertical bands per glyph; 0: as nh", (uint32_t)offsetof(yol_cset_desc, nv) },
    { "backward", "bool", 0, 1, 0, "", "write backward lists (flag bit 1)", (uint32_t)offsetof(yol_cset_desc, backward) },
    { "keep_overlaps", "bool", 0, 1, 0, "", "do not resolve glyphs (no RESOLVED flag)", (uint32_t)offsetof(yol_cset_desc, keep_overlaps) },
};
static const yol_param yol__glyph_params[] = {
    { "size", "f64", 0, 1e9, 1, "path units per em", "0: em units", (uint32_t)offsetof(yol_glyph_desc, size) },
    { "x", "f64", -1e9, 1e9, 0, "path units", "pen x", (uint32_t)offsetof(yol_glyph_desc, x) },
    { "y", "f64", -1e9, 1e9, 0, "path units", "pen y (the baseline)", (uint32_t)offsetof(yol_glyph_desc, y) },
    { "y_up", "bool", 0, 1, 0, "", "y up as in the font; default y down", (uint32_t)offsetof(yol_glyph_desc, y_up) },
    { "tol", "f64", 0, 1, 1e-4, "em", "CFF cubic to quadratic tolerance", (uint32_t)offsetof(yol_glyph_desc, tol) },
};

#define YOL__TABLE(fn, t) YOL_API const yol_param* fn(int* n) { if (n) *n = (int)(sizeof t / sizeof t[0]); return t; }
YOL__TABLE(yol_stroke_params, yol__stroke_params)
YOL__TABLE(yol_raster_params, yol__raster_params)
YOL__TABLE(yol_cset_params, yol__cset_params)
YOL__TABLE(yol_glyph_params, yol__glyph_params)
#undef YOL__TABLE

/* --- the calls on a context: a failure always leaves a message ------------ */

YOL_API int yol_font_glyph(yol_ctx* c, const yol_font* f, uint32_t glyph, yol_path* p, const yol_glyph_desc* d) {
    uint32_t n0 = c ? c->n_err : 0;
    return yol__done(c, n0, yol__font_glyph(c, f, glyph, p, d), "glyph");
}

YOL_API int yol_resolve(yol_ctx* c, const yol_path* in, yol_path* out) {
    uint32_t n0 = c ? c->n_err : 0;
    return yol__done(c, n0, yol__resolve(c, in, out), "resolve");
}

YOL_API int yol_raster(yol_ctx* c, const yol_path* in, const yol_raster_desc* d, yol_box* box) {
    uint32_t n0 = c ? c->n_err : 0;
    return yol__done(c, n0, yol__raster(c, in, d, box), "raster");
}

YOL_API int yol_stroke(yol_ctx* c, const yol_path* in, yol_path* out, const yol_stroke_desc* d) {
    uint32_t n0 = c ? c->n_err : 0;
    return yol__done(c, n0, yol__stroke(c, in, out, d), "stroke");
}

YOL_API int yol_cset_add(yol_cset* s, uint32_t glyph, const yol_path* p) {
    yol_ctx* c = s ? s->ctx : NULL;
    uint32_t n0 = c ? c->n_err : 0;
    return yol__done(c, n0, yol__cset_add(s, glyph, p), "curve set");
}

YOL_API int yol_cset_add_font(yol_cset* s, const yol_font* f, const uint32_t* glyphs, uint32_t n, double tol) {
    yol_ctx* c = s ? s->ctx : NULL;
    uint32_t n0 = c ? c->n_err : 0;
    return yol__done(c, n0, yol__cset_add_font(s, f, glyphs, n, tol), "curve set");
}

YOL_API int yol_svg(yol_ctx* c, const char* text, size_t n, const yol_svg_desc* d, double viewbox[4], char* err, size_t cap) {
    uint32_t n0 = c ? c->n_err : 0;
    return yol__done(c, n0, yol__svg(c, text, n, d, viewbox, err, cap), "SVG");
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#endif /* YSP_OUTLINE_IMPLEMENTATION_GUARD */
#endif /* YSP_OUTLINE_IMPLEMENTATION */

/* ------------------------------------------------------------------------
 * This software is available under the MIT-0 (MIT No Attribution) license.
 *
 * Copyright (c) 2026 ysp contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY.
 * ------------------------------------------------------------------------ */
