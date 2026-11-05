/* ysp_stair.c - MATLAB/Octave MEX binding for ysp/stair.h (builds to ysp_stair.<mexext>)
 *
 * Command dispatch in the style of widmann's ppdev-mex: the first argument is
 * a command string, and 'open' returns an opaque uint64 handle.
 *
 *     h = ysp_stair('open', struct('start', 0.5, 'n_down', 3, ...
 *                   'step_type', 'log', 'steps', [0.3 0.15 0.075], ...
 *                   'min', 0.001, 'max', 1, 'stop_reversals', 10));
 *     while ~ysp_stair('done', h)
 *         x = ysp_stair('next', h);
 *         ysp_stair('update', h, x, run_trial(x));
 *     end
 *     thr = ysp_stair('estimate', h, 'reversals');
 *     b = ysp_stair('save', h);             % uint8 snapshot
 *     ysp_stair('close', h);
 *     h = ysp_stair('load', b, desc);       % resumes, no replay
 *
 * Build with build.m. See README.md for the command reference.
 *
 * The handle is about 28 KB and is malloc'd once at open; nothing else
 * allocates except the mxArrays a command returns.
 */
#define PM_MOD "ysp_stair"
#include "ysp_mex_util.h"

#define YSP_STAIR_IMPLEMENTATION
#include "ysp/stair.h"

static void st_destroy(void* obj) { free(obj); }

static const char* const st_rule_names[] = { "updown", "asa" };
static const char* const st_step_names[] = { "lin", "log", "db" };
static const char* const st_est_names[]  = { "reversals", "trials", "median_rev", "last" };
static const char* const st_stop_names[] = { "none", "reversals", "trials", "limit", "full" };

static void st_check(int rc) {
    if (rc >= 0) return;
    switch (rc) {
        case YST_ERR_ARG:    pm_err("arg", "%s", yst_strerror(rc)); break;
        case YST_ERR_CLOSED: pm_err("closed", "%s", yst_strerror(rc)); break;
        case YST_ERR_FULL:   pm_err("full", "%s", yst_strerror(rc)); break;
        default:               pm_err("error", "%s", yst_strerror(rc)); break;
    }
}

static const char* const st_fields[] = {
    "start", "rule", "n_up", "n_down", "step_type", "steps", "step_down_scale",
    "target_p", "min", "max", "use_limits", "harder_is_up", "initial_rule",
    "stop_reversals", "stop_trials", "stop_at_limit", "est_reversals", "est_trials"
};

static void st_read_desc(const mxArray* s, yst_desc* d) {
    const mxArray* f;
    pm_check_fields(s, st_fields, (int)(sizeof(st_fields) / sizeof(st_fields[0])), "desc");
    if ((f = pm_field(s, "start")))           d->start = pm_scalar(f, "start");
    if ((f = pm_field(s, "rule")))            d->rule = (yst_rule)pm_enum(f, "rule", st_rule_names, 2);
    if ((f = pm_field(s, "n_up")))            d->n_up = pm_int(f, "n_up");
    if ((f = pm_field(s, "n_down")))          d->n_down = pm_int(f, "n_down");
    if ((f = pm_field(s, "step_type")))       d->step_type = (yst_step_type)pm_enum(f, "step_type", st_step_names, 3);
    if ((f = pm_field(s, "steps")))           d->n_steps = pm_vector(f, "steps", d->steps, YST_MAX_STEPS);
    if ((f = pm_field(s, "step_down_scale"))) d->step_down_scale = pm_scalar(f, "step_down_scale");
    if ((f = pm_field(s, "target_p")))        d->target_p = pm_scalar(f, "target_p");
    if ((f = pm_field(s, "min")))             d->min = pm_scalar(f, "min");
    if ((f = pm_field(s, "max")))             d->max = pm_scalar(f, "max");
    if ((f = pm_field(s, "use_limits")))      d->use_limits = pm_bool(f, "use_limits");
    if ((f = pm_field(s, "harder_is_up")))    d->harder_is_up = pm_bool(f, "harder_is_up");
    if ((f = pm_field(s, "initial_rule")))    d->initial_rule = pm_bool(f, "initial_rule");
    if ((f = pm_field(s, "stop_reversals")))  d->stop_reversals = pm_int(f, "stop_reversals");
    if ((f = pm_field(s, "stop_trials")))     d->stop_trials = pm_int(f, "stop_trials");
    if ((f = pm_field(s, "stop_at_limit")))   d->stop_at_limit = pm_int(f, "stop_at_limit");
    if ((f = pm_field(s, "est_reversals")))   d->est_reversals = pm_int(f, "est_reversals");
    if ((f = pm_field(s, "est_trials")))      d->est_trials = pm_int(f, "est_trials");
}

static yst_estimator st_how(int nrhs, const mxArray* prhs[], int at) {
    if (nrhs <= at) return YST_EST_REVERSALS;
    return (yst_estimator)pm_enum(prhs[at], "estimator", st_est_names, 4);
}

/* History as a struct of column vectors, one field per yst_trial member,
 * which struct2table turns into a table directly. */
static mxArray* st_history(const yst_stair* s) {
    static const char* fields[] = { "proposed", "shown", "response", "reversal",
                                    "direction", "step_index" };
    int n = 0, i;
    const yst_trial* h = yst_history(s, &n);
    mxArray* out = mxCreateStructMatrix(1, 1, 6, fields);
    mxArray* c[6];
    double* p[6];
    for (i = 0; i < 6; i++) {
        c[i] = mxCreateDoubleMatrix((size_t)n, 1, mxREAL);
        p[i] = mxGetPr(c[i]);
    }
    for (i = 0; i < n; i++) {
        p[0][i] = h[i].proposed;
        p[1][i] = h[i].shown;
        p[2][i] = h[i].response;
        p[3][i] = h[i].reversal;
        p[4][i] = h[i].direction;
        p[5][i] = h[i].step_index + 1;   /* 1-based into desc.steps */
    }
    for (i = 0; i < 6; i++) mxSetField(out, 0, fields[i], c[i]);
    return out;
}

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char* cmd;
    yst_stair* s;
    (void)nlhs;
    pm_destroy = st_destroy;
    if (nrhs < 1 || !mxIsChar(prhs[0]))
        pm_err("usage", "first argument must be a command string");
    cmd = mxArrayToString(prhs[0]);

    /* ---- module commands: no handle ---- */
    if (strcmp(cmd, "open") == 0 || strcmp(cmd, "load") == 0) {
        yst_desc d;
        const uint8_t* bytes = NULL;
        size_t len = 0;
        bool load = cmd[0] == 'l', ok;
        memset(&d, 0, sizeof(d));
        if (load) {
            pm_nargs(nrhs, 3, "h = ysp_stair('load', bytes, desc)");
            bytes = pm_bytes_in(prhs[1], "bytes", &len);
        } else {
            pm_nargs(nrhs, 2, "h = ysp_stair('open', desc)");
        }
        /* Parse first: a parse error raises, and nothing malloc'd may be live. */
        st_read_desc(prhs[load ? 2 : 1], &d);
        s = (yst_stair*)calloc(1, sizeof(*s));
        if (!s) pm_err("memory", "out of memory");
        ok = load ? yst_open_saved(s, &d, bytes, len) : yst_open(s, &d);
        if (!ok) {
            char msg[300];
            snprintf(msg, sizeof(msg), "%s", yst_error(s));
            free(s);
            pm_err(load ? "load" : "open", "%s", msg);
        }
        plhs[0] = pm_handle_out(pm_register(s));
        return;
    }
    if (strcmp(cmd, "version") == 0) { plhs[0] = mxCreateString(yst_version()); return; }
    if (strcmp(cmd, "weighted_scale") == 0) {
        pm_nargs(nrhs, 2, "ysp_stair('weighted_scale', p)");
        plhs[0] = mxCreateDoubleScalar(yst_weighted_scale(pm_scalar(prhs[1], "p")));
        return;
    }
    if (strcmp(cmd, "convergence_p") == 0) {
        pm_nargs(nrhs, 3, "ysp_stair('convergence_p', n_up, n_down [, scale])");
        plhs[0] = mxCreateDoubleScalar(yst_convergence_p(pm_int(prhs[1], "n_up"),
            pm_int(prhs[2], "n_down"), nrhs > 3 ? pm_scalar(prhs[3], "scale") : 0.0));
        return;
    }
    if (strcmp(cmd, "simulate_response") == 0) {
        pm_nargs(nrhs, 3, "ysp_stair('simulate_response', p_correct, u)");
        plhs[0] = mxCreateDoubleScalar((double)yst_simulate_response(
            pm_scalar(prhs[1], "p_correct"), pm_scalar(prhs[2], "u")));
        return;
    }
    if (strcmp(cmd, "strerror") == 0) {
        pm_nargs(nrhs, 2, "ysp_stair('strerror', code)");
        plhs[0] = mxCreateString(yst_strerror(pm_int(prhs[1], "code")));
        return;
    }

    /* ---- handle commands ---- */
    if (nrhs < 2) pm_err("usage", "'%s' needs a handle", cmd);
    s = (yst_stair*)pm_lookup(prhs[1]);

    if (strcmp(cmd, "next") == 0) {
        plhs[0] = mxCreateDoubleScalar(yst_next(s));
    } else if (strcmp(cmd, "update") == 0) {
        int r, rc;
        pm_nargs(nrhs, 4, "ysp_stair('update', h, level, response)");
        r = pm_int(prhs[3], "response");
        rc = yst_update(s, pm_scalar(prhs[2], "level"), r);
        st_check(rc);
        if (nlhs >= 1) plhs[0] = mxCreateDoubleScalar((double)rc);
    } else if (strcmp(cmd, "done") == 0) {
        plhs[0] = mxCreateLogicalScalar(yst_done(s));
    } else if (strcmp(cmd, "stop_reason") == 0) {
        int r = (int)yst_stop_reason(s);
        plhs[0] = mxCreateString(r >= 0 && r < 5 ? st_stop_names[r] : "none");
    } else if (strcmp(cmd, "estimate") == 0) {
        plhs[0] = mxCreateDoubleScalar(yst_estimate(s, st_how(nrhs, prhs, 2)));
    } else if (strcmp(cmd, "estimate_count") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)yst_estimate_count(s, st_how(nrhs, prhs, 2)));
    } else if (strcmp(cmd, "n_trials") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)yst_n_trials(s));
    } else if (strcmp(cmd, "n_reversals") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)yst_n_reversals(s));
    } else if (strcmp(cmd, "history") == 0) {
        plhs[0] = st_history(s);
    } else if (strcmp(cmd, "reversal_level") == 0 || strcmp(cmd, "reversal_trial") == 0) {
        int n = yst_n_reversals(s), rec = n < YST_MAX_REVERSALS ? n : YST_MAX_REVERSALS, i;
        int want_level = cmd[9] == 'l';
        if (nrhs >= 3) {
            /* One reversal, 1-based. */
            i = pm_index(prhs[2], "reversal", rec > 0 ? rec : 1);
            if (i >= rec) pm_err("arg", "no reversal recorded yet");
            plhs[0] = mxCreateDoubleScalar(want_level ? yst_reversal_level(s, i)
                                                      : (double)(yst_reversal_trial(s, i) + 1));
        } else {
            /* Every recorded reversal as a column. */
            mxArray* a = mxCreateDoubleMatrix((size_t)rec, 1, mxREAL);
            double* p = mxGetPr(a);
            for (i = 0; i < rec; i++)
                p[i] = want_level ? yst_reversal_level(s, i) : (double)(yst_reversal_trial(s, i) + 1);
            plhs[0] = a;
        }
    } else if (strcmp(cmd, "save") == 0) {
        size_t sz = yst_save_size(s);
        mxArray* out = mxCreateNumericMatrix(1, sz, mxUINT8_CLASS, mxREAL);
        st_check(yst_save(s, mxGetData(out), sz));
        plhs[0] = out;
    } else if (strcmp(cmd, "close") == 0) {
        pm_unregister(s);
        free(s);
    } else {
        pm_err("usage", "unknown command '%s'", cmd);
    }
}
