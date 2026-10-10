/* rdk_test.c - self-checking test for ysp/rdk.h. Returns 0 when every
 * check passed, 1 after printing each failure.
 *
 * Statistics, not pixels: the realized coherence (from the dots'
 * displacements, not the header's flags), density against uniform by
 * chi-square in equal-area cells and in the band along the edge, lifetimes,
 * displacement against speed x elapsed time, the noise rules' direction
 * and position distributions, frozen noise across coherence levels,
 * dropped frames under both clocks, interleaved sets, LEAST_RECENT's
 * rule, the edge invariant under stalls; and the exact pieces: Philox
 * against Random123's known answers, the angle conversion against exact
 * rationals, the sine tables against libm, golden digests that
 * tests/compare/rdk_ref.py (an independent Python model) reproduces, replay
 * and snapshots, every refusal, and no allocation after open.
 *
 * Every statistic uses fixed seeds, so a verdict is the same on every run;
 * the thresholds are the chi-square quantiles at p = 1e-7 (Wilson and
 * Hilferty), so the file's false alarm rate is under 1e-5 even if the
 * seeds were not fixed. YRDK_TEST_VERBOSE=1 prints every statistic.
 *
 *     gcc -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Werror -O2 -Iinclude \
 *         -o rdk_test tests/adapt/rdk_test.c -lm && ./rdk_test
 *     cl /nologo /W4 /WX /O2 /Iinclude tests\adapt\rdk_test.c
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Counts the header's allocations: open() makes one, nothing else may. */
static long g_allocs = 0;
static void* count_malloc(size_t n) { g_allocs++; return malloc(n); }
#define YRDK_MALLOC(n) count_malloc(n)
#define YRDK_FREE(p) free(p)
#define YSP_RDK_IMPLEMENTATION
#include "ysp/rdk.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------- harness */

static int g_failures = 0;
static int g_verbose = 0;
static const char* g_where = "";

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "rdk_test [%s]: FAIL line %d: %s\n", g_where, __LINE__, #cond); g_failures++; } } while (0)
#define CHECK_LE(v, lim) do { double v_ = (double)(v), l_ = (double)(lim); if (!(v_ <= l_)) { \
    fprintf(stderr, "rdk_test [%s]: FAIL line %d: %s = %.6g, limit %.6g\n", g_where, __LINE__, #v, v_, l_); \
    g_failures++; } else if (g_verbose) printf("  [%s] %s = %.6g (limit %.6g)\n", g_where, #v, v_, l_); } while (0)

#define P_NS 16666667LL
#define T0 1000000000LL

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static double chi2_crit(int df) {   /* p = 1e-7, Wilson and Hilferty */
    double z = 5.199, k = (double)df, a = 2.0 / (9.0 * k);
    double v = 1.0 - a + z * sqrt(a);
    return k * v * v * v;
}

static yrdk_desc base_desc(void) {
    yrdk_desc d;
    memset(&d, 0, sizeof d);
    return d;
}

/* v0.1's float seconds as v0.2's lifetime_ns, rounded as v0.1 did, so the
 * golden digests and tests/compare/rdk_ref.py (which keeps seconds) agree. */
static int64_t life_s(float s) { return (int64_t)llround((double)s * 1e9); }

static int open_ok(yrdk_field* f, const yrdk_desc* d) {
    int rc = yrdk_open(f, d) ? YRDK_OK : YRDK_ERR_ARG;
    if (rc < 0) { fprintf(stderr, "rdk_test [%s]: open: %s\n", g_where, yrdk_error(f)); g_failures++; }
    return rc;
}

/* Inside the aperture on the lattice, exactly (the header's own rule). */
static int inside_all(const yrdk_field* f) {
    int i;
    for (i = 0; i < f->total; i++) {
        int64_t x = f->xy_[2 * i], y = f->xy_[2 * i + 1];
        if (f->aperture_ == YRDK_RECT) {
            if (x < -f->hx_ || x >= f->hx_ || y < -f->hy_ || y >= f->hy_) return 0;
        } else if (x * x + y * y > ((int64_t)1 << 60)) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------- 1. exact pieces */

static void test_exact(void) {
    static const uint32_t kat[3][10] = {   /* Random123 kat_vectors, philox4x32 10 */
        { 0, 0, 0, 0, 0, 0, 0x6627e8d5u, 0xe169c58du, 0xbc57ac4cu, 0x9b00dbd8u },
        { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
          0x408f276du, 0x41c83b0eu, 0xa20bc7c6u, 0x6d5451fdu },
        { 0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u, 0xa4093822u, 0x299f31d0u,
          0xd16cfe09u, 0x94fdccebu, 0x5001e420u, 0x24126ea1u },
    };
    int i, k;
    double worst = 0, worst_norm = 0;
    g_where = "exact";
    for (i = 0; i < 3; i++) {
        uint32_t out[4];
        uint64_t seed = (uint64_t)kat[i][4] | ((uint64_t)kat[i][5] << 32);
        yrdk_philox(seed, kat[i], out);
        for (k = 0; k < 4; k++) CHECK(out[k] == kat[i][6 + k]);
    }
    /* angle(): exact cases, then against a long double reference where the
     * rounding is not near a tie. */
    CHECK(yrdk_angle(0) == 0);
    CHECK(yrdk_angle(90) == 0x40000000u);
    CHECK(yrdk_angle(180) == 0x80000000u);
    CHECK(yrdk_angle(-90) == 0xC0000000u);
    CHECK(yrdk_angle(360) == 0);
    CHECK(yrdk_angle(720) == 0);
    CHECK(yrdk_angle(-360) == 0);
    CHECK(yrdk_angle(45) == 0x20000000u);
    CHECK(yrdk_angle(1e30f) == yrdk_angle(fmodf(1e30f, 360.0f)));   /* fmodf is exact */
    CHECK(yrdk_angle(-1e-30f) == 0);
    CHECK(yrdk_angle(NAN) == 0);
    CHECK(yrdk_angle(INFINITY) == 0);
    /* 1 degree = 2^32 / 360 = 11930464.711 -> 11930465 */
    CHECK(yrdk_angle(1) == 11930465u);
    CHECK(yrdk_angle(-1) == (uint32_t)(0u - 11930465u));
    /* 45 2^-30 deg is exactly half a step: rnd rounds the tie up */
    CHECK(yrdk_angle(ldexpf(45.0f, -30)) == 1u);
    CHECK(yrdk_angle(-ldexpf(45.0f, -30)) == 0u);
    /* and a tie on the other path (exponent >= -33): 46603 x 45 2^-30 deg is
     * 23301.5 steps */
    CHECK(yrdk_angle(ldexpf(2097135.0f, -30)) == 23302u);
    CHECK(yrdk_angle(-ldexpf(2097135.0f, -30)) == (uint32_t)(0u - 23301u));
    {
        uint32_t g = 7u;
        long double unit = 4294967296.0L / 360.0L;
        int bad = 0;
        for (i = 0; i < 200000; i++) {
            float deg;
            long double v, fr;
            g = g * 1664525u + 1013904223u;
            deg = (float)((double)(g >> 8) / 16777216.0 * 1440.0 - 720.0);
            v = (long double)deg * unit;
            fr = v - floorl(v);
            if (fabsl(fr - 0.5L) < 1e-6L) continue;   /* a near tie: long double may not decide it */
            {
                long double r = floorl(v + 0.5L);
                long double m = fmodl(r, 4294967296.0L);
                if (m < 0) m += 4294967296.0L;
                if ((uint32_t)m != yrdk_angle(deg)) bad++;
            }
        }
        CHECK(bad == 0);
    }
    /* sincos against libm in double; exact at the quarter turns. */
    for (i = 0; i < (1 << 20); i++) {
        uint32_t a = (uint32_t)i * 4096u + (uint32_t)(i * 2654435761u >> 20);
        int32_t s, c;
        double th = (double)a * (2.0 * M_PI / 4294967296.0);
        double es, ec, nn;
        yrdk_sincos(a, &s, &c);
        es = fabs((double)s / 1073741824.0 - sin(th));
        ec = fabs((double)c / 1073741824.0 - cos(th));
        if (es > worst) worst = es;
        if (ec > worst) worst = ec;
        nn = fabs(hypot((double)s, (double)c) / 1073741824.0 - 1.0);
        if (nn > worst_norm) worst_norm = nn;
    }
    CHECK_LE(worst, 4e-9);
    CHECK_LE(worst_norm, 4e-9);
    {
        int32_t s, c;
        yrdk_sincos(0, &s, &c); CHECK(s == 0 && c == (1 << 30));
        yrdk_sincos(0x40000000u, &s, &c); CHECK(s == (1 << 30) && c == 0);
        yrdk_sincos(0x80000000u, &s, &c); CHECK(s == 0 && c == -(1 << 30));
        yrdk_sincos(0xC0000000u, &s, &c); CHECK(s == -(1 << 30) && c == 0);
    }
    /* The literal tables against libm (a copy error check, not the reference). */
    for (i = 0; i <= 256; i++) CHECK(fabs(yrdk__sin_coarse[i] - sin(i * M_PI / 512) * 1073741824.0) <= 0.5 + 1e-6);
    for (i = 0; i < 256; i++) {
        CHECK(fabs(yrdk__sin_fine[i] - sin(i * M_PI / 131072) * 1073741824.0) <= 0.5 + 1e-6);
        CHECK(fabs(yrdk__cos_fine[i] - cos(i * M_PI / 131072) * 1073741824.0) <= 0.5 + 1e-6);
    }
    /* The step D against double: speed dt 2^30 / (Rmax 1e9). */
    {
        static yrdk_field f;
        yrdk_desc d = base_desc();
        double worst_d = 0;
        d.w = 10; d.count = 1;
        if (open_ok(&f, &d) == 0) {
            for (i = 0; i < 1000; i++) {
                float v = (float)(0.001 * i * i);
                int64_t dt = (int64_t)i * 1234567;
                double ref = (double)v * (double)dt * 1073741824.0 / (5.0 * 1e9);
                double got = (double)yrdk__step(&f, v, dt);
                if (fabs(got - ref) > worst_d) worst_d = fabs(got - ref);
            }
            CHECK_LE(worst_d, 0.5 + 1e-3);
            CHECK(yrdk__step(&f, 1e30f, (int64_t)1 << 62) == (int64_t)1 << 62);
            CHECK(yrdk__step(&f, 1e-30f, 1) == 0);
            CHECK(yrdk__step(&f, -2.0f, 1000000000) == yrdk__step(&f, 2.0f, 1000000000));
            yrdk_close(&f);
        }
    }
    /* The 128 / 64 division against the bit-by-bit reference. */
    {
        uint64_t g = 0x9E3779B97F4A7C15ull;
        int bad = 0;
        for (i = 0; i < 200000; i++) {
            yrdk__u128 n, d, q1, q2;
            g ^= g << 13; g ^= g >> 7; g ^= g << 17; n.hi = g >> (g & 63);
            g ^= g << 13; g ^= g >> 7; g ^= g << 17; n.lo = g;
            g ^= g << 13; g ^= g >> 7; g ^= g << 17; d.lo = (g >> (g & 63)) | 1u;
            g ^= g << 13; g ^= g >> 7; g ^= g << 17; d.hi = (i % 7 == 0) ? (g >> 40) : 0;
            q1 = yrdk__div128(n, d);
            q2 = yrdk__div128_slow(n, d);
            if (q1.hi != q2.hi || q1.lo != q2.lo) bad++;
        }
        CHECK(bad == 0);
    }
    /* rnd2 of big products: the 128-bit path equals the int64 path where both apply */
    {
        int64_t as[6] = { ((int64_t)1 << 32) + 12345, -((int64_t)1 << 32) - 777, ((int64_t)1 << 40) + 3, -((int64_t)1 << 50) - 1,
                          ((int64_t)1 << 32), -((int64_t)1 << 32) };
        int32_t cs[5] = { 1 << 30, -(1 << 30), 12345678, -98765, 0 };
        int j;
        for (i = 0; i < 6; i++)
            for (j = 0; j < 5; j++) {
                /* reference in long double where exact (< 2^64 mantissa) */
                long double p = (long double)as[i] * (long double)cs[j];
                long double r = floorl((p + 536870912.0L) / 1073741824.0L);
                CHECK((long double)yrdk__mulq30(as[i], cs[j]) == r);
            }
    }
}

/* ------------------------------------------------------ 2. golden digests */

#define VARY_C 1
#define VARY_DIR 2
#define VARY_SPEED 4

typedef struct golden {
    const char* name;
    yrdk_desc d;
    uint64_t seed;
    int drop, stall, vary, updates;
    uint64_t digest;
} golden;

static int64_t sched(int k, int drop, int stall) {
    int64_t t = T0 + (int64_t)k * P_NS + (int64_t)(((k * 7919) % 101) - 50) * 1000;
    if (drop >= 0 && k >= drop) t += P_NS;
    if (stall >= 0 && k >= stall) t += 37000000000LL;
    return t;
}

static void vary_params(yrdk_field* f, const yrdk_desc* d, int k, int vary) {
    f->coherence = d->coherence; f->direction = d->direction; f->speed = d->speed;
    if (vary & VARY_C) f->coherence = (float)(k % 17) / 16.0f;
    if (vary & VARY_DIR) f->direction = (float)((k % 8) * 45) + 0.5f;
    if ((vary & VARY_SPEED) && k >= 100 && k < 140) f->speed = -d->speed;
}

static void test_golden(void) {
    golden g[8];
    int i, k;
    g_where = "golden";
    memset(g, 0, sizeof g);
    for (i = 0; i < 8; i++) { g[i].drop = -1; g[i].stall = -1; }
    /* The table of tests/compare/rdk_ref.py's scenarios(), field for field. */
    g[0].name = "same_dir_circle"; g[0].d.w = 10; g[0].d.count = 200; g[0].d.coherence = 0.3125f; g[0].d.direction = 30;
    g[0].d.speed = 7.5f; g[0].seed = 1; g[0].drop = 50; g[0].updates = 240;
    g[1].name = "wn"; g[1].d.algorithm = YRDK_WN; g[1].d.w = 8; g[1].d.density = 2.5f; g[1].d.coherence = 0.25f;
    g[1].d.direction = 200; g[1].d.speed = 6; g[1].seed = 2; g[1].updates = 240;
    g[2].name = "mn_frames"; g[2].d.algorithm = YRDK_MN; g[2].d.w = 12; g[2].d.count = 100; g[2].d.coherence = 0.5f;
    g[2].d.direction = 45; g[2].d.speed = 10; g[2].d.clock = YRDK_CLOCK_FRAMES; g[2].d.frame_ns = P_NS; g[2].d.lifetime_frames = 6; g[2].seed = 3;
    g[2].drop = 70; g[2].updates = 300;
    g[3].name = "ll"; g[3].d.algorithm = YRDK_LL; g[3].d.w = 9; g[3].d.count = 90; g[3].d.coherence = 0.6875f;
    g[3].d.direction = -30; g[3].d.speed = 4; g[3].seed = 4; g[3].updates = 300;
    g[4].name = "bm_rect"; g[4].d.algorithm = YRDK_BM; g[4].d.aperture = YRDK_RECT; g[4].d.w = 12; g[4].d.h = 5;
    g[4].d.count = 150; g[4].d.coherence = 0.4375f; g[4].d.direction = 100; g[4].d.speed = 9; g[4].d.stream = 7;
    g[4].seed = 5; g[4].stall = 120; g[4].updates = 240;
    g[5].name = "replot_life"; g[5].d.signal = YRDK_SIGNAL_DIFFERENT; g[5].d.noise = YRDK_NOISE_POSITION;
    g[5].d.edge = YRDK_REPLOT; g[5].d.w = 10; g[5].d.count = 120; g[5].d.lifetime_ns = life_s(0.25f); g[5].d.coherence = 0.5f;
    g[5].d.speed = 12; g[5].seed = 6; g[5].vary = VARY_C; g[5].updates = 240;
    g[6].name = "walk_rect_life"; g[6].d.select = YRDK_BERNOULLI; g[6].d.noise = YRDK_NOISE_WALK;
    g[6].d.aperture = YRDK_RECT; g[6].d.w = 7; g[6].d.h = 11; g[6].d.edge = YRDK_REPLOT; g[6].d.lifetime_frames = 5;
    g[6].d.frame_ns = P_NS; g[6].d.count = 80; g[6].d.coherence = 0.375f; g[6].d.speed = 5; g[6].seed = 7;
    g[6].vary = VARY_DIR | VARY_SPEED; g[6].updates = 300;
    g[7].name = "stall_circle"; g[7].d.signal = YRDK_SIGNAL_DIFFERENT; g[7].d.w = 6; g[7].d.count = 64;
    g[7].d.lifetime_ns = life_s(1.3f); g[7].d.coherence = 0.5f; g[7].d.direction = 77.25f; g[7].d.speed = 3.25f;
    g[7].seed = 0xDEADBEEFCAFEF00Dull; g[7].drop = 40; g[7].stall = 60; g[7].updates = 200;
    /* From tests/compare/rdk_ref.py (uv run --with mpmath python tests/compare/rdk_ref.py --c). */
    g[0].digest = 0x46c7add13bea5555ull;
    g[1].digest = 0xab0c3ce3ad1b3c70ull;
    g[2].digest = 0x890de1ab1d616c84ull;
    g[3].digest = 0x9fe3c4ca6fe8ed6dull;
    g[4].digest = 0x34079eb0fe076c8full;
    g[5].digest = 0xd4f6228848132c60ull;
    g[6].digest = 0xeb8733ef39eca7daull;
    g[7].digest = 0x1749463c5a7dd3e8ull;
    for (i = 0; i < 8; i++) {
        static yrdk_field f;
        uint64_t got;
        if (open_ok(&f, &g[i].d) < 0) continue;
        yrdk_start(&f, g[i].seed, T0);
        for (k = 1; k <= g[i].updates; k++) {
            vary_params(&f, &g[i].d, k, g[i].vary);
            if (yrdk_update(&f, sched(k, g[i].drop, g[i].stall)) < 0) { CHECK(0); break; }
        }
        got = yrdk_digest(&f);
        if (got != g[i].digest) {
            fprintf(stderr, "rdk_test [golden]: FAIL %s: digest %016llx, the reference gives %016llx\n", g[i].name,
                    (unsigned long long)got, (unsigned long long)g[i].digest);
            g_failures++;
        } else if (g_verbose) {
            printf("  [golden] %s %016llx (n %d)\n", g[i].name, (unsigned long long)got, f.n);
        }
        CHECK(inside_all(&f));
        yrdk_close(&f);
    }
}

/* ------------------------------------------------- 3. refusals and order */

static void refuse(const yrdk_desc* d, const char* word) {
    static yrdk_field f;
    if (yrdk_open(&f, d)) {
        fprintf(stderr, "rdk_test [refusals]: FAIL: a desc (%s) was accepted\n", word);
        g_failures++;
        yrdk_close(&f);
        return;
    }
    if (!strstr(yrdk_error(&f), word)) {
        fprintf(stderr, "rdk_test [refusals]: FAIL: message \"%s\" does not name %s\n", yrdk_error(&f), word);
        g_failures++;
    }
    CHECK(yrdk_bytes(d) == 0 || (d->mem && d->mem_bytes < yrdk_bytes(d)));
}

static void test_refusals(void) {
    yrdk_desc d;
    static yrdk_field f;
    static unsigned char small[64];
    g_where = "refusals";
    d = base_desc(); d.count = 10; refuse(&d, "w");
    d = base_desc(); d.w = -1; d.count = 10; refuse(&d, "w");
    d = base_desc(); d.w = NAN; d.count = 10; refuse(&d, "w");
    d = base_desc(); d.w = 10; refuse(&d, "count or density");
    d = base_desc(); d.w = 10; d.count = 5; d.density = 1; refuse(&d, "count or density");
    d = base_desc(); d.w = 10; d.count = -5; refuse(&d, "count");
    d = base_desc(); d.w = 10; d.density = 1e-6f; refuse(&d, "density");
    d = base_desc(); d.aperture = YRDK_RECT; d.w = 10; d.count = 5; refuse(&d, "h");
    d = base_desc(); d.w = 10; d.h = 5; d.count = 5; refuse(&d, "h");
    d = base_desc(); d.w = 10; d.count = 5; d.algorithm = YRDK_MN; d.noise = YRDK_NOISE_WALK; refuse(&d, "preset");
    d = base_desc(); d.w = 10; d.count = 5; d.algorithm = YRDK_WN; d.sets = 2; refuse(&d, "preset");
    d = base_desc(); d.w = 10; d.count = 5; d.sets = 9; refuse(&d, "sets");
    d = base_desc(); d.w = 10; d.count = 5; d.signal = YRDK_SIGNAL_LEAST_RECENT; d.select = YRDK_BERNOULLI; refuse(&d, "EXACT");
    d = base_desc(); d.w = 10; d.count = 5; d.lifetime_ns = 1; d.lifetime_frames = 3; d.frame_ns = 1; refuse(&d, "lifetime");
    d = base_desc(); d.w = 10; d.count = 5; d.lifetime_frames = 3; refuse(&d, "frame_ns");
    d = base_desc(); d.w = 10; d.count = 5; d.clock = YRDK_CLOCK_FRAMES; refuse(&d, "frame_ns");
    d = base_desc(); d.w = 10; d.count = 5; d.lifetime_ns = -1; refuse(&d, "lifetime_ns");
    d = base_desc(); d.w = 10; d.count = 5; d.noise = (yrdk_noise)7; refuse(&d, "noise");
    d = base_desc(); d.w = 10; d.count = 1 << 23; d.sets = 3; refuse(&d, "YRDK_MAX_DOTS");
    d = base_desc(); d.aperture = YRDK_RECT; d.w = 1e-20f; d.h = 1e20f; d.count = 5; refuse(&d, "w");
    d = base_desc(); d.w = 10; d.count = 5; d.mem = small; d.mem_bytes = sizeof small; refuse(&d, "mem_bytes");
    /* order */
    d = base_desc(); d.w = 10; d.count = 5;
    if (open_ok(&f, &d) == 0) {
        CHECK(yrdk_update(&f, 0) == YRDK_ERR_ORDER);
        CHECK(yrdk_xy(&f, NULL) == YRDK_ERR_ORDER || yrdk_xy(&f, NULL) == YRDK_ERR_ARG);
        yrdk_start(&f, 1, 100);
        CHECK(yrdk_update(&f, 99) == YRDK_ERR_ORDER);
        CHECK(strstr(yrdk_error(&f), "before") != NULL);
        CHECK(yrdk_update(&f, 100) == 5);
        CHECK(yrdk_update(&f, 100) == 5);   /* the same t: an update that moves nothing */
        yrdk_close(&f);
    }
    CHECK(yrdk_update(NULL, 0) == YRDK_ERR_ARG);
    CHECK(strcmp(yrdk_version(), YRDK_VERSION_STRING) == 0);
    CHECK(strlen(yrdk_strerror(YRDK_ERR_ORDER)) > 0);
}

/* ----------------------------------------------------- 4. realized coherence */

/* A dot is signal on an update when its displacement is the signal step
 * (within 2 lattice steps), measured from the state, not its flag. */
static void test_coherence(void) {
    static yrdk_field f;
    static int32_t prev[2 * 2000];
    int ns[4] = { 1, 7, 100, 1000 };
    int ni, ci, k, i;
    g_where = "coherence";
    for (ni = 0; ni < 4; ni++) {
        for (ci = 0; ci <= 100; ci += 1) {
            yrdk_desc d = base_desc();
            int n = ns[ni], bad = 0;
            if (n == 1000 && ci % 10) continue;
            d.w = 10; d.count = n; d.signal = YRDK_SIGNAL_DIFFERENT; d.noise = YRDK_NOISE_POSITION;
            d.coherence = (float)ci / 100.0f; d.speed = 0.6f; d.direction = 33;   /* 0.01 R per frame: rare wraps */
            if (open_ok(&f, &d) < 0) return;
            yrdk_start(&f, 1000u + (uint64_t)ci, T0);
            for (k = 1; k <= 30; k++) {
                int32_t s, c;
                int64_t D, ex, ey;
                int moved = 0, expect;
                memcpy(prev, f.xy_, (size_t)n * 8);
                yrdk_update(&f, T0 + k * P_NS);
                D = yrdk__step(&f, f.speed, P_NS);
                yrdk_sincos(yrdk_angle(f.direction), &s, &c);
                ex = yrdk__mulq30(D, c); ey = yrdk__mulq30(D, s);
                for (i = 0; i < n; i++) {
                    int64_t dx = (int64_t)f.xy_[2 * i] - prev[2 * i], dy = (int64_t)f.xy_[2 * i + 1] - prev[2 * i + 1];
                    if (llabs(dx - ex) <= 2 && llabs(dy - ey) <= 2) moved++;
                    else if (f.flags_[i] & YRDK_EV_WRAPPED) moved++;   /* a wrapped signal dot */
                }
                expect = (int)floor((double)d.coherence * n + 0.5);
                if (moved != expect) bad++;
            }
            CHECK(bad == 0);
            yrdk_close(&f);
        }
    }
    /* SAME: a coherence change converts the dots at the boundary of the
     * fixed order: the count follows, and the signal set only grows as the
     * coherence rises. */
    {
        yrdk_desc d = base_desc();
        static uint8_t prev_sig[300];
        int bad = 0, shrunk = 0;
        d.w = 10; d.count = 300; d.speed = 1; d.coherence = 0;
        if (open_ok(&f, &d) < 0) return;
        yrdk_start(&f, 66, T0);
        memset(prev_sig, 0, sizeof prev_sig);
        for (k = 1; k <= 40; k++) {
            int cnt = 0;
            f.coherence = (float)k / 40.0f;
            yrdk_update(&f, T0 + k * P_NS);
            for (i = 0; i < 300; i++) {
                int sg = f.flags_[i] & YRDK_EV_SIGNAL;
                cnt += sg;
                if (prev_sig[i] && !sg) shrunk++;
                prev_sig[i] = (uint8_t)sg;
            }
            if (cnt != (int)floor((double)f.coherence * 300 + 0.5)) bad++;
        }
        CHECK(bad == 0);
        CHECK(shrunk == 0);
        yrdk_close(&f);
    }
    /* BERNOULLI: the count's mean and variance are binomial. */
    {
        yrdk_desc d = base_desc();
        double sum = 0, sum2 = 0, p = 0.3, n = 400, mean, var, z, zv;
        int u, U = 4000;
        d.w = 10; d.count = 400; d.signal = YRDK_SIGNAL_DIFFERENT; d.select = YRDK_BERNOULLI;
        d.noise = YRDK_NOISE_POSITION; d.coherence = 0.3f; d.speed = 1;
        if (open_ok(&f, &d) < 0) return;
        yrdk_start(&f, 77, T0);
        for (u = 1; u <= U; u++) {
            int cnt = 0;
            yrdk_update(&f, T0 + u * P_NS);
            for (i = 0; i < 400; i++) cnt += f.flags_[i] & YRDK_EV_SIGNAL;
            sum += cnt; sum2 += (double)cnt * cnt;
        }
        mean = sum / U; var = sum2 / U - mean * mean;
        z = (mean - n * (double)0.3f) / sqrt(n * p * (1 - p) / U);
        zv = (var - n * p * (1 - p)) / (n * p * (1 - p) * sqrt(2.0 / (U - 1)));
        CHECK_LE(fabs(z), 5);
        CHECK_LE(fabs(zv), 5);
        yrdk_close(&f);
    }
}

/* ----------------------------------------------------------- 5. density */

/* Equal-area cells: CIRCLE 8 rings x 16 sectors, plus the outermost 5 % of
 * the area in 64 sectors (the edge band); RECT a 16 x 16 grid, plus the
 * one-cell border in 60 cells. Returns the larger chi-square / critical. */
static double density_stat(const yrdk_desc* d, int seeds, double seconds, int print_profile) {
    static yrdk_field f;
    static long cells[256], band[64];
    double chi = 0, chib = 0, crit, critb, e, eb;
    long nall = 0, nband = 0;
    int s, i, ncell = d->aperture == YRDK_RECT ? 256 : 128, nb = 64;
    int updates = (int)(seconds * 60.0);
    long prof[10];
    memset(cells, 0, sizeof cells);
    memset(band, 0, sizeof band);
    memset(prof, 0, sizeof prof);
    if (open_ok(&f, d) < 0) return 1e9;
    for (s = 0; s < seeds; s++) {
        int k;
        yrdk_start(&f, 5000u + (uint64_t)s, T0);
        for (k = 1; k <= updates; k++) yrdk_update(&f, T0 + k * P_NS);
        for (i = 0; i < f.total; i++) {
            double x = f.xy_[2 * i] / 1073741824.0, y = f.xy_[2 * i + 1] / 1073741824.0;
            if (d->aperture == YRDK_RECT) {
                double ax = f.hx_ / 1073741824.0, ay = f.hy_ / 1073741824.0;
                int cx = (int)floor((x + ax) / (2 * ax) * 16), cy = (int)floor((y + ay) / (2 * ay) * 16);
                cx = clampi(cx, 0, 15);
                cy = clampi(cy, 0, 15);
                cells[cy * 16 + cx]++;
                if (cx == 0 || cx == 15 || cy == 0 || cy == 15) {
                    int bi = cy == 0 ? cx : cy == 15 ? 16 + cx : cx == 0 ? 32 + cy - 1 : 46 + cy - 1;
                    band[bi]++;
                    nband++;
                }
                {
                    double along = x * cos(d->direction * M_PI / 180) + y * sin(d->direction * M_PI / 180);
                    int pb = (int)floor((along / ax + 1) * 5);
                    if (pb >= 0 && pb < 10) prof[pb]++;
                }
            } else {
                double r2 = x * x + y * y, th = atan2(y, x);
                int ring = (int)floor(r2 * 8), sec = (int)floor((th + M_PI) / (2 * M_PI) * 16);
                ring = clampi(ring, 0, 7);
                sec = clampi(sec, 0, 15);
                cells[ring * 16 + sec]++;
                if (r2 >= 0.95) {
                    int bs = (int)floor((th + M_PI) / (2 * M_PI) * 64);
                    bs = clampi(bs, 0, 63);
                    band[bs]++;
                    nband++;
                }
                {
                    /* along the motion, in 10 bins of equal width across the disc */
                    double along = x * cos(d->direction * M_PI / 180) + y * sin(d->direction * M_PI / 180);
                    int pb = (int)floor((along + 1) * 5);
                    if (pb >= 0 && pb < 10) prof[pb]++;
                }
            }
            nall++;
        }
    }
    e = (double)nall / ncell;
    for (i = 0; i < ncell; i++) chi += (cells[i] - e) * (cells[i] - e) / e;
    if (d->aperture == YRDK_RECT) nb = 60;
    eb = (double)nband / nb;
    for (i = 0; i < nb; i++) chib += (band[i] - eb) * (band[i] - eb) / eb;
    crit = chi2_crit(ncell - 1);
    critb = chi2_crit(nb - 1);
    if (print_profile) {
        printf("  profile along the motion (10 bins, trailing edge first; uniform = disc chord weights):");
        for (i = 0; i < 10; i++) printf(" %ld", prof[i]);
        printf("\n  chi-square cells %.1f (crit %.1f), edge band %.1f (crit %.1f)\n", chi, crit, chib, critb);
    }
    yrdk_close(&f);
    return chi / crit > chib / critb ? chi / crit : chib / critb;
}

static void test_density(void) {
    int sh, sg, no, life;
    char where[96];
    for (sh = 0; sh < 2; sh++)
        for (sg = 0; sg < 3; sg++)
            for (no = 0; no < 3; no++)
                for (life = 0; life < 2; life++) {
                    yrdk_desc d = base_desc();
                    double r;
                    d.aperture = (yrdk_shape)sh; d.w = 10; d.h = sh ? 6.0f : 0.0f;
                    d.count = 500; d.signal = (yrdk_signal)sg; d.noise = (yrdk_noise)no;
                    d.coherence = 0.5f; d.direction = 20; d.speed = 30;   /* 0.1 of the radius per frame */
                    if (life) d.lifetime_ns = life_s(0.2f);
                    snprintf(where, sizeof where, "density %s signal %d noise %d life %d", sh ? "rect" : "circle", sg, no, life);
                    g_where = where;
                    r = density_stat(&d, 100, 2.0, 0);
                    CHECK_LE(r, 1.0);
                }
    /* REPLOT must be rejected: the gradient along the motion. */
    for (sh = 0; sh < 2; sh++) {
        yrdk_desc d = base_desc();
        double r;
        d.aperture = (yrdk_shape)sh; d.w = 10; d.h = sh ? 6.0f : 0.0f;
        d.count = 500; d.coherence = 0.5f; d.direction = 20; d.speed = 30; d.edge = YRDK_REPLOT;
        g_where = sh ? "density REPLOT rect" : "density REPLOT circle";
        r = density_stat(&d, 100, 2.0, g_verbose);
        CHECK(r > 10.0);
        if (g_verbose) printf("  [%s] chi-square / critical = %.1f (must be > 10)\n", g_where, r);
    }
}

/* ----------------------------------------------------------- 6. lifetime */

static void test_lifetime(void) {
    static yrdk_field f;
    static int64_t born[1000];
    yrdk_desc d = base_desc();
    int i, k, bad = 0, maxd = 0;
    long lives = 0, hist[3] = { 0, 0, 0 };
    double sum_life = 0;
    g_where = "lifetime frames";
    d.w = 10; d.count = 1000; d.lifetime_frames = 7; d.frame_ns = P_NS; d.clock = YRDK_CLOCK_FRAMES;
    d.coherence = 0.5f; d.speed = 3;
    if (open_ok(&f, &d) < 0) return;
    yrdk_start(&f, 9, T0);
    for (i = 0; i < 1000; i++) born[i] = -1;
    {
        long first[8];
        memset(first, 0, sizeof first);
        for (k = 1; k <= 2000; k++) {
            int deaths = 0;
            uint64_t before = f.stats.deaths;
            yrdk_update(&f, T0 + k * P_NS);
            deaths = (int)(f.stats.deaths - before);
            if (deaths > maxd) maxd = deaths;
            for (i = 0; i < 1000; i++) {
                if (f.flags_[i] & YRDK_EV_PLACED) {
                    if (born[i] >= 0) { if (k - born[i] != 7) bad++; lives++; }
                    else if (k <= 7) first[k]++;
                    born[i] = k;
                }
            }
        }
        CHECK(bad == 0);
        CHECK(lives > 200000);
        /* start: remaining lives uniform over 1..7 updates: chi-square, 6 df */
        {
            double e = 1000.0 / 7, chi = 0;
            for (k = 1; k <= 7; k++) chi += (first[k] - e) * (first[k] - e) / e;
            CHECK_LE(chi, chi2_crit(6));
        }
        /* no pulse: deaths per update stay near 1000 / 7 */
        CHECK_LE(maxd, 1000.0 / 7 + 5 * sqrt(1000.0 / 7));
    }
    yrdk_close(&f);
    /* ONSET clock with L = 5.5 frames and jittered onsets: lives of 5 and 6
     * frames, mean 5.5 frames of time. */
    g_where = "lifetime onset";
    d = base_desc();
    d.w = 10; d.count = 1000; d.lifetime_ns = life_s((float)(5.5 * 16.666667e-3)); d.coherence = 0.5f; d.speed = 3;
    if (open_ok(&f, &d) < 0) return;
    yrdk_start(&f, 10, T0);
    for (i = 0; i < 1000; i++) born[i] = -1;
    for (k = 1; k <= 3000; k++) {
        int64_t t = T0 + k * P_NS;
        yrdk_update(&f, t);
        for (i = 0; i < 1000; i++) {
            if (f.flags_[i] & YRDK_EV_PLACED) {
                if (born[i] >= 0) {
                    int fr = (int)((t - born[i] + P_NS / 2) / P_NS);
                    if (fr >= 5 && fr <= 6) hist[fr - 5]++; else hist[2]++;
                    sum_life += (double)(t - born[i]);
                    lives++;
                }
                born[i] = t;
            }
        }
    }
    CHECK(hist[2] == 0);
    CHECK(hist[0] > 0 && hist[1] > 0);
    {
        /* the scheduled mean is exact; the realized one is quantized to the
         * update grid, so it is within half a frame over a life */
        double mean = sum_life / (double)(hist[0] + hist[1]);
        double L = (double)f.life_ns_;
        CHECK_LE(fabs(mean - L) / L, 0.002);
    }
    yrdk_close(&f);
}

/* ------------------------------------------------------- 7. displacement */

static void test_displacement(void) {
    static yrdk_field f;
    static float a[2 * 400], b[2 * 400];
    static uint8_t ev[400];
    yrdk_desc d = base_desc();
    int k, i, sh;
    double worst = 0, worst_wrap = 0;
    for (sh = 0; sh < 2; sh++) {
        int64_t t = T0;
        g_where = sh ? "displacement rect" : "displacement circle";
        d = base_desc();
        d.aperture = (yrdk_shape)sh; d.w = 10; d.h = sh ? 7.0f : 0.0f; d.count = 400; d.coherence = 1;
        d.direction = 37.5f; d.speed = 4.75f;
        if (open_ok(&f, &d) < 0) return;
        yrdk_start(&f, 3, t);
        yrdk_update(&f, t);
        yrdk_xy(&f, a);
        for (k = 1; k <= 600; k++) {
            int64_t dt = P_NS + (int64_t)(((k * 7919) % 101) - 50) * 1000 + (k % 97 == 0 ? P_NS : 0);   /* drops */
            double ex, ey;
            yrdk_out o;
            t += dt;
            f.direction = (float)(37.5 + (k % 5) * 61.25);
            f.speed = (float)(4.75 + (k % 3));
            yrdk_update(&f, t);
            memset(&o, 0, sizeof o);
            o.xy = b; o.event = ev;
            yrdk_write(&f, &o);
            ex = f.speed * (double)dt * 1e-9 * cos(f.direction * M_PI / 180);
            ey = f.speed * (double)dt * 1e-9 * sin(f.direction * M_PI / 180);
            for (i = 0; i < 400; i++) {
                double dx = (double)b[2 * i] - a[2 * i], dy = (double)b[2 * i + 1] - a[2 * i + 1];
                double err;
                if (ev[i] & YRDK_EV_WRAPPED) {
                    if (sh) {   /* the torus: the displacement modulo the width and height */
                        dx -= 10.0 * floor((dx - ex) / 10.0 + 0.5);
                        dy -= 7.0 * floor((dy - ey) / 7.0 + 0.5);
                        err = fmax(fabs(dx - ex), fabs(dy - ey));
                        if (err > worst_wrap) worst_wrap = err;
                    }
                    continue;
                }
                err = fmax(fabs(dx - ex), fabs(dy - ey));
                if (err > worst) worst = err;
            }
            memcpy(a, b, sizeof a);
        }
        yrdk_close(&f);
    }
    /* float output: an ulp at the edge is 5 x 2^-23 = 6e-7; two positions */
    g_where = "displacement";
    CHECK_LE(worst, 1.2e-6);
    CHECK_LE(worst_wrap, 2.4e-6);
}

/* ---------------------------------------------------------- 8. noise rules */

static void test_noise(void) {
    static yrdk_field f;
    static float dir_now[600], dir_prev[600], xy_prev[1200], xy_now[1200];
    static uint8_t sig[600], sig_prev[600], ev[600];
    static long bins[72], dbins[72], contingency[8][8];
    yrdk_desc d;
    int k, i, nn = 0;
    double chi, e;
    yrdk_out o;
    /* WALK: per-step directions uniform; the next step's direction
     * independent of this one's. */
    g_where = "noise walk";
    d = base_desc();
    d.w = 10; d.count = 600; d.signal = YRDK_SIGNAL_DIFFERENT; d.noise = YRDK_NOISE_WALK; d.coherence = 0.25f;
    d.direction = 0; d.speed = 3;
    if (open_ok(&f, &d) < 0) return;
    yrdk_start(&f, 21, T0);
    memset(bins, 0, sizeof bins); memset(dbins, 0, sizeof dbins);
    memset(&o, 0, sizeof o);
    o.dir = dir_now; o.signal = sig;
    for (k = 1; k <= 400; k++) {
        yrdk_update(&f, T0 + k * P_NS);
        yrdk_write(&f, &o);
        for (i = 0; i < 600; i++) {
            if (sig[i]) continue;
            bins[(int)(dir_now[i] / 5.0f) % 72]++;
            nn++;
            if (k > 1 && !sig_prev[i]) {
                double df = fmod(dir_now[i] - dir_prev[i] + 720.0, 360.0);
                dbins[(int)(df / 5.0) % 72]++;
            }
        }
        memcpy(dir_prev, dir_now, sizeof dir_now);
        memcpy(sig_prev, sig, sizeof sig);
    }
    {
        long nd = 0;
        e = nn / 72.0; chi = 0;
        for (i = 0; i < 72; i++) chi += (bins[i] - e) * (bins[i] - e) / e;
        CHECK_LE(chi, chi2_crit(71));
        for (i = 0; i < 72; i++) nd += dbins[i];
        e = nd / 72.0; chi = 0;
        for (i = 0; i < 72; i++) chi += (dbins[i] - e) * (dbins[i] - e) / e;
        CHECK_LE(chi, chi2_crit(71));
    }
    /* noise steps have the signal speed */
    {
        int64_t D = yrdk__step(&f, 3.0f, P_NS);
        double worst = 0;
        static int32_t prev[1200];
        memcpy(prev, f.xy_, sizeof prev);
        yrdk_update(&f, T0 + 401 * P_NS);
        for (i = 0; i < 600; i++) {
            double len;
            if ((f.flags_[i] & (YRDK_EV_SIGNAL | YRDK_EV_WRAPPED | YRDK_EV_PLACED))) continue;
            len = hypot((double)f.xy_[2 * i] - prev[2 * i], (double)f.xy_[2 * i + 1] - prev[2 * i + 1]);
            if (fabs(len - (double)D) > worst) worst = fabs(len - (double)D);
        }
        CHECK_LE(worst, 3.0);   /* lattice steps: the rounding of each axis */
    }
    yrdk_close(&f);

    /* DIRECTION: a noise dot keeps its direction; directions uniform over dots. */
    g_where = "noise direction";
    d.noise = YRDK_NOISE_DIRECTION;
    if (open_ok(&f, &d) < 0) return;
    yrdk_start(&f, 22, T0);
    {
        static uint32_t own[600];
        int changed = 0;
        memcpy(own, f.ang_, sizeof own);
        memset(bins, 0, sizeof bins);
        for (i = 0; i < 600; i++) bins[(int)((double)own[i] / 4294967296.0 * 72)]++;
        e = 600 / 72.0; chi = 0;
        for (i = 0; i < 72; i++) chi += (bins[i] - e) * (bins[i] - e) / e;
        CHECK_LE(chi, chi2_crit(71));
        for (k = 1; k <= 200; k++) {
            yrdk_update(&f, T0 + k * P_NS);
            for (i = 0; i < 600; i++) if (f.ang_[i] != own[i]) changed++;
        }
        CHECK(changed == 0);
    }
    yrdk_close(&f);

    /* POSITION: new positions uniform and independent of the old ones. */
    g_where = "noise position";
    d.noise = YRDK_NOISE_POSITION; d.coherence = 0;
    if (open_ok(&f, &d) < 0) return;
    yrdk_start(&f, 23, T0);
    memset(contingency, 0, sizeof contingency);
    memset(&o, 0, sizeof o);
    o.xy = xy_now; o.event = ev;
    yrdk_update(&f, T0);
    yrdk_write(&f, &o);
    for (k = 1; k <= 300; k++) {
        memcpy(xy_prev, xy_now, sizeof xy_now);
        yrdk_update(&f, T0 + k * P_NS);
        yrdk_write(&f, &o);
        for (i = 0; i < 600; i++) {
            /* cell: 8 sectors of the disc (equal area) */
            int a0 = (int)floor((atan2(xy_prev[2 * i + 1], xy_prev[2 * i]) + M_PI) / (2 * M_PI) * 8) & 7;
            int a1 = (int)floor((atan2(xy_now[2 * i + 1], xy_now[2 * i]) + M_PI) / (2 * M_PI) * 8) & 7;
            contingency[a0][a1]++;
        }
    }
    {
        double tot = 600.0 * 300, rows[8], cols[8];
        int r, c;
        memset(rows, 0, sizeof rows); memset(cols, 0, sizeof cols);
        for (r = 0; r < 8; r++) for (c = 0; c < 8; c++) { rows[r] += contingency[r][c]; cols[c] += contingency[r][c]; }
        chi = 0;
        for (r = 0; r < 8; r++) for (c = 0; c < 8; c++) {
            double ex = rows[r] * cols[c] / tot;
            chi += (contingency[r][c] - ex) * (contingency[r][c] - ex) / ex;
        }
        CHECK_LE(chi, chi2_crit(49));
    }
    yrdk_close(&f);
}

/* --------------------------------------------------------- 9. frozen noise */

static void test_frozen(void) {
    static yrdk_field A, B;
    static float xa[1000], xb[1000], da[500], db[500];
    static uint8_t sa[500], sb[500];
    int rule, k, i;
    for (rule = 0; rule < 3; rule++) {
        yrdk_desc d = base_desc();
        int same_pos = 0, diff_pos = 0, nested = 1, same_dir = 0, diff_dir = 0;
        yrdk_out oa, ob;
        g_where = rule == 0 ? "frozen position" : rule == 1 ? "frozen walk" : "frozen direction";
        d.w = 10; d.count = 500; d.signal = YRDK_SIGNAL_DIFFERENT;
        d.noise = rule == 0 ? YRDK_NOISE_POSITION : rule == 1 ? YRDK_NOISE_WALK : YRDK_NOISE_DIRECTION;
        d.speed = 3; d.direction = 10;
        d.coherence = 0.2f;
        if (open_ok(&A, &d) < 0) return;
        d.coherence = 0.55f;
        if (open_ok(&B, &d) < 0) return;
        yrdk_start(&A, 99, T0);
        yrdk_start(&B, 99, T0);
        memset(&oa, 0, sizeof oa); memset(&ob, 0, sizeof ob);
        oa.xy = xa; oa.dir = da; oa.signal = sa;
        ob.xy = xb; ob.dir = db; ob.signal = sb;
        for (k = 1; k <= 200; k++) {
            yrdk_update(&A, T0 + k * P_NS);
            yrdk_update(&B, T0 + k * P_NS);
            yrdk_write(&A, &oa);
            yrdk_write(&B, &ob);
            for (i = 0; i < 500; i++) {
                if (sa[i] && !sb[i]) nested = 0;
                if (sa[i] || sb[i]) continue;
                if (rule == 0) { if (xa[2 * i] == xb[2 * i] && xa[2 * i + 1] == xb[2 * i + 1]) same_pos++; else diff_pos++; }
                else { if (da[i] == db[i]) same_dir++; else diff_dir++; }
            }
        }
        CHECK(nested);
        if (rule == 0) { CHECK(diff_pos == 0); CHECK(same_pos > 10000); }
        else { CHECK(diff_dir == 0); CHECK(same_dir > 10000); }
        yrdk_close(&A);
        yrdk_close(&B);
    }
    /* BERNOULLI is nested too */
    {
        yrdk_desc d = base_desc();
        int nested = 1;
        g_where = "frozen bernoulli";
        d.w = 10; d.count = 500; d.signal = YRDK_SIGNAL_DIFFERENT; d.select = YRDK_BERNOULLI;
        d.noise = YRDK_NOISE_POSITION; d.speed = 3; d.coherence = 0.3f;
        if (open_ok(&A, &d) < 0) return;
        d.coherence = 0.7f;
        if (open_ok(&B, &d) < 0) return;
        yrdk_start(&A, 5, T0); yrdk_start(&B, 5, T0);
        for (k = 1; k <= 100; k++) {
            yrdk_update(&A, T0 + k * P_NS);
            yrdk_update(&B, T0 + k * P_NS);
            for (i = 0; i < 500; i++) if ((A.flags_[i] & 1) && !(B.flags_[i] & 1)) nested = 0;
        }
        CHECK(nested);
        yrdk_close(&A); yrdk_close(&B);
    }
}

/* -------------------------------------------------------- 10. dropped frames */

static void test_drops(void) {
    static yrdk_field A, B;
    static float xa[800], xb[800];
    yrdk_desc d = base_desc();
    int k, i, sh;
    double worst = 0;
    /* ONSET: a run that misses update 50 equals the full run at the same
     * time (SAME, DIRECTION, no lifetime: no draw after start). */
    for (sh = 0; sh < 2; sh++) {
        g_where = sh ? "drops onset rect" : "drops onset circle";
        d = base_desc();
        d.aperture = (yrdk_shape)sh; d.w = 10; d.h = sh ? 8.0f : 0.0f; d.count = 400; d.coherence = 0.5f;
        d.direction = 20; d.speed = 9;
        if (open_ok(&A, &d) < 0 || open_ok(&B, &d) < 0) return;
        yrdk_start(&A, 31, T0); yrdk_start(&B, 31, T0);
        for (k = 1; k <= 200; k++) {
            int64_t t = T0 + k * P_NS;
            yrdk_update(&A, t);
            if (k != 50) yrdk_update(&B, t);
            if (k > 50) {
                yrdk_xy(&A, xa); yrdk_xy(&B, xb);
                for (i = 0; i < 800; i++) if (fabs(xa[i] - xb[i]) > worst) worst = fabs(xa[i] - xb[i]);
            }
        }
        CHECK_LE(worst, 1.2e-6);
        yrdk_close(&A); yrdk_close(&B);
    }
    /* FRAMES: the run with a drop is the full run, one update behind, bit for bit. */
    g_where = "drops frames";
    d = base_desc();
    d.algorithm = YRDK_WN; d.w = 10; d.count = 300; d.coherence = 0.5f; d.speed = 9; d.lifetime_frames = 4;
    d.frame_ns = P_NS; d.clock = YRDK_CLOCK_FRAMES;
    if (open_ok(&A, &d) < 0 || open_ok(&B, &d) < 0) return;
    yrdk_start(&A, 32, T0); yrdk_start(&B, 32, T0);
    {
        int kb = 0, bad = 0;
        for (k = 1; k <= 200; k++)
            if (k != 50) { yrdk_update(&B, T0 + k * P_NS); kb++; }
        for (k = 1; k <= kb; k++) yrdk_update(&A, T0 + k * P_NS);
        for (i = 0; i < 600; i++) if (A.xy_[i] != B.xy_[i]) bad++;
        for (i = 0; i < 300; i++) if (A.ang_[i] != B.ang_[i] || A.flags_[i] != B.flags_[i]) bad++;
        CHECK(bad == 0);
    }
    yrdk_close(&A); yrdk_close(&B);
    /* ONSET under random rules: a drop moves the dots by the time that passed. */
    g_where = "drops onset step";
    d = base_desc();
    d.w = 10; d.count = 100; d.coherence = 1; d.speed = 3; d.direction = 90;
    if (open_ok(&A, &d) < 0) return;
    yrdk_start(&A, 33, T0);
    yrdk_update(&A, T0 + P_NS);
    yrdk_xy(&A, xa);
    yrdk_update(&A, T0 + 3 * P_NS);   /* two periods: a dropped frame */
    yrdk_xy(&A, xb);
    {
        double worst2 = 0;
        for (i = 0; i < 100; i++) {
            if (A.flags_[i] & YRDK_EV_WRAPPED) continue;
            worst2 = fmax(worst2, fabs((xb[2 * i + 1] - xa[2 * i + 1]) - 3.0 * 2 * P_NS * 1e-9));
        }
        CHECK_LE(worst2, 1.2e-6);
    }
    yrdk_close(&A);
}

/* ------------------------------------------------------ 11. interleaved sets */

static void test_sets(void) {
    static yrdk_field f;
    static float last[3][400], now[400];
    static int64_t last_t[3];
    yrdk_desc d = base_desc();
    int k, i;
    double worst = 0;
    g_where = "sets";
    d.w = 10; d.count = 200; d.sets = 3; d.coherence = 1; d.speed = 2; d.direction = 0;
    if (open_ok(&f, &d) < 0) return;
    CHECK(f.n == 200 && f.total == 600);
    yrdk_start(&f, 41, T0);
    for (k = 0; k < 3; k++) last_t[k] = -1;
    for (k = 1; k <= 300; k++) {
        int64_t t = T0 + k * P_NS + (k % 7) * 1000;
        int s, ok;
        static int32_t snap_other[600 * 2];
        memcpy(snap_other, f.xy_, sizeof snap_other);
        ok = yrdk_update(&f, t);
        CHECK(ok == 200);
        s = f.last.set;
        CHECK(s == (k - 1) % 3);
        /* the other sets did not move */
        for (i = 0; i < 600; i++) if (i / 200 != s && (f.xy_[2 * i] != snap_other[2 * i] || f.xy_[2 * i + 1] != snap_other[2 * i + 1])) { CHECK(0); break; }
        yrdk_xy(&f, now);
        if (last_t[s] >= 0) {
            double ex = 2.0 * (double)(t - last_t[s]) * 1e-9;
            for (i = 0; i < 200; i++) {
                if (f.flags_[s * 200 + i] & YRDK_EV_WRAPPED) continue;
                worst = fmax(worst, fabs((now[2 * i] - last[s][2 * i]) - ex));
            }
        }
        memcpy(last[s], now, sizeof now);
        last_t[s] = t;
    }
    CHECK_LE(worst, 1.2e-6);
    yrdk_close(&f);
}

/* ------------------------------------------------------- 12. LEAST_RECENT */

static void test_least_recent(void) {
    static yrdk_field f;
    static int run[300], was[300];
    yrdk_desc d;
    int k, i, twice = 0, order_bad = 0, ci;
    for (ci = 0; ci < 2; ci++) {
        d = base_desc();
        g_where = ci ? "least recent 0.7" : "least recent 0.4";
        d.w = 10; d.count = 300; d.signal = YRDK_SIGNAL_LEAST_RECENT; d.noise = YRDK_NOISE_POSITION;
        d.coherence = ci ? 0.7f : 0.4f; d.speed = 2;
        if (open_ok(&f, &d) < 0) return;
        yrdk_start(&f, 51 + (uint64_t)ci, T0);
        memset(run, 0, sizeof run);
        memset(was, 0, sizeof was);
        for (k = 1; k <= 300; k++) {
            int max_kept = -1, min_dropped = 1 << 30;
            yrdk_update(&f, T0 + k * P_NS);
            for (i = 0; i < 300; i++) {
                int s = f.flags_[i] & YRDK_EV_SIGNAL;
                if (s && was[i]) twice++;
                if (was[i]) {   /* previous signal dots: the kept ones ran no longer than the dropped */
                    if (s && run[i] > max_kept) max_kept = run[i];
                    if (!s && run[i] < min_dropped) min_dropped = run[i];
                }
            }
            if (max_kept > min_dropped) order_bad++;
            for (i = 0; i < 300; i++) {
                int s = f.flags_[i] & YRDK_EV_SIGNAL;
                run[i] = s ? run[i] + 1 : 0;
                was[i] = s;
                CHECK(f.run_[i] == run[i]);
            }
        }
        /* the kept are the shortest-running: every kept run <= every dropped run */
        if (ci == 0) CHECK(twice == 0);
        else {
            CHECK(twice > 0);
            CHECK(order_bad == 0);
        }
        twice = 0;
        yrdk_close(&f);
    }
    (void)order_bad;
}

/* ------------------------------------------------- 13. edges under stalls */

static void test_edges(void) {
    static yrdk_field f;
    int sh, ed, k, bad = 0;
    int64_t stalls[5] = { 100000000LL, 1000000000LL, 3000000000LL, 10000000000LL, 1000000000000LL };
    for (sh = 0; sh < 2; sh++)
        for (ed = 0; ed < 2; ed++) {
            yrdk_desc d = base_desc();
            int64_t t = T0;
            g_where = "edges";
            d.aperture = (yrdk_shape)sh; d.w = 10; d.h = sh ? 3.0f : 0.0f; d.edge = (yrdk_edge)ed; d.count = 300;
            d.signal = YRDK_SIGNAL_DIFFERENT; d.noise = YRDK_NOISE_WALK; d.coherence = 0.5f; d.speed = 13;
            d.direction = 123.4f;
            if (open_ok(&f, &d) < 0) return;
            yrdk_start(&f, 61, t);
            for (k = 1; k <= 500; k++) {
                t += (k % 50 == 0) ? stalls[(k / 50) % 5] : P_NS;
                f.direction = (float)(k * 13.7);
                yrdk_update(&f, t);
                if (!inside_all(&f)) bad++;
            }
            CHECK(bad == 0);
            CHECK(f.stats.exhausted == 0);
            if (g_verbose) printf("  [edges] shape %d edge %d: stuck %llu of %llu wraps\n", sh, ed,
                                  (unsigned long long)f.stats.stuck, (unsigned long long)f.stats.wraps);
            yrdk_close(&f);
        }
    /* a chord wrap keeps the dot on its own line */
    {
        yrdk_desc d = base_desc();
        int i, off = 0, wraps = 0;
        static int32_t prev[400];
        g_where = "edges chord";
        d.w = 10; d.count = 200; d.coherence = 1; d.speed = 20; d.direction = 33;
        if (open_ok(&f, &d) < 0) return;
        yrdk_start(&f, 62, T0);
        for (k = 1; k <= 200; k++) {
            int32_t s, c;
            memcpy(prev, f.xy_, sizeof prev);
            yrdk_update(&f, T0 + k * P_NS);
            yrdk_sincos(yrdk_angle(33), &s, &c);
            for (i = 0; i < 200; i++) {
                if (!(f.flags_[i] & YRDK_EV_WRAPPED)) continue;
                wraps++;
                /* perpendicular offset unchanged within 2 lattice steps */
                {
                    double p0 = ((double)prev[2 * i] * -s + (double)prev[2 * i + 1] * c) / 1073741824.0;
                    double p1 = ((double)f.xy_[2 * i] * -s + (double)f.xy_[2 * i + 1] * c) / 1073741824.0;
                    if (fabs(p1 - p0) > 2) off++;
                }
            }
        }
        CHECK(wraps > 100);
        CHECK(off == 0);
        yrdk_close(&f);
    }
}

/* ---------------------------------------- 14. reproducibility and memory */

static void test_repro(void) {
    static yrdk_field A, B, C;
    static yrdk_step steps[300];
    static unsigned char mem[1 << 16];
    static unsigned char snap[1 << 16];
    yrdk_desc d = base_desc();
    int k;
    uint64_t da, db;
    long before;
    g_where = "repro";
    d.algorithm = YRDK_LL; d.w = 8; d.count = 100; d.coherence = 0.6f; d.speed = 5; d.lifetime_ns = life_s(0.3f);
    if (open_ok(&A, &d) < 0 || open_ok(&B, &d) < 0) return;
    before = g_allocs;
    yrdk_start(&A, 71, T0);
    yrdk_start(&B, 71, T0);
    for (k = 1; k <= 300; k++) {
        int64_t t = sched(k, 30, -1);
        A.coherence = (float)(k % 9) / 8.0f;
        A.direction = (float)(k * 3);
        yrdk_update(&A, t);
        steps[k - 1] = A.last;
        B.coherence = A.coherence; B.direction = A.direction;
        yrdk_update(&B, t);
    }
    CHECK(g_allocs == before);   /* nothing after open allocates */
    da = yrdk_digest(&A); db = yrdk_digest(&B);
    CHECK(da == db);
    /* replay of the log */
    CHECK(yrdk_replay(&B, 71, T0, steps, 300) == 0);
    CHECK(yrdk_digest(&B) == da);
    /* another seed, another stream */
    yrdk_start(&B, 72, T0);
    for (k = 1; k <= 30; k++) yrdk_update(&B, sched(k, -1, -1));
    A.coherence = d.coherence; A.direction = d.direction;
    yrdk_start(&A, 71, T0);
    for (k = 1; k <= 30; k++) yrdk_update(&A, sched(k, -1, -1));
    CHECK(yrdk_digest(&A) != yrdk_digest(&B));
    yrdk_close(&B);
    d.stream = 1;
    if (open_ok(&B, &d) < 0) return;
    yrdk_start(&B, 71, T0);
    for (k = 1; k <= 30; k++) yrdk_update(&B, sched(k, -1, -1));
    CHECK(yrdk_digest(&A) != yrdk_digest(&B));
    /* caller memory gives the same field */
    d.stream = 0;
    d.mem = mem; d.mem_bytes = sizeof mem;
    before = g_allocs;
    if (open_ok(&C, &d) < 0) return;
    CHECK(g_allocs == before);
    yrdk_start(&C, 71, T0);
    for (k = 1; k <= 30; k++) yrdk_update(&C, sched(k, -1, -1));
    CHECK(yrdk_digest(&C) == yrdk_digest(&A));
    /* snapshot at 30, run on, restore, run on again: the same */
    CHECK(yrdk_snapshot_bytes(&A) <= sizeof snap);
    CHECK(yrdk_snapshot(&A, snap, sizeof snap) == 0);
    for (k = 31; k <= 90; k++) yrdk_update(&A, sched(k, -1, -1));
    da = yrdk_digest(&A);
    CHECK(yrdk_restore(&C, snap, yrdk_snapshot_bytes(&A)) == 0);
    for (k = 31; k <= 90; k++) yrdk_update(&C, sched(k, -1, -1));
    CHECK(yrdk_digest(&C) == da);
    CHECK(yrdk_restore(&C, snap, 10) == YRDK_ERR_ARG);
    yrdk_close(&A); yrdk_close(&B); yrdk_close(&C);
    /* two fields interleaved give what each gives alone */
    {
        yrdk_desc d1 = base_desc(), d2 = base_desc();
        uint64_t a1, a2;
        d1.w = 10; d1.count = 120; d1.coherence = 0.5f; d1.speed = 4; d1.direction = 0; d1.signal = YRDK_SIGNAL_DIFFERENT;
        d2 = d1; d2.direction = 180; d2.stream = 1;
        if (open_ok(&A, &d1) < 0 || open_ok(&B, &d2) < 0) return;
        yrdk_start(&A, 80, T0); yrdk_start(&B, 80, T0);
        for (k = 1; k <= 100; k++) { yrdk_update(&A, T0 + k * P_NS); yrdk_update(&B, T0 + k * P_NS); }
        a1 = yrdk_digest(&A); a2 = yrdk_digest(&B);
        yrdk_start(&A, 80, T0);
        for (k = 1; k <= 100; k++) yrdk_update(&A, T0 + k * P_NS);
        CHECK(yrdk_digest(&A) == a1);
        yrdk_start(&B, 80, T0);
        for (k = 1; k <= 100; k++) yrdk_update(&B, T0 + k * P_NS);
        CHECK(yrdk_digest(&B) == a2);
        CHECK(a1 != a2);
        yrdk_close(&A); yrdk_close(&B);
    }
}

/* ---------------------------------------------------------- 15. outputs */

typedef struct elem { float x, y, ori, phase, contrast, scale, gate, color; } elem;   /* ygfx_inst's layout */

static void test_outputs(void) {
    static yrdk_field f;
    static elem items[50];
    static float xy[100], dir[50], age[50];
    static uint8_t sig[50], ev[50];
    yrdk_desc d = base_desc();
    yrdk_out o;
    int i, k, n;
    const yrdk_param* p;
    g_where = "outputs";
    d.w = 10; d.count = 50; d.coherence = 0.5f; d.speed = 2; d.direction = -45; d.lifetime_ns = life_s(0.5f);
    if (open_ok(&f, &d) < 0) return;
    yrdk_start(&f, 90, T0);
    for (k = 1; k <= 20; k++) yrdk_update(&f, T0 + k * P_NS);
    memset(&o, 0, sizeof o);
    o.xy = xy; o.dir = dir; o.age_s = age; o.signal = sig; o.event = ev;
    CHECK(yrdk_write(&f, &o) == 50);
    memset(items, 0, sizeof items);
    memset(&o, 0, sizeof o);
    o.xy = &items[0].x; o.xy_stride = sizeof items[0];
    o.dir = &items[0].ori; o.dir_stride = sizeof items[0];
    o.age_s = &items[0].phase; o.age_stride = sizeof items[0];
    CHECK(yrdk_write(&f, &o) == 50);
    for (i = 0; i < 50; i++) {
        CHECK(items[i].x == xy[2 * i] && items[i].y == xy[2 * i + 1]);
        CHECK(items[i].ori == dir[i]);
        CHECK(items[i].phase == age[i]);
        CHECK(items[i].contrast == 0 && items[i].color == 0);   /* untouched */
        CHECK(age[i] >= 0 && age[i] <= 0.5f + 1e-6f);
        CHECK(sig[i] == (ev[i] & 1));
        if (sig[i]) CHECK(fabsf(dir[i] - 315.0f) < 1e-4f);
    }
    n = 0;
    for (i = 0; i < 50; i++) n += sig[i];
    CHECK(n == 25);
    p = yrdk_params(&n);
    CHECK(n == 3);
    CHECK(strcmp(p[0].name, "coherence") == 0 && p[0].offset == offsetof(yrdk_field, coherence));
    CHECK(strcmp(p[1].name, "direction") == 0 && p[1].offset == offsetof(yrdk_field, direction));
    CHECK(strcmp(p[2].name, "speed") == 0 && p[2].offset == offsetof(yrdk_field, speed));
    yrdk_close(&f);
}

/* v0.2.0: open returns bool, close is safe, failing calls write the message. */
static void test_conventions(void) {
    static yrdk_field z, f;
    yrdk_desc d = base_desc();
    yrdk_out o;
    float xy[2 * 8];
    g_where = "conventions";
    memset(&z, 0, sizeof z);
    yrdk_close(&z);                      /* zeroed, twice, NULL */
    yrdk_close(&z);
    yrdk_close(NULL);
    CHECK(yrdk_update(&z, 0) == YRDK_ERR_ARG);
    CHECK(strstr(yrdk_error(&z), "yrdk_update") != NULL);
    d.w = 10;                            /* no count or density: refused */
    CHECK(!yrdk_open(&f, &d));
    CHECK(strstr(yrdk_error(&f), "count") != NULL);
    yrdk_close(&f);                      /* after a failed open */
    d.count = 8;
    d.lifetime_ns = YRT_MS(300);
    CHECK(yrdk_open(&f, &d));
    CHECK(yrdk_error(&f)[0] == 0);
    CHECK(yrdk_xy(&f, xy) == YRDK_ERR_ORDER);
    CHECK(strstr(yrdk_error(&f), "before yrdk_start") != NULL);
    CHECK(yrdk_write(&f, NULL) == YRDK_ERR_ARG);
    CHECK(strstr(yrdk_error(&f), "out is NULL") != NULL);
    CHECK(yrdk_start(&f, 1, 1000) == YRDK_OK);
    CHECK(yrdk_update(&f, 999) == YRDK_ERR_ORDER);
    CHECK(strstr(yrdk_error(&f), "before the last update") != NULL);
    CHECK(yrdk_snapshot(&f, NULL, 0) == YRDK_ERR_ARG);
    CHECK(strstr(yrdk_error(&f), "yrdk_snapshot") != NULL);
    CHECK(yrdk_replay(&f, 1, 0, NULL, 3) == YRDK_ERR_ARG);
    CHECK(strstr(yrdk_error(&f), "steps is NULL") != NULL);
    memset(&o, 0, sizeof o);
    o.xy = xy;
    CHECK(yrdk_write(&f, &o) == 8);
    yrdk_close(&f);
    yrdk_close(&f);
    CHECK(yrdk_update(&f, 2000) == YRDK_ERR_ARG);
}

int main(void) {
    const char* v = getenv("YRDK_TEST_VERBOSE");
    g_verbose = v && *v && *v != '0';
    test_conventions();
    test_exact();
    /* The mutation runner sets this to see which property check catches a
     * fault that the digests would catch anyway. */
    if (!getenv("YRDK_TEST_NO_GOLDEN")) test_golden();
    test_refusals();
    test_coherence();
    test_density();
    test_lifetime();
    test_displacement();
    test_noise();
    test_frozen();
    test_drops();
    test_sets();
    test_least_recent();
    test_edges();
    test_repro();
    test_outputs();
    if (g_failures) { fprintf(stderr, "rdk_test: %d failures\n", g_failures); return 1; }
    printf("rdk_test: all checks passed\n");
    return 0;
}
