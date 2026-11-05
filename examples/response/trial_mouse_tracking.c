/* trial_mouse_tracking.c - a mouse-tracking trial (MouseTracker style) as
 * a small experiment, with what jsPsych's extension-mouse-tracking
 * records: ysp/trials.h, ysp/timeline.h, ysp/gfx.h and ysp/response.h.
 *
 * Each trial: click the gray start button at the bottom. A red or blue
 * disc appears at the center, with two boxes in the top corners: red on
 * the left, blue on the right. Move to the box of the disc's color and
 * click in it. A blank interval of 1 s follows.
 *
 * The pointer is sampled per event, not per frame: every mouse event
 * between the start click and the response click goes into the
 * collector's trace (desc.trace). The movement onset is a DISTANCE
 * crossing of 10 px from where the pointer was at the start click (the
 * collector's choice; desc.persist keeps the window open after it). The
 * click in a box ends the trial: the hit test is here.
 *
 * Two pointer paths:
 *   default      SDL's pointer: window coordinates with the OS's pointer
 *                acceleration, one pointer for every mouse, stamped with
 *                the window message's time (tier 3).
 *   --raw-mice   ysp/screen.h's raw mice (Windows): each report stamped
 *                when read, in counts without acceleration. A drawn
 *                cursor integrates them at 1 px per count (yrsp_cursor);
 *                the system cursor is hidden. Use it where the path's
 *                timing or its unaccelerated shape matters.
 *
 * Two data files. The trial file (--out) has one row per trial:
 * trial_index, color, answer, response, correct, init_time_s (movement
 * onset from the disc's flip onset), rt_s (the response click from the
 * same onset), max_deviation (px from the straight line, start to click),
 * n_samples; then onset_tier, onset_src, init_tier, rsp_flags, early_move
 * (1 when the pointer moved 10 px before the disc's onset: that crossing
 * is EARLY, and the next one needs a return to within 10 px of the start,
 * so init_time_s is empty). The trajectory file (--traj) has one row per
 * sample: trial_index, t_s (from the onset), x, y (px from the window's
 * top-left). The header lines give
 * the targets' boxes and the pointer path. Times in seconds. Shift+Esc
 * stops the session; the rows so far are kept.
 *
 * --sim runs 2 trials on the simulated display with a synthetic pointer:
 * a straight path to the correct box, and a path curved toward the other
 * box first. With --raw-mice too, it feeds the same paths as raw counts
 * through the drawn cursor. It checks its own results and exits 1 on a
 * mismatch. Under a second: the virtual clock does not wait.
 *
 * Usage: trial_mouse_tracking [--sim] [--raw-mice] [--reps N] [--seed N] [--out FILE] [--traj FILE]
 *   --reps N   repetitions of each color (default 10; --sim: 1)
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

enum { START_ON, STIM_ON, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };
enum { WAIT_START, MOVING, DONE };

#define ITI_S       1.0
#define ONSET_PX    10.0f         /* movement onset: this far from the start */
#define TRACE_CAP   16384         /* 16 s at 1 kHz; 768 KB */

static const char conditions_csv[] =
    "color,answer\n"
    "red,left\n"
    "blue,right\n";

/* The movement onset: the pointer 10 px from where it was at arm(). */
static const yrsp_choice moved[] = { { .name = "moved", .kind = YRSP_KIND_MOUSE, .cross = YRSP_CROSS_DISTANCE,
                                       .control = YRSP_AXIS_POSITION, .level = ONSET_PX } };

static yscr_screen scr;
/* SDL calls on the window go through yscr_window_call(): with the window
 * thread (ysp/screen.h, WINDOW THREAD) another thread owns the window. */
static void grab_on_window(void* unused) {
    (void)unused;
    SDL_HideCursor();
    SDL_SetWindowMouseGrab(yscr_window(&scr), true);
}
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static yrsp_collector rsp;
static yrsp_input trace[TRACE_CAP];
static uint64_t table_arena[1024];
static ytb_table tab;
static yrsp_cursor cursor;
static int raw, phase, chosen;        /* chosen: 0 left, 1 right */
static int64_t t_click;
static yrsp_result res;

static ygfx_stim start, disc, boxes[2], dot;
static const ygfx_bind binds[] = {
    { .stim = &start,    .param = YGFX_P_VISIBLE, .channel = START_ON },
    { .stim = &disc,     .param = YGFX_P_VISIBLE, .channel = STIM_ON  },
    { .stim = &boxes[0], .param = YGFX_P_VISIBLE, .channel = STIM_ON  },
    { .stim = &boxes[1], .param = YGFX_P_VISIBLE, .channel = STIM_ON  },
};

static ygfx_stim shape(ygfx_shape_kind kind, float x, float y, float w, float h, float stroke, float r, float g,
                       float b) {
    ygfx_shape_desc d;
    memset(&d, 0, sizeof d);
    d.shape = kind;
    d.x = x;
    d.y = y;
    d.w = w;
    d.h = h;
    d.stroke = stroke;
    d.color[0] = r; d.color[1] = g; d.color[2] = b;
    return ygfx_shape(&d);
}

/* A point in a box's bounds: ygfx_hit() of an outlined box is its band. */
static int inside(const ygfx_stim* s, float x, float y) {
    float x0, y0, x1, y1;
    ygfx_bounds(&gfx, s, &x0, &y0, &x1, &y1);
    return s->visible >= 0.5f && x >= x0 && x < x1 && y >= y0 && y < y1;
}

/* Every pointer event, real or synthetic. With raw mice the movement is in
 * counts: the drawn cursor integrates it and the collector gets the
 * position, so the DISTANCE crossing and the trace read pixels on both
 * paths. A click acts by phase: on the start button it arms the window
 * (at the click's own time, so the samples after it in this frame's batch
 * are in the trace); in a box it ends the window. */
static void pointer(const yrsp_input* ev, int from_bridge) {
    yrsp_input in = *ev;
    if (in.kind != YRSP_KIND_MOUSE || from_bridge != raw) return;
    if (raw) {
        if (in.type == YRSP_SAMPLE && !yrsp_cursor_feed(&cursor, &in)) return;
        if (in.type == YRSP_SAMPLE) in.control = YRSP_AXIS_POSITION;
        in.x = cursor.x;
        in.y = cursor.y;
    }
    if (in.type == YRSP_PRESS && in.control == 1 && phase == WAIT_START && ygfx_hit(&gfx, &start, in.x, in.y)) {
        yrsp_arm(&rsp, in.t);
        phase = MOVING;
    }
    yrsp_feed(&rsp, &in);
    if (in.type == YRSP_PRESS && in.control == 1 && phase == MOVING &&
        (inside(&boxes[0], in.x, in.y) || inside(&boxes[1], in.x, in.y))) {
        chosen = inside(&boxes[1], in.x, in.y);
        t_click = in.t;
        yrsp_finish(&rsp, &res);           /* the trace stops here */
        phase = DONE;
    }
}

/* The synthetic pointer of --sim: to the start button (raw: from where the
 * drawn cursor is), a click, then 0.3
 * s later 0.5 s of movement to the box, one sample per 8 ms; trial 1
 * bends toward the other box first (a quadratic Bezier). A click in the
 * box 0.1 s after the movement. Raw counts are the rounded differences. */
#define SIM_N 128
static yrsp_input sim_ev[SIM_N];
static int sim_path(int64_t t0, int trial, float sx, float sy, float bx, float by, float ox) {
    int n = 0, k, px = (int)lroundf(cursor.x), py = (int)lroundf(cursor.y);
    int64_t tc = t0 + YRT_MS(300);
    yrsp_input e;
    memset(&e, 0, sizeof e);
    e.kind = YRSP_KIND_MOUSE;
    e.stamp = YRSP_TIER_SIM;
    e.device = raw ? 7 : 0;
    for (k = 0; k <= 64 && n + 3 < SIM_N; k++) {
        float u = (float)k / 64, cx = trial == 1 ? ox : 0.5f * (sx + bx), cy = 0.5f * (sy + by);
        float x = k == 0 ? sx : (1 - u) * (1 - u) * sx + 2 * u * (1 - u) * cx + u * u * bx;
        float y = k == 0 ? sy : (1 - u) * (1 - u) * sy + 2 * u * (1 - u) * cy + u * u * by;
        if (k == 1) {   /* the start click, between the first two samples */
            e.type = YRSP_PRESS; e.control = 1; e.value = 1; e.x = sx; e.y = sy; e.t = tc;
            sim_ev[n++] = e;
            e.type = YRSP_RELEASE; e.value = 0; e.t = tc + YRT_MS(80);
            sim_ev[n++] = e;
        }
        e.type = YRSP_SAMPLE;
        e.t = k == 0 ? t0 + YRT_MS(100) : tc + YRT_MS(300) + (k - 1) * YRT_MS(8);
        e.control = raw ? YRSP_AXIS_DELTA : YRSP_AXIS_POSITION;
        e.value = 0;
        if (raw) {
            e.x = (float)((int)lroundf(x) - px);
            e.y = (float)((int)lroundf(y) - py);
            px += (int)e.x;
            py += (int)e.y;
        } else {
            e.x = x;
            e.y = y;
        }
        sim_ev[n++] = e;
    }
    e.type = YRSP_PRESS; e.control = 1; e.value = 1; e.x = bx; e.y = by; e.t = sim_ev[n - 1].t + YRT_MS(100);
    sim_ev[n++] = e;
    e.type = YRSP_RELEASE; e.value = 0; e.t += YRT_MS(80);
    sim_ev[n++] = e;
    return n;
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ytb_csv_desc cd;
    ytr_desc rd;
    yrsp_desc pd;
    yrsp_source sources[1];
    yscr_frame f;
    ytl_event fired[64];                   /* the timeline's capacity: all that can fire */
    ytr_trial_info ti = { 0 };
    FILE *out, *traj;
    char line[1024];
    const char* out_path = "trial_mouse_tracking.csv";
    const char* traj_path = "trial_mouse_tracking_trajectory.csv";
    uint64_t seed = 20261008, seed0;
    int i, n, sim = 0, reps = 0, aborted = 0, failed = 0, rows = 0, in_trial = 0, armed_seen = 0, ended_seen = 0;
    int n_sim_ev = 0, sim_next = 0, color_col, answer_col;
    float W, H, cx, cy;
    double sim_init[2], sim_dev[2];
    int64_t sim_t0[2] = { 0, 0 }, sim_rt_ns[2] = { 0, 0 }, sim_onset[2] = { 0, 0 };
    int sim_correct[2], sim_samples[2];

    memset(&sd, 0, sizeof sd);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--raw-mice")) raw = 1;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--traj") && i + 1 < argc) traj_path = argv[++i];
        else {
            fprintf(stderr, "usage: trial_mouse_tracking [--sim] [--raw-mice] [--reps N] [--seed N] [--out FILE] [--traj FILE]\n");
            return 2;
        }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    seed0 = seed;
    if (reps == 0) reps = sim ? 1 : 10;
    if (reps < 1 || reps > 100) { fprintf(stderr, "trial_mouse_tracking: --reps takes 1..100\n"); return 2; }
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = true;
    sd.window_w = 1024;
    sd.window_h = 768;
    sd.raw_mice = raw && !sim;    /* the simulated display has no window: --sim feeds counts itself */

    /* the display and the graphics, opened together and primed */
    if (!ygfx_open_screen(&gfx, &scr, &sd, &(ygfx_desc){ .background = { 0.5f, 0.5f, 0.5f } })) {
        fprintf(stderr, "trial_mouse_tracking: %s\n", ygfx_error(&gfx));
        return 1;
    }
    ygfx_size(&gfx, &W, &H);
    ygfx_center(&gfx, &cx, &cy);
    start = shape(YGFX_RECT, 0, H / 2 - 50, 120, 50, 0, 0.75f, 0.75f, 0.75f);
    boxes[0] = shape(YGFX_RECT, -(W / 2 - 110), -(H / 2 - 80), 180, 120, 6, 0.8f, 0.15f, 0.1f);
    boxes[1] = shape(YGFX_RECT, W / 2 - 110, -(H / 2 - 80), 180, 120, 6, 0.1f, 0.25f, 0.9f);
    disc = shape(YGFX_CIRCLE, 0, 0, 80, 80, 0, 0, 0, 0);
    dot = shape(YGFX_CIRCLE, 0, 0, 10, 10, 0, 1, 1, 1);
    dot.place = YGFX_TOP_LEFT;      /* the cursor is in window pixels */
    cursor.w = W;
    cursor.h = H;
    cursor.gain = 1;
    cursor.x = cx;
    cursor.y = cy + H / 2 - 50;
    if (raw && !sim) {
        /* the hidden system cursor still moves, with acceleration, and a
         * click goes to whatever window is under it: keep it in ours */
        yscr_window_call(&scr, grab_on_window, NULL);
    }

    memset(&cd, 0, sizeof cd);
    cd.text = conditions_csv;
    cd.len = sizeof conditions_csv - 1;
    cd.arena = table_arena;
    cd.arena_size = sizeof table_arena;
    if (!ytb_open_csv(&tab, &cd)) { fprintf(stderr, "trial_mouse_tracking: %s\n", ytb_error(&tab)); return 1; }
    color_col = ytb_col(&tab, "color");
    answer_col = ytb_col(&tab, "answer");
    memset(&rd, 0, sizeof rd);
    rd.table = &tab;
    rd.reps = reps;
    rd.order = YTR_ORDER_RANDOM;
    rd.rng = ytr_splitmix;
    rd.rng_ctx = &seed;
    if (!ytr_open(&trials, &rd)) { fprintf(stderr, "trial_mouse_tracking: %s\n", ytr_error(&trials)); return 1; }

    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS })) {
        fprintf(stderr, "trial_mouse_tracking: %s\n", ytl_error(&tl));
        return 1;
    }

    /* the movement onset, then the window stays open for the trace until
     * the click in a box */
    sources[0] = raw ? yrsp_raw_mouse_source() : yrsp_sdl_source(NULL, YRSP_KIND_MOUSE);
    memset(&pd, 0, sizeof pd);
    pd.choices = moved;
    pd.n_choices = 1;
    pd.persist = true;
    pd.sources = sources;
    pd.n_sources = 1;
    pd.trace = trace;
    pd.trace_cap = TRACE_CAP;
    pd.trace_kinds = YRSP_KIND_BIT(YRSP_KIND_MOUSE);
    if (!yrsp_open(&rsp, &pd)) { fprintf(stderr, "trial_mouse_tracking: %s\n", yrsp_error(&rsp)); return 1; }

    out = fopen(out_path, "w");
    traj = fopen(traj_path, "w");
    if (!out || !traj) { fprintf(stderr, "trial_mouse_tracking: cannot write %s or %s\n", out_path, traj_path); return 1; }
    yscr_describe(&scr, line, sizeof line);
    fprintf(out, "# reps %d, seed %llu\n# %s\n# ysp_response %s; pointer: %s%s\n", reps, (unsigned long long)seed0, line,
            yrsp_version(), raw ? "raw mice, drawn cursor, 1 px per count; " : "SDL window coordinates; ",
            sim ? "synthetic (tier SIM)" : sources[0].note ? sources[0].note : "");
    for (i = 0; i < 3; i++) {   /* the targets, as extension-mouse-tracking's `targets` */
        float x0, y0, x1, y1;
        ygfx_bounds(&gfx, i < 2 ? &boxes[i] : &start, &x0, &y0, &x1, &y1);
        fprintf(out, "# target %s: x %.0f..%.0f, y %.0f..%.0f px\n", i == 0 ? "left" : i == 1 ? "right" : "start", x0, x1,
                y0, y1);
    }
    fprintf(out, "trial_index,color,answer,response,correct,init_time_s,rt_s,max_deviation,n_samples,onset_tier,onset_src,"
                 "init_tier,rsp_flags,early_move\n");
    fprintf(traj, "trial_index,t_s,x,y\n");
    printf("%s\n", line);

    for (;;) {
        SDL_Event ev;
        int64_t bt = 0;
        yrsp_input in;
        int rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_mouse_tracking: %s\n", yscr_error(&scr)); failed = 1; break; }

        yrsp_update_frame(&rsp, &f);   /* the disc's flip record, when it arrives: the onset, final */

        if (!in_trial) {
            ytl_seq q;
            const char* color;
            if (ytr_next(&trials, &ti) < 0) break;
            color = ytb_text(&tab, ti.condition, color_col);
            disc.color[0] = !strcmp(color, "red") ? 0.8f : 0.1f;
            disc.color[1] = !strcmp(color, "red") ? 0.15f : 0.25f;
            disc.color[2] = !strcmp(color, "red") ? 0.1f : 0.9f;
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            ytl_on(&q, START_ON);
            if (q.err) { fprintf(stderr, "trial_mouse_tracking: timeline error %d\n", q.err); failed = 1; break; }
            phase = WAIT_START;
            in_trial = 1;
            armed_seen = ended_seen = 0;
            if (sim) {
                float sx, sy, bx, by;
                ygfx_resolve(&gfx, &start, &sx, &sy);
                ygfx_resolve(&gfx, &boxes[!strcmp(ytb_text(&tab, ti.condition, answer_col), "right")], &bx, &by);
                n_sim_ev = sim_path(f.onset, ti.index, sx, sy, bx, by, bx < cx ? W - 100 : 100);
                if (ti.index < 2) sim_t0[ti.index] = f.onset;
                sim_next = 0;
            }
        }

        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) pointer(&in, ev.type == yscr_input_event_type());
        while (sim && sim_next < n_sim_ev && sim_ev[sim_next].t <= f.onset) pointer(&sim_ev[sim_next++], raw);

        /* the start click: the disc and the boxes on this frame */
        if (phase != WAIT_START && !armed_seen) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, START_ON);
            ytl_on(&q, STIM_ON);
            armed_seen = 1;
        }
        /* the click in a box: everything off, then the interval */
        if (phase == DONE && !ended_seen) {
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, STIM_ON);
            ytl_wait(&q, YRT_S(ITI_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "trial_mouse_tracking: timeline error %d\n", q.err); failed = 1; break; }
            ended_seen = 1;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        yrsp_set_landing(&rsp, fired, n, STIM_ON);   /* the disc landed: its planned onset */
        for (i = 0; i < n; i++) {
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                /* the trial is over: the onset is final; the trace stopped at the click */
                char init[32], rt[32];
                const yrsp_input* p0 = NULL;
                const yrsp_entry* ent;
                const char* answer = ytb_text(&tab, ti.condition, answer_col);
                int k, n_ent, early_move = 0, n_samples = 0, correct = chosen == !strcmp(answer, "right");
                double dev = 0, lx, ly, len;
                yrsp_finish(&rsp, &res);
                ent = yrsp_entries(&rsp, &n_ent);   /* n_early counts the start click too */
                for (k = 0; k < n_ent; k++)
                    if ((ent[k].flags & YRSP_E_CROSSING) && (ent[k].flags & YRSP_E_EARLY)) early_move = 1;
                if (res.rt_ns != YRSP_NO_TIME) snprintf(init, sizeof init, "%.9f", YRT_TO_S(res.rt_ns));
                else init[0] = 0;
                snprintf(rt, sizeof rt, "%.9f", (double)(t_click - res.t_onset) / 1e9);
                for (k = 0; k < res.n_trace; k++) {
                    const yrsp_input* s = &trace[k];
                    if (s->type != YRSP_SAMPLE) continue;
                    if (!p0) p0 = s;
                    fprintf(traj, "%d,%.9f,%.1f,%.1f\n", ti.index, (double)(s->t - res.t_onset) / 1e9, s->x, s->y);
                    n_samples++;
                }
                if (p0 && n_samples > 1) {   /* the largest distance from the line, first sample to last */
                    const yrsp_input* p1 = p0;
                    for (k = 0; k < res.n_trace; k++)
                        if (trace[k].type == YRSP_SAMPLE) p1 = &trace[k];
                    lx = p1->x - p0->x;
                    ly = p1->y - p0->y;
                    len = sqrt(lx * lx + ly * ly);
                    for (k = 0; k < res.n_trace && len > 0; k++)
                        if (trace[k].type == YRSP_SAMPLE) {
                            double d = fabs((trace[k].x - p0->x) * ly - (trace[k].y - p0->y) * lx) / len;
                            if (d > dev) dev = d;
                        }
                }
                fprintf(out, "%d,%s,%s,%s,%d,%s,%s,%.1f,%d,%u,%u,%u,%u,%d\n", ti.index,
                        ytb_text(&tab, ti.condition, color_col), answer, chosen ? "right" : "left", correct, init, rt, dev,
                        n_samples, (unsigned)res.onset_tier, (unsigned)res.onset_src, (unsigned)res.stamp_tier,
                        (unsigned)res.flags, early_move);
                fflush(out);
                fflush(traj);
                printf("trial %2d %-4s -> %-5s %s init %s rt %s max deviation %.1f px, %d samples\n", ti.index,
                       ytb_text(&tab, ti.condition, color_col), chosen ? "right" : "left", correct ? "correct" : "wrong",
                       init[0] ? init : "-", rt, dev, n_samples);
                ytr_update(&trials, correct, NULL);
                if (ti.index < 2) {
                    sim_init[ti.index] = res.rt_ns == YRSP_NO_TIME ? NAN : YRT_TO_S(res.rt_ns);
                    sim_rt_ns[ti.index] = res.rt_ns;
                    sim_onset[ti.index] = res.t_onset;
                    sim_dev[ti.index] = dev;
                    sim_correct[ti.index] = correct;
                    sim_samples[ti.index] = n_samples;
                }
                rows++;
                in_trial = 0;
            }
        }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        dot.x = cursor.x;
        dot.y = cursor.y;
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &start);
        ygfx_draw_n(&gfx, boxes, 2);
        ygfx_draw(&gfx, &disc);
        if (raw) ygfx_draw(&gfx, &dot);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    fclose(out);
    fclose(traj);
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d trials written to %s, samples to %s%s\n", rows, out_path, traj_path,
           aborted ? " (stopped with Shift+Esc)" : "");

    if (sim && !failed && reps != 1) printf("--sim: the self-check needs the default --reps\n");
    else if (sim && !failed) {
        /* 64 samples after the start click. The movement onset is the
         * sample that passes 10 px: trial 0's first step is 8.7 px, so its
         * second sample; trial 1 bends, so its first. Samples run from
         * 0.3 s after the click (0.6 s after the script's t0), one per
         * 8 ms; the init is from the disc's flip record, the first vblank
         * at or after the click. On the virtual clock all of it is exact,
         * with raw counts too. */
        static const int cross[2] = { 2, 1 };
        int bad = rows != 2;
        if (bad) fprintf(stderr, "trial_mouse_tracking: --sim: %d rows, not 2\n", rows);
        for (i = 0; i < rows && i < 2; i++) {
            int64_t click = sim_t0[i] + YRT_MS(300), sample = click + YRT_MS(300) + (cross[i] - 1) * YRT_MS(8);
            int ok = sim_correct[i] && sim_samples[i] == 64 && sim_onset[i] >= click && sim_onset[i] - click < f.period &&
                     sim_rt_ns[i] == sample - sim_onset[i];
            if (i == 1) ok = ok && sim_dev[1] > sim_dev[0] + 50;
            if (!ok) {
                fprintf(stderr, "trial_mouse_tracking: --sim: trial %d: correct %d, %d samples, init %.9f, deviation %.1f\n",
                        i, sim_correct[i], sim_samples[i], sim_init[i], sim_dev[i]);
                bad = 1;
            }
        }
        if (bad) return 1;
        printf("--sim: every trial came out as scripted\n");
    }
    return failed;
}
