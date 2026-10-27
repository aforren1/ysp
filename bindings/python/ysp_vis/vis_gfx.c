/* vis_gfx.c - ysp.gfx in the ysp._vis extension: Gfx, Stim, DrawList, Group, Texture
 *
 * Not compiled alone: ysp_vis_ext.c includes it where the Gfx type goes,
 * after the Screen type. The work is bindings/share/ysp_vis_gfx.h's (the
 * same for MEX); this file turns Python arguments into its yv_kv lists and
 * its results into Python objects.
 *
 *     g = ygfx.Gfx(s, background=0.5)
 *     gab = g.gabor(sf=1 / 32, sigma=24, contrast=0.5, x=-200)
 *     scene = g.list([gab, g.shape("circle", w=8, color=1.0)])
 *     g.bind(gab, "contrast", tl, "contrast")     # a timeline channel, applied in begin()
 *     while True:
 *         f = s.begin()
 *         g.frame(scene)                          # begin, bindings, draws, end: one call
 *         s.flip()
 *
 * LIFETIME. A Stim, DrawList, Group or Texture holds its Gfx, so the yv_gfx
 * outlives every object; a closed Gfx (or Screen) makes each of them raise
 * ClosedError. A list holds its stimuli, a stimulus its group and texture.
 * An Instances stimulus keeps its elements in a bytearray it owns, and
 * `elements` is a view of it: a view kept after the stimulus is gone reads
 * the last values ("A view owns what it shows", docs/timeline.md).
 */

static PyObject *StimType, *ListType, *GroupType, *TexType;
static PyObject* vs_add_type(PyObject* m, PyType_Spec* spec, const char* name);

/* A gray level or (r, g, b), linear 0..1 (phase A's rect and background). */
static int vs_rgb(PyObject* o, float out[3], const char* what) {
    Py_ssize_t i;
    if (!o) return 0;
    if (PyFloat_Check(o) || PyLong_Check(o)) {
        double x = PyFloat_AsDouble(o);
        if (x == -1.0 && PyErr_Occurred()) return -1;
        out[0] = out[1] = out[2] = (float)x;
        return 0;
    }
    if (!PySequence_Check(o) || PySequence_Size(o) != 3) {
        PyErr_Clear();
        PyErr_Format(VsArgumentError, "%s: a gray level or (r, g, b), linear 0..1", what);
        return -1;
    }
    for (i = 0; i < 3; i++) {
        PyObject* it = PySequence_GetItem(o, i);
        double x = it ? PyFloat_AsDouble(it) : -1.0;
        Py_XDECREF(it);
        if (x == -1.0 && PyErr_Occurred()) return -1;
        out[i] = (float)x;
    }
    return 0;
}

typedef struct GfxObject {
    PyObject_HEAD
    yv_gfx*   g;
    PyObject* screen;
} GfxObject;

typedef struct StimObject {
    PyObject_HEAD
    yv_stim*  st;
    PyObject* gfx;
    PyObject* group;     /* the Group it is in, or NULL                  */
    PyObject* tex;       /* an image's Texture                           */
    PyObject* mem;       /* Instances: the bytearray of the elements     */
    Py_ssize_t mem_off;  /* the elements' offset in it (16-byte aligned) */
} StimObject;

typedef struct ListObject {
    PyObject_HEAD
    yv_list*  l;
    PyObject* gfx;
    PyObject* items;     /* a list of the Stim objects, in draw order    */
} ListObject;

typedef struct GroupObject {
    PyObject_HEAD
    yv_group* gr;
    PyObject* gfx;
} GroupObject;

typedef struct TexObject {
    PyObject_HEAD
    yv_tex*   t;
    PyObject* gfx;
} TexObject;

#define GF_OF(o) ((GfxObject*)(o))
#define ST_OF(o) ((StimObject*)(o))
#define LS_OF(o) ((ListObject*)(o))
#define GR_OF(o) ((GroupObject*)(o))
#define TX_OF(o) ((TexObject*)(o))

/* ---- errors -------------------------------------------------------------------- */

static PyObject* vg_raise(int code, const char* msg) {
    if (code == YV_ERR_HOST) return NULL;   /* a Python callback raised: its error is set */
    PyErr_SetString(vs_gfx_exc(code), msg && msg[0] ? msg : ygfx_strerror(code));
    return NULL;
}

static int gf_ready(GfxObject* o) {
    if (!o->g || !o->g->open) { PyErr_SetString(VsClosedError, "ysp_gfx: the gfx (or its screen) is closed"); return 0; }
    if (SC_OF(o->screen)->busy) { PyErr_SetString(VsBusyError, "ysp_gfx: another thread is in a call on its Screen"); return 0; }
    return 1;
}

/* An object of a Gfx: open, and no other thread in a call on its screen. */
static int vg_obj_ready(PyObject* gfx) {
    GfxObject* g = GF_OF(gfx);
    if (!g->g || !g->g->open) { PyErr_SetString(VsClosedError, "ysp_gfx: its gfx (or the gfx's screen) is closed"); return 0; }
    if (g->screen && SC_OF(g->screen)->busy) { PyErr_SetString(VsBusyError, "ysp_gfx: another thread is in a call on its Screen"); return 0; }
    return 1;
}

/* ---- arguments: Python values to the core's yv_kv --------------------------------- */

#define VG_MAX_KV 64

typedef struct vg_arena {
    void*     mem[VG_MAX_KV * 2];
    int       n_mem;
    PyObject* keep[VG_MAX_KV * 2];
    int       n_keep;
} vg_arena;

static void vg_arena_free(vg_arena* a) {
    int i;
    for (i = 0; i < a->n_mem; i++) PyMem_Free(a->mem[i]);
    for (i = 0; i < a->n_keep; i++) Py_DECREF(a->keep[i]);
    a->n_mem = a->n_keep = 0;
}

static double* vg_alloc(vg_arena* a, Py_ssize_t n) {
    double* p;
    if (a->n_mem == (int)(sizeof a->mem / sizeof a->mem[0])) { PyErr_SetString(VsArgumentError, "too many arguments"); return NULL; }
    p = (double*)PyMem_Malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
    if (!p) { PyErr_NoMemory(); return NULL; }
    a->mem[a->n_mem++] = p;
    return p;
}

static const char* vg_str(PyObject* o, vg_arena* a) {
    PyObject* b;
    if (a->n_keep == (int)(sizeof a->keep / sizeof a->keep[0])) { PyErr_SetString(VsArgumentError, "too many arguments"); return NULL; }
    b = PyUnicode_AsUTF8String(o);
    if (!b) return NULL;
    a->keep[a->n_keep++] = b;
    return PyBytes_AsString(b);
}

/* The bytes of a memoryview-able object as doubles, by its format. The
 * Limited API has no PyObject_GetBuffer before 3.11, so this goes through
 * memoryview's tobytes() (a copy, C order). -1 with an error; 1 when obj
 * is not a buffer (no error set). */
static int vg_buffer_doubles(PyObject* obj, vg_arena* a, const double** out, int* n) {
    PyObject *mv, *fmt_o = NULL, *fmt_b = NULL, *isz = NULL, *bytes = NULL;
    const char* fmt;
    long itemsize;
    const char* p;
    Py_ssize_t nb, i, cnt;
    double* d;
    char f;
    mv = PyMemoryView_FromObject(obj);
    if (!mv) { PyErr_Clear(); return 1; }
    isz = PyObject_GetAttrString(mv, "itemsize");
    fmt_o = PyObject_GetAttrString(mv, "format");
    if (!isz || !fmt_o) goto fail;
    itemsize = PyLong_AsLong(isz);
    fmt_b = PyUnicode_AsUTF8String(fmt_o);
    if (!fmt_b) goto fail;
    fmt = PyBytes_AsString(fmt_b);
    if (fmt[0] == '@' || fmt[0] == '=' || fmt[0] == '<') fmt++;
    f = fmt[0];
    if (fmt[1] != 0 || !strchr("fdbBhHiIlLqQ?", f)) {
        PyErr_Format(VsArgumentError, "a buffer of numbers (float32, float64 or integers), not format '%s'", fmt);
        goto fail;
    }
    bytes = PyObject_CallMethod(mv, "tobytes", NULL);
    if (!bytes) goto fail;
    p = PyBytes_AsString(bytes);
    nb = PyBytes_Size(bytes);
    cnt = itemsize > 0 ? nb / itemsize : 0;
    if (cnt > INT32_MAX / 2) { PyErr_SetString(VsArgumentError, "too many values"); goto fail; }
    d = vg_alloc(a, cnt);
    if (!d) goto fail;
    for (i = 0; i < cnt; i++) {
        const char* q = p + i * itemsize;
        switch (f) {
        case 'f': { float x; memcpy(&x, q, 4); d[i] = x; break; }
        case 'd': memcpy(&d[i], q, 8); break;
        case 'b': d[i] = *(const signed char*)q; break;
        case 'B': case '?': d[i] = *(const unsigned char*)q; break;
        case 'h': { int16_t x; memcpy(&x, q, 2); d[i] = x; break; }
        case 'H': { uint16_t x; memcpy(&x, q, 2); d[i] = x; break; }
        case 'i': { int32_t x; memcpy(&x, q, 4); d[i] = x; break; }
        case 'I': { uint32_t x; memcpy(&x, q, 4); d[i] = x; break; }
        default:
            if (itemsize == 8) {
                if (f == 'q' || f == 'l') { int64_t x; memcpy(&x, q, 8); d[i] = (double)x; }
                else { uint64_t x; memcpy(&x, q, 8); d[i] = (double)x; }
            } else {
                if (f == 'l') { int32_t x; memcpy(&x, q, 4); d[i] = x; }
                else { uint32_t x; memcpy(&x, q, 4); d[i] = x; }
            }
        }
    }
    *out = d;
    *n = (int)cnt;
    Py_DECREF(bytes);
    Py_DECREF(fmt_b);
    Py_DECREF(fmt_o);
    Py_DECREF(isz);
    Py_DECREF(mv);
    return 0;
fail:
    Py_XDECREF(bytes);
    Py_XDECREF(fmt_b);
    Py_XDECREF(fmt_o);
    Py_XDECREF(isz);
    Py_DECREF(mv);
    return -1;
}

static int vg_is_num(PyObject* o) { return PyFloat_Check(o) || PyLong_Check(o); }

/* Numbers from o: a number, a buffer, or a sequence of numbers or of
 * sequences of numbers (flattened row by row). */
static int vg_doubles(PyObject* o, vg_arena* a, const double** out, int* n, const char* what) {
    Py_ssize_t len, i, j, total = 0;
    double* d;
    int rc;
    if (vg_is_num(o)) {
        d = vg_alloc(a, 1);
        if (!d) return -1;
        d[0] = PyFloat_AsDouble(o);
        if (d[0] == -1.0 && PyErr_Occurred()) return -1;
        *out = d;
        *n = 1;
        return 0;
    }
    if (PyUnicode_Check(o)) { PyErr_Format(VsArgumentError, "%s: numbers, not a str", what); return -1; }
    if (!PyList_Check(o) && !PyTuple_Check(o)) {
        rc = vg_buffer_doubles(o, a, out, n);
        if (rc <= 0) return rc;
    }
    if (!PySequence_Check(o)) { PyErr_Format(VsArgumentError, "%s: a number, a sequence or a buffer of numbers", what); return -1; }
    len = PySequence_Size(o);
    if (len < 0) return -1;
    for (i = 0; i < len; i++) {
        PyObject* it = PySequence_GetItem(o, i);
        if (!it) return -1;
        if (vg_is_num(it)) total++;
        else if (PySequence_Check(it) && !PyUnicode_Check(it)) {
            Py_ssize_t m = PySequence_Size(it);
            if (m < 0) { Py_DECREF(it); return -1; }
            total += m;
        } else {
            Py_DECREF(it);
            PyErr_Format(VsArgumentError, "%s: numbers, or rows of numbers", what);
            return -1;
        }
        Py_DECREF(it);
    }
    if (total > INT32_MAX / 2) { PyErr_Format(VsArgumentError, "%s: too many values", what); return -1; }
    d = vg_alloc(a, total);
    if (!d) return -1;
    for (i = 0, total = 0; i < len; i++) {
        PyObject* it = PySequence_GetItem(o, i);
        if (vg_is_num(it)) {
            d[total] = PyFloat_AsDouble(it);
            if (d[total] == -1.0 && PyErr_Occurred()) { Py_DECREF(it); return -1; }
            total++;
        } else {
            Py_ssize_t m = PySequence_Size(it);
            for (j = 0; j < m; j++) {
                PyObject* x = PySequence_GetItem(it, j);
                if (!x) { Py_DECREF(it); return -1; }
                d[total] = PyFloat_AsDouble(x);
                Py_DECREF(x);
                if (d[total] == -1.0 && PyErr_Occurred()) {
                    Py_DECREF(it);
                    PyErr_Clear();
                    PyErr_Format(VsArgumentError, "%s: numbers, or rows of numbers", what);
                    return -1;
                }
                total++;
            }
        }
        Py_DECREF(it);
    }
    *out = d;
    *n = (int)total;
    return 0;
}

/* One keyword to a yv_kv. 0, 1 to skip (None), -1 with an error. */
static int vg_kv(const char* name, PyObject* v, yv_kv* k, vg_arena* a) {
    memset(k, 0, sizeof *k);
    k->name = name;
    if (v == Py_None) return 1;
    if (PyObject_TypeCheck(v, (PyTypeObject*)GroupType)) {
        if (!GR_OF(v)->gr) { PyErr_SetString(VsClosedError, "the group is closed"); return -1; }
        k->obj = &GR_OF(v)->gr->o;
        return 0;
    }
    if (PyObject_TypeCheck(v, (PyTypeObject*)TexType)) {
        if (!TX_OF(v)->t) { PyErr_SetString(VsClosedError, "the texture is closed"); return -1; }
        k->obj = &TX_OF(v)->t->o;
        return 0;
    }
    if (PyUnicode_Check(v)) {
        k->s = vg_str(v, a);
        return k->s ? 0 : -1;
    }
    if (PyBool_Check(v)) {
        double* d = vg_alloc(a, 1);
        if (!d) return -1;
        d[0] = v == Py_True ? 1.0 : 0.0;
        k->v = d;
        k->n = 1;
        return 0;
    }
    if ((!strcmp(name, "cal") || !strcmp(name, "lum")) && !PyBytes_Check(v) && !PyByteArray_Check(v) &&
        PyObject_HasAttrString(v, "save")) {
        /* a ysp.color Calibration or Lum: its canonical bytes */
        PyObject* b = PyObject_CallMethod(v, "save", NULL);
        int rc;
        if (!b) return -1;
        rc = vg_doubles(b, a, &k->v, &k->n, name);
        Py_DECREF(b);
        return rc;
    }
    return vg_doubles(v, a, &k->v, &k->n, name);
}

/* Every keyword of kw into kv (None skipped); the count, or -1. */
static int vg_kvs(PyObject* kw, yv_kv* kv, vg_arena* a, int start) {
    Py_ssize_t pos = 0;
    PyObject *key, *val;
    int n = start;
    while (kw && PyDict_Next(kw, &pos, &key, &val)) {
        const char* name;
        int rc;
        if (n == VG_MAX_KV) { PyErr_SetString(VsArgumentError, "too many keyword arguments"); return -1; }
        name = vg_str(key, a);
        if (!name) return -1;
        rc = vg_kv(name, val, &kv[n], a);
        if (rc < 0) return -1;
        if (rc == 0) n++;
    }
    return n;
}

/* The one keyword the core did not take, for the message. */
static PyObject* vg_kv_unused(yv_kv* kv, int n) {
    int i;
    for (i = 0; i < n; i++)
        if (!kv[i].used) return PyErr_Format(PyExc_TypeError, "unexpected keyword argument '%s'", kv[i].name);
    return NULL;
}

/* ---- object constructors ------------------------------------------------------------------- */

static PyObject* vg_new(PyObject* type) {
    PyTypeObject* t = (PyTypeObject*)type;
    allocfunc al = (allocfunc)PyType_GetSlot(t, Py_tp_alloc);
    return al(t, 0);
}

static PyObject* vg_wrap_stim(PyObject* gfx, yv_stim* st) {
    PyObject* o = vg_new(StimType);
    if (!o) { yv_stim_free(st); return NULL; }
    ST_OF(o)->st = st;
    Py_INCREF(gfx);
    ST_OF(o)->gfx = gfx;
    return o;
}

/* The Python references a stimulus's C pointers need: its group, its texture. */
static void vg_stim_refs(PyObject* so, PyObject* group, PyObject* tex) {
    StimObject* s = ST_OF(so);
    PyObject* old;
    if (group && group != Py_None && s->st->group) {
        old = s->group;
        Py_INCREF(group);
        s->group = group;
        Py_XDECREF(old);
    } else if (!s->st->group) {
        Py_CLEAR(s->group);
    }
    if (tex && tex != Py_None && s->st->tex) {
        old = s->tex;
        Py_INCREF(tex);
        s->tex = tex;
        Py_XDECREF(old);
    } else if (!s->st->tex) {
        Py_CLEAR(s->tex);
    }
}

/* ---- Gfx --------------------------------------------------------------------------------------- */

/* Screen(gfx=...): True or a dict of Gfx options; the gfx the screen's
 * on_context starts (yv_open), which Gfx(screen) finishes. */
static yv_gfx* vg_gstart(PyObject* opts) {
    yv_kv kv[VG_MAX_KV];
    vg_arena a;
    char err[512];
    int n = 0, rc;
    yv_gfx* g;
    if (opts != Py_True && !PyDict_Check(opts)) { PyErr_SetString(VsArgumentError, "gfx: True or a dict of Gfx options"); return NULL; }
    memset(&a, 0, sizeof a);
    if (PyDict_Check(opts) && (n = vg_kvs(opts, kv, &a, 0)) < 0) { vg_arena_free(&a); return NULL; }
    g = (yv_gfx*)calloc(1, sizeof(yv_gfx));
    if (!g) { vg_arena_free(&a); PyErr_NoMemory(); return NULL; }
    yv_gfx_defaults(g);
    rc = yv_gfx_options(g, kv, n, err, sizeof err);
    vg_arena_free(&a);
    if (rc < 0) { free(g); vg_raise(rc, err); return NULL; }
    return g;
}

static void Gfx_dealloc(PyObject* self) {
    GfxObject* o = GF_OF(self);
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    if (o->g) {
        yv_gfx_close(o->g);
        free(o->g);
        o->g = NULL;
    }
    Py_XDECREF(o->screen);
    fr(self);
    Py_DECREF(tp);
}

static int Gfx_init(PyObject* self, PyObject* args, PyObject* kw) {
    GfxObject* o = GF_OF(self);
    PyObject* scr = NULL;
    ScreenObject* so;
    yv_kv kv[VG_MAX_KV];
    vg_arena a;
    char err[512];
    int n = 0, rc = 0;
    bool ok;
    yv_gfx* g;
    if (o->g) { PyErr_SetString(VsError, "Gfx.__init__ called twice"); return -1; }
    if (PyTuple_Size(args) == 1) scr = PyTuple_GetItem(args, 0);
    else if (kw && PyDict_GetItemString(kw, "screen")) scr = PyDict_GetItemString(kw, "screen");
    if (!scr || PyTuple_Size(args) > 1) { PyErr_SetString(PyExc_TypeError, "Gfx(screen, **options)"); return -1; }
    if (!PyObject_TypeCheck(scr, (PyTypeObject*)ScreenType)) { PyErr_SetString(PyExc_TypeError, "Gfx(screen): a ysp.screen.Screen"); return -1; }
    so = SC_OF(scr);
    if (!sc_ready(so)) return -1;
    if (so->v->gfx) { PyErr_SetString(VsRefusedError, "ysp_gfx: the screen has a Gfx already (one per screen: its output stage writes the whole frame)"); return -1; }
    memset(&a, 0, sizeof a);
    if (kw) {
        PyObject* k2 = PyDict_Copy(kw);
        if (!k2) return -1;
        PyDict_DelItemString(k2, "screen");
        PyErr_Clear();
        n = vg_kvs(k2, kv, &a, 0);
        Py_DECREF(k2);
        if (n < 0) { vg_arena_free(&a); return -1; }
    }
    if (so->v->gstart) {
        /* The screen started this gfx while it settled (Screen(gfx=...)). */
        if (n > 0) {
            vg_arena_free(&a);
            PyErr_SetString(VsArgumentError, "ysp_gfx: the screen started its gfx at open (Screen(gfx={...})): give the options there");
            return -1;
        }
        g = so->v->gstart;
    } else {
        g = (yv_gfx*)calloc(1, sizeof(yv_gfx));
        if (!g) { vg_arena_free(&a); PyErr_NoMemory(); return -1; }
        yv_gfx_defaults(g);
        rc = yv_gfx_options(g, kv, n, err, sizeof err);
        if (rc < 0) { free(g); vg_arena_free(&a); vg_raise(rc, err); return -1; }
    }
    vg_arena_free(&a);
    so->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    ok = yv_gfx_open(g, so->v);
    Py_END_ALLOW_THREADS
    so->busy = 0;
    if (!ok) {
        PyErr_SetString(VsError, ygfx_error(&g->g));
        if (so->v->gstart == g) so->v->gstart = NULL;
        yv_gfx_close(g);
        free(g);
        return -1;
    }
    o->g = g;
    Py_INCREF(scr);
    o->screen = scr;
    return 0;
}

static PyObject* Gfx_close(PyObject* self, PyObject* unused) {
    GfxObject* o = GF_OF(self);
    (void)unused;
    if (o->screen && SC_OF(o->screen)->busy) { PyErr_SetString(VsBusyError, "ysp_gfx: another thread is in a call on its Screen"); return NULL; }
    if (o->g) yv_gfx_close(o->g);
    Py_RETURN_NONE;
}
static PyObject* Gfx_enter(PyObject* self, PyObject* unused) { (void)unused; Py_INCREF(self); return self; }
static PyObject* Gfx_exit(PyObject* self, PyObject* args) { (void)args; return Gfx_close(self, NULL); }

static PyObject* Gfx_begin(PyObject* self, PyObject* unused) {
    GfxObject* o = GF_OF(self);
    char err[512];
    int rc;
    (void)unused;
    if (!gf_ready(o)) return NULL;
    rc = yv_gfx_begin(o->g, err, sizeof err);
    if (rc < 0) return vg_raise(rc, err);
    Py_RETURN_NONE;
}

static PyObject* Gfx_end(PyObject* self, PyObject* unused) {
    GfxObject* o = GF_OF(self);
    char err[512];
    int rc;
    (void)unused;
    if (!gf_ready(o)) return NULL;
    rc = yv_gfx_end(o->g, err, sizeof err);
    if (rc < 0) return vg_raise(rc, err);
    Py_RETURN_NONE;
}

/* A Stim or DrawList of this Gfx as the core's object, or NULL with an error. */
static yv_obj* vg_drawable(GfxObject* o, PyObject* x) {
    if (PyObject_TypeCheck(x, (PyTypeObject*)StimType)) {
        if (ST_OF(x)->gfx != (PyObject*)o) { PyErr_SetString(VsArgumentError, "draw: a stimulus of another Gfx"); return NULL; }
        return &ST_OF(x)->st->o;
    }
    if (PyObject_TypeCheck(x, (PyTypeObject*)ListType)) {
        if (LS_OF(x)->gfx != (PyObject*)o) { PyErr_SetString(VsArgumentError, "draw: a list of another Gfx"); return NULL; }
        return &LS_OF(x)->l->o;
    }
    PyErr_SetString(PyExc_TypeError, "draw: a Stim or a DrawList");
    return NULL;
}

static PyObject* vg_draw_fail(int rc, const char* err, int obj, int item) {
    char m[600];
    if (item >= 0) PyOS_snprintf(m, sizeof m, "argument %d, item %d: %s", obj, item, err);
    else if (obj >= 0) PyOS_snprintf(m, sizeof m, "argument %d: %s", obj, err);
    else PyOS_snprintf(m, sizeof m, "%s", err);
    return vg_raise(rc, m);
}

static PyObject* Gfx_draw(PyObject* self, PyObject* args) {
    GfxObject* o = GF_OF(self);
    Py_ssize_t i, n = PyTuple_Size(args);
    char err[512];
    int rc, bad;
    if (!gf_ready(o)) return NULL;
    for (i = 0; i < n; i++) {
        yv_obj* x = vg_drawable(o, PyTuple_GetItem(args, i));
        if (!x) return NULL;
        rc = yv_gfx_draw(o->g, x, &bad, err, sizeof err);
        if (rc < 0) return vg_draw_fail(rc, err, (int)i, bad);
    }
    Py_RETURN_NONE;
}

static PyObject* Gfx_frame(PyObject* self, PyObject* args) {
    GfxObject* o = GF_OF(self);
    yv_obj* objs[64];
    Py_ssize_t i, n = PyTuple_Size(args);
    char err[512];
    int rc, bo, bi;
    if (!gf_ready(o)) return NULL;
    if (n > 64) { PyErr_SetString(VsArgumentError, "frame: at most 64 stimuli or lists (put more in a DrawList)"); return NULL; }
    for (i = 0; i < n; i++)
        if (!(objs[i] = vg_drawable(o, PyTuple_GetItem(args, i)))) return NULL;
    rc = yv_gfx_frame(o->g, objs, (int)n, &bo, &bi, err, sizeof err);
    if (rc < 0) return vg_draw_fail(rc, err, bo, bi);
    Py_RETURN_NONE;
}

/* rect(x, y, w, h, color): phase A's immediate filled rectangle. */
static PyObject* Gfx_rect(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "x", "y", "w", "h", "color", NULL };
    GfxObject* o = GF_OF(self);
    double x, y, w, h;
    PyObject* col = NULL;
    float c[3] = { 1, 1, 1 };
    ygfx_shape_desc sd;
    ygfx_stim st;
    int rc;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "dddd|O", kwlist, &x, &y, &w, &h, &col)) return NULL;
    if (vs_rgb(col, c, "color") < 0) return NULL;
    if (!gf_ready(o)) return NULL;
    if (!o->g->in_frame) { PyErr_SetString(VsOrderError, "ysp_gfx: rect() between begin() and end()"); return NULL; }
    memset(&sd, 0, sizeof sd);
    sd.shape = YGFX_RECT;
    sd.x = (float)x; sd.y = (float)y; sd.w = (float)w; sd.h = (float)h;
    sd.color[0] = c[0]; sd.color[1] = c[1]; sd.color[2] = c[2];
    st = ygfx_shape(&sd);
    rc = ygfx_draw(&o->g->g, &st);
    if (rc < 0) { char err[512]; yv_gfx_err(o->g, rc, err, sizeof err); return vg_raise(rc, err); }
    Py_RETURN_NONE;
}

/* The stimulus makers: g.shape(shape, **kw), g.grating(**kw), ... */
static PyObject* vg_make(GfxObject* o, int vkind, PyObject* first_name, const char* first_key, PyObject* kw) {
    yv_kv kv[VG_MAX_KV];
    vg_arena a;
    char err[512];
    int n = 0, code, i;
    yv_stim* st;
    PyObject* r;
    PyObject *group = NULL, *tex = NULL;
    if (!gf_ready(o)) return NULL;
    memset(&a, 0, sizeof a);
    if (first_name) {
        if (vg_kv(first_key, first_name, &kv[0], &a) < 0) { vg_arena_free(&a); return NULL; }
        n = 1;
    }
    n = vg_kvs(kw, kv, &a, n);
    if (n < 0) { vg_arena_free(&a); return NULL; }
    if (kw) {
        group = PyDict_GetItemString(kw, "group");
        tex = PyDict_GetItemString(kw, "tex");
    }
    st = yv_stim_make(o->g, vkind, kv, n, &code, err, sizeof err);
    if (!st) {
        vg_arena_free(&a);
        return vg_raise(code, err);
    }
    for (i = 0; i < n; i++)
        if (!kv[i].used) { yv_stim_free(st); vg_kv_unused(kv, n); vg_arena_free(&a); return NULL; }
    vg_arena_free(&a);
    r = vg_wrap_stim((PyObject*)o, st);
    if (r) vg_stim_refs(r, group, tex);
    return r;
}

static PyObject* Gfx_shape(PyObject* self, PyObject* args, PyObject* kw) {
    PyObject* shape = NULL;
    if (!PyArg_ParseTuple(args, "|O", &shape)) return NULL;
    if (shape && kw && PyDict_GetItemString(kw, "shape")) { PyErr_SetString(PyExc_TypeError, "shape given twice"); return NULL; }
    return vg_make(GF_OF(self), YV_K_SHAPE, shape, "shape", kw);
}

#define VG_MAKER(fn, kind)                                                                        \
    static PyObject* fn(PyObject* self, PyObject* args, PyObject* kw) {                           \
        if (PyTuple_Size(args) > 0) { PyErr_SetString(PyExc_TypeError, "keyword arguments only"); return NULL; } \
        return vg_make(GF_OF(self), kind, NULL, NULL, kw);                                         \
    }
VG_MAKER(Gfx_grating, YV_K_GRATING)
VG_MAKER(Gfx_gabor, YV_K_GABOR)
VG_MAKER(Gfx_noise, YV_K_NOISE)
VG_MAKER(Gfx_dots, YV_K_DOTS)

static PyObject* Gfx_image(PyObject* self, PyObject* args, PyObject* kw) {
    PyObject *tex = NULL, *k2, *r;
    if (!PyArg_ParseTuple(args, "|O", &tex)) return NULL;
    if (!tex) return vg_make(GF_OF(self), YV_K_IMAGE, NULL, NULL, kw);
    k2 = kw ? PyDict_Copy(kw) : PyDict_New();
    if (!k2) return NULL;
    if (PyDict_SetItemString(k2, "tex", tex) < 0) { Py_DECREF(k2); return NULL; }
    r = vg_make(GF_OF(self), YV_K_IMAGE, NULL, NULL, k2);
    Py_DECREF(k2);
    return r;
}

/* instances(template, n, fields=("xy",), palette=None): a bytearray holds
 * the elements, so `elements` is a view that owns its memory. */
static PyObject* Gfx_instances(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "template", "n", "fields", "palette", NULL };
    GfxObject* o = GF_OF(self);
    PyObject *tmpl, *fields = NULL, *pal = NULL, *ba, *r;
    int n, code, npal = 0, i;
    char err[512], fl[160];
    const double* pv = NULL;
    vg_arena a;
    yv_stim* st;
    char* p;
    Py_ssize_t off;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "Oi|OO", kwlist, &tmpl, &n, &fields, &pal)) return NULL;
    if (!gf_ready(o)) return NULL;
    if (!PyObject_TypeCheck(tmpl, (PyTypeObject*)StimType) || ST_OF(tmpl)->gfx != self) {
        PyErr_SetString(PyExc_TypeError, "instances: the template is a Stim of this Gfx");
        return NULL;
    }
    if (n < 1 || n > (1 << 22)) { PyErr_SetString(VsArgumentError, "instances: n, 1 or more"); return NULL; }
    fl[0] = 0;
    memset(&a, 0, sizeof a);
    if (fields && fields != Py_None) {
        if (PyUnicode_Check(fields)) {
            const char* s = vg_str(fields, &a);
            if (!s) { vg_arena_free(&a); return NULL; }
            PyOS_snprintf(fl, sizeof fl, "%s", s);
        } else {
            Py_ssize_t m = PySequence_Size(fields), j;
            size_t len = 0;
            if (m < 0) { vg_arena_free(&a); return NULL; }
            for (j = 0; j < m; j++) {
                PyObject* it = PySequence_GetItem(fields, j);
                const char* s = it && PyUnicode_Check(it) ? vg_str(it, &a) : NULL;
                Py_XDECREF(it);
                if (!s) {
                    vg_arena_free(&a);
                    if (!PyErr_Occurred()) PyErr_SetString(VsArgumentError, "fields: names (xy, ori, phase, contrast, scale, gate, color)");
                    return NULL;
                }
                len += (size_t)PyOS_snprintf(fl + len, sizeof fl - len, "%s%s", j ? "," : "", s);
                if (len >= sizeof fl) break;
            }
        }
    }
    if (pal && pal != Py_None) {
        if (vg_doubles(pal, &a, &pv, &npal, "palette") < 0) { vg_arena_free(&a); return NULL; }
        if (npal % 3 || npal < 3) { vg_arena_free(&a); PyErr_SetString(VsArgumentError, "palette: (r, g, b) entries"); return NULL; }
        npal /= 3;
    }
    /* The elements: a bytearray, never resized, 16-byte aligned inside. */
    ba = PyByteArray_FromStringAndSize(NULL, (Py_ssize_t)n * (Py_ssize_t)sizeof(ygfx_inst) + 16);
    if (!ba) { vg_arena_free(&a); return NULL; }
    p = PyByteArray_AsString(ba);
    off = (Py_ssize_t)((16u - ((uintptr_t)p & 15u)) & 15u);
    SC_OF(o->screen)->busy = 1;
    Py_BEGIN_ALLOW_THREADS   /* the instanced program compiles now */
    st = yv_inst_make(ST_OF(tmpl)->st, n, fl, pv, npal, (ygfx_inst*)(void*)(p + off), &code, err, sizeof err);
    Py_END_ALLOW_THREADS
    SC_OF(o->screen)->busy = 0;
    vg_arena_free(&a);
    if (!st) { Py_DECREF(ba); return vg_raise(code, err); }
    r = vg_wrap_stim(self, st);
    if (!r) { Py_DECREF(ba); return NULL; }
    ST_OF(r)->mem = ba;
    ST_OF(r)->mem_off = off;
    vg_stim_refs(r, ST_OF(tmpl)->group, ST_OF(tmpl)->tex);
    (void)i;
    return r;
}

static PyObject* Gfx_list(PyObject* self, PyObject* args) {
    GfxObject* o = GF_OF(self);
    PyObject *items = NULL, *r;
    yv_list* l;
    char err[512];
    int code;
    if (!PyArg_ParseTuple(args, "|O", &items)) return NULL;
    if (!gf_ready(o)) return NULL;
    l = yv_list_make(o->g, &code, err, sizeof err);
    if (!l) return vg_raise(code, err);
    r = vg_new(ListType);
    if (!r) { yv_list_free(l); return NULL; }
    LS_OF(r)->l = l;
    Py_INCREF(self);
    LS_OF(r)->gfx = self;
    LS_OF(r)->items = PyList_New(0);
    if (!LS_OF(r)->items) { Py_DECREF(r); return NULL; }
    if (items && items != Py_None) {
        PyObject* res = PyObject_CallMethod(r, "extend", "O", items);
        if (!res) { Py_DECREF(r); return NULL; }
        Py_DECREF(res);
    }
    return r;
}

static PyObject* Gfx_group(PyObject* self, PyObject* args, PyObject* kw) {
    GfxObject* o = GF_OF(self);
    yv_kv kv[VG_MAX_KV];
    vg_arena a;
    char err[512];
    int n, code;
    yv_group* gr;
    PyObject* r;
    if (PyTuple_Size(args) > 0) { PyErr_SetString(PyExc_TypeError, "group(**fields): keyword arguments only"); return NULL; }
    if (!gf_ready(o)) return NULL;
    memset(&a, 0, sizeof a);
    n = vg_kvs(kw, kv, &a, 0);
    if (n < 0) { vg_arena_free(&a); return NULL; }
    gr = yv_group_make(o->g, kv, n, &code, err, sizeof err);
    vg_arena_free(&a);
    if (!gr) return vg_raise(code, err);
    r = vg_new(GroupType);
    if (!r) { yv_group_free(gr); return NULL; }
    GR_OF(r)->gr = gr;
    Py_INCREF(self);
    GR_OF(r)->gfx = self;
    return r;
}

/* ---- textures: pixels from a buffer (rows top first) ----------------------------------- */

static const struct { const char* name; int f; } vg_formats[] = {
    { "r8", YGFX_R8 }, { "rg8", YGFX_RG8 }, { "rgba8", YGFX_RGBA8 }, { "r16f", YGFX_R16F }, { "rgba16f", YGFX_RGBA16F },
    { "r32f", YGFX_R32F }, { "rgba32f", YGFX_RGBA32F }, { "r16ui", YGFX_R16UI }, { NULL, 0 } };

/* The pixels of obj in the texture's layout (a PyMem copy, *out), with w,
 * h, format from its shape and item format when not given: (h, w) or (h,
 * w, c), uint8 c 1, 2, 3 (to rgba8, alpha 255) or 4, float32 or float64
 * (to float32) c 1, 3 (alpha 1) or 4, uint16 c 1. */
static int vg_pixels(PyObject* obj, int* w, int* h, int* format, void** out) {
    PyObject *mv = NULL, *shape = NULL, *fmt_o = NULL, *fmt_b = NULL, *bytes = NULL, *isz = NULL;
    const char* fmt;
    long itemsize;
    Py_ssize_t nd, hh = 0, ww = 0, c = 1, nb, i, px;
    const unsigned char* p;
    unsigned char* dst = NULL;
    int f = *format, rc = -1;
    mv = PyMemoryView_FromObject(obj);
    if (!mv) { PyErr_Clear(); PyErr_SetString(VsArgumentError, "texture: pixels in a buffer (a NumPy array, bytes, array.array)"); return -1; }
    shape = PyObject_GetAttrString(mv, "shape");
    fmt_o = PyObject_GetAttrString(mv, "format");
    isz = PyObject_GetAttrString(mv, "itemsize");
    if (!shape || !fmt_o || !isz) goto done;
    fmt_b = PyUnicode_AsUTF8String(fmt_o);
    if (!fmt_b) goto done;
    fmt = PyBytes_AsString(fmt_b);
    if (fmt[0] == '@' || fmt[0] == '=' || fmt[0] == '<') fmt++;
    itemsize = PyLong_AsLong(isz);
    nd = PyTuple_Size(shape);
    if (nd >= 2) {
        hh = PyLong_AsSsize_t(PyTuple_GetItem(shape, 0));
        ww = PyLong_AsSsize_t(PyTuple_GetItem(shape, 1));
        if (nd == 3) c = PyLong_AsSsize_t(PyTuple_GetItem(shape, 2));
        if (nd > 3) { PyErr_SetString(VsArgumentError, "texture: (h, w) or (h, w, channels)"); goto done; }
    }
    bytes = PyObject_CallMethod(mv, "tobytes", NULL);
    if (!bytes) goto done;
    p = (const unsigned char*)PyBytes_AsString(bytes);
    nb = PyBytes_Size(bytes);
    if (nd < 2) {
        /* a flat buffer: w, h and format are given */
        int bpp = yv_format_bytes(f);
        if (!f || *w < 1 || *h < 1) { PyErr_SetString(VsArgumentError, "texture: a flat buffer needs w, h and format"); goto done; }
        if (f == YGFX_R16F || f == YGFX_RGBA16F) { if (itemsize != 4 || fmt[0] != 'f') { PyErr_SetString(VsArgumentError, "texture: r16f and rgba16f take float32 values"); goto done; } }
        if (nb != (Py_ssize_t)bpp * *w * *h) {
            PyErr_Format(VsArgumentError, "texture: %d x %d %s is %d bytes, the buffer has %zd", *w, *h, "texels", bpp * *w * *h, nb);
            goto done;
        }
        dst = (unsigned char*)PyMem_Malloc((size_t)nb);
        if (!dst) { PyErr_NoMemory(); goto done; }
        memcpy(dst, p, (size_t)nb);
        *out = dst;
        rc = 0;
        goto done;
    }
    if (hh < 1 || ww < 1 || hh > 65536 || ww > 65536) { PyErr_SetString(VsArgumentError, "texture: 1 to 65536 texels a side"); goto done; }
    px = hh * ww;
    if (fmt[0] == 'B' && itemsize == 1) {
        if (c < 1 || c > 4) { PyErr_SetString(VsArgumentError, "texture: uint8 with 1 to 4 channels"); goto done; }
        if (!f) f = c == 1 ? YGFX_R8 : c == 2 ? YGFX_RG8 : YGFX_RGBA8;
        if ((f == YGFX_R8 && c != 1) || (f == YGFX_RG8 && c != 2) || (f == YGFX_RGBA8 && c < 3) ||
            (f != YGFX_R8 && f != YGFX_RG8 && f != YGFX_RGBA8)) {
            PyErr_SetString(VsArgumentError, "texture: the format does not fit the uint8 array's channels");
            goto done;
        }
        dst = (unsigned char*)PyMem_Malloc((size_t)px * (size_t)yv_format_bytes(f));
        if (!dst) { PyErr_NoMemory(); goto done; }
        if (c == 3) {
            for (i = 0; i < px; i++) { dst[4 * i] = p[3 * i]; dst[4 * i + 1] = p[3 * i + 1]; dst[4 * i + 2] = p[3 * i + 2]; dst[4 * i + 3] = 255; }
        } else {
            memcpy(dst, p, (size_t)px * (size_t)c);
        }
    } else if ((fmt[0] == 'f' && itemsize == 4) || (fmt[0] == 'd' && itemsize == 8)) {
        float* fd;
        int oc;
        if (c != 1 && c != 3 && c != 4) { PyErr_SetString(VsArgumentError, "texture: float with 1, 3 or 4 channels"); goto done; }
        if (!f) f = c == 1 ? YGFX_R32F : YGFX_RGBA32F;
        oc = (f == YGFX_R32F || f == YGFX_R16F) ? 1 : 4;
        if ((oc == 1 && c != 1) || (oc == 4 && c == 1) || (f != YGFX_R32F && f != YGFX_R16F && f != YGFX_RGBA32F && f != YGFX_RGBA16F)) {
            PyErr_SetString(VsArgumentError, "texture: the format does not fit the float array's channels");
            goto done;
        }
        fd = (float*)PyMem_Malloc(sizeof(float) * (size_t)px * (size_t)oc);
        if (!fd) { PyErr_NoMemory(); goto done; }
        for (i = 0; i < px; i++) {
            Py_ssize_t k;
            for (k = 0; k < oc; k++) {
                double v;
                Py_ssize_t src = i * c + (k < c ? k : 0);
                if (k == 3 && c == 3) { fd[i * oc + k] = 1.0f; continue; }
                if (itemsize == 4) { float x; memcpy(&x, p + 4 * src, 4); v = x; }
                else memcpy(&v, p + 8 * src, 8);
                fd[i * oc + k] = (float)v;
            }
        }
        dst = (unsigned char*)fd;
    } else if (fmt[0] == 'H' && itemsize == 2) {
        if (c != 1 || (f && f != YGFX_R16UI)) { PyErr_SetString(VsArgumentError, "texture: uint16 is one channel (r16ui)"); goto done; }
        f = YGFX_R16UI;
        dst = (unsigned char*)PyMem_Malloc((size_t)px * 2);
        if (!dst) { PyErr_NoMemory(); goto done; }
        memcpy(dst, p, (size_t)px * 2);
    } else {
        PyErr_Format(VsArgumentError, "texture: uint8, uint16, float32 or float64 pixels, not '%s'", fmt);
        goto done;
    }
    *w = (int)ww;
    *h = (int)hh;
    *format = f;
    *out = dst;
    rc = 0;
done:
    Py_XDECREF(bytes);
    Py_XDECREF(fmt_b);
    Py_XDECREF(fmt_o);
    Py_XDECREF(shape);
    Py_XDECREF(isz);
    Py_XDECREF(mv);
    return rc;
}

static int vg_format_name(PyObject* o, int* f) {
    char s[16];
    int i;
    if (!o || o == Py_None) { *f = 0; return 0; }
    if (vs_utf8(o, s, sizeof s) < 0) return -1;
    for (i = 0; vg_formats[i].name; i++) if (yv_lower_eq(s, vg_formats[i].name)) { *f = vg_formats[i].f; return 0; }
    PyErr_SetString(VsArgumentError, "format: r8, rg8, rgba8, r16f, rgba16f, r32f, rgba32f or r16ui");
    return -1;
}

static PyObject* Gfx_texture(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "pixels", "w", "h", "format", "linear", "srgb", NULL };
    GfxObject* o = GF_OF(self);
    PyObject *pix, *fo = NULL, *r;
    int w = 0, h = 0, f = 0, linear = 0, srgb = 0, code;
    void* data = NULL;
    char err[512];
    yv_tex* t;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|iiOpp", kwlist, &pix, &w, &h, &fo, &linear, &srgb)) return NULL;
    if (!gf_ready(o)) return NULL;
    if (vg_format_name(fo, &f) < 0) return NULL;
    if (vg_pixels(pix, &w, &h, &f, &data) < 0) return NULL;
    t = yv_tex_make(o->g, w, h, f, data, linear, srgb, &code, err, sizeof err);
    PyMem_Free(data);
    if (!t) return vg_raise(code, err);
    r = vg_new(TexType);
    if (!r) { yv_tex_free(t); return NULL; }
    TX_OF(r)->t = t;
    Py_INCREF(self);
    TX_OF(r)->gfx = self;
    return r;
}

/* ---- timeline bindings --------------------------------------------------------------- */

/* A Timeline's C object, pinned (the owner's acquire()). */
static ytl_timeline* vg_acquire(PyObject* tl) {
    char err[256];
    ytl_timeline* p = (ytl_timeline*)ysp_timeline_api_ptr->head.acquire(tl, err, sizeof err);
    if (!p) PyErr_SetString(PyExc_TypeError, err);
    return p;
}

/* A channel: an int, or a name the Timeline knows. */
static int vg_channel(PyObject* tl, PyObject* c, int* out) {
    long v;
    if (PyUnicode_Check(c)) {
        PyObject* r = PyObject_CallMethod(tl, "channel", "O", c);
        if (!r) return -1;
        v = PyLong_AsLong(r);
        Py_DECREF(r);
    } else {
        v = PyLong_AsLong(c);
    }
    if (v == -1 && PyErr_Occurred()) return -1;
    if (v < 0 || v >= YTL_MAX_CHANNELS) { PyErr_Format(VsArgumentError, "bind: a channel is 0 to %d", YTL_MAX_CHANNELS - 1); return -1; }
    *out = (int)v;
    return 0;
}

static yv_obj* vg_target(GfxObject* o, PyObject* t) {
    PyObject* gfx = NULL;
    yv_obj* r = NULL;
    if (PyObject_TypeCheck(t, (PyTypeObject*)StimType)) { gfx = ST_OF(t)->gfx; r = &ST_OF(t)->st->o; }
    else if (PyObject_TypeCheck(t, (PyTypeObject*)ListType)) { gfx = LS_OF(t)->gfx; r = &LS_OF(t)->l->o; }
    else if (PyObject_TypeCheck(t, (PyTypeObject*)GroupType)) { gfx = GR_OF(t)->gfx; r = &GR_OF(t)->gr->o; }
    else { PyErr_SetString(PyExc_TypeError, "a Stim, DrawList or Group"); return NULL; }
    if (gfx != (PyObject*)o) { PyErr_SetString(VsArgumentError, "an object of another Gfx"); return NULL; }
    return r;
}

/* bind(target, name, timeline, channel, element=None) */
static PyObject* Gfx_bind(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "target", "name", "timeline", "channel", "element", NULL };
    GfxObject* o = GF_OF(self);
    PyObject *target, *tl, *chan, *elem = Py_None;
    const char* name;
    int chs[4096], n_ch = 0, first = -1, rc;
    int* ch = chs;
    char err[512];
    yv_obj* t;
    ytl_timeline* p;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OsOO|O", kwlist, &target, &name, &tl, &chan, &elem)) return NULL;
    if (!gf_ready(o)) return NULL;
    if (!(t = vg_target(o, target))) return NULL;
    if (elem != Py_None) {
        long e = PyLong_AsLong(elem);
        if (e == -1 && PyErr_Occurred()) return NULL;
        if (e < 0) { PyErr_SetString(VsArgumentError, "bind: element, 0 or more"); return NULL; }
        first = (int)e;
    }
    if (PyLong_Check(chan) || PyUnicode_Check(chan)) {
        if (vg_channel(tl, chan, &chs[0]) < 0) return NULL;
        n_ch = 1;
    } else {
        Py_ssize_t m = PySequence_Size(chan), i;
        if (m < 1) { if (!PyErr_Occurred()) PyErr_SetString(VsArgumentError, "bind: channel, an int, a name, or a sequence of them"); return NULL; }
        if (m > (Py_ssize_t)(sizeof chs / sizeof chs[0])) {
            ch = (int*)PyMem_Malloc(sizeof(int) * (size_t)m);
            if (!ch) return PyErr_NoMemory();
        }
        for (i = 0; i < m; i++) {
            PyObject* it = PySequence_GetItem(chan, i);
            int bad = !it || vg_channel(tl, it, &ch[i]) < 0;
            Py_XDECREF(it);
            if (bad) { if (ch != chs) PyMem_Free(ch); return NULL; }
        }
        n_ch = (int)m;
    }
    if (!(p = vg_acquire(tl))) { if (ch != chs) PyMem_Free(ch); return NULL; }
    rc = yv_bind(o->g, p, t, name, first, ch, n_ch, err, sizeof err);
    if (ch != chs) PyMem_Free(ch);
    if (rc < 0) return vg_raise(rc, err);
    Py_RETURN_NONE;
}

static PyObject* Gfx_unbind(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "target", "name", "timeline", NULL };
    GfxObject* o = GF_OF(self);
    PyObject *target = Py_None, *tl = Py_None;
    const char* name = NULL;
    yv_obj* t = NULL;
    ytl_timeline* p = NULL;
    int n;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|OzO", kwlist, &target, &name, &tl)) return NULL;
    if (!o->g) Py_RETURN_NONE;
    if (target != Py_None && !(t = vg_target(o, target))) return NULL;
    if (tl != Py_None) {
        if (!(p = vg_acquire(tl))) return NULL;
        ysp_timeline_api_ptr->head.release(p);   /* only its address is needed */
    }
    n = yv_unbind(o->g, t, name, p);
    return PyLong_FromLong(n);
}

/* timeline(tl, evaluate=True): the gfx evaluates tl at each frame's
 * predicted onset in begin() (frame()), before the bindings. */
static PyObject* Gfx_timeline(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "timeline", "evaluate", NULL };
    GfxObject* o = GF_OF(self);
    PyObject* tl;
    int on = 1, rc;
    char err[512];
    ytl_timeline* p;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|p", kwlist, &tl, &on)) return NULL;
    if (!gf_ready(o)) return NULL;
    if (!(p = vg_acquire(tl))) return NULL;
    rc = yv_tl_evaluate(o->g, p, on, err, sizeof err);
    if (rc < 0) return vg_raise(rc, err);
    Py_RETURN_NONE;
}

/* ---- the rest of Gfx ------------------------------------------------------------------- */

static PyObject* Gfx_prime(PyObject* self, PyObject* unused) {
    GfxObject* o = GF_OF(self);
    int rc;
    (void)unused;
    if (!gf_ready(o)) return NULL;
    if (o->g->in_frame) { PyErr_SetString(VsOrderError, "ysp_gfx: prime() outside a frame"); return NULL; }
    SC_OF(o->screen)->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    rc = ygfx_prime(&o->g->g);
    Py_END_ALLOW_THREADS
    SC_OF(o->screen)->busy = 0;
    if (rc < 0) { char err[512]; yv_gfx_err(o->g, rc, err, sizeof err); return vg_raise(rc, err); }
    return PyLong_FromLong(rc);
}

static PyObject* Gfx_describe(PyObject* self, PyObject* unused) {
    GfxObject* o = GF_OF(self);
    char line[2048];
    (void)unused;
    if (!gf_ready(o)) return NULL;
    ygfx_describe(&o->g->g, line, sizeof line);
    return PyUnicode_FromString(line);
}

/* read_scene(x, y, w, h) -> memoryview float32 (h, w, 4); read_output ->
 * uint8 (h, w, 4). They wait for the GPU: tests and checks, not trials.
 * The output is framebuffer 0 after end(), before the flip. */
static PyObject* vg_read(GfxObject* o, PyObject* args, int scene) {
    int x, y, w, h, rc;
    size_t bpp = scene ? 16 : 4;
    PyObject *ba, *mv, *c, *shape;
    if (!PyArg_ParseTuple(args, "iiii", &x, &y, &w, &h)) return NULL;
    if (!gf_ready(o)) return NULL;
    if (w < 1 || h < 1 || w > 16384 || h > 16384) { PyErr_SetString(VsArgumentError, "read: w, h 1 or more"); return NULL; }
    ba = PyByteArray_FromStringAndSize(NULL, (Py_ssize_t)((size_t)w * (size_t)h * bpp));
    if (!ba) return NULL;
    rc = scene ? ygfx_read_scene(&o->g->g, x, y, w, h, (float*)(void*)PyByteArray_AsString(ba))
               : ygfx_read_output(&o->g->g, x, y, w, h, (uint8_t*)PyByteArray_AsString(ba));
    if (rc < 0) { char err[512]; Py_DECREF(ba); yv_gfx_err(o->g, rc, err, sizeof err); return vg_raise(rc, err); }
    mv = PyMemoryView_FromObject(ba);
    Py_DECREF(ba);
    if (!mv) return NULL;
    shape = Py_BuildValue("(iii)", h, w, 4);
    c = shape ? PyObject_CallMethod(mv, "cast", "sO", scene ? "f" : "B", shape) : NULL;
    Py_XDECREF(shape);
    Py_DECREF(mv);
    return c;
}
static PyObject* Gfx_read_scene(PyObject* self, PyObject* args) { return vg_read(GF_OF(self), args, 1); }
static PyObject* Gfx_read_output(PyObject* self, PyObject* args) { return vg_read(GF_OF(self), args, 0); }

static PyObject* Gfx_c_scene(PyObject* self, PyObject* arg) {
    GfxObject* o = GF_OF(self);
    long which = PyLong_AsLong(arg);
    char err[512];
    int rc;
    if (which == -1 && PyErr_Occurred()) return NULL;
    if (!gf_ready(o)) return NULL;
    if (!o->g->in_frame) { PyErr_SetString(VsOrderError, "_c_scene: between begin() and end()"); return NULL; }
    rc = yv_gfx_c_scene(o->g, (int)which, err, sizeof err);
    if (rc < 0) return vg_raise(rc, err);
    Py_RETURN_NONE;
}

/* color(space, a, b, c) -> (r, g, b): through the gfx's ysp/color.h
 * context (the calibration, background, cones and luminance of open);
 * dir(...) gives a modulation direction. */
static PyObject* vg_color(GfxObject* o, PyObject* args, int is_dir) {
    const char* sp;
    double v[3];
    float in[3], out[3];
    int space, rc;
    char err[512];
    if (!PyArg_ParseTuple(args, "sddd", &sp, &v[0], &v[1], &v[2])) return NULL;
    if (!gf_ready(o)) return NULL;
    if (yv_space_id(sp, &space, err, sizeof err) < 0) return vg_raise(YGFX_ERR_ARG, err);
    in[0] = (float)v[0]; in[1] = (float)v[1]; in[2] = (float)v[2];
    rc = yv_space_rgb(o->g, space, is_dir, in, out, err, sizeof err);
    if (rc < 0) return vg_raise(rc, err);
    return Py_BuildValue("(ddd)", (double)out[0], (double)out[1], (double)out[2]);
}
static PyObject* Gfx_color(PyObject* self, PyObject* args) { return vg_color(GF_OF(self), args, 0); }
static PyObject* Gfx_dir(PyObject* self, PyObject* args) { return vg_color(GF_OF(self), args, 1); }

/* capture_start(path=None, every=0, rect=None, slots=0, manual=False) */
static PyObject* Gfx_capture_start(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "path", "every", "rect", "slots", "manual", NULL };
    GfxObject* o = GF_OF(self);
    const char* path = NULL;
    int every = 0, slots = 0, manual = 0, rc;
    PyObject* rect = Py_None;
    ygfx_capture_desc d;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|ziOip", kwlist, &path, &every, &rect, &slots, &manual)) return NULL;
    if (!gf_ready(o)) return NULL;
    memset(&d, 0, sizeof d);
    if (rect != Py_None && !PyArg_ParseTuple(rect, "iiii", &d.x, &d.y, &d.w, &d.h)) return NULL;
    d.every = every;
    d.slots = slots;
    d.path = path;
    d.manual = manual != 0;
    rc = ygfx_capture_start(&o->g->g, &d);
    if (rc < 0) { char err[512]; yv_gfx_err(o->g, rc, err, sizeof err); return vg_raise(rc, err); }
    Py_RETURN_NONE;
}

static PyObject* Gfx_capture_mark(PyObject* self, PyObject* unused) {
    GfxObject* o = GF_OF(self);
    int rc;
    (void)unused;
    if (!gf_ready(o)) return NULL;
    rc = ygfx_capture_mark(&o->g->g);
    if (rc < 0) { char err[512]; yv_gfx_err(o->g, rc, err, sizeof err); return vg_raise(rc, err); }
    Py_RETURN_NONE;
}

static PyObject* Gfx_capture_pump(PyObject* self, PyObject* args) {
    GfxObject* o = GF_OF(self);
    int max = 0, rc;
    if (!PyArg_ParseTuple(args, "|i", &max)) return NULL;
    if (!gf_ready(o)) return NULL;
    rc = ygfx_capture_pump(&o->g->g, max);
    if (rc < 0) { char err[512]; yv_gfx_err(o->g, rc, err, sizeof err); return vg_raise(rc, err); }
    return PyLong_FromLong(rc);
}

static PyObject* vg_capture_stats(const ygfx_capture_stats* s) {
    return Py_BuildValue("{s:K,s:K,s:K,s:K,s:K,s:K,s:i,s:i,s:i,s:O}", "chosen", (unsigned long long)s->chosen, "dropped",
                         (unsigned long long)s->dropped, "read", (unsigned long long)s->read, "written",
                         (unsigned long long)s->written, "failed", (unsigned long long)s->failed, "no_record",
                         (unsigned long long)s->no_record, "busy", (int)s->busy, "busy_max", (int)s->busy_max, "slots",
                         (int)s->slots, "manual", s->manual ? Py_True : Py_False);
}

static PyObject* Gfx_capture_stop(PyObject* self, PyObject* unused) {
    GfxObject* o = GF_OF(self);
    ygfx_capture_stats s;
    int rc;
    (void)unused;
    if (!gf_ready(o)) return NULL;
    memset(&s, 0, sizeof s);
    SC_OF(o->screen)->busy = 1;
    Py_BEGIN_ALLOW_THREADS   /* waits for the copies and the writer */
    rc = ygfx_capture_stop(&o->g->g, &s);
    Py_END_ALLOW_THREADS
    SC_OF(o->screen)->busy = 0;
    if (rc < 0) { char err[512]; yv_gfx_err(o->g, rc, err, sizeof err); return vg_raise(rc, err); }
    return vg_capture_stats(&s);
}

static PyObject* Gfx_capture_stats(PyObject* self, PyObject* unused) {
    GfxObject* o = GF_OF(self);
    ygfx_capture_stats s;
    (void)unused;
    if (!gf_ready(o)) return NULL;
    memset(&s, 0, sizeof s);
    ygfx_capture_get_stats(&o->g->g, &s);
    return vg_capture_stats(&s);
}

static PyObject* Gfx_programs(PyObject* self, PyObject* unused) {
    GfxObject* o = GF_OF(self);
    ygfx_programs p;
    (void)unused;
    if (!gf_ready(o)) return NULL;
    ygfx_program_stats(&o->g->g, &p);
    return Py_BuildValue("{s:I,s:I,s:I,s:I,s:I,s:d,s:d,s:s}", "loaded", p.loaded, "compiled", p.compiled, "rejected",
                         p.rejected, "stored", p.stored, "store_failed", p.store_failed, "store", yv_s(p.store_ns), "open",
                         yv_s(p.open_ns), "cache", o->g->cache_dir);
}

static PyObject* Gfx_get_open(PyObject* self, void* c) { (void)c; return PyBool_FromLong(GF_OF(self)->g && GF_OF(self)->g->open); }
static PyObject* Gfx_get_screen(PyObject* self, void* c) {
    PyObject* s = GF_OF(self)->screen ? GF_OF(self)->screen : Py_None;
    (void)c;
    Py_INCREF(s);
    return s;
}
static PyObject* Gfx_get_size(PyObject* self, void* c) {
    GfxObject* o = GF_OF(self);
    float w, h;
    (void)c;
    if (!gf_ready(o)) return NULL;
    ygfx_size(&o->g->g, &w, &h);
    return Py_BuildValue("(dd)", (double)w, (double)h);
}
static PyObject* Gfx_get_center(PyObject* self, void* c) {
    GfxObject* o = GF_OF(self);
    float x, y;
    (void)c;
    if (!gf_ready(o)) return NULL;
    ygfx_center(&o->g->g, &x, &y);
    return Py_BuildValue("(dd)", (double)x, (double)y);
}
static PyObject* Gfx_get_bindings(PyObject* self, void* c) { (void)c; return PyLong_FromLong(GF_OF(self)->g ? yv_bind_count(GF_OF(self)->g) : 0); }
static PyObject* Gfx_get_clipped(PyObject* self, void* c) {
    (void)c;
    return PyLong_FromUnsignedLongLong(GF_OF(self)->g && GF_OF(self)->g->open ? ygfx_clipped(&GF_OF(self)->g->g) : 0);
}
static PyObject* Gfx_get_calibrated(PyObject* self, void* c) {
    (void)c;
    return PyBool_FromLong(GF_OF(self)->g && GF_OF(self)->g->open && ygfx_calibrated(&GF_OF(self)->g->g));
}
static PyObject* Gfx_get_in_frame(PyObject* self, void* c) { (void)c; return PyBool_FromLong(GF_OF(self)->g && GF_OF(self)->g->in_frame); }

static PyMethodDef Gfx_methods[] = {
    { "close", Gfx_close, METH_NOARGS, "close(): frees the GL objects; the Screen stays open, and the stimuli of this Gfx raise ClosedError." },
    { "__enter__", Gfx_enter, METH_NOARGS, NULL },
    { "__exit__", Gfx_exit, METH_VARARGS, NULL },
    { "begin", Gfx_begin, METH_NOARGS,
      "begin(): starts drawing the Screen's current frame (after Screen.begin()): evaluates the timelines given to "
      "timeline() at the frame's predicted onset and writes every binding first." },
    { "draw", Gfx_draw, METH_VARARGS, "draw(*items): each Stim or DrawList, in order (between begin() and end())." },
    { "end", Gfx_end, METH_NOARGS, "end(): draws the queue and the output stage into the back buffer; then Screen.flip()." },
    { "frame", Gfx_frame, METH_VARARGS, "frame(*items): begin(), the bindings, draw(*items) and end() in one call." },
    { "rect", (PyCFunction)(void (*)(void))Gfx_rect, METH_VARARGS | METH_KEYWORDS,
      "rect(x, y, w, h, color=1.0): draws a filled rectangle now (phase A): center x, y in units from the screen's center." },
    { "shape", (PyCFunction)(void (*)(void))Gfx_shape, METH_VARARGS | METH_KEYWORDS,
      "shape(shape='rect', **params) -> Stim: rect, circle, annulus, line, polygon (vertices), cross, rrect, arc, pie, "
      "capsule, ngon, star, ellipse, qbezier or polyline (path)." },
    { "grating", (PyCFunction)(void (*)(void))Gfx_grating, METH_VARARGS | METH_KEYWORDS, "grating(**params) -> Stim: a sine or square grating in an aperture." },
    { "gabor", (PyCFunction)(void (*)(void))Gfx_gabor, METH_VARARGS | METH_KEYWORDS, "gabor(**params) -> Stim: sf, phase, sigma, size (0: 8 sigma), contrast, ..." },
    { "noise", (PyCFunction)(void (*)(void))Gfx_noise, METH_VARARGS | METH_KEYWORDS, "noise(**params) -> Stim: hash noise per check (dist uniform, binary, gaussian, simplex)." },
    { "image", (PyCFunction)(void (*)(void))Gfx_image, METH_VARARGS | METH_KEYWORDS, "image(tex, **params) -> Stim: a Texture, color or modulation." },
    { "dots", (PyCFunction)(void (*)(void))Gfx_dots, METH_VARARGS | METH_KEYWORDS,
      "dots(n=, positions=, **params) -> Stim: a dot field; positions (x, y pairs, units from its center) update between frames." },
    { "instances", (PyCFunction)(void (*)(void))Gfx_instances, METH_VARARGS | METH_KEYWORDS,
      "instances(template, n, fields=('xy',), palette=None) -> Stim: n elements of the template in one draw; "
      "elements is a float32 (n, 8) view: x, y, ori, phase, contrast, scale, gate, color." },
    { "list", Gfx_list, METH_VARARGS, "list(items=()) -> DrawList: stimuli drawn in order by one call." },
    { "group", (PyCFunction)(void (*)(void))Gfx_group, METH_VARARGS | METH_KEYWORDS,
      "group(**fields) -> Group: place, anchor, x, y, w, h, ori, scale, opacity, visible; a stimulus joins with group=." },
    { "texture", (PyCFunction)(void (*)(void))Gfx_texture, METH_VARARGS | METH_KEYWORDS,
      "texture(pixels, w=0, h=0, format=None, linear=False, srgb=False) -> Texture: pixels (h, w[, c]) uint8, uint16 or float, rows top first." },
    { "bind", (PyCFunction)(void (*)(void))Gfx_bind, METH_VARARGS | METH_KEYWORDS,
      "bind(target, name, timeline, channel, element=None): the timeline's channel(s) write target's name in each begin(). "
      "target: a Stim (color takes 3 channels), DrawList (a channel per item, or one), Group, or Instances elements from element." },
    { "unbind", (PyCFunction)(void (*)(void))Gfx_unbind, METH_VARARGS | METH_KEYWORDS,
      "unbind(target=None, name=None, timeline=None) -> int: removes the matching bindings (None matches all); a timeline left with none is released." },
    { "timeline", (PyCFunction)(void (*)(void))Gfx_timeline, METH_VARARGS | METH_KEYWORDS,
      "timeline(tl, evaluate=True): begin() evaluates tl at the frame's predicted onset (its events land in tl's record)." },
    { "prime", Gfx_prime, METH_NOARGS, "prime() -> int: draws every program once (open did; again after new kinds: instances, images)." },
    { "describe", Gfx_describe, METH_NOARGS, "describe() -> str: the gfx's log line." },
    { "read_scene", Gfx_read_scene, METH_VARARGS, "read_scene(x, y, w, h) -> memoryview float32 (h, w, 4): linear scene values; waits for the GPU." },
    { "read_output", Gfx_read_output, METH_VARARGS, "read_output(x, y, w, h) -> memoryview uint8 (h, w, 4): the codes, after end() and before the flip." },
    { "_c_scene", Gfx_c_scene, METH_O, "test: draws scene k made by ysp/gfx.h's constructors in C." },
    { "color", Gfx_color, METH_VARARGS, "color(space, a, b, c) -> (r, g, b): linear device RGB through the calibration." },
    { "dir", Gfx_dir, METH_VARARGS, "dir(space, a, b, c) -> (r, g, b): a modulation direction about the background." },
    { "capture_start", (PyCFunction)(void (*)(void))Gfx_capture_start, METH_VARARGS | METH_KEYWORDS,
      "capture_start(path=None, every=0, rect=None, slots=0, manual=False): the output of every Nth or marked frame to <path><index>.ppm." },
    { "capture_mark", Gfx_capture_mark, METH_NOARGS, "capture_mark(): capture this frame too (between begin() and end())." },
    { "capture_pump", Gfx_capture_pump, METH_VARARGS, "capture_pump(max=0) -> int: collects and (manual) writes, outside a frame." },
    { "capture_stop", Gfx_capture_stop, METH_NOARGS, "capture_stop() -> dict: waits, writes the rest, the counts." },
    { "capture_stats", Gfx_capture_stats, METH_NOARGS, "capture_stats() -> dict." },
    { "programs", Gfx_programs, METH_NOARGS, "programs() -> dict: what the open did with the program cache." },
    { NULL, NULL, 0, NULL }
};
static PyGetSetDef Gfx_getset[] = {
    { "open", Gfx_get_open, NULL, "True while open (a closed Screen closes its Gfx)", NULL },
    { "screen", Gfx_get_screen, NULL, "the Screen it draws on", NULL },
    { "size", Gfx_get_size, NULL, "(w, h): the screen in units", NULL },
    { "center", Gfx_get_center, NULL, "(x, y): the screen's center in px from the top-left", NULL },
    { "bindings", Gfx_get_bindings, NULL, "the timeline bindings", NULL },
    { "clipped", Gfx_get_clipped, NULL, "draws that may have left 0..1 since open", NULL },
    { "calibrated", Gfx_get_calibrated, NULL, "opened with a calibration", NULL },
    { "in_frame", Gfx_get_in_frame, NULL, "between begin() and end()", NULL },
    { NULL, NULL, NULL, NULL, NULL }
};
static PyType_Slot Gfx_slots[] = {
    { Py_tp_doc, "Gfx(screen, **options): stimuli on a Screen (ysp/gfx.h). options: background, units ('px' or 'deg'), "
                 "view (distance_mm, width_mm), dither, seed, cones, cal and lum (the bytes of .yspcal and .ysplum files), "
                 "max_draws, max_instances, cache (True: the per-user program cache; False; or a folder), prime." },
    { Py_tp_new, PyType_GenericNew },
    { Py_tp_init, Gfx_init },
    { Py_tp_dealloc, Gfx_dealloc },
    { Py_tp_methods, Gfx_methods },
    { Py_tp_getset, Gfx_getset },
    { 0, NULL }
};
static PyType_Spec Gfx_spec = { "ysp.gfx.Gfx", sizeof(GfxObject), 0, Py_TPFLAGS_DEFAULT, Gfx_slots };

/* ---- Stim ---------------------------------------------------------------------------------- */

static void Stim_dealloc(PyObject* self) {
    StimObject* o = ST_OF(self);
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    /* the core first: it drops the bindings that point into mem */
    yv_stim_free(o->st);
    o->st = NULL;
    Py_CLEAR(o->mem);
    Py_CLEAR(o->group);
    Py_CLEAR(o->tex);
    Py_CLEAR(o->gfx);
    fr(self);
    Py_DECREF(tp);
}

static const char* vg_kind_name(const yv_stim* st) {
    static const char* names[] = { "?", "shape", "grating", "gabor", "noise", "image", "dots", "instances" };
    return st->vkind >= 1 && st->vkind <= 7 ? names[st->vkind] : "?";
}

/* A value as Python: one number, a tuple, or an enum's name. */
static PyObject* vg_attr_value(const yv_stim* st, const yv_attr* a) {
    double v[32];
    int n, i;
    char err[256];
    if (yv_attr_get(st, a, v, &n, err, sizeof err) < 0) return vg_raise(YGFX_ERR_ARG, err);
    if (a->type == YV_A_ENUM) {
        const ygfx_param* t = ygfx_enum_params(NULL);
        char buf[48];
        if (!strcmp(t[a->row].type, "bool")) return PyBool_FromLong(v[0] != 0);
        if (!t[a->row].values) return PyLong_FromLong((long)v[0]);
        return PyUnicode_FromString(yv_enum_name(t[a->row].values, (int)v[0], buf, sizeof buf));
    }
    if (n == 1 && a->type == YV_A_FLOAT) return PyFloat_FromDouble(v[0]);
    if (n == 1) return PyFloat_FromDouble(v[0]);
    {
        PyObject* t = PyTuple_New(n);
        for (i = 0; t && i < n; i++) PyTuple_SetItem(t, i, PyFloat_FromDouble(v[i]));
        return t;
    }
}

static PyObject* vg_elements_view(StimObject* o) {
    PyObject *mv, *a, *b, *sl, *part, *c, *shape;
    Py_ssize_t nb = (Py_ssize_t)o->st->s.n_inst * (Py_ssize_t)sizeof(ygfx_inst);
    if (!(mv = PyMemoryView_FromObject(o->mem))) return NULL;
    a = PyLong_FromSsize_t(o->mem_off);
    b = PyLong_FromSsize_t(o->mem_off + nb);
    sl = a && b ? PySlice_New(a, b, NULL) : NULL;
    part = sl ? PyObject_GetItem(mv, sl) : NULL;
    Py_XDECREF(a); Py_XDECREF(b); Py_XDECREF(sl);
    Py_DECREF(mv);
    if (!part) return NULL;
    shape = Py_BuildValue("(ii)", (int)o->st->s.n_inst, 8);
    c = shape ? PyObject_CallMethod(part, "cast", "sO", "f", shape) : NULL;
    Py_XDECREF(shape);
    Py_DECREF(part);
    return c;
}

static PyObject* Stim_getattro(PyObject* self, PyObject* name) {
    StimObject* o = ST_OF(self);
    PyObject *r = PyObject_GenericGetAttr(self, name), *b;
    const char* s;
    yv_attr a;
    char err[256];
    if (r || !PyErr_ExceptionMatches(PyExc_AttributeError)) return r;
    PyErr_Clear();
    if (!o->st) { PyErr_SetString(VsClosedError, "the stimulus is gone"); return NULL; }
    b = PyUnicode_AsUTF8String(name);
    if (!b) return NULL;
    s = PyBytes_AsString(b);
    if (!strcmp(s, "kind")) r = PyUnicode_FromString(vg_kind_name(o->st));
    else if (!strcmp(s, "group")) { r = o->group ? o->group : Py_None; Py_INCREF(r); }
    else if (!strcmp(s, "tex")) { r = o->tex ? o->tex : Py_None; Py_INCREF(r); }
    else if (!strcmp(s, "gfx")) { r = o->gfx; Py_INCREF(r); }
    else if (!strcmp(s, "elements") && o->mem) r = vg_elements_view(o);
    else if (!strcmp(s, "n")) r = PyLong_FromUnsignedLong(o->st->vkind == YV_K_DOTS ? o->st->dots_cap : o->st->s.n_inst);
    else if (!strcmp(s, "count") && o->st->vkind == YV_K_DOTS) r = PyLong_FromUnsignedLong(o->st->s.count);
    else if (!strcmp(s, "anchor")) r = Py_BuildValue("(dd)", (double)o->st->s.ax, (double)o->st->s.ay);
    else if (!strcmp(s, "shape") && o->st->s.kind == YGFX_SHAPE) {
        char buf[32];
        r = PyUnicode_FromString(yv_enum_name(ygfx_enum_params(NULL)[YGFX_E_SHAPE].values, o->st->s.shape, buf, sizeof buf));
    } else if (!strcmp(s, "path") && o->st->path) {
        uint32_t i;
        r = PyList_New((Py_ssize_t)o->st->s.n_path);
        for (i = 0; r && i < o->st->s.n_path; i++)
            PyList_SetItem(r, (Py_ssize_t)i, Py_BuildValue("(dd)", (double)o->st->path[2 * i], (double)o->st->path[2 * i + 1]));
    } else if (yv_attr_find(s, o->st->s.kind, o->st->s.shape, &a, err, sizeof err) == 0 && a.type != YV_A_SPECIAL) {
        r = vg_attr_value(o->st, &a);
    } else {
        PyErr_Format(PyExc_AttributeError, "'%s' stimulus has no attribute '%s'", vg_kind_name(o->st), s);
    }
    Py_DECREF(b);
    return r;
}

static int vg_stim_set1(StimObject* o, const char* name, PyObject* v) {
    yv_kv k;
    vg_arena a;
    char err[512];
    int rc;
    if (!o->st) { PyErr_SetString(VsClosedError, "the stimulus is gone"); return -1; }
    if (!vg_obj_ready(o->gfx)) return -1;
    memset(&a, 0, sizeof a);
    if (!strcmp(name, "group") && v == Py_None) {
        yv_set_group(o->st, NULL);
        Py_CLEAR(o->group);
        return 0;
    }
    rc = vg_kv(name, v, &k, &a);
    if (rc != 0) {
        vg_arena_free(&a);
        if (rc > 0) PyErr_Format(VsArgumentError, "%s: None is not a value", name);
        return -1;
    }
    rc = yv_set_kv(o->st, &k, 0, err, sizeof err);
    vg_arena_free(&a);
    if (rc < 0) { vg_raise(rc, err); return -1; }
    if (!strcmp(name, "group")) vg_stim_refs((PyObject*)o, v, NULL);
    if (!strcmp(name, "tex")) vg_stim_refs((PyObject*)o, NULL, v);
    return 0;
}

static int Stim_setattro(PyObject* self, PyObject* name, PyObject* v) {
    PyObject* b;
    int rc;
    if (!v) { PyErr_SetString(PyExc_AttributeError, "a stimulus's parameters cannot be deleted"); return -1; }
    b = PyUnicode_AsUTF8String(name);
    if (!b) return -1;
    if (ST_OF(self)->st) {
        /* an unknown name is an AttributeError, as for any object */
        yv_attr a;
        char err[256];
        const yv_stim* st = ST_OF(self)->st;
        if (yv_attr_find(PyBytes_AsString(b), st->s.kind, st->s.shape, &a, err, sizeof err) < 0 &&
            !strncmp(err, "no parameter", 12)) {
            PyErr_Format(PyExc_AttributeError, "'%s' stimulus has no attribute '%s'", vg_kind_name(st), PyBytes_AsString(b));
            Py_DECREF(b);
            return -1;
        }
    }
    rc = vg_stim_set1(ST_OF(self), PyBytes_AsString(b), v);
    Py_DECREF(b);
    return rc;
}

/* set(**params): several at once, as the makers take them (space= too). */
static PyObject* Stim_set(PyObject* self, PyObject* args, PyObject* kw) {
    StimObject* o = ST_OF(self);
    yv_kv kv[VG_MAX_KV];
    vg_arena a;
    char err[512];
    int n, rc;
    if (PyTuple_Size(args) > 0) { PyErr_SetString(PyExc_TypeError, "set(**params): keyword arguments only"); return NULL; }
    if (!o->st) { PyErr_SetString(VsClosedError, "the stimulus is gone"); return NULL; }
    if (!vg_obj_ready(o->gfx)) return NULL;
    memset(&a, 0, sizeof a);
    n = vg_kvs(kw, kv, &a, 0);
    if (n < 0) { vg_arena_free(&a); return NULL; }
    rc = yv_set_rest(o->st, kv, n, err, sizeof err);
    vg_arena_free(&a);
    if (rc < 0) return vg_raise(rc, err);
    if (kw && PyDict_GetItemString(kw, "group")) vg_stim_refs(self, PyDict_GetItemString(kw, "group"), NULL);
    if (kw && PyDict_GetItemString(kw, "tex")) vg_stim_refs(self, NULL, PyDict_GetItemString(kw, "tex"));
    Py_RETURN_NONE;
}

static PyObject* Stim_copy(PyObject* self, PyObject* unused) {
    StimObject* o = ST_OF(self);
    char err[512];
    int code;
    yv_stim* st;
    PyObject* r;
    (void)unused;
    if (!o->st) { PyErr_SetString(VsClosedError, "the stimulus is gone"); return NULL; }
    if (!vg_obj_ready(o->gfx)) return NULL;
    st = yv_stim_copy(o->st, &code, err, sizeof err);
    if (!st) return vg_raise(code, err);
    r = vg_wrap_stim(o->gfx, st);
    if (r) vg_stim_refs(r, o->group, o->tex);
    return r;
}

static int vg_xy(PyObject* args, float* x, float* y) {
    double a, b;
    if (!PyArg_ParseTuple(args, "dd", &a, &b)) return -1;
    *x = (float)a;
    *y = (float)b;
    return 0;
}

static PyObject* Stim_hit(PyObject* self, PyObject* args) {
    StimObject* o = ST_OF(self);
    float x, y;
    if (vg_xy(args, &x, &y) < 0) return NULL;
    if (!o->st || !vg_obj_ready(o->gfx)) return NULL;
    return PyBool_FromLong(yv_stim_hit(o->st, x, y) >= 0);
}

static PyObject* Stim_hit_index(PyObject* self, PyObject* args) {
    StimObject* o = ST_OF(self);
    float x, y;
    if (vg_xy(args, &x, &y) < 0) return NULL;
    if (!o->st || !vg_obj_ready(o->gfx)) return NULL;
    if (o->st->vkind != YV_K_INSTANCES) { PyErr_SetString(VsArgumentError, "hit_index: instances only (hit() for the rest)"); return NULL; }
    return PyLong_FromLong(yv_stim_hit(o->st, x, y));
}

static PyObject* Stim_bounds(PyObject* self, PyObject* unused) {
    StimObject* o = ST_OF(self);
    float a, b, c, d;
    (void)unused;
    if (!o->st || !vg_obj_ready(o->gfx)) return NULL;
    ygfx_bounds(&GF_OF(o->gfx)->g->g, &o->st->s, &a, &b, &c, &d);
    return Py_BuildValue("(dddd)", (double)a, (double)b, (double)c, (double)d);
}

static PyObject* Stim_resolve(PyObject* self, PyObject* args) {
    StimObject* o = ST_OF(self);
    float x, y;
    int i = -1;
    if (!PyArg_ParseTuple(args, "|i", &i)) return NULL;
    if (!o->st || !vg_obj_ready(o->gfx)) return NULL;
    if (i >= 0) {
        if (o->st->vkind != YV_K_INSTANCES || i >= (int)o->st->s.n_inst) { PyErr_SetString(VsArgumentError, "resolve(i): an element of instances"); return NULL; }
        ygfx_inst_resolve(&GF_OF(o->gfx)->g->g, &o->st->s, i, &x, &y);
    } else {
        ygfx_resolve(&GF_OF(o->gfx)->g->g, &o->st->s, &x, &y);
    }
    return Py_BuildValue("(dd)", (double)x, (double)y);
}

/* set_elements(name, values, first=0): an element field of instances
 * (x, y, xy, ori, phase, contrast, scale, gate, color). */
static PyObject* Stim_set_elements(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "name", "values", "first", NULL };
    StimObject* o = ST_OF(self);
    const char* name;
    PyObject* vals;
    int first = 0, n, rc;
    const double* v;
    vg_arena a;
    char err[512];
    if (!PyArg_ParseTupleAndKeywords(args, kw, "sO|i", kwlist, &name, &vals, &first)) return NULL;
    if (!o->st || !vg_obj_ready(o->gfx)) return NULL;
    memset(&a, 0, sizeof a);
    if (vg_doubles(vals, &a, &v, &n, name) < 0) { vg_arena_free(&a); return NULL; }
    rc = yv_inst_set(o->st, name, first, v, n, err, sizeof err);
    vg_arena_free(&a);
    if (rc < 0) return vg_raise(rc, err);
    Py_RETURN_NONE;
}

static PyObject* Stim_length(PyObject* self, PyObject* unused) {
    StimObject* o = ST_OF(self);
    double l;
    (void)unused;
    if (!o->st || !vg_obj_ready(o->gfx)) return NULL;
    l = ygfx_length(&GF_OF(o->gfx)->g->g, &o->st->s);
    if (l < 0) return vg_raise(YGFX_ERR_ARG, "length: this stimulus has no length (a cross, a compound, not a shape)");
    return PyFloat_FromDouble(l);
}

static PyObject* Stim_dir(PyObject* self, PyObject* unused) {
    StimObject* o = ST_OF(self);
    PyObject* l = PyList_New(0);
    const ygfx_param* t;
    int n, i;
    (void)unused;
    if (!l || !o->st) return l;
    t = ygfx_params(&n);
    for (i = 0; i < n; i++)
        if (t[i].kinds & YV_KB(o->st->s.kind)) {
            PyObject* s = PyUnicode_FromString(t[i].name);
            if (!s || PyList_Append(l, s) < 0) { Py_XDECREF(s); Py_DECREF(l); return NULL; }
            Py_DECREF(s);
        }
    t = ygfx_enum_params(&n);
    for (i = 0; i < n; i++)
        if (t[i].kinds & YV_KB(o->st->s.kind)) {
            PyObject* s = PyUnicode_FromString(t[i].name);
            if (!s || PyList_Append(l, s) < 0) { Py_XDECREF(s); Py_DECREF(l); return NULL; }
            Py_DECREF(s);
        }
    return l;
}

static PyMethodDef Stim_methods[] = {
    { "set", (PyCFunction)(void (*)(void))Stim_set, METH_VARARGS | METH_KEYWORDS, "set(**params): several parameters at once (space= converts color and dir)." },
    { "copy", Stim_copy, METH_NOARGS, "copy() -> Stim: the same parameters, its own path." },
    { "hit", Stim_hit, METH_VARARGS, "hit(x, y) -> bool: screen px (top-left origin) inside, where the GPU's coverage is 0.5 or more." },
    { "hit_index", Stim_hit_index, METH_VARARGS, "hit_index(x, y) -> int: instances: the topmost element there, -1 for none." },
    { "bounds", Stim_bounds, METH_NOARGS, "bounds() -> (x0, y0, x1, y1): the turned box's bounds, screen px." },
    { "resolve", Stim_resolve, METH_VARARGS, "resolve(i=-1) -> (x, y): the anchor on the screen (instances: element i's)." },
    { "set_elements", (PyCFunction)(void (*)(void))Stim_set_elements, METH_VARARGS | METH_KEYWORDS,
      "set_elements(name, values, first=0): instances: an element field for each element from first (one value: all)." },
    { "length", Stim_length, METH_NOARGS, "length() -> float: px along the boundary or center line (dashes, trim)." },
    { "__dir__", Stim_dir, METH_NOARGS, NULL },
    { NULL, NULL, 0, NULL }
};
static PyType_Slot Stim_slots[] = {
    { Py_tp_doc, "A stimulus of a Gfx (Gfx.shape(), grating(), gabor(), noise(), image(), dots(), instances()). Its "
                 "parameters are attributes by ysp/gfx.h's parameter names (x, y, w, h, ori, contrast, color, edge, ...); "
                 "ysp.gfx.params(kind) lists them." },
    { Py_tp_dealloc, Stim_dealloc },
    { Py_tp_getattro, Stim_getattro },
    { Py_tp_setattro, Stim_setattro },
    { Py_tp_methods, Stim_methods },
    { 0, NULL }
};
static PyType_Spec Stim_spec = { "ysp.gfx.Stim", sizeof(StimObject), 0, Py_TPFLAGS_DEFAULT, Stim_slots };

/* ---- DrawList ------------------------------------------------------------------------------ */

static void List_dealloc(PyObject* self) {
    ListObject* o = LS_OF(self);
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    if (o->l) { yv_list_free(o->l); o->l = NULL; }
    Py_CLEAR(o->items);
    Py_CLEAR(o->gfx);
    fr(self);
    Py_DECREF(tp);
}

static PyObject* List_append(PyObject* self, PyObject* st) {
    ListObject* o = LS_OF(self);
    char err[512];
    int rc;
    if (!PyObject_TypeCheck(st, (PyTypeObject*)StimType)) { PyErr_SetString(PyExc_TypeError, "append: a Stim"); return NULL; }
    if (ST_OF(st)->gfx != o->gfx) { PyErr_SetString(VsArgumentError, "append: a stimulus of another Gfx"); return NULL; }
    if (!vg_obj_ready(o->gfx)) return NULL;
    rc = yv_list_add(o->l, ST_OF(st)->st, err, sizeof err);
    if (rc < 0) return vg_raise(rc, err);
    if (PyList_Append(o->items, st) < 0) { yv_list_remove(o->l, o->l->n - 1, err, sizeof err); return NULL; }
    Py_RETURN_NONE;
}

static PyObject* List_extend(PyObject* self, PyObject* seq) {
    PyObject *it = PyObject_GetIter(seq), *x;
    if (!it) return NULL;
    while ((x = PyIter_Next(it))) {
        PyObject* r = List_append(self, x);
        Py_DECREF(x);
        if (!r) { Py_DECREF(it); return NULL; }
        Py_DECREF(r);
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) return NULL;
    Py_RETURN_NONE;
}

static PyObject* List_clear(PyObject* self, PyObject* unused) {
    ListObject* o = LS_OF(self);
    (void)unused;
    if (o->l) yv_list_clear(o->l);
    if (PyList_SetSlice(o->items, 0, PyList_Size(o->items), NULL) < 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject* List_pop(PyObject* self, PyObject* args) {
    ListObject* o = LS_OF(self);
    Py_ssize_t i = -1, n = PyList_Size(o->items);
    PyObject* x;
    char err[256];
    if (!PyArg_ParseTuple(args, "|n", &i)) return NULL;
    if (i < 0) i += n;
    if (i < 0 || i >= n) { PyErr_SetString(PyExc_IndexError, "pop index out of range"); return NULL; }
    x = PyList_GetItem(o->items, i);
    Py_INCREF(x);
    yv_list_remove(o->l, (int)i, err, sizeof err);
    if (PyList_SetSlice(o->items, i, i + 1, NULL) < 0) { Py_DECREF(x); return NULL; }
    return x;
}

/* set(name, values, space=None): values for every item (n, or n x k), or
 * one value (k) for all. */
static PyObject* List_set(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "name", "values", "space", NULL };
    ListObject* o = LS_OF(self);
    const char *name, *sp = NULL;
    PyObject* vals;
    const double* v;
    int n, rc, space = 0;
    vg_arena a;
    char err[512];
    if (!PyArg_ParseTupleAndKeywords(args, kw, "sO|z", kwlist, &name, &vals, &sp)) return NULL;
    if (!vg_obj_ready(o->gfx)) return NULL;
    if (sp && yv_space_id(sp, &space, err, sizeof err) < 0) return vg_raise(YGFX_ERR_ARG, err);
    memset(&a, 0, sizeof a);
    if (vg_doubles(vals, &a, &v, &n, name) < 0) { vg_arena_free(&a); return NULL; }
    rc = yv_list_set(o->l, name, v, n, space, err, sizeof err);
    vg_arena_free(&a);
    if (rc < 0) return vg_raise(rc, err);
    Py_RETURN_NONE;
}

/* get(name) -> memoryview float64, (n,) or (n, k): a copy, owned by the view. */
static PyObject* List_get(PyObject* self, PyObject* args) {
    ListObject* o = LS_OF(self);
    const char* name;
    double* buf;
    int k, rc, n;
    char err[512];
    PyObject *ba, *mv, *c, *shape;
    if (!PyArg_ParseTuple(args, "s", &name)) return NULL;
    if (!vg_obj_ready(o->gfx)) return NULL;
    n = o->l->n;
    ba = PyByteArray_FromStringAndSize(NULL, (Py_ssize_t)(sizeof(double) * 32 * (size_t)(n > 0 ? n : 1)));
    if (!ba) return NULL;
    buf = (double*)(void*)PyByteArray_AsString(ba);
    rc = yv_list_get(o->l, name, buf, &k, err, sizeof err);
    if (rc < 0) { Py_DECREF(ba); return vg_raise(rc, err); }
    if (PyByteArray_Resize(ba, (Py_ssize_t)(sizeof(double) * (size_t)k * (size_t)n)) < 0) { Py_DECREF(ba); return NULL; }
    mv = PyMemoryView_FromObject(ba);
    Py_DECREF(ba);
    if (!mv) return NULL;
    shape = k == 1 ? Py_BuildValue("(i)", n) : Py_BuildValue("(ii)", n, k);
    c = shape ? PyObject_CallMethod(mv, "cast", "sO", "d", shape) : NULL;
    Py_XDECREF(shape);
    Py_DECREF(mv);
    return c;
}

static PyObject* List_hit(PyObject* self, PyObject* args) {
    ListObject* o = LS_OF(self);
    float x, y;
    int e;
    if (vg_xy(args, &x, &y) < 0) return NULL;
    if (!vg_obj_ready(o->gfx)) return NULL;
    return PyLong_FromLong(yv_list_hit(o->l, x, y, &e));
}

static PyObject* List_hit_element(PyObject* self, PyObject* args) {
    ListObject* o = LS_OF(self);
    float x, y;
    int e, i;
    if (vg_xy(args, &x, &y) < 0) return NULL;
    if (!vg_obj_ready(o->gfx)) return NULL;
    i = yv_list_hit(o->l, x, y, &e);
    return Py_BuildValue("(ii)", i, e);
}

static Py_ssize_t List_len(PyObject* self) { return PyList_Size(LS_OF(self)->items); }
static PyObject* List_item(PyObject* self, Py_ssize_t i) {
    PyObject* x;
    if (i < 0 || i >= PyList_Size(LS_OF(self)->items)) { PyErr_SetString(PyExc_IndexError, "DrawList index out of range"); return NULL; }
    x = PyList_GetItem(LS_OF(self)->items, i);
    Py_XINCREF(x);
    return x;
}
static PyObject* List_iter(PyObject* self) { return PyObject_GetIter(LS_OF(self)->items); }

static PyMethodDef List_methods[] = {
    { "append", List_append, METH_O, "append(stim)." },
    { "extend", List_extend, METH_O, "extend(stims)." },
    { "clear", List_clear, METH_NOARGS, "clear()." },
    { "pop", List_pop, METH_VARARGS, "pop(i=-1) -> Stim." },
    { "set", (PyCFunction)(void (*)(void))List_set, METH_VARARGS | METH_KEYWORDS,
      "set(name, values, space=None): name of every item in one call: n values (n x k for color and the like), or one for all." },
    { "get", List_get, METH_VARARGS, "get(name) -> memoryview float64 (n,) or (n, k): every item's value." },
    { "hit", List_hit, METH_VARARGS, "hit(x, y) -> int: the topmost visible item under screen px x, y; -1 for none." },
    { "hit_element", List_hit_element, METH_VARARGS, "hit_element(x, y) -> (item, element): with the instances' element." },
    { NULL, NULL, 0, NULL }
};
static PyType_Slot List_slots[] = {
    { Py_tp_doc, "DrawList: stimuli of one Gfx drawn in order by one call (Gfx.draw(), Gfx.frame()), set in bulk (set()), "
                 "bound to timeline channels in one bind()." },
    { Py_tp_dealloc, List_dealloc },
    { Py_tp_methods, List_methods },
    { Py_sq_length, List_len },
    { Py_sq_item, List_item },
    { Py_tp_iter, List_iter },
    { 0, NULL }
};
static PyType_Spec List_spec = { "ysp.gfx.DrawList", sizeof(ListObject), 0, Py_TPFLAGS_DEFAULT, List_slots };

/* ---- Group ------------------------------------------------------------------------------------ */

static void Group_dealloc(PyObject* self) {
    GroupObject* o = GR_OF(self);
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    if (o->gr) { yv_group_free(o->gr); o->gr = NULL; }
    Py_CLEAR(o->gfx);
    fr(self);
    Py_DECREF(tp);
}

static PyObject* Group_getattro(PyObject* self, PyObject* name) {
    GroupObject* o = GR_OF(self);
    PyObject *r = PyObject_GenericGetAttr(self, name), *b;
    double v;
    char err[256];
    if (r || !PyErr_ExceptionMatches(PyExc_AttributeError)) return r;
    PyErr_Clear();
    b = PyUnicode_AsUTF8String(name);
    if (!b) return NULL;
    if (!strcmp(PyBytes_AsString(b), "anchor")) r = Py_BuildValue("(dd)", (double)o->gr->grp.ax, (double)o->gr->grp.ay);
    else if (yv_group_get(o->gr, PyBytes_AsString(b), &v, err, sizeof err) == 0) r = PyFloat_FromDouble(v);
    else PyErr_Format(PyExc_AttributeError, "Group has no attribute '%s'", PyBytes_AsString(b));
    Py_DECREF(b);
    return r;
}

static int Group_setattro(PyObject* self, PyObject* name, PyObject* v) {
    GroupObject* o = GR_OF(self);
    yv_kv k;
    vg_arena a;
    char err[512];
    PyObject* b;
    int rc;
    if (!v) { PyErr_SetString(PyExc_AttributeError, "cannot delete"); return -1; }
    if (!vg_obj_ready(o->gfx)) return -1;
    b = PyUnicode_AsUTF8String(name);
    if (!b) return -1;
    memset(&a, 0, sizeof a);
    rc = vg_kv(PyBytes_AsString(b), v, &k, &a);
    if (rc == 0) {
        rc = yv_group_set(o->gr, &k, err, sizeof err);
        if (rc < 0) PyErr_SetString(strncmp(err, "no group", 8) ? VsArgumentError : PyExc_AttributeError, err);
    } else if (rc > 0) {
        PyErr_SetString(VsArgumentError, "None is not a value");
        rc = -1;
    }
    vg_arena_free(&a);
    Py_DECREF(b);
    return rc < 0 ? -1 : 0;
}

static PyType_Slot Group_slots[] = {
    { Py_tp_doc, "Group: a flat group (ysp/gfx.h GROUPS): place, anchor, x, y, w, h, ori, scale, opacity, visible. "
                 "A member's place, x, y and ori are in the group's frame; bind its fields to move every member." },
    { Py_tp_dealloc, Group_dealloc },
    { Py_tp_getattro, Group_getattro },
    { Py_tp_setattro, Group_setattro },
    { 0, NULL }
};
static PyType_Spec Group_spec = { "ysp.gfx.Group", sizeof(GroupObject), 0, Py_TPFLAGS_DEFAULT, Group_slots };

/* ---- Texture ---------------------------------------------------------------------------------- */

static void Tex_dealloc(PyObject* self) {
    TexObject* o = TX_OF(self);
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    if (o->t) { yv_tex_free(o->t); o->t = NULL; }
    Py_CLEAR(o->gfx);
    fr(self);
    Py_DECREF(tp);
}

/* update(pixels, x=0, y=0): a rectangle of new texels, between frames. */
static PyObject* Tex_update(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "pixels", "x", "y", "w", "h", NULL };
    TexObject* o = TX_OF(self);
    PyObject* pix;
    int x = 0, y = 0, w = 0, h = 0, f, rc;
    void* data = NULL;
    char err[512];
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|iiii", kwlist, &pix, &x, &y, &w, &h)) return NULL;
    if (!vg_obj_ready(o->gfx)) return NULL;
    f = o->t->format;
    if (!w && !h) { w = o->t->w - x; h = o->t->h - y; }
    if (vg_pixels(pix, &w, &h, &f, &data) < 0) return NULL;
    if (f != o->t->format) { PyMem_Free(data); PyErr_SetString(VsArgumentError, "update: the pixels' format is not the texture's"); return NULL; }
    rc = yv_tex_update(o->t, x, y, w, h, data, err, sizeof err);
    PyMem_Free(data);
    if (rc < 0) return vg_raise(rc, err);
    Py_RETURN_NONE;
}

static PyObject* Tex_get(PyObject* self, void* c) {
    TexObject* o = TX_OF(self);
    int i;
    switch ((int)(intptr_t)c) {
    case 0: return PyLong_FromLong(o->t->w);
    case 1: return PyLong_FromLong(o->t->h);
    default:
        for (i = 0; vg_formats[i].name; i++) if (vg_formats[i].f == o->t->format) return PyUnicode_FromString(vg_formats[i].name);
        return PyUnicode_FromString("?");
    }
}

static PyMethodDef Tex_methods[] = {
    { "update", (PyCFunction)(void (*)(void))Tex_update, METH_VARARGS | METH_KEYWORDS,
      "update(pixels, x=0, y=0): new texels (the texture's format), between frames." },
    { NULL, NULL, 0, NULL }
};
static PyGetSetDef Tex_getset[] = {
    { "w", Tex_get, NULL, "texels", (void*)0 },
    { "h", Tex_get, NULL, "texels", (void*)1 },
    { "format", Tex_get, NULL, "r8, rg8, rgba8, r16f, rgba16f, r32f, rgba32f or r16ui", (void*)2 },
    { NULL, NULL, NULL, NULL, NULL }
};
static PyType_Slot Tex_slots[] = {
    { Py_tp_doc, "Texture: texels on the GPU for images (Gfx.texture()); values are linear unless made with srgb=True." },
    { Py_tp_dealloc, Tex_dealloc },
    { Py_tp_methods, Tex_methods },
    { Py_tp_getset, Tex_getset },
    { 0, NULL }
};
static PyType_Spec Tex_spec = { "ysp.gfx.Texture", sizeof(TexObject), 0, Py_TPFLAGS_DEFAULT, Tex_slots };

/* ---- module functions -------------------------------------------------------------------------- */

static const struct { const char* name; int kind; } vg_kinds[] = {
    { "shape", YGFX_SHAPE }, { "grating", YGFX_GRATING }, { "gabor", YGFX_GABOR }, { "dots", YGFX_DOTS },
    { "image", YGFX_IMAGE }, { "noise", YGFX_NOISE }, { "instances", -1 }, { NULL, 0 } };

/* params(kind=None) -> list of dicts: ysp/gfx.h's parameter rows (the
 * floats and the enums) a kind reads, with unit, range, default and doc. */
static PyObject* m_gfx_params(PyObject* m, PyObject* args) {
    const char* kind = NULL;
    uint32_t mask = 0xFFFFFFFFu;
    const ygfx_param* t;
    int n, i, pass;
    PyObject* l;
    (void)m;
    if (!PyArg_ParseTuple(args, "|z", &kind)) return NULL;
    if (kind) {
        for (i = 0; vg_kinds[i].name; i++)
            if (!strcmp(kind, vg_kinds[i].name)) { mask = vg_kinds[i].kind < 0 ? 0xFFFFFFFFu : YV_KB(vg_kinds[i].kind); break; }
        if (!vg_kinds[i].name) { PyErr_SetString(VsArgumentError, "params: shape, grating, gabor, dots, image, noise"); return NULL; }
    }
    l = PyList_New(0);
    for (pass = 0; l && pass < 2; pass++) {
        t = pass ? ygfx_enum_params(&n) : ygfx_params(&n);
        for (i = 0; i < n; i++) {
            PyObject* d;
            if (!(t[i].kinds & mask)) continue;
            d = Py_BuildValue("{s:s,s:s,s:d,s:d,s:d,s:s,s:s,s:s}", "name", t[i].name, "type", t[i].type, "min", t[i].min,
                              "max", t[i].max, "default", t[i].def, "unit", t[i].unit, "doc", t[i].doc, "values",
                              t[i].values ? t[i].values : "");
            if (!d || PyList_Append(l, d) < 0) { Py_XDECREF(d); Py_DECREF(l); return NULL; }
            Py_DECREF(d);
        }
    }
    return l;
}

static PyMethodDef vg_module_methods[] = {
    { "_gfx_params", m_gfx_params, METH_VARARGS, "params(kind=None) -> list[dict]: ysp/gfx.h's parameter rows a kind reads." },
    { NULL, NULL, 0, NULL }
};

/* The types and functions of ysp.gfx, into the module. */
static int vg_init(PyObject* m) {
    if (!(GfxType = vs_add_type(m, &Gfx_spec, "Gfx"))) return -1;
    if (!(StimType = vs_add_type(m, &Stim_spec, "Stim"))) return -1;
    if (!(ListType = vs_add_type(m, &List_spec, "DrawList"))) return -1;
    if (!(GroupType = vs_add_type(m, &Group_spec, "Group"))) return -1;
    if (!(TexType = vs_add_type(m, &Tex_spec, "Texture"))) return -1;
    if (PyModule_AddFunctions(m, vg_module_methods) < 0) return -1;
    return 0;
}
