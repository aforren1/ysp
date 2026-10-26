/* ysp_rt_ext.c - CPython extension wrapping ysp/rt.h (module ysp.rt)
 *
 * The clock, the deadline waits, the clock correlation, the device clock fit,
 * the virtual clock and the per-user folders, for a PsychoPy experiment that
 * wants its own stamps on the clock of the ysp data and devices, and for
 * analysis code that maps a device's clock to it.
 *
 *     import ysp.rt as rt
 *     t = rt.now() + 0.5
 *     late = rt.sleep_until(t)              # GIL released; seconds late
 *     c = rt.correlate(psychopy.core.getTime)   # (t, other, width, ...)
 *     fit = rt.Fit("late", tick=1e-6, bits=32)  # a board's micros()
 *     fit.add(us, rt.now()); fit.map(event_us)
 *
 * UNITS: every time is float seconds; the C API takes int64 ns, and the
 * conversion happens here, once. now_ns(), Corr.t_ns and Fit.map_ns() give
 * the exact int ns beside the seconds.
 *
 * Not bound, and why (docs/rt.md, "Bindings"): the worker, the pump and the
 * event ring (C-side infrastructure: a Python callback would need the GIL on
 * a real-time thread), the scheduling ladder (not undoable, and a deadline
 * reservation caps the interpreter thread's CPU), virtual timers, the
 * instrumentation macros.
 *
 * Limited API (abi3, CPython 3.8+), so the Fit type is a heap type.
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x03080000   /* target the CPython 3.8+ stable ABI */
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

/* Python.h first is safe: pyconfig.h already defines _GNU_SOURCE on Linux
 * and raises _WIN32_WINNT past 0x0600 on Windows, which is what the header
 * asks for when it is not the first include. */
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"

#include <math.h>
#include <string.h>

static PyObject* RtError;
static PyObject* CorrType;   /* namedtuple Corr(t, other, width, tries, t_ns) */

/* --- seconds <-> ns -------------------------------------------------------- */

#define RT_S_INF 1   /* math.inf is allowed and means "forever" */

/* Python seconds to int64 ns, rounded to the nearest ns. The whole seconds
 * convert exactly and only the fraction is scaled, so a time read from now()
 * comes back to the same ns below 2^23 s (97 days) of uptime. Negative and
 * NaN are refused: in C a negative deadline counts as 0, which is not what a
 * Python caller meant. */
static int rt_seconds(PyObject* o, const char* what, int flags, int64_t* out) {
    double s = PyFloat_AsDouble(o);
    if (s == -1.0 && PyErr_Occurred()) return -1;
    /* Comparisons, not isnan()/isinf(): MinGW's macros convert through float
     * and warn under -Wfloat-conversion. !(s >= 0) is true for NaN. */
    if (!(s >= 0)) {
        PyErr_Format(PyExc_ValueError, "%s: seconds, >= 0 (not NaN)", what);
        return -1;
    }
    if (s == HUGE_VAL) {
        if (flags & RT_S_INF) { *out = INT64_MAX; return 0; }
        PyErr_Format(PyExc_ValueError, "%s: seconds, finite", what);
        return -1;
    }
    if (s >= 9223372036.0) {   /* INT64_MAX ns is 9223372036.85 s */
        PyErr_Format(PyExc_OverflowError, "%s: seconds, < 9223372036 (int64 ns)", what);
        return -1;
    }
    {
        double whole = floor(s);
        *out = (int64_t)whole * 1000000000 + (int64_t)llround((s - whole) * 1e9);
    }
    return 0;
}

static PyObject* rt_s(int64_t ns) { return PyFloat_FromDouble((double)ns / 1e9); }

/* A device counter: an integer in 0..2^64-1. __index__, so a NumPy integer
 * from an analysis array works as a Python int does. */
static int rt_ticks(PyObject* o, uint64_t* out) {
    unsigned long long v;
    PyObject* i = PyIndex_Check(o) ? PyNumber_Index(o) : NULL;
    if (!i) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "ticks: an int (the device's counter)");
        return -1;
    }
    v = PyLong_AsUnsignedLongLong(i);
    Py_DECREF(i);
    if (v == (unsigned long long)-1 && PyErr_Occurred()) return -1;
    *out = (uint64_t)v;
    return 0;
}

/* --- clock ----------------------------------------------------------------- */

/* Seconds in a double step by 1.9 ns between 97 and 194 days of uptime:
 * finer than QueryPerformanceCounter's 100 ns tick. */
static PyObject* mod_now(PyObject* self, PyObject* args) {
    (void)self; (void)args;
    return PyFloat_FromDouble((double)yrt_now_ns() / 1e9);
}

static PyObject* mod_now_ns(PyObject* self, PyObject* args) {
    (void)self; (void)args;
    return PyLong_FromLongLong((long long)yrt_now_ns());
}

static PyObject* mod_real_now(PyObject* self, PyObject* args) {
    (void)self; (void)args;
    return PyFloat_FromDouble((double)yrt_real_ns() / 1e9);
}

static PyObject* mod_clock_info(PyObject* self, PyObject* args) {
    yrt_clock_info ci;
    (void)self; (void)args;
    yrt_get_clock_info(&ci);
    return Py_BuildValue("{s:d,s:O}", "resolution", (double)ci.resolution_ns / 1e9,
                         "hires_timer", ci.hires_timer ? Py_True : Py_False);
}

static PyObject* mod_describe(PyObject* self, PyObject* args) {
    yrt_report r;
    char line[256];
    (void)self; (void)args;
    /* NONE: the binding asked for no rung (the ladder is not bound). */
    yrt_report_get(&r, YRT_POLICY_NONE);
    yrt_describe(&r, line, sizeof line);
    return PyUnicode_FromString(line);
}

/* --- waits ----------------------------------------------------------------- */

/* A wait releases the GIL. A long wait goes to the OS in 100 ms slices with
 * no spin, and the signal handlers run between them, so Ctrl+C ends it as it
 * ends time.sleep(); the last 100 to 200 ms (plus the spin window) are one
 * call of the header's wait with the caller's spin, which sets the
 * lateness. */
#define RT_SLICE_NS 100000000LL

static int rt_wait(int64_t deadline, int64_t spin, int pure_spin, int64_t* late) {
    int64_t l;
    for (;;) {
        int64_t now = yrt_now_ns();
        if (deadline - now <= 2 * RT_SLICE_NS + spin) break;
        Py_BEGIN_ALLOW_THREADS
        if (pure_spin) (void)yrt_spin_until(now + RT_SLICE_NS);
        else           (void)yrt_sleep_until(now + RT_SLICE_NS, 0);
        Py_END_ALLOW_THREADS
        if (PyErr_CheckSignals() < 0) return -1;
    }
    Py_BEGIN_ALLOW_THREADS
    l = pure_spin ? yrt_spin_until(deadline) : yrt_sleep_until(deadline, spin);
    Py_END_ALLOW_THREADS
    *late = l;
    return 0;
}

static PyObject* mod_sleep_until(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kws[] = { "t", "spin", NULL };
    PyObject *to, *so = NULL;
    int64_t t, spin = YRT_DEFAULT_SPIN_NS, late;
    (void)self;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|O:sleep_until", kws, &to, &so)) return NULL;
    if (rt_seconds(to, "t", 0, &t) < 0) return NULL;
    if (so && rt_seconds(so, "spin", 0, &spin) < 0) return NULL;
    if (rt_wait(t, spin, 0, &late) < 0) return NULL;
    return rt_s(late);
}

static PyObject* mod_sleep(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kws[] = { "duration", "spin", NULL };
    PyObject *dO, *so = NULL;
    int64_t d, spin = YRT_DEFAULT_SPIN_NS, late, t0;
    (void)self;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|O:sleep", kws, &dO, &so)) return NULL;
    /* The deadline is taken first, so the parse is part of the wait, as in
     * yrt_sleep_ns(). */
    t0 = yrt_now_ns();
    if (rt_seconds(dO, "duration", 0, &d) < 0) return NULL;
    if (so && rt_seconds(so, "spin", 0, &spin) < 0) return NULL;
    if (d > INT64_MAX - t0) { PyErr_SetString(PyExc_OverflowError, "duration: too long"); return NULL; }
    if (rt_wait(t0 + d, spin, 0, &late) < 0) return NULL;
    return rt_s(late);
}

static PyObject* mod_spin_until(PyObject* self, PyObject* arg) {
    int64_t t, late;
    (void)self;
    if (rt_seconds(arg, "t", 0, &t) < 0) return NULL;
    if (rt_wait(t, 0, 1, &late) < 0) return NULL;
    return rt_s(late);
}

static PyObject* mod_thread_cleanup(PyObject* self, PyObject* args) {
    (void)self; (void)args;
    yrt_thread_cleanup();
    Py_RETURN_NONE;
}

/* --- virtual clock --------------------------------------------------------- */

static PyObject* mod_virtual_begin(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kws[] = { "t0", NULL };
    PyObject* to = NULL;
    int64_t t0 = 0;
    (void)self;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|O:virtual_begin", kws, &to)) return NULL;
    if (to && rt_seconds(to, "t0", 0, &t0) < 0) return NULL;
    if (yrt_virtual_begin(t0) < 0) {
        PyErr_SetString(RtError, "virtual_begin: the clock is already virtual");
        return NULL;
    }
    return rt_s(yrt_now_ns());
}

static PyObject* mod_virtual_end(PyObject* self, PyObject* args) {
    (void)self; (void)args;
    yrt_virtual_end();
    Py_RETURN_NONE;
}

static PyObject* mod_is_virtual(PyObject* self, PyObject* args) {
    (void)self; (void)args;
    return PyBool_FromLong(yrt_is_virtual());
}

/* --- correlation ----------------------------------------------------------- */

/* The other clock is a Python callable. An int is the clock's own counter
 * and passes through; a float is seconds and is carried as ns, so Corr.other
 * comes back as seconds again. */
typedef struct rt_corr_ctx {
    PyObject* fn;
    int       kind;      /* 0 not yet, 1 int, 2 float seconds */
    int       failed;    /* an exception is set: call no more Python */
    uint64_t  fake;
} rt_corr_ctx;

static uint64_t rt_corr_read(void* p) {
    rt_corr_ctx* c = (rt_corr_ctx*)p;
    PyObject* r;
    uint64_t v = 0;
    int kind;
    /* After a failure the header still finishes its loop. A value that
     * changes on every read ends edge mode after `tries` reads instead of at
     * the timeout; the result is thrown away. */
    if (c->failed) return ++c->fake;
    r = PyObject_CallObject(c->fn, NULL);
    if (!r) { c->failed = 1; return 0; }
    if (PyLong_Check(r)) {
        kind = 1;
        v = (uint64_t)PyLong_AsUnsignedLongLong(r);
        if (v == (uint64_t)-1 && PyErr_Occurred()) c->failed = 1;
    } else if (PyFloat_Check(r)) {
        int64_t ns;
        kind = 2;
        if (rt_seconds(r, "the clock's value", 0, &ns) < 0) c->failed = 1;
        else v = (uint64_t)ns;
    } else {
        kind = 0;
        PyErr_SetString(PyExc_TypeError, "correlate: read() must return an int (counter) or a float (seconds)");
        c->failed = 1;
    }
    Py_DECREF(r);
    if (!c->failed && c->kind && kind != c->kind) {
        PyErr_SetString(PyExc_TypeError, "correlate: read() returned an int once and a float once");
        c->failed = 1;
    }
    if (!c->failed) c->kind = kind;
    return v;
}

static PyObject* mod_correlate(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kws[] = { "read", "edge", "tries", "timeout", NULL };
    PyObject *fn, *timeout = NULL, *other;
    int edge = 0, tries = 0, rc;
    yrt_corr_desc d;
    yrt_corr out;
    rt_corr_ctx c;
    (void)self;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|$piO:correlate", kws, &fn, &edge, &tries, &timeout))
        return NULL;
    if (!PyCallable_Check(fn)) { PyErr_SetString(PyExc_TypeError, "read: a callable"); return NULL; }
    if (tries < 0) { PyErr_SetString(PyExc_ValueError, "tries: >= 0 (0 = 16, or 4 edges)"); return NULL; }
    memset(&d, 0, sizeof d);
    memset(&c, 0, sizeof c);
    c.fn = fn;
    d.read_ctx = rt_corr_read;
    d.ctx = &c;
    d.tries = tries;
    d.edge = edge != 0;
    if (timeout && rt_seconds(timeout, "timeout", 0, &d.timeout_ns) < 0) return NULL;
    if (timeout && d.timeout_ns == 0) d.timeout_ns = 1;   /* 0 in C means the default */
    /* The GIL stays held: every read calls Python. */
    rc = yrt_correlate(&d, &out);
    if (c.failed) return NULL;
    if (rc == YRT_ERR_TIMEOUT) {
        PyErr_SetString(RtError, "correlate: the clock did not change before the timeout");
        return NULL;
    }
    if (rc < 0) { PyErr_SetString(RtError, yrt_strerror(rc)); return NULL; }
    other = c.kind == 2 ? PyFloat_FromDouble((double)out.other / 1e9)
                        : PyLong_FromUnsignedLongLong((unsigned long long)out.other);
    if (!other) return NULL;
    return PyObject_CallFunction(CorrType, "dNdiL", (double)out.rt_ns / 1e9, other,
                                 (double)out.width_ns / 1e9, out.tries, (long long)out.rt_ns);
}

/* --- per-user folders ------------------------------------------------------ */

static PyObject* mod_user_dir(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kws[] = { "kind", "subdir", NULL };
    const char* kind;
    const char* sub = NULL;
    char out[1024];
    int k;
    (void)self;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "s|z:user_dir", kws, &kind, &sub)) return NULL;
    if (strcmp(kind, "cache") == 0) k = YRT_DIR_CACHE;
    else if (strcmp(kind, "config") == 0) k = YRT_DIR_CONFIG;
    else { PyErr_SetString(PyExc_ValueError, "kind: \"cache\" or \"config\""); return NULL; }
    /* None, not an exception: the header's rule is "use no folder", and a
     * caller must not fall back to a shared one. */
    if (yrt_user_dir(k, sub, out, sizeof out) != YRT_OK) Py_RETURN_NONE;
    return PyUnicode_DecodeFSDefault(out);
}

/* --- device clock fit ------------------------------------------------------ */

typedef struct {
    PyObject_HEAD
    yrt_fit fit;   /* about 40 KB, inside the object: nothing else allocates */
} FitObject;

#define AS_FIT(o) (&((FitObject*)(o))->fit)

static int Fit_init(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kws[] = { "mode", "tick", "bits", "bucket", "slope_span", "restart",
                           "restart_hold", "max_width", NULL };
    const char* mode = "late";
    double tick = 0;
    unsigned int bits = 0;
    PyObject *bucket = NULL, *span = NULL, *restart = NULL, *hold = NULL, *maxw = NULL;
    yrt_fit_desc d;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "s|$dIOOOOO:Fit", kws, &mode, &tick, &bits, &bucket,
                                     &span, &restart, &hold, &maxw))
        return -1;
    memset(&d, 0, sizeof d);
    if (strcmp(mode, "late") == 0) d.mode = YRT_FIT_LATE;
    else if (strcmp(mode, "bracket") == 0) d.mode = YRT_FIT_BRACKET;
    else if (strcmp(mode, "unbiased") == 0) d.mode = YRT_FIT_UNBIASED;
    else { PyErr_SetString(PyExc_ValueError, "mode: \"late\", \"bracket\" or \"unbiased\""); return -1; }
    if (!(tick > 0) || tick == HUGE_VAL) {
        PyErr_SetString(PyExc_ValueError, "tick: the device's nominal tick in seconds, > 0 (1e-3 for a ms timer)");
        return -1;
    }
    d.ns_per_tick = tick * 1e9;
    d.tick_bits = bits;
    if (bucket && rt_seconds(bucket, "bucket", 0, &d.bucket_ns) < 0) return -1;
    if (span && rt_seconds(span, "slope_span", 0, &d.slope_span_ns) < 0) return -1;
    if (restart && rt_seconds(restart, "restart", 0, &d.restart_ns) < 0) return -1;
    if (hold && rt_seconds(hold, "restart_hold", 0, &d.restart_hold_ns) < 0) return -1;
    if (maxw && rt_seconds(maxw, "max_width", 0, &d.max_width_ns) < 0) return -1;
    if (!yrt_fit_open(AS_FIT(self), &d)) {
        PyErr_SetString(PyExc_ValueError, yrt_fit_error(AS_FIT(self)));
        return -1;
    }
    return 0;
}

static void Fit_dealloc(PyObject* self) {
    PyTypeObject* tp = Py_TYPE(self);
    freefunc f = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    f(self);
    Py_DECREF(tp);
}

static PyObject* Fit_add(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kws[] = { "ticks", "t", "width", NULL };
    PyObject *tk, *to, *wo = NULL;
    uint64_t ticks;
    int64_t t, w = 0;
    int rc;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|O:add", kws, &tk, &to, &wo)) return NULL;
    if (rt_ticks(tk, &ticks) < 0 || rt_seconds(to, "t", 0, &t) < 0) return NULL;
    if (wo && rt_seconds(wo, "width", 0, &w) < 0) return NULL;
    rc = yrt_fit_add(AS_FIT(self), ticks, t, w);
    if (rc < 0) {   /* the only one is YRT_ERR_ARG: __init__ failed or was skipped */
        PyErr_SetString(RtError, "add: the fit is not open");
        return NULL;
    }
    return PyLong_FromLong(rc);
}

/* map() and map_ns() take one counter value or an iterable of them. */
static PyObject* fit_map(PyObject* self, PyObject* arg, int ns) {
    const yrt_fit* f = AS_FIT(self);
    yrt_fit_info info;
    uint64_t ticks;
    PyObject *it, *item, *list;
    /* The C map returns 0 here, which a Python caller would take for a time. */
    yrt_fit_get(f, &info);
    if (!info.ready) {
        PyErr_SetString(RtError, "map: the fit has no map yet (no pair added)");
        return NULL;
    }
    if (PyIndex_Check(arg)) {
        int64_t v;
        if (rt_ticks(arg, &ticks) < 0) return NULL;
        v = yrt_fit_map(f, ticks);
        return ns ? PyLong_FromLongLong((long long)v) : rt_s(v);
    }
    it = PyObject_GetIter(arg);
    if (!it) {
        PyErr_SetString(PyExc_TypeError, "map: an int or an iterable of ints");
        return NULL;
    }
    list = PyList_New(0);
    if (!list) { Py_DECREF(it); return NULL; }
    while ((item = PyIter_Next(it)) != NULL) {
        PyObject* o;
        int64_t v;
        int bad = rt_ticks(item, &ticks) < 0;
        Py_DECREF(item);
        if (bad) goto fail;
        v = yrt_fit_map(f, ticks);
        o = ns ? PyLong_FromLongLong((long long)v) : rt_s(v);
        if (!o || PyList_Append(list, o) < 0) { Py_XDECREF(o); goto fail; }
        Py_DECREF(o);
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) { Py_DECREF(list); return NULL; }
    return list;
fail:
    Py_DECREF(it);
    Py_DECREF(list);
    return NULL;
}

static PyObject* Fit_map(PyObject* self, PyObject* arg) { return fit_map(self, arg, 0); }
static PyObject* Fit_map_ns(PyObject* self, PyObject* arg) { return fit_map(self, arg, 1); }

static PyObject* Fit_unwrap(PyObject* self, PyObject* arg) {
    uint64_t ticks;
    if (rt_ticks(arg, &ticks) < 0) return NULL;
    return PyLong_FromUnsignedLongLong((unsigned long long)yrt_fit_unwrap(AS_FIT(self), ticks));
}

static PyObject* Fit_info(PyObject* self, PyObject* args) {
    yrt_fit_info i;
    (void)args;
    yrt_fit_get(AS_FIT(self), &i);
    return Py_BuildValue("{s:d,s:L,s:K,s:d,s:d,s:d,s:d,s:d,s:k,s:k,s:k,s:k,s:k,s:O,s:O}",
                         "t0", (double)i.t0_ns / 1e9, "t0_ns", (long long)i.t0_ns,
                         "ticks0", (unsigned long long)i.ticks0, "tick", i.ns_per_tick / 1e9,
                         "ppm", i.ppm, "spread", (double)i.spread_ns / 1e9,
                         "width", (double)i.width_ns / 1e9, "span", (double)i.span_ns / 1e9,
                         "points", (unsigned long)i.points, "pairs", (unsigned long)i.pairs,
                         "epoch", (unsigned long)i.epoch, "rejected", (unsigned long)i.rejected,
                         "pending", (unsigned long)i.pending,
                         "ready", i.ready ? Py_True : Py_False, "slope", i.slope ? Py_True : Py_False);
}

static PyMethodDef Fit_methods[] = {
    { "add", (PyCFunction)(void (*)(void))Fit_add, METH_VARARGS | METH_KEYWORDS,
      "add(ticks, t, width=0) -> int: one pair, the device's counter as it sent it (wrapped) and "
      "the now() seconds that go with it; for \"bracket\", t is the bracket's end and width its "
      "length. Returns FIT_* bits." },
    { "map", Fit_map, METH_O,
      "map(ticks) -> float or list: the now() seconds of a counter value, or of each value of an "
      "iterable. Error before the first pair." },
    { "map_ns", Fit_map_ns, METH_O, "map_ns(ticks) -> int or list: map() in exact int ns." },
    { "unwrap", Fit_unwrap, METH_O, "unwrap(ticks) -> int: a wrapped counter value unwrapped against the newest pair." },
    { "info", Fit_info, METH_NOARGS,
      "info() -> dict: t0 (and t0_ns) at ticks0, the fitted tick (s), ppm, spread (p99 distance of "
      "the points from the map, s), width, span, points, pairs, epoch, rejected, pending, ready, slope." },
    { NULL }
};

static PyType_Slot Fit_slots[] = {
    { Py_tp_init, (void*)Fit_init },
    { Py_tp_dealloc, (void*)Fit_dealloc },
    { Py_tp_methods, Fit_methods },
    { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_doc, (void*)"Fit(mode, *, tick, bits=0, bucket=0.2, slope_span=5, restart=0.05, "
                        "restart_hold=1, max_width=0.0013): map a device's clock to now() seconds "
                        "(yrt_fit). mode is \"late\", \"bracket\" or \"unbiased\"; tick is the "
                        "nominal tick in seconds; bits the counter's width (32 for a uint32 timer, "
                        "0 = no wrap). One thread at a time." },
    { 0, NULL }
};

static PyType_Spec Fit_spec = {
    "ysp.rt.Fit", sizeof(FitObject), 0, Py_TPFLAGS_DEFAULT, Fit_slots,
};

/* --- module ---------------------------------------------------------------- */

static PyMethodDef module_methods[] = {
    { "now", mod_now, METH_NOARGS,
      "now() -> float: seconds on the ysp clock (yrt_now_ns: QueryPerformanceCounter / "
      "CLOCK_MONOTONIC), the clock of ysp.serial.now(), ysp.parallel.now() and every ysp stamp." },
    { "now_ns", mod_now_ns, METH_NOARGS, "now_ns() -> int: now() in exact int ns." },
    { "real_now", mod_real_now, METH_NOARGS,
      "real_now() -> float: the real clock, also while the clock is virtual. For a cost, never "
      "for a time that feeds a plan." },
    { "sleep_until", (PyCFunction)(void (*)(void))mod_sleep_until, METH_VARARGS | METH_KEYWORDS,
      "sleep_until(t, spin=DEFAULT_SPIN) -> float: wait until now() reaches t, the OS wait first "
      "and the last `spin` seconds spun; returns how late the wake was (s). Releases the GIL." },
    { "sleep", (PyCFunction)(void (*)(void))mod_sleep, METH_VARARGS | METH_KEYWORDS,
      "sleep(duration, spin=DEFAULT_SPIN) -> float: sleep_until(now() + duration). For a "
      "sequence of frames, use sleep_until() against a running deadline." },
    { "spin_until", mod_spin_until, METH_O,
      "spin_until(t) -> float: spin on the clock until t (a whole core); returns the lateness. "
      "Releases the GIL." },
    { "thread_cleanup", mod_thread_cleanup, METH_NOARGS,
      "thread_cleanup(): release the calling thread's wait timer (Windows), for a thread that "
      "is about to end. Optional." },
    { "clock_info", mod_clock_info, METH_NOARGS,
      "clock_info() -> dict: resolution (s, the clock's tick) and hires_timer." },
    { "describe", mod_describe, METH_NOARGS,
      "describe() -> str: the yrt_describe() line for the calling thread; log it beside timing data." },
    { "virtual_begin", (PyCFunction)(void (*)(void))mod_virtual_begin, METH_VARARGS | METH_KEYWORDS,
      "virtual_begin(t0=0) -> float: put this module's clock on a virtual clock (t0 = 0 starts "
      "at VIRTUAL_T0); the calling thread's waits move it. Returns now(). Error if already "
      "virtual. Other ysp modules keep the real clock." },
    { "virtual_end", mod_virtual_end, METH_NOARGS, "virtual_end(): back to the real clock." },
    { "is_virtual", mod_is_virtual, METH_NOARGS, "is_virtual() -> bool" },
    { "correlate", (PyCFunction)(void (*)(void))mod_correlate, METH_VARARGS | METH_KEYWORDS,
      "correlate(read, *, edge=False, tries=0, timeout=0.1) -> Corr(t, other, width, tries, t_ns): "
      "read another clock between two reads of this one and keep the tightest bracket. read() "
      "returns an int (a counter) or a float (seconds). The true time of `other` is within "
      "width / 2 of t. edge=True times a change of a coarse clock." },
    { "user_dir", (PyCFunction)(void (*)(void))mod_user_dir, METH_VARARGS | METH_KEYWORDS,
      "user_dir(kind, subdir=None) -> str or None: <base>/ysp/<subdir> for kind \"cache\" or "
      "\"config\", or None (then use no folder). Makes no folder." },
    { NULL }
};

static PyModuleDef ysp_rt_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "ysp.rt",
    .m_doc = "The ysp clock, deadline waits, clock correlation and device clock fit (ysp/rt.h).",
    .m_size = -1,
    .m_methods = module_methods,
};

static int rt_add_obj(PyObject* m, const char* name, PyObject* o) {
    if (!o) return -1;
    if (PyModule_AddObject(m, name, o) < 0) { Py_DECREF(o); return -1; }
    return 0;
}

PyMODINIT_FUNC PyInit_rt(void) {
    PyObject *m, *t, *coll, *nt;
    m = PyModule_Create(&ysp_rt_module);
    if (!m) return NULL;
    if (!(t = PyType_FromSpec(&Fit_spec))) goto fail;
    if (PyModule_AddObject(m, "Fit", t) < 0) { Py_DECREF(t); goto fail; }
    RtError = PyErr_NewException("ysp.rt.Error", NULL, NULL);
    if (!RtError) goto fail;
    Py_INCREF(RtError);
    if (PyModule_AddObject(m, "Error", RtError) < 0) { Py_DECREF(RtError); goto fail; }
    coll = PyImport_ImportModule("collections");
    if (!coll) goto fail;
    nt = PyObject_GetAttrString(coll, "namedtuple");
    Py_DECREF(coll);
    if (!nt) goto fail;
    CorrType = PyObject_CallFunction(nt, "ss", "Corr", "t other width tries t_ns");
    Py_DECREF(nt);
    if (!CorrType) goto fail;
    {
        PyObject* modname = PyUnicode_FromString("ysp.rt");
        int bad = !modname || PyObject_SetAttrString(CorrType, "__module__", modname) < 0;
        Py_XDECREF(modname);
        if (bad) goto fail;
    }
    Py_INCREF(CorrType);
    if (PyModule_AddObject(m, "Corr", CorrType) < 0) { Py_DECREF(CorrType); goto fail; }
    if (PyModule_AddStringConstant(m, "__version__", YRT_VERSION_STRING) < 0) goto fail;
    if (rt_add_obj(m, "DEFAULT_SPIN", rt_s(YRT_DEFAULT_SPIN_NS)) < 0 ||
        rt_add_obj(m, "VIRTUAL_T0", rt_s(YRT_VIRTUAL_T0_NS)) < 0)
        goto fail;
    PyModule_AddIntConstant(m, "FIT_POINT", YRT_FIT_POINT);
    PyModule_AddIntConstant(m, "FIT_REFIT", YRT_FIT_REFIT);
    PyModule_AddIntConstant(m, "FIT_RESTART", YRT_FIT_RESTART);
    PyModule_AddIntConstant(m, "FIT_REJECTED", YRT_FIT_REJECTED);
    PyModule_AddIntConstant(m, "FIT_PENDING", YRT_FIT_PENDING);
    PyModule_AddIntConstant(m, "FIT_POINTS", YRT_FIT_POINTS);
    return m;
fail:
    Py_DECREF(m);
    return NULL;
}
