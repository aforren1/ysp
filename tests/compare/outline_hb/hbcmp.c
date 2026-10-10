/* Compares ysp/outline.h with HarfBuzz for every glyph of one font face:
 * yol_font_glyph() against HarfBuzz's outline (hbdump.py) put through the
 * same path builder, and a curve set from yol_cset_add_font() against one
 * from yol_cset_add() on HarfBuzz's outlines. A local check, not CI: it
 * needs a font and uharfbuzz (README.md).
 *
 *     hbcmp FONT DUMP.txt [FACE [GLYPH]]
 *
 * GLYPH prints both outlines of that glyph, in font units. Exit 0 when every
 * glyph and both sets are equal, 1 when not, 2 on a usage or file error. */
#define _CRT_SECURE_NO_WARNINGS
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "segcmp.h"

static unsigned char* slurp(const char* fn, size_t* n) {
    FILE* fp = fopen(fn, "rb");
    long m;
    unsigned char* b;
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0 || (m = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    b = (unsigned char*)malloc((size_t)m + 1);
    if (!b) { fclose(fp); return NULL; }
    *n = fread(b, 1, (size_t)m, fp);
    b[*n] = 0;
    fclose(fp);
    return b;
}

static void show(const yol_path* p, const char* name, int upem) {
    int k;
    printf("%s, %d contours:", name, p->n_contours);
    for (k = 0; k < p->n_contours; k++) printf(" %u", (unsigned)p->contours[k]);
    printf("\n");
    for (k = 0; k < p->n_pts; k++) printf("  %4d %12.4f %12.4f\n", k, p->pts[2 * k] * upem, -p->pts[2 * k + 1] * upem);
}

int main(int argc, char** argv) {
    size_t nf = 0, nt = 0;
    unsigned char* fd;
    char *tx, *q, err[256];
    yol_ctx c;
    yol_font f;
    yol_path a, b;
    yol_cset sa, sb;
    yol_cset_desc cd;
    int upem, ng, face = 0, show_g = -1, g = -1, eq = 0, ne = 0, shown = 0, sets_eq, rc = 0;
    double maxd = 0, inv;
    if (argc < 3) { fprintf(stderr, "usage: hbcmp FONT DUMP.txt [FACE [GLYPH]]\n"); return 2; }
    if (argc > 3) face = atoi(argv[3]);
    if (argc > 4) show_g = atoi(argv[4]);
    fd = slurp(argv[1], &nf);
    tx = (char*)slurp(argv[2], &nt);
    if (!fd || !tx) { fprintf(stderr, "hbcmp: cannot read %s\n", fd ? argv[2] : argv[1]); return 2; }
    yol_open(&c, NULL);
    if (yol_font_view(&f, fd, nf, face, err, sizeof err) != YOL_OK) { fprintf(stderr, "hbcmp: %s\n", err); return 2; }
    if (sscanf(tx, "U %d %d", &upem, &ng) != 2 || upem != f.units_per_em || ng != f.n_glyphs) {
        fprintf(stderr, "hbcmp: the dump is not of this font face\n");
        return 2;
    }
    inv = 1.0 / upem;
    memset(&cd, 0, sizeof cd);
    cd.n_glyphs = (uint32_t)ng;
    yol_cset_open(&sa, &c, &cd);
    yol_cset_open(&sb, &c, &cd);
    if (yol_cset_add_font(&sa, &f, NULL, 0, 0) != YOL_OK) { fprintf(stderr, "hbcmp: set: %s\n", yol_error(&c)); return 1; }
    yol_path_init(&a, &c);
    yol_path_init(&b, &c);
    for (q = strchr(tx, '\n'); q; q = strchr(q, '\n')) {
        char op = *++q;
        double v[6];
        if (op == 'G' || op == 'E' || op == 0) {
            if (g >= 0) {
                yol_path_end(&b);
                yol_path_clear(&a);
                if (yol_font_glyph(&c, &f, (uint32_t)g, &a, NULL) != YOL_OK) {
                    printf("glyph %d: %s\n", g, yol_error(&c));
                    ne++;
                } else if (seg_same(&a, &b, &maxd)) {
                    eq++;
                } else {
                    ne++;
                    if (shown++ < 10) printf("differs: glyph %d (%d and %d points)\n", g, a.n_pts, b.n_pts);
                }
                if (g == show_g) { show(&a, "ysp/outline.h", upem); show(&b, "HarfBuzz", upem); }
                if (yol_cset_add(&sb, (uint32_t)g, &b) != YOL_OK) { printf("glyph %d into a set: %s\n", g, yol_error(&c)); rc = 1; }
            }
            if (op != 'G') break;
            g = atoi(q + 2);
            yol_path_clear(&b);
            yol_set_xform(&b, yol_xf_scale(inv, -inv));
        } else if (op == 'M' && sscanf(q + 2, "%lf %lf", v, v + 1) == 2) yol_move(&b, v[0], v[1]);
        else if (op == 'L' && sscanf(q + 2, "%lf %lf", v, v + 1) == 2) yol_line(&b, v[0], v[1]);
        else if (op == 'Q' && sscanf(q + 2, "%lf %lf %lf %lf", v, v + 1, v + 2, v + 3) == 4) yol_quad(&b, v[0], v[1], v[2], v[3]);
        else if (op == 'C' && sscanf(q + 2, "%lf %lf %lf %lf %lf %lf", v, v + 1, v + 2, v + 3, v + 4, v + 5) == 6)
            yol_cubic(&b, v[0], v[1], v[2], v[3], v[4], v[5]);
        else if (op == 'Z') yol_close_path(&b);
        else { fprintf(stderr, "hbcmp: bad dump line near glyph %d\n", g); return 2; }
    }
    sets_eq = sa.n_words == sb.n_words && sa.n_texels == sb.n_texels &&
              !memcmp(sa.words, sb.words, 4 * (size_t)sa.n_words) &&
              !memcmp(sa.texels, sb.texels, 16 * (size_t)sa.n_texels);
    printf("%s face %d: %d glyphs, %d equal bit for bit, %d differ, largest difference %.3g em\n",
           argv[1], face, ng, eq, ne, maxd);
    printf("curve sets: %s (%u and %u words, %u and %u texels)\n", sets_eq ? "equal" : "DIFFER",
           (unsigned)sa.n_words, (unsigned)sb.n_words, (unsigned)sa.n_texels, (unsigned)sb.n_texels);
    yol_path_free(&a);
    yol_path_free(&b);
    yol_cset_close(&sa);
    yol_cset_close(&sb);
    yol_close(&c);
    free(fd);
    free(tx);
    return (ne || !sets_eq || rc) ? 1 : 0;
}
