/* gfx_capture.c - the presented output of a trial's frames, to files, and
 * what that does to the flips (ysp/gfx.h CAPTURE).
 *
 * Draws a drifting field of gabors and a moving bar on mid gray for N
 * frames, captures every Nth frame (or none, or marked frames), and prints
 * one result line: the flip records (late targets, dropped vblanks, early
 * flips, onset intervals), the frame thread's CPU time in ygfx_end(), and
 * the capture's counts. Run it with --off and with --every N, in a window
 * and fullscreen, to see whether capture moves a flip (docs/gfx.md, "v0.12").
 *
 * Usage: gfx_capture [--sim] [--fullscreen] [--size W H] [--frames N]
 *                    [--off | --every N | --mark N] [--slots N]
 *                    [--out PREFIX] [--png] [--manual] [--gabors N]
 *                    [--cache DIR] [--no-cache]
 *   --sim         no window and no GL: the simulated display and the null
 *                 backend (its frames are black), for CI
 *   --fullscreen  borderless fullscreen at the desktop mode (default: a
 *                 window of --size, 800 x 600)
 *   --frames N    frames to run (default 600; at most 40000)
 *   --off         no capture (the control)
 *   --every N     capture frames whose index % N == 0 (default 1)
 *   --mark N      capture only the frames ygfx_capture_mark() marks: one in N
 *   --slots N     pixel buffers (default 6)
 *   --out PREFIX  write PREFIX<frame>.ppm (or .png); default: no files, the
 *                 writer only counts
 *   --png         PNG (stored) instead of PPM
 *   --manual      no writer thread: ygfx_capture_pump() after the run
 *   --gabors N    gabors in the field (default 100)
 * Shift+Esc ends a run early. Exit code: 0; 1 when the screen or the gfx
 * did not open or capture did not start; 2 for a bad argument; 3 when a
 * capture failed or the counts do not add up.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 40000
#define MAX_GABORS 1000

static yscr_screen scr;
static ygfx_gfx gfx;
static yscr_record recs[MAX_FRAMES];
static int n_recs;
static double end_us[MAX_FRAMES];
static ygfx_stim gab[MAX_GABORS];

static void start_gfx(void* ctx, yscr_screen* s) {
    ygfx_desc* gd = (ygfx_desc*)ctx;
    gd->screen = s;
    ygfx_open_start(&gfx, gd);
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y);
}

static void keep(const yscr_frame* f) {
    int k;
    for (k = 0; k < f->n_done && n_recs < MAX_FRAMES; k++) recs[n_recs++] = f->done[k];
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    ygfx_capture_desc cd;
    ygfx_capture_stats st;
    ygfx_gabor_desc gbd;
    ygfx_shape_desc bd;
    ygfx_stim bar;
    yscr_frame f;
    static ygfx_file_cache pcache;
    static char cache_dir[512];
    const ygfx_cache* cache = NULL;
    int64_t t0 = 0, period = 0;
    int i, frames = 600, every = 1, mark = 0, off = 0, slots = 6, png = 0, manual = 0, n_gab = 100, no_cache = 0;
    int ran = 0, late = 0, dropped = 0, early = 0, estimated = 0, skipped = 0, long_iv = 0, n_iv = 0, bad = 0;
    const char* out = NULL;
    double iv_sum = 0, iv_sq = 0, iv_max = 0;
    char line[640];

    memset(&sd, 0, sizeof sd);
    sd.windowed = true;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sd.backend = YSCR_BACKEND_SIM;
        else if (!strcmp(argv[i], "--fullscreen")) sd.windowed = false;
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) { sd.window_w = atoi(argv[i + 1]); sd.window_h = atoi(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--off")) off = 1;
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mark") && i + 1 < argc) { mark = atoi(argv[++i]); every = 0; }
        else if (!strcmp(argv[i], "--slots") && i + 1 < argc) slots = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--png")) png = 1;
        else if (!strcmp(argv[i], "--manual")) manual = 1;
        else if (!strcmp(argv[i], "--gabors") && i + 1 < argc) n_gab = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc && (cache = ygfx_file_cache_init(&pcache, argv[i + 1])) != NULL) i++;
        else if (!strcmp(argv[i], "--no-cache")) no_cache = 1;
        else { i = -1; break; }
    }
    if (i < 0 || frames < 2 || frames > MAX_FRAMES || every < 0 || mark < 0 || (!every && !mark && !off) ||
        slots < 1 || slots > YGFX_CAPTURE_MAX_SLOTS || n_gab < 0 || n_gab > MAX_GABORS) {
        fprintf(stderr, "usage: gfx_capture [--sim] [--fullscreen] [--size W H] [--frames N] [--off | --every N | --mark N]\n"
                        "                   [--slots N] [--out PREFIX] [--png] [--manual] [--gabors N] [--cache DIR] [--no-cache]\n");
        return 2;
    }
    if (no_cache) cache = NULL;
    else if (!cache && ygfx_default_cache_dir(cache_dir, sizeof cache_dir) == YGFX_OK)
        cache = ygfx_file_cache_init(&pcache, cache_dir);

    memset(&gd, 0, sizeof gd);
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    gd.cache = cache;
    sd.on_context = start_gfx;
    sd.on_context_ctx = &gd;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "gfx_capture: %s\n", yscr_error(&scr)); ygfx_close(&gfx); return 1; }
    gd.screen = &scr;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_capture: %s\n", ygfx_error(&gfx)); yscr_close(&scr); return 1; }
    {
        float w, h;
        ygfx_size(&gfx, &w, &h);
        memset(&gbd, 0, sizeof gbd);
        gbd.sigma = 12; gbd.sf = 1 / 24.0f; gbd.contrast = 0.15f;
        for (i = 0; i < n_gab; i++) {   /* a grid over the screen, each its own orientation */
            gbd.x = (float)((i * 97) % 1000) / 1000.0f * w * 0.9f - 0.45f * w;
            gbd.y = (float)((i * 61) % 1000) / 1000.0f * h * 0.9f - 0.45f * h;
            gbd.ori = (float)(i * 23 % 180);
            gab[i] = ygfx_gabor(&gbd);
        }
        memset(&bd, 0, sizeof bd);
        bd.shape = YGFX_RECT; bd.w = 24; bd.h = h; bd.color[0] = bd.color[1] = bd.color[2] = 0.6f;
        bar = ygfx_shape(&bd);
    }
    ygfx_prime(&gfx);
    if (!off) {
        memset(&cd, 0, sizeof cd);
        cd.every = every;
        cd.slots = slots;
        cd.path = out;
        cd.format = png ? YGFX_IMAGE_PNG : YGFX_IMAGE_PPM;
        cd.manual = manual != 0;
        if (ygfx_capture_start(&gfx, &cd) != YGFX_OK) {
            fprintf(stderr, "gfx_capture: %s\n", ygfx_error(&gfx));
            ygfx_close(&gfx);
            yscr_close(&scr);
            return 1;
        }
    }
    ygfx_describe(&gfx, line, sizeof line);
    printf("%s\n", line);
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);

    while (ran < frames && yscr_begin(&scr, &f) == YSCR_OK) {
        float w, h;
        uint64_t a;
        int k;
        keep(&f);
        if (ran == 0) { t0 = f.onset; period = f.period; }
        ygfx_size(&gfx, &w, &h);
        /* every phase and position from the predicted onset: a drop shows as
         * a jump in the captured frames, not a slower drift */
        for (k = 0; k < n_gab; k++) gab[k].phase = (float)fmod((double)(f.onset - t0) * 1e-9 * (1.0 + 0.01 * k), 1.0);
        bar.x = (float)fmod((double)(f.onset - t0) * 1e-9 * 240.0, (double)w) - 0.5f * w;
        ygfx_begin(&gfx, &f);
        if (mark && f.index % mark == 0) ygfx_capture_mark(&gfx);
        ygfx_draw_n(&gfx, gab, n_gab);
        ygfx_draw(&gfx, &bar);
        a = yrt_now_ns();
        ygfx_end(&gfx);
        end_us[ran] = (double)(yrt_now_ns() - a) * 1e-3;
        yscr_flip(&scr);
        ran++;
    }
    /* one more frame brings the last flips' records (and gives the capture
     * its records), then the rest */
    if (yscr_begin(&scr, &f) == YSCR_OK) {
        keep(&f);
        ygfx_begin(&gfx, &f);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    if (yscr_wait_flip(&scr, NULL) == YSCR_OK && yscr_begin(&scr, &f) == YSCR_OK) {
        keep(&f);
        ygfx_begin(&gfx, &f);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    memset(&st, 0, sizeof st);
    if (!off) {
        if (manual) ygfx_capture_pump(&gfx, 0);
        ygfx_capture_stop(&gfx, &st);
    }

    for (i = 0; i < n_recs; i++) {
        const yscr_record* r = &recs[i];
        if (r->index >= ran) continue;   /* the closing frames */
        if (r->flags & YSCR_FLIP_LATE_TARGET) late++;
        if (r->dropped) dropped++;
        if (r->flags & YSCR_FLIP_EARLY) early++;
        if (r->flags & YSCR_FLIP_ESTIMATED) estimated++;
        if (r->flags & (YSCR_FLIP_SKIPPED | YSCR_FLIP_CANCELED)) skipped++;
        if (i > 0 && r->onset && recs[i - 1].onset && r->index == recs[i - 1].index + 1) {
            double iv = (double)(r->onset - recs[i - 1].onset) * 1e-6;
            iv_sum += iv; iv_sq += iv * iv; n_iv++;
            if (iv > iv_max) iv_max = iv;
            if (period > 0 && iv > 1.5 * (double)period * 1e-6) long_iv++;
        }
    }
    qsort(end_us, (size_t)ran, sizeof end_us[0], cmp_d);
    {
        double mean = n_iv ? iv_sum / n_iv : 0, sd2 = n_iv > 1 ? (iv_sq - n_iv * mean * mean) / (n_iv - 1) : 0;
        printf("result: %s, %d frames, %d records; late_target %d, dropped %d, early %d, estimated %d, skipped %d; "
               "interval mean %.3f ms sd %.3f max %.3f, > 1.5 periods %d; end() cpu p50 %.0f us p99 %.0f us max %.0f us; "
               "capture %s every %d mark %d slots %d: chosen %llu read %llu dropped %llu written %llu failed %llu "
               "no_record %llu busy_max %d\n",
               sd.backend == YSCR_BACKEND_SIM ? "sim" : sd.windowed ? "windowed" : "fullscreen", ran, n_recs, late, dropped,
               early, estimated, skipped, mean, sd2 > 0 ? sqrt(sd2) : 0.0, iv_max, long_iv,
               end_us[ran / 2], end_us[(int)(0.99 * (ran - 1))], end_us[ran - 1],
               off ? "off" : out ? (png ? "png" : "ppm") : "no-files", every, mark, slots,
               (unsigned long long)st.chosen, (unsigned long long)st.read, (unsigned long long)st.dropped,
               (unsigned long long)st.written, (unsigned long long)st.failed, (unsigned long long)st.no_record, st.busy_max);
    }
    if (!off && (st.failed || st.written + st.dropped != st.chosen)) bad = 1;
    ygfx_close(&gfx);
    yscr_close(&scr);
    return bad ? 3 : 0;
}
