/* colr_font_build.h - small TrueType color fonts written in code, for the
 * tests of color glyphs (pack/layout, the pack tool, ysp/gfx.h and the
 * flattening's fuzz seeds). Not part of any library. The tests need no font
 * file and no download: every font they use is written here.
 *
 * A font is glyf outlines (quadratic, font units, y up), a cmap (format 12),
 * hmtx, and any of: COLR version 0 (base and layer records), COLR version 1
 * (a BaseGlyphList, a LayerList and a ClipList of paint graphs built with
 * cfb_paint_*), CPAL (one palette), a GSUB ligature lookup (feature ccmp,
 * script DFLT: ZWJ sequences), and empty tables by tag (CBDT, sbix, SVG,
 * fvar) for the refusals. Offsets and sizes follow OpenType 1.9.1.
 *
 * Public domain / MIT-0, as the headers.
 */
#ifndef YSP_COLR_FONT_BUILD_H
#define YSP_COLR_FONT_BUILD_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Each test uses some of these: no warning for the rest. */
#if defined(__GNUC__) || defined(__clang__)
#define CFB_FN static __attribute__((unused))
#else
#define CFB_FN static
#pragma warning(disable : 4505)
#endif

typedef struct cfb_buf { uint8_t* d; size_t n, cap; } cfb_buf;

CFB_FN void cfb_need(cfb_buf* b, size_t k) {
    if (b->n + k > b->cap) {
        while (b->n + k > b->cap) b->cap = b->cap ? 2 * b->cap : 256;
        b->d = (uint8_t*)realloc(b->d, b->cap);
        if (!b->d) { printf("colr_font_build: out of memory\n"); exit(1); }
    }
}
static void cfb8(cfb_buf* b, uint32_t v) { cfb_need(b, 1); b->d[b->n++] = (uint8_t)v; }
static void cfb16(cfb_buf* b, uint32_t v) { cfb8(b, v >> 8 & 255); cfb8(b, v & 255); }
static void cfb24(cfb_buf* b, uint32_t v) { cfb8(b, v >> 16 & 255); cfb16(b, v & 0xffff); }
static void cfb32(cfb_buf* b, uint32_t v) { cfb16(b, v >> 16); cfb16(b, v & 0xffff); }
CFB_FN void cfb_pad4(cfb_buf* b) { while (b->n & 3) cfb8(b, 0); }
CFB_FN void cfb_put16(cfb_buf* b, size_t at, uint32_t v) { b->d[at] = (uint8_t)(v >> 8); b->d[at + 1] = (uint8_t)v; }
CFB_FN void cfb_put24(cfb_buf* b, size_t at, uint32_t v) { b->d[at] = (uint8_t)(v >> 16); cfb_put16(b, at + 1, v & 0xffff); }
CFB_FN void cfb_put32(cfb_buf* b, size_t at, uint32_t v) { cfb_put16(b, at, v >> 16); cfb_put16(b, at + 2, v & 0xffff); }
CFB_FN void cfb_bytes(cfb_buf* b, const void* p, size_t n) { cfb_need(b, n); memcpy(b->d + b->n, p, n); b->n += n; }
CFB_FN uint32_t cfb_f2dot14(double v) { return (uint32_t)(int32_t)floor(v * 16384.0 + 0.5) & 0xffff; }
CFB_FN uint32_t cfb_fixed(double v) { return (uint32_t)(int32_t)floor(v * 65536.0 + 0.5); }

/* --- outlines ---------------------------------------------------------------- */

#define CFB_MAXP 40
typedef struct cfb_glyph {
    int     n, nc;                 /* points, contours                          */
    int16_t x[CFB_MAXP], y[CFB_MAXP];
    uint8_t on[CFB_MAXP];
    int     end[6];                /* last point of each contour                */
} cfb_glyph;

CFB_FN void cfb_pt(cfb_glyph* g, int x, int y, int on) {
    if (g->n >= CFB_MAXP) { printf("colr_font_build: too many points\n"); exit(1); }
    g->x[g->n] = (int16_t)x; g->y[g->n] = (int16_t)y; g->on[g->n] = (uint8_t)on; g->n++;
}
CFB_FN void cfb_close(cfb_glyph* g) { g->end[g->nc++] = g->n - 1; }
/* A box, clockwise in font units (y up), as TrueType's outer contours. */
CFB_FN void cfb_box(cfb_glyph* g, int x0, int y0, int x1, int y1) {
    cfb_pt(g, x0, y0, 1); cfb_pt(g, x0, y1, 1); cfb_pt(g, x1, y1, 1); cfb_pt(g, x1, y0, 1); cfb_close(g);
}
/* A disc of 4 quadratics through the axis points, clockwise. */
CFB_FN void cfb_disc(cfb_glyph* g, int cx, int cy, int r) {
    cfb_pt(g, cx - r, cy, 1); cfb_pt(g, cx - r, cy + r, 0); cfb_pt(g, cx, cy + r, 1); cfb_pt(g, cx + r, cy + r, 0);
    cfb_pt(g, cx + r, cy, 1); cfb_pt(g, cx + r, cy - r, 0); cfb_pt(g, cx, cy - r, 1); cfb_pt(g, cx - r, cy - r, 0);
    cfb_close(g);
}
CFB_FN void cfb_tri(cfb_glyph* g, int x0, int y0, int x1, int y1, int x2, int y2) {
    cfb_pt(g, x0, y0, 1); cfb_pt(g, x1, y1, 1); cfb_pt(g, x2, y2, 1); cfb_close(g);
}

/* --- paint graphs (COLR version 1) ------------------------------------------ */

enum { CFB_EXTEND_PAD = 0, CFB_EXTEND_REPEAT = 1, CFB_EXTEND_REFLECT = 2 };
enum { CFB_SRC_OVER = 3, CFB_SRC_IN = 5, CFB_SOFT_LIGHT = 20 };   /* CompositeMode values */

typedef struct cfb_node {
    int    fmt;
    int    a, b;                   /* child nodes, -1 none                      */
    int    i0, i1, mode, extend, n_stops;
    double f[6];
    double so[20], sa[20];
    int    sp[20];
} cfb_node;

#define CFB_MAXN 4096
static cfb_node cfb_nodes[CFB_MAXN];
static int cfb_nn;

CFB_FN int cfb_new(int fmt) {
    cfb_node* n;
    if (cfb_nn >= CFB_MAXN) { printf("colr_font_build: too many nodes\n"); exit(1); }
    n = &cfb_nodes[cfb_nn];
    memset(n, 0, sizeof *n);
    n->fmt = fmt; n->a = n->b = -1;
    return cfb_nn++;
}
CFB_FN int cfb_paint_layers(int first, int n) { int k = cfb_new(1); cfb_nodes[k].i0 = first; cfb_nodes[k].i1 = n; return k; }
CFB_FN int cfb_paint_solid(int pal, double alpha) { int k = cfb_new(2); cfb_nodes[k].i0 = pal; cfb_nodes[k].f[0] = alpha; return k; }
CFB_FN int cfb_paint_var_solid(int pal, double alpha) { int k = cfb_new(3); cfb_nodes[k].i0 = pal; cfb_nodes[k].f[0] = alpha; return k; }
CFB_FN int cfb_paint_glyph(int glyph, int child) { int k = cfb_new(10); cfb_nodes[k].i0 = glyph; cfb_nodes[k].a = child; return k; }
CFB_FN int cfb_paint_colr_glyph(int glyph) { int k = cfb_new(11); cfb_nodes[k].i0 = glyph; return k; }
CFB_FN int cfb_paint_transform(int child, double xx, double yx, double xy, double yy, double dx, double dy) {
    int k = cfb_new(12);
    double* f = cfb_nodes[k].f;
    f[0] = xx; f[1] = yx; f[2] = xy; f[3] = yy; f[4] = dx; f[5] = dy;
    cfb_nodes[k].a = child;
    return k;
}
CFB_FN int cfb_paint_translate(int child, int dx, int dy) { int k = cfb_new(14); cfb_nodes[k].a = child; cfb_nodes[k].i0 = dx; cfb_nodes[k].i1 = dy; return k; }
CFB_FN int cfb_paint_scale(int child, double sx, double sy) { int k = cfb_new(16); cfb_nodes[k].a = child; cfb_nodes[k].f[0] = sx; cfb_nodes[k].f[1] = sy; return k; }
/* angle in degrees, counterclockwise in font units (y up), as COLR's */
CFB_FN int cfb_paint_rotate(int child, double deg) { int k = cfb_new(24); cfb_nodes[k].a = child; cfb_nodes[k].f[0] = deg / 180.0; return k; }
CFB_FN int cfb_paint_composite(int src, int mode, int backdrop) { int k = cfb_new(32); cfb_nodes[k].a = src; cfb_nodes[k].b = backdrop; cfb_nodes[k].mode = mode; return k; }
CFB_FN void cfb_stops(int k, int n, const double* off, const int* pal, const double* alpha, int extend) {
    int i;
    cfb_nodes[k].n_stops = n; cfb_nodes[k].extend = extend;
    for (i = 0; i < n && i < 20; i++) { cfb_nodes[k].so[i] = off[i]; cfb_nodes[k].sp[i] = pal[i]; cfb_nodes[k].sa[i] = alpha ? alpha[i] : 1.0; }
}
CFB_FN int cfb_paint_linear(int x0, int y0, int x1, int y1, int x2, int y2) {
    int k = cfb_new(4);
    double* f = cfb_nodes[k].f;
    f[0] = x0; f[1] = y0; f[2] = x1; f[3] = y1; f[4] = x2; f[5] = y2;
    return k;
}
CFB_FN int cfb_paint_radial(int x0, int y0, int r0, int x1, int y1, int r1) {
    int k = cfb_new(6);
    double* f = cfb_nodes[k].f;
    f[0] = x0; f[1] = y0; f[2] = r0; f[3] = x1; f[4] = y1; f[5] = r1;
    return k;
}
CFB_FN int cfb_paint_sweep(int cx, int cy, double a0, double a1) {
    int k = cfb_new(8);
    double* f = cfb_nodes[k].f;
    f[0] = cx; f[1] = cy; f[2] = a0; f[3] = a1;
    return k;
}

CFB_FN void cfb_colorline(cfb_buf* b, const cfb_node* n) {
    int i;
    cfb8(b, (uint32_t)n->extend); cfb16(b, (uint32_t)n->n_stops);
    for (i = 0; i < n->n_stops; i++) { cfb16(b, cfb_f2dot14(n->so[i])); cfb16(b, (uint32_t)n->sp[i]); cfb16(b, cfb_f2dot14(n->sa[i])); }
}

/* Writes node k at b's end, then its children and tables after it, and
 * returns its offset; Offset24s are from the paint's start. */
CFB_FN size_t cfb_ser(cfb_buf* b, int k) {
    const cfb_node* n = &cfb_nodes[k];
    size_t at = b->n, o;
    cfb8(b, (uint32_t)n->fmt);
    switch (n->fmt) {
    case 1: cfb8(b, (uint32_t)n->i1); cfb32(b, (uint32_t)n->i0); break;
    case 2: cfb16(b, (uint32_t)n->i0); cfb16(b, cfb_f2dot14(n->f[0])); break;
    case 3: cfb16(b, (uint32_t)n->i0); cfb16(b, cfb_f2dot14(n->f[0])); cfb32(b, 0xFFFFFFFFu); break;
    case 4: case 6:
        cfb24(b, 0);
        { int i; for (i = 0; i < 6; i++) cfb16(b, (uint32_t)(int32_t)n->f[i] & 0xffff); }
        cfb_put24(b, at + 1, (uint32_t)(b->n - at));
        cfb_colorline(b, n);
        break;
    case 8:
        cfb24(b, 0);
        cfb16(b, (uint32_t)(int32_t)n->f[0] & 0xffff); cfb16(b, (uint32_t)(int32_t)n->f[1] & 0xffff);
        cfb16(b, cfb_f2dot14(n->f[2])); cfb16(b, cfb_f2dot14(n->f[3]));
        cfb_put24(b, at + 1, (uint32_t)(b->n - at));
        cfb_colorline(b, n);
        break;
    case 10: cfb24(b, 0); cfb16(b, (uint32_t)n->i0); o = cfb_ser(b, n->a); cfb_put24(b, at + 1, (uint32_t)(o - at)); break;
    case 11: cfb16(b, (uint32_t)n->i0); break;
    case 12: {
        int i;
        cfb24(b, 0); cfb24(b, 0);
        cfb_put24(b, at + 4, (uint32_t)(b->n - at));
        for (i = 0; i < 6; i++) cfb32(b, cfb_fixed(n->f[i]));
        o = cfb_ser(b, n->a); cfb_put24(b, at + 1, (uint32_t)(o - at));
        break;
    }
    case 14: cfb24(b, 0); cfb16(b, (uint32_t)n->i0 & 0xffff); cfb16(b, (uint32_t)n->i1 & 0xffff);
             o = cfb_ser(b, n->a); cfb_put24(b, at + 1, (uint32_t)(o - at)); break;
    case 16: cfb24(b, 0); cfb16(b, cfb_f2dot14(n->f[0])); cfb16(b, cfb_f2dot14(n->f[1]));
             o = cfb_ser(b, n->a); cfb_put24(b, at + 1, (uint32_t)(o - at)); break;
    case 24: cfb24(b, 0); cfb16(b, cfb_f2dot14(n->f[0]));
             o = cfb_ser(b, n->a); cfb_put24(b, at + 1, (uint32_t)(o - at)); break;
    case 32:
        cfb24(b, 0); cfb8(b, (uint32_t)n->mode); cfb24(b, 0);
        o = cfb_ser(b, n->a); cfb_put24(b, at + 1, (uint32_t)(o - at));
        o = cfb_ser(b, n->b); cfb_put24(b, at + 5, (uint32_t)(o - at));
        break;
    default: printf("colr_font_build: paint format %d\n", n->fmt); exit(1);
    }
    return at;
}

/* --- the font ---------------------------------------------------------------- */

#define CFB_MAXG 600
typedef struct cfb_font {
    int       upem, asc, desc, adv;
    int       n_glyphs;
    cfb_glyph g[CFB_MAXG];
    int       n_cmap;
    uint32_t  cp[64], cgid[64];             /* sorted by codepoint              */
    int       n_base0, n_layer0;
    uint16_t  base0[64][3];                 /* glyph, first layer, layers       */
    uint16_t  layer0[256][2];               /* glyph, palette index             */
    int       n_base1, n_layer1, n_clip, v1;
    int       base1[64][2];                 /* glyph, node                      */
    int       layer1[600];                  /* nodes; -1: the same as the one before */
    int       clip[16][6];                  /* first glyph, last glyph, box     */
    int       n_colors;
    uint32_t  rgba[64];                     /* 0xRRGGBBAA, sRGB                 */
    int       n_lig;
    uint16_t  lig[16][9];                   /* the ligature glyph, then components, 0-ended */
    int       n_extra;
    char      extra[6][5];
} cfb_font;

CFB_FN void cfb_init(cfb_font* f, int upem) {
    memset(f, 0, sizeof *f);
    f->upem = upem; f->asc = upem * 8 / 10; f->desc = -upem / 5; f->adv = upem;
    f->n_glyphs = 1;                        /* .notdef, empty                   */
}
CFB_FN int cfb_add_glyph(cfb_font* f) {
    if (f->n_glyphs >= CFB_MAXG) { printf("colr_font_build: too many glyphs\n"); exit(1); }
    memset(&f->g[f->n_glyphs], 0, sizeof f->g[0]);
    return f->n_glyphs++;
}
CFB_FN void cfb_map(cfb_font* f, uint32_t cp, int glyph) {
    int i = f->n_cmap++;
    while (i > 0 && f->cp[i - 1] > cp) { f->cp[i] = f->cp[i - 1]; f->cgid[i] = f->cgid[i - 1]; i--; }
    f->cp[i] = cp; f->cgid[i] = (uint32_t)glyph;
}
CFB_FN int cfb_color(cfb_font* f, uint32_t rgba) { f->rgba[f->n_colors] = rgba; return f->n_colors++; }
/* COLR version 0: base glyph g with layers (glyph, palette index) */
CFB_FN void cfb_v0(cfb_font* f, int g, int n, const int* glyphs, const int* pals) {
    int i, k = f->n_base0++;
    while (k > 0 && f->base0[k - 1][0] > g) { memcpy(f->base0[k], f->base0[k - 1], sizeof f->base0[k]); k--; }
    f->base0[k][0] = (uint16_t)g; f->base0[k][1] = (uint16_t)f->n_layer0; f->base0[k][2] = (uint16_t)n;
    for (i = 0; i < n; i++) { f->layer0[f->n_layer0][0] = (uint16_t)glyphs[i]; f->layer0[f->n_layer0][1] = (uint16_t)pals[i]; f->n_layer0++; }
}
CFB_FN void cfb_v1_base(cfb_font* f, int g, int node) {
    int k = f->n_base1++;
    while (k > 0 && f->base1[k - 1][0] > g) { f->base1[k][0] = f->base1[k - 1][0]; f->base1[k][1] = f->base1[k - 1][1]; k--; }
    f->base1[k][0] = g; f->base1[k][1] = node; f->v1 = 1;
}
/* Appends a LayerList entry; returns its index. node -1 repeats the entry
 * before (the same offset), so a large list stays small. */
CFB_FN int cfb_v1_layer(cfb_font* f, int node) { f->layer1[f->n_layer1] = node; f->v1 = 1; return f->n_layer1++; }
CFB_FN void cfb_v1_clip(cfb_font* f, int g0, int g1, int x0, int y0, int x1, int y1) {
    int* c = f->clip[f->n_clip++];
    c[0] = g0; c[1] = g1; c[2] = x0; c[3] = y0; c[4] = x1; c[5] = y1; f->v1 = 1;
}
/* A GSUB ligature: components (glyphs) n of them become lig. */
CFB_FN void cfb_lig(cfb_font* f, int lig, int n, const int* comp) {
    int i;
    memset(f->lig[f->n_lig], 0, sizeof f->lig[0]);
    f->lig[f->n_lig][0] = (uint16_t)lig;
    for (i = 0; i < n && i < 8; i++) f->lig[f->n_lig][1 + i] = (uint16_t)comp[i];
    f->n_lig++;
}
CFB_FN void cfb_extra(cfb_font* f, const char* tag) { memcpy(f->extra[f->n_extra++], tag, 4); }

CFB_FN void cfb_t_glyf(const cfb_font* f, cfb_buf* glyf, cfb_buf* loca) {
    int gi, i;
    for (gi = 0; gi < f->n_glyphs; gi++) {
        const cfb_glyph* g = &f->g[gi];
        int x0 = 32767, y0 = 32767, x1 = -32768, y1 = -32768, px = 0, py = 0;
        cfb32(loca, (uint32_t)glyf->n);
        if (!g->n) continue;
        for (i = 0; i < g->n; i++) {
            if (g->x[i] < x0) x0 = g->x[i];
            if (g->x[i] > x1) x1 = g->x[i];
            if (g->y[i] < y0) y0 = g->y[i];
            if (g->y[i] > y1) y1 = g->y[i];
        }
        cfb16(glyf, (uint32_t)g->nc);
        cfb16(glyf, (uint32_t)x0 & 0xffff); cfb16(glyf, (uint32_t)y0 & 0xffff);
        cfb16(glyf, (uint32_t)x1 & 0xffff); cfb16(glyf, (uint32_t)y1 & 0xffff);
        for (i = 0; i < g->nc; i++) cfb16(glyf, (uint32_t)g->end[i]);
        cfb16(glyf, 0);                                      /* no instructions */
        for (i = 0; i < g->n; i++) cfb8(glyf, g->on[i] ? 1u : 0u);   /* word deltas */
        for (i = 0; i < g->n; i++) { cfb16(glyf, (uint32_t)(g->x[i] - px) & 0xffff); px = g->x[i]; }
        for (i = 0; i < g->n; i++) { cfb16(glyf, (uint32_t)(g->y[i] - py) & 0xffff); py = g->y[i]; }
        cfb_pad4(glyf);
    }
    cfb32(loca, (uint32_t)glyf->n);
}

CFB_FN void cfb_t_cmap(const cfb_font* f, cfb_buf* c) {
    int i;
    cfb16(c, 0); cfb16(c, 1); cfb16(c, 3); cfb16(c, 10); cfb32(c, 12);
    cfb16(c, 12); cfb16(c, 0); cfb32(c, 16 + 12 * (uint32_t)f->n_cmap); cfb32(c, 0); cfb32(c, (uint32_t)f->n_cmap);
    for (i = 0; i < f->n_cmap; i++) { cfb32(c, f->cp[i]); cfb32(c, f->cp[i]); cfb32(c, f->cgid[i]); }
}

CFB_FN void cfb_t_gsub(const cfb_font* f, cfb_buf* t) {
    /* the first glyphs of the ligatures, sorted, for the coverage */
    uint16_t first[16];
    int nf = 0, i, j;
    size_t lookup, sub, cov, sets;
    for (i = 0; i < f->n_lig; i++) {
        uint16_t g = f->lig[i][1];
        for (j = 0; j < nf && first[j] != g; j++) {}
        if (j == nf) { first[nf++] = g; for (j = nf - 1; j > 0 && first[j - 1] > first[j]; j--) { uint16_t s = first[j]; first[j] = first[j - 1]; first[j - 1] = s; } }
    }
    cfb16(t, 1); cfb16(t, 0); cfb16(t, 10); cfb16(t, 30); cfb16(t, 44);
    /* ScriptList at 10: DFLT, its Script at 8 from the list, LangSys at 4 from the Script */
    cfb16(t, 1); cfb32(t, 0x44464C54u); cfb16(t, 8);
    cfb16(t, 4); cfb16(t, 0);
    cfb16(t, 0); cfb16(t, 0xFFFF); cfb16(t, 1); cfb16(t, 0);
    /* FeatureList at 30: ccmp, its Feature at 8 */
    cfb16(t, 1); cfb32(t, 0x63636D70u); cfb16(t, 8);
    cfb16(t, 0); cfb16(t, 1); cfb16(t, 0);
    /* LookupList at 44: one lookup at 4 from it */
    cfb16(t, 1); cfb16(t, 4);
    lookup = t->n;
    cfb16(t, 4); cfb16(t, 0); cfb16(t, 1); cfb16(t, 8);
    sub = t->n;
    cfb16(t, 1); cfb16(t, 0); cfb16(t, (uint32_t)nf);
    for (i = 0; i < nf; i++) cfb16(t, 0);
    cov = t->n;
    cfb_put16(t, sub + 2, (uint32_t)(cov - sub));
    cfb16(t, 1); cfb16(t, (uint32_t)nf);
    for (i = 0; i < nf; i++) cfb16(t, first[i]);
    for (i = 0; i < nf; i++) {
        size_t set = t->n;
        int nl = 0, k;
        cfb_put16(t, sub + 6 + 2 * (size_t)i, (uint32_t)(set - sub));
        for (k = 0; k < f->n_lig; k++) if (f->lig[k][1] == first[i]) nl++;
        cfb16(t, (uint32_t)nl);
        sets = t->n;
        for (k = 0; k < nl; k++) cfb16(t, 0);
        nl = 0;
        for (k = 0; k < f->n_lig; k++) {
            int nc = 0;
            if (f->lig[k][1] != first[i]) continue;
            while (nc < 8 && f->lig[k][1 + nc]) nc++;
            cfb_put16(t, sets + 2 * (size_t)nl++, (uint32_t)(t->n - set));
            cfb16(t, f->lig[k][0]); cfb16(t, (uint32_t)nc);
            for (j = 1; j < nc; j++) cfb16(t, f->lig[k][1 + j]);
        }
    }
    (void)lookup;
}

CFB_FN void cfb_t_colr(const cfb_font* f, cfb_buf* t) {
    int i;
    size_t base_off = 0, layer_off = 0, bgl = 0, ll = 0, cl = 0;
    cfb16(t, f->v1 ? 1u : 0u); cfb16(t, (uint32_t)f->n_base0); cfb32(t, 0); cfb32(t, 0); cfb16(t, (uint32_t)f->n_layer0);
    if (f->v1) { cfb32(t, 0); cfb32(t, 0); cfb32(t, 0); cfb32(t, 0); cfb32(t, 0); }
    if (f->n_base0) {
        base_off = t->n;
        for (i = 0; i < f->n_base0; i++) { cfb16(t, f->base0[i][0]); cfb16(t, f->base0[i][1]); cfb16(t, f->base0[i][2]); }
        layer_off = t->n;
        for (i = 0; i < f->n_layer0; i++) { cfb16(t, f->layer0[i][0]); cfb16(t, f->layer0[i][1]); }
        cfb_put32(t, 4, (uint32_t)base_off); cfb_put32(t, 8, (uint32_t)layer_off);
    }
    if (!f->v1) return;
    {   /* BaseGlyphList: the records, then each paint graph */
        cfb_buf b = { 0 };
        cfb32(&b, (uint32_t)f->n_base1);
        for (i = 0; i < f->n_base1; i++) { cfb16(&b, (uint32_t)f->base1[i][0]); cfb32(&b, 0); }
        for (i = 0; i < f->n_base1; i++) cfb_put32(&b, 4 + 6 * (size_t)i + 2, (uint32_t)cfb_ser(&b, f->base1[i][1]));
        cfb_pad4(t);
        bgl = t->n; cfb_bytes(t, b.d, b.n); free(b.d);
    }
    if (f->n_layer1) {
        cfb_buf b = { 0 };
        size_t last = 0;
        cfb32(&b, (uint32_t)f->n_layer1);
        for (i = 0; i < f->n_layer1; i++) cfb32(&b, 0);
        for (i = 0; i < f->n_layer1; i++) {
            if (f->layer1[i] >= 0) last = cfb_ser(&b, f->layer1[i]);
            cfb_put32(&b, 4 + 4 * (size_t)i, (uint32_t)last);
        }
        cfb_pad4(t);
        ll = t->n; cfb_bytes(t, b.d, b.n); free(b.d);
    }
    if (f->n_clip) {
        cfb_buf b = { 0 };
        cfb8(&b, 1); cfb32(&b, (uint32_t)f->n_clip);
        for (i = 0; i < f->n_clip; i++) { cfb16(&b, (uint32_t)f->clip[i][0]); cfb16(&b, (uint32_t)f->clip[i][1]); cfb24(&b, 0); }
        for (i = 0; i < f->n_clip; i++) {
            int k;
            cfb_put24(&b, 5 + 7 * (size_t)i + 4, (uint32_t)b.n);
            cfb8(&b, 1);
            for (k = 2; k < 6; k++) cfb16(&b, (uint32_t)f->clip[i][k] & 0xffff);
        }
        cfb_pad4(t);
        cl = t->n; cfb_bytes(t, b.d, b.n); free(b.d);
    }
    cfb_put32(t, 14, (uint32_t)bgl); cfb_put32(t, 18, (uint32_t)ll); cfb_put32(t, 22, (uint32_t)cl);
}

CFB_FN void cfb_t_cpal(const cfb_font* f, cfb_buf* t) {
    int i;
    cfb16(t, 0); cfb16(t, (uint32_t)f->n_colors); cfb16(t, 1); cfb16(t, (uint32_t)f->n_colors); cfb32(t, 14); cfb16(t, 0);
    for (i = 0; i < f->n_colors; i++) {   /* BGRA */
        uint32_t c = f->rgba[i];
        cfb8(t, c >> 8 & 255); cfb8(t, c >> 16 & 255); cfb8(t, c >> 24 & 255); cfb8(t, c & 255);
    }
}

CFB_FN uint32_t cfb_sum(const uint8_t* d, size_t n) {
    uint32_t s = 0;
    size_t i;
    for (i = 0; i < n; i += 4) {
        uint32_t w = 0;
        int k;
        for (k = 0; k < 4; k++) w = w << 8 | (i + (size_t)k < n ? d[i + (size_t)k] : 0u);
        s += w;
    }
    return s;
}

/* The font file into out (out's bytes replaced). */
CFB_FN void cfb_write(const cfb_font* f, cfb_buf* out) {
    cfb_buf t[16];
    char tags[16][5];
    int nt = 0, i, j;
    size_t dir;
    uint32_t es = 0, sr = 1;
    memset(t, 0, sizeof t);
#define CFB_T(tag) (memcpy(tags[nt], tag, 5), &t[nt++])
    cfb_t_cmap(f, CFB_T("cmap"));
    {
        cfb_buf* g = CFB_T("glyf");
        cfb_buf* l = CFB_T("loca");
        cfb_t_glyf(f, g, l);
    }
    {
        cfb_buf* h = CFB_T("head");
        for (i = 0; i < 54; i++) cfb8(h, 0);
        cfb_put32(h, 0, 0x00010000u); cfb_put32(h, 12, 0x5F0F3CF5u);
        cfb_put16(h, 18, (uint32_t)f->upem); cfb_put16(h, 50, 1);
    }
    {
        cfb_buf* h = CFB_T("hhea");
        for (i = 0; i < 36; i++) cfb8(h, 0);
        cfb_put32(h, 0, 0x00010000u);
        cfb_put16(h, 4, (uint32_t)f->asc & 0xffff); cfb_put16(h, 6, (uint32_t)f->desc & 0xffff);
        cfb_put16(h, 10, (uint32_t)f->adv);
        cfb_put16(h, 34, (uint32_t)f->n_glyphs);
    }
    {
        cfb_buf* h = CFB_T("hmtx");
        for (i = 0; i < f->n_glyphs; i++) {
            int lsb = 0, k;
            if (f->g[i].n) { lsb = 32767; for (k = 0; k < f->g[i].n; k++) if (f->g[i].x[k] < lsb) lsb = f->g[i].x[k]; }
            cfb16(h, (uint32_t)f->adv); cfb16(h, (uint32_t)lsb & 0xffff);
        }
    }
    { cfb_buf* m = CFB_T("maxp"); cfb32(m, 0x00005000u); cfb16(m, (uint32_t)f->n_glyphs); }
    if (f->n_base0 || f->v1) cfb_t_colr(f, CFB_T("COLR"));
    if (f->n_colors) cfb_t_cpal(f, CFB_T("CPAL"));
    if (f->n_lig) cfb_t_gsub(f, CFB_T("GSUB"));
    for (i = 0; i < f->n_extra; i++) { cfb_buf* e = CFB_T(f->extra[i]); cfb32(e, 0); }
#undef CFB_T
    /* sorted by tag */
    for (i = 1; i < nt; i++)
        for (j = i; j > 0 && memcmp(tags[j - 1], tags[j], 4) > 0; j--) {
            cfb_buf tb = t[j]; char tg[5];
            t[j] = t[j - 1]; t[j - 1] = tb;
            memcpy(tg, tags[j], 5); memcpy(tags[j], tags[j - 1], 5); memcpy(tags[j - 1], tg, 5);
        }
    while (sr * 2 <= (uint32_t)nt) { sr *= 2; es++; }
    out->n = 0;
    cfb32(out, 0x00010000u); cfb16(out, (uint32_t)nt); cfb16(out, 16 * sr); cfb16(out, es); cfb16(out, 16 * (uint32_t)nt - 16 * sr);
    dir = out->n;
    for (i = 0; i < nt; i++) { cfb32(out, 0); cfb32(out, 0); cfb32(out, 0); cfb32(out, 0); }
    for (i = 0; i < nt; i++) {
        size_t off;
        cfb_pad4(out);
        off = out->n;
        cfb_bytes(out, t[i].d, t[i].n);
        memcpy(out->d + dir + 16 * (size_t)i, tags[i], 4);
        cfb_put32(out, dir + 16 * (size_t)i + 4, cfb_sum(t[i].d, t[i].n));
        cfb_put32(out, dir + 16 * (size_t)i + 8, (uint32_t)off);
        cfb_put32(out, dir + 16 * (size_t)i + 12, (uint32_t)t[i].n);
        free(t[i].d);
    }
    cfb_pad4(out);
}

/* --- the fonts the tests share ---------------------------------------------- */

/* Codepoints of the emoji fonts below. */
#define CFB_GRIN    0x1F600u   /* grinning face                        */
#define CFB_SNOW    0x2603u    /* snowman: text presentation by default */
#define CFB_MAN     0x1F468u
#define CFB_WOMAN   0x1F469u
#define CFB_ZWJ     0x200Du
#define CFB_HEART   0x2764u    /* heavy black heart: text by default    */

/* Glyph ids of cfb_emoji_v0(). */
enum { CFB0_GRIN = 2, CFB0_SNOW, CFB0_ZWJ, CFB0_MAN, CFB0_WOMAN, CFB0_FAMILY, CFB0_L_BOX, CFB0_L_TRI, CFB0_L_DISC, CFB0_L_EYE, CFB0_N };

/* A COLR version 0 emoji font (1000 units per em): the grinning face (two
 * layers), the snowman (one), man and woman, and their ZWJ sequence as a
 * GSUB ligature of three layers. Every base glyph has a monochrome outline
 * too, as COLRv0 fonts have. */
CFB_FN void cfb_emoji_v0(cfb_buf* out) {
    static cfb_font f;
    int g;
    cfb_init(&f, 1000);
    for (g = 1; g < CFB0_N; g++) cfb_add_glyph(&f);
    cfb_box(&f.g[CFB0_GRIN], 100, 0, 900, 800);
    cfb_box(&f.g[CFB0_SNOW], 200, 0, 800, 800);
    cfb_box(&f.g[CFB0_MAN], 100, 0, 500, 800);
    cfb_box(&f.g[CFB0_WOMAN], 500, 0, 900, 800);
    cfb_box(&f.g[CFB0_FAMILY], 100, 0, 900, 800);
    cfb_box(&f.g[CFB0_L_BOX], 100, 0, 900, 800);
    cfb_tri(&f.g[CFB0_L_TRI], 200, 100, 500, 700, 800, 100);
    cfb_disc(&f.g[CFB0_L_DISC], 500, 400, 300);
    cfb_disc(&f.g[CFB0_L_EYE], 350, 550, 60);
    cfb_disc(&f.g[CFB0_L_EYE], 650, 550, 60);
    cfb_map(&f, CFB_GRIN, CFB0_GRIN);
    cfb_map(&f, CFB_SNOW, CFB0_SNOW);
    cfb_map(&f, CFB_ZWJ, CFB0_ZWJ);
    cfb_map(&f, CFB_MAN, CFB0_MAN);
    cfb_map(&f, CFB_WOMAN, CFB0_WOMAN);
    cfb_color(&f, 0xFFCC22FFu);     /* 0 yellow  */
    cfb_color(&f, 0x202020FFu);     /* 1 near black */
    cfb_color(&f, 0x2266DDFFu);     /* 2 blue */
    cfb_color(&f, 0xDD4422FFu);     /* 3 red */
    cfb_color(&f, 0x22AA44FFu);     /* 4 green */
    cfb_color(&f, 0xFFFFFF80u);     /* 5 white, half alpha */
    {
        int gl[3], pl[3];
        gl[0] = CFB0_L_DISC; pl[0] = 0; gl[1] = CFB0_L_EYE; pl[1] = 1; cfb_v0(&f, CFB0_GRIN, 2, gl, pl);
        gl[0] = CFB0_L_DISC; pl[0] = 5; cfb_v0(&f, CFB0_SNOW, 1, gl, pl);
        gl[0] = CFB0_L_TRI; pl[0] = 2; cfb_v0(&f, CFB0_MAN, 1, gl, pl);
        gl[0] = CFB0_L_TRI; pl[0] = 3; cfb_v0(&f, CFB0_WOMAN, 1, gl, pl);
        gl[0] = CFB0_L_BOX; pl[0] = 4; gl[1] = CFB0_L_TRI; pl[1] = 2; gl[2] = CFB0_L_EYE; pl[2] = 3;
        cfb_v0(&f, CFB0_FAMILY, 3, gl, pl);
    }
    {
        int comp[3] = { CFB0_MAN, CFB0_ZWJ, CFB0_WOMAN };
        cfb_lig(&f, CFB0_FAMILY, 3, comp);
    }
    cfb_write(&f, out);
}


/* Glyph ids of cfb_emoji_v1(): the outlines, then one base glyph per case.
 * CFB1_B_SOLID .. CFB1_B_CLIP draw; CFB1_B_OVERLAP on are refused. */
enum {
    CFB1_L_BOX = 1, CFB1_L_DISC, CFB1_L_TRI, CFB1_L_SMALL, CFB1_L_RIGHT,
    CFB1_B_SOLID,      /* two layers: a box, a disc at alpha 0.5           */
    CFB1_B_LINEAR,     /* a box, a linear gradient from x 100 to 900        */
    CFB1_B_RADIAL,     /* a disc, a radial gradient: center (500, 400), r 300, 3 stops */
    CFB1_B_TWOPT,      /* a box, a two-point radial: focal (400, 400), end circle (500, 400) r 300 */
    CFB1_B_XFORM,      /* the small box translated by (300, 100): a layer glyph */
    CFB1_B_ROTATE,     /* the small box and its linear gradient turned 30 degrees (F2DOT14: 2731 / 16384 half turns) */
    CFB1_B_ELLIPSE,    /* a box; the radial gradient scaled 2 x 1 inside it  */
    CFB1_B_OPACITY,    /* group opacity 0.5 over two boxes that do not overlap */
    CFB1_B_CLIP,       /* a box cut by its clip box (200, 100) - (700, 600)  */
    CFB1_B_OVERLAP,    /* group opacity over a box and a disc that overlap   */
    CFB1_B_SOFT,       /* SOFT_LIGHT                                         */
    CFB1_B_SWEEP,      /* a sweep gradient                                   */
    CFB1_B_COLRGLYPH,  /* PaintColrGlyph                                     */
    CFB1_B_FG,         /* the foreground color                               */
    CFB1_B_REPEAT,     /* a linear gradient with extend repeat               */
    CFB1_B_VAR,        /* PaintVarSolid                                      */
    CFB1_B_DEEP,       /* 70 PaintColrLayers nested through the LayerList     */
    CFB1_B_WIDE,       /* 255 x 64 layers: past 16,384 edges                  */
    CFB1_B_STOPS17,    /* a color line of 17 stops                           */
    CFB1_B_R0,         /* a radial with a start radius of 50                 */
    CFB1_B_FOCAL,      /* a two-point radial whose focal point is outside     */
    CFB1_N
};
#define CFB1_CP(g) (0xE000u + (uint32_t)(g))   /* each base glyph's codepoint */

/* A COLR version 1 font (1000 units per em, palette red, green, blue,
 * white) of the cases above, each base glyph mapped to CFB1_CP(id). */
CFB_FN void cfb_emoji_v1_skip(cfb_buf* out, uint64_t skip) {
    static cfb_font f;
    int g, k, i;
    double off2[2] = { 0, 1 }, off3[3] = { 0, 0.5, 1 }, off17[17];
    int pal2[2] = { 0, 2 }, pal3[3] = { 0, 1, 2 }, pal17[17];
    double a3[3] = { 1, 0.5, 1 };
    cfb_nn = 0;
    cfb_init(&f, 1000);
    for (g = 1; g < CFB1_N; g++) cfb_add_glyph(&f);
    cfb_box(&f.g[CFB1_L_BOX], 100, 0, 900, 800);
    cfb_disc(&f.g[CFB1_L_DISC], 500, 400, 300);
    cfb_tri(&f.g[CFB1_L_TRI], 200, 100, 500, 700, 800, 100);
    cfb_box(&f.g[CFB1_L_SMALL], 0, 100, 200, 300);
    cfb_box(&f.g[CFB1_L_RIGHT], 600, 0, 900, 300);
    for (g = CFB1_B_SOLID; g < CFB1_N; g++) { cfb_box(&f.g[g], 100, 0, 900, 800); cfb_map(&f, CFB1_CP(g), g); }
    cfb_color(&f, 0xFF0000FFu); cfb_color(&f, 0x00FF00FFu); cfb_color(&f, 0x0000FFFFu); cfb_color(&f, 0xFFFFFFFFu);
    {
        int l0 = cfb_v1_layer(&f, cfb_paint_glyph(CFB1_L_BOX, cfb_paint_solid(0, 1.0)));
        cfb_v1_layer(&f, cfb_paint_glyph(CFB1_L_DISC, cfb_paint_solid(1, 0.5)));
        cfb_v1_base(&f, CFB1_B_SOLID, cfb_paint_layers(l0, 2));
    }
    k = cfb_paint_linear(100, 0, 900, 0, 100, 100); cfb_stops(k, 2, off2, pal2, NULL, CFB_EXTEND_PAD);
    cfb_v1_base(&f, CFB1_B_LINEAR, cfb_paint_glyph(CFB1_L_BOX, k));
    k = cfb_paint_radial(500, 400, 0, 500, 400, 300); cfb_stops(k, 3, off3, pal3, a3, CFB_EXTEND_PAD);
    cfb_v1_base(&f, CFB1_B_RADIAL, cfb_paint_glyph(CFB1_L_DISC, k));
    k = cfb_paint_radial(400, 400, 0, 500, 400, 300); cfb_stops(k, 2, off2, pal2, NULL, CFB_EXTEND_PAD);
    cfb_v1_base(&f, CFB1_B_TWOPT, cfb_paint_glyph(CFB1_L_BOX, k));
    cfb_v1_base(&f, CFB1_B_XFORM, cfb_paint_translate(cfb_paint_glyph(CFB1_L_SMALL, cfb_paint_solid(2, 1.0)), 300, 100));
    k = cfb_paint_linear(0, 0, 200, 0, 0, 100); cfb_stops(k, 2, off2, pal2, NULL, CFB_EXTEND_PAD);
    cfb_v1_base(&f, CFB1_B_ROTATE, cfb_paint_rotate(cfb_paint_glyph(CFB1_L_SMALL, k), 30));
    k = cfb_paint_radial(250, 400, 0, 250, 400, 150); cfb_stops(k, 2, off2, pal2, NULL, CFB_EXTEND_PAD);
    cfb_v1_base(&f, CFB1_B_ELLIPSE, cfb_paint_glyph(CFB1_L_BOX, cfb_paint_transform(k, 2.0, 0, 0, 1.0, 0, 0)));   /* F2DOT14 stops below 2 */
    {
        int l0 = cfb_v1_layer(&f, cfb_paint_glyph(CFB1_L_SMALL, cfb_paint_solid(2, 1.0)));
        cfb_v1_layer(&f, cfb_paint_glyph(CFB1_L_RIGHT, cfb_paint_solid(3, 0.8)));
        cfb_v1_base(&f, CFB1_B_OPACITY, cfb_paint_composite(cfb_paint_layers(l0, 2), CFB_SRC_IN, cfb_paint_solid(1, 0.5)));
    }
    cfb_v1_base(&f, CFB1_B_CLIP, cfb_paint_glyph(CFB1_L_BOX, cfb_paint_solid(1, 1.0)));
    cfb_v1_clip(&f, CFB1_B_CLIP, CFB1_B_CLIP, 200, 100, 700, 600);
    {
        int l0 = cfb_v1_layer(&f, cfb_paint_glyph(CFB1_L_BOX, cfb_paint_solid(0, 1.0)));
        cfb_v1_layer(&f, cfb_paint_glyph(CFB1_L_DISC, cfb_paint_solid(1, 1.0)));
        cfb_v1_base(&f, CFB1_B_OVERLAP, cfb_paint_composite(cfb_paint_layers(l0, 2), CFB_SRC_IN, cfb_paint_solid(1, 0.5)));
    }
    cfb_v1_base(&f, CFB1_B_SOFT, cfb_paint_composite(cfb_paint_glyph(CFB1_L_BOX, cfb_paint_solid(0, 1.0)), CFB_SOFT_LIGHT,
                                                     cfb_paint_glyph(CFB1_L_DISC, cfb_paint_solid(1, 1.0))));
    k = cfb_paint_sweep(500, 400, 0, 1.0); cfb_stops(k, 2, off2, pal2, NULL, CFB_EXTEND_PAD);
    cfb_v1_base(&f, CFB1_B_SWEEP, cfb_paint_glyph(CFB1_L_BOX, k));
    cfb_v1_base(&f, CFB1_B_COLRGLYPH, cfb_paint_colr_glyph(CFB1_B_SOLID));
    cfb_v1_base(&f, CFB1_B_FG, cfb_paint_glyph(CFB1_L_BOX, cfb_paint_solid(0xFFFF, 1.0)));
    k = cfb_paint_linear(100, 0, 900, 0, 100, 100); cfb_stops(k, 2, off2, pal2, NULL, CFB_EXTEND_REPEAT);
    cfb_v1_base(&f, CFB1_B_REPEAT, cfb_paint_glyph(CFB1_L_BOX, k));
    cfb_v1_base(&f, CFB1_B_VAR, cfb_paint_glyph(CFB1_L_BOX, cfb_paint_var_solid(0, 1.0)));
    {   /* nested through the LayerList: HarfBuzz's sanitizer does not follow
         * it (a chain of 70 transforms fails sanitizing, and the whole COLR
         * with it), its paint walk stops at 64 levels without an error */
        int d0 = f.n_layer1;
        for (i = 0; i < 70; i++) cfb_v1_layer(&f, cfb_paint_layers(d0 + i + 1, 1));
        cfb_v1_layer(&f, cfb_paint_glyph(CFB1_L_SMALL, cfb_paint_solid(0, 1.0)));
        cfb_v1_base(&f, CFB1_B_DEEP, cfb_paint_layers(d0, 1));
    }
    {
        int leaf0 = f.n_layer1, mid0;
        cfb_v1_layer(&f, cfb_paint_glyph(CFB1_L_SMALL, cfb_paint_solid(0, 1.0)));
        for (i = 1; i < 64; i++) cfb_v1_layer(&f, -1);
        mid0 = f.n_layer1;
        cfb_v1_layer(&f, cfb_paint_layers(leaf0, 64));
        for (i = 1; i < 255; i++) cfb_v1_layer(&f, -1);
        cfb_v1_base(&f, CFB1_B_WIDE, cfb_paint_layers(mid0, 255));
    }
    for (i = 0; i < 17; i++) { off17[i] = i / 16.0; pal17[i] = i & 3; }
    k = cfb_paint_linear(100, 0, 900, 0, 100, 100); cfb_stops(k, 17, off17, pal17, NULL, CFB_EXTEND_PAD);
    cfb_v1_base(&f, CFB1_B_STOPS17, cfb_paint_glyph(CFB1_L_BOX, k));
    k = cfb_paint_radial(500, 400, 50, 500, 400, 300); cfb_stops(k, 2, off2, pal2, NULL, CFB_EXTEND_PAD);
    cfb_v1_base(&f, CFB1_B_R0, cfb_paint_glyph(CFB1_L_BOX, k));
    k = cfb_paint_radial(100, 400, 0, 500, 400, 300); cfb_stops(k, 2, off2, pal2, NULL, CFB_EXTEND_PAD);
    cfb_v1_base(&f, CFB1_B_FOCAL, cfb_paint_glyph(CFB1_L_BOX, k));
    for (k = 0; k < f.n_base1; k++)   /* the cases skip leaves out: for a probe */
        if (skip >> (f.base1[k][0] - CFB1_B_SOLID) & 1u) { memmove(f.base1[k], f.base1[k + 1], sizeof f.base1[0] * (size_t)(f.n_base1 - k - 1)); f.n_base1--; k--; }
    cfb_write(&f, out);
}
CFB_FN void cfb_emoji_v1(cfb_buf* out) { cfb_emoji_v1_skip(out, 0); }

#endif
