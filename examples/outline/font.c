/* outline_font.c - a Latin label from a font file: exact alpha and a curve set.
 *
 * Shows ysp/outline.h without a pack: open a font, place the glyphs of a
 * short ASCII label by their advances, write the label's exact coverage
 * at a pixel size as an 8-bit PGM, and build the curve set ysp/gfx.h would
 * draw (the glyphs, resolved, in em units).
 *
 * The layout here is advances only: no shaping, no kerning, no bidi, no
 * line breaking. That is enough for a Latin label and wrong for most
 * scripts. Real text goes through Skribidi in the pack tool and the player
 * (rig_spec 5.2), which gives glyph indices and positions to ysp/outline.h.
 *
 * Build (from the repository root):
 *     cc -O2 -Iinclude -o outline_font examples/outline/font.c -lm
 *     cl /O2 /Iinclude examples\outline\font.c
 *
 * Usage: outline_font font.ttf "label" [px] [out.pgm]
 *        (px default 32, out default label.pgm)
 * Exit code: 0; 1 if the font does not open or a build fails; 2 for
 * missing or bad arguments.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(int argc, char** argv) {
    const char* label;
    const char* out = argc > 4 ? argv[4] : "label.pgm";
    double px = argc > 3 ? atof(argv[3]) : 32, pen = 0;
    size_t n, i;
    uint8_t *bytes, *img;
    char err[200];
    yol_ctx cx;
    yol_font font;
    yol_path label_path, glyph;
    yol_cset set;
    yol_cset_desc cd;
    yol_raster_desc rd;
    yol_box box;
    int w, h, rc = 0;
    FILE* fp;
    if (argc < 3 || !(px > 0 && px < 2000)) {
        fprintf(stderr, "usage: outline_font font.ttf \"label\" [px] [out.pgm]\n");
        return 2;
    }
    label = argv[2];
    bytes = read_file(argv[1], &n);
    if (!bytes) { fprintf(stderr, "outline_font: cannot read %s\n", argv[1]); return 1; }
    if (yol_font_view(&font, bytes, n, 0, err, sizeof err) < 0) { fprintf(stderr, "outline_font: %s\n", err); return 1; }
    yol_open(&cx, NULL);
    yol_path_init(&label_path, &cx);
    yol_path_init(&glyph, &cx);
    memset(&cd, 0, sizeof cd);
    cd.n_glyphs = (uint32_t)font.n_glyphs;
    if (!yol_cset_open(&set, &cx, &cd)) { fprintf(stderr, "outline_font: %s\n", yol_error(&cx)); return 1; }
    /* the label: each glyph at the pen, in px, the baseline at y = 0 */
    for (i = 0; label[i]; i++) {
        uint32_t g = yol_font_glyph_index(&font, (unsigned char)label[i]);
        yol_glyph_desc gd;
        double adv = 0;
        memset(&gd, 0, sizeof gd);
        gd.size = px; gd.x = pen;
        if (yol_font_glyph(&cx, &font, g, &label_path, &gd) < 0) { fprintf(stderr, "outline_font: %s\n", yol_error(&cx)); rc = 1; }
        yol_font_hmetrics(&font, g, &adv, NULL);
        pen += adv * px;
        /* the curve set holds each glyph once, in em, for ysp/gfx.h's runs */
        if (set.words[8 + g] == 0) {
            yol_path_clear(&glyph);
            if (yol_font_glyph(&cx, &font, g, &glyph, NULL) < 0 || yol_cset_add(&set, g, &glyph) < 0) {
                fprintf(stderr, "outline_font: glyph %u: %s\n", g, yol_error(&cx));
                rc = 1;
            }
        }
    }
    /* the exact coverage, on the pixel grid */
    memset(&rd, 0, sizeof rd);
    rd.scale = 1;
    if (yol_raster(&cx, &label_path, &rd, &box) < 0) { fprintf(stderr, "outline_font: %s\n", yol_error(&cx)); return 1; }
    w = box.x1 - box.x0; h = box.y1 - box.y0;
    if (w <= 0 || h <= 0) { fprintf(stderr, "outline_font: the label has no outline\n"); return 1; }
    img = (uint8_t*)malloc((size_t)w * (size_t)h);
    rd.x = -box.x0; rd.y = -box.y0; rd.out = img; rd.w = w; rd.h = h; rd.stride = w;
    if (!img || yol_raster(&cx, &label_path, &rd, NULL) < 0) { fprintf(stderr, "outline_font: %s\n", yol_error(&cx)); return 1; }
    fp = fopen(out, "wb");
    if (!fp) { fprintf(stderr, "outline_font: cannot write %s\n", out); return 1; }
    fprintf(fp, "P5\n%d %d\n255\n", w, h);
    fwrite(img, 1, (size_t)w * (size_t)h, fp);
    fclose(fp);
    printf("%s: %d x %d px, exact coverage; curve set of %u texels and %u words\n", out, w, h, set.n_texels, set.n_words);
    free(img);
    yol_cset_close(&set);
    yol_path_free(&label_path);
    yol_path_free(&glyph);
    yol_close(&cx);
    free(bytes);
    return rc;
}
