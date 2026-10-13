/* web_tutorial.c - the tutorial trial as a web demo: a Gabor tilted left
 * or right, a keyboard response with its reaction time from the flip that
 * showed the Gabor, and one data row per trial from ysp/trials.h's
 * writer, which the page offers as a CSV download.
 *
 * Each trial: a fixation cross for 0.5 s, then the Gabor until a response
 * or 2 s. F when the bars lean left, J when they lean right (the physical
 * keys), or a tap or click on the left or right half of the canvas (the
 * same collector: web_common.h). Responses before 0.1 s are
 * anticipations. Feedback for 0.4 s (a green ring when correct, red when
 * wrong, gray when too slow), then a blank of 0.6 s. The page shows the
 * instructions before the first frame and the results after the last;
 * the stimuli are made and primed before the page shows the canvas.
 * Sizes are in units of the canvas: 480 units are its shorter side.
 *
 * The data rows go to memory through ytr_data's write function, not to a
 * file: in a browser the page hands them to the visitor. Columns: the
 * session's (index, block, ..., outcome, tilt, key_answer), then response
 * (left or right), rsp_kind (keyboard, touch or mouse), rt_s, correct,
 * onset_tier, onset_src, rsp_tier, n_anticipations, frames_dropped, and
 * the constant columns coi (cross-origin isolated), clock_step_s and
 * audio. Times in seconds. In a browser onset_tier is 5
 * (YSCR_TIER_WEB): the onset is the browser's frame time, not a
 * measurement of light, and the RT carries the browser's unknown delay
 * (docs/web.md). The same program runs natively with the OS's flip times.
 *
 * --sim runs on the simulated display with a scripted participant (no
 * keyboard): correct, an anticipation then a response, wrong, a timeout,
 * and correct. It checks every row and exits 1 on a mismatch.
 *
 * Usage: web_tutorial [--sim] [--reps N] [--seed N] [--out FILE] [--size WxH]
 *   --reps N   repetitions of each tilt (default 5; --sim: 3)
 *   --out FILE also write the CSV to FILE (native; default none)
 *   --size WxH the window in CSS pixels (default 720x480; the page passes
 *              its stage's size)
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
#include "web_common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
/* The page renders the rows and offers the download. */
EM_JS(void, web_done, (const char* csv, const char* screen), {
    Module['yspCsv'] = UTF8ToString(csv);
    Module['yspScreen'] = UTF8ToString(screen);
    if (Module['onYspDone']) Module['onYspDone']();
})
EM_JS_DEPS(web_tutorial_deps, "$UTF8ToString")
#endif

enum { FIX_ON, GABOR_L, GABOR_R, FB_RIGHT, FB_WRONG, FB_NONE, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };

#define FIXATION_S 0.5
#define RESPONSE_S 2.0
#define MIN_RT_S   0.1
#define FEEDBACK_S 0.4
#define BLANK_S    0.6
#define TILT_DEG   20.0f

static const char conditions_csv[] =
    "tilt,key_answer\n"
    "left,f\n"
    "right,j\n";

/* The keys, and the canvas halves for touch and the mouse (web_side). */
static const yrsp_choice keys[] = {
    { .key = "f", .name = "left" }, { .key = "j", .name = "right" },
    { .name = "left", .kind = YRSP_KIND_TOUCH, .match = YRSP_MATCH_KEYCODE, .code = 1 },
    { .name = "right", .kind = YRSP_KIND_TOUCH, .match = YRSP_MATCH_KEYCODE, .code = 2 },
    { .name = "left", .kind = YRSP_KIND_MOUSE, .match = YRSP_MATCH_KEYCODE, .code = 1 },
    { .name = "right", .kind = YRSP_KIND_MOUSE, .match = YRSP_MATCH_KEYCODE, .code = 2 },
};
#define N_KEYS (int)(sizeof keys / sizeof keys[0])

/* --sim's participant, by trial: a press at `at` s after the onset; which
 * 1 the correct key, 2 the other. Trial 3 has no press: a timeout. */
typedef struct sim_press { int trial; double at; int which; } sim_press;
static const sim_press sim_script[] = {
    { 0, 0.420, 1 }, { 1, 0.060, 1 }, { 1, 0.510, 1 }, { 2, 0.380, 2 }, { 4, 0.450, 1 }, { 5, 0.600, 1 },
};
#define SIM_TRIALS 6

/* One row of the caller's columns. */
typedef struct row {
    const char* response;
    const char* rsp_kind;
    int64_t     rt_ns;           /* YTR_MISSING_NS: none */
    bool        correct;
    int         onset_tier, onset_src, rsp_tier, n_anticipations, frames_dropped;
} row;

static yscr_screen scr;
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static ytr_data data;
static yrsp_collector rsp;
static uint64_t table_arena[2048];
static ytb_table tab;
static char csv[1 << 16];
static size_t csv_len;
static FILE* csv_file;

static ygfx_stim fix, gabor_l, gabor_r, fb_right, fb_wrong, fb_none;
static const ygfx_bind binds[] = {
    { .stim = &fix,      .param = YGFX_P_VISIBLE, .channel = FIX_ON   },
    { .stim = &gabor_l,  .param = YGFX_P_VISIBLE, .channel = GABOR_L  },
    { .stim = &gabor_r,  .param = YGFX_P_VISIBLE, .channel = GABOR_R  },
    { .stim = &fb_right, .param = YGFX_P_VISIBLE, .channel = FB_RIGHT },
    { .stim = &fb_wrong, .param = YGFX_P_VISIBLE, .channel = FB_WRONG },
    { .stim = &fb_none,  .param = YGFX_P_VISIBLE, .channel = FB_NONE  },
};

/* ytr_data's bytes: kept in memory for the page, and in --out's file. */
static bool to_memory(void* ctx, const void* bytes, size_t n) {
    (void)ctx;
    if (csv_len + n >= sizeof csv) return false;
    memcpy(csv + csv_len, bytes, n);
    csv_len += n;
    csv[csv_len] = 0;
    if (csv_file && fwrite(bytes, 1, n, csv_file) != n) return false;
    return true;
}

static ygfx_stim shape(ygfx_shape_kind kind, float w, float p0, float r, float g, float b) {
    ygfx_shape_desc d;
    memset(&d, 0, sizeof d);
    d.shape = kind;
    d.w = d.h = w;
    d.shape_p[0] = p0;
    d.edge = YGFX_EDGE_COSINE;
    d.edge_width = 1;
    d.color[0] = r; d.color[1] = g; d.color[2] = b;
    return ygfx_shape(&d);
}

static ygfx_stim gabor(float ori, float u) {
    ygfx_gabor_desc d;
    memset(&d, 0, sizeof d);
    d.ori = ori;
    d.sf = 1.0f / (28.0f * u);    /* cycles per pixel */
    d.sigma = 26.0f * u;
    d.contrast = 0.8f;
    return ygfx_gabor(&d);
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    ytl_desc td;
    ytb_csv_desc cd;
    ytr_desc rd;
    ytr_data_desc dd;
    yrsp_desc pd;
    yrsp_sdl_ctx sdl;
    yrsp_source sources[3];
    yscr_caps caps;
    yscr_frame f;
    yrt_clock_info ci;
    ytl_event fired[16];
    ytr_trial_info ti = { 0 };
    yrsp_result res, sim_res[SIM_TRIALS];
    yrsp_input sim_ev[8];
    row r;
    char line[1536], step[32], coi[4];
    const char* out_path = NULL;
    uint64_t seed = 20261010;
    int i, n, sim = 0, reps = 0, aborted = 0, failed = 0, rows = 0, dropped = 0, win_w = 720, win_h = 480;
    int in_trial = 0, responding = 0, n_sim_ev = 0, sim_next = 0, stim_ch = 0, tilt_col, key_col;
    int64_t stim_frame = -1;
    float u, scr_w, scr_h;
    char key_answer[8] = "", tilt[8] = "";

    memset(&sd, 0, sizeof sd);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc && web_size_arg(argv[i + 1], &win_w, &win_h)) i++;
        else {
            fprintf(stderr, "usage: web_tutorial [--sim] [--reps N] [--seed N] [--out FILE] [--size WxH]\n");
            return 2;
        }
    }
    if (reps == 0) reps = sim ? SIM_TRIALS / 2 : 5;
    if (reps < 1 || reps > 50) { fprintf(stderr, "web_tutorial: --reps takes 1..50\n"); return 2; }
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = true;
    sd.window_w = win_w;
    sd.window_h = win_h;

    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "web_tutorial: %s\n", yscr_error(&scr)); return 1; }
    yscr_get_caps(&scr, &caps);
    yrt_get_clock_info(&ci);
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "web_tutorial: %s\n", ygfx_error(&gfx)); return 1; }
    /* 480 units on the canvas's shorter side, in its pixels: the same look
     * on a phone, a HiDPI laptop and the 720 x 480 window */
    ygfx_size(&gfx, &scr_w, &scr_h);
    u = (scr_w < scr_h ? scr_w : scr_h) / 480.0f;
    if (!(u > 0)) u = 1;
    fix = shape(YGFX_CROSS, 16 * u, 3 * u, 0.15f, 0.15f, 0.15f);
    gabor_l = gabor(-TILT_DEG, u);
    gabor_r = gabor(TILT_DEG, u);
    fb_right = shape(YGFX_ANNULUS, 60 * u, 24 * u, 0.1f, 0.6f, 0.1f);
    fb_wrong = shape(YGFX_ANNULUS, 60 * u, 24 * u, 0.75f, 0.1f, 0.1f);
    fb_none = shape(YGFX_ANNULUS, 60 * u, 24 * u, 0.35f, 0.35f, 0.35f);

    memset(&cd, 0, sizeof cd);
    cd.text = conditions_csv;
    cd.len = sizeof conditions_csv - 1;
    cd.arena = table_arena;
    cd.arena_size = sizeof table_arena;
    if (!ytb_open_csv(&tab, &cd)) { fprintf(stderr, "web_tutorial: %s\n", ytb_error(&tab)); return 1; }
    tilt_col = ytb_col(&tab, "tilt");
    key_col = ytb_col(&tab, "key_answer");
    memset(&rd, 0, sizeof rd);
    rd.table = &tab;
    rd.reps = reps;
    rd.order = YTR_ORDER_RANDOM;
    rd.rng = ytr_splitmix;
    rd.rng_ctx = &seed;
    if (!ytr_open(&trials, &rd)) { fprintf(stderr, "web_tutorial: %s\n", ytr_error(&trials)); return 1; }

    /* The rows: the session's columns, ours, and the session's setting. */
    snprintf(coi, sizeof coi, "%d", strstr(yscr_describe(&scr, line, sizeof line) >= 0 ? line : "", " coi=1") ? 1 : 0);
    snprintf(step, sizeof step, "%.9f", (double)ci.resolution_ns * 1e-9);
    {
        const ytr_col cols[] = {
            YTR_COL(row, response, YTR_COL_STRING),
            YTR_COL(row, rsp_kind, YTR_COL_STRING),
            YTR_COL_D(row, rt_ns, YTR_COL_TIME_NS, 6),
            YTR_COL(row, correct, YTR_COL_BOOL),
            YTR_COL(row, onset_tier, YTR_COL_INT),
            YTR_COL(row, onset_src, YTR_COL_INT),
            YTR_COL(row, rsp_tier, YTR_COL_INT),
            YTR_COL(row, n_anticipations, YTR_COL_INT),
            YTR_COL(row, frames_dropped, YTR_COL_INT),
            YTR_COL_TEXT("coi", coi),
            YTR_COL_TEXT("clock_step_s", step),
            YTR_COL_TEXT("audio", "none"),
        };
        if (out_path && !(csv_file = fopen(out_path, "wb"))) { fprintf(stderr, "web_tutorial: cannot write %s\n", out_path); return 1; }
        memset(&dd, 0, sizeof dd);
        dd.write = to_memory;
        dd.trials = &trials;
        dd.cols = cols;
        dd.n_cols = (int)(sizeof cols / sizeof cols[0]);
        if (!ytr_data_open(&data, &dd)) { fprintf(stderr, "web_tutorial: %s\n", ytr_data_error(&data)); return 1; }
    }

    memset(&td, 0, sizeof td);
    td.events = storage;
    td.event_capacity = 64;
    td.n_channels = N_CHANNELS;
    if (!ytl_open(&tl, &td)) { fprintf(stderr, "web_tutorial: %s\n", ytl_error(&tl)); return 1; }

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
    if (!yrsp_open(&rsp, &pd)) { fprintf(stderr, "web_tutorial: %s\n", yrsp_error(&rsp)); return 1; }

    printf("%s\n", line);
    /* every program's first draw before the page shows the canvas */
    ygfx_prime(&gfx);
    web_ready();
    for (;;) {
        SDL_Event ev;
        int64_t bt = 0;
        yrsp_input in;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "web_tutorial: %s\n", yscr_error(&scr)); failed = 1; break; }

        for (i = 0; i < f.n_done; i++) {
            if (in_trial && f.done[i].dropped) dropped++;
            if (stim_frame >= 0 && f.done[i].index == stim_frame) {
                yrsp_onset o = yrsp_onset_flip(&f.done[i]);
                yrsp_set_onset(&rsp, &o);
            }
        }

        if (!in_trial) {
            ytl_seq q;
            const char* ka;
            if (ytr_next(&trials, &ti) < 0) break;
            ka = ytb_text(&tab, ti.condition, key_col);
            snprintf(key_answer, sizeof key_answer, "%s", ka ? ka : "");
            ka = ytb_text(&tab, ti.condition, tilt_col);
            snprintf(tilt, sizeof tilt, "%s", ka ? ka : "");
            stim_ch = !strcmp(tilt, "left") ? GABOR_L : GABOR_R;
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            ytl_on(&q, FIX_ON);
            ytl_wait(&q, YRT_S(FIXATION_S));
            ytl_off(&q, FIX_ON);
            ytl_on(&q, stim_ch);
            if (q.err) { fprintf(stderr, "web_tutorial: timeline error %d\n", q.err); failed = 1; break; }
            yrsp_arm(&rsp, f.onset);
            stim_frame = -1;
            in_trial = responding = 1;
            n_sim_ev = sim_next = 0;
            dropped = 0;
        }

        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) { web_side(&scr, &in); yrsp_feed(&rsp, &in); }
        for (; sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset; sim_next++)
            if (yscr_key_filter(&scr, &sim_ev[sim_next])) yrsp_feed(&rsp, &sim_ev[sim_next]);

        if (responding && yrsp_update(&rsp, f.onset) == YRSP_ENDED) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            int fb;
            yrsp_finish(&rsp, &res);
            fb = !(res.flags & YRSP_R_RESPONDED) ? FB_NONE
                 : res.response_name && !strcmp(res.response_name, tilt) ? FB_RIGHT : FB_WRONG;
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, stim_ch);
            ytl_on(&q, fb);
            ytl_wait(&q, YRT_S(FEEDBACK_S));
            ytl_off(&q, fb);
            ytl_wait(&q, YRT_S(BLANK_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "web_tutorial: timeline error %d\n", q.err); failed = 1; break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 16);
        for (i = 0; i < n && i < 16; i++) {
            if (fired[i].kind == YTL_ONSET && fired[i].target == stim_ch) {
                yrsp_onset o = yrsp_onset_landing(&fired[i]);
                yrsp_set_onset(&rsp, &o);
                stim_frame = fired[i].frame;
                if (sim) {   /* the participant's presses, from this onset */
                    int k;
                    n_sim_ev = 0;
                    for (k = 0; k < (int)(sizeof sim_script / sizeof sim_script[0]) && n_sim_ev + 2 <= 8; k++) {
                        const sim_press* p = &sim_script[k];
                        const char* key = p->which == 1 ? key_answer : !strcmp(key_answer, "f") ? "j" : "f";
                        uint32_t sc = (uint32_t)yrsp_scancode(key);
                        yrsp_input* e = &sim_ev[n_sim_ev++];
                        if (p->trial != ti.index) { n_sim_ev--; continue; }
                        memset(e, 0, sizeof *e);
                        e->t = f.onset + YRT_S(p->at);
                        e->kind = YRSP_KIND_KEYBOARD;
                        e->type = YRSP_PRESS;
                        e->control = sc;
                        e->code = (uint32_t)key[0];
                        e->stamp = YRSP_TIER_SIM;
                        e->value = 1;
                        sim_ev[n_sim_ev] = *e;
                        sim_ev[n_sim_ev].type = YRSP_RELEASE;
                        sim_ev[n_sim_ev].value = 0;
                        sim_ev[n_sim_ev++].t = e->t + YRT_MS(80);
                    }
                }
            }
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                yrsp_finish(&rsp, &res);
                memset(&r, 0, sizeof r);
                r.response = res.response_name;
                r.rsp_kind = (res.flags & YRSP_R_RESPONDED) ? web_kind_name(res.kind) : NULL;
                r.rt_ns = res.rt_ns == YRSP_NO_TIME ? YTR_MISSING_NS : res.rt_ns;
                r.correct = (res.flags & YRSP_R_RESPONDED) && res.response_name && !strcmp(res.response_name, tilt);
                r.onset_tier = res.onset_tier;
                r.onset_src = res.onset_src;
                r.rsp_tier = res.stamp_tier;
                r.n_anticipations = res.n_anticipations;
                r.frames_dropped = dropped;
                ytr_update(&trials, (res.flags & YRSP_R_RESPONDED) ? r.correct : YTR_INVALID, NULL);
                if (ytr_data_write(&data, &r) < 0) { fprintf(stderr, "web_tutorial: %s\n", ytr_data_error(&data)); failed = 1; }
                printf("trial %2d %-5s rt %.3f s %s\n", ti.index, ytb_text(&tab, ti.condition, tilt_col),
                       r.rt_ns == YTR_MISSING_NS ? NAN : YRT_TO_S(r.rt_ns),
                       !(res.flags & YRSP_R_RESPONDED) ? "(too slow)" : r.correct ? "correct" : "wrong");
                if (ti.index < SIM_TRIALS) sim_res[ti.index] = res;
                rows++;
                in_trial = 0;
            }
        }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &fix);
        ygfx_draw(&gfx, &gabor_l);
        ygfx_draw(&gfx, &gabor_r);
        ygfx_draw(&gfx, &fb_right);
        ygfx_draw(&gfx, &fb_wrong);
        ygfx_draw(&gfx, &fb_none);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    if (ytr_data_finish(&data) < 0) { fprintf(stderr, "web_tutorial: %s\n", ytr_data_error(&data)); failed = 1; }
    ytr_data_close(&data);
    if (csv_file) fclose(csv_file);
    yscr_wait_flip(&scr, NULL);
    yscr_describe(&scr, line, sizeof line);
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d trials%s\n", rows, aborted ? " (stopped with Shift+Esc)" : "");
#if defined(__EMSCRIPTEN__)
    web_done(csv, line);
#endif

    if (sim && !failed) {
        int bad = rows != SIM_TRIALS;
        for (i = 0; i < rows && i < SIM_TRIALS; i++) {
            const yrsp_result* q = &sim_res[i];
            double want = i == 0 ? 0.42 : i == 1 ? 0.51 : i == 2 ? 0.38 : i == 4 ? 0.45 : i == 5 ? 0.60 : NAN;
            int ok = i == 3 ? !(q->flags & YRSP_R_RESPONDED) && (q->flags & YRSP_R_TIMEOUT)
                            : (q->flags & YRSP_R_RESPONDED) && fabs(YRT_TO_S(q->rt_ns) - want) < 1e-6;
            if (i == 1) ok = ok && q->n_anticipations == 1;
            if (i != 3) ok = ok && q->onset_tier == YSCR_TIER_SIM && q->onset_src == YRSP_ONSET_FLIP;
            if (!ok) { fprintf(stderr, "web_tutorial: --sim: trial %d: flags 0x%x rt %.9f\n", i, (unsigned)q->flags, YRT_TO_S(q->rt_ns)); bad = 1; }
        }
        if (bad || strncmp(csv, "index,block", 11)) { fprintf(stderr, "web_tutorial: --sim: %d rows\n", rows); return 1; }
        printf("--sim: every trial came out as scripted\n");
    }
    return failed;
}
