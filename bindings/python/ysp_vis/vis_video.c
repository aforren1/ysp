/* vis_video.c - ysp.video in ysp._vis: Movie, SeqWriter, probe(), make_index()
 *
 * Not a translation unit: ysp_vis_ext.c includes it after the Screen and
 * Gfx types, so a Movie reaches its Gfx's and its Screen's state. The part
 * both hosts share is bindings/share/ysp_vis_video.h.
 *
 *     import ysp.screen as yscr, ysp.gfx as ygfx, ysp.video as yvid
 *     with yscr.Screen() as s, ygfx.Gfx(s) as g, yvid.Movie(g, "clip.mp4") as m:
 *         m.play()
 *         while True:
 *             f = s.begin()
 *             playing = m.update()     # the frame due at f.onset; never waits
 *             g.begin(); m.draw(); g.end()
 *             s.flip()
 *             if not playing:
 *                 break
 *         s.wait_flip()
 *         frames = m.records()         # one Record per display frame
 *
 * TIMES: seconds, rounded to the nearest ns; records give the exact ns
 * beside each time.
 *
 * GIL: released in open, close, make_index, SeqWriter.write and finish
 * (file I/O, a decoder's open, the decode thread's join). update() and
 * draw() keep it: yvid_update() never waits. While a call runs without the
 * GIL the Screen is busy, so another thread's call raises BusyError.
 *
 * PATHS: the header opens files with fopen(), so a path goes in the ANSI
 * code page ("mbcs"); a path outside it is refused before the call.
 */

/* ysp_vis_ext.c's, defined after this file */
static PyObject* vs_add_exc(PyObject* m, const char* qual, const char* attr, PyObject* base);
static PyObject* vs_add_exc2(PyObject* m, const char* qual, const char* attr, PyObject* b1, PyObject* b2);
static PyObject* vs_add_type(PyObject* m, PyType_Spec* spec, const char* name);
static PyTypeObject* vs_add_seq(PyObject* m, PyStructSequence_Desc* d, const char* name);

static PyObject *VvFormatError, *VvNotFoundError, *VvIOError;
static PyObject *MovieType, *SeqWriterType;
static PyTypeObject* VRecordType;

/* ysp_vis_video.h: a pin goes back with the GIL held (a screen closes
 * without it, and its movies close inside). */
static void yv_video_release(void (*release)(void*), void* obj) {
    PyGILState_STATE st = PyGILState_Ensure();
    release(obj);
    PyGILState_Release(st);
}

static PyObject* vv_exc(int code) {
    switch (code) {
    case YVID_ERR_ARG: return VsArgumentError;
    case YVID_ERR_FORMAT: return VvFormatError;
    case YVID_ERR_CLOSED: return VsClosedError;
    case YVID_ERR_ORDER: return VsOrderError;
    case YVID_ERR_REFUSED: return VsRefusedError;
    case YVID_ERR_LOST: return VsLostError;
    case YVID_ERR_IO: return VvIOError;
    case YVID_ERR_NOT_FOUND: return VvNotFoundError;
    case YVID_ERR_NOT_IMPLEMENTED: return VsNotImplementedError;
    default: return VsError;
    }
}

/* A path (str, bytes, os.PathLike) as bytes in the ANSI code page. */
static PyObject* vv_path(PyObject* p) {
    PyObject *fp = PyOS_FSPath(p), *b;
    if (!fp) return NULL;
    if (PyBytes_Check(fp)) return fp;
    b = PyUnicode_AsEncodedString(fp, "mbcs", "strict");
    Py_DECREF(fp);
    if (!b) {
        PyErr_Clear();
        PyErr_SetString(VsArgumentError, "ysp_video: the path has a character outside the ANSI code page, which "
                                         "ysp/video.h's fopen() cannot open (pass the bytes as data= instead)");
    }
    return b;
}

static PyObject* vv_fields(const yv_fields* f) {
    PyObject* d = PyDict_New();
    int i;
    for (i = 0; d && i < f->n; i++) {
        const yv_field* x = &f->f[i];
        PyObject* v;
        switch (x->type) {
        case YVF_S: v = PyFloat_FromDouble(yv_s(x->i)); break;
        case YVF_NS:
        case YVF_INT: v = PyLong_FromLongLong(x->i); break;
        case YVF_NUM: v = PyFloat_FromDouble(x->d); break;
        case YVF_BOOL: v = PyBool_FromLong((long)x->i); break;
        default: v = PyUnicode_FromString(x->s ? x->s : ""); break;
        }
        if (!v || PyDict_SetItemString(d, x->name, v) < 0) { Py_XDECREF(v); Py_DECREF(d); return NULL; }
        Py_DECREF(v);
    }
    return d;
}

/* --- Record (FRAME) ------------------------------------------------------------- */

static PyStructSequence_Field vv_record_fields[] = {
    { "display", "the display frame's index (Frame.index), low 32 bits" },
    { "frame", "the video frame on screen" },
    { "onset", "the flip's onset, seconds on the ysp_rt clock; the predicted onset with F_ESTIMATED, F_PENDING or F_NOT_SHOWN" },
    { "onset_ns", "the same, exact int ns" },
    { "due", "when the frame was due on the movie clock, seconds" },
    { "due_ns", "the same, exact int ns" },
    { "residual", "onset - due, seconds: with the audio clock, the frame's A/V error" },
    { "residual_ns", "the same, exact int ns" },
    { "movie_t", "the movie time at the predicted onset, seconds" },
    { "movie_t_ns", "the same, exact int ns" },
    { "decision", "SHOWN, REPEATED or DROPPED" },
    { "why", "WHY_DUE, WHY_CADENCE, WHY_DISPLAY_LATE, WHY_DECODE_LATE, WHY_DRIFT, WHY_SEEK, WHY_LOOP, WHY_MANUAL" },
    { "tier", "the flip's tier (ysp.screen.TIER_*)" },
    { "path", "the movie's path: PATH_UPLOAD or PATH_GPU" },
    { "flags", "F_* bits" },
    { "ahead", "frames decoded and ready after this one" },
    { "shows", "display frames this video frame has been on screen" },
    { NULL, NULL }
};
static PyStructSequence_Desc vv_record_desc = {
    "ysp.video.Record", "The record of one display frame of a movie (YVID_EV_FRAME): times in seconds, exact ns beside them.",
    vv_record_fields, 17 };

static PyObject* vv_record(int64_t display, int64_t frame, int64_t onset, int64_t due, int64_t movie_t, unsigned decision,
                           unsigned why, unsigned tier, unsigned path, unsigned flags, unsigned ahead, unsigned shows) {
    PyObject* v[17];
    v[0] = PyLong_FromLongLong(display);
    v[1] = PyLong_FromLongLong(frame);
    v[2] = PyFloat_FromDouble(yv_s(onset));
    v[3] = PyLong_FromLongLong(onset);
    v[4] = PyFloat_FromDouble(yv_s(due));
    v[5] = PyLong_FromLongLong(due);
    v[6] = PyFloat_FromDouble(yv_s(onset - due));
    v[7] = PyLong_FromLongLong(onset - due);
    v[8] = PyFloat_FromDouble(yv_s(movie_t));
    v[9] = PyLong_FromLongLong(movie_t);
    v[10] = PyLong_FromUnsignedLong(decision);
    v[11] = PyLong_FromUnsignedLong(why);
    v[12] = PyLong_FromUnsignedLong(tier);
    v[13] = PyLong_FromUnsignedLong(path);
    v[14] = PyLong_FromUnsignedLong(flags);
    v[15] = PyLong_FromUnsignedLong(ahead);
    v[16] = PyLong_FromUnsignedLong(shows);
    return vs_seq(VRecordType, v, 17);
}

static PyObject* vv_record_ev(const yrt_event* e) {
    const yrt_payload* u = &e->u;
    unsigned w = u->u16[14];
    return vv_record(u->u32[9], u->i64[0], e->t_ns, u->i64[1], u->i64[2], YVID_EV_DECISION_OF(w), YVID_EV_WHY_OF(w),
                     YVID_EV_TIER_OF(w), YVID_EV_PATH_OF(w), u->u32[6], u->u16[15], u->u32[8]);
}

/* --- Movie ------------------------------------------------------------------------ */

typedef struct MovieObject {
    PyObject_HEAD
    yv_movie* mv;
    PyObject* gfx;
    PyObject* keep[4];    /* the path's bytes, data, index, the soundtrack's WAV: the movie reads them */
} MovieObject;

#define MV_OF(o) ((MovieObject*)(o))

static ScreenObject* mv_screen(MovieObject* o) {
    return o->gfx ? SC_OF(GF_OF(o->gfx)->screen) : NULL;
}

static int mv_ready(MovieObject* o) {
    ScreenObject* s = mv_screen(o);
    if (s && s->busy) { PyErr_SetString(VsBusyError, "ysp_video: another thread is in a call on the movie's Screen"); return 0; }
    if (!o->mv || !o->mv->open) {
        /* closed with its gfx or screen: the pins go back now */
        if (o->mv) yv_movie_unpin(o->mv);
        PyErr_SetString(VsClosedError, "ysp_video: the movie is closed (or its Gfx or Screen)");
        return 0;
    }
    return 1;
}

static PyObject* mv_fail(MovieObject* o, int code, const char* why) {
    const char* m = why ? why : (o->mv ? yvid_error(&o->mv->m) : "");
    if (code == YSP_SHARE_ERR_HOST && PyErr_Occurred()) return NULL;
    PyErr_SetString(code > -1000 ? vv_exc(code) : VsError, m && m[0] ? m : yvid_strerror(code));
    return NULL;
}

static void mv_shut(MovieObject* o) {
    ScreenObject* s = mv_screen(o);
    if (!o->mv) return;
    if (o->mv->open) {
        if (s) s->busy = 1;
        Py_BEGIN_ALLOW_THREADS
        yv_movie_close_c(o->mv);
        Py_END_ALLOW_THREADS
        if (s) s->busy = 0;
    }
    yv_movie_unpin(o->mv);
}

static void Movie_dealloc(PyObject* self) {
    MovieObject* o = MV_OF(self);
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    int k;
    mv_shut(o);
    yv_movie_free(o->mv);
    o->mv = NULL;
    for (k = 0; k < 4; k++) Py_CLEAR(o->keep[k]);
    Py_CLEAR(o->gfx);
    fr(self);
    Py_DECREF(tp);
}

/* An option by the table; 0, or -1 with the error set. */
static int mv_option(yvid_desc* d, const char* name, PyObject* val) {
    const yvv_opt* opt = yvv_find_opt(name);
    char err[256];
    if (!opt) {
        PyErr_Format(PyExc_TypeError, "Movie() got an unexpected keyword argument '%s' (ysp.video.options(): the list)", name);
        return -1;
    }
    if (val == Py_None) return 0;
    if (opt->type == YVV_ENUM) {
        char s[64];
        if (!PyUnicode_Check(val)) { PyErr_Format(PyExc_TypeError, "%s: a name", name); return -1; }
        if (vs_utf8(val, s, sizeof s) < 0) return -1;
        if (yvv_opt_enum(d, opt, s, err, sizeof err) < 0) { PyErr_SetString(VsArgumentError, err); return -1; }
        return 0;
    }
    {
        double x;
        if (opt->type == YVV_BOOL) {
            int t = PyObject_IsTrue(val);
            if (t < 0) return -1;
            x = t;
        } else {
            x = PyFloat_AsDouble(val);
            if (x == -1.0 && PyErr_Occurred()) return -1;
        }
        if (yvv_opt_num(d, opt, x, err, sizeof err) < 0) { PyErr_SetString(VsArgumentError, err); return -1; }
    }
    return 0;
}

/* Bytes the movie reads while open: kept by the Movie. */
static int mv_bytes(PyObject* val, const char* what, PyObject** keep, const void** p, size_t* n) {
    char* s;
    Py_ssize_t len;
    if (!PyBytes_Check(val)) { PyErr_Format(PyExc_TypeError, "%s: bytes", what); return -1; }
    if (PyBytes_AsStringAndSize(val, &s, &len) < 0) return -1;
    Py_INCREF(val);
    Py_XDECREF(*keep);
    *keep = val;
    *p = s;
    *n = (size_t)len;
    return 0;
}

static int Movie_init(PyObject* self, PyObject* args, PyObject* kw) {
    MovieObject* o = MV_OF(self);
    Py_ssize_t na = PyTuple_Size(args), pos = 0;
    PyObject *gfx, *path = NULL, *k, *val, *tlobj = NULL;
    GfxObject* go;
    ScreenObject* so;
    yvid_desc d;
    int64_t records = 0;
    char err[640];
    bool ok;
    if (o->mv) { PyErr_SetString(VsError, "Movie.__init__ called twice"); return -1; }
    if (na < 1 || na > 2) {
        PyErr_SetString(PyExc_TypeError, "Movie(gfx, path=None, *, data=None, index=None, timeline=None, records=36000, **options)");
        return -1;
    }
    gfx = PyTuple_GetItem(args, 0);
    if (na == 2) path = PyTuple_GetItem(args, 1);
    if (!PyObject_TypeCheck(gfx, (PyTypeObject*)GfxType)) { PyErr_SetString(PyExc_TypeError, "Movie(gfx, ...): a ysp.gfx.Gfx"); return -1; }
    go = GF_OF(gfx);
    if (!go->g || !go->g->open) { PyErr_SetString(VsClosedError, "ysp_video: the gfx is closed"); return -1; }
    so = SC_OF(go->screen);
    if (!sc_ready(so)) return -1;
    o->mv = (yv_movie*)calloc(1, sizeof(yv_movie));
    if (!o->mv) { PyErr_NoMemory(); return -1; }
    memset(&d, 0, sizeof d);
    while (kw && PyDict_Next(kw, &pos, &k, &val)) {
        char name[64];
        if (vs_utf8(k, name, sizeof name) < 0) return -1;
        if (!strcmp(name, "path")) {
            if (path) { PyErr_SetString(PyExc_TypeError, "Movie(): path given twice"); return -1; }
            path = val;
        } else if (!strcmp(name, "data")) {
            if (val != Py_None && mv_bytes(val, "data", &o->keep[1], &d.data, &d.size) < 0) return -1;
        } else if (!strcmp(name, "index")) {
            if (val == Py_None) continue;
            if (PyBytes_Check(val)) {
                if (mv_bytes(val, "index", &o->keep[2], &d.index, &d.index_size) < 0) return -1;
            } else {
                PyObject* b = vv_path(val);
                void* buf = NULL;
                size_t n = 0;
                int rc;
                if (!b) return -1;
                rc = yvv_read_file(PyBytes_AsString(b), &buf, &n, err, sizeof err);
                Py_DECREF(b);
                if (rc < 0) { PyErr_SetString(VvNotFoundError, err); return -1; }
                free(o->mv->owned[1]);
                o->mv->owned[1] = buf;
                d.index = buf;
                d.index_size = n;
            }
        } else if (!strcmp(name, "timeline")) {
            tlobj = val == Py_None ? NULL : val;
        } else if (!strcmp(name, "records")) {
            long long r = PyLong_AsLongLong(val);
            if (r == -1 && PyErr_Occurred()) return -1;
            if (r < 0) { PyErr_SetString(VsArgumentError, "records: >= 0 (0 = 36000)"); return -1; }
            records = r;
        } else if (mv_option(&d, name, val) < 0) {
            return -1;
        }
    }
    if (path && path != Py_None) {
        PyObject* b = vv_path(path);
        if (!b) return -1;
        Py_XDECREF(o->keep[0]);
        o->keep[0] = b;
        d.path = PyBytes_AsString(b);
    }
    if (!d.path && !d.data) { PyErr_SetString(VsArgumentError, "Movie(): a path, or data=bytes"); return -1; }
    if (d.path && !yvv_exists(d.path)) { PyErr_Format(VvNotFoundError, "ysp_video: cannot open %s", d.path); return -1; }
    if (d.path && d.data) { PyErr_SetString(VsArgumentError, "Movie(): a path or data=, not both"); return -1; }
    if (tlobj) {
        void* p = ysp_timeline_api_ptr->head.acquire(tlobj, err, sizeof err);
        if (!p) { PyErr_SetString(VsArgumentError, err); return -1; }
        o->mv->tl = (ytl_timeline*)p;
        d.timeline = o->mv->tl;
    }
    err[0] = 0;
    so->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    ok = yv_movie_open(o->mv, go->g, &d, records, err, sizeof err);
    Py_END_ALLOW_THREADS
    so->busy = 0;
    if (!ok) {
        /* yvid_open() returns false without a code (its message names the
         * cause), so the class is the base one; a missing file was found
         * before the call */
        yv_movie_free(o->mv);
        o->mv = NULL;
        PyErr_SetString(VsError, err[0] ? err : "ysp_video: open failed");
        return -1;
    }
    Py_INCREF(gfx);
    o->gfx = gfx;
    return 0;
}

static PyObject* Movie_close(PyObject* self, PyObject* unused) {
    MovieObject* o = MV_OF(self);
    ScreenObject* s = mv_screen(o);
    (void)unused;
    if (s && s->busy) { PyErr_SetString(VsBusyError, "ysp_video: another thread is in a call on the movie's Screen"); return NULL; }
    mv_shut(o);
    Py_RETURN_NONE;
}
static PyObject* Movie_enter(PyObject* self, PyObject* unused) { (void)unused; Py_INCREF(self); return self; }
static PyObject* Movie_exit(PyObject* self, PyObject* args) { (void)args; return Movie_close(self, NULL); }

static PyObject* Movie_update(PyObject* self, PyObject* unused) {
    MovieObject* o = MV_OF(self);
    const char* why;
    int rc;
    (void)unused;
    if (!mv_ready(o)) return NULL;
    rc = yv_movie_update(o->mv, &why);
    if (rc < 0) return mv_fail(o, rc, why);
    if (rc == YVID_ENDED) Py_RETURN_FALSE;
    Py_RETURN_TRUE;
}

static PyObject* Movie_draw(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "x", "y", "w", "h", "ori", "opacity", "linear", "place", "anchor", NULL };
    MovieObject* o = MV_OF(self);
    double x = 0, y = 0, w = 0, h = 0, ori = 0, op = 1.0;
    int linear = 0, pl = YGFX_CENTER, an = YGFX_CENTER, rc;
    const char *place = NULL, *anchor = NULL, *why;
    char err[256];
    yvid_stim_desc sd;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|ddddddpss", kwlist, &x, &y, &w, &h, &ori, &op, &linear, &place, &anchor))
        return NULL;
    if (place && yvv_lookup(yvv_aligns, "place", place, &pl, err, sizeof err) < 0) { PyErr_SetString(VsArgumentError, err); return NULL; }
    if (anchor && yvv_lookup(yvv_aligns, "anchor", anchor, &an, err, sizeof err) < 0) { PyErr_SetString(VsArgumentError, err); return NULL; }
    if (!mv_ready(o)) return NULL;
    memset(&sd, 0, sizeof sd);
    sd.place = (ygfx_align)pl;
    sd.anchor = (ygfx_align)an;
    sd.x = (float)x; sd.y = (float)y; sd.w = (float)w; sd.h = (float)h; sd.ori = (float)ori;
    sd.opacity = (float)op;
    sd.linear = linear != 0;
    rc = yv_movie_draw(o->mv, &sd, &why);
    if (rc < 0) {
        if (why) return mv_fail(o, rc, why);
        PyErr_SetString(vs_gfx_exc(rc), ygfx_error(&o->mv->gfx->g));
        return NULL;
    }
    Py_RETURN_NONE;
}

/* A time argument: None = ASAP (0), else seconds > 0. */
static int mv_at(PyObject* a, const char* what, int64_t* out) {
    if (!a || a == Py_None) { *out = YVID_ASAP; return 0; }
    if (vs_obj_s(a, what, 0, out) < 0) return -1;
    if (*out == 0) { PyErr_Format(VsArgumentError, "%s: a time > 0 on the ysp_rt clock, or None for as soon as possible", what); return -1; }
    return 0;
}

/* resume: None = ASAP, "paused" = stay paused, else a time. */
static int mv_resume(PyObject* a, int64_t* out) {
    if (a && PyUnicode_Check(a)) {
        char s[16];
        if (vs_utf8(a, s, sizeof s) < 0) return -1;
        if (strcmp(s, "paused") != 0) { PyErr_SetString(VsArgumentError, "resume: None (as soon as possible), 'paused' or a time"); return -1; }
        *out = YVID_STAY_PAUSED;
        return 0;
    }
    return mv_at(a, "resume", out);
}

static PyObject* mv_id(MovieObject* o, int64_t id) {
    if (id < 0) return mv_fail(o, (int)id, NULL);
    return PyLong_FromLongLong(id);
}

static PyObject* Movie_play(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "at", NULL };
    MovieObject* o = MV_OF(self);
    PyObject* at = NULL;
    int64_t t;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|O", kwlist, &at)) return NULL;
    if (mv_at(at, "play(at)", &t) < 0 || !mv_ready(o)) return NULL;
    return mv_id(o, yvid_play_at(&o->mv->m, t));
}

static PyObject* Movie_pause(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "at", NULL };
    MovieObject* o = MV_OF(self);
    PyObject* at = NULL;
    int64_t t;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|O", kwlist, &at)) return NULL;
    if (mv_at(at, "pause(at)", &t) < 0 || !mv_ready(o)) return NULL;
    return mv_id(o, yvid_pause_at(&o->mv->m, t));
}

static PyObject* Movie_seek(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "t", "resume", NULL };
    MovieObject* o = MV_OF(self);
    PyObject *tt, *res = NULL;
    int64_t t, r;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|O", kwlist, &tt, &res)) return NULL;
    if (vs_obj_s(tt, "seek(t): the movie time", 0, &t) < 0 || mv_resume(res, &r) < 0 || !mv_ready(o)) return NULL;
    return mv_id(o, yvid_seek(&o->mv->m, t, r));
}

static PyObject* Movie_seek_frame(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "frame", "resume", NULL };
    MovieObject* o = MV_OF(self);
    long long i;
    PyObject* res = NULL;
    int64_t r;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "L|O", kwlist, &i, &res)) return NULL;
    if (mv_resume(res, &r) < 0 || !mv_ready(o)) return NULL;
    return mv_id(o, yvid_seek_frame(&o->mv->m, i, r));
}

static PyObject* Movie_show(PyObject* self, PyObject* arg) {
    MovieObject* o = MV_OF(self);
    long long i = PyLong_AsLongLong(arg);
    int rc;
    if (i == -1 && PyErr_Occurred()) return NULL;
    if (!mv_ready(o)) return NULL;
    rc = yvid_show(&o->mv->m, i);
    if (rc < 0) return mv_fail(o, rc, NULL);
    Py_RETURN_NONE;
}

static PyObject* Movie_result(PyObject* self, PyObject* arg) {
    MovieObject* o = MV_OF(self);
    long long id = PyLong_AsLongLong(arg);
    yrt_event e;
    yv_fields f;
    int rc;
    if (id == -1 && PyErr_Occurred()) return NULL;
    if (!mv_ready(o)) return NULL;
    rc = yvid_result(&o->mv->m, id, &e);
    if (rc == YVID_PENDING) Py_RETURN_NONE;
    if (rc == YVID_ERR_NOT_FOUND) {
        PyErr_Format(VvNotFoundError, "ysp_video: no control %lld (an id from play, pause or seek; the last 8 are kept)", id);
        return NULL;
    }
    if (rc < 0) return mv_fail(o, rc, NULL);
    yvv_event_fields(&e, &f);
    return vv_fields(&f);
}

static PyObject* Movie_last(PyObject* self, PyObject* unused) {
    MovieObject* o = MV_OF(self);
    yvid_record r;
    (void)unused;
    if (!mv_ready(o)) return NULL;
    if (yvid_last(&o->mv->m, &r) != YVID_OK) Py_RETURN_NONE;
    return vv_record(r.display, r.frame, r.onset ? r.onset : r.predicted, r.due, r.movie_t, r.decision, r.why, r.tier,
                     r.path, r.flags, r.ahead, r.shows);
}

static PyObject* Movie_records(PyObject* self, PyObject* unused) {
    MovieObject* o = MV_OF(self);
    yv_movie* mv = o->mv;
    PyObject* list;
    yrt_event e;
    Py_ssize_t i = 0;
    (void)unused;
    if (!mv) return PyList_New(0);
    yv_movie_drain(mv);
    list = PyList_New((Py_ssize_t)mv->fr_n);
    while (list && yv_movie_take(mv->fr, mv->fr_cap, &mv->fr_head, &mv->fr_n, &e)) {
        PyObject* x = vv_record_ev(&e);
        if (!x) { Py_DECREF(list); return NULL; }
        PyList_SetItem(list, i++, x);
    }
    return list;
}

static PyObject* Movie_events(PyObject* self, PyObject* unused) {
    MovieObject* o = MV_OF(self);
    yv_movie* mv = o->mv;
    PyObject* list;
    yrt_event e;
    yv_fields f;
    (void)unused;
    if (!mv) return PyList_New(0);
    yv_movie_drain(mv);
    list = PyList_New(0);
    while (list && yv_movie_take(mv->ev, mv->ev_cap, &mv->ev_head, &mv->ev_n, &e)) {
        PyObject* x;
        yvv_event_fields(&e, &f);
        x = vv_fields(&f);
        if (!x || PyList_Append(list, x) < 0) { Py_XDECREF(x); Py_DECREF(list); return NULL; }
        Py_DECREF(x);
    }
    return list;
}

static PyObject* Movie_movie_time(PyObject* self, PyObject* arg) {
    MovieObject* o = MV_OF(self);
    int64_t t;
    if (vs_obj_s(arg, "movie_time(t)", 0, &t) < 0 || !mv_ready(o)) return NULL;
    return PyFloat_FromDouble(yv_s(yvid_movie_time(&o->mv->m, t)));
}

/* An Audio of ysp.audio, pinned; NULL with the error set. */
static yau_audio* mv_audio(PyObject* a) {
    char err[256];
    void* p = ysp_audio_api_ptr->head.acquire(a, err, sizeof err);
    if (!p) PyErr_SetString(PyExc_TypeError, err);
    return (yau_audio*)p;
}

static PyObject* Movie_soundtrack(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "audio", "path", "data", "db", "channels", "ramp", "ring", "follow", NULL };
    MovieObject* o = MV_OF(self);
    PyObject *audio, *path = Py_None, *data = Py_None, *pb = NULL, *rampo = NULL, *ringo = NULL;
    double db = 0.0;
    unsigned long long channels = 0;
    int follow = 1, rc;
    yvid_soundtrack_desc sd;
    yau_audio* au;
    yau_caps c;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|OOdKOOp", kwlist, &audio, &path, &data, &db, &channels, &rampo, &ringo, &follow))
        return NULL;
    if (!mv_ready(o)) return NULL;
    memset(&sd, 0, sizeof sd);
    if (rampo && rampo != Py_None && vs_obj_s(rampo, "ramp", 0, &sd.ramp_ns) < 0) return NULL;
    if ((path == Py_None) == (data == Py_None)) { PyErr_SetString(VsArgumentError, "soundtrack(audio, path) or soundtrack(audio, data=bytes)"); return NULL; }
    if (path != Py_None) {
        if (!(pb = vv_path(path))) return NULL;
        sd.path = PyBytes_AsString(pb);
    } else if (mv_bytes(data, "data", &o->keep[3], &sd.data, &sd.size) < 0) {
        return NULL;
    }
    sd.db = (float)db;
    sd.channels = channels;
    if (o->mv->snd_au) { Py_XDECREF(pb); PyErr_SetString(VsOrderError, "ysp_video: the movie has a soundtrack already"); return NULL; }
    if (!(au = mv_audio(audio))) { Py_XDECREF(pb); return NULL; }
    if (ringo && ringo != Py_None) {
        int64_t ns;
        if (vs_obj_s(ringo, "ring", 0, &ns) < 0) { yv_video_release(ysp_audio_api_ptr->head.release, au); Py_XDECREF(pb); return NULL; }
        yau_get_caps(au, &c);
        sd.ring = (int64_t)((double)ns * 1e-9 * c.rate + 0.5);
    }
    rc = yvid_soundtrack(&o->mv->m, au, &sd);
    Py_XDECREF(pb);
    if (rc < 0) { yv_video_release(ysp_audio_api_ptr->head.release, au); return mv_fail(o, rc, NULL); }
    o->mv->snd_au = au;
    if (follow && !o->mv->follow_au) {
        if (!(o->mv->follow_au = mv_audio(audio))) return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject* Movie_follow_audio(PyObject* self, PyObject* arg) {
    MovieObject* o = MV_OF(self);
    if (!mv_ready(o)) return NULL;
    if (o->mv->follow_au) {
        yau_audio* p = o->mv->follow_au;
        o->mv->follow_au = NULL;
        yv_video_release(ysp_audio_api_ptr->head.release, p);
    }
    if (arg != Py_None && !(o->mv->follow_au = mv_audio(arg))) return NULL;
    Py_RETURN_NONE;
}

static PyObject* Movie_describe(PyObject* self, PyObject* unused) {
    MovieObject* o = MV_OF(self);
    char line[2048];
    (void)unused;
    if (!mv_ready(o)) return NULL;
    yvid_describe(&o->mv->m, line, sizeof line);
    return PyUnicode_FromString(line);
}

static PyObject* Movie_get_info(PyObject* self, void* c) {
    MovieObject* o = MV_OF(self);
    yvid_info i;
    yv_fields f;
    PyObject *d, *rb, *db;
    int k;
    (void)c;
    if (!mv_ready(o)) return NULL;
    yvid_get_info(&o->mv->m, &i);
    yvv_info_fields(&i, &f);
    d = vv_fields(&f);
    if (!d) return NULL;
    rb = PyDict_New();
    db = PyDict_New();
    for (k = 0; rb && db && k < YVID_WHY_COUNT; k++) {
        PyObject* a = PyLong_FromUnsignedLongLong(i.repeats_by[k]);
        PyObject* b = PyLong_FromUnsignedLongLong(i.drops_by[k]);
        int bad = !a || !b || PyDict_SetItemString(rb, yvv_whys[k], a) < 0 || PyDict_SetItemString(db, yvv_whys[k], b) < 0;
        Py_XDECREF(a);
        Py_XDECREF(b);
        if (bad) { Py_CLEAR(rb); break; }
    }
    if (!rb || !db || PyDict_SetItemString(d, "repeats_by", rb) < 0 || PyDict_SetItemString(d, "drops_by", db) < 0) {
        Py_XDECREF(rb); Py_XDECREF(db); Py_DECREF(d);
        return NULL;
    }
    Py_DECREF(rb);
    Py_DECREF(db);
    return d;
}

static PyObject* Movie_get_open(PyObject* self, void* c) { (void)c; return PyBool_FromLong(MV_OF(self)->mv && MV_OF(self)->mv->open); }
static PyObject* Movie_get_error(PyObject* self, void* c) {
    (void)c;
    return PyUnicode_FromString(MV_OF(self)->mv ? yvid_error(&MV_OF(self)->mv->m) : "");
}
static PyObject* Movie_get_records_lost(PyObject* self, void* c) {
    yv_movie* mv = MV_OF(self)->mv;
    (void)c;
    return PyLong_FromUnsignedLongLong(mv ? mv->fr_lost + mv->ring_lost : 0);
}
static PyObject* Movie_get_events_lost(PyObject* self, void* c) {
    yv_movie* mv = MV_OF(self)->mv;
    (void)c;
    return PyLong_FromUnsignedLongLong(mv ? mv->ev_lost : 0);
}
static PyObject* Movie_get_gfx(PyObject* self, void* c) {
    PyObject* g = MV_OF(self)->gfx ? MV_OF(self)->gfx : Py_None;
    (void)c;
    Py_INCREF(g);
    return g;
}

static PyMethodDef Movie_methods[] = {
    { "close", Movie_close, METH_NOARGS, "close(): stops the decode thread and the soundtrack; the records stay readable." },
    { "__enter__", Movie_enter, METH_NOARGS, NULL },
    { "__exit__", Movie_exit, METH_VARARGS, NULL },
    { "update", Movie_update, METH_NOARGS,
      "update() -> bool: the frame due at the Screen's current frame's predicted onset goes into the texture; "
      "between Screen.begin() and Gfx.begin(), once a frame. False from the first frame past the movie's end. Never waits." },
    { "draw", (PyCFunction)(void (*)(void))Movie_draw, METH_VARARGS | METH_KEYWORDS,
      "draw(x=0, y=0, w=0, h=0, ori=0, opacity=1, linear=False, place='center', anchor='center'): the movie as an image "
      "between Gfx.begin() and Gfx.end(); w, h 0 = the frame's pixels (1:1)." },
    { "play", (PyCFunction)(void (*)(void))Movie_play, METH_VARARGS | METH_KEYWORDS,
      "play(at=None) -> id: starts at a time on the ysp_rt clock (seconds), or as soon as the frames are ready." },
    { "pause", (PyCFunction)(void (*)(void))Movie_pause, METH_VARARGS | METH_KEYWORDS, "pause(at=None) -> id." },
    { "seek", (PyCFunction)(void (*)(void))Movie_seek, METH_VARARGS | METH_KEYWORDS,
      "seek(t, resume=None) -> id: to movie time t (seconds); resume None = as soon as ready, 'paused', or a time." },
    { "seek_frame", (PyCFunction)(void (*)(void))Movie_seek_frame, METH_VARARGS | METH_KEYWORDS, "seek_frame(frame, resume=None) -> id." },
    { "show", Movie_show, METH_O, "show(frame): this display frame shows the frame (manual mode); before update()." },
    { "result", Movie_result, METH_O, "result(id) -> dict, or None while the control has not happened (the PLAY, PAUSE or SEEK record)." },
    { "last", Movie_last, METH_NOARGS, "last() -> Record of the newest update(), its onset predicted (F_PENDING), or None." },
    { "records", Movie_records, METH_NOARGS, "records() -> list[Record]: the FRAME records completed since the last call, oldest first." },
    { "events", Movie_events, METH_NOARGS,
      "events() -> list[dict]: the other records since the last call (open, play, pause, seek, loop, end, drop, clock, decode, sound, rate)." },
    { "movie_time", Movie_movie_time, METH_O, "movie_time(t) -> seconds: the movie time at t on the ysp_rt clock." },
    { "soundtrack", (PyCFunction)(void (*)(void))Movie_soundtrack, METH_VARARGS | METH_KEYWORDS,
      "soundtrack(audio, path=None, data=None, db=0, channels=0, ramp=None, ring=None, follow=True): a WAV at the "
      "device's rate played as a ysp.audio stream with the movie; before the first play. follow: the movie clock follows "
      "the sound (follow_audio). The Audio is held: its close() raises BusyError until the movie closes." },
    { "follow_audio", Movie_follow_audio, METH_O,
      "follow_audio(audio): each update() takes the movie time from the audio clock (the soundtrack's sample, or the "
      "device's stream position); None stops it." },
    { "describe", Movie_describe, METH_NOARGS, "describe() -> str: the log line (file, form, cadence, backend, counts)." },
    { NULL, NULL, 0, NULL }
};
static PyGetSetDef Movie_getset[] = {
    { "info", Movie_get_info, NULL, "dict: size, rate, frames, duration (s and ns), form, cadence, counts", NULL },
    { "open", Movie_get_open, NULL, "True while open (a closed Gfx or Screen closes its movies)", NULL },
    { "error", Movie_get_error, NULL, "the message of the last call that failed", NULL },
    { "records_lost", Movie_get_records_lost, NULL, "FRAME records lost before records() read them", NULL },
    { "events_lost", Movie_get_events_lost, NULL, "other records lost before events() read them", NULL },
    { "gfx", Movie_get_gfx, NULL, "the Gfx it draws on", NULL },
    { NULL, NULL, NULL, NULL, NULL }
};
static PyType_Slot Movie_slots[] = {
    { Py_tp_doc, "Movie(gfx, path=None, *, data=None, index=None, timeline=None, records=36000, **options): a movie as a "
                 "stimulus (ysp/video.h). The file in the canonical form: a frame sequence, or MPEG-1 or MP4 with its "
                 ".yspvi index (make_index()). options(): the keywords. Each display frame: update(), then draw()." },
    { Py_tp_new, PyType_GenericNew },
    { Py_tp_init, Movie_init },
    { Py_tp_dealloc, Movie_dealloc },
    { Py_tp_methods, Movie_methods },
    { Py_tp_getset, Movie_getset },
    { 0, NULL }
};
static PyType_Spec Movie_spec = { "ysp.video.Movie", sizeof(MovieObject), 0, Py_TPFLAGS_DEFAULT, Movie_slots };

/* --- SeqWriter ------------------------------------------------------------------------- */

typedef struct SeqObject {
    PyObject_HEAD
    yvid_seq* w;
    int       busy;
} SeqObject;

#define SQ_OF(o) ((SeqObject*)(o))

static void Seq_dealloc(PyObject* self) {
    SeqObject* o = SQ_OF(self);
    PyTypeObject* tp = Py_TYPE(self);
    freefunc fr = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    if (o->w) { yvid_seq_close(o->w); free(o->w); o->w = NULL; }
    fr(self);
    Py_DECREF(tp);
}

static int vv_color(PyObject* v, const yv_name* t, const char* what, uint8_t* out) {
    char s[32], err[256];
    int x;
    if (!v || v == Py_None) return 0;
    if (!PyUnicode_Check(v)) { PyErr_Format(PyExc_TypeError, "%s: a name", what); return -1; }
    if (vs_utf8(v, s, sizeof s) < 0) return -1;
    if (yvv_lookup(t, what, s, &x, err, sizeof err) < 0) { PyErr_SetString(VsArgumentError, err); return -1; }
    *out = (uint8_t)x;
    return 0;
}

static int vv_fps(PyObject* fps, int32_t* num, int32_t* den) {
    if (PyTuple_Check(fps) && PyTuple_Size(fps) == 2) {
        long a = PyLong_AsLong(PyTuple_GetItem(fps, 0)), b = PyLong_AsLong(PyTuple_GetItem(fps, 1));
        if (PyErr_Occurred()) return -1;
        *num = (int32_t)a;
        *den = (int32_t)b;
    } else {
        long a = PyLong_AsLong(fps);
        if (a == -1 && PyErr_Occurred()) {
            PyErr_Clear();
            PyErr_SetString(VsArgumentError, "fps: an integer, or (num, den)");
            return -1;
        }
        *num = (int32_t)a;
        *den = 1;
    }
    if (*num <= 0 || *den <= 0) { PyErr_SetString(VsArgumentError, "fps: > 0"); return -1; }
    return 0;
}

static int Seq_init(PyObject* self, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "path", "w", "h", "fps", "format", "compression", "matrix", "range", "transfer", "primaries",
                              "siting", "max_frames", NULL };
    SeqObject* o = SQ_OF(self);
    PyObject *path, *fps, *pb, *mat = NULL, *rng = NULL, *trc = NULL, *pri = NULL, *sit = NULL;
    const char *fmt = "rgba8", *comp = "raw";
    int w, h, f;
    long long maxf = 0;
    char err[256];
    yvid_seq_desc d;
    bool ok;
    if (o->w) { PyErr_SetString(VsError, "SeqWriter.__init__ called twice"); return -1; }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OiiO|ssOOOOOL", kwlist, &path, &w, &h, &fps, &fmt, &comp, &mat, &rng, &trc,
                                     &pri, &sit, &maxf))
        return -1;
    memset(&d, 0, sizeof d);
    d.w = w;
    d.h = h;
    if (vv_fps(fps, &d.fps_num, &d.fps_den) < 0) return -1;
    if (yvv_lookup(yvv_formats, "format", fmt, &f, err, sizeof err) < 0) { PyErr_SetString(VsArgumentError, err); return -1; }
    d.format = f;
    if (!strcmp(comp, "qoi")) d.compression = YVID_SEQ_QOI;
    else if (strcmp(comp, "raw") != 0) { PyErr_SetString(VsArgumentError, "compression: 'raw' or 'qoi' (RGBA8)"); return -1; }
    if (vv_color(mat, yvv_matrices, "matrix", &d.matrix) < 0 || vv_color(rng, yvv_ranges, "range", &d.range) < 0 ||
        vv_color(trc, yvv_transfers, "transfer", &d.transfer) < 0 || vv_color(pri, yvv_primaries, "primaries", &d.primaries) < 0 ||
        vv_color(sit, yvv_sitings, "siting", &d.siting) < 0)
        return -1;
    d.max_frames = maxf;
    if (!(pb = vv_path(path))) return -1;
    d.path = PyBytes_AsString(pb);
    o->w = (yvid_seq*)calloc(1, sizeof(yvid_seq));
    if (!o->w) { Py_DECREF(pb); PyErr_NoMemory(); return -1; }
    Py_BEGIN_ALLOW_THREADS
    ok = yvid_seq_open(o->w, &d);
    Py_END_ALLOW_THREADS
    Py_DECREF(pb);
    if (!ok) {
        PyErr_SetString(VsArgumentError, yvid_seq_error(o->w));
        free(o->w);
        o->w = NULL;
        return -1;
    }
    return 0;
}

static int sq_ready(SeqObject* o) {
    if (o->busy) { PyErr_SetString(VsBusyError, "ysp_video: another thread is in a call on this SeqWriter"); return 0; }
    if (!o->w || !o->w->open) { PyErr_SetString(VsClosedError, "ysp_video: the writer is finished or closed"); return 0; }
    return 1;
}

static PyObject* Seq_write(PyObject* self, PyObject* arg) {
    SeqObject* o = SQ_OF(self);
    const void* planes[3];
    int32_t strides[3];
    char* p;
    Py_ssize_t n;
    size_t need;
    int rc;
    if (!sq_ready(o)) return NULL;
    if (!PyBytes_Check(arg)) { PyErr_SetString(PyExc_TypeError, "write(frame): bytes (bytes(array) or array.tobytes())"); return NULL; }
    if (PyBytes_AsStringAndSize(arg, &p, &n) < 0) return NULL;
    need = yvv_frame_layout(o->w->d.format, o->w->d.w, o->w->d.h, (const uint8_t*)p, planes, strides);
    if (need == 0 || (size_t)n != need) {
        PyErr_Format(VsArgumentError, "write(frame): %zu bytes for a %dx%d %s frame (planes tight, in order), got %zd", need,
                     (int)o->w->d.w, (int)o->w->d.h, yvv_name(yvv_formats, o->w->d.format), n);
        return NULL;
    }
    o->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    rc = yvid_seq_write(o->w, planes, strides, NULL);
    Py_END_ALLOW_THREADS
    o->busy = 0;
    if (rc < 0) { PyErr_SetString(vv_exc(rc), yvid_seq_error(o->w)); return NULL; }
    Py_RETURN_NONE;
}

static PyObject* Seq_finish(PyObject* self, PyObject* unused) {
    SeqObject* o = SQ_OF(self);
    int rc;
    (void)unused;
    if (!sq_ready(o)) return NULL;
    o->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    rc = yvid_seq_finish(o->w);
    Py_END_ALLOW_THREADS
    o->busy = 0;
    if (rc < 0) { PyErr_SetString(vv_exc(rc), yvid_seq_error(o->w)); return NULL; }
    Py_RETURN_NONE;
}

static PyObject* Seq_close(PyObject* self, PyObject* unused) {
    SeqObject* o = SQ_OF(self);
    (void)unused;
    if (o->busy) { PyErr_SetString(VsBusyError, "ysp_video: another thread is in a call on this SeqWriter"); return NULL; }
    if (o->w) {
        Py_BEGIN_ALLOW_THREADS
        yvid_seq_close(o->w);
        Py_END_ALLOW_THREADS
    }
    Py_RETURN_NONE;
}

static PyObject* Seq_enter(PyObject* self, PyObject* unused) { (void)unused; Py_INCREF(self); return self; }
static PyObject* Seq_exit(PyObject* self, PyObject* args) {
    SeqObject* o = SQ_OF(self);
    PyObject* et = PyTuple_GetItem(args, 0);
    /* a clean exit finishes and raises its error; after an exception the
     * file is closed as it is */
    if (et == Py_None && o->w && o->w->open) {
        PyObject* r = Seq_finish(self, NULL);
        if (!r) return NULL;
        Py_DECREF(r);
    }
    return Seq_close(self, NULL);
}

static PyObject* Seq_get_frames(PyObject* self, void* c) {
    (void)c;
    return PyLong_FromLongLong(SQ_OF(self)->w ? SQ_OF(self)->w->n : 0);
}
static PyObject* Seq_get_frame_bytes(PyObject* self, void* c) {
    SeqObject* o = SQ_OF(self);
    const void* p[3];
    int32_t s[3];
    (void)c;
    if (!o->w) return PyLong_FromLong(0);
    return PyLong_FromSize_t(yvv_frame_layout(o->w->d.format, o->w->d.w, o->w->d.h, NULL, p, s));
}

static PyMethodDef Seq_methods[] = {
    { "write", Seq_write, METH_O, "write(frame): appends a frame, bytes with its planes tight and in order (GIL released)." },
    { "finish", Seq_finish, METH_NOARGS, "finish(): writes the index and the header; the file is complete only after it." },
    { "close", Seq_close, METH_NOARGS, "close(): finishes a writer not finished, then releases it." },
    { "__enter__", Seq_enter, METH_NOARGS, NULL },
    { "__exit__", Seq_exit, METH_VARARGS, NULL },
    { NULL, NULL, 0, NULL }
};
static PyGetSetDef Seq_getset[] = {
    { "frames", Seq_get_frames, NULL, "frames written", NULL },
    { "frame_bytes", Seq_get_frame_bytes, NULL, "the bytes write() takes", NULL },
    { NULL, NULL, NULL, NULL, NULL }
};
static PyType_Slot Seq_slots[] = {
    { Py_tp_doc, "SeqWriter(path, w, h, fps, format='rgba8', compression='raw', matrix=None, range=None, transfer=None, "
                 "primaries=None, siting=None, max_frames=0): writes a frame sequence (.yspseq), value-exact. fps an "
                 "integer or (num, den); format r8, rg8, rgba8, r16f, rgba16f, r32f, rgba32f, r16, i420 or nv12 (YUV needs "
                 "the five color names); compression 'qoi' for rgba8." },
    { Py_tp_new, PyType_GenericNew },
    { Py_tp_init, Seq_init },
    { Py_tp_dealloc, Seq_dealloc },
    { Py_tp_methods, Seq_methods },
    { Py_tp_getset, Seq_getset },
    { 0, NULL }
};
static PyType_Spec Seq_spec = { "ysp.video.SeqWriter", sizeof(SeqObject), 0, Py_TPFLAGS_DEFAULT, Seq_slots };

/* --- module functions ------------------------------------------------------------------- */

static PyObject* vm_probe(PyObject* m, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "path", "data", "index", NULL };
    PyObject *path = Py_None, *data = Py_None, *index = Py_None, *pb = NULL;
    yvid_desc d;
    yvid_info i;
    yv_fields f;
    char err[512];
    char* s;
    Py_ssize_t n;
    void* owned = NULL;
    int rc;
    (void)m;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|OOO", kwlist, &path, &data, &index)) return NULL;
    memset(&d, 0, sizeof d);
    if (path != Py_None) {
        if (!(pb = vv_path(path))) return NULL;
        d.path = PyBytes_AsString(pb);
    }
    if (data != Py_None) {
        if (!PyBytes_Check(data) || PyBytes_AsStringAndSize(data, &s, &n) < 0) { Py_XDECREF(pb); PyErr_SetString(PyExc_TypeError, "data: bytes"); return NULL; }
        d.data = s;
        d.size = (size_t)n;
    }
    if (index != Py_None) {
        if (PyBytes_Check(index)) {
            if (PyBytes_AsStringAndSize(index, &s, &n) < 0) { Py_XDECREF(pb); return NULL; }
            d.index = s;
            d.index_size = (size_t)n;
        } else {
            PyObject* ib = vv_path(index);
            size_t len = 0;
            if (!ib) { Py_XDECREF(pb); return NULL; }
            rc = yvv_read_file(PyBytes_AsString(ib), &owned, &len, err, sizeof err);
            Py_DECREF(ib);
            if (rc < 0) { Py_XDECREF(pb); PyErr_SetString(VvNotFoundError, err); return NULL; }
            d.index = owned;
            d.index_size = len;
        }
    }
    if ((d.path != NULL) == (d.data != NULL)) { Py_XDECREF(pb); free(owned); PyErr_SetString(VsArgumentError, "probe(path) or probe(data=bytes)"); return NULL; }
    /* yvid_probe() answers a missing file with REFUSED: found here first */
    if (d.path && !yvv_exists(d.path)) {
        PyErr_Format(VvNotFoundError, "ysp_video: cannot open %s", d.path);
        Py_XDECREF(pb);
        free(owned);
        return NULL;
    }
    err[0] = 0;
    Py_BEGIN_ALLOW_THREADS
    rc = yvid_probe(&d, &i, err, sizeof err);
    Py_END_ALLOW_THREADS
    Py_XDECREF(pb);
    free(owned);
    if (rc < 0) { PyErr_SetString(vv_exc(rc), err[0] ? err : yvid_strerror(rc)); return NULL; }
    yvv_info_fields(&i, &f);
    return vv_fields(&f);
}

static PyObject* vm_make_index(PyObject* m, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "media", "index", "matrix", "range", "transfer", "primaries", "siting", "backend", "hw", "note",
                              "no_seek_check", NULL };
    PyObject *media, *index = Py_None, *mat = NULL, *rng = NULL, *trc = NULL, *pri = NULL, *sit = NULL, *mb, *ib = NULL;
    const char *backend = "auto", *hw = "off", *note = NULL;
    int nsc = 0, v;
    yvid_index_desc d;
    char err[512];
    int64_t k;
    (void)m;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|OOOOOOsszp", kwlist, &media, &index, &mat, &rng, &trc, &pri, &sit, &backend,
                                     &hw, &note, &nsc))
        return NULL;
    memset(&d, 0, sizeof d);
    if (vv_color(mat, yvv_matrices, "matrix", &d.matrix) < 0 || vv_color(rng, yvv_ranges, "range", &d.range) < 0 ||
        vv_color(trc, yvv_transfers, "transfer", &d.transfer) < 0 || vv_color(pri, yvv_primaries, "primaries", &d.primaries) < 0 ||
        vv_color(sit, yvv_sitings, "siting", &d.siting) < 0)
        return NULL;
    if (yvv_lookup(yvv_backends, "backend", backend, &v, err, sizeof err) < 0) { PyErr_SetString(VsArgumentError, err); return NULL; }
    d.backend = (yvid_backend)v;
    if (yvv_lookup(yvv_hws, "hw", hw, &v, err, sizeof err) < 0) { PyErr_SetString(VsArgumentError, err); return NULL; }
    d.hw = (yvid_hw)v;
    d.note = note;
    d.no_seek_check = nsc != 0;
    if (!(mb = vv_path(media))) return NULL;
    if (index != Py_None && !(ib = vv_path(index))) { Py_DECREF(mb); return NULL; }
    err[0] = 0;
    {
        const char* mp = PyBytes_AsString(mb);
        const char* ip = ib ? PyBytes_AsString(ib) : NULL;
        Py_BEGIN_ALLOW_THREADS
        k = yvid_index_make(mp, ip, &d, err, sizeof err);
        Py_END_ALLOW_THREADS
    }
    Py_DECREF(mb);
    Py_XDECREF(ib);
    if (k < 0) { PyErr_SetString(vv_exc((int)k), err[0] ? err : yvid_strerror((int)k)); return NULL; }
    return PyLong_FromLongLong(k);
}

static PyObject* vm_options(PyObject* m, PyObject* unused) {
    PyObject* d = PyDict_New();
    int i;
    static const char* extra[][2] = {
        { "data", "the file in memory (bytes), in place of a path" },
        { "index", "the .yspvi index: a path or bytes; None = path + '.yspvi'" },
        { "timeline", "a ysp.timeline.Timeline whose base (base=) is the movie clock" },
        { "records", "FRAME records kept between records() calls; 0 = 36000" },
    };
    (void)m;
    (void)unused;
    for (i = 0; d && yvv_opts[i].name; i++) {
        PyObject* s = PyUnicode_FromString(yvv_opts[i].doc);
        if (!s || PyDict_SetItemString(d, yvv_opts[i].name, s) < 0) { Py_XDECREF(s); Py_DECREF(d); return NULL; }
        Py_DECREF(s);
    }
    for (i = 0; d && i < 4; i++) {
        PyObject* s = PyUnicode_FromString(extra[i][1]);
        if (!s || PyDict_SetItemString(d, extra[i][0], s) < 0) { Py_XDECREF(s); Py_DECREF(d); return NULL; }
        Py_DECREF(s);
    }
    return d;
}

/* Tests: the module's ysp_rt on a virtual clock (ysp/rt.h VIRTUAL CLOCK),
 * so a run on the simulated display repeats exactly, as video_play --sim.
 * Open nothing across the switch. */
static PyObject* vm_virtual_clock(PyObject* m, PyObject* args) {
    int on;
    long long t0 = 0;
    (void)m;
    if (!PyArg_ParseTuple(args, "p|L", &on, &t0)) return NULL;
    if (on) {
        if (yrt_virtual_begin(t0) < 0) { PyErr_SetString(VsOrderError, "the clock is virtual already"); return NULL; }
    } else {
        yrt_virtual_end();
    }
    Py_RETURN_NONE;
}

#define VV_MODULE_METHODS \
    { "probe", (PyCFunction)(void (*)(void))vm_probe, METH_VARARGS | METH_KEYWORDS, \
      "probe(path=None, *, data=None, index=None) -> dict: a movie's description without opening it (size, rate, frames, duration)." }, \
    { "make_index", (PyCFunction)(void (*)(void))vm_make_index, METH_VARARGS | METH_KEYWORDS, \
      "make_index(media, index=None, *, matrix, range, transfer, primaries, siting, backend='auto', hw='off', note=None, " \
      "no_seek_check=False) -> frames: the .yspvi of an MPEG-1 or MP4 file; decodes every frame (GIL released)." }, \
    { "video_options", vm_options, METH_NOARGS, "video_options() -> dict: Movie()'s keyword options." }, \
    { "_virtual_clock", vm_virtual_clock, METH_VARARGS, "_virtual_clock(on, t0_ns=0): tests only (ysp/rt.h VIRTUAL CLOCK)." },

/* Types, exceptions and constants of ysp.video; 0 or -1. */
static int vv_init(PyObject* m) {
    static const struct { const char* name; long v; } consts[] = {
        { "SHOWN", YVID_SHOWN }, { "REPEATED", YVID_REPEATED }, { "DROPPED", YVID_DROPPED },
        { "WHY_DUE", YVID_WHY_DUE }, { "WHY_CADENCE", YVID_WHY_CADENCE }, { "WHY_DISPLAY_LATE", YVID_WHY_DISPLAY_LATE },
        { "WHY_DECODE_LATE", YVID_WHY_DECODE_LATE }, { "WHY_DRIFT", YVID_WHY_DRIFT }, { "WHY_SEEK", YVID_WHY_SEEK },
        { "WHY_LOOP", YVID_WHY_LOOP }, { "WHY_MANUAL", YVID_WHY_MANUAL },
        { "F_PENDING", YVID_F_PENDING }, { "F_LATE", YVID_F_LATE }, { "F_ESTIMATED", YVID_F_ESTIMATED },
        { "F_BELOW_TIER", YVID_F_BELOW_TIER }, { "F_AUDIO_CLOCK", YVID_F_AUDIO_CLOCK }, { "F_TS_MISMATCH", YVID_F_TS_MISMATCH },
        { "F_HASH_MISMATCH", YVID_F_HASH_MISMATCH }, { "F_NOT_SHOWN", YVID_F_NOT_SHOWN },
        { "PATH_UPLOAD", YVID_PATH_UPLOAD }, { "PATH_GPU", YVID_PATH_GPU },
    };
    PyObject* c;
    int i;
    if (!(MovieType = vs_add_type(m, &Movie_spec, "Movie"))) return -1;
    if (!(SeqWriterType = vs_add_type(m, &Seq_spec, "SeqWriter"))) return -1;
    if (!(VRecordType = vs_add_seq(m, &vv_record_desc, "MovieRecord"))) return -1;
    if (!(VvFormatError = vs_add_exc(m, "ysp.video.FormatError", "FormatError", VsArgumentError))) return -1;
    if (!(VvNotFoundError = vs_add_exc2(m, "ysp.video.NotFoundError", "NotFoundError", VsError, PyExc_FileNotFoundError))) return -1;
    if (!(VvIOError = vs_add_exc2(m, "ysp.video.IOError", "VideoIOError", VsError, PyExc_OSError))) return -1;
    /* in a dict, not as attributes: ysp.screen re-exports _vis's upper-case names */
    c = PyDict_New();
    if (!c) return -1;
    for (i = 0; i < (int)(sizeof consts / sizeof consts[0]); i++) {
        PyObject* v = PyLong_FromLong(consts[i].v);
        if (!v || PyDict_SetItemString(c, consts[i].name, v) < 0) { Py_XDECREF(v); Py_DECREF(c); return -1; }
        Py_DECREF(v);
    }
    if (PyModule_AddObject(m, "_video_constants", c) < 0) { Py_DECREF(c); return -1; }
    return 0;
}
