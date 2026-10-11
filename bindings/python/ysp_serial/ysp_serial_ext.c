/* ysp_serial_ext.c - CPython extension wrapping ysp/serial.h (module ysp.serial)
 *
 * A thin, dependency-free binding (no nanobind/pybind/Cython): it needs only
 * Python.h, matching the single-header library's zero-dependency style. The
 * library implementation is compiled directly into this module.
 *
 * Built against the stable ABI / Limited API (Py_LIMITED_API), so one compiled
 * ysp/serial.abi3.so works across CPython >= 3.8 without recompiling per
 * version. That rules out the static PyTypeObject layout, so the Port type is
 * a heap type created with PyType_FromSpec.
 *
 *     import ysp.serial as ps
 *     for info in ps.list_ports():
 *         print(info)                       # {'name':..., 'vid':..., ...}
 *     with ps.Port("/dev/ttyUSB0", baud=115200) as port:
 *         port.write_byte(0x55)             # raise a trigger
 *         port.pulse(0x55, 0x00, 0.002)     # 0x55, hold 2 ms, then 0x00
 *         data = port.read(6, 0.5, all=True)   # a 6-byte frame within 0.5 s
 *
 * UNITS: every time is float seconds, as in every ysp binding; the C API
 * takes int64 ns, and the conversion happens here, once, at the boundary.
 *
 * THREADING: the header's one-reader-one-writer rule applies to Python threads
 * exactly as it does to C ones. read/write/drain/pulse/send_break and
 * pulse_async's onset write release the GIL, so a listener thread blocked in
 * read() does not stall the interpreter, and interrupt() from another thread
 * ends that read with ysp.serial.Interrupted.
 */
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x03080000   /* target the CPython 3.8+ stable ABI */
#endif
#define PY_SSIZE_T_CLEAN
#include <Python.h>

/* Python.h first is safe here: pyconfig.h already defines _GNU_SOURCE on Linux
 * and raises _WIN32_WINNT past 0x0600 on Windows, which is exactly what the
 * header asks for when it is not the first include. */
#define YSP_SERIAL_IMPLEMENTATION
#include "ysp/serial.h"

#include <limits.h>
#include <math.h>
#include <string.h>

typedef struct {
    PyObject_HEAD
    yser_port port;
} PortObject;

/* module-level exception types; Error is the base of the other three */
static PyObject* PsError;
static PyObject* PsDisconnected;
static PyObject* PsInterrupted;
static PyObject* PsClosed;

#define AS_PORT(self) (&((PortObject*)(self))->port)

/* The distribution's version is the header's, plus a fourth component that
 * counts changes to this binding alone (0.5.0.1: times in seconds), so such
 * a change gets a new version without naming a header that does not exist.
 * When the header's version moves, drop the suffix here and in
 * pyproject.toml; tests/test_serial.py checks that the two agree. */
#define PS_VERSION YSER_VERSION_STRING ".1"

/* ps_seconds() flags. */
#define PS_S_INF  1  /* math.inf means "forever" (YSER_TIMEOUT_INFINITE) */
#define PS_S_POS  2  /* a positive value never rounds to 0: 0 means poll or
                      * "library default" in C, which is not what was asked */

/* Python seconds to int64 ns, rounded to the nearest ns. The whole seconds
 * convert exactly and only the fraction is scaled, so a large value does not
 * lose nanoseconds to the product's rounding. A negative value is refused
 * rather than passed on: in C a negative read timeout polls and a negative
 * width fails after the call, and neither is what a Python caller meant. */
static int ps_seconds(PyObject* o, const char* what, int flags, int64_t* out) {
    double s = PyFloat_AsDouble(o);
    if (s == -1.0 && PyErr_Occurred()) return -1;
    /* Comparisons, not isnan()/isinf(): MinGW's macros convert through
     * float and warn under -Wfloat-conversion. !(s >= 0) is true for NaN. */
    if (!(s >= 0)) {
        PyErr_Format(PyExc_ValueError, "%s: seconds, >= 0 (not NaN)", what);
        return -1;
    }
    if (s == HUGE_VAL) {
        if (flags & PS_S_INF) { *out = YSER_TIMEOUT_INFINITE; return 0; }
        PyErr_Format(PyExc_ValueError, "%s: seconds, finite", what);
        return -1;
    }
    if (s >= 9223372036.0) {   /* INT64_MAX ns is 9223372036.85 s */
        PyErr_Format(PyExc_OverflowError, "%s: seconds, < 9223372036 (int64 ns)%s", what,
                     (flags & PS_S_INF) ? "; use math.inf to wait forever" : "");
        return -1;
    }
    double whole = floor(s);
    int64_t ns = (int64_t)whole * 1000000000 + (int64_t)llround((s - whole) * 1e9);
    if (ns == 0 && s > 0 && (flags & PS_S_POS)) ns = 1;
    *out = ns;
    return 0;
}

static PyObject* ps_ns_to_seconds(int64_t ns) {
    if (ns == YSER_TIMEOUT_INFINITE) return PyFloat_FromDouble(HUGE_VAL);
    return PyFloat_FromDouble((double)ns / 1e9);
}

/* Raise the exception matching a negative YSER_ERR_* code. `oserr` is the
 * per-role slot of the call that failed (rd_oserr, wr_oserr or misc_oserr);
 * it is read before any Python call, because another thread in the same role
 * could overwrite it. */
static PyObject* ps_fail(int code, int oserr) {
    PyObject* exc = PsError;
    if (code == YSER_ERR_DISCONNECTED)     exc = PsDisconnected;
    else if (code == YSER_ERR_INTERRUPTED) exc = PsInterrupted;
    else if (code == YSER_ERR_CLOSED)      exc = PsClosed;
    PyErr_Format(exc, "%s; os error %d", yser_strerror(code), oserr);
    return NULL;
}

static int Port_init(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "device", "baud", "data_bits", "parity", "stop_bits",
                          "flow", "write_timeout", "exclusive", "low_latency",
                          "dtr_low_on_open", "rts_low_on_open", "keep_dtr_on_close",
                          "rt_runtime", "rt_deadline", "rt_period", NULL };
    const char* device = NULL;
    unsigned int baud = 0;
    unsigned char data_bits = 0;
    int parity = YSER_PARITY_NONE, stop_bits = YSER_STOP_BITS_1, flow = YSER_FLOW_NONE;
    int exclusive = 0, low_latency = 0, dtr_low = 0, rts_low = 0, keep_dtr = 0;
    PyObject *wt_obj = NULL, *rt_obj[3] = { NULL, NULL, NULL };
    int64_t write_timeout_ns = 0, rt_ns[3] = { 0, 0, 0 };

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|IbiiiOpppppOOO", kw,
                                     &device, &baud, &data_bits, &parity, &stop_bits,
                                     &flow, &wt_obj, &exclusive, &low_latency,
                                     &dtr_low, &rts_low, &keep_dtr,
                                     &rt_obj[0], &rt_obj[1], &rt_obj[2]))
        return -1;
    /* Checked before anything is closed or opened, so a bad argument leaves a
     * live Port as it was. The C write timeout has no poll: 0 there means
     * the 1 s default, so a Python 0 is refused instead of silently waiting
     * a second. Omit the argument for the default. */
    if (wt_obj) {
        if (ps_seconds(wt_obj, "write_timeout", PS_S_INF | PS_S_POS, &write_timeout_ns) < 0)
            return -1;
        if (write_timeout_ns == 0) {
            PyErr_SetString(PyExc_ValueError,
                            "write_timeout: seconds, > 0 (a write cannot poll; "
                            "omit it for DEFAULT_WRITE_TIMEOUT)");
            return -1;
        }
    }
    {
        static const char* rt_names[3] = { "rt_runtime", "rt_deadline", "rt_period" };
        for (int i = 0; i < 3; i++)
            if (rt_obj[i] && ps_seconds(rt_obj[i], rt_names[i], PS_S_POS, &rt_ns[i]) < 0)
                return -1;
    }

    yser_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.device = device;
    desc.baud = baud;
    desc.data_bits = data_bits;
    desc.parity = (yser_parity)parity;
    desc.stop_bits = (yser_stop_bits)stop_bits;
    desc.flow = (yser_flow)flow;
    desc.write_timeout_ns = write_timeout_ns;
    desc.exclusive = exclusive ? true : false;
    desc.low_latency = low_latency ? true : false;
    desc.dtr_low_on_open = dtr_low ? true : false;
    desc.rts_low_on_open = rts_low ? true : false;
    desc.keep_dtr_on_close = keep_dtr ? true : false;
    /* Async-worker SCHED_DEADLINE reservation (Linux); all-zero -> defaults.
     * yser_open validates 0 < runtime <= deadline <= period and raises here. */
    desc.sched.runtime_ns = rt_ns[0];
    desc.sched.deadline_ns = rt_ns[1];
    desc.sched.period_ns = rt_ns[2];

    /* Python can call __init__ on a live object; yser_open would memset over
     * the open descriptor and orphan the async worker. The object memory is
     * zeroed at allocation, so is_open is valid here and close is a no-op on
     * a fresh instance. */
    yser_close(AS_PORT(self));
    if (!yser_open(AS_PORT(self), &desc)) {
        PyErr_SetString(PsError, yser_error(AS_PORT(self)));
        return -1;
    }
    return 0;
}

static void Port_dealloc(PyObject* self) {
    /* Same reason as Port_close: yser_close joins the async worker and can
     * block in the driver for up to write_timeout on a wedged device.
     * Holding the GIL through that stalls every other Python thread, and a
     * dealloc can run on any thread the last reference happens to die on. */
    Py_BEGIN_ALLOW_THREADS
    yser_close(AS_PORT(self));
    Py_END_ALLOW_THREADS
    /* Heap type: fetch tp_free via the stable ABI and release the type ref the
     * instance holds (PyType_GenericAlloc incref's the type since 3.8). */
    PyTypeObject* tp = Py_TYPE(self);
    freefunc tp_free = (freefunc)PyType_GetSlot(tp, Py_tp_free);
    tp_free(self);
    Py_DECREF(tp);
}

/* --- input (reader role) ------------------------------------------------ */

static PyObject* Port_read(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "n", "timeout", "all", NULL };
    int n;
    PyObject* timeout_obj;
    int all = 0;
    int64_t timeout_ns;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "iO|p", kw, &n, &timeout_obj, &all))
        return NULL;
    if (n < 0) { PyErr_SetString(PyExc_ValueError, "n must be >= 0"); return NULL; }
    /* 0 polls; a short positive timeout stays a wait (the C side rounds it up
     * to 1 ms), and a negative one is refused, not polled. */
    if (ps_seconds(timeout_obj, "timeout", PS_S_INF | PS_S_POS, &timeout_ns) < 0) return NULL;
    if (n == 0) return PyBytes_FromStringAndSize("", 0);

    char* buf = (char*)PyMem_Malloc((size_t)n);
    if (!buf) return PyErr_NoMemory();
    int got;
    /* Release the GIL: yser_read blocks for up to the timeout, and interrupt()
     * has to reach it from another Python thread. */
    Py_BEGIN_ALLOW_THREADS
    got = yser_read(AS_PORT(self), buf, n, timeout_ns,
                    all ? YSER_READ_ALL : YSER_READ_ANY);
    Py_END_ALLOW_THREADS
    if (got < 0) {
        int oserr = AS_PORT(self)->rd_oserr;
        PyMem_Free(buf);
        return ps_fail(got, oserr);
    }
    PyObject* out = PyBytes_FromStringAndSize(buf, (Py_ssize_t)got);
    PyMem_Free(buf);
    return out;
}

static PyObject* Port_available(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    int n = yser_available(AS_PORT(self));
    if (n < 0) return ps_fail(n, AS_PORT(self)->rd_oserr);
    return PyLong_FromLong(n);
}

/* --- output (writer role) ----------------------------------------------- */

static PyObject* Port_write(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "data", NULL };
    PyObject* obj;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kw, &obj)) return NULL;

    /* Not "y#": it rejects a bytearray, and its pointer into one would dangle
     * if another thread resized the object while we hold no GIL. This borrows
     * an immutable bytes (the same object when `obj` already is one) and
     * copies anything else, so the buffer cannot move under the driver. */
    PyObject* data = PyBytes_FromObject(obj);
    if (!data) return NULL;
    Py_ssize_t len = PyBytes_Size(data);
    const char* p = PyBytes_AsString(data);
    if (!p || len > INT_MAX) {
        Py_DECREF(data);
        if (len > INT_MAX) PyErr_SetString(PyExc_ValueError, "data is too long");
        return NULL;
    }

    int n;
    Py_BEGIN_ALLOW_THREADS
    n = yser_write(AS_PORT(self), p, (int)len);
    Py_END_ALLOW_THREADS
    Py_DECREF(data);
    if (n < 0) return ps_fail(n, AS_PORT(self)->wr_oserr);
    return PyLong_FromLong(n);
}

static PyObject* Port_write_byte(PyObject* self, PyObject* arg) {
    unsigned long v = PyLong_AsUnsignedLong(arg);
    if (v == (unsigned long)-1 && PyErr_Occurred()) return NULL;
    if (v > 0xFF) { PyErr_SetString(PyExc_ValueError, "value must be 0..255"); return NULL; }
    int n;
    Py_BEGIN_ALLOW_THREADS
    n = yser_write_byte(AS_PORT(self), (uint8_t)v);
    Py_END_ALLOW_THREADS
    if (n < 0) return ps_fail(n, AS_PORT(self)->wr_oserr);
    return PyLong_FromLong(n);
}

static PyObject* Port_drain(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    int rc;
    Py_BEGIN_ALLOW_THREADS
    rc = yser_drain(AS_PORT(self));
    Py_END_ALLOW_THREADS
    if (rc < 0) return ps_fail(rc, AS_PORT(self)->wr_oserr);
    Py_RETURN_NONE;
}

/* The arguments of pulse() and pulse_async(). */
static int ps_pulse_args(PyObject* args, PyObject* kwds, unsigned int* on, unsigned int* off,
                         int64_t* width_ns) {
    static char* kw[] = { "on", "off", "width", NULL };
    PyObject* width_obj;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "IIO", kw, on, off, &width_obj)) return -1;
    if (*on > 0xFF || *off > 0xFF) {
        PyErr_SetString(PyExc_ValueError, "on and off must be 0..255");
        return -1;
    }
    return ps_seconds(width_obj, "width", 0, width_ns);
}

static PyObject* Port_pulse(PyObject* self, PyObject* args, PyObject* kwds) {
    unsigned int on, off;
    int64_t width_ns;
    if (ps_pulse_args(args, kwds, &on, &off, &width_ns) < 0) return NULL;
    int n;
    /* Release the GIL: yser_pulse holds the line for the width. */
    Py_BEGIN_ALLOW_THREADS
    n = yser_pulse(AS_PORT(self), (uint8_t)on, (uint8_t)off, width_ns);
    Py_END_ALLOW_THREADS
    if (n < 0) return ps_fail(n, AS_PORT(self)->wr_oserr);
    return PyLong_FromLong(n);
}

static PyObject* Port_pulse_async(PyObject* self, PyObject* args, PyObject* kwds) {
    unsigned int on, off;
    int64_t width_ns;
    if (ps_pulse_args(args, kwds, &on, &off, &width_ns) < 0) return NULL;
    int n;
    /* The call returns before the width elapses, but its onset write still
     * goes through the driver and can block up to write_timeout. */
    Py_BEGIN_ALLOW_THREADS
    n = yser_pulse_async(AS_PORT(self), (uint8_t)on, (uint8_t)off, width_ns);
    Py_END_ALLOW_THREADS
    if (n < 0) return ps_fail(n, AS_PORT(self)->wr_oserr);
    return PyLong_FromLong(n);
}

static PyObject* Port_send_break(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "duration", NULL };
    PyObject* dur_obj;
    int64_t dur_ns;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kw, &dur_obj)) return NULL;
    if (ps_seconds(dur_obj, "duration", 0, &dur_ns) < 0) return NULL;
    int rc;
    /* Release the GIL: the break is held for the duration on the calling
     * thread. */
    Py_BEGIN_ALLOW_THREADS
    rc = yser_send_break(AS_PORT(self), dur_ns);
    Py_END_ALLOW_THREADS
    if (rc < 0) return ps_fail(rc, AS_PORT(self)->wr_oserr);
    Py_RETURN_NONE;
}

/* --- any role ----------------------------------------------------------- */

static PyObject* Port_purge(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "rx", "tx", NULL };
    int rx = 1, tx = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|pp", kw, &rx, &tx)) return NULL;
    int which = (rx ? YSER_PURGE_RX : 0) | (tx ? YSER_PURGE_TX : 0);
    int rc = yser_purge(AS_PORT(self), which);
    if (rc < 0) {
        /* The OS error lands in the slot of the role that owns the direction,
         * and in wr_oserr when both are purged. */
        int oserr = tx ? AS_PORT(self)->wr_oserr : AS_PORT(self)->rd_oserr;
        return ps_fail(rc, oserr);
    }
    Py_RETURN_NONE;
}

static PyObject* Port_interrupt(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    int rc = yser_interrupt(AS_PORT(self));
    if (rc < 0) return ps_fail(rc, AS_PORT(self)->misc_oserr);
    Py_RETURN_NONE;
}

static PyObject* Port_set_dtr(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "on", NULL };
    int on;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "p", kw, &on)) return NULL;
    int rc = yser_set_dtr(AS_PORT(self), on ? true : false);
    if (rc < 0) return ps_fail(rc, AS_PORT(self)->misc_oserr);
    Py_RETURN_NONE;
}

static PyObject* Port_set_rts(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "on", NULL };
    int on;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "p", kw, &on)) return NULL;
    int rc = yser_set_rts(AS_PORT(self), on ? true : false);
    if (rc < 0) return ps_fail(rc, AS_PORT(self)->misc_oserr);
    Py_RETURN_NONE;
}

static PyObject* Port_get_lines(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    int mask = yser_get_lines(AS_PORT(self));
    if (mask < 0) return ps_fail(mask, AS_PORT(self)->misc_oserr);
    return PyLong_FromLong(mask);
}

/* --- lifecycle / context manager ---------------------------------------- */

static PyObject* Port_close(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    /* yser_close joins the async worker and writes any pending trailing byte,
     * so it can block in the driver; do not hold the GIL through that. */
    Py_BEGIN_ALLOW_THREADS
    yser_close(AS_PORT(self));
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

static PyObject* Port_enter(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    Py_INCREF(self);
    return self;
}

static PyObject* Port_exit(PyObject* self, PyObject* Py_UNUSED(args)) {
    Py_BEGIN_ALLOW_THREADS
    yser_close(AS_PORT(self));
    Py_END_ALLOW_THREADS
    Py_RETURN_FALSE; /* don't suppress exceptions */
}

/* --- read-only properties ------------------------------------------------ */

static PyObject* Port_get_is_open(PyObject* self, void* Py_UNUSED(closure)) {
    return PyBool_FromLong(yser_is_open(AS_PORT(self)));
}
static PyObject* Port_get_device(PyObject* self, void* Py_UNUSED(closure)) {
    return PyUnicode_FromString(AS_PORT(self)->device);
}
static PyObject* Port_get_baud(PyObject* self, void* Py_UNUSED(closure)) {
    return PyLong_FromUnsignedLong(AS_PORT(self)->baud);
}
static PyObject* Port_get_data_bits(PyObject* self, void* Py_UNUSED(closure)) {
    return PyLong_FromLong(AS_PORT(self)->data_bits);
}
static PyObject* Port_get_parity(PyObject* self, void* Py_UNUSED(closure)) {
    return PyLong_FromLong((long)AS_PORT(self)->parity);
}
static PyObject* Port_get_stop_bits(PyObject* self, void* Py_UNUSED(closure)) {
    return PyLong_FromLong((long)AS_PORT(self)->stop_bits);
}
static PyObject* Port_get_flow(PyObject* self, void* Py_UNUSED(closure)) {
    return PyLong_FromLong((long)AS_PORT(self)->flow);
}
static PyObject* Port_get_write_timeout(PyObject* self, void* Py_UNUSED(closure)) {
    return ps_ns_to_seconds(AS_PORT(self)->write_timeout_ns);
}
static PyObject* Port_get_sched(PyObject* self, void* Py_UNUSED(closure)) {
    yser_port* p = AS_PORT(self);
    return Py_BuildValue("{s:d,s:d,s:d}",
                         "runtime",  (double)p->sched.runtime_ns / 1e9,
                         "deadline", (double)p->sched.deadline_ns / 1e9,
                         "period",   (double)p->sched.period_ns / 1e9);
}
static PyObject* Port_get_low_latency(PyObject* self, void* Py_UNUSED(closure)) {
    return PyBool_FromLong(AS_PORT(self)->low_latency);
}
static PyObject* Port_get_keep_dtr(PyObject* self, void* Py_UNUSED(closure)) {
    return PyBool_FromLong(AS_PORT(self)->keep_dtr_on_close);
}
static PyObject* Port_get_async_policy(PyObject* self, void* Py_UNUSED(closure)) {
    return PyLong_FromLong((long)AS_PORT(self)->async_policy);
}
static PyObject* Port_get_rd_oserr(PyObject* self, void* Py_UNUSED(closure)) {
    return PyLong_FromLong(AS_PORT(self)->rd_oserr);
}
static PyObject* Port_get_wr_oserr(PyObject* self, void* Py_UNUSED(closure)) {
    return PyLong_FromLong(AS_PORT(self)->wr_oserr);
}
static PyObject* Port_get_misc_oserr(PyObject* self, void* Py_UNUSED(closure)) {
    return PyLong_FromLong(AS_PORT(self)->misc_oserr);
}

static PyGetSetDef Port_getset[] = {
    { "is_open",          Port_get_is_open,          NULL, "True while the port is open.", NULL },
    { "device",           Port_get_device,           NULL, "Device name as opened.", NULL },
    { "baud",             Port_get_baud,             NULL, "Effective baud rate.", NULL },
    { "data_bits",        Port_get_data_bits,        NULL, "Effective data bits (5..8).", NULL },
    { "parity",           Port_get_parity,           NULL, "Effective parity (PARITY_*).", NULL },
    { "stop_bits",        Port_get_stop_bits,        NULL, "Effective stop bits (STOP_BITS_*).", NULL },
    { "flow",             Port_get_flow,             NULL, "Effective flow control (FLOW_*).", NULL },
    { "write_timeout",    Port_get_write_timeout,    NULL,
      "Write timeout in seconds (math.inf: none); immutable while the port is "
      "open. The OS waits in whole ms, so it rounds up to the next ms.", NULL },
    { "sched",            Port_get_sched,            NULL,
      "Effective async-worker RT params (Linux SCHED_DEADLINE) in seconds, as "
      "a dict {runtime, deadline, period}.", NULL },
    { "low_latency",      Port_get_low_latency,      NULL,
      "True if the driver accepted the low-latency request.", NULL },
    { "keep_dtr_on_close", Port_get_keep_dtr,        NULL,
      "True if DTR stays asserted after close (POSIX only).", NULL },
    { "async_policy",     Port_get_async_policy,     NULL,
      "Scheduling policy the async-pulse worker obtained (ASYNC_*). Log it "
      "with timing data: DEADLINE is refused without CAP_SYS_NICE or under "
      "CPU pinning, and the library falls back silently.", NULL },
    { "rd_oserr",         Port_get_rd_oserr,         NULL,
      "OS error number of the last reader-side failure.", NULL },
    { "wr_oserr",         Port_get_wr_oserr,         NULL,
      "OS error number of the last writer-side failure.", NULL },
    { "misc_oserr",       Port_get_misc_oserr,       NULL,
      "OS error number of the last any-role failure (interrupt, lines, DTR/RTS).", NULL },
    { NULL }
};

static PyMethodDef Port_methods[] = {
    { "read",        (PyCFunction)Port_read,        METH_VARARGS | METH_KEYWORDS,
      "read(n, timeout, all=False) -> bytes: read up to n bytes. all=False "
      "returns on the first bytes available (b'' on timeout); all=True keeps "
      "reading until n bytes or the timeout. timeout is in seconds: 0 polls, "
      "TIMEOUT_INFINITE (math.inf) blocks, a negative value raises ValueError; "
      "a wait rounds up to the next ms. Releases the GIL." },
    { "write",       (PyCFunction)Port_write,       METH_VARARGS | METH_KEYWORDS,
      "write(data) -> int: write a bytes-like object (or any iterable of ints "
      "0..255); returns the count the driver accepted (< len on write "
      "timeout). Releases the GIL." },
    { "write_byte",  Port_write_byte,               METH_O,
      "write_byte(value) -> int: write one byte (0..255); returns 1, or 0 on "
      "write timeout." },
    { "pulse",       (PyCFunction)Port_pulse,       METH_VARARGS | METH_KEYWORDS,
      "pulse(on, off, width) -> int: write on, block width seconds, write "
      "off. Returns 2 on success. Releases the GIL." },
    { "pulse_async", (PyCFunction)Port_pulse_async, METH_VARARGS | METH_KEYWORDS,
      "pulse_async(on, off, width) -> int: write on now and return; the worker "
      "thread writes off after width seconds. Returns 1 when the onset was "
      "accepted." },
    { "available",   Port_available,                METH_NOARGS,
      "available() -> int: bytes waiting in the driver's receive buffer." },
    { "drain",       Port_drain,                    METH_NOARGS,
      "drain(): block until the driver's transmit buffer is empty." },
    { "purge",       (PyCFunction)Port_purge,       METH_VARARGS | METH_KEYWORDS,
      "purge(rx=True, tx=False): discard buffered bytes." },
    { "interrupt",   Port_interrupt,                METH_NOARGS,
      "interrupt(): wake a read() blocked on another thread; that read raises "
      "Interrupted. The wake is latched until one read consumes it." },
    { "set_dtr",     (PyCFunction)Port_set_dtr,     METH_VARARGS | METH_KEYWORDS,
      "set_dtr(on): drive the DTR output line." },
    { "set_rts",     (PyCFunction)Port_set_rts,     METH_VARARGS | METH_KEYWORDS,
      "set_rts(on): drive the RTS output line (ignored under FLOW_RTSCTS)." },
    { "get_lines",   Port_get_lines,                METH_NOARGS,
      "get_lines() -> int: input lines as a LINE_* mask." },
    { "send_break",  (PyCFunction)Port_send_break,  METH_VARARGS | METH_KEYWORDS,
      "send_break(duration): hold TX in the break state for duration seconds." },
    { "close",       Port_close,                    METH_NOARGS,
      "close(): release the port (idempotent). Both roles must have returned." },
    { "__enter__",   Port_enter,                    METH_NOARGS, NULL },
    { "__exit__",    Port_exit,                     METH_VARARGS, NULL },
    { NULL }
};

static PyType_Slot Port_slots[] = {
    { Py_tp_doc,     (void*)"Serial port handle. Port(device, baud=0, data_bits=0, "
                            "parity=PARITY_NONE, stop_bits=STOP_BITS_1, flow=FLOW_NONE, "
                            "write_timeout=DEFAULT_WRITE_TIMEOUT, exclusive=False, "
                            "low_latency=False, dtr_low_on_open=False, "
                            "rts_low_on_open=False, keep_dtr_on_close=False, "
                            "rt_runtime=0, rt_deadline=0, rt_period=0). Times are "
                            "seconds; write_timeout=math.inf never times out. A zero "
                            "field means the library default." },
    { Py_tp_new,     (void*)PyType_GenericNew },
    { Py_tp_init,    (void*)Port_init },
    { Py_tp_dealloc, (void*)Port_dealloc },
    { Py_tp_methods, Port_methods },
    { Py_tp_getset,  Port_getset },
    { 0, NULL }
};

static PyType_Spec Port_spec = {
    .name = "ysp.serial.Port",
    .basicsize = sizeof(PortObject),
    .itemsize = 0,
    .flags = Py_TPFLAGS_DEFAULT,
    .slots = Port_slots,
};

/* --- module-level functions --------------------------------------------- */

/* Build the list[dict] both enumeration entry points return. */
static PyObject* ps_info_list(const yser_port_info* v, int n) {
    PyObject* list = PyList_New(n > 0 ? n : 0);
    if (!list) return NULL;
    for (int i = 0; i < n; i++) {
        PyObject* d = Py_BuildValue("{s:s,s:s,s:s,s:s,s:i,s:i}",
                                    "name",          v[i].name,
                                    "description",   v[i].description,
                                    "serial_number", v[i].serial_number,
                                    "location",      v[i].location,
                                    "vid",           (int)v[i].vid,
                                    "pid",           (int)v[i].pid);
        if (!d) { Py_DECREF(list); return NULL; }
        PyList_SetItem(list, i, d); /* steals ref to d */
    }
    return list;
}

/* Count, allocate, enumerate. `n` can shrink between the two calls when a port
 * vanishes, and the list must be sized from the final count: a list created
 * from the first count and then trimmed would keep NULL slots, which crash on
 * iteration. */
static PyObject* ps_enumerate(const yser_port_filter* filter) {
    int n = filter ? yser_find_ports(filter, NULL, 0) : yser_list_ports(NULL, 0);
    if (n < 0) { PyErr_SetString(PsError, yser_strerror(n)); return NULL; }
    yser_port_info* v = NULL;
    if (n > 0) {
        v = (yser_port_info*)PyMem_Malloc((size_t)n * sizeof(*v));
        if (!v) return PyErr_NoMemory();
        int got = filter ? yser_find_ports(filter, v, n) : yser_list_ports(v, n);
        if (got < 0) { PyMem_Free(v); PyErr_SetString(PsError, yser_strerror(got)); return NULL; }
        if (got < n) n = got;
    }
    PyObject* list = ps_info_list(v, n);
    PyMem_Free(v);
    return list;
}

static PyObject* mod_list_ports(PyObject* Py_UNUSED(self), PyObject* Py_UNUSED(args)) {
    return ps_enumerate(NULL);
}

static PyObject* mod_find_ports(PyObject* Py_UNUSED(self), PyObject* args, PyObject* kwds) {
    static char* kw[] = { "vid", "pid", "serial_number", "location",
                          "description", "name", NULL };
    unsigned int vid = 0, pid = 0;
    const char *serial_number = NULL, *location = NULL, *description = NULL, *name = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|IIzzzz", kw, &vid, &pid,
                                     &serial_number, &location, &description, &name))
        return NULL;
    if (vid > 0xFFFFu || pid > 0xFFFFu) {
        PyErr_SetString(PyExc_ValueError, "vid and pid must fit in 16 bits");
        return NULL;
    }
    yser_port_filter f;
    memset(&f, 0, sizeof(f));
    f.vid = (uint16_t)vid;
    f.pid = (uint16_t)pid;
    f.serial_number = serial_number;
    f.location = location;
    f.description = description;
    f.name = name;
    return ps_enumerate(&f);
}

/* The clock the library times its own deadlines against. Exposed so a caller
 * can bracket a write with the same time base instead of mixing in
 * time.monotonic(), which is a different clock on Windows. Seconds in a
 * double step by 1.9 ns between 97 and 194 days of uptime and by 3.7 ns up
 * to 388 days: finer than QueryPerformanceCounter's 100 ns tick. */
static PyObject* mod_now(PyObject* Py_UNUSED(self), PyObject* Py_UNUSED(args)) {
    return PyFloat_FromDouble((double)yrt_now_ns() / 1e9);
}

static PyMethodDef module_methods[] = {
    { "now", mod_now, METH_NOARGS,
      "now() -> float: monotonic seconds from the clock the library uses for "
      "its own deadlines (yrt_now_ns: CLOCK_MONOTONIC / "
      "QueryPerformanceCounter). Bracket a write with it: the physical onset "
      "lies after the first reading, and the difference bounds the syscall "
      "cost." },
    { "list_ports", mod_list_ports, METH_NOARGS,
      "list_ports() -> list[dict]: enumerate serial ports without opening them. "
      "Each dict has 'name', 'description', 'serial_number', 'location', "
      "'vid', 'pid'; fields the platform cannot determine are '' or 0." },
    { "find_ports", (PyCFunction)mod_find_ports, METH_VARARGS | METH_KEYWORDS,
      "find_ports(vid=0, pid=0, serial_number=None, location=None, "
      "description=None, name=None) -> list[dict]: the subset of list_ports() "
      "that matches. Zero/None matches anything; serial_number is an exact "
      "match, location a prefix, description and name substrings, all "
      "case-insensitive." },
    { NULL }
};

static PyModuleDef ysp_serial_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "ysp.serial",
    .m_doc = "Serial-port byte I/O for trigger and response boxes.",
    .m_size = -1,
    .m_methods = module_methods,
};

/* Add an exception type, keeping one reference for the static pointer and
 * giving one to the module. */
static PyObject* ps_add_exc(PyObject* m, const char* qualname, const char* attr,
                            PyObject* base) {
    PyObject* e = PyErr_NewException(qualname, base, NULL);
    if (!e) return NULL;
    Py_INCREF(e);
    if (PyModule_AddObject(m, attr, e) < 0) { Py_DECREF(e); Py_DECREF(e); return NULL; }
    return e;
}

/* PyModule_AddIntConstant takes a long, which is 32-bit and signed on Windows;
 * the unsigned constants go in as objects instead. */
static int ps_add_obj(PyObject* m, const char* name, PyObject* o) {
    if (!o) return -1;
    if (PyModule_AddObject(m, name, o) < 0) { Py_DECREF(o); return -1; }
    return 0;
}
static int ps_add_uint(PyObject* m, const char* name, unsigned long long v) {
    return ps_add_obj(m, name, PyLong_FromUnsignedLongLong(v));
}
static int ps_add_seconds(PyObject* m, const char* name, int64_t ns) {
    return ps_add_obj(m, name, ps_ns_to_seconds(ns));
}

/* The init function is named after the last dotted component: the loader looks
 * for PyInit_serial in ysp/serial.abi3.so. */
PyMODINIT_FUNC PyInit_serial(void) {
    PyObject* m = PyModule_Create(&ysp_serial_module);
    if (!m) return NULL;

    PyObject* type = PyType_FromSpec(&Port_spec);
    if (!type) { Py_DECREF(m); return NULL; }
    if (PyModule_AddObject(m, "Port", type) < 0) {
        Py_DECREF(type); Py_DECREF(m); return NULL;
    }

    /* Disconnected / Interrupted / Closed derive from Error, so `except
     * ps.Error` still catches everything the library raises. */
    PsError = ps_add_exc(m, "ysp.serial.Error", "Error", NULL);
    if (!PsError) { Py_DECREF(m); return NULL; }
    PsDisconnected = ps_add_exc(m, "ysp.serial.Disconnected", "Disconnected", PsError);
    if (!PsDisconnected) { Py_DECREF(m); return NULL; }
    PsInterrupted = ps_add_exc(m, "ysp.serial.Interrupted", "Interrupted", PsError);
    if (!PsInterrupted) { Py_DECREF(m); return NULL; }
    PsClosed = ps_add_exc(m, "ysp.serial.Closed", "Closed", PsError);
    if (!PsClosed) { Py_DECREF(m); return NULL; }

    /* parity */
    PyModule_AddIntConstant(m, "PARITY_NONE",  YSER_PARITY_NONE);
    PyModule_AddIntConstant(m, "PARITY_ODD",   YSER_PARITY_ODD);
    PyModule_AddIntConstant(m, "PARITY_EVEN",  YSER_PARITY_EVEN);
    PyModule_AddIntConstant(m, "PARITY_MARK",  YSER_PARITY_MARK);
    PyModule_AddIntConstant(m, "PARITY_SPACE", YSER_PARITY_SPACE);
    /* stop bits */
    PyModule_AddIntConstant(m, "STOP_BITS_1", YSER_STOP_BITS_1);
    PyModule_AddIntConstant(m, "STOP_BITS_2", YSER_STOP_BITS_2);
    /* flow control */
    PyModule_AddIntConstant(m, "FLOW_NONE",    YSER_FLOW_NONE);
    PyModule_AddIntConstant(m, "FLOW_RTSCTS",  YSER_FLOW_RTSCTS);
    PyModule_AddIntConstant(m, "FLOW_XONXOFF", YSER_FLOW_XONXOFF);
    /* input line mask (get_lines) */
    PyModule_AddIntConstant(m, "LINE_CTS", YSER_LINE_CTS);
    PyModule_AddIntConstant(m, "LINE_DSR", YSER_LINE_DSR);
    PyModule_AddIntConstant(m, "LINE_RI",  YSER_LINE_RI);
    PyModule_AddIntConstant(m, "LINE_DCD", YSER_LINE_DCD);
    /* purge flags (purge() takes rx/tx booleans; these name the raw bits) */
    PyModule_AddIntConstant(m, "PURGE_RX", YSER_PURGE_RX);
    PyModule_AddIntConstant(m, "PURGE_TX", YSER_PURGE_TX);
    /* read flags (read() takes all=; these name the raw values) */
    PyModule_AddIntConstant(m, "READ_ANY", YSER_READ_ANY);
    PyModule_AddIntConstant(m, "READ_ALL", YSER_READ_ALL);
    /* error codes, as carried by the exceptions' yser_strerror text */
    PyModule_AddIntConstant(m, "ERR_IO",           YSER_ERR_IO);
    PyModule_AddIntConstant(m, "ERR_DISCONNECTED", YSER_ERR_DISCONNECTED);
    PyModule_AddIntConstant(m, "ERR_CLOSED",       YSER_ERR_CLOSED);
    PyModule_AddIntConstant(m, "ERR_INTERRUPTED",  YSER_ERR_INTERRUPTED);
    PyModule_AddIntConstant(m, "ERR_ARG",          YSER_ERR_ARG);
    /* async-worker policy actually obtained (Port.async_policy). The values
     * are ysp/rt.h's yrt_policy, which ysp.parallel reports too, so one
     * constant means the same thing in both modules. TIME_CONSTRAINT is the
     * macOS rung. */
    PyModule_AddIntConstant(m, "ASYNC_NONE",            YRT_POLICY_NONE);
    PyModule_AddIntConstant(m, "ASYNC_DEADLINE",        YRT_POLICY_DEADLINE);
    PyModule_AddIntConstant(m, "ASYNC_TIME_CONSTRAINT", YRT_POLICY_TIME_CONSTRAINT);
    PyModule_AddIntConstant(m, "ASYNC_FIFO",            YRT_POLICY_FIFO);
    PyModule_AddIntConstant(m, "ASYNC_TIME_CRITICAL",   YRT_POLICY_TIME_CRITICAL);
    PyModule_AddIntConstant(m, "ASYNC_NORMAL",          YRT_POLICY_NORMAL);
    if (PyModule_AddStringConstant(m, "__version__", PS_VERSION) < 0) {
        Py_DECREF(m);
        return NULL;
    }
    /* defaults; times in seconds, TIMEOUT_INFINITE is math.inf */
    if (ps_add_seconds(m, "TIMEOUT_INFINITE", YSER_TIMEOUT_INFINITE) < 0 ||
        ps_add_uint(m, "DEFAULT_BAUD", YSER_DEFAULT_BAUD) < 0 ||
        ps_add_seconds(m, "DEFAULT_WRITE_TIMEOUT", YSER_DEFAULT_WRITE_TIMEOUT_NS) < 0 ||
        ps_add_seconds(m, "DEFAULT_RT_RUNTIME", YSER_DEFAULT_RT_RUNTIME_NS) < 0 ||
        ps_add_seconds(m, "DEFAULT_RT_DEADLINE", YSER_DEFAULT_RT_DEADLINE_NS) < 0 ||
        ps_add_seconds(m, "DEFAULT_RT_PERIOD", YSER_DEFAULT_RT_PERIOD_NS) < 0) {
        Py_DECREF(m);
        return NULL;
    }

    return m;
}
