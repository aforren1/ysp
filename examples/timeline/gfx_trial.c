/* gfx_trial.c - the ysp/timeline.h trial with ysp/gfx.h stimuli.
 *
 * A fixation cross from 0 to 0.5 s, then a grating in a circular aperture
 * with a raised-cosine edge, whose contrast ramps up over 100 ms with a
 * raised cosine, holds at 0.3, and ramps down to end at 1.2 s; a trigger
 * at the grating's onset. The timeline writes the stimuli's fields itself
 * (ysp/timeline.h TARGETS), as GSAP's timeline.set() and .to() do: sets of
 * `visible` that land where events at the same times land, and tweens of
 * the grating's contrast. Each fired event (the trigger) is printed with
 * its frame and residual.
 *
 * Usage: gfx_trial [--sim] [--cache DIR] [--no-cache]
 *   --cache DIR keep compiled programs in DIR; the default is the per-user
 *               folder of ygfx_default_cache_dir(), when there is one
 *   --no-cache  compile every program; read and write no cache file
 * Exit code: 0, 1 when the screen or the gfx did not open, 2 for a bad
 * argument.
 */
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"

#include <stdio.h>
#include <string.h>

enum { TRIAL = 1 };
#define S YRT_S(1)

static ytl_event storage[64];
static ytl_target_slot targets[8];
static ytl_tween_slot tweens[16];
static ytl_timeline tl;
static yscr_screen scr;
static ygfx_gfx gfx;
static ygfx_stim fix, grating;

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
    ytl_desc td;
    ygfx_shape_desc fd;
    ygfx_grating_desc grd;
    yscr_frame f;
    ytl_event fired[8];
    ytl_seq q;
    static ygfx_file_cache pcache;
    static char cache_dir[512];
    const ygfx_cache* cache = NULL;
    int i, n, no_cache = 0;
    int64_t t0 = 0;
    char line[600];

    memset(&sd, 0, sizeof sd);
    sd.windowed = true;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sd.backend = YSCR_BACKEND_SIM;
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc && (cache = ygfx_file_cache_init(&pcache, argv[i + 1])) != NULL) i++;
        else if (!strcmp(argv[i], "--no-cache")) no_cache = 1;
        else { fprintf(stderr, "usage: gfx_trial [--sim] [--cache DIR] [--no-cache]\n"); return 2; }
    }
    /* The per-user folder unless told otherwise, so that a second run loads
     * the programs instead of compiling them (PROGRAM CACHE). */
    if (no_cache) cache = NULL;
    else if (!cache && ygfx_default_cache_dir(cache_dir, sizeof cache_dir) == YGFX_OK)
        cache = ygfx_file_cache_init(&pcache, cache_dir);
    memset(&td, 0, sizeof td);
    td.events = storage; td.event_capacity = 64;
    td.targets = targets; td.target_capacity = 8; td.tweens = tweens; td.tween_capacity = 16;
    if (!ytl_open(&tl, &td)) { fprintf(stderr, "%s\n", ytl_error(&tl)); return 1; }
    /* The trial, in the order it plays. A field is the caller's until its
     * first tween starts, so the stimuli below start hidden. */
    q = ytl_seq_on(&tl, TRIAL);
    ytl_tween_set(&q, &fix.visible, 1.0f);
    ytl_wait(&q, S / 2);
    ytl_tween_set(&q, &fix.visible, 0.0f);
    ytl_tween_set(&q, &grating.visible, 1.0f);
    ytl_trigger(&q, 12);
    ytl_tween_from_to(&q, &grating.contrast, 0.0f, 0.3f, S / 10, YTL_EASE_COSINE);
    ytl_wait(&q, 6 * S / 10);
    ytl_tween_to(&q, &grating.contrast, 0.0f, S / 10, YTL_EASE_COSINE);
    ytl_wait(&q, S / 10);
    ytl_tween_set(&q, &grating.visible, 0.0f);
    if (q.err < 0) { fprintf(stderr, "%s\n", ytl_error(&tl)); return 1; }

    memset(&gd, 0, sizeof gd);
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    gd.cache = cache;
    sd.on_context = start_gfx;
    sd.on_context_ctx = &gd;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "gfx_trial: %s\n", yscr_error(&scr)); ygfx_close(&gfx); return 1; }
    gd.screen = &scr;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_trial: %s\n", ygfx_error(&gfx)); return 1; }
    ygfx_describe(&gfx, line, sizeof line);
    printf("%s\n", line);

    memset(&fd, 0, sizeof fd);
    fd.shape = YGFX_CROSS; fd.w = fd.h = 16; fd.shape_p[0] = 3;
    fd.edge = YGFX_EDGE_COSINE; fd.edge_width = 1;
    fd.color[0] = fd.color[1] = fd.color[2] = 0.35f;
    fix = ygfx_shape(&fd);
    memset(&grd, 0, sizeof grd);
    grd.w = 200; grd.sf = 1 / 20.0f; grd.ori = 45;
    grd.aperture = YGFX_CIRCLE; grd.edge = YGFX_EDGE_COSINE; grd.edge_width = 20;
    grating = ygfx_grating(&grd);
    fix.visible = grating.visible = 0.0f;   /* the timeline turns them on */
    ygfx_prime(&gfx);   /* each program's first draw now, not in the trial */

    while (yscr_begin(&scr, &f) == YSCR_OK) {
        if (f.index == 0) { ytl_anchor(&tl, TRIAL, f.onset, 0); t0 = f.onset; }
        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 8);
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &fix);
        ygfx_draw(&gfx, &grating);
        ygfx_end(&gfx);
        for (i = 0; i < n && i < 8; i++)
            printf("frame %3lld  kind %d ch %d code %2d  at %+8.3f ms, residual %+.3f ms\n",
                   (long long)fired[i].frame, fired[i].kind, fired[i].target, fired[i].code,
                   (double)(fired[i].onset - t0) / 1e6, (double)fired[i].residual / 1e6);
        yscr_flip(&scr);
        if (f.onset - t0 > 13 * S / 10 || f.index > 400) break;
    }
    ygfx_close(&gfx);
    yscr_close(&scr);
    return 0;
}
