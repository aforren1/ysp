/* rt_jitter.c - measure what ysp/rt.h's waits are worth on THIS machine.
 *
 * Elevates the calling thread, then times yrt_sleep_until() against a
 * running absolute deadline for a few spin windows and prints the wake
 * latency distribution for each. Ends with a one-shot deadline worker, so the
 * worker path is exercised too. Nothing here needs hardware.
 *
 * The numbers are the point. ysp/rt.h states no latency figure of its own;
 * this program is where a figure for your rig comes from. Run it on the
 * experiment machine, in the state the experiment runs in (same power plan,
 * same background load), and record the ysp_rt: line with the numbers.
 *
 * One of the windows swept is YRT_DEFAULT_SPIN_NS, marked (default) in the
 * output. That default is a starting point chosen per platform, not a
 * measurement of your machine: compare its row with the others and pass your
 * own window to yrt_sleep_until() or yrt_worker_desc.spin_ns.
 *
 * Build (from the repository root):
 *     cc -O2 -pthread -Iinclude -o rt_jitter examples/rt/jitter.c   # Linux / macOS
 *     cl /O2 /Iinclude examples\rt\jitter.c                         # Windows (MSVC)
 *     emcc -O2 -pthread -sPROXY_TO_PTHREAD=1 -sEXIT_RUNTIME=1 -Iinclude \
 *          -o rt_jitter.js examples/rt/jitter.c && node rt_jitter.js   # wasm
 * or:  cmake -B build && cmake --build build
 *
 * Under Emscripten the policy is always NORMAL (there is no ladder; the
 * ysp_rt: line says platform=wasm), and without -pthread every wait is a
 * busy-wait and the worker section reports that it could not start. See
 * WEBASSEMBLY in ysp/rt.h.
 *
 * Usage: rt_jitter [iterations] [period_us]
 *     rt_jitter             # 200 waits of 2 ms per spin window
 *     rt_jitter 1000 5000   # 1000 waits of 5 ms per spin window
 *
 * Exit code: 0 always, including with no privileges and no real-time policy.
 * A refused policy is a result, not a failure; it shows up in the ysp_rt:
 * line and in the numbers.
 */
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The measurement array is static and fixed: the loop being measured must not
 * call the allocator, and a heap block would also put a page fault in the
 * middle of a sample. */
#define MAX_ITERS 20000
static uint64_t g_late_ns[MAX_ITERS];

/* The spin windows to compare, in nanoseconds: everything to the OS, a small
 * window, a wide one, and this platform's default last, since the default is
 * larger on Windows than on Linux and macOS. It duplicates one of the fixed
 * windows on some platforms, and the sweep skips the repeat. */
static const uint32_t g_spins[] = { 0u, 200000u, 1000000u, YRT_DEFAULT_SPIN_NS };
#define NSPINS ((int)(sizeof(g_spins) / sizeof(g_spins[0])))

/* Shell sort: in place, no allocation, and short enough to keep the whole
 * program dependency-free. qsort() on glibc allocates for large inputs, which
 * this file is trying to avoid on principle. */
static void sort_u64(uint64_t* v, int n) {
    for (int gap = n / 2; gap > 0; gap /= 2) {
        for (int i = gap; i < n; i++) {
            uint64_t tmp = v[i];
            int j = i;
            while (j >= gap && v[j - gap] > tmp) { v[j] = v[j - gap]; j -= gap; }
            v[j] = tmp;
        }
    }
}

/* Nearest-rank percentile on the sorted array; 0.0 gives the minimum. */
static double pct_us(const uint64_t* sorted, int n, double p) {
    int idx = (int)(p * (double)(n - 1) + 0.5);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return (double)sorted[idx] / 1000.0;
}

/* One sweep: `iters` waits, each one period_ns after the last, at a fixed
 * spin window. The deadline is absolute and running, so a late wake is
 * reported and then forgotten instead of pushing every later deadline out. */
static void measure(uint32_t spin_ns, int iters, uint64_t period_ns) {
    uint64_t t = yrt_now_ns();
    for (int i = 0; i < iters; i++) {
        t += period_ns;
        g_late_ns[i] = yrt_sleep_until(t, spin_ns);
    }
    sort_u64(g_late_ns, iters);
    printf("  spin %7.1f us %-9s | min %8.1f  median %8.1f  p99 %8.1f  max %8.1f\n",
           (double)spin_ns / 1000.0,
           spin_ns == YRT_DEFAULT_SPIN_NS ? "(default)" : "",
           pct_us(g_late_ns, iters, 0.0),
           pct_us(g_late_ns, iters, 0.50),
           pct_us(g_late_ns, iters, 0.99),
           pct_us(g_late_ns, iters, 1.0));
}

#ifndef YRT_NO_THREADS
/* The worker job records when it ran and tells the main thread. `done` is
 * written by the worker thread and read by the main thread after a wait on
 * the clock, which is enough here: the main thread only reads it, and the
 * worker is joined by yrt_worker_close() before the value is used for
 * anything else. */
typedef struct job_result {
    volatile int done;
    int64_t      late_ns;
    int          flushed;
} job_result;

static void on_deadline(void* ctx, const yrt_job_info* info) {
    job_result* r = (job_result*)ctx;
    r->late_ns = info->late_ns;
    r->flushed = info->flushed ? 1 : 0;
    r->done = 1;
}
#endif

int main(int argc, char** argv) {
    int iters = (argc > 1) ? (int)strtol(argv[1], NULL, 0) : 200;
    uint64_t period_ns = (argc > 2)
        ? (uint64_t)strtoul(argv[2], NULL, 0) * 1000ull
        : 2000000ull;
    if (iters < 2) iters = 2;
    if (iters > MAX_ITERS) iters = MAX_ITERS;
    if (period_ns < 100000ull) period_ns = 100000ull;

    /* Ask for everything, in the order the manual gives: the process-wide
     * settings first, then the thread. Each one reports what it got. */
    bool locked = yrt_process_lock_memory() == 0;
    bool timeres = yrt_timer_resolution_begin() == 0;
    yrt_policy pol = yrt_thread_elevate(NULL);

    yrt_report rep;
    char line[256];
    yrt_report_get(&rep, pol);
    yrt_describe(&rep, line, sizeof(line));
    puts(line);
    printf("(mlock %s, timer resolution %s on this platform)\n",
           locked ? "granted" : "unavailable or refused",
           timeres ? "raised" : "unavailable or refused");

    printf("\nyrt_sleep_until wake latency, %d waits of %.3f ms each:\n",
           iters, (double)period_ns / 1000000.0);
    for (int i = 0; i < NSPINS; i++) {
        int seen = 0;
        for (int j = 0; j < i; j++) if (g_spins[j] == g_spins[i]) seen = 1;
        if (!seen) measure(g_spins[i], iters, period_ns);
    }

#ifndef YRT_NO_THREADS
    yrt_worker w;
    memset(&w, 0, sizeof(w));
    yrt_worker_desc wd;
    memset(&wd, 0, sizeof(wd));
    wd.spin_ns = 0; /* 0 asks for YRT_DEFAULT_SPIN_NS, the row tagged
                     * (default) above, so the worker is measured in the same
                     * configuration a zeroed desc gives an experiment. */
    if (!yrt_worker_open(&w, &wd)) {
        fprintf(stderr, "\nworker start failed: %s\n", yrt_worker_error(&w));
    } else {
        job_result r;
        memset(&r, 0, sizeof(r));
        printf("\nworker policy: %s\n", yrt_policy_name(yrt_worker_policy(&w)));
        /* A positive return is the job's seq; only a negative one is an error. */
        int rc = yrt_worker_submit(&w, yrt_now_ns() + 5000000ull, on_deadline, &r);
        if (rc < 0) {
            fprintf(stderr, "submit: %s\n", yrt_strerror(rc));
        } else {
            /* Wait past the deadline on this thread, then stop the worker,
             * which joins it: after that the job has either run or been
             * flushed, and r is settled. */
            yrt_sleep_ns(20000000ull);
            yrt_worker_close(&w);
            if (r.done)
                printf("  job ran %.1f us %s its 5 ms deadline%s\n",
                       (double)(r.late_ns < 0 ? -r.late_ns : r.late_ns) / 1000.0,
                       r.late_ns < 0 ? "before" : "after",
                       r.flushed ? " (flushed by stop)" : "");
            else
                printf("  job did not run\n");
        }
        if (yrt_worker_is_running(&w)) yrt_worker_close(&w);
    }
#endif

    yrt_timer_resolution_end();
    yrt_thread_cleanup();
    return 0;
}
