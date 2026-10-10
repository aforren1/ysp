/* gfx_rdk_bench.c - what an RDK costs per frame end to end, offscreen.
 *
 * ysp/rdk.h moves the dots and ysp/gfx.h draws them, on a headless GL ES
 * 3.0 context (tests/adapt/gfx_headless.h) at the panel's size, by
 * gfx_bench.c's method:
 *   cpu   per frame, from yrdk_update() through the upload (or the write
 *         into the instance records) to ygfx_end(); mean and p99. The
 *         field's own part (update and output) is given apart.
 *   gpu   the workload's frames back to back between two glFinish()
 *         calls, per frame: GPU-bound throughput, the room a frame loop
 *         must leave. No timer query (it costs about 250 us of CPU through
 *         ANGLE; docs/screen.md).
 * Workloads: an empty frame; dot fields of 1000 and 10,000 (WN, and SAME
 * with DIRECTION noise); two fields of 5000 (transparent motion); gabor
 * arrays of 1000 and 10,000 elements of 32 px riding the dots. The bars
 * (docs/rdk.md): 10,000 dots at most 0.5 ms of CPU and 0.5 ms of GPU
 * over the empty frame; 10,000 gabors at most 0.6 ms of CPU and 2 ms of
 * GPU. Nothing is shown. Take the measurement lock first.
 *
 * Usage: gfx_rdk_bench [--device hardware|warp|swiftshader|mesa|llvmpipe]
 *                      [--size W H] [--frames N] [--only NAME] [--cache DIR]
 *                      [--no-cache]
 *   --cache DIR  keep compiled programs in DIR; the default is the per-user
 *                folder of ygfx_default_cache_dir(), when there is one
 *   --no-cache   compile every program; read and write no cache file
 * Exit code: 0, 1 when no GL ES 3.0 context opened or a call failed, 2 for
 * a bad argument.
 * On Windows set YSCR_ANGLE_DIR to ANGLE's directory.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#include "../../tests/adapt/gfx_headless.h"
#define YSP_RDK_IMPLEMENTATION
#include "ysp/rdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #define GLCALL __stdcall
#else
    #define GLCALL
#endif

#define MAXF 2000
#define MAXN 10000

static ygfx_headless hl;
static yscr_screen scr;
static ygfx_gfx gfx;
static int W = 1920, H = 1200, FRAMES = 240;
static const char* only = NULL;
static void (GLCALL *glFinish_)(void);
static double cpu_us[MAXF], rdk_us[MAXF];

static yrdk_field fa, fb;
static ygfx_buf buf_a, buf_b;
static ygfx_stim dots_a, dots_b, gab_tmpl, gabors;
static float xy[2 * MAXN];
static ygfx_inst items[MAXN];
static int failures = 0;

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y);
}

static double mean_of(const double* v, int n) {
    double s = 0;
    int i;
    for (i = 0; i < n; i++) s += v[i];
    return s / n;
}

static double p99_of(double* v, int n) {
    qsort(v, (size_t)n, sizeof v[0], cmp_d);
    return v[(int)(0.99 * (n - 1))];
}

enum { W_EMPTY, W_DOTS, W_TWO, W_GABORS };

static int open_field(yrdk_field* f, yrdk_algorithm alg, int n, float dir, uint32_t stream) {
    yrdk_desc d;
    yrdk_close(f);
    memset(&d, 0, sizeof d);
    d.algorithm = alg;
    d.w = 1000; d.count = n; d.coherence = 0.5f; d.direction = dir; d.speed = 300; d.stream = stream;
    if (!yrdk_open(f, &d)) { fprintf(stderr, "gfx_rdk_bench: %s\n", yrdk_error(f)); failures++; return -1; }
    return yrdk_start(f, 7, 0);
}

static int step(int kind, int64_t t) {
    int rc = 0;
    if (kind == W_EMPTY) return 0;
    rc |= yrdk_update(&fa, t) < 0;
    if (kind == W_GABORS) {
        yrdk_out o;
        memset(&o, 0, sizeof o);
        o.xy = &items[0].x; o.xy_stride = sizeof items[0];
        o.dir = &items[0].ori; o.dir_stride = sizeof items[0];
        rc |= yrdk_write(&fa, &o) < 0;
        return rc;
    }
    rc |= yrdk_xy(&fa, xy) < 0;
    rc |= ygfx_buffer_update(&gfx, buf_a, 0, xy, (size_t)fa.n * 8) < 0;
    if (kind == W_TWO) {
        rc |= yrdk_update(&fb, t) < 0;
        rc |= yrdk_xy(&fb, xy) < 0;
        rc |= ygfx_buffer_update(&gfx, buf_b, 0, xy, (size_t)fb.n * 8) < 0;
    }
    return rc;
}

static void draw(int kind) {
    if (kind == W_DOTS || kind == W_TWO) ygfx_draw(&gfx, &dots_a);
    if (kind == W_TWO) ygfx_draw(&gfx, &dots_b);
    if (kind == W_GABORS) ygfx_draw(&gfx, &gabors);
}

static void bench(const char* name, int kind) {
    yscr_frame f;
    uint64_t t0, a, m, b;
    int i;
    if (only && !strstr(name, only)) return;
    memset(&f, 0, sizeof f);
    f.period = 16666667;
    for (i = 0; i < 5; i++) {
        if (step(kind, (int64_t)i * f.period)) failures++;
        ygfx_begin(&gfx, &f);
        draw(kind);
        ygfx_end(&gfx);
    }
    glFinish_();
    t0 = yrt_now_ns();
    for (i = 0; i < FRAMES; i++) {
        f.index = i;
        f.onset = (int64_t)(i + 5) * f.period;
        a = yrt_now_ns();
        if (step(kind, f.onset)) failures++;
        m = yrt_now_ns();
        ygfx_begin(&gfx, &f);
        draw(kind);
        ygfx_end(&gfx);
        b = yrt_now_ns();
        cpu_us[i] = (double)(b - a) * 1e-3;
        rdk_us[i] = (double)(m - a) * 1e-3;
    }
    glFinish_();
    {
        double wall = (double)(yrt_now_ns() - t0) / FRAMES * 1e-6;
        double cm = mean_of(cpu_us, FRAMES), rm = mean_of(rdk_us, FRAMES);
        printf("| %-44s | %8.1f | %8.1f | %8.1f | %8.3f |\n", name, cm, p99_of(cpu_us, FRAMES), rm, wall);
        fflush(stdout);
    }
}

int main(int argc, char** argv) {
    yscr_desc d;
    ygfx_desc gd;
    ygfx_dots_desc dd;
    ygfx_gabor_desc gbd;
    ygfx_instances_desc id;
    yscr_proc p;
    static ygfx_file_cache pcache;
    static char cache_dir[512];
    const ygfx_cache* cache = NULL;
    int i, round, no_cache = 0;
    hl.device = YGFX_HL_HARDWARE;
#if !defined(_WIN32)
    hl.device = YGFX_HL_MESA;
#endif
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            const char* v = argv[++i];
            if (!strcmp(v, "hardware")) hl.device = YGFX_HL_HARDWARE;
            else if (!strcmp(v, "warp")) hl.device = YGFX_HL_WARP;
            else if (!strcmp(v, "swiftshader")) hl.device = YGFX_HL_SWIFTSHADER;
            else if (!strcmp(v, "mesa")) hl.device = YGFX_HL_MESA;
            else if (!strcmp(v, "llvmpipe")) hl.device = YGFX_HL_MESA_SOFTWARE;
            else { fprintf(stderr, "gfx_rdk_bench: unknown device %s\n", v); return 2; }
        } else if (!strcmp(argv[i], "--size") && i + 2 < argc) {
            W = atoi(argv[++i]); H = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            FRAMES = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--only") && i + 1 < argc) {
            only = argv[++i];
        } else if (!strcmp(argv[i], "--cache") && i + 1 < argc && (cache = ygfx_file_cache_init(&pcache, argv[i + 1])) != NULL) {
            i++;
        } else if (!strcmp(argv[i], "--no-cache")) {
            no_cache = 1;
        } else {
            fprintf(stderr, "usage: gfx_rdk_bench [--device hardware|warp|swiftshader|mesa|llvmpipe] [--size W H] "
                            "[--frames N] [--only NAME] [--cache DIR] [--no-cache]\n");
            return 2;
        }
    }
    /* The per-user folder unless told otherwise, so that a second run loads
     * the programs instead of compiling them (PROGRAM CACHE). */
    if (no_cache) cache = NULL;
    else if (!cache && ygfx_default_cache_dir(cache_dir, sizeof cache_dir) == YGFX_OK)
        cache = ygfx_file_cache_init(&pcache, cache_dir);
    if (W < 256 || H < 256 || FRAMES < 10 || FRAMES > MAXF) { fprintf(stderr, "gfx_rdk_bench: bad size or frames\n"); return 2; }
    hl.w = W; hl.h = H;
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_CUSTOM;
    d.presenter = &ygfx_headless_presenter;
    d.presenter_ctx = &hl;
    d.sim_period_ns = 1000000;
    if (!yscr_open(&scr, &d)) { fprintf(stderr, "gfx_rdk_bench: %s\n", yscr_error(&scr)); return 1; }
    p = yscr_gl_proc(&scr, "glFinish");
    memcpy(&glFinish_, &p, sizeof p);
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    gd.max_instances = MAXN;
    gd.cache = cache;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_rdk_bench: %s\n", ygfx_error(&gfx)); return 1; }
    buf_a = ygfx_buffer(&gfx, sizeof xy);
    buf_b = ygfx_buffer(&gfx, sizeof xy);
    memset(&dd, 0, sizeof dd);
    dd.dot_size = 4; dd.edge = YGFX_EDGE_COSINE; dd.edge_width = 1;
    dd.color[0] = dd.color[1] = dd.color[2] = 1;
    dd.aperture = YGFX_CIRCLE; dd.w = 1010;
    dd.buf = buf_a; dots_a = ygfx_dots(&dd);
    dd.buf = buf_b; dots_b = ygfx_dots(&dd);
    memset(&gbd, 0, sizeof gbd);
    gbd.sf = 1 / 8.0f; gbd.sigma = 4; gbd.contrast = 0.3f;
    gab_tmpl = ygfx_gabor(&gbd);
    ygfx_inst_grid(items, MAXN, 1, 0, 0);
    {
        char line[640];
        ygfx_describe(&gfx, line, sizeof line);
        printf("%s\nysp/rdk.h %s; %d frames a row; CPU in us, GPU ms per frame\n\n", line, yrdk_version(), FRAMES);
    }
    printf("| %-44s | %8s | %8s | %8s | %8s |\n", "workload", "CPU mean", "CPU p99", "field", "GPU ms");
    printf("|---|---|---|---|---|\n");
    for (round = 0; round < 2; round++) {
        static const struct { const char* name; int kind; yrdk_algorithm alg; int n; } wl[] = {
            { "empty frame", W_EMPTY, YRDK_CUSTOM, 0 },
            { "1000 dots, WN", W_DOTS, YRDK_WN, 1000 },
            { "10000 dots, WN", W_DOTS, YRDK_WN, 10000 },
            { "10000 dots, SAME DIRECTION", W_DOTS, YRDK_CUSTOM, 10000 },
            { "2 x 5000 dots, transparent, WN", W_TWO, YRDK_WN, 5000 },
            { "1000 gabors 32 px riding the dots, WN", W_GABORS, YRDK_WN, 1000 },
            { "10000 gabors 32 px riding the dots, WN", W_GABORS, YRDK_WN, 10000 },
        };
        int k;
        printf("| round %c | | | | |\n", 'A' + round);
        for (k = 0; k < (int)(sizeof wl / sizeof wl[0]); k++) {
            if (wl[k].n) {
                if (open_field(&fa, wl[k].alg, wl[k].n, 0, 0) < 0) break;
                if (wl[k].kind == W_TWO && open_field(&fb, wl[k].alg, wl[k].n, 180, 1) < 0) break;
                dots_a.count = (uint32_t)fa.n;
                dots_b.count = (uint32_t)fb.n;
            }
            if (wl[k].kind == W_GABORS) {
                memset(&id, 0, sizeof id);
                id.inst = items; id.n = wl[k].n; id.fields = YGFX_I_XY | YGFX_I_ORI;
                gabors = ygfx_instances(&gfx, &gab_tmpl, &id);
                if (gabors.n_inst == 0) { fprintf(stderr, "gfx_rdk_bench: %s\n", ygfx_error(&gfx)); failures++; continue; }
            }
            bench(wl[k].name, wl[k].kind);
        }
    }
    yrdk_close(&fa);
    yrdk_close(&fb);
    ygfx_close(&gfx);
    yscr_close(&scr);
    if (failures) { fprintf(stderr, "gfx_rdk_bench: %d failed calls\n", failures); return 1; }
    return 0;
}
