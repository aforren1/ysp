/* ysp_timeline_ext.c - CPython extension wrapping ysp/timeline.h (module ysp.timeline)
 *
 * The binding a PsychoPy experiment uses when it draws its own stimuli and
 * wants the timeline's timing: build a trial (events, channels, keyframes,
 * tweens with eases, sequences, op tables), then each frame advance it to
 * the frame's predicted onset and read the channel values, the fields and
 * the events that fired, with their landing records.
 *
 *     import ysp.timeline as ytl
 *     tl = ytl.Timeline(["fix", "grating", "contrast"])
 *     q = tl.seq(1)                                        # base 1: the trial
 *     q.on("fix").wait(0.5).off("fix").on("grating").trigger(12)
 *     q.to("contrast", 0.5, duration=0.1, ease="cosine")
 *     q.wait(0.6).then("contrast", 0.0, duration=0.1, ease="cosine").off("grating")
 *     tl.anchor(1, first_onset)                            # trial time 0
 *     fired = tl.evaluate(onset, period, frame)            # each frame
 *     v = tl.values                                        # float32 view, n_channels
 *
 * TIMES: seconds (double), as every binding (docs/bindings.md). The header
 * counts int64 ns; each time converts once at the boundary, to the nearest
 * ns (tl_s_to_ns), and a negative or NaN time is refused there. Landing
 * records carry the exact ns beside the seconds.
 *
 * FIELDS: target tweens write C memory. The binding owns that memory: a
 * Field is a slot of four 32-bit words in an array that the Timeline
 * allocates once, so its address never moves, and Timeline.field_values is
 * a view of the whole array. That array and the handle (which holds the
 * channel values) are bytearrays, so a view the caller keeps holds them
 * alive after close() or the Timeline's end.
 *
 * THREADING: none. The header is pure computation and a frame costs
 * microseconds, so every call keeps the GIL (the callbacks need it).
 *
 * MEMORY: no allocation per frame when nothing fires: evaluate() fills a
 * report buffer the Timeline allocated at open (one entry per event of the
 * storage, so the report is never cut) and returns the empty tuple.
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x03080000
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

/* The header's default is 256. A Python experiment with one channel per
 * dot or per element of a grid needs more; the handle grows by 264 bytes a
 * channel (about 270 KB here), and an evaluate scans only n_channels. */
#define YTL_MAX_CHANNELS 1024
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>


/* The distribution's version is the header's; a change to this binding
 * alone adds a fourth number. tests/test_timeline.py checks this against
 * pyproject.toml. */
#define TL_VERSION YTL_VERSION_STRING

/* Seconds at or above this are refused: the header's range is |t| < 2^62
 * ns (TIME), and 4611686018 s is the largest whole second below it. */
#define TL_S_MAX 4611686018.0

static PyObject* TlError;
static PyObject* TlArgumentError;
static PyObject* TlClosedError;
static PyObject* TlOrderError;
static PyObject* TlFullError;
static PyObject* TlBoundError;
static PyObject* TlNotFoundError;
static PyObject* EventType;   /* namedtuple Event(...) */
static PyObject* SeqType;
static PyObject* FieldType;

/* --- times ------------------------------------------------------------------ */

/* Seconds to the nearest ns. s * 1e9 rounds once; fma() gives that
 * rounding's error exactly, so the result is the nearest ns to the real
 * product (the same conversion as ysp.rdk). */
static int tl_s_to_ns(double s, const char* what, int64_t* out) {
    volatile double p;   /* stored as a double: x87 (i686) would keep 80 bits */
    double e, n, r;
    if (!(s >= 0.0 && s < TL_S_MAX)) {
        PyErr_Format(TlArgumentError, "%s: seconds, >= 0 and below 2^62 ns (a negative or NaN time is refused)", what);
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

static int tl_obj_to_ns(PyObject* o, const char* what, int64_t* out) {
    double s = PyFloat_AsDouble(o);
    if (s == -1.0 && PyErr_Occurred()) return -1;
    return tl_s_to_ns(s, what, out);
}

static double tl_ns_to_s(int64_t ns) { return (double)ns / 1e9; }

static PyObject* tl_time_out(bool ok, int64_t ns) {
    if (!ok) Py_RETURN_NONE;
    return PyFloat_FromDouble(tl_ns_to_s(ns));
}

/* --- objects ---------------------------------------------------------------- */

struct TimelineObject;

/* The handle first, so a callback's ytl_timeline* leads back to its owner. */
typedef struct tl_box {
    ytl_timeline tl;
    struct TimelineObject* owner;
} tl_box;

/* A channel or field written into a Python attribute after each evaluate. */
typedef struct tl_bindrow {
    PyObject* obj;
    PyObject* name;
    PyObject* field;   /* a FieldObject, or NULL for a channel */
    int       ch;
    int       comp;    /* -1: every component (a tuple when n > 1) */
} tl_bindrow;

typedef struct TimelineObject {
    PyObject_HEAD
    tl_box*          box;
    int              open;
    int              n_channels;
    ytl_event*       events;
    int              ecap;
    ytl_event*       fired;      /* ecap entries: the report is never cut */
    ytl_key*         arena;
    int              kcap;
    ytl_target_slot* tg;
    ytl_tween_slot*  tw;
    int              twcap;
    float*           fields;     /* 4 words per field slot */
    uint8_t*         fused;
    int              fcap;
    void**           cbuf;       /* per channel: the keys, curves or samples
                                  * the header points at */
    int8_t*          cbase;      /* the base each cbuf was set on */
    PyObject*        names;      /* dict name -> index, or NULL */
    PyObject*        names_t;    /* tuple of names, or NULL */
    PyObject*        box_ba;     /* the bytearrays that hold the handle and the */
    PyObject*        fields_ba;  /* field slots: a view keeps its array alive  */
    PyObject*        values_mv;
    PyObject*        fields_mv;
    PyObject*        callbacks;  /* dict tween id -> (on_start, on_complete) */
    PyObject*        field_list; /* every live Field: a field whose variable
                                  * goes out of scope keeps its tweens */
    PyObject*        cb_type, *cb_value, *cb_tb;  /* the first callback error */
    tl_bindrow*      binds;
    int              n_binds, binds_cap;
    int64_t          frames;     /* the default frame index */
} TimelineObject;

typedef struct SeqObject {
    PyObject_HEAD
    PyObject* owner;
    ytl_seq   q;
    int       last_id;
} SeqObject;

typedef struct FieldObject {
    PyObject_HEAD
    PyObject* owner;
    int       index;
    int       n;
    int       type;
} FieldObject;

#define TL_OF(o) ((TimelineObject*)(o))

/* --- errors ----------------------------------------------------------------- */

static PyObject* tl_exc(int code) {
    switch (code) {
    case YTL_ERR_ARG: return TlArgumentError;
    case YTL_ERR_CLOSED: return TlClosedError;
    case YTL_ERR_ORDER: return TlOrderError;
    case YTL_ERR_FULL: return TlFullError;
    case YTL_ERR_BOUND: return TlBoundError;
    case YTL_ERR_NOT_FOUND: return TlNotFoundError;
    default: return TlError;
    }
}

static PyObject* tl_fail(TimelineObject* o, int code) {
    const char* msg = o && o->box ? ytl_error(&o->box->tl) : "";
    PyErr_SetString(tl_exc(code), msg && msg[0] ? msg : ytl_strerror(code));
    return NULL;
}

/* A failure with the op's index as `index` on the exception. */
static PyObject* tl_fail_at(TimelineObject* o, int code, int index) {
    PyObject *exc = tl_exc(code), *inst;
    const char* msg = ytl_error(&o->box->tl);
    inst = PyObject_CallFunction(exc, "s", msg && msg[0] ? msg : ytl_strerror(code));
    if (!inst) return NULL;
    {
        PyObject* ix = PyLong_FromLong(index);
        int rc = ix ? PyObject_SetAttrString(inst, "index", ix) : -1;
        Py_XDECREF(ix);
        if (rc < 0) { Py_DECREF(inst); return NULL; }
    }
    PyErr_SetObject(exc, inst);
    Py_DECREF(inst);
    return NULL;
}

static int tl_ready(TimelineObject* o) {
    if (!o || !o->open || !o->box) {
        PyErr_SetString(TlClosedError, "ysp_timeline: the timeline is closed");
        return 0;
    }
    return 1;
}

/* --- names ------------------------------------------------------------------ */

/* A str's UTF-8 into buf (PyUnicode_AsUTF8 is Limited API only from 3.10). */
static int tl_utf8(PyObject* o, char* buf, size_t cap) {
    PyObject* b = PyUnicode_AsUTF8String(o);
    if (!b) return -1;
    PyOS_snprintf(buf, cap, "%s", PyBytes_AsString(b));
    Py_DECREF(b);
    return 0;
}

/* Lower case, '-' and ' ' as '_'. */
static void tl_norm(char* s) {
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
        else if (*s == '-' || *s == ' ') *s = '_';
    }
}

typedef struct tl_name { const char* name; int value; } tl_name;

/* The header's names, then GSAP's for the same curves. */
static const tl_name EASES[] = {
    { "linear", YTL_EASE_LINEAR }, { "step", YTL_EASE_STEP }, { "cosine", YTL_EASE_COSINE },
    { "quad_in", YTL_EASE_QUAD_IN }, { "quad_out", YTL_EASE_QUAD_OUT }, { "log", YTL_EASE_LOG },
    { "bezier", YTL_EASE_BEZIER }, { "quad_in_out", YTL_EASE_QUAD_IN_OUT }, { "cubic_in", YTL_EASE_CUBIC_IN },
    { "cubic_out", YTL_EASE_CUBIC_OUT }, { "cubic_in_out", YTL_EASE_CUBIC_IN_OUT },
    { "none", YTL_EASE_LINEAR }, { "sine.inout", YTL_EASE_COSINE },
    { "power1.in", YTL_EASE_QUAD_IN }, { "power1.out", YTL_EASE_QUAD_OUT }, { "power1.inout", YTL_EASE_QUAD_IN_OUT },
    { "quad.in", YTL_EASE_QUAD_IN }, { "quad.out", YTL_EASE_QUAD_OUT }, { "quad.inout", YTL_EASE_QUAD_IN_OUT },
    { "power2.in", YTL_EASE_CUBIC_IN }, { "power2.out", YTL_EASE_CUBIC_OUT }, { "power2.inout", YTL_EASE_CUBIC_IN_OUT },
    { "cubic.in", YTL_EASE_CUBIC_IN }, { "cubic.out", YTL_EASE_CUBIC_OUT }, { "cubic.inout", YTL_EASE_CUBIC_IN_OUT },
    { NULL, 0 }
};
static const tl_name INTERPS[] = { { "linear", YTL_INTERP_LINEAR }, { "step", YTL_INTERP_STEP },
                                   { "cubic", YTL_INTERP_CUBIC }, { NULL, 0 } };
static const tl_name KINDS[] = { { "mark", YTL_MARK }, { "trigger", YTL_TRIGGER }, { "onset", YTL_ONSET },
                                 { "offset", YTL_OFFSET }, { "set", YTL_SET }, { NULL, 0 } };
static const tl_name OPS[] = { { "on", YTL_OP_ON }, { "off", YTL_OP_OFF }, { "set", YTL_OP_SET_VALUE },
                               { "set_value", YTL_OP_SET_VALUE }, { "trigger", YTL_OP_TRIGGER },
                               { "mark", YTL_OP_MARK }, { "wait", YTL_OP_WAIT }, { "at", YTL_OP_AT },
                               { "to", YTL_OP_TO }, { "then", YTL_OP_THEN }, { NULL, 0 } };

/* An enum: an int as it is (the header checks it), or a name. */
static int tl_enum(PyObject* o, const tl_name* names, const char* what, int* out) {
    char s[64];
    int i;
    if (PyLong_Check(o)) {
        long v = PyLong_AsLong(o);
        if (v == -1 && PyErr_Occurred()) return -1;
        if (v < INT32_MIN || v > INT32_MAX) { PyErr_Format(TlArgumentError, "%s: out of range", what); return -1; }
        *out = (int)v;
        return 0;
    }
    if (!PyUnicode_Check(o)) { PyErr_Format(PyExc_TypeError, "%s: a name or an int", what); return -1; }
    if (tl_utf8(o, s, sizeof s) < 0) return -1;
    tl_norm(s);
    for (i = 0; names[i].name; i++)
        if (strcmp(s, names[i].name) == 0) { *out = names[i].value; return 0; }
    PyErr_Format(TlArgumentError, "%s: unknown name '%s'", what, s);
    return -1;
}

/* An ease: None (LINEAR), a name or an int, or (x1, y1, x2, y2), which is
 * BEZIER with that curve (CSS cubic-bezier()). */
static int tl_ease(PyObject* o, int* ease, ytl_curve* curve) {
    memset(curve, 0, sizeof *curve);
    *ease = YTL_EASE_LINEAR;
    if (!o || o == Py_None) return 0;
    if (PyTuple_Check(o) || PyList_Check(o)) {
        double v[4];
        int i;
        if (PySequence_Size(o) != 4) { PyErr_SetString(TlArgumentError, "ease: a name, or (x1, y1, x2, y2)"); return -1; }
        for (i = 0; i < 4; i++) {
            PyObject* x = PySequence_GetItem(o, i);
            if (!x) return -1;
            v[i] = PyFloat_AsDouble(x);
            Py_DECREF(x);
            if (v[i] == -1.0 && PyErr_Occurred()) return -1;
        }
        curve->x1 = (float)v[0]; curve->y1 = (float)v[1]; curve->x2 = (float)v[2]; curve->y2 = (float)v[3];
        *ease = YTL_EASE_BEZIER;
        return 0;
    }
    return tl_enum(o, EASES, "ease", ease);
}

/* A channel: an int, or a name given to Timeline(). */
static int tl_chan(TimelineObject* o, PyObject* c, int* out) {
    if (PyLong_Check(c)) {
        long v = PyLong_AsLong(c);
        if (v == -1 && PyErr_Occurred()) return -1;
        if (v < INT32_MIN || v > INT32_MAX) { PyErr_SetString(TlArgumentError, "channel: out of range"); return -1; }
        *out = (int)v;
        return 0;
    }
    if (PyUnicode_Check(c) && o->names) {
        PyObject* v = PyDict_GetItem(o->names, c);
        if (v) { *out = (int)PyLong_AsLong(v); return 0; }
        PyErr_Format(TlArgumentError, "unknown channel %R", c);
        return -1;
    }
    PyErr_SetString(PyExc_TypeError, "a channel is an int or a name given to Timeline()");
    return -1;
}

static int tl_float(PyObject* x, float* out, const char* what) {
    double d = PyFloat_AsDouble(x);
    if (d == -1.0 && PyErr_Occurred()) return -1;
    (void)what;
    *out = (float)d;
    return 0;
}

static int tl_int32(PyObject* x, int32_t* out, const char* what) {
    long v = PyLong_AsLong(x);
    if (v == -1 && PyErr_Occurred()) return -1;
    if (v < INT32_MIN || v > INT32_MAX) { PyErr_Format(TlArgumentError, "%s: out of int32 range", what); return -1; }
    *out = (int32_t)v;
    return 0;
}

/* cycles: an int, or "forever" (YTL_FOREVER). */
static int tl_cycles(PyObject* x, int* out) {
    if (PyUnicode_Check(x)) {
        char s[16];
        if (tl_utf8(x, s, sizeof s) < 0) return -1;
        tl_norm(s);
        if (strcmp(s, "forever") == 0) { *out = YTL_FOREVER; return 0; }
        PyErr_SetString(TlArgumentError, "cycles: an int, or \"forever\"");
        return -1;
    }
    return tl_int32(x, (int32_t*)out, "cycles");
}

/* --- events out ------------------------------------------------------------- */

static PyObject* tl_event(TimelineObject* o, const ytl_event* e) {
    unsigned long long user = (unsigned long long)e->user;
    /* A set's record carries the field's address; the field's index is what
     * means something here. */
    if (e->kind == YTL_TARGET_SET && o->fields) {
        uintptr_t p = (uintptr_t)e->user, a = (uintptr_t)o->fields;
        if (p >= a && p < a + (uintptr_t)o->fcap * 16u) user = (unsigned long long)((p - a) / 16u);
    }
    return PyObject_CallFunction(EventType, "iiidifiKiLddLLL", (int)e->id, (int)e->kind, (int)e->base,
                                 tl_ns_to_s(e->time_ns), (int)e->target, (double)e->value, (int)e->code, user,
                                 (int)e->flags, (long long)e->frame, tl_ns_to_s(e->onset), tl_ns_to_s(e->residual),
                                 (long long)e->time_ns, (long long)e->onset, (long long)e->residual);
}

static PyObject* tl_event_list(TimelineObject* o, const ytl_event* e, int n) {
    PyObject* l = PyList_New(n);
    int i;
    if (!l) return NULL;
    for (i = 0; i < n; i++) {
        PyObject* x = tl_event(o, &e[i]);
        if (!x) { Py_DECREF(l); return NULL; }
        PyList_SetItem(l, i, x);
    }
    return l;
}

/* --- callbacks -------------------------------------------------------------- */

static void tl_cb(ytl_timeline* tl, int id, int which) {
    TimelineObject* o = ((tl_box*)(void*)tl)->owner;
    PyObject *key, *pair, *fn, *r;
    if (!o || !o->callbacks || o->cb_type) return;   /* one error per evaluate */
    key = PyLong_FromLong(id);
    if (!key) { PyErr_Fetch(&o->cb_type, &o->cb_value, &o->cb_tb); return; }
    pair = PyDict_GetItem(o->callbacks, key);
    Py_DECREF(key);
    if (!pair) return;
    fn = PyTuple_GetItem(pair, which);
    if (!fn || fn == Py_None) return;
    r = PyObject_CallObject(fn, NULL);
    if (!r) PyErr_Fetch(&o->cb_type, &o->cb_value, &o->cb_tb);
    else Py_DECREF(r);
}
static void tl_cb_start(ytl_timeline* tl, int id, void* user) { (void)user; tl_cb(tl, id, 0); }
static void tl_cb_complete(ytl_timeline* tl, int id, void* user) { (void)user; tl_cb(tl, id, 1); }

/* Drop the callables of tweens that can no longer call: a callback runs only
 * when its event fires, so an id with no CALL event left in the storage is
 * done (killed, cleared, cancelled, overwritten or pruned). */
static void tl_sweep(TimelineObject* o) {
    PyObject *live, *keys;
    Py_ssize_t i, n;
    int k;
    if (!o->callbacks || PyDict_Size(o->callbacks) == 0) return;
    live = PySet_New(NULL);
    if (!live) { PyErr_Clear(); return; }
    for (k = 0; k < o->box->tl.n_events; k++) {
        const ytl_event* e = &o->events[k];
        if (e->kind == YTL_CALL_START || e->kind == YTL_CALL_COMPLETE) {
            PyObject* id = PyLong_FromLong(e->code);
            if (id) { PySet_Add(live, id); Py_DECREF(id); }
        }
    }
    keys = PyDict_Keys(o->callbacks);
    n = keys ? PyList_Size(keys) : 0;
    for (i = 0; i < n; i++) {
        PyObject* id = PyList_GetItem(keys, i);
        if (PySet_Contains(live, id) == 0) PyDict_DelItem(o->callbacks, id);
    }
    Py_XDECREF(keys);
    Py_DECREF(live);
    PyErr_Clear();
}

/* --- tween options ------------------------------------------------------------ */

typedef struct tl_tw {
    int64_t   duration, delay, start, stagger;
    int       start_set;
    PyObject* from;
    int       ease;
    ytl_curve curve;
    int       cycles, yoyo, kv;
    int       immediate;   /* -1: the call's default */
    int       overwrite;
    PyObject* on_start;
    PyObject* on_complete;
    unsigned long long user;
} tl_tw;

enum { TW_CHANNEL = 1, TW_FIELD = 2, TW_START = 4 };

static int tl_truth(PyObject* v, int* out) {
    int t = PyObject_IsTrue(v);
    if (t < 0) return -1;
    *out = t;
    return 0;
}

/* The keyword options of a tween, from a dict (method keywords or an op's
 * dict). `allow` says which keys this call reads; any other key is refused,
 * since a misspelled key would leave a default in force without a word. */
static int tl_tw_parse(PyObject* kw, int allow, tl_tw* t) {
    Py_ssize_t pos = 0;
    PyObject *k, *v;
    memset(t, 0, sizeof *t);
    t->immediate = -1;
    t->cycles = 0;
    if (!kw) return 0;
    while (PyDict_Next(kw, &pos, &k, &v)) {
        char s[32];
        if (!PyUnicode_Check(k) || tl_utf8(k, s, sizeof s) < 0) {
            if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "option names are str");
            return -1;
        }
        if (strcmp(s, "duration") == 0) { if (tl_obj_to_ns(v, "duration", &t->duration) < 0) return -1; }
        else if (strcmp(s, "delay") == 0) { if (tl_obj_to_ns(v, "delay", &t->delay) < 0) return -1; }
        else if (strcmp(s, "ease") == 0) { if (tl_ease(v, &t->ease, &t->curve) < 0) return -1; }
        else if (strcmp(s, "from_") == 0 || strcmp(s, "from") == 0) { t->from = v == Py_None ? NULL : v; }
        else if (strcmp(s, "cycles") == 0) { if (tl_cycles(v, &t->cycles) < 0) return -1; }
        else if (strcmp(s, "yoyo") == 0) { if (tl_truth(v, &t->yoyo) < 0) return -1; }
        else if (strcmp(s, "keep_velocity") == 0) { if (tl_truth(v, &t->kv) < 0) return -1; }
        else if (strcmp(s, "start") == 0 && (allow & TW_START)) {
            if (v != Py_None) { if (tl_obj_to_ns(v, "start", &t->start) < 0) return -1; t->start_set = 1; }
        }
        else if (strcmp(s, "immediate") == 0 && (allow & TW_FIELD)) {
            if (v != Py_None && tl_truth(v, &t->immediate) < 0) return -1;
        }
        else if (strcmp(s, "overwrite") == 0 && (allow & TW_FIELD)) {
            if (PyUnicode_Check(v)) {
                char w[16];
                if (tl_utf8(v, w, sizeof w) < 0) return -1;
                tl_norm(w);
                if (strcmp(w, "all") == 0) t->overwrite = YTL_OVERWRITE_ALL;
                else if (strcmp(w, "none") == 0) t->overwrite = YTL_OVERWRITE_NONE;
                else { PyErr_SetString(TlArgumentError, "overwrite: True, False, \"all\" or \"none\""); return -1; }
            } else if (tl_truth(v, &t->overwrite) < 0) return -1;
        }
        else if (strcmp(s, "on_start") == 0 && (allow & TW_FIELD)) { t->on_start = v; }
        else if (strcmp(s, "on_complete") == 0 && (allow & TW_FIELD)) { t->on_complete = v; }
        else if (strcmp(s, "user") == 0 && (allow & TW_FIELD)) {
            t->user = PyLong_AsUnsignedLongLongMask(v);
            if (PyErr_Occurred()) return -1;
        }
        else if (strcmp(s, "stagger") == 0 && (allow & TW_FIELD)) { if (tl_obj_to_ns(v, "stagger", &t->stagger) < 0) return -1; }
        else { PyErr_Format(PyExc_TypeError, "unexpected option '%s'", s); return -1; }
    }
    return 0;
}

/* A channel tween's desc from the options and the end value. */
static int tl_tw_desc(const tl_tw* t, PyObject* to, ytl_tween_desc* d) {
    memset(d, 0, sizeof *d);
    if (tl_float(to, &d->to, "to") < 0) return -1;
    if (t->from) {
        if (tl_float(t->from, &d->from, "from_") < 0) return -1;
        d->from_set = true;
    }
    d->duration_ns = t->duration;
    d->delay_ns = t->delay;
    d->start_ns = t->start;
    d->start_set = t->start_set != 0;
    d->ease = t->ease;
    d->curve = t->curve;
    d->cycles = t->cycles;
    d->yoyo = t->yoyo != 0;
    d->keep_velocity = t->kv != 0;
    return 0;
}

/* --- Timeline: open and close ----------------------------------------------------- */

static void tl_free_cbuf(TimelineObject* o, int ch) {
    if (o->cbuf && o->cbuf[ch]) { PyMem_Free(o->cbuf[ch]); o->cbuf[ch] = NULL; }
}

static void tl_release(TimelineObject* o) {
    int i;
    for (i = 0; i < o->n_binds; i++) {
        Py_XDECREF(o->binds[i].obj);
        Py_XDECREF(o->binds[i].name);
        Py_XDECREF(o->binds[i].field);
    }
    o->n_binds = 0;
    Py_CLEAR(o->field_list);
    Py_CLEAR(o->callbacks);
    Py_CLEAR(o->cb_type);
    Py_CLEAR(o->cb_value);
    Py_CLEAR(o->cb_tb);
}

static int Timeline_traverse(PyObject* self, visitproc visit, void* arg) {
    TimelineObject* o = TL_OF(self);
    int i;
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(o->names);
    Py_VISIT(o->names_t);
    Py_VISIT(o->field_list);
    Py_VISIT(o->callbacks);
    Py_VISIT(o->cb_type);
    Py_VISIT(o->cb_value);
    Py_VISIT(o->cb_tb);
    for (i = 0; i < o->n_binds; i++) {
        Py_VISIT(o->binds[i].obj);
        Py_VISIT(o->binds[i].field);
    }
    return 0;
}

static int Timeline_clear_refs(PyObject* self) {
    tl_release(TL_OF(self));
    return 0;
}

static void Timeline_dealloc(PyObject* self) {
    TimelineObject* o = TL_OF(self);
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    int i;
    PyObject_GC_UnTrack(self);
    tl_release(o);
    if (o->box) ytl_close(&o->box->tl);
    if (o->cbuf) for (i = 0; i < YTL_MAX_CHANNELS; i++) tl_free_cbuf(o, i);
    PyMem_Free(o->cbuf);
    PyMem_Free(o->cbase);
    /* The handle and the field slots live in bytearrays, so a view the
     * caller kept reads them after this, stale but valid. */
    if (o->box) o->box->owner = NULL;
    PyMem_Free(o->events);
    PyMem_Free(o->fired);
    PyMem_Free(o->arena);
    PyMem_Free(o->tg);
    PyMem_Free(o->tw);
    PyMem_Free(o->fused);
    PyMem_Free(o->binds);
    Py_XDECREF(o->names);
    Py_XDECREF(o->names_t);
    Py_XDECREF(o->values_mv);
    Py_XDECREF(o->fields_mv);
    Py_XDECREF(o->box_ba);
    Py_XDECREF(o->fields_ba);
    fr(self);
    Py_DECREF(tp);
}

/* A bytearray of `bytes` bytes and a pointer into it aligned to 16 (the
 * handle needs 8; a field slot is 16 bytes). The array is never resized:
 * nothing else holds it until the views export it, and an export blocks a
 * resize. Python cannot hand out a view of PyMem memory that keeps the
 * memory alive (Py_bf_getbuffer is Limited API only from 3.11), so the
 * memory a view shows must belong to a Python object. */
static PyObject* tl_ba(size_t bytes, void** aligned, Py_ssize_t* off) {
    PyObject* ba = PyByteArray_FromStringAndSize(NULL, (Py_ssize_t)(bytes + 16));
    char* p;
    if (!ba) return NULL;
    p = PyByteArray_AsString(ba);
    *off = (Py_ssize_t)((16u - ((uintptr_t)p & 15u)) & 15u);
    *aligned = p + *off;
    memset(*aligned, 0, bytes);
    return ba;
}

/* A float32 view of ba[off:off + bytes], read-only unless `writable`. */
static PyObject* tl_view(PyObject* ba, Py_ssize_t off, Py_ssize_t bytes, int writable) {
    PyObject *mv, *a, *b, *sl, *part, *c, *r;
    if (!(mv = PyMemoryView_FromObject(ba))) return NULL;
    a = PyLong_FromSsize_t(off);
    b = PyLong_FromSsize_t(off + bytes);
    sl = a && b ? PySlice_New(a, b, NULL) : NULL;
    part = sl ? PyObject_GetItem(mv, sl) : NULL;
    Py_XDECREF(a); Py_XDECREF(b); Py_XDECREF(sl);
    Py_DECREF(mv);
    if (!part) return NULL;
    c = PyObject_CallMethod(part, "cast", "s", "f");
    Py_DECREF(part);
    if (!c || writable) return c;
    r = PyObject_CallMethod(c, "toreadonly", NULL);
    Py_DECREF(c);
    return r;
}

static int Timeline_init(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "channels", "initial", "events", "keys", "fields", "tweens", "lead", NULL };
    TimelineObject* o = TL_OF(self);
    PyObject *chans = NULL, *initial = NULL;
    int events = 1024, keys = 1024, fields = 64, tweens = 256, nch = 0, i;
    PyObject* leadobj = NULL;
    double lead = 0.0;
    float init[YTL_MAX_CHANNELS];
    ytl_desc d;
    Py_ssize_t box_off = 0, fields_off = 0;
    if (o->box) { PyErr_SetString(TlError, "Timeline.__init__ called twice"); return -1; }
    Py_CLEAR(o->names);
    Py_CLEAR(o->names_t);
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|O$OiiiiO", kwlist, &chans, &initial, &events, &keys, &fields,
                                     &tweens, &leadobj))
        return -1;
    if (leadobj && leadobj != Py_None) {
        if (PyUnicode_Check(leadobj)) {
            char s[16];
            if (tl_utf8(leadobj, s, sizeof s) < 0) return -1;
            tl_norm(s);
            if (strcmp(s, "none") != 0) { PyErr_SetString(TlArgumentError, "lead: a fraction of a period, or \"none\""); return -1; }
            lead = YTL_LEAD_NONE;
        } else {
            lead = PyFloat_AsDouble(leadobj);
            if (lead == -1.0 && PyErr_Occurred()) return -1;
        }
    }
    if (events < 0 || keys < 0 || fields < 0 || tweens < 0) {
        PyErr_SetString(TlArgumentError, "events, keys, fields and tweens are counts, >= 0");
        return -1;
    }
    memset(init, 0, sizeof init);
    if (chans && chans != Py_None) {
        if (PyLong_Check(chans)) {
            nch = (int)PyLong_AsLong(chans);
            if (nch == -1 && PyErr_Occurred()) return -1;
        } else {
            Py_ssize_t n = PySequence_Size(chans);
            if (n < 0) { PyErr_SetString(PyExc_TypeError, "channels: a count or a sequence of names"); return -1; }
            if (n > YTL_MAX_CHANNELS) { PyErr_Format(TlArgumentError, "channels: at most %d", YTL_MAX_CHANNELS); return -1; }
            o->names = PyDict_New();
            o->names_t = PyTuple_New(n);
            if (!o->names || !o->names_t) return -1;
            for (i = 0; i < (int)n; i++) {
                PyObject *nm = PySequence_GetItem(chans, i), *ix;
                if (!nm) return -1;
                if (!PyUnicode_Check(nm)) { Py_DECREF(nm); PyErr_SetString(PyExc_TypeError, "channel names are str"); return -1; }
                if (PyDict_GetItem(o->names, nm)) { PyErr_Format(TlArgumentError, "channel %R named twice", nm); Py_DECREF(nm); return -1; }
                ix = PyLong_FromLong(i);
                if (!ix || PyDict_SetItem(o->names, nm, ix) < 0) { Py_XDECREF(ix); Py_DECREF(nm); return -1; }
                Py_DECREF(ix);
                PyTuple_SetItem(o->names_t, i, nm);
            }
            nch = (int)n;
        }
    }
    if (nch < 0 || nch > YTL_MAX_CHANNELS) { PyErr_Format(TlArgumentError, "channels: 0 to %d", YTL_MAX_CHANNELS); return -1; }
    if (initial && initial != Py_None) {
        if (PyDict_Check(initial)) {
            Py_ssize_t pos = 0;
            PyObject *k, *v;
            while (PyDict_Next(initial, &pos, &k, &v)) {
                int ch;
                if (tl_chan(o, k, &ch) < 0) return -1;
                if (ch < 0 || ch >= nch) { PyErr_Format(TlArgumentError, "initial: no channel %R", k); return -1; }
                if (tl_float(v, &init[ch], "initial") < 0) return -1;
            }
        } else {
            Py_ssize_t n = PySequence_Size(initial);
            if (n != nch) { PyErr_SetString(TlArgumentError, "initial: one value per channel, or a dict"); return -1; }
            for (i = 0; i < nch; i++) {
                PyObject* v = PySequence_GetItem(initial, i);
                int rc = v ? tl_float(v, &init[i], "initial") : -1;
                Py_XDECREF(v);
                if (rc < 0) return -1;
            }
        }
    }
    o->box_ba = tl_ba(sizeof(tl_box), (void**)&o->box, &box_off);
    o->fields_ba = tl_ba(16 * (size_t)(fields ? fields : 1), (void**)&o->fields, &fields_off);
    if (!o->box_ba || !o->fields_ba) return -1;
    o->events = (ytl_event*)PyMem_Malloc(sizeof(ytl_event) * (size_t)(events ? events : 1));
    o->fired = (ytl_event*)PyMem_Malloc(sizeof(ytl_event) * (size_t)(events ? events : 1));
    o->arena = (ytl_key*)PyMem_Malloc(sizeof(ytl_key) * (size_t)(keys ? keys : 1));
    o->tg = (ytl_target_slot*)PyMem_Malloc(sizeof(ytl_target_slot) * (size_t)(fields ? fields : 1));
    o->tw = (ytl_tween_slot*)PyMem_Malloc(sizeof(ytl_tween_slot) * (size_t)(tweens ? tweens : 1));
    o->fused = (uint8_t*)PyMem_Malloc((size_t)(fields ? fields : 1));
    o->cbuf = (void**)PyMem_Malloc(sizeof(void*) * YTL_MAX_CHANNELS);
    o->cbase = (int8_t*)PyMem_Malloc(YTL_MAX_CHANNELS);
    if (!o->events || !o->fired || !o->arena || !o->tg || !o->tw || !o->fused || !o->cbuf || !o->cbase) {
        PyErr_NoMemory();
        return -1;
    }
    memset(o->fused, 0, (size_t)(fields ? fields : 1));
    memset(o->cbuf, 0, sizeof(void*) * YTL_MAX_CHANNELS);
    memset(o->cbase, 0, YTL_MAX_CHANNELS);
    o->box->owner = o;
    o->ecap = events; o->kcap = keys; o->fcap = fields; o->twcap = tweens;
    memset(&d, 0, sizeof d);
    d.events = o->events; d.event_capacity = events;
    d.n_channels = nch;
    d.initial = init;
    d.lead = lead;
    d.keys = o->arena; d.key_capacity = keys;
    d.targets = o->tg; d.target_capacity = fields;
    d.tweens = o->tw; d.tween_capacity = tweens;
    if (!ytl_open(&o->box->tl, &d)) {
        PyErr_SetString(TlArgumentError, ytl_error(&o->box->tl));
        return -1;
    }
    o->open = 1;
    o->n_channels = nch;
    o->callbacks = PyDict_New();
    o->field_list = PyList_New(0);
    if (!o->callbacks || !o->field_list) return -1;
    o->values_mv = tl_view(o->box_ba, box_off + (Py_ssize_t)((char*)o->box->tl.values - (char*)o->box),
                           (Py_ssize_t)nch * 4, 0);
    o->fields_mv = tl_view(o->fields_ba, fields_off, (Py_ssize_t)fields * 16, 1);
    if (!o->values_mv || !o->fields_mv) return -1;
    return 0;
}

static PyObject* Timeline_close(PyObject* self, PyObject* unused) {
    TimelineObject* o = TL_OF(self);
    (void)unused;
    if (o->box) ytl_close(&o->box->tl);
    o->open = 0;
    tl_release(o);
    Py_RETURN_NONE;
}

static PyObject* Timeline_enter(PyObject* self, PyObject* unused) { (void)unused; Py_INCREF(self); return self; }
static PyObject* Timeline_exit(PyObject* self, PyObject* args) { (void)args; return Timeline_close(self, NULL); }

/* --- Timeline: bases --------------------------------------------------------------- */

static PyObject* tl_rc(TimelineObject* o, int rc) {
    if (rc < 0) return tl_fail(o, rc);
    return PyLong_FromLong(rc);
}

static PyObject* Timeline_anchor(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "base", "rt", "bt", NULL };
    TimelineObject* o = TL_OF(self);
    int base;
    double rt, bt = 0.0;
    int64_t rn, bn;
    if (!tl_ready(o) || !PyArg_ParseTupleAndKeywords(args, kw, "id|d", kwlist, &base, &rt, &bt)) return NULL;
    if (tl_s_to_ns(rt, "rt", &rn) < 0 || tl_s_to_ns(bt, "bt", &bn) < 0) return NULL;
    if ((base = ytl_anchor(&o->box->tl, base, rn, bn)) < 0) return tl_fail(o, base);
    Py_RETURN_NONE;
}

static PyObject* Timeline_skip(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    int base;
    double rt, bt;
    int64_t rn, bn;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "idd", &base, &rt, &bt)) return NULL;
    if (tl_s_to_ns(rt, "rt", &rn) < 0 || tl_s_to_ns(bt, "bt", &bn) < 0) return NULL;
    return tl_rc(o, ytl_skip(&o->box->tl, base, rn, bn));
}

static PyObject* tl_base_rt(PyObject* self, PyObject* args, int which) {
    TimelineObject* o = TL_OF(self);
    int base, rc;
    double rt;
    int64_t rn;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "id", &base, &rt)) return NULL;
    if (tl_s_to_ns(rt, "rt", &rn) < 0) return NULL;
    rc = which == 0 ? ytl_pause(&o->box->tl, base, rn) : ytl_resume(&o->box->tl, base, rn);
    if (rc < 0) return tl_fail(o, rc);
    Py_RETURN_NONE;
}
static PyObject* Timeline_pause(PyObject* self, PyObject* args) { return tl_base_rt(self, args, 0); }
static PyObject* Timeline_resume(PyObject* self, PyObject* args) { return tl_base_rt(self, args, 1); }

static PyObject* Timeline_stop(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    int base, rc;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "i", &base)) return NULL;
    if ((rc = ytl_stop(&o->box->tl, base)) < 0) return tl_fail(o, rc);
    Py_RETURN_NONE;
}

static PyObject* Timeline_rate(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    int base, rc;
    long num, den = 1;
    double rt;
    int64_t rn;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "idl|l", &base, &rt, &num, &den)) return NULL;
    if (tl_s_to_ns(rt, "rt", &rn) < 0) return NULL;
    if (num < 1 || den < 1 || num > INT32_MAX || den > INT32_MAX) {
        PyErr_SetString(TlArgumentError, "rate: num and den in 1 to 2^31 - 1 (ytl_pause() freezes a base)");
        return NULL;
    }
    if ((rc = ytl_rate(&o->box->tl, base, rn, (int32_t)num, (int32_t)den)) < 0) return tl_fail(o, rc);
    Py_RETURN_NONE;
}

static PyObject* Timeline_get_rate(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    int base;
    int32_t n = 0, d = 0;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "i", &base)) return NULL;
    if (!ytl_get_rate(&o->box->tl, base, &n, &d)) { PyErr_SetString(TlArgumentError, "get_rate: no such base"); return NULL; }
    return Py_BuildValue("(ll)", (long)n, (long)d);
}

static PyObject* Timeline_base_time(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    int base;
    double rt;
    int64_t rn, out = 0;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "id", &base, &rt) || tl_s_to_ns(rt, "rt", &rn) < 0) return NULL;
    {
        /* The call writes out: evaluate it first (argument order is unspecified). */
        bool ok_ = ytl_base_time(&o->box->tl, base, rn, &out);
        return tl_time_out(ok_, out);
    }
}

static PyObject* Timeline_rt_time(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    int base;
    double bt;
    int64_t bn, out = 0;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "id", &base, &bt) || tl_s_to_ns(bt, "bt", &bn) < 0) return NULL;
    {
        /* The call writes out: evaluate it first (argument order is unspecified). */
        bool ok_ = ytl_rt_time(&o->box->tl, base, bn, &out);
        return tl_time_out(ok_, out);
    }
}

static PyObject* Timeline_window(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    int base;
    double onset, period = 0.0;
    ytl_frame f;
    int64_t out = 0;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "id|d", &base, &onset, &period)) return NULL;
    memset(&f, 0, sizeof f);
    if (tl_s_to_ns(onset, "onset", &f.onset) < 0 || tl_s_to_ns(period, "period", &f.period) < 0) return NULL;
    {
        /* The call writes out: evaluate it first (argument order is unspecified). */
        bool ok_ = ytl_window(&o->box->tl, base, &f, &out);
        return tl_time_out(ok_, out);
    }
}

static PyObject* Timeline_clamped(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    int base;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "i", &base)) return NULL;
    return tl_rc(o, ytl_clamped(&o->box->tl, base));
}

static PyObject* Timeline_next_due(PyObject* self, PyObject* unused) {
    TimelineObject* o = TL_OF(self);
    int64_t t = 0;
    (void)unused;
    if (!tl_ready(o)) return NULL;
    {
        /* The call writes t: evaluate it first (argument order is unspecified). */
        bool ok_ = ytl_next_due(&o->box->tl, &t);
        return tl_time_out(ok_, t);
    }
}

static PyObject* Timeline_horizon(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    int base;
    int64_t t = 0;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "i", &base)) return NULL;
    {
        /* The call writes t: evaluate it first (argument order is unspecified). */
        bool ok_ = ytl_horizon(&o->box->tl, base, &t);
        return tl_time_out(ok_, t);
    }
}

/* --- Timeline: events ----------------------------------------------------------------- */

/* An event from keywords (add) or a dict (add_many). */
static int tl_event_in(TimelineObject* o, PyObject* time, PyObject* kw, ytl_event* e) {
    Py_ssize_t pos = 0;
    PyObject *k, *v;
    int x;
    memset(e, 0, sizeof *e);
    if (time && tl_obj_to_ns(time, "time", &e->time_ns) < 0) return -1;
    if (!kw) return 0;
    while (PyDict_Next(kw, &pos, &k, &v)) {
        char s[16];
        if (!PyUnicode_Check(k) || tl_utf8(k, s, sizeof s) < 0) {
            if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "event keys are str");
            return -1;
        }
        if (strcmp(s, "time") == 0) { if (tl_obj_to_ns(v, "time", &e->time_ns) < 0) return -1; }
        else if (strcmp(s, "kind") == 0) {
            if (tl_enum(v, KINDS, "kind", &x) < 0) return -1;
            if (x < 0 || x > 255) { PyErr_SetString(TlArgumentError, "kind: 0 to 255"); return -1; }
            e->kind = (uint8_t)x;
        }
        else if (strcmp(s, "base") == 0) {
            int32_t b;
            if (tl_int32(v, &b, "base") < 0) return -1;
            if (b < 0 || b >= YTL_MAX_BASES) { PyErr_SetString(TlArgumentError, "base: 0 to 7"); return -1; }
            e->base = (uint8_t)b;
        }
        else if (strcmp(s, "target") == 0 || strcmp(s, "channel") == 0) { if (tl_chan(o, v, &x) < 0) return -1; e->target = x; }
        else if (strcmp(s, "value") == 0) { if (tl_float(v, &e->value, "value") < 0) return -1; }
        else if (strcmp(s, "code") == 0) { if (tl_int32(v, &e->code, "code") < 0) return -1; }
        else if (strcmp(s, "user") == 0) { e->user = PyLong_AsUnsignedLongLongMask(v); if (PyErr_Occurred()) return -1; }
        else { PyErr_Format(PyExc_TypeError, "unexpected event key '%s'", s); return -1; }
    }
    return 0;
}

static PyObject* Timeline_add(PyObject* self, PyObject* args, PyObject* kw) {
    TimelineObject* o = TL_OF(self);
    PyObject *time = NULL, *kind = NULL, *kw2;
    ytl_event e;
    int rc;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "O|O", &time, &kind)) return NULL;
    kw2 = kw ? PyDict_Copy(kw) : PyDict_New();
    if (!kw2) return NULL;
    if (kind && PyDict_SetItemString(kw2, "kind", kind) < 0) { Py_DECREF(kw2); return NULL; }
    rc = tl_event_in(o, time, kw2, &e);
    Py_DECREF(kw2);
    if (rc < 0) return NULL;
    return tl_rc(o, ytl_add(&o->box->tl, &e));
}

static PyObject* Timeline_add_many(PyObject* self, PyObject* arg) {
    TimelineObject* o = TL_OF(self);
    Py_ssize_t n, i;
    ytl_event* ev;
    int rc;
    if (!tl_ready(o)) return NULL;
    if ((n = PySequence_Size(arg)) < 0) return NULL;
    if (n > INT32_MAX) { PyErr_SetString(TlArgumentError, "add_many: too many events"); return NULL; }
    ev = (ytl_event*)PyMem_Malloc(sizeof(ytl_event) * (size_t)(n ? n : 1));
    if (!ev) return PyErr_NoMemory();
    for (i = 0; i < n; i++) {
        PyObject* d = PySequence_GetItem(arg, i);
        int r = -1;
        if (d && PyDict_Check(d)) r = tl_event_in(o, NULL, d, &ev[i]);
        else if (d) PyErr_SetString(PyExc_TypeError, "add_many: each event is a dict of add()'s keywords and time");
        Py_XDECREF(d);
        if (r < 0) { PyMem_Free(ev); return NULL; }
    }
    rc = ytl_add_n(&o->box->tl, ev, (int)n);
    PyMem_Free(ev);
    return tl_rc(o, rc);
}

static PyObject* Timeline_remove(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    int id, rc;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "i", &id)) return NULL;
    if ((rc = ytl_remove(&o->box->tl, id)) < 0) return tl_fail(o, rc);
    tl_sweep(o);
    Py_RETURN_NONE;
}

static PyObject* Timeline_prune(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    int base = YTL_ALL_BASES, rc;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "|i", &base)) return NULL;
    rc = ytl_prune(&o->box->tl, base);
    if (rc >= 0) tl_sweep(o);
    return tl_rc(o, rc);
}

static PyObject* Timeline_clear(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    int base = YTL_ALL_BASES, rc, ch;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "|i", &base)) return NULL;
    rc = ytl_clear(&o->box->tl, base);
    if (rc >= 0) {
        /* The clear dropped the tracks bound to the base, so the arrays they
         * pointed at can go. */
        for (ch = 0; ch < YTL_MAX_CHANNELS; ch++)
            if (o->cbuf[ch] && (base == YTL_ALL_BASES || o->cbase[ch] == base)) tl_free_cbuf(o, ch);
        tl_sweep(o);
    }
    return tl_rc(o, rc);
}

static PyObject* Timeline_events(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    int base = YTL_ALL_BASES, n = 0;
    const ytl_event* e;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "|i", &base)) return NULL;
    if (base < YTL_ALL_BASES || base >= YTL_MAX_BASES) { PyErr_SetString(TlArgumentError, "events: base 0 to 7, or ALL_BASES"); return NULL; }
    e = ytl_events(&o->box->tl, base, &n);
    return tl_event_list(o, e, e ? n : 0);
}

static PyObject* Timeline_find(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    int id;
    const ytl_event* e;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "i", &id)) return NULL;
    e = ytl_find(&o->box->tl, id);
    if (!e) Py_RETURN_NONE;
    return tl_event(o, e);
}

static PyObject* Timeline_peek(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    int base, n;
    double a, b;
    int64_t an, bn;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "idd", &base, &a, &b)) return NULL;
    if (tl_s_to_ns(a, "start", &an) < 0 || tl_s_to_ns(b, "end", &bn) < 0) return NULL;
    n = ytl_peek(&o->box->tl, base, an, bn, o->fired, o->ecap);
    if (n < 0) return tl_fail(o, n);
    return tl_event_list(o, o->fired, n < o->ecap ? n : o->ecap);
}

/* --- Timeline: tracks ----------------------------------------------------------------- */

/* Keys from rows (t, value) or (t, value, ease); a Bezier ease (a 4-tuple)
 * becomes a curve in the table. One allocation holds keys and curves. */
static void* tl_keys_in(PyObject* rows, int* n_keys, ytl_key** keys, ytl_curve** curves, int* n_curves) {
    Py_ssize_t n = PySequence_Size(rows), i;
    void* buf;
    if (n < 0) return NULL;
    if (n > 65535 * 16) { PyErr_SetString(TlArgumentError, "keys: too many"); return NULL; }
    buf = PyMem_Malloc((sizeof(ytl_key) + sizeof(ytl_curve)) * (size_t)(n ? n : 1));
    if (!buf) { PyErr_NoMemory(); return NULL; }
    *keys = (ytl_key*)buf;
    *curves = (ytl_curve*)(void*)((char*)buf + sizeof(ytl_key) * (size_t)(n ? n : 1));
    *n_curves = 0;
    for (i = 0; i < n; i++) {
        PyObject* r = PySequence_GetItem(rows, i);
        Py_ssize_t m = r ? PySequence_Size(r) : -1;
        ytl_key* k = &(*keys)[i];
        int ok = 0;
        memset(k, 0, sizeof *k);
        if (m == 2 || m == 3) {
            PyObject *t = PySequence_GetItem(r, 0), *v = PySequence_GetItem(r, 1), *e = m == 3 ? PySequence_GetItem(r, 2) : NULL;
            int ease = 0;
            ytl_curve c;
            if (t && v && tl_obj_to_ns(t, "key time", &k->time_ns) == 0 && tl_float(v, &k->value, "key value") == 0
                && tl_ease(e, &ease, &c) == 0) {
                k->ease = (uint8_t)(ease < 0 || ease > 255 ? 255 : ease);
                if (ease == YTL_EASE_BEZIER && e && (PyTuple_Check(e) || PyList_Check(e))) {
                    (*curves)[*n_curves] = c;
                    k->curve = (uint16_t)*n_curves;
                    (*n_curves)++;
                }
                ok = 1;
            }
            Py_XDECREF(t); Py_XDECREF(v); Py_XDECREF(e);
        } else if (r && !PyErr_Occurred()) {
            PyErr_SetString(TlArgumentError, "a key is (time, value) or (time, value, ease)");
        }
        Py_XDECREF(r);
        if (!ok) { PyMem_Free(buf); return NULL; }
    }
    *n_keys = (int)n;
    return buf;
}

/* Samples: a buffer of float32 (array('f'), numpy.float32) or a sequence. */
static float* tl_samples_in(PyObject* s, int* n_out) {
    PyObject* mv = PyMemoryView_FromObject(s);
    float* out;
    Py_ssize_t n, i;
    if (mv) {
        PyObject* f = PyObject_GetAttrString(mv, "format");
        int is_f = f && PyUnicode_Check(f) && (PyUnicode_CompareWithASCIIString(f, "f") == 0 ||
                                                PyUnicode_CompareWithASCIIString(f, "<f") == 0 ||
                                                PyUnicode_CompareWithASCIIString(f, "=f") == 0);
        Py_XDECREF(f);
        PyErr_Clear();
        if (is_f) {
            PyObject* b = PyBytes_FromObject(mv);
            Py_DECREF(mv);
            if (!b) return NULL;
            n = PyBytes_Size(b) / 4;
            if (n > INT32_MAX) { Py_DECREF(b); PyErr_SetString(TlArgumentError, "samples: too many"); return NULL; }
            out = (float*)PyMem_Malloc((size_t)(n ? n : 1) * 4);
            if (!out) { Py_DECREF(b); PyErr_NoMemory(); return NULL; }
            memcpy(out, PyBytes_AsString(b), (size_t)n * 4);
            Py_DECREF(b);
            *n_out = (int)n;
            return out;
        }
        Py_DECREF(mv);
    }
    PyErr_Clear();
    n = PySequence_Size(s);
    if (n < 0) { PyErr_SetString(PyExc_TypeError, "samples: a float32 buffer or a sequence of numbers"); return NULL; }
    if (n > INT32_MAX) { PyErr_SetString(TlArgumentError, "samples: too many"); return NULL; }
    out = (float*)PyMem_Malloc((size_t)(n ? n : 1) * 4);
    if (!out) { PyErr_NoMemory(); return NULL; }
    for (i = 0; i < n; i++) {
        PyObject* x = PySequence_GetItem(s, i);
        int rc = x ? tl_float(x, &out[i], "sample") : -1;
        Py_XDECREF(x);
        if (rc < 0) { PyMem_Free(out); return NULL; }
    }
    *n_out = (int)n;
    return out;
}

static PyObject* tl_track_done(TimelineObject* o, int ch, int base, void* buf, int rc) {
    if (rc < 0) { PyMem_Free(buf); return tl_fail(o, rc); }
    /* The header now points at buf and no longer at the old array. */
    if (ch >= 0 && ch < YTL_MAX_CHANNELS) {
        tl_free_cbuf(o, ch);
        o->cbuf[ch] = buf;
        o->cbase[ch] = (int8_t)base;
    } else {
        PyMem_Free(buf);
    }
    Py_RETURN_NONE;
}

static PyObject* Timeline_set_keys(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    PyObject *cho, *rows;
    int ch, base, nk = 0, nc = 0;
    ytl_key* keys;
    ytl_curve* curves;
    ytl_track tr;
    void* buf;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "OiO", &cho, &base, &rows) || tl_chan(o, cho, &ch) < 0) return NULL;
    if (!(buf = tl_keys_in(rows, &nk, &keys, &curves, &nc))) return NULL;
    if (nk == 0) { PyMem_Free(buf); return tl_track_done(o, ch, base, NULL, ytl_set_track(&o->box->tl, ch, base, NULL)); }
    memset(&tr, 0, sizeof tr);
    tr.keys = keys; tr.n_keys = nk;
    if (nc) { tr.curves = curves; tr.n_curves = nc; }
    return tl_track_done(o, ch, base, buf, ytl_set_track(&o->box->tl, ch, base, &tr));
}

static PyObject* Timeline_set_track(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "channel", "base", "keys", "samples", "t0", "rate", "interp", "period", "repeats", NULL };
    TimelineObject* o = TL_OF(self);
    PyObject *cho, *rows = NULL, *samples = NULL, *interp = NULL;
    int ch, base, nk = 0, nc = 0, ns = 0, iv = 0, repeats = 0;
    long long rate = 0;
    double t0 = 0.0, period = 0.0;
    ytl_key* keys = NULL;
    ytl_curve* curves = NULL;
    ytl_track tr;
    void* buf = NULL;
    if (!tl_ready(o) || !PyArg_ParseTupleAndKeywords(args, kw, "Oi|$OOdLOdi", kwlist, &cho, &base, &rows, &samples, &t0,
                                                     &rate, &interp, &period, &repeats))
        return NULL;
    if (tl_chan(o, cho, &ch) < 0) return NULL;
    if ((!rows || rows == Py_None) && (!samples || samples == Py_None))
        return tl_track_done(o, ch, base, NULL, ytl_set_track(&o->box->tl, ch, base, NULL));
    memset(&tr, 0, sizeof tr);
    if (interp && interp != Py_None && tl_enum(interp, INTERPS, "interp", &iv) < 0) return NULL;
    if (tl_s_to_ns(t0, "t0", &tr.t0_ns) < 0 || tl_s_to_ns(period, "period", &tr.period_ns) < 0) return NULL;
    if (rows && rows != Py_None && samples && samples != Py_None) {
        PyErr_SetString(TlArgumentError, "set_track: keys or samples, not both");
        return NULL;
    }
    if (rows && rows != Py_None) {
        if (!(buf = tl_keys_in(rows, &nk, &keys, &curves, &nc))) return NULL;
        tr.keys = keys; tr.n_keys = nk;
        if (nc) { tr.curves = curves; tr.n_curves = nc; }
    } else {
        if (!(buf = tl_samples_in(samples, &ns))) return NULL;
        tr.samples = (const float*)buf; tr.n_samples = ns;
    }
    tr.rate = (int64_t)rate;
    tr.interp = iv;
    tr.repeats = repeats;
    return tl_track_done(o, ch, base, buf, ytl_set_track(&o->box->tl, ch, base, &tr));
}

static PyObject* Timeline_tween(PyObject* self, PyObject* args, PyObject* kw) {
    TimelineObject* o = TL_OF(self);
    PyObject *cho, *to, *b = NULL, *kw2 = NULL;
    int ch, base = YTL_BASE_RT, rc;
    tl_tw t;
    ytl_tween_desc d;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "OO", &cho, &to) || tl_chan(o, cho, &ch) < 0) return NULL;
    if (kw && (b = PyDict_GetItemString(kw, "base"))) {
        long x = PyLong_AsLong(b);
        if (x == -1 && PyErr_Occurred()) return NULL;
        base = (int)x;
        if (!(kw2 = PyDict_Copy(kw))) return NULL;
        PyDict_DelItemString(kw2, "base");
    }
    rc = tl_tw_parse(kw2 ? kw2 : kw, TW_CHANNEL | TW_START, &t);
    Py_XDECREF(kw2);
    if (rc < 0 || tl_tw_desc(&t, to, &d) < 0) return NULL;
    if ((rc = ytl_tween(&o->box->tl, ch, base, &d)) < 0) return tl_fail(o, rc);
    Py_RETURN_NONE;
}

/* --- Timeline: fields --------------------------------------------------------------- */

static PyObject* tl_new_field(TimelineObject* o, int index, int n, int type) {
    allocfunc al = (allocfunc)PyType_GetSlot((PyTypeObject*)FieldType, Py_tp_alloc);
    FieldObject* f = (FieldObject*)al((PyTypeObject*)FieldType, 0);
    if (!f) return NULL;
    Py_INCREF(o);
    f->owner = (PyObject*)o;
    f->index = index;
    f->n = n;
    f->type = type;
    o->fused[index] = 1;
    if (o->field_list && PyList_Append(o->field_list, (PyObject*)f) < 0) { Py_DECREF(f); return NULL; }
    return (PyObject*)f;
}

static int tl_field_set(TimelineObject* o, int index, int n, int type, PyObject* v) {
    float* p = o->fields + 4 * index;
    int i;
    if (PyTuple_Check(v) || PyList_Check(v)) {
        if (PySequence_Size(v) != n) { PyErr_Format(TlArgumentError, "the field has %d components", n); return -1; }
        for (i = 0; i < n; i++) {
            PyObject* x = PySequence_GetItem(v, i);
            int rc;
            if (!x) return -1;
            if (type == YTL_I32) rc = tl_int32(x, (int32_t*)(void*)&p[i], "value");
            else rc = tl_float(x, &p[i], "value");
            Py_DECREF(x);
            if (rc < 0) return -1;
        }
        return 0;
    }
    if (n != 1) { PyErr_Format(TlArgumentError, "the field has %d components: give a tuple", n); return -1; }
    return type == YTL_I32 ? tl_int32(v, (int32_t*)(void*)p, "value") : tl_float(v, p, "value");
}

static PyObject* tl_field_get(TimelineObject* o, int index, int n, int type) {
    const float* p = o->fields + 4 * index;
    PyObject* t;
    int i;
    if (n == 1) return type == YTL_I32 ? PyLong_FromLong((long)*(const int32_t*)(const void*)p) : PyFloat_FromDouble((double)p[0]);
    t = PyTuple_New(n);
    if (!t) return NULL;
    for (i = 0; i < n; i++)
        PyTuple_SetItem(t, i, type == YTL_I32 ? PyLong_FromLong((long)((const int32_t*)(const void*)p)[i])
                                              : PyFloat_FromDouble((double)p[i]));
    return t;
}

/* n slots in a row (stride 16 bytes, which a stagger needs), from the
 * first run of free slots. */
static PyObject* tl_fields_make(TimelineObject* o, int count, PyObject* value, int n, int type, int as_list) {
    int i, j, start = -1;
    PyObject* l;
    if (n == 0) n = value && (PyTuple_Check(value) || PyList_Check(value)) ? (int)PySequence_Size(value) : 1;
    if (n < 1 || n > 4) { PyErr_SetString(TlArgumentError, "a field has 1 to 4 components"); return NULL; }
    if (count < 1) { PyErr_SetString(TlArgumentError, "count: >= 1"); return NULL; }
    for (i = 0; i + count <= o->fcap && start < 0; i++) {
        for (j = 0; j < count && !o->fused[i + j]; j++) {}
        if (j == count) start = i;
        else i += j;
    }
    if (start < 0) {
        PyErr_Format(TlFullError, "ysp_timeline: no %d free field slots in a row (fields=%d)", count, o->fcap);
        return NULL;
    }
    for (i = 0; i < count; i++) {
        memset(o->fields + 4 * (start + i), 0, 16);
        if (value && value != Py_None && tl_field_set(o, start + i, n, type, value) < 0) return NULL;
    }
    if (!as_list) return tl_new_field(o, start, n, type);
    l = PyList_New(count);
    if (!l) return NULL;
    for (i = 0; i < count; i++) {
        PyObject* f = tl_new_field(o, start + i, n, type);
        if (!f) { Py_DECREF(l); return NULL; }
        PyList_SetItem(l, i, f);
    }
    return l;
}

static PyObject* Timeline_field(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "value", "n", "int", NULL };
    TimelineObject* o = TL_OF(self);
    PyObject* value = NULL;
    int n = 0, is_int = 0;
    if (!tl_ready(o) || !PyArg_ParseTupleAndKeywords(args, kw, "|O$ip", kwlist, &value, &n, &is_int)) return NULL;
    return tl_fields_make(o, 1, value, n, is_int ? YTL_I32 : YTL_F32, 0);
}

static PyObject* Timeline_fields(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "count", "value", "n", "int", NULL };
    TimelineObject* o = TL_OF(self);
    PyObject* value = NULL;
    int count, n = 0, is_int = 0;
    if (!tl_ready(o) || !PyArg_ParseTupleAndKeywords(args, kw, "i|O$ip", kwlist, &count, &value, &n, &is_int)) return NULL;
    return tl_fields_make(o, count, value, n, is_int ? YTL_I32 : YTL_F32, 1);
}

static int tl_is_field(PyObject* x) { return PyObject_TypeCheck(x, (PyTypeObject*)FieldType); }
static int tl_field_live(PyObject* x) { return ((FieldObject*)x)->index >= 0 && ((FieldObject*)x)->owner; }

static PyObject* Timeline_kill(PyObject* self, PyObject* arg) {
    TimelineObject* o = TL_OF(self);
    FieldObject* f = (FieldObject*)arg;
    int rc;
    if (!tl_ready(o)) return NULL;
    if (!tl_is_field(arg) || f->owner != self) { PyErr_SetString(PyExc_TypeError, "kill: a Field of this Timeline"); return NULL; }
    if (!tl_field_live(arg)) { PyErr_SetString(TlArgumentError, "the field was freed"); return NULL; }
    rc = ytl_kill(&o->box->tl, o->fields + 4 * f->index);
    if (rc >= 0) tl_sweep(o);
    return tl_rc(o, rc);
}

static PyObject* Timeline_kill_tween(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    int id, rc;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "i", &id)) return NULL;
    if ((rc = ytl_kill_tween(&o->box->tl, id)) < 0) return tl_fail(o, rc);
    tl_sweep(o);
    Py_RETURN_NONE;
}

static PyObject* Timeline_tween_count(PyObject* self, PyObject* unused) {
    TimelineObject* o = TL_OF(self);
    int nt = 0, n;
    (void)unused;
    if (!tl_ready(o)) return NULL;
    n = ytl_tween_count(&o->box->tl, &nt);
    return Py_BuildValue("(ii)", n, nt);
}

/* --- Timeline: the frame ----------------------------------------------------------------- */

static int tl_apply_binds(TimelineObject* o) {
    int i;
    for (i = 0; i < o->n_binds; i++) {
        tl_bindrow* b = &o->binds[i];
        PyObject* v;
        int rc;
        if (b->field) {
            FieldObject* f = (FieldObject*)b->field;
            if (f->index < 0) continue;   /* freed: nothing to write */
            if (b->comp >= 0) {
                const float* p = o->fields + 4 * f->index + b->comp;
                v = f->type == YTL_I32 ? PyLong_FromLong((long)*(const int32_t*)(const void*)p) : PyFloat_FromDouble((double)*p);
            } else {
                v = tl_field_get(o, f->index, f->n, f->type);
            }
        } else {
            v = PyFloat_FromDouble((double)o->box->tl.values[b->ch]);
        }
        if (!v) return -1;
        rc = PyObject_SetAttr(b->obj, b->name, v);
        Py_DECREF(v);
        if (rc < 0) return -1;
    }
    return 0;
}

static PyObject* Timeline_evaluate(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    double onset, period = 0.0;
    PyObject* idx = Py_None;
    ytl_frame f;
    int n, i;
    PyObject* t;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "d|dO", &onset, &period, &idx)) return NULL;
    if (tl_s_to_ns(onset, "onset", &f.onset) < 0 || tl_s_to_ns(period, "period", &f.period) < 0) return NULL;
    if (idx == Py_None) f.index = o->frames;
    else {
        f.index = PyLong_AsLongLong(idx);
        if (f.index == -1 && PyErr_Occurred()) return NULL;
    }
    n = ytl_evaluate(&o->box->tl, &f, o->fired, o->ecap);
    if (o->cb_type) {
        /* A callback raised: the frame is evaluated and the storage holds
         * its record, so the error goes up now. */
        PyErr_Restore(o->cb_type, o->cb_value, o->cb_tb);
        o->cb_type = o->cb_value = o->cb_tb = NULL;
        return NULL;
    }
    if (n < 0) return tl_fail(o, n);
    o->frames = f.index + 1;
    if (o->n_binds && tl_apply_binds(o) < 0) return NULL;
    t = PyTuple_New(n);
    if (!t || n == 0) return t;
    for (i = 0; i < n; i++) {
        PyObject* e = tl_event(o, &o->fired[i]);
        if (!e) { Py_DECREF(t); return NULL; }
        PyTuple_SetItem(t, i, e);
    }
    return t;
}

static PyObject* Timeline_value(PyObject* self, PyObject* arg) {
    TimelineObject* o = TL_OF(self);
    int ch;
    if (!tl_ready(o) || tl_chan(o, arg, &ch) < 0) return NULL;
    if (ch < 0 || ch >= o->n_channels) { PyErr_SetString(TlArgumentError, "value: no such channel"); return NULL; }
    return PyFloat_FromDouble((double)o->box->tl.values[ch]);
}

static PyObject* Timeline_channel(PyObject* self, PyObject* arg) {
    TimelineObject* o = TL_OF(self);
    int ch;
    if (!tl_ready(o) || tl_chan(o, arg, &ch) < 0) return NULL;
    return PyLong_FromLong(ch);
}

static PyObject* Timeline_bind(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "obj", "attr", "source", "component", NULL };
    TimelineObject* o = TL_OF(self);
    PyObject *obj, *name, *src;
    int comp = -1, ch = 0;
    tl_bindrow* b;
    if (!tl_ready(o) || !PyArg_ParseTupleAndKeywords(args, kw, "OUO|i", kwlist, &obj, &name, &src, &comp)) return NULL;
    if (tl_is_field(src)) {
        FieldObject* f = (FieldObject*)src;
        if (f->owner != self || !tl_field_live(src)) { PyErr_SetString(TlArgumentError, "bind: a freed Field, or one of another Timeline"); return NULL; }
        if (comp < -1 || comp >= f->n) { PyErr_SetString(TlArgumentError, "bind: component out of range"); return NULL; }
    } else {
        if (tl_chan(o, src, &ch) < 0) return NULL;
        if (ch < 0 || ch >= o->n_channels) { PyErr_SetString(TlArgumentError, "bind: no such channel"); return NULL; }
    }
    if (o->n_binds == o->binds_cap) {
        int cap = o->binds_cap ? 2 * o->binds_cap : 16;
        tl_bindrow* nb = (tl_bindrow*)PyMem_Realloc(o->binds, sizeof(tl_bindrow) * (size_t)cap);
        if (!nb) return PyErr_NoMemory();
        o->binds = nb;
        o->binds_cap = cap;
    }
    b = &o->binds[o->n_binds++];
    Py_INCREF(obj); Py_INCREF(name);
    b->obj = obj; b->name = name;
    b->field = tl_is_field(src) ? src : NULL;
    Py_XINCREF(b->field);
    b->ch = ch;
    b->comp = comp;
    Py_RETURN_NONE;
}

static PyObject* Timeline_unbind(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    PyObject* obj = Py_None;
    int i, k = 0, n0 = o->n_binds;
    if (!PyArg_ParseTuple(args, "|O", &obj)) return NULL;
    for (i = 0; i < o->n_binds; i++) {
        tl_bindrow* b = &o->binds[i];
        if (obj == Py_None || b->obj == obj) {
            Py_DECREF(b->obj); Py_DECREF(b->name); Py_XDECREF(b->field);
        } else {
            o->binds[k++] = *b;
        }
    }
    o->n_binds = k;
    return PyLong_FromLong(n0 - k);
}

/* --- Timeline: sequences ------------------------------------------------------------------ */

static PyObject* tl_new_seq(TimelineObject* o, ytl_seq q) {
    allocfunc al = (allocfunc)PyType_GetSlot((PyTypeObject*)SeqType, Py_tp_alloc);
    SeqObject* s = (SeqObject*)al((PyTypeObject*)SeqType, 0);
    if (!s) return NULL;
    Py_INCREF(o);
    s->owner = (PyObject*)o;
    s->q = q;
    s->last_id = -1;
    return (PyObject*)s;
}

static PyObject* Timeline_seq(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "base", "now", NULL };
    TimelineObject* o = TL_OF(self);
    int base, now = 0;
    ytl_seq q;
    if (!tl_ready(o) || !PyArg_ParseTupleAndKeywords(args, kw, "i|p", kwlist, &base, &now)) return NULL;
    q = now ? ytl_seq_now(&o->box->tl, base) : ytl_seq_on(&o->box->tl, base);
    if (q.err < 0) return tl_fail(o, q.err);
    return tl_new_seq(o, q);
}

/* One op from a tuple ("on", ch), ("set", ch, v), ("trigger", code[, ch]),
 * ("mark", code[, user]), ("wait", dt), ("at", t), ("to" | "then", ch,
 * value[, options]). */
static int tl_op_in(TimelineObject* o, PyObject* t, ytl_op* op) {
    Py_ssize_t m = PyTuple_Check(t) || PyList_Check(t) ? PySequence_Size(t) : -1;
    PyObject* it[4] = { NULL, NULL, NULL, NULL };
    int i, rc = -1, kind;
    memset(op, 0, sizeof *op);
    if (m < 1 || m > 4) { PyErr_SetString(TlArgumentError, "an op is a tuple such as (\"wait\", 0.5)"); return -1; }
    for (i = 0; i < m; i++) if (!(it[i] = PySequence_GetItem(t, i))) goto done;
    if (tl_enum(it[0], OPS, "op", &kind) < 0) goto done;
    op->op = kind;
    switch (kind) {
    case YTL_OP_ON: case YTL_OP_OFF:
        if (m != 2) break;
        rc = tl_chan(o, it[1], &op->ch);
        goto done;
    case YTL_OP_SET_VALUE:
        if (m != 3) break;
        rc = tl_chan(o, it[1], &op->ch) < 0 ? -1 : tl_float(it[2], &op->value, "value");
        goto done;
    case YTL_OP_TRIGGER:
        if (m != 2 && m != 3) break;
        rc = tl_int32(it[1], &op->code, "code");
        if (rc == 0 && m == 3) rc = tl_int32(it[2], (int32_t*)&op->ch, "trigger channel");
        goto done;
    case YTL_OP_MARK:
        if (m != 2 && m != 3) break;
        rc = tl_int32(it[1], &op->code, "code");
        if (rc == 0 && m == 3) { op->user = PyLong_AsUnsignedLongLongMask(it[2]); rc = PyErr_Occurred() ? -1 : 0; }
        goto done;
    case YTL_OP_WAIT: case YTL_OP_AT:
        if (m != 2) break;
        rc = tl_obj_to_ns(it[1], kind == YTL_OP_WAIT ? "wait" : "at", &op->t_ns);
        goto done;
    case YTL_OP_TO: case YTL_OP_THEN: {
        tl_tw tw;
        if (m != 3 && m != 4) break;
        if (m == 4 && !PyDict_Check(it[3])) { PyErr_SetString(TlArgumentError, "a tween op's options are a dict"); goto done; }
        if (tl_chan(o, it[1], &op->ch) < 0 || tl_tw_parse(m == 4 ? it[3] : NULL, TW_CHANNEL, &tw) < 0) goto done;
        rc = tl_tw_desc(&tw, it[2], &op->tween);
        goto done;
    }
    default: break;
    }
    PyErr_SetString(TlArgumentError, "an op has the wrong number of items");
done:
    for (i = 0; i < 4; i++) Py_XDECREF(it[i]);
    return rc;
}

static ytl_op* tl_ops_in(TimelineObject* o, PyObject* ops, int* n_out) {
    Py_ssize_t n = PySequence_Size(ops), i;
    ytl_op* a;
    if (n < 0) return NULL;
    if (n > INT32_MAX) { PyErr_SetString(TlArgumentError, "ops: too many"); return NULL; }
    a = (ytl_op*)PyMem_Malloc(sizeof(ytl_op) * (size_t)(n ? n : 1));
    if (!a) { PyErr_NoMemory(); return NULL; }
    for (i = 0; i < n; i++) {
        PyObject* t = PySequence_GetItem(ops, i);
        int rc = t ? tl_op_in(o, t, &a[i]) : -1;
        Py_XDECREF(t);
        if (rc < 0) {
            PyObject *et, *ev, *tb;
            PyErr_Fetch(&et, &ev, &tb);
            PyErr_Format(et ? et : TlArgumentError, "op %d: %S", (int)i, ev ? ev : Py_None);
            Py_XDECREF(et); Py_XDECREF(ev); Py_XDECREF(tb);
            PyMem_Free(a);
            return NULL;
        }
    }
    *n_out = (int)n;
    return a;
}

static PyObject* Timeline_check_ops(PyObject* self, PyObject* args) {
    TimelineObject* o = TL_OF(self);
    PyObject* ops;
    int base, n = 0, bad = -1, rc;
    ytl_op* a;
    if (!tl_ready(o) || !PyArg_ParseTuple(args, "iO", &base, &ops)) return NULL;
    if (!(a = tl_ops_in(o, ops, &n))) return NULL;
    rc = ytl_check_ops(&o->box->tl, base, a, n, &bad);
    PyMem_Free(a);
    if (rc < 0) return tl_fail_at(o, rc, bad);
    Py_RETURN_NONE;
}

/* --- Timeline: properties ----------------------------------------------------------------- */

static PyObject* Timeline_get_values(PyObject* self, void* c) {
    TimelineObject* o = TL_OF(self);
    (void)c;
    if (!tl_ready(o)) return NULL;
    Py_INCREF(o->values_mv);
    return o->values_mv;
}
static PyObject* Timeline_get_field_values(PyObject* self, void* c) {
    TimelineObject* o = TL_OF(self);
    (void)c;
    if (!tl_ready(o)) return NULL;
    Py_INCREF(o->fields_mv);
    return o->fields_mv;
}
static PyObject* Timeline_get_channels(PyObject* self, void* c) {
    TimelineObject* o = TL_OF(self);
    (void)c;
    if (o->names_t) { Py_INCREF(o->names_t); return o->names_t; }
    return PyLong_FromLong(o->n_channels);
}
static PyObject* Timeline_get_lead(PyObject* self, void* c) {
    TimelineObject* o = TL_OF(self);
    (void)c;
    return PyFloat_FromDouble(o->box ? ytl_lead(&o->box->tl) : 0.0);
}
static PyObject* Timeline_get_error(PyObject* self, void* c) {
    TimelineObject* o = TL_OF(self);
    (void)c;
    return PyUnicode_FromString(o->box ? ytl_error(&o->box->tl) : "");
}
static PyObject* Timeline_get_is_open(PyObject* self, void* c) {
    (void)c;
    return PyBool_FromLong(TL_OF(self)->open);
}
static PyObject* Timeline_get_frames(PyObject* self, void* c) {
    (void)c;
    return PyLong_FromLongLong(TL_OF(self)->frames);
}

static PyMethodDef Timeline_methods[] = {
    { "close", Timeline_close, METH_NOARGS, "close(): end the use of the timeline; drops callbacks and bindings." },
    { "__enter__", Timeline_enter, METH_NOARGS, NULL },
    { "__exit__", Timeline_exit, METH_VARARGS, NULL },
    { "anchor", (PyCFunction)(void(*)(void))Timeline_anchor, METH_VARARGS | METH_KEYWORDS,
      "anchor(base, rt, bt=0.0): run base with base time bt at RT time rt (seconds). At or before the furthest "
      "evaluated base time this rewinds: the events from bt on fire again (REWIND)." },
    { "pause", Timeline_pause, METH_VARARGS, "pause(base, rt): freeze the base at its time at rt." },
    { "resume", Timeline_resume, METH_VARARGS, "resume(base, rt): run a paused base again from rt." },
    { "stop", Timeline_stop, METH_VARARGS, "stop(base): nothing on the base fires or writes until an anchor." },
    { "skip", Timeline_skip, METH_VARARGS, "skip(base, rt, bt) -> int: seek to bt at rt; the events passed over are "
                                           "marked skipped and never fire. Returns how many." },
    { "rate", Timeline_rate, METH_VARARGS, "rate(base, rt, num, den=1): from rt on, run the base at num/den." },
    { "get_rate", Timeline_get_rate, METH_VARARGS, "get_rate(base) -> (num, den)." },
    { "base_time", Timeline_base_time, METH_VARARGS, "base_time(base, rt) -> seconds, or None when stopped." },
    { "rt_time", Timeline_rt_time, METH_VARARGS, "rt_time(base, bt) -> the first RT time the running base reaches bt, or None." },
    { "window", Timeline_window, METH_VARARGS, "window(base, onset, period=0) -> the base time a frame's window reaches, or None." },
    { "clamped", Timeline_clamped, METH_VARARGS, "clamped(base) -> how many evaluates met a time past 2^62 ns." },
    { "next_due", Timeline_next_due, METH_NOARGS, "next_due() -> the earliest RT time of a pending event, or None." },
    { "horizon", Timeline_horizon, METH_VARARGS, "horizon(base) -> the oldest time a rewind replays exactly after a prune, or None." },
    { "add", (PyCFunction)(void(*)(void))Timeline_add, METH_VARARGS | METH_KEYWORDS,
      "add(time, kind='mark', *, base=0, target=0, value=0.0, code=0, user=0) -> id. kind: 'mark', 'trigger', "
      "'onset', 'offset', 'set' or an int (16 to 255 are yours); target is the channel (an int or a name), or "
      "for a trigger its trigger channel." },
    { "add_many", Timeline_add_many, METH_O, "add_many([dict, ...]) -> first id: add()'s keywords and time, all or none." },
    { "remove", Timeline_remove, METH_VARARGS, "remove(id): delete one event." },
    { "prune", Timeline_prune, METH_VARARGS, "prune(base=ALL_BASES) -> int: delete fired and skipped events; log them first." },
    { "clear", Timeline_clear, METH_VARARGS, "clear(base=ALL_BASES) -> int: delete every event, track and target tween of the base." },
    { "events", Timeline_events, METH_VARARGS, "events(base=ALL_BASES) -> [Event]: the storage, the record of where each landed." },
    { "find", Timeline_find, METH_VARARGS, "find(id) -> Event or None." },
    { "peek", Timeline_peek, METH_VARARGS, "peek(base, start, end) -> [Event]: the pending events whose RT time is in "
                                           "[start, end), onset = that RT time; changes nothing (for sound)." },
    { "set_keys", Timeline_set_keys, METH_VARARGS, "set_keys(channel, base, keys): keys are (time, value) or (time, value, "
                                                   "ease) rows; an ease may be (x1, y1, x2, y2). [] removes the track." },
    { "set_track", (PyCFunction)(void(*)(void))Timeline_set_track, METH_VARARGS | METH_KEYWORDS,
      "set_track(channel, base, *, keys=None, samples=None, t0=0.0, rate=0, interp='linear', period=0.0, repeats=0): "
      "keys, or samples (float32 buffer or sequence) at `rate` per second from t0; a period repeats it. Neither removes." },
    { "tween", (PyCFunction)(void(*)(void))Timeline_tween, METH_VARARGS | METH_KEYWORDS,
      "tween(channel, to, *, base=0, duration=0, start=None, delay=0, from_=None, ease=None, cycles=1, yoyo=False, "
      "keep_velocity=False): a channel tween from 'now' (or start); it takes over from the channel's value there." },
    { "seq", (PyCFunction)(void(*)(void))Timeline_seq, METH_VARARGS | METH_KEYWORDS,
      "seq(base, now=False) -> Seq: a cursor on the base at 0 (or at its 'now')." },
    { "check_ops", Timeline_check_ops, METH_VARARGS, "check_ops(base, ops): raise as Seq.run(ops) on a new sequence would; changes nothing." },
    { "field", (PyCFunction)(void(*)(void))Timeline_field, METH_VARARGS | METH_KEYWORDS,
      "field(value=0.0, *, n=0, int=False) -> Field: a value target tweens write, 1 to 4 float32 (or int32) components." },
    { "fields", (PyCFunction)(void(*)(void))Timeline_fields, METH_VARARGS | METH_KEYWORDS,
      "fields(count, value=0.0, *, n=0, int=False) -> [Field]: count fields in adjacent slots, for a stagger." },
    { "kill", Timeline_kill, METH_O, "kill(field) -> int: kill the field's tweens; it keeps its value." },
    { "kill_tween", Timeline_kill_tween, METH_VARARGS, "kill_tween(id): kill one target tween." },
    { "tween_count", Timeline_tween_count, METH_NOARGS, "tween_count() -> (live target tweens, targets)." },
    { "evaluate", Timeline_evaluate, METH_VARARGS,
      "evaluate(onset, period=0.0, index=None) -> (Event, ...): the frame at its predicted onset (seconds). Fills "
      "values and field_values, applies bind(), and returns the events that fired, in RT order. index defaults to "
      "the count of evaluates so far." },
    { "value", Timeline_value, METH_O, "value(channel) -> float." },
    { "channel", Timeline_channel, METH_O, "channel(name) -> its index." },
    { "bind", (PyCFunction)(void(*)(void))Timeline_bind, METH_VARARGS | METH_KEYWORDS,
      "bind(obj, attr, source, component=-1): after each evaluate, setattr(obj, attr, value of source), a channel "
      "or a Field (a tuple when it has several components, unless component is given)." },
    { "unbind", Timeline_unbind, METH_VARARGS, "unbind(obj=None) -> int: drop the bindings of obj, or all." },
    { NULL }
};

static PyGetSetDef Timeline_getset[] = {
    { "values", Timeline_get_values, NULL, "memoryview of float32: the channel values, written by evaluate()", NULL },
    { "field_values", Timeline_get_field_values, NULL, "memoryview of float32: 4 words per field slot, in slot order", NULL },
    { "channels", Timeline_get_channels, NULL, "the channel names, or their count", NULL },
    { "lead", Timeline_get_lead, NULL, "the lead in use: 0.5 by default, 0 for 'none'", NULL },
    { "error", Timeline_get_error, NULL, "the message of the last failure", NULL },
    { "is_open", Timeline_get_is_open, NULL, "False after close()", NULL },
    { "frames", Timeline_get_frames, NULL, "the next default frame index", NULL },
    { NULL }
};

static PyType_Slot Timeline_slots[] = {
    { Py_tp_init, (void*)Timeline_init }, { Py_tp_dealloc, (void*)Timeline_dealloc },
    { Py_tp_traverse, (void*)Timeline_traverse }, { Py_tp_clear, (void*)Timeline_clear_refs },
    { Py_tp_methods, Timeline_methods }, { Py_tp_getset, Timeline_getset }, { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_doc, (void*)"Timeline(channels=0, *, initial=None, events=1024, keys=1024, fields=64, tweens=256, "
                        "lead=0.0): one ytl_timeline. channels is a count or a list of names; events, keys, fields "
                        "and tweens size the pools; lead is the fraction of a period an event may land early "
                        "(0: half a frame; 'none': never early). Times are seconds." },
    { 0, NULL }
};
static PyType_Spec Timeline_spec = { "ysp.timeline.Timeline", sizeof(TimelineObject), 0,
                                     Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, Timeline_slots };

/* --- Seq ---------------------------------------------------------------------------------- */

#define SEQ_OF(s) ((SeqObject*)(s))

static int Seq_traverse(PyObject* self, visitproc visit, void* arg) {
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(SEQ_OF(self)->owner);
    return 0;
}

static int Seq_clear_refs(PyObject* self) {
    Py_CLEAR(SEQ_OF(self)->owner);
    return 0;
}

static void Seq_dealloc(PyObject* self) {
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    PyObject_GC_UnTrack(self);
    Py_CLEAR(SEQ_OF(self)->owner);
    fr(self);
    Py_DECREF(tp);
}

static PyObject* Seq_new(PyTypeObject* t, PyObject* a, PyObject* k) {
    (void)t; (void)a; (void)k;
    PyErr_SetString(PyExc_TypeError, "a Seq comes from Timeline.seq()");
    return NULL;
}

static TimelineObject* seq_tl(SeqObject* s) {
    TimelineObject* o = s->owner ? TL_OF(s->owner) : NULL;
    return tl_ready(o) ? o : NULL;
}

/* After a builder call: raise the sticky error, else return the sequence
 * for chaining. */
static PyObject* seq_done(SeqObject* s) {
    if (s->q.err < 0) return tl_fail_at(TL_OF(s->owner), s->q.err, s->q.err_call);
    Py_INCREF(s);
    return (PyObject*)s;
}

static PyObject* Seq_on(PyObject* self, PyObject* arg) {
    SeqObject* s = SEQ_OF(self);
    TimelineObject* o = seq_tl(s);
    int ch;
    if (!o || tl_chan(o, arg, &ch) < 0) return NULL;
    ytl_on(&s->q, ch);
    return seq_done(s);
}

static PyObject* Seq_off(PyObject* self, PyObject* arg) {
    SeqObject* s = SEQ_OF(self);
    TimelineObject* o = seq_tl(s);
    int ch;
    if (!o || tl_chan(o, arg, &ch) < 0) return NULL;
    ytl_off(&s->q, ch);
    return seq_done(s);
}

static PyObject* Seq_wait(PyObject* self, PyObject* arg) {
    SeqObject* s = SEQ_OF(self);
    int64_t dt;
    if (!seq_tl(s) || tl_obj_to_ns(arg, "wait", &dt) < 0) return NULL;
    ytl_wait(&s->q, dt);
    return seq_done(s);
}

static PyObject* Seq_at(PyObject* self, PyObject* arg) {
    SeqObject* s = SEQ_OF(self);
    int64_t t;
    if (!seq_tl(s) || tl_obj_to_ns(arg, "at", &t) < 0) return NULL;
    ytl_at(&s->q, t);
    return seq_done(s);
}

static PyObject* Seq_trigger(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "code", "ch", NULL };
    SeqObject* s = SEQ_OF(self);
    long code, ch = 0;
    if (!seq_tl(s) || !PyArg_ParseTupleAndKeywords(args, kw, "l|l", kwlist, &code, &ch)) return NULL;
    if (code < INT32_MIN || code > INT32_MAX || ch < INT32_MIN || ch > INT32_MAX) {
        PyErr_SetString(TlArgumentError, "trigger: code and ch are int32");
        return NULL;
    }
    ytl_trigger_ch(&s->q, (int)ch, (int32_t)code);
    return seq_done(s);
}

static PyObject* Seq_mark(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "code", "user", NULL };
    SeqObject* s = SEQ_OF(self);
    long code;
    unsigned long long user = 0;
    if (!seq_tl(s) || !PyArg_ParseTupleAndKeywords(args, kw, "l|K", kwlist, &code, &user)) return NULL;
    if (code < INT32_MIN || code > INT32_MAX) { PyErr_SetString(TlArgumentError, "mark: code is int32"); return NULL; }
    ytl_mark(&s->q, (int32_t)code, (uint64_t)user);
    return seq_done(s);
}

static PyObject* Seq_on_n(PyObject* self, PyObject* args) {
    SeqObject* s = SEQ_OF(self);
    TimelineObject* o = seq_tl(s);
    PyObject* chs;
    double st = 0.0;
    int64_t sn;
    Py_ssize_t n, i;
    int* a;
    if (!o || !PyArg_ParseTuple(args, "O|d", &chs, &st) || tl_s_to_ns(st, "stagger", &sn) < 0) return NULL;
    if ((n = PySequence_Size(chs)) < 0) return NULL;
    if (n > YTL_MAX_CHANNELS) { PyErr_SetString(TlArgumentError, "on_n: too many channels"); return NULL; }
    a = (int*)PyMem_Malloc(sizeof(int) * (size_t)(n ? n : 1));
    if (!a) return PyErr_NoMemory();
    for (i = 0; i < n; i++) {
        PyObject* c = PySequence_GetItem(chs, i);
        int rc = c ? tl_chan(o, c, &a[i]) : -1;
        Py_XDECREF(c);
        if (rc < 0) { PyMem_Free(a); return NULL; }
    }
    ytl_on_n(&s->q, a, (int)n, sn);
    PyMem_Free(a);
    return seq_done(s);
}

static PyObject* Seq_keyframes(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "channel", "values", "offsets", "ease", NULL };
    SeqObject* s = SEQ_OF(self);
    TimelineObject* o = seq_tl(s);
    PyObject *cho, *vals, *offs, *ease = NULL;
    int ch, e;
    ytl_curve c;
    Py_ssize_t n, i;
    float* v;
    int64_t* t;
    if (!o || !PyArg_ParseTupleAndKeywords(args, kw, "OOO|O", kwlist, &cho, &vals, &offs, &ease)) return NULL;
    if (tl_chan(o, cho, &ch) < 0 || tl_ease(ease, &e, &c) < 0) return NULL;
    if ((n = PySequence_Size(vals)) < 0) return NULL;
    if (PySequence_Size(offs) != n) { PyErr_SetString(TlArgumentError, "keyframes: one offset per value"); return NULL; }
    if (n > INT32_MAX) { PyErr_SetString(TlArgumentError, "keyframes: too many"); return NULL; }
    v = (float*)PyMem_Malloc(sizeof(float) * (size_t)(n ? n : 1));
    t = (int64_t*)PyMem_Malloc(sizeof(int64_t) * (size_t)(n ? n : 1));
    if (!v || !t) { PyMem_Free(v); PyMem_Free(t); return PyErr_NoMemory(); }
    for (i = 0; i < n; i++) {
        PyObject *a = PySequence_GetItem(vals, i), *b = PySequence_GetItem(offs, i);
        int rc = a && b && tl_float(a, &v[i], "value") == 0 && tl_obj_to_ns(b, "offset", &t[i]) == 0 ? 0 : -1;
        Py_XDECREF(a); Py_XDECREF(b);
        if (rc < 0) { PyMem_Free(v); PyMem_Free(t); return NULL; }
    }
    ytl_keyframes(&s->q, ch, v, t, (int)n, e);
    PyMem_Free(v);
    PyMem_Free(t);
    return seq_done(s);
}

/* A field's n components from a number (n = 1) or a sequence of n. */
static int tl_comps(PyObject* v, int n, float* out) {
    int i;
    if (PyTuple_Check(v) || PyList_Check(v)) {
        if (PySequence_Size(v) != n) { PyErr_Format(TlArgumentError, "the field has %d components", n); return -1; }
        for (i = 0; i < n; i++) {
            PyObject* x = PySequence_GetItem(v, i);
            int rc = x ? tl_float(x, &out[i], "value") : -1;
            Py_XDECREF(x);
            if (rc < 0) return -1;
        }
        return 0;
    }
    if (n != 1) { PyErr_Format(TlArgumentError, "the field has %d components: give a tuple", n); return -1; }
    return tl_float(v, &out[0], "value");
}

/* A target tween on one field, or on a list of fields in adjacent slots
 * (Timeline.fields()), staggered. mode: 0 to, 1 from, 2 from_to, 3 set. */
static PyObject* seq_anim(SeqObject* s, TimelineObject* o, PyObject* target, PyObject* a1, PyObject* a2, PyObject* kw,
                          int mode, int then) {
    ytl_anim a;
    tl_tw t;
    FieldObject* f0;
    int count = 1, i, id;
    PyObject* first;
    if (tl_tw_parse(kw, TW_CHANNEL | TW_FIELD, &t) < 0) return NULL;
    if (tl_is_field(target)) {
        first = target;
    } else {
        Py_ssize_t m = PySequence_Size(target);
        if (m < 1) { PyErr_Clear(); PyErr_SetString(PyExc_TypeError, "a target is a channel, a Field or a list of Fields"); return NULL; }
        first = PySequence_GetItem(target, 0);
        if (!first) return NULL;
        Py_DECREF(first);   /* the list holds it */
        if (!tl_is_field(first)) { PyErr_SetString(PyExc_TypeError, "a target list holds Fields"); return NULL; }
        count = (int)m;
        for (i = 1; i < count; i++) {
            PyObject* fi = PySequence_GetItem(target, i);
            FieldObject* ff = (FieldObject*)fi;
            int ok = fi && tl_is_field(fi) && ff->owner == s->owner && ff->index == ((FieldObject*)first)->index + i;
            Py_XDECREF(fi);
            if (!ok) {
                if (!PyErr_Occurred())
                    PyErr_SetString(TlArgumentError, "a staggered tween needs fields in adjacent slots: make them with Timeline.fields(n)");
                return NULL;
            }
        }
    }
    f0 = (FieldObject*)first;
    if (f0->owner != s->owner || !tl_field_live(first)) { PyErr_SetString(TlArgumentError, "a freed Field, or one of another Timeline"); return NULL; }
    memset(&a, 0, sizeof a);
    a.target = o->fields + 4 * f0->index;
    a.type = f0->type;
    a.n = f0->n;
    if (mode != 0 && t.from) { PyErr_SetString(PyExc_TypeError, "from_ is the call's first value here"); return NULL; }
    /* Values go in as float for both types: ytl_anim.to is float, and the
     * header rounds an int32 field's value when it writes it. */
    if (mode != 1 && tl_comps(mode == 2 ? a2 : a1, f0->n, a.to) < 0) return NULL;
    if (mode == 1 || mode == 2 || t.from) {
        if (tl_comps(mode == 0 ? t.from : a1, f0->n, a.from) < 0) return NULL;
        a.from_set = true;
    }
    a.to_current = mode == 1;
    a.duration_ns = mode == 3 ? 0 : t.duration;
    a.delay_ns = t.delay;
    a.stagger_ns = t.stagger;
    a.ease = mode == 3 ? YTL_EASE_STEP : t.ease;
    a.curve = t.curve;
    a.cycles = t.cycles;
    a.overwrite = t.overwrite;
    a.yoyo = t.yoyo != 0;
    a.keep_velocity = t.kv != 0;
    a.immediate = t.immediate >= 0 ? t.immediate != 0 : (mode == 1 || mode == 2);
    a.user = (void*)(uintptr_t)t.user;
    if (t.on_start && t.on_start != Py_None && t.on_start != Py_False) a.on_start = tl_cb_start;
    if (t.on_complete && t.on_complete != Py_None && t.on_complete != Py_False) a.on_complete = tl_cb_complete;
    id = ytl_animate_n(&s->q, &a, 16, count);
    if (id < 0) return seq_done(s);
    s->last_id = id + count - 1;
    if (a.on_start || a.on_complete) {
        /* True asks for the events only; a callable is called too. */
        PyObject *st = a.on_start && PyCallable_Check(t.on_start) ? t.on_start : Py_None;
        PyObject *co = a.on_complete && PyCallable_Check(t.on_complete) ? t.on_complete : Py_None;
        if (st != Py_None || co != Py_None) {
            for (i = 0; i < count; i++) {
                PyObject *key = PyLong_FromLong(id + i), *pair = PyTuple_Pack(2, st, co);
                int rc = key && pair ? PyDict_SetItem(o->callbacks, key, pair) : -1;
                Py_XDECREF(key); Py_XDECREF(pair);
                if (rc < 0) return NULL;
            }
        }
    }
    if (a.overwrite == YTL_OVERWRITE_ALL) tl_sweep(o);
    if (then) ytl_at(&s->q, s->q.prev_end);
    return seq_done(s);
}

/* to(target, value, **options): a channel tween (ytl_to), or a field tween
 * (ytl_animate) when target is a Field or a list of them. */
static PyObject* seq_tween(PyObject* self, PyObject* args, PyObject* kw, int mode, int then) {
    SeqObject* s = SEQ_OF(self);
    TimelineObject* o = seq_tl(s);
    PyObject *target, *a1 = NULL, *a2 = NULL;
    if (!o) return NULL;
    if (mode == 2) { if (!PyArg_ParseTuple(args, "OOO", &target, &a1, &a2)) return NULL; }
    else if (!PyArg_ParseTuple(args, "OO", &target, &a1)) return NULL;
    if (tl_is_field(target) || PyList_Check(target) || PyTuple_Check(target))
        return seq_anim(s, o, target, a1, a2, kw, mode, then);
    {
        int ch;
        tl_tw t;
        ytl_tween_desc d;
        if (mode == 1) { PyErr_SetString(PyExc_TypeError, "from_() takes a Field; for a channel use to(ch, v, from_=x)"); return NULL; }
        if (tl_chan(o, target, &ch) < 0 || tl_tw_parse(kw, TW_CHANNEL, &t) < 0) return NULL;
        if (mode == 2) {
            if (t.from) { PyErr_SetString(PyExc_TypeError, "from_to() takes from as its second argument"); return NULL; }
            t.from = a1;
            a1 = a2;
        }
        if (tl_tw_desc(&t, a1, &d) < 0) return NULL;
        if (then) ytl_then(&s->q, ch, &d);
        else ytl_to(&s->q, ch, &d);
        return seq_done(s);
    }
}

static PyObject* Seq_to(PyObject* self, PyObject* args, PyObject* kw) { return seq_tween(self, args, kw, 0, 0); }
static PyObject* Seq_then(PyObject* self, PyObject* args, PyObject* kw) { return seq_tween(self, args, kw, 0, 1); }
static PyObject* Seq_from(PyObject* self, PyObject* args, PyObject* kw) { return seq_tween(self, args, kw, 1, 0); }
static PyObject* Seq_from_to(PyObject* self, PyObject* args, PyObject* kw) { return seq_tween(self, args, kw, 2, 0); }

/* set(target, value): a SET event on a channel, or a field's set (a STEP
 * jump with a record). */
static PyObject* Seq_set(PyObject* self, PyObject* args, PyObject* kw) {
    SeqObject* s = SEQ_OF(self);
    TimelineObject* o = seq_tl(s);
    PyObject *target, *v;
    if (!o || !PyArg_ParseTuple(args, "OO", &target, &v)) return NULL;
    if (tl_is_field(target) || PyList_Check(target) || PyTuple_Check(target))
        return seq_anim(s, o, target, v, NULL, kw, 3, 0);
    {
        int ch;
        float x;
        if (kw && PyDict_Size(kw)) { PyErr_SetString(PyExc_TypeError, "a channel's set takes no options"); return NULL; }
        if (tl_chan(o, target, &ch) < 0 || tl_float(v, &x, "value") < 0) return NULL;
        ytl_set_value(&s->q, ch, x);
        return seq_done(s);
    }
}

static PyObject* Seq_run(PyObject* self, PyObject* arg) {
    SeqObject* s = SEQ_OF(self);
    TimelineObject* o = seq_tl(s);
    ytl_op* a;
    int n = 0;
    if (!o || !(a = tl_ops_in(o, arg, &n))) return NULL;
    ytl_run(&s->q, a, n);
    PyMem_Free(a);
    return seq_done(s);
}

static PyObject* Seq_cancel(PyObject* self, PyObject* unused) {
    SeqObject* s = SEQ_OF(self);
    TimelineObject* o = seq_tl(s);
    int rc;
    (void)unused;
    if (!o) return NULL;
    rc = ytl_seq_cancel(&s->q);
    if (rc < 0) return tl_fail(o, rc);
    tl_sweep(o);
    return PyLong_FromLong(rc);
}

static PyObject* Seq_get_t(PyObject* self, void* c) { (void)c; return PyFloat_FromDouble(tl_ns_to_s(SEQ_OF(self)->q.t)); }
static PyObject* Seq_get_t_ns(PyObject* self, void* c) { (void)c; return PyLong_FromLongLong(SEQ_OF(self)->q.t); }
static PyObject* Seq_get_ps(PyObject* self, void* c) { (void)c; return PyFloat_FromDouble(tl_ns_to_s(SEQ_OF(self)->q.prev_start)); }
static PyObject* Seq_get_pe(PyObject* self, void* c) { (void)c; return PyFloat_FromDouble(tl_ns_to_s(SEQ_OF(self)->q.prev_end)); }
static PyObject* Seq_get_ps_ns(PyObject* self, void* c) { (void)c; return PyLong_FromLongLong(SEQ_OF(self)->q.prev_start); }
static PyObject* Seq_get_pe_ns(PyObject* self, void* c) { (void)c; return PyLong_FromLongLong(SEQ_OF(self)->q.prev_end); }
static PyObject* Seq_get_base(PyObject* self, void* c) { (void)c; return PyLong_FromLong(SEQ_OF(self)->q.base); }
static PyObject* Seq_get_last_id(PyObject* self, void* c) { (void)c; return PyLong_FromLong(SEQ_OF(self)->last_id); }
static PyObject* Seq_get_error(PyObject* self, void* c) { (void)c; return PyLong_FromLong(SEQ_OF(self)->q.err); }

static PyMethodDef Seq_methods[] = {
    { "on", Seq_on, METH_O, "on(channel): ONSET at the cursor (the channel becomes 1)." },
    { "off", Seq_off, METH_O, "off(channel): OFFSET at the cursor (0)." },
    { "set", (PyCFunction)(void(*)(void))Seq_set, METH_VARARGS | METH_KEYWORDS,
      "set(target, value): a SET event on a channel, or a Field's set (gsap .set()), which lands where an event lands." },
    { "trigger", (PyCFunction)(void(*)(void))Seq_trigger, METH_VARARGS | METH_KEYWORDS, "trigger(code, ch=0): a TRIGGER at the cursor." },
    { "mark", (PyCFunction)(void(*)(void))Seq_mark, METH_VARARGS | METH_KEYWORDS, "mark(code, user=0): a MARK at the cursor." },
    { "wait", Seq_wait, METH_O, "wait(dt): move the cursor dt seconds on (gsap \"+=dt\")." },
    { "at", Seq_at, METH_O, "at(t): move the cursor to base time t (a label is a saved Seq.t)." },
    { "to", (PyCFunction)(void(*)(void))Seq_to, METH_VARARGS | METH_KEYWORDS,
      "to(target, value, *, duration=0, delay=0, ease=None, from_=None, cycles=1, yoyo=False, keep_velocity=False, "
      "[Field only:] immediate=None, overwrite=False, on_start=None, on_complete=None, user=0, stagger=0): a tween "
      "from the cursor (gsap .to(target, vars, \"<\")); the cursor stays. target: a channel, a Field or a list from "
      "Timeline.fields()." },
    { "then", (PyCFunction)(void(*)(void))Seq_then, METH_VARARGS | METH_KEYWORDS, "then(target, value, **options): to(), and the cursor moves to its end." },
    { "from_", (PyCFunction)(void(*)(void))Seq_from, METH_VARARGS | METH_KEYWORDS,
      "from_(field, value, **options): gsap.from(): from value to the field's value at the start, showing value until then." },
    { "from_to", (PyCFunction)(void(*)(void))Seq_from_to, METH_VARARGS | METH_KEYWORDS, "from_to(target, a, b, **options): gsap.fromTo()." },
    { "keyframes", (PyCFunction)(void(*)(void))Seq_keyframes, METH_VARARGS | METH_KEYWORDS,
      "keyframes(channel, values, offsets, ease=None): values[i] at the cursor + offsets[i] (seconds, rising)." },
    { "on_n", Seq_on_n, METH_VARARGS, "on_n(channels, stagger=0.0): ONSET on each, stagger seconds apart, all or none." },
    { "run", Seq_run, METH_O, "run(ops): an op table, all or none: (\"on\", ch), (\"off\", ch), (\"set\", ch, v), "
                              "(\"trigger\", code[, ch]), (\"mark\", code[, user]), (\"wait\", dt), (\"at\", t), "
                              "(\"to\" | \"then\", ch, value[, options dict])." },
    { "cancel", Seq_cancel, METH_NOARGS, "cancel() -> int: delete what the sequence added; the sequence does nothing more." },
    { NULL }
};

static PyGetSetDef Seq_getset[] = {
    { "t", Seq_get_t, NULL, "the cursor, base time in seconds", NULL },
    { "t_ns", Seq_get_t_ns, NULL, "the cursor, exact ns", NULL },
    { "prev_start", Seq_get_ps, NULL, "the start of what the last call added (gsap \"<\")", NULL },
    { "prev_end", Seq_get_pe, NULL, "its end (gsap \">\")", NULL },
    { "prev_start_ns", Seq_get_ps_ns, NULL, "prev_start, exact ns", NULL },
    { "prev_end_ns", Seq_get_pe_ns, NULL, "prev_end, exact ns", NULL },
    { "base", Seq_get_base, NULL, "the base", NULL },
    { "last_id", Seq_get_last_id, NULL, "the id of the last field tween, for kill_tween(); -1 before one", NULL },
    { "error", Seq_get_error, NULL, "0, or the sticky error code", NULL },
    { NULL }
};

static PyType_Slot Seq_slots[] = {
    { Py_tp_dealloc, (void*)Seq_dealloc }, { Py_tp_methods, Seq_methods }, { Py_tp_getset, Seq_getset },
    { Py_tp_traverse, (void*)Seq_traverse }, { Py_tp_clear, (void*)Seq_clear_refs },
    { Py_tp_new, (void*)Seq_new },
    { Py_tp_doc, (void*)"A sequence: a cursor on one base for writing a trial in the order it plays (ytl_seq). "
                        "Every method returns the sequence, and the first failing call raises; the calls before "
                        "it stay (cancel() removes them)." },
    { 0, NULL }
};
static PyType_Spec Seq_spec = { "ysp.timeline.Seq", sizeof(SeqObject), 0, Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, Seq_slots };

/* --- Field -------------------------------------------------------------------------------- */

/* Give the slot back: its tweens die with it, or a later field in the
 * slot would be driven by them. */
static void tl_field_release(FieldObject* f) {
    TimelineObject* o = f->owner ? TL_OF(f->owner) : NULL;
    if (!o || f->index < 0) return;
    if (o->open && o->box) ytl_kill(&o->box->tl, o->fields + 4 * f->index);
    if (o->fused) o->fused[f->index] = 0;
    if (o->open) tl_sweep(o);
    f->index = -1;
}

static int Field_traverse(PyObject* self, visitproc visit, void* arg) {
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(((FieldObject*)self)->owner);
    return 0;
}

static int Field_clear_refs(PyObject* self) {
    FieldObject* f = (FieldObject*)self;
    tl_field_release(f);
    Py_CLEAR(f->owner);
    return 0;
}

static void Field_dealloc(PyObject* self) {
    FieldObject* f = (FieldObject*)self;
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    PyObject *et, *ev, *tb;
    PyObject_GC_UnTrack(self);
    /* A dealloc can run while an exception propagates; the sweep must not
     * clear it. */
    PyErr_Fetch(&et, &ev, &tb);
    tl_field_release(f);
    Py_CLEAR(f->owner);
    PyErr_Restore(et, ev, tb);
    fr(self);
    Py_DECREF(tp);
}

static TimelineObject* field_tl(FieldObject* f) {
    if (!f->owner || f->index < 0) { PyErr_SetString(TlArgumentError, "the field was freed"); return NULL; }
    return TL_OF(f->owner);
}

static PyObject* Field_free(PyObject* self, PyObject* unused) {
    FieldObject* f = (FieldObject*)self;
    TimelineObject* o;
    (void)unused;
    if (!(o = field_tl(f))) return NULL;
    tl_field_release(f);
    if (o->field_list) {
        Py_ssize_t i, n = PyList_Size(o->field_list);
        for (i = 0; i < n; i++)
            if (PyList_GetItem(o->field_list, i) == self) { PySequence_DelItem(o->field_list, i); break; }
    }
    Py_RETURN_NONE;
}

static PyObject* Field_new(PyTypeObject* t, PyObject* a, PyObject* k) {
    (void)t; (void)a; (void)k;
    PyErr_SetString(PyExc_TypeError, "a Field comes from Timeline.field()");
    return NULL;
}

static PyObject* Field_get_value(PyObject* self, void* c) {
    FieldObject* f = (FieldObject*)self;
    (void)c;
    if (!field_tl(f)) return NULL;
    return tl_field_get(TL_OF(f->owner), f->index, f->n, f->type);
}
static int Field_set_value(PyObject* self, PyObject* v, void* c) {
    FieldObject* f = (FieldObject*)self;
    (void)c;
    if (!v) { PyErr_SetString(PyExc_TypeError, "cannot delete value"); return -1; }
    if (!field_tl(f)) return -1;
    return tl_field_set(TL_OF(f->owner), f->index, f->n, f->type, v);
}
static PyObject* Field_get_index(PyObject* self, void* c) { (void)c; return PyLong_FromLong(((FieldObject*)self)->index); }
static PyObject* Field_get_n(PyObject* self, void* c) { (void)c; return PyLong_FromLong(((FieldObject*)self)->n); }
static PyObject* Field_get_is_int(PyObject* self, void* c) { (void)c; return PyBool_FromLong(((FieldObject*)self)->type == YTL_I32); }

static PyObject* Field_kill(PyObject* self, PyObject* unused) {
    (void)unused;
    if (!field_tl((FieldObject*)self)) return NULL;
    return Timeline_kill(((FieldObject*)self)->owner, self);
}

static PyObject* Field_float(PyObject* self) {
    FieldObject* f = (FieldObject*)self;
    const float* p;
    if (!field_tl(f)) return NULL;
    p = TL_OF(f->owner)->fields + 4 * f->index;
    return PyFloat_FromDouble(f->type == YTL_I32 ? (double)*(const int32_t*)(const void*)p : (double)p[0]);
}

static PyMethodDef Field_methods[] = {
    { "kill", Field_kill, METH_NOARGS, "kill() -> int: kill this field's tweens (gsap.killTweensOf)." },
    { "free", Field_free, METH_NOARGS, "free(): kill its tweens and give its slot back; the Timeline keeps a field "
                                       "until this or close()." },
    { NULL }
};
static PyGetSetDef Field_getset[] = {
    { "value", Field_get_value, Field_set_value, "the value: a float (an int when int=True), or a tuple of n", NULL },
    { "index", Field_get_index, NULL, "the slot: field_values[4 * index] is component 0", NULL },
    { "n", Field_get_n, NULL, "components, 1 to 4", NULL },
    { "is_int", Field_get_is_int, NULL, "int32 components, rounded to nearest", NULL },
    { NULL }
};
static PyType_Slot Field_slots[] = {
    { Py_tp_dealloc, (void*)Field_dealloc }, { Py_tp_methods, Field_methods }, { Py_tp_getset, Field_getset },
    { Py_tp_traverse, (void*)Field_traverse }, { Py_tp_clear, (void*)Field_clear_refs },
    { Py_tp_new, (void*)Field_new }, { Py_nb_float, (void*)Field_float },
    { Py_tp_doc, (void*)"A value target tweens write (a ytl_anim target), made by Timeline.field(). The timeline "
                        "reads its value once, before the first tween, and writes it from then on." },
    { 0, NULL }
};
static PyType_Spec Field_spec = { "ysp.timeline.Field", sizeof(FieldObject), 0, Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, Field_slots };

/* --- module ------------------------------------------------------------------------------- */

static PyObject* mod_version(PyObject* m, PyObject* unused) {
    (void)m; (void)unused;
    return PyUnicode_FromString(ytl_version());
}

static PyMethodDef module_methods[] = {
    { "version", mod_version, METH_NOARGS, "version() -> the header's version string." },
    { NULL }
};

static PyModuleDef ysp_timeline_module = {
    PyModuleDef_HEAD_INIT, "ysp.timeline",
    "Events, tracks and tweens on a frame clock (ysp/timeline.h): build a trial, then evaluate it at each frame's "
    "predicted onset and read the values and the events that landed.", -1, module_methods,
    NULL, NULL, NULL, NULL,
};

static PyObject* tl_add_exc(PyObject* m, const char* qual, const char* attr, PyObject* base) {
    PyObject* e = PyErr_NewException(qual, base, NULL);
    if (!e) return NULL;
    Py_INCREF(e);
    if (PyModule_AddObject(m, attr, e) < 0) { Py_DECREF(e); Py_DECREF(e); return NULL; }
    return e;
}

static PyObject* tl_add_type(PyObject* m, PyType_Spec* spec, const char* name) {
    PyObject* t = PyType_FromSpec(spec);
    if (!t) return NULL;
    Py_INCREF(t);
    if (PyModule_AddObject(m, name, t) < 0) { Py_DECREF(t); Py_DECREF(t); return NULL; }
    return t;
}

PyMODINIT_FUNC PyInit_timeline(void) {
    PyObject *m, *coll, *nt, *a, *k, *base2;
    int i;
    static const struct { const char* name; long v; } consts[] = {
        { "BASE_RT", YTL_BASE_RT }, { "ALL_BASES", YTL_ALL_BASES }, { "MAX_BASES", YTL_MAX_BASES },
        { "MAX_CHANNELS", YTL_MAX_CHANNELS }, { "FOREVER", YTL_FOREVER },
        { "MARK", YTL_MARK }, { "TRIGGER", YTL_TRIGGER }, { "ONSET", YTL_ONSET }, { "OFFSET", YTL_OFFSET },
        { "SET", YTL_SET }, { "CALL_START", YTL_CALL_START }, { "CALL_COMPLETE", YTL_CALL_COMPLETE },
        { "TARGET_SET", YTL_TARGET_SET }, { "KIND_USER", YTL_KIND_USER },
        { "EV_FIRED", YTL_EV_FIRED }, { "EV_LATE", YTL_EV_LATE }, { "EV_SKIPPED", YTL_EV_SKIPPED },
        { "EASE_LINEAR", YTL_EASE_LINEAR }, { "EASE_STEP", YTL_EASE_STEP }, { "EASE_COSINE", YTL_EASE_COSINE },
        { "EASE_QUAD_IN", YTL_EASE_QUAD_IN }, { "EASE_QUAD_OUT", YTL_EASE_QUAD_OUT }, { "EASE_LOG", YTL_EASE_LOG },
        { "EASE_BEZIER", YTL_EASE_BEZIER }, { "EASE_QUAD_IN_OUT", YTL_EASE_QUAD_IN_OUT },
        { "EASE_CUBIC_IN", YTL_EASE_CUBIC_IN }, { "EASE_CUBIC_OUT", YTL_EASE_CUBIC_OUT },
        { "EASE_CUBIC_IN_OUT", YTL_EASE_CUBIC_IN_OUT },
        { "INTERP_LINEAR", YTL_INTERP_LINEAR }, { "INTERP_STEP", YTL_INTERP_STEP }, { "INTERP_CUBIC", YTL_INTERP_CUBIC },
    };
    m = PyModule_Create(&ysp_timeline_module);
    if (!m) return NULL;
    if (!tl_add_type(m, &Timeline_spec, "Timeline")) goto fail;
    if (!(SeqType = tl_add_type(m, &Seq_spec, "Seq"))) goto fail;
    if (!(FieldType = tl_add_type(m, &Field_spec, "Field"))) goto fail;
    if (!(TlError = tl_add_exc(m, "ysp.timeline.Error", "Error", NULL))) goto fail;
    base2 = PyTuple_Pack(2, TlError, PyExc_ValueError);
    if (!base2) goto fail;
    TlArgumentError = tl_add_exc(m, "ysp.timeline.ArgumentError", "ArgumentError", base2);
    Py_DECREF(base2);
    TlClosedError = tl_add_exc(m, "ysp.timeline.ClosedError", "ClosedError", TlError);
    TlOrderError = tl_add_exc(m, "ysp.timeline.OrderError", "OrderError", TlError);
    TlFullError = tl_add_exc(m, "ysp.timeline.FullError", "FullError", TlError);
    TlBoundError = tl_add_exc(m, "ysp.timeline.BoundError", "BoundError", TlError);
    TlNotFoundError = tl_add_exc(m, "ysp.timeline.NotFoundError", "NotFoundError", TlError);
    if (!TlArgumentError || !TlClosedError || !TlOrderError || !TlFullError || !TlBoundError || !TlNotFoundError) goto fail;
    coll = PyImport_ImportModule("collections");
    if (!coll) goto fail;
    nt = PyObject_GetAttrString(coll, "namedtuple");
    a = Py_BuildValue("(ss)", "Event", "id kind base time target value code user flags frame onset residual "
                                       "time_ns onset_ns residual_ns");
    k = Py_BuildValue("{s:s}", "module", "ysp.timeline");
    EventType = (nt && a && k) ? PyObject_Call(nt, a, k) : NULL;
    Py_XDECREF(nt); Py_XDECREF(a); Py_XDECREF(k); Py_DECREF(coll);
    if (!EventType) goto fail;
    Py_INCREF(EventType);
    if (PyModule_AddObject(m, "Event", EventType) < 0) { Py_DECREF(EventType); goto fail; }
    for (i = 0; i < (int)(sizeof consts / sizeof consts[0]); i++)
        if (PyModule_AddIntConstant(m, consts[i].name, consts[i].v) < 0) goto fail;
    if (PyModule_AddObject(m, "LEAD_NONE", PyFloat_FromDouble(YTL_LEAD_NONE)) < 0) goto fail;
    if (PyModule_AddStringConstant(m, "__version__", TL_VERSION) < 0) goto fail;
    return m;
fail:
    Py_DECREF(m);
    return NULL;
}
