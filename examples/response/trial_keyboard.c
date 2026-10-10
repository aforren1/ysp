/* trial_keyboard.c - the jsPsych html-keyboard-response trial as a small
 * experiment: ysp/trials.h, ysp/timeline.h, ysp/gfx.h and ysp/response.h.
 *
 * Each trial: a fixation cross for 0.5 s, then a circle or a square until
 * a response or 1.5 s; F for the circle, J for the square (the physical
 * keys: they do not move with the keyboard layout). Responses before 0.1
 * s are anticipations: logged, not responses. Feedback for 0.4 s (a green
 * ring when correct, red when wrong, gray when too slow), then a blank
 * inter-trial interval drawn from 0.8 to 1.2 s in whole frames.
 *
 * The conditions are a CSV string (stimulus,key_answer), read by
 * ysp/table.h and run by ysp/trials.h in a random order. RT runs from the
 * stimulus's flip onset (the OS's vblank time of the frame it landed on),
 * not from when the program started listening, and the data row says how
 * good that onset is (its tier) and how far the stimulus landed from its
 * planned time (the landing residual).
 *
 * The data file has one row per trial. Columns with jsPsych's names where
 * they match: trial_index, stimulus, key_answer, response, rt, correct,
 * rt_key_duration; then ysp's: onset_tier, onset_src, landing_residual,
 * onset_frame, rsp_tier, rsp_flags, n_anticipations, n_duplicates (second
 * key reports that ysp/screen.h's input bridge dropped during the trial),
 * iti. Times in seconds. A '#' line before the header has the session's
 * settings. Shift+Esc stops the session; the rows so far are kept.
 *
 * --sim runs on the simulated display with a synthetic participant (no
 * keyboard): a correct response, an anticipation then a response, a wrong
 * key reported twice 9 ms apart (the pattern SDL 3.4 gives for a
 * virtual-key tap, docs/response.md; the bridge's filter drops the
 * second), a timeout, and two more correct
 * responses. It checks its own results and exits 1 on a mismatch. CI runs
 * it. The simulated display runs on the real clock: about 15 s.
 *
 * Usage: trial_keyboard [--sim] [--fullscreen] [--reps N] [--seed N] [--out FILE]
 *   --reps N   repetitions of each condition (default 10; --sim: 3, which
 *              its self-check needs)
 *   --out FILE the data file (default trial_keyboard.csv)
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

/* timeline channels: what is visible */
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

static yscr_screen scr;
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;                  /* about 100 KB: not the stack */
static yrsp_collector rsp;
static uint64_t table_arena[2048];
static ytb_table tab;

static ygfx_stim fix, circle, square, fb_right, fb_wrong, fb_none;
static const ygfx_bind binds[] = {
    { .stim = &fix,      .param = YGFX_P_VISIBLE, .channel = FIX_ON    },
    { .stim = &circle,   .param = YGFX_P_VISIBLE, .channel = CIRCLE_ON },
    { .stim = &square,   .param = YGFX_P_VISIBLE, .channel = SQUARE_ON },
    { .stim = &fb_right, .param = YGFX_P_VISIBLE, .channel = FB_RIGHT  },
    { .stim = &fb_wrong, .param = YGFX_P_VISIBLE, .channel = FB_WRONG  },
    { .stim = &fb_none,  .param = YGFX_P_VISIBLE, .channel = FB_NONE   },
};

/* The bridge's count of dropped second key reports (a process total). */
static uint32_t key_doubles(void) {
    yscr_input_stats st;
    yscr_get_input_stats(&st);
    return st.key_doubles;
}

static ygfx_stim shape(ygfx_shape_kind kind, float w, float p0, float r, float g, float b) {
    ygfx_shape_desc d;
    memset(&d, 0, sizeof d);
    d.shape = kind;
    d.w = d.h = w;
    d.shape_p[0] = p0;
    d.edge = YGFX_EDGE_COSINE;
    d.edge_width = 1;
    d.color[0] = r; d.color[1] = g; d.color[2] = b;
    return ygfx_shape(&d);
}

static void sec(char* out, size_t cap, double s) {
    if (s != s) out[0] = 0;
    else snprintf(out, cap, "%.9f", s);
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
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
    yrsp_result res, sim_res[SIM_TRIALS];
    int sim_doubles[SIM_TRIALS];
    uint32_t doubles_at_arm = 0;
    yrsp_input sim_ev[16];
    FILE* out;
    char line[1024], meta[2048];
    const char* out_path = "trial_keyboard.csv";
    uint64_t seed = 20261007;
    int i, n, sim = 0, reps = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0;
    int in_trial = 0, responding = 0, n_sim_ev = 0, sim_next = 0, stim_ch = 0, stim_col, key_col;
    int64_t stim_frame = -1, landing_residual = 0, iti_ns = 0;
    int32_t rate_num, rate_den;
    char key_answer[8] = "";

    memset(&sd, 0, sizeof sd);
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
    if (reps == 0) reps = sim ? SIM_TRIALS / 2 : 10;
    if (reps < 1 || reps > 100) { fprintf(stderr, "trial_keyboard: --reps takes 1..100\n"); return 2; }
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = !fullscreen;

    /* the display first: the inter-trial interval snaps to its rate */
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "trial_keyboard: %s\n", yscr_error(&scr)); return 1; }
    yscr_get_caps(&scr, &caps);
    rate_num = caps.mode.refresh_num ? caps.mode.refresh_num : 60;
    rate_den = caps.mode.refresh_num ? caps.mode.refresh_den : 1;
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "trial_keyboard: %s\n", ygfx_error(&gfx)); return 1; }
    fix = shape(YGFX_CROSS, 16, 3, 0.2f, 0.2f, 0.2f);
    circle = shape(YGFX_CIRCLE, 120, 0, 0.9f, 0.9f, 0.9f);
    square = shape(YGFX_RECT, 106, 0, 0.9f, 0.9f, 0.9f);
    fb_right = shape(YGFX_ANNULUS, 60, 24, 0.1f, 0.7f, 0.1f);
    fb_wrong = shape(YGFX_ANNULUS, 60, 24, 0.8f, 0.1f, 0.1f);
    fb_none = shape(YGFX_ANNULUS, 60, 24, 0.35f, 0.35f, 0.35f);

    /* the conditions and the order */
    memset(&cd, 0, sizeof cd);
    cd.text = conditions_csv;
    cd.len = sizeof conditions_csv - 1;
    cd.arena = table_arena;
    cd.arena_size = sizeof table_arena;
    if (!ytb_csv(&tab, &cd)) { fprintf(stderr, "trial_keyboard: %s\n", ytb_error(&tab)); return 1; }
    stim_col = ytb_col(&tab, "stimulus");
    key_col = ytb_col(&tab, "key_answer");
    memset(&rd, 0, sizeof rd);
    rd.table = &tab;
    rd.reps = reps;
    rd.order = YTR_ORDER_RANDOM;
    rd.rng = ytr_splitmix;
    rd.rng_ctx = &seed;
    rd.jitters[0] = ytr_frames(ytr_uniform("iti", 0.8, 1.2), rate_num, rate_den);
    rd.n_jitters = 1;
    if (!ytr_open(&trials, &rd)) { fprintf(stderr, "trial_keyboard: %s\n", ytr_error(&trials)); return 1; }

    /* the timeline: one base for the trial */
    memset(&td, 0, sizeof td);
    td.events = storage;
    td.event_capacity = 64;
    td.n_channels = N_CHANNELS;
    if (!ytl_open(&tl, &td)) { fprintf(stderr, "trial_keyboard: %s\n", ytl_error(&tl)); return 1; }

    /* the response: f or j, within 1.5 s of the onset, none before 0.1 s */
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
    if (!yrsp_init(&rsp, &pd)) { fprintf(stderr, "trial_keyboard: %s\n", yrsp_error(&rsp)); return 1; }

    out = fopen(out_path, "w");
    if (!out) { fprintf(stderr, "trial_keyboard: cannot write %s\n", out_path); return 1; }
    ytr_format_meta(&trials, meta, sizeof meta);
    meta[strcspn(meta, "\r\n")] = 0;           /* one line under the '#' */
    yscr_describe(&scr, line, sizeof line);
    fprintf(out, "# %s\n# %s\n# ysp_response %s; keyboard: %s\n", meta, line, yrsp_version(),
            sim ? "synthetic (tier SIM)" : sources[0].note);
    fprintf(out, "trial_index,stimulus,key_answer,response,rt,correct,rt_key_duration,onset_tier,onset_src,"
                 "landing_residual,onset_frame,rsp_tier,rsp_flags,n_anticipations,n_duplicates,iti\n");
    printf("%s\n", line);

    for (;;) {
        SDL_Event ev;
        int64_t bt = 0;
        yrsp_input in;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_keyboard: %s\n", yscr_error(&scr)); failed = 1; break; }

        /* the stimulus frame's record: its onset, final */
        for (i = 0; i < f.n_done; i++)
            if (stim_frame >= 0 && f.done[i].index == stim_frame) {
                yrsp_onset o = yrsp_onset_flip(&f.done[i]);
                yrsp_set_onset(&rsp, &o);
            }

        if (!in_trial) {
            /* a new trial: fixation now, the stimulus 0.5 s later */
            ytl_seq q;
            const char* ka;
            if (ytr_next(&trials, &ti) < 0) break;
            ka = ytb_text(&tab, ti.condition, key_col);
            snprintf(key_answer, sizeof key_answer, "%s", ka ? ka : "");
            stim_ch = !strcmp(ytb_text(&tab, ti.condition, stim_col), "circle") ? CIRCLE_ON : SQUARE_ON;
            iti_ns = ytr_jitter(&trials, ti.index, 0).ns;
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            ytl_on(&q, FIX_ON);
            ytl_wait(&q, YRT_S(FIXATION_S));
            ytl_off(&q, FIX_ON);
            ytl_on(&q, stim_ch);
            if (q.err) { fprintf(stderr, "trial_keyboard: timeline error %d\n", q.err); failed = 1; break; }
            yrsp_arm(&rsp, f.onset);
            doubles_at_arm = key_doubles();
            stim_frame = -1;
            in_trial = responding = 1;
            n_sim_ev = sim_next = 0;
        }

        /* input: every event, so held keys are known between trials too.
         * yscr_event_input() reads the key path per event (another library
         * can turn SDL text input on) and drops second key reports; the
         * synthetic participant's go through the same filter. */
        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) yrsp_feed(&rsp, &in);
        for (; sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset; sim_next++)
            if (yscr_key_filter(&scr, &sim_ev[sim_next])) yrsp_feed(&rsp, &sim_ev[sim_next]);

        /* the end of the response window: hide the stimulus on this frame */
        if (responding && yrsp_update(&rsp, f.onset) == YRSP_ENDED) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            int fb;
            yrsp_finish(&rsp, &res);
            fb = !(res.flags & YRSP_R_RESPONDED) ? FB_NONE
                 : res.response_name && !strcmp(res.response_name, key_answer) ? FB_RIGHT : FB_WRONG;
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, stim_ch);
            ytl_on(&q, fb);
            ytl_wait(&q, YRT_S(FEEDBACK_S));
            ytl_off(&q, fb);
            ytl_wait(&q, iti_ns);
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "trial_keyboard: timeline error %d\n", q.err); failed = 1; break; }
            responding = 0;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 16);
        for (i = 0; i < n && i < 16; i++) {
            if (fired[i].kind == YTL_ONSET && fired[i].target == stim_ch) {
                /* the stimulus landed on this frame: its planned onset */
                yrsp_onset o = yrsp_onset_landing(&fired[i]);
                yrsp_set_onset(&rsp, &o);
                stim_frame = fired[i].frame;
                landing_residual = fired[i].residual;
                if (sim) {   /* the participant's presses, from this onset */
                    int k;
                    n_sim_ev = 0;
                    for (k = 0; k < (int)(sizeof sim_script / sizeof sim_script[0]); k++) {
                        const sim_press* p = &sim_script[k];
                        uint32_t sc = (uint32_t)yrsp_scancode(p->which == 1 ? key_answer
                                                                : !strcmp(key_answer, "f") ? "j" : "f");
                        int rep;
                        if (p->trial != ti.index) continue;
                        for (rep = 0; rep <= p->twice && n_sim_ev + 2 <= 16; rep++) {
                            yrsp_input* e = &sim_ev[n_sim_ev++];
                            memset(e, 0, sizeof *e);
                            e->t = f.onset + YRT_S(p->at) + (int64_t)rep * YRT_MS(9);
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
            }
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                /* the trial is over: the record of its stimulus is in */
                char rt[32], dur[32], lres[32];
                int correct;
                yrsp_finish(&rsp, &res);
                correct = (res.flags & YRSP_R_RESPONDED) && res.response_name &&
                          !strcmp(res.response_name, key_answer);
                sec(rt, sizeof rt, res.rt);
                sec(dur, sizeof dur, res.rt_key_duration);
                sec(lres, sizeof lres, (double)landing_residual / 1e9);
                fprintf(out, "%d,%s,%s,%s,%s,%d,%s,%u,%u,%s,%lld,%u,%u,%d,%d,%.9f\n", ti.index,
                        ytb_text(&tab, ti.condition, stim_col), key_answer,
                        res.response_name ? res.response_name : "", rt, correct, dur, (unsigned)res.onset_tier,
                        (unsigned)res.onset_src, lres, (long long)res.onset_frame, (unsigned)res.stamp_tier,
                        (unsigned)res.flags, res.n_anticipations, (int)(key_doubles() - doubles_at_arm),
                        (double)iti_ns / 1e9);
                fflush(out);
                printf("trial %2d %-6s rt %s response %s %s\n", ti.index, ytb_text(&tab, ti.condition, stim_col),
                       rt[0] ? rt : "-", res.response_name ? res.response_name : "-",
                       !(res.flags & YRSP_R_RESPONDED) ? "(too slow)" : correct ? "correct" : "wrong");
                ytr_update(&trials, (res.flags & YRSP_R_RESPONDED) ? correct : YTR_INVALID, NULL);
                if (ti.index < SIM_TRIALS) {
                    sim_res[ti.index] = res;
                    sim_doubles[ti.index] = (int)(key_doubles() - doubles_at_arm);
                }
                rows++;
                in_trial = 0;
            }
        }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &fix);
        ygfx_draw(&gfx, &circle);
        ygfx_draw(&gfx, &square);
        ygfx_draw(&gfx, &fb_right);
        ygfx_draw(&gfx, &fb_wrong);
        ygfx_draw(&gfx, &fb_none);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    fclose(out);
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");

    if (sim && !failed && reps != SIM_TRIALS / 2) printf("--sim: the self-check needs the default --reps\n");
    else if (sim && !failed) {
        /* the synthetic participant's trials must come out as scripted */
        int bad = 0;
        if (rows != SIM_TRIALS) { fprintf(stderr, "trial_keyboard: --sim: %d rows, not %d\n", rows, SIM_TRIALS); bad = 1; }
        for (i = 0; i < rows && i < SIM_TRIALS; i++) {
            const yrsp_result* r = &sim_res[i];
            double want_rt = i == 0 ? 0.45 : i == 1 ? 0.52 : i == 2 ? 0.40 : i == 4 ? 0.38 : i == 5 ? 0.61 : NAN;
            int ok = 1;
            if (i == 3) ok = !(r->flags & YRSP_R_RESPONDED) && (r->flags & YRSP_R_TIMEOUT);
            else ok = (r->flags & YRSP_R_RESPONDED) && fabs(r->rt - want_rt) < 1e-6;
            if (i == 0) ok = ok && fabs(r->rt_key_duration - 0.1) < 1e-6;
            if (i == 1) ok = ok && r->n_anticipations == 1;
            if (i == 2) ok = ok && sim_doubles[i] == 1 && r->n_responses == 1;
            if (i != 3) ok = ok && r->onset_tier == YSCR_TIER_SIM && r->onset_src == YRSP_ONSET_FLIP &&
                             r->stamp_tier == YRSP_TIER_SIM;
            if (!ok) {
                fprintf(stderr, "trial_keyboard: --sim: trial %d: rt %.9f flags 0x%x n_anticipations %d "
                                "n_duplicates %d onset tier %d src %d\n", i, r->rt, (unsigned)r->flags,
                        r->n_anticipations, sim_doubles[i], r->onset_tier, r->onset_src);
                bad = 1;
            }
        }
        if (bad) return 1;
        printf("--sim: every trial came out as scripted\n");
    }
    return failed;
}
