/* trial_srt.c - the jsPsych serial-reaction-time trial as a small
 * experiment: ysp/trials.h, ysp/timeline.h, ysp/gfx.h and ysp/response.h.
 *
 * Four outlined squares in a row. One fills; press the key under it (D, F,
 * J, K: physical keys). The first of the four keys ends the trial, right or
 * wrong; there is no deadline. The target goes off at the response, and
 * the next comes 0.25 s later (the RSI). Keys in the RSI are counted
 * (n_rsi_presses), never a response.
 *
 * Blocks: S plays a 12-trial second-order conditional sequence (1 2 1 4 3
 * 2 4 1 3 4 2 3: one position does not predict the next, two do) --reps
 * times; R plays the same positions in a random order with no immediate
 * repeat. Default S R S, each after a 2 s rest: learning shows as slower
 * responses in R.
 *
 * Data, one row per trial: block, block_type, trial_index, position,
 * response, rt_s (from the target's flip onset), correct, onset_tier,
 * onset_src, rsp_tier, rsp_flags, n_rsi_presses, n_duplicates (second key
 * reports the input bridge dropped). At the end: the mean correct RT per
 * block. Shift+Esc stops; the rows so far are kept.
 *
 * Usage: trial_srt [--sim] [--fullscreen] [--reps N] [--blocks SRS] [--seed N] [--out FILE]
 *   --sim       blocks S and R once each, on the simulated display and the
 *               virtual clock, with a scripted participant; checks every row
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

#define RSI_S      0.25
#define REST_S     2.0
#define SPACING    150.0f        /* px between the squares' centers */
#define SEQ_LEN    12
#define MAX_REPS   20

enum { TARGET_ON, TARGET_X, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };

static const int soc_sequence[SEQ_LEN] = { 0, 1, 0, 3, 2, 1, 3, 0, 2, 3, 1, 2 };
static const char conditions_csv[] =
    "position,key\n"
    "0,d\n"
    "1,f\n"
    "2,j\n"
    "3,k\n";
static const yrsp_choice keys[] = { { .key = "d" }, { .key = "f" }, { .key = "j" }, { .key = "k" } };

/* The scripted participant and the self-check of --sim, at the end of the
 * file. Without sim_begin() each call does nothing. */
static void sim_begin(int reps, const char* blocks);
static void sim_target_landed(int64_t onset, char block_type, int index_in_block, int pos, int trial);
static void sim_feed(yrsp_collector* c, int64_t now);
static void sim_check_row(int trial, const yrsp_result* r, int correct);
static int sim_check_end(int n_rows);

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

/* The options, then where the run is and the summary's sums */
static struct {
    int sim, fullscreen, reps;
    const char *blocks, *out_path;
    uint64_t seed;
} opt = { .out_path = "trial_srt.csv", .seed = 20261008 };
static FILE* out;
static ytr_trial_info ti;
static int block = -1, trial_index, position, pos_col, responding, rows;
static uint32_t doubles_at_arm;
static int n_correct[8];
static double sum_rt[8];

static int parse_options(int argc, char** argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) opt.sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) opt.fullscreen = 1;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) opt.reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--blocks") && i + 1 < argc) opt.blocks = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) opt.seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) opt.out_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_srt [--sim] [--fullscreen] [--reps N] [--blocks SRS] [--seed N] [--out FILE]\n");
            return 2;
        }
    }
    if (opt.reps == 0) opt.reps = opt.sim ? 1 : 4;
    if (!opt.blocks) opt.blocks = opt.sim ? "SR" : "SRS";
    if (opt.reps < 1 || opt.reps > MAX_REPS) { fprintf(stderr, "trial_srt: --reps takes 1..%d\n", MAX_REPS); return 2; }
    if (!opt.blocks[0] || strlen(opt.blocks) > 8 || strspn(opt.blocks, "SR") != strlen(opt.blocks)) {
        fprintf(stderr, "trial_srt: --blocks takes 1 to 8 of S and R\n");
        return 2;
    }
    return 0;
}

static bool fail(const char* why) {
    fprintf(stderr, "trial_srt: %s\n", why);
    return false;
}

static ygfx_stim square(float x, float stroke) {
    return ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_RECT, .x = x, .w = 100, .h = 100, .stroke = stroke,
                                          .edge = YGFX_EDGE_COSINE, .edge_width = 1, .color = { 0.9f, 0.9f, 0.9f } });
}

/* The bridge's count of dropped second key reports (a process total). */
static uint32_t key_doubles(void) {
    yscr_input_stats st;
    yscr_get_input_stats(&st);
    return st.key_doubles;
}

/* The display, the squares, the conditions, the timeline and the
 * collector: one of four keys, no deadline, the first key ends it. */
static bool open_task(void) {
    int i;
    if (!ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .backend = opt.sim ? YSCR_BACKEND_SIM : 0, .windowed = !opt.fullscreen },
                          &(ygfx_desc){ .background = { 0.5f, 0.5f, 0.5f } }))
        return fail(ygfx_error(&gfx));
    for (i = 0; i < 4; i++) boxes[i] = square(((float)i - 1.5f) * SPACING, 4);
    target = square(0, 0);
    if (!ytb_open_csv(&tab, &(ytb_csv_desc){ .text = conditions_csv, .len = sizeof conditions_csv - 1,
                                             .arena = table_arena, .arena_size = sizeof table_arena }))
        return fail(ytb_error(&tab));
    pos_col = ytb_col(&tab, "position");
    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS }))
        return fail(ytl_error(&tl));
    if (!yrsp_open(&rsp, &(yrsp_desc){ .choices = keys, .n_choices = 4 }))
        return fail(yrsp_error(&rsp));
    return true;
}

/* The data file: the settings as # lines, then the column names */
static bool open_data(void) {
    char line[1024];
    out = fopen(opt.out_path, "w");
    if (!out) { fprintf(stderr, "trial_srt: cannot write %s\n", opt.out_path); return false; }
    yscr_describe(&scr, line, sizeof line);
    fprintf(out, "# blocks %s, reps %d, seed %llu, rsi %.3f s\n# %s\n# ysp_response %s; keyboard: %s\n", opt.blocks,
            opt.reps, (unsigned long long)opt.seed, RSI_S, line, yrsp_version(),
            opt.sim ? "synthetic (tier SIM)" : "SDL through yscr_event_input");
    fprintf(out, "block,block_type,trial_index,position,response,rt_s,correct,onset_tier,onset_src,rsp_tier,rsp_flags,"
                 "n_rsi_presses,n_duplicates\n");
    printf("%s\n", line);
    return true;
}

/* One block's order: the sequence, or its positions shuffled with no
 * immediate repeat. A trial list per block: the two types order the same
 * table differently. */
static bool open_block(char type) {
    ytr_desc rd = { .table = &tab, .rng = ytr_splitmix, .rng_ctx = &opt.seed };
    int i;
    memset(&trials, 0, sizeof trials);
    if (type == 'S') {
        for (i = 0; i < SEQ_LEN * opt.reps; i++) order_list[i] = soc_sequence[i % SEQ_LEN];
        rd.order = YTR_ORDER_LIST;
        rd.order_list = order_list;
        rd.n_order_list = SEQ_LEN * opt.reps;
    } else {
        rd.reps = 3 * opt.reps;      /* each position 3 times per pass, as in the sequence */
        rd.order = YTR_ORDER_CONSTRAINED;
        rd.constraints[0] = ytr_max_run(YTR_CONDITION, YTR_ANY_LEVEL, 1);
        rd.n_constraints = 1;
    }
    return ytr_open(&trials, &rd) || fail(ytr_error(&trials));
}

/* The next target now, or after a rest at a block's start. 0 when the
 * blocks are done, -1 on an error. */
static int start_trial(const yscr_frame* f) {
    ytl_seq q;
    int rest = 0;
    if (block < 0 || ytr_next(&trials, &ti) < 0) {
        if (!opt.blocks[++block] || !open_block(opt.blocks[block])) return 0;
        ytr_next(&trials, &ti);
        rest = 1;
    }
    position = atoi(ytb_text(&tab, ti.condition, pos_col));
    ytl_clear(&tl, TRIAL);
    ytl_anchor(&tl, TRIAL, f->onset, 0);
    q = ytl_seq_on(&tl, TRIAL);
    ytl_wait(&q, rest ? YRT_S(REST_S) : 0);
    ytl_set_value(&q, TARGET_X, ((float)position - 1.5f) * SPACING);
    ytl_on(&q, TARGET_ON);
    if (q.err) { fprintf(stderr, "trial_srt: timeline error %d\n", q.err); return -1; }
    yrsp_arm(&rsp, f->onset);
    doubles_at_arm = key_doubles();
    responding = 1;
    return 1;
}

/* The RSI's end: yrsp_finish() comes this late so that the RSI's keys
 * count as AFTER_END, and the onset is final by now */
static void write_row(void) {
    yrsp_result res;
    char rt[32];
    int correct;
    yrsp_finish(&rsp, &res);
    correct = (res.flags & YRSP_R_RESPONDED) && res.response == position;
    if (res.rt_ns != YRSP_NO_TIME) snprintf(rt, sizeof rt, "%.9f", YRT_TO_S(res.rt_ns));
    else rt[0] = 0;
    fprintf(out, "%d,%s,%d,%d,%s,%s,%d,%u,%u,%u,%u,%d,%d\n", block, opt.blocks[block] == 'S' ? "sequence" : "random",
            trial_index, position, res.response_name ? res.response_name : "", rt, correct, (unsigned)res.onset_tier,
            (unsigned)res.onset_src, (unsigned)res.stamp_tier, (unsigned)res.flags, res.n_after_end,
            (int)(key_doubles() - doubles_at_arm));
    fflush(out);
    if (correct) { sum_rt[block] += YRT_TO_S(res.rt_ns); n_correct[block]++; }
    ytr_update(&trials, correct, NULL);
    sim_check_row(trial_index, &res, correct);
    rows++;
    trial_index++;
}

int main(int argc, char** argv) {
    char line[1024];
    int i, rc, in_trial = 0, aborted = 0, failed = 0;

    if ((rc = parse_options(argc, argv)) != 0) return rc;
    if (opt.sim) {
        yrt_virtual_begin(0);   /* ysp/rt.h's virtual clock: the same records on any host */
        sim_begin(opt.reps, opt.blocks);
    }
    if (!open_task() || !open_data()) return 1;

    for (;;) {
        yscr_frame f;
        ytl_event fired[64];   /* the timeline's capacity: all that can fire */
        int64_t bt = 0;
        int n, landed;
        rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_srt: %s\n", yscr_error(&scr)); failed = 1; break; }
        if (!in_trial) {
            if ((rc = start_trial(&f)) <= 0) { failed = rc < 0; break; }
            in_trial = 1;
        }

        yrsp_feed_screen(&scr, &rsp, 1);
        sim_feed(&rsp, f.onset);
        /* the target's flip record when it arrives; a key: the target goes
         * off on this frame, the next comes after the RSI */
        if (yrsp_update_frame(&rsp, &f) == YRSP_ENDED && responding) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, TARGET_ON);
            ytl_wait(&q, YRT_S(RSI_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "trial_srt: timeline error %d\n", q.err); failed = 1; break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        landed = yrsp_set_landing(&rsp, fired, n, TARGET_ON);   /* the target landed: its planned onset */
        if (landed >= 0) sim_target_landed(fired[landed].onset, opt.blocks[block], ti.index, position, trial_index);
        for (i = 0; i < n; i++)
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                write_row();
                in_trial = 0;
            }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw_n(&gfx, boxes, 4);
        ygfx_draw(&gfx, &target);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }

    /* the describe line again: its counters (key times, raw mice) are 0 at open */
    yscr_describe(&scr, line, sizeof line);
    fprintf(out, "# at the end: %s\n", line);
    fclose(out);
    ygfx_close(&gfx);
    yscr_close(&scr);
    for (i = 0; i <= block && opt.blocks[i]; i++)
        printf("block %d (%s): %d correct, mean rt %.3f s\n", i, opt.blocks[i] == 'S' ? "sequence" : "random",
               n_correct[i], n_correct[i] ? sum_rt[i] / n_correct[i] : NAN);
    printf("%d trials written to %s%s\n", rows, opt.out_path, aborted ? " (stopped with Shift+Esc)" : "");
    return failed ? failed : sim_check_end(rows);
}

/* ---- The scripted participant and the self-check (--sim) ----------------
 *
 * The correct key 0.30 s after the target's onset in S, 0.45 s in R; trial
 * 2 of R the key to the right, at 0.40 s; trial 4 of S a second press
 * 0.10 s after the response, in the RSI. The check needs the default
 * --reps and --blocks: on the virtual clock each onset is its plan and
 * each RT the script's, to the nanosecond. */
#define SIM_RT_S      0.30
#define SIM_RT_R      0.45
#define SIM_WRONG_RT  0.40
#define SIM_TRIALS    (2 * SEQ_LEN)

static int sim_on, sim_checks, sim_bad;
static yrsp_input sim_ev[8];
static int n_sim_ev, sim_next;
static int64_t sim_plan[SIM_TRIALS];   /* where the script's times start */

static void sim_begin(int reps, const char* blocks) {
    sim_on = 1;
    sim_checks = reps == 1 && !strcmp(blocks, "SR");
}

static void sim_target_landed(int64_t onset, char block_type, int index_in_block, int pos, int trial) {
    int odd = block_type == 'S' ? index_in_block == 4 : index_in_block == 2;
    int wrong = block_type == 'R' && odd;
    double at = wrong ? SIM_WRONG_RT : block_type == 'S' ? SIM_RT_S : SIM_RT_R;
    int p;
    if (!sim_on) return;
    n_sim_ev = sim_next = 0;
    if (trial < SIM_TRIALS) sim_plan[trial] = onset;
    for (p = 0; p < 1 + (block_type == 'S' && odd); p++) {
        yrsp_input* e = &sim_ev[n_sim_ev];
        *e = (yrsp_input){ .t = onset + YRT_S(at + 0.1 * p), .type = YRSP_PRESS, .stamp = YRSP_TIER_SIM, .value = 1,
                           .control = (uint32_t)yrsp_scancode(keys[(pos + wrong) % 4].key) };
        sim_ev[n_sim_ev + 1] = *e;
        sim_ev[n_sim_ev + 1].type = YRSP_RELEASE;
        sim_ev[n_sim_ev + 1].value = 0;
        sim_ev[n_sim_ev + 1].t = e->t + YRT_MS(60);
        n_sim_ev += 2;
    }
}

static void sim_feed(yrsp_collector* c, int64_t now) {
    while (sim_next < n_sim_ev && sim_ev[sim_next].t <= now) yrsp_feed(c, &sim_ev[sim_next++]);
}

static void sim_check_row(int trial, const yrsp_result* r, int correct) {
    int s = trial < SEQ_LEN, k = trial % SEQ_LEN;
    double want = s ? SIM_RT_S : k == 2 ? SIM_WRONG_RT : SIM_RT_R;
    if (!sim_checks || trial >= SIM_TRIALS) return;
    if (!((r->flags & YRSP_R_RESPONDED) && r->t_onset == sim_plan[trial] && r->rt_ns == YRT_S(want) &&
          correct == (s || k != 2) && r->n_after_end == (s && k == 4) && r->onset_tier == YSCR_TIER_SIM &&
          r->onset_src == YRSP_ONSET_FLIP)) {
        fprintf(stderr, "trial_srt: --sim: trial %d: rt %.9f correct %d flags 0x%x n_after_end %d\n", trial,
                YRT_TO_S(r->rt_ns), correct, (unsigned)r->flags, r->n_after_end);
        sim_bad = 1;
    }
}

static int sim_check_end(int n_rows) {
    if (!sim_on) return 0;
    if (!sim_checks) {
        printf("--sim: the self-check needs the default --reps and --blocks\n");
        return 0;
    }
    if (n_rows != SIM_TRIALS) {
        fprintf(stderr, "trial_srt: --sim: %d rows, not %d\n", n_rows, SIM_TRIALS);
        sim_bad = 1;
    }
    if (sim_bad) return 1;
    printf("--sim: every trial came out as scripted\n");
    return 0;
}
