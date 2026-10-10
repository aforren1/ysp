/* trial_stop_signal.c - the stop-signal task (jspsych-contrib
 * plugin-stop-signal; parameters of STOP-IT, Verbruggen, Logan and Stevens
 * 2008) as a small experiment: ysp/trials.h, ysp/timeline.h, ysp/gfx.h,
 * ysp/audio.h, ysp/stair.h and ysp/response.h.
 *
 * Each trial: a fixation cross for 0.25 s, then a circle (F) or a square
 * (J) until a response or 1.25 s. On a quarter of the trials a 750 Hz
 * tone of 75 ms, the stop signal, plays at the stop-signal delay (SSD)
 * after the go stimulus: then no key is the right answer. A blank interval
 * of 1 s follows. The SSD follows a 1-up/1-down staircase (ysp/stair.h):
 * 0.05 s longer after a stop, 0.05 s shorter after a failed stop, from
 * 0.25 s, within 0.05 to 1.15 s.
 *
 * The display and the sound share one clock (ysp/rt.h). The tone is
 * planned at the go stimulus's planned onset plus the SSD; the data row
 * has the SSD as shown (ssd_actual: the tone's onset record minus the go
 * stimulus's flip onset), and the staircase records that value. RT runs
 * from the go stimulus's flip onset.
 *
 * The data file has one row per trial: trial_index, stimulus, key_answer,
 * signal (go or stop), ssd, ssd_actual, response, rt, correct, stopped;
 * then go_tier, tone_tier, rsp_tier, rsp_flags. Times in seconds. The run
 * ends with p(respond | signal), the mean SSD, the mean go RT and the
 * SSRT by the mean method (go RT minus SSD: rough; use the integration
 * method on the data file). Shift+Esc stops the session; the rows so far
 * are kept.
 *
 * --sim runs one block of 8 trials (2 with a signal) on the simulated
 * display and miniaudio's null device with a synthetic participant who
 * stops on the first signal and fails on the second. It checks its own
 * results and exits 1 on a mismatch. About 16 s.
 *
 * Usage: trial_stop_signal [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE]
 *   --reps N   blocks of 8 trials, 2 with a signal (default 8; --sim: 1)
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

enum { FIX_ON, CIRCLE_ON, SQUARE_ON, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };

#define FIXATION_S  0.25
#define RESPONSE_S  1.25         /* the go stimulus's maximum duration */
#define ITI_S       1.0
#define TONE_S      0.075

static const char conditions_csv[] =
    "stimulus,key_answer,signal\n"
    "circle,f,go\n"
    "circle,f,go\n"
    "circle,f,go\n"
    "circle,f,stop\n"
    "square,j,go\n"
    "square,j,go\n"
    "square,j,go\n"
    "square,j,stop\n";

static const yrsp_choice keys[] = { { .key = "f" }, { .key = "j" } };

/* The synthetic participant of --sim: the correct key 0.50 s after each go
 * stimulus; on the first signal no key, on the second the correct key at
 * 0.45 s (after the SSD of 0.30 s: a failed stop). */
#define SIM_GO_RT   0.50
#define SIM_FAIL_RT 0.45

static yscr_screen scr;
static ygfx_gfx gfx;
static yau_audio au;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static yst_stair stair;
static yrsp_collector rsp;
static uint64_t table_arena[2048];
static ytb_table tab;

static ygfx_stim fix, circle, square;
static const ygfx_bind binds[] = {
    { .stim = &fix,    .param = YGFX_P_VISIBLE, .channel = FIX_ON    },
    { .stim = &circle, .param = YGFX_P_VISIBLE, .channel = CIRCLE_ON },
    { .stim = &square, .param = YGFX_P_VISIBLE, .channel = SQUARE_ON },
};

static ygfx_stim shape(ygfx_shape_kind kind, float w, float p0, float c) {
    ygfx_shape_desc d;
    memset(&d, 0, sizeof d);
    d.shape = kind;
    d.w = d.h = w;
    d.shape_p[0] = p0;
    d.edge = YGFX_EDGE_COSINE;
    d.edge_width = 1;
    d.color[0] = d.color[1] = d.color[2] = c;
    return ygfx_shape(&d);
}

static void sec(char* out, size_t cap, double s) {
    if (s != s) out[0] = 0;
    else snprintf(out, cap, "%.9f", s);
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    yau_desc ad;
    yau_tone_desc tdesc;
    ytl_desc td;
    ytb_csv_desc cd;
    ytr_desc rd;
    yst_desc stdesc;
    yrsp_desc pd;
    yscr_frame f;
    ytl_event fired[16];
    ytr_trial_info ti = { 0 };
    yrsp_result res;
    yrsp_onset go_onset;
    yrsp_input sim_ev[4];
    yau_onset rec;
    yau_buf tone;
    FILE* out;
    char line[1024], meta[2048], aline[512];
    const char* out_path = "trial_stop_signal.csv";
    uint64_t seed = 20261008;
    int i, n, sim = 0, reps = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0;
    int in_trial = 0, responding = 0, row_due = 0, stop = 0, stim_ch = 0, n_sim_ev = 0, sim_next = 0;
    int stim_col, key_col, signal_col, n_stop = 0, n_respond = 0, n_go_rt = 0, sim_stopped[2] = { -1, -1 };
    int64_t go_frame = -1, go_bt = 0;
    yau_id id = 0;
    double ssd = 0, sum_go_rt = 0, sum_ssd = 0, sim_ssd[2] = { 0, 0 }, sim_ssd_actual[2] = { 0, 0 };
    double sim_go_rt = 0;
    char key_answer[8] = "";

    memset(&sd, 0, sizeof sd);
    memset(&ad, 0, sizeof ad);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_stop_signal [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE]\n");
            return 2;
        }
    }
    if (reps == 0) reps = sim ? 1 : 8;
    if (reps < 1 || reps > 100) { fprintf(stderr, "trial_stop_signal: --reps takes 1..100\n"); return 2; }
    if (sim) {
        sd.backend = YSCR_BACKEND_SIM;
        ad.backend = YAU_BACKEND_NULL;
    }
    sd.windowed = !fullscreen;

    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "trial_stop_signal: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "trial_stop_signal: %s\n", ygfx_error(&gfx)); return 1; }
    fix = shape(YGFX_CROSS, 16, 3, 0.2f);
    circle = shape(YGFX_CIRCLE, 120, 0, 0.9f);
    square = shape(YGFX_RECT, 106, 0, 0.9f);

    ad.arena_bytes = 1 << 20;
    if (!yau_open(&au, &ad)) { fprintf(stderr, "trial_stop_signal: %s\n", yau_error(&au)); return 1; }
    memset(&tdesc, 0, sizeof tdesc);
    tdesc.hz = 750;
    tdesc.dur = YAU_S(TONE_S);
    tdesc.peak = yau_db(-30);
    tdesc.ramp = YAU_MS(5);
    tone = yau_tone(&au, &tdesc);
    if (!tone.frames) { fprintf(stderr, "trial_stop_signal: %s\n", yau_error(&au)); return 1; }

    memset(&cd, 0, sizeof cd);
    cd.text = conditions_csv;
    cd.len = sizeof conditions_csv - 1;
    cd.arena = table_arena;
    cd.arena_size = sizeof table_arena;
    if (!ytb_csv(&tab, &cd)) { fprintf(stderr, "trial_stop_signal: %s\n", ytb_error(&tab)); return 1; }
    stim_col = ytb_col(&tab, "stimulus");
    key_col = ytb_col(&tab, "key_answer");
    signal_col = ytb_col(&tab, "signal");
    memset(&rd, 0, sizeof rd);
    rd.table = &tab;
    rd.reps = reps;
    rd.order = YTR_ORDER_RANDOM;
    rd.rng = ytr_splitmix;
    rd.rng_ctx = &seed;
    if (!ytr_open(&trials, &rd)) { fprintf(stderr, "trial_stop_signal: %s\n", ytr_error(&trials)); return 1; }

    /* the SSD: a stop (response 1) makes it longer, harder */
    memset(&stdesc, 0, sizeof stdesc);
    stdesc.start = 0.25;
    stdesc.steps[0] = 0.05;
    stdesc.n_steps = 1;
    stdesc.harder_is_up = true;
    stdesc.min = 0.05;
    stdesc.max = 1.15;
    stdesc.use_limits = true;
    stdesc.stop_trials = 2 * reps;
    if (!yst_open(&stair, &stdesc)) { fprintf(stderr, "trial_stop_signal: %s\n", yst_error(&stair)); return 1; }

    memset(&td, 0, sizeof td);
    td.events = storage;
    td.event_capacity = 64;
    td.n_channels = N_CHANNELS;
    if (!ytl_open(&tl, &td)) { fprintf(stderr, "trial_stop_signal: %s\n", ytl_error(&tl)); return 1; }

    /* f or j within 1.25 s of the go stimulus; on a signal trial a key is a failed stop */
    memset(&pd, 0, sizeof pd);
    pd.choices = keys;
    pd.n_choices = 2;
    pd.duration = RESPONSE_S;
    if (!yrsp_init(&rsp, &pd)) { fprintf(stderr, "trial_stop_signal: %s\n", yrsp_error(&rsp)); return 1; }

    out = fopen(out_path, "w");
    if (!out) { fprintf(stderr, "trial_stop_signal: cannot write %s\n", out_path); return 1; }
    ytr_format_meta(&trials, meta, sizeof meta);
    meta[strcspn(meta, "\r\n")] = 0;
    yscr_describe(&scr, line, sizeof line);
    yau_describe(&au, aline, sizeof aline);
    fprintf(out, "# %s\n# %s\n# %s\n# ysp_stair %s, 1-up/1-down, step 0.05 s; ysp_response %s; keyboard: %s\n", meta,
            line, aline, yst_version(), yrsp_version(), sim ? "synthetic (tier SIM)" : "SDL through yscr_event_input");
    fprintf(out, "trial_index,stimulus,key_answer,signal,ssd,ssd_actual,response,rt,correct,stopped,go_tier,tone_tier,"
                 "rsp_tier,rsp_flags\n");
    printf("%s\n%s\n", line, aline);
    memset(&go_onset, 0, sizeof go_onset);
    memset(&rec, 0, sizeof rec);

    for (;;) {
        SDL_Event ev;
        int64_t bt = 0;
        yrsp_input in;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_stop_signal: %s\n", yscr_error(&scr)); failed = 1; break; }

        for (i = 0; i < f.n_done; i++)
            if (f.done[i].index == go_frame) {
                go_onset = yrsp_onset_flip(&f.done[i]);
                yrsp_set_onset(&rsp, &go_onset);
            }

        if (!in_trial) {
            /* the go stimulus after the fixation; on a signal trial the tone
             * at its planned onset plus the SSD */
            ytl_seq q;
            const char* ka;
            int64_t t_go;
            if (ytr_next(&trials, &ti) < 0) break;
            ka = ytb_text(&tab, ti.condition, key_col);
            snprintf(key_answer, sizeof key_answer, "%s", ka ? ka : "");
            stim_ch = !strcmp(ytb_text(&tab, ti.condition, stim_col), "circle") ? CIRCLE_ON : SQUARE_ON;
            stop = !strcmp(ytb_text(&tab, ti.condition, signal_col), "stop");
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            ytl_on(&q, FIX_ON);
            ytl_wait(&q, YRT_S(FIXATION_S));
            ytl_off(&q, FIX_ON);
            go_bt = q.t;
            ytl_on(&q, stim_ch);
            if (q.err) { fprintf(stderr, "trial_stop_signal: timeline error %d\n", q.err); failed = 1; break; }
            ytl_rt_time(&tl, TRIAL, go_bt, &t_go);
            id = 0;
            ssd = NAN;
            if (stop) {
                ssd = yst_next(&stair);
                id = yau_play_at(&au, tone, t_go + YAU_S(ssd));
                if (id <= 0) { fprintf(stderr, "trial_stop_signal: play: %s\n", yau_strerror((int)id)); failed = 1; break; }
            }
            yrsp_arm(&rsp, f.onset);
            go_frame = -1;
            memset(&go_onset, 0, sizeof go_onset);
            in_trial = responding = 1;
            row_due = 0;
            n_sim_ev = sim_next = 0;
            if (sim && (!stop || n_stop == 1)) {
                yrsp_input* e = &sim_ev[0];
                memset(e, 0, sizeof *e);
                e->t = t_go + YAU_S(stop ? SIM_FAIL_RT : SIM_GO_RT);
                e->type = YRSP_PRESS;
                e->control = (uint32_t)yrsp_scancode(key_answer);
                e->stamp = YRSP_TIER_SIM;
                e->value = 1;
                sim_ev[1] = *e;
                sim_ev[1].type = YRSP_RELEASE;
                sim_ev[1].value = 0;
                sim_ev[1].t = e->t + YAU_MS(80);
                n_sim_ev = 2;
            }
        }

        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) yrsp_feed(&rsp, &in);
        while (sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset) yrsp_feed(&rsp, &sim_ev[sim_next++]);

        /* a key or the deadline: the go stimulus off on this frame */
        if (responding && yrsp_update(&rsp, f.onset) == YRSP_ENDED) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, stim_ch);
            ytl_wait(&q, YRT_S(ITI_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "trial_stop_signal: timeline error %d\n", q.err); failed = 1; break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 16);
        for (i = 0; i < n && i < 16; i++) {
            if (fired[i].kind == YTL_ONSET && fired[i].target == stim_ch) {
                go_onset = yrsp_onset_landing(&fired[i]);
                yrsp_set_onset(&rsp, &go_onset);
                go_frame = fired[i].frame;
            }
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) row_due = 1;
        }

        /* the interval is over and, on a signal trial, the tone's record is in */
        if (row_due) {
            int arc = stop ? yau_result(&au, id, &rec) : YAU_OK;
            if (arc != YAU_OK && arc != YAU_PENDING) {
                fprintf(stderr, "trial_stop_signal: tone record: %s\n", yau_strerror(arc));
                failed = 1;
                break;
            }
            if (arc == YAU_OK) {
                char s_ssd[32], s_act[32], rt[32], tone_tier[8] = "";
                int responded, correct, stopped = -1;
                double actual = NAN;
                yrsp_finish(&rsp, &res);
                responded = (res.flags & YRSP_R_RESPONDED) != 0;
                correct = stop ? !responded : responded && !strcmp(res.response_name, key_answer);
                if (stop) {
                    actual = (double)(rec.onset - res.t_onset) / 1e9;
                    stopped = !responded;
                    yst_update(&stair, actual, stopped);
                    if (n_stop < 2) { sim_stopped[n_stop] = stopped; sim_ssd[n_stop] = ssd; sim_ssd_actual[n_stop] = actual; }
                    n_stop++;
                    n_respond += responded;
                    sum_ssd += actual;
                } else if (correct) {
                    sum_go_rt += res.rt;
                    n_go_rt++;
                    sim_go_rt = res.rt;
                }
                if (stop) snprintf(tone_tier, sizeof tone_tier, "%u", (unsigned)rec.tier);
                sec(s_ssd, sizeof s_ssd, ssd);
                sec(s_act, sizeof s_act, actual);
                sec(rt, sizeof rt, res.rt);
                fprintf(out, "%d,%s,%s,%s,%s,%s,%s,%s,%d,%s,%u,%s,%u,%u\n", ti.index,
                        ytb_text(&tab, ti.condition, stim_col), key_answer, stop ? "stop" : "go", s_ssd, s_act,
                        res.response_name ? res.response_name : "", rt, correct, stopped < 0 ? "" : stopped ? "1" : "0",
                        (unsigned)res.onset_tier, tone_tier, (unsigned)res.stamp_tier, (unsigned)res.flags);
                fflush(out);
                printf("trial %2d %-6s %-4s ssd %s (shown %s) rt %s %s\n", ti.index, ytb_text(&tab, ti.condition, stim_col),
                       stop ? "stop" : "go", s_ssd[0] ? s_ssd : "-", s_act[0] ? s_act : "-", rt[0] ? rt : "-",
                       stop ? (stopped ? "stopped" : "failed stop") : correct ? "correct" : responded ? "wrong" : "miss");
                ytr_update(&trials, correct, NULL);
                rows++;
                in_trial = 0;
                row_due = 0;
            }
        }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &fix);
        ygfx_draw(&gfx, &circle);
        ygfx_draw(&gfx, &square);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    fclose(out);
    yau_close(&au);
    ygfx_close(&gfx);
    yscr_close(&scr);
    if (n_stop && n_go_rt)
        printf("p(respond | signal) %.2f, mean ssd %.3f s, mean go rt %.3f s, ssrt (mean method) %.3f s\n",
               (double)n_respond / n_stop, sum_ssd / n_stop, sum_go_rt / n_go_rt, sum_go_rt / n_go_rt - sum_ssd / n_stop);
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");

    if (sim && !failed && reps != 1) printf("--sim: the self-check needs the default --reps\n");
    else if (sim && !failed) {
        /* the SSD 0.25 then 0.30 s; shown within the null device's 10 ms
         * period of it (the go stimulus is on the plan's frame there) */
        int bad = rows != 8 || n_stop != 2 || n_go_rt != 6 || fabs(sim_go_rt - SIM_GO_RT) > 1e-6;
        for (i = 0; i < 2; i++)
            if (sim_stopped[i] != !i || fabs(sim_ssd[i] - 0.25 - 0.05 * i) > 1e-9 ||
                fabs(sim_ssd_actual[i] - sim_ssd[i]) > 0.010)
                bad = 1;
        if (bad) {
            fprintf(stderr, "trial_stop_signal: --sim: %d rows, %d signals, %d go rts (last %.9f); stopped %d %d, ssd %.3f "
                            "%.3f, shown %.6f %.6f\n", rows, n_stop, n_go_rt, sim_go_rt, sim_stopped[0], sim_stopped[1],
                    sim_ssd[0], sim_ssd[1], sim_ssd_actual[0], sim_ssd_actual[1]);
            return 1;
        }
        printf("--sim: every trial came out as scripted\n");
    }
    return failed;
}
