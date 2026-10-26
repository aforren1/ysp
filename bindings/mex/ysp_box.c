/* ysp_box.c - MATLAB/Octave MEX binding for ysp/box.h (builds to ysp_box.<mexext>)
 *
 * The analysis side of a response box, a trigger box or a ysp line board:
 * captured bytes in, ysp/input.h events and clock pairs out, with no port
 * or thread. Also the bytes a host writes: queries and output codes.
 * Command dispatch in the style of the other ysp MEX files; 'open' returns
 * an opaque uint64 handle to a decoder.
 *
 *     h = ysp_box('open', 'line');                    % kind BOX, device 0
 *     [ev, pairs, info] = ysp_box('decode', h, bytes, t_read);
 *     [ev, pairs] = ysp_box('decode', 'xid', fread(fid, Inf, '*uint8'));
 *     b = ysp_box('encode', 'triggerbox', 'set', 12); % uint8 row
 *     c = ysp_box('constants');                       % KIND_BOX, PRESS, ...
 *     ysp_box('close', h);
 *
 * ev is an 1 x N struct array with the fields of ysp/input.h's event in
 * the order every ysp binding uses: t (seconds), t_ns (int64), device,
 * control, code, mods, kind, type, flags, stamp, unc (seconds), x, y,
 * value, aux, ticks. pairs is a struct of columns: ticks (uint64), probe.
 *
 * Build with build.m. See README.md for the command reference.
 */
#define PM_MOD "ysp_box"
#include "ysp_mex_util.h"

/* The MEX function's own version (docs/bindings.md, Versions); the
 * header's is the second output of 'version'. */
#define MEX_BINDING_VERSION "1.0.0"

#define YSP_BOX_IMPLEMENTATION
#include "ysp/box.h"

/* Entries per ybox_decode() call; the header needs 1, more means fewer
 * calls per chunk. */
#define BOX_CAP 64

typedef struct box_handle {
    ybox_decoder d;
    yin_event ev[BOX_CAP];
    ybox_pair pair[BOX_CAP];
    ybox_out out;
} box_handle;

static void box_destroy(void* obj) { free(obj); }

/* Seconds to the nearest ns of the exact product s x 1e9 (fma gives the
 * rounding error of the double product), so t = ns / 1e9 gives ns back, as
 * in ysp.rdk and ysp.box. Refuses NaN, a negative time, Inf and a time past
 * int64 ns. */
static int64_t box_s_to_ns(const mxArray* a, const char* what) {
    volatile double p;   /* stored as a double: x87 would keep 80 bits */
    double s = pm_scalar(a, what), e, n, r;
    if (!(s >= 0.0 && s < 9.2e9)) pm_err("arg", "%s: seconds, >= 0 and finite", what);
    p = s * 1e9;
    e = fma(s, 1e9, -p);
    n = floor(p + 0.5);
    r = (p - n) + e;
    if (r > 0.5) n += 1.0;
    else if (r < -0.5) n -= 1.0;
    return (int64_t)n;
}

/* An integer in [0, max]; fractions are refused, not truncated. */
static double box_uint(const mxArray* a, const char* what, double max) {
    double d = pm_scalar(a, what);
    if (!(d >= 0 && d <= max && d == floor(d))) pm_err("arg", "%s: an integer, 0 to %.0f", what, max);
    return d;
}

/* A family: its name (any case) or its value; an unknown family is refused
 * here, so no call below sees one. */
static int box_family(const mxArray* a) {
    int f;
    if (mxIsChar(a)) {
        char* s = pm_string(a, "family");
        for (f = YBOX_XID; f <= YBOX_FAMILY_LAST; f++)
            if (pm_lower_eq(s, ybox_family_name(f))) return f;
        pm_err("arg", "unknown family '%s'; expected xid, line, photo, triggerbox, biosemi, mmbts, lines or parallel", s);
    }
    f = (int)box_uint(a, "family", YBOX_FAMILY_LAST);
    if (f < YBOX_XID) pm_err("arg", "family: 1 to %d, or a name", YBOX_FAMILY_LAST);
    return f;
}

static const char* const box_op_names[] = { "", "set", "pulse", "width", "close" };

static int box_op(const mxArray* a) {
    int op = pm_enum(a, "op", box_op_names, 5);
    if (op == 0) pm_err("arg", "op: 'set', 'pulse', 'width' or 'close'");
    return op;
}

static box_handle* box_get(const mxArray* a) { return (box_handle*)pm_lookup(a); }

static void box_setup(box_handle* h, int family, int kind, uint32_t device) {
    if (ybox_init(&h->d, family, (uint8_t)kind, device) < 0) pm_err("arg", "%s", ybox_strerror(YBOX_ERR_ARG));
    memset(&h->out, 0, sizeof h->out);
    h->out.ev = h->ev;
    h->out.ev_cap = BOX_CAP;
    h->out.pair = h->pair;
    h->out.pair_cap = BOX_CAP;
}

/* ---- decode ----------------------------------------------------------------- */

static const char* box_ev_fields[] = { "t", "t_ns", "device", "control", "code", "mods", "kind", "type",
                                             "flags", "stamp", "unc", "x", "y", "value", "aux", "ticks" };

static mxArray* box_int64(int64_t v) {
    mxArray* a = mxCreateNumericMatrix(1, 1, mxINT64_CLASS, mxREAL);
    *(int64_t*)mxGetData(a) = v;
    return a;
}

static mxArray* box_events_out(const yin_event* ev, size_t n) {
    mxArray* s = mxCreateStructMatrix(1, n, 16, box_ev_fields);
    size_t i;
    for (i = 0; i < n; i++) {
        const yin_event* e = &ev[i];
        mxSetFieldByNumber(s, i, 0, mxCreateDoubleScalar((double)e->t / 1e9));
        mxSetFieldByNumber(s, i, 1, box_int64(e->t));
        mxSetFieldByNumber(s, i, 2, mxCreateDoubleScalar((double)e->device));
        mxSetFieldByNumber(s, i, 3, mxCreateDoubleScalar((double)e->control));
        mxSetFieldByNumber(s, i, 4, mxCreateDoubleScalar((double)e->code));
        mxSetFieldByNumber(s, i, 5, mxCreateDoubleScalar((double)e->mods));
        mxSetFieldByNumber(s, i, 6, mxCreateDoubleScalar((double)e->kind));
        mxSetFieldByNumber(s, i, 7, mxCreateDoubleScalar((double)e->type));
        mxSetFieldByNumber(s, i, 8, mxCreateDoubleScalar((double)e->flags));
        mxSetFieldByNumber(s, i, 9, mxCreateDoubleScalar((double)e->stamp));
        mxSetFieldByNumber(s, i, 10, mxCreateDoubleScalar((double)e->unc_us / 1e6));
        mxSetFieldByNumber(s, i, 11, mxCreateDoubleScalar((double)e->x));
        mxSetFieldByNumber(s, i, 12, mxCreateDoubleScalar((double)e->y));
        mxSetFieldByNumber(s, i, 13, mxCreateDoubleScalar((double)e->value));
        mxSetFieldByNumber(s, i, 14, mxCreateDoubleScalar((double)e->aux));
        mxSetFieldByNumber(s, i, 15, mxCreateDoubleScalar((double)e->ticks));
    }
    return s;
}

/* A growing array in mxMalloc memory, which MATLAB frees when the call
 * returns or raises. */
static void* box_grow(void* p, size_t* cap, size_t need, size_t size) {
    if (need <= *cap) return p;
    *cap = *cap ? *cap * 2 : 256;
    if (*cap < need) *cap = need;
    return p ? mxRealloc(p, *cap * size) : mxMalloc(*cap * size);
}

static mxArray* box_columns(const char* a, const char* b, const uint64_t* ticks, const double* other, size_t n) {
    const char* f[2];
    mxArray* s;
    mxArray* t = mxCreateNumericMatrix(n, 1, mxUINT64_CLASS, mxREAL);
    mxArray* o = mxCreateDoubleMatrix(n, 1, mxREAL);
    f[0] = a; f[1] = b;
    s = mxCreateStructMatrix(1, 1, 2, f);
    if (n) {
        memcpy(mxGetData(t), ticks, n * sizeof *ticks);
        memcpy(mxGetPr(o), other, n * sizeof *other);
    }
    mxSetField(s, 0, a, t);
    mxSetField(s, 0, b, o);
    return s;
}

/* Every byte through ybox_decode(), in calls that each fill the handle's
 * arrays at most; the results are gathered and returned at once. */
static void box_decode(box_handle* h, const mxArray* bytes, const mxArray* tq, int nlhs, mxArray* plhs[]) {
    size_t n, used = 0, nev = 0, cev = 0, np = 0, cp = 0, no = 0, co = 0, nt = 0, ct = 0, i;
    const uint8_t* p = pm_bytes_in(bytes, "bytes", &n);
    int64_t t = tq ? box_s_to_ns(tq, "t") : 0;
    yin_event* ev = NULL;
    uint64_t *pt = NULL, *ot = NULL;
    double *pp = NULL, *oc = NULL, garbage = 0;
    char** text = NULL;
    while (used < n) {
        int k;
        size_t r;
        ybox_out_clear(&h->out);
        r = ybox_decode(&h->d, p + used, n - used, t, &h->out);
        if (r == 0) pm_err("internal", "the decoder used no byte");
        used += r;
        garbage += (double)h->out.garbage;
        ev = (yin_event*)box_grow(ev, &cev, nev + (size_t)h->out.n_ev, sizeof *ev);
        for (k = 0; k < h->out.n_ev; k++) ev[nev++] = h->ev[k];
        if (h->out.n_pair) {
            size_t c0 = cp;
            pt = (uint64_t*)box_grow(pt, &cp, np + (size_t)h->out.n_pair, sizeof *pt);
            pp = (double*)box_grow(pp, &c0, np + (size_t)h->out.n_pair, sizeof *pp);
            for (k = 0; k < h->out.n_pair; k++, np++) { pt[np] = h->pair[k].ticks; pp[np] = (double)h->pair[k].probe; }
        }
        if (h->out.has_out) {
            size_t c0 = co;
            ot = (uint64_t*)box_grow(ot, &co, no + 1, sizeof *ot);
            oc = (double*)box_grow(oc, &c0, no + 1, sizeof *oc);
            ot[no] = h->out.out_ticks;
            oc[no++] = (double)h->out.out_code;
        }
        if (h->out.has_text) {
            size_t len = strlen(h->out.text);
            text = (char**)box_grow(text, &ct, nt + 1, sizeof *text);
            text[nt] = (char*)mxMalloc(len + 1);
            memcpy(text[nt++], h->out.text, len + 1);
        }
    }
    plhs[0] = box_events_out(ev, nev);
    if (nlhs > 1) plhs[1] = box_columns("ticks", "probe", pt, pp, np);
    if (nlhs > 2) {
        static const char* f[] = { "garbage", "text", "outputs" };
        mxArray* s = mxCreateStructMatrix(1, 1, 3, f);
        mxArray* c = mxCreateCellMatrix(nt, 1);
        for (i = 0; i < nt; i++) mxSetCell(c, i, mxCreateString(text[i]));
        mxSetField(s, 0, "garbage", mxCreateDoubleScalar(garbage));
        mxSetField(s, 0, "text", c);
        mxSetField(s, 0, "outputs", box_columns("ticks", "code", ot, oc, no));
        plhs[2] = s;
    }
}

/* ---- facts and constants ------------------------------------------------------ */

static mxArray* box_family_out(int f) {
    static const char* fn[] = { "name", "value", "caps", "code_max", "fixed_pulse", "baud", "tick", "slow_write" };
    mxArray* s = mxCreateStructMatrix(1, 1, 8, fn);
    mxSetField(s, 0, "name", mxCreateString(ybox_family_name(f)));
    mxSetField(s, 0, "value", mxCreateDoubleScalar(f));
    mxSetField(s, 0, "caps", mxCreateDoubleScalar((double)ybox_caps(f)));
    mxSetField(s, 0, "code_max", mxCreateDoubleScalar((double)ybox_code_max(f)));
    mxSetField(s, 0, "fixed_pulse", mxCreateDoubleScalar((double)ybox_fixed_pulse_ns(f) / 1e9));
    mxSetField(s, 0, "baud", mxCreateDoubleScalar((double)ybox_default_baud(f)));
    mxSetField(s, 0, "tick", mxCreateDoubleScalar(ybox_ns_per_tick(f) / 1e9));
    mxSetField(s, 0, "slow_write", mxCreateLogicalScalar(ybox_slow_write(f)));
    return s;
}

static mxArray* box_constants(void) {
    static const struct { const char* name; double v; } c[] = {
        { "XID", YBOX_XID }, { "LINE", YBOX_LINE }, { "PHOTO", YBOX_PHOTO }, { "TRIGGERBOX", YBOX_TRIGGERBOX },
        { "BIOSEMI", YBOX_BIOSEMI }, { "MMBTS", YBOX_MMBTS }, { "LINES", YBOX_LINES }, { "PARALLEL", YBOX_PARALLEL },
        { "CAP_IN", YBOX_CAP_IN }, { "CAP_OUT", YBOX_CAP_OUT }, { "CAP_LINES", YBOX_CAP_LINES },
        { "CAP_PULSE", YBOX_CAP_PULSE }, { "CAP_FIXED", YBOX_CAP_FIXED }, { "CAP_IDENT", YBOX_CAP_IDENT },
        { "OP_SET", YBOX_OP_SET }, { "OP_PULSE", YBOX_OP_PULSE }, { "OP_WIDTH", YBOX_OP_WIDTH },
        { "OP_CLOSE", YBOX_OP_CLOSE },
        { "KIND_KEYBOARD", YIN_KIND_KEYBOARD }, { "KIND_MOUSE", YIN_KIND_MOUSE }, { "KIND_TOUCH", YIN_KIND_TOUCH },
        { "KIND_PEN", YIN_KIND_PEN }, { "KIND_GAMEPAD", YIN_KIND_GAMEPAD }, { "KIND_BOX", YIN_KIND_BOX },
        { "KIND_EYE", YIN_KIND_EYE }, { "KIND_SYNC", YIN_KIND_SYNC }, { "KIND_USER", YIN_KIND_USER },
        { "PRESS", YIN_PRESS }, { "RELEASE", YIN_RELEASE }, { "SAMPLE", YIN_SAMPLE }, { "PROXIMITY", YIN_PROXIMITY },
        { "REPEAT", YIN_REPEAT }, { "SYNTHETIC", YIN_SYNTHETIC }, { "ERASER", YIN_ERASER },
        { "UNLISTED", YIN_UNLISTED }, { "DEVTICKS", YIN_DEVTICKS },
        { "TIER_UNKNOWN", YIN_TIER_UNKNOWN }, { "TIER_1", YIN_TIER_1 }, { "TIER_2", YIN_TIER_2 },
        { "TIER_3", YIN_TIER_3 }, { "TIER_SIM", YIN_TIER_SIM },
        { "UNC_MAX", (double)YIN_UNC_MAX / 1e6 }, { "SLOW_WRITE", (double)YBOX_SLOW_WRITE_NS / 1e9 },
    };
    const int n = (int)(sizeof c / sizeof c[0]);
    const char* names[64];
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
    pm_destroy = box_destroy;
    if (nrhs < 1 || !mxIsChar(prhs[0])) pm_err("usage", "usage: ysp_box(command, ...); see README.md");
    if (mxGetString(prhs[0], cmd, sizeof cmd)) pm_err("usage", "unknown command");

    if (strcmp(cmd, "version") == 0) { plhs[0] = mxCreateString(MEX_BINDING_VERSION); if (nlhs > 1) plhs[1] = mxCreateString(ybox_version()); return; }
    if (strcmp(cmd, "constants") == 0) { plhs[0] = box_constants(); return; }
    if (strcmp(cmd, "family") == 0) {
        pm_nargs(nrhs, 2, "info = ysp_box('family', family)");
        plhs[0] = box_family_out(box_family(prhs[1]));
        return;
    }
    if (strcmp(cmd, "open") == 0) {
        box_handle* h;
        int f, kind = YIN_KIND_BOX;
        uint32_t device = 0;
        pm_nargs(nrhs, 2, "h = ysp_box('open', family, kind, device)");
        f = box_family(prhs[1]);
        if (nrhs > 2 && !mxIsEmpty(prhs[2])) kind = (int)box_uint(prhs[2], "kind", 255);
        if (nrhs > 3 && !mxIsEmpty(prhs[3])) device = (uint32_t)box_uint(prhs[3], "device", 4294967295.0);
        h = (box_handle*)calloc(1, sizeof *h);
        if (!h) pm_err("memory", "out of memory");
        box_setup(h, f, kind, device);
        plhs[0] = pm_handle_out(pm_register(h));
        return;
    }
    if (strcmp(cmd, "decode") == 0) {
        pm_nargs(nrhs, 3, "[ev, pairs, info] = ysp_box('decode', h_or_family, bytes, t)");
        if (mxIsChar(prhs[1])) {
            /* one shot: a whole capture through a decoder of this call */
            box_handle* h = (box_handle*)mxCalloc(1, sizeof *h);
            box_setup(h, box_family(prhs[1]), YIN_KIND_BOX, 0);
            box_decode(h, prhs[2], nrhs > 3 ? prhs[3] : NULL, nlhs, plhs);
        } else {
            box_decode(box_get(prhs[1]), prhs[2], nrhs > 3 ? prhs[3] : NULL, nlhs, plhs);
        }
        return;
    }
    if (strcmp(cmd, "stats") == 0) {
        static const char* f[] = { "family", "kind", "device", "frames", "garbage", "pending" };
        box_handle* h;
        mxArray* s;
        pm_nargs(nrhs, 2, "s = ysp_box('stats', h)");
        h = box_get(prhs[1]);
        s = mxCreateStructMatrix(1, 1, 6, f);
        mxSetField(s, 0, "family", mxCreateString(ybox_family_name(h->d.family)));
        mxSetField(s, 0, "kind", mxCreateDoubleScalar(h->d.kind));
        mxSetField(s, 0, "device", mxCreateDoubleScalar(h->d.device));
        mxSetField(s, 0, "frames", mxCreateDoubleScalar((double)h->d.frames));
        mxSetField(s, 0, "garbage", mxCreateDoubleScalar((double)h->d.garbage));
        mxSetField(s, 0, "pending", mxCreateDoubleScalar(h->d.n));
        plhs[0] = s;
        return;
    }
    if (strcmp(cmd, "reset") == 0) {
        box_handle* h;
        pm_nargs(nrhs, 2, "ysp_box('reset', h)");
        h = box_get(prhs[1]);
        box_setup(h, h->d.family, h->d.kind, h->d.device);
        return;
    }
    if (strcmp(cmd, "close") == 0) {
        box_handle* h;
        pm_nargs(nrhs, 2, "ysp_box('close', h)");
        h = box_get(prhs[1]);
        pm_unregister(h);
        box_destroy(h);
        return;
    }
    if (strcmp(cmd, "encode") == 0) {
        uint8_t buf[64];
        int f, op, n;
        uint32_t code = 0;
        int64_t width = 0;
        pm_nargs(nrhs, 3, "bytes = ysp_box('encode', family, op, code, width)");
        f = box_family(prhs[1]);
        op = box_op(prhs[2]);
        if (nrhs > 3 && !mxIsEmpty(prhs[3])) code = (uint32_t)box_uint(prhs[3], "code", 4294967295.0);
        if (nrhs > 4 && !mxIsEmpty(prhs[4])) width = box_s_to_ns(prhs[4], "width");
        n = ybox_encode(f, op, code, width, buf, (int)sizeof buf);
        if (n < 0)
            pm_err("arg", "encode: %s (a family without outputs, a code above code_max = %lu, or a width the protocol "
                          "cannot carry)", ybox_strerror(n), (unsigned long)ybox_code_max(f));
        plhs[0] = pm_bytes_out(buf, (size_t)n);
        return;
    }
    if (strcmp(cmd, "probe") == 0) {
        uint8_t buf[32];
        int f, n;
        uint32_t seq = 1;
        pm_nargs(nrhs, 2, "bytes = ysp_box('probe', family, seq)");
        f = box_family(prhs[1]);
        if (nrhs > 2) seq = (uint32_t)box_uint(prhs[2], "seq", 4294967295.0);
        n = ybox_probe(f, seq, buf, (int)sizeof buf);
        plhs[0] = pm_bytes_out(buf, n > 0 ? (size_t)n : 0);
        return;
    }
    if (strcmp(cmd, "identify") == 0) {
        uint8_t buf[16];
        int n;
        pm_nargs(nrhs, 2, "bytes = ysp_box('identify', family)");
        n = ybox_identify(box_family(prhs[1]), buf, (int)sizeof buf);
        plhs[0] = pm_bytes_out(buf, n > 0 ? (size_t)n : 0);
        return;
    }
    pm_err("usage", "unknown command '%s'", cmd);
}
