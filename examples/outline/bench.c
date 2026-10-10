/* outline_bench.c - the cost of ysp/outline.h's builds on this machine.
 *
 * The outline builder runs in the pack tool and, for glyphs a pack lacks,
 * in the player between frames. This program times the rows of
 * docs/outline.md against their bars, on the fonts it finds:
 *   set      a whole font into a curve set, resolved (the default) and with
 *            keep_overlaps
 *   curves   curves per glyph of a CFF font at two cubic tolerances
 *   alpha    exact alpha: 3000 CJK glyphs at 32 px; the same at 24, 32 and
 *            48 px, resolved once; every Latin glyph at 24 px
 *   stroke   every glyph of a Latin and a CJK font outlined at 0.08 em,
 *            round joins, resolve included
 * Rows are interleaved: each round runs every row once, and each row
 * prints the median of its rounds. Times are ysp/rt.h clock reads.
 *
 * Build (from the repository root):
 *     cc -O2 -Iinclude -o outline_bench examples/outline/bench.c -lm
 *     cl /O2 /Iinclude examples\outline\bench.c
 *
 * Usage: outline_bench [rounds] [font directory]
 *        (default 3 rounds and C:/Windows/Fonts)
 * Exit code: 0; 1 if a build failed; 2 for a bad argument; 3 when none of
 * the fonts is in the directory (CI runs it that way).
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YRT_NO_THREADS
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"

#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROUNDS 15
#define NFONTS 5

static yol_ctx g_cx;
static int g_fail;

typedef struct font { const char* name; char path[512]; uint8_t* data; size_t n; yol_font f; int ok; } font;

static uint8_t* read_file(const char* path, size_t* n) {
    FILE* fp = fopen(path, "rb");
    uint8_t* d;
    long l;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); l = ftell(fp); fseek(fp, 0, SEEK_SET);
    d = l > 0 ? (uint8_t*)malloc((size_t)l) : NULL;
    *n = d ? fread(d, 1, (size_t)l, fp) : 0;
    fclose(fp);
    return d;
}

static double now_s(void) { return (double)yrt_now_ns() * 1e-9; }
static int cmp_d(const void* a, const void* b) { double p = *(const double*)a, q = *(const double*)b; return p < q ? -1 : p > q; }
static double median(double* v, int n) { qsort(v, (size_t)n, sizeof(double), cmp_d); return n ? v[n / 2] : 0; }

/* A whole font into a set: seconds. */
static double row_set(font* ft, int keep) {
    yol_cset s;
    yol_cset_desc d;
    double t0, t;
    memset(&d, 0, sizeof d);
    d.n_glyphs = (uint32_t)ft->f.n_glyphs; d.keep_overlaps = keep != 0;
    if (!yol_cset_open(&s, &g_cx, &d)) { g_fail = 1; return 0; }
    t0 = now_s();
    if (yol_cset_add_font(&s, &ft->f, NULL, 0, 0) < 0) { fprintf(stderr, "%s: %s\n", ft->name, yol_error(&g_cx)); g_fail = 1; }
    t = now_s() - t0;
    yol_cset_close(&s);
    return t;
}

/* Curves per glyph after the cubic conversion. */
static double curves_per_glyph(font* ft, double tol) {
    yol_path p;
    yol_glyph_desc gd;
    uint64_t n = 0;
    int g, ng = 0;
    memset(&gd, 0, sizeof gd);
    gd.tol = tol;
    yol_path_init(&p, &g_cx);
    for (g = 0; g < ft->f.n_glyphs; g++) {
        yol_path_clear(&p);
        if (yol_font_glyph(&g_cx, &ft->f, (uint32_t)g, &p, &gd) < 0) continue;
        n += (uint64_t)((p.n_pts - p.n_contours) / 2);
        ng++;
    }
    yol_path_free(&p);
    return ng ? (double)n / ng : 0;
}

/* Exact alpha for `count` glyphs, every `step`, at the sizes; with `once`
 * one resolve per glyph, else yol_raster() resolves on each call. */
static double row_alpha(font* ft, int step, int count, const double* sizes, int nsizes, int once, int* done_out) {
    static uint8_t img[512 * 512];
    yol_path p, r;
    double t0, t;
    int g, k, done = 0;
    yol_path_init(&p, &g_cx); yol_path_init(&r, &g_cx);
    t0 = now_s();
    for (g = 0; g < ft->f.n_glyphs && done < count; g += step) {
        const yol_path* src = &p;
        yol_path_clear(&p);
        if (yol_font_glyph(&g_cx, &ft->f, (uint32_t)g, &p, NULL) < 0) continue;
        if (once) { if (yol_resolve(&g_cx, &p, &r) < 0) { g_fail = 1; continue; } src = &r; }
        for (k = 0; k < nsizes; k++) {
            yol_raster_desc rd;
            yol_box b;
            memset(&rd, 0, sizeof rd);
            rd.scale = sizes[k];
            if (yol_raster(&g_cx, src, &rd, &b) < 0) { g_fail = 1; continue; }
            if (b.x1 - b.x0 > 512 || b.y1 - b.y0 > 512) continue;
            rd.x = -b.x0; rd.y = -b.y0; rd.out = img; rd.w = b.x1 - b.x0; rd.h = b.y1 - b.y0; rd.stride = rd.w;
            if (yol_raster(&g_cx, src, &rd, NULL) < 0) g_fail = 1;
        }
        done++;
    }
    t = now_s() - t0;
    yol_path_free(&p); yol_path_free(&r);
    *done_out = done;
    return t;
}

/* Every glyph outlined at 0.08 em: seconds; *worst gets the slowest glyph. */
static double row_stroke(font* ft, double* worst, int* worst_g) {
    yol_path p, o;
    yol_stroke_desc sd;
    double t0, t;
    int g;
    memset(&sd, 0, sizeof sd);
    sd.width = 0.08;
    yol_path_init(&p, &g_cx); yol_path_init(&o, &g_cx);
    *worst = 0;
    t0 = now_s();
    for (g = 0; g < ft->f.n_glyphs; g++) {
        double a = now_s(), b;
        yol_path_clear(&p);
        if (yol_font_glyph(&g_cx, &ft->f, (uint32_t)g, &p, NULL) < 0) continue;
        if (yol_stroke(&g_cx, &p, &o, &sd) < 0) { fprintf(stderr, "%s glyph %d: %s\n", ft->name, g, yol_error(&g_cx)); g_fail = 1; }
        b = now_s() - a;
        if (b > *worst) { *worst = b; *worst_g = g; }
    }
    t = now_s() - t0;
    yol_path_free(&p); yol_path_free(&o);
    return t;
}

int main(int argc, char** argv) {
    static const char* names[NFONTS] = { "segoeui.ttf", "bahnschrift.ttf", "Nirmala.ttc", "msyh.ttc", "SourceHanSansJP-Normal.otf" };
    static double t_set[NFONTS][2][MAX_ROUNDS], t_alpha[3][MAX_ROUNDS], t_stroke[2][MAX_ROUNDS];
    font fonts[NFONTS];
    const char* dir = argc > 2 ? argv[2] : "C:/Windows/Fonts";
    int rounds = argc > 1 ? atoi(argv[1]) : 3, i, r, any = 0, na[3] = { 0, 0, 0 }, swg[2] = { 0, 0 };
    double sw[2] = { 0, 0 };
    font *latin = NULL, *cjk = NULL, *cff = NULL;
    if (rounds < 1 || rounds > MAX_ROUNDS) { fprintf(stderr, "usage: outline_bench [rounds 1..%d] [font directory]\n", MAX_ROUNDS); return 2; }
    yol_open(&g_cx, NULL);
    for (i = 0; i < NFONTS; i++) {
        fonts[i].name = names[i];
        snprintf(fonts[i].path, sizeof fonts[i].path, "%s/%s", dir, names[i]);
        fonts[i].data = read_file(fonts[i].path, &fonts[i].n);
        fonts[i].ok = fonts[i].data && yol_font_view(&fonts[i].f, fonts[i].data, fonts[i].n, 0, NULL, 0) == YOL_OK;
        any |= fonts[i].ok;
    }
    if (!any) { printf("outline_bench: none of the fonts is in %s\n", dir); return 3; }
    if (fonts[0].ok) latin = &fonts[0];
    if (fonts[3].ok) cjk = &fonts[3];
    if (fonts[4].ok) cff = &fonts[4];
    for (r = 0; r < rounds; r++) {
        static const double one[1] = { 32 }, three[3] = { 24, 32, 48 }, l24[1] = { 24 };
        for (i = 0; i < NFONTS; i++) if (fonts[i].ok) {
            t_set[i][0][r] = row_set(&fonts[i], 0);
            t_set[i][1][r] = row_set(&fonts[i], 1);
        }
        if (cjk) { t_alpha[0][r] = row_alpha(cjk, 10, 3000, one, 1, 0, &na[0]); t_alpha[1][r] = row_alpha(cjk, 10, 3000, three, 3, 1, &na[1]); }
        if (latin) t_alpha[2][r] = row_alpha(latin, 1, 1 << 30, l24, 1, 0, &na[2]);
        if (latin) { double w; int wg = 0; t_stroke[0][r] = row_stroke(latin, &w, &wg); if (w > sw[0]) { sw[0] = w; swg[0] = wg; } }
        if (cjk) { double w; int wg = 0; t_stroke[1][r] = row_stroke(cjk, &w, &wg); if (w > sw[1]) { sw[1] = w; swg[1] = wg; } }
    }
    printf("outline_bench: %d rounds, medians\n", rounds);
    printf("%-28s %7s %10s %10s %10s %10s\n", "set (whole font)", "glyphs", "resolve s", "us/glyph", "keep s", "us/glyph");
    for (i = 0; i < NFONTS; i++) if (fonts[i].ok) {
        double a = median(t_set[i][0], rounds), b = median(t_set[i][1], rounds);
        printf("%-28s %7d %10.3f %10.1f %10.3f %10.1f\n", fonts[i].name, fonts[i].f.n_glyphs, a, 1e6 * a / fonts[i].f.n_glyphs, b, 1e6 * b / fonts[i].f.n_glyphs);
    }
    if (cff) printf("curves per glyph, %s: tol 1e-4 em %.2f, tol 1e-5 em %.2f\n", cff->name, curves_per_glyph(cff, 1e-4), curves_per_glyph(cff, 1e-5));
    if (cjk) {
        printf("alpha, %d %s glyphs at 32 px: %.3f s (bar 1 s)\n", na[0], cjk->name, median(t_alpha[0], rounds));
        printf("alpha, the same at 24, 32 and 48 px, resolved once: %.3f s (bar 2 s)\n", median(t_alpha[1], rounds));
    }
    if (latin) printf("alpha, every %s glyph at 24 px: %.1f us/glyph (bar 50)\n", latin->name, 1e6 * median(t_alpha[2], rounds) / na[2]);
    if (latin) printf("stroke 0.08 em, %s: %.2f s for the font, %.0f us/glyph (bar 200), slowest %.1f ms (glyph %d)\n", latin->name,
                      median(t_stroke[0], rounds), 1e6 * median(t_stroke[0], rounds) / latin->f.n_glyphs, 1e3 * sw[0], swg[0]);
    if (cjk) printf("stroke 0.08 em, %s: %.2f s for the font, %.0f us/glyph (bar 1000), slowest %.1f ms (glyph %d)\n", cjk->name,
                    median(t_stroke[1], rounds), 1e6 * median(t_stroke[1], rounds) / cjk->f.n_glyphs, 1e3 * sw[1], swg[1]);
    for (i = 0; i < NFONTS; i++) free(fonts[i].data);
    yol_close(&g_cx);
    return g_fail ? 1 : 0;
}
