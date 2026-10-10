/* ysp/layout.h - v0.1.0 - text layout for the pack tool and the player
 *
 *   Lays out UTF-8 text with Skribidi (vendored in third_party/, pinned, with
 *   a local bidi patch) and gives glyphs as ysp/gfx.h curve-run items over
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

#define YLAY_VERSION_STRING "0.1.0"
#define YLAY_MAX_FONTS      64

/* Return codes: ysp/outline.h's values for the same meaning. */
#define YLAY_OK            0
#define YLAY_ERR_ARG     (-1)
#define YLAY_ERR_FULL    (-4)
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
 * order, and the first that maps the codepoint (Skribidi's rule). Opens the
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
} ylay_font_info;
int ylay_font(const ylay_lib* L, int font, ylay_font_info* out);
int ylay_font_count(const ylay_lib* L);

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
typedef struct ylay_run {
    int      font;
    float    size;             /* crun_desc.size                                   */
    uint32_t first, n;         /* into the block's items                           */
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
    uint32_t      n_missing;     /* items with glyph 0: no font has the codepoint   */
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
