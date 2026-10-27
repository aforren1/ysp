/* ysp_vis_ext.c - CPython extension ysp._vis: ysp/screen.h, ysp/gfx.h, ysp/video.h in one module
 *
 * One module, because their handles point at each other and screen keeps
 * state per process (EGL, the abort log, the window thread). The thin
 * modules ysp.screen, ysp.gfx and ysp.video re-export from here
 * (docs/rig_spec.md, section 12; docs/bindings.md, ysp-vis):
 *
 *     import ysp.screen as yscr, ysp.gfx as ygfx
 *     with yscr.Screen(windowed=True) as s, ygfx.Gfx(s, background=(0.5,) * 3) as g:
 *         while True:
 *             f = s.begin()                  # GIL released; Abort on Shift+Esc
 *             g.begin(); g.rect(0, 0, 100, 100, (1, 1, 1)); g.end()
 *             s.flip()                       # GIL released
 *
 * TIMES: seconds (double), rounded to the nearest ns for the C call; a
 * negative or NaN time is refused before the call. Records, frames and
 * events give the exact ns beside the seconds.
 *
 * THREADS: the window belongs to ysp/screen.h's window thread, so the host
 * may wait between trials for any time (time.sleep, input) without a hung
 * window. open, close, begin, flip, flip_at, wait_flip, the display lists
 * and the window calls release the GIL; a second thread's call on a Screen
 * meanwhile raises BusyError. No C thread takes the GIL: no Python callback
 * runs on one.
 *
 * SHARING: audio.h and timeline.h are declarations here; their calls go
 * through the tables of ysp.audio and ysp.timeline (bindings/share/), which
 * this module imports when it loads.
 *
 * NATIVES: SDL3 is linked statically; ANGLE's libEGL.dll and libGLESv2.dll
 * are beside this file, and desc.angle_dir is always this file's directory.
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x03080000
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define YSP_SHARE_PYTHON
#include "ysp_vis_core.h"

static PyObject *VsError, *VsArgumentError, *VsClosedError, *VsOrderError, *VsTimeoutError, *VsLostError,
    *VsRefusedError, *VsNotImplementedError, *VsBusyError, *VsAbort;
static PyObject *ScreenType, *GfxType;
static PyTypeObject *FrameType, *RecordType, *EventType, *DisplayType, *ModeType;

/* --- errors ------------------------------------------------------------------ */

static PyObject* vs_exc(int code) {
    switch (code) {
    case YSCR_ERR_ARG: return VsArgumentError;
    case YSCR_ERR_CLOSED: return VsClosedError;
    case YSCR_ERR_TIMEOUT: return VsTimeoutError;
    case YSCR_ERR_LOST: return VsLostError;
    case YSCR_ERR_ORDER: return VsOrderError;
    case YSCR_ERR_NOT_IMPLEMENTED: return VsNotImplementedError;
    case YSCR_ERR_REFUSED: return VsRefusedError;
    default: return VsError;
    }
}

static PyObject* vs_gfx_exc(int code) {
    switch (code) {
    case YGFX_ERR_ARG: case YGFX_ERR_RANGE: case YGFX_ERR_FORMAT: return VsArgumentError;
    case YGFX_ERR_CLOSED: return VsClosedError;
    case YGFX_ERR_ORDER: return VsOrderError;
    case YGFX_ERR_LOST: return VsLostError;
    case YGFX_ERR_NOT_IMPLEMENTED: return VsNotImplementedError;
    case YGFX_ERR_REFUSED: return VsRefusedError;
    default: return VsError;
    }
}

static int vs_obj_s(PyObject* o, const char* what, int negative, int64_t* out) {
    double s = PyFloat_AsDouble(o);
    if (s == -1.0 && PyErr_Occurred()) return -1;
    if (yv_s_to_ns(s, negative, out) < 0) {
        PyErr_Format(VsArgumentError, "%s: seconds%s (NaN and infinity are refused)", what, negative ? "" : ", >= 0");
        return -1;
    }
    return 0;
}

static int vs_utf8(PyObject* o, char* buf, size_t cap) {
    PyObject* b;
    if (!PyUnicode_Check(o)) { PyErr_SetString(PyExc_TypeError, "a str"); return -1; }
    b = PyUnicode_AsUTF8String(o);
    if (!b) return -1;
    PyOS_snprintf(buf, cap, "%s", PyBytes_AsString(b));
    Py_DECREF(b);
    return 0;
}

/* --- structseqs ---------------------------------------------------------------- */

static PyObject* vs_seq(PyTypeObject* t, PyObject** v, int n) {
    PyObject* o;
    int i;
    for (i = 0; i < n; i++) {
        if (!v[i]) {
            for (i = 0; i < n; i++) Py_XDECREF(v[i]);
            return NULL;
        }
    }
    o = PyStructSequence_New(t);
    if (!o) { for (i = 0; i < n; i++) Py_DECREF(v[i]); return NULL; }
    for (i = 0; i < n; i++) PyStructSequence_SetItem(o, i, v[i]);   /* steals */
    return o;
}

static PyStructSequence_Field vs_record_fields[] = {
    { "index", "the frame number" },
    { "target", "the time given to flip_at, seconds on the ysp_rt clock" },
    { "target_ns", "the same, exact int ns" },
    { "planned", "the vblank the flip was planned for (+ onset_offset), seconds" },
    { "planned_ns", "the same, exact int ns" },
    { "onset", "the vblank it was shown on (+ onset_offset), seconds; 0 if never shown" },
    { "onset_ns", "the same, exact int ns" },
    { "residual", "onset - target, seconds" },
    { "residual_ns", "the same, exact int ns" },
    { "dropped", "vblanks between planned and shown: the system's lateness" },
    { "missed", "dropped > 0" },
    { "late", "FLIP_LATE_TARGET: the caller asked for a vblank it could no longer make" },
    { "path", "PATH_COMPOSED, PATH_OVERLAY, PATH_INDEPENDENT, PATH_SIMULATED" },
    { "tier", "TIER_1 to TIER_3, TIER_SIM: how far the onset can be trusted" },
    { "flags", "FLIP_* bits" },
    { "phases", "seconds charged to PHASE_EVALUATE, _SCRIPT, _DRAW, _UPLOAD, _SWAP, _GPU; None: unknown" },
    { "code_risk", "CODE_RISK_* bits; 0 without codes" },
    { NULL, NULL }
};
static PyStructSequence_Desc vs_record_desc = {
    "ysp.screen.Record", "The record of one flip (yscr_record): times in seconds with the exact ns beside them.",
    vs_record_fields, 17 };

static PyObject* vs_record(const yscr_record* r) {
    PyObject* v[17];
    PyObject* ph = PyTuple_New(YSCR_N_PHASES);
    int i;
    if (ph) {
        for (i = 0; i < YSCR_N_PHASES; i++) {
            PyObject* x;
            if (r->phase_ns[i] == YSCR_PHASE_UNKNOWN) { x = Py_None; Py_INCREF(x); }
            else x = PyFloat_FromDouble((double)r->phase_ns[i] / 1e9);
            if (!x) { Py_DECREF(ph); ph = NULL; break; }
            PyTuple_SetItem(ph, i, x);
        }
    }
    v[0] = PyLong_FromLongLong(r->index);
    v[1] = PyFloat_FromDouble(yv_s(r->target));
    v[2] = PyLong_FromLongLong(r->target);
    v[3] = PyFloat_FromDouble(yv_s(r->planned));
    v[4] = PyLong_FromLongLong(r->planned);
    v[5] = PyFloat_FromDouble(yv_s(r->onset));
    v[6] = PyLong_FromLongLong(r->onset);
    v[7] = PyFloat_FromDouble(yv_s(r->residual));
    v[8] = PyLong_FromLongLong(r->residual);
    v[9] = PyLong_FromUnsignedLong(r->dropped);
    v[10] = PyBool_FromLong(r->dropped > 0);
    v[11] = PyBool_FromLong((r->flags & YSCR_FLIP_LATE_TARGET) != 0);
    v[12] = PyLong_FromLong(r->path);
    v[13] = PyLong_FromLong(r->tier);
    v[14] = PyLong_FromLong(r->flags);
    v[15] = ph;
    v[16] = PyLong_FromLong(r->code_risk);
    return vs_seq(RecordType, v, 17);
}

static PyStructSequence_Field vs_frame_fields[] = {
    { "onset", "the predicted onset of this frame's flip, seconds on the ysp_rt clock" },
    { "onset_ns", "the same, exact int ns" },
    { "period", "the refresh period, seconds; 0 without a fixed period" },
    { "period_ns", "the same, exact int ns" },
    { "index", "the frame number, 0 for the first" },
    { "vblank", "the vblank count it is planned for" },
    { "done", "tuple of the Records completed since the begin() before, in completion order" },
    { "last", "the newest completed Record, or None before one" },
    { "done_lost", "records that did not fit in done since open (see Screen.records())" },
    { NULL, NULL }
};
static PyStructSequence_Desc vs_frame_desc = {
    "ysp.screen.Frame", "What begin() gives a frame (yscr_frame): draw for onset.", vs_frame_fields, 9 };

static PyObject* vs_frame(const yscr_frame* f) {
    PyObject* v[9];
    PyObject* done = PyTuple_New(f->n_done > 0 ? f->n_done : 0);
    PyObject* last = NULL;
    int i;
    if (done) {
        for (i = 0; i < f->n_done; i++) {
            PyObject* r = vs_record(&f->done[i]);
            if (!r) { Py_DECREF(done); done = NULL; break; }
            PyTuple_SetItem(done, i, r);
        }
    }
    /* f.last is the header's copy of done's final record: one object for both. */
    if (done && f->n_done > 0 && f->last && f->last->index == f->done[f->n_done - 1].index &&
        f->last->onset == f->done[f->n_done - 1].onset && f->last->flags == f->done[f->n_done - 1].flags) {
        last = PyTuple_GetItem(done, f->n_done - 1);
        Py_XINCREF(last);
    } else if (f->last) {
        last = vs_record(f->last);
    } else {
        last = Py_None;
        Py_INCREF(last);
    }
    v[0] = PyFloat_FromDouble(yv_s(f->onset));
    v[1] = PyLong_FromLongLong(f->onset);
    v[2] = PyFloat_FromDouble(yv_s(f->period));
    v[3] = PyLong_FromLongLong(f->period);
    v[4] = PyLong_FromLongLong(f->index);
    v[5] = PyLong_FromLongLong(f->vblank);
    v[6] = done;
    v[7] = last;
    v[8] = PyLong_FromUnsignedLong(f->done_lost);
    return vs_seq(FrameType, v, 9);
}

/* The shared input event layout (docs/bindings.md, "The input event"). */
static PyStructSequence_Field vs_event_fields[] = {
    { "t", "the event's time, seconds on the ysp_rt clock" },
    { "t_ns", "the same time, exact int ns" },
    { "device", "the instance within the kind" },
    { "control", "button, channel or scancode" },
    { "code", "keyboard: SDL keycode; else the device's" },
    { "mods", "keyboard: SDL_Keymod" },
    { "kind", "KIND_*" },
    { "type", "PRESS, RELEASE, SAMPLE, PROXIMITY" },
    { "flags", "REPEAT, SYNTHETIC, ERASER, UNLISTED, DEVTICKS" },
    { "stamp", "TIER_*; 0: the source's" },
    { "unc", "this event's uncertainty, seconds (unc_us); 0: the source's" },
    { "x", "position or axis" },
    { "y", "position or axis" },
    { "value", "level, sample value" },
    { "aux", "extra value" },
    { "ticks", "the device clock, low 32 bits (with DEVTICKS)" },
    { NULL, NULL }
};
static PyStructSequence_Desc vs_event_desc = {
    "ysp.screen.Event", "One input event (ysp/input.h yin_event). t is seconds on the ysp_rt clock; t_ns the exact int ns.",
    vs_event_fields, 16 };

static PyObject* vs_event(const yin_event* e) {
    PyObject* v[16];
    v[0] = PyFloat_FromDouble(yv_s(e->t));
    v[1] = PyLong_FromLongLong(e->t);
    v[2] = PyLong_FromUnsignedLong(e->device);
    v[3] = PyLong_FromUnsignedLong(e->control);
    v[4] = PyLong_FromUnsignedLong(e->code);
    v[5] = PyLong_FromLong(e->mods);
    v[6] = PyLong_FromLong(e->kind);
    v[7] = PyLong_FromLong(e->type);
    v[8] = PyLong_FromLong(e->flags);
    v[9] = PyLong_FromLong(e->stamp);
    v[10] = PyFloat_FromDouble((double)e->unc_us * 1e-6);
    v[11] = PyFloat_FromDouble(e->x);
    v[12] = PyFloat_FromDouble(e->y);
    v[13] = PyFloat_FromDouble(e->value);
    v[14] = PyFloat_FromDouble(e->aux);
    v[15] = PyLong_FromUnsignedLong(e->ticks);
    return vs_seq(EventType, v, 16);
}

static PyStructSequence_Field vs_mode_fields[] = {
    { "w", "pixels" }, { "h", "pixels" },
    { "refresh_num", "the refresh as num/den Hz; 0: unknown" }, { "refresh_den", "" },
    { "refresh", "Hz" }, { "period", "seconds" }, { "period_ns", "exact int ns" },
    { "format", "SDL pixel format" }, { "density", "pixel density" },
    { NULL, NULL }
};
static PyStructSequence_Desc vs_mode_desc = { "ysp.screen.Mode", "A display mode (yscr_mode).", vs_mode_fields, 9 };

static PyObject* vs_mode(const yscr_mode* m) {
    PyObject* v[9];
    v[0] = PyLong_FromLong(m->w);
    v[1] = PyLong_FromLong(m->h);
    v[2] = PyLong_FromLong(m->refresh_num);
    v[3] = PyLong_FromLong(m->refresh_den);
    v[4] = PyFloat_FromDouble(m->refresh_den ? (double)m->refresh_num / m->refresh_den : 0.0);
    v[5] = PyFloat_FromDouble(yv_s(m->period_ns));
    v[6] = PyLong_FromLongLong(m->period_ns);
    v[7] = PyLong_FromUnsignedLong(m->format);
    v[8] = PyFloat_FromDouble(m->density);
    return vs_seq(ModeType, v, 9);
}

static PyStructSequence_Field vs_display_fields[] = {
    { "id", "the display's id: Screen(display=id)" }, { "name", "" },
    { "x", "desktop bounds, pixels" }, { "y", "" }, { "w", "" }, { "h", "" },
    { "primary", "the primary display" }, { "desktop", "the desktop Mode" },
    { NULL, NULL }
};
static PyStructSequence_Desc vs_display_desc = { "ysp.screen.Display", "A connected display (yscr_display_info).",
                                                 vs_display_fields, 8 };

/* --- Screen -------------------------------------------------------------------- */

typedef struct ScreenObject {
    PyObject_HEAD
    yv_screen* v;
    int        busy;      /* a call runs without the GIL */
    struct ScreenObject* next_open;
} ScreenObject;

#define SC_OF(o) ((ScreenObject*)(o))

/* The open screens, for _close_all() at exit. */
static ScreenObject* vs_open_list;

static void vs_unlist(ScreenObject* o) {
    ScreenObject** p = &vs_open_list;
    while (*p) {
        if (*p == o) { *p = o->next_open; o->next_open = NULL; return; }
        p = &(*p)->next_open;
    }
}

static int sc_ready(ScreenObject* o) {
    if (o->busy) { PyErr_SetString(VsBusyError, "ysp_screen: another thread is in a call on this Screen"); return 0; }
    if (!o->v || !o->v->open) { PyErr_SetString(VsClosedError, "ysp_screen: the screen is closed"); return 0; }
    return 1;
}

static PyObject* sc_fail(ScreenObject* o, int code) {
    const char* m = o->v ? yscr_error(&o->v->s) : "";
    PyErr_SetString(vs_exc(code), m && m[0] ? m : yscr_strerror(code));
    return NULL;
}

static void sc_shut(ScreenObject* o) {
    if (o->v && o->v->open) {
        Py_BEGIN_ALLOW_THREADS
        yv_close(o->v);
        Py_END_ALLOW_THREADS
    }
    vs_unlist(o);
}

static void Screen_dealloc(PyObject* self) {
    ScreenObject* o = SC_OF(self);
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    sc_shut(o);
    yv_free(o->v);
    o->v = NULL;
    fr(self);
    Py_DECREF(tp);
}

/* A keyword's value into the desc by the option table. */
static int sc_option(yscr_desc* d, const char* name, PyObject* val) {
    const yv_opt* opt = yv_find_opt(name);
    char err[256];
    if (!opt) {
        PyErr_Format(PyExc_TypeError, "Screen() got an unexpected keyword argument '%s' (options(): the list)", name);
        return -1;
    }
    if (val == Py_None) return 0;
    if (opt->type == YV_ENUM) {
        char s[64];
        if (!PyUnicode_Check(val)) { PyErr_Format(PyExc_TypeError, "%s: a name", name); return -1; }
        if (vs_utf8(val, s, sizeof s) < 0) return -1;
        if (yv_opt_enum(d, opt, s, err, sizeof err) < 0) { PyErr_SetString(VsArgumentError, err); return -1; }
        return 0;
    }
    if (opt->type == YV_SIZE || opt->type == YV_MODE) {
        long n[4] = { 0, 0, 0, 0 };
        Py_ssize_t k, want = opt->type == YV_SIZE ? 2 : 4, len = PySequence_Check(val) ? PySequence_Size(val) : -1;
        /* A Mode from modes() has 9 fields; its first four are the mode. */
        if (len < want || (opt->type == YV_SIZE && len != 2)) {
            PyErr_Clear();
            PyErr_Format(VsArgumentError, opt->type == YV_SIZE ? "%s: (w, h)" : "%s: a Mode, or (w, h, refresh_num, refresh_den)", name);
            return -1;
        }
        for (k = 0; k < want; k++) {
            PyObject* it = PySequence_GetItem(val, k);
            n[k] = it ? PyLong_AsLong(it) : -1;
            Py_XDECREF(it);
            if (n[k] == -1 && PyErr_Occurred()) return -1;
        }
        if (opt->type == YV_SIZE) {
            if (n[0] < 0 || n[1] < 0) { PyErr_Format(VsArgumentError, "%s: pixels, >= 0", name); return -1; }
            d->window_w = (int32_t)n[0];
            d->window_h = (int32_t)n[1];
        } else {
            d->mode.w = (int32_t)n[0];
            d->mode.h = (int32_t)n[1];
            d->mode.refresh_num = (int32_t)n[2];
            d->mode.refresh_den = (int32_t)n[3];
        }
        return 0;
    }
    {
        double x;
        if (opt->type == YV_BOOL) {
            int t = PyObject_IsTrue(val);
            if (t < 0) return -1;
            x = t;
        } else {
            x = PyFloat_AsDouble(val);
            if (x == -1.0 && PyErr_Occurred()) return -1;
        }
        if (yv_opt_num(d, opt, x, err, sizeof err) < 0) { PyErr_SetString(VsArgumentError, err); return -1; }
    }
    return 0;
}

static yv_gfx* vg_gstart(PyObject* opts);   /* vis_gfx.c */

static int Screen_init(PyObject* self, PyObject* args, PyObject* kw) {
    ScreenObject* o = SC_OF(self);
    yscr_desc d;
    bool ok;
    Py_ssize_t pos = 0;
    PyObject *k, *val, *gfx = NULL;
    if (o->v) { PyErr_SetString(VsError, "Screen.__init__ called twice"); return -1; }
    if (PyTuple_Size(args) > 0) { PyErr_SetString(PyExc_TypeError, "Screen() takes keyword arguments only"); return -1; }
    memset(&d, 0, sizeof d);
    while (kw && PyDict_Next(kw, &pos, &k, &val)) {
        char name[64];
        if (vs_utf8(k, name, sizeof name) < 0) return -1;
        /* gfx: the Gfx options; its programs compile while the screen
         * settles, and Gfx(screen) then finishes the open */
        if (!strcmp(name, "gfx")) { gfx = val; continue; }
        if (sc_option(&d, name, val) < 0) return -1;
    }
    o->v = (yv_screen*)calloc(1, sizeof(yv_screen));
    if (!o->v) { PyErr_NoMemory(); return -1; }
    if (gfx && gfx != Py_None && gfx != Py_False) {
        o->v->gstart = vg_gstart(gfx);
        if (!o->v->gstart) { free(o->v); o->v = NULL; return -1; }
    }
    o->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    ok = yv_open(o->v, &d);
    Py_END_ALLOW_THREADS
    o->busy = 0;
    if (!ok) {
        const char* m = o->v->rec ? yscr_error(&o->v->s) : "out of memory";
        PyErr_SetString(VsError, m && m[0] ? m : "ysp_screen: open failed");
        yv_free(o->v);
        o->v = NULL;
        return -1;
    }
    o->next_open = vs_open_list;
    vs_open_list = o;
    return 0;
}

static PyObject* Screen_close(PyObject* self, PyObject* unused) {
    ScreenObject* o = SC_OF(self);
    (void)unused;
    if (o->busy) { PyErr_SetString(VsBusyError, "ysp_screen: another thread is in a call on this Screen"); return NULL; }
    o->busy = 1;
    sc_shut(o);
    o->busy = 0;
    Py_RETURN_NONE;
}

static PyObject* Screen_enter(PyObject* self, PyObject* unused) { (void)unused; Py_INCREF(self); return self; }
static PyObject* Screen_exit(PyObject* self, PyObject* args) { (void)args; return Screen_close(self, NULL); }

static PyObject* vs_abort(const yscr_frame* f) {
    char msg[200];
    PyObject* e;
    int panic = (f->abort & YSCR_ABORT_PANIC) != 0;
    PyOS_snprintf(msg, sizeof msg, "ysp_screen: abort 0x%x (%d press%s)%s", (unsigned)f->abort, (int)f->abort_presses,
                  f->abort_presses == 1 ? "" : "es",
                  panic ? ": the panic released the display; only close() is left" : "");
    static const char* names[5] = { "reasons", "presses", "t", "t_ns", "panic" };
    PyObject* v[5];
    int i, bad = 0;
    e = PyObject_CallFunction(VsAbort, "s", msg);
    if (!e) return NULL;
    v[0] = PyLong_FromUnsignedLong(f->abort);
    v[1] = PyLong_FromLong(f->abort_presses);
    v[2] = PyFloat_FromDouble(yv_s(f->abort_ns));
    v[3] = PyLong_FromLongLong(f->abort_ns);
    v[4] = PyBool_FromLong(panic);
    for (i = 0; i < 5; i++) {
        if (!v[i] || PyObject_SetAttrString(e, names[i], v[i]) < 0) bad = 1;
        Py_XDECREF(v[i]);
    }
    if (bad) { Py_DECREF(e); return NULL; }
    PyErr_SetObject(VsAbort, e);
    Py_DECREF(e);
    return NULL;
}

static PyObject* Screen_begin(PyObject* self, PyObject* unused) {
    ScreenObject* o = SC_OF(self);
    int rc;
    (void)unused;
    if (!sc_ready(o)) return NULL;
    o->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    rc = yv_begin(o->v);
    Py_END_ALLOW_THREADS
    o->busy = 0;
    if (rc == YSCR_QUIT) return vs_abort(&o->v->f);
    if (rc < 0) return sc_fail(o, rc);
    return vs_frame(&o->v->f);
}

static PyObject* sc_flip(ScreenObject* o, int64_t t, int at) {
    int rc;
    if (!sc_ready(o)) return NULL;
    if (o->v->gfx && o->v->gfx->in_frame) {
        PyErr_SetString(VsOrderError, "ysp_screen: Gfx.end() before flip()");
        return NULL;
    }
    o->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    rc = yv_flip_at(o->v, t, at);
    Py_END_ALLOW_THREADS
    o->busy = 0;
    if (rc < 0) return sc_fail(o, rc);
    Py_RETURN_NONE;
}

static PyObject* Screen_flip(PyObject* self, PyObject* unused) { (void)unused; return sc_flip(SC_OF(self), 0, 0); }

static PyObject* Screen_flip_at(PyObject* self, PyObject* arg) {
    int64_t t;
    if (vs_obj_s(arg, "flip_at(t)", 0, &t) < 0) return NULL;
    return sc_flip(SC_OF(self), t, 1);
}

static PyObject* Screen_wait_flip(PyObject* self, PyObject* unused) {
    ScreenObject* o = SC_OF(self);
    yscr_record r;
    int rc;
    (void)unused;
    if (!sc_ready(o)) return NULL;
    o->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    rc = yscr_wait_flip(&o->v->s, &r);
    Py_END_ALLOW_THREADS
    o->busy = 0;
    if (rc < 0) return sc_fail(o, rc);
    return vs_record(&r);
}

static PyObject* Screen_mark(PyObject* self, PyObject* arg) {
    ScreenObject* o = SC_OF(self);
    long ph = PyLong_AsLong(arg);
    if (ph == -1 && PyErr_Occurred()) return NULL;
    if (ph < YSCR_PHASE_EVALUATE || ph > YSCR_PHASE_UPLOAD) {
        PyErr_SetString(VsArgumentError, "mark(phase): PHASE_EVALUATE, PHASE_SCRIPT, PHASE_DRAW or PHASE_UPLOAD");
        return NULL;
    }
    if (!sc_ready(o)) return NULL;
    yscr_mark(&o->v->s, (int)ph);
    Py_RETURN_NONE;
}

static PyObject* Screen_records(PyObject* self, PyObject* unused) {
    ScreenObject* o = SC_OF(self);
    PyObject* list;
    (void)unused;
    if (o->busy) { PyErr_SetString(VsBusyError, "ysp_screen: another thread is in a call on this Screen"); return NULL; }
    if (!o->v) return PyList_New(0);
    list = PyList_New(0);
    while (list && o->v->rec_n > 0) {
        yscr_record r;
        PyObject* x;
        yv_records(o->v, &r, 1);
        x = vs_record(&r);
        if (!x || PyList_Append(list, x) < 0) { Py_XDECREF(x); Py_DECREF(list); return NULL; }
        Py_DECREF(x);
    }
    return list;
}

static PyObject* Screen_poll(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "max", NULL };
    ScreenObject* o = SC_OF(self);
    Py_ssize_t max = 0, n = 0;
    PyObject* list;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|n", kwlist, &max)) return NULL;
    if (!sc_ready(o)) return NULL;
    yv_drain(o->v);
    list = PyList_New(0);
    while (list && o->v->ev_n > 0 && (max <= 0 || n < max)) {
        yin_event e;
        PyObject* x;
        yv_events(o->v, &e, 1);
        x = vs_event(&e);
        if (!x || PyList_Append(list, x) < 0) { Py_XDECREF(x); Py_DECREF(list); return NULL; }
        Py_DECREF(x);
        n++;
    }
    return list;
}

static PyObject* sc_window(ScreenObject* o, void (*fn)(void*), const char* text, int on) {
    int rc;
    if (!sc_ready(o)) return NULL;
    o->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    rc = yv_window(o->v, fn, text, on);
    Py_END_ALLOW_THREADS
    o->busy = 0;
    if (rc == YSCR_ERR_NOT_IMPLEMENTED) {
        PyErr_SetString(VsNotImplementedError, "ysp_screen: this screen has no window (sim backend)");
        return NULL;
    }
    if (rc == YSCR_ERR_LOST) { PyErr_Format(VsLostError, "ysp_screen: SDL refused: %s", SDL_GetError()); return NULL; }
    if (rc < 0) return sc_fail(o, rc);
    Py_RETURN_NONE;
}

static PyObject* Screen_title(PyObject* self, PyObject* arg) {
    char t[512];
    if (vs_utf8(arg, t, sizeof t) < 0) return NULL;
    return sc_window(SC_OF(self), yv_w_title, t, 0);
}
static PyObject* Screen_cursor(PyObject* self, PyObject* arg) {
    int on = PyObject_IsTrue(arg);
    if (on < 0) return NULL;
    return sc_window(SC_OF(self), yv_w_cursor, NULL, on);
}
static PyObject* Screen_grab(PyObject* self, PyObject* arg) {
    int on = PyObject_IsTrue(arg);
    if (on < 0) return NULL;
    return sc_window(SC_OF(self), yv_w_grab, NULL, on);
}

static PyObject* Screen_describe(PyObject* self, PyObject* unused) {
    ScreenObject* o = SC_OF(self);
    char line[2048];
    (void)unused;
    if (!sc_ready(o)) return NULL;
    yscr_describe(&o->v->s, line, sizeof line);
    return PyUnicode_FromString(line);
}

static const char* vs_kind_name(int k) {
    return k == YSCR_FIXED_GRID ? "fixed_grid" : k == YSCR_CONTINUOUS ? "continuous" : k == YSCR_CALLBACK ? "callback" : "?";
}

static PyObject* Screen_caps(PyObject* self, PyObject* unused) {
    ScreenObject* o = SC_OF(self);
    yscr_caps c;
    (void)unused;
    if (!sc_ready(o)) return NULL;
    yscr_get_caps(&o->v->s, &c);
    return Py_BuildValue("{s:s,s:s,s:d,s:L,s:N,s:O,s:O,s:O,s:i,s:i,s:O}", "kind", vs_kind_name(c.kind), "backend",
                         c.backend == YSCR_BACKEND_SIM ? "sim" : yv_value_name(yv_backends, c.backend), "period",
                         yv_s(c.period_ns), "period_ns", (long long)c.period_ns, "mode", vs_mode(&c.mode),
                         "native_target", c.native_target ? Py_True : Py_False, "hw_onset",
                         c.hw_onset ? Py_True : Py_False, "vrr_capable", c.vrr_capable ? Py_True : Py_False,
                         "max_in_flight", (int)c.max_in_flight, "worst_tier", (int)c.worst_tier, "raw_keyboard",
                         c.raw_keyboard ? Py_True : Py_False);
}

static PyObject* Screen_settle(PyObject* self, PyObject* unused) {
    ScreenObject* o = SC_OF(self);
    yscr_settle_info i;
    (void)unused;
    if (!sc_ready(o)) return NULL;
    yscr_settle_check(&o->v->s, &i);
    return Py_BuildValue("{s:I,s:I,s:I,s:I,s:d,s:i,s:i,s:i,s:I,s:d,s:d,s:L,s:L,s:d,s:s}", "result", i.result, "mode",
                         i.mode, "source", i.source, "condition", i.condition, "time", yv_s(i.ns), "flips", i.flips,
                         "run", i.run, "need", i.need, "foreground", i.foreground, "min", yv_s(i.min_ns), "max",
                         yv_s(i.max_ns), "spread_sd_ns", (long long)i.spread_sd_ns, "spread_mean_ns",
                         (long long)i.spread_mean_ns, "hook", yv_s(i.hook_ns), "message", i.message);
}

static PyObject* Screen_sync(PyObject* self, PyObject* unused) {
    ScreenObject* o = SC_OF(self);
    yscr_sync_info i;
    (void)unused;
    if (!sc_ready(o)) return NULL;
    yscr_sync_check(&o->v->s, &i);
    return Py_BuildValue("{s:O,s:I,s:L,s:i,s:i,s:i,s:I,s:s}", "untimed", i.untimed ? Py_True : Py_False, "rule",
                         i.rule, "fired_index", (long long)i.fired_index, "window", i.window, "evidence", i.evidence,
                         "peak", i.peak, "observed", i.observed, "message", i.message);
}

static PyObject* Screen_set_patch(PyObject* self, PyObject* arg) {
    ScreenObject* o = SC_OF(self);
    double x = PyFloat_AsDouble(arg);
    if (x == -1.0 && PyErr_Occurred()) return NULL;
    if (!(x == x)) { PyErr_SetString(VsArgumentError, "set_patch(v): 0..1, not NaN"); return NULL; }
    if (!sc_ready(o)) return NULL;
    yscr_set_patch(&o->v->s, (float)x);
    Py_RETURN_NONE;
}

static PyObject* Screen_get_open(PyObject* self, void* c) {
    ScreenObject* o = SC_OF(self);
    (void)c;
    return PyBool_FromLong(o->v && o->v->open && yscr_is_open(&o->v->s));
}
static PyObject* Screen_get_error(PyObject* self, void* c) {
    (void)c;
    return PyUnicode_FromString(SC_OF(self)->v ? yscr_error(&SC_OF(self)->v->s) : "");
}
static PyObject* Screen_get_records_lost(PyObject* self, void* c) {
    (void)c;
    return PyLong_FromUnsignedLongLong(SC_OF(self)->v ? SC_OF(self)->v->rec_lost : 0);
}
static PyObject* Screen_get_events_lost(PyObject* self, void* c) {
    (void)c;
    return PyLong_FromUnsignedLongLong(SC_OF(self)->v ? SC_OF(self)->v->ev_lost : 0);
}
static PyObject* Screen_get_size(PyObject* self, void* c) {
    ScreenObject* o = SC_OF(self);
    yscr_caps cp;
    (void)c;
    if (!sc_ready(o)) return NULL;
    yscr_get_caps(&o->v->s, &cp);
    return Py_BuildValue("(ii)", (int)cp.mode.w, (int)cp.mode.h);
}

static PyMethodDef Screen_methods[] = {
    { "close", Screen_close, METH_NOARGS, "close(): lets the last flip complete, then closes the window (and a Gfx on it)." },
    { "__enter__", Screen_enter, METH_NOARGS, NULL },
    { "__exit__", Screen_exit, METH_VARARGS, NULL },
    { "begin", Screen_begin, METH_NOARGS,
      "begin() -> Frame: waits until the swap path takes a frame (GIL released) and predicts its onset. Raises "
      "Abort once for each abort (Shift+Esc, the close button, Alt+F4, request_abort(), the panic's release)." },
    { "flip", Screen_flip, METH_NOARGS, "flip(): presents the frame at the onset begin() predicted (GIL released)." },
    { "flip_at", Screen_flip_at, METH_O,
      "flip_at(t): presents the frame for the vblank t (seconds, ysp_rt clock) snaps to; never before the first it can make." },
    { "wait_flip", Screen_wait_flip, METH_NOARGS,
      "wait_flip() -> Record: blocks until every flip so far has its record (as Psychtoolbox's Flip); GIL released." },
    { "mark", Screen_mark, METH_O, "mark(phase): the time since begin() or the last mark goes to phase (PHASE_*)." },
    { "records", Screen_records, METH_NOARGS,
      "records() -> list[Record]: every flip record completed since the last call, oldest first (records_lost counts overflow)." },
    { "poll", (PyCFunction)(void (*)(void))Screen_poll, METH_VARARGS | METH_KEYWORDS,
      "poll(max=0) -> list[Event]: the input events since the last poll, oldest first, each with its own time." },
    { "title", Screen_title, METH_O, "title(text): the window's title (through the window thread)." },
    { "cursor", Screen_cursor, METH_O, "cursor(show): shows or hides the cursor (through the window thread)." },
    { "grab", Screen_grab, METH_O, "grab(on): confines the mouse to the window (through the window thread)." },
    { "describe", Screen_describe, METH_NOARGS, "describe() -> str: the log line (backend, adapter, ANGLE, mode, path, depth, warnings)." },
    { "caps", Screen_caps, METH_NOARGS, "caps() -> dict: the kind, backend, period, mode obtained, worst tier." },
    { "settle", Screen_settle, METH_NOARGS, "settle() -> dict: what open's settling did (yscr_settle_check)." },
    { "sync", Screen_sync, METH_NOARGS, "sync() -> dict: the sync guard's state (yscr_sync_check)." },
    { "set_patch", Screen_set_patch, METH_O, "set_patch(v): the photodiode patch's gray level, 0..1, for this and later frames." },
    { NULL, NULL, 0, NULL }
};

static PyGetSetDef Screen_getset[] = {
    { "open", Screen_get_open, NULL, "True while open", NULL },
    { "error", Screen_get_error, NULL, "the message of the last call that failed", NULL },
    { "records_lost", Screen_get_records_lost, NULL, "records overwritten before records() read them", NULL },
    { "events_lost", Screen_get_events_lost, NULL, "input events overwritten before poll() read them", NULL },
    { "size", Screen_get_size, NULL, "(w, h): the back buffer in pixels", NULL },
    { NULL, NULL, NULL, NULL, NULL }
};

static PyType_Slot Screen_slots[] = {
    { Py_tp_doc, "Screen(**options): a stimulus display (ysp/screen.h) with flip at a time and a record of every flip. "
                 "Keyword options only; options() lists them (times in seconds). Opening settles the display: about "
                 "0.5 to 0.8 s fullscreen, 2.5 s in a window. Shift+Esc aborts (begin() raises Abort)." },
    { Py_tp_new, PyType_GenericNew },
    { Py_tp_init, Screen_init },
    { Py_tp_dealloc, Screen_dealloc },
    { Py_tp_methods, Screen_methods },
    { Py_tp_getset, Screen_getset },
    { 0, NULL }
};
static PyType_Spec Screen_spec = { "ysp.screen.Screen", sizeof(ScreenObject), 0, Py_TPFLAGS_DEFAULT, Screen_slots };

/* --- Gfx, Stim, DrawList, Group, Texture: ysp.gfx (vis_gfx.c) ------------------------------ */

#include "vis_gfx.c"
#include "vis_text.c"   /* Font, Text */

/* --- ysp.video: Movie, SeqWriter, probe(), make_index() ---------------------------- */

#include "vis_video.c"

/* --- module functions ----------------------------------------------------------- */

static PyObject* m_displays(PyObject* m, PyObject* unused) {
    yscr_display_info d[16];
    int n, i;
    PyObject* list;
    (void)m;
    (void)unused;
    Py_BEGIN_ALLOW_THREADS
    n = yscr_displays(d, 16);
    Py_END_ALLOW_THREADS
    if (n < 0) { PyErr_SetString(vs_exc(n), yscr_strerror(n)); return NULL; }
    if (n > 16) n = 16;
    list = PyList_New(0);
    for (i = 0; list && i < n; i++) {
        PyObject* v[8];
        PyObject* x;
        v[0] = PyLong_FromUnsignedLong(d[i].id);
        v[1] = PyUnicode_FromString(d[i].name);
        v[2] = PyLong_FromLong(d[i].x);
        v[3] = PyLong_FromLong(d[i].y);
        v[4] = PyLong_FromLong(d[i].w);
        v[5] = PyLong_FromLong(d[i].h);
        v[6] = PyBool_FromLong(d[i].primary);
        v[7] = vs_mode(&d[i].desktop);
        x = vs_seq(DisplayType, v, 8);
        if (!x || PyList_Append(list, x) < 0) { Py_XDECREF(x); Py_DECREF(list); return NULL; }
        Py_DECREF(x);
    }
    return list;
}

static PyObject* m_modes(PyObject* m, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "display", NULL };
    unsigned int disp = 0;
    yscr_mode md[256];
    int n, i;
    PyObject* list;
    (void)m;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|I", kwlist, &disp)) return NULL;
    Py_BEGIN_ALLOW_THREADS
    n = yscr_modes(disp, md, 256);
    Py_END_ALLOW_THREADS
    if (n < 0) { PyErr_SetString(vs_exc(n), yscr_strerror(n)); return NULL; }
    if (n > 256) n = 256;
    list = PyList_New(0);
    for (i = 0; list && i < n; i++) {
        PyObject* x = vs_mode(&md[i]);
        if (!x || PyList_Append(list, x) < 0) { Py_XDECREF(x); Py_DECREF(list); return NULL; }
        Py_DECREF(x);
    }
    return list;
}

static PyObject* m_mode_multiple(PyObject* m, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "num", "den", "w", "h", "tol_ppm", "display", NULL };
    int num, den = 1, w = 0, h = 0, k;
    double tol = 0.0, err = 0.0;
    unsigned int disp = 0;
    yscr_mode out;
    (void)m;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "i|iiidI", kwlist, &num, &den, &w, &h, &tol, &disp)) return NULL;
    memset(&out, 0, sizeof out);
    Py_BEGIN_ALLOW_THREADS
    k = yscr_mode_multiple(disp, num, den, w, h, tol, &out, &err);
    Py_END_ALLOW_THREADS
    if (k < 0 && k != YSCR_ERR_REFUSED) { PyErr_SetString(vs_exc(k), yscr_strerror(k)); return NULL; }
    return Py_BuildValue("(iNd)", k < 0 ? 0 : k, vs_mode(&out), err);
}

static PyObject* m_request_abort(PyObject* m, PyObject* unused) { (void)m; (void)unused; yscr_request_abort(); Py_RETURN_NONE; }

static PyObject* m_input_stats(PyObject* m, PyObject* unused) {
    yscr_input_stats s;
    (void)m;
    (void)unused;
    yscr_get_input_stats(&s);
    return Py_BuildValue("{s:I,s:I,s:I,s:I,s:I,s:I,s:I}", "stored", s.stored, "overwritten", s.overwritten, "refused",
                         s.refused, "pending", s.pending, "lost", s.lost, "key_doubles", s.key_doubles,
                         "key_double_ups", s.key_double_ups);
}

static PyObject* m_now(PyObject* m, PyObject* unused) { (void)m; (void)unused; return PyFloat_FromDouble(yv_s(yrt_now_ns())); }
static PyObject* m_now_ns(PyObject* m, PyObject* unused) { (void)m; (void)unused; return PyLong_FromLongLong(yrt_now_ns()); }

static PyObject* m_options(PyObject* m, PyObject* unused) {
    PyObject* d = PyDict_New();
    int i;
    (void)m;
    (void)unused;
    for (i = 0; d && yv_opts[i].name; i++) {
        PyObject* s = PyUnicode_FromString(yv_opts[i].doc);
        if (!s || PyDict_SetItemString(d, yv_opts[i].name, s) < 0) { Py_XDECREF(s); Py_DECREF(d); return NULL; }
        Py_DECREF(s);
    }
    return d;
}

static PyObject* m_close_all(PyObject* m, PyObject* unused) {
    (void)m;
    (void)unused;
    while (vs_open_list) {
        ScreenObject* o = vs_open_list;
        if (o->busy) { vs_unlist(o); continue; }   /* another thread is in it: leave it to the process's end */
        sc_shut(o);
    }
    Py_RETURN_NONE;
}

static PyObject* m_shared(PyObject* m, PyObject* unused) {
    (void)m;
    (void)unused;
    return Py_BuildValue("{s:s,s:s,s:s,s:s,s:I,s:s,s:s,s:s,s:s}", "audio", ysp_audio_api_ptr->head.version, "audio_built",
                         YAU_VERSION_STRING, "timeline", ysp_timeline_api_ptr->head.version, "timeline_built",
                         YTL_VERSION_STRING, "abi", (unsigned)YSP_SHARE_ABI, "screen", yscr_version(), "gfx",
                         ygfx_version(), "video", yvid_version(), "module_dir", yv_module_dir());
}

static PyMethodDef module_methods[] = {
    { "displays", m_displays, METH_NOARGS, "displays() -> list[Display]: the connected displays (call at setup: about 0.1 s)." },
    { "modes", (PyCFunction)(void (*)(void))m_modes, METH_VARARGS | METH_KEYWORDS,
      "modes(display=0) -> list[Mode]: the fullscreen modes of a display, largest and fastest first." },
    { "mode_multiple", (PyCFunction)(void (*)(void))m_mode_multiple, METH_VARARGS | METH_KEYWORDS,
      "mode_multiple(num, den=1, w=0, h=0, tol_ppm=0, display=0) -> (k, Mode, err_ppm): the mode whose refresh is k "
      "times num/den Hz; k 0 when none qualifies (Mode is then the nearest)." },
    { "request_abort", m_request_abort, METH_NOARGS, "request_abort(): the next begin() of every open screen raises Abort." },
    { "input_stats", m_input_stats, METH_NOARGS, "input_stats() -> dict: the input bridge's counters." },
    { "now", m_now, METH_NOARGS, "now() -> seconds on the ysp_rt clock, the clock of every time here." },
    { "now_ns", m_now_ns, METH_NOARGS, "now_ns() -> int: the same, exact ns." },
    { "options", m_options, METH_NOARGS, "options() -> dict: Screen()'s keyword options and what each does." },
    { "_close_all", m_close_all, METH_NOARGS, "Closes every open Screen (atexit)." },
    { "_shared", m_shared, METH_NOARGS, "The versions of the shared tables and of the headers compiled here." },
    VV_MODULE_METHODS
    { NULL, NULL, 0, NULL }
};

static struct PyModuleDef vis_module = {
    PyModuleDef_HEAD_INIT, "ysp._vis",
    "ysp/screen.h, ysp/gfx.h and ysp/video.h in one extension; import ysp.screen, ysp.gfx and ysp.video.", -1,
    module_methods, NULL, NULL, NULL, NULL,
};

static PyObject* vs_add_exc(PyObject* m, const char* qual, const char* attr, PyObject* base) {
    PyObject* e = PyErr_NewException(qual, base, NULL);
    if (!e) return NULL;
    Py_INCREF(e);
    if (PyModule_AddObject(m, attr, e) < 0) { Py_DECREF(e); Py_DECREF(e); return NULL; }
    return e;
}

static PyObject* vs_add_exc2(PyObject* m, const char* qual, const char* attr, PyObject* b1, PyObject* b2) {
    PyObject *t = PyTuple_Pack(2, b1, b2), *e;
    if (!t) return NULL;
    e = vs_add_exc(m, qual, attr, t);
    Py_DECREF(t);
    return e;
}

static PyObject* vs_add_type(PyObject* m, PyType_Spec* spec, const char* name) {
    PyObject* t = PyType_FromSpec(spec);
    if (!t) return NULL;
    Py_INCREF(t);
    if (PyModule_AddObject(m, name, t) < 0) { Py_DECREF(t); Py_DECREF(t); return NULL; }
    return t;
}

static PyTypeObject* vs_add_seq(PyObject* m, PyStructSequence_Desc* d, const char* name) {
    PyTypeObject* t = PyStructSequence_NewType(d);
    if (!t) return NULL;
    Py_INCREF(t);
    if (PyModule_AddObject(m, name, (PyObject*)t) < 0) { Py_DECREF(t); Py_DECREF(t); return NULL; }
    return t;
}

PyMODINIT_FUNC PyInit__vis(void) {
    PyObject* m;
    int i;
    static const struct { const char* name; long v; } consts[] = {
        { "PATH_UNKNOWN", YSCR_PATH_UNKNOWN }, { "PATH_COMPOSED", YSCR_PATH_COMPOSED },
        { "PATH_OVERLAY", YSCR_PATH_OVERLAY }, { "PATH_INDEPENDENT", YSCR_PATH_INDEPENDENT },
        { "PATH_SIMULATED", YSCR_PATH_SIMULATED },
        { "TIER_UNKNOWN", YSCR_TIER_UNKNOWN }, { "TIER_1", YSCR_TIER_1 }, { "TIER_2", YSCR_TIER_2 },
        { "TIER_3", YSCR_TIER_3 }, { "TIER_SIM", YSCR_TIER_SIM },
        { "FLIP_PENDING", YSCR_FLIP_PENDING }, { "FLIP_ESTIMATED", YSCR_FLIP_ESTIMATED },
        { "FLIP_LATE_TARGET", YSCR_FLIP_LATE_TARGET }, { "FLIP_OCCLUDED", YSCR_FLIP_OCCLUDED },
        { "FLIP_GRID_UNSTABLE", YSCR_FLIP_GRID_UNSTABLE }, { "FLIP_EARLY", YSCR_FLIP_EARLY },
        { "FLIP_SKIPPED", YSCR_FLIP_SKIPPED }, { "FLIP_CANCELED", YSCR_FLIP_CANCELED },
        { "FLIP_ONSET_PLANNED", YSCR_FLIP_ONSET_PLANNED }, { "FLIP_BELOW_TIER", YSCR_FLIP_BELOW_TIER },
        { "FLIP_CODE_AT_RISK", YSCR_FLIP_CODE_AT_RISK }, { "FLIP_TEXT_INPUT", YSCR_FLIP_TEXT_INPUT },
        { "FLIP_UNSYNCED", YSCR_FLIP_UNSYNCED },
        { "ABORT_KEY", YSCR_ABORT_KEY }, { "ABORT_CLOSE", YSCR_ABORT_CLOSE }, { "ABORT_ALT_F4", YSCR_ABORT_ALT_F4 },
        { "ABORT_QUIT", YSCR_ABORT_QUIT }, { "ABORT_REQUEST", YSCR_ABORT_REQUEST }, { "ABORT_PANIC", YSCR_ABORT_PANIC },
        { "PHASE_EVALUATE", YSCR_PHASE_EVALUATE }, { "PHASE_SCRIPT", YSCR_PHASE_SCRIPT },
        { "PHASE_DRAW", YSCR_PHASE_DRAW }, { "PHASE_UPLOAD", YSCR_PHASE_UPLOAD }, { "PHASE_SWAP", YSCR_PHASE_SWAP },
        { "PHASE_GPU", YSCR_PHASE_GPU },
        { "SETTLED", YSCR_SETTLED }, { "SETTLE_FAILED", YSCR_SETTLE_FAILED }, { "SETTLE_SKIPPED", YSCR_SETTLE_SKIPPED },
        { "SETTLE_ABORTED", YSCR_SETTLE_ABORTED },
        { "PANIC_EXIT_CODE", YSCR_PANIC_EXIT_CODE },
        /* the input event (ysp/input.h), named as ysp.device and ysp.response name them */
        { "KIND_KEYBOARD", YIN_KIND_KEYBOARD }, { "KIND_MOUSE", YIN_KIND_MOUSE }, { "KIND_TOUCH", YIN_KIND_TOUCH },
        { "KIND_PEN", YIN_KIND_PEN }, { "KIND_GAMEPAD", YIN_KIND_GAMEPAD },
        { "PRESS", YIN_PRESS }, { "RELEASE", YIN_RELEASE }, { "SAMPLE", YIN_SAMPLE }, { "PROXIMITY", YIN_PROXIMITY },
        { "REPEAT", YIN_REPEAT }, { "SYNTHETIC", YIN_SYNTHETIC },
    };
    /* The tables first: a missing or mismatched ysp.audio or ysp.timeline
     * refuses the import with the reason (VERSIONS in ysp_share.h). */
    if (ysp_audio_api_import_py() < 0) return NULL;
    if (ysp_timeline_api_import_py() < 0) return NULL;
    m = PyModule_Create(&vis_module);
    if (!m) return NULL;
    if (!(ScreenType = vs_add_type(m, &Screen_spec, "Screen"))) goto fail;
    if (vg_init(m) < 0) goto fail;   /* ysp.gfx: Gfx, Stim, DrawList, Group, Texture */
    if (!(FrameType = vs_add_seq(m, &vs_frame_desc, "Frame"))) goto fail;
    if (!(RecordType = vs_add_seq(m, &vs_record_desc, "Record"))) goto fail;
    if (!(EventType = vs_add_seq(m, &vs_event_desc, "Event"))) goto fail;
    if (!(ModeType = vs_add_seq(m, &vs_mode_desc, "Mode"))) goto fail;
    if (!(DisplayType = vs_add_seq(m, &vs_display_desc, "Display"))) goto fail;
    if (!(VsError = vs_add_exc(m, "ysp.screen.Error", "Error", NULL))) goto fail;
    if (!(VsArgumentError = vs_add_exc2(m, "ysp.screen.ArgumentError", "ArgumentError", VsError, PyExc_ValueError))) goto fail;
    if (!(VsClosedError = vs_add_exc(m, "ysp.screen.ClosedError", "ClosedError", VsError))) goto fail;
    if (!(VsOrderError = vs_add_exc(m, "ysp.screen.OrderError", "OrderError", VsError))) goto fail;
    if (!(VsTimeoutError = vs_add_exc2(m, "ysp.screen.TimeoutError", "TimeoutError", VsError, PyExc_TimeoutError))) goto fail;
    if (!(VsLostError = vs_add_exc(m, "ysp.screen.LostError", "LostError", VsError))) goto fail;
    if (!(VsRefusedError = vs_add_exc(m, "ysp.screen.RefusedError", "RefusedError", VsError))) goto fail;
    if (!(VsNotImplementedError = vs_add_exc2(m, "ysp.screen.NotImplementedError", "NotImplementedError", VsError,
                                              PyExc_NotImplementedError))) goto fail;
    if (!(VsBusyError = vs_add_exc(m, "ysp.screen.BusyError", "BusyError", VsError))) goto fail;
    /* Not an Error: the operator asked to stop. */
    if (!(VsAbort = vs_add_exc(m, "ysp.screen.Abort", "Abort", NULL))) goto fail;
    if (vv_init(m) < 0) goto fail;
    for (i = 0; i < (int)(sizeof consts / sizeof consts[0]); i++)
        if (PyModule_AddIntConstant(m, consts[i].name, consts[i].v) < 0) goto fail;
    if (PyModule_AddStringConstant(m, "__version__", YV_VERSION) < 0) goto fail;
    if (PyModule_AddStringConstant(m, "SCREEN_VERSION", YSCR_VERSION_STRING) < 0) goto fail;
    if (PyModule_AddStringConstant(m, "GFX_VERSION", YGFX_VERSION_STRING) < 0) goto fail;
    if (PyModule_AddStringConstant(m, "VIDEO_VERSION", YVID_VERSION_STRING) < 0) goto fail;
    if (PyModule_AddStringConstant(m, "ANGLE_DIR", yv_module_dir()) < 0) goto fail;
    return m;
fail:
    Py_DECREF(m);
    return NULL;
}
