/* ysp_vis.c - the ysp-vis core for MATLAB and Octave: ysp/screen.h, ysp/gfx.h, ysp/video.h
 *
 * One library holds the implementation and the one handle table; the thin
 * MEX files ysp_screen, ysp_gfx and ysp_video load it and pass their calls
 * here (ysp_vis_mex.h: the layout and the LIFETIME rule). Windows only.
 *
 *     h = ysp_screen('open', 'windowed', true);          % settles: 0.5 to 2.5 s
 *     g = ysp_gfx('open', h, 0.5);                      % background gray
 *     while true
 *         [f, quit] = ysp_screen('begin', h);           % quit: Shift+Esc and the like
 *         if ~isempty(quit), break; end
 *         ysp_gfx('begin', g);
 *         ysp_gfx('rect', g, [0 0 200 100], [1 1 1]);   % center x, y from the screen's center
 *         ysp_gfx('end', g);
 *         ysp_screen('flip', h);                        % at f.onset
 *     end
 *     r = ysp_screen('records', h);                     % every flip record, seconds and int64 ns
 *     ysp_screen('close', h);
 *
 * TIMES: seconds (double); rounded to the nearest ns for the C call; a
 * negative or NaN time is refused. Records, frames and events carry the
 * exact ns beside the seconds as int64.
 *
 * THREADS: the window belongs to ysp/screen.h's window thread, so pause()
 * between trials does not hang it (in Octave too). No MATLAB callback runs
 * on a C thread.
 *
 * SHARING: ysp/audio.h and ysp/timeline.h are declarations here; their calls
 * go through the tables of ysp_audio and ysp_timeline, loaded from this
 * file's directory when first needed ('shared' loads them now).
 *
 * NATIVES: SDL3 is linked statically; ANGLE's libEGL.dll and libGLESv2.dll
 * sit beside this file, and desc.angle_dir is always this file's directory.
 */
#include "mex.h"

#define YSP_SHARE_MEX
#include "ysp_vis_core.h"
#include "ysp_vis_mex.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- the module of the call ------------------------------------------------- */

static const char* vx_mod = "ysp_vis";   /* the error ids' prefix: the thin MEX called */

static void vx_err(const char* code, const char* fmt, ...) {
    char id[64], msg[1024];
    va_list ap;
    snprintf(id, sizeof id, "%s:%s", vx_mod, code);
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    mexErrMsgIdAndTxt(id, "%s", msg);
}

static const char* vx_code(int code) {
    switch (code) {
    case YSCR_ERR_ARG: return "arg";
    case YSCR_ERR_CLOSED: return "closed";
    case YSCR_ERR_TIMEOUT: return "timeout";
    case YSCR_ERR_LOST: return "lost";
    case YSCR_ERR_ORDER: return "order";
    case YSCR_ERR_NOT_IMPLEMENTED: return "not_implemented";
    case YSCR_ERR_REFUSED: return "refused";
    default: return "error";
    }
}

static const char* vx_gfx_code(int code) {
    switch (code) {
    case YGFX_ERR_ARG: case YGFX_ERR_RANGE: case YGFX_ERR_FORMAT: return "arg";
    case YGFX_ERR_CLOSED: return "closed";
    case YGFX_ERR_ORDER: return "order";
    case YGFX_ERR_LOST: return "lost";
    case YGFX_ERR_NOT_IMPLEMENTED: return "not_implemented";
    case YGFX_ERR_REFUSED: return "refused";
    default: return "error";
    }
}

/* ---- the handle table --------------------------------------------------------- */
/* A handle is a counter, never reused, starting at a number of this module's
 * own ('VS' x 2^32 + 1), so a handle of another ysp MEX is not taken. */

enum { VX_SCREEN = 1, VX_GFX = 2 };

typedef struct vx_node {
    uint64_t        id;
    int             kind;
    void*           obj;
    struct vx_node* next;
} vx_node;

static vx_node* vx_list = NULL;
static uint64_t vx_next_id = ((uint64_t)0x5653 << 32) + 1;
static int      vx_users = 0;

static uint64_t vx_register(int kind, void* obj) {
    vx_node* n = (vx_node*)malloc(sizeof *n);
    if (!n) return 0;
    n->id = vx_next_id++;
    n->kind = kind;
    n->obj = obj;
    n->next = vx_list;
    vx_list = n;
    return n->id;
}

static void vx_unregister(void* obj) {
    vx_node** p = &vx_list;
    while (*p) {
        if ((*p)->obj == obj) { vx_node* d = *p; *p = d->next; free(d); return; }
        p = &(*p)->next;
    }
}

static void* vx_lookup(const mxArray* a, int kind, const char* what) {
    uint64_t id = 0;
    vx_node* n;
    if (mxIsUint64(a) && mxGetNumberOfElements(a) == 1) id = *(const uint64_t*)mxGetData(a);
    else vx_err("handle", "%s: a handle (uint64) from %s('open')", what, kind == VX_SCREEN ? "ysp_screen" : "ysp_gfx");
    for (n = vx_list; n; n = n->next)
        if (n->id == id) {
            if (n->kind != kind) vx_err("handle", "%s: a %s handle is needed here", what, kind == VX_SCREEN ? "screen" : "gfx");
            return n->obj;
        }
    vx_err("handle", "%s: invalid or closed handle", what);
    return NULL;
}

static mxArray* vx_handle_out(uint64_t id) {
    mxArray* h = mxCreateNumericMatrix(1, 1, mxUINT64_CLASS, mxREAL);
    *(uint64_t*)mxGetData(h) = id;
    return h;
}

static void vx_gfx_drop(struct yv_gfx* g);   /* ysp_vis_gfx.inc */
static void vx_video_close_all(void);       /* ysp_vis_video.inc */

/* Closes and frees every object: movies first, then each gfx (its GL
 * objects before the context), then screens. */
static void vx_close_all(void) {
    int pass;
    vx_node* g;
    vx_video_close_all();
    /* each gfx with its stimuli, lists, groups and textures (and their
     * handles), and its timeline pins */
    for (g = vx_list; g;) {
        if (g->kind == VX_GFX) { vx_gfx_drop((yv_gfx*)g->obj); g = vx_list; }
        else g = g->next;
    }
    for (pass = 0; pass < 2; pass++) {
        vx_node** p = &vx_list;
        while (*p) {
            vx_node* n = *p;
            if ((pass == 0 && n->kind == VX_GFX) || (pass == 1 && n->kind == VX_SCREEN)) {
                *p = n->next;
                if (n->kind == VX_GFX) { yv_gfx_close((yv_gfx*)n->obj); free(n->obj); }
                else yv_free((yv_screen*)n->obj);
                free(n);
            } else {
                p = &n->next;
            }
        }
    }
}

static void vx_attach(void) { vx_users++; }

static void vx_detach(void) {
    if (vx_users > 0) vx_users--;
    if (vx_users > 0) return;
    vx_close_all();
    /* The movies and the gfx gave back their pins above; now the owners' modules. */
    ysp_share_mex_unload(ysp_audio_api_module);
    ysp_share_mex_unload(ysp_timeline_api_module);
    ysp_audio_api_module = ysp_timeline_api_module = NULL;
    ysp_audio_api_ptr = NULL;
    ysp_timeline_api_ptr = NULL;
}

/* ---- arguments --------------------------------------------------------------- */

static void vx_nargs(int nrhs, int need, const char* usage) {
    if (nrhs < need) vx_err("usage", "usage: %s", usage);
}

static double vx_scalar(const mxArray* a, const char* what) {
    if ((!mxIsNumeric(a) && !mxIsLogical(a)) || mxIsComplex(a) || mxGetNumberOfElements(a) != 1)
        vx_err("arg", "%s: a real scalar", what);
    return mxGetScalar(a);
}

static int64_t vx_seconds(const mxArray* a, const char* what, int negative) {
    int64_t ns = 0;
    if (yv_s_to_ns(vx_scalar(a, what), negative, &ns) < 0)
        vx_err("arg", "%s: seconds%s (NaN and Inf are refused)", what, negative ? "" : ", >= 0");
    return ns;
}

static void vx_string(const mxArray* a, char* buf, size_t cap, const char* what) {
    if (!mxIsChar(a) || mxGetString(a, buf, (mwSize)cap) != 0) vx_err("arg", "%s: a string (at most %d characters)", what, (int)cap - 1);
}

static int vx_bool(const mxArray* a, const char* what) { return vx_scalar(a, what) != 0.0; }

static mxArray* vx_i64(int64_t v) {
    mxArray* a = mxCreateNumericMatrix(1, 1, mxINT64_CLASS, mxREAL);
    *(int64_t*)mxGetData(a) = v;
    return a;
}

static mxArray* vx_d(double v) { return mxCreateDoubleScalar(v); }

/* ---- conversions ---------------------------------------------------------------- */

static const char* vx_record_f[] = { "index", "target", "target_ns", "planned", "planned_ns", "onset", "onset_ns",
                                     "residual", "residual_ns", "dropped", "missed", "late", "path", "tier", "flags",
                                     "phases", "code_risk" };

static void vx_record_set(mxArray* s, size_t k, const yscr_record* r) {
    mxArray* ph = mxCreateDoubleMatrix(1, YSCR_N_PHASES, mxREAL);
    double* p = mxGetPr(ph);
    int i;
    for (i = 0; i < YSCR_N_PHASES; i++) p[i] = r->phase_ns[i] == YSCR_PHASE_UNKNOWN ? mxGetNaN() : (double)r->phase_ns[i] / 1e9;
    mxSetFieldByNumber(s, k, 0, vx_d((double)r->index));
    mxSetFieldByNumber(s, k, 1, vx_d(yv_s(r->target)));
    mxSetFieldByNumber(s, k, 2, vx_i64(r->target));
    mxSetFieldByNumber(s, k, 3, vx_d(yv_s(r->planned)));
    mxSetFieldByNumber(s, k, 4, vx_i64(r->planned));
    mxSetFieldByNumber(s, k, 5, vx_d(yv_s(r->onset)));
    mxSetFieldByNumber(s, k, 6, vx_i64(r->onset));
    mxSetFieldByNumber(s, k, 7, vx_d(yv_s(r->residual)));
    mxSetFieldByNumber(s, k, 8, vx_i64(r->residual));
    mxSetFieldByNumber(s, k, 9, vx_d(r->dropped));
    mxSetFieldByNumber(s, k, 10, mxCreateLogicalScalar(r->dropped > 0));
    mxSetFieldByNumber(s, k, 11, mxCreateLogicalScalar((r->flags & YSCR_FLIP_LATE_TARGET) != 0));
    mxSetFieldByNumber(s, k, 12, vx_d(r->path));
    mxSetFieldByNumber(s, k, 13, vx_d(r->tier));
    mxSetFieldByNumber(s, k, 14, vx_d(r->flags));
    mxSetFieldByNumber(s, k, 15, ph);
    mxSetFieldByNumber(s, k, 16, vx_d(r->code_risk));
}

static mxArray* vx_records(const yscr_record* r, int n) {
    mxArray* s = mxCreateStructMatrix(1, (size_t)(n > 0 ? n : 0), 17, vx_record_f);
    int i;
    for (i = 0; i < n; i++) vx_record_set(s, (size_t)i, &r[i]);
    return s;
}

/* Records as one n x 15 double matrix (the open option frame='row'): index,
 * target, planned, onset, residual (s), dropped, path, tier, flags, then the
 * six phases (s; NaN unknown). One array: a struct of records cost 3.5 us
 * a record in MATLAB and 94 us in Octave (bench_frame.m, 2026-10-27). */
#define VX_REC_COLS 15
static mxArray* vx_records_matrix(const yscr_record* r, int n) {
    mxArray* m = mxCreateDoubleMatrix((size_t)(n > 0 ? n : 0), VX_REC_COLS, mxREAL);
    double* p = mxGetPr(m);
    size_t N = (size_t)(n > 0 ? n : 0), i;
    int k;
    for (i = 0; i < N; i++) {
        const yscr_record* x = &r[i];
        p[i] = (double)x->index;
        p[i + N] = yv_s(x->target);
        p[i + 2 * N] = yv_s(x->planned);
        p[i + 3 * N] = yv_s(x->onset);
        p[i + 4 * N] = yv_s(x->residual);
        p[i + 5 * N] = x->dropped;
        p[i + 6 * N] = x->path;
        p[i + 7 * N] = x->tier;
        p[i + 8 * N] = x->flags;
        for (k = 0; k < YSCR_N_PHASES; k++)
            p[i + (size_t)(9 + k) * N] = x->phase_ns[k] == YSCR_PHASE_UNKNOWN ? mxGetNaN() : (double)x->phase_ns[k] / 1e9;
    }
    return m;
}

static mxArray* vx_frame(const yscr_frame* f) {
    static const char* fn[] = { "onset", "onset_ns", "period", "period_ns", "index", "vblank", "done", "last",
                                "done_lost" };
    mxArray* s = mxCreateStructMatrix(1, 1, 9, fn);
    mxSetFieldByNumber(s, 0, 0, vx_d(yv_s(f->onset)));
    mxSetFieldByNumber(s, 0, 1, vx_i64(f->onset));
    mxSetFieldByNumber(s, 0, 2, vx_d(yv_s(f->period)));
    mxSetFieldByNumber(s, 0, 3, vx_i64(f->period));
    mxSetFieldByNumber(s, 0, 4, vx_d((double)f->index));
    mxSetFieldByNumber(s, 0, 5, vx_d((double)f->vblank));
    mxSetFieldByNumber(s, 0, 6, vx_records(f->done, f->n_done));
    mxSetFieldByNumber(s, 0, 7, f->last ? vx_records(f->last, 1) : mxCreateDoubleMatrix(0, 0, mxREAL));
    mxSetFieldByNumber(s, 0, 8, vx_d(f->done_lost));
    return s;
}

/* The frame without its records: one mxArray per scalar field. */
static mxArray* vx_frame_slim(const yscr_frame* f) {
    static const char* fn[] = { "onset", "onset_ns", "period", "period_ns", "index", "vblank", "n_done", "done_lost" };
    mxArray* s = mxCreateStructMatrix(1, 1, 8, fn);
    mxSetFieldByNumber(s, 0, 0, vx_d(yv_s(f->onset)));
    mxSetFieldByNumber(s, 0, 1, vx_i64(f->onset));
    mxSetFieldByNumber(s, 0, 2, vx_d(yv_s(f->period)));
    mxSetFieldByNumber(s, 0, 3, vx_i64(f->period));
    mxSetFieldByNumber(s, 0, 4, vx_d((double)f->index));
    mxSetFieldByNumber(s, 0, 5, vx_d((double)f->vblank));
    mxSetFieldByNumber(s, 0, 6, vx_d(f->n_done));
    mxSetFieldByNumber(s, 0, 7, vx_d(f->done_lost));
    return s;
}

/* The frame as one row: [onset period index vblank n_done done_lost]. */
static mxArray* vx_frame_row(const yscr_frame* f) {
    mxArray* r = mxCreateDoubleMatrix(1, 6, mxREAL);
    double* p = mxGetPr(r);
    p[0] = yv_s(f->onset);
    p[1] = yv_s(f->period);
    p[2] = (double)f->index;
    p[3] = (double)f->vblank;
    p[4] = f->n_done;
    p[5] = f->done_lost;
    return r;
}

static mxArray* vx_events(const yin_event* ev, int n) {
    static const char* f[] = { "t", "t_ns", "device", "control", "code", "mods", "kind", "type", "flags", "stamp",
                               "unc", "x", "y", "value", "aux", "ticks" };
    mxArray* s = mxCreateStructMatrix(1, (size_t)(n > 0 ? n : 0), 16, f);
    int i;
    for (i = 0; i < n; i++) {
        const yin_event* e = &ev[i];
        size_t k = (size_t)i;
        mxSetFieldByNumber(s, k, 0, vx_d(yv_s(e->t)));
        mxSetFieldByNumber(s, k, 1, vx_i64(e->t));
        mxSetFieldByNumber(s, k, 2, vx_d(e->device));
        mxSetFieldByNumber(s, k, 3, vx_d(e->control));
        mxSetFieldByNumber(s, k, 4, vx_d(e->code));
        mxSetFieldByNumber(s, k, 5, vx_d(e->mods));
        mxSetFieldByNumber(s, k, 6, vx_d(e->kind));
        mxSetFieldByNumber(s, k, 7, vx_d(e->type));
        mxSetFieldByNumber(s, k, 8, vx_d(e->flags));
        mxSetFieldByNumber(s, k, 9, vx_d(e->stamp));
        mxSetFieldByNumber(s, k, 10, vx_d((double)e->unc_us * 1e-6));
        mxSetFieldByNumber(s, k, 11, vx_d(e->x));
        mxSetFieldByNumber(s, k, 12, vx_d(e->y));
        mxSetFieldByNumber(s, k, 13, vx_d(e->value));
        mxSetFieldByNumber(s, k, 14, vx_d(e->aux));
        mxSetFieldByNumber(s, k, 15, vx_d(e->ticks));
    }
    return s;
}

static const char* vx_mode_f[] = { "w", "h", "refresh_num", "refresh_den", "refresh", "period", "period_ns", "format",
                                   "density" };

static void vx_mode_set(mxArray* s, size_t k, const yscr_mode* m) {
    mxSetFieldByNumber(s, k, 0, vx_d(m->w));
    mxSetFieldByNumber(s, k, 1, vx_d(m->h));
    mxSetFieldByNumber(s, k, 2, vx_d(m->refresh_num));
    mxSetFieldByNumber(s, k, 3, vx_d(m->refresh_den));
    mxSetFieldByNumber(s, k, 4, vx_d(m->refresh_den ? (double)m->refresh_num / m->refresh_den : 0.0));
    mxSetFieldByNumber(s, k, 5, vx_d(yv_s(m->period_ns)));
    mxSetFieldByNumber(s, k, 6, vx_i64(m->period_ns));
    mxSetFieldByNumber(s, k, 7, vx_d(m->format));
    mxSetFieldByNumber(s, k, 8, vx_d(m->density));
}

static mxArray* vx_mode(const yscr_mode* m) {
    mxArray* s = mxCreateStructMatrix(1, 1, 9, vx_mode_f);
    vx_mode_set(s, 0, m);
    return s;
}

/* ---- open options ------------------------------------------------------------ */

static void vx_option(yscr_desc* d, const char* name, const mxArray* v) {
    const yv_opt* o = yv_find_opt(name);
    char err[256];
    if (!o) vx_err("arg", "open: unknown option '%s' (ysp_screen('options') lists them)", name);
    if (mxIsEmpty(v)) return;   /* [] is the default, as 0 in the C desc */
    if (o->type == YV_ENUM) {
        char s[64];
        vx_string(v, s, sizeof s, name);
        if (yv_opt_enum(d, o, s, err, sizeof err) < 0) vx_err("arg", "%s", err);
        return;
    }
    if (o->type == YV_SIZE || o->type == YV_MODE) {
        double n[4] = { 0, 0, 0, 0 };
        size_t k, want = o->type == YV_SIZE ? 2 : 4;
        if (o->type == YV_MODE && mxIsStruct(v)) {
            static const char* f[] = { "w", "h", "refresh_num", "refresh_den" };
            for (k = 0; k < 4; k++) {
                const mxArray* x = mxGetField(v, 0, f[k]);
                if (!x) vx_err("arg", "mode: a struct from ysp_screen('modes') (w, h, refresh_num, refresh_den)");
                n[k] = vx_scalar(x, f[k]);
            }
        } else {
            if (!mxIsDouble(v) || mxIsComplex(v) || mxGetNumberOfElements(v) != want)
                vx_err("arg", o->type == YV_SIZE ? "%s: [w h] in pixels" : "%s: a mode struct, or [w h refresh_num refresh_den]", name);
            for (k = 0; k < want; k++) n[k] = mxGetPr(v)[k];
        }
        for (k = 0; k < want; k++)
            if (n[k] != floor(n[k]) || n[k] < 0 || n[k] > 2147483647.0) vx_err("arg", "%s: whole numbers, >= 0", name);
        if (o->type == YV_SIZE) { d->window_w = (int32_t)n[0]; d->window_h = (int32_t)n[1]; }
        else {
            d->mode.w = (int32_t)n[0];
            d->mode.h = (int32_t)n[1];
            d->mode.refresh_num = (int32_t)n[2];
            d->mode.refresh_den = (int32_t)n[3];
        }
        return;
    }
    if (yv_opt_num(d, o, vx_scalar(v, name), err, sizeof err) < 0) vx_err("arg", "%s", err);
}

/* The open options that are not screen desc fields: gfx (the gfx options;
 * its programs compile while the screen settles) and frame ('struct' or
 * 'row': what begin returns). 1 when name is one of them. */
static int vx_option_extra(const char* name, const mxArray* v, const mxArray** gfx, int* row) {
    if (!strcmp(name, "gfx")) { *gfx = v; return 1; }
    if (!strcmp(name, "frame")) {
        char s[16];
        vx_string(v, s, sizeof s, "frame");
        if (!strcmp(s, "row")) *row = 1;
        else if (!strcmp(s, "struct")) *row = 0;
        else vx_err("arg", "frame: 'struct' (the default) or 'row' ([onset period index vblank n_done done_lost])");
        return 1;
    }
    return 0;
}

/* A struct, or name-value pairs, from prhs[first] (vx_option_extra). */
static void vx_options(yscr_desc* d, int nrhs, const mxArray* prhs[], int first, const mxArray** gfx, int* row) {
    char name[64];
    int i, k;
    *gfx = NULL;
    *row = 0;
    if (nrhs > first && mxIsStruct(prhs[first])) {
        if (mxGetNumberOfElements(prhs[first]) != 1) vx_err("arg", "open: a 1x1 struct of options");
        if (nrhs > first + 1) vx_err("usage", "open: a struct, or name-value pairs, not both");
        for (k = 0; k < mxGetNumberOfFields(prhs[first]); k++) {
            const char* fn = mxGetFieldNameByNumber(prhs[first], k);
            if (vx_option_extra(fn, mxGetFieldByNumber(prhs[first], 0, k), gfx, row)) continue;
            vx_option(d, fn, mxGetFieldByNumber(prhs[first], 0, k));
        }
        return;
    }
    if ((nrhs - first) % 2) vx_err("usage", "open: name-value pairs");
    for (i = first; i < nrhs; i += 2) {
        vx_string(prhs[i], name, sizeof name, "an option name");
        if (vx_option_extra(name, prhs[i + 1], gfx, row)) continue;
        vx_option(d, name, prhs[i + 1]);
    }
}

static struct yv_gfx* vx_gstart(const mxArray* opts);   /* ysp_vis_gfx.inc */

/* ---- screen commands -------------------------------------------------------------- */

static yv_screen* vx_scr(const mxArray* a) {
    yv_screen* v = (yv_screen*)vx_lookup(a, VX_SCREEN, "screen");
    if (!v->open) vx_err("closed", "the screen is closed");
    return v;
}

static void vx_fail(yv_screen* v, int rc) {
    const char* m = yscr_error(&v->s);
    vx_err(vx_code(rc), "%s", m && m[0] ? m : yscr_strerror(rc));
}

static mxArray* vx_abort(const yscr_frame* f) {
    static const char* fn[] = { "reasons", "presses", "t", "t_ns", "panic" };
    mxArray* s = mxCreateStructMatrix(1, 1, 5, fn);
    mxSetFieldByNumber(s, 0, 0, vx_d(f->abort));
    mxSetFieldByNumber(s, 0, 1, vx_d(f->abort_presses));
    mxSetFieldByNumber(s, 0, 2, vx_d(yv_s(f->abort_ns)));
    mxSetFieldByNumber(s, 0, 3, vx_i64(f->abort_ns));
    mxSetFieldByNumber(s, 0, 4, mxCreateLogicalScalar((f->abort & YSCR_ABORT_PANIC) != 0));
    return s;
}

typedef struct { const char* name; double v; } vx_const;
static const vx_const vx_consts[] = {
    { "PATH_UNKNOWN", YSCR_PATH_UNKNOWN }, { "PATH_COMPOSED", YSCR_PATH_COMPOSED }, { "PATH_OVERLAY", YSCR_PATH_OVERLAY },
    { "PATH_INDEPENDENT", YSCR_PATH_INDEPENDENT }, { "PATH_SIMULATED", YSCR_PATH_SIMULATED },
    { "TIER_UNKNOWN", YSCR_TIER_UNKNOWN }, { "TIER_1", YSCR_TIER_1 }, { "TIER_2", YSCR_TIER_2 }, { "TIER_3", YSCR_TIER_3 },
    { "TIER_SIM", YSCR_TIER_SIM },
    { "FLIP_ESTIMATED", YSCR_FLIP_ESTIMATED }, { "FLIP_LATE_TARGET", YSCR_FLIP_LATE_TARGET },
    { "FLIP_OCCLUDED", YSCR_FLIP_OCCLUDED }, { "FLIP_GRID_UNSTABLE", YSCR_FLIP_GRID_UNSTABLE },
    { "FLIP_EARLY", YSCR_FLIP_EARLY }, { "FLIP_SKIPPED", YSCR_FLIP_SKIPPED }, { "FLIP_CANCELED", YSCR_FLIP_CANCELED },
    { "FLIP_ONSET_PLANNED", YSCR_FLIP_ONSET_PLANNED }, { "FLIP_BELOW_TIER", YSCR_FLIP_BELOW_TIER },
    { "FLIP_UNSYNCED", YSCR_FLIP_UNSYNCED },
    { "ABORT_KEY", YSCR_ABORT_KEY }, { "ABORT_CLOSE", YSCR_ABORT_CLOSE }, { "ABORT_ALT_F4", YSCR_ABORT_ALT_F4 },
    { "ABORT_QUIT", YSCR_ABORT_QUIT }, { "ABORT_REQUEST", YSCR_ABORT_REQUEST }, { "ABORT_PANIC", YSCR_ABORT_PANIC },
    { "SETTLED", YSCR_SETTLED }, { "SETTLE_FAILED", YSCR_SETTLE_FAILED }, { "SETTLE_SKIPPED", YSCR_SETTLE_SKIPPED },
    { "SETTLE_ABORTED", YSCR_SETTLE_ABORTED }, { "PANIC_EXIT_CODE", YSCR_PANIC_EXIT_CODE },
    { "KIND_KEYBOARD", YIN_KIND_KEYBOARD }, { "KIND_MOUSE", YIN_KIND_MOUSE }, { "KIND_TOUCH", YIN_KIND_TOUCH },
    { "KIND_PEN", YIN_KIND_PEN }, { "KIND_GAMEPAD", YIN_KIND_GAMEPAD },
    { "PRESS", YIN_PRESS }, { "RELEASE", YIN_RELEASE }, { "SAMPLE", YIN_SAMPLE }, { "PROXIMITY", YIN_PROXIMITY },
    { "REPEAT", YIN_REPEAT }, { "SYNTHETIC", YIN_SYNTHETIC },
};

static const yv_name vx_phases[] = {
    { "evaluate", YSCR_PHASE_EVALUATE }, { "script", YSCR_PHASE_SCRIPT }, { "draw", YSCR_PHASE_DRAW },
    { "upload", YSCR_PHASE_UPLOAD }, { NULL, 0 } };

static void vx_window(yv_screen* v, void (*fn)(void*), const char* text, int on) {
    int rc = yv_window(v, fn, text, on);
    if (rc == YSCR_ERR_NOT_IMPLEMENTED) vx_err("not_implemented", "this screen has no window (sim backend)");
    if (rc == YSCR_ERR_LOST) vx_err("lost", "SDL refused: %s", SDL_GetError());
    if (rc < 0) vx_fail(v, rc);
}

static void vx_screen_cmd(const char* cmd, int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    (void)nlhs;
    if (!strcmp(cmd, "version")) {
        plhs[0] = mxCreateString(YV_MEX_VERSION);
        if (nlhs > 1) plhs[1] = mxCreateString(YSCR_VERSION_STRING);
    } else if (!strcmp(cmd, "open")) {
        yscr_desc d;
        yv_screen* v;
        uint64_t id;
        const mxArray* gfx;
        struct yv_gfx* gs = NULL;
        int row;
        memset(&d, 0, sizeof d);
        vx_options(&d, nrhs, prhs, 1, &gfx, &row);
        if (gfx && !mxIsEmpty(gfx) && !(mxIsLogical(gfx) && !mxIsLogicalScalarTrue(gfx))) gs = vx_gstart(gfx);
        v = (yv_screen*)calloc(1, sizeof *v);
        if (!v) { free(gs); vx_err("memory", "out of memory"); }
        v->gstart = gs;
        v->frame_row = row;
        if (!yv_open(v, &d)) {
            char msg[1024];
            snprintf(msg, sizeof msg, "%s", v->rec ? yscr_error(&v->s) : "out of memory");
            yv_free(v);
            vx_err("open", "%s", msg);
        }
        id = vx_register(VX_SCREEN, v);
        if (!id) { yv_free(v); vx_err("memory", "out of memory"); }
        plhs[0] = vx_handle_out(id);
    } else if (!strcmp(cmd, "close")) {
        yv_screen* v;
        vx_nargs(nrhs, 2, "ysp_screen('close', h)");
        v = (yv_screen*)vx_lookup(prhs[1], VX_SCREEN, "screen");
        /* the gfx on it closes with it; its handle and its objects' go too */
        if (v->gfx) vx_gfx_drop(v->gfx);
        yv_close(v);
        /* The records not read yet, with those close() completed. */
        if (nlhs > 0) {
            int n = (int)v->rec_n, k = 0;
            plhs[0] = mxCreateStructMatrix(1, (size_t)n, 17, vx_record_f);
            while (k < n) {
                yscr_record r;
                yv_records(v, &r, 1);
                vx_record_set(plhs[0], (size_t)k++, &r);
            }
        }
        vx_unregister(v);
        yv_free(v);
    } else if (!strcmp(cmd, "begin")) {
        yv_screen* v;
        int rc;
        /* [f, quit, done] = ysp_screen('begin', h): f without records (a
         * record cost 4.5 us in MATLAB and 45 us in Octave in phase A's
         * frame); done, the records completed since the last begin, only
         * when asked (a third output). */
        vx_nargs(nrhs, 2, "[f, quit, done] = ysp_screen('begin', h)");
        v = vx_scr(prhs[1]);
        rc = yv_begin(v);
        if (rc == YSCR_QUIT) {
            if (nlhs < 2)
                vx_err("abort", "abort 0x%x (%d press%s)%s; [f, quit] = ysp_screen('begin', h) returns it instead",
                       (unsigned)v->f.abort, (int)v->f.abort_presses, v->f.abort_presses == 1 ? "" : "es",
                       (v->f.abort & YSCR_ABORT_PANIC) ? ": the panic released the display, only 'close' is left" : "");
            plhs[0] = mxCreateDoubleMatrix(0, 0, mxREAL);
            plhs[1] = vx_abort(&v->f);
            if (nlhs > 2) plhs[2] = v->frame_row ? vx_records_matrix(v->f.done, v->f.n_done) : vx_records(v->f.done, v->f.n_done);
            return;
        }
        if (rc < 0) vx_fail(v, rc);
        plhs[0] = v->frame_row ? vx_frame_row(&v->f) : vx_frame_slim(&v->f);
        if (nlhs > 1) plhs[1] = mxCreateDoubleMatrix(0, 0, mxREAL);
        if (nlhs > 2) plhs[2] = v->frame_row ? vx_records_matrix(v->f.done, v->f.n_done) : vx_records(v->f.done, v->f.n_done);
    } else if (!strcmp(cmd, "_frame")) {
        /* test: the last frame converted as form k, with no begin: what
         * each form of the result costs (0 phase A's struct with done and
         * last, 1 the struct without records, 2 a row, 3 the records, 4 the
         * records as a matrix) */
        yv_screen* v;
        int k;
        vx_nargs(nrhs, 3, "f = ysp_screen('_frame', h, k)");
        v = vx_scr(prhs[1]);
        k = (int)vx_scalar(prhs[2], "k");
        plhs[0] = k == 0   ? vx_frame(&v->f)
                  : k == 1 ? vx_frame_slim(&v->f)
                  : k == 2 ? vx_frame_row(&v->f)
                  : k == 4 ? vx_records_matrix(v->f.done, v->f.n_done)
                  : vx_records(v->f.done, v->f.n_done);
    } else if (!strcmp(cmd, "flip") || !strcmp(cmd, "flip_at")) {
        yv_screen* v;
        int at = cmd[4] == '_', rc;
        int64_t t = 0;
        vx_nargs(nrhs, at ? 3 : 2, at ? "ysp_screen('flip_at', h, t)" : "ysp_screen('flip', h)");
        v = vx_scr(prhs[1]);
        if (at) t = vx_seconds(prhs[2], "flip_at: t", 0);
        if (v->gfx && v->gfx->in_frame) vx_err("order", "ysp_gfx('end', g) before the flip");
        rc = yv_flip_at(v, t, at);
        if (rc < 0) vx_fail(v, rc);
    } else if (!strcmp(cmd, "wait_flip")) {
        yv_screen* v;
        yscr_record r;
        int rc;
        vx_nargs(nrhs, 2, "r = ysp_screen('wait_flip', h)");
        v = vx_scr(prhs[1]);
        rc = yscr_wait_flip(&v->s, &r);
        if (rc < 0) vx_fail(v, rc);
        plhs[0] = vx_records(&r, 1);
    } else if (!strcmp(cmd, "mark")) {
        yv_screen* v;
        char ph[16];
        int p;
        vx_nargs(nrhs, 3, "ysp_screen('mark', h, 'evaluate' | 'script' | 'draw' | 'upload')");
        v = vx_scr(prhs[1]);
        vx_string(prhs[2], ph, sizeof ph, "mark: the phase");
        if (yv_name_value(vx_phases, ph, &p) < 0) vx_err("arg", "mark: 'evaluate', 'script', 'draw' or 'upload'");
        yscr_mark(&v->s, p);
    } else if (!strcmp(cmd, "records")) {
        yv_screen* v;
        mxArray* s;
        int n, k = 0;
        vx_nargs(nrhs, 2, "r = ysp_screen('records', h)");
        v = (yv_screen*)vx_lookup(prhs[1], VX_SCREEN, "screen");
        n = (int)v->rec_n;
        if (v->frame_row) {
            /* a frame='row' screen: the records as one matrix */
            yscr_record* buf = (yscr_record*)mxMalloc(sizeof(yscr_record) * (size_t)(n > 0 ? n : 1));
            yv_records(v, buf, n);
            plhs[0] = vx_records_matrix(buf, n);
            mxFree(buf);
            if (nlhs > 1) plhs[1] = vx_d((double)v->rec_lost);
            return;
        }
        s = mxCreateStructMatrix(1, (size_t)n, 17, vx_record_f);
        while (k < n) {
            yscr_record r;
            yv_records(v, &r, 1);
            vx_record_set(s, (size_t)k++, &r);
        }
        plhs[0] = s;
        if (nlhs > 1) plhs[1] = vx_d((double)v->rec_lost);
    } else if (!strcmp(cmd, "poll")) {
        static yin_event buf[YV_EVENTS];   /* one MATLAB thread: no allocation per poll */
        yv_screen* v;
        int n, max = YV_EVENTS;
        vx_nargs(nrhs, 2, "e = ysp_screen('poll', h)");
        v = vx_scr(prhs[1]);
        if (nrhs > 2) {
            double m = vx_scalar(prhs[2], "poll: max");
            if (m >= 1 && m < YV_EVENTS) max = (int)m;
        }
        yv_drain(v);
        n = yv_events(v, buf, max);
        plhs[0] = vx_events(buf, n);
        if (nlhs > 1) plhs[1] = vx_d((double)v->ev_lost);
    } else if (!strcmp(cmd, "title")) {
        char t[512];
        vx_nargs(nrhs, 3, "ysp_screen('title', h, text)");
        vx_string(prhs[2], t, sizeof t, "title");
        vx_window(vx_scr(prhs[1]), yv_w_title, t, 0);
    } else if (!strcmp(cmd, "cursor")) {
        vx_nargs(nrhs, 3, "ysp_screen('cursor', h, show)");
        vx_window(vx_scr(prhs[1]), yv_w_cursor, NULL, vx_bool(prhs[2], "cursor: show"));
    } else if (!strcmp(cmd, "grab")) {
        vx_nargs(nrhs, 3, "ysp_screen('grab', h, on)");
        vx_window(vx_scr(prhs[1]), yv_w_grab, NULL, vx_bool(prhs[2], "grab: on"));
    } else if (!strcmp(cmd, "describe")) {
        char line[2048];
        vx_nargs(nrhs, 2, "s = ysp_screen('describe', h)");
        yscr_describe(&vx_scr(prhs[1])->s, line, sizeof line);
        plhs[0] = mxCreateString(line);
    } else if (!strcmp(cmd, "caps")) {
        static const char* f[] = { "kind", "backend", "period", "period_ns", "mode", "native_target", "hw_onset",
                                   "vrr_capable", "max_in_flight", "worst_tier", "raw_keyboard" };
        yscr_caps c;
        mxArray* s;
        vx_nargs(nrhs, 2, "c = ysp_screen('caps', h)");
        yscr_get_caps(&vx_scr(prhs[1])->s, &c);
        s = mxCreateStructMatrix(1, 1, 11, f);
        mxSetFieldByNumber(s, 0, 0, mxCreateString(c.kind == YSCR_FIXED_GRID ? "fixed_grid" : c.kind == YSCR_CONTINUOUS ? "continuous" : "callback"));
        mxSetFieldByNumber(s, 0, 1, mxCreateString(yv_value_name(yv_backends, c.backend)));
        mxSetFieldByNumber(s, 0, 2, vx_d(yv_s(c.period_ns)));
        mxSetFieldByNumber(s, 0, 3, vx_i64(c.period_ns));
        mxSetFieldByNumber(s, 0, 4, vx_mode(&c.mode));
        mxSetFieldByNumber(s, 0, 5, mxCreateLogicalScalar(c.native_target));
        mxSetFieldByNumber(s, 0, 6, mxCreateLogicalScalar(c.hw_onset));
        mxSetFieldByNumber(s, 0, 7, mxCreateLogicalScalar(c.vrr_capable));
        mxSetFieldByNumber(s, 0, 8, vx_d(c.max_in_flight));
        mxSetFieldByNumber(s, 0, 9, vx_d(c.worst_tier));
        mxSetFieldByNumber(s, 0, 10, mxCreateLogicalScalar(c.raw_keyboard));
        plhs[0] = s;
    } else if (!strcmp(cmd, "settle")) {
        static const char* f[] = { "result", "mode", "source", "condition", "time", "flips", "run", "need",
                                   "foreground", "min", "max", "spread_sd_ns", "spread_mean_ns", "hook", "message" };
        yscr_settle_info i;
        mxArray* s;
        vx_nargs(nrhs, 2, "s = ysp_screen('settle', h)");
        yscr_settle_check(&vx_scr(prhs[1])->s, &i);
        s = mxCreateStructMatrix(1, 1, 15, f);
        mxSetFieldByNumber(s, 0, 0, vx_d(i.result));
        mxSetFieldByNumber(s, 0, 1, vx_d(i.mode));
        mxSetFieldByNumber(s, 0, 2, vx_d(i.source));
        mxSetFieldByNumber(s, 0, 3, vx_d(i.condition));
        mxSetFieldByNumber(s, 0, 4, vx_d(yv_s(i.ns)));
        mxSetFieldByNumber(s, 0, 5, vx_d(i.flips));
        mxSetFieldByNumber(s, 0, 6, vx_d(i.run));
        mxSetFieldByNumber(s, 0, 7, vx_d(i.need));
        mxSetFieldByNumber(s, 0, 8, vx_d(i.foreground));
        mxSetFieldByNumber(s, 0, 9, vx_d(yv_s(i.min_ns)));
        mxSetFieldByNumber(s, 0, 10, vx_d(yv_s(i.max_ns)));
        mxSetFieldByNumber(s, 0, 11, vx_i64(i.spread_sd_ns));
        mxSetFieldByNumber(s, 0, 12, vx_i64(i.spread_mean_ns));
        mxSetFieldByNumber(s, 0, 13, vx_d(yv_s(i.hook_ns)));
        mxSetFieldByNumber(s, 0, 14, mxCreateString(i.message));
        plhs[0] = s;
    } else if (!strcmp(cmd, "sync")) {
        static const char* f[] = { "untimed", "rule", "fired_index", "window", "evidence", "peak", "observed", "message" };
        yscr_sync_info i;
        mxArray* s;
        vx_nargs(nrhs, 2, "s = ysp_screen('sync', h)");
        yscr_sync_check(&vx_scr(prhs[1])->s, &i);
        s = mxCreateStructMatrix(1, 1, 8, f);
        mxSetFieldByNumber(s, 0, 0, mxCreateLogicalScalar(i.untimed != 0));
        mxSetFieldByNumber(s, 0, 1, vx_d(i.rule));
        mxSetFieldByNumber(s, 0, 2, vx_d((double)i.fired_index));
        mxSetFieldByNumber(s, 0, 3, vx_d(i.window));
        mxSetFieldByNumber(s, 0, 4, vx_d(i.evidence));
        mxSetFieldByNumber(s, 0, 5, vx_d(i.peak));
        mxSetFieldByNumber(s, 0, 6, vx_d(i.observed));
        mxSetFieldByNumber(s, 0, 7, mxCreateString(i.message));
        plhs[0] = s;
    } else if (!strcmp(cmd, "set_patch")) {
        double x;
        vx_nargs(nrhs, 3, "ysp_screen('set_patch', h, v)");
        x = vx_scalar(prhs[2], "set_patch: v");
        if (!(x == x)) vx_err("arg", "set_patch: 0..1, not NaN");
        yscr_set_patch(&vx_scr(prhs[1])->s, (float)x);
    } else if (!strcmp(cmd, "displays")) {
        static const char* f[] = { "id", "name", "x", "y", "w", "h", "primary", "desktop" };
        yscr_display_info d[16];
        int n = yscr_displays(d, 16), i;
        mxArray* s;
        if (n < 0) vx_err(vx_code(n), "%s", yscr_strerror(n));
        if (n > 16) n = 16;
        s = mxCreateStructMatrix(1, (size_t)n, 8, f);
        for (i = 0; i < n; i++) {
            mxSetFieldByNumber(s, i, 0, vx_d(d[i].id));
            mxSetFieldByNumber(s, i, 1, mxCreateString(d[i].name));
            mxSetFieldByNumber(s, i, 2, vx_d(d[i].x));
            mxSetFieldByNumber(s, i, 3, vx_d(d[i].y));
            mxSetFieldByNumber(s, i, 4, vx_d(d[i].w));
            mxSetFieldByNumber(s, i, 5, vx_d(d[i].h));
            mxSetFieldByNumber(s, i, 6, mxCreateLogicalScalar(d[i].primary));
            mxSetFieldByNumber(s, i, 7, vx_mode(&d[i].desktop));
        }
        plhs[0] = s;
    } else if (!strcmp(cmd, "modes")) {
        yscr_mode* md = (yscr_mode*)mxMalloc(sizeof(yscr_mode) * 256);
        uint32_t disp = nrhs > 1 ? (uint32_t)vx_scalar(prhs[1], "modes: display") : 0;
        int n = yscr_modes(disp, md, 256), i;
        mxArray* s;
        if (n < 0) vx_err(vx_code(n), "%s", yscr_strerror(n));
        if (n > 256) n = 256;
        s = mxCreateStructMatrix(1, (size_t)n, 9, vx_mode_f);
        for (i = 0; i < n; i++) vx_mode_set(s, (size_t)i, &md[i]);
        mxFree(md);
        plhs[0] = s;
    } else if (!strcmp(cmd, "mode_multiple")) {
        yscr_mode out;
        double err = 0.0, wh[2] = { 0, 0 };
        int32_t num, den = 1;
        double tol = 0.0;
        uint32_t disp = 0;
        int k;
        vx_nargs(nrhs, 2, "[k, mode, err_ppm] = ysp_screen('mode_multiple', num, den, [w h], tol_ppm, display)");
        num = (int32_t)vx_scalar(prhs[1], "num");
        if (nrhs > 2) den = (int32_t)vx_scalar(prhs[2], "den");
        if (nrhs > 3 && mxGetNumberOfElements(prhs[3]) == 2 && mxIsDouble(prhs[3])) { wh[0] = mxGetPr(prhs[3])[0]; wh[1] = mxGetPr(prhs[3])[1]; }
        if (nrhs > 4) tol = vx_scalar(prhs[4], "tol_ppm");
        if (nrhs > 5) disp = (uint32_t)vx_scalar(prhs[5], "display");
        memset(&out, 0, sizeof out);
        k = yscr_mode_multiple(disp, num, den, (int32_t)wh[0], (int32_t)wh[1], tol, &out, &err);
        if (k < 0 && k != YSCR_ERR_REFUSED) vx_err(vx_code(k), "%s", yscr_strerror(k));
        plhs[0] = vx_d(k < 0 ? 0 : k);
        if (nlhs > 1) plhs[1] = vx_mode(&out);
        if (nlhs > 2) plhs[2] = vx_d(err);
    } else if (!strcmp(cmd, "options")) {
        int n = 0, i;
        const char* names[64];
        mxArray* s;
        for (i = 0; yv_opts[i].name && n < 64; i++) names[n++] = yv_opts[i].name;
        s = mxCreateStructMatrix(1, 1, n, names);
        for (i = 0; i < n; i++) mxSetFieldByNumber(s, 0, i, mxCreateString(yv_opts[i].doc));
        plhs[0] = s;
    } else if (!strcmp(cmd, "constants")) {
        int n = (int)(sizeof vx_consts / sizeof vx_consts[0]), i;
        const char* names[64];
        mxArray* s;
        for (i = 0; i < n; i++) names[i] = vx_consts[i].name;
        s = mxCreateStructMatrix(1, 1, n, names);
        for (i = 0; i < n; i++) mxSetFieldByNumber(s, 0, i, vx_d(vx_consts[i].v));
        plhs[0] = s;
    } else if (!strcmp(cmd, "request_abort")) {
        yscr_request_abort();
    } else if (!strcmp(cmd, "input_stats")) {
        static const char* f[] = { "stored", "overwritten", "refused", "pending", "lost", "key_doubles", "key_double_ups" };
        yscr_input_stats st;
        mxArray* s = mxCreateStructMatrix(1, 1, 7, f);
        yscr_get_input_stats(&st);
        mxSetFieldByNumber(s, 0, 0, vx_d(st.stored));
        mxSetFieldByNumber(s, 0, 1, vx_d(st.overwritten));
        mxSetFieldByNumber(s, 0, 2, vx_d(st.refused));
        mxSetFieldByNumber(s, 0, 3, vx_d(st.pending));
        mxSetFieldByNumber(s, 0, 4, vx_d(st.lost));
        mxSetFieldByNumber(s, 0, 5, vx_d(st.key_doubles));
        mxSetFieldByNumber(s, 0, 6, vx_d(st.key_double_ups));
        plhs[0] = s;
    } else if (!strcmp(cmd, "now")) {
        plhs[0] = vx_d(yv_s(yrt_now_ns()));
    } else if (!strcmp(cmd, "now_ns")) {
        plhs[0] = vx_i64(yrt_now_ns());
    } else {
        vx_err("usage", "unknown command '%s'", cmd);
    }
}

/* ---- gfx commands: stimuli, lists, groups, textures, bindings (ysp_vis_gfx.inc) ---------- */

#include "ysp_vis_gfx.inc"

/* ---- video commands -------------------------------------------------------------------- */

static mxArray* vx_shared(void) {
    static const char* f[] = { "abi", "audio", "audio_built", "timeline", "timeline_built", "channels" };
    char err[512];
    mxArray* s;
    if (ysp_audio_api_import_mex(err, sizeof err) < 0) vx_err("shared", "%s", err);
    if (ysp_timeline_api_import_mex(err, sizeof err) < 0) vx_err("shared", "%s", err);
    s = mxCreateStructMatrix(1, 1, 6, f);
    mxSetFieldByNumber(s, 0, 0, vx_d(ysp_audio_api_ptr->head.abi));
    mxSetFieldByNumber(s, 0, 1, mxCreateString(yau_version()));
    mxSetFieldByNumber(s, 0, 2, mxCreateString(YAU_VERSION_STRING));
    mxSetFieldByNumber(s, 0, 3, mxCreateString(ytl_version()));
    mxSetFieldByNumber(s, 0, 4, mxCreateString(YTL_VERSION_STRING));
    mxSetFieldByNumber(s, 0, 5, vx_d(ysp_timeline_api_ptr->max_channels));
    return s;
}

#include "ysp_vis_video.inc"   /* ysp_video: movies, the index maker, frame sequences */

/* ---- the core itself ------------------------------------------------------------- */

static void vx_vis_cmd(const char* cmd, int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    (void)nrhs;
    (void)prhs;
    if (!strcmp(cmd, "version")) {
        plhs[0] = mxCreateString(YV_MEX_VERSION);
        if (nlhs > 1) plhs[1] = mxCreateString(YSCR_VERSION_STRING);
    } else if (!strcmp(cmd, "info")) {
        static const char* f[] = { "version", "users", "screens", "gfx", "module_dir", "audio", "timeline",
                                   "screen_h", "gfx_h", "video_h", "sdl" };
        mxArray* s = mxCreateStructMatrix(1, 1, 11, f);
        vx_node* n;
        int ns = 0, ng = 0, v = SDL_GetVersion();
        char sdl[32];
        for (n = vx_list; n; n = n->next) { if (n->kind == VX_SCREEN) ns++; else if (n->kind == VX_GFX) ng++; }
        snprintf(sdl, sizeof sdl, "%d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
        mxSetFieldByNumber(s, 0, 0, mxCreateString(YV_MEX_VERSION));
        mxSetFieldByNumber(s, 0, 1, vx_d(vx_users));
        mxSetFieldByNumber(s, 0, 2, vx_d(ns));
        mxSetFieldByNumber(s, 0, 3, vx_d(ng));
        mxSetFieldByNumber(s, 0, 4, mxCreateString(yv_module_dir()));
        mxSetFieldByNumber(s, 0, 5, mxCreateString(ysp_audio_api_ptr ? ysp_audio_api_ptr->head.version : ""));
        mxSetFieldByNumber(s, 0, 6, mxCreateString(ysp_timeline_api_ptr ? ysp_timeline_api_ptr->head.version : ""));
        mxSetFieldByNumber(s, 0, 7, mxCreateString(YSCR_VERSION_STRING));
        mxSetFieldByNumber(s, 0, 8, mxCreateString(YGFX_VERSION_STRING));
        mxSetFieldByNumber(s, 0, 9, mxCreateString(YVID_VERSION_STRING));
        mxSetFieldByNumber(s, 0, 10, mxCreateString(sdl));
        plhs[0] = s;
    } else {
        vx_err("usage", "ysp_vis is the core of ysp_screen, ysp_gfx and ysp_video: 'version' and 'info' only");
    }
}

static void vx_dispatch(int which, int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char cmd[32];
    static const char* mods[] = { "ysp_vis", "ysp_screen", "ysp_gfx", "ysp_video" };
    vx_mod = which >= 0 && which <= 3 ? mods[which] : "ysp_vis";
    if (nrhs < 1 || !mxIsChar(prhs[0]) || mxGetString(prhs[0], cmd, sizeof cmd) != 0)
        vx_err("usage", "%s(command, ...): the first argument is a command", vx_mod);
    switch (which) {
    case YV_WHICH_SCREEN: vx_screen_cmd(cmd, nlhs, plhs, nrhs, prhs); break;
    case YV_WHICH_GFX: vx_gfx_cmd(cmd, nlhs, plhs, nrhs, prhs); break;
    case YV_WHICH_VIDEO: vx_video_cmd(cmd, nlhs, plhs, nrhs, prhs); break;
    default: vx_vis_cmd(cmd, nlhs, plhs, nrhs, prhs); break;
    }
}

static const yv_core vx_core = { YV_CORE_ABI, sizeof(yv_core), YV_MEX_VERSION, vx_attach, vx_detach, vx_dispatch };

YSP_SHARE_EXPORT const yv_core* ysp_vis_core(void) { return &vx_core; }

/* Called itself: 'version' and 'info'. No exit hook here: the thin files'
 * hooks own the lifetime (ysp_vis_mex.h). */
YSP_SHARE_MEX_ENTRY void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    vx_dispatch(YV_WHICH_VIS, nlhs, plhs, nrhs, prhs);
}
