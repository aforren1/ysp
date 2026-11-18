/* colr.c - color glyphs flattened into layers; see colr.h and
 * docs/layout.md, "Color glyphs". */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L   /* clock_gettime under -std=c11 */
#endif
#include "colr.h"

#include <hb-ot.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

static double ylay__colr_now_us(void) {
#if defined(_WIN32)
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec * 1e-3;
#endif
}

/* HarfBuzz walks at most this deep and this many paint-graph edges per
 * glyph, then returns without an error (hb-limits.hh, 14.6.0). */
#define YLAY__HB_DEPTH 64
#define YLAY__HB_EDGES 16384
#define YLAY__MAX_STOPS 16

/* --- growth -------------------------------------------------------------------- */

static int ylay__cgrow(void** p, uint32_t* cap, uint32_t need, size_t size) {
    uint32_t c;
    void* q;
    if (need <= *cap) return 1;
    c = *cap ? *cap : 64;
    while (c < need) {
        if (c > 0x7fffffffu) return 0;
        c *= 2;
    }
    q = realloc(*p, (size_t)c * size);
    if (!q) return 0;
    *p = q;
    *cap = c;
    return 1;
}

/* --- 2 x 3 matrices: x' = a x + c y + e, y' = b x + d y + f ------------------------- */

typedef struct ylay__m { double a, b, c, d, e, f; } ylay__m;

static ylay__m ylay__mmul(ylay__m l, ylay__m r) {   /* l after r */
    ylay__m o;
    o.a = l.a * r.a + l.c * r.b; o.b = l.b * r.a + l.d * r.b;
    o.c = l.a * r.c + l.c * r.d; o.d = l.b * r.c + l.d * r.d;
    o.e = l.a * r.e + l.c * r.f + l.e; o.f = l.b * r.e + l.d * r.f + l.f;
    return o;
}
static int ylay__minv(ylay__m m, ylay__m* o) {
    double det = m.a * m.d - m.b * m.c, s;
    double mx = fabs(m.a) + fabs(m.b) + fabs(m.c) + fabs(m.d);
    if (!(fabs(det) > 1e-12 * mx * mx) || !isfinite(det)) return 0;
    s = 1.0 / det;
    o->a = m.d * s; o->b = -m.b * s; o->c = -m.c * s; o->d = m.a * s;
    o->e = -(o->a * m.e + o->c * m.f); o->f = -(o->b * m.e + o->d * m.f);
    return 1;
}

/* --- the separate walk: HarfBuzz's limits and variable paints --------------------- */

typedef struct ylay__walk {
    const uint8_t* t; uint32_t n;            /* the COLR table                  */
    uint32_t layer_list;                      /* offset, 0: none                */
    uint32_t edges;
    int      too_deep, var;
} ylay__walk;

static uint32_t ylay__rd16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t ylay__rd24(const uint8_t* p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint32_t ylay__rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Counts the paint at offset o and below it the way HarfBuzz recurses: one
 * edge per paint visited, depth from 1 at the root. Stops past the limits;
 * a cycle reaches the depth limit. Malformed offsets end the branch
 * (HarfBuzz's sanitizer has passed the table already). */
static void ylay__walk_paint(ylay__walk* w, uint32_t o, int depth) {
    uint32_t fmt;
    if (w->too_deep || w->edges > YLAY__HB_EDGES) return;
    if (depth > YLAY__HB_DEPTH - 1) { w->too_deep = 1; return; }
    if (++w->edges > YLAY__HB_EDGES) return;
    if (o >= w->n) return;
    fmt = w->t[o];
    if (fmt >= 3 && fmt <= 31 && (fmt & 1u) && fmt != 11) w->var = 1;   /* the PaintVar* formats; 11 is PaintColrGlyph */
    switch (fmt) {
    case 1: {
        uint32_t nl, first, i;
        if (o + 6 > w->n || !w->layer_list || w->layer_list + 4 > w->n) return;
        nl = w->t[o + 1]; first = ylay__rd32(w->t + o + 2);
        for (i = 0; i < nl; i++) {
            uint64_t at = (uint64_t)w->layer_list + 4 + 4 * ((uint64_t)first + i);
            if (at + 4 > w->n || (uint64_t)first + i >= ylay__rd32(w->t + w->layer_list)) return;
            ylay__walk_paint(w, w->layer_list + ylay__rd32(w->t + at), depth + 1);
        }
        return;
    }
    case 10: case 12: case 13: case 14: case 15: case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23:
    case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31:
        if (o + 4 > w->n) return;
        ylay__walk_paint(w, o + ylay__rd24(w->t + o + 1), depth + 1);
        return;
    case 32:
        if (o + 8 > w->n) return;
        ylay__walk_paint(w, o + ylay__rd24(w->t + o + 5), depth + 1);   /* backdrop first, as HarfBuzz */
        ylay__walk_paint(w, o + ylay__rd24(w->t + o + 1), depth + 1);
        return;
    default: return;   /* leaves: solids, gradients, PaintColrGlyph (refused by its callback) */
    }
}

/* --- the paint walk ------------------------------------------------------------- */

typedef struct ylay__fill {
    int      has_glyph, has_rect, kind;     /* kind 1 solid, 2 linear, 3 radial */
    uint32_t gid;
    ylay__m  m;                              /* the glyph clip's, font units     */
    double   rect[4];                        /* font units, y up                 */
    ylay__m  gm;                             /* the gradient's                   */
    double   g[6];
    int      n;
    float    off[YLAY__MAX_STOPS];
    hb_color_t col[YLAY__MAX_STOPS];
    double   mult;
} ylay__fill;

enum { YLAY__F_OUTER = 1, YLAY__F_OVER, YLAY__F_OPACITY };
typedef struct ylay__frame { int kind; uint32_t start; double mult; } ylay__frame;

#define YLAY__STACK 128
typedef struct ylay__pstate {
    const yol_font* yf;
    yol_ctx*        ol;
    double          upem;
    ylay__m         st[YLAY__STACK];         /* transforms                       */
    int             nt;
    /* clips: kind 1 glyph, 2 rect */
    int             ck[YLAY__STACK];
    double          crect[YLAY__STACK][4];
    uint32_t        cgid[YLAY__STACK];
    ylay__m         cm[YLAY__STACK];
    int             nc;
    ylay__frame     fr[YLAY__STACK];
    int             nf;
    ylay__fill*     fills; uint32_t n_fills, cap_fills;
    int             ref;
    int32_t         detail;
    int             nomem;
    yol_path        pa, pb;                  /* scratch for the overlap test     */
} ylay__pstate;

static void ylay__refuse(ylay__pstate* p, int ref, int32_t detail) {
    if (!p->ref) { p->ref = ref; p->detail = detail; }
}
static ylay__m ylay__cur(const ylay__pstate* p) {
    ylay__m id = { 1, 0, 0, 1, 0, 0 };
    return p->nt ? p->st[p->nt - 1] : id;
}
static double ylay__mult(const ylay__pstate* p) { return p->nf ? p->fr[p->nf - 1].mult : 1.0; }

/* The innermost glyph clip (-1: none) and the rect clips intersected. */
static int ylay__clip_now(const ylay__pstate* p, int* has_rect, double r[4]) {
    int k, g = -1, glyphs = 0;
    *has_rect = 0;
    for (k = 0; k < p->nc; k++) {
        if (p->ck[k] == 1) { g = k; glyphs++; }
        else if (p->ck[k] == 2) {
            if (!*has_rect) { memcpy(r, p->crect[k], sizeof(double) * 4); *has_rect = 1; }
            else {
                r[0] = fmax(r[0], p->crect[k][0]); r[1] = fmax(r[1], p->crect[k][1]);
                r[2] = fmin(r[2], p->crect[k][2]); r[3] = fmin(r[3], p->crect[k][3]);
            }
        }
    }
    return glyphs > 1 ? -2 : g;
}

static ylay__fill* ylay__new_fill(ylay__pstate* p, int kind) {
    ylay__fill* f;
    int g;
    if (!ylay__cgrow((void**)&p->fills, &p->cap_fills, p->n_fills + 1, sizeof *p->fills)) { p->nomem = 1; return NULL; }
    f = &p->fills[p->n_fills];
    memset(f, 0, sizeof *f);
    f->kind = kind;
    g = ylay__clip_now(p, &f->has_rect, f->rect);
    if (g == -2) { ylay__refuse(p, YLAY__REF_NESTED_CLIP, 0); return NULL; }
    if (g >= 0) { f->has_glyph = 1; f->gid = p->cgid[g]; f->m = p->cm[g]; }
    f->gm = ylay__cur(p);
    f->mult = ylay__mult(p);
    p->n_fills++;
    return f;
}

static void cb_push_transform(hb_paint_funcs_t* fu, void* d, float xx, float yx, float xy, float yy, float dx, float dy, void* u) {
    ylay__pstate* p = (ylay__pstate*)d;
    ylay__m m;
    (void)fu; (void)u;
    m.a = xx; m.b = yx; m.c = xy; m.d = yy; m.e = dx; m.f = dy;
    if (p->nt == YLAY__STACK) { ylay__refuse(p, YLAY__REF_LIMIT, 0); return; }
    p->st[p->nt] = ylay__mmul(ylay__cur(p), m);
    p->nt++;
}
static void cb_pop_transform(hb_paint_funcs_t* fu, void* d, void* u) {
    ylay__pstate* p = (ylay__pstate*)d;
    (void)fu; (void)u;
    if (p->nt) p->nt--;
}
static hb_bool_t cb_color_glyph(hb_paint_funcs_t* fu, void* d, hb_codepoint_t g, hb_font_t* font, void* u) {
    (void)fu; (void)g; (void)font; (void)u;
    ylay__refuse((ylay__pstate*)d, YLAY__REF_COLR_GLYPH, (int32_t)g);
    return 1;   /* handled: HarfBuzz does not recurse into it */
}
static void cb_push_clip_glyph(hb_paint_funcs_t* fu, void* d, hb_codepoint_t g, hb_font_t* font, void* u) {
    ylay__pstate* p = (ylay__pstate*)d;
    (void)fu; (void)font; (void)u;
    if (p->nc == YLAY__STACK) { ylay__refuse(p, YLAY__REF_LIMIT, 0); return; }
    p->ck[p->nc] = 1; p->cgid[p->nc] = g; p->cm[p->nc] = ylay__cur(p);
    p->nc++;
}
static void cb_push_clip_rect(hb_paint_funcs_t* fu, void* d, float x0, float y0, float x1, float y1, void* u) {
    ylay__pstate* p = (ylay__pstate*)d;
    ylay__m m = ylay__cur(p);
    double X0, Y0, X1, Y1;
    (void)fu; (void)u;
    if (p->nc == YLAY__STACK) { ylay__refuse(p, YLAY__REF_LIMIT, 0); return; }
    /* axis-aligned under m: the box's corners map to a box */
    if (!((m.b == 0 && m.c == 0) || (m.a == 0 && m.d == 0))) { ylay__refuse(p, YLAY__REF_TURNED_CLIP, 0); p->ck[p->nc++] = 0; return; }
    X0 = m.a * x0 + m.c * y0 + m.e; Y0 = m.b * x0 + m.d * y0 + m.f;
    X1 = m.a * x1 + m.c * y1 + m.e; Y1 = m.b * x1 + m.d * y1 + m.f;
    p->ck[p->nc] = 2;
    p->crect[p->nc][0] = fmin(X0, X1); p->crect[p->nc][1] = fmin(Y0, Y1);
    p->crect[p->nc][2] = fmax(X0, X1); p->crect[p->nc][3] = fmax(Y0, Y1);
    p->nc++;
}
static void cb_pop_clip(hb_paint_funcs_t* fu, void* d, void* u) {
    ylay__pstate* p = (ylay__pstate*)d;
    (void)fu; (void)u;
    if (p->nc) p->nc--;
}
static void cb_fill_glyph(hb_paint_funcs_t* fu, void* d, hb_codepoint_t g, hb_font_t* font, hb_bool_t fg, hb_color_t c, void* u) {
    ylay__pstate* p = (ylay__pstate*)d;
    ylay__fill* f;
    (void)fu; (void)font; (void)u;
    if (fg) { ylay__refuse(p, YLAY__REF_FOREGROUND, 0); return; }
    /* a solid in one glyph clip: the clip pushed and popped around it */
    cb_push_clip_glyph(fu, d, g, font, u);
    f = ylay__new_fill(p, 1);
    if (f) { f->n = 1; f->col[0] = c; }
    cb_pop_clip(fu, d, u);
}
static void cb_color(hb_paint_funcs_t* fu, void* d, hb_bool_t fg, hb_color_t c, void* u) {
    ylay__pstate* p = (ylay__pstate*)d;
    ylay__fill* f;
    (void)fu; (void)u;
    if (fg) { ylay__refuse(p, YLAY__REF_FOREGROUND, 0); return; }
    f = ylay__new_fill(p, 1);
    if (f) { f->n = 1; f->col[0] = c; }
}
static hb_bool_t cb_image(hb_paint_funcs_t* fu, void* d, hb_blob_t* b, unsigned w, unsigned h, hb_tag_t fmt, float sl,
                          hb_glyph_extents_t* ex, void* u) {
    (void)fu; (void)b; (void)w; (void)h; (void)fmt; (void)sl; (void)ex; (void)u;
    ylay__refuse((ylay__pstate*)d, YLAY__REF_IMAGE, 0);
    return 1;
}
/* The color line into f: pad only, 1 to 16 stops, none from the foreground. */
static int ylay__stops(ylay__pstate* p, ylay__fill* f, hb_color_line_t* cl) {
    hb_color_stop_t s[YLAY__MAX_STOPS + 1];
    unsigned total = hb_color_line_get_color_stops(cl, 0, NULL, NULL), n = YLAY__MAX_STOPS + 1, i, j;
    hb_paint_extend_t x = hb_color_line_get_extend(cl);
    if (x != HB_PAINT_EXTEND_PAD) { ylay__refuse(p, YLAY__REF_EXTEND, (int32_t)x); return 0; }
    if (total < 1 || total > YLAY__MAX_STOPS) { ylay__refuse(p, YLAY__REF_STOPS, (int32_t)total); return 0; }
    hb_color_line_get_color_stops(cl, 0, &n, s);
    if (n != total) { ylay__refuse(p, YLAY__REF_STOPS, (int32_t)total); return 0; }
    for (i = 0; i < n; i++) {
        if (s[i].is_foreground) { ylay__refuse(p, YLAY__REF_FOREGROUND, 0); return 0; }
        if (!isfinite(s[i].offset)) { ylay__refuse(p, YLAY__REF_DEGENERATE, 0); return 0; }
    }
    for (i = 1; i < n; i++)   /* stable: equal offsets keep their order (a hard edge) */
        for (j = i; j > 0 && s[j - 1].offset > s[j].offset; j--) { hb_color_stop_t t = s[j]; s[j] = s[j - 1]; s[j - 1] = t; }
    f->n = (int)n;
    for (i = 0; i < n; i++) { f->off[i] = s[i].offset; f->col[i] = s[i].color; }
    return 1;
}
static void cb_linear(hb_paint_funcs_t* fu, void* d, hb_color_line_t* cl, float x0, float y0, float x1, float y1, float x2, float y2, void* u) {
    ylay__pstate* p = (ylay__pstate*)d;
    ylay__fill* f;
    (void)fu; (void)u;
    if (p->ref) return;
    f = ylay__new_fill(p, 2);
    if (!f) return;
    f->g[0] = x0; f->g[1] = y0; f->g[2] = x1; f->g[3] = y1; f->g[4] = x2; f->g[5] = y2;
    if (!ylay__stops(p, f, cl)) p->n_fills--;
}
static void cb_radial(hb_paint_funcs_t* fu, void* d, hb_color_line_t* cl, float x0, float y0, float r0, float x1, float y1, float r1, void* u) {
    ylay__pstate* p = (ylay__pstate*)d;
    ylay__fill* f;
    (void)fu; (void)u;
    if (p->ref) return;
    f = ylay__new_fill(p, 3);
    if (!f) return;
    f->g[0] = x0; f->g[1] = y0; f->g[2] = r0; f->g[3] = x1; f->g[4] = y1; f->g[5] = r1;
    if (!ylay__stops(p, f, cl)) p->n_fills--;
}
static void cb_sweep(hb_paint_funcs_t* fu, void* d, hb_color_line_t* cl, float x0, float y0, float a0, float a1, void* u) {
    (void)fu; (void)cl; (void)x0; (void)y0; (void)a0; (void)a1; (void)u;
    ylay__refuse((ylay__pstate*)d, YLAY__REF_SWEEP, 0);
}

static int ylay__fill_path(const ylay__pstate* p, const ylay__fill* f, yol_path* out);
static int ylay__overlap(ylay__pstate* p, uint32_t a, uint32_t b);

static void ylay__push_frame(ylay__pstate* p, int kind, double mult) {
    if (p->nf == YLAY__STACK) { ylay__refuse(p, YLAY__REF_LIMIT, 0); return; }
    p->fr[p->nf].kind = kind; p->fr[p->nf].start = p->n_fills; p->fr[p->nf].mult = mult;
    p->nf++;
}
static void cb_push_group_for(hb_paint_funcs_t* fu, void* d, hb_paint_composite_mode_t mode, void* u) {
    ylay__pstate* p = (ylay__pstate*)d;
    (void)fu; (void)u;
    if (p->nf && p->fr[p->nf - 1].kind == YLAY__F_OUTER) {
        /* PaintComposite's inner group: the backdrop is drawn, the source follows */
        ylay__frame* o = &p->fr[p->nf - 1];
        if (mode == HB_PAINT_COMPOSITE_MODE_SRC_OVER) { ylay__push_frame(p, YLAY__F_OVER, ylay__mult(p)); return; }
        if (mode == HB_PAINT_COMPOSITE_MODE_SRC_IN) {
            /* group opacity (nanoemoji's SVG opacity): a backdrop of one
             * solid with no glyph clip. The source at the solid's alpha */
            const ylay__fill* b = p->n_fills == o->start + 1 ? &p->fills[o->start] : NULL;
            if (b && b->kind == 1 && !b->has_glyph) {
                double a = hb_color_get_alpha(b->col[0]) / 255.0 * b->mult;
                p->n_fills = o->start;   /* the backdrop is the alpha, not a layer */
                ylay__push_frame(p, YLAY__F_OPACITY, a);
                return;
            }
            ylay__refuse(p, YLAY__REF_MASK, (int32_t)mode);
        } else {
            ylay__refuse(p, YLAY__REF_COMPOSITE, (int32_t)mode);
        }
        ylay__push_frame(p, YLAY__F_OVER, ylay__mult(p));
        return;
    }
    if (mode != HB_PAINT_COMPOSITE_MODE_SRC_OVER) ylay__refuse(p, YLAY__REF_COMPOSITE, (int32_t)mode);
    ylay__push_frame(p, YLAY__F_OUTER, ylay__mult(p));
}
static void cb_push_group(hb_paint_funcs_t* fu, void* d, void* u) { cb_push_group_for(fu, d, HB_PAINT_COMPOSITE_MODE_SRC_OVER, u); }
static void cb_pop_group(hb_paint_funcs_t* fu, void* d, hb_paint_composite_mode_t mode, void* u) {
    ylay__pstate* p = (ylay__pstate*)d;
    (void)fu; (void)u; (void)mode;
    if (!p->nf) return;
    p->nf--;
    if (p->fr[p->nf].kind == YLAY__F_OPACITY && p->fr[p->nf].mult < 1.0 && !p->ref && !p->nomem) {
        /* per-layer alpha equals the faded group only where the layers do not overlap */
        uint32_t i, j;
        for (i = p->fr[p->nf].start; i < p->n_fills && !p->ref; i++)
            for (j = i + 1; j < p->n_fills && !p->ref; j++)
                if (ylay__overlap(p, i, j)) ylay__refuse(p, YLAY__REF_OVERLAP, 0);
    }
}

/* --- outlines ----------------------------------------------------------------- */

/* The em transform of a font-unit matrix: F m F^-1, F = diag(1/upem, -1/upem). */
static yol_xform ylay__em_xf(ylay__m m, double upem) {
    yol_xform x;
    x.a = m.a; x.b = -m.b; x.c = -m.c; x.d = m.d; x.e = m.e / upem; x.f = -m.f / upem;
    return x;
}
/* A font-unit box (y up) in em (y down). */
static void ylay__em_box(const double r[4], double upem, double o[4]) {
    o[0] = r[0] / upem; o[1] = -r[3] / upem; o[2] = r[2] / upem; o[3] = -r[1] / upem;
}

/* Contours of in clipped to one side of an axis line: axis 0 x, 1 y; keep
 * coordinates >= v (ge) or <= v. Each contour's pieces outside are replaced
 * by the boundary line between where it leaves and where it comes back,
 * which keeps the winding of every point strictly inside; the resolve that
 * follows removes the boundary's overlaps. Crossing points sit on v exactly. */
static int ylay__clip_half(yol_path* out, const yol_path* in, int axis, double v, int ge) {
    int32_t k;
    yol_path_clear(out);
    out->rule = in->rule;
    for (k = 0; k < in->n_contours; k++) {
        int32_t s = (int32_t)(in->contours[k] & ~YOL_OPEN);
        int32_t e = k + 1 < in->n_contours ? (int32_t)(in->contours[k + 1] & ~YOL_OPEN) : in->n_pts;
        int32_t i;
        int started = 0, gap = 0;
        double fx = 0, fy = 0, lx = 0, ly = 0;
        for (i = s; i + 2 < e; i += 2) {
            const double* q = in->pts + 2 * i;
            double P0[2], C[2], P1[2], t[2], cut[3];
            int nt = 0, line, j;
            P0[0] = q[0]; P0[1] = q[1]; C[0] = q[2]; C[1] = q[3]; P1[0] = q[4]; P1[1] = q[5];
            line = C[0] == P0[0] && C[1] == P0[1];
            {   /* parameters in (0, 1) where the coordinate is v */
                double a = P0[axis] - 2 * C[axis] + P1[axis], b = 2 * (C[axis] - P0[axis]), c = P0[axis] - v;
                if (line) {
                    double dd = P1[axis] - P0[axis];
                    if (dd != 0) { double r = sqrt(fmax(0.0, (v - P0[axis]) / dd)); if ((v - P0[axis]) / dd > 0 && (v - P0[axis]) / dd < 1) t[nt++] = r; }
                } else if (fabs(a) < 1e-15 * (fabs(b) + fabs(c) + 1e-300)) {
                    if (b != 0) { double r = -c / b; if (r > 0 && r < 1) t[nt++] = r; }
                } else {
                    double D = b * b - 4 * a * c;
                    if (D > 0) {
                        double sq = sqrt(D), qq = -0.5 * (b + (b >= 0 ? sq : -sq)), r1 = qq / a, r2 = qq != 0 ? c / qq : r1;
                        if (r1 > 0 && r1 < 1) t[nt++] = r1;
                        if (r2 > 0 && r2 < 1 && r2 != r1) t[nt++] = r2;
                        if (nt == 2 && t[0] > t[1]) { double tt = t[0]; t[0] = t[1]; t[1] = tt; }
                    }
                }
            }
            cut[0] = 0; for (j = 0; j < nt; j++) cut[j + 1] = t[j];
            for (j = 0; j <= nt; j++) {
                double ta = cut[j], tb = j < nt ? cut[j + 1] : 1.0, A[2], B[2], M[2], tm, mid;
                int m2, in_side;
                /* the piece [ta, tb] of the quadratic, by its values and the
                 * control point that the tangents at its ends meet in */
                for (m2 = 0; m2 < 2; m2++) {
                    double u = 1 - ta, w = 1 - tb;
                    A[m2] = u * u * P0[m2] + 2 * ta * u * C[m2] + ta * ta * P1[m2];
                    B[m2] = w * w * P0[m2] + 2 * tb * w * C[m2] + tb * tb * P1[m2];
                    /* the control point of the sub-curve: B(ta) + (tb - ta) B'(ta) / 2 */
                    M[m2] = A[m2] + (tb - ta) * ((1 - ta) * (C[m2] - P0[m2]) + ta * (P1[m2] - C[m2]));
                }
                if (j > 0) A[axis] = v;
                if (j < nt) B[axis] = v;
                if (j == 0) { A[0] = P0[0]; A[1] = P0[1]; }
                if (j == nt) { B[0] = P1[0]; B[1] = P1[1]; }
                tm = 0.5 * (ta + tb);
                mid = (1 - tm) * (1 - tm) * P0[axis] + 2 * tm * (1 - tm) * C[axis] + tm * tm * P1[axis];
                in_side = ge ? mid >= v : mid <= v;
                if (!in_side) { if (started) gap = 1; continue; }
                if (!started) { yol_move(out, A[0], A[1]); fx = A[0]; fy = A[1]; started = 1; }
                else if (gap || A[0] != lx || A[1] != ly) yol_line(out, A[0], A[1]);
                gap = 0;
                if (line) yol_line(out, B[0], B[1]);
                else yol_quad(out, M[0], M[1], B[0], B[1]);
                lx = B[0]; ly = B[1];
            }
        }
        if (started) {
            if (lx != fx || ly != fy) yol_line(out, fx, fy);
            yol_close_path(out);
        }
    }
    return yol_path_end(out);
}

/* The outline of layer glyph def lg into out, em, y down, unresolved. */
static int ylay__lglyph_path(const ylay__lglyph* lg, const yol_font* yf, yol_ctx* ol, yol_path* out, yol_path* tmp) {
    int rc = YOL_OK, k;
    yol_glyph_desc gd;
    yol_xform x;
    yol_path_clear(out);
    x.a = lg->m[0]; x.b = lg->m[1]; x.c = lg->m[2]; x.d = lg->m[3]; x.e = lg->m[4]; x.f = lg->m[5];
    if (lg->gid == UINT32_MAX) {
        yol_set_xform(out, yol_xf_identity());
        yol_rect(out, lg->box[0], lg->box[1], lg->box[2] - lg->box[0], lg->box[3] - lg->box[1], 0, 0);
        return yol_path_end(out);
    }
    yol_set_xform(out, x);
    memset(&gd, 0, sizeof gd);
    rc = yol_font_glyph(ol, yf, lg->gid, out, &gd);
    yol_set_xform(out, yol_xf_identity());
    if (rc < 0 || !lg->cut) return rc;
    for (k = 0; k < 4 && rc >= 0; k++) {
        /* x >= x0, y >= y0, x <= x1, y <= y1, through tmp and back */
        rc = ylay__clip_half(tmp, out, k & 1, lg->box[k], k < 2);
        if (rc >= 0) rc = yol_path_copy(out, tmp);
    }
    return rc;
}

/* The fill's outline and its cut, as a layer glyph def. Returns 1 with
 * *lg set, 0 when the outline is wholly outside its clip box (no layer). */
static int ylay__fill_lglyph(const ylay__pstate* p, const ylay__fill* f, ylay__lglyph* lg, yol_path* tmp, int* rc) {
    yol_xform x;
    double b[4];
    memset(lg, 0, sizeof *lg);
    *rc = YOL_OK;
    if (f->has_rect) ylay__em_box(f->rect, p->upem, b);
    if (!f->has_glyph) {
        lg->gid = UINT32_MAX;
        lg->m[0] = lg->m[3] = 1;
        memcpy(lg->box, b, sizeof b);
        lg->cut = 1;
        return b[2] > b[0] && b[3] > b[1];
    }
    x = ylay__em_xf(f->m, p->upem);
    lg->gid = f->gid;
    lg->m[0] = x.a; lg->m[1] = x.b; lg->m[2] = x.c; lg->m[3] = x.d; lg->m[4] = x.e; lg->m[5] = x.f;
    if (f->has_rect) {
        /* cut only where the box cuts the outline's control points' box */
        double lo[2] = { 1e300, 1e300 }, hi[2] = { -1e300, -1e300 };
        int32_t i;
        *rc = ylay__lglyph_path(lg, p->yf, p->ol, tmp, NULL);
        if (*rc < 0) return 0;
        for (i = 0; i < tmp->n_pts; i++) {
            int k;
            for (k = 0; k < 2; k++) {
                double v = tmp->pts[2 * i + k];
                if (v < lo[k]) lo[k] = v;
                if (v > hi[k]) hi[k] = v;
            }
        }
        if (tmp->n_pts == 0) return 0;
        if (hi[0] <= b[0] || lo[0] >= b[2] || hi[1] <= b[1] || lo[1] >= b[3]) return 0;
        if (lo[0] < b[0] || lo[1] < b[1] || hi[0] > b[2] || hi[1] > b[3]) { lg->cut = 1; memcpy(lg->box, b, sizeof b); }
    }
    return 1;
}

static int ylay__fill_path(const ylay__pstate* p, const ylay__fill* f, yol_path* out) {
    ylay__lglyph lg;
    yol_path tmp;
    int rc, any;
    yol_path_init(&tmp, p->ol);
    any = ylay__fill_lglyph(p, f, &lg, &tmp, &rc);
    yol_path_clear(out);
    if (rc >= 0 && any) {
        yol_path cut;
        yol_path_init(&cut, p->ol);
        rc = ylay__lglyph_path(&lg, p->yf, p->ol, out, &cut);
        yol_path_free(&cut);
    }
    yol_path_free(&tmp);
    if (rc >= 0 && out->n_pts) rc = yol_resolve(p->ol, out, out);
    return rc;
}

/* Whether fills a and b overlap by more than rounding: their areas against
 * the area of their union, each resolved (winding 0 or 1). */
static int ylay__overlap(ylay__pstate* p, uint32_t a, uint32_t b) {
    double sa, sb, su;
    int32_t k;
    if (ylay__fill_path(p, &p->fills[a], &p->pa) < 0 || ylay__fill_path(p, &p->fills[b], &p->pb) < 0) return 1;
    if (!p->pa.n_pts || !p->pb.n_pts) return 0;
    sa = fabs(yol_path_area(&p->pa)); sb = fabs(yol_path_area(&p->pb));
    for (k = 0; k < p->pb.n_contours; k++) {   /* pb's contours appended to pa */
        int32_t s = (int32_t)(p->pb.contours[k] & ~YOL_OPEN);
        int32_t e = k + 1 < p->pb.n_contours ? (int32_t)(p->pb.contours[k + 1] & ~YOL_OPEN) : p->pb.n_pts, i;
        yol_move(&p->pa, p->pb.pts[2 * s], p->pb.pts[2 * s + 1]);
        for (i = s + 1; i + 1 < e; i += 2) yol_quad(&p->pa, p->pb.pts[2 * i], p->pb.pts[2 * i + 1], p->pb.pts[2 * i + 2], p->pb.pts[2 * i + 3]);
        yol_close_path(&p->pa);
    }
    p->pa.flags = 0;
    if (yol_path_end(&p->pa) < 0 || yol_resolve(p->ol, &p->pa, &p->pa) < 0) return 1;
    su = fabs(yol_path_area(&p->pa));
    return sa + sb - su > 1e-7 * (sa + sb);
}

/* --- paint records (docs/gfx.md, "Paint records") ------------------------------- */

/* sRGB code to linear light (IEC 61966-2-1). */
static double ylay__lin(unsigned c) {
    double v = c / 255.0;
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}
static void ylay__premul(hb_color_t c, double mult, float* o) {
    double a = hb_color_get_alpha(c) / 255.0 * mult;
    o[0] = (float)(ylay__lin(hb_color_get_red(c)) * a);
    o[1] = (float)(ylay__lin(hb_color_get_green(c)) * a);
    o[2] = (float)(ylay__lin(hb_color_get_blue(c)) * a);
    o[3] = (float)a;
}

/* The record of fill f into rec (texels of 4 floats); returns its texel
 * count, or 0 with the refusal set. */
static uint32_t ylay__paint_rec(ylay__pstate* p, const ylay__fill* f, float* rec) {
    uint32_t n = 0, i, nofs;
    ylay__m F = { 0, 0, 0, 0, 0, 0 }, G, A;
    memset(rec, 0, sizeof(float) * 4 * (3 + 4 + YLAY__MAX_STOPS));
    if (f->kind == 1) {
        rec[0] = 1;
        ylay__premul(f->col[0], f->mult, rec + 4);
        return 2;
    }
    F.a = 1.0 / p->upem; F.d = -1.0 / p->upem;
    G = ylay__mmul(F, f->gm);              /* gradient space to em */
    if (!ylay__minv(G, &A)) { ylay__refuse(p, YLAY__REF_DEGENERATE, 1); return 0; }
    rec[0] = (float)f->kind; rec[1] = (float)f->n;
    if (f->kind == 2) {
        /* t = n . (A P - p0) / n . (p1 - p0), n normal to p2 - p0 (COLRv1) */
        double nx = -(f->g[5] - f->g[1]), ny = f->g[4] - f->g[0];
        double den = nx * (f->g[2] - f->g[0]) + ny * (f->g[3] - f->g[1]);
        if (!(fabs(den) > 1e-9 * (nx * nx + ny * ny + 1e-300)) || (nx == 0 && ny == 0)) { ylay__refuse(p, YLAY__REF_DEGENERATE, 0); return 0; }
        rec[4] = (float)((nx * A.a + ny * A.b) / den);
        rec[5] = (float)((nx * A.c + ny * A.d) / den);
        rec[6] = (float)((nx * (A.e - f->g[0]) + ny * (A.f - f->g[1])) / den);
        n = 2;
    } else {
        double r0 = f->g[2], r1 = f->g[5], dx = f->g[3] - f->g[0], dy = f->g[4] - f->g[1];
        if (r0 != 0) { ylay__refuse(p, YLAY__REF_RADIAL_R0, 0); return 0; }
        if (!(r1 > 0) || !(dx * dx + dy * dy < r1 * r1)) { ylay__refuse(p, YLAY__REF_RADIAL_FOCAL, 0); return 0; }
        /* q = (A P - c0) / r1, d = (c1 - c0) / r1: |q - t d| = t */
        rec[4] = (float)(A.a / r1); rec[5] = (float)(A.c / r1); rec[6] = (float)((A.e - f->g[0]) / r1); rec[7] = (float)(A.b / r1);
        rec[8] = (float)(A.d / r1); rec[9] = (float)((A.f - f->g[1]) / r1); rec[10] = (float)(dx / r1); rec[11] = (float)(dy / r1);
        n = 3;
    }
    nofs = ((uint32_t)f->n + 3) / 4;
    for (i = 0; i < (uint32_t)f->n; i++) rec[4 * n + i] = f->off[i];
    for (i = 0; i < (uint32_t)f->n; i++) ylay__premul(f->col[i], f->mult, rec + 4 * (n + nofs + i));
    for (i = 0; i < 4 * (n + nofs + (uint32_t)f->n); i++)
        if (!isfinite(rec[i])) { ylay__refuse(p, YLAY__REF_DEGENERATE, 2); return 0; }
    return n + nofs + (uint32_t)f->n;
}

/* --- dedupe: open addressing on FNV-1a ------------------------------------------- */

typedef struct ylay__hash { uint32_t* slot; uint32_t cap, n; } ylay__hash;

static uint64_t ylay__fnv(const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
/* Finds the slot of key k (value + 1 in slot, 0 empty) with eq(k, value). */
static int ylay__hgrow(ylay__hash* h) {
    if ((uint64_t)(h->n + 1) * 2 <= h->cap) return 1;
    return 0;
}

/* --- the build ------------------------------------------------------------------ */

static int ylay__lg_same(const ylay__lglyph* a, const ylay__lglyph* b) {
    return a->gid == b->gid && a->cut == b->cut && !memcmp(a->m, b->m, sizeof a->m) && (!a->cut || !memcmp(a->box, b->box, sizeof a->box));
}

typedef struct ylay__tables {
    ylay__hash lgh;            /* layer glyph defs, by value               */
    ylay__hash ph;             /* paint records: start texel, by contents  */
    uint32_t*  plen;           /* per paint start texel: its length; sized cap_paint */
} ylay__tables;

static int ylay__rehash(ylay__hash* h, uint32_t cap) {
    free(h->slot);
    h->slot = (uint32_t*)calloc(cap, sizeof(uint32_t));
    h->cap = cap;
    h->n = 0;
    return h->slot != NULL;
}

static uint64_t ylay__lg_hash(const ylay__lglyph* lg) {
    uint64_t h = ylay__fnv(&lg->gid, 4) ^ (ylay__fnv(lg->m, sizeof lg->m) * 31u);
    if (lg->cut) h ^= ylay__fnv(lg->box, sizeof lg->box) * 131u;
    return h;
}

/* The set glyph of a layer glyph def: an existing one, or a new id. */
static int ylay__lg_find(ylay__colr* c, ylay__tables* t, const ylay__lglyph* lg, uint32_t* out) {
    uint32_t i, s;
    if (!ylay__hgrow(&t->lgh)) {
        uint32_t cap = t->lgh.cap ? 2 * t->lgh.cap : 1024;
        if (!ylay__rehash(&t->lgh, cap)) return 0;
        for (i = 0; i < c->n_lg; i++) {
            s = (uint32_t)(ylay__lg_hash(&c->lg[i]) & (cap - 1));
            while (t->lgh.slot[s]) s = (s + 1) & (cap - 1);
            t->lgh.slot[s] = i + 1; t->lgh.n++;
        }
    }
    s = (uint32_t)(ylay__lg_hash(lg) & (t->lgh.cap - 1));
    while (t->lgh.slot[s]) {
        if (ylay__lg_same(&c->lg[t->lgh.slot[s] - 1], lg)) { *out = c->n_glyphs + t->lgh.slot[s] - 1; return 1; }
        s = (s + 1) & (t->lgh.cap - 1);
    }
    if (c->n_glyphs + c->n_lg + 1 >= (1u << 24)) return 0;
    if (!ylay__cgrow((void**)&c->lg, &c->cap_lg, c->n_lg + 1, sizeof *c->lg)) return 0;
    c->lg[c->n_lg] = *lg;
    t->lgh.slot[s] = c->n_lg + 1; t->lgh.n++;
    *out = c->n_glyphs + c->n_lg++;
    return 1;
}

/* The texel index of a paint record: an existing one, or appended. */
static int ylay__paint_find(ylay__colr* c, ylay__tables* t, const float* rec, uint32_t n, uint32_t* out) {
    uint32_t i, s;
    if (!ylay__hgrow(&t->ph)) {
        uint32_t cap = t->ph.cap ? 2 * t->ph.cap : 1024, at;
        if (!ylay__rehash(&t->ph, cap)) return 0;
        for (at = 0; at < c->n_paint; at += t->plen[at]) {
            s = (uint32_t)(ylay__fnv(c->paints + 4 * (size_t)at, 16 * (size_t)t->plen[at]) & (cap - 1));
            while (t->ph.slot[s]) s = (s + 1) & (cap - 1);
            t->ph.slot[s] = at + 1; t->ph.n++;
        }
    }
    s = (uint32_t)(ylay__fnv(rec, 16 * (size_t)n) & (t->ph.cap - 1));
    while (t->ph.slot[s]) {
        uint32_t at = t->ph.slot[s] - 1;
        if (t->plen[at] == n && !memcmp(c->paints + 4 * (size_t)at, rec, 16 * (size_t)n)) { *out = at; return 1; }
        s = (s + 1) & (t->ph.cap - 1);
    }
    {
        uint32_t old = c->cap_paint;
        if (c->n_paint + n >= (1u << 24)) return 0;
        if (!ylay__cgrow((void**)&c->paints, &c->cap_paint, c->n_paint + n, 16)) return 0;
        if (c->cap_paint != old) {
            uint32_t* q = (uint32_t*)realloc(t->plen, sizeof(uint32_t) * c->cap_paint);
            if (!q) return 0;
            t->plen = q;
        }
    }
    memcpy(c->paints + 4 * (size_t)c->n_paint, rec, 16 * (size_t)n);
    for (i = 0; i < n; i++) t->plen[c->n_paint + i] = 0;
    t->plen[c->n_paint] = n;
    t->ph.slot[s] = c->n_paint + 1; t->ph.n++;
    *out = c->n_paint;
    c->n_paint += n;
    return 1;
}

static hb_paint_funcs_t* ylay__funcs(void) {
    hb_paint_funcs_t* pf = hb_paint_funcs_create();
    hb_paint_funcs_set_push_transform_func(pf, cb_push_transform, NULL, NULL);
    hb_paint_funcs_set_pop_transform_func(pf, cb_pop_transform, NULL, NULL);
    hb_paint_funcs_set_color_glyph_func(pf, cb_color_glyph, NULL, NULL);
    hb_paint_funcs_set_fill_glyph_func(pf, cb_fill_glyph, NULL, NULL);
    hb_paint_funcs_set_push_clip_glyph_func(pf, cb_push_clip_glyph, NULL, NULL);
    hb_paint_funcs_set_push_clip_rectangle_func(pf, cb_push_clip_rect, NULL, NULL);
    hb_paint_funcs_set_pop_clip_func(pf, cb_pop_clip, NULL, NULL);
    hb_paint_funcs_set_color_func(pf, cb_color, NULL, NULL);
    hb_paint_funcs_set_image_func(pf, cb_image, NULL, NULL);
    hb_paint_funcs_set_linear_gradient_func(pf, cb_linear, NULL, NULL);
    hb_paint_funcs_set_radial_gradient_func(pf, cb_radial, NULL, NULL);
    hb_paint_funcs_set_sweep_gradient_func(pf, cb_sweep, NULL, NULL);
    hb_paint_funcs_set_push_group_func(pf, cb_push_group, NULL, NULL);
    hb_paint_funcs_set_push_group_for_func(pf, cb_push_group_for, NULL, NULL);
    hb_paint_funcs_set_pop_group_func(pf, cb_pop_group, NULL, NULL);
    hb_paint_funcs_make_immutable(pf);
    return pf;
}

/* COLR's v1 header and its glyph's root paint, for the separate walk. */
static uint32_t ylay__colr_root(const uint8_t* t, uint32_t n, uint32_t gid) {
    uint32_t bgl, cnt, lo, hi;
    if (n < 34 || ylay__rd16(t) < 1) return 0;
    bgl = ylay__rd32(t + 14);
    if (!bgl || (uint64_t)bgl + 4 > n) return 0;
    cnt = ylay__rd32(t + bgl);
    if ((uint64_t)bgl + 4 + 6 * (uint64_t)cnt > n) return 0;
    lo = 0; hi = cnt;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2, g = ylay__rd16(t + bgl + 4 + 6 * mid);
        if (g == gid) return bgl + ylay__rd32(t + bgl + 4 + 6 * mid + 2);
        if (g < gid) lo = mid + 1; else hi = mid;
    }
    return 0;
}

int ylay__colr_build(ylay__colr* c, hb_face_t* face, const yol_font* yf, yol_ctx* ol, char* err, size_t cap) {
    hb_font_t* font = NULL;
    hb_paint_funcs_t* pf = NULL;
    hb_blob_t* colr = NULL;
    ylay__pstate* p = NULL;
    ylay__tables t;
    ylay__walk w;
    const uint8_t* ct;
    uint32_t cn, g, ng = (uint32_t)yf->n_glyphs, i;
    int rc = YLAY_OK, var_table = 0;
    double t0 = ylay__colr_now_us();
    memset(c, 0, sizeof *c);
    memset(&t, 0, sizeof t);
    c->n_glyphs = ng;
    c->G = ng;
    c->first = (uint32_t*)calloc(ng ? ng : 1, sizeof *c->first);
    c->count = (uint16_t*)calloc(ng ? ng : 1, sizeof *c->count);
    c->ref = (uint8_t*)calloc(ng ? ng : 1, 1);
    c->detail = (int32_t*)calloc(ng ? ng : 1, sizeof *c->detail);
    c->is_color = (uint8_t*)calloc(ng ? ng : 1, 1);
    p = (ylay__pstate*)calloc(1, sizeof *p);
    font = hb_font_create(face);
    pf = ylay__funcs();
    if (!c->first || !c->count || !c->ref || !c->detail || !c->is_color || !p || !font || !pf) { rc = YLAY_ERR_NOMEM; goto done; }
    hb_font_set_scale(font, (int)hb_face_get_upem(face), (int)hb_face_get_upem(face));   /* font units: the identity */
    p->yf = yf; p->ol = ol; p->upem = hb_face_get_upem(face);
    yol_path_init(&p->pa, ol);
    yol_path_init(&p->pb, ol);
    colr = hb_face_reference_table(face, HB_TAG('C', 'O', 'L', 'R'));
    ct = (const uint8_t*)hb_blob_get_data(colr, &cn);
    memset(&w, 0, sizeof w);
    w.t = ct; w.n = cn;
    if (cn >= 34 && ylay__rd16(ct) >= 1) {
        w.layer_list = ylay__rd32(ct + 18);
        var_table = ylay__rd32(ct + 26) != 0 || ylay__rd32(ct + 30) != 0;
    }
    for (g = 0; g < ng && rc == YLAY_OK; g++) {
        int v1 = hb_ot_color_glyph_has_paint(face, g);
        uint32_t r;
        if (!v1 && hb_ot_color_glyph_get_layers(face, g, 0, NULL, NULL) == 0) continue;
        c->is_color[g] = 1;
        c->n_color++;
        p->nt = p->nc = p->nf = 0;
        p->n_fills = 0;
        p->ref = 0; p->detail = 0;
        if (v1 && (r = ylay__colr_root(ct, cn, g)) != 0) {
            w.edges = 0; w.too_deep = 0; w.var = 0;
            ylay__walk_paint(&w, r, 1);
            if (w.too_deep || w.edges >= YLAY__HB_EDGES) ylay__refuse(p, YLAY__REF_LIMIT, 0);
            else if (w.var || var_table) ylay__refuse(p, YLAY__REF_VARIABLE, 0);
        }
        if (!p->ref) hb_font_paint_glyph(font, g, pf, p, 0, HB_COLOR(0, 0, 0, 255));
        if (p->nomem) { rc = YLAY_ERR_NOMEM; break; }
        c->first[g] = c->n_layers;
        if (!p->ref) {
            uint32_t k, start = c->n_layers;
            for (k = 0; k < p->n_fills && !p->ref; k++) {
                const ylay__fill* f = &p->fills[k];
                ylay__lglyph lg;
                uint32_t sg = 0, pi = 0, n;
                float rec[4 * (3 + 4 + YLAY__MAX_STOPS)];
                int frc = YOL_OK, any;
                if (!f->has_glyph && !f->has_rect) { ylay__refuse(p, YLAY__REF_UNCLIPPED, 0); break; }
                /* HarfBuzz passes COLR's glyph ids on unchecked */
                if (f->has_glyph && f->gid >= ng) { ylay__refuse(p, YLAY__REF_GLYPH_ID, (int32_t)f->gid); break; }
                any = ylay__fill_lglyph(p, f, &lg, &p->pa, &frc);
                if (frc < 0) { ylay__refuse(p, YLAY__REF_OUTLINE, frc); break; }
                if (!any) continue;
                n = ylay__paint_rec(p, f, rec);
                if (!n) break;
                if (lg.gid != UINT32_MAX && !lg.cut && lg.m[0] == 1 && lg.m[1] == 0 && lg.m[2] == 0 && lg.m[3] == 1 &&
                    lg.m[4] == 0 && lg.m[5] == 0)
                    sg = lg.gid;   /* the font's own glyph: no layer glyph */
                else if (!ylay__lg_find(c, &t, &lg, &sg)) { rc = YLAY_ERR_NOMEM; break; }
                if (!ylay__paint_find(c, &t, rec, n, &pi)) { rc = YLAY_ERR_NOMEM; break; }
                if (!ylay__cgrow((void**)&c->layers, &c->cap_layers, c->n_layers + 1, sizeof *c->layers)) { rc = YLAY_ERR_NOMEM; break; }
                c->layers[c->n_layers].glyph = sg;
                c->layers[c->n_layers].paint = pi;
                c->n_layers++;
            }
            if (p->ref) c->n_layers = start;   /* a refusal found late keeps none */
            else if (c->n_layers - start > 0xFFFF) { p->ref = YLAY__REF_LIMIT; c->n_layers = start; }
        }
        if (p->ref) {
            c->ref[g] = (uint8_t)p->ref;
            c->detail[g] = p->detail;
            c->n_refused++;
            c->first[g] = 0;
        } else {
            c->count[g] = (uint16_t)(c->n_layers - c->first[g]);
        }
    }
    c->G = ng + c->n_lg;
done:
    if (rc == YLAY_ERR_NOMEM && err && cap) snprintf(err, cap, "no memory for the color glyphs");
    if (p) { yol_path_free(&p->pa); yol_path_free(&p->pb); free(p->fills); }
    free(p);
    free(t.lgh.slot); free(t.ph.slot); free(t.plen);
    hb_blob_destroy(colr);
    hb_paint_funcs_destroy(pf);
    hb_font_destroy(font);
    c->build_us = ylay__colr_now_us() - t0;
    (void)i;
    return rc;
}

void ylay__colr_free(ylay__colr* c) {
    if (!c) return;
    free(c->first); free(c->count); free(c->ref); free(c->detail); free(c->is_color);
    free(c->layers); free(c->lg); free(c->paints);
    memset(c, 0, sizeof *c);
}

int ylay__colr_layer_path(const ylay__colr* c, const yol_font* yf, yol_ctx* ol, uint32_t g, yol_path* out) {
    yol_path tmp;
    int rc;
    if (g < c->n_glyphs || g - c->n_glyphs >= c->n_lg) return YOL_ERR_ARG;
    yol_path_init(&tmp, ol);
    rc = ylay__lglyph_path(&c->lg[g - c->n_glyphs], yf, ol, out, &tmp);
    yol_path_free(&tmp);
    return rc;
}

static const char* const ylay__modes[28] = {
    "CLEAR", "SRC", "DEST", "SRC_OVER", "DEST_OVER", "SRC_IN", "DEST_IN", "SRC_OUT", "DEST_OUT", "SRC_ATOP", "DEST_ATOP",
    "XOR", "PLUS", "SCREEN", "OVERLAY", "DARKEN", "LIGHTEN", "COLOR_DODGE", "COLOR_BURN", "HARD_LIGHT", "SOFT_LIGHT",
    "DIFFERENCE", "EXCLUSION", "MULTIPLY", "HSL_HUE", "HSL_SATURATION", "HSL_COLOR", "HSL_LUMINOSITY",
};

void ylay__colr_message(int ref, int32_t detail, char* buf, size_t cap) {
    static const char* const other = "use a font without it, such as Twemoji (docs/layout.md, Color glyphs)";
    const char* mode = detail >= 0 && detail < 28 ? ylay__modes[detail] : "unknown";
    switch (ref) {
    case YLAY__REF_SWEEP: snprintf(buf, cap, "a sweep gradient (PaintSweepGradient) is not drawn; %s", other); break;
    case YLAY__REF_EXTEND:
        snprintf(buf, cap, "a gradient with extend %s is not drawn (pad only); %s", detail == 1 ? "repeat" : detail == 2 ? "reflect" : "unknown", other);
        break;
    case YLAY__REF_FOREGROUND: snprintf(buf, cap, "the foreground color (palette index 0xFFFF) is not drawn; %s", other); break;
    case YLAY__REF_VARIABLE: snprintf(buf, cap, "variable COLR (a PaintVar format or variation data) is not drawn; %s", other); break;
    case YLAY__REF_COLR_GLYPH: snprintf(buf, cap, "PaintColrGlyph (glyph %d) is not drawn; %s", (int)detail, other); break;
    case YLAY__REF_NESTED_CLIP: snprintf(buf, cap, "a fill under two glyph clips is not drawn; %s", other); break;
    case YLAY__REF_COMPOSITE:
        snprintf(buf, cap, "composite mode %s needs an offscreen group, which is not built; %s", mode,
                 detail == 20 ? "use Noto's build without flags (Noto-COLRv1-noflags.ttf) or Twemoji" : other);
        break;
    case YLAY__REF_MASK: snprintf(buf, cap, "composite mode %s as a mask (not a group opacity) needs an offscreen group, which is not built; %s", mode, other); break;
    case YLAY__REF_OVERLAP:
        snprintf(buf, cap, "group opacity over layers that overlap needs an offscreen group, which is not built; %s", other);
        break;
    case YLAY__REF_LIMIT:
        snprintf(buf, cap, "its paint graph reaches HarfBuzz's limits (64 levels, 16,384 edges), past which HarfBuzz stops without an error; %s", other);
        break;
    case YLAY__REF_IMAGE: snprintf(buf, cap, "an image paint is not drawn; %s", other); break;
    case YLAY__REF_TURNED_CLIP: snprintf(buf, cap, "a clip rectangle under a turn is not drawn; %s", other); break;
    case YLAY__REF_RADIAL_R0: snprintf(buf, cap, "a radial gradient with a start radius above 0 is not drawn; %s", other); break;
    case YLAY__REF_RADIAL_FOCAL:
        snprintf(buf, cap, "a two-point radial gradient whose start circle is not inside its end circle is not drawn; %s", other);
        break;
    case YLAY__REF_STOPS: snprintf(buf, cap, "a color line of %d stops is not drawn (1 to 16); %s", (int)detail, other); break;
    case YLAY__REF_DEGENERATE: snprintf(buf, cap, "a gradient with no direction or under a singular transform is not drawn; %s", other); break;
    case YLAY__REF_UNCLIPPED: snprintf(buf, cap, "a fill with no glyph clip and no clip box is not drawn; %s", other); break;
    case YLAY__REF_GLYPH_ID: snprintf(buf, cap, "a layer's glyph %d is past the font's glyphs (a damaged font); %s", (int)detail, other); break;
    case YLAY__REF_OUTLINE: snprintf(buf, cap, "a clip glyph's outline is refused by ysp/outline.h (code %d); %s", (int)detail, other); break;
    default: snprintf(buf, cap, "refused"); break;
    }
}
