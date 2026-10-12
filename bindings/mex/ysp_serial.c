/* ysp_serial.c - MATLAB/Octave MEX binding for ysp/serial.h (builds to ysp_serial.<mexext>)
 *
 * Command-dispatch interface in the style of widmann's ppdev-mex: the first
 * argument is a command string, the second (when needed) is an opaque uint64
 * port handle returned by 'open'.
 *
 *     ports = ysp_serial('list')                    % struct array
 *     ports = ysp_serial('find', struct('vid', 1027))
 *
 *     h = ysp_serial('open', '/dev/ttyUSB0')        % defaults (115200 8N1)
 *     h = ysp_serial('open', 'COM3', struct('baud', 9600, 'parity', 'even'))
 *
 *     n    = ysp_serial('write',     h, uint8([1 2 3]))
 *     n    = ysp_serial('writebyte', h, 85)
 *     data = ysp_serial('read',      h, 6, 0.5, 'all')  % 6-byte frame in 0.5 s
 *     data = ysp_serial('read',      h, 64, 0)          % poll, [] if nothing
 *     ysp_serial('pulse',      h, 85, 0, 0.002)         % blocking 2 ms pulse
 *     ysp_serial('pulseasync', h, 85, 0, 0.002)         % non-blocking
 *     t    = ysp_serial('now')                          % library clock (s)
 *     ysp_serial('async_start', h, 10)                  % reader thread, lines
 *     [lines, t, t_first] = ysp_serial('async_read', h) % stamps in s
 *     st   = ysp_serial('async_stop', h)
 *     ysp_serial('close',      h)
 *
 * Units: every time the binding takes or returns is a double in seconds on
 * the ysp/rt.h clock, as in the other bindings; the C API under it takes
 * int64 ns, and the conversion happens here, rounded to the nearest ns. A
 * timeout is >= 0 (0 polls) or Inf (forever); NaN and negative values are
 * refused rather than mapped, so a sign error cannot turn into a poll or a
 * hang. Binding v2.0.0 made this change; v1 took ms and us.
 *
 * Build with build.m (MATLAB or Octave). See README.md.
 *
 * NOTE: the handle table is freed (and every open port closed) on 'clear mex'
 * via mexAtExit, so a forgotten close cannot leave an async worker thread
 * pointing at a freed port. A handle is a small counter, not a pointer: a
 * freed port struct can be reallocated at the same address, and a stale
 * pointer handle would then silently drive the port that took its place.
 *
 * NOTE: a MEX call owns the interpreter thread for its whole duration. MATLAB
 * timer callbacks and Octave's event loop are dispatched on that same thread,
 * so nothing in the interpreter can run, let alone call 'interrupt', while a
 * 'read' is blocked here. Never pass Inf (forever) from MATLAB or Octave:
 * poll in short slices instead (see README.md). 'interrupt' latches a
 * wake that the NEXT read consumes, so from one thread it only makes that
 * read return immediately; it exists for symmetry with the C API.
 */
/* clock_gettime/nanosleep/pthread_* and the termios extensions (CRTSCTS,
 * CMSPAR, the B* rates above 38400) live behind this feature macro, and
 * ysp/serial.h can only set it for itself when it is the first include, which
 * it is not here. Guarded because some mex toolchains (MATLAB) already define
 * it on the command line, while others (Octave mkoctfile) do not. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "mex.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#define YSP_SERIAL_IMPLEMENTATION
#include "ysp/serial.h"

/* The binding's own version, apart from ysp/serial.h's: 2.0.0 moved every
 * time to seconds (v1 took ms and us) and added the async reader; 2.1.0
 * adds the min_hold setting of ysp/serial.h v0.6.0; 2.2.0 adds the port's
 * driver and rts_via_dtr ('info', 'list', 'find') of v0.6.1. */
#define PS_BINDING_VERSION "2.2.0"

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <pthread.h>
#endif

/* ---- async reader ------------------------------------------------------- */
/* MATLAB runs every MEX call on its one interpreter thread, so a blocking
 * 'read' cannot stamp a line when it arrives, only when the script gets round
 * to the read. The async reader is the C pattern of ysp/device.h moved into
 * the binding: a thread of its own (elevated as device.h elevates its reader)
 * blocks in yser_read, stamps each chunk with yrt_now_ns() the moment the
 * read returns, cuts lines at the terminator and keeps them in a bounded ring
 * that 'async_read' empties. The ring and the line buffer are allocated at
 * 'async_start', so the reader loop allocates nothing. The thread owns the
 * reader role of serial.h's THREADING while it runs; the MATLAB thread keeps
 * the writer role. */

#if defined(_WIN32)
typedef CRITICAL_SECTION ps_mutex;
static void ps_lock_init(ps_mutex* m) { InitializeCriticalSection(m); }
static void ps_lock_free(ps_mutex* m) { DeleteCriticalSection(m); }
static void ps_lock(ps_mutex* m)      { EnterCriticalSection(m); }
static void ps_unlock(ps_mutex* m)    { LeaveCriticalSection(m); }
#else
typedef pthread_mutex_t ps_mutex;
static void ps_lock_init(ps_mutex* m) { pthread_mutex_init(m, NULL); }
static void ps_lock_free(ps_mutex* m) { pthread_mutex_destroy(m); }
static void ps_lock(ps_mutex* m)      { pthread_mutex_lock(m); }
static void ps_unlock(ps_mutex* m)    { pthread_mutex_unlock(m); }
#endif

typedef struct ps_async {
    yser_port* port;
    ps_mutex   lock;
#if defined(_WIN32)
    HANDLE     thread;
#else
    pthread_t  thread;
#endif
    /* Ring of line slots, filled by the reader, emptied by async_read. head
     * and tail only grow; head - tail is the fill. Guarded by lock. */
    uint8_t*   data;          /* nslots * maxlen */
    uint16_t*  len;
    int64_t*   t_first;       /* stamp of the read that brought the first byte */
    int64_t*   t_end;         /* stamp of the read that brought the terminator */
    uint32_t   nslots, maxlen;
    uint64_t   head, tail;
    uint64_t   dropped;       /* complete lines lost to a full ring */
    uint64_t   overlong;      /* lines cut at maxlen */
    uint64_t   bytes, reads;
    int        stop;          /* set by async_stop before its interrupt */
    int        exited;        /* the reader left its loop */
    int        err;           /* the yser_read code that ended the loop, or 0 */
    /* Reader-only state: the line being assembled. */
    uint8_t*   cur;
    uint32_t   cur_len;
    int        cur_cut;
    int64_t    cur_first;
    uint8_t    term;
    uint8_t    chunk[4096];
} ps_async;

static void ps_async_push(ps_async* a, int64_t t_end) {
    ps_lock(&a->lock);
    if (a->head - a->tail < a->nslots) {
        uint32_t s = (uint32_t)(a->head % a->nslots);
        memcpy(a->data + (size_t)s * a->maxlen, a->cur, a->cur_len);
        a->len[s] = (uint16_t)a->cur_len;
        a->t_first[s] = a->cur_first;
        a->t_end[s] = t_end;
        a->head++;
    } else {
        a->dropped++;
    }
    if (a->cur_cut) a->overlong++;
    ps_unlock(&a->lock);
    a->cur_len = 0;
    a->cur_cut = 0;
}

static void ps_async_loop(ps_async* a) {
    (void)yrt_thread_elevate(NULL);
    for (;;) {
        /* Finite slices only so a wedged driver cannot hold the thread past
         * async_stop forever; the interrupt from async_stop is what normally
         * ends the wait. */
        int n = yser_read(a->port, a->chunk, (int)sizeof a->chunk, YRT_MS(200), YSER_READ_ANY);
        int64_t t = yrt_now_ns();
        if (n == YSER_ERR_INTERRUPTED) {
            /* The loop leaves only on the interrupt that async_stop sends after
             * it sets stop, so the latched wake is always consumed here and no
             * later 'read' on the MATLAB thread sees a stale one. */
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
        a->bytes += (uint64_t)n;
        a->reads++;
        for (int i = 0; i < n; i++) {
            uint8_t c = a->chunk[i];
            if (a->cur_len == 0 && !a->cur_cut) a->cur_first = t;
            if (a->cur_len < a->maxlen) a->cur[a->cur_len++] = c;
            else a->cur_cut = 1;
            if (c == a->term) ps_async_push(a, t);
        }
    }
    ps_lock(&a->lock);
    a->exited = 1;
    ps_unlock(&a->lock);
    yrt_thread_cleanup();
}

#if defined(_WIN32)
static DWORD WINAPI ps_async_main(LPVOID arg) { ps_async_loop((ps_async*)arg); return 0; }
#else
static void* ps_async_main(void* arg) { ps_async_loop((ps_async*)arg); return NULL; }
#endif

static void ps_async_free(ps_async* a) {
    if (!a) return;
    free(a->data); free(a->len); free(a->t_first); free(a->t_end); free(a->cur);
    free(a);
}

/* Stop and join. Returns the reader's error code (0 if it stopped cleanly). */
static int ps_async_join(ps_async* a) {
    int exited, err;
    ps_lock(&a->lock);
    a->stop = 1;
    exited = a->exited;
    ps_unlock(&a->lock);
    /* A reader that already left on an error is not reading, and a wake sent
     * now would stay latched for the next 'read' on the MATLAB thread. */
    if (!exited) (void)yser_interrupt(a->port);
#if defined(_WIN32)
    WaitForSingleObject(a->thread, INFINITE);
    CloseHandle(a->thread);
#else
    pthread_join(a->thread, NULL);
#endif
    ps_lock(&a->lock);
    err = a->err;
    ps_unlock(&a->lock);
    ps_lock_free(&a->lock);
    return err;
}

/* ---- handle table: tracks every open port for cleanup ------------------ */
/* The handle is `id`, not the port address. malloc happily hands a new port
 * the address a closed one just released, so a pointer handle held in a stale
 * MATLAB variable would pass the lookup and drive someone else's port. Ids
 * are never reused, so a stale handle always raises ysp_serial:handle. */
typedef struct ps_node {
    uint64_t        id;
    yser_port*      port;
    ps_async*       async;   /* NULL unless 'async_start' is running */
    struct ps_node* next;
} ps_node;

static ps_node* g_ports = NULL;
static uint64_t g_next_id = 1;
static int      g_atexit_registered = 0;

static void ps_cleanup(void) {
    ps_node* n = g_ports;
    while (n) {
        ps_node* next = n->next;
        if (n->async) { (void)ps_async_join(n->async); ps_async_free(n->async); }
        if (n->port) { yser_close(n->port); free(n->port); }
        free(n);
        n = next;
    }
    g_ports = NULL;
}

/* Takes ownership of `port` on success. On allocation failure the port is
 * closed here: it would otherwise stay open with no handle to reach it. */
static uint64_t ps_register(yser_port* port) {
    ps_node* n = (ps_node*)malloc(sizeof(*n));
    if (!n) {
        yser_close(port);
        free(port);
        mexErrMsgIdAndTxt("ysp_serial:mem", "out of memory");
        return 0;
    }
    n->id = g_next_id++;
    n->port = port;
    n->async = NULL;
    n->next = g_ports;
    g_ports = n;
    if (!g_atexit_registered) { mexAtExit(ps_cleanup); g_atexit_registered = 1; }
    return n->id;
}

static void ps_unregister(yser_port* port) {
    ps_node** pp = &g_ports;
    while (*pp) {
        if ((*pp)->port == port) { ps_node* dead = *pp; *pp = dead->next; free(dead); return; }
        pp = &(*pp)->next;
    }
}

/* Resolve a handle arg to its registered node; error if unknown. */
static ps_node* ps_handle(const mxArray* a) {
    uint64_t id;
    if (mxIsUint64(a))
        id = *(const uint64_t*)mxGetData(a);
    else if (mxIsDouble(a) && mxGetNumberOfElements(a) == 1)
        id = (uint64_t)mxGetScalar(a);
    else {
        mexErrMsgIdAndTxt("ysp_serial:handle", "handle must be a scalar uint64");
        return NULL;
    }
    for (ps_node* n = g_ports; n; n = n->next)
        if (n->id == id) return n;
    mexErrMsgIdAndTxt("ysp_serial:handle", "invalid or closed port handle");
    return NULL;
}

/* ---- argument conversion ------------------------------------------------ */

static uint8_t ps_byte(const mxArray* a, const char* what) {
    if (!mxIsNumeric(a) || mxGetNumberOfElements(a) != 1)
        mexErrMsgIdAndTxt("ysp_serial:arg", "%s must be a numeric scalar", what);
    double d = mxGetScalar(a);
    /* Reject fractions explicitly; a silent (uint8_t) truncation of 42.7 would
     * send trigger 42 with no warning. */
    if (d < 0 || d > 255 || d != (double)(uint8_t)d)
        mexErrMsgIdAndTxt("ysp_serial:arg", "%s must be an integer in 0..255", what);
    return (uint8_t)d;
}

static uint32_t ps_u32(const mxArray* a, const char* what) {
    if (!mxIsNumeric(a) || mxGetNumberOfElements(a) != 1)
        mexErrMsgIdAndTxt("ysp_serial:arg", "%s must be a numeric scalar", what);
    double d = mxGetScalar(a);
    if (d < 0) mexErrMsgIdAndTxt("ysp_serial:arg", "%s must be >= 0", what);
    return (uint32_t)d;
}

/* mxGetScalar yields 0/1 for logical and numeric alike. */
static bool ps_bool(const mxArray* a, const char* what) {
    if ((!mxIsNumeric(a) && !mxIsLogical(a)) || mxGetNumberOfElements(a) != 1)
        mexErrMsgIdAndTxt("ysp_serial:arg", "%s must be a logical or numeric scalar", what);
    return mxGetScalar(a) != 0;
}

/* Map a field of `s` that holds one of `names` to its index, or leave *out
 * untouched when the field is absent. Strings beat enum numbers here: a
 * struct written by hand reads better as 'even' than as 2, and 'info' returns
 * the same spellings. */
static void ps_field_enum(const mxArray* s, const char* field,
                          const char* const* names, int count, int* out) {
    const mxArray* f = mxGetField(s, 0, field);
    if (!f) return;
    if (!mxIsChar(f))
        mexErrMsgIdAndTxt("ysp_serial:arg", "%s must be a string", field);
    char* v = mxArrayToString(f);
    for (int i = 0; i < count; i++) {
        if (strcmp(v, names[i]) == 0) { *out = i; mxFree(v); return; }
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "unknown %s '%s'", field, v);
    mxFree(v);
    mexErrMsgIdAndTxt("ysp_serial:arg", "%s", msg);
}

static const char* const ps_parity_names[] = { "none", "odd", "even", "mark", "space" };
static const char* const ps_flow_names[]   = { "none", "rtscts", "xonxoff" };
/* The policy the worker obtained, as a string, so a log is readable without a
 * lookup table. ysp/rt.h owns the names; it spells them in capitals, and this
 * binding has always published lower case, so fold the case here rather than
 * keep a second table that ysp/rt.h could outgrow. */
static mxArray* ps_policy_string(yrt_policy pol) {
    const char* name = yrt_policy_name(pol);
    char buf[32];
    size_t i = 0;
    for (; name[i] != 0 && i + 1 < sizeof(buf); i++)
        buf[i] = (name[i] >= 'A' && name[i] <= 'Z')
               ? (char)(name[i] - 'A' + 'a') : name[i];
    buf[i] = 0;
    return mxCreateString(buf);
}

/* Turn a negative YSER_ERR_* code into a MATLAB error. The identifier names
 * the code so a script can branch on it (a listener catching
 * ysp_serial:interrupted is the shutdown path, not a failure). */
static const char* ps_errid(int code) {
    switch (code) {
        case YSER_ERR_IO:           return "ysp_serial:io";
        case YSER_ERR_DISCONNECTED: return "ysp_serial:disconnected";
        case YSER_ERR_CLOSED:       return "ysp_serial:closed";
        case YSER_ERR_INTERRUPTED:  return "ysp_serial:interrupted";
        case YSER_ERR_ARG:          return "ysp_serial:arg";
        default:                    return "ysp_serial:error";
    }
}

static int ps_check(int rc) {
    if (rc < 0) mexErrMsgIdAndTxt(ps_errid(rc), "%s", yser_strerror(rc));
    return rc;
}

/* Counts (bytes written, bytes read) only land in ans when the caller asked
 * for them; a bare ysp_serial('write', ...) should print nothing. */
static void ps_return_count(int nlhs, mxArray* plhs[], int value) {
    if (nlhs >= 1) plhs[0] = mxCreateDoubleScalar((double)value);
}

/* ---- enumeration -------------------------------------------------------- */

static mxArray* ps_info_struct(const yser_port_info* v, int n) {
    const char* fields[] = { "name", "description", "serial_number",
                             "location", "driver", "vid", "pid" };
    mxArray* s = mxCreateStructMatrix(n ? 1 : 0, n ? (size_t)n : 0, 7, fields);
    for (int i = 0; i < n; i++) {
        mxSetField(s, i, "name",          mxCreateString(v[i].name));
        mxSetField(s, i, "description",   mxCreateString(v[i].description));
        mxSetField(s, i, "serial_number", mxCreateString(v[i].serial_number));
        mxSetField(s, i, "location",      mxCreateString(v[i].location));
        mxSetField(s, i, "driver",        mxCreateString(v[i].driver));
        mxSetField(s, i, "vid",           mxCreateDoubleScalar((double)v[i].vid));
        mxSetField(s, i, "pid",           mxCreateDoubleScalar((double)v[i].pid));
    }
    return s;
}

/* Count, allocate, enumerate. The count can shrink between the two calls when
 * a port vanishes, so the struct array is sized from the second one. */
static mxArray* ps_enumerate(const yser_port_filter* filter) {
    int n = filter ? yser_find_ports(filter, NULL, 0) : yser_list_ports(NULL, 0);
    if (n < 0) mexErrMsgIdAndTxt(ps_errid(n), "%s", yser_strerror(n));
    yser_port_info* v = NULL;
    if (n > 0) {
        v = (yser_port_info*)mxMalloc((size_t)n * sizeof(*v));
        int got = filter ? yser_find_ports(filter, v, n) : yser_list_ports(v, n);
        if (got < 0) { mxFree(v); mexErrMsgIdAndTxt(ps_errid(got), "%s", yser_strerror(got)); }
        if (got < n) n = got;
    }
    mxArray* s = ps_info_struct(v, n);
    if (v) mxFree(v);
    return s;
}

/* 'find': a struct of filter fields. The string fields are borrowed by the
 * filter, so their buffers must outlive the yser_find_ports calls; they are
 * freed by the caller. */
static void ps_read_filter(const mxArray* s, yser_port_filter* f, char** owned, int* nowned) {
    static const char* const str_fields[] = { "serial_number", "location",
                                              "description", "name" };
    const char** slots[4];
    slots[0] = &f->serial_number;
    slots[1] = &f->location;
    slots[2] = &f->description;
    slots[3] = &f->name;

    const mxArray* fv;
    if ((fv = mxGetField(s, 0, "vid"))) f->vid = (uint16_t)ps_u32(fv, "vid");
    if ((fv = mxGetField(s, 0, "pid"))) f->pid = (uint16_t)ps_u32(fv, "pid");
    for (int i = 0; i < 4; i++) {
        fv = mxGetField(s, 0, str_fields[i]);
        if (!fv) continue;
        if (!mxIsChar(fv))
            mexErrMsgIdAndTxt("ysp_serial:arg", "%s must be a string", str_fields[i]);
        char* v = mxArrayToString(fv);
        owned[(*nowned)++] = v;
        *slots[i] = v;
    }
}

/* ---- 'open' ------------------------------------------------------------- */

/* Seconds in, int64 ns out, rounded to the nearest ns. NaN and negative
 * values are refused; Inf is YSER_TIMEOUT_INFINITE where `inf_ok`, else
 * refused. A positive value too small to round to 1 ns becomes 1 ns, so it
 * cannot turn into 0, which means "poll" or "default" in the C API. */
static int64_t ps_s_to_ns(const mxArray* a, const char* what, int inf_ok) {
    if (!mxIsNumeric(a) || mxIsComplex(a) || mxGetNumberOfElements(a) != 1)
        mexErrMsgIdAndTxt("ysp_serial:arg", "%s must be a real numeric scalar (seconds)", what);
    double s = mxGetScalar(a);
    if (isnan(s)) mexErrMsgIdAndTxt("ysp_serial:arg", "%s is NaN", what);
    if (s < 0) mexErrMsgIdAndTxt("ysp_serial:arg", "%s must be >= 0 seconds%s", what, inf_ok ? " or Inf" : "");
    if (isinf(s)) {
        if (!inf_ok) mexErrMsgIdAndTxt("ysp_serial:arg", "%s must be finite", what);
        return YSER_TIMEOUT_INFINITE;
    }
    if (s > 9.0e9) /* 285 years: past it, s * 1e9 leaves int64 */
        mexErrMsgIdAndTxt("ysp_serial:arg", "%s is too large (use Inf for forever)", what);
    int64_t ns = (int64_t)llround(s * 1e9);
    return (ns == 0 && s > 0) ? 1 : ns;
}

/* int64 ns on the ysp/rt.h clock to double seconds. */
static double ps_ns_to_s(int64_t ns) { return (double)ns * 1e-9; }

/* 'open' refuses a field it does not know: a misspelled or v1 field
 * (write_timeout_ms, rt_runtime_ns) would otherwise be dropped silently and
 * the port opened with the default. */
static const char* const ps_desc_fields[] = {
    "baud", "data_bits", "write_timeout", "stop_bits", "parity", "flow",
    "exclusive", "low_latency", "dtr_low_on_open", "rts_low_on_open",
    "keep_dtr_on_close", "rt_runtime", "rt_deadline", "rt_period", "min_hold" };

static void ps_read_desc(const mxArray* s, yser_desc* d) {
    const mxArray* f;
    int nf = mxGetNumberOfFields(s);
    for (int i = 0; i < nf; i++) {
        const char* name = mxGetFieldNameByNumber(s, i);
        int known = 0;
        for (size_t k = 0; k < sizeof(ps_desc_fields) / sizeof(ps_desc_fields[0]); k++)
            if (strcmp(name, ps_desc_fields[k]) == 0) known = 1;
        if (!known)
            mexErrMsgIdAndTxt("ysp_serial:arg",
                "open: unknown setting '%s' (times are seconds since binding v2: "
                "write_timeout, rt_runtime, rt_deadline, rt_period, min_hold)", name);
    }
    if ((f = mxGetField(s, 0, "baud")))             d->baud = ps_u32(f, "baud");
    if ((f = mxGetField(s, 0, "data_bits")))        d->data_bits = (uint8_t)ps_u32(f, "data_bits");
    if ((f = mxGetField(s, 0, "write_timeout")))    d->write_timeout_ns = ps_s_to_ns(f, "write_timeout", 1);
    if ((f = mxGetField(s, 0, "stop_bits"))) {
        uint32_t sb = ps_u32(f, "stop_bits");
        if (sb != 1 && sb != 2) mexErrMsgIdAndTxt("ysp_serial:arg", "stop_bits must be 1 or 2");
        d->stop_bits = (sb == 2) ? YSER_STOP_BITS_2 : YSER_STOP_BITS_1;
    }
    int parity = (int)d->parity, flow = (int)d->flow;
    ps_field_enum(s, "parity", ps_parity_names, 5, &parity);
    ps_field_enum(s, "flow",   ps_flow_names,   3, &flow);
    d->parity = (yser_parity)parity;
    d->flow = (yser_flow)flow;

    if ((f = mxGetField(s, 0, "exclusive")))         d->exclusive = ps_bool(f, "exclusive");
    if ((f = mxGetField(s, 0, "low_latency")))       d->low_latency = ps_bool(f, "low_latency");
    if ((f = mxGetField(s, 0, "dtr_low_on_open")))   d->dtr_low_on_open = ps_bool(f, "dtr_low_on_open");
    if ((f = mxGetField(s, 0, "rts_low_on_open")))   d->rts_low_on_open = ps_bool(f, "rts_low_on_open");
    if ((f = mxGetField(s, 0, "keep_dtr_on_close"))) d->keep_dtr_on_close = ps_bool(f, "keep_dtr_on_close");
    /* Async-worker RT reservation (s); missing fields stay 0, which yser_open
     * resolves: a 0 period means "same as deadline". */
    if ((f = mxGetField(s, 0, "min_hold")))   d->min_hold_ns = ps_s_to_ns(f, "min_hold", 0);
    if ((f = mxGetField(s, 0, "rt_runtime")))  d->sched.runtime_ns  = (uint64_t)ps_s_to_ns(f, "rt_runtime", 0);
    if ((f = mxGetField(s, 0, "rt_deadline"))) d->sched.deadline_ns = (uint64_t)ps_s_to_ns(f, "rt_deadline", 0);
    if ((f = mxGetField(s, 0, "rt_period")))   d->sched.period_ns   = (uint64_t)ps_s_to_ns(f, "rt_period", 0);
}

static void cmd_open(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    yser_desc desc;
    memset(&desc, 0, sizeof(desc));

    if (nrhs < 2 || !mxIsChar(prhs[1]))
        mexErrMsgIdAndTxt("ysp_serial:usage",
            "open needs a device name, e.g. 'COM3' or '/dev/ttyUSB0'");
    if (nrhs >= 3) {
        if (!mxIsStruct(prhs[2]) || mxGetNumberOfElements(prhs[2]) != 1)
            mexErrMsgIdAndTxt("ysp_serial:arg", "open: settings must be a 1x1 struct");
        ps_read_desc(prhs[2], &desc);
    }
    char* devbuf = mxArrayToString(prhs[1]);
    desc.device = devbuf;

    yser_port* port = (yser_port*)calloc(1, sizeof(*port));
    if (!port) { mxFree(devbuf); mexErrMsgIdAndTxt("ysp_serial:mem", "out of memory"); }
    bool ok = yser_open(port, &desc);
    mxFree(devbuf);
    if (!ok) {
        /* Copy the message out before the handle is freed. */
        char msg[300];
        snprintf(msg, sizeof(msg), "%s", yser_error(port));
        free(port);
        mexErrMsgIdAndTxt("ysp_serial:open", "%s", msg);
    }
    uint64_t id = ps_register(port);

    mxArray* h = mxCreateNumericMatrix(1, 1, mxUINT64_CLASS, mxREAL);
    *(uint64_t*)mxGetData(h) = id;
    plhs[0] = h;
    (void)nlhs;
}

/* ---- 'write' ------------------------------------------------------------ */

/* Accepts a uint8 array as-is and any other numeric array element by element,
 * so both ysp_serial('write', h, uint8([1 2 3])) and ('write', h, [1 2 3])
 * work. mxMalloc'd scratch is released by MATLAB when the mex call returns,
 * including on the error paths below. */
static const uint8_t* ps_write_bytes(const mxArray* a, int* len_out) {
    if (!mxIsNumeric(a) || mxIsComplex(a))
        mexErrMsgIdAndTxt("ysp_serial:arg", "data must be a real numeric or uint8 array");
    size_t n = mxGetNumberOfElements(a);
    if (n > (size_t)0x7FFFFFFF)
        mexErrMsgIdAndTxt("ysp_serial:arg", "data is too long");
    *len_out = (int)n;
    if (n == 0) return NULL;
    if (mxIsUint8(a)) return (const uint8_t*)mxGetData(a);

    if (!mxIsDouble(a))
        mexErrMsgIdAndTxt("ysp_serial:arg", "data must be a uint8 or double array");
    const double* src = (const double*)mxGetData(a);
    uint8_t* buf = (uint8_t*)mxMalloc(n);
    for (size_t i = 0; i < n; i++) {
        double d = src[i];
        if (d < 0 || d > 255 || d != (double)(uint8_t)d)
            mexErrMsgIdAndTxt("ysp_serial:arg", "data elements must be integers in 0..255");
        buf[i] = (uint8_t)d;
    }
    return buf;
}

/* ---- 'read' ------------------------------------------------------------- */

static void cmd_read(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[],
                     yser_port* port) {
    if (nrhs < 4)
        mexErrMsgIdAndTxt("ysp_serial:usage", "read needs n, timeout (seconds)");
    uint32_t cap = ps_u32(prhs[2], "n");
    int64_t timeout_ns = ps_s_to_ns(prhs[3], "timeout", 1);
    if (cap > (uint32_t)0x7FFFFFFF)
        mexErrMsgIdAndTxt("ysp_serial:arg", "n is too large");
    int flags = YSER_READ_ANY;
    if (nrhs >= 5) {
        if (!mxIsChar(prhs[4]))
            mexErrMsgIdAndTxt("ysp_serial:arg", "read: the 4th argument must be 'all'");
        char* mode = mxArrayToString(prhs[4]);
        int all = (strcmp(mode, "all") == 0);
        mxFree(mode);
        if (!all) mexErrMsgIdAndTxt("ysp_serial:arg", "read: the 4th argument must be 'all'");
        flags = YSER_READ_ALL;
    }
    /* Read into scratch and size the result from the count: a short read must
     * not return trailing zeros that the device never sent. n == 0 answers
     * itself, and it reaches here from the natural
     * n = available(); read(n, 0) pattern. */
    uint8_t* buf = cap ? (uint8_t*)mxMalloc(cap) : NULL;
    int got = cap ? ps_check(yser_read(port, buf, (int)cap, timeout_ns, flags)) : 0;
    mxArray* out = mxCreateNumericMatrix(1, (size_t)got, mxUINT8_CLASS, mxREAL);
    if (got > 0) memcpy(mxGetData(out), buf, (size_t)got);
    if (buf) mxFree(buf);
    plhs[0] = out;
    (void)nlhs;
}

/* ---- 'info' ------------------------------------------------------------- */

static mxArray* ps_info(const yser_port* p) {
    const char* fields[] = { "device", "baud", "data_bits", "parity", "stop_bits",
                             "flow", "write_timeout", "min_hold", "low_latency",
                             "keep_dtr_on_close", "async_policy",
                             "rd_oserr", "wr_oserr", "misc_oserr",
                             "driver", "rts_via_dtr" };
    mxArray* s = mxCreateStructMatrix(1, 1, 16, fields);
    int parity = (int)p->parity, flow = (int)p->flow;
    if (parity < 0 || parity > 4) parity = 0;
    if (flow < 0 || flow > 2) flow = 0;
    mxSetField(s, 0, "device",            mxCreateString(p->device));
    mxSetField(s, 0, "baud",              mxCreateDoubleScalar((double)p->baud));
    mxSetField(s, 0, "data_bits",         mxCreateDoubleScalar((double)p->data_bits));
    mxSetField(s, 0, "parity",            mxCreateString(ps_parity_names[parity]));
    mxSetField(s, 0, "stop_bits",
               mxCreateDoubleScalar(p->stop_bits == YSER_STOP_BITS_2 ? 2.0 : 1.0));
    mxSetField(s, 0, "flow",              mxCreateString(ps_flow_names[flow]));
    mxSetField(s, 0, "write_timeout",
               mxCreateDoubleScalar(p->write_timeout_ns == YSER_TIMEOUT_INFINITE
                                        ? mxGetInf()
                                        : ps_ns_to_s(p->write_timeout_ns)));
    mxSetField(s, 0, "min_hold",          mxCreateDoubleScalar(ps_ns_to_s(p->min_hold_ns)));
    mxSetField(s, 0, "low_latency",       mxCreateLogicalScalar(p->low_latency));
    mxSetField(s, 0, "keep_dtr_on_close", mxCreateLogicalScalar(p->keep_dtr_on_close));
    mxSetField(s, 0, "async_policy",      ps_policy_string(p->async_policy));
    mxSetField(s, 0, "rd_oserr",          mxCreateDoubleScalar((double)p->rd_oserr));
    mxSetField(s, 0, "wr_oserr",          mxCreateDoubleScalar((double)p->wr_oserr));
    mxSetField(s, 0, "misc_oserr",        mxCreateDoubleScalar((double)p->misc_oserr));
    mxSetField(s, 0, "driver",            mxCreateString(p->driver));
    mxSetField(s, 0, "rts_via_dtr",       mxCreateLogicalScalar(p->rts_via_dtr));
    return s;
}

/* ---- 'async_start', 'async_read', 'async_stop' ------------------------- */

static void cmd_async_start(ps_node* node, int nrhs, const mxArray* prhs[]) {
    if (node->async)
        mexErrMsgIdAndTxt("ysp_serial:async", "async_start: the async reader is already running");
    uint32_t term = 10, nslots = 8192, maxlen = 256;
    if (nrhs >= 3) term = ps_byte(prhs[2], "terminator");
    if (nrhs >= 4) nslots = ps_u32(prhs[3], "lines");
    if (nrhs >= 5) maxlen = ps_u32(prhs[4], "max_len");
    if (nslots < 1 || nslots > (1u << 24))
        mexErrMsgIdAndTxt("ysp_serial:arg", "lines must be 1..16777216");
    if (maxlen < 1 || maxlen > 65535)
        mexErrMsgIdAndTxt("ysp_serial:arg", "max_len must be 1..65535");
    ps_async* a = (ps_async*)calloc(1, sizeof(*a));
    if (!a) mexErrMsgIdAndTxt("ysp_serial:mem", "out of memory");
    a->data = (uint8_t*)malloc((size_t)nslots * maxlen);
    a->len = (uint16_t*)calloc(nslots, sizeof(uint16_t));
    a->t_first = (int64_t*)calloc(nslots, sizeof(int64_t));
    a->t_end = (int64_t*)calloc(nslots, sizeof(int64_t));
    a->cur = (uint8_t*)malloc(maxlen);
    if (!a->data || !a->len || !a->t_first || !a->t_end || !a->cur) {
        ps_async_free(a);
        mexErrMsgIdAndTxt("ysp_serial:mem", "out of memory for %u lines of %u bytes",
                          (unsigned)nslots, (unsigned)maxlen);
    }
    a->port = node->port;
    a->nslots = nslots;
    a->maxlen = maxlen;
    a->term = (uint8_t)term;
    ps_lock_init(&a->lock);
#if defined(_WIN32)
    a->thread = CreateThread(NULL, 0, ps_async_main, a, 0, NULL);
    int ok = a->thread != NULL;
#else
    int ok = pthread_create(&a->thread, NULL, ps_async_main, a) == 0;
#endif
    if (!ok) {
        ps_lock_free(&a->lock);
        ps_async_free(a);
        mexErrMsgIdAndTxt("ysp_serial:async", "async_start: could not start the reader thread");
    }
    node->async = a;
}

static ps_async* ps_async_of(ps_node* node, const char* cmd) {
    if (!node->async)
        mexErrMsgIdAndTxt("ysp_serial:async", "%s: the async reader is not running (call async_start)", cmd);
    return node->async;
}

static void cmd_async_read(ps_node* node, int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    ps_async* a = ps_async_of(node, "async_read");
    uint32_t want = 0xFFFFFFFFu;
    if (nrhs >= 3) want = ps_u32(prhs[2], "max_lines");
    uint64_t tail, head;
    int err;
    ps_lock(&a->lock);
    tail = a->tail;
    head = a->head;
    err = a->err;
    ps_unlock(&a->lock);
    /* Slots in [tail, head) are complete and the reader does not touch them
     * until tail moves, so they are copied without the lock: the reader is
     * never held up while MATLAB builds its arrays. */
    uint64_t n = head - tail;
    if (n > want) n = want;
    mxArray* lines = mxCreateCellMatrix((size_t)n, 1);
    mxArray* tend = mxCreateDoubleMatrix((size_t)n, 1, mxREAL);
    mxArray* tfirst = mxCreateDoubleMatrix((size_t)n, 1, mxREAL);
    double* pe = mxGetPr(tend);
    double* pf = mxGetPr(tfirst);
    for (uint64_t i = 0; i < n; i++) {
        uint32_t s = (uint32_t)((tail + i) % a->nslots);
        mxArray* r = mxCreateNumericMatrix(1, a->len[s], mxUINT8_CLASS, mxREAL);
        if (a->len[s]) memcpy(mxGetData(r), a->data + (size_t)s * a->maxlen, a->len[s]);
        mxSetCell(lines, (size_t)i, r);
        pe[i] = ps_ns_to_s(a->t_end[s]);
        pf[i] = ps_ns_to_s(a->t_first[s]);
    }
    ps_lock(&a->lock);
    a->tail = tail + n;
    ps_unlock(&a->lock);
    /* A reader that died (unplug, I/O error) says so once its lines are out,
     * so nothing it collected is lost to the error. */
    if (n == 0 && err < 0)
        mexErrMsgIdAndTxt(ps_errid(err), "async reader stopped: %s", yser_strerror(err));
    plhs[0] = lines;
    if (nlhs >= 2) plhs[1] = tend; else mxDestroyArray(tend);
    if (nlhs >= 3) plhs[2] = tfirst; else mxDestroyArray(tfirst);
}

static mxArray* ps_async_stop(ps_node* node) {
    ps_async* a = node->async;
    int err = ps_async_join(a);
    const char* fields[] = { "pending", "dropped", "overlong", "bytes", "reads", "error" };
    mxArray* s = mxCreateStructMatrix(1, 1, 6, fields);
    mxSetField(s, 0, "pending",  mxCreateDoubleScalar((double)(a->head - a->tail)));
    mxSetField(s, 0, "dropped",  mxCreateDoubleScalar((double)a->dropped));
    mxSetField(s, 0, "overlong", mxCreateDoubleScalar((double)a->overlong));
    mxSetField(s, 0, "bytes",    mxCreateDoubleScalar((double)a->bytes));
    mxSetField(s, 0, "reads",    mxCreateDoubleScalar((double)a->reads));
    mxSetField(s, 0, "error",    mxCreateString(err < 0 ? yser_strerror(err) : ""));
    ps_async_free(a);
    node->async = NULL;
    return s;
}

/* A TX purge is writer role and stays allowed while the reader runs. */
static int ps_is_tx_purge(int nrhs, const mxArray* prhs[]) {
    if (nrhs < 3 || !mxIsChar(prhs[2])) return 0;
    char* w = mxArrayToString(prhs[2]);
    int tx = w && strcmp(w, "tx") == 0;
    mxFree(w);
    return tx;
}

/* ---- dispatch ----------------------------------------------------------- */

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    if (nrhs < 1 || !mxIsChar(prhs[0]))
        mexErrMsgIdAndTxt("ysp_serial:usage", "first argument must be a command string");

    char* cmd = mxArrayToString(prhs[0]);

    if (strcmp(cmd, "open") == 0) {
        cmd_open(nlhs, plhs, nrhs, prhs);
        mxFree(cmd);
        return;
    }

    if (strcmp(cmd, "list") == 0) {
        plhs[0] = ps_enumerate(NULL);
        mxFree(cmd);
        return;
    }

    if (strcmp(cmd, "now") == 0) {
        /* The clock the library times its own deadlines against, and the
         * base of the async stamps. Bracket a write with it so your
         * timestamps and the library's share one base (see TIMING in
         * ysp/serial.h). A double in seconds resolves 0.12 ns at 10^6 s
         * (11.6 days) of uptime and 3.7 ns at 2^24 s (194 days). */
        plhs[0] = mxCreateDoubleScalar(ps_ns_to_s(yrt_now_ns()));
        mxFree(cmd);
        return;
    }

    if (strcmp(cmd, "find") == 0) {
        yser_port_filter f;
        memset(&f, 0, sizeof(f));
        char* owned[4];
        int nowned = 0;
        if (nrhs >= 2) {
            if (!mxIsStruct(prhs[1]) || mxGetNumberOfElements(prhs[1]) != 1)
                mexErrMsgIdAndTxt("ysp_serial:arg", "find: the filter must be a 1x1 struct");
            ps_read_filter(prhs[1], &f, owned, &nowned);
        }
        mxArray* s = ps_enumerate(&f);
        for (int i = 0; i < nowned; i++) mxFree(owned[i]);
        plhs[0] = s;
        mxFree(cmd);
        return;
    }

    if (strcmp(cmd, "version") == 0) {
        plhs[0] = mxCreateString(PS_BINDING_VERSION);
        if (nlhs >= 2) plhs[1] = mxCreateString(YSER_VERSION_STRING);
        mxFree(cmd);
        return;
    }

    /* all other commands take a handle as the 2nd argument */
    if (nrhs < 2) mexErrMsgIdAndTxt("ysp_serial:usage", "'%s' needs a port handle", cmd);
    ps_node* node = ps_handle(prhs[1]);
    yser_port* port = node->port;

    /* The async reader owns the reader role; a second reader would race it
     * for bytes and for the interrupt latch. */
    if (node->async &&
        (strcmp(cmd, "read") == 0 || strcmp(cmd, "available") == 0 ||
         strcmp(cmd, "interrupt") == 0 ||
         (strcmp(cmd, "purge") == 0 && !ps_is_tx_purge(nrhs, prhs)))) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: the async reader owns the input; call async_stop first", cmd);
        mxFree(cmd);
        mexErrMsgIdAndTxt("ysp_serial:async", "%s", msg);
    }

    if (strcmp(cmd, "async_start") == 0) {
        cmd_async_start(node, nrhs, prhs);

    } else if (strcmp(cmd, "async_read") == 0) {
        cmd_async_read(node, nlhs, plhs, nrhs, prhs);

    } else if (strcmp(cmd, "async_stop") == 0) {
        (void)ps_async_of(node, "async_stop");
        mxArray* s = ps_async_stop(node);
        if (nlhs >= 1) plhs[0] = s; else mxDestroyArray(s);

    } else if (strcmp(cmd, "write") == 0) {
        if (nrhs < 3) mexErrMsgIdAndTxt("ysp_serial:usage", "write needs data");
        int len = 0;
        const uint8_t* data = ps_write_bytes(prhs[2], &len);
        /* An empty write is a no-op here; yser_write would see a NULL buffer
         * and call it a bad argument. */
        ps_return_count(nlhs, plhs, len ? ps_check(yser_write(port, data, len)) : 0);

    } else if (strcmp(cmd, "writebyte") == 0) {
        if (nrhs < 3) mexErrMsgIdAndTxt("ysp_serial:usage", "writebyte needs a value");
        ps_return_count(nlhs, plhs, ps_check(yser_write_byte(port, ps_byte(prhs[2], "value"))));

    } else if (strcmp(cmd, "read") == 0) {
        cmd_read(nlhs, plhs, nrhs, prhs, port);

    } else if (strcmp(cmd, "available") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)ps_check(yser_available(port)));

    } else if (strcmp(cmd, "drain") == 0) {
        ps_check(yser_drain(port));

    } else if (strcmp(cmd, "purge") == 0) {
        int which = YSER_PURGE_RX;
        if (nrhs >= 3) {
            if (!mxIsChar(prhs[2]))
                mexErrMsgIdAndTxt("ysp_serial:arg", "purge takes 'rx', 'tx' or 'both'");
            char* w = mxArrayToString(prhs[2]);
            if (strcmp(w, "rx") == 0)        which = YSER_PURGE_RX;
            else if (strcmp(w, "tx") == 0)   which = YSER_PURGE_TX;
            else if (strcmp(w, "both") == 0) which = YSER_PURGE_RX | YSER_PURGE_TX;
            else { mxFree(w); mexErrMsgIdAndTxt("ysp_serial:arg", "purge takes 'rx', 'tx' or 'both'"); }
            mxFree(w);
        }
        ps_check(yser_purge(port, which));

    } else if (strcmp(cmd, "interrupt") == 0) {
        ps_check(yser_interrupt(port));

    } else if (strcmp(cmd, "pulse") == 0) {
        if (nrhs < 5) mexErrMsgIdAndTxt("ysp_serial:usage", "pulse needs on, off, width (seconds)");
        ps_return_count(nlhs, plhs,
            ps_check(yser_pulse(port, ps_byte(prhs[2], "on"), ps_byte(prhs[3], "off"),
                                ps_s_to_ns(prhs[4], "width", 0))));

    } else if (strcmp(cmd, "pulseasync") == 0) {
        if (nrhs < 5) mexErrMsgIdAndTxt("ysp_serial:usage", "pulseasync needs on, off, width (seconds)");
        ps_return_count(nlhs, plhs,
            ps_check(yser_pulse_async(port, ps_byte(prhs[2], "on"), ps_byte(prhs[3], "off"),
                                      ps_s_to_ns(prhs[4], "width", 0))));

    } else if (strcmp(cmd, "setdtr") == 0) {
        if (nrhs < 3) mexErrMsgIdAndTxt("ysp_serial:usage", "setdtr needs on");
        ps_check(yser_set_dtr(port, ps_bool(prhs[2], "on")));

    } else if (strcmp(cmd, "setrts") == 0) {
        if (nrhs < 3) mexErrMsgIdAndTxt("ysp_serial:usage", "setrts needs on");
        ps_check(yser_set_rts(port, ps_bool(prhs[2], "on")));

    } else if (strcmp(cmd, "lines") == 0) {
        int mask = ps_check(yser_get_lines(port));
        const char* fields[] = { "mask", "cts", "dsr", "ri", "dcd" };
        mxArray* s = mxCreateStructMatrix(1, 1, 5, fields);
        mxSetField(s, 0, "mask", mxCreateDoubleScalar((double)mask));
        mxSetField(s, 0, "cts",  mxCreateLogicalScalar((mask & YSER_LINE_CTS) != 0));
        mxSetField(s, 0, "dsr",  mxCreateLogicalScalar((mask & YSER_LINE_DSR) != 0));
        mxSetField(s, 0, "ri",   mxCreateLogicalScalar((mask & YSER_LINE_RI) != 0));
        mxSetField(s, 0, "dcd",  mxCreateLogicalScalar((mask & YSER_LINE_DCD) != 0));
        plhs[0] = s;

    } else if (strcmp(cmd, "break") == 0) {
        if (nrhs < 3) mexErrMsgIdAndTxt("ysp_serial:usage", "break needs a duration (seconds)");
        ps_check(yser_send_break(port, ps_s_to_ns(prhs[2], "duration", 0)));

    } else if (strcmp(cmd, "info") == 0) {
        plhs[0] = ps_info(port);

    } else if (strcmp(cmd, "close") == 0) {
        if (node->async) mxDestroyArray(ps_async_stop(node));
        yser_close(port);
        ps_unregister(port);
        free(port);

    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "unknown command '%s'", cmd);
        mxFree(cmd);
        mexErrMsgIdAndTxt("ysp_serial:usage", "%s", msg);
    }
    mxFree(cmd);
}
