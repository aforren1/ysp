/* ysp_rigfile_ext.c - CPython extension wrapping ysp/rigfile.h (module ysp.rigfile)
 *
 *     import ysp.rigfile as yrig, ysp.device as yd
 *     prof = yrig.load()                       # the user's profile.json
 *     for note in prof.notes: print(note)     # what the checks found
 *     print(prof.hash)                         # for the data file's header
 *     trig = yd.Device(**prof.desc("trig"))    # the role's device
 *     resp = yd.Device(**prof.desc("resp"))
 *     print(prof.source(resp)["tier"])         # TIER_1 only with checked bounds
 *
 * The rig profile is one distribution and the device layer another: a
 * role's device opens through Profile.desc(), a dict of ysp.device.Device's
 * keyword arguments (yrig_desc()), so neither extension links the other.
 * yrig_open_device() is yrig_desc() and ydev_open() with the role in the
 * message, and is not bound: Device(**prof.desc(role)) is the same call.
 *
 * UNITS: every time is float seconds; the C struct keeps int64 ns. Lengths
 * are mm, as in the file.
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x030A0000
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define YSP_RIGFILE_IMPLEMENTATION
#include "ysp/rigfile.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#define RG_VERSION YRIG_VERSION_STRING ".1"   /* .1: the CPython 3.10 floor (2026-10-27) */

static PyObject* RgError;
static PyObject* RgMissing;
static PyObject* RgFolder;
static PyObject* RgProfileType;

typedef struct {
    PyObject_HEAD
    yrig_profile* p;      /* calloc'd: about 50 KB */
} ProfileObject;

#define PROF(self) (((ProfileObject*)(self))->p)

/* An error with (message, code) as its args: Missing for no file, Folder
 * for a refused rig folder, Error for the rest. */
static PyObject* rg_fail(int code, const char* msg) {
    PyObject* exc = code == YRIG_ERR_MISSING ? RgMissing : code == YRIG_ERR_FOLDER ? RgFolder : RgError;
    PyObject* v = Py_BuildValue("(si)", msg && msg[0] ? msg : "ysp_rig: error", code);
    if (v) { PyErr_SetObject(exc, v); Py_DECREF(v); }
    return NULL;
}

static PyObject* rg_s(int64_t ns) { return PyFloat_FromDouble((double)ns / 1e9); }

static PyObject* rg_new_profile(void) {
    PyObject* o = PyObject_CallObject(RgProfileType, NULL);
    return o;
}

/* ---- Profile ---------------------------------------------------------------- */

static int Profile_init(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "rig", NULL };
    const char* rig = "rig";
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|s", kw, &rig)) return -1;
    if (!PROF(self)) {
        ((ProfileObject*)self)->p = (yrig_profile*)calloc(1, sizeof(yrig_profile));
        if (!PROF(self)) { PyErr_NoMemory(); return -1; }
    }
    if (strlen(rig) < 1 || strlen(rig) > 63) { PyErr_SetString(PyExc_ValueError, "rig: 1 to 63 bytes"); return -1; }
    yrig_init(PROF(self), rig);
    return 0;
}

static void Profile_dealloc(PyObject* self) {
    PyTypeObject* tp = Py_TYPE(self);
    freefunc f = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    free(PROF(self));
    ((ProfileObject*)self)->p = NULL;
    f(self);
    Py_DECREF(tp);
}

static yrig_profile* rg_prof(PyObject* self) {
    if (!PROF(self)) PyErr_SetString(RgError, "Profile: not initialized");
    return PROF(self);
}

static PyObject* rg_lb(const yrig_loopback* lb, int latency) {
    if (!lb->set) Py_RETURN_NONE;
    if (latency)
        return Py_BuildValue("{s:i,s:N,s:N,s:N,s:N,s:s,s:s,s:i,s:s}", "n", (int)lb->n, "p5", rg_s(lb->p5_ns),
                             "median", rg_s(lb->median_ns), "p95", rg_s(lb->p95_ns), "max", rg_s(lb->max_ns),
                             "date", lb->date, "sha256", lb->sha256, "check", lb->check,
                             "check_name", yrig_check_name(lb->check));
    return Py_BuildValue("{s:i,s:N,s:N,s:L,s:L,s:s,s:s,s:i,s:s}", "n", (int)lb->n, "lo", rg_s(lb->lo_ns),
                         "hi", rg_s(lb->hi_ns), "lo_ns", (long long)lb->lo_ns, "hi_ns", (long long)lb->hi_ns,
                         "date", lb->date, "sha256", lb->sha256, "check", lb->check,
                         "check_name", yrig_check_name(lb->check));
}

static PyObject* rg_opt_s(int64_t ns) {
    if (ns < 0) Py_RETURN_NONE;
    return rg_s(ns);
}

static PyObject* rg_role(const yrig_role* r) {
    PyObject* buttons = PyDict_New();
    int i;
    if (!buttons) return NULL;
    for (i = 0; i < r->n_buttons && i < YRIG_MAX_BUTTONS; i++) {
        PyObject* v = PyUnicode_FromString(r->buttons[i].name);
        if (!v || PyDict_SetItemString(buttons, r->buttons[i].code, v) < 0) {
            Py_XDECREF(v);
            Py_DECREF(buttons);
            return NULL;
        }
        Py_DECREF(v);
    }
    return Py_BuildValue("{s:s,s:s,s:i,s:s,s:k,s:O,s:N,s:N,s:N,s:N,s:N}", "name", r->name, "family", r->family,
                         "family_id", r->family_id, "key", r->key, "baud", (unsigned long)r->baud,
                         "latched", r->latched ? Py_True : Py_False, "ftdi_latency", rg_opt_s(r->ftdi_latency_ns),
                         "pulse", rg_opt_s(r->pulse_ns), "buttons", buttons, "latency", rg_lb(&r->latency, 1),
                         "bounds", rg_lb(&r->bounds, 0));
}

static PyObject* Profile_role(PyObject* self, PyObject* args) {
    const char* name;
    yrig_profile* p = rg_prof(self);
    const yrig_role* r;
    if (!p || !PyArg_ParseTuple(args, "s", &name)) return NULL;
    r = yrig_find(p, name);
    if (!r) {
        char msg[200];
        snprintf(msg, sizeof msg, "the rig profile \"%s\" has no role \"%.31s\"", p->rig, name);
        return rg_fail(YRIG_ERR_MISSING, msg);
    }
    return rg_role(r);
}

/* The keyword arguments of ysp.device.Device for a role (yrig_desc()). */
static PyObject* Profile_desc(PyObject* self, PyObject* args) {
    const char* name;
    yrig_profile* p = rg_prof(self);
    ydev_desc d;
    char err[256];
    int rc;
    PyObject* out;
    if (!p || !PyArg_ParseTuple(args, "s", &name)) return NULL;
    memset(&d, 0, sizeof d);
    err[0] = '\0';
    rc = yrig_desc(p, name, &d, err, sizeof err);
    if (rc != YRIG_OK) return rg_fail(rc, err);
    out = Py_BuildValue("{s:s,s:i,s:s,s:k,s:O}", "role", d.role, "family", d.family, "key", d.key,
                        "baud", (unsigned long)d.baud, "latched", d.latched ? Py_True : Py_False);
    if (out && d.pulse_ns > 0) {
        PyObject* v = rg_s(d.pulse_ns);
        if (!v || PyDict_SetItemString(out, "pulse", v) < 0) { Py_XDECREF(v); Py_DECREF(out); return NULL; }
        Py_DECREF(v);
    }
    return out;
}

static PyObject* rg_source(const yin_source* s) {
    return Py_BuildValue("{s:i,s:i,s:i,s:k,s:N,s:N,s:L,s:L,s:s}", "kind", (int)s->kind, "tier", (int)s->tier,
                         "partial", (int)s->partial, "device", (unsigned long)s->device, "lo", rg_s(s->lo_ns),
                         "hi", rg_s(s->hi_ns), "lo_ns", (long long)s->lo_ns, "hi_ns", (long long)s->hi_ns,
                         "note", s->note ? s->note : "");
}

static int rg_attr_long(PyObject* o, const char* name, long* out) {
    PyObject* v = PyObject_GetAttrString(o, name);
    if (!v) return -1;
    *out = PyLong_AsLong(v);
    Py_DECREF(v);
    return (*out == -1 && PyErr_Occurred()) ? -1 : 0;
}

/* source(role) or source(device): the tier rule (yrig_source_role(),
 * yrig_source()). A device is anything with role, family, kind and device
 * attributes (ysp.device.Device): its source entry is rebuilt from them in a
 * zeroed ydev_device, as ydev_source() reads only those fields. */
static PyObject* Profile_source(PyObject* self, PyObject* arg) {
    yrig_profile* p = rg_prof(self);
    yin_source s;
    if (!p) return NULL;
    if (PyUnicode_Check(arg)) {
        PyObject* b = PyUnicode_AsUTF8String(arg);
        if (!b) return NULL;
        memset(&s, 0, sizeof s);
        s = yrig_source_role(p, PyBytes_AsString(b), s);
        Py_DECREF(b);
        return rg_source(&s);
    }
    {
        PyObject *role_o, *b;
        long fam, kind, device;
        ydev_device* dev;
        char role[YDEV_ROLE_MAX];
        role_o = PyObject_GetAttrString(arg, "role");
        if (!role_o) {
            if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                PyErr_Clear();
                PyErr_SetString(PyExc_TypeError, "source: a role name or a ysp.device.Device");
            }
            return NULL;
        }
        b = PyUnicode_Check(role_o) ? PyUnicode_AsUTF8String(role_o) : NULL;
        Py_DECREF(role_o);
        if (!b) {
            if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "source: a role name or a ysp.device.Device");
            return NULL;
        }
        snprintf(role, sizeof role, "%s", PyBytes_AsString(b));
        Py_DECREF(b);
        if (rg_attr_long(arg, "family", &fam) < 0 || rg_attr_long(arg, "kind", &kind) < 0 ||
            rg_attr_long(arg, "device", &device) < 0)
            return NULL;
        dev = (ydev_device*)calloc(1, sizeof(ydev_device));
        if (!dev) return PyErr_NoMemory();
        dev->d.role = role;
        dev->d.family = (int)fam;
        dev->d.kind = (int)kind;
        dev->d.device = (uint32_t)device;
        s = yrig_source(p, dev);
        free(dev);
        return rg_source(&s);
    }
}

static PyObject* Profile_bind(PyObject* self, PyObject* args) {
    const char *role, *key;
    PyObject* fam_o;
    int fam = 0, rc;
    yrig_profile* p = rg_prof(self);
    if (!p || !PyArg_ParseTuple(args, "sOs", &role, &fam_o, &key)) return NULL;
    if (PyLong_Check(fam_o)) {
        fam = (int)PyLong_AsLong(fam_o);
        if (fam == -1 && PyErr_Occurred()) return NULL;
    } else if (PyUnicode_Check(fam_o)) {
        PyObject* b = PyUnicode_AsUTF8String(fam_o);
        const char* n;
        int f;
        if (!b) return NULL;
        n = PyBytes_AsString(b);
        if (strcmp(n, "lsl") == 0) fam = YRIG_FAMILY_LSL;
        for (f = YBOX_XID; f <= YBOX_FAMILY_LAST && !fam; f++)
            if (strcmp(n, ybox_family_name(f)) == 0) fam = f;
        Py_DECREF(b);
    }
    if (!fam) { PyErr_SetString(PyExc_ValueError, "family: a ysp/box.h family name or number, or 'lsl'"); return NULL; }
    rc = yrig_bind(p, role, fam, key);
    if (rc == YRIG_ERR_FULL) return rg_fail(rc, "bind: the profile has 32 roles (YRIG_MAX_ROLES)");
    if (rc != YRIG_OK)
        return rg_fail(rc, "bind: a role name of 1 to 31 of A-Z a-z 0-9 _ . -, a key below 160 bytes "
                           "without a quote, and 'lsl:' keys for the lsl family only");
    Py_RETURN_NONE;
}

static PyObject* Profile_remove(PyObject* self, PyObject* args) {
    const char* role;
    yrig_profile* p = rg_prof(self);
    if (!p || !PyArg_ParseTuple(args, "s", &role)) return NULL;
    return PyBool_FromLong(yrig_remove(p, role));
}

static PyObject* Profile_set_view(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "distance_mm", "width_mm", "width_px", NULL };
    double dist, width;
    long px;
    yrig_profile* p = rg_prof(self);
    if (!p || !PyArg_ParseTupleAndKeywords(args, kwds, "ddl", kw, &dist, &width, &px)) return NULL;
    /* the file's bounds (VIEWING GEOMETRY): refused here, before a save
     * would refuse the whole profile */
    if (!(dist > 0 && dist <= 100000) || !(width > 0 && width <= 100000) || px < 1 || px > 100000) {
        PyErr_SetString(PyExc_ValueError, "set_view: distance_mm and width_mm above 0 and at most 100000, "
                                          "width_px 1 to 100000");
        return NULL;
    }
    p->view.set = true;
    p->view.distance_mm = dist;
    p->view.width_mm = width;
    p->view.width_px = (int32_t)px;
    Py_RETURN_NONE;
}

static PyObject* Profile_clear_view(PyObject* self, PyObject* Py_UNUSED(a)) {
    yrig_profile* p = rg_prof(self);
    if (!p) return NULL;
    memset(&p->view, 0, sizeof p->view);
    Py_RETURN_NONE;
}

static PyObject* Profile_save(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "name", NULL };
    const char* name = NULL;
    char err[512];
    int rc;
    yrig_profile* p = rg_prof(self);
    if (!p || !PyArg_ParseTupleAndKeywords(args, kwds, "|z", kw, &name)) return NULL;
    err[0] = '\0';
    rc = yrig_save(p, name, err, sizeof err);
    if (rc != YRIG_OK) return rg_fail(rc, err);
    Py_RETURN_NONE;
}

static PyObject* Profile_save_file(PyObject* self, PyObject* args) {
    const char* path;
    char err[512];
    int rc;
    yrig_profile* p = rg_prof(self);
    if (!p || !PyArg_ParseTuple(args, "s", &path)) return NULL;
    err[0] = '\0';
    rc = yrig_save_file(p, path, err, sizeof err);
    if (rc != YRIG_OK) return rg_fail(rc, err);
    Py_RETURN_NONE;
}

static PyObject* Profile_get_text(PyObject* self, void* c) {
    yrig_profile* p = rg_prof(self);
    size_t n;
    char* buf;
    PyObject* s;
    (void)c;
    if (!p) return NULL;
    n = yrig_write(p, NULL, 0);
    if (n == 0) return rg_fail(YRIG_ERR_FORMAT, "text: the profile has a value the format cannot hold");
    buf = (char*)malloc(n + 1);
    if (!buf) return PyErr_NoMemory();
    if (yrig_write(p, buf, n + 1) != n) { free(buf); return rg_fail(YRIG_ERR_FORMAT, "text: write failed"); }
    s = PyUnicode_FromStringAndSize(buf, (Py_ssize_t)n);
    free(buf);
    return s;
}

static PyObject* Profile_get_hash(PyObject* self, void* c) {
    char hex[65];
    yrig_profile* p = rg_prof(self);
    (void)c;
    if (!p) return NULL;
    if (yrig_hash(p, hex) != YRIG_OK) return rg_fail(YRIG_ERR_FORMAT, "hash: the profile has a value the format cannot hold");
    return PyUnicode_FromString(hex);
}

static PyObject* Profile_get_notes(PyObject* self, void* c) {
    yrig_profile* p = rg_prof(self);
    PyObject* l;
    int i;
    (void)c;
    if (!p) return NULL;
    l = PyList_New(0);
    for (i = 0; l && i < p->n_notes && i < YRIG_MAX_NOTES; i++) {
        PyObject* s = PyUnicode_FromString(p->notes[i]);
        if (!s || PyList_Append(l, s) < 0) { Py_XDECREF(s); Py_CLEAR(l); break; }
        Py_DECREF(s);
    }
    return l;
}

static PyObject* Profile_get_roles(PyObject* self, void* c) {
    yrig_profile* p = rg_prof(self);
    PyObject* l;
    int i;
    (void)c;
    if (!p) return NULL;
    l = PyList_New(0);
    for (i = 0; l && i < p->n_roles && i < YRIG_MAX_ROLES; i++) {
        PyObject* s = PyUnicode_FromString(p->role[i].name);
        if (!s || PyList_Append(l, s) < 0) { Py_XDECREF(s); Py_CLEAR(l); break; }
        Py_DECREF(s);
    }
    return l;
}

static PyObject* Profile_get_display(PyObject* self, void* c) {
    yrig_profile* p = rg_prof(self);
    const yrig_display* d;
    (void)c;
    if (!p) return NULL;
    d = &p->display;
    if (!d->set) Py_RETURN_NONE;
    return Py_BuildValue("{s:N,s:L,s:N,s:s,s:s,s:i,s:s}", "onset_offset", rg_s(d->onset_offset_ns),
                         "onset_offset_ns", (long long)d->onset_offset_ns, "latency", rg_lb(&d->latency, 1),
                         "calibration", d->calibration, "calibration_sha256", d->calibration_sha256,
                         "calibration_check", d->check, "calibration_check_name", yrig_check_name(d->check));
}

static PyObject* Profile_get_view(PyObject* self, void* c) {
    yrig_profile* p = rg_prof(self);
    (void)c;
    if (!p) return NULL;
    if (!p->view.set) Py_RETURN_NONE;
    return Py_BuildValue("{s:d,s:d,s:i}", "distance_mm", p->view.distance_mm, "width_mm", p->view.width_mm,
                         "width_px", (int)p->view.width_px);
}

static PyObject* Profile_get_ppd(PyObject* self, void* c) {
    yrig_profile* p = rg_prof(self);
    (void)c;
    return p ? PyFloat_FromDouble(yrig_ppd(p)) : NULL;
}

#define RG_STR(field, doc)                                                      \
    static PyObject* Profile_get_##field(PyObject* self, void* c) {             \
        yrig_profile* p = rg_prof(self);                                        \
        (void)c;                                                                \
        return p ? PyUnicode_FromString(p->field) : NULL;                       \
    }
RG_STR(rig, "")
RG_STR(written, "")
RG_STR(dir, "")

static PyGetSetDef Profile_getset[] = {
    { "rig", Profile_get_rig, NULL, "The rig's name.", NULL },
    { "written", Profile_get_written, NULL, "When it was last written (UTC, ISO 8601).", NULL },
    { "dir", Profile_get_dir, NULL, "The folder of its loopback/ files; '' when parsed without one.", NULL },
    { "notes", Profile_get_notes, NULL,
      "list[str]: what the checks at load found (a missing, changed or differing loopback file). Log them.", NULL },
    { "roles", Profile_get_roles, NULL, "list[str]: the role names, in file order.", NULL },
    { "display", Profile_get_display, NULL,
      "dict or None: onset_offset (seconds; yscr_desc.onset_offset_ns), onset_offset_ns, latency (the photodiode "
      "run, or None), calibration (a file name), calibration_sha256, calibration_check(_name).", NULL },
    { "view", Profile_get_view, NULL, "dict or None: distance_mm, width_mm, width_px (VIEWING GEOMETRY).", NULL },
    { "ppd", Profile_get_ppd, NULL, "Pixels per degree at the screen's center from view; 0 without one.", NULL },
    { "text", Profile_get_text, NULL, "The canonical JSON text (yrig_write).", NULL },
    { "hash", Profile_get_hash, NULL, "The SHA-256 of the canonical text, hex: name it in the data file's header.", NULL },
    { NULL }
};

static PyMethodDef Profile_methods[] = {
    { "role", Profile_role, METH_VARARGS,
      "role(name) -> dict: name, family ('xid', ..., 'lsl'), family_id, key, baud (0: the family's), latched, "
      "ftdi_latency and pulse (seconds or None), buttons {code: name}, latency and bounds (dicts with times in "
      "seconds and check/check_name, or None). Raises Missing for no such role." },
    { "desc", Profile_desc, METH_VARARGS,
      "desc(role) -> dict: the keyword arguments of ysp.device.Device for the role (role, family, key, baud, "
      "latched, and pulse when the rig fixes it): ysp.device.Device(**prof.desc('trig')). Raises Missing for no "
      "such role, Error for an unbound role or an lsl role (start it with ysp/net.h)." },
    { "source", Profile_source, METH_O,
      "source(role or device) -> dict: kind, tier, partial, device, lo, hi, lo_ns, hi_ns, note. TIER_1 with the "
      "role's bounds only when its bounds file checked and its family maps a device clock; else TIER_UNKNOWN. "
      "With a ysp.device.Device, the device's family must be the role's." },
    { "bind", Profile_bind, METH_VARARGS,
      "bind(role, family, key): bind a role (added when new) to a device; drops its latency and bounds, which "
      "belonged to the old device. family: a name, a number, or 'lsl'." },
    { "remove", Profile_remove, METH_VARARGS, "remove(role) -> bool." },
    { "set_view", (PyCFunction)(void (*)(void))Profile_set_view, METH_VARARGS | METH_KEYWORDS,
      "set_view(distance_mm, width_mm, width_px): the viewing geometry. It changes the hash." },
    { "clear_view", Profile_clear_view, METH_NOARGS, "clear_view(): no viewing geometry." },
    { "save", (PyCFunction)(void (*)(void))Profile_save, METH_VARARGS | METH_KEYWORDS,
      "save(name=None): set written to now and write the profile to the user's rig folder (profile.json, or "
      "<name>.json), through a temporary file. Raises Folder when the folder is refused." },
    { "save_file", Profile_save_file, METH_VARARGS, "save_file(path): the same, to a path of your own." },
    { NULL }
};

static PyType_Slot Profile_slots[] = {
    { Py_tp_doc, (void*)"Profile(rig='rig'): a rig profile (ysp/rigfile.h). Profile(name) makes an empty one; "
                        "ysp.rigfile.load(), load_file() and parse() read one." },
    { Py_tp_new, (void*)PyType_GenericNew },
    { Py_tp_init, (void*)Profile_init },
    { Py_tp_dealloc, (void*)Profile_dealloc },
    { Py_tp_methods, Profile_methods },
    { Py_tp_getset, Profile_getset },
    { 0, NULL }
};

static PyType_Spec Profile_spec = { "ysp.rigfile.Profile", sizeof(ProfileObject), 0, Py_TPFLAGS_DEFAULT, Profile_slots };

/* ---- module functions ----------------------------------------------------- */

static PyObject* mod_load(PyObject* m, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "name", NULL };
    const char* name = NULL;
    char err[512];
    int rc;
    PyObject* o;
    (void)m;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|z", kw, &name)) return NULL;
    o = rg_new_profile();
    if (!o) return NULL;
    err[0] = '\0';
    rc = yrig_load(PROF(o), name, err, sizeof err);
    if (rc != YRIG_OK) { Py_DECREF(o); return rg_fail(rc, err); }
    return o;
}

static PyObject* mod_load_file(PyObject* m, PyObject* args) {
    const char* path;
    char err[512];
    int rc;
    PyObject* o;
    (void)m;
    if (!PyArg_ParseTuple(args, "s", &path)) return NULL;
    o = rg_new_profile();
    if (!o) return NULL;
    err[0] = '\0';
    rc = yrig_load_file(PROF(o), path, err, sizeof err);
    if (rc != YRIG_OK) { Py_DECREF(o); return rg_fail(rc, err); }
    return o;
}

static PyObject* mod_parse(PyObject* m, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "text", "dir", NULL };
    const char* text;
    Py_ssize_t n;
    const char* dir = NULL;
    char err[512];
    int rc;
    PyObject* o;
    (void)m;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s#|z", kw, &text, &n, &dir)) return NULL;
    o = rg_new_profile();
    if (!o) return NULL;
    err[0] = '\0';
    rc = yrig_parse(PROF(o), text, (size_t)n, dir, err, sizeof err);
    if (rc != YRIG_OK) { Py_DECREF(o); return rg_fail(rc, err); }
    return o;
}

static PyObject* mod_dir(PyObject* m, PyObject* Py_UNUSED(a)) {
    char d[512];
    int rc;
    (void)m;
    rc = yrig_dir(d, sizeof d);
    if (rc != YRIG_OK)
        return rg_fail(rc, "no rig folder: the user's config folder is not set, or another user can write it");
    return PyUnicode_FromString(d);
}

static PyObject* mod_path(PyObject* m, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "name", NULL };
    const char* name = NULL;
    char d[512], p[700];
    int rc;
    (void)m;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|z", kw, &name)) return NULL;
    rc = yrig_dir(d, sizeof d);
    if (rc != YRIG_OK)
        return rg_fail(rc, "no rig folder: the user's config folder is not set, or another user can write it");
    rc = yrig_path(d, name, p, sizeof p);
    if (rc != YRIG_OK) return rg_fail(rc, "path: a profile name is 1 to 63 of A-Z a-z 0-9 _ -");
    return PyUnicode_FromString(p);
}

static PyObject* mod_check_name(PyObject* m, PyObject* arg) {
    long c = PyLong_AsLong(arg);
    (void)m;
    if (c == -1 && PyErr_Occurred()) return NULL;
    return PyUnicode_FromString(yrig_check_name((int)c));
}

static PyMethodDef module_methods[] = {
    { "load", (PyCFunction)(void (*)(void))mod_load, METH_VARARGS | METH_KEYWORDS,
      "load(name=None) -> Profile: the user's rig profile (profile.json, or <name>.json for a machine with two "
      "rigs) from dir(), with every loopback file checked (Profile.notes). Raises Missing when there is none, "
      "Folder when the folder is refused (another user can write it), Error for a bad file (the message names "
      "the field, line and column)." },
    { "load_file", mod_load_file, METH_VARARGS, "load_file(path) -> Profile: a profile anywhere; its loopback/ beside it." },
    { "parse", (PyCFunction)(void (*)(void))mod_parse, METH_VARARGS | METH_KEYWORDS,
      "parse(text, dir=None) -> Profile: from text; dir is the folder for the checks (None: none run)." },
    { "dir", mod_dir, METH_NOARGS,
      "dir() -> str: the user's rig folder (%APPDATA%\\ysp\\rig, ~/.config/ysp/rig). Raises Folder when refused." },
    { "path", (PyCFunction)(void (*)(void))mod_path, METH_VARARGS | METH_KEYWORDS,
      "path(name=None) -> str: dir()/profile.json or dir()/<name>.json." },
    { "check_name", mod_check_name, METH_O, "check_name(check) -> str: 'ok', 'missing', 'changed', ..." },
    { NULL }
};

static PyModuleDef ysp_rigfile_module = {
    PyModuleDef_HEAD_INIT, "ysp.rigfile",
    "ysp/rigfile.h: the rig profile (devices per role, loopback results, viewing geometry).",
    -1, module_methods, NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit_rigfile(void) {
    PyObject* m = PyModule_Create(&ysp_rigfile_module);
    int ok = 1;
    if (!m) return NULL;
    RgProfileType = PyType_FromSpec(&Profile_spec);
    if (!RgProfileType) goto fail;
    Py_INCREF(RgProfileType);
    if (PyModule_AddObject(m, "Profile", RgProfileType) < 0) goto fail;
    RgError = PyErr_NewException("ysp.rigfile.Error", NULL, NULL);
    RgMissing = RgError ? PyErr_NewException("ysp.rigfile.Missing", RgError, NULL) : NULL;
    RgFolder = RgError ? PyErr_NewException("ysp.rigfile.Folder", RgError, NULL) : NULL;
    if (!RgError || !RgMissing || !RgFolder) goto fail;
    Py_INCREF(RgError); Py_INCREF(RgMissing); Py_INCREF(RgFolder);
    if (PyModule_AddObject(m, "Error", RgError) < 0 || PyModule_AddObject(m, "Missing", RgMissing) < 0 ||
        PyModule_AddObject(m, "Folder", RgFolder) < 0)
        goto fail;
    if (PyModule_AddStringConstant(m, "__version__", RG_VERSION) < 0) goto fail;
    ok &= PyModule_AddStringConstant(m, "FORMAT", YRIG_FORMAT) == 0;
    ok &= PyModule_AddIntConstant(m, "FAMILY_LSL", YRIG_FAMILY_LSL) == 0;
    ok &= PyModule_AddIntConstant(m, "MAX_ROLES", YRIG_MAX_ROLES) == 0;
    ok &= PyModule_AddIntConstant(m, "OK", YRIG_OK) == 0;
    ok &= PyModule_AddIntConstant(m, "ERR_ARG", YRIG_ERR_ARG) == 0;
    ok &= PyModule_AddIntConstant(m, "ERR_IO", YRIG_ERR_IO) == 0;
    ok &= PyModule_AddIntConstant(m, "ERR_FOLDER", YRIG_ERR_FOLDER) == 0;
    ok &= PyModule_AddIntConstant(m, "ERR_JSON", YRIG_ERR_JSON) == 0;
    ok &= PyModule_AddIntConstant(m, "ERR_FORMAT", YRIG_ERR_FORMAT) == 0;
    ok &= PyModule_AddIntConstant(m, "ERR_MISSING", YRIG_ERR_MISSING) == 0;
    ok &= PyModule_AddIntConstant(m, "ERR_FULL", YRIG_ERR_FULL) == 0;
    ok &= PyModule_AddIntConstant(m, "CHECK_NONE", YRIG_CHECK_NONE) == 0;
    ok &= PyModule_AddIntConstant(m, "CHECK_OK", YRIG_CHECK_OK) == 0;
    ok &= PyModule_AddIntConstant(m, "CHECK_MISSING", YRIG_CHECK_MISSING) == 0;
    ok &= PyModule_AddIntConstant(m, "CHECK_CHANGED", YRIG_CHECK_CHANGED) == 0;
    ok &= PyModule_AddIntConstant(m, "CHECK_DIFFERS", YRIG_CHECK_DIFFERS) == 0;
    ok &= PyModule_AddIntConstant(m, "CHECK_NOT_RUN", YRIG_CHECK_NOT_RUN) == 0;
    /* ysp/input.h tiers, the source()'s tier */
    ok &= PyModule_AddIntConstant(m, "TIER_UNKNOWN", YIN_TIER_UNKNOWN) == 0;
    ok &= PyModule_AddIntConstant(m, "TIER_1", YIN_TIER_1) == 0;
    ok &= PyModule_AddIntConstant(m, "TIER_2", YIN_TIER_2) == 0;
    ok &= PyModule_AddIntConstant(m, "TIER_3", YIN_TIER_3) == 0;
    ok &= PyModule_AddIntConstant(m, "TIER_SIM", YIN_TIER_SIM) == 0;
    if (!ok) goto fail;
    return m;
fail:
    Py_DECREF(m);
    return NULL;
}
