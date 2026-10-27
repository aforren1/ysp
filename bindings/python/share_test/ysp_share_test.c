/* ysp_share_test.c - a consumer of the ysp.audio and ysp.timeline tables (tests only)
 *
 * What ysp-vis will be, at its smallest: audio.h and timeline.h as
 * declarations, the shims of bindings/share/ forwarding every call to the
 * owners' implementations, objects taken from Python with acquire() and
 * given back with release(). tests/test_share.py drives it. Not shipped.
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x03080000
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define YTL_MAX_CHANNELS 1024
#include "ysp/audio.h"
#include "ysp/timeline.h"
#define YSP_SHARE_PYTHON
#define YSP_AUDIO_API_SHIMS
#include "ysp_audio_api.h"
#define YSP_TIMELINE_API_SHIMS
#include "ysp_timeline_api.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* Held objects: what a movie keeps between frames. */
#define SLOTS 8
static void* g_au[SLOTS];
static void* g_tl[SLOTS];
static float g_click[2 * 480];   /* the consumer's own sound memory */
static yau_wav g_wav;            /* a soundtrack on the owner's handle */
static float g_ring[2 * 48000];

static int slot_of(PyObject* arg, void** table) {
    long i = PyLong_AsLong(arg);
    if (i == -1 && PyErr_Occurred()) return -1;
    if (i < 0 || i >= SLOTS || !table[i]) { PyErr_SetString(PyExc_ValueError, "no object held in that slot"); return -1; }
    return (int)i;
}

static PyObject* hold(void** table, const ysp_share_head* h, PyObject* obj) {
    char err[256];
    int i;
    for (i = 0; i < SLOTS && table[i]; i++) {}
    if (i == SLOTS) { PyErr_SetString(PyExc_RuntimeError, "slots full"); return NULL; }
    table[i] = h->acquire(obj, err, sizeof err);
    if (!table[i]) { PyErr_SetString(PyExc_TypeError, err); return NULL; }
    return PyLong_FromLong(i);
}

static PyObject* drop(void** table, const ysp_share_head* h, PyObject* arg) {
    int i = slot_of(arg, table);
    if (i < 0) return NULL;
    h->release(table[i]);
    table[i] = NULL;
    Py_RETURN_NONE;
}

static PyObject* hold_audio(PyObject* m, PyObject* obj) { (void)m; return hold(g_au, &ysp_audio_api_ptr->head, obj); }
static PyObject* drop_audio(PyObject* m, PyObject* arg) { (void)m; return drop(g_au, &ysp_audio_api_ptr->head, arg); }
static PyObject* hold_timeline(PyObject* m, PyObject* obj) { (void)m; return hold(g_tl, &ysp_timeline_api_ptr->head, obj); }
static PyObject* drop_timeline(PyObject* m, PyObject* arg) { (void)m; return drop(g_tl, &ysp_timeline_api_ptr->head, arg); }

/* --- audio through the shims --- */

static PyObject* onset_dict(int rc, const yau_onset* r) {
    return Py_BuildValue("{s:i,s:L,s:L,s:L,s:L,s:i,s:i,s:I}", "rc", rc, "id", (long long)r->id, "target", (long long)r->target,
                         "onset", (long long)r->onset, "start_frame", (long long)r->start_frame, "flags", (int)r->flags,
                         "tier", (int)r->tier, "buffer_id", (unsigned)r->buffer_id);
}

static PyObject* au_info(PyObject* m, PyObject* arg) {
    int i = slot_of(arg, g_au);
    yau_audio* au;
    yau_caps c;
    char line[512];
    (void)m;
    if (i < 0) return NULL;
    au = (yau_audio*)g_au[i];
    yau_get_caps(au, &c);
    yau_describe(au, line, sizeof line);
    return Py_BuildValue("{s:N,s:I,s:i,s:L,s:s,s:s}", "open", PyBool_FromLong(yau_is_open(au)), "rate", (unsigned)c.rate,
                         "channels", (int)c.channels, "lead_ns", (long long)yau_lead_ns(au), "describe", line,
                         "version", yau_version());
}

/* A 10 ms click in the consumer's memory, played on the owner's device. */
static PyObject* au_play_at(PyObject* m, PyObject* args) {
    PyObject* s;
    long long t;
    int i, k;
    yau_buf b;
    yau_id id;
    (void)m;
    if (!PyArg_ParseTuple(args, "OL", &s, &t) || (i = slot_of(s, g_au)) < 0) return NULL;
    for (k = 0; k < 480; k++) g_click[k] = k < 48 ? 0.01f : 0.0f;
    memset(&b, 0, sizeof b);
    b.frames = g_click;
    b.n = 480;
    b.channels = 1;
    b.id = 77;
    id = yau_play_at((yau_audio*)g_au[i], b, (int64_t)t);
    if (id < 0) { PyErr_SetString(PyExc_RuntimeError, yau_error((yau_audio*)g_au[i])); return NULL; }
    return PyLong_FromLongLong(id);
}

static PyObject* au_wait(PyObject* m, PyObject* args) {
    PyObject* s;
    long long id;
    int i, rc;
    yau_onset r;
    yau_audio* au;
    (void)m;
    if (!PyArg_ParseTuple(args, "OL", &s, &id) || (i = slot_of(s, g_au)) < 0) return NULL;
    au = (yau_audio*)g_au[i];
    memset(&r, 0, sizeof r);
    Py_BEGIN_ALLOW_THREADS
    rc = yau_wait(au, id, 2000000000LL, &r);
    Py_END_ALLOW_THREADS
    return onset_dict(rc, &r);
}

/* What ysp/video.h's soundtrack does: a WAV streamed on the owner's handle
 * with the consumer's ring, played at t, its sample at a time, then closed. */
static PyObject* au_soundtrack(PyObject* m, PyObject* args) {
    PyObject *s, *res;
    const char* path;
    long long t;
    int i, rc, rc2;
    yau_audio* au;
    yau_wav_desc d;
    yau_onset r;
    yau_stream_info si;
    int64_t fed, smp = -1;
    yau_id id;
    (void)m;
    if (!PyArg_ParseTuple(args, "OsL", &s, &path, &t) || (i = slot_of(s, g_au)) < 0) return NULL;
    au = (yau_audio*)g_au[i];
    memset(&d, 0, sizeof d);
    memset(&g_wav, 0, sizeof g_wav);
    d.path = path;
    d.memory = g_ring;
    d.ring = 24000;
    d.id = 5;
    if (!yau_wav_open(au, &g_wav, &d)) { PyErr_SetString(PyExc_RuntimeError, yau_wav_error(&g_wav)); return NULL; }
    fed = yau_wav_feed(&g_wav, 0);
    id = yau_play_stream(au, &g_wav.stream, (int64_t)t);
    Py_BEGIN_ALLOW_THREADS
    rc = yau_wait(au, id, 2000000000LL, &r);
    Py_END_ALLOW_THREADS
    rc2 = yau_stream_sample_at(au, &g_wav.stream, r.onset, &smp);
    yau_stream_get_info(&g_wav.stream, &si);
    Py_BEGIN_ALLOW_THREADS
    yau_wav_close(au, &g_wav);
    Py_END_ALLOW_THREADS
    res = onset_dict(rc, &r);
    if (res) {
        PyObject* v = Py_BuildValue("(LiLLN)", (long long)fed, rc2, (long long)smp, (long long)si.origin, PyBool_FromLong(si.started));
        if (!v || PyDict_SetItemString(res, "stream", v) < 0) { Py_XDECREF(v); Py_DECREF(res); return NULL; }
        Py_DECREF(v);
    }
    return res;
}

/* --- timeline through the shims --- */

static PyObject* tl_anchor(PyObject* m, PyObject* args) {
    PyObject* s;
    int i, base, rc;
    long long rt, bt;
    (void)m;
    if (!PyArg_ParseTuple(args, "OiLL", &s, &base, &rt, &bt) || (i = slot_of(s, g_tl)) < 0) return NULL;
    rc = ytl_anchor((ytl_timeline*)g_tl[i], base, rt, bt);
    return PyLong_FromLong(rc);
}

/* One frame: the number fired, the values, and the base time at the onset. */
static PyObject* tl_evaluate(PyObject* m, PyObject* args) {
    PyObject *s, *vals;
    int i, n, k, nch;
    long long onset, period, index;
    ytl_frame f;
    ytl_event fired[16];
    const float* v;
    int64_t bt = -1;
    ytl_timeline* tl;
    (void)m;
    if (!PyArg_ParseTuple(args, "OLLLi", &s, &onset, &period, &index, &nch) || (i = slot_of(s, g_tl)) < 0) return NULL;
    tl = (ytl_timeline*)g_tl[i];
    f.onset = onset;
    f.period = period;
    f.index = index;
    n = ytl_evaluate(tl, &f, fired, 16);
    if (n == YSP_SHARE_ERR_HOST) return NULL;   /* a Python callback raised: its error is set */
    v = ytl_values(tl);
    vals = PyList_New(nch);
    if (!vals) return NULL;
    for (k = 0; k < nch; k++) PyList_SetItem(vals, k, PyFloat_FromDouble(v ? v[k] : 0.0));
    (void)ytl_base_time(tl, 1, onset, &bt);
    return Py_BuildValue("(iNLd)", n, vals, (long long)bt, ytl_lead(tl));
}

/* --- the checks a consumer runs, against a doctored copy --- */

static PyObject* check_table(PyObject* m, PyObject* args) {
    const char* what;
    char err[256] = "";
    int rc;
    (void)m;
    if (!PyArg_ParseTuple(args, "s", &what)) return NULL;
    if (what[0] == 'a') {
        ysp_audio_api a = *ysp_audio_api_ptr;
        static char ver[32];
        /* An owner of another version: the header's own numbers, one moved. */
        int ma = YAU_VERSION_MAJOR, mi = YAU_VERSION_MINOR, pa = YAU_VERSION_PATCH;
        if (!strcmp(what, "a_abi")) a.head.abi += 1;
        else if (!strcmp(what, "a_size")) a.head.size -= 8;
        else if (!strcmp(what, "a_version")) { strcpy(ver, "0.0.1"); a.head.version = ver; }
        else if (!strcmp(what, "a_patch")) { snprintf(ver, sizeof ver, "%d.%d.%d", ma, mi, pa + 1); a.head.version = ver; }
        else if (!strcmp(what, "a_minor")) { snprintf(ver, sizeof ver, "%d.%d.%d", ma, mi + 1, pa); a.head.version = ver; }
        else if (!strcmp(what, "a_major")) { snprintf(ver, sizeof ver, "%d.%d.%d", ma + 1, mi, pa); a.head.version = ver; }
        else if (!strcmp(what, "a_junk")) { strcpy(ver, "seven"); a.head.version = ver; }
        else if (!strcmp(what, "a_sizeof")) a.sizeof_yau_wav += 8;
        else if (!strcmp(what, "a_voices")) a.max_voices += 1;
        else if (!strcmp(what, "a_host")) a.head.host = YSP_SHARE_HOST_MEX;
        rc = ysp_audio_api_check(&a, YSP_SHARE_HOST_PYTHON, err, sizeof err);
    } else {
        ysp_timeline_api a = *ysp_timeline_api_ptr;
        static char ver[32];
        int ma = YTL_VERSION_MAJOR, mi = YTL_VERSION_MINOR, pa = YTL_VERSION_PATCH;
        if (!strcmp(what, "t_channels")) a.max_channels = 256;
        else if (!strcmp(what, "t_sizeof")) a.sizeof_ytl_timeline -= 64;
        else if (!strcmp(what, "t_patch")) { snprintf(ver, sizeof ver, "%d.%d.%d", ma, mi, pa + 7); a.head.version = ver; }
        else if (!strcmp(what, "t_minor")) { snprintf(ver, sizeof ver, "%d.%d.%d", ma, mi > 0 ? mi - 1 : mi + 1, pa); a.head.version = ver; }
        rc = ysp_timeline_api_check(&a, YSP_SHARE_HOST_PYTHON, err, sizeof err);
    }
    return Py_BuildValue("(is)", rc, err);
}

/* --- cost of a forwarded call ---
 * ns per call of ytl_value(tl, 0) through the shim (shim -> table ->
 * owner), through the table pointer alone, and of the inline read it
 * replaces; n calls in a C loop, GIL held, best of `reps`. */
static double now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
#endif
}

static volatile float g_sink;

static PyObject* bench(PyObject* m, PyObject* args) {
    PyObject* s;
    int i, r, reps;
    long long n, k;
    double best[3] = { 1e300, 1e300, 1e300 }, t0, dt;
    ytl_timeline* tl;
    float (*fn)(const ytl_timeline*, int);
    (void)m;
    if (!PyArg_ParseTuple(args, "OLi", &s, &n, &reps) || (i = slot_of(s, g_tl)) < 0) return NULL;
    tl = (ytl_timeline*)g_tl[i];
    fn = ysp_timeline_api_ptr->ytl_value;
    for (r = 0; r < reps; r++) {
        float acc = 0.0f;
        t0 = now_ns();
        for (k = 0; k < n; k++) acc += ytl_value(tl, (int)(k & 1));
        g_sink = acc;
        dt = (now_ns() - t0) / (double)n;
        if (dt < best[0]) best[0] = dt;
        acc = 0.0f;
        t0 = now_ns();
        for (k = 0; k < n; k++) acc += fn(tl, (int)(k & 1));
        g_sink = acc;
        dt = (now_ns() - t0) / (double)n;
        if (dt < best[1]) best[1] = dt;
        acc = 0.0f;
        t0 = now_ns();
        for (k = 0; k < n; k++) acc += ((volatile float*)tl->values)[k & 1];
        g_sink = acc;
        dt = (now_ns() - t0) / (double)n;
        if (dt < best[2]) best[2] = dt;
    }
    return Py_BuildValue("{s:d,s:d,s:d}", "shim", best[0], "table", best[1], "inline", best[2]);
}

static PyMethodDef methods[] = {
    { "hold_audio", hold_audio, METH_O, NULL },
    { "drop_audio", drop_audio, METH_O, NULL },
    { "hold_timeline", hold_timeline, METH_O, NULL },
    { "drop_timeline", drop_timeline, METH_O, NULL },
    { "au_info", au_info, METH_O, NULL },
    { "au_play_at", au_play_at, METH_VARARGS, NULL },
    { "au_wait", au_wait, METH_VARARGS, NULL },
    { "au_soundtrack", au_soundtrack, METH_VARARGS, NULL },
    { "tl_anchor", tl_anchor, METH_VARARGS, NULL },
    { "tl_evaluate", tl_evaluate, METH_VARARGS, NULL },
    { "check_table", check_table, METH_VARARGS, NULL },
    { "bench", bench, METH_VARARGS, NULL },
    { NULL, NULL, 0, NULL }
};

static struct PyModuleDef module = { PyModuleDef_HEAD_INIT, "ysp_share_test", NULL, -1, methods, NULL, NULL, NULL, NULL };

PyMODINIT_FUNC PyInit_ysp_share_test(void) {
    if (ysp_audio_api_import_py() < 0 || ysp_timeline_api_import_py() < 0) return NULL;
    return PyModule_Create(&module);
}
