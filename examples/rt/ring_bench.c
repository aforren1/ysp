/* rt_ring_bench.c - what ysp/rt.h's event ring, trace macros and clock
 * correlation cost on THIS machine.
 *
 * Sections, each printed as a table:
 *   B1  push from one thread, with the stamps set and with t_ns and tid 0
 *   B2  push from 2, 4 and 8 threads at once while one thread drains
 *   B3  an elevated producer pushing every 1 ms while 7 others hammer the
 *       ring, against a mutex-protected ring as the baseline
 *   B3b ("all" only) the same elevated producer on an oversubscribed
 *       machine with one producer below normal priority, and a sweep of
 *       injected mid-push stalls, to show when a stalled producer makes the
 *       ring refuse the elevated one
 *   B4  drain cost per record, by batch size
 *   B5  the instrumentation macros in this build's mode, and the clock read
 *   B6  clock correlation: bracket width and call cost per clock
 *
 * Times are taken around blocks of 64 operations, so one clock read is
 * spread over 64, and reported per operation as mean, p50, p99, p99.9 and
 * max over blocks. B3 times each push alone, clock read included, because
 * one slow push is what it looks for.
 *
 * The macro mode is this file's: YRT_TRACE_RING by default. Build it with
 * -DRT_BENCH_NOTHING for the default (empty) macros, or with
 * -DRT_BENCH_TRACY -DTRACY_ENABLE, Tracy's public/ directory on the include
 * path and TracyClient.cpp linked, for the Tracy C API.
 *
 * Build (from the repository root):
 *     cc -O2 -pthread -Iinclude -o rt_ring_bench examples/rt/ring_bench.c
 *     cl /O2 /Iinclude examples\rt\ring_bench.c
 * or:  cmake -B build && cmake --build build
 *
 * Usage: rt_ring_bench [pushes] [max_producers] [b3_ms] [all]
 *     rt_ring_bench                    # 1e6 pushes, up to 8 producers, 2 s of B3
 *     rt_ring_bench 100000 2 200       # the short run CI does
 *     rt_ring_bench 1000000 8 2000 all # adds B3b, about 25 s
 *
 * Exit code: 0. The numbers are the result; nothing here can fail but a
 * thread that does not start, which is reported.
 */
#if defined(RT_BENCH_TRACY)
    #define YRT_TRACY
#elif !defined(RT_BENCH_NOTHING)
    #define YRT_TRACE_RING
#endif
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef YRT_NO_THREADS
int main(void) {
    puts("rt_ring_bench: built with YRT_NO_THREADS; the contention sections need threads.");
    return 0;
}
#else

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <pthread.h>
    #include <time.h>
    #include <unistd.h>
#endif

#define BLOCK 64
#define MAX_P 8

/* ------------------------------------------------------------ statistics */

/* Shell sort: in place, no allocation, as in rt_jitter.c. */
static void sort_u64(uint64_t* a, size_t n) {
    static const size_t gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };
    size_t g, i, j;
    for (g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++) {
        size_t gap = gaps[g];
        for (i = gap; i < n; i++) {
            uint64_t v = a[i];
            for (j = i; j >= gap && a[j - gap] > v; j -= gap) a[j] = a[j - gap];
            a[j] = v;
        }
    }
}

/* One table row from per-block totals: everything per operation. */
static void row(const char* label, uint64_t* blk, size_t n, double per) {
    double sum = 0;
    size_t i;
    if (n == 0) { printf("| %-34s | no samples |\n", label); return; }
    for (i = 0; i < n; i++) sum += (double)blk[i];
    sort_u64(blk, n);
    printf("| %-34s | %8.1f | %8.1f | %8.1f | %8.1f | %9.1f |\n", label,
           sum / (double)n / per, (double)blk[n / 2] / per,
           (double)blk[(size_t)((double)n * 0.99)] / per,
           (double)blk[(size_t)((double)n * 0.999)] / per, (double)blk[n - 1] / per);
}

static void header(const char* title, const char* unit) {
    printf("\n%s\n\n| %-34s | mean %s | p50 | p99 | p99.9 | max |\n"
           "|---|---|---|---|---|---|\n", title, "", unit);
}

/* The test's own atomics, so the harness is not what is measured. */
#if defined(_MSC_VER)
static int b_load(const int* p) { return (int)*(const volatile long*)p; }
static void b_store(int* p, int v) { (void)InterlockedExchange((volatile LONG*)p, (LONG)v); }
#else
static int b_load(const int* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void b_store(int* p, int v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
#endif

/* The claim half of a push, for B3c's injected stall. */
#if defined(_MSC_VER)
static uint32_t bench_fadd(uint32_t* p) { return (uint32_t)_InterlockedExchangeAdd((volatile long*)p, 1); }
#else
static uint32_t bench_fadd(uint32_t* p) { return __atomic_fetch_add(p, 1u, __ATOMIC_ACQ_REL); }
#endif

/* ------------------------------------------------------------- the rings */

static unsigned char g_mem[YRT_RING_BYTES(65536)];
static yrt_ring g_ring;
static yrt_event g_out[4096];

static void ring_open(uint32_t records) {
    yrt_ring_desc d;
    memset(&d, 0, sizeof(d));
    d.memory = g_mem;
    d.bytes = YRT_RING_BYTES(records);
    if (!yrt_ring_open(&g_ring, &d)) {
        fprintf(stderr, "rt_ring_bench: %s\n", yrt_ring_error(&g_ring));
        exit(1);
    }
}

static void drain_all(void) {
    while (yrt_ring_drain(&g_ring, g_out, 4096) > 0) { }
}

/* The baseline: the same records behind one lock. */
typedef struct mring {
#if defined(_WIN32)
    SRWLOCK lock;
#else
    pthread_mutex_t lock;
#endif
    yrt_event* slots;
    uint32_t head, tail, cap, dropped;
} mring;

static mring g_mring;

static void mring_open(uint32_t cap) {
    memset(&g_mring, 0, sizeof(g_mring));
#if defined(_WIN32)
    InitializeSRWLock(&g_mring.lock);
#else
    pthread_mutex_init(&g_mring.lock, NULL);
#endif
    g_mring.slots = (yrt_event*)(void*)g_mem;
    g_mring.cap = cap;
}

static void mring_lock(void) {
#if defined(_WIN32)
    AcquireSRWLockExclusive(&g_mring.lock);
#else
    pthread_mutex_lock(&g_mring.lock);
#endif
}

static void mring_unlock(void) {
#if defined(_WIN32)
    ReleaseSRWLockExclusive(&g_mring.lock);
#else
    pthread_mutex_unlock(&g_mring.lock);
#endif
}

static int mring_push(const yrt_event* ev) {
    mring_lock();
    if (g_mring.head - g_mring.tail >= g_mring.cap) {
        g_mring.dropped++;
        mring_unlock();
        return YRT_ERR_FULL;
    }
    g_mring.slots[g_mring.head % g_mring.cap] = *ev;
    g_mring.head++;
    mring_unlock();
    return 0;
}

static int mring_drain(yrt_event* out, int cap) {
    int k = 0;
    mring_lock();
    while (k < cap && g_mring.tail != g_mring.head) {
        out[k++] = g_mring.slots[g_mring.tail % g_mring.cap];
        g_mring.tail++;
    }
    mring_unlock();
    return k;
}

static int g_use_mutex = 0;

static int any_push(const yrt_event* ev) {
    return g_use_mutex ? mring_push(ev) : yrt_ring_push(&g_ring, ev);
}

static int any_drain(yrt_event* out, int cap) {
    return g_use_mutex ? mring_drain(out, cap) : yrt_ring_drain(&g_ring, out, cap);
}

static yrt_event make_event(uint32_t i) {
    yrt_event e;
    memset(&e, 0, sizeof(e));
    e.t_ns = (uint64_t)i + 1u;
    e.tid = 1u;
    e.source = (uint16_t)YRT_SRC_USER;
    e.kind = 1;
    e.aux = i;
    e.u.u64[0] = i;
    return e;
}

/* ------------------------------------------------------------------- B1 */

static uint64_t* g_blk;   /* per-block samples, sized in main */

static void b1(size_t pushes) {
    size_t nb = pushes / BLOCK, b;
    int k, pass;
    yrt_event e = make_event(0), z;
    memset(&z, 0, sizeof(z));
    z.source = (uint16_t)YRT_SRC_USER;
    header("B1. push, one thread, ring of 4096 drained between blocks", "ns");
    for (pass = 0; pass < 2; pass++) {
        ring_open(4096);
        for (b = 0; b < nb; b++) {
            uint64_t t0 = yrt_now_ns();
            if (pass == 0) for (k = 0; k < BLOCK; k++) (void)yrt_ring_push(&g_ring, &e);
            else           for (k = 0; k < BLOCK; k++) (void)yrt_ring_push(&g_ring, &z);
            g_blk[b] = yrt_now_ns() - t0;
            drain_all();
        }
        row(pass == 0 ? "push, t_ns and tid set" : "push, t_ns and tid 0 (stamped)",
            g_blk, nb, BLOCK);
    }
}

/* ------------------------------------------------------------------- B2 */

typedef struct prod {
    yrt_worker w;
    const int* go;
    const int* stop;     /* NULL = run all `pushes` */
    int done;
    size_t pushes;
    uint64_t* blk;
    size_t nblk;
    long refused;
    uint32_t pause_ns;   /* spin after each block, 0 = back-to-back */
} prod;

static prod g_prod[MAX_P];

static void prod_job(void* ctx, const yrt_job_info* info) {
    prod* p = (prod*)ctx;
    yrt_event e = make_event(7);
    size_t b, nb = p->pushes / BLOCK;
    int k;
    (void)info;
    while (!b_load(p->go)) { }
    for (b = 0; b < nb; b++) {
        uint64_t t0 = yrt_now_ns();
        if (p->stop && b_load(p->stop)) break;
        for (k = 0; k < BLOCK; k++)
            if (any_push(&e) == YRT_ERR_FULL) p->refused++;
        p->blk[b] = yrt_now_ns() - t0;
        if (p->pause_ns) (void)yrt_spin_until(yrt_now_ns() + p->pause_ns);
    }
    p->nblk = b;
    b_store(&p->done, 1);
}

static int start_workers(prod* ps, int n, bool elevate) {
    yrt_worker_desc wd;
    int i;
    memset(&wd, 0, sizeof(wd));
    wd.no_elevate = !elevate;
    for (i = 0; i < n; i++) {
        memset(&ps[i].w, 0, sizeof(ps[i].w));
        if (!yrt_worker_open(&ps[i].w, &wd)) {
            fprintf(stderr, "rt_ring_bench: worker: %s\n", yrt_worker_error(&ps[i].w));
            return i;
        }
    }
    return n;
}

static void b2(size_t pushes, int max_p) {
    static uint64_t merged[1u << 20];
    int np;
    header("B2. push under contention: all producers back-to-back, one thread draining", "ns");
    for (np = 2; np <= max_p; np *= 2) {
        int go = 0, i, started, all;
        size_t per = pushes / (size_t)np, m = 0;
        long refused = 0;
        uint64_t t0, el;
        char label[64];
        ring_open(4096);
        memset(g_prod, 0, sizeof(g_prod));
        started = start_workers(g_prod, np, false);
        for (i = 0; i < started; i++) {
            g_prod[i].go = &go;
            g_prod[i].pushes = per;
            g_prod[i].blk = g_blk + (size_t)i * (per / BLOCK + 1);
            (void)yrt_worker_submit(&g_prod[i].w, 0, prod_job, &g_prod[i]);
        }
        yrt_sleep_ns(20000000ull);
        t0 = yrt_now_ns();
        b_store(&go, 1);
        do {
            all = 1;
            for (i = 0; i < started; i++) if (!b_load(&g_prod[i].done)) all = 0;
            while (yrt_ring_drain(&g_ring, g_out, 4096) > 0) { }
        } while (!all);
        el = yrt_now_ns() - t0;
        drain_all();
        for (i = 0; i < started; i++) {
            yrt_worker_close(&g_prod[i].w);
            refused += g_prod[i].refused;
            if (m + g_prod[i].nblk <= sizeof(merged) / sizeof(merged[0])) {
                memcpy(merged + m, g_prod[i].blk, g_prod[i].nblk * sizeof(uint64_t));
                m += g_prod[i].nblk;
            }
        }
        snprintf(label, sizeof(label), "%d producers, %.0f%% refused, %.0f M/s", started,
                 100.0 * (double)refused / (double)(per * (size_t)started),
                 (double)(per * (size_t)started) / ((double)el / 1e9) / 1e6);
        row(label, merged, m, BLOCK);
    }
}

/* ------------------------------------------------------------------- B3 */

typedef struct elev {
    yrt_worker w;
    const int* go;
    int done;
    int count;
    uint64_t* lat;
    long refused;
    yrt_policy policy;
    int control;         /* time the two clock reads only, no push */
} elev;

static elev g_elev;

static void elev_job(void* ctx, const yrt_job_info* info) {
    elev* e = (elev*)ctx;
    yrt_event ev = make_event(1);
    uint64_t next;
    int i;
    (void)info;
    while (!b_load(e->go)) { }
    next = yrt_now_ns();
    for (i = 0; i < e->count; i++) {
        uint64_t t0, t1;
        int rc;
        next += 1000000ull;
        (void)yrt_sleep_until(next, 0);
        t0 = yrt_now_ns();
        rc = e->control ? 0 : any_push(&ev);
        t1 = yrt_now_ns();
        e->lat[i] = t1 - t0;
        if (rc == YRT_ERR_FULL) e->refused++;
    }
    b_store(&e->done, 1);
}

/* One B3 run: the elevated producer, `hammers` paced producers, a drainer. */
static void b3_run(int mutex, int control, int ms, int hammers, uint64_t* lat,
                   const char* what) {
    int go = 0, stop = 0, i, started, stalls = 0;
    long hrefused = 0;
    uint64_t stall_t0 = 0, stall_max = 0, last_poll = 0, max_gap = 0;
    char label[96];
    g_use_mutex = mutex;
    if (mutex) mring_open(4096); else ring_open(4096);
    memset(g_prod, 0, sizeof(g_prod));
    memset(&g_elev, 0, sizeof(g_elev));
    started = start_workers(g_prod, hammers, false);
    for (i = 0; i < started; i++) {
        g_prod[i].go = &go;
        g_prod[i].stop = &stop;
        g_prod[i].pushes = (size_t)ms * 2000u;   /* more than the run needs */
        g_prod[i].pause_ns = 400000u;            /* bursts of 64, 1.1 M/s from 7:
                                                  * the ring holds 3.6 ms of it */
        g_prod[i].blk = g_blk + (size_t)i * ((size_t)ms * 2000u / BLOCK + 1);
        (void)yrt_worker_submit(&g_prod[i].w, 0, prod_job, &g_prod[i]);
    }
    {
        yrt_worker_desc wd;
        memset(&wd, 0, sizeof(wd));
        wd.spin_ns = 1;
        if (!yrt_worker_open(&g_elev.w, &wd)) {
            fprintf(stderr, "rt_ring_bench: elevated worker: %s\n", yrt_worker_error(&g_elev.w));
            return;
        }
    }
    g_elev.go = &go;
    g_elev.count = ms;
    g_elev.lat = lat;
    g_elev.control = control;
    g_elev.policy = yrt_worker_policy(&g_elev.w);
    (void)yrt_worker_submit(&g_elev.w, 0, elev_job, &g_elev);
    yrt_sleep_ns(20000000ull);
    b_store(&go, 1);
    while (!b_load(&g_elev.done)) {
        /* A drain that returns nothing while tickets are out is waiting on
         * a push that claimed a slot and has not published it. */
        int n = any_drain(g_out, 4096);
        if (!mutex) {
            /* The drainer itself can be descheduled, which looks the same
             * from here; a stall counts only if the drainer kept polling
             * through it, every 20 us or more often. */
            uint32_t head = *(volatile uint32_t*)&g_ring.head;
            uint64_t now = yrt_now_ns();
            if (stall_t0 && now - last_poll > max_gap) max_gap = now - last_poll;
            last_poll = now;
            if (n == 0 && head != g_ring.tail) {
                if (!stall_t0) { stall_t0 = now; max_gap = 0; }
            } else if (stall_t0) {
                uint64_t d = now - stall_t0;
                if (d > 100000u && max_gap < 20000u) {
                    stalls++;
                    if (d > stall_max) stall_max = d;
                }
                stall_t0 = 0;
            }
        }
    }
    b_store(&stop, 1);
    for (;;) {
        int all = 1;
        (void)any_drain(g_out, 4096);
        for (i = 0; i < started; i++) if (!b_load(&g_prod[i].done)) all = 0;
        if (all) break;
    }
    for (i = 0; i < started; i++) { yrt_worker_close(&g_prod[i].w); hrefused += g_prod[i].refused; }
    yrt_worker_close(&g_elev.w);
    while (any_drain(g_out, 4096) > 0) { }
    snprintf(label, sizeof(label), "%s, %s, refused %ld of %d (others %ld)", what,
             yrt_policy_name(g_elev.policy), g_elev.refused, ms, hrefused);
    row(label, lat, (size_t)ms, 1.0);
    if (!mutex && !control)
        printf("|   drain stalled behind an unpublished push, drainer polling throughout: "
               "%d times over 0.1 ms, "
               "longest %.2f ms |\n", stalls, (double)stall_max / 1e6);
    g_use_mutex = 0;
}

static void b3(int ms) {
    static uint64_t lat[200000];
    if (ms > 200000) ms = 200000;
    header("B3. one elevated producer, a push every 1 ms, 7 others hammering; per push, clock read included", "ns");
    b3_run(0, 1, ms, 7, lat, "control: clock reads, no push");
    b3_run(0, 0, ms, 7, lat, "wait-free ring");
    b3_run(1, 0, ms, 7, lat, "mutex ring (baseline)");
}

/* ------------------------------------------------------------------ B3b */

typedef struct spinner { yrt_worker w; const int* quit; } spinner;
static spinner g_spin[64];

static volatile uint64_t g_spin_sink;

static void spin_job(void* ctx, const yrt_job_info* info) {
    spinner* s = (spinner*)ctx;
    (void)info;
    while (!b_load(s->quit)) g_spin_sink++;
}

typedef struct lowp { const int* quit; uint64_t pushes; } lowp;
static lowp g_low;

/* The below-normal producer is a pump's idle callback: the pump is the one
 * ysp/rt.h thread that runs below normal. One push, then 50 us of spin, so it
 * is always runnable and competes for a core like real background work. */
static bool low_idle(void* ctx) {
    lowp* l = (lowp*)ctx;
    yrt_event ev = make_event(2);
    int k;
    if (b_load(l->quit)) return false;
    for (k = 0; k < 20; k++) {
        (void)yrt_ring_push(&g_ring, &ev);
        l->pushes++;
        (void)yrt_spin_until(yrt_now_ns() + 50000u);
    }
    return true;
}

static void low_msg(void* ctx, const void* msg, uint32_t seq) { (void)ctx; (void)msg; (void)seq; }

static int ncpu(void) {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

static void b3b(int ms) {
    static uint64_t lat[200000];
    static yrt_pump pump;
    yrt_pump_desc pd;
    int quit = 0, go = 0, i, ns = 2 * ncpu(), stalls = 0;
    yrt_policy low_policy;
    uint64_t longest = 0, stall_start = 0, next;
    yrt_worker_desc wd;
    char label[128];
    if (ns > 64) ns = 64;
    if (ms > 200000) ms = 200000;
    header("B3b. elevated producer on an oversubscribed machine, one producer below normal", "ns");
    ring_open(4096);
    memset(&wd, 0, sizeof(wd));
    wd.no_elevate = true;
    for (i = 0; i < ns; i++) {
        g_spin[i].quit = &quit;
        if (!yrt_worker_open(&g_spin[i].w, &wd)) { ns = i; break; }
        (void)yrt_worker_submit(&g_spin[i].w, 0, spin_job, &g_spin[i]);
    }
    memset(&g_low, 0, sizeof(g_low));
    g_low.quit = &quit;
    memset(&pd, 0, sizeof(pd));
    pd.msg_size = 4;
    pd.capacity = 1;
    pd.on_msg = low_msg;
    pd.on_idle = low_idle;
    pd.ctx = &g_low;
    pd.below_normal = true;
    memset(&pump, 0, sizeof(pump));
    if (!yrt_pump_open(&pump, &pd)) fprintf(stderr, "pump: %s\n", yrt_pump_error(&pump));
    low_policy = yrt_pump_policy(&pump);
    memset(&g_elev, 0, sizeof(g_elev));
    memset(&wd, 0, sizeof(wd));
    wd.spin_ns = 1;
    if (!yrt_worker_open(&g_elev.w, &wd)) return;
    g_elev.go = &go;
    g_elev.count = ms;
    g_elev.lat = lat;
    g_elev.policy = yrt_worker_policy(&g_elev.w);
    (void)yrt_worker_submit(&g_elev.w, 0, elev_job, &g_elev);
    b_store(&go, 1);
    /* The drainer: a frame loop's, once per 16.7 ms. A drain that finds
     * claimed records but can return none is waiting on a stalled push. */
    next = yrt_now_ns();
    while (!b_load(&g_elev.done)) {
        uint32_t head, tail;
        int n;
        next += 16666667ull;
        (void)yrt_sleep_until(next, 0);
        n = yrt_ring_drain(&g_ring, g_out, 4096);
        head = *(volatile uint32_t*)&g_ring.head;
        tail = g_ring.tail;
        if (n == 0 && head != tail) {
            if (!stall_start) { stall_start = yrt_now_ns(); stalls++; }
        } else if (stall_start) {
            uint64_t d = yrt_now_ns() - stall_start;
            if (d > longest) longest = d;
            stall_start = 0;
        }
    }
    b_store(&quit, 1);
    yrt_worker_close(&g_elev.w);
    yrt_pump_close(&pump);
    for (i = 0; i < ns; i++) yrt_worker_close(&g_spin[i].w);
    drain_all();
    snprintf(label, sizeof(label), "%d spinners on %d CPUs, %s, refused %ld of %d", ns, ncpu(),
             yrt_policy_name(g_elev.policy), g_elev.refused, ms);
    row(label, lat, (size_t)ms, 1.0);
    printf("\nbelow-normal producer: %llu pushes, policy %s; drains stalled behind an "
           "unpublished push: %d, longest %.1f ms; ring refusals in all: %lu\n",
           (unsigned long long)g_low.pushes, yrt_policy_name(low_policy),
           stalls, (double)longest / 1e6, (unsigned long)yrt_ring_dropped(&g_ring));
}

/* An injected stall: claim a ticket the way a push does, hold it for
 * `stall_ms` as a preempted producer would, then publish. Meanwhile a
 * producer pushes at 6000 records/s and the drainer drains every 16.7 ms. */
static void b3c(void) {
    static const int stalls_ms[] = { 100, 400, 600, 800, 1200 };
    size_t s;
    printf("\nB3c. a producer stalled mid-push for a set time; 4096 records, 6000 pushes/s, "
           "drained every 16.7 ms\n\n| stall | pushes during it | refused |\n|---|---|---|\n");
    for (s = 0; s < sizeof(stalls_ms) / sizeof(stalls_ms[0]); s++) {
        uint32_t t, pushes = 0, refused = 0;
        uint64_t now, end, next_push, next_drain;
        yrt_event ev = make_event(3);
        unsigned char* slot;
        ring_open(4096);
        /* the claim half of yrt_ring_push(), then nothing */
        (void)bench_fadd(&g_ring.used);
        t = bench_fadd(&g_ring.head);
        slot = g_ring.slots + (size_t)(t & g_ring.mask) * 64u;
        now = yrt_now_ns();
        end = now + (uint64_t)stalls_ms[s] * 1000000ull;
        next_push = now;
        next_drain = now;
        while ((now = yrt_now_ns()) < end) {
            if (now >= next_push) {
                if (yrt_ring_push(&g_ring, &ev) == YRT_ERR_FULL) refused++;
                pushes++;
                next_push += 166667ull;
            }
            if (now >= next_drain) {
                (void)yrt_ring_drain(&g_ring, g_out, 4096);
                next_drain += 16666667ull;
            }
        }
        /* publish, as the stalled producer finally would */
        memcpy(slot + 4, (const unsigned char*)&ev + 4, 60);
        *(volatile uint32_t*)(void*)slot = t + 1u;
        drain_all();
        printf("| %d ms | %lu | %lu |\n", stalls_ms[s], (unsigned long)pushes,
               (unsigned long)refused);
    }
}

/* ------------------------------------------------------------------- B4 */

static void b4(void) {
    static const int caps[] = { 64, 1024, 4096 };
    size_t c;
    int rep, k;
    yrt_event e = make_event(5);
    header("B4. drain, 4096 records per repetition, 200 repetitions", "ns/rec");
    for (c = 0; c < sizeof(caps) / sizeof(caps[0]); c++) {
        char label[64];
        ring_open(4096);
        for (rep = 0; rep < 200; rep++) {
            uint64_t t0;
            for (k = 0; k < 4096; k++) (void)yrt_ring_push(&g_ring, &e);
            t0 = yrt_now_ns();
            while (yrt_ring_drain(&g_ring, g_out, caps[c]) > 0) { }
            g_blk[rep] = yrt_now_ns() - t0;
        }
        snprintf(label, sizeof(label), "batch of %d", caps[c]);
        row(label, g_blk, 200, 4096);
    }
}

/* ------------------------------------------------------------------- B5 */

static volatile uint64_t g_sink;


#define B5_LOOP(label, body) do { \
        size_t b_; int k_; \
        for (b_ = 0; b_ < nb; b_++) { \
            uint64_t t0_ = yrt_now_ns(); \
            for (k_ = 0; k_ < BLOCK; k_++) { body; } \
            g_blk[b_] = yrt_now_ns() - t0_; \
            if (attached) drain_all(); \
        } \
        row(label, g_blk, nb, BLOCK); \
    } while (0)

static void b5_pass(size_t n, int attached) {
    size_t nb = n / BLOCK;
    double fill = 0.5;
    (void)fill;
    ring_open(4096);
    yrt_trace_set_ring(attached ? &g_ring : NULL);
    B5_LOOP("empty loop", g_sink++);
    B5_LOOP("zone pair", { YRT_ZONE(z, "bench"); g_sink++; YRT_ZONE_END(z); });
    B5_LOOP("plot", { YRT_PLOT("bench", fill); g_sink++; });
    B5_LOOP("message, literal", { YRT_MESSAGE("bench message"); g_sink++; });
    B5_LOOP("message, formatted", { YRT_MESSAGEF("trial %d", k_); g_sink++; });
    B5_LOOP("frame mark", { YRT_FRAME_MARK(); g_sink++; });
    yrt_trace_set_ring(NULL);
}

static void b5(size_t n) {
    size_t nb = n / BLOCK;
    int attached = 0;
#if defined(YRT_TRACE_RING)
    header("B5. macros, YRT_TRACE_RING, trace ring attached", "ns");
    b5_pass(n, 1);
    header("B5. macros, YRT_TRACE_RING, no trace ring attached", "ns");
    b5_pass(n, 0);
#elif defined(YRT_TRACY)
    header("B5. macros, YRT_TRACY", "ns");
    b5_pass(n, 0);
    B5_LOOP("Tracy clock pairing: plot", ___tracy_emit_plot("yrt_ns", (double)yrt_now_ns()));
    B5_LOOP("Tracy clock pairing: message", {
        char m_[32];
        int l_ = snprintf(m_, sizeof(m_), "yrt %llu", (unsigned long long)yrt_now_ns());
        TracyCMessage(m_, (size_t)l_); });
#else
    header("B5. macros, default (nothing)", "ns");
    b5_pass(n, 0);
#endif
    header("B5. clock and thread id", "ns");
    B5_LOOP("yrt_now_ns", g_sink += yrt_now_ns());
    B5_LOOP("yrt_thread_id", g_sink += yrt_thread_id());
}

/* ------------------------------------------------------------------- B6 */

#if defined(_WIN32)
static uint64_t tick64(void) { return (uint64_t)GetTickCount64(); }
#else
static uint64_t mono_raw(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint64_t realtime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

/* yrt_corr_desc.read takes the other clock's raw count, unsigned. */
static uint64_t rt_raw(void) { return (uint64_t)yrt_now_ns(); }

static void b6_clock(const char* label, uint64_t (*rd)(void), bool edge, int calls) {
    static uint64_t w[1000], cost[1000];
    yrt_corr_desc d;
    yrt_corr c;
    int i, ok = 0;
    memset(&d, 0, sizeof(d));
    d.read = rd;
    d.edge = edge;
    for (i = 0; i < calls && i < 1000; i++) {
        uint64_t t0 = yrt_now_ns();
        if (yrt_correlate(&d, &c) <= 0) continue;
        cost[ok] = yrt_now_ns() - t0;
        w[ok] = c.width_ns;
        ok++;
    }
    if (!ok) { printf("| %-44s | failed |\n", label); return; }
    sort_u64(w, (size_t)ok);
    sort_u64(cost, (size_t)ok);
    printf("| %-44s | %6llu | %6llu | %6llu | %10.1f |\n", label, (unsigned long long)w[0],
           (unsigned long long)w[ok / 2], (unsigned long long)w[(size_t)((double)ok * 0.99)],
           (double)cost[ok / 2] / 1e3);
}

static void b6(void) {
    printf("\nB6. yrt_correlate, default tries\n\n"
           "| %-44s | min width ns | median | p99 | median call us |\n"
           "|---|---|---|---|---|\n", "clock");
    b6_clock("yrt_now_ns (itself), 100 calls", rt_raw, false, 100);
#if defined(_WIN32)
    b6_clock("QueryInterruptTimePrecise, 100 calls", yrt_interrupt_time_100ns, false, 100);
    b6_clock("GetTickCount64, edge mode, 10 calls", tick64, true, 10);
#else
    b6_clock("CLOCK_MONOTONIC_RAW, 100 calls", mono_raw, false, 100);
    b6_clock("CLOCK_REALTIME, 100 calls", realtime, false, 100);
#endif
}

/* ------------------------------------------------------------------ main */

int main(int argc, char** argv) {
    size_t pushes = (argc > 1) ? (size_t)strtoul(argv[1], NULL, 10) : 1000000u;
    int max_p = (argc > 2) ? atoi(argv[2]) : 8;
    int b3_ms = (argc > 3) ? atoi(argv[3]) : 2000;
    int all = (argc > 4) && strcmp(argv[4], "all") == 0;
    yrt_report rep;
    char line[256];
    size_t blk_n;

    if (pushes < (size_t)BLOCK * 16u) pushes = (size_t)BLOCK * 16u;
    if (max_p < 2) max_p = 2;
    if (max_p > MAX_P) max_p = MAX_P;
    if (b3_ms < 10) b3_ms = 10;
    blk_n = pushes / BLOCK + (size_t)MAX_P * 2u + (size_t)8 * ((size_t)b3_ms * 2000u / BLOCK + 1);
    if (blk_n < 4096) blk_n = 4096;
    g_blk = (uint64_t*)calloc(blk_n, sizeof(uint64_t));
    if (!g_blk) { fputs("rt_ring_bench: out of memory\n", stderr); return 1; }

    yrt_report_get(&rep, YRT_POLICY_NORMAL);
    yrt_describe(&rep, line, sizeof(line));
    printf("rt_ring_bench: ysp_rt %s, %d CPUs, macro mode %s\n%s\n", yrt_version(), ncpu(),
#if defined(YRT_TRACE_RING)
           "YRT_TRACE_RING",
#elif defined(YRT_TRACY)
           "YRT_TRACY",
#else
           "default (nothing)",
#endif
           line);

    b1(pushes);
    b2(pushes, max_p);
    b3(b3_ms);
    if (all) { b3b(b3_ms * 5); b3c(); }
    b4();
    b5(pushes);
    b6();
    free(g_blk);
    return 0;
}
#endif /* YRT_NO_THREADS */
