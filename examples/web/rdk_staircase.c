/* web_rdk_staircase.c - random dot motion discrimination with a staircase
 * on coherence, as a web demo: ysp/rdk.h moves the dots, ysp/stair.h
 * picks each trial's coherence, ysp/response.h takes the arrow key, and
 * the page shows the threshold estimate and offers the rows as a CSV.
 *
 * Each trial: a fixation cross for 0.5 s, then 150 white dots in a circle
 * of 360 units (4 units, 200 units/s; 480 units are the canvas's shorter
 * side) until a response or 2 s. Left or Right arrow, or a tap or click on
 * the left or right half of the canvas (web_common.h), for the direction
 * of the coherent dots (180 or 0 deg, at random). The cross turns green or
 * red for 0.3 s, then a blank of 0.5 s.
 *
 * The staircase: 2-down 1-up on log10 coherence (70.7 % correct; Levitt
 * 1971), from 0.5, steps 0.3, 0.15 and 0.1 log10 units, coherence 0.01 to
 * 1, 1-down 1-up until the first reversal. It stops after 8 reversals or
 * 40 trials; the estimate is the geometric mean of the last 6 reversals. A
 * trial with no response is not given to the staircase; 60 trials with
 * those end the run.
 *
 * Columns: trial, coherence, direction, response, rsp_kind (keyboard,
 * touch or mouse), rt_s, correct, reversal, onset_tier, frames_dropped,
 * then the constant columns coi, clock_step_s and audio. In a browser the
 * onset is the browser's frame time (tier 5), not light (docs/web.md).
 *
 * --sim: the simulated display and a simulated observer (a Weibull with a
 * 70.7 % point at coherence 0.15, slope 2, lapse 0.02; RT 0.5 s). It
 * checks that the staircase stopped and that the estimate is within a
 * factor of 2 of 0.15, and exits 1 otherwise.
 *
 * Usage: web_rdk_staircase [--sim] [--seed N] [--size WxH]
 *   --size WxH  the window in CSS pixels (default 720x480; the page passes
 *               its stage's size)
 * Exit code: 0 (also after Shift+Esc), 1 when something did not open or a
 * --sim check failed, 2 for a bad argument.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"
#define YSP_RDK_IMPLEMENTATION
#include "ysp/rdk.h"
#define YSP_STAIR_IMPLEMENTATION
#include "ysp/stair.h"
#include "web_common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
EM_JS(void, web_done, (const char* csv, const char* screen, double threshold, int n_rev), {
    Module['yspCsv'] = UTF8ToString(csv);
    Module['yspScreen'] = UTF8ToString(screen);
    Module['yspThreshold'] = threshold;
    Module['yspReversals'] = n_rev;
    if (Module['onYspDone']) Module['onYspDone']();
})
EM_JS_DEPS(web_rdk_deps, "$UTF8ToString")
#endif

enum { FIX_ON, FIX_RIGHT, FIX_WRONG, DOTS_ON, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };

#define FIXATION_S 0.5
#define RESPONSE_S 2.0
#define MIN_RT_S   0.1
#define FEEDBACK_S 0.3
#define BLANK_S    0.5
#define N_DOTS     150
#define FIELD_PX   360
#define SIM_THRESHOLD 0.15
#define MAX_TRIALS 60       /* with misses: a participant who left ends the run */

/* The arrow keys, and the canvas halves for touch and the mouse (web_side). */
static const yrsp_choice keys[] = {
    { .key = "left" }, { .key = "right" },
    { .name = "left", .kind = YRSP_KIND_TOUCH, .match = YRSP_MATCH_KEYCODE, .code = 1 },
    { .name = "right", .kind = YRSP_KIND_TOUCH, .match = YRSP_MATCH_KEYCODE, .code = 2 },
    { .name = "left", .kind = YRSP_KIND_MOUSE, .match = YRSP_MATCH_KEYCODE, .code = 1 },
    { .name = "right", .kind = YRSP_KIND_MOUSE, .match = YRSP_MATCH_KEYCODE, .code = 2 },
};
#define N_KEYS (int)(sizeof keys / sizeof keys[0])

typedef struct row {
    int         trial;
    double      coherence;
    int         direction;
    const char* response;
    const char* rsp_kind;
    int64_t     rt_ns;
    bool        correct;
    bool        reversal;
    int         onset_tier, frames_dropped;
} row;

static yscr_screen scr;
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_data data;
static yrsp_collector rsp;
static yst_stair stair;
static yrdk_field rdk;
static yrdk_desc rdesc;
static ygfx_buf buf;
static ygfx_stim fix, fix_right, fix_wrong, dots;
static float xy[2 * N_DOTS];
static char csv[1 << 16];
static size_t csv_len;
static const ygfx_bind binds[] = {
    { .stim = &fix,       .param = YGFX_P_VISIBLE, .channel = FIX_ON    },
    { .stim = &fix_right, .param = YGFX_P_VISIBLE, .channel = FIX_RIGHT },
    { .stim = &fix_wrong, .param = YGFX_P_VISIBLE, .channel = FIX_WRONG },
    { .stim = &dots,      .param = YGFX_P_VISIBLE, .channel = DOTS_ON   },
};

static bool to_memory(void* ctx, const void* bytes, size_t n) {
    (void)ctx;
    if (csv_len + n >= sizeof csv) return false;
    memcpy(csv + csv_len, bytes, n);
    csv_len += n;
    csv[csv_len] = 0;
    return true;
}

static ygfx_stim cross(float r, float g, float b, float u) {
    ygfx_shape_desc d;
    memset(&d, 0, sizeof d);
    d.shape = YGFX_CROSS;
    d.w = d.h = 16 * u;
    d.shape_p[0] = 3 * u;
    d.edge = YGFX_EDGE_COSINE;
    d.edge_width = 1;
    d.color[0] = r; d.color[1] = g; d.color[2] = b;
    return ygfx_shape(&d);
}

static double uniform(uint64_t* s) { return ytr_splitmix(s); }

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    ygfx_dots_desc dd;
    ytl_desc td;
    ytr_data_desc wd;
    yst_desc stdesc;
    yrsp_desc pd;
    yrsp_sdl_ctx sdl;
    yrsp_source sources[3];
    yscr_caps caps;
    yscr_frame f;
    yrt_clock_info ci;
    ytl_event fired[16];
    yrsp_result res;
    yrsp_input sim_ev[2];
    row r;
    char line[1536], step[32], coi[4];
    uint64_t seed = 20261010, rng;
    int i, n, sim = 0, aborted = 0, failed = 0, trial = 0, n_sim_ev = 0, sim_next = 0;
    int in_trial = 0, responding = 0, moving = 0, answer = 0, dropped = 0, win_w = 720, win_h = 480;
    int64_t stim_frame = -1, motion_t0 = 0;
    double coherence = 0, threshold = NAN;
    float u, scr_w, scr_h;

    memset(&sd, 0, sizeof sd);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc && web_size_arg(argv[i + 1], &win_w, &win_h)) i++;
        else { fprintf(stderr, "usage: web_rdk_staircase [--sim] [--seed N] [--size WxH]\n"); return 2; }
    }
    rng = seed;
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = true;
    sd.window_w = win_w;
    sd.window_h = win_h;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "web_rdk_staircase: %s\n", yscr_error(&scr)); return 1; }
    yscr_get_caps(&scr, &caps);
    yrt_get_clock_info(&ci);
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.25f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "web_rdk_staircase: %s\n", ygfx_error(&gfx)); return 1; }
    /* 480 units on the canvas's shorter side, in its pixels */
    ygfx_size(&gfx, &scr_w, &scr_h);
    u = (scr_w < scr_h ? scr_w : scr_h) / 480.0f;
    if (!(u > 0)) u = 1;

    memset(&rdesc, 0, sizeof rdesc);
    rdesc.w = FIELD_PX * u;
    rdesc.count = N_DOTS;
    rdesc.speed = 200 * u;
    if (!yrdk_open(&rdk, &rdesc)) { fprintf(stderr, "web_rdk_staircase: %s\n", yrdk_error(&rdk)); return 1; }
    buf = ygfx_buffer(&gfx, sizeof xy);
    memset(&dd, 0, sizeof dd);
    dd.buf = buf;
    dd.count = N_DOTS;
    dd.dot_size = 4 * u;
    dd.aperture = YGFX_CIRCLE;
    dd.w = FIELD_PX * u;
    dd.color[0] = dd.color[1] = dd.color[2] = 1.0f;
    dots = ygfx_dots(&dd);
    fix = cross(0.85f, 0.85f, 0.85f, u);
    fix_right = cross(0.1f, 0.75f, 0.1f, u);
    fix_wrong = cross(0.85f, 0.1f, 0.1f, u);

    memset(&stdesc, 0, sizeof stdesc);
    stdesc.start = 0.5;
    stdesc.n_up = 1;
    stdesc.n_down = 2;
    stdesc.step_type = YST_STEP_LOG;
    stdesc.steps[0] = 0.3;
    stdesc.steps[1] = 0.15;
    stdesc.steps[2] = 0.1;
    stdesc.n_steps = 3;
    stdesc.min = 0.01;
    stdesc.max = 1.0;
    stdesc.initial_rule = true;
    stdesc.stop_reversals = 8;
    stdesc.stop_trials = 40;
    stdesc.est_reversals = 6;
    if (!yst_open(&stair, &stdesc)) { fprintf(stderr, "web_rdk_staircase: %s\n", yst_error(&stair)); return 1; }

    yscr_describe(&scr, line, sizeof line);
    snprintf(coi, sizeof coi, "%d", strstr(line, " coi=1") ? 1 : 0);
    snprintf(step, sizeof step, "%.9f", (double)ci.resolution_ns * 1e-9);
    {
        const ytr_col cols[] = {
            YTR_COL(row, trial, YTR_COL_INT),
            YTR_COL_D(row, coherence, YTR_COL_DOUBLE, 4),
            YTR_COL(row, direction, YTR_COL_INT),
            YTR_COL(row, response, YTR_COL_STRING),
            YTR_COL(row, rsp_kind, YTR_COL_STRING),
            YTR_COL_D(row, rt_ns, YTR_COL_TIME_NS, 6),
            YTR_COL(row, correct, YTR_COL_BOOL),
            YTR_COL(row, reversal, YTR_COL_BOOL),
            YTR_COL(row, onset_tier, YTR_COL_INT),
            YTR_COL(row, frames_dropped, YTR_COL_INT),
            YTR_COL_TEXT("coi", coi),
            YTR_COL_TEXT("clock_step_s", step),
            YTR_COL_TEXT("audio", "none"),
        };
        memset(&wd, 0, sizeof wd);
        wd.write = to_memory;
        wd.cols = cols;
        wd.n_cols = (int)(sizeof cols / sizeof cols[0]);
        if (!ytr_data_open(&data, &wd)) { fprintf(stderr, "web_rdk_staircase: %s\n", ytr_data_error(&data)); return 1; }
    }

    memset(&td, 0, sizeof td);
    td.events = storage;
    td.event_capacity = 64;
    td.n_channels = N_CHANNELS;
    if (!ytl_open(&tl, &td)) { fprintf(stderr, "web_rdk_staircase: %s\n", ytl_error(&tl)); return 1; }

    memset(&sdl, 0, sizeof sdl);
    sdl.raw_keyboard = caps.raw_keyboard;
    sources[0] = yrsp_sdl_source(&sdl, YRSP_KIND_KEYBOARD);
    sources[1] = yrsp_sdl_source(&sdl, YRSP_KIND_TOUCH);
    sources[2] = yrsp_sdl_source(&sdl, YRSP_KIND_MOUSE);
    memset(&pd, 0, sizeof pd);
    pd.choices = keys;
    pd.n_choices = N_KEYS;
    pd.duration_ns = YRT_S(RESPONSE_S);
    pd.min_rt_ns = YRT_S(MIN_RT_S);
    pd.sources = sources;
    pd.n_sources = 3;
    if (!yrsp_open(&rsp, &pd)) { fprintf(stderr, "web_rdk_staircase: %s\n", yrsp_error(&rsp)); return 1; }

    printf("%s\n", line);
    /* every program's first draw, and the dots' first upload, before the
     * page shows the canvas */
    yrdk_start(&rdk, seed, 0);
    yrdk_update(&rdk, 0);
    yrdk_xy(&rdk, xy);
    ygfx_buffer_update(&gfx, buf, 0, xy, sizeof xy);
    ygfx_prime(&gfx);
    web_ready();
    for (;;) {
        SDL_Event ev;
        int64_t bt = 0;
        yrsp_input in;
        const float* val;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "web_rdk_staircase: %s\n", yscr_error(&scr)); failed = 1; break; }
        for (i = 0; i < f.n_done; i++) {
            if (moving && f.done[i].dropped) dropped++;
            if (stim_frame >= 0 && f.done[i].index == stim_frame) {
                yrsp_onset o = yrsp_onset_flip(&f.done[i]);
                yrsp_set_onset(&rsp, &o);
            }
        }

        if (!in_trial) {
            ytl_seq q;
            if (yst_done(&stair) || trial >= MAX_TRIALS) break;   /* misses are not the staircase's */
            coherence = yst_next(&stair);
            answer = uniform(&rng) < 0.5;   /* 1: rightward (0 deg) */
            rdk.coherence = (float)coherence;
            rdk.direction = answer ? 0.0f : 180.0f;
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            ytl_on(&q, FIX_ON);
            ytl_wait(&q, YRT_S(FIXATION_S));
            ytl_on(&q, DOTS_ON);
            if (q.err) { fprintf(stderr, "web_rdk_staircase: timeline error %d\n", q.err); failed = 1; break; }
            yrsp_arm(&rsp, f.onset);
            stim_frame = -1;
            in_trial = responding = 1;
            moving = dropped = 0;
            n_sim_ev = sim_next = 0;
        }

        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) { web_side(&scr, &in); yrsp_feed(&rsp, &in); }
        for (; sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset; sim_next++)
            yrsp_feed(&rsp, &sim_ev[sim_next]);

        if (responding && yrsp_update(&rsp, f.onset) == YRSP_ENDED) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            int ok;
            yrsp_finish(&rsp, &res);
            ok = (res.flags & YRSP_R_RESPONDED) && res.response_name && !strcmp(res.response_name, answer ? "right" : "left");
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, DOTS_ON);
            ytl_off(&q, FIX_ON);
            ytl_on(&q, ok ? FIX_RIGHT : FIX_WRONG);
            ytl_wait(&q, YRT_S(FEEDBACK_S));
            ytl_off(&q, ok ? FIX_RIGHT : FIX_WRONG);
            ytl_wait(&q, YRT_S(BLANK_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "web_rdk_staircase: timeline error %d\n", q.err); failed = 1; break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 16);
        for (i = 0; i < n && i < 16; i++) {
            if (fired[i].kind == YTL_ONSET && fired[i].target == DOTS_ON) {
                yrsp_onset o = yrsp_onset_landing(&fired[i]);
                yrsp_set_onset(&rsp, &o);
                stim_frame = fired[i].frame;
                if (sim) {   /* the observer: a Weibull on coherence */
                    double pc = 0.5 + (0.5 - 0.02) * (1.0 - exp(-pow(coherence / (SIM_THRESHOLD / 1.0498), 2.0)));
                    int key = uniform(&rng) < pc ? answer : !answer;
                    memset(sim_ev, 0, sizeof sim_ev);
                    sim_ev[0].t = f.onset + YRT_MS(500);
                    sim_ev[0].kind = YRSP_KIND_KEYBOARD;
                    sim_ev[0].type = YRSP_PRESS;
                    sim_ev[0].control = (uint32_t)yrsp_scancode(key ? "right" : "left");
                    sim_ev[0].stamp = YRSP_TIER_SIM;
                    sim_ev[0].value = 1;
                    sim_ev[1] = sim_ev[0];
                    sim_ev[1].type = YRSP_RELEASE;
                    sim_ev[1].value = 0;
                    sim_ev[1].t += YRT_MS(60);
                    n_sim_ev = 2;
                    sim_next = 0;
                }
            }
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                const yst_trial* h;
                int nh;
                yrsp_finish(&rsp, &res);
                memset(&r, 0, sizeof r);
                r.trial = trial;
                r.coherence = coherence;
                r.direction = answer ? 0 : 180;
                r.response = res.response_name;
                r.rsp_kind = (res.flags & YRSP_R_RESPONDED) ? web_kind_name(res.kind) : NULL;
                r.rt_ns = res.rt_ns == YRSP_NO_TIME ? YTR_MISSING_NS : res.rt_ns;
                r.correct = (res.flags & YRSP_R_RESPONDED) && res.response_name &&
                            !strcmp(res.response_name, answer ? "right" : "left");
                r.onset_tier = res.onset_tier;
                r.frames_dropped = dropped;
                if (res.flags & YRSP_R_RESPONDED) {   /* a miss is not the staircase's */
                    if (yst_update(&stair, coherence, r.correct) < 0) {
                        fprintf(stderr, "web_rdk_staircase: %s\n", yst_error(&stair));
                        failed = 1;
                    }
                    h = yst_history(&stair, &nh);
                    r.reversal = h && nh > 0 && h[nh - 1].reversal;
                }
                if (ytr_data_write(&data, &r) < 0) { fprintf(stderr, "web_rdk_staircase: %s\n", ytr_data_error(&data)); failed = 1; }
                printf("trial %2d coherence %.3f %s %s\n", trial, coherence, answer ? "right" : "left ",
                       !(res.flags & YRSP_R_RESPONDED) ? "(too slow)" : r.correct ? "correct" : "wrong");
                trial++;
                in_trial = moving = 0;
            }
        }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));

        /* the dots at this frame's onset, on the trial's motion clock */
        val = ytl_values(&tl);
        if (val[DOTS_ON] > 0.5f) {
            if (!moving) {
                motion_t0 = f.onset;
                yrdk_start(&rdk, seed * 1000003u + (uint64_t)trial, 0);
                moving = 1;
            }
            yrdk_update(&rdk, f.onset - motion_t0);
            yrdk_xy(&rdk, xy);
            ygfx_buffer_update(&gfx, buf, 0, xy, sizeof xy);
        }
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &dots);
        ygfx_draw(&gfx, &fix);
        ygfx_draw(&gfx, &fix_right);
        ygfx_draw(&gfx, &fix_wrong);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    if (yst_estimate_count(&stair, YST_EST_REVERSALS) > 0) threshold = yst_estimate(&stair, YST_EST_REVERSALS);   /* LOG steps: the geometric mean */
    if (ytr_data_finish(&data) < 0) { fprintf(stderr, "web_rdk_staircase: %s\n", ytr_data_error(&data)); failed = 1; }
    ytr_data_close(&data);
    yscr_wait_flip(&scr, NULL);
    yscr_describe(&scr, line, sizeof line);
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d trials, threshold (70.7 %% correct) %.3f coherence from %d reversals%s\n", trial, threshold,
           yst_estimate_count(&stair, YST_EST_REVERSALS), aborted ? " (stopped with Shift+Esc)" : "");
#if defined(__EMSCRIPTEN__)
    web_done(csv, line, threshold, yst_estimate_count(&stair, YST_EST_REVERSALS));
#endif
    if (sim && !failed) {
        if (!yst_done(&stair) || !(threshold > SIM_THRESHOLD / 2 && threshold < SIM_THRESHOLD * 2)) {
            fprintf(stderr, "web_rdk_staircase: --sim: estimate %.4f, the observer's 0.15 (seed %llu; 40 trials are "
                            "few, so another seed can miss)\n", threshold, (unsigned long long)seed);
            return 1;
        }
        printf("--sim: the staircase stopped near the observer's threshold\n");
    }
    yst_close(&stair);
    return failed;
}
