/* gfx_load.c - ysp/gfx.h in a real frame loop, under load, with the flip
 * records as the measurement.
 *
 * An 800 x 600 window (or the simulated display) draws N small gabors and
 * M dots whose positions are uploaded every frame, marks the phases the
 * manual's frame loop marks (EVALUATE, UPLOAD; the rest is DRAW), and at the
 * end prints, from ysp/screen.h's flip records: frames, drops, late targets,
 * and the p50 and p99 of the DRAW, UPLOAD and SWAP phases. Low contrast, no
 * flicker: the display may be somebody's working screen.
 *
 * Usage: gfx_load [--sim] [--gabors N] [--dots N] [--seconds S] [--composition]
 *                 [--topmost] [--allocs | --allocs-control] [--cache DIR] [--no-cache]
 *   --topmost         keep the window on top and take the foreground (Windows)
 *   --allocs          count C runtime heap calls in the frame loop after
 *                     frame 30 (MSVC debug builds)
 *   --allocs-control  the same, with one malloc per frame the count must see
 *   --cache DIR       keep compiled programs in DIR; the default is the
 *                     per-user folder of ygfx_default_cache_dir()
 *   --no-cache        compile every program; read and write no cache file
 * Exit code: 0, 1 when the screen or the gfx did not open, 2 for a bad
 * argument.
 */
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
/* Windows gives the foreground to a program started from the background
 * after it sends one input event; a zero-size mouse move is that event. */
static void take_foreground(SDL_Window* w) {
    HWND h = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof in);
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(h);
}
/* SDL calls on the window go through yscr_window_call(): with the window
 * thread (ysp/screen.h, WINDOW THREAD) another thread owns the window. */
static void take_foreground_on_window(void* w) { take_foreground((SDL_Window*)w); }
#endif

static volatile long g_allocs;
static volatile int g_count_allocs;
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
static int alloc_hook(int type, void* data, size_t size, int block, long req, const unsigned char* file, int line) {
    (void)data; (void)size; (void)block; (void)req; (void)file; (void)line;
    if (g_count_allocs && (type == _HOOK_ALLOC || type == _HOOK_REALLOC)) g_allocs++;
    return 1;
}
#endif

static yscr_screen scr;
static ygfx_gfx gfx;
static ygfx_stim stims[1001];
static float xy[2 * 100000];
static yrt_event ring_mem[8192];
static yrt_ring ring;

#define MAXREC 40000
static uint32_t ph[3][MAXREC];

static int cmp_u(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : (x > y);
}

static double pct(uint32_t* v, int n, double p) {
    if (n <= 0) return 0;
    qsort(v, (size_t)n, sizeof v[0], cmp_u);
    return v[(int)(p * (n - 1))] * 1e-3;
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    yrt_ring_desc rd;
    yscr_frame f;
    ygfx_buf dots_buf = { 0 };
    int n_gabors = 0, n_dots = 0, i, n_stims = 0, nrec = 0, topmost = 0, allocs = 0;
    double seconds = 10;
    long dropped = 0, late = 0, flips = 0;
    uint32_t seed = 1;
    static ygfx_file_cache pcache;
    static char cache_dir[512];
    const ygfx_cache* cache = NULL;
    int no_cache = 0;
    char line[640];

    memset(&sd, 0, sizeof sd);
    sd.windowed = true;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sd.backend = YSCR_BACKEND_SIM;
        else if (!strcmp(argv[i], "--composition")) sd.backend = YSCR_BACKEND_COMPOSITION;
        else if (!strcmp(argv[i], "--gabors") && i + 1 < argc) n_gabors = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dots") && i + 1 < argc) n_dots = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--topmost")) topmost = 1;
        else if (!strcmp(argv[i], "--allocs")) allocs = 1;
        else if (!strcmp(argv[i], "--allocs-control")) allocs = 2;
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc && (cache = ygfx_file_cache_init(&pcache, argv[i + 1])) != NULL) i++;
        else if (!strcmp(argv[i], "--no-cache")) no_cache = 1;
        else {
            fprintf(stderr, "usage: gfx_load [--sim] [--gabors N] [--dots N] [--seconds S] [--composition] "
                            "[--topmost] [--allocs | --allocs-control] [--cache DIR] [--no-cache]\n");
            return 2;
        }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sd.backend == YSCR_BACKEND_SIM) yrt_virtual_begin(0);
    /* The per-user folder unless told otherwise, so that a second run loads
     * the programs instead of compiling them (PROGRAM CACHE). */
    if (no_cache) cache = NULL;
    else if (!cache && ygfx_default_cache_dir(cache_dir, sizeof cache_dir) == YGFX_OK)
        cache = ygfx_file_cache_init(&pcache, cache_dir);
    if (n_gabors < 0 || n_gabors > 1000 || n_dots < 0 || n_dots > 100000 || !(seconds > 0)) {
        fprintf(stderr, "gfx_load: gabors 0..1000, dots 0..100000, seconds > 0\n");
        return 2;
    }
    memset(&rd, 0, sizeof rd);
    rd.memory = ring_mem;
    rd.bytes = sizeof ring_mem;
    if (!yrt_ring_open(&ring, &rd)) { fprintf(stderr, "gfx_load: ring\n"); return 1; }
    sd.ring = &ring;
#if defined(_WIN32)
    if (topmost) {   /* open() settles (ysp/screen.h v0.5.0): one input event lets this
                     * program take the foreground during open, not after it */
        INPUT in0;
        memset(&in0, 0, sizeof in0);
        in0.type = INPUT_MOUSE;
        in0.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in0, sizeof in0);
    }
#endif
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "gfx_load: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    gd.max_draws = 1024;
    gd.cache = cache;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_load: %s\n", ygfx_error(&gfx)); return 1; }
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    ygfx_describe(&gfx, line, sizeof line);
    printf("%s\n", line);
#if defined(_WIN32)
    if (topmost && yscr_window(&scr)) yscr_window_call(&scr, take_foreground_on_window, yscr_window(&scr));
#else
    if (topmost) printf("--topmost acts on Win32 windows; ignored here\n");
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
    if (allocs) _CrtSetAllocHook(alloc_hook);
#else
    if (allocs) printf("--allocs needs an MSVC debug build; not counted\n");
#endif
    for (i = 0; i < n_gabors; i++) {
        ygfx_gabor_desc gb;
        memset(&gb, 0, sizeof gb);
        gb.sigma = 6; gb.sf = 0.08f; gb.contrast = 0.1f; gb.ori = (float)(i * 17 % 180);
        gb.place = YGFX_TOP_LEFT;
        gb.x = (float)(30 + (i * 37) % 740);
        gb.y = (float)(30 + (i * 53) % 540);
        stims[n_stims++] = ygfx_gabor(&gb);
    }
    if (n_dots) {
        ygfx_dots_desc dd;
        dots_buf = ygfx_buffer(&gfx, (size_t)n_dots * 8);
        memset(&dd, 0, sizeof dd);
        dd.buf = dots_buf; dd.count = (uint32_t)n_dots; dd.dot_size = 3; dd.edge = YGFX_EDGE_COSINE; dd.edge_width = 1;
        dd.color[0] = dd.color[1] = dd.color[2] = 0.56f;
        dd.aperture = YGFX_CIRCLE; dd.w = 500;
        stims[n_stims++] = ygfx_dots(&dd);
        for (i = 0; i < 2 * n_dots; i++) { seed = seed * 1664525u + 1013904223u; xy[i] = (float)(seed >> 8) / 16777216.0f * 500.0f - 250.0f; }
    }
    {
        int64_t t_end = 0;
        while (yscr_begin(&scr, &f) == YSCR_OK) {
            yrt_event ev[64];
            int n, k;
            if (f.index == 0) t_end = f.onset + (int64_t)(seconds * 1e9);
            g_count_allocs = allocs && f.index >= 30;
            if (allocs == 2 && f.index >= 30) free(malloc(16));   /* the control */
            for (k = 0; k < n_gabors; k++) stims[k].phase = (float)fmod((double)(f.onset % 1000000000) * 1e-9, 1.0);
            yscr_mark(&scr, YSCR_PHASE_EVALUATE);
            if (n_dots) {
                for (k = 0; k < 2 * n_dots; k++) {   /* a small random walk, wrapped */
                    seed = seed * 1664525u + 1013904223u;
                    xy[k] += (float)((int)(seed >> 29) - 3) * 0.5f;
                    if (xy[k] > 250) xy[k] -= 500;
                    if (xy[k] < -250) xy[k] += 500;
                }
                ygfx_buffer_update(&gfx, dots_buf, 0, xy, (size_t)n_dots * 8);
            }
            yscr_mark(&scr, YSCR_PHASE_UPLOAD);
            ygfx_begin(&gfx, &f);
            ygfx_draw_n(&gfx, stims, n_stims);
            ygfx_end(&gfx);
            yscr_flip(&scr);
            n = yrt_ring_drain(&ring, ev, 64);
            for (k = 0; k < n; k++) {
                unsigned flags;
                if (ev[k].source != YRT_SRC_SCREEN || ev[k].kind != YSCR_EV_FLIP) continue;
                flags = YSCR_EV_FLAGS_OF(ev[k].u.u16[5]);
                flips++;
                dropped += ev[k].u.u16[4];
                late += (flags & YSCR_FLIP_LATE_TARGET) != 0;
                if (nrec < MAXREC) {
                    ph[0][nrec] = ev[k].u.u32[3 + YSCR_PHASE_DRAW];
                    ph[1][nrec] = ev[k].u.u32[3 + YSCR_PHASE_UPLOAD];
                    ph[2][nrec] = ev[k].u.u32[3 + YSCR_PHASE_SWAP];
                    nrec++;
                }
            }
            g_count_allocs = 0;
            if (f.onset >= t_end) break;
        }
    }
    ygfx_close(&gfx);
    yscr_close(&scr);
    if (allocs) printf("C runtime heap calls in the frame loop after frame 30: %ld\n", (long)g_allocs);
    printf("gabors %d, dots %d: %ld flips, %ld dropped vblanks, %ld late targets\n", n_gabors, n_dots, flips, dropped, late);
    printf("DRAW p50 %.1f us p99 %.1f us; UPLOAD p50 %.1f us p99 %.1f us; SWAP p50 %.1f us p99 %.1f us\n",
           pct(ph[0], nrec, 0.5), pct(ph[0], nrec, 0.99), pct(ph[1], nrec, 0.5), pct(ph[1], nrec, 0.99),
           pct(ph[2], nrec, 0.5), pct(ph[2], nrec, 0.99));
    return 0;
}
