/* timeline_bench.c - time ytl_evaluate() and the loads on this machine.
 *
 * ysp/timeline.h's manual states costs as operation counts: a binary search
 * per base that is not stopped, a copy and a report insertion per fired
 * event, a key lookup per keyed channel that is O(1) while time moves
 * forward. What those cost depends on the machine and the compiler, so this
 * program measures them against the 16 ms frame budget.
 *
 * Four workloads, each on a simulated grid at the refresh rate given on
 * the command line (60 Hz by default; 500 Hz panels are on sale, and the
 * budget there is 2 ms):
 *   trial   the USAGE example's trial (5 events, 1 keyed channel), 1000
 *           trials of 1.33 s, re-anchored at 0 each time (a rewind each)
 *   movie   a two-hour movie base: 10000 annotations, 32 keyed channels of
 *           256 keys each, 8 SET-driven channels with 128 events each;
 *           two hours of frames (432000 at 60 Hz, 3.6 million at 500 Hz)
 *   rate    the movie at a base rate of 1001/1000 (v0.4.0 and later), so
 *           every conversion between RT and base time on that base takes
 *           the scaled path: the cost of a rate against `movie`
 *   tracks  ten minutes of a sampled CUBIC path at 120 samples/s, the
 *           same length as 600001 keys, a repeating flicker and four
 *           BEZIER keyed channels
 *   script RT-base waits: every frame adds one MARK up to 2 s ahead, as a
 *           script's waits would, and prunes every 60 frames; 100000 frames
 * and the loads: 10000 sorted events by one add_n, the same one add at a
 * time, the same shuffled one at a time (the quadratic case the manual
 * names), and the prune and clear of all of them.
 *
 * Per-frame times are ysp/rt.h clock reads around each evaluate, so each
 * includes one clock read's overhead, printed first. The "floor" line is
 * the movie's frame count with nothing between the clock reads: its max
 * is what the machine and the OS cost a frame with no header code, the
 * yardstick for every other max. On Windows the
 * clock ticks every 100 ns, so the quantiles there are multiples of 100 ns
 * and only the mean resolves below that.
 *
 * Build (from the repository root):
 *     cc -O2 -Iinclude -o timeline_bench examples/timeline/bench.c -lm
 *     cl /O2 /Iinclude examples\timeline\bench.c
 *
 * Usage: timeline_bench [refresh_hz]     (default 60, 1 to 2000)
 * Exit code: 0, or 1 if a call failed or the argument is bad.
 */
#define YRT_NO_THREADS
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"

#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S YRT_S(1)
#define N_ANNOT 10000
#define N_KEYED 32
#define N_KEYS 256
#define N_SET_CH 8
#define N_SET_EV 128
#define MAX_HZ 2000
#define HIST_NS 100000
#define SCRIPT_FRAMES 100000
#define CAPACITY 20000

static ytl_event storage[CAPACITY];
static ytl_event batch[CAPACITY];
static ytl_key keys[N_KEYED][N_KEYS];
/* A histogram at 1 ns, not one sample per frame: two hours at 2000 Hz is
 * 14 million frames. Times of HIST_NS and above count only in the max. */
static uint32_t g_hist[HIST_NS];
static uint32_t g_max;
static int64_t g_n;
static double g_sum;
static int64_t g_hz = 60;
static ytl_timeline tl;

static uint64_t g_rng = 20261004u;
static uint64_t next_u64(void) {
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static int64_t rand_below(int64_t n) { return (int64_t)(next_u64() % (uint64_t)n); }

/* Frame k at round(k * 1e9 / hz) ns. */
static int64_t grid(int64_t k) { return (k * S * 2 + g_hz) / (2 * g_hz); }

static void record(int dt) {
    uint32_t u = (uint32_t)dt;
    if (u < HIST_NS) g_hist[u]++;
    if (u > g_max) g_max = u;
    g_sum += (double)u;
    g_n++;
}

/* The smallest time with at least q of the frames at or below it; -1 when
 * it is past the histogram. */
static long quantile(double q) {
    double want = q * (double)g_n, acc = 0.0;
    long i;
    for (i = 0; i < HIST_NS; i++) {
        acc += g_hist[i];
        if (acc >= want) return i;
    }
    return -1;
}

static void report(const char* name, int64_t fired) {
    double budget = 1e9 / (double)g_hz;
    long p50 = quantile(0.5), p99 = quantile(0.99), p9999 = quantile(0.9999);
    printf("%-7s %8lld frames  mean %5.0f ns  p50 %5ld  p99 %5ld  p99.99 %6ld  max %7u ns"
           "  (p99 %.3f%%, max %.1f%% of a frame)  fired %lld\n",
           name, (long long)g_n, g_sum / (double)g_n, p50, p99, p9999, g_max,
           100.0 * (double)p99 / budget, 100.0 * (double)g_max / budget, (long long)fired);
    memset(g_hist, 0, sizeof(g_hist));
    g_max = 0;
    g_n = 0;
    g_sum = 0.0;
}

static bool open_tl(int n_channels) {
    ytl_desc d;
    memset(&d, 0, sizeof(d));
    d.events = storage;
    d.event_capacity = CAPACITY;
    d.n_channels = n_channels;
    if (!ytl_open(&tl, &d)) {
        fprintf(stderr, "%s\n", ytl_error(&tl));
        return false;
    }
    return true;
}

static ytl_event ev(int64_t t, int base, int kind, int target) {
    ytl_event e;
    memset(&e, 0, sizeof(e));
    e.time_ns = t;
    e.base = (uint8_t)base;
    e.kind = (uint8_t)kind;
    e.target = target;
    return e;
}

static int frame(int64_t k, int64_t* fired_total) {
    ytl_frame f;
    ytl_event fired[16];
    uint64_t t0, t1;
    int n;
    f.onset = grid(k);
    f.period = grid(k + 1) - grid(k);
    f.index = k;
    t0 = yrt_now_ns();
    n = ytl_evaluate(&tl, &f, fired, 16);
    t1 = yrt_now_ns();
    if (n < 0) return -1;
    *fired_total += n;
    return (int)(t1 - t0);
}

/* The same number of frames as the movie with nothing between the two
 * clock reads: the slow frames of this machine and OS without the header.
 * A workload's max far above the floor's max would be the header's. */
static void bench_floor(void) {
    int64_t i, n = 2 * 3600 * g_hz;
    for (i = 0; i < n; i++) {
        uint64_t t0 = yrt_now_ns();
        uint64_t t1 = yrt_now_ns();
        record((int)(t1 - t0));
    }
    report("floor", 0);
}

static int bench_trial(void) {
    static const ytl_key ramp[] = {
        { S / 2,          0.0f, YTL_EASE_COSINE, 0, 0 },
        { S / 2 + S / 10, 0.5f, YTL_EASE_LINEAR, 0, 0 },
        { 11 * S / 10,    0.5f, YTL_EASE_COSINE, 0, 0 },
        { 6 * S / 5,      0.0f, YTL_EASE_LINEAR, 0, 0 },
    };
    ytl_event e[5];
    int64_t k = 0, fired = 0;
    int trial, i, per_trial = (int)(g_hz * 4 / 3);   /* 1.33 s */
    if (!open_tl(3)) return 1;
    e[0] = ev(0, 1, YTL_ONSET, 0);
    e[1] = ev(S / 2, 1, YTL_OFFSET, 0);
    e[2] = ev(S / 2, 1, YTL_ONSET, 1);
    e[3] = ev(S / 2, 1, YTL_TRIGGER, 0);
    e[4] = ev(6 * S / 5, 1, YTL_OFFSET, 1);
    if (ytl_add_n(&tl, e, 5) < 0 || ytl_set_keys(&tl, 2, 1, ramp, 4) < 0) return 1;
    for (trial = 0; trial < 1000; trial++) {
        ytl_anchor(&tl, 1, grid(k), 0);
        for (i = 0; i < per_trial; i++) {
            int dt = frame(k++, &fired);
            if (dt < 0) return 1;
            record(dt);
        }
    }
    report("trial", fired);
    return 0;
}

static int bench_movie(const char* name, int32_t num, int32_t den) {
    const int64_t len = 2 * 3600 * S;
    int64_t fired = 0;
    int i, j, movie_frames = (int)(2 * 3600 * g_hz);
    if (!open_tl(N_KEYED + N_SET_CH)) return 1;
    for (i = 0; i < N_ANNOT; i++) batch[i] = ev(rand_below(len), 2, YTL_MARK, 0);
    for (j = 0; j < N_SET_CH; j++) {
        for (i = 0; i < N_SET_EV; i++) {
            ytl_event* e = &batch[N_ANNOT + j * N_SET_EV + i];
            *e = ev(rand_below(len), 2, YTL_SET, N_KEYED + j);
            e->value = (float)i;
        }
    }
    if (ytl_add_n(&tl, batch, N_ANNOT + N_SET_CH * N_SET_EV) < 0) return 1;
    for (j = 0; j < N_KEYED; j++) {
        for (i = 0; i < N_KEYS; i++) {
            keys[j][i].time_ns = len * i / (N_KEYS - 1);
            keys[j][i].value = (float)(i % 7) + 1.0f;
            keys[j][i].ease = (uint8_t)(i % 6);
        }
        if (ytl_set_keys(&tl, j, 2, keys[j], N_KEYS) < 0) return 1;
    }
    ytl_anchor(&tl, 2, grid(0), 0);
#if YTL_VERSION_MAJOR > 0 || YTL_VERSION_MINOR >= 4
    if (ytl_rate(&tl, 2, grid(0), num, den) < 0) return 1;
#else
    if (num != den) return 0;
#endif
    for (i = 0; i < movie_frames; i++) {
        int dt = frame(i, &fired);
        if (dt < 0) return 1;
        record(dt);
    }
    report(name, fired);
    return 0;
}

/* Ten minutes of the four track kinds side by side: a sampled CUBIC path
 * at 120 samples/s, the same path as 600001 keys (1 kHz), a repeating
 * square-wave flicker, and four BEZIER keyed channels. */
#define TR_RATE 120
#define TR_SAMPLES (600 * TR_RATE + 1)
#define TR_KEYS (600 * 1000 + 1)
static float tr_samples[TR_SAMPLES];
static ytl_key tr_keys[TR_KEYS];

static int bench_tracks(void) {
    static const ytl_key square[] = {
        { 0,          1.0f, YTL_EASE_STEP, 0, 0 },
        { 2 * S / 30, 0.0f, YTL_EASE_STEP, 0, 0 },
    };
    static const ytl_curve ease_io = { 0.42f, 0.0f, 0.58f, 1.0f };
    static ytl_key bez[4][64];
    ytl_track tr;
    int64_t fired = 0, n = 600 * g_hz;
    int i, j;
    if (!open_tl(7)) return 1;
    for (i = 0; i < TR_SAMPLES; i++) tr_samples[i] = (float)(i % 97) * 0.25f;
    for (i = 0; i < TR_KEYS; i++) {
        tr_keys[i].time_ns = (int64_t)i * YRT_MS(1);
        tr_keys[i].value = (float)(i % 89);
    }
    memset(&tr, 0, sizeof(tr));
    tr.samples = tr_samples;
    tr.n_samples = TR_SAMPLES;
    tr.rate = TR_RATE;
    tr.interp = YTL_INTERP_CUBIC;
    if (ytl_set_track(&tl, 0, 1, &tr) < 0) return 1;
    if (ytl_set_keys(&tl, 1, 1, tr_keys, TR_KEYS) < 0) return 1;
    memset(&tr, 0, sizeof(tr));
    tr.keys = square;
    tr.n_keys = 2;
    tr.period_ns = 4 * S / 30;
    if (ytl_set_track(&tl, 2, 1, &tr) < 0) return 1;
    for (j = 0; j < 4; j++) {
        for (i = 0; i < 64; i++) {
            bez[j][i].time_ns = (int64_t)i * 600 * S / 63;
            bez[j][i].value = (float)((i + j) % 5);
            bez[j][i].ease = YTL_EASE_BEZIER;
        }
        memset(&tr, 0, sizeof(tr));
        tr.keys = bez[j];
        tr.n_keys = 64;
        tr.curves = &ease_io;
        tr.n_curves = 1;
        if (ytl_set_track(&tl, 3 + j, 1, &tr) < 0) return 1;
    }
    ytl_anchor(&tl, 1, grid(0), 0);
    for (i = 0; i < n; i++) {
        int dt = frame(i, &fired);
        if (dt < 0) return 1;
        record(dt);
    }
    report("tracks", fired);
    return 0;
}

static int bench_script(void) {
    int64_t fired = 0;
    int i, max_live = 0;
    if (!open_tl(0)) return 1;
    for (i = 0; i < SCRIPT_FRAMES; i++) {
        ytl_event e = ev(grid(i) + rand_below(2 * S), 0, YTL_MARK, 0);
        int dt, live;
        if (ytl_add(&tl, &e) < 0) return 1;
        dt = frame(i, &fired);
        if (dt < 0) return 1;
        record(dt);
        if (i % 60 == 59) ytl_prune(&tl, YTL_BASE_RT);
        ytl_events(&tl, YTL_BASE_RT, &live);
        if (live > max_live) max_live = live;
    }
    report("script", fired);
    printf("        at most %d events stored at once\n", max_live);
    return 0;
}

static int bench_loads(void) {
    uint64_t t0;
    int i, removed;
    for (i = 0; i < N_ANNOT; i++) batch[i] = ev((int64_t)i * S / 10, 2, YTL_MARK, 0);

    if (!open_tl(0)) return 1;
    t0 = yrt_now_ns();
    if (ytl_add_n(&tl, batch, N_ANNOT) < 0) return 1;
    printf("load    %d sorted, one add_n:        %9.3f ms\n", N_ANNOT,
           (double)(yrt_now_ns() - t0) / 1e6);

    if (!open_tl(0)) return 1;
    t0 = yrt_now_ns();
    for (i = 0; i < N_ANNOT; i++) if (ytl_add(&tl, &batch[i]) < 0) return 1;
    printf("load    %d sorted, one add each:     %9.3f ms\n", N_ANNOT,
           (double)(yrt_now_ns() - t0) / 1e6);

    for (i = N_ANNOT - 1; i > 0; i--) {
        int j = (int)rand_below(i + 1);
        ytl_event tmp = batch[i];
        batch[i] = batch[j];
        batch[j] = tmp;
    }
    if (!open_tl(0)) return 1;
    t0 = yrt_now_ns();
    for (i = 0; i < N_ANNOT; i++) if (ytl_add(&tl, &batch[i]) < 0) return 1;
    printf("load    %d shuffled, one add each:   %9.3f ms\n", N_ANNOT,
           (double)(yrt_now_ns() - t0) / 1e6);

    ytl_anchor(&tl, 2, 0, 0);
    {
        ytl_frame f;
        f.onset = N_ANNOT / 2 * S / 10;
        f.period = 0;
        f.index = 0;
        ytl_evaluate(&tl, &f, NULL, 0);
    }
    t0 = yrt_now_ns();
    removed = ytl_prune(&tl, YTL_ALL_BASES);
    printf("prune   %d fired of %d:              %9.3f ms\n", removed, N_ANNOT,
           (double)(yrt_now_ns() - t0) / 1e6);
    t0 = yrt_now_ns();
    removed = ytl_clear(&tl, 2);
    printf("clear   %d:                          %9.3f ms\n", removed,
           (double)(yrt_now_ns() - t0) / 1e6);
    return 0;
}

int main(int argc, char** argv) {
    uint64_t t0, t1, best = (uint64_t)-1;
    int i;
    if (argc > 1) {
        g_hz = atoi(argv[1]);
        if (g_hz < 1 || g_hz > MAX_HZ) {
            fprintf(stderr, "usage: timeline_bench [refresh_hz 1..%d]\n", MAX_HZ);
            return 1;
        }
    }
    for (i = 0; i < 1000; i++) {
        t0 = yrt_now_ns();
        t1 = yrt_now_ns();
        if (t1 - t0 < best) best = t1 - t0;
    }
    printf("ysp_timeline %s; handle %u bytes; clock read %llu ns at best; %lld Hz, "
           "frame budget %.3f ms\n",
           ytl_version(), (unsigned)sizeof(ytl_timeline), (unsigned long long)best,
           (long long)g_hz, 1e3 / (double)g_hz);
    bench_floor();
    if (bench_trial() || bench_movie("movie", 1, 1) || bench_movie("rate", 1001, 1000) || bench_tracks() || bench_script() || bench_loads()) {
        fprintf(stderr, "a call failed\n");
        return 1;
    }
    return 0;
}
