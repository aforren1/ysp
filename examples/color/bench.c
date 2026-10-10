/* color_bench.c - the cost of ysp/color.h's conversions on this machine.
 *
 * Conversion cost matters only where a frame converts colors: a color bound
 * to timeline channels (a DKL azimuth tween), one ycol_to_dir() per
 * element per frame, or the designer's color picker redrawing a gamut ring.
 * This program measures those against stated bars:
 *   to_dir   DKL spherical to a direction, one call: bar 100 ns, so 10000
 *            instanced elements cost 1 ms of a 16 ms frame
 *   ring     ycol_max_ring() with 360 hues in OkLCh (bisection): bar 16 ms
 * and prints the rest for the record: every space to and from RGB, one call
 * and in batches of 10000 through ycol_convert_n(), a context init with
 * the 2 and the 10 degree cones, and the DKL ring.
 *
 * The calibration is nominal (sRGB primaries, gamma 2.2, Gaussian spectra);
 * the cost does not depend on its values. Times are ysp/rt.h clock reads
 * around a loop, divided by its count: the median of 9 rounds. Run it on a
 * quiet machine; CI builds it and does not run it.
 *
 * Build (from the repository root):
 *     cc -O2 -Iinclude -o color_bench examples/color/bench.c -lm
 *     cl /O2 /Iinclude examples\color\bench.c
 *
 * Usage: color_bench [scale]   (scale multiplies the loop counts; default 1)
 * Exit code: 0, 1 if a call failed, 2 for a bad argument.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YRT_NO_THREADS
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"

#define YSP_COLOR_IMPLEMENTATION
#include "ysp/color.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROUNDS 9
#define BATCH 10000

static ycol_cal cal;
static double in_buf[3 * BATCH], out_buf[3 * BATCH];
static volatile double g_sink;

static int make_cal(void) {
    static const float xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };
    static float r[401], g[401], b[401];
    char err[200];
    int i;
    if (ycol_cal_nominal(&cal, xy, 100.0f, 2.2) < 0) return -1;
    for (i = 0; i < 401; i++) {
        double nm = 380 + i;
        r[i] = (float)(0.010 * exp(-0.5 * pow((nm - 612) / 14.0, 2)));
        g[i] = (float)(0.012 * exp(-0.5 * pow((nm - 545) / 24.0, 2)));
        b[i] = (float)(0.014 * exp(-0.5 * pow((nm - 455) / 12.0, 2)));
    }
    if (ycol_cal_set_spectra_nominal(&cal, 380, 1, 401, r, g, b, NULL) < 0) return -1;
    return ycol_cal_derive(&cal, err, sizeof err) < 0 ? -1 : 0;
}

static int cmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static double median(double* v) {
    qsort(v, ROUNDS, sizeof v[0], cmp);
    return v[ROUNDS / 2];
}

int main(int argc, char** argv) {
    ycol_ctx cx;
    ycol_ctx_desc d;
    double t[ROUNDS], scale = 1;
    long n_one;
    int s, r, i, fail = 0;
    if (argc > 2 || (argc == 2 && !((scale = atof(argv[1])) > 0 && scale <= 1000))) {
        fprintf(stderr, "usage: color_bench [scale]\n");
        return 2;
    }
    n_one = (long)(100000 * scale);
    if (make_cal() < 0) { fprintf(stderr, "color_bench: calibration failed\n"); return 1; }
    memset(&d, 0, sizeof d);
    d.cal = &cal;
    d.background[0] = d.background[1] = d.background[2] = 0.5;
    for (s = 0; s < 2; s++) {
        d.cones = s;
        for (r = 0; r < ROUNDS; r++) {
            uint64_t t0 = yrt_now_ns();
            if (!ycol_ctx_open(&cx, &d)) fail = 1;
            t[r] = (double)(yrt_now_ns() - t0);
        }
        printf("ctx open, %s cones: %.1f us\n", s ? "SS10" : "SS2", median(t) / 1000.0);
    }
    d.cones = 0;
    if (!ycol_ctx_open(&cx, &d)) { fprintf(stderr, "color_bench: %s\n", ycol_ctx_error(&cx)); return 1; }

    for (r = 0; r < ROUNDS; r++) {   /* the per-element case */
        uint64_t t0 = yrt_now_ns();
        double acc = 0;
        long k;
        for (k = 0; k < n_one; k++) {
            ycol_rgb v = ycol_to_dir(&cx, ycol_make(YCOL_SPACE_DKL, 0, (double)(k % 360), 0.05), NULL);
            acc += v.r;
        }
        g_sink = acc;
        t[r] = (double)(yrt_now_ns() - t0) / (double)n_one;
    }
    printf("to_dir, DKL spherical, one call: %.1f ns (bar 100 ns)\n", median(t));

    printf("%-11s %12s %12s %16s %16s\n", "space", "from rgb ns", "to rgb ns", "rgb->x n=1e4 us", "x->rgb n=1e4 us");
    for (i = 0; i < 3 * BATCH; i++) in_buf[i] = (double)((i * 7919) % 1000) / 1000.0;
    for (s = YCOL_SPACE_RGB; s < YCOL_SPACE_COUNT; s++) {
        double tf[ROUNDS], tt[ROUNDS], bf[ROUNDS], bt[ROUNDS];
        int nn;
        const ycol_space_info* si = ycol_spaces(&nn);
        for (r = 0; r < ROUNDS; r++) {
            uint64_t t0;
            double acc = 0;
            long k, nk = n_one / 10;
            int st;
            ycol_rgb v0;
            ycol_color c;
            t0 = yrt_now_ns();
            for (k = 0; k < nk; k++) {
                ycol_rgb v;
                v.r = 0.2 + (double)(k & 255) / 512.0; v.g = 0.4; v.b = 0.6;
                c = ycol_from_rgb(&cx, v, s, &st);
                acc += c.u.v[0];
            }
            tf[r] = (double)(yrt_now_ns() - t0) / (double)nk;
            v0.r = 0.2; v0.g = 0.4; v0.b = 0.6;
            c = ycol_from_rgb(&cx, v0, s, &st);
            if (st < 0) fail = 1;
            t0 = yrt_now_ns();
            for (k = 0; k < nk; k++) {
                ycol_rgb v = ycol_to_rgb(&cx, c, NULL);
                acc += v.r;
            }
            tt[r] = (double)(yrt_now_ns() - t0) / (double)nk;
            t0 = yrt_now_ns();
            if (ycol_convert_n(&cx, YCOL_SPACE_RGB, in_buf, 0, s, out_buf, 0, BATCH, NULL) != BATCH) fail = 1;
            bf[r] = (double)(yrt_now_ns() - t0) / 1000.0;
            t0 = yrt_now_ns();
            if (ycol_convert_n(&cx, s, out_buf, 0, YCOL_SPACE_RGB, in_buf, 0, BATCH, NULL) != BATCH) fail = 1;
            bt[r] = (double)(yrt_now_ns() - t0) / 1000.0;
            g_sink = acc;
        }
        printf("%-11s %12.1f %12.1f %16.1f %16.1f\n", si[s - 1].name, median(tf), median(tt), median(bf), median(bt));
    }
    {
        static double ring[360];
        for (r = 0; r < ROUNDS; r++) {
            uint64_t t0 = yrt_now_ns();
            if (ycol_max_ring(&cx, YCOL_PLANE_OKLCH, 0.7, 0, 360, ring) < 0) fail = 1;
            t[r] = (double)(yrt_now_ns() - t0) / 1e6;
        }
        printf("max_ring OkLCh, 360 hues: %.2f ms (bar 16 ms)\n", median(t));
        for (r = 0; r < ROUNDS; r++) {
            uint64_t t0 = yrt_now_ns();
            if (ycol_max_ring(&cx, YCOL_PLANE_DKL, 0, YCOL_SYMMETRIC, 360, ring) < 0) fail = 1;
            t[r] = (double)(yrt_now_ns() - t0) / 1e6;
        }
        printf("max_ring DKL, 360 azimuths: %.3f ms\n", median(t));
    }
    if (fail) fprintf(stderr, "color_bench: a call failed\n");
    return fail;
}
