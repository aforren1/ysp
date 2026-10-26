/* ysp_response_ext.c - CPython extension wrapping ysp/response.h (module ysp.response)
 *
 * A thin, dependency-free binding: it needs only Python.h, and compiles the
 * header's implementation into the module (with ysp/input.h, which the
 * header includes). Built against the stable ABI / Limited API (CPython
 * 3.8+), so the types are heap types made with PyType_FromSpec and
 * PyStructSequence_NewType.
 *
 *     import ysp.response as yr
 *     c = yr.Collector(["f", "j"], duration=1.5, min_rt=0.1)
 *     c.arm(t_fixation)
 *     c.set_onset(t_flip, src="flip")
 *     c.key(t_down, "f")                    # or c.feed(event), c.feed_many(events)
 *     if c.update(now) == yr.ENDED: ...
 *     r = c.finish()                        # r.rt, r.response, r.onset_tier, ...
 *
 * The input event is the structseq Event (t, t_ns, device, control, code,
 * mods, kind, type, flags, stamp, unc, x, y, value, aux, ticks), the layout
 * ysp.box and ysp.device use too. feed() takes any 16-tuple in that order
 * (so their Events go through without an import) or any object with those
 * attributes, and reads it without allocating.
 *
 * THREADING: none. The header is pure computation on objects the GIL
 * guards. A reader thread queues its events; the frame thread feeds them.
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x03080000
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"

#include <math.h>
#include <string.h>

static PyObject* RspError;
static PyObject* RspArgumentError;
static PyObject* RspStateError;
static PyTypeObject* EventType;
static PyTypeObject* ResultType;
static PyTypeObject* EntryType;

/* Attribute names of the event, interned once so feed() of an object that
 * is not a tuple allocates nothing for the lookups. */
#define EV_NFIELDS 16
static PyObject* ev_attr[EV_NFIELDS];
static const char* const ev_names[EV_NFIELDS] = { "t", "t_ns", "device", "control", "code", "mods", "kind", "type",
                                                  "flags", "stamp", "unc", "x", "y", "value", "aux", "ticks" };
enum { F_T, F_TNS, F_DEVICE, F_CONTROL, F_CODE, F_MODS, F_KIND, F_TYPE, F_FLAGS, F_STAMP, F_UNC, F_X, F_Y,
       F_VALUE, F_AUX, F_TICKS };

static PyObject* rsp_fail_code(int code, const char* msg) {
    PyObject* exc = code == YRSP_ERR_STATE ? RspStateError : code == YRSP_ERR_ARG ? RspArgumentError : RspError;
    PyErr_SetString(exc, msg && msg[0] ? msg : yrsp_strerror(code));
    return NULL;
}

/* --- seconds and ns ------------------------------------------------------- */

#define S_NEG 1   /* a difference (a residual, a bound): may be negative */
#define S_INF 2   /* math.inf is accepted and gives *out = INT64_MAX     */

/* Seconds to int64 ns, rounded to the nearest ns. The whole seconds convert
 * exactly and only the fraction is scaled, so a clock time of days keeps its
 * nanoseconds. NaN is refused, and a negative time unless it is a
 * difference: the header would take it and classify against it. */
static int rsp_seconds(PyObject* o, const char* what, int flags, int64_t* out) {
    double s = PyFloat_AsDouble(o), whole;
    if (s == -1.0 && PyErr_Occurred()) return -1;
    /* Comparisons, not isnan(): MinGW's macro converts through float. */
    if (s != s || (!(flags & S_NEG) && s < 0)) {
        PyErr_Format(RspArgumentError, "%s: seconds, %s(not NaN)", what, (flags & S_NEG) ? "" : ">= 0 ");
        return -1;
    }
    if (s == HUGE_VAL && (flags & S_INF)) { *out = INT64_MAX; return 0; }
    if (!(fabs(s) < 9223372036.0)) {   /* INT64_MAX ns is 9223372036.85 s */
        PyErr_Format(RspArgumentError, "%s: seconds, finite and below 9223372036 (int64 ns)", what);
        return -1;
    }
    whole = floor(s);
    *out = (int64_t)whole * 1000000000 + (int64_t)llround((s - whole) * 1e9);
    return 0;
}

static PyObject* rsp_s(int64_t ns) { return PyFloat_FromDouble((double)ns / 1e9); }

/* An integer field in lo..hi: an int (or anything with __index__), or a
 * float with an integral value, as a table column from pandas has. None is
 * 0. */
static int rsp_int(PyObject* o, long long lo, long long hi, const char* what, long long* out) {
    long long v;
    if (o == Py_None) { *out = 0; return 0; }
    if (PyFloat_Check(o)) {
        double d = PyFloat_AsDouble(o);
        if (!(d == floor(d)) || d < (double)lo || d > (double)hi) {
            PyErr_Format(RspArgumentError, "%s: an integer in %lld..%lld", what, lo, hi);
            return -1;
        }
        *out = (long long)d;
        return 0;
    }
    v = PyLong_AsLongLong(o);
    if (v == -1 && PyErr_Occurred()) {
        if (PyErr_ExceptionMatches(PyExc_TypeError) || PyErr_ExceptionMatches(PyExc_OverflowError)) {
            PyErr_Clear();
            PyErr_Format(RspArgumentError, "%s: an integer in %lld..%lld", what, lo, hi);
        }
        return -1;
    }
    if (v < lo || v > hi) {
        PyErr_Format(RspArgumentError, "%s: %lld is outside %lld..%lld", what, v, lo, hi);
        return -1;
    }
    *out = v;
    return 0;
}

static int rsp_float(PyObject* o, const char* what, float* out) {
    double d;
    if (o == Py_None) { *out = 0.0f; return 0; }
    d = PyFloat_AsDouble(o);
    if (d == -1.0 && PyErr_Occurred()) {
        PyErr_Clear();
        PyErr_Format(RspArgumentError, "%s: a number", what);
        return -1;
    }
    *out = (float)d;
    return 0;
}

/* A str's UTF-8 as a bytes object (PyUnicode_AsUTF8 is Limited API only
 * from 3.10). New reference. */
static PyObject* rsp_utf8(PyObject* o, const char* what) {
    if (!PyUnicode_Check(o)) { PyErr_Format(PyExc_TypeError, "%s must be a str", what); return NULL; }
    return PyUnicode_AsUTF8String(o);
}

/* An enum by its name in a "name=value,..." list (the parameter table's
 * `values`), or by its number in lo..hi. */
static int rsp_enum_in(const char* values, const char* name, int* out) {
    size_t len = strlen(name);
    const char* p = values;
    while (p && *p) {
        const char* eq = strchr(p, '=');
        if (!eq) break;
        if ((size_t)(eq - p) == len && strncmp(p, name, len) == 0) { *out = atoi(eq + 1); return 1; }
        p = strchr(eq, ',');
        if (p) p++;
    }
    return 0;
}

static const char* rsp_param_values(const char* field) {
    int n, i;
    const yrsp_param* t = yrsp_params(&n);
    for (i = 0; i < n; i++) if (strcmp(t[i].name, field) == 0) return t[i].values;
    return NULL;
}

static int rsp_enum(PyObject* o, const char* values, int lo, int hi, const char* what, int* out) {
    if (PyUnicode_Check(o)) {
        PyObject* b = PyUnicode_AsUTF8String(o);
        int ok;
        if (!b) return -1;
        ok = rsp_enum_in(values, PyBytes_AsString(b), out);
        Py_DECREF(b);
        if (ok) return 0;
        PyErr_Format(RspArgumentError, "%s: not one of %s", what, values);
        return -1;
    } else {
        long long v;
        if (rsp_int(o, lo, hi, what, &v) < 0) return -1;
        *out = (int)v;
        return 0;
    }
}

static const char onset_src_values[] = "other=0,flip=1,audio=2,plan=3";

/* --- the event --------------------------------------------------------------- */

/* One event from a 16-tuple in Event's order (an Event of this or another
 * ysp package) or an object with Event's attributes (a missing one is 0).
 * t_ns wins over t when it is an int. No allocation on the tuple path; the
 * attribute path makes the references the object hands out. */
static int rsp_event_in(PyObject* o, yin_event* e) {
    PyObject* f[EV_NFIELDS];
    int owned = 0, i, rc = -1;
    long long v;
    if (PyTuple_Check(o)) {
        if (PyTuple_Size(o) != EV_NFIELDS) {
            PyErr_SetString(RspArgumentError, "an event tuple has 16 fields: t, t_ns, device, control, code, mods, kind, "
                                              "type, flags, stamp, unc, x, y, value, aux, ticks");
            return -1;
        }
        for (i = 0; i < EV_NFIELDS; i++) f[i] = PyTuple_GetItem(o, i);
    } else {
        owned = 1;
        for (i = 0; i < EV_NFIELDS; i++) {
            f[i] = PyObject_GetAttr(o, ev_attr[i]);
            if (!f[i]) {
                if (!PyErr_ExceptionMatches(PyExc_AttributeError)) { while (i-- > 0) Py_DECREF(f[i]); return -1; }
                PyErr_Clear();
                Py_INCREF(Py_None);
                f[i] = Py_None;
            }
        }
    }
    memset(e, 0, sizeof *e);
    if (f[F_TNS] != Py_None) {
        if (rsp_int(f[F_TNS], 0, INT64_MAX, "event t_ns", &v) < 0) goto done;
        e->t = (int64_t)v;
    } else if (f[F_T] != Py_None) {
        if (rsp_seconds(f[F_T], "event t", 0, &e->t) < 0) goto done;
    } else {
        PyErr_SetString(RspArgumentError, "an event needs t or t_ns");
        goto done;
    }
#define RSP_U(idx, field, hi, T)                                                \
    if (rsp_int(f[idx], 0, (hi), "event " #field, &v) < 0) goto done;          \
    e->field = (T)v;
    RSP_U(F_DEVICE, device, 0xFFFFFFFFLL, uint32_t)
    RSP_U(F_CONTROL, control, 0xFFFFFFFFLL, uint32_t)
    RSP_U(F_CODE, code, 0xFFFFFFFFLL, uint32_t)
    RSP_U(F_MODS, mods, 0xFFFF, uint16_t)
    RSP_U(F_KIND, kind, 0xFF, uint8_t)
    RSP_U(F_TYPE, type, 0xFF, uint8_t)
    RSP_U(F_FLAGS, flags, 0xFF, uint8_t)
    RSP_U(F_STAMP, stamp, 0xFF, uint8_t)
    RSP_U(F_TICKS, ticks, 0xFFFFFFFFLL, uint32_t)
#undef RSP_U
    if (f[F_UNC] != Py_None) {
        int64_t ns;
        if (rsp_seconds(f[F_UNC], "event unc", S_INF, &ns) < 0) goto done;
        /* us, rounded; YIN_UNC_MAX means that much or more */
        e->unc_us = ns >= (int64_t)YIN_UNC_MAX * 1000 ? (uint16_t)YIN_UNC_MAX : (uint16_t)((ns + 500) / 1000);
    }
    if (rsp_float(f[F_X], "event x", &e->x) < 0 || rsp_float(f[F_Y], "event y", &e->y) < 0 ||
        rsp_float(f[F_VALUE], "event value", &e->value) < 0 || rsp_float(f[F_AUX], "event aux", &e->aux) < 0)
        goto done;
    rc = 0;
done:
    if (owned) for (i = 0; i < EV_NFIELDS; i++) Py_DECREF(f[i]);
    return rc;
}

static PyObject* rsp_event_out(const yin_event* e) {
    PyObject* o = PyStructSequence_New(EventType);
    if (!o) return NULL;
    PyStructSequence_SetItem(o, F_T, rsp_s(e->t));
    PyStructSequence_SetItem(o, F_TNS, PyLong_FromLongLong(e->t));
    PyStructSequence_SetItem(o, F_DEVICE, PyLong_FromUnsignedLong(e->device));
    PyStructSequence_SetItem(o, F_CONTROL, PyLong_FromUnsignedLong(e->control));
    PyStructSequence_SetItem(o, F_CODE, PyLong_FromUnsignedLong(e->code));
    PyStructSequence_SetItem(o, F_MODS, PyLong_FromLong(e->mods));
    PyStructSequence_SetItem(o, F_KIND, PyLong_FromLong(e->kind));
    PyStructSequence_SetItem(o, F_TYPE, PyLong_FromLong(e->type));
    PyStructSequence_SetItem(o, F_FLAGS, PyLong_FromLong(e->flags));
    PyStructSequence_SetItem(o, F_STAMP, PyLong_FromLong(e->stamp));
    PyStructSequence_SetItem(o, F_UNC, PyFloat_FromDouble((double)e->unc_us / 1e6));
    PyStructSequence_SetItem(o, F_X, PyFloat_FromDouble(e->x));
    PyStructSequence_SetItem(o, F_Y, PyFloat_FromDouble(e->y));
    PyStructSequence_SetItem(o, F_VALUE, PyFloat_FromDouble(e->value));
    PyStructSequence_SetItem(o, F_AUX, PyFloat_FromDouble(e->aux));
    PyStructSequence_SetItem(o, F_TICKS, PyLong_FromUnsignedLong(e->ticks));
    if (PyErr_Occurred()) { Py_DECREF(o); return NULL; }
    return o;
}

/* A key: a name (CHOICES) or a scancode. */
static int rsp_key(PyObject* o, uint32_t* sc) {
    if (PyUnicode_Check(o)) {
        PyObject* b = PyUnicode_AsUTF8String(o);
        int k;
        if (!b) return -1;
        k = yrsp_scancode(PyBytes_AsString(b));
        if (k < 0) PyErr_Format(RspArgumentError, "'%s' is not a key name (see ysp/response.h, CHOICES)", PyBytes_AsString(b));
        Py_DECREF(b);
        if (k < 0) return -1;
        *sc = (uint32_t)k;
        return 0;
    } else {
        long long v;
        if (rsp_int(o, 0, 0xFFFFFFFFLL, "key", &v) < 0) return -1;
        *sc = (uint32_t)v;
        return 0;
    }
}

static PyObject* mod_event(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "t", "kind", "type", "control", "device", "code", "mods", "flags", "stamp", "unc", "x", "y",
                          "value", "aux", "ticks", "t_ns", NULL };
    PyObject *t = Py_None, *kind = NULL, *control = NULL, *t_ns = Py_None, *value = Py_None;
    long long device = 0, code = 0, ticks = 0;
    int type = YIN_PRESS, mods = 0, flags = 0, stamp = 0, k = 0;
    double unc = 0.0, x = 0.0, y = 0.0, aux = 0.0;
    yin_event e;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OOiOLLiiidddOdLO", kw, &t, &kind, &type, &control, &device, &code,
                                     &mods, &flags, &stamp, &unc, &x, &y, &value, &aux, &ticks, &t_ns))
        return NULL;
    memset(&e, 0, sizeof e);
    if (t_ns != Py_None) {
        long long v;
        if (rsp_int(t_ns, 0, INT64_MAX, "t_ns", &v) < 0) return NULL;
        e.t = (int64_t)v;
    } else if (t != Py_None) {
        if (rsp_seconds(t, "t", 0, &e.t) < 0) return NULL;
    } else {
        PyErr_SetString(RspArgumentError, "event() needs t or t_ns");
        return NULL;
    }
    if (kind && rsp_enum(kind, rsp_param_values("choices.kind"), 0, 15, "kind", &k) < 0) return NULL;
    e.kind = (uint8_t)k;
    if (control && control != Py_None) {
        if (PyUnicode_Check(control) && e.kind != YIN_KIND_KEYBOARD) {
            PyErr_SetString(RspArgumentError, "a key name as control needs kind keyboard");
            return NULL;
        }
        if (rsp_key(control, &e.control) < 0) return NULL;
    }
    if (type < 0 || type > 255 || mods < 0 || mods > 65535 || flags < 0 || flags > 255 || stamp < 0 || stamp > 255 ||
        device < 0 || device > 0xFFFFFFFFLL || code < 0 || code > 0xFFFFFFFFLL || ticks < 0 || ticks > 0xFFFFFFFFLL) {
        PyErr_SetString(RspArgumentError, "event(): a field is outside its C type");
        return NULL;
    }
    e.type = (uint8_t)type;
    e.device = (uint32_t)device;
    e.code = (uint32_t)code;
    e.mods = (uint16_t)mods;
    e.flags = (uint8_t)flags;
    e.stamp = (uint8_t)stamp;
    e.ticks = (uint32_t)ticks;
    if (unc != unc || unc < 0) { PyErr_SetString(RspArgumentError, "unc: seconds, >= 0 (not NaN)"); return NULL; }
    e.unc_us = unc >= YIN_UNC_MAX / 1e6 ? (uint16_t)YIN_UNC_MAX : (uint16_t)llround(unc * 1e6);
    e.x = (float)x;
    e.y = (float)y;
    e.aux = (float)aux;
    if (value != Py_None) {
        if (rsp_float(value, "value", &e.value) < 0) return NULL;
    } else {
        e.value = e.type == YIN_PRESS ? 1.0f : 0.0f;   /* ysp/input.h: 1 for a press, 0 for a release */
    }
    return rsp_event_out(&e);
}

/* --- result and entries ------------------------------------------------------ */

static PyObject* rsp_opt_s(int64_t ns, int none) {
    if (none) Py_RETURN_NONE;
    return rsp_s(ns);
}
static PyObject* rsp_opt_ns(int64_t ns, int none) {
    if (none) Py_RETURN_NONE;
    return PyLong_FromLongLong(ns);
}

enum { R_RT, R_RT_NS, R_DUR, R_DUR_NS, R_RESPONSE, R_CHOICE, R_ENTRY, R_TRESP, R_TRESP_NS, R_KIND, R_DEVICE, R_CONTROL,
       R_CODE, R_STIER, R_SPARTIAL, R_SLO, R_SHI, R_TONSET, R_TONSET_NS, R_OSRC, R_OTIER, R_ORES, R_OUNC, R_OFRAME,
       R_PLAN, R_FLAGS, R_NRESP, R_NEARLY, R_NANT, R_NLATE, R_NAFTER, R_NNOT, R_NHELD, R_NENT, R_NLOST, R_NTRACE,
       R_NSENT, R_NFIELDS };

static PyObject* rsp_result_out(const yrsp_result* r, PyObject* names) {
    PyObject* o = PyStructSequence_New(ResultType);
    int resp = (r->flags & YRSP_R_RESPONDED) != 0, on = r->t_onset != 0;
    PyObject* name;
    if (!o) return NULL;
    PyStructSequence_SetItem(o, R_RT, rsp_opt_s(r->rt_ns, r->rt_ns == YRSP_NO_TIME));
    PyStructSequence_SetItem(o, R_RT_NS, rsp_opt_ns(r->rt_ns, r->rt_ns == YRSP_NO_TIME));
    PyStructSequence_SetItem(o, R_DUR, rsp_opt_s(r->rt_key_duration_ns, r->rt_key_duration_ns == YRSP_NO_TIME));
    PyStructSequence_SetItem(o, R_DUR_NS, rsp_opt_ns(r->rt_key_duration_ns, r->rt_key_duration_ns == YRSP_NO_TIME));
    /* the choice's name as the str the caller gave, not a new one per trial */
    if (r->response >= 0 && names && r->response < PyTuple_Size(names)) name = PyTuple_GetItem(names, r->response);
    else name = Py_None;
    Py_INCREF(name);
    PyStructSequence_SetItem(o, R_RESPONSE, name);
    if (r->response >= 0) PyStructSequence_SetItem(o, R_CHOICE, PyLong_FromLong(r->response));
    else { Py_INCREF(Py_None); PyStructSequence_SetItem(o, R_CHOICE, Py_None); }
    if (r->entry >= 0) PyStructSequence_SetItem(o, R_ENTRY, PyLong_FromLong(r->entry));
    else { Py_INCREF(Py_None); PyStructSequence_SetItem(o, R_ENTRY, Py_None); }
    PyStructSequence_SetItem(o, R_TRESP, rsp_opt_s(r->t_response, !resp));
    PyStructSequence_SetItem(o, R_TRESP_NS, rsp_opt_ns(r->t_response, !resp));
    PyStructSequence_SetItem(o, R_KIND, PyLong_FromLong(r->kind));
    PyStructSequence_SetItem(o, R_DEVICE, PyLong_FromUnsignedLong(r->device));
    PyStructSequence_SetItem(o, R_CONTROL, PyLong_FromUnsignedLong(r->control));
    PyStructSequence_SetItem(o, R_CODE, PyLong_FromUnsignedLong(r->code));
    PyStructSequence_SetItem(o, R_STIER, PyLong_FromLong(r->stamp_tier));
    PyStructSequence_SetItem(o, R_SPARTIAL, PyBool_FromLong(r->stamp_partial));
    PyStructSequence_SetItem(o, R_SLO, rsp_s(r->stamp_lo_ns));
    PyStructSequence_SetItem(o, R_SHI, rsp_s(r->stamp_hi_ns));
    PyStructSequence_SetItem(o, R_TONSET, rsp_opt_s(r->t_onset, !on));
    PyStructSequence_SetItem(o, R_TONSET_NS, rsp_opt_ns(r->t_onset, !on));
    PyStructSequence_SetItem(o, R_OSRC, PyLong_FromLong(r->onset_src));
    PyStructSequence_SetItem(o, R_OTIER, PyLong_FromLong(r->onset_tier));
    PyStructSequence_SetItem(o, R_ORES, rsp_s(r->onset_residual));
    PyStructSequence_SetItem(o, R_OUNC, rsp_s(r->onset_uncertainty));
    if (r->onset_frame >= 0) PyStructSequence_SetItem(o, R_OFRAME, PyLong_FromLongLong(r->onset_frame));
    else { Py_INCREF(Py_None); PyStructSequence_SetItem(o, R_OFRAME, Py_None); }
    PyStructSequence_SetItem(o, R_PLAN, rsp_opt_s(r->plan_residual, r->plan_residual == YRSP_NO_TIME));
    PyStructSequence_SetItem(o, R_FLAGS, PyLong_FromUnsignedLong(r->flags));
    PyStructSequence_SetItem(o, R_NRESP, PyLong_FromLong(r->n_responses));
    PyStructSequence_SetItem(o, R_NEARLY, PyLong_FromLong(r->n_early));
    PyStructSequence_SetItem(o, R_NANT, PyLong_FromLong(r->n_anticipations));
    PyStructSequence_SetItem(o, R_NLATE, PyLong_FromLong(r->n_late));
    PyStructSequence_SetItem(o, R_NAFTER, PyLong_FromLong(r->n_after_end));
    PyStructSequence_SetItem(o, R_NNOT, PyLong_FromLong(r->n_not_choice));
    PyStructSequence_SetItem(o, R_NHELD, PyLong_FromLong(r->n_held));
    PyStructSequence_SetItem(o, R_NENT, PyLong_FromLong(r->n_entries));
    PyStructSequence_SetItem(o, R_NLOST, PyLong_FromLong(r->n_lost));
    PyStructSequence_SetItem(o, R_NTRACE, PyLong_FromLong(r->n_trace));
    PyStructSequence_SetItem(o, R_NSENT, PyLong_FromLong(r->n_sent));
    if (PyErr_Occurred()) { Py_DECREF(o); return NULL; }
    return o;
}

enum { N_T, N_TNS, N_TREL, N_TREL_NS, N_KIND, N_DEVICE, N_CONTROL, N_CODE, N_CHOICE, N_FLAGS, N_STAMP, N_MODS,
       N_VALUE, N_ARRIVAL, N_NFIELDS };

static PyObject* rsp_entry_out(const yrsp_entry* e) {
    PyObject* o = PyStructSequence_New(EntryType);
    int rel = (e->flags & YRSP_E_RELEASED) != 0;
    if (!o) return NULL;
    PyStructSequence_SetItem(o, N_T, rsp_s(e->t));
    PyStructSequence_SetItem(o, N_TNS, PyLong_FromLongLong(e->t));
    PyStructSequence_SetItem(o, N_TREL, rsp_opt_s(e->t_release, !rel));
    PyStructSequence_SetItem(o, N_TREL_NS, rsp_opt_ns(e->t_release, !rel));
    PyStructSequence_SetItem(o, N_KIND, PyLong_FromLong(e->kind));
    PyStructSequence_SetItem(o, N_DEVICE, PyLong_FromUnsignedLong(e->device));
    PyStructSequence_SetItem(o, N_CONTROL, PyLong_FromUnsignedLong(e->control));
    PyStructSequence_SetItem(o, N_CODE, PyLong_FromUnsignedLong(e->code));
    if (e->choice >= 0) PyStructSequence_SetItem(o, N_CHOICE, PyLong_FromLong(e->choice));
    else { Py_INCREF(Py_None); PyStructSequence_SetItem(o, N_CHOICE, Py_None); }
    PyStructSequence_SetItem(o, N_FLAGS, PyLong_FromLong(e->flags));
    PyStructSequence_SetItem(o, N_STAMP, PyLong_FromLong(e->stamp));
    PyStructSequence_SetItem(o, N_MODS, PyLong_FromLong(e->mods));
    PyStructSequence_SetItem(o, N_VALUE, PyFloat_FromDouble(e->value));
    PyStructSequence_SetItem(o, N_ARRIVAL, PyLong_FromLong(e->arrival_class));
    if (PyErr_Occurred()) { Py_DECREF(o); return NULL; }
    return o;
}

/* --- Collector -------------------------------------------------------------- */

typedef struct {
    PyObject_HEAD
    yrsp_collector c;
    int opened;
    PyObject* keep;     /* list: the bytes the copied choices and sources point into */
    PyObject* names;    /* tuple: each choice's name as a str, or None */
    yin_event* trace;   /* desc.trace, PyMem, trace_cap events */
    int trace_cap;
    yrsp_result last;   /* the last finish() */
    int have_last;
} CollObject;

static void coll_release(CollObject* s) {
    if (s->opened) yrsp_close(&s->c);
    s->opened = 0;
    Py_CLEAR(s->keep);
    Py_CLEAR(s->names);
    if (s->trace) PyMem_Free(s->trace);
    s->trace = NULL;
    s->trace_cap = 0;
    s->have_last = 0;
}

static void Coll_dealloc(PyObject* self) {
    PyTypeObject* tp = Py_TYPE(self);
    freefunc f = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    coll_release((CollObject*)self);
    f(self);
    Py_DECREF(tp);
}

/* A const char* that lives as long as the collector: the bytes go into keep. */
static const char* coll_keep_str(CollObject* s, PyObject* str, const char* what) {
    PyObject* b = rsp_utf8(str, what);
    if (!b) return NULL;
    if (PyList_Append(s->keep, b) < 0) { Py_DECREF(b); return NULL; }
    Py_DECREF(b);
    return PyBytes_AsString(b);
}

/* A dict's value by key, borrowed; NULL when absent (no error). */
static PyObject* rsp_get(PyObject* d, const char* k) { return PyDict_GetItemString(d, k); }

static int coll_choice(CollObject* s, PyObject* o, int i, yrsp_choice* h) {
    static const char* const allowed[] = { "key", "name", "kind", "match", "cross", "device", "control", "code",
                                           "level", "hysteresis", NULL };
    PyObject *v, *key, *val;
    Py_ssize_t pos = 0;
    long long n;
    int e;
    memset(h, 0, sizeof *h);
    if (PyUnicode_Check(o)) {
        if (!(h->key = coll_keep_str(s, o, "choice"))) return -1;
        return 0;
    }
    if (!PyDict_Check(o)) {
        PyErr_Format(PyExc_TypeError, "choice %d: a key name (str) or a dict of choice fields", i);
        return -1;
    }
    /* a typo ("controll") would leave a default in force without a word */
    while (PyDict_Next(o, &pos, &key, &val)) {
        PyObject* b = PyUnicode_Check(key) ? PyUnicode_AsUTF8String(key) : NULL;
        int k, ok = 0;
        if (b) for (k = 0; allowed[k]; k++) if (strcmp(PyBytes_AsString(b), allowed[k]) == 0) { ok = 1; break; }
        if (!ok) {
            PyErr_Format(RspArgumentError, "choice %d: unknown field %R (key, name, kind, match, cross, device, "
                                           "control, code, level, hysteresis)", i, key);
            Py_XDECREF(b);
            return -1;
        }
        Py_DECREF(b);
    }
    if ((v = rsp_get(o, "key")) && v != Py_None && !(h->key = coll_keep_str(s, v, "choice key"))) return -1;
    if ((v = rsp_get(o, "name")) && v != Py_None && !(h->name = coll_keep_str(s, v, "choice name"))) return -1;
    if ((v = rsp_get(o, "kind"))) { if (rsp_enum(v, rsp_param_values("choices.kind"), 0, 15, "choice kind", &e) < 0) return -1; h->kind = (uint8_t)e; }
    if ((v = rsp_get(o, "match"))) { if (rsp_enum(v, rsp_param_values("choices.match"), 0, 255, "choice match", &e) < 0) return -1; h->match = (uint8_t)e; }
    if ((v = rsp_get(o, "cross"))) { if (rsp_enum(v, rsp_param_values("choices.cross"), 0, 255, "choice cross", &e) < 0) return -1; h->cross = (uint8_t)e; }
    if ((v = rsp_get(o, "device"))) { if (rsp_int(v, 0, 0xFFFFFFFFLL, "choice device", &n) < 0) return -1; h->device = (uint32_t)n; }
    if ((v = rsp_get(o, "control"))) {
        if (PyUnicode_Check(v)) { if (rsp_key(v, &h->control) < 0) return -1; }
        else { if (rsp_int(v, 0, 0xFFFFFFFFLL, "choice control", &n) < 0) return -1; h->control = (uint32_t)n; }
    }
    if ((v = rsp_get(o, "code"))) { if (rsp_int(v, 0, 0xFFFFFFFFLL, "choice code", &n) < 0) return -1; h->code = (uint32_t)n; }
    if ((v = rsp_get(o, "level")) && rsp_float(v, "choice level", &h->level) < 0) return -1;
    if ((v = rsp_get(o, "hysteresis")) && rsp_float(v, "choice hysteresis", &h->hysteresis) < 0) return -1;
    return 0;
}

static int coll_source(CollObject* s, PyObject* o, int i, yin_source* src) {
    static const char* const allowed[] = { "kind", "device", "tier", "partial", "lo", "hi", "note", NULL };
    PyObject *v, *key, *val;
    Py_ssize_t pos = 0;
    long long n;
    int e;
    memset(src, 0, sizeof *src);
    if (!PyDict_Check(o)) { PyErr_Format(PyExc_TypeError, "source %d: a dict (kind, device, tier, partial, lo, hi, note)", i); return -1; }
    while (PyDict_Next(o, &pos, &key, &val)) {
        PyObject* b = PyUnicode_Check(key) ? PyUnicode_AsUTF8String(key) : NULL;
        int k, ok = 0;
        if (b) for (k = 0; allowed[k]; k++) if (strcmp(PyBytes_AsString(b), allowed[k]) == 0) { ok = 1; break; }
        Py_XDECREF(b);
        if (!ok) { PyErr_Format(RspArgumentError, "source %d: unknown field %R", i, key); return -1; }
    }
    if ((v = rsp_get(o, "kind"))) { if (rsp_enum(v, rsp_param_values("choices.kind"), 0, 15, "source kind", &e) < 0) return -1; src->kind = (uint8_t)e; }
    if ((v = rsp_get(o, "device"))) { if (rsp_int(v, 0, 0xFFFFFFFFLL, "source device", &n) < 0) return -1; src->device = (uint32_t)n; }
    if ((v = rsp_get(o, "tier"))) { if (rsp_int(v, 0, 255, "source tier", &n) < 0) return -1; src->tier = (uint8_t)n; }
    if ((v = rsp_get(o, "partial"))) { int t = PyObject_IsTrue(v); if (t < 0) return -1; src->partial = (uint8_t)t; }
    if ((v = rsp_get(o, "lo")) && rsp_seconds(v, "source lo", S_NEG, &src->lo_ns) < 0) return -1;
    if ((v = rsp_get(o, "hi")) && rsp_seconds(v, "source hi", S_NEG, &src->hi_ns) < 0) return -1;
    if ((v = rsp_get(o, "note")) && v != Py_None && !(src->note = coll_keep_str(s, v, "source note"))) return -1;
    return 0;
}

/* kinds: an int mask, "all", or an iterable of kinds (names or numbers). */
static int coll_kinds(PyObject* o, const char* what, uint16_t* out) {
    long long n;
    if (PyUnicode_Check(o) && PyUnicode_CompareWithASCIIString(o, "all") == 0) { *out = YIN_KINDS_ALL; return 0; }
    if (PyLong_Check(o)) {
        if (rsp_int(o, 0, 65535, what, &n) < 0) return -1;
        *out = (uint16_t)n;
        return 0;
    } else {
        PyObject* it = PyObject_GetIter(o);
        PyObject* x;
        unsigned mask = 0;
        if (!it) { PyErr_Clear(); PyErr_Format(PyExc_TypeError, "%s: a bit mask, \"all\", or a list of kinds", what); return -1; }
        while ((x = PyIter_Next(it))) {
            int k;
            int rc = rsp_enum(x, rsp_param_values("choices.kind"), 0, 15, what, &k);
            Py_DECREF(x);
            if (rc < 0) { Py_DECREF(it); return -1; }
            mask |= 1u << k;
        }
        Py_DECREF(it);
        if (PyErr_Occurred()) return -1;
        *out = (uint16_t)mask;
        return 0;
    }
}

/* A duration of the desc: None, 0 and math.inf are "none" where allowed. */
static int coll_dur(PyObject* o, const char* what, int inf_none, int64_t* out) {
    int64_t ns;
    if (!o || o == Py_None) { *out = 0; return 0; }
    if (rsp_seconds(o, what, inf_none ? S_INF : 0, &ns) < 0) return -1;
    *out = ns == INT64_MAX ? 0 : ns;
    return 0;
}

static int Coll_init(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "choices", "mode", "kinds", "min_rt", "duration", "persist", "max_responses", "allow_held_key",
                          "wait_for_key_release", "max_hold", "sources", "trace_cap", "trace_kinds", NULL };
    CollObject* s = (CollObject*)self;
    PyObject *choices = Py_None, *mode = Py_None, *kinds = Py_None, *min_rt = Py_None, *duration = Py_None;
    PyObject *max_hold = Py_None, *sources = Py_None, *trace_kinds = Py_None, *seq = NULL;
    int persist = 0, max_responses = 0, held = 0, wait = 0, trace_cap = 0, i, n = 0, e;
    yrsp_choice ch[YRSP_MAX_CHOICES];
    yin_source src[YRSP_MAX_SOURCES];
    yrsp_desc d;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O$OOOOpippOOiO", kw, &choices, &mode, &kinds, &min_rt, &duration,
                                     &persist, &max_responses, &held, &wait, &max_hold, &sources, &trace_cap,
                                     &trace_kinds))
        return -1;
    coll_release(s);
    memset(&d, 0, sizeof d);
    if (!(s->keep = PyList_New(0))) return -1;
    if (choices != Py_None) {
        if (PyUnicode_Check(choices)) { PyErr_SetString(PyExc_TypeError, "choices: a list of key names or dicts, not one str"); return -1; }
        if (!(seq = PySequence_List(choices))) return -1;
        n = (int)PyList_Size(seq);
        if (n > YRSP_MAX_CHOICES) {
            Py_DECREF(seq);
            PyErr_Format(RspArgumentError, "choices: at most %d", YRSP_MAX_CHOICES);
            return -1;
        }
        for (i = 0; i < n; i++)
            if (coll_choice(s, PyList_GetItem(seq, i), i, &ch[i]) < 0) { Py_DECREF(seq); return -1; }
        Py_DECREF(seq);
        d.choices = ch;
        d.n_choices = n;
    }
    if (mode != Py_None) {
        if (rsp_enum(mode, rsp_param_values("mode"), 0, 2, "mode", &e) < 0) return -1;
        d.mode = e;
    }
    if (kinds != Py_None && coll_kinds(kinds, "kinds", &d.kinds) < 0) return -1;
    if (trace_kinds != Py_None && coll_kinds(trace_kinds, "trace_kinds", &d.trace_kinds) < 0) return -1;
    if (coll_dur(min_rt, "min_rt", 0, &d.min_rt_ns) < 0 || coll_dur(duration, "duration", 1, &d.duration_ns) < 0 ||
        coll_dur(max_hold, "max_hold", 0, &d.max_hold_ns) < 0)
        return -1;
    d.persist = persist != 0;
    d.max_responses = max_responses;
    d.allow_held_key = held != 0;
    d.wait_for_key_release = wait != 0;
    if (sources != Py_None) {
        int m;
        if (!(seq = PySequence_List(sources))) return -1;
        m = (int)PyList_Size(seq);
        if (m > YRSP_MAX_SOURCES) { Py_DECREF(seq); PyErr_Format(RspArgumentError, "sources: at most %d", YRSP_MAX_SOURCES); return -1; }
        for (i = 0; i < m; i++)
            if (coll_source(s, PyList_GetItem(seq, i), i, &src[i]) < 0) { Py_DECREF(seq); return -1; }
        Py_DECREF(seq);
        d.sources = src;
        d.n_sources = m;
    }
    if (trace_cap < 0 || trace_cap > 10000000) { PyErr_SetString(RspArgumentError, "trace_cap: 0..10000000 events"); return -1; }
    if (trace_cap > 0) {
        s->trace = (yin_event*)PyMem_Malloc((size_t)trace_cap * sizeof(yin_event));
        if (!s->trace) { PyErr_NoMemory(); return -1; }
        s->trace_cap = trace_cap;
        d.trace = s->trace;
        d.trace_cap = trace_cap;
    }
    if (!yrsp_open(&s->c, &d)) {
        rsp_fail_code(YRSP_ERR_ARG, yrsp_error(&s->c));
        return -1;
    }
    s->opened = 1;
    /* each choice's name as the str the result returns: open() resolved
     * name = key where the caller gave no name */
    if (!(s->names = PyTuple_New(n))) return -1;
    for (i = 0; i < n; i++) {
        const char* nm = s->c.ch[i].name;
        PyObject* u;
        if (nm) u = PyUnicode_FromString(nm);
        else { Py_INCREF(Py_None); u = Py_None; }
        if (!u) return -1;
        PyTuple_SetItem(s->names, i, u);
    }
    return 0;
}

static PyObject* coll_status(CollObject* s, int rc) {
    if (rc < 0) return rsp_fail_code(rc, yrsp_error(&s->c));
    return PyLong_FromLong(rc);
}

static int coll_open(CollObject* s) {
    if (!s->opened) { PyErr_SetString(RspStateError, "ysp_response: the collector is closed"); return -1; }
    return 0;
}

static PyObject* Coll_arm(PyObject* self, PyObject* arg) {
    CollObject* s = (CollObject*)self;
    int64_t t;
    if (coll_open(s) < 0 || rsp_seconds(arg, "arm t", 0, &t) < 0) return NULL;
    yrsp_arm(&s->c, t);
    Py_RETURN_NONE;
}

static PyObject* Coll_set_onset(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "t", "final", "src", "tier", "residual", "uncertainty", "frame", "t_ns", NULL };
    CollObject* s = (CollObject*)self;
    PyObject *t = Py_None, *src = NULL, *residual = NULL, *unc = NULL, *frame = Py_None, *t_ns = Py_None;
    int final = 1, tier = 0, e = YRSP_ONSET_OTHER;
    yrsp_onset o;
    long long v;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O$pOiOOOO", kw, &t, &final, &src, &tier, &residual, &unc, &frame, &t_ns))
        return NULL;
    if (coll_open(s) < 0) return NULL;
    memset(&o, 0, sizeof o);
    if (t_ns != Py_None) {
        if (rsp_int(t_ns, 0, INT64_MAX, "t_ns", &v) < 0) return NULL;
        o.t_ns = (int64_t)v;
    } else if (t != Py_None) {
        if (rsp_seconds(t, "onset t", 0, &o.t_ns) < 0) return NULL;
    } else {
        PyErr_SetString(RspArgumentError, "set_onset() needs t or t_ns");
        return NULL;
    }
    if (src && rsp_enum(src, onset_src_values, 0, 255, "src", &e) < 0) return NULL;
    if (tier < 0 || tier > 255) { PyErr_SetString(RspArgumentError, "tier: 0..255"); return NULL; }
    if (residual && rsp_seconds(residual, "residual", S_NEG, &o.residual_ns) < 0) return NULL;
    if (unc && rsp_seconds(unc, "uncertainty", 0, &o.uncertainty_ns) < 0) return NULL;
    o.frame = -1;
    if (frame != Py_None) {
        if (rsp_int(frame, -1, INT64_MAX, "frame", &v) < 0) return NULL;
        o.frame = (int64_t)v;
    }
    o.tier = (uint8_t)tier;
    o.src = (uint8_t)e;
    o.final = (uint8_t)(final != 0);
    return coll_status(s, yrsp_set_onset(&s->c, &o));
}

static PyObject* Coll_feed(PyObject* self, PyObject* arg) {
    CollObject* s = (CollObject*)self;
    yin_event e;
    if (coll_open(s) < 0 || rsp_event_in(arg, &e) < 0) return NULL;
    return coll_status(s, yrsp_feed(&s->c, &e));
}

static PyObject* Coll_feed_many(PyObject* self, PyObject* arg) {
    CollObject* s = (CollObject*)self;
    PyObject *it, *x;
    int rc = 0;
    yin_event e;
    if (coll_open(s) < 0) return NULL;
    if (!(it = PyObject_GetIter(arg))) return NULL;
    while ((x = PyIter_Next(it))) {
        int bad = rsp_event_in(x, &e) < 0;
        Py_DECREF(x);
        if (bad) { Py_DECREF(it); return NULL; }
        rc = yrsp_feed(&s->c, &e);
        if (rc < 0) { Py_DECREF(it); return coll_status(s, rc); }
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) return NULL;
    /* An empty iterable fed nothing, so ask for the status: update() at
     * INT64_MIN reaches no deadline and changes nothing. */
    return coll_status(s, yrsp_update(&s->c, INT64_MIN));
}

static PyObject* Coll_key(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "t", "key", "down", "device", "repeat", "stamp", "code", "t_ns", NULL };
    CollObject* s = (CollObject*)self;
    PyObject *t = Py_None, *key, *t_ns = Py_None;
    int down = 1, repeat = 0, stamp = 0;
    long long device = 0, code = 0;
    yin_event e;
    long long v;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO|p$LpiLO", kw, &t, &key, &down, &device, &repeat, &stamp, &code, &t_ns))
        return NULL;
    if (coll_open(s) < 0) return NULL;
    memset(&e, 0, sizeof e);
    if (t_ns != Py_None) {
        if (rsp_int(t_ns, 0, INT64_MAX, "t_ns", &v) < 0) return NULL;
        e.t = (int64_t)v;
    } else if (rsp_seconds(t, "key t", 0, &e.t) < 0) {
        return NULL;
    }
    if (rsp_key(key, &e.control) < 0) return NULL;
    if (stamp < 0 || stamp > 255 || device < 0 || device > 0xFFFFFFFFLL || code < 0 || code > 0xFFFFFFFFLL) {
        PyErr_SetString(RspArgumentError, "key(): stamp 0..255, device and code 32 bits");
        return NULL;
    }
    e.kind = (uint8_t)YIN_KIND_KEYBOARD;
    e.type = (uint8_t)(down ? YIN_PRESS : YIN_RELEASE);
    e.flags = (uint8_t)(repeat ? YIN_REPEAT : 0u);
    e.device = (uint32_t)device;
    e.code = (uint32_t)code;
    e.stamp = (uint8_t)stamp;
    e.value = down ? 1.0f : 0.0f;
    return coll_status(s, yrsp_feed(&s->c, &e));
}

static PyObject* Coll_update(PyObject* self, PyObject* arg) {
    CollObject* s = (CollObject*)self;
    int64_t t;
    if (coll_open(s) < 0 || rsp_seconds(arg, "update now", 0, &t) < 0) return NULL;
    return coll_status(s, yrsp_update(&s->c, t));
}

static PyObject* Coll_finish(PyObject* self, PyObject* Py_UNUSED(a)) {
    CollObject* s = (CollObject*)self;
    int rc;
    if (coll_open(s) < 0) return NULL;
    rc = yrsp_finish(&s->c, &s->last);
    if (rc < 0) return rsp_fail_code(rc, yrsp_error(&s->c));
    s->have_last = 1;
    return rsp_result_out(&s->last, s->names);
}

static PyObject* Coll_entries(PyObject* self, PyObject* Py_UNUSED(a)) {
    CollObject* s = (CollObject*)self;
    int n, i;
    const yrsp_entry* e;
    PyObject* t;
    if (coll_open(s) < 0) return NULL;
    e = yrsp_entries(&s->c, &n);
    if (!(t = PyTuple_New(n))) return NULL;
    for (i = 0; i < n; i++) {
        PyObject* x = rsp_entry_out(&e[i]);
        if (!x) { Py_DECREF(t); return NULL; }
        PyTuple_SetItem(t, i, x);
    }
    return t;
}

static PyObject* Coll_trace(PyObject* self, PyObject* Py_UNUSED(a)) {
    CollObject* s = (CollObject*)self;
    int n = s->have_last && s->trace ? s->last.n_trace : 0, i;
    PyObject* t;
    if (n > s->trace_cap) n = s->trace_cap;
    if (!(t = PyTuple_New(n))) return NULL;
    for (i = 0; i < n; i++) {
        PyObject* x = rsp_event_out(&s->trace[i]);
        if (!x) { Py_DECREF(t); return NULL; }
        PyTuple_SetItem(t, i, x);
    }
    return t;
}

/* sent: an iterable of (t, kind, device, control, choice, written) tuples or
 * objects with those attributes (t_ns beside t where exact). */
static PyObject* Coll_set_sent(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "sent", "total", NULL };
    static const char* const f[] = { "t", "kind", "device", "control", "choice", "written" };
    CollObject* s = (CollObject*)self;
    PyObject *sent, *total_o = Py_None, *seq;
    yrsp_sent buf[YRSP_MAX_SENT];
    int n, k, m, total;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O", kw, &sent, &total_o)) return NULL;
    if (coll_open(s) < 0) return NULL;
    if (!(seq = PySequence_List(sent))) return NULL;
    n = (int)PyList_Size(seq);
    m = n < YRSP_MAX_SENT ? n : YRSP_MAX_SENT;
    for (k = 0; k < m; k++) {
        PyObject* o = PyList_GetItem(seq, k);
        PyObject* v[7] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL };   /* f[], then t_ns */
        long long x = 0;
        int j, bad = 0, w;
        if (PyTuple_Check(o)) {
            if (PyTuple_Size(o) != 6) {
                PyErr_SetString(RspArgumentError, "a sent tuple: (t, kind, device, control, choice, written)");
                bad = 1;
            } else {
                for (j = 0; j < 6; j++) { v[j] = PyTuple_GetItem(o, j); Py_INCREF(v[j]); }
            }
        } else {
            for (j = 0; j < 6 && !bad; j++) bad = !(v[j] = PyObject_GetAttrString(o, f[j]));
            if (!bad && !(v[6] = PyObject_GetAttrString(o, "t_ns"))) PyErr_Clear();
        }
        memset(&buf[k], 0, sizeof buf[k]);
        if (!bad) {
            if (v[6] && v[6] != Py_None) { bad = rsp_int(v[6], 0, INT64_MAX, "sent t_ns", &x) < 0; buf[k].t = (int64_t)x; }
            else bad = rsp_seconds(v[0], "sent t", 0, &buf[k].t) < 0;
        }
        if (!bad) { bad = rsp_int(v[1], 0, 15, "sent kind", &x) < 0; buf[k].kind = (uint8_t)x; }
        if (!bad) { bad = rsp_int(v[2], 0, 0xFFFFFFFFLL, "sent device", &x) < 0; buf[k].device = (uint32_t)x; }
        if (!bad) { bad = rsp_int(v[3], 0, 0xFFFFFFFFLL, "sent control", &x) < 0; buf[k].control = (uint32_t)x; }
        if (!bad) {
            if (v[4] == Py_None) buf[k].choice = -1;
            else { bad = rsp_int(v[4], -1, 32767, "sent choice", &x) < 0; buf[k].choice = (int16_t)x; }
        }
        if (!bad) { w = PyObject_IsTrue(v[5]); bad = w < 0; buf[k].written = (uint8_t)(w > 0); }
        for (j = 0; j < 7; j++) Py_XDECREF(v[j]);
        if (bad) { Py_DECREF(seq); return NULL; }
    }
    Py_DECREF(seq);
    total = n;
    if (total_o != Py_None) {
        long long x;
        if (rsp_int(total_o, n, INT32_MAX, "total", &x) < 0) return NULL;
        total = (int)x;
    }
    k = yrsp_set_sent(&s->c, buf, m, total);
    if (k < 0) return rsp_fail_code(k, yrsp_error(&s->c));
    Py_RETURN_NONE;
}

/* Offline: one trial from a logged stream (THE WINDOW AND RT). The events
 * stamped before `arm` that come first in the list are fed before
 * yrsp_arm(), as a live loop would have fed them, so keys held from before
 * the window stay held; the rest after it. Then the onset, update(end)
 * when given, and finish(). */
static PyObject* Coll_score(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "events", "onset", "arm", "end", "final", "src", "tier", "residual", "uncertainty", "frame", NULL };
    CollObject* s = (CollObject*)self;
    PyObject *events, *onset_o, *arm_o = Py_None, *end_o = Py_None, *src = NULL, *residual = NULL, *unc = NULL, *frame = Py_None;
    PyObject *it, *x;
    int final = 1, tier = 0, e = YRSP_ONSET_OTHER, armed = 0, rc;
    int64_t t_arm, t_end = 0;
    yrsp_onset o;
    yin_event ev;
    long long v;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO|$OOpOiOOO", kw, &events, &onset_o, &arm_o, &end_o, &final, &src,
                                     &tier, &residual, &unc, &frame))
        return NULL;
    if (coll_open(s) < 0) return NULL;
    memset(&o, 0, sizeof o);
    if (rsp_seconds(onset_o, "onset", 0, &o.t_ns) < 0) return NULL;
    if (src && rsp_enum(src, onset_src_values, 0, 255, "src", &e) < 0) return NULL;
    if (tier < 0 || tier > 255) { PyErr_SetString(RspArgumentError, "tier: 0..255"); return NULL; }
    if (residual && rsp_seconds(residual, "residual", S_NEG, &o.residual_ns) < 0) return NULL;
    if (unc && rsp_seconds(unc, "uncertainty", 0, &o.uncertainty_ns) < 0) return NULL;
    o.frame = -1;
    if (frame != Py_None) {
        if (rsp_int(frame, -1, INT64_MAX, "frame", &v) < 0) return NULL;
        o.frame = (int64_t)v;
    }
    o.tier = (uint8_t)tier;
    o.src = (uint8_t)e;
    o.final = (uint8_t)(final != 0);
    t_arm = o.t_ns;
    if (arm_o != Py_None && rsp_seconds(arm_o, "arm", 0, &t_arm) < 0) return NULL;
    if (end_o != Py_None && rsp_seconds(end_o, "end", 0, &t_end) < 0) return NULL;
    if (!(it = PyObject_GetIter(events))) return NULL;
    while ((x = PyIter_Next(it))) {
        int bad = rsp_event_in(x, &ev) < 0;
        Py_DECREF(x);
        if (bad) { Py_DECREF(it); return NULL; }
        if (!armed && ev.t >= t_arm) {
            yrsp_arm(&s->c, t_arm);
            yrsp_set_onset(&s->c, &o);
            armed = 1;
        }
        if ((rc = yrsp_feed(&s->c, &ev)) < 0) { Py_DECREF(it); return coll_status(s, rc); }
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) return NULL;
    if (!armed) {
        yrsp_arm(&s->c, t_arm);
        yrsp_set_onset(&s->c, &o);
    }
    if (end_o != Py_None && (rc = yrsp_update(&s->c, t_end)) < 0) return coll_status(s, rc);
    return Coll_finish(self, NULL);
}

/* The YRSP_DATA_COLS columns of the last finish(), by yrsp_data_set()'s
 * rules: None where that writes an empty field. */
static PyObject* Coll_data_row(PyObject* self, PyObject* Py_UNUSED(a)) {
    CollObject* s = (CollObject*)self;
    const yrsp_result* r = &s->last;
    int on = r->t_onset != 0, resp = (r->flags & YRSP_R_RESPONDED) != 0;
    PyObject *d, *name;
    if (!s->have_last) { PyErr_SetString(RspStateError, "data_row(): no finish() yet"); return NULL; }
    name = r->response >= 0 && s->names && r->response < PyTuple_Size(s->names) ? PyTuple_GetItem(s->names, r->response) : Py_None;
    d = Py_BuildValue("{s:O,s:N,s:N,s:N,s:N,s:N,s:N,s:N,s:N,s:k,s:i,s:i}", "response", name, "rt_s",
                      rsp_opt_s(r->rt_ns, r->rt_ns == YRSP_NO_TIME), "rt_key_duration_s",
                      rsp_opt_s(r->rt_key_duration_ns, r->rt_key_duration_ns == YRSP_NO_TIME), "onset_src",
                      on ? PyLong_FromLong(r->onset_src) : (Py_INCREF(Py_None), Py_None), "onset_tier",
                      on ? PyLong_FromLong(r->onset_tier) : (Py_INCREF(Py_None), Py_None), "onset_residual_s",
                      rsp_opt_s(r->onset_residual, !on), "landing_residual_s",
                      rsp_opt_s(r->plan_residual, r->plan_residual == YRSP_NO_TIME), "onset_frame",
                      rsp_opt_ns(r->onset_frame, r->onset_frame < 0), "rsp_tier",
                      resp ? PyLong_FromLong(r->stamp_tier) : (Py_INCREF(Py_None), Py_None), "rsp_flags",
                      (unsigned long)r->flags, "n_anticipations", r->n_anticipations, "n_early", r->n_early);
    return d;
}

static PyObject* Coll_close(PyObject* self, PyObject* Py_UNUSED(a)) {
    CollObject* s = (CollObject*)self;
    if (s->opened) yrsp_close(&s->c);
    s->opened = 0;
    Py_RETURN_NONE;
}

static PyObject* Coll_enter(PyObject* self, PyObject* Py_UNUSED(a)) {
    Py_INCREF(self);
    return self;
}

static PyObject* Coll_exit(PyObject* self, PyObject* Py_UNUSED(a)) {
    PyObject* r = Coll_close(self, NULL);
    Py_XDECREF(r);
    Py_RETURN_FALSE;
}

static PyObject* Coll_get_error(PyObject* self, void* Py_UNUSED(c)) {
    return PyUnicode_FromString(yrsp_error(&((CollObject*)self)->c));
}

static PyObject* Coll_get_plan_frame(PyObject* self, void* Py_UNUSED(c)) {
    int64_t k = yrsp_plan_frame(&((CollObject*)self)->c);
    if (k < 0) Py_RETURN_NONE;
    return PyLong_FromLongLong(k);
}

static PyObject* Coll_get_closed(PyObject* self, void* Py_UNUSED(c)) { return PyBool_FromLong(!((CollObject*)self)->opened); }

static PyObject* Coll_get_choices(PyObject* self, void* Py_UNUSED(c)) {
    CollObject* s = (CollObject*)self;
    if (!s->names) return PyTuple_New(0);
    Py_INCREF(s->names);
    return s->names;
}

#define KWF(f) (PyCFunction)(void (*)(void))(f), METH_VARARGS | METH_KEYWORDS
static PyMethodDef Coll_methods[] = {
    { "arm", Coll_arm, METH_O, "arm(t): start a window at t (s); clears the entries and the onset, keeps which keys are down." },
    { "set_onset", KWF(Coll_set_onset),
      "set_onset(t, *, final=True, src='other', tier=0, residual=0, uncertainty=0, frame=None, t_ns=None) -> status. "
      "The onset RT runs from; a plan (final=False) does not replace a final onset. Classifies every entry again." },
    { "feed", Coll_feed, METH_O, "feed(event) -> status. One input: an Event, a 16-tuple in its order, or an object with its attributes." },
    { "feed_many", Coll_feed_many, METH_O, "feed_many(events) -> status. Each event of an iterable, in order." },
    { "key", KWF(Coll_key),
      "key(t, key, down=True, *, device=0, repeat=False, stamp=0, code=0, t_ns=None) -> status. One keyboard event: "
      "key is a name ('f', 'space', 'left') or a scancode." },
    { "update", Coll_update, METH_O, "update(now) -> status. The deadline, and max_hold while waiting for a release." },
    { "finish", Coll_finish, METH_NOARGS, "finish() -> Result. Call it again after a late release, onset or report." },
    { "entries", Coll_entries, METH_NOARGS, "entries() -> tuple of Entry: every press of the window, in arrival order." },
    { "trace", Coll_trace, METH_NOARGS, "trace() -> tuple of Event: the inputs traced up to the last finish()." },
    { "set_sent", KWF(Coll_set_sent),
      "set_sent(sent, total=None): the codes sent at presses of this window, (t, kind, device, control, choice, written) "
      "each; finish() flags R_SENT_MISMATCH where they and the responses disagree." },
    { "score", KWF(Coll_score),
      "score(events, onset, *, arm=None, end=None, final=True, src='other', tier=0, residual=0, uncertainty=0, frame=None) "
      "-> Result. One trial from a logged event stream." },
    { "data_row", Coll_data_row, METH_NOARGS, "data_row() -> dict: the YRSP_DATA_COLS columns of the last finish()." },
    { "close", Coll_close, METH_NOARGS, "close(): calls after it raise StateError." },
    { "__enter__", Coll_enter, METH_NOARGS, NULL },
    { "__exit__", Coll_exit, METH_VARARGS, NULL },
    { NULL }
};
#undef KWF

static PyGetSetDef Coll_getset[] = {
    { "error", Coll_get_error, NULL, "The message of the last failure.", NULL },
    { "plan_frame", Coll_get_plan_frame, NULL, "The frame whose flip record makes the onset final, or None.", NULL },
    { "closed", Coll_get_closed, NULL, "True after close().", NULL },
    { "choices", Coll_get_choices, NULL, "Each choice's name (the response column), in order.", NULL },
    { NULL }
};

static PyType_Slot Coll_slots[] = {
    { Py_tp_init, (void*)Coll_init },
    { Py_tp_dealloc, (void*)Coll_dealloc },
    { Py_tp_methods, Coll_methods },
    { Py_tp_getset, Coll_getset },
    { Py_tp_doc, (void*)"Collector(choices=None, *, mode=None, kinds=None, min_rt=0, duration=None, persist=False, "
                        "max_responses=0, allow_held_key=False, wait_for_key_release=False, max_hold=0, sources=None, "
                        "trace_cap=0, trace_kinds=None)\n\nThe response rules of a trial (ysp/response.h). Times in seconds." },
    { 0, NULL }
};

static PyType_Spec Coll_spec = { "ysp.response.Collector", sizeof(CollObject), 0, Py_TPFLAGS_DEFAULT, Coll_slots };

/* --- module ------------------------------------------------------------------ */

static PyObject* mod_scancode(PyObject* Py_UNUSED(m), PyObject* arg) {
    PyObject* b = rsp_utf8(arg, "name");
    int k;
    if (!b) return NULL;
    k = yrsp_scancode(PyBytes_AsString(b));
    Py_DECREF(b);
    if (k < 0) Py_RETURN_NONE;
    return PyLong_FromLong(k);
}

static PyObject* mod_key_name(PyObject* Py_UNUSED(m), PyObject* arg) {
    long long v;
    const char* n;
    if (rsp_int(arg, 0, 0xFFFFFFFFLL, "scancode", &v) < 0) return NULL;
    n = yrsp_key_name((uint32_t)v);
    if (!n) Py_RETURN_NONE;
    return PyUnicode_FromString(n);
}

static PyObject* mod_version(PyObject* Py_UNUSED(m), PyObject* Py_UNUSED(a)) { return PyUnicode_FromString(yrsp_version()); }

#define KWF(f) (PyCFunction)(void (*)(void))(f), METH_VARARGS | METH_KEYWORDS
static PyMethodDef module_methods[] = {
    { "event", KWF(mod_event),
      "event(t, kind=KIND_KEYBOARD, type=PRESS, control=0, device=0, code=0, mods=0, flags=0, stamp=0, unc=0, x=0, "
      "y=0, value=None, aux=0, ticks=0, t_ns=None) -> Event. control may be a key name; value None is 1 for a press, 0 else." },
    { "scancode", mod_scancode, METH_O, "scancode(name) -> int or None: the key's scancode on the US layout." },
    { "key_name", mod_key_name, METH_O, "key_name(scancode) -> str or None." },
    { "version", mod_version, METH_NOARGS, "version() -> ysp/response.h's version string." },
    { NULL }
};
#undef KWF

static PyModuleDef ysp_response_module = {
    PyModuleDef_HEAD_INIT, "ysp.response",
    "The response of a trial from any input device: choices, the window, RT from the stimulus onset and the timing "
    "quality of both (ysp/response.h). Times in seconds.",
    -1, module_methods,
};

static PyObject* rsp_add_exc(PyObject* m, const char* qual, const char* attr, PyObject* base) {
    PyObject* e = PyErr_NewException(qual, base, NULL);
    if (!e) return NULL;
    Py_INCREF(e);
    if (PyModule_AddObject(m, attr, e) < 0) { Py_DECREF(e); Py_DECREF(e); return NULL; }
    return e;
}

static PyStructSequence_Field ev_fields[EV_NFIELDS + 1];
static PyStructSequence_Field res_fields[] = {
    { "rt", "seconds from the onset to the response; None when there is none" },
    { "rt_ns", "rt in exact ns" },
    { "rt_key_duration", "seconds from the response's press to its release; None when no release came" },
    { "rt_key_duration_ns", "rt_key_duration in exact ns" },
    { "response", "the choice's name (the data's response column); None" },
    { "choice", "the choice's index; None (no response, or ALL mode)" },
    { "entry", "the response's index in entries(); None" },
    { "t_response", "the response's time, seconds; None" },
    { "t_response_ns", "t_response in exact ns" },
    { "kind", "the response's kind" },
    { "device", "the response's device" },
    { "control", "the response's control (scancode, button)" },
    { "code", "the response's code (keycode)" },
    { "stamp_tier", "the response stamp's tier" },
    { "stamp_partial", "the source's bounds cover the host part only" },
    { "stamp_lo", "the source's bounds: stamp minus event time, seconds" },
    { "stamp_hi", "" },
    { "t_onset", "the onset RT runs from, seconds; None when there is none" },
    { "t_onset_ns", "t_onset in exact ns" },
    { "onset_src", "ONSET_OTHER, ONSET_FLIP, ONSET_AUDIO, ONSET_PLAN" },
    { "onset_tier", "the onset's tier" },
    { "onset_residual", "the onset's residual, seconds" },
    { "onset_uncertainty", "the half-width the caller stated, seconds; 0 = not stated" },
    { "onset_frame", "the onset's frame; None" },
    { "plan_residual", "the first plan's residual (a landing residual), seconds; None" },
    { "flags", "R_* bits" },
    { "n_responses", "" }, { "n_early", "" }, { "n_anticipations", "" }, { "n_late", "" }, { "n_after_end", "" },
    { "n_not_choice", "" }, { "n_held", "" }, { "n_entries", "" }, { "n_lost", "" }, { "n_trace", "" },
    { "n_sent", "codes written (set_sent)" },
    { NULL, NULL }
};
static PyStructSequence_Field ent_fields[] = {
    { "t", "seconds" }, { "t_ns", "" }, { "t_release", "seconds; None" }, { "t_release_ns", "" }, { "kind", "" },
    { "device", "" }, { "control", "" }, { "code", "" }, { "choice", "index; None" }, { "flags", "E_* bits" },
    { "stamp", "tier" }, { "mods", "" }, { "value", "" }, { "arrival_class", "E_* class bits when it arrived" },
    { NULL, NULL }
};

static PyTypeObject* rsp_structseq(PyObject* m, const char* attr, const char* name, const char* doc,
                                   PyStructSequence_Field* fields, int n) {
    PyStructSequence_Desc d;
    PyTypeObject* t;
    d.name = name;
    d.doc = doc;
    d.fields = fields;
    d.n_in_sequence = n;
    t = PyStructSequence_NewType(&d);
    if (!t) return NULL;
    Py_INCREF(t);
    if (PyModule_AddObject(m, attr, (PyObject*)t) < 0) { Py_DECREF(t); Py_DECREF(t); return NULL; }
    return t;
}

PyMODINIT_FUNC PyInit_response(void) {
    PyObject *m, *t, *base2;
    int i;
    m = PyModule_Create(&ysp_response_module);
    if (!m) return NULL;
    for (i = 0; i < EV_NFIELDS; i++) {
        ev_fields[i].name = ev_names[i];
        ev_fields[i].doc = i == F_T ? "seconds" : i == F_TNS ? "exact ns" : i == F_UNC ? "seconds (from unc_us)" : "";
        if (!ev_attr[i] && !(ev_attr[i] = PyUnicode_InternFromString(ev_names[i]))) goto fail;
    }
    ev_fields[EV_NFIELDS].name = NULL;
    ev_fields[EV_NFIELDS].doc = NULL;
    if (!(EventType = rsp_structseq(m, "Event", "ysp.response.Event", "One input event (ysp/input.h yin_event), times in seconds.",
                                    ev_fields, EV_NFIELDS)))
        goto fail;
    if (!(ResultType = rsp_structseq(m, "Result", "ysp.response.Result", "A trial's response (yrsp_result), times in seconds.",
                                     res_fields, R_NFIELDS)))
        goto fail;
    if (!(EntryType = rsp_structseq(m, "Entry", "ysp.response.Entry", "One press of the window (yrsp_entry).", ent_fields, N_NFIELDS)))
        goto fail;
    if (!(t = PyType_FromSpec(&Coll_spec))) goto fail;
    if (PyModule_AddObject(m, "Collector", t) < 0) { Py_DECREF(t); goto fail; }
    if (!(RspError = rsp_add_exc(m, "ysp.response.Error", "Error", NULL))) goto fail;
    base2 = PyTuple_Pack(2, RspError, PyExc_ValueError);
    if (!base2) goto fail;
    RspArgumentError = rsp_add_exc(m, "ysp.response.ArgumentError", "ArgumentError", base2);
    Py_DECREF(base2);
    RspStateError = rsp_add_exc(m, "ysp.response.StateError", "StateError", RspError);
    if (!RspArgumentError || !RspStateError) goto fail;
    if (PyModule_AddStringConstant(m, "__version__", yrsp_version()) < 0) goto fail;
    if (PyModule_AddStringConstant(m, "INPUT_VERSION", YIN_VERSION_STRING) < 0) goto fail;
#define C(name, v) if (PyModule_AddIntConstant(m, name, (long)(v)) < 0) goto fail;
    /* ysp/input.h */
    C("KIND_KEYBOARD", YIN_KIND_KEYBOARD) C("KIND_MOUSE", YIN_KIND_MOUSE) C("KIND_TOUCH", YIN_KIND_TOUCH)
    C("KIND_PEN", YIN_KIND_PEN) C("KIND_GAMEPAD", YIN_KIND_GAMEPAD) C("KIND_BOX", YIN_KIND_BOX)
    C("KIND_EYE", YIN_KIND_EYE) C("KIND_SYNC", YIN_KIND_SYNC) C("KIND_USER", YIN_KIND_USER)
    C("KINDS_ALL", YIN_KINDS_ALL)
    C("NONE", YIN_NONE) C("PRESS", YIN_PRESS) C("RELEASE", YIN_RELEASE) C("SAMPLE", YIN_SAMPLE) C("PROXIMITY", YIN_PROXIMITY)
    C("REPEAT", YIN_REPEAT) C("SYNTHETIC", YIN_SYNTHETIC) C("ERASER", YIN_ERASER) C("UNLISTED", YIN_UNLISTED)
    C("DEVTICKS", YIN_DEVTICKS)
    C("TIER_UNKNOWN", YIN_TIER_UNKNOWN) C("TIER_1", YIN_TIER_1) C("TIER_2", YIN_TIER_2) C("TIER_3", YIN_TIER_3)
    C("TIER_SIM", YIN_TIER_SIM)
    C("AXIS_POSITION", YIN_AXIS_POSITION) C("AXIS_PEN", YIN_AXIS_PEN) C("AXIS_PEN_PRESSURE", YIN_AXIS_PEN_PRESSURE)
    C("AXIS_GAMEPAD", YIN_AXIS_GAMEPAD) C("AXIS_DELTA", YIN_AXIS_DELTA) C("AXIS_ABSOLUTE", YIN_AXIS_ABSOLUTE)
    C("AXIS_WHEEL", YIN_AXIS_WHEEL)
    C("EYE_GAZE", YIN_EYE_GAZE) C("EYE_FIXATION", YIN_EYE_FIXATION) C("EYE_SACCADE", YIN_EYE_SACCADE)
    C("EYE_BLINK", YIN_EYE_BLINK) C("EYE_LEFT", YIN_EYE_LEFT) C("EYE_RIGHT", YIN_EYE_RIGHT) C("EYE_BOTH", YIN_EYE_BOTH)
    C("UNC_MAX_US", YIN_UNC_MAX)
    /* ysp/response.h */
    C("IDLE", YRSP_IDLE) C("OPEN", YRSP_OPEN) C("ENDED", YRSP_ENDED)
    C("CHOICES_ALL", YRSP_CHOICES_ALL) C("CHOICES_LIST", YRSP_CHOICES_LIST) C("CHOICES_NONE", YRSP_CHOICES_NONE)
    C("MATCH_SCANCODE", YRSP_MATCH_SCANCODE) C("MATCH_KEYCODE", YRSP_MATCH_KEYCODE)
    C("CROSS_NONE", YRSP_CROSS_NONE) C("CROSS_RISING", YRSP_CROSS_RISING) C("CROSS_FALLING", YRSP_CROSS_FALLING)
    C("CROSS_DISTANCE", YRSP_CROSS_DISTANCE)
    C("ONSET_OTHER", YRSP_ONSET_OTHER) C("ONSET_FLIP", YRSP_ONSET_FLIP) C("ONSET_AUDIO", YRSP_ONSET_AUDIO)
    C("ONSET_PLAN", YRSP_ONSET_PLAN)
    C("E_VALID", YRSP_E_VALID) C("E_EARLY", YRSP_E_EARLY) C("E_ANTICIPATION", YRSP_E_ANTICIPATION)
    C("E_LATE", YRSP_E_LATE) C("E_AFTER_END", YRSP_E_AFTER_END) C("E_NOT_CHOICE", YRSP_E_NOT_CHOICE)
    C("E_HELD", YRSP_E_HELD) C("E_CROSSING", YRSP_E_CROSSING) C("E_RECLASSIFIED", YRSP_E_RECLASSIFIED)
    C("E_RELEASED", YRSP_E_RELEASED) C("E_ENDED", YRSP_E_ENDED) C("E_CLASS_MASK", YRSP_E_CLASS_MASK)
    C("R_RESPONDED", YRSP_R_RESPONDED) C("R_TIMEOUT", YRSP_R_TIMEOUT) C("R_ENDED_EARLY", YRSP_R_ENDED_EARLY)
    C("R_ONSET_PLAN", YRSP_R_ONSET_PLAN) C("R_NO_ONSET", YRSP_R_NO_ONSET) C("R_LOG_FULL", YRSP_R_LOG_FULL)
    C("R_TRACE_FULL", YRSP_R_TRACE_FULL) C("R_RECLASSIFIED", YRSP_R_RECLASSIFIED)
    C("R_HELD_AT_OPEN", YRSP_R_HELD_AT_OPEN) C("R_HOLD_TIMEOUT", YRSP_R_HOLD_TIMEOUT)
    C("R_SENT_MISMATCH", YRSP_R_SENT_MISMATCH)
    C("MAX_CHOICES", YRSP_MAX_CHOICES) C("MAX_SOURCES", YRSP_MAX_SOURCES) C("LOG_CAP", YRSP_LOG_CAP)
    C("LOG_RESERVE", YRSP_LOG_RESERVE) C("MAX_SENT", YRSP_MAX_SENT)
#undef C
    {
        static const char* const cols[] = { "response", "rt_s", "rt_key_duration_s", "onset_src", "onset_tier",
                                            "onset_residual_s", "landing_residual_s", "onset_frame", "rsp_tier",
                                            "rsp_flags", "n_anticipations", "n_early" };
        PyObject* c = PyTuple_New(12);
        if (!c) goto fail;
        for (i = 0; i < 12; i++) PyTuple_SetItem(c, i, PyUnicode_FromString(cols[i]));
        if (PyErr_Occurred() || PyModule_AddObject(m, "DATA_COLS", c) < 0) { Py_DECREF(c); goto fail; }
    }
    return m;
fail:
    Py_DECREF(m);
    return NULL;
}
