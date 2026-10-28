/* ysp_device_ext.c - CPython extension wrapping ysp/device.h (module ysp.device)
 *
 *     import ysp.device as yd
 *     box = yd.Device("resp", "line", "serial:16C0:0483::")
 *     if not box.wait_running(3.0): raise SystemExit(box.error)
 *     for e in box.wait(1.0):              # Events, t in seconds on now()
 *         print(e.t, e.control, e.type == yd.PRESS)
 *     trig = yd.Device("trig", "triggerbox", "serial:0403:6001:TB0123:")
 *     trig.wait_running(3.0)
 *     trig.pulse(12, 0.002)                # 12 for 2 ms, then 0
 *     box.close(); trig.close()
 *
 * UNITS: every time is float seconds, as in every ysp binding; the C API
 * takes int64 ns, and the conversion happens here, rounded to the nearest
 * ns. An event's t_ns is the exact C value beside t.
 *
 * THREADS: each Device has the header's reader thread (and, for a
 * host-timed output, its trailing-edge worker). They never take the GIL:
 * the sink is a C ring (ysp_device_bind.h) that Device.poll() and
 * Device.wait() empty. wait(), wait_running(), close() and the output calls
 * release the GIL.
 *
 * LIFETIME: a call that uses the handle's locks counts itself in `users`;
 * close() waits for those calls to leave before ydev_close() frees the
 * locks, and a call after close() sees a closed handle, whose state, stats
 * and error the header keeps readable. A Rule holds its output Device, and
 * a Device holds its Rule and Sim, so the garbage collector closes them in
 * the order the header needs (box, rule, output).
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x030A0000   /* the CPython 3.10+ stable ABI */
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

/* ysp/parallel.h first: ysp/device.h then builds its parallel-port
 * transport. ysp/parallel.h #errors on macOS. */
#if !defined(__APPLE__)
#define YDB_HAVE_PARALLEL 1
#define YSP_PARALLEL_IMPLEMENTATION
#include "ysp/parallel.h"
#endif
#define YSP_DEVICE_IMPLEMENTATION
#include "ysp/device.h"
#include "ysp_device_bind.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#define DV_VERSION YDEV_VERSION_STRING ".1"   /* .1: the CPython 3.10 floor (2026-10-27) */
#define DV_QUEUE_DEFAULT 1024
#define DV_QUEUE_MAX (1u << 20)

static PyObject* DvError;
static PyObject* DvClosed;
static PyTypeObject* DvEventType;
static PyObject* DvDeviceType;
static PyObject* DvRuleType;
static PyObject* DvSimType;

/* The process's role table: a role keeps its index (yin_event.device)
 * across a close and an open. ydev_open() and ydev_close() write it, so
 * both run under this lock. */
static ydev_roles g_roles;
static ybd_mutex  g_roles_lock;

/* ---- seconds ---------------------------------------------------------------- */

#define DV_S_INF 1   /* math.inf is allowed; *out = INT64_MAX */
#define DV_S_POS 2   /* a positive value never rounds to 0 */

/* Python seconds to int64 ns, rounded to the nearest ns: the whole seconds
 * convert exactly and only the fraction is scaled. Negative and NaN are
 * refused before the C call. */
static int dv_seconds(PyObject* o, const char* what, int flags, int64_t* out) {
    double s = PyFloat_AsDouble(o);
    double whole;
    int64_t ns;
    if (s == -1.0 && PyErr_Occurred()) return -1;
    if (!(s >= 0)) {
        PyErr_Format(PyExc_ValueError, "%s: seconds, >= 0 (not NaN)", what);
        return -1;
    }
    if (s == HUGE_VAL) {
        if (flags & DV_S_INF) { *out = INT64_MAX; return 0; }
        PyErr_Format(PyExc_ValueError, "%s: seconds, finite", what);
        return -1;
    }
    if (s >= 9223372036.0) {
        PyErr_Format(PyExc_OverflowError, "%s: seconds, < 9223372036 (int64 ns)", what);
        return -1;
    }
    whole = floor(s);
    ns = (int64_t)whole * 1000000000 + (int64_t)llround((s - whole) * 1e9);
    if (ns == 0 && s > 0 && (flags & DV_S_POS)) ns = 1;
    *out = ns;
    return 0;
}

static PyObject* dv_s(int64_t ns) {
    if (ns == INT64_MAX) return PyFloat_FromDouble(HUGE_VAL);
    return PyFloat_FromDouble((double)ns / 1e9);
}

/* An error with (message, code) as its args. */
static PyObject* dv_fail(PyObject* exc, const char* what, int code, const char* msg) {
    PyObject* v = Py_BuildValue("(si)", msg && msg[0] ? msg : ydev_strerror(code), code);
    (void)what;
    if (v) { PyErr_SetObject(exc, v); Py_DECREF(v); }
    return NULL;
}

static PyObject* dv_out_fail(const char* what, int code) {
    char msg[160];
    snprintf(msg, sizeof msg, "ysp_device: %s: %s", what, ydev_strerror(code));
    return dv_fail(DvError, what, code, msg);
}

/* A UTF-8 copy of a str into buf; -1 with an exception. */
static int dv_str(PyObject* o, const char* what, char* buf, size_t cap) {
    PyObject* b;
    const char* s;
    Py_ssize_t n;
    if (!PyUnicode_Check(o)) { PyErr_Format(PyExc_TypeError, "%s: a str", what); return -1; }
    b = PyUnicode_AsUTF8String(o);
    if (!b) return -1;
    s = PyBytes_AsString(b);
    n = PyBytes_Size(b);
    if (!s || n < 0) { Py_DECREF(b); return -1; }
    if ((size_t)n >= cap || strlen(s) != (size_t)n) {
        Py_DECREF(b);
        PyErr_Format(PyExc_ValueError, "%s: at most %d bytes, no NUL", what, (int)cap - 1);
        return -1;
    }
    memcpy(buf, s, (size_t)n + 1);
    Py_DECREF(b);
    return 0;
}

/* A family: a YBOX_ number or its name ("xid", "line", ...). */
static int dv_family(PyObject* o, int* out) {
    int f;
    if (PyLong_Check(o)) {
        long v = PyLong_AsLong(o);
        if (v == -1 && PyErr_Occurred()) return -1;
        if (v < YBOX_XID || v > YBOX_FAMILY_LAST) {
            PyErr_Format(PyExc_ValueError, "family: %ld is not a ysp/box.h family", v);
            return -1;
        }
        *out = (int)v;
        return 0;
    }
    {
        char name[32];
        if (dv_str(o, "family", name, sizeof name) < 0) return -1;
        for (f = YBOX_XID; f <= YBOX_FAMILY_LAST; f++)
            if (strcmp(name, ybox_family_name(f)) == 0) { *out = f; return 0; }
        PyErr_Format(PyExc_ValueError, "family: unknown name '%s' (xid, line, photo, triggerbox, biosemi, mmbts, lines, parallel)", name);
        return -1;
    }
}

/* ---- the Event --------------------------------------------------------------- */

static PyStructSequence_Field dv_event_fields[] = {
    { "t",       "the event's time, seconds on the ysp_rt clock (now())" },
    { "t_ns",    "the same time, exact int ns" },
    { "device",  "the instance: the role's index (yin_event.device)" },
    { "control", "button, channel or scancode" },
    { "code",    "keyboard: SDL keycode; else the device's" },
    { "mods",    "keyboard: SDL_Keymod" },
    { "kind",    "KIND_*" },
    { "type",    "PRESS, RELEASE, SAMPLE, PROXIMITY" },
    { "flags",   "REPEAT, SYNTHETIC, ERASER, UNLISTED, DEVTICKS" },
    { "stamp",   "TIER_*; 0: the source's" },
    { "unc",     "this event's uncertainty, seconds (unc_us); 0: the source's" },
    { "x",       "position or axis" },
    { "y",       "position or axis" },
    { "value",   "level, sample value" },
    { "aux",     "extra value" },
    { "ticks",   "the device clock, low 32 bits (with DEVTICKS)" },
    { NULL, NULL }
};

static PyStructSequence_Desc dv_event_desc = {
    "ysp.device.Event",
    "One input event (ysp/input.h yin_event). t is seconds on the ysp_rt clock; "
    "t_ns the exact int ns.",
    dv_event_fields,
    16
};

static PyObject* dv_event(const yin_event* e) {
    PyObject* o = PyStructSequence_New(DvEventType);
    PyObject* v[16];
    int i;
    if (!o) return NULL;
    v[0] = PyFloat_FromDouble((double)e->t / 1e9);
    v[1] = PyLong_FromLongLong((long long)e->t);
    v[2] = PyLong_FromUnsignedLong((unsigned long)e->device);
    v[3] = PyLong_FromUnsignedLong((unsigned long)e->control);
    v[4] = PyLong_FromUnsignedLong((unsigned long)e->code);
    v[5] = PyLong_FromLong((long)e->mods);
    v[6] = PyLong_FromLong((long)e->kind);
    v[7] = PyLong_FromLong((long)e->type);
    v[8] = PyLong_FromLong((long)e->flags);
    v[9] = PyLong_FromLong((long)e->stamp);
    v[10] = PyFloat_FromDouble((double)e->unc_us * 1e-6);
    v[11] = PyFloat_FromDouble((double)e->x);
    v[12] = PyFloat_FromDouble((double)e->y);
    v[13] = PyFloat_FromDouble((double)e->value);
    v[14] = PyFloat_FromDouble((double)e->aux);
    v[15] = PyLong_FromUnsignedLong((unsigned long)e->ticks);
    for (i = 0; i < 16; i++) {
        if (!v[i]) {
            int k;
            for (k = 0; k < 16; k++) Py_XDECREF(v[k]);
            Py_DECREF(o);
            return NULL;
        }
    }
    for (i = 0; i < 16; i++) PyStructSequence_SetItem(o, i, v[i]);   /* steals */
    return o;
}

/* ---- Sim --------------------------------------------------------------------- */

typedef struct {
    PyObject_HEAD
    ybd_sim* sim;
} SimObject;

static int Sim_init(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "family", "ppm", "frame", "tail", "key", "identify", NULL };
    SimObject* so = (SimObject*)self;
    PyObject *fam_o, *frame_o = NULL, *tail_o = NULL;
    double ppm = 20.0;
    const char* key = NULL;
    int identify = 1, family;
    int64_t frame_ns = 125000, tail_ns = 50000;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|dOOzp", kw, &fam_o, &ppm, &frame_o, &tail_o, &key, &identify))
        return -1;
    if (dv_family(fam_o, &family) < 0) return -1;
    if (family == YBOX_PARALLEL) { PyErr_SetString(PyExc_ValueError, "Sim: no simulated parallel port"); return -1; }
    if (!(ppm > -1000.0 && ppm < 1000.0)) { PyErr_SetString(PyExc_ValueError, "ppm: -1000 to 1000"); return -1; }
    if (frame_o && dv_seconds(frame_o, "frame", 0, &frame_ns) < 0) return -1;
    if (tail_o && dv_seconds(tail_o, "tail", 0, &tail_ns) < 0) return -1;
    if (key && strlen(key) >= sizeof so->sim->key) { PyErr_SetString(PyExc_ValueError, "key: at most 95 bytes"); return -1; }
    if (so->sim) { PyErr_SetString(DvError, "Sim: already initialized"); return -1; }
    so->sim = (ybd_sim*)calloc(1, sizeof(ybd_sim));
    if (!so->sim) { PyErr_NoMemory(); return -1; }
    ybd_sim_init(so->sim, family, ppm, (double)frame_ns, (double)tail_ns, key);
    so->sim->no_identity = !identify;
    return 0;
}

static void Sim_dealloc(PyObject* self) {
    SimObject* so = (SimObject*)self;
    PyTypeObject* tp = Py_TYPE(self);
    freefunc f = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    if (so->sim) { ybd_sim_free(so->sim); free(so->sim); so->sim = NULL; }
    f(self);
    Py_DECREF(tp);
}

static ybd_sim* dv_sim(PyObject* self) {
    SimObject* so = (SimObject*)self;
    if (!so->sim) PyErr_SetString(DvError, "Sim: not initialized");
    return so->sim;
}

static PyObject* Sim_edge(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "t", "level", "channel", NULL };
    PyObject* t_o;
    int level, ch = 1;
    int64_t t;
    ybd_sim* s = dv_sim(self);
    if (!s || !PyArg_ParseTupleAndKeywords(args, kwds, "Op|i", kw, &t_o, &level, &ch)) return NULL;
    if (dv_seconds(t_o, "t", 0, &t) < 0) return NULL;
    if (ch < 1 || ch > (s->family == YBOX_XID ? 8 : 255)) {
        PyErr_SetString(PyExc_ValueError, "channel: 1 to 255 (XID: 1 to 8)");
        return NULL;
    }
    if (ybd_sim_add_edge(s, t, ch, level) < 0) { PyErr_SetString(DvError, "Sim: the edge list is full"); return NULL; }
    Py_RETURN_NONE;
}

static PyObject* Sim_press(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "t", "duration", "channel", NULL };
    PyObject *t_o, *d_o;
    int ch = 1;
    int64_t t, d;
    ybd_sim* s = dv_sim(self);
    if (!s || !PyArg_ParseTupleAndKeywords(args, kwds, "OO|i", kw, &t_o, &d_o, &ch)) return NULL;
    if (dv_seconds(t_o, "t", 0, &t) < 0 || dv_seconds(d_o, "duration", DV_S_POS, &d) < 0) return NULL;
    if (ch < 1 || ch > (s->family == YBOX_XID ? 8 : 255)) {
        PyErr_SetString(PyExc_ValueError, "channel: 1 to 255 (XID: 1 to 8)");
        return NULL;
    }
    if (ybd_sim_add_edge(s, t, ch, 1) < 0 || ybd_sim_add_edge(s, t + d, ch, 0) < 0) {
        PyErr_SetString(DvError, "Sim: the edge list is full");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject* Sim_unplug(PyObject* self, PyObject* args) {
    PyObject *a_o, *b_o;
    int64_t a, b;
    ybd_sim* s = dv_sim(self);
    if (!s || !PyArg_ParseTuple(args, "OO", &a_o, &b_o)) return NULL;
    if (dv_seconds(a_o, "start", 0, &a) < 0 || dv_seconds(b_o, "stop", DV_S_INF, &b) < 0) return NULL;
    ybd_sim_unplug(s, a, b);
    Py_RETURN_NONE;
}

static PyObject* Sim_ticks(PyObject* self, PyObject* arg) {
    int64_t t;
    ybd_sim* s = dv_sim(self);
    if (!s || dv_seconds(arg, "t", 0, &t) < 0) return NULL;
    return PyLong_FromUnsignedLongLong((unsigned long long)ybd_sim_ticks_at(s, t));
}

/* writes(): the host's byte writes; lines(): its line changes. Copied
 * under the lock, then built. */
static PyObject* dv_sim_log(PyObject* self, int lines) {
    ybd_sim* s = dv_sim(self);
    ybd_sim_wrec* w;
    int n, i, k = 0;
    PyObject* out;
    if (!s) return NULL;
    w = (ybd_sim_wrec*)malloc(sizeof(ybd_sim_wrec) * YBD_SIM_WRITES);
    if (!w) return PyErr_NoMemory();
    ybd_lock(&s->lock);
    n = s->n_writes;
    memcpy(w, s->writes, sizeof(ybd_sim_wrec) * (size_t)n);
    ybd_unlock(&s->lock);
    out = PyList_New(0);
    for (i = 0; out && i < n; i++) {
        PyObject* item;
        if ((w[i].n == 0) != (lines != 0)) continue;
        item = lines ? Py_BuildValue("(dII)", (double)w[i].t / 1e9, (unsigned)w[i].code, (unsigned)w[i].changed)
                     : Py_BuildValue("(dN)", (double)w[i].t / 1e9,
                                     PyBytes_FromStringAndSize((const char*)w[i].b,
                                                               w[i].n < YBD_SIM_WBYTES ? w[i].n : YBD_SIM_WBYTES));
        if (!item || PyList_Append(out, item) < 0) { Py_XDECREF(item); Py_CLEAR(out); break; }
        Py_DECREF(item);
        k++;
    }
    free(w);
    return out;
}

static PyObject* Sim_writes(PyObject* self, PyObject* Py_UNUSED(a)) { return dv_sim_log(self, 0); }
static PyObject* Sim_lines(PyObject* self, PyObject* Py_UNUSED(a)) { return dv_sim_log(self, 1); }

static PyObject* Sim_get_key(PyObject* self, void* c) {
    ybd_sim* s = dv_sim(self);
    (void)c;
    return s ? PyUnicode_FromString(s->key) : NULL;
}
static PyObject* Sim_get_family(PyObject* self, void* c) {
    ybd_sim* s = dv_sim(self);
    (void)c;
    return s ? PyLong_FromLong(s->family) : NULL;
}
static PyObject* Sim_get_opens(PyObject* self, void* c) {
    ybd_sim* s = dv_sim(self);
    int n;
    (void)c;
    if (!s) return NULL;
    ybd_lock(&s->lock);
    n = s->opens;
    ybd_unlock(&s->lock);
    return PyLong_FromLong(n);
}

static PyGetSetDef Sim_getset[] = {
    { "key", Sim_get_key, NULL, "The key the simulated port lists as; a Device key must match it.", NULL },
    { "family", Sim_get_family, NULL, "The board's family (XID, LINE, ...).", NULL },
    { "opens", Sim_get_opens, NULL, "How many times a Device opened the board.", NULL },
    { NULL }
};

static PyMethodDef Sim_methods[] = {
    { "edge", (PyCFunction)(void (*)(void))Sim_edge, METH_VARARGS | METH_KEYWORDS,
      "edge(t, level, channel=1): the board's input changes at t (seconds on now()): "
      "level True on (pressed, light), False off. XID keys 1 to 8." },
    { "press", (PyCFunction)(void (*)(void))Sim_press, METH_VARARGS | METH_KEYWORDS,
      "press(t, duration, channel=1): an edge on at t and off at t + duration." },
    { "unplug", Sim_unplug, METH_VARARGS,
      "unplug(start, stop): the board is gone from start to stop (seconds on now(); "
      "stop math.inf: for good)." },
    { "ticks", Sim_ticks, METH_O, "ticks(t) -> int: the board's clock at t, unwrapped (the truth)." },
    { "writes", Sim_writes, METH_NOARGS,
      "writes() -> list of (t, data): each byte write of the host, its time and its first 16 bytes "
      "(the first 4096 writes)." },
    { "lines", Sim_lines, METH_NOARGS, "lines() -> list of (t, code, changed): each modem-line change (LINES)." },
    { NULL }
};

static PyType_Slot Sim_slots[] = {
    { Py_tp_doc, (void*)"Sim(family, ppm=20.0, frame=125e-6, tail=50e-6, key=None, identify=True): "
                        "a simulated board for Device(sim=...), on the real clock: a line board, an XID box, "
                        "a photodiode board, or an output-only box. Its clock runs ppm fast; its bytes reach "
                        "the host after a uniform delay over frame seconds plus an exponential tail. For tests "
                        "and examples without hardware." },
    { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_init, (void*)Sim_init },
    { Py_tp_dealloc, (void*)Sim_dealloc },
    { Py_tp_methods, Sim_methods },
    { Py_tp_getset, Sim_getset },
    { 0, NULL }
};

static PyType_Spec Sim_spec = { "ysp.device.Sim", sizeof(SimObject), 0, Py_TPFLAGS_DEFAULT, Sim_slots };

/* ---- Device ------------------------------------------------------------------ */

typedef struct {
    PyObject_HEAD
    ydev_device* dev;       /* calloc'd: about 45 KB                         */
    ybd_queue    q;
    ybd_mutex    lk;        /* opened, closing, users, rule_refs             */
    ybd_cond     cv;
    int          inited;    /* lk and cv exist                               */
    int          opened;
    int          closing;
    int          users;     /* calls inside that use the handle's locks      */
    int          rule_refs; /* open Rules that write to this Device          */
    PyObject*    rule;      /* the Rule it feeds, or NULL                    */
    PyObject*    sim;       /* its Sim, or NULL                              */
#if YDB_HAVE_PARALLEL
    ypar_port*   par;
#endif
} DeviceObject;

typedef struct {
    PyObject_HEAD
    ydev_rule* rule;        /* calloc'd                                      */
    PyObject*  out;         /* the output Device                             */
    int        opened;
    int        users;       /* open Devices that feed it (under the GIL)     */
} RuleObject;

#define DEV(self) ((DeviceObject*)(self))

/* 1: entered (call dv_leave), 0: closed (the header's closed handle is safe
 * to read), -1: closed and need_open (an exception is set). A close in
 * progress is waited out. */
static int dv_enter(PyObject* self, int need_open) {
    DeviceObject* d = DEV(self);
    int st = 0, closing;
    if (!d->inited) goto closed;
    ybd_lock(&d->lk);
    closing = d->closing;
    if (!closing) { st = d->opened; if (st) d->users++; }
    ybd_unlock(&d->lk);
    if (closing) {
        Py_BEGIN_ALLOW_THREADS
        ybd_lock(&d->lk);
        while (d->closing) ybd_cond_wait(&d->cv, &d->lk, YRT_MS(50));
        st = d->opened;
        if (st) d->users++;
        ybd_unlock(&d->lk);
        Py_END_ALLOW_THREADS
    }
    if (st) return 1;
closed:
    if (need_open) {
        dv_fail(DvClosed, "", YDEV_ERR_STATE, "ysp_device: the device is closed");
        return -1;
    }
    return 0;
}

static void dv_leave(PyObject* self) {
    DeviceObject* d = DEV(self);
    ybd_lock(&d->lk);
    d->users--;
    if (d->users == 0 && d->closing) ybd_broadcast(&d->cv);
    ybd_unlock(&d->lk);
}

static int Device_init(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "role", "family", "key", "device", "kind", "baud", "pulse", "latched", "min_hold",
                          "probe", "retry", "silent", "max_width", "no_identify", "no_elevate", "queue",
                          "rule", "sim", NULL };
    DeviceObject* d = DEV(self);
    const char *role, *key;
    PyObject *fam_o, *pulse_o = NULL, *hold_o = NULL, *probe_o = NULL, *retry_o = NULL, *silent_o = NULL,
             *mw_o = NULL, *rule_o = NULL, *sim_o = NULL;
    unsigned int device = 0, baud = 0, queue = DV_QUEUE_DEFAULT;
    int kind = 0, latched = 0, no_identify = 0, no_elevate = 0, family;
    int64_t v;
    ydev_desc desc;
    bool ok;
    char err[200];

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sOs|$IiIOpOOOOOppIOO", kw, &role, &fam_o, &key, &device, &kind,
                                     &baud, &pulse_o, &latched, &hold_o, &probe_o, &retry_o, &silent_o, &mw_o,
                                     &no_identify, &no_elevate, &queue, &rule_o, &sim_o))
        return -1;
    if (d->dev) { PyErr_SetString(DvError, "Device: already opened; make a new Device"); return -1; }
    if (dv_family(fam_o, &family) < 0) return -1;
    memset(&desc, 0, sizeof desc);
    desc.role = role;
    desc.family = family;
    desc.key = key;
    desc.device = device;
    desc.roles = device ? NULL : &g_roles;
    desc.kind = kind;
    desc.baud = baud;
    desc.latched = latched ? true : false;
    desc.no_identify = no_identify ? true : false;
    desc.no_elevate = no_elevate ? true : false;
    if (pulse_o && pulse_o != Py_None) { if (dv_seconds(pulse_o, "pulse", DV_S_POS, &desc.pulse_ns) < 0) return -1; }
    if (hold_o && hold_o != Py_None) {
        if (dv_seconds(hold_o, "min_hold", DV_S_POS, &desc.min_hold_ns) < 0) return -1;
        if (desc.min_hold_ns == 0) {
            PyErr_SetString(PyExc_ValueError, "min_hold: > 0 (omit it for the family's default)");
            return -1;
        }
    }
    /* math.inf: never (C < 0); a finite value > 0; omitted: the default */
    if (probe_o && probe_o != Py_None) {
        if (dv_seconds(probe_o, "probe", DV_S_INF | DV_S_POS, &v) < 0) return -1;
        if (v == 0) { PyErr_SetString(PyExc_ValueError, "probe: > 0, or math.inf for no queries"); return -1; }
        desc.probe_ns = v == INT64_MAX ? -1 : v;
    }
    if (silent_o && silent_o != Py_None) {
        if (dv_seconds(silent_o, "silent", DV_S_INF | DV_S_POS, &v) < 0) return -1;
        if (v == 0) { PyErr_SetString(PyExc_ValueError, "silent: > 0, or math.inf for never LOST"); return -1; }
        desc.silent_ns = v == INT64_MAX ? -1 : v;
    }
    if (retry_o && retry_o != Py_None) {
        if (dv_seconds(retry_o, "retry", DV_S_POS, &desc.retry_ns) < 0) return -1;
        if (desc.retry_ns == 0) { PyErr_SetString(PyExc_ValueError, "retry: > 0"); return -1; }
    }
    if (mw_o && mw_o != Py_None) {
        if (dv_seconds(mw_o, "max_width", DV_S_POS, &desc.max_width_ns) < 0) return -1;
        if (desc.max_width_ns == 0) { PyErr_SetString(PyExc_ValueError, "max_width: > 0"); return -1; }
    }
    if (queue < 1 || queue > DV_QUEUE_MAX) {
        PyErr_Format(PyExc_ValueError, "queue: 1 to %u events", DV_QUEUE_MAX);
        return -1;
    }
    if (rule_o == Py_None) rule_o = NULL;
    if (sim_o == Py_None) sim_o = NULL;
    if (rule_o) {
        RuleObject* r;
        if (!PyObject_TypeCheck(rule_o, (PyTypeObject*)DvRuleType)) {
            PyErr_SetString(PyExc_TypeError, "rule: a ysp.device.Rule");
            return -1;
        }
        r = (RuleObject*)rule_o;
        if (!r->opened) { PyErr_SetString(DvClosed, "rule: the Rule is closed"); return -1; }
        if (!(ybox_caps(family) & YBOX_CAP_IN)) {
            PyErr_SetString(PyExc_ValueError, "rule: this family has no inputs");
            return -1;
        }
        desc.rule = r->rule;
    }
    if (sim_o) {
        if (!PyObject_TypeCheck(sim_o, (PyTypeObject*)DvSimType) || !((SimObject*)sim_o)->sim) {
            PyErr_SetString(PyExc_TypeError, "sim: a ysp.device.Sim");
            return -1;
        }
        if (((SimObject*)sim_o)->sim->family != family) {
            PyErr_SetString(PyExc_ValueError, "sim: the Sim is of another family");
            return -1;
        }
        desc.transport = ybd_sim_transport(((SimObject*)sim_o)->sim);
    }
    if (family == YBOX_PARALLEL && !sim_o) {
#if YDB_HAVE_PARALLEL
        d->par = (ypar_port*)calloc(1, sizeof(ypar_port));
        if (!d->par) { PyErr_NoMemory(); return -1; }
        desc.transport = ydev_parallel_transport(d->par);
#else
        PyErr_SetString(DvError, "family parallel: ysp/parallel.h does not support this platform");
        return -1;
#endif
    }
    d->dev = (ydev_device*)calloc(1, sizeof(ydev_device));
    if (!d->dev) { PyErr_NoMemory(); return -1; }
    if (ybd_queue_init(&d->q, queue) < 0) { PyErr_NoMemory(); return -1; }
    desc.sink = ybd_sink;
    desc.sink_ctx = &d->q;
    ybd_sync_init(&d->lk, &d->cv);
    d->inited = 1;

    Py_BEGIN_ALLOW_THREADS
    ybd_lock(&g_roles_lock);
    ok = ydev_open(d->dev, &desc);
    ybd_unlock(&g_roles_lock);
    Py_END_ALLOW_THREADS
    if (!ok) {
        snprintf(err, sizeof err, "%s", ydev_error(d->dev));
        dv_fail(DvError, "open", YDEV_ERR_ARG, err);
        return -1;
    }
    d->opened = 1;
    if (rule_o) { Py_INCREF(rule_o); d->rule = rule_o; ((RuleObject*)rule_o)->users++; }
    if (sim_o) { Py_INCREF(sim_o); d->sim = sim_o; }
    return 0;
}

/* Close: refuse while a Rule writes to it; wait for the calls inside;
 * ydev_close() without the GIL. 0, or -1 with an exception. */
static int dv_close(PyObject* self) {
    DeviceObject* d = DEV(self);
    int refs, act = 0;
    if (!d->inited) return 0;
    ybd_lock(&d->lk);
    refs = d->rule_refs;
    if (d->opened && !d->closing && !refs) { d->closing = 1; act = 1; }
    ybd_unlock(&d->lk);
    if (refs) {
        PyErr_SetString(DvError, "close: an open Rule writes to this device; close the Rule first "
                                 "(close the input, then the Rule, then the output)");
        return -1;
    }
    if (!act) {
        /* closed, or another thread is closing it: return when it is done */
        Py_BEGIN_ALLOW_THREADS
        ybd_lock(&d->lk);
        while (d->closing) ybd_cond_wait(&d->cv, &d->lk, YRT_MS(50));
        ybd_unlock(&d->lk);
        Py_END_ALLOW_THREADS
        return 0;
    }
    Py_BEGIN_ALLOW_THREADS
    ybd_lock(&d->lk);
    while (d->users > 0) ybd_cond_wait(&d->cv, &d->lk, YRT_MS(50));
    ybd_unlock(&d->lk);
    ybd_lock(&g_roles_lock);
    ydev_close(d->dev);
    ybd_unlock(&g_roles_lock);
    ybd_queue_close(&d->q);
    ybd_lock(&d->lk);
    d->opened = 0;
    d->closing = 0;
    ybd_unlock(&d->lk);
    ybd_broadcast(&d->cv);
    Py_END_ALLOW_THREADS
    if (d->rule) ((RuleObject*)d->rule)->users--;
    return 0;
}

static int dv_clear(PyObject* self) {
    DeviceObject* d = DEV(self);
    Py_CLEAR(d->rule);
    Py_CLEAR(d->sim);
    return 0;
}

static void Device_dealloc(PyObject* self) {
    DeviceObject* d = DEV(self);
    PyTypeObject* tp = Py_TYPE(self);
    freefunc f = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    if (d->inited) {
        if (dv_close(self) < 0) {
            /* a Rule still writes to it: the Rule holds a reference, so this
             * cannot run; keep the interpreter consistent anyway */
            PyErr_WriteUnraisable(self);
        }
    }
    dv_clear(self);
    if (d->inited) { ybd_sync_free(&d->lk, &d->cv); d->inited = 0; }
    ybd_queue_free(&d->q);
    free(d->dev);
    d->dev = NULL;
#if YDB_HAVE_PARALLEL
    free(d->par);
    d->par = NULL;
#endif
    f(self);
    Py_DECREF(tp);
}

static PyObject* Device_close(PyObject* self, PyObject* Py_UNUSED(a)) {
    if (dv_close(self) < 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject* Device_enter(PyObject* self, PyObject* Py_UNUSED(a)) {
    Py_INCREF(self);
    return self;
}

static PyObject* Device_exit(PyObject* self, PyObject* Py_UNUSED(a)) {
    if (dv_close(self) < 0) return NULL;
    Py_RETURN_FALSE;
}

/* The longest a GIL-free wait lasts before signals get a look. */
#define DV_SLICE_NS YRT_MS(50)

static PyObject* Device_wait_running(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "timeout", NULL };
    PyObject* t_o;
    int64_t timeout, end;
    int rc = YDEV_ERR_TIMEOUT;
    char msg[200];
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kw, &t_o)) return NULL;
    if (dv_seconds(t_o, "timeout", DV_S_INF, &timeout) < 0) return NULL;
    if (dv_enter(self, 1) < 0) return NULL;
    end = timeout == INT64_MAX ? INT64_MAX : yrt_now_ns() + timeout;
    for (;;) {
        int64_t now = yrt_now_ns(), slice = end - now;
        if (slice > DV_SLICE_NS) slice = DV_SLICE_NS;
        if (slice < 0) slice = 0;
        Py_BEGIN_ALLOW_THREADS
        rc = ydev_wait_running(DEV(self)->dev, slice);
        Py_END_ALLOW_THREADS
        if (rc != YDEV_ERR_TIMEOUT || yrt_now_ns() >= end) break;
        if (PyErr_CheckSignals() < 0) { dv_leave(self); return NULL; }
    }
    snprintf(msg, sizeof msg, "%s", ydev_error(DEV(self)->dev));
    dv_leave(self);
    if (rc == YDEV_RUNNING) Py_RETURN_TRUE;
    if (rc == YDEV_ERR_TIMEOUT) Py_RETURN_FALSE;
    return dv_fail(DvError, "wait_running", rc, msg);
}

/* Build at most max (0: all) of the queued events. */
static PyObject* dv_take(PyObject* self, Py_ssize_t max) {
    DeviceObject* d = DEV(self);
    uint64_t tail, n, i;
    PyObject* out;
    if (!d->q.ev) return PyList_New(0);
    if (ybd_queue_claim(&d->q, (uint64_t)max, &tail, &n) < 0) {
        PyErr_SetString(DvError, "poll: another thread is taking this device's events");
        return NULL;
    }
    out = PyList_New((Py_ssize_t)n);
    for (i = 0; out && i < n; i++) {
        PyObject* e = dv_event(ybd_queue_at(&d->q, tail + i));
        if (!e) { Py_CLEAR(out); break; }
        PyList_SetItem(out, (Py_ssize_t)i, e);   /* steals */
    }
    /* on failure the events stay for the next call */
    ybd_queue_release(&d->q, out ? n : 0);
    return out;
}

static PyObject* Device_poll(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "max", NULL };
    Py_ssize_t max = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|n", kw, &max)) return NULL;
    if (max < 0) { PyErr_SetString(PyExc_ValueError, "max: events, >= 0 (0: all)"); return NULL; }
    return dv_take(self, max);
}

static PyObject* Device_wait(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "timeout", "max", NULL };
    PyObject* t_o;
    Py_ssize_t max = 0;
    int64_t timeout, end;
    DeviceObject* d = DEV(self);
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|n", kw, &t_o, &max)) return NULL;
    if (max < 0) { PyErr_SetString(PyExc_ValueError, "max: events, >= 0 (0: all)"); return NULL; }
    if (dv_seconds(t_o, "timeout", DV_S_INF, &timeout) < 0) return NULL;
    if (!d->q.ev) return PyList_New(0);
    end = timeout == INT64_MAX ? INT64_MAX : yrt_now_ns() + timeout;
    for (;;) {
        uint64_t n;
        int closed;
        int64_t now = yrt_now_ns(), slice = end - now;
        if (slice > DV_SLICE_NS) slice = DV_SLICE_NS;
        if (slice < 0) slice = 0;
        Py_BEGIN_ALLOW_THREADS
        n = ybd_queue_wait(&d->q, slice, &closed);
        Py_END_ALLOW_THREADS
        if (n || closed || yrt_now_ns() >= end) break;
        if (PyErr_CheckSignals() < 0) return NULL;
    }
    return dv_take(self, max);
}

static PyObject* Device_clear(PyObject* self, PyObject* Py_UNUSED(a)) {
    DeviceObject* d = DEV(self);
    if (!d->q.ev) return PyLong_FromLong(0);
    return PyLong_FromUnsignedLongLong((unsigned long long)ybd_queue_clear(&d->q));
}

static PyObject* Device_get_pending(PyObject* self, void* c) {
    DeviceObject* d = DEV(self);
    uint64_t n = 0;
    (void)c;
    if (d->q.ev) {
        ybd_lock(&d->q.lock);
        n = d->q.head - d->q.tail;
        ybd_unlock(&d->q.lock);
    }
    return PyLong_FromUnsignedLongLong((unsigned long long)n);
}

/* ---- outputs ---- */

static int dv_code(PyObject* o, uint32_t* out) {
    unsigned long long v = PyLong_AsUnsignedLongLong(o);
    if (v == (unsigned long long)-1 && PyErr_Occurred()) return -1;
    if (v > 0xFFFFFFFFull) { PyErr_SetString(PyExc_OverflowError, "code: 0 to 2**32 - 1"); return -1; }
    *out = (uint32_t)v;
    return 0;
}

static PyObject* Device_set(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "code", NULL };
    PyObject* c_o;
    uint32_t code;
    int rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kw, &c_o) || dv_code(c_o, &code) < 0) return NULL;
    if (dv_enter(self, 1) < 0) return NULL;
    Py_BEGIN_ALLOW_THREADS
    rc = ydev_out_set(DEV(self)->dev, code);
    Py_END_ALLOW_THREADS
    dv_leave(self);
    if (rc < 0) return dv_out_fail("set", rc);
    Py_RETURN_NONE;
}

static PyObject* Device_pulse(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "code", "width", NULL };
    PyObject *c_o, *w_o;
    uint32_t code;
    int64_t width;
    int rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO", kw, &c_o, &w_o) || dv_code(c_o, &code) < 0) return NULL;
    if (dv_seconds(w_o, "width", DV_S_POS, &width) < 0) return NULL;
    if (dv_enter(self, 1) < 0) return NULL;
    Py_BEGIN_ALLOW_THREADS
    rc = ydev_out_pulse(DEV(self)->dev, code, width);
    Py_END_ALLOW_THREADS
    dv_leave(self);
    if (rc < 0) return dv_out_fail("pulse", rc);
    Py_RETURN_NONE;
}

static PyObject* Device_mark(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "code", "text", NULL };
    PyObject* c_o;
    const char* text = NULL;
    uint32_t code;
    int rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|z", kw, &c_o, &text) || dv_code(c_o, &code) < 0) return NULL;
    if (dv_enter(self, 1) < 0) return NULL;
    Py_BEGIN_ALLOW_THREADS
    rc = ydev_out_mark(DEV(self)->dev, code, text);
    Py_END_ALLOW_THREADS
    dv_leave(self);
    if (rc < 0) return dv_out_fail("mark", rc);
    Py_RETURN_NONE;
}

static PyObject* dv_out_info(const ydev_out_info* o) {
    return Py_BuildValue("{s:N,s:N,s:N,s:N,s:k,s:k,s:k,s:i}",
                         "t_before", dv_s(o->t_before), "t_after", dv_s(o->t_after),
                         "t_before_ns", PyLong_FromLongLong((long long)o->t_before),
                         "width", dv_s(o->width_ns), "code", (unsigned long)o->code,
                         "seq", (unsigned long)o->seq, "flags", (unsigned long)o->flags, "result", o->result);
}

static PyObject* Device_last_out(PyObject* self, PyObject* Py_UNUSED(a)) {
    ydev_out_info o;
    int st = dv_enter(self, 0);
    memset(&o, 0, sizeof o);
    if (st < 0) return NULL;
    if (st) { ydev_out_last(DEV(self)->dev, &o); dv_leave(self); }
    return dv_out_info(&o);
}

/* ---- status ---- */

static PyObject* Device_stats(PyObject* self, PyObject* Py_UNUSED(a)) {
    DeviceObject* d = DEV(self);
    ydev_stats s;
    uint64_t qn = 0, qev = 0, qdrop = 0, qmax = 0;
    unsigned long qcap = 0;
    int st = dv_enter(self, 0);
    PyObject *fit, *out;
    if (st < 0) return NULL;
    memset(&s, 0, sizeof s);
    if (d->dev) ydev_get_stats(d->dev, &s);   /* a closed handle gives its last snapshot */
    if (st) dv_leave(self);
    if (d->q.ev) {
        ybd_lock(&d->q.lock);
        qn = d->q.head - d->q.tail;
        qev = d->q.events;
        qdrop = d->q.dropped;
        qmax = d->q.max_used;
        qcap = d->q.cap;
        ybd_unlock(&d->q.lock);
    }
    fit = Py_BuildValue("{s:N,s:K,s:d,s:d,s:N,s:N,s:N,s:I,s:I,s:I,s:I,s:I,s:O,s:O}",
                        "t0", dv_s(s.fit.t0_ns), "ticks0", (unsigned long long)s.fit.ticks0,
                        "ns_per_tick", s.fit.ns_per_tick, "ppm", s.fit.ppm,
                        "spread", dv_s(s.fit.spread_ns), "width", dv_s(s.fit.width_ns), "span", dv_s(s.fit.span_ns),
                        "points", (unsigned)s.fit.points, "pairs", (unsigned)s.fit.pairs,
                        "epoch", (unsigned)s.fit.epoch, "rejected", (unsigned)s.fit.rejected,
                        "pending", (unsigned)s.fit.pending, "ready", s.fit.ready ? Py_True : Py_False,
                        "slope", s.fit.slope ? Py_True : Py_False);
    if (!fit) return NULL;
    out = Py_BuildValue("{s:i,s:s,s:I,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:N,s:N,s:s,s:s,"
                        "s:K,s:K,s:K,s:K,s:k}",
                        "state", s.state, "state_name", ydev_state_name(s.state), "opens", (unsigned)s.opens,
                        "reads", (unsigned long long)s.reads, "bytes", (unsigned long long)s.bytes,
                        "events", (unsigned long long)s.events, "pairs", (unsigned long long)s.pairs,
                        "probes", (unsigned long long)s.probes, "answers", (unsigned long long)s.answers,
                        "refused", (unsigned long long)s.refused, "garbage", (unsigned long long)s.garbage,
                        "clamped", (unsigned long long)s.clamped, "outs", (unsigned long long)s.outs,
                        "out_errors", (unsigned long long)s.out_errors, "reports", (unsigned long long)s.reports,
                        "last_rx", dv_s(s.last_rx_ns), "fit", fit, "found", s.found, "ident", s.ident,
                        "queued", (unsigned long long)qn, "queue_events", (unsigned long long)qev,
                        "queue_dropped", (unsigned long long)qdrop, "queue_max_used", (unsigned long long)qmax,
                        "queue_capacity", qcap);
    return out;
}

static PyObject* dv_source_dict(const yin_source* s) {
    return Py_BuildValue("{s:i,s:i,s:i,s:k,s:N,s:N,s:L,s:L,s:s}",
                         "kind", (int)s->kind, "tier", (int)s->tier, "partial", (int)s->partial,
                         "device", (unsigned long)s->device, "lo", dv_s(s->lo_ns), "hi", dv_s(s->hi_ns),
                         "lo_ns", (long long)s->lo_ns, "hi_ns", (long long)s->hi_ns,
                         "note", s->note ? s->note : "");
}

static PyObject* Device_source(PyObject* self, PyObject* Py_UNUSED(a)) {
    yin_source s;
    if (!DEV(self)->dev) { PyErr_SetString(DvError, "source: not opened"); return NULL; }
    s = ydev_source(DEV(self)->dev);
    return dv_source_dict(&s);
}

static PyObject* Device_get_state(PyObject* self, void* c) {
    int st = dv_enter(self, 0), s = YDEV_CLOSED;
    (void)c;
    if (st < 0) return NULL;
    if (st) { s = ydev_state(DEV(self)->dev); dv_leave(self); }
    return PyLong_FromLong(s);
}

static PyObject* Device_get_state_name(PyObject* self, void* c) {
    PyObject* s = Device_get_state(self, c);
    long v;
    if (!s) return NULL;
    v = PyLong_AsLong(s);
    Py_DECREF(s);
    return PyUnicode_FromString(ydev_state_name((int)v));
}

static PyObject* Device_get_is_open(PyObject* self, void* c) {
    DeviceObject* d = DEV(self);
    int o = 0;
    (void)c;
    if (d->inited) { ybd_lock(&d->lk); o = d->opened && !d->closing; ybd_unlock(&d->lk); }
    return PyBool_FromLong(o);
}

static PyObject* Device_get_error(PyObject* self, void* c) {
    char msg[200];
    int st = dv_enter(self, 0);
    (void)c;
    if (st < 0) return NULL;
    if (!DEV(self)->dev) msg[0] = '\0';
    else if (st) {
        /* the reader thread writes the message under the instance's lock;
         * a state read takes that lock, so the copy is after its last write */
        (void)ydev_state(DEV(self)->dev);
        snprintf(msg, sizeof msg, "%s", ydev_error(DEV(self)->dev));
    } else {
        snprintf(msg, sizeof msg, "%s", ydev_error(DEV(self)->dev));
    }
    if (st > 0) dv_leave(self);
    return PyUnicode_FromString(msg);
}

#define DV_GET(name, expr)                                                         \
    static PyObject* Device_get_##name(PyObject* self, void* c) {                  \
        ydev_device* dv = DEV(self)->dev;                                          \
        (void)c;                                                                   \
        if (!dv) { PyErr_SetString(DvError, "not opened"); return NULL; }         \
        return expr;                                                               \
    }
DV_GET(role, PyUnicode_FromString(dv->role_buf))
DV_GET(key, PyUnicode_FromString(dv->key_buf))
DV_GET(family, PyLong_FromLong(dv->d.family))
DV_GET(family_name, PyUnicode_FromString(ybox_family_name(dv->d.family)))
DV_GET(device, PyLong_FromUnsignedLong((unsigned long)dv->d.device))
DV_GET(kind, PyLong_FromLong(dv->d.kind))
DV_GET(baud, PyLong_FromUnsignedLong((unsigned long)dv->d.baud))
DV_GET(pulse, dv_s(dv->d.pulse_ns))
DV_GET(min_hold, dv_s(dv->d.min_hold_ns))
DV_GET(latched, PyBool_FromLong(dv->d.latched))

static PyGetSetDef Device_getset[] = {
    { "role", Device_get_role, NULL, "The role (the experiment's name for the device).", NULL },
    { "key", Device_get_key, NULL, "The match key.", NULL },
    { "family", Device_get_family, NULL, "The family (XID, LINE, ...).", NULL },
    { "family_name", Device_get_family_name, NULL, "The family's name ('xid', 'line', ...).", NULL },
    { "device", Device_get_device, NULL, "The events' device field: the role's index in the process's role table, "
                                         "or the device= given.", NULL },
    { "kind", Device_get_kind, NULL, "The events' kind (KIND_*).", NULL },
    { "baud", Device_get_baud, NULL, "The serial rate opened (0 for the parallel port).", NULL },
    { "pulse", Device_get_pulse, NULL, "The pulse width of mark() in seconds; 0: a set.", NULL },
    { "min_hold", Device_get_min_hold, NULL, "A host-timed pulse's least hold after its leading write returned, seconds.", NULL },
    { "latched", Device_get_latched, NULL, "MMBT-S with its switch at S.", NULL },
    { "state", Device_get_state, NULL, "CLOSED, OPENING, RUNNING, LOST or FAILED.", NULL },
    { "state_name", Device_get_state_name, NULL, "The state's name.", NULL },
    { "is_open", Device_get_is_open, NULL, "True until close().", NULL },
    { "error", Device_get_error, NULL, "The last failure's message (ydev_error), '' for none.", NULL },
    { "pending", Device_get_pending, NULL, "Events waiting in the queue.", NULL },
    { NULL }
};

static PyMethodDef Device_methods[] = {
    { "wait_running", (PyCFunction)(void (*)(void))Device_wait_running, METH_VARARGS | METH_KEYWORDS,
      "wait_running(timeout) -> bool: wait up to timeout seconds (math.inf: forever) until the device is "
      "RUNNING. Call it before the first output, or the output raises. False on timeout; raises Error when "
      "the device ends FAILED (a wrong device at the key). Releases the GIL." },
    { "poll", (PyCFunction)(void (*)(void))Device_poll, METH_VARARGS | METH_KEYWORDS,
      "poll(max=0) -> list[Event]: take the queued events without waiting, oldest first, at most max "
      "(0: all)." },
    { "wait", (PyCFunction)(void (*)(void))Device_wait, METH_VARARGS | METH_KEYWORDS,
      "wait(timeout, max=0) -> list[Event]: wait up to timeout seconds (0 polls, math.inf forever) for an "
      "event, then take them as poll(). [] on timeout or after close(). Releases the GIL; Ctrl+C ends it." },
    { "clear", Device_clear, METH_NOARGS, "clear() -> int: drop the queued events; returns how many." },
    { "set", (PyCFunction)(void (*)(void))Device_set, METH_VARARGS | METH_KEYWORDS,
      "set(code): hold code on the outputs (0 is idle). Raises Error (args: message, code) when it was not "
      "written: ERR_STATE before RUNNING, ERR_ARG for a code the family cannot give, ERR_IO. Releases the GIL." },
    { "pulse", (PyCFunction)(void (*)(void))Device_pulse, METH_VARARGS | METH_KEYWORDS,
      "pulse(code, width): code for width seconds, then 0. The device times the end (XID, line board), or "
      "the family's fixed 8 ms (BioSemi, MMBT-S at P: width must be 0.008), or the instance's worker from "
      "the start of the leading write. Raises as set()." },
    { "mark", (PyCFunction)(void (*)(void))Device_mark, METH_VARARGS | METH_KEYWORDS,
      "mark(code, text=None): the code as pulse= says (a pulse, or a set when 0), and the text in the "
      "device's log. Raises as set()." },
    { "last_out", Device_last_out, METH_NOARGS,
      "last_out() -> dict: the last output call of any thread: t_before and t_after (the write's bracket, "
      "seconds), t_before_ns, width, code, seq, flags (OUT_*), result (OK or ERR_*)." },
    { "stats", Device_stats, METH_NOARGS,
      "stats() -> dict: the reader's counters (state, opens, reads, bytes, events, pairs, probes, answers, "
      "refused, garbage, clamped, outs, out_errors, reports, last_rx), the clock fit (fit: a dict, times in "
      "seconds), found (the port), ident (the device's answer), and the binding's queue (queued, "
      "queue_events, queue_dropped, queue_max_used, queue_capacity). Readable after close()." },
    { "source", Device_source, METH_NOARGS,
      "source() -> dict: the events' source entry (ydev_source): kind, tier (TIER_UNKNOWN: no loopback "
      "checked this layer), partial, device, lo, hi, note. ysp.rigfile's Profile.source(device) gives tier 1 "
      "when the rig profile's bounds checked." },
    { "close", Device_close, METH_NOARGS,
      "close(): stop the reader, end a pending pulse, write 0 if a code is held, close the port. Idempotent. "
      "Raises while an open Rule writes to this device. Releases the GIL." },
    { "__enter__", Device_enter, METH_NOARGS, NULL },
    { "__exit__", Device_exit, METH_VARARGS, NULL },
    { NULL }
};

static PyType_Slot Device_slots[] = {
    { Py_tp_doc, (void*)"Device(role, family, key, *, device=0, kind=0, baud=0, pulse=None, latched=False, "
                        "min_hold=None, probe=None, retry=None, silent=None, max_width=None, no_identify=False, "
                        "no_elevate=False, queue=1024, rule=None, sim=None): one ysp/device.h instance with its "
                        "reader thread. family: a number (XID, LINE, ...) or a name ('xid', 'line', 'photo', "
                        "'triggerbox', 'biosemi', 'mmbts', 'lines', 'parallel'). key: the match key "
                        "('serial:<vid>:<pid>:<serial>:<location>', 'port:COM5', 'parallel:'). device=0 takes "
                        "the role's index from the process's role table. Times are seconds; probe and silent "
                        "take math.inf for none. queue: the events kept for poll() and wait(); a full queue "
                        "drops new events and counts them. The device need not be there yet: it is OPENING "
                        "until found; see wait_running(). ysp.rigfile's Profile.desc(role) gives the keyword "
                        "arguments of a role in the rig profile." },
    { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_init, (void*)Device_init },
    { Py_tp_dealloc, (void*)Device_dealloc },
    { Py_tp_methods, Device_methods },
    { Py_tp_getset, Device_getset },
    { 0, NULL }
};

/* No GC: a Device holds its Rule and Sim, a Rule its output Device, and
 * none of them can hold itself through the others, so no cycle forms. */
static PyType_Spec Device_spec = { "ysp.device.Device", sizeof(DeviceObject), 0, Py_TPFLAGS_DEFAULT, Device_slots };

/* ---- Rule -------------------------------------------------------------------- */

#define RULE(self) ((RuleObject*)(self))

static int dv_choice(PyObject* o, ydev_rule_choice* c) {
    static const char* const names[4] = { "kind", "device", "control", "code" };
    unsigned long v[4] = { YIN_KIND_BOX, 0, 0, 0 };
    int i;
    PyObject* item;
    if (!PyDict_Check(o)) {
        PyErr_SetString(PyExc_TypeError, "choices: dicts with kind, device, control and code");
        return -1;
    }
    {
        PyObject *k, *val;
        Py_ssize_t pos = 0;
        while (PyDict_Next(o, &pos, &k, &val)) {
            int known = 0;
            for (i = 0; i < 4; i++) {
                PyObject* n = PyUnicode_FromString(names[i]);
                int eq = n ? PyObject_RichCompareBool(k, n, Py_EQ) : -1;
                Py_XDECREF(n);
                if (eq < 0) return -1;
                if (eq) known = 1;
            }
            if (!known) { PyErr_SetString(PyExc_ValueError, "choices: unknown key (kind, device, control, code)"); return -1; }
        }
    }
    for (i = 0; i < 4; i++) {
        item = PyDict_GetItemString(o, names[i]);   /* borrowed */
        if (!item) {
            if (i == 3) { PyErr_SetString(PyExc_ValueError, "choices: each needs a code"); return -1; }
            continue;
        }
        v[i] = PyLong_AsUnsignedLong(item);
        if (v[i] == (unsigned long)-1 && PyErr_Occurred()) return -1;
    }
    if (v[0] > 15) { PyErr_SetString(PyExc_ValueError, "choices: kind is a KIND_* (0 to 15)"); return -1; }
    memset(c, 0, sizeof *c);
    c->kind = (uint8_t)v[0];
    c->device = (uint32_t)v[1];
    c->control = (uint32_t)v[2];
    c->code = (uint32_t)v[3];
    return 0;
}

static int Rule_init(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "out", "choices", "width", "max_fires", NULL };
    RuleObject* r = RULE(self);
    PyObject *out_o, *ch_o, *w_o = NULL, *mf_o = NULL;
    ydev_rule_choice ch[YDEV_RULE_CHOICES];
    ydev_rule_desc rd;
    Py_ssize_t n, i;
    int st;
    bool ok;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO|OO", kw, &out_o, &ch_o, &w_o, &mf_o)) return -1;
    if (r->rule) { PyErr_SetString(DvError, "Rule: already opened"); return -1; }
    if (!PyObject_TypeCheck(out_o, (PyTypeObject*)DvDeviceType)) {
        PyErr_SetString(PyExc_TypeError, "out: a ysp.device.Device");
        return -1;
    }
    memset(&rd, 0, sizeof rd);
    if (PyUnicode_Check(ch_o) || PyDict_Check(ch_o) || !PySequence_Check(ch_o)) {
        PyErr_SetString(PyExc_TypeError, "choices: a sequence of dicts");
        return -1;
    }
    n = PySequence_Size(ch_o);
    if (n < 0) return -1;
    if (n < 1 || n > YDEV_RULE_CHOICES) {
        PyErr_SetString(PyExc_ValueError, "choices: 1 to 32");
        return -1;
    }
    for (i = 0; i < n; i++) {
        PyObject* item = PySequence_GetItem(ch_o, i);
        int rc = item ? dv_choice(item, &ch[i]) : -1;
        Py_XDECREF(item);
        if (rc < 0) return -1;
    }
    if (w_o && w_o != Py_None && dv_seconds(w_o, "width", DV_S_POS, &rd.width_ns) < 0) return -1;
    rd.max_fires = 0;   /* 1 */
    if (mf_o == Py_None) rd.max_fires = -1;
    else if (mf_o) {
        long m = PyLong_AsLong(mf_o);
        if (m == -1 && PyErr_Occurred()) return -1;
        if (m < 1 || m > INT_MAX) { PyErr_SetString(PyExc_ValueError, "max_fires: >= 1, or None for no limit"); return -1; }
        rd.max_fires = (int)m;
    }
    rd.choices = ch;
    rd.n_choices = (int)n;
    st = dv_enter(out_o, 1);
    if (st < 0) return -1;
    rd.out = DEV(out_o)->dev;
    r->rule = (ydev_rule*)calloc(1, sizeof(ydev_rule));
    if (!r->rule) { dv_leave(out_o); PyErr_NoMemory(); return -1; }
    ok = ydev_rule_open(r->rule, &rd);
    if (ok) {
        DeviceObject* od = DEV(out_o);
        ybd_lock(&od->lk);
        od->rule_refs++;
        ybd_unlock(&od->lk);
    }
    dv_leave(out_o);
    if (!ok) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s", ydev_rule_error(r->rule));
        free(r->rule);
        r->rule = NULL;
        dv_fail(DvError, "Rule", YDEV_ERR_ARG, msg);
        return -1;
    }
    Py_INCREF(out_o);
    r->out = out_o;
    r->opened = 1;
    return 0;
}

static int dv_rule_close(PyObject* self) {
    RuleObject* r = RULE(self);
    DeviceObject* od;
    if (!r->opened) return 0;
    if (r->users > 0) {
        PyErr_SetString(DvError, "Rule.close: an open Device feeds this rule; close it first");
        return -1;
    }
    ydev_rule_close(r->rule);
    r->opened = 0;
    od = DEV(r->out);
    ybd_lock(&od->lk);
    od->rule_refs--;
    ybd_unlock(&od->lk);
    return 0;
}

static PyObject* Rule_close(PyObject* self, PyObject* Py_UNUSED(a)) {
    if (dv_rule_close(self) < 0) return NULL;
    Py_RETURN_NONE;
}

static int Rule_clear(PyObject* self) {
    Py_CLEAR(RULE(self)->out);
    return 0;
}

static void Rule_dealloc(PyObject* self) {
    RuleObject* r = RULE(self);
    PyTypeObject* tp = Py_TYPE(self);
    freefunc f = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    /* no Device holds it (each holds a reference), so users is 0 */
    if (r->opened && r->out) (void)dv_rule_close(self);
    Rule_clear(self);
    free(r->rule);
    r->rule = NULL;
    f(self);
    Py_DECREF(tp);
}

static ydev_rule* dv_rule(PyObject* self) {
    RuleObject* r = RULE(self);
    if (!r->opened) { dv_fail(DvClosed, "", YDEV_ERR_STATE, "ysp_device: the rule is closed"); return NULL; }
    return r->rule;
}

/* from and until: seconds on now(); math.inf is YDEV_RULE_NEVER. */
static int dv_window(PyObject* args, PyObject* kwds, int64_t* from, int64_t* until) {
    static char* kw[] = { "start", "until", NULL };
    PyObject *a_o, *b_o = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O", kw, &a_o, &b_o)) return -1;
    if (dv_seconds(a_o, "start", DV_S_INF, from) < 0) return -1;
    *until = YDEV_RULE_NEVER;
    if (b_o && dv_seconds(b_o, "until", DV_S_INF, until) < 0) return -1;
    return 0;
}

static PyObject* Rule_arm(PyObject* self, PyObject* args, PyObject* kwds) {
    int64_t a, b;
    ydev_rule* r = dv_rule(self);
    if (!r || dv_window(args, kwds, &a, &b) < 0) return NULL;
    ydev_rule_arm(r, a, b);
    Py_RETURN_NONE;
}

static PyObject* Rule_window(PyObject* self, PyObject* args, PyObject* kwds) {
    int64_t a, b;
    ydev_rule* r = dv_rule(self);
    if (!r || dv_window(args, kwds, &a, &b) < 0) return NULL;
    ydev_rule_window(r, a, b);
    Py_RETURN_NONE;
}

static PyObject* Rule_disarm(PyObject* self, PyObject* Py_UNUSED(a)) {
    ydev_rule* r = dv_rule(self);
    if (!r) return NULL;
    ydev_rule_disarm(r);
    Py_RETURN_NONE;
}

static PyObject* dv_opt_s(int64_t ns, int have) {
    if (!have) Py_RETURN_NONE;
    return dv_s(ns);
}

static PyObject* Rule_info(PyObject* self, PyObject* Py_UNUSED(a)) {
    ydev_rule_info i;
    ydev_rule* r = dv_rule(self);
    int have;
    if (!r) return NULL;
    ydev_rule_get(r, &i);
    have = i.choice >= 0;
    return Py_BuildValue("{s:O,s:i,s:i,s:i,s:N,s:N,s:N,s:k,s:k,s:k,s:k,s:N}",
                         "armed", i.armed ? Py_True : Py_False, "fired", i.fired, "over", i.over,
                         "result", i.result, "t_event", dv_opt_s(i.t_event, have),
                         "t_before", dv_opt_s(i.t_before, have), "t_after", dv_opt_s(i.t_after, have),
                         "device", (unsigned long)i.device, "control", (unsigned long)i.control,
                         "code", (unsigned long)i.code, "seq", (unsigned long)i.seq,
                         "choice", have ? PyLong_FromLong(i.choice) : (Py_INCREF(Py_None), Py_None));
}

static PyObject* Rule_fires(PyObject* self, PyObject* Py_UNUSED(a)) {
    ydev_rule_fire f[YDEV_RULE_FIRES];
    ydev_rule* r = dv_rule(self);
    int n, i, k;
    PyObject* out;
    if (!r) return NULL;
    n = ydev_rule_fires(r, f, YDEV_RULE_FIRES);
    k = n < YDEV_RULE_FIRES ? n : YDEV_RULE_FIRES;
    out = PyList_New(k);
    for (i = 0; out && i < k; i++) {
        PyObject* d = Py_BuildValue("{s:N,s:L,s:k,s:k,s:k,s:i,s:i,s:i}",
                                    "t", dv_s(f[i].t), "t_ns", (long long)f[i].t,
                                    "device", (unsigned long)f[i].device, "control", (unsigned long)f[i].control,
                                    "code", (unsigned long)f[i].code, "kind", (int)f[i].kind,
                                    "choice", f[i].choice, "result", f[i].result);
        if (!d) { Py_CLEAR(out); break; }
        PyList_SetItem(out, i, d);
    }
    return out;
}

static PyObject* Rule_get_is_open(PyObject* self, void* c) {
    (void)c;
    return PyBool_FromLong(RULE(self)->opened);
}

static PyObject* Rule_enter(PyObject* self, PyObject* Py_UNUSED(a)) { Py_INCREF(self); return self; }
static PyObject* Rule_exit(PyObject* self, PyObject* Py_UNUSED(a)) {
    if (dv_rule_close(self) < 0) return NULL;
    Py_RETURN_FALSE;
}

static PyGetSetDef Rule_getset[] = {
    { "is_open", Rule_get_is_open, NULL, "True until close().", NULL },
    { NULL }
};

static PyMethodDef Rule_methods[] = {
    { "arm", (PyCFunction)(void (*)(void))Rule_arm, METH_VARARGS | METH_KEYWORDS,
      "arm(start, until=math.inf): a new window [start, until) on the press's t (seconds on now()), its fire "
      "count 0. start=math.inf: closed." },
    { "window", (PyCFunction)(void (*)(void))Rule_window, METH_VARARGS | METH_KEYWORDS,
      "window(start, until=math.inf): move the armed window (the onset moved); the count is kept." },
    { "disarm", Rule_disarm, METH_NOARGS, "disarm(): no fire until the next arm; info and fires stay." },
    { "info", Rule_info, METH_NOARGS,
      "info() -> dict: armed, fired (since the arm), over, result, and the last fire's t_event (the press), "
      "t_before and t_after (its write), device, control, code, seq, choice (None: no fire yet)." },
    { "fires", Rule_fires, METH_NOARGS,
      "fires() -> list[dict]: this arm's fires in order (at most 8 kept): t, t_ns, device, control, code, kind, "
      "choice, result (OK, ERR_*, or RULE_PENDING while the write runs)." },
    { "close", Rule_close, METH_NOARGS,
      "close(): after every Device that feeds it is closed; raises otherwise. Idempotent." },
    { "__enter__", Rule_enter, METH_NOARGS, NULL },
    { "__exit__", Rule_exit, METH_VARARGS, NULL },
    { NULL }
};

static PyType_Slot Rule_slots[] = {
    { Py_tp_doc, (void*)"Rule(out, choices, width=None, max_fires=1): a response rule (ysp/device.h RESPONSE "
                        "RULES). The input Device given rule=this tries each event on its reader thread and writes "
                        "the code of the first matching choice to out at once, not a frame later. choices: dicts "
                        "{kind=KIND_BOX, device=0 (any), control=0 (any), code}. width: the pulse in seconds "
                        "(None: out's pulse=, a set when 0). max_fires: per arm, None for no limit. Close order: "
                        "the input Device, the Rule, then out." },
    { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_init, (void*)Rule_init },
    { Py_tp_dealloc, (void*)Rule_dealloc },
    { Py_tp_methods, Rule_methods },
    { Py_tp_getset, Rule_getset },
    { 0, NULL }
};

static PyType_Spec Rule_spec = { "ysp.device.Rule", sizeof(RuleObject), 0, Py_TPFLAGS_DEFAULT, Rule_slots };

/* ---- module functions -------------------------------------------------------- */

static PyObject* mod_now(PyObject* m, PyObject* Py_UNUSED(a)) {
    (void)m;
    return PyFloat_FromDouble((double)yrt_now_ns() / 1e9);
}

static PyObject* mod_list_ports(PyObject* m, PyObject* Py_UNUSED(a)) {
    int n, got, i;
    yser_port_info* v = NULL;
    PyObject* list;
    (void)m;
    n = yser_list_ports(NULL, 0);
    if (n < 0) { PyErr_SetString(DvError, yser_strerror(n)); return NULL; }
    if (n > 0) {
        v = (yser_port_info*)PyMem_Malloc((size_t)n * sizeof *v);
        if (!v) return PyErr_NoMemory();
        got = yser_list_ports(v, n);
        if (got < 0) { PyMem_Free(v); PyErr_SetString(DvError, yser_strerror(got)); return NULL; }
        if (got < n) n = got;
    }
    list = PyList_New(n);
    for (i = 0; list && i < n; i++) {
        char key[YDEV_KEY_MAX];
        PyObject* d;
        if (ydev_port_key(&v[i], key, sizeof key) < 0) key[0] = '\0';
        d = Py_BuildValue("{s:s,s:s,s:s,s:s,s:s,s:s,s:i,s:i}", "key", key, "name", v[i].name,
                          "description", v[i].description, "serial_number", v[i].serial_number,
                          "location", v[i].location, "driver", v[i].driver, "vid", (int)v[i].vid, "pid", (int)v[i].pid);
        if (!d) { Py_CLEAR(list); break; }
        PyList_SetItem(list, i, d);
    }
    PyMem_Free(v);
    return list;
}

static PyObject* mod_key_match(PyObject* m, PyObject* args) {
    const char *p, *k;
    (void)m;
    if (!PyArg_ParseTuple(args, "ss", &p, &k)) return NULL;
    return PyBool_FromLong(ydev_key_match(p, k));
}

static PyObject* mod_family(PyObject* m, PyObject* arg) {
    int f;
    (void)m;
    if (dv_family(arg, &f) < 0) return NULL;
    return PyLong_FromLong(f);
}

static PyObject* mod_family_name(PyObject* m, PyObject* arg) {
    long f = PyLong_AsLong(arg);
    (void)m;
    if (f == -1 && PyErr_Occurred()) return NULL;
    if (f < YBOX_XID || f > YBOX_FAMILY_LAST) { PyErr_SetString(PyExc_ValueError, "not a ysp/box.h family"); return NULL; }
    return PyUnicode_FromString(ybox_family_name((int)f));
}

static PyObject* mod_state_name(PyObject* m, PyObject* arg) {
    long s = PyLong_AsLong(arg);
    (void)m;
    if (s == -1 && PyErr_Occurred()) return NULL;
    return PyUnicode_FromString(ydev_state_name((int)s));
}

static PyObject* mod_strerror(PyObject* m, PyObject* arg) {
    long c = PyLong_AsLong(arg);
    (void)m;
    if (c == -1 && PyErr_Occurred()) return NULL;
    return PyUnicode_FromString(ydev_strerror((int)c));
}

static PyObject* mod_roles(PyObject* m, PyObject* Py_UNUSED(a)) {
    PyObject* d = PyDict_New();
    int i, n;
    char names[YDEV_MAX_ROLES][YDEV_ROLE_MAX];
    (void)m;
    if (!d) return NULL;
    ybd_lock(&g_roles_lock);
    n = g_roles.n;
    memcpy(names, g_roles.name, sizeof names);
    ybd_unlock(&g_roles_lock);
    for (i = 0; i < n; i++) {
        PyObject* v = PyLong_FromLong(i + 1);
        if (!v || PyDict_SetItemString(d, names[i], v) < 0) { Py_XDECREF(v); Py_DECREF(d); return NULL; }
        Py_DECREF(v);
    }
    return d;
}

static PyMethodDef module_methods[] = {
    { "now", mod_now, METH_NOARGS,
      "now() -> float: seconds on the ysp_rt clock (QueryPerformanceCounter, CLOCK_MONOTONIC), the clock of "
      "every event's t." },
    { "list_ports", mod_list_ports, METH_NOARGS,
      "list_ports() -> list[dict]: the serial ports with their match key ('key') and name, description, "
      "serial_number, location, driver, vid, pid." },
    { "key_match", mod_key_match, METH_VARARGS,
      "key_match(pattern, key) -> bool: whether a listed key matches a pattern (empty fields match anything, "
      "case-insensitively)." },
    { "family", mod_family, METH_O, "family(name) -> int: the family's number ('xid' -> XID)." },
    { "family_name", mod_family_name, METH_O, "family_name(n) -> str: the family's name." },
    { "state_name", mod_state_name, METH_O, "state_name(state) -> str." },
    { "strerror", mod_strerror, METH_O, "strerror(code) -> str: the name of an ERR_* code." },
    { "roles", mod_roles, METH_NOARGS,
      "roles() -> dict: the process's role table, name -> index (each event's device field)." },
    { NULL }
};

static PyModuleDef ysp_device_module = {
    PyModuleDef_HEAD_INIT, "ysp.device",
    "ysp/device.h: response boxes, trigger boxes and boards as events on the ysp_rt clock, and outputs.",
    -1, module_methods, NULL, NULL, NULL, NULL
};

static int dv_add_int(PyObject* m, const char* name, long v) { return PyModule_AddIntConstant(m, name, v); }

PyMODINIT_FUNC PyInit_device(void) {
    PyObject* m;
    int ok = 1;
    ybd_sync_init(&g_roles_lock, NULL);
    m = PyModule_Create(&ysp_device_module);
    if (!m) return NULL;
    DvEventType = PyStructSequence_NewType(&dv_event_desc);
    if (!DvEventType) goto fail;
    Py_INCREF(DvEventType);
    if (PyModule_AddObject(m, "Event", (PyObject*)DvEventType) < 0) { Py_DECREF(DvEventType); goto fail; }
    DvDeviceType = PyType_FromSpec(&Device_spec);
    DvRuleType = PyType_FromSpec(&Rule_spec);
    DvSimType = PyType_FromSpec(&Sim_spec);
    if (!DvDeviceType || !DvRuleType || !DvSimType) goto fail;
    Py_INCREF(DvDeviceType); Py_INCREF(DvRuleType); Py_INCREF(DvSimType);
    if (PyModule_AddObject(m, "Device", DvDeviceType) < 0 || PyModule_AddObject(m, "Rule", DvRuleType) < 0 ||
        PyModule_AddObject(m, "Sim", DvSimType) < 0)
        goto fail;
    DvError = PyErr_NewException("ysp.device.Error", NULL, NULL);
    if (!DvError) goto fail;
    DvClosed = PyErr_NewException("ysp.device.Closed", DvError, NULL);
    if (!DvClosed) goto fail;
    Py_INCREF(DvError); Py_INCREF(DvClosed);
    if (PyModule_AddObject(m, "Error", DvError) < 0 || PyModule_AddObject(m, "Closed", DvClosed) < 0) goto fail;
    if (PyModule_AddStringConstant(m, "__version__", DV_VERSION) < 0) goto fail;

    /* families (ysp/box.h) */
    ok &= dv_add_int(m, "XID", YBOX_XID) == 0;
    ok &= dv_add_int(m, "LINE", YBOX_LINE) == 0;
    ok &= dv_add_int(m, "PHOTO", YBOX_PHOTO) == 0;
    ok &= dv_add_int(m, "TRIGGERBOX", YBOX_TRIGGERBOX) == 0;
    ok &= dv_add_int(m, "BIOSEMI", YBOX_BIOSEMI) == 0;
    ok &= dv_add_int(m, "MMBTS", YBOX_MMBTS) == 0;
    ok &= dv_add_int(m, "LINES", YBOX_LINES) == 0;
    ok &= dv_add_int(m, "PARALLEL", YBOX_PARALLEL) == 0;
    /* states */
    ok &= dv_add_int(m, "CLOSED", YDEV_CLOSED) == 0;
    ok &= dv_add_int(m, "OPENING", YDEV_OPENING) == 0;
    ok &= dv_add_int(m, "RUNNING", YDEV_RUNNING) == 0;
    ok &= dv_add_int(m, "LOST", YDEV_LOST) == 0;
    ok &= dv_add_int(m, "FAILED", YDEV_FAILED) == 0;
    /* results */
    ok &= dv_add_int(m, "OK", YDEV_OK) == 0;
    ok &= dv_add_int(m, "ERR_ARG", YDEV_ERR_ARG) == 0;
    ok &= dv_add_int(m, "ERR_FAMILY", YDEV_ERR_FAMILY) == 0;
    ok &= dv_add_int(m, "ERR_STATE", YDEV_ERR_STATE) == 0;
    ok &= dv_add_int(m, "ERR_IO", YDEV_ERR_IO) == 0;
    ok &= dv_add_int(m, "ERR_TIMEOUT", YDEV_ERR_TIMEOUT) == 0;
    ok &= dv_add_int(m, "RULE_PENDING", YDEV_RULE_PENDING) == 0;
    /* output flags (last_out()['flags']) */
    ok &= dv_add_int(m, "OUT_DEVICE_TIMED", YDEV_OUT_DEVICE_TIMED) == 0;
    ok &= dv_add_int(m, "OUT_FLUSHED", YDEV_OUT_FLUSHED) == 0;
    ok &= dv_add_int(m, "OUT_TRAILING", YDEV_OUT_TRAILING) == 0;
    ok &= dv_add_int(m, "OUT_FAILED", YDEV_OUT_FAILED) == 0;
    ok &= dv_add_int(m, "OUT_MARK", YDEV_OUT_MARK) == 0;
    ok &= dv_add_int(m, "OUT_FLIP", YDEV_OUT_FLIP) == 0;
    ok &= dv_add_int(m, "OUT_REPORTED", YDEV_OUT_REPORTED) == 0;
    ok &= dv_add_int(m, "OUT_REPLACED", YDEV_OUT_REPLACED) == 0;
    ok &= dv_add_int(m, "OUT_PULSE", YDEV_OUT_PULSE) == 0;
    ok &= dv_add_int(m, "OUT_RULE", YDEV_OUT_RULE) == 0;
    ok &= dv_add_int(m, "MAX_ROLES", YDEV_MAX_ROLES) == 0;
    /* ysp/input.h: kinds, types, flags, tiers (the C names without YIN_) */
    ok &= dv_add_int(m, "KIND_KEYBOARD", YIN_KIND_KEYBOARD) == 0;
    ok &= dv_add_int(m, "KIND_MOUSE", YIN_KIND_MOUSE) == 0;
    ok &= dv_add_int(m, "KIND_TOUCH", YIN_KIND_TOUCH) == 0;
    ok &= dv_add_int(m, "KIND_PEN", YIN_KIND_PEN) == 0;
    ok &= dv_add_int(m, "KIND_GAMEPAD", YIN_KIND_GAMEPAD) == 0;
    ok &= dv_add_int(m, "KIND_BOX", YIN_KIND_BOX) == 0;
    ok &= dv_add_int(m, "KIND_EYE", YIN_KIND_EYE) == 0;
    ok &= dv_add_int(m, "KIND_SYNC", YIN_KIND_SYNC) == 0;
    ok &= dv_add_int(m, "KIND_USER", YIN_KIND_USER) == 0;
    ok &= dv_add_int(m, "NONE", YIN_NONE) == 0;
    ok &= dv_add_int(m, "PRESS", YIN_PRESS) == 0;
    ok &= dv_add_int(m, "RELEASE", YIN_RELEASE) == 0;
    ok &= dv_add_int(m, "SAMPLE", YIN_SAMPLE) == 0;
    ok &= dv_add_int(m, "PROXIMITY", YIN_PROXIMITY) == 0;
    ok &= dv_add_int(m, "REPEAT", YIN_REPEAT) == 0;
    ok &= dv_add_int(m, "SYNTHETIC", YIN_SYNTHETIC) == 0;
    ok &= dv_add_int(m, "ERASER", YIN_ERASER) == 0;
    ok &= dv_add_int(m, "UNLISTED", YIN_UNLISTED) == 0;
    ok &= dv_add_int(m, "DEVTICKS", YIN_DEVTICKS) == 0;
    ok &= dv_add_int(m, "TIER_UNKNOWN", YIN_TIER_UNKNOWN) == 0;
    ok &= dv_add_int(m, "TIER_1", YIN_TIER_1) == 0;
    ok &= dv_add_int(m, "TIER_2", YIN_TIER_2) == 0;
    ok &= dv_add_int(m, "TIER_3", YIN_TIER_3) == 0;
    ok &= dv_add_int(m, "TIER_SIM", YIN_TIER_SIM) == 0;
    if (!ok) goto fail;
    return m;
fail:
    Py_DECREF(m);
    return NULL;
}
