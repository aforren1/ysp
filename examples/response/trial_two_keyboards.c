/* trial_two_keyboards.c - two keyboards as two response roles: two
 * participants race to a target, each on a keyboard of their own.
 *
 * Mirrors Psychtoolbox-3's KbQueueDemo.m (a key queue per keyboard, by
 * deviceIndex) and the per-device idea of MouseTraceDemo3.m. Read for
 * coverage only; no code is copied. docs/response.md, "Run two keyboards
 * or two participants", is the how-to this program runs.
 *
 * Setup: player 1 presses a key on their keyboard, then player 2 on
 * another one. ysp/screen.h gives each keyboard on SDL's raw path its own
 * device id, so the ids are read at run time (they are Windows handles and
 * change between sessions; the console prints the names). A key with
 * device 0 (the message path, SendInput, remote desktop) cannot be told
 * apart and is refused; so is the same keyboard twice.
 * Each trial: a fixation cross for 0.4 s, a foreperiod of 0.6 to 1.2 s in
 * whole frames, then a target (a circle or a square) until both have
 * pressed their space bar or 1.5 s. One ysp/response.h collector per
 * player, fed only that keyboard's keys, so each has its own RT from the
 * target's flip onset, its own early presses (before the target) and
 * anticipations (under 0.1 s). The winner has the earlier stamp; equal
 * stamps are a tie. Keys from a third keyboard and keys with device 0 are
 * counted and ignored. Feedback for 0.6 s: the winner's bar (left for
 * player 1) green, a tie both yellow.
 * --one-keyboard: no setup; one keyboard plays both roles, F for player 1
 * and J for player 2, from any keyboard.
 *
 * The data file has one row per trial, times in seconds: trial_index,
 * target, foreperiod, onset_tier, winner, p1_rt, p2_rt, p2_minus_p1,
 * p1_device, p2_device, p1_tier, p2_tier, p1_n_early, p2_n_early,
 * p1_n_anticipations, p2_n_anticipations, n_other (presses from another
 * keyboard), n_device0, key_doubles and key_double_ups (second key reports
 * that ysp/screen.h's input bridge dropped in the trial). Shift+Esc from
 * any keyboard stops the session; the rows so far are kept.
 *
 * --sim: the simulated display and synthetic keys from three device ids,
 * through the bridge's second-report filter (yscr_key_filter): a device-0
 * key and the same keyboard twice refused at setup; then a clear win, the
 * same key on both keyboards 5 ms apart (both kept), equal stamps (a tie),
 * a press reported twice (the second key-down and its key-up dropped; that
 * key-up comes while player 2 holds the same key) beside a third keyboard's
 * press (ignored), an early press, a timeout. It checks its own results,
 * key_doubles and key_double_ups included, and exits 1 on a mismatch.
 * About 20 s.
 *
 * Usage: trial_two_keyboards [--sim] [--one-keyboard] [--fullscreen] [--reps N] [--seed N] [--out FILE]
 * Exit code: 0 (also after Shift+Esc), 1 when something did not open or a
 * --sim check failed, 2 for a bad argument.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXATION_S 0.4
#define RESPONSE_S 1.5
#define MIN_RT_S   0.1
#define FEEDBACK_S 0.6
#define NS(s) ((int64_t)((s) * 1e9 + ((s) < 0 ? -0.5 : 0.5)))

static const char conditions_csv[] = "target\ncircle\nsquare\n";

/* --sim: three keyboards, and presses at `at` s from the target's onset
 * (setup presses: from the start of setup); dev 0 is the message path */
enum { P1 = 65601, P2 = 65659, OTHER = 65599 };
typedef struct sim_press { int trial; double at; uint32_t dev; const char* key; } sim_press;
#define SETUP (-1)
static const sim_press sim_script[] = {
    { SETUP, 0.2, 0, "space" },     /* device 0: refused */
    { SETUP, 0.4, P1, "space" },
    { SETUP, 0.6, P1, "space" },    /* the same keyboard again: refused */
    { SETUP, 0.8, P2, "space" },
    { 0, 0.300, P1, "space" }, { 0, 0.350, P2, "space" },
    { 1, 0.400, P1, "space" }, { 1, 0.395, P2, "space" },          /* one key, two keyboards, 5 ms */
    { 2, 0.320, P1, "space" }, { 2, 0.320, P2, "space" },          /* equal stamps */
    { 3, 0.200, OTHER, "space" }, { 3, 0.280, P1, "space" }, { 3, 0.289, 0, "space" },  /* a second report */
    { 3, 0.300, P2, "space" },
    { 4, -0.300, P1, "space" }, { 4, 0.450, P1, "space" }, { 4, 0.500, P2, "space" },  /* early */
    { 5, 0.600, P2, "space" },                                       /* player 1 times out */
};
#define N_SIM ((int)(sizeof sim_script / sizeof sim_script[0]))
#define SIM_TRIALS 6
/* per trial: the winner (1, 2, 3 a tie, 0 none) and p2 - p1 in ms (NO_DIFF:
 * one did not respond) */
#define NO_DIFF 1e9
static const int sim_winner[SIM_TRIALS] = { 1, 2, 3, 1, 1, 2 };
static const double sim_diff_ms[SIM_TRIALS] = { 50, -5, 0, 20, 50, NO_DIFF };

static yscr_screen scr;
static ygfx_gfx gfx;
static ytr_trials trials;
static yrsp_collector rsp[2];
static uint64_t table_arena[1024];
static ytb_table tab;
static yin_event sim_ev[64];
static int n_sim_ev, sim_next;

static uint32_t stat_of(int ups) {
    yscr_input_stats st;
    yscr_get_input_stats(&st);
    return ups ? st.key_double_ups : st.key_doubles;
}

static ygfx_stim shape(ygfx_shape_kind kind, float x, float w, float h, float p0, float g) {
    ygfx_shape_desc d;
    memset(&d, 0, sizeof d);
    d.shape = kind;
    d.x = x;
    d.w = w;
    d.h = h;
    d.shape_p[0] = p0;
    d.edge = YGFX_EDGE_COSINE;
    d.edge_width = 1;
    d.color[0] = d.color[1] = d.color[2] = g;
    return ygfx_shape(&d);
}

static void set_color(ygfx_stim* s, float r, float g, float b) { s->color[0] = r; s->color[1] = g; s->color[2] = b; }

/* --sim: one press and its release 80 ms later, kept in time order */
static void sim_add(int64_t t, uint32_t dev, const char* key) {
    int k, type;
    for (type = 0; type < 2 && n_sim_ev < 64; type++) {
        yin_event e;
        memset(&e, 0, sizeof e);
        e.t = t + (type ? NS(0.080) : 0);
        e.kind = YRSP_KIND_KEYBOARD;
        e.type = type ? YRSP_RELEASE : YRSP_PRESS;
        e.device = dev;
        e.control = (uint32_t)yrsp_scancode(key);
        e.stamp = YRSP_TIER_SIM;
        e.value = type ? 0.0f : 1.0f;
        for (k = n_sim_ev; k > sim_next && sim_ev[k - 1].t > e.t; k--) sim_ev[k] = sim_ev[k - 1];
        sim_ev[k] = e;
        n_sim_ev++;
    }
}

static void sim_queue(int trial, int64_t t0) {
    int k;
    for (k = 0; k < N_SIM; k++)
        if (sim_script[k].trial == trial) sim_add(t0 + NS(sim_script[k].at), sim_script[k].dev, sim_script[k].key);
}

static void sec(char* out, size_t cap, double s) {
    if (s != s) out[0] = 0;
    else snprintf(out, cap, "%.9f", s);
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    ytb_csv_desc cd;
    ytr_desc rd;
    yrsp_desc pd;
    yrsp_choice ch[2];
    yscr_caps caps;
    yscr_frame f;
    ytr_trial_info ti = { 0 };
    yrsp_result res[2];
    ygfx_stim fix, circle, square, bar[2];
    FILE* out;
    char line[1024], meta[2048];
    const char* out_path = "trial_two_keyboards.csv";
    uint64_t seed = 20261009;
    uint32_t dev[2] = { 0, 0 }, doubles0 = 0, ups0 = 0;
    int i, p, sim = 0, one = 0, reps = 0, fullscreen = 0, aborted = 0, failed = 0, rows = 0, bad = 0;
    int state = 0, winner = 0, n_other = 0, n_dev0 = 0, refused = 0, ready = 0, sim_w[SIM_TRIALS];
    int ended[2] = { 0, 0 }, sim_early[SIM_TRIALS], sim_doubles[SIM_TRIALS], sim_ups[SIM_TRIALS],
        sim_others[SIM_TRIALS];
    double sim_d[SIM_TRIALS], sim_rt1[SIM_TRIALS];
    int64_t target_frame = -1, t_state = 0, n_frames = 0;
    int32_t rate_num, rate_den;
    enum { SETUP_1, SETUP_2, START, WAIT, FEEDBACK };

    memset(&sd, 0, sizeof sd);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--one-keyboard")) one = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_two_keyboards [--sim] [--one-keyboard] [--fullscreen] [--reps N] [--seed N] [--out FILE]\n");
            return 2;
        }
    }
    if (reps == 0) reps = sim ? SIM_TRIALS / 2 : 10;
    if (reps < 1 || reps > 100 || (sim && one)) { fprintf(stderr, "trial_two_keyboards: --reps 1..100; --sim plays two keyboards\n"); return 2; }
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = !fullscreen;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "trial_two_keyboards: %s\n", yscr_error(&scr)); return 1; }
    yscr_get_caps(&scr, &caps);
    rate_num = caps.mode.refresh_num ? caps.mode.refresh_num : 60;
    rate_den = caps.mode.refresh_num ? caps.mode.refresh_den : 1;
    if (!sim && !one && !caps.raw_keyboard)
        printf("warning: keys are not on SDL's raw path now, so they come with device 0 and cannot be told apart\n");
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "trial_two_keyboards: %s\n", ygfx_error(&gfx)); return 1; }
    fix = shape(YGFX_CROSS, 0, 16, 16, 3, 0.2f);
    circle = shape(YGFX_CIRCLE, 0, 120, 120, 0, 0.9f);
    square = shape(YGFX_RECT, 0, 106, 106, 0, 0.9f);
    bar[0] = shape(YGFX_RECT, -300, 60, 240, 0, 0.35f);
    bar[1] = shape(YGFX_RECT, 300, 60, 240, 0, 0.35f);

    memset(&cd, 0, sizeof cd);
    cd.text = conditions_csv;
    cd.len = sizeof conditions_csv - 1;
    cd.arena = table_arena;
    cd.arena_size = sizeof table_arena;
    if (!ytb_open_csv(&tab, &cd)) { fprintf(stderr, "trial_two_keyboards: %s\n", ytb_error(&tab)); return 1; }
    memset(&rd, 0, sizeof rd);
    rd.table = &tab;
    rd.reps = reps;
    rd.order = YTR_ORDER_RANDOM;
    rd.rng = ytr_splitmix;
    rd.rng_ctx = &seed;
    rd.jitters[0] = ytr_frames(ytr_uniform("foreperiod", YRT_MS(600), YRT_MS(1200)), rate_num, rate_den);
    rd.n_jitters = 1;
    if (!ytr_open(&trials, &rd)) { fprintf(stderr, "trial_two_keyboards: %s\n", ytr_error(&trials)); return 1; }

    out = fopen(out_path, "w");
    if (!out) { fprintf(stderr, "trial_two_keyboards: cannot write %s\n", out_path); return 1; }
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    if (one) { state = START; printf("one keyboard: F is player 1, J is player 2\n"); }
    else printf("player 1: press a key on your keyboard\n");
    t_state = 0;

    ygfx_prime(&gfx);   /* each kind's first draw now, not on a trial's first frames (docs/gfx.md) */
    for (;;) {
        SDL_Event ev;
        yin_event in;
        int rc = yscr_begin(&scr, &f), have;
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_two_keyboards: %s\n", yscr_error(&scr)); failed = 1; break; }
        if (!t_state) { t_state = f.onset; if (sim) sim_queue(SETUP, f.onset); }

        if (state == START && ready) {
            /* a new trial: fixation now, the target after the foreperiod */
            if (ytr_next(&trials, &ti) < 0) break;
            n_frames = (int64_t)(FIXATION_S * rate_num / rate_den + 0.5) + ytr_jitter(&trials, ti.index, 0).frames;
            target_frame = f.index + n_frames;
            for (p = 0; p < 2; p++) { yrsp_arm(&rsp[p], f.onset); ended[p] = 0; }
            doubles0 = stat_of(0);
            ups0 = stat_of(1);
            n_other = n_dev0 = 0;
            if (sim) sim_queue(ti.index, f.onset + n_frames * (f.period ? f.period : NS(1.0 / 60)));
            set_color(&bar[0], 0.35f, 0.35f, 0.35f);
            set_color(&bar[1], 0.35f, 0.35f, 0.35f);
            state = WAIT;
        }
        /* the target's flip record: its onset, final */
        for (i = 0; i < f.n_done; i++)
            if (state >= WAIT && f.done[i].index == target_frame) {
                yrsp_onset o = yrsp_onset_flip(&f.done[i]);
                for (p = 0; p < 2; p++) yrsp_set_onset(&rsp[p], &o);
            }
        if (state == WAIT && f.index == target_frame) {
            yrsp_onset o;
            memset(&o, 0, sizeof o);   /* the plan until the record comes */
            o.t_ns = f.onset;
            o.frame = f.index;
            o.src = YRSP_ONSET_PLAN;
            for (p = 0; p < 2; p++) yrsp_set_onset(&rsp[p], &o);
        }

        /* input: route each key to its player's collector */
        for (;;) {
            if (yscr_poll(&scr, &ev, NULL)) { if (!yscr_event_input(&scr, &ev, &in)) continue; }
            else if (sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset) {
                in = sim_ev[sim_next++];
                if (!yscr_key_filter(&scr, &in)) continue;   /* a second report */
            } else break;
            if (in.kind != YRSP_KIND_KEYBOARD) continue;
            if (state <= SETUP_2) {
                if (in.type != YRSP_PRESS || (in.flags & YRSP_IN_REPEAT)) continue;
                if (!in.device || (state == SETUP_2 && in.device == dev[0])) {
                    refused++;
                    printf("  refused: %s\n", in.device ? "the same keyboard as player 1" : "a key with no device id");
                    continue;
                }
                dev[state] = in.device;
                {
                    const char* nm = sim ? "synthetic" : SDL_GetKeyboardNameForID((SDL_KeyboardID)in.device);
                    printf("player %d: keyboard %u (%s)\n", state + 1, (unsigned)in.device, nm ? nm : "no name");
                }
                if (++state == SETUP_2) printf("player 2: press a key on another keyboard\n");
                else printf("each player: press your space bar when the target appears\n");
                continue;
            }
            p = one ? (in.control == (uint32_t)yrsp_scancode("f") ? 0 : in.control == (uint32_t)yrsp_scancode("j") ? 1 : -1)
                    : (!in.device ? -2 : in.device == dev[0] ? 0 : in.device == dev[1] ? 1 : -1);
            if (p < 0) {
                if (in.type == YRSP_PRESS && !(in.flags & YRSP_IN_REPEAT) && state == WAIT) { if (p == -2) n_dev0++; else n_other++; }
                continue;
            }
            yrsp_feed(&rsp[p], &in);
        }
        if (state == START && !ready) {
            /* the collectors, once the ids are known */
            ready = 1;
            memset(ch, 0, sizeof ch);
            ch[0].key = one ? "f" : "space"; ch[0].name = "p1"; ch[0].device = one ? 0 : dev[0];
            ch[1].key = one ? "j" : "space"; ch[1].name = "p2"; ch[1].device = one ? 0 : dev[1];
            memset(&pd, 0, sizeof pd);
            pd.n_choices = 1;
            pd.duration_ns = YRT_S(RESPONSE_S);
            pd.min_rt_ns = YRT_S(MIN_RT_S);
            for (p = 0; p < 2; p++) {
                pd.choices = &ch[p];
                if (!yrsp_open(&rsp[p], &pd)) { fprintf(stderr, "trial_two_keyboards: %s\n", yrsp_error(&rsp[p])); failed = 1; }
            }
            if (failed) break;
            ytr_format_meta(&trials, meta, sizeof meta);
            meta[strcspn(meta, "\r\n")] = 0;
            fprintf(out, "# %s\n# %s\n# ysp_response %s; keyboards: player 1 %u, player 2 %u%s\n", meta, line,
                    yrsp_version(), (unsigned)dev[0], (unsigned)dev[1], one ? " (one keyboard: F and J)" : sim ? " (synthetic)" : "");
            fprintf(out, "trial_index,target,foreperiod,onset_tier,winner,p1_rt,p2_rt,p2_minus_p1,p1_device,p2_device,"
                         "p1_tier,p2_tier,p1_n_early,p2_n_early,p1_n_anticipations,p2_n_anticipations,n_other,n_device0,"
                         "key_doubles,key_double_ups\n");
        }

        if (state == WAIT && f.index >= target_frame) {
            for (p = 0; p < 2; p++) if (!ended[p] && yrsp_update(&rsp[p], f.onset) == YRSP_ENDED) ended[p] = 1;
            if (ended[0] && ended[1]) {
                /* the race is over: the winner by the stamps, for the feedback */
                for (p = 0; p < 2; p++) yrsp_finish(&rsp[p], &res[p]);
                have = ((res[0].flags & YRSP_R_RESPONDED) ? 1 : 0) | ((res[1].flags & YRSP_R_RESPONDED) ? 2 : 0);
                winner = have == 3 ? (res[0].t_response < res[1].t_response ? 1 : res[1].t_response < res[0].t_response ? 2 : 3)
                                   : have;
                if (winner == 1 || winner == 3) set_color(&bar[0], winner == 3 ? 0.8f : 0.1f, 0.7f, 0.1f);
                if (winner == 2 || winner == 3) set_color(&bar[1], winner == 3 ? 0.8f : 0.1f, 0.7f, 0.1f);
                t_state = f.onset;
                state = FEEDBACK;
            }
        }
        if (state == FEEDBACK && f.onset >= t_state + NS(FEEDBACK_S)) {
            char rt1[32], rt2[32], diff[32];
            static const char* names[] = { "none", "p1", "p2", "tie" };
            int dbl = (int)(stat_of(0) - doubles0), dbu = (int)(stat_of(1) - ups0);
            for (p = 0; p < 2; p++) yrsp_finish(&rsp[p], &res[p]);   /* final: the onset is the record's */
            sec(rt1, sizeof rt1, res[0].rt_ns == YRSP_NO_TIME ? NAN : YRT_TO_S(res[0].rt_ns));
            sec(rt2, sizeof rt2, res[1].rt_ns == YRSP_NO_TIME ? NAN : YRT_TO_S(res[1].rt_ns));
            sec(diff, sizeof diff, (winner == 3 || (res[0].flags & res[1].flags & YRSP_R_RESPONDED))
                                       ? (double)(res[1].t_response - res[0].t_response) / 1e9 : NAN);
            fprintf(out, "%d,%s,%.9f,%u,%s,%s,%s,%s,%u,%u,%u,%u,%d,%d,%d,%d,%d,%d,%d,%d\n", ti.index,
                    ytb_text(&tab, ti.condition, 0), YRT_TO_S(ytr_jitter(&trials, ti.index, 0).ns), (unsigned)res[0].onset_tier,
                    names[winner], rt1, rt2, diff, (unsigned)dev[0], (unsigned)dev[1], (unsigned)res[0].stamp_tier,
                    (unsigned)res[1].stamp_tier, res[0].n_early, res[1].n_early, res[0].n_anticipations,
                    res[1].n_anticipations, n_other, n_dev0, dbl, dbu);
            fflush(out);
            printf("trial %2d winner %-4s rt p1 %s p2 %s, p2 - p1 %s; other keyboards %d, device 0 %d, doubles %d\n",
                   ti.index, names[winner], rt1[0] ? rt1 : "-", rt2[0] ? rt2 : "-", diff[0] ? diff : "-", n_other, n_dev0, dbl);
            ytr_update(&trials, winner, NULL);
            if (ti.index < SIM_TRIALS) {
                sim_w[ti.index] = winner;
                sim_d[ti.index] = diff[0] ? atof(diff) * 1e3 : NO_DIFF;
                sim_rt1[ti.index] = res[0].rt_ns == YRSP_NO_TIME ? NAN : YRT_TO_S(res[0].rt_ns);
                sim_early[ti.index] = res[0].n_early + res[1].n_early;
                sim_doubles[ti.index] = dbl;
                sim_ups[ti.index] = dbu;
                sim_others[ti.index] = n_other;
            }
            rows++;
            state = START;
        }

        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        if (state <= SETUP_2) {
            set_color(&bar[state], 0.95f, 0.95f, 0.95f);
            set_color(&bar[!state], 0.35f, 0.35f, 0.35f);
        }
        ygfx_begin(&gfx, &f);
        if (state <= SETUP_2 || state == FEEDBACK) { ygfx_draw(&gfx, &bar[0]); ygfx_draw(&gfx, &bar[1]); }
        if (state == WAIT) ygfx_draw(&gfx, f.index >= target_frame ? (ti.condition ? &square : &circle) : &fix);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    fclose(out);
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d trials written to %s%s\n", rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");

    if (sim && !failed && reps != SIM_TRIALS / 2) printf("--sim: the self-check needs the default --reps\n");
    else if (sim && !failed) {
        if (rows != SIM_TRIALS || dev[0] != P1 || dev[1] != P2 || refused != 2) {
            fprintf(stderr, "trial_two_keyboards: --sim: %d rows, keyboards %u and %u, %d refused at setup\n", rows,
                    (unsigned)dev[0], (unsigned)dev[1], refused);
            bad = 1;
        }
        for (i = 0; i < rows && i < SIM_TRIALS; i++) {
            int ok = sim_w[i] == sim_winner[i] && sim_early[i] == (i == 4) && sim_doubles[i] == (i == 3) && sim_ups[i] == (i == 3) &&
                     sim_others[i] == (i == 3) && fabs(sim_d[i] - sim_diff_ms[i]) < 1e-3 &&
                     (i != 0 || fabs(sim_rt1[i] - 0.300) < 1e-6) && (i != 4 || fabs(sim_rt1[i] - 0.450) < 1e-6);
            if (!ok) {
                fprintf(stderr, "trial_two_keyboards: --sim: trial %d: winner %d, p2 - p1 %.6f ms, early %d, doubles %d, "
                                "double ups %d, others %d\n", i, sim_w[i], sim_d[i], sim_early[i], sim_doubles[i],
                        sim_ups[i], sim_others[i]);
                bad = 1;
            }
        }
        if (bad) return 1;
        printf("--sim: every trial came out as scripted\n");
    }
    return failed;
}
