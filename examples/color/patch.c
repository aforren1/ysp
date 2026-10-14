/* color_patch.c - a calibrated color patch: a color given in CIELAB, xyY
 * or DKL, shown through the display's calibration (ysp/color.h in
 * ysp/gfx.h's output stage), walked in hue, chroma and lightness by keys,
 * with the device RGB, the gamut distance and every refusal printed and
 * written per step.
 *
 * Mirrors PsychoPy's Coder demos hardware/monitorDemo.py (a monitor's
 * calibration loaded by name), stimuli/colors/colors.py and the Builder
 * demo Helper Tools/colors/ (type a color in a space, see it), and
 * hsvColorPalette.py as a picker in a calibrated space. Written from their
 * descriptions in docs/rig_spec.md 15; no PsychoPy code (GPL-3) was
 * copied.
 *
 * THE CALIBRATION. --cal FILE.yspcal loads a measured one (gfx_calib
 * writes one; docs/color.md). Without it the calibration is NOMINAL:
 * sRGB's primaries, D65 at 100 cd/m2, gamma 2.2, and Gaussian spectra
 * (for DKL), flagged in the describe line and the data file. A nominal
 * patch is not a statement about light. The linearization runs in
 * ysp/gfx.h's output stage; the OS gamma ramp is left alone
 * (desc.keep_os_gamma, also fullscreen), and the describe line says what
 * the ramp was. In a window run the program reads the output pixel back
 * after each change and checks it against ycol_output_code().
 *
 * THE WALK. A 300 px square on a gray background (linear device RGB 0.5,
 * the zero of DKL). Each space is walked in its polar form:
 *   space   hue (Left, Right)      chroma (Up, Down)     lightness (W, S)
 *   CIELAB  h of LCh, 10 deg       C*ab, 5               L*, 5
 *   xyY     angle about the white  distance from the     Y, 5 cd/m2
 *           point, 10 deg          white point, 0.01
 *   DKL     azimuth, 10 deg        contrast, 0.02        elevation, 10 deg
 * Tab converts the color to the next space (the same light) and walks
 * there. A color outside the gamut is shown brought inside (CIELAB: less
 * chroma at the same L* and h; xyY: toward the background; DKL: less of
 * the chromatic part) with a small cross above the patch, and the row says
 * how far outside and how much was kept. A step the calibration cannot
 * take is refused and the color stays: DKL with a calibration without
 * spectra, a lightness whose gray is outside. Esc or Shift+Esc ends it.
 *
 * Data (--out, ysp/trials.h's writer): one row per step: step, key,
 * space, v0, v1, v2 (the color as given), r, g, b (linear device RGB,
 * unclipped), in_gamut, distance, kept, shown_r, shown_g, shown_b, code_r,
 * code_g, code_b (the 8-bit codes the output stage writes), refused (the
 * reason, empty when shown; then the columns it did not reach are empty
 * too), onset (s from the first color's onset, both from flip records) and
 * onset_tier. FILE.meta has the calibration and the screen.
 *
 * --sim: the simulated display and a scripted walk of 53 keys through the
 * three spaces, out of the gamut and into a refusal (L* 105). It checks
 * every row (each step against the polar arithmetic, the round trip from
 * device RGB, a Tab's color against the one before, the gamut flags, the
 * shown color inside, the onset from a flip record within two frames of
 * the key) and exits 1 on a mismatch. 11 s on the Iris Xe laptop (MSVC).
 *
 * Usage: color_patch [--sim] [--auto] [--fullscreen] [--cal FILE] [--space lab|xyy|dkl]
 *                    [--color A B C] [--out FILE]
 *   --auto            the --sim walk and checks in a window, with the output
 *                     pixel read back after each change
 *   --space, --color  the first color (default lab 60 20 20)
 * Exit code: 0 (also after Esc), 1 when something did not open or a check
 * failed, 2 for a bad argument.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_SPACES   3
#define PATCH_PX   300
#define MAX_PEND   16
#define PI         3.14159265358979323846

static const int space_id[N_SPACES] = { YCOL_SPACE_CIELAB, YCOL_SPACE_XYY, YCOL_SPACE_DKL };
static const char* const space_name[N_SPACES] = { "lab", "xyy", "dkl" };
static const int map_method[N_SPACES] = { YCOL_MAP_CHROMA_CIELCH, YCOL_MAP_SCALE, YCOL_MAP_CHROMA_DKL };
static const double step_hue = 10, step_chroma[N_SPACES] = { 5, 0.01, 0.02 }, step_light[N_SPACES] = { 5, 5, 10 };

typedef struct row {
    int step, in_gamut, code_r, code_g, code_b, onset_tier;
    const char *key, *space, *refused;
    double v0, v1, v2, r, g, b, distance, kept, shown_r, shown_g, shown_b;
    int64_t onset_ns;
} row;
static const ytr_col cols[] = {
    YTR_COL(row, step, YTR_COL_INT), YTR_COL(row, key, YTR_COL_STRING), YTR_COL(row, space, YTR_COL_STRING),
    YTR_COL(row, v0, YTR_COL_DOUBLE), YTR_COL(row, v1, YTR_COL_DOUBLE), YTR_COL(row, v2, YTR_COL_DOUBLE),
    YTR_COL_D(row, r, YTR_COL_DOUBLE, 6), YTR_COL_D(row, g, YTR_COL_DOUBLE, 6), YTR_COL_D(row, b, YTR_COL_DOUBLE, 6),
    YTR_COL(row, in_gamut, YTR_COL_INT), YTR_COL_D(row, distance, YTR_COL_DOUBLE, 6), YTR_COL_D(row, kept, YTR_COL_DOUBLE, 6),
    YTR_COL_D(row, shown_r, YTR_COL_DOUBLE, 6), YTR_COL_D(row, shown_g, YTR_COL_DOUBLE, 6),
    YTR_COL_D(row, shown_b, YTR_COL_DOUBLE, 6), YTR_COL(row, code_r, YTR_COL_INT), YTR_COL(row, code_g, YTR_COL_INT),
    YTR_COL(row, code_b, YTR_COL_INT), YTR_COL(row, refused, YTR_COL_STRING), YTR_COL(row, onset_ns, YTR_COL_TIME_NS),
    YTR_COL(row, onset_tier, YTR_COL_INT),
};

static yscr_screen scr;
static ygfx_gfx gfx;
static ycol_cal cal;
static ycol_ctx cx;
static ytr_data data;
static double white_xy[2];

/* The current color, its space, and what is on screen. */
static int sp;
static ycol_color cur;
static ycol_rgb shown;
static int out_of_gamut;

/* --sim: the walk, one key every 0.2 s, from CIELAB 60 20 20. The
 * comments say what it reaches. */
static const char* const walk[] = {
    "right", "right", "up", "up", "up", "up", "up", "up", "up", "up", "up", /* hue, then chroma to the edge */
    "up", "down",                                                  /* out (C* 78), back in */
    "w", "w", "w", "w", "w", "w", "w", "w", "w",                   /* L* to 100, out; 105: refused */
    "s", "s", "s", "s", "s", "s", "s", "down", "down", "down", "down", "down", "down", /* L* 65, C* 41: in */
    "tab", "left", "up", "up", "w", "s",                           /* xyY */
    "tab", "right", "right", "up", "up", "up", "up", "up", "up", "w", "s", /* DKL, past its gamut */
    "tab",                                                         /* back to CIELAB */
};
#define N_WALK ((int)(sizeof walk / sizeof walk[0]))

static uint32_t scancode_of(const char* k) {
    if (!strcmp(k, "left")) return SDL_SCANCODE_LEFT;
    if (!strcmp(k, "right")) return SDL_SCANCODE_RIGHT;
    if (!strcmp(k, "up")) return SDL_SCANCODE_UP;
    if (!strcmp(k, "down")) return SDL_SCANCODE_DOWN;
    if (!strcmp(k, "w")) return SDL_SCANCODE_W;
    if (!strcmp(k, "s")) return SDL_SCANCODE_S;
    if (!strcmp(k, "tab")) return SDL_SCANCODE_TAB;
    if (!strcmp(k, "escape")) return SDL_SCANCODE_ESCAPE;
    return SDL_SCANCODE_UNKNOWN;
}
static const char* key_of(uint32_t sc) {
    static const char* const names[] = { "left", "right", "up", "down", "w", "s", "tab", "escape" };
    int i;
    for (i = 0; i < 8; i++) if (scancode_of(names[i]) == sc) return names[i];
    return NULL;
}

/* Gaussian primaries at 1 nm: a stand-in for measured spectra, so DKL has
 * cones to work on. Flagged NOMINAL with the chromaticities. */
static int nominal(void) {
    static const float xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };
    static float r[401], g[401], b[401];
    char err[200];
    int i;
    if (ycol_cal_nominal(&cal, xy, 100.0f, 2.2) < 0) return -1;
    for (i = 0; i < 401; i++) {
        double nm = 380 + i;
        r[i] = (float)(0.010 * exp(-0.5 * pow((nm - 612) / 14.0, 2)));
        g[i] = (float)(0.012 * exp(-0.5 * pow((nm - 545) / 24.0, 2)));
        b[i] = (float)(0.014 * exp(-0.5 * pow((nm - 455) / 12.0, 2)));
    }
    if (ycol_cal_set_spectra_nominal(&cal, 380, 1, 401, r, g, b, NULL) < 0) return -1;
    if (ycol_cal_derive(&cal, err, sizeof err) < 0) { fprintf(stderr, "color_patch: %s\n", err); return -1; }
    return 0;
}

static int load_cal(const char* path) {
    static unsigned char bytes[sizeof(ycol_cal)];
    char err[200];
    size_t n;
    FILE* fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "color_patch: cannot read %s\n", path); return -1; }
    n = fread(bytes, 1, sizeof bytes, fp);
    fclose(fp);
    if (ycol_cal_load(&cal, bytes, n, err, sizeof err) < 0) { fprintf(stderr, "color_patch: %s\n", err); return -1; }
    return 0;
}

static double wrap360(double h) { h = fmod(h, 360.0); return h < 0 ? h + 360.0 : h; }

/* A color as (lightness, chroma, hue) in its space's polar form, and back. */
static void to_polar(int s, ycol_color c, double p[3]) {
    if (s == 0) { p[0] = c.u.lab.L; p[1] = hypot(c.u.lab.a, c.u.lab.b); p[2] = wrap360(atan2(c.u.lab.b, c.u.lab.a) * 180 / PI); }
    else if (s == 1) {
        double dx = c.u.xyy.x - white_xy[0], dy = c.u.xyy.y - white_xy[1];
        p[0] = c.u.xyy.Y; p[1] = hypot(dx, dy); p[2] = wrap360(atan2(dy, dx) * 180 / PI);
    } else { p[0] = c.u.dkl.elev; p[1] = c.u.dkl.contrast; p[2] = wrap360(c.u.dkl.azim); }
}
static ycol_color from_polar(int s, const double p[3]) {
    double h = p[2] * PI / 180;
    if (s == 0) return ycol_make(YCOL_SPACE_CIELAB, p[0], p[1] * cos(h), p[1] * sin(h));
    if (s == 1) return ycol_make(YCOL_SPACE_XYY, white_xy[0] + p[1] * cos(h), white_xy[1] + p[1] * sin(h), p[0]);
    return ycol_make(YCOL_SPACE_DKL, p[0], p[2], p[1]);
}

/* The color c of space s as the patch shows it; NULL when shown, else
 * the reason it is refused. Fills the row's color columns. */
static const char* resolve(int s, ycol_color c, row* w, ycol_rgb* out, int* oog) {
    ycol_gamut g, gm;
    ycol_rgb v = ycol_to_rgb(&cx, c, &g);
    uint32_t code[3];
    memset(&gm, 0, sizeof gm);
    /* a refused step leaves what it did not reach empty in the row */
    w->r = w->g = w->b = w->distance = w->kept = w->shown_r = w->shown_g = w->shown_b = NAN;
    w->in_gamut = w->code_r = w->code_g = w->code_b = YTR_MISSING_INT;
    if (g.status < 0) return g.why;
    w->r = v.r; w->g = v.g; w->b = v.b;
    w->in_gamut = g.in;
    w->distance = g.distance;
    *oog = !g.in;
    if (!g.in) {
        v = ycol_map(&cx, c, map_method[s], &gm);
        if (gm.status < 0) return gm.why;
    }
    w->kept = g.in ? 1.0 : gm.kept;
    w->shown_r = v.r; w->shown_g = v.g; w->shown_b = v.b;
    ycol_output_code(&cal, v, 8, code);
    w->code_r = (int)code[0]; w->code_g = (int)code[1]; w->code_b = (int)code[2];
    *out = v;
    return NULL;
}

/* Two colors of space s are the same to 1e-6; a hue or an azimuth modulo
 * 360, and only where the chroma or contrast gives it a meaning. */
static int same_color(int s, ycol_color a, ycol_color b) {
    double p[3], q[3];
    to_polar(s, a, p);
    to_polar(s, b, q);
    return fabs(p[0] - q[0]) < 1e-6 && fabs(p[1] - q[1]) < 1e-6 &&
           (p[1] < 1e-6 || fabs(wrap360(p[2] - q[2] + 180) - 180) < 1e-6);
}

static void print_row(const row* w) {
    printf("%3d %-6s %-3s %9.4f %9.4f %9.4f -> ", w->step, w->key, w->space, w->v0, w->v1, w->v2);
    if (w->refused) { printf("REFUSED: %s\n", w->refused); return; }
    printf("rgb %.4f %.4f %.4f", w->r, w->g, w->b);
    if (w->in_gamut) printf(" in gamut");
    else printf(" OUT by %.4f, shown %.4f %.4f %.4f (%.3f kept)", w->distance, w->shown_r, w->shown_g, w->shown_b, w->kept);
    printf(", codes %d %d %d\n", w->code_r, w->code_g, w->code_b);
}

int main(int argc, char** argv) {
    static row rows[1024];
    static int64_t key_t[1024];
    yscr_frame f;
    ygfx_stim patch, cross;
    char line[1024], cline[1024], kline[512], meta[4096], meta_path[1100];
    const char *out_path = "color_patch.csv", *cal_path = NULL;
    double v0 = 60, v1 = 20, v2 = 20;
    static int64_t frame_of[1024];   /* the frame whose flip record is the row's onset; -1 when known */
    static int stalled[1024];        /* the host held the loop or the flip: no latency check */
    int64_t prev_vblank = -1;
    int i, sim = 0, scripted = 0, fullscreen = 0, aborted = 0, failed = 0, n_rows = 0, sim_next = 0, ending = 0, n_read = 0, bad_read = 0;
    int n_pend = 0, n_written = 0, readback = -1, sp_first;
    ycol_color first;
    int64_t t0 = 0, period = YRT_S(1.0 / 60);

    sp = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = scripted = 1;
        else if (!strcmp(argv[i], "--auto")) scripted = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--cal") && i + 1 < argc) cal_path = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--space") && i + 1 < argc) {
            const char* s = argv[++i];
            for (sp = 0; sp < N_SPACES && strcmp(s, space_name[sp]); sp++) {}
            if (sp == N_SPACES) { fprintf(stderr, "color_patch: --space takes lab, xyy or dkl\n"); return 2; }
        } else if (!strcmp(argv[i], "--color") && i + 3 < argc) {
            v0 = atof(argv[i + 1]); v1 = atof(argv[i + 2]); v2 = atof(argv[i + 3]);
            i += 3;
        } else {
            fprintf(stderr, "usage: color_patch [--sim] [--auto] [--fullscreen] [--cal FILE] [--space lab|xyy|dkl] [--color A B C] "
                            "[--out FILE]\n");
            return 2;
        }
    }

    /* the calibration, measured or nominal; the context about mid-gray */
    if (cal_path ? load_cal(cal_path) < 0 : nominal() < 0) return 1;
    {
        ycol_ctx_desc d;
        ycol_rgb wt = { 1, 1, 1 };
        ycol_color w;
        memset(&d, 0, sizeof d);
        d.cal = &cal;
        d.background[0] = d.background[1] = d.background[2] = 0.5;
        if (!ycol_ctx_open(&cx, &d)) { fprintf(stderr, "color_patch: %s\n", ycol_ctx_error(&cx)); return 1; }
        w = ycol_from_rgb(&cx, wt, YCOL_SPACE_XYY, NULL);
        white_xy[0] = w.u.xyy.x;
        white_xy[1] = w.u.xyy.y;
    }

    /* the display: the calibration in the output stage, the OS ramp alone */
    if (!ygfx_open_screen(&gfx, &scr, &(yscr_desc){ .backend = sim ? YSCR_BACKEND_SIM : 0, .windowed = !fullscreen,
                                                     .keep_os_gamma = true },
                          &(ygfx_desc){ .background = { 0.5f, 0.5f, 0.5f }, .cal = &cal })) {
        fprintf(stderr, "color_patch: %s\n", ygfx_error(&gfx));
        return 1;
    }
    patch = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_RECT, .w = PATCH_PX, .h = PATCH_PX });
    cross = ygfx_shape(&(ygfx_shape_desc){ .shape = YGFX_CROSS, .w = 24, .h = 24, .shape_p = { 4 }, .y = -PATCH_PX / 2 - 30,
                                           .edge = YGFX_EDGE_COSINE, .edge_width = 1, .color = { 0.05f, 0.05f, 0.05f } });

    ycol_cal_describe(&cal, kline, sizeof kline);
    ycol_ctx_describe(&cx, cline, sizeof cline);
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n%s\n%s\n", kline, cline, line);
    snprintf(meta, sizeof meta, "calibration=%s\ncolor=%s\nscreen=%s\nkeys=%s\n", kline, cline, line,
             scripted ? "scripted (--sim, --auto)" : "SDL through yscr_event_input");
    snprintf(meta_path, sizeof meta_path, "%s.meta", out_path);
    if (!ytr_data_open(&data, &(ytr_data_desc){ .path = out_path, .cols = cols, .n_cols = (int)(sizeof cols / sizeof cols[0]),
                                                .meta_path = meta_path, .meta = meta })) {
        fprintf(stderr, "color_patch: %s\n", ytr_data_error(&data));
        return 1;
    }

    /* the first color: refused is an error here, there is nothing to keep */
    cur = first = ycol_make(space_id[sp], v0, v1, v2);
    sp_first = sp;
    {
        row* w = &rows[0];
        memset(w, 0, sizeof *w);
        w->key = "start"; w->space = space_name[sp]; w->v0 = v0; w->v1 = v1; w->v2 = v2;
        w->refused = resolve(sp, cur, w, &shown, &out_of_gamut);
        print_row(w);
        if (w->refused) { fprintf(stderr, "color_patch: the first color is refused\n"); return 1; }
        n_rows = 1;
        frame_of[0] = 0;   /* the first frame shows it */
        n_pend = 1;
        key_t[0] = 0;
    }

    for (;;) {
        SDL_Event ev;
        yin_event in;
        int changed = 0, k, rc = yscr_begin(&scr, &f);
        if (rc == YSCR_QUIT) { aborted = 1; break; }
        if (rc != YSCR_OK) { fprintf(stderr, "color_patch: %s\n", yscr_error(&scr)); failed = 1; break; }
        if (!t0) t0 = f.onset;
        if (f.period > 0) period = f.period;

        /* the flip records of the frames that showed a change: the onsets;
         * rows go out in step order once each is complete */
        for (i = 0; i < f.n_done; i++)
            for (k = n_written; k < n_rows; k++)
                if (frame_of[k] == f.done[i].index && f.done[i].onset && !(f.done[i].flags & YSCR_FLIP_PENDING)) {
                    rows[k].onset_ns = f.done[i].onset;
                    rows[k].onset_tier = f.done[i].tier;
                    stalled[k] |= f.done[i].dropped > 0;
                    frame_of[k] = -1;
                    n_pend--;
                }
        for (; n_written < n_rows && frame_of[n_written] < 0; n_written++) {
            row out = rows[n_written];   /* onset in s from the first color's */
            if (out.onset_ns != YTR_MISSING_NS) out.onset_ns -= rows[0].onset_ns;
            if (ytr_data_write(&data, &out) < 0) { fprintf(stderr, "color_patch: %s\n", ytr_data_error(&data)); failed = 1; }
        }
        if (ending && !n_pend) break;

        /* keys: the window's, or the script's */
        while (!ending) {
            const char* key = NULL;
            int64_t kt = f.onset;
            if (yscr_poll(&scr, &ev, NULL)) {
                if (!yscr_event_input(&scr, &ev, &in) || in.kind != YIN_KIND_KEYBOARD || in.type != YIN_PRESS ||
                    (in.flags & YIN_REPEAT))
                    continue;
                key = key_of(in.control);
                kt = in.t;
                if (!key) continue;
            } else if (scripted && sim_next < N_WALK && f.onset >= t0 + YRT_MS(200) * (sim_next + 1)) {
                key = walk[sim_next++];
                kt = t0 + YRT_MS(200) * sim_next;
            } else if (scripted && sim_next == N_WALK && !n_pend) {
                key = "escape";
            } else break;
            if (!strcmp(key, "escape")) { ending = 1; break; }   /* after the last onset is in */
            if (n_rows >= 1024 || n_pend >= MAX_PEND) break;
            {
                row* w = &rows[n_rows];
                ycol_color next;
                ycol_rgb v;
                int s = sp, oog = 0;
                double p[3];
                memset(w, 0, sizeof *w);
                w->step = n_rows;
                w->key = key;
                if (!strcmp(key, "tab")) {
                    /* the same light in the next space that takes it */
                    for (s = (sp + 1) % N_SPACES; s != sp; s = (s + 1) % N_SPACES) {
                        ycol_gamut g;
                        if (ycol_convert(&cx, cur, space_id[s], &next, &g) == 0) break;
                        printf("    %s refused: %s\n", space_name[s], g.why ? g.why : "no reason");
                    }
                    if (s == sp) next = cur;
                } else {
                    to_polar(sp, cur, p);
                    if (!strcmp(key, "left") || !strcmp(key, "right")) p[2] = wrap360(p[2] + (key[0] == 'r' ? step_hue : -step_hue));
                    else if (!strcmp(key, "up")) p[1] += step_chroma[sp];
                    else if (!strcmp(key, "down")) p[1] = p[1] > step_chroma[sp] ? p[1] - step_chroma[sp] : 0;
                    else if (!strcmp(key, "w")) p[0] += step_light[sp];
                    else p[0] -= step_light[sp];
                    next = from_polar(sp, p);
                }
                w->space = space_name[s];
                w->v0 = next.u.v[0]; w->v1 = next.u.v[1]; w->v2 = next.u.v[2];
                w->refused = resolve(s, next, w, &v, &oog);
                print_row(w);
                if (!w->refused) {
                    sp = s;
                    cur = next;
                    shown = v;
                    out_of_gamut = oog;
                    changed = 1;
                    frame_of[n_rows] = f.index;
                    stalled[n_rows] = prev_vblank >= 0 && f.vblank - prev_vblank > 1;
                    n_pend++;
                } else {
                    w->onset_ns = YTR_MISSING_NS;
                    w->onset_tier = YTR_MISSING_INT;
                    frame_of[n_rows] = -1;
                }
                key_t[n_rows] = kt;
                n_rows++;
            }
        }
        if (f.index == 0) changed = 1;

        ycol_store3f(patch.color, shown);
        cross.visible = out_of_gamut ? 1.0f : 0.0f;
        yscr_mark(&scr, YSCR_PHASE_EVALUATE);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &patch);
        ygfx_draw(&gfx, &cross);
        ygfx_end(&gfx);
        if (changed) readback = n_rows - 1;
        /* a window run: the pixel the output stage wrote, against the codes */
        if (!sim && readback >= 0) {
            uint8_t px[4];
            float cxp, cyp;
            const row* w = &rows[readback];
            ygfx_center(&gfx, &cxp, &cyp);
            if (ygfx_read_output(&gfx, (int)cxp, (int)cyp, 1, 1, px) >= 0) {
                n_read++;
                if (px[0] != w->code_r || px[1] != w->code_g || px[2] != w->code_b) {
                    fprintf(stderr, "color_patch: step %d: the output pixel is %u %u %u, the codes %d %d %d\n", w->step, px[0],
                            px[1], px[2], w->code_r, w->code_g, w->code_b);
                    bad_read++;
                }
            }
            readback = -1;
        }
        yscr_flip(&scr);
        prev_vblank = f.vblank;
    }
    if (ytr_data_finish(&data) < 0) { fprintf(stderr, "color_patch: %s\n", ytr_data_error(&data)); failed = 1; }
    yscr_describe(&scr, line, sizeof line);
    ygfx_close(&gfx);
    yscr_close(&scr);
    printf("%d steps written to %s%s\n", n_rows, out_path, aborted ? " (stopped with Shift+Esc)" : "");
    if (!sim) {
        printf("output read back after %d changes: %d differ from ycol_output_code()\n", n_read, bad_read);
        if (bad_read) failed = 1;
    }

    if (scripted && !failed && !aborted) {
        /* every step against the arithmetic it claims; a failed part is
         * named: step, round trip, tab, gamut, shown, onset */
        static const char* const part[6] = { "step", "round trip", "tab", "gamut", "shown", "onset" };
        int bad = 0, n_out = 0, n_refused = 0, n_late = 0, s_prev = sp_first, k, j;
        ycol_color prev = first;
        if (n_rows != N_WALK + 1) { fprintf(stderr, "color_patch: --sim: %d steps, not %d\n", n_rows, N_WALK + 1); bad = 1; }
        for (k = 0; k < n_rows && k <= N_WALK; k++) {
            const row* w = &rows[k];
            int s = !strcmp(w->space, "lab") ? 0 : !strcmp(w->space, "xyy") ? 1 : 2, off = 0;
            ycol_color c = ycol_make(space_id[s], w->v0, w->v1, w->v2), back;
            double p[3], q[3], e = YCOL_GAMUT_EPS;
            if (k > 0 && strcmp(w->key, "tab")) {   /* one polar step from the color before */
                to_polar(s_prev, prev, p);
                to_polar(s, c, q);
                if (!strcmp(w->key, "left") || !strcmp(w->key, "right"))
                    off |= (fabs(wrap360(q[2] - p[2] + 180) - 180 - (w->key[0] == 'r' ? step_hue : -step_hue)) > 1e-9) << 0;
                else if (!strcmp(w->key, "up") || !strcmp(w->key, "down"))
                    off |= (fabs(q[1] - p[1] - (w->key[0] == 'u' ? step_chroma[s] : -step_chroma[s])) > 1e-9) << 0;
                else off |= (fabs(q[0] - p[0] - (w->key[0] == 'w' ? step_light[s] : -step_light[s])) > 1e-9) << 0;
            }
            if (!w->refused) {
                ycol_rgb v = { w->r, w->g, w->b };
                int in = v.r >= -e && v.r <= 1 + e && v.g >= -e && v.g <= 1 + e && v.b >= -e && v.b <= 1 + e;
                back = ycol_from_rgb(&cx, v, space_id[s], NULL);   /* the device RGB names the same color */
                off |= !same_color(s, back, c) << 1;
                if (k > 0 && !strcmp(w->key, "tab")) {   /* the same light as the color before */
                    ycol_rgb u = ycol_to_rgb(&cx, prev, NULL);
                    off |= (fabs(u.r - v.r) > 1e-9 || fabs(u.g - v.g) > 1e-9 || fabs(u.b - v.b) > 1e-9) << 2;
                }
                off |= (in != w->in_gamut || (in ? w->kept != 1 || w->shown_r != w->r : !(w->kept >= 0 && w->kept < 1))) << 3;
                off |= !(w->shown_r >= -e && w->shown_r <= 1 + e && w->shown_g >= -e && w->shown_g <= 1 + e &&
                         w->shown_b >= -e && w->shown_b <= 1 + e) << 4;
                /* the change on the key's frame: within two periods of it on
                 * the simulated grid, four in a window that a compositor
                 * runs; not checked where the host held the loop (a vblank
                 * skipped) or the flip (dropped), which is counted */
                n_late += k > 0 && stalled[k];
                off |= !((sim ? w->onset_tier == YSCR_TIER_SIM : w->onset_tier > 0) && w->onset_ns > 0 &&
                         (k == 0 || (w->onset_ns >= key_t[k] &&
                                     (stalled[k] || w->onset_ns - key_t[k] <= (sim ? 2 : 4) * period)))) << 5;
                n_out += !in;
                prev = c;
                s_prev = s;
            } else n_refused++;
            if (off) {
                fprintf(stderr, "color_patch: --sim: step %d (%s) is off:", k, w->key);
                for (j = 0; j < 6; j++) if (off >> j & 1) fprintf(stderr, " %s", part[j]);
                if (off >> 5 & 1) fprintf(stderr, " (tier %d, %.6f s after the key)", w->onset_tier, YRT_TO_S(w->onset_ns - key_t[k]));
                fprintf(stderr, "\n");
                print_row(w);
                bad = 1;
            }
        }
        printf("--sim: %d steps, %d outside the gamut, %d refused; %d shown late by the host\n", n_rows, n_out, n_refused,
               n_late);
        if (n_out < 2 || n_refused < 1) { fprintf(stderr, "color_patch: --sim: the walk did not leave the gamut or meet a refusal\n"); bad = 1; }
        if (bad) failed = 1;
        else printf("--sim: every step came out as computed\n");
    }
    return failed;
}
