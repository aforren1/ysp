/* trial_keyboard.c - the jsPsych html-keyboard-response trial as a small
 * experiment: ysp/trials.h, ysp/timeline.h, ysp/gfx.h and ysp/response.h.
 * The tutorial in docs/response.md walks through it.
 *
 * An instruction screen until space, then each trial: a fixation cross for
 * 0.5 s, then a circle or a square until a response or 1.5 s; F for the
 * circle, J for the square (the physical keys: they do not move with the
 * keyboard layout). Responses before 0.1 s are anticipations: logged, not
 * responses. Feedback for 0.4 s (a green ring when correct, red when wrong,
 * gray when too slow), then a blank inter-trial interval drawn from 0.8 to
 * 1.2 s in whole frames.
 *
 * The conditions are a CSV string (stimulus,key_answer), read by
 * ysp/table.h and run by ysp/trials.h in a random order. RT runs from the
 * stimulus's flip onset (the OS's vblank time of the frame it landed on),
 * not from when the program started listening, and the data row says how
 * good that onset is (its source and tier) and how far the stimulus landed
 * from its planned time.
 *
 * The data file has one row per trial (ysp/trials.h's data writer): the
 * session's columns (index, block, condition, ..., stimulus, key_answer,
 * iti), then trial_index, correct, n_duplicates (second key reports that
 * ysp/screen.h's input bridge dropped during the trial), then the
 * response's (YRSP_DATA_COLS: response, rt, rt_key_duration, onset_src,
 * onset_tier, onset_residual, landing_residual, onset_frame, rsp_tier,
 * rsp_flags, n_anticipations, n_early). Times in seconds. The settings go
 * to FILE.meta. Shift+Esc stops the session; the rows so far are kept.
 *
 * --sim runs on the simulated display with a synthetic participant (no
 * keyboard; the instructions show for 0.5 s): a correct response, an
 * anticipation then a response, a wrong key reported twice 9 ms apart (the
 * pattern SDL 3.4 gives for a virtual-key tap, docs/response.md; the
 * bridge's filter drops the second), a timeout, and two more correct
 * responses. It checks its own results and the file's columns, and exits 1
 * on a mismatch. CI runs it. The simulated display runs on the real clock:
 * about 15 s.
 *
 * Usage: trial_keyboard [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE]
 *   --reps N   repetitions of each condition (default 10; --sim: 3, which
 *              its self-check needs)
 *   --out FILE the data file (default trial_keyboard.csv)
 * Exit code: 0 (also after Shift+Esc), 1 when something did not open or a
 * --sim check failed, 2 for a bad argument. Without a system font the
 * instruction screen is blank (the message says so); the trials run.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"          /* before ysp/gfx.h: ygfx_text_make() */
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_TIMELINE_IMPLEMENTATION
#include "ysp/timeline.h"
#define YTB_STDIO                 /* --sim reads the file back */
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* timeline channels: what is visible; one stimulus each */
enum { FIX_ON, CIRCLE_ON, SQUARE_ON, FB_RIGHT, FB_WRONG, FB_NONE, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };       /* a MARK's code */

#define FIXATION_S  0.5
#define RESPONSE_S  1.5          /* jsPsych trial_duration */
#define MIN_RT_S    0.1          /* jsPsych minimum_valid_rt */
#define FEEDBACK_S  0.4

static const char conditions_csv[] =
    "stimulus,key_answer\n"
    "circle,f\n"
    "square,j\n";

static const yrsp_choice keys[] = { { .key = "f" }, { .key = "j" } };

static const struct { ygfx_shape_kind kind; float w, p0, rgb[3]; } look[N_CHANNELS] = {
    { YGFX_CROSS, 16, 3, { 0.2f, 0.2f, 0.2f } },     { YGFX_CIRCLE, 120, 0, { 0.9f, 0.9f, 0.9f } },
    { YGFX_RECT, 106, 0, { 0.9f, 0.9f, 0.9f } },     { YGFX_ANNULUS, 60, 24, { 0.1f, 0.7f, 0.1f } },
    { YGFX_ANNULUS, 60, 24, { 0.8f, 0.1f, 0.1f } },  { YGFX_ANNULUS, 60, 24, { 0.35f, 0.35f, 0.35f } },
};

/* The program's own columns; ysp/trials.h writes the session's before
 * them and yrsp_data_set() fills YRSP_DATA_COLS. */
typedef struct row { int trial_index, n_duplicates; bool correct; } row;
static const ytr_col cols[] = {
    YTR_COL(row, trial_index, YTR_COL_INT), YTR_COL(row, correct, YTR_COL_BOOL),
    YTR_COL(row, n_duplicates, YTR_COL_INT), YRSP_DATA_COLS,
};

/* The synthetic participant of --sim, by trial number: presses at `at` s
 * after the stimulus onset, held `hold` s; `twice` reports it a second
 * time 9 ms later. which: 1 the correct key, 2 the other one. */
typedef struct sim_press { int trial; double at, hold; int which, twice; } sim_press;
static const sim_press sim_script[] = {
    { 0, 0.450, 0.100, 1, 0 },
    { 1, 0.050, 0.040, 1, 0 },   /* an anticipation */
    { 1, 0.520, 0.100, 1, 0 },
    { 2, 0.400, 0.003, 2, 1 },   /* the wrong key, reported twice */
    /* trial 3: no press, a timeout */
    { 4, 0.380, 0.120, 1, 0 },
    { 5, 0.610, 0.090, 1, 0 },
};
#define SIM_TRIALS 6
static yrsp_input sim_ev[16];
static int n_sim_ev, sim_next;

static yscr_screen scr;
static ygfx_gfx gfx;
static ygfx_text intro;
static ygfx_stim stims[N_CHANNELS];
static ygfx_bind binds[N_CHANNELS];
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;                  /* about 100 KB: not the stack */
static ytr_data data;
static yrsp_collector rsp;
static uint64_t table_arena[2048], back_arena[8192];
static ytb_table tab, back;

static int fail(const char* why) {
    fprintf(stderr, "trial_keyboard: %s\n", why);
    return 1;
}

/* The bridge's count of dropped second key reports (a process total). */
static uint32_t key_doubles(void) {
    yscr_input_stats st;
    yscr_get_input_stats(&st);
    return st.key_doubles;
}

/* --sim: the scripted presses of trial `trial`, from the stimulus onset. */
static void sim_schedule(int trial, int64_t onset, const char* key_answer) {
    int k, rep;
    n_sim_ev = sim_next = 0;
    for (k = 0; k < (int)(sizeof sim_script / sizeof sim_script[0]); k++) {
        const sim_press* p = &sim_script[k];
        uint32_t sc = (uint32_t)yrsp_scancode(p->which == 1 ? key_answer : !strcmp(key_answer, "f") ? "j" : "f");
        if (p->trial != trial) continue;
        for (rep = 0; rep <= p->twice && n_sim_ev + 2 <= 16; rep++) {
            yrsp_input* e = &sim_ev[n_sim_ev++];
            memset(e, 0, sizeof *e);
            e->t = onset + YRT_S(p->at) + (int64_t)rep * YRT_MS(9);
            e->kind = YRSP_KIND_KEYBOARD;
            e->type = YRSP_PRESS;
            e->control = sc;
            e->code = (uint32_t)(sc == 9 ? 'f' : 'j');
            e->stamp = YRSP_TIER_SIM;
            e->value = 1;
            sim_ev[n_sim_ev] = *e;
            sim_ev[n_sim_ev].type = YRSP_RELEASE;
            sim_ev[n_sim_ev].value = 0;
            sim_ev[n_sim_ev++].t = e->t + YRT_S(p->hold);
        }
    }
}

int main(int argc, char** argv) {
    yscr_caps caps;
    yscr_frame f;
    yrsp_source source;
    ytl_event fired[64];                   /* the timeline's capacity: all that can fire */
    ytr_trial_info ti = { 0 };
    yrsp_result res, sim_res[SIM_TRIALS];
    row r = { 0 };
    int sim_doubles[SIM_TRIALS];
    uint32_t doubles_at_arm = 0, space = (uint32_t)yrsp_scancode("space");
    char line[1024], meta[1536], meta_path[1100];
    const char *out_path = "trial_keyboard.csv", *key_answer = "", *stimulus = "";
    uint64_t seed = 20261007;
    int i, n, sim = 0, reps = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0;
    int in_trial = 0, responding = 0, stim_ch = 0, stim_col, key_col;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_keyboard [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE]\n");
            return 2;
        }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    if (reps == 0) reps = sim ? SIM_TRIALS / 2 : 10;
    if (reps < 1 || reps > 100) { fprintf(stderr, "trial_keyboard: --reps takes 1..100\n"); return 2; }

    /* the display and the graphics, opened together and primed */
    if (!ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .backend = sim ? YSCR_BACKEND_SIM : 0, .windowed = !fullscreen },
                          &(ygfx_desc){ .background = { 0.5f, 0.5f, 0.5f } }))
        return fail(ygfx_error(&gfx));
    yscr_get_caps(&scr, &caps);
    for (i = 0; i < N_CHANNELS; i++) {      /* stimulus i is visible while channel i is on */
        stims[i] = ygfx_shape(&(ygfx_shape_desc){ .shape = look[i].kind, .w = look[i].w, .h = look[i].w,
            .shape_p = { look[i].p0 }, .edge = YGFX_EDGE_COSINE, .edge_width = 1,
            .color = { look[i].rgb[0], look[i].rgb[1], look[i].rgb[2] } });
        binds[i] = (ygfx_bind){ .stim = &stims[i], .param = YGFX_P_VISIBLE, .channel = (uint16_t)i };
    }
    if (ygfx_text_make(&gfx, &intro, &(ygfx_text_desc){ .color = { 0.02f, 0.02f, 0.02f }, .text =
            "Press F when you see a circle and J when you see a square.\n\nPress space to start." }) < 0)
        fprintf(stderr, "trial_keyboard: %s; the instruction screen is blank\n", ygfx_error(&gfx));

    /* the conditions, and their order with an inter-trial interval in whole frames */
    if (!ytb_open_csv(&tab, &(ytb_csv_desc){ .text = conditions_csv, .len = sizeof conditions_csv - 1,
                                             .arena = table_arena, .arena_size = sizeof table_arena }))
        return fail(ytb_error(&tab));
    stim_col = ytb_col(&tab, "stimulus");
    key_col = ytb_col(&tab, "key_answer");
    if (!ytr_open(&trials, &(ytr_desc){ .table = &tab, .reps = reps, .order = YTR_ORDER_RANDOM, .rng = ytr_splitmix,
            .rng_ctx = &seed, .n_jitters = 1, .jitters = { ytr_frames(ytr_uniform("iti", YRT_MS(800), YRT_MS(1200)),
            caps.mode.refresh_num ? caps.mode.refresh_num : 60, caps.mode.refresh_num ? caps.mode.refresh_den : 1) } }))
        return fail(ytr_error(&trials));

    /* the timeline: one base for the trial */
    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS }))
        return fail(ytl_error(&tl));

    /* the response: f or j, within 1.5 s of the onset, none before 0.1 s */
    source = yrsp_sdl_source(&(yrsp_sdl_ctx){ .raw_keyboard = caps.raw_keyboard }, YRSP_KIND_KEYBOARD);
    if (!yrsp_open(&rsp, &(yrsp_desc){ .choices = keys, .n_choices = 2, .duration_ns = YRT_S(RESPONSE_S),
                                       .min_rt_ns = YRT_S(MIN_RT_S), .sources = &source, .n_sources = 1 }))
        return fail(yrsp_error(&rsp));

    /* the data file, and the settings beside it */
    yscr_describe(&scr, line, sizeof line);
    snprintf(meta, sizeof meta, "screen=%s\nysp_response=%s\nkeyboard=%s\n", line, yrsp_version(),
             sim ? "synthetic (tier SIM)" : source.note);
    snprintf(meta_path, sizeof meta_path, "%s.meta", out_path);
    if (!ytr_data_open(&data, &(ytr_data_desc){ .path = out_path, .trials = &trials, .cols = cols,
            .n_cols = (int)(sizeof cols / sizeof cols[0]), .meta_path = meta_path, .meta = meta }))
        return fail(ytr_data_error(&data));
    printf("%s\n", line);

    /* the instructions, until space (--sim: 0.5 s) */
    if (ygfx_show_until_key(&gfx, &intro.stim, intro.n ? 1 : 0, &space, 1, sim ? YRT_MS(500) : 0, NULL) == YGFX_SHOW_QUIT)
        aborted = 1;
    while (!aborted) {
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { failed = fail(yscr_error(&scr)); break; }

        if (!in_trial) {
            /* a new trial: fixation now, the stimulus 0.5 s later */
            ytl_seq q;
            if (ytr_next(&trials, &ti) < 0) break;
            stimulus = ytb_text(&tab, ti.condition, stim_col);
            key_answer = ytb_text(&tab, ti.condition, key_col);
            stim_ch = !strcmp(stimulus, "circle") ? CIRCLE_ON : SQUARE_ON;
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            ytl_on(&q, FIX_ON);
            ytl_wait(&q, YRT_S(FIXATION_S));
            ytl_off(&q, FIX_ON);
            ytl_on(&q, stim_ch);
            if (q.err) { failed = fail(ytl_strerror(q.err)); break; }
            yrsp_arm(&rsp, f.onset);      /* presses from the fixation on are logged (EARLY) */
            doubles_at_arm = key_doubles();
            in_trial = responding = 1;
        }

        /* every input event goes to the collector, so held keys are known
         * between trials too; the synthetic participant's go through the
         * bridge's filter as a keyboard's would */
        yrsp_feed_screen(&scr, &rsp, 1);
        for (; sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset; sim_next++)
            if (yscr_key_filter(&scr, &sim_ev[sim_next])) yrsp_feed(&rsp, &sim_ev[sim_next]);

        /* the stimulus frame's flip record, when it arrives, is the final
         * onset; a response or the deadline ends the window: hide the
         * stimulus on this frame */
        if (yrsp_update_frame(&rsp, &f) == YRSP_ENDED && responding) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            int64_t bt = 0;
            int fb;
            yrsp_finish(&rsp, &res);
            fb = !(res.flags & YRSP_R_RESPONDED) ? FB_NONE : !strcmp(res.response_name, key_answer) ? FB_RIGHT : FB_WRONG;
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, stim_ch);
            ytl_on(&q, fb);
            ytl_wait(&q, YRT_S(FEEDBACK_S));
            ytl_off(&q, fb);
            ytl_wait(&q, ytr_jitter(&trials, ti.index, 0).ns);
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { failed = fail(ytl_strerror(q.err)); break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        /* the stimulus landed on this frame: its planned onset, for now */
        if (yrsp_set_landing(&rsp, fired, n, stim_ch) >= 0 && sim) sim_schedule(ti.index, f.onset, key_answer);
        for (i = 0; i < n; i++)
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                /* the trial is over, and the record of its stimulus is in */
                yrsp_finish(&rsp, &res);
                r.trial_index = ti.index;
                r.correct = (res.flags & YRSP_R_RESPONDED) && !strcmp(res.response_name, key_answer);
                r.n_duplicates = (int)(key_doubles() - doubles_at_arm);
                ytr_update(&trials, (res.flags & YRSP_R_RESPONDED) ? r.correct : YTR_INVALID, NULL);
                if (yrsp_data_set(&data, &res) < 0 || ytr_data_write(&data, &r) < 0) failed = fail(ytr_data_error(&data));
                if (!(res.flags & YRSP_R_RESPONDED)) printf("trial %2d %-6s too slow\n", ti.index, stimulus);
                else printf("trial %2d %-6s rt %.3f s, %s\n", ti.index, stimulus, YRT_TO_S(res.rt_ns), r.correct ? "correct" : "wrong");
                if (ti.index < SIM_TRIALS) {
                    sim_res[ti.index] = res;
                    sim_doubles[ti.index] = r.n_duplicates;
                }
                rows++;
                in_trial = 0;
            }
        ygfx_apply(binds, N_CHANNELS, ytl_values(&tl));
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        for (i = 0; i < N_CHANNELS; i++) ygfx_draw(&gfx, &stims[i]);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    if (ytr_data_finish(&data) < 0) failed = fail(ytr_data_error(&data));
    ygfx_text_free(&gfx, &intro);
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");

    if (sim && !failed && reps != SIM_TRIALS / 2) printf("--sim: the self-check needs the default --reps\n");
    else if (sim && !failed) {
        /* the synthetic participant's trials must come out as scripted */
        static const char* const v030_cols[] = { "trial_index", "stimulus", "key_answer", "response", "rt",
            "correct", "rt_key_duration", "onset_tier", "onset_src", "landing_residual", "onset_frame",
            "rsp_tier", "rsp_flags", "n_anticipations", "n_duplicates", "iti" };
        int bad = 0;
        if (rows != SIM_TRIALS) { fprintf(stderr, "trial_keyboard: --sim: %d rows, not %d\n", rows, SIM_TRIALS); bad = 1; }
        for (i = 0; i < rows && i < SIM_TRIALS; i++) {
            const yrsp_result* q = &sim_res[i];
            double want_rt = i == 0 ? 0.45 : i == 1 ? 0.52 : i == 2 ? 0.40 : i == 4 ? 0.38 : i == 5 ? 0.61 : NAN;
            int ok = 1;
            if (i == 3) ok = !(q->flags & YRSP_R_RESPONDED) && (q->flags & YRSP_R_TIMEOUT);
            else ok = (q->flags & YRSP_R_RESPONDED) && fabs(YRT_TO_S(q->rt_ns) - want_rt) < 1e-6;
            if (i == 0) ok = ok && fabs(YRT_TO_S(q->rt_key_duration_ns) - 0.1) < 1e-6;
            if (i == 1) ok = ok && q->n_anticipations == 1;
            if (i == 2) ok = ok && sim_doubles[i] == 1 && q->n_responses == 1;
            ok = ok && q->onset_tier == YSCR_TIER_SIM && q->onset_src == YRSP_ONSET_FLIP &&
                 q->plan_residual != YRSP_NO_TIME && !(q->flags & YRSP_R_ONSET_PLAN);
            if (i != 3) ok = ok && q->stamp_tier == YRSP_TIER_SIM;
            if (!ok) {
                fprintf(stderr, "trial_keyboard: --sim: trial %d: rt %.9f flags 0x%x n_anticipations %d "
                                "n_duplicates %d onset tier %d src %d\n", i, YRT_TO_S(q->rt_ns), (unsigned)q->flags,
                        q->n_anticipations, sim_doubles[i], q->onset_tier, q->onset_src);
                bad = 1;
            }
        }
        /* the file: every row, and every column the v0.3.0 example wrote */
        if (!ytb_open_csv_file(&back, out_path, &(ytb_csv_desc){ .allow_empty = true, .arena = back_arena,
                                                                 .arena_size = sizeof back_arena })) { fprintf(stderr, "trial_keyboard: --sim: %s\n", ytb_error(&back)); bad = 1; }
        else {
            if (back.n_rows != rows) { fprintf(stderr, "trial_keyboard: --sim: the file has %d rows\n", back.n_rows); bad = 1; }
            for (i = 0; i < (int)(sizeof v030_cols / sizeof v030_cols[0]); i++)
                if (ytb_col(&back, v030_cols[i]) < 0) { fprintf(stderr, "trial_keyboard: --sim: no column %s\n", v030_cols[i]); bad = 1; }
            ytb_close(&back);
        }
        if (bad) return 1;
        printf("--sim: every trial came out as scripted\n");
    }
    return failed;
}
