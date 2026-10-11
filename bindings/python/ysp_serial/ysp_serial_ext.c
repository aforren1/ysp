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
 *         port.async_start(terminator=b"\n")   # C reader thread, stamped lines
 *         if port.async_wait(1.0):
 *             for line, t in port.async_read(): ...   # t: ps.now() seconds
 *
 * UNITS: every time is float seconds, as in every ysp binding; the C API
 * takes int64 ns, and the conversion happens here, once, at the boundary.
 *
 * THREADING: the header's one-reader-one-writer rule applies to Python threads
 * exactly as it does to C ones. read/write/drain/pulse/send_break and
 * pulse_async's onset write release the GIL, so a listener thread blocked in
 * read() does not stall the interpreter, and interrupt() from another thread
 * ends that read with ysp.serial.Interrupted.
 *
 * ASYNC READER (0.5.0.2): async_start() starts a C thread that owns the
 * reader role until async_stop() or close(). It never takes the GIL; the
 * Python side only copies finished records out of its ring.
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
#include <stdlib.h>
#include <math.h>
#include <string.h>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <pthread.h>
    #include <time.h>
#endif

struct ps_async;

typedef struct {
    PyObject_HEAD
    yser_port port;
    /* NULL until the first async_start(). Kept, with its lock, until the
     * object dies: a thread still inside async_wait() when another thread
     * stops the reader must find a live lock to leave through. */
    struct ps_async* async;
} PortObject;

/* module-level exception types; Error is the base of the other three */
static PyObject* PsError;
static PyObject* PsDisconnected;
static PyObject* PsInterrupted;
static PyObject* PsClosed;

#define AS_PORT(self) (&((PortObject*)(self))->port)

/* The distribution's version is the header's, plus a fourth component that
 * counts changes to this binding alone (.1: times in seconds, .2: the async
 * reader), so such a change gets a new version without naming a header that
 * does not exist.
 * When the header's version moves, drop the suffix here and in
 * pyproject.toml; tests/test_serial.py checks that the two agree. */
#define PS_VERSION YSER_VERSION_STRING ".2"

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

/* --- async reader --------------------------------------------------------- */
/* A Python thread blocked in read() stamps a line only when the interpreter
 * gets round to it, after the GIL and whatever else the program is doing.
 * This is the reader of ysp/device.h and of the MEX binding (ysp_serial.c,
 * v2.0.0) moved into this module: a C thread, elevated as device.h elevates
 * its reader, blocks in yser_read, stamps each chunk with yrt_now_ns() the
 * moment the read returns, cuts records (at a terminator, at a fixed size,
 * or one per chunk) and keeps them in a bounded ring that async_read()
 * empties. The ring and the chunk buffer are allocated at async_start(), so
 * the reader loop allocates nothing, and it never touches the GIL. A full
 * ring drops the new record and counts it: the reader must never wait for
 * Python. */

#if defined(_WIN32)
typedef CRITICAL_SECTION   ps_mutex;
typedef CONDITION_VARIABLE ps_cond;
static void ps_sync_init(ps_mutex* m, ps_cond* c) {
    InitializeCriticalSection(m);
    InitializeConditionVariable(c);
}
static void ps_sync_free(ps_mutex* m, ps_cond* c) { (void)c; DeleteCriticalSection(m); }
static void ps_lock(ps_mutex* m)     { EnterCriticalSection(m); }
static void ps_unlock(ps_mutex* m)   { LeaveCriticalSection(m); }
static void ps_broadcast(ps_cond* c) { WakeAllConditionVariable(c); }
/* Rounds up to whole ms, so a wait never turns into a spin; a broadcast
 * still ends it at once. */
static void ps_cond_wait(ps_cond* c, ps_mutex* m, int64_t ns) {
    (void)SleepConditionVariableCS(c, m, (DWORD)((ns + 999999) / 1000000));
}
#else
typedef pthread_mutex_t ps_mutex;
typedef pthread_cond_t  ps_cond;
static void ps_sync_init(ps_mutex* m, ps_cond* c) {
    pthread_condattr_t at;
    pthread_mutex_init(m, NULL);
    pthread_condattr_init(&at);
#if !defined(__APPLE__)
    /* A wall-clock step must not stretch a wait; macOS has no setclock and
     * waits on a relative time instead. */
    pthread_condattr_setclock(&at, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(c, &at);
    pthread_condattr_destroy(&at);
}
static void ps_sync_free(ps_mutex* m, ps_cond* c) { pthread_cond_destroy(c); pthread_mutex_destroy(m); }
static void ps_lock(ps_mutex* m)     { pthread_mutex_lock(m); }
static void ps_unlock(ps_mutex* m)   { pthread_mutex_unlock(m); }
static void ps_broadcast(ps_cond* c) { pthread_cond_broadcast(c); }
static void ps_cond_wait(ps_cond* c, ps_mutex* m, int64_t ns) {
    struct timespec ts;
#if defined(__APPLE__)
    ts.tv_sec = (time_t)(ns / 1000000000);
    ts.tv_nsec = (long)(ns % 1000000000);
    (void)pthread_cond_timedwait_relative_np(c, m, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += (time_t)(ns / 1000000000);
    ts.tv_nsec += (long)(ns % 1000000000);
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    (void)pthread_cond_timedwait(c, m, &ts);
#endif
}
#endif

/* The longest a GIL-free wait lasts before the waiter looks at Python's
 * signals, so Ctrl+C reaches async_wait(math.inf). */
#define PS_SLICE_NS YRT_MS(50)
/* The reader's longest read: async_stop() ends it at once with
 * yser_interrupt; the bound only keeps a wedged driver from holding the
 * thread forever. */
#define PS_READ_SLICE_NS YRT_MS(200)
#define PS_CHUNK 4096

enum { PS_A_IDLE = 0, PS_A_RUNNING, PS_A_STOPPING };
enum { PS_CUT_TERM = 0, PS_CUT_FIXED, PS_CUT_CHUNK };

typedef struct ps_async {
    yser_port* port;
    ps_mutex   lock;
    ps_cond    cond;          /* records arrived, the reader left, or users fell */
#if defined(_WIN32)
    HANDLE     thread;
#else
    pthread_t  thread;
#endif
    /* Guarded by lock. */
    int        state;         /* PS_A_*; IDLE between runs */
    int        users;         /* Python calls inside async_read/async_wait */
    int        busy;          /* an async_read is copying [tail, head) */
    int        stop;          /* set by async_stop before its interrupt */
    int        exited;        /* the reader left its loop */
    int        err;           /* the yser_read code that ended the loop, or 0 */
    yrt_policy policy;        /* what yrt_thread_elevate gave the reader */
    /* The ring: head and tail only grow, head - tail is the fill. The reader
     * writes slot head % nslots; the consumer copies [tail, head) without the
     * lock, because the reader does not touch those slots until tail moves. */
    uint8_t*   data;          /* nslots * maxlen */
    uint16_t*  len;
    int64_t*   t_end;         /* stamp of the read that brought the last byte */
    int64_t*   t_first;       /* stamp of the read that brought the first byte */
    uint32_t   nslots, maxlen;
    uint64_t   head, tail;
    uint64_t   lines;         /* records cut, stored or not */
    uint64_t   dropped;       /* records lost to a full ring */
    uint64_t   overlong;      /* terminated records cut at maxlen */
    uint64_t   max_used;      /* the largest fill */
    uint64_t   bytes, reads;
    /* Set at start, then reader-only. */
    int        cut;           /* PS_CUT_* */
    uint8_t    term;
    uint8_t*   cur;           /* the record being assembled */
    uint32_t   cur_len;
    int        cur_cut;
    int64_t    cur_first;
    uint8_t*   chunk;         /* PS_CHUNK */
} ps_async;

#define ASYNC_OF(self) (((PortObject*)(self))->async)

/* Under lock. */
static void ps_async_push(ps_async* a, const uint8_t* src, uint32_t n, int64_t t_first,
                          int64_t t_end) {
    a->lines++;
    if (a->head - a->tail < a->nslots) {
        uint32_t s = (uint32_t)(a->head % a->nslots);
        memcpy(a->data + (size_t)s * a->maxlen, src, n);
        a->len[s] = (uint16_t)n;
        a->t_first[s] = t_first;
        a->t_end[s] = t_end;
        a->head++;
        if (a->head - a->tail > a->max_used) a->max_used = a->head - a->tail;
    } else {
        a->dropped++;
    }
}

static void ps_async_loop(ps_async* a) {
    yrt_policy pol = yrt_thread_elevate(NULL);
    ps_lock(&a->lock);
    a->policy = pol;
    ps_unlock(&a->lock);
    for (;;) {
        int n = yser_read(a->port, a->chunk, PS_CHUNK, PS_READ_SLICE_NS, YSER_READ_ANY);
        int64_t t = yrt_now_ns();
        if (n == YSER_ERR_INTERRUPTED) {
            /* Only async_stop interrupts while the reader runs (the binding
             * refuses interrupt() then), so the latched wake is consumed
             * here and no later read() sees a stale one. */
            int s;
            ps_lock(&a->lock);
            s = a->stop;
            ps_unlock(&a->lock);
            if (s) break;
            continue;
        }
        if (n < 0) {
            ps_lock(&a->lock);
            a->err = n;
            ps_unlock(&a->lock);
            break;
        }
        if (n == 0) continue;
        /* One lock per chunk, not per record: a chunk scans in well under a
         * microsecond, and the consumer holds the lock only for a snapshot. */
        uint64_t head0;
        ps_lock(&a->lock);
        head0 = a->head;
        a->bytes += (uint64_t)n;
        a->reads++;
        if (a->cut == PS_CUT_CHUNK) {
            for (int off = 0; off < n; off += (int)a->maxlen) {
                uint32_t k = (uint32_t)(n - off) < a->maxlen ? (uint32_t)(n - off) : a->maxlen;
                ps_async_push(a, a->chunk + off, k, t, t);
            }
        } else {
            for (int i = 0; i < n; i++) {
                uint8_t c = a->chunk[i];
                if (a->cur_len == 0 && !a->cur_cut) a->cur_first = t;
                if (a->cur_len < a->maxlen) a->cur[a->cur_len++] = c;
                else a->cur_cut = 1;
                if (a->cut == PS_CUT_TERM ? c == a->term : a->cur_len == a->maxlen) {
                    ps_async_push(a, a->cur, a->cur_len, a->cur_first, t);
                    if (a->cur_cut) a->overlong++;
                    a->cur_len = 0;
                    a->cur_cut = 0;
                }
            }
        }
        int pushed = a->head != head0;
        ps_unlock(&a->lock);
        if (pushed) ps_broadcast(&a->cond);
    }
    ps_lock(&a->lock);
    a->exited = 1;
    ps_unlock(&a->lock);
    ps_broadcast(&a->cond);
    yrt_thread_cleanup();
}

#if defined(_WIN32)
static DWORD WINAPI ps_async_main(LPVOID arg) { ps_async_loop((ps_async*)arg); return 0; }
#else
static void* ps_async_main(void* arg) { ps_async_loop((ps_async*)arg); return NULL; }
#endif

static void ps_async_free_ring(ps_async* a) {
    free(a->data);  free(a->len);
    free(a->t_end); free(a->t_first);
    free(a->cur);   free(a->chunk);
    a->data = NULL; a->len = NULL; a->t_end = NULL; a->t_first = NULL;
    a->cur = NULL; a->chunk = NULL;
}

/* Stop and join the reader, wait for the Python calls inside the ring to
 * leave, free the ring. Returns 1 if this call stopped a reader, 0 if none
 * was running. Call with the GIL held; it is released for the waits, which
 * may need another Python thread to finish its async_read. */
static int ps_async_stop(PyObject* self) {
    ps_async* a = ASYNC_OF(self);
    int stopped = 0;
    if (!a) return 0;
    Py_BEGIN_ALLOW_THREADS
    ps_lock(&a->lock);
    if (a->state == PS_A_RUNNING) {
        int exited = a->exited;
        a->state = PS_A_STOPPING;
        a->stop = 1;
        ps_unlock(&a->lock);
        ps_broadcast(&a->cond);   /* async_wait() callers leave with Interrupted */
        /* A reader that already left on an error is not reading, and a wake
         * sent now would stay latched for the next read(). */
        if (!exited) (void)yser_interrupt(a->port);
#if defined(_WIN32)
        WaitForSingleObject(a->thread, INFINITE);
        CloseHandle(a->thread);
#else
        pthread_join(a->thread, NULL);
#endif
        ps_lock(&a->lock);
        while (a->users > 0) ps_cond_wait(&a->cond, &a->lock, PS_SLICE_NS);
        ps_async_free_ring(a);
        a->state = PS_A_IDLE;
        stopped = 1;
    } else {
        /* Another thread is stopping it: return when it is done, so close()
         * after this call never frees the port under the reader. */
        while (a->state == PS_A_STOPPING) ps_cond_wait(&a->cond, &a->lock, PS_SLICE_NS);
    }
    ps_unlock(&a->lock);
    ps_broadcast(&a->cond);
    Py_END_ALLOW_THREADS
    return stopped;
}

/* For dealloc: the object has no other references, so no call is inside. */
static void ps_async_destroy(PyObject* self) {
    ps_async* a = ASYNC_OF(self);
    if (!a) return;
    (void)ps_async_stop(self);
    ps_sync_free(&a->lock, &a->cond);
    free(a);
    ASYNC_OF(self) = NULL;
}

/* The reader role belongs to the async reader while it runs (or until
 * async_stop() after it died): a second reader would race it for bytes and
 * for the interrupt latch. */
static int ps_async_owns_input(PyObject* self, const char* what) {
    ps_async* a = ASYNC_OF(self);
    int st;
    if (!a) return 0;
    ps_lock(&a->lock);
    st = a->state;
    ps_unlock(&a->lock);
    if (st == PS_A_IDLE) return 0;
    PyErr_Format(PsError, "%s: the async reader owns the input; call async_stop() first", what);
    return 1;
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
     * a fresh instance. The async reader goes first: it reads this port. */
    (void)ps_async_stop(self);
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
    ps_async_destroy(self);
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
    if (ps_async_owns_input(self, "read")) return NULL;
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
    if (ps_async_owns_input(self, "available")) return NULL;
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
    if (rx && ps_async_owns_input(self, "purge(rx=True)")) return NULL;
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
    if (ps_async_owns_input(self, "interrupt")) return NULL;
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

/* --- async reader: Python side ------------------------------------------ */

static PyObject* Port_async_start(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "terminator", "capacity", "max_len", "record_size", NULL };
    PyObject* term_obj = NULL;
    Py_ssize_t capacity = 4096, max_len = 256, record_size = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|Onnn", kw, &term_obj, &capacity,
                                     &max_len, &record_size))
        return NULL;
    if (capacity < 1 || capacity > (1 << 24)) {
        PyErr_SetString(PyExc_ValueError, "capacity: records, 1..16777216");
        return NULL;
    }
    if (max_len < 1 || max_len > 65535) {
        PyErr_SetString(PyExc_ValueError, "max_len: bytes, 1..65535");
        return NULL;
    }
    if (record_size < 0 || record_size > 65535) {
        PyErr_SetString(PyExc_ValueError, "record_size: bytes, 0..65535");
        return NULL;
    }
    /* The default terminator is b"\n", unless record_size asks for fixed
     * records; naming both is ambiguous and refused. */
    int cut = record_size > 0 ? PS_CUT_FIXED : PS_CUT_TERM;
    unsigned long term = '\n';
    if (term_obj == Py_None) {
        if (cut != PS_CUT_FIXED) cut = PS_CUT_CHUNK;
    } else if (term_obj) {
        if (cut == PS_CUT_FIXED) {
            PyErr_SetString(PyExc_ValueError,
                            "terminator and record_size: give one (record_size cuts fixed records)");
            return NULL;
        }
        if (PyBytes_Check(term_obj) && PyBytes_Size(term_obj) == 1) {
            term = (unsigned char)PyBytes_AsString(term_obj)[0];
        } else if (PyLong_Check(term_obj)) {
            term = PyLong_AsUnsignedLong(term_obj);
            if (term == (unsigned long)-1 && PyErr_Occurred()) return NULL;
            if (term > 0xFF) {
                PyErr_SetString(PyExc_ValueError, "terminator: one byte, 0..255");
                return NULL;
            }
        } else {
            PyErr_SetString(PyExc_TypeError, "terminator: one byte (b'\\n' or 10), or None");
            return NULL;
        }
    }
    if (!yser_is_open(AS_PORT(self))) return ps_fail(YSER_ERR_CLOSED, 0);

    ps_async* a = ASYNC_OF(self);
    if (!a) {
        a = (ps_async*)calloc(1, sizeof(*a));
        if (!a) return PyErr_NoMemory();
        ps_sync_init(&a->lock, &a->cond);
        ASYNC_OF(self) = a;
    }
    ps_lock(&a->lock);
    int st = a->state;
    ps_unlock(&a->lock);
    if (st != PS_A_IDLE) {
        PyErr_SetString(PsError, "async_start: the async reader is already running");
        return NULL;
    }
    /* Only this thread (holding the GIL) moves the state out of IDLE, so the
     * fields can be set without the lock until the thread exists. */
    uint32_t nslots = (uint32_t)capacity;
    uint32_t maxlen = (uint32_t)(cut == PS_CUT_FIXED ? record_size : max_len);
    a->data = (uint8_t*)malloc((size_t)nslots * maxlen);
    a->len = (uint16_t*)calloc(nslots, sizeof(uint16_t));
    a->t_end = (int64_t*)calloc(nslots, sizeof(int64_t));
    a->t_first = (int64_t*)calloc(nslots, sizeof(int64_t));
    a->cur = (uint8_t*)malloc(maxlen);
    a->chunk = (uint8_t*)malloc(PS_CHUNK);
    if (!a->data || !a->len || !a->t_end || !a->t_first || !a->cur || !a->chunk) {
        ps_async_free_ring(a);
        return PyErr_NoMemory();
    }
    a->port = AS_PORT(self);
    a->nslots = nslots;
    a->maxlen = maxlen;
    a->cut = cut;
    a->term = (uint8_t)term;
    a->head = a->tail = 0;
    a->lines = a->dropped = a->overlong = a->max_used = a->bytes = a->reads = 0;
    a->users = a->busy = a->stop = a->exited = a->err = 0;
    a->policy = YRT_POLICY_NONE;
    a->cur_len = 0;
    a->cur_cut = 0;
    a->cur_first = 0;
    a->state = PS_A_RUNNING;
#if defined(_WIN32)
    a->thread = CreateThread(NULL, 0, ps_async_main, a, 0, NULL);
    int ok = a->thread != NULL;
#else
    int ok = pthread_create(&a->thread, NULL, ps_async_main, a) == 0;
#endif
    if (!ok) {
        a->state = PS_A_IDLE;
        ps_async_free_ring(a);
        PyErr_SetString(PsError, "async_start: could not start the reader thread");
        return NULL;
    }
    Py_RETURN_NONE;
}

/* Enter the ring as a user, or raise. Returns NULL with an exception set. */
static ps_async* ps_async_enter(PyObject* self, const char* what) {
    ps_async* a = ASYNC_OF(self);
    if (a) {
        ps_lock(&a->lock);
        if (a->state == PS_A_RUNNING) {
            a->users++;
            ps_unlock(&a->lock);
            return a;
        }
        ps_unlock(&a->lock);
    }
    if (!yser_is_open(AS_PORT(self))) { (void)ps_fail(YSER_ERR_CLOSED, 0); return NULL; }
    PyErr_Format(PsError, "%s: the async reader is not running (call async_start())", what);
    return NULL;
}

/* Under lock. A stopper may be waiting for the last user. */
static void ps_async_leave_locked(ps_async* a) {
    a->users--;
    if (a->users == 0 && a->state == PS_A_STOPPING) ps_broadcast(&a->cond);
}

static PyObject* Port_async_read(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "max", "first", NULL };
    Py_ssize_t max = 0;
    int first = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|np", kw, &max, &first)) return NULL;
    if (max < 0) { PyErr_SetString(PyExc_ValueError, "max: records, >= 0 (0: all)"); return NULL; }
    ps_async* a = ps_async_enter(self, "async_read");
    if (!a) return NULL;
    uint64_t tail, head;
    int err, exited;
    ps_lock(&a->lock);
    if (a->busy) {
        ps_async_leave_locked(a);
        ps_unlock(&a->lock);
        PyErr_SetString(PsError, "async_read: another thread is in async_read on this port");
        return NULL;
    }
    /* busy keeps a second consumer out while this one builds objects: the
     * allocations below can run a GC pass and switch threads. */
    a->busy = 1;
    tail = a->tail;
    head = a->head;
    err = a->err;
    exited = a->exited;
    ps_unlock(&a->lock);

    uint64_t n = head - tail;
    if (max > 0 && n > (uint64_t)max) n = (uint64_t)max;
    PyObject* out = PyList_New((Py_ssize_t)n);
    for (uint64_t i = 0; out && i < n; i++) {
        uint32_t s = (uint32_t)((tail + i) % a->nslots);
        /* Not Py_BuildValue's "y#": the abi3 build against CPython 3.14
         * headers raised SystemError ("PY_SSIZE_T_CLEAN macro must be
         * defined") for it, although this file defines the macro. */
        PyObject* data = PyBytes_FromStringAndSize((const char*)a->data + (size_t)s * a->maxlen,
                                                   (Py_ssize_t)a->len[s]);
        PyObject* rec = !data ? NULL
            : first ? Py_BuildValue("(Ndd)", data, (double)a->t_end[s] / 1e9,
                                    (double)a->t_first[s] / 1e9)
                    : Py_BuildValue("(Nd)", data, (double)a->t_end[s] / 1e9);
        if (!rec) { Py_CLEAR(out); break; }
        PyList_SetItem(out, (Py_ssize_t)i, rec);   /* steals rec */
    }
    ps_lock(&a->lock);
    if (out) a->tail = tail + n;   /* on failure the records stay for the next call */
    a->busy = 0;
    ps_async_leave_locked(a);
    ps_unlock(&a->lock);
    if (!out) return NULL;
    /* A reader that died (unplug, I/O error) says so once its records are
     * out, so nothing it collected is lost to the error. */
    if (n == 0 && exited && err < 0) {
        Py_DECREF(out);
        return ps_fail(err, AS_PORT(self)->rd_oserr);
    }
    return out;
}

static PyObject* Port_async_wait(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kw[] = { "timeout", NULL };
    PyObject* timeout_obj;
    int64_t timeout_ns;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kw, &timeout_obj)) return NULL;
    if (ps_seconds(timeout_obj, "timeout", PS_S_INF | PS_S_POS, &timeout_ns) < 0) return NULL;
    ps_async* a = ps_async_enter(self, "async_wait");
    if (!a) return NULL;
    int64_t deadline = timeout_ns == YSER_TIMEOUT_INFINITE ? -1 : yrt_now_ns() + timeout_ns;
    uint64_t n = 0;
    int stop = 0, exited = 0, err = 0, timed_out = 0;
    for (;;) {
        Py_BEGIN_ALLOW_THREADS
        ps_lock(&a->lock);
        /* At most one slice per GIL release, so signals get a look in. */
        for (int waited = 0;; waited = 1) {
            n = a->head - a->tail;
            stop = a->stop;
            exited = a->exited;
            if (n || stop || exited) break;
            int64_t now = yrt_now_ns();
            if (deadline >= 0 && now >= deadline) { timed_out = 1; break; }
            if (waited) break;
            int64_t slice = PS_SLICE_NS;
            if (deadline >= 0 && deadline - now < slice) slice = deadline - now;
            ps_cond_wait(&a->cond, &a->lock, slice);
        }
        ps_unlock(&a->lock);
        Py_END_ALLOW_THREADS
        if (n || stop || exited || timed_out) break;
        if (PyErr_CheckSignals() < 0) break;
    }
    ps_lock(&a->lock);
    err = a->err;
    ps_async_leave_locked(a);
    ps_unlock(&a->lock);
    if (PyErr_Occurred()) return NULL;
    if (n) return PyLong_FromUnsignedLongLong(n);
    if (stop) {
        PyErr_SetString(PsInterrupted, "async_wait: the async reader was stopped");
        return NULL;
    }
    if (exited && err < 0) return ps_fail(err, AS_PORT(self)->rd_oserr);
    return PyLong_FromLong(0);
}

/* The reader's counters as a dict, or None before the first async_start. */
static PyObject* ps_async_stats(PyObject* self) {
    ps_async* a = ASYNC_OF(self);
    if (!a) Py_RETURN_NONE;
    unsigned long long pending, max_used, lines, dropped, overlong, bytes, reads;
    unsigned long capacity, maxlen;
    int running, err;
    long policy;
    ps_lock(&a->lock);
    running = a->state == PS_A_RUNNING && !a->exited;
    pending = a->head - a->tail;
    max_used = a->max_used;
    lines = a->lines;
    dropped = a->dropped;
    overlong = a->overlong;
    bytes = a->bytes;
    reads = a->reads;
    capacity = a->nslots;
    maxlen = a->maxlen;
    err = a->err;
    policy = (long)a->policy;
    ps_unlock(&a->lock);
    PyObject* error = err < 0 ? PyUnicode_FromString(yser_strerror(err)) : Py_None;
    if (!error) return NULL;
    if (error == Py_None) Py_INCREF(error);
    return Py_BuildValue("{s:O,s:k,s:k,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:N,s:l}",
                         "running", running ? Py_True : Py_False,
                         "capacity", capacity, "max_len", maxlen,
                         "pending", pending, "max_used", max_used,
                         "lines", lines, "overflows", dropped, "overlong", overlong,
                         "bytes", bytes, "reads", reads,
                         "error", error, "policy", policy);
}

static PyObject* Port_async_stop(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    if (!ps_async_stop(self)) Py_RETURN_NONE;
    /* The ring is gone; the counters stay until the next async_start. */
    return ps_async_stats(self);
}

static PyObject* Port_get_async_stats(PyObject* self, void* Py_UNUSED(closure)) {
    return ps_async_stats(self);
}

/* --- lifecycle / context manager ---------------------------------------- */

static PyObject* Port_close(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    /* yser_close joins the async worker and writes any pending trailing byte,
     * so it can block in the driver; do not hold the GIL through that. The
     * async reader stops first: closing a port another thread reads is
     * undefined on POSIX. */
    (void)ps_async_stop(self);
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
    (void)ps_async_stop(self);
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
    { "async_stats",      Port_get_async_stats,      NULL,
      "The async reader's counters as a dict, or None before the first "
      "async_start(): running, capacity, max_len, pending, max_used (the "
      "largest ring fill), lines (records cut), overflows (records lost to a "
      "full ring), overlong (lines cut at max_len), bytes, reads, error (the "
      "message that ended the reader, or None) and policy (ASYNC_*, what the "
      "reader thread's elevation obtained). Kept after async_stop() until the "
      "next async_start().", NULL },
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
    { "async_start", (PyCFunction)Port_async_start, METH_VARARGS | METH_KEYWORDS,
      "async_start(terminator=b'\\n', capacity=4096, max_len=256, record_size=0): "
      "start a C thread that reads the port, stamps each read with now() when "
      "it returns, cuts records and keeps them in a ring of `capacity` "
      "records. terminator: one byte (bytes or int); a record ends with it, "
      "and a longer one is cut at max_len bytes. record_size > 0: fixed "
      "records of that many bytes instead. terminator=None without "
      "record_size: one record per read. A full ring drops the new record "
      "and counts it in async_stats. The thread owns the reader role: read, "
      "available, interrupt and purge(rx=True) raise until async_stop(). "
      "Writes stay allowed." },
    { "async_read",  (PyCFunction)Port_async_read,  METH_VARARGS | METH_KEYWORDS,
      "async_read(max=0, first=False) -> list: take the records in the ring "
      "without waiting, oldest first, at most max (0: all). Each is "
      "(data, t): bytes and the now() seconds of the read that brought its "
      "last byte; first=True adds the stamp of the read that brought its "
      "first byte. Raises the reader's error (Disconnected, ...) once the "
      "ring is empty if the reader died." },
    { "async_wait",  (PyCFunction)Port_async_wait,  METH_VARARGS | METH_KEYWORDS,
      "async_wait(timeout) -> int: wait up to timeout seconds (0 polls, "
      "math.inf forever) for a record; returns the number in the ring, 0 on "
      "timeout. Releases the GIL. Raises Interrupted when async_stop() or "
      "close() ends the wait, and the reader's error if it died." },
    { "async_stop",  Port_async_stop,               METH_NOARGS,
      "async_stop() -> dict | None: stop and join the reader and free the "
      "ring (records not read are lost). Returns async_stats, or None when "
      "no reader was running. Idempotent; close() calls it." },
    { "close",       Port_close,                    METH_NOARGS,
      "close(): release the port (idempotent). Both roles must have returned; "
      "the async reader is stopped first." },
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
