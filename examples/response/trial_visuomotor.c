/* trial_visuomotor.c - center-out reaching with a visuomotor rotation (the
 * classic adaptation task: baseline, rotation, washout), driven by raw
 * mouse counts: ysp/screen.h, ysp/gfx.h, ysp/timeline.h, ysp/trials.h,
 * ysp/response.h and ysp/sig.h.
 *
 * A start circle at the screen's center; 8 targets at 45 deg steps on a
 * circle of 0.35 x the screen's smaller side (--radius px). Angles are
 * counterclockwise on the screen from rightward. Each trial:
 *   hold      the cursor in the start circle for 0.5 s
 *   reach     the target appears (its flip onset is time 0); the trial
 *             ends when the hand crosses the target radius
 *   feedback  the cursor frozen at the crossing for 0.5 s, then the
 *             target goes off
 *   return    to the start: the cursor shows within 0.25 x the radius of
 *             the start; farther out a ring shows the distance only, so
 *             the return shows nothing of the rotation
 *
 * The cursor is the hand's displacement from the start, turned by the
 * trial's rotation: a signal path (ysp/sig.h) on the role "hand", which an
 * adapter makes from the raw mouse's counts (--gain px per count, no
 * acceleration), SDL's pointer (--sdl-mouse: OS acceleration, tier 3) or
 * a tablet pen (--pen: absolute). Feedback: online, endpoint only
 * (--endpoint-only), or none (washout). Schedule (--schedule B,R,W,
 * multiples of 8): baseline 40, rotation 80 at +30 deg (--rotation; abrupt,
 * or 1 deg more per trial with --gradual), washout 40. Targets in random
 * order in each cycle of 8.
 *
 * Measures: the movement onset, a cursor travel of --onset-frac of the
 * radius from the hold's end, from the target's flip onset; the hand's
 * angle to the target at the first sample past 1/3 of the radius, at peak
 * speed (over 10 ms) and at the crossing. At the end: the mean hand angle
 * per cycle of 8 and the aftereffect (first washout cycle minus the last
 * baseline cycle). Design notes: docs/sig.md, and docs/response.md, "The
 * task examples (v0.8.0)".
 *
 * Data. --out, one row per trial: trial_index, block, cycle, target_deg,
 * rotation_deg, feedback, rt_s (the movement onset; empty after an early
 * move), mt_s (onset to crossing), cross_s, hand_third_deg, hand_pv_deg,
 * hand_end_deg, cursor_end_deg, peak_speed_px_s, n_samples, input,
 * onset_tier, onset_src, move_tier (the onset sample's tier), rsp_flags,
 * early_move (the onset distance crossed before the target's onset).
 * --traj, one row per hand sample from the hold's end to the crossing:
 * trial_index, t_s (from the target's onset), hand_x, hand_y, cursor_x,
 * cursor_y (px from the window's top-left). Shift+Esc stops; the rows so
 * far are kept.
 *
 * Usage: trial_visuomotor [--sim | --auto] [--sdl-mouse | --pen] [--schedule B,R,W] [--rotation DEG] [--gradual]
 *          [--endpoint-only] [--gain PX] [--cpi N] [--radius PX] [--onset-frac F] [--seed N] [--out FILE]
 *          [--traj FILE] [--trace-cap N]
 *   --sim            the simulated display and the virtual clock, with a
 *                    scripted hand; checks every reach
 *   --auto           the same hand in a window
 *   --gain PX        px per count (default 0.25: about 2.7 cm of hand travel
 *                    to a 269 px target at 1000 CPI)
 *   --onset-frac F   movement onset as a fraction of the radius, 0 < F < 1/3
 *                    (default 0.1)
 *   --trace-cap N    trajectory rows kept per reach, 64..16384 (default 16384;
 *                    rows past it are counted, and --sim fails)
 * Exit code: 0 (also after Shift+Esc), 1 when something did not open or a
 * check failed, 2 for a bad argument.
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
#define YSP_SIG_IMPLEMENTATION
#include "ysp/sig.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The design's defaults, in one place (s, px, deg; counts for the mouse). */
#define HOLD_S        0.5     /* in the start circle before the target       */
#define FEEDBACK_S    0.5     /* the frozen endpoint                         */
#define RADIUS_FRAC   0.35    /* target distance, of the smaller screen side */
#define START_R       12.0f   /* the start circle (and the hold's zone)      */
#define TARGET_R      12.0f
#define SHOW_FRAC     0.25    /* returning: the cursor within this x radius  */
#define ROTATION_DEG  30.0    /* +: counterclockwise                         */
#define BASELINE_N    40
#define ROTATION_N    80
#define WASHOUT_N     40
#define RAMP_DEG      1.0     /* --gradual: per rotation trial               */
#define GAIN          0.25f   /* px per count                                */
#define CPI           1000.0  /* the mouse's counts per inch                 */
#define ONSET_FRAC    0.10    /* movement onset: cursor travel, of the radius */
#define PV_LAG_S      0.010   /* speed over this lag, for the peak           */
#define N_TARGETS     8
#define VMA_PI        3.14159265358979323846
#define MAX_TRIALS    800
#define TRACE_CAP     16384   /* 16 s at 1 kHz; 768 KB                       */

enum { TARGET_ON, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRIAL = 1 };
enum { RETURN, HOLD, REACH, FEEDBACK };   /* RETURN and HOLD: the graph's hold detect says which */
enum { IN_RAW, IN_SDL, IN_PEN };
/* The task's graph: the hand role, the cursor, the trace, the detectors. */
enum { N_HAND, N_CUR, N_TRACE, N_REC, N_EMIT, N_HOLD, N_REACH, N_BIND_CUR, N_BIND_HAND, N_EXP };
enum { CH_CUR_X, CH_CUR_Y, CH_HAND_X, CH_HAND_Y, N_SIG_CH };
enum { FB_ONLINE, FB_ENDPOINT, FB_NONE };
static const char* const fb_name[] = { "online", "endpoint", "none" };
static const char* const block_name[] = { "baseline", "rotation", "washout" };

static const char conditions_csv[] = "target_deg\n0\n45\n90\n135\n180\n225\n270\n315\n";

/* The record node's rows, the hand and the cursor per sample (px) */
typedef struct trace_row { int64_t t; float hx, hy, cx, cy; } trace_row;

/* A reach's measures, for its row */
typedef struct reach_row {
    int trial, fb, early, n_samples;
    double target_deg, rotation;
    double a_third, a_pv, a_end, c_end, vmax;   /* hand (a_) and cursor (c_) angles to the target */
    int64_t rt, cross, t_onset;                 /* rt: YRSP_NO_TIME after an early move */
    float end_hx, end_hy;                       /* the hand at the crossing */
    const trace_row *third, *pv;                /* the samples of a_third and a_pv */
} reach_row;

/* The scripted hand and the self-checks of --sim and --auto, at the end of
 * the file. Without sim_begin() each call does nothing. */
static void sim_begin(bool virtual_clock);
static void sim_trial_start(const yscr_frame* f, int trial, bool first);
static const yrsp_input* sim_input_due(int64_t now);
static void sim_frame(int trial, double deg, int phase_now, int64_t arm_t);
static void sim_reach_done(const reach_row* m);
static void sim_print_model(int first_trial);
static int sim_check_end(int failed, int aborted, int n_rows, int total, int n_lost);

/* The options */
static struct {
    bool sim, autorun, gradual, endpoint_only, raw;
    int input, sched[3], trace_cap;
    double rotation, cpi, onset_frac, radius;
    float gain;                       /* px per count */
    uint64_t seed;
    const char *out_path, *traj_path;
} opt = { .raw = true, .input = IN_RAW, .sched = { BASELINE_N, ROTATION_N, WASHOUT_N }, .trace_cap = TRACE_CAP,
          .rotation = ROTATION_DEG, .cpi = CPI, .onset_frac = ONSET_FRAC, .gain = GAIN, .seed = 20261106,
          .out_path = "trial_visuomotor.csv", .traj_path = "trial_visuomotor_trajectory.csv" };

static yscr_screen scr;
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static yrsp_collector rsp;
static yrsp_choice moved[1];    /* level set at run time from the radius */
static yrsp_source sources[1];
static ysig_graph sg;
static ysig_node exp_nodes[N_EXP], ad_nodes[2];
static uint64_t sig_arena[(256u << 10) / 8];
static float sig_ch[N_SIG_CH];
static uint64_t table_arena[1024];
static ytb_table tab;
static FILE *out, *traj;

static ygfx_stim start, target, dot, ring;
static const ygfx_bind binds[] = { { .stim = &target, .param = YGFX_P_VISIBLE, .channel = TARGET_ON } };

/* The workspace: the window, the start's center, the target radius */
static float W, H, sx, sy, radius;
static double onset_px;
static int deg_col, total;

/* Where the run is: the trial, the hand's phase, the trace, the summary */
static ytr_trial_info ti;
static yrsp_result res;
static int phase, rows, target_seen, fb_seen;
static double rot_deg, target_deg;     /* this trial's rotation and target */
static int64_t t_arm, t_cross;         /* hold end (armed), crossing */
static float cross_hx, cross_hy, cross_cx, cross_cy;
static trace_row trace[TRACE_CAP];
static int n_trace, n_trace_lost, trial_lost;
static double hand_end[MAX_TRIALS], rot_of[MAX_TRIALS];
static int fb_of[MAX_TRIALS];

static int parse_options(int argc, char** argv) {
    int i, *s = opt.sched;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) opt.sim = 1;
        else if (!strcmp(argv[i], "--auto")) opt.autorun = 1;
        else if (!strcmp(argv[i], "--sdl-mouse")) opt.input = IN_SDL;
        else if (!strcmp(argv[i], "--pen")) opt.input = IN_PEN;
        else if (!strcmp(argv[i], "--gradual")) opt.gradual = 1;
        else if (!strcmp(argv[i], "--endpoint-only")) opt.endpoint_only = 1;
        else if (!strcmp(argv[i], "--schedule") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d,%d", &s[0], &s[1], &s[2]) != 3) s[0] = -1;
        } else if (!strcmp(argv[i], "--rotation") && i + 1 < argc) opt.rotation = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gain") && i + 1 < argc) opt.gain = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--cpi") && i + 1 < argc) opt.cpi = atof(argv[++i]);
        else if (!strcmp(argv[i], "--radius") && i + 1 < argc) opt.radius = atof(argv[++i]);
        else if (!strcmp(argv[i], "--onset-frac") && i + 1 < argc) opt.onset_frac = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) opt.seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) opt.out_path = argv[++i];
        else if (!strcmp(argv[i], "--traj") && i + 1 < argc) opt.traj_path = argv[++i];
        else if (!strcmp(argv[i], "--trace-cap") && i + 1 < argc) opt.trace_cap = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: trial_visuomotor [--sim | --auto] [--sdl-mouse | --pen] [--schedule B,R,W] [--rotation DEG] "
                            "[--gradual] [--endpoint-only] [--gain PX] [--cpi N] [--radius PX] [--onset-frac F] [--seed N] "
                            "[--out FILE] [--traj FILE]\n");
            return 2;
        }
    }
    opt.raw = opt.input == IN_RAW;
    if (opt.trace_cap < 64 || opt.trace_cap > TRACE_CAP) {
        fprintf(stderr, "trial_visuomotor: --trace-cap takes 64..%d\n", TRACE_CAP);
        return 2;
    }
    total = s[0] + s[1] + s[2];
    if (s[0] < 0 || s[1] < 0 || s[2] < 0 || total < N_TARGETS || total > MAX_TRIALS || s[0] % N_TARGETS ||
        s[1] % N_TARGETS || s[2] % N_TARGETS) {
        fprintf(stderr, "trial_visuomotor: --schedule takes B,R,W, multiples of 8, 8..800 in all\n");
        return 2;
    }
    if (!(opt.gain > 0) || !(opt.cpi > 0) || !(fabs(opt.rotation) <= 180) || opt.radius < 0 ||
        !(opt.onset_frac > 0 && opt.onset_frac < 1 / 3.0)) {
        fprintf(stderr, "trial_visuomotor: --gain and --cpi must be > 0, --rotation in -180..180, --onset-frac in (0, 1/3)\n");
        return 2;
    }
    return 0;
}

static bool fail(const char* why) {
    fprintf(stderr, "trial_visuomotor: %s\n", why);
    return false;
}

static int block_of(int trial) { return trial < opt.sched[0] ? 0 : trial < opt.sched[0] + opt.sched[1] ? 1 : 2; }

static ygfx_stim circle(float x, float y, float d, float stroke, float r, float g, float b) {
    return ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_CIRCLE,
                                          .place = YGFX_TOP_LEFT,   /* everything here is in window pixels */
                                          .x = x, .y = y, .w = d, .h = d, .stroke = stroke, .color = { r, g, b } });
}

static double dist(float x, float y) { return sqrt((double)(x - sx) * (x - sx) + (double)(y - sy) * (y - sy)); }
/* A point's direction from the start relative to the target, in (-180, 180]. */
static double rel_deg(float x, float y, double deg) {
    double a = atan2((double)sy - y, (double)x - sx) * 180 / VMA_PI - deg;
    while (a > 180) a -= 360;
    while (a <= -180) a += 360;
    return a;
}

/* The hold's end: the collector's window and the crossing detect open
 * there. The trace keeps the rows from then on: the sample that ended the
 * hold came through the graph before this call. */
static void arm(int64_t t) {
    int k, m = 0;
    yrsp_arm(&rsp, t);
    ysig_arm(&sg, N_REACH, t);
    t_arm = t;
    phase = REACH;
    for (k = 0; k < n_trace; k++)
        if (trace[k].t >= t) trace[m++] = trace[k];
    n_trace = m;
}

/* The record node's rows: every hand sample from the hold's end to the
 * crossing. Before the hold's end only the newest row is kept: the sample
 * that ends the hold comes through the graph before arm() sees it, and the
 * return's hundreds of rows must not fill the trace before the reach. */
static void on_row(void* user, const ysig_row* r) {
    (void)user;
    if (phase == FEEDBACK) return;
    if (phase != REACH) n_trace = 0;
    if (n_trace == opt.trace_cap) { n_trace_lost++; trial_lost++; return; }
    trace[n_trace] = (trace_row){ r->value.t, (float)r->value.v[0], (float)r->value.v[1], (float)r->value.v[2],
                                  (float)r->value.v[3] };
    n_trace++;
}

/* The hold's end found by a frame: a still hand sends nothing. */
static void frame_hits(void) {
    ysig_hit h[8];
    int n = ysig_detected(&sg, h, 8), i;
    for (i = 0; i < n; i++)
        if (h[i].node == N_HOLD && phase != REACH && phase != FEEDBACK) arm(h[i].t);
}

/* Every input event, real or scripted. The graph takes the hand's; the
 * collector gets the hand's position as the emit node's derived events, so
 * its DISTANCE crossing reads pixels. */
static void pointer(const yrsp_input* ev) {
    yin_event der[8];
    ysig_hit h[8];
    int n, i, nh;
    if (ysig_feed(&sg, ev) <= 0) return;
    /* the hold first, then the collector, then the crossing: the order the
     * window needs */
    nh = ysig_detected(&sg, h, 8);
    for (i = 0; i < nh; i++)
        if (h[i].node == N_HOLD && phase != REACH && phase != FEEDBACK) arm(h[i].t);
    n = ysig_derived(&sg, der, 8);
    for (i = 0; i < n; i++) yrsp_feed(&rsp, &der[i]);
    for (i = 0; i < nh; i++)
        if (h[i].node == N_REACH && phase == REACH) {
            const ysig_value* c = ysig_value_of(&sg, N_CUR);
            t_cross = h[i].t;
            cross_hx = (float)h[i].value.v[0];
            cross_hy = (float)h[i].value.v[1];
            cross_cx = (float)c->v[0];
            cross_cy = (float)c->v[1];
            yrsp_finish(&rsp, &res);           /* the window stops at the crossing */
            phase = FEEDBACK;
        }
}

/* The graph: the task's nodes on the role "hand", and the adapter of the
 * input path. A node's p[] are the rows of ysig_op_params(), by name
 * (docs/sig.md, "Ops"). */
static bool open_graph(void) {
    ysig_adapter ad = { .role = "hand", .units = YSIG_UNITS_DISPLAY_PX, .nodes = ad_nodes, .n_nodes = 1 };
    int k;
    for (k = 0; k < N_EXP; k++) exp_nodes[k] = ysig_node_of(YSIG_OP_NONE, -1, -1);
    exp_nodes[N_HAND] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
    exp_nodes[N_CUR] = ysig_node_of(YSIG_OP_AFFINE, N_HAND, -1);    /* turned about the start */
    exp_nodes[N_CUR].p[YSIG_AFFINE_PRE_X] = exp_nodes[N_CUR].p[YSIG_AFFINE_POST_X] = sx;
    exp_nodes[N_CUR].p[YSIG_AFFINE_PRE_Y] = exp_nodes[N_CUR].p[YSIG_AFFINE_POST_Y] = sy;
    exp_nodes[N_TRACE] = ysig_node_of(YSIG_OP_JOIN, N_HAND, N_CUR);
    exp_nodes[N_REC] = ysig_node_of(YSIG_OP_RECORD, N_TRACE, -1);
    exp_nodes[N_REC].name = "trace";
    exp_nodes[N_EMIT] = ysig_node_of(YSIG_OP_EMIT, N_HAND, -1);
    exp_nodes[N_EMIT].p[YSIG_EMIT_KIND] = 7;                         /* YIN_KIND_USER + 7 */
    exp_nodes[N_HOLD] = ysig_node_of(YSIG_OP_DETECT, N_HAND, -1);   /* in the start for HOLD_S */
    exp_nodes[N_HOLD].p[YSIG_DETECT_TEST] = YSIG_TEST_LE;
    exp_nodes[N_HOLD].p[YSIG_DETECT_LANE] = -1;                     /* the norm of hand - center */
    exp_nodes[N_HOLD].p[YSIG_DETECT_LEVEL] = START_R;
    exp_nodes[N_HOLD].p[YSIG_DETECT_HOLD] = HOLD_S;
    exp_nodes[N_HOLD].p[YSIG_DETECT_A_X] = sx;
    exp_nodes[N_HOLD].p[YSIG_DETECT_A_Y] = sy;
    exp_nodes[N_REACH] = ysig_node_of(YSIG_OP_DETECT, N_HAND, -1);  /* past the target radius */
    exp_nodes[N_REACH].p[YSIG_DETECT_TEST] = YSIG_TEST_GE;
    exp_nodes[N_REACH].p[YSIG_DETECT_LANE] = -1;
    exp_nodes[N_REACH].p[YSIG_DETECT_LEVEL] = radius;
    exp_nodes[N_REACH].p[YSIG_DETECT_START] = 1;                    /* armed at the hold's end */
    exp_nodes[N_REACH].p[YSIG_DETECT_A_X] = sx;
    exp_nodes[N_REACH].p[YSIG_DETECT_A_Y] = sy;
    exp_nodes[N_BIND_CUR] = ysig_node_of(YSIG_OP_BIND, N_CUR, -1);
    exp_nodes[N_BIND_CUR].p[YSIG_BIND_CHANNEL] = CH_CUR_X;
    exp_nodes[N_BIND_HAND] = ysig_node_of(YSIG_OP_BIND, N_HAND, -1);
    exp_nodes[N_BIND_HAND].p[YSIG_BIND_CHANNEL] = CH_HAND_X;
    ad_nodes[0] = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
    ad_nodes[0].lanes = 2;
    if (opt.input == IN_RAW) {
        /* counts integrated from the start, held inside the window */
        ad.name = "raw mouse";
        ad.type = YSIG_TYPE_POSITION2D_RELATIVE;
        ad_nodes[0].p[YSIG_SOURCE_KIND] = YIN_KIND_MOUSE;
        ad_nodes[0].p[YSIG_SOURCE_CONTROL] = YIN_AXIS_DELTA;
        ad_nodes[0].p[YSIG_SOURCE_TIER] = yin_raw_mouse_source().tier;
        ad_nodes[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
        ad_nodes[1].p[YSIG_INTEGRATE_GAIN_X] = ad_nodes[1].p[YSIG_INTEGRATE_GAIN_Y] = opt.gain;
        ad_nodes[1].p[YSIG_INTEGRATE_ORIGIN_X] = sx;
        ad_nodes[1].p[YSIG_INTEGRATE_ORIGIN_Y] = sy;
        ad_nodes[1].p[YSIG_INTEGRATE_HIGH_X] = W;    /* [0, W] x [0, H]: the count sum stays in the window */
        ad_nodes[1].p[YSIG_INTEGRATE_HIGH_Y] = H;
        ad.n_nodes = 2;
    } else {
        int kind = opt.input == IN_PEN ? YIN_KIND_PEN : YIN_KIND_MOUSE;
        ad.name = opt.input == IN_PEN ? "tablet pen" : "SDL pointer";
        ad.type = YSIG_TYPE_POSITION2D;
        ad_nodes[0].p[YSIG_SOURCE_KIND] = kind;
        ad_nodes[0].p[YSIG_SOURCE_CONTROL] = YIN_AXIS_POSITION;
        ad_nodes[0].p[YSIG_SOURCE_TIER] = yin_sdl_source(NULL, kind).tier;
    }
    /* relative: the hand is placed on the start at the first trial */
    if (!ysig_open(&sg, &(ysig_desc){ .nodes = exp_nodes, .n_nodes = N_EXP,
                                      .roles = &(ysig_role){ .name = "hand", .type = YSIG_TYPE_POSITION2D,
                                                             .units = YSIG_UNITS_DISPLAY_PX, .allow_relative = true },
                                      .n_roles = 1, .adapters = &ad, .n_adapters = 1, .channels = sig_ch,
                                      .n_channels = N_SIG_CH, .on_row = on_row, .arena = sig_arena,
                                      .arena_size = sizeof sig_arena }))
        return fail(ysig_error(&sg));
    return true;
}

/* The hidden system cursor still moves, and a click goes to the window
 * under it: keep it in ours. On the window's thread. */
static void grab_on_window(void* p) {
    const float* xy = (const float*)p;
    SDL_HideCursor();
    SDL_SetWindowMouseGrab(yscr_window(&scr), true);
    if (!opt.raw) SDL_WarpMouseInWindow(yscr_window(&scr), xy[0], xy[1]);
}

/* The display and the workspace, the graph, the conditions, the trials,
 * the timeline and the collector */
static bool open_task(void) {
    static float xy[2];
    /* the simulated display has no window: the script feeds counts itself */
    yscr_desc sd = { .backend = opt.sim ? YSCR_BACKEND_SIM : 0, .windowed = true, .window_w = 1024, .window_h = 768,
                     .raw_mice = opt.raw && !opt.sim };
    /* ysp/gfx.h 0.18.0 gives the simulated display the window's size, so
     * --sim has the geometry of a window run */
    if (!ygfx_open_screen(&gfx, &scr, &sd, &(ygfx_desc){ .background = { 0.3f, 0.3f, 0.3f } })) return fail(ygfx_error(&gfx));
    ygfx_size(&gfx, &W, &H);
    ygfx_center(&gfx, &sx, &sy);
    radius = opt.radius > 0 ? (float)opt.radius : (float)(RADIUS_FRAC * (W < H ? W : H));
    /* in cursor px, so it stays inside 1/3 of the radius (where the angle
     * is read) at any gain */
    onset_px = opt.onset_frac * radius;
    start = circle(sx, sy, 2 * START_R, 0, 0.9f, 0.9f, 0.9f);
    target = circle(sx, sy, 2 * TARGET_R, 0, 0.1f, 0.8f, 0.2f);
    dot = circle(sx, sy, 8, 0, 1, 1, 0.2f);
    ring = circle(sx, sy, 10, 2, 1, 1, 0.2f);
    if (!open_graph()) return false;
    if (!opt.sim) {
        xy[0] = sx;
        xy[1] = sy;
        yscr_window_call(&scr, grab_on_window, xy);
    }
    if (!ytb_open_csv(&tab, &(ytb_csv_desc){ .text = conditions_csv, .len = sizeof conditions_csv - 1,
                                             .arena = table_arena, .arena_size = sizeof table_arena }))
        return fail(ytb_error(&tab));
    deg_col = ytb_col(&tab, "target_deg");
    /* each repetition, a cycle of 8, shuffled */
    if (!ytr_open(&trials, &(ytr_desc){ .table = &tab, .reps = total / N_TARGETS, .order = YTR_ORDER_RANDOM,
                                        .rng = ytr_splitmix, .rng_ctx = &opt.seed }))
        return fail(ytr_error(&trials));
    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS }))
        return fail(ytl_error(&tl));
    /* the movement onset from the emit node's derived events; the window
     * stays open (persist) for the trace until the crossing */
    moved[0] = (yrsp_choice){ .name = "moved", .kind = YRSP_KIND_USER + 7, .device = N_EMIT, .cross = YRSP_CROSS_DISTANCE,
                              .control = YRSP_AXIS_POSITION, .level = (float)onset_px };
    sources[0] = opt.raw ? yrsp_raw_mouse_source()
                         : yrsp_sdl_source(NULL, opt.input == IN_PEN ? YIN_KIND_PEN : YRSP_KIND_MOUSE);
    if (!yrsp_open(&rsp, &(yrsp_desc){ .choices = moved, .n_choices = 1, .persist = true, .sources = sources,
                                       .n_sources = 1 }))
        return fail(yrsp_error(&rsp));
    return true;
}

/* The two data files; the settings as # lines */
static bool open_data(uint64_t seed0) {
    const char* pen_or = opt.input == IN_PEN ? "pen" : "pointer";
    char line[1024];
    out = fopen(opt.out_path, "w");
    traj = fopen(opt.traj_path, "w");
    if (!out || !traj) {
        fprintf(stderr, "trial_visuomotor: cannot write %s or %s\n", opt.out_path, opt.traj_path);
        return false;
    }
    yscr_describe(&scr, line, sizeof line);
    fprintf(out, "# schedule %d,%d,%d; rotation %.1f deg %s; %s feedback; seed %llu\n# %s\n", opt.sched[0], opt.sched[1],
            opt.sched[2], opt.rotation, opt.gradual ? "gradual" : "abrupt", opt.endpoint_only ? "endpoint" : "online",
            (unsigned long long)seed0, line);
    fprintf(out, "# ysp_response %s; hand: %s%s\n", yrsp_version(),
            opt.raw ? "raw mice, counts integrated, no acceleration; "
                    : opt.input == IN_PEN ? "tablet pen, SDL window coordinates; " : "SDL window coordinates, OS acceleration; ",
            opt.sim || opt.autorun ? "scripted (tier SIM)" : sources[0].note ? sources[0].note : "");
    if (opt.raw)
        fprintf(out, "# gain %.6g px/count, cpi %.0f, onset %.2f px (%.3g of the radius; %.3f cm of hand travel)",
                opt.gain, opt.cpi, onset_px, opt.onset_frac, onset_px / opt.gain / opt.cpi * 2.54);
    else fprintf(out, "# onset %.2f %s px (%.3g of the radius)", onset_px, pen_or, opt.onset_frac);
    fprintf(out, "; start (%.1f, %.1f) r %.0f px; target radius %.2f px\n", sx, sy, START_R, radius);
    fprintf(out, "trial_index,block,cycle,target_deg,rotation_deg,feedback,rt_s,mt_s,cross_s,hand_third_deg,hand_pv_deg,"
                 "hand_end_deg,cursor_end_deg,peak_speed_px_s,n_samples,input,onset_tier,onset_src,move_tier,rsp_flags,"
                 "early_move\n");
    fprintf(traj, "trial_index,t_s,hand_x,hand_y,cursor_x,cursor_y\n");
    printf("%s\n", line);
    printf("signal path: %d nodes, adapter %s\n", sg.n, opt.raw ? "raw mouse" : opt.input == IN_PEN ? "tablet pen" : "SDL pointer");
    return true;
}

/* The next trial: its rotation, feedback and target. The target comes on
 * at the hold's end, so the timeline only anchors the trial here. */
static bool start_trial(const yscr_frame* f) {
    int b = block_of(ti.index), k = ti.index - opt.sched[0];
    rot_deg = b != 1 ? 0 : opt.gradual && (k + 1) * RAMP_DEG < fabs(opt.rotation)
                               ? (opt.rotation < 0 ? -1 : 1) * (k + 1) * RAMP_DEG : opt.rotation;
    fb_of[ti.index] = b == 2 ? FB_NONE : opt.endpoint_only ? FB_ENDPOINT : FB_ONLINE;
    rot_of[ti.index] = rot_deg;
    /* the cursor's turn, counterclockwise on the screen: y is down */
    if (!ysig_set(&sg, N_CUR, YSIG_AFFINE_ANGLE, -rot_deg)) { fprintf(stderr, "trial_visuomotor: rotation %g\n", rot_deg); return false; }
    target_deg = ytb_num(&tab, ti.condition, deg_col);
    target.x = (float)(sx + radius * cos(target_deg * VMA_PI / 180));
    target.y = (float)(sy - radius * sin(target_deg * VMA_PI / 180));
    ytl_clear(&tl, TRIAL);
    ytl_anchor(&tl, TRIAL, f->onset, 0);
    target_seen = fb_seen = 0;
    /* the first trial: the hand is on the start, and the hold detect,
     * armed at the reset, counts from here */
    if (phase != HOLD) phase = RETURN;
    else ysig_reset(&sg, f->onset);
    sim_trial_start(f, ti.index, phase == HOLD);
    return true;
}

/* The reach's measures from the trace, and its rows in the trajectory
 * file. No interpolation between samples (the collector's rule). */
static void measure_reach(reach_row* m) {
    const yrsp_entry* ent;
    const trace_row *p, *lag = NULL;
    int k, j = 0, n_ent;
    *m = (reach_row){ .trial = ti.index, .fb = fb_of[ti.index], .target_deg = target_deg, .rotation = rot_deg,
                      .a_third = NAN, .a_pv = NAN, .t_onset = res.t_onset, .end_hx = cross_hx, .end_hy = cross_hy };
    ent = yrsp_entries(&rsp, &n_ent);
    for (k = 0; k < n_ent; k++)
        if ((ent[k].flags & YRSP_E_CROSSING) && (ent[k].flags & YRSP_E_EARLY)) m->early = 1;
    /* rows to the crossing; the loop stops at the first row past it, so
     * with two mice on an any-device source (rows out of time order) a
     * reach's last rows can be out of order */
    for (k = 0; k < n_trace && trace[k].t <= t_cross; k++) {
        p = &trace[k];
        fprintf(traj, "%d,%.9f,%.3f,%.3f,%.3f,%.3f\n", ti.index, (double)(p->t - res.t_onset) / 1e9, p->hx, p->hy, p->cx,
                p->cy);
        m->n_samples++;
        if (!m->third && dist(p->hx, p->hy) >= radius / 3) m->third = p;
        /* speed over at least PV_LAG_S: one report per ms is too coarse in
         * counts for a direction or a speed */
        while (j < k && p->t - trace[j + 1].t >= YRT_S(PV_LAG_S)) j++;
        lag = &trace[j];
        if (p->t - lag->t >= YRT_S(PV_LAG_S)) {
            double v = sqrt((double)(p->hx - lag->hx) * (p->hx - lag->hx) + (double)(p->hy - lag->hy) * (p->hy - lag->hy)) /
                       YRT_TO_S(p->t - lag->t);
            if (v > m->vmax) { m->vmax = v; m->pv = p; }
        }
    }
    if (m->third) m->a_third = rel_deg(m->third->hx, m->third->hy, target_deg);
    if (m->pv) m->a_pv = rel_deg(m->pv->hx, m->pv->hy, target_deg);
    m->a_end = rel_deg(cross_hx, cross_hy, target_deg);
    m->c_end = rel_deg(cross_cx, cross_cy, target_deg);
    m->rt = m->early ? YRSP_NO_TIME : res.rt_ns;
    m->cross = t_cross - res.t_onset;
}

static void write_row(const reach_row* m) {
    char rt[32] = "", mt[32] = "";
    if (res.rt_ns != YRSP_NO_TIME && !m->early) {
        snprintf(rt, sizeof rt, "%.9f", YRT_TO_S(res.rt_ns));
        snprintf(mt, sizeof mt, "%.9f", YRT_TO_S(t_cross - res.t_response));
    }
    fprintf(out, "%d,%s,%d,%.0f,%.3f,%s,%s,%s,%.9f,%.4f,%.4f,%.4f,%.4f,%.1f,%d,%s,%u,%u,%u,%u,%d\n", m->trial,
            block_name[block_of(m->trial)], m->trial / N_TARGETS, m->target_deg, m->rotation, fb_name[m->fb], rt, mt,
            YRT_TO_S(m->cross), m->a_third, m->a_pv, m->a_end, m->c_end, m->vmax, m->n_samples,
            opt.sim || opt.autorun ? "script" : opt.raw ? "raw" : opt.input == IN_PEN ? "pen" : "sdl",
            (unsigned)res.onset_tier, (unsigned)res.onset_src, (unsigned)res.stamp_tier, (unsigned)res.flags, m->early);
    fflush(out);
    fflush(traj);
    if (!opt.sim)
        printf("trial %3d target %3.0f rotation %5.1f %-8s rt %s hand %7.2f cursor %7.2f deg%s\n", m->trial, m->target_deg,
               m->rotation, fb_name[m->fb], rt[0] ? rt : "-", m->a_end, m->c_end, m->early ? " (early)" : "");
}

/* The feedback's end: the onset is final, and the trace holds the reach */
static void finish_trial(const yscr_frame* f) {
    reach_row m;
    yrsp_finish(&rsp, &res);
    measure_reach(&m);
    write_row(&m);
    hand_end[ti.index] = m.a_end;
    sim_reach_done(&m);
    ytr_update(&trials, 1, NULL);
    rows++;
    phase = RETURN;
    /* the trial's trajectory is short by the rows past the cap: said here,
     * not in a column (the data file keeps its columns) */
    if (trial_lost) printf("trial %d: %d trajectory rows lost past --trace-cap %d\n", ti.index, trial_lost, opt.trace_cap);
    trial_lost = 0;
    n_trace = 0;
    ysig_arm(&sg, N_HOLD, f->onset);   /* the next hold, once the hand is back */
}

/* The dot: the frozen endpoint, the rotated cursor in the reach, or the
 * hand near the start; farther out on the return, a ring */
static void place_cursor(int fb) {
    double d = dist(sig_ch[CH_HAND_X], sig_ch[CH_HAND_Y]);
    dot.visible = 1;
    ring.visible = 0;
    if (phase == FEEDBACK) {
        dot.x = cross_cx;
        dot.y = cross_cy;
        dot.visible = fb != FB_NONE ? 1.0f : 0.0f;
    } else if (phase == REACH) {
        dot.x = sig_ch[CH_CUR_X];
        dot.y = sig_ch[CH_CUR_Y];
        dot.visible = fb == FB_ONLINE ? 1.0f : 0.0f;
    } else {
        dot.x = sig_ch[CH_HAND_X];
        dot.y = sig_ch[CH_HAND_Y];
        if (phase == RETURN && d > SHOW_FRAC * radius) {
            dot.visible = 0;
            ring.visible = 1;
            ring.w = ring.h = (float)(2 * d);
        }
    }
}

/* The mean endpoint hand angle per cycle of 8, and the aftereffect */
static void print_summary(void) {
    double last_base = NAN, first_wash = NAN;
    int i, k;
    if (rows < N_TARGETS) return;
    printf("cycle block     rotation  hand_deg%s\n", opt.sim || opt.autorun ? "  model_deg" : "");
    for (i = 0; i + N_TARGETS <= rows; i += N_TARGETS) {
        double m = 0, r = 0;
        int b = block_of(i);
        for (k = i; k < i + N_TARGETS; k++) {
            m += hand_end[k] / N_TARGETS;
            r += rot_of[k] / N_TARGETS;
        }
        if (b == 0) last_base = m;
        if (b == 2 && isnan(first_wash)) first_wash = m;
        printf("%5d %-9s %8.2f %9.3f", i / N_TARGETS, block_name[b], r, m);
        sim_print_model(i);
        printf("\n");
    }
    if (!isnan(first_wash))
        printf("aftereffect: %.3f deg (first washout cycle %.3f minus last baseline cycle %.3f)\n",
               first_wash - (isnan(last_base) ? 0 : last_base), first_wash, isnan(last_base) ? 0 : last_base);
}

int main(int argc, char** argv) {
    char line[1024];
    uint64_t seed0;
    int n, rc, in_trial = 0, aborted = 0, failed = 0;

    if ((rc = parse_options(argc, argv)) != 0) return rc;
    if (opt.sim) yrt_virtual_begin(0);   /* ysp/rt.h's virtual clock: the same records on any host */
    if (opt.sim || opt.autorun) sim_begin(opt.sim);
    seed0 = opt.seed;   /* for the data file: the trials advance opt.seed */
    if (!open_task() || !open_data(seed0)) return 1;
    phase = HOLD;   /* the hand starts on the start */

    for (;;) {
        SDL_Event ev;
        yrsp_input in;
        const yrsp_input* scripted;
        yscr_frame f;
        ytl_event fired[64];
        rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fail(yscr_error(&scr)); failed = 1; break; }
        yrsp_update_frame(&rsp, &f);   /* the target's flip record, when it arrives */
        if (!in_trial) {
            if (ytr_next(&trials, &ti) < 0) break;
            if (!start_trial(&f)) return 1;
            in_trial = 1;
        }

        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) pointer(&in);
        while ((scripted = sim_input_due(f.onset)) != NULL) pointer(scripted);
        /* The graph at the frame's onset, after its input: the binds, and
         * the hold's end when no sample said so (the hold detect). The
         * target goes on the first frame whose onset is at or after it, so
         * this is decided up to a frame early, and a hand that leaves in
         * that last frame counts as an early move, not as a broken hold.
         * Scripted input is known up to the onset, real input up to now.
         * -1 can only be a programming error (a bad argument). */
        if (ysig_frame(&sg, &(ysig_frame_in){ f.index, f.onset, 0, NULL, 0, opt.sim || opt.autorun ? f.onset : yrt_now_ns(),
                                             true }) != 0) {
            fail(ysig_error(&sg));
            failed = 1;
            break;
        }
        frame_hits();
        if (phase >= REACH && !target_seen) {   /* the target on this frame */
            ytl_seq q = ytl_seq_at(&tl, TRIAL, f.onset);
            ytl_on(&q, TARGET_ON);
            if (q.err) { fprintf(stderr, "trial_visuomotor: timeline error %d\n", q.err); failed = 1; break; }
            target_seen = 1;
        }
        sim_frame(ti.index, target_deg, phase, t_arm);
        if (phase == FEEDBACK && !fb_seen) {   /* frozen endpoint, then the target off */
            ytl_seq q = ytl_seq_at(&tl, TRIAL, f.onset);
            ytl_wait(&q, YRT_S(FEEDBACK_S));
            ytl_off(&q, TARGET_ON);
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "trial_visuomotor: timeline error %d\n", q.err); failed = 1; break; }
            fb_seen = 1;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        yrsp_set_landing(&rsp, fired, n, TARGET_ON);   /* the plan, until the flip record */
        if (ytl_find_mark(fired, n, END_OF_TRIAL) >= 0) {
            finish_trial(&f);
            in_trial = 0;
        }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        place_cursor(fb_of[ti.index]);
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &start);
        ygfx_draw(&gfx, &target);
        if (ring.visible >= 0.5f) ygfx_draw(&gfx, &ring);
        if (dot.visible >= 0.5f) ygfx_draw(&gfx, &dot);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }

    /* the describe line again: its counters (key times, raw mice) are 0 at open */
    yscr_describe(&scr, line, sizeof line);
    fprintf(out, "# at the end: %s\n", line);
    fclose(out);
    fclose(traj);
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d trials written to %s, samples to %s%s\n", rows, opt.out_path, opt.traj_path,
           aborted ? " (stopped with Shift+Esc)" : "");
    printf("trace rows lost (a reach longer than --trace-cap %d): %d\n", opt.trace_cap, n_trace_lost);
    print_summary();
    return sim_check_end(failed, aborted, rows, total, n_trace_lost);
}

/* ---- The scripted participant and the self-checks (--sim, --auto) -------
 *
 * A hand fed as raw counts (or as SDL or pen positions) through the same
 * path, one sample per ms. It reaches straight (minimum jerk, 0.4 s) at
 * the target plus x(n), with x(n+1) = 0.98 x(n) + 0.15 e(n), e(n) minus
 * the shown endpoint's angle from the target (0 without feedback). Trial
 * 2 moves early. The checks: each hand angle and the model within the
 * counts' quantization, the rotation, the times of the movement onset,
 * the first sample past 1/3 of the radius and the crossing to the ns, the
 * early move flagged. They hold at any --gain: yrsp_cursor (v0.8) and the
 * integrate node sum whole counts, so the hand stays on the count grid.
 * --sim --pen gives the raw run's rows (CI compares them). */
#define SIM_A         0.98
#define SIM_B         0.15
#define SIM_MT_S      0.4     /* minimum-jerk duration                       */
#define SIM_EARLY     2       /* the trial that moves before the target      */
#define SIM_N         1024

static bool sim_on, sim_virtual;
static int sim_reached, sim_failed;
static yrsp_input sim_ev[SIM_N];
static int n_sim, sim_next;
static long sim_kx, sim_ky;
static double sim_x[MAX_TRIALS + 1], sim_rot[MAX_TRIALS], hand_dev, model_xm, model_max;
static int sim_fb[MAX_TRIALS];
static int64_t sim_t[MAX_TRIALS][3], sim_onset[MAX_TRIALS];   /* move, 1/3 radius, crossing */

static void sim_begin(bool virtual_clock) {
    sim_on = true;
    sim_virtual = virtual_clock;
}

/* The scripted hand: one sample per ms, as counts on the gain's lattice
 * (or as the same positions for --sdl-mouse and --pen). kx, ky: its counts
 * from the start's center. */
static void sim_put(int64_t t, long kx, long ky) {
    yrsp_input e;
    if (n_sim >= SIM_N) return;
    memset(&e, 0, sizeof e);
    e.kind = YRSP_KIND_MOUSE;
    e.type = YRSP_SAMPLE;
    e.stamp = YRSP_TIER_SIM;
    e.t = t;
    if (opt.raw) {
        e.device = 7;
        e.control = YRSP_AXIS_DELTA;
        e.x = (float)(kx - sim_kx);
        e.y = (float)(ky - sim_ky);
    } else {
        /* SDL's pointer or the pen: the same hand as positions */
        if (opt.input == IN_PEN) e.kind = YIN_KIND_PEN;
        e.control = YRSP_AXIS_POSITION;
        e.x = (float)(sx + (double)opt.gain * kx);
        e.y = (float)(sy + (double)opt.gain * ky);
    }
    sim_ev[n_sim++] = e;
    sim_kx = kx;
    sim_ky = ky;
}

/* The return goes straight to 2 start radii out, then one sample lands on
 * the center, so the hold starts at a known sample. */
static void sim_return(int64_t t_end) {
    double ux = (double)sim_kx, uy = (double)sim_ky, n = sqrt(ux * ux + uy * uy), g = opt.gain;
    double ex = n > 0 ? ux / n * 2 * START_R / g : 0, ey = n > 0 ? uy / n * 2 * START_R / g : 0;
    long kx0 = sim_kx, ky0 = sim_ky;
    int j;
    n_sim = sim_next = 0;
    for (j = 1; j < 300; j++)
        sim_put(t_end - YRT_MS(300 - j), lround(kx0 + (ex - kx0) * j / 299.0), lround(ky0 + (ey - ky0) * j / 299.0));
    sim_put(t_end, 0, 0);
}

/* The reach at angle deg (screen, counterclockwise): minimum jerk to
 * 1.25 x the radius over SIM_MT_S from t_go, or (early) a flick at
 * 10 px per ms. t[] gets the times of the first samples past `level`
 * px, 1/3 of the radius and the radius, from the lattice in double: the
 * samples the program must find. */
static void sim_reach(int64_t t_go, double deg, int early, double level, int64_t* t) {
    double a = deg * VMA_PI / 180, D = 1.25 * radius, g = opt.gain;
    int j, n = early ? (int)ceil(D / 10) : (int)(SIM_MT_S * 1000);
    n_sim = sim_next = 0;
    t[0] = t[1] = t[2] = 0;
    for (j = 1; j <= n; j++) {
        double u = (double)j / n, s = early ? u : u * u * u * (10 - 15 * u + 6 * u * u), p = D * s;
        long kx = lround(p * cos(a) / g), ky = lround(-p * sin(a) / g);
        double r = g * sqrt((double)kx * kx + (double)ky * ky);
        int64_t tj = t_go + YRT_MS(j);
        if (!t[0] && r >= level) t[0] = tj;
        if (!t[1] && r >= radius / 3.0) t[1] = tj;
        if (!t[2] && r >= radius) t[2] = tj;
        sim_put(tj, kx, ky);
    }
}

/* A sample's direction is off the scripted line by at most half a count
 * per axis: asin(0.71 gain / d) at d px from the start. */
static double quant_deg(double d) { return asin(0.5 * sqrt(2.0) * opt.gain / d) * 180 / VMA_PI + 1e-9; }

static void sim_trial_start(const yscr_frame* f, int trial, bool first) {
    int64_t t_end;
    sim_reached = 0;
    if (!sim_on) return;
    if (first) {
        /* a pointer or a pen is where it is: the script puts it on the
         * start, for the graph only (the collector's window is not open;
         * its distance starts at the arm) */
        if (!opt.raw) {
            yin_event e, der[8];
            memset(&e, 0, sizeof e);
            e.t = f->onset;
            e.kind = opt.input == IN_PEN ? YIN_KIND_PEN : YIN_KIND_MOUSE;
            e.type = YIN_SAMPLE;
            e.stamp = YIN_TIER_SIM;
            e.x = sx;
            e.y = sy;
            ysig_feed(&sg, &e);
            while (ysig_derived(&sg, der, 8) > 0) { }
        }
        return;
    }
    t_end = f->onset + YRT_MS(400);
    if (trial == SIM_EARLY && f->period > 0) {
        /* the hold ends half a frame before a vblank, so the flick has
         * that long to cross the onset distance before the target */
        int64_t m = (t_end + YRT_S(HOLD_S) + f->period / 2 - f->onset + f->period - 1) / f->period;
        t_end = f->onset + m * f->period - f->period / 2 - YRT_S(HOLD_S);
    }
    sim_return(t_end);
}

static const yrsp_input* sim_input_due(int64_t now) {
    return sim_next < n_sim && sim_ev[sim_next].t <= now ? &sim_ev[sim_next++] : NULL;
}

/* The reach: the early trial's flick at the hold's end, as the target goes
 * on; any other from the target's flip onset, as a participant would see
 * it (the collector's onset once final) */
static void sim_frame(int trial, double deg, int phase_now, int64_t arm_t) {
    yrsp_onset on;
    if (!sim_on || sim_reached || phase_now < REACH) return;
    if (trial == SIM_EARLY) {
        sim_reach(arm_t, deg + sim_x[trial], 1, onset_px, sim_t[trial]);
        sim_reached = 1;
    } else if (phase_now == REACH && yrsp_onset_now(&rsp, &on) && on.final) {
        sim_onset[trial] = on.t_ns;
        sim_reach(on.t_ns + YRT_MS(300 + 10 * (trial % 7)), deg + sim_x[trial], 0, onset_px, sim_t[trial]);
        sim_reached = 1;
    }
}

/* Each angle within the quantization of its distance, the rotation exact
 * to float, the times to the ns; then the hand learns from the endpoint it
 * was shown */
static void sim_reach_done(const reach_row* m) {
    int i = m->trial;
    double x = sim_x[i], e, dev;
    char rt[32] = "";
    if (!sim_on) return;
    if (m->rt != YRSP_NO_TIME) snprintf(rt, sizeof rt, "%.9f", YRT_TO_S(m->rt));
    dev = fabs(m->a_end - x) - quant_deg(dist(m->end_hx, m->end_hy));
    hand_dev = fmax(hand_dev, fmax(fabs(m->a_end - x), fmax(fabs(m->a_third - x), fabs(m->a_pv - x))));
    if (m->third) dev = fmax(dev, fabs(m->a_third - x) - quant_deg(dist(m->third->hx, m->third->hy)));
    if (m->pv) dev = fmax(dev, fabs(m->a_pv - x) - quant_deg(dist(m->pv->hx, m->pv->hy)));
    dev = fmax(dev, fabs(m->c_end - m->a_end - m->rotation) - 1e-3);
    if (dev > 0 || !m->third || !m->pv || m->early != (i == SIM_EARLY) || m->cross != sim_t[i][2] - m->t_onset ||
        m->third->t != sim_t[i][1] || (!m->early && (m->rt != sim_t[i][0] - sim_onset[i] || m->t_onset != sim_onset[i]))) {
        fprintf(stderr, "trial_visuomotor: check: trial %d: hand %.4f third %.4f pv %.4f (script %.4f), "
                        "cursor %.4f, early %d, rt %s, cross %.9f s (script %.9f)\n",
                i, m->a_end, m->a_third, m->a_pv, x, m->c_end, m->early, rt[0] ? rt : "-", YRT_TO_S(m->cross),
                YRT_TO_S(sim_t[i][2] - m->t_onset));
        sim_failed = 1;
    }
    e = m->fb == FB_NONE ? 0 : -m->c_end;
    sim_x[i + 1] = SIM_A * x + SIM_B * e;
    sim_fb[i] = m->fb;
    sim_rot[i] = m->rotation;
}

/* The model's column of the summary: the open loop, on the errors it would
 * see without quantization */
static void sim_print_model(int first_trial) {
    double mm = 0;
    int k;
    if (!sim_on) return;
    for (k = first_trial; k < first_trial + N_TARGETS; k++) {
        mm += model_xm / N_TARGETS;
        if (fabs(sim_x[k] - model_xm) > model_max) model_max = fabs(sim_x[k] - model_xm);
        model_xm = SIM_A * model_xm + SIM_B * (sim_fb[k] == FB_NONE ? 0 : -(sim_rot[k] + model_xm));
    }
    printf(" %10.3f", mm);
}

static int sim_check_end(int failed, int aborted, int n_rows, int n_total, int n_lost) {
    if (!sim_on) return failed;
    if (n_lost || sim_failed) failed = 1;
    if (n_rows >= N_TARGETS) {
        /* the closed loop (the endpoint shown) against the open model:
         * d(n+1) = (A - B) d(n) - B q, so |d| <= B q / (1 - A + B) */
        double bound = SIM_B * quant_deg(radius) / (1 - SIM_A + SIM_B);
        printf("hand angles within %.4f deg of the script's (quantization %.4f deg at 1/3 radius); the script within "
               "%.4f deg of the open-loop model (bound %.4f)\n", hand_dev, quant_deg(radius / 3), model_max, bound);
        if (model_max > bound) failed = 1;
    }
    if (!aborted && n_rows != n_total) failed = 1;
    if (!aborted) printf("%s: %s\n", sim_virtual ? "--sim" : "--auto", failed ? "a check failed" : "every trial came out as scripted");
    return failed;
}
