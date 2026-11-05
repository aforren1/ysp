/* trial_audio_keyboard.c - the jsPsych audio-keyboard-response trial as a
 * small experiment: ysp/trials.h, ysp/audio.h, ysp/gfx.h and
 * ysp/response.h.
 *
 * A fixation cross stays on. Each trial plays a 0.3 s tone, low (500 Hz)
 * or high (1000 Hz), after a foreperiod drawn from 0.6 to 1.0 s. F for
 * low, J for high (physical keys). Responses are allowed while the tone
 * plays (jsPsych's response_allowed_while_playing) and end the trial
 * (response_ends_trial); the window is 2 s from the tone's onset, and
 * responses before 0.1 s are anticipations. A blank interval of 1 s
 * follows.
 *
 * RT runs from the tone's onset record (yrsp_onset_audio()): until the
 * record completes it is the plan (the target time), then the device-clock
 * fit's time for the tone's first frame. That time is a model value, never
 * an observed one. Its tier is in the data: 2 at best on WASAPI (fit time,
 * confirmed played, on a loopback-checked path), 3 when unconfirmed, 4
 * (SIM) on the null device. The display plays no part in the onset.
 *
 * The data file has one row per trial (ysp/trials.h's data writer): the
 * session's columns (index, ..., stimulus, hz, key_answer, foreperiod_s),
 * then trial_index, correct, audio_flags (YAU_ONSET_*), then the
 * response's (YRSP_DATA_COLS: response, rt_s, onset_src, onset_tier,
 * onset_residual_s: the tone's onset minus its target, rsp_tier, rsp_flags,
 * n_anticipations, n_early). Times in seconds; the settings go to
 * FILE.meta. Shift+Esc stops the session; the rows so far are kept.
 *
 * --sim runs on the simulated display and the null device, on ysp/rt.h's
 * virtual clock, with a synthetic participant: a correct response, a
 * press in the foreperiod and then a response, an anticipation and then a
 * wrong key, a timeout. It checks its own results and exits 1 on a
 * mismatch. Under a second: the virtual clock does not wait.
 *
 * Usage: trial_audio_keyboard [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE]
 *   --reps N   repetitions of each tone (default 10; --sim: 2)
 * Exit code: 0 (also after Shift+Esc), 1 when something did not open or a
 * --sim check failed, 2 for a bad argument.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TONE_S      0.3
#define TONE_DB     (-30.0f)
#define RESPONSE_S  2.0
#define MIN_RT_S    0.1
#define ITI_S       1.0

static const char conditions_csv[] =
    "stimulus,hz,key_answer\n"
    "low,500,f\n"
    "high,1000,j\n";

static const yrsp_choice keys[] = { { .key = "f" }, { .key = "j" } };

/* The synthetic participant of --sim: presses at `at` s from the tone's
 * target time, held 0.08 s. which: 1 the correct key, 2 the other one. */
typedef struct sim_press { int trial; double at; int which; } sim_press;
static const sim_press sim_script[] = {
    { 0, 0.400, 1 },
    { 1, -0.200, 1 },   /* in the foreperiod: EARLY */
    { 1, 0.550, 1 },
    { 2, 0.050, 1 },    /* an anticipation */
    { 2, 0.500, 2 },
    /* trial 3: no press, a timeout */
};
#define SIM_TRIALS 4

static yscr_screen scr;
static ygfx_gfx gfx;
static yau_audio au;
static ytr_trials trials;
static ytr_data data;
static yrsp_collector rsp;
static uint64_t table_arena[1024];
static ytb_table tab;

/* The program's columns, after the session's; then YRSP_DATA_COLS. */
typedef struct row { int trial_index, audio_flags; bool correct; } row;
static const ytr_col cols[] = {
    YTR_COL(row, trial_index, YTR_COL_INT), YTR_COL(row, correct, YTR_COL_BOOL),
    YTR_COL(row, audio_flags, YTR_COL_INT), YRSP_DATA_COLS,
};

int main(int argc, char** argv) {
    yau_desc ad;
    yscr_frame f;
    ytr_trial_info ti = { 0 };
    yrsp_result res, sim_res[SIM_TRIALS];
    yrsp_input sim_ev[8];
    yau_onset rec;
    yau_buf tones[2];
    ygfx_stim fix;
    row r = { 0 };
    char line[1024], meta[2048], aline[512], meta_path[1100];
    const char* out_path = "trial_audio_keyboard.csv";
    const char* key_answer = "";
    uint64_t seed = 20261008;
    int i, sim = 0, reps = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0;
    int in_trial = 0, responding = 0, n_sim_ev = 0, sim_next = 0, rc = YAU_PENDING, stim_col, key_col;
    int sim_correct[SIM_TRIALS];
    int64_t t_tone = 0, t_end = 0, sim_t_press[SIM_TRIALS] = { 0 };
    yau_id id = 0;

    memset(&ad, 0, sizeof ad);
    memset(&rec, 0, sizeof rec);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_audio_keyboard [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE]\n");
            return 2;
        }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    if (reps == 0) reps = sim ? SIM_TRIALS / 2 : 10;
    if (reps < 1 || reps > 100) { fprintf(stderr, "trial_audio_keyboard: --reps takes 1..100\n"); return 2; }
    if (sim) ad.backend = YAU_BACKEND_NULL;

    /* the display and the graphics, opened together and primed */
    if (!ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .backend = sim ? YSCR_BACKEND_SIM : 0, .windowed = !fullscreen },
                          &(ygfx_desc){ .background = { 0.5f, 0.5f, 0.5f } })) {
        fprintf(stderr, "trial_audio_keyboard: %s\n", ygfx_error(&gfx));
        return 1;
    }
    fix = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_CROSS, .w = 16, .h = 16, .shape_p = { 3 },
                                         .color = { 0.2f, 0.2f, 0.2f } });

    /* the sound: the default device in the project format (48 kHz stereo) */
    ad.arena_bytes = 1 << 20;
    if (!yau_open(&au, &ad)) { fprintf(stderr, "trial_audio_keyboard: %s\n", yau_error(&au)); return 1; }

    if (!ytb_open_csv(&tab, &(ytb_csv_desc){ .text = conditions_csv, .len = sizeof conditions_csv - 1,
                                             .arena = table_arena, .arena_size = sizeof table_arena })) {
        fprintf(stderr, "trial_audio_keyboard: %s\n", ytb_error(&tab));
        return 1;
    }
    stim_col = ytb_col(&tab, "stimulus");
    key_col = ytb_col(&tab, "key_answer");
    for (i = 0; i < 2; i++) {
        yau_tone_desc t;
        memset(&t, 0, sizeof t);
        t.hz = atof(ytb_text(&tab, i, ytb_col(&tab, "hz")));
        t.dur_ns = YRT_S(TONE_S);
        t.peak = yau_db(TONE_DB);
        t.ramp_ns = YRT_MS(5);
        tones[i] = yau_tone(&au, &t);
        if (!tones[i].frames) { fprintf(stderr, "trial_audio_keyboard: %s\n", yau_error(&au)); return 1; }
    }
    /* any time: a sound is not on the frame grid */
    if (!ytr_open(&trials, &(ytr_desc){ .table = &tab, .reps = reps, .order = YTR_ORDER_RANDOM, .rng = ytr_splitmix,
            .rng_ctx = &seed, .n_jitters = 1, .jitters = { ytr_uniform("foreperiod_s", YRT_MS(600), YRT_MS(1000)) } })) {
        fprintf(stderr, "trial_audio_keyboard: %s\n", ytr_error(&trials));
        return 1;
    }

    /* f or j, within 2 s of the tone's onset, none before 0.1 s */
    if (!yrsp_open(&rsp, &(yrsp_desc){ .choices = keys, .n_choices = 2, .duration_ns = YRT_S(RESPONSE_S),
                                       .min_rt_ns = YRT_S(MIN_RT_S) })) {
        fprintf(stderr, "trial_audio_keyboard: %s\n", yrsp_error(&rsp));
        return 1;
    }

    /* the data file, and the settings beside it */
    yscr_describe(&scr, line, sizeof line);
    yau_describe(&au, aline, sizeof aline);
    snprintf(meta, sizeof meta, "screen=%s\naudio=%s\nysp_response=%s\nkeyboard=%s\n", line, aline, yrsp_version(),
             sim ? "synthetic (tier SIM)" : "SDL through yscr_event_input");
    snprintf(meta_path, sizeof meta_path, "%s.meta", out_path);
    if (!ytr_data_open(&data, &(ytr_data_desc){ .path = out_path, .trials = &trials, .cols = cols,
            .n_cols = (int)(sizeof cols / sizeof cols[0]), .meta_path = meta_path, .meta = meta })) {
        fprintf(stderr, "trial_audio_keyboard: %s\n", ytr_data_error(&data));
        return 1;
    }
    printf("%s\n%s\n", line, aline);

    for (;;) {
        int fr = yscr_begin(&scr, &f);
        if (fr == YSCR_QUIT) { aborted = 1; break; }
        if (fr != YSCR_OK) { fprintf(stderr, "trial_audio_keyboard: %s\n", yscr_error(&scr)); failed = 1; break; }

        if (!in_trial) {
            /* a new trial: the tone after the foreperiod, the window open from now */
            int c;
            if (ytr_next(&trials, &ti) < 0) break;
            c = ti.condition;
            key_answer = ytb_text(&tab, c, key_col);
            t_tone = f.onset + ytr_jitter(&trials, ti.index, 0).ns;
            id = yau_play_at(&au, tones[c], t_tone);
            if (id <= 0) { fprintf(stderr, "trial_audio_keyboard: play: %s\n", yau_strerror((int)id)); failed = 1; break; }
            yrsp_arm(&rsp, f.onset);
            in_trial = responding = 1;
            rc = YAU_PENDING;
            n_sim_ev = sim_next = 0;
            if (sim) {
                int k;
                for (k = 0; k < (int)(sizeof sim_script / sizeof sim_script[0]); k++) {
                    const sim_press* p = &sim_script[k];
                    const char* key = p->which == 1 ? key_answer : !strcmp(key_answer, "f") ? "j" : "f";
                    yrsp_input* e = &sim_ev[n_sim_ev];
                    if (p->trial != ti.index || n_sim_ev + 2 > 8) continue;
                    memset(e, 0, sizeof *e);
                    e->t = t_tone + YRT_S(p->at);
                    e->type = YRSP_PRESS;
                    e->control = (uint32_t)yrsp_scancode(key);
                    e->stamp = YRSP_TIER_SIM;
                    e->value = 1;
                    sim_ev[n_sim_ev + 1] = *e;
                    sim_ev[n_sim_ev + 1].type = YRSP_RELEASE;
                    sim_ev[n_sim_ev + 1].value = 0;
                    sim_ev[n_sim_ev + 1].t = e->t + YRT_MS(80);
                    n_sim_ev += 2;
                    if (ti.index < SIM_TRIALS) sim_t_press[ti.index] = e->t;   /* the last press is the response */
                }
            }
        }

        /* the tone's record: the plan until it completes, then the fit's time */
        if (rc == YAU_PENDING) {
            yrsp_onset o;
            rc = yau_result(&au, id, &rec);
            if (rc != YAU_OK && rc != YAU_PENDING) {
                fprintf(stderr, "trial_audio_keyboard: tone record: %s\n", yau_strerror(rc));
                failed = 1;
                break;
            }
            o = yrsp_onset_audio(&rec);
            yrsp_set_onset(&rsp, &o);
        }

        yrsp_feed_screen(&scr, &rsp, 1);
        while (sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset) yrsp_feed(&rsp, &sim_ev[sim_next++]);

        if (yrsp_update(&rsp, f.onset) == YRSP_ENDED && responding) {
            responding = 0;
            t_end = f.onset;
        }

        /* the interval is over and the record is complete: the row */
        if (!responding && rc == YAU_OK && f.onset >= t_end + YRT_S(ITI_S)) {
            yrsp_finish(&rsp, &res);
            r.trial_index = ti.index;
            r.correct = (res.flags & YRSP_R_RESPONDED) && res.response_name && !strcmp(res.response_name, key_answer);
            r.audio_flags = rec.flags;
            ytr_update(&trials, (res.flags & YRSP_R_RESPONDED) ? r.correct : YTR_INVALID, NULL);
            if (yrsp_data_set(&data, &res) < 0 || ytr_data_write(&data, &r) < 0) {
                fprintf(stderr, "trial_audio_keyboard: %s\n", ytr_data_error(&data));
                failed = 1;
            }
            printf("trial %2d %-4s tone tier %u residual %+.3f ms, %s\n", ti.index, ytb_text(&tab, ti.condition, stim_col),
                   (unsigned)res.onset_tier, (double)res.onset_residual / 1e6,
                   !(res.flags & YRSP_R_RESPONDED) ? "too slow" : r.correct ? "correct" : "wrong");
            if (ti.index < SIM_TRIALS) { sim_res[ti.index] = res; sim_correct[ti.index] = r.correct; }
            rows++;
            in_trial = 0;
        }

        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &fix);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    if (ytr_data_finish(&data) < 0) { fprintf(stderr, "trial_audio_keyboard: %s\n", ytr_data_error(&data)); failed = 1; }
    yau_describe(&au, aline, sizeof aline);
    printf("%s\n", aline);
    yau_close(&au);
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");

    if (sim && !failed && reps != SIM_TRIALS / 2) printf("--sim: the self-check needs the default --reps\n");
    else if (sim && !failed) {
        /* RT is the press minus the record's onset. On the virtual clock
         * the null device's fit is exact: the onset is on the target
         * within half a sample at 48 kHz, where the plan meets the sample
         * grid */
        int bad = rows != SIM_TRIALS;
        if (bad) fprintf(stderr, "trial_audio_keyboard: --sim: %d rows, not %d\n", rows, SIM_TRIALS);
        for (i = 0; i < rows && i < SIM_TRIALS; i++) {
            const yrsp_result* sr = &sim_res[i];
            int ok = sr->onset_src == YRSP_ONSET_AUDIO && sr->onset_tier == YAU_TIER_SIM &&
                     !(sr->flags & YRSP_R_ONSET_PLAN) && llabs(sr->onset_residual) <= 1000000000 / 48000 / 2;
            if (i == 3) ok = ok && !(sr->flags & YRSP_R_RESPONDED) && (sr->flags & YRSP_R_TIMEOUT);
            else ok = ok && (sr->flags & YRSP_R_RESPONDED) &&
                      fabs(YRT_TO_S(sr->rt_ns) - (double)(sim_t_press[i] - sr->t_onset) / 1e9) < 1e-9 && sim_correct[i] == (i < 2);
            if (i == 1) ok = ok && sr->n_early == 1;
            if (i == 2) ok = ok && sr->n_anticipations == 1;
            if (!ok) {
                fprintf(stderr, "trial_audio_keyboard: --sim: trial %d: rt %.9f flags 0x%x tier %u src %u residual %lld "
                                "n_early %d n_anticipations %d\n", i, YRT_TO_S(sr->rt_ns), (unsigned)sr->flags, (unsigned)sr->onset_tier,
                        (unsigned)sr->onset_src, (long long)sr->onset_residual, sr->n_early, sr->n_anticipations);
                bad = 1;
            }
        }
        if (bad) return 1;
        printf("--sim: every trial came out as scripted\n");
    }
    return failed;
}
