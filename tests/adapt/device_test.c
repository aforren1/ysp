/* device_test.c - self-checking test for ysp/device.h. No framework: it
 * returns 0 when every check passed and 1 after printing each failure.
 *
 * A simulated board (a ysp line protocol board, a Cedrus XID box, a
 * photodiode frame board) sits behind a fake transport. Its clock runs at a
 * stated rate from a stated origin; it sends syncs and the edges the test
 * schedules, answers timer and identity queries, and hands its bytes to the
 * host after a delay drawn from a model of the USB path (a native-USB
 * microframe and a tail, or an FTDI latency timer). Most of the test runs
 * in manual mode on a virtual clock: a read that finds nothing advances the
 * clock by its timeout, so a minute of device time takes milliseconds and
 * every run is the same. One part runs the reader thread on the real
 * clock. The reference for each event is the true host time of its edge.
 * The outputs run against a fake trigger box that keeps every write and
 * its time; their timing part runs on the real clock and the worker.
 * ysp/screen.h comes first (its declarations only, no SDL) so the trigger
 * channel glue is built and called with a fake flip.
 *
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude \
 *         -o device_test tests/adapt/device_test.c -lsetupapi   (Windows)
 */
#include "ysp/screen.h"
#define YSP_DEVICE_IMPLEMENTATION
#include "ysp/device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { \
    fprintf(stderr, "device_test: FAIL at line %d: %s\n", __LINE__, #cond); g_failures++; } } while (0)
#define CHECK_I(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); g_checks++; \
    if (g_ != w_) { fprintf(stderr, "device_test: FAIL at line %d: %s (got %lld, want %lld)\n", \
                            __LINE__, #got, g_, w_); g_failures++; } } while (0)

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static double u01(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return ((double)(g_rng >> 11) + 1.0) / 9007199254740992.0;
}

/* --- the virtual clock ------------------------------------------------------ */

static int64_t g_now = 1000000000000LL;    /* the host clock reads 1000 s */
static bool    g_real;                     /* the thread test: the real clock */
static int64_t now_fn(void* ctx) { (void)ctx; return g_real ? yrt_now_ns() : g_now; }

/* --- the simulated board ---------------------------------------------------- */

#define QBYTES (1 << 16)

typedef struct board {
    int      family;
    /* its clock: ticks = (host - t0) / tick, from tick u0 */
    int64_t  t0;
    double   tick;            /* true ns per tick                         */
    uint64_t u0;
    /* the path to the host */
    double   frame_ns;        /* phase of a frame or latency timer        */
    double   tail_ns;         /* exponential tail                         */
    /* bytes to the host: when each becomes readable */
    uint8_t  q[QBYTES];
    int64_t  qt[QBYTES];
    uint32_t qh, qtail;
    /* what it sends */
    int64_t  next_sync;       /* LINE and PHOTO: every 100 ms             */
    int64_t  edges[4096];     /* host times of edges the test scheduled  */
    int      n_edges, next_edge;
    int      level;
    /* host to board */
    char     line[64];
    int      nline;
    /* faults */
    int64_t  gone_from, gone_until;   /* unplugged: open and read fail     */
    int      future_edge;             /* this edge is stamped 60 ms ahead  */
    int64_t  mute_from, mute_until;   /* plugged, but it sends nothing     */
    bool     no_identity;             /* never answers "i" or "_c1"       */
    bool     reset_on_return;         /* its clock restarts after an unplug */
    bool     open;
    int      opens;
    int64_t  generated_until;
    /* LINE outputs: when the board set each code ("o") */
    int64_t  out_at[64];
    int      nout;
} board;

static board g_b;

static uint64_t b_ticks(const board* b, int64_t host) {
    return b->u0 + (uint64_t)((double)(host - b->t0) / b->tick);
}

static bool b_muted(const board* b, int64_t t) { return t >= b->mute_from && t < b->mute_until; }
static bool b_gone(const board* b, int64_t t) { return t >= b->gone_from && t < b->gone_until; }

/* Queue bytes made at host time t; they arrive after the path's delay, in
 * order (a later chunk never overtakes an earlier one). */
static void b_send(board* b, int64_t t, const void* bytes, int n) {
    int64_t at = t + (int64_t)(u01() * b->frame_ns + -b->tail_ns * log(u01()));
    int i;
    if (b->qtail != b->qh && at < b->qt[(b->qtail - 1) % QBYTES]) at = b->qt[(b->qtail - 1) % QBYTES];
    for (i = 0; i < n; i++) {
        b->q[b->qtail % QBYTES] = ((const uint8_t*)bytes)[i];
        b->qt[b->qtail % QBYTES] = at;
        b->qtail++;
    }
}

static void b_le32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void b_edge(board* b, int64_t t) {
    uint64_t u = b_ticks(b, t);
    if (b->future_edge && b->next_edge == b->future_edge) u += (uint64_t)(60e6 / b->tick);
    b->level ^= 1;
    if (b->family == YBOX_LINE) {
        char s[64];
        int n = snprintf(s, sizeof s, "E %lu 1 %d\n", (unsigned long)(uint32_t)u, b->level);
        b_send(b, t, s, n);
    } else if (b->family == YBOX_XID) {
        uint8_t f[6] = { 'k', 0, 0, 0, 0, 0 };
        f[1] = (uint8_t)((1u << 5) | (b->level ? 0x10u : 0u));    /* key 1, port 0 */
        b_le32(f + 2, (uint32_t)u);
        b_send(b, t, f, 6);
    } else {
        uint8_t f[6] = { 0xA5, 0, 0, 0, 0, 0 };
        f[1] = (uint8_t)b->level;
        b_le32(f + 2, (uint32_t)u);
        b_send(b, t, f, 6);
    }
}

static void b_sync(board* b, int64_t t) {
    uint64_t u = b_ticks(b, t);
    if (b->family == YBOX_LINE) {
        char s[40];
        int n = snprintf(s, sizeof s, "S %lu\n", (unsigned long)(uint32_t)u);
        b_send(b, t, s, n);
    } else if (b->family == YBOX_PHOTO) {
        uint8_t f[6] = { 0x5A, 0, 0, 0, 0, 0 };
        b_le32(f + 2, (uint32_t)u);
        b_send(b, t, f, 6);
    }
}

/* Everything the board makes up to host time t. */
static void b_run(board* b, int64_t t) {
    for (;;) {
        int64_t te = b->next_edge < b->n_edges ? b->edges[b->next_edge] : INT64_MAX;
        int64_t ts = b->family == YBOX_XID ? INT64_MAX : b->next_sync;
        int64_t tn = te < ts ? te : ts;
        if (tn > t) break;
        if (tn == te) {
            if (!b_muted(b, te) && !b_gone(b, te) && b->open) b_edge(b, te);
            b->next_edge++;
        } else {
            if (!b_muted(b, ts) && !b_gone(b, ts) && b->open) b_sync(b, ts);
            b->next_sync += 100000000;
        }
    }
    if (t > b->generated_until) b->generated_until = t;
}

/* A byte from the host, at host time t (it reaches the board 0.2 ms later). */
static void b_hear(board* b, uint8_t c, int64_t t) {
    int64_t at = t + 200000;
    if (b_muted(b, at)) return;
    if (b->family == YBOX_LINE) {
        if (c != '\n') { if (b->nline < 63) b->line[b->nline++] = (char)c; return; }
        b->line[b->nline] = '\0';
        b->nline = 0;
        if (b->line[0] == 'q') {
            char s[64];
            int n = snprintf(s, sizeof s, "Q %s %lu\n", b->line + 2, (unsigned long)(uint32_t)b_ticks(b, at));
            b_send(b, at, s, n);
        } else if (b->line[0] == 'i' && !b->no_identity) {
            b_send(b, at, "I ysp-line 1 sim test\n", 22);
        } else if (b->line[0] == 'o' && b->line[1] == ' ') {
            char s[64];
            int n = snprintf(s, sizeof s, "O %lu %s\n", (unsigned long)(uint32_t)b_ticks(b, at), b->line + 2);
            if (b->nout < 64) b->out_at[b->nout++] = at;
            b_send(b, at, s, n);
        }
        return;
    }
    if (b->family == YBOX_XID) {
        if (b->nline < 63) b->line[b->nline++] = (char)c;
        b->line[b->nline] = '\0';
        if (b->nline < 3) return;
        if (memcmp(b->line, "_e5", 3) == 0) {
            uint8_t f[7] = { '_', 'e', '5', 0, 0, 0, 0 };
            b_le32(f + 3, (uint32_t)b_ticks(b, at));
            b_send(b, at, f, 7);
        } else if (memcmp(b->line, "_c1", 3) == 0 && !b->no_identity) {
            b_send(b, at, "_xid0", 5);
        } else if (memcmp(b->line, "_d2", 3) == 0) {
            b_send(b, at, "2", 1);
        } else if (memcmp(b->line, "_d3", 3) == 0) {
            b_send(b, at, "1", 1);
        } else if (memcmp(b->line, "_d4", 3) == 0) {
            b_send(b, at, "2", 1);
        }
        b->nline = 0;
    }
}

/* --- the fake transport ----------------------------------------------------- */

static void* t_open(void* user, const char* key, char* found, size_t cap) {
    board* b = (board*)user;
    int64_t t = now_fn(NULL);
    if (b_gone(b, t) || !ydev_key_match(key, "serial:16C0:0483:SIM1:1-1")) return NULL;
    if (b->opens > 0 && b->reset_on_return) { b->t0 = t; b->u0 = 0; }
    b->open = true;
    b->opens++;
    b->qh = b->qtail;                 /* a fresh port: nothing waiting */
    b->next_sync = t + 50000000;
    while (b->next_edge < b->n_edges && b->edges[b->next_edge] < t) b->next_edge++;
    snprintf(found, cap, "SIM1");
    return b;
}

static int t_read(void* conn, uint8_t* buf, int cap, int64_t timeout_ns) {
    board* b = (board*)conn;
    int64_t t = now_fn(NULL), end = t + timeout_ns;
    int n = 0;
    if (b_gone(b, t)) { b->open = false; return -1; }
    b_run(b, g_real ? t : end);
    /* the first chunk readable by end: the clock jumps to it */
    if (b->qh != b->qtail && b->qt[b->qh % QBYTES] <= end) {
        int64_t at = b->qt[b->qh % QBYTES];
        if (b_gone(b, at)) { if (!g_real) g_now = b->gone_from; b->open = false; return -1; }
        if (!g_real && at > g_now) g_now = at;
        while (n < cap && b->qh != b->qtail && b->qt[b->qh % QBYTES] <= (g_real ? t : g_now))
            buf[n++] = b->q[b->qh++ % QBYTES];
        if (n > 0) return n;
    }
    if (!g_real) {
        if (b_gone(b, end)) { g_now = b->gone_from > g_now ? b->gone_from : g_now; b->open = false; return -1; }
        g_now = end;
    } else {
        (void)yrt_sleep_ns(1000000);
    }
    return 0;
}

static int t_write(void* conn, const uint8_t* buf, int n) {
    board* b = (board*)conn;
    int i;
    int64_t t = now_fn(NULL);
    if (b_gone(b, t)) return -1;
    b_run(b, t);
    for (i = 0; i < n; i++) b_hear(b, buf[i], t);
    return n;
}

static void t_close(void* conn) { ((board*)conn)->open = false; }

static ydev_transport fake(void) {
    ydev_transport tr;
    memset(&tr, 0, sizeof tr);
    tr.open = t_open;
    tr.read = t_read;
    tr.write = t_write;
    tr.close = t_close;
    tr.user = &g_b;
    return tr;
}

/* --- the sink and the log ------------------------------------------------------ */

static yin_event g_ev[8192];
static int       g_nev;
static void sink(void* ctx, const yin_event* e) { (void)ctx; if (g_nev < 8192) g_ev[g_nev++] = *e; }

static unsigned char g_ring_mem[YRT_RING_BYTES(16384)];
static yrt_ring      g_ring;
static yrt_event     g_rec[16384];

static void ring_reset(void) {
    yrt_ring_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.memory = g_ring_mem;
    rd.bytes = sizeof g_ring_mem;
    memset(&g_ring, 0, sizeof g_ring);
    CHECK(yrt_ring_open(&g_ring, &rd));
}

static ydev_device g_dev;

static void board_init(int family, double ppm, double frame_ns, double tail_ns) {
    memset(&g_b, 0, sizeof g_b);
    g_b.family = family;
    g_b.tick = (family == YBOX_XID ? 1e6 : 1e3) * (1.0 + ppm * 1e-6);
    g_b.t0 = g_now - 123456789;
    g_b.u0 = family == YBOX_XID ? 77777 : 4294000000u;   /* LINE wraps soon */
    g_b.frame_ns = frame_ns;
    g_b.tail_ns = tail_ns;
    g_b.gone_from = g_b.gone_until = INT64_MAX;
    g_b.mute_from = g_b.mute_until = INT64_MAX;
}

static void schedule_edges(int64_t from, int64_t to, int64_t period, int64_t jitter) {
    int64_t t = from;
    while (t < to && g_b.n_edges < 4096) {
        g_b.edges[g_b.n_edges++] = t;
        t += period + (int64_t)(u01() * (double)jitter);
    }
}

static ydev_desc desc_for(int family) {
    ydev_desc d;
    memset(&d, 0, sizeof d);
    d.role = family == YBOX_XID ? "resp" : "photo";
    d.family = family;
    d.key = "serial:16C0:0483::";
    d.device = 3;
    d.kind = family == YBOX_XID ? 0 : YIN_KIND_SYNC;
    d.sink = sink;
    d.transport = fake();
    d.ring = &g_ring;
    d.manual = true;
    d.now = now_fn;
    return d;
}

/* The caller's loop: a poll every 20 ms. A poll that reads moves the
 * virtual clock itself; one that does not (no device) is the caller's
 * frame passing. */
static void run_until(int64_t t) {
    while (g_now < t) {
        int64_t before = g_now;
        (void)ydev_poll(&g_dev, YRT_MS(20));
        if (g_now == before) g_now += 20000000;
    }
}

/* Map each delivered event back to its edge: events come in edge order, so
 * the k-th event after the board opened is the k-th edge sent. Returns the
 * extremes of t minus truth for events after `from`. */
static void event_errors(int first_ev, int first_edge, int64_t from, double* lo, double* hi, int* n) {
    int i, k = first_edge;
    *lo = 1e300; *hi = -1e300; *n = 0;
    for (i = first_ev; i < g_nev && k < g_b.n_edges; i++, k++) {
        double e = (double)g_ev[i].t - (double)g_b.edges[k];
        if (g_b.edges[k] < from) continue;
        if (e < *lo) *lo = e;
        if (e > *hi) *hi = e;
        (*n)++;
    }
}

/* --- the cases ---------------------------------------------------------------------- */

/* A line-protocol photodiode: edges every 250 ms for 60 s, native USB (a
 * 125 us microframe and a 50 us tail), 30 ppm fast, a counter that wraps;
 * unplugged at 30 s for 3 s, its clock restarted when it comes back. */
static void test_line_unplug(void) {
    ydev_stats st;
    int64_t t_start = g_now, t_unplug, i, n_ev_before;
    double lo, hi;
    int n, k, recs, states[16], ns = 0, gaps = 0, texts = 0, fits = 0;
    int64_t gap_first = 0, gap_last = 0;
    ring_reset();
    g_nev = 0;
    board_init(YBOX_LINE, 30.0, 125000.0, 50000.0);
    schedule_edges(t_start + 100000000, t_start + 60000000000LL, 250000000, 20000000);
    t_unplug = t_start + 30000000000LL;
    g_b.gone_from = t_unplug;
    g_b.gone_until = t_unplug + 3000000000LL;
    g_b.reset_on_return = true;
    {
        ydev_desc d = desc_for(YBOX_LINE);
        CHECK(ydev_open(&g_dev, &d));
    }
    CHECK_I(ydev_poll(&g_dev, YRT_MS(20)), YDEV_RUNNING);
    ydev_get_stats(&g_dev, &st);
    CHECK(strcmp(st.ident, "ysp-line 1 sim test") == 0);
    CHECK(strcmp(st.found, "SIM1") == 0);
    run_until(t_unplug - 1);
    n_ev_before = g_nev;
    event_errors(0, 0, t_start + 10000000000LL, &lo, &hi, &n);
    printf("  line, 10 to 30 s: %d edges, event t minus edge %+.1f .. %+.1f us\n", n, lo / 1e3, hi / 1e3);
    CHECK(n > 70 && lo > -100e3 && hi < 300e3);
    run_until(t_unplug + 100000000);
    CHECK_I(ydev_state(&g_dev), YDEV_LOST);
    run_until(t_unplug + 5000000000LL);
    CHECK_I(ydev_state(&g_dev), YDEV_RUNNING);
    run_until(t_start + 60000000000LL);
    ydev_get_stats(&g_dev, &st);
    /* the events after the return map through the new fit */
    for (k = 0; k < g_b.n_edges && g_b.edges[k] < t_unplug + 3000000000LL; k++) { }
    {
        /* edges sent after reopening: the first edge at or after the reopen */
        int first_edge = 0;
        for (i = 0; i < g_b.n_edges; i++) if (g_b.edges[i] >= t_unplug + 3000000000LL) { first_edge = (int)i; break; }
        /* the reopen happened at the first retry after the board returned */
        while (first_edge < g_b.n_edges && g_ev[n_ev_before].ticks != 0 &&
               fabs((double)g_ev[n_ev_before].t - (double)g_b.edges[first_edge]) > 50e6) first_edge++;
        event_errors((int)n_ev_before, first_edge, t_unplug + 13000000000LL, &lo, &hi, &n);
        printf("  line after the unplug and a clock reset, 43 to 60 s: %d edges, %+.1f .. %+.1f us\n", n, lo / 1e3,
               hi / 1e3);
        CHECK(n > 50 && lo > -100e3 && hi < 300e3);
    }
    CHECK_I(st.opens, 2);
    CHECK(st.probes > 500 && st.answers > 0.95 * (double)st.probes);
    CHECK_I(st.clamped, 0);
    CHECK(st.fit.ready && st.fit.slope && fabs(st.fit.ppm - 30.0) < 3.0);
    CHECK(st.fit.width_ns > 0 && st.fit.width_ns <= 1300000);
    printf("  line: %llu probes, %llu answers, %llu pairs, refused %llu, fit %+.2f ppm, spread %.1f us, width %.1f us\n",
           (unsigned long long)st.probes, (unsigned long long)st.answers, (unsigned long long)st.pairs,
           (unsigned long long)st.refused, st.fit.ppm, (double)st.fit.spread_ns / 1e3, (double)st.fit.width_ns / 1e3);
    ydev_close(&g_dev);
    CHECK_I(ydev_state(&g_dev), YDEV_CLOSED);
    /* the log */
    recs = yrt_ring_drain(&g_ring, g_rec, 16384);
    for (i = 0; i < recs; i++) {
        const yrt_event* r = &g_rec[i];
        if (r->source == YRT_SRC_DEVICE) {
            CHECK_I(r->aux, 3);
            if (r->kind == YDEV_REC_STATE && ns < 16) states[ns++] = (int)r->u.u32[1];
            if (r->kind == YDEV_REC_GAP) { gaps++; gap_first = r->u.i64[0]; gap_last = r->u.i64[1]; }
            if (r->kind == YDEV_REC_TEXT) texts++;
        } else if (r->source == YRT_SRC_RT && r->kind == YRT_KIND_FIT) {
            CHECK_I(r->aux, 3);
            fits++;
        }
    }
    CHECK(ns == 5 && states[0] == YDEV_OPENING && states[1] == YDEV_RUNNING && states[2] == YDEV_LOST &&
          states[3] == YDEV_RUNNING && states[4] == YDEV_CLOSED);
    CHECK_I(gaps, 1);
    CHECK(gap_first <= t_unplug && gap_first > t_unplug - 200000000);
    CHECK(gap_last >= t_unplug + 3000000000LL && gap_last < t_unplug + 4100000000LL);
    CHECK(texts >= 5);    /* role, port and ident twice */
    CHECK(fits > 100);
    printf("  line: states opening, running, lost, running, closed; gap %.3f s; %d texts, %d fit records\n",
           (double)(gap_last - gap_first) / 1e9, texts, fits);
}

/* A Cedrus XID box: presses every 2 to 4 s behind a 16 ms FTDI latency
 * timer, 50 ppm slow, timer queries every 100 ms. */
static void test_xid(void) {
    ydev_stats st;
    int64_t t_start = g_now;
    double lo, hi, lo_early, hi_early;
    int n, n_early;
    ring_reset();
    g_nev = 0;
    board_init(YBOX_XID, -50.0, 16000000.0, 100000.0);
    schedule_edges(t_start + 500000000, t_start + 300000000000LL, 2000000000, 2000000000);
    {
        ydev_desc d = desc_for(YBOX_XID);
        CHECK(ydev_open(&g_dev, &d));
    }
    CHECK_I(ydev_poll(&g_dev, YRT_MS(20)), YDEV_RUNNING);
    run_until(t_start + 300000000000LL);
    ydev_get_stats(&g_dev, &st);
    CHECK(strcmp(st.ident, "xid product 2 model 1 firmware 2") == 0);
    event_errors(0, 0, t_start + 10000000000LL, &lo_early, &hi_early, &n_early);
    event_errors(0, 0, t_start + 120000000000LL, &lo, &hi, &n);
    printf("  xid, presses every 2 to 4 s, 16 ms latency timer: from 10 s %+.1f .. %+.1f us (%d), from 120 s "
           "%+.1f .. %+.1f us (%d)\n", lo_early / 1e3, hi_early / 1e3, n_early, lo / 1e3, hi / 1e3, n);
    printf("  xid: %llu probes, %llu answers, refused %llu, fit %+.2f ppm, spread %.1f us, width %.1f us\n",
           (unsigned long long)st.probes, (unsigned long long)st.answers, (unsigned long long)st.refused,
           st.fit.ppm, (double)st.fit.spread_ns / 1e3, (double)st.fit.width_ns / 1e3);
    /* the box's ms timer: an event is in [tick, tick + 1 ms) */
    CHECK(n > 30 && lo > -1500e3 && hi < 1500e3);
    /* unc_us: the 1 ms tick plus the bracket the map rests on, near the
     * measured error and far from the 15 ms spread of the answers */
    CHECK(g_nev > 0 && g_ev[g_nev - 1].unc_us >= 1000 && g_ev[g_nev - 1].unc_us <= 5000);   /* 1428 measured */
    printf("  xid: the last event's unc_us %u\n", (unsigned)g_ev[g_nev - 1].unc_us);
    CHECK(g_nev > 0 && g_ev[0].kind == YIN_KIND_BOX && g_ev[0].control == 1 && g_ev[0].device == 3);
    CHECK(st.answers > 2000);
    ydev_close(&g_dev);
}

/* The photodiode frame: no queries, syncs every 100 ms. */
static void test_photo(void) {
    ydev_stats st;
    int64_t t_start = g_now;
    double lo, hi;
    int n;
    ring_reset();
    g_nev = 0;
    board_init(YBOX_PHOTO, 10.0, 1000000.0, 100000.0);
    schedule_edges(t_start + 100000000, t_start + 60000000000LL, 100000000, 30000000);
    {
        ydev_desc d = desc_for(YBOX_PHOTO);
        d.kind = 0;   /* the family's: SYNC */
        CHECK(ydev_open(&g_dev, &d));
    }
    run_until(t_start + 60000000000LL);
    ydev_get_stats(&g_dev, &st);
    event_errors(0, 0, t_start + 10000000000LL, &lo, &hi, &n);
    printf("  photo frame, 1 ms USB frame: %d edges, %+.1f .. %+.1f us; %llu probes\n", n, lo / 1e3, hi / 1e3,
           (unsigned long long)st.probes);
    CHECK(n > 300 && lo > -150e3 && hi < 400e3);
    CHECK_I(st.probes, 0);
    CHECK(g_nev > 0 && g_ev[0].kind == YIN_KIND_SYNC && g_ev[0].unc_us > 0 && (g_ev[0].flags & YIN_DEVTICKS));
    ydev_close(&g_dev);
}

/* An edge stamped 60 ms past its arrival (a board bug): its pair is held
 * as an outlier and its map would pass the read time, so it is clamped to
 * the read time and counted. */
static void test_clamp(void) {
    int64_t t_start = g_now;
    ydev_stats st;
    int i, found = 0;
    ring_reset();
    g_nev = 0;
    board_init(YBOX_LINE, 0.0, 125000.0, 50000.0);
    schedule_edges(t_start + 100000000, t_start + 20000000000LL, 250000000, 0);
    g_b.future_edge = 60;
    {
        ydev_desc d = desc_for(YBOX_LINE);
        CHECK(ydev_open(&g_dev, &d));
    }
    run_until(t_start + 20000000000LL);
    ydev_get_stats(&g_dev, &st);
    CHECK_I(st.clamped, 1);
    for (i = 0; i < g_nev; i++)
        if (g_ev[i].t > g_b.edges[60] + 30000000 && g_ev[i].t < g_b.edges[60] + 100000000) found++;
    CHECK_I(found, 0);   /* no event mapped into the future */
    ydev_close(&g_dev);
    printf("  an edge stamped 60 ms ahead: clamped to its read time, counted: ok\n");
}

/* Plugged in but silent for 3 s: LOST after 2 s, then found again. */
static void test_silence(void) {
    int64_t t_start = g_now;
    ydev_stats st;
    ring_reset();
    g_nev = 0;
    board_init(YBOX_LINE, 0.0, 125000.0, 50000.0);
    g_b.mute_from = t_start + 10000000000LL;
    g_b.mute_until = t_start + 13000000000LL;
    {
        ydev_desc d = desc_for(YBOX_LINE);
        CHECK(ydev_open(&g_dev, &d));
    }
    run_until(t_start + 12500000000LL);
    CHECK_I(ydev_state(&g_dev), YDEV_LOST);
    run_until(t_start + 20000000000LL);
    CHECK_I(ydev_state(&g_dev), YDEV_RUNNING);
    ydev_get_stats(&g_dev, &st);
    CHECK_I(st.opens, 2);
    ydev_close(&g_dev);
    printf("  silence: lost after 2 s, running again: ok\n");
}

/* The port at the key answers nothing: FAILED, no retry. */
static void test_wrong_device(void) {
    int64_t t_start = g_now;
    ydev_stats st;
    ring_reset();
    board_init(YBOX_LINE, 0.0, 125000.0, 50000.0);
    g_b.no_identity = true;
    {
        ydev_desc d = desc_for(YBOX_LINE);
        CHECK(ydev_open(&g_dev, &d));
    }
    CHECK_I(ydev_poll(&g_dev, YRT_MS(20)), YDEV_FAILED);
    CHECK(g_now - t_start >= 1500000000LL && g_now - t_start < 2000000000LL);
    run_until(g_now + 5000000000LL);
    ydev_get_stats(&g_dev, &st);
    CHECK_I(st.state, YDEV_FAILED);
    CHECK_I(g_b.opens, 1);
    CHECK(strstr(ydev_error(&g_dev), "did not answer") != NULL);
    /* a wait on a FAILED instance ends at once and keeps the reason */
    CHECK_I(ydev_wait_running(&g_dev, YRT_S(1)), YDEV_ERR_STATE);
    CHECK(strstr(ydev_error(&g_dev), "did not answer") != NULL);
    ydev_close(&g_dev);
    printf("  wrong device: failed after 1.5 s, not retried: ok\n");
}

/* The reader thread on the real clock for 1 s. */
static void test_thread(void) {
    ydev_stats st;
    int64_t t0;
    g_real = true;
    t0 = yrt_now_ns();
    ring_reset();
    g_nev = 0;
    board_init(YBOX_LINE, 20.0, 125000.0, 50000.0);
    g_b.t0 = t0 - 1000000000;
    schedule_edges(t0 + 100000000, t0 + 1100000000, 50000000, 0);
    {
        ydev_desc d = desc_for(YBOX_LINE);
        d.manual = false;
        d.now = NULL;          /* the reader thread's own clock */
        d.no_elevate = true;   /* a test thread need not be elevated */
        CHECK(ydev_open(&g_dev, &d));
    }
    /* the reader thread opens and identifies; the wait polls ydev_state() */
    CHECK_I(ydev_wait_running(&g_dev, YRT_S(3)), YDEV_RUNNING);
    (void)yrt_sleep_ns(1200000000);
    ydev_close(&g_dev);         /* joins: g_ev is ours again */
    ydev_get_stats(&g_dev, &st);
    printf("  thread: %d events in 1.2 s, %llu probes, state %s after stop\n", g_nev,
           (unsigned long long)st.probes, ydev_state_name(st.state));
    CHECK(g_nev >= 15);
    CHECK(st.probes >= 8);
    CHECK_I(st.state, YDEV_CLOSED);
    g_real = false;
}

static void test_keys(void) {
    yser_port_info p;
    char k[160];
    memset(&p, 0, sizeof p);
    p.vid = 0x16C0; p.pid = 0x0483;
    snprintf(p.serial_number, sizeof p.serial_number, "1234560");
    snprintf(p.location, sizeof p.location, "Port_#0003.Hub_#0001");
    CHECK(ydev_port_key(&p, k, sizeof k) > 0 && strcmp(k, "serial:16C0:0483:1234560:Port_#0003.Hub_#0001") == 0);
    CHECK(ydev_port_key(&p, k, 10) < 0 && k[0] == '\0');
    CHECK(ydev_key_match("serial:16C0:0483::", "serial:16C0:0483:1234560:Port_#0003.Hub_#0001"));
    CHECK(ydev_key_match("serial:16c0", "serial:16C0:0483:1234560:x"));
    CHECK(ydev_key_match("serial:::1234560", "serial:16C0:0483:1234560:x"));
    CHECK(!ydev_key_match("serial:::1234561", "serial:16C0:0483:1234560:x"));
    CHECK(!ydev_key_match("serial:0403", "serial:16C0:0483:1234560:x"));
    CHECK(!ydev_key_match("serial:16C0:0483:1234560:Port_#0004", "serial:16C0:0483:1234560:Port_#0003"));
    CHECK(ydev_key_match("port:COM5", "port:COM5"));
    CHECK(!ydev_key_match("port:COM5", "port:COM50"));
    CHECK(!ydev_key_match("port:COM5", "serial:16C0:0483::"));
    CHECK(!ydev_key_match("usb:1", "usb:1"));
    CHECK(!ydev_key_match(NULL, "port:x"));
    printf("  match keys: ok\n");
}

static void test_desc(void) {
    ydev_desc d = desc_for(YBOX_LINE);
    ydev_desc e;
    e = d; e.role = NULL;      CHECK(!ydev_open(&g_dev, &e) && strstr(ydev_error(&g_dev), "role"));
    e = d; e.family = 9;       CHECK(!ydev_open(&g_dev, &e) && strstr(ydev_error(&g_dev), "family"));
    e = d; e.key = "";         CHECK(!ydev_open(&g_dev, &e) && strstr(ydev_error(&g_dev), "key"));
    e = d; e.device = 0;       CHECK(!ydev_open(&g_dev, &e) && strstr(ydev_error(&g_dev), "device"));
    e = d; e.kind = 16;        CHECK(!ydev_open(&g_dev, &e) && strstr(ydev_error(&g_dev), "kind"));
    e = d; e.transport.read = NULL; CHECK(!ydev_open(&g_dev, &e) && strstr(ydev_error(&g_dev), "transport"));
    e = d; memset(&e.transport, 0, sizeof e.transport); e.key = "usb:1";
    CHECK(!ydev_open(&g_dev, &e) && strstr(ydev_error(&g_dev), "serial:"));
    {
        /* key and role are copied into the handle: limits (v0.3.0) */
        char longkey[YDEV_KEY_MAX + 1], longrole[YDEV_ROLE_MAX + 1];
        memset(longkey, 'k', sizeof longkey);
        memcpy(longkey, "serial:", 7);
        longkey[YDEV_KEY_MAX] = '\0';          /* 160 characters: one too many */
        memset(longrole, 'r', sizeof longrole);
        longrole[YDEV_ROLE_MAX] = '\0';        /* 32 characters: one too many */
        e = d; e.key = longkey;  CHECK(!ydev_open(&g_dev, &e) && strstr(ydev_error(&g_dev), "YDEV_KEY_MAX"));
        e = d; e.role = longrole; CHECK(!ydev_open(&g_dev, &e) && strstr(ydev_error(&g_dev), "YDEV_ROLE_MAX"));
        longkey[YDEV_KEY_MAX - 1] = '\0';      /* 159: fits */
        longrole[YDEV_ROLE_MAX - 1] = '\0';    /* 31: fits */
        e = d; e.key = longkey; e.role = longrole;
        CHECK(ydev_open(&g_dev, &e));
        CHECK(g_dev.d.key != longkey && strcmp(g_dev.d.key, longkey) == 0);
        CHECK(g_dev.d.role != longrole && strcmp(g_dev.d.role, longrole) == 0);
        ydev_close(&g_dev);
    }
    ydev_close(&g_dev);         /* a close after a failed open is harmless */
    {
        yin_source s;
        board_init(YBOX_LINE, 0.0, 125000.0, 50000.0);
        CHECK(ydev_open(&g_dev, &d));
        s = ydev_source(&g_dev);
        CHECK(s.kind == YIN_KIND_SYNC && s.device == 3 && s.tier == YIN_TIER_UNKNOWN && s.note != NULL);
        ydev_close(&g_dev);
        ydev_close(&g_dev);     /* twice */
    }
    printf("  desc checks and the source entry: ok\n");
}

/* ydev_open() returns while OPENING; ydev_wait_running() waits for
 * RUNNING (v0.3.0). The v0.2 USAGE pulsed at once and lost that trigger. */
static void test_wait_running(void) {
    char key[64];
    ring_reset();
    board_init(YBOX_LINE, 0.0, 125000.0, 50000.0);
    {
        ydev_desc d = desc_for(YBOX_LINE);
        snprintf(key, sizeof key, "%s", "serial:16C0:0483::");
        d.key = key;
        CHECK(ydev_open(&g_dev, &d));
    }
    key[0] = '\0';              /* the caller's buffer is gone: the handle has a copy */
    CHECK_I(ydev_state(&g_dev), YDEV_OPENING);
    /* the old USAGE: a pulse right after the open is refused */
    CHECK_I(ydev_out_pulse(&g_dev, 1, YRT_MS(2)), YDEV_ERR_STATE);
    CHECK_I(ydev_wait_running(&g_dev, YRT_S(3)), YDEV_RUNNING);
    CHECK_I(ydev_out_pulse(&g_dev, 1, YRT_MS(2)), YDEV_OK);
    CHECK_I(ydev_wait_running(&g_dev, 0), YDEV_RUNNING);   /* already: look once */
    /* unplugged: LOST, and found again by the copied key */
    g_b.gone_from = g_now;
    g_b.gone_until = g_now + 1500000000LL;
    run_until(g_now + 500000000LL);
    CHECK_I(ydev_state(&g_dev), YDEV_LOST);
    run_until(g_now + 2500000000LL);   /* the caller's clock passes the gap */
    CHECK_I(ydev_wait_running(&g_dev, 0), YDEV_RUNNING);
    CHECK_I(g_b.opens, 2);
    ydev_close(&g_dev);
    /* closed: the wait ends at once with a message */
    CHECK_I(ydev_wait_running(&g_dev, YRT_S(1)), YDEV_ERR_STATE);
    CHECK(strstr(ydev_error(&g_dev), "not open") != NULL);
    /* no device at the key: the virtual clock stands still while the open
     * fails, so the wait gives up instead of spinning */
    board_init(YBOX_LINE, 0.0, 125000.0, 50000.0);
    g_b.gone_from = 0;
    {
        ydev_desc d = desc_for(YBOX_LINE);
        CHECK(ydev_open(&g_dev, &d));
    }
    CHECK_I(ydev_wait_running(&g_dev, YRT_S(1)), YDEV_ERR_TIMEOUT);
    CHECK(strstr(ydev_error(&g_dev), "did not run in time") != NULL);
    ydev_close(&g_dev);
    CHECK(strcmp(ydev_strerror(YDEV_ERR_TIMEOUT), "the device did not run in time") == 0);
    CHECK(strcmp(ydev_strerror(0), "ok") == 0);
    printf("  wait for RUNNING, the copied key, a refused early pulse: ok\n");
}

/* --- outputs ---------------------------------------------------------------------- */

/* A trigger box behind a fake transport: it keeps every write (bytes and
 * time) and every change of the modem lines. On the virtual clock a read
 * that finds nothing moves the clock; on the real clock it sleeps 1 ms. */
typedef struct obox {
    uint8_t  bytes[65536];
    int      nbytes;
    int64_t  wt[16384];       /* each write: time, first byte, size        */
    int      wfirst[16384], wn[16384];
    int      nw;
    uint32_t lcode[256], lchanged[256];
    int      nl;
    bool     open, fail;
    int      opens;
    int64_t  wcost;           /* virtual clock: what a write or a line
                               * change takes, as a USB write does         */
} obox;

static obox g_o;

static void* o_open(void* user, const char* key, char* found, size_t cap) {
    obox* o = (obox*)user;
    if (!ydev_key_match(key, "serial:0403:6001:TB1:A") && strcmp(key, "parallel:") != 0) return NULL;
    o->open = true;
    o->opens++;
    snprintf(found, cap, "TB1");
    return o;
}

static int o_read(void* conn, uint8_t* buf, int cap, int64_t timeout_ns) {
    (void)conn; (void)buf; (void)cap;
    if (g_real) (void)yrt_sleep_ns(1000000);
    else g_now += timeout_ns;
    return 0;
}

static int o_write(void* conn, const uint8_t* buf, int n) {
    obox* o = (obox*)conn;
    int64_t t = now_fn(NULL);
    if (o->fail) return -1;
    if (o->nw < 16384) { o->wt[o->nw] = t; o->wfirst[o->nw] = o->nbytes; o->wn[o->nw++] = n; }
    if (o->nbytes + n <= (int)sizeof o->bytes) { memcpy(o->bytes + o->nbytes, buf, (size_t)n); o->nbytes += n; }
    if (!g_real) g_now += o->wcost;
    return n;
}

static int o_lines(void* conn, uint32_t code, uint32_t changed) {
    obox* o = (obox*)conn;
    if (o->nl < 256) { o->lcode[o->nl] = code; o->lchanged[o->nl++] = changed; }
    if (!g_real) g_now += o->wcost;
    return 0;
}

static void o_close(void* conn) { ((obox*)conn)->open = false; }

static ydev_transport ofake(bool with_read) {
    ydev_transport tr;
    memset(&tr, 0, sizeof tr);
    tr.open = o_open;
    tr.read = with_read ? o_read : NULL;
    tr.write = o_write;
    tr.close = o_close;
    tr.lines = o_lines;
    tr.user = &g_o;
    return tr;
}

static ydev_desc odesc(int family) {
    ydev_desc d;
    memset(&d, 0, sizeof d);
    d.role = "trig";
    d.family = family;
    d.key = family == YBOX_PARALLEL ? "parallel:" : "serial:0403:6001:TB1:";
    d.device = 5;
    d.transport = ofake(family != YBOX_PARALLEL);
    d.ring = &g_ring;
    d.manual = true;
    d.now = now_fn;
    d.probe_ns = -1;
    d.no_identify = true;
    return d;
}

static ydev_device g_out;

/* Polls the output instance until virtual time t. */
static void orun_until(int64_t t) {
    while (g_now < t) {
        int64_t before = g_now;
        (void)ydev_poll(&g_out, YRT_MS(1));
        if (g_now == before) g_now += 1000000;
    }
}

static bool obytes_are(const uint8_t* want, int n) {
    int i;
    if (g_o.nbytes != n) {
        fprintf(stderr, "device_test: %d bytes written, want %d:", g_o.nbytes, n);
        for (i = 0; i < g_o.nbytes; i++) fprintf(stderr, " %02X", g_o.bytes[i]);
        fprintf(stderr, "\n");
        return false;
    }
    return memcmp(g_o.bytes, want, (size_t)n) == 0;
}

static void ostart(const ydev_desc* d) {
    memset(&g_o, 0, sizeof g_o);
    ring_reset();
    CHECK(ydev_open(&g_out, d));
    CHECK_I(ydev_poll(&g_out, YRT_MS(1)), YDEV_RUNNING);
}

/* The OUT records in the ring, in order. */
static int g_nout;
static yrt_event g_outrec[4096];
static int g_ntext;
static char g_text[16][40];
static void odrain(void) {
    int i, n = yrt_ring_drain(&g_ring, g_rec, 16384);
    g_nout = 0;
    g_ntext = 0;
    for (i = 0; i < n; i++) {
        if (g_rec[i].source != YRT_SRC_DEVICE) continue;
        if (g_rec[i].kind == YDEV_REC_OUT && g_nout < 4096) g_outrec[g_nout++] = g_rec[i];
        if (g_rec[i].kind == YDEV_REC_TEXT && strncmp(g_rec[i].u.text, "mark ", 5) == 0 && g_ntext < 16)
            memcpy(g_text[g_ntext++], g_rec[i].u.text, 40);
    }
}

/* The bytes of each byte family, from the open to the stop. */
static void test_out_bytes(void) {
    ydev_desc d;
    /* TriggerBox: a 0 after the open; a byte holds; a pulse's 0 from the
     * host; a set cancels a pending 0; 0 then 0xFF at the stop */
    {
        static const uint8_t want[] = { 0x00, 0x05, 0x07, 0x00, 0x09, 0x03, 0x00, 0xFF };
        int64_t t;
        d = odesc(YBOX_TRIGGERBOX);
        ostart(&d);
        CHECK_I(ydev_out_set(&g_out, 5), YDEV_OK);
        CHECK_I(ydev_out_pulse(&g_out, 7, 2000000), YDEV_OK);
        t = g_now;
        orun_until(t + 3000000);
        CHECK(g_o.nw == 4 && g_o.wt[3] - g_o.wt[2] >= 2000000 && g_o.wt[3] - g_o.wt[2] < 3000000);
        CHECK_I(ydev_out_pulse(&g_out, 9, 2000000), YDEV_OK);
        CHECK_I(ydev_out_set(&g_out, 3), YDEV_OK);
        orun_until(g_now + 5000000);
        CHECK_I(ydev_out_pulse(&g_out, 1, 0), YDEV_ERR_ARG);     /* no width: no pulse */
        CHECK_I(ydev_out_set(&g_out, 256), YDEV_ERR_ARG);
        ydev_close(&g_out);
        CHECK(obytes_are(want, (int)sizeof want));
    }
    /* BioSemi: every code is the box's 8 ms pulse; no 0 at the open or the
     * stop; a width other than 8 ms is refused, at the call and at start */
    {
        static const uint8_t want[] = { 0x05, 0x05, 0x05 };
        d = odesc(YBOX_BIOSEMI);
        ostart(&d);
        CHECK_I(ydev_out_set(&g_out, 5), YDEV_OK);
        CHECK_I(ydev_out_pulse(&g_out, 5, 2000000), YDEV_ERR_ARG);
        CHECK_I(ydev_out_pulse(&g_out, 5, 0), YDEV_OK);
        CHECK_I(ydev_out_pulse(&g_out, 5, 8500000), YDEV_OK);
        orun_until(g_now + 20000000);
        ydev_close(&g_out);
        CHECK(obytes_are(want, (int)sizeof want));
        d.pulse_ns = 2000000;
        CHECK(!ydev_open(&g_out, &d) && strstr(ydev_error(&g_out), "8 ms"));
    }
    /* MMBT-S at switch P is BioSemi's case; at switch S a byte holds */
    {
        static const uint8_t want_p[] = { 0x07 };
        static const uint8_t want_s[] = { 0x00, 0x07, 0x00 };
        d = odesc(YBOX_MMBTS);
        ostart(&d);
        CHECK_I(ydev_out_pulse(&g_out, 7, 2000000), YDEV_ERR_ARG);
        CHECK_I(ydev_out_pulse(&g_out, 7, 8000000), YDEV_OK);
        ydev_close(&g_out);
        CHECK(obytes_are(want_p, 1));
        d.latched = true;
        ostart(&d);
        CHECK_I(ydev_out_pulse(&g_out, 7, 2000000), YDEV_OK);
        orun_until(g_now + 4000000);
        ydev_close(&g_out);
        CHECK(obytes_are(want_s, 3));
    }
    /* The parallel port's family on a transport without read */
    {
        static const uint8_t want[] = { 0x00, 0x81, 0x00 };
        d = odesc(YBOX_PARALLEL);
        ostart(&d);
        CHECK_I(ydev_out_pulse(&g_out, 0x81, 1000000), YDEV_OK);
        orun_until(g_now + 2000000);
        ydev_close(&g_out);
        CHECK(obytes_are(want, 3));
        memset(&d.transport, 0, sizeof d.transport);
        CHECK(!ydev_open(&g_out, &d) && strstr(ydev_error(&g_out), "ydev_parallel_transport"));
    }
    /* DTR and RTS: both low at the open; only the lines that change */
    {
        d = odesc(YBOX_LINES);
        ostart(&d);
        CHECK_I(ydev_out_set(&g_out, 1), YDEV_OK);
        CHECK_I(ydev_out_pulse(&g_out, 2, 1000000), YDEV_OK);
        orun_until(g_now + 2000000);
        CHECK_I(ydev_out_set(&g_out, 4), YDEV_ERR_ARG);
        ydev_close(&g_out);
        CHECK_I(g_o.nl, 4);
        CHECK(g_o.lcode[0] == 0 && g_o.lchanged[0] == 3);
        CHECK(g_o.lcode[1] == 1 && g_o.lchanged[1] == 1);
        CHECK(g_o.lcode[2] == 2 && g_o.lchanged[2] == 3);
        CHECK(g_o.lcode[3] == 0 && g_o.lchanged[3] == 2);
        CHECK_I(g_o.nbytes, 0);
    }
    /* XID: mp (whole ms, sent when it changes) then mh, in one write */
    {
        static const uint8_t want[] = { 'm', 'p', 0, 0, 0, 0, 'm', 'h', 0x02, 0x01,
                                        'm', 'h', 0x03, 0x00,
                                        'm', 'p', 5, 0, 0, 0, 'm', 'h', 0x01, 0x00,
                                        'm', 'h', 0x02, 0x00,
                                        'm', 'p', 0, 0, 0, 0, 'm', 'h', 0x00, 0x00 };
        ydev_out_info oi;
        d = odesc(YBOX_XID);
        ostart(&d);
        CHECK_I(ydev_out_set(&g_out, 0x0102), YDEV_OK);
        CHECK_I(ydev_out_set(&g_out, 3), YDEV_OK);
        CHECK_I(ydev_out_pulse(&g_out, 1, 4800000), YDEV_OK);
        ydev_out_last(&g_out, &oi);
        CHECK(oi.width_ns == 5000000 && (oi.flags & YDEV_OUT_DEVICE_TIMED) && oi.result == YDEV_OK);
        CHECK_I(ydev_out_pulse(&g_out, 2, 5000000), YDEV_OK);
        CHECK_I(ydev_out_set(&g_out, 0), YDEV_OK);
        CHECK_I(ydev_out_set(&g_out, 0x10000), YDEV_ERR_ARG);
        ydev_close(&g_out);
        CHECK(obytes_are(want, (int)sizeof want));
        CHECK_I(g_o.nw, 5);
    }
    /* The line protocol: o and p */
    {
        static const char want[] = "o 5\np 1 2000\n";
        d = odesc(YBOX_LINE);
        ostart(&d);
        CHECK_I(ydev_out_set(&g_out, 5), YDEV_OK);
        CHECK_I(ydev_out_pulse(&g_out, 1, 2000000), YDEV_OK);
        ydev_close(&g_out);     /* the board ends p at 0: nothing held at the stop */
        CHECK(obytes_are((const uint8_t*)want, (int)sizeof want - 1));
    }
    /* No outputs: a photodiode */
    {
        d = odesc(YBOX_PHOTO);
        ostart(&d);
        CHECK_I(ydev_out_set(&g_out, 1), YDEV_ERR_FAMILY);
        ydev_close(&g_out);
    }
    printf("  output bytes: TriggerBox, BioSemi, MMBT-S (P and S), parallel, DTR and RTS, XID, line: ok\n");
}

/* The DEVICE_OUT records: contents, pulses and their trailing edges,
 * replaced and canceled edges, failures, marks, the trigger channel. */
static void test_out_records(void) {
    ydev_desc d = odesc(YBOX_TRIGGERBOX);
    const yrt_event* r;
    int64_t lead_t1;
    d.pulse_ns = 1000000;
    ostart(&d);
    odrain();                                   /* the open's 0 */
    CHECK_I(g_nout, 1);
    CHECK(g_nout == 1 && g_outrec[0].u.u32[8] == 0 && g_outrec[0].u.u16[18] == 0);
    /* a pulse and its trailing edge */
    CHECK_I(ydev_out_pulse(&g_out, 12, 2000000), YDEV_OK);
    orun_until(g_now + 3000000);
    odrain();
    CHECK_I(g_nout, 2);
    if (g_nout == 2) {
        r = &g_outrec[0];
        CHECK(r->aux == 5 && (int64_t)r->t_ns == r->u.i64[0] && r->u.i64[1] >= r->u.i64[0]);
        CHECK(r->u.i64[2] == 2000000 && r->u.i64[3] == 0 && r->u.u32[8] == 12);
        CHECK_I(r->u.u16[18], YDEV_OUT_PULSE);
        lead_t1 = r->u.i64[1];
        r = &g_outrec[1];
        CHECK_I(r->u.u16[18], YDEV_OUT_TRAILING);
        CHECK(r->u.u16[19] == g_outrec[0].u.u16[19] && r->u.u32[8] == 0);
        /* from the leading write's start (v0.3.1), at least 0.25 ms after
         * its end */
        CHECK(r->u.i64[0] >= g_outrec[0].u.i64[0] + 2000000 && r->u.i64[0] >= lead_t1 + 250000 &&
              r->u.i64[0] < g_outrec[0].u.i64[0] + 3000000);
        CHECK(r->u.i64[2] == r->u.i64[0] - g_outrec[0].u.i64[0]);       /* the host width */
    }
    /* a pulse that moves a pending edge; a set that cancels one */
    CHECK_I(ydev_out_pulse(&g_out, 1, 2000000), YDEV_OK);
    CHECK_I(ydev_out_pulse(&g_out, 2, 2000000), YDEV_OK);
    orun_until(g_now + 3000000);
    CHECK_I(ydev_out_pulse(&g_out, 3, 2000000), YDEV_OK);
    CHECK_I(ydev_out_set(&g_out, 4), YDEV_OK);
    orun_until(g_now + 3000000);
    odrain();
    CHECK_I(g_nout, 5);
    if (g_nout == 5) {
        CHECK(g_outrec[1].u.u16[18] == (YDEV_OUT_PULSE | YDEV_OUT_REPLACED));
        CHECK(g_outrec[2].u.u16[18] == YDEV_OUT_TRAILING && g_outrec[2].u.u16[19] == g_outrec[1].u.u16[19]);
        CHECK(g_outrec[3].u.u32[8] == 3 && g_outrec[4].u.u32[8] == 4 && g_outrec[4].u.u16[18] == 0);
    }
    /* failures are records too */
    CHECK_I(ydev_out_set(&g_out, 300), YDEV_ERR_ARG);
    g_o.fail = true;
    CHECK_I(ydev_out_set(&g_out, 1), YDEV_ERR_IO);
    g_o.fail = false;
    odrain();
    CHECK(g_nout == 2 && (g_outrec[0].u.u16[18] & YDEV_OUT_FAILED) && (g_outrec[1].u.u16[18] & YDEV_OUT_FAILED));
    /* a mark: desc.pulse_ns's pulse, then its text */
    CHECK_I(ydev_out_mark(&g_out, 6, "condition A, block 2, trial 17: the long label"), YDEV_OK);
    orun_until(g_now + 2000000);
    odrain();
    CHECK(g_nout == 2 && g_outrec[0].u.u16[18] == (YDEV_OUT_PULSE | YDEV_OUT_MARK) && g_outrec[0].u.i64[2] == 1000000);
    CHECK(g_ntext == 2 && strcmp(g_text[0], "mark condition A, block 2, trial 17: th") == 0 &&
          strcmp(g_text[1], "mark e long label") == 0);
    /* the trigger channel with a fake flip: the screen's worker would call
     * this at the planned vblank */
    {
        yscr_trigger_desc ch = ydev_trigger_channel(&g_out, 250000);
        yscr_trigger_info ti;
        memset(&ti, 0, sizeof ti);
        ti.deadline_ns = g_now - 20000;
        ti.fired_ns = g_now;
        ti.code = 33;
        CHECK(ch.fn == ydev_trigger_fn && ch.ctx == (void*)&g_out && ch.offset_ns == 250000 &&
              strcmp(ch.name, "trig") == 0);
        ch.fn(ch.ctx, &ti);
        ti.flags = YSCR_TRIG_FLUSHED;           /* the screen's close: nothing */
        ch.fn(ch.ctx, &ti);
        orun_until(g_now + 2000000);
        odrain();
        CHECK_I(g_nout, 2);
        CHECK(g_nout == 2 && g_outrec[0].u.u16[18] == (YDEV_OUT_PULSE | YDEV_OUT_FLIP) &&
              g_outrec[0].u.i64[3] == ti.deadline_ns && g_outrec[0].u.u32[8] == 33);
        CHECK(g_nout == 2 && g_outrec[1].u.u16[18] == YDEV_OUT_TRAILING);
    }
    /* a pending edge at the stop: written at once, flagged */
    CHECK_I(ydev_out_pulse(&g_out, 8, 50000000), YDEV_OK);
    ydev_close(&g_out);
    odrain();
    CHECK(g_nout >= 2 && g_outrec[1].u.u16[18] == (YDEV_OUT_TRAILING | YDEV_OUT_FLUSHED));
    /* not running: refused and recorded */
    d.key = "serial:0403:6001:NOPE:";
    memset(&g_o, 0, sizeof g_o);
    ring_reset();
    CHECK(ydev_open(&g_out, &d));
    CHECK_I(ydev_poll(&g_out, YRT_MS(1)), YDEV_OPENING);
    CHECK_I(ydev_out_pulse(&g_out, 1, 1000000), YDEV_ERR_STATE);
    odrain();
    CHECK(g_nout == 1 && (g_outrec[0].u.u16[18] & YDEV_OUT_FAILED));
    CHECK_I(g_o.nbytes, 0);
    {
        ydev_stats st;
        ydev_get_stats(&g_out, &st);
        CHECK(st.outs == 1 && st.out_errors == 1);
    }
    ydev_close(&g_out);
    CHECK_I(ydev_out_set(&g_out, 1), YDEV_ERR_STATE);   /* after the stop */
    printf("  output records: pulse and trailing edge, replaced, canceled, failed, mark, trigger channel: ok\n");
}

/* Polls in 10 us steps until virtual time t. */
static void orun_fine(int64_t t) {
    while (g_now < t) {
        int64_t before = g_now;
        (void)ydev_poll(&g_out, 10000);
        if (g_now == before) g_now += 10000;
    }
}

/* One pulse of width w whose writes each take 0.4 ms of virtual time:
 * true with the leading write's start and return and the trailing
 * write's start. */
static bool ohold_pulse(int64_t w, int64_t* t0, int64_t* t1, int64_t* off) {
    g_o.wcost = 400000;
    if (ydev_out_pulse(&g_out, 1, w) != YDEV_OK) return false;
    orun_fine(g_now + w + 2000000);
    g_o.wcost = 0;
    odrain();
    if (g_nout != 2 || g_outrec[1].u.u16[18] != YDEV_OUT_TRAILING) return false;
    *t0 = g_outrec[0].u.i64[0];
    *t1 = g_outrec[0].u.i64[1];
    *off = g_outrec[1].u.i64[0];
    return *t1 - *t0 == 400000;
}

/* Where a host-timed pulse ends (v0.3.1): at the later of the leading
 * write's start + width and its return + desc.min_hold_ns, within one
 * 10 us poll step. Until v0.3.0 it was the return + width. */
static void test_out_hold(void) {
    static const int fam[3] = { YBOX_TRIGGERBOX, YBOX_LINES, YBOX_PARALLEL };
    int f;
    int64_t t0, t1, off;
    ydev_desc d;
    for (f = 0; f < 3; f++) {
        int64_t hold = fam[f] == YBOX_PARALLEL ? 0 : 250000;   /* the defaults */
        d = odesc(fam[f]);
        ostart(&d);
        odrain();
        /* a width longer than the write: from the start */
        CHECK(ohold_pulse(1000000, &t0, &t1, &off));
        CHECK(off >= t0 + 1000000 && off < t0 + 1000000 + 20000);
        /* shorter: the hold after the return */
        CHECK(ohold_pulse(100000, &t0, &t1, &off));
        CHECK(off >= t1 + hold && off < t1 + hold + 20000);
        ydev_close(&g_out);
    }
    /* practically no hold */
    d = odesc(YBOX_TRIGGERBOX);
    d.min_hold_ns = 1;
    ostart(&d);
    odrain();
    CHECK(ohold_pulse(100000, &t0, &t1, &off));
    CHECK(off >= t1 + 1 && off < t1 + 20000);
    ydev_close(&g_out);
    /* a hold on the parallel port, when asked for */
    d = odesc(YBOX_PARALLEL);
    d.min_hold_ns = 300000;
    ostart(&d);
    odrain();
    CHECK(ohold_pulse(100000, &t0, &t1, &off));
    CHECK(off >= t1 + 300000 && off < t1 + 300000 + 20000);
    ydev_close(&g_out);
    d.min_hold_ns = -1;
    CHECK(!ydev_open(&g_out, &d) && strstr(ydev_error(&g_out), "min_hold_ns") != NULL);
    printf("  host-timed pulse end: from the leading write's start, the hold after its return: ok\n");
}

/* A line-protocol board's O reports: mapped through the fit. */
static void test_out_report(void) {
    int64_t t_start = g_now;
    int i, n = 0;
    ydev_stats st;
    ring_reset();
    g_nev = 0;
    board_init(YBOX_LINE, 15.0, 125000.0, 50000.0);
    {
        ydev_desc d = desc_for(YBOX_LINE);
        CHECK(ydev_open(&g_dev, &d));
    }
    run_until(t_start + 10000000000LL);
    g_b.nout = 0;
    for (i = 0; i < 20; i++) {
        CHECK_I(ydev_out_set(&g_dev, (uint32_t)(i & 1)), YDEV_OK);
        run_until(g_now + 100000000);
    }
    ydev_get_stats(&g_dev, &st);
    CHECK_I(st.reports, 20);
    odrain();
    for (i = 0; i < g_nout && n < 20; i++) {
        const yrt_event* r = &g_outrec[i];
        double err;
        if (!(r->u.u16[18] & YDEV_OUT_REPORTED)) continue;
        err = (double)(r->u.i64[0] - g_b.out_at[n]);
        CHECK(r->u.u32[8] == (uint32_t)(n & 1) && fabs(err) < 100e3);
        n++;
    }
    CHECK_I(n, 20);
    ydev_close(&g_dev);
    printf("  line O reports: 20 of 20, mapped within 100 us of the board's change: ok\n");
}

/* Roles: indexes by name, kept across a restart, one instance per role. */
static void test_roles(void) {
    static ydev_device a, b;
    ydev_roles roles;
    ydev_desc d = odesc(YBOX_TRIGGERBOX), e;
    memset(&roles, 0, sizeof roles);
    memset(&g_o, 0, sizeof g_o);
    ring_reset();
    d.device = 0;
    d.roles = &roles;
    d.role = "trig";
    CHECK(ydev_open(&a, &d));
    e = d;
    e.role = "aux";
    CHECK(ydev_open(&b, &e));
    CHECK(a.d.device == 1 && b.d.device == 2);
    CHECK(ydev_role_index(&roles, "aux") == 2 && ydev_role_index(&roles, "eye") == 0);
    CHECK(ydev_role_device(&roles, 1) == &a && ydev_role_device(&roles, 2) == &b && ydev_role_device(&roles, 3) == NULL);
    CHECK(strcmp(ydev_role_name(&roles, 2), "aux") == 0 && ydev_role_name(&roles, 0) == NULL);
    (void)ydev_poll(&a, YRT_MS(1));
    (void)ydev_poll(&b, YRT_MS(1));
    CHECK_I(ydev_out_set(&b, 9), YDEV_OK);
    odrain();
    CHECK(g_nout >= 1 && g_outrec[g_nout - 1].aux == 2);
    /* a second running instance cannot take a role */
    {
        static ydev_device c;
        CHECK(!ydev_open(&c, &e) && strstr(ydev_error(&c), "bound"));
    }
    ydev_close(&a);
    CHECK(ydev_role_device(&roles, 1) == NULL);
    CHECK(ydev_open(&a, &d) && a.d.device == 1);        /* the same index again */
    e.device = 4;
    {
        static ydev_device c;
        CHECK(!ydev_open(&c, &e) && strstr(ydev_error(&c), "leave it 0"));
    }
    ydev_close(&a);
    ydev_close(&b);
    CHECK_I(roles.n, 2);
    printf("  roles: indexes 1 and 2, kept across a restart, one instance each: ok\n");
}

/* v0.4: the parameter tables. kind_name() has no default, so -Wswitch
 * (CI's -Wall -Werror) stops the build when yin_kind gains a value, and the
 * value check then fails until the kind row lists it. */
static const char* kind_name(int v) {
    switch ((yin_kind)v) {
    case YIN_KIND_KEYBOARD: return "keyboard";
    case YIN_KIND_MOUSE: return "mouse";
    case YIN_KIND_TOUCH: return "touch";
    case YIN_KIND_PEN: return "pen";
    case YIN_KIND_GAMEPAD: return "gamepad";
    case YIN_KIND_BOX: return "box";
    case YIN_KIND_EYE: return "eye";
    case YIN_KIND_SYNC: return "sync";
    case YIN_KIND_USER: return "user";
    }
    return NULL;
}

/* An enum row's "name=value" list: values rising, each the C enum's name,
 * inside min..max, and every value of the enum but skip listed. */
static bool values_ok(const ydev_param* p, const char* (*name_of)(int), int skip) {
    const char* s = p->values;
    int prev = -1, seen[64], v;
    memset(seen, 0, sizeof seen);
    if (!s) return false;
    while (*s) {
        const char* eq = strchr(s, '=');
        const char* end = strchr(s, ',');
        char name[48];
        if (!end) end = s + strlen(s);
        if (!eq || eq > end || (size_t)(eq - s) >= sizeof name) return false;
        memcpy(name, s, (size_t)(eq - s));
        name[eq - s] = '\0';
        v = atoi(eq + 1);
        if (v <= prev || v >= 64 || v < p->min || v > p->max || !name_of(v) || strcmp(name_of(v), name)) return false;
        seen[v] = 1;
        prev = v;
        s = *end ? end + 1 : end;
    }
    for (v = 0; v < 64; v++)
        if (v != skip && name_of(v) && !seen[v]) return false;
    return true;
}

typedef struct dev_field { const char* name; size_t off, size; } dev_field;
#define DEV_F(f) { #f, offsetof(ydev_desc, f), sizeof(((ydev_desc*)0)->f) }
static const dev_field g_dev_fields[] = {
    DEV_F(role), DEV_F(key), DEV_F(kind), DEV_F(baud), DEV_F(probe_ns), DEV_F(retry_ns), DEV_F(silent_ns),
    DEV_F(max_width_ns), DEV_F(read_timeout_ns), DEV_F(no_identify), DEV_F(no_elevate), DEV_F(pulse_ns),
    DEV_F(latched), DEV_F(min_hold_ns),
};

/* Whether family reads the field, from its capabilities: the rows must
 * follow them. */
static bool dev_reads(int family, const char* name) {
    uint32_t c = ybox_caps(family);
    bool host = (c & YBOX_CAP_OUT) && !(c & YBOX_CAP_PULSE) && !(c & YBOX_CAP_FIXED);
    if (!strcmp(name, "kind") || !strcmp(name, "silent_ns")) return (c & YBOX_CAP_IN) != 0;
    if (!strcmp(name, "probe_ns") || !strcmp(name, "max_width_ns") || !strcmp(name, "no_identify"))
        return (c & YBOX_CAP_IDENT) != 0;
    if (!strcmp(name, "baud")) return family != YBOX_PARALLEL;
    if (!strcmp(name, "pulse_ns")) return (c & YBOX_CAP_OUT) != 0;
    if (!strcmp(name, "latched")) return family == YBOX_MMBTS;
    if (!strcmp(name, "min_hold_ns")) return host || family == YBOX_MMBTS;
    return true;
}

static void test_params(void) {
    static ydev_device dev;
    const int nf = (int)(sizeof g_dev_fields / sizeof g_dev_fields[0]);
    int f, i, j, k, n = -1, rows = 0;
    CHECK(ydev_params(YBOX_NONE, &n) == NULL && n == 0);
    CHECK(ydev_params(YBOX_FAMILY_LAST + 1, &n) == NULL && n == 0);
    CHECK(ydev_params(YBOX_XID, NULL) != NULL);
    /* every family has a table: a new family fails here until it has one */
    for (f = YBOX_XID; f <= YBOX_FAMILY_LAST; f++) {
        const ydev_param* p = ydev_params(f, &n);
        int seen[sizeof g_dev_fields / sizeof g_dev_fields[0]];
        ydev_desc d;
        memset(seen, 0, sizeof seen);
        CHECK(p != NULL && n > 0);
        if (!p) continue;
        /* zero fields: what open makes of them */
        memset(&d, 0, sizeof d);
        d.role = "p";
        d.family = f;
        d.key = "serial:0403:6001::";
        d.device = 7;
        d.transport = ofake(true);
        d.manual = true;
        d.now = now_fn;
        CHECK(ydev_open(&dev, &d));
        for (i = 0; i < n; i++) {
            const dev_field* fl = NULL;
            size_t want = 0;
            rows++;
            for (j = 0; j < nf; j++)
                if (!strcmp(p[i].name, g_dev_fields[j].name)) { fl = &g_dev_fields[j]; seen[j]++; }
            g_checks++;
            if (!fl) {
                fprintf(stderr, "device_test: FAIL: %s row %s names no field\n", ybox_family_name(f), p[i].name);
                g_failures++;
                continue;
            }
            CHECK(p[i].type && p[i].unit && p[i].doc && p[i].doc[0]);
            CHECK(p[i].min <= p[i].def && p[i].def <= p[i].max);
            for (k = 0; k < i; k++) CHECK(strcmp(p[i].name, p[k].name) != 0);
            CHECK((strcmp(p[i].unit, "ns") == 0) == (strstr(p[i].name, "_ns") != NULL));
            if (!strcmp(p[i].type, "bool")) want = sizeof(bool);
            else if (!strcmp(p[i].type, "u32") || !strcmp(p[i].type, "enum")) want = 4;
            else if (!strcmp(p[i].type, "i64")) want = 8;
            else if (!strcmp(p[i].type, "str")) want = sizeof(const char*);
            CHECK_I(fl->size, want);
            CHECK((p[i].values != NULL) == (strcmp(p[i].type, "enum") == 0));
            /* 0 is the family's kind, so keyboard cannot be chosen */
            if (!strcmp(p[i].name, "kind")) CHECK(values_ok(&p[i], kind_name, YIN_KIND_KEYBOARD));
            if (!strcmp(p[i].type, "str")) continue;
            {
                const unsigned char* b = (const unsigned char*)&dev.d + fl->off;
                double got;
                if (fl->size == 1) got = *b;
                else if (fl->size == 4 && !strcmp(p[i].type, "u32")) { uint32_t x; memcpy(&x, b, 4); got = x; }
                else if (fl->size == 4) { int32_t x; memcpy(&x, b, 4); got = x; }
                else { int64_t x; memcpy(&x, b, 8); got = (double)x; }
                g_checks++;
                if (got != p[i].def) {
                    fprintf(stderr, "device_test: FAIL: %s row %s: a zero field is %.17g, the table says %.17g\n",
                            ybox_family_name(f), p[i].name, got, p[i].def);
                    g_failures++;
                }
            }
        }
        for (j = 0; j < nf; j++) {
            g_checks++;
            if (seen[j] != (dev_reads(f, g_dev_fields[j].name) ? 1 : 0)) {
                fprintf(stderr, "device_test: FAIL: %s has %d rows of %s\n", ybox_family_name(f), seen[j],
                        g_dev_fields[j].name);
                g_failures++;
            }
        }
        ydev_close(&dev);
    }
    printf("  parameters: %d families, %d rows, each a field the family reads with its default: ok\n",
           YBOX_FAMILY_LAST, rows);
}

static int cmp_i64(const void* x, const void* y) {
    int64_t a = *(const int64_t*)x, b = *(const int64_t*)y;
    return a < b ? -1 : a > b;
}

static int64_t pct(const int64_t* v, int n, double p) {
    int k = (int)(p * (double)(n - 1) + 0.5);
    return n ? v[k < 0 ? 0 : k >= n ? n - 1 : k] : 0;
}

/* Host-timed pulses on the real clock: the worker writes the trailing 0.
 * Width error = the start of the trailing write minus the start of the
 * leading write minus the width (the records' times); the worker's
 * lateness = trailing write minus its deadline (yser_pulse_off_at_ns() of
 * the leading write). Run under the guard's time mode for the
 * numbers (docs/device.md). On a loaded machine the worker can be later
 * than the gap to the next pulse, which then moves the edge (REPLACED):
 * that is the contract, so the check is that every pulse has its
 * trailing edge or was replaced, never that none was. */
static void test_out_timing(void) {
    enum { N = 300 };
    static int64_t werr[N], late[N], call[N], wr[N];
    ydev_desc d = odesc(YBOX_TRIGGERBOX);
    int i, k, n = 0, nl = 0, short_ = 0, npulse = 0, ntrail = 0, nrepl = 0;
    const char* pol;
    g_real = true;
    d.manual = false;
    d.now = NULL;
    d.no_elevate = false;
    memset(&g_o, 0, sizeof g_o);
    ring_reset();
    CHECK(ydev_open(&g_out, &d));
    for (i = 0; i < 200 && ydev_state(&g_out) != YDEV_RUNNING; i++) (void)yrt_sleep_ns(1000000);
    CHECK_I(ydev_state(&g_out), YDEV_RUNNING);
    for (i = 0; i < N; i++) {
        ydev_out_info oi;
        int64_t c0 = yrt_now_ns(), c1;
        CHECK_I(ydev_out_pulse(&g_out, (uint32_t)(1 + (i % 200)), 2000000), YDEV_OK);
        c1 = yrt_now_ns();
        ydev_out_last(&g_out, &oi);
        call[i] = c1 - c0;
        wr[i] = oi.t_after - oi.t_before;
        (void)yrt_sleep_ns(3000000 + (int64_t)(u01() * 4000000.0));
    }
    pol = yrt_policy_name(yrt_worker_policy(&g_out.worker));
    ydev_close(&g_out);
    g_real = false;
    /* writes: the open's 0, then each code and, unless the next pulse moved
     * it, its 0; the close's 0xFF */
    for (k = 1; k + 1 < g_o.nw && n < N; k++)
        if (g_o.bytes[g_o.wfirst[k]] != 0 && g_o.bytes[g_o.wfirst[k + 1]] == 0) n++;
    odrain();
    for (k = 0; k < g_nout; k++) {
        uint32_t f = g_outrec[k].u.u16[18];
        if (f & YDEV_OUT_PULSE) npulse++;
        if (f & YDEV_OUT_REPLACED) nrepl++;
        if (f & YDEV_OUT_TRAILING) ntrail++;
        if ((f & YDEV_OUT_PULSE) && k + 1 < g_nout && (g_outrec[k + 1].u.u16[18] & YDEV_OUT_TRAILING) && nl < N) {
            /* the host width, start to start; the deadline as v0.3.1 sets it */
            werr[nl] = g_outrec[k + 1].u.i64[2] - 2000000;
            if (werr[nl] < 0) short_++;
            late[nl++] = g_outrec[k + 1].u.i64[0] -
                         yser_pulse_off_at_ns(g_outrec[k].u.i64[0], g_outrec[k].u.i64[1], 2000000, YSER_DEFAULT_MIN_HOLD_NS);
        }
    }
    CHECK_I(short_, 0);
    CHECK_I(npulse, N);
    CHECK_I(ntrail + nrepl, N);     /* every pulse ended, or moved by the next */
    CHECK_I(n, ntrail);
    CHECK(nl > 0 && n > 0);
    if (nl == 0 || n == 0) { g_real = false; return; }
    n = nl;
    qsort(werr, (size_t)n, sizeof werr[0], cmp_i64);
    qsort(late, (size_t)nl, sizeof late[0], cmp_i64);
    qsort(call, N, sizeof call[0], cmp_i64);
    qsort(wr, N, sizeof wr[0], cmp_i64);
    printf("  timing: %d pulses, %d ended by the worker, %d moved by the next pulse\n", npulse, ntrail, nrepl);
    printf("  timing, %d pulses of 2 ms on the worker (%s): width error p50 %+.1f us, p95 %+.1f, p99 %+.1f, max %+.1f\n",
           n, pol, (double)pct(werr, n, 0.5) / 1e3,
           (double)pct(werr, n, 0.95) / 1e3, (double)pct(werr, n, 0.99) / 1e3, (double)werr[n - 1] / 1e3);
    printf("  timing: trailing write minus deadline p50 %.1f us, p95 %.1f, p99 %.1f, max %.1f\n",
           (double)pct(late, nl, 0.5) / 1e3, (double)pct(late, nl, 0.95) / 1e3, (double)pct(late, nl, 0.99) / 1e3,
           (double)late[nl - 1] / 1e3);
    printf("  timing: ydev_out_pulse() call p50 %.2f us, p99 %.2f, max %.2f; bracket (t_after - t_before) p50 %.3f us, "
           "max %.3f\n", (double)pct(call, N, 0.5) / 1e3, (double)pct(call, N, 0.99) / 1e3, (double)call[N - 1] / 1e3,
           (double)pct(wr, N, 0.5) / 1e3, (double)wr[N - 1] / 1e3);
    /* loose enough for a loaded CI machine; the numbers are the guard's */
    CHECK(pct(werr, n, 0.5) < 1000000);
}

int main(void) {
    printf("device_test: ysp/device.h %s, %d-byte instance\n", ydev_version(), (int)sizeof(ydev_device));
    test_keys();
    test_desc();
    test_line_unplug();
    test_xid();
    test_photo();
    test_clamp();
    test_silence();
    test_wrong_device();
    test_wait_running();
    test_thread();
    test_out_bytes();
    test_out_records();
    test_out_hold();
    test_out_report();
    test_roles();
    test_params();
    test_out_timing();
    if (g_failures) {
        fprintf(stderr, "device_test: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("device_test: all %d checks passed\n", g_checks);
    return 0;
}
