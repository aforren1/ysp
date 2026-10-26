/* ysp_box_ext.c - CPython extension wrapping ysp/box.h (module ysp.box)
 *
 * The analysis side of a response box, a trigger box or a ysp line board:
 * bytes captured from the device in, ysp/input.h events and clock pairs
 * out, with no device layer, thread or port. Also the bytes a host writes
 * (queries, output codes), for a script that drives a trigger box through
 * its own serial port. Built against the stable ABI / Limited API (CPython
 * 3.8+): the Decoder is a heap type made with PyType_FromSpec, and an event
 * is a structseq made with PyStructSequence_NewType.
 *
 *     import ysp.box as box
 *     d = box.Decoder("line", kind=box.KIND_BOX)
 *     for t, chunk in capture:              # t: the read time, seconds
 *         r = d.decode(chunk, t)            # Decoded(events, pairs, outputs, text, garbage)
 *     box.encode("triggerbox", "set", 12)   # b'\x0c'
 *
 * TIMES: seconds (double), as every binding (docs/bindings.md). A read time
 * converts once, to the nearest ns, and an Event carries the exact ns
 * beside the seconds (t_ns). Device times (ticks) stay the device's own
 * counts: a fit maps them, and tick() gives the nominal unit.
 *
 * THREADING: none. Decoding is arithmetic on the object's own state under
 * the GIL; it never blocks.
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x03080000
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define YSP_BOX_IMPLEMENTATION
#include "ysp/box.h"

#include <math.h>
#include <string.h>

/* The distribution's version is the header's; a change to this binding
 * alone adds a fourth number. tests/test_box.py checks it against
 * pyproject.toml. */
#define BOX_VERSION YBOX_VERSION_STRING

/* Entries the decoder fills per ybox_decode() call. The header needs 1;
 * more means fewer calls per chunk. 64 events are 3 KB in the object. */
#define BOX_CAP 64

/* The largest read time: int64 ns end at 9.22e9 s. */
#define BOX_S_MAX 9.2e9

static PyObject* BoxError;
static PyObject* BoxArgumentError;
static PyTypeObject* EventType;   /* structseq Event */
static PyObject* PairType;        /* namedtuple Pair(ticks, probe) */
static PyObject* OutputType;      /* namedtuple Output(ticks, code) */
static PyObject* DecodedType;     /* namedtuple Decoded(events, pairs, outputs, text, garbage) */

/* --- times ------------------------------------------------------------------ */

/* Seconds to the nearest ns of the exact product s x 1e9: fma() gives the
 * rounding error of the double product, so a time given as ns / 1e9 comes
 * back as ns (ysp.rdk's conversion; docs/rdk.md "Bindings"). Refuses NaN, a
 * negative time, infinity and a time past int64 ns. */
static int box_s_to_ns(PyObject* o, const char* what, int64_t* out) {
    volatile double p;   /* stored as a double: x87 would keep 80 bits */
    double s, e, n, r;
    s = PyFloat_AsDouble(o);
    if (s == -1.0 && PyErr_Occurred()) return -1;
    if (!(s >= 0.0 && s < BOX_S_MAX)) {
        PyErr_Format(BoxArgumentError, "%s: seconds, >= 0 and finite", what);
        return -1;
    }
    p = s * 1e9;
    e = fma(s, 1e9, -p);
    n = floor(p + 0.5);
    r = (p - n) + e;
    if (r > 0.5) n += 1.0;
    else if (r < -0.5) n -= 1.0;
    *out = (int64_t)n;
    return 0;
}

/* --- names ------------------------------------------------------------------ */

static int box_ieq(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* A str's UTF-8 into buf (PyUnicode_AsUTF8 is Limited API only from 3.10).
 * A name longer than buf cannot match, so it is cut, not refused. */
static int box_utf8(PyObject* o, char* buf, size_t cap) {
    PyObject* b = PyUnicode_AsUTF8String(o);
    if (!b) return -1;
    PyOS_snprintf(buf, cap, "%s", PyBytes_AsString(b));
    Py_DECREF(b);
    return 0;
}

/* A family: its name (ybox_family_name(), any case) or its value. An
 * unknown family is refused here, so no call below sees one. */
static int box_family(PyObject* o) {
    if (PyLong_Check(o)) {
        long v = PyLong_AsLong(o);
        if (v == -1 && PyErr_Occurred()) {
            if (!PyErr_ExceptionMatches(PyExc_OverflowError)) return -1;
            PyErr_Clear();
        } else if (v >= YBOX_XID && v <= YBOX_FAMILY_LAST) {
            return (int)v;
        }
    } else if (PyUnicode_Check(o)) {
        char s[32];
        int f;
        if (box_utf8(o, s, sizeof s) < 0) return -1;
        for (f = YBOX_XID; f <= YBOX_FAMILY_LAST; f++)
            if (box_ieq(s, ybox_family_name(f))) return f;
    } else {
        PyErr_SetString(PyExc_TypeError, "family: a name (\"xid\", \"line\", ...) or a family constant");
        return -1;
    }
    PyErr_SetString(BoxArgumentError, "family: not one of xid, line, photo, triggerbox, biosemi, mmbts, lines, parallel");
    return -1;
}

static const char* const box_op_names[] = { NULL, "set", "pulse", "width", "close" };

static int box_op(PyObject* o) {
    if (PyLong_Check(o)) {
        long v = PyLong_AsLong(o);
        if (v == -1 && PyErr_Occurred()) {
            if (!PyErr_ExceptionMatches(PyExc_OverflowError)) return -1;
            PyErr_Clear();
        } else if (v >= YBOX_OP_SET && v <= YBOX_OP_CLOSE) {
            return (int)v;
        }
    } else if (PyUnicode_Check(o)) {
        char s[16];
        int i;
        if (box_utf8(o, s, sizeof s) < 0) return -1;
        for (i = YBOX_OP_SET; i <= YBOX_OP_CLOSE; i++)
            if (box_ieq(s, box_op_names[i])) return i;
    } else {
        PyErr_SetString(PyExc_TypeError, "op: \"set\", \"pulse\", \"width\", \"close\" or an OP_* constant");
        return -1;
    }
    PyErr_SetString(BoxArgumentError, "op: not one of set, pulse, width, close");
    return -1;
}

/* An unsigned integer in [0, max]; a bool or a float is refused, since a
 * truncated 2.7 would be another code. */
static int box_uint(PyObject* o, const char* what, unsigned long long max, unsigned long long* out) {
    unsigned long long v;
    if (!PyLong_Check(o) || PyBool_Check(o)) {
        PyErr_Format(PyExc_TypeError, "%s: an int", what);
        return -1;
    }
    v = PyLong_AsUnsignedLongLong(o);
    if (v == (unsigned long long)-1 && PyErr_Occurred()) {
        if (!PyErr_ExceptionMatches(PyExc_OverflowError)) return -1;
        PyErr_Clear();
        PyErr_Format(BoxArgumentError, "%s: 0 to %llu", what, max);
        return -1;
    }
    if (v > max) {
        PyErr_Format(BoxArgumentError, "%s: 0 to %llu", what, max);
        return -1;
    }
    *out = v;
    return 0;
}

/* --- events ------------------------------------------------------------------- */

static PyStructSequence_Field box_event_fields[] = {
    { "t", "seconds on the ysp_rt clock (here: the read time given to decode())" },
    { "t_ns", "t as int ns, exact" },
    { "device", "the instance within the kind" },
    { "control", "the button, key or channel" },
    { "code", "XID: the port; else yours" },
    { "mods", "keyboard modifiers" },
    { "kind", "KIND_*" },
    { "type", "PRESS, RELEASE, SAMPLE, PROXIMITY" },
    { "flags", "REPEAT, SYNTHETIC, ERASER, UNLISTED, DEVTICKS bits" },
    { "stamp", "the stamp's tier (TIER_*); 0 = the source table's" },
    { "unc", "the event's own uncertainty, seconds; 0 = the source's" },
    { "x", "position x" },
    { "y", "position y" },
    { "value", "1 press, 0 release; a sample's value" },
    { "aux", "yours" },
    { "ticks", "with DEVTICKS: the device clock's count, low 32 bits" },
    { NULL, NULL }
};

static PyStructSequence_Desc box_event_desc = {
    "ysp.box.Event",
    "One input event (yin_event of ysp/input.h); times in seconds, t_ns the exact ns.",
    box_event_fields, 16
};

static PyObject* box_event(const yin_event* e) {
    PyObject* o = PyStructSequence_New(EventType);
    PyObject* v[16];
    int i, bad = 0;
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
    v[10] = PyFloat_FromDouble((double)e->unc_us / 1e6);
    v[11] = PyFloat_FromDouble((double)e->x);
    v[12] = PyFloat_FromDouble((double)e->y);
    v[13] = PyFloat_FromDouble((double)e->value);
    v[14] = PyFloat_FromDouble((double)e->aux);
    v[15] = PyLong_FromUnsignedLong((unsigned long)e->ticks);
    for (i = 0; i < 16; i++) {
        if (!v[i]) bad = 1;
        PyStructSequence_SetItem(o, i, v[i]);   /* steals; a NULL slot is freed as none */
    }
    if (bad) { Py_DECREF(o); return NULL; }
    return o;
}

/* --- the decoder -------------------------------------------------------------- */

typedef struct {
    PyObject_HEAD
    ybox_decoder d;
    ybox_out out;
    int ready;
    yin_event ev[BOX_CAP];
    ybox_pair pair[BOX_CAP];
} DecObject;

static int dec_setup(DecObject* o, int family, unsigned long long kind, unsigned long long device) {
    if (ybox_init(&o->d, family, (uint8_t)kind, (uint32_t)device) < 0) {
        PyErr_SetString(BoxArgumentError, ybox_strerror(YBOX_ERR_ARG));
        return -1;
    }
    memset(&o->out, 0, sizeof o->out);
    o->out.ev = o->ev;
    o->out.ev_cap = BOX_CAP;
    o->out.pair = o->pair;
    o->out.pair_cap = BOX_CAP;
    o->ready = 1;
    return 0;
}

static int Dec_init(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "family", "kind", "device", NULL };
    PyObject *fo, *ko = NULL, *dvo = NULL;
    unsigned long long kind = YIN_KIND_BOX, device = 0;
    int family;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|OO", kwlist, &fo, &ko, &dvo)) return -1;
    if ((family = box_family(fo)) < 0) return -1;
    if (ko && ko != Py_None && box_uint(ko, "kind", 255, &kind) < 0) return -1;
    if (dvo && dvo != Py_None && box_uint(dvo, "device", 0xFFFFFFFFull, &device) < 0) return -1;
    return dec_setup((DecObject*)self, family, kind, device);
}

static void Dec_dealloc(PyObject* self) {
    PyTypeObject* tp = Py_TYPE(self);
    freefunc f = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    f(self);
    Py_DECREF(tp);
}

static int Dec_ready(DecObject* o) {
    if (!o->ready) { PyErr_SetString(BoxError, "the decoder is not initialized"); return 0; }
    return 1;
}

static int box_append(PyObject* list, PyObject* item) {
    int rc;
    if (!item) return -1;
    rc = PyList_Append(list, item);
    Py_DECREF(item);
    return rc;
}

/* Every byte of data through ybox_decode(), in calls that each fill the
 * object's arrays at most; the results of each call are appended. */
static PyObject* dec_run(DecObject* o, PyObject* data, PyObject* to) {
    PyObject *b, *evs = NULL, *pairs = NULL, *outs = NULL, *texts = NULL, *res = NULL;
    const uint8_t* p;
    Py_ssize_t n;
    size_t used = 0;
    unsigned long long garbage = 0;
    int64_t t = 0;
    if (to && to != Py_None && box_s_to_ns(to, "t", &t) < 0) return NULL;
    if (PyUnicode_Check(data)) {
        /* bytes(str) needs an encoding; a capture is bytes */
        PyErr_SetString(PyExc_TypeError, "data: bytes, bytearray, memoryview or a sequence of ints 0..255, not str");
        return NULL;
    }
    if (!(b = PyBytes_FromObject(data))) return NULL;   /* bytes: the same object; else a copy */
    p = (const uint8_t*)PyBytes_AsString(b);
    n = PyBytes_Size(b);
    evs = PyList_New(0);
    pairs = PyList_New(0);
    outs = PyList_New(0);
    texts = PyList_New(0);
    if (!evs || !pairs || !outs || !texts) goto done;
    while (used < (size_t)n) {
        int i;
        size_t r;
        ybox_out_clear(&o->out);
        r = ybox_decode(&o->d, p + used, (size_t)n - used, t, &o->out);
        if (r == 0) { PyErr_SetString(BoxError, "the decoder used no byte"); goto done; }
        used += r;
        garbage += o->out.garbage;
        for (i = 0; i < o->out.n_ev; i++)
            if (box_append(evs, box_event(&o->ev[i])) < 0) goto done;
        for (i = 0; i < o->out.n_pair; i++)
            if (box_append(pairs, PyObject_CallFunction(PairType, "Kk", (unsigned long long)o->pair[i].ticks,
                                                        (unsigned long)o->pair[i].probe)) < 0)
                goto done;
        if (o->out.has_out &&
            box_append(outs, PyObject_CallFunction(OutputType, "Kk", (unsigned long long)o->out.out_ticks,
                                                   (unsigned long)o->out.out_code)) < 0)
            goto done;
        /* printable ASCII by the header's line rule, so the decode cannot fail */
        if (o->out.has_text && box_append(texts, PyUnicode_DecodeASCII(o->out.text, (Py_ssize_t)strlen(o->out.text), "replace")) < 0)
            goto done;
    }
    res = PyObject_CallFunction(DecodedType, "OOOOK", evs, pairs, outs, texts, garbage);
done:
    Py_DECREF(b);
    Py_XDECREF(evs);
    Py_XDECREF(pairs);
    Py_XDECREF(outs);
    Py_XDECREF(texts);
    return res;
}

static PyObject* Dec_decode(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "data", "t", NULL };
    PyObject *data, *to = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|O", kwlist, &data, &to)) return NULL;
    if (!Dec_ready((DecObject*)self)) return NULL;
    return dec_run((DecObject*)self, data, to);
}

static PyObject* Dec_reset(PyObject* self, PyObject* unused) {
    DecObject* o = (DecObject*)self;
    (void)unused;
    if (!Dec_ready(o)) return NULL;
    if (dec_setup(o, o->d.family, o->d.kind, o->d.device) < 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject* Dec_get_family(PyObject* s, void* c) {
    (void)c;
    return Dec_ready((DecObject*)s) ? PyUnicode_FromString(ybox_family_name(((DecObject*)s)->d.family)) : NULL;
}
static PyObject* Dec_get_kind(PyObject* s, void* c) {
    (void)c;
    return Dec_ready((DecObject*)s) ? PyLong_FromLong(((DecObject*)s)->d.kind) : NULL;
}
static PyObject* Dec_get_device(PyObject* s, void* c) {
    (void)c;
    return Dec_ready((DecObject*)s) ? PyLong_FromUnsignedLong(((DecObject*)s)->d.device) : NULL;
}
static PyObject* Dec_get_frames(PyObject* s, void* c) {
    (void)c;
    return Dec_ready((DecObject*)s) ? PyLong_FromUnsignedLongLong(((DecObject*)s)->d.frames) : NULL;
}
static PyObject* Dec_get_garbage(PyObject* s, void* c) {
    (void)c;
    return Dec_ready((DecObject*)s) ? PyLong_FromUnsignedLongLong(((DecObject*)s)->d.garbage) : NULL;
}
static PyObject* Dec_get_pending(PyObject* s, void* c) {
    (void)c;
    return Dec_ready((DecObject*)s) ? PyLong_FromLong(((DecObject*)s)->d.n) : NULL;
}

static PyMethodDef Dec_methods[] = {
    { "decode", (PyCFunction)(void (*)(void))Dec_decode, METH_VARARGS | METH_KEYWORDS,
      "decode(data, t=0.0) -> Decoded(events, pairs, outputs, text, garbage): every byte of data, read at "
      "time t (seconds). A frame split across calls is kept until its rest comes." },
    { "reset", Dec_reset, METH_NOARGS, "reset(): forget a partial frame and zero the counts (ybox_init again)." },
    { NULL }
};

static PyGetSetDef Dec_getset[] = {
    { "family", Dec_get_family, NULL, "the family's name", NULL },
    { "kind", Dec_get_kind, NULL, "Event.kind of every event", NULL },
    { "device", Dec_get_device, NULL, "Event.device of every event", NULL },
    { "frames", Dec_get_frames, NULL, "frames decoded since init or reset", NULL },
    { "garbage", Dec_get_garbage, NULL, "bytes that fit no frame since init or reset", NULL },
    { "pending", Dec_get_pending, NULL, "bytes of a partial frame or line held for the next call", NULL },
    { NULL }
};

static PyType_Slot Dec_slots[] = {
    { Py_tp_init, (void*)Dec_init }, { Py_tp_dealloc, (void*)Dec_dealloc }, { Py_tp_methods, Dec_methods },
    { Py_tp_getset, Dec_getset }, { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_doc, (void*)"Decoder(family, kind=KIND_BOX, device=0): a ybox_decoder. family: a name (\"xid\", "
                        "\"line\", \"photo\", ...) or a family constant; kind and device go into every event." },
    { 0, NULL }
};

static PyType_Spec Dec_spec = { "ysp.box.Decoder", sizeof(DecObject), 0, Py_TPFLAGS_DEFAULT, Dec_slots };
static PyObject* DecType;

/* --- module functions --------------------------------------------------------- */

static PyObject* mod_decode(PyObject* m, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "family", "data", "t", "kind", "device", NULL };
    PyObject *fo, *data, *to = NULL, *ko = NULL, *dvo = NULL, *dec = NULL, *res, *kwd, *pa;
    (void)m;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|OOO", kwlist, &fo, &data, &to, &ko, &dvo)) return NULL;
    kwd = PyDict_New();
    pa = PyTuple_Pack(1, fo);
    if (kwd && pa && (!ko || PyDict_SetItemString(kwd, "kind", ko) == 0) &&
        (!dvo || PyDict_SetItemString(kwd, "device", dvo) == 0))
        dec = PyObject_Call(DecType, pa, kwd);
    Py_XDECREF(kwd);
    Py_XDECREF(pa);
    if (!dec) return NULL;
    res = dec_run((DecObject*)dec, data, to);
    Py_DECREF(dec);
    return res;
}

static PyObject* mod_family_name(PyObject* m, PyObject* arg) {
    int f = box_family(arg);
    (void)m;
    return f < 0 ? NULL : PyUnicode_FromString(ybox_family_name(f));
}

static PyObject* mod_caps(PyObject* m, PyObject* arg) {
    int f = box_family(arg);
    (void)m;
    return f < 0 ? NULL : PyLong_FromUnsignedLong((unsigned long)ybox_caps(f));
}

static PyObject* mod_code_max(PyObject* m, PyObject* arg) {
    int f = box_family(arg);
    (void)m;
    return f < 0 ? NULL : PyLong_FromUnsignedLong((unsigned long)ybox_code_max(f));
}

static PyObject* mod_fixed_pulse(PyObject* m, PyObject* arg) {
    int f = box_family(arg);
    (void)m;
    return f < 0 ? NULL : PyFloat_FromDouble((double)ybox_fixed_pulse_ns(f) / 1e9);
}

static PyObject* mod_default_baud(PyObject* m, PyObject* arg) {
    int f = box_family(arg);
    (void)m;
    return f < 0 ? NULL : PyLong_FromUnsignedLong((unsigned long)ybox_default_baud(f));
}

static PyObject* mod_tick(PyObject* m, PyObject* arg) {
    int f = box_family(arg);
    (void)m;
    return f < 0 ? NULL : PyFloat_FromDouble(ybox_ns_per_tick(f) / 1e9);
}

static PyObject* mod_slow_write(PyObject* m, PyObject* arg) {
    int f = box_family(arg);
    (void)m;
    if (f < 0) return NULL;
    return PyBool_FromLong(ybox_slow_write(f) ? 1 : 0);
}

static PyObject* mod_probe(PyObject* m, PyObject* args) {
    PyObject *fo, *so = NULL;
    unsigned long long seq = 1;
    uint8_t buf[32];
    int f, n;
    (void)m;
    if (!PyArg_ParseTuple(args, "O|O", &fo, &so)) return NULL;
    if ((f = box_family(fo)) < 0) return NULL;
    if (so && box_uint(so, "seq", 0xFFFFFFFFull, &seq) < 0) return NULL;
    n = ybox_probe(f, (uint32_t)seq, buf, (int)sizeof buf);
    return PyBytes_FromStringAndSize((const char*)buf, n > 0 ? n : 0);
}

static PyObject* mod_identify(PyObject* m, PyObject* arg) {
    uint8_t buf[16];
    int f = box_family(arg), n;
    (void)m;
    if (f < 0) return NULL;
    n = ybox_identify(f, buf, (int)sizeof buf);
    return PyBytes_FromStringAndSize((const char*)buf, n > 0 ? n : 0);
}

static PyObject* mod_encode(PyObject* m, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "family", "op", "code", "width", NULL };
    PyObject *fo, *oo, *co = NULL, *wo = NULL;
    unsigned long long code = 0;
    int64_t width = 0;
    uint8_t buf[64];
    int f, op, n;
    (void)m;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|OO", kwlist, &fo, &oo, &co, &wo)) return NULL;
    if ((f = box_family(fo)) < 0 || (op = box_op(oo)) < 0) return NULL;
    if (co && box_uint(co, "code", 0xFFFFFFFFull, &code) < 0) return NULL;
    if (wo && wo != Py_None && box_s_to_ns(wo, "width", &width) < 0) return NULL;
    n = ybox_encode(f, op, (uint32_t)code, width, buf, (int)sizeof buf);
    if (n < 0) {
        PyErr_Format(BoxArgumentError,
                     "encode: %s (a family without outputs, a code above code_max() = %lu, or a width the "
                     "protocol cannot carry)",
                     ybox_strerror(n), (unsigned long)ybox_code_max(f));
        return NULL;
    }
    return PyBytes_FromStringAndSize((const char*)buf, n);
}

static PyObject* mod_version(PyObject* m, PyObject* unused) {
    (void)m; (void)unused;
    return PyUnicode_FromString(ybox_version());
}

static PyMethodDef module_methods[] = {
    { "decode", (PyCFunction)(void (*)(void))mod_decode, METH_VARARGS | METH_KEYWORDS,
      "decode(family, data, t=0.0, kind=KIND_BOX, device=0) -> Decoded: a whole capture through a new Decoder." },
    { "family_name", mod_family_name, METH_O, "family_name(family) -> str" },
    { "caps", mod_caps, METH_O, "caps(family) -> CAP_* bits: what the family does." },
    { "code_max", mod_code_max, METH_O, "code_max(family) -> the highest output code (0 without outputs)." },
    { "fixed_pulse", mod_fixed_pulse, METH_O, "fixed_pulse(family) -> seconds: the width of a CAP_FIXED family's pulses, else 0." },
    { "default_baud", mod_default_baud, METH_O, "default_baud(family) -> the baud rate the device expects." },
    { "tick", mod_tick, METH_O, "tick(family) -> seconds: the nominal unit of the device clock (ticks)." },
    { "slow_write", mod_slow_write, METH_O, "slow_write(family) -> bool: write commands one byte at a time, SLOW_WRITE apart." },
    { "probe", mod_probe, METH_VARARGS, "probe(family, seq=1) -> bytes of a timer query (b'' when the family has none; LINE needs seq 1..2^32-1)." },
    { "identify", mod_identify, METH_O, "identify(family) -> bytes of an identity query (b'' when none)." },
    { "encode", (PyCFunction)(void (*)(void))mod_encode, METH_VARARGS | METH_KEYWORDS,
      "encode(family, op, code=0, width=0.0) -> bytes that put code on the outputs; op \"set\", \"pulse\", "
      "\"width\" or \"close\"; width in seconds (pulse, width). b'' when the family has no bytes for the op." },
    { "version", mod_version, METH_NOARGS, "version() -> the header's version string." },
    { NULL }
};

static PyModuleDef ysp_box_module = {
    PyModuleDef_HEAD_INIT, "ysp.box",
    "Response and trigger box protocols (ysp/box.h): bytes to events and clock pairs, and output bytes.", -1,
    module_methods, NULL, NULL, NULL, NULL,
};

static PyObject* box_add_exc(PyObject* m, const char* qual, const char* attr, PyObject* base) {
    PyObject* e = PyErr_NewException(qual, base, NULL);
    if (!e) return NULL;
    Py_INCREF(e);
    if (PyModule_AddObject(m, attr, e) < 0) { Py_DECREF(e); Py_DECREF(e); return NULL; }
    return e;
}

static PyObject* box_namedtuple(PyObject* m, const char* name, const char* fields) {
    PyObject *coll, *nt, *a, *k, *t;
    if (!(coll = PyImport_ImportModule("collections"))) return NULL;
    nt = PyObject_GetAttrString(coll, "namedtuple");
    a = Py_BuildValue("(ss)", name, fields);
    k = Py_BuildValue("{s:s}", "module", "ysp.box");
    t = (nt && a && k) ? PyObject_Call(nt, a, k) : NULL;
    Py_XDECREF(nt); Py_XDECREF(a); Py_XDECREF(k); Py_DECREF(coll);
    if (!t) return NULL;
    Py_INCREF(t);
    if (PyModule_AddObject(m, name, t) < 0) { Py_DECREF(t); Py_DECREF(t); return NULL; }
    return t;
}

PyMODINIT_FUNC PyInit_box(void) {
    PyObject *m, *base2;
    int i;
    static const struct { const char* name; long v; } consts[] = {
        { "XID", YBOX_XID }, { "LINE", YBOX_LINE }, { "PHOTO", YBOX_PHOTO }, { "TRIGGERBOX", YBOX_TRIGGERBOX },
        { "BIOSEMI", YBOX_BIOSEMI }, { "MMBTS", YBOX_MMBTS }, { "LINES", YBOX_LINES }, { "PARALLEL", YBOX_PARALLEL },
        { "CAP_IN", YBOX_CAP_IN }, { "CAP_OUT", YBOX_CAP_OUT }, { "CAP_LINES", YBOX_CAP_LINES },
        { "CAP_PULSE", YBOX_CAP_PULSE }, { "CAP_FIXED", YBOX_CAP_FIXED }, { "CAP_IDENT", YBOX_CAP_IDENT },
        { "OP_SET", YBOX_OP_SET }, { "OP_PULSE", YBOX_OP_PULSE }, { "OP_WIDTH", YBOX_OP_WIDTH },
        { "OP_CLOSE", YBOX_OP_CLOSE },
        /* ysp/input.h: the event's kinds, types, flags and tiers */
        { "KIND_KEYBOARD", YIN_KIND_KEYBOARD }, { "KIND_MOUSE", YIN_KIND_MOUSE }, { "KIND_TOUCH", YIN_KIND_TOUCH },
        { "KIND_PEN", YIN_KIND_PEN }, { "KIND_GAMEPAD", YIN_KIND_GAMEPAD }, { "KIND_BOX", YIN_KIND_BOX },
        { "KIND_EYE", YIN_KIND_EYE }, { "KIND_SYNC", YIN_KIND_SYNC }, { "KIND_USER", YIN_KIND_USER },
        { "PRESS", YIN_PRESS }, { "RELEASE", YIN_RELEASE }, { "SAMPLE", YIN_SAMPLE }, { "PROXIMITY", YIN_PROXIMITY },
        { "REPEAT", YIN_REPEAT }, { "SYNTHETIC", YIN_SYNTHETIC }, { "ERASER", YIN_ERASER },
        { "UNLISTED", YIN_UNLISTED }, { "DEVTICKS", YIN_DEVTICKS },
        { "TIER_UNKNOWN", YIN_TIER_UNKNOWN }, { "TIER_1", YIN_TIER_1 }, { "TIER_2", YIN_TIER_2 },
        { "TIER_3", YIN_TIER_3 }, { "TIER_SIM", YIN_TIER_SIM },
    };
    m = PyModule_Create(&ysp_box_module);
    if (!m) return NULL;
    if (!(DecType = PyType_FromSpec(&Dec_spec))) goto fail;
    Py_INCREF(DecType);
    if (PyModule_AddObject(m, "Decoder", DecType) < 0) { Py_DECREF(DecType); goto fail; }
    if (!(EventType = PyStructSequence_NewType(&box_event_desc))) goto fail;
    Py_INCREF((PyObject*)EventType);
    if (PyModule_AddObject(m, "Event", (PyObject*)EventType) < 0) { Py_DECREF((PyObject*)EventType); goto fail; }
    if (!(BoxError = box_add_exc(m, "ysp.box.Error", "Error", NULL))) goto fail;
    base2 = PyTuple_Pack(2, BoxError, PyExc_ValueError);
    if (!base2) goto fail;
    BoxArgumentError = box_add_exc(m, "ysp.box.ArgumentError", "ArgumentError", base2);
    Py_DECREF(base2);
    if (!BoxArgumentError) goto fail;
    if (!(PairType = box_namedtuple(m, "Pair", "ticks probe"))) goto fail;
    if (!(OutputType = box_namedtuple(m, "Output", "ticks code"))) goto fail;
    if (!(DecodedType = box_namedtuple(m, "Decoded", "events pairs outputs text garbage"))) goto fail;
    for (i = 0; i < (int)(sizeof consts / sizeof consts[0]); i++)
        if (PyModule_AddIntConstant(m, consts[i].name, consts[i].v) < 0) goto fail;
    if (PyModule_AddObject(m, "UNC_MAX", PyFloat_FromDouble((double)YIN_UNC_MAX / 1e6)) < 0) goto fail;
    if (PyModule_AddObject(m, "SLOW_WRITE", PyFloat_FromDouble((double)YBOX_SLOW_WRITE_NS / 1e9)) < 0) goto fail;
    if (PyModule_AddStringConstant(m, "__version__", BOX_VERSION) < 0) goto fail;
    return m;
fail:
    Py_DECREF(m);
    return NULL;
}
