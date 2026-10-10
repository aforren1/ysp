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
 * The data file has one row per trial: trial_index, stimulus, key_answer,
 * response, rt, correct (jsPsych's names); then foreperiod, onset_tier,
 * onset_src, audio_residual (onset minus target), audio_flags
 * (YAU_ONSET_*), rsp_tier, rsp_flags, n_early, n_anticipations. Times in
 * seconds. Shift+Esc stops the session; the rows so far are kept.
 *
 * --sim runs on the simulated display and miniaudio's null device with a
 * synthetic participant: a correct response, a press in the foreperiod
 * and then a response, an anticipation and then a wrong key, a timeout.
 * It checks its own results and exits 1 on a mismatch. About 12 s.
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
static yrsp_collector rsp;
static uint64_t table_arena[1024];
static ytb_table tab;

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    yau_desc ad;
    ytb_csv_desc cd;
    ytr_desc rd;
    yrsp_desc pd;
    yscr_frame f;
    ytr_trial_info ti = { 0 };
    yrsp_result res, sim_res[SIM_TRIALS];
    yrsp_input sim_ev[8];
    yau_onset rec;
    yau_buf tones[2];
    ygfx_stim fix;
    FILE* out;
    char line[1024], meta[2048], aline[512];
    const char* out_path = "trial_audio_keyboard.csv";
    uint64_t seed = 20261008;
    int i, sim = 0, reps = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0;
    int in_trial = 0, responding = 0, n_sim_ev = 0, sim_next = 0, rc = YAU_PENDING, stim_col, key_col;
    int sim_correct[SIM_TRIALS];
    int64_t t_tone = 0, t_end = 0, sim_t_press[SIM_TRIALS] = { 0 };
    yau_id id = 0;
    char key_answer[8] = "";

    memset(&sd, 0, sizeof sd);
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
    if (reps == 0) reps = sim ? SIM_TRIALS / 2 : 10;
    if (reps < 1 || reps > 100) { fprintf(stderr, "trial_audio_keyboard: --reps takes 1..100\n"); return 2; }
    if (sim) {
        sd.backend = YSCR_BACKEND_SIM;
        ad.backend = YAU_BACKEND_NULL;
    }
    sd.windowed = !fullscreen;

    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "trial_audio_keyboard: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "trial_audio_keyboard: %s\n", ygfx_error(&gfx)); return 1; }
    {
        ygfx_shape_desc d;
        memset(&d, 0, sizeof d);
        d.shape = YGFX_CROSS;
        d.w = d.h = 16;
        d.shape_p[0] = 3;
        d.color[0] = d.color[1] = d.color[2] = 0.2f;
        fix = ygfx_shape(&d);
    }

    /* the sound: the default device in the project format (48 kHz stereo) */
    ad.arena_bytes = 1 << 20;
    if (!yau_open(&au, &ad)) { fprintf(stderr, "trial_audio_keyboard: %s\n", yau_error(&au)); return 1; }

    memset(&cd, 0, sizeof cd);
    cd.text = conditions_csv;
    cd.len = sizeof conditions_csv - 1;
    cd.arena = table_arena;
    cd.arena_size = sizeof table_arena;
    if (!ytb_csv(&tab, &cd)) { fprintf(stderr, "trial_audio_keyboard: %s\n", ytb_error(&tab)); return 1; }
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
    memset(&rd, 0, sizeof rd);
    rd.table = &tab;
    rd.reps = reps;
    rd.order = YTR_ORDER_RANDOM;
    rd.rng = ytr_splitmix;
    rd.rng_ctx = &seed;
    rd.jitters[0] = ytr_uniform("foreperiod", 0.6, 1.0);   /* any time: a sound is not on the frame grid */
    rd.n_jitters = 1;
    if (!ytr_open(&trials, &rd)) { fprintf(stderr, "trial_audio_keyboard: %s\n", ytr_error(&trials)); return 1; }

    /* f or j, within 2 s of the tone's onset, none before 0.1 s */
    memset(&pd, 0, sizeof pd);
    pd.choices = keys;
    pd.n_choices = 2;
    pd.duration_ns = YRT_S(RESPONSE_S);
    pd.min_rt_ns = YRT_S(MIN_RT_S);
    if (!yrsp_open(&rsp, &pd)) { fprintf(stderr, "trial_audio_keyboard: %s\n", yrsp_error(&rsp)); return 1; }

    out = fopen(out_path, "w");
    if (!out) { fprintf(stderr, "trial_audio_keyboard: cannot write %s\n", out_path); return 1; }
    ytr_format_meta(&trials, meta, sizeof meta);
    meta[strcspn(meta, "\r\n")] = 0;
    yscr_describe(&scr, line, sizeof line);
    yau_describe(&au, aline, sizeof aline);
    fprintf(out, "# %s\n# %s\n# %s\n# ysp_response %s; keyboard: %s\n", meta, line, aline, yrsp_version(),
            sim ? "synthetic (tier SIM)" : "SDL through yscr_event_input");
    fprintf(out, "trial_index,stimulus,key_answer,response,rt,correct,foreperiod,onset_tier,onset_src,audio_residual,"
                 "audio_flags,rsp_tier,rsp_flags,n_early,n_anticipations\n");
    printf("%s\n%s\n", line, aline);

    for (;;) {
        SDL_Event ev;
        yrsp_input in;
        int fr = yscr_begin(&scr, &f);
        if (fr == YSCR_QUIT) { aborted = 1; break; }
        if (fr != YSCR_OK) { fprintf(stderr, "trial_audio_keyboard: %s\n", yscr_error(&scr)); failed = 1; break; }

        if (!in_trial) {
            /* a new trial: the tone after the foreperiod, the window open from now */
            int c;
            const char* ka;
            if (ytr_next(&trials, &ti) < 0) break;
            c = ti.condition;
            ka = ytb_text(&tab, c, key_col);
            snprintf(key_answer, sizeof key_answer, "%s", ka ? ka : "");
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

        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) yrsp_feed(&rsp, &in);
        while (sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset) yrsp_feed(&rsp, &sim_ev[sim_next++]);

        if (responding && yrsp_update(&rsp, f.onset) == YRSP_ENDED) {
            responding = 0;
            t_end = f.onset;
        }

        /* the interval is over and the record is complete: the row */
        if (!responding && rc == YAU_OK && f.onset >= t_end + YRT_S(ITI_S)) {
            char rt[32];
            int correct;
            yrsp_finish(&rsp, &res);
            correct = (res.flags & YRSP_R_RESPONDED) && res.response_name && !strcmp(res.response_name, key_answer);
            if (res.rt_ns != YRSP_NO_TIME) snprintf(rt, sizeof rt, "%.9f", YRT_TO_S(res.rt_ns));
            else rt[0] = 0;
            fprintf(out, "%d,%s,%s,%s,%s,%d,%.9f,%u,%u,%.9f,%u,%u,%u,%d,%d\n", ti.index,
                    ytb_text(&tab, ti.condition, stim_col), key_answer, res.response_name ? res.response_name : "", rt,
                    correct, ytr_jitter(&trials, ti.index, 0).s, (unsigned)res.onset_tier, (unsigned)res.onset_src,
                    (double)res.onset_residual / 1e9, (unsigned)rec.flags, (unsigned)res.stamp_tier, (unsigned)res.flags,
                    res.n_early, res.n_anticipations);
            fflush(out);
            printf("trial %2d %-4s tone tier %u residual %+.3f ms rt %s response %s %s\n", ti.index,
                   ytb_text(&tab, ti.condition, stim_col), (unsigned)res.onset_tier, (double)res.onset_residual / 1e6,
                   rt[0] ? rt : "-", res.response_name ? res.response_name : "-",
                   !(res.flags & YRSP_R_RESPONDED) ? "(too slow)" : correct ? "correct" : "wrong");
            ytr_update(&trials, (res.flags & YRSP_R_RESPONDED) ? correct : YTR_INVALID, NULL);
            if (ti.index < SIM_TRIALS) { sim_res[ti.index] = res; sim_correct[ti.index] = correct; }
            rows++;
            in_trial = 0;
        }

        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &fix);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    fclose(out);
    yau_describe(&au, aline, sizeof aline);
    printf("%s\n", aline);
    yau_close(&au);
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");

    if (sim && !failed && reps != SIM_TRIALS / 2) printf("--sim: the self-check needs the default --reps\n");
    else if (sim && !failed) {
        /* RT is the press minus the record's onset, which the null
         * device's fit puts within a period (10 ms) of the target */
        int bad = rows != SIM_TRIALS;
        if (bad) fprintf(stderr, "trial_audio_keyboard: --sim: %d rows, not %d\n", rows, SIM_TRIALS);
        for (i = 0; i < rows && i < SIM_TRIALS; i++) {
            const yrsp_result* r = &sim_res[i];
            int ok = r->onset_src == YRSP_ONSET_AUDIO && r->onset_tier == YAU_TIER_SIM &&
                     !(r->flags & YRSP_R_ONSET_PLAN) && llabs(r->onset_residual) <= YRT_MS(10);
            if (i == 3) ok = ok && !(r->flags & YRSP_R_RESPONDED) && (r->flags & YRSP_R_TIMEOUT);
            else ok = ok && (r->flags & YRSP_R_RESPONDED) &&
                      fabs(YRT_TO_S(r->rt_ns) - (double)(sim_t_press[i] - r->t_onset) / 1e9) < 1e-9 && sim_correct[i] == (i < 2);
            if (i == 1) ok = ok && r->n_early == 1;
            if (i == 2) ok = ok && r->n_anticipations == 1;
            if (!ok) {
                fprintf(stderr, "trial_audio_keyboard: --sim: trial %d: rt %.9f flags 0x%x tier %u src %u residual %lld "
                                "n_early %d n_anticipations %d\n", i, YRT_TO_S(r->rt_ns), (unsigned)r->flags, (unsigned)r->onset_tier,
                        (unsigned)r->onset_src, (long long)r->onset_residual, r->n_early, r->n_anticipations);
                bad = 1;
            }
        }
        if (bad) return 1;
        printf("--sim: every trial came out as scripted\n");
    }
    return failed;
}
