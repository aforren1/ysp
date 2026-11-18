/* colr.h - the layout library's color glyphs (COLR versions 0 and 1), not
 * a public header: pack/layout.c and the flattening's fuzz target include
 * it. docs/layout.md, "Color glyphs", is the manual; ysp/layout.h has the
 * API (ylay_color_glyph, ylay_color_paints, ylay_open_set,
 * ylay_build_glyphs).
 *
 * At ylay_add_font() each color glyph is walked once with HarfBuzz's paint
 * API (hb_font_paint_glyph) and flattened into layers: a clip outline (a
 * font glyph, or a layer glyph past the font's: the glyph under a baked
 * transform, cut by the clip box where the box cuts it) and a paint record
 * (a solid, linear or radial gradient, docs/gfx.md "Paint records") at a
 * texel index of the font's curve set. What the layer model cannot draw
 * exactly is refused per glyph, by name (ylay__colr_message()).
 */
#ifndef YSP_LAYOUT_COLR_H_INCLUDED
#define YSP_LAYOUT_COLR_H_INCLUDED

#include <stdint.h>
#include <hb.h>
#include "ysp/layout.h"

/* Why a color glyph is refused: the feature, named in its message. */
enum {
    YLAY__REF_NONE = 0,
    YLAY__REF_SWEEP,          /* a sweep gradient                                */
    YLAY__REF_EXTEND,         /* extend repeat or reflect (detail: the mode)     */
    YLAY__REF_FOREGROUND,     /* palette index 0xFFFF                            */
    YLAY__REF_VARIABLE,       /* variable COLR                                   */
    YLAY__REF_COLR_GLYPH,     /* PaintColrGlyph                                  */
    YLAY__REF_NESTED_CLIP,    /* a fill under two glyph clips                    */
    YLAY__REF_COMPOSITE,      /* a composite mode (detail: the mode)             */
    YLAY__REF_MASK,           /* SRC_IN that is not group opacity                */
    YLAY__REF_OVERLAP,        /* group opacity over layers that overlap          */
    YLAY__REF_LIMIT,          /* HarfBuzz's 64 levels or 16,384 edges            */
    YLAY__REF_IMAGE,          /* an image paint                                  */
    YLAY__REF_TURNED_CLIP,    /* a clip rectangle under a turn                   */
    YLAY__REF_RADIAL_R0,      /* a radial with a start radius above 0            */
    YLAY__REF_RADIAL_FOCAL,   /* a two-point radial whose focal point is not inside */
    YLAY__REF_STOPS,          /* a color line of 0 or more than 16 stops (detail) */
    YLAY__REF_DEGENERATE,     /* a gradient with no direction or a singular transform */
    YLAY__REF_UNCLIPPED,      /* a fill with no glyph clip and no clip box       */
    YLAY__REF_OUTLINE,        /* an outline ysp/outline.h refuses (detail: its code) */
    YLAY__REF_GLYPH_ID        /* a clip glyph past the font's glyphs (detail: the id) */
};

/* A layer glyph past the font's glyphs: font glyph gid (or none: the cut box
 * alone) under the em-space transform m (yol_xform order a b c d e f), cut
 * by box (em, x0 y0 x1 y1, y down) when cut. */
typedef struct ylay__lglyph {
    uint32_t gid;             /* UINT32_MAX: the box alone                       */
    uint32_t cut;
    double   m[6];
    double   box[4];
} ylay__lglyph;

typedef struct ylay__colr {
    uint32_t      n_glyphs;   /* the font's                                      */
    uint32_t      G;          /* the set's: n_glyphs + n_lg                      */
    uint32_t*     first;      /* per font glyph: its first layer                 */
    uint16_t*     count;      /* per font glyph: layers; 0 and ref 0: not color  */
    uint8_t*      ref;        /* per font glyph: YLAY__REF_*                     */
    int32_t*      detail;     /* per font glyph: the refusal's value             */
    uint8_t*      is_color;   /* per font glyph: 1 when COLR has it              */
    ylay_layer*   layers;     uint32_t n_layers, cap_layers;
    ylay__lglyph* lg;         uint32_t n_lg, cap_lg;
    float*        paints;     uint32_t n_paint, cap_paint;   /* texels, 4 floats */
    uint32_t      n_color, n_refused;
    double        build_us;
} ylay__colr;

/* Flattens every color glyph of the face (COLR v0 layers or v1 paints).
 * Returns YLAY_OK (glyphs refused or not), or a code with err set for a
 * failure of the whole font (no memory). */
int  ylay__colr_build(ylay__colr* out, hb_face_t* face, const yol_font* yf, yol_ctx* ol, char* err, size_t cap);
void ylay__colr_free(ylay__colr* c);
/* Layer glyph g (>= n_glyphs) as a path in em, y down, unresolved. */
int  ylay__colr_layer_path(const ylay__colr* c, const yol_font* yf, yol_ctx* ol, uint32_t g, yol_path* out);
/* The feature and the advice of a refusal, for a message. */
void ylay__colr_message(int ref, int32_t detail, char* buf, size_t cap);

#endif
