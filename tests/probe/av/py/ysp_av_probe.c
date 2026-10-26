/* ysp_av_probe.c - the ysp-av probe as a CPython extension (Limited API,
 * abi3, cp38+). Not the ysp-av API: one call per step of a frame, so that
 * the interpreter's cost per frame shows (docs/av_probe.md).
 *
 * Times are seconds as doubles; the core keeps int64 ns. begin() and
 * flip_at() release the GIL, because both wait for the display.
 */
#define Py_LIMITED_API 0x03080000
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <math.h>
#include "../probe_core.h"

#define S(ns) ((double)(ns) * 1e-9)

static PyObject* need_open(void) {
    PyErr_SetString(PyExc_RuntimeError, "ysp_av_probe: not open");
    return NULL;
}

static PyObject* m_open(PyObject* self, PyObject* args, PyObject* kw) {
    static char* names[] = { "windowed", "display", "w", "h", "angle_dir", "panic", "foreground", "backend", NULL };
    pv_opts o;
    const char* angle = NULL;
    int ok;
    static char buf[4096];
    (void)self;
    memset(&o, 0, sizeof o);
    o.w = 1280; o.h = 720; o.foreground = YSCR_FOREGROUND_FORCE;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|piiizpii", names, &o.windowed, &o.display, &o.w, &o.h, &angle,
                                     &o.panic, &o.foreground, &o.backend)) return NULL;
    o.angle_dir = angle;
    Py_BEGIN_ALLOW_THREADS
    ok = pv_open(&o);
    Py_END_ALLOW_THREADS
    if (!ok) { PyErr_SetString(PyExc_RuntimeError, pv_last_error()); return NULL; }
    pv_describe(buf, sizeof buf);
    return PyUnicode_FromString(buf);
}

static PyObject* m_setup(PyObject* self, PyObject* args) {
    int n;
    (void)self;
    if (!PyArg_ParseTuple(args, "i", &n)) return NULL;
    if (!pv) return need_open();
    if (pv_setup(n) < 0) { PyErr_SetString(PyExc_RuntimeError, ygfx_error(&pv->gfx)); return NULL; }
    Py_RETURN_NONE;
}

static PyObject* m_elevate(PyObject* self, PyObject* noargs) {
    (void)self; (void)noargs;
    return PyLong_FromLong((long)yrt_thread_elevate(NULL));
}

/* (index, onset, period, [records]) or None after an abort. A record is
 * (index, target, onset, residual, dropped, path, tier, flags). */
static PyObject* m_begin(PyObject* self, PyObject* noargs) {
    int rc, i;
    PyObject *list, *out;
    (void)self; (void)noargs;
    if (!pv || !pv->open) return need_open();
    Py_BEGIN_ALLOW_THREADS
    rc = pv_begin();
    Py_END_ALLOW_THREADS
    if (rc == YSCR_QUIT) Py_RETURN_NONE;
    if (rc != YSCR_OK) { PyErr_SetString(PyExc_RuntimeError, yscr_error(&pv->scr)); return NULL; }
    list = PyList_New(pv->f.n_done);
    if (!list) return NULL;
    for (i = 0; i < pv->f.n_done; i++) {
        const yscr_record* r = &pv->f.done[i];
        PyObject* t = Py_BuildValue("(LdddIiiI)", (long long)r->index, S(r->target), S(r->onset), S(r->residual),
                                    (unsigned)r->dropped, (int)r->path, (int)r->tier, (unsigned)r->flags);
        if (!t) { Py_DECREF(list); return NULL; }
        PyList_SetItem(list, i, t);
    }
    out = Py_BuildValue("(LddN)", (long long)pv->f.index, S(pv->f.onset), S(pv->f.period), list);
    return out;
}

static PyObject* m_abort_info(PyObject* self, PyObject* noargs) {
    (void)self; (void)noargs;
    if (!pv) return need_open();
    return Py_BuildValue("(Ii)", (unsigned)pv->f.abort, (int)pv->f.abort_presses);
}

static PyObject* m_evaluate(PyObject* self, PyObject* noargs) {
    (void)self; (void)noargs;
    pv_evaluate();
    Py_RETURN_NONE;
}

static PyObject* m_gfx_begin(PyObject* self, PyObject* noargs) {
    (void)self; (void)noargs;
    pv_gfx_begin();
    Py_RETURN_NONE;
}

static PyObject* m_draw(PyObject* self, PyObject* arg) {
    long i = PyLong_AsLong(arg);
    (void)self;
    if (i == -1 && PyErr_Occurred()) return NULL;
    if (pv_draw((int)i) != YGFX_OK) { PyErr_SetString(PyExc_RuntimeError, ygfx_error(&pv->gfx)); return NULL; }
    Py_RETURN_NONE;
}

static PyObject* m_set(PyObject* self, PyObject* args) {
    int i, p;
    float v;
    (void)self;
    if (!PyArg_ParseTuple(args, "iif", &i, &p, &v)) return NULL;
    pv_set(i, p, v);
    Py_RETURN_NONE;
}

static PyObject* m_gfx_end(PyObject* self, PyObject* noargs) {
    (void)self; (void)noargs;
    pv_gfx_end();
    Py_RETURN_NONE;
}

static PyObject* m_flip_at(PyObject* self, PyObject* arg) {
    double t = PyFloat_AsDouble(arg);
    int rc;
    (void)self;
    if (t == -1.0 && PyErr_Occurred()) return NULL;
    if (!(t >= 0)) { PyErr_SetString(PyExc_ValueError, "flip_at: t must be >= 0"); return NULL; }
    Py_BEGIN_ALLOW_THREADS
    rc = pv_flip_at((int64_t)llround(t * 1e9));
    Py_END_ALLOW_THREADS
    if (rc < 0) { PyErr_SetString(PyExc_RuntimeError, yscr_error(&pv->scr)); return NULL; }
    Py_RETURN_NONE;
}

static PyObject* m_frame_c(PyObject* self, PyObject* noargs) {
    int rc;
    (void)self; (void)noargs;
    Py_BEGIN_ALLOW_THREADS
    rc = pv_frame_c(1);
    Py_END_ALLOW_THREADS
    if (rc == YSCR_QUIT) Py_RETURN_FALSE;
    if (rc < 0) { PyErr_SetString(PyExc_RuntimeError, yscr_error(&pv->scr)); return NULL; }
    Py_RETURN_TRUE;
}

static PyObject* m_close(PyObject* self, PyObject* noargs) {
    (void)self; (void)noargs;
    Py_BEGIN_ALLOW_THREADS
    pv_close();
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

static PyObject* m_free(PyObject* self, PyObject* noargs) {
    (void)self; (void)noargs;
    pv_close();
    pv_free();
    Py_RETURN_NONE;
}

static PyObject* m_dump(PyObject* self, PyObject* args) {
    const char *path, *tag = "";
    (void)self;
    if (!PyArg_ParseTuple(args, "s|s", &path, &tag)) return NULL;
    if (!pv) return need_open();
    return PyLong_FromLong(pv_dump(path, tag));
}

static PyObject* m_counts(PyObject* self, PyObject* noargs) {
    (void)self; (void)noargs;
    if (!pv) return need_open();
    return Py_BuildValue("(LLL)", (long long)pv->n_frames, (long long)pv->n_recs, (long long)pv->lost);
}

static PyObject* m_describe(PyObject* self, PyObject* noargs) {
    static char buf[4096];
    (void)self; (void)noargs;
    if (!pv || !pv->open) return need_open();
    pv_describe(buf, sizeof buf);
    return PyUnicode_FromString(buf);
}

static PyObject* m_modules(PyObject* self, PyObject* args) {
    static char buf[65536];
    int all = 0;
    (void)self;
    if (!PyArg_ParseTuple(args, "|p", &all)) return NULL;
    pv_modules(buf, sizeof buf, all);
    return PyUnicode_FromString(buf);
}

static PyObject* m_preload(PyObject* self, PyObject* args) {
    const char* p;
    (void)self;
    if (!PyArg_ParseTuple(args, "s", &p)) return NULL;
    return PyBool_FromLong(pv_preload(p) != NULL);
}

static PyObject* m_displays(PyObject* self, PyObject* noargs) {
    static char buf[2048];
    (void)self; (void)noargs;
    pv_displays(buf, sizeof buf);
    return PyUnicode_FromString(buf);
}

static PyObject* m_watch_start(PyObject* self, PyObject* noargs) {
    (void)self; (void)noargs;
    if (!pv || !pv->open) return need_open();
    return PyLong_FromLong(pv_watch_start());
}

static PyObject* m_watch_stop(PyObject* self, PyObject* noargs) {
    static char buf[16384];
    (void)self; (void)noargs;
    if (!pv) return need_open();
    Py_BEGIN_ALLOW_THREADS
    pv_watch_stop(buf, sizeof buf);
    Py_END_ALLOW_THREADS
    return PyUnicode_FromString(buf);
}

static PyObject* m_keys(PyObject* self, PyObject* args) {
    int n, delay, gap;
    (void)self;
    if (!PyArg_ParseTuple(args, "iii", &n, &delay, &gap)) return NULL;
    if (!pv || !pv->open) return need_open();
    return PyLong_FromLong(pv_keys(n, delay, gap));
}

static PyObject* m_keys_status(PyObject* self, PyObject* noargs) {
    (void)self; (void)noargs;
    if (!pv) return need_open();
    return Py_BuildValue("(ll)", (long)pv->keys_sent, (long)pv->keys_skipped);
}

static PyObject* m_audio_open(PyObject* self, PyObject* args) {
    static char buf[1024];
    int null_dev = 1, rc;
    (void)self;
    if (!PyArg_ParseTuple(args, "|p", &null_dev)) return NULL;
    if (!pv || !pv->open) return need_open();
    Py_BEGIN_ALLOW_THREADS
    rc = pv_audio_open(null_dev, buf, sizeof buf);
    Py_END_ALLOW_THREADS
    if (rc < 0) { PyErr_SetString(PyExc_RuntimeError, buf); return NULL; }
    return PyUnicode_FromString(buf);
}

static PyObject* m_now(PyObject* self, PyObject* noargs) {
    (void)self; (void)noargs;
    return PyFloat_FromDouble(S(yrt_now_ns()));
}

static PyMethodDef methods[] = {
    { "open", (PyCFunction)(void (*)(void))m_open, METH_VARARGS | METH_KEYWORDS, "open the screen and gfx; returns the describe lines" },
    { "setup", m_setup, METH_VARARGS, "setup(n): the probe's scene of n stimuli" },
    { "elevate", m_elevate, METH_NOARGS, "yrt_thread_elevate() on this thread" },
    { "begin", m_begin, METH_NOARGS, "(index, onset, period, records) or None after an abort" },
    { "abort_info", m_abort_info, METH_NOARGS, "(abort flags, presses) of the last begin()" },
    { "evaluate", m_evaluate, METH_NOARGS, "timeline evaluate and apply the bindings" },
    { "gfx_begin", m_gfx_begin, METH_NOARGS, "" },
    { "draw", m_draw, METH_O, "draw(i)" },
    { "set", m_set, METH_VARARGS, "set(i, param, value)" },
    { "gfx_end", m_gfx_end, METH_NOARGS, "" },
    { "flip_at", m_flip_at, METH_O, "flip_at(t seconds)" },
    { "frame_c", m_frame_c, METH_NOARGS, "one whole frame in C" },
    { "close", m_close, METH_NOARGS, "close the screen; the log stays for dump()" },
    { "free", m_free, METH_NOARGS, "close and free the log" },
    { "dump", m_dump, METH_VARARGS, "dump(path, tag): the per-frame CSV" },
    { "counts", m_counts, METH_NOARGS, "(frames, records, lost)" },
    { "describe", m_describe, METH_NOARGS, "" },
    { "modules", m_modules, METH_VARARGS, "loaded modules of interest with full paths" },
    { "preload", m_preload, METH_VARARGS, "LoadLibraryW(path)" },
    { "displays", m_displays, METH_NOARGS, "" },
    { "watch_start", m_watch_start, METH_NOARGS, "" },
    { "watch_stop", m_watch_stop, METH_NOARGS, "" },
    { "keys", m_keys, METH_VARARGS, "keys(presses, delay_ms, gap_ms): Shift+Esc from a thread" },
    { "keys_status", m_keys_status, METH_NOARGS, "(sent, skipped)" },
    { "audio_open", m_audio_open, METH_VARARGS, "audio_open(null=True)" },
    { "now", m_now, METH_NOARGS, "the ysp_rt clock, seconds" },
    { NULL, NULL, 0, NULL }
};

static struct PyModuleDef moddef = { PyModuleDef_HEAD_INIT, "ysp_av_probe", NULL, -1, methods, NULL, NULL, NULL, NULL };

PyMODINIT_FUNC PyInit_ysp_av_probe(void) {
    PyObject* m = PyModule_Create(&moddef);
    if (!m) return NULL;
    PyModule_AddIntConstant(m, "P_ORI", YGFX_P_ORI);
    PyModule_AddIntConstant(m, "P_PHASE", YGFX_P_PHASE);
    PyModule_AddIntConstant(m, "P_CONTRAST", YGFX_P_CONTRAST);
    PyModule_AddIntConstant(m, "FLIP_LATE_TARGET", YSCR_FLIP_LATE_TARGET);
    return m;
}
