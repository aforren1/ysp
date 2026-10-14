/* gfx_hello.c - a gabor in a window: ysp/gfx.h's USAGE example.
 *
 * The frame loop is the manual's ten lines. A mid-gray 800 x 600 window, a
 * gabor of sigma 32 px and 1/32 cycle per pixel at contrast 0.5 in its
 * center, drifting at 1 Hz: its phase is computed from the predicted onset
 * of each frame. Low contrast and small, because the display may be
 * somebody's working screen.
 *
 * Usage: gfx_hello [--sim] [--frames N] [--cache DIR] [--no-cache]
 *   --sim       no window and no GL: the simulated display and the null
 *               backend, for CI
 *   --frames N  stop after N frames (default: until Shift+Esc or the window closes)
 *   --cache DIR keep compiled programs in DIR; the default is the per-user
 *               folder of ygfx_default_cache_dir(), when there is one
 *   --no-cache  compile every program; read and write no cache file
 * Exit code: 0, 1 when the screen or the gfx did not open, 2 for a bad
 * argument.
 */
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static yscr_screen scr;
static ygfx_gfx gfx;

/* yscr_open() calls this once its GL context exists, before it settles
 * the display (0.5 to 2.5 s). The gfx submits its programs and returns; the
 * driver compiles them on its own threads while the display settles, and
 * ygfx_open() waits for the rest (ysp/screen.h CONTEXT HOOK, ysp/gfx.h
 * OPEN START). A failure here is reported by ygfx_open(). */
static void start_gfx(void* ctx, yscr_screen* s) {
    ygfx_desc* gd = (ygfx_desc*)ctx;
    gd->screen = s;
    ygfx_open_start(&gfx, gd);
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    ygfx_gabor_desc gab;
    ygfx_stim g;
    yscr_frame f;
    int64_t t0 = 0, frames = -1;
    static ygfx_file_cache pcache;
    static char cache_dir[512];
    const ygfx_cache* cache = NULL;
    int i, no_cache = 0;
    char line[600];

    memset(&sd, 0, sizeof sd);
    sd.windowed = true;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sd.backend = YSCR_BACKEND_SIM;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc && (cache = ygfx_file_cache_init(&pcache, argv[i + 1])) != NULL) i++;
        else if (!strcmp(argv[i], "--no-cache")) no_cache = 1;
        else { fprintf(stderr, "usage: gfx_hello [--sim] [--frames N] [--cache DIR] [--no-cache]\n"); return 2; }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sd.backend == YSCR_BACKEND_SIM) yrt_virtual_begin(0);
    /* The per-user folder unless told otherwise, so that a second run loads
     * the programs instead of compiling them (PROGRAM CACHE). */
    if (no_cache) cache = NULL;
    else if (!cache && ygfx_default_cache_dir(cache_dir, sizeof cache_dir) == YGFX_OK)
        cache = ygfx_file_cache_init(&pcache, cache_dir);

    /* In C99 these are compound literals with designated initializers:
     *   ygfx_desc gd = { .background = { 0.5f, 0.5f, 0.5f } };
     *   yscr_open(&scr, &(yscr_desc){ .windowed = true, .on_context = start_gfx, .on_context_ctx = &gd });
     *   ygfx_open(&gfx, &gd);
     *   ygfx_gabor(&(ygfx_gabor_desc){ .sf = 1 / 32.0f, .sigma = 32, .contrast = 0.5f });
     * Written out field by field here so the file also builds as C++17. */
    memset(&gd, 0, sizeof gd);
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    gd.cache = cache;
    sd.on_context = start_gfx;
    sd.on_context_ctx = &gd;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "gfx_hello: %s\n", yscr_error(&scr)); ygfx_close(&gfx); return 1; }
    gd.screen = &scr;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_hello: %s\n", ygfx_error(&gfx)); return 1; }
    memset(&gab, 0, sizeof gab);
    gab.sf = 1 / 32.0f;
    gab.sigma = 32;
    gab.contrast = 0.5f;
    g = ygfx_gabor(&gab);
    ygfx_prime(&gfx);   /* each program's first draw now, not in the first frames */
    ygfx_describe(&gfx, line, sizeof line);
    printf("%s\n", line);

    while (yscr_begin(&scr, &f) == YSCR_OK) {   /* Shift+Esc ends it */
        if (f.index == 0) t0 = f.onset;
        g.phase = (float)fmod(1.0 * (double)(f.onset - t0) * 1e-9, 1.0);   /* 1 Hz drift */
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &g);
        ygfx_end(&gfx);
        yscr_flip(&scr);
        if (frames > 0 && f.index + 1 >= frames) break;
    }
    ygfx_close(&gfx);
    yscr_close(&scr);
    return 0;
}
