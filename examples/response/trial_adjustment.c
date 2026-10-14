/* trial_adjustment.c - the jsPsych reconstruction trial (the method of
 * adjustment) as a small experiment: ysp/trials.h, ysp/timeline.h,
 * ysp/gfx.h and ysp/response.h.
 *
 * Each trial: a fixation cross for 0.5 s, then two bars. The left one is
 * the reference (120, 160 or 200 px long); the right one starts at a
 * random length. G makes it shorter and H longer (jsPsych's key_decrease
 * and key_increase), one step of 0.01 per press, and a held key steps at
 * the OS's repeat rate. Space confirms (jsPsych has a button). The
 * parameter is 0..1, the bar 40 + 280 x value px long, as jsPsych's
 * stim_function maps its parameter. A blank interval of 0.8 s follows.
 *
 * The collector takes the confirm key only: RT runs from the bars' flip
 * onset, with its tier. The adjust keys come from the same event loop
 * (ysp/input.h events from yscr_event_input()) and step the value
 * directly: a stream of steps is not a response. yscr_event_input()
 * drops a key's second report (ysp/screen.h v0.4.1), so a virtual-key
 * tap that SDL reports twice steps once.
 *
 * Two data files. The trial file (--out) has one row per trial:
 * trial_index, reference, start_value, final_value (jsPsych's names),
 * error_px, n_steps, rt; then onset_tier, onset_src, rsp_tier, rsp_flags.
 * The trajectory file (--traj) has one row per step: trial_index, t (from
 * the bars' onset), key, repeat (1 for an OS repeat), value. Times in
 * seconds. Shift+Esc stops the session; the rows so far are kept.
 *
 * --sim runs the 3 references once on the simulated display with a
 * synthetic participant: taps, a tap reported twice 9 ms apart (through
 * the same filter, yscr_key_filter()), a held key with OS repeats, a
 * confirm. It
 * checks its own results and exits 1 on a mismatch.
 * Under a second: the virtual clock does not wait.
 *
 * Usage: trial_adjustment [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE] [--traj FILE]
 *   --reps N   repetitions of each reference (default 4; --sim: 1)
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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FIX_ON, BARS_ON, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };

#define FIXATION_S 0.5
#define ITI_S      0.8
#define STEP       0.01
#define MAX_STEPS  4096           /* per trial, in memory until the trial ends */

static const char conditions_csv[] =
    "reference\n"
    "120\n"
    "160\n"
    "200\n";

static const yrsp_choice confirm[] = { { .key = "space", .name = "confirm" } };

static float bar_length(double value) { return (float)(40.0 + 280.0 * value); }

/* One step of the trajectory. */
typedef struct step { int64_t t; double value; uint8_t key, repeat; } step;

/* The synthetic participant of --sim, from the bars' onset: `n` presses of
 * key ('g' or 'h') from `at` s, `every` s apart; repeat 1 marks presses 2
 * to n as OS repeats of one held key; twice 1 reports each tap again 9
 * ms later (SDL 3.4's virtual-key tap). Then space at `confirm_at`. */
typedef struct sim_burst { int trial; char key; double at, every; int n, repeat, twice; } sim_burst;
static const sim_burst sim_script[] = {
    { 0, 'h', 0.40, 0.15, 4, 0, 0 },
    { 0, 'g', 1.20, 0.15, 1, 0, 1 },   /* one step, not two */
    { 1, 'g', 0.50, 0.033, 15, 1, 0 }, /* held for 0.5 s */
    { 2, 'h', 0.30, 0.20, 3, 0, 0 },
    { 2, 'h', 1.00, 0.033, 10, 1, 0 },
};
static const double sim_confirm_at[3] = { 1.80, 1.40, 1.90 };
#define SIM_TRIALS 3

static yscr_screen scr;
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static yrsp_collector rsp;
static uint64_t table_arena[1024];
static ytb_table tab;
static step steps[MAX_STEPS];
static int n_steps, n_lost_steps;
static double value;
static yrsp_input sim_ev[64];

static ygfx_stim fix, reference, comparison;
static const ygfx_bind binds[] = {
    { .stim = &fix,        .param = YGFX_P_VISIBLE, .channel = FIX_ON  },
    { .stim = &reference,  .param = YGFX_P_VISIBLE, .channel = BARS_ON },
    { .stim = &comparison, .param = YGFX_P_VISIBLE, .channel = BARS_ON },
};

static ygfx_stim bar(float x) {
    ygfx_shape_desc d;
    memset(&d, 0, sizeof d);
    d.shape = YGFX_RECT;
    d.x = x;
    d.w = 100;
    d.h = 10;
    d.edge = YGFX_EDGE_COSINE;
    d.edge_width = 1;
    d.color[0] = d.color[1] = d.color[2] = 0.9f;
    return ygfx_shape(&d);
}

/* G and H while the bars are up: one step per press, OS repeats too, so a
 * held key keeps moving the bar. */
static void adjust(const yrsp_input* in, int bars_up) {
    int dir;
    if (!bars_up || in->kind != YRSP_KIND_KEYBOARD || in->type != YRSP_PRESS) return;
    dir = in->control == (uint32_t)yrsp_scancode("h") ? 1 : in->control == (uint32_t)yrsp_scancode("g") ? -1 : 0;
    if (!dir) return;
    value += dir * STEP;
    value = value < 0 ? 0 : value > 1 ? 1 : value;
    value = floor(value / STEP + 0.5) * STEP;     /* no drift from adding 0.01 */
    if (n_steps < MAX_STEPS) {
        steps[n_steps].t = in->t;
        steps[n_steps].value = value;
        steps[n_steps].key = dir > 0 ? 'h' : 'g';
        steps[n_steps++].repeat = (in->flags & YRSP_IN_REPEAT) ? 1 : 0;
    } else n_lost_steps++;
}

int main(int argc, char** argv) {
    yscr_frame f;
    ytl_event fired[64];                   /* the timeline's capacity: all that can fire */
    ytr_trial_info ti = { 0 };
    yrsp_result res, sim_res[SIM_TRIALS];
    FILE *out, *traj;
    char line[1024], meta[2048];
    const char* out_path = "trial_adjustment.csv";
    const char* traj_path = "trial_adjustment_trajectory.csv";
    uint64_t seed = 20261008;
    int i, n, sim = 0, reps = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0;
    int in_trial = 0, responding = 0, bars_up = 0, n_sim_ev = 0, sim_next = 0, ref_col;
    int sim_steps[SIM_TRIALS];
    double start_value = 0, sim_start[SIM_TRIALS], sim_final[SIM_TRIALS];
    int64_t sim_plan_onset[SIM_TRIALS] = { 0 };   /* where the script's times start */

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--traj") && i + 1 < argc) traj_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_adjustment [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE] [--traj FILE]\n");
            return 2;
        }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    if (reps == 0) reps = sim ? 1 : 4;
    if (reps < 1 || reps > 100) { fprintf(stderr, "trial_adjustment: --reps takes 1..100\n"); return 2; }
    /* the display and the graphics, opened together and primed */
    if (!ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .backend = sim ? YSCR_BACKEND_SIM : 0, .windowed = !fullscreen },
                          &(ygfx_desc){ .background = { 0.5f, 0.5f, 0.5f } })) {
        fprintf(stderr, "trial_adjustment: %s\n", ygfx_error(&gfx));
        return 1;
    }
    fix = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_CROSS, .w = 16, .h = 16, .shape_p = { 3 },
                                         .color = { 0.2f, 0.2f, 0.2f } });
    reference = bar(-200);
    comparison = bar(200);

    if (!ytb_open_csv(&tab, &(ytb_csv_desc){ .text = conditions_csv, .len = sizeof conditions_csv - 1,
                                             .arena = table_arena, .arena_size = sizeof table_arena })) {
        fprintf(stderr, "trial_adjustment: %s\n", ytb_error(&tab));
        return 1;
    }
    ref_col = ytb_col(&tab, "reference");
    if (!ytr_open(&trials, &(ytr_desc){ .table = &tab, .reps = reps, .order = YTR_ORDER_RANDOM,
                                        .rng = ytr_splitmix, .rng_ctx = &seed })) {
        fprintf(stderr, "trial_adjustment: %s\n", ytr_error(&trials));
        return 1;
    }
    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS })) {
        fprintf(stderr, "trial_adjustment: %s\n", ytl_error(&tl));
        return 1;
    }
    /* the confirm key, no deadline */
    if (!yrsp_open(&rsp, &(yrsp_desc){ .choices = confirm, .n_choices = 1 })) {
        fprintf(stderr, "trial_adjustment: %s\n", yrsp_error(&rsp));
        return 1;
    }

    out = fopen(out_path, "w");
    traj = fopen(traj_path, "w");
    if (!out || !traj) { fprintf(stderr, "trial_adjustment: cannot write %s or %s\n", out_path, traj_path); return 1; }
    ytr_format_meta(&trials, meta, sizeof meta);
    meta[strcspn(meta, "\r\n")] = 0;
    yscr_describe(&scr, line, sizeof line);
    fprintf(out, "# %s\n# %s\n# ysp_response %s; keyboard: %s; step %.2f, length 40 + 280 x value px\n", meta, line,
            yrsp_version(), sim ? "synthetic (tier SIM)" : "SDL through yscr_event_input", STEP);
    fprintf(out, "trial_index,reference,start_value,final_value,error_px,n_steps,rt,onset_tier,onset_src,rsp_tier,"
                 "rsp_flags\n");
    fprintf(traj, "trial_index,t,key,repeat,value\n");
    printf("%s\n", line);

    for (;;) {
        SDL_Event ev;
        int64_t bt = 0;
        yrsp_input in;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_adjustment: %s\n", yscr_error(&scr)); failed = 1; break; }

        if (!in_trial) {
            ytl_seq q;
            if (ytr_next(&trials, &ti) < 0) break;
            start_value = floor((0.25 + 0.5 * ytr_splitmix(&seed)) / STEP + 0.5) * STEP;
            value = start_value;
            n_steps = n_lost_steps = 0;
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            ytl_on(&q, FIX_ON);
            ytl_wait(&q, YRT_S(FIXATION_S));
            ytl_off(&q, FIX_ON);
            ytl_on(&q, BARS_ON);
            if (q.err) { fprintf(stderr, "trial_adjustment: timeline error %d\n", q.err); failed = 1; break; }
            yrsp_arm(&rsp, f.onset);
            in_trial = responding = 1;
            n_sim_ev = sim_next = 0;
        }

        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) {
                yrsp_feed(&rsp, &in);
                if (responding) adjust(&in, bars_up);
            }
        for (; sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset; sim_next++) {
            if (!yscr_key_filter(&scr, &sim_ev[sim_next])) continue;
            yrsp_feed(&rsp, &sim_ev[sim_next]);
            if (responding) adjust(&sim_ev[sim_next], bars_up);
        }

        /* the bars' flip record when it arrives; confirmed: the bars go
         * off on this frame */
        if (yrsp_update_frame(&rsp, &f) == YRSP_ENDED && responding) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, BARS_ON);
            ytl_wait(&q, YRT_S(ITI_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "trial_adjustment: timeline error %d\n", q.err); failed = 1; break; }
            responding = bars_up = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        yrsp_set_landing(&rsp, fired, n, BARS_ON);   /* the bars landed: their planned onset */
        for (i = 0; i < n; i++) {
            if (fired[i].kind == YTL_ONSET && fired[i].target == BARS_ON) {
                bars_up = 1;
                if (sim) {   /* the participant's presses, from this onset */
                    int k, j;
                    for (k = 0; k < (int)(sizeof sim_script / sizeof sim_script[0]); k++) {
                        const sim_burst* b = &sim_script[k];
                        char key[2] = { b->key, 0 };
                        if (b->trial != ti.index) continue;
                        for (j = 0; j < b->n && n_sim_ev + 4 <= 64; j++) {
                            yrsp_input* e = &sim_ev[n_sim_ev++];
                            memset(e, 0, sizeof *e);
                            e->t = fired[i].onset + YRT_S(b->at + j * b->every);
                            e->type = YRSP_PRESS;
                            e->control = (uint32_t)yrsp_scancode(key);
                            e->flags = (uint8_t)(b->repeat && j > 0 ? YRSP_IN_REPEAT : 0);
                            e->stamp = YRSP_TIER_SIM;
                            e->value = 1;
                            if (!b->repeat || j == b->n - 1) {
                                sim_ev[n_sim_ev] = *e;
                                sim_ev[n_sim_ev].type = YRSP_RELEASE;
                                sim_ev[n_sim_ev].flags = 0;
                                sim_ev[n_sim_ev].value = 0;
                                sim_ev[n_sim_ev++].t = e->t + YRT_MS(50);
                            }
                            if (b->twice) {   /* the message path's report of the tap */
                                sim_ev[n_sim_ev] = sim_ev[n_sim_ev - 2];
                                sim_ev[n_sim_ev + 1] = sim_ev[n_sim_ev - 1];
                                sim_ev[n_sim_ev].t += YRT_MS(9);
                                sim_ev[n_sim_ev + 1].t += YRT_MS(9);
                                n_sim_ev += 2;
                            }
                        }
                    }
                    if (ti.index < SIM_TRIALS) sim_plan_onset[ti.index] = fired[i].onset;
                    if (ti.index < SIM_TRIALS && n_sim_ev + 2 <= 64) {
                        /* without its release, space stays down and the next
                         * trial's press is a held key, not a response */
                        yrsp_input* e = &sim_ev[n_sim_ev];
                        memset(e, 0, sizeof *e);
                        e->t = fired[i].onset + YRT_S(sim_confirm_at[ti.index]);
                        e->type = YRSP_PRESS;
                        e->control = (uint32_t)yrsp_scancode("space");
                        e->stamp = YRSP_TIER_SIM;
                        e->value = 1;
                        sim_ev[n_sim_ev + 1] = *e;
                        sim_ev[n_sim_ev + 1].type = YRSP_RELEASE;
                        sim_ev[n_sim_ev + 1].value = 0;
                        sim_ev[n_sim_ev + 1].t = e->t + YRT_MS(70);
                        n_sim_ev += 2;
                    }
                    /* fed in time order below: a burst can end after the next one starts */
                    for (k = 1; k < n_sim_ev; k++)
                        for (j = k; j > 0 && sim_ev[j - 1].t > sim_ev[j].t; j--) {
                            yrsp_input tmp = sim_ev[j];
                            sim_ev[j] = sim_ev[j - 1];
                            sim_ev[j - 1] = tmp;
                        }
                }
            }
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                char rt[32];
                int ref = atoi(ytb_text(&tab, ti.condition, ref_col)), k;
                yrsp_finish(&rsp, &res);
                if (res.rt_ns != YRSP_NO_TIME) snprintf(rt, sizeof rt, "%.9f", YRT_TO_S(res.rt_ns));
                else rt[0] = 0;
                fprintf(out, "%d,%d,%.2f,%.2f,%.1f,%d,%s,%u,%u,%u,%u\n", ti.index, ref, start_value, value,
                        bar_length(value) - ref, n_steps, rt, (unsigned)res.onset_tier, (unsigned)res.onset_src,
                        (unsigned)res.stamp_tier, (unsigned)res.flags);
                for (k = 0; k < n_steps; k++)
                    fprintf(traj, "%d,%.9f,%c,%d,%.2f\n", ti.index, (double)(steps[k].t - res.t_onset) / 1e9,
                            steps[k].key, steps[k].repeat, steps[k].value);
                fflush(out);
                fflush(traj);
                printf("trial %2d reference %d px: %.2f -> %.2f (error %+.1f px) in %d steps, rt %s%s\n", ti.index, ref,
                       start_value, value, bar_length(value) - ref, n_steps, rt,
                       n_lost_steps ? " (steps lost: MAX_STEPS)" : "");
                ytr_update(&trials, 1, NULL);
                if (ti.index < SIM_TRIALS) {
                    sim_res[ti.index] = res;
                    sim_steps[ti.index] = n_steps;
                    sim_start[ti.index] = start_value;
                    sim_final[ti.index] = value;
                }
                rows++;
                in_trial = 0;
            }
        }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        comparison.w = bar_length(value);
        reference.w = in_trial ? (float)atof(ytb_text(&tab, ti.condition, ref_col)) : 0;
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &fix);
        ygfx_draw(&gfx, &reference);
        ygfx_draw(&gfx, &comparison);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    fclose(out);
    fclose(traj);
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d trials written to %s, steps to %s%s\n", rows, out_path, traj_path,
           aborted ? " (stopped with Shift+Esc)" : "");

    if (sim && !failed && reps != 1) printf("--sim: the self-check needs the default --reps\n");
    else if (sim && !failed) {
        /* net steps per trial: +4 - 1, -15, +13 */
        static const int want_steps[SIM_TRIALS] = { 5, 15, 13 }, want_net[SIM_TRIALS] = { 3, -15, 13 };
        int bad = rows != SIM_TRIALS;
        if (bad) fprintf(stderr, "trial_adjustment: --sim: %d rows, not %d\n", rows, SIM_TRIALS);
        for (i = 0; i < rows && i < SIM_TRIALS; i++) {
            const yrsp_result* r = &sim_res[i];
            int ok = sim_steps[i] == want_steps[i] && fabs(sim_final[i] - sim_start[i] - want_net[i] * STEP) < 1e-9 &&
                     (r->flags & YRSP_R_RESPONDED) &&
                     /* on the virtual clock the onset is the plan and the RT
                      * the script's, to the nanosecond */
                     r->t_onset == sim_plan_onset[i] && r->rt_ns == YRT_S(sim_confirm_at[i]) &&
                     r->onset_tier == YSCR_TIER_SIM && r->onset_src == YRSP_ONSET_FLIP;
            if (!ok) {
                fprintf(stderr, "trial_adjustment: --sim: trial %d: %d steps, %.2f -> %.2f, rt %.9f flags 0x%x\n", i,
                        sim_steps[i], sim_start[i], sim_final[i], YRT_TO_S(r->rt_ns), (unsigned)r->flags);
                bad = 1;
            }
        }
        if (bad) return 1;
        printf("--sim: every trial came out as scripted\n");
    }
    return failed;
}
