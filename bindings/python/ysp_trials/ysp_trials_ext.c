/* ysp_trials_ext.c - CPython extension wrapping ysp/trials.h (module ysp.trials)
 *
 * A thin, dependency-free binding (no nanobind/pybind/Cython): it needs only
 * Python.h. The library implementation is compiled directly into this module.
 *
 * Built against the stable ABI / Limited API (Py_LIMITED_API 3.8), so one
 * compiled ysp/trials.abi3.so works across CPython >= 3.8; the Trials type is
 * a heap type made with PyType_FromSpec.
 *
 *     import ysp.trials as pt
 *     t = pt.Trials(factors=[("orientation", 2), ("contrast", 5)], reps=20,
 *                   order=pt.ORDER_CONSTRAINED,
 *                   constraints=[pt.max_run(0, pt.ANY_LEVEL, 3)], rng=20260923)
 *     while (ti := t.next()) is not None:
 *         t.update(run_trial(t.level(ti.condition, 0), t.level(ti.condition, 1)))
 *
 *     tab = pt.Table(open("conditions.csv", encoding="utf-8").read())
 *     t = pt.Trials(table=tab, rules="order constrained\nreps 10\nmax_run target 3\n",
 *                   participant=7, rng=20260923)
 *     row = t.values(ti.condition)          # {"target": "a", "contrast": 0.25, ...}
 *
 * TABLES. Table wraps ysp/table.h: the parsed block lives in memory the
 * Table owns, and a Trials keeps a reference to its Table, which the header
 * reads for the whole session.
 *
 * CALLBACKS. A track's is_done and a callable rng are Python; the header calls
 * them from next(), done, restore() and (rng) open() and requeue(). Those calls
 * keep the GIL. open() and load() release it when the generator is the
 * binding's own splitmix, since open() calls no track and the repair of a
 * tight design can take tens of milliseconds. An exception in a callback is
 * held until the C call returns and then raised; the callbacks after it in the
 * same call get a neutral answer (not done; u = 0).
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x03080000   /* target the CPython 3.8+ stable ABI */
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"

#include <string.h>

static PyObject* TError;
static PyObject* TArgumentError;
static PyObject* TClosed;
static PyObject* TOutOfOrder;
static PyObject* TFull;

static PyObject* TInfoType;     /* namedtuple TrialInfo */
static PyObject* TTrialType;    /* namedtuple Trial     */
static PyObject* TTableType;    /* ysp.trials.Table     */
static PyObject* TJitterType;   /* namedtuple Jitter    */

/* Heap-type instances visit their type in tp_traverse from 3.9 on only. */
static int g_visit_type = 1;

static PyObject* t_fail(int code) {
    PyObject* exc = TError;
    switch (code) {
    case YTR_ERR_ARG:    exc = TArgumentError; break;
    case YTR_ERR_CLOSED: exc = TClosed; break;
    case YTR_ERR_ORDER:  exc = TOutOfOrder; break;
    case YTR_ERR_FULL:   exc = TFull; break;
    default: break;
    }
    PyErr_SetString(exc, ytr_strerror(code));
    return NULL;
}

/* PyUnicode_AsUTF8 is Limited API only from 3.10. */
static const char* t_utf8(PyObject* s, PyObject** keep) {
    *keep = PyUnicode_AsUTF8String(s);
    return *keep ? PyBytes_AsString(*keep) : NULL;
}

/* ======================================================================= *
 *  Object
 * ======================================================================= */

struct TrialsObject;

/* What a track's is_done trampoline gets as ctx: the owner and the index. */
typedef struct {
    struct TrialsObject* owner;
    int index;
} t_track_ctx;

typedef struct TrialsObject {
    PyObject_HEAD
    PyObject* tracks[YTR_MAX_TRACKS];   /* the caller's track objects     */
    PyObject* rng_obj;                    /* callable rng, or NULL          */
    PyObject* names;                      /* list of bytes kept for names   */
    uint64_t  rng_state;                  /* splitmix state for an int rng  */
    int       own_rng;                    /* rng is the binding's splitmix  */
    int*      cond_reps;
    int*      warmup;
    unsigned char* records;               /* record_size x YTR_MAX_TRIALS */
    size_t    record_size;
    PyObject* table_obj;                  /* the Table desc.table points into */
    int*      order_list;
    double*   weights;
    int*      group_list;
    uint64_t* rules_arena;                /* ytr_rules() output, until open */
    int64_t   jit_vals[YTR_MAX_JITTERS][YTR_MAX_JITTER_VALUES]; /* CHOICE values,
                                           * until open copies them    */
    int       busy;
    PyObject *cb_type, *cb_value, *cb_tb;
    t_track_ctx track_ctx[YTR_MAX_TRACKS];
    ytr_trials t;
} TrialsObject;

#define TO(self) ((TrialsObject*)(self))

static int t_traverse(PyObject* self, visitproc visit, void* arg) {
    TrialsObject* o = TO(self);
    int i;
    for (i = 0; i < YTR_MAX_TRACKS; i++) Py_VISIT(o->tracks[i]);
    Py_VISIT(o->rng_obj);
    Py_VISIT(o->names);
    Py_VISIT(o->table_obj);
    Py_VISIT(o->cb_type);
    Py_VISIT(o->cb_value);
    Py_VISIT(o->cb_tb);
    if (g_visit_type) Py_VISIT((PyObject*)Py_TYPE(self));
    return 0;
}

static int t_clear(PyObject* self) {
    TrialsObject* o = TO(self);
    int i;
    /* The handle points at the objects below; a cleared object must not be
     * driven again. */
    o->t.open = false;
    for (i = 0; i < YTR_MAX_TRACKS; i++) Py_CLEAR(o->tracks[i]);
    Py_CLEAR(o->rng_obj);
    Py_CLEAR(o->names);
    Py_CLEAR(o->table_obj);
    Py_CLEAR(o->cb_type);
    Py_CLEAR(o->cb_value);
    Py_CLEAR(o->cb_tb);
    return 0;
}

static void t_free_bufs(TrialsObject* o) {
    PyMem_Free(o->cond_reps); o->cond_reps = NULL;
    PyMem_Free(o->warmup);    o->warmup = NULL;
    PyMem_Free(o->records);   o->records = NULL;
    o->record_size = 0;
    PyMem_Free(o->order_list);  o->order_list = NULL;
    PyMem_Free(o->weights);     o->weights = NULL;
    PyMem_Free(o->group_list);  o->group_list = NULL;
    PyMem_Free(o->rules_arena); o->rules_arena = NULL;
}

static void Trials_dealloc(PyObject* self) {
    PyTypeObject* tp = Py_TYPE(self);
    freefunc tp_free;
    PyObject_GC_UnTrack(self);
    t_clear(self);
    t_free_bufs(TO(self));
    tp_free = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    tp_free(self);
    Py_DECREF(tp);
}

static void t_cb_store(TrialsObject* o) {
    if (o->cb_type || o->cb_value) { PyErr_Clear(); return; }
    PyErr_Fetch(&o->cb_type, &o->cb_value, &o->cb_tb);
}

static int t_enter(TrialsObject* o) {
    if (o->busy) {
        PyErr_SetString(TError, "this Trials is already in a call (another thread, or a "
                                "callback re-entering the Trials it runs under)");
        return -1;
    }
    o->busy = 1;
    return 0;
}

static int t_leave(TrialsObject* o) {
    o->busy = 0;
    if (o->cb_type || o->cb_value) {
        PyErr_Restore(o->cb_type, o->cb_value, o->cb_tb);
        o->cb_type = o->cb_value = o->cb_tb = NULL;
        return -1;
    }
    return 0;
}

/* --- trampolines -------------------------------------------------------- */

static double t_tramp_rng(void* ctx) {
    TrialsObject* o = (TrialsObject*)ctx;
    PyObject* r;
    double u;
    if (o->cb_type || o->cb_value) return 0.0;
    r = PyObject_CallObject(o->rng_obj, NULL);
    if (!r) { t_cb_store(o); return 0.0; }
    u = PyFloat_AsDouble(r);
    Py_DECREF(r);
    if (u == -1.0 && PyErr_Occurred()) { t_cb_store(o); return 0.0; }
    return u;
}

/* A track is an object with a callable is_done(), or with a `done`
 * attribute (ysp.stair.Staircase, ysp.quest.Quest, ysp.aep.GP), which may
 * itself be callable. */
static bool t_tramp_done(void* ctx) {
    t_track_ctx* c = (t_track_ctx*)ctx;
    TrialsObject* o = c->owner;
    PyObject* obj = o->tracks[c->index];
    PyObject* r = NULL;
    int v;
    if (o->cb_type || o->cb_value || !obj) return false;
    if (PyObject_HasAttrString(obj, "is_done")) {
        r = PyObject_CallMethod(obj, "is_done", NULL);
    } else {
        r = PyObject_GetAttrString(obj, "done");
        if (r && PyCallable_Check(r)) {
            PyObject* called = PyObject_CallObject(r, NULL);
            Py_DECREF(r);
            r = called;
        }
    }
    if (!r) { t_cb_store(o); return false; }
    v = PyObject_IsTrue(r);
    Py_DECREF(r);
    if (v < 0) { t_cb_store(o); return false; }
    return v != 0;
}

/* ======================================================================= *
 *  Table (ysp/table.h)
 * ======================================================================= */

typedef struct TableObject {
    PyObject_HEAD
    uint64_t*   mem;        /* the block, 8-byte aligned, owned */
    ytb_table tab;
} TableObject;

#define TB(self) ((TableObject*)(self))

static void Table_dealloc(PyObject* self) {
    PyTypeObject* tp = Py_TYPE(self);
    freefunc tp_free = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    PyMem_Free(TB(self)->mem);
    tp_free(self);
    Py_DECREF(tp);
}

/* Keep exactly the block: copy it out of the parse arena and view it. */
static int tb_adopt(TableObject* o, const void* block, size_t size) {
    uint64_t* mem = (uint64_t*)PyMem_Malloc((size + 7) & ~(size_t)7);
    ytb_table v;
    if (!mem) { PyErr_NoMemory(); return -1; }
    memcpy(mem, block, size);
    if (!ytb_open_view(&v, mem, size)) {
        PyErr_SetString(TArgumentError, ytb_error(&v));
        PyMem_Free(mem);
        return -1;
    }
    PyMem_Free(o->mem);
    o->mem = mem;
    o->tab = v;
    return 0;
}

static int t_type_name(PyObject* v, ytb_type* out) {
    PyObject* keep = NULL;
    const char* s;
    if (PyLong_Check(v)) { *out = (ytb_type)PyLong_AsLong(v); return 0; }
    s = PyUnicode_Check(v) ? t_utf8(v, &keep) : NULL;
    if (!s) { PyErr_SetString(PyExc_TypeError, "a column type is 'integer', 'number', 'string' or 'auto'"); return -1; }
    if (strcmp(s, "integer") == 0) *out = YTB_INTEGER;
    else if (strcmp(s, "number") == 0) *out = YTB_NUMBER;
    else if (strcmp(s, "string") == 0) *out = YTB_STRING;
    else if (strcmp(s, "auto") == 0) *out = YTB_AUTO;
    else { Py_XDECREF(keep); PyErr_Format(TArgumentError, "unknown column type '%s'", s); return -1; }
    Py_XDECREF(keep);
    return 0;
}

static int Table_init(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "csv", "types", "delimiter", "allow_empty", NULL };
    TableObject* o = TB(self);
    PyObject *csv, *types = Py_None, *bytes = NULL, *keys = NULL;
    const char* delim = ",";
    int allow_empty = 0, ok;
    ytb_csv_desc d;
    ytb_table t;
    uint64_t* arena;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|$Osp", kw, &csv, &types, &delim, &allow_empty))
        return -1;
    memset(&d, 0, sizeof(d));
    if (PyUnicode_Check(csv)) bytes = PyUnicode_AsUTF8String(csv);
    else bytes = PyBytes_FromObject(csv);
    if (!bytes) return -1;
    if (strlen(delim) != 1) { Py_DECREF(bytes); PyErr_SetString(TArgumentError, "delimiter must be one character"); return -1; }
    d.delimiter = delim[0];
    d.allow_empty = allow_empty != 0;
    if (types != Py_None) {
        Py_ssize_t i, n;
        if (!PyDict_Check(types)) { Py_DECREF(bytes); PyErr_SetString(PyExc_TypeError, "types must be a dict of column name to type"); return -1; }
        keys = PyDict_Keys(types);
        if (!keys) { Py_DECREF(bytes); return -1; }
        n = PyList_Size(keys);
        if (n > YTB_MAX_COLUMNS) { Py_DECREF(keys); Py_DECREF(bytes); PyErr_SetString(TArgumentError, "too many types"); return -1; }
        for (i = 0; i < n; i++) {
            PyObject* k = PyList_GetItem(keys, i);
            PyObject* kb = NULL;
            if (!PyUnicode_Check(k) || !(kb = PyUnicode_AsUTF8String(k)) ||
                t_type_name(PyDict_GetItem(types, k), &d.types[i].type) < 0) {
                Py_XDECREF(kb);
                Py_DECREF(keys);
                Py_DECREF(bytes);
                if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "types keys must be column names");
                return -1;
            }
            /* The name must live through the parse: keep the bytes in the
             * keys list (which this function holds) by replacing the key. */
            PyList_SetItem(keys, i, kb);
            d.types[i].name = PyBytes_AsString(kb);
        }
        d.n_types = (int)n;
    }
    d.text = PyBytes_AsString(bytes);
    d.len = (size_t)PyBytes_Size(bytes);
    /* A sizing pass, then the parse into an arena of that size. */
    ytb_open_csv(&t, &d);
    if (t.need == 0) {
        Py_XDECREF(keys);
        Py_DECREF(bytes);
        PyErr_SetString(TArgumentError, ytb_error(&t));
        return -1;
    }
    arena = (uint64_t*)PyMem_Malloc((t.need + 7) & ~(size_t)7);
    if (!arena) { Py_XDECREF(keys); Py_DECREF(bytes); PyErr_NoMemory(); return -1; }
    d.arena = arena;
    d.arena_size = t.need;
    Py_BEGIN_ALLOW_THREADS
    ok = ytb_open_csv(&t, &d);
    Py_END_ALLOW_THREADS
    Py_XDECREF(keys);
    Py_DECREF(bytes);
    if (!ok) {
        PyErr_SetString(TArgumentError, ytb_error(&t));
        PyMem_Free(arena);
        return -1;
    }
    ok = tb_adopt(o, t.base, t.size);
    PyMem_Free(arena);
    return ok;
}

static PyObject* Table_from_bytes(PyObject* cls, PyObject* arg) {
    /* Not the buffer protocol: it is Limited API only from 3.11. */
    PyObject *self, *b = PyBytes_FromObject(arg);
    if (!b) return NULL;
    self = PyObject_CallMethod(cls, "__new__", "O", cls);
    if (!self) { Py_DECREF(b); return NULL; }
    if (tb_adopt(TB(self), PyBytes_AsString(b), (size_t)PyBytes_Size(b)) < 0) {
        Py_DECREF(b);
        Py_DECREF(self);
        return NULL;
    }
    Py_DECREF(b);
    return self;
}

static int tb_ready(TableObject* o) {
    if (!o->tab.base) { PyErr_SetString(TClosed, "the Table holds no table"); return -1; }
    return 0;
}

/* A column argument: index or name. */
static int tb_col(TableObject* o, PyObject* c) {
    if (PyUnicode_Check(c)) {
        PyObject* keep;
        const char* s = t_utf8(c, &keep);
        int i;
        if (!s) return -1;
        i = ytb_col(&o->tab, s);
        if (i < 0) PyErr_Format(PyExc_KeyError, "no column '%s'", s);
        Py_DECREF(keep);
        return i;
    } else {
        long i = PyLong_AsLong(c);
        if (i == -1 && PyErr_Occurred()) return -1;
        if (i < 0 || i >= o->tab.n_cols) { PyErr_Format(PyExc_IndexError, "column %ld out of range", i); return -1; }
        return (int)i;
    }
}

static PyObject* tb_cell(TableObject* o, int r, int c) {
    switch (ytb_col_type(&o->tab, c)) {
    case YTB_INTEGER: return PyLong_FromLong((long)ytb_int(&o->tab, r, c));
    case YTB_NUMBER: {
        double v = ytb_num(&o->tab, r, c);
        if (v != v) Py_RETURN_NONE;   /* an empty cell under allow_empty */
        return PyFloat_FromDouble(v);
    }
    default: return PyUnicode_FromString(ytb_text(&o->tab, r, c));
    }
}

static PyObject* Table_value(PyObject* self, PyObject* args) {
    TableObject* o = TB(self);
    int r, c;
    PyObject* col;
    if (!PyArg_ParseTuple(args, "iO", &r, &col)) return NULL;
    if (tb_ready(o) < 0 || (c = tb_col(o, col)) < 0) return NULL;
    if (r < 0 || r >= o->tab.n_rows) { PyErr_Format(PyExc_IndexError, "row %d out of range", r); return NULL; }
    return tb_cell(o, r, c);
}

static PyObject* tb_row(TableObject* o, int r) {
    PyObject* d = PyDict_New();
    int c;
    if (!d) return NULL;
    for (c = 0; c < o->tab.n_cols; c++) {
        PyObject* v = tb_cell(o, r, c);
        if (!v || PyDict_SetItemString(d, ytb_col_name(&o->tab, c), v) < 0) {
            Py_XDECREF(v);
            Py_DECREF(d);
            return NULL;
        }
        Py_DECREF(v);
    }
    return d;
}

static PyObject* Table_row(PyObject* self, PyObject* arg) {
    TableObject* o = TB(self);
    long r = PyLong_AsLong(arg);
    if (r == -1 && PyErr_Occurred()) return NULL;
    if (tb_ready(o) < 0) return NULL;
    if (r < 0 || r >= o->tab.n_rows) { PyErr_Format(PyExc_IndexError, "row %ld out of range", r); return NULL; }
    return tb_row(o, (int)r);
}

static PyObject* Table_levels(PyObject* self, PyObject* arg) {
    TableObject* o = TB(self);
    int c, l, n;
    PyObject* list;
    if (tb_ready(o) < 0 || (c = tb_col(o, arg)) < 0) return NULL;
    n = ytb_n_levels(&o->tab, c);
    list = PyList_New(n);
    if (!list) return NULL;
    for (l = 0; l < n; l++) PyList_SetItem(list, l, PyUnicode_FromString(ytb_level_text(&o->tab, c, l)));
    return list;
}

static PyObject* Table_level(PyObject* self, PyObject* args) {
    TableObject* o = TB(self);
    int r, c;
    PyObject* col;
    if (!PyArg_ParseTuple(args, "iO", &r, &col)) return NULL;
    if (tb_ready(o) < 0 || (c = tb_col(o, col)) < 0) return NULL;
    if (r < 0 || r >= o->tab.n_rows) { PyErr_Format(PyExc_IndexError, "row %d out of range", r); return NULL; }
    return PyLong_FromLong(ytb_level(&o->tab, r, c));
}

static PyObject* Table_find(PyObject* self, PyObject* args) {
    TableObject* o = TB(self);
    PyObject *col, *val, *s, *keep;
    const char* txt;
    int c, l;
    if (!PyArg_ParseTuple(args, "OO", &col, &val)) return NULL;
    if (tb_ready(o) < 0 || (c = tb_col(o, col)) < 0) return NULL;
    s = PyUnicode_Check(val) ? (Py_INCREF(val), val) : PyObject_Str(val);
    if (!s) return NULL;
    txt = t_utf8(s, &keep);
    Py_DECREF(s);
    if (!txt) return NULL;
    l = ytb_find(&o->tab, c, txt);
    Py_DECREF(keep);
    return PyLong_FromLong(l);
}

static PyObject* Table_col(PyObject* self, PyObject* arg) {
    TableObject* o = TB(self);
    int c;
    if (tb_ready(o) < 0 || (c = tb_col(o, arg)) < 0) return NULL;
    return PyLong_FromLong(c);
}

static PyObject* Table_to_bytes(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    TableObject* o = TB(self);
    if (tb_ready(o) < 0) return NULL;
    return PyBytes_FromStringAndSize((const char*)o->tab.base, (Py_ssize_t)o->tab.size);
}

static Py_ssize_t Table_len(PyObject* self) { return TB(self)->tab.n_rows; }

static PyObject* Table_get_columns(PyObject* self, void* Py_UNUSED(c)) {
    TableObject* o = TB(self);
    PyObject* list = PyList_New(o->tab.n_cols);
    int c;
    if (!list) return NULL;
    for (c = 0; c < o->tab.n_cols; c++) PyList_SetItem(list, c, PyUnicode_FromString(ytb_col_name(&o->tab, c)));
    return list;
}

static PyObject* Table_get_types(PyObject* self, void* Py_UNUSED(c)) {
    TableObject* o = TB(self);
    PyObject* list = PyList_New(o->tab.n_cols);
    int c;
    if (!list) return NULL;
    for (c = 0; c < o->tab.n_cols; c++)
        PyList_SetItem(list, c, PyUnicode_FromString(ytb_type_name(ytb_col_type(&o->tab, c))));
    return list;
}

static PyObject* Table_get_n_rows(PyObject* self, void* Py_UNUSED(c)) { return PyLong_FromLong(TB(self)->tab.n_rows); }
static PyObject* Table_get_n_skipped(PyObject* self, void* Py_UNUSED(c)) { return PyLong_FromLong(TB(self)->tab.n_skipped); }
static PyObject* Table_get_hash(PyObject* self, void* Py_UNUSED(c)) {
    return PyLong_FromUnsignedLongLong((unsigned long long)ytb_hash(&TB(self)->tab));
}

static PyGetSetDef Table_getset[] = {
    { "columns", Table_get_columns, NULL, "Column names.", NULL },
    { "types", Table_get_types, NULL, "Column types: 'integer', 'number' or 'string'.", NULL },
    { "n_rows", Table_get_n_rows, NULL, "Data rows.", NULL },
    { "n_skipped", Table_get_n_skipped, NULL, "CSV rows with every field empty, skipped.", NULL },
    { "hash", Table_get_hash, NULL, "The block's content hash.", NULL },
    { NULL }
};

static PyMethodDef Table_methods[] = {
    { "value", Table_value, METH_VARARGS, "value(row, col) -> int | float | str | None" },
    { "row", Table_row, METH_O, "row(i) -> dict: column name to value." },
    { "levels", Table_levels, METH_O, "levels(col) -> list[str]: the level texts in order." },
    { "level", Table_level, METH_VARARGS, "level(row, col) -> int" },
    { "find", Table_find, METH_VARARGS, "find(col, value) -> int: the level of a value, or -1." },
    { "col", Table_col, METH_O, "col(name) -> int" },
    { "to_bytes", Table_to_bytes, METH_NOARGS, "to_bytes() -> bytes: the block, as a pack stores it." },
    { "from_bytes", Table_from_bytes, METH_O | METH_CLASS,
      "Table.from_bytes(b) -> Table: check a block and keep a copy." },
    { NULL }
};

static PyType_Slot Table_slots[] = {
    { Py_tp_doc, (void*)
      "Table(csv, *, types=None, delimiter=',', allow_empty=False)\n\n"
      "A ysp/table.h table parsed from CSV text (str or bytes). types maps column "
      "names to 'integer', 'number' or 'string'; other columns are inferred. "
      "Raises ArgumentError with the line, row and column of a fault." },
    { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_init, (void*)Table_init },
    { Py_tp_dealloc, (void*)Table_dealloc },
    { Py_tp_methods, Table_methods },
    { Py_tp_getset, Table_getset },
    { Py_sq_length, (void*)Table_len },
    { 0, NULL }
};

static PyType_Spec Table_spec = {
    .name = "ysp.trials.Table",
    .basicsize = sizeof(TableObject),
    .itemsize = 0,
    .flags = Py_TPFLAGS_DEFAULT,
    .slots = Table_slots,
};

/* ======================================================================= *
 *  Desc parsing
 * ======================================================================= */

static int t_int_list(PyObject* seq, int** out, int* n_out, const char* what) {
    Py_ssize_t n = PySequence_Size(seq), i;
    int* v;
    if (n < 0) {
        PyErr_Clear();
        PyErr_Format(PyExc_TypeError, "%s must be a sequence of ints", what);
        return -1;
    }
    v = (int*)PyMem_Malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!v) { PyErr_NoMemory(); return -1; }
    for (i = 0; i < n; i++) {
        PyObject* it = PySequence_GetItem(seq, i);
        long x;
        if (!it) { PyMem_Free(v); return -1; }
        x = PyLong_AsLong(it);
        Py_DECREF(it);
        if (x == -1 && PyErr_Occurred()) { PyMem_Free(v); return -1; }
        v[i] = (int)x;
    }
    *out = v;
    *n_out = (int)n;
    return 0;
}

/* Index of a factor named `name` in the desc (a table column, or a named
 * factor), or -1. */
static int t_factor_by_name(const ytr_desc* d, const char* name) {
    int f;
    if (d->table) return ytb_col(d->table, name);
    for (f = 0; f < d->n_factors; f++)
        if (d->factors[f].name && strcmp(d->factors[f].name, name) == 0) return f;
    return -1;
}

/* A level: an int, or with a table a value (str, int or float) looked up
 * in the factor's column. */
static int t_level(const ytr_desc* d, int factor, PyObject* v, int* out, const char* what) {
    if (!v) return 0;
    if (PyUnicode_Check(v) || (d->table && factor >= 0 && !PyLong_Check(v))) {
        PyObject *s, *keep;
        const char* txt;
        if (!d->table || factor < 0) {
            PyErr_Format(TArgumentError, "%s: a level by name needs a table column", what);
            return -1;
        }
        s = PyUnicode_Check(v) ? (Py_INCREF(v), v) : PyObject_Str(v);
        if (!s) return -1;
        txt = t_utf8(s, &keep);
        Py_DECREF(s);
        if (!txt) return -1;
        *out = ytb_find(d->table, factor, txt);
        if (*out < 0) PyErr_Format(TArgumentError, "%s: column '%s' has no level '%s'", what,
                                   ytb_col_name(d->table, factor), txt);
        Py_DECREF(keep);
        return *out < 0 ? -1 : 0;
    } else {
        long x = PyLong_AsLong(v);
        if (x == -1 && PyErr_Occurred()) return -1;
        /* An int level of a table column: a level number, as in C. */
        *out = (int)x;
        return 0;
    }
}

static int t_dict_int(PyObject* dict, const char* key, int* out, int dflt) {
    PyObject* v = PyDict_GetItemString(dict, key);
    long x;
    if (!v) { *out = dflt; return 0; }
    x = PyLong_AsLong(v);
    if (x == -1 && PyErr_Occurred()) return -1;
    *out = (int)x;
    return 0;
}

/* A constraint is the dict a helper returns: rule, factor (index or factor
 * name), level, level2, n, window. */
static int t_parse_constraint(PyObject* c, const ytr_desc* d, ytr_constraint* out, int ci) {
    PyObject* f;
    int rule;
    if (!PyDict_Check(c)) {
        PyErr_Format(PyExc_TypeError, "constraints[%d] must come from max_run(), "
                     "max_in_window(), min_gap(), no_transition(), first_not(), "
                     "followed_by(), preceded_by(), chunk() or balance()", ci);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (t_dict_int(c, "rule", &rule, -1) < 0) return -1;
    out->rule = (ytr_rule)rule;
    f = PyDict_GetItemString(c, "factor");
    if (f && PyUnicode_Check(f)) {
        PyObject* keep;
        const char* name = t_utf8(f, &keep);
        if (!name) return -1;
        out->factor = t_factor_by_name(d, name);
        if (out->factor < 0) {
            PyErr_Format(TArgumentError, "constraints[%d] names factor '%s', which is not in "
                         "factors or the table", ci, name);
            Py_DECREF(keep);
            return -1;
        }
        Py_DECREF(keep);
    } else if (t_dict_int(c, "factor", &out->factor, YTR_CONDITION) < 0) {
        return -1;
    }
    {
        char what[32];
        PyOS_snprintf(what, sizeof(what), "constraints[%d]", ci);
        if (t_level(d, out->factor, PyDict_GetItemString(c, "level"), &out->level, what) < 0 ||
            t_level(d, out->factor, PyDict_GetItemString(c, "level2"), &out->level2, what) < 0)
            return -1;
    }
    if (t_dict_int(c, "n", &out->n, 0) < 0 ||
        t_dict_int(c, "window", &out->window, 0) < 0)
        return -1;
    return 0;
}

/* A number of seconds, or a column name (kept as bytes in `names`). The
 * header takes nanoseconds since v0.3.0; ytr__s_ns() converts the way
 * v0.2.1 read seconds, so a draw is the nanosecond it was, and turns a
 * value out of [0, 1e6] s into -1 ns, which the header refuses by name. */
static int t_jit_num(PyObject* dict, const char* key, int64_t* out, const char** col, PyObject* names) {
    double x;
    PyObject* v = PyDict_GetItemString(dict, key);
    if (!v || v == Py_None) return 0;
    if (PyUnicode_Check(v)) {
        PyObject* b = PyUnicode_AsUTF8String(v);
        if (!b) return -1;
        if (PyList_Append(names, b) < 0) { Py_DECREF(b); return -1; }
        *col = PyBytes_AsString(b);
        Py_DECREF(b);   /* the list holds it */
        return 0;
    }
    x = PyFloat_AsDouble(v);
    if (x == -1.0 && PyErr_Occurred()) return -1;
    *out = ytr__s_ns(x);
    return 0;
}

/* {"name", "dist", "lo", "hi", "scale", "values", "rate"} from uniform(),
 * choice() or exponential() into j. Strings in `names`, values in vals. */
static int t_jitter_desc(PyObject* dict, ytr_jitter_desc* j, int64_t* vals, PyObject* names) {
    static const char* const dists[] = { "uniform", "choice", "exponential" };
    PyObject *v, *b;
    Py_ssize_t n, i;
    int k;
    memset(j, 0, sizeof(*j));
    if (!PyDict_Check(dict)) { PyErr_SetString(PyExc_TypeError, "a jitter is a dict from uniform(), choice() or exponential()"); return -1; }
    v = PyDict_GetItemString(dict, "name");
    if (v && v != Py_None) {
        if (!PyUnicode_Check(v)) { PyErr_SetString(PyExc_TypeError, "a jitter's name must be a str"); return -1; }
        b = PyUnicode_AsUTF8String(v);
        if (!b) return -1;
        if (PyList_Append(names, b) < 0) { Py_DECREF(b); return -1; }
        j->name = PyBytes_AsString(b);
        Py_DECREF(b);
    }
    v = PyDict_GetItemString(dict, "dist");
    if (!v || !PyUnicode_Check(v)) { PyErr_SetString(TArgumentError, "a jitter needs dist 'uniform', 'choice' or 'exponential'"); return -1; }
    {
        PyObject* keep;
        const char* nm = t_utf8(v, &keep);
        if (!nm) return -1;
        for (k = 0; k < 3 && strcmp(nm, dists[k]) != 0; k++) {}
        Py_DECREF(keep);
        if (k == 3) { PyErr_SetString(TArgumentError, "dist is 'uniform', 'choice' or 'exponential'"); return -1; }
        j->dist = (ytr_jitter_dist)k;
    }
    if (t_jit_num(dict, "lo", &j->lo_ns, &j->lo_column, names) < 0) return -1;
    if (t_jit_num(dict, "hi", &j->hi_ns, &j->hi_column, names) < 0) return -1;
    if (t_jit_num(dict, "scale", &j->scale_ns, &j->scale_column, names) < 0) return -1;
    v = PyDict_GetItemString(dict, "values");
    if (v && v != Py_None) {
        n = PySequence_Size(v);
        if (n < 0) { PyErr_Clear(); PyErr_SetString(PyExc_TypeError, "values must be a sequence of numbers"); return -1; }
        if (n > YTR_MAX_JITTER_VALUES) {
            PyErr_Format(TArgumentError, "%zd values; the maximum is %d", n, YTR_MAX_JITTER_VALUES);
            return -1;
        }
        for (i = 0; i < n; i++) {
            PyObject* it = PySequence_GetItem(v, i);
            double x;
            if (!it) return -1;
            x = PyFloat_AsDouble(it);
            Py_DECREF(it);
            if (x == -1.0 && PyErr_Occurred()) return -1;
            vals[i] = ytr__s_ns(x);
        }
        j->values_ns = vals;
        j->n_values = (int)n;
    }
    v = PyDict_GetItemString(dict, "rate");
    if (v && v != Py_None) {
        long num, den = 1;
        if (PyTuple_Check(v) && PyTuple_Size(v) == 2) {
            num = PyLong_AsLong(PyTuple_GetItem(v, 0));
            den = PyLong_AsLong(PyTuple_GetItem(v, 1));
        } else {
            num = PyLong_AsLong(v);
        }
        if (PyErr_Occurred()) { PyErr_Clear(); PyErr_SetString(PyExc_TypeError, "rate is an int or (num, den)"); return -1; }
        if (num < 1 || den < 1 || num > 0x7FFFFFFF || den > 0x7FFFFFFF) {
            PyErr_SetString(TArgumentError, "rate needs positive num and den");
            return -1;
        }
        j->rate_num = (int)num;
        j->rate_den = (int)den;
    }
    return 0;
}

/* .s is the correctly rounded ns / 1e9, as the header's own .s was before
 * v0.3.0 (the divisor is volatile so no reciprocal math replaces the
 * division), and NaN for an invalid desc. */
static PyObject* t_jitter_value(ytr_jitter_value v) {
    static volatile double billion = 1e9;
    double s = v.ns < 0 ? Py_NAN : (double)v.ns / billion;
    return PyObject_CallFunction(TJitterType, "dLL", s, (long long)v.ns, (long long)v.frames);
}

static int t_build(TrialsObject* o, PyObject* args, PyObject* kwds, ytr_desc* d) {
    static char* kw[] = { "n_conditions", "factors", "reps", "cond_reps", "order",
                          "constraints", "max_swaps", "tracks", "interleave",
                          "track_rate", "block_size", "constraints_span_blocks",
                          "n_practice", "n_warmup", "warmup_conditions", "requeue_gap",
                          "rng", "record_size", "table", "order_list", "draws", "weights",
                          "subset", "groups", "rules", "participant", "jitters", NULL };
    PyObject *factors = Py_None, *cond_reps = Py_None, *constraints = Py_None,
             *tracks = Py_None, *warmup = Py_None, *rng = Py_None, *table = Py_None,
             *order_list = Py_None, *weights = Py_None, *groups = Py_None, *rules = Py_None,
             *jitters = Py_None;
    int order = 0, interleave = 0, span = 0, participant = 0;
    Py_ssize_t record_size = 0, n, i;

    memset(d, 0, sizeof(*d));
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$iOiOiOiOidipiiOiOnOOiOiOOiO", kw,
                                     &d->n_conditions, &factors, &d->reps, &cond_reps,
                                     &order, &constraints, &d->max_swaps, &tracks,
                                     &interleave, &d->track_rate, &d->block_size, &span,
                                     &d->n_practice, &d->n_warmup, &warmup,
                                     &d->requeue_gap, &rng, &record_size, &table, &order_list,
                                     &d->draws, &weights, &d->subset, &groups, &rules,
                                     &participant, &jitters))
        return -1;
    if (table != Py_None) {
        if (!PyObject_TypeCheck(table, (PyTypeObject*)TTableType)) {
            PyErr_SetString(PyExc_TypeError, "table must be a ysp.trials.Table");
            return -1;
        }
        if (tb_ready(TB(table)) < 0) return -1;
        Py_INCREF(table);
        o->table_obj = table;
        d->table = &TB(table)->tab;
    }
    d->order = (ytr_order)order;
    d->interleave = (ytr_interleave)interleave;
    d->constraints_span_blocks = span ? true : false;

    /* Factors: (name, n_levels) pairs. The names are kept as bytes on the
     * object because the header reads them after open. */
    o->names = PyList_New(0);
    if (!o->names) return -1;
    if (factors != Py_None) {
        n = PySequence_Size(factors);
        if (n < 0) { PyErr_Clear(); PyErr_SetString(PyExc_TypeError, "factors must be a list of (name, n_levels)"); return -1; }
        if (n > YTR_MAX_FACTORS) {
            PyErr_Format(TArgumentError, "%zd factors; the maximum is %d", n, YTR_MAX_FACTORS);
            return -1;
        }
        for (i = 0; i < n; i++) {
            PyObject* it = PySequence_GetItem(factors, i);
            PyObject *name_o, *nl_o;
            long nl;
            if (!it) return -1;
            if (PySequence_Size(it) != 2) {
                Py_DECREF(it);
                PyErr_Clear();
                PyErr_Format(PyExc_TypeError, "factors[%zd] must be (name, n_levels)", i);
                return -1;
            }
            name_o = PySequence_GetItem(it, 0);
            nl_o = PySequence_GetItem(it, 1);
            Py_DECREF(it);
            if (!name_o || !nl_o) { Py_XDECREF(name_o); Py_XDECREF(nl_o); return -1; }
            nl = PyLong_AsLong(nl_o);
            Py_DECREF(nl_o);
            if (nl == -1 && PyErr_Occurred()) { Py_DECREF(name_o); return -1; }
            d->factors[i].n_levels = (int)nl;
            if (name_o != Py_None) {
                PyObject* b;
                if (!PyUnicode_Check(name_o)) {
                    Py_DECREF(name_o);
                    PyErr_Format(PyExc_TypeError, "factors[%zd]: the name must be a str or None", i);
                    return -1;
                }
                b = PyUnicode_AsUTF8String(name_o);
                Py_DECREF(name_o);
                if (!b) return -1;
                if (PyList_Append(o->names, b) < 0) { Py_DECREF(b); return -1; }
                d->factors[i].name = PyBytes_AsString(b);
                Py_DECREF(b);   /* the list holds it */
            } else {
                Py_DECREF(name_o);
            }
        }
        d->n_factors = (int)n;
    }

    if (cond_reps != Py_None) {
        int nc;
        if (t_int_list(cond_reps, &o->cond_reps, &nc, "cond_reps") < 0) return -1;
        /* The header reads one entry per condition; a short list would be
         * read past its end, so the count is checked against the rows. */
        {
            int rows = d->table ? d->table->n_rows : d->n_conditions, f;
            if (rows == 0 && d->n_factors > 0) {
                rows = 1;
                for (f = 0; f < d->n_factors; f++) rows *= d->factors[f].n_levels > 0 ? d->factors[f].n_levels : 1;
            }
            if (nc != rows) {
                PyErr_Format(TArgumentError, "cond_reps has %d entries, the design has %d conditions",
                             nc, rows);
                return -1;
            }
        }
        d->cond_reps = o->cond_reps;
    }
    if (warmup != Py_None) {
        if (t_int_list(warmup, &o->warmup, &d->n_warmup_conditions, "warmup_conditions") < 0)
            return -1;
        d->warmup_conditions = o->warmup;
    }

    if (constraints != Py_None) {
        n = PySequence_Size(constraints);
        if (n < 0) { PyErr_Clear(); PyErr_SetString(PyExc_TypeError, "constraints must be a list"); return -1; }
        if (n > YTR_MAX_CONSTRAINTS) {
            PyErr_Format(TArgumentError, "%zd constraints; the maximum is %d", n, YTR_MAX_CONSTRAINTS);
            return -1;
        }
        for (i = 0; i < n; i++) {
            PyObject* it = PySequence_GetItem(constraints, i);
            int rc;
            if (!it) return -1;
            rc = t_parse_constraint(it, d, &d->constraints[i], (int)i);
            Py_DECREF(it);
            if (rc < 0) return -1;
        }
        d->n_constraints = (int)n;
    }

    if (order_list != Py_None) {
        if (t_int_list(order_list, &o->order_list, &d->n_order_list, "order_list") < 0) return -1;
        d->order_list = o->order_list;
    }
    if (weights != Py_None || groups != Py_None) {
        int rows = d->table ? d->table->n_rows : d->n_conditions, f;
        if (rows == 0 && d->n_factors > 0) {
            rows = 1;
            for (f = 0; f < d->n_factors; f++) rows *= d->factors[f].n_levels > 0 ? d->factors[f].n_levels : 1;
        }
        if (weights != Py_None) {
            n = PySequence_Size(weights);
            if (n < 0) { PyErr_Clear(); PyErr_SetString(PyExc_TypeError, "weights must be a sequence of numbers"); return -1; }
            if (n != rows) {
                PyErr_Format(TArgumentError, "weights has %zd entries, the design has %d conditions", n, rows);
                return -1;
            }
            o->weights = (double*)PyMem_Malloc((size_t)(n > 0 ? n : 1) * sizeof(double));
            if (!o->weights) { PyErr_NoMemory(); return -1; }
            for (i = 0; i < n; i++) {
                PyObject* it = PySequence_GetItem(weights, i);
                double w;
                if (!it) return -1;
                w = PyFloat_AsDouble(it);
                Py_DECREF(it);
                if (w == -1.0 && PyErr_Occurred()) return -1;
                o->weights[i] = w;
            }
            d->weights = o->weights;
        }
        if (groups != Py_None) {
            /* {"factor": name or index, "mode": "blocked" | "alternate",
             *  "order": "sequential" | "random" | "latin" | "balanced_latin"
             *  | "list", "participant": int, "list": [values]} */
            static const char* const modes[] = { "none", "blocked", "alternate" };
            static const char* const orders[] = { "sequential", "random", "latin", "balanced_latin", "list" };
            PyObject *v;
            int k;
            if (!PyDict_Check(groups)) { PyErr_SetString(PyExc_TypeError, "groups must be a dict (see groups())"); return -1; }
            v = PyDict_GetItemString(groups, "factor");
            if (!v) { PyErr_SetString(TArgumentError, "groups needs a factor"); return -1; }
            if (PyUnicode_Check(v)) {
                PyObject* keep;
                const char* nm = t_utf8(v, &keep);
                if (!nm) return -1;
                d->groups.factor = t_factor_by_name(d, nm);
                Py_DECREF(keep);
                if (d->groups.factor < 0) { PyErr_SetString(TArgumentError, "groups names an unknown factor"); return -1; }
            } else if (t_dict_int(groups, "factor", &d->groups.factor, 0) < 0) {
                return -1;
            }
            d->groups.mode = YTR_GROUPS_BLOCKED;
            v = PyDict_GetItemString(groups, "mode");
            if (v) {
                if (PyLong_Check(v)) d->groups.mode = (ytr_group_mode)PyLong_AsLong(v);
                else {
                    PyObject* keep;
                    const char* nm = t_utf8(v, &keep);
                    if (!nm) return -1;
                    for (k = 0; k < 3 && strcmp(nm, modes[k]) != 0; k++) {}
                    Py_DECREF(keep);
                    if (k == 3) { PyErr_SetString(TArgumentError, "groups mode is 'blocked' or 'alternate'"); return -1; }
                    d->groups.mode = (ytr_group_mode)k;
                }
            }
            v = PyDict_GetItemString(groups, "order");
            if (v) {
                if (PyLong_Check(v)) d->groups.order = (ytr_group_order)PyLong_AsLong(v);
                else {
                    PyObject* keep;
                    const char* nm = t_utf8(v, &keep);
                    if (!nm) return -1;
                    for (k = 0; k < 5 && strcmp(nm, orders[k]) != 0; k++) {}
                    Py_DECREF(keep);
                    if (k == 5) { PyErr_SetString(TArgumentError, "unknown groups order"); return -1; }
                    d->groups.order = (ytr_group_order)k;
                }
            }
            if (t_dict_int(groups, "participant", &d->groups.participant, participant) < 0) return -1;
            v = PyDict_GetItemString(groups, "list");
            if (v && v != Py_None) {
                n = PySequence_Size(v);
                if (n < 0) { PyErr_Clear(); PyErr_SetString(PyExc_TypeError, "groups list must be a sequence"); return -1; }
                o->group_list = (int*)PyMem_Malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
                if (!o->group_list) { PyErr_NoMemory(); return -1; }
                for (i = 0; i < n; i++) {
                    PyObject* it = PySequence_GetItem(v, i);
                    int rc;
                    if (!it) return -1;
                    rc = t_level(d, d->groups.factor, it, &o->group_list[i], "groups list");
                    Py_DECREF(it);
                    if (rc < 0) return -1;
                }
                d->groups.list = o->group_list;
                d->groups.n_list = (int)n;
            }
        }
    }

    if (jitters != Py_None) {
        n = PySequence_Size(jitters);
        if (n < 0) { PyErr_Clear(); PyErr_SetString(PyExc_TypeError, "jitters must be a list"); return -1; }
        if (n > YTR_MAX_JITTERS) {
            PyErr_Format(TArgumentError, "%zd jitters; the maximum is %d", n, YTR_MAX_JITTERS);
            return -1;
        }
        for (i = 0; i < n; i++) {
            PyObject* it = PySequence_GetItem(jitters, i);
            int rc;
            if (!it) return -1;
            rc = t_jitter_desc(it, &d->jitters[i], o->jit_vals[i], o->names);
            Py_DECREF(it);
            if (rc < 0) return -1;
        }
        d->n_jitters = (int)n;
    }

    /* Tracks: an object, or (object, weight). */
    if (tracks != Py_None) {
        n = PySequence_Size(tracks);
        if (n < 0) { PyErr_Clear(); PyErr_SetString(PyExc_TypeError, "tracks must be a list"); return -1; }
        if (n > YTR_MAX_TRACKS) {
            PyErr_Format(TArgumentError, "%zd tracks; the maximum is %d", n, YTR_MAX_TRACKS);
            return -1;
        }
        for (i = 0; i < n; i++) {
            PyObject* it = PySequence_GetItem(tracks, i);
            PyObject* obj = it;
            double w = 0.0;
            if (!it) return -1;
            if (PyTuple_Check(it) && PyTuple_Size(it) == 2) {
                obj = PyTuple_GetItem(it, 0);
                w = PyFloat_AsDouble(PyTuple_GetItem(it, 1));
                if (w == -1.0 && PyErr_Occurred()) { Py_DECREF(it); return -1; }
            }
            if (!PyObject_HasAttrString(obj, "is_done") && !PyObject_HasAttrString(obj, "done")) {
                Py_DECREF(it);
                PyErr_Format(PyExc_TypeError, "tracks[%zd] has neither is_done() nor done", i);
                return -1;
            }
            Py_INCREF(obj);
            o->tracks[i] = obj;
            Py_DECREF(it);
            o->track_ctx[i].owner = o;
            o->track_ctx[i].index = (int)i;
            d->tracks[i] = ytr_track(&o->track_ctx[i], t_tramp_done);
            d->tracks[i].weight = w;
        }
        d->n_tracks = (int)n;
    }

    /* rng: a callable, or an int that seeds the binding's splitmix state. */
    if (rng != Py_None) {
        if (PyLong_Check(rng)) {
            unsigned long long s = PyLong_AsUnsignedLongLongMask(rng);
            if (s == (unsigned long long)-1 && PyErr_Occurred()) return -1;
            o->rng_state = (uint64_t)s;
            o->own_rng = 1;
            d->rng = ytr_splitmix;
            d->rng_ctx = &o->rng_state;
        } else if (PyCallable_Check(rng)) {
            Py_INCREF(rng);
            o->rng_obj = rng;
            d->rng = t_tramp_rng;
            d->rng_ctx = o;
        } else {
            PyErr_SetString(PyExc_TypeError, "rng must be a callable or an int seed");
            return -1;
        }
    }

    if (rules != Py_None) {
        /* Rules text, applied over the keywords above (ytr_rules). The
         * arena holds what the rules make until open() has read it. */
        ytr_rules_desc rd;
        PyObject *keep;
        char err[512];
        int line, rows = d->table ? d->table->n_rows : YTR_MAX_CONDITIONS;
        size_t sz = (size_t)4 * YTR_MAX_TRIALS + (size_t)32 * (size_t)(rows > 0 ? rows : 1) + 1024 +
                    (size_t)320 * YTR_MAX_JITTERS;
        const char* txt;
        if (!PyUnicode_Check(rules)) { PyErr_SetString(PyExc_TypeError, "rules must be a str"); return -1; }
        txt = t_utf8(rules, &keep);
        if (!txt) return -1;
        o->rules_arena = (uint64_t*)PyMem_Malloc(sz);
        if (!o->rules_arena) { Py_DECREF(keep); PyErr_NoMemory(); return -1; }
        memset(&rd, 0, sizeof(rd));
        rd.text = txt;
        rd.len = strlen(txt);
        rd.table = d->table;
        rd.participant = participant;
        rd.arena = o->rules_arena;
        rd.arena_size = sz;
        line = ytr_rules(d, &rd, err, sizeof(err));
        Py_DECREF(keep);
        if (line != 0) { PyErr_SetString(TArgumentError, err); return -1; }
    }

    if (record_size < 0) { PyErr_SetString(TArgumentError, "record_size must be >= 0"); return -1; }
    if (record_size > 0) {
        if ((size_t)record_size > ((size_t)-1) / YTR_MAX_TRIALS / 2) {
            PyErr_SetString(TArgumentError, "record_size is too large");
            return -1;
        }
        /* Not PyMem_Calloc: it is in the stable ABI from 3.7, but 3.8 and
         * 3.9 declare it only in cpython/pymem.h, which Py_LIMITED_API
         * leaves out; PyMem_RawCalloc is Limited API only from 3.13. */
        o->records = (unsigned char*)PyMem_Malloc((size_t)YTR_MAX_TRIALS * (size_t)record_size);
        if (!o->records) { PyErr_NoMemory(); return -1; }
        memset(o->records, 0, (size_t)YTR_MAX_TRIALS * (size_t)record_size);
        o->record_size = (size_t)record_size;
        d->records = o->records;
        d->record_size = (size_t)record_size;
    }
    return 0;
}

/* Reset the Python-side state before a (re)build. */
static void t_reset(TrialsObject* o) {
    t_clear((PyObject*)o);
    t_free_bufs(o);
    o->own_rng = 0;
    o->rng_state = 0;
    memset(o->track_ctx, 0, sizeof(o->track_ctx));
}

static int Trials_init(PyObject* self, PyObject* args, PyObject* kwds) {
    TrialsObject* o = TO(self);
    ytr_desc d;
    bool ok;
    if (t_enter(o) < 0) return -1;
    o->busy = 0;
    t_reset(o);
    if (t_build(o, args, kwds, &d) < 0) { t_reset(o); return -1; }
    o->busy = 1;
    if (o->rng_obj) {
        ok = ytr_open(&o->t, &d);
    } else {
        /* open() calls no track, and the rng is C: nothing needs the GIL. */
        Py_BEGIN_ALLOW_THREADS
        ok = ytr_open(&o->t, &d);
        Py_END_ALLOW_THREADS
    }
    PyMem_Free(o->rules_arena);
    o->rules_arena = NULL;
    if (t_leave(o) < 0) { o->t.open = false; t_reset(o); return -1; }
    if (!ok) {
        /* An unmet constraint is a property of the design, not a bad
         * argument: it is Error, as the header's message says. */
        const char* msg = ytr_error(&o->t);
        PyErr_SetString(strstr(msg, "is still broken") ? TError : TArgumentError, msg);
        t_reset(o);
        return -1;
    }
    return 0;
}

/* ======================================================================= *
 *  Methods
 * ======================================================================= */

static int t_open_enter(TrialsObject* o) {
    if (t_enter(o) < 0) return -1;
    if (!ytr_is_open(&o->t)) {
        o->busy = 0;
        t_fail(YTR_ERR_CLOSED);
        return -1;
    }
    return 0;
}

static PyObject* t_info(const ytr_trial_info* ti) {
    return PyObject_CallFunction(TInfoType, "iiiiiOOOOO", ti->index, ti->condition, ti->track,
                                 ti->rep, ti->block,
                                 ti->first_in_block ? Py_True : Py_False,
                                 ti->after_break ? Py_True : Py_False,
                                 ti->practice ? Py_True : Py_False,
                                 ti->warmup ? Py_True : Py_False,
                                 ti->requeued ? Py_True : Py_False);
}

static PyObject* Trials_next(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    TrialsObject* o = TO(self);
    ytr_trial_info ti;
    int rc;
    if (t_open_enter(o) < 0) return NULL;
    rc = ytr_next(&o->t, &ti);
    if (t_leave(o) < 0) return NULL;
    if (rc == YTR_DONE) Py_RETURN_NONE;
    if (rc < 0) return t_fail(rc);
    return t_info(&ti);
}

/* Borrow the bytes of a record; NULL (no error) for None. */
static int t_record_arg(TrialsObject* o, PyObject* rec, PyObject** keep, const char** p) {
    *keep = NULL;
    *p = NULL;
    if (!rec || rec == Py_None) return 0;
    if (o->record_size == 0) return 0;   /* the header ignores it; so do we */
    *keep = PyBytes_FromObject(rec);
    if (!*keep) return -1;
    if ((size_t)PyBytes_Size(*keep) != o->record_size) {
        PyErr_Format(TArgumentError, "the record has %zd bytes; record_size is %zu",
                     PyBytes_Size(*keep), o->record_size);
        Py_CLEAR(*keep);
        return -1;
    }
    *p = PyBytes_AsString(*keep);
    return 0;
}

static PyObject* Trials_update(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "outcome", "rec", NULL };
    TrialsObject* o = TO(self);
    int outcome, rc;
    PyObject *rec = Py_None, *keep;
    const char* p;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i|O", kw, &outcome, &rec)) return NULL;
    if (t_record_arg(o, rec, &keep, &p) < 0) return NULL;
    if (t_open_enter(o) < 0) { Py_XDECREF(keep); return NULL; }
    rc = ytr_update(&o->t, outcome, p);
    o->busy = 0;
    Py_XDECREF(keep);
    if (rc < 0) return t_fail(rc);
    Py_RETURN_NONE;
}

static PyObject* Trials_requeue(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    TrialsObject* o = TO(self);
    int rc;
    if (t_open_enter(o) < 0) return NULL;
    rc = ytr_requeue(&o->t);
    if (t_leave(o) < 0) return NULL;
    if (rc < 0) return t_fail(rc);
    Py_RETURN_NONE;
}

static PyObject* Trials_mark_break(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    TrialsObject* o = TO(self);
    int rc;
    if (t_open_enter(o) < 0) return NULL;
    rc = ytr_mark_break(&o->t);
    o->busy = 0;
    if (rc < 0) return t_fail(rc);
    Py_RETURN_NONE;
}

static PyObject* Trials_level(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "condition", "factor", NULL };
    TrialsObject* o = TO(self);
    int c, f, rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "ii", kw, &c, &f)) return NULL;
    if (t_open_enter(o) < 0) return NULL;
    o->busy = 0;
    rc = ytr_level(&o->t, c, f);
    if (rc < 0) return t_fail(rc);
    return PyLong_FromLong(rc);
}

static PyObject* Trials_levels(PyObject* self, PyObject* arg) {
    TrialsObject* o = TO(self);
    long c = PyLong_AsLong(arg);
    int nf, f;
    PyObject* t;
    if (c == -1 && PyErr_Occurred()) return NULL;
    if (t_open_enter(o) < 0) return NULL;
    o->busy = 0;
    nf = ytr_n_factors(&o->t);
    t = PyTuple_New(nf);
    if (!t) return NULL;
    for (f = 0; f < nf; f++) {
        int v = ytr_level(&o->t, (int)c, f);
        if (v < 0) { Py_DECREF(t); return t_fail(v); }
        PyTuple_SetItem(t, f, PyLong_FromLong(v));
    }
    return t;
}

static PyObject* Trials_condition_from_levels(PyObject* self, PyObject* arg) {
    TrialsObject* o = TO(self);
    int* v = NULL;
    int n, rc;
    if (t_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (t_int_list(arg, &v, &n, "levels") < 0) return NULL;
    if (n != ytr_n_factors(&o->t)) {
        PyMem_Free(v);
        PyErr_Format(TArgumentError, "levels has %d entries; the design has %d factors",
                     n, ytr_n_factors(&o->t));
        return NULL;
    }
    rc = ytr_condition_from_levels(&o->t, v);
    PyMem_Free(v);
    if (rc < 0) return t_fail(rc);
    return PyLong_FromLong(rc);
}

static PyObject* Trials_condition_at(PyObject* self, PyObject* arg) {
    TrialsObject* o = TO(self);
    long i = PyLong_AsLong(arg);
    int rc;
    if (i == -1 && PyErr_Occurred()) return NULL;
    if (t_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (i < 0 || i > 0x7FFFFFFF) return t_fail(YTR_ERR_ARG);
    rc = ytr_condition_at(&o->t, (int)i);
    if (rc < 0) return t_fail(rc);
    return PyLong_FromLong(rc);
}

static PyObject* Trials_schedule(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    TrialsObject* o = TO(self);
    int n, i;
    PyObject* list;
    if (t_open_enter(o) < 0) return NULL;
    o->busy = 0;
    n = ytr_n_scheduled(&o->t);
    list = PyList_New(n);
    if (!list) return NULL;
    for (i = 0; i < n; i++) PyList_SetItem(list, i, PyLong_FromLong(ytr_condition_at(&o->t, i)));
    return list;
}

static int t_cond_arg(TrialsObject* o, PyObject* arg, int* c) {
    long v = PyLong_AsLong(arg);
    if (v == -1 && PyErr_Occurred()) return -1;
    if (v < 0 || v >= ytr_n_conditions(&o->t)) {
        PyErr_Format(PyExc_IndexError, "condition %ld out of range", v);
        return -1;
    }
    *c = (int)v;
    return 0;
}

static PyObject* Trials_n_valid(PyObject* self, PyObject* arg) {
    TrialsObject* o = TO(self);
    int c;
    if (t_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (t_cond_arg(o, arg, &c) < 0) return NULL;
    return PyLong_FromLong(ytr_n_valid(&o->t, c));
}

static PyObject* Trials_count(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "condition", "outcome", NULL };
    TrialsObject* o = TO(self);
    PyObject* c_o;
    int c, outcome = 1, rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|i", kw, &c_o, &outcome)) return NULL;
    if (t_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (t_cond_arg(o, c_o, &c) < 0) return NULL;
    rc = ytr_count(&o->t, c, outcome);
    if (rc < 0) return t_fail(rc);
    return PyLong_FromLong(rc);
}

static PyObject* Trials_proportion(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "condition", "outcome", NULL };
    TrialsObject* o = TO(self);
    PyObject* c_o;
    int c, outcome = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|i", kw, &c_o, &outcome)) return NULL;
    if (t_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (t_cond_arg(o, c_o, &c) < 0) return NULL;
    return PyFloat_FromDouble(ytr_proportion(&o->t, c, outcome));
}

static PyObject* Trials_history(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    TrialsObject* o = TO(self);
    const ytr_trial* h;
    int n = 0, i;
    PyObject* list;
    if (t_open_enter(o) < 0) return NULL;
    o->busy = 0;
    h = ytr_history(&o->t, &n);
    list = PyList_New(n);
    if (!list) return NULL;
    for (i = 0; i < n; i++) {
        unsigned f = h[i].flags;
        PyObject* tr = PyObject_CallFunction(
            TTrialType, "iiiiiiIOOOOOOOO", i, (int)h[i].condition, (int)h[i].track,
            (int)h[i].rep, (int)h[i].block, (int)h[i].outcome, f,
            (f & YTR_FLAG_PRACTICE) ? Py_True : Py_False,
            (f & YTR_FLAG_WARMUP) ? Py_True : Py_False,
            (f & YTR_FLAG_REQUEUED) ? Py_True : Py_False,
            (f & YTR_FLAG_AFTER_BREAK) ? Py_True : Py_False,
            (f & YTR_FLAG_FIRST_IN_BLOCK) ? Py_True : Py_False,
            (f & YTR_FLAG_VIOLATION) ? Py_True : Py_False,
            (f & YTR_FLAG_DONE) ? Py_True : Py_False,
            (f & YTR_FLAG_LEADIN) ? Py_True : Py_False);
        if (!tr) { Py_DECREF(list); return NULL; }
        PyList_SetItem(list, i, tr);
    }
    return list;
}

static PyObject* Trials_record(PyObject* self, PyObject* arg) {
    TrialsObject* o = TO(self);
    long i = PyLong_AsLong(arg);
    const void* p;
    if (i == -1 && PyErr_Occurred()) return NULL;
    if (t_open_enter(o) < 0) return NULL;
    o->busy = 0;
    if (o->record_size == 0) { PyErr_SetString(TArgumentError, "the session has no records (record_size 0)"); return NULL; }
    if (i < 0 || i >= ytr_n_run(&o->t)) {
        PyErr_Format(PyExc_IndexError, "trial %ld out of range", i);
        return NULL;
    }
    p = ytr_record(&o->t, (int)i);
    if (!p) return t_fail(YTR_ERR_ARG);
    return PyBytes_FromStringAndSize((const char*)p, (Py_ssize_t)o->record_size);
}

/* Call a snprintf-semantics formatter into a buffer that grows to fit. */
typedef int (*t_fmt_fn)(const ytr_trials*, int, char*, size_t);

static int t_fmt_header(const ytr_trials* t, int i, char* b, size_t c) { (void)i; return ytr_format_header(t, b, c); }
static int t_fmt_meta(const ytr_trials* t, int i, char* b, size_t c) { (void)i; return ytr_format_meta(t, b, c); }

static PyObject* t_format(TrialsObject* o, t_fmt_fn fn, int i) {
    char small[512];
    int n;
    PyObject* r;
    if (t_open_enter(o) < 0) return NULL;
    o->busy = 0;
    n = fn(&o->t, i, small, sizeof(small));
    if (n < 0) return t_fail(n);
    if ((size_t)n < sizeof(small)) return PyUnicode_FromStringAndSize(small, n);
    {
        char* big = (char*)PyMem_Malloc((size_t)n + 1);
        if (!big) return PyErr_NoMemory();
        n = fn(&o->t, i, big, (size_t)n + 1);
        r = n < 0 ? t_fail(n) : PyUnicode_FromStringAndSize(big, n);
        PyMem_Free(big);
        return r;
    }
}

static PyObject* Trials_format_row(PyObject* self, PyObject* arg) {
    long i = PyLong_AsLong(arg);
    if (i == -1 && PyErr_Occurred()) return NULL;
    if (i < 0 || i > 0x7FFFFFFF) return t_fail(YTR_ERR_ARG);
    return t_format(TO(self), ytr_format_row, (int)i);
}
static PyObject* Trials_format_header(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    return t_format(TO(self), t_fmt_header, 0);
}
static int t_fmt_rules(const ytr_trials* t, int i, char* b, size_t c) { (void)i; return ytr_format_rules(t, b, c); }
static PyObject* Trials_format_rules(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    return t_format(TO(self), t_fmt_rules, 0);
}

static PyObject* Trials_values(PyObject* self, PyObject* arg) {
    TrialsObject* o = TO(self);
    long c = PyLong_AsLong(arg);
    if (c == -1 && PyErr_Occurred()) return NULL;
    if (!o->table_obj) { PyErr_SetString(TArgumentError, "values() needs a session with a table"); return NULL; }
    if (c < 0 || c >= TB(o->table_obj)->tab.n_rows) { PyErr_Format(PyExc_IndexError, "condition %ld out of range", c); return NULL; }
    return tb_row(TB(o->table_obj), (int)c);
}

static PyObject* Trials_jitter(PyObject* self, PyObject* args) {
    TrialsObject* o = TO(self);
    PyObject* jo;
    long i;
    int j;
    if (!PyArg_ParseTuple(args, "lO", &i, &jo)) return NULL;
    if (!o->t.open) return t_fail(YTR_ERR_CLOSED);
    if (PyUnicode_Check(jo)) {
        PyObject* keep;
        const char* nm = t_utf8(jo, &keep);
        if (!nm) return NULL;
        j = ytr_jitter_index(&o->t, nm);
        Py_DECREF(keep);
        if (j < 0) { PyErr_SetString(TArgumentError, "no jitter of that name"); return NULL; }
    } else {
        long jl = PyLong_AsLong(jo);
        if (jl == -1 && PyErr_Occurred()) return NULL;
        if (jl < 0 || jl >= o->t.n_jit) { PyErr_Format(PyExc_IndexError, "jitter %ld out of range", jl); return NULL; }
        j = (int)jl;
    }
    if (i < 0 || i >= ytr_n_run(&o->t)) { PyErr_Format(PyExc_IndexError, "trial %ld has not run", i); return NULL; }
    return t_jitter_value(ytr_jitter(&o->t, (int)i, j));
}

static PyObject* Trials_get_jitters(PyObject* self, void* Py_UNUSED(c)) {
    TrialsObject* o = TO(self);
    PyObject* l;
    int j;
    l = PyList_New(o->t.open ? o->t.n_jit : 0);
    if (!l || !o->t.open) return l;
    for (j = 0; j < o->t.n_jit; j++) PyList_SetItem(l, j, PyUnicode_FromString(o->t.jit[j].name));
    return l;
}

static PyObject* Trials_get_table(PyObject* self, void* Py_UNUSED(c)) {
    TrialsObject* o = TO(self);
    if (!o->table_obj) Py_RETURN_NONE;
    Py_INCREF(o->table_obj);
    return o->table_obj;
}

static PyObject* Trials_format_meta(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    return t_format(TO(self), t_fmt_meta, 0);
}

static PyObject* Trials_save(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    TrialsObject* o = TO(self);
    size_t n;
    char* buf;
    int rc;
    PyObject* r;
    if (t_open_enter(o) < 0) return NULL;
    o->busy = 0;
    n = ytr_save_size(&o->t);
    buf = (char*)PyMem_Malloc(n > 0 ? n : 1);
    if (!buf) return PyErr_NoMemory();
    rc = ytr_save(&o->t, buf, n);
    if (rc < 0) { PyMem_Free(buf); return t_fail(rc); }
    r = PyBytes_FromStringAndSize(buf, rc);
    PyMem_Free(buf);
    return r;
}

/* Trials.load(data, **desc): a new Trials rebuilt from a snapshot. */
static PyObject* Trials_load(PyObject* cls, PyObject* args, PyObject* kwds) {
    PyObject *data, *bytes, *self, *rest;
    TrialsObject* o;
    ytr_desc d;
    bool ok;
    Py_ssize_t na = PyTuple_Size(args);
    if (na != 1) {
        PyErr_SetString(PyExc_TypeError, "load(data, **desc) takes the snapshot bytes and the desc as keywords");
        return NULL;
    }
    data = PyTuple_GetItem(args, 0);
    bytes = PyBytes_FromObject(data);
    if (!bytes) return NULL;
    self = PyObject_CallMethod(cls, "__new__", "O", cls);
    if (!self) { Py_DECREF(bytes); return NULL; }
    o = TO(self);
    rest = PyTuple_New(0);
    if (!rest) { Py_DECREF(bytes); Py_DECREF(self); return NULL; }
    if (t_build(o, rest, kwds, &d) < 0) {
        Py_DECREF(rest); Py_DECREF(bytes); Py_DECREF(self);
        return NULL;
    }
    Py_DECREF(rest);
    o->busy = 1;
    /* load() draws nothing and calls no track. */
    {
        const char* p = PyBytes_AsString(bytes);
        size_t len = (size_t)PyBytes_Size(bytes);
        Py_BEGIN_ALLOW_THREADS
        ok = ytr_open_saved(&o->t, &d, p, len);
        Py_END_ALLOW_THREADS
    }
    o->busy = 0;
    Py_DECREF(bytes);
    if (!ok) {
        PyErr_SetString(TError, ytr_error(&o->t));
        Py_DECREF(self);
        return NULL;
    }
    return self;
}

static PyObject* Trials_restore(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "outcomes", "records", NULL };
    TrialsObject* o = TO(self);
    PyObject *outs, *recs = Py_None, *blob = NULL;
    int* v = NULL;
    int n, rc;
    const char* rp = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O", kw, &outs, &recs)) return NULL;
    if (t_int_list(outs, &v, &n, "outcomes") < 0) return NULL;
    if (recs != Py_None && o->record_size > 0) {
        /* One bytes-like of n * record_size, or a sequence of n records. */
        if (PyBytes_Check(recs) || PyByteArray_Check(recs) || PyMemoryView_Check(recs)) {
            blob = PyBytes_FromObject(recs);
        } else {
            PyObject* empty = PyBytes_FromStringAndSize("", 0);
            PyObject* joined = empty ? PyObject_CallMethod(empty, "join", "O", recs) : NULL;
            Py_XDECREF(empty);
            blob = joined;
        }
        if (!blob) { PyMem_Free(v); return NULL; }
        if ((size_t)PyBytes_Size(blob) != (size_t)n * o->record_size) {
            PyErr_Format(TArgumentError, "records hold %zd bytes; %d trials of %zu need %zu",
                         PyBytes_Size(blob), n, o->record_size, (size_t)n * o->record_size);
            Py_DECREF(blob);
            PyMem_Free(v);
            return NULL;
        }
        rp = PyBytes_AsString(blob);
    }
    if (t_open_enter(o) < 0) { Py_XDECREF(blob); PyMem_Free(v); return NULL; }
    rc = ytr_restore(&o->t, v, rp, n);
    PyMem_Free(v);
    Py_XDECREF(blob);
    if (t_leave(o) < 0) return NULL;
    if (rc < 0) return t_fail(rc);
    Py_RETURN_NONE;
}

/* --- properties --------------------------------------------------------- */

static PyObject* Trials_get_done(PyObject* self, void* Py_UNUSED(c)) {
    TrialsObject* o = TO(self);
    bool d;
    if (t_enter(o) < 0) return NULL;
    d = ytr_done(&o->t);
    if (t_leave(o) < 0) return NULL;
    return PyBool_FromLong(d);
}
#define T_INT_GETTER(name, expr)                                              \
    static PyObject* Trials_get_##name(PyObject* self, void* Py_UNUSED(c)) {  \
        return PyLong_FromLong((long)(expr));                                 \
    }
T_INT_GETTER(n_conditions, ytr_n_conditions(&TO(self)->t))
T_INT_GETTER(n_factors, ytr_n_factors(&TO(self)->t))
T_INT_GETTER(n_scheduled, ytr_n_scheduled(&TO(self)->t))
T_INT_GETTER(n_run, ytr_n_run(&TO(self)->t))
T_INT_GETTER(n_done, ytr_n_done(&TO(self)->t))
T_INT_GETTER(swaps, ytr_is_open(&TO(self)->t) ? TO(self)->t.swaps : 0)

static PyObject* Trials_get_is_open(PyObject* self, void* Py_UNUSED(c)) {
    return PyBool_FromLong(ytr_is_open(&TO(self)->t));
}
static PyObject* Trials_get_record_size(PyObject* self, void* Py_UNUSED(c)) {
    return PyLong_FromSize_t(TO(self)->record_size);
}
static PyObject* Trials_get_rng_state(PyObject* self, void* Py_UNUSED(c)) {
    TrialsObject* o = TO(self);
    if (!o->own_rng) Py_RETURN_NONE;
    return PyLong_FromUnsignedLongLong(o->rng_state);
}
static int Trials_set_rng_state(PyObject* self, PyObject* v, void* Py_UNUSED(c)) {
    TrialsObject* o = TO(self);
    unsigned long long s;
    if (!v) { PyErr_SetString(PyExc_TypeError, "rng_state cannot be deleted"); return -1; }
    if (!o->own_rng) {
        PyErr_SetString(TError, "rng_state exists only when rng was an int seed");
        return -1;
    }
    s = PyLong_AsUnsignedLongLongMask(v);
    if (s == (unsigned long long)-1 && PyErr_Occurred()) return -1;
    o->rng_state = (uint64_t)s;
    return 0;
}

static PyGetSetDef Trials_getset[] = {
    { "done", Trials_get_done, NULL,
      "True when nothing is pending or scheduled and every track is done. "
      "Asks the tracks.", NULL },
    { "n_conditions", Trials_get_n_conditions, NULL, "Condition rows.", NULL },
    { "n_factors", Trials_get_n_factors, NULL, "Factors.", NULL },
    { "n_scheduled", Trials_get_n_scheduled, NULL,
      "Schedule slots: practice, then main, re-queued copies included.", NULL },
    { "n_run", Trials_get_n_run, NULL, "Trials handed out, a pending one included.", NULL },
    { "n_done", Trials_get_n_done, NULL, "Trials updated or re-queued.", NULL },
    { "swaps", Trials_get_swaps, NULL, "Swaps the constraint repair used at open.", NULL },
    { "is_open", Trials_get_is_open, NULL, "True after a successful open or load.", NULL },
    { "record_size", Trials_get_record_size, NULL, "Bytes per trial record.", NULL },
    { "table", Trials_get_table, NULL, "The Table, or None.", NULL },
    { "jitters", Trials_get_jitters, NULL, "The jitters' names, in order.", NULL },
    { "rng_state", Trials_get_rng_state, Trials_set_rng_state,
      "The splitmix64 state when rng was an int seed (None otherwise). Save it "
      "beside save(); pass it as rng= to load().", NULL },
    { NULL }
};

static PyMethodDef Trials_methods[] = {
    { "next", Trials_next, METH_NOARGS,
      "next() -> TrialInfo | None: the next trial, or None when the session is "
      "done. Repeats the pending trial until update() or requeue()." },
    { "update", (PyCFunction)(void (*)(void))Trials_update, METH_VARARGS | METH_KEYWORDS,
      "update(outcome, rec=None): record the pending trial's outcome (>= 0, or "
      "INVALID) and its record_size-byte record (None zero-fills)." },
    { "requeue", Trials_requeue, METH_NOARGS,
      "requeue(): instead of update(), record REQUEUE and schedule the "
      "condition again. Scheduled trials only." },
    { "mark_break", Trials_mark_break, METH_NOARGS,
      "mark_break(): a break was taken; flags the pending or next trial and "
      "starts a constraint segment." },
    { "level", (PyCFunction)(void (*)(void))Trials_level, METH_VARARGS | METH_KEYWORDS,
      "level(condition, factor) -> int: the factor's level index in that row." },
    { "levels", Trials_levels, METH_O,
      "levels(condition) -> tuple[int]: every factor's level in that row." },
    { "condition_from_levels", Trials_condition_from_levels, METH_O,
      "condition_from_levels(levels) -> int: the row with these level indices." },
    { "condition_at", Trials_condition_at, METH_O,
      "condition_at(slot) -> int: the condition in schedule slot `slot`." },
    { "schedule", Trials_schedule, METH_NOARGS,
      "schedule() -> list[int]: condition_at() for every slot." },
    { "n_valid", Trials_n_valid, METH_O,
      "n_valid(condition) -> int: valid, non-practice, non-warmup trials." },
    { "count", (PyCFunction)(void (*)(void))Trials_count, METH_VARARGS | METH_KEYWORDS,
      "count(condition, outcome=1) -> int" },
    { "proportion", (PyCFunction)(void (*)(void))Trials_proportion, METH_VARARGS | METH_KEYWORDS,
      "proportion(condition, outcome=1) -> float: count / n_valid; NaN when "
      "n_valid is 0." },
    { "history", Trials_history, METH_NOARGS,
      "history() -> list[Trial]: every trial handed out, a pending one included." },
    { "record", Trials_record, METH_O, "record(i) -> bytes: trial i's record." },
    { "format_row", Trials_format_row, METH_O,
      "format_row(i) -> str: one newline-terminated CSV line." },
    { "format_header", Trials_format_header, METH_NOARGS,
      "format_header() -> str: the matching CSV header line." },
    { "format_meta", Trials_format_meta, METH_NOARGS,
      "format_meta() -> str: one key=value line describing the session." },
    { "format_rules", Trials_format_rules, METH_NOARGS,
      "format_rules() -> str: the order settings as rules text, which pastes "
      "back as rules=." },
    { "jitter", Trials_jitter, METH_VARARGS,
      "jitter(trial, j) -> Jitter(s, ns, frames): trial's draw of jitter j (an index "
      "or a name). frames is -1 unless the jitter snaps to frames." },
    { "values", Trials_values, METH_O,
      "values(condition) -> dict: the table row of a condition." },
    { "save", Trials_save, METH_NOARGS, "save() -> bytes: a snapshot of the session." },
    { "load", (PyCFunction)(void (*)(void))Trials_load, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
      "Trials.load(data, **desc) -> Trials: rebuild a session from save()'s "
      "bytes. The desc must match the saved one; it re-supplies the tracks, "
      "the rng (for an int seed, pass the rng_state saved with the snapshot) "
      "and record_size. Raises Error with the header's message on a mismatch." },
    { "restore", (PyCFunction)(void (*)(void))Trials_restore, METH_VARARGS | METH_KEYWORDS,
      "restore(outcomes, records=None): replay outcomes on a fresh session "
      "with the same desc and seed. REQUEUE re-queues." },
    { NULL }
};

static PyType_Slot Trials_slots[] = {
    { Py_tp_doc, (void*)
      "Trials(*, n_conditions=0, factors=None, reps=0, cond_reps=None, "
      "order=ORDER_SEQUENTIAL, constraints=None, max_swaps=0, tracks=None, "
      "interleave=INTERLEAVE_RANDOM, track_rate=0.0, block_size=0, "
      "constraints_span_blocks=False, n_practice=0, n_warmup=0, "
      "warmup_conditions=None, requeue_gap=0, rng=None, record_size=0, "
      "table=None, order_list=None, draws=0, weights=None, subset=0, "
      "groups=None, rules=None, participant=0, jitters=None)\n\n"
      "One ysp/trials.h session. factors is a list of (name, n_levels); "
      "table is a Table whose rows are the conditions (names then resolve "
      "against its columns and levels); rules is rules text (ytr_rules) "
      "applied over the keywords; groups comes from groups(); jitters "
      "come from uniform(), choice() and exponential(); "
      "constraints come from max_run() and friends; tracks are objects with "
      "is_done() or a done attribute, or (object, weight); rng is a callable "
      "returning [0, 1) or an int seed for the header's splitmix64." },
    { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_init, (void*)Trials_init },
    { Py_tp_dealloc, (void*)Trials_dealloc },
    { Py_tp_traverse, (void*)t_traverse },
    { Py_tp_clear, (void*)t_clear },
    { Py_tp_methods, Trials_methods },
    { Py_tp_getset, Trials_getset },
    { 0, NULL }
};

static PyType_Spec Trials_spec = {
    .name = "ysp.trials.Trials",
    .basicsize = sizeof(TrialsObject),
    .itemsize = 0,
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = Trials_slots,
};

/* ======================================================================= *
 *  Module functions: constraint helpers and splitmix
 * ======================================================================= */

/* A constraint as a dict. `factor` may be a factor or column name and a
 * level a column value, resolved at open. */
static PyObject* t_constraint_o(const ytr_constraint* c, PyObject* factor, PyObject* level,
                                PyObject* level2) {
    PyObject* d = Py_BuildValue("{s:i,s:O,s:O,s:O,s:i,s:i}", "rule", (int)c->rule,
                                "factor", factor, "level", level, "level2", level2,
                                "n", c->n, "window", c->window);
    return d;
}

static PyObject* t_constraint(const ytr_constraint* c, PyObject* factor) {
    PyObject *l = PyLong_FromLong(c->level), *l2 = PyLong_FromLong(c->level2), *r = NULL;
    if (l && l2) r = t_constraint_o(c, factor, l, l2);
    Py_XDECREF(l);
    Py_XDECREF(l2);
    return r;
}

static PyObject* t_factor_arg(PyObject* f, int* idx) {
    if (PyUnicode_Check(f)) { *idx = 0; Py_INCREF(f); return f; }
    {
        long v = PyLong_AsLong(f);
        if (v == -1 && PyErr_Occurred()) return NULL;
        *idx = (int)v;
        return PyLong_FromLong(v);
    }
}

/* The helpers keep a level as given (an int, or a column value with a
 * table) and the factor as an index or a name; open() resolves both. */
static PyObject* t_make(ytr_constraint c, PyObject* f, PyObject* l, PyObject* l2) {
    PyObject *fo, *r;
    int fi;
    PyObject* zero = PyLong_FromLong(0);
    if (!zero) return NULL;
    if (!(fo = t_factor_arg(f, &fi))) { Py_DECREF(zero); return NULL; }
    r = t_constraint_o(&c, fo, l ? l : zero, l2 ? l2 : zero);
    Py_DECREF(fo);
    Py_DECREF(zero);
    return r;
}

static PyObject* mod_max_run(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "factor", "level", "n", NULL };
    PyObject *f, *l;
    int n;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOi", kw, &f, &l, &n)) return NULL;
    return t_make(ytr_max_run(0, 0, n), f, l, NULL);
}

static PyObject* mod_max_in_window(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "factor", "level", "window", "n", NULL };
    PyObject *f, *l;
    int w, n;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOii", kw, &f, &l, &w, &n)) return NULL;
    return t_make(ytr_max_in_window(0, 0, w, n), f, l, NULL);
}

static PyObject* mod_min_gap(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "factor", "level", "gap", NULL };
    PyObject *f, *l;
    int g;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOi", kw, &f, &l, &g)) return NULL;
    return t_make(ytr_min_gap(0, 0, g), f, l, NULL);
}

static PyObject* mod_no_transition(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "factor", "from_level", "to_level", NULL };
    PyObject *f, *a, *b;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOO", kw, &f, &a, &b)) return NULL;
    return t_make(ytr_no_transition(0, 0, 0), f, a, b);
}

static PyObject* mod_first_not(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "factor", "level", NULL };
    PyObject *f, *l;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO", kw, &f, &l)) return NULL;
    return t_make(ytr_first_not(0, 0), f, l, NULL);
}

static PyObject* mod_followed_by(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "factor", "level", "next_level", NULL };
    PyObject *f, *a, *b;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOO", kw, &f, &a, &b)) return NULL;
    return t_make(ytr_followed_by(0, 0, 0), f, a, b);
}

static PyObject* mod_preceded_by(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "factor", "level", "prev_level", NULL };
    PyObject *f, *a, *b;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOO", kw, &f, &a, &b)) return NULL;
    return t_make(ytr_preceded_by(0, 0, 0), f, a, b);
}

static PyObject* mod_chunk(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "factor", NULL };
    PyObject* f;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kw, &f)) return NULL;
    return t_make(ytr_chunk(0), f, NULL, NULL);
}

static PyObject* mod_balance(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "factor", "no_repeat", "no_leadin", NULL };
    PyObject* f;
    int nr = 0, nl = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|$pp", kw, &f, &nr, &nl)) return NULL;
    return t_make(ytr_balance_flags(0, (nr ? YTR_BALANCE_NO_REPEAT : 0) | (nl ? YTR_BALANCE_NO_LEADIN : 0)),
                  f, NULL, NULL);
}

static PyObject* mod_groups(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "factor", "mode", "order", "participant", "list", NULL };
    PyObject *f, *lst = Py_None;
    const char *mode = "blocked", *order = "sequential";
    int p = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|ss$iO", kw, &f, &mode, &order, &p, &lst)) return NULL;
    return Py_BuildValue("{s:O,s:s,s:s,s:i,s:O}", "factor", f, "mode", mode, "order", order,
                         "participant", p, "list", lst);
}

static PyObject* mod_latin(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "n", "row", "balanced", NULL };
    int n, row, bal = 0, rows, j;
    int* out;
    PyObject* list;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "ii|p", kw, &n, &row, &bal)) return NULL;
    if (n < 1 || n > YTR_MAX_CONDITIONS || row < 0) {
        PyErr_SetString(TArgumentError, "latin(n, row): n in [1, MAX_CONDITIONS], row >= 0");
        return NULL;
    }
    out = (int*)PyMem_Malloc(sizeof(int) * (size_t)n);
    if (!out) return PyErr_NoMemory();
    rows = ytr_latin(n, row, bal != 0, out);
    (void)rows;
    list = PyList_New(n);
    if (list)
        for (j = 0; j < n; j++) PyList_SetItem(list, j, PyLong_FromLong(out[j]));
    PyMem_Free(out);
    return list;
}

static PyObject* mod_latin_rows(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "n", "balanced", NULL };
    int n, bal = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i|p", kw, &n, &bal)) return NULL;
    if (n < 1 || n > YTR_MAX_CONDITIONS) { PyErr_SetString(TArgumentError, "n in [1, MAX_CONDITIONS]"); return NULL; }
    return PyLong_FromLong((bal && n % 2) ? 2 * n : n);
}

static PyObject* t_jit_dict(const char* name, const char* dist, PyObject* lo, PyObject* hi,
                            PyObject* scale, PyObject* values, PyObject* rate) {
    return Py_BuildValue("{s:s,s:s,s:O,s:O,s:O,s:O,s:O}", "name", name, "dist", dist, "lo", lo,
                         "hi", hi, "scale", scale, "values", values, "rate", rate);
}

static PyObject* mod_uniform(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "name", "lo", "hi", "rate", NULL };
    const char* name;
    PyObject *lo, *hi, *rate = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sOO|$O", kw, &name, &lo, &hi, &rate)) return NULL;
    return t_jit_dict(name, "uniform", lo, hi, Py_None, Py_None, rate);
}

static PyObject* mod_choice(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "name", "values", "rate", NULL };
    const char* name;
    PyObject *vals, *rate = Py_None, *lst, *r;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sO|$O", kw, &name, &vals, &rate)) return NULL;
    lst = PySequence_List(vals);
    if (!lst) return NULL;
    r = t_jit_dict(name, "choice", Py_None, Py_None, Py_None, lst, rate);
    Py_DECREF(lst);
    return r;
}

static PyObject* mod_exponential(PyObject* Py_UNUSED(m), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "name", "lo", "hi", "scale", "rate", NULL };
    const char* name;
    PyObject *lo, *hi, *scale, *rate = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sOOO|$O", kw, &name, &lo, &hi, &scale, &rate)) return NULL;
    return t_jit_dict(name, "exponential", lo, hi, scale, Py_None, rate);
}

static PyObject* mod_jitter_map(PyObject* Py_UNUSED(m), PyObject* args) {
    PyObject *spec, *names;
    ytr_jitter_desc j;
    int64_t vals[YTR_MAX_JITTER_VALUES];
    double u;
    char err[256];
    PyObject* r = NULL;
    if (!PyArg_ParseTuple(args, "Od", &spec, &u)) return NULL;
    names = PyList_New(0);
    if (!names) return NULL;
    if (t_jitter_desc(spec, &j, vals, names) == 0) {
        if (ytr_jitter_check(&j, err, sizeof(err)) < 0) PyErr_SetString(TArgumentError, err);
        else r = t_jitter_value(ytr_jitter_map(&j, u));
    }
    Py_DECREF(names);
    return r;
}

static PyObject* mod_splitmix(PyObject* Py_UNUSED(m), PyObject* arg) {
    unsigned long long s = PyLong_AsUnsignedLongLongMask(arg);
    uint64_t st;
    double u;
    if (s == (unsigned long long)-1 && PyErr_Occurred()) return NULL;
    st = (uint64_t)s;
    u = ytr_splitmix(&st);
    return Py_BuildValue("(dK)", u, (unsigned long long)st);
}

static PyObject* mod_strerror(PyObject* Py_UNUSED(m), PyObject* arg) {
    long c = PyLong_AsLong(arg);
    if (c == -1 && PyErr_Occurred()) return NULL;
    return PyUnicode_FromString(ytr_strerror((int)c));
}

static PyMethodDef module_methods[] = {
    { "max_run", (PyCFunction)(void (*)(void))mod_max_run, METH_VARARGS | METH_KEYWORDS,
      "max_run(factor, level, n): no more than n consecutive trials with that "
      "level. factor is an index, a factor name, or CONDITION; level may be "
      "ANY_LEVEL." },
    { "max_in_window", (PyCFunction)(void (*)(void))mod_max_in_window, METH_VARARGS | METH_KEYWORDS,
      "max_in_window(factor, level, window, n): at most n in any window trials." },
    { "min_gap", (PyCFunction)(void (*)(void))mod_min_gap, METH_VARARGS | METH_KEYWORDS,
      "min_gap(factor, level, gap): at least gap other trials between two." },
    { "no_transition", (PyCFunction)(void (*)(void))mod_no_transition, METH_VARARGS | METH_KEYWORDS,
      "no_transition(factor, from_level, to_level): from_level never directly "
      "followed by to_level." },
    { "first_not", (PyCFunction)(void (*)(void))mod_first_not, METH_VARARGS | METH_KEYWORDS,
      "first_not(factor, level): the first main trial does not have it." },
    { "followed_by", (PyCFunction)(void (*)(void))mod_followed_by, METH_VARARGS | METH_KEYWORDS,
      "followed_by(factor, level, next_level): every trial with level is "
      "directly followed by one with next_level (units)." },
    { "preceded_by", (PyCFunction)(void (*)(void))mod_preceded_by, METH_VARARGS | METH_KEYWORDS,
      "preceded_by(factor, level, prev_level): every trial with level is "
      "directly preceded by one with prev_level (units)." },
    { "chunk", (PyCFunction)(void (*)(void))mod_chunk, METH_VARARGS | METH_KEYWORDS,
      "chunk(factor): contiguous rows with one value run as one unit." },
    { "balance", (PyCFunction)(void (*)(void))mod_balance, METH_VARARGS | METH_KEYWORDS,
      "balance(factor, *, no_repeat=False, no_leadin=False): every ordered pair "
      "of levels adjacent equally often." },
    { "groups", (PyCFunction)(void (*)(void))mod_groups, METH_VARARGS | METH_KEYWORDS,
      "groups(factor, mode='blocked', order='sequential', *, participant=0, "
      "list=None) -> dict for Trials(groups=)." },
    { "latin", (PyCFunction)(void (*)(void))mod_latin, METH_VARARGS | METH_KEYWORDS,
      "latin(n, row, balanced=False) -> list[int]: row of a cyclic or "
      "Williams Latin square (row taken modulo the design's rows)." },
    { "latin_rows", (PyCFunction)(void (*)(void))mod_latin_rows, METH_VARARGS | METH_KEYWORDS,
      "latin_rows(n, balanced=False) -> int: rows of the design (2n for a "
      "balanced square of odd n)." },
    { "uniform", (PyCFunction)(void (*)(void))mod_uniform, METH_VARARGS | METH_KEYWORDS,
      "uniform(name, lo, hi, *, rate=None) -> dict for Trials(jitters=): a duration "
      "uniform on [lo, hi] s. lo and hi are seconds or a table column's name; rate "
      "is an int or (num, den) Hz to snap to whole frames." },
    { "choice", (PyCFunction)(void (*)(void))mod_choice, METH_VARARGS | METH_KEYWORDS,
      "choice(name, values, *, rate=None) -> dict: one of the values (s), each "
      "equally likely." },
    { "exponential", (PyCFunction)(void (*)(void))mod_exponential, METH_VARARGS | METH_KEYWORDS,
      "exponential(name, lo, hi, scale, *, rate=None) -> dict: lo + an exponential "
      "of mean scale, truncated at hi (a non-aging foreperiod)." },
    { "jitter_map", mod_jitter_map, METH_VARARGS,
      "jitter_map(spec, u) -> Jitter(s, ns, frames): the duration for a variate u "
      "in [0, 1), with no session (ytr_jitter_map)." },
    { "splitmix", mod_splitmix, METH_O,
      "splitmix(state) -> (u, new_state): one ytr_splitmix step, for a "
      "caller that wants to reproduce the header's generator." },
    { "strerror", mod_strerror, METH_O, "strerror(code) -> str" },
    { NULL }
};

static PyModuleDef ysp_trials_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "ysp.trials",
    .m_doc = "Trial sequencing: conditions, orders, constraints, interleaved "
             "adaptive tracks, blocks, re-queues, tallies (ysp/trials.h); "
             "tables from CSV (ysp/table.h), order lists, draws, subsets, "
             "groups, units, balance, Latin squares and rules text.",
    .m_size = -1,
    .m_methods = module_methods,
};

static PyObject* t_add_exc(PyObject* m, const char* qualname, const char* attr, PyObject* base) {
    PyObject* e = PyErr_NewException(qualname, base, NULL);
    if (!e) return NULL;
    Py_INCREF(e);
    if (PyModule_AddObject(m, attr, e) < 0) { Py_DECREF(e); Py_DECREF(e); return NULL; }
    return e;
}

typedef struct { const char* name; long value; } t_member;

static PyObject* t_add_enum(PyObject* m, PyObject* enum_mod, const char* cls_name,
                            const char* prefix, const t_member* mem, int n) {
    PyObject *base = NULL, *members = NULL, *args = NULL, *kwargs = NULL, *cls = NULL;
    int i;
    base = PyObject_GetAttrString(enum_mod, "IntEnum");
    if (!base) goto fail;
    members = PyList_New(n);
    if (!members) goto fail;
    for (i = 0; i < n; i++) {
        PyObject* t = Py_BuildValue("(sl)", mem[i].name, mem[i].value);
        if (!t) goto fail;
        PyList_SetItem(members, i, t);
    }
    args = Py_BuildValue("(sO)", cls_name, members);
    kwargs = Py_BuildValue("{s:s}", "module", "ysp.trials");
    if (!args || !kwargs) goto fail;
    cls = PyObject_Call(base, args, kwargs);
    if (!cls) goto fail;
    Py_INCREF(cls);
    if (PyModule_AddObject(m, cls_name, cls) < 0) { Py_DECREF(cls); goto fail; }
    for (i = 0; i < n; i++) {
        char full[64];
        PyObject* v = PyObject_GetAttrString(cls, mem[i].name);
        if (!v) goto fail;
        PyOS_snprintf(full, sizeof(full), "%s_%s", prefix, mem[i].name);
        if (PyModule_AddObject(m, full, v) < 0) { Py_DECREF(v); goto fail; }
    }
    Py_DECREF(base); Py_DECREF(members); Py_DECREF(args); Py_DECREF(kwargs);
    return cls;
fail:
    Py_XDECREF(base); Py_XDECREF(members); Py_XDECREF(args); Py_XDECREF(kwargs);
    Py_XDECREF(cls);
    return NULL;
}

static PyObject* t_namedtuple(PyObject* coll, const char* name, PyObject* fields) {
    PyObject *nt, *a, *k, *r = NULL;
    nt = PyObject_GetAttrString(coll, "namedtuple");
    a = Py_BuildValue("(sO)", name, fields);
    k = Py_BuildValue("{s:s}", "module", "ysp.trials");
    if (nt && a && k) r = PyObject_Call(nt, a, k);
    Py_XDECREF(nt); Py_XDECREF(a); Py_XDECREF(k);
    return r;
}

PyMODINIT_FUNC PyInit_trials(void) {
    static const t_member order_m[] = { { "SEQUENTIAL", YTR_ORDER_SEQUENTIAL },
                                        { "RANDOM", YTR_ORDER_RANDOM },
                                        { "FULL_RANDOM", YTR_ORDER_FULL_RANDOM },
                                        { "CONSTRAINED", YTR_ORDER_CONSTRAINED },
                                        { "LIST", YTR_ORDER_LIST },
                                        { "WITH_REPLACEMENT", YTR_ORDER_WITH_REPLACEMENT } };
    static const t_member il_m[] = { { "RANDOM", YTR_INTERLEAVE_RANDOM },
                                     { "ROUND_ROBIN", YTR_INTERLEAVE_ROUND_ROBIN } };
    static const t_member rule_m[] = { { "MAX_RUN", YTR_RULE_MAX_RUN },
                                       { "MAX_IN_WINDOW", YTR_RULE_MAX_IN_WINDOW },
                                       { "MIN_GAP", YTR_RULE_MIN_GAP },
                                       { "NO_TRANSITION", YTR_RULE_NO_TRANSITION },
                                       { "FIRST_NOT", YTR_RULE_FIRST_NOT },
                                       { "FOLLOWED_BY", YTR_RULE_FOLLOWED_BY },
                                       { "PRECEDED_BY", YTR_RULE_PRECEDED_BY },
                                       { "CHUNK", YTR_RULE_CHUNK },
                                       { "BALANCE", YTR_RULE_BALANCE } };
    static const t_member gm_m[] = { { "NONE", YTR_GROUPS_NONE },
                                     { "BLOCKED", YTR_GROUPS_BLOCKED },
                                     { "ALTERNATE", YTR_GROUPS_ALTERNATE } };
    static const t_member go_m[] = { { "SEQUENTIAL", YTR_GROUP_ORDER_SEQUENTIAL },
                                     { "RANDOM", YTR_GROUP_ORDER_RANDOM },
                                     { "LATIN", YTR_GROUP_ORDER_LATIN },
                                     { "BALANCED_LATIN", YTR_GROUP_ORDER_BALANCED_LATIN },
                                     { "LIST", YTR_GROUP_ORDER_LIST } };
    PyObject *m, *type, *mod = NULL, *cls, *fields, *bases;

    m = PyModule_Create(&ysp_trials_module);
    if (!m) return NULL;

    mod = PyImport_ImportModule("sys");
    if (!mod) goto fail;
    {
        PyObject* vi = PyObject_GetAttrString(mod, "version_info");
        PyObject* major = vi ? PySequence_GetItem(vi, 0) : NULL;
        PyObject* minor = vi ? PySequence_GetItem(vi, 1) : NULL;
        if (!major || !minor) { Py_XDECREF(vi); Py_XDECREF(major); Py_XDECREF(minor); goto fail; }
        g_visit_type = PyLong_AsLong(major) > 3 || PyLong_AsLong(minor) >= 9;
        Py_DECREF(vi); Py_DECREF(major); Py_DECREF(minor);
    }
    Py_CLEAR(mod);

    type = PyType_FromSpec(&Trials_spec);
    if (!type) goto fail;
    if (PyModule_AddObject(m, "Trials", type) < 0) { Py_DECREF(type); goto fail; }
    TTableType = PyType_FromSpec(&Table_spec);
    if (!TTableType) goto fail;
    Py_INCREF(TTableType);
    if (PyModule_AddObject(m, "Table", TTableType) < 0) { Py_DECREF(TTableType); goto fail; }

    if (!(TError = t_add_exc(m, "ysp.trials.Error", "Error", NULL))) goto fail;
    bases = PyTuple_Pack(2, TError, PyExc_ValueError);
    if (!bases) goto fail;
    TArgumentError = t_add_exc(m, "ysp.trials.ArgumentError", "ArgumentError", bases);
    Py_DECREF(bases);
    if (!TArgumentError) goto fail;
    if (!(TClosed = t_add_exc(m, "ysp.trials.Closed", "Closed", TError))) goto fail;
    if (!(TOutOfOrder = t_add_exc(m, "ysp.trials.OutOfOrder", "OutOfOrder", TError))) goto fail;
    if (!(TFull = t_add_exc(m, "ysp.trials.Full", "Full", TError))) goto fail;

    mod = PyImport_ImportModule("enum");
    if (!mod) goto fail;
    if (!(cls = t_add_enum(m, mod, "Order", "ORDER", order_m, 6))) goto fail;
    Py_DECREF(cls);
    if (!(cls = t_add_enum(m, mod, "Interleave", "INTERLEAVE", il_m, 2))) goto fail;
    Py_DECREF(cls);
    if (!(cls = t_add_enum(m, mod, "Rule", "RULE", rule_m, 9))) goto fail;
    Py_DECREF(cls);
    if (!(cls = t_add_enum(m, mod, "GroupMode", "GROUPS", gm_m, 3))) goto fail;
    Py_DECREF(cls);
    if (!(cls = t_add_enum(m, mod, "GroupOrder", "GROUP_ORDER", go_m, 5))) goto fail;
    Py_DECREF(cls);
    Py_CLEAR(mod);

    mod = PyImport_ImportModule("collections");
    if (!mod) goto fail;
    fields = Py_BuildValue("[ssssssssss]", "index", "condition", "track", "rep", "block",
                           "first_in_block", "after_break", "practice", "warmup", "requeued");
    TInfoType = fields ? t_namedtuple(mod, "TrialInfo", fields) : NULL;
    Py_XDECREF(fields);
    if (!TInfoType) goto fail;
    Py_INCREF(TInfoType);
    if (PyModule_AddObject(m, "TrialInfo", TInfoType) < 0) { Py_DECREF(TInfoType); goto fail; }
    fields = Py_BuildValue("[sssssssssssssss]", "index", "condition", "track", "rep", "block",
                           "outcome", "flags", "practice", "warmup", "requeued", "after_break",
                           "first_in_block", "violation", "done", "leadin");
    TTrialType = fields ? t_namedtuple(mod, "Trial", fields) : NULL;
    Py_XDECREF(fields);
    if (!TTrialType) goto fail;
    Py_INCREF(TTrialType);
    if (PyModule_AddObject(m, "Trial", TTrialType) < 0) { Py_DECREF(TTrialType); goto fail; }
    fields = Py_BuildValue("[sss]", "s", "ns", "frames");
    TJitterType = fields ? t_namedtuple(mod, "Jitter", fields) : NULL;
    Py_XDECREF(fields);
    if (!TJitterType) goto fail;
    Py_INCREF(TJitterType);
    if (PyModule_AddObject(m, "Jitter", TJitterType) < 0) { Py_DECREF(TJitterType); goto fail; }
    Py_CLEAR(mod);

    /* From the compiled implementation, so it names the header actually built
     * in; the tests check pyproject.toml against it. */
    if (PyModule_AddStringConstant(m, "__version__", ytr_version()) < 0) goto fail;
    PyModule_AddIntConstant(m, "DONE", YTR_DONE);
    PyModule_AddIntConstant(m, "ERR_ARG", YTR_ERR_ARG);
    PyModule_AddIntConstant(m, "ERR_CLOSED", YTR_ERR_CLOSED);
    PyModule_AddIntConstant(m, "ERR_ORDER", YTR_ERR_ORDER);
    PyModule_AddIntConstant(m, "ERR_FULL", YTR_ERR_FULL);
    PyModule_AddIntConstant(m, "INVALID", YTR_INVALID);
    PyModule_AddIntConstant(m, "REQUEUE", YTR_REQUEUE);
    PyModule_AddIntConstant(m, "CONDITION", YTR_CONDITION);
    PyModule_AddIntConstant(m, "ANY_LEVEL", YTR_ANY_LEVEL);
    PyModule_AddIntConstant(m, "MAX_TRIALS", YTR_MAX_TRIALS);
    PyModule_AddIntConstant(m, "MAX_CONDITIONS", YTR_MAX_CONDITIONS);
    PyModule_AddIntConstant(m, "MAX_FACTORS", YTR_MAX_FACTORS);
    PyModule_AddIntConstant(m, "MAX_CONSTRAINTS", YTR_MAX_CONSTRAINTS);
    PyModule_AddIntConstant(m, "MAX_TRACKS", YTR_MAX_TRACKS);
    PyModule_AddIntConstant(m, "MAX_JITTERS", YTR_MAX_JITTERS);
    PyModule_AddIntConstant(m, "MAX_JITTER_VALUES", YTR_MAX_JITTER_VALUES);
    PyModule_AddIntConstant(m, "FLAG_PRACTICE", YTR_FLAG_PRACTICE);
    PyModule_AddIntConstant(m, "FLAG_REQUEUED", YTR_FLAG_REQUEUED);
    PyModule_AddIntConstant(m, "FLAG_DONE", YTR_FLAG_DONE);
    PyModule_AddIntConstant(m, "FLAG_WARMUP", YTR_FLAG_WARMUP);
    PyModule_AddIntConstant(m, "FLAG_AFTER_BREAK", YTR_FLAG_AFTER_BREAK);
    PyModule_AddIntConstant(m, "FLAG_FIRST_IN_BLOCK", YTR_FLAG_FIRST_IN_BLOCK);
    PyModule_AddIntConstant(m, "FLAG_VIOLATION", YTR_FLAG_VIOLATION);
    PyModule_AddIntConstant(m, "FLAG_LEADIN", YTR_FLAG_LEADIN);
    PyModule_AddIntConstant(m, "BALANCE_NO_REPEAT", YTR_BALANCE_NO_REPEAT);
    PyModule_AddIntConstant(m, "BALANCE_NO_LEADIN", YTR_BALANCE_NO_LEADIN);
    if (PyModule_AddStringConstant(m, "table_version", ytb_version()) < 0) goto fail;
    return m;

fail:
    Py_XDECREF(mod);
    Py_DECREF(m);
    return NULL;
}
