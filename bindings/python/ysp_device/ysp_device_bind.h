/* ysp_device_bind.h - binding-internal code that ysp.device (Python,
 * ysp_device_ext.c) and ysp_device (MEX, bindings/mex/ysp_device.c) share.
 * Not a library header: include it once, after ysp/device.h with its
 * implementation. Every function is static.
 *
 *   - ybd_mutex, ybd_cond: a lock and a condition variable (Win32, pthreads).
 *   - ybd_queue: the device's sink. The reader thread pushes each event into
 *     a bounded ring and wakes waiters; it never waits for the consumer and
 *     never touches an interpreter. A full ring drops the new event and
 *     counts it.
 *   - ybd_sim: a simulated board behind a ydev_transport, on the real clock,
 *     so the bindings' tests run without hardware. It is the board of
 *     tests/adapt/device_test.c (a ysp line board, a Cedrus XID box, a
 *     photodiode frame board, or an output-only box) with the virtual clock
 *     taken out: its own clock at a stated rate, syncs every 100 ms, answers
 *     to identity and timer queries, edges the caller schedules, a USB
 *     delivery delay, and a log of what the host wrote.
 */
#ifndef YSP_DEVICE_BIND_H
#define YSP_DEVICE_BIND_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <pthread.h>
    #include <time.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define YBD_FN static __attribute__((unused))
#else
#define YBD_FN static
#endif

/* ---- lock and condition variable ------------------------------------------ */

#if defined(_WIN32)
typedef CRITICAL_SECTION   ybd_mutex;
typedef CONDITION_VARIABLE ybd_cond;
YBD_FN void ybd_sync_init(ybd_mutex* m, ybd_cond* c) {
    InitializeCriticalSection(m);
    if (c) InitializeConditionVariable(c);
}
YBD_FN void ybd_sync_free(ybd_mutex* m, ybd_cond* c) { (void)c; DeleteCriticalSection(m); }
YBD_FN void ybd_lock(ybd_mutex* m)     { EnterCriticalSection(m); }
YBD_FN void ybd_unlock(ybd_mutex* m)   { LeaveCriticalSection(m); }
YBD_FN void ybd_broadcast(ybd_cond* c) { WakeAllConditionVariable(c); }
/* Whole ms, rounded up, so a short wait never spins; a broadcast ends it. */
YBD_FN void ybd_cond_wait(ybd_cond* c, ybd_mutex* m, int64_t ns) {
    (void)SleepConditionVariableCS(c, m, (DWORD)((ns + 999999) / 1000000));
}
#else
typedef pthread_mutex_t ybd_mutex;
typedef pthread_cond_t  ybd_cond;
YBD_FN void ybd_sync_init(ybd_mutex* m, ybd_cond* c) {
    pthread_mutex_init(m, NULL);
    if (c) {
        pthread_condattr_t at;
        pthread_condattr_init(&at);
#if !defined(__APPLE__)
        /* a wall-clock step must not stretch a wait */
        pthread_condattr_setclock(&at, CLOCK_MONOTONIC);
#endif
        pthread_cond_init(c, &at);
        pthread_condattr_destroy(&at);
    }
}
YBD_FN void ybd_sync_free(ybd_mutex* m, ybd_cond* c) {
    if (c) pthread_cond_destroy(c);
    pthread_mutex_destroy(m);
}
YBD_FN void ybd_lock(ybd_mutex* m)     { pthread_mutex_lock(m); }
YBD_FN void ybd_unlock(ybd_mutex* m)   { pthread_mutex_unlock(m); }
YBD_FN void ybd_broadcast(ybd_cond* c) { pthread_cond_broadcast(c); }
YBD_FN void ybd_cond_wait(ybd_cond* c, ybd_mutex* m, int64_t ns) {
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

/* ---- the event queue (the device's sink) ----------------------------------- */

typedef struct ybd_queue {
    ybd_mutex  lock;
    ybd_cond   cond;          /* an event arrived, or the device closed       */
    yin_event* ev;            /* cap slots, allocated at init                 */
    uint32_t   cap;
    /* head and tail only grow; head - tail is the fill. The reader writes
     * slot head % cap; a consumer copies [tail, head) without the lock,
     * because the reader does not touch those slots until tail moves. */
    uint64_t   head, tail;
    uint64_t   events;        /* pushed or dropped                            */
    uint64_t   dropped;       /* lost to a full ring                          */
    uint64_t   max_used;
    int        closed;        /* the device closed: waiters leave             */
    int        busy;          /* a consumer is copying                        */
} ybd_queue;

YBD_FN int ybd_queue_init(ybd_queue* q, uint32_t cap) {
    memset(q, 0, sizeof *q);
    q->ev = (yin_event*)calloc(cap, sizeof(yin_event));
    if (!q->ev) return -1;
    q->cap = cap;
    ybd_sync_init(&q->lock, &q->cond);
    return 0;
}

/* After the device closed and no consumer is inside. */
YBD_FN void ybd_queue_free(ybd_queue* q) {
    if (!q->ev) return;
    ybd_sync_free(&q->lock, &q->cond);
    free(q->ev);
    q->ev = NULL;
}

/* The sink: the reader thread, for each event. One lock, no allocation. */
YBD_FN void ybd_sink(void* ctx, const yin_event* e) {
    ybd_queue* q = (ybd_queue*)ctx;
    ybd_lock(&q->lock);
    q->events++;
    if (q->head - q->tail < q->cap) {
        q->ev[q->head % q->cap] = *e;
        q->head++;
        if (q->head - q->tail > q->max_used) q->max_used = q->head - q->tail;
    } else {
        q->dropped++;
    }
    ybd_unlock(&q->lock);
    ybd_broadcast(&q->cond);
}

YBD_FN void ybd_queue_close(ybd_queue* q) {
    if (!q->ev) return;
    ybd_lock(&q->lock);
    q->closed = 1;
    ybd_unlock(&q->lock);
    ybd_broadcast(&q->cond);
}

/* Wait at most ns (< 0: forever) for an event or the close. Returns the
 * fill; *closed whether the device closed. The caller slices a long wait to
 * look at its interpreter. */
YBD_FN uint64_t ybd_queue_wait(ybd_queue* q, int64_t ns, int* closed) {
    uint64_t n;
    int64_t end = ns < 0 ? INT64_MAX : yrt_now_ns() + ns;
    ybd_lock(&q->lock);
    for (;;) {
        int64_t now;
        n = q->head - q->tail;
        if (n || q->closed) break;
        now = yrt_now_ns();
        if (now >= end) break;
        ybd_cond_wait(&q->cond, &q->lock, end - now < YRT_MS(50) ? end - now : YRT_MS(50));
    }
    if (closed) *closed = q->closed;
    ybd_unlock(&q->lock);
    return n;
}

/* Claim [tail, tail + n) for copying: at most max (0: all). Returns -1
 * when another consumer is copying. */
YBD_FN int ybd_queue_claim(ybd_queue* q, uint64_t max, uint64_t* tail, uint64_t* n) {
    ybd_lock(&q->lock);
    if (q->busy) { ybd_unlock(&q->lock); return -1; }
    q->busy = 1;
    *tail = q->tail;
    *n = q->head - q->tail;
    if (max > 0 && *n > max) *n = max;
    ybd_unlock(&q->lock);
    return 0;
}

/* End a claim; took events leave the ring (0 keeps them for the next call). */
YBD_FN void ybd_queue_release(ybd_queue* q, uint64_t took) {
    ybd_lock(&q->lock);
    q->tail += took;
    q->busy = 0;
    ybd_unlock(&q->lock);
}

YBD_FN const yin_event* ybd_queue_at(const ybd_queue* q, uint64_t i) { return &q->ev[i % q->cap]; }

/* Drop every queued event; the count dropped. */
YBD_FN uint64_t ybd_queue_clear(ybd_queue* q) {
    uint64_t n = 0;
    ybd_lock(&q->lock);
    if (!q->busy) { n = q->head - q->tail; q->tail = q->head; }
    ybd_unlock(&q->lock);
    return n;
}

/* ---- the simulated board ----------------------------------------------------- */

#define YBD_SIM_Q      (1 << 16)
#define YBD_SIM_EDGES  4096
#define YBD_SIM_WRITES 4096
#define YBD_SIM_WBYTES 16

typedef struct ybd_sim_edge {
    int64_t t;
    uint8_t ch, level;
} ybd_sim_edge;

typedef struct ybd_sim_wrec {
    int64_t t;
    int     n;                    /* bytes written; 0 for a line change      */
    uint8_t b[YBD_SIM_WBYTES];    /* the first of them                       */
    uint32_t code, changed;       /* a line change (LINES)                   */
} ybd_sim_wrec;

typedef struct ybd_sim {
    ybd_mutex lock;
    int      family;
    char     key[96];             /* what the board's port lists as          */
    /* its clock: ticks = u0 + (host - t0) / tick */
    int64_t  t0;
    double   tick;
    uint64_t u0;
    /* the USB path: uniform over frame_ns, then an exponential tail */
    double   frame_ns, tail_ns;
    uint64_t rng;
    /* bytes to the host and when each becomes readable */
    uint8_t  q[YBD_SIM_Q];
    int64_t  qt[YBD_SIM_Q];
    uint32_t qh, qtail;
    int64_t  next_sync;
    /* the caller's edges, in time order */
    ybd_sim_edge edges[YBD_SIM_EDGES];
    int      n_edges, next_edge;
    /* the host's bytes */
    char     line[64];
    int      nline;
    ybd_sim_wrec writes[YBD_SIM_WRITES];
    int      n_writes;
    uint64_t write_calls;
    /* faults */
    int64_t  gone_from, gone_until;
    int      no_identity;
    int      open, opens, interrupted;
    uint64_t reads;
} ybd_sim;

YBD_FN double ybd_sim_u01(ybd_sim* s) {
    s->rng ^= s->rng << 13; s->rng ^= s->rng >> 7; s->rng ^= s->rng << 17;
    return ((double)(s->rng >> 11) + 1.0) / 9007199254740992.0;
}

YBD_FN uint64_t ybd_sim_ticks(const ybd_sim* s, int64_t host) {
    return s->u0 + (uint64_t)((double)(host - s->t0) / s->tick);
}

YBD_FN int ybd_sim_gone(const ybd_sim* s, int64_t t) { return t >= s->gone_from && t < s->gone_until; }

YBD_FN const char* ybd_sim_default_key(int family) {
    switch (family) {
    case YBOX_XID:   return "serial:0403:6001:SIMXID:1-1";
    case YBOX_LINE:  return "serial:16C0:0483:SIMLINE:1-2";
    case YBOX_PHOTO: return "serial:16C0:0483:SIMPHOTO:1-3";
    default:         return "serial:0403:6001:SIMOUT:1-4";
    }
}

/* ppm: the board's clock against the host's; frame_ns and tail_ns: the USB
 * delivery delay. Returns -1 for a family the board cannot be. */
YBD_FN int ybd_sim_init(ybd_sim* s, int family, double ppm, double frame_ns, double tail_ns, const char* key) {
    int64_t now = yrt_now_ns();
    if (family < YBOX_XID || family > YBOX_FAMILY_LAST || family == YBOX_PARALLEL) return -1;
    memset(s, 0, sizeof *s);
    s->family = family;
    snprintf(s->key, sizeof s->key, "%s", key && key[0] ? key : ybd_sim_default_key(family));
    s->tick = (family == YBOX_XID ? 1e6 : 1e3) * (1.0 + ppm * 1e-6);
    s->t0 = now - 123456789;
    /* a LINE counter that wraps 32 bits a few seconds in */
    s->u0 = family == YBOX_XID ? 77777 : 4294000000u;
    s->frame_ns = frame_ns;
    s->tail_ns = tail_ns;
    s->rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)now;
    s->gone_from = s->gone_until = INT64_MAX;
    ybd_sync_init(&s->lock, NULL);
    return 0;
}

YBD_FN void ybd_sim_free(ybd_sim* s) { ybd_sync_free(&s->lock, NULL); }

/* Under lock. Bytes made at host time t; they arrive in order after the
 * path's delay. */
YBD_FN void ybd_sim_send(ybd_sim* s, int64_t t, const void* bytes, int n) {
    int64_t at = t + (int64_t)(ybd_sim_u01(s) * s->frame_ns - s->tail_ns * log(ybd_sim_u01(s)));
    int i;
    if (s->qtail != s->qh && at < s->qt[(s->qtail - 1) % YBD_SIM_Q]) at = s->qt[(s->qtail - 1) % YBD_SIM_Q];
    for (i = 0; i < n && s->qtail - s->qh < YBD_SIM_Q; i++) {
        s->q[s->qtail % YBD_SIM_Q] = ((const uint8_t*)bytes)[i];
        s->qt[s->qtail % YBD_SIM_Q] = at;
        s->qtail++;
    }
}

YBD_FN void ybd_sim_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

YBD_FN void ybd_sim_edge_out(ybd_sim* s, const ybd_sim_edge* e) {
    uint64_t u = ybd_sim_ticks(s, e->t);
    if (s->family == YBOX_LINE) {
        char b[64];
        int n = snprintf(b, sizeof b, "E %lu %u %u\n", (unsigned long)(uint32_t)u, (unsigned)e->ch, (unsigned)e->level);
        ybd_sim_send(s, e->t, b, n);
    } else if (s->family == YBOX_XID) {
        /* key in bits 5..7 (0 is key 8), press in bit 4, port 0 */
        uint8_t f[6] = { 'k', 0, 0, 0, 0, 0 };
        f[1] = (uint8_t)(((e->ch & 7u) << 5) | (e->level ? 0x10u : 0u));
        ybd_sim_le32(f + 2, (uint32_t)u);
        ybd_sim_send(s, e->t, f, 6);
    } else if (s->family == YBOX_PHOTO) {
        uint8_t f[6] = { 0xA5, 0, 0, 0, 0, 0 };
        f[1] = (uint8_t)(e->level ? 1 : 0);
        ybd_sim_le32(f + 2, (uint32_t)u);
        ybd_sim_send(s, e->t, f, 6);
    }
}

YBD_FN void ybd_sim_sync(ybd_sim* s, int64_t t) {
    uint64_t u = ybd_sim_ticks(s, t);
    if (s->family == YBOX_LINE) {
        char b[40];
        int n = snprintf(b, sizeof b, "S %lu\n", (unsigned long)(uint32_t)u);
        ybd_sim_send(s, t, b, n);
    } else if (s->family == YBOX_PHOTO) {
        uint8_t f[6] = { 0x5A, 0, 0, 0, 0, 0 };
        ybd_sim_le32(f + 2, (uint32_t)u);
        ybd_sim_send(s, t, f, 6);
    }
}

/* Under lock: everything the board makes up to host time t. Edges of an
 * unplugged or closed board are lost, as a real board's are. */
YBD_FN void ybd_sim_run(ybd_sim* s, int64_t t) {
    int has_sync = s->family == YBOX_LINE || s->family == YBOX_PHOTO;
    for (;;) {
        int64_t te = s->next_edge < s->n_edges ? s->edges[s->next_edge].t : INT64_MAX;
        int64_t ts = has_sync ? s->next_sync : INT64_MAX;
        int64_t tn = te < ts ? te : ts;
        if (tn > t) break;
        if (tn == te) {
            if (!ybd_sim_gone(s, te) && s->open) ybd_sim_edge_out(s, &s->edges[s->next_edge]);
            s->next_edge++;
        } else {
            if (!ybd_sim_gone(s, ts) && s->open) ybd_sim_sync(s, ts);
            s->next_sync += 100000000;
        }
    }
}

/* Under lock: one byte from the host at host time t; the board reads it
 * 0.2 ms later. */
YBD_FN void ybd_sim_hear(ybd_sim* s, uint8_t c, int64_t t) {
    int64_t at = t + 200000;
    if (s->family == YBOX_LINE) {
        if (c != '\n') { if (s->nline < 63) s->line[s->nline++] = (char)c; return; }
        s->line[s->nline] = '\0';
        s->nline = 0;
        if (s->line[0] == 'q' && s->line[1] == ' ') {
            char b[64];
            int n = snprintf(b, sizeof b, "Q %.20s %lu\n", s->line + 2, (unsigned long)(uint32_t)ybd_sim_ticks(s, at));
            ybd_sim_send(s, at, b, n);
        } else if (s->line[0] == 'i' && !s->no_identity) {
            ybd_sim_send(s, at, "I ysp-line 1 sim binding\n", 25);
        } else if (s->line[0] == 'o' && s->line[1] == ' ') {
            char b[64];
            int n = snprintf(b, sizeof b, "O %lu %.20s\n", (unsigned long)(uint32_t)ybd_sim_ticks(s, at), s->line + 2);
            ybd_sim_send(s, at, b, n);
        } else if (s->line[0] == 'p' && s->line[1] == ' ') {
            /* "p <code> <us>": the code now, 0 when the board's own timer ends */
            unsigned long code = 0, us = 0;
            char b[64];
            int n;
            if (sscanf(s->line + 2, "%lu %lu", &code, &us) == 2) {
                n = snprintf(b, sizeof b, "O %lu %lu\n", (unsigned long)(uint32_t)ybd_sim_ticks(s, at), code);
                ybd_sim_send(s, at, b, n);
                n = snprintf(b, sizeof b, "O %lu 0\n", (unsigned long)(uint32_t)ybd_sim_ticks(s, at + (int64_t)us * 1000));
                ybd_sim_send(s, at + (int64_t)us * 1000, b, n);
            }
        }
        return;
    }
    if (s->family == YBOX_XID) {
        int need;
        if (s->nline < 63) s->line[s->nline++] = (char)c;
        /* "mh" lo hi and "mp" + 4 bytes are output commands; the rest have 3 bytes */
        need = s->nline >= 2 && s->line[0] == 'm' && s->line[1] == 'h' ? 4
             : s->nline >= 2 && s->line[0] == 'm' && s->line[1] == 'p' ? 6 : 3;
        if (s->nline < need) return;
        if (memcmp(s->line, "_e5", 3) == 0) {
            uint8_t f[7] = { '_', 'e', '5', 0, 0, 0, 0 };
            ybd_sim_le32(f + 3, (uint32_t)ybd_sim_ticks(s, at));
            ybd_sim_send(s, at, f, 7);
        } else if (memcmp(s->line, "_c1", 3) == 0 && !s->no_identity) {
            ybd_sim_send(s, at, "_xid0", 5);
        } else if (memcmp(s->line, "_d2", 3) == 0) {
            ybd_sim_send(s, at, "2", 1);
        } else if (memcmp(s->line, "_d3", 3) == 0) {
            ybd_sim_send(s, at, "1", 1);
        } else if (memcmp(s->line, "_d4", 3) == 0) {
            ybd_sim_send(s, at, "2", 1);
        }
        s->nline = 0;
    }
}

/* Under lock. */
YBD_FN void ybd_sim_log(ybd_sim* s, int64_t t, const uint8_t* b, int n, uint32_t code, uint32_t changed) {
    ybd_sim_wrec* w;
    s->write_calls++;
    if (s->n_writes >= YBD_SIM_WRITES) return;
    w = &s->writes[s->n_writes++];
    memset(w, 0, sizeof *w);
    w->t = t;
    w->n = n;
    if (b && n > 0) memcpy(w->b, b, (size_t)(n < YBD_SIM_WBYTES ? n : YBD_SIM_WBYTES));
    w->code = code;
    w->changed = changed;
}

/* --- the transport --- */

YBD_FN void* ybd_sim_open(void* user, const char* key, char* found, size_t cap) {
    ybd_sim* s = (ybd_sim*)user;
    int64_t t = yrt_now_ns();
    void* r = NULL;
    ybd_lock(&s->lock);
    if (!ybd_sim_gone(s, t) && ydev_key_match(key, s->key)) {
        s->open = 1;
        s->opens++;
        s->qh = s->qtail;              /* a fresh port: nothing waiting */
        s->nline = 0;
        s->next_sync = t + 50000000;
        while (s->next_edge < s->n_edges && s->edges[s->next_edge].t < t) s->next_edge++;
        snprintf(found, cap, "sim %s", ybox_family_name(s->family));
        r = s;
    }
    ybd_unlock(&s->lock);
    return r;
}

/* Returns at once with the bytes that have arrived, or sleeps until the
 * next one arrives or the timeout: a read is late by the sleep's own
 * error, not by a fixed poll period. */
YBD_FN int ybd_sim_read(void* conn, uint8_t* buf, int cap, int64_t timeout_ns) {
    ybd_sim* s = (ybd_sim*)conn;
    int64_t end = yrt_now_ns() + (timeout_ns > 0 ? timeout_ns : 0);
    for (;;) {
        int64_t t = yrt_now_ns(), next = end;
        int n = 0;
        ybd_lock(&s->lock);
        s->reads++;
        if (ybd_sim_gone(s, t)) { s->open = 0; ybd_unlock(&s->lock); return -1; }
        ybd_sim_run(s, t);
        while (n < cap && s->qh != s->qtail && s->qt[s->qh % YBD_SIM_Q] <= t) buf[n++] = s->q[s->qh++ % YBD_SIM_Q];
        if (n == 0) {
            if (s->qh != s->qtail && s->qt[s->qh % YBD_SIM_Q] < next) next = s->qt[s->qh % YBD_SIM_Q];
            if (s->next_edge < s->n_edges && s->edges[s->next_edge].t < next) next = s->edges[s->next_edge].t;
            if ((s->family == YBOX_LINE || s->family == YBOX_PHOTO) && s->next_sync < next) next = s->next_sync;
            if (s->gone_from > t && s->gone_from < next) next = s->gone_from;
        }
        if (s->interrupted) { s->interrupted = 0; ybd_unlock(&s->lock); return n; }
        ybd_unlock(&s->lock);
        if (n > 0 || t >= end) return n;
        /* at most 5 ms a sleep: an edge scheduled meanwhile is not slept past by more */
        if (next - t > YRT_MS(5)) next = t + YRT_MS(5);
        (void)yrt_sleep_ns(next - t > 0 ? next - t : 0);
    }
}

YBD_FN int ybd_sim_write(void* conn, const uint8_t* buf, int n) {
    ybd_sim* s = (ybd_sim*)conn;
    int64_t t = yrt_now_ns();
    int i;
    ybd_lock(&s->lock);
    if (ybd_sim_gone(s, t)) { ybd_unlock(&s->lock); return -1; }
    ybd_sim_run(s, t);
    ybd_sim_log(s, t, buf, n, 0, 0);
    for (i = 0; i < n; i++) ybd_sim_hear(s, buf[i], t);
    ybd_unlock(&s->lock);
    return n;
}

YBD_FN int ybd_sim_lines(void* conn, uint32_t code, uint32_t changed) {
    ybd_sim* s = (ybd_sim*)conn;
    int64_t t = yrt_now_ns();
    ybd_lock(&s->lock);
    if (ybd_sim_gone(s, t)) { ybd_unlock(&s->lock); return -1; }
    ybd_sim_log(s, t, NULL, 0, code, changed);
    ybd_unlock(&s->lock);
    return 0;
}

YBD_FN void ybd_sim_interrupt(void* conn) {
    ybd_sim* s = (ybd_sim*)conn;
    ybd_lock(&s->lock);
    s->interrupted = 1;
    ybd_unlock(&s->lock);
}

YBD_FN void ybd_sim_close(void* conn) {
    ybd_sim* s = (ybd_sim*)conn;
    ybd_lock(&s->lock);
    s->open = 0;
    ybd_unlock(&s->lock);
}

YBD_FN ydev_transport ybd_sim_transport(ybd_sim* s) {
    ydev_transport tr;
    memset(&tr, 0, sizeof tr);
    tr.open = ybd_sim_open;
    tr.read = ybd_sim_read;
    tr.write = ybd_sim_write;
    tr.interrupt = ybd_sim_interrupt;
    tr.close = ybd_sim_close;
    tr.lines = ybd_sim_lines;
    tr.user = s;
    return tr;
}

/* --- the caller's side (any thread) --- */

/* An edge at host time t (ns on the ysp_rt clock): level 1 on (pressed,
 * light), 0 off. Kept in time order. -1 when the list is full. */
YBD_FN int ybd_sim_add_edge(ybd_sim* s, int64_t t, int ch, int level) {
    int i;
    ybd_lock(&s->lock);
    if (s->n_edges >= YBD_SIM_EDGES) {
        /* drop the edges already made, then try again */
        if (s->next_edge > 0) {
            memmove(s->edges, s->edges + s->next_edge, (size_t)(s->n_edges - s->next_edge) * sizeof s->edges[0]);
            s->n_edges -= s->next_edge;
            s->next_edge = 0;
        }
        if (s->n_edges >= YBD_SIM_EDGES) { ybd_unlock(&s->lock); return -1; }
    }
    i = s->n_edges;
    while (i > s->next_edge && s->edges[i - 1].t > t) { s->edges[i] = s->edges[i - 1]; i--; }
    s->edges[i].t = t;
    s->edges[i].ch = (uint8_t)ch;
    s->edges[i].level = (uint8_t)(level ? 1 : 0);
    s->n_edges++;
    ybd_unlock(&s->lock);
    return 0;
}

YBD_FN void ybd_sim_unplug(ybd_sim* s, int64_t from, int64_t until) {
    ybd_lock(&s->lock);
    s->gone_from = from;
    s->gone_until = until;
    ybd_unlock(&s->lock);
}

/* The board's clock at host time t, unwrapped (the truth a test compares
 * the fit with). */
YBD_FN uint64_t ybd_sim_ticks_at(ybd_sim* s, int64_t t) {
    uint64_t u;
    ybd_lock(&s->lock);
    u = ybd_sim_ticks(s, t);
    ybd_unlock(&s->lock);
    return u;
}

#endif /* YSP_DEVICE_BIND_H */
