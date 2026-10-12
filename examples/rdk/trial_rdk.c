/* trial_rdk.c - a random dot motion direction task: ysp/rdk.h for the
 * dots, ysp/trials.h for the coherence, ysp/response.h for the RT from
 * the motion onset's flip record, and a replay digest in each data row.
 *
 * It does what PsychoPy's Coder demos stimuli/dots.py and dot_gabors.py
 * show, as a trial with a response, and what jspsych-contrib's
 * plugin-rdk records. PsychoPy is GPL-3: its demos were read, no code
 * was copied.
 *
 * Each trial: a fixation cross for 0.5 s, then 200 white dots in a 400 px
 * circle (4 px, 200 px/s, the same dots carry the signal all trial, noise
 * dots in their own directions: PsychoPy's DotStim defaults, and ysp/rdk.h
 * names them) until a response or 1.5 s. Left or Right arrow for the
 * coherent direction (180 or 0 deg). Responses before 0.1 s are
 * anticipations. A blank of 0.8 s follows. The conditions are a CSV
 * string (coherence x direction), run in a random order.
 *
 * The motion's time is the trial's own: yrdk_start(seed, 0) on the dots'
 * first frame, then yrdk_update(f.onset - that frame's onset) on each
 * frame, so the field moves by the predicted onsets (a late frame shows
 * the dots where they are at its own time). Each update's inputs go to
 * the steps file (--steps); the row has the seed, the update count and
 * yrdk_digest() of the final state. ysp/rdk.h's REPRODUCIBILITY section
 * defines every rule in integers, so yrdk_replay() of a trial's steps
 * from its seed gives the same digest on any machine: a reviewer can
 * regenerate every dot of every frame from the two files and the desc in
 * FILE.meta (its rdk= line).
 *
 * The data file has one row per trial (ysp/trials.h's data writer): the
 * session's columns (index, block, ..., coherence, coherent_direction,
 * key_answer: jsPsych's names), then trial_index, correct, rdk_seed,
 * rdk_updates, rdk_digest, frames_dropped (flip records of the motion
 * frames with a dropped vblank), then the response's (YRSP_DATA_COLS:
 * response, rt, onset_src, onset_tier, the residuals, onset_frame,
 * rsp_tier, rsp_flags, n_anticipations, n_early). Times in seconds; the
 * settings go to FILE.meta. Steps file: trial_index, update, t (s from the
 * motion onset), coherence, direction, speed.
 *
 * At the end of every run the program reads the steps file back and
 * replays each trial: every digest must match the row's, or it exits 1.
 *
 * --sim: the simulated display and a synthetic participant (RT 0.65 s
 * minus 0.8 s x coherence, correct on every trial but the one at the
 * lowest coherence leftward). It also checks every row as scripted.
 * Exits 1 on a mismatch. About 15 s.
 *
 * Usage: trial_rdk [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE] [--steps FILE]
 *   --reps N    repetitions of each condition (default 5; --sim: 1)
 * Exit code: 0 (also after Shift+Esc), 1 when something did not open, a
 * replay differed or a --sim check failed, 2 for a bad argument.
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
#define YSP_RDK_IMPLEMENTATION
#include "ysp/rdk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FIX_ON, DOTS_ON, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };

#define FIXATION_S 0.5
#define RESPONSE_S 1.5
#define MIN_RT_S   0.1
#define ITI_S      0.8
#define N_DOTS     200
#define MAX_STEPS  1024     /* 1.5 s at 240 Hz is 360 updates */
#define MAX_ROWS   512

static const char conditions_csv[] =
    "coherence,coherent_direction,key_answer\n"
    "0.032,0,right\n0.032,180,left\n"
    "0.064,0,right\n0.064,180,left\n"
    "0.128,0,right\n0.128,180,left\n"
    "0.256,0,right\n0.256,180,left\n";

static const yrsp_choice keys[] = { { .key = "left" }, { .key = "right" } };

/* The program's own columns; ysp/trials.h writes the session's before
 * them and yrsp_data_set() fills YRSP_DATA_COLS. The seed and the digest
 * are text: a uint64_t, the digest in hex. */
typedef struct row { int trial_index, rdk_updates, frames_dropped; bool correct; char rdk_seed[24], rdk_digest[17]; } row;
static const ytr_col cols[] = {
    YTR_COL(row, trial_index, YTR_COL_INT), YTR_COL(row, correct, YTR_COL_BOOL),
    YTR_COL(row, rdk_seed, YTR_COL_CHARS), YTR_COL(row, rdk_updates, YTR_COL_INT),
    YTR_COL(row, rdk_digest, YTR_COL_CHARS), YTR_COL(row, frames_dropped, YTR_COL_INT), YRSP_DATA_COLS,
};

static yscr_screen scr;
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static ytr_data data;
static yrsp_collector rsp;
static uint64_t table_arena[2048];
static ytb_table tab;
static yrdk_field rdk, replay;
static ygfx_buf buf;
static ygfx_stim fix, dots;
static float xy[2 * N_DOTS];
static yrdk_step steps[MAX_STEPS];
static const ygfx_bind binds[] = {
    { .stim = &fix,  .param = YGFX_P_VISIBLE, .channel = FIX_ON  },
    { .stim = &dots, .param = YGFX_P_VISIBLE, .channel = DOTS_ON },
};
static yrsp_input sim_ev[2];
static int n_sim_ev, sim_next;

static int fail(const char* why) {
    fprintf(stderr, "trial_rdk: %s\n", why);
    return 1;
}

/* --sim: a press of `key` at t, held 0.06 s. */
static void sim_press(int64_t t, const char* key) {
    sim_ev[0] = (yrsp_input){ .t = t, .kind = YRSP_KIND_KEYBOARD, .type = YRSP_PRESS,
                              .control = (uint32_t)yrsp_scancode(key), .stamp = YRSP_TIER_SIM, .value = 1 };
    sim_ev[1] = sim_ev[0];
    sim_ev[1].type = YRSP_RELEASE;
    sim_ev[1].value = 0;
    sim_ev[1].t += YRT_MS(60);
    n_sim_ev = 2;
    sim_next = 0;
}

/* Read the steps file back, replay each trial, compare digests. */
static int check_replays(const char* path, const uint64_t* seed, const uint64_t* digest, int rows) {
    FILE* fp = fopen(path, "r");
    char line[256];
    int trial = -1, n = 0, bad = 0, done = 0, k = 0, u = 0;
    double t = 0;
    float c = 0, d = 0, s = 0;
    if (!fp) { fprintf(stderr, "trial_rdk: cannot read %s\n", path); return 1; }
    if (!fgets(line, sizeof line, fp)) line[0] = 0;   /* the header */
    for (;;) {
        int got = fgets(line, sizeof line, fp) ? sscanf(line, "%d,%d,%lf,%f,%f,%f", &k, &u, &t, &c, &d, &s) : 0;
        if ((got != 6 || k != trial) && trial >= 0) {
            /* the previous trial is complete: regenerate it */
            if (yrdk_replay(&replay, seed[trial], 0, steps, n) < 0 || yrdk_digest(&replay) != digest[trial]) {
                fprintf(stderr, "trial_rdk: trial %d: the replay of %d updates gives %016llx, the row %016llx\n",
                        trial, n, (unsigned long long)yrdk_digest(&replay), (unsigned long long)digest[trial]);
                bad = 1;
            }
            done++;
            n = 0;
        }
        if (got != 6) break;
        if (k < 0 || k >= rows || n >= MAX_STEPS) { bad = 1; break; }
        trial = k;
        steps[n].t = (int64_t)llround(t * 1e9);   /* 9 decimals: the exact ns */
        steps[n].coherence = c;
        steps[n].direction = d;
        steps[n].speed = s;
        n++;
    }
    fclose(fp);
    if (done != rows) { fprintf(stderr, "trial_rdk: %d trials in %s, %d rows\n", done, path, rows); bad = 1; }
    if (!bad) printf("%d trials replayed from %s, every digest the same\n", done, path);
    return bad;
}

int main(int argc, char** argv) {
    static uint64_t row_seed[MAX_ROWS], row_digest[MAX_ROWS];
    static double sim_rt[MAX_ROWS];
    static int sim_key[MAX_ROWS];
    static yrsp_result sim_res[MAX_ROWS];
    static int64_t sim_plan[MAX_ROWS];   /* where the script's times start */
    const yrdk_desc rdesc = { .w = 400, .count = N_DOTS, .speed = 200 };   /* DotStim's SAME signal, DIRECTION noise */
    yscr_caps caps;
    yscr_frame f;
    yrsp_source source;
    ytl_event fired[64];                   /* the timeline's capacity: all that can fire */
    ytr_trial_info ti = { 0 };
    yrsp_result res;
    row r = { 0 };
    FILE* sout;
    char line[1024], meta[2048], meta_path[1100];
    const char *out_path = "trial_rdk.csv", *steps_path = "trial_rdk_steps.csv";
    uint64_t seed = 20261009, trial_seed = 0;
    int i, n, landed, sim = 0, reps = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0, sim_bad = 0;
    int in_trial = 0, responding = 0, moving = 0, n_steps = 0, dropped = 0, coh_col, dir_col, key_col, answer = 0;
    int64_t motion_t0 = 0, first_motion_frame = -1, last_motion_frame = -1;
    double coherence = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) steps_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_rdk [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE] [--steps FILE]\n");
            return 2;
        }
    }
    if (reps == 0) reps = sim ? 1 : 5;
    if (reps < 1 || reps > 50) { fprintf(stderr, "trial_rdk: --reps takes 1..50\n"); return 2; }

    /* the display and the graphics, opened together and primed */
    if (!ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .backend = sim ? YSCR_BACKEND_SIM : 0, .windowed = !fullscreen },
                          &(ygfx_desc){ .background = { 0.5f, 0.5f, 0.5f } }))
        return fail(ygfx_error(&gfx));
    yscr_get_caps(&scr, &caps);

    /* the field, a second one for the replays, and the stimuli */
    if (!yrdk_open(&rdk, &rdesc) || !yrdk_open(&replay, &rdesc)) return fail(yrdk_error(&rdk));
    buf = ygfx_buffer(&gfx, sizeof xy);
    dots = ygfx_dots(&(ygfx_dots_desc){ .buf = buf, .count = N_DOTS, .dot_size = 4, .aperture = YGFX_CIRCLE, .w = 400,
                                        .color = { 1, 1, 1 } });
    fix = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_CROSS, .w = 16, .h = 16, .shape_p = { 3 },
                                         .edge = YGFX_EDGE_COSINE, .edge_width = 1, .color = { 0.2f, 0.2f, 0.2f } });

    /* the conditions, in a random order */
    if (!ytb_open_csv(&tab, &(ytb_csv_desc){ .text = conditions_csv, .len = sizeof conditions_csv - 1,
                                             .arena = table_arena, .arena_size = sizeof table_arena }))
        return fail(ytb_error(&tab));
    coh_col = ytb_col(&tab, "coherence");
    dir_col = ytb_col(&tab, "coherent_direction");
    key_col = ytb_col(&tab, "key_answer");
    if (!ytr_open(&trials, &(ytr_desc){ .table = &tab, .reps = reps, .order = YTR_ORDER_RANDOM, .rng = ytr_splitmix,
                                        .rng_ctx = &seed }))
        return fail(ytr_error(&trials));
    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS }))
        return fail(ytl_error(&tl));

    /* the response: an arrow key, within 1.5 s of the motion onset, none before 0.1 s */
    source = yrsp_sdl_source(&(yrsp_sdl_ctx){ .raw_keyboard = caps.raw_keyboard }, YRSP_KIND_KEYBOARD);
    if (!yrsp_open(&rsp, &(yrsp_desc){ .choices = keys, .n_choices = 2, .duration_ns = YRT_S(RESPONSE_S),
                                       .min_rt_ns = YRT_S(MIN_RT_S), .sources = &source, .n_sources = 1 }))
        return fail(yrsp_error(&rsp));

    /* the data file, the settings beside it, and the steps file */
    yscr_describe(&scr, line, sizeof line);
    snprintf(meta, sizeof meta, "screen=%s\nysp_response=%s\nkeyboard=%s\nrdk=ysp_rdk %s: w=400 count=%d speed=200 "
             "aperture=circle signal=same select=exact noise=direction edge=wrap sets=1 lifetime=0 clock=onset "
             "stream=0\nsteps=%s\n", line, yrsp_version(), sim ? "synthetic (tier SIM)" : source.note, yrdk_version(),
             N_DOTS, steps_path);
    snprintf(meta_path, sizeof meta_path, "%s.meta", out_path);
    if (!ytr_data_open(&data, &(ytr_data_desc){ .path = out_path, .trials = &trials, .cols = cols,
            .n_cols = (int)(sizeof cols / sizeof cols[0]), .meta_path = meta_path, .meta = meta }))
        return fail(ytr_data_error(&data));
    if (!(sout = fopen(steps_path, "w"))) return fail("cannot write the steps file");
    fprintf(sout, "trial_index,update,t,coherence,direction,speed\n");
    printf("%s\n", line);

    while (!aborted) {
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { failed = fail(yscr_error(&scr)); break; }
        for (i = 0; i < f.n_done; i++)   /* the motion frames' records: dropped vblanks */
            if (first_motion_frame >= 0 && f.done[i].index >= first_motion_frame &&
                (last_motion_frame < 0 || f.done[i].index <= last_motion_frame) && f.done[i].dropped)
                dropped++;

        if (!in_trial) {
            /* a new trial: fixation now, the dots 0.5 s later */
            ytl_seq q;
            if (ytr_next(&trials, &ti) < 0 || ti.index >= MAX_ROWS) break;
            coherence = ytb_num(&tab, ti.condition, coh_col);
            answer = !strcmp(ytb_text(&tab, ti.condition, key_col), "right");
            trial_seed = seed * 1000003u + (uint64_t)ti.index;   /* one field per trial, from the session's seed */
            rdk.coherence = (float)coherence;
            rdk.direction = (float)ytb_num(&tab, ti.condition, dir_col);
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            ytl_on(&q, FIX_ON);
            ytl_wait(&q, YRT_S(FIXATION_S));
            ytl_on(&q, DOTS_ON);
            if (q.err) { failed = fail(ytl_strerror(q.err)); break; }
            yrsp_arm(&rsp, f.onset);
            first_motion_frame = last_motion_frame = -1;
            in_trial = responding = 1;
            moving = n_steps = dropped = 0;
        }

        yrsp_feed_screen(&scr, &rsp, 1);
        for (; sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset; sim_next++) yrsp_feed(&rsp, &sim_ev[sim_next]);

        /* the motion frame's flip record, when it arrives, is the final
         * onset; the response or the deadline ends the motion on this frame */
        if (yrsp_update_frame(&rsp, &f) == YRSP_ENDED && responding) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            int64_t bt = 0;
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, DOTS_ON);
            ytl_off(&q, FIX_ON);
            ytl_wait(&q, YRT_S(ITI_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { failed = fail(ytl_strerror(q.err)); break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        /* the dots landed on this frame: their planned onset, for now */
        landed = yrsp_set_landing(&rsp, fired, n, DOTS_ON);
        if (landed >= 0 && sim) {   /* faster at higher coherence; one wrong key */
            for (; sim_next < n_sim_ev; sim_next++) yrsp_feed(&rsp, &sim_ev[sim_next]);
            sim_rt[ti.index] = 0.65 - 0.8 * coherence;
            sim_key[ti.index] = coherence < 0.05 && !answer ? !answer : answer;
            sim_plan[ti.index] = fired[landed].onset;
            sim_press(sim_plan[ti.index] + YRT_S(sim_rt[ti.index]), sim_key[ti.index] ? "right" : "left");
        }
        for (i = 0; i < n; i++)
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                /* the trial is over, and the record of its motion onset is in */
                yrsp_finish(&rsp, &res);
                r.trial_index = ti.index;
                r.correct = (res.flags & YRSP_R_RESPONDED) && res.response == answer;
                r.rdk_updates = n_steps;
                r.frames_dropped = dropped;
                row_seed[ti.index] = trial_seed;
                row_digest[ti.index] = yrdk_digest(&rdk);
                snprintf(r.rdk_seed, sizeof r.rdk_seed, "%llu", (unsigned long long)trial_seed);
                snprintf(r.rdk_digest, sizeof r.rdk_digest, "%016llx", (unsigned long long)row_digest[ti.index]);
                ytr_update(&trials, (res.flags & YRSP_R_RESPONDED) ? r.correct : YTR_INVALID, NULL);
                if (yrsp_data_set(&data, &res) < 0 || ytr_data_write(&data, &r) < 0) failed = fail(ytr_data_error(&data));
                if (!(res.flags & YRSP_R_RESPONDED)) printf("trial %2d coherence %.3f too slow", ti.index, coherence);
                else printf("trial %2d coherence %.3f rt %.3f s, %s", ti.index, coherence, YRT_TO_S(res.rt_ns),
                            r.correct ? "correct" : "wrong");
                printf("; %d updates, digest %s\n", n_steps, r.rdk_digest);
                sim_res[ti.index] = res;
                rows++;
                in_trial = 0;
            }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));

        /* the dots at this frame's onset, on the trial's motion clock */
        if (ytl_values(&tl)[DOTS_ON] > 0.5f) {
            if (!moving) {
                motion_t0 = f.onset;
                first_motion_frame = f.index;
                yrdk_start(&rdk, trial_seed, 0);
                moving = 1;
            }
            if (n_steps < MAX_STEPS && yrdk_update(&rdk, f.onset - motion_t0) >= 0) {
                steps[n_steps] = rdk.last;
                fprintf(sout, "%d,%d,%.9f,%.9g,%.9g,%.9g\n", ti.index, n_steps, (double)rdk.last.t / 1e9,
                        (double)rdk.last.coherence, (double)rdk.last.direction, (double)rdk.last.speed);
                n_steps++;
            }
            yrdk_xy(&rdk, xy);
            ygfx_buffer_update(&gfx, buf, 0, xy, sizeof xy);
        } else if (moving && last_motion_frame < 0) last_motion_frame = f.index - 1;
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &fix);
        ygfx_draw(&gfx, &dots);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    if (ytr_data_finish(&data) < 0) failed = fail(ytr_data_error(&data));
    fclose(sout);
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d trials written to %s, their steps to %s%s\n", rows, out_path, steps_path,
           aborted ? " (stopped with Shift+Esc)" : "");

    if (sim && !failed && !aborted) {
        for (i = 0; i < rows; i++) {
            const yrsp_result* q = &sim_res[i];
            if (!(q->flags & YRSP_R_RESPONDED) || q->response != sim_key[i] || /* script from the plan, RT from the flip record */
                llabs(q->rt_ns + (q->t_onset - sim_plan[i]) - YRT_S(sim_rt[i])) > 1000 ||
                q->onset_tier != YSCR_TIER_SIM || q->onset_src != YRSP_ONSET_FLIP) {
                fprintf(stderr, "trial_rdk: --sim: trial %d: rt %.9f (want %.9f) response %d (want %d) onset tier %d "
                                "src %d\n", i, YRT_TO_S(q->rt_ns), sim_rt[i], q->response, sim_key[i], q->onset_tier, q->onset_src);
                sim_bad = 1;
            }
        }
        if (rows != 8 * reps) { fprintf(stderr, "trial_rdk: --sim: %d rows, not %d\n", rows, 8 * reps); sim_bad = 1; }
        if (!sim_bad) printf("--sim: every trial came out as scripted\n");
        failed = sim_bad;
    }
    /* every run: the steps file must regenerate each row's digest */
    if (!failed && !aborted && check_replays(steps_path, row_seed, row_digest, rows)) failed = 1;
    yrdk_close(&rdk);
    yrdk_close(&replay);
    return failed;
}
