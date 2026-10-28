/* ysp_rdk_ext.c - CPython extension wrapping ysp/rdk.h (module ysp.rdk)
 *
 * The binding an analysis uses to regenerate a logged trial's dots bit for
 * bit: the same desc as keyword arguments, start and update, replay from
 * the logged inputs (rdk.last records), the whole trial's positions in one
 * call, snapshots, and the outputs as NumPy arrays. Built against the
 * stable ABI / Limited API (CPython 3.10+): the type is a heap type made
 * with PyType_FromSpec, and an output is a bytearray that numpy.frombuffer
 * views (no per-dot Python work). NumPy is imported on the first output.
 *
 *     import ysp.rdk as rdk
 *     f = rdk.Field(algorithm="MN", w=10, count=100, coherence=0.256, speed=5)
 *     f.start(seed, t0)                             # t0 in seconds
 *     for t in onsets: f.update(t); xy = f.xy()     # (n, 2) float32
 *     xy, sig = f.trajectory(seed, t0, steps)       # the whole trial
 *
 * TIMES: seconds (double), as every binding (docs/bindings.md). The header
 * counts int64 ns, and a replay is exact only if the ns are, so each time
 * converts once at the boundary, to the nearest ns (rdk_s_to_ns). Step rows
 * carry the exact ns beside the seconds.
 *
 * THREADING: none. A Field is used under the GIL; trajectory() and
 * replay() release it while the C loop runs, as the field is the object's
 * own.
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x030A0000
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define YSP_RDK_IMPLEMENTATION
#include "ysp/rdk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The distribution's version is the header's. A change to this binding
 * alone adds a fourth number (0.2.0.1 was times in seconds), which goes
 * when the header's version moves; tests/test_rdk.py checks that this and
 * pyproject.toml agree. */
#define RDK_VERSION YRDK_VERSION_STRING ".1"   /* .1: the CPython 3.10 floor (2026-10-27) */

/* The largest time in seconds that converts back to its ns exactly. With
 * s the double nearest n / 1e9, |s 1e9 - n| <= 1e9 ulp(s) / 2, which is
 * below 0.5 while ulp(s) <= 2^-30, that is s < 2^23 s (about 97 days of a
 * clock's count; 2^53 ns, where a double stops holding every ns, is about
 * 104 days). rdk_s_to_ns() rounds the exact product, so that bound is the
 * only one. A time at or above it is refused, not rounded: a replay off by
 * one ns is not the trial. It also refuses an old ns integer passed as
 * seconds (1e9 "s" is 31 years). */
#define RDK_S_MAX 8388608.0

static PyObject* RdkError;
static PyObject* RdkArgumentError;
static PyObject* RdkOrderError;
static PyObject* StepType;   /* namedtuple Step(t, coherence, direction, speed, set, ns) */
static PyObject* numpy_mod;  /* imported on the first output */

static PyObject* rdk_fail(int code, const char* msg) {
    PyObject* exc = RdkError;
    if (code == YRDK_ERR_ARG) exc = RdkArgumentError;
    else if (code == YRDK_ERR_ORDER) exc = RdkOrderError;
    else if (code == YRDK_ERR_MEM) exc = PyExc_MemoryError;
    PyErr_SetString(exc, msg && msg[0] ? msg : yrdk_strerror(code));
    return NULL;
}

/* Seconds to the nearest ns. s * 1e9 rounds once; fma() gives that
 * rounding's error exactly, so the result is the nearest ns to the real
 * product, and n / 1e9 -> s -> n holds for every n below RDK_S_MAX s.
 * Refuses NaN, a negative time and a time at or above RDK_S_MAX. */
static int rdk_s_to_ns(double s, const char* what, int64_t* out) {
    volatile double p;   /* stored as a double: x87 (i686) would keep 80 bits */
    double e, n, r;
    if (!(s >= 0.0 && s < RDK_S_MAX)) {
        PyErr_Format(RdkArgumentError, "%s: seconds, >= 0 and < 8388608 (2^23 s, about 97 days)", what);
        return -1;
    }
    p = s * 1e9;
    e = fma(s, 1e9, -p);
    n = floor(p + 0.5);   /* within 1 of p; the step below corrects either side */
    r = (p - n) + e;      /* p - n is exact (Sterbenz) */
    if (r > 0.5) n += 1.0;
    else if (r < -0.5) n -= 1.0;
    *out = (int64_t)n;
    return 0;
}

/* A Python number of seconds to ns. */
static int rdk_obj_to_ns(PyObject* o, const char* what, int64_t* out) {
    double s = PyFloat_AsDouble(o);
    if (s == -1.0 && PyErr_Occurred()) return -1;
    return rdk_s_to_ns(s, what, out);
}

/* ns to the double nearest ns / 1e9: the division is correctly rounded,
 * and (double)ns is exact below 2^53. */
static double rdk_ns_to_s(int64_t ns) { return (double)ns / 1e9; }

/* --- names ------------------------------------------------------------------ */

typedef struct rdk_name { const char* name; int value; } rdk_name;

static const rdk_name ALGORITHMS[] = { { "custom", YRDK_CUSTOM }, { "WN", YRDK_WN }, { "MN", YRDK_MN },
                                       { "LL", YRDK_LL }, { "BM", YRDK_BM }, { NULL, 0 } };
static const rdk_name SIGNALS[] = { { "same", YRDK_SIGNAL_SAME }, { "different", YRDK_SIGNAL_DIFFERENT },
                                    { "least_recent", YRDK_SIGNAL_LEAST_RECENT }, { NULL, 0 } };
static const rdk_name SELECTS[] = { { "exact", YRDK_EXACT }, { "bernoulli", YRDK_BERNOULLI }, { NULL, 0 } };
static const rdk_name NOISES[] = { { "direction", YRDK_NOISE_DIRECTION }, { "position", YRDK_NOISE_POSITION },
                                   { "walk", YRDK_NOISE_WALK }, { NULL, 0 } };
static const rdk_name SHAPES[] = { { "circle", YRDK_CIRCLE }, { "rect", YRDK_RECT }, { NULL, 0 } };
static const rdk_name EDGES[] = { { "wrap", YRDK_WRAP }, { "replot", YRDK_REPLOT }, { NULL, 0 } };
static const rdk_name CLOCKS[] = { { "onset", YRDK_CLOCK_ONSET }, { "frames", YRDK_CLOCK_FRAMES }, { NULL, 0 } };

/* A str's UTF-8 into buf (PyUnicode_AsUTF8 is Limited API only from 3.10). */
static int rdk_utf8(PyObject* o, char* buf, size_t cap) {
    PyObject* b = PyUnicode_AsUTF8String(o);
    if (!b) return -1;
    PyOS_snprintf(buf, cap, "%s", PyBytes_AsString(b));
    Py_DECREF(b);
    return 0;
}

static int rdk_ieq(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* An enum field: None (0), an int, or one of the names (any case). */
static int rdk_enum(PyObject* o, const rdk_name* names, const char* field, int* out) {
    if (!o || o == Py_None) { *out = 0; return 0; }
    if (PyLong_Check(o)) {
        long v = PyLong_AsLong(o);
        if (v == -1 && PyErr_Occurred()) return -1;
        *out = (int)v;
        return 0;
    }
    if (PyUnicode_Check(o)) {
        char s[64];
        int i;
        if (rdk_utf8(o, s, sizeof s) < 0) return -1;
        for (i = 0; names[i].name; i++)
            if (rdk_ieq(s, names[i].name)) { *out = names[i].value; return 0; }
    }
    PyErr_Format(RdkArgumentError, "%s: not one of its names or values", field);
    return -1;
}

/* --- the Field type ------------------------------------------------------------ */

typedef struct {
    PyObject_HEAD
    yrdk_field f;
    yrdk_desc d;    /* the desc it opened with, for logs */
    int open;
} FieldObject;

static void Field_dealloc(PyObject* self) {
    FieldObject* o = (FieldObject*)self;
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    if (o->open) yrdk_close(&o->f);
    o->open = 0;
    fr(self);
    Py_DECREF(tp);
}

static int Field_init(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "algorithm", "aperture", "w", "h", "count", "density", "coherence", "direction", "speed",
                              "signal", "select", "noise", "edge", "sets", "lifetime", "lifetime_frames", "clock",
                              "frame_period", "seed", "stream", NULL };
    FieldObject* o = (FieldObject*)self;
    PyObject *alg = NULL, *ap = NULL, *sig = NULL, *sel = NULL, *noi = NULL, *edg = NULL, *clk = NULL, *fp = NULL;
    double w = 0, h = 0, density = 0, coherence = 0, direction = 0, speed = 0, lifetime = 0;
    int count = 0, sets = 0, lifetime_frames = 0, v, rc;
    unsigned long long seed = 0;
    unsigned long stream = 0;
    yrdk_desc d;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|$OOddiddddOOOOidiOOKk", kwlist, &alg, &ap, &w, &h, &count, &density,
                                     &coherence, &direction, &speed, &sig, &sel, &noi, &edg, &sets, &lifetime,
                                     &lifetime_frames, &clk, &fp, &seed, &stream))
        return -1;
    memset(&d, 0, sizeof d);
    if (rdk_enum(alg, ALGORITHMS, "algorithm", &v) < 0) return -1;
    d.algorithm = (yrdk_algorithm)v;
    if (rdk_enum(ap, SHAPES, "aperture", &v) < 0) return -1;
    d.aperture = (yrdk_shape)v;
    if (rdk_enum(sig, SIGNALS, "signal", &v) < 0) return -1;
    d.signal = (yrdk_signal)v;
    if (rdk_enum(sel, SELECTS, "select", &v) < 0) return -1;
    d.select = (yrdk_select)v;
    if (rdk_enum(noi, NOISES, "noise", &v) < 0) return -1;
    d.noise = (yrdk_noise)v;
    if (rdk_enum(edg, EDGES, "edge", &v) < 0) return -1;
    d.edge = (yrdk_edge)v;
    if (rdk_enum(clk, CLOCKS, "clock", &v) < 0) return -1;
    d.clock = (yrdk_clock)v;
    /* The desc's floats are float32: a Python float rounds to the nearest,
     * as a C float literal does, so the same numbers give the same field. */
    d.w = (float)w; d.h = (float)h; d.count = count; d.density = (float)density;
    d.coherence = (float)coherence; d.direction = (float)direction; d.speed = (float)speed;
    d.sets = sets; d.lifetime_frames = lifetime_frames;
    /* lifetime stays seconds here. ysp/rdk.h v0.2 takes ns; the float32
     * rounding and llround of v0.1 keep every field (and the digests that
     * tests/compare/rdk_ref.py reproduces) as they were. */
    {
        float life = (float)lifetime;
        if (!(life >= 0.0f && life <= 1e9f)) {
            PyErr_SetString(RdkArgumentError, "lifetime: seconds, > 0");
            return -1;
        }
        d.lifetime_ns = (int64_t)llround((double)life * 1e9);
        if (life > 0.0f && d.lifetime_ns < 1) {
            PyErr_SetString(RdkArgumentError, "lifetime: shorter than 1 ns");
            return -1;
        }
    }
    if (fp && fp != Py_None && rdk_obj_to_ns(fp, "frame_period", &d.frame_ns) < 0) return -1;
    d.seed = (uint64_t)seed; d.stream = (uint32_t)stream;
    if (o->open) yrdk_close(&o->f);
    o->open = 0;
    rc = yrdk_open(&o->f, &d) ? YRDK_OK : strstr(yrdk_error(&o->f), "allocation") ? YRDK_ERR_MEM : YRDK_ERR_ARG;
    if (rc < 0) { rdk_fail(rc, yrdk_error(&o->f)); return -1; }
    o->d = d;
    o->open = 1;
    return 0;
}

static int Field_ready(FieldObject* o) {
    if (!o->open) { PyErr_SetString(RdkError, "the field is not open"); return 0; }
    return 1;
}

static PyObject* Field_start(PyObject* self, PyObject* args) {
    FieldObject* o = (FieldObject*)self;
    unsigned long long seed;
    double s;
    int64_t t;
    int rc;
    if (!PyArg_ParseTuple(args, "Kd", &seed, &s) || !Field_ready(o)) return NULL;
    if (rdk_s_to_ns(s, "t", &t) < 0) return NULL;
    rc = yrdk_start(&o->f, (uint64_t)seed, t);
    if (rc < 0) return rdk_fail(rc, yrdk_error(&o->f));
    Py_RETURN_NONE;
}

static PyObject* Field_update(PyObject* self, PyObject* arg) {
    FieldObject* o = (FieldObject*)self;
    int64_t t;
    int rc;
    if (!Field_ready(o) || rdk_obj_to_ns(arg, "t", &t) < 0) return NULL;
    rc = yrdk_update(&o->f, t);
    if (rc < 0) return rdk_fail(rc, yrdk_error(&o->f));
    return PyLong_FromLong(rc);
}

/* steps: an iterable of rows (t, coherence, direction, speed[, set[, ns]]),
 * such as Field.last records, into a C array the caller frees. t is
 * seconds. A row with ns (not None) uses it, so a log replays exactly at any
 * magnitude, and its t must be the double nearest ns / 1e9, as Field.last
 * gives it: a row whose two times disagree is refused, not resolved. */
static yrdk_step* rdk_steps(PyObject* seq, Py_ssize_t* n_out) {
    PyObject* list = PySequence_List(seq);
    yrdk_step* s;
    Py_ssize_t n, i;
    if (!list) return NULL;
    n = PyList_Size(list);
    s = (yrdk_step*)malloc(sizeof *s * (size_t)(n > 0 ? n : 1));
    if (!s) { Py_DECREF(list); PyErr_NoMemory(); return NULL; }
    for (i = 0; i < n; i++) {
        PyObject* row = PySequence_Tuple(PyList_GetItem(list, i));   /* a tuple, a Step or a NumPy record */
        if (!row) goto fail;
        if (PyTuple_Size(row) < 4) {
            Py_DECREF(row);
            PyErr_Format(RdkArgumentError, "step %zd: needs t, coherence, direction, speed", i);
            goto fail;
        }
        if (PyTuple_Size(row) >= 6 && PyTuple_GetItem(row, 5) != Py_None) {
            double ts = PyFloat_AsDouble(PyTuple_GetItem(row, 0));
            s[i].t = (int64_t)PyLong_AsLongLong(PyTuple_GetItem(row, 5));
            if (!PyErr_Occurred() && (s[i].t < 0 || ts != rdk_ns_to_s(s[i].t))) {
                Py_DECREF(row);
                PyErr_Format(RdkArgumentError, "step %zd: t is not ns / 1e9 (or ns < 0)", i);
                goto fail;
            }
        } else {
            char what[32];
            PyOS_snprintf(what, sizeof what, "step %d: t", (int)i);
            if (rdk_obj_to_ns(PyTuple_GetItem(row, 0), what, &s[i].t) < 0) { Py_DECREF(row); goto fail; }
        }
        s[i].coherence = (float)PyFloat_AsDouble(PyTuple_GetItem(row, 1));
        s[i].direction = (float)PyFloat_AsDouble(PyTuple_GetItem(row, 2));
        s[i].speed = (float)PyFloat_AsDouble(PyTuple_GetItem(row, 3));
        s[i].set = 0;
        Py_DECREF(row);
        if (PyErr_Occurred()) goto fail;
    }
    Py_DECREF(list);
    *n_out = n;
    return s;
fail:
    Py_DECREF(list);
    free(s);
    return NULL;
}

static PyObject* Field_replay(PyObject* self, PyObject* args) {
    FieldObject* o = (FieldObject*)self;
    unsigned long long seed;
    double t0s;
    int64_t t0;
    PyObject* seq;
    yrdk_step* s;
    Py_ssize_t n;
    int rc;
    if (!PyArg_ParseTuple(args, "KdO", &seed, &t0s, &seq) || !Field_ready(o)) return NULL;
    if (rdk_s_to_ns(t0s, "t0", &t0) < 0 || !(s = rdk_steps(seq, &n))) return NULL;
    Py_BEGIN_ALLOW_THREADS
    rc = yrdk_replay(&o->f, (uint64_t)seed, t0, s, (int)n);
    Py_END_ALLOW_THREADS
    free(s);
    if (rc < 0) return rdk_fail(rc, yrdk_error(&o->f));
    Py_RETURN_NONE;
}

/* --- outputs: bytearray -> numpy.frombuffer ------------------------------------ */

static PyObject* rdk_array(PyObject* ba, const char* dtype, Py_ssize_t rows, Py_ssize_t cols, Py_ssize_t pages) {
    PyObject *fb, *arr, *shaped;
    if (!ba) return NULL;
    if (!numpy_mod && !(numpy_mod = PyImport_ImportModule("numpy"))) { Py_DECREF(ba); return NULL; }
    fb = PyObject_GetAttrString(numpy_mod, "frombuffer");
    arr = fb ? PyObject_CallFunction(fb, "Os", ba, dtype) : NULL;
    Py_XDECREF(fb);
    Py_DECREF(ba);
    if (!arr) return NULL;
    if (pages > 0) shaped = PyObject_CallMethod(arr, "reshape", "nnn", pages, rows, cols);
    else if (cols > 0) shaped = PyObject_CallMethod(arr, "reshape", "nn", rows, cols);
    else return arr;
    Py_DECREF(arr);
    return shaped;
}

/* One output of the shown set into a new array. */
static PyObject* Field_out(FieldObject* o, int which) {
    Py_ssize_t n;
    PyObject* ba;
    yrdk_out out;
    char* p;
    if (!Field_ready(o)) return NULL;
    if (!o->f.started_) return rdk_fail(YRDK_ERR_ORDER, "no output before start()");
    n = o->f.n;
    memset(&out, 0, sizeof out);
    ba = PyByteArray_FromStringAndSize(NULL, n * (which == 0 ? 8 : which >= 3 ? 1 : 4));
    if (!ba) return NULL;
    p = PyByteArray_AsString(ba);
    if (which == 0) out.xy = (float*)(void*)p;
    else if (which == 1) out.dir = (float*)(void*)p;
    else if (which == 2) out.age_s = (float*)(void*)p;
    else if (which == 3) out.signal = (uint8_t*)p;
    else out.event = (uint8_t*)p;
    yrdk_write(&o->f, &out);
    if (which == 0) return rdk_array(ba, "float32", n, 2, 0);
    return rdk_array(ba, which >= 3 ? "uint8" : "float32", n, 0, 0);
}

static PyObject* Field_xy(PyObject* self, PyObject* unused) { (void)unused; return Field_out((FieldObject*)self, 0); }
static PyObject* Field_dir(PyObject* self, PyObject* unused) { (void)unused; return Field_out((FieldObject*)self, 1); }
static PyObject* Field_age(PyObject* self, PyObject* unused) { (void)unused; return Field_out((FieldObject*)self, 2); }
static PyObject* Field_signal(PyObject* self, PyObject* unused) { (void)unused; return Field_out((FieldObject*)self, 3); }
static PyObject* Field_event(PyObject* self, PyObject* unused) { (void)unused; return Field_out((FieldObject*)self, 4); }

/* trajectory(seed, t0, steps) -> (xy, direction, signal): start, then each
 * step; after each update the shown set's outputs go into row k of
 * (k, n, 2), (k, n) and (k, n) arrays. One C loop for the whole trial. */
static PyObject* Field_trajectory(PyObject* self, PyObject* args) {
    FieldObject* o = (FieldObject*)self;
    unsigned long long seed;
    double t0s;
    int64_t t0;
    PyObject *seq, *bxy = NULL, *bdir = NULL, *bsig = NULL, *axy, *adir, *asig;
    yrdk_step* s;
    Py_ssize_t n, k, nd;
    int rc = 0;
    if (!PyArg_ParseTuple(args, "KdO", &seed, &t0s, &seq) || !Field_ready(o)) return NULL;
    if (rdk_s_to_ns(t0s, "t0", &t0) < 0 || !(s = rdk_steps(seq, &n))) return NULL;
    nd = o->f.n;
    bxy = PyByteArray_FromStringAndSize(NULL, n * nd * 8);
    bdir = PyByteArray_FromStringAndSize(NULL, n * nd * 4);
    bsig = PyByteArray_FromStringAndSize(NULL, n * nd);
    if (!bxy || !bdir || !bsig) { free(s); Py_XDECREF(bxy); Py_XDECREF(bdir); Py_XDECREF(bsig); return NULL; }
    {
        char *pxy = PyByteArray_AsString(bxy), *pdir = PyByteArray_AsString(bdir), *psig = PyByteArray_AsString(bsig);
        Py_BEGIN_ALLOW_THREADS
        rc = yrdk_start(&o->f, (uint64_t)seed, t0);
        for (k = 0; k < n && rc >= 0; k++) {
            yrdk_out out;
            o->f.coherence = s[k].coherence;
            o->f.direction = s[k].direction;
            o->f.speed = s[k].speed;
            rc = yrdk_update(&o->f, s[k].t);
            if (rc < 0) break;
            memset(&out, 0, sizeof out);
            out.xy = (float*)(void*)(pxy + (size_t)k * (size_t)nd * 8);
            out.dir = (float*)(void*)(pdir + (size_t)k * (size_t)nd * 4);
            out.signal = (uint8_t*)(psig + (size_t)k * (size_t)nd);
            yrdk_write(&o->f, &out);
        }
        Py_END_ALLOW_THREADS
    }
    free(s);
    if (rc < 0) { Py_DECREF(bxy); Py_DECREF(bdir); Py_DECREF(bsig); return rdk_fail(rc, yrdk_error(&o->f)); }
    axy = rdk_array(bxy, "float32", nd, 2, n);
    adir = rdk_array(bdir, "float32", n, nd, 0);
    asig = rdk_array(bsig, "uint8", n, nd, 0);
    if (!axy || !adir || !asig) { Py_XDECREF(axy); Py_XDECREF(adir); Py_XDECREF(asig); return NULL; }
    return Py_BuildValue("(NNN)", axy, adir, asig);
}

static PyObject* Field_digest(PyObject* self, PyObject* unused) {
    FieldObject* o = (FieldObject*)self;
    (void)unused;
    if (!Field_ready(o)) return NULL;
    return PyLong_FromUnsignedLongLong(yrdk_digest(&o->f));
}

static PyObject* Field_snapshot(PyObject* self, PyObject* unused) {
    FieldObject* o = (FieldObject*)self;
    size_t n;
    PyObject* b;
    int rc;
    (void)unused;
    if (!Field_ready(o)) return NULL;
    n = yrdk_snapshot_bytes(&o->f);
    b = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)n);
    if (!b) return NULL;
    rc = yrdk_snapshot(&o->f, PyBytes_AsString(b), n);
    if (rc < 0) { Py_DECREF(b); return rdk_fail(rc, yrdk_error(&o->f)); }
    return b;
}

static PyObject* Field_restore(PyObject* self, PyObject* arg) {
    FieldObject* o = (FieldObject*)self;
    char* p;
    Py_ssize_t n;
    int rc;
    if (!Field_ready(o)) return NULL;
    if (!PyBytes_Check(arg)) { PyErr_SetString(RdkArgumentError, "restore: the bytes snapshot() gave"); return NULL; }
    if (PyBytes_AsStringAndSize(arg, &p, &n) < 0) return NULL;
    rc = yrdk_restore(&o->f, p, (size_t)n);
    if (rc < 0) return rdk_fail(rc, yrdk_error(&o->f));
    Py_RETURN_NONE;
}

/* --- attributes ---------------------------------------------------------------- */

static PyObject* get_float(FieldObject* o, float v) { return Field_ready(o) ? PyFloat_FromDouble((double)v) : NULL; }
static int set_float(FieldObject* o, PyObject* v, float* dst) {
    double x;
    if (!Field_ready(o)) return -1;
    if (!v) { PyErr_SetString(PyExc_AttributeError, "cannot delete"); return -1; }
    x = PyFloat_AsDouble(v);
    if (x == -1.0 && PyErr_Occurred()) return -1;
    *dst = (float)x;
    return 0;
}

static PyObject* Field_get_coherence(PyObject* s, void* c) { (void)c; return get_float((FieldObject*)s, ((FieldObject*)s)->f.coherence); }
static PyObject* Field_get_direction(PyObject* s, void* c) { (void)c; return get_float((FieldObject*)s, ((FieldObject*)s)->f.direction); }
static PyObject* Field_get_speed(PyObject* s, void* c) { (void)c; return get_float((FieldObject*)s, ((FieldObject*)s)->f.speed); }
static int Field_set_coherence(PyObject* s, PyObject* v, void* c) { (void)c; return set_float((FieldObject*)s, v, &((FieldObject*)s)->f.coherence); }
static int Field_set_direction(PyObject* s, PyObject* v, void* c) { (void)c; return set_float((FieldObject*)s, v, &((FieldObject*)s)->f.direction); }
static int Field_set_speed(PyObject* s, PyObject* v, void* c) { (void)c; return set_float((FieldObject*)s, v, &((FieldObject*)s)->f.speed); }

static PyObject* Field_get_n(PyObject* s, void* c) {
    (void)c;
    return Field_ready((FieldObject*)s) ? PyLong_FromLong(((FieldObject*)s)->f.n) : NULL;
}
static PyObject* Field_get_total(PyObject* s, void* c) {
    (void)c;
    return Field_ready((FieldObject*)s) ? PyLong_FromLong(((FieldObject*)s)->f.total) : NULL;
}

static PyObject* Field_get_last(PyObject* s, void* c) {
    FieldObject* o = (FieldObject*)s;
    const yrdk_step* l = &o->f.last;
    (void)c;
    if (!Field_ready(o)) return NULL;
    return PyObject_CallFunction(StepType, "dfffiL", rdk_ns_to_s(l->t), (double)l->coherence, (double)l->direction,
                                 (double)l->speed, (int)l->set, (long long)l->t);
}

static PyObject* Field_get_stats(PyObject* s, void* c) {
    FieldObject* o = (FieldObject*)s;
    const yrdk_stats* st = &o->f.stats;
    (void)c;
    if (!Field_ready(o)) return NULL;
    return Py_BuildValue("{s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K}", "updates", (unsigned long long)st->updates,
                         "signal", (unsigned long long)st->signal, "wraps", (unsigned long long)st->wraps,
                         "replots", (unsigned long long)st->replots, "deaths", (unsigned long long)st->deaths,
                         "placed", (unsigned long long)st->placed, "exhausted", (unsigned long long)st->exhausted,
                         "stuck", (unsigned long long)st->stuck);
}

static PyMethodDef Field_methods[] = {
    { "start", Field_start, METH_VARARGS, "start(seed, t): a trial; every dot placed from seed, the clock at t (seconds)." },
    { "update", Field_update, METH_O, "update(t) -> n: the next set at time t (seconds), with the field's coherence, "
                                      "direction and speed." },
    { "replay", Field_replay, METH_VARARGS, "replay(seed, t0, steps): start(seed, t0), then each step's coherence, "
                                            "direction and speed and update(t). t0 in seconds; steps: rows such as Field.last." },
    { "trajectory", Field_trajectory, METH_VARARGS, "trajectory(seed, t0, steps) -> (xy, direction, signal): replay, "
                                                    "with the shown set after each step: (k, n, 2) float32, (k, n) "
                                                    "float32, (k, n) uint8." },
    { "xy", Field_xy, METH_NOARGS, "xy() -> (n, 2) float32: the shown set, units from the center, y down." },
    { "directions", Field_dir, METH_NOARGS, "directions() -> (n,) float32: degrees, clockwise on the screen." },
    { "ages", Field_age, METH_NOARGS, "ages() -> (n,) float32: seconds since the last birth (or start)." },
    { "signals", Field_signal, METH_NOARGS, "signals() -> (n,) uint8: 1 for a signal dot on the last update." },
    { "events", Field_event, METH_NOARGS, "events() -> (n,) uint8: EV_SIGNAL, EV_WRAPPED and EV_PLACED bits." },
    { "digest", Field_digest, METH_NOARGS, "digest() -> int: FNV-1a 64 of the state (the header's DIGEST)." },
    { "snapshot", Field_snapshot, METH_NOARGS, "snapshot() -> bytes: the state after an update." },
    { "restore", Field_restore, METH_O, "restore(bytes): a snapshot of a field opened with the same desc." },
    { NULL }
};

static PyGetSetDef Field_getset[] = {
    { "coherence", Field_get_coherence, Field_set_coherence, "0..1, read at each update (float32)", NULL },
    { "direction", Field_get_direction, Field_set_direction, "degrees, clockwise on the screen (float32)", NULL },
    { "speed", Field_get_speed, Field_set_speed, "units per second (float32)", NULL },
    { "n", Field_get_n, NULL, "dots shown per update", NULL },
    { "total", Field_get_total, NULL, "dots in the field (n x sets)", NULL },
    { "last", Field_get_last, NULL, "Step(t, coherence, direction, speed, set, ns): the last update's inputs; t in seconds, ns its exact integer", NULL },
    { "stats", Field_get_stats, NULL, "counts since start", NULL },
    { NULL }
};

static PyType_Slot Field_slots[] = {
    { Py_tp_init, (void*)Field_init }, { Py_tp_dealloc, (void*)Field_dealloc }, { Py_tp_methods, Field_methods },
    { Py_tp_getset, Field_getset }, { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_doc, (void*)"Field(**desc): one random dot kinematogram (yrdk_field). The keyword arguments are "
                        "yrdk_desc's fields; enums take their names (\"MN\", \"walk\", \"rect\") or values. Times are "
                        "seconds: lifetime, frame_period (desc.frame_ns)." },
    { 0, NULL }
};

static PyType_Spec Field_spec = { "ysp.rdk.Field", sizeof(FieldObject), 0, Py_TPFLAGS_DEFAULT, Field_slots };

/* --- module functions ---------------------------------------------------------- */

static PyObject* mod_philox(PyObject* m, PyObject* args) {
    unsigned long long seed;
    unsigned long c[4];
    uint32_t ctr[4], out[4];
    (void)m;
    if (!PyArg_ParseTuple(args, "K(kkkk)", &seed, &c[0], &c[1], &c[2], &c[3])) return NULL;
    ctr[0] = (uint32_t)c[0]; ctr[1] = (uint32_t)c[1]; ctr[2] = (uint32_t)c[2]; ctr[3] = (uint32_t)c[3];
    yrdk_philox((uint64_t)seed, ctr, out);
    return Py_BuildValue("(kkkk)", (unsigned long)out[0], (unsigned long)out[1], (unsigned long)out[2], (unsigned long)out[3]);
}

static PyObject* mod_angle(PyObject* m, PyObject* arg) {
    double d = PyFloat_AsDouble(arg);
    (void)m;
    if (d == -1.0 && PyErr_Occurred()) return NULL;
    return PyLong_FromUnsignedLong((unsigned long)yrdk_angle((float)d));
}

static PyObject* mod_sincos(PyObject* m, PyObject* arg) {
    unsigned long a = PyLong_AsUnsignedLongMask(arg);
    int32_t s, c;
    (void)m;
    if (PyErr_Occurred()) return NULL;
    yrdk_sincos((uint32_t)a, &s, &c);
    return Py_BuildValue("(ll)", (long)s, (long)c);
}

static PyObject* mod_version(PyObject* m, PyObject* unused) {
    (void)m; (void)unused;
    return PyUnicode_FromString(yrdk_version());
}

static PyMethodDef module_methods[] = {
    { "philox", mod_philox, METH_VARARGS, "philox(seed, (c0, c1, c2, c3)) -> 4 words: Philox4x32-10, the header's generator." },
    { "angle", mod_angle, METH_O, "angle(deg) -> int: the binary angle of a float32 direction (REPRODUCIBILITY)." },
    { "sincos", mod_sincos, METH_O, "sincos(angle) -> (s, c): Q30 sine and cosine of a binary angle." },
    { "version", mod_version, METH_NOARGS, "version() -> the header's version string." },
    { NULL }
};

static PyModuleDef ysp_rdk_module = {
    PyModuleDef_HEAD_INIT, "ysp.rdk",
    "Random dot kinematograms (ysp/rdk.h): regenerate a logged trial bit for bit.", -1, module_methods,
    NULL, NULL, NULL, NULL,
};

static PyObject* rdk_add_exc(PyObject* m, const char* qual, const char* attr, PyObject* base) {
    PyObject* e = PyErr_NewException(qual, base, NULL);
    if (!e) return NULL;
    Py_INCREF(e);
    if (PyModule_AddObject(m, attr, e) < 0) { Py_DECREF(e); Py_DECREF(e); return NULL; }
    return e;
}

PyMODINIT_FUNC PyInit_rdk(void) {
    PyObject *m, *t, *coll, *nt, *a, *k, *base2;
    int i;
    static const struct { const char* name; long v; } consts[] = {
        { "CUSTOM", YRDK_CUSTOM }, { "WN", YRDK_WN }, { "MN", YRDK_MN }, { "LL", YRDK_LL }, { "BM", YRDK_BM },
        { "SIGNAL_SAME", YRDK_SIGNAL_SAME }, { "SIGNAL_DIFFERENT", YRDK_SIGNAL_DIFFERENT },
        { "SIGNAL_LEAST_RECENT", YRDK_SIGNAL_LEAST_RECENT }, { "EXACT", YRDK_EXACT }, { "BERNOULLI", YRDK_BERNOULLI },
        { "NOISE_DIRECTION", YRDK_NOISE_DIRECTION }, { "NOISE_POSITION", YRDK_NOISE_POSITION },
        { "NOISE_WALK", YRDK_NOISE_WALK }, { "CIRCLE", YRDK_CIRCLE }, { "RECT", YRDK_RECT },
        { "WRAP", YRDK_WRAP }, { "REPLOT", YRDK_REPLOT }, { "CLOCK_ONSET", YRDK_CLOCK_ONSET },
        { "CLOCK_FRAMES", YRDK_CLOCK_FRAMES }, { "EV_SIGNAL", YRDK_EV_SIGNAL }, { "EV_WRAPPED", YRDK_EV_WRAPPED },
        { "EV_PLACED", YRDK_EV_PLACED },
    };
    m = PyModule_Create(&ysp_rdk_module);
    if (!m) return NULL;
    if (!(t = PyType_FromSpec(&Field_spec))) goto fail;
    if (PyModule_AddObject(m, "Field", t) < 0) { Py_DECREF(t); goto fail; }
    if (!(RdkError = rdk_add_exc(m, "ysp.rdk.Error", "Error", NULL))) goto fail;
    base2 = PyTuple_Pack(2, RdkError, PyExc_ValueError);
    if (!base2) goto fail;
    RdkArgumentError = rdk_add_exc(m, "ysp.rdk.ArgumentError", "ArgumentError", base2);
    RdkOrderError = rdk_add_exc(m, "ysp.rdk.OrderError", "OrderError", base2);
    Py_DECREF(base2);
    if (!RdkArgumentError || !RdkOrderError) goto fail;
    coll = PyImport_ImportModule("collections");
    if (!coll) goto fail;
    nt = PyObject_GetAttrString(coll, "namedtuple");
    a = Py_BuildValue("(ss)", "Step", "t coherence direction speed set ns");
    k = Py_BuildValue("{s:s}", "module", "ysp.rdk");
    StepType = (nt && a && k) ? PyObject_Call(nt, a, k) : NULL;
    Py_XDECREF(nt); Py_XDECREF(a); Py_XDECREF(k); Py_DECREF(coll);
    if (!StepType) goto fail;
    Py_INCREF(StepType);
    if (PyModule_AddObject(m, "Step", StepType) < 0) { Py_DECREF(StepType); goto fail; }
    for (i = 0; i < (int)(sizeof consts / sizeof consts[0]); i++)
        if (PyModule_AddIntConstant(m, consts[i].name, consts[i].v) < 0) goto fail;
    if (PyModule_AddStringConstant(m, "__version__", RDK_VERSION) < 0) goto fail;
    return m;
fail:
    Py_DECREF(m);
    return NULL;
}
