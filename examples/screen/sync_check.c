/* screen_sync_check.c - is the display synced to the vblank? Your eyes and
 * ysp/screen.h's sync guard answer together.
 *
 * Mirrors Psychtoolbox-3's PerceptualVBLSyncTest.m (full-screen flicker
 * where tearing shows) and the moving bar of OSXCompositorIdiocyTest.m.
 * Read for coverage only; no code is copied.
 *
 * WARNING: the default pattern flickers the whole screen between black and
 * white at half the refresh rate (30 Hz on a 60 Hz display). Flicker in
 * this range can cause seizures in people with photosensitive epilepsy.
 * Do not run it where such a person can see the screen; --no-flicker
 * shows the same test without flicker, at low contrast.
 *
 * What it shows: the screen alternates black and white on every flip, and
 * a vertical bar of the other color moves 8 pixels to the right per flip.
 * Synced, the flicker is even and the bar moves in one piece. Not synced
 * (tearing), the bar breaks into pieces offset sideways, and bands of black
 * and white cross the screen and jump or crawl. A bar that stops for a
 * moment, or uneven flicker with an unbroken bar, is a dropped frame, not
 * tearing. Each second it prints what the flip records show: flips
 * completed per refresh, flips off the vblank grid, drops, early and
 * estimated flips, the presentation path and its changes, and the sync
 * guard's evidence (SYNC GUARD in ysp/screen.h). At the end: the guard's
 * verdict, and its message if it fired.
 *
 * To test a driver: set its panel to force vsync off (AMD Software "Wait
 * for Vertical Refresh: Always off", NVIDIA Control Panel "Vertical sync:
 * Off", Intel Graphics "Vertical Sync: Speed"), run this, then set the
 * panel back to the application's choice and run it again.
 *
 * Usage: screen_sync_check [--sim | --sim-vsync-off] [--no-flicker] [--windowed]
 *                          [--backend dxgi|composition] [--seconds N]
 *   --sim            the simulated display (CI): no window, nothing drawn
 *   --sim-vsync-off  a simulated swap path that behaves like a driver with
 *                    vsync forced off (each flip in the refresh it was
 *                    presented in), so the guard fires (CI)
 *   --no-flicker     dark gray, with a lighter bar: tearing still shows
 *   --windowed       an 800 x 600 window instead of borderless fullscreen
 *   --backend B      dxgi (DXGI_FLIP) or composition (COMPOSITION)
 *   --seconds N      run time (default 10, simulated 2)
 * Shift+Esc or closing the window ends the run.
 * Exit code: 0 synced, 3 the guard fired (not synced), 1 when the screen
 * did not open, 2 for a bad argument.
 */
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #define GLCALL __stdcall
#else
    #define GLCALL
#endif
static void (GLCALL *glClearColor_)(float, float, float, float);
static void (GLCALL *glClear_)(unsigned int);
static void (GLCALL *glScissor_)(int, int, int, int);
static void (GLCALL *glEnable_)(unsigned int);
static void (GLCALL *glDisable_)(unsigned int);
static void (GLCALL *glViewport_)(int, int, int, int);

/* --- --sim-vsync-off: a swap path with vsync forced off ------------------- */

/* A 60 Hz grid on the ysp/rt.h clock. The slot is free at once and each
 * present flips in the refresh it was made in, reported at that refresh's
 * vblank: what a driver that ignores the sync interval would report. */
typedef struct off_path {
    int64_t  t0, period, count, t;
    uint64_t id;
    int      has;
} off_path;

static int off_open(void* ctx, const yscr_presenter_open* in, yscr_caps* caps, char* err, size_t cap) {
    off_path* o = (off_path*)ctx;
    (void)in; (void)err; (void)cap;
    o->t0 = yrt_now_ns();
    o->period = 16666667;
    caps->kind = YSCR_FIXED_GRID;
    caps->period_ns = o->period;
    caps->hw_onset = true;
    caps->max_in_flight = 1;
    caps->mode.w = 800;
    caps->mode.h = 600;
    caps->mode.refresh_num = 60;
    caps->mode.refresh_den = 1;
    caps->mode.period_ns = o->period;
    return YSCR_OK;
}
static void off_close(void* ctx) { (void)ctx; }
static int off_acquire(void* ctx, int64_t deadline_ns, yscr_vblank* newest) {
    (void)ctx; (void)deadline_ns;
    newest->t_ns = 0;
    return YSCR_OK;
}
static int off_present(void* ctx, const yscr_present_req* req) {
    off_path* o = (off_path*)ctx;
    o->count = (yrt_now_ns() - o->t0) / o->period;
    o->t = o->t0 + o->count * o->period;
    o->id = req->present_id;
    o->has = 1;
    return YSCR_OK;
}
static int off_completions(void* ctx, yscr_vblank* out, int cap) {
    off_path* o = (off_path*)ctx;
    if (!o->has || cap < 1) return 0;
    memset(out, 0, sizeof *out);
    out->present_id = o->id;
    out->t_ns = o->t;
    out->count = o->count;
    out->path = YSCR_PATH_INDEPENDENT;
    o->has = 0;
    return 1;
}
static const yscr_presenter off_presenter = {
    YSCR_PRESENTER_VERSION, "vsync-off (simulated)", false, false, false,
    off_open, off_close, off_acquire, off_present, off_completions, NULL, NULL, NULL, NULL
};

/* --- drawing ------------------------------------------------------------- */

static void fill(int x, int y, int w, int h, float g) {
    glScissor_(x, y, w, h);
    glClearColor_(g, g, g, 1.0f);
    glClear_(0x4000u);   /* GL_COLOR_BUFFER_BIT */
}

/* The background flips each frame; the bar steps 8 pixels per frame. */
static void draw(const yscr_screen* s, int64_t frame, int flicker) {
    int w = s->caps.mode.w, h = s->caps.mode.h, bar = w / 16 > 8 ? w / 16 : 8;
    int x = (int)((frame * 8) % (w + bar)) - bar;
    float bg = flicker ? (float)(frame & 1) : 0.2f, fg = flicker ? 1.0f - bg : 0.45f;
    if (!glClear_ || w <= 0 || h <= 0) return;
    glViewport_(0, 0, w, h);
    glEnable_(0x0C11u);   /* GL_SCISSOR_TEST */
    fill(0, 0, w, h, bg);
    fill(x < 0 ? 0 : x, 0, x < 0 ? bar + x : bar, h, fg);
    glDisable_(0x0C11u);
}

static const char* path_name(int p) {
    static const char* const n[] = { "unknown", "composed", "overlay", "independent", "simulated" };
    return p >= 0 && p <= 4 ? n[p] : "?";
}

/* --- the run --------------------------------------------------------------- */

typedef struct tally { int flips, unstable, drops, early, estimated, changes; } tally;

static void print_second(int sec, const tally* t, double refreshes, int path, const yscr_sync_info* si) {
    printf("%3d s: %4d flips / %6.1f refreshes = %.2f per refresh; off grid %d, dropped %d, early %d, "
           "estimated %d; path %s (%d changes); guard %d of %d%s\n",
           sec, t->flips, refreshes, refreshes > 0 ? t->flips / refreshes : 0.0, t->unstable, t->drops, t->early,
           t->estimated, path_name(path), t->changes, si->evidence, si->window, si->untimed ? " FIRED" : "");
}

int main(int argc, char** argv) {
    static yscr_screen scr;
    static off_path off;
    yscr_desc d;
    yscr_frame f;
    yscr_sync_info si;
    tally sec_t, all;
    int i, rc = YSCR_OK, sim = 0, sim_off = 0, flicker = 1, windowed = 0, path = -1, sec = 0, told = 0;
    double seconds = 0;
    int64_t t_start = 0, t_sec = 0;
    yscr_backend backend = YSCR_BACKEND_AUTO;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--sim-vsync-off")) sim_off = 1;
        else if (!strcmp(argv[i], "--no-flicker")) flicker = 0;
        else if (!strcmp(argv[i], "--windowed")) windowed = 1;
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "dxgi")) backend = YSCR_BACKEND_DXGI_FLIP;
            else if (!strcmp(argv[i], "composition")) backend = YSCR_BACKEND_COMPOSITION;
            else { fprintf(stderr, "screen_sync_check: --backend dxgi|composition\n"); return 2; }
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            seconds = atof(argv[++i]);
            if (seconds <= 0 || seconds > 600) { fprintf(stderr, "screen_sync_check: --seconds 0..600\n"); return 2; }
        } else {
            fprintf(stderr, "usage: screen_sync_check [--sim | --sim-vsync-off] [--no-flicker] [--windowed] "
                            "[--backend dxgi|composition] [--seconds N]\n");
            return 2;
        }
    }
    if (sim && sim_off) { fprintf(stderr, "screen_sync_check: --sim or --sim-vsync-off, not both\n"); return 2; }
    if (seconds == 0) seconds = sim || sim_off ? 2 : 10;
    if (flicker && !sim && !sim_off)
        printf("WARNING: the whole screen flickers black and white at half the refresh rate. Flicker in this\n"
               "range can cause seizures in people with photosensitive epilepsy. Shift+Esc stops it;\n"
               "--no-flicker runs the same test without flicker.\n");

    memset(&d, 0, sizeof d);
    d.backend = sim ? YSCR_BACKEND_SIM : sim_off ? YSCR_BACKEND_CUSTOM : backend;
    d.presenter = sim_off ? &off_presenter : NULL;
    d.presenter_ctx = sim_off ? &off : NULL;
    d.windowed = windowed;
    /* this check reports a driver that forces vsync off; under the default
     * (strict fullscreen) open() would refuse such a display (ysp/screen.h
     * v0.5.0, SETTLE), so it warns and the verdict below says why */
    d.settle = YSCR_SETTLE_WARN;
    if (!yscr_open(&scr, &d)) { fprintf(stderr, "screen_sync_check: %s\n", yscr_error(&scr)); return 1; }
    {
        char line[1024];
        yscr_describe(&scr, line, sizeof line);
        printf("%s\n", line);
    }
    glClearColor_ = (void (GLCALL*)(float, float, float, float))yscr_gl_proc(&scr, "glClearColor");
    glClear_ = (void (GLCALL*)(unsigned int))yscr_gl_proc(&scr, "glClear");
    glScissor_ = (void (GLCALL*)(int, int, int, int))yscr_gl_proc(&scr, "glScissor");
    glEnable_ = (void (GLCALL*)(unsigned int))yscr_gl_proc(&scr, "glEnable");
    glDisable_ = (void (GLCALL*)(unsigned int))yscr_gl_proc(&scr, "glDisable");
    glViewport_ = (void (GLCALL*)(int, int, int, int))yscr_gl_proc(&scr, "glViewport");
    if (!glClear_ || !glClearColor_ || !glScissor_ || !glEnable_ || !glDisable_ || !glViewport_) glClear_ = NULL;

    memset(&sec_t, 0, sizeof sec_t);
    memset(&all, 0, sizeof all);
    while ((rc = yscr_begin(&scr, &f)) == YSCR_OK) {
        if (!t_start) t_start = t_sec = f.onset;
        for (i = 0; i < f.n_done; i++) {
            const yscr_record* r = &f.done[i];
            sec_t.flips++;
            sec_t.unstable += (r->flags & YSCR_FLIP_GRID_UNSTABLE) != 0;
            sec_t.drops += (int)r->dropped;
            sec_t.early += (r->flags & YSCR_FLIP_EARLY) != 0;
            sec_t.estimated += (r->flags & YSCR_FLIP_ESTIMATED) != 0;
            if (path >= 0 && r->path != path) sec_t.changes++;
            path = r->path;
        }
        yscr_sync_check(&scr, &si);
        if (si.untimed && !told) {
            printf("the sync guard fired at frame %lld: %s\n", (long long)si.fired_index, si.message);
            told = 1;
        }
        if (f.onset - t_sec >= 1000000000) {
            /* refreshes from the clock: a record has no vblank count */
            print_second(++sec, &sec_t, f.period ? (double)(f.onset - t_sec) / (double)f.period : 0, path, &si);
            all.flips += sec_t.flips;
            all.unstable += sec_t.unstable;
            all.drops += sec_t.drops;
            all.early += sec_t.early;
            all.estimated += sec_t.estimated;
            all.changes += sec_t.changes;
            memset(&sec_t, 0, sizeof sec_t);
            t_sec = f.onset;
        }
        draw(&scr, f.index, flicker);
        if (yscr_flip(&scr) != YSCR_OK) break;
        if ((double)(f.onset - t_start) >= seconds * 1e9) break;
    }
    if (rc == YSCR_OK) yscr_wait_flip(&scr, NULL);
    all.flips += sec_t.flips;   /* the last part second */
    all.unstable += sec_t.unstable;
    all.drops += sec_t.drops;
    all.early += sec_t.early;
    all.estimated += sec_t.estimated;
    all.changes += sec_t.changes;
    yscr_sync_check(&scr, &si);
    yscr_close(&scr);
    if (rc < 0) { fprintf(stderr, "screen_sync_check: %s\n", yscr_strerror(rc)); return 1; }

    printf("\nflips %d: off grid %d, dropped vblanks %d, early %d, estimated %d, path changes %d\n", all.flips,
           all.unstable, all.drops, all.early, all.estimated, all.changes);
    printf("sync guard: %u flips with an OS time; evidence: same refresh %u, no vblank wait %u, torn %u; "
           "at most %d of 64 at once (32 fire)\n", (unsigned)si.observed, (unsigned)si.same_refresh,
           (unsigned)si.no_wait, (unsigned)si.torn, si.peak);
    if (si.untimed) {
        printf("verdict: NOT SYNCED. %s\n", si.message);
        return 3;
    }
    printf("verdict: synced as far as the flip records show. The records cannot see a driver that tears but\n"
           "reports the next vblank: if the bar broke or bands crossed the screen, it is not synced.\n");
    return 0;
}
