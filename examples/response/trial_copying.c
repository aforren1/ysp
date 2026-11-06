/* trial_copying.c - the motor-pattern copying task of Sun, Xie and McDougle
 * 2026 (Nature Human Behaviour, "Motor abstraction training generalizes to
 * the refinement of specific movement patterns"): Experiment 1, and the
 * tracking training of Experiments 7 and 8. ysp/gfx.h, ysp/timeline.h,
 * ysp/trials.h and ysp/response.h, with mouse, raw mouse or pen input.
 *
 * A block is 5 trials on one pattern: upright (pre-test), turned 90, 180
 * and 270 deg clockwise (training, in that order), upright (post-test).
 * Blocks run in a random order (ysp/trials.h: 5 table rows per pattern,
 * one unit each). A copy trial: two 300 x 300 px boxes, one above the
 * other at the center. The top box draws the pattern at a uniform speed in
 * 4 s (the paper's demos took 4 s on average) and keeps it; then the
 * bottom box shows the starting point. A click (or the pen touching) starts
 * the stroke: the ink starts at the starting point and follows the
 * movement from there; the system cursor is hidden. One stroke, no time
 * limit; a pen's ink stops when it lifts; the space bar ends the trial;
 * 0.5 s blank. --mode track makes the 3 training trials tracking trials
 * (Experiment 7): a blue disc (the cursor) in a red disc, a 3 s countdown
 * ring, then the red disc moves along the turned pattern at a uniform speed
 * in 5 s (20% slower than the demo). --backward runs it from the end to the
 * start (Experiment 8).
 *
 * Patterns. The paper's are Omniglot characters; none ship here. Each
 * pattern is made from the seed: 3 sinusoids per axis (0.5 to 2 cycles),
 * fitted into 240 px (a 30 px margin), kept when its length is 2.2 to 4.5
 * times 240 px, its radius of curvature 12 px or more, both extents 108 px
 * or more, and no part runs within 8 px of another part 40 px or more away
 * along the stroke within 25 deg of its direction (no retracing; crossings
 * are allowed). A lab loads its own strokes: --patterns FILE, one pattern
 * per line as x y pairs (any unit, y down, '#' lines skipped), fitted the
 * same way, no rules applied.
 *
 * Score (the paper's Methods). Target and copy are resampled to 1000
 * points at uniform arc-length steps (linear interpolation). D is the
 * Procrustes distance after the best translation, rotation and scale of
 * the copy: 1 - (sum x.R y)^2 / (|x|^2 |y|^2), x, y centered, the residual
 * sum of squares over the target's, 0 to 1. That is MATLAB's
 * procrustes(X, Y, 'reflection', false); its default 'best' would fit a
 * mirror image. MT runs from the first ink sample 2 px or more from the
 * starting point to the last ink sample (the paper does not define the
 * ends). PI = (1 - D) / MT; a block's learning effect is PI(post) -
 * PI(pre). A tracking trial scores the blue disc's path while the red disc
 * moves against the path the red disc took, and their mean distance.
 *
 * Input (--input). mouse: SDL's pointer, message time (tier 3). raw: raw
 * mice through yrsp_cursor, 1 px per count. pen: SDL's pen events
 * (YIN_KIND_PEN), position, tip, pressure; docs/input.md, "Drawing
 * tablets". NOT verified with a tablet. The stroke is the offset from where
 * it started, so a pen inks from the starting point wherever it touches.
 *
 * Ink. A growing stroke is POLYLINE chunks of 32 points: the vector
 * program evaluates every segment of a polyline at every pixel of its
 * quad, so short chunks keep that product small. Measured on an Intel UHD
 * 770 (Mesa 22.3.6, headless 1024 x 768, 2026-11-06; not the RX 6600): a
 * 900 px stroke as chunks of 32 takes 0.6 ms of GPU a frame; as chunks of
 * 224, 5.1 ms (10.6 ms before ysp/gfx.h 0.18.0 centered the quad on the
 * path's box).
 *
 * Data. --out, a row per trial: block, trial (0..4), kind (copy, track),
 * rotation, pattern, rt_s (the stroke's start from the starting point's
 * flip onset), mt_s, d, pi, fit_scale and fit_rot_deg (taking the copy onto
 * the target, clockwise positive), n_samples, track_err_px, input,
 * onset_tier, onset_src, rsp_tier, rsp_flags. --samples, a row per sample:
 * trial, t_s (from that onset, or the red disc's), x, y (window px),
 * pressure (pen). Seconds. Shift+Esc stops and keeps the rows.
 *
 * --sim: 2 blocks on the simulated display. A scripted writer, its events
 * through ysp/input.h's SDL and raw mouse adapters, copies each pattern
 * scaled by 1/0.93 and turned 5 deg with a wobble (14 px pre, 8 px post,
 * none in training) in 7.7 s pre and 6.9 s post, a sample every 8 ms; the
 * tracker keeps the blue disc 3, 4 px off the red one. Checks: the scorer
 * against closed forms; each row against the script (D by an independent
 * Procrustes search, MT, PI, rt, pressure, learning). --auto: in a window.
 *
 * Usage: trial_copying [--sim | --auto] [--input mouse|raw|pen] [--mode copy|track] [--backward]
 *                      [--blocks N] [--patterns FILE] [--seed N] [--out FILE] [--samples FILE]
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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BOXES_ON, DEMO_ON, DEMO_P, START_ON, TRACK_ON, COUNT_ON, COUNT_P, MOVE_ON, MOVE_P, N_CH };
enum { TRIAL = 1, END_OF_TRIAL = 1 };
enum { P_DEMO, P_READY, P_WRITE, P_DONE, P_TRACK };
enum { IN_MOUSE, IN_RAW, IN_PEN };

#define BOX_PX    300.0
#define FIT_PX    240.0
#define DEMO_S    4.0
#define TRACK_S   (DEMO_S / 0.8)   /* 20% slower than the demo */
#define COUNT_S   3.0
#define ITI_S     0.5
#define ONSET_PX  2.0              /* MT starts this far from the start */
#define N_RS      1000             /* the paper's resampling */
#define N_FINE    256              /* a pattern as kept */
#define N_DEMO    200              /* the demo's polyline */
#define MAX_PAT   64
#define INK_STEP  1.5f             /* px between drawn ink points */
#define CHUNK     32               /* points per drawn polyline (the header comment says why) */
#define MAX_PTS   8192
#define MAX_CHUNK (MAX_PTS / (CHUNK - 1) + 1)
#define TRACE_CAP 32768            /* 1.5 MB: 30 s of pen reports with 3 axes */
#define PI_D      3.14159265358979323846
#define SIM_DT    YRT_MS(8)

typedef double pt2[2];
typedef struct { int64_t t; double x, y; float p; } sample;
typedef struct {   /* a stroke drawn as chunks that share their end points */
    float pt[MAX_PTS][2];
    double s[MAX_PTS];             /* arc length at each point */
    int n, built;                  /* points; chunks whose points are final */
    float loc[MAX_CHUNK][CHUNK * 2];
    ygfx_stim st[MAX_CHUNK];
} poly;
typedef struct { int on, done, track; double rx, ry, sx, sy; } stroke_st;

static pt2 pat[MAX_PAT][N_FINE];
static yscr_screen scr;
static ygfx_gfx gfx;
static ytl_timeline tl;
static ytl_event storage[64];
static ytl_key keys[256];
static ytr_trials trials;
static yrsp_collector rsp;
static yrsp_input trace[TRACE_CAP];
static sample samp[MAX_PTS * 2];
static uint64_t table_arena[8192];
static char table_csv[8192];
static ytb_table tab;
static yrsp_cursor cursor;
static yrsp_result res;
static poly demo, ink;
static ygfx_stim boxes[2], start_dot, red, blue, ring;
static pt2 tgt[N_RS];              /* this trial's target, window px */
static double sx, sy, px, py;      /* the starting point; the pointer */
static int input_path, phase;
static int64_t t_ready;
static stroke_st live;
static const ygfx_bind binds[] = {
    { .stim = &boxes[0], .param = YGFX_P_VISIBLE, .channel = BOXES_ON },
    { .stim = &boxes[1], .param = YGFX_P_VISIBLE, .channel = BOXES_ON },
    { .stim = &start_dot, .param = YGFX_P_VISIBLE, .channel = START_ON },
    { .stim = &red, .param = YGFX_P_VISIBLE, .channel = TRACK_ON },
    { .stim = &blue, .param = YGFX_P_VISIBLE, .channel = TRACK_ON },
    { .stim = &ring, .param = YGFX_P_VISIBLE, .channel = COUNT_ON },
    { .stim = &ring, .param = YGFX_P_TRIM_END, .channel = COUNT_P },
};

/* --- geometry and the score ---------------------------------------------- */

/* m points into n at uniform arc-length steps, linear between points;
 * returns the length. */
static double resample(pt2* p, int m, pt2* out, int n) {
    double L = 0, acc = 0, seg;
    int i, j = 0, k;
    for (i = 1; i < m; i++) L += hypot(p[i][0] - p[i - 1][0], p[i][1] - p[i - 1][1]);
    for (k = 0; k < n; k++) {
        double s = L * k / (n - 1), u;
        while (j < m - 2 && acc + (seg = hypot(p[j + 1][0] - p[j][0], p[j + 1][1] - p[j][1])) < s) { acc += seg; j++; }
        seg = m > 1 ? hypot(p[j + 1][0] - p[j][0], p[j + 1][1] - p[j][1]) : 0;
        u = seg > 0 ? fmin((s - acc) / seg, 1) : 0;
        for (i = 0; i < 2; i++) out[k][i] = m > 1 ? p[j][i] + u * (p[j + 1][i] - p[j][i]) : p[0][i];
    }
    return L;
}

/* The point at fraction u (0..1) of an N_RS-point path. */
static void at(pt2* p, double u, double* x, double* y) {
    double f = fmin(fmax(u, 0), 1) * (N_RS - 1);
    int i = (int)f < N_RS - 1 ? (int)f : N_RS - 2;
    *x = p[i][0] + (f - i) * (p[i + 1][0] - p[i][0]);
    *y = p[i][1] + (f - i) * (p[i + 1][1] - p[i][1]);
}

/* Procrustes distance of copy y onto target x by rotation and scale (no
 * reflection), as complex numbers: the best rotation is the argument of
 * sum conj(y) x. scale and rot_deg take y onto x. */
static double procrustes(pt2* x, pt2* y, int n, double* scale, double* rot_deg) {
    double mx = 0, my = 0, nx = 0, ny = 0, ssx = 0, ssy = 0, a = 0, b = 0;
    int k;
    for (k = 0; k < n; k++) { mx += x[k][0]; my += x[k][1]; nx += y[k][0]; ny += y[k][1]; }
    mx /= n; my /= n; nx /= n; ny /= n;
    for (k = 0; k < n; k++) {
        double x0 = x[k][0] - mx, x1 = x[k][1] - my, y0 = y[k][0] - nx, y1 = y[k][1] - ny;
        ssx += x0 * x0 + x1 * x1;
        ssy += y0 * y0 + y1 * y1;
        a += y0 * x0 + y1 * x1;
        b += y0 * x1 - y1 * x0;
    }
    if (!(ssx > 0 && ssy > 0)) { *scale = *rot_deg = NAN; return NAN; }
    *scale = sqrt(a * a + b * b) / ssy;
    *rot_deg = atan2(b, a) * 180 / PI_D;
    return 1 - (a * a + b * b) / (ssx * ssy);
}

/* m points fitted into FIT_PX around 0, 0, then N_FINE points of out;
 * returns the length, 0 for a degenerate pattern. */
static double fit(pt2* raw, int m, pt2* out) {
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300, k;
    int i;
    for (i = 0; i < m; i++) {
        x0 = fmin(x0, raw[i][0]); x1 = fmax(x1, raw[i][0]);
        y0 = fmin(y0, raw[i][1]); y1 = fmax(y1, raw[i][1]);
    }
    if (!(fmax(x1 - x0, y1 - y0) > 0)) return 0;
    k = FIT_PX / fmax(x1 - x0, y1 - y0);
    for (i = 0; i < m; i++) {
        raw[i][0] = (raw[i][0] - 0.5 * (x0 + x1)) * k;
        raw[i][1] = (raw[i][1] - 0.5 * (y0 + y1)) * k;
    }
    return resample(raw, m, out, N_FINE);
}

/* A legible single-stroke pattern from a seed (the header comment's
 * rules). Returns the draws it took, or -1. */
static int make_pattern(uint64_t seed, pt2* out) {
    static pt2 raw[512];
    uint64_t st = seed;
    int tries, i, j, k, ok;
    for (tries = 1; tries <= 2000; tries++) {
        double a[2][3], f[2][3], ph[2][3], L, ds, ext[2][2] = { { 1e300, -1e300 }, { 1e300, -1e300 } };
        for (j = 0; j < 6; j++) {
            a[j / 3][j % 3] = (2 * ytr_splitmix(&st) - 1) / (j % 3 + 1);
            f[j / 3][j % 3] = 0.5 * (1 + floor(4 * ytr_splitmix(&st)));
            ph[j / 3][j % 3] = 2 * PI_D * ytr_splitmix(&st);
        }
        for (i = 0; i < 512; i++)
            for (j = 0; j < 2; j++)
                for (raw[i][j] = 0, k = 0; k < 3; k++) raw[i][j] += a[j][k] * sin(2 * PI_D * f[j][k] * i / 511.0 + ph[j][k]);
        L = fit(raw, 512, out);
        ds = L / (N_FINE - 1);
        for (i = 0; i < N_FINE; i++)
            for (j = 0; j < 2; j++) { ext[j][0] = fmin(ext[j][0], out[i][j]); ext[j][1] = fmax(ext[j][1], out[i][j]); }
        ok = L >= 2.2 * FIT_PX && L <= 4.5 * FIT_PX && fmin(ext[0][1] - ext[0][0], ext[1][1] - ext[1][0]) >= 0.45 * FIT_PX;
        for (i = 1; i < N_FINE - 1 && ok; i++) {   /* turning per px at most 1/12 */
            double d = fabs(atan2(out[i + 1][1] - out[i][1], out[i + 1][0] - out[i][0]) -
                            atan2(out[i][1] - out[i - 1][1], out[i][0] - out[i - 1][0]));
            ok = fmin(d, 2 * PI_D - d) <= ds / 12.0;
        }
        for (i = 0; i < N_FINE - 1 && ok; i++)     /* no part runs along another */
            for (j = i + (int)(40 / ds) + 1; j < N_FINE - 1 && ok; j++) {
                double ux = out[i + 1][0] - out[i][0], uy = out[i + 1][1] - out[i][1];
                double vx = out[j + 1][0] - out[j][0], vy = out[j + 1][1] - out[j][1];
                ok = hypot(out[j][0] - out[i][0], out[j][1] - out[i][1]) >= 8 ||
                     fabs(ux * vx + uy * vy) <= cos(25 * PI_D / 180) * hypot(ux, uy) * hypot(vx, vy);
            }
        if (ok) return tries;
    }
    return -1;
}

static int load_patterns(const char* path) {
    static pt2 raw[4096];
    static char line[65536];
    int n = 0;
    FILE* fp = fopen(path, "r");
    if (!fp) return -1;
    while (n < MAX_PAT - 1 && fgets(line, sizeof line, fp)) {
        char *p = line, *e;
        int m = 0;
        if (line[0] == '#') continue;
        while (m < 4096) {
            raw[m][0] = strtod(p, &e);
            if (e == p) break;
            raw[m][1] = strtod(p = e, &e);
            if (e == p) break;
            p = e;
            m++;
        }
        if (m >= 2 && fit(raw, m, pat[n]) > 0) n++;
    }
    fclose(fp);
    return n;
}

/* Pattern i turned clockwise by deg (y down) at cx, cy, reversed if rev,
 * into tgt and, when d is given, the demo's polyline. */
static void place(int i, int deg, int rev, double cx, double cy, poly* d) {
    static pt2 fine[N_FINE], dm[N_DEMO];
    double c = cos(deg * PI_D / 180), s = sin(deg * PI_D / 180);
    int k;
    for (k = 0; k < N_FINE; k++) {
        const double* q = pat[i][rev ? N_FINE - 1 - k : k];
        fine[k][0] = cx + q[0] * c - q[1] * s;
        fine[k][1] = cy + q[0] * s + q[1] * c;
    }
    resample(fine, N_FINE, tgt, N_RS);
    if (!d) return;
    resample(fine, N_FINE, dm, N_DEMO);
    for (k = 0; k < N_DEMO; k++) {
        d->pt[k][0] = (float)dm[k][0];
        d->pt[k][1] = (float)dm[k][1];
        d->s[k] = k ? d->s[k - 1] + hypot(dm[k][0] - dm[k - 1][0], dm[k][1] - dm[k - 1][1]) : 0;
    }
    d->n = N_DEMO;
    d->built = 0;
}

/* --- drawing a stroke ---------------------------------------------------- */

static void poly_add(poly* p, float x, float y) {
    float l = p->n ? hypotf(x - p->pt[p->n - 1][0], y - p->pt[p->n - 1][1]) : 0;
    if (p->n >= MAX_PTS || (p->n && l < INK_STEP)) return;   /* fewer segments per pixel */
    p->pt[p->n][0] = x;
    p->pt[p->n][1] = y;
    p->s[p->n] = p->n ? p->s[p->n - 1] + l : 0;
    p->n++;
}

/* Chunk c (points c * (CHUNK - 1) on) as a polyline centered on its box. */
static void poly_chunk(poly* p, int c) {
    int a = c * (CHUNK - 1), n = p->n - a < CHUNK ? p->n - a : CHUNK, k;
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (k = a; k < a + n; k++) {
        x0 = fminf(x0, p->pt[k][0]); x1 = fmaxf(x1, p->pt[k][0]);
        y0 = fminf(y0, p->pt[k][1]); y1 = fmaxf(y1, p->pt[k][1]);
    }
    for (k = 0; k < n; k++) {
        p->loc[c][2 * k] = p->pt[a + k][0] - 0.5f * (x0 + x1);
        p->loc[c][2 * k + 1] = p->pt[a + k][1] - 0.5f * (y0 + y1);
    }
    p->st[c] = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_POLYLINE, .place = YGFX_TOP_LEFT, .x = 0.5f * (x0 + x1),
                                              .y = 0.5f * (y0 + y1), .w = x1 - x0 + 3, .h = y1 - y0 + 3, .shape_p = { 3 },
                                              .cap = YGFX_CAP_ROUND, .edge = YGFX_EDGE_COSINE, .edge_width = 1,
                                              .path = p->loc[c], .n_path = n });
}

/* Draws the first `upto` px of the stroke (all with upto < 0): the chunks
 * whose points are final are made once, the growing one each frame. */
static void poly_draw(poly* p, double upto) {
    int c, last = p->n >= 2 ? (p->n - 2) / (CHUNK - 1) : -1;
    for (c = p->built; c <= last; c++) {
        poly_chunk(p, c);
        if (p->n >= (c + 1) * (CHUNK - 1) + 1) p->built = c + 1;
    }
    for (c = 0; c <= last; c++) {
        int a = c * (CHUNK - 1), b = a + (int)p->st[c].n_path - 1;
        if (upto >= 0 && upto <= p->s[a]) break;
        p->st[c].trim[1] = upto >= 0 && upto < p->s[b] ? (float)((upto - p->s[a]) / (p->s[b] - p->s[a])) : 1;
        ygfx_draw(&gfx, &p->st[c]);
    }
}

/* --- input --------------------------------------------------------------- */

/* One input into the stroke: 1 and the ink point in *x, *y for a point of
 * it, 2 for the pen's pressure, 0 for the rest. A copy starts at the first
 * click (pen down) at or after t_from and ends when the pen lifts; a
 * tracking stroke is every position. */
static int stroke_step(stroke_st* s, const yrsp_input* in, int64_t t_from, double* x, double* y) {
    int pen = in->kind == YRSP_KIND_PEN, tip = pen ? 0 : 1;
    int pos = in->type == YRSP_SAMPLE && in->control == YRSP_AXIS_POSITION;
    if (s->done) return 0;
    if (pen && in->type == YRSP_SAMPLE && in->control == YRSP_AXIS_PEN_PRESSURE) return s->on ? 2 : 0;
    if (!s->on) {
        if (in->type != YRSP_PRESS || in->control != (uint32_t)tip || in->t < t_from) return 0;
        s->rx = in->x;
        s->ry = in->y;
        s->on = 1;
    } else if (pen && !s->track && in->type == YRSP_RELEASE && in->control == 0) {
        s->done = 1;
        return 0;
    } else if (!pos) return 0;
    *x = s->sx + in->x - s->rx;
    *y = s->sy + in->y - s->ry;
    return 1;
}

/* The stroke again from the trace, as an analysis of a saved trace would
 * read it: samples with t in [t0, t1]. A pressure belongs to the position
 * before it: SDL 3.4's backends send a report's motion, then its axes. */
static int replay(stroke_st st, int64_t t_from, int64_t t0, int64_t t1) {
    float p = 0;
    int k, n = 0;
    st.on = st.track;   /* a copy starts again at its press */
    st.done = 0;
    for (k = 0; k < res.n_trace; k++) {
        double x, y;
        int r = stroke_step(&st, &trace[k], t_from, &x, &y);
        if (r == 2 && n) samp[n - 1].p = trace[k].value;
        if (r == 2) p = trace[k].value;
        if (r == 1 && trace[k].t >= t0 && trace[k].t <= t1 && n < MAX_PTS * 2) {
            samp[n].t = trace[k].t; samp[n].x = x; samp[n].y = y; samp[n].p = p;
            n++;
        }
    }
    return n;
}

static void input(const yrsp_input* ev, int from_bridge) {
    yrsp_input in = *ev;
    double x, y;
    if (in.kind == YRSP_KIND_KEYBOARD) {
        yrsp_feed(&rsp, &in);
        if (in.type == YRSP_PRESS && in.control == SDL_SCANCODE_SPACE && phase == P_WRITE) {
            yrsp_finish(&rsp, &res);   /* the trace stops here */
            phase = P_DONE;
        }
        return;
    }
    if (in.kind != (input_path == IN_PEN ? YRSP_KIND_PEN : YRSP_KIND_MOUSE) || from_bridge != (input_path == IN_RAW)) return;
    if (input_path == IN_RAW) {   /* counts to the drawn cursor's position */
        if (in.type == YRSP_SAMPLE && !yrsp_cursor_feed(&cursor, &in)) return;
        if (in.type == YRSP_SAMPLE) in.control = YRSP_AXIS_POSITION;
        in.x = cursor.x;
        in.y = cursor.y;
    }
    if (in.type != YRSP_PROXIMITY && (in.type != YRSP_SAMPLE || in.control == YRSP_AXIS_POSITION)) {
        px = in.x;
        py = in.y;
    }
    yrsp_feed(&rsp, &in);
    if ((phase == P_READY || phase == P_WRITE) && stroke_step(&live, &in, t_ready, &x, &y) == 1) poly_add(&ink, (float)x, (float)y);
    if (phase == P_READY && live.on) phase = P_WRITE;
}

/* SDL calls on the window go through yscr_window_call() (WINDOW THREAD). */
static void hide_cursor(void* grab) {
    SDL_HideCursor();
    if (grab) SDL_SetWindowMouseGrab(yscr_window(&scr), true);   /* a click goes to the window under it */
}
static void center_pointer(void* unused) {   /* away from the edges, where it would stop */
    int w, h;
    (void)unused;
    SDL_GetWindowSize(yscr_window(&scr), &w, &h);
    SDL_WarpMouseInWindow(yscr_window(&scr), 0.5f * w, 0.5f * h);
}

/* --- the scripted writer (--sim, --auto) --------------------------------- */

static struct {
    int on, k, n, kind, pen_up;            /* kind: 0 copy, 1 track */
    int64_t t0;
    double amp, scale, rot, bx, by, ix, iy;   /* the path's base; the raw cursor's whole counts */
    pt2 pts[MAX_PTS * 2];                  /* the positions the program should see */
    int64_t ts[MAX_PTS * 2];
    float pr[MAX_PTS * 2];
    int n_pts;
} script;

/* One event through the screen's path a real run uses (its key filter and
 * synthetic-mouse rule), stamped SIM. */
static void sim_sdl(SDL_Event* e, int64_t t) {
    yrsp_input in;
    if (yscr_event_input_at(&scr, e, t, &in)) {
        in.stamp = YRSP_TIER_SIM;
        input(&in, 0);
    }
}
static void sim_raw(int64_t t, int dx, int dy, uint8_t down, uint8_t up) {
    yin_mouse_report r = { 0 };
    yrsp_input e[12];
    int i, n;
    r.t = t; r.device = 7; r.dx = dx; r.dy = dy; r.down = down; r.up = up;
    for (n = yin_from_mouse(&r, e, 12), i = 0; i < n; i++) { e[i].stamp = YRSP_TIER_SIM; input(&e[i], 1); }
}

/* Step k: a copy is the target about its center, scaled, turned and
 * wobbled; a tracker the red disc's position 3, 4 px off. A press, its
 * release, and the pen's pressure after each motion. */
static void sim_step(int64_t t, int press) {
    double u = (double)script.k / script.n, c = cos(script.rot), s = sin(script.rot), q[2], x, y;
    float pr = (float)(0.3 + 0.4 * u);
    SDL_Event e;
    at(tgt, u, &q[0], &q[1]);
    if (script.kind) {
        x = script.bx + q[0] - tgt[0][0] + 3;
        y = script.by + q[1] - tgt[0][1] + 4;
    } else {
        q[0] -= tgt[N_RS / 2][0]; q[1] -= tgt[N_RS / 2][1];
        x = script.bx + script.scale * (q[0] * c - q[1] * s) + script.amp * sin(2 * PI_D * 7 * u);
        y = script.by + script.scale * (q[0] * s + q[1] * c) + script.amp * cos(2 * PI_D * 5 * u);
    }
    memset(&e, 0, sizeof e);
    if (input_path == IN_RAW) {   /* a report's press comes before its movement: move, then press */
        int dx = (int)lround(x - script.ix), dy = (int)lround(y - script.iy);
        script.ix += dx; script.iy += dy;
        if (dx || dy) sim_raw(t, dx, dy, 0, 0);
        if (press) sim_raw(t, 0, 0, YIN_MOUSE_LEFT, 0);
        if (press) sim_raw(t + YRT_MS(1), 0, 0, 0, YIN_MOUSE_LEFT);
        x = script.ix; y = script.iy;
        if (!dx && !dy && !press) return;   /* no movement, no sample */
    } else if (input_path == IN_MOUSE) {
        e.type = press ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_MOTION;
        if (press) { e.button.button = 1; e.button.down = true; e.button.x = (float)x; e.button.y = (float)y; }
        else { e.motion.x = (float)x; e.motion.y = (float)y; }
        sim_sdl(&e, t);
        e.type = SDL_EVENT_MOUSE_BUTTON_UP; e.button.down = false;
        if (press) sim_sdl(&e, t + YRT_MS(1));
    } else {
        e.type = press ? SDL_EVENT_PEN_DOWN : SDL_EVENT_PEN_MOTION;
        if (press) { e.ptouch.which = 1; e.ptouch.down = true; e.ptouch.x = (float)x; e.ptouch.y = (float)y; }
        else { e.pmotion.which = 1; e.pmotion.x = (float)x; e.pmotion.y = (float)y; }
        sim_sdl(&e, t);
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_PEN_AXIS;
        e.paxis.which = 1; e.paxis.axis = SDL_PEN_AXIS_PRESSURE; e.paxis.x = (float)x; e.paxis.y = (float)y; e.paxis.value = pr;
        if (!script.kind) sim_sdl(&e, t);
    }
    if (script.n_pts < MAX_PTS * 2) {
        script.pts[script.n_pts][0] = (float)x; script.pts[script.n_pts][1] = (float)y;
        script.ts[script.n_pts] = t;
        script.pr[script.n_pts++] = pr;
    }
}

/* The events due by now. A copy: the pen in range and a hover (no ink),
 * the press 0.4 s after the starting point, a sample every 8 ms, the pen's
 * lift and a hover (no ink), the space bar 0.6 s after the last sample. */
static void sim_run(int64_t now) {
    SDL_Event e;
    int64_t t, tl0 = script.t0 + script.n * SIM_DT;
    while (script.on && (t = script.t0 + script.k * SIM_DT) <= now) {
        memset(&e, 0, sizeof e);
        if (script.k > script.n) {
            if (script.kind) { script.on = 0; break; }
            if (input_path == IN_PEN && !script.pen_up && tl0 + YRT_MS(10) <= now) {
                e.type = SDL_EVENT_PEN_UP; e.ptouch.which = 1;
                e.ptouch.x = (float)script.pts[script.n_pts - 1][0]; e.ptouch.y = (float)script.pts[script.n_pts - 1][1];
                sim_sdl(&e, tl0 + YRT_MS(10));
                e.type = SDL_EVENT_PEN_MOTION; e.pmotion.x += 30;
                sim_sdl(&e, tl0 + YRT_MS(50));
                script.pen_up = 1;
            }
            if (tl0 + YRT_MS(600) > now) break;
            memset(&e, 0, sizeof e);
            e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_SPACE; e.key.down = true;
            sim_sdl(&e, tl0 + YRT_MS(600));
            script.on = 0;
            break;
        }
        if (script.k == 0 && !script.kind && input_path == IN_PEN) {
            e.type = SDL_EVENT_PEN_PROXIMITY_IN; e.pproximity.which = 1;
            sim_sdl(&e, t - YRT_MS(100));
            e.type = SDL_EVENT_PEN_MOTION; e.pmotion.which = 1; e.pmotion.x = (float)script.bx; e.pmotion.y = (float)script.by;
            sim_sdl(&e, t - YRT_MS(50));
        }
        sim_step(t, script.k == 0 && !script.kind);
        script.k++;
    }
}

static void sim_start(int64_t onset, int kind, int trial) {
    memset(&script, 0, sizeof script);   /* 330 KB, once a trial */
    script.on = 1;
    script.kind = kind;
    script.ix = input_path == IN_RAW ? lround(cursor.x) : px;
    script.iy = input_path == IN_RAW ? lround(cursor.y) : py;
    script.bx = script.ix + (kind || input_path == IN_RAW ? 0 : 37);
    script.by = script.iy - (kind || input_path == IN_RAW ? 0 : 21);
    script.t0 = onset + (kind ? 0 : YRT_MS(400));
    script.n = (int)(YRT_S(kind ? TRACK_S : trial == 0 ? 7.7 : trial == 4 ? 6.9 : 7.0) / SIM_DT);
    script.amp = kind ? 0 : trial == 0 ? 14 : trial == 4 ? 8 : 0;
    script.scale = 1 / 0.93;
    script.rot = 5 * PI_D / 180;
}

/* sum x.R(t)y over centered points */
static double turned_dot(pt2* x, pt2* y, int n, const double* m, double t) {
    double v = 0;
    int k;
    for (k = 0; k < n; k++) {
        double y0 = y[k][0] - m[2], y1 = y[k][1] - m[3];
        v += (x[k][0] - m[0]) * (y0 * cos(t) - y1 * sin(t)) + (x[k][1] - m[1]) * (y0 * sin(t) + y1 * cos(t));
    }
    return v;
}

/* An independent Procrustes for the checks: the rotation by a search (a
 * 0.5 deg scan, then golden sections), scale and translation by least
 * squares; the residual over the target's sum of squares. */
static double procrustes_ref(pt2* x, pt2* y, int n) {
    double m[4] = { 0, 0, 0, 0 }, ssx = 0, ssy = 0, best = -1e300, th = 0, lo, hi;
    int k, i;
    for (k = 0; k < n; k++)
        for (i = 0; i < 4; i++) m[i] += (i < 2 ? x : y)[k][i % 2] / n;
    for (k = 0; k < n; k++)
        for (i = 0; i < 2; i++) { ssx += pow(x[k][i] - m[i], 2); ssy += pow(y[k][i] - m[2 + i], 2); }
    for (i = 0; i < 720; i++) {
        double v = turned_dot(x, y, n, m, i * PI_D / 360);
        if (v > best) { best = v; th = i * PI_D / 360; }
    }
    for (lo = th - PI_D / 360, hi = th + PI_D / 360, i = 0; i < 80; i++) {
        double t1 = hi - 0.618 * (hi - lo), t2 = lo + 0.618 * (hi - lo), v1 = turned_dot(x, y, n, m, t1),
               v2 = turned_dot(x, y, n, m, t2);
        if (v1 > v2) { hi = t2; best = v1; } else { lo = t1; best = v2; }
    }
    return (ssx - best * best / ssy) / ssx;
}

/* The scorer against closed forms: a straight stroke against one bent by
 * phi at its middle (equal arms, so the resampled points correspond),
 * D = tan^2(phi/2) / (4 + tan^2(phi/2)) in the continuous limit; a similar
 * copy (D 0, scale and rotation back); a mirror image (not fitted). */
static int check_scorer(void) {
    static pt2 a[N_RS], b[N_RS], c[N_RS], tmp[N_FINE];
    const double ph = 60 * PI_D / 180, t2 = pow(tan(ph / 2), 2), c5 = cos(5 * PI_D / 180), s5 = sin(5 * PI_D / 180);
    pt2 line[3] = { { -100, 0 }, { 0, 0 }, { 100, 0 } };
    pt2 bent[3] = { { -100 * cos(ph / 2), 100 * sin(ph / 2) }, { 0, 0 }, { 100 * cos(ph / 2), 100 * sin(ph / 2) } };
    double sc, rot, d1, d2, d3;
    int k;
    resample(line, 3, a, N_RS);
    resample(bent, 3, b, N_RS);
    d1 = procrustes(a, b, N_RS, &sc, &rot);
    for (k = 0; k < N_FINE; k++) {   /* pattern 0 scaled by 1/0.93, turned 5 deg */
        tmp[k][0] = (pat[0][k][0] * c5 - pat[0][k][1] * s5) / 0.93 + 17;
        tmp[k][1] = (pat[0][k][0] * s5 + pat[0][k][1] * c5) / 0.93 - 9;
    }
    resample(pat[0], N_FINE, a, N_RS);
    resample(tmp, N_FINE, b, N_RS);
    for (k = 0; k < N_RS; k++) { c[k][0] = -a[k][0]; c[k][1] = a[k][1]; }
    d3 = procrustes(a, c, N_RS, &sc, &rot);
    d2 = procrustes(a, b, N_RS, &sc, &rot);
    printf("scorer: bent 60 deg D %.6f (closed form %.6f); similar D %.2e, scale %.6f, rot %.6f deg; mirror D %.4f\n",
           d1, t2 / (4 + t2), d2, sc, rot, d3);
    return fabs(d1 - t2 / (4 + t2)) > 5e-4 || fabs(d2) > 1e-12 || fabs(sc - 0.93) > 1e-9 || fabs(rot + 5) > 1e-9 ||
           d3 < 0.05 || fabs(procrustes_ref(a, c, N_RS) - d3) > 1e-9;
}

static ygfx_stim disc(float w, float r, float g, float b) {
    return ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_CIRCLE, .place = YGFX_TOP_LEFT, .w = w, .h = w,
                                          .edge = YGFX_EDGE_COSINE, .edge_width = 1, .color = { r, g, b } });
}

int main(int argc, char** argv) {
    yscr_desc sd = { 0 };
    ytr_desc rd = { 0 };
    yrsp_choice tip = { 0 };
    yrsp_source source;
    yscr_frame f;
    ytl_event fired[64];
    ytr_trial_info ti = { 0 };
    FILE *out, *sf;
    char line[1024];
    const char *out_path = "trial_copying.csv", *samp_path = "trial_copying_samples.csv", *pat_path = NULL, *a;
    static const char* in_names[3] = { "mouse", "raw", "pen" };
    uint64_t seed = 20261106, seed0;
    int i, n, sim = 0, autorun = 0, track = 0, backward = 0, blocks = 0, aborted = 0, failed = 0, bad = 0, rows = 0;
    int in_trial = 0, step = 0, rot = 0, pid = 0, ended_seen = 0, n_learn = 0, n_ref = 0;
    double pi_pre = NAN, ref_pre = NAN, sum_learn = 0, sum_ref = 0, demo_cy, wr_cy, W2, H2;
    float W, H;
    int64_t max_draw_ns = 0;
    long dropped = 0;

    for (i = 1; i < argc; i++) {
        a = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(argv[i], "--sim")) sim = 1;
        else if (!strcmp(argv[i], "--auto")) autorun = 1;
        else if (!strcmp(argv[i], "--backward")) backward = 1;
        else if (!strcmp(argv[i], "--input") && (input_path = !strcmp(a, "mouse") ? 0 : !strcmp(a, "raw") ? 1 : !strcmp(a, "pen") ? 2 : -1) >= 0) i++;
        else if (!strcmp(argv[i], "--mode") && (track = !strcmp(a, "track") ? 1 : !strcmp(a, "copy") ? 0 : -1) >= 0) i++;
        else if (!strcmp(argv[i], "--blocks") && *a) blocks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--patterns") && *a) pat_path = argv[++i];
        else if (!strcmp(argv[i], "--seed") && *a) seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--out") && *a) out_path = argv[++i];
        else if (!strcmp(argv[i], "--samples") && *a) samp_path = argv[++i];
        else break;
    }
    if (i < argc || (backward && !track) || (sim && autorun)) {
        fprintf(stderr, "usage: trial_copying [--sim | --auto] [--input mouse|raw|pen] [--mode copy|track] [--backward]\n"
                        "                     [--blocks N] [--patterns FILE] [--seed N] [--out FILE] [--samples FILE]\n");
        return 2;
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sim) yrt_virtual_begin(0);
    seed0 = seed;
    if (blocks == 0) blocks = sim || autorun ? 2 : 30;
    if (blocks < 1 || blocks > MAX_PAT - 1) { fprintf(stderr, "trial_copying: --blocks takes 1..%d\n", MAX_PAT - 1); return 2; }
    if (pat_path) {
        if ((n = load_patterns(pat_path)) < 1) { fprintf(stderr, "trial_copying: no pattern in %s\n", pat_path); return 1; }
        if (blocks > n) blocks = n;
    } else {
        int64_t t0 = yrt_real_ns(), tries = 0;
        for (i = 0; i < blocks; i++) {
            if ((n = make_pattern(seed * 1000003u + (uint64_t)i, pat[i])) < 0) { fprintf(stderr, "trial_copying: no pattern\n"); return 1; }
            tries += n;
        }
        printf("%d patterns in %lld draws, %.1f ms\n", blocks, (long long)tries, (yrt_real_ns() - t0) / 1e6);
    }
    if (sim || autorun) bad |= check_scorer();

    sd = (yscr_desc){ .backend = sim ? YSCR_BACKEND_SIM : 0, .windowed = true, .window_w = 1024, .window_h = 768,
                      .raw_mice = input_path == IN_RAW && !sim && !autorun };
    /* chunks of a long ink take more blocks than the default 1024 */
    if (!ygfx_open_screen(&gfx, &scr, &sd, &(ygfx_desc){ .background = { 0.5f, 0.5f, 0.5f }, .max_draws = 4096 })) {
        fprintf(stderr, "trial_copying: %s\n", ygfx_error(&gfx));
        return 1;
    }
    ygfx_size(&gfx, &W, &H);
    W2 = 0.5 * W; H2 = 0.5 * H;
    demo_cy = H2 - 0.5 * BOX_PX - 10, wr_cy = H2 + 0.5 * BOX_PX + 10;
    for (i = 0; i < 2; i++)
        boxes[i] = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_RECT, .place = YGFX_TOP_LEFT, .x = (float)W2,
                                                  .y = (float)(i ? wr_cy : demo_cy), .w = (float)BOX_PX,
                                                  .h = (float)BOX_PX, .color = { 0.95f, 0.95f, 0.95f } });
    start_dot = disc(12, 0.1f, 0.6f, 0.2f);
    red = disc(40, 0.8f, 0.1f, 0.1f);
    blue = disc(16, 0.1f, 0.25f, 0.9f);
    ring = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_CIRCLE, .place = YGFX_TOP_LEFT, .w = 64, .h = 64, .stroke = 4,
                                          .edge = YGFX_EDGE_COSINE, .edge_width = 1, .color = { 0.1f, 0.1f, 0.1f } });
    cursor.w = W; cursor.h = H; cursor.gain = 1;
    if (!sim && !autorun) yscr_window_call(&scr, hide_cursor, input_path == IN_PEN ? NULL : &scr);

    /* 5 rows per pattern; each pattern's rows are one unit (a block) */
    n = snprintf(table_csv, sizeof table_csv, "pattern,step,rotation\n");
    for (i = 0; i < 5 * blocks; i++)
        n += snprintf(table_csv + n, sizeof table_csv - (size_t)n, "p%d,%d,%d\n", i / 5, i % 5, i % 5 == 4 ? 0 : 90 * (i % 5));
    if (!ytb_open_csv(&tab, &(ytb_csv_desc){ .text = table_csv, .len = (size_t)n, .arena = table_arena,
                                               .arena_size = sizeof table_arena })) {
        fprintf(stderr, "trial_copying: %s\n", ytb_error(&tab));
        return 1;
    }
    rd.table = &tab;
    rd.reps = 1;
    rd.order = YTR_ORDER_CONSTRAINED;
    rd.constraints[0] = ytr_chunk(ytb_col(&tab, "pattern"));
    rd.n_constraints = 1;
    rd.rng = ytr_splitmix;
    rd.rng_ctx = &seed;
    if (!ytr_open(&trials, &rd)) { fprintf(stderr, "trial_copying: %s\n", ytr_error(&trials)); return 1; }
    if (!ytl_open(&tl, &(ytl_desc){ .events = storage, .event_capacity = 64, .n_channels = N_CH, .keys = keys, .key_capacity = 256 })) {
        fprintf(stderr, "trial_copying: %s\n", ytl_error(&tl));
        return 1;
    }

    /* the stroke's start (a click, the pen's tip) is the response; the
     * trace is every pointer or pen input of the window */
    tip.kind = input_path == IN_PEN ? YRSP_KIND_PEN : YRSP_KIND_MOUSE;
    tip.control = input_path == IN_PEN ? 0 : 1;
    tip.name = input_path == IN_PEN ? "pen" : "click";
    source = input_path == IN_RAW ? yrsp_raw_mouse_source() : yrsp_sdl_source(NULL, tip.kind);
    if (!yrsp_open(&rsp, &(yrsp_desc){ .choices = &tip, .n_choices = 1, .persist = true, .sources = &source, .n_sources = 1,
                                       .trace = trace, .trace_cap = TRACE_CAP, .trace_kinds = YRSP_KIND_BIT(tip.kind) })) {
        fprintf(stderr, "trial_copying: %s\n", yrsp_error(&rsp));
        return 1;
    }

    out = fopen(out_path, "w");
    sf = fopen(samp_path, "w");
    if (!out || !sf) { fprintf(stderr, "trial_copying: cannot write %s or %s\n", out_path, samp_path); return 1; }
    yscr_describe(&scr, line, sizeof line);
    fprintf(out, "# blocks %d, seed %llu, patterns %s, mode %s%s\n# %s\n# ysp_response %s; input %s: %s\n", blocks,
            (unsigned long long)seed0, pat_path ? pat_path : "procedural", track ? "track" : "copy",
            backward ? " backward" : "", line, yrsp_version(), in_names[input_path],
            sim || autorun ? "scripted (tier SIM)" : source.note ? source.note : "");
    fprintf(out, "# demo box center %.0f, %.0f; writing box center %.0f, %.0f; %.0f px; window px, y down\n", W2, demo_cy, W2,
            wr_cy, BOX_PX);
    fprintf(out, "block,trial,kind,rotation,pattern,rt_s,mt_s,d,pi,fit_scale,fit_rot_deg,n_samples,track_err_px,input,"
                 "onset_tier,onset_src,rsp_tier,rsp_flags\n");
    fprintf(sf, "trial,t_s,x,y,pressure\n");
    printf("%s\n", line);

    for (;;) {
        SDL_Event ev;
        yrsp_input in;
        const float* v;
        int rc = yscr_begin(&scr, &f), kt = track && step >= 1 && step <= 3;
        int64_t bt = 0, t_draw;
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "trial_copying: %s\n", yscr_error(&scr)); failed = 1; break; }
        for (i = 0; i < f.n_done; i++) dropped += (long)f.done[i].dropped;
        yrsp_update_frame(&rsp, &f);   /* the onset's flip record, when it arrives: final */

        if (!in_trial) {
            ytl_seq q;
            if (ytr_next(&trials, &ti) < 0) break;
            pid = atoi(ytb_text(&tab, ti.condition, 0) + 1);
            step = atoi(ytb_text(&tab, ti.condition, 1));
            rot = atoi(ytb_text(&tab, ti.condition, 2));
            kt = track && step >= 1 && step <= 3;
            ytl_clear(&tl, TRIAL);
            ytl_anchor(&tl, TRIAL, f.onset, 0);
            q = ytl_seq_on(&tl, TRIAL);
            memset(&live, 0, sizeof live);
            ink.n = ink.built = 0;
            if (!sim && !autorun && input_path == IN_MOUSE) yscr_window_call(&scr, center_pointer, NULL);
            cursor.x = (float)W2; cursor.y = (float)H2;
            if (input_path == IN_RAW) { px = W2; py = H2; }
            if (kt) {   /* the countdown, then the red disc moves */
                place(pid, rot, backward, W2, H2, NULL);
                ytl_on(&q, TRACK_ON);
                ytl_on(&q, COUNT_ON);
                ytl_to(&q, COUNT_P, &(ytl_tween_desc){ .from = 1, .from_set = true, .to = 0, .duration_ns = YRT_S(COUNT_S) });
                ytl_wait(&q, YRT_S(COUNT_S));
                ytl_off(&q, COUNT_ON);
                ytl_on(&q, MOVE_ON);
                ytl_to(&q, MOVE_P, &(ytl_tween_desc){ .from = 0, .from_set = true, .to = 1, .duration_ns = YRT_S(TRACK_S) });
                ytl_wait(&q, YRT_S(TRACK_S + 0.3));
                ytl_off(&q, TRACK_ON);
                ytl_off(&q, MOVE_ON);
                ytl_wait(&q, YRT_S(ITI_S));
                ytl_mark(&q, END_OF_TRIAL, 0);
                phase = P_TRACK;
            } else {    /* the demo, then the starting point */
                place(pid, rot, 0, W2, demo_cy, &demo);
                place(pid, rot, 0, W2, wr_cy, NULL);
                ytl_on(&q, BOXES_ON);
                ytl_on(&q, DEMO_ON);
                ytl_to(&q, DEMO_P, &(ytl_tween_desc){ .from = 0, .from_set = true, .to = 1, .duration_ns = YRT_S(DEMO_S) });
                ytl_wait(&q, YRT_S(DEMO_S));
                ytl_on(&q, START_ON);
                phase = P_DEMO;
            }
            live.sx = sx = tgt[0][0];
            live.sy = sy = tgt[0][1];
            live.track = kt;
            if (q.err) { fprintf(stderr, "trial_copying: timeline error %d\n", q.err); failed = 1; break; }
            in_trial = 1;
            ended_seen = 0;
        }

        while (yscr_poll(&scr, &ev, NULL))
            if (yscr_event_input(&scr, &ev, &in)) input(&in, ev.type == yscr_input_event_type());
        if (sim || autorun) sim_run(f.onset);

        if (phase == P_DONE && !ended_seen) {   /* the space bar: everything off, then the blank */
            ytl_seq q = ytl_seq_on(&tl, TRIAL);
            ytl_base_time(&tl, TRIAL, f.onset, &bt);
            ytl_at(&q, bt);
            ytl_off(&q, BOXES_ON);
            ytl_off(&q, DEMO_ON);
            ytl_off(&q, START_ON);
            ytl_wait(&q, YRT_S(ITI_S));
            ytl_mark(&q, END_OF_TRIAL, 0);
            ended_seen = 1;
        }

        n = ytl_evaluate(&tl, &(ytl_frame){ f.onset, f.period, f.index }, fired, 64);
        for (i = 0; i < n; i++) {
            if (fired[i].kind != YTL_ONSET) continue;
            if (fired[i].target == START_ON || fired[i].target == TRACK_ON) {
                /* the window opens with the starting point, or the countdown */
                yrsp_arm(&rsp, f.onset);
                t_ready = f.onset;
                live.rx = px; live.ry = py;
                if (fired[i].target == START_ON) phase = P_READY;
                if ((sim || autorun) && fired[i].target == START_ON) sim_start(f.onset, 0, step);
            }
            if (fired[i].target == MOVE_ON && (sim || autorun)) sim_start(f.onset, 1, step);
        }
        yrsp_set_landing(&rsp, fired, n, START_ON);
        yrsp_set_landing(&rsp, fired, n, MOVE_ON);
        for (i = 0; i < n; i++) {
            static pt2 cp[MAX_PTS * 2], cr[N_RS];
            double scale = NAN, frot = NAN, d = NAN, mt = NAN, err = NAN, pi, rt_s = NAN, x, y;
            int k, n0 = 0, n_samp;
            int64_t tz;
            if (fired[i].kind != YTL_MARK || fired[i].code != END_OF_TRIAL) continue;
            yrsp_finish(&rsp, &res);   /* again: the final onset */
            tz = res.t_onset;
            if (kt) {   /* the blue disc while the red one moves */
                n_samp = replay(live, 0, tz, tz + YRT_S(TRACK_S));
                for (err = 0, k = 0; k < n_samp; k++) {
                    at(tgt, YRT_TO_S(samp[k].t - tz) / TRACK_S, &x, &y);
                    err += hypot(samp[k].x - x, samp[k].y - y) / n_samp;
                }
                mt = TRACK_S;
            } else {
                n_samp = replay(live, t_ready, INT64_MIN, INT64_MAX);
                if (res.rt_ns != YRSP_NO_TIME) rt_s = YRT_TO_S(res.rt_ns);
                while (n0 < n_samp && hypot(samp[n0].x - sx, samp[n0].y - sy) < ONSET_PX) n0++;
                if (n0 < n_samp) mt = YRT_TO_S(samp[n_samp - 1].t - samp[n0].t);
            }
            for (k = 0; k < n_samp; k++) { cp[k][0] = samp[k].x; cp[k][1] = samp[k].y; }
            if (n_samp >= 2) {
                resample(cp, n_samp, cr, N_RS);
                d = procrustes(tgt, cr, N_RS, &scale, &frot);
            }
            pi = (1 - d) / mt;
            fprintf(out, "%d,%d,%s,%d,%d,", rows / 5, step, kt ? "track" : "copy", rot, pid);
            if (isnan(rt_s)) fprintf(out, ","); else fprintf(out, "%.9f,", rt_s);
            fprintf(out, "%.9f,%.9f,%.9f,%.9f,%.6f,%d,", mt, d, pi, scale, frot, n_samp);
            if (isnan(err)) fprintf(out, ","); else fprintf(out, "%.4f,", err);
            fprintf(out, "%s,%u,%u,%u,%u\n", in_names[input_path], (unsigned)res.onset_tier, (unsigned)res.onset_src,
                    (unsigned)res.stamp_tier, (unsigned)res.flags);
            for (k = 0; k < n_samp; k++) {
                fprintf(sf, "%d,%.9f,%.3f,%.3f,", ti.index, YRT_TO_S(samp[k].t - tz), samp[k].x, samp[k].y);
                if (input_path == IN_PEN) fprintf(sf, "%.4f\n", samp[k].p); else fprintf(sf, "\n");
            }
            fflush(out);
            fflush(sf);
            printf("block %2d trial %d %-5s rot %3d pattern %2d: D %.5f, MT %.3f s, PI %.5f, scale %.4f, rot %.3f, %d samples",
                   rows / 5, step, kt ? "track" : "copy", rot, pid, d, mt, pi, scale, frot, n_samp);
            if (kt) printf(", error %.3f px", err);
            printf("\n");
            if (step == 0) pi_pre = pi;
            if (step == 4 && !isnan(pi_pre) && !isnan(pi)) { sum_learn += pi - pi_pre; n_learn++; }

            if (sim || autorun) {   /* this row against the script */
                double dref, mtref = NAN;
                int raw = input_path == IN_RAW;
                int j = 0, ok;
                for (k = 0; k < script.n_pts; k++) {
                    if (kt && (script.ts[k] < tz || script.ts[k] > tz + YRT_S(TRACK_S))) continue;
                    cp[j][0] = sx + script.pts[k][0] - script.pts[0][0];   /* as stroke_step() does */
                    cp[j][1] = sy + script.pts[k][1] - script.pts[0][1];
                    if (!kt && isnan(mtref) && hypot(cp[j][0] - sx, cp[j][1] - sy) >= ONSET_PX)
                        mtref = YRT_TO_S(script.ts[script.n_pts - 1] - script.ts[k]);
                    j++;
                }
                resample(cp, j, cr, N_RS);
                dref = procrustes_ref(tgt, cr, N_RS);
                ok = n_samp == j && fabs(d - dref) < (kt ? 1e-6 : 1e-9);
                /* on the real clock (--auto) the final onset is not the planned one */
                if (kt) ok = ok && fabs(err - 5) < (raw ? 0.75 : sim ? 1e-3 : 0.05) && d < (raw ? 1e-3 : sim ? 1e-5 : 1e-4);
                else ok = ok && mt == mtref && fabs(pi - (1 - dref) / mtref) < 1e-9 &&
                          llabs(res.rt_ns - YRT_MS(400)) <= (sim ? 0 : YRT_MS(5));
                if (!kt && step >= 1 && step <= 3)   /* no wobble: similar, but for 8 ms chords and whole counts */
                    ok = ok && d < (raw ? 2e-3 : 1e-6) && fabs(scale - 0.93) < (raw ? 1e-2 : 5e-4) &&
                         fabs(frot + 5) < (raw ? 0.5 : 0.1);
                for (k = 0; input_path == IN_PEN && !kt && k < n_samp && k < script.n_pts; k++)
                    ok = ok && fabsf(samp[k].p - script.pr[k]) < 1e-6f;
                if (step == 0) ref_pre = (1 - dref) / mtref;
                if (step == 4) { sum_ref += (1 - dref) / mtref - ref_pre; n_ref++; }
                if (!ok) {
                    fprintf(stderr, "trial_copying: check: trial %d: %d samples (script %d), D %.9f (reference %.9f), "
                                    "MT %.9f (script %.9f)\n", ti.index, n_samp, j, d, dref, mt, mtref);
                    bad = 1;
                }
            }
            ytr_update(&trials, 1, NULL);
            rows++;
            in_trial = 0;
        }

        v = ytl_values(&tl);
        ygfx_apply(binds, (int)(sizeof binds / sizeof binds[0]), v);
        start_dot.x = (float)sx; start_dot.y = (float)sy;
        if (phase == P_TRACK) {
            double x, y;
            at(tgt, v[MOVE_P], &x, &y);
            red.x = ring.x = (float)x;
            red.y = ring.y = (float)y;
            blue.x = (float)(sx + px - live.rx);
            blue.y = (float)(sy + py - live.ry);
            if (ring.trim[1] < 0.002f) ring.visible = 0;   /* trim 0, 0 is the whole ring */
        }
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        t_draw = yrt_real_ns();
        ygfx_begin(&gfx, &f);
        ygfx_draw_n(&gfx, boxes, 2);
        if (v[DEMO_ON] >= 0.5f) poly_draw(&demo, v[DEMO_P] * demo.s[demo.n - 1]);
        ygfx_draw(&gfx, &start_dot);
        if (v[BOXES_ON] >= 0.5f) poly_draw(&ink, -1);
        ygfx_draw(&gfx, &ring);
        ygfx_draw(&gfx, &red);
        ygfx_draw(&gfx, &blue);
        if (ygfx_end(&gfx) < 0) { fprintf(stderr, "trial_copying: %s\n", ygfx_error(&gfx)); failed = 1; break; }
        t_draw = yrt_real_ns() - t_draw;
        if (t_draw > max_draw_ns) max_draw_ns = t_draw;
        yscr_flip(&scr);
    }
    /* the describe line again: its counters (key times, raw mice) are 0 at open */
    yscr_describe(&scr, line, sizeof line);
    fprintf(out, "# at the end: %s\n", line);
    fclose(out);
    fclose(sf);
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d trials written to %s, samples to %s%s\n", rows, out_path, samp_path, aborted ? " (stopped with Shift+Esc)" : "");
    printf("a frame's draws, begin to end, at most %.3f ms (real clock); %ld vblanks dropped\n", max_draw_ns / 1e6, dropped);
    if (n_learn) printf("mean learning effect PI(post) - PI(pre) over %d blocks: %.6f per s\n", n_learn, sum_learn / n_learn);
    if ((sim || autorun) && !failed && !aborted) {
        if (rows != 5 * blocks || n_learn != n_ref || !(sum_learn > 0) || fabs(sum_learn - sum_ref) > 1e-9 * n_ref) bad = 1;
        if (bad) { fprintf(stderr, "trial_copying: a check failed\n"); return 1; }
        printf("checks: every row as scripted; mean learning effect %.6f (script %.6f)\n", sum_learn / n_learn, sum_ref / n_ref);
    }
    return failed;
}
