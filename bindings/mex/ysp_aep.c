/* ysp_aep.c - MATLAB/Octave MEX binding for ysp/aep.h (builds to ysp_aep.<mexext>)
 *
 * Gaussian-process adaptive psychophysics, and the header's YAEP_ASYNC layer:
 * a C thread that runs the update, the selection and the threshold search
 * between trials. Command dispatch in the style of widmann's ppdev-mex.
 *
 *     d = struct('lo', [0 -3], 'hi', [1.5 0], 'intensity_dim', 2, ...
 *                'acq', 'eavc', 'target_p', 0.75, 'grid', [15 25], ...
 *                'n_init', 10, 'fit', true, 'fit_every', 10, 'stop_trials', 150);
 *     h = ysp_aep('open', d);
 *     while ~ysp_aep('done', h)
 *         x = ysp_aep('next', h);                  % 1 x n_dims
 *         ysp_aep('update', h, x, run_trial(x));   % outcome 1 = yes
 *     end
 *     [thr, lo, hi] = ysp_aep('threshold', h, 0.7);
 *     ysp_aep('close', h);
 *
 * Build with build.m. See README.md for the command reference.
 *
 * A MATLAB rng runs on the interpreter thread through mexCallMATLABWithTrap;
 * an error in it is raised after the header call returns. The async thread is
 * C and never enters the interpreter, so async_start refuses a MATLAB rng.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define YAEP_ASYNC
#define YSP_AEP_IMPLEMENTATION
#include "ysp/aep.h"

#define PM_MOD "ysp_aep"
#include "ysp_mex_util.h"

typedef struct gn {
    yaep_gp    g;
    yaep_async a;
    int         async_running;
    int         busy;         /* inside a header call that may run MATLAB  */
    pm_rng      rng;
    double*     cand;         /* the binding's copy of desc.candidates      */
    double*     scratch;      /* row-major points for the *_many calls      */
    size_t      scratch_cap;  /* doubles; grows, never shrinks              */
    int         nd;
} gn;

static void gn_destroy(void* obj) {
    gn* n = (gn*)obj;
    if (n->async_running) { yaep_async_close(&n->a); n->async_running = 0; }
    yaep_close(&n->g);
    pm_rng_free(&n->rng);
    free(n->cand);
    free(n->scratch);
    free(n);
}

static const char* const g_lik_names[]   = { "bernoulli", "ordinal", "categorical", "gaussian", "pairwise" };
static const char* const g_kern_names[]  = { "rbf", "semip" };
static const char* const g_link_names[]  = { "probit", "logit" };
static const char* const g_model_names[] = { "gp", "psychometric" };
static const char* const g_dim_names[]   = { "continuous", "integer", "categorical" };
static const char* const g_acq_names[]   = { "lse", "eavc", "localmi", "balv", "bald", "random", "ucb", "ei", "thompson" };
static const char* const g_stop_names[]  = { "none", "trials", "threshold_sd", "full" };

static void g_check(int rc) {
    if (rc >= 0) return;
    switch (rc) {
        case YAEP_ERR_ARG:     pm_err("arg", "%s", yaep_strerror(rc)); break;
        case YAEP_ERR_CLOSED:  pm_err("closed", "%s", yaep_strerror(rc)); break;
        case YAEP_ERR_FULL:    pm_err("full", "%s", yaep_strerror(rc)); break;
        case YAEP_ERR_MEMORY:  pm_err("memory", "%s", yaep_strerror(rc)); break;
        case YAEP_ERR_NOCROSS: pm_err("nocross", "%s", yaep_strerror(rc)); break;
        case YAEP_ERR_NUMERIC: pm_err("numeric", "%s", yaep_strerror(rc)); break;
        case YAEP_ERR_BUSY:    pm_err("busy", "%s", yaep_strerror(rc)); break;
        case YAEP_ERR_TIMEOUT: pm_err("timeout", "%s", yaep_strerror(rc)); break;
        default:                pm_err("error", "%s", yaep_strerror(rc)); break;
    }
}

/* ---- desc --------------------------------------------------------------- */

static const char* const g_fields[] = {
    "n_dims", "lo", "hi", "intensity_dim", "lik", "n_outcomes", "kernel", "link",
    "guess", "lapse", "hyper", "fit", "fit_every", "no_hyper_prior", "refit_every",
    "hyper_min", "hyper_max", "jitter", "acq", "target_p", "target_value",
    "target_outcome", "acq_beta", "n_init", "rng", "candidates", "n_candidates",
    "grid", "stop_trials", "stop_threshold_sd", "stop_context", "max_trials",
    "refine_steps", "model", "priors", "minimize", "dim_kind", "dim_levels",
    "monotone_dims", "pcg_threshold", "fit_max_evals", "fit_tol", "fit_pcg"
};

static const char* const g_hyper_fields[] = {
    "lengthscale", "outputscale", "mean", "lengthscale_b", "outputscale_b",
    "cutpoint", "noise_sd", "lengthscale_g", "outputscale_g", "mean_g"
};

static const char* const g_prior_fields[] = {
    "lengthscale", "outputscale", "outputscale_b", "outputscale_g", "mean",
    "mean_g", "noise_sd"
};

static void g_read_hyper(const mxArray* s, const char* what, yaep_hyper* h) {
    const mxArray* f;
    pm_check_fields(s, g_hyper_fields, 10, what);
    if ((f = pm_field(s, "lengthscale")))   pm_vector(f, "lengthscale", h->lengthscale, YAEP_MAX_DIMS);
    if ((f = pm_field(s, "outputscale")))   h->outputscale = pm_scalar(f, "outputscale");
    if ((f = pm_field(s, "mean")))          h->mean = pm_scalar(f, "mean");
    if ((f = pm_field(s, "lengthscale_b"))) pm_vector(f, "lengthscale_b", h->lengthscale_b, YAEP_MAX_DIMS);
    if ((f = pm_field(s, "outputscale_b"))) h->outputscale_b = pm_scalar(f, "outputscale_b");
    if ((f = pm_field(s, "cutpoint")))      pm_vector(f, "cutpoint", h->cutpoint, YAEP_MAX_OUTCOMES - 1);
    if ((f = pm_field(s, "noise_sd")))      h->noise_sd = pm_scalar(f, "noise_sd");
    if ((f = pm_field(s, "lengthscale_g"))) pm_vector(f, "lengthscale_g", h->lengthscale_g, YAEP_MAX_DIMS);
    if ((f = pm_field(s, "outputscale_g"))) h->outputscale_g = pm_scalar(f, "outputscale_g");
    if ((f = pm_field(s, "mean_g")))        h->mean_g = pm_scalar(f, "mean_g");
}

/* A prior is [center sd], [center sd ceiling], or a struct with those
 * fields. */
static void g_read_prior(const mxArray* a, const char* what, yaep_prior* p) {
    if (mxIsStruct(a)) {
        static const char* const pf[] = { "center", "sd", "ceiling" };
        const mxArray* f;
        pm_check_fields(a, pf, 3, what);
        if ((f = pm_field(a, "center")))  p->center = pm_scalar(f, "center");
        if ((f = pm_field(a, "sd")))      p->sd = pm_scalar(f, "sd");
        if ((f = pm_field(a, "ceiling"))) p->ceiling = pm_scalar(f, "ceiling");
    } else {
        double v[3] = { 0, 0, 0 };
        int m = pm_vector(a, what, v, 3);
        if (m < 2) pm_err("arg", "%s: a prior is [center sd] or [center sd ceiling]", what);
        p->center = v[0];
        p->sd = v[1];
        p->ceiling = v[2];
    }
}

static void g_read_priors(const mxArray* s, yaep_priors* p) {
    const mxArray* f;
    pm_check_fields(s, g_prior_fields, 7, "priors");
    if ((f = pm_field(s, "lengthscale")))   g_read_prior(f, "priors.lengthscale", &p->lengthscale);
    if ((f = pm_field(s, "outputscale")))   g_read_prior(f, "priors.outputscale", &p->outputscale);
    if ((f = pm_field(s, "outputscale_b"))) g_read_prior(f, "priors.outputscale_b", &p->outputscale_b);
    if ((f = pm_field(s, "outputscale_g"))) g_read_prior(f, "priors.outputscale_g", &p->outputscale_g);
    if ((f = pm_field(s, "mean")))          g_read_prior(f, "priors.mean", &p->mean);
    if ((f = pm_field(s, "mean_g")))        g_read_prior(f, "priors.mean_g", &p->mean_g);
    if ((f = pm_field(s, "noise_sd")))      g_read_prior(f, "priors.noise_sd", &p->noise_sd);
}

/* Parses every number; returns the candidates as an mxArray (or NULL) for the
 * caller to copy once the node exists. */
static const mxArray* g_read_desc(const mxArray* s, yaep_desc* d) {
    const mxArray* f;
    const mxArray* cand = NULL;
    int nd, i;
    pm_check_fields(s, g_fields, (int)(sizeof(g_fields) / sizeof(g_fields[0])), "desc");
    if (!(f = pm_field(s, "lo"))) pm_err("arg", "desc.lo is required");
    nd = pm_vector(f, "lo", d->lo, YAEP_MAX_DIMS);
    if (!(f = pm_field(s, "hi"))) pm_err("arg", "desc.hi is required");
    if (pm_vector(f, "hi", d->hi, YAEP_MAX_DIMS) != nd) pm_err("arg", "desc.lo and desc.hi differ in length");
    d->n_dims = nd;
    if ((f = pm_field(s, "n_dims")) && pm_int(f, "n_dims") != nd)
        pm_err("arg", "desc.n_dims disagrees with numel(desc.lo)");
    if ((f = pm_field(s, "intensity_dim")))  d->intensity_dim = pm_index(f, "intensity_dim", nd);
    if ((f = pm_field(s, "lik")))            d->lik = (yaep_lik)pm_enum(f, "lik", g_lik_names, 5);
    if ((f = pm_field(s, "n_outcomes")))     d->n_outcomes = pm_int(f, "n_outcomes");
    if ((f = pm_field(s, "kernel")))         d->kernel = (yaep_kernel)pm_enum(f, "kernel", g_kern_names, 2);
    if ((f = pm_field(s, "link")))           d->link = (yaep_link)pm_enum(f, "link", g_link_names, 2);
    if ((f = pm_field(s, "model")))          d->model = (yaep_model)pm_enum(f, "model", g_model_names, 2);
    if ((f = pm_field(s, "guess")))          d->guess = pm_scalar(f, "guess");
    if ((f = pm_field(s, "lapse")))          d->lapse = pm_scalar(f, "lapse");
    if ((f = pm_field(s, "hyper")))          g_read_hyper(f, "hyper", &d->hyper);
    if ((f = pm_field(s, "hyper_min")))      g_read_hyper(f, "hyper_min", &d->hyper_min);
    if ((f = pm_field(s, "hyper_max")))      g_read_hyper(f, "hyper_max", &d->hyper_max);
    if ((f = pm_field(s, "priors")))         g_read_priors(f, &d->priors);
    if ((f = pm_field(s, "fit")))            d->fit = pm_bool(f, "fit") != 0;
    if ((f = pm_field(s, "fit_every")))      d->fit_every = pm_int(f, "fit_every");
    if ((f = pm_field(s, "no_hyper_prior"))) d->no_hyper_prior = pm_bool(f, "no_hyper_prior") != 0;
    if ((f = pm_field(s, "refit_every")))    d->refit_every = pm_int(f, "refit_every");
    if ((f = pm_field(s, "jitter")))         d->jitter = pm_scalar(f, "jitter");
    if ((f = pm_field(s, "acq")))            d->acq = (yaep_acq)pm_enum(f, "acq", g_acq_names, 9);
    if ((f = pm_field(s, "target_p")))       d->target_p = pm_scalar(f, "target_p");
    if ((f = pm_field(s, "target_value")))   d->target_value = pm_scalar(f, "target_value");
    if ((f = pm_field(s, "target_outcome"))) d->target_outcome = pm_int(f, "target_outcome");
    if ((f = pm_field(s, "acq_beta")))       d->acq_beta = pm_scalar(f, "acq_beta");
    if ((f = pm_field(s, "n_init")))         d->n_init = pm_int(f, "n_init");
    if ((f = pm_field(s, "n_candidates")))   d->n_candidates = pm_int(f, "n_candidates");
    if ((f = pm_field(s, "grid"))) {
        double v[YAEP_MAX_DIMS];
        if (pm_vector(f, "grid", v, YAEP_MAX_DIMS) != nd) pm_err("arg", "desc.grid needs one count per dimension");
        for (i = 0; i < nd; i++) {
            if (v[i] != floor(v[i])) pm_err("arg", "desc.grid entries must be integers");
            d->grid[i] = (int)v[i];
        }
    }
    if ((f = pm_field(s, "candidates"))) {
        if (!mxIsDouble(f) || mxIsComplex(f) || (int)mxGetN(f) != nd)
            pm_err("arg", "desc.candidates must be a real M x n_dims double matrix");
        cand = f;
        d->n_candidates = (int)mxGetM(f);
    }
    if ((f = pm_field(s, "stop_trials")))       d->stop_trials = pm_int(f, "stop_trials");
    if ((f = pm_field(s, "stop_threshold_sd"))) d->stop_threshold_sd = pm_scalar(f, "stop_threshold_sd");
    if ((f = pm_field(s, "stop_context")))      pm_vector(f, "stop_context", d->stop_context, YAEP_MAX_DIMS);
    if ((f = pm_field(s, "max_trials")))        d->max_trials = pm_int(f, "max_trials");
    if ((f = pm_field(s, "refine_steps")))      d->refine_steps = pm_int(f, "refine_steps");
    if ((f = pm_field(s, "minimize")))          d->minimize = pm_bool(f, "minimize") != 0;
    if ((f = pm_field(s, "dim_kind"))) {
        if (!mxIsCell(f) || (int)mxGetNumberOfElements(f) != nd)
            pm_err("arg", "desc.dim_kind must be a cell with one name per dimension");
        for (i = 0; i < nd; i++)
            d->dim_kind[i] = (yaep_dim_kind)pm_enum(mxGetCell(f, (size_t)i), "dim_kind", g_dim_names, 3);
    }
    if ((f = pm_field(s, "dim_levels"))) {
        double v[YAEP_MAX_DIMS];
        if (pm_vector(f, "dim_levels", v, YAEP_MAX_DIMS) != nd) pm_err("arg", "desc.dim_levels needs one entry per dimension");
        for (i = 0; i < nd; i++) d->dim_levels[i] = (int)v[i];
    }
    if ((f = pm_field(s, "monotone_dims"))) {
        /* A list of 1-based dimensions; the header wants a bit mask. */
        double v[YAEP_MAX_DIMS];
        int m = pm_vector(f, "monotone_dims", v, YAEP_MAX_DIMS);
        for (i = 0; i < m; i++) {
            if (v[i] != floor(v[i]) || v[i] < 1 || v[i] > nd)
                pm_err("arg", "desc.monotone_dims entries must be dimensions in 1..%d", nd);
            d->monotone_dims |= 1u << ((int)v[i] - 1);
        }
    }
    if ((f = pm_field(s, "pcg_threshold"))) d->pcg_threshold = pm_int(f, "pcg_threshold");
    if ((f = pm_field(s, "fit_max_evals"))) d->fit_max_evals = pm_int(f, "fit_max_evals");
    if ((f = pm_field(s, "fit_tol")))       d->fit_tol = pm_scalar(f, "fit_tol");
    if ((f = pm_field(s, "fit_pcg")))       d->fit_pcg = pm_bool(f, "fit_pcg") != 0;
    return cand;
}

static gn* g_make(const mxArray* s, const uint8_t* bytes, size_t len) {
    yaep_desc d;
    const mxArray* cand;
    const mxArray* f;
    gn* n;
    bool ok;
    memset(&d, 0, sizeof(d));
    cand = g_read_desc(s, &d);
    /* A bad seed must raise before anything below is allocated. */
    if ((f = pm_field(s, "rng")) && !mxIsClass(f, "function_handle")) (void)pm_u64(f, "rng");
    n = (gn*)calloc(1, sizeof(*n));
    if (!n) pm_err("memory", "out of memory");
    n->nd = d.n_dims;
    if (cand) {
        /* The header keeps the pointer for the life of the handle, so the
         * binding owns a row-major copy. */
        int M = d.n_candidates, i, j, nd = d.n_dims;
        const double* src = mxGetPr(cand);
        n->cand = (double*)malloc((size_t)(M > 0 ? M : 1) * nd * sizeof(double));
        if (!n->cand) { free(n); pm_err("memory", "out of memory"); }
        for (i = 0; i < M; i++)
            for (j = 0; j < nd; j++) n->cand[(size_t)i * nd + j] = src[(size_t)j * M + i];
        d.candidates = n->cand;
    }
    if ((f = pm_field(s, "rng"))) {
        pm_rng_parse(f, &n->rng);
        d.rng = pm_rng_call;
        d.rng_ctx = &n->rng;
    }
    n->busy = 1;
    ok = bytes ? yaep_open_saved(&n->g, &d, bytes, len) : yaep_open(&n->g, &d);
    n->busy = 0;
    if (!ok || pm_pending) {
        char msg[300];
        snprintf(msg, sizeof(msg), "%s", ok ? "" : yaep_error(&n->g));
        gn_destroy(n);
        pm_rethrow();
        pm_err(bytes ? "load" : "open", "%s", msg);
    }
    return n;
}

/* ---- outputs ------------------------------------------------------------ */

static mxArray* g_hyper_out(const yaep_hyper* h, int nd, int n_cut) {
    mxArray* s = mxCreateStructMatrix(1, 1, 10, (const char**)g_hyper_fields);
    mxSetField(s, 0, "lengthscale",   pm_row(h->lengthscale, nd));
    mxSetField(s, 0, "outputscale",   mxCreateDoubleScalar(h->outputscale));
    mxSetField(s, 0, "mean",          mxCreateDoubleScalar(h->mean));
    mxSetField(s, 0, "lengthscale_b", pm_row(h->lengthscale_b, nd));
    mxSetField(s, 0, "outputscale_b", mxCreateDoubleScalar(h->outputscale_b));
    mxSetField(s, 0, "cutpoint",      pm_row(h->cutpoint, n_cut));
    mxSetField(s, 0, "noise_sd",      mxCreateDoubleScalar(h->noise_sd));
    mxSetField(s, 0, "lengthscale_g", pm_row(h->lengthscale_g, nd));
    mxSetField(s, 0, "outputscale_g", mxCreateDoubleScalar(h->outputscale_g));
    mxSetField(s, 0, "mean_g",        mxCreateDoubleScalar(h->mean_g));
    return s;
}

static int g_n_cut(const yaep_gp* g) {
    return g->desc.lik == YAEP_LIK_ORDINAL && g->desc.n_outcomes > 1 ? g->desc.n_outcomes - 1 : 0;
}

static mxArray* g_prior_out(const yaep_prior* p) {
    static const char* f[] = { "center", "sd", "ceiling" };
    mxArray* s = mxCreateStructMatrix(1, 1, 3, f);
    mxSetField(s, 0, "center", mxCreateDoubleScalar(p->center));
    mxSetField(s, 0, "sd", mxCreateDoubleScalar(p->sd));
    mxSetField(s, 0, "ceiling", mxCreateDoubleScalar(p->ceiling));
    return s;
}

static mxArray* g_priors_out(const yaep_priors* p) {
    mxArray* s = mxCreateStructMatrix(1, 1, 7, (const char**)g_prior_fields);
    mxSetField(s, 0, "lengthscale",   g_prior_out(&p->lengthscale));
    mxSetField(s, 0, "outputscale",   g_prior_out(&p->outputscale));
    mxSetField(s, 0, "outputscale_b", g_prior_out(&p->outputscale_b));
    mxSetField(s, 0, "outputscale_g", g_prior_out(&p->outputscale_g));
    mxSetField(s, 0, "mean",          g_prior_out(&p->mean));
    mxSetField(s, 0, "mean_g",        g_prior_out(&p->mean_g));
    mxSetField(s, 0, "noise_sd",      g_prior_out(&p->noise_sd));
    return s;
}

static mxArray* g_history(const yaep_gp* g, int nd) {
    static const char* fields[] = { "x", "y", "proposed", "init", "x2" };
    int n = 0, i, j, pair = g->desc.lik == YAEP_LIK_PAIRWISE;
    const yaep_trial* h = yaep_history(g, &n);
    mxArray* out = mxCreateStructMatrix(1, 1, pair ? 5 : 4, fields);
    mxArray* x = mxCreateDoubleMatrix((size_t)n, (size_t)nd, mxREAL);
    mxArray* x2 = pair ? mxCreateDoubleMatrix((size_t)n, (size_t)nd, mxREAL) : NULL;
    mxArray* y = mxCreateDoubleMatrix((size_t)n, 1, mxREAL);
    mxArray* pr = mxCreateLogicalMatrix((size_t)n, 1);
    mxArray* in = mxCreateLogicalMatrix((size_t)n, 1);
    double *px = mxGetPr(x), *py = mxGetPr(y);
    mxLogical *ppr = mxGetLogicals(pr), *pin = mxGetLogicals(in);
    for (i = 0; i < n; i++) {
        for (j = 0; j < nd; j++) {
            px[(size_t)j * n + i] = h[i].x[j];
            if (pair) mxGetPr(x2)[(size_t)j * n + i] = h[i].x2[j];
        }
        py[i] = h[i].y;
        ppr[i] = h[i].proposed != 0;
        pin[i] = h[i].init != 0;
    }
    mxSetField(out, 0, "x", x);
    mxSetField(out, 0, "y", y);
    mxSetField(out, 0, "proposed", pr);
    mxSetField(out, 0, "init", in);
    if (pair) mxSetField(out, 0, "x2", x2);
    return out;
}

static mxArray* g_policy_string(yrt_policy pol) {
    const char* name = yrt_policy_name(pol);
    char buf[32];
    size_t i = 0;
    for (; name[i] != 0 && i + 1 < sizeof(buf); i++)
        buf[i] = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] - 'A' + 'a') : name[i];
    buf[i] = 0;
    return mxCreateString(buf);
}

static mxArray* g_snapshot(const yaep_snapshot* s, const yaep_gp* g, int nd) {
    static const char* fields[] = { "seq", "x", "proposed", "update_rc", "n_trials", "done",
                                    "stop", "threshold", "threshold_lo", "threshold_hi",
                                    "threshold_rc", "multi_cross", "hyper", "log_marginal",
                                    "numeric", "fitting" };
    mxArray* out = mxCreateStructMatrix(1, 1, 16, fields);
    int st = (int)s->stop;
    mxSetField(out, 0, "seq", mxCreateDoubleScalar((double)s->seq));
    mxSetField(out, 0, "x", pm_row(s->x, nd));
    /* 1-based candidate, 0 for a Halton, rng or refined point, else the
     * negative code of a refused selection. */
    mxSetField(out, 0, "proposed", mxCreateDoubleScalar(s->proposed >= -1 ? s->proposed + 1.0 : (double)s->proposed));
    mxSetField(out, 0, "update_rc", mxCreateDoubleScalar((double)s->update_rc));
    mxSetField(out, 0, "n_trials", mxCreateDoubleScalar((double)s->n_trials));
    mxSetField(out, 0, "done", mxCreateLogicalScalar(s->done));
    mxSetField(out, 0, "stop", mxCreateString(st >= 0 && st < 4 ? g_stop_names[st] : "none"));
    mxSetField(out, 0, "threshold", mxCreateDoubleScalar(s->threshold));
    mxSetField(out, 0, "threshold_lo", mxCreateDoubleScalar(s->threshold_lo));
    mxSetField(out, 0, "threshold_hi", mxCreateDoubleScalar(s->threshold_hi));
    mxSetField(out, 0, "threshold_rc", mxCreateDoubleScalar((double)s->threshold_rc));
    mxSetField(out, 0, "multi_cross", mxCreateLogicalScalar(s->multi_cross));
    mxSetField(out, 0, "hyper", g_hyper_out(&s->hyper, nd, g_n_cut(g)));
    mxSetField(out, 0, "log_marginal", mxCreateDoubleScalar(s->log_marginal));
    mxSetField(out, 0, "numeric", mxCreateDoubleScalar((double)s->numeric));
    mxSetField(out, 0, "fitting", mxCreateLogicalScalar(s->fitting));
    return out;
}

/* ---- argument helpers --------------------------------------------------- */

static void g_point(gn* n, const mxArray* a, const char* what, double* x) {
    pm_vector_n(a, what, x, n->nd);
}

/* An n x n_dims matrix of points, transposed into the node's scratch. */
static const double* g_points(gn* n, const mxArray* a, int* count) {
    size_t m, need, i, j;
    const double* src;
    if (!mxIsDouble(a) || mxIsComplex(a) || (int)mxGetN(a) != n->nd)
        pm_err("arg", "points must be a real n x %d double matrix", n->nd);
    m = mxGetM(a);
    if (m > 0x7fffffff) pm_err("arg", "too many points");
    need = m * (size_t)n->nd;
    if (need > n->scratch_cap) {
        double* p = (double*)realloc(n->scratch, need * sizeof(double));
        if (!p) pm_err("memory", "out of memory");
        n->scratch = p;
        n->scratch_cap = need;
    }
    src = mxGetPr(a);
    for (i = 0; i < m; i++)
        for (j = 0; j < (size_t)n->nd; j++) n->scratch[i * n->nd + j] = src[j * m + i];
    *count = (int)m;
    return n->scratch;
}

static int g_latent_arg(gn* n, int nrhs, const mxArray* prhs[], int at) {
    int K = n->g.desc.model == YAEP_MODEL_PSYCHOMETRIC ? 2 : (n->g.K > 0 ? n->g.K : 1);
    return nrhs > at ? pm_index(prhs[at], "latent", K) : 0;
}

static void g_async_cmd(const char* cmd, int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[], gn* n);

/* An update's code: NUMERIC means the trial was recorded and the previous
 * posterior stays in force, which is not a failure of the call. With an output
 * requested it is returned; without one it is raised, so it cannot pass
 * unnoticed. */
static void g_update_rc(int nlhs, mxArray* plhs[], int rc) {
    if (rc == YAEP_ERR_NUMERIC && nlhs >= 1) { plhs[0] = mxCreateDoubleScalar((double)rc); return; }
    g_check(rc);
    if (nlhs >= 1) plhs[0] = mxCreateDoubleScalar((double)rc);
}

/* ---- dispatch ----------------------------------------------------------- */

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char* cmd;
    gn* n;
    yaep_gp* g;
    pm_destroy = gn_destroy;
    pm_pending = NULL;
    if (nrhs < 1 || !mxIsChar(prhs[0]))
        pm_err("usage", "first argument must be a command string");
    cmd = mxArrayToString(prhs[0]);

    /* ---- module commands ---- */
    if (strcmp(cmd, "open") == 0) {
        pm_nargs(nrhs, 2, "h = ysp_aep('open', desc)");
        n = g_make(prhs[1], NULL, 0);
        plhs[0] = pm_handle_out(pm_register(n));
        return;
    }
    if (strcmp(cmd, "load") == 0) {
        size_t len;
        const uint8_t* b;
        pm_nargs(nrhs, 3, "h = ysp_aep('load', bytes, desc)");
        b = pm_bytes_in(prhs[1], "bytes", &len);
        n = g_make(prhs[2], b, len);
        plhs[0] = pm_handle_out(pm_register(n));
        return;
    }
    if (strcmp(cmd, "version") == 0) { plhs[0] = mxCreateString(yaep_version()); return; }
    if (strcmp(cmd, "strerror") == 0) {
        pm_nargs(nrhs, 2, "ysp_aep('strerror', code)");
        plhs[0] = mxCreateString(yaep_strerror(pm_int(prhs[1], "code")));
        return;
    }
    if (strcmp(cmd, "simulate_outcome") == 0) {
        double p[YAEP_MAX_OUTCOMES];
        int K, rc;
        pm_nargs(nrhs, 3, "k = ysp_aep('simulate_outcome', p, u)");
        K = pm_vector(prhs[1], "p", p, YAEP_MAX_OUTCOMES);
        rc = yaep_simulate_outcome(p, K, pm_scalar(prhs[2], "u"));
        g_check(rc);
        plhs[0] = mxCreateDoubleScalar((double)rc);
        return;
    }
    if (strcmp(cmd, "memory_size") == 0) {
        yaep_desc d;
        const mxArray* cand;
        memset(&d, 0, sizeof(d));
        pm_nargs(nrhs, 2, "bytes = ysp_aep('memory_size', desc)");
        cand = g_read_desc(prhs[1], &d);
        if (cand) d.candidates = mxGetPr(cand);   /* only its presence matters here */
        if (pm_field(prhs[1], "rng")) d.rng = pm_rng_call;
        plhs[0] = mxCreateDoubleScalar((double)yaep_memory_size(&d));
        return;
    }

    /* ---- handle commands ---- */
    if (nrhs < 2) pm_err("usage", "'%s' needs a handle", cmd);
    n = (gn*)pm_lookup(prhs[1]);
    g = &n->g;

    if (strcmp(cmd, "close") == 0) {
        if (n->busy) pm_err("busy", "a callback may not close the GP it runs under");
        pm_unregister(n);
        gn_destroy(n);
        return;
    }
    if (strncmp(cmd, "async_", 6) == 0) {
        g_async_cmd(cmd, nlhs, plhs, nrhs, prhs, n);
        return;
    }
    if (n->busy) pm_err("busy", "a callback may not call into the GP it runs under");
    if (n->async_running)
        pm_err("busy", "the GP is owned by a running async session; call async_stop first");

    if (strcmp(cmd, "next") == 0 || strcmp(cmd, "next_subset") == 0) {
        double x[YAEP_MAX_DIMS];
        int rc;
        if (cmd[4] == 0) {
            n->busy = 1;
            rc = yaep_next(g, x);
            n->busy = 0;
        } else {
            int m, i, M = yaep_n_candidates(g);
            double* v;
            int* sub;
            pm_nargs(nrhs, 3, "[x, i] = ysp_aep('next_subset', h, indices)");
            m = (int)mxGetNumberOfElements(prhs[2]);
            if (m < 1) pm_err("arg", "next_subset needs at least one index");
            v = (double*)mxMalloc((size_t)m * sizeof(double));
            sub = (int*)mxMalloc((size_t)m * sizeof(int));
            pm_vector(prhs[2], "indices", v, m);
            for (i = 0; i < m; i++) {
                if (v[i] != floor(v[i]) || v[i] < 1 || v[i] > M)
                    pm_err("arg", "indices must be integers in 1..%d", M);
                sub[i] = (int)v[i] - 1;
            }
            n->busy = 1;
            rc = yaep_next_subset(g, sub, m, x);
            n->busy = 0;
        }
        pm_rethrow();
        /* -1 is a Halton, rng or refined point; errors are below it. */
        if (rc < -1) g_check(rc == -1 ? YAEP_ERR_CLOSED : rc);
        plhs[0] = pm_row(x, n->nd);
        if (nlhs >= 2) plhs[1] = mxCreateDoubleScalar(rc + 1.0);
    } else if (strcmp(cmd, "acq_score") == 0) {
        int M = yaep_n_candidates(g), m, i;
        mxArray* out;
        double* o;
        if (nrhs >= 3) {
            m = (int)mxGetNumberOfElements(prhs[2]);
            out = mxCreateDoubleMatrix((size_t)m, 1, mxREAL);
            o = mxGetPr(out);
            pm_vector(prhs[2], "indices", o, m);
            for (i = 0; i < m; i++) {
                if (o[i] != floor(o[i]) || o[i] < 1 || o[i] > M)
                    pm_err("arg", "indices must be integers in 1..%d", M);
                o[i] = yaep_acq_score(g, (int)o[i] - 1);
            }
        } else {
            out = mxCreateDoubleMatrix((size_t)M, 1, mxREAL);
            o = mxGetPr(out);
            for (i = 0; i < M; i++) o[i] = yaep_acq_score(g, i);
        }
        plhs[0] = out;
    } else if (strcmp(cmd, "update") == 0 || strcmp(cmd, "update_real") == 0) {
        double x[YAEP_MAX_DIMS];
        int rc;
        pm_nargs(nrhs, 4, "ysp_aep('update', h, x, outcome)");
        g_point(n, prhs[2], "x", x);
        n->busy = 1;
        if (cmd[6] == 0) {
            int k = pm_int(prhs[3], "outcome");
            rc = yaep_update(g, x, k);
        } else {
            double y = pm_scalar(prhs[3], "y");
            rc = yaep_update_real(g, x, y);
        }
        n->busy = 0;
        pm_rethrow();
        g_update_rc(nlhs, plhs, rc);
    } else if (strcmp(cmd, "next_pair") == 0) {
        double x1[YAEP_MAX_DIMS], x2[YAEP_MAX_DIMS];
        int rc;
        n->busy = 1;
        rc = yaep_next_pair(g, x1, x2);
        n->busy = 0;
        pm_rethrow();
        g_check(rc);
        plhs[0] = pm_row(x1, n->nd);
        if (nlhs >= 2) plhs[1] = pm_row(x2, n->nd);
    } else if (strcmp(cmd, "update_pair") == 0) {
        double x1[YAEP_MAX_DIMS], x2[YAEP_MAX_DIMS];
        int rc, k;
        pm_nargs(nrhs, 5, "ysp_aep('update_pair', h, x1, x2, outcome)");
        g_point(n, prhs[2], "x1", x1);
        g_point(n, prhs[3], "x2", x2);
        k = pm_int(prhs[4], "outcome");
        n->busy = 1;
        rc = yaep_update_pair(g, x1, x2, k);
        n->busy = 0;
        pm_rethrow();
        g_update_rc(nlhs, plhs, rc);
    } else if (strcmp(cmd, "predict_pair") == 0) {
        double x1[YAEP_MAX_DIMS], x2[YAEP_MAX_DIMS];
        pm_nargs(nrhs, 4, "p = ysp_aep('predict_pair', h, x1, x2)");
        g_point(n, prhs[2], "x1", x1);
        g_point(n, prhs[3], "x2", x2);
        plhs[0] = mxCreateDoubleScalar(yaep_predict_pair(g, x1, x2));
    } else if (strcmp(cmd, "done") == 0) {
        plhs[0] = mxCreateLogicalScalar(yaep_done(g));
    } else if (strcmp(cmd, "stop_reason") == 0) {
        int r = (int)yaep_stop_reason(g);
        plhs[0] = mxCreateString(r >= 0 && r < 4 ? g_stop_names[r] : "none");
    } else if (strcmp(cmd, "predict_f") == 0) {
        double x[YAEP_MAX_DIMS], mu = 0, sd = 0;
        pm_nargs(nrhs, 3, "[mu, sd] = ysp_aep('predict_f', h, x [, latent])");
        g_point(n, prhs[2], "x", x);
        g_check(yaep_predict_f(g, x, g_latent_arg(n, nrhs, prhs, 3), &mu, &sd));
        plhs[0] = mxCreateDoubleScalar(mu);
        if (nlhs >= 2) plhs[1] = mxCreateDoubleScalar(sd);
    } else if (strcmp(cmd, "predict_p") == 0 || strcmp(cmd, "predict_p_var") == 0) {
        double x[YAEP_MAX_DIMS];
        pm_nargs(nrhs, 3, "p = ysp_aep('predict_p', h, x)");
        g_point(n, prhs[2], "x", x);
        plhs[0] = mxCreateDoubleScalar(cmd[9] == 0 ? yaep_predict_p(g, x) : yaep_predict_p_var(g, x));
    } else if (strcmp(cmd, "predict_outcomes") == 0) {
        double x[YAEP_MAX_DIMS], p[YAEP_MAX_OUTCOMES];
        int K = g->desc.lik == YAEP_LIK_ORDINAL || g->desc.lik == YAEP_LIK_CATEGORICAL ? g->desc.n_outcomes : 2;
        pm_nargs(nrhs, 3, "p = ysp_aep('predict_outcomes', h, x)");
        g_point(n, prhs[2], "x", x);
        g_check(yaep_predict_outcomes(g, x, p));
        plhs[0] = pm_row(p, K);
    } else if (strcmp(cmd, "predict_p_many") == 0) {
        int m;
        const double* xs;
        mxArray* out;
        pm_nargs(nrhs, 3, "p = ysp_aep('predict_p_many', h, X)");
        xs = g_points(n, prhs[2], &m);
        out = mxCreateDoubleMatrix((size_t)m, 1, mxREAL);
        if (m > 0) g_check(yaep_predict_p_many(g, xs, m, mxGetPr(out)));
        plhs[0] = out;
    } else if (strcmp(cmd, "predict_f_many") == 0) {
        int m, k;
        const double* xs;
        mxArray *mu, *sd;
        pm_nargs(nrhs, 3, "[mu, sd] = ysp_aep('predict_f_many', h, X [, latent])");
        k = g_latent_arg(n, nrhs, prhs, 3);
        xs = g_points(n, prhs[2], &m);
        mu = mxCreateDoubleMatrix((size_t)m, 1, mxREAL);
        sd = mxCreateDoubleMatrix((size_t)m, 1, mxREAL);
        if (m > 0) g_check(yaep_predict_f_many(g, xs, m, k, mxGetPr(mu), mxGetPr(sd)));
        plhs[0] = mu;
        if (nlhs >= 2) plhs[1] = sd; else mxDestroyArray(sd);
    } else if (strcmp(cmd, "threshold") == 0) {
        double ctx[YAEP_MAX_DIMS], x = 0, lo = 0, hi = 0, target = 0;
        const double* cp = NULL;
        if (n->nd > 1) {
            pm_nargs(nrhs, 3, "[x, lo, hi] = ysp_aep('threshold', h, ctx [, target])");
            pm_vector_n(prhs[2], "ctx", ctx, n->nd - 1);
            cp = ctx;
        }
        if (nrhs >= 4) target = pm_scalar(prhs[3], "target");
        g_check(yaep_threshold(g, cp, target, &x, &lo, &hi));
        plhs[0] = mxCreateDoubleScalar(x);
        if (nlhs >= 2) plhs[1] = mxCreateDoubleScalar(lo);
        if (nlhs >= 3) plhs[2] = mxCreateDoubleScalar(hi);
    } else if (strcmp(cmd, "threshold_multi_cross") == 0) {
        plhs[0] = mxCreateLogicalScalar(yaep_threshold_multi_cross(g));
    } else if (strcmp(cmd, "argmax") == 0) {
        double x[YAEP_MAX_DIMS], v = 0;
        int rc;
        n->busy = 1;
        rc = yaep_argmax(g, x, &v);
        n->busy = 0;
        pm_rethrow();
        if (rc < -1) g_check(rc);
        plhs[0] = pm_row(x, n->nd);
        if (nlhs >= 2) plhs[1] = mxCreateDoubleScalar(v);
        if (nlhs >= 3) plhs[2] = mxCreateDoubleScalar(rc + 1.0);
    } else if (strcmp(cmd, "fit") == 0 || strcmp(cmd, "fit_step") == 0 || strcmp(cmd, "refit") == 0) {
        int rc;
        n->busy = 1;
        rc = cmd[0] == 'r' ? yaep_refit(g) : (cmd[3] == 0 ? yaep_fit(g) : yaep_fit_step(g));
        n->busy = 0;
        pm_rethrow();
        g_check(rc);
        if (cmd[0] == 'r') { if (nlhs >= 1) plhs[0] = mxCreateDoubleScalar((double)rc); }
        else plhs[0] = mxCreateLogicalScalar(rc != 0);
    } else if (strcmp(cmd, "fit_delta") == 0) {
        plhs[0] = mxCreateDoubleScalar(yaep_fit_delta(g));
    } else if (strcmp(cmd, "get_hyper") == 0) {
        yaep_hyper h;
        g_check(yaep_get_hyper(g, &h));
        plhs[0] = g_hyper_out(&h, n->nd, g_n_cut(g));
    } else if (strcmp(cmd, "get_priors") == 0) {
        yaep_priors p;
        g_check(yaep_get_priors(g, &p));
        plhs[0] = g_priors_out(&p);
    } else if (strcmp(cmd, "log_marginal") == 0) {
        plhs[0] = mxCreateDoubleScalar(yaep_log_marginal(g));
    } else if (strcmp(cmd, "n_candidates") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)yaep_n_candidates(g));
    } else if (strcmp(cmd, "candidate") == 0) {
        int M = yaep_n_candidates(g), i, j;
        double x[YAEP_MAX_DIMS];
        if (nrhs >= 3) {
            g_check(yaep_candidate(g, pm_index(prhs[2], "candidate", M), x));
            plhs[0] = pm_row(x, n->nd);
        } else {
            /* Every candidate, one per row. */
            mxArray* out = mxCreateDoubleMatrix((size_t)M, (size_t)n->nd, mxREAL);
            double* o = mxGetPr(out);
            for (i = 0; i < M; i++) {
                g_check(yaep_candidate(g, i, x));
                for (j = 0; j < n->nd; j++) o[(size_t)j * M + i] = x[j];
            }
            plhs[0] = out;
        }
    } else if (strcmp(cmd, "n_trials") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)yaep_n_trials(g));
    } else if (strcmp(cmd, "n_dims") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)n->nd);
    } else if (strcmp(cmd, "history") == 0) {
        plhs[0] = g_history(g, n->nd);
    } else if (strcmp(cmd, "save") == 0) {
        size_t sz = yaep_save_size(g);
        mxArray* out = mxCreateNumericMatrix(1, sz, mxUINT8_CLASS, mxREAL);
        g_check(yaep_save(g, mxGetData(out), sz));
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

static const char* const ga_fields[] = { "context", "target", "fit_in_idle", "below_normal", "pin_cpu" };

static void g_async_cmd(const char* cmd, int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[], gn* n) {
    yaep_snapshot snap;
    (void)nlhs;
    if (n->busy) pm_err("busy", "a callback may not call into the GP it runs under");
    if (strcmp(cmd, "async_start") == 0) {
        yaep_async_desc ad;
        const mxArray* f;
        if (n->async_running) pm_err("busy", "async session already running");
        if (n->g.desc.lik == YAEP_LIK_PAIRWISE)
            pm_err("arg", "async_start refuses a pairwise GP: the async layer submits single stimuli");
        if (n->rng.fn)
            pm_err("arg", "async_start refuses a GP whose rng is a MATLAB function; use an integer seed");
        memset(&ad, 0, sizeof(ad));
        if (nrhs >= 3 && !mxIsEmpty(prhs[2])) {
            pm_check_fields(prhs[2], ga_fields, 5, "async opts");
            if ((f = pm_field(prhs[2], "context")))      pm_vector_n(f, "context", ad.context, n->nd - 1);
            if ((f = pm_field(prhs[2], "target")))       ad.target = pm_scalar(f, "target");
            if ((f = pm_field(prhs[2], "fit_in_idle")))  ad.fit_in_idle = pm_bool(f, "fit_in_idle") != 0;
            if ((f = pm_field(prhs[2], "below_normal"))) ad.below_normal = pm_bool(f, "below_normal") != 0;
            if ((f = pm_field(prhs[2], "pin_cpu")))      ad.pin_cpu = pm_int(f, "pin_cpu");
        }
        ad.gp = &n->g;
        memset(&n->a, 0, sizeof(n->a));
        if (!yaep_async_open(&n->a, &ad))
            pm_err("async", "%s", yaep_async_error(&n->a));
        n->async_running = 1;
        return;
    }
    if (strcmp(cmd, "async_stop") == 0) {
        if (n->async_running) { yaep_async_close(&n->a); n->async_running = 0; }
        return;
    }
    if (strcmp(cmd, "async_policy") == 0) {
        plhs[0] = g_policy_string(yaep_async_policy(&n->a));
        return;
    }
    if (strcmp(cmd, "async_running") == 0) {
        plhs[0] = mxCreateLogicalScalar(n->async_running && yaep_async_is_running(&n->a));
        return;
    }
    if (!n->async_running) pm_err("closed", "no async session is running; call async_start");
    if (strcmp(cmd, "async_submit") == 0 || strcmp(cmd, "async_submit_real") == 0) {
        double x[YAEP_MAX_DIMS];
        int rc;
        pm_nargs(nrhs, 4, "seq = ysp_aep('async_submit', h, x, outcome)");
        g_point(n, prhs[2], "x", x);
        if (cmd[12] == 0) rc = yaep_async_submit(&n->a, x, pm_int(prhs[3], "outcome"));
        else rc = yaep_async_submit_real(&n->a, x, pm_scalar(prhs[3], "y"));
        g_check(rc);
        plhs[0] = mxCreateDoubleScalar((double)rc);
    } else if (strcmp(cmd, "async_poll") == 0) {
        g_check(yaep_async_poll(&n->a, &snap));
        plhs[0] = g_snapshot(&snap, &n->g, n->nd);
    } else if (strcmp(cmd, "async_wait") == 0) {
        int rc, seq;
        double t;
        pm_nargs(nrhs, 3, "snap = ysp_aep('async_wait', h, seq [, timeout_s])");
        seq = pm_int(prhs[2], "seq");
        if (seq < 0) pm_err("arg", "seq must be >= 0");
        t = nrhs > 3 ? pm_scalar(prhs[3], "timeout_s") : 1.0;
        if (!(t >= 0) || t > 1e6) pm_err("arg", "timeout_s must be in [0, 1e6] seconds");
        rc = yaep_async_wait(&n->a, (uint32_t)seq, (int64_t)(t * 1e9), &snap);
        g_check(rc);
        plhs[0] = g_snapshot(&snap, &n->g, n->nd);
    } else if (strcmp(cmd, "async_pending") == 0) {
        int rc = yaep_async_pending(&n->a);
        g_check(rc);
        plhs[0] = mxCreateDoubleScalar((double)rc);
    } else {
        pm_err("usage", "unknown command '%s'", cmd);
    }
}
