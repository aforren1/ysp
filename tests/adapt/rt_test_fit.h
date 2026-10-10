/* rt_test_fit.h - the device clock fit (yrt_fit, DEVICE CLOCK FIT),
 * included by rt_test.c, which provides CHECK, CHECK_I and g_failures.
 *
 * Every scenario is synthetic and seeded, so a run is reproducible to the
 * bit. A simulated device counts ticks of its own crystal: tick u happens at
 * true host time T0 + u * tick * (1 + ppm / 1e6). The host sees each pair
 * late by a delay drawn from a model of the path (a USB frame's phase, an
 * exponential tail, bursts behind an FTDI latency timer, OS stalls), or, for
 * BRACKET, reads the device inside a bracket of known width. The reference
 * is the true line itself, computed in long double; the fit's error is its
 * map minus that line at the tick of each new pair, the moment a device
 * layer maps an event. A second reference, a brute-force envelope over every
 * pair of points (and the closed form of least squares), checks that the
 * estimator is the one the manual names.
 *
 * The bounds the checks use cover the measured extremes of these seeds and of
 * three other seed sets (+100, +200, +300), with a margin; docs/rt.md has
 * the table. YRT_TEST_FIT_ONLY runs only this
 * part (the mutation runs use it). */
#include <math.h>

/* ------------------------------------------------------------ generators */

typedef struct fit_rng { uint64_t s; } fit_rng;

static uint64_t fit_next(fit_rng* r) {     /* splitmix64 */
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static double fit_u01(fit_rng* r) {         /* (0, 1] */
    return ((double)(fit_next(r) >> 11) + 1.0) / 9007199254740992.0;
}

static double fit_exp(fit_rng* r, double mean) { return -mean * log(fit_u01(r)); }

/* A simulated device clock and the host's view of it. */
typedef struct fit_dev {
    long double t0;      /* true host ns at tick 0                    */
    long double tick;    /* true ns per tick: nominal * (1 + ppm/1e6)  */
    uint64_t    u0;      /* the counter at true time t0 (before wraps) */
    uint32_t    bits;    /* counter width; 0 = 64                      */
} fit_dev;

static uint64_t fit_wrap(const fit_dev* d, uint64_t u) {
    return (d->bits && d->bits < 64) ? (u & ((1ull << d->bits) - 1u)) : u;
}

/* The device's unwrapped tick count at true host time h (floor). */
static uint64_t fit_tick_at(const fit_dev* d, long double h) {
    return d->u0 + (uint64_t)((h - d->t0) / d->tick);
}

/* The true host time of an unwrapped tick count. */
static long double fit_true(const fit_dev* d, uint64_t u) {
    return d->t0 + (long double)(int64_t)(u - d->u0) * d->tick;
}

/* USB-like lateness: the frame's phase, an exponential tail, sometimes a
 * burst behind a 16 ms latency timer, rarely an OS stall. */
typedef struct fit_path {
    double frame_ns, tail_ns;
    double burst_p;   int burst_len; double burst_ns;
    double stall_p;   double stall_lo, stall_hi;
    int    burst_left;
} fit_path;

static double fit_delay(fit_rng* r, fit_path* p) {
    double d = fit_u01(r) * p->frame_ns + fit_exp(r, p->tail_ns);
    if (p->burst_left > 0) { p->burst_left--; d += fit_u01(r) * p->burst_ns; }
    else if (fit_u01(r) < p->burst_p) { p->burst_left = p->burst_len; d += fit_u01(r) * p->burst_ns; }
    if (fit_u01(r) < p->stall_p) d += p->stall_lo + fit_u01(r) * (p->stall_hi - p->stall_lo);
    return d;
}

/* The error statistics of one run, in ns. */
typedef struct fit_stats {
    double   lo, hi;          /* map minus truth after the warm-up          */
    double   slo, shi;        /* the same from 205 s on (a full window of
                               * 200 ms buckets)                            */
    int      restarts;        /* RESTART bits seen                          */
    int64_t  restart_at;      /* host ns of the first RESTART                */
    int      n;               /* checked pairs                              */
} fit_stats;

static void fit_stats_init(fit_stats* s) {
    s->lo = 1e300; s->hi = -1e300; s->slo = 1e300; s->shi = -1e300; s->restarts = 0; s->restart_at = 0; s->n = 0;
}

static void fit_stats_add(fit_stats* s, double e, double h) {
    if (e < s->lo) s->lo = e;
    if (e > s->hi) s->hi = e;
    if (h >= 205e9) {
        if (e < s->slo) s->slo = e;
        if (e > s->shi) s->shi = e;
    }
    s->n++;
}

/* Feed pairs every `period` ns (with +-jitter) of true time for `secs`; check
 * the map at each pair's own tick from `warm` s on. */
typedef struct fit_run {
    yrt_fit_desc desc;
    fit_dev  dev;
    fit_path path;
    double   period_ns, period_jitter_ns;
    double   secs, warm;
    uint64_t seed;
    double   reset_at;        /* the counter restarts from 0 here; 0 = never */
    double   stall_at, stall_len;  /* pairs during it arrive at its end   */
} fit_run;

static yrt_fit g_fit;   /* 40 KB: static, not on the stack */

static void fit_simulate(fit_run* run, fit_stats* st, yrt_fit_info* info) {
    fit_rng r = { run->seed };
    fit_dev dev = run->dev;
    double  h = 0.0;
    bool    reset_done = false;
    fit_stats_init(st);
    CHECK(yrt_fit_open(&g_fit, &run->desc), "fit: init");
    while (h < run->secs * 1e9) {
        uint64_t u;
        int64_t host, m;
        double d;
        int rc;
        h += run->period_ns + (fit_u01(&r) * 2.0 - 1.0) * run->period_jitter_ns;
        if (run->reset_at > 0 && !reset_done && h >= run->reset_at * 1e9) {
            /* the counter starts again at 0 now: a new t0 */
            dev.u0 = 0;
            dev.t0 = (long double)h;
            reset_done = true;
        }
        /* the event at true time h; the host clock reads 1000 s more */
        u = fit_tick_at(&dev, (long double)h);
        d = fit_delay(&r, &run->path);
        host = (int64_t)((long double)h + (long double)d + 1e12L);
        if (run->stall_len > 0 && h >= run->stall_at * 1e9 && h < (run->stall_at + run->stall_len) * 1e9)
            host = (int64_t)((run->stall_at + run->stall_len) * 1e9 + 1e12 + fit_u01(&r) * 50000.0);
        rc = yrt_fit_add(&g_fit, fit_wrap(&dev, u), host, 0);
        if (rc & YRT_FIT_RESTART) {
            if (!st->restarts) st->restart_at = host - (int64_t)1e12;
            st->restarts++;
        }
        if (h >= run->warm * 1e9 && !(run->reset_at > 0 && h >= run->reset_at * 1e9 &&
                                      h < (run->reset_at + run->warm) * 1e9)) {
            m = yrt_fit_map(&g_fit, fit_wrap(&dev, u));
            fit_stats_add(st, (double)((long double)m - (fit_true(&dev, u) + 1e12L)), h);
        }
    }
    yrt_fit_get(&g_fit, info);
}

/* ------------------------------------------------------------ scenarios */

static fit_path fit_usb_path(void) {
    fit_path p;
    memset(&p, 0, sizeof p);
    p.frame_ns = 1e6;        /* full-speed USB frame */
    p.tail_ns = 1e5;         /* driver and wake-up   */
    p.burst_p = 0.01; p.burst_len = 10; p.burst_ns = 16e6;
    p.stall_p = 0.002; p.stall_lo = 60e6; p.stall_hi = 200e6;
    return p;
}

static void fit_report(const char* name, const fit_stats* s, const yrt_fit_info* in) {
    printf("  fit %-38s err %+8.1f .. %+8.1f us (after 205 s %+8.1f .. %+8.1f), ppm %+8.2f, "
           "spread %6.1f us, points %4u, epoch %u, rejected %u\n", name, s->lo / 1e3, s->hi / 1e3,
           s->slo / 1e3, s->shi / 1e3, in->ppm,
           (double)in->spread_ns / 1e3, in->points, in->epoch, in->rejected);
}

/* A ms box clock at 100 pairs a second, 100 ppm fast, over 10 minutes. */
static void test_fit_late_ms(void) {
    fit_run run;
    fit_stats st;
    yrt_fit_info in;
    memset(&run, 0, sizeof run);
    run.desc.mode = YRT_FIT_LATE;
    run.desc.ns_per_tick = 1e6;
    run.desc.tick_bits = 32;
    run.dev.t0 = 0; run.dev.u0 = 123456; run.dev.bits = 32;
    run.dev.tick = 1e6L * (1.0L + 100e-6L);
    run.path = fit_usb_path();
    run.period_ns = 10e6; run.period_jitter_ns = 3e6;
    run.secs = 600; run.warm = 10; run.seed = 1;
    fit_simulate(&run, &st, &in);
    fit_report("LATE ms, 100 Hz, +100 ppm", &st, &in);
    /* measured with seed 1: -75..+130 us, -47..+75 after 205 s */
    CHECK(st.lo > -150e3 && st.hi < 250e3, "fit LATE ms: map within -0.15..+0.25 ms of the tick's start");
    CHECK(st.slo > -100e3 && st.shi < 150e3, "fit LATE ms: within -0.1..+0.15 ms after 205 s");
    CHECK(in.slope && fabs(in.ppm - 100.0) < 5.0, "fit LATE ms: rate within 5 ppm");
    CHECK_I(in.epoch, 0, "fit LATE ms: stalls must not restart the fit");
    CHECK(in.rejected > 0, "fit LATE ms: stalls are rejected, not points");
    CHECK_I(in.points, YRT_FIT_POINTS, "fit LATE ms: the window is full after 600 s");
}

/* An Arduino micros() counter that wraps at 2^32 us (71.6 min) 200 s into
 * the run, 80 ppm slow. */
static void test_fit_late_us_wrap(void) {
    fit_run run;
    fit_stats st;
    yrt_fit_info in;
    memset(&run, 0, sizeof run);
    run.desc.mode = YRT_FIT_LATE;
    run.desc.ns_per_tick = 1e3;
    run.desc.tick_bits = 32;
    run.dev.t0 = 0; run.dev.bits = 32;
    run.dev.tick = 1e3L * (1.0L - 80e-6L);
    run.dev.u0 = (1ull << 32) - 200000000ull;
    run.path = fit_usb_path();
    run.period_ns = 10e6; run.period_jitter_ns = 3e6;
    run.secs = 600; run.warm = 10; run.seed = 2;
    fit_simulate(&run, &st, &in);
    fit_report("LATE us, wraps at 200 s, -80 ppm", &st, &in);
    /* measured with seed 2: -2.7..+37 us, -2.2..+9.1 after 205 s */
    CHECK(st.lo > -20e3 && st.hi < 80e3, "fit LATE us: map within -20..+80 us across the wrap");
    CHECK(st.slo > -10e3 && st.shi < 25e3, "fit LATE us: within -10..+25 us after 205 s");
    CHECK(in.slope && fabs(in.ppm + 80.0) < 5.0, "fit LATE us: rate within 5 ppm");
    CHECK_I(in.epoch, 0, "fit LATE us: a wrap is not a restart");
    CHECK(in.ticks0 > (1ull << 32), "fit LATE us: ticks unwrap past 2^32");
}

/* The counter is reset (XID's e5) 300 s in. */
static void test_fit_restart(void) {
    fit_run run;
    fit_stats st;
    yrt_fit_info in;
    memset(&run, 0, sizeof run);
    run.desc.mode = YRT_FIT_LATE;
    run.desc.ns_per_tick = 1e6;
    run.desc.tick_bits = 32;
    run.dev.t0 = 0; run.dev.u0 = 50000000; run.dev.bits = 32;
    run.dev.tick = 1e6L * (1.0L + 30e-6L);
    run.path = fit_usb_path();
    run.path.stall_p = 0;   /* a clean path: the restart's timing is the check */
    run.period_ns = 10e6; run.period_jitter_ns = 3e6;
    run.secs = 400; run.warm = 10; run.seed = 3;
    run.reset_at = 300;
    fit_simulate(&run, &st, &in);
    fit_report("LATE ms, reset at 300 s", &st, &in);
    CHECK_I(st.restarts, 1, "fit restart: one RESTART");
    CHECK(st.restart_at >= (int64_t)301.0e9 && st.restart_at <= (int64_t)301.3e9,
          "fit restart: within the 1 s hold of the reset");
    CHECK_I(in.epoch, 1, "fit restart: epoch 1");
    /* measured with seed 3: -35..+284 us, the warm-up after the reset excluded */
    CHECK(st.lo > -150e3 && st.hi < 500e3, "fit restart: map bounded before and after");
}

/* A 300 ms stall at 200 s: 30 pairs arrive together at its end. */
static void test_fit_stall(void) {
    fit_run run;
    fit_stats st;
    yrt_fit_info in;
    memset(&run, 0, sizeof run);
    run.desc.mode = YRT_FIT_LATE;
    run.desc.ns_per_tick = 1e6;
    run.desc.tick_bits = 32;
    run.dev.t0 = 0; run.dev.u0 = 7; run.dev.bits = 32;
    run.dev.tick = 1e6L * (1.0L - 20e-6L);
    run.path = fit_usb_path();
    run.period_ns = 10e6; run.period_jitter_ns = 3e6;
    run.secs = 300; run.warm = 10; run.seed = 4;
    run.stall_at = 200; run.stall_len = 0.3;
    fit_simulate(&run, &st, &in);
    fit_report("LATE ms, 300 ms stall at 200 s", &st, &in);
    CHECK_I(st.restarts, 0, "fit stall: a burst after a stall is not a restart");
    /* measured with seed 4: -59..+165 us */
    CHECK(st.lo > -150e3 && st.hi < 300e3, "fit stall: map bounded");
}

/* XID presses: a pair every 2 to 4 s, each behind a 16 ms latency timer.
 * Few points, so the envelope finds the timer's floor slowly. */
static void test_fit_sparse(void) {
    fit_run run;
    fit_stats st;
    yrt_fit_info in;
    memset(&run, 0, sizeof run);
    run.desc.mode = YRT_FIT_LATE;
    run.desc.ns_per_tick = 1e6;
    run.desc.tick_bits = 32;
    run.dev.t0 = 0; run.dev.u0 = 1000; run.dev.bits = 32;
    run.dev.tick = 1e6L * (1.0L + 50e-6L);
    memset(&run.path, 0, sizeof run.path);
    run.path.frame_ns = 16e6;   /* the FTDI latency timer's phase */
    run.path.tail_ns = 1e5;
    run.period_ns = 3e9; run.period_jitter_ns = 1e9;
    run.secs = 1200; run.warm = 120; run.seed = 5;
    fit_simulate(&run, &st, &in);
    fit_report("LATE ms, sparse presses, 16 ms timer", &st, &in);
    /* measured with seed 5: up to +8.7 ms at 2 min (40 points), then -1.1..+1.3 ms
     * after 205 s: few points behind a 16 ms timer need BRACKET probes */
    CHECK(st.lo > -3e6 && st.hi < 15e6, "fit sparse: map within -3..+15 ms from 2 min");
    CHECK(st.slo > -3e6 && st.shi < 5e6, "fit sparse: within -3..+5 ms after 205 s");
    CHECK_I(in.epoch, 0, "fit sparse: no restart");
}

/* BRACKET: a timer query every 100 ms, the device's ms timer read at a
 * uniform moment inside a bracket of 0.4 to 3 ms; wider than 1.3 ms is
 * refused. */
static void test_fit_bracket(void) {
    fit_rng r = { 6 };
    fit_dev dev;
    fit_stats st;
    yrt_fit_info in;
    yrt_fit_desc d;
    double h = 0.0, lo_w = 1e300;
    int refused = 0, rc_refused = 0;
    memset(&d, 0, sizeof d);
    d.mode = YRT_FIT_BRACKET;
    d.ns_per_tick = 1e6;
    d.tick_bits = 32;
    dev.t0 = 0; dev.u0 = 99; dev.bits = 32; dev.tick = 1e6L * (1.0L + 70e-6L);
    fit_stats_init(&st);
    CHECK(yrt_fit_open(&g_fit, &d), "fit bracket: init");
    while (h < 600e9) {
        double w = 0.4e6 + fit_u01(&r) * 2.6e6;
        double t_read = h + fit_u01(&r) * w;      /* the device reads its timer here */
        uint64_t u = fit_tick_at(&dev, (long double)t_read);
        int64_t t1 = (int64_t)(h + w + 1e12);
        int rc = yrt_fit_add(&g_fit, fit_wrap(&dev, u), t1, (int64_t)w);
        if (w > 1.3e6) refused++;
        if (rc & YRT_FIT_REJECTED) rc_refused++;
        if (w < lo_w) lo_w = w;
        if (h >= 10e9 && !(rc & YRT_FIT_REJECTED)) {
            int64_t m = yrt_fit_map(&g_fit, fit_wrap(&dev, u));
            fit_stats_add(&st, (double)((long double)m - (fit_true(&dev, u) + 1e12L)), h);
        }
        h += 100e6;
    }
    yrt_fit_get(&g_fit, &in);
    fit_report("BRACKET ms, 0.4..3 ms brackets", &st, &in);
    CHECK_I(rc_refused, refused, "fit bracket: every bracket over 1.3 ms refused");
    CHECK_I(in.rejected, refused, "fit bracket: the count says so");
    CHECK(in.width_ns > 0 && in.width_ns <= 1300000, "fit bracket: width of the envelope's pairs");
    /* measured with seed 6: -327..+349 us (the first 10 s of slope), +57..+80 after 205 s */
    CHECK(st.lo > -600e3 && st.hi < 700e3, "fit bracket: map within -0.6..+0.7 ms from 10 s");
    CHECK(st.slo > -100e3 && st.shi < 200e3, "fit bracket: within -0.1..+0.2 ms after 205 s");
    CHECK(fabs(in.ppm - 70.0) < 5.0, "fit bracket: rate within 5 ppm");
}

/* UNBIASED: a 64-bit us counter stamped at the source with symmetric noise
 * (a sum of four uniforms, SD 115 us), 50 ppm slow. */
static void test_fit_unbiased(void) {
    fit_rng r = { 7 };
    fit_dev dev;
    fit_stats st;
    yrt_fit_info in;
    yrt_fit_desc d;
    double h = 0.0;
    memset(&d, 0, sizeof d);
    d.mode = YRT_FIT_UNBIASED;
    d.ns_per_tick = 1e3;
    dev.t0 = 0; dev.u0 = 5000000000ull; dev.bits = 0; dev.tick = 1e3L * (1.0L - 50e-6L);
    fit_stats_init(&st);
    CHECK(yrt_fit_open(&g_fit, &d), "fit unbiased: init");
    while (h < 600e9) {
        uint64_t u = fit_tick_at(&dev, (long double)h);
        double noise = (fit_u01(&r) + fit_u01(&r) + fit_u01(&r) + fit_u01(&r) - 2.0) * 200e3;
        int64_t host = (int64_t)(fit_true(&dev, u) + 1e12L + (long double)noise);
        (void)yrt_fit_add(&g_fit, u, host, 0);
        if (h >= 10e9) {
            int64_t m = yrt_fit_map(&g_fit, u);
            fit_stats_add(&st, (double)((long double)m - (fit_true(&dev, u) + 1e12L)), h);
        }
        h += 10e6;
    }
    yrt_fit_get(&g_fit, &in);
    fit_report("UNBIASED us, SD 115 us, -50 ppm", &st, &in);
    /* measured with seed 7: -10..+13 us, -2.2..+5.2 after 205 s */
    CHECK(st.lo > -30e3 && st.hi < 30e3, "fit unbiased: map within 30 us");
    CHECK(st.slo > -10e3 && st.shi < 10e3, "fit unbiased: within 10 us after 205 s");
    CHECK(fabs(in.ppm + 50.0) < 2.0, "fit unbiased: rate within 2 ppm");
    /* points are means of 20 pairs: SD 115 / sqrt(20) = 26 us, p99 about 67 */
    CHECK(in.spread_ns > 40000 && in.spread_ns < 120000, "fit unbiased: p99 |residual| of bucket means");
}

/* The estimator against brute force: one pair per bucket, so the points are
 * the pairs, and every candidate line through two points is tried. */
static void test_fit_reference(void) {
    enum { NP = 48 };
    fit_rng r = { 8 };
    yrt_fit_desc d;
    int64_t xs[NP], ys[NP];
    int i, j, q, mode;
    for (mode = 0; mode < 2; mode++) {
        double best = 1e300, ba = 0, bb = 0, x0, y0, xl, want, sx = 0, sy = 0, sxx = 0, sxy = 0;
        int64_t got;
        memset(&d, 0, sizeof d);
        d.mode = mode == 0 ? YRT_FIT_LATE : YRT_FIT_UNBIASED;
        d.ns_per_tick = 1e3;
        d.slope_span_ns = 1;   /* fit the slope at once */
        CHECK(yrt_fit_open(&g_fit, &d), "fit reference: init");
        for (i = 0; i < NP; i++) {
            /* 250 ms apart: one pair per 200 ms bucket */
            xs[i] = 1000000 + (int64_t)i * 250000 + (int64_t)(fit_u01(&r) * 1000.0);
            ys[i] = 5000000000LL + (int64_t)((double)(xs[i] - 1000000) * 1000.03)
                  + (int64_t)(mode == 0 ? fit_exp(&r, 3e5) : (fit_u01(&r) - 0.5) * 6e5);
            CHECK(yrt_fit_add(&g_fit, (uint64_t)xs[i], ys[i], 0) >= 0, "fit reference: add");
        }
        x0 = (double)xs[0]; y0 = (double)ys[0];
        xl = (double)xs[NP - 1] - x0;
        if (mode == 0) {
            for (i = 0; i < NP; i++) for (j = i + 1; j < NP; j++) {
                double xi = (double)xs[i] - x0, yi = (double)ys[i] - y0;
                double xj = (double)xs[j] - x0, yj = (double)ys[j] - y0;
                double b = (yj - yi) / (xj - xi), a = yi - b * xi, sum = 0;
                bool ok = true;
                for (q = 0; q < NP && ok; q++) {
                    double g = ((double)ys[q] - y0) - (a + b * ((double)xs[q] - x0));
                    if (g < -1e-3) ok = false;
                    sum += g;
                }
                if (ok && sum < best) { best = sum; ba = a; bb = b; }
            }
        } else {
            for (i = 0; i < NP; i++) {
                double x = (double)xs[i] - x0, y = (double)ys[i] - y0;
                sx += x; sy += y; sxx += x * x; sxy += x * y;
            }
            bb = (NP * sxy - sx * sy) / (NP * sxx - sx * sx);
            ba = (sy - bb * sx) / NP;
        }
        want = y0 + ba + bb * xl;
        got = yrt_fit_map(&g_fit, (uint64_t)xs[NP - 1]);
        printf("  fit reference %s: map %lld, brute force %.1f\n", mode == 0 ? "envelope" : "least squares",
               (long long)got, want);
        CHECK(fabs((double)got - want) <= 2.0, mode == 0 ? "fit reference: the envelope is the brute-force one"
                                                          : "fit reference: least squares is the closed form");
        got = yrt_fit_map(&g_fit, (uint64_t)xs[0]);
        CHECK(fabs((double)got - (y0 + ba)) <= 2.0, "fit reference: the line at the oldest point too");
    }
}

/* The records: one CLOCK per closed bucket, one FIT per refit at a close. */
static void test_fit_records(void) {
    static unsigned char mem[YRT_RING_BYTES(1024)];
    static yrt_event ev[1024];
    yrt_ring ring;
    yrt_fit_desc d;
    yrt_ring_desc rd;
    int i, n, clocks = 0, fits = 0, bad = 0;
    int64_t last_t = 0;
    memset(&rd, 0, sizeof rd);
    rd.memory = mem; rd.bytes = sizeof mem;
    CHECK(yrt_ring_open(&ring, &rd), "fit records: ring");
    memset(&d, 0, sizeof d);
    d.mode = YRT_FIT_LATE;
    d.ns_per_tick = 1e6;
    d.tick_bits = 32;
    d.clock_id = 77;
    d.ring = &ring;
    CHECK(yrt_fit_open(&g_fit, &d), "fit records: init");
    /* 30 s at 100 Hz from host 1000 s: 150 buckets of 200 ms */
    for (i = 0; i < 3000; i++)
        (void)yrt_fit_add(&g_fit, (uint64_t)(500 + i * 10), 1000000000000LL + (int64_t)i * 10000000LL + 300000, 0);
    n = yrt_ring_drain(&ring, ev, 1024);
    for (i = 0; i < n; i++) {
        if (ev[i].source != YRT_SRC_RT) continue;
        if (ev[i].aux != 77u) bad++;
        if (ev[i].kind == YRT_KIND_CLOCK) {
            clocks++;
            if (ev[i].t_ns < last_t) bad++;
            last_t = ev[i].t_ns;
            if ((ev[i].u.u64[0] - 500u) % 10u != 0 || ev[i].u.u64[2] != 0) bad++;
            if (ev[i].t_ns != 1000000000000LL + (int64_t)(ev[i].u.u64[0] - 500u) * 1000000 + 300000) bad++;
        } else if (ev[i].kind == YRT_KIND_FIT) {
            fits++;
            if (ev[i].u.f64[2] < 0.99e6 || ev[i].u.f64[2] > 1.01e6) bad++;
        }
    }
    CHECK_I(clocks, 149, "fit records: one CLOCK per closed bucket");
    CHECK_I(fits, 150, "fit records: one FIT at the first pair and per close");
    CHECK_I(bad, 0, "fit records: fields");
    printf("  fit records: %d CLOCK, %d FIT\n", clocks, fits);
}

static void test_fit_args(void) {
    yrt_fit_desc d;
    yrt_fit_info in;
    memset(&d, 0, sizeof d);
    CHECK(!yrt_fit_open(NULL, &d), "fit args: NULL fit");
    CHECK(!yrt_fit_open(&g_fit, NULL), "fit args: NULL desc");
    CHECK(!yrt_fit_open(&g_fit, &d), "fit args: no tick");
    d.ns_per_tick = 1e3;
    d.mode = 3;
    CHECK(!yrt_fit_open(&g_fit, &d), "fit args: unknown mode");
    d.mode = YRT_FIT_LATE;
    d.tick_bits = 65;
    CHECK(!yrt_fit_open(&g_fit, &d), "fit args: 65 bits");
    memset(&g_fit, 0, sizeof g_fit);
    CHECK(yrt_fit_add(&g_fit, 1, 1, 0) == YRT_ERR_ARG, "fit args: a zeroed fit refuses pairs");
    d.tick_bits = 16;
    CHECK(yrt_fit_open(&g_fit, &d), "fit args: 16 bits");
    CHECK(yrt_fit_map(&g_fit, 5) == 0, "fit args: no map before a pair");
    yrt_fit_get(&g_fit, &in);
    CHECK(!in.ready, "fit args: not ready before a pair");
    CHECK((yrt_fit_add(&g_fit, 65000, 10000000000LL, 0) & YRT_FIT_REFIT) != 0, "fit args: first pair maps");
    CHECK(yrt_fit_map(&g_fit, 65000) == 10000000000LL, "fit args: the first pair maps to itself");
    /* 50 ms before the epoch's first pair: bucket -1, before the newest point */
    CHECK(yrt_fit_add(&g_fit, 64950, 9950000000LL, 0) == YRT_FIT_REJECTED,
          "fit args: a pair before the first bucket is refused");
    /* 16-bit us counter: 600 ticks later it reads 64, past the wrap */
    CHECK(yrt_fit_unwrap(&g_fit, 64) == 65600u, "fit args: unwrap past 2^16");
    CHECK(yrt_fit_map(&g_fit, 64) == 10000600000LL, "fit args: map past 2^16");
    CHECK(yrt_fit_unwrap(&g_fit, 64900) == 64900u, "fit args: no unwrap backward within half a period");
    /* a pair from an earlier bucket than the newest point */
    CHECK((yrt_fit_add(&g_fit, 65400, 10400000000LL, 0) & YRT_FIT_POINT) != 0, "fit args: a later pair");
    CHECK(yrt_fit_add(&g_fit, 65300, 10000300000LL, 0) == YRT_FIT_REJECTED,
          "fit args: an out-of-order pair is refused");
    yrt_fit_get(&g_fit, &in);
    CHECK(in.ready && in.pairs == 4 && in.rejected == 2, "fit args: counts");
    CHECK(yrt_fit_error(&g_fit)[0] == '\0', "fit args: no message after an open that worked");
    yrt_fit_close(&g_fit);
    CHECK(yrt_fit_add(&g_fit, 65500, 10500000000LL, 0) == YRT_ERR_ARG, "fit args: a closed fit refuses pairs");
    yrt_fit_get(&g_fit, &in);
    CHECK(!in.ready, "fit args: a closed fit has no map");
    yrt_fit_close(&g_fit);
    d.ns_per_tick = 0.0;
    CHECK(!yrt_fit_open(&g_fit, &d) && strstr(yrt_fit_error(&g_fit), "ns_per_tick") != NULL,
          "fit args: the message names the field");
}

/* The first 5 s, before the slope is fitted: a 500 ppm clock (a ceramic
 * resonator is worse) at 100 pairs a second. The map at each pair's tick,
 * and 200 ms past it, where events between pairs land. */
static void test_fit_early(void) {
    fit_rng r = { 9 };
    fit_dev dev;
    yrt_fit_desc d;
    fit_path path;
    double h = 0.0, lo = 1e300, hi = -1e300, alo = 1e300, ahi = -1e300, e;
    memset(&d, 0, sizeof d);
    d.mode = YRT_FIT_LATE;
    d.ns_per_tick = 1e3;
    dev.t0 = 0; dev.u0 = 0; dev.bits = 0; dev.tick = 1e3L * (1.0L + 500e-6L);
    memset(&path, 0, sizeof path);
    path.frame_ns = 1e6; path.tail_ns = 1e5;
    CHECK(yrt_fit_open(&g_fit, &d), "fit early: init");
    while (h < 4.9e9) {
        uint64_t u;
        h += 10e6;
        u = fit_tick_at(&dev, (long double)h);
        (void)yrt_fit_add(&g_fit, u, (int64_t)((long double)h + (long double)fit_delay(&r, &path) + 1e12L), 0);
        if (h < 0.3e9) continue;
        e = (double)((long double)yrt_fit_map(&g_fit, u) - (fit_true(&dev, u) + 1e12L));
        if (e < lo) lo = e;
        if (e > hi) hi = e;
        e = (double)((long double)yrt_fit_map(&g_fit, u + 200000u) - (fit_true(&dev, u + 200000u) + 1e12L));
        if (e < alo) alo = e;
        if (e > ahi) ahi = e;
    }
    printf("  fit early (0.3..4.9 s, +500 ppm): err %+.1f .. %+.1f us; 200 ms ahead %+.1f .. %+.1f us\n",
           lo / 1e3, hi / 1e3, alo / 1e3, ahi / 1e3);
    CHECK(lo > -800e3 && hi < 800e3, "fit early: the offset follows the drift");
    CHECK(alo > -800e3 && ahi < 800e3, "fit early: the nominal slope 200 ms ahead");
}

static void test_fit(void) {
    test_fit_args();
    test_fit_reference();
    test_fit_records();
    test_fit_early();
    test_fit_late_ms();
    test_fit_late_us_wrap();
    test_fit_restart();
    test_fit_stall();
    test_fit_sparse();
    test_fit_bracket();
    test_fit_unbiased();
}
