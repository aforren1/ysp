/* ysp_pack_ext.c - CPython extension wrapping ysp/pack.h (module ysp.pack)
 *
 * A thin, dependency-free binding: it needs only Python.h and compiles the
 * header's implementation (and ysp/json.h's, which ysp/pack.h requires) into
 * the module. Built against the stable ABI / Limited API (CPython 3.10+), so
 * the Pack type is a heap type made with PyType_FromSpec.
 *
 *     import ysp.pack as yp
 *     with yp.Pack("study.ysppak") as pk:
 *         print(pk.id, len(pk), pk.describe())
 *         for e in pk.entries():
 *             print(e.name, e.kind, e.size)
 *         table = pk.read("conditions.pstb")      # bytes, verified
 *         definition = pk.definition()             # the EXPERIMENT entry's bytes
 *         pk.verify()                              # every entry, now
 *
 * COPIES: read() copies an entry once, from the mapping into a new bytes
 * object, so the bytes stay valid after close(). No memoryview of the
 * mapping is handed out: the Limited API before 3.11 has no buffer export
 * that could keep the Pack alive while a view exists, and a view that
 * outlived close() would read unmapped memory.
 *
 * THREADING: the header lets any number of threads call lookups, data and
 * cursors on one open pack. Every call that hashes or copies entry data
 * releases the GIL, so a verify on one thread does not stall the others.
 * close() from another thread while such a call runs only marks the Pack
 * closed; the last running call unmaps it when it returns.
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x030A0000
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

/* Python.h first is safe: pyconfig.h defines _GNU_SOURCE on Linux, which
 * gives the header's mmap() and fstat() without its own feature macro. */
#define YSP_PACK_IMPLEMENTATION
#include "ysp/pack.h"

#include <stdlib.h>
#include <string.h>

static PyObject* PkError;
static PyObject* PkArgumentError;
static PyObject* PkReadError;
static PyObject* PkFormatError;
static PyObject* PkCorruptError;
static PyObject* PkVersionError;
static PyObject* PkNotFoundError;
static PyObject* EntryType;   /* namedtuple Entry(...) */

static const char* const pk_policy_names[3] = { "use", "open", "none" };

typedef struct {
    PyObject_HEAD
    ypak_pack pk;
    PyObject* src;       /* the bytes a memory pack reads, kept until close */
    void*     own;       /* an 8-byte aligned copy when src's bytes were not */
    int       open;      /* 1 between a successful open and close */
    int       busy;      /* calls running without the GIL */
    int       closing;   /* close() came while busy; the last call closes */
    int       policy;    /* the ypak_verify the pack was opened with */
} PackObject;

#define AS_PACK(o) ((PackObject*)(o))

/* --- errors ------------------------------------------------------------------ */

static PyObject* pk_exc(int code) {
    switch (code) {
    case YPAK_ERR_ARG:       return PkArgumentError;
    case YPAK_ERR_IO:        return PkReadError;
    case YPAK_ERR_FORMAT:    return PkFormatError;
    case YPAK_ERR_CORRUPT:   return PkCorruptError;
    case YPAK_ERR_VERSION:   return PkVersionError;
    case YPAK_ERR_NOT_FOUND: return PkNotFoundError;
    case YPAK_ERR_NOMEM:     return PyExc_MemoryError;
    default:                 return PkError;
    }
}

/* The header's message when it belongs to this failure. A failure on
 * another thread can hold the message claim (THREADS AND COST); the
 * message then carries that thread's code, and the code's name is the
 * honest text for this one. */
static PyObject* pk_fail(const ypak_pack* pk, int code) {
    const char* msg = pk && pk->error[0] && pk->error_code == code ? ypak_error(pk) : ypak_strerror(code);
    PyErr_SetString(pk_exc(code), msg);
    return NULL;
}

/* --- lifetime ---------------------------------------------------------------- */

static void pk_release(PackObject* s) {
    ypak_close(&s->pk);
    free(s->own);
    s->own = NULL;
    Py_CLEAR(s->src);
    s->open = 0;
    s->closing = 0;
}

/* Every method that touches the pack starts here. Holds the GIL. */
static int pk_usable(PackObject* s) {
    if (!s->open || s->closing) {
        PyErr_SetString(PkArgumentError, "the pack is closed");
        return 0;
    }
    return 1;
}

/* Before and after a stretch without the GIL; both hold the GIL. */
static void pk_enter(PackObject* s) { s->busy++; }
static void pk_leave(PackObject* s) {
    if (--s->busy == 0 && s->closing) pk_release(s);
}

static int pk_policy(PyObject* o, int* out) {
    char buf[16];
    PyObject* b;
    int i;
    if (!o || o == Py_None) { *out = YPAK_VERIFY_USE; return 0; }
    if (!PyUnicode_Check(o)) {
        PyErr_SetString(PyExc_TypeError, "verify must be \"use\", \"open\" or \"none\"");
        return -1;
    }
    b = PyUnicode_AsUTF8String(o);
    if (!b) return -1;
    PyOS_snprintf(buf, sizeof buf, "%s", PyBytes_AsString(b));
    Py_DECREF(b);
    for (i = 0; i < 3; i++)
        if (strcmp(buf, pk_policy_names[i]) == 0) { *out = i; return 0; }
    PyErr_SetString(PkArgumentError, "verify must be \"use\" (hash an entry on first use), \"open\" (hash every entry at open) or \"none\"");
    return -1;
}

/* Opens s from d with the GIL released: an open hashes the manifest, and
 * with verify="open" every entry. */
static int pk_open_desc(PackObject* s, ypak_desc* d) {
    bool ok;
    s->busy++;   /* keeps a second __init__ on this object out */
    Py_BEGIN_ALLOW_THREADS
    ok = ypak_open(&s->pk, d);
    Py_END_ALLOW_THREADS
    s->busy--;
    if (!ok) {
        pk_fail(&s->pk, s->pk.error_code ? s->pk.error_code : YPAK_ERR_ARG);
        pk_release(s);
        return -1;
    }
    s->open = 1;
    s->policy = (int)d->verify;
    return 0;
}

/* A closed Pack; __init__ (a path) or from_bytes() opens it. */
static PyObject* Pack_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    allocfunc alloc = (allocfunc)PyType_GetSlot(type, Py_tp_alloc);
    PyObject* self = alloc(type, 0);
    (void)args; (void)kwds;
    if (self) {
        PackObject* s = AS_PACK(self);
        memset(&s->pk, 0, sizeof s->pk);
        s->src = NULL; s->own = NULL; s->open = s->busy = s->closing = s->policy = 0;
    }
    return self;
}

static int Pack_init(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "path", "verify", NULL };
    PackObject* s = AS_PACK(self);
    PyObject *path = NULL, *pol = NULL, *enc = NULL;
    ypak_desc d;
    int policy, rc;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O", kw, &path, &pol)) return -1;
    if (s->open || s->busy) {
        PyErr_SetString(PkArgumentError, "this Pack is already open");
        return -1;
    }
    if (PyBytes_Check(path) || PyByteArray_Check(path)) {
        PyErr_SetString(PyExc_TypeError, "path must be a str or os.PathLike; for a pack in memory use Pack.from_bytes()");
        return -1;
    }
    if (pk_policy(pol, &policy) < 0) return -1;
    /* str and PathLike to the file system's bytes: UTF-8 on Windows (PEP 529),
     * which is what the header converts; the original bytes on POSIX. */
    if (!PyUnicode_FSConverter(path, &enc)) return -1;
    memset(&d, 0, sizeof d);
    d.path = PyBytes_AsString(enc);
    d.verify = (ypak_verify)policy;
    rc = pk_open_desc(s, &d);
    Py_DECREF(enc);
    return rc;
}

static PyObject* Pack_from_bytes(PyObject* cls, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "data", "verify", NULL };
    PyObject *data, *pol = NULL, *b, *self;
    PackObject* s;
    ypak_desc d;
    const char* p;
    Py_ssize_t n;
    int policy;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O", kw, &data, &pol)) return NULL;
    if (PyUnicode_Check(data)) {
        PyErr_SetString(PyExc_TypeError, "data must be bytes-like; for a file use Pack(path)");
        return NULL;
    }
    if (pk_policy(pol, &policy) < 0) return NULL;
    /* bytes: the same object, no copy. bytearray, memoryview: one copy into
     * immutable bytes, so the pack cannot change under the reader. */
    b = PyBytes_FromObject(data);
    if (!b) return NULL;
    self = Pack_new((PyTypeObject*)cls, NULL, NULL);
    if (!self) { Py_DECREF(b); return NULL; }
    s = AS_PACK(self);
    s->src = b;
    p = PyBytes_AsString(b);
    n = PyBytes_Size(b);
    if (((uintptr_t)p & 7u) != 0) {   /* the header wants 8-byte aligned memory */
        s->own = malloc(n > 0 ? (size_t)n : 1);
        if (!s->own) { Py_DECREF(self); return PyErr_NoMemory(); }
        memcpy(s->own, p, (size_t)n);
        p = (const char*)s->own;
        Py_CLEAR(s->src);
    }
    memset(&d, 0, sizeof d);
    d.data = p;
    d.size = (size_t)n;
    d.verify = (ypak_verify)policy;
    if (pk_open_desc(s, &d) < 0) { Py_DECREF(self); return NULL; }
    return self;
}


static void Pack_dealloc(PyObject* self) {
    PyTypeObject* tp = Py_TYPE(self);
    freefunc f = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    pk_release(AS_PACK(self));   /* busy is 0: a running method holds a reference */
    f(self);
    Py_DECREF(tp);
}

static PyObject* Pack_close(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    PackObject* s = AS_PACK(self);
    if (s->busy) s->closing = 1;
    else pk_release(s);
    Py_RETURN_NONE;
}

static PyObject* Pack_enter(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    if (!pk_usable(AS_PACK(self))) return NULL;
    Py_INCREF(self);
    return self;
}

static PyObject* Pack_exit(PyObject* self, PyObject* Py_UNUSED(args)) {
    PyObject* r = Pack_close(self, NULL);
    Py_XDECREF(r);
    Py_RETURN_FALSE;
}

/* --- entries ------------------------------------------------------------------- */

static PyObject* pk_entry_obj(const ypak_entry* e) {
    const char* kind = ypak_kind_name(e->kind);
    return PyObject_CallFunction(EntryType, "s#IsLOIKKKk", e->name, (Py_ssize_t)e->name_len, (unsigned int)e->index,
                                 kind ? kind : "unknown", (long long)e->size,
                                 (e->flags & YPAK_F_STREAM) ? Py_True : Py_False, (unsigned int)e->flags,
                                 (unsigned long long)e->header_off, (unsigned long long)e->data_off,
                                 (unsigned long long)e->hash, (unsigned long)e->crc32);
}

/* A name (str) or an Entry from entries() to this pack's entry. Holds the GIL. */
static int pk_lookup(PackObject* s, PyObject* name, ypak_entry* e) {
    PyObject *b, *o = name;
    int rc;
    if (!PyUnicode_Check(o) && PyTuple_Check(o) && PyTuple_Size(o) > 0) o = PyTuple_GetItem(o, 0);   /* an Entry */
    if (!o || !PyUnicode_Check(o)) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "an entry is named by a str (or an Entry)");
        return -1;
    }
    b = PyUnicode_AsUTF8String(o);
    if (!b) return -1;
    rc = ypak_find_n(&s->pk, PyBytes_AsString(b), (size_t)PyBytes_Size(b), e);
    Py_DECREF(b);
    if (rc < 0) { pk_fail(&s->pk, rc); return -1; }
    return 0;
}

static PyObject* Pack_entry(PyObject* self, PyObject* name) {
    ypak_entry e;
    if (!pk_usable(AS_PACK(self)) || pk_lookup(AS_PACK(self), name, &e) < 0) return NULL;
    return pk_entry_obj(&e);
}

static PyObject* Pack_entries(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    PackObject* s = AS_PACK(self);
    PyObject* list;
    uint32_t i;
    if (!pk_usable(s)) return NULL;
    list = PyList_New((Py_ssize_t)s->pk.n);
    if (!list) return NULL;
    for (i = 0; i < s->pk.n; i++) {
        ypak_entry e;
        PyObject* o;
        ypak_at(&s->pk, i, &e);
        o = pk_entry_obj(&e);
        if (!o) { Py_DECREF(list); return NULL; }
        PyList_SetItem(list, (Py_ssize_t)i, o);
    }
    return list;
}

static PyObject* Pack_names(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    PackObject* s = AS_PACK(self);
    PyObject* list;
    uint32_t i;
    if (!pk_usable(s)) return NULL;
    list = PyList_New((Py_ssize_t)s->pk.n);
    if (!list) return NULL;
    for (i = 0; i < s->pk.n; i++) {
        ypak_entry e;
        PyObject* o;
        ypak_at(&s->pk, i, &e);
        o = PyUnicode_FromStringAndSize(e.name, (Py_ssize_t)e.name_len);
        if (!o) { Py_DECREF(list); return NULL; }
        PyList_SetItem(list, (Py_ssize_t)i, o);
    }
    return list;
}

static Py_ssize_t Pack_len(PyObject* self) {
    PackObject* s = AS_PACK(self);
    if (!pk_usable(s)) return -1;
    return (Py_ssize_t)s->pk.n;
}

static int Pack_contains(PyObject* self, PyObject* name) {
    PackObject* s = AS_PACK(self);
    PyObject* b;
    ypak_entry e;
    int rc;
    if (!pk_usable(s)) return -1;
    if (!PyUnicode_Check(name)) return 0;
    b = PyUnicode_AsUTF8String(name);
    if (!b) return -1;
    rc = ypak_find_n(&s->pk, PyBytes_AsString(b), (size_t)PyBytes_Size(b), &e);
    Py_DECREF(b);
    return rc == 0;
}

static PyObject* Pack_iter(PyObject* self) {
    PyObject *names = Pack_names(self, NULL), *it;
    if (!names) return NULL;
    it = PyObject_GetIter(names);
    Py_DECREF(names);
    return it;
}

/* --- data ------------------------------------------------------------------------ */

/* [offset, offset + size) of entry e into a new bytes object: one copy. The
 * whole entry goes through ypak_data() (the entry hash, by the policy); a
 * range through a cursor, which hashes only the 1 MiB chunks it touches. */
static PyObject* pk_read_entry(PackObject* s, const ypak_entry* e, long long offset, long long size) {
    PyObject* out;
    char* dst;
    int64_t n, rc = 0;
    int whole;
    if (offset < 0) { PyErr_SetString(PkArgumentError, "offset must be >= 0"); return NULL; }
    if (size < -1) { PyErr_SetString(PkArgumentError, "size must be >= 0, or -1 for the rest of the entry"); return NULL; }
    n = offset >= e->size ? 0 : e->size - offset;
    if (size >= 0 && size < n) n = size;
    if (n > (int64_t)PY_SSIZE_T_MAX) { PyErr_SetString(PyExc_OverflowError, "the entry is larger than this Python can hold in bytes"); return NULL; }
    whole = offset == 0 && n == e->size;
    out = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)n);
    if (!out) return NULL;
    dst = PyBytes_AsString(out);   /* a new object nobody else sees yet */
    pk_enter(s);
    Py_BEGIN_ALLOW_THREADS
    if (whole) {
        const void* p = ypak_data(&s->pk, e);
        if (p) memcpy(dst, p, (size_t)n);
        else rc = s->pk.error_code ? s->pk.error_code : YPAK_ERR_ARG;
    } else if (n > 0) {
        ypak_cursor c;
        rc = ypak_cursor_init(&c, &s->pk, e, NULL, 0);
        if (rc == 0) {
            int64_t got = ypak_cursor_read(&c, offset, dst, n);
            rc = got < 0 ? got : got != n ? YPAK_ERR_IO : 0;
        }
    }
    Py_END_ALLOW_THREADS
    if (rc < 0) {
        pk_fail(&s->pk, (int)rc);
        pk_leave(s);
        Py_DECREF(out);
        return NULL;
    }
    pk_leave(s);
    return out;
}

static PyObject* Pack_read(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "name", "offset", "size", NULL };
    PackObject* s = AS_PACK(self);
    PyObject* name;
    long long offset = 0, size = -1;
    ypak_entry e;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|LL", kw, &name, &offset, &size)) return NULL;
    if (!pk_usable(s) || pk_lookup(s, name, &e) < 0) return NULL;
    return pk_read_entry(s, &e, offset, size);
}

static PyObject* Pack_getitem(PyObject* self, PyObject* name) {
    PackObject* s = AS_PACK(self);
    ypak_entry e;
    if (!pk_usable(s) || pk_lookup(s, name, &e) < 0) return NULL;
    return pk_read_entry(s, &e, 0, -1);
}

static PyObject* Pack_verify(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "name", NULL };
    PackObject* s = AS_PACK(self);
    PyObject* name = Py_None;
    ypak_entry e;
    int rc = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kw, &name)) return NULL;
    if (!pk_usable(s)) return NULL;
    if (name != Py_None && pk_lookup(s, name, &e) < 0) return NULL;
    pk_enter(s);
    Py_BEGIN_ALLOW_THREADS
    /* both hash whatever the policy, "none" included (ysp/pack.h 0.4.0) */
    if (name == Py_None) rc = ypak_verify_all(&s->pk);
    else rc = ypak_verify_entry(&s->pk, &e);
    Py_END_ALLOW_THREADS
    if (rc < 0) { pk_fail(&s->pk, rc); pk_leave(s); return NULL; }
    pk_leave(s);
    Py_RETURN_NONE;
}

static PyObject* Pack_experiment(PyObject* self, void* Py_UNUSED(c)) {
    PackObject* s = AS_PACK(self);
    ypak_entry e;
    if (!pk_usable(s)) return NULL;
    if (s->pk.experiment < 0 || ypak_at(&s->pk, (uint32_t)s->pk.experiment, &e) < 0) Py_RETURN_NONE;
    return pk_entry_obj(&e);
}

static PyObject* Pack_definition(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    PackObject* s = AS_PACK(self);
    ypak_entry e;
    if (!pk_usable(s)) return NULL;
    if (s->pk.experiment < 0 || ypak_at(&s->pk, (uint32_t)s->pk.experiment, &e) < 0) Py_RETURN_NONE;
    return pk_read_entry(s, &e, 0, -1);
}

/* The manifest through the standard library's json: the header does not
 * parse it (docs/pack.md 3), and its one form needs nothing more. */
static PyObject* Pack_manifest(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    PackObject* s = AS_PACK(self);
    ypak_entry e;
    PyObject *b, *json, *r;
    if (!pk_usable(s)) return NULL;
    ypak_at(&s->pk, 0, &e);   /* entry 0 is the manifest, hashed against the pack ID at open */
    b = pk_read_entry(s, &e, 0, -1);
    if (!b) return NULL;
    json = PyImport_ImportModule("json");
    r = json ? PyObject_CallMethod(json, "loads", "O", b) : NULL;
    Py_XDECREF(json);
    Py_DECREF(b);
    return r;
}

static PyObject* Pack_describe(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    PackObject* s = AS_PACK(self);
    char buf[512];
    if (!pk_usable(s)) return NULL;
    ypak_describe(&s->pk, buf, sizeof buf);
    return PyUnicode_FromString(buf);
}

/* --- properties -------------------------------------------------------------- */

static PyObject* Pack_get_id(PyObject* self, void* Py_UNUSED(c)) {
    char id[72];
    if (!pk_usable(AS_PACK(self))) return NULL;
    ypak_id(&AS_PACK(self)->pk, id);
    return PyUnicode_FromString(id);
}

#define PK_GETTER(fname, expr)                                              \
    static PyObject* fname(PyObject* self, void* Py_UNUSED(c)) {            \
        const ypak_pack* p = &AS_PACK(self)->pk;                          \
        if (!pk_usable(AS_PACK(self))) return NULL;                         \
        return expr;                                                        \
    }
PK_GETTER(Pack_get_format, PyLong_FromLong(p->format))
PK_GETTER(Pack_get_file_size, PyLong_FromLongLong(p->file_size))
PK_GETTER(Pack_get_zip_start, PyLong_FromLongLong(p->zip_start))
PK_GETTER(Pack_get_n_chunks, PyLong_FromUnsignedLong(p->n_chunks))
PK_GETTER(Pack_get_hashed_bytes, PyLong_FromUnsignedLongLong(p->hashed_bytes))
PK_GETTER(Pack_get_policy, ((void)p, PyUnicode_FromString(pk_policy_names[AS_PACK(self)->policy])))
#undef PK_GETTER

static PyObject* Pack_get_closed(PyObject* self, void* Py_UNUSED(c)) {
    PackObject* s = AS_PACK(self);
    return PyBool_FromLong(!s->open || s->closing);
}

static PyMethodDef Pack_methods[] = {
    { "from_bytes", (PyCFunction)(void (*)(void))Pack_from_bytes, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
      "Pack.from_bytes(data, verify=\"use\") -> a Pack read from memory. bytes are kept, not copied; "
      "bytearray and memoryview are copied once." },
    { "close", Pack_close, METH_NOARGS, "close() -> unmaps the file. Safe twice. Bytes read() returned stay valid." },
    { "entries", Pack_entries, METH_NOARGS, "entries() -> every Entry in pack order; the manifest is first." },
    { "names", Pack_names, METH_NOARGS, "names() -> every entry's name in pack order." },
    { "entry", Pack_entry, METH_O, "entry(name) -> the Entry; NotFoundError when absent." },
    { "read", (PyCFunction)(void (*)(void))Pack_read, METH_VARARGS | METH_KEYWORDS,
      "read(name, offset=0, size=-1) -> bytes. The whole entry is hashed on first use (verify=\"use\"); "
      "a range hashes the 1 MiB chunks it touches. CorruptError names the entry and the chunk." },
    { "verify", (PyCFunction)(void (*)(void))Pack_verify, METH_VARARGS | METH_KEYWORDS,
      "verify(name=None) -> None. Hashes every entry now (ypak_verify_all), or one (ypak_verify_entry), "
      "whatever the policy; raises on a mismatch." },
    { "definition", Pack_definition, METH_NOARGS,
      "definition() -> the EXPERIMENT entry's bytes (a definition's canonical bytes), or None." },
    { "manifest", Pack_manifest, METH_NOARGS, "manifest() -> ysp/manifest.json parsed with the json module." },
    { "describe", Pack_describe, METH_NOARGS, "describe() -> one line for a log: format, pack ID, entries, policy." },
    { "__enter__", Pack_enter, METH_NOARGS, NULL },
    { "__exit__", Pack_exit, METH_VARARGS, NULL },
    { NULL }
};

static PyGetSetDef Pack_getset[] = {
    { "id", Pack_get_id, NULL, "The pack ID, \"sha256:<64 hex>\": the SHA-256 of the manifest.", NULL },
    { "format", Pack_get_format, NULL, "The pack format version.", NULL },
    { "file_size", Pack_get_file_size, NULL, "Bytes in the file (or in memory).", NULL },
    { "zip_start", Pack_get_zip_start, NULL, "Offset of the first local header: 0, or past a player the pack was appended to.", NULL },
    { "n_chunks", Pack_get_n_chunks, NULL, "Chunk hashes in ysp/chunks.", NULL },
    { "hashed_bytes", Pack_get_hashed_bytes, NULL, "Entry bytes hashed so far (approximate while threads hash).", NULL },
    { "policy", Pack_get_policy, NULL, "The verify policy: \"use\", \"open\" or \"none\".", NULL },
    { "experiment", Pack_experiment, NULL, "The EXPERIMENT Entry, or None.", NULL },
    { "closed", Pack_get_closed, NULL, "True after close().", NULL },
    { NULL }
};

static PyType_Slot Pack_slots[] = {
    { Py_tp_new, (void*)Pack_new },
    { Py_tp_init, (void*)Pack_init },
    { Py_tp_dealloc, (void*)Pack_dealloc },
    { Py_tp_methods, Pack_methods },
    { Py_tp_getset, Pack_getset },
    { Py_tp_iter, (void*)Pack_iter },
    { Py_mp_length, (void*)Pack_len },
    { Py_mp_subscript, (void*)Pack_getitem },
    { Py_sq_contains, (void*)Pack_contains },
    { Py_tp_doc, (void*)"Pack(path, verify=\"use\"): a ysp pack, mapped read-only (ypak_pack). "
                        "verify: \"use\" hashes an entry on first read, \"open\" every entry at open, "
                        "\"none\" none after open. pk[name] is pk.read(name)." },
    { 0, NULL }
};
static PyType_Spec Pack_spec = { "ysp.pack.Pack", sizeof(PackObject), 0, Py_TPFLAGS_DEFAULT, Pack_slots };

/* --- module functions --------------------------------------------------------- */

static PyObject* mod_xxh64(PyObject* m, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "data", "seed", NULL };
    PyObject *data, *b;
    unsigned long long seed = 0, h;
    const char* p;
    Py_ssize_t n;
    (void)m;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|K", kw, &data, &seed)) return NULL;
    if (PyUnicode_Check(data)) { PyErr_SetString(PyExc_TypeError, "data must be bytes-like"); return NULL; }
    if (!(b = PyBytes_FromObject(data))) return NULL;
    p = PyBytes_AsString(b);
    n = PyBytes_Size(b);
    Py_BEGIN_ALLOW_THREADS
    h = ypak_xxh64(p, (size_t)n, seed);
    Py_END_ALLOW_THREADS
    Py_DECREF(b);
    return PyLong_FromUnsignedLongLong(h);
}

static PyObject* mod_entry_hash(PyObject* m, PyObject* data) {
    PyObject* b;
    const char* p;
    Py_ssize_t n;
    uint64_t h;
    (void)m;
    if (PyUnicode_Check(data)) { PyErr_SetString(PyExc_TypeError, "data must be bytes-like"); return NULL; }
    if (!(b = PyBytes_FromObject(data))) return NULL;
    p = PyBytes_AsString(b);
    n = PyBytes_Size(b);
    Py_BEGIN_ALLOW_THREADS
    h = ypak_entry_hash(p, (int64_t)n, NULL);
    Py_END_ALLOW_THREADS
    Py_DECREF(b);
    return PyLong_FromUnsignedLongLong(h);
}

static PyObject* mod_version(PyObject* m, PyObject* Py_UNUSED(ignored)) {
    (void)m;
    return PyUnicode_FromString(YPAK_VERSION_STRING);
}

static PyMethodDef module_methods[] = {
    { "xxh64", (PyCFunction)(void (*)(void))mod_xxh64, METH_VARARGS | METH_KEYWORDS, "xxh64(data, seed=0) -> int." },
    { "entry_hash", mod_entry_hash, METH_O,
      "entry_hash(data) -> int: the pack's entry hash (XXH64 over 1 MiB chunk hashes), the manifest's \"xxh64\"." },
    { "version", mod_version, METH_NOARGS, "version() -> the header's version string." },
    { NULL }
};

static PyModuleDef ysp_pack_module = {
    PyModuleDef_HEAD_INIT, "ysp.pack",
    "Read and verify ysp packs: store-only zip, 4 KiB aligned, XXH64 chunks, SHA-256 manifest (ysp/pack.h).", -1,
    module_methods, NULL, NULL, NULL, NULL,
};

static PyObject* pk_add_exc(PyObject* m, const char* qual, const char* attr, PyObject* base, const char* doc) {
    PyObject* e = PyErr_NewExceptionWithDoc(qual, doc, base, NULL);
    if (!e) return NULL;
    Py_INCREF(e);
    if (PyModule_AddObject(m, attr, e) < 0) { Py_DECREF(e); Py_DECREF(e); return NULL; }
    return e;
}

static PyObject* pk_add_exc2(PyObject* m, const char* qual, const char* attr, PyObject* std, const char* doc) {
    PyObject *bases = PyTuple_Pack(2, PkError, std), *e;
    if (!bases) return NULL;
    e = pk_add_exc(m, qual, attr, bases, doc);
    Py_DECREF(bases);
    return e;
}

PyMODINIT_FUNC PyInit_pack(void) {
    PyObject *m, *t, *coll = NULL, *nt = NULL, *a = NULL, *k = NULL;
    m = PyModule_Create(&ysp_pack_module);
    if (!m) return NULL;
    if (!(t = PyType_FromSpec(&Pack_spec))) goto fail;
    if (PyModule_AddObject(m, "Pack", t) < 0) { Py_DECREF(t); goto fail; }
    if (!(PkError = pk_add_exc(m, "ysp.pack.Error", "Error", NULL, "A ysp/pack.h failure; the message is the header's."))) goto fail;
    if (!(PkArgumentError = pk_add_exc2(m, "ysp.pack.ArgumentError", "ArgumentError", PyExc_ValueError, "YPAK_ERR_ARG, or a closed pack."))) goto fail;
    if (!(PkReadError = pk_add_exc2(m, "ysp.pack.ReadError", "ReadError", PyExc_OSError, "YPAK_ERR_IO: the file cannot be opened, mapped or read."))) goto fail;
    if (!(PkFormatError = pk_add_exc2(m, "ysp.pack.FormatError", "FormatError", PyExc_ValueError, "YPAK_ERR_FORMAT: outside the format; the message names the rule."))) goto fail;
    if (!(PkCorruptError = pk_add_exc2(m, "ysp.pack.CorruptError", "CorruptError", PyExc_ValueError, "YPAK_ERR_CORRUPT: a hash does not match."))) goto fail;
    if (!(PkVersionError = pk_add_exc2(m, "ysp.pack.VersionError", "VersionError", PyExc_ValueError, "YPAK_ERR_VERSION: another pack format version."))) goto fail;
    if (!(PkNotFoundError = pk_add_exc2(m, "ysp.pack.NotFoundError", "NotFoundError", PyExc_KeyError, "YPAK_ERR_NOT_FOUND: no entry of that name."))) goto fail;
    coll = PyImport_ImportModule("collections");
    if (coll) nt = PyObject_GetAttrString(coll, "namedtuple");
    a = Py_BuildValue("(ss)", "Entry", "name index kind size stream flags header_off data_off xxh64 crc32");
    k = Py_BuildValue("{s:s}", "module", "ysp.pack");
    if (nt && a && k) EntryType = PyObject_Call(nt, a, k);
    Py_XDECREF(coll); Py_XDECREF(nt); Py_XDECREF(a); Py_XDECREF(k);
    if (!EntryType) goto fail;
    Py_INCREF(EntryType);
    if (PyModule_AddObject(m, "Entry", EntryType) < 0) { Py_DECREF(EntryType); goto fail; }
    if (PyModule_AddStringConstant(m, "__version__", YPAK_VERSION_STRING ".1") < 0) goto fail;   /* .1: the CPython 3.10 floor (2026-10-27) */
    PyModule_AddIntConstant(m, "FORMAT", YPAK_FORMAT);
    PyModule_AddIntConstant(m, "ALIGN", YPAK_ALIGN);
    PyModule_AddIntConstant(m, "CHUNK", (long)YPAK_CHUNK);
    PyModule_AddIntConstant(m, "F_STREAM", YPAK_F_STREAM);
    PyModule_AddStringConstant(m, "MANIFEST", YPAK_MANIFEST);
    PyModule_AddStringConstant(m, "CHUNKS", YPAK_CHUNKS);
    return m;
fail:
    Py_DECREF(m);
    return NULL;
}
