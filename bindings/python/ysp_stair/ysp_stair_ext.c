/* ysp_stair_ext.c - CPython extension wrapping ysp/stair.h (module ysp.stair)
 *
 * A thin, dependency-free binding (no nanobind/pybind/Cython): it needs only
 * Python.h, matching the single-header library's zero-dependency style. The
 * library implementation is compiled directly into this module.
 *
 * Built against the stable ABI / Limited API (Py_LIMITED_API), so one compiled
 * ysp/stair.abi3.so works across CPython >= 3.10 without recompiling per
 * version. That rules out the static PyTypeObject layout, so the Staircase
 * type is a heap type created with PyType_FromSpec.
 *
 *     import ysp.stair as st
 *     s = st.Staircase(start=0.5, n_down=3, step_type=st.STEP_LOG,
 *                      steps=[0.3, 0.15, 0.075], min=0.001, max=1.0,
 *                      stop_reversals=10)
 *     while not s.done:
 *         level = s.next()
 *         s.update(level, run_trial(level))
 *     print(s.estimate(st.EST_REVERSALS))
 *
 * THREADING: none. The header is pure arithmetic on an inline handle, every
 * call is O(1) or O(reversals), so no call releases the GIL: the GIL is what
 * keeps two Python threads from racing on one handle.
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x030A0000   /* target the CPython 3.10+ stable ABI */
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define YSP_STAIR_IMPLEMENTATION
#include "ysp/stair.h"

#include <string.h>

typedef struct {
    PyObject_HEAD
    yst_stair s;
} StairObject;

#define AS_STAIR(self) (&((StairObject*)(self))->s)

/* Exceptions: Error is the base; ArgumentError is also a ValueError so code
 * that already catches ValueError for bad input keeps working. */
static PyObject* StError;
static PyObject* StArgumentError;
static PyObject* StClosed;
static PyObject* StFull;

/* The enum classes, kept so return values can be wrapped in them. */
static PyObject* StStopEnum;
static PyObject* StEventFlag;
static PyObject* StTrialType;   /* collections.namedtuple for history() */

static PyObject* st_fail(int code) {
    PyObject* exc = StError;
    if (code == YST_ERR_ARG)         exc = StArgumentError;
    else if (code == YST_ERR_CLOSED) exc = StClosed;
    else if (code == YST_ERR_FULL)   exc = StFull;
    PyErr_SetString(exc, yst_strerror(code));
    return NULL;
}

/* Wrap a plain int in one of the enum classes; falls back to the int if the
 * class rejects it, so an unexpected value is still visible. */
static PyObject* st_enum(PyObject* cls, long v) {
    PyObject* r = PyObject_CallFunction(cls, "l", v);
    if (!r) { PyErr_Clear(); r = PyLong_FromLong(v); }
    return r;
}

static int st_require_open(PyObject* self) {
    if (!yst_is_open(AS_STAIR(self))) {
        PyErr_SetString(StClosed, yst_strerror(YST_ERR_CLOSED));
        return -1;
    }
    return 0;
}

/* steps may be one number or a sequence of up to YST_MAX_STEPS numbers. */
static int st_parse_steps(PyObject* obj, yst_desc* d) {
    Py_ssize_t n, i;
    if (!obj || obj == Py_None) return 0;
    if (PyFloat_Check(obj) || PyLong_Check(obj)) {
        d->steps[0] = PyFloat_AsDouble(obj);
        if (PyErr_Occurred()) return -1;
        d->n_steps = 1;
        return 0;
    }
    n = PySequence_Size(obj);
    if (n < 0) {
        PyErr_Clear();
        PyErr_SetString(PyExc_TypeError, "steps must be a number or a sequence of numbers");
        return -1;
    }
    if (n > YST_MAX_STEPS) {
        PyErr_Format(StArgumentError, "steps has %zd entries; the maximum is %d",
                     n, YST_MAX_STEPS);
        return -1;
    }
    for (i = 0; i < n; i++) {
        PyObject* it = PySequence_GetItem(obj, i);
        if (!it) return -1;
        d->steps[i] = PyFloat_AsDouble(it);
        Py_DECREF(it);
        if (PyErr_Occurred()) return -1;
    }
    d->n_steps = (int)n;
    return 0;
}

/* Build the desc from the keywords, then open, or load from `data` when it
 * is not NULL. */
static int st_setup(PyObject* self, PyObject* args, PyObject* kwds,
                    const void* data, size_t len) {
    static char* kw[] = { "start", "rule", "n_up", "n_down", "step_type", "steps",
                          "step_down_scale", "target_p", "min", "max",
                          "use_limits", "harder_is_up", "initial_rule",
                          "stop_reversals", "stop_trials", "stop_at_limit",
                          "est_reversals", "est_trials", NULL };
    yst_desc d;
    int rule = 0, step_type = 0, use_limits = 0, harder_is_up = 0, initial_rule = 0;
    PyObject* steps = NULL;

    memset(&d, 0, sizeof(d));
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$diiiiOddddpppiiiii", kw,
                                     &d.start, &rule, &d.n_up, &d.n_down, &step_type,
                                     &steps, &d.step_down_scale, &d.target_p,
                                     &d.min, &d.max, &use_limits, &harder_is_up,
                                     &initial_rule, &d.stop_reversals, &d.stop_trials,
                                     &d.stop_at_limit, &d.est_reversals, &d.est_trials))
        return -1;
    if (st_parse_steps(steps, &d) < 0) return -1;
    d.rule = (yst_rule)rule;
    d.step_type = (yst_step_type)step_type;
    d.use_limits = use_limits ? true : false;
    d.harder_is_up = harder_is_up ? true : false;
    d.initial_rule = initial_rule ? true : false;

    if (!(data ? yst_open_saved(AS_STAIR(self), &d, data, len) : yst_open(AS_STAIR(self), &d))) {
        /* Every failure is a desc the manual lists as invalid, or a snapshot
         * that does not belong to the desc or is corrupt. */
        PyErr_SetString(StArgumentError, yst_error(AS_STAIR(self)));
        return -1;
    }
    return 0;
}

static int Stair_init(PyObject* self, PyObject* args, PyObject* kwds) {
    return st_setup(self, args, kwds, NULL, 0);
}

/* --- snapshots ------------------------------------------------------------ */

static PyObject* Stair_save(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    size_t n;
    PyObject* b;
    int rc;
    if (st_require_open(self) < 0) return NULL;
    n = yst_save_size(AS_STAIR(self));
    b = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)n);
    if (!b) return NULL;
    rc = yst_save(AS_STAIR(self), PyBytes_AsString(b), n);
    if (rc < 0) { Py_DECREF(b); return st_fail(rc); }
    return b;
}

/* Staircase.load(data, **desc): a new Staircase resumed from save(). */
static PyObject* Stair_load(PyObject* cls, PyObject* args, PyObject* kwds) {
    PyObject *bytes, *self, *rest;
    Py_ssize_t na = PyTuple_Size(args);
    int rc;
    if (na != 1) {
        PyErr_SetString(PyExc_TypeError, "load(data, **desc) takes the snapshot bytes "
                        "and the desc as keywords");
        return NULL;
    }
    bytes = PyBytes_FromObject(PyTuple_GetItem(args, 0));
    if (!bytes) return NULL;
    rest = PyTuple_New(0);
    self = rest ? PyObject_CallMethod(cls, "__new__", "O", cls) : NULL;
    if (!self) { Py_XDECREF(rest); Py_DECREF(bytes); return NULL; }
    rc = st_setup(self, rest, kwds, PyBytes_AsString(bytes), (size_t)PyBytes_Size(bytes));
    Py_DECREF(rest);
    Py_DECREF(bytes);
    if (rc < 0) { Py_DECREF(self); return NULL; }
    return self;
}

static void Stair_dealloc(PyObject* self) {
    PyTypeObject* tp = Py_TYPE(self);
    freefunc tp_free = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    tp_free(self);
    Py_DECREF(tp);
}

/* --- the loop ------------------------------------------------------------ */

static PyObject* Stair_next(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    if (st_require_open(self) < 0) return NULL;
    return PyFloat_FromDouble(yst_next(AS_STAIR(self)));
}

static PyObject* Stair_update(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "level", "response", NULL };
    double level;
    int response;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "di", kw, &level, &response)) return NULL;
    int rc = yst_update(AS_STAIR(self), level, response);
    if (rc < 0) return st_fail(rc);
    return st_enum(StEventFlag, rc);
}

static PyObject* Stair_estimate(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "how", NULL };
    int how = YST_EST_REVERSALS;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|i", kw, &how)) return NULL;
    if (st_require_open(self) < 0) return NULL;
    if (how < YST_EST_REVERSALS || how > YST_EST_LAST) {
        PyErr_SetString(StArgumentError, "how must be one of the EST_* values");
        return NULL;
    }
    return PyFloat_FromDouble(yst_estimate(AS_STAIR(self), (yst_estimator)how));
}

static PyObject* Stair_estimate_count(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "how", NULL };
    int how = YST_EST_REVERSALS;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|i", kw, &how)) return NULL;
    if (st_require_open(self) < 0) return NULL;
    if (how < YST_EST_REVERSALS || how > YST_EST_LAST) {
        PyErr_SetString(StArgumentError, "how must be one of the EST_* values");
        return NULL;
    }
    return PyLong_FromLong(yst_estimate_count(AS_STAIR(self), (yst_estimator)how));
}

static PyObject* Stair_history(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    int n = 0, i;
    const yst_trial* h;
    PyObject* list;
    if (st_require_open(self) < 0) return NULL;
    h = yst_history(AS_STAIR(self), &n);
    list = PyList_New(n);
    if (!list) return NULL;
    for (i = 0; i < n; i++) {
        PyObject* t = PyObject_CallFunction(StTrialType, "ddiOii",
                                            h[i].proposed, h[i].shown,
                                            (int)h[i].response,
                                            h[i].reversal ? Py_True : Py_False,
                                            (int)h[i].direction, (int)h[i].step_index);
        if (!t) { Py_DECREF(list); return NULL; }
        PyList_SetItem(list, i, t);  /* steals t */
    }
    return list;
}

static PyObject* Stair_reversal_level(PyObject* self, PyObject* arg) {
    long i = PyLong_AsLong(arg);
    if (i == -1 && PyErr_Occurred()) return NULL;
    if (st_require_open(self) < 0) return NULL;
    if (i < 0 || i >= yst_n_reversals(AS_STAIR(self)) || i >= YST_MAX_REVERSALS) {
        PyErr_SetString(PyExc_IndexError, "reversal index out of range (or past MAX_REVERSALS)");
        return NULL;
    }
    return PyFloat_FromDouble(yst_reversal_level(AS_STAIR(self), (int)i));
}

static PyObject* Stair_reversal_trial(PyObject* self, PyObject* arg) {
    long i = PyLong_AsLong(arg);
    if (i == -1 && PyErr_Occurred()) return NULL;
    if (st_require_open(self) < 0) return NULL;
    if (i < 0 || i >= yst_n_reversals(AS_STAIR(self)) || i >= YST_MAX_REVERSALS) {
        PyErr_SetString(PyExc_IndexError, "reversal index out of range (or past MAX_REVERSALS)");
        return NULL;
    }
    return PyLong_FromLong(yst_reversal_trial(AS_STAIR(self), (int)i));
}

static PyObject* Stair_reversals(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    int n, i;
    PyObject* list;
    if (st_require_open(self) < 0) return NULL;
    n = yst_n_reversals(AS_STAIR(self));
    if (n > YST_MAX_REVERSALS) n = YST_MAX_REVERSALS;
    list = PyList_New(n);
    if (!list) return NULL;
    for (i = 0; i < n; i++) {
        PyObject* t = Py_BuildValue("(id)", yst_reversal_trial(AS_STAIR(self), i),
                                    yst_reversal_level(AS_STAIR(self), i));
        if (!t) { Py_DECREF(list); return NULL; }
        PyList_SetItem(list, i, t);
    }
    return list;
}

/* --- properties ------------------------------------------------------------ */

static PyObject* Stair_get_done(PyObject* self, void* Py_UNUSED(c)) {
    return PyBool_FromLong(yst_done(AS_STAIR(self)));
}
static PyObject* Stair_get_stop_reason(PyObject* self, void* Py_UNUSED(c)) {
    return st_enum(StStopEnum, (long)yst_stop_reason(AS_STAIR(self)));
}
static PyObject* Stair_get_n_trials(PyObject* self, void* Py_UNUSED(c)) {
    return PyLong_FromLong(yst_n_trials(AS_STAIR(self)));
}
static PyObject* Stair_get_n_reversals(PyObject* self, void* Py_UNUSED(c)) {
    return PyLong_FromLong(yst_n_reversals(AS_STAIR(self)));
}
static PyObject* Stair_get_is_open(PyObject* self, void* Py_UNUSED(c)) {
    return PyBool_FromLong(yst_is_open(AS_STAIR(self)));
}

static PyGetSetDef Stair_getset[] = {
    { "done",        Stair_get_done,        NULL,
      "True once a stop criterion has fired. Stays true; later updates are "
      "still recorded.", NULL },
    { "stop_reason", Stair_get_stop_reason, NULL,
      "Which criterion ended the staircase (Stop); Stop.NONE while running.", NULL },
    { "n_trials",    Stair_get_n_trials,    NULL, "Trials recorded.", NULL },
    { "n_reversals", Stair_get_n_reversals, NULL,
      "Reversals counted, including any past MAX_REVERSALS whose level was "
      "not recorded.", NULL },
    { "is_open",     Stair_get_is_open,     NULL,
      "True after a successful construction.", NULL },
    { NULL }
};

static PyMethodDef Stair_methods[] = {
    { "next", Stair_next, METH_NOARGS,
      "next() -> float: the level the rule proposes for the next trial. "
      "Calling it twice without an update returns the same value." },
    { "update", (PyCFunction)Stair_update, METH_VARARGS | METH_KEYWORDS,
      "update(level, response) -> Event: record a trial shown at `level` with "
      "`response` (1 correct / detected, 0 not) and advance the rule. Returns "
      "the Event mask (0 when only the counters moved). The rule steps from its "
      "own proposal, not from `level`." },
    { "estimate", (PyCFunction)Stair_estimate, METH_VARARGS | METH_KEYWORDS,
      "estimate(how=EST_REVERSALS) -> float: threshold estimate; NaN when the "
      "window is empty." },
    { "estimate_count", (PyCFunction)Stair_estimate_count, METH_VARARGS | METH_KEYWORDS,
      "estimate_count(how=EST_REVERSALS) -> int: reversals or trials the "
      "estimate averages right now; 0 means the estimate is NaN." },
    { "history", Stair_history, METH_NOARGS,
      "history() -> list[Trial]: every recorded trial, oldest first, as Trial "
      "named tuples (proposed, shown, response, reversal, direction, step_index)." },
    { "reversal_level", Stair_reversal_level, METH_O,
      "reversal_level(i) -> float: the proposed level at reversal i. "
      "IndexError past the count or past MAX_REVERSALS." },
    { "reversal_trial", Stair_reversal_trial, METH_O,
      "reversal_trial(i) -> int: the trial index of reversal i." },
    { "reversals", Stair_reversals, METH_NOARGS,
      "reversals() -> list[(trial, level)]: every recorded reversal." },
    { "save", Stair_save, METH_NOARGS,
      "save() -> bytes: a snapshot of the staircase; load() resumes it without "
      "replaying the updates." },
    { "load", (PyCFunction)(void (*)(void))Stair_load, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
      "Staircase.load(data, **desc) -> Staircase: resume from save(). The desc "
      "keywords are the constructor's and must open the same staircase; a "
      "mismatch, or a corrupt or truncated snapshot, raises ArgumentError with "
      "the library's message." },
    { NULL }
};

static PyType_Slot Stair_slots[] = {
    { Py_tp_doc, (void*)
      "Staircase(*, start=0.0, rule=RULE_UPDOWN, n_up=0, n_down=0, "
      "step_type=STEP_LIN, steps=None, step_down_scale=0.0, target_p=0.0, "
      "min=0.0, max=0.0, use_limits=False, harder_is_up=False, "
      "initial_rule=False, stop_reversals=0, stop_trials=0, stop_at_limit=0, "
      "est_reversals=0, est_trials=0)\n\n"
      "One ysp/stair.h staircase. Every argument is a yst_desc field and a "
      "zero means the library default, as in C. `steps` is one number or a "
      "sequence of up to MAX_STEPS numbers (the step schedule). An invalid "
      "desc raises ArgumentError with the library's message." },
    { Py_tp_new,     (void*)PyType_GenericNew },
    { Py_tp_init,    (void*)Stair_init },
    { Py_tp_dealloc, (void*)Stair_dealloc },
    { Py_tp_methods, Stair_methods },
    { Py_tp_getset,  Stair_getset },
    { 0, NULL }
};

static PyType_Spec Stair_spec = {
    .name = "ysp.stair.Staircase",
    .basicsize = sizeof(StairObject),
    .itemsize = 0,
    .flags = Py_TPFLAGS_DEFAULT,
    .slots = Stair_slots,
};

/* --- module-level functions ---------------------------------------------- */

static PyObject* mod_weighted_scale(PyObject* Py_UNUSED(m), PyObject* arg) {
    double p = PyFloat_AsDouble(arg);
    if (p == -1.0 && PyErr_Occurred()) return NULL;
    return PyFloat_FromDouble(yst_weighted_scale(p));
}

static PyObject* mod_convergence_p(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "n_up", "n_down", "scale", NULL };
    int n_up, n_down;
    double scale = 0.0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "ii|d", kw, &n_up, &n_down, &scale))
        return NULL;
    return PyFloat_FromDouble(yst_convergence_p(n_up, n_down, scale));
}

static PyObject* mod_simulate_response(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "p", "u", NULL };
    double p, u;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "dd", kw, &p, &u)) return NULL;
    return PyLong_FromLong(yst_simulate_response(p, u));
}

static PyObject* mod_strerror(PyObject* Py_UNUSED(m), PyObject* arg) {
    long c = PyLong_AsLong(arg);
    if (c == -1 && PyErr_Occurred()) return NULL;
    return PyUnicode_FromString(yst_strerror((int)c));
}

static PyMethodDef module_methods[] = {
    { "weighted_scale", mod_weighted_scale, METH_O,
      "weighted_scale(p) -> float: the step_down_scale that makes a 1-up-1-down "
      "weighted staircase converge on p, (1 - p) / p (Kaernbach 1991). NaN "
      "outside (0, 1)." },
    { "convergence_p", (PyCFunction)mod_convergence_p, METH_VARARGS | METH_KEYWORDS,
      "convergence_p(n_up, n_down, scale=0.0) -> float: the proportion correct "
      "a transformed staircase converges on. NaN when n_up > 1 and n_down > 1." },
    { "simulate_response", (PyCFunction)mod_simulate_response, METH_VARARGS | METH_KEYWORDS,
      "simulate_response(p, u) -> int: 1 when u < p, else 0. `u` is a uniform "
      "variate in [0, 1) from your own generator." },
    { "strerror", mod_strerror, METH_O,
      "strerror(code) -> str: the library's text for an ERR_* code." },
    { NULL }
};

static PyModuleDef ysp_stair_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "ysp.stair",
    .m_doc = "Adaptive staircases: transformed and weighted up/down, and "
             "accelerated stochastic approximation (ysp/stair.h).",
    .m_size = -1,
    .m_methods = module_methods,
};

static PyObject* st_add_exc(PyObject* m, const char* qualname, const char* attr,
                            PyObject* base) {
    PyObject* e = PyErr_NewException(qualname, base, NULL);
    if (!e) return NULL;
    Py_INCREF(e);
    if (PyModule_AddObject(m, attr, e) < 0) { Py_DECREF(e); Py_DECREF(e); return NULL; }
    return e;
}

typedef struct { const char* name; long value; } st_member;

/* Build an enum.IntEnum / enum.IntFlag with the functional API, add it to the
 * module as `cls_name`, and add each member again as PREFIX_NAME so C-style
 * code reads the same as the header. Returns a new reference. */
static PyObject* st_add_enum(PyObject* m, PyObject* enum_mod, const char* kind,
                             const char* cls_name, const char* prefix,
                             const st_member* mem, int n) {
    PyObject *base = NULL, *members = NULL, *args = NULL, *kwargs = NULL, *cls = NULL;
    int i;
    base = PyObject_GetAttrString(enum_mod, kind);
    if (!base) goto fail;
    members = PyList_New(n);
    if (!members) goto fail;
    for (i = 0; i < n; i++) {
        PyObject* t = Py_BuildValue("(sl)", mem[i].name, mem[i].value);
        if (!t) goto fail;
        PyList_SetItem(members, i, t);
    }
    args = Py_BuildValue("(sO)", cls_name, members);
    kwargs = Py_BuildValue("{s:s}", "module", "ysp.stair");
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

PyMODINIT_FUNC PyInit_stair(void) {
    static const st_member rule_m[] = { { "UPDOWN", YST_RULE_UPDOWN }, { "ASA", YST_RULE_ASA } };
    static const st_member step_m[] = { { "LIN", YST_STEP_LIN }, { "LOG", YST_STEP_LOG },
                                        { "DB", YST_STEP_DB } };
    static const st_member est_m[] = { { "REVERSALS", YST_EST_REVERSALS },
                                       { "TRIALS", YST_EST_TRIALS },
                                       { "MEDIAN_REV", YST_EST_MEDIAN_REV },
                                       { "LAST", YST_EST_LAST } };
    static const st_member stop_m[] = { { "NONE", YST_STOP_NONE },
                                        { "REVERSALS", YST_STOP_REVERSALS },
                                        { "TRIALS", YST_STOP_TRIALS },
                                        { "LIMIT", YST_STOP_LIMIT },
                                        { "FULL", YST_STOP_FULL } };
    static const st_member event_m[] = { { "STEP", YST_EVENT_STEP },
                                         { "REVERSAL", YST_EVENT_REVERSAL },
                                         { "DONE", YST_EVENT_DONE } };
    PyObject *m, *type, *enum_mod = NULL, *coll = NULL, *cls, *arg_bases;

    m = PyModule_Create(&ysp_stair_module);
    if (!m) return NULL;

    type = PyType_FromSpec(&Stair_spec);
    if (!type) goto fail;
    if (PyModule_AddObject(m, "Staircase", type) < 0) { Py_DECREF(type); goto fail; }

    StError = st_add_exc(m, "ysp.stair.Error", "Error", NULL);
    if (!StError) goto fail;
    arg_bases = PyTuple_Pack(2, StError, PyExc_ValueError);
    if (!arg_bases) goto fail;
    StArgumentError = st_add_exc(m, "ysp.stair.ArgumentError", "ArgumentError", arg_bases);
    Py_DECREF(arg_bases);
    if (!StArgumentError) goto fail;
    StClosed = st_add_exc(m, "ysp.stair.Closed", "Closed", StError);
    if (!StClosed) goto fail;
    StFull = st_add_exc(m, "ysp.stair.Full", "Full", StError);
    if (!StFull) goto fail;

    /* enum is the standard library, so this adds no dependency. */
    enum_mod = PyImport_ImportModule("enum");
    if (!enum_mod) goto fail;
    if (!(cls = st_add_enum(m, enum_mod, "IntEnum", "Rule", "RULE", rule_m, 2))) goto fail;
    Py_DECREF(cls);
    if (!(cls = st_add_enum(m, enum_mod, "IntEnum", "StepType", "STEP", step_m, 3))) goto fail;
    Py_DECREF(cls);
    if (!(cls = st_add_enum(m, enum_mod, "IntEnum", "Estimator", "EST", est_m, 4))) goto fail;
    Py_DECREF(cls);
    if (!(StStopEnum = st_add_enum(m, enum_mod, "IntEnum", "Stop", "STOP", stop_m, 5))) goto fail;
    if (!(StEventFlag = st_add_enum(m, enum_mod, "IntFlag", "Event", "EVENT", event_m, 3))) goto fail;
    Py_CLEAR(enum_mod);

    coll = PyImport_ImportModule("collections");
    if (!coll) goto fail;
    {
        PyObject* nt = PyObject_GetAttrString(coll, "namedtuple");
        PyObject* a = Py_BuildValue("(s[ssssss])", "Trial", "proposed", "shown",
                                    "response", "reversal", "direction", "step_index");
        PyObject* k = Py_BuildValue("{s:s}", "module", "ysp.stair");
        StTrialType = (nt && a && k) ? PyObject_Call(nt, a, k) : NULL;
        Py_XDECREF(nt); Py_XDECREF(a); Py_XDECREF(k);
    }
    Py_CLEAR(coll);
    if (!StTrialType) goto fail;
    Py_INCREF(StTrialType);
    if (PyModule_AddObject(m, "Trial", StTrialType) < 0) { Py_DECREF(StTrialType); goto fail; }

    /* From the compiled implementation, so it names the header actually built
     * in; the tests check pyproject.toml against it. */
    if (PyModule_AddStringConstant(m, "__version__", YST_VERSION_STRING ".1") < 0) goto fail;   /* .1: the CPython 3.10 floor (2026-10-27) */
    PyModule_AddIntConstant(m, "ERR_ARG", YST_ERR_ARG);
    PyModule_AddIntConstant(m, "ERR_CLOSED", YST_ERR_CLOSED);
    PyModule_AddIntConstant(m, "ERR_FULL", YST_ERR_FULL);
    PyModule_AddIntConstant(m, "MAX_TRIALS", YST_MAX_TRIALS);
    PyModule_AddIntConstant(m, "MAX_REVERSALS", YST_MAX_REVERSALS);
    PyModule_AddIntConstant(m, "MAX_STEPS", YST_MAX_STEPS);
    return m;

fail:
    Py_XDECREF(enum_mod);
    Py_XDECREF(coll);
    Py_DECREF(m);
    return NULL;
}
