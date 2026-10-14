/* gamma_null.c - a psychophysical check of the display's linearization by
 * motion nulling (Ledgeway and Smith 1994; Anstis and Cavanagh 1983 for
 * the idea), through the output stage's lookup table of ysp/gfx.h, a
 * staircase (ysp/stair.h) on the null, and no photometer.
 *
 * It does what PsychoPy's Coder demo hardware/gammaMotionNull.py does.
 * PsychoPy is GPL-3: the demo's description was read (docs/rig_spec.md
 * 15), no code was copied.
 *
 * The stimulus: a 512 x 256 px field of 8 vertical square-wave cycles (64
 * px). Images alternate between a luminance grating (contrast 0.15) and a
 * texture grating: bars of binary pixel noise (each pixel scene 0 or 1,
 * so device codes 0 and 255 whatever the table) between bars of uniform
 * scene 0.5 (whose code the table sets). Each image steps a quarter cycle
 * (16 px) left or right of the last, 2 refresh frames each at 60 Hz, for
 * 0.6 s. The noise's mean light is (L(0) + L(255)) / 2 on any display;
 * the uniform bars match it only when the table puts linear 0.5 at half
 * light. When they do not match, the texture grating carries a luminance
 * grating a quarter cycle from each luminance image, and the bars seem to
 * move: with the step when the uniform bars are too dark, against it when
 * they are too light. At the null the motion is ambiguous.
 *
 * The table: device = base(x^(1/e)), base the identity (no --cal: e is
 * the display's gamma between its black and white) or the calibration's
 * own table (--cal FILE: e is the calibration's error, 1 when it holds at
 * mid light). Two 1-up-1-down staircases on log10 e (steps 0.04, 0.02,
 * 0.01), one from above and one from below, interleaved at random by
 * ysp/trials.h: the answer "with the step" raises e. Each one's estimate
 * is the mean of its last 6 reversals; the result is their geometric
 * mean. Press Left or Right for the direction the bars seemed to move.
 *
 * Why not a photometer: this needs none and checks the light the eye
 * gets at the viewing position. Its limits: it tests one level (mid
 * light) and gray only; it assumes that a pixel's light does not depend
 * on its neighbors (true for most LCDs, not for CRTs along a scan line);
 * the 8-bit output stage quantizes the uniform bars' code (one code near
 * mid gray is about 0.03 in e). docs/color.md has the calibration with a
 * photometer.
 *
 * The data file has one row per trial (ysp/trials.h's data writer): the
 * session's columns (index, ..., track), then trial_index, method
 * (from_above or from_below), gamma (e), code_mid (the code of scene 0.5),
 * step (left or right), seen (left or right), with_step, estimate (the
 * track's after the trial), frames_dropped (motion frames that missed
 * their vblank), then the response's (YRSP_DATA_COLS). Times in seconds.
 * A window run also reads the output back once (ygfx_read_output) and
 * checks that the texture bars are codes 0 and 255 pixel for pixel and
 * the uniform bars code_mid.
 *
 * --sim: the simulated display and a simulated observer on a display of
 * gamma 2.2 (--sim-gamma) with a black of 0.2 % of white, who sees the
 * motion of the luminance artifact (the noise's mean minus the uniform
 * bars' light, over the mean) through a cumulative normal of SD 1 % with
 * 2 % lapses; shorter fixation and blank (about 30 s for 2 x 20 trials).
 * It checks each row (key and RT as scripted, onsets from flip records)
 * and that the result is within 0.02 log10 units of the observer's null
 * (seeds 1 to 24 on the simulated display, 2026-10-13: 2.179 to 2.238
 * against 2.2, at most 0.008 log10 units off).
 * Exits 1 on a mismatch.
 *
 * Usage: gfx_gamma_null [--sim] [--fullscreen] [--trials N] [--seed N]
 *          [--cal FILE] [--sim-gamma G] [--out FILE]
 *   --trials N  trials per staircase (default 30; --sim: 20)
 * Exit code: 0 (also after Shift+Esc), 1 when something did not open or a
 * check failed, 2 for a bad argument.
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
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FIX_ON, MOTION_ON, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };

#define PERIOD_PX   64        /* a quarter cycle is a whole 16 px */
#define CYCLES      8
#define FIELD_H     256
#define C_LUM       0.15f
#define IMAGE_S     (1.0 / 30)
#define MOTION_S    0.6
#define RESPONSE_S  3.0
#define LUT_N       257       /* entry 128 is x = 0.5 exactly */
#define MAX_TRIALS  512
#define MAX_BARS    (CYCLES + 4)
#define OBS_SD      0.01      /* the simulated observer */
#define OBS_LAPSE   0.02
#define OBS_BLACK   0.002

static const yrsp_choice keys[] = { { .key = "left" }, { .key = "right" } };
static const char* const dir_name[] = { "left", "right" };
static const char* const track_name[] = { "from_above", "from_below" };

static yscr_screen scr;
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static ytr_data data;
static yrsp_collector rsp;
static yst_stair stair[2];
static ycol_cal cal;
static float lut[3 * LUT_N];
static ygfx_stim fix, lum, bars[MAX_BARS];
static int n_bars, bar_a[MAX_BARS], bar_b[MAX_BARS];   /* the bars from the field's left, px */
static int have_cal;

static bool stair_done(void* c) { return yst_done((const yst_stair*)c); }

/* The base table at x: the identity, or the calibration's, interpolated. */
static double base(int gun, double x) {
    int n = cal.lut_n ? cal.lut_n : YCOL_CAL_MAX_LUT, i;
    double p;
    if (!have_cal) return x;
    p = x * (n - 1);
    i = (int)p;
    if (i >= n - 1) return cal.lut[gun][n - 1];
    return cal.lut[gun][i] + (p - i) * (cal.lut[gun][i + 1] - cal.lut[gun][i]);
}
/* The table for e, and the code the output stage gives scene 0.5 (green). */
static int make_lut(double e) {
    int gun, i;
    for (gun = 0; gun < 3; gun++)
        for (i = 0; i < LUT_N; i++) lut[gun * LUT_N + i] = (float)base(gun, pow(i / (LUT_N - 1.0), 1.0 / e));
    return (int)floor(lut[LUT_N + (LUT_N - 1) / 2] * 255.0 + 0.5);
}

/* The texture bars of an image at phase ph (cycles): where the square
 * wave cos(2 pi (x / P + ph)) is positive, cut to the field. */
static void place_bars(float left, float top, double ph, uint32_t seed) {
    int k;
    n_bars = 0;
    for (k = -CYCLES / 2 - 1; k <= CYCLES / 2 + 1 && n_bars < MAX_BARS; k++) {
        double c = (k - ph) * PERIOD_PX + CYCLES * PERIOD_PX / 2.0;   /* the bar's center from the left */
        double a = c - PERIOD_PX / 4.0, b = c + PERIOD_PX / 4.0;
        if (a < 0) a = 0;
        if (b > CYCLES * PERIOD_PX) b = CYCLES * PERIOD_PX;
        if (b - a < 1) continue;
        bar_a[n_bars] = (int)a;
        bar_b[n_bars] = (int)b;
        bars[n_bars] = ygfx_noise(&(ygfx_noise_desc){ .place = YGFX_TOP_LEFT, .anchor = YGFX_TOP_LEFT,
            .x = left + (float)a, .y = top, .w = (float)(b - a), .h = FIELD_H, .check = 1,
            .seed = seed + (uint32_t)n_bars, .dist = YGFX_BINARY, .contrast = 1 });
        n_bars++;
    }
}

/* The simulated display's light at a code, and the observer's answer. */
static double sim_light(int code, double gamma) { return OBS_BLACK + (1 - OBS_BLACK) * pow(code / 255.0, gamma); }
static double sim_p_with(int code_mid, double gamma) {
    double noise = (sim_light(0, gamma) + sim_light(255, gamma)) / 2, a = (noise - sim_light(code_mid, gamma)) / noise;
    return OBS_LAPSE / 2 + (1 - OBS_LAPSE) * 0.5 * erfc(-a / (OBS_SD * sqrt(2.0)));
}

typedef struct row {
    int trial_index, code_mid, frames_dropped;
    bool with_step;
    const char *method, *step, *seen;
    double gamma, estimate;
} row;
static const ytr_col cols[] = {
    YTR_COL(row, trial_index, YTR_COL_INT), YTR_COL(row, method, YTR_COL_STRING),
    YTR_COL_D(row, gamma, YTR_COL_DOUBLE, 5), YTR_COL(row, code_mid, YTR_COL_INT), YTR_COL(row, step, YTR_COL_STRING),
    YTR_COL(row, seen, YTR_COL_STRING), YTR_COL(row, with_step, YTR_COL_BOOL),
    YTR_COL_D(row, estimate, YTR_COL_DOUBLE, 5), YTR_COL(row, frames_dropped, YTR_COL_INT), YRSP_DATA_COLS,
};

static int load_cal(const char* path) {
    static unsigned char bytes[sizeof(ycol_cal)];
    char err[200];
    size_t n;
    FILE* fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "gfx_gamma_null: cannot read %s\n", path); return 0; }
    n = fread(bytes, 1, sizeof bytes, fp);
    fclose(fp);
    if (ycol_cal_load(&cal, bytes, n, err, sizeof err) < 0) { fprintf(stderr, "gfx_gamma_null: %s\n", err); return 0; }
    return 1;
}

static double estimate(int k) { return yst_estimate(&stair[k], YST_EST_REVERSALS); }

int main(int argc, char** argv) {
    static double sim_rt[MAX_TRIALS];
    static int sim_key[MAX_TRIALS];
    static yrsp_result sim_res[MAX_TRIALS];
    ytr_desc rd = { 0 };
    yst_desc sdesc = { 0 };
    yscr_caps caps;
    yscr_frame f;
    yrsp_source source;
    ytl_event fired[64];
    ytr_trial_info ti = { 0 };
    yrsp_result res;
    yrsp_input sim_ev[2];
    row r = { 0 };
    char line[1024], meta[3072], meta_path[1100];
    const char *out_path = "gfx_gamma_null.csv", *cal_path = NULL;
    uint64_t seed = 20261013, rng_dir, rng_obs;
    double sim_gamma = 2.2, guess, e = 0, result;
    float cx = 0, cy = 0, left, top;
    int i, n, sim = 0, n_trials = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0, sim_bad = 0;
    int in_trial = 0, responding = 0, n_sim_ev = 0, sim_next = 0, track = 0, dir = 0, code_mid = 0, hold = 2;
    int moving = 0, image = -1, checked_output = 0, dropped = 0;
    int64_t fix_ns, iti_ns, motion_t0 = 0, first_frame = -1, last_frame = -1, sim_frame = -1;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--trials") && i + 1 < argc) n_trials = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--cal") && i + 1 < argc) cal_path = argv[++i];
        else if (!strcmp(argv[i], "--sim-gamma") && i + 1 < argc) sim_gamma = atof(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else {
            fprintf(stderr, "usage: gfx_gamma_null [--sim] [--fullscreen] [--trials N] [--seed N] [--cal FILE] "
                            "[--sim-gamma G] [--out FILE]\n");
            return 2;
        }
    }
    if (n_trials == 0) n_trials = sim ? 20 : 30;
    if (n_trials < 5 || n_trials > MAX_TRIALS / 2 || !(sim_gamma >= 1.0 && sim_gamma <= 4.0)) {
        fprintf(stderr, "gfx_gamma_null: --trials takes 5..256, --sim-gamma 1..4\n");
        return 2;
    }
    if (cal_path && !load_cal(cal_path)) return 1;
    have_cal = cal_path != NULL;
    fix_ns = YRT_S(sim ? 0.1 : 0.5);
    iti_ns = YRT_S(sim ? 0.05 : 0.4);
    rng_dir = seed ^ 0xd17u;
    rng_obs = seed ^ 0x0b5u;

    /* the staircases: one from each side of the expected null */
    guess = have_cal ? 1.0 : 2.2;
    sdesc.n_up = sdesc.n_down = 1;
    sdesc.step_type = YST_STEP_LOG;
    sdesc.steps[0] = 0.04; sdesc.steps[1] = 0.02; sdesc.steps[2] = 0.01;
    sdesc.n_steps = 3;
    sdesc.min = guess / 3;
    sdesc.max = guess * 3;
    sdesc.stop_trials = n_trials;
    sdesc.est_reversals = 6;
    for (i = 0; i < 2; i++) {
        sdesc.start = guess * pow(10.0, i == 0 ? 0.15 : -0.15);
        if (!yst_open(&stair[i], &sdesc)) { fprintf(stderr, "gfx_gamma_null: %s\n", yst_error(&stair[i])); return 1; }
        rd.tracks[rd.n_tracks++] = ytr_track(&stair[i], stair_done);
    }
    rd.interleave = YTR_INTERLEAVE_RANDOM;
    rd.rng = ytr_splitmix;
    rd.rng_ctx = &seed;
    if (!ytr_open(&trials, &rd)) { fprintf(stderr, "gfx_gamma_null: %s\n", ytr_error(&trials)); return 1; }

    /* no calibration in the desc: the example's table replaces the output stage's */
    if (!ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .backend = sim ? YSCR_BACKEND_SIM : 0, .windowed = !fullscreen },
                          &(ygfx_desc){ .background = { 0.5f, 0.5f, 0.5f } })) {
        fprintf(stderr, "gfx_gamma_null: %s\n", ygfx_error(&gfx));
        return 1;
    }
    yscr_get_caps(&scr, &caps);
    ygfx_center(&gfx, &cx, &cy);
    left = floorf(cx) - CYCLES * PERIOD_PX / 2;   /* whole pixels: a noise check is one pixel */
    top = floorf(cy) - FIELD_H / 2;
    if (caps.mode.period_ns > 0) hold = (int)floor(IMAGE_S * 1e9 / (double)caps.mode.period_ns + 0.5);
    if (hold < 1) hold = 1;
    fix = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_CROSS, .w = 14, .h = 14, .shape_p = { 3 }, .y = -FIELD_H / 2 - 24,
                                         .edge = YGFX_EDGE_COSINE, .edge_width = 1, .color = { 0.1f, 0.1f, 0.1f } });
    lum = ygfx_grating(&(ygfx_grating_desc){ .place = YGFX_TOP_LEFT, .anchor = YGFX_TOP_LEFT, .x = left, .y = top,
                                             .w = CYCLES * PERIOD_PX, .h = FIELD_H, .sf = 1.0f / PERIOD_PX,
                                             .contrast = C_LUM, .square = 1 });

    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS })) {
        fprintf(stderr, "gfx_gamma_null: %s\n", ytl_error(&tl));
        return 1;
    }
    source = yrsp_sdl_source(&(yrsp_sdl_ctx){ .raw_keyboard = caps.raw_keyboard }, YRSP_KIND_KEYBOARD);
    if (!yrsp_open(&rsp, &(yrsp_desc){ .choices = keys, .n_choices = 2, .duration_ns = YRT_S(RESPONSE_S),
                                       .sources = &source, .n_sources = 1 })) {
        fprintf(stderr, "gfx_gamma_null: %s\n", yrsp_error(&rsp));
        return 1;
    }

    yscr_describe(&scr, line, sizeof line);
    printf("%s\ntable: %s; images of %d frames\n", line, have_cal ? cal_path : "identity (no --cal)", hold);
    snprintf(meta, sizeof meta, "screen=%s\ntable=device = base(x^(1/e)); base %s; %d entries\nstimulus=%d cycles of %d "
             "px, %d px high; luminance contrast %.3f; binary noise bars, 1 px checks; quarter-cycle steps, %d frames "
             "an image, %.3f s\nmethod=two 1-up-1-down staircases on log10 e, steps 0.04 0.02 0.01, from %.4f and "
             "%.4f; %d trials each; estimate the mean of the last 6 reversals\nysp_stair=%s\nysp_response=%s\n"
             "keyboard=%s\n", line, have_cal ? cal_path : "identity", LUT_N, CYCLES, PERIOD_PX, FIELD_H, C_LUM, hold,
             MOTION_S, guess * pow(10.0, 0.15), guess * pow(10.0, -0.15), n_trials, yst_version(), yrsp_version(),
             sim ? "synthetic (tier SIM)" : source.note);
    snprintf(meta_path, sizeof meta_path, "%s.meta", out_path);
    if (!ytr_data_open(&data, &(ytr_data_desc){ .path = out_path, .trials = &trials, .cols = cols,
            .n_cols = (int)(sizeof cols / sizeof cols[0]), .meta_path = meta_path, .meta = meta })) {
        fprintf(stderr, "gfx_gamma_null: %s\n", ytr_data_error(&data));
        return 1;
    }

    for (;;) {
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "gfx_gamma_null: %s\n", yscr_error(&scr)); failed = 1; break; }
        for (i = 0; i < f.n_done; i++)   /* the motion frames' records */
            if (first_frame >= 0 && f.done[i].index >= first_frame && (last_frame < 0 || f.done[i].index <= last_frame) &&
                f.done[i].dropped)
                dropped++;

        if (!in_trial) {
            /* a new trial: the track's e into the table, a direction, the sequence */
            ytl_seq q;
            if (ytr_next(&trials, &ti) < 0 || ti.index >= MAX_TRIALS) break;
            track = rd.tracks[ti.track].ctx == (void*)&stair[0] ? 0 : 1;
            e = yst_next(&stair[track]);
            code_mid = make_lut(e);
            if (ygfx_set_lut(&gfx, lut, LUT_N) < 0) { fprintf(stderr, "gfx_gamma_null: %s\n", ygfx_error(&gfx)); failed = 1; break; }
            dir = ytr_splitmix(&rng_dir) < 0.5 ? 0 : 1;
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            ytl_on(&q, FIX_ON);
            ytl_wait(&q, fix_ns);
            ytl_on(&q, MOTION_ON);
            ytl_wait(&q, YRT_S(MOTION_S));
            ytl_off(&q, MOTION_ON);
            if (q.err) { fprintf(stderr, "gfx_gamma_null: timeline error %d\n", q.err); failed = 1; break; }
            yrsp_arm(&rsp, f.onset);
            in_trial = responding = 1;
            moving = dropped = 0;
            first_frame = last_frame = -1;
        }

        /* --sim: the observer's key from the motion onset's flip record */
        for (i = 0; sim_frame >= 0 && i < f.n_done; i++)
            if (f.done[i].index == sim_frame && yrsp_onset_flip(&f.done[i]).final) {
                sim_ev[0].t = f.done[i].onset + YRT_S(sim_rt[ti.index]);
                sim_ev[1].t = sim_ev[0].t + YRT_MS(80);
                n_sim_ev = 2;
                sim_frame = -1;
            }
        yrsp_feed_screen(&scr, &rsp, 1);
        for (; sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset; sim_next++) yrsp_feed(&rsp, &sim_ev[sim_next]);

        if (yrsp_update_frame(&rsp, &f) == YRSP_ENDED && responding) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            int64_t bt = 0, motion_end = fix_ns + YRT_S(MOTION_S);
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt > motion_end ? bt : motion_end);
            ytl_off(&q, FIX_ON);
            ytl_wait(&q, iti_ns);
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "gfx_gamma_null: timeline error %d\n", q.err); failed = 1; break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        for (i = 0; i < n; i++) {
            if (fired[i].kind == YTL_ONSET && fired[i].target == MOTION_ON) {
                yrsp_set_landing(&rsp, &fired[i], 1, MOTION_ON);
                if (sim && ti.index < MAX_TRIALS) {
                    int with = ytr_splitmix(&rng_obs) < sim_p_with(code_mid, sim_gamma);
                    for (; sim_next < n_sim_ev; sim_next++) yrsp_feed(&rsp, &sim_ev[sim_next]);
                    sim_rt[ti.index] = 0.15 + 0.1 * ytr_splitmix(&rng_obs);
                    sim_key[ti.index] = with ? dir : 1 - dir;
                    memset(sim_ev, 0, sizeof sim_ev);
                    sim_ev[0].kind = YRSP_KIND_KEYBOARD;
                    sim_ev[0].type = YRSP_PRESS;
                    sim_ev[0].control = (uint32_t)yrsp_scancode(dir_name[sim_key[ti.index]]);
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
                r.with_step = responded && res.response == dir;
                if (responded) yst_update(&stair[track], e, !r.with_step);   /* "with the step": e goes up */
                ytr_update(&trials, responded ? !r.with_step : YTR_INVALID, NULL);
                r.trial_index = ti.index;
                r.method = track_name[track];
                r.gamma = e;
                r.code_mid = code_mid;
                r.step = dir_name[dir];
                r.seen = responded ? dir_name[res.response] : NULL;
                r.estimate = estimate(track);
                r.frames_dropped = dropped;
                if (yrsp_data_set(&data, &res) < 0 || ytr_data_write(&data, &r) < 0) {
                    fprintf(stderr, "gfx_gamma_null: %s\n", ytr_data_error(&data));
                    failed = 1;
                }
                printf("trial %3d %-10s e %.4f code %3d step %-5s seen %s\n", ti.index, track_name[track], e, code_mid,
                       dir_name[dir], !responded ? "(no response)" : r.with_step ? "with it" : "against it");
                if (ti.index < MAX_TRIALS) sim_res[ti.index] = res;
                rows++;
                in_trial = 0;
            }
        }
        ygfx_apply(&(ygfx_bind){ .stim = &fix, .param = YGFX_P_VISIBLE, .channel = FIX_ON }, 1, ytl_values(&tl));

        /* the image of this frame: luminance on even images, texture on odd */
        if (ytl_values(&tl)[MOTION_ON] > 0.5f) {
            int k;
            double ph;
            if (!moving) { motion_t0 = f.onset; first_frame = f.index; moving = 1; }
            k = f.period > 0 ? (int)(((f.onset - motion_t0) + f.period / 2) / f.period) / hold : 0;
            ph = (dir ? -0.25 : 0.25) * k;   /* +x is right: the phase falls as the bars move right */
            if (k != image) {
                image = k;
                lum.p[YGFX_P_PHASE] = (float)(ph - floor(ph));
                if (k % 2) place_bars(left, top, ph - floor(ph), (uint32_t)(ti.index * 1000 + k) * 16u);
            }
        } else {
            if (moving && last_frame < 0) last_frame = f.index - 1;
            moving = 0;
            image = -1;
        }
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        if (moving && image % 2 == 0) ygfx_draw(&gfx, &lum);
        if (moving && image % 2 == 1)
            for (i = 0; i < n_bars; i++) ygfx_draw(&gfx, &bars[i]);
        ygfx_draw(&gfx, &fix);
        ygfx_end(&gfx);
        if (!sim && !checked_output && moving && image % 2 == 1) {
            /* once: the output stage's codes along a row through the field */
            static uint8_t px[CYCLES * PERIOD_PX * 4];
            int bad = 0, x0 = (int)left, y0 = (int)top + FIELD_H / 2, b;
            checked_output = 1;
            if (ygfx_read_output(&gfx, x0, y0, CYCLES * PERIOD_PX, 1, px) < 0) bad = -1;
            for (i = 0; bad >= 0 && i < CYCLES * PERIOD_PX; i++) {
                int in_bar = 0, g = px[4 * i + 1];
                for (b = 0; b < n_bars; b++)
                    in_bar |= i >= bar_a[b] && i < bar_b[b];
                if (in_bar ? (g != 0 && g != 255) : g != code_mid) bad++;
            }
            if (bad) {
                fprintf(stderr, "gfx_gamma_null: the output read back: %d of %d pixels off (want 0 or 255 in the bars, "
                                "%d between); from x %d:", bad, CYCLES * PERIOD_PX, code_mid, x0);
                for (i = 0; bad > 0 && i < 2 * PERIOD_PX; i++) fprintf(stderr, " %d", px[4 * i + 1]);
                fputc('\n', stderr);
                failed = 1;
            }
            else printf("output read back: %d pixels, the texture bars 0 or 255, the uniform bars %d\n", CYCLES * PERIOD_PX, code_mid);
        }
        yscr_flip(&scr);
    }
    if (ytr_data_finish(&data) < 0) { fprintf(stderr, "gfx_gamma_null: %s\n", ytr_data_error(&data)); failed = 1; }
    ygfx_close(&gfx);
    yscr_close(&scr);
    result = sqrt(estimate(0) * estimate(1));
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");
    printf("null: e %.4f from above (%d reversals), %.4f from below (%d); together %.4f%s\n", estimate(0),
           yst_n_reversals(&stair[0]), estimate(1), yst_n_reversals(&stair[1]), result,
           have_cal ? " (1: the calibration holds at mid light)" : " (the display's gamma)");

    if (sim && !failed && !aborted) {
        for (i = 0; i < rows && i < MAX_TRIALS; i++) {
            const yrsp_result* q = &sim_res[i];
            if (!(q->flags & YRSP_R_RESPONDED) || q->response != sim_key[i] || fabs(YRT_TO_S(q->rt_ns) - sim_rt[i]) > 1e-6 ||
                q->onset_tier != YSCR_TIER_SIM || q->onset_src != YRSP_ONSET_FLIP) {
                fprintf(stderr, "gfx_gamma_null: --sim: trial %d: rt %.9f (want %.9f) response %d (want %d) onset tier %d "
                                "src %d\n", i, YRT_TO_S(q->rt_ns), sim_rt[i], q->response, sim_key[i], q->onset_tier,
                        q->onset_src);
                sim_bad = 1;
            }
        }
        /* the observer's null: the table's e that gives the noise's light */
        {
            double want = have_cal ? NAN : sim_gamma;
            if (have_cal) printf("--sim: with --cal, the null is not checked\n");
            else if (!(fabs(log10(result / want)) <= 0.02)) {
                fprintf(stderr, "gfx_gamma_null: --sim: the null %.4f is %.3f log10 units from the observer's %.4f "
                                "(tolerance 0.02; another --seed can miss it)\n", result, log10(result / want), want);
                sim_bad = 1;
            } else printf("--sim: null %.4f against the observer's %.4f (%+.3f log10)\n", result, want, log10(result / want));
        }
        if (rows != 2 * n_trials) { fprintf(stderr, "gfx_gamma_null: --sim: %d rows, not %d\n", rows, 2 * n_trials); sim_bad = 1; }
        if (!sim_bad) printf("--sim: every trial came out as scripted\n");
        failed = sim_bad;
    }
    return failed;
}
