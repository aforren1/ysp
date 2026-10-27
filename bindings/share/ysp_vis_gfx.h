/* ysp_vis_gfx.h - the host-independent gfx part of ysp-vis (Python ysp.gfx, MEX ysp_gfx)
 *
 * Not a library header. ysp_vis_core.h includes it at its end, so it sees
 * yv_screen, the seconds helpers and the shims of ysp.timeline's table.
 * What both hosts do the same way lives here: the gfx on a screen (its
 * options, the open started while the screen settles, prime), the objects
 * a gfx owns (stimuli, draw lists, groups, textures), their parameters by
 * name (ysp/gfx.h's parameter tables), instances, dot fields, the timeline
 * bindings applied in the frame, hit tests, capture and the reads.
 *
 * OWNERSHIP. A ygfx_stim holds raw pointers (path, inst, palette, group)
 * that must stay valid while it is drawn. Here every one of them points
 * into memory the binding owns: the yv_stim itself (path, palette), the
 * instances' element array (the core's, or a host buffer the host keeps
 * alive as long as the yv_stim), a yv_group. Each object counts its users
 * (the lists, stimuli and bindings that point at it); MEX refuses to free
 * one in use, Python's references keep it alive.
 *
 * LIFETIME. Every object is in its gfx's list. When the gfx closes (or its
 * screen does), the objects stay as memory but are dead: every call on one
 * returns YGFX_ERR_CLOSED. The host frees them later (Python: when the
 * object goes; MEX: the gfx's close frees its handles). No object outlives
 * its yv_gfx struct: Python's objects hold the Gfx, MEX frees them first.
 *
 * TIMELINE BINDINGS. A binding writes values[channel] of a timeline into a
 * stimulus, group or element field (ygfx_apply) in yv_gfx_begin(), so a
 * tweened stimulus costs no host call per frame. A gfx pins each timeline
 * it uses once (the owner's acquire(), through ysp_timeline_api.h); the
 * pin goes when its last binding does (or at close). With evaluate on, the
 * gfx evaluates the timeline at the screen frame's predicted onset first.
 *
 * MEMORY. Making objects and bindings allocates (setup). yv_gfx_begin(),
 * the draws, yv_gfx_end(), the bulk sets and the hit tests allocate
 * nothing.
 *
 * Public domain / MIT-0, as the headers.
 */
#ifndef YSP_VIS_GFX_H
#define YSP_VIS_GFX_H

#ifndef YSP_VIS_CORE_H
#error "include ysp_vis_core.h, which includes this file"
#endif

#include <stdarg.h>
#include <stdio.h>

#define YV_MAX_TL      8     /* timelines one gfx binds to                  */
#define YV_MAX_DEFER   64    /* GL frees put off to the end of a frame      */
#define YV_ERR_HOST    YSP_SHARE_ERR_HOST   /* the host's error is set     */

enum { YV_O_STIM = 1, YV_O_LIST, YV_O_GROUP, YV_O_TEX };

/* The stimulus kinds a host makes (ygfx_kind, plus instances). */
enum { YV_K_SHAPE = 1, YV_K_GRATING, YV_K_GABOR, YV_K_NOISE, YV_K_IMAGE, YV_K_DOTS, YV_K_INSTANCES };

struct yv_gfx;

typedef struct yv_obj {
    int             type;      /* YV_O_*                                       */
    struct yv_gfx*  g;
    struct yv_obj  *prev, *next;
    int             users;     /* lists, stimuli and bindings that point here  */
} yv_obj;

typedef struct yv_group {
    yv_obj     o;
    ygfx_group grp;
} yv_group;

typedef struct yv_tex {
    yv_obj   o;
    ygfx_tex t;
    int32_t  w, h, format;
} yv_tex;

typedef struct yv_stim {
    yv_obj     o;
    ygfx_stim  s;
    int        vkind;          /* YV_K_*                                       */
    float*     path;           /* POLYLINE, QBEZIER: 2 x n_path, owned         */
    ygfx_inst* inst;           /* INSTANCES: n_inst elements                   */
    int        inst_owned;     /* 1: the core frees inst; 0: the host's buffer */
    float      palette[3 * YGFX_MAX_PALETTE];
    yv_group*  group;          /* users counted                                */
    yv_tex*    tex;            /* IMAGE; users counted                          */
    uint32_t   dots_cap;       /* DOTS: x, y pairs the buffer holds             */
    int        bound;          /* bindings that write it                       */
} yv_stim;

typedef struct yv_list {
    yv_obj    o;
    yv_stim** it;
    int       n, cap;
} yv_list;

/* One timeline the gfx binds to, with its bindings. */
typedef struct yv_tl {
    ytl_timeline* tl;          /* pinned by acquire()                          */
    int           evaluate;    /* evaluate it in yv_gfx_begin()                */
    ygfx_bind*    b;
    yv_obj**      who;         /* the object each binding writes               */
    int           n, cap;
} yv_tl;

typedef struct yv_gfx {
    ygfx_gfx        g;
    yv_screen*      scr;
    int             open;      /* ygfx_open() succeeded                        */
    int             in_frame;
    int             started;   /* ygfx_open_start() ran in on_context          */
    int             prime;     /* ygfx_prime() at open                          */
    int             has_cal, has_lum;
    ygfx_desc       desc;
    ycol_cal        cal;
    ycol_lum        lum;
    ygfx_file_cache fc;
    char            cache_dir[512];
    yv_obj*         objs;      /* every object it made                         */
    yv_tl           tl[YV_MAX_TL];
    int             n_tl;
    uint32_t        defer_tex[YV_MAX_DEFER], defer_buf[YV_MAX_DEFER];
    int             n_defer_tex, n_defer_buf;
    uint64_t        frames;
} yv_gfx;

/* ---- errors ------------------------------------------------------------------ */

static int yv_fail(char* err, size_t cap, int code, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return code;
}

/* A gfx call failed: its message, or its code's text. */
static int yv_gfx_err(yv_gfx* g, int code, char* err, size_t cap) {
    const char* m = ygfx_error(&g->g);
    snprintf(err, cap, "%s", m && m[0] ? m : ygfx_strerror(code));
    return code;
}

/* ---- names and values: what a host passes ------------------------------------ */

/* One argument: numbers, a name, or an object the host resolved. */
typedef struct yv_kv {
    const char*   name;
    const double* v;           /* numbers, row-major for a matrix              */
    int           n;
    const char*   s;           /* a name (or a hex color); NULL for numbers    */
    yv_obj*       obj;         /* a group, a texture, a template               */
    int           used;
} yv_kv;

static yv_kv* yv_take(yv_kv* kv, int n, const char* name) {
    int i;
    for (i = 0; i < n; i++)
        if (!kv[i].used && strcmp(kv[i].name, name) == 0) { kv[i].used = 1; return &kv[i]; }
    return NULL;
}

static int yv_num(const yv_kv* k, double* out, char* err, size_t cap) {
    if (k->s || k->obj || k->n != 1 || !(k->v[0] == k->v[0]))
        return yv_fail(err, cap, YGFX_ERR_ARG, "%s: a number (not NaN)", k->name);
    *out = k->v[0];
    return 0;
}

static int yv_int(const yv_kv* k, int lo, int hi, int* out, char* err, size_t cap) {
    double d;
    if (yv_num(k, &d, err, cap) < 0) return YGFX_ERR_ARG;
    if (d != floor(d) || d < lo || d > hi) return yv_fail(err, cap, YGFX_ERR_ARG, "%s: an integer, %d to %d", k->name, lo, hi);
    *out = (int)d;
    return 0;
}

/* n floats (want), or one that fills them all when `fill`. */
static int yv_floats(const yv_kv* k, float* out, int want, int fill, char* err, size_t cap) {
    int i;
    if (k->s || k->obj || !(k->n == want || (fill && k->n == 1)))
        return yv_fail(err, cap, YGFX_ERR_ARG, "%s: %d numbers%s", k->name, want, fill ? " (or one for all)" : "");
    for (i = 0; i < want; i++) {
        double d = k->v[k->n == 1 ? 0 : i];
        if (!(d == d)) return yv_fail(err, cap, YGFX_ERR_ARG, "%s: NaN refused", k->name);
        out[i] = (float)d;
    }
    return 0;
}

static int yv_lower_eq_n(const char* a, const char* b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        char c = (a[i] >= 'A' && a[i] <= 'Z') ? (char)(a[i] + 32) : a[i];
        char d = (b[i] >= 'A' && b[i] <= 'Z') ? (char)(b[i] + 32) : b[i];
        if (c != d || !d) return 0;
    }
    return 1;
}

/* A name in a "name=value,..." list (the parameter tables' values). */
static int yv_enum_value(const char* values, const char* name, int* out) {
    const char* p = values;
    size_t len = strlen(name);
    while (p && *p) {
        const char* eq = strchr(p, '=');
        const char* end;
        if (!eq) return -1;
        end = strchr(eq, ',');
        if ((size_t)(eq - p) == len && yv_lower_eq_n(p, name, len)) { *out = atoi(eq + 1); return 0; }
        p = end ? end + 1 : NULL;
    }
    return -1;
}

static const char* yv_enum_name(const char* values, int v, char* buf, size_t cap) {
    const char* p = values;
    while (p && *p) {
        const char* eq = strchr(p, '=');
        const char* end;
        if (!eq) break;
        end = strchr(eq, ',');
        if (atoi(eq + 1) == v) {
            size_t n = (size_t)(eq - p);
            if (n >= cap) n = cap - 1;
            memcpy(buf, p, n);
            buf[n] = 0;
            return buf;
        }
        p = end ? end + 1 : NULL;
    }
    snprintf(buf, cap, "%d", v);
    return buf;
}

static const char* const yv_align_values =
    "center=0,top_left=1,top=2,top_right=3,left=4,right=5,bottom_left=6,bottom=7,bottom_right=8";

static int yv_align(const yv_kv* k, int* out, char* err, size_t cap) {
    if (!k->s || yv_enum_value(yv_align_values, k->s, out) < 0)
        return yv_fail(err, cap, YGFX_ERR_ARG, "%s: center, top_left, top, top_right, left, right, bottom_left, bottom "
                                               "or bottom_right", k->name);
    return 0;
}

static const float yv_align_fx[9] = { 0.5f, 0.0f, 0.5f, 1.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f };
static const float yv_align_fy[9] = { 0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f };

/* ---- the objects' list --------------------------------------------------------- */

static void yv_link(yv_gfx* g, yv_obj* o, int type) {
    o->type = type;
    o->g = g;
    o->prev = NULL;
    o->next = g->objs;
    if (g->objs) g->objs->prev = o;
    g->objs = o;
}

static void yv_unlink(yv_obj* o) {
    if (!o->g) return;
    if (o->prev) o->prev->next = o->next;
    else if (o->g->objs == o) o->g->objs = o->next;
    if (o->next) o->next->prev = o->prev;
    o->prev = o->next = NULL;
}

/* 0, or YGFX_ERR_CLOSED with the message: every call on an object asks. */
static int yv_live(const yv_obj* o, char* err, size_t cap) {
    if (!o || !o->g || !o->g->open)
        return yv_fail(err, cap, YGFX_ERR_CLOSED, "ysp_gfx: its gfx (or the gfx's screen) is closed");
    return 0;
}

/* GL frees inside a frame wait for its end: a queued draw may use them. */
static void yv_free_tex_id(yv_gfx* g, ygfx_tex t) {
    if (!t.id || !g->open) return;
    if (g->in_frame && g->n_defer_tex < YV_MAX_DEFER) { g->defer_tex[g->n_defer_tex++] = t.id; return; }
    ygfx_texture_free(&g->g, t);
}

static void yv_free_buf_id(yv_gfx* g, ygfx_buf b) {
    if (!b.id || !g->open) return;
    if (g->in_frame && g->n_defer_buf < YV_MAX_DEFER) { g->defer_buf[g->n_defer_buf++] = b.id; return; }
    ygfx_buffer_free(&g->g, b);
}

static void yv_flush_defer(yv_gfx* g) {
    int i;
    for (i = 0; i < g->n_defer_tex; i++) { ygfx_tex t; t.id = g->defer_tex[i]; ygfx_texture_free(&g->g, t); }
    for (i = 0; i < g->n_defer_buf; i++) { ygfx_buf b; b.id = g->defer_buf[i]; ygfx_buffer_free(&g->g, b); }
    g->n_defer_tex = g->n_defer_buf = 0;
}

/* ---- the gfx: options, open (with the start in the screen's hook), close ------ */

static void yv_gfx_defaults(yv_gfx* g) {
    memset(&g->desc, 0, sizeof g->desc);
    g->prime = 1;
    g->cache_dir[0] = 0;
    /* The per-user program cache unless cache=False: a warm cache opens in
     * 11 to 35 ms, a cold one compiles for 2.3 to 5.3 s (docs/gfx.md, v0.4). */
    if (ygfx_default_cache_dir(g->cache_dir, sizeof g->cache_dir) != YGFX_OK) g->cache_dir[0] = 0;
}

/* The gfx options (Gfx(**kw), ysp_gfx('open', h, ...), the screen's gfx=).
 * Bytes (cal, lum) come as numbers 0..255. */
static int yv_gfx_options(yv_gfx* g, yv_kv* kv, int n, char* err, size_t cap) {
    int i;
    for (i = 0; i < n; i++) {
        yv_kv* k = &kv[i];
        int e;
        if (k->used) continue;
        k->used = 1;
        if (!strcmp(k->name, "background")) {
            if (yv_floats(k, g->desc.background, 3, 1, err, cap) < 0) return YGFX_ERR_ARG;
        } else if (!strcmp(k->name, "units")) {
            if (!k->s || yv_enum_value("px=0,deg=1", k->s, &e) < 0) return yv_fail(err, cap, YGFX_ERR_ARG, "units: 'px' or 'deg'");
            g->desc.units = (ygfx_units)e;
        } else if (!strcmp(k->name, "view")) {
            float v[2];
            if (yv_floats(k, v, 2, 0, err, cap) < 0) return yv_fail(err, cap, YGFX_ERR_ARG, "view: (distance_mm, width_mm)");
            g->desc.view.distance_mm = v[0];
            g->desc.view.width_mm = v[1];
        } else if (!strcmp(k->name, "dither")) {
            if (!k->s || yv_enum_value("none=0,ordered=1,noise=2", k->s, &e) < 0)
                return yv_fail(err, cap, YGFX_ERR_ARG, "dither: 'none', 'ordered' or 'noise'");
            g->desc.dither = (ygfx_dither)e;
        } else if (!strcmp(k->name, "seed")) {
            double d;
            if (yv_num(k, &d, err, cap) < 0 || d < 0 || d > 4294967295.0 || d != floor(d))
                return yv_fail(err, cap, YGFX_ERR_ARG, "seed: an integer, 0 to 2^32 - 1");
            g->desc.seed = (uint32_t)d;
        } else if (!strcmp(k->name, "cones")) {
            if (!k->s || yv_enum_value("ss2=0,ss10=1", k->s, &e) < 0) return yv_fail(err, cap, YGFX_ERR_ARG, "cones: 'ss2' or 'ss10'");
            g->desc.cones = e;
        } else if (!strcmp(k->name, "max_draws")) {
            if (yv_int(k, 1, 65536, &e, err, cap) < 0) return YGFX_ERR_ARG;
            g->desc.max_draws = e;
        } else if (!strcmp(k->name, "max_instances")) {
            if (yv_int(k, 1, 1 << 22, &e, err, cap) < 0) return YGFX_ERR_ARG;
            g->desc.max_instances = e;
        } else if (!strcmp(k->name, "prime")) {
            double d;
            if (yv_num(k, &d, err, cap) < 0) return YGFX_ERR_ARG;
            g->prime = d != 0.0;
        } else if (!strcmp(k->name, "cache")) {
            if (k->s) {
                if (strlen(k->s) >= sizeof g->cache_dir) return yv_fail(err, cap, YGFX_ERR_ARG, "cache: the folder's path is too long");
                snprintf(g->cache_dir, sizeof g->cache_dir, "%s", k->s);
            } else {
                double d;
                if (yv_num(k, &d, err, cap) < 0) return yv_fail(err, cap, YGFX_ERR_ARG, "cache: True, False or a folder");
                if (d == 0.0) g->cache_dir[0] = 0;
                else if (ygfx_default_cache_dir(g->cache_dir, sizeof g->cache_dir) != YGFX_OK) g->cache_dir[0] = 0;
            }
        } else if (!strcmp(k->name, "cal") || !strcmp(k->name, "lum")) {
            /* The bytes of a .yspcal or .ysplum file (ysp.color's save()). */
            unsigned char* b;
            int j, rc;
            char m[256];
            if (k->s || k->obj || k->n < 1) return yv_fail(err, cap, YGFX_ERR_ARG, "%s: the bytes of a file", k->name);
            b = (unsigned char*)malloc((size_t)k->n);
            if (!b) return yv_fail(err, cap, YGFX_ERR_FULL, "out of memory");
            for (j = 0; j < k->n; j++) {
                if (k->v[j] < 0 || k->v[j] > 255 || k->v[j] != floor(k->v[j])) {
                    free(b);
                    return yv_fail(err, cap, YGFX_ERR_ARG, "%s: bytes, 0..255", k->name);
                }
                b[j] = (unsigned char)k->v[j];
            }
            m[0] = 0;
            if (k->name[0] == 'c') rc = ycol_cal_load(&g->cal, b, (size_t)k->n, m, sizeof m);
            else rc = ycol_lum_load(&g->lum, b, (size_t)k->n, m, sizeof m);
            free(b);
            if (rc < 0) return yv_fail(err, cap, YGFX_ERR_ARG, "%s: %s", k->name, m[0] ? m : "refused");
            if (k->name[0] == 'c') g->has_cal = 1; else g->has_lum = 1;
        } else {
            return yv_fail(err, cap, YGFX_ERR_ARG,
                           "unknown gfx option '%s' (background, units, view, dither, seed, cones, cal, lum, max_draws, "
                           "max_instances, cache, prime)", k->name);
        }
    }
    return 0;
}

/* Points the desc at the gfx's own storage: it must stay valid until
 * ygfx_open() returns (OPEN START), and yv_gfx does not move. */
static void yv_gfx_bind_desc(yv_gfx* g, yscr_screen* s) {
    g->desc.screen = s;
    g->desc.cal = g->has_cal ? &g->cal : NULL;
    g->desc.lum = g->has_lum ? &g->lum : NULL;
    g->desc.cache = g->cache_dir[0] ? ygfx_file_cache_init(&g->fc, g->cache_dir) : NULL;
}

/* The screen's desc.on_context: the programs compile while it settles. */
static void yv_gfx_on_context(void* ctx, yscr_screen* s) {
    yv_gfx* g = (yv_gfx*)ctx;
    yv_gfx_bind_desc(g, s);
    g->started = 1;
    ygfx_open_start(&g->g, &g->desc);   /* a failed start leaves ygfx_open() to try again */
}

/* Opens g on v: finishes a start (v->gstart is g) or opens whole. Then
 * primes the built-in programs. true, or false with ygfx_error(). */
static bool yv_gfx_open(yv_gfx* g, yv_screen* v) {
    if (v->gstart == g) {
        v->gstart = NULL;
        if (!g->started) yv_gfx_bind_desc(g, &v->s);   /* the hook did not run */
    } else {
        yv_gfx_bind_desc(g, &v->s);
    }
    if (!ygfx_open(&g->g, &g->desc)) return false;
    g->scr = v;
    g->open = 1;
    v->gfx = g;
    if (g->prime && ygfx_prime(&g->g) < 0) {
        ygfx_close(&g->g);
        g->open = 0;
        v->gfx = NULL;
        g->scr = NULL;
        return false;
    }
    return true;
}

static void yv_tl_release_all(yv_gfx* g);

/* Closes the GL side; the objects stay, dead, until the host frees them. */
static void yv_gfx_close(yv_gfx* g) {
    yv_obj* o;
    if (!g) return;
    yv_tl_release_all(g);
    if (g->open || g->started) ygfx_close(&g->g);
    for (o = g->objs; o; o = o->next) {
        /* their GL objects went with ygfx_close() */
        if (o->type == YV_O_TEX) ((yv_tex*)o)->t.id = 0;
        else if (o->type == YV_O_STIM && ((yv_stim*)o)->vkind == YV_K_DOTS) ((yv_stim*)o)->s.buf.id = 0;
    }
    g->n_defer_tex = g->n_defer_buf = 0;
    g->open = 0;
    g->started = 0;
    g->in_frame = 0;
    if (g->scr && g->scr->gfx == g) g->scr->gfx = NULL;
    g->scr = NULL;
}

/* A screen's started gfx that no host opened goes with the screen. */
static void yv_gstart_drop(yv_screen* v) {
    if (!v->gstart) return;
    yv_gfx_close(v->gstart);
    free(v->gstart);
    v->gstart = NULL;
}

/* ---- the attribute table: a stimulus's parameters by name ---------------------- */

enum {
    YV_A_FLOAT = 1,   /* a float row of ygfx_params(): offset, 1 float           */
    YV_A_VEC,         /* floats at an offset: color, dir, tint, src, shape_p, trim */
    YV_A_ENUM,        /* a row of ygfx_enum_params()                             */
    YV_A_ALIAS,       /* a shape parameter by its meaning: shape_p[k]            */
    YV_A_SPECIAL      /* anchor, vertices, path, group, tex, positions, count     */
};

typedef struct yv_attr {
    int         type;
    const char* name;
    uint32_t    off;           /* in ygfx_stim                                  */
    int         width;         /* floats                                         */
    int         fill;          /* one value may fill them all                    */
    uint32_t    kinds;         /* 1 << ygfx_kind that read it                     */
    int         row;           /* YV_A_FLOAT: YGFX_P_*; YV_A_ENUM: YGFX_E_*        */
    int         index;         /* YV_A_ALIAS: shape_p index                       */
    int         minn;          /* YV_A_VEC: fewest values (the rest 0; tint: alpha 1) */
} yv_attr;

#define YV_KB(k) (1u << (k))
#define YV_OFF(f) (uint32_t)offsetof(ygfx_stim, f)
#define YV_K_COL (YV_KB(YGFX_SHAPE) | YV_KB(YGFX_DOTS) | YV_KB(YGFX_IMAGE) | YV_KB(YGFX_USER) | YV_KB(YGFX_TEXT))
#define YV_K_MOD (YV_KB(YGFX_GRATING) | YV_KB(YGFX_GABOR) | YV_KB(YGFX_NOISE) | YV_KB(YGFX_IMAGE) | YV_KB(YGFX_USER))
#define YV_K_APT (YV_KB(YGFX_SHAPE) | YV_KB(YGFX_GRATING) | YV_KB(YGFX_NOISE) | YV_KB(YGFX_USER) | YV_KB(YGFX_DOTS))

static const yv_attr yv_vecs[] = {
    { YV_A_VEC, "color", YV_OFF(color), 3, 1, YV_K_COL, 0, 0, 3 },
    { YV_A_VEC, "dir", YV_OFF(dir), 3, 0, YV_K_MOD, 0, 0, 3 },
    { YV_A_VEC, "tint", YV_OFF(tint), 4, 0, YV_KB(YGFX_IMAGE), 0, 0, 3 },
    { YV_A_VEC, "src", YV_OFF(src), 4, 0, YV_KB(YGFX_IMAGE), 0, 0, 4 },
    { YV_A_VEC, "shape_p", YV_OFF(shape_p), 4, 0, YV_K_APT, 0, 0, 1 },
    { YV_A_VEC, "trim", YV_OFF(trim), 2, 0, YV_KB(YGFX_SHAPE), 0, 0, 2 },
    { YV_A_VEC, "p", YV_OFF(p), 32, 0, YV_KB(YGFX_SHAPE) | YV_KB(YGFX_USER), 0, 0, 1 },
};

/* The shape parameters by meaning, per SHAPE kind (ygfx_shape_kind). */
typedef struct yv_alias { const char* name; int shape; int index; int width; } yv_alias;
static const yv_alias yv_aliases[] = {
    { "radius", YGFX_RECT, 1, 1 },     { "inner", YGFX_ANNULUS, 0, 1 },    { "width", YGFX_LINE, 0, 1 },
    { "width", YGFX_CROSS, 0, 1 },     { "rounding", YGFX_POLYGON, 1, 1 }, { "radii", YGFX_RRECT, 0, 4 },
    { "radius", YGFX_RRECT, 0, -4 },   { "sweep", YGFX_ARC, 0, 1 },        { "thickness", YGFX_ARC, 1, 1 },
    { "start", YGFX_ARC, 2, 1 },       { "sweep", YGFX_PIE, 0, 1 },        { "start", YGFX_PIE, 2, 1 },
    { "radii", YGFX_CAPSULE, 0, 2 },   { "radius", YGFX_CAPSULE, 0, -2 },  { "sides", YGFX_NGON, 0, 1 },
    { "rounding", YGFX_NGON, 1, 1 },   { "points", YGFX_STAR, 0, 1 },      { "inner", YGFX_STAR, 1, 1 },
    { "rounding", YGFX_STAR, 2, 1 },   { "width", YGFX_QBEZIER, 0, 1 },    { "width", YGFX_POLYLINE, 0, 1 },
};

static const char* const yv_specials[] = { "anchor", "vertices", "path", "group", "tex", "positions", "count", "space", NULL };

/* Finds name for a stimulus of kind (ygfx_kind) and shape. 0, or ERR_ARG. */
static int yv_attr_find(const char* name, int kind, int shape, yv_attr* a, char* err, size_t cap) {
    const ygfx_param* t;
    int n, i, known = 0;
    memset(a, 0, sizeof *a);
    a->name = name;
    for (i = 0; yv_specials[i]; i++)
        if (!strcmp(name, yv_specials[i])) { a->type = YV_A_SPECIAL; return 0; }
    for (i = 0; i < (int)(sizeof yv_vecs / sizeof yv_vecs[0]); i++)
        if (!strcmp(name, yv_vecs[i].name)) {
            if (!(yv_vecs[i].kinds & YV_KB(kind)))
                return yv_fail(err, cap, YGFX_ERR_ARG, "%s: not a parameter of this kind of stimulus", name);
            *a = yv_vecs[i];
            return 0;
        }
    for (i = 0; i < (int)(sizeof yv_aliases / sizeof yv_aliases[0]); i++)
        if (!strcmp(name, yv_aliases[i].name)) known = 1;
    if (kind == YGFX_SHAPE)
        for (i = 0; i < (int)(sizeof yv_aliases / sizeof yv_aliases[0]); i++)
            if (yv_aliases[i].shape == shape && !strcmp(name, yv_aliases[i].name)) {
                a->type = YV_A_ALIAS;
                a->index = yv_aliases[i].index;
                a->width = yv_aliases[i].width < 0 ? -yv_aliases[i].width : yv_aliases[i].width;
                a->fill = yv_aliases[i].width < 0;
                a->off = YV_OFF(shape_p) + (uint32_t)(sizeof(float) * (size_t)a->index);
                return 0;
            }
    t = ygfx_params(&n);
    for (i = 0; i < n; i++)
        if (!strcmp(name, t[i].name)) {
            if (!(t[i].kinds & YV_KB(kind)))
                return yv_fail(err, cap, YGFX_ERR_ARG, "%s: not a parameter of this kind of stimulus", name);
            a->type = YV_A_FLOAT;
            a->off = t[i].offset;
            a->width = 1;
            a->row = i;
            a->kinds = t[i].kinds;
            return 0;
        }
    t = ygfx_enum_params(&n);
    for (i = 0; i < n; i++)
        if (!strcmp(name, t[i].name)) {
            known = 1;
            if (!(t[i].kinds & YV_KB(kind))) continue;   /* aperture has a row for DOTS and one for the rest */
            a->type = YV_A_ENUM;
            a->row = i;
            a->kinds = t[i].kinds;
            return 0;
        }
    if (known) return yv_fail(err, cap, YGFX_ERR_ARG, "%s: not a parameter of this kind of stimulus", name);
    return yv_fail(err, cap, YGFX_ERR_ARG, "no parameter '%s' (params() lists them)", name);
}

/* Colors in a named space go through the gfx's ysp/color.h context. */
static int yv_space_rgb(yv_gfx* g, int space, int is_dir, const float in[3], float out[3], char* err, size_t cap) {
    const ycol_ctx* cx;
    ycol_gamut gm;
    ycol_rgb r;
    if (space == YCOL_SPACE_RGB) { out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; return 0; }
    cx = ygfx_color(&g->g);
    if (!cx) return yv_fail(err, cap, YGFX_ERR_ARG, "space: a color space other than rgb needs a calibration (the gfx option cal)");
    r = is_dir ? ycol_to_dir(cx, ycol_make(space, in[0], in[1], in[2]), &gm)
               : ycol_to_rgb(cx, ycol_make(space, in[0], in[1], in[2]), &gm);
    if (gm.status < 0) return yv_fail(err, cap, YGFX_ERR_ARG, "space: %s", gm.why ? gm.why : "refused");
    out[0] = (float)r.r;
    out[1] = (float)r.g;
    out[2] = (float)r.b;
    return 0;
}

static int yv_hex_rgb(yv_gfx* g, const char* hex, float out[3], char* err, size_t cap) {
    const ycol_ctx* cx = ygfx_color(&g->g);
    ycol_gamut gm;
    ycol_rgb r;
    ycol_color c = ycol_hex(hex);
    if (c.space == YCOL_SPACE_NONE) return yv_fail(err, cap, YGFX_ERR_ARG, "color: '%s' is not #rgb or #rrggbb", hex);
    if (!cx) return yv_fail(err, cap, YGFX_ERR_ARG, "color: a hex (sRGB) color needs a calibration (the gfx option cal)");
    r = ycol_to_rgb(cx, c, &gm);
    if (gm.status < 0) return yv_fail(err, cap, YGFX_ERR_ARG, "color: %s", gm.why ? gm.why : "refused");
    out[0] = (float)r.r;
    out[1] = (float)r.g;
    out[2] = (float)r.b;
    return 0;
}

static int yv_space_id(const char* s, int* out, char* err, size_t cap) {
    int sp = ycol_space_from_name(s);
    if (sp <= 0) return yv_fail(err, cap, YGFX_ERR_ARG, "space: '%s' is not a ysp/color.h space (rgb, xyy, dkl, cone, ...)", s);
    *out = sp;
    return 0;
}

/* Writes v (n values) to attribute a of st. Numbers only (enums by their
 * value); the name forms are in yv_set_kv(). space applies to color, dir. */
static int yv_attr_set(yv_stim* st, const yv_attr* a, const double* v, int n, int space, char* err, size_t cap) {
    float f[32];
    int i;
    switch (a->type) {
    case YV_A_FLOAT:
        if (n != 1 || !(v[0] == v[0])) return yv_fail(err, cap, YGFX_ERR_ARG, "%s: a number (not NaN)", a->name);
        *(float*)((char*)&st->s + a->off) = (float)v[0];
        return 0;
    case YV_A_VEC:
    case YV_A_ALIAS: {
        int minn = a->type == YV_A_VEC ? a->minn : a->width;
        int filled = a->fill && n == 1;
        if (!filled && (n < minn || n > a->width)) {
            if (minn == a->width)
                return yv_fail(err, cap, YGFX_ERR_ARG, "%s: %d number%s%s", a->name, a->width, a->width > 1 ? "s" : "",
                               a->fill ? " (or one for all)" : "");
            return yv_fail(err, cap, YGFX_ERR_ARG, "%s: %d to %d numbers", a->name, minn, a->width);
        }
        for (i = 0; i < a->width; i++) {
            double d = filled ? v[0] : i < n ? v[i] : 0.0;
            if (!(d == d)) return yv_fail(err, cap, YGFX_ERR_ARG, "%s: NaN refused", a->name);
            f[i] = (float)d;
        }
        if (a->off == YV_OFF(tint) && n == 3) f[3] = 1.0f;
    }
        if (space && (a->off == YV_OFF(color) || a->off == YV_OFF(dir))) {
            float o[3];
            if (n == 1) return yv_fail(err, cap, YGFX_ERR_ARG, "%s: three numbers in the space", a->name);
            if (yv_space_rgb(st->o.g, space, a->off == YV_OFF(dir), f, o, err, cap) < 0) return YGFX_ERR_ARG;
            memcpy(f, o, sizeof o);
        }
        memcpy((char*)&st->s + a->off, f, sizeof(float) * (size_t)a->width);
        return 0;
    case YV_A_ENUM: {
        double d;
        int rc;
        if (n != 1 || (d = v[0]) != floor(d)) return yv_fail(err, cap, YGFX_ERR_ARG, "%s: a name, or its integer value", a->name);
        rc = ygfx_enum_set(&st->s, a->row, (int)d);
        if (rc < 0) return yv_fail(err, cap, YGFX_ERR_ARG, "%s: %d is not one of its values for this stimulus", a->name, (int)d);
        return 0;
    }
    default:
        return yv_fail(err, cap, YGFX_ERR_ARG, "%s: not set by number here", a->name);
    }
}

/* Reads attribute a: up to 32 floats into out (count in *n), or an enum's
 * value. */
static int yv_attr_get(const yv_stim* st, const yv_attr* a, double* out, int* n, char* err, size_t cap) {
    int i;
    switch (a->type) {
    case YV_A_FLOAT:
        out[0] = *(const float*)((const char*)&st->s + a->off);
        *n = 1;
        return 0;
    case YV_A_VEC:
    case YV_A_ALIAS:
        for (i = 0; i < a->width; i++) out[i] = ((const float*)((const char*)&st->s + a->off))[i];
        *n = a->width;
        return 0;
    case YV_A_ENUM: {
        int v = ygfx_enum_get(&st->s, a->row);
        if (v < 0) return yv_fail(err, cap, YGFX_ERR_ARG, "%s: no value", a->name);
        out[0] = v;
        *n = 1;
        return 0;
    }
    default:
        return yv_fail(err, cap, YGFX_ERR_ARG, "%s: not read as numbers", a->name);
    }
}

/* ---- users: who points at whom ---------------------------------------------------- */

static void yv_set_group(yv_stim* st, yv_group* gr) {
    if (st->group) st->group->o.users--;
    st->group = gr;
    st->s.group = gr ? &gr->grp : NULL;
    if (gr) gr->o.users++;
}

static void yv_set_tex(yv_stim* st, yv_tex* t) {
    if (st->tex) st->tex->o.users--;
    st->tex = t;
    st->s.tex.id = t ? t->t.id : 0u;
    if (t) t->o.users++;
}

/* ---- setting one parameter by its host argument ------------------------------------ */

static int yv_dots_positions(yv_stim* st, const double* v, int n, char* err, size_t cap);

/* One argument to a stimulus: every form a host passes (numbers, a name,
 * an object). space: a ysp/color.h space for color and dir, 0 = rgb. */
static int yv_set_kv(yv_stim* st, yv_kv* k, int space, char* err, size_t cap) {
    yv_attr a;
    int kind = st->s.kind, rc;
    if (yv_attr_find(k->name, kind, st->s.shape, &a, err, cap) < 0) return YGFX_ERR_ARG;
    if (a.type == YV_A_SPECIAL) {
        if (!strcmp(k->name, "space")) return 0;   /* read by the caller */
        if (!strcmp(k->name, "anchor")) {
            int e;
            if (k->s) {
                if (yv_align(k, &e, err, cap) < 0) return YGFX_ERR_ARG;
                st->s.ax = yv_align_fx[e];
                st->s.ay = yv_align_fy[e];
                return 0;
            }
            {
                float f[2];
                if (yv_floats(k, f, 2, 0, err, cap) < 0) return yv_fail(err, cap, YGFX_ERR_ARG, "anchor: a name or (ax, ay)");
                st->s.ax = f[0];
                st->s.ay = f[1];
                return 0;
            }
        }
        if (!strcmp(k->name, "group")) {
            if (k->obj && k->obj->type != YV_O_GROUP) return yv_fail(err, cap, YGFX_ERR_ARG, "group: a Group of this gfx, or none");
            if (k->obj && k->obj->g != st->o.g) return yv_fail(err, cap, YGFX_ERR_ARG, "group: of another gfx");
            if (!k->obj && (k->s || k->n > 1 || (k->n == 1 && k->v[0] != 0)))
                return yv_fail(err, cap, YGFX_ERR_ARG, "group: a Group of this gfx, or none");
            yv_set_group(st, (yv_group*)k->obj);
            return 0;
        }
        if (!strcmp(k->name, "tex")) {
            if (kind != YGFX_IMAGE) return yv_fail(err, cap, YGFX_ERR_ARG, "tex: images only");
            if (!k->obj || k->obj->type != YV_O_TEX || k->obj->g != st->o.g)
                return yv_fail(err, cap, YGFX_ERR_ARG, "tex: a Texture of this gfx");
            yv_set_tex(st, (yv_tex*)k->obj);
            return 0;
        }
        if (!strcmp(k->name, "vertices")) {
            int i, m;
            if (kind != YGFX_SHAPE || st->s.shape != YGFX_POLYGON) return yv_fail(err, cap, YGFX_ERR_ARG, "vertices: polygons only");
            if (k->s || k->obj || k->n % 2 || k->n < 6 || k->n > 32)
                return yv_fail(err, cap, YGFX_ERR_ARG, "vertices: 3 to 16 (x, y) pairs from the box center");
            m = k->n / 2;
            for (i = 0; i < 32; i++) st->s.p[i] = i < k->n ? (float)k->v[i] : 0.0f;
            st->s.shape_p[0] = (float)m;
            return 0;
        }
        if (!strcmp(k->name, "path")) {
            float* p;
            int i, m;
            if (kind != YGFX_SHAPE || (st->s.shape != YGFX_POLYLINE && st->s.shape != YGFX_QBEZIER))
                return yv_fail(err, cap, YGFX_ERR_ARG, "path: polyline and qbezier only");
            m = k->n / 2;
            if (k->s || k->obj || k->n % 2 || m < 2 || m > YGFX_MAX_PATH || (st->s.shape == YGFX_QBEZIER && m != 3))
                return yv_fail(err, cap, YGFX_ERR_ARG, "path: (x, y) pairs from the box center: 2 to %d (qbezier: 3)", YGFX_MAX_PATH);
            if (m * 2 > (int)st->s.n_path * 2 || !st->path) {
                p = (float*)realloc(st->path, sizeof(float) * (size_t)(2 * m));
                if (!p) return yv_fail(err, cap, YGFX_ERR_FULL, "out of memory");
                st->path = p;
            }
            for (i = 0; i < 2 * m; i++) st->path[i] = (float)k->v[i];
            st->s.path = st->path;
            st->s.n_path = (uint32_t)m;
            return 0;
        }
        if (!strcmp(k->name, "positions")) {
            if (st->vkind != YV_K_DOTS) return yv_fail(err, cap, YGFX_ERR_ARG, "positions: dot fields only");
            return yv_dots_positions(st, k->v, k->s || k->obj ? -1 : k->n, err, cap);
        }
        if (!strcmp(k->name, "count")) {
            int c;
            if (st->vkind != YV_K_DOTS) return yv_fail(err, cap, YGFX_ERR_ARG, "count: dot fields only");
            if (yv_int(k, 0, (int)st->dots_cap, &c, err, cap) < 0) return YGFX_ERR_ARG;
            st->s.count = (uint32_t)c;
            return 0;
        }
        return yv_fail(err, cap, YGFX_ERR_ARG, "%s: not settable here", k->name);
    }
    if (k->obj) return yv_fail(err, cap, YGFX_ERR_ARG, "%s: numbers, not an object", k->name);
    if (k->s) {
        if (a.type == YV_A_ENUM) {
            const ygfx_param* t = ygfx_enum_params(NULL);
            int v;
            if (!t[a.row].values || yv_enum_value(t[a.row].values, k->s, &v) < 0)
                return yv_fail(err, cap, YGFX_ERR_ARG, "%s: '%s' is not one of %s", k->name, k->s,
                               t[a.row].values ? t[a.row].values : "its values (a number)");
            rc = ygfx_enum_set(&st->s, a.row, v);
            if (rc < 0) return yv_fail(err, cap, YGFX_ERR_ARG, "%s: '%s' is refused for this stimulus", k->name, k->s);
            return 0;
        }
        if (a.type == YV_A_VEC && a.off == YV_OFF(color)) return yv_hex_rgb(st->o.g, k->s, st->s.color, err, cap);
        return yv_fail(err, cap, YGFX_ERR_ARG, "%s: numbers, not a name", k->name);
    }
    if (a.type == YV_A_ENUM && k->n == 1 && (k->v[0] == 0.0 || k->v[0] == 1.0)) {
        const ygfx_param* t = ygfx_enum_params(NULL);
        if (!strcmp(t[a.row].type, "bool")) {
            rc = ygfx_enum_set(&st->s, a.row, (int)k->v[0]);
            return rc < 0 ? yv_fail(err, cap, YGFX_ERR_ARG, "%s: refused", k->name) : 0;
        }
    }
    return yv_attr_set(st, &a, k->v, k->n, space, err, cap);
}

/* Applies every argument not used yet. */
static int yv_set_rest(yv_stim* st, yv_kv* kv, int n, char* err, size_t cap) {
    int i, space = 0;
    yv_kv* sp = yv_take(kv, n, "space");
    if (sp) {
        if (!sp->s) return yv_fail(err, cap, YGFX_ERR_ARG, "space: a name");
        if (yv_space_id(sp->s, &space, err, cap) < 0) return YGFX_ERR_ARG;
    }
    for (i = 0; i < n; i++) {
        if (kv[i].used) continue;
        kv[i].used = 1;
        if (yv_set_kv(st, &kv[i], space, err, cap) < 0) return YGFX_ERR_ARG;
    }
    return 0;
}

/* ---- making stimuli ------------------------------------------------------------------ */

static yv_stim* yv_stim_alloc(yv_gfx* g, int vkind) {
    yv_stim* st = (yv_stim*)calloc(1, sizeof *st);
    if (!st) return NULL;
    st->vkind = vkind;
    yv_link(g, &st->o, YV_O_STIM);
    return st;
}

static void yv_unbind_obj(yv_gfx* g, yv_obj* o);

static void yv_stim_free(yv_stim* st) {
    if (!st) return;
    if (st->o.g) {
        if (st->bound) yv_unbind_obj(st->o.g, &st->o);
        if (st->vkind == YV_K_DOTS && st->s.buf.id) yv_free_buf_id(st->o.g, st->s.buf);
    }
    yv_set_group(st, NULL);
    yv_set_tex(st, NULL);
    yv_unlink(&st->o);
    free(st->path);
    if (st->inst_owned) free(st->inst);
    free(st);
}

/* The shape kinds a host names (ygfx_enum_params' shape row). */
static int yv_shape_kind(const char* s, int* out, char* err, size_t cap) {
    const ygfx_param* t = ygfx_enum_params(NULL);
    if (!s || yv_enum_value(t[YGFX_E_SHAPE].values, s, out) < 0 || *out == YGFX_MASK_TEX || *out == YGFX_COMPOUND ||
        *out == YGFX_NO_APERTURE)
        return yv_fail(err, cap, YGFX_ERR_ARG,
                       "shape: rect, circle, annulus, line, polygon, cross, rrect, arc, pie, capsule, ngon, star, "
                       "ellipse, qbezier or polyline");
    return 0;
}

/* place, anchor, x, y, w, h, ori: the box every desc has. */
typedef struct yv_box { int place, anchor; float x, y, w, h, ori; } yv_box;

static int yv_box_take(yv_kv* kv, int n, yv_box* b, char* err, size_t cap) {
    yv_kv* k;
    double d;
    float* f[5];
    static const char* names[5] = { "x", "y", "w", "h", "ori" };
    int i;
    memset(b, 0, sizeof *b);
    f[0] = &b->x; f[1] = &b->y; f[2] = &b->w; f[3] = &b->h; f[4] = &b->ori;
    if ((k = yv_take(kv, n, "place")) && yv_align(k, &b->place, err, cap) < 0) return YGFX_ERR_ARG;
    if ((k = yv_take(kv, n, "anchor"))) {
        if (k->s) { if (yv_align(k, &b->anchor, err, cap) < 0) return YGFX_ERR_ARG; }
        else k->used = 0;   /* (ax, ay): set after the constructor */
    }
    for (i = 0; i < 5; i++)
        if ((k = yv_take(kv, n, names[i]))) {
            if (yv_num(k, &d, err, cap) < 0) return YGFX_ERR_ARG;
            *f[i] = (float)d;
        }
    return 0;
}

static int yv_take_num(yv_kv* kv, int n, const char* name, float* out, char* err, size_t cap) {
    yv_kv* k = yv_take(kv, n, name);
    double d;
    if (!k) return 0;
    if (yv_num(k, &d, err, cap) < 0) return YGFX_ERR_ARG;
    *out = (float)d;
    return 1;
}

/* An aperture name for GRATING, NOISE (dots have their own). */
static int yv_take_aperture(yv_kv* kv, int n, int dots, int* out, char* err, size_t cap) {
    yv_kv* k = yv_take(kv, n, "aperture");
    const ygfx_param* t = ygfx_enum_params(NULL);
    const char* vals = t[dots ? YGFX_E_DOTS_APERTURE : YGFX_E_APERTURE].values;
    if (!k) return 0;
    if (!k->s || yv_enum_value(vals, k->s, out) < 0 || *out == YGFX_MASK_TEX)
        return yv_fail(err, cap, YGFX_ERR_ARG, "aperture: one of %s (mask_tex is not bound)", vals);
    return 1;
}

/* Shape parameters through the desc (the constructor reads some: a star's
 * inner ratio, a capsule's box). */
static int yv_take_shape_p(yv_kv* kv, int n, int shape, float p[4], char* err, size_t cap) {
    yv_kv* k;
    int i;
    if ((k = yv_take(kv, n, "shape_p"))) {
        if (k->s || k->obj || k->n < 1 || k->n > 4) return yv_fail(err, cap, YGFX_ERR_ARG, "shape_p: 1 to 4 numbers");
        for (i = 0; i < k->n; i++) p[i] = (float)k->v[i];
    }
    for (i = 0; i < (int)(sizeof yv_aliases / sizeof yv_aliases[0]); i++) {
        const yv_alias* al = &yv_aliases[i];
        int w = al->width < 0 ? -al->width : al->width, j;
        if (al->shape != shape || !(k = yv_take(kv, n, al->name))) continue;
        if (k->s || k->obj || !(k->n == w || (al->width < 0 && k->n == 1)))
            return yv_fail(err, cap, YGFX_ERR_ARG, "%s: %d number%s", al->name, w, w > 1 ? "s" : "");
        for (j = 0; j < w; j++) p[al->index + j] = (float)k->v[k->n == 1 ? 0 : j];
    }
    return 0;
}

static int yv_dots_positions(yv_stim* st, const double* v, int n, char* err, size_t cap) {
    yv_gfx* g = st->o.g;
    float buf[512];
    int i, m, done = 0, rc;
    if (n < 0 || n % 2) return yv_fail(err, cap, YGFX_ERR_ARG, "positions: (x, y) pairs, units from the field's center");
    m = n / 2;
    if ((uint32_t)m > st->dots_cap) return yv_fail(err, cap, YGFX_ERR_ARG, "positions: %d dots; the field holds %u", m, st->dots_cap);
    if (g->in_frame) return yv_fail(err, cap, YGFX_ERR_ORDER, "positions: between frames (a queued draw reads the buffer at end())");
    /* f64 to f32 in blocks: nothing allocated per update */
    while (done < n) {
        int c = n - done < 512 ? n - done : 512;
        for (i = 0; i < c; i++) {
            if (!(v[done + i] == v[done + i])) return yv_fail(err, cap, YGFX_ERR_ARG, "positions: NaN refused");
            buf[i] = (float)v[done + i];
        }
        rc = ygfx_buffer_update(&g->g, st->s.buf, sizeof(float) * (size_t)done, buf, sizeof(float) * (size_t)c);
        if (rc < 0) return yv_gfx_err(g, rc, err, cap);
        done += c;
    }
    st->s.count = (uint32_t)m;
    return 0;
}

/* A stimulus of vkind (not INSTANCES) from the host's arguments: the desc
 * takes what its constructor reads, then the rest is set by name. NULL with
 * err. */
static yv_stim* yv_stim_make(yv_gfx* g, int vkind, yv_kv* kv, int n, int* code, char* err, size_t cap) {
    yv_stim* st;
    yv_box b;
    yv_kv* k;
    int rc = 0;
    *code = YGFX_ERR_ARG;
    if (!g->open) { *code = yv_fail(err, cap, YGFX_ERR_CLOSED, "ysp_gfx: the gfx (or its screen) is closed"); return NULL; }
    if (yv_box_take(kv, n, &b, err, cap) < 0) return NULL;
    st = yv_stim_alloc(g, vkind);
    if (!st) { *code = yv_fail(err, cap, YGFX_ERR_FULL, "out of memory"); return NULL; }
    switch (vkind) {
    case YV_K_SHAPE: {
        ygfx_shape_desc d;
        int shape = YGFX_RECT;
        memset(&d, 0, sizeof d);
        if ((k = yv_take(kv, n, "shape")) && yv_shape_kind(k->s, &shape, err, cap) < 0) goto fail;
        d.shape = (ygfx_shape_kind)shape;
        d.place = (ygfx_align)b.place; d.anchor = (ygfx_align)b.anchor;
        d.x = b.x; d.y = b.y; d.w = b.w; d.h = b.h; d.ori = b.ori;
        if (yv_take_shape_p(kv, n, shape, d.shape_p, err, cap) < 0) goto fail;
        if ((k = yv_take(kv, n, "vertices"))) {
            float vtx[32];
            int i;
            if (shape != YGFX_POLYGON) { rc = yv_fail(err, cap, YGFX_ERR_ARG, "vertices: polygons only"); goto fail; }
            if (k->s || k->obj || k->n % 2 || k->n < 6 || k->n > 32) {
                rc = yv_fail(err, cap, YGFX_ERR_ARG, "vertices: 3 to 16 (x, y) pairs from the box center");
                goto fail;
            }
            for (i = 0; i < k->n; i++) vtx[i] = (float)k->v[i];
            d.shape_p[0] = (float)(k->n / 2);
            d.vertices = vtx;
            st->s = ygfx_shape(&d);
        } else {
            if (shape == YGFX_POLYGON) { rc = yv_fail(err, cap, YGFX_ERR_ARG, "polygon: vertices are required"); goto fail; }
            st->s = ygfx_shape(&d);
        }
        if (shape == YGFX_POLYLINE || shape == YGFX_QBEZIER) {
            k = yv_take(kv, n, "path");
            if (!k) {
                rc = yv_fail(err, cap, YGFX_ERR_ARG, "%s: path is required", shape == YGFX_POLYLINE ? "polyline" : "qbezier");
                goto fail;
            }
            if (yv_set_kv(st, k, 0, err, cap) < 0) goto fail;
        }
        break;
    }
    case YV_K_GRATING: {
        ygfx_grating_desc d;
        int ap = YGFX_RECT;
        memset(&d, 0, sizeof d);
        d.place = (ygfx_align)b.place; d.anchor = (ygfx_align)b.anchor;
        d.x = b.x; d.y = b.y; d.w = b.w; d.h = b.h; d.ori = b.ori;
        if (yv_take_aperture(kv, n, 0, &ap, err, cap) < 0) goto fail;
        d.aperture = (ygfx_shape_kind)ap;
        if (yv_take_num(kv, n, "sf", &d.sf, err, cap) < 0 || yv_take_num(kv, n, "phase", &d.phase, err, cap) < 0) goto fail;
        if (yv_take_shape_p(kv, n, -1, d.shape_p, err, cap) < 0) goto fail;
        st->s = ygfx_grating(&d);
        break;
    }
    case YV_K_GABOR: {
        ygfx_gabor_desc d;
        memset(&d, 0, sizeof d);
        d.place = (ygfx_align)b.place; d.anchor = (ygfx_align)b.anchor;
        d.x = b.x; d.y = b.y; d.ori = b.ori;
        if (b.w != 0 || b.h != 0) { rc = yv_fail(err, cap, YGFX_ERR_ARG, "gabor: size, not w and h (0 = 8 sigma)"); goto fail; }
        if (yv_take_num(kv, n, "sf", &d.sf, err, cap) < 0 || yv_take_num(kv, n, "phase", &d.phase, err, cap) < 0 ||
            yv_take_num(kv, n, "sigma", &d.sigma, err, cap) < 0 || yv_take_num(kv, n, "size", &d.size, err, cap) < 0)
            goto fail;
        st->s = ygfx_gabor(&d);
        break;
    }
    case YV_K_NOISE: {
        ygfx_noise_desc d;
        int ap = YGFX_RECT, dist = 0;
        memset(&d, 0, sizeof d);
        d.place = (ygfx_align)b.place; d.anchor = (ygfx_align)b.anchor;
        d.x = b.x; d.y = b.y; d.w = b.w; d.h = b.h; d.ori = b.ori;
        if (yv_take_aperture(kv, n, 0, &ap, err, cap) < 0) goto fail;
        d.aperture = (ygfx_shape_kind)ap;
        if ((k = yv_take(kv, n, "dist"))) {
            const ygfx_param* t = ygfx_enum_params(NULL);
            if (!k->s || yv_enum_value(t[YGFX_E_DIST].values, k->s, &dist) < 0) {
                rc = yv_fail(err, cap, YGFX_ERR_ARG, "dist: uniform, binary, gaussian or simplex");
                goto fail;
            }
        }
        d.dist = (ygfx_noise_dist)dist;
        if (yv_take_num(kv, n, "scale", &d.scale, err, cap) < 0 || yv_take_num(kv, n, "z", &d.z, err, cap) < 0) goto fail;
        if (yv_take_shape_p(kv, n, -1, d.shape_p, err, cap) < 0) goto fail;
        st->s = ygfx_noise(&d);
        break;
    }
    case YV_K_IMAGE: {
        ygfx_image_desc d;
        yv_tex* tx;
        memset(&d, 0, sizeof d);
        d.place = (ygfx_align)b.place; d.anchor = (ygfx_align)b.anchor;
        d.x = b.x; d.y = b.y; d.w = b.w; d.h = b.h; d.ori = b.ori;
        k = yv_take(kv, n, "tex");
        if (!k || !k->obj || k->obj->type != YV_O_TEX || k->obj->g != g || !((yv_tex*)k->obj)->t.id) {
            rc = yv_fail(err, cap, YGFX_ERR_ARG, "image: tex, a live Texture of this gfx, is required");
            goto fail;
        }
        tx = (yv_tex*)k->obj;
        d.tex = tx->t;
        if ((k = yv_take(kv, n, "src"))) {
            if (yv_floats(k, d.src, 4, 0, err, cap) < 0) goto fail;
        }
        st->s = ygfx_image(&g->g, &d);
        st->s.tex.id = 0;
        yv_set_tex(st, tx);
        break;
    }
    case YV_K_DOTS: {
        ygfx_dots_desc d;
        int ap = YGFX_NO_APERTURE, count = 0;
        yv_kv* pos = yv_take(kv, n, "positions");
        memset(&d, 0, sizeof d);
        d.place = (ygfx_align)b.place; d.anchor = (ygfx_align)b.anchor;
        d.x = b.x; d.y = b.y; d.w = b.w; d.h = b.h; d.ori = b.ori;
        if (yv_take_aperture(kv, n, 1, &ap, err, cap) < 0) goto fail;
        d.aperture = (ygfx_shape_kind)ap;
        if ((k = yv_take(kv, n, "n")) && yv_int(k, 1, 1 << 24, &count, err, cap) < 0) goto fail;
        if (pos && (pos->s || pos->obj || pos->n % 2 || pos->n < 2)) {
            rc = yv_fail(err, cap, YGFX_ERR_ARG, "positions: (x, y) pairs, units from the field's center");
            goto fail;
        }
        if (!count) count = pos ? pos->n / 2 : 0;
        if (count < 1) { rc = yv_fail(err, cap, YGFX_ERR_ARG, "dots: n (the most dots) or positions is required"); goto fail; }
        if (pos && pos->n / 2 > count) { rc = yv_fail(err, cap, YGFX_ERR_ARG, "positions: more than n dots"); goto fail; }
        if (g->in_frame) { rc = yv_fail(err, cap, YGFX_ERR_ORDER, "dots: make it between frames"); goto fail; }
        d.buf = ygfx_buffer(&g->g, sizeof(float) * 2 * (size_t)count);
        if (!d.buf.id) { *code = yv_gfx_err(g, YGFX_ERR_FULL, err, cap); goto fail_code; }
        st->s = ygfx_dots(&d);
        st->dots_cap = (uint32_t)count;
        st->s.count = 0;
        if (pos && yv_dots_positions(st, pos->v, pos->n, err, cap) < 0) goto fail;
        break;
    }
    default:
        rc = yv_fail(err, cap, YGFX_ERR_ARG, "no such kind");
        goto fail;
    }
    if (yv_set_rest(st, kv, n, err, cap) < 0) goto fail;
    return st;
fail:
    *code = YGFX_ERR_ARG;
    (void)rc;
fail_code:
    yv_stim_free(st);
    return NULL;
}

/* A copy: its own path and elements, the same group and texture. */
static yv_stim* yv_stim_copy(const yv_stim* src, int* code, char* err, size_t cap) {
    yv_gfx* g = src->o.g;
    yv_stim* st;
    *code = YGFX_ERR_ARG;
    if (yv_live(&src->o, err, cap) < 0) { *code = YGFX_ERR_CLOSED; return NULL; }
    if (src->vkind == YV_K_DOTS || src->vkind == YV_K_INSTANCES) {
        yv_fail(err, cap, YGFX_ERR_ARG, "copy: not of dot fields and instances (they own GPU or element memory)");
        return NULL;
    }
    st = yv_stim_alloc(g, src->vkind);
    if (!st) { *code = yv_fail(err, cap, YGFX_ERR_FULL, "out of memory"); return NULL; }
    st->s = src->s;
    st->s.group = NULL;
    st->s.tex.id = 0;
    if (src->path) {
        st->path = (float*)malloc(sizeof(float) * 2 * src->s.n_path);
        if (!st->path) { yv_stim_free(st); *code = yv_fail(err, cap, YGFX_ERR_FULL, "out of memory"); return NULL; }
        memcpy(st->path, src->path, sizeof(float) * 2 * src->s.n_path);
        st->s.path = st->path;
    }
    yv_set_group(st, src->group);
    yv_set_tex(st, src->tex);
    return st;
}

/* ---- instances ---------------------------------------------------------------------------- */

typedef struct yv_ifield { const char* name; int off; uint32_t bit; } yv_ifield;
static const yv_ifield yv_ifields[] = {
    { "x", 0, YGFX_I_XY }, { "y", 1, YGFX_I_XY }, { "ori", 2, YGFX_I_ORI }, { "phase", 3, YGFX_I_PHASE },
    { "contrast", 4, YGFX_I_CONTRAST }, { "scale", 5, YGFX_I_SCALE }, { "gate", 6, YGFX_I_GATE }, { "color", 7, YGFX_I_COLOR },
};

static int yv_ifield_find(const char* name) {
    int i;
    for (i = 0; i < 8; i++) if (!strcmp(name, yv_ifields[i].name)) return i;
    return -1;
}

/* "xy, ori, gate" (commas or spaces) to YGFX_I_* bits. */
static int yv_ifields_parse(const char* s, uint32_t* out, char* err, size_t cap) {
    char w[16];
    uint32_t m = 0;
    while (s && *s) {
        size_t n = 0;
        int i, found = 0;
        while (*s == ',' || *s == ' ') s++;
        while (*s && *s != ',' && *s != ' ' && n + 1 < sizeof w) w[n++] = *s++;
        w[n] = 0;
        if (!n) break;
        if (!strcmp(w, "xy")) { m |= YGFX_I_XY; continue; }
        for (i = 0; i < 8; i++)
            if (!strcmp(w, yv_ifields[i].name) && i > 1) { m |= yv_ifields[i].bit; found = 1; }
        if (!found)
            return yv_fail(err, cap, YGFX_ERR_ARG, "fields: '%s' is not xy, ori, phase, contrast, scale, gate or color", w);
    }
    *out = m ? m | YGFX_I_XY : YGFX_I_XY;
    return 0;
}

/* n elements of tmpl in one draw. mem: the host's element memory (n x 32
 * bytes, kept alive by the host as long as the stimulus), or NULL for the
 * core's own. Elements start at x, y 0, contrast, scale, gate 1. */
static yv_stim* yv_inst_make(yv_stim* tmpl, int n, const char* fields, const double* palette, int n_pal, ygfx_inst* mem,
                             int* code, char* err, size_t cap) {
    yv_gfx* g = tmpl->o.g;
    yv_stim* st;
    ygfx_instances_desc d;
    ygfx_stim t;
    uint32_t bits;
    int i;
    *code = YGFX_ERR_ARG;
    if (yv_live(&tmpl->o, err, cap) < 0) { *code = YGFX_ERR_CLOSED; return NULL; }
    if (tmpl->vkind == YV_K_DOTS || tmpl->vkind == YV_K_INSTANCES) {
        yv_fail(err, cap, YGFX_ERR_ARG, "instances: the template is a shape, grating, gabor, noise or image");
        return NULL;
    }
    if (n < 1 || n > (1 << 22)) { yv_fail(err, cap, YGFX_ERR_ARG, "instances: n, 1 or more"); return NULL; }
    if (n_pal < 0 || n_pal > YGFX_MAX_PALETTE) { yv_fail(err, cap, YGFX_ERR_ARG, "palette: 1 to %d (r, g, b) entries", YGFX_MAX_PALETTE); return NULL; }
    if (yv_ifields_parse(fields, &bits, err, cap) < 0) return NULL;
    if ((bits & YGFX_I_COLOR) && n_pal < 1) { yv_fail(err, cap, YGFX_ERR_ARG, "fields: color needs a palette"); return NULL; }
    if (g->in_frame) { *code = yv_fail(err, cap, YGFX_ERR_ORDER, "instances: make them between frames"); return NULL; }
    st = yv_stim_alloc(g, YV_K_INSTANCES);
    if (!st) { *code = yv_fail(err, cap, YGFX_ERR_FULL, "out of memory"); return NULL; }
    if (mem) st->inst = mem;
    else {
        st->inst = (ygfx_inst*)calloc((size_t)n, sizeof(ygfx_inst));
        st->inst_owned = 1;
        if (!st->inst) { yv_stim_free(st); *code = yv_fail(err, cap, YGFX_ERR_FULL, "out of memory"); return NULL; }
    }
    for (i = 0; i < n; i++) {
        memset(&st->inst[i], 0, sizeof st->inst[i]);
        st->inst[i].contrast = st->inst[i].scale = st->inst[i].gate = 1.0f;
    }
    for (i = 0; i < 3 * n_pal; i++) st->palette[i] = (float)palette[i];
    if (tmpl->path) {
        st->path = (float*)malloc(sizeof(float) * 2 * tmpl->s.n_path);
        if (!st->path) { yv_stim_free(st); *code = yv_fail(err, cap, YGFX_ERR_FULL, "out of memory"); return NULL; }
        memcpy(st->path, tmpl->path, sizeof(float) * 2 * tmpl->s.n_path);
    }
    t = tmpl->s;
    t.path = st->path;
    memset(&d, 0, sizeof d);
    d.inst = st->inst;
    d.n = n;
    d.fields = bits;
    d.palette = n_pal ? st->palette : NULL;
    d.n_palette = n_pal;
    st->s = ygfx_instances(&g->g, &t, &d);
    if (st->s.n_inst == 0) {
        *code = yv_gfx_err(g, YGFX_ERR_ARG, err, cap);
        yv_stim_free(st);
        return NULL;
    }
    st->s.group = NULL;
    st->s.tex.id = 0;
    yv_set_group(st, tmpl->group);
    yv_set_tex(st, tmpl->tex);
    return st;
}

/* Element field `name` (or xy, 2 per element) from v: count values for the
 * elements from `first`, or one value for all. */
static int yv_inst_set(yv_stim* st, const char* name, int first, const double* v, int count, char* err, size_t cap) {
    int i, f, w = 1, n;
    if (yv_live(&st->o, err, cap) < 0) return YGFX_ERR_CLOSED;
    if (st->vkind != YV_K_INSTANCES) return yv_fail(err, cap, YGFX_ERR_ARG, "elements: instances only");
    n = (int)st->s.n_inst;
    if (!strcmp(name, "xy")) { f = 0; w = 2; }
    else if ((f = yv_ifield_find(name)) < 0)
        return yv_fail(err, cap, YGFX_ERR_ARG, "elements: '%s' is not x, y, xy, ori, phase, contrast, scale, gate or color", name);
    if (!(st->s.inst_fields & yv_ifields[f].bit))
        return yv_fail(err, cap, YGFX_ERR_ARG, "elements: %s is not in the instances' fields, so the draw does not read it", name);
    if (first < 0 || first >= n) return yv_fail(err, cap, YGFX_ERR_ARG, "elements: first is 0 to %d", n - 1);
    if (count == w) {   /* one value (pair) for every element from first */
        for (i = first; i < n; i++) {
            float* e = (float*)&st->inst[i];
            e[f] = (float)v[0];
            if (w == 2) e[f + 1] = (float)v[1];
        }
        return 0;
    }
    if (count % w || first + count / w > n)
        return yv_fail(err, cap, YGFX_ERR_ARG, "elements: %d values for %d elements from %d (%d per element)", count, n - first, first, w);
    for (i = 0; i < count / w; i++) {
        float* e = (float*)&st->inst[first + i];
        if (!(v[i * w] == v[i * w])) return yv_fail(err, cap, YGFX_ERR_ARG, "elements: NaN refused");
        e[f] = (float)v[i * w];
        if (w == 2) e[f + 1] = (float)v[i * w + 1];
    }
    return 0;
}

/* ---- groups and textures ----------------------------------------------------------------- */

static yv_group* yv_group_make(yv_gfx* g, yv_kv* kv, int n, int* code, char* err, size_t cap);
static int yv_group_set(yv_group* gr, yv_kv* k, char* err, size_t cap) {
    const ygfx_param* t;
    int m, i;
    if (!strcmp(k->name, "place") || !strcmp(k->name, "anchor")) {
        int e;
        if (!k->s && !strcmp(k->name, "anchor")) {
            float f[2];
            if (yv_floats(k, f, 2, 0, err, cap) < 0) return YGFX_ERR_ARG;
            gr->grp.ax = f[0];
            gr->grp.ay = f[1];
            return 0;
        }
        if (yv_align(k, &e, err, cap) < 0) return YGFX_ERR_ARG;
        if (k->name[0] == 'p') gr->grp.place = (uint8_t)e;
        else { gr->grp.ax = yv_align_fx[e]; gr->grp.ay = yv_align_fy[e]; }
        return 0;
    }
    t = ygfx_group_params(&m);
    for (i = 0; i < m; i++)
        if (!strcmp(k->name, t[i].name)) {
            double d;
            if (yv_num(k, &d, err, cap) < 0) return YGFX_ERR_ARG;
            *(float*)((char*)&gr->grp + t[i].offset) = (float)d;
            return 0;
        }
    return yv_fail(err, cap, YGFX_ERR_ARG, "no group parameter '%s' (place, anchor, x, y, w, h, ori, scale, opacity, visible)", k->name);
}

static yv_group* yv_group_make(yv_gfx* g, yv_kv* kv, int n, int* code, char* err, size_t cap) {
    yv_group* gr;
    ygfx_group_desc d;
    int i;
    *code = YGFX_ERR_ARG;
    if (!g->open) { *code = yv_fail(err, cap, YGFX_ERR_CLOSED, "ysp_gfx: the gfx (or its screen) is closed"); return NULL; }
    gr = (yv_group*)calloc(1, sizeof *gr);
    if (!gr) { *code = yv_fail(err, cap, YGFX_ERR_FULL, "out of memory"); return NULL; }
    memset(&d, 0, sizeof d);
    gr->grp = ygfx_group_make(&d);
    yv_link(g, &gr->o, YV_O_GROUP);
    for (i = 0; i < n; i++) {
        if (kv[i].used) continue;
        kv[i].used = 1;
        if (yv_group_set(gr, &kv[i], err, cap) < 0) {
            yv_unlink(&gr->o);
            free(gr);
            return NULL;
        }
    }
    return gr;
}

static int yv_group_get(const yv_group* gr, const char* name, double* out, char* err, size_t cap) {
    const ygfx_param* t;
    int m, i;
    t = ygfx_group_params(&m);
    for (i = 0; i < m; i++)
        if (!strcmp(name, t[i].name)) { *out = *(const float*)((const char*)&gr->grp + t[i].offset); return 0; }
    return yv_fail(err, cap, YGFX_ERR_ARG, "no group parameter '%s'", name);
}

static int yv_format_bytes(int f) {
    switch (f) {
    case YGFX_R8: return 1;
    case YGFX_RG8: return 2;
    case YGFX_RGBA8: return 4;
    case YGFX_R16F: case YGFX_R32F: return 4;
    case YGFX_RGBA16F: case YGFX_RGBA32F: return 16;
    case YGFX_R16UI: return 2;
    default: return 0;
    }
}

/* data: rows top first, tight (the host converts its arrays). srgb: 8-bit
 * values are sRGB codes, drawn in linear light (VIDEO, an encoded texture). */
static yv_tex* yv_tex_make(yv_gfx* g, int w, int h, int format, const void* data, int linear, int srgb, int* code,
                           char* err, size_t cap) {
    yv_tex* t;
    ygfx_texture_desc d;
    *code = YGFX_ERR_ARG;
    if (!g->open) { *code = yv_fail(err, cap, YGFX_ERR_CLOSED, "ysp_gfx: the gfx (or its screen) is closed"); return NULL; }
    if (g->in_frame) { *code = yv_fail(err, cap, YGFX_ERR_ORDER, "texture: make it between frames"); return NULL; }
    if (w < 1 || h < 1) { yv_fail(err, cap, YGFX_ERR_ARG, "texture: w and h, 1 or more"); return NULL; }
    if (!yv_format_bytes(format)) { yv_fail(err, cap, YGFX_ERR_ARG, "texture: no such format"); return NULL; }
    if (srgb && format != YGFX_RGBA8 && format != YGFX_R8 && format != YGFX_RG8) {
        yv_fail(err, cap, YGFX_ERR_ARG, "texture: srgb is for 8-bit textures (r8, rg8, rgba8)");
        return NULL;
    }
    t = (yv_tex*)calloc(1, sizeof *t);
    if (!t) { *code = yv_fail(err, cap, YGFX_ERR_FULL, "out of memory"); return NULL; }
    memset(&d, 0, sizeof d);
    d.w = w; d.h = h; d.format = (ygfx_format)format; d.linear = linear != 0; d.data = data;
    if (srgb) {
        d.enc.matrix = YGFX_MATRIX_RGB;
        d.enc.range = YGFX_RANGE_FULL;
        d.enc.transfer = YGFX_TRC_SRGB;
        d.enc.primaries = YGFX_PRIM_DEVICE;
        d.enc.siting = YGFX_SITING_NONE;
    }
    t->t = ygfx_texture(&g->g, &d);
    if (!t->t.id) { *code = yv_gfx_err(g, YGFX_ERR_FULL, err, cap); free(t); return NULL; }
    t->w = w; t->h = h; t->format = format;
    yv_link(g, &t->o, YV_O_TEX);
    return t;
}

static int yv_tex_update(yv_tex* t, int x, int y, int w, int h, const void* data, char* err, size_t cap) {
    yv_gfx* g = t->o.g;
    int rc;
    if (yv_live(&t->o, err, cap) < 0 || !t->t.id) return yv_fail(err, cap, YGFX_ERR_CLOSED, "texture: closed");
    if (g->in_frame) return yv_fail(err, cap, YGFX_ERR_ORDER, "texture: update it between frames (a queued draw reads it at end())");
    if (x < 0 || y < 0 || w < 1 || h < 1 || x + w > t->w || y + h > t->h)
        return yv_fail(err, cap, YGFX_ERR_ARG, "texture: the rectangle is outside its %d x %d texels", t->w, t->h);
    rc = ygfx_texture_update(&g->g, t->t, x, y, w, h, data, 0);
    return rc < 0 ? yv_gfx_err(g, rc, err, cap) : 0;
}

static void yv_tex_free(yv_tex* t) {
    if (!t) return;
    if (t->o.g && t->t.id) yv_free_tex_id(t->o.g, t->t);
    yv_unlink(&t->o);
    free(t);
}

static void yv_group_free(yv_group* gr) {
    if (!gr) return;
    if (gr->o.g && gr->o.users) yv_unbind_obj(gr->o.g, &gr->o);
    yv_unlink(&gr->o);
    free(gr);
}

/* ---- draw lists ------------------------------------------------------------------------------ */

static yv_list* yv_list_make(yv_gfx* g, int* code, char* err, size_t cap) {
    yv_list* l;
    *code = YGFX_ERR_ARG;
    if (!g->open) { *code = yv_fail(err, cap, YGFX_ERR_CLOSED, "ysp_gfx: the gfx (or its screen) is closed"); return NULL; }
    l = (yv_list*)calloc(1, sizeof *l);
    if (!l) { *code = yv_fail(err, cap, YGFX_ERR_FULL, "out of memory"); return NULL; }
    yv_link(g, &l->o, YV_O_LIST);
    return l;
}

/* Appends st (one more user of it). */
static int yv_list_add(yv_list* l, yv_stim* st, char* err, size_t cap) {
    if (yv_live(&l->o, err, cap) < 0) return YGFX_ERR_CLOSED;
    if (!st || st->o.g != l->o.g) return yv_fail(err, cap, YGFX_ERR_ARG, "list: a stimulus of the same gfx");
    if (l->n == l->cap) {
        int nc = l->cap ? 2 * l->cap : 16;
        yv_stim** p = (yv_stim**)realloc(l->it, sizeof(yv_stim*) * (size_t)nc);
        if (!p) return yv_fail(err, cap, YGFX_ERR_FULL, "out of memory");
        l->it = p;
        l->cap = nc;
    }
    l->it[l->n++] = st;
    st->o.users++;
    return 0;
}

static void yv_list_clear(yv_list* l) {
    int i;
    for (i = 0; i < l->n; i++) l->it[i]->o.users--;
    l->n = 0;
}

static int yv_list_remove(yv_list* l, int i, char* err, size_t cap) {
    if (i < 0 || i >= l->n) return yv_fail(err, cap, YGFX_ERR_ARG, "list: index %d outside 0 to %d", i, l->n - 1);
    l->it[i]->o.users--;
    memmove(&l->it[i], &l->it[i + 1], sizeof(yv_stim*) * (size_t)(l->n - i - 1));
    l->n--;
    return 0;
}

static void yv_list_free(yv_list* l) {
    if (!l) return;
    yv_list_clear(l);
    yv_unlink(&l->o);
    free(l->it);
    free(l);
}

/* name for every item: k values each (count = n x k), or k values for all.
 * The name is found once per run of items of one kind and shape. */
static int yv_list_set(yv_list* l, const char* name, const double* v, int count, int space, char* err, size_t cap) {
    yv_attr a;
    int i, k = 0, last_kind = -1, last_shape = -1, all;
    if (yv_live(&l->o, err, cap) < 0) return YGFX_ERR_CLOSED;
    if (l->n == 0) return 0;
    for (i = 0; i < l->n; i++) {
        yv_stim* st = l->it[i];
        if (st->s.kind != last_kind || st->s.shape != last_shape) {
            if (yv_attr_find(name, st->s.kind, st->s.shape, &a, err, cap) < 0) return YGFX_ERR_ARG;
            if (a.type == YV_A_SPECIAL) return yv_fail(err, cap, YGFX_ERR_ARG, "%s: set it item by item", name);
            last_kind = st->s.kind;
            last_shape = st->s.shape;
            k = a.type == YV_A_FLOAT || a.type == YV_A_ENUM ? 1 : a.width;
            if (count != k && count != k * l->n && !(count == l->n && a.fill))
                return yv_fail(err, cap, YGFX_ERR_ARG, "%s: %d values (%d per item) or %d for all, not %d", name, k * l->n, k, k, count);
        }
        /* k values for all first (3 items and a color of 3 is one color),
         * then k per item, then one per item filling a vector (gray) */
        all = count == k;
        if (all || count == k * l->n) {
            if (yv_attr_set(st, &a, all ? v : v + (size_t)i * (size_t)k, k, space, err, cap) < 0) return YGFX_ERR_ARG;
        } else if (yv_attr_set(st, &a, v + i, 1, space, err, cap) < 0) {
            return YGFX_ERR_ARG;
        }
    }
    return 0;
}

/* name of every item into out (n x k, k in *k; out holds n x 32). */
static int yv_list_get(yv_list* l, const char* name, double* out, int* k, char* err, size_t cap) {
    yv_attr a;
    int i, m, last_kind = -1, last_shape = -1;
    *k = 1;
    if (yv_live(&l->o, err, cap) < 0) return YGFX_ERR_CLOSED;
    for (i = 0; i < l->n; i++) {
        yv_stim* st = l->it[i];
        if (st->s.kind != last_kind || st->s.shape != last_shape) {
            if (yv_attr_find(name, st->s.kind, st->s.shape, &a, err, cap) < 0) return YGFX_ERR_ARG;
            if (a.type == YV_A_SPECIAL) return yv_fail(err, cap, YGFX_ERR_ARG, "%s: read it item by item", name);
            last_kind = st->s.kind;
            last_shape = st->s.shape;
            if (i > 0 && (a.type == YV_A_FLOAT || a.type == YV_A_ENUM ? 1 : a.width) != *k)
                return yv_fail(err, cap, YGFX_ERR_ARG, "%s: the items differ in its width", name);
            *k = a.type == YV_A_FLOAT || a.type == YV_A_ENUM ? 1 : a.width;
        }
        if (yv_attr_get(st, &a, out + (size_t)i * (size_t)*k, &m, err, cap) < 0) return YGFX_ERR_ARG;
    }
    return 0;
}

/* ---- timeline bindings ----------------------------------------------------------------------- */

static yv_tl* yv_tl_find(yv_gfx* g, const ytl_timeline* tl) {
    int i;
    for (i = 0; i < g->n_tl; i++) if (g->tl[i].tl == tl) return &g->tl[i];
    return NULL;
}

/* Attaches tl, pinned by the host's acquire(). The gfx keeps one pin per
 * timeline: a second pin of one attached already is given back now. */
static yv_tl* yv_tl_attach(yv_gfx* g, ytl_timeline* tl, char* err, size_t cap) {
    yv_tl* t = yv_tl_find(g, tl);
    if (t) { ysp_timeline_api_ptr->head.release(tl); return t; }
    if (g->n_tl == YV_MAX_TL) {
        ysp_timeline_api_ptr->head.release(tl);
        yv_fail(err, cap, YGFX_ERR_FULL, "bind: at most %d timelines per gfx", YV_MAX_TL);
        return NULL;
    }
    t = &g->tl[g->n_tl++];
    memset(t, 0, sizeof *t);
    t->tl = tl;
    return t;
}

static void yv_tl_drop(yv_gfx* g, yv_tl* t) {
    int i = (int)(t - g->tl);
    ysp_timeline_api_ptr->head.release(t->tl);
    free(t->b);
    free(t->who);
    memmove(&g->tl[i], &g->tl[i + 1], sizeof(yv_tl) * (size_t)(g->n_tl - i - 1));
    g->n_tl--;
}

static void yv_tl_release_all(yv_gfx* g) {
    int i, j;
    for (i = 0; i < g->n_tl; i++)
        for (j = 0; j < g->tl[i].n; j++) {
            yv_obj* o = g->tl[i].who[j];
            o->users--;
            if (o->type == YV_O_STIM) ((yv_stim*)o)->bound--;
        }
    while (g->n_tl) yv_tl_drop(g, &g->tl[g->n_tl - 1]);
}

static void yv_bind_remove_at(yv_tl* t, int j) {
    yv_obj* o = t->who[j];
    o->users--;
    if (o->type == YV_O_STIM) ((yv_stim*)o)->bound--;
    memmove(&t->b[j], &t->b[j + 1], sizeof(ygfx_bind) * (size_t)(t->n - j - 1));
    memmove(&t->who[j], &t->who[j + 1], sizeof(yv_obj*) * (size_t)(t->n - j - 1));
    t->n--;
}

/* Timelines with no binding left and no evaluate go (their pin too). */
static void yv_tl_gc(yv_gfx* g) {
    int i;
    for (i = g->n_tl - 1; i >= 0; i--)
        if (g->tl[i].n == 0 && !g->tl[i].evaluate) yv_tl_drop(g, &g->tl[i]);
}

/* Removes every binding that writes o (one object's free). */
static void yv_unbind_obj(yv_gfx* g, yv_obj* o) {
    int i, j;
    for (i = 0; i < g->n_tl; i++)
        for (j = g->tl[i].n - 1; j >= 0; j--)
            if (g->tl[i].who[j] == o) yv_bind_remove_at(&g->tl[i], j);
    yv_tl_gc(g);
}

/* A binding that writes the same place as b, anywhere: replaced. */
static void yv_bind_same_remove(yv_gfx* g, const ygfx_bind* b) {
    int i, j;
    for (i = 0; i < g->n_tl; i++)
        for (j = g->tl[i].n - 1; j >= 0; j--) {
            const ygfx_bind* c = &g->tl[i].b[j];
            if (b->field ? c->field == b->field
                         : (!c->field && c->stim == b->stim && c->group == b->group && c->param == b->param))
                yv_bind_remove_at(&g->tl[i], j);
        }
}

static int yv_bind_push(yv_gfx* g, yv_tl* t, const ygfx_bind* b, yv_obj* who, char* err, size_t cap) {
    yv_bind_same_remove(g, b);
    if (t->n == t->cap) {
        int nc = t->cap ? 2 * t->cap : 16;
        ygfx_bind* nb = (ygfx_bind*)realloc(t->b, sizeof(ygfx_bind) * (size_t)nc);
        yv_obj** nw;
        if (!nb) return yv_fail(err, cap, YGFX_ERR_FULL, "out of memory");
        t->b = nb;
        nw = (yv_obj**)realloc(t->who, sizeof(yv_obj*) * (size_t)nc);
        if (!nw) return yv_fail(err, cap, YGFX_ERR_FULL, "out of memory");
        t->who = nw;
        t->cap = nc;
    }
    t->b[t->n] = *b;
    t->who[t->n] = who;
    t->n++;
    who->users++;
    if (who->type == YV_O_STIM) ((yv_stim*)who)->bound++;
    return 0;
}

/* A float field of a stimulus by name: its YGFX_P_* rows (k of them for
 * color, dir and tint, which bind to k channels). */
static int yv_bind_params(const yv_stim* st, const char* name, int* p0, int* k, char* err, size_t cap) {
    const ygfx_param* t;
    int n, i;
    static const struct { const char* name; int p, k; } vecs[] = {
        { "color", YGFX_P_COLOR_R, 3 }, { "dir", YGFX_P_DIR_R, 3 }, { "tint", YGFX_P_TINT_R, 4 },
        { "src", YGFX_P_SRC_X, 4 }, { "shape_p", YGFX_P_SHAPE_P0, 4 }, { "trim", YGFX_P_TRIM_START, 2 },
    };
    for (i = 0; i < (int)(sizeof vecs / sizeof vecs[0]); i++)
        if (!strcmp(name, vecs[i].name)) { *p0 = vecs[i].p; *k = vecs[i].k; return 0; }
    t = ygfx_params(&n);
    for (i = 0; i < n; i++)
        if (!strcmp(name, t[i].name)) {
            if (!(t[i].kinds & YV_KB(st->s.kind)))
                return yv_fail(err, cap, YGFX_ERR_ARG, "bind: %s is not a parameter of this kind of stimulus", name);
            *p0 = i;
            *k = 1;
            return 0;
        }
    return yv_fail(err, cap, YGFX_ERR_ARG, "bind: '%s' is not a float parameter (enums do not bind)", name);
}

static int yv_chan_ok(const int* ch, int n, char* err, size_t cap) {
    int i;
    for (i = 0; i < n; i++)
        if (ch[i] < 0 || ch[i] >= YTL_MAX_CHANNELS) return yv_fail(err, cap, YGFX_ERR_ARG, "bind: a channel is 0 to %d", YTL_MAX_CHANNELS - 1);
    return 0;
}

/* Binds target's `name` to channels of tl (already pinned by the host's
 * acquire(); the gfx keeps or gives back that pin). target: a stimulus
 * (k channels for a k-wide field), a group, a list (one channel per item,
 * or one for all) or an instances' elements (first, then one channel per
 * element, or `name` of the template when first < 0). */
static int yv_bind(yv_gfx* g, ytl_timeline* tl, yv_obj* target, const char* name, int first, const int* ch, int n_ch,
                   char* err, size_t cap) {
    yv_tl* t;
    ygfx_bind b;
    int i, rc = 0;
    if (!g->open) { ysp_timeline_api_ptr->head.release(tl); return yv_fail(err, cap, YGFX_ERR_CLOSED, "ysp_gfx: the gfx is closed"); }
    if (!target || target->g != g) { ysp_timeline_api_ptr->head.release(tl); return yv_fail(err, cap, YGFX_ERR_ARG, "bind: an object of this gfx"); }
    if (n_ch < 1 || yv_chan_ok(ch, n_ch, err, cap) < 0) {
        ysp_timeline_api_ptr->head.release(tl);
        return n_ch < 1 ? yv_fail(err, cap, YGFX_ERR_ARG, "bind: one channel or more") : YGFX_ERR_ARG;
    }
    if (!(t = yv_tl_attach(g, tl, err, cap))) return YGFX_ERR_FULL;
    memset(&b, 0, sizeof b);
    if (target->type == YV_O_GROUP) {
        const ygfx_param* gp = ygfx_group_params(NULL);
        int m;
        ygfx_group_params(&m);
        for (i = 0; i < m && strcmp(gp[i].name, name); i++) {}
        if (i == m) rc = yv_fail(err, cap, YGFX_ERR_ARG, "bind: no group parameter '%s'", name);
        else if (n_ch != 1) rc = yv_fail(err, cap, YGFX_ERR_ARG, "bind: one channel for a group's %s", name);
        else {
            b.group = &((yv_group*)target)->grp;
            b.param = (uint16_t)i;
            b.channel = (uint16_t)ch[0];
            rc = yv_bind_push(g, t, &b, target, err, cap);
        }
    } else if (target->type == YV_O_STIM && ((yv_stim*)target)->vkind == YV_K_INSTANCES && first >= 0) {
        yv_stim* st = (yv_stim*)target;
        int f = yv_ifield_find(name);
        if (f < 0) rc = yv_fail(err, cap, YGFX_ERR_ARG, "bind: an element's x, y, ori, phase, contrast, scale, gate or color");
        else if (!(st->s.inst_fields & yv_ifields[f].bit)) rc = yv_fail(err, cap, YGFX_ERR_ARG, "bind: %s is not in the instances' fields", name);
        else if (first + n_ch > (int)st->s.n_inst) rc = yv_fail(err, cap, YGFX_ERR_ARG, "bind: elements %d to %d of %u", first, first + n_ch - 1, st->s.n_inst);
        for (i = 0; rc == 0 && i < n_ch; i++) {
            b.field = (float*)&st->inst[first + i] + yv_ifields[f].off;
            b.channel = (uint16_t)ch[i];
            rc = yv_bind_push(g, t, &b, target, err, cap);
        }
    } else if (target->type == YV_O_STIM) {
        yv_stim* st = (yv_stim*)target;
        int p0, k;
        rc = yv_bind_params(st, name, &p0, &k, err, cap);
        if (rc == 0 && n_ch != k) rc = yv_fail(err, cap, YGFX_ERR_ARG, "bind: %s takes %d channel%s", name, k, k > 1 ? "s" : "");
        for (i = 0; rc == 0 && i < k; i++) {
            b.stim = &st->s;
            b.param = (uint16_t)(p0 + i);
            b.channel = (uint16_t)ch[i];
            rc = yv_bind_push(g, t, &b, target, err, cap);
        }
    } else if (target->type == YV_O_LIST) {
        yv_list* l = (yv_list*)target;
        if (n_ch != 1 && n_ch != l->n) rc = yv_fail(err, cap, YGFX_ERR_ARG, "bind: one channel per item (%d), or one for all", l->n);
        for (i = 0; rc == 0 && i < l->n; i++) {
            int p0, k;
            rc = yv_bind_params(l->it[i], name, &p0, &k, err, cap);
            if (rc == 0 && k != 1) rc = yv_fail(err, cap, YGFX_ERR_ARG, "bind: a list binds one-number parameters (%s has %d)", name, k);
            if (rc == 0) {
                b.stim = &l->it[i]->s;
                b.param = (uint16_t)p0;
                b.channel = (uint16_t)ch[n_ch == 1 ? 0 : i];
                rc = yv_bind_push(g, t, &b, &l->it[i]->o, err, cap);
            }
        }
    } else {
        rc = yv_fail(err, cap, YGFX_ERR_ARG, "bind: a stimulus, list, group or instances");
    }
    yv_tl_gc(g);
    return rc;
}

/* Removes bindings: of target (NULL: any), named name (NULL: any), of tl
 * (NULL: any). Returns how many. Timelines left with none go. */
static int yv_unbind(yv_gfx* g, yv_obj* target, const char* name, const ytl_timeline* tl) {
    int i, j, removed = 0;
    for (i = 0; i < g->n_tl; i++) {
        yv_tl* t = &g->tl[i];
        if (tl && t->tl != tl) continue;
        for (j = t->n - 1; j >= 0; j--) {
            const ygfx_bind* b = &t->b[j];
            int match = !target || t->who[j] == target;
            if (target && target->type == YV_O_LIST) {
                int k;
                yv_list* l = (yv_list*)target;
                match = 0;
                for (k = 0; k < l->n; k++) if (t->who[j] == &l->it[k]->o) match = 1;
            }
            if (match && name) {
                if (b->field) match = 0;
                else if (b->group) match = !strcmp(ygfx_group_params(NULL)[b->param].name, name);
                else {
                    const char* pn = ygfx_params(NULL)[b->param].name;
                    size_t ln = strlen(name);
                    match = !strcmp(pn, name) || (!strncmp(pn, name, ln) && pn[ln] == '_' &&
                                                  (!strcmp(name, "color") || !strcmp(name, "dir") || !strcmp(name, "tint") ||
                                                   !strcmp(name, "src") || !strcmp(name, "trim")));
                }
                if (!match && b->field && t->who[j]->type == YV_O_STIM) {
                    yv_stim* st = (yv_stim*)t->who[j];
                    int f = yv_ifield_find(name);
                    if (st->vkind == YV_K_INSTANCES && f >= 0) {
                        size_t off = (size_t)((const char*)b->field - (const char*)st->inst) % sizeof(ygfx_inst);
                        match = off == sizeof(float) * (size_t)yv_ifields[f].off;
                    }
                }
            }
            if (match) { yv_bind_remove_at(t, j); removed++; }
        }
        if (tl) t->evaluate = 0;
    }
    yv_tl_gc(g);
    return removed;
}

/* Evaluate on or off for tl (attached by the host's pin). */
static int yv_tl_evaluate(yv_gfx* g, ytl_timeline* tl, int on, char* err, size_t cap) {
    yv_tl* t;
    if (!g->open) { ysp_timeline_api_ptr->head.release(tl); return yv_fail(err, cap, YGFX_ERR_CLOSED, "ysp_gfx: the gfx is closed"); }
    if (!(t = yv_tl_attach(g, tl, err, cap))) return YGFX_ERR_FULL;
    t->evaluate = on != 0;
    yv_tl_gc(g);
    return 0;
}

static int yv_bind_count(const yv_gfx* g) {
    int i, n = 0;
    for (i = 0; i < g->n_tl; i++) n += g->tl[i].n;
    return n;
}

/* ---- the frame ----------------------------------------------------------------------------- */

/* The bindings: each timeline evaluated (when asked) at the screen frame,
 * then its values written. YV_ERR_HOST: a Python callback raised. */
static int yv_gfx_apply(yv_gfx* g, char* err, size_t cap) {
    int i, rc;
    for (i = 0; i < g->n_tl; i++) {
        yv_tl* t = &g->tl[i];
        if (t->evaluate) {
            ytl_frame f;
            f.onset = g->scr->f.onset;
            f.period = g->scr->f.period;
            f.index = g->scr->f.index;
            rc = ytl_evaluate(t->tl, &f, NULL, 0);
            if (rc == YV_ERR_HOST) return rc;
            if (rc < 0) {
                const char* m = ytl_error(t->tl);
                return yv_fail(err, cap, YGFX_ERR_ORDER, "evaluate: %s", m && m[0] ? m : ytl_strerror(rc));
            }
        }
        if (t->n) ygfx_apply(t->b, t->n, ytl_values(t->tl));
    }
    return 0;
}

/* The screen's current frame: call between its begin() and flip(). */
static int yv_gfx_begin(yv_gfx* g, char* err, size_t cap) {
    int rc;
    if (!g->open || !g->scr) return yv_fail(err, cap, YGFX_ERR_CLOSED, "ysp_gfx: the gfx (or its screen) is closed");
    if (!g->scr->in_frame) return yv_fail(err, cap, YGFX_ERR_ORDER, "ysp_gfx: begin() after the screen's begin()");
    if (g->in_frame) return yv_fail(err, cap, YGFX_ERR_ORDER, "ysp_gfx: begin() twice");
    rc = yv_gfx_apply(g, err, cap);
    if (rc < 0) return rc;
    rc = ygfx_begin(&g->g, &g->scr->f);
    if (rc < 0) return yv_gfx_err(g, rc, err, cap);
    g->in_frame = 1;
    return 0;
}

/* Draws a stimulus or every item of a list, in order. *bad: the item that
 * failed (-1: none, or the stimulus itself). */
static int yv_gfx_draw(yv_gfx* g, yv_obj* o, int* bad, char* err, size_t cap) {
    int rc, i;
    *bad = -1;
    if (!g->in_frame) return yv_fail(err, cap, YGFX_ERR_ORDER, "ysp_gfx: draw between begin() and end()");
    if (!o || o->g != g) return yv_fail(err, cap, YGFX_ERR_ARG, "draw: a stimulus or list of this gfx");
    if (o->type == YV_O_STIM) {
        rc = ygfx_draw(&g->g, &((yv_stim*)o)->s);
        return rc < 0 ? yv_gfx_err(g, rc, err, cap) : 0;
    }
    if (o->type == YV_O_LIST) {
        yv_list* l = (yv_list*)o;
        for (i = 0; i < l->n; i++) {
            rc = ygfx_draw(&g->g, &l->it[i]->s);
            if (rc < 0) { *bad = i; return yv_gfx_err(g, rc, err, cap); }
        }
        return 0;
    }
    return yv_fail(err, cap, YGFX_ERR_ARG, "draw: a stimulus or list");
}

static int yv_gfx_end(yv_gfx* g, char* err, size_t cap) {
    int rc;
    if (!g->open) return yv_fail(err, cap, YGFX_ERR_CLOSED, "ysp_gfx: the gfx (or its screen) is closed");
    if (!g->in_frame) return yv_fail(err, cap, YGFX_ERR_ORDER, "ysp_gfx: end() after begin()");
    rc = ygfx_end(&g->g);
    g->in_frame = 0;
    yv_flush_defer(g);
    g->frames++;
    return rc < 0 ? yv_gfx_err(g, rc, err, cap) : 0;
}

/* begin, the bindings, every object in order, end: a frame in one call.
 * A draw that fails still ends the frame; its error is returned. */
static int yv_gfx_frame(yv_gfx* g, yv_obj* const* objs, int n, int* bad_obj, int* bad_item, char* err, size_t cap) {
    int rc, i;
    *bad_obj = *bad_item = -1;
    rc = yv_gfx_begin(g, err, cap);
    if (rc < 0) return rc;
    for (i = 0; i < n; i++) {
        rc = yv_gfx_draw(g, objs[i], bad_item, err, cap);
        if (rc < 0) {
            char keep[512];
            *bad_obj = i;
            snprintf(keep, sizeof keep, "%s", err);
            yv_gfx_end(g, err, cap);
            snprintf(err, cap, "%s", keep);
            return rc;
        }
    }
    return yv_gfx_end(g, err, cap);
}

/* ---- hit tests -------------------------------------------------------------------------- */

/* Whether screen pixel (px, py) is inside st (ygfx_hit; instances: the
 * topmost element, else -1). */
static int yv_stim_hit(const yv_stim* st, float px, float py) {
    if (st->vkind == YV_K_INSTANCES) return ygfx_hit_index(&st->o.g->g, &st->s, px, py);
    return ygfx_hit(&st->o.g->g, &st->s, px, py) ? 0 : -1;
}

/* The topmost visible item of l under (px, py), and its element; -1. */
static int yv_list_hit(const yv_list* l, float px, float py, int* element) {
    int i;
    *element = -1;
    for (i = l->n - 1; i >= 0; i--) {
        const yv_stim* st = l->it[i];
        int e;
        if (st->s.visible < 0.5f) continue;
        if (st->s.group && st->s.group->visible < 0.5f) continue;
        e = yv_stim_hit(st, px, py);
        if (e >= 0) { *element = st->vkind == YV_K_INSTANCES ? e : -1; return i; }
    }
    return -1;
}

/* ---- the C path, for the pixel test ---------------------------------------------------------- */

/* Draws a fixed scene made with ysp/gfx.h's constructors directly, as C
 * code would: the tests make the same stimuli through the binding and
 * compare the scenes read back (tests/test_window.py, test_mex_vis_window.m).
 * which: 0 a gabor, 1 a grating in a circle with a cosine edge, 2 a stroked
 * rounded rect, 3 a star, 4 binary noise in a circle. */
static int yv_gfx_c_scene(yv_gfx* g, int which, char* err, size_t cap) {
    ygfx_stim s;
    int rc;
    switch (which) {
    case 0: {
        ygfx_gabor_desc d;
        memset(&d, 0, sizeof d);
        d.x = -100; d.y = 40; d.ori = 30; d.sf = 1.0f / 16; d.phase = 0.25f; d.sigma = 12; d.contrast = 0.4f;
        s = ygfx_gabor(&d);
        break;
    }
    case 1: {
        ygfx_grating_desc d;
        memset(&d, 0, sizeof d);
        d.x = 80; d.y = -30; d.w = 120; d.ori = -20; d.sf = 1.0f / 20; d.contrast = 0.3f;
        d.aperture = YGFX_CIRCLE; d.edge = YGFX_EDGE_COSINE; d.edge_width = 6;
        s = ygfx_grating(&d);
        break;
    }
    case 2: {
        ygfx_shape_desc d;
        memset(&d, 0, sizeof d);
        d.shape = YGFX_RECT; d.x = 0; d.y = 0; d.w = 160; d.h = 90; d.ori = 10; d.shape_p[1] = 12;
        d.color[0] = 0.9f; d.color[1] = 0.2f; d.color[2] = 0.1f; d.stroke = 4; d.stroke_align = YGFX_STROKE_INSIDE;
        d.edge = YGFX_EDGE_GAUSSIAN; d.edge_width = 1;
        s = ygfx_shape(&d);
        break;
    }
    case 3: {
        ygfx_shape_desc d;
        memset(&d, 0, sizeof d);
        d.shape = YGFX_STAR; d.place = YGFX_TOP_LEFT; d.anchor = YGFX_TOP_LEFT; d.x = 20; d.y = 20; d.w = 100;
        d.shape_p[0] = 5; d.shape_p[1] = 0.4f; d.color[0] = 0.1f; d.color[1] = 0.8f; d.color[2] = 0.3f; d.opacity = 0.75f;
        s = ygfx_shape(&d);
        break;
    }
    case 4: {
        ygfx_noise_desc d;
        memset(&d, 0, sizeof d);
        d.x = 150; d.y = 60; d.w = 64; d.check = 4; d.seed = 7; d.dist = YGFX_BINARY; d.contrast = 0.25f; d.aperture = YGFX_CIRCLE;
        s = ygfx_noise(&d);
        break;
    }
    default:
        return yv_fail(err, cap, YGFX_ERR_ARG, "c_scene: 0 to 4");
    }
    rc = ygfx_draw(&g->g, &s);
    return rc < 0 ? yv_gfx_err(g, rc, err, cap) : 0;
}

#endif /* YSP_VIS_GFX_H */
