/* gfx_rdk.c - random dot kinematograms from ysp/rdk.h, drawn by ysp/gfx.h.
 *
 * Four pages in a 1000 x 760 window, units px:
 *   1  a classic RDK: 300 dots in a 400 px circle. Up and Down change the
 *      coherence by 0.05, Left and Right the direction by 45 deg. N cycles
 *      the noise rule, S the signal rule, E the edge (WRAP or REPLOT: with
 *      REPLOT and 100 % coherence the dots thin out toward the trailing
 *      edge), L the lifetime (none, 0.2 s, 0.5 s), P the presets (WN, MN,
 *      LL, BM, then custom again).
 *   2  the same motion carried by 300 instanced gabors (a global-motion
 *      gabor array): each element rides its dot, its carrier turned and
 *      drifting along the dot's direction at 4 Hz; an element restarts its
 *      carrier when its dot is placed (lifetime 0.4 s). Up, Down, Left,
 *      Right as on page 1.
 *   3  transparent motion: two fields of 150 dots, two streams of one
 *      seed, 180 deg apart; Left and Right change the angle between them.
 *   4  trials on a ysp/timeline.h sequence: 1 s of motion at 25.6 %, the
 *      direction turning 90 deg over the last 300 ms (a tween on a channel
 *      bound to the field's direction through ygfx_bind.field), then 300
 *      ms of dynamic noise (a NOISE stimulus with a new seed each frame),
 *      then a blank; every 2 s a new trial with a new seed. Each update's
 *      inputs are logged (rdk.last), and at the end of a trial the log is
 *      replayed into a second field with yrdk_replay(): the digests must
 *      agree, or the program says so and exits 1.
 * Space, Page Down: next page; Page Up: previous; 1 to 4: that page;
 * Shift+Esc or closing the window: quit. Settings print to stdout and the
 * title bar when they change.
 *
 * Contrast is low and the motion slow, because the display may be
 * somebody's working screen. No calibration: scene values are device
 * values here, and nothing is a measurement of light.
 *
 * Usage: gfx_rdk [--sim] [--page N] [--frames N] [--composition] [--shots PREFIX]
 *                [--cache DIR] [--no-cache]
 *   --sim          the simulated display (240 Hz) and the null backend, for
 *                  CI: 30 frames of each page, then two trials of page 4
 *                  with their replays checked
 *   --shots PREFIX show each page for 90 frames, read the output back, write
 *                  PREFIX-pN.ppm, then quit
 *   --cache DIR    keep compiled programs in DIR; the default is the per-user
 *                  folder of ygfx_default_cache_dir(), when there is one
 *   --no-cache     compile every program; read and write no cache file
 * Exit code: 0; 1 when the screen or the gfx did not open, a call failed,
 * or a replay differed; 2 for a bad argument.
 * On Windows set YSCR_ANGLE_DIR to ANGLE's directory.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"
#define YSP_RDK_IMPLEMENTATION
#include "ysp/rdk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN_W 1000
#define WIN_H 760
#define N_PAGES 4
#define SIM_FRAMES 30
#define MAX_DOTS 300
#define MAX_STEPS 1024  /* a trial is 2 s: 120 updates at 60 Hz, 480 at 240 */
#define S_NS YRT_S(1)

static yscr_screen scr;
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event tl_events[16];
static ytl_key tl_keys[16];

static yrdk_field rdk, rdk2, replay;
static yrdk_desc rdesc, rdesc2;
static ygfx_buf buf, buf2;
static ygfx_stim dots, dots2, gabors, mask, gabor_tmpl;
static ygfx_inst items[MAX_DOTS];
static float xy[2 * MAX_DOTS], xy2[2 * MAX_DOTS];
static float age[MAX_DOTS];
static uint8_t ev[MAX_DOTS];
static float carrier[MAX_DOTS];   /* each element's carrier phase, cycles */

static int page = 0, quit = 0, sim = 0, failures = 0, trials_done = 0;
static int preset = 0, life_choice = 0, transparent_angle = 180;
static uint64_t trial_seed = 1;
static int64_t page_t0, trial_t0, last_t;
static yrdk_step steps[MAX_STEPS];
static int n_steps;

enum { DOTS_ON, COH, DIR, MASK_ON, N_CH };
enum { TRIAL = 1 };

static ygfx_bind binds[4];

static const char* noise_name[] = { "DIRECTION", "POSITION", "WALK" };
static const char* signal_name[] = { "SAME", "DIFFERENT", "LEAST_RECENT" };
static const char* preset_name[] = { "custom", "WN", "MN", "LL", "BM" };

static void say(const char* what) {
    char line[256];
    snprintf(line, sizeof line, "gfx_rdk %d/%d: %s", page + 1, N_PAGES, what);
    printf("%s\n", line);
    if (yscr_window(&scr)) SDL_SetWindowTitle(yscr_window(&scr), line);   /* may allocate: key presses only */
}

static void describe_field(void) {
    char line[200];
    snprintf(line, sizeof line, "%s, coherence %.2f, direction %.0f, noise %s, signal %s, edge %s, lifetime %s",
             preset_name[preset], rdk.coherence, rdk.direction,
             noise_name[preset ? (preset == 4 ? 2 : 1) : rdesc.noise],
             signal_name[preset ? (preset == 3 ? 2 : 1) : rdesc.signal], rdesc.edge ? "REPLOT" : "WRAP",
             life_choice == 0 ? "none" : life_choice == 1 ? "0.2 s" : "0.5 s");
    say(line);
}

/* Opens the main field from rdesc, keeping the bound floats. Setup and key
 * presses only: it allocates. */
static int reopen(yrdk_field* f, yrdk_desc* d, int64_t t) {
    float c = f->coherence, dir = f->direction, sp = f->speed;
    int had = f->n > 0;
    yrdk_close(f);
    if (!yrdk_open(f, d)) { fprintf(stderr, "gfx_rdk: %s\n", yrdk_error(f)); failures++; return -1; }
    if (had) { f->coherence = c; f->direction = dir; f->speed = sp; }
    yrdk_start(f, trial_seed, t);
    return 0;
}

static void apply_page1_desc(void) {
    static const yrdk_algorithm algs[5] = { YRDK_CUSTOM, YRDK_WN, YRDK_MN, YRDK_LL, YRDK_BM };
    rdesc.algorithm = algs[preset];
    if (preset) { rdesc.signal = YRDK_SIGNAL_SAME; rdesc.select = YRDK_EXACT; rdesc.noise = YRDK_NOISE_DIRECTION; rdesc.sets = 0; }
    rdesc.lifetime_ns = life_choice == 0 ? 0 : life_choice == 1 ? YRT_MS(200) : YRT_MS(500);
    /* MN and LL show a third of the dots on each frame: 100 shown */
    rdesc.count = (preset == 2 || preset == 3) ? MAX_DOTS / 3 : MAX_DOTS;
}

static void start_page(int p, const yscr_frame* f) {
    page = p;
    page_t0 = f->onset;
    trial_seed++;
    memset(&rdesc, 0, sizeof rdesc);
    rdesc.w = 400; rdesc.coherence = 0.5f; rdesc.direction = 0; rdesc.speed = 80; rdesc.count = MAX_DOTS;
    preset = 0; life_choice = 0;
    if (p == 1) { rdesc.lifetime_ns = YRT_MS(400); rdesc.w = 500; }
    if (p == 2) { rdesc.count = MAX_DOTS / 2; rdesc.coherence = 1; rdesc.signal = YRDK_SIGNAL_SAME; }
    if (p == 3) { rdesc.coherence = 0.256f; rdesc.signal = YRDK_SIGNAL_DIFFERENT; rdesc.noise = YRDK_NOISE_POSITION; }
    rdk.n = 0;
    reopen(&rdk, &rdesc, f->onset);
    if (p == 2) {
        rdesc2 = rdesc;
        rdesc2.stream = 1;
        rdk2.n = 0;
        reopen(&rdk2, &rdesc2, f->onset);
        rdk2.direction = rdk.direction + (float)transparent_angle;
    }
    yrdk_close(&replay);
    if (p == 3) {
        if (!yrdk_open(&replay, &rdesc)) { fprintf(stderr, "gfx_rdk: %s\n", yrdk_error(&replay)); failures++; }
        trial_t0 = f->onset;
        ytl_anchor(&tl, TRIAL, f->onset, 0);
        n_steps = 0;
    }
    memset(carrier, 0, sizeof carrier);
    if (p == 0) describe_field();
    else if (p == 1) say("global-motion gabor array: Up, Down coherence; Left, Right direction");
    else if (p == 2) say("transparent motion: Left, Right change the angle between the fields");
    else say("trials: 25.6 % coherence, the direction turns 90 deg in the last 300 ms, then dynamic noise");
}

static void key(SDL_Keycode k, const yscr_frame* f) {
    char line[96];
    if (k == SDLK_SPACE || k == SDLK_PAGEDOWN) { start_page((page + 1) % N_PAGES, f); return; }
    if (k == SDLK_PAGEUP) { start_page((page + N_PAGES - 1) % N_PAGES, f); return; }
    if (k >= SDLK_1 && k < SDLK_1 + N_PAGES) { start_page((int)(k - SDLK_1), f); return; }
    if (page == 3) return;
    if (k == SDLK_UP || k == SDLK_DOWN) {
        float c = rdk.coherence + (k == SDLK_UP ? 0.05f : -0.05f);
        rdk.coherence = c < 0 ? 0 : c > 1 ? 1 : c;
    } else if (k == SDLK_LEFT || k == SDLK_RIGHT) {
        if (page == 2) {
            transparent_angle = (transparent_angle + (k == SDLK_RIGHT ? 45 : 315)) % 360;
            rdk2.direction = rdk.direction + (float)transparent_angle;
            snprintf(line, sizeof line, "transparent motion: %d deg between the fields", transparent_angle);
            say(line);
            return;
        }
        rdk.direction = fmodf(rdk.direction + (k == SDLK_RIGHT ? 45.0f : 315.0f), 360.0f);
    } else if (page == 0 && (k == SDLK_N || k == SDLK_S || k == SDLK_E || k == SDLK_L || k == SDLK_P)) {
        if (k == SDLK_P) preset = (preset + 1) % 5;
        if (k == SDLK_L) life_choice = (life_choice + 1) % 3;
        if (k == SDLK_N) { preset = 0; rdesc.noise = (yrdk_noise)((rdesc.noise + 1) % 3); }
        if (k == SDLK_S) { preset = 0; rdesc.signal = (yrdk_signal)((rdesc.signal + 1) % 3); }
        if (k == SDLK_E) rdesc.edge = (yrdk_edge)((rdesc.edge + 1) % 2);
        if (!preset) rdesc.algorithm = YRDK_CUSTOM;
        apply_page1_desc();
        rdesc.coherence = rdk.coherence; rdesc.direction = rdk.direction;
        reopen(&rdk, &rdesc, f->onset);
        dots.count = (uint32_t)rdk.n;
    } else {
        return;
    }
    if (page == 0) describe_field();
    else {
        snprintf(line, sizeof line, "coherence %.2f, direction %.0f", rdk.coherence, rdk.direction);
        say(line);
    }
}

static int setup(void) {
    ytl_desc td;
    ytl_seq q;
    ygfx_dots_desc dd;
    ygfx_gabor_desc gd;
    ygfx_noise_desc nd;
    ygfx_instances_desc id;
    ytl_tween_desc turn;
    static const float white[3] = { 0.85f, 0.85f, 0.85f };

    memset(&td, 0, sizeof td);
    td.events = tl_events; td.event_capacity = 16; td.n_channels = N_CH;
    td.keys = tl_keys; td.key_capacity = 16;
    if (!ytl_open(&tl, &td)) { fprintf(stderr, "gfx_rdk: %s\n", ytl_error(&tl)); return -1; }
    /* The trial, in the order it plays (GSAP: tl.set().to()...). */
    q = ytl_seq_on(&tl, TRIAL);
    ytl_set_value(&q, COH, 0.256f);
    ytl_on(&q, DOTS_ON);
    ytl_wait(&q, YRT_MS(700));
    memset(&turn, 0, sizeof turn);
    turn.from = 0; turn.from_set = true; turn.to = 90; turn.duration_ns = YRT_MS(300); turn.ease = YTL_EASE_COSINE;
    ytl_then(&q, DIR, &turn);
    ytl_off(&q, DOTS_ON);
    ytl_on(&q, MASK_ON);
    ytl_wait(&q, YRT_MS(300));
    ytl_off(&q, MASK_ON);
    if (q.err < 0) { fprintf(stderr, "gfx_rdk: the trial's sequence: %s at call %d\n", ytl_strerror(q.err), q.err_call); return -1; }

    memset(&rdesc, 0, sizeof rdesc);
    rdesc.w = 400; rdesc.count = MAX_DOTS; rdesc.coherence = 0.5f; rdesc.speed = 80;
    if (!yrdk_open(&rdk, &rdesc)) { fprintf(stderr, "gfx_rdk: %s\n", yrdk_error(&rdk)); return -1; }
    buf = ygfx_buffer(&gfx, sizeof xy);
    buf2 = ygfx_buffer(&gfx, sizeof xy2);
    if (!buf.id || !buf2.id) { fprintf(stderr, "gfx_rdk: %s\n", ygfx_error(&gfx)); return -1; }

    memset(&dd, 0, sizeof dd);
    dd.buf = buf; dd.count = MAX_DOTS; dd.dot_size = 5; dd.edge = YGFX_EDGE_COSINE; dd.edge_width = 1.5f;
    memcpy(dd.color, white, sizeof dd.color);
    dd.aperture = YGFX_CIRCLE; dd.w = 410;   /* the field's own edge keeps every center inside */
    dots = ygfx_dots(&dd);
    dd.buf = buf2;
    dots2 = ygfx_dots(&dd);

    memset(&gd, 0, sizeof gd);
    gd.sf = 1 / 10.0f; gd.sigma = 5; gd.contrast = 0.35f;
    gabor_tmpl = ygfx_gabor(&gd);
    memset(&id, 0, sizeof id);
    id.inst = items; id.n = MAX_DOTS; id.fields = YGFX_I_XY | YGFX_I_ORI | YGFX_I_PHASE;
    ygfx_inst_grid(items, MAX_DOTS, 1, 0, 0);
    gabors = ygfx_instances(&gfx, &gabor_tmpl, &id);
    if (gabors.n_inst == 0) { fprintf(stderr, "gfx_rdk: instances: %s\n", ygfx_error(&gfx)); return -1; }

    memset(&nd, 0, sizeof nd);
    nd.w = 410; nd.h = 410; nd.check = 4; nd.contrast = 0.25f; nd.aperture = YGFX_CIRCLE;
    mask = ygfx_noise(&nd);

    memset(binds, 0, sizeof binds);
    binds[0].stim = &dots; binds[0].param = YGFX_P_VISIBLE; binds[0].channel = DOTS_ON;
    binds[1].field = &rdk.coherence; binds[1].channel = COH;
    binds[2].field = &rdk.direction; binds[2].channel = DIR;
    binds[3].stim = &mask; binds[3].param = YGFX_P_VISIBLE; binds[3].channel = MASK_ON;
    return 0;
}

static int upload(ygfx_buf b, yrdk_field* f, float* out) {
    if (yrdk_xy(f, out) < 0) return -1;
    return ygfx_buffer_update(&gfx, b, 0, out, (size_t)f->n * 2 * sizeof(float));
}

/* Page 4: at the trial's end, replay its log into another field. */
static void end_trial(const yscr_frame* f) {
    uint64_t want = yrdk_digest(&rdk);
    if (yrdk_replay(&replay, trial_seed, trial_t0, steps, n_steps) < 0) {
        fprintf(stderr, "gfx_rdk: replay: %s\n", yrdk_error(&replay));
        failures++;
    } else if (yrdk_digest(&replay) != want) {
        fprintf(stderr, "gfx_rdk: the replay of trial %llu differs (%016llx, live %016llx)\n",
                (unsigned long long)trial_seed, (unsigned long long)yrdk_digest(&replay), (unsigned long long)want);
        failures++;
    } else {
        printf("trial seed %llu: %d updates, digest %016llx, replayed the same\n", (unsigned long long)trial_seed, n_steps,
               (unsigned long long)want);
    }
    trials_done++;
    trial_seed++;
    trial_t0 = f->onset;
    ytl_anchor(&tl, TRIAL, f->onset, 0);   /* base time 0 again: the trial replays */
    yrdk_start(&rdk, trial_seed, f->onset);
    n_steps = 0;
}

static int frame(const yscr_frame* f) {
    int rc = 0, i;
    if (page == 3) {
        if (f->onset - trial_t0 >= 2 * S_NS || n_steps >= MAX_STEPS) end_trial(f);
        ytl_evaluate(&tl, &(ytl_frame){ f->onset, f->period, f->index }, NULL, 0);
        ygfx_apply(binds, 4, ytl_values(&tl));
        mask.seed = (float)(uint32_t)(f->index & 0xffffff);   /* a new pattern each frame */
    }
    yscr_mark(&scr, YSCR_PHASE_EVALUATE);
    if (yrdk_update(&rdk, f->onset) < 0) return -1;
    if (page == 3) steps[n_steps++] = rdk.last;
    if (page == 2 && yrdk_update(&rdk2, f->onset) < 0) return -1;
    if (page == 1) {
        /* elements ride the dots; the carrier drifts along each dot's
         * direction at 4 Hz and restarts when the dot is placed */
        yrdk_out o;
        double dt = last_t ? (double)(f->onset - last_t) * 1e-9 : 0;
        memset(&o, 0, sizeof o);
        o.xy = &items[0].x; o.xy_stride = sizeof items[0];
        o.dir = &items[0].ori; o.dir_stride = sizeof items[0];
        o.event = ev; o.age_s = age;
        if (yrdk_write(&rdk, &o) < 0) return -1;
        for (i = 0; i < rdk.n; i++) {
            carrier[i] = (ev[i] & YRDK_EV_PLACED) ? 0.0f : (float)fmod(carrier[i] - 4.0 * dt, 1.0);
            items[i].phase = carrier[i];   /* cos(2 pi (sf x + phase)): a falling phase moves it along +x */
        }
    } else {
        rc |= upload(buf, &rdk, xy);
        if (page == 2) rc |= upload(buf2, &rdk2, xy2);
    }
    last_t = f->onset;
    yscr_mark(&scr, YSCR_PHASE_UPLOAD);
    dots.count = (uint32_t)rdk.n;
    dots2.count = (uint32_t)rdk2.n;
    ygfx_begin(&gfx, f);
    if (page == 1) rc |= ygfx_draw(&gfx, &gabors);
    else rc |= ygfx_draw(&gfx, &dots);
    if (page == 2) rc |= ygfx_draw(&gfx, &dots2);
    if (page == 3) rc |= ygfx_draw(&gfx, &mask);
    rc |= ygfx_end(&gfx);
    if (rc < 0) { fprintf(stderr, "gfx_rdk: %s\n", ygfx_error(&gfx)); return -1; }
    return 0;
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    yscr_frame f;
    int i, start = 0, on_page = 0, started = 0;
    int64_t frames = -1;
    const char* shots = NULL;
    uint8_t* shot_px = NULL;
    float scr_w, scr_h;
    static ygfx_file_cache pcache;
    static char cache_dir[512];
    const ygfx_cache* cache = NULL;
    int no_cache = 0;
    char line[640];
    memset(&sd, 0, sizeof sd);
    sd.windowed = true; sd.window_w = WIN_W; sd.window_h = WIN_H;
    sd.sim_period_ns = 4166667;   /* 240 Hz when --sim */
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) { sd.backend = YSCR_BACKEND_SIM; sim = 1; }
        else if (!strcmp(argv[i], "--composition")) sd.backend = YSCR_BACKEND_COMPOSITION;
        else if (!strcmp(argv[i], "--page") && i + 1 < argc) { start = atoi(argv[++i]) - 1; if (start < 0 || start >= N_PAGES) start = -1; }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { frames = atoll(argv[++i]); if (frames < 1) start = -1; }
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) shots = argv[++i];
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc) { cache = ygfx_file_cache_init(&pcache, argv[++i]); if (!cache) start = -1; }
        else if (!strcmp(argv[i], "--no-cache")) no_cache = 1;
        else start = -1;
        if (start < 0) {
            fprintf(stderr, "usage: gfx_rdk [--sim] [--page 1..%d] [--frames N] [--composition] [--shots PREFIX] "
                            "[--cache DIR] [--no-cache]\n", N_PAGES);
            return 2;
        }
    }
    /* The per-user folder unless told otherwise, so that a second run loads
     * the programs instead of compiling them (PROGRAM CACHE). */
    if (no_cache) cache = NULL;
    else if (!cache && ygfx_default_cache_dir(cache_dir, sizeof cache_dir) == YGFX_OK)
        cache = ygfx_file_cache_init(&pcache, cache_dir);
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "gfx_rdk: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.18f;
    gd.width = WIN_W; gd.height = WIN_H;   /* the simulated display's size */
    gd.cache = cache;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_rdk: %s\n", ygfx_error(&gfx)); yscr_close(&scr); return 1; }
    ygfx_describe(&gfx, line, sizeof line);
    printf("%s\n", line);
    if (setup() < 0) { ygfx_close(&gfx); yscr_close(&scr); return 1; }
    ygfx_size(&gfx, &scr_w, &scr_h);
    if (shots) shot_px = (uint8_t*)malloc((size_t)scr_w * (size_t)scr_h * 4);

    while (yscr_begin(&scr, &f) == YSCR_OK) {
        if (yscr_window(&scr)) {
            SDL_Event e;
            while (yscr_poll(&scr, &e, NULL)) {   /* yscr_begin() reports Shift+Esc and close itself */
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = 1;
                else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) { key(e.key.key, &f); on_page = 0; }
            }
        }
        if (!started) { start_page(start, &f); started = 1; }
        if (frame(&f) < 0) { failures++; break; }
        if (shot_px && on_page == 89) {   /* the back buffer, before the flip */
            char path[512];
            snprintf(path, sizeof path, "%s-p%d.ppm", shots, page + 1);
            if (ygfx_read_output(&gfx, 0, 0, (int)scr_w, (int)scr_h, shot_px) < 0 ||
                ygfx_image_write(path, shot_px, (int)scr_w, (int)scr_h, 0, NULL, NULL, 0) < 0)
                fprintf(stderr, "gfx_rdk: could not write %s\n", path);
            else
                printf("wrote %s\n", path);
        }
        yscr_flip(&scr);
        on_page++;
        if (shots && on_page >= 90) {
            if (page == N_PAGES - 1) break;
            start_page(page + 1, &f);
            on_page = 0;
        }
        if (sim && (page == 3 ? trials_done >= 2 : on_page >= SIM_FRAMES)) {
            if (page == N_PAGES - 1 || (page + 1) % N_PAGES == start) break;
            start_page(page + 1, &f);
            on_page = 0;
        }
        if (quit || (frames > 0 && f.index + 1 >= frames)) break;
    }
    yrdk_close(&rdk);
    yrdk_close(&rdk2);
    yrdk_close(&replay);
    free(shot_px);
    ygfx_close(&gfx);
    yscr_close(&scr);
    if (failures) { fprintf(stderr, "gfx_rdk: %d failures\n", failures); return 1; }
    return 0;
}
