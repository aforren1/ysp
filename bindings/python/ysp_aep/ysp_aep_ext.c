/* ysp_aep_ext.c - CPython extension wrapping ysp/aep.h (module ysp.aep)
 *
 * A thin, dependency-free binding (no nanobind/pybind/Cython, no NumPy): it
 * needs only Python.h, matching the single-header library's zero-dependency
 * style. The library implementation, with its YAEP_ASYNC layer and the
 * ysp/rt.h pump that layer runs on, is compiled directly into this module.
 *
 * Built against the stable ABI / Limited API (Py_LIMITED_API), so one compiled
 * ysp/aep.abi3.so works across CPython >= 3.10 without recompiling per version.
 * That rules out the static PyTypeObject layout, so GP and Async are heap types
 * created with PyType_FromSpec. It also rules out the buffer protocol before
 * 3.11: arrays come in through memoryview + bytes (a copy) and go out as bytes
 * cast to a 'd' memoryview (a copy NumPy can view with frombuffer).
 *
 *     import ysp.aep as pg
 *     gp = pg.GP(lo=[0.0], hi=[1.0], acq=pg.ACQ_EAVC, target_p=0.75,
 *                fit=True, fit_every=10, stop_trials=100)
 *     while not gp.done:
 *         i, x = gp.next()
 *         gp.update(x, run_trial(x))
 *     thr, lo, hi = gp.threshold()
 *
 * THREADING. A yaep_gp handle is not thread-safe, and next/update/fit/
 * fit_step/refit/predict_*_many release the GIL. A per-object flag, read and
 * written only under the GIL, turns a second call on the same GP from another
 * Python thread into ysp.aep.Busy instead of a data race. When desc.rng is a
 * Python callable, next() keeps the GIL, because the library calls the
 * generator from inside yaep_next(); that is the only place it does.
 * The Async pump thread is a C thread that never touches the interpreter: it
 * runs yaep_update / yaep_next / yaep_threshold and publishes a plain C
 * struct, and Async refuses a GP that has a Python rng for that reason.
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x030A0000   /* target the CPython 3.10+ stable ABI */
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

/* Python.h first is safe here: pyconfig.h already defines _GNU_SOURCE on Linux
 * and raises _WIN32_WINNT past 0x0600 on Windows, which is what ysp/rt.h asks
 * for when it is not the first include. setup.py defines YAEP_ASYNC; the
 * guard keeps a hand build without it compiling the async layer too. */
#ifndef YAEP_ASYNC
#define YAEP_ASYNC
#endif
#define YSP_AEP_IMPLEMENTATION
#include "ysp/aep.h"

#include <math.h>
#include <string.h>

/* module-level exception types; Error is the base of the other five */
static PyObject* GpError;
static PyObject* GpNumeric;
static PyObject* GpNoCross;
static PyObject* GpFull;
static PyObject* GpBusy;
static PyObject* GpTimeout;

/* the GP type, for Async's type check; types.SimpleNamespace subclasses */
static PyObject* GpType;
static PyObject* HyperType;
static PyObject* SnapshotType;

typedef struct {
    PyObject_HEAD
    yaep_gp g;
    double*   cand;   /* owned copy of desc.candidates: it must outlive g */
    PyObject* rng;    /* the callable lent to desc.rng, or NULL */
    int       busy;   /* a call on g is in flight (GIL released or rng running) */
    int       owned;  /* an Async has taken g over */
} GPObject;

typedef struct {
    PyObject_HEAD
    yaep_async a;
    GPObject*   gp;          /* strong reference; g is ours while running */
    double      context[YAEP_MAX_DIMS];
    double      target;
    int         fit_in_idle, below_normal, pin_cpu;
    int         started;     /* this Async's start() succeeded and no stop yet */
    int         stopping;    /* stop() has released the GIL and is joining */
    int         waiters;     /* wait() calls blocked with the GIL released */
} AsyncObject;

/* --- errors -------------------------------------------------------------- */

/* Raise the exception matching a negative YAEP_ERR_* code, with the code
 * attached as `.code` so a caller can tell ARG from CLOSED without parsing. */
static PyObject* gp_fail(int code, const char* what) {
    PyObject* exc = GpError;
    PyObject *msg, *inst, *c;
    switch (code) {
        case YAEP_ERR_NUMERIC: exc = GpNumeric; break;
        case YAEP_ERR_NOCROSS: exc = GpNoCross; break;
        case YAEP_ERR_FULL:    exc = GpFull;    break;
        case YAEP_ERR_BUSY:    exc = GpBusy;    break;
        case YAEP_ERR_TIMEOUT: exc = GpTimeout; break;
        default: break;
    }
    msg = PyUnicode_FromFormat("%s: %s", what, yaep_strerror(code));
    if (!msg) return NULL;
    inst = PyObject_CallFunctionObjArgs(exc, msg, NULL);
    Py_DECREF(msg);
    if (!inst) return NULL;
    c = PyLong_FromLong(code);
    if (c) {
        PyObject_SetAttrString(inst, "code", c);
        Py_DECREF(c);
    }
    PyErr_SetObject(exc, inst);
    Py_DECREF(inst);
    return NULL;
}

/* The same without a library code: the binding's own refusals. */
static PyObject* gp_fail_msg(PyObject* exc, const char* msg) {
    PyErr_SetString(exc, msg);
    return NULL;
}

/* --- conversions --------------------------------------------------------- */

/* A plain number, or NULL with an exception set. */
static int gp_as_double(PyObject* o, double* out) {
    double v = PyFloat_AsDouble(o);
    if (v == -1.0 && PyErr_Occurred()) return -1;
    *out = v;
    return 0;
}

/* A sequence of at most `cap` numbers into out[]; returns the count or -1. */
static Py_ssize_t gp_small_doubles(PyObject* obj, double* out, Py_ssize_t cap,
                                   const char* name) {
    PyObject* list = PySequence_List(obj);
    Py_ssize_t n, i;
    if (!list) {
        PyErr_Format(PyExc_TypeError, "%s must be a sequence of numbers", name);
        return -1;
    }
    n = PyList_Size(list);
    if (n > cap) {
        Py_DECREF(list);
        PyErr_Format(PyExc_ValueError, "%s has %zd entries, at most %zd allowed",
                     name, n, cap);
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (gp_as_double(PyList_GetItem(list, i), &out[i]) < 0) {
            Py_DECREF(list);
            return -1;
        }
    }
    Py_DECREF(list);
    return n;
}

static Py_ssize_t gp_small_ints(PyObject* obj, int* out, Py_ssize_t cap,
                                const char* name) {
    double tmp[YAEP_MAX_DIMS];
    Py_ssize_t n, i;
    if (cap > YAEP_MAX_DIMS) cap = YAEP_MAX_DIMS;
    n = gp_small_doubles(obj, tmp, cap, name);
    for (i = 0; i < n; i++) {
        if (tmp[i] != floor(tmp[i]) || tmp[i] < 0 || tmp[i] > 1e9) {
            PyErr_Format(PyExc_ValueError, "%s must hold non-negative integers", name);
            return -1;
        }
        out[i] = (int)tmp[i];
    }
    return n;
}

/* Rows of `width` doubles from a list or tuple (flat, or of rows), appended
 * into a growing PyMem array. */
static double* gp_doubles_seq(PyObject* obj, int width, Py_ssize_t* rows_out,
                              const char* name) {
    PyObject* list = PySequence_List(obj);
    Py_ssize_t n, i, j, total = 0;
    double* out = NULL;
    int nested;
    if (!list) {
        PyErr_Format(PyExc_TypeError,
                     "%s must be a buffer of doubles or a sequence of points", name);
        return NULL;
    }
    n = PyList_Size(list);
    if (n == 0) {
        Py_DECREF(list);
        *rows_out = 0;
        out = (double*)PyMem_Malloc(sizeof(double));
        if (!out) PyErr_NoMemory();
        return out;
    }
    {
        PyObject* first = PyList_GetItem(list, 0);
        nested = PySequence_Check(first) && !PyUnicode_Check(first);
    }
    if (nested) {
        out = (double*)PyMem_Malloc((size_t)n * (size_t)width * sizeof(double));
        if (!out) { Py_DECREF(list); return (double*)PyErr_NoMemory(); }
        for (i = 0; i < n; i++) {
            PyObject* row = PySequence_List(PyList_GetItem(list, i));
            if (!row) goto fail;
            if (PyList_Size(row) != width) {
                PyErr_Format(PyExc_ValueError, "%s row %zd has %zd entries, n_dims is %d",
                             name, i, PyList_Size(row), width);
                Py_DECREF(row);
                goto fail;
            }
            for (j = 0; j < width; j++) {
                if (gp_as_double(PyList_GetItem(row, j), &out[total++]) < 0) {
                    Py_DECREF(row);
                    goto fail;
                }
            }
            Py_DECREF(row);
        }
        *rows_out = n;
    } else {
        if (n % width) {
            PyErr_Format(PyExc_ValueError,
                         "%s has %zd numbers, not a multiple of n_dims = %d",
                         name, n, width);
            Py_DECREF(list);
            return NULL;
        }
        out = (double*)PyMem_Malloc((size_t)n * sizeof(double));
        if (!out) { Py_DECREF(list); return (double*)PyErr_NoMemory(); }
        for (i = 0; i < n; i++)
            if (gp_as_double(PyList_GetItem(list, i), &out[i]) < 0) goto fail;
        *rows_out = n / width;
    }
    Py_DECREF(list);
    return out;
fail:
    Py_DECREF(list);
    PyMem_Free(out);
    return NULL;
}

/* True when a memoryview's format string names a native double. */
static int gp_format_is_double(PyObject* mv) {
    PyObject* f = PyObject_GetAttrString(mv, "format");
    int ok = 0;
    if (!f) { PyErr_Clear(); return 0; }
    if (PyUnicode_Check(f)) {
        ok = PyUnicode_CompareWithASCIIString(f, "d") == 0 ||
             PyUnicode_CompareWithASCIIString(f, "@d") == 0 ||
             PyUnicode_CompareWithASCIIString(f, "=d") == 0;
        {   /* '<d' is native on a little-endian host, which is every host the
             * header has been built on; test it rather than assume it. */
            const unsigned one = 1;
            if (!ok && *(const unsigned char*)&one == 1)
                ok = PyUnicode_CompareWithASCIIString(f, "<d") == 0;
        }
    }
    Py_DECREF(f);
    return ok;
}

/* n x width doubles from any buffer (a NumPy array, array.array('d'), a
 * memoryview) or from a sequence of points. A native-double buffer is copied
 * through bytes(), which is C-order whatever the strides; any other buffer
 * goes through memoryview.tolist(). The Limited API before 3.11 has no
 * PyObject_GetBuffer, which is why this copies. PyMem array; caller frees. */
static double* gp_doubles(PyObject* obj, int width, Py_ssize_t* rows_out,
                          const char* name) {
    PyObject* mv;
    double* out;
    if (PyList_Check(obj) || PyTuple_Check(obj))
        return gp_doubles_seq(obj, width, rows_out, name);
    mv = PyMemoryView_FromObject(obj);
    if (!mv) {
        PyErr_Clear();
        return gp_doubles_seq(obj, width, rows_out, name);
    }
    if (gp_format_is_double(mv)) {
        PyObject* nd = PyObject_GetAttrString(mv, "ndim");
        PyObject* shape = PyObject_GetAttrString(mv, "shape");
        PyObject* bytes = NULL;
        long ndim = nd ? PyLong_AsLong(nd) : -1;
        Py_ssize_t n;
        Py_XDECREF(nd);
        if (!shape || ndim < 0 || PyErr_Occurred()) goto mv_fail;
        if (ndim == 2) {
            long w = PyLong_AsLong(PyTuple_GetItem(shape, 1));
            if (w != width) {
                PyErr_Format(PyExc_ValueError, "%s has %ld columns, n_dims is %d",
                             name, w, width);
                goto mv_fail;
            }
        } else if (ndim != 1) {
            PyErr_Format(PyExc_ValueError, "%s must be 1-D or 2-D, not %ld-D",
                         name, ndim);
            goto mv_fail;
        }
        bytes = PyBytes_FromObject(mv);
        if (!bytes) goto mv_fail;
        n = PyBytes_Size(bytes) / (Py_ssize_t)sizeof(double);
        if (n % width) {
            PyErr_Format(PyExc_ValueError,
                         "%s has %zd numbers, not a multiple of n_dims = %d",
                         name, n, width);
            Py_DECREF(bytes);
            goto mv_fail;
        }
        out = (double*)PyMem_Malloc((size_t)(n ? n : 1) * sizeof(double));
        if (!out) { Py_DECREF(bytes); PyErr_NoMemory(); goto mv_fail; }
        memcpy(out, PyBytes_AsString(bytes), (size_t)n * sizeof(double));
        Py_DECREF(bytes);
        Py_DECREF(shape);
        Py_DECREF(mv);
        *rows_out = n / width;
        return out;
    mv_fail:
        Py_XDECREF(shape);
        Py_DECREF(mv);
        return NULL;
    } else {
        PyObject* list = PyObject_CallMethod(mv, "tolist", NULL);
        Py_DECREF(mv);
        if (!list) return NULL;
        out = gp_doubles_seq(list, width, rows_out, name);
        Py_DECREF(list);
        return out;
    }
}

/* One point of n_dims coordinates: a number when n_dims is 1, else any
 * sequence or buffer of n_dims numbers. Lists and tuples take a fast path,
 * because predict_p() in a Python loop is where this cost shows. */
static int gp_point(PyObject* obj, int n_dims, double* x, const char* name) {
    if (PyFloat_Check(obj) || PyLong_Check(obj)) {
        if (n_dims != 1) {
            PyErr_Format(PyExc_ValueError, "%s must have %d coordinates", name, n_dims);
            return -1;
        }
        return gp_as_double(obj, &x[0]);
    }
    if (PyList_Check(obj) || PyTuple_Check(obj)) {
        Py_ssize_t n = PySequence_Size(obj), i;
        if (n != n_dims) {
            PyErr_Format(PyExc_ValueError, "%s has %zd coordinates, n_dims is %d",
                         name, n, n_dims);
            return -1;
        }
        for (i = 0; i < n; i++) {
            PyObject* it = PySequence_GetItem(obj, i);
            int rc;
            if (!it) return -1;
            rc = gp_as_double(it, &x[i]);
            Py_DECREF(it);
            if (rc < 0) return -1;
        }
        return 0;
    }
    {
        Py_ssize_t rows;
        double* v = gp_doubles(obj, n_dims, &rows, name);
        if (!v) {
            /* A NumPy scalar is neither a list nor a buffer of one. */
            if (n_dims == 1 && PyErr_ExceptionMatches(PyExc_TypeError)) {
                PyErr_Clear();
                return gp_as_double(obj, &x[0]);
            }
            return -1;
        }
        if (rows != 1) {
            PyMem_Free(v);
            PyErr_Format(PyExc_ValueError, "%s must be one point of %d coordinates",
                         name, n_dims);
            return -1;
        }
        memcpy(x, v, (size_t)n_dims * sizeof(double));
        PyMem_Free(v);
        return 0;
    }
}

static PyObject* gp_list(const double* v, int n) {
    PyObject* l = PyList_New(n);
    int i;
    if (!l) return NULL;
    for (i = 0; i < n; i++) {
        PyObject* f = PyFloat_FromDouble(v[i]);
        if (!f) { Py_DECREF(l); return NULL; }
        PyList_SetItem(l, i, f);   /* steals f */
    }
    return l;
}

/* A fresh bytes object of n doubles whose storage the caller fills before
 * anyone else sees it; the fill may run with the GIL released. */
static PyObject* gp_bytes(Py_ssize_t n, double** data) {
    PyObject* b = PyBytes_FromStringAndSize(NULL, (n ? n : 0) * (Py_ssize_t)sizeof(double));
    if (!b) return NULL;
    *data = (double*)PyBytes_AsString(b);
    return b;
}

/* bytes -> memoryview cast to 'd'. Steals the reference to `bytes`. */
static PyObject* gp_view(PyObject* bytes) {
    PyObject* mv = PyMemoryView_FromObject(bytes);
    PyObject* cast;
    Py_DECREF(bytes);
    if (!mv) return NULL;
    cast = PyObject_CallMethod(mv, "cast", "s", "d");
    Py_DECREF(mv);
    return cast;
}

/* --- hyperparameters ------------------------------------------------------ */

static const char* const hyper_keys[] = {
    "lengthscale", "outputscale", "mean", "lengthscale_b", "outputscale_b",
    "cutpoint", "noise_sd", "lengthscale_g", "outputscale_g", "mean_g", NULL
};

static int gp_hyper_field(yaep_hyper* h, const char* key, PyObject* v) {
    if (v == Py_None) return 0;
    if (!strcmp(key, "lengthscale"))
        return gp_small_doubles(v, h->lengthscale, YAEP_MAX_DIMS, "hyper.lengthscale") < 0 ? -1 : 0;
    if (!strcmp(key, "lengthscale_b"))
        return gp_small_doubles(v, h->lengthscale_b, YAEP_MAX_DIMS, "hyper.lengthscale_b") < 0 ? -1 : 0;
    if (!strcmp(key, "lengthscale_g"))
        return gp_small_doubles(v, h->lengthscale_g, YAEP_MAX_DIMS, "hyper.lengthscale_g") < 0 ? -1 : 0;
    if (!strcmp(key, "cutpoint"))
        return gp_small_doubles(v, h->cutpoint, YAEP_MAX_OUTCOMES - 1, "hyper.cutpoint") < 0 ? -1 : 0;
    if (!strcmp(key, "outputscale"))   return gp_as_double(v, &h->outputscale);
    if (!strcmp(key, "mean"))          return gp_as_double(v, &h->mean);
    if (!strcmp(key, "outputscale_b")) return gp_as_double(v, &h->outputscale_b);
    if (!strcmp(key, "noise_sd"))      return gp_as_double(v, &h->noise_sd);
    if (!strcmp(key, "outputscale_g")) return gp_as_double(v, &h->outputscale_g);
    if (!strcmp(key, "mean_g"))        return gp_as_double(v, &h->mean_g);
    return -1;
}

/* A dict with the yaep_hyper field names, or any object carrying them as
 * attributes (ysp.aep.Hyper, a dataclass, a SimpleNamespace). A missing or
 * None field stays 0, which the header reads as "default, or fit". */
static int gp_parse_hyper(PyObject* obj, yaep_hyper* h, const char* name) {
    memset(h, 0, sizeof(*h));
    if (obj == Py_None) return 0;
    if (PyDict_Check(obj)) {
        Py_ssize_t pos = 0;
        PyObject *k, *v;
        while (PyDict_Next(obj, &pos, &k, &v)) {
            int i, found = 0;
            for (i = 0; hyper_keys[i]; i++) {
                if (PyUnicode_Check(k) && PyUnicode_CompareWithASCIIString(k, hyper_keys[i]) == 0) {
                    if (gp_hyper_field(h, hyper_keys[i], v) < 0) return -1;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                PyErr_Format(PyExc_TypeError, "%s has an unknown field %R", name, k);
                return -1;
            }
        }
        return 0;
    }
    {
        int i;
        for (i = 0; hyper_keys[i]; i++) {
            PyObject* v = PyObject_GetAttrString(obj, hyper_keys[i]);
            int rc;
            if (!v) { PyErr_Clear(); continue; }
            rc = gp_hyper_field(h, hyper_keys[i], v);
            Py_DECREF(v);
            if (rc < 0) return -1;
        }
    }
    return 0;
}

static PyObject* gp_hyper_dict(const yaep_hyper* h, const yaep_desc* d) {
    int ncut = d->lik == YAEP_LIK_ORDINAL ? d->n_outcomes - 1 : 0;
    PyObject* ls = gp_list(h->lengthscale, d->n_dims);
    PyObject* lsb = gp_list(h->lengthscale_b, d->n_dims);
    PyObject* cut = gp_list(h->cutpoint, ncut > 0 ? ncut : 0);
    PyObject* lsg = gp_list(h->lengthscale_g, d->n_dims);
    PyObject* out = NULL;
    if (ls && lsb && cut && lsg)
        out = Py_BuildValue("{s:O,s:d,s:d,s:O,s:d,s:O,s:d,s:O,s:d,s:d}",
                            "lengthscale", ls, "outputscale", h->outputscale,
                            "mean", h->mean, "lengthscale_b", lsb,
                            "outputscale_b", h->outputscale_b, "cutpoint", cut,
                            "noise_sd", h->noise_sd, "lengthscale_g", lsg,
                            "outputscale_g", h->outputscale_g, "mean_g", h->mean_g);
    Py_XDECREF(lsg);
    Py_XDECREF(ls);
    Py_XDECREF(lsb);
    Py_XDECREF(cut);
    return out;
}

/* --- enum names ----------------------------------------------------------- */

typedef struct { const char* name; int value; } gp_name;

static const gp_name lik_names[] = {
    { "bernoulli", YAEP_LIK_BERNOULLI }, { "ordinal", YAEP_LIK_ORDINAL },
    { "categorical", YAEP_LIK_CATEGORICAL }, { "gaussian", YAEP_LIK_GAUSSIAN },
    { "pairwise", YAEP_LIK_PAIRWISE }, { NULL, 0 } };
static const gp_name dim_kind_names[] = {
    { "continuous", YAEP_DIM_CONTINUOUS }, { "integer", YAEP_DIM_INTEGER },
    { "categorical", YAEP_DIM_CATEGORICAL }, { NULL, 0 } };
static const gp_name kernel_names[] = {
    { "rbf", YAEP_KERNEL_RBF }, { "semip", YAEP_KERNEL_SEMIP }, { NULL, 0 } };
static const gp_name link_names[] = {
    { "probit", YAEP_LINK_PROBIT }, { "logit", YAEP_LINK_LOGIT }, { NULL, 0 } };
static const gp_name model_names[] = {
    { "gp", YAEP_MODEL_GP }, { "psychometric", YAEP_MODEL_PSYCHOMETRIC },
    { NULL, 0 } };
static const gp_name acq_names[] = {
    { "lse", YAEP_ACQ_LSE }, { "eavc", YAEP_ACQ_EAVC },
    { "localmi", YAEP_ACQ_LOCALMI }, { "balv", YAEP_ACQ_BALV },
    { "bald", YAEP_ACQ_BALD }, { "random", YAEP_ACQ_RANDOM },
    { "ucb", YAEP_ACQ_UCB }, { "ei", YAEP_ACQ_EI },
    { "thompson", YAEP_ACQ_THOMPSON }, { NULL, 0 } };

/* An enum field as the module constant (an int) or its lower-case name. */
static int gp_enum(PyObject* v, const gp_name* names, const char* field, int* out) {
    int i;
    if (PyUnicode_Check(v)) {
        PyObject* low = PyObject_CallMethod(v, "lower", NULL);
        if (!low) return -1;
        for (i = 0; names[i].name; i++) {
            if (PyUnicode_CompareWithASCIIString(low, names[i].name) == 0) {
                Py_DECREF(low);
                *out = names[i].value;
                return 0;
            }
        }
        Py_DECREF(low);
        PyErr_Format(PyExc_ValueError, "unknown %s %R", field, v);
        return -1;
    }
    {
        long n = PyLong_AsLong(v);
        if (n == -1 && PyErr_Occurred()) return -1;
        for (i = 0; names[i].name; i++) {
            if (names[i].value == n) { *out = (int)n; return 0; }
        }
        PyErr_Format(PyExc_ValueError, "unknown %s %ld", field, n);
        return -1;
    }
}

/* --- priors ------------------------------------------------------------------ */

static const char* const prior_keys[] = {
    "lengthscale", "outputscale", "outputscale_b", "outputscale_g", "mean",
    "mean_g", "noise_sd", NULL
};

static yaep_prior* gp_prior_slot(yaep_priors* p, int i) {
    switch (i) {
        case 0: return &p->lengthscale;
        case 1: return &p->outputscale;
        case 2: return &p->outputscale_b;
        case 3: return &p->outputscale_g;
        case 4: return &p->mean;
        case 5: return &p->mean_g;
        default: return &p->noise_sd;
    }
}

/* One prior: a dict {center, sd, ceiling}, a (center, sd[, ceiling])
 * sequence, or an object with those attributes. A missing field stays 0, the
 * measured default; a negative sd turns the prior off, as in C. */
static int gp_parse_prior(PyObject* v, yaep_prior* out, const char* name) {
    static const char* const f[] = { "center", "sd", "ceiling" };
    double* dst[3];
    int i;
    dst[0] = &out->center; dst[1] = &out->sd; dst[2] = &out->ceiling;
    memset(out, 0, sizeof(*out));
    if (v == Py_None) return 0;
    if (PyDict_Check(v)) {
        Py_ssize_t pos = 0;
        PyObject *k, *x;
        while (PyDict_Next(v, &pos, &k, &x)) {
            int found = 0;
            for (i = 0; i < 3; i++) {
                if (PyUnicode_Check(k) && PyUnicode_CompareWithASCIIString(k, f[i]) == 0) {
                    if (x != Py_None && gp_as_double(x, dst[i]) < 0) return -1;
                    found = 1;
                }
            }
            if (!found) {
                PyErr_Format(PyExc_TypeError, "priors.%s has an unknown field %R", name, k);
                return -1;
            }
        }
        return 0;
    }
    if (PyList_Check(v) || PyTuple_Check(v)) {
        double tmp[3] = { 0.0, 0.0, 0.0 };
        Py_ssize_t n = gp_small_doubles(v, tmp, 3, name);
        if (n < 0) return -1;
        if (n < 2) {
            PyErr_Format(PyExc_ValueError, "priors.%s needs (center, sd[, ceiling])", name);
            return -1;
        }
        out->center = tmp[0]; out->sd = tmp[1]; out->ceiling = tmp[2];
        return 0;
    }
    for (i = 0; i < 3; i++) {
        PyObject* x = PyObject_GetAttrString(v, f[i]);
        int rc;
        if (!x) { PyErr_Clear(); continue; }
        rc = x == Py_None ? 0 : gp_as_double(x, dst[i]);
        Py_DECREF(x);
        if (rc < 0) return -1;
    }
    return 0;
}

/* desc.priors: a dict of the seven priors, or an object with them as
 * attributes (ysp.aep.Priors). */
static int gp_parse_priors(PyObject* obj, yaep_priors* p) {
    int i;
    memset(p, 0, sizeof(*p));
    if (obj == Py_None) return 0;
    if (PyDict_Check(obj)) {
        Py_ssize_t pos = 0;
        PyObject *k, *v;
        while (PyDict_Next(obj, &pos, &k, &v)) {
            int found = 0;
            for (i = 0; prior_keys[i]; i++) {
                if (PyUnicode_Check(k) && PyUnicode_CompareWithASCIIString(k, prior_keys[i]) == 0) {
                    if (gp_parse_prior(v, gp_prior_slot(p, i), prior_keys[i]) < 0) return -1;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                PyErr_Format(PyExc_TypeError, "priors has an unknown field %R", k);
                return -1;
            }
        }
        return 0;
    }
    for (i = 0; prior_keys[i]; i++) {
        PyObject* v = PyObject_GetAttrString(obj, prior_keys[i]);
        int rc;
        if (!v) { PyErr_Clear(); continue; }
        rc = gp_parse_prior(v, gp_prior_slot(p, i), prior_keys[i]);
        Py_DECREF(v);
        if (rc < 0) return -1;
    }
    return 0;
}

static PyObject* gp_priors_dict(const yaep_priors* p) {
    PyObject* d = PyDict_New();
    int i;
    if (!d) return NULL;
    for (i = 0; prior_keys[i]; i++) {
        yaep_prior* q = gp_prior_slot((yaep_priors*)p, i);
        PyObject* e = Py_BuildValue("{s:d,s:d,s:d}", "center", q->center,
                                    "sd", q->sd, "ceiling", q->ceiling);
        if (!e || PyDict_SetItemString(d, prior_keys[i], e) < 0) {
            Py_XDECREF(e);
            Py_DECREF(d);
            return NULL;
        }
        Py_DECREF(e);
    }
    return d;
}

/* desc.dim_kind: one kind per dimension, each a DIM_* constant or its name. */
static int gp_parse_dim_kind(PyObject* v, yaep_dim_kind* out) {
    PyObject* list = PySequence_List(v);
    Py_ssize_t n, i;
    if (!list) return -1;
    n = PyList_Size(list);
    if (n > YAEP_MAX_DIMS) {
        Py_DECREF(list);
        PyErr_Format(PyExc_ValueError, "dim_kind has %zd entries, at most %d", n, YAEP_MAX_DIMS);
        return -1;
    }
    for (i = 0; i < n; i++) {
        int k;
        if (gp_enum(PyList_GetItem(list, i), dim_kind_names, "dim_kind", &k) < 0) {
            Py_DECREF(list);
            return -1;
        }
        out[i] = (yaep_dim_kind)k;
    }
    Py_DECREF(list);
    return 0;
}

/* desc.monotone_dims: the C bit mask as an int, or a sequence of dimension
 * indices. */
static int gp_parse_mask(PyObject* v, unsigned* out) {
    if (PyLong_Check(v)) {
        long m = PyLong_AsLong(v);
        if (m == -1 && PyErr_Occurred()) return -1;
        if (m < 0 || m >= (1L << YAEP_MAX_DIMS)) {
            PyErr_Format(PyExc_ValueError, "monotone_dims mask %ld is outside 0..%ld",
                         m, (1L << YAEP_MAX_DIMS) - 1);
            return -1;
        }
        *out = (unsigned)m;
        return 0;
    }
    {
        int dims[YAEP_MAX_DIMS];
        Py_ssize_t n = gp_small_ints(v, dims, YAEP_MAX_DIMS, "monotone_dims"), i;
        if (n < 0) return -1;
        *out = 0u;
        for (i = 0; i < n; i++) {
            if (dims[i] >= YAEP_MAX_DIMS) {
                PyErr_Format(PyExc_ValueError, "monotone_dims names dimension %d", dims[i]);
                return -1;
            }
            *out |= 1u << dims[i];
        }
        return 0;
    }
}

/* --- desc parsing --------------------------------------------------------- */

static int gp_int(PyObject* v, int* out) {
    long n = PyLong_AsLong(v);
    if (n == -1 && PyErr_Occurred()) return -1;
    if (n < -2147483647L || n > 2147483647L) {
        PyErr_SetString(PyExc_OverflowError, "value does not fit in an int");
        return -1;
    }
    *out = (int)n;
    return 0;
}

static int gp_bool(PyObject* v, bool* out) {
    int t = PyObject_IsTrue(v);
    if (t < 0) return -1;
    *out = t ? true : false;
    return 0;
}

#define KEY(s) (PyUnicode_CompareWithASCIIString(k, s) == 0)

/* Fill a yaep_desc from GP(**kwargs). The candidate array, when given, is
 * copied into *cand, which the caller owns and must keep for the life of the
 * handle; the rng callable is returned borrowed. */
static int gp_parse_desc(PyObject* kw, yaep_desc* d, double** cand, PyObject** rng) {
    Py_ssize_t pos = 0, nlo = -1, nhi = -1;
    PyObject *k, *v, *cand_obj = NULL;
    int have_n_dims = 0;
    memset(d, 0, sizeof(*d));
    *cand = NULL;
    *rng = NULL;
    if (!kw) return 0;
    while (PyDict_Next(kw, &pos, &k, &v)) {
        int rc = 0, tmp = 0;
        if (!PyUnicode_Check(k)) { PyErr_SetString(PyExc_TypeError, "keywords must be strings"); return -1; }
        if (v == Py_None && !KEY("rng") && !KEY("candidates")) continue;
        if (KEY("n_dims"))              { rc = gp_int(v, &d->n_dims); have_n_dims = 1; }
        else if (KEY("lo"))             { nlo = gp_small_doubles(v, d->lo, YAEP_MAX_DIMS, "lo"); rc = nlo < 0 ? -1 : 0; }
        else if (KEY("hi"))             { nhi = gp_small_doubles(v, d->hi, YAEP_MAX_DIMS, "hi"); rc = nhi < 0 ? -1 : 0; }
        else if (KEY("intensity_dim"))  rc = gp_int(v, &d->intensity_dim);
        else if (KEY("lik"))            { rc = gp_enum(v, lik_names, "lik", &tmp); d->lik = (yaep_lik)tmp; }
        else if (KEY("n_outcomes"))     rc = gp_int(v, &d->n_outcomes);
        else if (KEY("kernel"))         { rc = gp_enum(v, kernel_names, "kernel", &tmp); d->kernel = (yaep_kernel)tmp; }
        else if (KEY("link"))           { rc = gp_enum(v, link_names, "link", &tmp); d->link = (yaep_link)tmp; }
        else if (KEY("guess"))          rc = gp_as_double(v, &d->guess);
        else if (KEY("lapse"))          rc = gp_as_double(v, &d->lapse);
        else if (KEY("hyper"))          rc = gp_parse_hyper(v, &d->hyper, "hyper");
        else if (KEY("fit"))            rc = gp_bool(v, &d->fit);
        else if (KEY("fit_every"))      rc = gp_int(v, &d->fit_every);
        else if (KEY("no_hyper_prior")) rc = gp_bool(v, &d->no_hyper_prior);
        else if (KEY("refit_every"))    rc = gp_int(v, &d->refit_every);
        else if (KEY("hyper_min"))      rc = gp_parse_hyper(v, &d->hyper_min, "hyper_min");
        else if (KEY("hyper_max"))      rc = gp_parse_hyper(v, &d->hyper_max, "hyper_max");
        else if (KEY("jitter"))         rc = gp_as_double(v, &d->jitter);
        else if (KEY("acq"))            { rc = gp_enum(v, acq_names, "acq", &tmp); d->acq = (yaep_acq)tmp; }
        else if (KEY("target_p"))       rc = gp_as_double(v, &d->target_p);
        else if (KEY("target_value"))   rc = gp_as_double(v, &d->target_value);
        else if (KEY("target_outcome")) rc = gp_int(v, &d->target_outcome);
        else if (KEY("acq_beta"))       rc = gp_as_double(v, &d->acq_beta);
        else if (KEY("n_init"))         rc = gp_int(v, &d->n_init);
        else if (KEY("rng")) {
            if (v != Py_None && !PyCallable_Check(v)) {
                PyErr_SetString(PyExc_TypeError,
                                "rng must be a callable returning a uniform variate in [0, 1)");
                return -1;
            }
            *rng = v == Py_None ? NULL : v;
        }
        else if (KEY("candidates"))     cand_obj = v == Py_None ? NULL : v;
        else if (KEY("n_candidates"))   rc = gp_int(v, &d->n_candidates);
        else if (KEY("grid"))           rc = gp_small_ints(v, d->grid, YAEP_MAX_DIMS, "grid") < 0 ? -1 : 0;
        else if (KEY("stop_trials"))    rc = gp_int(v, &d->stop_trials);
        else if (KEY("stop_threshold_sd")) rc = gp_as_double(v, &d->stop_threshold_sd);
        else if (KEY("stop_context"))   rc = gp_small_doubles(v, d->stop_context, YAEP_MAX_DIMS, "stop_context") < 0 ? -1 : 0;
        else if (KEY("max_trials"))     rc = gp_int(v, &d->max_trials);
        else if (KEY("refine_steps"))   rc = gp_int(v, &d->refine_steps);
        else if (KEY("model"))          { rc = gp_enum(v, model_names, "model", &tmp); d->model = (yaep_model)tmp; }
        else if (KEY("priors"))         rc = gp_parse_priors(v, &d->priors);
        else if (KEY("minimize"))       rc = gp_bool(v, &d->minimize);
        else if (KEY("dim_kind"))       rc = gp_parse_dim_kind(v, d->dim_kind);
        else if (KEY("dim_levels"))     rc = gp_small_ints(v, d->dim_levels, YAEP_MAX_DIMS, "dim_levels") < 0 ? -1 : 0;
        else if (KEY("monotone_dims"))  rc = gp_parse_mask(v, &d->monotone_dims);
        else if (KEY("pcg_threshold"))  rc = gp_int(v, &d->pcg_threshold);
        else if (KEY("fit_max_evals"))  rc = gp_int(v, &d->fit_max_evals);
        else if (KEY("fit_tol"))        rc = gp_as_double(v, &d->fit_tol);
        else if (KEY("fit_pcg"))        rc = gp_bool(v, &d->fit_pcg);
        else {
            PyErr_Format(PyExc_TypeError, "GP() got an unexpected keyword %R", k);
            return -1;
        }
        if (rc < 0) return -1;
    }
    if (nlo >= 0 && nhi >= 0 && nlo != nhi) {
        PyErr_SetString(PyExc_ValueError, "lo and hi must have the same length");
        return -1;
    }
    if (!have_n_dims) d->n_dims = (int)(nlo >= 0 ? nlo : (nhi >= 0 ? nhi : 0));
    if (cand_obj) {
        Py_ssize_t rows;
        if (d->n_dims < 1 || d->n_dims > YAEP_MAX_DIMS) {
            PyErr_SetString(PyExc_ValueError, "candidates need n_dims (or lo and hi) first");
            return -1;
        }
        *cand = gp_doubles(cand_obj, d->n_dims, &rows, "candidates");
        if (!*cand) return -1;
        if (rows < 1 || rows > 0x7fffffff) {
            PyMem_Free(*cand);
            *cand = NULL;
            PyErr_SetString(PyExc_ValueError, "candidates is empty");
            return -1;
        }
        if (d->n_candidates && d->n_candidates != (int)rows) {
            PyMem_Free(*cand);
            *cand = NULL;
            PyErr_Format(PyExc_ValueError,
                         "n_candidates = %d but candidates has %zd rows",
                         d->n_candidates, rows);
            return -1;
        }
        d->n_candidates = (int)rows;
        d->candidates = *cand;
    }
    return 0;
}

#undef KEY

/* --- GP -------------------------------------------------------------------- */

#define AS_GP(o) ((GPObject*)(o))

/* Every call on the handle goes through this: a closed handle, one an Async
 * owns, or one another thread is inside, raise before the C call runs. */
static int gp_ready(GPObject* o) {
    if (o->owned) {
        gp_fail_msg(GpBusy, "an Async owns this GP until its stop() returns");
        return -1;
    }
    if (o->busy) {
        gp_fail_msg(GpBusy, "another call on this GP is in flight (a GP is "
                            "not thread-safe; give each thread its own)");
        return -1;
    }
    if (!yaep_is_open(&o->g)) {
        gp_fail(YAEP_ERR_CLOSED, "GP");
        return -1;
    }
    return 0;
}

/* Called by yaep_next() with the GIL held, because next() keeps the GIL when
 * rng is set. An exception is left pending and the draw becomes 0; next()
 * raises it once yaep_next() returns. */
static double gp_rng_call(void* ctx) {
    GPObject* o = (GPObject*)ctx;
    PyObject* r;
    double u;
    if (PyErr_Occurred() || !o->rng) return 0.0;
    r = PyObject_CallObject(o->rng, NULL);
    if (!r) return 0.0;
    u = PyFloat_AsDouble(r);
    Py_DECREF(r);
    if (u == -1.0 && PyErr_Occurred()) return 0.0;
    return u;
}

static void gp_release(GPObject* o) {
    yaep_close(&o->g);
    PyMem_Free(o->cand);
    o->cand = NULL;
    Py_CLEAR(o->rng);
}

/* Open (snap == NULL) or resume from a snapshot. One path for both, so the
 * rng trampoline and the candidate copy are set up the same way. */
static int gp_setup(GPObject* o, PyObject* kwds, const void* snap, size_t len) {
    yaep_desc d;
    double* cand;
    PyObject* rng;
    bool ok;
    if (o->owned || o->busy) {
        gp_fail_msg(GpBusy, "GP is in use and cannot be re-initialized");
        return -1;
    }
    if (gp_parse_desc(kwds, &d, &cand, &rng) < 0) return -1;
    gp_release(o);
    if (rng) {
        Py_INCREF(rng);
        o->rng = rng;
        d.rng = gp_rng_call;
        d.rng_ctx = o;
    }
    o->cand = cand;
    if (snap) {
        /* The load rebuilds every matrix from the image, with no refit. */
        o->busy = 1;
        Py_BEGIN_ALLOW_THREADS
        ok = yaep_open_saved(&o->g, &d, snap, len);
        Py_END_ALLOW_THREADS
        o->busy = 0;
    } else {
        ok = yaep_open(&o->g, &d);
    }
    if (!ok) {
        PyErr_SetString(GpError, yaep_error(&o->g));
        gp_release(o);
        return -1;
    }
    return 0;
}

static int GP_init(PyObject* self, PyObject* args, PyObject* kwds) {
    if (args && PyTuple_Size(args) > 0) {
        PyErr_SetString(PyExc_TypeError, "GP() takes keyword arguments only: GP(**desc)");
        return -1;
    }
    return gp_setup(AS_GP(self), kwds, NULL, 0);
}

static PyObject* GP_save(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    GPObject* o = AS_GP(self);
    size_t n;
    PyObject* b;
    int rc;
    if (gp_ready(o) < 0) return NULL;
    n = yaep_save_size(&o->g);
    if (n == 0) return gp_fail(YAEP_ERR_CLOSED, "save");
    b = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)n);
    if (!b) return NULL;
    rc = yaep_save(&o->g, PyBytes_AsString(b), n);
    if (rc < 0) { Py_DECREF(b); return gp_fail(rc, "save"); }
    return b;
}

/* GP.load(data, **desc): a new GP resumed from save(). */
static PyObject* GP_load(PyObject* cls, PyObject* args, PyObject* kwds) {
    PyObject *bytes, *self, *empty;
    if (PyTuple_Size(args) != 1) {
        PyErr_SetString(PyExc_TypeError, "GP.load(data, **desc) takes the snapshot bytes "
                                         "and the desc as keywords");
        return NULL;
    }
    bytes = PyBytes_FromObject(PyTuple_GetItem(args, 0));
    if (!bytes) return NULL;
    empty = PyTuple_New(0);
    self = empty ? PyType_GenericNew((PyTypeObject*)cls, empty, NULL) : NULL;
    Py_XDECREF(empty);
    if (!self) { Py_DECREF(bytes); return NULL; }
    if (gp_setup(AS_GP(self), kwds, PyBytes_AsString(bytes),
                 (size_t)PyBytes_Size(bytes)) < 0) {
        Py_DECREF(bytes);
        Py_DECREF(self);
        return NULL;
    }
    Py_DECREF(bytes);
    return self;
}

static int GP_traverse(PyObject* self, visitproc visit, void* arg) {
    Py_VISIT(AS_GP(self)->rng);
    return 0;
}

static int GP_clear(PyObject* self) {
    Py_CLEAR(AS_GP(self)->rng);
    return 0;
}

static void GP_dealloc(PyObject* self) {
    PyTypeObject* tp = Py_TYPE(self);
    freefunc tp_free;
    PyObject_GC_UnTrack(self);
    gp_release(AS_GP(self));
    tp_free = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    tp_free(self);
    Py_DECREF(tp);
}

/* GIL release around a call on the handle; the busy flag is what keeps a
 * second Python thread out while the GIL is down. */
#define GP_CALL(o, stmt)                         \
    do {                                         \
        (o)->busy = 1;                           \
        Py_BEGIN_ALLOW_THREADS                   \
        stmt;                                    \
        Py_END_ALLOW_THREADS                     \
        (o)->busy = 0;                           \
    } while (0)

static PyObject* gp_next_result(GPObject* o, int idx, const double* x) {
    PyObject* xl;
    if (idx < -1) return gp_fail(idx, "next");
    xl = gp_list(x, o->g.desc.n_dims);
    if (!xl) return NULL;
    return Py_BuildValue("(iN)", idx, xl);
}

static PyObject* GP_next(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    GPObject* o = AS_GP(self);
    double x[YAEP_MAX_DIMS];
    int idx;
    if (gp_ready(o) < 0) return NULL;
    if (o->rng) {
        o->busy = 1;   /* the callback may try to re-enter this GP */
        idx = yaep_next(&o->g, x);
        o->busy = 0;
        if (PyErr_Occurred()) return NULL;
    } else {
        GP_CALL(o, idx = yaep_next(&o->g, x));
    }
    return gp_next_result(o, idx, x);
}

static PyObject* GP_next_subset(PyObject* self, PyObject* arg) {
    GPObject* o = AS_GP(self);
    double x[YAEP_MAX_DIMS];
    PyObject* list;
    Py_ssize_t n, i;
    int* sub;
    int idx;
    if (gp_ready(o) < 0) return NULL;
    list = PySequence_List(arg);
    if (!list) return NULL;
    n = PyList_Size(list);
    if (n < 1 || n > 0x7fffffff) {
        Py_DECREF(list);
        return gp_fail_msg(PyExc_ValueError, "subset must hold at least one index");
    }
    sub = (int*)PyMem_Malloc((size_t)n * sizeof(int));
    if (!sub) { Py_DECREF(list); return PyErr_NoMemory(); }
    for (i = 0; i < n; i++) {
        if (gp_int(PyList_GetItem(list, i), &sub[i]) < 0) {
            PyMem_Free(sub);
            Py_DECREF(list);
            return NULL;
        }
    }
    Py_DECREF(list);
    if (o->rng) {
        o->busy = 1;
        idx = yaep_next_subset(&o->g, sub, (int)n, x);
        o->busy = 0;
        if (PyErr_Occurred()) { PyMem_Free(sub); return NULL; }
    } else {
        GP_CALL(o, idx = yaep_next_subset(&o->g, sub, (int)n, x));
    }
    PyMem_Free(sub);
    return gp_next_result(o, idx, x);
}

static PyObject* GP_acq_score(PyObject* self, PyObject* arg) {
    GPObject* o = AS_GP(self);
    int i;
    if (gp_ready(o) < 0) return NULL;
    if (gp_int(arg, &i) < 0) return NULL;
    return PyFloat_FromDouble(yaep_acq_score(&o->g, i));
}

static PyObject* gp_update_common(GPObject* o, PyObject* xo, PyObject* yo, int real) {
    double x[YAEP_MAX_DIMS], y = 0.0;
    int outcome = 0, rc;
    if (gp_ready(o) < 0) return NULL;
    if (gp_point(xo, o->g.desc.n_dims, x, "x") < 0) return NULL;
    if (real) { if (gp_as_double(yo, &y) < 0) return NULL; }
    else if (gp_int(yo, &outcome) < 0) return NULL;
    if (real) GP_CALL(o, rc = yaep_update_real(&o->g, x, y));
    else      GP_CALL(o, rc = yaep_update(&o->g, x, outcome));
    if (rc < 0) return gp_fail(rc, real ? "update_real" : "update");
    Py_RETURN_NONE;
}

static PyObject* GP_update(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "x", "outcome", NULL };
    PyObject *x, *y;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO", kw, &x, &y)) return NULL;
    return gp_update_common(AS_GP(self), x, y, 0);
}

static PyObject* GP_update_real(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "x", "y", NULL };
    PyObject *x, *y;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO", kw, &x, &y)) return NULL;
    return gp_update_common(AS_GP(self), x, y, 1);
}

static PyObject* GP_predict_f(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "x", "k", NULL };
    GPObject* o = AS_GP(self);
    PyObject* xo;
    int k = 0, rc;
    double x[YAEP_MAX_DIMS], mu = 0.0, sd = 0.0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|i", kw, &xo, &k)) return NULL;
    if (gp_ready(o) < 0) return NULL;
    if (gp_point(xo, o->g.desc.n_dims, x, "x") < 0) return NULL;
    rc = yaep_predict_f(&o->g, x, k, &mu, &sd);
    if (rc < 0) return gp_fail(rc, "predict_f");
    return Py_BuildValue("(dd)", mu, sd);
}

static PyObject* gp_scalar(GPObject* o, PyObject* xo, int var) {
    double x[YAEP_MAX_DIMS], p;
    if (gp_ready(o) < 0) return NULL;
    if (gp_point(xo, o->g.desc.n_dims, x, "x") < 0) return NULL;
    p = var ? yaep_predict_p_var(&o->g, x) : yaep_predict_p(&o->g, x);
    /* NaN is the header's bad-argument answer: x outside the box, mostly. */
    if (p != p) return gp_fail(YAEP_ERR_ARG, var ? "predict_p_var" : "predict_p");
    return PyFloat_FromDouble(p);
}

static PyObject* GP_predict_p(PyObject* self, PyObject* arg) {
    return gp_scalar(AS_GP(self), arg, 0);
}

static PyObject* GP_predict_p_var(PyObject* self, PyObject* arg) {
    return gp_scalar(AS_GP(self), arg, 1);
}

static PyObject* GP_predict_outcomes(PyObject* self, PyObject* arg) {
    GPObject* o = AS_GP(self);
    double x[YAEP_MAX_DIMS], p[YAEP_MAX_OUTCOMES];
    int rc, K;
    if (gp_ready(o) < 0) return NULL;
    if (gp_point(arg, o->g.desc.n_dims, x, "x") < 0) return NULL;
    rc = yaep_predict_outcomes(&o->g, x, p);
    if (rc < 0) return gp_fail(rc, "predict_outcomes");
    K = o->g.desc.lik == YAEP_LIK_BERNOULLI ? 2 : o->g.desc.n_outcomes;
    return gp_list(p, K);
}

static PyObject* GP_predict_p_many(PyObject* self, PyObject* arg) {
    GPObject* o = AS_GP(self);
    Py_ssize_t n;
    double *xs, *p;
    PyObject* out;
    int rc;
    if (gp_ready(o) < 0) return NULL;
    xs = gp_doubles(arg, o->g.desc.n_dims, &n, "xs");
    if (!xs) return NULL;
    if (n > 0x7fffffff) { PyMem_Free(xs); return gp_fail_msg(PyExc_ValueError, "too many points"); }
    out = gp_bytes(n, &p);
    if (!out) { PyMem_Free(xs); return NULL; }
    rc = YAEP_OK;
    if (n > 0) GP_CALL(o, rc = yaep_predict_p_many(&o->g, xs, (int)n, p));
    PyMem_Free(xs);
    if (rc < 0) { Py_DECREF(out); return gp_fail(rc, "predict_p_many"); }
    return gp_view(out);
}

static PyObject* GP_predict_f_many(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "xs", "k", NULL };
    GPObject* o = AS_GP(self);
    PyObject *xo, *bmu, *bsd, *vmu, *vsd;
    Py_ssize_t n;
    double *xs, *mu = NULL, *sd = NULL;
    int k = 0, rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|i", kw, &xo, &k)) return NULL;
    if (gp_ready(o) < 0) return NULL;
    xs = gp_doubles(xo, o->g.desc.n_dims, &n, "xs");
    if (!xs) return NULL;
    if (n > 0x7fffffff) { PyMem_Free(xs); return gp_fail_msg(PyExc_ValueError, "too many points"); }
    bmu = gp_bytes(n, &mu);
    bsd = bmu ? gp_bytes(n, &sd) : NULL;
    if (!bmu || !bsd) { Py_XDECREF(bmu); PyMem_Free(xs); return NULL; }
    rc = YAEP_OK;
    if (n > 0) GP_CALL(o, rc = yaep_predict_f_many(&o->g, xs, (int)n, k, mu, sd));
    PyMem_Free(xs);
    if (rc < 0) {
        Py_DECREF(bmu);
        Py_DECREF(bsd);
        return gp_fail(rc, "predict_f_many");
    }
    vmu = gp_view(bmu);
    if (!vmu) { Py_DECREF(bsd); return NULL; }
    vsd = gp_view(bsd);
    if (!vsd) { Py_DECREF(vmu); return NULL; }
    return Py_BuildValue("(NN)", vmu, vsd);
}

/* The n_dims - 1 context coordinates: None or empty for a 1-D GP, a number
 * when there is exactly one, or a sequence. */
static int gp_context(PyObject* obj, int n_dims, double* ctx) {
    int want = n_dims - 1;
    Py_ssize_t n;
    memset(ctx, 0, sizeof(double) * YAEP_MAX_DIMS);
    if (obj == NULL || obj == Py_None) {
        if (want == 0) return 0;
        PyErr_Format(PyExc_ValueError, "ctx needs %d coordinates", want);
        return -1;
    }
    if (PyFloat_Check(obj) || PyLong_Check(obj)) {
        if (want != 1) {
            PyErr_Format(PyExc_ValueError, "ctx needs %d coordinates", want);
            return -1;
        }
        return gp_as_double(obj, &ctx[0]);
    }
    n = gp_small_doubles(obj, ctx, YAEP_MAX_DIMS, "ctx");
    if (n < 0) return -1;
    if (n != want) {
        PyErr_Format(PyExc_ValueError, "ctx has %zd coordinates, needs %d", n, want);
        return -1;
    }
    return 0;
}

static PyObject* GP_threshold(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "ctx", "target", NULL };
    GPObject* o = AS_GP(self);
    PyObject* co = NULL;
    double target = 0.0, ctx[YAEP_MAX_DIMS], x = 0.0, lo = 0.0, hi = 0.0;
    int rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|Od", kw, &co, &target)) return NULL;
    if (gp_ready(o) < 0) return NULL;
    if (gp_context(co, o->g.desc.n_dims, ctx) < 0) return NULL;
    rc = yaep_threshold(&o->g, o->g.desc.n_dims > 1 ? ctx : NULL, target, &x, &lo, &hi);
    if (rc < 0) return gp_fail(rc, "threshold");
    return Py_BuildValue("(ddd)", x, lo, hi);
}

static PyObject* GP_fit(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    GPObject* o = AS_GP(self);
    int rc;
    if (gp_ready(o) < 0) return NULL;
    GP_CALL(o, rc = yaep_fit(&o->g));
    if (rc < 0) return gp_fail(rc, "fit");
    return PyLong_FromLong(rc);   /* 1 converged, 0 budget or guard */
}

static PyObject* GP_fit_delta(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    if (gp_ready(AS_GP(self)) < 0) return NULL;
    return PyFloat_FromDouble(yaep_fit_delta(&AS_GP(self)->g));
}

static PyObject* GP_argmax(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    GPObject* o = AS_GP(self);
    double x[YAEP_MAX_DIMS], v = 0.0;
    PyObject* xl;
    int rc;
    if (gp_ready(o) < 0) return NULL;
    GP_CALL(o, rc = yaep_argmax(&o->g, x, &v));
    if (rc < -1) return gp_fail(rc, "argmax");
    xl = gp_list(x, o->g.desc.n_dims);
    if (!xl) return NULL;
    return Py_BuildValue("(Nd)", xl, v);
}

static PyObject* GP_get_priors(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    yaep_priors p;
    int rc;
    if (gp_ready(AS_GP(self)) < 0) return NULL;
    memset(&p, 0, sizeof(p));
    rc = yaep_get_priors(&AS_GP(self)->g, &p);
    if (rc < 0) return gp_fail(rc, "get_priors");
    return gp_priors_dict(&p);
}

/* --- pairwise ---------------------------------------------------------------- */

static PyObject* GP_next_pair(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    GPObject* o = AS_GP(self);
    double x1[YAEP_MAX_DIMS], x2[YAEP_MAX_DIMS];
    PyObject *a, *b;
    int rc;
    if (gp_ready(o) < 0) return NULL;
    if (o->rng) {
        /* THOMPSON draws from the Python generator inside the selection */
        o->busy = 1;
        rc = yaep_next_pair(&o->g, x1, x2);
        o->busy = 0;
        if (PyErr_Occurred()) return NULL;
    } else {
        GP_CALL(o, rc = yaep_next_pair(&o->g, x1, x2));
    }
    if (rc < 0) return gp_fail(rc, "next_pair");
    a = gp_list(x1, o->g.desc.n_dims);
    b = a ? gp_list(x2, o->g.desc.n_dims) : NULL;
    if (!b) { Py_XDECREF(a); return NULL; }
    return Py_BuildValue("(NN)", a, b);
}

static PyObject* GP_update_pair(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "x1", "x2", "outcome", NULL };
    GPObject* o = AS_GP(self);
    PyObject *xo1, *xo2, *yo;
    double x1[YAEP_MAX_DIMS], x2[YAEP_MAX_DIMS];
    int outcome, rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOO", kw, &xo1, &xo2, &yo)) return NULL;
    if (gp_ready(o) < 0) return NULL;
    if (gp_point(xo1, o->g.desc.n_dims, x1, "x1") < 0) return NULL;
    if (gp_point(xo2, o->g.desc.n_dims, x2, "x2") < 0) return NULL;
    if (gp_int(yo, &outcome) < 0) return NULL;
    GP_CALL(o, rc = yaep_update_pair(&o->g, x1, x2, outcome));
    if (rc < 0) return gp_fail(rc, "update_pair");
    Py_RETURN_NONE;
}

static PyObject* GP_predict_pair(PyObject* self, PyObject* args) {
    GPObject* o = AS_GP(self);
    PyObject *xo1, *xo2;
    double x1[YAEP_MAX_DIMS], x2[YAEP_MAX_DIMS], p;
    if (!PyArg_ParseTuple(args, "OO", &xo1, &xo2)) return NULL;
    if (gp_ready(o) < 0) return NULL;
    if (gp_point(xo1, o->g.desc.n_dims, x1, "x1") < 0) return NULL;
    if (gp_point(xo2, o->g.desc.n_dims, x2, "x2") < 0) return NULL;
    p = yaep_predict_pair(&o->g, x1, x2);
    if (p != p) return gp_fail(YAEP_ERR_ARG, "predict_pair");
    return PyFloat_FromDouble(p);
}

static PyObject* GP_fit_step(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    GPObject* o = AS_GP(self);
    int rc;
    if (gp_ready(o) < 0) return NULL;
    GP_CALL(o, rc = yaep_fit_step(&o->g));
    if (rc < 0) return gp_fail(rc, "fit_step");
    return PyBool_FromLong(rc > 0);
}

static PyObject* GP_refit(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    GPObject* o = AS_GP(self);
    int rc;
    if (gp_ready(o) < 0) return NULL;
    GP_CALL(o, rc = yaep_refit(&o->g));
    if (rc < 0) return gp_fail(rc, "refit");
    Py_RETURN_NONE;
}

static PyObject* GP_hyper(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    GPObject* o = AS_GP(self);
    yaep_hyper h;
    int rc;
    if (gp_ready(o) < 0) return NULL;
    memset(&h, 0, sizeof(h));
    rc = yaep_get_hyper(&o->g, &h);
    if (rc < 0) return gp_fail(rc, "hyper");
    return gp_hyper_dict(&h, &o->g.desc);
}

static PyObject* GP_candidate(PyObject* self, PyObject* arg) {
    GPObject* o = AS_GP(self);
    double x[YAEP_MAX_DIMS];
    int i, rc;
    if (gp_ready(o) < 0) return NULL;
    if (gp_int(arg, &i) < 0) return NULL;
    rc = yaep_candidate(&o->g, i, x);
    if (rc < 0) return gp_fail(rc, "candidate");
    return gp_list(x, o->g.desc.n_dims);
}

static PyObject* GP_history(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    GPObject* o = AS_GP(self);
    const yaep_trial* h;
    PyObject* list;
    int n = 0, i;
    if (gp_ready(o) < 0) return NULL;
    h = yaep_history(&o->g, &n);
    list = PyList_New(n > 0 ? n : 0);
    if (!list) return NULL;
    for (i = 0; i < n; i++) {
        PyObject* x = gp_list(h[i].x, o->g.desc.n_dims);
        PyObject* d;
        if (!x) { Py_DECREF(list); return NULL; }
        d = Py_BuildValue("{s:N,s:d,s:O,s:O}", "x", x, "y", h[i].y,
                          "proposed", h[i].proposed ? Py_True : Py_False,
                          "init", h[i].init ? Py_True : Py_False);
        if (!d) { Py_DECREF(list); return NULL; }
        if (o->g.desc.lik == YAEP_LIK_PAIRWISE) {
            PyObject* x2 = gp_list(h[i].x2, o->g.desc.n_dims);
            if (!x2 || PyDict_SetItemString(d, "x2", x2) < 0) {
                Py_XDECREF(x2);
                Py_DECREF(d);
                Py_DECREF(list);
                return NULL;
            }
            Py_DECREF(x2);
        }
        PyList_SetItem(list, i, d);   /* steals d */
    }
    return list;
}

static PyObject* GP_close(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    GPObject* o = AS_GP(self);
    if (o->owned || o->busy) return gp_fail_msg(GpBusy, "GP is in use and cannot be closed");
    gp_release(o);
    Py_RETURN_NONE;
}

static PyObject* GP_enter(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    Py_INCREF(self);
    return self;
}

static PyObject* GP_exit(PyObject* self, PyObject* Py_UNUSED(args)) {
    GPObject* o = AS_GP(self);
    if (!o->owned && !o->busy) gp_release(o);
    Py_RETURN_FALSE;
}

/* simulate_outcome(p, u): a staticmethod and a module function. */
static PyObject* gp_simulate(PyObject* Py_UNUSED(self), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "p", "u", NULL };
    PyObject* po;
    double u, p[YAEP_MAX_OUTCOMES];
    Py_ssize_t K;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "Od", kw, &po, &u)) return NULL;
    K = gp_small_doubles(po, p, YAEP_MAX_OUTCOMES, "p");
    if (K < 0) return NULL;
    if (K < 1) return gp_fail_msg(PyExc_ValueError, "p is empty");
    return PyLong_FromLong(yaep_simulate_outcome(p, (int)K, u));
}

/* memory_size(**desc): bytes open() would take. An invalid desc raises with
 * the open() message instead of returning the header's 0. */
static PyObject* gp_memory_size(PyObject* Py_UNUSED(self), PyObject* args, PyObject* kwds) {
    yaep_desc d;
    double* cand;
    PyObject* rng;
    size_t n;
    if (args && PyTuple_Size(args) > 0)
        return gp_fail_msg(PyExc_TypeError, "memory_size() takes keyword arguments only");
    if (gp_parse_desc(kwds, &d, &cand, &rng) < 0) return NULL;
    n = yaep_memory_size(&d);
    if (n == 0) {
        yaep_gp* g = (yaep_gp*)PyMem_Malloc(sizeof(yaep_gp));
        if (!g) { PyMem_Free(cand); return PyErr_NoMemory(); }
        memset(g, 0, sizeof(*g));
        (void)yaep_open(g, &d);   /* fails validation; only the message is wanted */
        PyErr_SetString(GpError, yaep_error(g));
        yaep_close(g);
        PyMem_Free(g);
        PyMem_Free(cand);
        return NULL;
    }
    PyMem_Free(cand);
    return PyLong_FromSize_t(n);
}

/* --- GP properties --------------------------------------------------------- */

static PyObject* GP_get_done(PyObject* self, void* Py_UNUSED(c)) {
    if (gp_ready(AS_GP(self)) < 0) return NULL;
    return PyBool_FromLong(yaep_done(&AS_GP(self)->g));
}
static PyObject* GP_get_stop_reason(PyObject* self, void* Py_UNUSED(c)) {
    if (gp_ready(AS_GP(self)) < 0) return NULL;
    return PyLong_FromLong((long)yaep_stop_reason(&AS_GP(self)->g));
}
static PyObject* GP_get_log_marginal(PyObject* self, void* Py_UNUSED(c)) {
    if (gp_ready(AS_GP(self)) < 0) return NULL;
    return PyFloat_FromDouble(yaep_log_marginal(&AS_GP(self)->g));
}
static PyObject* GP_get_n_candidates(PyObject* self, void* Py_UNUSED(c)) {
    if (gp_ready(AS_GP(self)) < 0) return NULL;
    return PyLong_FromLong(yaep_n_candidates(&AS_GP(self)->g));
}
static PyObject* GP_get_n_trials(PyObject* self, void* Py_UNUSED(c)) {
    if (gp_ready(AS_GP(self)) < 0) return NULL;
    return PyLong_FromLong(yaep_n_trials(&AS_GP(self)->g));
}
static PyObject* GP_get_multi_cross(PyObject* self, void* Py_UNUSED(c)) {
    if (gp_ready(AS_GP(self)) < 0) return NULL;
    return PyBool_FromLong(yaep_threshold_multi_cross(&AS_GP(self)->g));
}
static PyObject* GP_get_n_dims(PyObject* self, void* Py_UNUSED(c)) {
    return PyLong_FromLong(AS_GP(self)->g.desc.n_dims);
}
static PyObject* GP_get_is_open(PyObject* self, void* Py_UNUSED(c)) {
    return PyBool_FromLong(yaep_is_open(&AS_GP(self)->g));
}

static PyGetSetDef GP_getset[] = {
    { "done", GP_get_done, NULL,
      "True once a stop criterion has fired. Runs a threshold search when "
      "stop_threshold_sd is set, so read it once per trial.", NULL },
    { "stop_reason", GP_get_stop_reason, NULL, "Why the run stopped (STOP_*).", NULL },
    { "log_marginal", GP_get_log_marginal, NULL,
      "Log marginal likelihood of the current fit (Laplace, or exact under "
      "GAUSSIAN). Log it: a value far closer to 0 than the responses allow "
      "means the fit interpolated.", NULL },
    { "n_candidates", GP_get_n_candidates, NULL, "Candidate count M.", NULL },
    { "n_trials", GP_get_n_trials, NULL, "Trials recorded.", NULL },
    { "threshold_multi_cross", GP_get_multi_cross, NULL,
      "True when the last threshold search crossed the target more than once.", NULL },
    { "n_dims", GP_get_n_dims, NULL, "Stimulus dimensions.", NULL },
    { "is_open", GP_get_is_open, NULL, "False after close().", NULL },
    { NULL }
};

static PyMethodDef GP_methods[] = {
    { "next", GP_next, METH_NOARGS,
      "next() -> (index, x): the next stimulus. index is the candidate index, "
      "or -1 for a Halton or rng point. Calling it twice without an update "
      "returns the same point. Releases the GIL unless rng is set." },
    { "next_subset", GP_next_subset, METH_O,
      "next_subset(indices) -> (index, x): next() restricted to those "
      "candidate indices; skips the init phase." },
    { "acq_score", GP_acq_score, METH_O,
      "acq_score(i) -> float: acquisition score of candidate i (NaN in the "
      "init phase)." },
    { "update", (PyCFunction)(void (*)(void))GP_update, METH_VARARGS | METH_KEYWORDS,
      "update(x, outcome): record outcome (0..K-1) at the stimulus shown, and "
      "refit. Raises Numeric when a Cholesky failed; the trial is still "
      "recorded and the previous posterior stays in force. Releases the GIL." },
    { "update_real", (PyCFunction)(void (*)(void))GP_update_real, METH_VARARGS | METH_KEYWORDS,
      "update_real(x, y): the same for a continuous y under LIK_GAUSSIAN." },
    { "predict_f", (PyCFunction)(void (*)(void))GP_predict_f, METH_VARARGS | METH_KEYWORDS,
      "predict_f(x, k=0) -> (mu, sd): latent k's posterior mean and sd." },
    { "predict_p", GP_predict_p, METH_O,
      "predict_p(x) -> float: the target quantity (E[P(y=1)], E[P(y>=k*)], "
      "E[P(y=k*)] or E[y]) at x." },
    { "predict_p_var", GP_predict_p_var, METH_O,
      "predict_p_var(x) -> float: posterior variance of the target quantity "
      "(the BALV score)." },
    { "predict_outcomes", GP_predict_outcomes, METH_O,
      "predict_outcomes(x) -> list: all K outcome probabilities." },
    { "predict_p_many", GP_predict_p_many, METH_O,
      "predict_p_many(xs) -> memoryview('d'): predict_p at n points. xs is "
      "any buffer or sequence of n x n_dims doubles. The result is a copy; "
      "numpy.frombuffer(result) views it. Releases the GIL." },
    { "predict_f_many", (PyCFunction)(void (*)(void))GP_predict_f_many, METH_VARARGS | METH_KEYWORDS,
      "predict_f_many(xs, k=0) -> (mu, sd): memoryviews of format 'd'. "
      "Releases the GIL." },
    { "threshold", (PyCFunction)(void (*)(void))GP_threshold, METH_VARARGS | METH_KEYWORDS,
      "threshold(ctx=None, target=0) -> (x, lo, hi): where predict_p crosses "
      "target along the intensity dimension at context ctx (the other "
      "n_dims - 1 coordinates). lo and hi are the crossings of the "
      "mu +- 1.96 sd latent curves. Raises NoCross." },
    { "fit", GP_fit, METH_NOARGS,
      "fit() -> int: fit the free hyperparameters. 1 when it converged, 0 when "
      "the evaluation budget (fit_max_evals) or a guard stopped it; call again "
      "to continue. Tens of refits; releases the GIL." },
    { "fit_step", GP_fit_step, METH_NOARGS,
      "fit_step() -> bool: one step of the fit; True while another would "
      "help. Releases the GIL." },
    { "refit", GP_refit, METH_NOARGS,
      "refit(): the exact Newton refit refit_every skips. Releases the GIL." },
    { "hyper", GP_hyper, METH_NOARGS,
      "hyper() -> dict: the hyperparameters in force." },
    { "candidate", GP_candidate, METH_O, "candidate(i) -> list: candidate i." },
    { "history", GP_history, METH_NOARGS,
      "history() -> list[dict]: every trial as {'x', 'y', 'proposed', 'init'}." },
    { "close", GP_close, METH_NOARGS, "close(): free the handle (idempotent)." },
    { "fit_delta", GP_fit_delta, METH_NOARGS,
      "fit_delta() -> float: the change of the fit objective (log marginal plus "
      "log prior) over the last whole fit, 0 when a guard undid it. Repeat fit() "
      "until this is small to converge a fit the budget stops." },
    { "argmax", GP_argmax, METH_NOARGS,
      "argmax() -> (x, value): the stimulus where the posterior mean of the "
      "target quantity is highest (lowest under minimize), and that mean. "
      "Releases the GIL." },
    { "get_priors", GP_get_priors, METH_NOARGS,
      "get_priors() -> dict: the weak priors in force, defaults filled in; a "
      "prior that is off has sd -1." },
    { "next_pair", GP_next_pair, METH_NOARGS,
      "next_pair() -> (x1, x2): LIK_PAIRWISE's next comparison. Releases the "
      "GIL unless rng is set." },
    { "update_pair", (PyCFunction)(void (*)(void))GP_update_pair, METH_VARARGS | METH_KEYWORDS,
      "update_pair(x1, x2, outcome): record a comparison; outcome 1 if x1 was "
      "preferred, 0 if x2. Releases the GIL." },
    { "predict_pair", GP_predict_pair, METH_VARARGS,
      "predict_pair(x1, x2) -> float: P(x1 is preferred to x2)." },
    { "save", GP_save, METH_NOARGS,
      "save() -> bytes: a snapshot of the whole session, pending proposal "
      "included; GP.load() resumes it bit for bit without replay. The rng "
      "state is not in it." },
    { "load", (PyCFunction)(void (*)(void))GP_load, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
      "GP.load(data, **desc) -> GP: resume from save(). The desc must agree "
      "with the snapshot on every number and re-supplies rng and candidates; "
      "put your generator's state back yourself. A mismatch raises Error." },
    { "memory_size", (PyCFunction)(void (*)(void))gp_memory_size,
      METH_VARARGS | METH_KEYWORDS | METH_STATIC,
      "memory_size(**desc) -> int: bytes GP(**desc) allocates at open. "
      "Raises Error with the validation message on an invalid desc." },
    { "simulate_outcome", (PyCFunction)(void (*)(void))gp_simulate,
      METH_VARARGS | METH_KEYWORDS | METH_STATIC,
      "simulate_outcome(p, u) -> int: the smallest k with cumulative p > u." },
    { "__enter__", GP_enter, METH_NOARGS, NULL },
    { "__exit__", GP_exit, METH_VARARGS, NULL },
    { NULL }
};

static PyType_Slot GP_slots[] = {
    { Py_tp_doc, (void*)"GP(**desc): a Gaussian-process adaptive run. The keywords "
                        "mirror yaep_desc: lo, hi, intensity_dim, lik, n_outcomes, "
                        "kernel, link, guess, lapse, hyper, fit, fit_every, "
                        "no_hyper_prior, refit_every, hyper_min, hyper_max, jitter, "
                        "acq, target_p, target_value, target_outcome, acq_beta, "
                        "n_init, rng, candidates, n_candidates, grid, stop_trials, "
                        "stop_threshold_sd, stop_context, max_trials, refine_steps, model, priors, "
                        "minimize, dim_kind, dim_levels, monotone_dims, pcg_threshold, "
                        "fit_max_evals, fit_tol, fit_pcg. An omitted "
                        "or zero field takes the library default." },
    { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_init, (void*)GP_init },
    { Py_tp_dealloc, (void*)GP_dealloc },
    { Py_tp_traverse, (void*)GP_traverse },
    { Py_tp_clear, (void*)GP_clear },
    { Py_tp_methods, GP_methods },
    { Py_tp_getset, GP_getset },
    { 0, NULL }
};

static PyType_Spec GP_spec = {
    "ysp.aep.GP", sizeof(GPObject), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, GP_slots
};

/* --- Async ----------------------------------------------------------------- */

#define AS_ASYNC(o) ((AsyncObject*)(o))

static PyObject* gp_snapshot(const yaep_snapshot* s, const yaep_desc* d) {
    PyObject *kw, *x, *h, *out;
    x = gp_list(s->x, d->n_dims);
    h = gp_hyper_dict(&s->hyper, d);
    if (!x || !h) { Py_XDECREF(x); Py_XDECREF(h); return NULL; }
    kw = Py_BuildValue("{s:k,s:N,s:i,s:i,s:i,s:O,s:i,s:d,s:d,s:d,s:i,s:O,s:N,s:d,s:i,s:O}",
                       "seq", (unsigned long)s->seq, "x", x, "proposed", s->proposed,
                       "update_rc", s->update_rc, "n_trials", s->n_trials,
                       "done", s->done ? Py_True : Py_False, "stop", (int)s->stop,
                       "threshold", s->threshold, "threshold_lo", s->threshold_lo,
                       "threshold_hi", s->threshold_hi, "threshold_rc", s->threshold_rc,
                       "multi_cross", s->multi_cross ? Py_True : Py_False,
                       "hyper", h, "log_marginal", s->log_marginal,
                       "numeric", s->numeric,
                       "fitting", s->fitting ? Py_True : Py_False);
    if (!kw) return NULL;
    {
        PyObject* empty = PyTuple_New(0);
        out = empty ? PyObject_Call(SnapshotType, empty, kw) : NULL;
        Py_XDECREF(empty);
    }
    Py_DECREF(kw);
    return out;
}

static int Async_init(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "gp", "context", "target", "fit_in_idle", "below_normal",
                          "pin_cpu", NULL };
    AsyncObject* o = AS_ASYNC(self);
    PyObject *gpo, *ctx = NULL;
    double target = 0.0;
    int fit_in_idle = 0, below_normal = 0, pin_cpu = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|Odppi", kw, &gpo, &ctx, &target,
                                     &fit_in_idle, &below_normal, &pin_cpu))
        return -1;
    if (!PyObject_TypeCheck(gpo, (PyTypeObject*)GpType)) {
        PyErr_SetString(PyExc_TypeError, "Async(gp): gp must be a ysp.aep.GP");
        return -1;
    }
    if (o->started || o->stopping) {
        gp_fail_msg(GpBusy, "Async is running; stop() it before re-initializing");
        return -1;
    }
    if (!yaep_is_open(&AS_GP(gpo)->g)) {
        gp_fail(YAEP_ERR_CLOSED, "Async");
        return -1;
    }
    if (gp_context(ctx, AS_GP(gpo)->g.desc.n_dims, o->context) < 0) return -1;
    o->target = target;
    o->fit_in_idle = fit_in_idle;
    o->below_normal = below_normal;
    o->pin_cpu = pin_cpu;
    Py_INCREF(gpo);
    {
        GPObject* old = o->gp;
        o->gp = AS_GP(gpo);
        Py_XDECREF((PyObject*)old);
    }
    return 0;
}

static int async_ready(AsyncObject* o) {
    if (!o->gp) { gp_fail_msg(GpError, "Async was not initialized"); return -1; }
    if (o->stopping) { gp_fail_msg(GpBusy, "Async is stopping"); return -1; }
    return 0;
}

static void async_stop(AsyncObject* o) {
    /* `started`, not gp->owned: a second Async on the same GP must not hand
     * back a handle that the first one still drives. */
    if (!o->gp || !o->started) return;
    o->stopping = 1;
    /* The pump drains the queue and joins; the thread touches no Python
     * object, so it can finish with the GIL down. */
    Py_BEGIN_ALLOW_THREADS
    yaep_async_close(&o->a);
    Py_END_ALLOW_THREADS
    o->stopping = 0;
    o->started = 0;
    o->gp->owned = 0;
}

static PyObject* Async_start(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    AsyncObject* o = AS_ASYNC(self);
    yaep_async_desc d;
    bool ok;
    if (async_ready(o) < 0) return NULL;
    if (o->gp->owned) return gp_fail_msg(GpBusy, "this GP is already owned by a running Async");
    if (o->waiters) return gp_fail_msg(GpBusy, "a wait() is still blocked on this Async");
    if (gp_ready(o->gp) < 0) return NULL;
    if (o->gp->g.desc.lik == YAEP_LIK_PAIRWISE)
        return gp_fail_msg(PyExc_ValueError,
                           "Async submits single stimuli; LIK_PAIRWISE is not supported");
    if (o->gp->rng)
        return gp_fail_msg(PyExc_ValueError,
                           "Async cannot drive a GP whose rng is a Python callable: "
                           "the pump thread never touches the interpreter");
    memset(&d, 0, sizeof(d));
    d.gp = &o->gp->g;
    memcpy(d.context, o->context, sizeof(d.context));
    d.target = o->target;
    d.fit_in_idle = o->fit_in_idle ? true : false;
    d.below_normal = o->below_normal ? true : false;
    d.pin_cpu = o->pin_cpu;
    o->gp->owned = 1;
    /* start() runs one yaep_next() and one yaep_threshold() on this thread
     * before the pump exists; that is tens of ms under EAVC. */
    Py_BEGIN_ALLOW_THREADS
    ok = yaep_async_open(&o->a, &d);
    Py_END_ALLOW_THREADS
    if (!ok) {
        o->gp->owned = 0;
        return gp_fail_msg(GpError, yaep_async_error(&o->a));
    }
    o->started = 1;
    Py_RETURN_NONE;
}

static PyObject* Async_stop(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    AsyncObject* o = AS_ASYNC(self);
    if (o->stopping) return gp_fail_msg(GpBusy, "Async is already stopping");
    async_stop(o);
    Py_RETURN_NONE;
}

static PyObject* Async_enter(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    PyObject* r = Async_start(self, NULL);
    if (!r) return NULL;
    Py_DECREF(r);
    Py_INCREF(self);
    return self;
}

static PyObject* Async_exit(PyObject* self, PyObject* Py_UNUSED(args)) {
    AsyncObject* o = AS_ASYNC(self);
    if (!o->stopping) async_stop(o);
    Py_RETURN_FALSE;
}

static PyObject* async_submit_common(AsyncObject* o, PyObject* xo, PyObject* yo, int real) {
    double x[YAEP_MAX_DIMS], y = 0.0;
    int outcome = 0, rc;
    if (async_ready(o) < 0) return NULL;
    if (gp_point(xo, o->gp->g.desc.n_dims, x, "x") < 0) return NULL;
    if (real) { if (gp_as_double(yo, &y) < 0) return NULL; }
    else if (gp_int(yo, &outcome) < 0) return NULL;
    /* A mutex, an 80-byte copy and a signal: not worth dropping the GIL. */
    rc = real ? yaep_async_submit_real(&o->a, x, y) : yaep_async_submit(&o->a, x, outcome);
    if (rc < 0) return gp_fail(rc, real ? "submit_real" : "submit");
    return PyLong_FromLong(rc);
}

static PyObject* Async_submit(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "x", "outcome", NULL };
    PyObject *x, *y;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO", kw, &x, &y)) return NULL;
    return async_submit_common(AS_ASYNC(self), x, y, 0);
}

static PyObject* Async_submit_real(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "x", "y", NULL };
    PyObject *x, *y;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO", kw, &x, &y)) return NULL;
    return async_submit_common(AS_ASYNC(self), x, y, 1);
}

static PyObject* async_result(AsyncObject* o, int seq, const yaep_snapshot* s) {
    PyObject* snap = gp_snapshot(s, &o->gp->g.desc);
    if (!snap) return NULL;
    return Py_BuildValue("(iN)", seq, snap);
}

static PyObject* Async_poll(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    AsyncObject* o = AS_ASYNC(self);
    yaep_snapshot s;
    int seq;
    if (async_ready(o) < 0) return NULL;
    seq = yaep_async_poll(&o->a, &s);
    if (seq < 0) return gp_fail(seq, "poll");
    return async_result(o, seq, &s);
}

static PyObject* Async_wait(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "seq", "timeout_s", NULL };
    AsyncObject* o = AS_ASYNC(self);
    PyObject* to = Py_None;
    unsigned long seq;
    int64_t ns = INT64_MAX;
    yaep_snapshot s;
    int rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "k|O", kw, &seq, &to)) return NULL;
    if (async_ready(o) < 0) return NULL;
    if (seq > 0xFFFFFFFFul) return gp_fail_msg(PyExc_OverflowError, "seq must fit in 32 bits");
    if (to != Py_None) {
        double t;
        if (gp_as_double(to, &t) < 0) return NULL;
        if (!(t >= 0.0)) return gp_fail_msg(PyExc_ValueError, "timeout_s must be >= 0");
        ns = t >= 9.2e9 ? INT64_MAX : (int64_t)(t * 1e9);
    }
    o->waiters++;
    Py_BEGIN_ALLOW_THREADS
    rc = yaep_async_wait(&o->a, (uint32_t)seq, ns, &s);
    Py_END_ALLOW_THREADS
    o->waiters--;
    if (rc < 0) return gp_fail(rc, "wait");
    return async_result(o, rc, &s);
}

static PyObject* Async_get_pending(PyObject* self, void* Py_UNUSED(c)) {
    AsyncObject* o = AS_ASYNC(self);
    int n;
    if (async_ready(o) < 0) return NULL;
    n = yaep_async_pending(&o->a);
    if (n < 0) return gp_fail(n, "pending");
    return PyLong_FromLong(n);
}

static PyObject* Async_get_policy(PyObject* self, void* Py_UNUSED(c)) {
    return PyLong_FromLong((long)yaep_async_policy(&AS_ASYNC(self)->a));
}

static PyObject* Async_get_running(PyObject* self, void* Py_UNUSED(c)) {
    return PyBool_FromLong(yaep_async_is_running(&AS_ASYNC(self)->a));
}

static PyObject* Async_get_gp(PyObject* self, void* Py_UNUSED(c)) {
    AsyncObject* o = AS_ASYNC(self);
    if (!o->gp) Py_RETURN_NONE;
    Py_INCREF((PyObject*)o->gp);
    return (PyObject*)o->gp;
}

static int Async_traverse(PyObject* self, visitproc visit, void* arg) {
    Py_VISIT((PyObject*)AS_ASYNC(self)->gp);
    return 0;
}

static int Async_clear(PyObject* self) {
    AsyncObject* o = AS_ASYNC(self);
    /* A running pump points into o->gp; the reference goes only after stop. */
    if (o->started) async_stop(o);
    Py_CLEAR(o->gp);
    return 0;
}

static void Async_dealloc(PyObject* self) {
    PyTypeObject* tp = Py_TYPE(self);
    freefunc tp_free;
    PyObject_GC_UnTrack(self);
    Async_clear(self);
    tp_free = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    tp_free(self);
    Py_DECREF(tp);
}

static PyGetSetDef Async_getset[] = {
    { "pending", Async_get_pending, NULL,
      "Responses queued and not yet applied. Takes the queue's mutex: for a "
      "log line, not a frame loop.", NULL },
    { "policy", Async_get_policy, NULL,
      "Scheduling rung the thread runs at: POLICY_NORMAL, or "
      "POLICY_BELOW_NORMAL when below_normal was asked for and granted; "
      "POLICY_NONE when not running. Log it.", NULL },
    { "running", Async_get_running, NULL, "True while the thread runs.", NULL },
    { "gp", Async_get_gp, NULL, "The GP this Async drives.", NULL },
    { NULL }
};

static PyMethodDef Async_methods[] = {
    { "start", Async_start, METH_NOARGS,
      "start(): take the GP over and start the thread. The first proposal is "
      "ready when it returns: poll() gives seq 0. Until stop() returns, every "
      "call on the GP raises Busy." },
    { "stop", Async_stop, METH_NOARGS,
      "stop(): apply every queued response, join the thread, give the GP back." },
    { "submit", (PyCFunction)(void (*)(void))Async_submit, METH_VARARGS | METH_KEYWORDS,
      "submit(x, outcome) -> seq: queue one response and return at once. "
      "Raises Busy when the queue is full; nothing was copied, retry next frame." },
    { "submit_real", (PyCFunction)(void (*)(void))Async_submit_real, METH_VARARGS | METH_KEYWORDS,
      "submit_real(x, y) -> seq: the same under LIK_GAUSSIAN." },
    { "poll", Async_poll, METH_NOARGS,
      "poll() -> (seq, Snapshot): the newest snapshot. seq >= a submit's seq "
      "means that response is applied and snapshot.x is the next stimulus. "
      "Cheap enough for every frame." },
    { "wait", (PyCFunction)(void (*)(void))Async_wait, METH_VARARGS | METH_KEYWORDS,
      "wait(seq, timeout_s=None) -> (seq, Snapshot): block, GIL released, until "
      "response seq is applied or timeout_s passes (Timeout). None waits forever." },
    { "__enter__", Async_enter, METH_NOARGS, NULL },
    { "__exit__", Async_exit, METH_VARARGS, NULL },
    { NULL }
};

static PyType_Slot Async_slots[] = {
    { Py_tp_doc, (void*)"Async(gp, context=None, target=0, fit_in_idle=False, "
                        "below_normal=False, pin_cpu=0): run gp's update, next and "
                        "threshold on a C thread that never touches the interpreter. "
                        "context is where the snapshot's threshold is taken (the "
                        "n_dims - 1 non-intensity coordinates). A context manager: "
                        "`with Async(gp) as a:` starts and stops it." },
    { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_init, (void*)Async_init },
    { Py_tp_dealloc, (void*)Async_dealloc },
    { Py_tp_traverse, (void*)Async_traverse },
    { Py_tp_clear, (void*)Async_clear },
    { Py_tp_methods, Async_methods },
    { Py_tp_getset, Async_getset },
    { 0, NULL }
};

static PyType_Spec Async_spec = {
    "ysp.aep.Async", sizeof(AsyncObject), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, Async_slots
};

/* --- module ---------------------------------------------------------------- */

static PyMethodDef module_methods[] = {
    { "simulate_outcome", (PyCFunction)(void (*)(void))gp_simulate, METH_VARARGS | METH_KEYWORDS,
      "simulate_outcome(p, u) -> int: draw an outcome from the probabilities p "
      "with the caller's uniform variate u in [0, 1): the smallest k with "
      "cumulative p > u." },
    { "memory_size", (PyCFunction)(void (*)(void))gp_memory_size, METH_VARARGS | METH_KEYWORDS,
      "memory_size(**desc) -> int: bytes GP(**desc) allocates at open." },
    { NULL }
};

static PyModuleDef ysp_aep_module = {
    PyModuleDef_HEAD_INIT,
    "ysp.aep",
    "Gaussian-process adaptive psychophysics (ysp/aep.h): Laplace GP "
    "classification over a stimulus box, level-set and global acquisitions, "
    "and an optional inference thread.",
    -1,
    module_methods,
};

static PyObject* gp_add_exc(PyObject* m, const char* qualname, const char* attr,
                            PyObject* base, const char* doc) {
    PyObject* e = PyErr_NewExceptionWithDoc(qualname, doc, base, NULL);
    if (!e) return NULL;
    Py_INCREF(e);
    if (PyModule_AddObject(m, attr, e) < 0) { Py_DECREF(e); Py_DECREF(e); return NULL; }
    return e;
}

/* A types.SimpleNamespace subclass: attribute access, a readable repr and
 * equality for free, and no Python source file in a namespace package. */
static PyObject* gp_namespace_type(PyObject* m, const char* name, const char* doc) {
    PyObject *types = PyImport_ImportModule("types"), *ns, *t = NULL;
    if (!types) return NULL;
    ns = PyObject_GetAttrString(types, "SimpleNamespace");
    Py_DECREF(types);
    if (!ns) return NULL;
    t = PyObject_CallFunction((PyObject*)&PyType_Type, "s(O){s:s,s:s}", name, ns,
                              "__module__", "ysp.aep", "__doc__", doc);
    Py_DECREF(ns);
    if (!t) return NULL;
    Py_INCREF(t);
    if (PyModule_AddObject(m, name, t) < 0) { Py_DECREF(t); Py_DECREF(t); return NULL; }
    return t;
}

/* The loader looks for PyInit_aep in ysp/aep.abi3.so. */
PyMODINIT_FUNC PyInit_aep(void) {
    PyObject* m = PyModule_Create(&ysp_aep_module);
    PyObject* at;
    if (!m) return NULL;

    GpType = PyType_FromSpec(&GP_spec);
    if (!GpType) goto fail;
    Py_INCREF(GpType);
    if (PyModule_AddObject(m, "GP", GpType) < 0) { Py_DECREF(GpType); goto fail; }
    at = PyType_FromSpec(&Async_spec);
    if (!at) goto fail;
    if (PyModule_AddObject(m, "Async", at) < 0) { Py_DECREF(at); goto fail; }

    GpError = gp_add_exc(m, "ysp.aep.Error", "Error", NULL,
                         "Base of every ysp.aep error; also a bad desc, a closed "
                         "handle or a bad argument. `.code` is the YAEP_ERR_* value.");
    if (!GpError) goto fail;
    GpNumeric = gp_add_exc(m, "ysp.aep.Numeric", "Numeric", GpError,
                           "A Cholesky failed. The trial is recorded and the "
                           "previous posterior stays in force.");
    if (!GpNumeric) goto fail;
    GpNoCross = gp_add_exc(m, "ysp.aep.NoCross", "NoCross", GpError,
                           "No level-set crossing inside the box at that context.");
    if (!GpNoCross) goto fail;
    GpFull = gp_add_exc(m, "ysp.aep.Full", "Full", GpError, "max_trials recorded.");
    if (!GpFull) goto fail;
    GpBusy = gp_add_exc(m, "ysp.aep.Busy", "Busy", GpError,
                        "The Async queue is full (retry next frame), or the GP is "
                        "owned by an Async or in use on another thread.");
    if (!GpBusy) goto fail;
    GpTimeout = gp_add_exc(m, "ysp.aep.Timeout", "Timeout", GpError,
                           "Async.wait() ran out of time.");
    if (!GpTimeout) goto fail;

    if (!gp_namespace_type(m, "Priors",
        "Priors(lengthscale=..., outputscale=..., outputscale_b=..., "
        "outputscale_g=..., mean=..., mean_g=..., noise_sd=...): the weak "
        "hyperparameter priors for GP(priors=...). Each is a dict {center, sd, "
        "ceiling} or a (center, sd[, ceiling]) tuple; 0 or missing is the "
        "measured default and a negative sd turns that prior off. A dict of "
        "the same keys works too.")) goto fail;
    HyperType = gp_namespace_type(m, "Hyper",
        "Hyper(lengthscale=[...], outputscale=0, mean=0, lengthscale_b=[...], "
        "outputscale_b=0, cutpoint=[...], noise_sd=0, lengthscale_g=[...], "
        "outputscale_g=0, mean_g=0): hyperparameters for "
        "GP(hyper=...). A zero or missing field is a default, or fitted when "
        "fit=True. A dict with the same keys works too.");
    if (!HyperType) goto fail;
    SnapshotType = gp_namespace_type(m, "Snapshot",
        "What Async publishes after every response: seq, x, proposed, "
        "update_rc, n_trials, done, stop, threshold, threshold_lo, "
        "threshold_hi, threshold_rc, multi_cross, hyper, log_marginal, "
        "numeric, fitting.");
    if (!SnapshotType) goto fail;

    /* From the compiled header, not a copy: the drift test compares it with
     * pyproject.toml. */
    if (PyModule_AddStringConstant(m, "__version__", YAEP_VERSION_STRING ".1") < 0) goto fail;   /* .1: the CPython 3.10 floor (2026-10-27) */

#define C(name, v) if (PyModule_AddIntConstant(m, name, (long)(v)) < 0) goto fail
    C("LIK_BERNOULLI", YAEP_LIK_BERNOULLI);
    C("LIK_ORDINAL", YAEP_LIK_ORDINAL);
    C("LIK_CATEGORICAL", YAEP_LIK_CATEGORICAL);
    C("LIK_GAUSSIAN", YAEP_LIK_GAUSSIAN);
    C("LIK_PAIRWISE", YAEP_LIK_PAIRWISE);
    C("DIM_CONTINUOUS", YAEP_DIM_CONTINUOUS);
    C("DIM_INTEGER", YAEP_DIM_INTEGER);
    C("DIM_CATEGORICAL", YAEP_DIM_CATEGORICAL);
    C("MODEL_GP", YAEP_MODEL_GP);
    C("MODEL_PSYCHOMETRIC", YAEP_MODEL_PSYCHOMETRIC);
    C("KERNEL_RBF", YAEP_KERNEL_RBF);
    C("KERNEL_SEMIP", YAEP_KERNEL_SEMIP);
    C("LINK_PROBIT", YAEP_LINK_PROBIT);
    C("LINK_LOGIT", YAEP_LINK_LOGIT);
    C("ACQ_LSE", YAEP_ACQ_LSE);
    C("ACQ_EAVC", YAEP_ACQ_EAVC);
    C("ACQ_LOCALMI", YAEP_ACQ_LOCALMI);
    C("ACQ_BALV", YAEP_ACQ_BALV);
    C("ACQ_BALD", YAEP_ACQ_BALD);
    C("ACQ_RANDOM", YAEP_ACQ_RANDOM);
    C("ACQ_UCB", YAEP_ACQ_UCB);
    C("ACQ_EI", YAEP_ACQ_EI);
    C("ACQ_THOMPSON", YAEP_ACQ_THOMPSON);
    C("STOP_NONE", YAEP_STOP_NONE);
    C("STOP_TRIALS", YAEP_STOP_TRIALS);
    C("STOP_THRESHOLD_SD", YAEP_STOP_THRESHOLD_SD);
    C("STOP_FULL", YAEP_STOP_FULL);
    C("OK", YAEP_OK);
    C("ERR_ARG", YAEP_ERR_ARG);
    C("ERR_CLOSED", YAEP_ERR_CLOSED);
    C("ERR_FULL", YAEP_ERR_FULL);
    C("ERR_MEMORY", YAEP_ERR_MEMORY);
    C("ERR_NOCROSS", YAEP_ERR_NOCROSS);
    C("ERR_NUMERIC", YAEP_ERR_NUMERIC);
    C("ERR_BUSY", YAEP_ERR_BUSY);
    C("ERR_TIMEOUT", YAEP_ERR_TIMEOUT);
    /* ysp/rt.h's yrt_policy, the same numbers ysp.serial and ysp.parallel
     * report as ASYNC_*; only the two bottom rungs occur for a pump. */
    C("POLICY_NONE", YRT_POLICY_NONE);
    C("POLICY_NORMAL", YRT_POLICY_NORMAL);
    C("POLICY_BELOW_NORMAL", YRT_POLICY_BELOW_NORMAL);
    C("MAX_DIMS", YAEP_MAX_DIMS);
    C("MAX_OUTCOMES", YAEP_MAX_OUTCOMES);
    C("MAX_TRIALS", YAEP_MAX_TRIALS);
    C("QUAD_N", YAEP_QUAD_N);
    C("ASYNC_QUEUE", YAEP_ASYNC_QUEUE);
#undef C
    return m;
fail:
    Py_DECREF(m);
    return NULL;
}
