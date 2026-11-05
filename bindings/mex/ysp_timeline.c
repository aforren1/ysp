/* ysp_timeline.c - MATLAB/Octave MEX binding for ysp/timeline.h (builds to ysp_timeline.<mexext>)
 *
 * Events, tracks and tweens on a frame clock, for a Psychtoolbox script that
 * draws its own stimuli: build a trial, then each frame advance it to the
 * frame's predicted onset and read the channel values, the fields and the
 * events that landed. Command dispatch in the style of the other ysp MEX
 * files.
 *
 *     h = ysp_timeline('open', struct('channels', 3));        % FIX, GRATING, CONTRAST
 *     q = ysp_timeline('seq', h, 1);                           % base 1: the trial
 *     ysp_timeline('on', q, 1); ysp_timeline('wait', q, 0.5); ysp_timeline('off', q, 1);
 *     ysp_timeline('on', q, 2); ysp_timeline('trigger', q, 12);
 *     ysp_timeline('to', q, 3, 0.5, 'duration', 0.1, 'ease', 'cosine');
 *     ysp_timeline('anchor', h, 1, vbl + ifi);                 % trial time 0
 *     [v, ev] = ysp_timeline('evaluate', h, vbl + ifi, ifi);   % each frame
 *
 * TIMES are seconds (double) and convert once to the nearest ns; a negative
 * or NaN time is refused. A landing record carries the exact ns as int64
 * beside the seconds.
 *
 * INDICES: channels and fields are 1-based, as in ysp_mex_util.h. A base
 * keeps the header's number (0 is the RT clock, 1 to 7 are yours), and a
 * trigger channel keeps its value (0 by default): it names one of the
 * caller's outputs, as the header's report does.
 *
 * SHARING: the exported function ysp_timeline_api_table() returns the table of
 * bindings/share/ysp_timeline_api.h. Its acquire() takes a pointer to a
 * timeline handle (uint64) and pins the timeline; 'close' refuses a pinned
 * one (ysp_timeline:busy), and a cleared handle table leaves it in place
 * until the last release().
 *
 * Build with build.m. See README.md for the command reference.
 */
#define PM_MOD "ysp_timeline"
#include "ysp_mex_util.h"

/* As the Python binding: a channel per dot or grid element needs more than
 * the header's 256; an evaluate scans only n_channels. */
#define YTL_MAX_CHANNELS 1024
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"
/* The table ysp-vis calls through (bindings/share/ysp_timeline_api.h). */
#define YSP_TIMELINE_API_OWNER
#include "ysp_timeline_api.h"

#include <stddef.h>

/* The binding's own version (README.md, "Versions"); the second output of
 * 'version' is the header's. */
#define TLM_VERSION "1.1.0"   /* 1.1.0: the shared table, 'pins', busy */
#define TLM_S_MAX 4611686018.0

enum { TLM_TIMELINE = 1, TLM_SEQ = 2 };

typedef struct tlm_tl {
    int              kind;        /* TLM_TIMELINE */
    ytl_timeline     tl;
    ytl_event*       events;
    ytl_event*       fired;
    int              ecap;
    ytl_key*         arena;
    ytl_target_slot* tg;
    ytl_tween_slot*  tw;
    float*           fields;      /* 4 words per slot */
    uint8_t*         fused;
    int              fcap;
    void*            cbuf[YTL_MAX_CHANNELS];   /* the arrays a track points at */
    int8_t           cbase[YTL_MAX_CHANNELS];
    int64_t          frames;
    int              pins;        /* acquire() calls not yet released */
    int              orphan;      /* the table was cleared while pinned */
} tlm_tl;

typedef struct tlm_seq {
    int      kind;   /* TLM_SEQ */
    uint64_t tl_id;
    tlm_tl*  t;      /* valid while tl_id is in the table */
    ytl_seq  q;
} tlm_seq;

static void tlm_destroy(void* obj) {
    int i;
    tlm_tl* t = (tlm_tl*)obj;
    if (!obj) return;
    /* A pinned timeline stays for the module that holds it (SHARING). */
    if (t->kind == TLM_TIMELINE && t->pins > 0) { t->orphan = 1; return; }
    if (t->kind == TLM_TIMELINE) {
        ytl_close(&t->tl);
        for (i = 0; i < YTL_MAX_CHANNELS; i++) free(t->cbuf[i]);
        free(t->events); free(t->fired); free(t->arena); free(t->tg); free(t->tw); free(t->fields); free(t->fused);
    }
    free(obj);
}

/* ---- arguments ---------------------------------------------------------- */

static uint64_t tlm_id(const mxArray* a) {
    if (mxIsUint64(a) && mxGetNumberOfElements(a) == 1) return *(const uint64_t*)mxGetData(a);
    if (mxIsDouble(a) && !mxIsComplex(a) && mxGetNumberOfElements(a) == 1 && mxGetScalar(a) >= 1)
        return (uint64_t)mxGetScalar(a);
    pm_err("handle", "handle must be a scalar uint64");
    return 0;
}

static void* tlm_find(uint64_t id) {
    pm_node* n;
    for (n = pm_list; n; n = n->next)
        if (n->id == id) return n->obj;
    return NULL;
}

/* ---- sharing ----------------------------------------------------------------- */

static ysp_timeline_api g_api;

static void* tlm_api_acquire(void* ref, char* err, size_t cap) {
    tlm_tl* t = ref ? (tlm_tl*)tlm_find(*(const uint64_t*)ref) : NULL;
    if (!t || t->kind != TLM_TIMELINE) { snprintf(err, cap, "ysp_timeline: not an open timeline handle"); return NULL; }
    t->pins++;
    return &t->tl;
}

static void tlm_api_release(void* p) {
    tlm_tl* t;
    if (!p) return;
    t = (tlm_tl*)(void*)((char*)p - offsetof(tlm_tl, tl));
    if (--t->pins == 0 && t->orphan) tlm_destroy(t);
}

YSP_SHARE_EXPORT const ysp_timeline_api* ysp_timeline_api_table(void) {
    if (!g_api.head.abi) ysp_timeline_api_fill(&g_api, YSP_SHARE_HOST_MEX, tlm_api_acquire, tlm_api_release);
    return &g_api;
}

static tlm_tl* tlm_get_tl(const mxArray* a) {
    tlm_tl* t = (tlm_tl*)pm_lookup(a);
    if (t->kind != TLM_TIMELINE) pm_err("handle", "a timeline handle is needed here, not a sequence");
    return t;
}

static tlm_seq* tlm_get_seq(const mxArray* a) {
    tlm_seq* s = (tlm_seq*)pm_lookup(a);
    if (s->kind != TLM_SEQ) pm_err("handle", "a sequence (from 'seq') is needed here");
    if (tlm_find(s->tl_id) != (void*)s->t) pm_err("closed", "the sequence's timeline is closed");
    return s;
}

static int64_t tlm_ns(double s, const char* what) {
    volatile double p;
    double e, n, r;
    if (!(s >= 0.0 && s < TLM_S_MAX))
        pm_err("arg", "%s: seconds, >= 0 and below 2^62 ns (a negative or NaN time is refused)", what);
    p = s * 1e9;
    e = fma(s, 1e9, -p);
    n = floor(p + 0.5);
    r = (p - n) + e;
    if (r > 0.5) n += 1.0;
    else if (r < -0.5) n -= 1.0;
    return (int64_t)n;
}

static int64_t tlm_time(const mxArray* a, const char* what) { return tlm_ns(pm_scalar(a, what), what); }
static double tlm_s(int64_t ns) { return (double)ns / 1e9; }

static mxArray* tlm_time_out(bool ok, int64_t ns) {
    return ok ? mxCreateDoubleScalar(tlm_s(ns)) : mxCreateDoubleMatrix(0, 0, mxREAL);
}

static void tlm_check(ytl_timeline* tl, int rc) {
    const char* m;
    if (rc >= 0) return;
    m = ytl_error(tl);
    if (!m || !m[0]) m = ytl_strerror(rc);
    switch (rc) {
    case YTL_ERR_ARG: pm_err("arg", "%s", m); break;
    case YTL_ERR_CLOSED: pm_err("closed", "%s", m); break;
    case YTL_ERR_ORDER: pm_err("order", "%s", m); break;
    case YTL_ERR_FULL: pm_err("full", "%s", m); break;
    case YTL_ERR_BOUND: pm_err("bound", "%s", m); break;
    case YTL_ERR_NOT_FOUND: pm_err("not_found", "%s", m); break;
    default: pm_err("error", "%s", m); break;
    }
}

static void tlm_seq_check(tlm_seq* s, const char* what) {
    if (s->q.err < 0) {
        char m[256];
        snprintf(m, sizeof m, "%s (call %d)", ytl_error(&s->t->tl), s->q.err_call);
        (void)what;
        switch (s->q.err) {
        case YTL_ERR_ARG: pm_err("arg", "%s", m); break;
        case YTL_ERR_FULL: pm_err("full", "%s", m); break;
        case YTL_ERR_BOUND: pm_err("bound", "%s", m); break;
        case YTL_ERR_CLOSED: pm_err("closed", "%s", m); break;
        default: pm_err("error", "%s", m); break;
        }
    }
}

/* A 1-based channel to the header's 0-based one. */
static int tlm_ch(const mxArray* a, const char* what) { return pm_int(a, what) - 1; }

static int tlm_base(const mxArray* a) {
    int b = pm_int(a, "base");
    if (b < YTL_ALL_BASES || b >= YTL_MAX_BASES) pm_err("arg", "base: 0 to 7 (0 is the RT clock), or -1 for all");
    return b;
}

static const char* const tlm_ease_names[] = { "linear", "step", "cosine", "quad_in", "quad_out", "log", "bezier",
                                              "quad_in_out", "cubic_in", "cubic_out", "cubic_in_out" };
static const char* const tlm_interp_names[] = { "linear", "step", "cubic" };
static const char* const tlm_kind_names[] = { "mark", "trigger", "onset", "offset", "set", "call_start",
                                              "call_complete", "target_set" };

/* An ease: a name, a number, or [x1 y1 x2 y2] (BEZIER with that curve). */
static int tlm_ease(const mxArray* a, ytl_curve* c) {
    if (mxIsNumeric(a) && mxGetNumberOfElements(a) == 4) {
        double v[4];
        pm_vector_n(a, "ease", v, 4);
        c->x1 = (float)v[0]; c->y1 = (float)v[1]; c->x2 = (float)v[2]; c->y2 = (float)v[3];
        return YTL_EASE_BEZIER;
    }
    if (mxIsNumeric(a)) return pm_int(a, "ease");
    return pm_enum(a, "ease", tlm_ease_names, 11);
}

static int tlm_cycles(const mxArray* a) {
    if (mxIsChar(a)) {
        char* s = pm_string(a, "cycles");
        if (pm_lower_eq(s, "forever")) return YTL_FOREVER;
        pm_err("arg", "cycles: a count, or 'forever'");
    }
    if (mxIsInf(pm_scalar(a, "cycles"))) return YTL_FOREVER;
    return pm_int(a, "cycles");
}

/* Name/value options of a tween. */
typedef struct tlm_tw {
    ytl_tween_desc d;
    int   from_given, to_current, immediate, overwrite, on_start, on_complete;
    float to4[4], from4[4];
    int   n_to, n_from;
    int64_t stagger;
    uint64_t user;
} tlm_tw;

enum { TW_CH = 1, TW_FIELD = 2, TW_START = 4, TW_TO = 8 };

static void tlm_tw_parse(int nrhs, const mxArray* prhs[], int first, int allow, tlm_tw* t) {
    int i;
    memset(t, 0, sizeof *t);
    t->immediate = -1;
    if ((nrhs - first) % 2) pm_err("usage", "tween options are name, value pairs");
    for (i = first; i < nrhs; i += 2) {
        char* k = pm_string(prhs[i], "option name");
        const mxArray* v = prhs[i + 1];
        if (pm_lower_eq(k, "duration")) t->d.duration_ns = tlm_time(v, "duration");
        else if (pm_lower_eq(k, "delay")) t->d.delay_ns = tlm_time(v, "delay");
        else if (pm_lower_eq(k, "ease")) t->d.ease = tlm_ease(v, &t->d.curve);
        else if (pm_lower_eq(k, "curve")) {
            double c[4];
            pm_vector_n(v, "curve", c, 4);
            t->d.curve.x1 = (float)c[0]; t->d.curve.y1 = (float)c[1]; t->d.curve.x2 = (float)c[2]; t->d.curve.y2 = (float)c[3];
        }
        else if (pm_lower_eq(k, "cycles")) t->d.cycles = tlm_cycles(v);
        else if (pm_lower_eq(k, "yoyo")) t->d.yoyo = pm_bool(v, "yoyo");
        else if (pm_lower_eq(k, "keep_velocity")) t->d.keep_velocity = pm_bool(v, "keep_velocity");
        else if (pm_lower_eq(k, "from")) {
            double x[4];
            int n = pm_vector(v, "from", x, 4), j;
            for (j = 0; j < n; j++) t->from4[j] = (float)x[j];
            t->n_from = n;
            t->from_given = 1;
            t->d.from = t->from4[0];
            t->d.from_set = true;
        }
        else if (pm_lower_eq(k, "to") && (allow & TW_TO)) {
            double x[4];
            int n = pm_vector(v, "to", x, 4), j;
            for (j = 0; j < n; j++) t->to4[j] = (float)x[j];
            t->n_to = n;
        }
        else if (pm_lower_eq(k, "start") && (allow & TW_START)) { t->d.start_ns = tlm_time(v, "start"); t->d.start_set = true; }
        else if (pm_lower_eq(k, "immediate") && (allow & TW_FIELD)) t->immediate = pm_bool(v, "immediate");
        else if (pm_lower_eq(k, "to_current") && (allow & TW_FIELD)) t->to_current = pm_bool(v, "to_current");
        else if (pm_lower_eq(k, "overwrite") && (allow & TW_FIELD)) {
            if (mxIsChar(v)) {
                char* w = pm_string(v, "overwrite");
                if (pm_lower_eq(w, "all")) t->overwrite = 1;
                else if (pm_lower_eq(w, "none")) t->overwrite = 0;
                else pm_err("arg", "overwrite: true, false, 'all' or 'none'");
            } else t->overwrite = pm_bool(v, "overwrite");
        }
        else if (pm_lower_eq(k, "on_start") && (allow & TW_FIELD)) t->on_start = pm_bool(v, "on_start");
        else if (pm_lower_eq(k, "on_complete") && (allow & TW_FIELD)) t->on_complete = pm_bool(v, "on_complete");
        else if (pm_lower_eq(k, "user") && (allow & TW_FIELD)) t->user = pm_u64(v, "user");
        else if (pm_lower_eq(k, "stagger") && (allow & TW_FIELD)) t->stagger = tlm_time(v, "stagger");
        else pm_err("arg", "unknown option '%s'", k);
    }
}

/* ---- events out ------------------------------------------------------------- */

static const char* tlm_ev_fields[] = { "id", "kind", "kind_name", "base", "time", "target", "value", "code", "user",
                                       "flags", "frame", "onset", "residual", "time_ns", "onset_ns", "residual_ns" };

static mxArray* tlm_i64(int64_t v) {
    mxArray* a = mxCreateNumericMatrix(1, 1, mxINT64_CLASS, mxREAL);
    *(int64_t*)mxGetData(a) = v;
    return a;
}

static mxArray* tlm_events_out(tlm_tl* t, const ytl_event* e, int n) {
    mxArray* s = mxCreateStructMatrix((size_t)n, 1, 16, tlm_ev_fields);
    int i;
    for (i = 0; i < n; i++) {
        const ytl_event* x = &e[i];
        int32_t target = x->target;
        uint64_t user = x->user;
        char kn[16];
        if (x->kind == YTL_ONSET || x->kind == YTL_OFFSET || x->kind == YTL_SET) target += 1;
        if (x->kind == YTL_TARGET_SET) {
            uintptr_t p = (uintptr_t)x->user, a = (uintptr_t)t->fields;
            if (p >= a && p < a + (uintptr_t)t->fcap * 16u) user = (uint64_t)((p - a) / 16u) + 1u;
        }
        if (x->kind < 8) snprintf(kn, sizeof kn, "%s", tlm_kind_names[x->kind]);
        else snprintf(kn, sizeof kn, "user");
        mxSetField(s, i, "id", mxCreateDoubleScalar(x->id));
        mxSetField(s, i, "kind", mxCreateDoubleScalar(x->kind));
        mxSetField(s, i, "kind_name", mxCreateString(kn));
        mxSetField(s, i, "base", mxCreateDoubleScalar(x->base));
        mxSetField(s, i, "time", mxCreateDoubleScalar(tlm_s(x->time_ns)));
        mxSetField(s, i, "target", mxCreateDoubleScalar(target));
        mxSetField(s, i, "value", mxCreateDoubleScalar(x->value));
        mxSetField(s, i, "code", mxCreateDoubleScalar(x->code));
        mxSetField(s, i, "user", pm_u64_out(user));
        mxSetField(s, i, "flags", mxCreateDoubleScalar(x->flags));
        mxSetField(s, i, "frame", mxCreateDoubleScalar((double)x->frame));
        mxSetField(s, i, "onset", mxCreateDoubleScalar(tlm_s(x->onset)));
        mxSetField(s, i, "residual", mxCreateDoubleScalar(tlm_s(x->residual)));
        mxSetField(s, i, "time_ns", tlm_i64(x->time_ns));
        mxSetField(s, i, "onset_ns", tlm_i64(x->onset));
        mxSetField(s, i, "residual_ns", tlm_i64(x->residual));
    }
    return s;
}

static mxArray* tlm_values_out(tlm_tl* t) {
    int n = t->tl.n_channels, i;
    mxArray* a = mxCreateDoubleMatrix(1, (size_t)n, mxREAL);
    double* p = mxGetPr(a);
    for (i = 0; i < n; i++) p[i] = t->tl.values[i];
    return a;
}

static uint8_t* tlm_fshape(tlm_tl* t);

/* Every slot as a column; an int field's words are int32, not float. */
static mxArray* tlm_fields_out(tlm_tl* t) {
    mxArray* a = mxCreateDoubleMatrix(4, (size_t)t->fcap, mxREAL);
    double* p = mxGetPr(a);
    const uint8_t* shape = tlm_fshape(t);
    int i;
    for (i = 0; i < 4 * t->fcap; i++)
        p[i] = (shape[i / 4] & 8) ? (double)((const int32_t*)(const void*)t->fields)[i] : (double)t->fields[i];
    return a;
}

/* ---- events in -------------------------------------------------------------------- */

static void tlm_event_in(tlm_tl* t, const mxArray* s, mwIndex i, ytl_event* e) {
    static const char* const allowed[] = { "time", "kind", "base", "target", "value", "code", "user" };
    const mxArray* f;
    int j, nf = mxGetNumberOfFields(s);
    (void)t;
    for (j = 0; j < nf; j++) {
        const char* fn = mxGetFieldNameByNumber(s, j);
        int k, ok = 0;
        for (k = 0; k < 7; k++) if (strcmp(fn, allowed[k]) == 0) ok = 1;
        if (!ok) pm_err("arg", "event: unknown field '%s'", fn);
    }
    memset(e, 0, sizeof *e);
    if ((f = mxGetField(s, i, "time")) && !mxIsEmpty(f)) e->time_ns = tlm_time(f, "time");
    if ((f = mxGetField(s, i, "kind")) && !mxIsEmpty(f)) {
        int k = mxIsChar(f) ? pm_enum(f, "kind", tlm_kind_names, 5) : pm_int(f, "kind");
        if (k < 0 || k > 255) pm_err("arg", "kind: 0 to 255");
        e->kind = (uint8_t)k;
    }
    if ((f = mxGetField(s, i, "base")) && !mxIsEmpty(f)) {
        int b = pm_int(f, "base");
        if (b < 0 || b >= YTL_MAX_BASES) pm_err("arg", "base: 0 to 7");
        e->base = (uint8_t)b;
    }
    if ((f = mxGetField(s, i, "target")) && !mxIsEmpty(f)) {
        e->target = pm_int(f, "target");
        if (e->kind == YTL_ONSET || e->kind == YTL_OFFSET || e->kind == YTL_SET) e->target -= 1;
    }
    if ((f = mxGetField(s, i, "value")) && !mxIsEmpty(f)) e->value = (float)pm_scalar(f, "value");
    if ((f = mxGetField(s, i, "code")) && !mxIsEmpty(f)) e->code = pm_int(f, "code");
    if ((f = mxGetField(s, i, "user")) && !mxIsEmpty(f)) e->user = pm_u64(f, "user");
}

/* ---- tracks ------------------------------------------------------------------------ */

/* Keys: an N x 2, 3 or 4 matrix [time value ease curve], curve a 1-based row
 * of `curves` (M x 4), for BEZIER keys. One malloc holds keys and curves. */
static void* tlm_keys_in(const mxArray* K, const mxArray* C, ytl_track* tr) {
    size_t n, m = 0, i, cols;
    const double* k;
    char* buf;
    ytl_key* keys;
    ytl_curve* curves;
    if (!mxIsDouble(K) || mxIsComplex(K)) pm_err("arg", "keys: a real N x 2..4 double matrix [time value ease curve]");
    n = mxGetM(K); cols = mxGetN(K);
    if (n && (cols < 2 || cols > 4)) pm_err("arg", "keys: N x 2, 3 or 4 [time value ease curve]");
    if (C && !mxIsEmpty(C)) {
        if (!mxIsDouble(C) || mxGetN(C) != 4) pm_err("arg", "curves: M x 4 [x1 y1 x2 y2]");
        m = mxGetM(C);
    }
    if (n > 1000000 || m > 65535) pm_err("arg", "keys: too many");
    buf = (char*)malloc(sizeof(ytl_key) * (n ? n : 1) + sizeof(ytl_curve) * (m ? m : 1));
    if (!buf) pm_err("memory", "out of memory");
    keys = (ytl_key*)(void*)buf;
    curves = (ytl_curve*)(void*)(buf + sizeof(ytl_key) * (n ? n : 1));
    k = mxGetPr(K);
    for (i = 0; i < n; i++) {
        double t = k[i];
        memset(&keys[i], 0, sizeof keys[i]);
        if (!(t >= 0.0 && t < TLM_S_MAX)) { free(buf); pm_err("arg", "key time: seconds, >= 0"); }
        keys[i].time_ns = tlm_ns(t, "key time");
        keys[i].value = (float)k[i + n];
        if (cols >= 3) {
            double e = k[i + 2 * n];
            keys[i].ease = (uint8_t)(e >= 0 && e <= 255 && e == floor(e) ? e : 255);
        }
        if (cols >= 4) {
            double c = k[i + 3 * n];
            if (c >= 1 && c <= (double)m && c == floor(c)) keys[i].curve = (uint16_t)(c - 1);
            else if (keys[i].ease == YTL_EASE_BEZIER) { free(buf); pm_err("arg", "key %d: BEZIER needs a curve row of curves", (int)i + 1); }
        }
    }
    if (m) {
        const double* c = mxGetPr(C);
        for (i = 0; i < m; i++) {
            curves[i].x1 = (float)c[i]; curves[i].y1 = (float)c[i + m];
            curves[i].x2 = (float)c[i + 2 * m]; curves[i].y2 = (float)c[i + 3 * m];
        }
        tr->curves = curves;
        tr->n_curves = (int)m;
    }
    tr->keys = keys;
    tr->n_keys = (int)n;
    return buf;
}

static void tlm_track_done(tlm_tl* t, int ch, int base, void* buf, int rc) {
    if (rc < 0) { free(buf); tlm_check(&t->tl, rc); }
    if (ch >= 0 && ch < YTL_MAX_CHANNELS) {
        free(t->cbuf[ch]);
        t->cbuf[ch] = buf;
        t->cbase[ch] = (int8_t)base;
    } else {
        free(buf);
    }
}

/* ---- op tables ---------------------------------------------------------------------- */

static const char* const tlm_op_names[] = { "none", "on", "off", "set", "trigger", "mark", "wait", "at", "to", "then" };

/* A cell array of cells: {'on', ch}, {'off', ch}, {'set', ch, v},
 * {'trigger', code [, ch]}, {'mark', code [, user]}, {'wait', dt},
 * {'at', t}, {'to' | 'then', ch, value, name, value, ...}. */
static ytl_op* tlm_ops_in(const mxArray* ops, int* n_out) {
    size_t n, i;
    ytl_op* a;
    if (!mxIsCell(ops)) pm_err("arg", "ops: a cell array of cells, such as {{'on', 1}, {'wait', 0.5}}");
    n = mxGetNumberOfElements(ops);
    a = (ytl_op*)mxCalloc(n ? n : 1, sizeof(ytl_op));
    for (i = 0; i < n; i++) {
        const mxArray* c = mxGetCell(ops, i);
        const mxArray* it[16];
        int m, j, kind;
        ytl_op* op = &a[i];
        if (!c || !mxIsCell(c) || mxGetNumberOfElements(c) < 1 || mxGetNumberOfElements(c) > 16)
            pm_err("arg", "op %d: a cell such as {'wait', 0.5}", (int)i + 1);
        m = (int)mxGetNumberOfElements(c);
        for (j = 0; j < m; j++) it[j] = mxGetCell(c, j);
        kind = pm_enum(it[0], "op", tlm_op_names, 10);
        op->op = kind;
        switch (kind) {
        case YTL_OP_ON: case YTL_OP_OFF:
            if (m != 2) goto bad;
            op->ch = tlm_ch(it[1], "channel");
            break;
        case YTL_OP_SET_VALUE:
            if (m != 3) goto bad;
            op->ch = tlm_ch(it[1], "channel");
            op->value = (float)pm_scalar(it[2], "value");
            break;
        case YTL_OP_TRIGGER:
            if (m != 2 && m != 3) goto bad;
            op->code = pm_int(it[1], "code");
            if (m == 3) op->ch = pm_int(it[2], "trigger channel");
            break;
        case YTL_OP_MARK:
            if (m != 2 && m != 3) goto bad;
            op->code = pm_int(it[1], "code");
            if (m == 3) op->user = pm_u64(it[2], "user");
            break;
        case YTL_OP_WAIT: case YTL_OP_AT:
            if (m != 2) goto bad;
            op->t_ns = tlm_time(it[1], kind == YTL_OP_WAIT ? "wait" : "at");
            break;
        case YTL_OP_TO: case YTL_OP_THEN: {
            tlm_tw t;
            if (m < 3) goto bad;
            op->ch = tlm_ch(it[1], "channel");
            tlm_tw_parse(m, it, 3, TW_CH, &t);
            op->tween = t.d;
            op->tween.to = (float)pm_scalar(it[2], "value");
            break;
        }
        default: goto bad;
        }
        continue;
    bad:
        pm_err("arg", "op %d: the wrong number of items for '%s'", (int)i + 1, tlm_op_names[kind]);
    }
    *n_out = (int)n;
    return a;
}

/* ---- fields ------------------------------------------------------------------------------ */

/* A callback that does nothing: on_start and on_complete then add their
 * events to the storage, and the report carries them. */
static void tlm_noop(ytl_timeline* tl, int id, void* user) { (void)tl; (void)id; (void)user; }

static int tlm_field(tlm_tl* t, const mxArray* a) {
    int f = pm_int(a, "field") - 1;
    if (f < 0 || f >= t->fcap || !t->fused[f]) pm_err("arg", "no field %d", f + 1);
    return f;
}

/* Field i's shape lives with the binding: the header knows it only after
 * the first tween. */
static uint8_t* tlm_fshape(tlm_tl* t) { return t->fused + t->fcap; }

/* ---- mexFunction ------------------------------------------------------------------------- */

static void tlm_close_seqs(uint64_t id) {
    pm_node** pp = &pm_list;
    while (*pp) {
        tlm_seq* s = (tlm_seq*)(*pp)->obj;
        if (s && s->kind == TLM_SEQ && s->tl_id == id) {
            pm_node* dead = *pp;
            *pp = dead->next;
            free(dead->obj);
            free(dead);
        } else {
            pp = &(*pp)->next;
        }
    }
}

YSP_SHARE_MEX_ENTRY void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char* cmd;
    tlm_tl* t;
    tlm_seq* s;
    int rc;
    pm_destroy = tlm_destroy;
    /* Handles start at 0x544C << 32, so a handle of another module (whose
     * counter starts at 1) is not taken for one of these by acquire(); still
     * exact as a double. */
    if (pm_next_id == 1) pm_next_id = ((uint64_t)0x544C << 32) + 1;
    if (nrhs < 1 || !mxIsChar(prhs[0])) pm_err("usage", "first argument must be a command string");
    cmd = mxArrayToString(prhs[0]);

    /* ---- module commands ---- */
    if (strcmp(cmd, "version") == 0) {
        plhs[0] = mxCreateString(TLM_VERSION);
        if (nlhs > 1) plhs[1] = mxCreateString(ytl_version());
        return;
    }
    if (strcmp(cmd, "open") == 0) {
        static const char* const allowed[] = { "channels", "initial", "events", "keys", "fields", "tweens", "lead" };
        const mxArray *d = nrhs > 1 ? prhs[1] : NULL, *f;
        ytl_desc desc;
        float init[YTL_MAX_CHANNELS];
        double iv[YTL_MAX_CHANNELS];
        int nch = 0, ev = 1024, ks = 1024, fs = 64, tws = 256, i;
        double lead = 0.0;
        if (d) {
            pm_check_fields(d, allowed, 7, "open");
            if ((f = pm_field(d, "channels"))) nch = pm_int(f, "channels");
            if ((f = pm_field(d, "events"))) ev = pm_int(f, "events");
            if ((f = pm_field(d, "keys"))) ks = pm_int(f, "keys");
            if ((f = pm_field(d, "fields"))) fs = pm_int(f, "fields");
            if ((f = pm_field(d, "tweens"))) tws = pm_int(f, "tweens");
            if ((f = pm_field(d, "lead"))) {
                if (mxIsChar(f)) {
                    if (!pm_lower_eq(pm_string(f, "lead"), "none")) pm_err("arg", "lead: a fraction of a period, or 'none'");
                    lead = YTL_LEAD_NONE;
                } else lead = pm_scalar(f, "lead");
            }
        }
        if (nch < 0 || nch > YTL_MAX_CHANNELS) pm_err("arg", "channels: 0 to %d", YTL_MAX_CHANNELS);
        if (ev < 0 || ks < 0 || fs < 0 || tws < 0) pm_err("arg", "events, keys, fields and tweens are counts, >= 0");
        memset(init, 0, sizeof init);
        if (d && (f = pm_field(d, "initial"))) {
            int n = pm_vector(f, "initial", iv, YTL_MAX_CHANNELS);
            if (n != nch) pm_err("arg", "initial: one value per channel");
            for (i = 0; i < n; i++) init[i] = (float)iv[i];
        }
        t = (tlm_tl*)calloc(1, sizeof(tlm_tl));
        if (!t) pm_err("memory", "out of memory");
        t->kind = TLM_TIMELINE;
        t->events = (ytl_event*)malloc(sizeof(ytl_event) * (size_t)(ev ? ev : 1));
        t->fired = (ytl_event*)malloc(sizeof(ytl_event) * (size_t)(ev ? ev : 1));
        t->arena = (ytl_key*)malloc(sizeof(ytl_key) * (size_t)(ks ? ks : 1));
        t->tg = (ytl_target_slot*)malloc(sizeof(ytl_target_slot) * (size_t)(fs ? fs : 1));
        t->tw = (ytl_tween_slot*)malloc(sizeof(ytl_tween_slot) * (size_t)(tws ? tws : 1));
        t->fields = (float*)calloc((size_t)(fs ? fs : 1), 16);
        t->fused = (uint8_t*)calloc((size_t)(fs ? fs : 1), 2);
        if (!t->events || !t->fired || !t->arena || !t->tg || !t->tw || !t->fields || !t->fused) {
            tlm_destroy(t);
            pm_err("memory", "out of memory");
        }
        t->ecap = ev;
        t->fcap = fs;
        memset(&desc, 0, sizeof desc);
        desc.events = t->events; desc.event_capacity = ev;
        desc.n_channels = nch; desc.initial = init; desc.lead = lead;
        desc.keys = t->arena; desc.key_capacity = ks;
        desc.targets = t->tg; desc.target_capacity = fs;
        desc.tweens = t->tw; desc.tween_capacity = tws;
        if (!ytl_open(&t->tl, &desc)) {
            char m[192];
            snprintf(m, sizeof m, "%s", ytl_error(&t->tl));
            tlm_destroy(t);
            pm_err("arg", "%s", m);
        }
        plhs[0] = pm_handle_out(pm_register(t));
        return;
    }

    /* ---- sequence commands: the first argument is a sequence ---- */
    if (nrhs >= 2 && mxIsUint64(prhs[1]) && ((tlm_tl*)pm_lookup(prhs[1]))->kind == TLM_SEQ) {
        s = tlm_get_seq(prhs[1]);
        t = s->t;
        if (strcmp(cmd, "on") == 0 || strcmp(cmd, "off") == 0) {
            pm_nargs(nrhs, 3, "ysp_timeline('on' | 'off', q, ch)");
            if (cmd[1] == 'n') ytl_on(&s->q, tlm_ch(prhs[2], "channel"));
            else ytl_off(&s->q, tlm_ch(prhs[2], "channel"));
        } else if (strcmp(cmd, "set") == 0) {
            pm_nargs(nrhs, 4, "ysp_timeline('set', q, ch, value)");
            ytl_set_value(&s->q, tlm_ch(prhs[2], "channel"), (float)pm_scalar(prhs[3], "value"));
        } else if (strcmp(cmd, "trigger") == 0) {
            pm_nargs(nrhs, 3, "ysp_timeline('trigger', q, code [, ch])");
            ytl_trigger_ch(&s->q, nrhs > 3 ? pm_int(prhs[3], "trigger channel") : 0, pm_int(prhs[2], "code"));
        } else if (strcmp(cmd, "mark") == 0) {
            pm_nargs(nrhs, 3, "ysp_timeline('mark', q, code [, user])");
            ytl_mark(&s->q, pm_int(prhs[2], "code"), nrhs > 3 ? pm_u64(prhs[3], "user") : 0);
        } else if (strcmp(cmd, "wait") == 0) {
            pm_nargs(nrhs, 3, "ysp_timeline('wait', q, dt)");
            ytl_wait(&s->q, tlm_time(prhs[2], "wait"));
        } else if (strcmp(cmd, "at") == 0) {
            pm_nargs(nrhs, 3, "ysp_timeline('at', q, t)");
            ytl_at(&s->q, tlm_time(prhs[2], "at"));
        } else if (strcmp(cmd, "to") == 0 || strcmp(cmd, "then") == 0) {
            tlm_tw w;
            int64_t end;
            pm_nargs(nrhs, 4, "end = ysp_timeline('to' | 'then', q, ch, value, name, value, ...)");
            tlm_tw_parse(nrhs, prhs, 4, TW_CH, &w);
            w.d.to = (float)pm_scalar(prhs[3], "value");
            if (cmd[1] == 'o') end = ytl_to(&s->q, tlm_ch(prhs[2], "channel"), &w.d);
            else { ytl_then(&s->q, tlm_ch(prhs[2], "channel"), &w.d); end = s->q.t; }
            tlm_seq_check(s, cmd);
            if (nlhs > 0) plhs[0] = mxCreateDoubleScalar(tlm_s(end));
            return;
        } else if (strcmp(cmd, "keyframes") == 0) {
            size_t n, i;
            double *v, *o;
            float* fv;
            int64_t* ov;
            ytl_curve c;
            int ease = 0;
            pm_nargs(nrhs, 5, "ysp_timeline('keyframes', q, ch, values, offsets [, ease])");
            n = mxGetNumberOfElements(prhs[3]);
            if (mxGetNumberOfElements(prhs[4]) != n || !mxIsDouble(prhs[3]) || !mxIsDouble(prhs[4]))
                pm_err("arg", "keyframes: values and offsets are double vectors of one length");
            if (nrhs > 5) ease = tlm_ease(prhs[5], &c);
            v = mxGetPr(prhs[3]); o = mxGetPr(prhs[4]);
            fv = (float*)mxMalloc(sizeof(float) * (n ? n : 1));
            ov = (int64_t*)mxMalloc(sizeof(int64_t) * (n ? n : 1));
            for (i = 0; i < n; i++) { fv[i] = (float)v[i]; ov[i] = tlm_ns(o[i], "offset"); }
            ytl_keyframes(&s->q, tlm_ch(prhs[2], "channel"), fv, ov, (int)n, ease);
        } else if (strcmp(cmd, "on_n") == 0) {
            size_t n, i;
            int* ch;
            double* c;
            pm_nargs(nrhs, 3, "ysp_timeline('on_n', q, channels [, stagger])");
            if (!mxIsDouble(prhs[2])) pm_err("arg", "channels: a double vector");
            n = mxGetNumberOfElements(prhs[2]);
            c = mxGetPr(prhs[2]);
            ch = (int*)mxMalloc(sizeof(int) * (n ? n : 1));
            for (i = 0; i < n; i++) ch[i] = (int)c[i] - 1;
            ytl_on_n(&s->q, ch, (int)n, nrhs > 3 ? tlm_time(prhs[3], "stagger") : 0);
        } else if (strcmp(cmd, "run") == 0) {
            int n = 0;
            ytl_op* ops;
            pm_nargs(nrhs, 3, "ysp_timeline('run', q, ops)");
            ops = tlm_ops_in(prhs[2], &n);
            ytl_run(&s->q, ops, n);
            if (s->q.err < 0) {
                char m[256];
                snprintf(m, sizeof m, "op %d: %s", s->q.err_call + 1, ytl_error(&t->tl));
                pm_err(s->q.err == YTL_ERR_FULL ? "full" : s->q.err == YTL_ERR_BOUND ? "bound" : "arg", "%s", m);
            }
        } else if (strcmp(cmd, "cancel") == 0) {
            rc = ytl_seq_cancel(&s->q);
            tlm_check(&t->tl, rc);
            plhs[0] = mxCreateDoubleScalar(rc);
            return;
        } else if (strcmp(cmd, "cursor") == 0) {
            static const char* fn[] = { "t", "prev_start", "prev_end", "t_ns", "base", "err", "err_call" };
            mxArray* o = mxCreateStructMatrix(1, 1, 7, fn);
            mxSetField(o, 0, "t", mxCreateDoubleScalar(tlm_s(s->q.t)));
            mxSetField(o, 0, "prev_start", mxCreateDoubleScalar(tlm_s(s->q.prev_start)));
            mxSetField(o, 0, "prev_end", mxCreateDoubleScalar(tlm_s(s->q.prev_end)));
            mxSetField(o, 0, "t_ns", tlm_i64(s->q.t));
            mxSetField(o, 0, "base", mxCreateDoubleScalar(s->q.base));
            mxSetField(o, 0, "err", mxCreateDoubleScalar(s->q.err));
            mxSetField(o, 0, "err_call", mxCreateDoubleScalar(s->q.err_call + 1));
            plhs[0] = o;
            return;
        } else if (strcmp(cmd, "release") == 0) {
            pm_unregister(s);
            free(s);
            return;
        } else if (strcmp(cmd, "tween_to") == 0 || strcmp(cmd, "tween_from") == 0 ||
                   strcmp(cmd, "tween_from_to") == 0 || strcmp(cmd, "tween_set") == 0 || strcmp(cmd, "animate") == 0) {
            /* A target tween on field f (or adjacent fields f(1):f(end),
             * staggered). Every form goes through ytl_animate_n(). */
            ytl_anim a;
            {
                tlm_tw w;
                int mode = cmd[0] == 'a' ? 4 : strcmp(cmd, "tween_to") == 0 ? 0 : strcmp(cmd, "tween_from") == 0 ? 1
                         : strcmp(cmd, "tween_from_to") == 0 ? 2 : 3;
                size_t nf, i;
                const double* fp;
                int f0, ncomp, id, first_opt;
                uint8_t* shape = tlm_fshape(t);
                pm_nargs(nrhs, mode == 4 ? 3 : mode == 3 ? 4 : mode == 2 ? 6 : 5,
                         "id = ysp_timeline('tween_to', q, f, to, duration [, ease]) | ('tween_from', q, f, from, "
                         "duration [, ease]) | ('tween_from_to', q, f, from, to, duration [, ease]) | ('tween_set', "
                         "q, f, value) | ('animate', q, f, name, value, ...)");
                if (!mxIsDouble(prhs[2]) || (nf = mxGetNumberOfElements(prhs[2])) < 1) pm_err("arg", "field: an index, or adjacent indices");
                fp = mxGetPr(prhs[2]);
                f0 = (int)fp[0] - 1;
                if (fp[0] != floor(fp[0]) || f0 < 0 || f0 >= t->fcap || !t->fused[f0]) pm_err("arg", "no field %g", fp[0]);
                for (i = 1; i < nf; i++)
                    if (fp[i] != fp[0] + (double)i || f0 + (int)i >= t->fcap || !t->fused[f0 + (int)i])
                        pm_err("arg", "a staggered tween needs adjacent fields: make them with 'fields'");
                ncomp = shape[f0] & 7;
                memset(&a, 0, sizeof a);
                memset(&w, 0, sizeof w);
                w.immediate = -1;
                a.target = t->fields + 4 * f0;
                a.type = (shape[f0] & 8) ? YTL_I32 : YTL_F32;
                a.n = ncomp;
                if (mode == 4) {
                    tlm_tw_parse(nrhs, prhs, 3, TW_CH | TW_FIELD | TW_TO, &w);
                    if (w.n_to && w.n_to != ncomp) pm_err("arg", "to: the field has %d components", ncomp);
                    if (w.n_from && w.n_from != ncomp) pm_err("arg", "from: the field has %d components", ncomp);
                    memcpy(a.to, w.to4, sizeof a.to);
                    memcpy(a.from, w.from4, sizeof a.from);
                    a.from_set = w.from_given;
                    a.to_current = w.to_current;
                } else {
                    double v[4];
                    first_opt = mode == 2 ? 6 : mode == 3 ? 4 : 5;
                    if (pm_vector(prhs[3], "value", v, 4) != ncomp) pm_err("arg", "the field has %d components", ncomp);
                    for (i = 0; i < (size_t)ncomp; i++) {
                        if (mode == 1 || mode == 2) a.from[i] = (float)v[i];
                        else a.to[i] = (float)v[i];
                    }
                    if (mode == 2) {
                        if (pm_vector(prhs[4], "to", v, 4) != ncomp) pm_err("arg", "the field has %d components", ncomp);
                        for (i = 0; i < (size_t)ncomp; i++) a.to[i] = (float)v[i];
                    }
                    a.from_set = mode == 1 || mode == 2;
                    a.to_current = mode == 1;
                    if (mode != 3) w.d.duration_ns = tlm_time(prhs[first_opt - 1], "duration");
                    if (mode != 3 && nrhs > first_opt) w.d.ease = tlm_ease(prhs[first_opt], &w.d.curve);
                    if (mode == 3) w.d.ease = YTL_EASE_STEP;
                }
                a.duration_ns = w.d.duration_ns;
                a.delay_ns = w.d.delay_ns;
                a.stagger_ns = w.stagger;
                a.ease = w.d.ease;
                a.curve = w.d.curve;
                a.cycles = w.d.cycles;
                a.yoyo = w.d.yoyo;
                a.keep_velocity = w.d.keep_velocity;
                a.overwrite = w.overwrite ? YTL_OVERWRITE_ALL : YTL_OVERWRITE_NONE;
                a.immediate = w.immediate >= 0 ? w.immediate != 0 : a.from_set;
                a.user = (void*)(uintptr_t)w.user;
                /* The callbacks' events go in the report; a MATLAB function
                 * called from inside the evaluate is not offered. */
                if (w.on_start) a.on_start = tlm_noop;
                if (w.on_complete) a.on_complete = tlm_noop;
                id = ytl_animate_n(&s->q, &a, 16, (int)nf);
                tlm_seq_check(s, cmd);
                plhs[0] = mxCreateDoubleScalar(id);
                return;
            }
        } else {
            pm_err("usage", "unknown sequence command '%s'", cmd);
        }
        tlm_seq_check(s, cmd);
        return;
    }

    /* ---- timeline commands: the first argument is a timeline ---- */
    pm_nargs(nrhs, 2, "ysp_timeline(command, h, ...)");
    t = tlm_get_tl(prhs[1]);
    if (strcmp(cmd, "close") == 0) {
        if (t->pins > 0) pm_err("busy", "%d other module object(s) use this timeline (a movie?); close them first", t->pins);
        tlm_close_seqs(tlm_id(prhs[1]));
        pm_unregister(t);
        tlm_destroy(t);
    } else if (strcmp(cmd, "evaluate") == 0) {
        ytl_frame f;
        int n;
        pm_nargs(nrhs, 3, "[values, events, fields] = ysp_timeline('evaluate', h, onset [, period [, index]])");
        f.onset = tlm_time(prhs[2], "onset");
        f.period = nrhs > 3 ? tlm_time(prhs[3], "period") : 0;
        f.index = nrhs > 4 ? (int64_t)pm_scalar(prhs[4], "index") : t->frames;
        n = ytl_evaluate(&t->tl, &f, t->fired, t->ecap);
        tlm_check(&t->tl, n);
        t->frames = f.index + 1;
        plhs[0] = tlm_values_out(t);
        if (nlhs > 1) plhs[1] = tlm_events_out(t, t->fired, n < t->ecap ? n : t->ecap);
        if (nlhs > 2) plhs[2] = tlm_fields_out(t);
    } else if (strcmp(cmd, "pins") == 0) {
        plhs[0] = mxCreateDoubleScalar(t->pins);
    } else if (strcmp(cmd, "values") == 0) {
        plhs[0] = tlm_values_out(t);
    } else if (strcmp(cmd, "field_values") == 0) {
        plhs[0] = tlm_fields_out(t);
    } else if (strcmp(cmd, "anchor") == 0) {
        pm_nargs(nrhs, 4, "ysp_timeline('anchor', h, base, rt [, bt])");
        tlm_check(&t->tl, ytl_anchor(&t->tl, tlm_base(prhs[2]), tlm_time(prhs[3], "rt"), nrhs > 4 ? tlm_time(prhs[4], "bt") : 0));
    } else if (strcmp(cmd, "pause") == 0 || strcmp(cmd, "resume") == 0) {
        pm_nargs(nrhs, 4, "ysp_timeline('pause' | 'resume', h, base, rt)");
        rc = cmd[0] == 'p' ? ytl_pause(&t->tl, tlm_base(prhs[2]), tlm_time(prhs[3], "rt"))
                           : ytl_resume(&t->tl, tlm_base(prhs[2]), tlm_time(prhs[3], "rt"));
        tlm_check(&t->tl, rc);
    } else if (strcmp(cmd, "stop") == 0) {
        pm_nargs(nrhs, 3, "ysp_timeline('stop', h, base)");
        tlm_check(&t->tl, ytl_stop(&t->tl, tlm_base(prhs[2])));
    } else if (strcmp(cmd, "skip") == 0) {
        pm_nargs(nrhs, 5, "n = ysp_timeline('skip', h, base, rt, bt)");
        rc = ytl_skip(&t->tl, tlm_base(prhs[2]), tlm_time(prhs[3], "rt"), tlm_time(prhs[4], "bt"));
        tlm_check(&t->tl, rc);
        plhs[0] = mxCreateDoubleScalar(rc);
    } else if (strcmp(cmd, "rate") == 0) {
        double num, den;
        pm_nargs(nrhs, 5, "ysp_timeline('rate', h, base, rt, num [, den])");
        num = pm_scalar(prhs[4], "num");
        den = nrhs > 5 ? pm_scalar(prhs[5], "den") : 1.0;
        if (!(num >= 1 && num <= 2147483647.0 && num == floor(num) && den >= 1 && den <= 2147483647.0 && den == floor(den)))
            pm_err("arg", "rate: num and den are integers in 1 to 2^31 - 1 ('pause' freezes a base)");
        tlm_check(&t->tl, ytl_rate(&t->tl, tlm_base(prhs[2]), tlm_time(prhs[3], "rt"), (int32_t)num, (int32_t)den));
    } else if (strcmp(cmd, "get_rate") == 0) {
        int32_t n = 0, d = 0;
        double r[2];
        pm_nargs(nrhs, 3, "r = ysp_timeline('get_rate', h, base)");
        if (!ytl_get_rate(&t->tl, tlm_base(prhs[2]), &n, &d)) pm_err("arg", "get_rate: no such base");
        r[0] = n; r[1] = d;
        plhs[0] = pm_row(r, 2);
    } else if (strcmp(cmd, "base_time") == 0 || strcmp(cmd, "rt_time") == 0) {
        int64_t out = 0;
        bool ok;
        pm_nargs(nrhs, 4, "t = ysp_timeline('base_time' | 'rt_time', h, base, t)");
        ok = cmd[0] == 'b' ? ytl_base_time(&t->tl, tlm_base(prhs[2]), tlm_time(prhs[3], "rt"), &out)
                           : ytl_rt_time(&t->tl, tlm_base(prhs[2]), tlm_time(prhs[3], "bt"), &out);
        plhs[0] = tlm_time_out(ok, out);
    } else if (strcmp(cmd, "window") == 0) {
        ytl_frame f;
        int64_t out = 0;
        pm_nargs(nrhs, 5, "t = ysp_timeline('window', h, base, onset, period)");
        memset(&f, 0, sizeof f);
        f.onset = tlm_time(prhs[3], "onset");
        f.period = tlm_time(prhs[4], "period");
        {
            /* The call writes out: evaluate it first (argument order is unspecified). */
            bool ok_ = ytl_window(&t->tl, tlm_base(prhs[2]), &f, &out);
            plhs[0] = tlm_time_out(ok_, out);
        }
    } else if (strcmp(cmd, "clamped") == 0) {
        pm_nargs(nrhs, 3, "n = ysp_timeline('clamped', h, base)");
        rc = ytl_clamped(&t->tl, tlm_base(prhs[2]));
        tlm_check(&t->tl, rc);
        plhs[0] = mxCreateDoubleScalar(rc);
    } else if (strcmp(cmd, "next_due") == 0) {
        int64_t out = 0;
        {
            /* The call writes out: evaluate it first (argument order is unspecified). */
            bool ok_ = ytl_next_due(&t->tl, &out);
            plhs[0] = tlm_time_out(ok_, out);
        }
    } else if (strcmp(cmd, "horizon") == 0) {
        int64_t out = 0;
        pm_nargs(nrhs, 3, "t = ysp_timeline('horizon', h, base)");
        {
            /* The call writes out: evaluate it first (argument order is unspecified). */
            bool ok_ = ytl_horizon(&t->tl, tlm_base(prhs[2]), &out);
            plhs[0] = tlm_time_out(ok_, out);
        }
    } else if (strcmp(cmd, "lead") == 0) {
        plhs[0] = mxCreateDoubleScalar(ytl_lead(&t->tl));
    } else if (strcmp(cmd, "error") == 0) {
        plhs[0] = mxCreateString(ytl_error(&t->tl));
    } else if (strcmp(cmd, "add") == 0) {
        size_t n, i;
        ytl_event* e;
        pm_nargs(nrhs, 3, "id = ysp_timeline('add', h, events)");
        if (!mxIsStruct(prhs[2])) pm_err("arg", "events: a struct (array) with time, kind, base, target, value, code, user");
        n = mxGetNumberOfElements(prhs[2]);
        e = (ytl_event*)mxCalloc(n ? n : 1, sizeof(ytl_event));
        for (i = 0; i < n; i++) tlm_event_in(t, prhs[2], (mwIndex)i, &e[i]);
        rc = ytl_add_n(&t->tl, e, (int)n);
        tlm_check(&t->tl, rc);
        plhs[0] = mxCreateDoubleScalar(rc);
    } else if (strcmp(cmd, "remove") == 0) {
        pm_nargs(nrhs, 3, "ysp_timeline('remove', h, id)");
        tlm_check(&t->tl, ytl_remove(&t->tl, pm_int(prhs[2], "id")));
    } else if (strcmp(cmd, "prune") == 0 || strcmp(cmd, "clear") == 0) {
        int base = nrhs > 2 ? tlm_base(prhs[2]) : YTL_ALL_BASES, ch;
        rc = cmd[0] == 'p' ? ytl_prune(&t->tl, base) : ytl_clear(&t->tl, base);
        tlm_check(&t->tl, rc);
        if (cmd[0] == 'c')
            for (ch = 0; ch < YTL_MAX_CHANNELS; ch++)
                if (t->cbuf[ch] && (base == YTL_ALL_BASES || t->cbase[ch] == base)) { free(t->cbuf[ch]); t->cbuf[ch] = NULL; }
        plhs[0] = mxCreateDoubleScalar(rc);
    } else if (strcmp(cmd, "events") == 0) {
        int n = 0;
        const ytl_event* e = ytl_events(&t->tl, nrhs > 2 ? tlm_base(prhs[2]) : YTL_ALL_BASES, &n);
        plhs[0] = tlm_events_out(t, e, e ? n : 0);
    } else if (strcmp(cmd, "find") == 0) {
        const ytl_event* e;
        pm_nargs(nrhs, 3, "e = ysp_timeline('find', h, id)");
        e = ytl_find(&t->tl, pm_int(prhs[2], "id"));
        plhs[0] = tlm_events_out(t, e, e ? 1 : 0);
    } else if (strcmp(cmd, "peek") == 0) {
        pm_nargs(nrhs, 5, "ev = ysp_timeline('peek', h, base, from, to)");
        rc = ytl_peek(&t->tl, tlm_base(prhs[2]), tlm_time(prhs[3], "from"), tlm_time(prhs[4], "to"), t->fired, t->ecap);
        tlm_check(&t->tl, rc);
        plhs[0] = tlm_events_out(t, t->fired, rc < t->ecap ? rc : t->ecap);
    } else if (strcmp(cmd, "set_keys") == 0) {
        ytl_track tr;
        void* buf;
        int ch, base;
        pm_nargs(nrhs, 5, "ysp_timeline('set_keys', h, ch, base, keys [, curves])");
        ch = tlm_ch(prhs[2], "channel");
        base = tlm_base(prhs[3]);
        memset(&tr, 0, sizeof tr);
        if (mxIsEmpty(prhs[4])) {
            tlm_track_done(t, ch, base, NULL, ytl_set_track(&t->tl, ch, base, NULL));
        } else {
            buf = tlm_keys_in(prhs[4], nrhs > 5 ? prhs[5] : NULL, &tr);
            tlm_track_done(t, ch, base, buf, ytl_set_track(&t->tl, ch, base, &tr));
        }
    } else if (strcmp(cmd, "set_track") == 0) {
        static const char* const allowed[] = { "keys", "curves", "samples", "t0", "rate", "interp", "period", "repeats" };
        ytl_track tr;
        void* buf = NULL;
        int ch, base;
        const mxArray *d, *f, *k, *smp;
        pm_nargs(nrhs, 4, "ysp_timeline('set_track', h, ch, base [, track])");
        ch = tlm_ch(prhs[2], "channel");
        base = tlm_base(prhs[3]);
        memset(&tr, 0, sizeof tr);
        d = nrhs > 4 && !mxIsEmpty(prhs[4]) ? prhs[4] : NULL;
        if (!d) {
            tlm_track_done(t, ch, base, NULL, ytl_set_track(&t->tl, ch, base, NULL));
            return;
        }
        pm_check_fields(d, allowed, 8, "track");
        k = pm_field(d, "keys");
        smp = pm_field(d, "samples");
        if ((f = pm_field(d, "t0"))) tr.t0_ns = tlm_time(f, "t0");
        if ((f = pm_field(d, "rate"))) {
            double r = pm_scalar(f, "rate");
            if (!(r >= 0 && r <= 1e9 && r == floor(r))) pm_err("arg", "rate: an integer, 1 to 1e9 samples per second");
            tr.rate = (int64_t)r;
        }
        if ((f = pm_field(d, "interp"))) tr.interp = mxIsChar(f) ? pm_enum(f, "interp", tlm_interp_names, 3) : pm_int(f, "interp");
        if ((f = pm_field(d, "period"))) tr.period_ns = tlm_time(f, "period");
        if ((f = pm_field(d, "repeats"))) tr.repeats = pm_int(f, "repeats");
        if (k && smp) pm_err("arg", "track: keys or samples, not both");
        if (k) {
            buf = tlm_keys_in(k, pm_field(d, "curves"), &tr);
        } else if (smp) {
            size_t n = mxGetNumberOfElements(smp), i;
            float* fs;
            if (!mxIsDouble(smp) && !mxIsSingle(smp)) pm_err("arg", "samples: a double or single vector");
            if (n > 2147483647u) pm_err("arg", "samples: too many");
            fs = (float*)malloc(sizeof(float) * (n ? n : 1));
            if (!fs) pm_err("memory", "out of memory");
            if (mxIsSingle(smp)) memcpy(fs, mxGetData(smp), sizeof(float) * n);
            else { const double* p = mxGetPr(smp); for (i = 0; i < n; i++) fs[i] = (float)p[i]; }
            tr.samples = fs;
            tr.n_samples = (int)n;
            buf = fs;
        }
        tlm_track_done(t, ch, base, buf, ytl_set_track(&t->tl, ch, base, &tr));
    } else if (strcmp(cmd, "tween") == 0) {
        tlm_tw w;
        pm_nargs(nrhs, 5, "ysp_timeline('tween', h, ch, base, to, name, value, ...)");
        tlm_tw_parse(nrhs, prhs, 5, TW_CH | TW_START, &w);
        w.d.to = (float)pm_scalar(prhs[4], "to");
        tlm_check(&t->tl, ytl_tween(&t->tl, tlm_ch(prhs[2], "channel"), tlm_base(prhs[3]), &w.d));
    } else if (strcmp(cmd, "seq") == 0) {
        tlm_seq* q;
        int now = 0;
        pm_nargs(nrhs, 3, "q = ysp_timeline('seq', h, base [, 'now'])");
        if (nrhs > 3) {
            if (!pm_lower_eq(pm_string(prhs[3], "mode"), "now")) pm_err("arg", "seq: the third argument is 'now'");
            now = 1;
        }
        q = (tlm_seq*)calloc(1, sizeof(tlm_seq));
        if (!q) pm_err("memory", "out of memory");
        q->kind = TLM_SEQ;
        q->t = t;
        q->tl_id = tlm_id(prhs[1]);
        q->q = now ? ytl_seq_now(&t->tl, tlm_base(prhs[2])) : ytl_seq_on(&t->tl, tlm_base(prhs[2]));
        if (q->q.err < 0) { free(q); tlm_check(&t->tl, YTL_ERR_ARG); }
        plhs[0] = pm_handle_out(pm_register(q));
    } else if (strcmp(cmd, "check_ops") == 0) {
        int n = 0, bad = -1;
        ytl_op* ops;
        pm_nargs(nrhs, 4, "ysp_timeline('check_ops', h, base, ops)");
        ops = tlm_ops_in(prhs[3], &n);
        rc = ytl_check_ops(&t->tl, tlm_base(prhs[2]), ops, n, &bad);
        if (rc < 0) {
            char m[256];
            snprintf(m, sizeof m, "op %d: %s", bad + 1, ytl_error(&t->tl));
            pm_err(rc == YTL_ERR_FULL ? "full" : rc == YTL_ERR_BOUND ? "bound" : "arg", "%s", m);
        }
    } else if (strcmp(cmd, "field") == 0 || strcmp(cmd, "fields") == 0) {
        /* f = ('field', h [, value [, 'int']]); fs = ('fields', h, count [, value [, 'int']]) */
        int many = cmd[5] == 's', count = 1, a0 = 2, n = 1, is_int = 0, i, j, start = -1;
        double v[4] = { 0, 0, 0, 0 };
        uint8_t* shape = tlm_fshape(t);
        if (many) { pm_nargs(nrhs, 3, "fs = ysp_timeline('fields', h, count [, value [, 'int']])"); count = pm_int(prhs[2], "count"); a0 = 3; }
        if (nrhs > a0 && !mxIsEmpty(prhs[a0])) n = pm_vector(prhs[a0], "value", v, 4);
        if (nrhs > a0 + 1) {
            if (!pm_lower_eq(pm_string(prhs[a0 + 1], "type"), "int")) pm_err("arg", "the type is 'int'");
            is_int = 1;
        }
        if (n < 1 || n > 4) pm_err("arg", "a field has 1 to 4 components");
        if (count < 1) pm_err("arg", "count: >= 1");
        for (i = 0; i + count <= t->fcap && start < 0; i++) {
            for (j = 0; j < count && !t->fused[i + j]; j++) {}
            if (j == count) start = i;
            else i += j;
        }
        if (start < 0) pm_err("full", "no %d free field slots in a row (fields=%d)", count, t->fcap);
        for (i = start; i < start + count; i++) {
            float* p = t->fields + 4 * i;
            memset(p, 0, 16);
            for (j = 0; j < n; j++) {
                if (is_int) ((int32_t*)(void*)p)[j] = (int32_t)floor(v[j] + 0.5);
                else p[j] = (float)v[j];
            }
            t->fused[i] = 1;
            shape[i] = (uint8_t)(n | (is_int ? 8 : 0));
        }
        plhs[0] = mxCreateDoubleMatrix(1, (size_t)count, mxREAL);
        for (i = 0; i < count; i++) mxGetPr(plhs[0])[i] = start + i + 1;
    } else if (strcmp(cmd, "field_value") == 0) {
        int f, n, i;
        uint8_t* shape = tlm_fshape(t);
        float* p;
        pm_nargs(nrhs, 3, "v = ysp_timeline('field_value', h, f [, value])");
        f = tlm_field(t, prhs[2]);
        n = shape[f] & 7;
        p = t->fields + 4 * f;
        if (nrhs > 3) {
            double v[4];
            if (pm_vector(prhs[3], "value", v, 4) != n) pm_err("arg", "the field has %d components", n);
            for (i = 0; i < n; i++) {
                if (shape[f] & 8) ((int32_t*)(void*)p)[i] = (int32_t)floor(v[i] + 0.5);
                else p[i] = (float)v[i];
            }
        }
        plhs[0] = mxCreateDoubleMatrix(1, (size_t)n, mxREAL);
        for (i = 0; i < n; i++) mxGetPr(plhs[0])[i] = (shape[f] & 8) ? (double)((int32_t*)(void*)p)[i] : (double)p[i];
    } else if (strcmp(cmd, "field_free") == 0) {
        int f;
        pm_nargs(nrhs, 3, "ysp_timeline('field_free', h, f)");
        f = tlm_field(t, prhs[2]);
        ytl_kill(&t->tl, t->fields + 4 * f);
        t->fused[f] = 0;
    } else if (strcmp(cmd, "kill") == 0) {
        pm_nargs(nrhs, 3, "n = ysp_timeline('kill', h, f)");
        rc = ytl_kill(&t->tl, t->fields + 4 * tlm_field(t, prhs[2]));
        tlm_check(&t->tl, rc);
        plhs[0] = mxCreateDoubleScalar(rc);
    } else if (strcmp(cmd, "kill_tween") == 0) {
        pm_nargs(nrhs, 3, "ysp_timeline('kill_tween', h, id)");
        tlm_check(&t->tl, ytl_kill_tween(&t->tl, pm_int(prhs[2], "id")));
    } else if (strcmp(cmd, "tween_count") == 0) {
        int nt = 0;
        plhs[0] = mxCreateDoubleScalar(ytl_tween_count(&t->tl, &nt));
        if (nlhs > 1) plhs[1] = mxCreateDoubleScalar(nt);
    } else {
        pm_err("usage", "unknown command '%s'", cmd);
    }
}
