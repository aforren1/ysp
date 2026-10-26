/* ysp_response.c - MATLAB/Octave MEX binding for ysp/response.h (builds to ysp_response.<mexext>)
 *
 * Command dispatch in the style of the other ysp MEX files: the first
 * argument is a command string, and 'open' returns an opaque uint64 handle
 * to a response collector. Every time is seconds as a double; the exact
 * int64 ns is beside each absolute time (t_ns, t_response_ns, ...).
 *
 *     h = ysp_response('open', struct('choices', {{'f', 'j'}}, 'duration', 1.5, 'min_rt', 0.1));
 *     ysp_response('arm', h, t_fixation);
 *     ysp_response('set_onset', h, t_flip, struct('src', 'flip'));
 *     st = ysp_response('key', h, t_down, 'f');     % or 'feed' with an event struct array
 *     st = ysp_response('update', h, GetSecs);       % 2 = ended
 *     [r, row] = ysp_response('finish', h);          % r.rt, r.response; row: the data columns
 *     ysp_response('close', h);
 *
 * An event is a struct with the fields of ysp/input.h's yin_event: t
 * (seconds), t_ns (int64), device, control, code, mods, kind, type, flags,
 * stamp, unc (seconds), x, y, value, aux, ticks; the same layout as the
 * ysp_box and ysp_device events. 'feed' takes a struct array of them; a
 * missing field is 0, and t_ns wins over t when it is there.
 *
 * Indices are 1-based and 0 is "none" (ysp_mex_util.h): r.choice, r.entry,
 * an entry's choice. A missing time is NaN in seconds and
 * intmin('int64') (the header's YRSP_NO_TIME) in ns.
 *
 * Build with build.m. See README.md for the command reference.
 */
#define PM_MOD "ysp_response"
#include "ysp_mex_util.h"

/* The MEX function's own version (docs/bindings.md, Versions); the
 * header's is the second output of 'version'. */
#define MEX_BINDING_VERSION "1.0.0"

#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"

#define RSP_MAX_STRS (2 * YRSP_MAX_CHOICES + YRSP_MAX_SOURCES)

/* A handle: the collector, the strings its copied choices and sources
 * point into, the trace array and the last finish(). All mxMalloc'd and
 * made persistent once the open has succeeded, so an error during the
 * parse frees them with the call. */
typedef struct rsp_handle {
    yrsp_collector c;
    char*          strs[RSP_MAX_STRS];
    int            n_strs;
    yin_event*     trace;
    int            trace_cap;
    yrsp_result    last;
    int            have_last;
} rsp_handle;

static void rsp_destroy(void* obj) {
    rsp_handle* h = (rsp_handle*)obj;
    int i;
    yrsp_close(&h->c);
    for (i = 0; i < h->n_strs; i++) mxFree(h->strs[i]);
    if (h->trace) mxFree(h->trace);
    mxFree(h);
}

static void rsp_check(rsp_handle* h, int rc) {
    if (rc >= 0) return;
    pm_err(rc == YRSP_ERR_STATE ? "state" : "arg", "%s", yrsp_error(&h->c));
}

/* ---- units --------------------------------------------------------------- */

#define S_NEG 1   /* a difference: may be negative */
#define S_INF 2   /* Inf accepted: *out = INT64_MAX */

/* Seconds to int64 ns, rounded to the nearest ns; the whole seconds convert
 * exactly so a clock time of days keeps its ns. NaN, and a negative time
 * that is not a difference, are refused. */
static int64_t rsp_ns_of(double s, const char* what, int flags) {
    double whole;
    if (s != s || (!(flags & S_NEG) && s < 0))
        pm_err("arg", "%s: seconds, %s(not NaN)", what, (flags & S_NEG) ? "" : ">= 0 ");
    if (s == HUGE_VAL && (flags & S_INF)) return INT64_MAX;
    if (!(fabs(s) < 9223372036.0)) pm_err("arg", "%s: seconds, finite and below 9223372036 (int64 ns)", what);
    whole = floor(s);
    return (int64_t)whole * 1000000000 + (int64_t)llround((s - whole) * 1e9);
}

static int64_t rsp_seconds(const mxArray* a, const char* what, int flags) {
    return rsp_ns_of(pm_scalar(a, what), what, flags);
}

/* An exact ns argument: an int64 scalar, or a double holding an integer. */
static int64_t rsp_ns_arg(const mxArray* a, const char* what) {
    double d;
    if (mxIsInt64(a) && mxGetNumberOfElements(a) == 1) {
        int64_t v = *(const int64_t*)mxGetData(a);
        if (v < 0) pm_err("arg", "%s: ns, >= 0", what);
        return v;
    }
    d = pm_scalar(a, what);
    if (!(d >= 0) || d != floor(d) || d > 9.2e18) pm_err("arg", "%s: ns, a whole number >= 0", what);
    return (int64_t)d;
}

static mxArray* rsp_s(int64_t ns, int none) { return mxCreateDoubleScalar(none ? mxGetNaN() : (double)ns / 1e9); }

static mxArray* rsp_i64(int64_t v) {
    mxArray* a = mxCreateNumericMatrix(1, 1, mxINT64_CLASS, mxREAL);
    *(int64_t*)mxGetData(a) = v;
    return a;
}

static mxArray* rsp_d(double v) { return mxCreateDoubleScalar(v); }

/* An integer in lo..hi. */
static long long rsp_int(const mxArray* a, const char* what, long long lo, long long hi) {
    double d;
    if ((mxIsInt64(a) || mxIsUint64(a)) && mxGetNumberOfElements(a) == 1) {
        long long v = mxIsInt64(a) ? (long long)*(const int64_t*)mxGetData(a) : (long long)*(const uint64_t*)mxGetData(a);
        if (v < lo || v > hi) pm_err("arg", "%s: %lld is outside %lld..%lld", what, v, lo, hi);
        return v;
    }
    d = pm_scalar(a, what);
    if (!(d == floor(d)) || d < (double)lo || d > (double)hi) pm_err("arg", "%s: an integer in %lld..%lld", what, lo, hi);
    return (long long)d;
}

/* ---- names --------------------------------------------------------------- */

static char* rsp_utf8(const mxArray* a, const char* what) {
    char* v;
    if (!mxIsChar(a)) pm_err("arg", "%s must be a string", what);
#if defined(HAVE_OCTAVE) || defined(OCTMEX_API)
    v = mxArrayToString(a);             /* Octave's char arrays hold UTF-8 */
#else
    v = mxArrayToUTF8String(a);         /* MATLAB's hold UTF-16          */
#endif
    if (!v) pm_err("arg", "%s is not convertible to UTF-8", what);
    return v;
}

static const char* rsp_param_values(const char* field) {
    int n, i;
    const yrsp_param* t = yrsp_params(&n);
    for (i = 0; i < n; i++) if (strcmp(t[i].name, field) == 0) return t[i].values;
    return "";
}

static const char onset_src_values[] = "other=0,flip=1,audio=2,plan=3";

/* An enum by name in a "name=value,..." list (the parameter table's
 * `values`, case-insensitive), or by number in lo..hi. */
static int rsp_enum(const mxArray* a, const char* values, int lo, int hi, const char* what) {
    if (mxIsChar(a)) {
        char* s = rsp_utf8(a, what);
        size_t len = strlen(s);
        const char* p = values;
        while (p && *p) {
            const char* eq = strchr(p, '=');
            size_t k;
            int ok = 1;
            if (!eq) break;
            if ((size_t)(eq - p) == len) {
                for (k = 0; k < len && ok; k++) {
                    char ch = s[k] >= 'A' && s[k] <= 'Z' ? (char)(s[k] - 'A' + 'a') : s[k];
                    ok = ch == p[k];
                }
                if (ok) return atoi(eq + 1);
            }
            p = strchr(eq, ',');
            if (p) p++;
        }
        pm_err("arg", "%s: '%s' is not one of %s", what, s, values);
    }
    return (int)rsp_int(a, what, lo, hi);
}

/* A key: a name (ysp/response.h CHOICES) or a scancode. */
static uint32_t rsp_key(const mxArray* a, const char* what) {
    if (mxIsChar(a)) {
        char* s = rsp_utf8(a, what);
        int k = yrsp_scancode(s);
        if (k < 0) pm_err("arg", "%s: '%s' is not a key name (ysp/response.h, CHOICES)", what, s);
        return (uint32_t)k;
    }
    return (uint32_t)rsp_int(a, what, 0, 0xFFFFFFFFLL);
}

/* kinds: a bit mask, 'all', or a cell of kinds (names or numbers). */
static uint16_t rsp_kinds(const mxArray* a, const char* what) {
    if (mxIsChar(a)) {
        char* s = rsp_utf8(a, what);
        if (pm_lower_eq(s, "all")) return YIN_KINDS_ALL;
        return (uint16_t)(1u << rsp_enum(a, rsp_param_values("choices.kind"), 0, 15, what));
    }
    if (mxIsCell(a)) {
        size_t i, n = mxGetNumberOfElements(a);
        unsigned mask = 0;
        for (i = 0; i < n; i++) {
            const mxArray* c = mxGetCell(a, i);
            if (!c) pm_err("arg", "%s: an empty cell", what);
            mask |= 1u << rsp_enum(c, rsp_param_values("choices.kind"), 0, 15, what);
        }
        return (uint16_t)mask;
    }
    return (uint16_t)rsp_int(a, what, 0, 65535);
}

/* ---- events -------------------------------------------------------------- */

static const char* ev_fields[] = { "t", "t_ns", "device", "control", "code", "mods", "kind", "type",
                                   "flags", "stamp", "unc", "x", "y", "value", "aux", "ticks" };
#define EV_NF 16

/* Element i of an event struct array; fn[] holds the field numbers (-1 when
 * the struct has no such field). Reads in place: no allocation. */
static void rsp_event_in(const mxArray* s, size_t i, const int* fn, yin_event* e) {
    const mxArray* f;
#define FIELD(k) (fn[k] >= 0 && (f = mxGetFieldByNumber(s, i, fn[k])) != NULL && !mxIsEmpty(f))
    memset(e, 0, sizeof *e);
    if (FIELD(1)) e->t = rsp_ns_arg(f, "event t_ns");
    else if (FIELD(0)) e->t = rsp_seconds(f, "event t", 0);
    else pm_err("arg", "event %d: needs t or t_ns", (int)i + 1);
    if (FIELD(6)) e->kind = (uint8_t)(mxIsChar(f) ? rsp_enum(f, rsp_param_values("choices.kind"), 0, 15, "event kind")
                                                  : rsp_int(f, "event kind", 0, 255));
    if (FIELD(2)) e->device = (uint32_t)rsp_int(f, "event device", 0, 0xFFFFFFFFLL);
    if (FIELD(3)) {
        if (mxIsChar(f) && e->kind != YIN_KIND_KEYBOARD) pm_err("arg", "event %d: a key name as control needs kind keyboard", (int)i + 1);
        e->control = rsp_key(f, "event control");
    }
    if (FIELD(4)) e->code = (uint32_t)rsp_int(f, "event code", 0, 0xFFFFFFFFLL);
    if (FIELD(5)) e->mods = (uint16_t)rsp_int(f, "event mods", 0, 65535);
    if (FIELD(7)) e->type = (uint8_t)rsp_int(f, "event type", 0, 255);
    if (FIELD(8)) e->flags = (uint8_t)rsp_int(f, "event flags", 0, 255);
    if (FIELD(9)) e->stamp = (uint8_t)rsp_int(f, "event stamp", 0, 255);
    if (FIELD(10)) {
        int64_t ns = rsp_seconds(f, "event unc", S_INF);
        e->unc_us = ns >= (int64_t)YIN_UNC_MAX * 1000 ? (uint16_t)YIN_UNC_MAX : (uint16_t)((ns + 500) / 1000);
    }
    if (FIELD(11)) e->x = (float)pm_scalar(f, "event x");
    if (FIELD(12)) e->y = (float)pm_scalar(f, "event y");
    if (FIELD(13)) e->value = (float)pm_scalar(f, "event value");
    if (FIELD(14)) e->aux = (float)pm_scalar(f, "event aux");
    if (FIELD(15)) e->ticks = (uint32_t)rsp_int(f, "event ticks", 0, 0xFFFFFFFFLL);
#undef FIELD
}

static void rsp_event_fields(const mxArray* s, int* fn) {
    int k;
    if (!mxIsStruct(s)) pm_err("arg", "events must be a struct array (t, kind, type, control, ...)");
    for (k = 0; k < EV_NF; k++) fn[k] = mxGetFieldNumber(s, ev_fields[k]);
}

static mxArray* rsp_events_out(const yin_event* e, int n) {
    mxArray* s = mxCreateStructMatrix(1, (size_t)n, EV_NF, ev_fields);
    int i;
    for (i = 0; i < n; i++) {
        mxSetFieldByNumber(s, i, 0, rsp_s(e[i].t, 0));
        mxSetFieldByNumber(s, i, 1, rsp_i64(e[i].t));
        mxSetFieldByNumber(s, i, 2, rsp_d(e[i].device));
        mxSetFieldByNumber(s, i, 3, rsp_d(e[i].control));
        mxSetFieldByNumber(s, i, 4, rsp_d(e[i].code));
        mxSetFieldByNumber(s, i, 5, rsp_d(e[i].mods));
        mxSetFieldByNumber(s, i, 6, rsp_d(e[i].kind));
        mxSetFieldByNumber(s, i, 7, rsp_d(e[i].type));
        mxSetFieldByNumber(s, i, 8, rsp_d(e[i].flags));
        mxSetFieldByNumber(s, i, 9, rsp_d(e[i].stamp));
        mxSetFieldByNumber(s, i, 10, rsp_d((double)e[i].unc_us / 1e6));
        mxSetFieldByNumber(s, i, 11, rsp_d(e[i].x));
        mxSetFieldByNumber(s, i, 12, rsp_d(e[i].y));
        mxSetFieldByNumber(s, i, 13, rsp_d(e[i].value));
        mxSetFieldByNumber(s, i, 14, rsp_d(e[i].aux));
        mxSetFieldByNumber(s, i, 15, rsp_d(e[i].ticks));
    }
    return s;
}

/* ---- results ------------------------------------------------------------- */

static mxArray* rsp_result_out(const rsp_handle* h, const yrsp_result* r) {
    static const char* f[] = { "rt", "rt_ns", "rt_key_duration", "rt_key_duration_ns", "response", "choice", "entry",
                               "t_response", "t_response_ns", "kind", "device", "control", "code", "stamp_tier",
                               "stamp_partial", "stamp_lo", "stamp_hi", "t_onset", "t_onset_ns", "onset_src",
                               "onset_tier", "onset_residual", "onset_uncertainty", "onset_frame", "plan_residual",
                               "flags", "n_responses", "n_early", "n_anticipations", "n_late", "n_after_end",
                               "n_not_choice", "n_held", "n_entries", "n_lost", "n_trace", "n_sent" };
    mxArray* s = mxCreateStructMatrix(1, 1, sizeof f / sizeof f[0], f);
    int resp = (r->flags & YRSP_R_RESPONDED) != 0, on = r->t_onset != 0;
    (void)h;
    mxSetField(s, 0, "rt", rsp_s(r->rt_ns, r->rt_ns == YRSP_NO_TIME));
    mxSetField(s, 0, "rt_ns", rsp_i64(r->rt_ns));
    mxSetField(s, 0, "rt_key_duration", rsp_s(r->rt_key_duration_ns, r->rt_key_duration_ns == YRSP_NO_TIME));
    mxSetField(s, 0, "rt_key_duration_ns", rsp_i64(r->rt_key_duration_ns));
    mxSetField(s, 0, "response", mxCreateString(r->response_name ? r->response_name : ""));
    mxSetField(s, 0, "choice", rsp_d(r->response + 1));
    mxSetField(s, 0, "entry", rsp_d(r->entry + 1));
    mxSetField(s, 0, "t_response", rsp_s(r->t_response, !resp));
    mxSetField(s, 0, "t_response_ns", rsp_i64(resp ? r->t_response : YRSP_NO_TIME));
    mxSetField(s, 0, "kind", rsp_d(r->kind));
    mxSetField(s, 0, "device", rsp_d(r->device));
    mxSetField(s, 0, "control", rsp_d(r->control));
    mxSetField(s, 0, "code", rsp_d(r->code));
    mxSetField(s, 0, "stamp_tier", rsp_d(r->stamp_tier));
    mxSetField(s, 0, "stamp_partial", mxCreateLogicalScalar(r->stamp_partial != 0));
    mxSetField(s, 0, "stamp_lo", rsp_s(r->stamp_lo_ns, 0));
    mxSetField(s, 0, "stamp_hi", rsp_s(r->stamp_hi_ns, 0));
    mxSetField(s, 0, "t_onset", rsp_s(r->t_onset, !on));
    mxSetField(s, 0, "t_onset_ns", rsp_i64(on ? r->t_onset : YRSP_NO_TIME));
    mxSetField(s, 0, "onset_src", rsp_d(r->onset_src));
    mxSetField(s, 0, "onset_tier", rsp_d(r->onset_tier));
    mxSetField(s, 0, "onset_residual", rsp_s(r->onset_residual, 0));
    mxSetField(s, 0, "onset_uncertainty", rsp_s(r->onset_uncertainty, 0));
    mxSetField(s, 0, "onset_frame", rsp_d(r->onset_frame >= 0 ? (double)r->onset_frame : mxGetNaN()));
    mxSetField(s, 0, "plan_residual", rsp_s(r->plan_residual, r->plan_residual == YRSP_NO_TIME));
    mxSetField(s, 0, "flags", rsp_d(r->flags));
    mxSetField(s, 0, "n_responses", rsp_d(r->n_responses));
    mxSetField(s, 0, "n_early", rsp_d(r->n_early));
    mxSetField(s, 0, "n_anticipations", rsp_d(r->n_anticipations));
    mxSetField(s, 0, "n_late", rsp_d(r->n_late));
    mxSetField(s, 0, "n_after_end", rsp_d(r->n_after_end));
    mxSetField(s, 0, "n_not_choice", rsp_d(r->n_not_choice));
    mxSetField(s, 0, "n_held", rsp_d(r->n_held));
    mxSetField(s, 0, "n_entries", rsp_d(r->n_entries));
    mxSetField(s, 0, "n_lost", rsp_d(r->n_lost));
    mxSetField(s, 0, "n_trace", rsp_d(r->n_trace));
    mxSetField(s, 0, "n_sent", rsp_d(r->n_sent));
    return s;
}

/* The YRSP_DATA_COLS columns by yrsp_data_set()'s rules: NaN (or '') where
 * that writes an empty field. */
static mxArray* rsp_row_out(const yrsp_result* r) {
    static const char* f[] = { "response", "rt_s", "rt_key_duration_s", "onset_src", "onset_tier", "onset_residual_s",
                               "landing_residual_s", "onset_frame", "rsp_tier", "rsp_flags", "n_anticipations",
                               "n_early" };
    mxArray* s = mxCreateStructMatrix(1, 1, sizeof f / sizeof f[0], f);
    int on = r->t_onset != 0, resp = (r->flags & YRSP_R_RESPONDED) != 0;
    double nan = mxGetNaN();
    mxSetField(s, 0, "response", mxCreateString(r->response_name ? r->response_name : ""));
    mxSetField(s, 0, "rt_s", rsp_s(r->rt_ns, r->rt_ns == YRSP_NO_TIME));
    mxSetField(s, 0, "rt_key_duration_s", rsp_s(r->rt_key_duration_ns, r->rt_key_duration_ns == YRSP_NO_TIME));
    mxSetField(s, 0, "onset_src", rsp_d(on ? (double)r->onset_src : nan));
    mxSetField(s, 0, "onset_tier", rsp_d(on ? (double)r->onset_tier : nan));
    mxSetField(s, 0, "onset_residual_s", rsp_s(r->onset_residual, !on));
    mxSetField(s, 0, "landing_residual_s", rsp_s(r->plan_residual, r->plan_residual == YRSP_NO_TIME));
    mxSetField(s, 0, "onset_frame", rsp_d(r->onset_frame >= 0 ? (double)r->onset_frame : nan));
    mxSetField(s, 0, "rsp_tier", rsp_d(resp ? (double)r->stamp_tier : nan));
    mxSetField(s, 0, "rsp_flags", rsp_d(r->flags));
    mxSetField(s, 0, "n_anticipations", rsp_d(r->n_anticipations));
    mxSetField(s, 0, "n_early", rsp_d(r->n_early));
    return s;
}

static mxArray* rsp_entries_out(const yrsp_entry* e, int n) {
    static const char* f[] = { "t", "t_ns", "t_release", "t_release_ns", "kind", "device", "control", "code", "choice",
                               "flags", "stamp", "mods", "value", "arrival_class" };
    mxArray* s = mxCreateStructMatrix(1, (size_t)n, sizeof f / sizeof f[0], f);
    int i;
    for (i = 0; i < n; i++) {
        int rel = (e[i].flags & YRSP_E_RELEASED) != 0;
        mxSetFieldByNumber(s, i, 0, rsp_s(e[i].t, 0));
        mxSetFieldByNumber(s, i, 1, rsp_i64(e[i].t));
        mxSetFieldByNumber(s, i, 2, rsp_s(e[i].t_release, !rel));
        mxSetFieldByNumber(s, i, 3, rsp_i64(rel ? e[i].t_release : YRSP_NO_TIME));
        mxSetFieldByNumber(s, i, 4, rsp_d(e[i].kind));
        mxSetFieldByNumber(s, i, 5, rsp_d(e[i].device));
        mxSetFieldByNumber(s, i, 6, rsp_d(e[i].control));
        mxSetFieldByNumber(s, i, 7, rsp_d(e[i].code));
        mxSetFieldByNumber(s, i, 8, rsp_d(e[i].choice + 1));
        mxSetFieldByNumber(s, i, 9, rsp_d(e[i].flags));
        mxSetFieldByNumber(s, i, 10, rsp_d(e[i].stamp));
        mxSetFieldByNumber(s, i, 11, rsp_d(e[i].mods));
        mxSetFieldByNumber(s, i, 12, rsp_d(e[i].value));
        mxSetFieldByNumber(s, i, 13, rsp_d(e[i].arrival_class));
    }
    return s;
}

/* ---- open ---------------------------------------------------------------- */

static const char* const open_fields[] = { "choices", "mode", "kinds", "min_rt", "duration", "persist", "max_responses",
                                           "allow_held_key", "wait_for_key_release", "max_hold", "sources",
                                           "trace_cap", "trace_kinds" };
static const char* const choice_fields[] = { "key", "name", "kind", "match", "cross", "device", "control", "code",
                                             "level", "hysteresis" };
static const char* const source_fields[] = { "kind", "device", "tier", "partial", "lo", "hi", "note" };

static char* rsp_keep(rsp_handle* h, const mxArray* a, const char* what) {
    char* s = rsp_utf8(a, what);
    if (h->n_strs >= RSP_MAX_STRS) pm_err("arg", "too many strings");
    h->strs[h->n_strs++] = s;
    return s;
}

static void rsp_check_fields_n(const mxArray* s, const char* const* allowed, int n, const char* what) {
    int i, j, nf = mxGetNumberOfFields(s);
    for (i = 0; i < nf; i++) {
        const char* f = mxGetFieldNameByNumber(s, i);
        int ok = 0;
        for (j = 0; j < n; j++) if (strcmp(f, allowed[j]) == 0) { ok = 1; break; }
        if (!ok) pm_err("arg", "%s: unknown field '%s'", what, f);
    }
}

static void rsp_choice_in(rsp_handle* h, const mxArray* s, size_t i, yrsp_choice* c) {
    const mxArray* f;
    memset(c, 0, sizeof *c);
#define CF(name) ((f = mxGetField(s, i, name)) != NULL && !mxIsEmpty(f))
    if (CF("key")) c->key = rsp_keep(h, f, "choice key");
    if (CF("name")) c->name = rsp_keep(h, f, "choice name");
    if (CF("kind")) c->kind = (uint8_t)rsp_enum(f, rsp_param_values("choices.kind"), 0, 15, "choice kind");
    if (CF("match")) c->match = (uint8_t)rsp_enum(f, rsp_param_values("choices.match"), 0, 255, "choice match");
    if (CF("cross")) c->cross = (uint8_t)rsp_enum(f, rsp_param_values("choices.cross"), 0, 255, "choice cross");
    if (CF("device")) c->device = (uint32_t)rsp_int(f, "choice device", 0, 0xFFFFFFFFLL);
    if (CF("control")) c->control = rsp_key(f, "choice control");
    if (CF("code")) c->code = (uint32_t)rsp_int(f, "choice code", 0, 0xFFFFFFFFLL);
    if (CF("level")) c->level = (float)pm_scalar(f, "choice level");
    if (CF("hysteresis")) c->hysteresis = (float)pm_scalar(f, "choice hysteresis");
#undef CF
}

static void rsp_source_in(rsp_handle* h, const mxArray* s, size_t i, yin_source* src) {
    const mxArray* f;
    memset(src, 0, sizeof *src);
#define SF(name) ((f = mxGetField(s, i, name)) != NULL && !mxIsEmpty(f))
    if (SF("kind")) src->kind = (uint8_t)rsp_enum(f, rsp_param_values("choices.kind"), 0, 255, "source kind");
    if (SF("device")) src->device = (uint32_t)rsp_int(f, "source device", 0, 0xFFFFFFFFLL);
    if (SF("tier")) src->tier = (uint8_t)rsp_int(f, "source tier", 0, 255);
    if (SF("partial")) src->partial = (uint8_t)pm_bool(f, "source partial");
    if (SF("lo")) src->lo_ns = rsp_seconds(f, "source lo", S_NEG);
    if (SF("hi")) src->hi_ns = rsp_seconds(f, "source hi", S_NEG);
    if (SF("note")) src->note = rsp_keep(h, f, "source note");
#undef SF
}

static uint64_t rsp_open(const mxArray* d) {
    rsp_handle* h = (rsp_handle*)mxCalloc(1, sizeof *h);
    yrsp_choice ch[YRSP_MAX_CHOICES];
    yin_source src[YRSP_MAX_SOURCES];
    yrsp_desc desc;
    const mxArray* f;
    int i;
    memset(&desc, 0, sizeof desc);
    if (d) {
        pm_check_fields(d, open_fields, (int)(sizeof open_fields / sizeof open_fields[0]), "desc");
        if ((f = pm_field(d, "choices"))) {
            size_t n = mxGetNumberOfElements(f), k;
            if (n > YRSP_MAX_CHOICES) pm_err("arg", "choices: at most %d", YRSP_MAX_CHOICES);
            if (mxIsCell(f)) {   /* key names */
                for (k = 0; k < n; k++) {
                    const mxArray* c = mxGetCell(f, k);
                    memset(&ch[k], 0, sizeof ch[k]);
                    if (!c || !mxIsChar(c)) pm_err("arg", "choices: a cell of key names, or a struct array");
                    ch[k].key = rsp_keep(h, c, "choice");
                }
            } else if (mxIsStruct(f)) {
                rsp_check_fields_n(f, choice_fields, (int)(sizeof choice_fields / sizeof choice_fields[0]), "choices");
                for (k = 0; k < n; k++) rsp_choice_in(h, f, k, &ch[k]);
            } else if (mxIsChar(f)) {
                pm_err("arg", "choices: a cell of key names ({'f', 'j'}), not one string");
            } else {
                pm_err("arg", "choices: a cell of key names, or a struct array");
            }
            desc.choices = ch;
            desc.n_choices = (int)n;
        }
        if ((f = pm_field(d, "mode"))) desc.mode = rsp_enum(f, rsp_param_values("mode"), 0, 2, "mode");
        if ((f = pm_field(d, "kinds"))) desc.kinds = rsp_kinds(f, "kinds");
        if ((f = pm_field(d, "trace_kinds"))) desc.trace_kinds = rsp_kinds(f, "trace_kinds");
        if ((f = pm_field(d, "min_rt"))) desc.min_rt_ns = rsp_seconds(f, "min_rt", 0);
        if ((f = pm_field(d, "duration"))) {
            desc.duration_ns = rsp_seconds(f, "duration", S_INF);
            if (desc.duration_ns == INT64_MAX) desc.duration_ns = 0;   /* Inf: no deadline */
        }
        if ((f = pm_field(d, "max_hold"))) desc.max_hold_ns = rsp_seconds(f, "max_hold", 0);
        if ((f = pm_field(d, "persist"))) desc.persist = pm_bool(f, "persist") != 0;
        if ((f = pm_field(d, "max_responses"))) desc.max_responses = pm_int(f, "max_responses");
        if ((f = pm_field(d, "allow_held_key"))) desc.allow_held_key = pm_bool(f, "allow_held_key") != 0;
        if ((f = pm_field(d, "wait_for_key_release"))) desc.wait_for_key_release = pm_bool(f, "wait_for_key_release") != 0;
        if ((f = pm_field(d, "sources"))) {
            size_t n = mxGetNumberOfElements(f), k;
            if (!mxIsStruct(f)) pm_err("arg", "sources: a struct array (kind, device, tier, partial, lo, hi, note)");
            if (n > YRSP_MAX_SOURCES) pm_err("arg", "sources: at most %d", YRSP_MAX_SOURCES);
            rsp_check_fields_n(f, source_fields, (int)(sizeof source_fields / sizeof source_fields[0]), "sources");
            for (k = 0; k < n; k++) rsp_source_in(h, f, k, &src[k]);
            desc.sources = src;
            desc.n_sources = (int)n;
        }
        if ((f = pm_field(d, "trace_cap"))) {
            int cap = pm_int(f, "trace_cap");
            if (cap < 0 || cap > 10000000) pm_err("arg", "trace_cap: 0..10000000 events");
            if (cap > 0) {
                h->trace = (yin_event*)mxMalloc((size_t)cap * sizeof(yin_event));
                h->trace_cap = cap;
                desc.trace = h->trace;
                desc.trace_cap = cap;
            }
        }
    }
    if (!yrsp_open(&h->c, &desc)) pm_err("arg", "%s", yrsp_error(&h->c));
    mexMakeMemoryPersistent(h);
    for (i = 0; i < h->n_strs; i++) mexMakeMemoryPersistent(h->strs[i]);
    if (h->trace) mexMakeMemoryPersistent(h->trace);
    return pm_register(h);
}

/* ---- set_onset and score options ---------------------------------------- */

static const char* const onset_fields[] = { "final", "src", "tier", "residual", "uncertainty", "frame", "arm", "end" };

static void rsp_onset_opts(const mxArray* o, yrsp_onset* on, int allow_arm, int64_t* arm, int64_t* end, int* has_end) {
    const mxArray* f;
    on->final = 1;
    on->frame = -1;
    if (!o) return;
    pm_check_fields(o, onset_fields, allow_arm ? 8 : 6, "onset options");
    if ((f = pm_field(o, "final"))) on->final = (uint8_t)(pm_bool(f, "final") != 0);
    if ((f = pm_field(o, "src"))) on->src = (uint8_t)rsp_enum(f, onset_src_values, 0, 255, "src");
    if ((f = pm_field(o, "tier"))) on->tier = (uint8_t)rsp_int(f, "tier", 0, 255);
    if ((f = pm_field(o, "residual"))) on->residual_ns = rsp_seconds(f, "residual", S_NEG);
    if ((f = pm_field(o, "uncertainty"))) on->uncertainty_ns = rsp_seconds(f, "uncertainty", 0);
    if ((f = pm_field(o, "frame"))) on->frame = (int64_t)rsp_int(f, "frame", -1, 9007199254740992LL);
    if (allow_arm) {
        if ((f = pm_field(o, "arm"))) *arm = rsp_seconds(f, "arm", 0);
        if ((f = pm_field(o, "end"))) { *end = rsp_seconds(f, "end", 0); *has_end = 1; }
    }
}

/* ---- constants ----------------------------------------------------------- */

static mxArray* rsp_constants(void) {
    static const struct { const char* name; double v; } k[] = {
        { "KIND_KEYBOARD", YIN_KIND_KEYBOARD }, { "KIND_MOUSE", YIN_KIND_MOUSE }, { "KIND_TOUCH", YIN_KIND_TOUCH },
        { "KIND_PEN", YIN_KIND_PEN }, { "KIND_GAMEPAD", YIN_KIND_GAMEPAD }, { "KIND_BOX", YIN_KIND_BOX },
        { "KIND_EYE", YIN_KIND_EYE }, { "KIND_SYNC", YIN_KIND_SYNC }, { "KIND_USER", YIN_KIND_USER },
        { "KINDS_ALL", YIN_KINDS_ALL },
        { "NONE", YIN_NONE }, { "PRESS", YIN_PRESS }, { "RELEASE", YIN_RELEASE }, { "SAMPLE", YIN_SAMPLE },
        { "PROXIMITY", YIN_PROXIMITY },
        { "REPEAT", YIN_REPEAT }, { "SYNTHETIC", YIN_SYNTHETIC }, { "ERASER", YIN_ERASER }, { "UNLISTED", YIN_UNLISTED },
        { "DEVTICKS", YIN_DEVTICKS },
        { "TIER_UNKNOWN", YIN_TIER_UNKNOWN }, { "TIER_1", YIN_TIER_1 }, { "TIER_2", YIN_TIER_2 }, { "TIER_3", YIN_TIER_3 },
        { "TIER_SIM", YIN_TIER_SIM },
        { "AXIS_POSITION", YIN_AXIS_POSITION }, { "AXIS_PEN", YIN_AXIS_PEN }, { "AXIS_PEN_PRESSURE", YIN_AXIS_PEN_PRESSURE },
        { "AXIS_GAMEPAD", YIN_AXIS_GAMEPAD }, { "AXIS_DELTA", YIN_AXIS_DELTA }, { "AXIS_ABSOLUTE", YIN_AXIS_ABSOLUTE },
        { "AXIS_WHEEL", YIN_AXIS_WHEEL },
        { "EYE_GAZE", YIN_EYE_GAZE }, { "EYE_FIXATION", YIN_EYE_FIXATION }, { "EYE_SACCADE", YIN_EYE_SACCADE },
        { "EYE_BLINK", YIN_EYE_BLINK }, { "EYE_LEFT", YIN_EYE_LEFT }, { "EYE_RIGHT", YIN_EYE_RIGHT },
        { "EYE_BOTH", YIN_EYE_BOTH }, { "UNC_MAX_US", YIN_UNC_MAX },
        { "IDLE", YRSP_IDLE }, { "OPEN", YRSP_OPEN }, { "ENDED", YRSP_ENDED },
        { "CHOICES_ALL", YRSP_CHOICES_ALL }, { "CHOICES_LIST", YRSP_CHOICES_LIST }, { "CHOICES_NONE", YRSP_CHOICES_NONE },
        { "MATCH_SCANCODE", YRSP_MATCH_SCANCODE }, { "MATCH_KEYCODE", YRSP_MATCH_KEYCODE },
        { "CROSS_NONE", YRSP_CROSS_NONE }, { "CROSS_RISING", YRSP_CROSS_RISING }, { "CROSS_FALLING", YRSP_CROSS_FALLING },
        { "CROSS_DISTANCE", YRSP_CROSS_DISTANCE },
        { "ONSET_OTHER", YRSP_ONSET_OTHER }, { "ONSET_FLIP", YRSP_ONSET_FLIP }, { "ONSET_AUDIO", YRSP_ONSET_AUDIO },
        { "ONSET_PLAN", YRSP_ONSET_PLAN },
        { "E_VALID", YRSP_E_VALID }, { "E_EARLY", YRSP_E_EARLY }, { "E_ANTICIPATION", YRSP_E_ANTICIPATION },
        { "E_LATE", YRSP_E_LATE }, { "E_AFTER_END", YRSP_E_AFTER_END }, { "E_NOT_CHOICE", YRSP_E_NOT_CHOICE },
        { "E_HELD", YRSP_E_HELD }, { "E_CROSSING", YRSP_E_CROSSING }, { "E_RECLASSIFIED", YRSP_E_RECLASSIFIED },
        { "E_RELEASED", YRSP_E_RELEASED }, { "E_ENDED", YRSP_E_ENDED }, { "E_CLASS_MASK", YRSP_E_CLASS_MASK },
        { "R_RESPONDED", YRSP_R_RESPONDED }, { "R_TIMEOUT", YRSP_R_TIMEOUT }, { "R_ENDED_EARLY", YRSP_R_ENDED_EARLY },
        { "R_ONSET_PLAN", YRSP_R_ONSET_PLAN }, { "R_NO_ONSET", YRSP_R_NO_ONSET }, { "R_LOG_FULL", YRSP_R_LOG_FULL },
        { "R_TRACE_FULL", YRSP_R_TRACE_FULL }, { "R_RECLASSIFIED", YRSP_R_RECLASSIFIED },
        { "R_HELD_AT_OPEN", YRSP_R_HELD_AT_OPEN }, { "R_HOLD_TIMEOUT", YRSP_R_HOLD_TIMEOUT },
        { "R_SENT_MISMATCH", YRSP_R_SENT_MISMATCH },
        { "MAX_CHOICES", YRSP_MAX_CHOICES }, { "MAX_SOURCES", YRSP_MAX_SOURCES }, { "LOG_CAP", YRSP_LOG_CAP },
        { "LOG_RESERVE", YRSP_LOG_RESERVE }, { "MAX_SENT", YRSP_MAX_SENT },
    };
    size_t n = sizeof k / sizeof k[0], i;
    const char* names[sizeof k / sizeof k[0]];
    mxArray* s;
    for (i = 0; i < n; i++) names[i] = k[i].name;
    s = mxCreateStructMatrix(1, 1, (int)n, names);
    for (i = 0; i < n; i++) mxSetFieldByNumber(s, 0, (int)i, mxCreateDoubleScalar(k[i].v));
    return s;
}

/* ---- dispatch ------------------------------------------------------------ */

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char* cmd;
    rsp_handle* h;
    pm_destroy = rsp_destroy;
    if (nrhs < 1 || !mxIsChar(prhs[0])) pm_err("usage", "first argument must be a command string");
    cmd = mxArrayToString(prhs[0]);

    /* ---- commands without a handle ---- */
    if (strcmp(cmd, "version") == 0) {
        plhs[0] = mxCreateString(MEX_BINDING_VERSION);
        if (nlhs > 1) plhs[1] = mxCreateString(yrsp_version());
        return;
    }
    if (strcmp(cmd, "constants") == 0) { plhs[0] = rsp_constants(); return; }
    if (strcmp(cmd, "scancode") == 0) {
        char* s;
        int k;
        pm_nargs(nrhs, 2, "sc = ysp_response('scancode', name)");
        s = rsp_utf8(prhs[1], "name");
        k = yrsp_scancode(s);
        plhs[0] = k < 0 ? mxCreateDoubleMatrix(0, 0, mxREAL) : mxCreateDoubleScalar(k);
        return;
    }
    if (strcmp(cmd, "key_name") == 0) {
        const char* n;
        pm_nargs(nrhs, 2, "name = ysp_response('key_name', scancode)");
        n = yrsp_key_name((uint32_t)rsp_int(prhs[1], "scancode", 0, 0xFFFFFFFFLL));
        plhs[0] = mxCreateString(n ? n : "");
        return;
    }
    if (strcmp(cmd, "event") == 0) {
        /* a full event from a struct with some of its fields: t_ns, key
         * names and the defaults filled in (type PRESS, value 1 for a press,
         * 0 else); 'feed' itself reads a missing field as 0 */
        int fn[EV_NF];
        size_t n, i;
        yin_event* e;
        pm_nargs(nrhs, 2, "ev = ysp_response('event', struct('t', t, 'control', 'f', ...))");
        rsp_event_fields(prhs[1], fn);
        n = mxGetNumberOfElements(prhs[1]);
        e = (yin_event*)mxMalloc((n ? n : 1) * sizeof *e);
        for (i = 0; i < n; i++) {
            const mxArray* ty = fn[7] >= 0 ? mxGetFieldByNumber(prhs[1], i, fn[7]) : NULL;
            rsp_event_in(prhs[1], i, fn, &e[i]);
            if (!ty || mxIsEmpty(ty)) e[i].type = (uint8_t)YIN_PRESS;   /* as Python's event() */
            if (fn[13] < 0 || !mxGetFieldByNumber(prhs[1], i, fn[13]) || mxIsEmpty(mxGetFieldByNumber(prhs[1], i, fn[13])))
                e[i].value = e[i].type == YIN_PRESS ? 1.0f : 0.0f;   /* ysp/input.h: 1 for a press */
        }
        plhs[0] = rsp_events_out(e, (int)n);
        return;
    }
    if (strcmp(cmd, "open") == 0) {
        plhs[0] = pm_handle_out(rsp_open(nrhs > 1 && !mxIsEmpty(prhs[1]) ? prhs[1] : NULL));
        return;
    }

    /* ---- commands on a handle ---- */
    pm_nargs(nrhs, 2, "ysp_response(cmd, h, ...)");
    h = (rsp_handle*)pm_lookup(prhs[1]);

    if (strcmp(cmd, "close") == 0) {
        pm_unregister(h);
        rsp_destroy(h);
    } else if (strcmp(cmd, "arm") == 0) {
        pm_nargs(nrhs, 3, "ysp_response('arm', h, t)");
        yrsp_arm(&h->c, rsp_seconds(prhs[2], "arm t", 0));
    } else if (strcmp(cmd, "set_onset") == 0) {
        yrsp_onset o;
        pm_nargs(nrhs, 3, "st = ysp_response('set_onset', h, t, [struct('final', 1, 'src', 'flip', 'tier', 1, ...)])");
        memset(&o, 0, sizeof o);
        o.t_ns = mxIsInt64(prhs[2]) ? rsp_ns_arg(prhs[2], "onset t_ns") : rsp_seconds(prhs[2], "onset t", 0);
        rsp_onset_opts(nrhs > 3 && !mxIsEmpty(prhs[3]) ? prhs[3] : NULL, &o, 0, NULL, NULL, NULL);
        {
            int rc = yrsp_set_onset(&h->c, &o);
            rsp_check(h, rc);
            plhs[0] = mxCreateDoubleScalar(rc);
        }
    } else if (strcmp(cmd, "feed") == 0) {
        /* one call per frame: every event the frame's loop read, in order */
        int fn[EV_NF], rc = 0;
        size_t n, i;
        yin_event e;
        pm_nargs(nrhs, 3, "st = ysp_response('feed', h, events)");
        rsp_event_fields(prhs[2], fn);
        n = mxGetNumberOfElements(prhs[2]);
        for (i = 0; i < n; i++) {
            rsp_event_in(prhs[2], i, fn, &e);
            rc = yrsp_feed(&h->c, &e);
            rsp_check(h, rc);
        }
        /* with no event, the status: update() at INT64_MIN reaches no deadline */
        if (n == 0) rsp_check(h, rc = yrsp_update(&h->c, INT64_MIN));
        plhs[0] = mxCreateDoubleScalar(rc);
    } else if (strcmp(cmd, "key") == 0) {
        yin_event e;
        int down = 1, rc;
        pm_nargs(nrhs, 4, "st = ysp_response('key', h, t, key, [down], [device])");
        memset(&e, 0, sizeof e);
        e.t = mxIsInt64(prhs[2]) ? rsp_ns_arg(prhs[2], "key t_ns") : rsp_seconds(prhs[2], "key t", 0);
        e.control = rsp_key(prhs[3], "key");
        if (nrhs > 4 && !mxIsEmpty(prhs[4])) down = pm_bool(prhs[4], "down");
        if (nrhs > 5 && !mxIsEmpty(prhs[5])) e.device = (uint32_t)rsp_int(prhs[5], "device", 0, 0xFFFFFFFFLL);
        e.kind = (uint8_t)YIN_KIND_KEYBOARD;
        e.type = (uint8_t)(down ? YIN_PRESS : YIN_RELEASE);
        e.value = down ? 1.0f : 0.0f;
        rc = yrsp_feed(&h->c, &e);
        rsp_check(h, rc);
        plhs[0] = mxCreateDoubleScalar(rc);
    } else if (strcmp(cmd, "update") == 0) {
        int rc;
        pm_nargs(nrhs, 3, "st = ysp_response('update', h, now)");
        rc = yrsp_update(&h->c, rsp_seconds(prhs[2], "update now", 0));
        rsp_check(h, rc);
        plhs[0] = mxCreateDoubleScalar(rc);
    } else if (strcmp(cmd, "finish") == 0 || strcmp(cmd, "score") == 0) {
        if (cmd[0] == 's') {
            /* offline: one trial of a logged stream. The events stamped
             * before arm that come first are fed before yrsp_arm(), as a
             * live loop fed them; the rest after the arm and the onset. */
            int fn[EV_NF], armed = 0, has_end = 0, rc;
            size_t n, i;
            int64_t t_arm, t_end = 0;
            yrsp_onset o;
            yin_event e;
            pm_nargs(nrhs, 4, "[r, row] = ysp_response('score', h, events, onset, [struct('arm', t, 'end', t, 'src', ...)])");
            memset(&o, 0, sizeof o);
            o.t_ns = rsp_seconds(prhs[3], "onset", 0);
            t_arm = o.t_ns;
            rsp_onset_opts(nrhs > 4 && !mxIsEmpty(prhs[4]) ? prhs[4] : NULL, &o, 1, &t_arm, &t_end, &has_end);
            rsp_event_fields(prhs[2], fn);
            n = mxGetNumberOfElements(prhs[2]);
            for (i = 0; i < n; i++) {
                rsp_event_in(prhs[2], i, fn, &e);
                if (!armed && e.t >= t_arm) {
                    yrsp_arm(&h->c, t_arm);
                    rsp_check(h, yrsp_set_onset(&h->c, &o));
                    armed = 1;
                }
                rsp_check(h, yrsp_feed(&h->c, &e));
            }
            if (!armed) {
                yrsp_arm(&h->c, t_arm);
                rsp_check(h, yrsp_set_onset(&h->c, &o));
            }
            if (has_end) rsp_check(h, yrsp_update(&h->c, t_end));
            rc = yrsp_finish(&h->c, &h->last);
            rsp_check(h, rc);
        } else {
            rsp_check(h, yrsp_finish(&h->c, &h->last));
        }
        h->have_last = 1;
        plhs[0] = rsp_result_out(h, &h->last);
        if (nlhs > 1) plhs[1] = rsp_row_out(&h->last);
    } else if (strcmp(cmd, "entries") == 0) {
        int n;
        const yrsp_entry* e = yrsp_entries(&h->c, &n);
        plhs[0] = rsp_entries_out(e, n);
    } else if (strcmp(cmd, "trace") == 0) {
        int n = h->have_last && h->trace ? h->last.n_trace : 0;
        if (n > h->trace_cap) n = h->trace_cap;
        plhs[0] = rsp_events_out(h->trace, n);
    } else if (strcmp(cmd, "set_sent") == 0) {
        /* sent: a struct array (t or t_ns, kind, device, control, choice
         * 1-based with 0 none, written) */
        yrsp_sent buf[YRSP_MAX_SENT];
        const mxArray* s;
        const mxArray* f;
        size_t n, k, m;
        int total;
        pm_nargs(nrhs, 3, "ysp_response('set_sent', h, sent, [total])");
        s = prhs[2];
        n = mxIsEmpty(s) ? 0 : mxGetNumberOfElements(s);
        if (n && !mxIsStruct(s)) pm_err("arg", "sent: a struct array (t, kind, device, control, choice, written)");
        m = n < YRSP_MAX_SENT ? n : YRSP_MAX_SENT;
        for (k = 0; k < m; k++) {
            memset(&buf[k], 0, sizeof buf[k]);
#define SF(name) ((f = mxGetField(s, k, name)) != NULL && !mxIsEmpty(f))
            if (SF("t_ns")) buf[k].t = rsp_ns_arg(f, "sent t_ns");
            else if (SF("t")) buf[k].t = rsp_seconds(f, "sent t", 0);
            else pm_err("arg", "sent %d: needs t or t_ns", (int)k + 1);
            if (SF("kind")) buf[k].kind = (uint8_t)rsp_enum(f, rsp_param_values("choices.kind"), 0, 15, "sent kind");
            if (SF("device")) buf[k].device = (uint32_t)rsp_int(f, "sent device", 0, 0xFFFFFFFFLL);
            if (SF("control")) buf[k].control = rsp_key(f, "sent control");
            buf[k].choice = SF("choice") ? (int16_t)(rsp_int(f, "sent choice", 0, 32767) - 1) : (int16_t)-1;
            buf[k].written = (uint8_t)(SF("written") ? pm_bool(f, "sent written") != 0 : 1);
#undef SF
        }
        total = (int)n;
        if (nrhs > 3) total = (int)rsp_int(prhs[3], "total", (long long)n, 2147483647LL);
        rsp_check(h, yrsp_set_sent(&h->c, buf, (int)m, total));
    } else if (strcmp(cmd, "data_row") == 0) {
        if (!h->have_last) pm_err("state", "data_row: no finish yet");
        plhs[0] = rsp_row_out(&h->last);
    } else if (strcmp(cmd, "plan_frame") == 0) {
        int64_t k = yrsp_plan_frame(&h->c);
        plhs[0] = k < 0 ? mxCreateDoubleMatrix(0, 0, mxREAL) : mxCreateDoubleScalar((double)k);
    } else if (strcmp(cmd, "error") == 0) {
        plhs[0] = mxCreateString(yrsp_error(&h->c));
    } else {
        pm_err("usage", "unknown command '%s'", cmd);
    }
}
