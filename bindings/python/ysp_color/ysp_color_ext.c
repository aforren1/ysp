/* ysp_color_ext.c - CPython extension wrapping ysp/color.h (module ysp.color)
 *
 * A thin, dependency-free binding: it needs only Python.h, and compiles the
 * header's implementation into the module. Built against the stable ABI /
 * Limited API (CPython 3.8+), so the types are heap types made with
 * PyType_FromSpec, and arrays come in through memoryview and bytes (a copy)
 * and go out as bytes cast to a 'd' memoryview, as ysp.aep does.
 *
 *     import ysp.color as pc
 *     cal = pc.Calibration.load(open("rig.yspcal", "rb").read())
 *     cx = pc.Context(cal, background=(0.5, 0.5, 0.5))
 *     rgb, g = cx.to_rgb(pc.dkl(elev=0, azim=90, contrast=0.1))
 *     lab = cx.from_rgb(rgb, "cielab")
 *     k = cx.max_scale(pc.dkl(azim=90, contrast=1), symmetric=True)
 *     sh = cx.michelson(0.03)          # gfx's contrast, codes, contrast shown
 *
 * A Color is a namedtuple (space, a, b, c): the space's name ("dkl") and
 * its three values in the header's order. A Gamut and a Shown are
 * namedtuples too.
 *
 * THREADING: none. Every call is arithmetic on objects the GIL guards; a
 * Context holds references to its Calibration and Lum, so the pointers the
 * header keeps stay valid.
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x03080000
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define YSP_COLOR_IMPLEMENTATION
#include "ysp/color.h"

#include <string.h>

static PyObject* ColError;
static PyObject* ColArgumentError;
static PyObject* ColRefusedError;
static PyObject* ColFormatError;
static PyObject* ColRangeError;
static PyObject* ColorType;   /* namedtuple Color(space, a, b, c) */
static PyObject* GamutType;   /* namedtuple Gamut(...) */
static PyObject* ShownType;   /* namedtuple Shown(...) */
static PyObject* CalType;
static PyObject* LumType;

static PyObject* col_fail(int code, const char* msg) {
    PyObject* exc = ColError;
    if (code == YCOL_ERR_ARG) exc = ColArgumentError;
    else if (code == YCOL_ERR_REFUSED) exc = ColRefusedError;
    else if (code == YCOL_ERR_FORMAT) exc = ColFormatError;
    else if (code == YCOL_ERR_RANGE) exc = ColRangeError;
    PyErr_SetString(exc, msg && msg[0] ? msg : ycol_strerror(code));
    return NULL;
}

/* --- names ------------------------------------------------------------------ */

/* A str's UTF-8 into buf (PyUnicode_AsUTF8 is Limited API only from 3.10). */
static int col_utf8(PyObject* o, char* buf, size_t cap) {
    PyObject* b = PyUnicode_AsUTF8String(o);
    if (!b) return -1;
    PyOS_snprintf(buf, cap, "%s", PyBytes_AsString(b));
    Py_DECREF(b);
    return 0;
}

static int col_space(PyObject* o) {
    if (PyLong_Check(o)) {
        long v = PyLong_AsLong(o);
        if (v > YCOL_SPACE_NONE && v < YCOL_SPACE_COUNT) return (int)v;
    } else if (PyUnicode_Check(o)) {
        char s[64];
        int sp = col_utf8(o, s, sizeof s) == 0 ? ycol_space_from_name(s) : 0;
        if (sp) return sp;
    }
    if (!PyErr_Occurred()) PyErr_SetString(ColArgumentError, "not a color space (a name such as \"dkl\" or a SPACE_* value)");
    return -1;
}

static const char* col_space_name(int sp) {
    int n;
    const ycol_space_info* si = ycol_spaces(&n);
    return sp > 0 && sp <= n ? si[sp - 1].name : "none";
}

static int col_named(PyObject* o, const char* const* names, int n, const char* what) {
    int i;
    if (PyLong_Check(o)) {
        long v = PyLong_AsLong(o);
        if (v >= 0 && v < n) return (int)v;
    } else if (PyUnicode_Check(o)) {
        char s[64];
        if (col_utf8(o, s, sizeof s) == 0)
            for (i = 0; i < n; i++) if (names[i] && strcmp(s, names[i]) == 0) return i;
    }
    if (!PyErr_Occurred()) PyErr_Format(ColArgumentError, "not a known %s", what);
    return -1;
}

static const char* const col_cones_names[] = { "ss2", "ss10" };
static const char* const col_adapt_names[] = { "none", "bradford" };
static const char* const col_method_names[] = { NULL, "hfp", "min_motion", "min_border", "other" };
static const char* const col_map_names[] = { NULL, "scale", "chroma_oklch", "chroma_cielch", "chroma_dkl", "clip" };
static const char* const col_plane_names[] = { NULL, "dkl", "cone", "oklch", "cielch" };

/* --- colors and triples ------------------------------------------------------ */

static PyObject* col_color(const ycol_color* c) {
    return PyObject_CallFunction(ColorType, "sddd", col_space_name(c->space), c->u.v[0], c->u.v[1], c->u.v[2]);
}

/* A Color, or any (space, a, b, c) sequence. */
static int col_parse_color(PyObject* o, ycol_color* c) {
    PyObject* it[4];
    double v[3];
    int sp, i;
    if (!PySequence_Check(o) || PySequence_Size(o) != 4) {
        PyErr_SetString(PyExc_TypeError, "a color is a Color or a (space, a, b, c) sequence");
        return -1;
    }
    for (i = 0; i < 4; i++) {
        it[i] = PySequence_GetItem(o, i);
        if (!it[i]) { while (i-- > 0) Py_DECREF(it[i]); return -1; }
    }
    sp = col_space(it[0]);
    for (i = 0; i < 3; i++) v[i] = PyFloat_AsDouble(it[i + 1]);
    for (i = 0; i < 4; i++) Py_DECREF(it[i]);
    if (sp < 0 || PyErr_Occurred()) return -1;
    *c = ycol_make(sp, v[0], v[1], v[2]);
    return 0;
}

static int col_triple(PyObject* o, double v[3], const char* name) {
    int i;
    if (!PySequence_Check(o) || PySequence_Size(o) != 3) {
        PyErr_Format(PyExc_TypeError, "%s must be a sequence of three numbers", name);
        return -1;
    }
    for (i = 0; i < 3; i++) {
        PyObject* x = PySequence_GetItem(o, i);
        if (!x) return -1;
        v[i] = PyFloat_AsDouble(x);
        Py_DECREF(x);
        if (PyErr_Occurred()) return -1;
    }
    return 0;
}

static PyObject* col_tuple3(const double* v) { return Py_BuildValue("(ddd)", v[0], v[1], v[2]); }

static PyObject* col_gamut(const ycol_gamut* g) {
    return PyObject_CallFunction(GamutType, "iOkk(ddd)dddz", g->status, g->in ? Py_True : Py_False,
                                 (unsigned long)g->below, (unsigned long)g->above, g->margin[0], g->margin[1],
                                 g->margin[2], g->distance, g->scale, g->kept, g->why);
}

/* n x 3 doubles from a buffer of doubles (NumPy, array.array('d')) or a
 * sequence of triples. PyMem array; caller frees. */
static double* col_rows(PyObject* obj, Py_ssize_t* rows) {
    PyObject* mv;
    double* out;
    Py_ssize_t n, i;
    mv = PyMemoryView_FromObject(obj);
    if (mv) {
        PyObject* f = PyObject_GetAttrString(mv, "format");
        int dbl = f && PyUnicode_Check(f) && (PyUnicode_CompareWithASCIIString(f, "d") == 0 ||
                                             PyUnicode_CompareWithASCIIString(f, "@d") == 0 ||
                                             PyUnicode_CompareWithASCIIString(f, "<d") == 0 ||
                                             PyUnicode_CompareWithASCIIString(f, "=d") == 0);
        Py_XDECREF(f);
        PyErr_Clear();
        if (dbl) {
            PyObject* b = PyBytes_FromObject(mv);
            Py_DECREF(mv);
            if (!b) return NULL;
            n = PyBytes_Size(b) / (Py_ssize_t)sizeof(double);
            if (n % 3) { Py_DECREF(b); PyErr_SetString(ColArgumentError, "values: not a multiple of 3 numbers"); return NULL; }
            out = (double*)PyMem_Malloc((size_t)(n ? n : 1) * sizeof(double));
            if (!out) { Py_DECREF(b); PyErr_NoMemory(); return NULL; }
            memcpy(out, PyBytes_AsString(b), (size_t)n * sizeof(double));
            Py_DECREF(b);
            *rows = n / 3;
            return out;
        }
        Py_DECREF(mv);
    }
    PyErr_Clear();
    n = PySequence_Size(obj);
    if (n < 0) { PyErr_SetString(PyExc_TypeError, "values must be a buffer of doubles or a sequence of triples"); return NULL; }
    out = (double*)PyMem_Malloc((size_t)(n ? n : 1) * 3 * sizeof(double));
    if (!out) { PyErr_NoMemory(); return NULL; }
    for (i = 0; i < n; i++) {
        PyObject* row = PySequence_GetItem(obj, i);
        int rc = row ? col_triple(row, out + 3 * i, "each row") : -1;
        Py_XDECREF(row);
        if (rc < 0) { PyMem_Free(out); return NULL; }
    }
    *rows = n;
    return out;
}

static PyObject* col_view(const void* data, Py_ssize_t bytes, const char* fmt) {
    PyObject* b = PyBytes_FromStringAndSize((const char*)data, bytes);
    PyObject *mv, *cast;
    if (!b) return NULL;
    mv = PyMemoryView_FromObject(b);
    Py_DECREF(b);
    if (!mv) return NULL;
    cast = PyObject_CallMethod(mv, "cast", "s", fmt);
    Py_DECREF(mv);
    return cast;
}

/* --- Calibration ------------------------------------------------------------- */

typedef struct { PyObject_HEAD ycol_cal c; } CalObject;
#define AS_CAL(o) (&((CalObject*)(o))->c)

static int Cal_init(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { NULL };
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "", kw)) return -1;
    ycol_cal_init(AS_CAL(self));
    return 0;
}

static void col_dealloc(PyObject* self) {
    PyTypeObject* tp = Py_TYPE(self);
    freefunc f = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    f(self);
    Py_DECREF(tp);
}

static PyObject* Cal_load(PyObject* cls, PyObject* arg) {
    char err[256];
    PyObject* b = PyBytes_FromObject(arg);
    PyObject* self;
    int rc;
    if (!b) return NULL;
    self = PyObject_CallObject(cls, NULL);
    if (!self) { Py_DECREF(b); return NULL; }
    rc = ycol_cal_load(AS_CAL(self), PyBytes_AsString(b), (size_t)PyBytes_Size(b), err, sizeof err);
    Py_DECREF(b);
    if (rc < 0) { Py_DECREF(self); return col_fail(rc, err); }
    return self;
}

static PyObject* Cal_nominal(PyObject* cls, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "xy", "white_Y", "gamma", NULL };
    PyObject *xy, *self;
    double white_Y, gamma;
    float fxy[4][2];
    int i, rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "Odd", kw, &xy, &white_Y, &gamma)) return NULL;
    if (!PySequence_Check(xy) || PySequence_Size(xy) != 4) {
        PyErr_SetString(PyExc_TypeError, "xy: four (x, y) pairs, R, G, B and white");
        return NULL;
    }
    for (i = 0; i < 4; i++) {
        PyObject* p = PySequence_GetItem(xy, i);
        double a = 0, b = 0;
        if (!p || !PyArg_ParseTuple(p, "dd", &a, &b)) { Py_XDECREF(p); return NULL; }
        Py_DECREF(p);
        fxy[i][0] = (float)a; fxy[i][1] = (float)b;
    }
    self = PyObject_CallObject(cls, NULL);
    if (!self) return NULL;
    rc = ycol_cal_nominal(AS_CAL(self), (const float(*)[2])fxy, (float)white_Y, gamma);
    if (rc < 0) { Py_DECREF(self); return col_fail(rc, "the stated primaries and gamma give no calibration"); }
    return self;
}

static PyObject* Cal_add(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "gun", "level", "Y", "x", "y", NULL };
    int gun, rc;
    double level, Y, x = 0, y = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "idd|dd", kw, &gun, &level, &Y, &x, &y)) return NULL;
    rc = ycol_cal_add(AS_CAL(self), gun, (float)level, (float)Y, (float)x, (float)y);
    if (rc < 0) return col_fail(rc, "a reading: gun -1 (black), 0, 1, 2 or 3 (white); level 0..1; Y >= 0");
    Py_RETURN_NONE;
}

static float* col_floats(PyObject* o, int n, const char* name) {
    float* f;
    int i;
    if (!PySequence_Check(o) || PySequence_Size(o) != n) {
        PyErr_Format(PyExc_TypeError, "%s must have as many samples as r", name);
        return NULL;
    }
    f = (float*)PyMem_Malloc((size_t)n * sizeof(float));
    if (!f) { PyErr_NoMemory(); return NULL; }
    for (i = 0; i < n; i++) {
        PyObject* x = PySequence_GetItem(o, i);
        double v = x ? PyFloat_AsDouble(x) : 0;
        Py_XDECREF(x);
        if (!x || PyErr_Occurred()) { PyMem_Free(f); return NULL; }
        f[i] = (float)v;
    }
    return f;
}

static PyObject* Cal_set_spectra(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "wl_start", "wl_step", "r", "g", "b", "black", "nominal", NULL };
    double start, step;
    PyObject *r, *g, *b, *k = Py_None;
    int nominal = 0, n, rc;
    float *fr = NULL, *fg = NULL, *fb = NULL, *fk = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "ddOOO|Op", kw, &start, &step, &r, &g, &b, &k, &nominal)) return NULL;
    n = (int)PySequence_Size(r);
    if (n < 0) return NULL;
    fr = col_floats(r, n, "r"); fg = fr ? col_floats(g, n, "g") : NULL; fb = fg ? col_floats(b, n, "b") : NULL;
    if (fb && k != Py_None) fk = col_floats(k, n, "black");
    if (!fb || (k != Py_None && !fk)) { PyMem_Free(fr); PyMem_Free(fg); PyMem_Free(fb); return NULL; }
    rc = nominal ? ycol_cal_set_spectra_nominal(AS_CAL(self), (float)start, (float)step, n, fr, fg, fb, fk)
                 : ycol_cal_set_spectra(AS_CAL(self), (float)start, (float)step, n, fr, fg, fb, fk);
    PyMem_Free(fr); PyMem_Free(fg); PyMem_Free(fb); PyMem_Free(fk);
    if (rc < 0) return col_fail(rc, "spectra: 2 to 471 samples, a step above 0");
    Py_RETURN_NONE;
}

static PyObject* Cal_derive(PyObject* self, PyObject* Py_UNUSED(a)) {
    char err[256];
    int rc = ycol_cal_derive(AS_CAL(self), err, sizeof err);
    if (rc < 0) return col_fail(rc, err);
    Py_RETURN_NONE;
}

static PyObject* Cal_check(PyObject* self, PyObject* Py_UNUSED(a)) {
    char err[256];
    int rc = ycol_cal_check(AS_CAL(self), err, sizeof err);
    if (rc < 0) return col_fail(rc, err);
    Py_RETURN_NONE;
}

static PyObject* Cal_save(PyObject* self, PyObject* Py_UNUSED(a)) {
    ycol_cal_save(AS_CAL(self), NULL, 0);
    return PyBytes_FromStringAndSize((const char*)AS_CAL(self), (Py_ssize_t)sizeof(ycol_cal));
}

static PyObject* Cal_describe(PyObject* self, PyObject* Py_UNUSED(a)) {
    char buf[512];
    ycol_cal_describe(AS_CAL(self), buf, sizeof buf);
    return PyUnicode_FromString(buf);
}

static PyObject* Cal_get(PyObject* self, void* which) {
    const ycol_cal* c = AS_CAL(self);
    switch ((int)(intptr_t)which) {
    case 0: return PyLong_FromUnsignedLong(c->flags);
    case 1: return PyLong_FromUnsignedLong(c->crc);
    case 2: return Py_BuildValue("(ddddddddd)", c->rgb_to_xyz[0], c->rgb_to_xyz[1], c->rgb_to_xyz[2], c->rgb_to_xyz[3],
                                 c->rgb_to_xyz[4], c->rgb_to_xyz[5], c->rgb_to_xyz[6], c->rgb_to_xyz[7], c->rgb_to_xyz[8]);
    case 3: return Py_BuildValue("(ddddddddd)", c->rgb_to_lms[0], c->rgb_to_lms[1], c->rgb_to_lms[2], c->rgb_to_lms[3],
                                 c->rgb_to_lms[4], c->rgb_to_lms[5], c->rgb_to_lms[6], c->rgb_to_lms[7], c->rgb_to_lms[8]);
    case 4: return col_tuple3(c->black_xyz);
    case 5: return col_tuple3(c->black_lms);
    case 6: {   /* 3 x lut_n floats, gun by gun */
        int n = c->lut_n ? c->lut_n : YCOL_CAL_MAX_LUT, k;
        float* t = (float*)PyMem_Malloc((size_t)n * 3 * sizeof(float));
        PyObject* v;
        if (!t) return PyErr_NoMemory();
        for (k = 0; k < 3; k++) memcpy(t + (size_t)k * n, c->lut[k], (size_t)n * sizeof(float));
        v = col_view(t, (Py_ssize_t)n * 3 * (Py_ssize_t)sizeof(float), "f");
        PyMem_Free(t);
        return v;
    }
    case 7: return PyLong_FromLong(c->n_readings);
    case 8: return PyFloat_FromDouble(c->white_err);
    default: Py_RETURN_NONE;
    }
}

static PyMethodDef Cal_methods[] = {
    { "load", (PyCFunction)Cal_load, METH_O | METH_CLASS, "Calibration.load(data) -> Calibration: the bytes of a .yspcal file, checked." },
    { "nominal", (PyCFunction)(void (*)(void))Cal_nominal, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
      "Calibration.nominal(xy, white_Y, gamma) -> Calibration from stated chromaticities (R, G, B, white), flagged NOMINAL." },
    { "add", (PyCFunction)(void (*)(void))Cal_add, METH_VARARGS | METH_KEYWORDS, "add(gun, level, Y, x=0, y=0): one reading." },
    { "set_spectra", (PyCFunction)(void (*)(void))Cal_set_spectra, METH_VARARGS | METH_KEYWORDS,
      "set_spectra(wl_start, wl_step, r, g, b, black=None, nominal=False): the primaries' spectra." },
    { "derive", Cal_derive, METH_NOARGS, "derive(): the CLUT and the matrices from the readings; seals the CRC." },
    { "check", Cal_check, METH_NOARGS, "check(): the CRC and the derived fields against the readings." },
    { "save", Cal_save, METH_NOARGS, "save() -> bytes: the canonical .yspcal bytes." },
    { "describe", Cal_describe, METH_NOARGS, "describe() -> str: one line for the log." },
    { NULL }
};

static PyGetSetDef Cal_getset[] = {
    { "flags", Cal_get, NULL, "CAL_* flags", (void*)0 },
    { "crc", Cal_get, NULL, "the CRC-32", (void*)1 },
    { "rgb_to_xyz", Cal_get, NULL, "9 numbers, row-major, black removed", (void*)2 },
    { "rgb_to_lms", Cal_get, NULL, "9 numbers, row-major, black removed (2 degree cones)", (void*)3 },
    { "black_xyz", Cal_get, NULL, "the black's XYZ", (void*)4 },
    { "black_lms", Cal_get, NULL, "the black's LMS", (void*)5 },
    { "lut", Cal_get, NULL, "the CLUT: memoryview('f') of 3 x n, gun by gun", (void*)6 },
    { "n_readings", Cal_get, NULL, "readings", (void*)7 },
    { "white_err", Cal_get, NULL, "white minus the sum of the guns, percent", (void*)8 },
    { NULL }
};

static PyType_Slot Cal_slots[] = {
    { Py_tp_init, (void*)Cal_init }, { Py_tp_dealloc, (void*)col_dealloc }, { Py_tp_methods, Cal_methods },
    { Py_tp_getset, Cal_getset }, { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_doc, (void*)"A display calibration: readings, spectra and what derive() makes of them (ycol_cal)." },
    { 0, NULL }
};
static PyType_Spec Cal_spec = { "ysp.color.Calibration", sizeof(CalObject), 0, Py_TPFLAGS_DEFAULT, Cal_slots };

/* --- Lum --------------------------------------------------------------------- */

typedef struct { PyObject_HEAD ycol_lum l; } LumObject;
#define AS_LUM(o) (&((LumObject*)(o))->l)

static int Lum_init(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "cones", "method", "participant", "field_deg", "ecc_deg", "freq_hz", "fit_s", NULL };
    PyObject *cones_o = NULL, *method_o = NULL;
    const char* who = "";
    double field = 0, ecc = 0, freq = 0;
    int fit_s = 0, cones = 0, method = YCOL_LUM_HFP;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$OOsdddp", kw, &cones_o, &method_o, &who, &field, &ecc, &freq, &fit_s))
        return -1;
    if (cones_o && (cones = col_named(cones_o, col_cones_names, 2, "cones (\"ss2\", \"ss10\")")) < 0) return -1;
    if (method_o && (method = col_named(method_o, col_method_names, 5, "method (\"hfp\", \"min_motion\", \"min_border\", \"other\")")) < 0)
        return -1;
    ycol_lum_init(AS_LUM(self), cones, method);
    PyOS_snprintf(AS_LUM(self)->participant, sizeof AS_LUM(self)->participant, "%s", who);
    AS_LUM(self)->field_deg = (float)field; AS_LUM(self)->ecc_deg = (float)ecc; AS_LUM(self)->freq_hz = (float)freq;
    if (fit_s) AS_LUM(self)->flags |= YCOL_LUM_FIT_S;
    return 0;
}

static PyObject* Lum_load(PyObject* cls, PyObject* arg) {
    char err[256];
    PyObject* b = PyBytes_FromObject(arg);
    PyObject* self;
    int rc;
    if (!b) return NULL;
    self = PyObject_CallObject(cls, NULL);
    if (!self) { Py_DECREF(b); return NULL; }
    rc = ycol_lum_load(AS_LUM(self), PyBytes_AsString(b), (size_t)PyBytes_Size(b), err, sizeof err);
    Py_DECREF(b);
    if (rc < 0) { Py_DECREF(self); return col_fail(rc, err); }
    return self;
}

static PyObject* Lum_add(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "a", "b", "bg", "sd", "n", NULL };
    PyObject *ao, *bo, *go = Py_None;
    double a[3], b[3], g[3], sd = 0;
    int n = 1, rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO|Odi", kw, &ao, &bo, &go, &sd, &n)) return NULL;
    if (col_triple(ao, a, "a") < 0 || col_triple(bo, b, "b") < 0) return NULL;
    if (go != Py_None && col_triple(go, g, "bg") < 0) return NULL;
    rc = ycol_lum_add(AS_LUM(self), a, b, go != Py_None ? g : NULL, sd, n);
    if (rc < 0) return col_fail(rc, rc == YCOL_ERR_FULL ? "16 settings at most" : "a setting: finite lights, n >= 0");
    Py_RETURN_NONE;
}

static PyObject* Lum_derive(PyObject* self, PyObject* cal) {
    char err[256];
    int rc;
    if (!PyObject_TypeCheck(cal, (PyTypeObject*)CalType)) { PyErr_SetString(PyExc_TypeError, "derive(cal): a Calibration"); return NULL; }
    rc = ycol_lum_derive(AS_LUM(self), AS_CAL(cal), err, sizeof err);
    if (rc < 0) return col_fail(rc, err);
    Py_RETURN_NONE;
}

static PyObject* Lum_stated(PyObject* cls, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "w", "cones", NULL };
    PyObject *wo, *co = NULL, *self;
    double w[3];
    int cones = 0, rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O", kw, &wo, &co)) return NULL;
    if (col_triple(wo, w, "w") < 0) return NULL;
    if (co && (cones = col_named(co, col_cones_names, 2, "cones")) < 0) return NULL;
    self = PyObject_CallObject(cls, NULL);
    if (!self) return NULL;
    rc = ycol_lum_stated(AS_LUM(self), cones, w);
    if (rc < 0) { Py_DECREF(self); return col_fail(rc, "stated weights: L and M above 0"); }
    return self;
}

static PyObject* Lum_check(PyObject* self, PyObject* args) {
    PyObject* cal = Py_None;
    char err[256];
    int rc;
    if (!PyArg_ParseTuple(args, "|O", &cal)) return NULL;
    if (cal != Py_None && !PyObject_TypeCheck(cal, (PyTypeObject*)CalType)) { PyErr_SetString(PyExc_TypeError, "check(cal=None)"); return NULL; }
    rc = ycol_lum_check(AS_LUM(self), cal != Py_None ? AS_CAL(cal) : NULL, err, sizeof err);
    if (rc < 0) return col_fail(rc, err);
    Py_RETURN_NONE;
}

static PyObject* Lum_save(PyObject* self, PyObject* Py_UNUSED(a)) {
    ycol_lum_save(AS_LUM(self), NULL, 0);
    return PyBytes_FromStringAndSize((const char*)AS_LUM(self), (Py_ssize_t)sizeof(ycol_lum));
}

static PyObject* Lum_describe(PyObject* self, PyObject* Py_UNUSED(a)) {
    char buf[512];
    ycol_lum_describe(AS_LUM(self), buf, sizeof buf);
    return PyUnicode_FromString(buf);
}

static PyObject* Lum_get(PyObject* self, void* which) {
    const ycol_lum* l = AS_LUM(self);
    int i;
    switch ((int)(intptr_t)which) {
    case 0: return col_tuple3(l->w);
    case 1: {
        PyObject* t = PyTuple_New(l->n_settings > 0 ? l->n_settings : 0);
        for (i = 0; t && i < l->n_settings; i++) PyTuple_SetItem(t, i, PyFloat_FromDouble(l->resid[i]));
        return t;
    }
    case 2: return PyLong_FromUnsignedLong(l->flags);
    case 3: return PyLong_FromUnsignedLong(l->crc);
    case 4: return PyLong_FromUnsignedLong(l->cal_crc);
    case 5: return PyUnicode_FromString(col_cones_names[l->cones == YCOL_CONES_SS10]);
    case 6: return PyUnicode_FromString(l->participant);
    default: Py_RETURN_NONE;
    }
}

static PyMethodDef Lum_methods[] = {
    { "load", (PyCFunction)Lum_load, METH_O | METH_CLASS, "Lum.load(data) -> Lum: the bytes of a .ysplum file." },
    { "stated", (PyCFunction)(void (*)(void))Lum_stated, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
      "Lum.stated(w, cones=\"ss2\") -> Lum: published weights, flagged STATED." },
    { "add", (PyCFunction)(void (*)(void))Lum_add, METH_VARARGS | METH_KEYWORDS,
      "add(a, b, bg=None, sd=0, n=1): the two lights at a null, linear device RGB." },
    { "derive", Lum_derive, METH_O, "derive(cal): fit the weights on the settings' calibration." },
    { "check", Lum_check, METH_VARARGS, "check(cal=None): the CRC, and with cal the fit again." },
    { "save", Lum_save, METH_NOARGS, "save() -> bytes: the canonical .ysplum bytes." },
    { "describe", Lum_describe, METH_NOARGS, "describe() -> str: one line for the log." },
    { NULL }
};

static PyGetSetDef Lum_getset[] = {
    { "w", Lum_get, NULL, "the luminance weights (L, M, S)", (void*)0 },
    { "resid", Lum_get, NULL, "each setting's luminance contrast left", (void*)1 },
    { "flags", Lum_get, NULL, "LUM_* flags", (void*)2 },
    { "crc", Lum_get, NULL, "the CRC-32", (void*)3 },
    { "cal_crc", Lum_get, NULL, "the settings' calibration", (void*)4 },
    { "cones", Lum_get, NULL, "\"ss2\" or \"ss10\"", (void*)5 },
    { "participant", Lum_get, NULL, "the participant's label", (void*)6 },
    { NULL }
};

static PyType_Slot Lum_slots[] = {
    { Py_tp_init, (void*)Lum_init }, { Py_tp_dealloc, (void*)col_dealloc }, { Py_tp_methods, Lum_methods },
    { Py_tp_getset, Lum_getset }, { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_doc, (void*)"A participant's luminance from flicker photometry (ycol_lum)." },
    { 0, NULL }
};
static PyType_Spec Lum_spec = { "ysp.color.Lum", sizeof(LumObject), 0, Py_TPFLAGS_DEFAULT, Lum_slots };

/* --- Context ----------------------------------------------------------------- */

typedef struct { PyObject_HEAD ycol_ctx cx; PyObject* cal; PyObject* lum; } CtxObject;
#define AS_CTX(o) (&((CtxObject*)(o))->cx)

static int Ctx_init(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "cal", "background", "cones", "lum", "white", "src_white_Y", "adapt", NULL };
    CtxObject* o = (CtxObject*)self;
    PyObject *cal, *bg = NULL, *cones_o = NULL, *lum = Py_None, *white = Py_None, *adapt_o = NULL;
    ycol_ctx_desc d;
    memset(&d, 0, sizeof d);
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O$OOOdO", kw, &cal, &bg, &cones_o, &lum, &white, &d.src_white_Y, &adapt_o))
        return -1;
    if (!PyObject_TypeCheck(cal, (PyTypeObject*)CalType)) { PyErr_SetString(PyExc_TypeError, "cal must be a Calibration"); return -1; }
    if (lum != Py_None && !PyObject_TypeCheck(lum, (PyTypeObject*)LumType)) { PyErr_SetString(PyExc_TypeError, "lum must be a Lum or None"); return -1; }
    if (bg && col_triple(bg, d.background, "background") < 0) return -1;
    if (white != Py_None && col_triple(white, d.white, "white") < 0) return -1;
    if (cones_o && (d.cones = col_named(cones_o, col_cones_names, 2, "cones (\"ss2\", \"ss10\")")) < 0) return -1;
    if (adapt_o && (d.adapt = col_named(adapt_o, col_adapt_names, 2, "adapt (\"none\", \"bradford\")")) < 0) return -1;
    d.cal = AS_CAL(cal);
    d.lum = lum != Py_None ? AS_LUM(lum) : NULL;
    if (!ycol_ctx_open(&o->cx, &d)) { col_fail(o->cx.status, ycol_ctx_error(&o->cx)); return -1; }
    {
        PyObject *oc = o->cal, *ol = o->lum;
        Py_INCREF(cal); o->cal = cal;
        Py_INCREF(lum); o->lum = lum;
        Py_XDECREF(oc); Py_XDECREF(ol);
    }
    return 0;
}

static void Ctx_dealloc(PyObject* self) {
    Py_CLEAR(((CtxObject*)self)->cal);
    Py_CLEAR(((CtxObject*)self)->lum);
    col_dealloc(self);
}

static int col_ready(PyObject* self) {
    if (!((CtxObject*)self)->cal) { PyErr_SetString(ColError, "the Context was not initialized"); return -1; }
    return 0;
}

static PyObject* col_rgb_gamut(ycol_rgb r, const ycol_gamut* g) {
    PyObject *t, *gg;
    if (g->status < 0) return col_fail(g->status, g->why);
    t = Py_BuildValue("(ddd)", r.r, r.g, r.b);
    gg = col_gamut(g);
    if (!t || !gg) { Py_XDECREF(t); Py_XDECREF(gg); return NULL; }
    return Py_BuildValue("(NN)", t, gg);
}

static PyObject* Ctx_to_rgb(PyObject* self, PyObject* arg) {
    ycol_color c;
    ycol_gamut g;
    ycol_rgb r;
    if (col_ready(self) < 0 || col_parse_color(arg, &c) < 0) return NULL;
    r = ycol_to_rgb(AS_CTX(self), c, &g);
    return col_rgb_gamut(r, &g);
}

static PyObject* Ctx_to_dir(PyObject* self, PyObject* arg) {
    ycol_color c;
    ycol_gamut g;
    ycol_rgb r;
    if (col_ready(self) < 0 || col_parse_color(arg, &c) < 0) return NULL;
    r = ycol_to_dir(AS_CTX(self), c, &g);
    return col_rgb_gamut(r, &g);
}

static PyObject* Ctx_from_rgb(PyObject* self, PyObject* args) {
    PyObject *ro, *so;
    double v[3];
    ycol_rgb r;
    ycol_color c;
    int sp, st;
    if (col_ready(self) < 0 || !PyArg_ParseTuple(args, "OO", &ro, &so)) return NULL;
    if (col_triple(ro, v, "rgb") < 0 || (sp = col_space(so)) < 0) return NULL;
    r.r = v[0]; r.g = v[1]; r.b = v[2];
    c = ycol_from_rgb(AS_CTX(self), r, sp, &st);
    if (st < 0) {
        const char* why = AS_CTX(self)->why_no_xyz;
        int n;
        const ycol_space_info* si = ycol_spaces(&n);
        if (si[sp - 1].needs & YCOL_CAN_LMS) why = AS_CTX(self)->why_no_lms;
        if ((si[sp - 1].needs & YCOL_CAN_BG) && AS_CTX(self)->why_no_bg) why = AS_CTX(self)->why_no_bg;
        return col_fail(st, st == YCOL_ERR_REFUSED ? why : NULL);
    }
    return col_color(&c);
}

static PyObject* Ctx_convert(PyObject* self, PyObject* args) {
    PyObject *co, *so, *cc, *gg;
    ycol_color c, out;
    ycol_gamut g;
    int sp, rc;
    if (col_ready(self) < 0 || !PyArg_ParseTuple(args, "OO", &co, &so)) return NULL;
    if (col_parse_color(co, &c) < 0 || (sp = col_space(so)) < 0) return NULL;
    rc = ycol_convert(AS_CTX(self), c, sp, &out, &g);
    if (rc < 0) return col_fail(rc, g.why);
    cc = col_color(&out);
    gg = col_gamut(&g);
    if (!cc || !gg) { Py_XDECREF(cc); Py_XDECREF(gg); return NULL; }
    return Py_BuildValue("(NNk)", cc, gg, (unsigned long)out.flags);
}

static PyObject* Ctx_convert_n(PyObject* self, PyObject* args) {
    PyObject *vo, *fo, *to, *mv, *fl;
    double *in, *out;
    uint8_t* flags;
    Py_ssize_t n;
    int from, tsp;
    if (col_ready(self) < 0 || !PyArg_ParseTuple(args, "OOO", &vo, &fo, &to)) return NULL;
    if ((from = col_space(fo)) < 0 || (tsp = col_space(to)) < 0) return NULL;
    in = col_rows(vo, &n);
    if (!in) return NULL;
    out = (double*)PyMem_Malloc((size_t)(n ? n : 1) * 3 * sizeof(double));
    flags = (uint8_t*)PyMem_Malloc((size_t)(n ? n : 1));
    if (!out || !flags) { PyMem_Free(in); PyMem_Free(out); PyMem_Free(flags); return PyErr_NoMemory(); }
    ycol_convert_n(AS_CTX(self), from, in, 0, tsp, out, 0, (size_t)n, flags);
    mv = col_view(out, n * 3 * (Py_ssize_t)sizeof(double), "d");
    fl = PyBytes_FromStringAndSize((const char*)flags, n);
    PyMem_Free(in); PyMem_Free(out); PyMem_Free(flags);
    if (!mv || !fl) { Py_XDECREF(mv); Py_XDECREF(fl); return NULL; }
    return Py_BuildValue("(NN)", mv, fl);
}

static PyObject* Ctx_max_scale(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "color", "symmetric", NULL };
    PyObject* co;
    ycol_color c;
    int sym = 0;
    double k;
    if (col_ready(self) < 0 || !PyArg_ParseTupleAndKeywords(args, kwds, "O|p", kw, &co, &sym)) return NULL;
    if (col_parse_color(co, &c) < 0) return NULL;
    k = ycol_max_scale(AS_CTX(self), c, sym ? YCOL_SYMMETRIC : YCOL_ONE_SIDED);
    if (k < 0) {
        ycol_gamut g;
        ycol_to_rgb(AS_CTX(self), c, &g);
        return col_fail((int)k, g.why);
    }
    return PyFloat_FromDouble(k);
}

static PyObject* Ctx_ring(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "plane", "fixed", "n", "symmetric", NULL };
    PyObject *po, *t;
    double fixed, *out;
    int n, sym = 0, plane, rc, i;
    if (col_ready(self) < 0 || !PyArg_ParseTupleAndKeywords(args, kwds, "Odi|p", kw, &po, &fixed, &n, &sym)) return NULL;
    if ((plane = col_named(po, col_plane_names, 5, "plane (\"dkl\", \"cone\", \"oklch\", \"cielch\")")) < 0) return NULL;
    if (n < 1 || n > 1000000) return col_fail(YCOL_ERR_ARG, "n: 1 to 1000000");
    out = (double*)PyMem_Malloc((size_t)n * sizeof(double));
    if (!out) return PyErr_NoMemory();
    rc = ycol_max_ring(AS_CTX(self), plane, fixed, sym ? YCOL_SYMMETRIC : YCOL_ONE_SIDED, n, out);
    if (rc < 0) { PyMem_Free(out); return col_fail(rc, rc == YCOL_ERR_REFUSED ? AS_CTX(self)->why_no_bg : NULL); }
    t = PyTuple_New(n);
    for (i = 0; t && i < n; i++) PyTuple_SetItem(t, i, PyFloat_FromDouble(out[i]));
    PyMem_Free(out);
    return t;
}

static PyObject* Ctx_map(PyObject* self, PyObject* args) {
    PyObject *co, *mo;
    ycol_color c;
    ycol_gamut g;
    ycol_rgb r;
    int m;
    if (col_ready(self) < 0 || !PyArg_ParseTuple(args, "OO", &co, &mo)) return NULL;
    if (col_parse_color(co, &c) < 0) return NULL;
    if ((m = col_named(mo, col_map_names, 6, "map method (\"scale\", \"chroma_oklch\", \"chroma_cielch\", \"chroma_dkl\", \"clip\")")) < 0)
        return NULL;
    r = ycol_map(AS_CTX(self), c, m, &g);
    return col_rgb_gamut(r, &g);
}

static PyObject* Ctx_michelson(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "michelson", "dir", "bits", NULL };
    PyObject *dir_o = NULL, *gg;
    double m, d[3] = { 0.0, 0.0, 0.0 };
    ycol_rgb dir;
    ycol_shown r;
    int bits = 8;
    if (col_ready(self) < 0 || !PyArg_ParseTupleAndKeywords(args, kwds, "d|Oi", kw, &m, &dir_o, &bits)) return NULL;
    if (dir_o && dir_o != Py_None && col_triple(dir_o, d, "dir") < 0) return NULL;
    dir.r = d[0]; dir.g = d[1]; dir.b = d[2];
    if (ycol_michelson(AS_CTX(self), m, dir, bits, &r) < 0) return col_fail(r.status, r.why);
    gg = col_gamut(&r.gamut);
    if (!gg) return NULL;
    return PyObject_CallFunction(ShownType, "ddddddd(kkk)(kkk)N", r.gain, r.scene, r.max, r.shown, r.Y_bg, r.Y_peak, r.Y_trough,
                                 (unsigned long)r.peak[0], (unsigned long)r.peak[1], (unsigned long)r.peak[2],
                                 (unsigned long)r.trough[0], (unsigned long)r.trough[1], (unsigned long)r.trough[2], gg);
}

static PyObject* Ctx_set_background(PyObject* self, PyObject* arg) {
    double bg[3];
    int rc;
    if (col_ready(self) < 0 || col_triple(arg, bg, "background") < 0) return NULL;
    rc = ycol_ctx_set_background(AS_CTX(self), bg);
    if (rc < 0) return col_fail(rc, "background: three numbers in 0..1");
    Py_RETURN_NONE;
}

static PyObject* Ctx_lum_pair(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "azim", "elev", "contrast", NULL };
    double az, el, c, a[3], b[3];
    int rc;
    if (col_ready(self) < 0 || !PyArg_ParseTupleAndKeywords(args, kwds, "ddd", kw, &az, &el, &c)) return NULL;
    rc = ycol_lum_pair(AS_CTX(self), az, el, c, a, b);
    if (rc < 0) return col_fail(rc, rc == YCOL_ERR_REFUSED ? AS_CTX(self)->why_no_bg : NULL);
    return Py_BuildValue("((ddd)(ddd))", a[0], a[1], a[2], b[0], b[1], b[2]);
}

static PyObject* Ctx_describe(PyObject* self, PyObject* Py_UNUSED(a)) {
    char buf[1024];
    if (col_ready(self) < 0) return NULL;
    ycol_ctx_describe(AS_CTX(self), buf, sizeof buf);
    return PyUnicode_FromString(buf);
}

static PyObject* Ctx_get(PyObject* self, void* which) {
    const ycol_ctx* cx = AS_CTX(self);
    if (col_ready(self) < 0) return NULL;
    switch ((int)(intptr_t)which) {
    case 0: return PyLong_FromUnsignedLong(cx->id);
    case 1: return PyLong_FromUnsignedLong(cx->can);
    case 2: return col_tuple3(cx->bg);
    case 3: return col_tuple3(cx->w);
    case 4: return col_tuple3(cx->white_xyz);
    case 5: return Py_BuildValue("(ddddddddd)", cx->dkl[0], cx->dkl[1], cx->dkl[2], cx->dkl[3], cx->dkl[4], cx->dkl[5],
                                 cx->dkl[6], cx->dkl[7], cx->dkl[8]);
    case 6: return Py_BuildValue("(ddddddddd)", cx->rgb_to_lms[0], cx->rgb_to_lms[1], cx->rgb_to_lms[2], cx->rgb_to_lms[3],
                                 cx->rgb_to_lms[4], cx->rgb_to_lms[5], cx->rgb_to_lms[6], cx->rgb_to_lms[7], cx->rgb_to_lms[8]);
    case 7: return PyFloat_FromDouble(cx->mb_k);
    case 8: { PyObject* c = ((CtxObject*)self)->cal; Py_INCREF(c); return c; }
    default: Py_RETURN_NONE;
    }
}

static PyMethodDef Ctx_methods[] = {
    { "to_rgb", Ctx_to_rgb, METH_O, "to_rgb(color) -> ((r, g, b), Gamut): linear device RGB, unclipped." },
    { "to_dir", Ctx_to_dir, METH_O, "to_dir(color) -> ((r, g, b), Gamut): the color minus the background; the gamut of bg +- dir." },
    { "from_rgb", Ctx_from_rgb, METH_VARARGS, "from_rgb(rgb, space) -> Color." },
    { "convert", Ctx_convert, METH_VARARGS, "convert(color, space) -> (Color, Gamut, flags)." },
    { "convert_n", Ctx_convert_n, METH_VARARGS,
      "convert_n(values, src, dst) -> (memoryview('d') of 3n, bytes of n flags: bit 0 in gamut, bit 1 refused)." },
    { "max_scale", (PyCFunction)(void (*)(void))Ctx_max_scale, METH_VARARGS | METH_KEYWORDS,
      "max_scale(color, symmetric=False) -> float: the largest factor of the offset from the background in gamut." },
    { "ring", (PyCFunction)(void (*)(void))Ctx_ring, METH_VARARGS | METH_KEYWORDS,
      "ring(plane, fixed, n, symmetric=False) -> tuple of n radii: the gamut boundary in a plane." },
    { "map", Ctx_map, METH_VARARGS, "map(color, method) -> ((r, g, b), Gamut): the color brought inside by a stated method." },
    { "michelson", (PyCFunction)(void (*)(void))Ctx_michelson, METH_VARARGS | METH_KEYWORDS,
      "michelson(michelson, dir=(0, 0, 0), bits=8) -> Shown: a Michelson contrast in light as ysp/gfx.h's contrast "
      "along dir (0 = the background), the output codes of the peak and trough, and the contrast they show." },
    { "set_background", Ctx_set_background, METH_O, "set_background((r, g, b))." },
    { "lum_pair", (PyCFunction)(void (*)(void))Ctx_lum_pair, METH_VARARGS | METH_KEYWORDS,
      "lum_pair(azim, elev, contrast) -> (a, b): the two lights of a flicker probe." },
    { "describe", Ctx_describe, METH_NOARGS, "describe() -> str: one line for the log." },
    { NULL }
};

static PyGetSetDef Ctx_getset[] = {
    { "id", Ctx_get, NULL, "a CRC-32 of every input", (void*)0 },
    { "can", Ctx_get, NULL, "CAN_* levels", (void*)1 },
    { "background", Ctx_get, NULL, "the background", (void*)2 },
    { "w", Ctx_get, NULL, "the luminance weights", (void*)3 },
    { "white", Ctx_get, NULL, "the reference white, XYZ", (void*)4 },
    { "dkl", Ctx_get, NULL, "the DKL matrix (LMS increment to DKL), row-major", (void*)5 },
    { "rgb_to_lms", Ctx_get, NULL, "RGB to LMS for the context's cones, row-major", (void*)6 },
    { "mb_k", Ctx_get, NULL, "MacLeod-Boynton's s scale", (void*)7 },
    { "cal", Ctx_get, NULL, "the Calibration", (void*)8 },
    { NULL }
};

static PyType_Slot Ctx_slots[] = {
    { Py_tp_init, (void*)Ctx_init }, { Py_tp_dealloc, (void*)Ctx_dealloc }, { Py_tp_methods, Ctx_methods },
    { Py_tp_getset, Ctx_getset }, { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_doc, (void*)"Context(cal, background=(0, 0, 0), *, cones=\"ss2\", lum=None, white=None, src_white_Y=0, adapt=\"none\")" },
    { 0, NULL }
};
static PyType_Spec Ctx_spec = { "ysp.color.Context", sizeof(CtxObject), 0, Py_TPFLAGS_DEFAULT, Ctx_slots };

/* --- module functions --------------------------------------------------------- */

/* The value constructors: one per space, the header's field names as keywords. */
#define COL_MAKER(fn, SP, k0, k1, k2)                                                                  \
    static PyObject* fn(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {                       \
        static char* kw[] = { k0, k1, k2, NULL };                                                       \
        double v[3] = { 0, 0, 0 };                                                                      \
        if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ddd", kw, &v[0], &v[1], &v[2])) return NULL;      \
        return PyObject_CallFunction(ColorType, "sddd", col_space_name(SP), v[0], v[1], v[2]);           \
    }
COL_MAKER(mk_rgb, YCOL_SPACE_RGB, "r", "g", "b")
COL_MAKER(mk_device, YCOL_SPACE_DEVICE, "r", "g", "b")
COL_MAKER(mk_srgb, YCOL_SPACE_SRGB, "r", "g", "b")
COL_MAKER(mk_p3, YCOL_SPACE_DISPLAY_P3, "r", "g", "b")
COL_MAKER(mk_2020, YCOL_SPACE_REC2020, "r", "g", "b")
COL_MAKER(mk_xyz, YCOL_SPACE_XYZ, "X", "Y", "Z")
COL_MAKER(mk_xyy, YCOL_SPACE_XYY, "x", "y", "Y")
COL_MAKER(mk_cielab, YCOL_SPACE_CIELAB, "L", "a", "b")
COL_MAKER(mk_cielch, YCOL_SPACE_CIELCH, "L", "C", "h")
COL_MAKER(mk_cieluv, YCOL_SPACE_CIELUV, "L", "u", "v")
COL_MAKER(mk_cielchuv, YCOL_SPACE_CIELCHUV, "L", "C", "h")
COL_MAKER(mk_oklab, YCOL_SPACE_OKLAB, "L", "a", "b")
COL_MAKER(mk_oklch, YCOL_SPACE_OKLCH, "L", "C", "h")
COL_MAKER(mk_lms, YCOL_SPACE_LMS, "l", "m", "s")
COL_MAKER(mk_cone, YCOL_SPACE_CONE, "l", "m", "s")
COL_MAKER(mk_dkl, YCOL_SPACE_DKL, "elev", "azim", "contrast")
COL_MAKER(mk_dkl_cart, YCOL_SPACE_DKL_CART, "lum", "lm", "s")
COL_MAKER(mk_mb, YCOL_SPACE_MB, "l", "s", "lum")
#undef COL_MAKER

static PyObject* mod_hex(PyObject* Py_UNUSED(m), PyObject* arg) {
    char s[64];
    ycol_color c;
    if (!PyUnicode_Check(arg)) { PyErr_SetString(PyExc_TypeError, "hex(str)"); return NULL; }
    if (col_utf8(arg, s, sizeof s) < 0) return NULL;
    c = ycol_hex(s);
    if (c.space == YCOL_SPACE_NONE) return col_fail(YCOL_ERR_ARG, "not a hex color (\"#rgb\" or \"#rrggbb\")");
    return col_color(&c);
}

static PyObject* mod_hex_format(PyObject* Py_UNUSED(m), PyObject* arg) {
    ycol_color c;
    char out[8];
    int rc;
    if (col_parse_color(arg, &c) < 0) return NULL;
    rc = ycol_hex_format(c, out);
    if (rc < 0) return col_fail(rc, rc == YCOL_ERR_RANGE ? "a value rounds outside 0..255" : "hex_format takes an srgb color");
    return PyUnicode_FromString(out);
}

static PyObject* mod_format(PyObject* Py_UNUSED(m), PyObject* arg) {
    ycol_color c;
    char buf[160];
    if (col_parse_color(arg, &c) < 0) return NULL;
    ycol_format(c, buf, sizeof buf);
    return PyUnicode_FromString(buf);
}

static PyObject* mod_spaces(PyObject* Py_UNUSED(m), PyObject* Py_UNUSED(a)) {
    int n, i;
    const ycol_space_info* si = ycol_spaces(&n);
    PyObject* t = PyTuple_New(n);
    for (i = 0; t && i < n; i++)
        PyTuple_SetItem(t, i, Py_BuildValue("{s:s,s:(sss),s:(sss),s:(ddd),s:(ddd),s:k}", "name", si[i].name, "comp", si[i].comp[0],
                                            si[i].comp[1], si[i].comp[2], "unit", si[i].unit[0], si[i].unit[1], si[i].unit[2], "lo",
                                            si[i].lo[0], si[i].lo[1], si[i].lo[2], "hi", si[i].hi[0], si[i].hi[1], si[i].hi[2], "needs",
                                            (unsigned long)si[i].needs));
    return t;
}

static PyObject* mod_cone_fundamentals(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "nm", "cones", NULL };
    PyObject* co = NULL;
    double nm, l[3];
    int cones = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "d|O", kw, &nm, &co)) return NULL;
    if (co && (cones = col_named(co, col_cones_names, 2, "cones")) < 0) return NULL;
    ycol_cone_fundamentals(cones, nm, l);
    return col_tuple3(l);
}

static PyObject* mod_cone_luminance(PyObject* Py_UNUSED(m), PyObject* args) {
    PyObject* co = NULL;
    double w[3];
    int cones = 0;
    if (!PyArg_ParseTuple(args, "|O", &co)) return NULL;
    if (co && (cones = col_named(co, col_cones_names, 2, "cones")) < 0) return NULL;
    ycol_cone_luminance(cones, w);
    return col_tuple3(w);
}

static PyObject* mod_output_code(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "cal", "rgb", "bits", NULL };
    PyObject *cal, *ro;
    double v[3];
    ycol_rgb r;
    uint32_t code[3];
    int bits = 8, rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO|i", kw, &cal, &ro, &bits)) return NULL;
    if (!PyObject_TypeCheck(cal, (PyTypeObject*)CalType)) { PyErr_SetString(PyExc_TypeError, "cal must be a Calibration"); return NULL; }
    if (col_triple(ro, v, "rgb") < 0) return NULL;
    r.r = v[0]; r.g = v[1]; r.b = v[2];
    rc = ycol_output_code(AS_CAL(cal), r, bits, code);
    if (rc < 0) return col_fail(rc, "bits: 8 to 16");
    return Py_BuildValue("(kkk)", (unsigned long)code[0], (unsigned long)code[1], (unsigned long)code[2]);
}

static PyObject* mod_version(PyObject* Py_UNUSED(m), PyObject* Py_UNUSED(a)) { return PyUnicode_FromString(ycol_version()); }

#define KWF(f) (PyCFunction)(void (*)(void))(f), METH_VARARGS | METH_KEYWORDS
static PyMethodDef module_methods[] = {
    { "rgb", KWF(mk_rgb), "rgb(r=0, g=0, b=0) -> Color: linear device RGB." },
    { "device", KWF(mk_device), "device(r, g, b) -> Color: drive values 0..1." },
    { "srgb", KWF(mk_srgb), "srgb(r, g, b) -> Color: IEC 61966-2-1 encoded values." },
    { "display_p3", KWF(mk_p3), "display_p3(r, g, b) -> Color." },
    { "rec2020", KWF(mk_2020), "rec2020(r, g, b) -> Color." },
    { "xyz", KWF(mk_xyz), "xyz(X, Y, Z) -> Color: CIE 1931, cd/m2." },
    { "xyy", KWF(mk_xyy), "xyy(x, y, Y) -> Color." },
    { "cielab", KWF(mk_cielab), "cielab(L, a, b) -> Color." },
    { "cielch", KWF(mk_cielch), "cielch(L, C, h) -> Color." },
    { "cieluv", KWF(mk_cieluv), "cieluv(L, u, v) -> Color." },
    { "cielchuv", KWF(mk_cielchuv), "cielchuv(L, C, h) -> Color." },
    { "oklab", KWF(mk_oklab), "oklab(L, a, b) -> Color." },
    { "oklch", KWF(mk_oklch), "oklch(L, C, h) -> Color." },
    { "lms", KWF(mk_lms), "lms(l, m, s) -> Color: cone excitations." },
    { "cone", KWF(mk_cone), "cone(l, m, s) -> Color: cone contrast about the background." },
    { "dkl", KWF(mk_dkl), "dkl(elev=0, azim=0, contrast=0) -> Color: DKL spherical, degrees." },
    { "dkl_cart", KWF(mk_dkl_cart), "dkl_cart(lum, lm, s) -> Color." },
    { "mb", KWF(mk_mb), "mb(l, s, lum) -> Color: MacLeod-Boynton." },
    { "hex", mod_hex, METH_O, "hex(\"#3366cc\") -> Color in srgb." },
    { "hex_format", mod_hex_format, METH_O, "hex_format(srgb color) -> \"#rrggbb\"." },
    { "format", mod_format, METH_O, "format(color) -> \"dkl(0 90 0.1)\" for logs." },
    { "spaces", mod_spaces, METH_NOARGS, "spaces() -> tuple of dicts: name, comp, unit, lo, hi, needs." },
    { "cone_fundamentals", KWF(mod_cone_fundamentals), "cone_fundamentals(nm, cones=\"ss2\") -> (l, m, s)." },
    { "cone_luminance", mod_cone_luminance, METH_VARARGS, "cone_luminance(cones=\"ss2\") -> (wL, wM, wS)." },
    { "output_code", KWF(mod_output_code), "output_code(cal, rgb, bits=8) -> the codes ysp/gfx.h's output stage writes." },
    { "version", mod_version, METH_NOARGS, "version() -> the header's version string." },
    { NULL }
};
#undef KWF

static PyModuleDef ysp_color_module = {
    PyModuleDef_HEAD_INIT, "ysp.color",
    "Color on calibrated displays: calibration, conversions, gamut questions (ysp/color.h).", -1, module_methods,
};

static PyObject* col_add_exc(PyObject* m, const char* qual, const char* attr, PyObject* base) {
    PyObject* e = PyErr_NewException(qual, base, NULL);
    if (!e) return NULL;
    Py_INCREF(e);
    if (PyModule_AddObject(m, attr, e) < 0) { Py_DECREF(e); Py_DECREF(e); return NULL; }
    return e;
}

static PyObject* col_namedtuple(PyObject* m, const char* name, const char* fields) {
    PyObject* coll = PyImport_ImportModule("collections");
    PyObject *nt, *a, *k, *t = NULL;
    if (!coll) return NULL;
    nt = PyObject_GetAttrString(coll, "namedtuple");
    a = Py_BuildValue("(ss)", name, fields);
    k = Py_BuildValue("{s:s}", "module", "ysp.color");
    if (nt && a && k) t = PyObject_Call(nt, a, k);
    Py_XDECREF(nt); Py_XDECREF(a); Py_XDECREF(k); Py_DECREF(coll);
    if (!t) return NULL;
    Py_INCREF(t);
    if (PyModule_AddObject(m, name, t) < 0) { Py_DECREF(t); Py_DECREF(t); return NULL; }
    return t;
}

PyMODINIT_FUNC PyInit_color(void) {
    PyObject *m, *base2, *t;
    int i, n;
    const ycol_space_info* si;
    m = PyModule_Create(&ysp_color_module);
    if (!m) return NULL;
    if (!(CalType = PyType_FromSpec(&Cal_spec))) goto fail;
    Py_INCREF(CalType);
    if (PyModule_AddObject(m, "Calibration", CalType) < 0) goto fail;
    if (!(LumType = PyType_FromSpec(&Lum_spec))) goto fail;
    Py_INCREF(LumType);
    if (PyModule_AddObject(m, "Lum", LumType) < 0) goto fail;
    if (!(t = PyType_FromSpec(&Ctx_spec))) goto fail;
    if (PyModule_AddObject(m, "Context", t) < 0) { Py_DECREF(t); goto fail; }
    if (!(ColError = col_add_exc(m, "ysp.color.Error", "Error", NULL))) goto fail;
    base2 = PyTuple_Pack(2, ColError, PyExc_ValueError);
    if (!base2) goto fail;
    ColArgumentError = col_add_exc(m, "ysp.color.ArgumentError", "ArgumentError", base2);
    ColRefusedError = col_add_exc(m, "ysp.color.RefusedError", "RefusedError", base2);
    ColFormatError = col_add_exc(m, "ysp.color.FormatError", "FormatError", base2);
    ColRangeError = col_add_exc(m, "ysp.color.RangeError", "RangeError", base2);
    Py_DECREF(base2);
    if (!ColArgumentError || !ColRefusedError || !ColFormatError || !ColRangeError) goto fail;
    if (!(ColorType = col_namedtuple(m, "Color", "space a b c"))) goto fail;
    if (!(GamutType = col_namedtuple(m, "Gamut", "status in_gamut below above margin distance scale kept why"))) goto fail;
    if (!(ShownType = col_namedtuple(m, "Shown", "gain scene max shown Y_bg Y_peak Y_trough peak trough gamut"))) goto fail;
    if (PyModule_AddStringConstant(m, "__version__", ycol_version()) < 0) goto fail;
    si = ycol_spaces(&n);
    for (i = 0; i < n; i++) {   /* SPACE_DKL = 16 and so on, as the header */
        char name[48], *p;
        PyOS_snprintf(name, sizeof name, "SPACE_%s", si[i].name);
        for (p = name; *p; p++) { if (*p == '-') *p = '_'; else if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32); }
        PyModule_AddIntConstant(m, name, si[i].space);
    }
    PyModule_AddIntConstant(m, "CAN_RGB", YCOL_CAN_RGB);
    PyModule_AddIntConstant(m, "CAN_XYZ", YCOL_CAN_XYZ);
    PyModule_AddIntConstant(m, "CAN_LMS", YCOL_CAN_LMS);
    PyModule_AddIntConstant(m, "CAN_BG", YCOL_CAN_BG);
    PyModule_AddIntConstant(m, "CAL_NOMINAL", YCOL_CAL_NOMINAL);
    PyModule_AddIntConstant(m, "CAL_HAS_XY", YCOL_CAL_HAS_XY);
    PyModule_AddIntConstant(m, "CAL_HAS_SPECTRA", YCOL_CAL_HAS_SPECTRA);
    PyModule_AddIntConstant(m, "CAL_SPECTRA_NOMINAL", YCOL_CAL_SPECTRA_NOMINAL);
    PyModule_AddIntConstant(m, "GUN_BLACK", YCOL_GUN_BLACK);
    PyModule_AddIntConstant(m, "GUN_WHITE", YCOL_GUN_WHITE);
    PyModule_AddIntConstant(m, "LUM_STATED", YCOL_LUM_STATED);
    PyModule_AddIntConstant(m, "LUM_FIT_S", YCOL_LUM_FIT_S);
    PyModule_AddIntConstant(m, "F_ACHROMATIC", YCOL_F_ACHROMATIC);
    PyModule_AddIntConstant(m, "F_VIA_DEVICE", YCOL_F_VIA_DEVICE);
    PyModule_AddObject(m, "GAMUT_EPS", PyFloat_FromDouble(YCOL_GAMUT_EPS));
    return m;
fail:
    Py_DECREF(m);
    return NULL;
}
