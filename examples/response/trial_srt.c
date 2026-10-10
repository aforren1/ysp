/* trial_srt.c - the jsPsych serial-reaction-time trial as a small
 * experiment: ysp/trials.h, ysp/timeline.h, ysp/gfx.h and ysp/response.h.
 *
 * Four outlined squares in a row. One fills; press the key under it (D,
 * F, J, K: physical keys). The first key of the four ends the trial,
 * right or wrong, as in jsPsych; there is no deadline. The target goes
 * off at the response, and the next target comes 0.25 s later (the
 * response-to-stimulus interval, RSI). Keys pressed during the RSI are
 * logged and counted (n_rsi_presses), never a response.
 *
 * Blocks: S plays a 12-trial second-order conditional sequence (1 2 1 4 3
 * 2 4 1 3 4 2 3: each of the 12 ordered pairs once, so one position does
 * not predict the next, two do) --reps times; R plays the same positions,
 * as many of each, in a random order with no immediate repeat
 * (ysp/trials.h, ORDER_CONSTRAINED). The default order is S R S: learning
 * shows as slower responses in R. A 2 s rest precedes each block.
 *
 * The data file has one row per trial: block, block_type, trial_index,
 * position, response, rt, correct; then onset_tier, onset_src, rsp_tier,
 * rsp_flags, n_rsi_presses, n_duplicates (second key reports that
 * ysp/screen.h's input bridge dropped during the trial). Times in
 * seconds; RT from the target's flip onset. A summary of the mean correct
 * RT per block ends the run. Shift+Esc stops the session; the rows so
 * far are kept.
 *
 * --sim runs blocks S and R once each on the simulated display with a
 * synthetic participant: 0.30 s in S, 0.45 s in R, one wrong key in R, one
 * extra press during an RSI. It checks its own results and exits 1 on a
 * mismatch. About 20 s.
 *
 * Usage: trial_srt [--sim] [--fullscreen] [--reps N] [--blocks SRS] [--seed N] [--out FILE]
 *   --reps N    passes of the sequence per block (default 4; --sim: 1)
 *   --blocks B  block types in order, S or R, up to 8 (default SRS; --sim: SR)
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

enum { TARGET_ON, TARGET_X, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };

#define RSI_S      0.25
#define REST_S     2.0
#define SPACING    150.0f        /* px between the squares' centers */
#define SEQ_LEN    12
#define MAX_REPS   20

static const int soc_sequence[SEQ_LEN] = { 0, 1, 0, 3, 2, 1, 3, 0, 2, 3, 1, 2 };
static const char conditions_csv[] =
    "position,key\n"
    "0,d\n"
    "1,f\n"
    "2,j\n"
    "3,k\n";
static const yrsp_choice keys[] = { { .key = "d" }, { .key = "f" }, { .key = "j" }, { .key = "k" } };

/* The synthetic participant of --sim: the correct key 0.30 s after the
 * onset in S, 0.45 s in R; trial 2 of R the key to the right, at 0.40 s;
 * trial 4 of S a second press 0.10 s after the response, in the RSI. */
#define SIM_RT_S      0.30
#define SIM_RT_R      0.45
#define SIM_WRONG_RT  0.40
#define SIM_TRIALS    (2 * SEQ_LEN)

static yscr_screen scr;
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static yrsp_collector rsp;
static uint64_t table_arena[2048];
static ytb_table tab;
static int order_list[SEQ_LEN * MAX_REPS];

static ygfx_stim boxes[4], target;
static const ygfx_bind binds[] = {
    { .stim = &target, .param = YGFX_P_VISIBLE, .channel = TARGET_ON },
    { .stim = &target, .param = YGFX_P_X,       .channel = TARGET_X  },
};

/* The bridge's count of dropped second key reports (a process total). */
static uint32_t key_doubles(void) {
    yscr_input_stats st;
    yscr_get_input_stats(&st);
    return st.key_doubles;
}

static ygfx_stim square(float x, float stroke) {
    ygfx_shape_desc d;
    memset(&d, 0, sizeof d);
    d.shape = YGFX_RECT;
    d.x = x;
    d.w = d.h = 100;
    d.stroke = stroke;
    d.edge = YGFX_EDGE_COSINE;
    d.edge_width = 1;
    d.color[0] = d.color[1] = d.color[2] = 0.9f;
    return ygfx_shape(&d);
}

/* One block's order: the sequence, or its positions shuffled with no
 * immediate repeat. */
static int open_block(char type, int reps, uint64_t* seed) {
    ytr_desc rd;
    int i;
    memset(&rd, 0, sizeof rd);
    memset(&trials, 0, sizeof trials);
    rd.table = &tab;
    rd.rng = ytr_splitmix;
    rd.rng_ctx = seed;
    if (type == 'S') {
        for (i = 0; i < SEQ_LEN * reps; i++) order_list[i] = soc_sequence[i % SEQ_LEN];
        rd.order = YTR_ORDER_LIST;
        rd.order_list = order_list;
        rd.n_order_list = SEQ_LEN * reps;
    } else {
        rd.reps = 3 * reps;      /* each position 3 times per pass, as in the sequence */
        rd.order = YTR_ORDER_CONSTRAINED;
        rd.constraints[0] = ytr_max_run(YTR_CONDITION, YTR_ANY_LEVEL, 1);
        rd.n_constraints = 1;
    }
    if (!ytr_open(&trials, &rd)) { fprintf(stderr, "trial_srt: %s\n", ytr_error(&trials)); return 0; }
    return 1;
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    ytl_desc td;
    ytb_csv_desc cd;
    yrsp_desc pd;
    yscr_frame f;
    ytl_event fired[16];
    ytr_trial_info ti = { 0 };
    yrsp_result res, sim_res[SIM_TRIALS];
    uint32_t doubles_at_arm = 0;
    yrsp_input sim_ev[8];
    FILE* out;
    char line[1024];
    const char* out_path = "trial_srt.csv";
    const char* blocks = NULL;
    uint64_t seed = 20261008;
    int i, n, sim = 0, reps = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0;
    int in_trial = 0, responding = 0, n_sim_ev = 0, sim_next = 0, block = -1, trial_index = 0, position = 0;
    int64_t target_frame = -1;
    int pos_col, n_correct[8] = { 0 }, sim_correct[SIM_TRIALS];
    double sum_rt[8] = { 0 };

    memset(&sd, 0, sizeof sd);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--blocks") && i + 1 < argc) blocks = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_srt [--sim] [--fullscreen] [--reps N] [--blocks SRS] [--seed N] [--out FILE]\n");
            return 2;
        }
    }
    if (reps == 0) reps = sim ? 1 : 4;
    if (!blocks) blocks = sim ? "SR" : "SRS";
    if (reps < 1 || reps > MAX_REPS) { fprintf(stderr, "trial_srt: --reps takes 1..%d\n", MAX_REPS); return 2; }
    if (!blocks[0] || strlen(blocks) > 8 || strspn(blocks, "SR") != strlen(blocks)) {
        fprintf(stderr, "trial_srt: --blocks takes 1 to 8 of S and R\n");
        return 2;
    }
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = !fullscreen;

    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "trial_srt: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "trial_srt: %s\n", ygfx_error(&gfx)); return 1; }
    for (i = 0; i < 4; i++) boxes[i] = square(((float)i - 1.5f) * SPACING, 4);
    target = square(0, 0);

    memset(&cd, 0, sizeof cd);
    cd.text = conditions_csv;
    cd.len = sizeof conditions_csv - 1;
    cd.arena = table_arena;
    cd.arena_size = sizeof table_arena;
    if (!ytb_open_csv(&tab, &cd)) { fprintf(stderr, "trial_srt: %s\n", ytb_error(&tab)); return 1; }
    pos_col = ytb_col(&tab, "position");

    memset(&td, 0, sizeof td);
    td.events = storage;
    td.event_capacity = 64;
    td.n_channels = N_CHANNELS;
    if (!ytl_open(&tl, &td)) { fprintf(stderr, "trial_srt: %s\n", ytl_error(&tl)); return 1; }

    /* one of four keys, no deadline, the first key ends it */
    memset(&pd, 0, sizeof pd);
    pd.choices = keys;
    pd.n_choices = 4;
    if (!yrsp_init(&rsp, &pd)) { fprintf(stderr, "trial_srt: %s\n", yrsp_error(&rsp)); return 1; }

    out = fopen(out_path, "w");
    if (!out) { fprintf(stderr, "trial_srt: cannot write %s\n", out_path); return 1; }
    yscr_describe(&scr, line, sizeof line);
    fprintf(out, "# blocks %s, reps %d, seed %llu, rsi %.3f s\n# %s\n# ysp_response %s; keyboard: %s\n", blocks, reps,
            (unsigned long long)seed, RSI_S, line, yrsp_version(), sim ? "synthetic (tier SIM)" : "SDL through yscr_event_input");
    fprintf(out, "block,block_type,trial_index,position,response,rt,correct,onset_tier,onset_src,rsp_tier,rsp_flags,"
                 "n_rsi_presses,n_duplicates\n");
    printf("%s\n", line);

    for (;;) {
        SDL_Event ev;
        int64_t bt = 0;
        yrsp_input in;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_srt: %s\n", yscr_error(&scr)); failed = 1; break; }

        /* the target's flip record: its onset, final */
        for (i = 0; i < f.n_done; i++)
            if (f.done[i].index == target_frame) {
                yrsp_onset o = yrsp_onset_flip(&f.done[i]);
                yrsp_set_onset(&rsp, &o);
            }

        if (!in_trial) {
            /* the next target now, or after a rest at a block's start */
            ytl_seq q;
            int rest = 0;
            if (block < 0 || ytr_next(&trials, &ti) < 0) {
                if (!blocks[++block] || !open_block(blocks[block], reps, &seed)) break;
                ytr_next(&trials, &ti);
                rest = 1;
            }
            position = atoi(ytb_text(&tab, ti.condition, pos_col));
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            ytl_wait(&q, rest ? YTL_S(REST_S) : 0);
            ytl_set_value(&q, TARGET_X, ((float)position - 1.5f) * SPACING);
            ytl_on(&q, TARGET_ON);
            if (q.err) { fprintf(stderr, "trial_srt: timeline error %d\n", q.err); failed = 1; break; }
            yrsp_arm(&rsp, f.onset);
            doubles_at_arm = key_doubles();
            target_frame = -1;
            in_trial = responding = 1;
            n_sim_ev = sim_next = 0;
        }

        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) yrsp_feed(&rsp, &in);
        while (sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset) yrsp_feed(&rsp, &sim_ev[sim_next++]);

        /* a key: the target goes off on this frame, the next comes after the RSI */
        if (responding && yrsp_update(&rsp, f.onset) == YRSP_ENDED) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, TARGET_ON);
            ytl_wait(&q, YTL_S(RSI_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "trial_srt: timeline error %d\n", q.err); failed = 1; break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 16);
        for (i = 0; i < n && i < 16; i++) {
            if (fired[i].kind == YTL_ONSET && fired[i].target == TARGET_ON) {
                yrsp_onset o = yrsp_onset_landing(&fired[i]);
                yrsp_set_onset(&rsp, &o);
                target_frame = fired[i].frame;
                if (sim) {   /* the participant's presses, from this onset */
                    int k = blocks[block] == 'S' ? ti.index == 4 : ti.index == 2;
                    int wrong = blocks[block] == 'R' && k;
                    double at = wrong ? SIM_WRONG_RT : blocks[block] == 'S' ? SIM_RT_S : SIM_RT_R;
                    int p;
                    for (p = 0; p < 1 + (blocks[block] == 'S' && k); p++) {
                        yrsp_input* e = &sim_ev[n_sim_ev];
                        memset(e, 0, sizeof *e);
                        e->t = fired[i].onset + YTL_S(at + 0.1 * p);
                        e->type = YRSP_PRESS;
                        e->control = (uint32_t)yrsp_scancode(keys[(position + wrong) % 4].key);
                        e->stamp = YRSP_TIER_SIM;
                        e->value = 1;
                        sim_ev[n_sim_ev + 1] = *e;
                        sim_ev[n_sim_ev + 1].type = YRSP_RELEASE;
                        sim_ev[n_sim_ev + 1].value = 0;
                        sim_ev[n_sim_ev + 1].t = e->t + YTL_MS(60);
                        n_sim_ev += 2;
                    }
                }
            }
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                char rt[32];
                int correct;
                yrsp_finish(&rsp, &res);
                correct = (res.flags & YRSP_R_RESPONDED) && res.response == position;
                if (res.rt == res.rt) snprintf(rt, sizeof rt, "%.9f", res.rt);
                else rt[0] = 0;
                fprintf(out, "%d,%s,%d,%d,%s,%s,%d,%u,%u,%u,%u,%d,%d\n", block,
                        blocks[block] == 'S' ? "sequence" : "random", trial_index, position,
                        res.response_name ? res.response_name : "", rt, correct, (unsigned)res.onset_tier,
                        (unsigned)res.onset_src, (unsigned)res.stamp_tier, (unsigned)res.flags, res.n_after_end,
                        (int)(key_doubles() - doubles_at_arm));
                fflush(out);
                if (correct) { sum_rt[block] += res.rt; n_correct[block]++; }
                ytr_update(&trials, correct, NULL);
                if (trial_index < SIM_TRIALS) { sim_res[trial_index] = res; sim_correct[trial_index] = correct; }
                rows++;
                trial_index++;
                in_trial = 0;
            }
        }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw_n(&gfx, boxes, 4);
        ygfx_draw(&gfx, &target);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    fclose(out);
    ygfx_close(&gfx);
    yscr_close(&scr);
    for (i = 0; i <= block && blocks[i]; i++)
        printf("block %d (%s): %d correct, mean rt %.3f s\n", i, blocks[i] == 'S' ? "sequence" : "random",
               n_correct[i], n_correct[i] ? sum_rt[i] / n_correct[i] : NAN);
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");

    if (sim && !failed && (reps != 1 || strcmp(blocks, "SR"))) printf("--sim: the self-check needs the default --reps and --blocks\n");
    else if (sim && !failed) {
        int bad = rows != SIM_TRIALS;
        if (bad) fprintf(stderr, "trial_srt: --sim: %d rows, not %d\n", rows, SIM_TRIALS);
        for (i = 0; i < rows && i < SIM_TRIALS; i++) {
            const yrsp_result* r = &sim_res[i];
            int s = i < SEQ_LEN, k = i % SEQ_LEN;
            double want = s ? SIM_RT_S : k == 2 ? SIM_WRONG_RT : SIM_RT_R;
            int ok = (r->flags & YRSP_R_RESPONDED) && fabs(r->rt - want) < 1e-6 &&
                     sim_correct[i] == (s || k != 2) && r->n_after_end == (s && k == 4) &&
                     r->onset_tier == YSCR_TIER_SIM && r->onset_src == YRSP_ONSET_FLIP;
            if (!ok) {
                fprintf(stderr, "trial_srt: --sim: trial %d: rt %.9f correct %d flags 0x%x n_after_end %d\n", i, r->rt,
                        sim_correct[i], (unsigned)r->flags, r->n_after_end);
                bad = 1;
            }
        }
        if (bad) return 1;
        printf("--sim: every trial came out as scripted\n");
    }
    return failed;
}
