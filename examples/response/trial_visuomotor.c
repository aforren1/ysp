/* trial_visuomotor.c - center-out reaching with a visuomotor rotation (the
 * classic adaptation task: baseline, rotation, washout), driven by raw
 * mouse counts: ysp/screen.h, ysp/gfx.h, ysp/timeline.h, ysp/trials.h,
 * ysp/response.h and ysp/sig.h.
 *
 * Workspace: a start circle at the screen center, 8 targets at 45 deg
 * steps on a circle of 0.35 x the screen's smaller side (--radius px).
 * Angles are counterclockwise on the screen from rightward.
 *
 * Each trial: bring the cursor into the start circle and hold it there
 * 0.5 s. The target appears; its flip onset is time 0. Reach out through
 * it: the trial ends when the hand crosses the target radius. The cursor
 * stays frozen at the crossing point for 0.5 s (endpoint feedback), the
 * target goes off, and the hand returns to the start.
 *
 * Hand and cursor. A signal path (ysp/sig.h) makes both. The task's graph
 * reads the role "hand" (position2d, display px); an adapter makes it from
 * the device: the raw mouse's counts integrated at --gain px per count, no
 * acceleration, the system cursor hidden (relative, which the role allows);
 * SDL's pointer (--sdl-mouse); or a tablet pen's position (--pen). The
 * cursor is the hand's displacement from the start's center turned by the
 * trial's rotation (an affine node). The graph's detectors find the hold
 * (in the start circle for 0.5 s, a still hand's hold ended by a frame) and
 * the crossing of the target radius; an emit node feeds the hand to the
 * collector as derived events for the movement onset. Returning, the
 * cursor is veridical and shown only within 0.25 x the target radius of
 * the start; farther out a ring about the start shows the hand's distance
 * but not its direction, so the return shows nothing of the rotation.
 * Feedback modes: online (the cursor during the reach, then the
 * endpoint), endpoint (--endpoint-only: no cursor during the reach), none
 * (washout: neither). --sdl-mouse takes SDL's pointer instead (window
 * coordinates with the OS's acceleration, message-timed, tier 3) for a
 * machine without raw access; the data file says which path ran. --pen
 * takes a tablet's pen (SDL's pen position, absolute: the hand is where the
 * pen is, so the start needs the pen on it).
 *
 * Schedule (--schedule B,R,W, multiples of 8): baseline 40 (veridical),
 * rotation 80 at +30 deg (--rotation), washout 40 (no feedback). Abrupt;
 * --gradual ramps the rotation 1 deg per trial. Targets in random order
 * within each cycle of 8 (ysp/trials.h, YTR_ORDER_RANDOM).
 *
 * Measures. Movement onset: a DISTANCE crossing of 10% of the target
 * radius (--onset-frac; 26.9 px at the default radius) of cursor travel
 * from where the hand was at the hold's end, by the collector, from the
 * target's flip onset. The rotation keeps distances from the start, so
 * the hand and the cursor travel the same px. The data file's header
 * gives the same distance as hand travel in cm (from --cpi and --gain;
 * 0.27 cm at the defaults). A fixed hand distance would not do: at a
 * gain fast enough for a mouse it can lie past 1/3 of the radius, where
 * the angle is read. Hand angles relative to
 * the target: at the first sample past 1/3 of the
 * target radius, at peak speed (over a 10 ms lag), and at the crossing.
 * No interpolation between samples (the collector's rule, CROSSINGS).
 * At the end: the mean hand angle per cycle of 8 and the aftereffect
 * (first washout cycle minus the last baseline cycle).
 *
 * Data. --out, one row per trial: trial_index, block, cycle, target_deg,
 * rotation_deg, feedback, rt_s (movement onset from the target's onset;
 * empty after an early move), mt_s (onset to crossing), cross_s (the
 * crossing from the target's onset), hand_third_deg, hand_pv_deg,
 * hand_end_deg, cursor_end_deg, peak_speed_px_s, n_samples, input (raw,
 * sdl or script), onset_tier, onset_src, move_tier (the onset sample's
 * stamp tier: 0 on the raw path, which has no tier yet; 3 for SDL's; 4
 * scripted), rsp_flags, early_move (the hand crossed the onset distance
 * between the hold's end and the target's onset). --traj, one row
 * per hand sample from the hold's end to the crossing: trial_index, t_s
 * (from the target's onset), hand_x, hand_y, cursor_x, cursor_y (px from
 * the window's top-left). Shift+Esc stops; the rows so far are kept.
 *
 * --sim: the simulated display on the virtual clock and a scripted hand
 * fed as raw counts (or as SDL positions with --sdl-mouse) through the
 * same path. It reaches straight (minimum jerk, 0.4 s) at the target plus
 * x(n), with x(n+1) = 0.98 x(n) + 0.15 e(n), e(n) minus the shown
 * endpoint's angle from the target (0 without feedback). Trial 2 moves
 * early. The checks: each hand angle and the model within the counts'
 * quantization, the rotation, the times of the movement onset, the
 * first sample past 1/3 of the radius and the crossing to the ns, the
 * early move flagged. The checks hold at any --gain: yrsp_cursor (v0.8)
 * sums whole counts, so the hand stays on the count grid (a float sum
 * drifted 0.033 px off it in 160 trials at gain 0.1). --sim --pen feeds the
 * same hand as pen positions through the tablet adapter: its rows equal the
 * raw run's where the scripted hand is the same (CI compares them). --auto
 * runs the same hand in a window. Under 0.1 s: the virtual clock does not
 * wait.
 *
 * Usage: trial_visuomotor [--sim | --auto] [--sdl-mouse | --pen] [--schedule B,R,W] [--rotation DEG] [--gradual]
 *          [--endpoint-only] [--gain PX] [--cpi N] [--radius PX] [--onset-frac F] [--seed N] [--out FILE]
 *          [--traj FILE] [--trace-cap N]
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
/* The scripted hand of --sim and --auto */
#define SIM_A         0.98
#define SIM_B         0.15
#define SIM_MT_S      0.4     /* minimum-jerk duration                       */
#define SIM_EARLY     2       /* the trial that moves before the target      */

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

static yrsp_choice moved[1];    /* level set at run time from the radius */

static yscr_screen scr;
static int raw = 1, input = IN_RAW;
static void grab_on_window(void* p) {
    const float* xy = (const float*)p;
    SDL_HideCursor();
    SDL_SetWindowMouseGrab(yscr_window(&scr), true);
    if (!raw) SDL_WarpMouseInWindow(yscr_window(&scr), xy[0], xy[1]);
}
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event storage[64];
static ytr_trials trials;
static yrsp_collector rsp;
static ysig_graph sg;
static uint64_t sig_arena[(256u << 10) / 8];
static float sig_ch[N_SIG_CH];
/* The record node's rows, the hand and the cursor per sample (px) */
typedef struct trace_row { int64_t t; float hx, hy, cx, cy; } trace_row;
static trace_row trace[TRACE_CAP];
static int n_trace, n_trace_lost, trial_lost, trace_cap = TRACE_CAP;   /* --trace-cap: rows a reach keeps */
static uint64_t table_arena[1024];
static ytb_table tab;
static yrsp_result res;

static float gain = GAIN;               /* px per count */
static float W, H, sx, sy, radius;           /* the start's center; the target radius */
static int phase;
static double rot_deg;                 /* this trial's rotation */
static int64_t t_arm, t_cross;         /* hold end (armed), crossing */
static float cross_hx, cross_hy, cross_cx, cross_cy;

static ygfx_stim start, target, dot, ring;
static const ygfx_bind binds[] = { { .stim = &target, .param = YGFX_P_VISIBLE, .channel = TARGET_ON } };

static ygfx_stim shape(float x, float y, float d, float stroke, float r, float g, float b) {
    ygfx_shape_desc s;
    memset(&s, 0, sizeof s);
    s.shape = YGFX_CIRCLE;
    s.place = YGFX_TOP_LEFT;   /* everything here is in window pixels */
    s.x = x;
    s.y = y;
    s.w = s.h = d;
    s.stroke = stroke;
    s.color[0] = r; s.color[1] = g; s.color[2] = b;
    return ygfx_shape(&s);
}

static double dist(float x, float y) { return sqrt((double)(x - sx) * (x - sx) + (double)(y - sy) * (y - sy)); }
/* A point's direction from the start relative to the target, in (-180, 180]. */
static double rel_deg(float x, float y, double target_deg) {
    double a = atan2((double)sy - y, (double)x - sx) * 180 / VMA_PI - target_deg;
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
    if (n_trace == trace_cap) { n_trace_lost++; trial_lost++; return; }
    trace[n_trace].t = r->value.t;
    trace[n_trace].hx = (float)r->value.v[0];
    trace[n_trace].hy = (float)r->value.v[1];
    trace[n_trace].cx = (float)r->value.v[2];
    trace[n_trace].cy = (float)r->value.v[3];
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
 * input path. */
static ysig_node exp_nodes[N_EXP], ad_nodes[2];
static bool open_graph(void) {
    ysig_role role;
    ysig_adapter ad;
    ysig_desc d;
    int k;
    for (k = 0; k < N_EXP; k++) exp_nodes[k] = ysig_node_of(YSIG_OP_NONE, -1, -1);
    exp_nodes[N_HAND] = ysig_node_of(YSIG_OP_ROLE, -1, -1);
    exp_nodes[N_CUR] = ysig_node_of(YSIG_OP_AFFINE, N_HAND, -1);    /* turned about the start */
    exp_nodes[N_CUR].p[4] = exp_nodes[N_CUR].p[6] = sx;
    exp_nodes[N_CUR].p[5] = exp_nodes[N_CUR].p[7] = sy;
    exp_nodes[N_TRACE] = ysig_node_of(YSIG_OP_JOIN, N_HAND, N_CUR);
    exp_nodes[N_REC] = ysig_node_of(YSIG_OP_RECORD, N_TRACE, -1);
    exp_nodes[N_REC].name = "trace";
    exp_nodes[N_EMIT] = ysig_node_of(YSIG_OP_EMIT, N_HAND, -1);
    exp_nodes[N_EMIT].p[0] = 7;                                      /* YIN_KIND_USER + 7 */
    exp_nodes[N_HOLD] = ysig_node_of(YSIG_OP_DETECT, N_HAND, -1);   /* in the start for HOLD_S */
    exp_nodes[N_HOLD].p[0] = YSIG_TEST_LE;
    exp_nodes[N_HOLD].p[1] = -1;
    exp_nodes[N_HOLD].p[2] = START_R;
    exp_nodes[N_HOLD].p[4] = HOLD_S;
    exp_nodes[N_HOLD].p[8] = sx;
    exp_nodes[N_HOLD].p[9] = sy;
    exp_nodes[N_REACH] = ysig_node_of(YSIG_OP_DETECT, N_HAND, -1);  /* past the target radius */
    exp_nodes[N_REACH].p[0] = YSIG_TEST_GE;
    exp_nodes[N_REACH].p[1] = -1;
    exp_nodes[N_REACH].p[2] = radius;
    exp_nodes[N_REACH].p[6] = 1;                                     /* armed at the hold's end */
    exp_nodes[N_REACH].p[8] = sx;
    exp_nodes[N_REACH].p[9] = sy;
    exp_nodes[N_BIND_CUR] = ysig_node_of(YSIG_OP_BIND, N_CUR, -1);
    exp_nodes[N_BIND_CUR].p[0] = CH_CUR_X;
    exp_nodes[N_BIND_HAND] = ysig_node_of(YSIG_OP_BIND, N_HAND, -1);
    exp_nodes[N_BIND_HAND].p[0] = CH_HAND_X;
    memset(&role, 0, sizeof role);
    role.name = "hand";
    role.type = YSIG_TYPE_POSITION2D;
    role.units = YSIG_UNITS_DISPLAY_PX;
    role.allow_relative = true;   /* the hand is placed on the start at the first trial */
    memset(&ad, 0, sizeof ad);
    ad.role = "hand";
    ad.units = YSIG_UNITS_DISPLAY_PX;
    ad.nodes = ad_nodes;
    ad.n_nodes = 1;
    ad_nodes[0] = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
    ad_nodes[0].lanes = 2;
    if (input == IN_RAW) {
        /* counts integrated from the start, held inside the window */
        ad.name = "raw mouse";
        ad.type = YSIG_TYPE_POSITION2D_RELATIVE;
        ad_nodes[0].p[0] = YIN_KIND_MOUSE;
        ad_nodes[0].p[1] = YIN_AXIS_DELTA;
        ad_nodes[0].p[4] = yin_raw_mouse_source().tier;
        ad_nodes[1] = ysig_node_of(YSIG_OP_INTEGRATE, 0, -1);
        ad_nodes[1].p[0] = ad_nodes[1].p[1] = gain;
        ad_nodes[1].p[4] = sx;
        ad_nodes[1].p[5] = sy;
        ad_nodes[1].p[12] = W;    /* [0, W] x [0, H]: the count sum stays in the window */
        ad_nodes[1].p[13] = H;
        ad.n_nodes = 2;
    } else if (input == IN_SDL) {
        ad.name = "SDL pointer";
        ad.type = YSIG_TYPE_POSITION2D;
        ad_nodes[0].p[0] = YIN_KIND_MOUSE;
        ad_nodes[0].p[1] = YIN_AXIS_POSITION;
        ad_nodes[0].p[4] = yin_sdl_source(NULL, YIN_KIND_MOUSE).tier;
    } else {
        ad.name = "tablet pen";
        ad.type = YSIG_TYPE_POSITION2D;
        ad_nodes[0].p[0] = YIN_KIND_PEN;
        ad_nodes[0].p[1] = YIN_AXIS_POSITION;
        ad_nodes[0].p[4] = yin_sdl_source(NULL, YIN_KIND_PEN).tier;
    }
    memset(&d, 0, sizeof d);
    d.nodes = exp_nodes;
    d.n_nodes = N_EXP;
    d.roles = &role;
    d.n_roles = 1;
    d.adapters = &ad;
    d.n_adapters = 1;
    d.channels = sig_ch;
    d.n_channels = N_SIG_CH;
    d.on_row = on_row;
    d.arena = sig_arena;
    d.arena_size = sizeof sig_arena;
    if (!ysig_open(&sg, &d)) { fprintf(stderr, "trial_visuomotor: %s\n", ysig_error(&sg)); return false; }
    return true;
}

/* The scripted hand: one sample per ms, as counts on the gain's lattice
 * (or as the same positions for --sdl-mouse). kx, ky: its counts from the
 * start's center. The return goes straight to 2 start radii out, then one
 * sample lands on the center, so the hold starts at a known sample. */
#define SIM_N 1024
static yrsp_input sim_ev[SIM_N];
static int n_sim, sim_next;
static long sim_kx, sim_ky;
static void sim_put(int64_t t, long kx, long ky) {
    yrsp_input e;
    if (n_sim >= SIM_N) return;
    memset(&e, 0, sizeof e);
    e.kind = YRSP_KIND_MOUSE;
    e.type = YRSP_SAMPLE;
    e.stamp = YRSP_TIER_SIM;
    e.t = t;
    if (raw) {
        e.device = 7;
        e.control = YRSP_AXIS_DELTA;
        e.x = (float)(kx - sim_kx);
        e.y = (float)(ky - sim_ky);
    } else {
        /* SDL's pointer or the pen: the same hand as positions */
        if (input == IN_PEN) e.kind = YIN_KIND_PEN;
        e.control = YRSP_AXIS_POSITION;
        e.x = (float)(sx + (double)gain * kx);
        e.y = (float)(sy + (double)gain * ky);
    }
    sim_ev[n_sim++] = e;
    sim_kx = kx;
    sim_ky = ky;
}
static void sim_return(int64_t t_end) {
    double ux = (double)sim_kx, uy = (double)sim_ky, n = sqrt(ux * ux + uy * uy), g = gain;
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
    double a = deg * VMA_PI / 180, D = 1.25 * radius, g = gain;
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
static double quant_deg(double d) { return asin(0.5 * sqrt(2.0) * gain / d) * 180 / VMA_PI + 1e-9; }

static double sim_x[MAX_TRIALS + 1];
static int64_t sim_t[MAX_TRIALS][3], sim_onset[MAX_TRIALS];   /* move, 1/3 radius, crossing */

int main(int argc, char** argv) {
    yscr_desc sd;
    ytb_csv_desc cd;
    ytr_desc rd;
    yrsp_desc pd;
    yrsp_source sources[1];
    yscr_frame f;
    ytl_event fired[64];
    ytr_trial_info ti = { 0 };
    FILE *out, *traj;
    char line[1024];
    const char* out_path = "trial_visuomotor.csv";
    const char* traj_path = "trial_visuomotor_trajectory.csv";
    uint64_t seed = 20261106, seed0;
    int i, n, sim = 0, autorun = 0, gradual = 0, endpoint_only = 0, aborted = 0, failed = 0, rows = 0, in_trial = 0;
    int target_seen = 0, fb_seen = 0, reach_made = 0, sched[3] = { BASELINE_N, ROTATION_N, WASHOUT_N }, total, deg_col;
    double rotation = ROTATION_DEG, cpi = CPI, onset_px, onset_frac = ONSET_FRAC, target_deg = 0, radius_arg = 0;
    float xy[2];
    static double hand_end[MAX_TRIALS], rot_of[MAX_TRIALS], hand_dev = 0;
    static int fb_of[MAX_TRIALS];
    static int64_t rt_of[MAX_TRIALS], cross_of[MAX_TRIALS];

    memset(&sd, 0, sizeof sd);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--auto")) autorun = 1;
        else if (!strcmp(argv[i], "--sdl-mouse")) input = IN_SDL;
        else if (!strcmp(argv[i], "--pen")) input = IN_PEN;
        else if (!strcmp(argv[i], "--gradual")) gradual = 1;
        else if (!strcmp(argv[i], "--endpoint-only")) endpoint_only = 1;
        else if (!strcmp(argv[i], "--schedule") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d,%d", &sched[0], &sched[1], &sched[2]) != 3) sched[0] = -1;
        } else if (!strcmp(argv[i], "--rotation") && i + 1 < argc) rotation = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gain") && i + 1 < argc) gain = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--cpi") && i + 1 < argc) cpi = atof(argv[++i]);
        else if (!strcmp(argv[i], "--radius") && i + 1 < argc) radius_arg = atof(argv[++i]);
        else if (!strcmp(argv[i], "--onset-frac") && i + 1 < argc) onset_frac = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--traj") && i + 1 < argc) traj_path = argv[++i];
        else if (!strcmp(argv[i], "--trace-cap") && i + 1 < argc) trace_cap = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: trial_visuomotor [--sim | --auto] [--sdl-mouse | --pen] [--schedule B,R,W] [--rotation DEG] "
                            "[--gradual] [--endpoint-only] [--gain PX] [--cpi N] [--radius PX] [--onset-frac F] [--seed N] "
                            "[--out FILE] [--traj FILE]\n");
            return 2;
        }
    }
    raw = input == IN_RAW;
    if (trace_cap < 64 || trace_cap > TRACE_CAP) {
        fprintf(stderr, "trial_visuomotor: --trace-cap takes 64..%d\n", TRACE_CAP);
        return 2;
    }
    total = sched[0] + sched[1] + sched[2];
    if (sched[0] < 0 || sched[1] < 0 || sched[2] < 0 || total < N_TARGETS || total > MAX_TRIALS ||
        sched[0] % N_TARGETS || sched[1] % N_TARGETS || sched[2] % N_TARGETS) {
        fprintf(stderr, "trial_visuomotor: --schedule takes B,R,W, multiples of 8, 8..800 in all\n");
        return 2;
    }
    if (!(gain > 0) || !(cpi > 0) || !(fabs(rotation) <= 180) || radius_arg < 0 || !(onset_frac > 0 && onset_frac < 1 / 3.0)) {
        fprintf(stderr, "trial_visuomotor: --gain and --cpi must be > 0, --rotation in -180..180, --onset-frac in (0, 1/3)\n");
        return 2;
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    seed0 = seed;
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = true;
    sd.window_w = 1024;
    sd.window_h = 768;
    sd.raw_mice = raw && !sim;    /* the simulated display has no window: the script feeds counts itself */

    /* ysp/gfx.h 0.18.0 gives the simulated display the window's size, so
     * --sim has the geometry of a window run */
    if (!ygfx_open_screen(&gfx, &scr, &sd, &(ygfx_desc){ .background = { 0.3f, 0.3f, 0.3f } })) {
        fprintf(stderr, "trial_visuomotor: %s\n", ygfx_error(&gfx));
        return 1;
    }
    ygfx_size(&gfx, &W, &H);
    ygfx_center(&gfx, &sx, &sy);
    radius = radius_arg > 0 ? (float)radius_arg : (float)(RADIUS_FRAC * (W < H ? W : H));
    /* in cursor px, so it stays inside 1/3 of the radius (where the angle
     * is read) at any gain */
    onset_px = onset_frac * radius;
    start = shape(sx, sy, 2 * START_R, 0, 0.9f, 0.9f, 0.9f);
    target = shape(sx, sy, 2 * TARGET_R, 0, 0.1f, 0.8f, 0.2f);
    dot = shape(sx, sy, 8, 0, 1, 1, 0.2f);
    ring = shape(sx, sy, 10, 2, 1, 1, 0.2f);
    if (!open_graph()) return 1;
    if (!sim) {
        /* the hidden system cursor still moves, and a click goes to the
         * window under it: keep it in ours */
        xy[0] = sx;
        xy[1] = sy;
        yscr_window_call(&scr, grab_on_window, xy);
    }

    memset(&cd, 0, sizeof cd);
    cd.text = conditions_csv;
    cd.len = sizeof conditions_csv - 1;
    cd.arena = table_arena;
    cd.arena_size = sizeof table_arena;
    if (!ytb_open_csv(&tab, &cd)) { fprintf(stderr, "trial_visuomotor: %s\n", ytb_error(&tab)); return 1; }
    deg_col = ytb_col(&tab, "target_deg");
    memset(&rd, 0, sizeof rd);
    rd.table = &tab;
    rd.reps = total / N_TARGETS;
    rd.order = YTR_ORDER_RANDOM;   /* each repetition, a cycle of 8, shuffled */
    rd.rng = ytr_splitmix;
    rd.rng_ctx = &seed;
    if (!ytr_open(&trials, &rd)) { fprintf(stderr, "trial_visuomotor: %s\n", ytr_error(&trials)); return 1; }
    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS })) {
        fprintf(stderr, "trial_visuomotor: %s\n", ytl_error(&tl));
        return 1;
    }

    /* the movement onset; the window stays open for the trace until the crossing */
    moved[0].name = "moved";
    moved[0].kind = YRSP_KIND_USER + 7;    /* the emit node's derived events */
    moved[0].device = N_EMIT;
    moved[0].cross = YRSP_CROSS_DISTANCE;
    moved[0].control = YRSP_AXIS_POSITION;
    moved[0].level = (float)onset_px;
    sources[0] = raw ? yrsp_raw_mouse_source() : yrsp_sdl_source(NULL, input == IN_PEN ? YIN_KIND_PEN : YRSP_KIND_MOUSE);
    memset(&pd, 0, sizeof pd);
    pd.choices = moved;
    pd.n_choices = 1;
    pd.persist = true;
    pd.sources = sources;
    pd.n_sources = 1;
    if (!yrsp_open(&rsp, &pd)) { fprintf(stderr, "trial_visuomotor: %s\n", yrsp_error(&rsp)); return 1; }

    out = fopen(out_path, "w");
    traj = fopen(traj_path, "w");
    if (!out || !traj) { fprintf(stderr, "trial_visuomotor: cannot write %s or %s\n", out_path, traj_path); return 1; }
    yscr_describe(&scr, line, sizeof line);
    fprintf(out, "# schedule %d,%d,%d; rotation %.1f deg %s; %s feedback; seed %llu\n# %s\n", sched[0], sched[1],
            sched[2], rotation, gradual ? "gradual" : "abrupt", endpoint_only ? "endpoint" : "online",
            (unsigned long long)seed0, line);
    fprintf(out, "# ysp_response %s; hand: %s%s\n", yrsp_version(),
            raw ? "raw mice, counts integrated, no acceleration; "
                : input == IN_PEN ? "tablet pen, SDL window coordinates; " : "SDL window coordinates, OS acceleration; ",
            sim || autorun ? "scripted (tier SIM)" : sources[0].note ? sources[0].note : "");
    if (raw)
        fprintf(out, "# gain %.6g px/count, cpi %.0f, onset %.2f px (%.3g of the radius; %.3f cm of hand travel)", gain, cpi,
                onset_px, onset_frac, onset_px / gain / cpi * 2.54);
    else fprintf(out, "# onset %.2f %s px (%.3g of the radius)", onset_px, input == IN_PEN ? "pen" : "pointer", onset_frac);
    fprintf(out, "; start (%.1f, %.1f) r %.0f px; target radius %.2f px\n", sx, sy, START_R, radius);
    fprintf(out, "trial_index,block,cycle,target_deg,rotation_deg,feedback,rt_s,mt_s,cross_s,hand_third_deg,hand_pv_deg,"
                 "hand_end_deg,cursor_end_deg,peak_speed_px_s,n_samples,input,onset_tier,onset_src,move_tier,rsp_flags,"
                 "early_move\n");
    fprintf(traj, "trial_index,t_s,hand_x,hand_y,cursor_x,cursor_y\n");
    printf("%s\n", line);
    printf("signal path: %d nodes, adapter %s\n", sg.n, raw ? "raw mouse" : input == IN_PEN ? "tablet pen" : "SDL pointer");
    phase = HOLD;   /* the hand starts on the start */
    sim_x[0] = 0;

    for (;;) {
        SDL_Event ev;
        int64_t bt = 0;
        yrsp_input in;
        int rc = yscr_begin(&scr, &f);
        double d;
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_visuomotor: %s\n", yscr_error(&scr)); failed = 1; break; }

        yrsp_update_frame(&rsp, &f);   /* the target's flip record, when it arrives */

        if (!in_trial) {
            int b, k;
            if (ytr_next(&trials, &ti) < 0) break;
            b = ti.index < sched[0] ? 0 : ti.index < sched[0] + sched[1] ? 1 : 2;
            k = ti.index - sched[0];
            rot_deg = b != 1 ? 0 : gradual && (k + 1) * RAMP_DEG < fabs(rotation) ? (rotation < 0 ? -1 : 1) * (k + 1) * RAMP_DEG
                                                                                  : rotation;
            fb_of[ti.index] = b == 2 ? FB_NONE : endpoint_only ? FB_ENDPOINT : FB_ONLINE;
            rot_of[ti.index] = rot_deg;
            /* the cursor's turn, counterclockwise on the screen: y is down */
            if (!ysig_set(&sg, N_CUR, 8, -rot_deg)) { fprintf(stderr, "trial_visuomotor: rotation %g\n", rot_deg); return 1; }
            target_deg = ytb_num(&tab, ti.condition, deg_col);
            target.x = (float)(sx + radius * cos(target_deg * VMA_PI / 180));
            target.y = (float)(sy - radius * sin(target_deg * VMA_PI / 180));
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            in_trial = 1;
            target_seen = fb_seen = reach_made = 0;
            if (phase != HOLD) phase = RETURN;
            else {
                /* the first trial: the hand is on the start, and the hold
                 * detect, armed at the reset, counts from here */
                ysig_reset(&sg, f.onset);
                if (!raw && (sim || autorun)) {
                    /* a pointer or a pen is where it is: the script puts it
                     * on the start, for the graph only (the collector's
                     * window is not open; its distance starts at the arm) */
                    yin_event e, der[8];
                    memset(&e, 0, sizeof e);
                    e.t = f.onset;
                    e.kind = input == IN_PEN ? YIN_KIND_PEN : YIN_KIND_MOUSE;
                    e.type = YIN_SAMPLE;
                    e.stamp = YIN_TIER_SIM;
                    e.x = sx;
                    e.y = sy;
                    ysig_feed(&sg, &e);
                    while (ysig_derived(&sg, der, 8) > 0) { }
                }
            }
            if ((sim || autorun) && phase == RETURN) {
                int64_t t_end = f.onset + YRT_MS(400);
                if (ti.index == SIM_EARLY && f.period > 0) {
                    /* the hold ends half a frame before a vblank, so the
                     * flick has that long to cross the onset distance before
                     * the target */
                    int64_t m = (t_end + YRT_S(HOLD_S) + f.period / 2 - f.onset + f.period - 1) / f.period;
                    t_end = f.onset + m * f.period - f.period / 2 - YRT_S(HOLD_S);
                }
                sim_return(t_end);
            }
        }

        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) pointer(&in);
        while ((sim || autorun) && sim_next < n_sim && sim_ev[sim_next].t <= f.onset) pointer(&sim_ev[sim_next++]);

        /* the graph at the frame's onset, after its input: the binds, and
         * the hold's end when no sample said so (the hold detect). The
         * target goes on the first frame whose onset is at or after it, so
         * this is decided up to a frame early, and a hand that leaves in
         * that last frame counts as an early move, not as a broken hold */
        /* -1 can only be a programming error (a bad argument) */
        if (ysig_frame(&sg, &(ysig_frame_in){ f.index, f.onset, 0, NULL, 0, sim || autorun ? f.onset : yrt_now_ns() }) != 0) {
            fprintf(stderr, "trial_visuomotor: %s\n", ysig_error(&sg));
            failed = 1;
            break;
        }
        frame_hits();
        if (phase >= REACH && !target_seen) {   /* the target on this frame */
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_on(&q, TARGET_ON);
            if (q.err) { fprintf(stderr, "trial_visuomotor: timeline error %d\n", q.err); failed = 1; break; }
            target_seen = 1;
            if ((sim || autorun) && ti.index == SIM_EARLY) {
                sim_reach(t_arm, target_deg + sim_x[ti.index], 1, onset_px, sim_t[ti.index]);
                reach_made = 1;
            }
        }
        /* the script's reach: from the target's flip onset, as a participant
         * would see it (the collector's onset once final) */
        if ((sim || autorun) && !reach_made && phase == REACH) {
            yrsp_onset on;
            if (yrsp_onset_now(&rsp, &on) && on.final) {
                sim_onset[ti.index] = on.t_ns;
                sim_reach(on.t_ns + YRT_MS(300 + 10 * (ti.index % 7)), target_deg + sim_x[ti.index], 0, onset_px,
                          sim_t[ti.index]);
                reach_made = 1;
            }
        }
        if (phase == FEEDBACK && !fb_seen) {   /* frozen endpoint, then the target off */
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_wait(&q, YRT_S(FEEDBACK_S));
            ytl_off(&q, TARGET_ON);
            ytl_mark(&q, END_OF_TRIAL, 0);
            if (q.err) { fprintf(stderr, "trial_visuomotor: timeline error %d\n", q.err); failed = 1; break; }
            fb_seen = 1;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        yrsp_set_landing(&rsp, fired, n, TARGET_ON);   /* the plan, until the flip record */
        for (i = 0; i < n; i++) {
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRIAL) {
                /* the onset is final; the trace holds the reach */
                const yrsp_entry* ent;
                const trace_row *p, *lag = NULL, *third = NULL, *pv = NULL;
                char rt[32], mt[32];
                int k, j = 0, n_ent, early = 0, n_samples = 0, fb = fb_of[ti.index];
                double vmax = 0, a_third = NAN, a_pv = NAN, a_end, c_end;
                yrsp_finish(&rsp, &res);
                ent = yrsp_entries(&rsp, &n_ent);
                for (k = 0; k < n_ent; k++)
                    if ((ent[k].flags & YRSP_E_CROSSING) && (ent[k].flags & YRSP_E_EARLY)) early = 1;
                for (k = 0; k < n_trace && trace[k].t <= t_cross; k++) {
                    p = &trace[k];
                    fprintf(traj, "%d,%.9f,%.3f,%.3f,%.3f,%.3f\n", ti.index, (double)(p->t - res.t_onset) / 1e9, p->hx,
                            p->hy, p->cx, p->cy);
                    n_samples++;
                    if (!third && dist(p->hx, p->hy) >= radius / 3) third = p;
                    /* speed over at least PV_LAG_S: one report per ms is too
                     * coarse in counts for a direction or a speed */
                    while (j < k && p->t - trace[j + 1].t >= YRT_S(PV_LAG_S)) j++;
                    lag = &trace[j];
                    if (p->t - lag->t >= YRT_S(PV_LAG_S)) {
                        double v = sqrt((double)(p->hx - lag->hx) * (p->hx - lag->hx) +
                                        (double)(p->hy - lag->hy) * (p->hy - lag->hy)) / YRT_TO_S(p->t - lag->t);
                        if (v > vmax) { vmax = v; pv = p; }
                    }
                }
                if (third) a_third = rel_deg(third->hx, third->hy, target_deg);
                if (pv) a_pv = rel_deg(pv->hx, pv->hy, target_deg);
                a_end = rel_deg(cross_hx, cross_hy, target_deg);
                c_end = rel_deg(cross_cx, cross_cy, target_deg);
                rt[0] = mt[0] = 0;
                if (res.rt_ns != YRSP_NO_TIME && !early) {
                    snprintf(rt, sizeof rt, "%.9f", YRT_TO_S(res.rt_ns));
                    snprintf(mt, sizeof mt, "%.9f", YRT_TO_S(t_cross - res.t_response));
                }
                fprintf(out, "%d,%s,%d,%.0f,%.3f,%s,%s,%s,%.9f,%.4f,%.4f,%.4f,%.4f,%.1f,%d,%s,%u,%u,%u,%u,%d\n", ti.index,
                        block_name[ti.index < sched[0] ? 0 : ti.index < sched[0] + sched[1] ? 1 : 2],
                        ti.index / N_TARGETS, target_deg, rot_deg, fb_name[fb], rt, mt, YRT_TO_S(t_cross - res.t_onset),
                        a_third, a_pv, a_end, c_end, vmax, n_samples,
                        sim || autorun ? "script" : raw ? "raw" : input == IN_PEN ? "pen" : "sdl",
                        (unsigned)res.onset_tier, (unsigned)res.onset_src, (unsigned)res.stamp_tier, (unsigned)res.flags,
                        early);
                fflush(out);
                fflush(traj);
                if (!sim)
                    printf("trial %3d target %3.0f rotation %5.1f %-8s rt %s hand %7.2f cursor %7.2f deg%s\n", ti.index,
                           target_deg, rot_deg, fb_name[fb], rt[0] ? rt : "-", a_end, c_end, early ? " (early)" : "");
                hand_end[ti.index] = a_end;
                rt_of[ti.index] = early ? YRSP_NO_TIME : res.rt_ns;
                cross_of[ti.index] = t_cross - res.t_onset;
                if (sim || autorun) {
                    /* the script's own checks: each angle within the
                     * quantization of its distance, the rotation exact to
                     * float, the times to the ns */
                    double x = sim_x[ti.index], e;
                    double dev = fabs(a_end - x) - quant_deg(dist(cross_hx, cross_hy));
                    hand_dev = fmax(hand_dev, fmax(fabs(a_end - x), fmax(fabs(a_third - x), fabs(a_pv - x))));
                    if (third) dev = fmax(dev, fabs(a_third - x) - quant_deg(dist(third->hx, third->hy)));
                    if (pv) dev = fmax(dev, fabs(a_pv - x) - quant_deg(dist(pv->hx, pv->hy)));
                    dev = fmax(dev, fabs(c_end - a_end - rot_deg) - 1e-3);
                    if (dev > 0 || !third || !pv || early != (ti.index == SIM_EARLY) ||
                        cross_of[ti.index] != sim_t[ti.index][2] - res.t_onset || third->t != sim_t[ti.index][1] ||
                        (!early && (rt_of[ti.index] != sim_t[ti.index][0] - sim_onset[ti.index] ||
                                    res.t_onset != sim_onset[ti.index]))) {
                        fprintf(stderr, "trial_visuomotor: check: trial %d: hand %.4f third %.4f pv %.4f (script %.4f), "
                                        "cursor %.4f, early %d, rt %s, cross %.9f s (script %.9f)\n",
                                ti.index, a_end, a_third, a_pv, x, c_end, early, rt[0] ? rt : "-",
                                YRT_TO_S(cross_of[ti.index]), YRT_TO_S(sim_t[ti.index][2] - res.t_onset));
                        failed = 1;
                    }
                    /* it learns from the endpoint it was shown */
                    e = fb == FB_NONE ? 0 : -c_end;
                    sim_x[ti.index + 1] = SIM_A * x + SIM_B * e;
                }
                ytr_update(&trials, 1, NULL);
                rows++;
                in_trial = 0;
                phase = RETURN;
                /* the trial's trajectory is short by the rows past the
                 * cap: said here, not in a column (the data file keeps its
                 * columns) */
                if (trial_lost) printf("trial %d: %d trajectory rows lost past --trace-cap %d\n", ti.index, trial_lost, trace_cap);
                trial_lost = 0;
                n_trace = 0;
                ysig_arm(&sg, N_HOLD, f.onset);   /* the next hold, once the hand is back */
            }
        }
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), ytl_values(&tl));
        d = dist(sig_ch[CH_HAND_X], sig_ch[CH_HAND_Y]);
        dot.visible = 1;
        ring.visible = 0;
        if (phase == FEEDBACK) {
            dot.x = cross_cx;
            dot.y = cross_cy;
            dot.visible = fb_of[ti.index] != FB_NONE ? 1.0f : 0.0f;
        } else if (phase == REACH) {
            dot.x = sig_ch[CH_CUR_X];
            dot.y = sig_ch[CH_CUR_Y];
            dot.visible = fb_of[ti.index] == FB_ONLINE ? 1.0f : 0.0f;
        } else {
            dot.x = sig_ch[CH_HAND_X];
            dot.y = sig_ch[CH_HAND_Y];
            if (phase == RETURN && d > SHOW_FRAC * radius) {
                dot.visible = 0;
                ring.visible = 1;
                ring.w = ring.h = (float)(2 * d);
            }
        }
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
    printf("%d trials written to %s, samples to %s%s\n", rows, out_path, traj_path,
           aborted ? " (stopped with Shift+Esc)" : "");
    printf("trace rows lost (a reach longer than --trace-cap %d): %d\n", trace_cap, n_trace_lost);
    if ((sim || autorun) && n_trace_lost) failed = 1;

    /* The learning curve: the mean endpoint hand angle per cycle of 8. */
    if (rows >= N_TARGETS) {
        double xm = 0, model_max = 0, last_base = NAN, first_wash = NAN;
        printf("cycle block     rotation  hand_deg%s\n", sim || autorun ? "  model_deg" : "");
        for (i = 0; i + N_TARGETS <= rows; i += N_TARGETS) {
            double m = 0, r = 0, mm = 0;
            int k, b = i < sched[0] ? 0 : i < sched[0] + sched[1] ? 1 : 2;
            for (k = i; k < i + N_TARGETS; k++) {
                m += hand_end[k] / N_TARGETS;
                r += rot_of[k] / N_TARGETS;
                /* the model on the errors it would see without quantization */
                mm += xm / N_TARGETS;
                if (fabs(sim_x[k] - xm) > model_max) model_max = fabs(sim_x[k] - xm);
                xm = SIM_A * xm + SIM_B * (fb_of[k] == FB_NONE ? 0 : -(rot_of[k] + xm));
            }
            if (b == 0) last_base = m;
            if (b == 2 && isnan(first_wash)) first_wash = m;
            if (sim || autorun) printf("%5d %-9s %8.2f %9.3f %10.3f\n", i / N_TARGETS, block_name[b], r, m, mm);
            else printf("%5d %-9s %8.2f %9.3f\n", i / N_TARGETS, block_name[b], r, m);
        }
        if (!isnan(first_wash))
            printf("aftereffect: %.3f deg (first washout cycle %.3f minus last baseline cycle %.3f)\n",
                   first_wash - (isnan(last_base) ? 0 : last_base), first_wash, isnan(last_base) ? 0 : last_base);
        if (sim || autorun) {
            /* the closed loop (the endpoint shown) against the open model:
             * d(n+1) = (A - B) d(n) - B q, so |d| <= B q / (1 - A + B) */
            double bound = SIM_B * quant_deg(radius) / (1 - SIM_A + SIM_B);
            printf("hand angles within %.4f deg of the script's (quantization %.4f deg at 1/3 radius); the script within "
                   "%.4f deg of the open-loop model (bound %.4f)\n", hand_dev, quant_deg(radius / 3), model_max, bound);
            if (model_max > bound) failed = 1;
        }
    }
    if ((sim || autorun) && !aborted && rows != total) failed = 1;
    if ((sim || autorun) && !aborted) printf("%s: %s\n", sim ? "--sim" : "--auto", failed ? "a check failed" :
                                             "every trial came out as scripted");
    return failed;
}
