/* ysp_device.c - MATLAB/Octave MEX binding for ysp/device.h (builds to ysp_device.<mexext>)
 *
 * Command dispatch: the first argument is a command string; 'open',
 * 'rule_open' and 'sim_open' return uint64 counter handles, never reused.
 *
 *     h = ysp_device('open', struct('role', 'resp', 'family', 'line', 'key', 'serial:16C0:0483::'));
 *     if ~ysp_device('wait_running', h, 3), error(ysp_device('error', h)); end
 *     ev = ysp_device('wait', h, 1.0);        % struct array of events, t in s
 *     t  = ysp_device('open', ysp_rigfile('desc', p, 'trig'));   % a role from the rig profile
 *     ysp_device('wait_running', t, 3);
 *     ysp_device('pulse', t, 12, 0.002);      % 12 for 2 ms, then 0
 *     ysp_device('close', h); ysp_device('close', t);
 *
 * Every time is seconds as a double; the C API takes int64 ns, rounded to
 * the nearest ns here. Inf is "forever" (wait, wait_running) or "never"
 * (probe, silent, a rule window's end). The reader threads never call
 * into MATLAB or Octave: events wait in a C ring (ysp_device_bind.h,
 * shared with the Python binding) that 'poll' and 'wait' empty. A MEX
 * call owns the interpreter, so a 'wait' cannot be interrupted with
 * Ctrl+C: give a finite timeout and loop.
 *
 * Build with build.m; it needs ../python/ysp_device/ysp_device_bind.h.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define PM_MOD "ysp_device"
#include "ysp_mex_util.h"

#if !defined(__APPLE__)
#define YDB_HAVE_PARALLEL 1
#define YSP_PARALLEL_IMPLEMENTATION
#include "ysp/parallel.h"
#endif
#define YSP_DEVICE_IMPLEMENTATION
#include "ysp/device.h"
#include "../python/ysp_device/ysp_device_bind.h"

/* The binding's own version; the second output of 'version' is the header's. */
#define DM_BINDING_VERSION "1.0.1"
#define DM_QUEUE_DEFAULT 1024u
#define DM_QUEUE_MAX (1u << 20)

enum { DM_DEV = 1, DM_RULE = 2, DM_SIM = 3 };

struct dm_rule;
struct dm_sim;

typedef struct dm_obj {
    int            tag;
    struct dm_obj* next;     /* every live object, for the ordered teardown */
} dm_obj;

typedef struct dm_dev {
    dm_obj          h;
    ydev_device     dev;
    ybd_queue       q;
    int             opened;
    int             rule_refs;   /* open rules that write to it */
    struct dm_rule* rule;
    struct dm_sim*  sim;
#if YDB_HAVE_PARALLEL
    ypar_port       par;
#endif
} dm_dev;

typedef struct dm_rule {
    dm_obj    h;
    ydev_rule r;
    dm_dev*   out;
    int       opened;
    int       users;            /* open devices that feed it */
} dm_rule;

typedef struct dm_sim {
    dm_obj  h;
    ybd_sim s;
    int     users;
} dm_sim;

static dm_obj*    dm_all = NULL;
static ydev_roles dm_roles;
static int        dm_tearing = 0;

static void dm_link(dm_obj* o) { o->next = dm_all; dm_all = o; }
static void dm_unlink(dm_obj* o) {
    dm_obj** pp = &dm_all;
    while (*pp) { if (*pp == o) { *pp = o->next; return; } pp = &(*pp)->next; }
}

static void dm_dev_close(dm_dev* d) {
    if (!d->opened) return;
    ydev_close(&d->dev);
    ybd_queue_close(&d->q);
    d->opened = 0;
    if (d->rule) { d->rule->users--; d->rule = NULL; }
    if (d->sim) { d->sim->users--; d->sim = NULL; }
}

static void dm_rule_close(dm_rule* r) {
    if (!r->opened) return;
    ydev_rule_close(&r->r);
    r->opened = 0;
    if (r->out) { r->out->rule_refs--; r->out = NULL; }
}

/* 'clear mex': close in the header's order (inputs with rules, rules,
 * everything else) on the first destroy, then free one object a call. */
static void dm_teardown(void) {
    dm_obj* o;
    for (o = dm_all; o; o = o->next)
        if (o->tag == DM_DEV && ((dm_dev*)o)->rule) dm_dev_close((dm_dev*)o);
    for (o = dm_all; o; o = o->next)
        if (o->tag == DM_RULE) dm_rule_close((dm_rule*)o);
    for (o = dm_all; o; o = o->next)
        if (o->tag == DM_DEV) dm_dev_close((dm_dev*)o);
}

static void dm_free(dm_obj* o) {
    dm_unlink(o);
    if (o->tag == DM_DEV) ybd_queue_free(&((dm_dev*)o)->q);
    if (o->tag == DM_SIM) ybd_sim_free(&((dm_sim*)o)->s);
    free(o);
}

static void dm_destroy(void* p) {
    if (!dm_tearing) { dm_tearing = 1; dm_teardown(); }
    dm_free((dm_obj*)p);
    if (!dm_all) dm_tearing = 0;
}

static void* dm_get(const mxArray* a, int tag, const char* what) {
    dm_obj* o = (dm_obj*)pm_lookup(a);
    if (o->tag != tag) pm_err("handle", "the handle is not %s", what);
    return o;
}

/* ---- arguments ---------------------------------------------------------- */

#define DM_INF 1
#define DM_POS 2

static int64_t dm_seconds(const mxArray* a, const char* what, int flags) {
    double s = pm_scalar(a, what), whole;
    int64_t ns;
    if (!(s >= 0)) pm_err("arg", "%s: seconds, >= 0 (not NaN)", what);
    if (s == HUGE_VAL) {
        if (flags & DM_INF) return INT64_MAX;
        pm_err("arg", "%s: seconds, finite", what);
    }
    if (s >= 9223372036.0) pm_err("arg", "%s: seconds, < 9223372036 (int64 ns)", what);
    whole = floor(s);
    ns = (int64_t)whole * 1000000000 + (int64_t)llround((s - whole) * 1e9);
    if (ns == 0 && s > 0 && (flags & DM_POS)) ns = 1;
    return ns;
}

static double dm_s(int64_t ns) { return ns == INT64_MAX ? HUGE_VAL : (double)ns / 1e9; }

static int dm_family(const mxArray* a) {
    int f;
    if (mxIsChar(a)) {
        char* n = pm_string(a, "family");
        for (f = YBOX_XID; f <= YBOX_FAMILY_LAST; f++)
            if (pm_lower_eq(n, ybox_family_name(f))) return f;
        pm_err("arg", "family: unknown name '%s' (xid, line, photo, triggerbox, biosemi, mmbts, lines, parallel)", n);
    }
    f = pm_int(a, "family");
    if (f < YBOX_XID || f > YBOX_FAMILY_LAST) pm_err("arg", "family: %d is not a ysp/box.h family", f);
    return f;
}

static uint32_t dm_u32(const mxArray* a, const char* what) {
    double d = pm_scalar(a, what);
    if (!(d >= 0) || d != floor(d) || d > 4294967295.0) pm_err("arg", "%s: an integer, 0 to 2^32 - 1", what);
    return (uint32_t)d;
}

static void dm_out_fail(const char* what, int rc) {
    const char* id = rc == YDEV_ERR_STATE ? "state" : rc == YDEV_ERR_ARG ? "arg" : rc == YDEV_ERR_FAMILY ? "family"
                   : rc == YDEV_ERR_IO ? "io" : "error";
    pm_err(id, "ysp_device: %s: %s", what, ydev_strerror(rc));
}

static dm_dev* dm_open_dev(const mxArray* a) {
    dm_dev* d = (dm_dev*)dm_get(a, DM_DEV, "a device");
    if (!d->opened) pm_err("closed", "the device is closed");
    return d;
}

/* ---- results -------------------------------------------------------------- */

static mxArray* dm_events(const ybd_queue* q, uint64_t tail, uint64_t n) {
    static const char* f[] = { "t", "t_ns", "device", "control", "code", "mods", "kind", "type", "flags", "stamp",
                               "unc", "x", "y", "value", "aux", "ticks" };
    mxArray* s = mxCreateStructMatrix(1, (size_t)n, 16, f);
    uint64_t i;
    for (i = 0; i < n; i++) {
        const yin_event* e = ybd_queue_at(q, tail + i);
        mxArray* tn = mxCreateNumericMatrix(1, 1, mxINT64_CLASS, mxREAL);
        size_t k = (size_t)i;
        *(int64_t*)mxGetData(tn) = e->t;
        mxSetFieldByNumber(s, k, 0, mxCreateDoubleScalar((double)e->t / 1e9));
        mxSetFieldByNumber(s, k, 1, tn);
        mxSetFieldByNumber(s, k, 2, mxCreateDoubleScalar((double)e->device));
        mxSetFieldByNumber(s, k, 3, mxCreateDoubleScalar((double)e->control));
        mxSetFieldByNumber(s, k, 4, mxCreateDoubleScalar((double)e->code));
        mxSetFieldByNumber(s, k, 5, mxCreateDoubleScalar((double)e->mods));
        mxSetFieldByNumber(s, k, 6, mxCreateDoubleScalar((double)e->kind));
        mxSetFieldByNumber(s, k, 7, mxCreateDoubleScalar((double)e->type));
        mxSetFieldByNumber(s, k, 8, mxCreateDoubleScalar((double)e->flags));
        mxSetFieldByNumber(s, k, 9, mxCreateDoubleScalar((double)e->stamp));
        mxSetFieldByNumber(s, k, 10, mxCreateDoubleScalar((double)e->unc_us * 1e-6));
        mxSetFieldByNumber(s, k, 11, mxCreateDoubleScalar((double)e->x));
        mxSetFieldByNumber(s, k, 12, mxCreateDoubleScalar((double)e->y));
        mxSetFieldByNumber(s, k, 13, mxCreateDoubleScalar((double)e->value));
        mxSetFieldByNumber(s, k, 14, mxCreateDoubleScalar((double)e->aux));
        mxSetFieldByNumber(s, k, 15, mxCreateDoubleScalar((double)e->ticks));
    }
    return s;
}

static mxArray* dm_take(dm_dev* d, uint64_t max) {
    uint64_t tail, n;
    mxArray* s;
    if (ybd_queue_claim(&d->q, max, &tail, &n) < 0) pm_err("busy", "another call is taking the events");
    s = dm_events(&d->q, tail, n);
    ybd_queue_release(&d->q, n);
    return s;
}

static void dm_set(mxArray* s, const char* name, mxArray* v) { mxSetField(s, 0, name, v); }
static mxArray* dm_d(double v) { return mxCreateDoubleScalar(v); }

static mxArray* dm_source(const yin_source* src) {
    static const char* f[] = { "kind", "tier", "partial", "device", "lo", "hi", "note" };
    mxArray* s = mxCreateStructMatrix(1, 1, 7, f);
    dm_set(s, "kind", dm_d(src->kind));
    dm_set(s, "tier", dm_d(src->tier));
    dm_set(s, "partial", dm_d(src->partial));
    dm_set(s, "device", dm_d(src->device));
    dm_set(s, "lo", dm_d(dm_s(src->lo_ns)));
    dm_set(s, "hi", dm_d(dm_s(src->hi_ns)));
    dm_set(s, "note", mxCreateString(src->note ? src->note : ""));
    return s;
}

static mxArray* dm_stats(dm_dev* d) {
    static const char* f[] = { "state", "state_name", "opens", "reads", "bytes", "events", "pairs", "probes",
                               "answers", "refused", "garbage", "clamped", "outs", "out_errors", "reports", "last_rx",
                               "found", "ident", "fit", "queued", "queue_events", "queue_dropped", "queue_max_used",
                               "queue_capacity" };
    static const char* ff[] = { "t0", "ns_per_tick", "ppm", "spread", "width", "span", "points", "pairs", "epoch",
                                "rejected", "ready", "slope" };
    ydev_stats st;
    mxArray *s, *fit;
    ydev_get_stats(&d->dev, &st);
    s = mxCreateStructMatrix(1, 1, 24, f);
    dm_set(s, "state", dm_d(st.state));
    dm_set(s, "state_name", mxCreateString(ydev_state_name(st.state)));
    dm_set(s, "opens", dm_d(st.opens));
    dm_set(s, "reads", dm_d((double)st.reads));
    dm_set(s, "bytes", dm_d((double)st.bytes));
    dm_set(s, "events", dm_d((double)st.events));
    dm_set(s, "pairs", dm_d((double)st.pairs));
    dm_set(s, "probes", dm_d((double)st.probes));
    dm_set(s, "answers", dm_d((double)st.answers));
    dm_set(s, "refused", dm_d((double)st.refused));
    dm_set(s, "garbage", dm_d((double)st.garbage));
    dm_set(s, "clamped", dm_d((double)st.clamped));
    dm_set(s, "outs", dm_d((double)st.outs));
    dm_set(s, "out_errors", dm_d((double)st.out_errors));
    dm_set(s, "reports", dm_d((double)st.reports));
    dm_set(s, "last_rx", dm_d(dm_s(st.last_rx_ns)));
    dm_set(s, "found", mxCreateString(st.found));
    dm_set(s, "ident", mxCreateString(st.ident));
    fit = mxCreateStructMatrix(1, 1, 12, ff);
    dm_set(fit, "t0", dm_d(dm_s(st.fit.t0_ns)));
    dm_set(fit, "ns_per_tick", dm_d(st.fit.ns_per_tick));
    dm_set(fit, "ppm", dm_d(st.fit.ppm));
    dm_set(fit, "spread", dm_d(dm_s(st.fit.spread_ns)));
    dm_set(fit, "width", dm_d(dm_s(st.fit.width_ns)));
    dm_set(fit, "span", dm_d(dm_s(st.fit.span_ns)));
    dm_set(fit, "points", dm_d(st.fit.points));
    dm_set(fit, "pairs", dm_d(st.fit.pairs));
    dm_set(fit, "epoch", dm_d(st.fit.epoch));
    dm_set(fit, "rejected", dm_d(st.fit.rejected));
    dm_set(fit, "ready", mxCreateLogicalScalar(st.fit.ready));
    dm_set(fit, "slope", mxCreateLogicalScalar(st.fit.slope));
    dm_set(s, "fit", fit);
    ybd_lock(&d->q.lock);
    dm_set(s, "queued", dm_d((double)(d->q.head - d->q.tail)));
    dm_set(s, "queue_events", dm_d((double)d->q.events));
    dm_set(s, "queue_dropped", dm_d((double)d->q.dropped));
    dm_set(s, "queue_max_used", dm_d((double)d->q.max_used));
    dm_set(s, "queue_capacity", dm_d((double)d->q.cap));
    ybd_unlock(&d->q.lock);
    return s;
}

static mxArray* dm_info(dm_dev* d) {
    static const char* f[] = { "role", "key", "family", "family_name", "device", "kind", "baud", "pulse", "min_hold",
                               "latched", "is_open" };
    mxArray* s = mxCreateStructMatrix(1, 1, 11, f);
    dm_set(s, "role", mxCreateString(d->dev.role_buf));
    dm_set(s, "key", mxCreateString(d->dev.key_buf));
    dm_set(s, "family", dm_d(d->dev.d.family));
    dm_set(s, "family_name", mxCreateString(ybox_family_name(d->dev.d.family)));
    dm_set(s, "device", dm_d(d->dev.d.device));
    dm_set(s, "kind", dm_d(d->dev.d.kind));
    dm_set(s, "baud", dm_d(d->dev.d.baud));
    dm_set(s, "pulse", dm_d(dm_s(d->dev.d.pulse_ns)));
    dm_set(s, "min_hold", dm_d(dm_s(d->dev.d.min_hold_ns)));
    dm_set(s, "latched", mxCreateLogicalScalar(d->dev.d.latched));
    dm_set(s, "is_open", mxCreateLogicalScalar(d->opened != 0));
    return s;
}

static mxArray* dm_out_info(const ydev_out_info* o) {
    static const char* f[] = { "t_before", "t_after", "width", "code", "seq", "flags", "result" };
    mxArray* s = mxCreateStructMatrix(1, 1, 7, f);
    dm_set(s, "t_before", dm_d(dm_s(o->t_before)));
    dm_set(s, "t_after", dm_d(dm_s(o->t_after)));
    dm_set(s, "width", dm_d(dm_s(o->width_ns)));
    dm_set(s, "code", dm_d(o->code));
    dm_set(s, "seq", dm_d(o->seq));
    dm_set(s, "flags", dm_d(o->flags));
    dm_set(s, "result", dm_d(o->result));
    return s;
}

/* ---- open ------------------------------------------------------------------ */

static void cmd_open(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    static const char* const fields[] = { "role", "family", "key", "device", "kind", "baud", "pulse", "latched",
                                          "min_hold", "probe", "retry", "silent", "max_width", "no_identify",
                                          "no_elevate", "queue", "rule", "sim" };
    const mxArray* s;
    const mxArray* v;
    ydev_desc desc;
    dm_dev* d;
    dm_rule* rule = NULL;
    dm_sim* sim = NULL;
    char *role, *key;
    uint32_t queue = DM_QUEUE_DEFAULT;
    int64_t x;
    (void)nlhs;
    pm_nargs(nrhs, 2, "h = ysp_device('open', struct('role', r, 'family', f, 'key', k, ...))");
    s = prhs[1];
    pm_check_fields(s, fields, (int)(sizeof fields / sizeof fields[0]), "open");
    memset(&desc, 0, sizeof desc);
    if (!(v = pm_field(s, "role"))) pm_err("arg", "open: role is required");
    role = pm_string(v, "role");
    if (!(v = pm_field(s, "family"))) pm_err("arg", "open: family is required");
    desc.family = dm_family(v);
    if (!(v = pm_field(s, "key"))) pm_err("arg", "open: key is required");
    key = pm_string(v, "key");
    desc.role = role;
    desc.key = key;
    if ((v = pm_field(s, "device"))) desc.device = dm_u32(v, "device");
    desc.roles = desc.device ? NULL : &dm_roles;
    if ((v = pm_field(s, "kind"))) desc.kind = pm_int(v, "kind");
    if ((v = pm_field(s, "baud"))) desc.baud = dm_u32(v, "baud");
    if ((v = pm_field(s, "pulse"))) desc.pulse_ns = dm_seconds(v, "pulse", DM_POS);
    if ((v = pm_field(s, "latched"))) desc.latched = pm_bool(v, "latched") != 0;
    if ((v = pm_field(s, "min_hold"))) {
        desc.min_hold_ns = dm_seconds(v, "min_hold", DM_POS);
        if (desc.min_hold_ns == 0) pm_err("arg", "min_hold: > 0 (leave it out for the default)");
    }
    if ((v = pm_field(s, "probe"))) {
        x = dm_seconds(v, "probe", DM_INF | DM_POS);
        if (x == 0) pm_err("arg", "probe: > 0, or Inf for no queries");
        desc.probe_ns = x == INT64_MAX ? -1 : x;
    }
    if ((v = pm_field(s, "silent"))) {
        x = dm_seconds(v, "silent", DM_INF | DM_POS);
        if (x == 0) pm_err("arg", "silent: > 0, or Inf for never LOST");
        desc.silent_ns = x == INT64_MAX ? -1 : x;
    }
    if ((v = pm_field(s, "retry"))) {
        desc.retry_ns = dm_seconds(v, "retry", DM_POS);
        if (desc.retry_ns == 0) pm_err("arg", "retry: > 0");
    }
    if ((v = pm_field(s, "max_width"))) {
        desc.max_width_ns = dm_seconds(v, "max_width", DM_POS);
        if (desc.max_width_ns == 0) pm_err("arg", "max_width: > 0");
    }
    if ((v = pm_field(s, "no_identify"))) desc.no_identify = pm_bool(v, "no_identify") != 0;
    if ((v = pm_field(s, "no_elevate"))) desc.no_elevate = pm_bool(v, "no_elevate") != 0;
    if ((v = pm_field(s, "queue"))) {
        queue = dm_u32(v, "queue");
        if (queue < 1 || queue > DM_QUEUE_MAX) pm_err("arg", "queue: 1 to %u events", DM_QUEUE_MAX);
    }
    if ((v = pm_field(s, "rule"))) {
        rule = (dm_rule*)dm_get(v, DM_RULE, "a rule");
        if (!rule->opened) pm_err("closed", "rule: the rule is closed");
        if (!(ybox_caps(desc.family) & YBOX_CAP_IN)) pm_err("arg", "rule: this family has no inputs");
        desc.rule = &rule->r;
    }
    if ((v = pm_field(s, "sim"))) {
        sim = (dm_sim*)dm_get(v, DM_SIM, "a sim");
        if (sim->s.family != desc.family) pm_err("arg", "sim: the sim is of another family");
    }
    d = (dm_dev*)calloc(1, sizeof *d);
    if (!d) pm_err("memory", "out of memory");
    d->h.tag = DM_DEV;
    if (ybd_queue_init(&d->q, queue) < 0) { free(d); pm_err("memory", "out of memory"); }
    if (sim) desc.transport = ybd_sim_transport(&sim->s);
    else if (desc.family == YBOX_PARALLEL) {
#if YDB_HAVE_PARALLEL
        desc.transport = ydev_parallel_transport(&d->par);
#else
        ybd_queue_free(&d->q);
        free(d);
        pm_err("arg", "family parallel: ysp/parallel.h does not support this platform");
#endif
    }
    desc.sink = ybd_sink;
    desc.sink_ctx = &d->q;
    if (!ydev_open(&d->dev, &desc)) {
        char msg[200];
        snprintf(msg, sizeof msg, "%s", ydev_error(&d->dev));
        ybd_queue_free(&d->q);
        free(d);
        pm_err("open", "%s", msg);
    }
    d->opened = 1;
    if (rule) { d->rule = rule; rule->users++; }
    if (sim) { d->sim = sim; sim->users++; }
    dm_link(&d->h);
    plhs[0] = pm_handle_out(pm_register(d));
}

/* ---- rules and sims ---------------------------------------------------------- */

static void cmd_rule_open(mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    static const char* const ofields[] = { "width", "max_fires" };
    static const char* const cfields[] = { "kind", "device", "control", "code" };
    ydev_rule_choice ch[YDEV_RULE_CHOICES];
    ydev_rule_desc rd;
    dm_dev* out;
    dm_rule* r;
    const mxArray* c;
    size_t n, i;
    pm_nargs(nrhs, 3, "r = ysp_device('rule_open', out, choices, [struct('width', s, 'max_fires', n)])");
    out = dm_open_dev(prhs[1]);
    c = prhs[2];
    if (!mxIsStruct(c)) pm_err("arg", "choices: a struct array with kind, device, control, code");
    n = mxGetNumberOfElements(c);
    if (n < 1 || n > YDEV_RULE_CHOICES) pm_err("arg", "choices: 1 to 32");
    {
        int nf = mxGetNumberOfFields(c), k, j;
        for (k = 0; k < nf; k++) {
            const char* fn = mxGetFieldNameByNumber(c, k);
            int ok = 0;
            for (j = 0; j < 4; j++) if (strcmp(fn, cfields[j]) == 0) ok = 1;
            if (!ok) pm_err("arg", "choices: unknown field '%s'", fn);
        }
    }
    memset(ch, 0, sizeof ch);
    for (i = 0; i < n; i++) {
        const mxArray* v;
        v = mxGetField(c, i, "kind");
        ch[i].kind = (uint8_t)((v && !mxIsEmpty(v)) ? (uint32_t)pm_int(v, "kind") : YIN_KIND_BOX);
        if ((v = mxGetField(c, i, "kind")) && !mxIsEmpty(v) && (pm_int(v, "kind") < 0 || pm_int(v, "kind") > 15))
            pm_err("arg", "choices: kind is 0 to 15");
        if ((v = mxGetField(c, i, "device")) && !mxIsEmpty(v)) ch[i].device = dm_u32(v, "device");
        if ((v = mxGetField(c, i, "control")) && !mxIsEmpty(v)) ch[i].control = dm_u32(v, "control");
        v = mxGetField(c, i, "code");
        if (!v || mxIsEmpty(v)) pm_err("arg", "choices: each needs a code");
        ch[i].code = dm_u32(v, "code");
    }
    memset(&rd, 0, sizeof rd);
    if (nrhs > 3) {
        const mxArray* v;
        pm_check_fields(prhs[3], ofields, 2, "rule_open options");
        if ((v = pm_field(prhs[3], "width"))) rd.width_ns = dm_seconds(v, "width", DM_POS);
        if ((v = pm_field(prhs[3], "max_fires"))) {
            double m = pm_scalar(v, "max_fires");
            if (m == HUGE_VAL) rd.max_fires = -1;
            else {
                int k = pm_int(v, "max_fires");
                if (k < 1) pm_err("arg", "max_fires: >= 1, or Inf for no limit");
                rd.max_fires = k;
            }
        }
    }
    rd.out = &out->dev;
    rd.choices = ch;
    rd.n_choices = (int)n;
    r = (dm_rule*)calloc(1, sizeof *r);
    if (!r) pm_err("memory", "out of memory");
    r->h.tag = DM_RULE;
    if (!ydev_rule_open(&r->r, &rd)) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s", ydev_rule_error(&r->r));
        free(r);
        pm_err("arg", "%s", msg);
    }
    r->opened = 1;
    r->out = out;
    out->rule_refs++;
    dm_link(&r->h);
    plhs[0] = pm_handle_out(pm_register(r));
}

static dm_rule* dm_open_rule(const mxArray* a) {
    dm_rule* r = (dm_rule*)dm_get(a, DM_RULE, "a rule");
    if (!r->opened) pm_err("closed", "the rule is closed");
    return r;
}

static void dm_window(int nrhs, const mxArray* prhs[], int64_t* from, int64_t* until) {
    pm_nargs(nrhs, 3, "ysp_device('rule_arm', r, start, [until])");
    *from = dm_seconds(prhs[2], "start", DM_INF);
    *until = nrhs > 3 ? dm_seconds(prhs[3], "until", DM_INF) : YDEV_RULE_NEVER;
}

static mxArray* dm_rule_info(dm_rule* r) {
    static const char* f[] = { "armed", "fired", "over", "result", "t_event", "t_before", "t_after", "device",
                               "control", "code", "seq", "choice" };
    ydev_rule_info i;
    mxArray* s = mxCreateStructMatrix(1, 1, 12, f);
    int have;
    ydev_rule_get(&r->r, &i);
    have = i.choice >= 0;
    dm_set(s, "armed", mxCreateLogicalScalar(i.armed != 0));
    dm_set(s, "fired", dm_d(i.fired));
    dm_set(s, "over", dm_d(i.over));
    dm_set(s, "result", dm_d(i.result));
    dm_set(s, "t_event", have ? dm_d(dm_s(i.t_event)) : mxCreateDoubleMatrix(0, 0, mxREAL));
    dm_set(s, "t_before", have ? dm_d(dm_s(i.t_before)) : mxCreateDoubleMatrix(0, 0, mxREAL));
    dm_set(s, "t_after", have ? dm_d(dm_s(i.t_after)) : mxCreateDoubleMatrix(0, 0, mxREAL));
    dm_set(s, "device", dm_d(i.device));
    dm_set(s, "control", dm_d(i.control));
    dm_set(s, "code", dm_d(i.code));
    dm_set(s, "seq", dm_d(i.seq));
    /* 1-based, as every MEX index; [] for no fire */
    dm_set(s, "choice", have ? dm_d(i.choice + 1) : mxCreateDoubleMatrix(0, 0, mxREAL));
    return s;
}

static mxArray* dm_rule_fires(dm_rule* r) {
    static const char* f[] = { "t", "device", "control", "code", "kind", "choice", "result" };
    ydev_rule_fire fr[YDEV_RULE_FIRES];
    int n = ydev_rule_fires(&r->r, fr, YDEV_RULE_FIRES), k, i;
    mxArray* s;
    k = n < YDEV_RULE_FIRES ? n : YDEV_RULE_FIRES;
    s = mxCreateStructMatrix(1, (size_t)k, 7, f);
    for (i = 0; i < k; i++) {
        mxSetField(s, (size_t)i, "t", dm_d(dm_s(fr[i].t)));
        mxSetField(s, (size_t)i, "device", dm_d(fr[i].device));
        mxSetField(s, (size_t)i, "control", dm_d(fr[i].control));
        mxSetField(s, (size_t)i, "code", dm_d(fr[i].code));
        mxSetField(s, (size_t)i, "kind", dm_d(fr[i].kind));
        mxSetField(s, (size_t)i, "choice", dm_d(fr[i].choice + 1));
        mxSetField(s, (size_t)i, "result", dm_d(fr[i].result));
    }
    return s;
}

static void cmd_sim_open(mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    static const char* const fields[] = { "ppm", "frame", "tail", "key", "identify" };
    double ppm = 20.0;
    int64_t frame = 125000, tail = 50000;
    int family, identify = 1;
    char* key = NULL;
    dm_sim* s;
    pm_nargs(nrhs, 2, "s = ysp_device('sim_open', family, [struct('ppm', p, 'frame', s, 'tail', s, 'key', k, 'identify', b)])");
    family = dm_family(prhs[1]);
    if (family == YBOX_PARALLEL) pm_err("arg", "sim_open: no simulated parallel port");
    if (nrhs > 2) {
        const mxArray* v;
        pm_check_fields(prhs[2], fields, 5, "sim_open options");
        if ((v = pm_field(prhs[2], "ppm"))) ppm = pm_finite(v, "ppm");
        if ((v = pm_field(prhs[2], "frame"))) frame = dm_seconds(v, "frame", 0);
        if ((v = pm_field(prhs[2], "tail"))) tail = dm_seconds(v, "tail", 0);
        if ((v = pm_field(prhs[2], "key"))) key = pm_string(v, "key");
        if ((v = pm_field(prhs[2], "identify"))) identify = pm_bool(v, "identify");
    }
    if (!(ppm > -1000 && ppm < 1000)) pm_err("arg", "ppm: -1000 to 1000");
    if (key && strlen(key) >= 96) pm_err("arg", "key: at most 95 bytes");
    s = (dm_sim*)calloc(1, sizeof *s);
    if (!s) pm_err("memory", "out of memory");
    s->h.tag = DM_SIM;
    ybd_sim_init(&s->s, family, ppm, (double)frame, (double)tail, key);
    s->s.no_identity = !identify;
    dm_link(&s->h);
    plhs[0] = pm_handle_out(pm_register(s));
}

static mxArray* dm_sim_log(dm_sim* s, int lines) {
    static const char* fw[] = { "t", "data" };
    static const char* fl[] = { "t", "code", "changed" };
    int i, k = 0, n;
    mxArray* out;
    ybd_lock(&s->s.lock);
    n = s->s.n_writes;
    for (i = 0; i < n; i++) if ((s->s.writes[i].n == 0) == (lines != 0)) k++;
    out = lines ? mxCreateStructMatrix(1, (size_t)k, 3, fl) : mxCreateStructMatrix(1, (size_t)k, 2, fw);
    k = 0;
    for (i = 0; i < n; i++) {
        const ybd_sim_wrec* w = &s->s.writes[i];
        if ((w->n == 0) != (lines != 0)) continue;
        mxSetField(out, (size_t)k, "t", dm_d((double)w->t / 1e9));
        if (lines) {
            mxSetField(out, (size_t)k, "code", dm_d(w->code));
            mxSetField(out, (size_t)k, "changed", dm_d(w->changed));
        } else {
            mxSetField(out, (size_t)k, "data", pm_bytes_out(w->b, (size_t)(w->n < YBD_SIM_WBYTES ? w->n : YBD_SIM_WBYTES)));
        }
        k++;
    }
    ybd_unlock(&s->s.lock);
    return out;
}

/* ---- constants ---------------------------------------------------------------- */

static mxArray* dm_constants(void) {
    static const struct { const char* name; double v; } c[] = {
        { "XID", YBOX_XID }, { "LINE", YBOX_LINE }, { "PHOTO", YBOX_PHOTO }, { "TRIGGERBOX", YBOX_TRIGGERBOX },
        { "BIOSEMI", YBOX_BIOSEMI }, { "MMBTS", YBOX_MMBTS }, { "LINES", YBOX_LINES }, { "PARALLEL", YBOX_PARALLEL },
        { "CLOSED", YDEV_CLOSED }, { "OPENING", YDEV_OPENING }, { "RUNNING", YDEV_RUNNING }, { "LOST", YDEV_LOST },
        { "FAILED", YDEV_FAILED },
        { "OK", YDEV_OK }, { "ERR_ARG", YDEV_ERR_ARG }, { "ERR_FAMILY", YDEV_ERR_FAMILY },
        { "ERR_STATE", YDEV_ERR_STATE }, { "ERR_IO", YDEV_ERR_IO }, { "ERR_TIMEOUT", YDEV_ERR_TIMEOUT },
        { "RULE_PENDING", YDEV_RULE_PENDING },
        { "OUT_DEVICE_TIMED", YDEV_OUT_DEVICE_TIMED }, { "OUT_FLUSHED", YDEV_OUT_FLUSHED },
        { "OUT_TRAILING", YDEV_OUT_TRAILING }, { "OUT_FAILED", YDEV_OUT_FAILED }, { "OUT_MARK", YDEV_OUT_MARK },
        { "OUT_FLIP", YDEV_OUT_FLIP }, { "OUT_REPORTED", YDEV_OUT_REPORTED }, { "OUT_REPLACED", YDEV_OUT_REPLACED },
        { "OUT_PULSE", YDEV_OUT_PULSE }, { "OUT_RULE", YDEV_OUT_RULE }, { "MAX_ROLES", YDEV_MAX_ROLES },
        { "KIND_KEYBOARD", YIN_KIND_KEYBOARD }, { "KIND_MOUSE", YIN_KIND_MOUSE }, { "KIND_TOUCH", YIN_KIND_TOUCH },
        { "KIND_PEN", YIN_KIND_PEN }, { "KIND_GAMEPAD", YIN_KIND_GAMEPAD }, { "KIND_BOX", YIN_KIND_BOX },
        { "KIND_EYE", YIN_KIND_EYE }, { "KIND_SYNC", YIN_KIND_SYNC }, { "KIND_USER", YIN_KIND_USER },
        { "NONE", YIN_NONE }, { "PRESS", YIN_PRESS }, { "RELEASE", YIN_RELEASE }, { "SAMPLE", YIN_SAMPLE },
        { "PROXIMITY", YIN_PROXIMITY },
        { "REPEAT", YIN_REPEAT }, { "SYNTHETIC", YIN_SYNTHETIC }, { "ERASER", YIN_ERASER },
        { "UNLISTED", YIN_UNLISTED }, { "DEVTICKS", YIN_DEVTICKS },
        { "TIER_UNKNOWN", YIN_TIER_UNKNOWN }, { "TIER_1", YIN_TIER_1 }, { "TIER_2", YIN_TIER_2 },
        { "TIER_3", YIN_TIER_3 }, { "TIER_SIM", YIN_TIER_SIM },
    };
    const char* names[64];
    int n = (int)(sizeof c / sizeof c[0]), i;
    mxArray* s;
    for (i = 0; i < n; i++) names[i] = c[i].name;
    s = mxCreateStructMatrix(1, 1, n, names);
    for (i = 0; i < n; i++) mxSetFieldByNumber(s, 0, i, dm_d(c[i].v));
    return s;
}

/* ---- dispatch ------------------------------------------------------------------- */

#define DM_SLICE_NS YRT_MS(50)

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char* cmd;
    pm_destroy = dm_destroy;
    if (nrhs < 1 || !mxIsChar(prhs[0])) pm_err("usage", "first argument must be a command string");
    cmd = mxArrayToString(prhs[0]);

    if (strcmp(cmd, "version") == 0) {
        plhs[0] = mxCreateString(DM_BINDING_VERSION);
        if (nlhs > 1) plhs[1] = mxCreateString(ydev_version());
        return;
    }
    if (strcmp(cmd, "now") == 0) { plhs[0] = dm_d((double)yrt_now_ns() / 1e9); return; }
    if (strcmp(cmd, "constants") == 0) { plhs[0] = dm_constants(); return; }
    if (strcmp(cmd, "list") == 0) {
        static const char* f[] = { "key", "name", "description", "serial_number", "location", "driver", "vid", "pid" };
        int n = yser_list_ports(NULL, 0), got, i;
        yser_port_info* v = NULL;
        if (n < 0) pm_err("io", "%s", yser_strerror(n));
        if (n > 0) {
            v = (yser_port_info*)mxMalloc((size_t)n * sizeof *v);
            got = yser_list_ports(v, n);
            if (got < 0) pm_err("io", "%s", yser_strerror(got));
            if (got < n) n = got;
        }
        plhs[0] = mxCreateStructMatrix(1, (size_t)n, 8, f);
        for (i = 0; i < n; i++) {
            char key[YDEV_KEY_MAX];
            if (ydev_port_key(&v[i], key, sizeof key) < 0) key[0] = '\0';
            mxSetField(plhs[0], (size_t)i, "key", mxCreateString(key));
            mxSetField(plhs[0], (size_t)i, "name", mxCreateString(v[i].name));
            mxSetField(plhs[0], (size_t)i, "description", mxCreateString(v[i].description));
            mxSetField(plhs[0], (size_t)i, "serial_number", mxCreateString(v[i].serial_number));
            mxSetField(plhs[0], (size_t)i, "location", mxCreateString(v[i].location));
            mxSetField(plhs[0], (size_t)i, "driver", mxCreateString(v[i].driver));
            mxSetField(plhs[0], (size_t)i, "vid", dm_d(v[i].vid));
            mxSetField(plhs[0], (size_t)i, "pid", dm_d(v[i].pid));
        }
        return;
    }
    if (strcmp(cmd, "key_match") == 0) {
        pm_nargs(nrhs, 3, "tf = ysp_device('key_match', pattern, key)");
        plhs[0] = mxCreateLogicalScalar(ydev_key_match(pm_string(prhs[1], "pattern"), pm_string(prhs[2], "key")));
        return;
    }
    if (strcmp(cmd, "roles") == 0) {
        int i;
        plhs[0] = mxCreateCellMatrix(1, (size_t)dm_roles.n);
        for (i = 0; i < dm_roles.n; i++) mxSetCell(plhs[0], (size_t)i, mxCreateString(dm_roles.name[i]));
        return;
    }
    if (strcmp(cmd, "family") == 0) {
        pm_nargs(nrhs, 2, "[n, name] = ysp_device('family', name_or_number)");
        {
            int f = dm_family(prhs[1]);
            plhs[0] = dm_d(f);
            if (nlhs > 1) plhs[1] = mxCreateString(ybox_family_name(f));
        }
        return;
    }
    if (strcmp(cmd, "open") == 0) { cmd_open(nlhs, plhs, nrhs, prhs); return; }
    if (strcmp(cmd, "rule_open") == 0) { cmd_rule_open(plhs, nrhs, prhs); return; }
    if (strcmp(cmd, "sim_open") == 0) { cmd_sim_open(plhs, nrhs, prhs); return; }

    pm_nargs(nrhs, 2, "ysp_device(command, handle, ...)");

    /* ---- devices ---- */
    if (strcmp(cmd, "close") == 0) {
        dm_dev* d = (dm_dev*)dm_get(prhs[1], DM_DEV, "a device");
        if (d->rule_refs > 0)
            pm_err("order", "close: an open rule writes to this device; close the input, then the rule, then this");
        dm_dev_close(d);
        pm_unregister(d);
        dm_free(&d->h);
        return;
    }
    if (strcmp(cmd, "wait_running") == 0) {
        dm_dev* d = dm_open_dev(prhs[1]);
        int64_t timeout;
        int rc;
        pm_nargs(nrhs, 3, "ok = ysp_device('wait_running', h, timeout)");
        timeout = dm_seconds(prhs[2], "timeout", DM_INF);
        rc = ydev_wait_running(&d->dev, timeout);   /* Inf: INT64_MAX, which waits (ysp/device.h 0.5.1) */
        if (rc == YDEV_RUNNING) { plhs[0] = mxCreateLogicalScalar(true); return; }
        if (rc == YDEV_ERR_TIMEOUT) { plhs[0] = mxCreateLogicalScalar(false); return; }
        pm_err("failed", "%s", ydev_error(&d->dev));
    }
    if (strcmp(cmd, "poll") == 0 || strcmp(cmd, "wait") == 0) {
        dm_dev* d = (dm_dev*)dm_get(prhs[1], DM_DEV, "a device");
        int is_wait = cmd[0] == 'w';
        int ai = is_wait ? 3 : 2;
        uint64_t max = 0;
        if (is_wait) {
            int64_t timeout, end;
            pm_nargs(nrhs, 3, "ev = ysp_device('wait', h, timeout, [max])");
            timeout = dm_seconds(prhs[2], "timeout", DM_INF);
            end = timeout == INT64_MAX ? INT64_MAX : yrt_now_ns() + timeout;
            for (;;) {
                int closed;
                int64_t now = yrt_now_ns(), slice = end - now;
                if (slice > DM_SLICE_NS) slice = DM_SLICE_NS;
                if (slice < 0) slice = 0;
                if (ybd_queue_wait(&d->q, slice, &closed) || closed || yrt_now_ns() >= end) break;
            }
        }
        if (nrhs > ai) {
            double m = pm_scalar(prhs[ai], "max");
            if (!(m >= 0) || m != floor(m)) pm_err("arg", "max: events, >= 0 (0: all)");
            max = (uint64_t)m;
        }
        plhs[0] = dm_take(d, max);
        return;
    }
    if (strcmp(cmd, "pending") == 0 || strcmp(cmd, "clear") == 0) {
        dm_dev* d = (dm_dev*)dm_get(prhs[1], DM_DEV, "a device");
        uint64_t n;
        if (cmd[0] == 'c') n = ybd_queue_clear(&d->q);
        else { ybd_lock(&d->q.lock); n = d->q.head - d->q.tail; ybd_unlock(&d->q.lock); }
        plhs[0] = dm_d((double)n);
        return;
    }
    if (strcmp(cmd, "set") == 0 || strcmp(cmd, "pulse") == 0 || strcmp(cmd, "mark") == 0) {
        dm_dev* d = dm_open_dev(prhs[1]);
        uint32_t code;
        int rc;
        pm_nargs(nrhs, 3, "ysp_device('set', h, code) | ('pulse', h, code, width) | ('mark', h, code, text)");
        code = dm_u32(prhs[2], "code");
        if (cmd[0] == 's') rc = ydev_out_set(&d->dev, code);
        else if (cmd[0] == 'p') {
            pm_nargs(nrhs, 4, "ysp_device('pulse', h, code, width)");
            rc = ydev_out_pulse(&d->dev, code, dm_seconds(prhs[3], "width", DM_POS));
        } else {
            rc = ydev_out_mark(&d->dev, code, nrhs > 3 ? pm_string(prhs[3], "text") : NULL);
        }
        if (rc < 0) dm_out_fail(cmd, rc);
        return;
    }
    if (strcmp(cmd, "last_out") == 0) {
        dm_dev* d = (dm_dev*)dm_get(prhs[1], DM_DEV, "a device");
        ydev_out_info o;
        ydev_out_last(&d->dev, &o);
        plhs[0] = dm_out_info(&o);
        return;
    }
    if (strcmp(cmd, "stats") == 0) { plhs[0] = dm_stats((dm_dev*)dm_get(prhs[1], DM_DEV, "a device")); return; }
    if (strcmp(cmd, "info") == 0) { plhs[0] = dm_info((dm_dev*)dm_get(prhs[1], DM_DEV, "a device")); return; }
    if (strcmp(cmd, "state") == 0) {
        dm_dev* d = (dm_dev*)dm_get(prhs[1], DM_DEV, "a device");
        int s = ydev_state(&d->dev);
        plhs[0] = dm_d(s);
        if (nlhs > 1) plhs[1] = mxCreateString(ydev_state_name(s));
        return;
    }
    if (strcmp(cmd, "source") == 0) {
        yin_source s = ydev_source(&((dm_dev*)dm_get(prhs[1], DM_DEV, "a device"))->dev);
        plhs[0] = dm_source(&s);
        return;
    }
    if (strcmp(cmd, "error") == 0) {
        dm_dev* d = (dm_dev*)dm_get(prhs[1], DM_DEV, "a device");
        (void)ydev_state(&d->dev);   /* after the reader's last write under the lock */
        plhs[0] = mxCreateString(ydev_error(&d->dev));
        return;
    }

    /* ---- rules ---- */
    if (strcmp(cmd, "rule_arm") == 0 || strcmp(cmd, "rule_window") == 0) {
        dm_rule* r = dm_open_rule(prhs[1]);
        int64_t a, b;
        dm_window(nrhs, prhs, &a, &b);
        if (cmd[5] == 'a') ydev_rule_arm(&r->r, a, b);
        else ydev_rule_window(&r->r, a, b);
        return;
    }
    if (strcmp(cmd, "rule_disarm") == 0) { ydev_rule_disarm(&dm_open_rule(prhs[1])->r); return; }
    if (strcmp(cmd, "rule_info") == 0) { plhs[0] = dm_rule_info(dm_open_rule(prhs[1])); return; }
    if (strcmp(cmd, "rule_fires") == 0) { plhs[0] = dm_rule_fires(dm_open_rule(prhs[1])); return; }
    if (strcmp(cmd, "rule_close") == 0) {
        dm_rule* r = (dm_rule*)dm_get(prhs[1], DM_RULE, "a rule");
        if (r->users > 0) pm_err("order", "rule_close: an open device feeds this rule; close it first");
        dm_rule_close(r);
        pm_unregister(r);
        dm_free(&r->h);
        return;
    }

    /* ---- simulated boards ---- */
    if (strcmp(cmd, "sim_edge") == 0 || strcmp(cmd, "sim_press") == 0) {
        dm_sim* s = (dm_sim*)dm_get(prhs[1], DM_SIM, "a sim");
        int64_t t;
        int ch = 1, maxch = s->s.family == YBOX_XID ? 8 : 255;
        pm_nargs(nrhs, 4, "ysp_device('sim_edge', s, t, level, [channel]) | ('sim_press', s, t, duration, [channel])");
        t = dm_seconds(prhs[2], "t", 0);
        if (nrhs > 4) ch = pm_int(prhs[4], "channel");
        if (ch < 1 || ch > maxch) pm_err("arg", "channel: 1 to 255 (XID: 1 to 8)");
        if (strcmp(cmd, "sim_edge") == 0) {
            if (ybd_sim_add_edge(&s->s, t, ch, pm_bool(prhs[3], "level")) < 0) pm_err("full", "the edge list is full");
        } else {
            int64_t dur = dm_seconds(prhs[3], "duration", DM_POS);
            if (ybd_sim_add_edge(&s->s, t, ch, 1) < 0 || ybd_sim_add_edge(&s->s, t + dur, ch, 0) < 0)
                pm_err("full", "the edge list is full");
        }
        return;
    }
    if (strcmp(cmd, "sim_unplug") == 0) {
        dm_sim* s = (dm_sim*)dm_get(prhs[1], DM_SIM, "a sim");
        pm_nargs(nrhs, 4, "ysp_device('sim_unplug', s, start, stop)");
        ybd_sim_unplug(&s->s, dm_seconds(prhs[2], "start", 0), dm_seconds(prhs[3], "stop", DM_INF));
        return;
    }
    if (strcmp(cmd, "sim_writes") == 0 || strcmp(cmd, "sim_lines") == 0) {
        plhs[0] = dm_sim_log((dm_sim*)dm_get(prhs[1], DM_SIM, "a sim"), cmd[4] == 'l');
        return;
    }
    if (strcmp(cmd, "sim_key") == 0) {
        dm_sim* s = (dm_sim*)dm_get(prhs[1], DM_SIM, "a sim");
        plhs[0] = mxCreateString(s->s.key);
        return;
    }
    if (strcmp(cmd, "sim_opens") == 0) {
        dm_sim* s = (dm_sim*)dm_get(prhs[1], DM_SIM, "a sim");
        int n;
        ybd_lock(&s->s.lock);
        n = s->s.opens;
        ybd_unlock(&s->s.lock);
        plhs[0] = dm_d(n);
        return;
    }
    if (strcmp(cmd, "sim_close") == 0) {
        dm_sim* s = (dm_sim*)dm_get(prhs[1], DM_SIM, "a sim");
        if (s->users > 0) pm_err("order", "sim_close: an open device uses this sim; close it first");
        pm_unregister(s);
        dm_free(&s->h);
        return;
    }
    pm_err("usage", "unknown command '%s'", cmd);
}
