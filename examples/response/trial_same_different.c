/* trial_same_different.c - the jsPsych same-different-html (and -image) trial as
 * a small experiment: ysp/trials.h, ysp/timeline.h, ysp/gfx.h and
 * ysp/response.h.
 *
 * Each trial: a fixation cross for 0.5 s, a bar for 0.3 s
 * (first_stim_duration), a blank gap of 0.5 s (gap_duration), a second bar
 * for 0.3 s (second_stim_duration). Q if the two bars have the same
 * orientation, P if not (jsPsych's same_key and different_key, as
 * physical keys). The response window is 2 s from the second bar's onset;
 * responses before 0.1 s are anticipations. Presses from the fixation on
 * are logged: a press before the second bar is EARLY, never a response.
 * A blank inter-trial interval of 1 s follows the response.
 *
 * The two onsets come from flip records, so the data row has the stimulus
 * onset asynchrony as shown (soa: second onset minus first onset) beside
 * the planned 0.8 s, and both onsets' tiers. RT runs from the second
 * bar's flip onset.
 *
 * The data file has one row per trial: trial_index, first, second, answer
 * (jsPsych's names), response, rt, correct; then first_tier, soa,
 * onset_tier, onset_src, rsp_tier, rsp_flags, n_early, n_anticipations,
 * n_duplicates (second key reports that ysp/screen.h's input bridge
 * dropped during the trial). Times in seconds. Shift+Esc stops the session; the rows
 * so far are kept.
 *
 * --sim runs on the simulated display with a synthetic participant: a
 * correct response, a press during the first bar and then a response, a
 * timeout, and a wrong key. It checks its own results and exits 1 on a
 * mismatch. About 13 s.
 *
 * Usage: trial_same_different [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE]
 *   --reps N   repetitions of each of the 4 conditions (default 5; --sim: 1)
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

enum { FIX_ON, FIRST_ON, SECOND_ON, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };

#define FIXATION_S  0.5
#define FIRST_S     0.3          /* first_stim_duration */
#define GAP_S       0.5          /* gap_duration */
#define SECOND_S    0.3          /* second_stim_duration */
#define RESPONSE_S  2.0
#define MIN_RT_S    0.1
#define ITI_S       1.0

/* orientations in degrees; answer is the key */
static const char conditions_csv[] =
    "first,second,answer\n"
    "45,45,q\n"
    "45,135,p\n"
    "135,135,q\n"
    "135,45,p\n";

static const yrsp_choice keys[] = { { .key = "q", .name = "same" }, { .key = "p", .name = "different" } };

/* The synthetic participant of --sim: presses at `at` s from the second
 * bar's planned onset (negative: before it), held 0.08 s. which: 1 the
 * correct key, 2 the other one. */
typedef struct sim_press { int trial; double at; int which; } sim_press;
static const sim_press sim_script[] = {
    { 0, 0.550, 1 },
    { 1, -0.900, 1 },   /* during the first bar: EARLY */
    { 1, 0.480, 1 },
    /* trial 2: no press, a timeout */
    { 3, 0.620, 2 },
};
#define SIM_TRIALS 4

static yscr_screen scr;
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static yrsp_collector rsp;
static uint64_t table_arena[2048];
static ytb_table tab;

static ygfx_stim fix, first, second;
static const ygfx_bind binds[] = {
    { .stim = &fix,    .param = YGFX_P_VISIBLE, .channel = FIX_ON    },
    { .stim = &first,  .param = YGFX_P_VISIBLE, .channel = FIRST_ON  },
    { .stim = &second, .param = YGFX_P_VISIBLE, .channel = SECOND_ON },
};

/* The bridge's count of dropped second key reports (a process total). */
static uint32_t key_doubles(void) {
    yscr_input_stats st;
    yscr_get_input_stats(&st);
    return st.key_doubles;
}

static ygfx_stim shape(ygfx_shape_kind kind, float w, float h, float p0) {
    ygfx_shape_desc d;
    memset(&d, 0, sizeof d);
    d.shape = kind;
    d.w = w;
    d.h = h;
    d.shape_p[0] = p0;
    d.edge = YGFX_EDGE_COSINE;
    d.edge_width = 1;
    d.color[0] = d.color[1] = d.color[2] = 0.9f;
    return ygfx_shape(&d);
}

static void sec(char* out, size_t cap, double s) {
    if (s != s) out[0] = 0;
    else snprintf(out, cap, "%.9f", s);
}

/* --sim's check: x equals want plus a whole number of display periods,
 * at most 6 (frames a loaded host dropped). */
static int whole_frames(double x, double want, int64_t period_ns) {
    double p = (double)period_ns / 1e9, d = (x - want) / p;
    double m = floor(d + 0.5);
    return p > 0 && fabs(m) <= 6 && fabs(x - (want + m * p)) < 1e-6;
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    ytl_desc td;
    ytb_csv_desc cd;
    ytr_desc rd;
    yrsp_desc pd;
    yscr_frame f;
    ytl_event fired[16];
    ytr_trial_info ti = { 0 };
    yrsp_result res, sim_res[SIM_TRIALS];
    uint32_t doubles_at_arm = 0;
    yrsp_onset first_onset;
    yrsp_input sim_ev[8];
    double sim_soa[SIM_TRIALS];
    int sim_correct[SIM_TRIALS];
    FILE* out;
    char line[1024], meta[2048];
    const char* out_path = "trial_same_different.csv";
    uint64_t seed = 20261008;
    int i, n, sim = 0, reps = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0;
    int in_trial = 0, responding = 0, n_sim_ev = 0, sim_next = 0;
    int first_col, second_col, answer_col;
    int64_t first_frame = -1, second_frame = -1;
    char answer[8] = "";

    memset(&sd, 0, sizeof sd);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_same_different [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE]\n");
            return 2;
        }
    }
    if (reps == 0) reps = sim ? 1 : 5;
    if (reps < 1 || reps > 100) { fprintf(stderr, "trial_same_different: --reps takes 1..100\n"); return 2; }
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = !fullscreen;

    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "trial_same_different: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "trial_same_different: %s\n", ygfx_error(&gfx)); return 1; }
    fix = shape(YGFX_CROSS, 16, 16, 3);
    fix.color[0] = fix.color[1] = fix.color[2] = 0.2f;
    first = shape(YGFX_RECT, 160, 16, 0);
    second = shape(YGFX_RECT, 160, 16, 0);

    memset(&cd, 0, sizeof cd);
    cd.text = conditions_csv;
    cd.len = sizeof conditions_csv - 1;
    cd.arena = table_arena;
    cd.arena_size = sizeof table_arena;
    if (!ytb_open_csv(&tab, &cd)) { fprintf(stderr, "trial_same_different: %s\n", ytb_error(&tab)); return 1; }
    first_col = ytb_col(&tab, "first");
    second_col = ytb_col(&tab, "second");
    answer_col = ytb_col(&tab, "answer");
    memset(&rd, 0, sizeof rd);
    rd.table = &tab;
    rd.reps = reps;
    rd.order = YTR_ORDER_RANDOM;
    rd.rng = ytr_splitmix;
    rd.rng_ctx = &seed;
    if (!ytr_open(&trials, &rd)) { fprintf(stderr, "trial_same_different: %s\n", ytr_error(&trials)); return 1; }

    memset(&td, 0, sizeof td);
    td.events = storage;
    td.event_capacity = 64;
    td.n_channels = N_CHANNELS;
    if (!ytl_open(&tl, &td)) { fprintf(stderr, "trial_same_different: %s\n", ytl_error(&tl)); return 1; }

    /* q or p, within 2 s of the second bar, none before 0.1 s */
    memset(&pd, 0, sizeof pd);
    pd.choices = keys;
    pd.n_choices = 2;
    pd.duration_ns = YRT_S(RESPONSE_S);
    pd.min_rt_ns = YRT_S(MIN_RT_S);
    if (!yrsp_open(&rsp, &pd)) { fprintf(stderr, "trial_same_different: %s\n", yrsp_error(&rsp)); return 1; }

    out = fopen(out_path, "w");
    if (!out) { fprintf(stderr, "trial_same_different: cannot write %s\n", out_path); return 1; }
    ytr_format_meta(&trials, meta, sizeof meta);
    meta[strcspn(meta, "\r\n")] = 0;
    yscr_describe(&scr, line, sizeof line);
    fprintf(out, "# %s\n# %s\n# ysp_response %s; keyboard: %s\n", meta, line, yrsp_version(),
            sim ? "synthetic (tier SIM)" : "SDL through yscr_event_input");
    fprintf(out, "trial_index,first,second,answer,response,rt,correct,first_tier,soa,onset_tier,onset_src,"
                 "rsp_tier,rsp_flags,n_early,n_anticipations,n_duplicates\n");
    printf("%s\n", line);
    memset(&first_onset, 0, sizeof first_onset);

    for (;;) {
        SDL_Event ev;
        int64_t bt = 0;
        yrsp_input in;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_same_different: %s\n", yscr_error(&scr)); failed = 1; break; }

        /* the two bars' flip records: their onsets, final */
        for (i = 0; i < f.n_done; i++) {
            if (f.done[i].index == first_frame) first_onset = yrsp_onset_flip(&f.done[i]);
            if (f.done[i].index == second_frame) {
                yrsp_onset o = yrsp_onset_flip(&f.done[i]);
                yrsp_set_onset(&rsp, &o);
            }
        }

        if (!in_trial) {
            ytl_seq q;
            const char* a;
            if (ytr_next(&trials, &ti) < 0) break;
            a = ytb_text(&tab, ti.condition, answer_col);
            snprintf(answer, sizeof answer, "%s", a ? a : "");
            first.ori = (float)atof(ytb_text(&tab, ti.condition, first_col));
            second.ori = (float)atof(ytb_text(&tab, ti.condition, second_col));
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            ytl_on(&q, FIX_ON);
            ytl_wait(&q, YRT_S(FIXATION_S));
            ytl_off(&q, FIX_ON);
            ytl_on(&q, FIRST_ON);
            ytl_wait(&q, YRT_S(FIRST_S));
            ytl_off(&q, FIRST_ON);
            ytl_wait(&q, YRT_S(GAP_S));
            ytl_on(&q, SECOND_ON);
            ytl_wait(&q, YRT_S(SECOND_S));
            ytl_off(&q, SECOND_ON);
            if (q.err) { fprintf(stderr, "trial_same_different: timeline error %d\n", q.err); failed = 1; break; }
            yrsp_arm(&rsp, f.onset);
            doubles_at_arm = key_doubles();
            first_frame = second_frame = -1;
            memset(&first_onset, 0, sizeof first_onset);
            in_trial = responding = 1;
            n_sim_ev = sim_next = 0;
            if (sim) {   /* the participant's presses, from the second bar's planned onset */
                int64_t t2 = f.onset + YRT_S(FIXATION_S + FIRST_S + GAP_S);
                int k;
                for (k = 0; k < (int)(sizeof sim_script / sizeof sim_script[0]); k++) {
                    const sim_press* p = &sim_script[k];
                    const char* key = p->which == 1 ? answer : !strcmp(answer, "q") ? "p" : "q";
                    yrsp_input* e = &sim_ev[n_sim_ev];
                    if (p->trial != ti.index || n_sim_ev + 2 > 8) continue;
                    memset(e, 0, sizeof *e);
                    e->t = t2 + YRT_S(p->at);
                    e->type = YRSP_PRESS;
                    e->control = (uint32_t)yrsp_scancode(key);
                    e->stamp = YRSP_TIER_SIM;
                    e->value = 1;
                    sim_ev[n_sim_ev + 1] = *e;
                    sim_ev[n_sim_ev + 1].type = YRSP_RELEASE;
                    sim_ev[n_sim_ev + 1].value = 0;
                    sim_ev[n_sim_ev + 1].t = e->t + YRT_MS(80);
                    n_sim_ev += 2;
                }
            }
        }

        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) yrsp_feed(&rsp, &in);
        while (sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset) yrsp_feed(&rsp, &sim_ev[sim_next++]);

        if (responding && yrsp_update(&rsp, f.onset) == YRSP_ENDED) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, SECOND_ON);
            ytl_wait(&q, YRT_S(ITI_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "trial_same_different: timeline error %d\n", q.err); failed = 1; break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 16);
        for (i = 0; i < n && i < 16; i++) {
            if (fired[i].kind == YTL_ONSET && fired[i].target == FIRST_ON) {
                first_frame = fired[i].frame;
                first_onset = yrsp_onset_landing(&fired[i]);
            }
            if (fired[i].kind == YTL_ONSET && fired[i].target == SECOND_ON) {
                yrsp_onset o = yrsp_onset_landing(&fired[i]);
                yrsp_set_onset(&rsp, &o);
                second_frame = fired[i].frame;
            }
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                char rt[32], soa[32];
                int correct;
                yrsp_finish(&rsp, &res);
                correct = (res.flags & YRSP_R_RESPONDED) && !strcmp(keys[res.response].key, answer);
                sec(rt, sizeof rt, res.rt_ns == YRSP_NO_TIME ? NAN : YRT_TO_S(res.rt_ns));
                sec(soa, sizeof soa, res.t_onset && first_onset.t_ns ? (double)(res.t_onset - first_onset.t_ns) / 1e9 : NAN);
                fprintf(out, "%d,%s,%s,%s,%s,%s,%d,%u,%s,%u,%u,%u,%u,%d,%d,%d\n", ti.index,
                        ytb_text(&tab, ti.condition, first_col), ytb_text(&tab, ti.condition, second_col), answer,
                        res.response_name ? res.response_name : "", rt, correct, (unsigned)first_onset.tier, soa,
                        (unsigned)res.onset_tier, (unsigned)res.onset_src, (unsigned)res.stamp_tier,
                        (unsigned)res.flags, res.n_early, res.n_anticipations,
                        (int)(key_doubles() - doubles_at_arm));
                fflush(out);
                printf("trial %2d %3s %3s soa %s rt %s response %s %s\n", ti.index,
                       ytb_text(&tab, ti.condition, first_col), ytb_text(&tab, ti.condition, second_col), soa,
                       rt[0] ? rt : "-", res.response_name ? res.response_name : "-",
                       !(res.flags & YRSP_R_RESPONDED) ? "(too slow)" : correct ? "correct" : "wrong");
                ytr_update(&trials, (res.flags & YRSP_R_RESPONDED) ? correct : YTR_INVALID, NULL);
                if (ti.index < SIM_TRIALS) {
                    sim_res[ti.index] = res;
                    sim_soa[ti.index] = atof(soa);
                    sim_correct[ti.index] = correct;
                }
                rows++;
                in_trial = 0;
            }
        }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &fix);
        ygfx_draw(&gfx, &first);
        ygfx_draw(&gfx, &second);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    fclose(out);
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");

    if (sim && !failed && reps != 1) printf("--sim: the self-check needs the default --reps\n");
    else if (sim && !failed) {
        int bad = rows != SIM_TRIALS;
        if (bad) fprintf(stderr, "trial_same_different: --sim: %d rows, not %d\n", rows, SIM_TRIALS);
        for (i = 0; i < rows && i < SIM_TRIALS; i++) {
            const yrsp_result* r = &sim_res[i];
            static const double want_rt[SIM_TRIALS] = { 0.55, 0.48, 0, 0.62 };
            /* The simulated display runs on the host's clock, so a loaded
             * host (macOS CI) drops frames like a real display: the SOA and
             * the RT (from the actual onset) then differ from the script by
             * a whole number of periods. A logic error does not. */
            int ok = whole_frames(sim_soa[i], FIRST_S + GAP_S, f.period);
            if (i == 2) ok = ok && !(r->flags & YRSP_R_RESPONDED) && (r->flags & YRSP_R_TIMEOUT);
            else ok = ok && (r->flags & YRSP_R_RESPONDED) && whole_frames(YRT_TO_S(r->rt_ns), want_rt[i], f.period);
            if (i == 1) ok = ok && r->n_early == 1;
            ok = ok && sim_correct[i] == (i < 2);
            ok = ok && r->onset_tier == YSCR_TIER_SIM && r->onset_src == YRSP_ONSET_FLIP;
            if (!ok) {
                fprintf(stderr, "trial_same_different: --sim: trial %d: rt %.9f soa %.9f flags 0x%x n_early %d\n", i, YRT_TO_S(r->rt_ns),
                        sim_soa[i], (unsigned)r->flags, r->n_early);
                bad = 1;
            }
        }
        if (bad) return 1;
        printf("--sim: every trial came out as scripted\n");
    }
    return failed;
}
