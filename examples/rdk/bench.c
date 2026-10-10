/* rdk_bench.c - what ysp/rdk.h costs per frame on this machine, CPU only.
 *
 * For each algorithm, aperture and dot count, yrdk_update() is timed per
 * call over a run of updates at 60 Hz (mean, median, p99), then the
 * outputs: yrdk_xy() into packed pairs and yrdk_write() strided into
 * 32-byte records (ysp/gfx.h's ygfx_inst layout). The bars
 * (docs/rdk.md): 1000 dots at most 10 us mean; 10,000 dots at most
 * 100 us mean and 250 us p99 for every rule; a 10,000-dot write at most
 * 20 us. The table runs twice, round A then round B, so a slow spell of
 * the machine shows as a difference between the rounds. Also, for the
 * record, the two counter-based generators that were considered: Philox4x32-10
 * (the header's) and Widynski's squares32, per 32-bit word.
 *
 * Times are ysp/rt.h clock reads around each call. Take the measurement
 * lock and run it on a quiet machine; CI runs --quick to check it works.
 *
 * Build (from the repository root):
 *     cc -O2 -Iinclude -o rdk_bench examples/rdk/bench.c -lm
 *     cl /O2 /Iinclude examples\rdk\bench.c
 *
 * Usage: rdk_bench [--quick] [--big] [--only NAME]   (--big adds 100,000 dots)
 * Exit code: 0, 1 if a call failed, 2 for a bad argument.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YRT_NO_THREADS
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"

#define YSP_RDK_IMPLEMENTATION
#include "ysp/rdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P_NS 16666667LL
#define MAXU 2000

typedef struct rec32 { float x, y, ori, phase, contrast, scale, gate, color; } rec32;

static double tus[MAXU];
static float* xy;
static rec32* recs;
static int quick = 0, big = 0;
static const char* only = NULL;

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y);
}

typedef struct cfg { const char* name; yrdk_algorithm alg; yrdk_edge edge; yrdk_signal sig; yrdk_noise noise; float life; } cfg;

static const cfg cfgs[] = {
    { "WN", YRDK_WN, YRDK_WRAP, YRDK_SIGNAL_SAME, YRDK_NOISE_DIRECTION, 0 },
    { "MN (3 sets)", YRDK_MN, YRDK_WRAP, YRDK_SIGNAL_SAME, YRDK_NOISE_DIRECTION, 0 },
    { "LL (3 sets)", YRDK_LL, YRDK_WRAP, YRDK_SIGNAL_SAME, YRDK_NOISE_DIRECTION, 0 },
    { "BM", YRDK_BM, YRDK_WRAP, YRDK_SIGNAL_SAME, YRDK_NOISE_DIRECTION, 0 },
    { "SAME DIRECTION", YRDK_CUSTOM, YRDK_WRAP, YRDK_SIGNAL_SAME, YRDK_NOISE_DIRECTION, 0 },
    { "SAME DIRECTION life 0.2 s", YRDK_CUSTOM, YRDK_WRAP, YRDK_SIGNAL_SAME, YRDK_NOISE_DIRECTION, 0.2f },
    { "DIFFERENT WALK", YRDK_CUSTOM, YRDK_WRAP, YRDK_SIGNAL_DIFFERENT, YRDK_NOISE_WALK, 0 },
    { "SAME DIRECTION REPLOT", YRDK_CUSTOM, YRDK_REPLOT, YRDK_SIGNAL_SAME, YRDK_NOISE_DIRECTION, 0 },
};

static int row(const cfg* c, int rect, int n, char round) {
    static yrdk_field f;
    yrdk_desc d;
    int k, updates = n >= 100000 ? 200 : (quick ? 50 : MAXU);
    double mean = 0, wmean = 0, smean = 0;
    char name[96];
    snprintf(name, sizeof name, "%s, %s, %d", c->name, rect ? "rect" : "circle", n);
    if (only && !strstr(name, only)) return 0;
    memset(&d, 0, sizeof d);
    d.algorithm = c->alg;
    if (c->alg == YRDK_CUSTOM) { d.signal = c->sig; d.noise = c->noise; }
    d.edge = c->edge;
    d.aperture = rect ? YRDK_RECT : YRDK_CIRCLE;
    d.w = 10; d.h = rect ? 8.0f : 0.0f;
    d.count = n;
    d.coherence = 0.5f; d.direction = 30; d.speed = 5;   /* deg/s on a 10 deg field */
    d.lifetime_ns = YRT_S(c->life);
    if (!yrdk_open(&f, &d)) { fprintf(stderr, "rdk_bench: %s: %s\n", name, yrdk_error(&f)); return -1; }
    yrdk_start(&f, 1234, 0);
    for (k = 0; k < updates; k++) {
        uint64_t t0 = yrt_now_ns();
        if (yrdk_update(&f, (int64_t)(k + 1) * P_NS) < 0) return -1;
        tus[k] = (double)(yrt_now_ns() - t0) * 1e-3;
        mean += tus[k];
        {
            yrdk_out o;
            uint64_t t1 = yrt_now_ns(), t2;
            yrdk_xy(&f, xy);
            t2 = yrt_now_ns();
            memset(&o, 0, sizeof o);
            o.xy = &recs[0].x; o.xy_stride = sizeof recs[0];
            o.dir = &recs[0].ori; o.dir_stride = sizeof recs[0];
            yrdk_write(&f, &o);
            smean += (double)(t2 - t1) * 1e-3;
            wmean += (double)(yrt_now_ns() - t2) * 1e-3;
        }
    }
    mean /= updates; smean /= updates; wmean /= updates;
    qsort(tus, (size_t)updates, sizeof tus[0], cmp_d);
    printf("| %c | %-40s | %9.1f | %9.1f | %9.1f | %8.1f | %8.1f |\n", round, name, mean, tus[updates / 2],
           tus[(int)(0.99 * (updates - 1))], smean, wmean);
    fflush(stdout);
    yrdk_close(&f);
    return 0;
}

/* Widynski (2020), squares32, timed for the record only: its keys must be
 * drawn by his stated procedure, so a 64-bit seed is not a key as is; the
 * key here is an arbitrary odd word, which costs the same. */
static uint32_t squares32(uint64_t ctr, uint64_t key) {
    uint64_t x, y, z;
    y = x = ctr * key; z = y + key;
    x = x * x + y; x = (x >> 32) | (x << 32);
    x = x * x + z; x = (x >> 32) | (x << 32);
    x = x * x + y; x = (x >> 32) | (x << 32);
    return (uint32_t)((x * x + z) >> 32);
}

static void rng_rows(void) {
    volatile uint32_t sink = 0;
    uint32_t i, n = quick ? 100000u : 10000000u;
    uint64_t t0;
    double ph, sq;
    uint32_t out[4], ctr[4] = { 0, 7, 0, 0 };
    t0 = yrt_now_ns();
    for (i = 0; i < n; i++) { ctr[0] = i; yrdk_philox(0x1234567890abcdefull, ctr, out); sink ^= out[0] ^ out[3]; }
    ph = (double)(yrt_now_ns() - t0) / n;
    t0 = yrt_now_ns();
    for (i = 0; i < n; i++) sink ^= squares32(i, 0x548c9decbce65297ull);
    sq = (double)(yrt_now_ns() - t0) / n;
    printf("\nPhilox4x32-10: %.2f ns a call (4 words, %.2f ns a word); squares32: %.2f ns a word\n", ph, ph / 4, sq);
    (void)sink;
}

int main(int argc, char** argv) {
    static const int ns[4] = { 100, 1000, 10000, 100000 };
    int i, r, rect, ni, rc = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--quick")) quick = 1;
        else if (!strcmp(argv[i], "--big")) big = 1;
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else { fprintf(stderr, "usage: rdk_bench [--quick] [--big] [--only NAME]\n"); return 2; }
    }
    xy = (float*)malloc(sizeof(float) * 2 * 100000);
    recs = (rec32*)malloc(sizeof(rec32) * 100000);
    if (!xy || !recs) return 1;
    printf("ysp/rdk.h %s: yrdk_update() per call, us; then yrdk_xy() and a strided write, us per frame\n\n",
           yrdk_version());
    printf("| R | %-40s | %9s | %9s | %9s | %8s | %8s |\n", "workload (algorithm, aperture, dots)", "mean", "median", "p99",
           "xy", "strided");
    printf("|---|---|---|---|---|---|---|\n");
    for (r = 0; r < (quick ? 1 : 2); r++)
        for (ni = 0; ni < 4; ni++) {
            if (quick && ns[ni] > 1000) continue;
            if (!big && ns[ni] > 10000) continue;
            for (i = 0; i < (int)(sizeof cfgs / sizeof cfgs[0]); i++)
                for (rect = 0; rect < 2; rect++)
                    if (row(&cfgs[i], rect, ns[ni], (char)('A' + r)) < 0) rc = 1;
        }
    rng_rows();
    free(xy);
    free(recs);
    return rc;
}
