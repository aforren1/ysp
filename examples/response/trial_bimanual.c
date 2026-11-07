/* trial_bimanual.c - continuous tracking and point-to-point reaches with
 * one mouse per hand, under a rotation or a mirror (Yang, Cowan and Haith
 * 2021, eLife 10:e62578, made bimanual): ysp/screen.h's raw mice,
 * ysp/gfx.h, ysp/timeline.h, ysp/trials.h, ysp/response.h and ysp/sig.h.
 *
 * Hands. Two mice, read through ysp/screen.h's desc.raw_mice. A signal
 * path (ysp/sig.h) makes every value from them: each mouse's counts
 * integrated at --gain px per count (no acceleration), each hand in cm,
 * the mapping, the perturbation, the cursor's speed, the target (sines at
 * the frame's base time times the timeline's ramp), the derived events
 * the collector reads, and each value at a flip record's onset from the
 * hands' rings. L and R are each hand's displacement in cm (y up) from
 * where it was placed at the end of the setup; a hand stops at the
 * window's size from there. Setup: click the left hand's mouse, then the
 * right hand's (left button); the console says which, and the lit box
 * (left or right) shows it. A click with device 0 and the same mouse twice
 * are refused. After the setup, every other mouse is ignored and counted.
 * Mappings: --mapping split (the default): the cursor's x from L's x, its
 * y from R's y; L's y and R's x are recorded, not used. --mapping mean:
 * (L + R) / 2. The mapped vector is u. The cursor is P u: P is the
 * identity, or under a perturbation (a "*" block) --perturb rotation
 * (--rotation DEG, default -90: clockwise, as the paper) or mirror (x and
 * y swap: the mirror about the 45 deg axis). With split and mirror the
 * hands swap axes: L's x moves the cursor up, R's y moves it right.
 *
 * Sizes are cm through --px-per-cm. Without it the geometry is NOMINAL:
 * the window's smaller side is 24 cm (32 px/cm in the 1024 x 768
 * window), marked NOMINAL in the data files' headers; ysp/screen.h does
 * not report the screen's size.
 *
 * Tracking trial (T). A gray target (radius 0.8 cm) at the center; the
 * cursor (radius 0.25 cm) must be in it and still (under 6.5 cm/s over a
 * frame), at least 1 s after the last trial. Then the target moves for
 * --track-s (46) s, from the first moving frame's onset: per axis r(t) =
 * A(t) sum_i a_i cos(2 pi w_i t + phi_i), A rising linearly from 0 to 1
 * over the first 5 s (a timeline tween), then 1.
 *   x: a = 2.31 2.31 2.31 1.76 1.30 0.97 0.73 cm, w = 0.10 0.25 0.55 0.85 1.15 1.55 2.05 Hz
 *   y: a = 2.31 2.31 2.31 1.58 1.03 0.81 0.70 cm, w = 0.15 0.35 0.65 0.95 1.45 1.85 2.15 Hz
 * The phases are uniform in [-pi, pi), drawn per block from --seed, the
 * same for every trial of the block (the header lists them). The target
 * is yellow while the cursor's center is inside it. Each frame's target
 * is the sum at that frame's planned onset; its hand values are each
 * mouse's last report at or before the frame's onset from its flip
 * record. The analysis takes the last 40 s (the last 20 s when --track-s
 * is under 46), so the stimulated frequencies, prime multiples of 0.05
 * Hz, fall on bins. Sums run over the frames' real onsets, each frame
 * weighted by its time on screen, so a missed frame does not shift a bin.
 * Measures: MSE (cursor to target), the fraction of the window inside,
 * the alignment matrix M = argmin ||H - M T||^2 (u on the target),
 * S_rotation = (c - b) / 2 and S_mirror = (b + c) / 2 with M = [a b; c d],
 * theta = atan2(R21, R11) of R = U V^T from M's SVD; the same M and theta
 * for L alone and R alone; the DFT of each signal at each stimulated
 * frequency. All at the trial's end, not in the frame loop.
 *
 * Reach block (R). A gray start circle (radius 1 cm) at the center; after
 * the cursor has been in it for 0.5 s a target (radius 1 cm) appears 12
 * cm away in a random direction, inside a 20 x 20 cm workspace about the
 * center. Move to it in a straight line, as fast and accurately as you
 * can. Once the cursor has been in it and still for 1 s, the next target
 * appears 12 cm from there; the old one is the new start. The target
 * turns yellow when the hold starts if the peak speed was over 39 cm/s,
 * else blue (the paper's tone is left out). Movement onset: the cursor
 * leaves the start circle, a DISTANCE crossing of 1 cm from the start's
 * center on the cursor's position (the collector, fed one position per
 * hand report). Initial direction error: the angle of the cursor's
 * velocity over 140 to 150 ms after the onset, minus the target's
 * direction from the start's center.
 *
 * Blocks (--blocks): T and R, "*" for perturbed. The default
 * T,R,T*,R*,T*,R*,T*,R*,T*,T is the paper's main experiment: baseline
 * tracking and reaching, early tracking under the perturbation, three
 * alternations of reaching and tracking, late tracking, then tracking with
 * the perturbation off (the aftereffect). --track-trials N (default 2;
 * the paper's 8) per T block, --reaches N (default 30; the paper's 150)
 * per R block.
 *
 * Data, seconds and cm. --out, one row per tracking trial: trial_index,
 * block, block_trial, perturbation, mapping, mse_cm2, frac_inside, m_a,
 * m_b, m_c, m_d, theta_deg, s_rotation, s_mirror, then l_a .. l_d,
 * l_theta_deg and r_a .. r_theta_deg (L alone, R alone), n_frames,
 * n_missed (frames whose record says a vblank was missed), n_norecord,
 * n_other (other mice's reports in the trial), onset_tier, onset_src (the
 * first moving frame's). --reaches-out, one row per reach: reach_index,
 * block, block_reach, perturbation, start_x_cm, start_y_cm, target_x_cm,
 * target_y_cm, target_deg, rt_s (the onset from the target's flip onset),
 * init_err_deg, peak_speed_cm_s, fast, mt_s (onset to the hold's start),
 * onset_tier, onset_src, move_tier (the crossing report's stamp tier),
 * move_hand (the hand whose report crossed), rsp_flags. --track, one row
 * per moving frame: trial_index, t_s, target_x, target_y, cursor_x,
 * cursor_y, l_x, l_y, r_x, r_y, inside. --freq, per tracking trial and
 * stimulated frequency: trial_index, block, axis, freq_hz, then the
 * complex amplitude (re, im) of the target on that axis and of u x, u y,
 * L x, L y, R x and R y. Shift+Esc stops; the rows so far are kept.
 *
 * --sim: the simulated display on the virtual clock, and a scripted
 * participant as counts from three mice: L reports on whole ms, R 0.5 ms
 * after, a third mouse at 125 Hz that moves all the time and must change
 * nothing. The setup is scripted too: a click with device 0 and L twice,
 * both refused. Tracking: u(t) = g Rot(theta_b) r(t - tau), g 0.8, tau
 * 0.25 s, theta_b 0 at baseline, a partial compensation growing over the
 * perturbed blocks, a quarter of it after (the aftereffect). Split: L's y
 * and R's x carry 0.2 of the other hand's axis (crosstalk). Mean: L = u +
 * d and R = u - d, d a 1.5 cm sinusoid at 0.20 Hz (no stimulated
 * frequency), which must cancel in the cursor. Reaches: minimum jerk, 12
 * cm at a known direction error, then a correction into the target;
 * every third one slow. Three checks per tracking trial. (1) The data
 * path: every frame's L, R, u, cursor and drawn cursor equal the script's
 * exact values at the same report times within the count grid (half a
 * count per axis). (2) The math: the analysis of the script's model at
 * the exact onsets gives g and phase -2 pi f tau at every stimulated
 * frequency, theta_b, the closed-form MSE, M = g Rot(theta_b) diag(cx,
 * cy), the crosstalk or d, all within 1e-6. (3) The trial's MSE, M,
 * theta, S and DFTs equal those of the exact report values within bounds
 * derived from the count grid. The 1 kHz report grid itself (up to 1 ms
 * of hold) is the gap between (1) and the model; the run prints it (the
 * defaults: gain 0.5 %, phase 0.46 deg, theta 0.003 deg, MSE 0.2 %).
 * Reaches: the onset to the ns, the initial direction error within the
 * count grid, fast or slow as scripted. Over a 10 ms lag the count grid
 * moves the direction up to 1.9 deg from the script's line; the hands'
 * reports 0.5 ms apart move it 0.055 deg. The default --sim (12 trials of
 * 46 s and 120 reaches, 14 virtual minutes) took 0.42 s of wall time on
 * the Linux desktop (Release, quiet, one performance core), 0.34 s before
 * the signal path: the graph's 1.7 million hand reports cost about 50 ns
 * each more than the hand-written arithmetic.
 * --auto: the same script in a window, checks (1) and (3) and the reaches.
 *
 * Usage: trial_bimanual [--sim | --auto] [--mapping split|mean] [--perturb rotation|mirror] [--rotation DEG]
 *          [--blocks LIST] [--track-trials N] [--reaches N] [--track-s S] [--gain PX] [--cpi N]
 *          [--px-per-cm PX] [--fullscreen] [--seed N] [--out FILE] [--reaches-out FILE] [--track FILE] [--freq FILE]
 *   --gain PX     px per count (default 0.25; at 1000 CPI and the nominal
 *                 32 px/cm, 1 cm of hand moves the cursor 3.1 cm)
 *   --cpi N       the mice's counts per inch, for the header's cm (default 1000)
 *   --track-s S   seconds of target motion, 26 to 46 (default 46)
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

/* The paper's numbers (cm, s), in one place. */
#define TARGET_R      0.8     /* tracking target                              */
#define CURSOR_R      0.25
#define STILL_CM_S    6.5     /* 0.065 m/s                                    */
#define RAMP_S        5.0
#define TRACK_S       46.0
#define ITI_S         1.0     /* between tracking trials, at least            */
#define START_R       1.0     /* reach start circle; reach targets too        */
#define REACH_CM      12.0
#define WORK_CM       10.0    /* half of the 20 x 20 cm workspace             */
#define START_HOLD_S  0.5
#define TARGET_HOLD_S 1.0
#define FAST_CM_S     39.0    /* about 0.39 m/s                               */
#define INIT_S        0.150   /* the direction is read this long after onset */
#define VEL_LAG_S     0.010   /* velocity over this lag                       */
#define NOMINAL_CM    24.0    /* the window's smaller side without --px-per-cm */
#define GAIN          0.25f   /* px per count                                 */
#define CPI           1000.0
#define ROTATION_DEG  (-90.0) /* clockwise                                    */
#define N_SINES       7
#define MAX_BLOCKS    64
#define MAX_REACHES   4096
#define FRAME_CAP     (46 * 240 + 256)   /* 46 s at 240 Hz                    */
#define TRACE_CAP     32768              /* 16 s of two 1 kHz mice; 1.5 MB    */
#define RING_CAP      4096               /* per hand: 4 s at 1 kHz            */
/* The signal path: the hands, the mapping, the cursor, the target */
enum { B_SRC_L, B_SRC_R, B_LPX, B_RPX, B_L, B_R, B_LX, B_RY, B_U, B_C, B_EMIT, B_SPEED, B_AMP, B_SINES, B_TGT,
       B_ERR, B_INSIDE, B_TPX, B_BIND_T, B_N };
enum { CH_TGT_X, CH_TGT_Y, N_SIG_CH };
#define BM_PI         3.14159265358979323846
/* The scripted participant (--sim, --auto) */
#define SIM_G         0.8
#define SIM_TAU_S     0.25
#define SIM_KAPPA     0.2     /* split: crosstalk                             */
#define SIM_D_CM      1.5     /* mean: L = u + d, R = u - d                   */
#define SIM_D_HZ      0.20
#define SIM_RT_S      0.30
#define SIM_FAST_S    0.40    /* minimum-jerk durations: peak 56 or 25 cm/s   */
#define SIM_SLOW_S    0.90
#define SIM_CORR_S    0.35
#define SIM_RETURN_S  1.0
#define SIM_MIRROR_DEG 40.0   /* the rotation the script tries under a mirror */
enum { DEV_L = 1101, DEV_R = 1102, DEV_O = 1103 };   /* --sim's mice */

static const double amp_cm[2][N_SINES] = { { 2.31, 2.31, 2.31, 1.76, 1.30, 0.97, 0.73 },
                                           { 2.31, 2.31, 2.31, 1.58, 1.03, 0.81, 0.70 } };
static const double freq_hz[2][N_SINES] = { { 0.10, 0.25, 0.55, 0.85, 1.15, 1.55, 2.05 },
                                            { 0.15, 0.35, 0.65, 0.95, 1.45, 1.85, 2.15 } };

enum { TARGET_ON, MOVING, AMP, N_CHANNELS };
enum { TRIAL = 1 };
enum { END_OF_TRACK = 1 };
enum { S_NEXT, S_T_WAIT, S_T_MOVE, S_T_FINISH, S_R_START, S_R_REACH, S_DONE };
static const char* const pert_name[] = { "none", "rotation", "mirror" };

static yscr_screen scr;
static void grab_on_window(void* unused) {
    (void)unused;
    SDL_HideCursor();
    SDL_SetWindowMouseGrab(yscr_window(&scr), true);
}
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event storage[64];
static ytl_key keys[16];
static ytr_trials trials;
static ytb_table tab;
static uint64_t table_arena[2048];
static char blocks_csv[MAX_BLOCKS * 8 + 32];
static int order_list[YTR_MAX_TRIALS];
static yrsp_collector rsp;
static yrsp_input trace[TRACE_CAP];
static yrsp_result res;

/* --- geometry and the hands -------------------------------------------- */

static float W, H, cx, cy;         /* window size and center, px */
static double ppc;                 /* px per cm */
static int mapping;                /* 0 split, 1 mean */
static int perturb;                /* 1 rotation, 2 mirror (pert_name) */
static int pert_on;                /* this block's */
static double rot_rad, rot_deg;     /* the rotation; rot_rad = rot_deg pi / 180 */
static uint32_t dev[2];            /* the hands' mice: L, R */
static float gain;                 /* px per count */
static int n_other, n_dev0;        /* reports from other mice; device 0 */
static ysig_graph sg;
static uint64_t sig_arena[(2u << 20) / 8];
static float sig_ch[N_SIG_CH];

/* A hand's displacement from where it was placed, cm, y up, from its
 * position in px. */
static void hand_cm(float x, float y, double out[2]) {
    out[0] = ((double)x - (double)W) / ppc;
    out[1] = -((double)y - (double)H) / ppc;
}
static void map_u(const double L[2], const double R[2], double u[2]) {
    if (mapping) { u[0] = (L[0] + R[0]) / 2; u[1] = (L[1] + R[1]) / 2; }
    else { u[0] = L[0]; u[1] = R[1]; }
}
/* c = P u, and its inverse */
static void apply_p(int on, const double u[2], double c[2]) {
    double x = u[0], y = u[1];
    if (!on) { c[0] = x; c[1] = y; }
    else if (perturb == 2) { c[0] = y; c[1] = x; }
    else { c[0] = cos(rot_rad) * x - sin(rot_rad) * y; c[1] = sin(rot_rad) * x + cos(rot_rad) * y; }
}
static void unapply_p(int on, const double c[2], double u[2]) {
    double x = c[0], y = c[1];
    if (!on) { u[0] = x; u[1] = y; }
    else if (perturb == 2) { u[0] = y; u[1] = x; }
    else { u[0] = cos(rot_rad) * x + sin(rot_rad) * y; u[1] = -sin(rot_rad) * x + cos(rot_rad) * y; }
}
static void cursor_of(const double L[2], const double R[2], double u[2], double c[2]) {
    map_u(L, R, u);
    apply_p(pert_on, u, c);
}
static float px_x(double x) { return (float)(cx + x * ppc); }
static float px_y(double y) { return (float)(cy - y * ppc); }

/* The collector's own distance, so the script predicts its crossing
 * exactly (ysp/response.h, yrsp__sample: float differences, then double) */
static float coll_dist(float x, float y, float rx, float ry) {
    return (float)sqrt((double)(x - rx) * (x - rx) + (double)(y - ry) * (y - ry));
}

/* A node's value at t (a flip record's onset): the hands keep rings of
 * their values, so the mapping and the cursor at t come from theirs. */
static int ring_miss;
static void value_at(int node, int64_t t, double out[2]) {
    ysig_value v;
    if (ysig_value_at(&sg, node, t, &v) < 1) ring_miss++;   /* older than the ring: its oldest */
    out[0] = v.v[0];
    out[1] = v.v[1];
}

/* --- the target --------------------------------------------------------- */

static double phase[MAX_BLOCKS][2][N_SINES];
static void target_at(int b, double t, double amp, double out[2]) {
    int a, i;
    for (a = 0; a < 2; a++) {
        double s = 0;
        for (i = 0; i < N_SINES; i++) s += amp_cm[a][i] * cos(2 * BM_PI * freq_hz[a][i] * t + phase[b][a][i]);
        out[a] = amp * s;
    }
}
static double ramp_of(double t) { return t <= 0 ? 0 : t < RAMP_S ? t / RAMP_S : 1; }

/* --- input -------------------------------------------------------------- */

static int setup = 0, refused = 0;     /* 0: waiting for L, 1: for R, 2: done */
static int is_script;
static void script_placed(int64_t t);
/* The cursor's speed over VEL_LAG_S, the graph's speed node at each hand
 * report: the target's color needs the peak at the hold's start (the
 * collector's trace is readable only after yrsp_finish(); F19). */
static double peak_run;                /* cm/s since the target appeared */

/* Every mouse event: the setup's clicks, then each hand's counts. After a
 * hand's report the collector gets the cursor's position (cm), stamped
 * and tagged with that report's time, tier and device. */
/* clicks by path (docs/examples_findings.md B8): SDL's own pointer, every
 * mouse as one, and the hands' mice on the raw path after the setup */
static int n_clicks_sdl[2], n_clicks_raw[2];
static void on_input(const yrsp_input* in, int from_bridge) {
    int h;
    if (in->kind != YRSP_KIND_MOUSE) return;
    if (!from_bridge) {
        if (in->type == YRSP_PRESS || in->type == YRSP_RELEASE) n_clicks_sdl[in->type == YRSP_RELEASE]++;
        return;
    }
    if (setup < 2) {
        if (in->type != YRSP_PRESS || in->control != 1) return;
        if (!in->device || (setup == 1 && in->device == dev[0])) {
            refused++;
            printf("  refused: %s\n", in->device ? "the left hand's mouse again" : "a click with no device id");
            return;
        }
        dev[setup] = in->device;
        {
            const char* nm = is_script ? "scripted" : SDL_GetMouseNameForID((SDL_MouseID)in->device);
            printf("%s hand: mouse %u (%s)%s\n", setup ? "right" : "left", (unsigned)in->device, nm && *nm ? nm : "no name",
                   (in->flags & YRSP_IN_UNLISTED) ? ", not in SDL's list" : "");
        }
        if (++setup == 1) printf("click the right hand's mouse (left button)\n");
        else {
            /* both placed now: L and R are displacements from here */
            ysig_set(&sg, B_SRC_L, 2, dev[0]);
            ysig_set(&sg, B_SRC_R, 2, dev[1]);
            ysig_reset(&sg, in->t);
            if (is_script) script_placed(in->t);
        }
        return;
    }
    h = in->device == dev[0] ? 0 : in->device == dev[1] ? 1 : -1;
    if (h < 0) {
        if (in->device) n_other++;
        else n_dev0++;
        return;
    }
    if (in->type == YRSP_PRESS || in->type == YRSP_RELEASE) n_clicks_raw[in->type == YRSP_RELEASE]++;
    if (ysig_feed(&sg, in) > 0) {
        /* the cursor (cm) to the collector, stamped and tagged with this
         * report's time and the worse tier of the two hands */
        yin_event der[8];
        int n = ysig_derived(&sg, der, 8), k;
        double v = ysig_value_of(&sg, B_SPEED)->v[0];
        for (k = 0; k < n; k++) yrsp_feed(&rsp, &der[k]);
        if (v > peak_run) peak_run = v;
    }
}

/* --- the scripted participant (--sim, --auto) --------------------------- */

/* Its plan: u(t) in cm as segments, newest last. HOLD keeps a; MJ goes
 * from a to b by minimum jerk over [t0, t1], then stays; TRACK is g
 * Rot(theta) r(t - t0 - tau) for the block's target. */
enum { SEG_HOLD, SEG_MJ, SEG_TRACK };
typedef struct seg { int kind, block; int64_t t0, t1; double a[2], b[2], theta; } seg;
#define SEG_CAP 32
static seg plan[SEG_CAP];
static int n_plan;
static void seg_u(const seg* s, int64_t t, double u[2]) {
    if (s->kind == SEG_MJ) {
        double v = t <= s->t0 ? 0 : t >= s->t1 ? 1 : (double)(t - s->t0) / (double)(s->t1 - s->t0);
        double m = v * v * v * (10 - 15 * v + 6 * v * v);
        u[0] = s->a[0] + (s->b[0] - s->a[0]) * m;
        u[1] = s->a[1] + (s->b[1] - s->a[1]) * m;
    } else if (s->kind == SEG_TRACK) {
        double r[2], tt = (double)((t < s->t1 ? t : s->t1) - s->t0) / 1e9 - SIM_TAU_S;
        target_at(s->block, tt, ramp_of(tt), r);
        u[0] = SIM_G * (cos(s->theta) * r[0] - sin(s->theta) * r[1]);
        u[1] = SIM_G * (sin(s->theta) * r[0] + cos(s->theta) * r[1]);
    } else {
        u[0] = s->a[0];
        u[1] = s->a[1];
    }
}
static void plan_u(int64_t t, double u[2]) {
    int k;
    for (k = n_plan - 1; k > 0 && plan[k].t0 > t; k--) { }
    if (n_plan) seg_u(&plan[k], t, u);
    else u[0] = u[1] = 0;
}
static void plan_add(seg s) {
    if (n_plan == SEG_CAP) { memmove(plan, plan + 1, sizeof plan[0] * (SEG_CAP - 1)); n_plan--; }
    plan[n_plan++] = s;
}
static void plan_mj(int64_t t0, double dur_s, const double b[2]) {
    seg s;
    memset(&s, 0, sizeof s);
    s.kind = SEG_MJ;
    s.t0 = t0;
    s.t1 = t0 + YRT_S(dur_s);
    plan_u(t0, s.a);
    s.b[0] = b[0];
    s.b[1] = b[1];
    plan_add(s);
}

/* The script's clocks: placement, then each mouse's report grid. */
static struct {
    int64_t ts;                    /* the hands were placed (0: not yet) */
    int64_t next[3], period[3], offset[3];
    int64_t n[3][2];               /* counts sent per mouse */
    int64_t t_start;
    /* the reach onset the collector must find: the first report past
     * `level` from `origin`, from `from` on */
    int watch, found;
    int64_t from, t_found;
    float ox, oy, level;
} sc;
static yrsp_input clicks[32];
static int n_clicks, next_click;
static void click_add(int64_t t, uint32_t device) {
    int k, type;
    for (type = 0; type < 2 && n_clicks < 32; type++) {
        yrsp_input e;
        memset(&e, 0, sizeof e);
        e.kind = YRSP_KIND_MOUSE;
        e.type = type ? YRSP_RELEASE : YRSP_PRESS;
        e.t = t + (type ? YRT_MS(80) : 0);
        e.device = device;
        e.control = 1;
        e.stamp = YRSP_TIER_SIM;
        e.value = type ? 0.0f : 1.0f;
        for (k = n_clicks; k > next_click && clicks[k - 1].t > e.t; k--) clicks[k] = clicks[k - 1];
        clicks[k] = e;
        n_clicks++;
    }
}
static void script_placed(int64_t t) {
    seg s;
    memset(&s, 0, sizeof s);
    sc.ts = t;
    n_plan = 0;
    s.kind = SEG_HOLD;
    s.t0 = t;
    plan_add(s);
    click_add(t + YRT_S(1.0), DEV_O);   /* a third mouse's click: ignored */
}
/* The hands' exact positions (cm) at t; zero before placement. */
static void hand_exact(int h, int64_t t, double out[2]) {
    double u[2], d = 0;
    if (!sc.ts || t <= sc.ts) { out[0] = out[1] = 0; return; }
    plan_u(t, u);
    if (mapping) {
        d = (h ? -1 : 1) * SIM_D_CM * sin(2 * BM_PI * SIM_D_HZ * (double)(t - sc.ts) / 1e9);
        out[0] = u[0] + d;
        out[1] = u[1] - d;
    } else if (!h) {
        out[0] = u[0];
        out[1] = SIM_KAPPA * u[1];
    } else {
        out[0] = SIM_KAPPA * u[0];
        out[1] = u[1];
    }
}
/* The last report time of mouse h at or before t (its grid). */
static int64_t grid_le(int h, int64_t t) {
    int64_t k = t - sc.offset[h];
    k = (k >= 0 ? k / sc.period[h] : -((-k + sc.period[h] - 1) / sc.period[h])) * sc.period[h];
    return k + sc.offset[h];
}
/* The values a frame at onset t must read: each hand at its last report
 * time, unrounded (exact), or at t itself (model). */
static void script_values(int64_t t, int model, double L[2], double R[2]) {
    hand_exact(0, model ? t : grid_le(0, t), L);
    hand_exact(1, model ? t : grid_le(1, t), R);
}
static void script_start(int64_t t0) {
    memset(&sc, 0, sizeof sc);
    sc.period[0] = sc.period[1] = YRT_MS(1);
    sc.offset[1] = YRT_US(500);
    sc.period[2] = YRT_MS(8);
    sc.offset[2] = YRT_US(250);
    sc.t_start = t0;
    {
        int h;
        for (h = 0; h < 3; h++) sc.next[h] = grid_le(h, t0) + sc.period[h];
    }
    n_clicks = next_click = 0;
    click_add(t0 + YRT_S(0.2), 0);       /* device 0: refused */
    click_add(t0 + YRT_S(0.4), DEV_L);
    click_add(t0 + YRT_S(0.6), DEV_L);   /* the same mouse: refused */
    click_add(t0 + YRT_S(0.8), DEV_R);
}
/* Every scripted event up to t, in time order. */
static void script_emit(int64_t t_upto) {
    for (;;) {
        int h, m = -1;
        int64_t t = INT64_MAX;
        yrsp_input e;
        for (h = 0; h < 3; h++)
            if (sc.next[h] < t) { t = sc.next[h]; m = h; }
        if (next_click < n_clicks && clicks[next_click].t <= t) { t = clicks[next_click].t; m = 3; }
        if (t > t_upto) break;
        if (m == 3) { on_input(&clicks[next_click++], 1); continue; }
        sc.next[m] += sc.period[m];
        memset(&e, 0, sizeof e);
        e.kind = YRSP_KIND_MOUSE;
        e.type = YRSP_SAMPLE;
        e.control = YRSP_AXIS_DELTA;
        e.stamp = YRSP_TIER_SIM;
        e.t = t;
        if (m == 2) {   /* the third mouse: a 300-count circle every 2 s */
            double a = 2 * BM_PI * (double)(t - sc.t_start) / 2e9;
            int64_t kx = lround(300 * cos(a)), ky = lround(300 * sin(a));
            e.device = DEV_O;
            e.x = (float)(kx - sc.n[2][0]);
            e.y = (float)(ky - sc.n[2][1]);
            sc.n[2][0] = kx;
            sc.n[2][1] = ky;
        } else {
            double v[2];
            int64_t kx, ky;
            if (!sc.ts || t <= sc.ts) continue;
            hand_exact(m, t, v);
            kx = lround(v[0] * ppc / gain);
            ky = lround(-v[1] * ppc / gain);
            if (kx == sc.n[m][0] && ky == sc.n[m][1]) continue;   /* a mouse reports only movement */
            e.device = m ? DEV_R : DEV_L;
            e.x = (float)(kx - sc.n[m][0]);
            e.y = (float)(ky - sc.n[m][1]);
            sc.n[m][0] = kx;
            sc.n[m][1] = ky;
        }
        if (e.x != 0 || e.y != 0) on_input(&e, 1);
        if (m < 2 && sc.watch && !sc.found && t >= sc.from) {
            /* the cursor the program has now, by its own arithmetic */
            double L[2], R[2], u[2], c[2];
            hand_cm((float)((double)W + (double)gain * (double)sc.n[0][0]),
                    (float)((double)H + (double)gain * (double)sc.n[0][1]), L);
            hand_cm((float)((double)W + (double)gain * (double)sc.n[1][0]),
                    (float)((double)H + (double)gain * (double)sc.n[1][1]), R);
            cursor_of(L, R, u, c);
            if (coll_dist((float)c[0], (float)c[1], sc.ox, sc.oy) >= sc.level) { sc.found = 1; sc.t_found = t; }
        }
    }
}

/* --- the analysis --------------------------------------------------------- */

typedef struct fv { double t, tg[2], L[2], R[2], u[2], c[2]; } fv;   /* t: s from the first moving frame */
typedef struct cpx { double re, im; } cpx;
enum { CH_TG, CH_U, CH_L, CH_R, N_CH };
typedef struct an {
    double T, mse, inside;
    double M[3][4], th[3];      /* u, L, R on the target: a b c d; theta, rad */
    double srot, smir;
    cpx F[N_CH][2][2][N_SINES]; /* channel, component, the axis stimulated, i */
    cpx dd[2];                  /* (L - R) / 2 at SIM_D_HZ */
    double sum_t, lam;          /* sum of w |T|; S's smaller eigenvalue */
} an;

static double polar_theta(const double* m) {
    double a = m[0], b = m[1], c = m[2], d = m[3];
    return a * d - b * c >= 0 ? atan2(c - b, a + d) : atan2(b + c, a - d);
}
static const double* chan(const fv* v, int ch) { return ch == CH_TG ? v->tg : ch == CH_U ? v->u : ch == CH_L ? v->L : v->R; }

/* Frames with t in [w0, w1), each weighted by its time on screen. */
static void analyze(const fv* v, int n, double w0, double w1, an* r) {
    double S[3] = { 0, 0, 0 }, G[3][4];
    int k, ch, a, ax, i, s;
    memset(r, 0, sizeof *r);
    memset(G, 0, sizeof G);
    for (k = 0; k < n; k++) {
        double w, e0, e1;
        if (v[k].t < w0 || v[k].t >= w1) continue;
        w = (k + 1 < n && v[k + 1].t < w1 ? v[k + 1].t : w1) - v[k].t;
        r->T += w;
        e0 = v[k].c[0] - v[k].tg[0];
        e1 = v[k].c[1] - v[k].tg[1];
        r->mse += w * (e0 * e0 + e1 * e1);
        if (e0 * e0 + e1 * e1 <= TARGET_R * TARGET_R) r->inside += w;
        S[0] += w * v[k].tg[0] * v[k].tg[0];
        S[1] += w * v[k].tg[0] * v[k].tg[1];
        S[2] += w * v[k].tg[1] * v[k].tg[1];
        r->sum_t += w * sqrt(v[k].tg[0] * v[k].tg[0] + v[k].tg[1] * v[k].tg[1]);
        for (s = 0; s < 3; s++) {
            const double* h = chan(&v[k], CH_U + s);
            G[s][0] += w * h[0] * v[k].tg[0];
            G[s][1] += w * h[0] * v[k].tg[1];
            G[s][2] += w * h[1] * v[k].tg[0];
            G[s][3] += w * h[1] * v[k].tg[1];
        }
        for (ax = 0; ax < 2; ax++)
            for (i = 0; i < N_SINES; i++) {
                double ph = 2 * BM_PI * freq_hz[ax][i] * v[k].t, co = cos(ph) * w, si = -sin(ph) * w;
                for (ch = 0; ch < N_CH; ch++)
                    for (a = 0; a < 2; a++) {
                        double x = chan(&v[k], ch)[a];
                        r->F[ch][a][ax][i].re += x * co;
                        r->F[ch][a][ax][i].im += x * si;
                    }
            }
        {
            double ph = 2 * BM_PI * SIM_D_HZ * v[k].t;
            for (a = 0; a < 2; a++) {
                double x = (v[k].L[a] - v[k].R[a]) / 2;
                r->dd[a].re += x * cos(ph) * w;
                r->dd[a].im -= x * sin(ph) * w;
            }
        }
    }
    if (!(r->T > 0)) return;
    r->mse /= r->T;
    r->inside /= r->T;
    {
        /* M = G S^-1, S symmetric 2 x 2 */
        double det = S[0] * S[2] - S[1] * S[1], tr = S[0] + S[2];
        r->lam = tr / 2 - sqrt(tr * tr / 4 - det);
        for (s = 0; s < 3; s++) {
            double* m = r->M[s];
            m[0] = (G[s][0] * S[2] - G[s][1] * S[1]) / det;
            m[1] = (G[s][1] * S[0] - G[s][0] * S[1]) / det;
            m[2] = (G[s][2] * S[2] - G[s][3] * S[1]) / det;
            m[3] = (G[s][3] * S[0] - G[s][2] * S[1]) / det;
            r->th[s] = polar_theta(m);
        }
    }
    r->srot = (r->M[0][2] - r->M[0][1]) / 2;
    r->smir = (r->M[0][1] + r->M[0][2]) / 2;
    for (ch = 0; ch < N_CH; ch++)
        for (a = 0; a < 2; a++)
            for (ax = 0; ax < 2; ax++)
                for (i = 0; i < N_SINES; i++) {
                    r->F[ch][a][ax][i].re *= 2 / r->T;
                    r->F[ch][a][ax][i].im *= 2 / r->T;
                }
    for (a = 0; a < 2; a++) { r->dd[a].re *= 2 / r->T; r->dd[a].im *= 2 / r->T; }
}

static double cabs2(double re, double im) { return sqrt(re * re + im * im); }
static double wrap_deg(double a) {
    while (a > 180) a -= 360;
    while (a <= -180) a += 360;
    return a;
}

/* --- the session's state ---------------------------------------------------- */

static FILE *out, *rout, *tout, *qout;
static int sim, failed, n_blocks, btype[MAX_BLOCKS], bpert[MAX_BLOCKS], track_trials = 2, n_reaches = 30;
static double track_s = TRACK_S, win_s, theta_b[MAX_BLOCKS];
static int state = S_NEXT, cur_block = -1, block_trial, block_reach, track_index, reach_index;
static uint64_t rng;
static double unit01(void) { return ytr_splitmix(&rng); }   /* [0, 1) */

/* Tracking: one record per moving frame; the hands at its onset as the
 * program read them (fp), the script's exact reports then (fx), and the
 * script's model at the onset itself (fa). */
typedef struct frame_rec { int64_t index, tp, ta; double tg[2]; float dot[2]; uint8_t have, missed; } frame_rec;
static frame_rec fr[FRAME_CAP];
static fv fp[FRAME_CAP], fx[FRAME_CAP], fa[FRAME_CAP];
static int n_fr, n_norecord, other0;
static yrsp_onset onset0;               /* the first moving frame's */
static int64_t t_ready, last_end, finish_frame;

/* Reaching */
static double start_c[2], tgt_c[2], peak_hold;
static int64_t t_in, t_hold;
static int holding, fast_now, reach_planned, blk_fast;
static double blk_err;

/* --sim and --auto: the worst deviation of each check, in units of its
 * bound; then the gaps to the model the report grid leaves (printed) */
static struct {
    double frame, dot, result, math, reach, init, init_x, resid;
    double gain, phase, theta, mse, kappa;
    int frames;
} dv;
typedef struct reach_exp { double err_deg; int fast; } reach_exp;
static reach_exp rexp[MAX_REACHES];

static void finalize(int k, int64_t ta) {
    fr[k].ta = ta;
    fr[k].have = 1;
    value_at(B_L, ta, fp[k].L);
    value_at(B_R, ta, fp[k].R);
    value_at(B_U, ta, fp[k].u);
    value_at(B_C, ta, fp[k].c);
    if (is_script) {
        script_values(ta, 0, fx[k].L, fx[k].R);
        cursor_of(fx[k].L, fx[k].R, fx[k].u, fx[k].c);
        script_values(ta, 1, fa[k].L, fa[k].R);
        cursor_of(fa[k].L, fa[k].R, fa[k].u, fa[k].c);
    }
}

/* The checks of one tracking trial (header: (1), (2), (3)). */
static void check_tracking(const an* rp, int64_t ta0) {
    double q = (0.5 * gain + 1e-4) / ppc, d2 = sqrt(2.0) * q, th = theta_b[cur_block], worst = 0, w = 0;
    double A[4], Bx[2], By[2], cxy[2];
    an rx, ra;
    int k, a, ax, i;
    /* (1) the data path, per frame */
    for (k = 0; k < n_fr; k++) {
        for (a = 0; a < 2; a++) {
            worst = fmax(worst, fabs(fp[k].L[a] - fx[k].L[a]) / q);
            worst = fmax(worst, fabs(fp[k].R[a] - fx[k].R[a]) / q);
            worst = fmax(worst, fabs(fp[k].u[a] - fx[k].u[a]) / q);
        }
        worst = fmax(worst, cabs2(fp[k].c[0] - fx[k].c[0], fp[k].c[1] - fx[k].c[1]) / d2);
        {
            /* the drawn cursor read the hands at the planned onset */
            double L[2], R[2], u[2], c[2];
            script_values(fr[k].tp, 0, L, R);
            cursor_of(L, R, u, c);
            dv.dot = fmax(dv.dot, cabs2(fr[k].dot[0] - px_x(c[0]), fr[k].dot[1] - px_y(c[1])) / (d2 * ppc + 1e-3));
        }
        dv.frames++;
    }
    dv.frame = fmax(dv.frame, worst);
    /* (3) the results against the exact reports', bounds from the grid */
    analyze(fx, n_fr, track_s - win_s, track_s, &rx);
    {
        double bm = d2 * rx.sum_t / rx.lam;   /* ||dM||_F <= ||sum w dH T'|| ||S^-1|| */
        w = fmax(w, fabs(rp->mse - rx.mse) / (d2 * (2 * sqrt(rx.mse) + d2)));
        for (i = 0; i < 3; i++) {
            const double* m = rx.M[i];
            double v = m[0] * m[3] - m[1] * m[2] >= 0 ? cabs2(m[2] - m[1], m[0] + m[3]) : cabs2(m[1] + m[2], m[0] - m[3]);
            for (a = 0; a < 4; a++) w = fmax(w, fabs(rp->M[i][a] - m[a]) / bm);
            w = fmax(w, fabs(rp->th[i] - rx.th[i]) / (sqrt(2.0) * bm < v ? asin(sqrt(2.0) * bm / v) + 1e-12 : BM_PI));
        }
        w = fmax(w, fabs(rp->srot - rx.srot) / bm);
        w = fmax(w, fabs(rp->smir - rx.smir) / bm);
        for (i = CH_U; i < N_CH; i++)
            for (a = 0; a < 2; a++)
                for (ax = 0; ax < 2; ax++)
                    for (k = 0; k < N_SINES; k++)
                        w = fmax(w, cabs2(rp->F[i][a][ax][k].re - rx.F[i][a][ax][k].re,
                                          rp->F[i][a][ax][k].im - rx.F[i][a][ax][k].im) / (2 * q));
        for (a = 0; a < 2; a++) w = fmax(w, cabs2(rp->dd[a].re - rx.dd[a].re, rp->dd[a].im - rx.dd[a].im) / (2 * q));
        dv.result = fmax(dv.result, w);
    }
    if (worst > 1 || w > 1) {
        fprintf(stderr, "trial_bimanual: check: trial %d: frames %.3f, results %.3f of their bounds\n", track_index, worst, w);
        failed = 1;
    }
    if (!sim) return;
    /* (2) the math, on the model at the exact onsets (--sim only: a
     * window's onsets are not on a grid of whole cycles). u = A r(t -
     * tau), A = g Rot(theta); the cursor B r(t - tau), B = P A. */
    for (k = 0; k < n_fr; k++) target_at(cur_block, fa[k].t, ramp_of(fa[k].t), fa[k].tg);
    analyze(fa, n_fr, track_s - win_s, track_s, &ra);
    A[0] = SIM_G * cos(th); A[1] = -SIM_G * sin(th); A[2] = SIM_G * sin(th); A[3] = SIM_G * cos(th);
    apply_p(pert_on, (double[2]){ A[0], A[2] }, Bx);   /* B's columns */
    apply_p(pert_on, (double[2]){ A[1], A[3] }, By);
    w = 0;
    {
        double mse_cf = 0;
        for (ax = 0; ax < 2; ax++) {
            double baa = ax ? By[1] : Bx[0], num = 0, den = 0;
            for (k = 0; k < N_SINES; k++) {
                double om = 2 * BM_PI * freq_hz[ax][k], a2 = amp_cm[ax][k] * amp_cm[ax][k], c = cos(om * SIM_TAU_S),
                       s = sin(om * SIM_TAU_S);
                cpx T0 = ra.F[CH_TG][ax][ax][k], Tp = rp->F[CH_TG][ax][ax][k];
                /* the error at this frequency: |B e_a e^-iwt - e_a|^2 = g^2 + 1 - 2 cos(w tau) B_aa */
                mse_cf += a2 / 2 * (SIM_G * SIM_G + 1 - 2 * c * baa);
                num += a2 * c;
                den += a2;
                for (a = 0; a < 2; a++) {
                    /* u / r = g e^-iwt Rot(theta) e_a; L and R the same, or with the crosstalk */
                    double rot = ax ? (a ? cos(th) : -sin(th)) : (a ? sin(th) : cos(th)), er = SIM_G * rot * c,
                           ei = -SIM_G * rot * s, d = T0.re * T0.re + T0.im * T0.im, dp = Tp.re * Tp.re + Tp.im * Tp.im;
                    double kl = mapping ? 1 : (a ? SIM_KAPPA : 1), kr = mapping ? 1 : (a ? 1 : SIM_KAPPA);
                    cpx U = ra.F[CH_U][a][ax][k], Lc = ra.F[CH_L][a][ax][k], Rc = ra.F[CH_R][a][ax][k],
                        Up = rp->F[CH_U][a][ax][k];
                    w = fmax(w, cabs2((U.re * T0.re + U.im * T0.im) / d - er, (U.im * T0.re - U.re * T0.im) / d - ei));
                    w = fmax(w, cabs2(Lc.re - kl * U.re, Lc.im - kl * U.im));
                    w = fmax(w, cabs2(Rc.re - kr * U.re, Rc.im - kr * U.im));
                    if (a == ax) {   /* the measured response against the model */
                        double pr = (Up.re * Tp.re + Up.im * Tp.im) / dp, pi = (Up.im * Tp.re - Up.re * Tp.im) / dp;
                        dv.gain = fmax(dv.gain, fabs(cabs2(pr, pi) / cabs2(er, ei) - 1));
                        dv.phase = fmax(dv.phase, fabs(wrap_deg((atan2(pi, pr) - atan2(ei, er)) * 180 / BM_PI)));
                    }
                }
            }
            cxy[ax] = num / den;
        }
        w = fmax(w, fabs(ra.mse - mse_cf));
        dv.mse = fmax(dv.mse, fabs(rp->mse - mse_cf) / mse_cf);
    }
    w = fmax(w, fabs(wrap_deg((ra.th[0] - th) * 180 / BM_PI)) * BM_PI / 180);
    dv.theta = fmax(dv.theta, fabs(wrap_deg((rp->th[0] - th) * 180 / BM_PI)));
    {
        /* M = A diag(cx, cy); L's and R's through the crosstalk */
        double Mc[4];
        Mc[0] = A[0] * cxy[0]; Mc[1] = A[1] * cxy[1]; Mc[2] = A[2] * cxy[0]; Mc[3] = A[3] * cxy[1];
        for (a = 0; a < 4; a++) {
            double kl = mapping || a < 2 ? 1 : SIM_KAPPA, kr = mapping || a >= 2 ? 1 : SIM_KAPPA;
            w = fmax(w, fabs(ra.M[0][a] - Mc[a]));
            w = fmax(w, fabs(ra.M[1][a] - kl * Mc[a]));
            w = fmax(w, fabs(ra.M[2][a] - kr * Mc[a]));
        }
        w = fmax(w, fabs(ra.srot - (Mc[2] - Mc[1]) / 2));
        w = fmax(w, fabs(ra.smir - (Mc[1] + Mc[2]) / 2));
    }
    if (mapping) {
        /* d = D sin(w (t - ts)) on x, minus that on y */
        double ph = 2 * BM_PI * SIM_D_HZ * YRT_TO_S(ta0 - sc.ts) - BM_PI / 2;
        w = fmax(w, cabs2(ra.dd[0].re - SIM_D_CM * cos(ph), ra.dd[0].im - SIM_D_CM * sin(ph)));
        w = fmax(w, cabs2(ra.dd[1].re + SIM_D_CM * cos(ph), ra.dd[1].im + SIM_D_CM * sin(ph)));
        dv.kappa = fmax(dv.kappa, fabs(cabs2(rp->dd[0].re, rp->dd[0].im) - SIM_D_CM) / SIM_D_CM);
    } else {
        /* the crosstalk from M: L's second row over u's, R's first */
        const double* m = rp->M[0];
        double kl = (rp->M[1][2] * m[2] + rp->M[1][3] * m[3]) / (m[2] * m[2] + m[3] * m[3]);
        double kr = (rp->M[2][0] * m[0] + rp->M[2][1] * m[1]) / (m[0] * m[0] + m[1] * m[1]);
        dv.kappa = fmax(dv.kappa, fmax(fabs(kl - SIM_KAPPA), fabs(kr - SIM_KAPPA)) / SIM_KAPPA);
    }
    dv.math = fmax(dv.math, w / 1e-6);
    if (w > 1e-6) {
        fprintf(stderr, "trial_bimanual: check: trial %d: the model's analysis is %.3g off its closed form\n", track_index, w);
        failed = 1;
    }
}

/* The tracking trial's end: the analysis, its rows, the checks. */
static void end_tracking(void) {
    an rp;
    int k, ax, a, i, missed = 0;
    int64_t ta0 = fr[0].ta;
    for (k = 0; k < n_fr; k++) {
        double t = YRT_TO_S(fr[k].ta - ta0);
        missed += fr[k].missed;
        fp[k].t = fx[k].t = fa[k].t = t;
        memcpy(fp[k].tg, fr[k].tg, sizeof fp[k].tg);
        memcpy(fx[k].tg, fr[k].tg, sizeof fx[k].tg);
    }
    analyze(fp, n_fr, track_s - win_s, track_s, &rp);
    fprintf(out, "%d,%d,%d,%s,%s,%.6f,%.4f,%.6f,%.6f,%.6f,%.6f,%.4f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.4f,%.6f,%.6f,%.6f,%.6f,"
                 "%.4f,%d,%d,%d,%d,%u,%u\n",
            track_index, cur_block, block_trial, pert_on ? pert_name[perturb] : "none", mapping ? "mean" : "split", rp.mse,
            rp.inside, rp.M[0][0], rp.M[0][1], rp.M[0][2], rp.M[0][3], rp.th[0] * 180 / BM_PI, rp.srot, rp.smir,
            rp.M[1][0], rp.M[1][1], rp.M[1][2], rp.M[1][3], rp.th[1] * 180 / BM_PI, rp.M[2][0], rp.M[2][1], rp.M[2][2],
            rp.M[2][3], rp.th[2] * 180 / BM_PI, n_fr, missed, n_norecord, n_other - other0, (unsigned)onset0.tier,
            (unsigned)onset0.src);
    for (k = 0; k < n_fr; k++) {
        double e0 = fp[k].c[0] - fp[k].tg[0], e1 = fp[k].c[1] - fp[k].tg[1];
        fprintf(tout, "%d,%.6f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d\n", track_index, fp[k].t, fp[k].tg[0], fp[k].tg[1],
                fp[k].c[0], fp[k].c[1], fp[k].L[0], fp[k].L[1], fp[k].R[0], fp[k].R[1],
                e0 * e0 + e1 * e1 <= TARGET_R * TARGET_R);
    }
    for (ax = 0; ax < 2; ax++)
        for (k = 0; k < N_SINES; k++) {
            fprintf(qout, "%d,%d,%c,%.2f,%.6f,%.6f", track_index, cur_block, ax ? 'y' : 'x', freq_hz[ax][k],
                    rp.F[CH_TG][ax][ax][k].re, rp.F[CH_TG][ax][ax][k].im);
            for (i = CH_U; i < N_CH; i++)
                for (a = 0; a < 2; a++) fprintf(qout, ",%.6f,%.6f", rp.F[i][a][ax][k].re, rp.F[i][a][ax][k].im);
            fprintf(qout, "\n");
        }
    fflush(out);
    fflush(tout);
    fflush(qout);
    printf("trial %2d block %d %-8s mse %6.3f cm2, inside %.3f, theta %7.2f deg (L %7.2f, R %7.2f), S_rot %6.3f, "
           "S_mir %6.3f, %d frames, %d missed\n", track_index, cur_block, pert_on ? pert_name[perturb] : "none", rp.mse,
           rp.inside, rp.th[0] * 180 / BM_PI, rp.th[1] * 180 / BM_PI, rp.th[2] * 180 / BM_PI, rp.srot, rp.smir, n_fr, missed);
    if (is_script) check_tracking(&rp, ta0);
    ytr_update(&trials, 1, NULL);
    track_index++;
    block_trial++;
    n_fr = 0;
}

/* The reach's end: its row from the collector, then the script's checks. */
static void end_reach(void) {
    double init = NAN, tdeg = atan2(tgt_c[1] - start_c[1], tgt_c[0] - start_c[0]) * 180 / BM_PI;
    int k, hand_moved = -1, j = reach_index < MAX_REACHES ? reach_index : MAX_REACHES - 1;
    char rt[32], it[32], mt[32];
    yrsp_finish(&rsp, &res);
    rt[0] = it[0] = mt[0] = 0;
    if (res.rt_ns != YRSP_NO_TIME) {
        /* the velocity over 140 to 150 ms after the onset: the cursor at
         * each, the last trace entry at or before it */
        int64_t t1 = res.t_response + YRT_S(INIT_S - VEL_LAG_S), t2 = res.t_response + YRT_S(INIT_S), b1 = 0, b2 = 0;
        float p1[2] = { 0, 0 }, p2[2] = { 0, 0 };
        for (k = 0; k < res.n_trace; k++) {
            const yrsp_input* p = &trace[k];
            if (p->t <= t1 && (!b1 || p->t >= b1)) { b1 = p->t; p1[0] = p->x; p1[1] = p->y; }
            if (p->t <= t2 && (!b2 || p->t >= b2)) { b2 = p->t; p2[0] = p->x; p2[1] = p->y; }
        }
        if (b1 && b2 && b2 > b1) init = wrap_deg(atan2((double)p2[1] - p1[1], (double)p2[0] - p1[0]) * 180 / BM_PI - tdeg);
        snprintf(rt, sizeof rt, "%.9f", YRT_TO_S(res.rt_ns));
        snprintf(mt, sizeof mt, "%.9f", YRT_TO_S(t_hold - res.t_response));
        if (!isnan(init)) snprintf(it, sizeof it, "%.4f", init);
        /* the derived event's code: the source node of the newest report */
        hand_moved = (res.code & 0xFFFFu) == B_SRC_L ? 0 : (res.code & 0xFFFFu) == B_SRC_R ? 1 : -1;
    }
    fprintf(rout, "%d,%d,%d,%s,%.4f,%.4f,%.4f,%.4f,%.3f,%s,%s,%.2f,%d,%s,%u,%u,%u,%s,%u\n", reach_index, cur_block, block_reach,
            pert_on ? pert_name[perturb] : "none", start_c[0], start_c[1], tgt_c[0], tgt_c[1], tdeg, rt, it, peak_hold,
            fast_now, mt, (unsigned)res.onset_tier, (unsigned)res.onset_src, (unsigned)res.stamp_tier,
            hand_moved == 0 ? "L" : hand_moved == 1 ? "R" : "", (unsigned)res.flags);
    fflush(rout);
    if (is_script) {
        /* the onset to the ns; the direction against the exact reports at
         * the same times, within the count grid: each end of the 10 ms
         * displacement is off by at most sqrt(2) q */
        double q = (0.5 * gain + 1e-4) / ppc, L[2], R[2], u[2], c1[2], c2[2], dist, bound, ix;
        int64_t te = sc.t_found;
        script_values(te + YRT_S(INIT_S - VEL_LAG_S), 0, L, R);
        cursor_of(L, R, u, c1);
        script_values(te + YRT_S(INIT_S), 0, L, R);
        cursor_of(L, R, u, c2);
        dist = cabs2(c2[0] - c1[0], c2[1] - c1[1]);
        ix = wrap_deg(atan2(c2[1] - c1[1], c2[0] - c1[0]) * 180 / BM_PI - tdeg);
        bound = (2 * sqrt(2.0) * q < dist ? asin(2 * sqrt(2.0) * q / dist) * 180 / BM_PI : 180) + 1e-6;
        dv.reach = fmax(dv.reach, fabs(wrap_deg(init - ix)) / bound);
        dv.init = fmax(dv.init, fabs(wrap_deg(init - rexp[j].err_deg)));
        dv.init_x = fmax(dv.init_x, fabs(wrap_deg(ix - rexp[j].err_deg)));
        if (!sc.found || res.t_response != te || isnan(init) || fabs(wrap_deg(init - ix)) > bound ||
            fast_now != rexp[j].fast || res.n_early) {
            fprintf(stderr, "trial_bimanual: check: reach %d: onset %lld (script %lld), init %.4f deg (script %.4f, bound "
                            "%.4f), fast %d (script %d), early %d\n", reach_index, (long long)res.t_response, (long long)te,
                    init, ix, bound, fast_now, rexp[j].fast, res.n_early);
            failed = 1;
        }
        sc.watch = 0;
    }
    ytr_update(&trials, 1, NULL);
    blk_err += isnan(init) ? 0 : init;
    blk_fast += fast_now;
    reach_index++;
    if (++block_reach == n_reaches)
        printf("block %d: %d reaches, initial direction error %.2f deg on average, %d fast\n", cur_block, n_reaches,
               blk_err / n_reaches, blk_fast);
}

/* A reach target 12 cm from the start, inside the workspace, on this
 * frame; the collector's window opens here. */
static int new_target(const yscr_frame* f, int first) {
    int tries;
    int64_t bt = 0;
    ytl_seq q;
    if (first) start_c[0] = start_c[1] = 0;
    else { start_c[0] = tgt_c[0]; start_c[1] = tgt_c[1]; }
    for (tries = 0; tries < 10000; tries++) {
        double a = 2 * BM_PI * unit01();
        tgt_c[0] = start_c[0] + REACH_CM * cos(a);
        tgt_c[1] = start_c[1] + REACH_CM * sin(a);
        if (fabs(tgt_c[0]) <= WORK_CM && fabs(tgt_c[1]) <= WORK_CM) break;
    }
    ytl_clear(&tl, TRIAL);
    ytl_anchor(&tl, TRIAL, f->onset, 0);
    q = ytl_seq_on(&tl, TRIAL);
    ytl_base_time(&tl, TRIAL, f->onset, &bt);
    ytl_at(&q, bt);
    ytl_on(&q, TARGET_ON);
    if (q.err) { fprintf(stderr, "trial_bimanual: timeline: %s\n", ytl_error(&tl)); return 0; }
    yrsp_arm(&rsp, f->onset);
    yrsp_set_origin(&rsp, 0, (float)start_c[0], (float)start_c[1]);
    peak_run = peak_hold = 0;
    holding = fast_now = reach_planned = 0;
    state = S_R_REACH;
    return 1;
}

/* The target starts moving on this frame: the timeline's ramp and end. */
static int start_tracking(const yscr_frame* f) {
    int64_t bt = 0;
    ytl_seq q;
    ytl_clear(&tl, TRIAL);
    ytl_anchor(&tl, TRIAL, f->onset, 0);
    q = ytl_seq_on(&tl, TRIAL);
    ytl_base_time(&tl, TRIAL, f->onset, &bt);
    ytl_at(&q, bt);
    ytl_on(&q, MOVING);
    ytl_to(&q, AMP, &(ytl_tween_desc){ .from = 0, .from_set = true, .to = 1, .duration_ns = YRT_S(RAMP_S),
                                       .ease = YTL_EASE_LINEAR });
    ytl_wait(&q, YRT_S(track_s));
    ytl_off(&q, MOVING);
    ytl_mark(&q, END_OF_TRACK, 0);
    if (q.err) { fprintf(stderr, "trial_bimanual: timeline: %s\n", ytl_error(&tl)); return 0; }
    n_fr = n_norecord = 0;
    other0 = n_other;
    memset(&onset0, 0, sizeof onset0);
    onset0.src = YRSP_ONSET_PLAN;
    state = S_T_MOVE;
    if (is_script) {
        /* follow the target, then go back to the center */
        seg s;
        double z[2] = { 0, 0 };
        memset(&s, 0, sizeof s);
        s.kind = SEG_TRACK;
        s.block = cur_block;
        s.t0 = f->onset;
        s.t1 = f->onset + YRT_S(track_s);
        s.theta = theta_b[cur_block];
        plan_add(s);
        plan_mj(s.t1, SIM_RETURN_S, z);
    }
    return 1;
}

/* The block's cursor map P (identity, rotation or mirror) and target
 * phases into the graph, between trials. */
static void set_graph_block(int b) {
    int k, a, ok = 1;
    for (k = 0; k < 4; k++) ok &= ysig_set(&sg, B_C, k, 0);   /* all zero: the identity */
    ok &= ysig_set(&sg, B_C, 8, pert_on && perturb == 1 ? rot_deg : 0);
    if (pert_on && perturb == 2) ok &= ysig_set(&sg, B_C, 1, 1) && ysig_set(&sg, B_C, 2, 1);
    for (a = 0; a < 2; a++)
        for (k = 0; k < N_SINES; k++) ok &= ysig_set_data(&sg, B_SINES, (a * N_SINES + k) * 3 + 2, phase[b][a][k]);
    if (!ok) { fprintf(stderr, "trial_bimanual: the graph refused block %d's parameters\n", b); failed = 1; }
}

/* The graph: two mice to the cursor, the target, the derived events. */
static ysig_node sig_nodes[B_N];
static double sines_data[2 * N_SINES * 3];
static bool open_graph(void) {
    ysig_node* n = sig_nodes;
    ysig_desc d;
    int k, a;
    for (k = 0; k < 2; k++) {
        n[B_SRC_L + k] = ysig_node_of(YSIG_OP_SOURCE, -1, -1);
        n[B_SRC_L + k].lanes = 2;
        n[B_SRC_L + k].p[0] = YIN_KIND_MOUSE;
        n[B_SRC_L + k].p[1] = YIN_AXIS_DELTA;
        n[B_SRC_L + k].p[2] = 4294967295.0;   /* no mouse until the setup names it */
        n[B_SRC_L + k].p[4] = yin_raw_mouse_source().tier;
        n[B_LPX + k] = ysig_node_of(YSIG_OP_INTEGRATE, B_SRC_L + k, -1);   /* px, from (W, H) */
        n[B_LPX + k].p[0] = n[B_LPX + k].p[1] = gain;
        n[B_LPX + k].p[4] = W;
        n[B_LPX + k].p[5] = H;
        n[B_LPX + k].p[12] = 2 * (double)W;   /* a hand stops at the window's size from there */
        n[B_LPX + k].p[13] = 2 * (double)H;
        n[B_L + k] = ysig_node_of(YSIG_OP_AFFINE, B_LPX + k, -1);          /* cm, y up */
        n[B_L + k].p[0] = 1 / ppc;
        n[B_L + k].p[3] = -1 / ppc;
        n[B_L + k].p[4] = W;
        n[B_L + k].p[5] = H;
        n[B_L + k].history = RING_CAP;
    }
    n[B_LX] = ysig_node_of(YSIG_OP_SELECT, B_L, -1);
    n[B_LX].lanes = 1;
    n[B_RY] = ysig_node_of(YSIG_OP_SELECT, B_R, -1);
    n[B_RY].lanes = 1;
    n[B_RY].p[0] = 1;
    n[B_U] = mapping ? ysig_node_of(YSIG_OP_MEAN, B_L, B_R) : ysig_node_of(YSIG_OP_JOIN, B_LX, B_RY);
    n[B_C] = ysig_node_of(YSIG_OP_AFFINE, B_U, -1);   /* P, per block */
    n[B_C].history = RING_CAP;                        /* computed from the hands' rings at t */
    n[B_U].history = RING_CAP;
    n[B_EMIT] = ysig_node_of(YSIG_OP_EMIT, B_C, -1);
    n[B_EMIT].p[0] = 7;                               /* YIN_KIND_USER + 7 */
    n[B_SPEED] = ysig_node_of(YSIG_OP_SPEED, B_C, -1);
    n[B_SPEED].p[0] = VEL_LAG_S;
    n[B_AMP] = ysig_node_of(YSIG_OP_CHANNEL, -1, -1);
    n[B_AMP].p[0] = AMP;
    for (a = 0; a < 2; a++)
        for (k = 0; k < N_SINES; k++) {
            sines_data[(a * N_SINES + k) * 3] = amp_cm[a][k];
            sines_data[(a * N_SINES + k) * 3 + 1] = freq_hz[a][k];
            sines_data[(a * N_SINES + k) * 3 + 2] = phase[0][a][k];
        }
    n[B_SINES] = ysig_node_of(YSIG_OP_SINES, -1, -1);
    n[B_SINES].lanes = 2;
    n[B_SINES].p[0] = N_SINES;
    n[B_SINES].data = sines_data;
    n[B_SINES].n_data = 2 * N_SINES * 3;
    n[B_TGT] = ysig_node_of(YSIG_OP_SCALE, B_SINES, B_AMP);   /* the ramp: the timeline's tween */
    n[B_ERR] = ysig_node_of(YSIG_OP_DIFF, B_C, B_TGT);
    n[B_INSIDE] = ysig_node_of(YSIG_OP_DETECT, B_ERR, -1);    /* the target's color */
    n[B_INSIDE].p[0] = YSIG_TEST_LE;
    n[B_INSIDE].p[1] = -1;
    n[B_INSIDE].p[2] = TARGET_R;
    n[B_INSIDE].p[5] = 1;                                     /* at each frame */
    n[B_TPX] = ysig_node_of(YSIG_OP_AFFINE, B_TGT, -1);       /* px for drawing */
    n[B_TPX].p[0] = ppc;
    n[B_TPX].p[3] = -ppc;
    n[B_TPX].p[6] = cx;
    n[B_TPX].p[7] = cy;
    n[B_BIND_T] = ysig_node_of(YSIG_OP_BIND, B_TPX, -1);
    n[B_BIND_T].p[0] = CH_TGT_X;
    memset(&d, 0, sizeof d);
    d.nodes = n;
    d.n_nodes = B_N;
    d.channels = sig_ch;
    d.n_channels = N_SIG_CH;
    d.arena = sig_arena;
    d.arena_size = sizeof sig_arena;
    if (!ysig_open(&sg, &d)) { fprintf(stderr, "trial_bimanual: %s\n", ysig_error(&sg)); return false; }
    return true;
}

/* The next trial from the session: a new block, or the block's next. */
static int next_trial(const yscr_frame* f) {
    ytr_trial_info ti;
    int b;
    if (ytr_next(&trials, &ti) < 0) { state = S_DONE; return 1; }
    b = ti.condition;
    if (b != cur_block) {
        /* ysp/trials.h's first_in_block marks only the session's first
         * trial here (ORDER_LIST, no block_size): the row says the block */
        cur_block = b;
        block_trial = block_reach = blk_fast = 0;
        blk_err = 0;
        pert_on = bpert[b];
        set_graph_block(b);
        printf("block %d: %s%s\n", b, btype[b] ? "reaching" : "tracking", pert_on ? " (perturbed)" : "");
        if (is_script) {
            double z[2] = { 0, 0 };
            plan_mj(f->onset, SIM_RETURN_S, z);
        }
        holding = 0;
        state = btype[b] ? S_R_START : S_T_WAIT;
    } else if (btype[b]) return new_target(f, 0);
    else state = S_T_WAIT;
    if (state == S_T_WAIT) t_ready = last_end ? last_end + YRT_S(ITI_S) : f->onset;
    return 1;
}

int main(int argc, char** argv) {
    yscr_desc sd;
    yscr_caps caps;
    ytb_csv_desc cd;
    ytr_desc rd;
    yrsp_desc pd;
    yrsp_choice moved;
    yrsp_source sources[1];
    yscr_frame f;
    ytl_event fired[64];
    char line[1024], meta[2048], view_note[160];
    const char* out_path = "trial_bimanual.csv";
    const char* reach_path = "trial_bimanual_reaches.csv";
    const char* track_path = "trial_bimanual_track.csv";
    const char* freq_path = "trial_bimanual_freq.csv";
    const char* blocks_arg = "T,R,T*,R*,T*,R*,T*,R*,T*,T";
    uint64_t seed = 20261106;
    int i, n, autorun = 0, fullscreen = 0, aborted = 0, total = 0, want_t = 0, want_r = 0, have_prev = 0;
    double rotation = ROTATION_DEG, cpi = CPI, ppc_arg = 0, peak[2] = { 0, 0 }, c_prev[2] = { 0, 0 };
    gain = GAIN;
    int64_t prev_onset = 0;
    ygfx_stim tgt, cur, start, box[2];

    memset(&sd, 0, sizeof sd);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--auto")) autorun = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--mapping") && i + 1 < argc) {
            i++;
            mapping = !strcmp(argv[i], "mean") ? 1 : !strcmp(argv[i], "split") ? 0 : -1;
        } else if (!strcmp(argv[i], "--perturb") && i + 1 < argc) {
            i++;
            perturb = !strcmp(argv[i], "rotation") ? 1 : !strcmp(argv[i], "mirror") ? 2 : -1;
        } else if (!strcmp(argv[i], "--rotation") && i + 1 < argc) rotation = atof(argv[++i]);
        else if (!strcmp(argv[i], "--blocks") && i + 1 < argc) blocks_arg = argv[++i];
        else if (!strcmp(argv[i], "--track-trials") && i + 1 < argc) track_trials = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reaches") && i + 1 < argc) n_reaches = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--track-s") && i + 1 < argc) track_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gain") && i + 1 < argc) gain = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--cpi") && i + 1 < argc) cpi = atof(argv[++i]);
        else if (!strcmp(argv[i], "--px-per-cm") && i + 1 < argc) ppc_arg = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--reaches-out") && i + 1 < argc) reach_path = argv[++i];
        else if (!strcmp(argv[i], "--track") && i + 1 < argc) track_path = argv[++i];
        else if (!strcmp(argv[i], "--freq") && i + 1 < argc) freq_path = argv[++i];
        else break;
    }
    if (!perturb) perturb = 1;
    {
        /* the block list: T or R, "*" perturbed, by commas */
        const char* p = blocks_arg;
        while (*p && n_blocks < MAX_BLOCKS) {
            if (*p != 'T' && *p != 'R') break;
            btype[n_blocks] = *p++ == 'R';
            bpert[n_blocks] = *p == '*';
            if (*p == '*') p++;
            n_blocks++;
            if (*p == ',' && p[1]) p++;
            else break;
        }
        if (*p) n_blocks = 0;
    }
    if (i < argc || mapping < 0 || perturb < 0 || n_blocks == 0 || track_trials < 1 || n_reaches < 1 ||
        !(track_s >= 26 && track_s <= TRACK_S) || !(gain > 0) || !(cpi > 0) || !(fabs(rotation) <= 180) ||
        (ppc_arg != 0 && !(ppc_arg >= 1 && ppc_arg <= 1000)) || (sim && autorun)) {
        fprintf(stderr, "usage: trial_bimanual [--sim | --auto] [--mapping split|mean] [--perturb rotation|mirror] "
                        "[--rotation DEG]\n                      [--blocks LIST] [--track-trials N] [--reaches N] "
                        "[--track-s S] [--gain PX] [--cpi N]\n                      [--px-per-cm PX] [--fullscreen] "
                        "[--seed N] [--out FILE] [--reaches-out FILE] [--track FILE] [--freq FILE]\n"
                        "  LIST: T and R, \"*\" perturbed, by commas; --track-s 26..46\n");
        return 2;
    }
    for (i = 0; i < n_blocks; i++) {
        if (btype[i]) want_r += n_reaches;
        else want_t += track_trials;
    }
    total = want_r + want_t;
    if (total > YTR_MAX_TRIALS) {
        fprintf(stderr, "trial_bimanual: %d trials; at most %d\n", total, YTR_MAX_TRIALS);
        return 2;
    }
    rot_deg = rotation;
    rot_rad = rotation * BM_PI / 180;
    /* a multiple of 20 s, so every frequency falls on a bin, starting at
     * least 1 s after the ramp */
    win_s = track_s >= 46 ? 40 : 20;
    is_script = sim || autorun;

    /* the phases, per block, from the seed (ysp/trials.h draws per trial) */
    rng = seed;
    for (i = 0; i < n_blocks; i++) {
        int a, k;
        for (a = 0; a < 2; a++)
            for (k = 0; k < N_SINES; k++) phase[i][a][k] = -BM_PI + 2 * BM_PI * unit01();
    }
    {
        /* the script's compensation per block: half, 3/4, 7/8 ... of the
         * perturbation over the perturbed T blocks, a quarter after them */
        double comp = perturb == 2 ? SIM_MIRROR_DEG : -rotation;
        int k = 0, any = 0;
        for (i = 0; i < n_blocks; i++) {
            if (bpert[i]) {
                theta_b[i] = comp * (1 - pow(0.5, k + 1));
                if (!btype[i]) k++;
                any = 1;
            } else theta_b[i] = any ? 0.25 * comp : 0;
            theta_b[i] *= BM_PI / 180;
        }
    }

    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    if (sim) sd.backend = YSCR_BACKEND_SIM;
    sd.windowed = !fullscreen;
    sd.window_w = 1024;
    sd.window_h = 768;
    sd.raw_mice = !is_script;   /* the script feeds counts itself */
    if (!ygfx_open_screen(&gfx, &scr, &sd, &(ygfx_desc){ .background = { 0.05f, 0.05f, 0.05f } })) {
        fprintf(stderr, "trial_bimanual: %s\n", ygfx_error(&gfx));
        return 1;
    }
    ygfx_size(&gfx, &W, &H);
    ygfx_center(&gfx, &cx, &cy);
    yscr_get_caps(&scr, &caps);
    if (caps.mode.refresh_num && track_s * caps.mode.refresh_num / caps.mode.refresh_den > FRAME_CAP - 64) {
        fprintf(stderr, "trial_bimanual: %.0f s at %.1f Hz is more frames than the buffers hold (%d)\n", track_s,
                (double)caps.mode.refresh_num / caps.mode.refresh_den, FRAME_CAP);
        return 1;
    }
    if (ppc_arg > 0) {
        ppc = ppc_arg;
        snprintf(view_note, sizeof view_note, "%g px/cm (--px-per-cm)", ppc);
    } else {
        ppc = (W < H ? W : H) / NOMINAL_CM;
        snprintf(view_note, sizeof view_note, "NOMINAL (the window's smaller side is %.0f cm: %.4g px/cm): cm are not "
                 "cm on the screen", NOMINAL_CM, ppc);
    }
    if (!is_script) yscr_window_call(&scr, grab_on_window, NULL);
    if (!open_graph()) return 1;

    /* the stimuli, px from the top-left */
    {
        ygfx_shape_desc s;
        memset(&s, 0, sizeof s);
        s.shape = YGFX_CIRCLE;
        s.place = YGFX_TOP_LEFT;
        s.x = cx;
        s.y = cy;
        s.w = s.h = (float)(2 * TARGET_R * ppc);
        s.color[0] = s.color[1] = s.color[2] = 0.35f;
        tgt = ygfx_shape(&s);
        s.w = s.h = (float)(2 * START_R * ppc);
        start = ygfx_shape(&s);
        s.w = s.h = (float)(2 * CURSOR_R * ppc);
        s.color[0] = s.color[1] = s.color[2] = 1.0f;
        cur = ygfx_shape(&s);
        s.shape = YGFX_RECT;
        s.w = W * 0.3f;
        s.h = H * 0.5f;
        s.x = W * 0.25f;
        box[0] = ygfx_shape(&s);
        s.x = W * 0.75f;
        box[1] = ygfx_shape(&s);
    }

    /* the session: one table row per block, and an order list that
     * repeats each row for its trials (ysp/trials.h's block_size is one
     * size for every block) */
    {
        size_t len = (size_t)snprintf(blocks_csv, sizeof blocks_csv, "block,type,perturbed\n");
        int k = 0, j;
        for (i = 0; i < n_blocks; i++) {
            len += (size_t)snprintf(blocks_csv + len, sizeof blocks_csv - len, "%d,%c,%d\n", i, btype[i] ? 'R' : 'T',
                                    bpert[i]);
            for (j = 0; j < (btype[i] ? n_reaches : track_trials); j++) order_list[k++] = i;
        }
        memset(&cd, 0, sizeof cd);
        cd.text = blocks_csv;
        cd.len = len;
        cd.arena = table_arena;
        cd.arena_size = sizeof table_arena;
        if (!ytb_open_csv(&tab, &cd)) { fprintf(stderr, "trial_bimanual: %s\n", ytb_error(&tab)); return 1; }
        memset(&rd, 0, sizeof rd);
        rd.table = &tab;
        rd.order = YTR_ORDER_LIST;
        rd.order_list = order_list;
        rd.n_order_list = k;
        rd.rng = ytr_splitmix;
        rd.rng_ctx = &rng;
        if (!ytr_open(&trials, &rd)) { fprintf(stderr, "trial_bimanual: %s\n", ytr_error(&trials)); return 1; }
    }
    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CHANNELS, .keys = keys,
                                    .key_capacity = 16 })) {
        fprintf(stderr, "trial_bimanual: %s\n", ytl_error(&tl));
        return 1;
    }

    /* movement onset: the cursor (cm) 1 cm from the start's center */
    memset(&moved, 0, sizeof moved);
    moved.name = "moved";
    moved.kind = YRSP_KIND_USER + 7;   /* the emit node's derived events */
    moved.device = B_EMIT;
    moved.cross = YRSP_CROSS_DISTANCE;
    moved.control = YRSP_AXIS_POSITION;
    moved.level = (float)START_R;
    sources[0] = yrsp_raw_mouse_source();
    memset(&pd, 0, sizeof pd);
    pd.choices = &moved;
    pd.n_choices = 1;
    pd.persist = true;   /* the trace runs on to the hold */
    pd.sources = sources;
    pd.n_sources = 1;
    pd.trace = trace;
    pd.trace_cap = TRACE_CAP;
    pd.trace_kinds = YRSP_KIND_BIT(YRSP_KIND_USER + 7);
    if (!yrsp_open(&rsp, &pd)) { fprintf(stderr, "trial_bimanual: %s\n", yrsp_error(&rsp)); return 1; }

    out = fopen(out_path, "w");
    rout = fopen(reach_path, "w");
    tout = fopen(track_path, "w");
    qout = fopen(freq_path, "w");
    if (!out || !rout || !tout || !qout) {
        fprintf(stderr, "trial_bimanual: cannot write %s, %s, %s or %s\n", out_path, reach_path, track_path, freq_path);
        return 1;
    }
    /* the target's peak excursion for these phases, at 1 ms */
    for (i = 0; i < n_blocks; i++) {
        int64_t k;
        if (btype[i]) continue;
        for (k = 0; k <= (int64_t)(track_s * 1000); k++) {
            double r[2];
            target_at(i, k / 1000.0, 1, r);
            peak[0] = fmax(peak[0], fabs(r[0]));
            peak[1] = fmax(peak[1], fabs(r[1]));
        }
    }
    yscr_describe(&scr, line, sizeof line);
    ytr_format_meta(&trials, meta, sizeof meta);
    meta[strcspn(meta, "\r\n")] = 0;
    {
        FILE* fs[4];
        int k;
        fs[0] = out; fs[1] = rout; fs[2] = tout; fs[3] = qout;
        for (k = 0; k < 4; k++) {
            fprintf(fs[k], "# blocks %s; mapping %s; perturbation %s", blocks_arg, mapping ? "mean" : "split",
                    pert_name[perturb]);
            if (perturb == 1) fprintf(fs[k], " %.1f deg (+: counterclockwise)", rotation);
            fprintf(fs[k], "; track %.0f s (ramp %.0f s, analysis the last %.0f s); %d tracking trials and %d reaches a "
                           "block; seed %llu\n", track_s, RAMP_S, win_s, track_trials, n_reaches, (unsigned long long)seed);
            fprintf(fs[k], "# view: %s\n# gain %.6g px/count, cpi %.0f: 1 count = %.5g cm on the screen, %.5g cm of hand\n",
                    view_note, gain, cpi, gain / ppc, 2.54 / cpi);
            fprintf(fs[k], "# %s\n# %s\n# ysp_response %s; hands: raw mice (counts, no acceleration)%s\n", line, meta,
                    yrsp_version(), is_script ? ", scripted (tier SIM)" : "");
            fprintf(fs[k], "# target peak |x| %.3f cm, |y| %.3f cm over the T blocks' phases; window half-size %.2f x %.2f cm\n",
                    peak[0], peak[1], W / 2 / ppc, H / 2 / ppc);
        }
        for (i = 0; i < n_blocks; i++) {
            int a;
            if (btype[i]) continue;
            for (a = 0; a < 2; a++) {
                fprintf(out, "# block %d phases %c (rad):", i, a ? 'y' : 'x');
                for (k = 0; k < N_SINES; k++) fprintf(out, " %.6f", phase[i][a][k]);
                fprintf(out, "\n");
            }
        }
    }
    fprintf(out, "trial_index,block,block_trial,perturbation,mapping,mse_cm2,frac_inside,m_a,m_b,m_c,m_d,theta_deg,"
                 "s_rotation,s_mirror,l_a,l_b,l_c,l_d,l_theta_deg,r_a,r_b,r_c,r_d,r_theta_deg,n_frames,n_missed,"
                 "n_norecord,n_other,onset_tier,onset_src\n");
    fprintf(rout, "reach_index,block,block_reach,perturbation,start_x_cm,start_y_cm,target_x_cm,target_y_cm,target_deg,"
                  "rt_s,init_err_deg,peak_speed_cm_s,fast,mt_s,onset_tier,onset_src,move_tier,move_hand,rsp_flags\n");
    fprintf(tout, "trial_index,t_s,target_x,target_y,cursor_x,cursor_y,l_x,l_y,r_x,r_y,inside\n");
    fprintf(qout, "trial_index,block,axis,freq_hz,target_re,target_im,ux_re,ux_im,uy_re,uy_im,lx_re,lx_im,ly_re,ly_im,"
                  "rx_re,rx_im,ry_re,ry_im\n");
    printf("%s\nview: %s\ntarget peak |x| %.3f cm, |y| %.3f cm; window half-size %.2f x %.2f cm\n", line, view_note,
           peak[0], peak[1], W / 2 / ppc, H / 2 / ppc);
    if (peak[0] + TARGET_R > W / 2 / ppc || peak[1] + TARGET_R > H / 2 / ppc)
        printf("warning: the target can leave the window at %.4g px/cm\n", ppc);
    printf("click the left hand's mouse (left button)\n");

    ygfx_prime(&gfx);
    for (;;) {
        SDL_Event ev;
        yrsp_input in;
        double c[2] = { 0, 0 }, speed = 0;
        int rc = yscr_begin(&scr, &f), pass, ok = 1;
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_bimanual: %s\n", yscr_error(&scr)); failed = 1; break; }
        if (is_script && f.index == 0) script_start(f.onset);

        yrsp_update_frame(&rsp, &f);   /* the reach target's flip record */
        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) on_input(&in, ev.type == yscr_input_event_type());
        if (is_script) script_emit(f.onset);
        /* tracking frames: the onset from each record, and the hands then.
         * After the input: reports stamped between a frame's planned and
         * its real onset come in this batch, not the one before. */
        for (i = 0; i < f.n_done; i++) {
            int64_t k = f.done[i].index - (n_fr ? fr[0].index : 0);
            if ((state != S_T_MOVE && state != S_T_FINISH) || k < 0 || k >= n_fr) continue;
            fr[k].missed = f.done[i].dropped > 0;
            if (k == 0) onset0 = yrsp_onset_flip(&f.done[i]);
            finalize((int)k, f.done[i].onset ? f.done[i].onset : fr[k].tp);
            if (f.done[i].onset) dv.resid = fmax(dv.resid, fabs((double)(f.done[i].onset - fr[k].tp)));
        }

        /* the cursor at the frame's onset: the start and hold tests read it
         * once a frame (the graph's value at t, with this block's P) */
        if (setup == 2) {
            value_at(B_C, f.onset, c);
            if (have_prev && f.onset > prev_onset)
                speed = cabs2(c[0] - c_prev[0], c[1] - c_prev[1]) / YRT_TO_S(f.onset - prev_onset);
            have_prev = 1;
        }

        /* the session; a trial that ends here lets the next start on this frame */
        for (pass = 0; pass < 2 && setup == 2 && ok; pass++) {
            if (state == S_NEXT) ok = next_trial(&f);
            if (state == S_DONE || !ok) break;
            if (state == S_T_WAIT && f.onset >= t_ready && speed < STILL_CM_S &&
                c[0] * c[0] + c[1] * c[1] <= TARGET_R * TARGET_R)
                ok = start_tracking(&f);
            if (state == S_T_FINISH) {
                int all = 1;
                for (i = 0; i < n_fr; i++) all &= fr[i].have;
                if (!all && f.index > finish_frame + 8)   /* records that never came: the plan */
                    for (i = 0; i < n_fr; i++)
                        if (!fr[i].have) { finalize(i, fr[i].tp); n_norecord++; }
                if (all || f.index > finish_frame + 8) { end_tracking(); state = S_NEXT; continue; }
            }
            if (state == S_R_START) {
                if (c[0] * c[0] + c[1] * c[1] <= START_R * START_R) {
                    if (!holding) { holding = 1; t_in = f.onset; }
                    if (f.onset - t_in >= YRT_S(START_HOLD_S)) ok = new_target(&f, 1);
                } else holding = 0;
            }
            if (state == S_R_REACH && is_script && !reach_planned) {
                /* the script reaches once the target's onset is final */
                yrsp_onset on;
                if (yrsp_onset_now(&rsp, &on) && on.final) {
                    int j = reach_index < MAX_REACHES ? reach_index : MAX_REACHES - 1, fast = reach_index % 3 != 2;
                    double err = 4.0 * (reach_index % 3 - 1), dir, ce[2], ue[2], ut[2], mt = fast ? SIM_FAST_S : SIM_SLOW_S;
                    int64_t t_go = on.t_ns + YRT_S(SIM_RT_S);
                    if (pert_on) err += 25.0 * pow(0.9, block_reach);
                    dir = atan2(tgt_c[1] - start_c[1], tgt_c[0] - start_c[0]) + err * BM_PI / 180;
                    ce[0] = start_c[0] + REACH_CM * cos(dir);
                    ce[1] = start_c[1] + REACH_CM * sin(dir);
                    unapply_p(pert_on, ce, ue);
                    unapply_p(pert_on, tgt_c, ut);
                    plan_mj(t_go, mt, ue);
                    plan_mj(t_go + YRT_S(mt), SIM_CORR_S, ut);
                    sc.watch = 1;
                    sc.found = 0;
                    sc.from = on.t_ns;
                    sc.ox = (float)start_c[0];
                    sc.oy = (float)start_c[1];
                    sc.level = (float)START_R;
                    rexp[j].err_deg = err;
                    rexp[j].fast = fast;
                    reach_planned = 1;
                }
            }
            if (state == S_R_REACH) {
                double dx = c[0] - tgt_c[0], dy = c[1] - tgt_c[1];
                if (dx * dx + dy * dy <= START_R * START_R && speed < STILL_CM_S) {
                    if (!holding) {
                        /* the hold starts: the peak speed so far sets the color */
                        holding = 1;
                        t_hold = f.onset;
                        peak_hold = peak_run;
                        fast_now = peak_hold > FAST_CM_S;
                    } else if (f.onset - t_hold >= YRT_S(TARGET_HOLD_S)) {
                        end_reach();
                        state = S_NEXT;   /* the next target, or the next block, on this frame */
                        continue;
                    }
                } else holding = 0;
            }
            break;
        }
        if (!ok) { failed = 1; break; }
        if (state == S_DONE) break;

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        yrsp_set_landing(&rsp, fired, n, TARGET_ON);
        {
            /* the graph's frame: the target at the frame's base time, the
             * ramp from the timeline, the binds */
            int64_t bt = 0;
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ysig_frame(&sg, &(ysig_frame_in){ f.index, f.onset, bt, ytl_values(&tl), N_CHANNELS });
        }
        for (i = 0; i < n; i++)
            if (fired[i].kind == YTL_MARK && fired[i].code == END_OF_TRACK && state == S_T_MOVE) {
                state = S_T_FINISH;
                finish_frame = f.index;
                last_end = f.onset;
            }
        if (state == S_T_MOVE) {
            /* the target on this frame: the sum at the frame's base time */
            frame_rec* r = &fr[n_fr];
            const ysig_value* tg = ysig_value_of(&sg, B_TGT);
            if (n_fr >= FRAME_CAP) { fprintf(stderr, "trial_bimanual: more frames than %d\n", FRAME_CAP); failed = 1; break; }
            r->tg[0] = tg->v[0];
            r->tg[1] = tg->v[1];
            r->index = f.index;
            r->tp = f.onset;
            r->ta = 0;
            r->have = r->missed = 0;
            r->dot[0] = px_x(c[0]);
            r->dot[1] = px_y(c[1]);
            n_fr++;
        }

        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        if (setup < 2) {
            for (i = 0; i < 2; i++) box[i].color[0] = box[i].color[1] = box[i].color[2] = i == setup ? 0.9f : 0.2f;
            ygfx_draw(&gfx, &box[0]);
            ygfx_draw(&gfx, &box[1]);
        } else {
            if (state == S_T_WAIT || state == S_T_MOVE || state == S_T_FINISH) {
                /* moving: the graph's target and its inside detect; else at
                 * the center */
                int inside = state == S_T_MOVE ? ysig_value_of(&sg, B_INSIDE)->v[0] != 0
                                               : c[0] * c[0] + c[1] * c[1] <= TARGET_R * TARGET_R;
                tgt.x = state == S_T_MOVE ? sig_ch[CH_TGT_X] : px_x(0);
                tgt.y = state == S_T_MOVE ? sig_ch[CH_TGT_Y] : px_y(0);
                tgt.w = tgt.h = (float)(2 * TARGET_R * ppc);
                tgt.color[0] = inside ? 1.0f : 0.35f;
                tgt.color[1] = inside ? 0.85f : 0.35f;
                tgt.color[2] = inside ? 0.0f : 0.35f;
                ygfx_draw(&gfx, &tgt);
            } else if (state == S_R_START || state == S_R_REACH) {
                start.x = px_x(start_c[0]);
                start.y = px_y(start_c[1]);
                ygfx_draw(&gfx, &start);
                if (state == S_R_REACH) {
                    /* white, then yellow (fast) or blue from the hold's start */
                    tgt.x = px_x(tgt_c[0]);
                    tgt.y = px_y(tgt_c[1]);
                    tgt.w = tgt.h = (float)(2 * START_R * ppc);
                    tgt.color[0] = !holding ? 0.9f : fast_now ? 1.0f : 0.1f;
                    tgt.color[1] = !holding ? 0.9f : fast_now ? 0.85f : 0.35f;
                    tgt.color[2] = !holding ? 0.9f : fast_now ? 0.0f : 1.0f;
                    ygfx_draw(&gfx, &tgt);
                }
            }
            cur.x = px_x(c[0]);
            cur.y = px_y(c[1]);
            ygfx_draw(&gfx, &cur);
            c_prev[0] = c[0];
            c_prev[1] = c[1];
            prev_onset = f.onset;
        }
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    yscr_describe(&scr, line, sizeof line);   /* the counters, at the end */
    fprintf(out, "# at close: %s\n", line);
    fclose(out);
    fclose(rout);
    fclose(tout);
    fclose(qout);
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d tracking trials to %s (frames %s, frequencies %s), %d reaches to %s%s\n", track_index, out_path, track_path,
           freq_path, reach_index, reach_path, aborted ? " (stopped with Shift+Esc)" : "");
    printf("other mice: %d reports ignored; device 0: %d; refused at setup: %d; hand reads older than the ring: %d\n",
           n_other, n_dev0, refused, ring_miss);
    printf("clicks (not used): the hands' mice after the setup %d down, %d up; SDL's pointer %d down, %d up\n",
           n_clicks_raw[0], n_clicks_raw[1], n_clicks_sdl[0], n_clicks_sdl[1]);

    if (is_script && !aborted) {
        printf("frames: L, R, u and the cursor within %.3f of the count grid's bound (%d frames), the drawn cursor %.3f; "
               "flip onsets up to %.1f us from the plan\n", dv.frame, dv.frames, dv.dot, dv.resid / 1e3);
        printf("results: MSE, M, theta, S and the DFTs within %.3f of their count-grid bounds\n", dv.result);
        if (sim) {
            printf("math: the model's analysis within %.3g x 1e-6 of the closed forms (gain, phase, theta, MSE, M, %s)\n",
                   dv.math, mapping ? "d" : "crosstalk");
            printf("the 1 kHz report grid, measured against the model: gain %.4f, phase %.3f deg, theta %.4f deg, MSE "
                   "%.4f, %s %.4f (relative but phase and theta)\n", dv.gain, dv.phase, dv.theta, dv.mse,
                   mapping ? "d" : "crosstalk", dv.kappa);
        }
        printf("reaches: onsets to the ns, the initial direction within %.3f of its bound\n", dv.reach);
        printf("the initial direction against the script's line: %.3f deg at most; %.3f deg from the report times "
               "alone (exact values at each hand's last report)\n", dv.init, dv.init_x);
        if (dv.frame > 1 || dv.result > 1 || dv.dot > 1 || dv.reach > 1 || (sim && dv.math > 1)) failed = 1;
        if (track_index != want_t || reach_index != want_r || refused != 2 || dev[0] != DEV_L || dev[1] != DEV_R ||
            n_other == 0 || ring_miss) {
            fprintf(stderr, "trial_bimanual: check: %d of %d tracking trials, %d of %d reaches, %d refused, mice %u %u, "
                            "%d other reports, %d ring misses\n", track_index, want_t, reach_index, want_r, refused,
                    (unsigned)dev[0], (unsigned)dev[1], n_other, ring_miss);
            failed = 1;
        }
        printf("%s: %s\n", sim ? "--sim" : "--auto", failed ? "a check failed" : "every trial came out as scripted");
    }
    return failed;
}
