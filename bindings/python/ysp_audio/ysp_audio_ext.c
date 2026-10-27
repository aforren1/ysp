/* ysp_audio_ext.c - CPython extension wrapping ysp/audio.h (module ysp.audio)
 *
 * Sound at a time on the ysp_rt clock for a PsychoPy experiment, and the one
 * audio implementation that ysp-vis (a movie's soundtrack) shares:
 *
 *     import ysp.audio as yau
 *     a = yau.Audio()                               # the OS default output
 *     beep = a.tone(1000, 0.1, 0.03, ramp=0.005)
 *     sid = a.play_at(beep, t)                      # t: seconds, ysp_rt clock
 *     r = a.wait(sid)                               # GIL released
 *     print(r.onset, r.residual_ns, r.tier)
 *
 * TIMES: seconds (double), converted once to the nearest ns; a negative or
 * NaN time is refused before the call. Records give the exact ns beside the
 * seconds. Frame counts (offset, frames, samples, capture frames) stay
 * integers.
 *
 * THREADS: miniaudio's device thread never takes the GIL. open, close,
 * wait, the stream and WAV closes, a WAV feed, a capture open and the device
 * list release it. The rest are short and keep it.
 *
 * MEMORY: buffers live in the handle's arena (yau_tone, yau_alloc,
 * yau_wav_load); a Buffer from before arena_reset() is refused. A stream's
 * ring is in the arena too; its struct, and a WAV reader's, is freed when
 * the Audio closes, because the header may keep a stream it could not
 * detach until then (STREAMS, CLOSE). play_at() allocates nothing.
 *
 * SHARING: the module's _C_API capsule holds a ysp_audio_api table
 * (bindings/share/ysp_audio_api.h); acquire() pins an Audio, and close()
 * refuses while it is pinned (BusyError).
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x03080000
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"
#define YSP_AUDIO_API_OWNER
#include "ysp_audio_api.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The distribution's version is the header's (tests check pyproject.toml). */
#define AU_VERSION YAU_VERSION_STRING
#define AU_S_MAX 4611686018.0

static PyObject *AuError, *AuArgumentError, *AuClosedError, *AuFullError, *AuFormatError, *AuNotFoundError,
    *AuLostError, *AuBusyError, *AuTimeoutError, *AuIOError;
static PyObject *AudioType, *BufferType, *StreamType, *WavType, *CaptureType, *OnsetType;
static ysp_audio_api g_api;

/* --- times --------------------------------------------------------------------- */

/* Seconds to the nearest ns: fma() gives the product's rounding error, so
 * the result is the nearest ns to the real product (as ysp.timeline). */
static int au_s_to_ns(double s, const char* what, int64_t* out) {
    volatile double p;
    double e, n, r;
    if (!(s >= 0.0 && s < AU_S_MAX)) {
        PyErr_Format(AuArgumentError, "%s: seconds, >= 0 and below 2^62 ns (a negative or NaN time is refused)", what);
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

static int au_obj_ns(PyObject* o, const char* what, int64_t* out) {
    double s = PyFloat_AsDouble(o);
    if (s == -1.0 && PyErr_Occurred()) return -1;
    return au_s_to_ns(s, what, out);
}

static double au_s(int64_t ns) { return (double)ns / 1e9; }

/* An optional duration in seconds as frames at `rate`, rounded. */
static int au_s_to_frames(double s, uint32_t rate, const char* what, int64_t* out) {
    if (!(s >= 0.0 && s < 1e9)) { PyErr_Format(AuArgumentError, "%s: seconds, >= 0", what); return -1; }
    *out = (int64_t)floor(s * (double)rate + 0.5);
    return 0;
}

/* --- errors -------------------------------------------------------------------- */

static PyObject* au_exc(int code) {
    switch (code) {
    case YAU_ERR_ARG: return AuArgumentError;
    case YAU_ERR_CLOSED: return AuClosedError;
    case YAU_ERR_FULL: return AuFullError;
    case YAU_ERR_FORMAT: return AuFormatError;
    case YAU_ERR_NOT_FOUND: return AuNotFoundError;
    case YAU_ERR_LOST: return AuLostError;
    case YAU_ERR_BUSY: return AuBusyError;
    case YAU_ERR_TIMEOUT: return AuTimeoutError;
    case YAU_ERR_IO: return AuIOError;
    default: return AuError;
    }
}

/* --- objects ------------------------------------------------------------------- */

struct AudioObject;

/* The handle first, so the pointer acquire() hands out leads back here. */
typedef struct au_box {
    yau_audio au;
    struct AudioObject* owner;
} au_box;

/* A stream or a WAV reader: freed when the Audio closes (MEMORY). */
typedef struct au_child {
    struct au_child* next;
    int       kind;         /* 1 stream, 2 WAV */
    int       open;
    void*     obj;          /* yau_stream* or yau_wav* */
    PyObject* keep;         /* a WAV in memory: its bytes */
    PyObject* path;         /* a WAV path: its bytes */
} au_child;

typedef struct AudioObject {
    PyObject_HEAD
    au_box*   box;
    int       open;
    int       pins;         /* acquire() calls not yet released (SHARING) */
    int       waiting;      /* threads in wait() */
    uint32_t  gen;          /* arena generation */
    au_child* children;
    PyObject* device;       /* desc.device's bytes: the handle keeps the pointer */
} AudioObject;

typedef struct BufferObject {
    PyObject_HEAD
    PyObject* owner;
    yau_buf   buf;
    uint32_t  gen;
} BufferObject;

typedef struct StreamObject {   /* Stream and Wav */
    PyObject_HEAD
    PyObject* owner;
    au_child* c;
} StreamObject;

typedef struct CaptureObject {
    PyObject_HEAD
    yau_capture* c;
    int          open;
    PyObject*    device;
} CaptureObject;

#define AU_OF(o) ((AudioObject*)(o))

static int au_ready(AudioObject* o) {
    if (!o || !o->open || !o->box) { PyErr_SetString(AuClosedError, "ysp_audio: the device is closed"); return 0; }
    /* The handle has one frame thread (MEMORY AND THREADS); wait() runs
     * without the GIL, so another thread's call is refused meanwhile. */
    if (o->waiting) { PyErr_SetString(AuBusyError, "ysp_audio: another thread is in wait() on this Audio"); return 0; }
    return 1;
}

static PyObject* au_fail(AudioObject* o, int code) {
    const char* msg = o && o->box ? yau_error(&o->box->au) : "";
    PyErr_SetString(au_exc(code), msg && msg[0] ? msg : yau_strerror(code));
    return NULL;
}

/* --- names --------------------------------------------------------------------- */

static int au_utf8(PyObject* o, char* buf, size_t cap) {
    PyObject* b = PyUnicode_AsUTF8String(o);
    if (!b) return -1;
    PyOS_snprintf(buf, cap, "%s", PyBytes_AsString(b));
    Py_DECREF(b);
    return 0;
}

typedef struct au_name { const char* name; int value; } au_name;

static const au_name au_backends[] = {
    { "auto", YAU_BACKEND_AUTO }, { "wasapi", YAU_BACKEND_WASAPI }, { "coreaudio", YAU_BACKEND_COREAUDIO },
    { "alsa", YAU_BACKEND_ALSA }, { "pulse", YAU_BACKEND_PULSE }, { "web", YAU_BACKEND_WEB },
    { "null", YAU_BACKEND_NULL }, { NULL, 0 },
};
static const au_name au_samples[] = { { "f32", YAU_F32 }, { "s16", YAU_S16 }, { "s24", YAU_S24 }, { NULL, 0 } };

static int au_lookup(PyObject* o, const au_name* t, const char* what, int* out) {
    char s[32], *p;
    int i;
    if (!o || o == Py_None) return 0;
    if (!PyUnicode_Check(o)) { PyErr_Format(PyExc_TypeError, "%s: a name", what); return -1; }
    if (au_utf8(o, s, sizeof s) < 0) return -1;
    for (p = s; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    for (i = 0; t[i].name; i++) if (strcmp(s, t[i].name) == 0) { *out = t[i].value; return 0; }
    PyErr_Format(AuArgumentError, "%s: unknown name '%s'", what, s);
    return -1;
}

static const char* au_backend_name(int b) {
    int i;
    for (i = 0; au_backends[i].name; i++) if (au_backends[i].value == b) return au_backends[i].name;
    return b == YAU_BACKEND_CUSTOM ? "custom" : "?";
}

/* A path as bytes in the file system encoding (str, bytes or os.PathLike). */
static PyObject* au_path(PyObject* o) {
    PyObject* b = NULL;
    if (!PyUnicode_FSConverter(o, &b)) return NULL;
    return b;
}

/* --- arrays in and out --------------------------------------------------------- */

static int au_fmt_is(PyObject* f, char c) {
    char s[8];
    if (!f || !PyUnicode_Check(f) || au_utf8(f, s, sizeof s) < 0) { PyErr_Clear(); return 0; }
    if (s[0] == c && !s[1]) return 1;
    return (s[0] == '@' || s[0] == '=' || s[0] == '<') && s[1] == c && !s[2];
}

/* Samples from a buffer of float32 or float64 (NumPy, array.array, a
 * memoryview) or a sequence of numbers, as PyMem floats. A 2-D buffer gives
 * its channel count; else `*ch` (0 = 1) divides the count. The Limited API
 * before 3.11 has no PyObject_GetBuffer, so this copies through bytes. */
static float* au_floats(PyObject* obj, int64_t* frames, int* ch) {
    PyObject* mv = PyMemoryView_FromObject(obj);
    float* out = NULL;
    Py_ssize_t n = 0, i;
    int c = *ch;
    if (mv) {
        PyObject *f = PyObject_GetAttrString(mv, "format"), *nd = PyObject_GetAttrString(mv, "ndim");
        int isf = au_fmt_is(f, 'f'), isd = au_fmt_is(f, 'd');
        long ndim = nd ? PyLong_AsLong(nd) : -1;
        Py_XDECREF(f);
        Py_XDECREF(nd);
        PyErr_Clear();
        if (ndim == 2) {
            PyObject* sh = PyObject_GetAttrString(mv, "shape");
            PyObject* s1 = sh ? PyTuple_GetItem(sh, 1) : NULL;
            long w = s1 ? PyLong_AsLong(s1) : -1;
            Py_XDECREF(sh);
            if (w < 1) { Py_DECREF(mv); PyErr_SetString(AuArgumentError, "data: a 2-D array of frames x channels"); return NULL; }
            if (c && c != (int)w) { Py_DECREF(mv); PyErr_Format(AuArgumentError, "data has %ld channels, channels=%d", w, c); return NULL; }
            c = (int)w;
        } else if (ndim > 2) {
            Py_DECREF(mv);
            PyErr_SetString(AuArgumentError, "data: 1-D samples or 2-D frames x channels");
            return NULL;
        }
        if (isf || isd) {
            PyObject* b = PyBytes_FromObject(mv);
            const char* p;
            Py_DECREF(mv);
            if (!b) return NULL;
            p = PyBytes_AsString(b);
            n = PyBytes_Size(b) / (isf ? 4 : 8);
            out = (float*)PyMem_Malloc((size_t)(n ? n : 1) * sizeof(float));
            if (!out) { Py_DECREF(b); PyErr_NoMemory(); return NULL; }
            if (isf) memcpy(out, p, (size_t)n * sizeof(float));
            else for (i = 0; i < n; i++) { double d; memcpy(&d, p + 8 * i, 8); out[i] = (float)d; }
            Py_DECREF(b);
        } else {
            Py_DECREF(mv);
            mv = NULL;
            if (!PyBytes_Check(obj) && !PyByteArray_Check(obj)) goto seq;
            PyErr_SetString(PyExc_TypeError, "data: float32 or float64 samples (bytes are not samples)");
            return NULL;
        }
    } else {
    seq:
        PyErr_Clear();
        n = PySequence_Size(obj);
        if (n < 0) { PyErr_SetString(PyExc_TypeError, "data: a buffer of float32 or float64, or a sequence of numbers"); return NULL; }
        out = (float*)PyMem_Malloc((size_t)(n ? n : 1) * sizeof(float));
        if (!out) { PyErr_NoMemory(); return NULL; }
        for (i = 0; i < n; i++) {
            PyObject* v = PySequence_GetItem(obj, i);
            double d = v ? PyFloat_AsDouble(v) : -1.0;
            Py_XDECREF(v);
            if (d == -1.0 && PyErr_Occurred()) { PyMem_Free(out); return NULL; }
            out[i] = (float)d;
        }
    }
    if (c <= 0) c = 1;
    if (n % c) { PyMem_Free(out); PyErr_Format(AuArgumentError, "data: %zd samples are not whole frames of %d channels", n, c); return NULL; }
    for (i = 0; i < n; i++)
        if (!isfinite(out[i])) { PyMem_Free(out); PyErr_Format(AuArgumentError, "data: sample %zd is not finite", i); return NULL; }
    *frames = n / c;
    *ch = c;
    return out;
}

/* A float32 memoryview of frames x channels over a fresh bytes copy. */
static PyObject* au_view(const float* data, int64_t frames, int ch) {
    PyObject *b = PyBytes_FromStringAndSize((const char*)data, (Py_ssize_t)(frames * ch) * 4), *mv, *shape, *r;
    if (!b) return NULL;
    mv = PyMemoryView_FromObject(b);
    Py_DECREF(b);
    if (!mv) return NULL;
    shape = Py_BuildValue("(Li)", (long long)frames, ch);
    r = shape ? PyObject_CallMethod(mv, "cast", "sO", "f", shape) : NULL;
    Py_XDECREF(shape);
    Py_DECREF(mv);
    return r;
}

/* A channel mask: an int, or a sequence of 0-based channel numbers. */
static int au_mask(PyObject* o, uint64_t* out) {
    Py_ssize_t n, i;
    *out = 0;
    if (!o || o == Py_None) return 0;
    if (PyLong_Check(o)) {
        *out = (uint64_t)PyLong_AsUnsignedLongLong(o);
        return PyErr_Occurred() ? -1 : 0;
    }
    n = PySequence_Size(o);
    if (n < 0) { PyErr_SetString(PyExc_TypeError, "channels: a mask, or a sequence of channel numbers from 0"); return -1; }
    for (i = 0; i < n; i++) {
        PyObject* v = PySequence_GetItem(o, i);
        long c = v ? PyLong_AsLong(v) : -1;
        Py_XDECREF(v);
        if (c == -1 && PyErr_Occurred()) return -1;
        if (c < 0 || c >= 64) { PyErr_SetString(AuArgumentError, "channels: numbers 0 to 63"); return -1; }
        *out |= (uint64_t)1 << c;
    }
    return 0;
}

/* --- the record ---------------------------------------------------------------- */

static PyStructSequence_Field au_onset_fields[] = {
    { "id", "the play's id" },
    { "target", "the time asked for, seconds (0: as soon as it could)" },
    { "target_ns", "the same, exact ns" },
    { "onset", "the device-clock fit's time of start_frame, seconds; a model value, never observed" },
    { "onset_ns", "the same, exact ns" },
    { "residual", "onset - target, seconds" },
    { "residual_ns", "the same, exact ns" },
    { "start_frame", "stream frame of the first sample; -1: not started" },
    { "end_frame", "stream frame after the last; 0: playing" },
    { "device_pos", "the position report that confirmed it, or -1" },
    { "rendered_at", "when the callback rendered start_frame, seconds" },
    { "rendered_at_ns", "the same, exact ns" },
    { "buffer_id", "the buffer's or stream's id" },
    { "flags", "PENDING, LATE, CANCELED, STOPPED, XRUN, CLIPPED, UNCONFIRMED, BELOW_TIER, LOST, GAP bits" },
    { "tier", "TIER_1 to TIER_3, TIER_SIM, TIER_WEB" },
    { "sample", "the source's sample on start_frame" },
    { "gap_frames", "a stream's silent frames" },
    { "pending", "True: not complete yet, the plan so far" },
    { NULL, NULL }
};

static PyStructSequence_Desc au_onset_desc = {
    "ysp.audio.Onset",
    "The record of one sound (yau_onset): times in seconds with the exact ns beside them.",
    au_onset_fields, 18
};

static PyObject* au_onset(const yau_onset* r, int pending) {
    PyObject* o = PyStructSequence_New((PyTypeObject*)OnsetType);
    PyObject* v[18];
    int i, bad = 0;
    if (!o) return NULL;
    v[0] = PyLong_FromLongLong(r->id);
    v[1] = PyFloat_FromDouble(au_s(r->target));
    v[2] = PyLong_FromLongLong(r->target);
    v[3] = PyFloat_FromDouble(au_s(r->onset));
    v[4] = PyLong_FromLongLong(r->onset);
    v[5] = PyFloat_FromDouble(au_s(r->residual));
    v[6] = PyLong_FromLongLong(r->residual);
    v[7] = PyLong_FromLongLong(r->start_frame);
    v[8] = PyLong_FromLongLong(r->end_frame);
    v[9] = PyLong_FromLongLong(r->device_pos);
    v[10] = PyFloat_FromDouble(au_s(r->rendered_at));
    v[11] = PyLong_FromLongLong(r->rendered_at);
    v[12] = PyLong_FromUnsignedLong(r->buffer_id);
    v[13] = PyLong_FromLong(r->flags);
    v[14] = PyLong_FromLong(r->tier);
    v[15] = PyLong_FromLongLong(r->sample);
    v[16] = PyLong_FromLongLong(r->gap_frames);
    v[17] = PyBool_FromLong(pending);
    for (i = 0; i < 18; i++) {
        if (!v[i]) bad = 1;
        else PyStructSequence_SetItem(o, i, v[i]);
    }
    if (bad) { Py_DECREF(o); return NULL; }
    return o;
}

/* --- sharing (bindings/share/ysp_audio_api.h) ----------------------------------- */

static void* au_api_acquire(void* ref, char* err, size_t cap) {
    PyObject* obj = (PyObject*)ref;
    AudioObject* o;
    if (!obj || !PyObject_TypeCheck(obj, (PyTypeObject*)AudioType)) {
        PyOS_snprintf(err, cap, "ysp.audio: an Audio is needed here");
        return NULL;
    }
    o = AU_OF(obj);
    if (!o->open || !o->box) { PyOS_snprintf(err, cap, "ysp.audio: the Audio is closed"); return NULL; }
    o->pins++;
    Py_INCREF(obj);
    return &o->box->au;
}

static void au_api_release(void* p) {
    AudioObject* o;
    if (!p) return;
    o = ((au_box*)p)->owner;
    if (!o) return;
    o->pins--;
    Py_DECREF((PyObject*)o);
}

/* --- Audio --------------------------------------------------------------------- */

static void au_close_children(AudioObject* o) {
    au_child* c;
    for (c = o->children; c; c = c->next) {
        if (!c->open) continue;
        c->open = 0;
        Py_BEGIN_ALLOW_THREADS
        if (c->kind == 1) yau_stream_close(&o->box->au, (yau_stream*)c->obj);
        else yau_wav_close(&o->box->au, (yau_wav*)c->obj);
        Py_END_ALLOW_THREADS
    }
}

static void au_free_children(AudioObject* o) {
    au_child* c = o->children;
    o->children = NULL;
    while (c) {
        au_child* next = c->next;
        PyMem_Free(c->obj);
        Py_XDECREF(c->keep);
        Py_XDECREF(c->path);
        PyMem_Free(c);
        c = next;
    }
}

static void au_shut(AudioObject* o) {
    if (o->box && o->open) {
        au_close_children(o);
        o->open = 0;
        Py_BEGIN_ALLOW_THREADS
        yau_close(&o->box->au);
        Py_END_ALLOW_THREADS
    }
    au_free_children(o);
}

static void Audio_dealloc(PyObject* self) {
    AudioObject* o = AU_OF(self);
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    au_shut(o);
    if (o->box) free(o->box);
    Py_XDECREF(o->device);
    fr(self);
    Py_DECREF(tp);
}

static int Audio_init(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "device", "backend", "rate", "channels", "sample", "exclusive", "period", "voices",
                              "queue", "min_tier", "onset_offset", "arena_bytes", "device_index", NULL };
    AudioObject* o = AU_OF(self);
    PyObject *dev = Py_None, *backend = NULL, *sample = NULL;
    unsigned int rate = 48000;
    int channels = 2, exclusive = 0, voices = 0, queue = 0, min_tier = 0, b = 0, smp = 0;
    unsigned int device_index = 0;
    double period = 0.0, offset = 0.0;
    Py_ssize_t arena = 0;
    yau_desc d;
    bool ok;
    int64_t pf = 0;
    if (o->box) { PyErr_SetString(AuError, "Audio.__init__ called twice"); return -1; }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|O$OIiOpdiiidnI", kwlist, &dev, &backend, &rate, &channels, &sample,
                                     &exclusive, &period, &voices, &queue, &min_tier, &offset, &arena, &device_index))
        return -1;
    if (au_lookup(backend, au_backends, "backend", &b) < 0 || au_lookup(sample, au_samples, "sample", &smp) < 0) return -1;
    if (channels < 1 || channels > YAU_MAX_CHANNELS) { PyErr_Format(AuArgumentError, "channels: 1 to %d", YAU_MAX_CHANNELS); return -1; }
    if (rate == 0) { PyErr_SetString(AuArgumentError, "rate: Hz, > 0"); return -1; }
    if (arena < 0) { PyErr_SetString(AuArgumentError, "arena_bytes: >= 0"); return -1; }
    if (!(offset >= -1.0 && offset <= 1.0)) { PyErr_SetString(AuArgumentError, "onset_offset: seconds, within +-1"); return -1; }
    if (au_s_to_frames(period, rate, "period", &pf) < 0) return -1;
    if (dev != Py_None) {
        if (!PyUnicode_Check(dev)) { PyErr_SetString(PyExc_TypeError, "device: a part of the device's name, or None"); return -1; }
        if (!(o->device = PyUnicode_AsUTF8String(dev))) return -1;
    }
    o->box = (au_box*)calloc(1, sizeof(au_box));
    if (!o->box) { PyErr_NoMemory(); return -1; }
    o->box->owner = o;
    memset(&d, 0, sizeof d);
    d.format.rate = rate;
    d.format.channels = (uint16_t)channels;
    d.format.sample = (uint16_t)smp;
    d.backend = (yau_backend)b;
    d.device = o->device ? PyBytes_AsString(o->device) : NULL;
    d.exclusive = exclusive != 0;
    d.period = (int32_t)pf;
    d.voices = voices;
    d.queue = queue;
    d.min_tier = min_tier;
    d.onset_offset_ns = (int64_t)llround(offset * 1e9);
    d.arena_bytes = (size_t)arena;
    d.device_index = device_index;
    Py_BEGIN_ALLOW_THREADS
    ok = yau_open(&o->box->au, &d);
    Py_END_ALLOW_THREADS
    if (!ok) {
        PyErr_SetString(AuError, yau_error(&o->box->au));
        return -1;
    }
    o->open = 1;
    return 0;
}

static PyObject* Audio_close(PyObject* self, PyObject* unused) {
    AudioObject* o = AU_OF(self);
    (void)unused;
    if (o->pins > 0) {
        PyErr_Format(AuBusyError, "ysp_audio: %d other module object(s) use this Audio (a movie's soundtrack?); close them first",
                     o->pins);
        return NULL;
    }
    if (o->waiting > 0) { PyErr_SetString(AuBusyError, "ysp_audio: another thread is in wait()"); return NULL; }
    au_shut(o);
    Py_RETURN_NONE;
}

static PyObject* Audio_enter(PyObject* self, PyObject* unused) { (void)unused; Py_INCREF(self); return self; }
static PyObject* Audio_exit(PyObject* self, PyObject* args) { (void)args; return Audio_close(self, NULL); }

static PyObject* Audio_get_open(PyObject* self, void* c) { (void)c; return PyBool_FromLong(AU_OF(self)->open && yau_is_open(&AU_OF(self)->box->au)); }
static PyObject* Audio_get_error(PyObject* self, void* c) { (void)c; return PyUnicode_FromString(AU_OF(self)->box ? yau_error(&AU_OF(self)->box->au) : ""); }
static PyObject* Audio_get_lead(PyObject* self, void* c) {
    (void)c;
    if (!au_ready(AU_OF(self))) return NULL;
    return PyFloat_FromDouble(au_s(yau_lead_ns(&AU_OF(self)->box->au)));
}
static PyObject* Audio_get_lead_ns(PyObject* self, void* c) {
    (void)c;
    if (!au_ready(AU_OF(self))) return NULL;
    return PyLong_FromLongLong(yau_lead_ns(&AU_OF(self)->box->au));
}
static PyObject* Audio_get_pins(PyObject* self, void* c) { (void)c; return PyLong_FromLong(AU_OF(self)->pins); }

static PyObject* au_claim(int64_t ns) {
    if (ns < 0) Py_RETURN_NONE;
    return PyFloat_FromDouble(au_s(ns));
}

static PyObject* Audio_caps(PyObject* self, PyObject* unused) {
    AudioObject* o = AU_OF(self);
    yau_caps c;
    double r;
    (void)unused;
    if (!au_ready(o)) return NULL;
    yau_get_caps(&o->box->au, &c);
    r = c.rate ? (double)c.rate : 1.0;
    return Py_BuildValue("{s:s,s:s,s:I,s:i,s:i,s:d,s:i,s:d,s:i,s:N,s:N,s:s,s:d,s:d,s:I,s:d,s:L,s:I,s:i,s:N,s:N,s:s,s:I,s:i}",
        "backend", au_backend_name(c.backend), "name", c.name, "rate", (unsigned)c.rate, "channels", (int)c.channels,
        "out", (int)c.out, "period", c.period / r, "period_frames", (int)c.period, "buffer", c.buffer / r,
        "buffer_frames", (int)c.buffer, "exclusive", PyBool_FromLong(c.exclusive), "low_latency", PyBool_FromLong(c.low_latency),
        "pos_source", c.pos_source == YAU_POS_DEVICE ? "device" : "callback", "drift_ppm", c.drift_ppm,
        "fit_spread", c.fit_spread_ns / 1e9, "fit_points", (unsigned)c.fit_points, "lead", au_s(c.lead_ns),
        "lead_ns", (long long)c.lead_ns, "xruns", (unsigned)c.xruns, "worst_tier", (int)c.worst_tier,
        "os_latency", au_claim(c.os_latency_ns), "os_stream_latency", au_claim(c.os_stream_latency_ns),
        "os_latency_src", c.os_latency_src, "gaps", (unsigned)c.gaps, "queue", (int)c.queue);
}

static PyObject* Audio_describe(PyObject* self, PyObject* unused) {
    AudioObject* o = AU_OF(self);
    char buf[1024];
    (void)unused;
    if (!au_ready(o)) return NULL;
    yau_describe(&o->box->au, buf, sizeof buf);
    return PyUnicode_FromString(buf);
}

static PyObject* Audio_frame_at(PyObject* self, PyObject* arg) {
    AudioObject* o = AU_OF(self);
    int64_t t, f;
    if (!au_ready(o) || au_obj_ns(arg, "t", &t) < 0) return NULL;
    f = yau_frame_at(&o->box->au, t);
    if (f < 0) Py_RETURN_NONE;
    return PyLong_FromLongLong(f);
}

static PyObject* Audio_time_of(PyObject* self, PyObject* arg) {
    AudioObject* o = AU_OF(self);
    long long fr = PyLong_AsLongLong(arg);
    int64_t t;
    if (fr == -1 && PyErr_Occurred()) return NULL;
    if (!au_ready(o)) return NULL;
    t = yau_time_of(&o->box->au, fr);
    if (t < 0) Py_RETURN_NONE;
    return Py_BuildValue("(dL)", au_s(t), (long long)t);
}

static PyObject* Audio_update(PyObject* self, PyObject* unused) {
    AudioObject* o = AU_OF(self);
    int n;
    (void)unused;
    if (!au_ready(o)) return NULL;
    n = yau_update(&o->box->au);
    if (n < 0) return au_fail(o, n);
    return PyLong_FromLong(n);
}

/* --- Audio: buffers --- */

static PyObject* au_buffer_new(AudioObject* o, yau_buf b) {
    BufferObject* bo;
    if (!b.frames) return au_fail(o, YAU_ERR_ARG);
    bo = PyObject_New(BufferObject, (PyTypeObject*)BufferType);
    if (!bo) return NULL;
    Py_INCREF((PyObject*)o);
    bo->owner = (PyObject*)o;
    bo->buf = b;
    bo->gen = o->gen;
    return (PyObject*)bo;
}

static PyObject* Audio_tone(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "hz", "duration", "peak", "ramp", "phase", "channels", "id", NULL };
    AudioObject* o = AU_OF(self);
    yau_tone_desc d;
    double hz, dur, ramp = 0.0, phase = 0.0;
    float peak;
    int ch = 1;
    unsigned int id = 0;
    memset(&d, 0, sizeof d);
    if (!au_ready(o) || !PyArg_ParseTupleAndKeywords(args, kw, "ddf|$ddiI", kwlist, &hz, &dur, &peak, &ramp, &phase, &ch, &id))
        return NULL;
    if (au_s_to_ns(dur, "duration", &d.dur_ns) < 0 || au_s_to_ns(ramp, "ramp", &d.ramp_ns) < 0) return NULL;
    if (ch < 1 || ch > YAU_MAX_CHANNELS) { PyErr_SetString(AuArgumentError, "channels: 1 or the device's"); return NULL; }
    d.hz = hz; d.peak = peak; d.phase = phase; d.channels = (uint16_t)ch;
    {
        yau_buf b = yau_tone(&o->box->au, &d);
        b.id = id;
        return au_buffer_new(o, b);
    }
}

static PyObject* Audio_noise(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "duration", "rms", "ramp", "seed", "dist", "channels", "id", NULL };
    static const au_name dists[] = { { "gauss", YAU_GAUSS }, { "uniform", YAU_UNIFORM }, { NULL, 0 } };
    AudioObject* o = AU_OF(self);
    yau_noise_desc d;
    double dur, ramp = 0.0;
    float rms;
    unsigned long long seed = 0;
    PyObject* dist = NULL;
    int ch = 1, di = YAU_GAUSS;
    unsigned int id = 0;
    memset(&d, 0, sizeof d);
    if (!au_ready(o) || !PyArg_ParseTupleAndKeywords(args, kw, "df|$dKOiI", kwlist, &dur, &rms, &ramp, &seed, &dist, &ch, &id))
        return NULL;
    if (au_lookup(dist, dists, "dist", &di) < 0) return NULL;
    if (au_s_to_ns(dur, "duration", &d.dur_ns) < 0 || au_s_to_ns(ramp, "ramp", &d.ramp_ns) < 0) return NULL;
    if (ch < 1 || ch > YAU_MAX_CHANNELS) { PyErr_SetString(AuArgumentError, "channels: 1 or the device's"); return NULL; }
    d.rms = rms; d.seed = seed; d.dist = di; d.channels = (uint16_t)ch;
    {
        yau_buf b = yau_noise(&o->box->au, &d);
        b.id = id;
        return au_buffer_new(o, b);
    }
}

static PyObject* Audio_click(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "peak", "duration", "id", NULL };
    AudioObject* o = AU_OF(self);
    yau_click_desc d;
    double dur = 0.0;
    float peak;
    unsigned int id = 0;
    memset(&d, 0, sizeof d);
    if (!au_ready(o) || !PyArg_ParseTupleAndKeywords(args, kw, "f|d$I", kwlist, &peak, &dur, &id)) return NULL;
    if (au_s_to_ns(dur, "duration", &d.dur_ns) < 0) return NULL;
    d.peak = peak;
    {
        yau_buf b = yau_click(&o->box->au, &d);
        b.id = id;
        return au_buffer_new(o, b);
    }
}

static PyObject* Audio_buffer(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "data", "channels", "id", NULL };
    AudioObject* o = AU_OF(self);
    PyObject* data;
    int ch = 0;
    unsigned int id = 0;
    int64_t n;
    float* f;
    yau_buf b;
    if (!au_ready(o) || !PyArg_ParseTupleAndKeywords(args, kw, "O|i$I", kwlist, &data, &ch, &id)) return NULL;
    if (ch < 0 || ch > YAU_MAX_CHANNELS) { PyErr_SetString(AuArgumentError, "channels: 1 or the device's"); return NULL; }
    if (!(f = au_floats(data, &n, &ch))) return NULL;
    if (n == 0) { PyMem_Free(f); PyErr_SetString(AuArgumentError, "data: no samples"); return NULL; }
    b = yau_alloc(&o->box->au, n, (uint16_t)ch);
    if (b.frames) memcpy(b.frames, f, (size_t)(n * ch) * sizeof(float));
    PyMem_Free(f);
    b.id = id;
    return au_buffer_new(o, b);
}

static PyObject* Audio_load_wav(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "path", "id", NULL };
    AudioObject* o = AU_OF(self);
    PyObject *p, *pb;
    unsigned int id = 0;
    yau_wav_desc d;
    yau_buf b;
    if (!au_ready(o) || !PyArg_ParseTupleAndKeywords(args, kw, "O|$I", kwlist, &p, &id)) return NULL;
    if (!(pb = au_path(p))) return NULL;
    memset(&d, 0, sizeof d);
    d.path = PyBytes_AsString(pb);
    d.id = id;
    Py_BEGIN_ALLOW_THREADS
    b = yau_wav_load(&o->box->au, &d);
    Py_END_ALLOW_THREADS
    Py_DECREF(pb);
    if (!b.frames) {
        const char* m = yau_error(&o->box->au);
        PyErr_SetString(strstr(m, "read") || strstr(m, "open") ? AuIOError : AuFormatError, m);
        return NULL;
    }
    b.id = id;
    return au_buffer_new(o, b);
}

static PyObject* Audio_arena_reset(PyObject* self, PyObject* unused) {
    AudioObject* o = AU_OF(self);
    int rc;
    (void)unused;
    if (!au_ready(o)) return NULL;
    if ((rc = yau_arena_reset(&o->box->au)) < 0) return au_fail(o, rc);
    o->gen++;
    Py_RETURN_NONE;
}

/* --- Audio: playing --- */

static int au_buf_of(AudioObject* o, PyObject* x, yau_buf* out) {
    BufferObject* b;
    if (!PyObject_TypeCheck(x, (PyTypeObject*)BufferType)) { PyErr_SetString(PyExc_TypeError, "a Buffer of this Audio"); return -1; }
    b = (BufferObject*)x;
    if (b->owner != (PyObject*)o) { PyErr_SetString(AuArgumentError, "the Buffer belongs to another Audio"); return -1; }
    if (b->gen != o->gen) { PyErr_SetString(AuArgumentError, "the Buffer was freed by arena_reset()"); return -1; }
    *out = b->buf;
    return 0;
}

/* The frame loop's call: no keywords, nothing allocated but the int. */
static PyObject* Audio_play_at(PyObject* self, PyObject* args) {
    AudioObject* o = AU_OF(self);
    PyObject* bo;
    double t;
    int64_t ns;
    yau_buf b;
    yau_id id;
    if (!PyArg_ParseTuple(args, "Od", &bo, &t)) return NULL;
    if (!au_ready(o) || au_buf_of(o, bo, &b) < 0 || au_s_to_ns(t, "t", &ns) < 0) return NULL;
    id = yau_play_at(&o->box->au, b, ns);
    if (id < 0) return au_fail(o, (int)id);
    return PyLong_FromLongLong(id);
}

static PyObject* Audio_play(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "sound", "at", "db", "channels", "offset", "frames", "loops", NULL };
    AudioObject* o = AU_OF(self);
    PyObject *snd, *mask = Py_None;
    double at = 0.0;
    float db = 0.0f;
    long long offset = 0, frames = 0;
    int loops = 0;
    yau_play_desc d;
    yau_id id;
    memset(&d, 0, sizeof d);
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|d$fOLLi", kwlist, &snd, &at, &db, &mask, &offset, &frames, &loops))
        return NULL;
    if (!au_ready(o) || au_s_to_ns(at, "at", &d.at_ns) < 0 || au_mask(mask, &d.channels) < 0) return NULL;
    if (PyObject_TypeCheck(snd, (PyTypeObject*)StreamType) || PyObject_TypeCheck(snd, (PyTypeObject*)WavType)) {
        StreamObject* s = (StreamObject*)snd;
        if (s->owner != self) { PyErr_SetString(AuArgumentError, "the stream belongs to another Audio"); return NULL; }
        if (!s->c->open) { PyErr_SetString(AuClosedError, "ysp_audio: the stream is closed"); return NULL; }
        if (offset || frames || loops) { PyErr_SetString(AuArgumentError, "offset, frames and loops are for buffers; a WAV loops itself"); return NULL; }
        d.stream = s->c->kind == 1 ? (yau_stream*)s->c->obj : &((yau_wav*)s->c->obj)->stream;
    } else {
        if (au_buf_of(o, snd, &d.buf) < 0) return NULL;
        d.offset = offset; d.frames = frames; d.loops = loops;
    }
    d.db = db;
    id = yau_play(&o->box->au, &d);
    if (id < 0) return au_fail(o, (int)id);
    return PyLong_FromLongLong(id);
}

static PyObject* Audio_cancel(PyObject* self, PyObject* arg) {
    AudioObject* o = AU_OF(self);
    long long id = PyLong_AsLongLong(arg);
    int rc;
    if (id == -1 && PyErr_Occurred()) return NULL;
    if (!au_ready(o)) return NULL;
    if ((rc = yau_cancel(&o->box->au, id)) < 0) return au_fail(o, rc);
    Py_RETURN_NONE;
}

static PyObject* Audio_stop(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "id", "at", "ramp", NULL };
    AudioObject* o = AU_OF(self);
    long long id = 0;
    double at = 0.0, ramp = 0.0;
    int64_t t, r;
    int rc;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|L$dd", kwlist, &id, &at, &ramp)) return NULL;
    if (!au_ready(o) || au_s_to_ns(at, "at", &t) < 0 || au_s_to_ns(ramp, "ramp", &r) < 0) return NULL;
    if ((rc = yau_stop_at(&o->box->au, id, t, r)) < 0) return au_fail(o, rc);
    Py_RETURN_NONE;
}

static PyObject* Audio_gain(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "id", "db", "at", "ramp", NULL };
    AudioObject* o = AU_OF(self);
    long long id;
    float db;
    double at = 0.0, ramp = 0.0;
    int64_t t, r;
    int rc;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "Lf|$dd", kwlist, &id, &db, &at, &ramp)) return NULL;
    if (!au_ready(o) || au_s_to_ns(at, "at", &t) < 0 || au_s_to_ns(ramp, "ramp", &r) < 0) return NULL;
    if ((rc = yau_gain_at(&o->box->au, id, t, db, r)) < 0) return au_fail(o, rc);
    Py_RETURN_NONE;
}

static PyObject* Audio_result(PyObject* self, PyObject* arg) {
    AudioObject* o = AU_OF(self);
    long long id = PyLong_AsLongLong(arg);
    yau_onset r;
    int rc;
    if (id == -1 && PyErr_Occurred()) return NULL;
    if (!au_ready(o)) return NULL;
    rc = yau_result(&o->box->au, id, &r);
    if (rc < 0) return au_fail(o, rc);
    return au_onset(&r, rc == YAU_PENDING);
}

static PyObject* Audio_wait(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "id", "timeout", NULL };
    AudioObject* o = AU_OF(self);
    long long id;
    double timeout = INFINITY;
    int64_t tns;
    yau_onset r;
    int rc;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "L|d", kwlist, &id, &timeout)) return NULL;
    if (!au_ready(o)) return NULL;
    if (timeout == INFINITY) tns = INT64_MAX;   /* yau_wait()'s "forever" */
    else if (au_s_to_ns(timeout, "timeout", &tns) < 0) return NULL;
    /* yau_wait() sleeps in 1 ms steps without the GIL; close() refuses
     * meanwhile (waiting), so no other thread closes the handle under it. */
    o->waiting++;
    Py_BEGIN_ALLOW_THREADS
    rc = yau_wait(&o->box->au, id, tns, &r);
    Py_END_ALLOW_THREADS
    o->waiting--;
    if (rc < 0) return au_fail(o, rc);
    return au_onset(&r, rc == YAU_PENDING);
}

/* --- Audio: streams and WAV readers --- */

static au_child* au_child_new(AudioObject* o, int kind, size_t bytes) {
    au_child* c = (au_child*)PyMem_Malloc(sizeof(au_child));
    if (!c) { PyErr_NoMemory(); return NULL; }
    memset(c, 0, sizeof *c);
    c->obj = PyMem_Malloc(bytes);
    if (!c->obj) { PyMem_Free(c); PyErr_NoMemory(); return NULL; }
    memset(c->obj, 0, bytes);
    c->kind = kind;
    c->next = o->children;
    o->children = c;
    return c;
}

static PyObject* au_stream_obj(AudioObject* o, au_child* c, PyObject* type) {
    StreamObject* s = PyObject_New(StreamObject, (PyTypeObject*)type);
    if (!s) return NULL;
    Py_INCREF((PyObject*)o);
    s->owner = (PyObject*)o;
    s->c = c;
    return (PyObject*)s;
}

static PyObject* Audio_stream(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "channels", "ring", "preroll", "first", "id", NULL };
    AudioObject* o = AU_OF(self);
    int ch = 0;
    double ring = 0.0;
    PyObject* pre = Py_None;
    long long first = 0;
    unsigned int id = 0;
    yau_stream_desc d;
    au_child* c;
    uint32_t rate;
    memset(&d, 0, sizeof d);
    if (!au_ready(o) || !PyArg_ParseTupleAndKeywords(args, kw, "|i$dOLI", kwlist, &ch, &ring, &pre, &first, &id)) return NULL;
    rate = o->box->au.dcaps.rate ? o->box->au.dcaps.rate : 48000;
    if (au_s_to_frames(ring, rate, "ring", &d.frames) < 0) return NULL;
    if (pre != Py_None) {
        if (PyUnicode_Check(pre)) {
            char s[8];
            if (au_utf8(pre, s, sizeof s) < 0) return NULL;
            if (strcmp(s, "none") != 0) { PyErr_SetString(AuArgumentError, "preroll: seconds, or \"none\""); return NULL; }
            d.preroll = YAU_PREROLL_NONE;
        } else {
            double p = PyFloat_AsDouble(pre);
            if (p == -1.0 && PyErr_Occurred()) return NULL;
            if (au_s_to_frames(p, rate, "preroll", &d.preroll) < 0) return NULL;
            if (d.preroll == 0) d.preroll = YAU_PREROLL_NONE;
        }
    }
    if (ch < 0 || ch > YAU_MAX_CHANNELS) { PyErr_SetString(AuArgumentError, "channels: 1 or the device's"); return NULL; }
    d.channels = (uint16_t)ch;
    d.first = first;
    d.id = id;
    if (!(c = au_child_new(o, 1, sizeof(yau_stream)))) return NULL;
    if (!yau_stream_open(&o->box->au, (yau_stream*)c->obj, &d)) return au_fail(o, YAU_ERR_ARG);
    c->open = 1;
    return au_stream_obj(o, c, StreamType);
}

static PyObject* Audio_wav(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "path", "data", "loops", "start", "ring", "id", NULL };
    AudioObject* o = AU_OF(self);
    PyObject *p = Py_None, *data = Py_None;
    int loops = 0;
    long long start = 0;
    double ring = 0.0;
    unsigned int id = 0;
    yau_wav_desc d;
    au_child* c;
    yau_wav* w;
    uint32_t rate;
    memset(&d, 0, sizeof d);
    if (!au_ready(o) || !PyArg_ParseTupleAndKeywords(args, kw, "|O$OiLdI", kwlist, &p, &data, &loops, &start, &ring, &id)) return NULL;
    if ((p == Py_None) == (data == Py_None)) { PyErr_SetString(AuArgumentError, "a path or data (bytes), one of them"); return NULL; }
    rate = o->box->au.dcaps.rate ? o->box->au.dcaps.rate : 48000;
    if (au_s_to_frames(ring, rate, "ring", &d.ring) < 0) return NULL;
    if (!(c = au_child_new(o, 2, sizeof(yau_wav)))) return NULL;
    if (p != Py_None) {
        if (!(c->path = au_path(p))) return NULL;
        d.path = PyBytes_AsString(c->path);
    } else {
        if (!PyBytes_Check(data)) { PyErr_SetString(PyExc_TypeError, "data: the WAV file's bytes"); return NULL; }
        Py_INCREF(data);
        c->keep = data;
        d.data = PyBytes_AsString(data);
        d.size = (size_t)PyBytes_Size(data);
    }
    d.loops = loops < 0 ? YAU_FOREVER : loops;
    d.start = start;
    d.id = id;
    w = (yau_wav*)c->obj;
    if (!yau_wav_open(&o->box->au, w, &d)) {
        PyErr_SetString(au_exc(w->error_code), yau_wav_error(w));
        return NULL;
    }
    c->open = 1;
    return au_stream_obj(o, c, WavType);
}

static PyMethodDef Audio_methods[] = {
    { "close", Audio_close, METH_NOARGS, "close(): stop the device; every record completes. BusyError while another module holds it." },
    { "__enter__", Audio_enter, METH_NOARGS, NULL },
    { "__exit__", Audio_exit, METH_VARARGS, NULL },
    { "caps", Audio_caps, METH_NOARGS, "caps() -> dict: what the device runs at (seconds and frames), the fit, the lead, the claims." },
    { "describe", Audio_describe, METH_NOARGS, "describe() -> str: one line for the log." },
    { "frame_at", Audio_frame_at, METH_O, "frame_at(t) -> int or None: the stream frame on which time t falls (the fit)." },
    { "time_of", Audio_time_of, METH_O, "time_of(frame) -> (seconds, ns) or None: the fit's time of a stream frame." },
    { "update", Audio_update, METH_NOARGS, "update() -> int: move completed records into the handle." },
    { "tone", (PyCFunction)(void (*)(void))Audio_tone, METH_VARARGS | METH_KEYWORDS,
      "tone(hz, duration, peak, *, ramp=0, phase=0, channels=1, id=0) -> Buffer: a sine at the device rate, in the arena." },
    { "noise", (PyCFunction)(void (*)(void))Audio_noise, METH_VARARGS | METH_KEYWORDS,
      "noise(duration, rms, *, ramp=0, seed=0, dist='gauss', channels=1, id=0) -> Buffer. Noise that would clip is refused." },
    { "click", (PyCFunction)(void (*)(void))Audio_click, METH_VARARGS | METH_KEYWORDS,
      "click(peak, duration=0, *, id=0) -> Buffer: a rectangular click; 0 = one frame." },
    { "buffer", (PyCFunction)(void (*)(void))Audio_buffer, METH_VARARGS | METH_KEYWORDS,
      "buffer(data, channels=0, *, id=0) -> Buffer: float32 or float64 samples (1-D, or frames x channels) copied into the arena." },
    { "load_wav", (PyCFunction)(void (*)(void))Audio_load_wav, METH_VARARGS | METH_KEYWORDS,
      "load_wav(path, *, id=0) -> Buffer: a whole WAV file in the arena, at the device rate." },
    { "arena_reset", Audio_arena_reset, METH_NOARGS, "arena_reset(): free every buffer and arena ring; BusyError while one plays." },
    { "play_at", Audio_play_at, METH_VARARGS, "play_at(buffer, t) -> id: play at time t (seconds, ysp_rt clock; 0: as soon as it can)." },
    { "play", (PyCFunction)(void (*)(void))Audio_play, METH_VARARGS | METH_KEYWORDS,
      "play(sound, at=0, *, db=0, channels=None, offset=0, frames=0, loops=0) -> id. sound: a Buffer, Stream or Wav; "
      "channels: a mask or channel numbers from 0; loops -1: until stopped." },
    { "cancel", Audio_cancel, METH_O, "cancel(id): a sound not started never plays; one playing stops at the next block." },
    { "stop", (PyCFunction)(void (*)(void))Audio_stop, METH_VARARGS | METH_KEYWORDS,
      "stop(id=0, *, at=0, ramp=0): a raised-cosine fade of `ramp` seconds that ends at `at`; id 0: every sound." },
    { "gain", (PyCFunction)(void (*)(void))Audio_gain, METH_VARARGS | METH_KEYWORDS,
      "gain(id, db, *, at=0, ramp=0): a linear change of gain from `at` over `ramp` seconds." },
    { "result", Audio_result, METH_O, "result(id) -> Onset: the record; pending=True until the onset is known." },
    { "wait", (PyCFunction)(void (*)(void))Audio_wait, METH_VARARGS | METH_KEYWORDS,
      "wait(id, timeout=inf) -> Onset: block (GIL released) until the record is complete. TimeoutError." },
    { "stream", (PyCFunction)(void (*)(void))Audio_stream, METH_VARARGS | METH_KEYWORDS,
      "stream(channels=0, *, ring=1.0, preroll=None, first=0, id=0) -> Stream: a voice that Stream.write() fills." },
    { "wav", (PyCFunction)(void (*)(void))Audio_wav, METH_VARARGS | METH_KEYWORDS,
      "wav(path=None, *, data=None, loops=0, start=0, ring=1.0, id=0) -> Wav: a WAV file streamed; feed() fills its ring." },
    { NULL, NULL, 0, NULL }
};

static PyGetSetDef Audio_getset[] = {
    { "is_open", Audio_get_open, NULL, "True while the device is open and not lost", NULL },
    { "error", Audio_get_error, NULL, "the message of the last failure", NULL },
    { "lead", Audio_get_lead, NULL, "the shortest lead a sound handed over now can meet, seconds", NULL },
    { "lead_ns", Audio_get_lead_ns, NULL, "the same, ns", NULL },
    { "pins", Audio_get_pins, NULL, "objects of other modules that hold this Audio (SHARING)", NULL },
    { NULL, NULL, NULL, NULL, NULL }
};

static PyType_Slot Audio_slots[] = {
    { Py_tp_doc, "Audio(device=None, *, backend='auto', rate=48000, channels=2, sample='f32', exclusive=False, period=0, "
                 "voices=0, queue=0, min_tier=0, onset_offset=0, arena_bytes=0, device_index=0): an output on the ysp_rt "
                 "clock (ysp/audio.h). device: part of a name from devices(); backend 'null' for tests. Opening "
                 "refuses any format or rate conversion (STRICT OPEN)." },
    { Py_tp_new, PyType_GenericNew },
    { Py_tp_init, Audio_init },
    { Py_tp_dealloc, Audio_dealloc },
    { Py_tp_methods, Audio_methods },
    { Py_tp_getset, Audio_getset },
    { 0, NULL }
};
static PyType_Spec Audio_spec = { "ysp.audio.Audio", sizeof(AudioObject), 0, Py_TPFLAGS_DEFAULT, Audio_slots };

/* --- Buffer -------------------------------------------------------------------- */

static void Buffer_dealloc(PyObject* self) {
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    Py_XDECREF(((BufferObject*)self)->owner);
    fr(self);
    Py_DECREF(tp);
}

static int buf_live(BufferObject* b) {
    AudioObject* o = AU_OF(b->owner);
    if (!au_ready(o)) return 0;
    if (b->gen != o->gen) { PyErr_SetString(AuArgumentError, "the Buffer was freed by arena_reset()"); return 0; }
    return 1;
}

static PyObject* Buffer_get(PyObject* self, void* which) {
    BufferObject* b = (BufferObject*)self;
    AudioObject* o = AU_OF(b->owner);
    switch ((int)(intptr_t)which) {
    case 0: return PyLong_FromLongLong(b->buf.n);
    case 1: return PyLong_FromLong(b->buf.channels);
    case 2: return PyLong_FromUnsignedLong(b->buf.id);
    default: {
        uint32_t r = o->box && o->box->au.dcaps.rate ? o->box->au.dcaps.rate : 48000;
        return PyFloat_FromDouble((double)b->buf.n / (double)r);
    }
    }
}

static PyObject* Buffer_samples(PyObject* self, PyObject* unused) {
    BufferObject* b = (BufferObject*)self;
    (void)unused;
    if (!buf_live(b)) return NULL;
    return au_view(b->buf.frames, b->buf.n, b->buf.channels);
}

static PyMethodDef Buffer_methods[] = {
    { "samples", Buffer_samples, METH_NOARGS, "samples() -> memoryview('f') of frames x channels: a copy." },
    { NULL, NULL, 0, NULL }
};

static PyGetSetDef Buffer_getset[] = {
    { "frames", Buffer_get, NULL, "frames", (void*)0 },
    { "channels", Buffer_get, NULL, "1, or the device's count", (void*)1 },
    { "id", Buffer_get, NULL, "your id, in every record", (void*)2 },
    { "duration", Buffer_get, NULL, "seconds at the device rate", (void*)3 },
    { NULL, NULL, NULL, NULL, NULL }
};

static PyType_Slot Buffer_slots[] = {
    { Py_tp_doc, "A sound in an Audio's arena: from tone(), noise(), click(), buffer() or load_wav()." },
    { Py_tp_dealloc, Buffer_dealloc },
    { Py_tp_methods, Buffer_methods },
    { Py_tp_getset, Buffer_getset },
    { 0, NULL }
};
static PyType_Spec Buffer_spec = { "ysp.audio.Buffer", sizeof(BufferObject), 0, Py_TPFLAGS_DEFAULT, Buffer_slots };

/* --- Stream and Wav ------------------------------------------------------------ */

static void Stream_dealloc(PyObject* self) {
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    Py_XDECREF(((StreamObject*)self)->owner);
    fr(self);
    Py_DECREF(tp);
}

/* The owner first: a closed Audio has freed its children. */
static yau_stream* st_ready(StreamObject* s) {
    if (!au_ready(AU_OF(s->owner))) return NULL;
    if (!s->c->open) { PyErr_SetString(AuClosedError, "ysp_audio: the stream is closed"); return NULL; }
    return s->c->kind == 1 ? (yau_stream*)s->c->obj : &((yau_wav*)s->c->obj)->stream;
}

static PyObject* Stream_write(PyObject* self, PyObject* arg) {
    StreamObject* s = (StreamObject*)self;
    yau_stream* st = st_ready(s);
    int64_t n, k;
    int ch;
    float* f;
    if (!st) return NULL;
    if (s->c->kind != 1) { PyErr_SetString(PyExc_TypeError, "a Wav fills itself: feed()"); return NULL; }
    ch = st->ch_;
    if (!(f = au_floats(arg, &n, &ch))) return NULL;
    k = yau_stream_write(st, f, n);
    PyMem_Free(f);
    if (k < 0) { PyErr_SetString(au_exc((int)k), yau_strerror((int)k)); return NULL; }
    return PyLong_FromLongLong(k);
}

static PyObject* Stream_end(PyObject* self, PyObject* unused) {
    yau_stream* st = st_ready((StreamObject*)self);
    int rc;
    (void)unused;
    if (!st) return NULL;
    if ((rc = yau_stream_end(st)) < 0) { PyErr_SetString(au_exc(rc), yau_strerror(rc)); return NULL; }
    Py_RETURN_NONE;
}

static PyObject* Stream_reset(PyObject* self, PyObject* args) {
    yau_stream* st = st_ready((StreamObject*)self);
    long long first = 0;
    int rc;
    if (!st || !PyArg_ParseTuple(args, "|L", &first)) return NULL;
    if ((rc = yau_stream_reset(st, first)) < 0) { PyErr_SetString(au_exc(rc), "ysp_audio: reset: the stream is playing (BUSY until it has ended)"); return NULL; }
    Py_RETURN_NONE;
}

static PyObject* Stream_info(PyObject* self, PyObject* unused) {
    yau_stream* st = st_ready((StreamObject*)self);
    yau_stream_info i;
    static const char* states[] = { "idle", "queued", "waiting", "playing", "ended" };
    (void)unused;
    if (!st) return NULL;
    yau_stream_get_info(st, &i);
    return Py_BuildValue("{s:s,s:N,s:N,s:L,s:L,s:L,s:L,s:L,s:L,s:L,s:L,s:L,s:L,s:L,s:I,s:L,s:L}",
        "state", i.state >= 0 && i.state <= 4 ? states[i.state] : "?", "started", PyBool_FromLong(i.started),
        "ready", PyBool_FromLong(i.ready), "id", (long long)i.id, "first", (long long)i.first, "written", (long long)i.written,
        "read", (long long)i.read, "fill", (long long)i.fill, "preroll", (long long)i.preroll, "next", (long long)i.next,
        "start_frame", (long long)i.start_frame, "frames_to_start", (long long)i.frames_to_start,
        "origin", (long long)i.origin, "end", (long long)i.end, "gaps", (unsigned)i.gaps,
        "gap_frames", (long long)i.gap_frames, "discarded", (long long)i.discarded);
}

static PyObject* Stream_play(PyObject* self, PyObject* args, PyObject* kw) {
    StreamObject* s = (StreamObject*)self;
    PyObject *a2, *r;
    Py_ssize_t n, i;
    if (!st_ready(s)) return NULL;
    n = PyTuple_Size(args);
    a2 = PyTuple_New(n + 1);
    if (!a2) return NULL;
    Py_INCREF(self);
    PyTuple_SetItem(a2, 0, self);
    for (i = 0; i < n; i++) { PyObject* x = PyTuple_GetItem(args, i); Py_INCREF(x); PyTuple_SetItem(a2, i + 1, x); }
    r = Audio_play(s->owner, a2, kw);
    Py_DECREF(a2);
    return r;
}

static PyObject* Stream_sample_at(PyObject* self, PyObject* arg) {
    StreamObject* s = (StreamObject*)self;
    yau_stream* st = st_ready(s);
    int64_t t, smp;
    int rc;
    if (!st || au_obj_ns(arg, "t", &t) < 0) return NULL;
    rc = yau_stream_sample_at(&AU_OF(s->owner)->box->au, st, t, &smp);
    if (rc == YAU_PENDING) Py_RETURN_NONE;
    if (rc < 0) return au_fail(AU_OF(s->owner), rc);
    return PyLong_FromLongLong(smp);
}

static PyObject* Stream_time_of(PyObject* self, PyObject* arg) {
    StreamObject* s = (StreamObject*)self;
    yau_stream* st = st_ready(s);
    long long smp = PyLong_AsLongLong(arg);
    int64_t t;
    int rc;
    if (smp == -1 && PyErr_Occurred()) return NULL;
    if (!st) return NULL;
    rc = yau_stream_time_of(&AU_OF(s->owner)->box->au, st, smp, &t);
    if (rc == YAU_PENDING) Py_RETURN_NONE;
    if (rc < 0) return au_fail(AU_OF(s->owner), rc);
    return Py_BuildValue("(dL)", au_s(t), (long long)t);
}

static PyObject* Stream_close(PyObject* self, PyObject* unused) {
    StreamObject* s = (StreamObject*)self;
    AudioObject* o = AU_OF(s->owner);
    (void)unused;
    if (!o->open || !s->c->open) Py_RETURN_NONE;
    s->c->open = 0;
    Py_BEGIN_ALLOW_THREADS
    if (s->c->kind == 1) yau_stream_close(&o->box->au, (yau_stream*)s->c->obj);
    else yau_wav_close(&o->box->au, (yau_wav*)s->c->obj);
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

static PyObject* Wav_feed(PyObject* self, PyObject* args) {
    StreamObject* s = (StreamObject*)self;
    long long maxf = 0;
    int64_t n;
    yau_wav* w;
    if (!st_ready(s) || !PyArg_ParseTuple(args, "|L", &maxf)) return NULL;
    w = (yau_wav*)s->c->obj;
    Py_BEGIN_ALLOW_THREADS
    n = yau_wav_feed(w, maxf);
    Py_END_ALLOW_THREADS
    if (n < 0) { PyErr_SetString(au_exc((int)n), yau_wav_error(w)); return NULL; }
    return PyLong_FromLongLong(n);
}

static PyObject* Wav_seek(PyObject* self, PyObject* arg) {
    StreamObject* s = (StreamObject*)self;
    long long smp = PyLong_AsLongLong(arg);
    int rc;
    if (smp == -1 && PyErr_Occurred()) return NULL;
    if (!st_ready(s)) return NULL;
    rc = yau_wav_seek((yau_wav*)s->c->obj, smp);
    if (rc < 0) { PyErr_SetString(au_exc(rc), rc == YAU_ERR_BUSY ? "ysp_audio: seek: the WAV is playing (stop it, then seek)" : yau_wav_error((yau_wav*)s->c->obj)); return NULL; }
    Py_RETURN_NONE;
}

static PyObject* Wav_get(PyObject* self, void* which) {
    StreamObject* s = (StreamObject*)self;
    yau_wav* w;
    if (!st_ready(s)) return NULL;
    w = (yau_wav*)s->c->obj;
    if (which) return PyBool_FromLong(yau_wav_wants(w));
    return Py_BuildValue("{s:I,s:i,s:i,s:L,s:d,s:N}", "rate", (unsigned)w->info.rate, "channels", (int)w->info.channels,
                         "format", (int)w->info.format, "frames", (long long)w->info.frames,
                         "duration", w->info.rate ? (double)w->info.frames / w->info.rate : 0.0, "rf64", PyBool_FromLong(w->info.rf64));
}

static PyMethodDef Stream_methods[] = {
    { "write", Stream_write, METH_O, "write(data) -> frames taken: float32/float64 samples into the ring (the producer's call)." },
    { "end", Stream_end, METH_NOARGS, "end(): no sample follows the last one written; the play then ends there." },
    { "reset", Stream_reset, METH_VARARGS, "reset(first=0): empty the ring and number the next sample; BusyError while playing." },
    { "info", Stream_info, METH_NOARGS, "info() -> dict: state, fill, origin, gaps (yau_stream_get_info)." },
    { "play", (PyCFunction)(void (*)(void))Stream_play, METH_VARARGS | METH_KEYWORDS, "play(at=0, *, db=0, channels=None) -> id." },
    { "sample_at", Stream_sample_at, METH_O, "sample_at(t) -> int or None: the sample on the frame of time t, once started." },
    { "time_of", Stream_time_of, METH_O, "time_of(sample) -> (seconds, ns) or None, once started." },
    { "close", Stream_close, METH_NOARGS, "close(): cancel a play still running and wait for the device to let go." },
    { NULL, NULL, 0, NULL }
};

static PyMethodDef Wav_methods[] = {
    { "feed", Wav_feed, METH_VARARGS, "feed(max_frames=0) -> frames written: read the file into the ring (0: until full); GIL released." },
    { "seek", Wav_seek, METH_O, "seek(sample): the next play starts at this sample; BusyError while playing." },
    { "info", Stream_info, METH_NOARGS, "info() -> dict: the stream's state (yau_stream_get_info)." },
    { "play", (PyCFunction)(void (*)(void))Stream_play, METH_VARARGS | METH_KEYWORDS, "play(at=0, *, db=0, channels=None) -> id." },
    { "sample_at", Stream_sample_at, METH_O, "sample_at(t) -> int or None." },
    { "time_of", Stream_time_of, METH_O, "time_of(sample) -> (seconds, ns) or None." },
    { "close", Stream_close, METH_NOARGS, "close(): cancel a play still running and close the file." },
    { NULL, NULL, 0, NULL }
};

static PyGetSetDef Wav_getset[] = {
    { "file", Wav_get, NULL, "the file's form: rate, channels, format, frames, duration, rf64", NULL },
    { "wants", Wav_get, NULL, "True when the ring is under half full: feed() now", (void*)1 },
    { NULL, NULL, NULL, NULL, NULL }
};

static PyType_Slot Stream_slots[] = {
    { Py_tp_doc, "A voice fed from a ring (Audio.stream()): write() from any one thread, play() from the frame thread." },
    { Py_tp_dealloc, Stream_dealloc },
    { Py_tp_methods, Stream_methods },
    { 0, NULL }
};
static PyType_Spec Stream_spec = { "ysp.audio.Stream", sizeof(StreamObject), 0, Py_TPFLAGS_DEFAULT, Stream_slots };

static PyType_Slot Wav_slots[] = {
    { Py_tp_doc, "A WAV file streamed (Audio.wav()): feed() fills the ring, from the frame thread or one other thread." },
    { Py_tp_dealloc, Stream_dealloc },
    { Py_tp_methods, Wav_methods },
    { Py_tp_getset, Wav_getset },
    { 0, NULL }
};
static PyType_Spec Wav_spec = { "ysp.audio.Wav", sizeof(StreamObject), 0, Py_TPFLAGS_DEFAULT, Wav_slots };

/* --- Capture ------------------------------------------------------------------- */

#define CAP_OF(o) ((CaptureObject*)(o))

static int cap_ready(CaptureObject* o) {
    if (!o->open || !o->c) { PyErr_SetString(AuClosedError, "ysp_audio: the capture is closed"); return 0; }
    return 1;
}

static void cap_shut(CaptureObject* o) {
    if (o->c && o->open) {
        o->open = 0;
        Py_BEGIN_ALLOW_THREADS
        yau_capture_close(o->c);
        Py_END_ALLOW_THREADS
    }
}

static void Capture_dealloc(PyObject* self) {
    CaptureObject* o = CAP_OF(self);
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    cap_shut(o);
    free(o->c);
    Py_XDECREF(o->device);
    fr(self);
    Py_DECREF(tp);
}

static int Capture_init(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "device", "backend", "rate", "channels", "sample", "exclusive", "period", "ring", "offset",
                              "device_index", NULL };
    CaptureObject* o = CAP_OF(self);
    PyObject *dev = Py_None, *backend = NULL, *sample = NULL;
    unsigned int rate = 48000, device_index = 0;
    int channels = 0, exclusive = 0, b = 0, smp = 0;
    double period = 0.0, ring = 0.0, offset = 0.0;
    int64_t pf, rf;
    yau_capture_desc d;
    bool ok;
    if (o->c) { PyErr_SetString(AuError, "Capture.__init__ called twice"); return -1; }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|O$OIiOpdddI", kwlist, &dev, &backend, &rate, &channels, &sample, &exclusive,
                                     &period, &ring, &offset, &device_index))
        return -1;
    if (au_lookup(backend, au_backends, "backend", &b) < 0 || au_lookup(sample, au_samples, "sample", &smp) < 0) return -1;
    if (rate == 0) { PyErr_SetString(AuArgumentError, "rate: Hz, > 0"); return -1; }
    if (channels < 0 || channels > YAU_MAX_CHANNELS) { PyErr_SetString(AuArgumentError, "channels: 0 (the device's) to 64"); return -1; }
    if (au_s_to_frames(period, rate, "period", &pf) < 0 || au_s_to_frames(ring, rate, "ring", &rf) < 0) return -1;
    if (!(offset >= -1.0 && offset <= 1.0)) { PyErr_SetString(AuArgumentError, "offset: seconds, within +-1"); return -1; }
    if (dev != Py_None) {
        if (!PyUnicode_Check(dev)) { PyErr_SetString(PyExc_TypeError, "device: a part of the device's name, or None"); return -1; }
        if (!(o->device = PyUnicode_AsUTF8String(dev))) return -1;
    }
    o->c = (yau_capture*)calloc(1, sizeof(yau_capture));
    if (!o->c) { PyErr_NoMemory(); return -1; }
    memset(&d, 0, sizeof d);
    d.format.rate = rate;
    d.format.channels = (uint16_t)channels;
    d.format.sample = (uint16_t)smp;
    d.backend = (yau_backend)b;
    d.device = o->device ? PyBytes_AsString(o->device) : NULL;
    d.exclusive = exclusive != 0;
    d.period = (int32_t)pf;
    d.frames = rf;
    d.offset_ns = (int64_t)llround(offset * 1e9);
    d.device_index = device_index;
    Py_BEGIN_ALLOW_THREADS
    ok = yau_capture_open(o->c, &d);
    Py_END_ALLOW_THREADS
    if (!ok) { PyErr_SetString(AuError, yau_capture_error(o->c)); return -1; }
    o->open = 1;
    return 0;
}

static PyObject* Capture_close(PyObject* self, PyObject* unused) { (void)unused; cap_shut(CAP_OF(self)); Py_RETURN_NONE; }
static PyObject* Capture_enter(PyObject* self, PyObject* unused) { (void)unused; Py_INCREF(self); return self; }
static PyObject* Capture_exit(PyObject* self, PyObject* args) { (void)args; return Capture_close(self, NULL); }

static PyObject* Capture_read(PyObject* self, PyObject* args) {
    CaptureObject* o = CAP_OF(self);
    long long maxf = -1;
    yau_capture_info info;
    float* buf;
    int64_t n, first = -1;
    PyObject *v, *r;
    if (!cap_ready(o) || !PyArg_ParseTuple(args, "|L", &maxf)) return NULL;
    yau_capture_update(o->c);
    yau_capture_get_info(o->c, &info);
    if (maxf < 0) maxf = info.fill[0];
    buf = (float*)PyMem_Malloc((size_t)(maxf > 0 ? maxf : 1) * info.channels * sizeof(float));
    if (!buf) return PyErr_NoMemory();
    n = yau_capture_read(o->c, 0, buf, maxf, &first);
    if (n < 0) { PyMem_Free(buf); PyErr_SetString(au_exc((int)n), yau_capture_error(o->c)); return NULL; }
    v = au_view(buf, n, info.channels);
    PyMem_Free(buf);
    if (!v) return NULL;
    r = Py_BuildValue("(NL)", v, (long long)first);
    return r;
}

static PyObject* Capture_time_of(PyObject* self, PyObject* arg) {
    CaptureObject* o = CAP_OF(self);
    double f = PyFloat_AsDouble(arg);
    int64_t t;
    if (f == -1.0 && PyErr_Occurred()) return NULL;
    if (!cap_ready(o)) return NULL;
    t = PyLong_Check(arg) ? yau_capture_time_of(o->c, PyLong_AsLongLong(arg)) : yau_capture_time_at(o->c, f);
    if (t < 0) Py_RETURN_NONE;
    return Py_BuildValue("(dL)", au_s(t), (long long)t);
}

static PyObject* Capture_frame_at(PyObject* self, PyObject* arg) {
    CaptureObject* o = CAP_OF(self);
    int64_t t, f;
    if (!cap_ready(o) || au_obj_ns(arg, "t", &t) < 0) return NULL;
    f = yau_capture_frame_at(o->c, t);
    if (f < 0) Py_RETURN_NONE;
    return PyLong_FromLongLong(f);
}

static PyObject* Capture_info(PyObject* self, PyObject* unused) {
    CaptureObject* o = CAP_OF(self);
    yau_capture_info i;
    double r;
    (void)unused;
    if (!cap_ready(o)) return NULL;
    yau_capture_update(o->c);
    yau_capture_get_info(o->c, &i);
    r = i.rate ? (double)i.rate : 1.0;
    return Py_BuildValue("{s:s,s:I,s:i,s:i,s:d,s:d,s:N,s:N,s:s,s:i,s:d,s:d,s:I,s:I,s:I,s:I,s:L,s:L,s:L,s:L,s:N,s:N}",
        "name", i.name, "rate", (unsigned)i.rate, "channels", (int)i.channels, "in", (int)i.in, "period", i.period / r,
        "buffer", i.buffer / r, "exclusive", PyBool_FromLong(i.exclusive), "fit_ready", PyBool_FromLong(i.fit_ready),
        "pos_source", i.pos_source == YAU_POS_DEVICE ? "device" : "callback", "tier", (int)i.tier, "drift_ppm", i.drift_ppm,
        "fit_spread", i.fit_spread_ns / 1e9, "fit_points", (unsigned)i.fit_points, "xruns", (unsigned)i.xruns,
        "drops", (unsigned)i.drops, "set_aside", (unsigned)i.set_aside, "frames", (long long)i.frames,
        "dropped", (long long)i.dropped, "lost", (long long)i.lost, "fill", (long long)i.fill[0],
        "os_latency", au_claim(i.os_latency_ns), "os_stream_latency", au_claim(i.os_stream_latency_ns));
}

static PyObject* Capture_describe(PyObject* self, PyObject* unused) {
    CaptureObject* o = CAP_OF(self);
    char buf[1024];
    (void)unused;
    if (!cap_ready(o)) return NULL;
    yau_capture_describe(o->c, buf, sizeof buf);
    return PyUnicode_FromString(buf);
}

static PyMethodDef Capture_methods[] = {
    { "close", Capture_close, METH_NOARGS, "close(): stop the input." },
    { "__enter__", Capture_enter, METH_NOARGS, NULL },
    { "__exit__", Capture_exit, METH_VARARGS, NULL },
    { "read", Capture_read, METH_VARARGS,
      "read(max_frames=-1) -> (memoryview('f') frames x channels, first_frame): the captured frames up to the next gap "
      "(-1: all waiting). first_frame is the capture frame of row 0." },
    { "time_of", Capture_time_of, METH_O, "time_of(frame) -> (seconds, ns) or None: an int frame, or a float (a detector's crossing)." },
    { "frame_at", Capture_frame_at, METH_O, "frame_at(t) -> int or None: the capture frame at time t." },
    { "info", Capture_info, METH_NOARGS, "info() -> dict: format, fit, tier, losses and drops, fill." },
    { "describe", Capture_describe, METH_NOARGS, "describe() -> str: one line for the log." },
    { NULL, NULL, 0, NULL }
};

static PyType_Slot Capture_slots[] = {
    { Py_tp_doc, "Capture(device=None, *, backend='auto', rate=48000, channels=0, sample='f32', exclusive=False, period=0, "
                 "ring=2.0, offset=0, device_index=0): an input on the ysp_rt clock; every frame has a time from the "
                 "input's own fit. Tier 3 on hardware, SIM on 'null'." },
    { Py_tp_new, PyType_GenericNew },
    { Py_tp_init, Capture_init },
    { Py_tp_dealloc, Capture_dealloc },
    { Py_tp_methods, Capture_methods },
    { 0, NULL }
};
static PyType_Spec Capture_spec = { "ysp.audio.Capture", sizeof(CaptureObject), 0, Py_TPFLAGS_DEFAULT, Capture_slots };

/* --- module functions ---------------------------------------------------------- */

static PyObject* m_now(PyObject* m, PyObject* unused) { (void)m; (void)unused; return PyFloat_FromDouble(au_s(yrt_now_ns())); }
static PyObject* m_now_ns(PyObject* m, PyObject* unused) { (void)m; (void)unused; return PyLong_FromLongLong(yrt_now_ns()); }
static PyObject* m_db(PyObject* m, PyObject* arg) {
    double d = PyFloat_AsDouble(arg);
    (void)m;
    if (d == -1.0 && PyErr_Occurred()) return NULL;
    return PyFloat_FromDouble((double)yau_db((float)d));
}

static ma_backend au_ma_backend(int b, int* ok) {
    *ok = 1;
    switch (b) {
    case YAU_BACKEND_WASAPI: return ma_backend_wasapi;
    case YAU_BACKEND_COREAUDIO: return ma_backend_coreaudio;
    case YAU_BACKEND_ALSA: return ma_backend_alsa;
    case YAU_BACKEND_PULSE: return ma_backend_pulseaudio;
    case YAU_BACKEND_WEB: return ma_backend_webaudio;
    case YAU_BACKEND_NULL: return ma_backend_null;
    default: *ok = 0; return ma_backend_null;
    }
}

/* The header names no devices: desc.device is a part of a name that it
 * matches against miniaudio's list. This lists the same names, through the
 * miniaudio compiled in with the header. */
static PyObject* m_devices(PyObject* m, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "backend", "capture", NULL };
    PyObject *backend = NULL, *list, *names = NULL;
    int capture = 0, b = 0, ok;
    ma_backend mb;
    ma_context ctx;
    ma_device_info *pi = NULL, *ci = NULL, *infos;
    ma_uint32 pn = 0, cn = 0, n, i;
    ma_result r;
    (void)m;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|Op", kwlist, &backend, &capture)) return NULL;
    if (au_lookup(backend, au_backends, "backend", &b) < 0) return NULL;
    mb = au_ma_backend(b, &ok);
    Py_BEGIN_ALLOW_THREADS
    r = ma_context_init(ok ? &mb : NULL, ok ? 1 : 0, NULL, &ctx);
    if (r == MA_SUCCESS) r = ma_context_get_devices(&ctx, &pi, &pn, &ci, &cn);
    Py_END_ALLOW_THREADS
    if (r != MA_SUCCESS) {
        PyErr_Format(AuError, "ysp_audio: no device list (%s)", ma_result_description(r));
        return NULL;
    }
    infos = capture ? ci : pi;
    n = capture ? cn : pn;
    list = PyList_New(0);
    for (i = 0; list && i < n; i++) {
        PyObject* d = Py_BuildValue("{s:s,s:N,s:s}", "name", infos[i].name, "default", PyBool_FromLong(infos[i].isDefault),
                                    "backend", ma_get_backend_name(ctx.backend));
        if (!d || PyList_Append(list, d) < 0) { Py_XDECREF(d); Py_CLEAR(list); break; }
        Py_DECREF(d);
    }
    (void)names;
    ma_context_uninit(&ctx);
    return list;
}

static PyObject* m_wav_info(PyObject* m, PyObject* arg) {
    PyObject* pb;
    yau_wav_desc d;
    yau_wav_info info;
    char err[256];
    int rc;
    (void)m;
    if (!(pb = au_path(arg))) return NULL;
    memset(&d, 0, sizeof d);
    d.path = PyBytes_AsString(pb);
    err[0] = 0;
    Py_BEGIN_ALLOW_THREADS
    rc = yau_wav_probe(&d, &info, err, sizeof err);
    Py_END_ALLOW_THREADS
    Py_DECREF(pb);
    if (rc < 0) { PyErr_SetString(au_exc(rc), err[0] ? err : yau_strerror(rc)); return NULL; }
    return Py_BuildValue("{s:I,s:i,s:i,s:L,s:d,s:k,s:N}", "rate", (unsigned)info.rate, "channels", (int)info.channels,
                         "format", (int)info.format, "frames", (long long)info.frames,
                         "duration", info.rate ? (double)info.frames / info.rate : 0.0, "channel_mask", (unsigned long)info.channel_mask,
                         "rf64", PyBool_FromLong(info.rf64));
}

static PyMethodDef module_methods[] = {
    { "now", m_now, METH_NOARGS, "now() -> seconds on the ysp_rt clock (yrt_now_ns()), the clock of every time here." },
    { "now_ns", m_now_ns, METH_NOARGS, "now_ns() -> int: the same, exact ns." },
    { "db", m_db, METH_O, "db(x) -> the linear gain of x dB: db(-30) is 0.0316." },
    { "devices", (PyCFunction)(void (*)(void))m_devices, METH_VARARGS | METH_KEYWORDS,
      "devices(backend='auto', capture=False) -> [ {name, default, backend} ]: the names Audio(device=...) matches." },
    { "wav_info", m_wav_info, METH_O, "wav_info(path) -> dict: a WAV file's form without a device; the header's refusals." },
    { NULL, NULL, 0, NULL }
};

static struct PyModuleDef ysp_audio_module = {
    PyModuleDef_HEAD_INIT, "ysp.audio",
    "Sound at a time on the ysp_rt clock (ysp/audio.h): buffers, streams and WAV files played at a time, with the record "
    "of where each landed and its tier; an input on the same clock.", -1, module_methods,
    NULL, NULL, NULL, NULL,
};

static PyObject* au_add_exc(PyObject* m, const char* qual, const char* attr, PyObject* base) {
    PyObject* e = PyErr_NewException(qual, base, NULL);
    if (!e) return NULL;
    Py_INCREF(e);
    if (PyModule_AddObject(m, attr, e) < 0) { Py_DECREF(e); Py_DECREF(e); return NULL; }
    return e;
}

static PyObject* au_add_type(PyObject* m, PyType_Spec* spec, const char* name) {
    PyObject* t = PyType_FromSpec(spec);
    if (!t) return NULL;
    Py_INCREF(t);
    if (PyModule_AddObject(m, name, t) < 0) { Py_DECREF(t); Py_DECREF(t); return NULL; }
    return t;
}

PyMODINIT_FUNC PyInit_audio(void) {
    PyObject *m, *b2, *cap;
    int i;
    static const struct { const char* name; long v; } consts[] = {
        { "TIER_UNKNOWN", YAU_TIER_UNKNOWN }, { "TIER_1", YAU_TIER_1 }, { "TIER_2", YAU_TIER_2 }, { "TIER_3", YAU_TIER_3 },
        { "TIER_SIM", YAU_TIER_SIM }, { "TIER_WEB", YAU_TIER_WEB },
        { "PENDING", YAU_ONSET_PENDING }, { "LATE", YAU_ONSET_LATE }, { "CANCELED", YAU_ONSET_CANCELED },
        { "STOPPED", YAU_ONSET_STOPPED }, { "XRUN", YAU_ONSET_XRUN }, { "CLIPPED", YAU_ONSET_CLIPPED },
        { "UNCONFIRMED", YAU_ONSET_UNCONFIRMED }, { "BELOW_TIER", YAU_ONSET_BELOW_TIER }, { "LOST", YAU_ONSET_LOST },
        { "GAP", YAU_ONSET_GAP }, { "FOREVER", YAU_FOREVER }, { "MAX_VOICES", YAU_MAX_VOICES },
        { "MAX_CHANNELS", YAU_MAX_CHANNELS }, { "WAV_S16", YAU_WAV_S16 }, { "WAV_S24", YAU_WAV_S24 },
        { "WAV_S24_32", YAU_WAV_S24_32 }, { "WAV_F32", YAU_WAV_F32 },
    };
    m = PyModule_Create(&ysp_audio_module);
    if (!m) return NULL;
    if (!(AudioType = au_add_type(m, &Audio_spec, "Audio"))) goto fail;
    if (!(BufferType = au_add_type(m, &Buffer_spec, "Buffer"))) goto fail;
    if (!(StreamType = au_add_type(m, &Stream_spec, "Stream"))) goto fail;
    if (!(WavType = au_add_type(m, &Wav_spec, "Wav"))) goto fail;
    if (!(CaptureType = au_add_type(m, &Capture_spec, "Capture"))) goto fail;
    if (!(OnsetType = (PyObject*)PyStructSequence_NewType(&au_onset_desc))) goto fail;
    Py_INCREF(OnsetType);
    if (PyModule_AddObject(m, "Onset", OnsetType) < 0) { Py_DECREF(OnsetType); goto fail; }
    if (!(AuError = au_add_exc(m, "ysp.audio.Error", "Error", NULL))) goto fail;
    if (!(b2 = PyTuple_Pack(2, AuError, PyExc_ValueError))) goto fail;
    AuArgumentError = au_add_exc(m, "ysp.audio.ArgumentError", "ArgumentError", b2);
    Py_DECREF(b2);
    AuClosedError = au_add_exc(m, "ysp.audio.ClosedError", "ClosedError", AuError);
    AuFullError = au_add_exc(m, "ysp.audio.FullError", "FullError", AuError);
    AuFormatError = au_add_exc(m, "ysp.audio.FormatError", "FormatError", AuError);
    AuNotFoundError = au_add_exc(m, "ysp.audio.NotFoundError", "NotFoundError", AuError);
    AuLostError = au_add_exc(m, "ysp.audio.LostError", "LostError", AuError);
    AuBusyError = au_add_exc(m, "ysp.audio.BusyError", "BusyError", AuError);
    if (!(b2 = PyTuple_Pack(2, AuError, PyExc_TimeoutError))) goto fail;
    AuTimeoutError = au_add_exc(m, "ysp.audio.TimeoutError", "TimeoutError", b2);
    Py_DECREF(b2);
    if (!(b2 = PyTuple_Pack(2, AuError, PyExc_OSError))) goto fail;
    AuIOError = au_add_exc(m, "ysp.audio.IOError", "IOError", b2);
    Py_DECREF(b2);
    if (!AuArgumentError || !AuClosedError || !AuFullError || !AuFormatError || !AuNotFoundError || !AuLostError ||
        !AuBusyError || !AuTimeoutError || !AuIOError)
        goto fail;
    for (i = 0; i < (int)(sizeof consts / sizeof consts[0]); i++)
        if (PyModule_AddIntConstant(m, consts[i].name, consts[i].v) < 0) goto fail;
    if (PyModule_AddStringConstant(m, "__version__", AU_VERSION) < 0) goto fail;
    ysp_audio_api_fill(&g_api, YSP_SHARE_HOST_PYTHON, au_api_acquire, au_api_release);
    cap = PyCapsule_New(&g_api, YSP_AUDIO_API_CAPSULE, NULL);
    if (!cap || PyModule_AddObject(m, "_C_API", cap) < 0) { Py_XDECREF(cap); goto fail; }
    return m;
fail:
    Py_DECREF(m);
    return NULL;
}
