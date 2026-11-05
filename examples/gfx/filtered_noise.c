/* filtered_noise.c - band-pass, orientation-filtered and 1/f noise made on
 * the CPU each trial and uploaded as textures, against ysp/gfx.h's NOISE
 * kinds; the generation and upload cost against the 16 ms frame.
 *
 * It does what Psychtoolbox-3's FastFilteredNoiseDemo.m shows (noise
 * filtered each frame, as a benchmark), with the filter on the CPU: ysp/gfx.h
 * has no GPU filter pass (docs/rig_spec.md 16.5, rank 7). PTB is MIT; its
 * files were read for coverage, no code was copied.
 *
 * Each trial (--trial-frames frames, default 30; --every-frame for 1) makes
 * two N x N fields (N = --size, default 256) from white Gaussian noise
 * (ygfx_noise_gauss()), each by a 2-D FFT, a filter and the inverse FFT, in
 * single precision, with no allocation after start:
 *   band    a log-Gaussian band, 1 octave wide (full width at half height)
 *           about 1/16 cycle per px, times a Gaussian of 30 deg in
 *           orientation about vertical bars;
 *   1/f     amplitude 1/f (power 1/f^2), as in natural images.
 * Each is scaled to SD 1, clipped at 4.5 SD and drawn as a modulation IMAGE
 * (R32F) at an RMS contrast of 0.15. Beside them, the two built-in kinds:
 * NOISE GAUSSIAN at 1 px checks and NOISE SIMPLEX (1 octave, its lattice
 * set so its spectral peak, 0.64 of the lattice frequency, is at 1/16).
 *
 * Measured and printed: per trial, the CPU time to make both fields and to
 * upload both (ygfx_texture_update()), against the frame period; the flip
 * records' drops and SWAP phase on trial frames and on other frames. And,
 * from each field's own spectrum (the same FFT): the fraction of its power
 * inside the band (1 octave by 30 deg about the filter's center) and the
 * power along the filter's orientation over the power across it. NOISE's
 * kinds are not band-limited (ysp/gfx.h NOISE): these numbers say by how
 * much. The GPU time of the draws is not measured (the flip record's GPU
 * phase is unknown in this ysp/screen.h); SWAP holds the wait for the GPU.
 *
 * --sim: the simulated display and the null backend, 4 trials. Checks (in
 * every mode): the FFT round trip within 1e-4, each field's SD 1, the band
 * field's in-band fraction above 0.6 and its orientation ratio above 10, the
 * white field's in-band fraction below 0.1 and SIMPLEX's below half the band
 * field's. Exits 1 on a mismatch.
 *
 * Usage: gfx_filtered_noise [--sim] [--fullscreen] [--seconds S] [--size N]
 *                           [--trial-frames N] [--every-frame]
 *   --seconds S  run S seconds (default 5); --size N 64 to 512, a power of 2
 * Exit code: 0; 1 when something did not open or a check failed; 2 for a bad
 * argument. On Windows set YSCR_ANGLE_DIR.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NMAX 512
#define MAXT 4096
#define F0 (1.0 / 16)       /* the band's center, cycles per px */
#define OCT 1.0             /* its full width at half height, octaves */
#define ORI_W 30.0          /* and in orientation, degrees */
#define CLIP 4.5
#define RMS 0.15
#define PI 3.14159265358979323846

static int N, LOG2N;
static float re[NMAX * NMAX], im[NMAX * NMAX], col_re[NMAX], col_im[NMAX];
static float tw_re[NMAX / 2], tw_im[NMAX / 2], filt[2][NMAX * NMAX];
static float field[2][NMAX * NMAX], spare[NMAX * NMAX];
static int bitrev[NMAX];
static double gen_ms[MAXT], up_ms[MAXT], part_ms[3];   /* white, FFT and filter, scaling */

/* In place, radix 2; sign -1 forward, +1 inverse (unscaled). */
static void fft1(float* xr, float* xi, int sign) {
    int i, j, len;
    for (i = 0; i < N; i++)
        if (bitrev[i] > i) {
            float t = xr[i]; xr[i] = xr[bitrev[i]]; xr[bitrev[i]] = t;
            t = xi[i]; xi[i] = xi[bitrev[i]]; xi[bitrev[i]] = t;
        }
    for (len = 2; len <= N; len <<= 1) {
        int step = N / len;
        for (i = 0; i < N; i += len)
            for (j = 0; j < len / 2; j++) {
                float wr = tw_re[j * step], wi = sign * tw_im[j * step];
                float* ar = &xr[i + j]; float* ai = &xi[i + j];
                float* br = &xr[i + j + len / 2]; float* bi = &xi[i + j + len / 2];
                float tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;
                *br = *ar - tr; *bi = *ai - ti; *ar += tr; *ai += ti;
            }
    }
}
/* Rows, then columns through a contiguous copy (the cache-friendly order). */
static void fft2(int sign) {
    int r, c;
    for (r = 0; r < N; r++) fft1(&re[r * N], &im[r * N], sign);
    for (c = 0; c < N; c++) {
        for (r = 0; r < N; r++) { col_re[r] = re[r * N + c]; col_im[r] = im[r * N + c]; }
        fft1(col_re, col_im, sign);
        for (r = 0; r < N; r++) { re[r * N + c] = col_re[r]; im[r * N + c] = col_im[r]; }
    }
}
static void freq(int u, int v, double* f, double* ang) {
    double fu = (u < N / 2 ? u : u - N) / (double)N, fv = (v < N / 2 ? v : v - N) / (double)N;
    *f = hypot(fu, fv);
    *ang = atan2(fv, fu) * 180 / PI;   /* 0: the frequency along x, so vertical bars */
}
static double ang_diff(double a) {   /* orientation: modulo 180 */
    a = fmod(fabs(a), 180.0);
    return a > 90 ? 180 - a : a;
}
static void setup(void) {
    int i, b, u, v;
    for (i = 0; i < N / 2; i++) { tw_re[i] = (float)cos(2 * PI * i / N); tw_im[i] = (float)-sin(2 * PI * i / N); }
    for (i = 0; i < N; i++) { int r = 0; for (b = 0; b < LOG2N; b++) r |= ((i >> b) & 1) << (LOG2N - 1 - b); bitrev[i] = r; }
    for (v = 0; v < N; v++)
        for (u = 0; u < N; u++) {
            double f, a, so = OCT / 2.3548, sa = ORI_W / 2.3548;
            freq(u, v, &f, &a);
            filt[0][v * N + u] = f == 0 ? 0.0f : (float)(exp(-pow(log2(f / F0), 2) / (2 * so * so)) *
                                                          exp(-pow(ang_diff(a), 2) / (2 * sa * sa)));
            filt[1][v * N + u] = f == 0 ? 0.0f : (float)(1.0 / (f * N));
        }
}
/* White noise, filtered, scaled to SD 1 (sd_out), clipped and divided by
 * CLIP so the IMAGE's g stays in [-1, 1]. */
static void make(int kind, uint32_t seed, float* out, double* sd_out) {
    int i;
    double s = 0, s2 = 0, sd;
    int64_t t0 = yrt_real_ns(), t1, t2;
    ygfx_noise_gauss(re, N, N, seed);
    memset(im, 0, (size_t)N * N * sizeof im[0]);
    t1 = yrt_real_ns();
    fft2(-1);
    for (i = 0; i < N * N; i++) { re[i] *= filt[kind][i]; im[i] *= filt[kind][i]; }
    fft2(1);
    t2 = yrt_real_ns();
    for (i = 0; i < N * N; i++) { s += re[i]; s2 += (double)re[i] * re[i]; }
    sd = sqrt(s2 / (N * N) - (s / (N * N)) * (s / (N * N)));
    for (i = 0; i < N * N; i++) {
        float g = (float)((re[i] - s / (N * N)) / sd / CLIP);
        out[i] = g < -1 ? -1 : g > 1 ? 1 : g;
    }
    part_ms[0] += (double)(t1 - t0) / 1e6; part_ms[1] += (double)(t2 - t1) / 1e6; part_ms[2] += (double)(yrt_real_ns() - t2) / 1e6;
    if (sd_out) {   /* the SD after scaling, before the clip */
        double q = 0;
        for (i = 0; i < N * N; i++) { double x = (re[i] - s / (N * N)) / sd; q += x * x; }
        *sd_out = sqrt(q / (N * N));
    }
}
/* From a field's power spectrum: the fraction inside the band, and the
 * in-band power along the filter's orientation over that across it. */
static void band_stats(const float* x, double* inband, double* ori_ratio) {
    int u, v;
    double tot = 0, in = 0, along = 0, across = 0;
    memcpy(re, x, (size_t)N * N * sizeof re[0]);
    memset(im, 0, (size_t)N * N * sizeof im[0]);
    fft2(-1);
    for (v = 0; v < N; v++)
        for (u = 0; u < N; u++) {
            double f, a, p = (double)re[v * N + u] * re[v * N + u] + (double)im[v * N + u] * im[v * N + u];
            int radial;
            freq(u, v, &f, &a);
            if (f == 0) continue;
            tot += p;
            radial = fabs(log2(f / F0)) <= OCT / 2;
            if (radial && ang_diff(a) <= ORI_W / 2) { in += p; along += p; }
            if (radial && ang_diff(a - 90) <= ORI_W / 2) across += p;
        }
    *inband = in / tot;
    *ori_ratio = across > 0 ? along / across : HUGE_VAL;
}
static void summary(const char* what, const double* v, int n) {
    static double s[MAXT];
    int i, j;
    if (n <= 0) return;
    memcpy(s, v, (size_t)n * sizeof *v);
    for (i = 1; i < n; i++) for (j = i; j > 0 && s[j] < s[j - 1]; j--) { double t = s[j]; s[j] = s[j - 1]; s[j - 1] = t; }
    printf("  %-30s median %7.3f  p99 %7.3f  max %7.3f ms (n %d)\n", what, s[n / 2], s[(n * 99) / 100], s[n - 1], n);
}

static yscr_screen scr;
static ygfx_gfx gfx;

int main(int argc, char** argv) {
    static ygfx_file_cache pcache;
    static char cache_dir[512];
    yscr_desc sd;
    ygfx_desc gd;
    ygfx_texture_desc td;
    ygfx_image_desc id;
    ygfx_noise_desc nd;
    ygfx_tex tex[2];
    ygfx_stim stim[4];
    yscr_frame f;
    double seconds = 5, rt_err = 0, sd_f[2], inb[4], ori[4], swap_trial = 0, swap_other = 0, period_ms = 0;
    int i, k, sim = 0, fullscreen = 0, trial_frames = 30, failed = 0, n_trials = 0, rc, disp;
    long drop_trial = 0, drop_other = 0, n_rec_trial = 0, n_rec_other = 0;
    int64_t t0 = 0, n = 0;

    N = 256;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--every-frame")) trial_frames = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) N = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trial-frames") && i + 1 < argc) trial_frames = atoi(argv[++i]);
        else { fprintf(stderr, "usage: gfx_filtered_noise [--sim] [--fullscreen] [--seconds S] [--size N] [--trial-frames N] [--every-frame]\n"); return 2; }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    for (LOG2N = 0; (1 << LOG2N) < N; LOG2N++) { }
    if (N < 64 || N > NMAX || (1 << LOG2N) != N || trial_frames < 1 || !(seconds > 0)) {
        fprintf(stderr, "gfx_filtered_noise: --size takes 64 to 512, a power of 2; --trial-frames >= 1; --seconds > 0\n");
        return 2;
    }
    setup();
    memset(&sd, 0, sizeof sd);
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = !fullscreen;
    sd.window_w = 960; sd.window_h = 640;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "gfx_filtered_noise: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    if (ygfx_default_cache_dir(cache_dir, sizeof cache_dir) == YGFX_OK) gd.cache = ygfx_file_cache_init(&pcache, cache_dir);
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_filtered_noise: %s\n", ygfx_error(&gfx)); return 1; }

    /* the round trip, then the first trial's fields, made before the window shows them */
    ygfx_noise_gauss(spare, N, N, 99);
    memcpy(re, spare, (size_t)N * N * sizeof re[0]);
    memset(im, 0, (size_t)N * N * sizeof im[0]);
    fft2(-1); fft2(1);
    for (i = 0; i < N * N; i++) { double e = fabs(re[i] / ((double)N * N) - spare[i]); rt_err = e > rt_err ? e : rt_err; }
    for (k = 0; k < 2; k++) make(k, 1000u + (uint32_t)k, field[k], &sd_f[k]);
    memset(part_ms, 0, sizeof part_ms);
    disp = N > 256 ? 256 : N;
    for (k = 0; k < 2; k++) {
        memset(&td, 0, sizeof td);
        td.w = td.h = N; td.format = YGFX_R32F; td.data = field[k];
        tex[k] = ygfx_texture(&gfx, &td);
        memset(&id, 0, sizeof id);
        id.tex = tex[k]; id.modulation = true; id.contrast = (float)(RMS * CLIP);
        id.linear = N > disp;
        id.x = k ? 140.0f : -140.0f; id.y = -150; id.w = id.h = (float)disp;
        stim[k] = ygfx_image(&gfx, &id);
    }
    memset(&nd, 0, sizeof nd);
    nd.x = -140; nd.y = 150; nd.w = nd.h = (float)disp; nd.check = 1; nd.seed = 5; nd.dist = YGFX_GAUSSIAN; nd.contrast = (float)RMS;
    stim[2] = ygfx_noise(&nd);
    nd.x = 140; nd.dist = YGFX_SIMPLEX; nd.scale = (float)(0.64 / F0); nd.octaves = 1; nd.contrast = (float)(RMS / 0.425);
    stim[3] = ygfx_noise(&nd);
    if (!tex[0].id || !tex[1].id) { fprintf(stderr, "gfx_filtered_noise: %s\n", ygfx_error(&gfx)); return 1; }

    /* each field's spectrum: the two CPU fields, NOISE GAUSSIAN, NOISE SIMPLEX */
    for (k = 0; k < 2; k++) band_stats(field[k], &inb[k], &ori[k]);
    ygfx_noise_fill(spare, N, N, 5, YGFX_GAUSSIAN);
    band_stats(spare, &inb[2], &ori[2]);
    for (i = 0; i < N * N; i++) spare[i] = ygfx_simplex_value(&gfx, &stim[3], i % N, i / N);
    band_stats(spare, &inb[3], &ori[3]);
    printf("top: band (1/16 c/px, 1 octave, 30 deg, vertical bars), 1/f; bottom: NOISE GAUSSIAN, NOISE SIMPLEX. %d x %d, RMS %.2f\n", N, N, RMS);

    while ((rc = yscr_begin(&scr, &f)) == YSCR_OK) {
        int trial = (int)(n % trial_frames) == 0 && n > 0;
        if (n == 0) { t0 = f.onset; period_ms = (double)f.period / 1e6; }
        for (i = 0; i < f.n_done; i++) {
            const yscr_record* r = &f.done[i];
            int was_trial = (r->index - (f.index - n)) % trial_frames == 0 && r->index > f.index - n;
            if (r->phase_ns[YSCR_PHASE_SWAP] == YSCR_PHASE_UNKNOWN) continue;
            if (was_trial) { drop_trial += r->dropped != 0; swap_trial += r->phase_ns[YSCR_PHASE_SWAP] / 1e6; n_rec_trial++; }
            else { drop_other += r->dropped != 0; swap_other += r->phase_ns[YSCR_PHASE_SWAP] / 1e6; n_rec_other++; }
        }
        if (trial && n_trials < MAXT) {   /* a new trial: both fields made and uploaded in this frame */
            int64_t a = yrt_real_ns(), b;
            for (k = 0; k < 2; k++) make(k, (uint32_t)(2 * n_trials + k), field[k], NULL);
            b = yrt_real_ns();
            yscr_mark(&scr, YSCR_PHASE_EVALUATE);
            for (k = 0; k < 2; k++)
                if (ygfx_texture_update(&gfx, tex[k], 0, 0, N, N, field[k], 0) != YGFX_OK) failed = 1;
            yscr_mark(&scr, YSCR_PHASE_UPLOAD);
            gen_ms[n_trials] = (double)(b - a) / 1e6;
            up_ms[n_trials++] = (double)(yrt_real_ns() - b) / 1e6;
        }
        ygfx_begin(&gfx, &f);
        for (k = 0; k < 4; k++)
            if (ygfx_draw(&gfx, &stim[k]) != YGFX_OK) { fprintf(stderr, "gfx_filtered_noise: %s\n", ygfx_error(&gfx)); failed = 1; }
        ygfx_end(&gfx);
        yscr_flip(&scr);
        n++;
        if (sim ? n > 4 * trial_frames : f.onset + f.period - t0 >= (int64_t)(seconds * 1e9)) break;
    }
    if (rc != YSCR_OK && rc != YSCR_QUIT) { fprintf(stderr, "gfx_filtered_noise: %s\n", yscr_error(&scr)); failed = 1; }
    ygfx_close(&gfx);
    yscr_close(&scr);

    printf("FFT round trip: largest error %.2e; SD after scaling: band %.6f, 1/f %.6f\n", rt_err, sd_f[0], sd_f[1]);
    printf("power inside the band (1 octave x 30 deg about 1/16 c/px, vertical bars), and along/across:\n");
    {
        static const char* const names[4] = { "band (CPU)", "1/f (CPU)", "NOISE GAUSSIAN", "NOISE SIMPLEX 1 octave" };
        for (k = 0; k < 4; k++) printf("  %-24s %6.3f   %10.3g\n", names[k], inb[k], ori[k]);
    }
    printf("cost per trial, two %d x %d fields, frame period %.3f ms:\n", N, N, period_ms);
    summary("make (FFT, filter, inverse)", gen_ms, n_trials);
    if (n_trials) printf("    of which, per field: white noise %.3f, FFT + filter + inverse %.3f, scaling %.3f ms (means)\n",
                         part_ms[0] / (2 * n_trials), part_ms[1] / (2 * n_trials), part_ms[2] / (2 * n_trials));
    summary("upload (texture_update, R32F)", up_ms, n_trials);
    printf("flip records: trial frames %ld (dropped %ld, SWAP mean %.3f ms); other frames %ld (dropped %ld, SWAP mean %.3f ms)\n",
           n_rec_trial, drop_trial, n_rec_trial ? swap_trial / n_rec_trial : 0.0, n_rec_other, drop_other,
           n_rec_other ? swap_other / n_rec_other : 0.0);
    failed |= !(rt_err < 1e-4) || fabs(sd_f[0] - 1) > 1e-3 || fabs(sd_f[1] - 1) > 1e-3;
    failed |= !(inb[0] > 0.6) || !(ori[0] > 10) || !(inb[2] < 0.1) || !(inb[3] < inb[0] / 2) || n_trials == 0;
    if (failed) { fprintf(stderr, "gfx_filtered_noise: a check failed\n"); return 1; }
    printf("checks passed\n");
    return 0;
}
