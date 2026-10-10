/* ysp_quest_ext.c - CPython extension wrapping ysp/quest.h (module ysp.quest)
 *
 * A thin, dependency-free binding (no nanobind/pybind/Cython, no NumPy): it
 * needs only Python.h. The library implementation, with its YQST_ASYNC layer
 * and the ysp/rt.h that layer brings, is compiled directly into this module.
 *
 * Built against the stable ABI / Limited API (Py_LIMITED_API 3.8), so one
 * compiled ysp/quest.abi3.so works across CPython >= 3.8. That rules out the
 * static PyTypeObject layout (Quest and Async are heap types made with
 * PyType_FromSpec) and, before 3.11, the buffer protocol: arrays come in as
 * any object memoryview() accepts, or as a sequence, and go out as a COPY in a
 * read-only memoryview over a bytes object.
 *
 *     import ysp.quest as pq
 *     q = pq.Quest(stim=[(-3.0, 0.0, 31)],
 *                  params=[(-3.0, 0.0, 61), (0.5, 6.0, 12), 0.5, 0.02],
 *                  stop_trials=60)
 *     while not q.done:
 *         i = q.next()
 *         q.update(i, run_trial(q.stim_value(i)))
 *     print(q.estimate(pq.EST_MEAN))
 *
 * GIL. A call releases the GIL only when no Python callback can run inside
 * it: no rng callable (next, next_subset), and no Python pf where the call
 * evaluates the model (open always; update and expected_entropy under
 * no_table; update_values, p, p_values and simulate always). A callback
 * needs the GIL, and taking it back from inside a call that released it is
 * the kind of re-entry this binding does not do. A per-object busy flag
 * keeps a second Python thread off the handle while the first has the GIL
 * released, and keeps a callback from re-entering the Quest it runs under.
 *
 * ASYNC. yqst_async's thread runs yqst_update and yqst_next and nothing
 * else. Async refuses a Quest whose next or update would call back into
 * Python (an rng, or a Python pf under no_table), and submit_values() refuses
 * a Python pf, so the C thread never touches the interpreter and never needs
 * the GIL.
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x03080000   /* target the CPython 3.8+ stable ABI */
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

/* Python.h first is safe: pyconfig.h already defines _GNU_SOURCE on Linux and
 * raises _WIN32_WINNT on Windows, which is what ysp/rt.h asks for when it is
 * not the first include. YQST_ASYNC makes ysp/quest.h include ysp/rt.h and
 * YSP_QUEST_IMPLEMENTATION compile both. */
#define YQST_ASYNC
#define YSP_QUEST_IMPLEMENTATION
#include "ysp/quest.h"

#include <string.h>

/* ======================================================================= *
 *  Module-level state
 * ======================================================================= */

static PyObject* QError;
static PyObject* QArgumentError;
static PyObject* QClosed;
static PyObject* QFull;
static PyObject* QOutOfMemory;
static PyObject* QBusy;
static PyObject* QTimeout;

static PyObject* QStopEnum;
static PyObject* QTrialType;      /* namedtuple for history()            */
static PyObject* QSnapshotType;   /* namedtuple for Async.poll()/wait()  */
static PyObject* QQuestType;      /* the Quest heap type, for isinstance */

/* Heap-type instances must visit their type in tp_traverse from 3.9 on, and
 * must not before; the Limited API has no version macro to test at run time,
 * so the module reads sys.version_info once at import. */
static int g_visit_type = 1;

static PyObject* q_fail(int code) {
    PyObject* exc = QError;
    switch (code) {
    case YQST_ERR_ARG:     exc = QArgumentError; break;
    case YQST_ERR_CLOSED:  exc = QClosed; break;
    case YQST_ERR_FULL:    exc = QFull; break;
    case YQST_ERR_MEMORY:  exc = QOutOfMemory; break;
    case YQST_ERR_BUSY:    exc = QBusy; break;
    case YQST_ERR_TIMEOUT: exc = QTimeout; break;
    default: break;
    }
    PyErr_SetString(exc, yqst_strerror(code));
    return NULL;
}

/* PyUnicode_AsUTF8 is Limited API only from 3.10; this goes through a bytes
 * object the caller releases. */
static const char* q_utf8(PyObject* s, PyObject** keep) {
    *keep = PyUnicode_AsUTF8String(s);
    return *keep ? PyBytes_AsString(*keep) : NULL;
}

static PyObject* q_enum(PyObject* cls, long v) {
    PyObject* r = PyObject_CallFunction(cls, "l", v);
    if (!r) { PyErr_Clear(); r = PyLong_FromLong(v); }
    return r;
}

/* ======================================================================= *
 *  Array conversion. No buffer protocol before 3.11 in the Limited API, so a
 *  buffer is read through memoryview: its format and size are checked, then
 *  tobytes() gives a contiguous copy in C order. Anything that is not a buffer
 *  is read as a sequence of numbers.
 * ======================================================================= */

/* Return a PyMem_Malloc'd array of the values in `obj` and their count, or
 * NULL with an exception set. */
static double* q_doubles(PyObject* obj, Py_ssize_t* n_out, const char* what) {
    double* out = NULL;
    Py_ssize_t n, i;
    PyObject* mv = PyMemoryView_FromObject(obj);
    if (mv) {
        PyObject *fmt_o = NULL, *fmt_b = NULL, *bytes = NULL;
        const char* fmt;
        long itemsize;
        PyObject* isz = PyObject_GetAttrString(mv, "itemsize");
        fmt_o = PyObject_GetAttrString(mv, "format");
        if (!isz || !fmt_o) { Py_XDECREF(isz); Py_XDECREF(fmt_o); Py_DECREF(mv); return NULL; }
        itemsize = PyLong_AsLong(isz);
        Py_DECREF(isz);
        fmt = q_utf8(fmt_o, &fmt_b);
        Py_DECREF(fmt_o);
        fmt_o = fmt_b;   /* keeps `fmt` alive; released below */
        if (!fmt) { Py_DECREF(mv); return NULL; }
        /* A native or little-endian marker in front changes nothing on the
         * platforms this builds for; anything else is refused below. */
        if (fmt[0] == '@' || fmt[0] == '=' || fmt[0] == '<') fmt++;
        if (!((fmt[0] == 'd' && itemsize == 8) || (fmt[0] == 'f' && itemsize == 4)) ||
            fmt[1] != '\0') {
            PyErr_Format(PyExc_TypeError,
                         "%s: a buffer must hold float64 ('d') or float32 ('f'), not '%s'",
                         what, fmt);
            Py_DECREF(fmt_o); Py_DECREF(mv);
            return NULL;
        }
        bytes = PyObject_CallMethod(mv, "tobytes", NULL);
        Py_DECREF(mv);
        if (!bytes) { Py_DECREF(fmt_o); return NULL; }
        {
            const char* p = PyBytes_AsString(bytes);
            Py_ssize_t nb = PyBytes_Size(bytes);
            n = nb / itemsize;
            out = (double*)PyMem_Malloc((size_t)(n > 0 ? n : 1) * sizeof(double));
            if (!out) { Py_DECREF(bytes); Py_DECREF(fmt_o); PyErr_NoMemory(); return NULL; }
            if (fmt[0] == 'd') {
                memcpy(out, p, (size_t)n * sizeof(double));
            } else {
                for (i = 0; i < n; i++) {
                    float f;
                    memcpy(&f, p + i * 4, 4);
                    out[i] = (double)f;
                }
            }
        }
        Py_DECREF(bytes);
        Py_DECREF(fmt_o);
        *n_out = n;
        return out;
    }
    if (!PyErr_ExceptionMatches(PyExc_TypeError)) return NULL;
    PyErr_Clear();

    n = PySequence_Size(obj);
    if (n < 0) {
        PyErr_Clear();
        PyErr_Format(PyExc_TypeError, "%s must be a sequence of numbers or a float buffer", what);
        return NULL;
    }
    out = (double*)PyMem_Malloc((size_t)(n > 0 ? n : 1) * sizeof(double));
    if (!out) { PyErr_NoMemory(); return NULL; }
    for (i = 0; i < n; i++) {
        PyObject* it = PySequence_GetItem(obj, i);
        if (!it) { PyMem_Free(out); return NULL; }
        out[i] = PyFloat_AsDouble(it);
        Py_DECREF(it);
        if (out[i] == -1.0 && PyErr_Occurred()) { PyMem_Free(out); return NULL; }
    }
    *n_out = n;
    return out;
}

/* Exactly `n` values into `out`. */
static int q_doubles_into(PyObject* obj, double* out, Py_ssize_t n, const char* what) {
    Py_ssize_t got = 0;
    double* v = q_doubles(obj, &got, what);
    if (!v) return -1;
    if (got != n) {
        PyErr_Format(QArgumentError, "%s has %zd values, need %zd", what, got, n);
        PyMem_Free(v);
        return -1;
    }
    memcpy(out, v, (size_t)n * sizeof(double));
    PyMem_Free(v);
    return 0;
}

static int q_ints_into(PyObject* obj, int* out, Py_ssize_t n, const char* what) {
    Py_ssize_t got = PySequence_Size(obj), i;
    if (got < 0) {
        PyErr_Clear();
        PyErr_Format(PyExc_TypeError, "%s must be a sequence of ints", what);
        return -1;
    }
    if (got != n) {
        PyErr_Format(QArgumentError, "%s has %zd values, need %zd", what, got, n);
        return -1;
    }
    for (i = 0; i < n; i++) {
        PyObject* it = PySequence_GetItem(obj, i);
        long v;
        if (!it) return -1;
        v = PyLong_AsLong(it);
        Py_DECREF(it);
        if (v == -1 && PyErr_Occurred()) return -1;
        out[i] = (int)v;
    }
    return 0;
}

static PyObject* q_tuple_d(const double* v, int n) {
    PyObject* t = PyTuple_New(n);
    int i;
    if (!t) return NULL;
    for (i = 0; i < n; i++) {
        PyObject* f = PyFloat_FromDouble(v[i]);
        if (!f) { Py_DECREF(t); return NULL; }
        PyTuple_SetItem(t, i, f);
    }
    return t;
}

/* A read-only memoryview of format 'd' over a COPY of `src`. `shape` is a
 * tuple for a multi-dimensional cast, or NULL for 1-D. */
static PyObject* q_view(const double* src, Py_ssize_t n, PyObject* shape) {
    PyObject *b, *mv, *r;
    b = PyBytes_FromStringAndSize((const char*)src, n * (Py_ssize_t)sizeof(double));
    if (!b) return NULL;
    mv = PyMemoryView_FromObject(b);
    Py_DECREF(b);
    if (!mv) return NULL;
    r = shape ? PyObject_CallMethod(mv, "cast", "sO", "d", shape)
              : PyObject_CallMethod(mv, "cast", "s", "d");
    Py_DECREF(mv);
    return r;
}

/* ======================================================================= *
 *  Quest
 * ======================================================================= */

typedef struct {
    PyObject_HEAD
    PyObject* pf_obj;        /* pf_fn or pf_batch callable, or NULL          */
    PyObject* rng_obj;       /* rng callable, or NULL                        */
    PyObject* pm_view;       /* cached view of the header's parameter matrix */
    const double* pm_ptr;    /* ... and what it was made from                */
    int pm_P;
    int pf_batch;            /* pf_obj is a batch callable                   */
    int no_table;
    int busy;                /* a C call is in progress on this handle       */
    int owned;               /* a running Async owns the handle              */
    /* The first exception a callback raised during the current call. The
     * header has no way to abort a callback, so later callbacks in the same
     * call return a harmless value and the exception is raised on return. */
    PyObject *cb_type, *cb_value, *cb_tb;
    /* Axis counts, kept because the snapshot needs them while the Async
     * thread owns the handle. Axis sizes come from yqst_*_axis_n. */
    int n_stim_dims, n_param_dims;
    yqst_quest q;
} QuestObject;

#define QO(self) ((QuestObject*)(self))

static int q_traverse(PyObject* self, visitproc visit, void* arg) {
    QuestObject* o = QO(self);
    Py_VISIT(o->pf_obj);
    Py_VISIT(o->rng_obj);
    Py_VISIT(o->pm_view);
    Py_VISIT(o->cb_type);
    Py_VISIT(o->cb_value);
    Py_VISIT(o->cb_tb);
    if (g_visit_type) Py_VISIT((PyObject*)Py_TYPE(self));
    return 0;
}

static int q_clear(PyObject* self) {
    QuestObject* o = QO(self);
    Py_CLEAR(o->pf_obj);
    Py_CLEAR(o->rng_obj);
    Py_CLEAR(o->pm_view);
    Py_CLEAR(o->cb_type);
    Py_CLEAR(o->cb_value);
    Py_CLEAR(o->cb_tb);
    return 0;
}

static void q_cb_store(QuestObject* o) {
    if (o->cb_type || o->cb_value) { PyErr_Clear(); return; }
    PyErr_Fetch(&o->cb_type, &o->cb_value, &o->cb_tb);
}

/* Enter/leave a C call. leave() re-raises a stored callback exception. */
static int q_enter(QuestObject* o) {
    if (o->owned) {
        PyErr_SetString(QError, "this Quest is owned by a running Async; stop it first");
        return -1;
    }
    if (o->busy) {
        PyErr_SetString(QError, "this Quest is already in a call (another thread, or a "
                                "callback re-entering the Quest it runs under)");
        return -1;
    }
    o->busy = 1;
    return 0;
}

static int q_leave(QuestObject* o) {
    o->busy = 0;
    if (o->cb_type || o->cb_value) {
        PyErr_Restore(o->cb_type, o->cb_value, o->cb_tb);
        o->cb_type = o->cb_value = o->cb_tb = NULL;
        return -1;
    }
    return 0;
}

/* --- callback trampolines --------------------------------------------- */

static double q_tramp_rng(void* ctx) {
    QuestObject* o = QO(ctx);
    PyObject* r;
    double u;
    if (o->cb_type || o->cb_value) return 0.0;
    r = PyObject_CallObject(o->rng_obj, NULL);
    if (!r) { q_cb_store(o); return 0.0; }
    u = PyFloat_AsDouble(r);
    Py_DECREF(r);
    if (u == -1.0 && PyErr_Occurred()) { q_cb_store(o); return 0.0; }
    return u;
}

static void q_uniform(double* p, int K) {
    int k;
    for (k = 0; k < K; k++) p[k] = 1.0 / (double)K;
}

static void q_tramp_pf(void* ctx, const double* stim, const double* params, double* p) {
    QuestObject* o = QO(ctx);
    int K = o->q.K;
    PyObject *s = NULL, *t = NULL, *r = NULL;
    if (o->cb_type || o->cb_value) { q_uniform(p, K); return; }
    s = q_tuple_d(stim, o->q.desc.n_stim);
    t = q_tuple_d(params, o->q.desc.n_param);
    if (s && t) r = PyObject_CallFunctionObjArgs(o->pf_obj, s, t, NULL);
    Py_XDECREF(s);
    Py_XDECREF(t);
    if (!r || q_doubles_into(r, p, K, "pf_fn's return value") < 0) {
        Py_XDECREF(r);
        q_cb_store(o);
        q_uniform(p, K);
        return;
    }
    Py_DECREF(r);
}

static void q_tramp_batch(void* ctx, const double* stims, int S, const double* params,
                          int P, float* out) {
    QuestObject* o = QO(ctx);
    int K = o->q.K, np = o->q.desc.n_param, ns = o->q.desc.n_stim;
    Py_ssize_t n = (Py_ssize_t)S * P * K, i;
    PyObject *s = NULL, *view = NULL, *r = NULL;
    double* v;
    Py_ssize_t got = 0;

    if (o->cb_type || o->cb_value) goto fallback;

    /* The full parameter matrix is built once at open and never changes, so
     * its copy is made once and handed to every per-stimulus call. */
    if (o->pm_view && params == o->pm_ptr && P == o->pm_P) {
        view = o->pm_view;
        Py_INCREF(view);
    } else {
        PyObject* shape = Py_BuildValue("(ii)", P, np);
        if (!shape) goto error;
        view = q_view(params, (Py_ssize_t)P * np, shape);
        Py_DECREF(shape);
        if (!view) goto error;
        if (params == o->q.param_matrix && P == o->q.P) {
            Py_XDECREF(o->pm_view);
            o->pm_view = view;
            Py_INCREF(view);
            o->pm_ptr = params;
            o->pm_P = P;
        }
    }
    if (S == 1) {
        s = q_tuple_d(stims, ns);
    } else {
        /* The header only ever passes S = 1; a batch of stimuli would come as a
         * tuple of rows. */
        s = PyTuple_New(S);
        if (s) for (i = 0; i < S; i++) {
            PyObject* row = q_tuple_d(stims + i * ns, ns);
            if (!row) { Py_CLEAR(s); break; }
            PyTuple_SetItem(s, i, row);
        }
    }
    if (!s) goto error;
    r = PyObject_CallFunctionObjArgs(o->pf_obj, s, view, NULL);
    Py_CLEAR(s);
    Py_CLEAR(view);
    if (!r) goto error;
    v = q_doubles(r, &got, "pf_batch's return value");
    Py_CLEAR(r);
    if (!v) goto error;
    if (got != n) {
        PyMem_Free(v);
        PyErr_Format(QArgumentError, "pf_batch returned %zd values, need P*K = %zd", got, n);
        goto error;
    }
    for (i = 0; i < n; i++) out[i] = (float)v[i];
    PyMem_Free(v);
    return;

error:
    Py_XDECREF(s);
    Py_XDECREF(view);
    Py_XDECREF(r);
    q_cb_store(o);
fallback:
    for (i = 0; i < n; i++) out[i] = (float)(1.0 / (double)K);
}

/* --- desc parsing ------------------------------------------------------ */

/* Arrays the desc points at while open() runs; open() copies them. */
typedef struct {
    yqst_desc d;
    void* bufs[2 * (YQST_MAX_STIM_DIMS + YQST_MAX_PARAMS) + 2];
    int nbufs;
    PyObject* pf_fn;      /* borrowed */
    PyObject* pf_batch;   /* borrowed */
    PyObject* rng;        /* borrowed */
} q_build;

static void q_build_free(q_build* b) {
    int i;
    for (i = 0; i < b->nbufs; i++) PyMem_Free(b->bufs[i]);
    b->nbufs = 0;
}

static double* q_build_doubles(q_build* b, PyObject* obj, Py_ssize_t* n, const char* what) {
    double* v = q_doubles(obj, n, what);
    if (v) b->bufs[b->nbufs++] = v;
    return v;
}

static int q_parse_int(PyObject* obj, int* out, const char* what) {
    long v;
    if (!PyLong_Check(obj)) {
        PyErr_Format(PyExc_TypeError, "%s must be an int", what);
        return -1;
    }
    v = PyLong_AsLong(obj);
    if (v == -1 && PyErr_Occurred()) return -1;
    if (v < 0 || v > 0x7FFFFFFF) {
        PyErr_Format(QArgumentError, "%s must be a non-negative int", what);
        return -1;
    }
    *out = (int)v;
    return 0;
}

/* One axis: a number (fixed), a (lo, hi, n) tuple, a dict with values or
 * lo/hi/n plus prior and nuisance, or a sequence/buffer of values. */
static int q_parse_axis(q_build* b, PyObject* spec, yqst_axis* ax, const char* what) {
    memset(ax, 0, sizeof(*ax));
    if (PyFloat_Check(spec) || (PyLong_Check(spec) && !PyBool_Check(spec))) {
        double v = PyFloat_AsDouble(spec);
        if (v == -1.0 && PyErr_Occurred()) return -1;
        *ax = yqst_fixed(v);
        return 0;
    }
    if (PyTuple_Check(spec) && PyTuple_Size(spec) == 3) {
        PyObject* n_o = PyTuple_GetItem(spec, 2);
        int n;
        if (!PyLong_Check(n_o)) {
            PyErr_Format(PyExc_TypeError,
                         "%s: a 3-tuple axis is (lo, hi, n) with an int n; pass a list for "
                         "explicit values", what);
            return -1;
        }
        if (q_parse_int(n_o, &n, what) < 0) return -1;
        ax->lo = PyFloat_AsDouble(PyTuple_GetItem(spec, 0));
        if (PyErr_Occurred()) return -1;
        ax->hi = PyFloat_AsDouble(PyTuple_GetItem(spec, 1));
        if (PyErr_Occurred()) return -1;
        ax->n = n;
        return 0;
    }
    if (PyDict_Check(spec)) {
        static const char* known[] = { "values", "lo", "hi", "n", "prior", "nuisance", NULL };
        PyObject *key, *val, *v;
        Py_ssize_t pos = 0;
        while (PyDict_Next(spec, &pos, &key, &val)) {
            PyObject* kb = NULL;
            const char* k = PyUnicode_Check(key) ? q_utf8(key, &kb) : NULL;
            int j, ok = 0;
            if (!k) { PyErr_Clear(); k = "?"; }
            for (j = 0; known[j]; j++) if (strcmp(k, known[j]) == 0) ok = 1;
            if (!ok) {
                PyErr_Format(PyExc_TypeError, "%s: unknown axis key '%s' (use values, lo, "
                             "hi, n, prior, nuisance)", what, k);
                Py_XDECREF(kb);
                return -1;
            }
            Py_XDECREF(kb);
        }
        v = PyDict_GetItemString(spec, "values");
        if (v) {
            Py_ssize_t n;
            double* vals = q_build_doubles(b, v, &n, what);
            if (!vals) return -1;
            if (n > 0x7FFFFFFF) { PyErr_Format(QArgumentError, "%s is too long", what); return -1; }
            ax->values = vals;
            ax->n = (int)n;
        } else {
            PyObject *lo = PyDict_GetItemString(spec, "lo"), *hi = PyDict_GetItemString(spec, "hi"),
                     *n_o = PyDict_GetItemString(spec, "n");
            if (!n_o) {
                PyErr_Format(PyExc_TypeError, "%s: an axis dict needs 'values' or 'lo', 'hi', 'n'", what);
                return -1;
            }
            if (q_parse_int(n_o, &ax->n, what) < 0) return -1;
            if (ax->n == 1 && !lo) {
                PyErr_Format(PyExc_TypeError, "%s: an axis dict needs 'lo'", what);
                return -1;
            }
            ax->lo = lo ? PyFloat_AsDouble(lo) : 0.0;
            if (PyErr_Occurred()) return -1;
            ax->hi = hi ? PyFloat_AsDouble(hi) : ax->lo;
            if (PyErr_Occurred()) return -1;
            if (ax->n > 1 && (!lo || !hi)) {
                PyErr_Format(PyExc_TypeError, "%s: an axis dict needs 'lo' and 'hi'", what);
                return -1;
            }
        }
        v = PyDict_GetItemString(spec, "prior");
        if (v && v != Py_None) {
            Py_ssize_t n;
            double* pr = q_build_doubles(b, v, &n, "prior");
            if (!pr) return -1;
            /* The header reads exactly n weights; a short list would be read
             * past its end. */
            if (n != ax->n) {
                PyErr_Format(QArgumentError, "%s: prior has %zd weights, the axis has %d points",
                             what, n, ax->n);
                return -1;
            }
            ax->prior = pr;
        }
        v = PyDict_GetItemString(spec, "nuisance");
        if (v) {
            int t = PyObject_IsTrue(v);
            if (t < 0) return -1;
            ax->nuisance = t ? true : false;
        }
        return 0;
    }
    {
        Py_ssize_t n;
        double* vals = q_build_doubles(b, spec, &n, what);
        if (!vals) return -1;
        if (n > 0x7FFFFFFF) { PyErr_Format(QArgumentError, "%s is too long", what); return -1; }
        ax->values = vals;
        ax->n = (int)n;
    }
    return 0;
}

static int q_parse_axes(q_build* b, PyObject* list, yqst_axis* axes, int max, int* n_out,
                        const char* what) {
    Py_ssize_t n, i;
    char label[48];
    if (!PyList_Check(list) && !PyTuple_Check(list)) {
        PyErr_Format(PyExc_TypeError, "%s must be a list of axes", what);
        return -1;
    }
    n = PySequence_Size(list);
    if (n < 1 || n > max) {
        PyErr_Format(QArgumentError, "%s needs 1 to %d axes, got %zd", what, max, n);
        return -1;
    }
    for (i = 0; i < n; i++) {
        PyObject* it = PySequence_GetItem(list, i);
        int rc;
        if (!it) return -1;
        PyOS_snprintf(label, sizeof(label), "%s[%d]", what, (int)i);
        rc = q_parse_axis(b, it, &axes[i], label);
        Py_DECREF(it);
        if (rc < 0) return -1;
    }
    *n_out = (int)n;
    return 0;
}

static int q_build_desc(PyObject* args, PyObject* kwds, q_build* b) {
    static char* kw[] = { "stim", "params", "pf", "pf_fn", "pf_batch", "n_outcomes",
                          "joint_prior", "select", "select_param", "select_quantile",
                          "tiebreak", "tie_tolerance", "rng", "subset_size",
                          "stop_trials", "stop_entropy", "stop_sd", "stop_sd_param",
                          "no_table", NULL };
    PyObject *stim, *params, *pf_fn = Py_None, *pf_batch = Py_None, *joint = Py_None,
             *rng = Py_None;
    int pf = -1, select = 0, tiebreak = 0, no_table = 0;
    yqst_desc* d = &b->d;

    memset(b, 0, sizeof(*b));
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO|$iOOiOiididOiiddip", kw,
                                     &stim, &params, &pf, &pf_fn, &pf_batch,
                                     &d->n_outcomes, &joint, &select, &d->select_param,
                                     &d->select_quantile, &tiebreak, &d->tie_tolerance,
                                     &rng, &d->subset_size, &d->stop_trials,
                                     &d->stop_entropy, &d->stop_sd, &d->stop_sd_param,
                                     &no_table))
        return -1;
    if (q_parse_axes(b, stim, d->stim, YQST_MAX_STIM_DIMS, &d->n_stim, "stim") < 0 ||
        q_parse_axes(b, params, d->param, YQST_MAX_PARAMS, &d->n_param, "params") < 0)
        goto fail;

    if (pf_fn != Py_None) {
        if (!PyCallable_Check(pf_fn)) { PyErr_SetString(PyExc_TypeError, "pf_fn must be callable"); goto fail; }
        b->pf_fn = pf_fn;
        d->pf_fn = q_tramp_pf;
    }
    if (pf_batch != Py_None) {
        if (!PyCallable_Check(pf_batch)) { PyErr_SetString(PyExc_TypeError, "pf_batch must be callable"); goto fail; }
        b->pf_batch = pf_batch;
        d->pf_batch = q_tramp_batch;
    }
    if (rng != Py_None) {
        if (!PyCallable_Check(rng)) { PyErr_SetString(PyExc_TypeError, "rng must be callable"); goto fail; }
        b->rng = rng;
        d->rng = q_tramp_rng;
    }
    /* A callable implies the custom model, and a custom model with no count
     * is the binary one; both are the obvious reading and cost nothing. */
    if (pf < 0) pf = (b->pf_fn || b->pf_batch) ? YQST_PF_CUSTOM : YQST_PF_GUMBEL;
    d->pf = (yqst_pf)pf;
    if (pf == YQST_PF_CUSTOM && d->n_outcomes == 0) d->n_outcomes = 2;
    d->select = (yqst_select)select;
    d->tiebreak = (yqst_tiebreak)tiebreak;
    d->no_table = no_table ? true : false;

    if (joint != Py_None) {
        Py_ssize_t P = 1, n;
        int i;
        double* jp;
        for (i = 0; i < d->n_param; i++) P *= (d->param[i].n > 0 ? d->param[i].n : 1);
        jp = q_build_doubles(b, joint, &n, "joint_prior");
        if (!jp) goto fail;
        if (n != P) {
            PyErr_Format(QArgumentError, "joint_prior has %zd weights, the parameter grid "
                         "has %zd points", n, P);
            goto fail;
        }
        d->joint_prior = jp;
    }
    return 0;
fail:
    q_build_free(b);
    return -1;
}

/* --- lifecycle -------------------------------------------------------- */

static int q_open_enter(QuestObject* o);

/* Open (data == NULL) or load a snapshot into `self` from the desc in
 * args/kwds. Both build the table, so both call a Python model. */
static int q_setup(PyObject* self, PyObject* args, PyObject* kwds, const char* data,
                   size_t len) {
    QuestObject* o = QO(self);
    q_build b;
    bool ok;
    int may_cb;

    if (q_enter(o) < 0) return -1;
    o->busy = 0;
    yqst_close(&o->q);
    q_clear(self);
    o->pm_ptr = NULL;
    o->pm_P = 0;

    if (q_build_desc(args, kwds, &b) < 0) return -1;
    b.d.pf_ctx = self;
    b.d.rng_ctx = self;
    if (b.pf_fn || b.pf_batch) { o->pf_obj = b.pf_fn ? b.pf_fn : b.pf_batch; Py_INCREF(o->pf_obj); }
    if (b.rng) { o->rng_obj = b.rng; Py_INCREF(b.rng); }
    o->pf_batch = b.pf_batch != NULL;
    o->no_table = b.d.no_table;
    o->n_stim_dims = b.d.n_stim;
    o->n_param_dims = b.d.n_param;

    /* open() builds the table, S*P model evaluations: a Python pf needs the
     * GIL for each, a built-in needs nothing from the interpreter. */
    may_cb = o->pf_obj != NULL;
    o->busy = 1;
    if (may_cb) {
        ok = data ? yqst_open_saved(&o->q, &b.d, data, len) : yqst_open(&o->q, &b.d);
    } else {
        Py_BEGIN_ALLOW_THREADS
        ok = data ? yqst_open_saved(&o->q, &b.d, data, len) : yqst_open(&o->q, &b.d);
        Py_END_ALLOW_THREADS
    }
    q_build_free(&b);
    if (q_leave(o) < 0) {
        yqst_close(&o->q);
        return -1;
    }
    if (!ok) {
        const char* msg = yqst_error(&o->q);
        /* A load that fails on a desc or snapshot mismatch is Error, as in
         * ysp.trials; an open that fails is a bad desc. */
        PyObject* exc = strstr(msg, yqst_strerror(YQST_ERR_MEMORY)) ? QOutOfMemory
                      : data ? QError : QArgumentError;
        PyErr_SetString(exc, msg);
        return -1;
    }
    return 0;
}

static int Quest_init(PyObject* self, PyObject* args, PyObject* kwds) {
    return q_setup(self, args, kwds, NULL, 0);
}

static PyObject* Quest_save(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    QuestObject* o = QO(self);
    size_t n;
    char* buf;
    int rc;
    PyObject* r;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    n = yqst_save_size(&o->q);
    buf = (char*)PyMem_Malloc(n > 0 ? n : 1);
    if (!buf) return PyErr_NoMemory();
    rc = yqst_save(&o->q, buf, n);
    if (rc < 0) { PyMem_Free(buf); return q_fail(rc); }
    r = PyBytes_FromStringAndSize(buf, rc);
    PyMem_Free(buf);
    return r;
}

/* Quest.load(data, stim, params, **desc): a new Quest resumed from save(). */
static PyObject* Quest_load(PyObject* cls, PyObject* args, PyObject* kwds) {
    PyObject *bytes, *self, *rest;
    Py_ssize_t na = PyTuple_Size(args);
    int rc;
    if (na < 1) {
        PyErr_SetString(PyExc_TypeError, "load(data, stim, params, **desc) needs the snapshot bytes");
        return NULL;
    }
    bytes = PyBytes_FromObject(PyTuple_GetItem(args, 0));
    if (!bytes) return NULL;
    rest = PyTuple_GetSlice(args, 1, na);
    self = rest ? PyObject_CallMethod(cls, "__new__", "O", cls) : NULL;
    if (!self) { Py_XDECREF(rest); Py_DECREF(bytes); return NULL; }
    rc = q_setup(self, rest, kwds, PyBytes_AsString(bytes), (size_t)PyBytes_Size(bytes));
    Py_DECREF(rest);
    Py_DECREF(bytes);
    if (rc < 0) { Py_DECREF(self); return NULL; }
    return self;
}

static void Quest_dealloc(PyObject* self) {
    PyTypeObject* tp = Py_TYPE(self);
    freefunc tp_free;
    PyObject_GC_UnTrack(self);
    yqst_close(&QO(self)->q);
    q_clear(self);
    tp_free = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    tp_free(self);
    Py_DECREF(tp);
}

static PyObject* Quest_memory_size(PyObject* Py_UNUSED(cls), PyObject* args, PyObject* kwds) {
    q_build b;
    size_t n;
    if (q_build_desc(args, kwds, &b) < 0) return NULL;
    n = yqst_memory_size(&b.d);
    q_build_free(&b);
    return PyLong_FromSize_t(n);
}

static PyObject* Quest_close(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    QuestObject* o = QO(self);
    if (q_enter(o) < 0) return NULL;
    yqst_close(&o->q);
    Py_CLEAR(o->pm_view);
    o->pm_ptr = NULL;
    o->busy = 0;
    Py_RETURN_NONE;
}

static PyObject* Quest_enter(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    Py_INCREF(self);
    return self;
}

static PyObject* Quest_exit(PyObject* self, PyObject* Py_UNUSED(args)) {
    PyObject* r = Quest_close(self, NULL);
    if (!r) return NULL;
    Py_DECREF(r);
    Py_RETURN_FALSE;
}

/* A call that needs an open handle. */
static int q_open_enter(QuestObject* o) {
    if (q_enter(o) < 0) return -1;
    if (!yqst_is_open(&o->q)) {
        o->busy = 0;
        PyErr_SetString(QClosed, yqst_strerror(YQST_ERR_CLOSED));
        return -1;
    }
    return 0;
}

static int q_check_stim(QuestObject* o, long i) {
    if (i < 0 || i >= o->q.S) {
        PyErr_Format(PyExc_IndexError, "stimulus index %ld out of range 0..%d", i, o->q.S - 1);
        return -1;
    }
    return 0;
}

static int q_check_param_index(QuestObject* o, long t) {
    if (t < 0 || t >= o->q.P) {
        PyErr_Format(PyExc_IndexError, "parameter index %ld out of range 0..%d", t, o->q.P - 1);
        return -1;
    }
    return 0;
}

static int q_check_param_axis(QuestObject* o, int a) {
    if (a < 0 || a >= o->n_param_dims) {
        PyErr_Format(PyExc_IndexError, "parameter axis %d out of range 0..%d", a, o->n_param_dims - 1);
        return -1;
    }
    return 0;
}

/* --- the loop --------------------------------------------------------- */

static int q_next_may_cb(QuestObject* o) {
    return o->rng_obj != NULL || (o->pf_obj != NULL && o->no_table);
}

static PyObject* Quest_next(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    QuestObject* o = QO(self);
    int rc;
    if (q_open_enter(o) < 0) return NULL;
    if (q_next_may_cb(o)) {
        rc = yqst_next(&o->q);
    } else {
        Py_BEGIN_ALLOW_THREADS
        rc = yqst_next(&o->q);
        Py_END_ALLOW_THREADS
    }
    if (q_leave(o) < 0) return NULL;
    if (rc < 0) return q_fail(rc);
    return PyLong_FromLong(rc);
}

static PyObject* Quest_next_subset(PyObject* self, PyObject* arg) {
    QuestObject* o = QO(self);
    Py_ssize_t n = PySequence_Size(arg);
    int* idx;
    int rc;
    if (n < 0) return NULL;
    if (n < 1) { PyErr_SetString(QArgumentError, "the subset is empty"); return NULL; }
    idx = (int*)PyMem_Malloc((size_t)n * sizeof(int));
    if (!idx) return PyErr_NoMemory();
    if (q_ints_into(arg, idx, n, "subset") < 0) { PyMem_Free(idx); return NULL; }
    if (q_open_enter(o) < 0) { PyMem_Free(idx); return NULL; }
    if (n > o->q.S) {
        o->busy = 0;
        PyMem_Free(idx);
        PyErr_Format(QArgumentError, "the subset has %zd indices; S is %d", n, o->q.S);
        return NULL;
    }
    if (q_next_may_cb(o)) {
        rc = yqst_next_subset(&o->q, idx, (int)n);
    } else {
        Py_BEGIN_ALLOW_THREADS
        rc = yqst_next_subset(&o->q, idx, (int)n);
        Py_END_ALLOW_THREADS
    }
    PyMem_Free(idx);
    if (q_leave(o) < 0) return NULL;
    if (rc < 0) return q_fail(rc);
    return PyLong_FromLong(rc);
}

static PyObject* Quest_expected_entropy(PyObject* self, PyObject* arg) {
    QuestObject* o = QO(self);
    long i = PyLong_AsLong(arg);
    double h;
    if (i == -1 && PyErr_Occurred()) return NULL;
    if (q_open_enter(o) < 0) return NULL;
    if (q_check_stim(o, i) < 0) { o->busy = 0; return NULL; }
    h = yqst_expected_entropy(&o->q, (int)i);
    if (q_leave(o) < 0) return NULL;
    return PyFloat_FromDouble(h);
}

static PyObject* Quest_update(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "index", "outcome", NULL };
    QuestObject* o = QO(self);
    int i, k, rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "ii", kw, &i, &k)) return NULL;
    if (q_enter(o) < 0) return NULL;
    if (o->pf_obj && o->no_table) {
        rc = yqst_update(&o->q, i, k);
    } else {
        Py_BEGIN_ALLOW_THREADS
        rc = yqst_update(&o->q, i, k);
        Py_END_ALLOW_THREADS
    }
    if (q_leave(o) < 0) return NULL;
    if (rc < 0) return q_fail(rc);
    Py_RETURN_NONE;
}

static PyObject* Quest_update_values(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "stim", "outcome", NULL };
    QuestObject* o = QO(self);
    PyObject* stim_o;
    double stim[YQST_MAX_STIM_DIMS];
    int k, rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "Oi", kw, &stim_o, &k)) return NULL;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (q_doubles_into(stim_o, stim, o->n_stim_dims, "stim") < 0) return NULL;
    if (q_enter(o) < 0) return NULL;
    if (o->pf_obj) {
        rc = yqst_update_values(&o->q, stim, k);
    } else {
        Py_BEGIN_ALLOW_THREADS
        rc = yqst_update_values(&o->q, stim, k);
        Py_END_ALLOW_THREADS
    }
    if (q_leave(o) < 0) return NULL;
    if (rc < 0) return q_fail(rc);
    Py_RETURN_NONE;
}

/* --- estimates -------------------------------------------------------- */

static PyObject* Quest_estimate(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "how", NULL };
    QuestObject* o = QO(self);
    int how = YQST_EST_MEAN, rc, i;
    double est[YQST_MAX_PARAMS];
    PyObject* list;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|i", kw, &how)) return NULL;
    if (q_open_enter(o) < 0) return NULL;
    rc = yqst_estimate(&o->q, (yqst_estimator)how, est);
    o->busy = 0;
    if (rc < 0) return q_fail(rc);
    list = PyList_New(o->n_param_dims);
    if (!list) return NULL;
    for (i = 0; i < o->n_param_dims; i++) {
        PyObject* f = PyFloat_FromDouble(est[i]);
        if (!f) { Py_DECREF(list); return NULL; }
        PyList_SetItem(list, i, f);
    }
    return list;
}

static PyObject* Quest_quantile(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "axis", "p", NULL };
    QuestObject* o = QO(self);
    int axis;
    double p, v;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "id", kw, &axis, &p)) return NULL;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (q_check_param_axis(o, axis) < 0) return NULL;
    if (!(p > 0.0 && p < 1.0)) { PyErr_SetString(QArgumentError, "p must be in (0, 1)"); return NULL; }
    v = yqst_quantile(&o->q, axis, p);
    return PyFloat_FromDouble(v);
}

static PyObject* Quest_sd(PyObject* self, PyObject* arg) {
    QuestObject* o = QO(self);
    long axis = PyLong_AsLong(arg);
    if (axis == -1 && PyErr_Occurred()) return NULL;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (q_check_param_axis(o, (int)axis) < 0) return NULL;
    return PyFloat_FromDouble(yqst_sd(&o->q, (int)axis));
}

static PyObject* Quest_marginal(PyObject* self, PyObject* arg) {
    QuestObject* o = QO(self);
    long axis = PyLong_AsLong(arg);
    double* buf;
    int n;
    PyObject* r;
    if (axis == -1 && PyErr_Occurred()) return NULL;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (q_check_param_axis(o, (int)axis) < 0) return NULL;
    n = yqst_param_axis_n(&o->q, (int)axis);
    if (n < 0) return q_fail(n);
    buf = (double*)PyMem_Malloc((size_t)n * sizeof(double));
    if (!buf) return PyErr_NoMemory();
    n = yqst_marginal(&o->q, (int)axis, buf);
    if (n < 0) { PyMem_Free(buf); return q_fail(n); }
    r = q_view(buf, n, NULL);
    PyMem_Free(buf);
    return r;
}

/* Axis sizes are fixed at open, so reading them beside a running Async
 * thread is safe. */
static PyObject* q_shape(QuestObject* o, int stim) {
    int n = stim ? o->n_stim_dims : o->n_param_dims, i;
    PyObject* t;
    if (!yqst_is_open(&o->q)) return q_fail(YQST_ERR_CLOSED);
    t = PyTuple_New(n);
    if (!t) return NULL;
    for (i = 0; i < n; i++) {
        int k = stim ? yqst_stim_axis_n(&o->q, i) : yqst_param_axis_n(&o->q, i);
        PyObject* v;
        if (k < 0) { Py_DECREF(t); return q_fail(k); }
        v = PyLong_FromLong(k);
        if (!v) { Py_DECREF(t); return NULL; }
        PyTuple_SetItem(t, i, v);
    }
    return t;
}

static PyObject* q_param_shape(QuestObject* o) { return q_shape(o, 0); }

static PyObject* Quest_posterior(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "shaped", NULL };
    QuestObject* o = QO(self);
    int shaped = 0;
    const double* p;
    PyObject *shape = NULL, *r;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|p", kw, &shaped)) return NULL;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    p = yqst_posterior(&o->q);
    if (!p) return q_fail(YQST_ERR_CLOSED);
    if (shaped && !(shape = q_param_shape(o))) return NULL;
    r = q_view(p, o->q.P, shape);
    Py_XDECREF(shape);
    return r;
}

static PyObject* Quest_entropy(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    QuestObject* o = QO(self);
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    return PyFloat_FromDouble(yqst_entropy(&o->q));
}

/* --- model ------------------------------------------------------------ */

static PyObject* q_probs(QuestObject* o, const double* p) {
    return q_tuple_d(p, o->q.K);
}

static PyObject* Quest_p(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "index", "params", NULL };
    QuestObject* o = QO(self);
    int i, rc;
    PyObject* params_o;
    double params[YQST_MAX_PARAMS], p[YQST_MAX_OUTCOMES];
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "iO", kw, &i, &params_o)) return NULL;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (q_doubles_into(params_o, params, o->n_param_dims, "params") < 0) return NULL;
    if (q_enter(o) < 0) return NULL;
    rc = yqst_p(&o->q, i, params, p);
    if (q_leave(o) < 0) return NULL;
    if (rc < 0) return q_fail(rc);
    return q_probs(o, p);
}

static PyObject* Quest_p_values(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "stim", "params", NULL };
    QuestObject* o = QO(self);
    int rc;
    PyObject *stim_o, *params_o;
    double stim[YQST_MAX_STIM_DIMS], params[YQST_MAX_PARAMS], p[YQST_MAX_OUTCOMES];
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO", kw, &stim_o, &params_o)) return NULL;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (q_doubles_into(stim_o, stim, o->n_stim_dims, "stim") < 0) return NULL;
    if (q_doubles_into(params_o, params, o->n_param_dims, "params") < 0) return NULL;
    if (q_enter(o) < 0) return NULL;
    rc = yqst_p_values(&o->q, stim, params, p);
    if (q_leave(o) < 0) return NULL;
    if (rc < 0) return q_fail(rc);
    return q_probs(o, p);
}

static PyObject* Quest_simulate(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "index", "params", "u", NULL };
    QuestObject* o = QO(self);
    int i, rc;
    double u;
    PyObject* params_o;
    double params[YQST_MAX_PARAMS];
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "iOd", kw, &i, &params_o, &u)) return NULL;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (q_doubles_into(params_o, params, o->n_param_dims, "params") < 0) return NULL;
    if (q_enter(o) < 0) return NULL;
    rc = yqst_simulate(&o->q, i, params, u);
    if (q_leave(o) < 0) return NULL;
    if (rc < 0) return q_fail(rc);
    return PyLong_FromLong(rc);
}

/* --- grids ------------------------------------------------------------ */

static PyObject* Quest_stim_value(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "index", "axis", NULL };
    QuestObject* o = QO(self);
    int i, axis = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i|i", kw, &i, &axis)) return NULL;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (q_check_stim(o, i) < 0) return NULL;
    if (axis < 0 || axis >= o->n_stim_dims) {
        PyErr_Format(PyExc_IndexError, "stimulus axis %d out of range", axis);
        return NULL;
    }
    return PyFloat_FromDouble(yqst_stim_value(&o->q, i, axis));
}

static PyObject* Quest_stim_values(PyObject* self, PyObject* arg) {
    QuestObject* o = QO(self);
    long i = PyLong_AsLong(arg);
    double v[YQST_MAX_STIM_DIMS];
    int n;
    if (i == -1 && PyErr_Occurred()) return NULL;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (q_check_stim(o, i) < 0) return NULL;
    n = yqst_stim_values(&o->q, (int)i, v);
    if (n < 0) return q_fail(n);
    return q_tuple_d(v, n);
}

static PyObject* Quest_stim_index(PyObject* self, PyObject* arg) {
    QuestObject* o = QO(self);
    int sub[YQST_MAX_STIM_DIMS], rc;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (q_ints_into(arg, sub, o->n_stim_dims, "sub") < 0) return NULL;
    rc = yqst_stim_index(&o->q, sub);
    if (rc < 0) return q_fail(rc);
    return PyLong_FromLong(rc);
}

static PyObject* Quest_stim_nearest(PyObject* self, PyObject* arg) {
    QuestObject* o = QO(self);
    double v[YQST_MAX_STIM_DIMS];
    int rc;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (q_doubles_into(arg, v, o->n_stim_dims, "stim") < 0) return NULL;
    rc = yqst_stim_nearest(&o->q, v);
    if (rc < 0) return q_fail(rc);
    return PyLong_FromLong(rc);
}

static PyObject* Quest_param_value(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "index", "axis", NULL };
    QuestObject* o = QO(self);
    int t, axis = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i|i", kw, &t, &axis)) return NULL;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (q_check_param_index(o, t) < 0 || q_check_param_axis(o, axis) < 0) return NULL;
    return PyFloat_FromDouble(yqst_param_value(&o->q, t, axis));
}

static PyObject* Quest_param_values(PyObject* self, PyObject* arg) {
    QuestObject* o = QO(self);
    long t = PyLong_AsLong(arg);
    double v[YQST_MAX_PARAMS];
    int n;
    if (t == -1 && PyErr_Occurred()) return NULL;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (q_check_param_index(o, t) < 0) return NULL;
    n = yqst_param_values(&o->q, (int)t, v);
    if (n < 0) return q_fail(n);
    return q_tuple_d(v, n);
}

static PyObject* Quest_param_index(PyObject* self, PyObject* arg) {
    QuestObject* o = QO(self);
    int sub[YQST_MAX_PARAMS], rc;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (q_ints_into(arg, sub, o->n_param_dims, "sub") < 0) return NULL;
    rc = yqst_param_index(&o->q, sub);
    if (rc < 0) return q_fail(rc);
    return PyLong_FromLong(rc);
}

/* --- history ---------------------------------------------------------- */

static PyObject* Quest_history(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    QuestObject* o = QO(self);
    const yqst_trial* h;
    int n = 0, i;
    PyObject* list;
    if (q_open_enter(o) < 0) return NULL;
    o->busy = 0;
    h = yqst_history(&o->q, &n);
    list = PyList_New(n);
    if (!list) return NULL;
    for (i = 0; i < n; i++) {
        PyObject* stim = q_tuple_d(h[i].stim, o->n_stim_dims);
        PyObject* t = stim ? PyObject_CallFunction(QTrialType, "Oiii", stim, h[i].stim_index,
                                                   h[i].proposed_index, (int)h[i].outcome)
                           : NULL;
        Py_XDECREF(stim);
        if (!t) { Py_DECREF(list); return NULL; }
        PyList_SetItem(list, i, t);
    }
    return list;
}

/* --- properties ------------------------------------------------------- */

/* Properties read plain fields or cheap accessors, but still refuse while
 * another thread holds the handle or an Async owns it: the thread writes
 * those fields. */
static int q_prop_enter(PyObject* self) {
    QuestObject* o = QO(self);
    if (q_enter(o) < 0) return -1;
    o->busy = 0;
    return 0;
}

static PyObject* Quest_get_done(PyObject* self, void* Py_UNUSED(c)) {
    if (q_prop_enter(self) < 0) return NULL;
    return PyBool_FromLong(yqst_done(&QO(self)->q));
}
static PyObject* Quest_get_stop_reason(PyObject* self, void* Py_UNUSED(c)) {
    if (q_prop_enter(self) < 0) return NULL;
    return q_enum(QStopEnum, (long)yqst_stop_reason(&QO(self)->q));
}
static PyObject* Quest_get_n_trials(PyObject* self, void* Py_UNUSED(c)) {
    if (q_prop_enter(self) < 0) return NULL;
    return PyLong_FromLong(yqst_n_trials(&QO(self)->q));
}
static PyObject* q_count(PyObject* self, int v) {
    if (q_prop_enter(self) < 0) return NULL;
    if (v < 0) return q_fail(v);
    return PyLong_FromLong(v);
}
static PyObject* Quest_get_n_stim(PyObject* self, void* Py_UNUSED(c)) {
    return q_count(self, yqst_n_stim(&QO(self)->q));
}
static PyObject* Quest_get_n_param(PyObject* self, void* Py_UNUSED(c)) {
    return q_count(self, yqst_n_param(&QO(self)->q));
}
static PyObject* Quest_get_n_outcomes(PyObject* self, void* Py_UNUSED(c)) {
    return q_count(self, yqst_n_outcomes(&QO(self)->q));
}
static PyObject* Quest_get_is_open(PyObject* self, void* Py_UNUSED(c)) {
    return PyBool_FromLong(yqst_is_open(&QO(self)->q));
}
static PyObject* Quest_get_param_shape(PyObject* self, void* Py_UNUSED(c)) {
    return q_param_shape(QO(self));
}
static PyObject* Quest_get_stim_shape(PyObject* self, void* Py_UNUSED(c)) {
    return q_shape(QO(self), 1);
}
static PyObject* Quest_get_releases_gil(PyObject* self, void* Py_UNUSED(c)) {
    QuestObject* o = QO(self);
    return Py_BuildValue("{s:O,s:O,s:O}",
                         "next", q_next_may_cb(o) ? Py_False : Py_True,
                         "update", (o->pf_obj && o->no_table) ? Py_False : Py_True,
                         "update_values", o->pf_obj ? Py_False : Py_True);
}

static PyGetSetDef Quest_getset[] = {
    { "done", Quest_get_done, NULL, "True once a stop criterion has fired.", NULL },
    { "stop_reason", Quest_get_stop_reason, NULL, "Which criterion fired (Stop).", NULL },
    { "n_trials", Quest_get_n_trials, NULL, "Trials recorded.", NULL },
    { "n_stim", Quest_get_n_stim, NULL, "S, points in the joint stimulus grid.", NULL },
    { "n_param", Quest_get_n_param, NULL, "P, points in the joint parameter grid.", NULL },
    { "n_outcomes", Quest_get_n_outcomes, NULL, "K, outcomes per trial.", NULL },
    { "is_open", Quest_get_is_open, NULL, "True between construction and close().", NULL },
    { "stim_shape", Quest_get_stim_shape, NULL, "Points per stimulus axis.", NULL },
    { "param_shape", Quest_get_param_shape, NULL,
      "Points per parameter axis; the posterior is this shape in C order.", NULL },
    { "releases_gil", Quest_get_releases_gil, NULL,
      "Which calls release the GIL for this configuration, as a dict.", NULL },
    { NULL }
};

static PyMethodDef Quest_methods[] = {
    { "memory_size", (PyCFunction)(void (*)(void))Quest_memory_size,
      METH_VARARGS | METH_KEYWORDS | METH_STATIC,
      "memory_size(stim, params, **desc) -> int: bytes the arena of a Quest with "
      "this desc needs (yqst_memory_size), 0 for a desc the constructor would "
      "reject. Takes the constructor's arguments; calls no callback." },
    { "next", Quest_next, METH_NOARGS,
      "next() -> int: the stimulus grid index to show next. Cached until the "
      "next update." },
    { "next_subset", Quest_next_subset, METH_O,
      "next_subset(indices) -> int: next() restricted to these stimulus indices." },
    { "expected_entropy", Quest_expected_entropy, METH_O,
      "expected_entropy(i) -> float: expected posterior entropy in bits after "
      "showing stimulus i." },
    { "update", (PyCFunction)(void (*)(void))Quest_update, METH_VARARGS | METH_KEYWORDS,
      "update(index, outcome): record outcome for the stimulus at grid index." },
    { "update_values", (PyCFunction)(void (*)(void))Quest_update_values, METH_VARARGS | METH_KEYWORDS,
      "update_values(stim, outcome): the same for a stimulus given by value, on "
      "or off the grid; recorded with stim_index -1." },
    { "estimate", (PyCFunction)(void (*)(void))Quest_estimate, METH_VARARGS | METH_KEYWORDS,
      "estimate(how=EST_MEAN) -> list[float]: one value per parameter axis." },
    { "quantile", (PyCFunction)(void (*)(void))Quest_quantile, METH_VARARGS | METH_KEYWORDS,
      "quantile(axis, p) -> float: marginal quantile, a grid point." },
    { "sd", Quest_sd, METH_O, "sd(axis) -> float: marginal posterior sd." },
    { "marginal", Quest_marginal, METH_O,
      "marginal(axis) -> memoryview('d'): a copy of the marginal posterior." },
    { "posterior", (PyCFunction)(void (*)(void))Quest_posterior, METH_VARARGS | METH_KEYWORDS,
      "posterior(shaped=False) -> memoryview('d'): a COPY of the joint posterior, "
      "P values in C order (last axis fastest); shaped=True casts it to "
      "param_shape. numpy.asarray() views the copy without copying again." },
    { "entropy", Quest_entropy, METH_NOARGS,
      "entropy() -> float: posterior entropy in bits, over the non-nuisance axes." },
    { "p", (PyCFunction)(void (*)(void))Quest_p, METH_VARARGS | METH_KEYWORDS,
      "p(index, params) -> tuple[float]: the K outcome probabilities at a grid "
      "stimulus and any parameter values." },
    { "p_values", (PyCFunction)(void (*)(void))Quest_p_values, METH_VARARGS | METH_KEYWORDS,
      "p_values(stim, params) -> tuple[float]: the same at any stimulus value." },
    { "simulate", (PyCFunction)(void (*)(void))Quest_simulate, METH_VARARGS | METH_KEYWORDS,
      "simulate(index, params, u) -> int: the smallest outcome k whose cumulative "
      "probability exceeds u, u in [0, 1) from your generator." },
    { "stim_value", (PyCFunction)(void (*)(void))Quest_stim_value, METH_VARARGS | METH_KEYWORDS,
      "stim_value(index, axis=0) -> float" },
    { "stim_values", Quest_stim_values, METH_O, "stim_values(index) -> tuple[float]" },
    { "stim_index", Quest_stim_index, METH_O,
      "stim_index(sub) -> int: flat index of per-axis indices." },
    { "stim_nearest", Quest_stim_nearest, METH_O,
      "stim_nearest(values) -> int: the grid point nearest a value on each axis." },
    { "param_value", (PyCFunction)(void (*)(void))Quest_param_value, METH_VARARGS | METH_KEYWORDS,
      "param_value(index, axis=0) -> float" },
    { "param_values", Quest_param_values, METH_O, "param_values(index) -> tuple[float]" },
    { "param_index", Quest_param_index, METH_O,
      "param_index(sub) -> int: flat index of per-axis indices." },
    { "history", Quest_history, METH_NOARGS,
      "history() -> list[Trial]: Trial(stim, stim_index, proposed_index, outcome) "
      "named tuples, oldest first." },
    { "save", Quest_save, METH_NOARGS,
      "save() -> bytes: a snapshot of the posterior, history, pending proposal, "
      "tie and stop state. The table is not in it; load() rebuilds it." },
    { "load", (PyCFunction)(void (*)(void))Quest_load, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
      "Quest.load(data, stim, params, **desc) -> Quest: resume from save(). The "
      "desc must match on every number except the priors, and re-supplies the "
      "model callable and rng; put your generator's state back yourself. A "
      "mismatch raises Error naming the first field that differs." },
    { "close", Quest_close, METH_NOARGS,
      "close(): free the arena. Idempotent; the Quest is a context manager." },
    { "__enter__", Quest_enter, METH_NOARGS, NULL },
    { "__exit__", Quest_exit, METH_VARARGS, NULL },
    { NULL }
};

static PyType_Slot Quest_slots[] = {
    { Py_tp_doc, (void*)
      "Quest(stim, params, *, pf=PF_GUMBEL, pf_fn=None, pf_batch=None, "
      "n_outcomes=0, joint_prior=None, select=SELECT_ENTROPY, select_param=0, "
      "select_quantile=0.0, tiebreak=TIE_LOWEST, tie_tolerance=0.0, rng=None, "
      "subset_size=0, stop_trials=0, stop_entropy=0.0, stop_sd=0.0, "
      "stop_sd_param=0, no_table=False)\n\n"
      "One ysp/quest.h QUEST+ run. `stim` and `params` are lists of axes; an "
      "axis is a number (fixed), a (lo, hi, n) tuple, a list or float buffer "
      "of values, or a dict from linspace()/values()/fixed() with a prior and "
      "a nuisance flag. Zero means the library default, as in C." },
    { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_init, (void*)Quest_init },
    { Py_tp_dealloc, (void*)Quest_dealloc },
    { Py_tp_traverse, (void*)q_traverse },
    { Py_tp_clear, (void*)q_clear },
    { Py_tp_methods, Quest_methods },
    { Py_tp_getset, Quest_getset },
    { 0, NULL }
};

static PyType_Spec Quest_spec = {
    .name = "ysp.quest.Quest",
    .basicsize = sizeof(QuestObject),
    .itemsize = 0,
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = Quest_slots,
};

/* ======================================================================= *
 *  Async
 * ======================================================================= */

typedef struct {
    PyObject_HEAD
    PyObject* quest;        /* the Quest; owned by the thread while running */
    int estimator;
    int below_normal;
    int pin_cpu;
    int queue_depth;
    int stopping;           /* stop() has released the GIL                 */
    int in_wait;            /* wait() calls inside the C wait right now; a
                             * restart would reset the snapshot they are
                             * about to copy, so start() refuses meanwhile */
    yqst_async a;
} AsyncObject;

#define AO(self) ((AsyncObject*)(self))

static int a_traverse(PyObject* self, visitproc visit, void* arg) {
    Py_VISIT(AO(self)->quest);
    if (g_visit_type) Py_VISIT((PyObject*)Py_TYPE(self));
    return 0;
}

static void a_stop(AsyncObject* ao) {
    if (!yqst_async_is_running(&ao->a)) return;
    ao->stopping = 1;
    /* A wait() blocked on another thread needs nothing from us: ysp/rt.h
     * 0.3.1 makes yrt_pump_wait safe against a concurrent stop, and it
     * returns once the drain reaches its seq, or STOPPED. The thread only runs yqst_update/yqst_next on a Quest whose callbacks
     * were refused at start, so it never needs the GIL: release it for the
     * drain and the join. */
    Py_BEGIN_ALLOW_THREADS
    yqst_async_close(&ao->a);
    Py_END_ALLOW_THREADS
    ao->stopping = 0;
    if (ao->quest) QO(ao->quest)->owned = 0;
}

static int a_clear(PyObject* self) {
    /* The thread reads the Quest's handle until it is joined. */
    a_stop(AO(self));
    Py_CLEAR(AO(self)->quest);
    return 0;
}

static void Async_dealloc(PyObject* self) {
    PyTypeObject* tp = Py_TYPE(self);
    freefunc tp_free;
    PyObject_GC_UnTrack(self);
    a_stop(AO(self));
    a_clear(self);
    tp_free = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    tp_free(self);
    Py_DECREF(tp);
}

static int Async_init(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "quest", "estimator", "below_normal", "pin_cpu", "queue_depth", NULL };
    AsyncObject* ao = AO(self);
    PyObject* quest;
    int estimator = YQST_EST_MEAN, below = 0, pin = 0, depth = 0, r;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|ipii", kw, &quest, &estimator, &below, &pin,
                                     &depth))
        return -1;
    if (depth < 0 || depth > YQST_ASYNC_QUEUE) {
        PyErr_Format(QArgumentError, "queue_depth must be 0..%d", YQST_ASYNC_QUEUE);
        return -1;
    }
    r = PyObject_IsInstance(quest, QQuestType);
    if (r < 0) return -1;
    if (!r) { PyErr_SetString(PyExc_TypeError, "quest must be a ysp.quest.Quest"); return -1; }
    if (yqst_async_is_running(&ao->a)) {
        PyErr_SetString(QError, "this Async is running; stop it before re-initializing");
        return -1;
    }
    if (estimator < YQST_EST_MEAN || estimator > YQST_EST_MEDIAN) {
        PyErr_SetString(QArgumentError, "estimator must be one of the EST_* values");
        return -1;
    }
    Py_INCREF(quest);
    Py_XDECREF(ao->quest);
    ao->quest = quest;
    ao->estimator = estimator;
    ao->below_normal = below;
    ao->pin_cpu = pin;
    ao->queue_depth = depth;
    return 0;
}

static int a_require_running(AsyncObject* ao) {
    if (!yqst_async_is_running(&ao->a) || ao->stopping) {
        PyErr_SetString(QClosed, "the Async is not running");
        return -1;
    }
    return 0;
}

static PyObject* Async_start(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    AsyncObject* ao = AO(self);
    QuestObject* qo;
    yqst_async_desc d;
    if (!ao->quest) { PyErr_SetString(QError, "Async was not initialized"); return NULL; }
    qo = QO(ao->quest);
    if (yqst_async_is_running(&ao->a)) { PyErr_SetString(QError, "already running"); return NULL; }
    if (ao->in_wait > 0) { PyErr_SetString(QError, "a wait() on this Async is still returning"); return NULL; }
    if (qo->rng_obj) {
        PyErr_SetString(QArgumentError, "Async cannot drive a Quest with an rng callable: the "
                        "thread would call into Python for every selection");
        return NULL;
    }
    if (qo->pf_obj && qo->no_table) {
        PyErr_SetString(QArgumentError, "Async cannot drive a Quest with a Python pf under "
                        "no_table: the thread would call into Python for every trial");
        return NULL;
    }
    if (q_enter(qo) < 0) return NULL;
    qo->busy = 0;
    if (!yqst_is_open(&qo->q)) return q_fail(YQST_ERR_CLOSED);

    /* No memset of ao->a: it is zeroed at allocation or stopped, which is what
     * start() requires. */
    memset(&d, 0, sizeof(d));
    d.quest = &qo->q;
    d.estimator = (yqst_estimator)ao->estimator;
    d.below_normal = ao->below_normal ? true : false;
    d.pin_cpu = ao->pin_cpu;
    d.queue_depth = ao->queue_depth;
    qo->owned = 1;
    /* start() runs one yqst_next() here; nothing in it can call Python. */
    if (!yqst_async_open(&ao->a, &d)) {
        qo->owned = 0;
        PyErr_SetString(QError, yqst_async_error(&ao->a));
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject* Async_stop(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    if (AO(self)->stopping) {
        PyErr_SetString(QError, "stop() is already in progress on another thread");
        return NULL;
    }
    a_stop(AO(self));
    Py_RETURN_NONE;
}

static PyObject* Async_enter(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    if (!yqst_async_is_running(&AO(self)->a)) {
        PyObject* r = Async_start(self, NULL);
        if (!r) return NULL;
        Py_DECREF(r);
    }
    Py_INCREF(self);
    return self;
}

static PyObject* Async_exit(PyObject* self, PyObject* Py_UNUSED(args)) {
    a_stop(AO(self));
    Py_RETURN_FALSE;
}

static PyObject* Async_submit(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "index", "outcome", NULL };
    AsyncObject* ao = AO(self);
    int i, k, rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "ii", kw, &i, &k)) return NULL;
    if (a_require_running(ao) < 0) return NULL;
    /* A mutex, a 40-byte copy and a signal: not worth a GIL round trip. */
    rc = yqst_async_submit(&ao->a, i, k);
    if (rc < 0) return q_fail(rc);
    return PyLong_FromLong(rc);
}

static PyObject* Async_submit_values(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "stim", "outcome", NULL };
    AsyncObject* ao = AO(self);
    PyObject* stim_o;
    double stim[YQST_MAX_STIM_DIMS];
    int k, rc;
    QuestObject* qo;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "Oi", kw, &stim_o, &k)) return NULL;
    if (a_require_running(ao) < 0) return NULL;
    qo = QO(ao->quest);
    if (qo->pf_obj) {
        PyErr_SetString(QArgumentError, "submit_values evaluates the model on the thread, and "
                        "this Quest's model is a Python callable; submit a grid index instead");
        return NULL;
    }
    if (q_doubles_into(stim_o, stim, qo->n_stim_dims, "stim") < 0) return NULL;
    rc = yqst_async_submit_values(&ao->a, stim, k);
    if (rc < 0) return q_fail(rc);
    return PyLong_FromLong(rc);
}

static PyObject* a_snapshot(AsyncObject* ao, const yqst_snapshot* s) {
    QuestObject* qo = QO(ao->quest);
    PyObject *stim, *est, *stop, *r;
    stim = q_tuple_d(s->stim, qo->n_stim_dims);
    est = q_tuple_d(s->estimate, qo->n_param_dims);
    stop = q_enum(QStopEnum, (long)s->stop);
    if (!stim || !est || !stop) { Py_XDECREF(stim); Py_XDECREF(est); Py_XDECREF(stop); return NULL; }
    r = PyObject_CallFunction(QSnapshotType, "kiOiiOOOdd", (unsigned long)s->seq, s->proposed,
                              stim, s->update_rc, s->n_trials, s->done ? Py_True : Py_False,
                              stop, est, s->entropy, s->sd);
    Py_DECREF(stim);
    Py_DECREF(est);
    Py_DECREF(stop);
    return r;
}

static PyObject* Async_poll(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    AsyncObject* ao = AO(self);
    yqst_snapshot s;
    PyObject* snap;
    int seq;
    if (!ao->quest) { PyErr_SetString(QError, "Async was not initialized"); return NULL; }
    if (ao->stopping) { PyErr_SetString(QClosed, "stop() is in progress"); return NULL; }
    seq = yqst_async_poll(&ao->a, &s);
    if (seq < 0) return q_fail(seq);
    snap = a_snapshot(ao, &s);
    if (!snap) return NULL;
    return Py_BuildValue("(iN)", seq, snap);
}

static PyObject* Async_wait(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "seq", "timeout_s", NULL };
    AsyncObject* ao = AO(self);
    unsigned long seq_l;
    PyObject* timeout_o = Py_None;
    int64_t timeout_ns = INT64_MAX;
    int rc;
    yqst_snapshot s;
    PyObject* snap;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "k|O", kw, &seq_l, &timeout_o)) return NULL;
    if (!ao->quest) { PyErr_SetString(QError, "Async was not initialized"); return NULL; }
    if (seq_l > 0x7FFFFFFFul) { PyErr_SetString(QArgumentError, "seq out of range"); return NULL; }
    if (timeout_o != Py_None) {
        double t = PyFloat_AsDouble(timeout_o);
        if (t == -1.0 && PyErr_Occurred()) return NULL;
        if (!(t >= 0.0)) { PyErr_SetString(QArgumentError, "timeout_s must be >= 0 or None"); return NULL; }
        if (t < 9.2e9) timeout_ns = (int64_t)(t * 1e9);   /* else INT64_MAX: forever */
    }
    /* One blocking call with the GIL released. A stop() on another thread
     * while it blocks is safe (ysp/rt.h 0.3.1): a seq already queued is
     * drained and returns, a seq the pump never reaches returns STOPPED. */
    ao->in_wait++;
    Py_BEGIN_ALLOW_THREADS
    rc = yqst_async_wait(&ao->a, (uint32_t)seq_l, timeout_ns, &s);
    Py_END_ALLOW_THREADS
    ao->in_wait--;
    if (rc < 0) return q_fail(rc);
    snap = a_snapshot(ao, &s);
    if (!snap) return NULL;
    return Py_BuildValue("(iN)", rc, snap);
}

static PyObject* Async_get_pending(PyObject* self, void* Py_UNUSED(c)) {
    AsyncObject* ao = AO(self);
    int n;
    if (a_require_running(ao) < 0) return NULL;
    n = yqst_async_pending(&ao->a);
    if (n < 0) return q_fail(n);
    return PyLong_FromLong(n);
}

static PyObject* Async_get_policy(PyObject* self, void* Py_UNUSED(c)) {
    return PyLong_FromLong((long)yqst_async_policy(&AO(self)->a));
}

static PyObject* Async_get_running(PyObject* self, void* Py_UNUSED(c)) {
    return PyBool_FromLong(yqst_async_is_running(&AO(self)->a) && !AO(self)->stopping);
}

static PyObject* Async_get_quest(PyObject* self, void* Py_UNUSED(c)) {
    PyObject* q = AO(self)->quest ? AO(self)->quest : Py_None;
    Py_INCREF(q);
    return q;
}

static PyGetSetDef Async_getset[] = {
    { "pending", Async_get_pending, NULL,
      "Responses queued and not yet applied. Takes the queue's mutex; for a log "
      "line, not a frame loop.", NULL },
    { "policy", Async_get_policy, NULL,
      "The thread's scheduling rung (ASYNC_NORMAL or ASYNC_BELOW_NORMAL while "
      "running, ASYNC_NONE otherwise). Log it.", NULL },
    { "running", Async_get_running, NULL, "True between start() and stop().", NULL },
    { "quest", Async_get_quest, NULL, "The Quest this Async drives.", NULL },
    { NULL }
};

static PyMethodDef Async_methods[] = {
    { "start", Async_start, METH_NOARGS,
      "start(): take over the Quest, publish the first proposal (seq 0) and "
      "start the thread. The Quest refuses every call until stop()." },
    { "stop", Async_stop, METH_NOARGS,
      "stop(): apply every queued response, join the thread and hand the Quest "
      "back. Idempotent." },
    { "submit", (PyCFunction)(void (*)(void))Async_submit, METH_VARARGS | METH_KEYWORDS,
      "submit(index, outcome) -> int: queue one trial; returns its seq. Raises "
      "Busy when the queue is full (nothing was queued; retry next frame)." },
    { "submit_values", (PyCFunction)(void (*)(void))Async_submit_values, METH_VARARGS | METH_KEYWORDS,
      "submit_values(stim, outcome) -> int: the same for an off-grid stimulus. "
      "Refused when the model is a Python callable." },
    { "poll", Async_poll, METH_NOARGS,
      "poll() -> (seq, Snapshot): the newest snapshot and the seq it accounts "
      "for. `poll()[0] >= seq` is the 'is the next stimulus ready?' test." },
    { "wait", (PyCFunction)(void (*)(void))Async_wait, METH_VARARGS | METH_KEYWORDS,
      "wait(seq, timeout_s=None) -> (seq, Snapshot): block, with the GIL "
      "released, until the thread has finished `seq`. Raises Timeout, or "
      "Closed if the thread stopped without reaching it. A stop() racing the "
      "wait drains first, so a seq already queued still returns." },
    { "__enter__", Async_enter, METH_NOARGS, NULL },
    { "__exit__", Async_exit, METH_VARARGS, NULL },
    { NULL }
};

static PyType_Slot Async_slots[] = {
    { Py_tp_doc, (void*)
      "Async(quest, estimator=EST_MEAN, below_normal=False, pin_cpu=0, queue_depth=0)\n\n"
      "yqst_async: a background C thread that owns `quest` between start() and "
      "stop(), applies submitted responses and publishes a Snapshot after each. "
      "queue_depth caps how many responses may wait (0 = ASYNC_QUEUE; 1 keeps "
      "the trial loop in lockstep with the inference). "
      "A context manager: `with Async(q) as a:` starts and stops it." },
    { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_init, (void*)Async_init },
    { Py_tp_dealloc, (void*)Async_dealloc },
    { Py_tp_traverse, (void*)a_traverse },
    { Py_tp_clear, (void*)a_clear },
    { Py_tp_methods, Async_methods },
    { Py_tp_getset, Async_getset },
    { 0, NULL }
};

static PyType_Spec Async_spec = {
    .name = "ysp.quest.Async",
    .basicsize = sizeof(AsyncObject),
    .itemsize = 0,
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = Async_slots,
};

/* ======================================================================= *
 *  Module functions
 * ======================================================================= */

/* The axis helpers return plain dicts, which is also what the constructor
 * accepts, so an axis is inspectable and needs no type of its own. */
static PyObject* q_axis_dict(PyObject* base, PyObject* prior, int nuisance) {
    if (prior && prior != Py_None && PyDict_SetItemString(base, "prior", prior) < 0) goto fail;
    if (nuisance && PyDict_SetItemString(base, "nuisance", Py_True) < 0) goto fail;
    return base;
fail:
    Py_DECREF(base);
    return NULL;
}

static PyObject* mod_linspace(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "lo", "hi", "n", "prior", "nuisance", NULL };
    double lo, hi;
    int n, nuisance = 0;
    PyObject* prior = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "ddi|$Op", kw, &lo, &hi, &n, &prior, &nuisance))
        return NULL;
    return q_axis_dict(Py_BuildValue("{s:d,s:d,s:i}", "lo", lo, "hi", hi, "n", n), prior, nuisance);
}

static PyObject* mod_values(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "values", "prior", "nuisance", NULL };
    PyObject *values, *prior = Py_None, *list;
    int nuisance = 0;
    Py_ssize_t n, i;
    double* v;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|$Op", kw, &values, &prior, &nuisance))
        return NULL;
    v = q_doubles(values, &n, "values");
    if (!v) return NULL;
    list = PyList_New(n);
    if (!list) { PyMem_Free(v); return NULL; }
    for (i = 0; i < n; i++) PyList_SetItem(list, i, PyFloat_FromDouble(v[i]));
    PyMem_Free(v);
    return q_axis_dict(Py_BuildValue("{s:N}", "values", list), prior, nuisance);
}

static PyObject* mod_fixed(PyObject* Py_UNUSED(m), PyObject* arg) {
    double v = PyFloat_AsDouble(arg);
    if (v == -1.0 && PyErr_Occurred()) return NULL;
    return Py_BuildValue("{s:d,s:d,s:i}", "lo", v, "hi", v, "n", 1);
}

static PyObject* mod_prior_normal(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "axis", "mean", "sd", NULL };
    PyObject *axis_o, *list;
    double mean, sd, *out;
    q_build b;
    yqst_axis ax;
    int i;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "Odd", kw, &axis_o, &mean, &sd)) return NULL;
    memset(&b, 0, sizeof(b));
    if (q_parse_axis(&b, axis_o, &ax, "axis") < 0) { q_build_free(&b); return NULL; }
    if (ax.n < 1) { q_build_free(&b); PyErr_SetString(QArgumentError, "the axis has no points"); return NULL; }
    out = (double*)PyMem_Malloc((size_t)ax.n * sizeof(double));
    if (!out) { q_build_free(&b); return PyErr_NoMemory(); }
    if (yqst_prior_normal(&ax, mean, sd, out) < 0) {
        PyMem_Free(out);
        q_build_free(&b);
        PyErr_SetString(QArgumentError, "yqst_prior_normal refused the axis or the sd");
        return NULL;
    }
    list = PyList_New(ax.n);
    if (list) for (i = 0; i < ax.n; i++) PyList_SetItem(list, i, PyFloat_FromDouble(out[i]));
    PyMem_Free(out);
    q_build_free(&b);
    return list;
}

static PyObject* mod_strerror(PyObject* Py_UNUSED(m), PyObject* arg) {
    long c = PyLong_AsLong(arg);
    if (c == -1 && PyErr_Occurred()) return NULL;
    return PyUnicode_FromString(yqst_strerror((int)c));
}

static PyMethodDef module_methods[] = {
    { "linspace", (PyCFunction)(void (*)(void))mod_linspace, METH_VARARGS | METH_KEYWORDS,
      "linspace(lo, hi, n, *, prior=None, nuisance=False) -> dict: an axis." },
    { "values", (PyCFunction)(void (*)(void))mod_values, METH_VARARGS | METH_KEYWORDS,
      "values(values, *, prior=None, nuisance=False) -> dict: an explicit axis." },
    { "fixed", mod_fixed, METH_O, "fixed(value) -> dict: a one-point axis." },
    { "prior_normal", (PyCFunction)(void (*)(void))mod_prior_normal, METH_VARARGS | METH_KEYWORDS,
      "prior_normal(axis, mean, sd) -> list[float]: unnormalized Gaussian weights "
      "over the axis's points, for an axis prior." },
    { "strerror", mod_strerror, METH_O, "strerror(code) -> str" },
    { NULL }
};

static PyModuleDef ysp_quest_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "ysp.quest",
    .m_doc = "QUEST+ Bayesian adaptive estimation on a grid (ysp/quest.h), with "
             "an optional background inference thread (Async).",
    .m_size = -1,
    .m_methods = module_methods,
};

static PyObject* q_add_exc(PyObject* m, const char* qualname, const char* attr, PyObject* base) {
    PyObject* e = PyErr_NewException(qualname, base, NULL);
    if (!e) return NULL;
    Py_INCREF(e);
    if (PyModule_AddObject(m, attr, e) < 0) { Py_DECREF(e); Py_DECREF(e); return NULL; }
    return e;
}

static PyObject* q_add_exc2(PyObject* m, const char* qualname, const char* attr, PyObject* builtin) {
    PyObject* bases = PyTuple_Pack(2, QError, builtin);
    PyObject* e;
    if (!bases) return NULL;
    e = q_add_exc(m, qualname, attr, bases);
    Py_DECREF(bases);
    return e;
}

typedef struct { const char* name; long value; } q_member;

static PyObject* q_add_enum(PyObject* m, PyObject* enum_mod, const char* cls_name,
                            const char* prefix, const q_member* mem, int n) {
    PyObject *base = NULL, *members = NULL, *args = NULL, *kwargs = NULL, *cls = NULL;
    int i;
    base = PyObject_GetAttrString(enum_mod, "IntEnum");
    if (!base) goto fail;
    members = PyList_New(n);
    if (!members) goto fail;
    for (i = 0; i < n; i++) {
        PyObject* t = Py_BuildValue("(sl)", mem[i].name, mem[i].value);
        if (!t) goto fail;
        PyList_SetItem(members, i, t);
    }
    args = Py_BuildValue("(sO)", cls_name, members);
    kwargs = Py_BuildValue("{s:s}", "module", "ysp.quest");
    if (!args || !kwargs) goto fail;
    cls = PyObject_Call(base, args, kwargs);
    if (!cls) goto fail;
    Py_INCREF(cls);
    if (PyModule_AddObject(m, cls_name, cls) < 0) { Py_DECREF(cls); goto fail; }
    for (i = 0; i < n; i++) {
        char full[64];
        PyObject* v = PyObject_GetAttrString(cls, mem[i].name);
        if (!v) goto fail;
        PyOS_snprintf(full, sizeof(full), "%s_%s", prefix, mem[i].name);
        if (PyModule_AddObject(m, full, v) < 0) { Py_DECREF(v); goto fail; }
    }
    Py_DECREF(base); Py_DECREF(members); Py_DECREF(args); Py_DECREF(kwargs);
    return cls;
fail:
    Py_XDECREF(base); Py_XDECREF(members); Py_XDECREF(args); Py_XDECREF(kwargs);
    Py_XDECREF(cls);
    return NULL;
}

static PyObject* q_namedtuple(PyObject* coll, const char* name, PyObject* fields) {
    PyObject *nt, *a, *k, *r = NULL;
    nt = PyObject_GetAttrString(coll, "namedtuple");
    a = Py_BuildValue("(sO)", name, fields);
    k = Py_BuildValue("{s:s}", "module", "ysp.quest");
    if (nt && a && k) r = PyObject_Call(nt, a, k);
    Py_XDECREF(nt); Py_XDECREF(a); Py_XDECREF(k);
    return r;
}

PyMODINIT_FUNC PyInit_quest(void) {
    static const q_member pf_m[] = { { "GUMBEL", YQST_PF_GUMBEL }, { "WEIBULL", YQST_PF_WEIBULL },
                                     { "LOGISTIC", YQST_PF_LOGISTIC }, { "NORMAL", YQST_PF_NORMAL },
                                     { "HYPSEC", YQST_PF_HYPSEC }, { "CUSTOM", YQST_PF_CUSTOM } };
    static const q_member sel_m[] = { { "ENTROPY", YQST_SELECT_ENTROPY },
                                      { "QUANTILE", YQST_SELECT_QUANTILE },
                                      { "MEAN", YQST_SELECT_MEAN }, { "MODE", YQST_SELECT_MODE } };
    static const q_member est_m[] = { { "MEAN", YQST_EST_MEAN }, { "MODE", YQST_EST_MODE },
                                      { "MEDIAN", YQST_EST_MEDIAN } };
    static const q_member tie_m[] = { { "LOWEST", YQST_TIE_LOWEST }, { "NEAREST", YQST_TIE_NEAREST },
                                      { "ALTERNATE", YQST_TIE_ALTERNATE },
                                      { "RANDOM", YQST_TIE_RANDOM } };
    static const q_member stop_m[] = { { "NONE", YQST_STOP_NONE }, { "TRIALS", YQST_STOP_TRIALS },
                                       { "ENTROPY", YQST_STOP_ENTROPY }, { "SD", YQST_STOP_SD },
                                       { "FULL", YQST_STOP_FULL } };
    PyObject *m, *type, *mod = NULL, *cls, *fields;

    m = PyModule_Create(&ysp_quest_module);
    if (!m) return NULL;

    mod = PyImport_ImportModule("sys");
    if (!mod) goto fail;
    {
        PyObject* vi = PyObject_GetAttrString(mod, "version_info");
        PyObject* minor = vi ? PySequence_GetItem(vi, 1) : NULL;
        PyObject* major = vi ? PySequence_GetItem(vi, 0) : NULL;
        if (!minor || !major) { Py_XDECREF(vi); Py_XDECREF(minor); Py_XDECREF(major); goto fail; }
        g_visit_type = PyLong_AsLong(major) > 3 || PyLong_AsLong(minor) >= 9;
        Py_DECREF(vi); Py_DECREF(minor); Py_DECREF(major);
    }
    Py_CLEAR(mod);

    type = PyType_FromSpec(&Quest_spec);
    if (!type) goto fail;
    QQuestType = type;
    Py_INCREF(type);
    if (PyModule_AddObject(m, "Quest", type) < 0) { Py_DECREF(type); goto fail; }
    type = PyType_FromSpec(&Async_spec);
    if (!type) goto fail;
    if (PyModule_AddObject(m, "Async", type) < 0) { Py_DECREF(type); goto fail; }

    if (!(QError = q_add_exc(m, "ysp.quest.Error", "Error", NULL))) goto fail;
    if (!(QArgumentError = q_add_exc2(m, "ysp.quest.ArgumentError", "ArgumentError", PyExc_ValueError))) goto fail;
    if (!(QClosed = q_add_exc(m, "ysp.quest.Closed", "Closed", QError))) goto fail;
    if (!(QFull = q_add_exc(m, "ysp.quest.Full", "Full", QError))) goto fail;
    if (!(QOutOfMemory = q_add_exc2(m, "ysp.quest.OutOfMemory", "OutOfMemory", PyExc_MemoryError))) goto fail;
    if (!(QBusy = q_add_exc(m, "ysp.quest.Busy", "Busy", QError))) goto fail;
    if (!(QTimeout = q_add_exc2(m, "ysp.quest.Timeout", "Timeout", PyExc_TimeoutError))) goto fail;

    mod = PyImport_ImportModule("enum");
    if (!mod) goto fail;
    if (!(cls = q_add_enum(m, mod, "PF", "PF", pf_m, 6))) goto fail;
    Py_DECREF(cls);
    if (!(cls = q_add_enum(m, mod, "Select", "SELECT", sel_m, 4))) goto fail;
    Py_DECREF(cls);
    if (!(cls = q_add_enum(m, mod, "Estimator", "EST", est_m, 3))) goto fail;
    Py_DECREF(cls);
    if (!(cls = q_add_enum(m, mod, "Tiebreak", "TIE", tie_m, 4))) goto fail;
    Py_DECREF(cls);
    if (!(QStopEnum = q_add_enum(m, mod, "Stop", "STOP", stop_m, 5))) goto fail;
    Py_CLEAR(mod);

    mod = PyImport_ImportModule("collections");
    if (!mod) goto fail;
    fields = Py_BuildValue("[ssss]", "stim", "stim_index", "proposed_index", "outcome");
    QTrialType = fields ? q_namedtuple(mod, "Trial", fields) : NULL;
    Py_XDECREF(fields);
    if (!QTrialType) goto fail;
    Py_INCREF(QTrialType);
    if (PyModule_AddObject(m, "Trial", QTrialType) < 0) { Py_DECREF(QTrialType); goto fail; }
    fields = Py_BuildValue("[ssssssssss]", "seq", "proposed", "stim", "update_rc", "n_trials",
                           "done", "stop", "estimate", "entropy", "sd");
    QSnapshotType = fields ? q_namedtuple(mod, "Snapshot", fields) : NULL;
    Py_XDECREF(fields);
    if (!QSnapshotType) goto fail;
    Py_INCREF(QSnapshotType);
    if (PyModule_AddObject(m, "Snapshot", QSnapshotType) < 0) { Py_DECREF(QSnapshotType); goto fail; }
    Py_CLEAR(mod);

    /* From the compiled implementation, so it names the header actually built
     * in; the tests check pyproject.toml against it. */
    if (PyModule_AddStringConstant(m, "__version__", yqst_version()) < 0) goto fail;
    PyModule_AddIntConstant(m, "ERR_ARG", YQST_ERR_ARG);
    PyModule_AddIntConstant(m, "ERR_CLOSED", YQST_ERR_CLOSED);
    PyModule_AddIntConstant(m, "ERR_FULL", YQST_ERR_FULL);
    PyModule_AddIntConstant(m, "ERR_MEMORY", YQST_ERR_MEMORY);
    PyModule_AddIntConstant(m, "ERR_BUSY", YQST_ERR_BUSY);
    PyModule_AddIntConstant(m, "ERR_TIMEOUT", YQST_ERR_TIMEOUT);
    PyModule_AddIntConstant(m, "MAX_STIM_DIMS", YQST_MAX_STIM_DIMS);
    PyModule_AddIntConstant(m, "MAX_PARAMS", YQST_MAX_PARAMS);
    PyModule_AddIntConstant(m, "MAX_OUTCOMES", YQST_MAX_OUTCOMES);
    PyModule_AddIntConstant(m, "MAX_TRIALS", YQST_MAX_TRIALS);
    PyModule_AddIntConstant(m, "ASYNC_QUEUE", YQST_ASYNC_QUEUE);
    /* ysp/rt.h's yrt_policy, named as ysp.serial and ysp.parallel name it. */
    PyModule_AddIntConstant(m, "ASYNC_NONE", YRT_POLICY_NONE);
    PyModule_AddIntConstant(m, "ASYNC_NORMAL", YRT_POLICY_NORMAL);
    PyModule_AddIntConstant(m, "ASYNC_BELOW_NORMAL", YRT_POLICY_BELOW_NORMAL);
    return m;

fail:
    Py_XDECREF(mod);
    Py_DECREF(m);
    return NULL;
}
