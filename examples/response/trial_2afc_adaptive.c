/* trial_2afc_adaptive.c - a spatial two-alternative forced-choice (2AFC)
 * contrast detection task: a staircase (ysp/stair.h) and QUEST+
 * (ysp/quest.h) interleaved by ysp/trials.h, the contrast set through the
 * display calibration (ysp/color.h), the response from ysp/response.h.
 *
 * It does what PsychoPy's Builder demos psychophysicsStaircase/ and
 * psychophysicsStairsInterleaved/ and the Coder demo
 * experiment control/JND_staircase_exp.py do. PsychoPy is GPL-3: those
 * demos were read, no code was copied.
 *
 * Each trial: a fixation cross; 0.5 s later a vertical gabor (2 c/deg,
 * sigma 0.4 deg) for 0.2 s, 5 deg left or right of the cross. Press the
 * Left or Right arrow for the side. The window opens at the gabor's flip
 * and lasts 2.5 s; presses before the onset are EARLY (counted, not
 * responses). A blank of 0.5 s follows the response. A trial with no
 * response is not told to its track, so the track runs it again.
 *
 * The level is the Michelson luminance contrast of the gabor's peak and
 * trough, in light: Y in cd/m2 from the calibration, the display's black
 * included. ysp/gfx.h's contrast is about the background in linear device
 * RGB, which leaves the black out, so the example scales it by
 * Y_bg / (Y_bg - Y_black). The output stage writes 8-bit codes (no
 * dither), so the contrast shown is that of the two codes the peak and
 * trough get (ycol_output_code(), the output stage's arithmetic on the
 * CPU). The methods get the contrast shown, not the one asked for: on a
 * gamma 2.2 display at a mid-gray background one code step is a
 * contrast of about 0.006, so below about 0.02 the levels are coarse.
 * The gamut check (ycol_max_scale()) sets the highest level.
 *
 * HARDWARE: without --cal the calibration is nominal (sRGB primaries,
 * gamma 2.2, 100 cd/m2, no black), flagged NOMINAL in FILE.meta: no
 * contrast in the file is then a statement about light. A claim about
 * contrast needs a photometer and a .yspcal from it (docs/color.md). Sizes
 * in deg use PPD_DISTANCE_MM and PPD_MM_PER_PX below: measure both.
 *
 * --method: stair (1-up-3-down on log10 contrast, steps 0.2, 0.1, 0.05,
 * the 1-up-1-down rule until the first reversal: 79.4 % correct), quest
 * (QUEST+ with threshold and slope free, guess 0.5, lapse 0.02: the Psi
 * method), or both (the default), interleaved at random by ysp/trials.h.
 *
 * Columns (ysp/trials.h's data writer): the session's (index, ..., track
 * as a number), then trial_index, stimulus (left or right), key_answer,
 * correct, method (stair or quest), intensity (the level the method
 * proposed), contrast_shown, scene_contrast (ysp/gfx.h's), code_peak,
 * code_trough, estimate (the track's threshold after the trial), then the
 * response's (YRSP_DATA_COLS: response, rt, onset_src, onset_tier,
 * landing_residual, onset_frame, rsp_tier, n_early and the rest). Times
 * in seconds. FILE.meta has the session, the screen, the calibration and
 * the methods.
 *
 * --sim: the simulated display and a simulated observer, a Weibull on the
 * contrast shown (threshold 0.03 at 81.6 % correct, slope 3.5, lapse
 * 0.02), RT 0.15 to 0.25 s, with shorter fixation and blank (about 35 s
 * for 2 x 40 trials). It checks each row (RT and key as scripted, onsets
 * from flip records) and that each estimate is near the observer's own
 * value: within 0.25 log10 units of its 79.4 % point (0.0295) for the
 * staircase, within 0.15 of its threshold (0.03) for QUEST+. 40 trials
 * are few: over 400 seeds of the same methods and observer on the CPU
 * (no display), 350 staircases and 382 QUEST+ runs were inside those
 * bounds, so another --seed can fail the check, and the message says so.
 * Exits 1 on a mismatch.
 *
 * Usage: trial_2afc_adaptive [--sim] [--fullscreen] [--method stair|quest|both]
 *          [--trials N] [--seed N] [--cal FILE] [--out FILE]
 *   --trials N  trials per track (default 60; --sim: 40)
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
#define YSP_STAIR_IMPLEMENTATION
#include "ysp/stair.h"
#define YSP_QUEST_IMPLEMENTATION
#include "ysp/quest.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FIX_ON, GABOR_ON, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };
enum { STAIR, QUEST };

#define PPD_DISTANCE_MM 570.0     /* viewing distance: measure it */
#define PPD_MM_PER_PX   0.27      /* pixel pitch: measure it */
#define STIM_S      0.2
#define RESPONSE_S  2.5
#define OBS_ALPHA   0.03          /* the simulated observer */
#define OBS_BETA    3.5
#define OBS_LAPSE   0.02
#define MAX_TRIALS  1024

static const yrsp_choice keys[] = { { .key = "left" }, { .key = "right" } };
static const char* const side_name[] = { "left", "right" };
static const char* const track_name[] = { "stair", "quest" };
static const double sim_tol[] = { 0.25, 0.15 };   /* log10 units; see --sim */

static yscr_screen scr;
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static yrsp_collector rsp;
static yst_stair stair;
static yqst_quest quest;
static ycol_cal cal;
static ycol_ctx cx;
static ygfx_stim fix, gabor;
static const ygfx_bind binds[] = {
    { .stim = &fix,   .param = YGFX_P_VISIBLE, .channel = FIX_ON },
    { .stim = &gabor, .param = YGFX_P_VISIBLE, .channel = GABOR_ON },
};
static double bg_scale;           /* Y_bg / (Y_bg - Y_black) */

static bool stair_done(void* c) { return yst_done((const yst_stair*)c); }
static bool quest_done(void* c) { return yqst_done((const yqst_quest*)c); }

static double luminance(ycol_rgb v) { return ycol_from_rgb(&cx, v, YCOL_SPACE_XYY, NULL).u.xyy.Y; }
static double code_luminance(uint32_t code) {
    double d = code / 255.0;
    return luminance(ycol_to_rgb(&cx, ycol_make(YCOL_SPACE_DEVICE, d, d, d), NULL));
}

/* The level as ysp/gfx.h's contrast, and the contrast its codes show. */
static double shown_contrast(double level, double* scene, uint32_t code[2]) {
    uint32_t q[3];
    double c = level * bg_scale, yp, yt;
    ycol_rgb v;
    *scene = c;
    v.r = v.g = v.b = 0.5 * (1.0 + c);
    ycol_output_code(&cal, v, 8, q);
    code[0] = q[0];
    v.r = v.g = v.b = 0.5 * (1.0 - c);
    ycol_output_code(&cal, v, 8, q);
    code[1] = q[0];
    yp = code_luminance(code[0]);
    yt = code_luminance(code[1]);
    return (yp - yt) / (yp + yt);
}

static double obs_p(double c) {
    return c <= 0 ? 0.5 : 0.5 + (0.5 - OBS_LAPSE) * (1.0 - exp(-pow(c / OBS_ALPHA, OBS_BETA)));
}

static double estimate(int track) {
    double est[4];
    if (track == STAIR) return yst_estimate(&stair, YST_EST_REVERSALS);
    return yqst_estimate(&quest, YQST_EST_MEAN, est) < 0 ? NAN : pow(10.0, est[0]);
}

/* The program's columns, after the session's; then YRSP_DATA_COLS.
 * "method", not "track": the session has a track column (its number). */
typedef struct row {
    int trial_index, code_peak, code_trough;
    bool correct;
    const char *stimulus, *key_answer, *method;
    double intensity, contrast_shown, scene_contrast, estimate;
} row;
static const ytr_col cols[] = {
    YTR_COL(row, trial_index, YTR_COL_INT), YTR_COL(row, stimulus, YTR_COL_STRING),
    YTR_COL(row, key_answer, YTR_COL_STRING), YTR_COL(row, correct, YTR_COL_BOOL),
    YTR_COL(row, method, YTR_COL_STRING), YTR_COL_D(row, intensity, YTR_COL_DOUBLE, 6),
    YTR_COL_D(row, contrast_shown, YTR_COL_DOUBLE, 6), YTR_COL_D(row, scene_contrast, YTR_COL_DOUBLE, 6),
    YTR_COL(row, code_peak, YTR_COL_INT), YTR_COL(row, code_trough, YTR_COL_INT),
    YTR_COL_D(row, estimate, YTR_COL_DOUBLE, 9), YRSP_DATA_COLS,
};
static ytr_data data;

static int load_cal(const char* path) {
    static unsigned char bytes[sizeof(ycol_cal)];
    char err[200];
    size_t n;
    FILE* fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "trial_2afc_adaptive: cannot read %s\n", path); return 0; }
    n = fread(bytes, 1, sizeof bytes, fp);
    fclose(fp);
    if (ycol_cal_load(&cal, bytes, n, err, sizeof err) < 0) { fprintf(stderr, "trial_2afc_adaptive: %s\n", err); return 0; }
    return 1;
}

int main(int argc, char** argv) {
    static const float srgb_xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };
    static double sim_rt[MAX_TRIALS];
    static int sim_key[MAX_TRIALS];
    static yrsp_result sim_res[MAX_TRIALS];
    ygfx_gabor_desc gb;
    ytr_desc rd;
    yrsp_source source;
    ycol_ctx_desc cxd;
    yst_desc sdesc;
    yqst_desc qdesc;
    yscr_caps caps;
    yscr_frame f;
    ytl_event fired[64];                   /* the timeline's capacity: all that can fire */
    ytr_trial_info ti = { 0 };
    yrsp_result res;
    yrsp_input sim_ev[2];
    row r = { 0 };
    char line[1024], cline[1024], meta[3072], meta_path[1100];
    const char* out_path = "trial_2afc_adaptive.csv";
    const char* cal_path = NULL;
    const char* method = "both";
    uint64_t seed = 20261009, rng_side, rng_obs;
    int i, n, sim = 0, n_trials = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0, sim_bad = 0;
    int in_trial = 0, responding = 0, n_sim_ev = 0, sim_next = 0, side = 0, track = 0;
    int64_t fix_ns, iti_ns, sim_frame = -1;   /* --sim: the gabor's landing frame, until its record */
    double ppd, level = 0, shown = 0, scene = 0, level_max, y_bg, y_black;
    uint32_t codes[2] = { 0, 0 };
    ycol_rgb v;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--method") && i + 1 < argc) method = argv[++i];
        else if (!strcmp(argv[i], "--trials") && i + 1 < argc) n_trials = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--cal") && i + 1 < argc) cal_path = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_2afc_adaptive [--sim] [--fullscreen] [--method stair|quest|both] "
                            "[--trials N] [--seed N] [--cal FILE] [--out FILE]\n");
            return 2;
        }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    if (strcmp(method, "stair") && strcmp(method, "quest") && strcmp(method, "both")) {
        fprintf(stderr, "trial_2afc_adaptive: --method takes stair, quest or both\n");
        return 2;
    }
    if (n_trials == 0) n_trials = sim ? 40 : 60;
    if (n_trials < 5 || n_trials > MAX_TRIALS / 2) { fprintf(stderr, "trial_2afc_adaptive: --trials takes 5..512\n"); return 2; }
    fix_ns = YRT_S(sim ? 0.1 : 0.5);
    iti_ns = YRT_S(sim ? 0.05 : 0.5);
    rng_side = seed ^ 0x5157u;
    rng_obs = seed ^ 0x0b5u;

    /* the calibration: measured, or nominal and flagged */
    if (cal_path ? !load_cal(cal_path) : ycol_cal_nominal(&cal, srgb_xy, 100.0f, 2.2) < 0) {
        if (!cal_path) fprintf(stderr, "trial_2afc_adaptive: no nominal calibration\n");
        return 1;
    }
    memset(&cxd, 0, sizeof cxd);
    cxd.cal = &cal;
    cxd.background[0] = cxd.background[1] = cxd.background[2] = 0.5;
    if (!ycol_ctx_open(&cx, &cxd)) { fprintf(stderr, "trial_2afc_adaptive: %s\n", ycol_ctx_error(&cx)); return 1; }
    v.r = v.g = v.b = 0.5;
    y_bg = luminance(v);
    v.r = v.g = v.b = 0.0;
    y_black = luminance(v);
    bg_scale = y_bg / (y_bg - y_black);
    v.r = v.g = v.b = 1.0;   /* a scene contrast of 1: the gamut's limit along the gray axis */
    level_max = ycol_max_scale(&cx, ycol_make(YCOL_SPACE_RGB, v.r, v.g, v.b), YCOL_SYMMETRIC) / bg_scale;
    if (level_max > 1.0) level_max = 1.0;

    /* the methods: each one a track of ysp/trials.h */
    memset(&sdesc, 0, sizeof sdesc);
    sdesc.start = 0.2;
    sdesc.n_up = 1;
    sdesc.n_down = 3;
    sdesc.step_type = YST_STEP_LOG;
    sdesc.steps[0] = 0.2; sdesc.steps[1] = 0.1; sdesc.steps[2] = 0.05;
    sdesc.n_steps = 3;
    sdesc.min = 0.002;
    sdesc.max = level_max;
    sdesc.initial_rule = true;
    sdesc.stop_trials = n_trials;
    if (!yst_open(&stair, &sdesc)) { fprintf(stderr, "trial_2afc_adaptive: %s\n", yst_error(&stair)); return 1; }
    memset(&qdesc, 0, sizeof qdesc);
    qdesc.stim[0] = yqst_linspace(-2.5, log10(level_max), 51);   /* log10 contrast */
    qdesc.n_stim = 1;
    qdesc.param[0] = yqst_linspace(-2.5, log10(level_max), 51);  /* threshold */
    qdesc.param[1] = yqst_linspace(1.0, 7.0, 13);                /* slope */
    qdesc.param[2] = yqst_fixed(0.5);
    qdesc.param[3] = yqst_fixed(OBS_LAPSE);
    qdesc.n_param = 4;
    qdesc.pf = YQST_PF_GUMBEL;    /* a Weibull in log10 units */
    qdesc.stop_trials = n_trials;
    if (!yqst_open(&quest, &qdesc)) { fprintf(stderr, "trial_2afc_adaptive: %s\n", yqst_error(&quest)); return 1; }
    memset(&rd, 0, sizeof rd);
    if (strcmp(method, "quest")) rd.tracks[rd.n_tracks++] = ytr_track(&stair, stair_done);
    if (strcmp(method, "stair")) rd.tracks[rd.n_tracks++] = ytr_track(&quest, quest_done);
    rd.interleave = YTR_INTERLEAVE_RANDOM;
    rd.rng = ytr_splitmix;
    rd.rng_ctx = &seed;
    if (!ytr_open(&trials, &rd)) { fprintf(stderr, "trial_2afc_adaptive: %s\n", ytr_error(&trials)); return 1; }

    /* the display and the graphics, opened together and primed; then the stimuli in deg */
    if (!ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .backend = sim ? YSCR_BACKEND_SIM : 0, .windowed = !fullscreen },
                          &(ygfx_desc){ .background = { 0.5f, 0.5f, 0.5f }, .cal = &cal })) {
        fprintf(stderr, "trial_2afc_adaptive: %s\n", ygfx_error(&gfx));
        return 1;
    }
    yscr_get_caps(&scr, &caps);
    ppd = PPD_DISTANCE_MM * tan(3.14159265358979323846 / 180.0) / PPD_MM_PER_PX;
    {
        ygfx_shape_desc s;
        memset(&s, 0, sizeof s);
        s.shape = YGFX_CROSS;
        s.w = s.h = (float)(0.4 * ppd);
        s.shape_p[0] = (float)(0.06 * ppd);
        s.edge = YGFX_EDGE_COSINE;
        s.edge_width = 1;
        s.color[0] = s.color[1] = s.color[2] = 0.15f;
        fix = ygfx_shape(&s);
    }
    memset(&gb, 0, sizeof gb);
    gb.sf = (float)(2.0 / ppd);
    gb.sigma = (float)(0.4 * ppd);
    gabor = ygfx_gabor(&gb);

    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS })) {
        fprintf(stderr, "trial_2afc_adaptive: %s\n", ytl_error(&tl));
        return 1;
    }

    source = yrsp_sdl_source(&(yrsp_sdl_ctx){ .raw_keyboard = caps.raw_keyboard }, YRSP_KIND_KEYBOARD);
    if (!yrsp_open(&rsp, &(yrsp_desc){ .choices = keys, .n_choices = 2, .duration_ns = YRT_S(RESPONSE_S),
                                       .sources = &source, .n_sources = 1 })) {
        fprintf(stderr, "trial_2afc_adaptive: %s\n", yrsp_error(&rsp));
        return 1;
    }

    /* the data file, and the settings beside it */
    yscr_describe(&scr, line, sizeof line);
    ycol_ctx_describe(&cx, cline, sizeof cline);
    printf("%s\n%s\n", line, cline);
    snprintf(meta, sizeof meta, "screen=%s\ncolor=%s\nmethod=%s; stair 1-up-3-down log10 steps 0.2 0.1 0.05, initial "
             "rule; quest Gumbel, threshold and slope free, guess 0.5, lapse %.2f; %d trials a track; level_max %.4f; "
             "%.2f px/deg\nysp_stair=%s\nysp_quest=%s\nysp_response=%s\nkeyboard=%s\n", line, cline, method, OBS_LAPSE,
             n_trials, level_max, ppd, yst_version(), yqst_version(), yrsp_version(),
             sim ? "synthetic (tier SIM)" : source.note);
    snprintf(meta_path, sizeof meta_path, "%s.meta", out_path);
    if (!ytr_data_open(&data, &(ytr_data_desc){ .path = out_path, .trials = &trials, .cols = cols,
            .n_cols = (int)(sizeof cols / sizeof cols[0]), .meta_path = meta_path, .meta = meta })) {
        fprintf(stderr, "trial_2afc_adaptive: %s\n", ytr_data_error(&data));
        return 1;
    }

    for (;;) {
        int64_t bt = 0;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_2afc_adaptive: %s\n", yscr_error(&scr)); failed = 1; break; }

        if (!in_trial) {
            /* a new trial: the track's level, a side, the sequence */
            ytl_seq q;
            if (ytr_next(&trials, &ti) < 0) break;
            track = rd.tracks[ti.track].ctx == (void*)&stair ? STAIR : QUEST;
            level = track == STAIR ? yst_next(&stair) : pow(10.0, yqst_stim_value(&quest, yqst_next(&quest), 0));
            shown = shown_contrast(level, &scene, codes);
            side = ytr_splitmix(&rng_side) < 0.5 ? 0 : 1;
            gabor.x = (float)((side ? 5.0 : -5.0) * ppd);
            gabor.contrast = (float)scene;
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            ytl_on(&q, FIX_ON);
            ytl_wait(&q, fix_ns);
            ytl_on(&q, GABOR_ON);
            ytl_wait(&q, YRT_S(STIM_S));
            ytl_off(&q, GABOR_ON);
            if (q.err) { fprintf(stderr, "trial_2afc_adaptive: timeline error %d\n", q.err); failed = 1; break; }
            yrsp_arm(&rsp, f.onset);
            in_trial = responding = 1;
        }

        /* --sim: the observer's key from the gabor's flip record, which RT
         * is measured from; the landing frame's predicted onset is a plan
         * that a frame loop behind on a loaded host can miss */
        for (i = 0; sim_frame >= 0 && i < f.n_done; i++)
            if (f.done[i].index == sim_frame && yrsp_onset_flip(&f.done[i]).final) {
                sim_ev[0].t = f.done[i].onset + YRT_S(sim_rt[ti.index]);
                sim_ev[1].t = sim_ev[0].t + YRT_MS(80);
                n_sim_ev = 2;
                sim_frame = -1;
            }

        yrsp_feed_screen(&scr, &rsp, 1);
        for (; sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset; sim_next++)
            yrsp_feed(&rsp, &sim_ev[sim_next]);

        /* the gabor's flip record when it arrives; the response, or the
         * deadline: a blank after the gabor's end */
        if (yrsp_update_frame(&rsp, &f) == YRSP_ENDED && responding) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            int64_t stim_end = fix_ns + YRT_S(STIM_S);
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt > stim_end ? bt : stim_end);
            ytl_off(&q, FIX_ON);
            ytl_wait(&q, iti_ns);
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "trial_2afc_adaptive: timeline error %d\n", q.err); failed = 1; break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        for (i = 0; i < n; i++) {
            if (fired[i].kind == YTL_ONSET && fired[i].target == GABOR_ON) {
                yrsp_set_landing(&rsp, &fired[i], 1, GABOR_ON);   /* the gabor landed: its planned onset */
                if (sim && ti.index < MAX_TRIALS) {
                    /* the observer: correct with the Weibull's p at the contrast shown */
                    int correct = ytr_splitmix(&rng_obs) < obs_p(shown);
                    uint32_t sc;
                    for (; sim_next < n_sim_ev; sim_next++) yrsp_feed(&rsp, &sim_ev[sim_next]);   /* a late release */
                    sim_rt[ti.index] = 0.15 + 0.1 * ytr_splitmix(&rng_obs);
                    sim_key[ti.index] = correct ? side : 1 - side;
                    sc = (uint32_t)yrsp_scancode(side_name[sim_key[ti.index]]);
                    memset(sim_ev, 0, sizeof sim_ev);   /* times from the flip record, above */
                    sim_ev[0].kind = YRSP_KIND_KEYBOARD;
                    sim_ev[0].type = YRSP_PRESS;
                    sim_ev[0].control = sc;
                    sim_ev[0].stamp = YRSP_TIER_SIM;
                    sim_ev[0].value = 1;
                    sim_ev[1] = sim_ev[0];
                    sim_ev[1].type = YRSP_RELEASE;
                    sim_ev[1].value = 0;
                    n_sim_ev = sim_next = 0;
                    sim_frame = fired[i].frame;
                }
            }
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                int responded;
                yrsp_finish(&rsp, &res);
                responded = (res.flags & YRSP_R_RESPONDED) && res.response >= 0;
                r.correct = responded && res.response == side;
                if (responded) {
                    /* the method hears the contrast shown */
                    double x = shown > 1e-4 ? shown : 1e-4;
                    if (track == STAIR) yst_update(&stair, x, r.correct);
                    else { x = log10(x); yqst_update_values(&quest, &x, r.correct); }
                }
                ytr_update(&trials, responded ? r.correct : YTR_INVALID, NULL);
                r.trial_index = ti.index;
                r.stimulus = r.key_answer = side_name[side];
                r.method = track_name[track];
                r.intensity = level;
                r.contrast_shown = shown;
                r.scene_contrast = scene;
                r.code_peak = (int)codes[0];
                r.code_trough = (int)codes[1];
                r.estimate = estimate(track);
                if (yrsp_data_set(&data, &res) < 0 || ytr_data_write(&data, &r) < 0) {
                    fprintf(stderr, "trial_2afc_adaptive: %s\n", ytr_data_error(&data));
                    failed = 1;
                }
                printf("trial %3d %-5s level %.4f shown %.4f %-5s %s\n", ti.index, track_name[track], level, shown,
                       side_name[side], !responded ? "(no response)" : r.correct ? "correct" : "wrong");
                if (ti.index < MAX_TRIALS) sim_res[ti.index] = res;
                rows++;
                in_trial = 0;
            }
        }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &gabor);
        ygfx_draw(&gfx, &fix);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    if (ytr_data_finish(&data) < 0) { fprintf(stderr, "trial_2afc_adaptive: %s\n", ytr_data_error(&data)); failed = 1; }
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");
    if (strcmp(method, "quest")) printf("staircase: %.4f (%d reversals)\n", estimate(STAIR), yst_n_reversals(&stair));
    if (strcmp(method, "stair")) printf("QUEST+: %.4f (sd of log10 threshold %.3f)\n", estimate(QUEST), yqst_sd(&quest, 0));

    if (sim && !failed && !aborted) {
        /* every row as scripted, then the estimates against the observer */
        double p794 = pow(0.5, 1.0 / 3.0), want[2], got[2];   /* 1-up-3-down's 0.794 */
        want[STAIR] = OBS_ALPHA * pow(-log(1.0 - (p794 - 0.5) / (0.5 - OBS_LAPSE)), 1.0 / OBS_BETA);
        want[QUEST] = OBS_ALPHA;
        for (i = 0; i < rows && i < MAX_TRIALS; i++) {
            const yrsp_result* sr = &sim_res[i];
            if (!(sr->flags & YRSP_R_RESPONDED) || sr->response != sim_key[i] || fabs(YRT_TO_S(sr->rt_ns) - sim_rt[i]) > 1e-6 ||
                sr->onset_tier != YSCR_TIER_SIM || sr->onset_src != YRSP_ONSET_FLIP) {
                fprintf(stderr, "trial_2afc_adaptive: --sim: trial %d: rt %.9f (want %.9f) response %d (want %d) "
                                "onset tier %d src %d\n", i, YRT_TO_S(sr->rt_ns), sim_rt[i], sr->response, sim_key[i],
                        sr->onset_tier, sr->onset_src);
                sim_bad = 1;
            }
        }
        for (i = STAIR; i <= QUEST; i++) {
            if (!strcmp(method, i == STAIR ? "quest" : "stair")) continue;
            got[i] = estimate(i);
            if (!(fabs(log10(got[i] / want[i])) <= sim_tol[i])) {
                fprintf(stderr, "trial_2afc_adaptive: --sim: the %s estimate %.4f is %.3f log10 units from the "
                                "observer's %.4f (tolerance %.2f; another --seed can miss it)\n",
                        track_name[i], got[i], log10(got[i] / want[i]), want[i], sim_tol[i]);
                sim_bad = 1;
            } else printf("--sim: %s %.4f against the observer's %.4f (%+.3f log10)\n", track_name[i], got[i],
                          want[i], log10(got[i] / want[i]));
        }
        if (sim_bad) failed = 1;
        else printf("--sim: every trial came out as scripted\n");
    }
    yqst_close(&quest);
    return failed;
}
