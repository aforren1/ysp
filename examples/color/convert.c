/* color_convert.c - ysp/color.h on a calibration: conversions, gamut
 * questions and gamut mapping, printed.
 *
 * Usage:
 *   color_convert                 a nominal display: sRGB's primaries, D65 at
 *                                 100 cd/m2, gamma 2.2, and Gaussian spectra
 *                                 flagged NOMINAL (not a measurement)
 *   color_convert FILE.yspcal     a calibration file (gfx_calib writes one)
 * Prints the context's describe line; sRGB colors as device RGB with their
 * gamut, in CIELAB and OkLCh; the largest DKL contrast at eight azimuths
 * about mid-gray and a direction for a 5 percent L-M grating; one color
 * outside the gamut brought inside by each mapping method. Without spectra
 * the cone spaces are refused, and it says why.
 * Exit code: 0, 1 when the file cannot be read or is refused, 2 for a bad
 * argument.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_COLOR_IMPLEMENTATION
#include "ysp/color.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static ycol_cal cal;
static unsigned char bytes[sizeof(ycol_cal)];

/* Gaussian primaries at 1 nm: a stand-in for measured spectra, so the cone
 * spaces have something to work on. Flagged NOMINAL. */
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
    if (ycol_cal_derive(&cal, err, sizeof err) < 0) { fprintf(stderr, "color_convert: %s\n", err); return -1; }
    return 0;
}

static double min3(const double* v) { return v[0] < v[1] ? (v[0] < v[2] ? v[0] : v[2]) : (v[1] < v[2] ? v[1] : v[2]); }

static void show_gamut(const ycol_gamut* g) {
    if (g->status < 0) printf("refused: %s", g->why);
    else if (g->in && min3(g->margin) >= 0) printf("in gamut, margin %.4f", min3(g->margin));
    else if (g->in) printf("in gamut (%.2g outside the cube, within the tolerance)", g->distance);
    else printf("OUT by %.2g; %.6f of the offset from the background fits", g->distance, g->scale);
}

int main(int argc, char** argv) {
    static const char* hexes[] = { "#3366cc", "#ff8800", "#808080", "#00ff00", "#fedcba" };
    static const char* mname[] = { "", "SCALE", "CHROMA_OKLCH", "CHROMA_CIELCH", "CHROMA_DKL", "CLIP" };
    ycol_ctx cx;
    ycol_ctx_desc d;
    ycol_gamut g;
    char err[200], line[1024];
    int i, m;
    if (argc > 2) { fprintf(stderr, "usage: color_convert [FILE.yspcal]\n"); return 2; }
    if (argc == 2) {
        FILE* fp = fopen(argv[1], "rb");
        size_t n;
        if (!fp) { fprintf(stderr, "color_convert: cannot open %s\n", argv[1]); return 1; }
        n = fread(bytes, 1, sizeof bytes, fp);
        fclose(fp);
        if (ycol_cal_load(&cal, bytes, n, err, sizeof err) < 0) { fprintf(stderr, "color_convert: %s\n", err); return 1; }
    } else if (nominal() < 0) {
        return 1;
    }
    memset(&d, 0, sizeof d);
    d.cal = &cal;
    d.background[0] = d.background[1] = d.background[2] = 0.5;
    if (!ycol_ctx_open(&cx, &d)) { fprintf(stderr, "color_convert: %s\n", ycol_ctx_error(&cx)); return 1; }
    ycol_cal_describe(&cal, line, sizeof line);
    printf("%s\n", line);
    ycol_ctx_describe(&cx, line, sizeof line);
    printf("%s\n\n", line);

    printf("sRGB colors on this display (relative to its white):\n");
    for (i = 0; i < 5; i++) {
        ycol_color c = ycol_hex(hexes[i]), lab, lch;
        ycol_rgb r = ycol_to_rgb(&cx, c, &g);
        printf("  %s -> rgb %.4f %.4f %.4f, ", hexes[i], r.r, r.g, r.b);
        show_gamut(&g);
        if (ycol_convert(&cx, c, YCOL_SPACE_CIELAB, &lab, NULL) == 0 && ycol_convert(&cx, c, YCOL_SPACE_OKLCH, &lch, NULL) == 0)
            printf("\n      CIELAB %.2f %.2f %.2f, OkLCh %.4f %.4f %.1f", lab.u.lab.L, lab.u.lab.a, lab.u.lab.b, lch.u.lch.L,
                   lch.u.lch.C, lch.u.lch.h);
        printf("\n");
    }

    printf("\nDKL about mid-gray, elevation 0: the largest contrast a grating can swing\n");
    for (i = 0; i < 8; i++) {
        double k = ycol_max_scale(&cx, ycol_make(YCOL_SPACE_DKL, 0, 45.0 * i, 1), YCOL_SYMMETRIC);
        if (k < 0) { printf("  refused: %s\n", cx.why_no_bg ? cx.why_no_bg : ycol_strerror((int)k)); break; }
        printf("  azimuth %5.1f: %.4f\n", 45.0 * i, k);
    }
    {
        ycol_rgb dir = ycol_to_dir(&cx, YCOL_DKL(.azim = 0, .contrast = 0.05), &g);
        if (g.status == 0) {
            printf("  a 5%% L-M grating: dir %.5f %.5f %.5f, ", dir.r, dir.g, dir.b);
            show_gamut(&g);
            printf("\n");
        }
    }

    printf("\nOkLCh 0.70 0.30 150 brought inside, by method:\n");
    for (m = YCOL_MAP_SCALE; m <= YCOL_MAP_CLIP; m++) {
        ycol_rgb r = ycol_map(&cx, YCOL_OKLCH(.L = 0.70, .C = 0.30, .h = 150), m, &g);
        printf("  %-13s ", mname[m]);
        if (g.status < 0) printf("refused: %s\n", g.why);
        else if (g.kept == g.kept) printf("rgb %.4f %.4f %.4f, kept %.4f\n", r.r, r.g, r.b, g.kept);
        else printf("rgb %.4f %.4f %.4f (each channel clamped)\n", r.r, r.g, r.b);
    }
    return 0;
}
