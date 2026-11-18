/* colr_survey.c - what color emoji fonts use from COLRv0 and COLRv1.
 *
 * Walks every color glyph of each font given with HarfBuzz's paint API
 * (hb_font_paint_glyph) and counts the operations, composite modes,
 * gradients, layers, nesting, foreground use and outline cost a flattened
 * "clip outline, transform, fill" layer model would have to carry.
 * docs/emoji_colr_survey.md is the record; sh tests/probe/colr/run.sh
 * builds and runs it. Single-threaded, one font at a time.
 *
 *   colr_survey [--list out.tsv] [--dump GID] font.ttf [font.ttf ...]
 *
 * --list writes one line per color glyph: layers, nesting depth, outline
 * segments (all clip glyphs, distinct ones), callbacks and flags (L linear,
 * R radial, S sweep, C a composite mode other than SRC_OVER, T a transform,
 * U a fill with no glyph clip, X a clip rectangle that cuts, F foreground).
 * --dump prints one glyph's paint calls, nested by the stack.
 *
 * The font scale is set to units per em, so HarfBuzz's font transform is
 * the identity and every coordinate below is in font units. */
#include <hb.h>
#include <hb-ot.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAXSTACK 256
#define NMODES 28
#define EPS 1e-4f

static const char* mode_name[NMODES] = {
    "CLEAR", "SRC", "DEST", "SRC_OVER", "DEST_OVER", "SRC_IN", "DEST_IN",
    "SRC_OUT", "DEST_OUT", "SRC_ATOP", "DEST_ATOP", "XOR", "PLUS", "SCREEN",
    "OVERLAY", "DARKEN", "LIGHTEN", "COLOR_DODGE", "COLOR_BURN",
    "HARD_LIGHT", "SOFT_LIGHT", "DIFFERENCE", "EXCLUSION", "MULTIPLY",
    "HSL_HUE", "HSL_SATURATION", "HSL_COLOR", "HSL_LUMINOSITY"};

typedef struct { float a, b, c, d, e, f; } mat; /* x' = a x + c y + e */

static mat mul(mat p, mat q) /* p then q applied inside: p * q */
{
    mat r;
    r.a = p.a * q.a + p.c * q.b;  r.b = p.b * q.a + p.d * q.b;
    r.c = p.a * q.c + p.c * q.d;  r.d = p.b * q.c + p.d * q.d;
    r.e = p.a * q.e + p.c * q.f + p.e;
    r.f = p.b * q.e + p.d * q.f + p.f;
    return r;
}

/* Transform kinds, from the most to the least special */
enum { T_IDENT, T_TRANSLATE, T_SCALE_U, T_SCALE_N, T_ROTATE, T_SIMILAR,
       T_SKEW, T_GENERAL, T_N };
static const char* tkind_name[T_N] = {"identity", "translate",
    "scale (uniform)", "scale (non-uniform)", "rotate", "rotate+scale",
    "skew", "general"};

static int near(float x, float y) { return fabsf(x - y) <= EPS * (1 + fabsf(y)); }

static int tkind(mat m)
{
    int lin_id = near(m.a, 1) && near(m.d, 1) && near(m.b, 0) && near(m.c, 0);
    if (lin_id) return (near(m.e, 0) && near(m.f, 0)) ? T_IDENT : T_TRANSLATE;
    if (near(m.b, 0) && near(m.c, 0))
        return near(fabsf(m.a), fabsf(m.d)) ? T_SCALE_U : T_SCALE_N;
    if (near(m.a, m.d) && near(m.b, -m.c)) {
        float s = m.a * m.a + m.b * m.b;
        return near(s, 1) ? T_ROTATE : T_SIMILAR;
    }
    /* a mirrored similarity: a = -d, b = c */
    if (near(m.a, -m.d) && near(m.b, m.c)) return T_SIMILAR;
    if (near(m.a, 1) && near(m.d, 1) && (near(m.b, 0) || near(m.c, 0))) return T_SKEW;
    return T_GENERAL;
}
/* similarity of the linear part (radial circles stay circles) */
static int similar(mat m)
{
    return (near(m.a, m.d) && near(m.b, -m.c)) || (near(m.a, -m.d) && near(m.b, m.c));
}

/* ---- outline cost, cached per glyph ---- */
typedef struct { int lines, quads, cubics, contours; int done; } gcost;
typedef struct { mat m; float x0, y0, x1, y1; int any; int lines, quads, cubics, contours; } dstate;

static void bb(dstate* s, float x, float y)
{
    float X = s->m.a * x + s->m.c * y + s->m.e, Y = s->m.b * x + s->m.d * y + s->m.f;
    if (!s->any) { s->x0 = s->x1 = X; s->y0 = s->y1 = Y; s->any = 1; }
    s->x0 = fminf(s->x0, X); s->x1 = fmaxf(s->x1, X);
    s->y0 = fminf(s->y0, Y); s->y1 = fmaxf(s->y1, Y);
}
static void d_move(hb_draw_funcs_t* f, void* d, hb_draw_state_t* st, float x, float y, void* u)
{ (void)f; (void)st; (void)u; ((dstate*)d)->contours++; bb(d, x, y); }
static void d_line(hb_draw_funcs_t* f, void* d, hb_draw_state_t* st, float x, float y, void* u)
{ (void)f; (void)st; (void)u; ((dstate*)d)->lines++; bb(d, x, y); }
static void d_quad(hb_draw_funcs_t* f, void* d, hb_draw_state_t* st, float cx, float cy, float x, float y, void* u)
{
    /* the curve's own extremes, not its control point: a clip box may cut
     * the control polygon and still hold the outline */
    (void)f; (void)u; dstate* s = d; s->quads++;
    float x0 = st->current_x, y0 = st->current_y;
    mat m = s->m;
    for (int ax = 0; ax < 2; ax++) {
        float a = ax ? m.b : m.a, b = ax ? m.d : m.c;
        float p0 = a * x0 + b * y0, p1 = a * cx + b * cy, p2 = a * x + b * y;
        float den = p0 - 2 * p1 + p2;
        if (fabsf(den) < 1e-12f) continue;
        float t = (p0 - p1) / den;
        if (t <= 0 || t >= 1) continue;
        float u0 = (1 - t) * (1 - t), u1 = 2 * (1 - t) * t, u2 = t * t;
        bb(s, u0 * x0 + u1 * cx + u2 * x, u0 * y0 + u1 * cy + u2 * y);
    }
    bb(s, x, y);
}
static void d_cubic(hb_draw_funcs_t* f, void* d, hb_draw_state_t* st, float c1x, float c1y,
                    float c2x, float c2y, float x, float y, void* u)
{ (void)f; (void)st; (void)u; ((dstate*)d)->cubics++; bb(d, c1x, c1y); bb(d, c2x, c2y); bb(d, x, y); }

static hb_draw_funcs_t* dfuncs;
static long g_dump = -1;   /* --dump GID: print that glyph's paint calls */
static FILE* g_list;       /* --list FILE: one line per color glyph */
#define TR(p, ...) do { if (g_dump >= 0) { printf("    %*s", 2 * (p)->sp, ""); printf(__VA_ARGS__); } } while (0)

/* PaintComposite shapes, by mode and by what the backdrop and source hold */
enum { K_OPACITY, K_OPACITY_GRAD, K_MASK, K_MASK_TRANSL, K_MASKED_SHADE, K_OTHER, K_N };
static const char* kname[K_N] = {
    "SRC_IN over one unclipped solid (group opacity), source solid fills",
    "SRC_IN over one unclipped solid (group opacity), source has gradients",
    "SRC_IN of one unclipped fill over opaque fills (paint masked by the backdrop's shape)",
    "SRC_IN of one unclipped fill over fills with a translucent one",
    "blend of a source that holds a nested composite",
    "other"};

/* ---- per-font statistics ---- */
typedef struct {
    /* operations */
    long solid, fill_glyph, linear, radial, sweep, image;
    long clip_glyph, clip_rect, color_glyph_calls, groups;
    long tpush[T_N];               /* push_transform by kind, before composing */
    long mode_inner[NMODES];       /* push_group_for(mode) */
    /* gradients */
    long extend[3];
    long radial_kind[3];           /* concentric r0=0 / concentric r0>0 / two-point */
    long radial_focal, radial_inside;  /* two-point: r0 = 0; start circle inside end circle */
    long sweep_partial, stop_out_of_range, grad_translucent_stop;
    /* per-fill transforms of gradients */
    long grad_tkind[T_N];
    long radial_nonsimilar, sweep_nonsimilar, sweep_mirrored;
    /* fills */
    long fills, fill_fg, fill_translucent, fill_noclip, fill_multiclip,
         fill_rect_cut, fill_rect_rotated, fill_in_nonover, grad_in_nonover,
         fill_paint_xform_differs;
    /* palettes */
    long fg_glyphs;
    /* glyph-level counts (how many glyphs need each feature) */
    int n;
    int need_linear, need_radial, need_radial_2pt, need_sweep, need_nonover,
        need_repeat, need_reflect, need_fg, need_multiclip, need_rect_cut,
        need_noclip, need_grad_xform_nonsim, need_translucent, need_gradient,
        need_transform, near_limit;
    int need_mode[NMODES];
    /* arrays per glyph */
    int* layers; int* depth; int* segs; int* useg; int* od; int* stops_arr; long nstops, cap_stops;
    long max_callbacks;
    long comp_kind[NMODES][K_N];
    long opacity_overlap, opacity_single, opacity_disjoint;
    float max_cut;   /* font units the outline passes its clip rectangle */
    int glyph_opacity_only, glyph_blend;
} fontstat;

typedef enum { E_T, E_CG, E_CR, E_G } ekind;
typedef struct {
    ekind k;
    mat m;                  /* cumulative transform after this entry */
    float x0, y0, x1, y1;   /* clip bbox in root space */
    int rotated;            /* clip rectangle under a turn */
    int mode;               /* group mode */
    int nfill, ngrad, nunclip_solid, nunclip, ntransl, ncomp; /* fills and composites inside */
    int bd_nfill, bd_ngrad, bd_unclip_solid, bd_ntransl;    /* inner group: its backdrop's */
    int fill_start;                           /* inner group: first source fill */
} entry;

typedef struct { float x0, y0, x1, y1; } box;

typedef struct {
    fontstat* fs;
    hb_font_t* font;
    gcost* cache;
    entry st[MAXSTACK]; int sp;
    int layers, maxdepth, segs, callbacks, overflow;
    unsigned char* seen;   /* glyphs already counted in useg */
    int useg;
    /* per-glyph flags */
    int f_linear, f_radial, f_radial2, f_sweep, f_nonover, f_repeat, f_reflect,
        f_fg, f_multi, f_rectcut, f_noclip, f_gxns, f_transl, f_grad, f_xform, f_blend;
    int f_mode[NMODES];
    box* boxes; int nbox, capbox;   /* every fill's clip box, in order */
} pstate;

static mat cur(pstate* p) { if (p->sp == 0) { mat i = {1, 0, 0, 1, 0, 0}; return i; } return p->st[p->sp - 1].m; }

static int depth_now(pstate* p)
{
    int d = 0;
    for (int i = 0; i < p->sp; i++)
        if (p->st[i].k != E_T || tkind(p->st[i].m) != T_IDENT) d++;
    return d;
}

static void push(pstate* p, entry e)
{
    p->callbacks++;
    if (p->sp >= MAXSTACK) { p->overflow = 1; return; }
    p->st[p->sp++] = e;
    int d = depth_now(p);
    if (d > p->maxdepth) p->maxdepth = d;
}
static void pop(pstate* p) { p->callbacks++; if (p->sp > 0) p->sp--; TR(p, "pop\n"); }

static gcost* cost(pstate* p, hb_codepoint_t g)
{
    gcost* c = &p->cache[g];
    if (!c->done) {
        dstate s; memset(&s, 0, sizeof s); s.m.a = s.m.d = 1;
        hb_font_draw_glyph(p->font, g, dfuncs, &s);
        c->lines = s.lines; c->quads = s.quads; c->cubics = s.cubics; c->contours = s.contours;
        c->done = 1;
    }
    return c;
}

static void clip_glyph_entry(pstate* p, hb_codepoint_t g, entry* e)
{
    dstate s; memset(&s, 0, sizeof s); s.m = cur(p);
    hb_font_draw_glyph(p->font, g, dfuncs, &s);
    e->k = E_CG; e->m = cur(p);
    e->x0 = s.x0; e->y0 = s.y0; e->x1 = s.x1; e->y1 = s.y1;
    if (!s.any) { e->x0 = e->y0 = e->x1 = e->y1 = 0; }
    gcost* c = cost(p, g);
    p->segs += c->lines + c->quads + c->cubics;
    if (!p->seen[g]) { p->seen[g] = 1; p->useg += c->lines + c->quads + c->cubics; }
}

/* A fill: the layer the flattened model draws. kind 0 solid, 1 gradient. */
static void fill(pstate* p, int is_grad, int fg, int translucent, int extra_glyph_clip,
                 float gx0, float gy0, float gx1, float gy1)
{
    fontstat* fs = p->fs;
    p->layers++; fs->fills++;
    if (fg) { fs->fill_fg++; p->f_fg = 1; }
    if (translucent) { fs->fill_translucent++; p->f_transl = 1; }
    int nclip = extra_glyph_clip, inner_cg = -1, nonover = 0;
    float x0 = -1e30f, y0 = -1e30f, x1 = 1e30f, y1 = 1e30f;
    if (extra_glyph_clip) { x0 = gx0; y0 = gy0; x1 = gx1; y1 = gy1; }
    for (int i = 0; i < p->sp; i++) {
        entry* e = &p->st[i];
        if (e->k == E_CG) {
            nclip++; inner_cg = i;
            x0 = fmaxf(x0, e->x0); y0 = fmaxf(y0, e->y0);
            x1 = fminf(x1, e->x1); y1 = fminf(y1, e->y1);
        }
        if (e->k == E_G && e->mode != HB_PAINT_COMPOSITE_MODE_SRC_OVER) nonover = 1;
    }
    if (nclip == 0) { fs->fill_noclip++; p->f_noclip = 1; }
    if (nclip >= 2) { fs->fill_multiclip++; p->f_multi = 1; }
    if (nclip >= 1) {
        for (int i = 0; i < p->sp; i++) {
            entry* e = &p->st[i];
            if (e->k != E_CR) continue;
            if (e->rotated) { fs->fill_rect_rotated++; }
            float tol = 0.5f;
            if (x0 < e->x0 - tol || y0 < e->y0 - tol || x1 > e->x1 + tol || y1 > e->y1 + tol) {
                float cut = fmaxf(fmaxf(e->x0 - x0, e->y0 - y0), fmaxf(x1 - e->x1, y1 - e->y1));
                if (cut > fs->max_cut) fs->max_cut = cut;
                fs->fill_rect_cut++; p->f_rectcut = 1; break;
            }
        }
    }
    if (nonover) { fs->fill_in_nonover++; if (is_grad) fs->grad_in_nonover++; }
    for (int i = 0; i < p->sp; i++) {
        entry* e = &p->st[i];
        if (e->k != E_G) continue;
        e->nfill++; e->ngrad += is_grad;
        e->nunclip_solid += (nclip == 0 && !is_grad);
        e->nunclip += (nclip == 0);
        e->ntransl += translucent != 0;
    }
    if (nclip == 0) for (int i = 0; i < p->sp; i++) {
        entry* e = &p->st[i];
        if (e->k != E_CR) continue;
        x0 = fmaxf(x0, e->x0); y0 = fmaxf(y0, e->y0); x1 = fminf(x1, e->x1); y1 = fminf(y1, e->y1);
    }
    if (p->nbox == p->capbox) {
        p->capbox = p->capbox ? 2 * p->capbox : 1024;
        p->boxes = realloc(p->boxes, (size_t)p->capbox * sizeof(box));
    }
    box bx = {x0, y0, x1, y1};
    p->boxes[p->nbox++] = bx;
    if (is_grad && inner_cg >= 0) {
        mat a = p->st[inner_cg].m, b = cur(p);
        if (!(near(a.a, b.a) && near(a.b, b.b) && near(a.c, b.c) && near(a.d, b.d) &&
              near(a.e, b.e) && near(a.f, b.f)))
            fs->fill_paint_xform_differs++;
    }
}

/* ---- paint callbacks ---- */
static void cb_push_transform(hb_paint_funcs_t* f, void* d, float xx, float yx, float xy,
                              float yy, float dx, float dy, void* u)
{
    (void)f; (void)u; pstate* p = d;
    mat m = {xx, yx, xy, yy, dx, dy};
    int k = tkind(m);
    p->fs->tpush[k]++;
    if (k != T_IDENT) p->f_xform = 1;
    TR(p, "transform %s [%g %g %g %g %g %g]\n", tkind_name[k], xx, yx, xy, yy, dx, dy);
    entry e; memset(&e, 0, sizeof e); e.k = E_T; e.m = mul(cur(p), m);
    push(p, e);
}
static void cb_pop_transform(hb_paint_funcs_t* f, void* d, void* u) { (void)f; (void)u; pop(d); }
static void cb_pop_clip(hb_paint_funcs_t* f, void* d, void* u) { (void)f; (void)u; pop(d); }

static hb_bool_t cb_color_glyph(hb_paint_funcs_t* f, void* d, hb_codepoint_t g, hb_font_t* font, void* u)
{ (void)f; (void)g; (void)font; (void)u; TR((pstate*)d, "PaintColrGlyph %u\n", g); ((pstate*)d)->fs->color_glyph_calls++; ((pstate*)d)->callbacks++; return 0; }

static void cb_push_clip_glyph(hb_paint_funcs_t* f, void* d, hb_codepoint_t g, hb_font_t* font, void* u)
{
    (void)f; (void)font; (void)u; pstate* p = d;
    p->fs->clip_glyph++;
    entry e; memset(&e, 0, sizeof e);
    clip_glyph_entry(p, g, &e);
    TR(p, "clip glyph %u (%d segments)\n", g, cost(p, g)->lines + cost(p, g)->quads + cost(p, g)->cubics);
    push(p, e);
}
static void cb_push_clip_rect(hb_paint_funcs_t* f, void* d, float x0, float y0, float x1, float y1, void* u)
{
    (void)f; (void)u; pstate* p = d;
    p->fs->clip_rect++;
    entry e; memset(&e, 0, sizeof e); e.k = E_CR; e.m = cur(p);
    dstate s; memset(&s, 0, sizeof s); s.m = e.m;
    bb(&s, x0, y0); bb(&s, x1, y0); bb(&s, x0, y1); bb(&s, x1, y1);
    e.x0 = s.x0; e.y0 = s.y0; e.x1 = s.x1; e.y1 = s.y1;
    e.rotated = !(near(e.m.b, 0) && near(e.m.c, 0));
    TR(p, "clip rect %g %g %g %g\n", x0, y0, x1, y1);
    push(p, e);
}
static void cb_fill_glyph(hb_paint_funcs_t* f, void* d, hb_codepoint_t g, hb_font_t* font,
                          hb_bool_t fg, hb_color_t c, void* u)
{
    (void)f; (void)font; (void)u; pstate* p = d;
    p->callbacks++;
    p->fs->fill_glyph++; p->fs->clip_glyph++; p->fs->solid++;
    entry e; memset(&e, 0, sizeof e);
    clip_glyph_entry(p, g, &e);
    TR(p, "fill glyph %u solid #%02x%02x%02x%02x%s\n", g, hb_color_get_red(c), hb_color_get_green(c),
       hb_color_get_blue(c), hb_color_get_alpha(c), fg ? " fg" : "");
    fill(p, 0, fg, hb_color_get_alpha(c) < 255, 1, e.x0, e.y0, e.x1, e.y1);
}
static void cb_color(hb_paint_funcs_t* f, void* d, hb_bool_t fg, hb_color_t c, void* u)
{
    (void)f; (void)u; pstate* p = d;
    p->callbacks++; p->fs->solid++;
    TR(p, "solid #%02x%02x%02x%02x%s\n", hb_color_get_red(c), hb_color_get_green(c),
       hb_color_get_blue(c), hb_color_get_alpha(c), fg ? " fg" : "");
    fill(p, 0, fg, hb_color_get_alpha(c) < 255, 0, 0, 0, 0, 0);
}
static hb_bool_t cb_image(hb_paint_funcs_t* f, void* d, hb_blob_t* b, unsigned w, unsigned h,
                          hb_tag_t fmt, float sl, hb_glyph_extents_t* ex, void* u)
{
    (void)f; (void)b; (void)w; (void)h; (void)fmt; (void)sl; (void)ex; (void)u;
    pstate* p = d; p->callbacks++; p->fs->image++; return 1;
}

static void stops(pstate* p, hb_color_line_t* cl, int* fg, int* transl)
{
    fontstat* fs = p->fs;
    unsigned total = hb_color_line_get_color_stops(cl, 0, NULL, NULL);
    hb_color_stop_t buf[256];
    unsigned n = total < 256 ? total : 256;
    hb_color_line_get_color_stops(cl, 0, &n, buf);
    *fg = 0; *transl = 0;
    for (unsigned i = 0; i < n; i++) {
        if (buf[i].is_foreground) *fg = 1;
        if (hb_color_get_alpha(buf[i].color) < 255) *transl = 1;
        if (buf[i].offset < 0 || buf[i].offset > 1) fs->stop_out_of_range++;
    }
    if (*transl) fs->grad_translucent_stop++;
    if (fs->nstops == fs->cap_stops) {
        fs->cap_stops = fs->cap_stops ? fs->cap_stops * 2 : 4096;
        fs->stops_arr = realloc(fs->stops_arr, fs->cap_stops * sizeof(int));
    }
    fs->stops_arr[fs->nstops++] = (int)total;
    hb_paint_extend_t x = hb_color_line_get_extend(cl);
    if (x <= 2) fs->extend[x]++;
    if (x == HB_PAINT_EXTEND_REPEAT) p->f_repeat = 1;
    if (x == HB_PAINT_EXTEND_REFLECT) p->f_reflect = 1;
    p->f_grad = 1;
    int k = tkind(cur(p));
    fs->grad_tkind[k]++;
}

static void cb_linear(hb_paint_funcs_t* f, void* d, hb_color_line_t* cl, float x0, float y0,
                      float x1, float y1, float x2, float y2, void* u)
{
    (void)f; (void)u; (void)x0; (void)y0; (void)x1; (void)y1; (void)x2; (void)y2;
    pstate* p = d; int fg, tr;
    p->callbacks++; p->fs->linear++; p->f_linear = 1;
    TR(p, "linear (%g %g) (%g %g) (%g %g) stops %u extend %d\n", x0, y0, x1, y1, x2, y2,
       hb_color_line_get_color_stops(cl, 0, NULL, NULL), (int)hb_color_line_get_extend(cl));
    stops(p, cl, &fg, &tr);
    fill(p, 1, fg, tr, 0, 0, 0, 0, 0);
}
static void cb_radial(hb_paint_funcs_t* f, void* d, hb_color_line_t* cl, float x0, float y0,
                      float r0, float x1, float y1, float r1, void* u)
{
    (void)f; (void)u; (void)r1;
    pstate* p = d; int fg, tr;
    p->callbacks++; p->fs->radial++; p->f_radial = 1;
    TR(p, "radial (%g %g r %g) (%g %g r %g) stops %u extend %d\n", x0, y0, r0, x1, y1, r1,
       hb_color_line_get_color_stops(cl, 0, NULL, NULL), (int)hb_color_line_get_extend(cl));
    int conc = near(x0, x1) && near(y0, y1);
    int k = conc ? (fabsf(r0) < EPS ? 0 : 1) : 2;
    p->fs->radial_kind[k]++;
    if (k == 2) {
        if (fabsf(r0) < EPS) p->fs->radial_focal++;
        if (hypotf(x1 - x0, y1 - y0) + r0 <= r1) p->fs->radial_inside++;
    }
    if (k == 2) p->f_radial2 = 1;
    if (!similar(cur(p))) { p->fs->radial_nonsimilar++; p->f_gxns = 1; }
    stops(p, cl, &fg, &tr);
    fill(p, 1, fg, tr, 0, 0, 0, 0, 0);
}
static void cb_sweep(hb_paint_funcs_t* f, void* d, hb_color_line_t* cl, float x0, float y0,
                     float a0, float a1, void* u)
{
    (void)f; (void)u; (void)x0; (void)y0;
    pstate* p = d; int fg, tr;
    p->callbacks++; p->fs->sweep++; p->f_sweep = 1;
    TR(p, "sweep (%g %g) %g..%g rad stops %u extend %d\n", x0, y0, a0, a1,
       hb_color_line_get_color_stops(cl, 0, NULL, NULL), (int)hb_color_line_get_extend(cl));
    if (fabsf(fabsf(a1 - a0) - 2 * (float)M_PI) > 1e-3f) p->fs->sweep_partial++;
    mat m = cur(p);
    if (!similar(m)) { p->fs->sweep_nonsimilar++; p->f_gxns = 1; }
    if (m.a * m.d - m.b * m.c < 0) p->fs->sweep_mirrored++;
    stops(p, cl, &fg, &tr);
    fill(p, 1, fg, tr, 0, 0, 0, 0, 0);
}
static void cb_push_group(hb_paint_funcs_t* f, void* d, void* u)
{
    (void)f; (void)u; pstate* p = d;
    p->fs->groups++;
    entry e; memset(&e, 0, sizeof e); e.k = E_G; e.m = cur(p); e.mode = HB_PAINT_COMPOSITE_MODE_SRC_OVER;
    push(p, e);
}
static void cb_push_group_for(hb_paint_funcs_t* f, void* d, hb_paint_composite_mode_t mode, void* u)
{
    (void)f; (void)u; pstate* p = d;
    p->fs->groups++;
    if ((int)mode < NMODES) { p->fs->mode_inner[mode]++; p->f_mode[mode] = 1; }
    if (mode != HB_PAINT_COMPOSITE_MODE_SRC_OVER) p->f_nonover = 1;
    TR(p, "group %s\n", (int)mode < NMODES ? mode_name[mode] : "?");
    entry e; memset(&e, 0, sizeof e); e.k = E_G; e.m = cur(p); e.mode = (int)mode;
    if (p->sp > 0 && p->st[p->sp - 1].k == E_G) {
        entry* o = &p->st[p->sp - 1];
        e.bd_nfill = o->nfill; e.bd_ngrad = o->ngrad; e.bd_unclip_solid = o->nunclip_solid;
        e.bd_ntransl = o->ntransl;
    }
    e.fill_start = p->nbox;
    push(p, e);
}
static void cb_pop_group(hb_paint_funcs_t* f, void* d, hb_paint_composite_mode_t mode, void* u)
{
    (void)f; (void)u; pstate* p = d;
    if (mode != HB_PAINT_COMPOSITE_MODE_SRC_OVER && p->sp > 0 && (int)mode < NMODES) {
        entry* e = &p->st[p->sp - 1];
        int k;
        if (mode == HB_PAINT_COMPOSITE_MODE_SRC_IN && e->bd_nfill == 1 && e->bd_unclip_solid == 1 && e->ncomp == 0) {
            k = e->ngrad ? K_OPACITY_GRAD : K_OPACITY;
            int n = p->nbox - e->fill_start, over = 0;
            for (int i = e->fill_start; i < p->nbox && !over; i++)
                for (int j = i + 1; j < p->nbox; j++) {
                    box a = p->boxes[i], b = p->boxes[j];
                    if (fminf(a.x1, b.x1) > fmaxf(a.x0, b.x0) && fminf(a.y1, b.y1) > fmaxf(a.y0, b.y0)) { over = 1; break; }
                }
            if (n <= 1) p->fs->opacity_single++;
            else if (over) p->fs->opacity_overlap++;
            else p->fs->opacity_disjoint++;
        } else if (mode == HB_PAINT_COMPOSITE_MODE_SRC_IN && e->nfill == 1 && e->nunclip == 1 && e->ncomp == 0)
            k = e->bd_ntransl ? K_MASK_TRANSL : K_MASK;
        else if (e->ncomp > 0) k = K_MASKED_SHADE;
        else k = K_OTHER;
        p->fs->comp_kind[mode][k]++;
        if (k != K_OPACITY && k != K_OPACITY_GRAD && k != K_MASK && k != K_MASK_TRANSL) p->f_blend = 1;
        for (int i = 0; i < p->sp - 1; i++) if (p->st[i].k == E_G) p->st[i].ncomp++;
    }
    pop(d);
}

static int cmpint(const void* a, const void* b) { int x = *(const int*)a, y = *(const int*)b; return (x > y) - (x < y); }
static void pct(const char* name, int* v, long n)
{
    if (n == 0) { printf("  %-34s n=0\n", name); return; }
    qsort(v, (size_t)n, sizeof(int), cmpint);
    long i50 = (n - 1) / 2, i99 = (long)ceil(0.99 * n) - 1;
    double sum = 0; for (long i = 0; i < n; i++) sum += v[i];
    printf("  %-34s n=%ld min=%d median=%d p99=%d max=%d mean=%.1f sum=%.0f\n", name, n, v[0],
           v[i50], v[i99 < 0 ? 0 : i99], v[n - 1], sum / n, sum);
}

static uint32_t be32(const uint8_t* q) { return (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3]; }
static unsigned be16(const uint8_t* q) { return (unsigned)q[0] << 8 | q[1]; }

static unsigned has_table(hb_face_t* face, hb_tag_t t)
{
    hb_blob_t* b = hb_face_reference_table(face, t);
    unsigned r = hb_blob_get_length(b);
    hb_blob_destroy(b);
    return r;
}

static void survey(const char* path, hb_paint_funcs_t* pf)
{
    struct stat sb;
    if (stat(path, &sb) != 0) { printf("%s: cannot stat\n", path); return; }
    hb_blob_t* blob = hb_blob_create_from_file_or_fail(path);
    if (!blob) { printf("%s: cannot read\n", path); return; }
    hb_face_t* face = hb_face_create(blob, 0);
    hb_font_t* font = hb_font_create(face);
    unsigned upem = hb_face_get_upem(face);
    hb_font_set_scale(font, (int)upem, (int)upem);
    unsigned ng = hb_face_get_glyph_count(face);

    printf("== %s\n", path);
    printf("file bytes %lld, glyphs %u, upem %u\n", (long long)sb.st_size, ng, upem);
    printf("table bytes: COLR %u CPAL %u glyf %u CFF %u CFF2 %u CBDT %u sbix %u SVG %u fvar %u\n",
           has_table(face, HB_TAG('C','O','L','R')), has_table(face, HB_TAG('C','P','A','L')),
           has_table(face, HB_TAG('g','l','y','f')), has_table(face, HB_TAG('C','F','F',' ')),
           has_table(face, HB_TAG('C','F','F','2')), has_table(face, HB_TAG('C','B','D','T')),
           has_table(face, HB_TAG('s','b','i','x')), has_table(face, HB_TAG('S','V','G',' ')),
           has_table(face, HB_TAG('f','v','a','r')));
    printf("hb: has_layers(v0) %d has_paint(v1) %d has_png %d has_svg %d var_data %d\n",
           hb_ot_color_has_layers(face), hb_ot_color_has_paint(face), hb_ot_color_has_png(face),
           hb_ot_color_has_svg(face), hb_ot_var_has_data(face));
    {
        hb_blob_t* cb = hb_face_reference_table(face, HB_TAG('C','O','L','R'));
        unsigned len; const uint8_t* q = (const uint8_t*)hb_blob_get_data(cb, &len);
        if (len >= 14) {
            printf("COLR version %u, v0 base glyphs %u, v0 layers %u\n", be16(q), be16(q + 2), be16(q + 12));
            if (be16(q) >= 1 && len >= 34) {
                uint32_t bgl = be32(q + 14), ll = be32(q + 18), cl = be32(q + 22),
                         vim = be32(q + 26), vs = be32(q + 30);
                unsigned nbg = (bgl && bgl + 4 <= len) ? be32(q + bgl) : 0;
                unsigned nl = (ll && ll + 4 <= len) ? be32(q + ll) : 0;
                unsigned ncl = (cl && cl + 5 <= len) ? be32(q + cl + 1) : 0;
                printf("COLRv1: BaseGlyphPaintRecords %u, LayerList %u, ClipList %s (%u clips), "
                       "DeltaSetIndexMap %s, ItemVariationStore %s\n", nbg, nl, cl ? "yes" : "no", ncl,
                       vim ? "yes" : "no", vs ? "yes" : "no");
            }
        }
        hb_blob_destroy(cb);
    }
    unsigned npal = hb_ot_color_palette_get_count(face);
    unsigned nent = hb_ot_color_palette_get_colors(face, 0, 0, NULL, NULL);
    printf("CPAL palettes %u, entries per palette %u\n", npal, nent);

    fontstat fs; memset(&fs, 0, sizeof fs);
    fs.layers = calloc(ng, sizeof(int)); fs.depth = calloc(ng, sizeof(int)); fs.od = calloc(ng, sizeof(int));
    fs.segs = calloc(ng, sizeof(int)); fs.useg = calloc(ng, sizeof(int));
    pstate* p = calloc(1, sizeof *p);
    p->fs = &fs; p->font = font;
    p->cache = calloc(ng, sizeof(gcost));
    p->seen = calloc(ng, 1);
    int nv0 = 0, nv1 = 0, nboth = 0, noverflow = 0;
    uint32_t* rev = calloc(ng, sizeof(uint32_t)); /* glyph to its lowest code point */
    {
        hb_map_t* m = hb_map_create(); hb_set_t* u = hb_set_create();
        hb_face_collect_nominal_glyph_mapping(face, m, u);
        hb_codepoint_t cp = HB_SET_VALUE_INVALID;
        while (hb_set_next(u, &cp)) {
            hb_codepoint_t gg = hb_map_get(m, cp);
            if (gg < ng && !rev[gg]) rev[gg] = cp;
        }
        hb_map_destroy(m); hb_set_destroy(u);
    }

    for (hb_codepoint_t g = 0; g < ng; g++) {
        int v1 = hb_ot_color_glyph_has_paint(face, g);
        int v0 = hb_ot_color_glyph_get_layers(face, g, 0, NULL, NULL) > 0;
        if (!v0 && !v1) continue;
        if (g_dump >= 0 && (long)g != g_dump) continue;
        if (g_dump >= 0) printf("glyph %u:\n", g);
        nv0 += v0 && !v1; nv1 += v1; nboth += v0 && v1;
        pstate keep = *p;
        memset(p, 0, sizeof *p);
        p->fs = keep.fs; p->font = keep.font; p->cache = keep.cache; p->seen = keep.seen;
        p->boxes = keep.boxes; p->capbox = keep.capbox;
        hb_font_paint_glyph(font, g, pf, p, 0, HB_COLOR(0, 0, 0, 255));
        if (p->overflow || p->sp != 0) noverflow++;
        int k = fs.n++;
        fs.layers[k] = p->layers; fs.depth[k] = p->maxdepth; fs.segs[k] = p->segs; fs.useg[k] = p->useg;
        {   /* overdraw: the layers' boxes summed, over the box of them all, in percent */
            float ux0 = 1e30f, uy0 = 1e30f, ux1 = -1e30f, uy1 = -1e30f; double sum = 0;
            for (int i = 0; i < p->nbox; i++) {
                box b = p->boxes[i];
                if (b.x1 <= b.x0 || b.y1 <= b.y0 || b.x1 > 1e29f) continue;
                ux0 = fminf(ux0, b.x0); uy0 = fminf(uy0, b.y0); ux1 = fmaxf(ux1, b.x1); uy1 = fmaxf(uy1, b.y1);
                sum += (double)(b.x1 - b.x0) * (b.y1 - b.y0);
            }
            fs.od[k] = ux1 > ux0 ? (int)lround(100.0 * sum / ((double)(ux1 - ux0) * (uy1 - uy0))) : 0;
        }
        if (p->callbacks > fs.max_callbacks) fs.max_callbacks = p->callbacks;
        if (p->callbacks > 8000) fs.near_limit++;
        fs.need_linear += p->f_linear; fs.need_radial += p->f_radial; fs.need_radial_2pt += p->f_radial2;
        fs.need_sweep += p->f_sweep; fs.need_nonover += p->f_nonover; fs.need_repeat += p->f_repeat;
        fs.need_reflect += p->f_reflect; fs.need_fg += p->f_fg; fs.need_multiclip += p->f_multi;
        fs.need_rect_cut += p->f_rectcut; fs.need_noclip += p->f_noclip;
        fs.need_grad_xform_nonsim += p->f_gxns; fs.need_translucent += p->f_transl;
        fs.need_gradient += p->f_grad; fs.need_transform += p->f_xform;
        if (p->f_fg) fs.fg_glyphs++;
        if (p->f_nonover && !p->f_blend) fs.glyph_opacity_only++;
        if (p->f_blend) fs.glyph_blend++;
        if (g_list) {
            char name[64] = "";
            hb_font_get_glyph_name(font, g, name, sizeof name);
            if (rev[g]) snprintf(name + strlen(name), sizeof name - strlen(name), "%sU+%04X", name[0] ? " " : "", rev[g]);
            fprintf(g_list, "%s\t%u\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%s%s%s%s%s%s%s%s\n", path, g, name, p->layers,
                    p->maxdepth, p->segs, p->useg, fs.od[k], p->callbacks,
                    p->f_linear ? "L" : "", p->f_radial ? "R" : "", p->f_sweep ? "S" : "",
                    p->f_nonover ? "C" : "", p->f_xform ? "T" : "", p->f_noclip ? "U" : "",
                    p->f_rectcut ? "X" : "", p->f_fg ? "F" : "");
        }
        for (int m = 0; m < NMODES; m++) fs.need_mode[m] += p->f_mode[m];
        /* clear the unique-glyph marks: redraw-free by walking the cache is costly, so reset all */
        memset(p->seen, 0, ng);
    }

    printf("color glyphs %d (v1 %d, v0 only %d, both %d); stack overflow or unbalanced %d\n",
           fs.n, nv1, nv0, nboth, noverflow);
    printf("ops:\n");
    printf("  fills (layers) %ld: solid %ld (of which fill_glyph %ld), linear %ld, radial %ld, sweep %ld, image %ld\n",
           fs.fills, fs.solid, fs.fill_glyph, fs.linear, fs.radial, fs.sweep, fs.image);
    printf("  clip glyph %ld (incl. fill_glyph), clip rectangle %ld, PaintColrGlyph %ld, groups %ld\n",
           fs.clip_glyph, fs.clip_rect, fs.color_glyph_calls, fs.groups);
    printf("  push_transform by kind:");
    for (int k = 0; k < T_N; k++) printf(" %s %ld;", tkind_name[k], fs.tpush[k]);
    printf("\n  composite modes (push_group_for):");
    for (int m = 0; m < NMODES; m++) if (fs.mode_inner[m]) printf(" %s %ld (%d glyphs);", mode_name[m], fs.mode_inner[m], fs.need_mode[m]);
    printf("\n  PaintComposite (groups / 2) %ld\n", fs.groups / 2);
    for (int m = 0; m < NMODES; m++) for (int k = 0; k < K_N; k++)
        if (fs.comp_kind[m][k]) printf("    %s: %s: %ld\n", mode_name[m], kname[k], fs.comp_kind[m][k]);
    printf("    group opacity: source of one fill %ld, fills' boxes disjoint %ld, boxes overlap %ld\n",
           fs.opacity_single, fs.opacity_disjoint, fs.opacity_overlap);
    printf("    glyphs whose only non-SRC_OVER composites are SRC_IN (opacity or mask) %d, with a blend or other %d\n",
           fs.glyph_opacity_only, fs.glyph_blend);
    printf("gradients:\n");
    printf("  extend pad %ld repeat %ld reflect %ld\n", fs.extend[0], fs.extend[1], fs.extend[2]);
    printf("  radial: concentric r0=0 %ld, concentric r0>0 %ld, two-point %ld (r0=0 %ld, start circle inside end %ld);\n"
           "    under non-similarity %ld\n",
           fs.radial_kind[0], fs.radial_kind[1], fs.radial_kind[2], fs.radial_focal, fs.radial_inside, fs.radial_nonsimilar);
    printf("  sweep: partial %ld, under non-similarity %ld, mirrored %ld\n", fs.sweep_partial, fs.sweep_nonsimilar, fs.sweep_mirrored);
    printf("  stops outside [0,1] %ld, gradients with a translucent stop %ld\n", fs.stop_out_of_range, fs.grad_translucent_stop);
    printf("  gradient cumulative transform:");
    for (int k = 0; k < T_N; k++) printf(" %s %ld;", tkind_name[k], fs.grad_tkind[k]);
    printf("\n  gradient transform differs from its clip glyph's %ld\n", fs.fill_paint_xform_differs);
    pct("color stops per gradient", fs.stops_arr, fs.nstops);
    printf("fills:\n");
    printf("  foreground %ld, translucent %ld, no glyph clip %ld, >=2 glyph clips %ld,\n"
           "  clip rectangle cuts the clip glyph %ld, clip rectangle under a turn %ld,\n"
           "  inside a non-SRC_OVER group %ld (gradients %ld); largest cut %.1f units\n",
           fs.fill_fg, fs.fill_translucent, fs.fill_noclip, fs.fill_multiclip, fs.fill_rect_cut,
           fs.fill_rect_rotated, fs.fill_in_nonover, fs.grad_in_nonover, fs.max_cut);
    printf("per glyph:\n");
    pct("layers", fs.layers, fs.n);
    pct("nesting depth (non-identity pushes)", fs.depth, fs.n);
    pct("outline segments, all clip glyphs", fs.segs, fs.n);
    pct("outline segments, distinct glyphs", fs.useg, fs.n);
    pct("overdraw, layer boxes / ink box, %", fs.od, fs.n);
    printf("  max callbacks in one glyph %ld; glyphs over 8000 callbacks %d\n", fs.max_callbacks, fs.near_limit);
    printf("glyphs needing: gradient %d, linear %d, radial %d, two-point radial %d, sweep %d,\n"
           "  non-SRC_OVER mode %d, repeat %d, reflect %d, foreground %d, translucent %d,\n"
           "  non-identity transform %d, radial/sweep under non-similarity %d,\n"
           "  >=2 glyph clips %d, cutting clip rect %d, fill without glyph clip %d\n",
           fs.need_gradient, fs.need_linear, fs.need_radial, fs.need_radial_2pt, fs.need_sweep,
           fs.need_nonover, fs.need_repeat, fs.need_reflect, fs.need_fg, fs.need_translucent,
           fs.need_transform, fs.need_grad_xform_nonsim, fs.need_multiclip, fs.need_rect_cut,
           fs.need_noclip);
    {
        long tl = 0, tq = 0, tc = 0;
        for (unsigned g = 0; g < ng; g++) if (p->cache[g].done) { tl += p->cache[g].lines; tq += p->cache[g].quads; tc += p->cache[g].cubics; }
        printf("distinct clip glyphs' segments: lines %ld, quadratics %ld, cubics %ld\n", tl, tq, tc);
    }
    printf("\n");
    free(fs.layers); free(fs.od); free(fs.depth); free(fs.segs); free(fs.useg); free(fs.stops_arr);
    free(p->cache); free(p->seen); free(p->boxes); free(p); free(rev);
    hb_font_destroy(font); hb_face_destroy(face); hb_blob_destroy(blob);
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: colr_survey font.ttf [...]\n"); return 2; }
    printf("HarfBuzz %s\n", hb_version_string());
    dfuncs = hb_draw_funcs_create();
    hb_draw_funcs_set_move_to_func(dfuncs, d_move, NULL, NULL);
    hb_draw_funcs_set_line_to_func(dfuncs, d_line, NULL, NULL);
    hb_draw_funcs_set_quadratic_to_func(dfuncs, d_quad, NULL, NULL);
    hb_draw_funcs_set_cubic_to_func(dfuncs, d_cubic, NULL, NULL);
    hb_draw_funcs_make_immutable(dfuncs);
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
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dump") && i + 1 < argc) { g_dump = atol(argv[++i]); continue; }
        if (!strcmp(argv[i], "--list") && i + 1 < argc) {
            g_list = fopen(argv[++i], "wb");
            if (!g_list) { perror(argv[i]); return 1; }
            fprintf(g_list, "font\tgid\tname\tlayers\tdepth\tsegments\tdistinct_segments\toverdraw_pct\tcallbacks\tflags\n");
            continue;
        }
        survey(argv[i], pf);
    }
    if (g_list) fclose(g_list);
    hb_paint_funcs_destroy(pf);
    hb_draw_funcs_destroy(dfuncs);
    return 0;
}
