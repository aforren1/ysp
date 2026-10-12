/* gfx_stress.c - how much ysp/gfx.h can draw per frame on this machine
 * before frames drop, in the manner of bgfx's drawstress.
 *
 * The frame loop of the manual on the stimulus display (fullscreen), with one
 * kind of load, raised level by level: each level runs a fixed number of
 * frames; with no dropped frame and no late target the load doubles, and at
 * the first level that drops, it bisects between the last good level and
 * that one. The result is the largest load that dropped nothing, per kind:
 * the headroom a trial has on this machine. The flip records are the
 * measurement, as in gfx_load.
 *
 *   gabors  N gabors (sigma 6 px), each its own draw: CPU and GPU per stimulus
 *   noise   N full-screen layers of gradient noise (4 octaves): GPU fill
 *
 * A level fails on a dropped frame (the system was late), a late target
 * (the frame thread was late), or a slow frame (shown two or more vblanks
 * after the one before, which a raised depth causes with no drop counted).
 * After the ramp, the start load runs 600 frames: whether the screen comes
 * back to one frame per vblank. Past the knee, frames
 * the GPU makes late with the present on time are the case that raises the
 * screen's depth (docs/screen.md, "Depth on a backend that holds frames");
 * --hold N runs one load for a while to see that.
 *
 * Usage: gfx_stress [--kind gabors|noise] [--start N] [--max N] [--frames F]
 *                   [--bisect K] [--hold N --seconds S] [--windowed]
 *                   [--backend glx|dxgi|composition] [--cache DIR] [--no-cache]
 *   --start N   the first level (default 64 gabors, 1 noise layer)
 *   --max N     the largest level tried (default 16384 gabors, 64 layers)
 *   --frames F  frames per level (default 120, 2 s at 60 Hz)
 *   --bisect K  bisection steps after the first failing level (default 5)
 *   --hold N    no ramp: N for --seconds S (default 10)
 * Low contrast, no flicker, but fullscreen: the display is taken while it
 * runs. Shift+Esc ends it. Run nothing else while it measures.
 * Exit code: 0, 1 when the screen or the gfx did not open, 2 for a bad
 * argument.
 */
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STIMS 16384
#define MAX_LEVEL_FRAMES 4096

static yscr_screen scr;
static ygfx_gfx gfx;
static ygfx_stim stims[MAX_STIMS];
static yrt_event ring_mem[16384];
static yrt_ring ring;
static uint32_t g_draw[MAX_LEVEL_FRAMES], g_swap[MAX_LEVEL_FRAMES];

typedef struct level {
    int load, frames, dropped, late_target, slow, depth;
    double draw_p50, draw_p99, swap_p99;
} level;

static int cmp_u(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : (x > y);
}

static double pct(uint32_t* v, int n, double p) {
    if (n <= 0) return 0;
    qsort(v, (size_t)n, sizeof v[0], cmp_u);
    return v[(int)(p * (n - 1))] * 1e-3;
}

static int depth_of(void) {
    char line[1024];
    const char* p;
    yscr_describe(&scr, line, sizeof line);
    p = strstr(line, " depth=");
    return p ? atoi(p + 7) : 0;
}

/* One level: frames frames at this load, then the records of all of them. */
static int run_level(int load, int frames, int kind_noise, level* out) {
    yscr_frame f;
    int i, n_rec = 0, quit = 0;
    int64_t first = -1, prev_onset = 0, period = 0;
    memset(out, 0, sizeof *out);
    out->load = load;
    for (i = 0; i < frames + 1 && !quit; i++) {
        yrt_event ev[64];
        int n, k;
        if (yscr_begin(&scr, &f) != YSCR_OK) { quit = 1; break; }
        if (first < 0) first = f.index;
        period = f.period;
        if (i < frames) {
            float ph = (float)((double)(f.onset % 1000000000) * 1e-9);
            /* gabors drift; a noise layer is the same work every frame */
            if (!kind_noise) for (k = 0; k < load; k++) stims[k].phase = ph;
            yscr_mark(&scr, YSCR_PHASE_EVALUATE);
            ygfx_begin(&gfx, &f);
            ygfx_draw_n(&gfx, stims, load);
            ygfx_end(&gfx);
            yscr_flip(&scr);
        } else {
            /* the last frame of the level: wait for every record */
            ygfx_begin(&gfx, &f);
            ygfx_end(&gfx);
            yscr_flip(&scr);
            yscr_wait_flip(&scr, NULL);
        }
        n = yrt_ring_drain(&ring, ev, 64);
        for (k = 0; k < n; k++) {
            int64_t idx;
            unsigned flags;
            if (ev[k].source != YRT_SRC_SCREEN || ev[k].kind != YSCR_EV_FLIP) continue;
            idx = (int64_t)ev[k].u.u32[9];
            if (idx < first || idx >= first + frames) continue;
            flags = YSCR_EV_FLAGS_OF(ev[k].u.u16[5]);
            out->frames++;
            out->dropped += ev[k].u.u16[4];
            out->late_target += (flags & YSCR_FLIP_LATE_TARGET) != 0;
            /* shown two or more vblanks after the frame before: a drop, or
             * a frame planned at a depth above 1, which halves the rate */
            if (prev_onset && (int64_t)ev[k].t_ns - prev_onset > period * 3 / 2) out->slow++;
            prev_onset = (int64_t)ev[k].t_ns;
            if (n_rec < MAX_LEVEL_FRAMES) {
                g_draw[n_rec] = ev[k].u.u32[3 + YSCR_PHASE_DRAW];
                g_swap[n_rec] = ev[k].u.u32[3 + YSCR_PHASE_SWAP];
                n_rec++;
            }
        }
    }
    out->draw_p50 = pct(g_draw, n_rec, 0.5);
    out->draw_p99 = pct(g_draw, n_rec, 0.99);
    out->swap_p99 = pct(g_swap, n_rec, 0.99);
    out->depth = depth_of();
    return quit;
}

static int failed(const level* l) { return l->dropped || l->late_target || l->slow; }

static void print_level(const level* l, const char* unit) {
    printf("  %6d %-7s frames %4d  dropped %4d  late targets %4d  slow %4d  draw p50 %8.1f p99 %8.1f us  "
           "swap p99 %8.1f us  depth %d  %s\n",
           l->load, unit, l->frames, l->dropped, l->late_target, l->slow, l->draw_p50, l->draw_p99, l->swap_p99,
           l->depth, failed(l) ? "FAIL" : "ok");
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    yrt_ring_desc rd;
    static ygfx_file_cache pcache;
    static char cache_dir[512];
    const ygfx_cache* cache = NULL;
    int i, kind_noise = 0, start = 0, max = 0, frames = 120, bisect = 5, hold = 0, no_cache = 0;
    double seconds = 10;
    char line[1024];
    const char* unit;

    memset(&sd, 0, sizeof sd);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--kind") && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "noise")) kind_noise = 1;
            else if (strcmp(argv[i], "gabors")) { fprintf(stderr, "--kind: gabors or noise\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--start") && i + 1 < argc) start = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) max = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bisect") && i + 1 < argc) bisect = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) hold = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--windowed")) sd.windowed = true;
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "glx")) sd.backend = YSCR_BACKEND_GLX_OML;
            else if (!strcmp(argv[i], "dxgi")) sd.backend = YSCR_BACKEND_DXGI_FLIP;
            else if (!strcmp(argv[i], "composition")) sd.backend = YSCR_BACKEND_COMPOSITION;
            else { fprintf(stderr, "--backend: glx, dxgi or composition\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc && (cache = ygfx_file_cache_init(&pcache, argv[i + 1])) != NULL) i++;
        else if (!strcmp(argv[i], "--no-cache")) no_cache = 1;
        else {
            fprintf(stderr, "usage: see the comment at the top of gfx_stress.c\n");
            return 2;
        }
    }
    if (!start) start = kind_noise ? 1 : 64;
    if (!max) max = kind_noise ? 64 : MAX_STIMS;
    if (start < 1 || max > MAX_STIMS || start > max || hold < 0 || hold > MAX_STIMS || frames < 10 ||
        frames > MAX_LEVEL_FRAMES || bisect < 0 || !(seconds > 0)) {
        fprintf(stderr, "gfx_stress: 1 <= start <= max <= %d, 10 <= frames <= %d\n", MAX_STIMS, MAX_LEVEL_FRAMES);
        return 2;
    }
    if (no_cache) cache = NULL;
    else if (!cache && ygfx_default_cache_dir(cache_dir, sizeof cache_dir) == YGFX_OK)
        cache = ygfx_file_cache_init(&pcache, cache_dir);
    unit = kind_noise ? "layers" : "gabors";

    memset(&rd, 0, sizeof rd);
    rd.memory = ring_mem;
    rd.bytes = sizeof ring_mem;
    if (!yrt_ring_open(&ring, &rd)) { fprintf(stderr, "gfx_stress: ring\n"); return 1; }
    sd.ring = &ring;
    (void)yrt_thread_elevate(NULL);
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "gfx_stress: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    gd.max_draws = MAX_STIMS + 16;
    gd.cache = cache;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_stress: %s\n", ygfx_error(&gfx)); return 1; }
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    ygfx_describe(&gfx, line, sizeof line);
    printf("%s\n", line);
    {
        yscr_caps caps;
        uint32_t seed = 1;
        yscr_get_caps(&scr, &caps);
        for (i = 0; i < MAX_STIMS; i++) {
            if (!kind_noise) {
                ygfx_gabor_desc gb;
                memset(&gb, 0, sizeof gb);
                gb.sigma = 6; gb.sf = 0.08f; gb.contrast = 0.05f; gb.ori = (float)(i * 17 % 180);
                gb.place = YGFX_TOP_LEFT;
                seed = seed * 1664525u + 1013904223u;
                gb.x = (float)(20 + (seed >> 8) % (uint32_t)(caps.mode.w - 40));
                seed = seed * 1664525u + 1013904223u;
                gb.y = (float)(20 + (seed >> 8) % (uint32_t)(caps.mode.h - 40));
                stims[i] = ygfx_gabor(&gb);
            } else {
                ygfx_noise_desc nd;
                memset(&nd, 0, sizeof nd);
                nd.place = YGFX_TOP_LEFT;
                nd.w = (float)caps.mode.w; nd.h = (float)caps.mode.h;
                nd.dist = YGFX_SIMPLEX; nd.scale = 40; nd.octaves = 4; nd.contrast = 0.02f;
                nd.seed = (uint32_t)i + 1u;
                stims[i] = ygfx_noise(&nd);
            }
        }
    }
    if (hold) {
        level l;
        yscr_caps caps;
        int n;
        yscr_get_caps(&scr, &caps);
        n = (int)(seconds * 1e9 / (double)caps.period_ns + 0.5);
        if (n > MAX_LEVEL_FRAMES) n = MAX_LEVEL_FRAMES;
        printf("hold: %d %s for %d frames\n", hold, unit, n);
        run_level(hold, n, kind_noise, &l);
        print_level(&l, unit);
    } else {
        level l;
        int good = 0, bad = 0, load = start, quit = 0, k;
        printf("ramp: %s from %d, x2 per level, %d frames a level\n", unit, start, frames);
        /* one warm level, unreported: the programs, the first uploads */
        quit = run_level(start, 30, kind_noise, &l);
        while (!quit && load <= max) {
            quit = run_level(load, frames, kind_noise, &l);
            print_level(&l, unit);
            if (failed(&l)) { bad = load; break; }
            good = load;
            if (load == max) break;
            load = load * 2 > max ? max : load * 2;
        }
        for (k = 0; k < bisect && !quit && bad && bad - good > 1; k++) {
            load = good + (bad - good) / 2;
            quit = run_level(load, frames, kind_noise, &l);
            print_level(&l, unit);
            if (failed(&l)) bad = load; else good = load;
        }
        /* Does the screen come back to one frame per vblank at the light
         * load? A raised depth that never falls shows here as slow frames. */
        if (!quit) {
            printf("recovery: %d %s for 600 frames after the ramp\n", start, unit);
            run_level(start, 600, kind_noise, &l);
            print_level(&l, unit);
        }
        if (bad) printf("knee: %d %s dropped nothing in %d frames; %d dropped or ran late\n", good, unit, frames, bad);
        else printf("knee: not reached; %d %s dropped nothing (raise --max)\n", good, unit);
    }
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    ygfx_close(&gfx);
    yscr_close(&scr);
    return 0;
}
