/* ysp_parallel_ext.c - CPython extension wrapping ysp/parallel.h (module ysp.parallel)
 *
 * A thin, dependency-free binding (no nanobind/pybind/Cython): it needs only
 * Python.h, matching the single-header library's zero-dependency style. The
 * library implementation is compiled directly into this module.
 *
 * Built against the stable ABI / Limited API (Py_LIMITED_API), so one compiled
 * .abi3.so works across CPython >= 3.8 without recompiling per version. That
 * rules out the static PyTypeObject layout, so the Port type is a heap type
 * created with PyType_FromSpec.
 *
 *     import ysp.parallel as pp
 *     for info in pp.list_ports():
 *         print(info)                    # {'name':..., 'backend':..., 'base_addr':...}
 *     with pp.Port() as port:            # defaults per platform
 *         port.write_data(0x55)
 *         port.pulse(0x55, usec=2000)    # blocking
 *         port.pulse_async(0x55, 2000)   # returns immediately
 *         print(hex(port.read_status()))
 *
 * THREADING: ysp/parallel.h locks data writes against its own async worker,
 * but nothing else on the handle: every bool-returning call clears and then
 * rewrites one shared error buffer, and the ppdev descriptor is shared too.
 * pulse() and pulse_async() drop the GIL, so the GIL alone would not keep two
 * Python threads out of the library at once. Every method therefore takes a
 * per-Port lock for the duration of its library call, which makes a Port safe
 * to share between threads and makes two pulses serialize instead of
 * interleaving. A thread waiting for that lock releases the GIL, so a pulse
 * in flight never stalls unrelated Python code.
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x03080000   /* target the CPython 3.8+ stable ABI */
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#define YSP_PARALLEL_IMPLEMENTATION
#include "ysp/parallel.h"

/* An OS mutex, not PyThread_type_lock: the PyThread_* functions are listed
 * in the stable ABI, but python3.dll did not export them before CPython 3.10
 * (PC/python3dll.c), so an abi3 wheel built on 3.9 for Windows fails to link
 * against them. */
#if defined(_WIN32)
    #include <windows.h>
    typedef CRITICAL_SECTION pp_mutex;
#else
    #include <pthread.h>
    typedef pthread_mutex_t pp_mutex;
#endif

typedef struct {
    PyObject_HEAD
    ypar_port port;
    pp_mutex  mu;        /* serializes library calls on this handle */
    int       mu_ready;  /* mu is initialized; 0 until __init__ runs */
} PortObject;

static PyObject* PpError; /* module-level exception type */

#define AS_PORT(self) (&((PortObject*)(self))->port)

/* Take the per-Port lock. The fast path is uncontended; only a real wait
 * gives up the GIL, so the common case costs one atomic and no thread state
 * switch. NULL means __new__ ran but __init__ did not, and there is nothing
 * open to protect. */
static void pp_lock(PyObject* self) {
    PortObject* p = (PortObject*)self;
    if (!p->mu_ready) return;
#if defined(_WIN32)
    if (TryEnterCriticalSection(&p->mu)) return;
    Py_BEGIN_ALLOW_THREADS
    EnterCriticalSection(&p->mu);
    Py_END_ALLOW_THREADS
#else
    if (pthread_mutex_trylock(&p->mu) == 0) return;
    Py_BEGIN_ALLOW_THREADS
    pthread_mutex_lock(&p->mu);
    Py_END_ALLOW_THREADS
#endif
}

static void pp_unlock(PyObject* self) {
    PortObject* p = (PortObject*)self;
    if (!p->mu_ready) return;
#if defined(_WIN32)
    LeaveCriticalSection(&p->mu);
#else
    pthread_mutex_unlock(&p->mu);
#endif
}

/* Raise ysp.parallel.Error carrying the library's last error string. */
static PyObject* pp_raise(ypar_port* port) {
    PyErr_SetString(PpError, ypar_error(port));
    return NULL;
}

static int Port_init(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "device", "backend", "base_addr", "exclusive",
                          "rt_runtime_ns", "rt_deadline_ns", "rt_period_ns", NULL };
    const char* device = NULL;
    int backend = YPAR_BACKEND_DEFAULT;
    unsigned int base_addr = 0;
    int exclusive = 0;
    unsigned long long rt_runtime = 0, rt_deadline = 0, rt_period = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ziIpKKK", kw,
                                     &device, &backend, &base_addr, &exclusive,
                                     &rt_runtime, &rt_deadline, &rt_period))
        return -1;
    if (base_addr > 0xFFFFu) {
        PyErr_SetString(PyExc_ValueError, "base_addr must fit in 16 bits");
        return -1;
    }

    ypar_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.backend = (ypar_backend)backend;
    desc.device = device;
    desc.base_addr = (uint16_t)base_addr;
    desc.exclusive = exclusive ? true : false;
    /* Async-worker SCHED_DEADLINE reservation (Linux); all-zero -> defaults.
     * ypar_open validates 0 < runtime <= deadline <= period and raises here. */
    desc.sched.runtime_ns = rt_runtime;
    desc.sched.deadline_ns = rt_deadline;
    desc.sched.period_ns = rt_period;

    PortObject* p = (PortObject*)self;
    if (!p->mu_ready) {
#if defined(_WIN32)
        InitializeCriticalSection(&p->mu);
#else
        if (pthread_mutex_init(&p->mu, NULL) != 0) { PyErr_NoMemory(); return -1; }
#endif
        p->mu_ready = 1;
    }

    /* Python can call __init__ on a live object; ypar_open would memset over
     * the open descriptor and orphan the async worker. The object memory is
     * zeroed at allocation, so is_open is valid here and close is a no-op on
     * a fresh instance. */
    pp_lock(self);
    ypar_close(AS_PORT(self));
    bool ok = ypar_open(AS_PORT(self), &desc);
    if (!ok) PyErr_SetString(PpError, ypar_error(AS_PORT(self)));
    pp_unlock(self);
    return ok ? 0 : -1;
}

static void Port_dealloc(PyObject* self) {
    /* No lock here: the last reference is gone, so no other thread can be
     * inside a call on this handle. The GIL still goes, because ypar_close
     * joins the async worker and a dealloc runs on whatever thread drops the
     * last reference. */
    Py_BEGIN_ALLOW_THREADS
    ypar_close(AS_PORT(self));
    Py_END_ALLOW_THREADS
    if (((PortObject*)self)->mu_ready) {
#if defined(_WIN32)
        DeleteCriticalSection(&((PortObject*)self)->mu);
#else
        pthread_mutex_destroy(&((PortObject*)self)->mu);
#endif
        ((PortObject*)self)->mu_ready = 0;
    }
    /* Heap type: fetch tp_free via the stable ABI and release the type ref the
     * instance holds (PyType_GenericAlloc incref's the type since 3.8). */
    PyTypeObject* tp = Py_TYPE(self);
    freefunc tp_free = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    tp_free(self);
    Py_DECREF(tp);
}

/* --- data register ------------------------------------------------------ */

static PyObject* Port_write_data(PyObject* self, PyObject* arg) {
    unsigned long v = PyLong_AsUnsignedLong(arg);
    if (v == (unsigned long)-1 && PyErr_Occurred()) return NULL;
    if (v > 0xFF) { PyErr_SetString(PyExc_ValueError, "value must be 0..255"); return NULL; }
    pp_lock(self);
    bool ok = ypar_write_data(AS_PORT(self), (uint8_t)v) >= 0;
    if (!ok) pp_raise(AS_PORT(self));
    pp_unlock(self);
    if (!ok) return NULL;
    Py_RETURN_NONE;
}

static PyObject* Port_read_data(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    pp_lock(self);
    int v = ypar_read_data(AS_PORT(self));
    bool ok = v >= 0;
    if (!ok) pp_raise(AS_PORT(self));
    pp_unlock(self);
    if (!ok) return NULL;
    return PyLong_FromUnsignedLong((unsigned long)v);
}

static PyObject* Port_set_data_dir(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "input", NULL };
    int input = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "p", kw, &input)) return NULL;
    pp_lock(self);
    bool ok = ypar_set_data_dir(AS_PORT(self), input ? true : false) >= 0;
    if (!ok) pp_raise(AS_PORT(self));
    pp_unlock(self);
    if (!ok) return NULL;
    Py_RETURN_NONE;
}

static PyObject* Port_pulse(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "value", "usec", NULL };
    unsigned int value, usec;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "II", kw, &value, &usec)) return NULL;
    if (value > 0xFF) { PyErr_SetString(PyExc_ValueError, "value must be 0..255"); return NULL; }
    bool ok;
    /* Release the GIL: ypar_pulse blocks for `usec`. The per-Port lock is what
     * keeps another thread out of the library while it does. */
    pp_lock(self);
    Py_BEGIN_ALLOW_THREADS
    /* The module keeps microseconds; the C API takes ns. */
    ok = ypar_pulse(AS_PORT(self), (uint8_t)value, (int64_t)usec * 1000) >= 0;
    Py_END_ALLOW_THREADS
    if (!ok) pp_raise(AS_PORT(self));
    pp_unlock(self);
    if (!ok) return NULL;
    Py_RETURN_NONE;
}

static PyObject* Port_pulse_async(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "value", "usec", NULL };
    unsigned int value, usec;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "II", kw, &value, &usec)) return NULL;
    if (value > 0xFF) { PyErr_SetString(PyExc_ValueError, "value must be 0..255"); return NULL; }
    bool ok;
    /* The call returns before the width elapses, but its onset write still
     * goes to the port and takes the worker's lock. */
    pp_lock(self);
    Py_BEGIN_ALLOW_THREADS
    ok = ypar_pulse_async(AS_PORT(self), (uint8_t)value, (int64_t)usec * 1000) >= 0;
    Py_END_ALLOW_THREADS
    if (!ok) pp_raise(AS_PORT(self));
    pp_unlock(self);
    if (!ok) return NULL;
    Py_RETURN_NONE;
}

/* --- status / control --------------------------------------------------- */

static PyObject* Port_read_status(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    pp_lock(self);
    int v = ypar_read_status(AS_PORT(self));
    bool ok = v >= 0;
    if (!ok) pp_raise(AS_PORT(self));
    pp_unlock(self);
    if (!ok) return NULL;
    return PyLong_FromUnsignedLong((unsigned long)v);
}

static PyObject* Port_get_status_bit(PyObject* self, PyObject* arg) {
    unsigned long mask = PyLong_AsUnsignedLong(arg);
    if (mask == (unsigned long)-1 && PyErr_Occurred()) return NULL;
    pp_lock(self);
    int set = ypar_get_status_bit(AS_PORT(self), (uint8_t)mask);
    bool ok = set >= 0;
    if (!ok) pp_raise(AS_PORT(self));
    pp_unlock(self);
    if (!ok) return NULL;
    return PyBool_FromLong(set);
}

static PyObject* Port_read_control(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    pp_lock(self);
    int v = ypar_read_control(AS_PORT(self));
    bool ok = v >= 0;
    if (!ok) pp_raise(AS_PORT(self));
    pp_unlock(self);
    if (!ok) return NULL;
    return PyLong_FromUnsignedLong((unsigned long)v);
}

static PyObject* Port_write_control(PyObject* self, PyObject* arg) {
    unsigned long v = PyLong_AsUnsignedLong(arg);
    if (v == (unsigned long)-1 && PyErr_Occurred()) return NULL;
    if (v > 0xFF) { PyErr_SetString(PyExc_ValueError, "value must be 0..255"); return NULL; }
    pp_lock(self);
    bool ok = ypar_write_control(AS_PORT(self), (uint8_t)v) >= 0;
    if (!ok) pp_raise(AS_PORT(self));
    pp_unlock(self);
    if (!ok) return NULL;
    Py_RETURN_NONE;
}

static PyObject* Port_set_control_bit(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "mask", "on", NULL };
    unsigned int mask;
    int on;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "Ip", kw, &mask, &on)) return NULL;
    pp_lock(self);
    bool ok = ypar_set_control_bit(AS_PORT(self), (uint8_t)mask, on ? true : false) >= 0;
    if (!ok) pp_raise(AS_PORT(self));
    pp_unlock(self);
    if (!ok) return NULL;
    Py_RETURN_NONE;
}

/* --- lifecycle / context manager --------------------------------------- */

static PyObject* Port_close(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    /* ypar_close stops the worker and writes any pending trailing edge, so it
     * must not run beside another call on this handle. */
    pp_lock(self);
    Py_BEGIN_ALLOW_THREADS
    ypar_close(AS_PORT(self));
    Py_END_ALLOW_THREADS
    pp_unlock(self);
    Py_RETURN_NONE;
}

static PyObject* Port_enter(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    Py_INCREF(self);
    return self;
}

static PyObject* Port_exit(PyObject* self, PyObject* Py_UNUSED(args)) {
    pp_lock(self);
    Py_BEGIN_ALLOW_THREADS
    ypar_close(AS_PORT(self));
    Py_END_ALLOW_THREADS
    pp_unlock(self);
    Py_RETURN_FALSE; /* don't suppress exceptions */
}

static PyObject* Port_get_is_open(PyObject* self, void* Py_UNUSED(closure)) {
    return PyBool_FromLong(ypar_is_open(AS_PORT(self)));
}

static PyObject* Port_get_base_addr(PyObject* self, void* Py_UNUSED(closure)) {
    return PyLong_FromUnsignedLong(AS_PORT(self)->base_addr);
}

static PyObject* Port_get_backend(PyObject* self, void* Py_UNUSED(closure)) {
    return PyLong_FromLong((long)AS_PORT(self)->backend);
}

static PyObject* Port_get_sched(PyObject* self, void* Py_UNUSED(closure)) {
    ypar_port* p = AS_PORT(self);
    return Py_BuildValue("{s:K,s:K,s:K}",
                         "runtime_ns",  (unsigned long long)p->sched.runtime_ns,
                         "deadline_ns", (unsigned long long)p->sched.deadline_ns,
                         "period_ns",   (unsigned long long)p->sched.period_ns);
}

static PyObject* Port_get_async_policy(PyObject* self, void* Py_UNUSED(closure)) {
    return PyLong_FromLong((long)AS_PORT(self)->async_policy);
}

static PyGetSetDef Port_getset[] = {
    { "is_open",   Port_get_is_open,   NULL, "True while the port is open.", NULL },
    { "base_addr", Port_get_base_addr, NULL, "Resolved I/O base address.",   NULL },
    { "backend",   Port_get_backend,   NULL, "Active backend (BACKEND_*).",  NULL },
    { "async_policy", Port_get_async_policy, NULL,
      "Scheduling policy the async-pulse worker obtained (ASYNC_*). Log it "
      "with timing data: DEADLINE is refused without CAP_SYS_NICE or under "
      "CPU pinning, and the library falls back silently.", NULL },
    { "sched",     Port_get_sched,     NULL,
      "Effective async-worker RT params (Linux SCHED_DEADLINE) as a dict "
      "{runtime_ns, deadline_ns, period_ns}.", NULL },
    { NULL }
};

static PyMethodDef Port_methods[] = {
    { "write_data",     Port_write_data,     METH_O,
      "write_data(value): drive the 8 data bits (0..255)." },
    { "read_data",      Port_read_data,      METH_NOARGS,
      "read_data() -> int: read the data register." },
    { "set_data_dir",   (PyCFunction)Port_set_data_dir,   METH_VARARGS | METH_KEYWORDS,
      "set_data_dir(input): set data-line direction on a bidirectional port." },
    { "pulse",          (PyCFunction)Port_pulse,          METH_VARARGS | METH_KEYWORDS,
      "pulse(value, usec): write value, block usec microseconds, then write 0." },
    { "pulse_async",    (PyCFunction)Port_pulse_async,    METH_VARARGS | METH_KEYWORDS,
      "pulse_async(value, usec): write value now, return immediately; a worker "
      "thread writes 0 after usec." },
    { "read_status",    Port_read_status,    METH_NOARGS,
      "read_status() -> int: read the status register (STATUS_* bits)." },
    { "get_status_bit", Port_get_status_bit, METH_O,
      "get_status_bit(mask) -> bool: test one status bit." },
    { "read_control",   Port_read_control,   METH_NOARGS,
      "read_control() -> int: read the control register (CONTROL_* bits)." },
    { "write_control",  Port_write_control,  METH_O,
      "write_control(value): write the control register." },
    { "set_control_bit",(PyCFunction)Port_set_control_bit,METH_VARARGS | METH_KEYWORDS,
      "set_control_bit(mask, on): set/clear one control bit." },
    { "close",          Port_close,          METH_NOARGS,
      "close(): release the port (idempotent)." },
    { "__enter__",      Port_enter,          METH_NOARGS, NULL },
    { "__exit__",       Port_exit,           METH_VARARGS, NULL },
    { NULL }
};

static PyType_Slot Port_slots[] = {
    { Py_tp_doc,     (void*)"Parallel port handle. Port(device=None, "
                            "backend=BACKEND_DEFAULT, base_addr=0, exclusive=False)." },
    { Py_tp_new,     (void*)PyType_GenericNew },
    { Py_tp_init,    (void*)Port_init },
    { Py_tp_dealloc, (void*)Port_dealloc },
    { Py_tp_methods, Port_methods },
    { Py_tp_getset,  Port_getset },
    { 0, NULL }
};

static PyType_Spec Port_spec = {
    .name = "ysp.parallel.Port",
    .basicsize = sizeof(PortObject),
    .itemsize = 0,
    .flags = Py_TPFLAGS_DEFAULT,
    .slots = Port_slots,
};

/* --- module-level functions -------------------------------------------- */

static PyObject* mod_list_ports(PyObject* Py_UNUSED(self), PyObject* Py_UNUSED(args)) {
    int n = ypar_list_ports(NULL, 0);
    ypar_port_info* v = NULL;
    if (n > 0) {
        v = (ypar_port_info*)PyMem_Malloc((size_t)n * sizeof(*v));
        if (!v) return PyErr_NoMemory();
        int got = ypar_list_ports(v, n);
        if (got < n) n = got; /* a port vanished between the two calls */
    }
    /* Size the list from the final count: a list created from the first count
     * and then trimmed would keep NULL slots, which crash on iteration. */
    PyObject* list = PyList_New(n > 0 ? n : 0);
    if (!list) { PyMem_Free(v); return NULL; }

    for (int i = 0; i < n; i++) {
        PyObject* d = PyDict_New();
        PyObject* name = PyUnicode_FromString(v[i].name);
        PyObject* be = PyLong_FromLong((long)v[i].backend);
        PyObject* base = PyLong_FromUnsignedLong(v[i].base_addr);
        int ok = d && name && be && base &&
                 PyDict_SetItemString(d, "name", name) == 0 &&
                 PyDict_SetItemString(d, "backend", be) == 0 &&
                 PyDict_SetItemString(d, "base_addr", base) == 0;
        Py_XDECREF(name); Py_XDECREF(be); Py_XDECREF(base);
        if (!ok) {
            Py_XDECREF(d); Py_DECREF(list); PyMem_Free(v); return NULL;
        }
        PyList_SetItem(list, i, d); /* steals ref to d */
    }
    PyMem_Free(v);
    return list;
}

/* The clock the library times its own deadlines against, from ysp/rt.h, which
 * ysp/parallel.h builds on. Exposed so a caller can bracket a write with the
 * same time base instead of mixing in time.monotonic(), which is a different
 * clock on Windows. ysp.serial.now_us() reads the same clock. */
static PyObject* mod_now_us(PyObject* Py_UNUSED(self), PyObject* Py_UNUSED(args)) {
    return PyLong_FromUnsignedLongLong((unsigned long long)yrt_now_ns() / 1000u);
}

static PyMethodDef module_methods[] = {
    { "now_us", mod_now_us, METH_NOARGS,
      "now_us() -> int: monotonic microseconds from the clock the library uses "
      "for its own deadlines (CLOCK_MONOTONIC / QueryPerformanceCounter). "
      "Bracket a write with it: the physical onset lies after the first "
      "reading, and the difference bounds the syscall cost." },
    { "list_ports", mod_list_ports, METH_NOARGS,
      "list_ports() -> list[dict]: enumerate available parallel ports without "
      "opening them. Each dict has 'name', 'backend', 'base_addr'." },
    { NULL }
};

static PyModuleDef ysp_parallel_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "ysp.parallel",
    .m_doc = "Parallel-port access (data/status/control, blocking and async pulses).",
    .m_size = -1,
    .m_methods = module_methods,
};

/* The init function is named after the last dotted component: the loader looks
 * for PyInit_parallel in ysp/parallel.abi3.so. */
PyMODINIT_FUNC PyInit_parallel(void) {
    PyObject* m = PyModule_Create(&ysp_parallel_module);
    if (!m) return NULL;

    PyObject* type = PyType_FromSpec(&Port_spec);
    if (!type) { Py_DECREF(m); return NULL; }
    if (PyModule_AddObject(m, "Port", type) < 0) {
        Py_DECREF(type); Py_DECREF(m); return NULL;
    }

    PpError = PyErr_NewException("ysp.parallel.Error", NULL, NULL);
    if (!PpError) { Py_DECREF(m); return NULL; }
    Py_INCREF(PpError);
    if (PyModule_AddObject(m, "Error", PpError) < 0) {
        Py_DECREF(PpError); Py_DECREF(PpError); Py_DECREF(m); return NULL;
    }

    /* backends */
    PyModule_AddIntConstant(m, "BACKEND_DEFAULT", YPAR_BACKEND_DEFAULT);
    PyModule_AddIntConstant(m, "BACKEND_PPDEV",   YPAR_BACKEND_PPDEV);
    PyModule_AddIntConstant(m, "BACKEND_DIRECT",  YPAR_BACKEND_DIRECT);
    PyModule_AddIntConstant(m, "BACKEND_INPOUT",  YPAR_BACKEND_INPOUT);
    /* default base addresses */
    PyModule_AddIntConstant(m, "LPT1", YPAR_LPT1);
    PyModule_AddIntConstant(m, "LPT2", YPAR_LPT2);
    PyModule_AddIntConstant(m, "LPT3", YPAR_LPT3);
    /* async-worker policy actually obtained (Port.async_policy). The values
     * are ysp/rt.h's yrt_policy, which ysp.serial reports too, so one
     * constant means the same thing in both modules. TIME_CONSTRAINT is the
     * macOS rung; ysp/parallel.h never returns it, but the value is part of
     * the shared enum. */
    PyModule_AddIntConstant(m, "ASYNC_NONE",            YRT_POLICY_NONE);
    PyModule_AddIntConstant(m, "ASYNC_DEADLINE",        YRT_POLICY_DEADLINE);
    PyModule_AddIntConstant(m, "ASYNC_TIME_CONSTRAINT", YRT_POLICY_TIME_CONSTRAINT);
    PyModule_AddIntConstant(m, "ASYNC_FIFO",            YRT_POLICY_FIFO);
    PyModule_AddIntConstant(m, "ASYNC_TIME_CRITICAL",   YRT_POLICY_TIME_CRITICAL);
    PyModule_AddIntConstant(m, "ASYNC_NORMAL",          YRT_POLICY_NORMAL);
    /* default async-worker SCHED_DEADLINE params (nanoseconds) */
    PyModule_AddIntConstant(m, "DEFAULT_RT_RUNTIME_NS",  (long)YPAR_DEFAULT_RT_RUNTIME_NS);
    PyModule_AddIntConstant(m, "DEFAULT_RT_DEADLINE_NS", (long)YPAR_DEFAULT_RT_DEADLINE_NS);
    PyModule_AddIntConstant(m, "DEFAULT_RT_PERIOD_NS",   (long)YPAR_DEFAULT_RT_PERIOD_NS);
    /* status bits */
    PyModule_AddIntConstant(m, "STATUS_BUSY",   YPAR_STATUS_BUSY);
    PyModule_AddIntConstant(m, "STATUS_ACK",    YPAR_STATUS_ACK);
    PyModule_AddIntConstant(m, "STATUS_PAPER",  YPAR_STATUS_PAPER);
    PyModule_AddIntConstant(m, "STATUS_SELECT", YPAR_STATUS_SELECT);
    PyModule_AddIntConstant(m, "STATUS_ERROR",  YPAR_STATUS_ERROR);
    /* control bits */
    PyModule_AddIntConstant(m, "CONTROL_STROBE",   YPAR_CONTROL_STROBE);
    PyModule_AddIntConstant(m, "CONTROL_AUTOFEED", YPAR_CONTROL_AUTOFEED);
    PyModule_AddIntConstant(m, "CONTROL_INIT",     YPAR_CONTROL_INIT);
    PyModule_AddIntConstant(m, "CONTROL_SELECT",   YPAR_CONTROL_SELECT);
    PyModule_AddIntConstant(m, "CONTROL_DIR",      YPAR_CONTROL_DIR);

    return m;
}
