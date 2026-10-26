/* ysp_rdk.c - MATLAB/Octave MEX binding for ysp/rdk.h (builds to ysp_rdk.<mexext>)
 *
 * The analysis side of ysp/rdk.h, as the Python ysp.rdk: open a field with
 * the rig's desc, then replay a logged trial and get back the dots a
 * participant saw, bit for bit. Command dispatch in the style of the other
 * ysp MEX files; 'open' returns an opaque uint64 handle to a field.
 *
 *     h = ysp_rdk('open', struct('algorithm', 'LL', 'w', 10, 'count', 100, ...
 *                                'coherence', 0.5, 'speed', 5));
 *     [xy, dir, sig] = ysp_rdk('trajectory', h, seed, t0, steps);
 *     %  xy: n x 2 x K single, dir: n x K single, sig: n x K uint8
 *     ysp_rdk('start', h, seed, t0);  n = ysp_rdk('update', h, t);
 *     ysp_rdk('set', h, 'coherence', 0.25);   xy = ysp_rdk('xy', h);
 *     ysp_rdk('close', h);
 *
 * TIMES: seconds (double), as every binding (docs/bindings.md). The header
 * counts int64 ns, and a replay is exact only if the ns are, so each time
 * converts once, to the nearest ns of the exact product (rdk_s_to_ns, the
 * same as ysp.rdk's); a time at or above 2^23 s is refused, not rounded.
 * 'get' gives the last update's t in seconds with its exact ns beside it,
 * and a steps row with ns replays from the ns.
 *
 * Build with build.m. See README.md for the command reference.
 */
#define PM_MOD "ysp_rdk"
#include "ysp_mex_util.h"

/* The MEX function's own version (docs/bindings.md, Versions); the
 * header's is the second output of 'version'. */
#define MEX_BINDING_VERSION "1.0.0"

#define YSP_RDK_IMPLEMENTATION
#include "ysp/rdk.h"

/* ysp.rdk's bound (docs/rdk.md "Bindings"): below 2^23 s, the double
 * nearest ns / 1e9 converts back to ns. */
#define RDK_S_MAX 8388608.0

static void rdk_destroy(void* obj) {
    yrdk_field* f = (yrdk_field*)obj;
    yrdk_close(f);
    free(f);
}

static void rdk_check(int rc, yrdk_field* f) {
    if (rc >= 0) return;
    if (rc == YRDK_ERR_ARG) pm_err("arg", "%s", yrdk_error(f));
    if (rc == YRDK_ERR_ORDER) pm_err("order", "%s", yrdk_error(f));
    if (rc == YRDK_ERR_MEM) pm_err("memory", "%s", yrdk_error(f));
    pm_err("error", "%s", yrdk_error(f));
}

/* Seconds to the nearest ns: s * 1e9 rounds once, fma() gives that
 * rounding's error exactly, so n / 1e9 -> s -> n holds below RDK_S_MAX. */
static int64_t rdk_s_to_ns_d(double s, const char* what) {
    volatile double p;   /* stored as a double: x87 would keep 80 bits */
    double e, n, r;
    if (!(s >= 0.0 && s < RDK_S_MAX)) pm_err("arg", "%s: seconds, >= 0 and < 8388608 (2^23 s, about 97 days)", what);
    p = s * 1e9;
    e = fma(s, 1e9, -p);
    n = floor(p + 0.5);
    r = (p - n) + e;
    if (r > 0.5) n += 1.0;
    else if (r < -0.5) n -= 1.0;
    return (int64_t)n;
}

static int64_t rdk_s_to_ns(const mxArray* a, const char* what) { return rdk_s_to_ns_d(pm_scalar(a, what), what); }

static yrdk_field* rdk_get(const mxArray* a) { return (yrdk_field*)pm_lookup(a); }

static mxArray* rdk_int64(int64_t v) {
    mxArray* a = mxCreateNumericMatrix(1, 1, mxINT64_CLASS, mxREAL);
    *(int64_t*)mxGetData(a) = v;
    return a;
}

/* ---- open --------------------------------------------------------------------- */

static const char* const rdk_alg[] = { "custom", "WN", "MN", "LL", "BM" };
static const char* const rdk_sig[] = { "same", "different", "least_recent" };
static const char* const rdk_sel[] = { "exact", "bernoulli" };
static const char* const rdk_noise[] = { "direction", "position", "walk" };
static const char* const rdk_shape[] = { "circle", "rect" };
static const char* const rdk_edge[] = { "wrap", "replot" };
static const char* const rdk_clock[] = { "onset", "frames" };

static const char* const rdk_open_fields[] = { "algorithm", "aperture", "w", "h", "count", "density", "coherence",
                                               "direction", "speed", "signal", "select", "noise", "edge", "sets",
                                               "lifetime", "lifetime_frames", "clock", "frame_period", "seed",
                                               "stream" };

/* pm_enum compares in lower case; the algorithm names are upper case. */
static int rdk_enum(const mxArray* s, const char* field, const char* const* names, int n) {
    const mxArray* a = pm_field(s, field);
    char low[8][16];
    const char* lp[8];
    int i, j;
    if (!a) return 0;
    for (i = 0; i < n; i++) {
        for (j = 0; names[i][j] && j < 15; j++)
            low[i][j] = (char)(names[i][j] >= 'A' && names[i][j] <= 'Z' ? names[i][j] + 32 : names[i][j]);
        low[i][j] = 0;
        lp[i] = low[i];
    }
    return pm_enum(a, field, lp, n);
}

static float rdk_float(const mxArray* s, const char* field) {
    const mxArray* a = pm_field(s, field);
    return a ? (float)pm_finite(a, field) : 0.0f;
}

static int rdk_intf(const mxArray* s, const char* field) {
    const mxArray* a = pm_field(s, field);
    return a ? pm_int(a, field) : 0;
}

static void rdk_open(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    yrdk_desc d;
    yrdk_field* f;
    const mxArray *s, *a;
    (void)nlhs;
    pm_nargs(nrhs, 2, "h = ysp_rdk('open', desc)");
    s = prhs[1];
    pm_check_fields(s, rdk_open_fields, (int)(sizeof rdk_open_fields / sizeof rdk_open_fields[0]), "desc");
    memset(&d, 0, sizeof d);
    d.algorithm = (yrdk_algorithm)rdk_enum(s, "algorithm", rdk_alg, 5);
    d.aperture = (yrdk_shape)rdk_enum(s, "aperture", rdk_shape, 2);
    d.signal = (yrdk_signal)rdk_enum(s, "signal", rdk_sig, 3);
    d.select = (yrdk_select)rdk_enum(s, "select", rdk_sel, 2);
    d.noise = (yrdk_noise)rdk_enum(s, "noise", rdk_noise, 3);
    d.edge = (yrdk_edge)rdk_enum(s, "edge", rdk_edge, 2);
    d.clock = (yrdk_clock)rdk_enum(s, "clock", rdk_clock, 2);
    /* The desc's floats are float32: a double rounds to the nearest, as a C
     * float literal does, so the same numbers give the same field. */
    d.w = rdk_float(s, "w");
    d.h = rdk_float(s, "h");
    d.count = rdk_intf(s, "count");
    d.density = rdk_float(s, "density");
    d.coherence = rdk_float(s, "coherence");
    d.direction = rdk_float(s, "direction");
    d.speed = rdk_float(s, "speed");
    d.sets = rdk_intf(s, "sets");
    d.lifetime_frames = rdk_intf(s, "lifetime_frames");
    /* lifetime as ysp.rdk takes it: rounded to float32 first, then to ns,
     * so a desc gives the same field (and digest) in both bindings */
    {
        float life = rdk_float(s, "lifetime");
        if (!(life >= 0.0f && life <= 1e9f)) pm_err("arg", "lifetime: seconds, > 0");
        d.lifetime_ns = (int64_t)llround((double)life * 1e9);
        if (life > 0.0f && d.lifetime_ns < 1) pm_err("arg", "lifetime: shorter than 1 ns");
    }
    if ((a = pm_field(s, "frame_period")) != NULL) d.frame_ns = rdk_s_to_ns(a, "frame_period");
    if ((a = pm_field(s, "seed")) != NULL) d.seed = pm_u64(a, "seed");
    if ((a = pm_field(s, "stream")) != NULL) {
        double v = pm_scalar(a, "stream");
        if (!(v >= 0 && v <= 4294967295.0 && v == floor(v))) pm_err("arg", "stream: an integer, 0 to 2^32 - 1");
        d.stream = (uint32_t)v;
    }
    f = (yrdk_field*)calloc(1, sizeof *f);
    if (!f) pm_err("memory", "out of memory");
    if (!yrdk_open(f, &d)) {
        char msg[200];
        int mem = strstr(yrdk_error(f), "allocation") != NULL;
        snprintf(msg, sizeof msg, "%s", yrdk_error(f));
        yrdk_close(f);
        free(f);
        pm_err(mem ? "memory" : "arg", "%s", msg);
    }
    plhs[0] = pm_handle_out(pm_register(f));
}

/* ---- steps -------------------------------------------------------------------- */

/* steps: a K x 4 double matrix [t coherence direction speed] (t in
 * seconds), or a struct array with fields t, coherence, direction, speed
 * and optional ns, such as the 'get' log's last. A row with ns (not empty)
 * uses it, so a log replays exactly at any magnitude; its t must then be
 * the double nearest ns / 1e9, or the row is refused. mxMalloc memory. */
static yrdk_step* rdk_steps(const mxArray* a, int* n_out) {
    yrdk_step* s;
    size_t n, i;
    if (mxIsStruct(a)) {
        static const char* need[] = { "t", "coherence", "direction", "speed" };
        int k;
        n = mxGetNumberOfElements(a);
        for (k = 0; k < 4; k++)
            if (mxGetFieldNumber(a, need[k]) < 0) pm_err("arg", "steps: a struct array needs the field '%s'", need[k]);
        s = (yrdk_step*)mxCalloc(n ? n : 1, sizeof *s);
        for (i = 0; i < n; i++) {
            const mxArray* f[4];
            const mxArray* ns = mxGetFieldNumber(a, "ns") >= 0 ? mxGetField(a, i, "ns") : NULL;
            char what[48];
            for (k = 0; k < 4; k++) {
                f[k] = mxGetField(a, i, need[k]);
                if (!f[k]) pm_err("arg", "steps(%d).%s is empty", (int)i + 1, need[k]);
            }
            snprintf(what, sizeof what, "steps(%d).t", (int)i + 1);
            if (ns && !mxIsEmpty(ns)) {
                double ts = pm_scalar(f[0], what);
                int64_t v;
                if (mxIsInt64(ns) && mxGetNumberOfElements(ns) == 1) v = *(const int64_t*)mxGetData(ns);
                else v = (int64_t)pm_u64(ns, "ns");
                if (v < 0 || ts != (double)v / 1e9) pm_err("arg", "steps(%d): t is not ns / 1e9 (or ns < 0)", (int)i + 1);
                s[i].t = v;
            } else {
                s[i].t = rdk_s_to_ns(f[0], what);
            }
            s[i].coherence = (float)pm_finite(f[1], "coherence");
            s[i].direction = (float)pm_finite(f[2], "direction");
            s[i].speed = (float)pm_finite(f[3], "speed");
        }
    } else {
        const double* p;
        if (!mxIsDouble(a) || mxIsComplex(a) || (mxGetN(a) != 4 && !mxIsEmpty(a)))
            pm_err("arg", "steps: a K x 4 double matrix [t coherence direction speed] or a struct array");
        n = mxGetM(a);
        if (mxIsEmpty(a)) n = 0;
        p = mxGetPr(a);
        s = (yrdk_step*)mxCalloc(n ? n : 1, sizeof *s);
        for (i = 0; i < n; i++) {
            char what[32];
            snprintf(what, sizeof what, "steps(%d, 1)", (int)i + 1);
            s[i].t = rdk_s_to_ns_d(p[i], what);
            if (!isfinite(p[i + n]) || !isfinite(p[i + 2 * n]) || !isfinite(p[i + 3 * n]))
                pm_err("arg", "steps(%d, 2:4) must be finite", (int)i + 1);
            s[i].coherence = (float)p[i + n];
            s[i].direction = (float)p[i + 2 * n];
            s[i].speed = (float)p[i + 3 * n];
        }
    }
    if (n > 2147483647u) pm_err("arg", "steps: too many rows");
    *n_out = (int)n;
    return s;
}

/* ---- outputs ------------------------------------------------------------------ */

static mxArray* rdk_last(const yrdk_field* f) {
    static const char* fn[] = { "t", "coherence", "direction", "speed", "set", "ns" };
    mxArray* s = mxCreateStructMatrix(1, 1, 6, fn);
    const yrdk_step* l = &f->last;
    mxSetField(s, 0, "t", mxCreateDoubleScalar((double)l->t / 1e9));
    mxSetField(s, 0, "coherence", mxCreateDoubleScalar(l->coherence));
    mxSetField(s, 0, "direction", mxCreateDoubleScalar(l->direction));
    mxSetField(s, 0, "speed", mxCreateDoubleScalar(l->speed));
    mxSetField(s, 0, "set", mxCreateDoubleScalar(l->set));
    mxSetField(s, 0, "ns", rdk_int64(l->t));
    return s;
}

static mxArray* rdk_stats(const yrdk_field* f) {
    static const char* fn[] = { "updates", "signal", "wraps", "replots", "deaths", "placed", "exhausted", "stuck" };
    const yrdk_stats* st = &f->stats;
    mxArray* s = mxCreateStructMatrix(1, 1, 8, fn);
    mxSetField(s, 0, "updates", mxCreateDoubleScalar((double)st->updates));
    mxSetField(s, 0, "signal", mxCreateDoubleScalar((double)st->signal));
    mxSetField(s, 0, "wraps", mxCreateDoubleScalar((double)st->wraps));
    mxSetField(s, 0, "replots", mxCreateDoubleScalar((double)st->replots));
    mxSetField(s, 0, "deaths", mxCreateDoubleScalar((double)st->deaths));
    mxSetField(s, 0, "placed", mxCreateDoubleScalar((double)st->placed));
    mxSetField(s, 0, "exhausted", mxCreateDoubleScalar((double)st->exhausted));
    mxSetField(s, 0, "stuck", mxCreateDoubleScalar((double)st->stuck));
    return s;
}

/* x, y pairs (C order) into the first two columns of n x 2 page k. */
static void rdk_scatter_xy(float* dst, const float* pairs, size_t n, size_t k) {
    size_t i;
    float* page = dst + k * 2 * n;
    for (i = 0; i < n; i++) { page[i] = pairs[2 * i]; page[n + i] = pairs[2 * i + 1]; }
}

static void rdk_output(yrdk_field* f, int which, mxArray* plhs[]) {
    size_t n = (size_t)f->n;
    yrdk_out o;
    if (!f->started_) pm_err("order", "no output before 'start'");
    memset(&o, 0, sizeof o);
    if (which == 0) {
        float* pairs = (float*)mxMalloc((n ? n : 1) * 2 * sizeof(float));
        plhs[0] = mxCreateNumericMatrix(n, 2, mxSINGLE_CLASS, mxREAL);
        o.xy = pairs;
        rdk_check(yrdk_write(f, &o), f);
        rdk_scatter_xy((float*)mxGetData(plhs[0]), pairs, n, 0);
        return;
    }
    plhs[0] = mxCreateNumericMatrix(n, 1, which >= 3 ? mxUINT8_CLASS : mxSINGLE_CLASS, mxREAL);
    if (which == 1) o.dir = (float*)mxGetData(plhs[0]);
    else if (which == 2) o.age_s = (float*)mxGetData(plhs[0]);
    else if (which == 3) o.signal = (uint8_t*)mxGetData(plhs[0]);
    else o.event = (uint8_t*)mxGetData(plhs[0]);
    rdk_check(yrdk_write(f, &o), f);
}

/* start, then each step; the shown set after each update into page k. */
static void rdk_trajectory(yrdk_field* f, uint64_t seed, int64_t t0, const yrdk_step* s, int k, int nlhs,
                           mxArray* plhs[]) {
    size_t n = (size_t)f->n, j;
    mwSize dims[3];
    float *xy, *dir, *pairs;
    uint8_t* sig;
    int rc;
    dims[0] = n; dims[1] = 2; dims[2] = (mwSize)k;
    plhs[0] = mxCreateNumericArray(3, dims, mxSINGLE_CLASS, mxREAL);
    if (nlhs > 1) plhs[1] = mxCreateNumericMatrix(n, (size_t)k, mxSINGLE_CLASS, mxREAL);
    if (nlhs > 2) plhs[2] = mxCreateNumericMatrix(n, (size_t)k, mxUINT8_CLASS, mxREAL);
    xy = (float*)mxGetData(plhs[0]);
    dir = nlhs > 1 ? (float*)mxGetData(plhs[1]) : NULL;
    sig = nlhs > 2 ? (uint8_t*)mxGetData(plhs[2]) : NULL;
    pairs = (float*)mxMalloc((n ? n : 1) * 2 * sizeof(float));
    rdk_check(yrdk_start(f, seed, t0), f);
    for (j = 0; j < (size_t)k; j++) {
        yrdk_out o;
        f->coherence = s[j].coherence;
        f->direction = s[j].direction;
        f->speed = s[j].speed;
        rc = yrdk_update(f, s[j].t);
        rdk_check(rc, f);
        memset(&o, 0, sizeof o);
        o.xy = pairs;
        if (dir) o.dir = dir + j * n;
        if (sig) o.signal = sig + j * n;
        rdk_check(yrdk_write(f, &o), f);
        rdk_scatter_xy(xy, pairs, n, j);
    }
}

static mxArray* rdk_constants(void) {
    static const struct { const char* name; double v; } c[] = {
        { "CUSTOM", YRDK_CUSTOM }, { "WN", YRDK_WN }, { "MN", YRDK_MN }, { "LL", YRDK_LL }, { "BM", YRDK_BM },
        { "SIGNAL_SAME", YRDK_SIGNAL_SAME }, { "SIGNAL_DIFFERENT", YRDK_SIGNAL_DIFFERENT },
        { "SIGNAL_LEAST_RECENT", YRDK_SIGNAL_LEAST_RECENT }, { "EXACT", YRDK_EXACT }, { "BERNOULLI", YRDK_BERNOULLI },
        { "NOISE_DIRECTION", YRDK_NOISE_DIRECTION }, { "NOISE_POSITION", YRDK_NOISE_POSITION },
        { "NOISE_WALK", YRDK_NOISE_WALK }, { "CIRCLE", YRDK_CIRCLE }, { "RECT", YRDK_RECT },
        { "WRAP", YRDK_WRAP }, { "REPLOT", YRDK_REPLOT }, { "CLOCK_ONSET", YRDK_CLOCK_ONSET },
        { "CLOCK_FRAMES", YRDK_CLOCK_FRAMES }, { "EV_SIGNAL", YRDK_EV_SIGNAL }, { "EV_WRAPPED", YRDK_EV_WRAPPED },
        { "EV_PLACED", YRDK_EV_PLACED },
    };
    const int n = (int)(sizeof c / sizeof c[0]);
    const char* names[32];
    mxArray* s;
    int i;
    for (i = 0; i < n; i++) names[i] = c[i].name;
    s = mxCreateStructMatrix(1, 1, n, names);
    for (i = 0; i < n; i++) mxSetFieldByNumber(s, 0, i, mxCreateDoubleScalar(c[i].v));
    return s;
}

/* ---- dispatch ----------------------------------------------------------------- */

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char cmd[32];
    pm_destroy = rdk_destroy;
    if (nrhs < 1 || !mxIsChar(prhs[0])) pm_err("usage", "usage: ysp_rdk(command, ...); see README.md");
    if (mxGetString(prhs[0], cmd, sizeof cmd)) pm_err("usage", "unknown command");

    if (strcmp(cmd, "version") == 0) { plhs[0] = mxCreateString(MEX_BINDING_VERSION); if (nlhs > 1) plhs[1] = mxCreateString(yrdk_version()); return; }
    if (strcmp(cmd, "constants") == 0) { plhs[0] = rdk_constants(); return; }
    if (strcmp(cmd, "open") == 0) { rdk_open(nlhs, plhs, nrhs, prhs); return; }
    if (strcmp(cmd, "philox") == 0) {
        double c[4];
        uint32_t ctr[4], out[4];
        int i;
        pm_nargs(nrhs, 3, "w = ysp_rdk('philox', seed, [c0 c1 c2 c3])");
        pm_vector_n(prhs[2], "counter", c, 4);
        for (i = 0; i < 4; i++) {
            if (!(c[i] >= 0 && c[i] <= 4294967295.0 && c[i] == floor(c[i]))) pm_err("arg", "counter: integers, 0 to 2^32 - 1");
            ctr[i] = (uint32_t)c[i];
        }
        yrdk_philox(pm_u64(prhs[1], "seed"), ctr, out);
        plhs[0] = mxCreateNumericMatrix(1, 4, mxUINT32_CLASS, mxREAL);
        memcpy(mxGetData(plhs[0]), out, sizeof out);
        return;
    }
    if (strcmp(cmd, "angle") == 0) {
        pm_nargs(nrhs, 2, "a = ysp_rdk('angle', deg)");
        plhs[0] = mxCreateNumericMatrix(1, 1, mxUINT32_CLASS, mxREAL);
        *(uint32_t*)mxGetData(plhs[0]) = yrdk_angle((float)pm_finite(prhs[1], "deg"));
        return;
    }
    if (strcmp(cmd, "sincos") == 0) {
        int32_t sc[2];
        double a;
        pm_nargs(nrhs, 2, "sc = ysp_rdk('sincos', angle)");
        a = pm_scalar(prhs[1], "angle");
        if (!(a >= 0 && a <= 4294967295.0 && a == floor(a))) pm_err("arg", "angle: an integer, 0 to 2^32 - 1");
        yrdk_sincos((uint32_t)a, &sc[0], &sc[1]);
        plhs[0] = mxCreateNumericMatrix(1, 2, mxINT32_CLASS, mxREAL);
        memcpy(mxGetData(plhs[0]), sc, sizeof sc);
        return;
    }

    /* every command below takes a handle */
    {
        yrdk_field* f;
        if (nrhs < 2) pm_err("usage", "usage: ysp_rdk('%s', h, ...)", cmd);
        f = rdk_get(prhs[1]);
        if (strcmp(cmd, "close") == 0) {
            pm_unregister(f);
            rdk_destroy(f);
            return;
        }
        if (strcmp(cmd, "start") == 0) {
            pm_nargs(nrhs, 4, "ysp_rdk('start', h, seed, t)");
            rdk_check(yrdk_start(f, pm_u64(prhs[2], "seed"), rdk_s_to_ns(prhs[3], "t")), f);
            return;
        }
        if (strcmp(cmd, "update") == 0) {
            int rc;
            pm_nargs(nrhs, 3, "n = ysp_rdk('update', h, t)");
            rc = yrdk_update(f, rdk_s_to_ns(prhs[2], "t"));
            rdk_check(rc, f);
            plhs[0] = mxCreateDoubleScalar(rc);
            return;
        }
        if (strcmp(cmd, "set") == 0) {
            int i;
            if (nrhs < 4 || (nrhs - 2) % 2) pm_err("usage", "usage: ysp_rdk('set', h, name, value, ...)");
            for (i = 2; i < nrhs; i += 2) {
                char* name = pm_string(prhs[i], "name");
                float v = (float)pm_finite(prhs[i + 1], name);
                if (strcmp(name, "coherence") == 0) f->coherence = v;
                else if (strcmp(name, "direction") == 0) f->direction = v;
                else if (strcmp(name, "speed") == 0) f->speed = v;
                else pm_err("arg", "set: 'coherence', 'direction' or 'speed', not '%s'", name);
            }
            return;
        }
        if (strcmp(cmd, "get") == 0) {
            static const char* fn[] = { "coherence", "direction", "speed", "n", "total", "last", "stats" };
            mxArray* s = mxCreateStructMatrix(1, 1, 7, fn);
            mxSetField(s, 0, "coherence", mxCreateDoubleScalar(f->coherence));
            mxSetField(s, 0, "direction", mxCreateDoubleScalar(f->direction));
            mxSetField(s, 0, "speed", mxCreateDoubleScalar(f->speed));
            mxSetField(s, 0, "n", mxCreateDoubleScalar(f->n));
            mxSetField(s, 0, "total", mxCreateDoubleScalar(f->total));
            mxSetField(s, 0, "last", rdk_last(f));
            mxSetField(s, 0, "stats", rdk_stats(f));
            plhs[0] = s;
            return;
        }
        if (strcmp(cmd, "replay") == 0 || strcmp(cmd, "trajectory") == 0) {
            yrdk_step* s;
            int k;
            uint64_t seed;
            int64_t t0;
            pm_nargs(nrhs, 5, "ysp_rdk('replay' or 'trajectory', h, seed, t0, steps)");
            seed = pm_u64(prhs[2], "seed");
            t0 = rdk_s_to_ns(prhs[3], "t0");
            s = rdk_steps(prhs[4], &k);
            if (cmd[0] == 'r') rdk_check(yrdk_replay(f, seed, t0, s, k), f);
            else rdk_trajectory(f, seed, t0, s, k, nlhs, plhs);
            return;
        }
        if (strcmp(cmd, "xy") == 0) { rdk_output(f, 0, plhs); return; }
        if (strcmp(cmd, "directions") == 0) { rdk_output(f, 1, plhs); return; }
        if (strcmp(cmd, "ages") == 0) { rdk_output(f, 2, plhs); return; }
        if (strcmp(cmd, "signals") == 0) { rdk_output(f, 3, plhs); return; }
        if (strcmp(cmd, "events") == 0) { rdk_output(f, 4, plhs); return; }
        if (strcmp(cmd, "digest") == 0) { plhs[0] = pm_u64_out(yrdk_digest(f)); return; }
        if (strcmp(cmd, "snapshot") == 0) {
            size_t n = yrdk_snapshot_bytes(f);
            plhs[0] = mxCreateNumericMatrix(1, n, mxUINT8_CLASS, mxREAL);
            rdk_check(yrdk_snapshot(f, mxGetData(plhs[0]), n), f);
            return;
        }
        if (strcmp(cmd, "restore") == 0) {
            size_t n;
            const uint8_t* b;
            pm_nargs(nrhs, 3, "ysp_rdk('restore', h, bytes)");
            b = pm_bytes_in(prhs[2], "bytes", &n);
            rdk_check(yrdk_restore(f, b, n), f);
            return;
        }
    }
    pm_err("usage", "unknown command '%s'", cmd);
}
