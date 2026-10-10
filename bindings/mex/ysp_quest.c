/* ysp_quest.c - MATLAB/Octave MEX binding for ysp/quest.h (builds to ysp_quest.<mexext>)
 *
 * QUEST+ on a grid, with QUEST, Psi and Psi-marginal as configurations, and
 * the header's YQST_ASYNC layer: a C thread that runs the inference between
 * trials. Command dispatch in the style of widmann's ppdev-mex.
 *
 *     d = struct('stim', {{{-3, 0, 31}}}, ...          % one axis, a linspace
 *                'param', {{{-3, 0, 61}, {0.5, 6, 12}, 0.5, 0.02}}, ...
 *                'stop_trials', 60);
 *     h = ysp_quest('open', d);
 *     while ~ysp_quest('done', h)
 *         [i, x] = ysp_quest('next', h);               % 1-based grid index
 *         ysp_quest('update', h, i, run_trial(10^x));  % outcome 1 = correct
 *     end
 *     est = ysp_quest('estimate', h, 'mean');
 *     ysp_quest('close', h);
 *
 * Build with build.m. See README.md for the command reference.
 *
 * A MATLAB pf_batch or rng runs on the interpreter thread, inside the header
 * call that needs it, through mexCallMATLABWithTrap; an error in it is raised
 * after the header call has returned. The async thread is C and never enters
 * the interpreter, so async_start refuses a session that would need MATLAB on
 * that thread.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define YQST_ASYNC
#define YSP_QUEST_IMPLEMENTATION
#include "ysp/quest.h"

#define PM_MOD "ysp_quest"
#include "ysp_mex_util.h"

typedef struct qn {
    yqst_quest q;
    yqst_async a;
    int        async_running;
    int        busy;          /* inside a header call that may run MATLAB   */
    pm_rng     rng;
    int        has_rng;
    mxArray*   pf;            /* persistent pf_batch handle, or NULL        */
    mxArray*   pmat;          /* persistent P x n_param matrix for pf_batch */
    int        n_stim, n_param, K;
    char       cb_msg[256];   /* a malformed callback result, raised after  */
} qn;

static void qn_destroy(void* obj) {
    qn* n = (qn*)obj;
    if (n->async_running) { yqst_async_close(&n->a); n->async_running = 0; }
    yqst_close(&n->q);
    if (n->pf) mxDestroyArray(n->pf);
    if (n->pmat) mxDestroyArray(n->pmat);
    pm_rng_free(&n->rng);
    free(n);
}

static const char* const q_pf_names[]   = { "gumbel", "weibull", "logistic", "normal", "hypsec", "custom" };
static const char* const q_sel_names[]  = { "entropy", "quantile", "mean", "mode" };
static const char* const q_est_names[]  = { "mean", "mode", "median" };
static const char* const q_tie_names[]  = { "lowest", "nearest", "alternate", "random" };
static const char* const q_stop_names[] = { "none", "trials", "entropy", "sd", "full" };

static void q_check(int rc) {
    if (rc >= 0) return;
    switch (rc) {
        case YQST_ERR_ARG:     pm_err("arg", "%s", yqst_strerror(rc)); break;
        case YQST_ERR_CLOSED:  pm_err("closed", "%s", yqst_strerror(rc)); break;
        case YQST_ERR_FULL:    pm_err("full", "%s", yqst_strerror(rc)); break;
        case YQST_ERR_MEMORY:  pm_err("memory", "%s", yqst_strerror(rc)); break;
        case YQST_ERR_BUSY:    pm_err("busy", "%s", yqst_strerror(rc)); break;
        case YQST_ERR_TIMEOUT: pm_err("timeout", "%s", yqst_strerror(rc)); break;
        default:               pm_err("error", "%s", yqst_strerror(rc)); break;
    }
}

/* Raise whatever a callback left behind: the MATLAB exception first, then a
 * shape or type problem the binding found in its result. */
static void qn_after_callbacks(qn* n) {
    pm_rethrow();
    if (n->cb_msg[0]) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s", n->cb_msg);
        n->cb_msg[0] = 0;
        pm_err("callback", "%s", msg);
    }
}

/* ---- the pf_batch trampoline ------------------------------------------ */

static void qn_cb_fail(qn* n, const char* msg) {
    if (!n->cb_msg[0]) snprintf(n->cb_msg, sizeof(n->cb_msg), "pf_batch: %s", msg);
}

/* The header hands a P x n_param matrix in row-major order; MATLAB wants it
 * column-major. The grid's matrix never changes, so it is converted once and
 * kept; a single-row call (yqst_p, an off-grid cell) gets a fresh one. */
static mxArray* qn_param_matrix(qn* n, const double* params, int P) {
    mxArray* m;
    double* d;
    int i, j, np = n->n_param;
    if (params == n->q.param_matrix && P == n->q.P && n->pmat) return n->pmat;
    m = mxCreateDoubleMatrix((size_t)P, (size_t)np, mxREAL);
    d = mxGetPr(m);
    for (i = 0; i < P; i++)
        for (j = 0; j < np; j++) d[(size_t)j * P + i] = params[(size_t)i * np + j];
    if (params == n->q.param_matrix && P == n->q.P) {
        mexMakeArrayPersistent(m);
        n->pmat = m;
    }
    return m;
}

static void qn_pf_batch(void* ctx, const double* stims, int S, const double* params, int P, float* out) {
    qn* n = (qn*)ctx;
    int K = n->K, s, i, k;
    for (s = 0; s < S; s++) {
        float* o = out + (size_t)s * P * K;
        mxArray* in[3];
        mxArray* res = NULL;
        int ok = 0;
        if (!pm_pending && !n->cb_msg[0]) {
            in[0] = n->pf;
            in[1] = pm_row(stims + (size_t)s * n->n_stim, n->n_stim);
            in[2] = qn_param_matrix(n, params, P);
            if (pm_feval(1, &res, 3, in)) {
                if (!res || (!mxIsDouble(res) && !mxIsSingle(res)) || mxIsComplex(res))
                    qn_cb_fail(n, "must return a real double or single matrix");
                else if (mxGetM(res) != (size_t)P || mxGetN(res) != (size_t)K)
                    qn_cb_fail(n, "must return a P x K matrix (one row per parameter row)");
                else {
                    if (mxIsDouble(res)) {
                        const double* r = mxGetPr(res);
                        for (i = 0; i < P; i++)
                            for (k = 0; k < K; k++) o[(size_t)i * K + k] = (float)r[(size_t)k * P + i];
                    } else {
                        const float* r = (const float*)mxGetData(res);
                        for (i = 0; i < P; i++)
                            for (k = 0; k < K; k++) o[(size_t)i * K + k] = r[(size_t)k * P + i];
                    }
                    ok = 1;
                }
            }
            if (res) mxDestroyArray(res);
            mxDestroyArray(in[1]);
            if (in[2] != n->pmat) mxDestroyArray(in[2]);
        }
        if (!ok) {
            /* A neutral model, so the header finishes its loop on valid
             * numbers; the error is raised when it returns. */
            for (i = 0; i < P * K; i++) o[i] = 1.0f / (float)K;
        }
    }
}

/* ---- desc --------------------------------------------------------------- */

static const char* const q_fields[] = {
    "stim", "param", "prior", "nuisance", "joint_prior", "pf", "pf_batch",
    "n_outcomes", "select", "select_param", "select_quantile", "tiebreak",
    "tie_tolerance", "rng", "subset_size", "stop_trials", "stop_entropy",
    "stop_sd", "stop_sd_param", "no_table"
};

/* An axis: a numeric vector of explicit values (a scalar is a fixed
 * parameter), or a cell {lo, hi, n} for a linspace. The values are copied
 * into mxMalloc scratch; yqst_open() copies them again into its arena, so the
 * scratch only has to live until open returns. */
static void q_axis(const mxArray* a, const char* what, yqst_axis* ax) {
    memset(ax, 0, sizeof(*ax));
    if (mxIsCell(a)) {
        if (mxGetNumberOfElements(a) != 3)
            pm_err("arg", "%s: a cell axis is {lo, hi, n}", what);
        ax->lo = pm_finite(mxGetCell(a, 0), "axis lo");
        ax->hi = pm_finite(mxGetCell(a, 1), "axis hi");
        ax->n = pm_int(mxGetCell(a, 2), "axis n");
        return;
    }
    if (!mxIsNumeric(a) || mxIsEmpty(a))
        pm_err("arg", "%s: an axis is a numeric vector or a cell {lo, hi, n}", what);
    {
        int n = (int)mxGetNumberOfElements(a);
        double* v = (double*)mxMalloc((size_t)n * sizeof(double));
        pm_vector(a, what, v, n);
        ax->values = v;
        ax->n = n;
    }
}

/* Axes as a cell array, one entry per axis. A bare numeric vector is one axis
 * of explicit values. A cell is always a list of axes, so a single linspace
 * axis is written {{lo, hi, n}}. */
static int q_axes(const mxArray* a, const char* what, yqst_axis* axes, int cap) {
    int i, n;
    if (!mxIsCell(a)) {
        q_axis(a, what, &axes[0]);
        return 1;
    }
    n = (int)mxGetNumberOfElements(a);
    if (n < 1 || n > cap) pm_err("arg", "%s: 1 to %d axes", what, cap);
    for (i = 0; i < n; i++) {
        char w[48];
        snprintf(w, sizeof(w), "%s{%d}", what, i + 1);
        q_axis(mxGetCell(a, (size_t)i), w, &axes[i]);
    }
    return n;
}

typedef struct q_shape { int n_stim, n_param, K; } q_shape;

/* Reads everything but pf_batch and rng into d; those two need the node. */
static void q_read_desc(const mxArray* s, yqst_desc* d, q_shape* n) {
    const mxArray* f;
    int i;
    pm_check_fields(s, q_fields, (int)(sizeof(q_fields) / sizeof(q_fields[0])), "desc");
    if (!(f = pm_field(s, "stim")))  pm_err("arg", "desc.stim is required");
    d->n_stim = q_axes(f, "stim", d->stim, YQST_MAX_STIM_DIMS);
    if (!(f = pm_field(s, "param"))) pm_err("arg", "desc.param is required");
    d->n_param = q_axes(f, "param", d->param, YQST_MAX_PARAMS);
    if ((f = pm_field(s, "prior"))) {
        if (!mxIsCell(f) || (int)mxGetNumberOfElements(f) != d->n_param)
            pm_err("arg", "desc.prior must be a cell with one entry per parameter axis ([] for uniform)");
        for (i = 0; i < d->n_param; i++) {
            const mxArray* p = mxGetCell(f, (size_t)i);
            if (p && !mxIsEmpty(p)) {
                double* w = (double*)mxMalloc((size_t)d->param[i].n * sizeof(double));
                pm_vector_n(p, "prior", w, d->param[i].n);
                d->param[i].prior = w;
            }
        }
    }
    if ((f = pm_field(s, "nuisance"))) {
        double m[YQST_MAX_PARAMS];
        pm_vector_n(f, "nuisance", m, d->n_param);
        for (i = 0; i < d->n_param; i++) d->param[i].nuisance = m[i] != 0.0;
    }
    if ((f = pm_field(s, "joint_prior"))) {
        /* Given in the shape 'posterior' returns (first axis fastest); the
         * header wants LAYOUT order (last axis fastest). */
        int P = 1, np = d->n_param, k;
        int dims[YQST_MAX_PARAMS];
        double* src;
        double* dst;
        for (i = 0; i < np; i++) { dims[i] = d->param[i].n; P *= dims[i]; }
        if ((int)mxGetNumberOfElements(f) != P || !mxIsDouble(f))
            pm_err("arg", "desc.joint_prior must be a double array of %d weights", P);
        src = mxGetPr(f);
        dst = (double*)mxMalloc((size_t)P * sizeof(double));
        for (k = 0; k < P; k++) {
            /* k is the column-major index; build the row-major one. */
            int rem = k, c = 0;
            int sub[YQST_MAX_PARAMS];
            for (i = 0; i < np; i++) { sub[i] = rem % dims[i]; rem /= dims[i]; }
            for (i = 0; i < np; i++) c = c * dims[i] + sub[i];
            dst[c] = src[k];
        }
        d->joint_prior = dst;
    }
    if ((f = pm_field(s, "pf")))              d->pf = (yqst_pf)pm_enum(f, "pf", q_pf_names, 6);
    if ((f = pm_field(s, "n_outcomes")))      d->n_outcomes = pm_int(f, "n_outcomes");
    if ((f = pm_field(s, "select")))          d->select = (yqst_select)pm_enum(f, "select", q_sel_names, 4);
    if ((f = pm_field(s, "select_param")))    d->select_param = pm_index(f, "select_param", d->n_param);
    if ((f = pm_field(s, "select_quantile"))) d->select_quantile = pm_scalar(f, "select_quantile");
    if ((f = pm_field(s, "tiebreak")))        d->tiebreak = (yqst_tiebreak)pm_enum(f, "tiebreak", q_tie_names, 4);
    if ((f = pm_field(s, "tie_tolerance")))   d->tie_tolerance = pm_scalar(f, "tie_tolerance");
    if ((f = pm_field(s, "subset_size")))     d->subset_size = pm_int(f, "subset_size");
    if ((f = pm_field(s, "stop_trials")))     d->stop_trials = pm_int(f, "stop_trials");
    if ((f = pm_field(s, "stop_entropy")))    d->stop_entropy = pm_scalar(f, "stop_entropy");
    if ((f = pm_field(s, "stop_sd")))         d->stop_sd = pm_scalar(f, "stop_sd");
    if ((f = pm_field(s, "stop_sd_param")))   d->stop_sd_param = pm_index(f, "stop_sd_param", d->n_param);
    if ((f = pm_field(s, "no_table")))        d->no_table = pm_bool(f, "no_table") != 0;
    if ((f = pm_field(s, "pf_batch"))) {
        if (!mxIsClass(f, "function_handle"))
            pm_err("arg", "desc.pf_batch must be a function handle");
        if (d->pf != YQST_PF_GUMBEL && d->pf != YQST_PF_CUSTOM)
            pm_err("arg", "desc.pf_batch implies pf 'custom'");
        d->pf = YQST_PF_CUSTOM;
        if (d->n_outcomes == 0) d->n_outcomes = 2;
    }
    if (d->pf == YQST_PF_CUSTOM && !pm_field(s, "pf_batch"))
        pm_err("arg", "pf 'custom' needs desc.pf_batch (a function handle)");
    n->n_stim = d->n_stim;
    n->n_param = d->n_param;
    n->K = d->pf == YQST_PF_CUSTOM ? d->n_outcomes : 2;
    if (n->K < 1) n->K = 1;
}

/* Build a node from a desc struct, then open it, or load it from bytes. */
static qn* q_make(const mxArray* s, const uint8_t* bytes, size_t len) {
    yqst_desc d;
    qn* n;
    const mxArray* f;
    bool ok;
    q_shape shp;
    memset(&d, 0, sizeof(d));
    /* Parse first: a parse error raises, and nothing allocated with malloc
     * may be live at that point. */
    q_read_desc(s, &d, &shp);
    /* A bad seed must raise before anything below is allocated. */
    if ((f = pm_field(s, "rng")) && !mxIsClass(f, "function_handle")) (void)pm_u64(f, "rng");
    n = (qn*)calloc(1, sizeof(*n));
    if (!n) pm_err("memory", "out of memory");
    n->n_stim = shp.n_stim;
    n->n_param = shp.n_param;
    n->K = shp.K;
    if ((f = pm_field(s, "pf_batch"))) {
        n->pf = mxDuplicateArray(f);
        mexMakeArrayPersistent(n->pf);
        d.pf_batch = qn_pf_batch;
        d.pf_ctx = n;
    }
    if ((f = pm_field(s, "rng"))) {
        n->has_rng = pm_rng_parse(f, &n->rng);
        d.rng = pm_rng_call;
        d.rng_ctx = &n->rng;
    }
    n->busy = 1;
    ok = bytes ? yqst_open_saved(&n->q, &d, bytes, len) : yqst_open(&n->q, &d);
    n->busy = 0;
    if (!ok || pm_pending || n->cb_msg[0]) {
        char msg[300];
        char cb[256];
        snprintf(msg, sizeof(msg), "%s", ok ? "" : yqst_error(&n->q));
        snprintf(cb, sizeof(cb), "%s", n->cb_msg);
        qn_destroy(n);
        pm_rethrow();
        if (cb[0]) pm_err("callback", "%s", cb);
        pm_err(bytes ? "load" : "open", "%s", msg);
    }
    return n;
}

/* ---- outputs ------------------------------------------------------------ */

static mxArray* q_history(const yqst_quest* q, int n_stim) {
    static const char* fields[] = { "stim", "stim_index", "proposed_index", "outcome" };
    int n = 0, i, j;
    const yqst_trial* h = yqst_history(q, &n);
    mxArray* out = mxCreateStructMatrix(1, 1, 4, fields);
    mxArray* st = mxCreateDoubleMatrix((size_t)n, (size_t)n_stim, mxREAL);
    mxArray* si = mxCreateDoubleMatrix((size_t)n, 1, mxREAL);
    mxArray* pi = mxCreateDoubleMatrix((size_t)n, 1, mxREAL);
    mxArray* oc = mxCreateDoubleMatrix((size_t)n, 1, mxREAL);
    double *ps = mxGetPr(st), *psi = mxGetPr(si), *ppi = mxGetPr(pi), *po = mxGetPr(oc);
    for (i = 0; i < n; i++) {
        for (j = 0; j < n_stim; j++) ps[(size_t)j * n + i] = h[i].stim[j];
        psi[i] = h[i].stim_index + 1;       /* 0 = off the grid */
        ppi[i] = h[i].proposed_index + 1;   /* 0 = nothing proposed */
        po[i] = h[i].outcome;
    }
    mxSetField(out, 0, "stim", st);
    mxSetField(out, 0, "stim_index", si);
    mxSetField(out, 0, "proposed_index", pi);
    mxSetField(out, 0, "outcome", oc);
    return out;
}

static mxArray* q_policy_string(yrt_policy pol) {
    const char* name = yrt_policy_name(pol);
    char buf[32];
    size_t i = 0;
    for (; name[i] != 0 && i + 1 < sizeof(buf); i++)
        buf[i] = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] - 'A' + 'a') : name[i];
    buf[i] = 0;
    return mxCreateString(buf);
}

static mxArray* q_snapshot(const yqst_snapshot* s, int n_stim, int n_param) {
    static const char* fields[] = { "seq", "proposed", "stim", "update_rc", "n_trials",
                                    "done", "stop", "estimate", "entropy", "sd" };
    mxArray* out = mxCreateStructMatrix(1, 1, 10, fields);
    int st = (int)s->stop;
    mxSetField(out, 0, "seq", mxCreateDoubleScalar((double)s->seq));
    /* 1-based; a failed selection keeps its negative code. */
    mxSetField(out, 0, "proposed", mxCreateDoubleScalar(s->proposed >= 0 ? s->proposed + 1.0 : (double)s->proposed));
    mxSetField(out, 0, "stim", pm_row(s->stim, n_stim));
    mxSetField(out, 0, "update_rc", mxCreateDoubleScalar((double)s->update_rc));
    mxSetField(out, 0, "n_trials", mxCreateDoubleScalar((double)s->n_trials));
    mxSetField(out, 0, "done", mxCreateLogicalScalar(s->done));
    mxSetField(out, 0, "stop", mxCreateString(st >= 0 && st < 5 ? q_stop_names[st] : "none"));
    mxSetField(out, 0, "estimate", pm_row(s->estimate, n_param));
    mxSetField(out, 0, "entropy", mxCreateDoubleScalar(s->entropy));
    mxSetField(out, 0, "sd", mxCreateDoubleScalar(s->sd));
    return out;
}

/* The joint posterior as an n_0 x n_1 x ... array: element (i0+1, i1+1, ...)
 * is the mass at those axis indices, the MATLAB reading of the caller's axis
 * order. The header's buffer has the last axis fastest, so this is one
 * index transposition, not a permute() afterwards. */
static mxArray* q_posterior(const yqst_quest* q, int np) {
    const double* post = yqst_posterior(q);
    mwSize dims[YQST_MAX_PARAMS > 2 ? YQST_MAX_PARAMS : 2];
    int nd[YQST_MAX_PARAMS], sub[YQST_MAX_PARAMS];
    int i, P = yqst_n_param(q), t;
    mxArray* out;
    double* d;
    for (i = 0; i < np; i++) { nd[i] = yqst_param_axis_n(q, i); dims[i] = (mwSize)nd[i]; sub[i] = 0; }
    if (np == 1) dims[1] = 1;
    out = mxCreateNumericArray((mwSize)(np == 1 ? 2 : np), dims, mxDOUBLE_CLASS, mxREAL);
    d = mxGetPr(out);
    if (!post) return out;
    for (t = 0; t < P; t++) {
        /* sub[] is the odometer of the row-major index t. */
        size_t cm = 0, stride = 1;
        for (i = 0; i < np; i++) { cm += (size_t)sub[i] * stride; stride *= (size_t)nd[i]; }
        d[cm] = post[t];
        for (i = np - 1; i >= 0; i--) { if (++sub[i] < nd[i]) break; sub[i] = 0; }
    }
    return out;
}

static int q_stim_arg(qn* n, const mxArray* a) { return pm_index(a, "stimulus index", n->q.S); }

static void q_params_arg(qn* n, const mxArray* a, double* out) {
    pm_vector_n(a, "params", out, n->n_param);
}

/* ---- dispatch ----------------------------------------------------------- */

static void q_async_cmd(const char* cmd, int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[], qn* n);

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char* cmd;
    qn* n;
    yqst_quest* q;
    pm_destroy = qn_destroy;
    pm_pending = NULL;
    if (nrhs < 1 || !mxIsChar(prhs[0]))
        pm_err("usage", "first argument must be a command string");
    cmd = mxArrayToString(prhs[0]);

    /* ---- module commands ---- */
    if (strcmp(cmd, "open") == 0) {
        pm_nargs(nrhs, 2, "h = ysp_quest('open', desc)");
        n = q_make(prhs[1], NULL, 0);
        plhs[0] = pm_handle_out(pm_register(n));
        return;
    }
    if (strcmp(cmd, "load") == 0) {
        size_t len;
        const uint8_t* b;
        pm_nargs(nrhs, 3, "h = ysp_quest('load', bytes, desc)");
        b = pm_bytes_in(prhs[1], "bytes", &len);
        n = q_make(prhs[2], b, len);
        plhs[0] = pm_handle_out(pm_register(n));
        return;
    }
    if (strcmp(cmd, "version") == 0) { plhs[0] = mxCreateString(yqst_version()); return; }
    if (strcmp(cmd, "strerror") == 0) {
        pm_nargs(nrhs, 2, "ysp_quest('strerror', code)");
        plhs[0] = mxCreateString(yqst_strerror(pm_int(prhs[1], "code")));
        return;
    }
    if (strcmp(cmd, "memory_size") == 0) {
        /* Validates without calling any callback. */
        yqst_desc d;
        q_shape tmp;
        memset(&d, 0, sizeof(d));
        pm_nargs(nrhs, 2, "ysp_quest('memory_size', desc)");
        q_read_desc(prhs[1], &d, &tmp);
        if (pm_field(prhs[1], "pf_batch")) d.pf_batch = qn_pf_batch;
        if (pm_field(prhs[1], "rng")) d.rng = pm_rng_call;
        plhs[0] = mxCreateDoubleScalar((double)yqst_memory_size(&d));
        return;
    }
    if (strcmp(cmd, "prior_normal") == 0) {
        /* A Gaussian over an axis's points, for desc.prior. */
        yqst_axis ax;
        mxArray* out;
        pm_nargs(nrhs, 4, "w = ysp_quest('prior_normal', axis, mean, sd)");
        q_axis(prhs[1], "axis", &ax);
        out = mxCreateDoubleMatrix(1, (size_t)(ax.n > 0 ? ax.n : 0), mxREAL);
        if (ax.n < 1 || yqst_prior_normal(&ax, pm_scalar(prhs[2], "mean"), pm_scalar(prhs[3], "sd"), mxGetPr(out)) < 0)
            pm_err("arg", "prior_normal: invalid axis or sd");
        plhs[0] = out;
        return;
    }

    /* ---- handle commands ---- */
    if (nrhs < 2) pm_err("usage", "'%s' needs a handle", cmd);
    n = (qn*)pm_lookup(prhs[1]);
    q = &n->q;

    if (strcmp(cmd, "close") == 0) {
        if (n->busy) pm_err("busy", "a callback may not close the Quest it runs under");
        pm_unregister(n);
        qn_destroy(n);
        return;
    }
    if (strncmp(cmd, "async_", 6) == 0) {
        q_async_cmd(cmd, nlhs, plhs, nrhs, prhs, n);
        return;
    }
    if (n->busy) pm_err("busy", "a callback may not call into the Quest it runs under");
    if (n->async_running)
        pm_err("busy", "the Quest is owned by a running async session; call async_stop first");

    if (strcmp(cmd, "next") == 0 || strcmp(cmd, "next_subset") == 0) {
        int rc;
        if (cmd[4] == 0) {
            n->busy = 1;
            rc = yqst_next(q);
            n->busy = 0;
        } else {
            int m, i;
            int* sub;
            double* v;
            pm_nargs(nrhs, 3, "i = ysp_quest('next_subset', h, indices)");
            m = (int)mxGetNumberOfElements(prhs[2]);
            if (m < 1) pm_err("arg", "next_subset needs at least one index");
            v = (double*)mxMalloc((size_t)m * sizeof(double));
            sub = (int*)mxMalloc((size_t)m * sizeof(int));
            pm_vector(prhs[2], "indices", v, m);
            for (i = 0; i < m; i++) {
                if (v[i] != floor(v[i]) || v[i] < 1 || v[i] > q->S)
                    pm_err("arg", "indices must be integers in 1..%d", q->S);
                sub[i] = (int)v[i] - 1;
            }
            n->busy = 1;
            rc = yqst_next_subset(q, sub, m);
            n->busy = 0;
        }
        qn_after_callbacks(n);
        q_check(rc);
        plhs[0] = mxCreateDoubleScalar(rc + 1.0);
        if (nlhs >= 2) {
            double x[YQST_MAX_STIM_DIMS];
            yqst_stim_values(q, rc, x);
            plhs[1] = pm_row(x, n->n_stim);
        }
    } else if (strcmp(cmd, "expected_entropy") == 0) {
        /* One index, a vector of them, or none for the whole landscape. */
        int m, i;
        mxArray* out;
        double* o;
        if (nrhs >= 3) {
            m = (int)mxGetNumberOfElements(prhs[2]);
            out = mxCreateDoubleMatrix((size_t)m, 1, mxREAL);
            o = mxGetPr(out);
            pm_vector(prhs[2], "indices", o, m);
            for (i = 0; i < m; i++) {
                if (o[i] != floor(o[i]) || o[i] < 1 || o[i] > q->S)
                    pm_err("arg", "indices must be integers in 1..%d", q->S);
                o[i] = yqst_expected_entropy(q, (int)o[i] - 1);
            }
        } else {
            m = q->S;
            out = mxCreateDoubleMatrix((size_t)m, 1, mxREAL);
            o = mxGetPr(out);
            for (i = 0; i < m; i++) o[i] = yqst_expected_entropy(q, i);
        }
        plhs[0] = out;
    } else if (strcmp(cmd, "update") == 0) {
        int rc;
        pm_nargs(nrhs, 4, "ysp_quest('update', h, index, outcome)");
        {
            int i = q_stim_arg(n, prhs[2]);
            int k = pm_int(prhs[3], "outcome");
            n->busy = 1;
            rc = yqst_update(q, i, k);
            n->busy = 0;
        }
        qn_after_callbacks(n);
        q_check(rc);
    } else if (strcmp(cmd, "update_values") == 0) {
        double x[YQST_MAX_STIM_DIMS];
        int rc, k;
        pm_nargs(nrhs, 4, "ysp_quest('update_values', h, stim, outcome)");
        pm_vector_n(prhs[2], "stim", x, n->n_stim);
        k = pm_int(prhs[3], "outcome");
        n->busy = 1;
        rc = yqst_update_values(q, x, k);
        n->busy = 0;
        qn_after_callbacks(n);
        q_check(rc);
    } else if (strcmp(cmd, "done") == 0) {
        plhs[0] = mxCreateLogicalScalar(yqst_done(q));
    } else if (strcmp(cmd, "stop_reason") == 0) {
        int r = (int)yqst_stop_reason(q);
        plhs[0] = mxCreateString(r >= 0 && r < 5 ? q_stop_names[r] : "none");
    } else if (strcmp(cmd, "estimate") == 0) {
        double e[YQST_MAX_PARAMS];
        yqst_estimator how = nrhs >= 3 ? (yqst_estimator)pm_enum(prhs[2], "estimator", q_est_names, 3) : YQST_EST_MEAN;
        q_check(yqst_estimate(q, how, e));
        plhs[0] = pm_row(e, n->n_param);
    } else if (strcmp(cmd, "quantile") == 0) {
        pm_nargs(nrhs, 4, "ysp_quest('quantile', h, axis, p)");
        plhs[0] = mxCreateDoubleScalar(yqst_quantile(q, pm_index(prhs[2], "axis", n->n_param), pm_scalar(prhs[3], "p")));
    } else if (strcmp(cmd, "sd") == 0) {
        pm_nargs(nrhs, 3, "ysp_quest('sd', h, axis)");
        plhs[0] = mxCreateDoubleScalar(yqst_sd(q, pm_index(prhs[2], "axis", n->n_param)));
    } else if (strcmp(cmd, "marginal") == 0) {
        int ax, m;
        mxArray* out;
        pm_nargs(nrhs, 3, "ysp_quest('marginal', h, axis)");
        ax = pm_index(prhs[2], "axis", n->n_param);
        m = yqst_param_axis_n(q, ax);
        out = mxCreateDoubleMatrix((size_t)m, 1, mxREAL);
        q_check(yqst_marginal(q, ax, mxGetPr(out)));
        plhs[0] = out;
    } else if (strcmp(cmd, "posterior") == 0) {
        plhs[0] = q_posterior(q, n->n_param);
    } else if (strcmp(cmd, "entropy") == 0) {
        plhs[0] = mxCreateDoubleScalar(yqst_entropy(q));
    } else if (strcmp(cmd, "p") == 0 || strcmp(cmd, "p_values") == 0 || strcmp(cmd, "simulate") == 0) {
        double prm[YQST_MAX_PARAMS], pr[YQST_MAX_OUTCOMES], x[YQST_MAX_STIM_DIMS];
        int rc, idx = -1;
        int is_values = cmd[1] == '_', is_sim = cmd[0] == 's';
        pm_nargs(nrhs, is_sim ? 5 : 4, is_sim ? "ysp_quest('simulate', h, index, params, u)"
                                              : "ysp_quest('p', h, index, params)");
        if (is_values) pm_vector_n(prhs[2], "stim", x, n->n_stim);
        else idx = q_stim_arg(n, prhs[2]);
        q_params_arg(n, prhs[3], prm);
        n->busy = 1;
        if (is_sim) rc = yqst_simulate(q, idx, prm, pm_scalar(prhs[4], "u"));
        else if (is_values) rc = yqst_p_values(q, x, prm, pr);
        else rc = yqst_p(q, idx, prm, pr);
        n->busy = 0;
        qn_after_callbacks(n);
        q_check(rc);
        plhs[0] = is_sim ? mxCreateDoubleScalar((double)rc) : pm_row(pr, q->K);
    } else if (strcmp(cmd, "stim_value") == 0 || strcmp(cmd, "param_value") == 0) {
        int is_stim = cmd[0] == 's';
        int idx, ax = 0;
        pm_nargs(nrhs, 3, "ysp_quest('stim_value', h, index [, axis])");
        idx = pm_index(prhs[2], "index", is_stim ? q->S : q->P);
        if (nrhs >= 4) ax = pm_index(prhs[3], "axis", is_stim ? n->n_stim : n->n_param);
        plhs[0] = mxCreateDoubleScalar(is_stim ? yqst_stim_value(q, idx, ax) : yqst_param_value(q, idx, ax));
    } else if (strcmp(cmd, "stim_values") == 0 || strcmp(cmd, "param_values") == 0) {
        double v[YQST_MAX_PARAMS > YQST_MAX_STIM_DIMS ? YQST_MAX_PARAMS : YQST_MAX_STIM_DIMS];
        int is_stim = cmd[0] == 's';
        int idx;
        pm_nargs(nrhs, 3, "ysp_quest('stim_values', h, index)");
        idx = pm_index(prhs[2], "index", is_stim ? q->S : q->P);
        q_check(is_stim ? yqst_stim_values(q, idx, v) : yqst_param_values(q, idx, v));
        plhs[0] = pm_row(v, is_stim ? n->n_stim : n->n_param);
    } else if (strcmp(cmd, "stim_index") == 0 || strcmp(cmd, "param_index") == 0) {
        double v[YQST_MAX_PARAMS];
        int sub[YQST_MAX_PARAMS], i, m, rc;
        int is_stim = cmd[0] == 's';
        pm_nargs(nrhs, 3, "ysp_quest('stim_index', h, subs)");
        m = is_stim ? n->n_stim : n->n_param;
        pm_vector_n(prhs[2], "subs", v, m);
        for (i = 0; i < m; i++) {
            if (v[i] != floor(v[i])) pm_err("arg", "subs must be integers");
            sub[i] = (int)v[i] - 1;
        }
        rc = is_stim ? yqst_stim_index(q, sub) : yqst_param_index(q, sub);
        q_check(rc);
        plhs[0] = mxCreateDoubleScalar(rc + 1.0);
    } else if (strcmp(cmd, "stim_nearest") == 0) {
        double x[YQST_MAX_STIM_DIMS];
        int rc;
        pm_nargs(nrhs, 3, "ysp_quest('stim_nearest', h, values)");
        pm_vector_n(prhs[2], "values", x, n->n_stim);
        rc = yqst_stim_nearest(q, x);
        q_check(rc);
        plhs[0] = mxCreateDoubleScalar(rc + 1.0);
    } else if (strcmp(cmd, "n_stim") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)q->S);
    } else if (strcmp(cmd, "n_param") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)q->P);
    } else if (strcmp(cmd, "n_outcomes") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)q->K);
    } else if (strcmp(cmd, "stim_shape") == 0 || strcmp(cmd, "param_shape") == 0) {
        double v[YQST_MAX_PARAMS];
        int i, is_stim = cmd[0] == 's', m = is_stim ? n->n_stim : n->n_param;
        for (i = 0; i < m; i++) v[i] = is_stim ? yqst_stim_axis_n(q, i) : yqst_param_axis_n(q, i);
        plhs[0] = pm_row(v, m);
    } else if (strcmp(cmd, "n_trials") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)yqst_n_trials(q));
    } else if (strcmp(cmd, "history") == 0) {
        plhs[0] = q_history(q, n->n_stim);
    } else if (strcmp(cmd, "save") == 0) {
        size_t sz = yqst_save_size(q);
        mxArray* out = mxCreateNumericMatrix(1, sz, mxUINT8_CLASS, mxREAL);
        q_check(yqst_save(q, mxGetData(out), sz));
        plhs[0] = out;
    } else if (strcmp(cmd, "rng_state") == 0) {
        if (!n->rng.has_seed) pm_err("arg", "rng_state needs an integer-seed rng");
        if (nrhs >= 3) n->rng.state = pm_u64(prhs[2], "state");
        else plhs[0] = pm_u64_out(n->rng.state);
    } else {
        pm_err("usage", "unknown command '%s'", cmd);
    }
}

/* ---- async -------------------------------------------------------------- */

static const char* const qa_fields[] = { "estimator", "below_normal", "pin_cpu", "queue_depth" };

static double q_timeout(int nrhs, const mxArray* prhs[], int at) {
    double t = nrhs > at ? pm_scalar(prhs[at], "timeout_s") : 1.0;
    if (!(t >= 0) || t > 1e6) pm_err("arg", "timeout_s must be in [0, 1e6] seconds");
    return t;
}

static void q_async_cmd(const char* cmd, int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[], qn* n) {
    yqst_snapshot snap;
    (void)nlhs;
    if (n->busy) pm_err("busy", "a callback may not call into the Quest it runs under");
    if (strcmp(cmd, "async_start") == 0) {
        yqst_async_desc ad;
        const mxArray* f;
        if (n->async_running) pm_err("busy", "async session already running");
        /* The thread is C and must never need the interpreter. */
        if (n->rng.fn)
            pm_err("arg", "async_start refuses a Quest whose rng is a MATLAB function; use an integer seed");
        if (n->pf && n->q.desc.no_table)
            pm_err("arg", "async_start refuses a MATLAB pf_batch under no_table: the thread would call MATLAB every trial");
        memset(&ad, 0, sizeof(ad));
        if (nrhs >= 3 && !mxIsEmpty(prhs[2])) {
            pm_check_fields(prhs[2], qa_fields, 4, "async opts");
            if ((f = pm_field(prhs[2], "estimator")))   ad.estimator = (yqst_estimator)pm_enum(f, "estimator", q_est_names, 3);
            if ((f = pm_field(prhs[2], "below_normal"))) ad.below_normal = pm_bool(f, "below_normal") != 0;
            if ((f = pm_field(prhs[2], "pin_cpu")))     ad.pin_cpu = pm_int(f, "pin_cpu");
            if ((f = pm_field(prhs[2], "queue_depth"))) ad.queue_depth = pm_int(f, "queue_depth");
        }
        ad.quest = &n->q;
        memset(&n->a, 0, sizeof(n->a));
        if (!yqst_async_open(&n->a, &ad))
            pm_err("async", "%s", yqst_async_error(&n->a));
        n->async_running = 1;
        return;
    }
    if (strcmp(cmd, "async_stop") == 0) {
        if (n->async_running) { yqst_async_close(&n->a); n->async_running = 0; }
        return;
    }
    if (strcmp(cmd, "async_policy") == 0) {
        plhs[0] = q_policy_string(yqst_async_policy(&n->a));
        return;
    }
    if (strcmp(cmd, "async_running") == 0) {
        plhs[0] = mxCreateLogicalScalar(n->async_running && yqst_async_is_running(&n->a));
        return;
    }
    if (!n->async_running) pm_err("closed", "no async session is running; call async_start");
    if (strcmp(cmd, "async_submit") == 0) {
        int rc;
        pm_nargs(nrhs, 4, "seq = ysp_quest('async_submit', h, index, outcome)");
        rc = yqst_async_submit(&n->a, q_stim_arg(n, prhs[2]), pm_int(prhs[3], "outcome"));
        q_check(rc);
        plhs[0] = mxCreateDoubleScalar((double)rc);
    } else if (strcmp(cmd, "async_submit_values") == 0) {
        double x[YQST_MAX_STIM_DIMS];
        int rc;
        pm_nargs(nrhs, 4, "seq = ysp_quest('async_submit_values', h, stim, outcome)");
        if (n->pf) pm_err("arg", "async_submit_values refuses a MATLAB pf_batch: the thread would call MATLAB");
        pm_vector_n(prhs[2], "stim", x, n->n_stim);
        rc = yqst_async_submit_values(&n->a, x, pm_int(prhs[3], "outcome"));
        q_check(rc);
        plhs[0] = mxCreateDoubleScalar((double)rc);
    } else if (strcmp(cmd, "async_poll") == 0) {
        q_check(yqst_async_poll(&n->a, &snap));
        plhs[0] = q_snapshot(&snap, n->n_stim, n->n_param);
    } else if (strcmp(cmd, "async_wait") == 0) {
        int rc;
        double t;
        pm_nargs(nrhs, 3, "snap = ysp_quest('async_wait', h, seq [, timeout_s])");
        {
            int seq = pm_int(prhs[2], "seq");
            if (seq < 0) pm_err("arg", "seq must be >= 0");
            t = q_timeout(nrhs, prhs, 3);
            rc = yqst_async_wait(&n->a, (uint32_t)seq, (int64_t)(t * 1e9), &snap);
        }
        q_check(rc);
        plhs[0] = q_snapshot(&snap, n->n_stim, n->n_param);
    } else if (strcmp(cmd, "async_pending") == 0) {
        int rc = yqst_async_pending(&n->a);
        q_check(rc);
        plhs[0] = mxCreateDoubleScalar((double)rc);
    } else {
        pm_err("usage", "unknown command '%s'", cmd);
    }
}
