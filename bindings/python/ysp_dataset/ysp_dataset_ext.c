/* ysp_dataset_ext.c - CPython extension wrapping ysp/dataset.h (module ysp.dataset)
 *
 * A thin, dependency-free binding: it needs only Python.h, and compiles the
 * header's implementation (with ysp/trials.h, ysp/table.h and ysp/json.h,
 * which it includes) into the module. Built against the stable ABI / Limited
 * API (CPython 3.8+), so the type is a heap type made with PyType_FromSpec.
 *
 *     import ysp.dataset as yds
 *     ds = yds.Dataset("data/P07_20261021T140312Z",
 *                      [("participant", "const", "P07"), ("trial_index", "int"),
 *                       ("response", "string"), ("correct", "bool"), ("rt_s", "time")],
 *                      session={"experiment": "gabor-2afc", "seed": 20261021})
 *     ds.write_frame(frame=k, trial_index=i, onset_s=t_flip)
 *     ds.write_event(t_s=t_key, trial_index=i, kind="keyboard", text="left")
 *     ds.write_trial(trial_index=i, response="left", correct=True, rt_s=0.512)
 *     ds.finish()
 *     open("P07.zip", "wb").write(ds.zip())
 *
 * TIMES are seconds as floats in and out, rounded to the nearest ns for the
 * header, which writes them back as seconds. A data value may be negative
 * (a residual, an SOA); None, and NaN in a number or time column, are a
 * missing value (an empty cell), as pandas reads them.
 *
 * ROWS. Every column of trials.csv and of a stream is a YTR_COL_SET column
 * (or YTR_COL_TEXT for "const"): the binding sets each value the row has
 * with ytr_data_set_*() and writes the row, so a column the row does not
 * name is empty, BOOL included, which a bound struct field cannot say.
 * frames.csv, events.csv and clock.csv fill the header's structs after
 * their _init(), from the header's own column lists. A row's text is
 * encoded into one scratch buffer the object keeps (PyUnicode_ReadChar:
 * PyUnicode_AsUTF8 is Limited API only from 3.10, and AsUTF8String makes
 * a bytes object per value), so a row allocates nothing in the binding
 * once the buffer has grown to the longest row.
 *
 * AN ABNORMAL END. finish(), close() and the end of a with block mark the
 * session "closed": true. A with block left by an exception, abandon(), and
 * the teardown of a Dataset never finished or closed (with a
 * ResourceWarning) call yds_abandon(): the files close and session.json
 * keeps "closed": false, as after a crash.
 *
 * THREADS. The header has none. The calls that sync the disk (the open,
 * session_set(), session_push(), finish(), close(), zip()) release the GIL;
 * a busy flag refuses a second call on the same object meanwhile, since the
 * header wants one thread per data set. Row writes keep the GIL: a row
 * costs about 12 us, and getting the GIL back after a release can wait for
 * another thread's switch interval (5 ms).
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x03080000
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define YSP_DATASET_IMPLEMENTATION
#include "ysp/dataset.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The distribution's version is the header's (docs/bindings.md, Versions);
 * tests/test_dataset.py checks that this and pyproject.toml agree. */
#define DS_VERSION YDS_VERSION_STRING

static PyObject* DsError;
static PyObject* DsArgumentError;
static PyObject* DsExistsError;
static PyObject* DsWriteError;
static PyObject* DsClosedError;
static PyObject* DsOrderError;
static PyObject* DsFullError;
static PyObject* DsRangeError;

static PyObject* ds_fail(int code, const char* msg) {
    PyObject* exc = DsError;
    switch (code) {
    case YDS_ERR_ARG:    exc = DsArgumentError; break;
    case YDS_ERR_IO:     exc = DsWriteError; break;
    case YDS_ERR_EXISTS: exc = DsExistsError; break;
    case YDS_ERR_CLOSED: exc = DsClosedError; break;
    case YDS_ERR_ORDER:  exc = DsOrderError; break;
    case YDS_ERR_FULL:   exc = DsFullError; break;
    case YDS_ERR_RANGE:  exc = DsRangeError; break;
    case YDS_ERR_NOMEM:  exc = PyExc_MemoryError; break;
    default: break;
    }
    PyErr_SetString(exc, msg && msg[0] ? msg : yds_strerror(code));
    return NULL;
}

/* --- seconds ---------------------------------------------------------------- */

/* |s| below this keeps s * 1e9 inside int64 (9.22e18 ns, 292 years). */
#define DS_S_MAX 9.2e9

/* Seconds to the nearest ns. s * 1e9 rounds once; fma() gives that
 * rounding's error exactly, so the result is the ns nearest the real
 * product, and ns -> ns / 1e9 -> ns holds for every |s| below 2^23 s (97
 * days); ysp.rdk's conversion, with a sign. */
static int ds_s_to_ns(double s, int64_t* out) {
    volatile double p;   /* stored as a double: x87 (i686) would keep 80 bits */
    double e, n, r;
    if (!(fabs(s) < DS_S_MAX)) {
        PyErr_SetString(DsArgumentError, "a time is seconds, finite and below 9.2e9 in size (None or NaN: missing)");
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

/* --- objects ---------------------------------------------------------------- */

/* One value of a row, converted before any is set, so a refused value
 * leaves the writer's row as it was. */
typedef struct ds_pend {
    int col;
    int type;
    int missing;
    union { int64_t i; double d; size_t off; } v;
} ds_pend;

/* trials.csv or one stream: the columns as the caller declared them. */
typedef struct ds_table {
    int        n;
    int        id;           /* -1: trials.csv; else the stream's id      */
    PyObject** key;          /* interned names, one per column            */
    uint8_t*   type;         /* ytr_col_type                              */
} ds_table;

typedef struct {
    PyObject_HEAD
    yds_dataset* ds;         /* calloc: about 770 KB; the pages of unused
                              * streams are never touched                 */
    int          busy;       /* a call with the GIL released is running   */
    int          finished;
    ds_table     trials;
    ds_table     stream[YDS_MAX_STREAMS];
    PyObject*    rows_at_finish;
    char*        sbuf;       /* a row's text, NUL-separated               */
    size_t       scap, slen;
    ds_pend      pend[YTR_DATA_MAX_COLS];
} DsObject;

static PyObject* DsType;

/* A fixed table's field: frames.csv, events.csv, clock.csv. Taken from the
 * header's own column lists (the implementation is in this file), so a
 * field the header adds is bound without an edit here. */
typedef struct ds_field {
    char      csv[YTB_MAX_NAME + 1];   /* the column's name: _ns -> _s   */
    int       type;
    size_t    offset;
    PyObject* key;
} ds_field;

static ds_field ds_frame_f[YDS_FRAME_COLS], ds_event_f[YDS_EVENT_COLS], ds_clock_f[YDS_CLOCK_COLS];

static int ds_fields_init(ds_field* f, const ytr_col* c, int n) {
    int i;
    for (i = 0; i < n; i++) {
        size_t len = strlen(c[i].name);
        memcpy(f[i].csv, c[i].name, len + 1);
        if (c[i].type == YTR_COL_TIME_NS && len > 3 && strcmp(c[i].name + len - 3, "_ns") == 0)
            f[i].csv[len - 2] = 's', f[i].csv[len - 1] = '\0';
        f[i].type = (int)c[i].type;
        f[i].offset = c[i].offset;
        if (!(f[i].key = PyUnicode_InternFromString(f[i].csv))) return -1;
    }
    return 0;
}

static int ds_ready(DsObject* self, int need_open) {
    if (!self->ds) { PyErr_SetString(DsClosedError, "the Dataset was not opened"); return -1; }
    if (self->busy) { PyErr_SetString(DsError, "the Dataset is in use by another thread (one thread writes one data set)"); return -1; }
    if (need_open && !self->ds->open) { PyErr_SetString(DsClosedError, "the data set is not open"); return -1; }
    return 0;
}

/* A key's column, by identity first: names in source code are interned. */
static int ds_find(PyObject* key, PyObject* const* keys, int n, size_t stride) {
    int i;
    for (i = 0; i < n; i++)
        if (*(PyObject* const*)((const char*)keys + i * stride) == key) return i;
    if (!PyUnicode_Check(key)) return -2;
    for (i = 0; i < n; i++) {
        int c = PyUnicode_Compare(key, *(PyObject* const*)((const char*)keys + i * stride));
        if (c == 0) return i;
        if (c == -1 && PyErr_Occurred()) return -3;
    }
    return -1;
}

/* --- values ----------------------------------------------------------------- */

/* A str (or UTF-8 bytes) into the scratch buffer; *off is where it starts. */
static int ds_text(DsObject* self, PyObject* o, size_t* off) {
    Py_ssize_t n, i;
    size_t need;
    char* p;
    if (PyBytes_Check(o)) {
        char* b;
        Py_ssize_t bn;
        if (PyBytes_AsStringAndSize(o, &b, &bn) < 0) return -1;
        n = bn;
        need = self->slen + (size_t)bn + 1;
        if (memchr(b, 0, (size_t)bn)) { PyErr_SetString(DsArgumentError, "a text value holds a NUL"); return -1; }
        if (need > self->scap) {
            size_t cap = self->scap ? self->scap : 256;
            char* q;
            while (cap < need) cap *= 2;
            if (!(q = (char*)realloc(self->sbuf, cap))) { PyErr_NoMemory(); return -1; }
            self->sbuf = q, self->scap = cap;
        }
        memcpy(self->sbuf + self->slen, b, (size_t)bn);
        self->sbuf[self->slen + (size_t)bn] = '\0';
        *off = self->slen;
        self->slen = need;
        return 0;
    }
    if (!PyUnicode_Check(o)) { PyErr_SetString(PyExc_TypeError, "a text column takes a str (or UTF-8 bytes)"); return -1; }
    n = PyUnicode_GetLength(o);
    if (n < 0) return -1;
    need = self->slen + 4 * (size_t)n + 1;
    if (need > self->scap) {
        size_t cap = self->scap ? self->scap : 256;
        char* q;
        while (cap < need) cap *= 2;
        if (!(q = (char*)realloc(self->sbuf, cap))) { PyErr_NoMemory(); return -1; }
        self->sbuf = q, self->scap = cap;
    }
    p = self->sbuf + self->slen;
    for (i = 0; i < n; i++) {
        Py_UCS4 c = PyUnicode_ReadChar(o, i);
        if (c == 0) { PyErr_SetString(DsArgumentError, "a text value holds a NUL"); return -1; }
        /* A lone surrogate goes out as its 3 bytes, which are not UTF-8:
         * the writer turns them into U+FFFD, as for any bad byte. */
        if (c < 0x80) *p++ = (char)c;
        else if (c < 0x800) { *p++ = (char)(0xC0 | (c >> 6)); *p++ = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            *p++ = (char)(0xE0 | (c >> 12)); *p++ = (char)(0x80 | ((c >> 6) & 0x3F)); *p++ = (char)(0x80 | (c & 0x3F));
        } else {
            *p++ = (char)(0xF0 | (c >> 18)); *p++ = (char)(0x80 | ((c >> 12) & 0x3F));
            *p++ = (char)(0x80 | ((c >> 6) & 0x3F)); *p++ = (char)(0x80 | (c & 0x3F));
        }
    }
    *p++ = '\0';
    *off = self->slen;
    self->slen = (size_t)(p - self->sbuf);
    return 0;
}

/* One value of column type t into *pv. None, and NaN for a number, are
 * missing. An integer column refuses a fraction rather than truncating. */
static int ds_value(DsObject* self, PyObject* o, int t, ds_pend* pv, PyObject* col) {
    double d;
    pv->missing = 0;
    if (o == Py_None) { pv->missing = 1; return 0; }
    switch (t) {
    case YTR_COL_INT:
    case YTR_COL_INT64: {
        long long v;
        int over = 0;
        if (PyFloat_Check(o)) {
            d = PyFloat_AsDouble(o);
            if (isnan(d)) { pv->missing = 1; return 0; }
            if (d != floor(d) || fabs(d) > 9007199254740992.0) goto not_int;
            v = (long long)d;
        } else {
            PyObject* ix = PyNumber_Index(o);
            if (!ix) goto not_int;
            v = PyLong_AsLongLongAndOverflow(ix, &over);
            Py_DECREF(ix);
            if (v == -1 && PyErr_Occurred()) return -1;
            if (over) goto range;
        }
        if (t == YTR_COL_INT ? (v <= INT_MIN || v > INT_MAX) : v == INT64_MIN) goto range;
        pv->v.i = v;
        return 0;
    not_int:
        PyErr_Clear();
        PyErr_Format(DsArgumentError, "column %U: an integer (or None)", col);
        return -1;
    range:
        PyErr_Format(DsArgumentError, "column %U: out of range for %s", col, t == YTR_COL_INT ? "int" : "int64");
        return -1;
    }
    case YTR_COL_DOUBLE:
    case YTR_COL_TIME_NS:
    case YTR_COL_BOOL:
        if (PyUnicode_Check(o) || PyBytes_Check(o)) {
            PyErr_Format(DsArgumentError, "column %U: a number (or None), not text", col);
            return -1;
        }
        d = PyFloat_AsDouble(o);
        if (d == -1.0 && PyErr_Occurred()) {
            PyErr_Clear();
            PyErr_Format(DsArgumentError, "column %U: a number (or None)", col);
            return -1;
        }
        if (isnan(d)) { pv->missing = 1; return 0; }
        if (t == YTR_COL_DOUBLE) { pv->v.d = d; return 0; }
        if (t == YTR_COL_BOOL) {
            if (d != 0.0 && d != 1.0) { PyErr_Format(DsArgumentError, "column %U: a bool (or 0 or 1, or None)", col); return -1; }
            pv->v.i = d != 0.0;
            return 0;
        }
        if (ds_s_to_ns(d, &pv->v.i) < 0) {
            PyErr_Clear();
            PyErr_Format(DsArgumentError, "column %U: seconds, finite and below 9.2e9 in size (None or NaN: missing)", col);
            return -1;
        }
        return 0;
    case YTR_COL_STRING:
        if (!PyUnicode_Check(o) && !PyBytes_Check(o)) {
            PyErr_Format(DsArgumentError, "column %U: a str (or None)", col);
            return -1;
        }
        return ds_text(self, o, &pv->v.off);
    default:
        PyErr_Format(DsArgumentError, "column %U is constant; its text is set at the open", col);
        return -1;
    }
}

/* The row: a mapping as the one positional argument, or keywords. Returns
 * a borrowed dict-like object, or NULL with an error. */
static PyObject* ds_row_arg(PyObject* args, PyObject* kw, Py_ssize_t first, const char* fn) {
    Py_ssize_t na = PyTuple_Size(args);
    if (na > first + 1) { PyErr_Format(PyExc_TypeError, "%s(): one row (a dict) or keywords", fn); return NULL; }
    if (na == first + 1) {
        PyObject* r = PyTuple_GetItem(args, first);
        if (kw && PyDict_Size(kw) > 0) { PyErr_Format(PyExc_TypeError, "%s(): a dict or keywords, not both", fn); return NULL; }
        if (!PyDict_Check(r)) { PyErr_Format(PyExc_TypeError, "%s(): the row is a dict of column: value", fn); return NULL; }
        return r;
    }
    return kw ? kw : Py_None;
}

/* A fixed table's row into its struct, after the struct's _init(). */
static int ds_fill_fixed(DsObject* self, PyObject* row, const ds_field* f, int nf, void* rec, const char* table) {
    Py_ssize_t pos = 0;
    PyObject *k, *v;
    int i, k_str = 0;
    size_t offs[YDS_EVENT_COLS], soff[YDS_EVENT_COLS];
    if (row == Py_None) return 0;
    self->slen = 0;
    while (PyDict_Next(row, &pos, &k, &v)) {
        ds_pend pv;
        char* at;
        int c = ds_find(k, &f[0].key, nf, sizeof(ds_field));
        if (c == -3) return -1;
        if (c < 0) {
            PyErr_Format(DsArgumentError, "%s has no column %R", table, k);
            return -1;
        }
        if (ds_value(self, v, f[c].type, &pv, f[c].key) < 0) return -1;
        if (pv.missing) continue;   /* the _init() set it missing */
        at = (char*)rec + f[c].offset;
        switch (f[c].type) {
        case YTR_COL_INT: *(int*)at = (int)pv.v.i; break;
        case YTR_COL_INT64:
        case YTR_COL_TIME_NS: *(int64_t*)at = pv.v.i; break;
        case YTR_COL_DOUBLE: *(double*)at = pv.v.d; break;
        case YTR_COL_STRING:
            /* The buffer may move while later values come in: pointers
             * after the last one. */
            offs[k_str] = f[c].offset, soff[k_str] = pv.v.off, k_str++;
            break;
        default: break;
        }
    }
    for (i = 0; i < k_str; i++) *(const char**)((char*)rec + offs[i]) = self->sbuf + soff[i];
    return 0;
}

/* A table of the caller's: every value converted, then set, then the row. */
static PyObject* ds_write_table(DsObject* self, ds_table* t, PyObject* row) {
    Py_ssize_t pos = 0;
    PyObject *k, *v;
    int i, n = 0, rc;
    ytr_data* w;
    if (ds_ready(self, 1) < 0) return NULL;
    self->slen = 0;
    if (row != Py_None) {
        while (PyDict_Next(row, &pos, &k, &v)) {
            int c = ds_find(k, t->key, t->n, sizeof(PyObject*));
            if (c == -3) return NULL;
            if (c < 0) {
                PyErr_Format(DsArgumentError, "%s has no column %R", t->id < 0 ? "trials.csv" : "the stream", k);
                return NULL;
            }
            self->pend[n].col = c;
            self->pend[n].type = t->type[c];
            if (ds_value(self, v, t->type[c], &self->pend[n], t->key[c]) < 0) return NULL;
            n++;
        }
    }
    w = t->id < 0 ? yds_trials_writer(self->ds) : yds_stream_writer(self->ds, t->id);
    for (i = 0; i < n; i++) {
        ds_pend* p = &self->pend[i];
        if (p->missing) continue;   /* not set: an empty cell */
        switch (p->type) {
        case YTR_COL_INT: rc = ytr_data_set_int(w, p->col, (int)p->v.i); break;
        case YTR_COL_INT64: rc = ytr_data_set_int64(w, p->col, p->v.i); break;
        case YTR_COL_DOUBLE: rc = ytr_data_set_double(w, p->col, p->v.d); break;
        case YTR_COL_BOOL: rc = ytr_data_set_bool(w, p->col, p->v.i != 0); break;
        case YTR_COL_STRING: rc = ytr_data_set_string(w, p->col, self->sbuf + p->v.off); break;
        default: rc = ytr_data_set_time_ns(w, p->col, p->v.i); break;
        }
        if (rc < 0) return ds_fail(YDS_ERR_ARG, ytr_data_error(w));
    }
    rc = t->id < 0 ? yds_write_trial(self->ds, NULL) : yds_write_stream(self->ds, t->id, NULL);
    if (rc < 0) return ds_fail(rc, yds_error(self->ds));
    Py_RETURN_NONE;
}

/* --- JSON ------------------------------------------------------------------- */

static yjs_value* ds_json(yjs_arena* a, PyObject* o, int depth) {
    yjs_value* v = NULL;
    if (depth > YJS_DEFAULT_DEPTH) { PyErr_SetString(DsArgumentError, "a session value nests deeper than 64"); return NULL; }
    if (o == Py_None) v = yjs_new(a, YJS_NULL);
    else if (PyBool_Check(o)) v = yjs_new(a, o == Py_True ? YJS_TRUE : YJS_FALSE);
    else if (PyLong_Check(o)) {
        int over = 0;
        long long x = PyLong_AsLongLongAndOverflow(o, &over);
        if (x == -1 && PyErr_Occurred()) return NULL;
        if (over) {   /* beyond int64: its decimal digits, exactly */
            PyObject* s = PyObject_Str(o);
            PyObject* b = s ? PyUnicode_AsUTF8String(s) : NULL;
            Py_XDECREF(s);
            if (!b) return NULL;
            v = yjs_new_number(a, PyBytes_AsString(b), (size_t)PyBytes_Size(b));
            Py_DECREF(b);
        } else {
            v = yjs_new_int(a, (int64_t)x);
        }
    } else if (PyFloat_Check(o)) {
        double d = PyFloat_AsDouble(o);
        if (!isfinite(d)) { PyErr_SetString(DsArgumentError, "a session value: JSON has no NaN or infinity"); return NULL; }
        v = yjs_new_double(a, d);
    } else if (PyUnicode_Check(o)) {
        PyObject* b = PyUnicode_AsUTF8String(o);
        if (!b) return NULL;
        v = yjs_new_string(a, PyBytes_AsString(b), (size_t)PyBytes_Size(b));
        Py_DECREF(b);
        if (!v) { PyErr_SetString(DsArgumentError, "a session string: not UTF-8, or a NUL"); return NULL; }
        return v;
    } else if (PyDict_Check(o)) {
        Py_ssize_t pos = 0;
        PyObject *k, *x;
        if (!(v = yjs_new(a, YJS_OBJECT))) goto nomem;
        while (PyDict_Next(o, &pos, &k, &x)) {
            PyObject* b;
            yjs_value* c;
            int rc;
            if (!PyUnicode_Check(k)) { PyErr_SetString(DsArgumentError, "a session object's keys are str"); return NULL; }
            if (!(c = ds_json(a, x, depth + 1))) return NULL;
            if (!(b = PyUnicode_AsUTF8String(k))) return NULL;
            rc = yjs_set_n(a, v, PyBytes_AsString(b), (size_t)PyBytes_Size(b), c);
            Py_DECREF(b);
            if (rc != YJS_OK) {
                if (rc == YJS_ERR_NOMEM) goto nomem;
                PyErr_SetString(DsArgumentError, "a session key: not UTF-8, or a NUL");
                return NULL;
            }
        }
        return v;
    } else if (PyList_Check(o) || PyTuple_Check(o)) {
        Py_ssize_t i, n = PySequence_Size(o);
        if (!(v = yjs_new(a, YJS_ARRAY))) goto nomem;
        for (i = 0; i < n; i++) {
            PyObject* x = PySequence_GetItem(o, i);
            yjs_value* c;
            if (!x) return NULL;
            c = ds_json(a, x, depth + 1);
            Py_DECREF(x);
            if (!c) return NULL;
            if (yjs_push(a, v, c) != YJS_OK) goto nomem;
        }
        return v;
    } else if (PyIndex_Check(o)) {   /* numpy integers */
        PyObject* ix = PyNumber_Index(o);
        if (!ix) return NULL;
        v = ds_json(a, ix, depth);
        Py_DECREF(ix);
        return v;
    } else if (PyNumber_Check(o)) {   /* numpy floats and bools have __float__ */
        double d = PyFloat_AsDouble(o);
        if (d == -1.0 && PyErr_Occurred()) return NULL;
        if (!isfinite(d)) { PyErr_SetString(DsArgumentError, "a session value: JSON has no NaN or infinity"); return NULL; }
        v = yjs_new_double(a, d);
    } else {
        PyObject* tn = PyObject_GetAttrString((PyObject*)Py_TYPE(o), "__name__");
        PyObject* b = tn ? PyUnicode_AsUTF8String(tn) : NULL;
        PyErr_Format(DsArgumentError, "a session value is None, bool, int, float, str, list, tuple or dict, not %s",
                     b ? PyBytes_AsString(b) : "this type");
        Py_XDECREF(b);
        Py_XDECREF(tn);
        return NULL;
    }
    if (!v) goto nomem;
    return v;
nomem:
    PyErr_NoMemory();
    return NULL;
}

/* --- columns ---------------------------------------------------------------- */

static void ds_table_free(ds_table* t) {
    int i;
    for (i = 0; i < t->n; i++) Py_XDECREF(t->key[i]);
    PyMem_Free(t->key);
    PyMem_Free(t->type);
    memset(t, 0, sizeof *t);
}

/* A list of (name, type[, decimals | text]) into the header's columns and
 * the binding's table. The ytr_col names point into `hold`, a list of bytes
 * kept until the header has copied them. */
static int ds_parse_cols(PyObject* spec, ytr_col* cols, ds_table* t, PyObject* hold, const char* what) {
    static const struct { const char* name; int type; } types[] = {
        { "int", YTR_COL_INT }, { "int64", YTR_COL_INT64 }, { "double", YTR_COL_DOUBLE }, { "float", YTR_COL_DOUBLE },
        { "bool", YTR_COL_BOOL }, { "string", YTR_COL_STRING }, { "str", YTR_COL_STRING }, { "time", YTR_COL_TIME_NS },
        { "const", YTR_COL_CONST },
    };
    Py_ssize_t n, i;
    PyObject* seq = PySequence_Fast(spec, "columns: a list of (name, type) tuples");
    if (!seq) return -1;
    n = PySequence_Size(seq);
    if (n < 1 || n > YTR_DATA_MAX_COLS) {
        PyErr_Format(DsArgumentError, "%s: 1 to %d columns", what, YTR_DATA_MAX_COLS);
        goto fail;
    }
    t->key = (PyObject**)PyMem_Calloc((size_t)n, sizeof(PyObject*));
    t->type = (uint8_t*)PyMem_Calloc((size_t)n, 1);
    if (!t->key || !t->type) { PyErr_NoMemory(); goto fail; }
    for (i = 0; i < n; i++) {
        PyObject* it = PySequence_GetItem(seq, i);
        PyObject *nm, *ty, *ex = NULL, *nb = NULL, *tb = NULL;
        Py_ssize_t m;
        size_t k;
        const char* tname;
        int type = -1;
        if (!it) goto fail;
        m = PyTuple_Check(it) || PyList_Check(it) ? PySequence_Size(it) : -1;
        if (m != 2 && m != 3) {
            Py_DECREF(it);
            PyErr_Format(DsArgumentError, "%s: column %zd is not (name, type) or (name, type, decimals | text)", what, i);
            goto fail;
        }
        nm = PySequence_GetItem(it, 0);
        ty = PySequence_GetItem(it, 1);
        if (m == 3) ex = PySequence_GetItem(it, 2);
        Py_DECREF(it);
        if (!nm || !ty || (m == 3 && !ex) || !PyUnicode_Check(nm) || !PyUnicode_Check(ty)) {
            Py_XDECREF(nm); Py_XDECREF(ty); Py_XDECREF(ex);
            if (!PyErr_Occurred()) PyErr_Format(DsArgumentError, "%s: column %zd: the name and the type are str", what, i);
            goto fail;
        }
        nb = PyUnicode_AsUTF8String(nm);
        tb = PyUnicode_AsUTF8String(ty);
        if (!nb || !tb || PyList_Append(hold, nb) < 0) {
            Py_XDECREF(nm); Py_XDECREF(ty); Py_XDECREF(ex); Py_XDECREF(nb); Py_XDECREF(tb);
            goto fail;
        }
        tname = PyBytes_AsString(tb);
        for (k = 0; k < sizeof types / sizeof types[0]; k++)
            if (strcmp(tname, types[k].name) == 0) type = types[k].type;
        memset(&cols[i], 0, sizeof cols[i]);
        cols[i].name = PyBytes_AsString(nb);
        cols[i].type = (ytr_col_type)type;
        cols[i].offset = YTR_UNBOUND;
        if (type < 0) {
            PyErr_Format(DsArgumentError, "%s: column %s: type '%s' is not int, int64, double, bool, string, time or const",
                         what, cols[i].name, tname);
        } else if (type == YTR_COL_TIME_NS) {
            size_t len = strlen(cols[i].name);
            /* The header also takes "_ns" and writes "_s"; here every value
             * is seconds, so the name says so as the file does. */
            if (len < 2 || strcmp(cols[i].name + len - 2, "_s") != 0)
                PyErr_Format(DsArgumentError, "%s: column %s: a time column's name ends in _s (its values are seconds)",
                             what, cols[i].name);
        }
        if (!PyErr_Occurred() && ex) {
            if (type == YTR_COL_CONST) {
                PyObject* xb = PyUnicode_Check(ex) ? PyUnicode_AsUTF8String(ex) : NULL;
                if (!xb || PyList_Append(hold, xb) < 0) {
                    if (!PyErr_Occurred()) PyErr_Format(DsArgumentError, "%s: column %s: a const column's text is a str", what, cols[i].name);
                } else {
                    cols[i].text = PyBytes_AsString(xb);
                }
                Py_XDECREF(xb);
            } else if (type == YTR_COL_DOUBLE || type == YTR_COL_TIME_NS) {
                long d = PyLong_Check(ex) ? PyLong_AsLong(ex) : -1;
                if (d < 0 || d > 17) {
                    PyErr_Clear();
                    PyErr_Format(DsArgumentError, "%s: column %s: decimals is an int (double 0..17, time 0..9)", what, cols[i].name);
                } else {
                    cols[i].decimals = (int)d;
                }
            } else {
                PyErr_Format(DsArgumentError, "%s: column %s: only double and time take decimals, only const a text",
                             what, cols[i].name);
            }
        } else if (!PyErr_Occurred() && type == YTR_COL_CONST) {
            PyErr_Format(DsArgumentError, "%s: column %s: a const column is (name, \"const\", text)", what, cols[i].name);
        }
        t->type[i] = (uint8_t)(type < 0 ? 0 : type);
        if (!PyErr_Occurred()) {
            PyObject* key = nm;
            Py_INCREF(key);
            PyUnicode_InternInPlace(&key);   /* may swap key for the interned copy */
            t->key[i] = key;
            t->n = (int)i + 1;
        }
        Py_DECREF(nm); Py_DECREF(ty); Py_XDECREF(ex); Py_DECREF(nb); Py_DECREF(tb);
        if (PyErr_Occurred()) goto fail;
    }
    Py_DECREF(seq);
    return (int)n;
fail:
    Py_DECREF(seq);
    ds_table_free(t);
    return -1;
}

/* --- Dataset ---------------------------------------------------------------- */

static PyObject* Ds_new(PyTypeObject* tp, PyObject* args, PyObject* kw) {
    allocfunc a = (allocfunc)PyType_GetSlot(tp, Py_tp_alloc);
    DsObject* self = (DsObject*)a(tp, 0);
    (void)args;
    (void)kw;
    if (!self) return NULL;
    self->trials.id = -1;
    return (PyObject*)self;
}

static int Ds_init(PyObject* o, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "dir", "trial_cols", "session", "sync", NULL };
    DsObject* self = (DsObject*)o;
    PyObject *dir, *spec, *session = Py_None, *path = NULL, *pb = NULL, *hold = NULL;
    int sync = 0, n, ok;
    ytr_col cols[YTR_DATA_MAX_COLS];
    yds_desc d;
    yjs_arena a;
    int arena = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|$Op", kwlist, &dir, &spec, &session, &sync)) return -1;
    if (self->ds) {
        PyErr_SetString(DsError, "this Dataset was opened already; make a new one");
        return -1;
    }
    if (!(path = PyOS_FSPath(dir))) return -1;
    if (PyUnicode_Check(path)) pb = PyUnicode_AsUTF8String(path);
    else { pb = path; Py_INCREF(pb); }
    Py_DECREF(path);
    if (!pb) return -1;
    memset(&d, 0, sizeof d);
    d.dir = PyBytes_AsString(pb);
    d.flush = sync ? YTR_FLUSH_SYNC : YTR_FLUSH_ROW;
    if (!(hold = PyList_New(0))) goto fail;
    if ((n = ds_parse_cols(spec, cols, &self->trials, hold, "trial_cols")) < 0) goto fail;
    self->trials.id = -1;
    d.trial_cols = cols;
    d.n_trial_cols = n;
    if (session != Py_None) {
        if (!PyDict_Check(session)) { PyErr_SetString(DsArgumentError, "session: a dict of the caller's keys"); goto fail; }
        yjs_arena_init_heap(&a, 0);
        arena = 1;
        if (!(d.session = ds_json(&a, session, 0))) goto fail;
    }
    if (!(self->ds = (yds_dataset*)calloc(1, sizeof(yds_dataset)))) { PyErr_NoMemory(); goto fail; }
    self->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    ok = yds_open(self->ds, &d);
    Py_END_ALLOW_THREADS
    self->busy = 0;
    if (!ok) {
        ds_fail(self->ds->error_code, yds_error(self->ds));
        free(self->ds);
        self->ds = NULL;
        goto fail;
    }
    if (arena) yjs_arena_free(&a);
    Py_DECREF(hold);
    Py_DECREF(pb);
    return 0;
fail:
    if (arena) yjs_arena_free(&a);
    ds_table_free(&self->trials);
    self->trials.id = -1;
    Py_XDECREF(hold);
    Py_DECREF(pb);
    return -1;
}

/* The teardown of a data set the script never finished or closed is an
 * abnormal end (an exception unwound past it, the interpreter is exiting):
 * abandon it, so session.json keeps "closed": false, and warn as an
 * unclosed file does. */
static void Ds_release(DsObject* self) {
    int i;
    if (self->ds) {
        yds_dataset* ds = self->ds;
        if (ds->open) {
            PyObject *et, *ev, *tb;
            PyErr_Fetch(&et, &ev, &tb);
            if (PyErr_WarnFormat(PyExc_ResourceWarning, 1,
                                 "ysp.dataset.Dataset('%s') was never finished or closed; abandoned: "
                                 "session.json says \"closed\": false", ds->dir) < 0)
                PyErr_WriteUnraisable(NULL);
            PyErr_Restore(et, ev, tb);
        }
        Py_BEGIN_ALLOW_THREADS
        yds_abandon(ds);   /* frees the arena of a finished one too */
        Py_END_ALLOW_THREADS
        free(ds);
        self->ds = NULL;
    }
    ds_table_free(&self->trials);
    for (i = 0; i < YDS_MAX_STREAMS; i++) ds_table_free(&self->stream[i]);
    Py_CLEAR(self->rows_at_finish);
    free(self->sbuf);
    self->sbuf = NULL;
    self->scap = self->slen = 0;
}

static void Ds_dealloc(PyObject* o) {
    PyTypeObject* tp = Py_TYPE(o);
    freefunc f = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    Ds_release((DsObject*)o);
    f(o);
    Py_DECREF(tp);
}

static PyObject* Ds_write_trial(PyObject* o, PyObject* args, PyObject* kw) {
    DsObject* self = (DsObject*)o;
    PyObject* row = ds_row_arg(args, kw, 0, "write_trial");
    if (!row) return NULL;
    return ds_write_table(self, &self->trials, row);
}

static PyObject* Ds_write_frame(PyObject* o, PyObject* args, PyObject* kw) {
    DsObject* self = (DsObject*)o;
    PyObject* row = ds_row_arg(args, kw, 0, "write_frame");
    yds_frame f;
    int rc;
    if (!row || ds_ready(self, 1) < 0) return NULL;
    yds_frame_init(&f);
    if (ds_fill_fixed(self, row, ds_frame_f, YDS_FRAME_COLS, &f, "frames.csv") < 0) return NULL;
    if ((rc = yds_write_frame(self->ds, &f)) < 0) return ds_fail(rc, yds_error(self->ds));
    Py_RETURN_NONE;
}

static PyObject* Ds_write_event(PyObject* o, PyObject* args, PyObject* kw) {
    DsObject* self = (DsObject*)o;
    PyObject* row = ds_row_arg(args, kw, 0, "write_event");
    yds_event e;
    int rc;
    if (!row || ds_ready(self, 1) < 0) return NULL;
    yds_event_init(&e);
    if (ds_fill_fixed(self, row, ds_event_f, YDS_EVENT_COLS, &e, "events.csv") < 0) return NULL;
    if ((rc = yds_write_event(self->ds, &e)) < 0) return ds_fail(rc, yds_error(self->ds));
    Py_RETURN_NONE;
}

static PyObject* Ds_write_clock(PyObject* o, PyObject* args, PyObject* kw) {
    DsObject* self = (DsObject*)o;
    PyObject* row = ds_row_arg(args, kw, 0, "write_clock");
    yds_clock c;
    int rc;
    if (!row || ds_ready(self, 1) < 0) return NULL;
    yds_clock_init(&c);
    if (ds_fill_fixed(self, row, ds_clock_f, YDS_CLOCK_COLS, &c, "clock.csv") < 0) return NULL;
    if ((rc = yds_write_clock(self->ds, &c)) < 0) return ds_fail(rc, yds_error(self->ds));
    Py_RETURN_NONE;
}

static PyObject* Ds_stream(PyObject* o, PyObject* args) {
    DsObject* self = (DsObject*)o;
    const char* name;
    PyObject *spec, *hold;
    ytr_col cols[YTR_DATA_MAX_COLS];
    ds_table t;
    int n, id;
    if (!PyArg_ParseTuple(args, "sO:stream", &name, &spec)) return NULL;
    if (ds_ready(self, 1) < 0) return NULL;
    memset(&t, 0, sizeof t);
    if (!(hold = PyList_New(0))) return NULL;
    if ((n = ds_parse_cols(spec, cols, &t, hold, name)) < 0) { Py_DECREF(hold); return NULL; }
    id = yds_stream(self->ds, name, cols, n);
    Py_DECREF(hold);
    if (id < 0) { ds_table_free(&t); return ds_fail(id, yds_error(self->ds)); }
    t.id = id;
    self->stream[id] = t;
    return PyLong_FromLong(id);
}

/* A stream by its id or its name. */
static ds_table* ds_stream_arg(DsObject* self, PyObject* s) {
    int i;
    if (PyLong_Check(s)) {
        long id = PyLong_AsLong(s);
        if (id >= 0 && id < self->ds->n_streams) return &self->stream[id];
    } else if (PyUnicode_Check(s)) {
        /* Stream names are ASCII (the column rule): no conversion. */
        for (i = 0; i < self->ds->n_streams; i++)
            if (PyUnicode_CompareWithASCIIString(s, self->ds->stream_name[i]) == 0) return &self->stream[i];
    }
    PyErr_SetString(DsArgumentError, "no such stream (an id from stream(), or its name)");
    return NULL;
}

static PyObject* Ds_write_stream(PyObject* o, PyObject* args, PyObject* kw) {
    DsObject* self = (DsObject*)o;
    PyObject* row;
    ds_table* t;
    if (PyTuple_Size(args) < 1) { PyErr_SetString(PyExc_TypeError, "write_stream(stream, row=None, **values)"); return NULL; }
    if (ds_ready(self, 1) < 0) return NULL;
    if (!(row = ds_row_arg(args, kw, 1, "write_stream"))) return NULL;
    if (!(t = ds_stream_arg(self, PyTuple_GetItem(args, 0)))) return NULL;
    return ds_write_table(self, t, row);
}

static PyObject* ds_session_call(DsObject* self, PyObject* args, int push) {
    const char* key;
    PyObject* value;
    yjs_arena a;
    yjs_value* v;
    int rc;
    if (!PyArg_ParseTuple(args, push ? "sO:session_push" : "sO:session_set", &key, &value)) return NULL;
    if (ds_ready(self, 1) < 0) return NULL;
    yjs_arena_init_heap(&a, 0);
    if (!(v = ds_json(&a, value, 0))) { yjs_arena_free(&a); return NULL; }
    self->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    rc = push ? yds_session_push(self->ds, key, v) : yds_session_set(self->ds, key, v);
    Py_END_ALLOW_THREADS
    self->busy = 0;
    yjs_arena_free(&a);
    if (rc < 0) return ds_fail(rc, yds_error(self->ds));
    Py_RETURN_NONE;
}

static PyObject* Ds_session_set(PyObject* o, PyObject* args) { return ds_session_call((DsObject*)o, args, 0); }
static PyObject* Ds_session_push(PyObject* o, PyObject* args) { return ds_session_call((DsObject*)o, args, 1); }

/* The rows written, as session.json's "rows" says them at the finish. */
static PyObject* ds_rows_now(DsObject* self) {
    PyObject *r = NULL, *s = NULL;
    int i;
    yds_dataset* ds = self->ds;
    if (!(s = PyDict_New())) return NULL;
    for (i = 0; i < ds->n_streams; i++) {
        PyObject* v = PyLong_FromLongLong(yds_rows(ds, YDS_TABLE_STREAM + i));
        if (!v || PyDict_SetItemString(s, ds->stream_name[i], v) < 0) { Py_XDECREF(v); Py_DECREF(s); return NULL; }
        Py_DECREF(v);
    }
    r = Py_BuildValue("{s:L,s:L,s:L,s:L,s:N}", "trials", (long long)yds_rows(ds, YDS_TABLE_TRIALS), "frames",
                      (long long)yds_rows(ds, YDS_TABLE_FRAMES), "events", (long long)yds_rows(ds, YDS_TABLE_EVENTS),
                      "clock", (long long)yds_rows(ds, YDS_TABLE_CLOCK), "streams", s);
    return r;
}

static PyObject* Ds_rows(PyObject* o, PyObject* unused) {
    DsObject* self = (DsObject*)o;
    (void)unused;
    if (self->rows_at_finish) { Py_INCREF(self->rows_at_finish); return self->rows_at_finish; }
    if (ds_ready(self, 1) < 0) return NULL;
    return ds_rows_now(self);
}

/* finish() and close(): the counts first, since a finished handle says 0.
 * close() finishes through yds_finish() itself, not yds_close() alone, so a
 * write that failed reaches the caller from close() and from a with block. */
static PyObject* ds_end(DsObject* self, int close) {
    int rc = 0;
    yds_dataset* ds = self->ds;
    if (!ds) {
        if (close) Py_RETURN_NONE;
        return ds_fail(YDS_ERR_CLOSED, "finish(): the Dataset was not opened");
    }
    if (self->busy) { PyErr_SetString(DsError, "the Dataset is in use by another thread (one thread writes one data set)"); return NULL; }
    if (!close && !ds->open) return ds_fail(YDS_ERR_CLOSED, "finish(): the data set is not open");
    if (ds->open && !self->rows_at_finish && !(self->rows_at_finish = ds_rows_now(self))) return NULL;
    self->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    if (ds->open) rc = yds_finish(ds);
    if (close) yds_close(ds);   /* frees session.json's arena */
    Py_END_ALLOW_THREADS
    self->busy = 0;
    if (rc < 0) return ds_fail(rc, yds_error(ds));
    Py_RETURN_NONE;
}

static PyObject* Ds_finish(PyObject* o, PyObject* unused) {
    PyObject* r;
    (void)unused;
    r = ds_end((DsObject*)o, 0);
    if (!r) return NULL;
    Py_DECREF(r);
    Py_INCREF(((DsObject*)o)->rows_at_finish);
    return ((DsObject*)o)->rows_at_finish;
}

static PyObject* Ds_close(PyObject* o, PyObject* unused) {
    (void)unused;
    return ds_end((DsObject*)o, 1);
}

typedef struct ds_mem { char* p; size_t n, cap; } ds_mem;

static bool ds_mem_write(void* ctx, const void* bytes, size_t n) {
    ds_mem* m = (ds_mem*)ctx;
    if (m->n + n > m->cap) {
        size_t cap = m->cap ? m->cap : (size_t)1 << 16;
        char* q;
        while (cap < m->n + n) cap *= 2;
        if (!(q = (char*)realloc(m->p, cap))) return false;
        m->p = q, m->cap = cap;
    }
    memcpy(m->p + m->n, bytes, n);
    m->n += n;
    return true;
}

static PyObject* Ds_zip(PyObject* o, PyObject* args, PyObject* kw) {
    static char* kwlist[] = { "path", NULL };
    DsObject* self = (DsObject*)o;
    PyObject *path = Py_None, *pb = NULL, *out;
    ds_mem m = { NULL, 0, 0 };
    const char* p = NULL;
    int rc;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|O:zip", kwlist, &path)) return NULL;
    if (ds_ready(self, 0) < 0) return NULL;
    if (path != Py_None) {
        PyObject* fp = PyOS_FSPath(path);
        if (!fp) return NULL;
        if (PyUnicode_Check(fp)) pb = PyUnicode_AsUTF8String(fp);
        else { pb = fp; Py_INCREF(pb); }
        Py_DECREF(fp);
        if (!pb) return NULL;
        p = PyBytes_AsString(pb);
    }
    self->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    rc = yds_zip(self->ds, p, p ? NULL : ds_mem_write, &m);
    Py_END_ALLOW_THREADS
    self->busy = 0;
    Py_XDECREF(pb);
    if (rc < 0) {
        free(m.p);
        return ds_fail(rc, yds_error(self->ds));
    }
    if (p) Py_RETURN_NONE;
    out = PyBytes_FromStringAndSize(m.p, (Py_ssize_t)m.n);
    free(m.p);
    return out;
}

static PyObject* Ds_enter(PyObject* o, PyObject* unused) {
    (void)unused;
    Py_INCREF(o);
    return o;
}

static PyObject* Ds_abandon(PyObject* o, PyObject* unused) {
    DsObject* self = (DsObject*)o;
    yds_dataset* ds = self->ds;
    (void)unused;
    if (!ds) Py_RETURN_NONE;
    if (self->busy) {
        PyErr_SetString(DsError, "the Dataset is in use by another thread (one thread writes one data set)");
        return NULL;
    }
    self->busy = 1;
    Py_BEGIN_ALLOW_THREADS
    yds_abandon(ds);
    Py_END_ALLOW_THREADS
    self->busy = 0;
    Py_RETURN_NONE;
}

/* A with block that ends by an exception is an abnormal end: abandon, so
 * session.json keeps "closed": false, and let the exception go on. */
static PyObject* Ds_exit(PyObject* o, PyObject* args) {
    PyObject* r;
    PyObject* et = PyTuple_Size(args) > 0 ? PyTuple_GetItem(args, 0) : Py_None;
    if (et && et != Py_None) r = Ds_abandon(o, NULL);
    else r = ds_end((DsObject*)o, 1);
    if (!r) return NULL;
    Py_DECREF(r);
    Py_RETURN_FALSE;
}

static PyObject* Ds_get_dir(PyObject* o, void* c) {
    DsObject* self = (DsObject*)o;
    (void)c;
    if (!self->ds) Py_RETURN_NONE;
    return PyUnicode_FromString(self->ds->dir);
}

static PyObject* Ds_get_is_open(PyObject* o, void* c) {
    DsObject* self = (DsObject*)o;
    (void)c;
    return PyBool_FromLong(self->ds && self->ds->open);
}

static PyObject* Ds_get_finished(PyObject* o, void* c) {
    DsObject* self = (DsObject*)o;
    (void)c;
    return PyBool_FromLong(self->ds && self->ds->finished);
}

static PyObject* Ds_get_streams(PyObject* o, void* c) {
    DsObject* self = (DsObject*)o;
    PyObject* t;
    int i, n = self->ds ? self->ds->n_streams : 0;
    (void)c;
    if (!(t = PyTuple_New(n))) return NULL;
    for (i = 0; i < n; i++) {
        PyObject* s = PyUnicode_FromString(self->ds->stream_name[i]);
        if (!s) { Py_DECREF(t); return NULL; }
        PyTuple_SetItem(t, i, s);
    }
    return t;
}

static PyObject* Ds_get_trial_columns(PyObject* o, void* c) {
    DsObject* self = (DsObject*)o;
    PyObject* t;
    int i;
    (void)c;
    if (!(t = PyTuple_New(self->trials.n))) return NULL;
    for (i = 0; i < self->trials.n; i++) {
        Py_INCREF(self->trials.key[i]);
        PyTuple_SetItem(t, i, self->trials.key[i]);
    }
    return t;
}

#define KWF(f) (PyCFunction)(void (*)(void))(f), METH_VARARGS | METH_KEYWORDS
static PyMethodDef Ds_methods[] = {
    { "write_trial", KWF(Ds_write_trial), "write_trial(row=None, **values): one row of trials.csv. A column not named is empty." },
    { "write_frame", KWF(Ds_write_frame), "write_frame(row=None, **values): one row of frames.csv (FRAME_COLUMNS)." },
    { "write_event", KWF(Ds_write_event), "write_event(row=None, **values): one row of events.csv (EVENT_COLUMNS)." },
    { "write_clock", KWF(Ds_write_clock), "write_clock(row=None, **values): one row of clock.csv (CLOCK_COLUMNS)." },
    { "stream", Ds_stream, METH_VARARGS, "stream(name, cols) -> id: make streams/<name>.csv with these columns." },
    { "write_stream", KWF(Ds_write_stream), "write_stream(stream, row=None, **values): one row of a stream (its id or name)." },
    { "session_set", Ds_session_set, METH_VARARGS, "session_set(key, value): session.json's key := value; rewrites session.json atomically." },
    { "session_push", Ds_session_push, METH_VARARGS, "session_push(key, value): appends value to the list at key; rewrites session.json atomically." },
    { "rows", Ds_rows, METH_NOARGS, "rows() -> dict: rows written per table, as session.json's \"rows\" at the finish." },
    { "finish", Ds_finish, METH_NOARGS, "finish() -> rows: close every table; session.json says \"closed\": true." },
    { "close", Ds_close, METH_NOARGS, "close(): finish an open data set; safe twice." },
    { "abandon", Ds_abandon, METH_NOARGS, "abandon(): an abnormal end: close the files, leave session.json \"closed\": false." },
    { "zip", KWF(Ds_zip), "zip(path=None) -> bytes or None: the finished folder as one stored zip." },
    { "__enter__", Ds_enter, METH_NOARGS, NULL },
    { "__exit__", Ds_exit, METH_VARARGS, NULL },
    { NULL }
};

static PyGetSetDef Ds_getset[] = {
    { "dir", Ds_get_dir, NULL, "The data set's folder.", NULL },
    { "is_open", Ds_get_is_open, NULL, "True until finish() or close().", NULL },
    { "finished", Ds_get_finished, NULL, "True after finish() or close(): zip() may read the folder.", NULL },
    { "streams", Ds_get_streams, NULL, "The streams' names, by id.", NULL },
    { "trial_columns", Ds_get_trial_columns, NULL, "trials.csv's columns as declared.", NULL },
    { NULL }
};

static PyType_Slot Ds_slots[] = {
    { Py_tp_new, (void*)Ds_new }, { Py_tp_init, (void*)Ds_init }, { Py_tp_dealloc, (void*)Ds_dealloc },
    { Py_tp_methods, Ds_methods }, { Py_tp_getset, Ds_getset },
    { Py_tp_doc, (void*)"Dataset(dir, trial_cols, *, session=None, sync=False): a session's data set (yds_dataset)." },
    { 0, NULL }
};

static PyType_Spec Ds_spec = { "ysp.dataset.Dataset", sizeof(DsObject), 0, Py_TPFLAGS_DEFAULT, Ds_slots };

/* --- module ----------------------------------------------------------------- */

static PyObject* mod_seed_text(PyObject* m, PyObject* arg) {
    char out[YDS_SEED_TEXT];
    unsigned long long s;
    (void)m;
    if (!PyLong_Check(arg)) { PyErr_SetString(PyExc_TypeError, "seed_text(seed): an int in [0, 2**64)"); return NULL; }
    s = PyLong_AsUnsignedLongLong(arg);
    if (s == (unsigned long long)-1 && PyErr_Occurred()) {
        PyErr_Clear();
        PyErr_SetString(DsArgumentError, "seed_text(seed): an int in [0, 2**64)");
        return NULL;
    }
    yds_seed_text((uint64_t)s, out);
    return PyUnicode_FromString(out);
}

static PyObject* mod_version(PyObject* m, PyObject* unused) {
    (void)m;
    (void)unused;
    return PyUnicode_FromString(yds_version());
}

static PyMethodDef module_methods[] = {
    { "seed_text", mod_seed_text, METH_O, "seed_text(seed) -> \"x\" and 16 hex digits: a trial_seed column's text, as ysp/trials.h writes it." },
    { "version", mod_version, METH_NOARGS, "version() -> the header's version string." },
    { NULL }
};

static PyModuleDef ysp_dataset_module = {
    PyModuleDef_HEAD_INIT, "ysp.dataset",
    "A session's data set: session.json, trials, frames, events, clock and stream tables, and a zip (ysp/dataset.h).",
    -1, module_methods,
};

static PyObject* ds_add_exc(PyObject* m, const char* qual, const char* attr, PyObject* base) {
    PyObject* e = PyErr_NewException(qual, base, NULL);
    if (!e) return NULL;
    Py_INCREF(e);
    if (PyModule_AddObject(m, attr, e) < 0) { Py_DECREF(e); Py_DECREF(e); return NULL; }
    return e;
}

static PyObject* ds_sub(PyObject* m, const char* qual, const char* attr, PyObject* other) {
    PyObject* b = other ? PyTuple_Pack(2, DsError, other) : PyTuple_Pack(1, DsError);
    PyObject* e;
    if (!b) return NULL;
    e = ds_add_exc(m, qual, attr, b);
    Py_DECREF(b);
    return e;
}

static int ds_add_names(PyObject* m, const char* attr, const ds_field* f, int n) {
    PyObject* t = PyTuple_New(n);
    int i;
    if (!t) return -1;
    for (i = 0; i < n; i++) { Py_INCREF(f[i].key); PyTuple_SetItem(t, i, f[i].key); }
    if (PyModule_AddObject(m, attr, t) < 0) { Py_DECREF(t); return -1; }
    return 0;
}

PyMODINIT_FUNC PyInit_dataset(void) {
    PyObject* m = PyModule_Create(&ysp_dataset_module);
    if (!m) return NULL;
    if (ds_fields_init(ds_frame_f, yds__frame_cols, YDS_FRAME_COLS) < 0 ||
        ds_fields_init(ds_event_f, yds__event_cols, YDS_EVENT_COLS) < 0 ||
        ds_fields_init(ds_clock_f, yds__clock_cols, YDS_CLOCK_COLS) < 0)
        goto fail;
    if (!(DsType = PyType_FromSpec(&Ds_spec))) goto fail;
    Py_INCREF(DsType);
    if (PyModule_AddObject(m, "Dataset", DsType) < 0) goto fail;
    if (!(DsError = ds_add_exc(m, "ysp.dataset.Error", "Error", NULL))) goto fail;
    if (!(DsArgumentError = ds_sub(m, "ysp.dataset.ArgumentError", "ArgumentError", PyExc_ValueError))) goto fail;
    if (!(DsExistsError = ds_sub(m, "ysp.dataset.ExistsError", "ExistsError", PyExc_FileExistsError))) goto fail;
    if (!(DsWriteError = ds_sub(m, "ysp.dataset.WriteError", "WriteError", PyExc_OSError))) goto fail;
    if (!(DsClosedError = ds_sub(m, "ysp.dataset.ClosedError", "ClosedError", NULL))) goto fail;
    if (!(DsOrderError = ds_sub(m, "ysp.dataset.OrderError", "OrderError", NULL))) goto fail;
    if (!(DsFullError = ds_sub(m, "ysp.dataset.FullError", "FullError", NULL))) goto fail;
    if (!(DsRangeError = ds_sub(m, "ysp.dataset.RangeError", "RangeError", PyExc_ValueError))) goto fail;
    if (ds_add_names(m, "FRAME_COLUMNS", ds_frame_f, YDS_FRAME_COLS) < 0) goto fail;
    if (ds_add_names(m, "EVENT_COLUMNS", ds_event_f, YDS_EVENT_COLS) < 0) goto fail;
    if (ds_add_names(m, "CLOCK_COLUMNS", ds_clock_f, YDS_CLOCK_COLS) < 0) goto fail;
    if (PyModule_AddStringConstant(m, "__version__", DS_VERSION) < 0) goto fail;
    if (PyModule_AddStringConstant(m, "FORMAT", YDS_FORMAT) < 0) goto fail;
    PyModule_AddIntConstant(m, "FORMAT_VERSION", YDS_FORMAT_VERSION);
    PyModule_AddIntConstant(m, "MAX_STREAMS", YDS_MAX_STREAMS);
    PyModule_AddIntConstant(m, "MAX_COLUMNS", YTR_DATA_MAX_COLS);
    return m;
fail:
    Py_DECREF(m);
    return NULL;
}
