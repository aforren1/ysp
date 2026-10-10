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
 * regenerate every dot of every frame from the two files and the desc on
 * the data file's '#' line.
 *
 * Columns, jsPsych's names first: trial_index, coherence,
 * coherent_direction, key_answer, response, rt, correct; then onset_tier,
 * onset_src, landing_residual, onset_frame, rsp_tier, n_anticipations,
 * rdk_seed, rdk_updates, rdk_digest, frames_dropped (flip records of the
 * motion frames with a dropped vblank). Times in seconds. Steps file:
 * trial_index, update, t (s from the motion onset), coherence, direction,
 * speed.
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

static yscr_screen scr;
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static yrsp_collector rsp;
static uint64_t table_arena[2048];
static ytb_table tab;
static yrdk_field rdk, replay;
static yrdk_desc rdesc;
static ygfx_buf buf;
static ygfx_stim fix, dots;
static float xy[2 * N_DOTS];
static yrdk_step steps[MAX_STEPS];
static const ygfx_bind binds[] = {
    { .stim = &fix,  .param = YGFX_P_VISIBLE, .channel = FIX_ON  },
    { .stim = &dots, .param = YGFX_P_VISIBLE, .channel = DOTS_ON },
};

static void sec(char* out, size_t cap, double s) {
    if (s != s) out[0] = 0;
    else snprintf(out, cap, "%.9f", s);
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
    yscr_desc sd;
    ygfx_desc gd;
    ygfx_dots_desc dd;
    ygfx_shape_desc shd;
    ytl_desc td;
    ytb_csv_desc cd;
    ytr_desc rd;
    yrsp_desc pd;
    yrsp_sdl_ctx sdl;
    yrsp_source sources[1];
    yscr_caps caps;
    yscr_frame f;
    ytl_event fired[16];
    ytr_trial_info ti = { 0 };
    yrsp_result res;
    yrsp_input sim_ev[2];
    FILE *out, *sout;
    char line[1024], meta[2048];
    const char* out_path = "trial_rdk.csv";
    const char* steps_path = "trial_rdk_steps.csv";
    uint64_t seed = 20261009, trial_seed = 0;
    int i, n, sim = 0, reps = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0, sim_bad = 0;
    int in_trial = 0, responding = 0, moving = 0, n_steps = 0, n_sim_ev = 0, sim_next = 0, dropped = 0;
    int coh_col, dir_col, key_col, answer = 0;
    int64_t stim_frame = -1, landing_residual = 0, motion_t0 = 0, first_motion_frame = -1, last_motion_frame = -1;
    double coherence = 0;
    float direction = 0;

    memset(&sd, 0, sizeof sd);
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
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = !fullscreen;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "trial_rdk: %s\n", yscr_error(&scr)); return 1; }
    yscr_get_caps(&scr, &caps);
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "trial_rdk: %s\n", ygfx_error(&gfx)); return 1; }

    /* the field (PsychoPy's DotStim defaults: SAME signal, DIRECTION noise) */
    memset(&rdesc, 0, sizeof rdesc);
    rdesc.w = 400;
    rdesc.count = N_DOTS;
    rdesc.speed = 200;
    if (yrdk_open(&rdk, &rdesc) < 0 || yrdk_open(&replay, &rdesc) < 0) {
        fprintf(stderr, "trial_rdk: %s\n", yrdk_error(&rdk));
        return 1;
    }
    buf = ygfx_buffer(&gfx, sizeof xy);
    memset(&dd, 0, sizeof dd);
    dd.buf = buf;
    dd.count = N_DOTS;
    dd.dot_size = 4;
    dd.aperture = YGFX_CIRCLE;
    dd.w = 400;
    dd.color[0] = dd.color[1] = dd.color[2] = 1.0f;
    dots = ygfx_dots(&dd);
    memset(&shd, 0, sizeof shd);
    shd.shape = YGFX_CROSS;
    shd.w = shd.h = 16;
    shd.shape_p[0] = 3;
    shd.edge = YGFX_EDGE_COSINE;
    shd.edge_width = 1;
    shd.color[0] = shd.color[1] = shd.color[2] = 0.2f;
    fix = ygfx_shape(&shd);

    memset(&cd, 0, sizeof cd);
    cd.text = conditions_csv;
    cd.len = sizeof conditions_csv - 1;
    cd.arena = table_arena;
    cd.arena_size = sizeof table_arena;
    if (!ytb_csv(&tab, &cd)) { fprintf(stderr, "trial_rdk: %s\n", ytb_error(&tab)); return 1; }
    coh_col = ytb_col(&tab, "coherence");
    dir_col = ytb_col(&tab, "coherent_direction");
    key_col = ytb_col(&tab, "key_answer");
    memset(&rd, 0, sizeof rd);
    rd.table = &tab;
    rd.reps = reps;
    rd.order = YTR_ORDER_RANDOM;
    rd.rng = ytr_splitmix;
    rd.rng_ctx = &seed;
    if (!ytr_open(&trials, &rd)) { fprintf(stderr, "trial_rdk: %s\n", ytr_error(&trials)); return 1; }

    memset(&td, 0, sizeof td);
    td.events = storage;
    td.event_capacity = 64;
    td.n_channels = N_CHANNELS;
    if (!ytl_open(&tl, &td)) { fprintf(stderr, "trial_rdk: %s\n", ytl_error(&tl)); return 1; }

    memset(&sdl, 0, sizeof sdl);
    sdl.raw_keyboard = caps.raw_keyboard;
    sources[0] = yrsp_sdl_source(&sdl, YRSP_KIND_KEYBOARD);
    memset(&pd, 0, sizeof pd);
    pd.choices = keys;
    pd.n_choices = 2;
    pd.duration = RESPONSE_S;
    pd.minimum_valid_rt = MIN_RT_S;
    pd.sources = sources;
    pd.n_sources = 1;
    if (!yrsp_init(&rsp, &pd)) { fprintf(stderr, "trial_rdk: %s\n", yrsp_error(&rsp)); return 1; }

    out = fopen(out_path, "w");
    sout = fopen(steps_path, "w");
    if (!out || !sout) { fprintf(stderr, "trial_rdk: cannot write %s or %s\n", out_path, steps_path); return 1; }
    ytr_format_meta(&trials, meta, sizeof meta);
    meta[strcspn(meta, "\r\n")] = 0;
    yscr_describe(&scr, line, sizeof line);
    fprintf(out, "# %s\n# %s\n", meta, line);
    fprintf(out, "# ysp_rdk %s: w=400 count=%d speed=200 aperture=circle signal=same select=exact noise=direction "
                 "edge=wrap sets=1 lifetime=0 clock=onset stream=0; the steps in %s\n",
            yrdk_version(), N_DOTS, steps_path);
    fprintf(out, "# ysp_response %s; keyboard: %s\n", yrsp_version(), sim ? "synthetic (tier SIM)" : sources[0].note);
    fprintf(out, "trial_index,coherence,coherent_direction,key_answer,response,rt,correct,onset_tier,onset_src,"
                 "landing_residual,onset_frame,rsp_tier,n_anticipations,rdk_seed,rdk_updates,rdk_digest,frames_dropped\n");
    fprintf(sout, "trial_index,update,t,coherence,direction,speed\n");
    printf("%s\n", line);

    for (;;) {
        SDL_Event ev;
        int64_t bt = 0;
        yrsp_input in;
        const float* val;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_rdk: %s\n", yscr_error(&scr)); failed = 1; break; }
        for (i = 0; i < f.n_done; i++) {
            if (stim_frame >= 0 && f.done[i].index == stim_frame) {
                yrsp_onset o = yrsp_onset_flip(&f.done[i]);
                yrsp_set_onset(&rsp, &o);
            }
            if (first_motion_frame >= 0 && f.done[i].index >= first_motion_frame &&
                (last_motion_frame < 0 || f.done[i].index <= last_motion_frame) && f.done[i].dropped)
                dropped++;
        }

        if (!in_trial) {
            ytl_seq q;
            if (ytr_next(&trials, &ti) < 0 || ti.index >= MAX_ROWS) break;
            coherence = ytb_num(&tab, ti.condition, coh_col);
            direction = (float)ytb_num(&tab, ti.condition, dir_col);
            answer = !strcmp(ytb_text(&tab, ti.condition, key_col), "right");
            trial_seed = seed * 1000003u + (uint64_t)ti.index;   /* one field per trial, from the session's seed */
            rdk.coherence = (float)coherence;
            rdk.direction = direction;
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            ytl_on(&q, FIX_ON);
            ytl_wait(&q, YRT_S(FIXATION_S));
            ytl_on(&q, DOTS_ON);
            if (q.err) { fprintf(stderr, "trial_rdk: timeline error %d\n", q.err); failed = 1; break; }
            yrsp_arm(&rsp, f.onset);
            stim_frame = first_motion_frame = last_motion_frame = -1;
            in_trial = responding = 1;
            moving = n_steps = dropped = 0;
        }

        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) yrsp_feed(&rsp, &in);
        for (; sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset; sim_next++)
            yrsp_feed(&rsp, &sim_ev[sim_next]);

        /* the response or the deadline ends the motion on this frame */
        if (responding && yrsp_update(&rsp, f.onset) == YRSP_ENDED) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, DOTS_ON);
            ytl_off(&q, FIX_ON);
            ytl_wait(&q, YRT_S(ITI_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "trial_rdk: timeline error %d\n", q.err); failed = 1; break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 16);
        for (i = 0; i < n && i < 16; i++) {
            if (fired[i].kind == YTL_ONSET && fired[i].target == DOTS_ON) {
                yrsp_onset o = yrsp_onset_landing(&fired[i]);
                yrsp_set_onset(&rsp, &o);
                stim_frame = fired[i].frame;
                landing_residual = fired[i].residual;
                if (sim) {   /* faster at higher coherence; one wrong key */
                    int wrong = coherence < 0.05 && !answer;
                    uint32_t sc;
                    for (; sim_next < n_sim_ev; sim_next++) yrsp_feed(&rsp, &sim_ev[sim_next]);
                    sim_rt[ti.index] = 0.65 - 0.8 * coherence;
                    sim_key[ti.index] = wrong ? !answer : answer;
                    sc = (uint32_t)yrsp_scancode(sim_key[ti.index] ? "right" : "left");
                    memset(sim_ev, 0, sizeof sim_ev);
                    sim_ev[0].t = f.onset + YRT_S(sim_rt[ti.index]);
                    sim_ev[0].kind = YRSP_KIND_KEYBOARD;
                    sim_ev[0].type = YRSP_PRESS;
                    sim_ev[0].control = sc;
                    sim_ev[0].stamp = YRSP_TIER_SIM;
                    sim_ev[0].value = 1;
                    sim_ev[1] = sim_ev[0];
                    sim_ev[1].type = YRSP_RELEASE;
                    sim_ev[1].value = 0;
                    sim_ev[1].t += YRT_MS(60);
                    n_sim_ev = 2;
                    sim_next = 0;
                }
            }
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                char rt[32], lres[32];
                int correct;
                yrsp_finish(&rsp, &res);
                correct = (res.flags & YRSP_R_RESPONDED) && res.response == answer;
                sec(rt, sizeof rt, res.rt);
                sec(lres, sizeof lres, (double)landing_residual / 1e9);
                row_seed[ti.index] = trial_seed;
                row_digest[ti.index] = yrdk_digest(&rdk);
                fprintf(out, "%d,%g,%g,%s,%s,%s,%d,%u,%u,%s,%lld,%u,%d,%llu,%d,%016llx,%d\n", ti.index, coherence,
                        (double)direction, keys[answer].key, res.response_name ? res.response_name : "", rt, correct,
                        (unsigned)res.onset_tier, (unsigned)res.onset_src, lres, (long long)res.onset_frame,
                        (unsigned)res.stamp_tier, res.n_anticipations, (unsigned long long)trial_seed, n_steps,
                        (unsigned long long)row_digest[ti.index], dropped);
                fflush(out);
                printf("trial %2d coherence %.3f %-5s rt %s %s; %d updates, digest %016llx\n", ti.index, coherence,
                       keys[answer].key, rt[0] ? rt : "-", !(res.flags & YRSP_R_RESPONDED) ? "(too slow)"
                       : correct ? "correct" : "wrong", n_steps, (unsigned long long)row_digest[ti.index]);
                ytr_update(&trials, (res.flags & YRSP_R_RESPONDED) ? correct : YTR_INVALID, NULL);
                sim_res[ti.index] = res;
                rows++;
                in_trial = 0;
            }
        }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));

        /* the dots at this frame's onset, on the trial's motion clock */
        val = ytl_values(&tl);
        if (val[DOTS_ON] > 0.5f) {
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
    fclose(out);
    fclose(sout);
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d trials written to %s, their steps to %s%s\n", rows, out_path, steps_path,
           aborted ? " (stopped with Shift+Esc)" : "");

    if (sim && !failed && !aborted) {
        for (i = 0; i < rows; i++) {
            const yrsp_result* r = &sim_res[i];
            if (!(r->flags & YRSP_R_RESPONDED) || r->response != sim_key[i] || fabs(r->rt - sim_rt[i]) > 1e-6 ||
                r->onset_tier != YSCR_TIER_SIM || r->onset_src != YRSP_ONSET_FLIP) {
                fprintf(stderr, "trial_rdk: --sim: trial %d: rt %.9f (want %.9f) response %d (want %d) onset tier %d "
                                "src %d\n", i, r->rt, sim_rt[i], r->response, sim_key[i], r->onset_tier, r->onset_src);
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
