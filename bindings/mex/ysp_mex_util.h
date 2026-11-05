/* ysp_mex_util.h - helpers shared by the adaptive-method MEX bindings
 *
 * Not a library header: it is included by ysp_stair.c, ysp_quest.c, ysp_aep.c
 * and ysp_trials.c, once each, after the including file defines PM_MOD (the
 * module name, "ysp_stair" and so on, which prefixes every error id). Every
 * function is static, so each MEX file gets its own copy and its own handle
 * table. Two MEX files never share a table: MATLAB and Octave load them as
 * separate shared libraries, and a table in one is invisible to the other.
 *
 * Indices: every index a caller passes or gets back (a stimulus, a parameter
 * axis, a candidate, a condition, a factor, a level, a track, a trial, a
 * reversal) is 1-based, and 0 means "none" where C uses -1. Outcomes and
 * responses are values, not indices, and keep the header's numbering (1 =
 * correct, 0 = incorrect, 0..K-1 in general).
 */
#ifndef YSP_MEX_UTIL_H
#define YSP_MEX_UTIL_H

#include "mex.h"
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PM_MOD
#error "define PM_MOD before including ysp_mex_util.h"
#endif

/* Each module uses a different subset of these helpers. */
#if defined(__GNUC__) || defined(__clang__)
#define PM_FN static __attribute__((unused))
#else
#define PM_FN static
#endif

/* ---- errors -------------------------------------------------------------- */

/* Raise PM_MOD:<code> with a printf message. The id names the header's code
 * (arg, closed, full, ...) so a script can branch on it with try/catch. */
PM_FN void pm_err(const char* code, const char* fmt, ...) {
    char id[64], msg[512];
    va_list ap;
    snprintf(id, sizeof(id), "%s:%s", PM_MOD, code);
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    mexErrMsgIdAndTxt(id, "%s", msg);
}

/* ---- handle table ------------------------------------------------------ */
/* The handle is a counter, not an address: a freed object's address can come
 * back from malloc for the next open, and a stale pointer handle would then
 * drive someone else's session. Ids are never reused. */
typedef void (*pm_destroy_fn)(void* obj);

typedef struct pm_node {
    uint64_t        id;
    void*           obj;
    struct pm_node* next;
} pm_node;

static pm_node*      pm_list = NULL;
static uint64_t      pm_next_id = 1;
static int           pm_atexit_done = 0;
static pm_destroy_fn pm_destroy = NULL;   /* set by the module before register */

PM_FN void pm_oct_free(void);

PM_FN void pm_cleanup(void) {
    pm_node* n = pm_list;
    pm_oct_free();
    pm_list = NULL;
    /* Octave runs this hook on `clear` but keeps the library loaded, so the
     * statics survive: the next pm_register() must hook again, or the
     * handles opened after the clear are never closed, and a thread they
     * started runs on while Octave unloads the library at exit (a crash,
     * measured 2 of 3 exits). */
    pm_atexit_done = 0;
    while (n) {
        pm_node* next = n->next;
        if (n->obj && pm_destroy) pm_destroy(n->obj);
        free(n);
        n = next;
    }
}

/* Takes ownership of obj. On allocation failure the object is destroyed here,
 * since nothing else could reach it. */
PM_FN uint64_t pm_register(void* obj) {
    pm_node* n = (pm_node*)malloc(sizeof(*n));
    if (!n) {
        if (pm_destroy) pm_destroy(obj);
        pm_err("memory", "out of memory");
    }
    n->id = pm_next_id++;
    n->obj = obj;
    n->next = pm_list;
    pm_list = n;
    if (!pm_atexit_done) { mexAtExit(pm_cleanup); pm_atexit_done = 1; }
    return n->id;
}

PM_FN void pm_unregister(void* obj) {
    pm_node** pp = &pm_list;
    while (*pp) {
        if ((*pp)->obj == obj) { pm_node* dead = *pp; *pp = dead->next; free(dead); return; }
        pp = &(*pp)->next;
    }
}

PM_FN void* pm_lookup(const mxArray* a) {
    uint64_t id;
    pm_node* n;
    if (mxIsUint64(a) && mxGetNumberOfElements(a) == 1)
        id = *(const uint64_t*)mxGetData(a);
    else if (mxIsDouble(a) && !mxIsComplex(a) && mxGetNumberOfElements(a) == 1 && mxGetScalar(a) >= 1)
        id = (uint64_t)mxGetScalar(a);
    else {
        pm_err("handle", "handle must be a scalar uint64");
        return NULL;
    }
    for (n = pm_list; n; n = n->next)
        if (n->id == id) return n->obj;
    pm_err("handle", "invalid or closed handle");
    return NULL;
}

PM_FN mxArray* pm_handle_out(uint64_t id) {
    mxArray* h = mxCreateNumericMatrix(1, 1, mxUINT64_CLASS, mxREAL);
    *(uint64_t*)mxGetData(h) = id;
    return h;
}

/* ---- scalar arguments --------------------------------------------------- */

PM_FN double pm_scalar(const mxArray* a, const char* what) {
    if ((!mxIsNumeric(a) && !mxIsLogical(a)) || mxIsComplex(a) || mxGetNumberOfElements(a) != 1)
        pm_err("arg", "%s must be a real numeric scalar", what);
    return mxGetScalar(a);
}

PM_FN double pm_finite(const mxArray* a, const char* what) {
    double d = pm_scalar(a, what);
    if (!isfinite(d)) pm_err("arg", "%s must be finite", what);
    return d;
}

/* An integer value; fractions are refused rather than truncated, since a
 * silently truncated 2.7 would run a different design. */
PM_FN int pm_int(const mxArray* a, const char* what) {
    double d = pm_scalar(a, what);
    if (!(d == floor(d)) || d < -2147483647.0 || d > 2147483647.0)
        pm_err("arg", "%s must be an integer", what);
    return (int)d;
}

/* 1-based index in 1..n, returned 0-based. */
PM_FN int pm_index(const mxArray* a, const char* what, int n) {
    int i = pm_int(a, what);
    if (i < 1 || i > n) pm_err("arg", "%s must be in 1..%d, got %d", what, n, i);
    return i - 1;
}

PM_FN int pm_bool(const mxArray* a, const char* what) {
    return pm_scalar(a, what) != 0.0;
}

/* The command's argument count check. */
PM_FN void pm_nargs(int nrhs, int need, const char* usage) {
    if (nrhs < need) pm_err("usage", "usage: %s", usage);
}

/* A string argument; mxMalloc'd, released by MATLAB when the call returns. */
PM_FN char* pm_string(const mxArray* a, const char* what) {
    if (!mxIsChar(a)) pm_err("arg", "%s must be a string", what);
    return mxArrayToString(a);
}

/* Map a string to its index in names[]; case-insensitive, so 'Gumbel' and
 * 'gumbel' both work. */
PM_FN int pm_lower_eq(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        if (ca != *b) return 0;
    }
    return *a == 0 && *b == 0;
}

PM_FN int pm_enum(const mxArray* a, const char* what, const char* const* names, int count) {
    char* v;
    int i;
    if (mxIsNumeric(a) && mxGetNumberOfElements(a) == 1) {
        i = pm_int(a, what);
        if (i < 0 || i >= count) pm_err("arg", "%s: value %d out of range", what, i);
        return i;
    }
    v = pm_string(a, what);
    for (i = 0; i < count; i++)
        if (pm_lower_eq(v, names[i])) return i;
    {
        char list[256];
        size_t len = 0;
        list[0] = 0;
        for (i = 0; i < count && len + 1 < sizeof(list); i++)
            len += (size_t)snprintf(list + len, sizeof(list) - len, "%s'%s'", i ? ", " : "", names[i]);
        pm_err("arg", "unknown %s '%s'; expected one of %s", what, v, list);
    }
    return 0;
}

/* ---- struct descs ------------------------------------------------------- */

/* A field of a 1x1 struct, or NULL when it is absent or []: an empty field
 * means "default", as a zero does in the C desc. */
PM_FN const mxArray* pm_field(const mxArray* s, const char* name) {
    const mxArray* f = mxGetField(s, 0, name);
    if (!f || mxIsEmpty(f)) return NULL;
    return f;
}

/* Refuse field names the desc does not have. A typo such as 'stop_trial'
 * would otherwise leave a default in force without a word. */
PM_FN void pm_check_fields(const mxArray* s, const char* const* allowed, int n, const char* what) {
    int i, j, nf;
    if (!mxIsStruct(s) || mxGetNumberOfElements(s) != 1)
        pm_err("arg", "%s must be a 1x1 struct", what);
    nf = mxGetNumberOfFields(s);
    for (i = 0; i < nf; i++) {
        const char* f = mxGetFieldNameByNumber(s, i);
        int ok = 0;
        for (j = 0; j < n; j++) if (strcmp(f, allowed[j]) == 0) { ok = 1; break; }
        if (!ok) pm_err("arg", "%s: unknown field '%s'", what, f);
    }
}

/* Copy a real numeric vector of at most `cap` elements into out[]. Returns
 * the count. */
PM_FN int pm_vector(const mxArray* a, const char* what, double* out, int cap) {
    size_t n, i;
    if (!mxIsNumeric(a) && !mxIsLogical(a)) pm_err("arg", "%s must be numeric", what);
    if (mxIsComplex(a)) pm_err("arg", "%s must be real", what);
    n = mxGetNumberOfElements(a);
    if (n > (size_t)cap) pm_err("arg", "%s has %d elements; at most %d allowed", what, (int)n, cap);
    if (mxIsDouble(a)) {
        const double* p = mxGetPr(a);
        for (i = 0; i < n; i++) out[i] = p[i];
    } else {
        /* Other classes are rare here; go through a converted copy. */
        mxArray* in = (mxArray*)a;
        mxArray* d = NULL;
        mexCallMATLAB(1, &d, 1, &in, "double");
        {
            const double* p = mxGetPr(d);
            for (i = 0; i < n; i++) out[i] = p[i];
        }
        mxDestroyArray(d);
    }
    return (int)n;
}

/* A vector of exactly n doubles (a stimulus, a point). */
PM_FN void pm_vector_n(const mxArray* a, const char* what, double* out, int n) {
    int got = pm_vector(a, what, out, n);
    if (got != n) pm_err("arg", "%s must have %d elements, got %d", what, n, got);
}

PM_FN mxArray* pm_row(const double* v, int n) {
    mxArray* a = mxCreateDoubleMatrix(1, (size_t)(n > 0 ? n : 0), mxREAL);
    if (n > 0) memcpy(mxGetPr(a), v, (size_t)n * sizeof(double));
    return a;
}

PM_FN mxArray* pm_bytes_out(const void* buf, size_t n) {
    mxArray* a = mxCreateNumericMatrix(1, n, mxUINT8_CLASS, mxREAL);
    if (n) memcpy(mxGetData(a), buf, n);
    return a;
}

/* Raw bytes of a uint8 (or int8) array. */
PM_FN const uint8_t* pm_bytes_in(const mxArray* a, const char* what, size_t* n) {
    if (!mxIsUint8(a) && !mxIsInt8(a)) pm_err("arg", "%s must be a uint8 array", what);
    *n = mxGetNumberOfElements(a);
    return (const uint8_t*)mxGetData(a);
}

/* ---- MATLAB callbacks --------------------------------------------------- */
/* A header calls a callback in the middle of its own loops and cannot be
 * stopped there. mexCallMATLAB would longjmp out of the header on an error,
 * leaving the handle half updated, so every callback goes through the trap
 * form: the first exception is kept, the callbacks after it return a neutral
 * value, and pm_rethrow() raises it once the header call has returned and
 * the handle is consistent again. The exception is an ordinary temporary; it
 * lives until the end of this MEX call, which is where it is rethrown. */
PM_FN mxArray* pm_pending = NULL;

#ifdef HAVE_OCTAVE
/* Octave's mexCallMATLABWithTrap reports every failure as Octave:MEX and
 * drops the callback's own identifier and message. So under Octave a callback
 * runs inside cellfun with an ErrorHandler, which hands the original error
 * back as a struct instead of raising it. W calls f(varargin{:}); EH returns
 * the error struct (identifier, message, index). */
PM_FN mxArray* pm_oct_w = NULL;
PM_FN mxArray* pm_oct_eh = NULL;

PM_FN void pm_oct_free(void) {
    if (pm_oct_w) { mxDestroyArray(pm_oct_w); pm_oct_w = NULL; }
    if (pm_oct_eh) { mxDestroyArray(pm_oct_eh); pm_oct_eh = NULL; }
}

PM_FN int pm_feval(int nlhs, mxArray** out, int nrhs, mxArray** in) {
    mxArray* args[16];
    mxArray* res = NULL;
    mxArray* ex;
    const mxArray* el;
    int i, na = 0, ok = 0;
    if (pm_pending) return 0;
    if (nrhs > 10) return 0;
    if (!pm_oct_w) {
        mxArray* s = mxCreateString("@(f, varargin) f(varargin{:})");
        mexCallMATLAB(1, &pm_oct_w, 1, &s, "str2func");
        mexMakeArrayPersistent(pm_oct_w);
        mxDestroyArray(s);
        s = mxCreateString("@(e, varargin) e");
        mexCallMATLAB(1, &pm_oct_eh, 1, &s, "str2func");
        mexMakeArrayPersistent(pm_oct_eh);
        mxDestroyArray(s);
    }
    args[na++] = pm_oct_w;
    for (i = 0; i < nrhs; i++) {
        mxArray* c = mxCreateCellMatrix(1, 1);
        mxSetCell(c, 0, mxDuplicateArray(in[i]));
        args[na++] = c;
    }
    args[na++] = mxCreateString("UniformOutput");
    args[na++] = mxCreateLogicalScalar(false);
    args[na++] = mxCreateString("ErrorHandler");
    args[na++] = pm_oct_eh;
    ex = mexCallMATLABWithTrap(1, &res, na, args, "cellfun");
    for (i = 1; i < na - 1; i++) mxDestroyArray(args[i]);
    if (ex) {
        pm_pending = ex;
    } else if (res && mxIsCell(res) && mxGetNumberOfElements(res) == 1) {
        el = mxGetCell(res, 0);
        if (el && mxIsStruct(el) && mxGetField(el, 0, "index") && mxGetField(el, 0, "identifier")) {
            pm_pending = mxDuplicateArray(el);
        } else {
            if (nlhs >= 1) out[0] = el ? mxDuplicateArray(el) : NULL;
            ok = 1;
        }
    }
    if (res) mxDestroyArray(res);
    return ok;
}
#else
PM_FN void pm_oct_free(void) {}

PM_FN int pm_feval(int nlhs, mxArray** out, int nrhs, mxArray** in) {
    mxArray* ex;
    if (pm_pending) return 0;      /* one error per call is enough */
    ex = mexCallMATLABWithTrap(nlhs, out, nrhs, in, "feval");
    if (ex) { pm_pending = ex; return 0; }
    return 1;
}
#endif

/* Raise the callback's own identifier and message. rethrow() called through
 * mexCallMATLAB would wrap it as MATLAB:MException:rethrow:uncaughtException
 * and hide the id a script branches on. */
PM_FN void pm_rethrow(void) {
    if (pm_pending) {
        mxArray* e = pm_pending;
        const mxArray* idp;
        const mxArray* msgp;
        char id[128] = "";
        char msg[512] = "error in a MATLAB callback";
        pm_pending = NULL;
        idp = mxIsStruct(e) ? mxGetField(e, 0, "identifier") : mxGetProperty(e, 0, "identifier");
        msgp = mxIsStruct(e) ? mxGetField(e, 0, "message") : mxGetProperty(e, 0, "message");
        if (idp && mxIsChar(idp)) mxGetString(idp, id, sizeof(id));
        if (msgp && mxIsChar(msgp)) mxGetString(msgp, msg, sizeof(msg));
        if (!id[0]) snprintf(id, sizeof(id), "%s:callback", PM_MOD);
        mexErrMsgIdAndTxt(id, "%s", msg);
    }
}

/* A generator: either a MATLAB function handle returning a uniform in [0, 1),
 * or an integer seed turned into a splitmix64 state the module keeps. The
 * seed form never enters the interpreter, so an async layer can use it. */
typedef struct pm_rng {
    mxArray* fn;       /* persistent function handle, or NULL */
    uint64_t state;    /* splitmix64 state when fn is NULL and has_seed */
    int      has_seed;
} pm_rng;

/* The same splitmix64 step as ytr_splitmix(), so a seed means the same
 * stream in every module. */
PM_FN double pm_splitmix(void* ctx) {
    uint64_t* s = (uint64_t*)ctx;
    uint64_t z;
    *s += 0x9E3779B97F4A7C15ULL;
    z = *s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}

PM_FN double pm_rng_call(void* ctx) {
    pm_rng* r = (pm_rng*)ctx;
    mxArray* out = NULL;
    double u;
    if (!r->fn) return pm_splitmix(&r->state);
    if (!pm_feval(1, &out, 1, &r->fn)) return 0.0;
    if (!out || !mxIsNumeric(out) || mxGetNumberOfElements(out) < 1) {
        if (out) mxDestroyArray(out);
        return 0.0;
    }
    u = mxGetScalar(out);
    mxDestroyArray(out);
    return u;
}

/* A seed or a state: a uint64 scalar exactly, or a double holding an integer
 * below 2^53. */
PM_FN uint64_t pm_u64(const mxArray* a, const char* what) {
    double d;
    if (mxIsUint64(a) && mxGetNumberOfElements(a) == 1) return *(const uint64_t*)mxGetData(a);
    if (mxIsInt64(a) && mxGetNumberOfElements(a) == 1) return (uint64_t)*(const int64_t*)mxGetData(a);
    d = pm_scalar(a, what);
    if (d < 0 || d != floor(d) || d > 9007199254740992.0)
        pm_err("arg", "%s must be a uint64, or a nonnegative integer below 2^53", what);
    return (uint64_t)d;
}

PM_FN mxArray* pm_u64_out(uint64_t v) {
    mxArray* a = mxCreateNumericMatrix(1, 1, mxUINT64_CLASS, mxREAL);
    *(uint64_t*)mxGetData(a) = v;
    return a;
}

/* Parse desc.rng: a function handle, or a seed. Returns 1 when a generator
 * was given. */
PM_FN int pm_rng_parse(const mxArray* a, pm_rng* r) {
    r->fn = NULL;
    r->has_seed = 0;
    r->state = 0;
    if (!a) return 0;
    if (mxIsClass(a, "function_handle")) {
        r->fn = mxDuplicateArray(a);
        mexMakeArrayPersistent(r->fn);
        return 1;
    }
    r->state = pm_u64(a, "rng");
    r->has_seed = 1;
    return 1;
}

PM_FN void pm_rng_free(pm_rng* r) {
    if (r->fn) { mxDestroyArray(r->fn); r->fn = NULL; }
}

#endif /* YSP_MEX_UTIL_H */
