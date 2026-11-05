/* ysp_rt.c - MATLAB/Octave MEX binding for ysp/rt.h (builds to ysp_rt.<mexext>)
 *
 * The clock, the deadline waits, the clock correlation, the device clock
 * fit, the virtual clock and the per-user folders, for a Psychtoolbox
 * experiment that wants its own stamps on the clock of the ysp data and
 * devices, and for analysis code that maps a device's clock to it.
 *
 *     t    = ysp_rt('now')                        % seconds; ysp_serial('now') reads the same clock
 *     late = ysp_rt('sleep_until', t + 0.5)       % seconds late
 *     c    = ysp_rt('correlate', @GetSecs)        % struct t, other, width, tries, t_ns
 *     h    = ysp_rt('fit_open', struct('mode', 'late', 'tick', 1e-6, 'bits', 32));
 *     ysp_rt('fit_add', h, us, t);                % vectors of pairs are fine
 *     [s, ns] = ysp_rt('fit_map', h, event_us);   % event times on the ysp clock
 *     ysp_rt('fit_close', h);
 *
 * Units: every time is a double in seconds; the C API takes int64 ns, and
 * the conversion happens here, rounded to the nearest ns. Negative and NaN
 * times are refused. 'now_ns' and the second output of 'fit_map' give the
 * exact int64 ns.
 *
 * A wait blocks MATLAB for its whole length and Ctrl+C does not end it: the
 * MEX API has no documented way to see the key while C code runs.
 *
 * Not bound (docs/rt.md, "Bindings"): the worker, the pump, the event ring,
 * the scheduling ladder, virtual timers and the instrumentation macros.
 *
 * Build with build.m. See README.md.
 */
#define PM_MOD "ysp_rt"
#include "ysp_mex_util.h"

#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"

/* The binding's own version (README.md); the second output of 'version'
 * is the header's. */
#define RT_BINDING_VERSION "1.0.0"

static void rt_destroy(void* obj) { free(obj); }

/* Seconds to int64 ns: the whole seconds exactly and the fraction scaled, so
 * a time read from 'now' comes back to the same ns below 2^23 s (97 days). */
static int64_t rt_ns(const mxArray* a, const char* what) {
    double s = pm_scalar(a, what), whole;
    if (!(s >= 0)) pm_err("arg", "%s: seconds, >= 0 (not NaN)", what);
    if (!(s < 9223372036.0)) pm_err("arg", "%s: seconds, finite and < 9223372036 (int64 ns)", what);
    whole = floor(s);
    return (int64_t)whole * 1000000000 + (int64_t)llround((s - whole) * 1e9);
}

static double rt_s(int64_t ns) { return (double)ns / 1e9; }

static mxArray* rt_i64(int64_t v) {
    mxArray* a = mxCreateNumericMatrix(1, 1, mxINT64_CLASS, mxREAL);
    *(int64_t*)mxGetData(a) = v;
    return a;
}

/* Device counters: uint64 or int64 (>= 0) arrays as they are, or doubles
 * that hold integers below 2^53. Copied into mxMalloc memory, which MATLAB
 * frees when the call returns. */
static uint64_t* rt_ticks(const mxArray* a, size_t* n) {
    size_t i, k = mxGetNumberOfElements(a);
    uint64_t* t = (uint64_t*)mxMalloc((k ? k : 1) * sizeof *t);
    *n = k;
    if (mxIsUint64(a)) {
        memcpy(t, mxGetData(a), k * sizeof *t);
    } else if (mxIsInt64(a)) {
        const int64_t* p = (const int64_t*)mxGetData(a);
        for (i = 0; i < k; i++) {
            if (p[i] < 0) pm_err("arg", "ticks: >= 0");
            t[i] = (uint64_t)p[i];
        }
    } else if (mxIsDouble(a) && !mxIsComplex(a)) {
        const double* p = mxGetPr(a);
        for (i = 0; i < k; i++) {
            if (!(p[i] >= 0) || p[i] != floor(p[i]) || p[i] > 9007199254740992.0)
                pm_err("arg", "ticks: integers in 0..2^53 as doubles, or a uint64 array");
            t[i] = (uint64_t)p[i];
        }
    } else {
        pm_err("arg", "ticks: a uint64 array, or doubles that hold integers");
    }
    return t;
}

/* ---- correlation --------------------------------------------------------- */

/* The other clock is a MATLAB function. An integer class is the clock's own
 * counter; a double is seconds and is carried as ns, so 'other' comes back
 * as seconds again. A failure cannot leave the header's loop (an error here
 * would jump out of it), so it is kept and raised after the call. */
typedef struct rt_corr_ctx {
    mxArray*    fn;
    int         kind;      /* 0 not yet, 1 integer, 2 double seconds */
    const char* bad;       /* a message to raise after the call      */
    uint64_t    fake;
} rt_corr_ctx;

static uint64_t rt_corr_read(void* p) {
    rt_corr_ctx* c = (rt_corr_ctx*)p;
    mxArray* out = NULL;
    uint64_t v = 0;
    int kind = 0;
    /* After a failure the header still finishes its loop; a value that
     * changes on every read ends edge mode after `tries` reads instead of at
     * the timeout. The result is thrown away. */
    if (c->bad || pm_pending) return ++c->fake;
    if (!pm_feval(1, &out, 1, &c->fn) || !out) {
        if (!pm_pending) c->bad = "correlate: the clock function returned nothing";
        return ++c->fake;
    }
    if (mxGetNumberOfElements(out) != 1 || mxIsComplex(out) || !mxIsNumeric(out)) {
        c->bad = "correlate: the clock function must return a real numeric scalar";
    } else if (mxIsDouble(out) || mxIsSingle(out)) {
        double s = mxGetScalar(out), whole;
        kind = 2;
        if (!(s >= 0) || !(s < 9223372036.0)) c->bad = "correlate: a clock in seconds must be >= 0 and finite";
        else { whole = floor(s); v = (uint64_t)((int64_t)whole * 1000000000 + (int64_t)llround((s - whole) * 1e9)); }
    } else if (mxIsUint64(out)) {
        kind = 1;
        v = *(const uint64_t*)mxGetData(out);
    } else {
        double d = mxGetScalar(out);   /* the other integer classes fit a double */
        kind = 1;
        if (d < 0) c->bad = "correlate: a counter must be >= 0";
        else v = mxIsInt64(out) ? (uint64_t)*(const int64_t*)mxGetData(out) : (uint64_t)d;
    }
    mxDestroyArray(out);
    if (!c->bad && c->kind && kind != c->kind) c->bad = "correlate: the clock returned an integer once and a double once";
    if (!c->bad) c->kind = kind;
    return v;
}

static const char* const rt_corr_fields[] = { "edge", "tries", "timeout" };

static void cmd_correlate(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    static const char* f[] = { "t", "other", "width", "tries", "t_ns" };
    yrt_corr_desc d;
    yrt_corr o;
    rt_corr_ctx c;
    const mxArray* v;
    mxArray* s;
    int rc;
    (void)nlhs;
    pm_nargs(nrhs, 2, "c = ysp_rt('correlate', clockfn, [struct('edge', false, 'tries', 0, 'timeout', 0.1)])");
    if (!mxIsClass(prhs[1], "function_handle")) pm_err("arg", "correlate: the clock must be a function handle");
    memset(&d, 0, sizeof d);
    memset(&c, 0, sizeof c);
    if (nrhs > 2) {
        pm_check_fields(prhs[2], rt_corr_fields, 3, "correlate options");
        if ((v = pm_field(prhs[2], "edge"))) d.edge = pm_bool(v, "edge") != 0;
        if ((v = pm_field(prhs[2], "tries"))) {
            d.tries = pm_int(v, "tries");
            if (d.tries < 0) pm_err("arg", "tries: >= 0 (0 = 16, or 4 edges)");
        }
        if ((v = pm_field(prhs[2], "timeout"))) {
            d.timeout_ns = rt_ns(v, "timeout");
            if (d.timeout_ns == 0) d.timeout_ns = 1;   /* 0 in C means the default */
        }
    }
    c.fn = (mxArray*)prhs[1];
    d.read_ctx = rt_corr_read;
    d.ctx = &c;
    rc = yrt_correlate(&d, &o);
    pm_rethrow();
    if (c.bad) pm_err("arg", "%s", c.bad);
    if (rc == YRT_ERR_TIMEOUT) pm_err("timeout", "correlate: the clock did not change before the timeout");
    if (rc < 0) pm_err("arg", "correlate: %s", yrt_strerror(rc));
    s = mxCreateStructMatrix(1, 1, 5, f);
    mxSetField(s, 0, "t", mxCreateDoubleScalar(rt_s(o.rt_ns)));
    mxSetField(s, 0, "other", c.kind == 2 ? mxCreateDoubleScalar((double)o.other / 1e9) : pm_u64_out(o.other));
    mxSetField(s, 0, "width", mxCreateDoubleScalar(rt_s(o.width_ns)));
    mxSetField(s, 0, "tries", mxCreateDoubleScalar(o.tries));
    mxSetField(s, 0, "t_ns", rt_i64(o.rt_ns));
    plhs[0] = s;
}

/* ---- device clock fit ---------------------------------------------------- */

static const char* const rt_fit_fields[] = { "mode", "tick", "bits", "bucket", "slope_span",
                                             "restart", "restart_hold", "max_width" };
static const char* const rt_fit_modes[] = { "late", "bracket", "unbiased" };

static void cmd_fit_open(mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    yrt_fit_desc d;
    yrt_fit* f;
    const mxArray* v;
    double tick;
    pm_nargs(nrhs, 2, "h = ysp_rt('fit_open', struct('mode', 'late', 'tick', 1e-6, 'bits', 32))");
    pm_check_fields(prhs[1], rt_fit_fields, 8, "fit_open");
    memset(&d, 0, sizeof d);
    if ((v = pm_field(prhs[1], "mode"))) d.mode = pm_enum(v, "mode", rt_fit_modes, 3);
    if (!(v = pm_field(prhs[1], "tick"))) pm_err("arg", "fit_open: tick (the device's nominal tick in seconds) is required");
    tick = pm_finite(v, "tick");
    if (!(tick > 0)) pm_err("arg", "tick: seconds, > 0 (1e-3 for a ms timer)");
    d.ns_per_tick = tick * 1e9;
    if ((v = pm_field(prhs[1], "bits"))) {
        int b = pm_int(v, "bits");
        if (b < 0) pm_err("arg", "bits: 0 to 64");
        d.tick_bits = (uint32_t)b;
    }
    if ((v = pm_field(prhs[1], "bucket"))) d.bucket_ns = rt_ns(v, "bucket");
    if ((v = pm_field(prhs[1], "slope_span"))) d.slope_span_ns = rt_ns(v, "slope_span");
    if ((v = pm_field(prhs[1], "restart"))) d.restart_ns = rt_ns(v, "restart");
    if ((v = pm_field(prhs[1], "restart_hold"))) d.restart_hold_ns = rt_ns(v, "restart_hold");
    if ((v = pm_field(prhs[1], "max_width"))) d.max_width_ns = rt_ns(v, "max_width");
    f = (yrt_fit*)calloc(1, sizeof *f);
    if (!f) pm_err("memory", "out of memory");
    if (!yrt_fit_open(f, &d)) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s", yrt_fit_error(f));
        free(f);
        pm_err("arg", "fit_open: %s", msg);
    }
    plhs[0] = pm_handle_out(pm_register(f));
}

static void cmd_fit_add(mxArray* plhs[], int nrhs, const mxArray* prhs[], yrt_fit* f) {
    size_t n, i, nw = 0;
    uint64_t* ticks;
    const double *t, *w = NULL;
    double* out;
    pm_nargs(nrhs, 4, "flags = ysp_rt('fit_add', h, ticks, t, [width])");
    ticks = rt_ticks(prhs[2], &n);
    if (!mxIsDouble(prhs[3]) || mxIsComplex(prhs[3]) || mxGetNumberOfElements(prhs[3]) != n)
        pm_err("arg", "fit_add: t must be real doubles, one per tick");
    t = mxGetPr(prhs[3]);
    if (nrhs > 4) {
        nw = mxGetNumberOfElements(prhs[4]);
        if (!mxIsDouble(prhs[4]) || mxIsComplex(prhs[4]) || (nw != n && nw != 1))
            pm_err("arg", "fit_add: width must be one double or one per tick");
        w = mxGetPr(prhs[4]);
    }
    /* Every value is checked before the first pair goes in, so a refusal
     * leaves the fit as it was. */
    for (i = 0; i < n; i++)
        if (!(t[i] >= 0) || !(t[i] < 9223372036.0)) pm_err("arg", "t(%d): seconds, >= 0 and finite", (int)i + 1);
    for (i = 0; i < nw; i++)
        if (!(w[i] >= 0) || !(w[i] < 9223372036.0)) pm_err("arg", "width(%d): seconds, >= 0 and finite", (int)i + 1);
    plhs[0] = mxCreateDoubleMatrix(mxGetM(prhs[2]), mxGetN(prhs[2]), mxREAL);
    out = mxGetPr(plhs[0]);
    for (i = 0; i < n; i++) {
        double s = t[i], whole = floor(s), ws;
        int64_t tn = (int64_t)whole * 1000000000 + (int64_t)llround((s - whole) * 1e9), wn = 0;
        if (w) {
            ws = w[nw == 1 ? 0 : i];
            whole = floor(ws);
            wn = (int64_t)whole * 1000000000 + (int64_t)llround((ws - whole) * 1e9);
        }
        out[i] = yrt_fit_add(f, ticks[i], tn, wn);
    }
}

static void cmd_fit_map(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[], const yrt_fit* f) {
    size_t n, i;
    uint64_t* ticks;
    yrt_fit_info info;
    double* s;
    int64_t* ns = NULL;
    pm_nargs(nrhs, 3, "[t, t_ns] = ysp_rt('fit_map', h, ticks)");
    /* The C map returns 0 here, which a caller would take for a time. */
    yrt_fit_get(f, &info);
    if (!info.ready) pm_err("state", "fit_map: the fit has no map yet (no pair added)");
    ticks = rt_ticks(prhs[2], &n);
    plhs[0] = mxCreateDoubleMatrix(mxGetM(prhs[2]), mxGetN(prhs[2]), mxREAL);
    s = mxGetPr(plhs[0]);
    if (nlhs > 1) {
        plhs[1] = mxCreateNumericMatrix(mxGetM(prhs[2]), mxGetN(prhs[2]), mxINT64_CLASS, mxREAL);
        ns = (int64_t*)mxGetData(plhs[1]);
    }
    for (i = 0; i < n; i++) {
        int64_t v = yrt_fit_map(f, ticks[i]);
        s[i] = rt_s(v);
        if (ns) ns[i] = v;
    }
}

static mxArray* rt_fit_info(const yrt_fit* f) {
    static const char* fl[] = { "t0", "t0_ns", "ticks0", "tick", "ppm", "spread", "width", "span",
                                "points", "pairs", "epoch", "rejected", "pending", "ready", "slope" };
    yrt_fit_info i;
    mxArray* s = mxCreateStructMatrix(1, 1, 15, fl);
    yrt_fit_get(f, &i);
    mxSetField(s, 0, "t0", mxCreateDoubleScalar(rt_s(i.t0_ns)));
    mxSetField(s, 0, "t0_ns", rt_i64(i.t0_ns));
    mxSetField(s, 0, "ticks0", pm_u64_out(i.ticks0));
    mxSetField(s, 0, "tick", mxCreateDoubleScalar(i.ns_per_tick / 1e9));
    mxSetField(s, 0, "ppm", mxCreateDoubleScalar(i.ppm));
    mxSetField(s, 0, "spread", mxCreateDoubleScalar(rt_s(i.spread_ns)));
    mxSetField(s, 0, "width", mxCreateDoubleScalar(rt_s(i.width_ns)));
    mxSetField(s, 0, "span", mxCreateDoubleScalar(rt_s(i.span_ns)));
    mxSetField(s, 0, "points", mxCreateDoubleScalar(i.points));
    mxSetField(s, 0, "pairs", mxCreateDoubleScalar(i.pairs));
    mxSetField(s, 0, "epoch", mxCreateDoubleScalar(i.epoch));
    mxSetField(s, 0, "rejected", mxCreateDoubleScalar(i.rejected));
    mxSetField(s, 0, "pending", mxCreateDoubleScalar(i.pending));
    mxSetField(s, 0, "ready", mxCreateLogicalScalar(i.ready));
    mxSetField(s, 0, "slope", mxCreateLogicalScalar(i.slope));
    return s;
}

/* ---- dispatch ------------------------------------------------------------ */

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    /* A stack buffer, not mxArrayToString: 'now' is called once per frame
     * or per event, and this path allocates nothing. */
    char cmd[24];
    pm_destroy = rt_destroy;
    if (nrhs < 1 || !mxIsChar(prhs[0])) pm_err("usage", "first argument must be a command string");
    if (mxGetString(prhs[0], cmd, sizeof cmd) != 0) pm_err("usage", "unknown command");

    if (strcmp(cmd, "now") == 0) { plhs[0] = mxCreateDoubleScalar(rt_s(yrt_now_ns())); return; }
    if (strcmp(cmd, "now_ns") == 0) { plhs[0] = rt_i64(yrt_now_ns()); return; }
    if (strcmp(cmd, "sleep_until") == 0) {
        int64_t t, spin = YRT_DEFAULT_SPIN_NS;
        pm_nargs(nrhs, 2, "late = ysp_rt('sleep_until', t, [spin])");
        t = rt_ns(prhs[1], "t");
        if (nrhs > 2) spin = rt_ns(prhs[2], "spin");
        plhs[0] = mxCreateDoubleScalar(rt_s(yrt_sleep_until(t, spin)));
        return;
    }
    if (strcmp(cmd, "sleep") == 0) {
        int64_t t0 = yrt_now_ns(), d, spin = YRT_DEFAULT_SPIN_NS;
        pm_nargs(nrhs, 2, "late = ysp_rt('sleep', duration, [spin])");
        d = rt_ns(prhs[1], "duration");
        if (nrhs > 2) spin = rt_ns(prhs[2], "spin");
        if (d > INT64_MAX - t0) pm_err("arg", "duration: too long");
        plhs[0] = mxCreateDoubleScalar(rt_s(yrt_sleep_until(t0 + d, spin)));
        return;
    }
    if (strcmp(cmd, "spin_until") == 0) {
        pm_nargs(nrhs, 2, "late = ysp_rt('spin_until', t)");
        plhs[0] = mxCreateDoubleScalar(rt_s(yrt_spin_until(rt_ns(prhs[1], "t"))));
        return;
    }
    if (strcmp(cmd, "real_now") == 0) { plhs[0] = mxCreateDoubleScalar(rt_s(yrt_real_ns())); return; }
    if (strcmp(cmd, "version") == 0) {
        plhs[0] = mxCreateString(RT_BINDING_VERSION);
        if (nlhs > 1) plhs[1] = mxCreateString(yrt_version());
        return;
    }
    if (strcmp(cmd, "clock_info") == 0) {
        static const char* f[] = { "resolution", "hires_timer" };
        yrt_clock_info ci;
        yrt_get_clock_info(&ci);
        plhs[0] = mxCreateStructMatrix(1, 1, 2, f);
        mxSetField(plhs[0], 0, "resolution", mxCreateDoubleScalar(rt_s(ci.resolution_ns)));
        mxSetField(plhs[0], 0, "hires_timer", mxCreateLogicalScalar(ci.hires_timer));
        return;
    }
    if (strcmp(cmd, "constants") == 0) {
        static const char* f[] = { "default_spin", "virtual_t0", "fit_points", "fit_point", "fit_refit",
                                   "fit_restart", "fit_rejected", "fit_pending" };
        mxArray* s = mxCreateStructMatrix(1, 1, 8, f);
        mxSetField(s, 0, "default_spin", mxCreateDoubleScalar(rt_s(YRT_DEFAULT_SPIN_NS)));
        mxSetField(s, 0, "virtual_t0", mxCreateDoubleScalar(rt_s(YRT_VIRTUAL_T0_NS)));
        mxSetField(s, 0, "fit_points", mxCreateDoubleScalar(YRT_FIT_POINTS));
        mxSetField(s, 0, "fit_point", mxCreateDoubleScalar(YRT_FIT_POINT));
        mxSetField(s, 0, "fit_refit", mxCreateDoubleScalar(YRT_FIT_REFIT));
        mxSetField(s, 0, "fit_restart", mxCreateDoubleScalar(YRT_FIT_RESTART));
        mxSetField(s, 0, "fit_rejected", mxCreateDoubleScalar(YRT_FIT_REJECTED));
        mxSetField(s, 0, "fit_pending", mxCreateDoubleScalar(YRT_FIT_PENDING));
        plhs[0] = s;
        return;
    }
    if (strcmp(cmd, "describe") == 0) {
        yrt_report r;
        char line[256];
        yrt_report_get(&r, YRT_POLICY_NONE);   /* the ladder is not bound */
        yrt_describe(&r, line, sizeof line);
        plhs[0] = mxCreateString(line);
        return;
    }
    if (strcmp(cmd, "virtual_begin") == 0) {
        int64_t t0 = nrhs > 1 ? rt_ns(prhs[1], "t0") : 0;
        if (yrt_virtual_begin(t0) < 0) pm_err("state", "virtual_begin: the clock is already virtual");
        plhs[0] = mxCreateDoubleScalar(rt_s(yrt_now_ns()));
        return;
    }
    if (strcmp(cmd, "virtual_end") == 0) { yrt_virtual_end(); return; }
    if (strcmp(cmd, "is_virtual") == 0) { plhs[0] = mxCreateLogicalScalar(yrt_is_virtual()); return; }
    if (strcmp(cmd, "correlate") == 0) { cmd_correlate(nlhs, plhs, nrhs, prhs); return; }
    if (strcmp(cmd, "user_dir") == 0) {
        static const char* const kinds[] = { "cache", "config" };
        char out[1024];
        char* sub = NULL;
        int k;
        pm_nargs(nrhs, 2, "dir = ysp_rt('user_dir', 'cache' | 'config', [subdir])");
        k = pm_enum(prhs[1], "kind", kinds, 2);
        if (nrhs > 2 && !mxIsEmpty(prhs[2])) sub = pm_string(prhs[2], "subdir");
        /* '' and not an error: the header's rule is "use no folder". */
        plhs[0] = mxCreateString(yrt_user_dir(k == 0 ? YRT_DIR_CACHE : YRT_DIR_CONFIG, sub, out, sizeof out) == YRT_OK
                                     ? out : "");
        return;
    }
    if (strcmp(cmd, "fit_open") == 0) { cmd_fit_open(plhs, nrhs, prhs); return; }

    if (strncmp(cmd, "fit_", 4) == 0) {
        yrt_fit* f;
        pm_nargs(nrhs, 2, "ysp_rt('fit_...', h, ...)");
        f = (yrt_fit*)pm_lookup(prhs[1]);
        if (strcmp(cmd, "fit_add") == 0) { cmd_fit_add(plhs, nrhs, prhs, f); return; }
        if (strcmp(cmd, "fit_map") == 0) { cmd_fit_map(nlhs, plhs, nrhs, prhs, f); return; }
        if (strcmp(cmd, "fit_unwrap") == 0) {
            size_t n, i;
            uint64_t* t;
            uint64_t* o;
            pm_nargs(nrhs, 3, "u = ysp_rt('fit_unwrap', h, ticks)");
            t = rt_ticks(prhs[2], &n);
            plhs[0] = mxCreateNumericMatrix(mxGetM(prhs[2]), mxGetN(prhs[2]), mxUINT64_CLASS, mxREAL);
            o = (uint64_t*)mxGetData(plhs[0]);
            for (i = 0; i < n; i++) o[i] = yrt_fit_unwrap(f, t[i]);
            return;
        }
        if (strcmp(cmd, "fit_info") == 0) { plhs[0] = rt_fit_info(f); return; }
        if (strcmp(cmd, "fit_close") == 0) {
            pm_unregister(f);
            free(f);
            return;
        }
    }
    pm_err("usage", "unknown command '%s'", cmd);
}
