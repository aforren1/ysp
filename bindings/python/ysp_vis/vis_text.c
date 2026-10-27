/* vis_text.c - ysp.gfx's text in the ysp._vis extension: Font, Text
 *
 * Not compiled alone: ysp_vis_ext.c includes it after vis_gfx.c. The work
 * is bindings/share/ysp_vis_text.h's (the same for MEX); this file turns
 * Python arguments into its yv_kv lists and its results into Python
 * objects.
 *
 *     f = g.font(["C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/msyh.ttc"])
 *     t = g.text("RED", font=f, size=48, color=(0.6, 0.1, 0.1))
 *     t.text = "GREEN"                  # a layout, between frames
 *     g.frame(t)
 *     t.words()                         # [(word, start, end, line, x0, y0, x1, y1), ...] screen px
 *
 * A Text is a Stim (lists, set, bind, hit, bounds take it) with its own
 * parameters: text, font, size, width, align, line_height, dir, lang,
 * spans. Character indices are str indices (codepoints).
 *
 * LIFETIME. A Font holds its Gfx; a Text holds its Gfx (as every Stim) and
 * its Font object when it was given one. A font named by path (font="...")
 * belongs to the Gfx, which frees it last. After the Gfx closes, a Text or
 * Font raises ClosedError. The lists glyphs(), words() and lines() return
 * are copies: they own their memory.
 */

typedef struct FontObject {
    PyObject_HEAD
    yv_font*  f;
    PyObject* gfx;
} FontObject;

typedef struct TextObject {
    StimObject base;
    PyObject*  font;     /* the Font given, or NULL (the system's, or a path's) */
} TextObject;

#define FT_OF(o) ((FontObject*)(o))
#define TT_OF(o) ((TextObject*)(o))

/* spans=[(start, end, color=None, size=0), ...] (or dicts with those keys)
 * as rows of start, end, r, g, b, size; NaN color: the text's. */
static int vt_spans(PyObject* v, struct vg_arena* a, const double** out, int* n) {
    Py_ssize_t k, i;
    double* d;
    if (!PyList_Check(v) && !PyTuple_Check(v)) {
        PyErr_SetString(VsArgumentError, "spans: a list of (start, end, color=None, size=0)");
        return -1;
    }
    k = PySequence_Size(v);
    d = vg_alloc(a, 6 * k);
    if (!d) return -1;
    for (i = 0; i < k; i++) {
        PyObject* it = PySequence_GetItem(v, i);
        PyObject *s = NULL, *e = NULL, *c = NULL, *z = NULL;
        double* r = d + 6 * i;
        int bad = 0;
        if (!it) return -1;
        if (PyDict_Check(it)) {
            s = PyDict_GetItemString(it, "start");
            e = PyDict_GetItemString(it, "end");
            c = PyDict_GetItemString(it, "color");
            z = PyDict_GetItemString(it, "size");
            Py_XINCREF(s); Py_XINCREF(e); Py_XINCREF(c); Py_XINCREF(z);
        } else if ((PyTuple_Check(it) || PyList_Check(it)) && PySequence_Size(it) >= 2 && PySequence_Size(it) <= 4) {
            Py_ssize_t m = PySequence_Size(it);
            s = PySequence_GetItem(it, 0);
            e = PySequence_GetItem(it, 1);
            if (m > 2) c = PySequence_GetItem(it, 2);
            if (m > 3) z = PySequence_GetItem(it, 3);
        } else {
            bad = 1;
        }
        Py_DECREF(it);
        if (!bad && (!s || !e || !PyLong_Check(s) || !PyLong_Check(e))) bad = 1;
        if (!bad) {
            r[0] = (double)PyLong_AsLongLong(s);
            r[1] = (double)PyLong_AsLongLong(e);
            r[2] = r[3] = r[4] = NAN;
            r[5] = 0;
            if (c && c != Py_None) {
                float rgb[3];
                if (vs_rgb(c, rgb, "spans: color") < 0) bad = 2;
                else { r[2] = rgb[0]; r[3] = rgb[1]; r[4] = rgb[2]; }
            }
            if (!bad && z && z != Py_None) {
                r[5] = PyFloat_AsDouble(z);
                if (r[5] == -1.0 && PyErr_Occurred()) bad = 2;
            }
        }
        Py_XDECREF(s); Py_XDECREF(e); Py_XDECREF(c); Py_XDECREF(z);
        if (bad == 1) {
            PyErr_Format(VsArgumentError, "spans: span %d is (start, end, color=None, size=0), str indices", (int)i);
            return -1;
        }
        if (bad) return -1;
    }
    *out = d;
    *n = (int)(6 * k);
    return 0;
}

/* font=["a.ttf", "b.ttc"]: the paths joined for the core's implicit font. */
static int vt_font_paths(PyObject* v, struct vg_arena* a, const char** out) {
    Py_ssize_t k = PySequence_Size(v), i;
    PyObject* sep = PyUnicode_FromString("|");
    PyObject* joined;
    for (i = 0; i < k; i++) {
        PyObject* it = PySequence_GetItem(v, i);
        int ok = it && PyUnicode_Check(it);
        Py_XDECREF(it);
        if (!ok) {
            Py_XDECREF(sep);
            PyErr_SetString(VsArgumentError, "font: a Font, a path, or a list of paths (a fallback list)");
            return -1;
        }
    }
    if (!sep) return -1;
    joined = PyUnicode_Join(sep, v);
    Py_DECREF(sep);
    if (!joined) return -1;
    *out = vg_str(joined, a);
    Py_DECREF(joined);
    return *out ? 0 : -1;
}

/* ---- Font ------------------------------------------------------------------------------- */

static yv_obj* vt_font_obj(PyObject* v) {
    if (!FT_OF(v)->f) { PyErr_SetString(VsClosedError, "the font is gone"); return NULL; }
    return &FT_OF(v)->f->o;
}

static void Font_dealloc(PyObject* self) {
    FontObject* o = FT_OF(self);
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    if (o->f) { yv_font_free(o->f); o->f = NULL; }
    Py_CLEAR(o->gfx);
    fr(self);
    Py_DECREF(tp);
}

static PyObject* Font_info(PyObject* self, void* c) {
    FontObject* o = FT_OF(self);
    PyObject* l;
    int i;
    (void)c;
    if (!o->f) { PyErr_SetString(VsClosedError, "the font is gone"); return NULL; }
    l = PyList_New(0);
    for (i = 0; l && i < o->f->n; i++) {
        ylay_font_info fi;
        PyObject* d;
        if (yv_font_info(o->f, i, &fi) < 0) continue;
        d = Py_BuildValue("{s:s,s:i,s:s,s:i,s:i,s:d,s:d,s:d,s:I,s:d}", "name", fi.name, "face", fi.face, "sha256", fi.sha256,
                          "units_per_em", (int)fi.units_per_em, "glyphs", (int)fi.n_glyphs, "ascender", fi.ascender,
                          "descender", fi.descender, "line_gap", fi.line_gap, "built", (unsigned)fi.n_built,
                          "build_time", fi.build_us / 1e6);
        if (!d || PyList_Append(l, d) < 0) { Py_XDECREF(d); Py_DECREF(l); return NULL; }
        Py_DECREF(d);
    }
    return l;
}

static PyObject* Font_stamp(PyObject* self, PyObject* unused) {
    FontObject* o = FT_OF(self);
    char buf[4096];
    (void)unused;
    if (!o->f) { PyErr_SetString(VsClosedError, "the font is gone"); return NULL; }
    ylay_stamp(o->f->L, NULL, buf, sizeof buf);
    return PyUnicode_FromString(buf);
}

static PyMethodDef Font_methods[] = {
    { "stamp", Font_stamp, METH_NOARGS, "stamp() -> str: the layout's versions and every file's name#face=sha256, for a log." },
    { NULL, NULL, 0, NULL }
};
static PyGetSetDef Font_getset[] = {
    { "info", Font_info, NULL, "per file: name, face, sha256, units_per_em, glyphs, ascender, descender, line_gap (em), "
                               "built (glyphs in its curve set), build_time (s)", NULL },
    { NULL, NULL, NULL, NULL, NULL }
};
static PyType_Slot Font_slots[] = {
    { Py_tp_doc, "Font: a fallback list of font files for texts (Gfx.font()). For each run of one script the files that "
                 "support it are tried in order, and the first that has the character draws it (Skribidi's rule)." },
    { Py_tp_dealloc, Font_dealloc },
    { Py_tp_methods, Font_methods },
    { Py_tp_getset, Font_getset },
    { 0, NULL }
};
static PyType_Spec Font_spec = { "ysp.gfx.Font", sizeof(FontObject), 0, Py_TPFLAGS_DEFAULT, Font_slots };

/* font(source, face=0, name=None) -> Font: source a path, the bytes of a
 * file, or a list of them (or of (source, face) pairs): a fallback list. */
static PyObject* Gfx_font(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "source", "face", "name", NULL };
    GfxObject* o = GF_OF(self);
    PyObject *src, *items = NULL, *keep[YV_FONT_MAX];
    int face = 0, n, i, code, nk = 0;
    const char* name = NULL;
    yv_font_src fs[YV_FONT_MAX];
    char err[512];
    yv_font* f;
    PyObject* r = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|iz", kwlist, &src, &face, &name)) return NULL;
    if (!gf_ready(o)) return NULL;
    if (PyList_Check(src) || PyTuple_Check(src)) {
        Py_INCREF(src);
        items = src;
    } else {
        items = PyTuple_Pack(1, src);
        if (!items) return NULL;
    }
    n = (int)PySequence_Size(items);
    if (n < 1 || n > YV_FONT_MAX) {
        Py_DECREF(items);
        return PyErr_Format(VsArgumentError, "font: 1 to %d files", YV_FONT_MAX);
    }
    memset(fs, 0, sizeof fs);
    for (i = 0; i < n; i++) {
        PyObject* it = PySequence_GetItem(items, i);
        PyObject* s = it;
        int fc = face;
        if (!it) goto done;
        if (PyTuple_Check(it) && PyTuple_Size(it) == 2) {
            s = PyTuple_GetItem(it, 0);
            fc = (int)PyLong_AsLong(PyTuple_GetItem(it, 1));
            if (fc == -1 && PyErr_Occurred()) { Py_DECREF(it); goto done; }
        }
        fs[i].face = fc;
        fs[i].name = name;
        if (PyUnicode_Check(s)) {
            PyObject* b = PyUnicode_AsUTF8String(s);
            if (!b) { Py_DECREF(it); goto done; }
            keep[nk++] = b;
            fs[i].path = PyBytes_AsString(b);
        } else if (PyBytes_Check(s)) {
            Py_INCREF(s);
            keep[nk++] = s;
            fs[i].bytes = PyBytes_AsString(s);
            fs[i].n = (size_t)PyBytes_Size(s);
        } else if (PyByteArray_Check(s)) {
            PyObject* b = PyBytes_FromObject(s);
            if (!b) { Py_DECREF(it); goto done; }
            keep[nk++] = b;
            fs[i].bytes = PyBytes_AsString(b);
            fs[i].n = (size_t)PyBytes_Size(b);
        } else {
            Py_DECREF(it);
            PyErr_SetString(VsArgumentError, "font: a path, the bytes of a font file, or a list of them");
            goto done;
        }
        Py_DECREF(it);
    }
    f = yv_font_make(o->g, fs, n, NULL, &code, err, sizeof err);
    if (!f) { vg_raise(code, err); goto done; }
    r = vg_new(FontType);
    if (!r) { yv_font_free(f); goto done; }
    FT_OF(r)->f = f;
    Py_INCREF(self);
    FT_OF(r)->gfx = self;
done:
    for (i = 0; i < nk; i++) Py_DECREF(keep[i]);
    Py_DECREF(items);
    return r;
}

/* ---- Text --------------------------------------------------------------------------------- */

static void Text_dealloc(PyObject* self) {
    TextObject* o = TT_OF(self);
    /* the core first: it gives back its use of the font */
    yv_stim_free(o->base.st);
    o->base.st = NULL;
    Py_CLEAR(o->font);
    Stim_dealloc(self);
}

static int vt_ready(TextObject* o) {
    if (!o->base.st) { PyErr_SetString(VsClosedError, "the text is gone"); return 0; }
    return vg_obj_ready(o->base.gfx);
}

/* Keeps the Python Font of a font= argument (and drops the old one). */
static void vt_font_ref(TextObject* o, PyObject* v) {
    if (v && PyObject_TypeCheck(v, (PyTypeObject*)FontType)) {
        PyObject* old = o->font;
        Py_INCREF(v);
        o->font = v;
        Py_XDECREF(old);
    } else if (v) {
        Py_CLEAR(o->font);
    }
}

/* text(text="", **params) -> Text. */
static PyObject* Gfx_text(PyObject* self, PyObject* args, PyObject* kw) {
    GfxObject* o = GF_OF(self);
    yv_kv kv[VG_MAX_KV];
    vg_arena a;
    char err[512];
    int n = 0, code, i;
    yv_stim* st;
    PyObject *text = NULL, *r, *group = NULL, *font = NULL;
    if (!PyArg_ParseTuple(args, "|O", &text)) return NULL;
    if (text && kw && PyDict_GetItemString(kw, "text")) { PyErr_SetString(PyExc_TypeError, "text given twice"); return NULL; }
    if (!gf_ready(o)) return NULL;
    memset(&a, 0, sizeof a);
    if (text) {
        if (!PyUnicode_Check(text)) { PyErr_SetString(VsArgumentError, "text: a str"); return NULL; }
        if (vg_kv("text", text, &kv[0], &a) < 0) { vg_arena_free(&a); return NULL; }
        n = 1;
    }
    n = vg_kvs(kw, kv, &a, n);
    if (n < 0) { vg_arena_free(&a); return NULL; }
    if (kw) {
        group = PyDict_GetItemString(kw, "group");
        font = PyDict_GetItemString(kw, "font");
    }
    st = yv_text_make(o->g, kv, n, &code, err, sizeof err);
    if (!st) { vg_arena_free(&a); return vg_raise(code, err); }
    for (i = 0; i < n; i++)
        if (!kv[i].used) { yv_stim_free(st); vg_kv_unused(kv, n); vg_arena_free(&a); return NULL; }
    vg_arena_free(&a);
    r = vg_new(TextType);
    if (!r) { yv_stim_free(st); return NULL; }
    ST_OF(r)->st = st;
    Py_INCREF(self);
    ST_OF(r)->gfx = self;
    vg_stim_refs(r, group, NULL);
    vt_font_ref(TT_OF(r), font);
    return r;
}

/* Sets the text's parameters (its own and the stimulus's): one layout. */
static int vt_set(TextObject* o, PyObject* kw) {
    yv_kv kv[VG_MAX_KV];
    vg_arena a;
    char err[512];
    int n, rc, i;
    if (!vt_ready(o)) return -1;
    memset(&a, 0, sizeof a);
    n = vg_kvs(kw, kv, &a, 0);
    if (n < 0) { vg_arena_free(&a); return -1; }
    rc = yv_text_set(o->base.st, kv, n, err, sizeof err);
    if (rc == 0)
        for (i = 0; i < n; i++)
            if (!kv[i].used) { vg_kv_unused(kv, n); rc = -1; break; }
    vg_arena_free(&a);
    if (rc < 0) {
        if (!PyErr_Occurred()) vg_raise(rc, err);
        return -1;
    }
    vg_stim_refs((PyObject*)o, PyDict_GetItemString(kw, "group"), NULL);
    vt_font_ref(o, PyDict_GetItemString(kw, "font"));
    return 0;
}

static PyObject* Text_set(PyObject* self, PyObject* args, PyObject* kw) {
    if (PyTuple_Size(args) > 0) { PyErr_SetString(PyExc_TypeError, "set(**params): keyword arguments only"); return NULL; }
    if (!kw) Py_RETURN_NONE;
    if (vt_set(TT_OF(self), kw) < 0) return NULL;
    Py_RETURN_NONE;
}

static int Text_setattro(PyObject* self, PyObject* name, PyObject* v) {
    PyObject* b;
    int rc;
    if (!v) { PyErr_SetString(PyExc_AttributeError, "a text's parameters cannot be deleted"); return -1; }
    b = PyUnicode_AsUTF8String(name);
    if (!b) return -1;
    if (yv_text_key(PyBytes_AsString(b))) {
        PyObject* kw = PyDict_New();
        rc = kw && PyDict_SetItem(kw, name, v) == 0 ? vt_set(TT_OF(self), kw) : -1;
        Py_XDECREF(kw);
    } else {
        rc = Stim_setattro(self, name, v);
    }
    Py_DECREF(b);
    return rc;
}

/* The text's own parameters and its layout's numbers, as attributes. */
static PyObject* Text_getattro(PyObject* self, PyObject* name) {
    TextObject* o = TT_OF(self);
    PyObject *b, *r = NULL;
    const char* s;
    double v[2];
    int nv, rc;
    char buf[4096];
    b = PyUnicode_AsUTF8String(name);
    if (!b) return NULL;
    if (!o->base.st) { Py_DECREF(b); return Stim_getattro(self, name); }
    rc = yv_text_get(o->base.st, PyBytes_AsString(b), v, &nv, &s, buf, sizeof buf);
    if (!strcmp(PyBytes_AsString(b), "font")) {
        r = o->font ? o->font : Py_None;
        Py_INCREF(r);
    } else if (rc == 1) {   /* spans */
        const yv_text* t = (const yv_text*)o->base.st->text;
        int i;
        r = PyList_New(t->n_spans);
        for (i = 0; r && i < t->n_spans; i++) {
            const double* q = t->span_v + 6 * i;
            PyObject* col = q[2] == q[2] ? Py_BuildValue("(ddd)", q[2], q[3], q[4]) : (Py_INCREF(Py_None), Py_None);
            PyList_SetItem(r, i, Py_BuildValue("(iiNd)", (int)q[0], (int)q[1], col, q[5]));
        }
    } else if (rc == 0) {
        if (s) r = PyUnicode_DecodeUTF8(s, (Py_ssize_t)strlen(s), "strict");
        else if (nv == 1) {
            const char* nm = PyBytes_AsString(b);
            r = (!strncmp(nm, "n_", 2) || !strcmp(nm, "missing") || !strcmp(nm, "new_glyphs")) ? PyLong_FromLong((long)v[0])
                                                                                                : PyFloat_FromDouble(v[0]);
        } else r = Py_BuildValue("(dd)", v[0], v[1]);
    }
    Py_DECREF(b);
    return r ? r : Stim_getattro(self, name);
}

static PyObject* Text_glyphs(PyObject* self, PyObject* unused) {
    TextObject* o = TT_OF(self);
    const yv_text* t;
    double* d;
    int n, i;
    PyObject* l;
    (void)unused;
    if (!vt_ready(o)) return NULL;
    t = (const yv_text*)o->base.st->text;
    d = (double*)PyMem_Malloc(sizeof(double) * 6 * (t->b.n_items ? t->b.n_items : 1));
    if (!d) return PyErr_NoMemory();
    n = yv_text_glyphs(o->base.st, d);
    l = PyList_New(n);
    for (i = 0; l && i < n; i++) {
        const double* q = d + 6 * i;
        PyList_SetItem(l, i, Py_BuildValue("(iidddd)", (int)q[0], (int)q[1], q[2], q[3], q[4], q[5]));
    }
    PyMem_Free(d);
    return l;
}

static PyObject* Text_lines(PyObject* self, PyObject* unused) {
    TextObject* o = TT_OF(self);
    const yv_text* t;
    double* d;
    int n, i;
    PyObject* l;
    (void)unused;
    if (!vt_ready(o)) return NULL;
    t = (const yv_text*)o->base.st->text;
    d = (double*)PyMem_Malloc(sizeof(double) * 6 * (t->b.n_lines ? t->b.n_lines : 1));
    if (!d) return PyErr_NoMemory();
    n = yv_text_lines(o->base.st, d);
    l = PyList_New(n);
    for (i = 0; l && i < n; i++) {
        const double* q = d + 6 * i;
        PyList_SetItem(l, i, Py_BuildValue("(iidddd)", (int)q[0], (int)q[1], q[2], q[3], q[4], q[5]));
    }
    PyMem_Free(d);
    return l;
}

static PyObject* Text_words(PyObject* self, PyObject* unused) {
    TextObject* o = TT_OF(self);
    const yv_text* t;
    double* d;
    int n, i, cap;
    PyObject *l, *str;
    (void)unused;
    if (!vt_ready(o)) return NULL;
    t = (const yv_text*)o->base.st->text;
    cap = yv_text_word_count(t);
    d = (double*)PyMem_Malloc(sizeof(double) * 7 * (size_t)(cap ? cap : 1));
    if (!d) return PyErr_NoMemory();
    n = yv_text_words(o->base.st, d, cap);
    if (n < 0) { PyMem_Free(d); return PyErr_NoMemory(); }
    str = PyUnicode_DecodeUTF8(t->utf8, (Py_ssize_t)t->n_bytes, "strict");
    l = str ? PyList_New(n) : NULL;
    for (i = 0; l && i < n; i++) {
        const double* q = d + 7 * i;
        PyObject* w = PyUnicode_Substring(str, (Py_ssize_t)q[0], (Py_ssize_t)q[1]);
        PyList_SetItem(l, i, w ? Py_BuildValue("(Niiidddd)", w, (int)q[0], (int)q[1], (int)q[2], q[3], q[4], q[5], q[6]) : NULL);
    }
    Py_XDECREF(str);
    PyMem_Free(d);
    return l;
}

static PyObject* Text_hit_index(PyObject* self, PyObject* args) {
    TextObject* o = TT_OF(self);
    float x, y;
    if (vg_xy(args, &x, &y) < 0) return NULL;
    if (!vt_ready(o)) return NULL;
    return PyLong_FromLong(yv_text_hit(o->base.st, x, y));
}

static PyMethodDef Text_methods[] = {
    { "set", (PyCFunction)(void (*)(void))Text_set, METH_VARARGS | METH_KEYWORDS,
      "set(**params): the text's parameters (text, font, size, width, align, line_height, dir, lang, spans: one layout) "
      "and the stimulus's (x, y, color, ...), between frames when a layout is due." },
    { "glyphs", Text_glyphs, METH_NOARGS,
      "glyphs() -> [(index, line, x0, y0, x1, y1)]: each glyph's character (str index), line and ink box, screen px; "
      "a glyph with no ink gives its pen point." },
    { "words", Text_words, METH_NOARGS,
      "words() -> [(word, start, end, line, x0, y0, x1, y1)]: the runs of characters between white space, their str "
      "indices, line, and box in screen px: across, its glyphs' advances; down, its line." },
    { "lines", Text_lines, METH_NOARGS, "lines() -> [(start, end, x0, y0, x1, y1)]: each line's characters and its box, screen px." },
    { "hit_index", Text_hit_index, METH_VARARGS, "hit_index(x, y) -> int: the str index of the glyph under screen px x, y, or -1." },
    { NULL, NULL, 0, NULL }
};
static PyType_Slot Text_slots[] = {
    { Py_tp_doc, "Text: a Stim laid out from a str (Gfx.text()) by Skribidi and HarfBuzz: scripts, bidi, line breaking. "
                 "Its own parameters: text, font, size (units per em), width (wrap; 0 none), align (start, center, end), "
                 "line_height, dir (auto, ltr, rtl), lang, spans; changing one lays it out again, between frames. The rest "
                 "are a stimulus's: place, anchor, x, y, ori, color, space, opacity, visible, group, ... Read-only: box, "
                 "n_lines, n_glyphs, n_runs, n_chars, missing, resolved_dir, layout_time, build_time, upload_time (s), "
                 "new_glyphs, stamp." },
    { Py_tp_dealloc, Text_dealloc },
    { Py_tp_getattro, Text_getattro },
    { Py_tp_setattro, Text_setattro },
    { Py_tp_methods, Text_methods },
    { 0, NULL }
};
static PyType_Spec Text_spec = { "ysp.gfx.Text", sizeof(TextObject), 0, Py_TPFLAGS_DEFAULT, Text_slots };

/* ---- the C path, for tests/test_window.py ---------------------------------------------- */

static PyObject* Gfx_c_text_setup(PyObject* self, PyObject* unused) {
    GfxObject* o = GF_OF(self);
    char err[512];
    int rc;
    (void)unused;
    if (!gf_ready(o)) return NULL;
    rc = yv_ctext_setup(o->g, err, sizeof err);
    if (rc < 0) return vg_raise(rc, err);
    Py_RETURN_NONE;
}

static PyObject* Gfx_c_text(PyObject* self, PyObject* arg) {
    GfxObject* o = GF_OF(self);
    char err[512];
    long k = PyLong_AsLong(arg);
    int rc;
    if (k == -1 && PyErr_Occurred()) return NULL;
    if (!gf_ready(o)) return NULL;
    if (!o->g->in_frame) { PyErr_SetString(VsOrderError, "_c_text: between begin() and end()"); return NULL; }
    rc = yv_ctext_draw(o->g, (int)k, err, sizeof err);
    if (rc < 0) return vg_raise(rc, err);
    Py_RETURN_NONE;
}

static int vt_init(PyObject* m) {
    PyObject* bases;
    if (!(FontType = vs_add_type(m, &Font_spec, "Font"))) return -1;
    bases = PyTuple_Pack(1, StimType);
    if (!bases) return -1;
    TextType = PyType_FromSpecWithBases(&Text_spec, bases);
    Py_DECREF(bases);
    if (!TextType) return -1;
    Py_INCREF(TextType);
    if (PyModule_AddObject(m, "Text", TextType) < 0) { Py_DECREF(TextType); Py_DECREF(TextType); return -1; }
    return 0;
}
