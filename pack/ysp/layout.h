/* ysp/layout.h - v0.3.1 - text layout for the pack tool and the player
 *
 *   Lays out UTF-8 text with Skribidi (vendored in third_party/, pinned, with
 *   local patches) and gives glyphs as ysp/gfx.h curve-run items over
 *   ysp/outline.h curve sets built from the same font bytes. Not a single
 *   header: it links the vendored Skribidi, HarfBuzz, SheenBidi, libunibreak
 *   and budouxc (CMake option YSP_BUILD_LAYOUT). The headers in include/ysp stay
 *   free of them (rig_spec 5.2, Font). docs/layout.md is the manual.
 *
 *   The program compiles ysp/outline.h's implementation once
 *   (YSP_OUTLINE_IMPLEMENTATION). This library uses ysp/gfx.h's types only
 *   and calls nothing in it, so the pack tool runs without a GPU; uploading
 *   the curve sets (ygfx_cset_make, ygfx_cset_add) is the caller's.
 *
 *   Usage:
 *       yol_ctx ol; yol_open(&ol, NULL);
 *       ylay_lib* L;
 *       ylay_create(&L, &(ylay_desc){ .ol = &ol });
 *       int ui = ylay_add_font(L, bytes, n, 0, "segoeui.ttf");     // fallback order
 *       ylay_block b = { 0 };
 *       ylay_layout(L, "Hello, world", -1, &(ylay_style){ .size = 32, .width = 400 }, NULL, 0, &b);
 *       // b.runs[r]: font b.runs[r].font, items b.items[first .. first + n - 1],
 *       // one ygfx_crun each, .size = b.runs[r].size, place and anchor TOP_LEFT,
 *       // .w = b.w, .h = b.h, so every run shares the block's origin.
 *       ylay_stamp(L, &b, line, sizeof line);    // versions and font hashes
 *
 *   Threads: a ylay_lib is for one thread, with its yol_ctx.
 *
 *   v0.2.0 (2026-11-18): font fallback past the script (Skribidi patch
 *   0003). A character that no font supporting its run's script maps, such
 *   as a digit or punctuation in Hebrew text, now comes from the first font
 *   in the order added that has its grapheme cluster, as browsers do;
 *   before, it got glyph 0 from the script's font. Text whose characters
 *   the script's fonts map lays out byte for byte as in v0.1.0.
 *   ylay_missing_chars() names the characters of the last call's glyph-0
 *   items and their runs' scripts. The stamp's patch field lists three
 *   hashes.
 *
 *   v0.3.0 (2026-11-18): emoji. A font with a COLR table (version 0 or 1)
 *   goes in Skribidi's emoji family, so emoji runs (emoji presentation,
 *   VS16, ZWJ and flag sequences) take it; other fonts stay in the default
 *   family. Skribidi patch 0004: a run whose family has no font is laid
 *   out in the other family, and a codepoint that the run's fonts lack
 *   falls back to the run's family, then to the other one, so an emoji
 *   no font has is glyph 0 (n_missing counts it; before, it vanished with
 *   n_missing 0). A font whose color glyphs are CBDT or sbix bitmaps or
 *   SVG documents and that has no COLR is refused (YLAY_ERR_REFUSED);
 *   ylay_font_info.color says which COLR a font has. The stamp's patch
 *   field lists four hashes.
 *   Color glyphs: each color glyph of a COLR font is flattened at
 *   ylay_add_font() with HarfBuzz's paint API into layers, each a clip
 *   outline (the font's glyph, or a layer glyph past the font's: the glyph
 *   under its transform, baked, and cut by the clip box where the box cuts
 *   it) and a paint record (an RGBA solid, or a linear or radial gradient
 *   of up to 16 stops, premultiplied, linear light) stored as texels at
 *   the start of the font's curve set (ysp/gfx.h PAINT RECORDS). A layout
 *   gives a color glyph as one item per layer in a run with
 *   YLAY_RUN_PAINT: glyph the layer's set glyph, color its paint's texel
 *   index; draw such a run with fields YGFX_I_PAINT. What the layers
 *   cannot draw exactly is refused per glyph by name when a layout uses
 *   it (YLAY_ERR_REFUSED; docs/layout.md, Color glyphs). New:
 *   ylay_layer, ylay_color_glyph(), ylay_color_paints(), ylay_open_set(),
 *   ylay_build_glyphs(); ylay_run.flags; ylay_font_info's color counts.
 *   Breaking: ylay_run and ylay_font_info grew.
 *
 *   v0.3.1 (2026-11-18): Skribidi patches 0005 and 0006 fix two editing
 *   faults with Devanagari (docs/layout.md, Patches 0005 and 0006). Glyphs,
 *   clusters and line breaks do not change; only skb_text's grapheme breaks
 *   and the caret, hit test and selection functions do, which the pack tool
 *   and the player do not call. The stamp's patch field lists six hashes,
 *   and ylay_stamp() no longer cuts a version line longer than 512 bytes.
 */
#ifndef YSP_LAYOUT_H_INCLUDED
#define YSP_LAYOUT_H_INCLUDED

#include <stddef.h>
#include <stdint.h>
#include "ysp/outline.h"
#include "ysp/gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YLAY_VERSION_STRING "0.3.1"
#define YLAY_MAX_FONTS      64

/* Return codes: ysp/outline.h's values for the same meaning. */
#define YLAY_OK            0
#define YLAY_ERR_ARG     (-1)
#define YLAY_ERR_FULL    (-4)
#define YLAY_ERR_REFUSED (-7)   /* v0.3: a font or glyph feature ysp does not draw, named */
#define YLAY_ERR_FORMAT  (-8)
#define YLAY_ERR_NOMEM  (-10)

/* What lays out the text. The pack manifest records these (rig_spec 5.1). */
typedef struct ylay_versions {
    const char* ylay;           /* YLAY_VERSION_STRING                          */
    const char* skribidi;         /* the full commit of the vendored Skribidi       */
    const char* skribidi_patch;   /* "sha256:<hex>" of each local patch file, in
                                   * the order they apply, comma-separated, then
                                   * ",tree-differs" when the patched source does
                                   * not match third_party/MANIFEST.sha256         */
    const char* harfbuzz;         /* hb_version_string(), e.g. "14.6.0"            */
    const char* sheenbidi;        /* commit                                         */
    const char* libunibreak;      /* tag                                            */
    const char* budouxc;          /* commit                                         */
} ylay_versions;
const ylay_versions* ylay_get_versions(void);

typedef struct ylay_lib ylay_lib;

typedef struct ylay_desc {
    yol_ctx* ol;                /* required: builds the curve sets               */
} ylay_desc;

int  ylay_create(ylay_lib** out, const ylay_desc* d);
void ylay_destroy(ylay_lib* L);
/* The message of the last call that failed ("" if none). */
const char* ylay_error(const ylay_lib* L);

/* Adds a font: face `face` of the bytes (a .ttc holds several). The bytes
 * stay the caller's and must outlive L. Fonts are tried in the order added:
 * for each run of one script, the fonts that support the script, in that
 * order, and the first that maps the codepoint (Skribidi's rule); for a
 * codepoint none of them maps (digits and punctuation take the script of
 * the text around them), the first font of all that maps its grapheme
 * cluster, else the codepoint (docs/layout.md, Font fallback).
 * A font with a COLR table goes in the emoji family: emoji runs try those
 * fonts first, in the order added, then the others; other runs the other
 * way round (v0.3, docs/layout.md, Emoji). A font whose color glyphs are
 * CBDT, sbix or SVG with no COLR is refused (YLAY_ERR_REFUSED). Opens the
 * face with HarfBuzz and with ysp/outline.h, hashes the bytes (SHA-256) and
 * makes an empty curve set (resolved, backward lists) whose glyphs are built
 * when a layout first uses them. Returns the font index or a code. */
int ylay_add_font(ylay_lib* L, const void* bytes, size_t n, int face, const char* name);

typedef struct ylay_font_info {
    const char*       name;
    int               face;
    const char*       sha256;          /* 64 hex digits of the file's bytes        */
    int32_t           units_per_em, n_glyphs;
    double            ascender, descender, line_gap;   /* em, hhea; descender < 0  */
    const yol_cset* set;             /* the glyphs built so far                  */
    uint32_t          n_built;
    double            build_us;        /* time spent building them, in all         */
    int               color;           /* v0.3: 0 no COLR; 1 COLR version 0 layers;
                                        * 2 COLR version 1 paints. A COLR font is in
                                        * the emoji family (docs/layout.md, Emoji)   */
    uint32_t          set_glyphs;      /* v0.3: the set's glyph table: n_glyphs plus
                                        * the layer glyphs                         */
    uint32_t          n_color_glyphs, n_refused;   /* v0.3: color glyphs; of them
                                        * refused (a layout that uses one fails)    */
    uint32_t          n_layers, n_layer_glyphs;    /* v0.3: layers of all color
                                        * glyphs; layer glyphs past the font's      */
    uint32_t          n_paint_texels;  /* v0.3: texels 0 .. n - 1 of the set        */
    double            color_us;        /* v0.3: the flattening's time at add        */
} ylay_font_info;
int ylay_font(const ylay_lib* L, int font, ylay_font_info* out);
int ylay_font_count(const ylay_lib* L);

/* v0.3: color glyphs. A layer of a color glyph: a glyph of the font's set
 * (the font's own, or a layer glyph, >= n_glyphs) and the texel index of
 * its paint record in the same set. */
typedef struct ylay_layer { uint32_t glyph, paint; } ylay_layer;

/* The layers of glyph (a font glyph id), bottom first, in *layers (L's,
 * valid until L is destroyed). Returns the layer count (0: not a color
 * glyph, or a color glyph with nothing to draw), or YLAY_ERR_REFUSED with
 * the reason in ylay_error() (the feature and what to use instead). */
int ylay_color_glyph(ylay_lib* L, int font, uint32_t glyph, const ylay_layer** layers);
/* The paint records' texels (4 floats each), the first texels of the
 * font's set. Returns their count (0 for a font without COLR). */
int ylay_color_paints(const ylay_lib* L, int font, const float** texels);
/* Opens a set the way L builds the font's: set_glyphs entries, resolved,
 * backward lists, the paint records first, so the glyph ids and paint
 * indexes of L's layouts hold in it (the pack tool's CURVESET). On ctx. */
int ylay_open_set(ylay_lib* L, int font, yol_cset* set, yol_ctx* ctx);
/* Builds set glyphs (font glyphs, layer glyphs, or both; NULL: all of
 * them, 0 .. set_glyphs - 1) into a set from ylay_open_set(). tol: the CFF
 * cubic tolerance in em (0: 1e-4). */
int ylay_build_glyphs(ylay_lib* L, int font, yol_cset* set, const uint32_t* glyphs, uint32_t n, double tol);

/* The glyphs added to font's set since the last call, for ygfx_cset_add()
 * (or ygfx_cset_make() the first time). *glyphs is L's array and stays
 * valid until the next call that lays out. Returns the count. */
int ylay_take_new_glyphs(ylay_lib* L, int font, const uint32_t** glyphs);

enum { YLAY_DIR_AUTO = 0, YLAY_DIR_LTR, YLAY_DIR_RTL };
enum { YLAY_ALIGN_START = 0, YLAY_ALIGN_END, YLAY_ALIGN_CENTER };

typedef struct ylay_style {
    float       size;          /* units (px or deg) per em; > 0                    */
    float       width;         /* the box width in units; 0: no wrapping           */
    float       line_height;   /* times the fonts' own line; 0: 1                  */
    int         dir;           /* YLAY_DIR_*; AUTO: the first strong character   */
    int         align;         /* YLAY_ALIGN_*; START and END follow dir         */
    const char* lang;          /* BCP 47. "ja", "zh-Hans", "zh-Hant" and "th" break
                                * at BudouX phrases (Skribidi's rule); NULL: ""   */
    float       color;         /* the palette position of every item              */
} ylay_style;

/* Attributes over the text's bytes [start, end). Spans are sorted and do not
 * overlap; text outside them has the style's. */
typedef struct ylay_span {
    uint32_t    start, end;
    float       size;          /* 0: the style's                                   */
    float       color;         /* < 0: the style's                                 */
    const char* lang;          /* NULL: the style's                                */
} ylay_span;

typedef struct ylay_line {
    float    x, baseline;      /* units from the block's top-left                  */
    float    width, ascent, descent;   /* descent > 0, below the baseline           */
    uint32_t text_start, text_end;     /* codepoint offsets                         */
} ylay_line;

/* The items of one font at one size: one ygfx_crun. */
#define YLAY_RUN_PAINT 0x1u    /* v0.3: color glyph layers: glyph a set glyph, color
                                * a paint record's texel index; YGFX_I_PAINT    */
typedef struct ylay_run {
    int      font;
    float    size;             /* crun_desc.size                                   */
    uint32_t first, n;         /* into the block's items                           */
    uint32_t flags;            /* v0.3: YLAY_RUN_PAINT                             */
} ylay_run;

typedef struct ylay_block {
    /* items[i]: x, y the glyph's pen in units from the block's top-left (y down,
     * on the baseline), glyph its id in the run's font, color from the style or
     * span; the other fields 0, scale 1, gate 1. cluster[i]: the codepoint offset
     * where items[i]'s cluster starts. Grouped by run, visual order inside one. */
    ygfx_citem* items;   uint32_t* cluster;  uint32_t n_items, cap_items;
    ylay_run*   runs;    uint32_t n_runs,  cap_runs;
    ylay_line*  lines;   uint32_t n_lines, cap_lines;
    float         w, h;          /* the laid-out box, units                        */
    int           dir;           /* the paragraph's resolved direction             */
    uint32_t      n_missing;     /* items with glyph 0 (ylay_missing_chars())      */
    uint64_t      fonts_used;    /* bit i: font i                                  */
    const ylay_versions* versions;
    double        layout_us;     /* the last call: Skribidi and the conversion     */
    double        build_us;      /* the last call: building new glyphs             */
    uint32_t      n_new_glyphs;  /* the last call                                  */
} ylay_block;

/* Lays out text (n bytes, or to its NUL when n < 0) into b. b's arrays grow
 * only when too small, so laying out again at the same size or smaller
 * allocates nothing here; Skribidi allocates (docs/layout.md, Cost).
 * Builds the glyphs the result uses that the font sets lack. */
int  ylay_layout(ylay_lib* L, const char* text, int n, const ylay_style* st,
                   const ylay_span* spans, int n_spans, ylay_block* b);
void ylay_block_free(ylay_block* b);

/* A character of a glyph-0 item. */
typedef struct ylay_missing {
    uint32_t codepoint;
    uint32_t offset;           /* codepoint offset of its first use in the text   */
    char     script[5];        /* ISO 15924 tag of its run's script, e.g. "Hebr"  */
    int      in_a_font;        /* 0: no font of L maps it; 1: a font maps it, and
                                * glyph 0 came from the font or from shaping     */
} ylay_missing;

/* The distinct characters of the last ylay_layout() or ylay_from_skb() call's
 * glyph-0 items, in text order: for each glyph-0 cluster, the
 * characters no font of L maps, or its first character with in_a_font set
 * when every one is mapped. Copies up to cap into out; returns the count. */
int  ylay_missing_chars(const ylay_lib* L, ylay_missing* out, int cap);

/* One line for a log or a manifest: the versions, the patch hash, and
 * name#face=sha256 for each font b used (every font when b is NULL).
 * Returns the length it needed. */
int  ylay_stamp(const ylay_lib* L, const ylay_block* b, char* buf, size_t cap);

/* Skribidi access, for its editor. The collections hold L's fonts. */
struct skb_font_collection_t;
struct skb_attribute_collection_t;
struct skb_temp_alloc_t;
struct skb_layout_t;
struct skb_font_collection_t*      ylay_skb_fonts(ylay_lib* L);
struct skb_attribute_collection_t* ylay_skb_attributes(ylay_lib* L);
struct skb_temp_alloc_t*           ylay_skb_temp(ylay_lib* L);
/* Adds a Skribidi layout (an editor's paragraph) to b, offset by x, y, with
 * every item at palette position color; append 0 empties b first. Builds
 * missing glyphs as ylay_layout() does. */
int ylay_from_skb(ylay_lib* L, const struct skb_layout_t* lay, float x, float y,
                    float color, int append, ylay_block* b);

#ifdef __cplusplus
}
#endif
#endif
