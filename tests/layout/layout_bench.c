/* layout_bench.c - what pack/layout costs (docs/layout.md, Costs).
 *
 *   Re-layout of a label and of a 500-codepoint paragraph (the glue and
 *   Skribidi, glyphs already built), one editor keystroke at 500
 *   codepoints (Skribidi's edit and layout and the glue's conversion), the
 *   first build of a glyph into a curve set, and adding a font (HarfBuzz and
 *   ysp/outline.h open it, the glue hashes it).
 *   Medians of each round's median, 3 interleaved rounds, at least 0.3 s an
 *   item a round. Run it under the shared measurement lock.
 * Usage: layout_bench [FONT_DIR]   (default C:/Windows/Fonts)
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"
#include "ysp/layout.h"
#include "skribidi/skb_editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROUNDS 3
#define MAX_SAMPLES 200000

static double samples[MAX_SAMPLES];

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}
static double median(double* v, int n) { qsort(v, (size_t)n, sizeof *v, cmp_d); return n ? v[n / 2] : 0; }

static uint8_t* read_file(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    uint8_t* d;
    long k;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); k = ftell(f); fseek(f, 0, SEEK_SET);
    d = k > 0 ? (uint8_t*)malloc((size_t)k) : NULL;
    if (!d || fread(d, 1, (size_t)k, f) != (size_t)k) { fclose(f); free(d); return NULL; }
    fclose(f);
    *n = (size_t)k;
    return d;
}

static const char* EN_LABEL = "Press F for yes, J for no.";
static const char* AR_LABEL = "\xd8\xa7\xd8\xb6\xd8\xba\xd8\xb7 F \xd9\x84\xd9\x84\xd9\x85\xd9\x88\xd8\xa7\xd9\x81\xd9\x82\xd8\xa9";
static const char* EN_PARA =
    "Participants sat 57 cm from the display and pressed a key as soon as the grating appeared. Each block began with "
    "ten practice trials, and the next block started only after a short rest. Contrast, spatial frequency and "
    "orientation varied from trial to trial. ";
static const char* AR_PARA =
    "\xd8\xac\xd9\x84\xd8\xb3 \xd8\xa7\xd9\x84\xd9\x85\xd8\xb4\xd8\xa7\xd8\xb1\xd9\x83\xd9\x88\xd9\x86 \xd8\xb9\xd9\x84\xd9\x89 "
    "\xd8\xa8\xd8\xb9\xd8\xaf 57 \xd8\xb3\xd9\x85 \xd9\x85\xd9\x86 \xd8\xa7\xd9\x84\xd8\xb4\xd8\xa7\xd8\xb4\xd8\xa9\xd8\x8c "
    "\xd9\x88\xd8\xb6\xd8\xba\xd8\xb7\xd9\x88\xd8\xa7 \xd8\xb9\xd9\x84\xd9\x89 \xd9\x85\xd9\x81\xd8\xaa\xd8\xa7\xd8\xad "
    "\xd8\xb9\xd9\x86\xd8\xaf \xd8\xb8\xd9\x87\xd9\x88\xd8\xb1 \xd8\xa7\xd9\x84\xd8\xb4\xd8\xa8\xd9\x83\xd8\xa9. "
    "The trial ended after 2.5 seconds. ";
static const char* ZH_PARA =
    "\xe5\x8f\x82\xe4\xb8\x8e\xe8\x80\x85\xe5\x9d\x90\xe5\x9c\xa8\xe8\xb7\x9d\xe7\xa6\xbb\xe5\xb1\x8f\xe5\xb9\x95\xe4\xba\x94\xe5\x8d\x81"
    "\xe4\xb8\x83\xe5\x8e\x98\xe7\xb1\xb3\xe7\x9a\x84\xe4\xbd\x8d\xe7\xbd\xae\xef\xbc\x8c\xe7\x9c\x8b\xe5\x88\xb0\xe5\x85\x89\xe6\xa0\x85"
    "\xe5\x87\xba\xe7\x8e\xb0\xe5\x90\x8e\xe7\xab\x8b\xe5\x8d\xb3\xe6\x8c\x89\xe9\x94\xae\xe3\x80\x82";

/* unit repeated until the text has n_cp codepoints or more; *got the count. */
static char* repeat_to(const char* unit, int n_cp, int* got) {
    size_t ul = strlen(unit), cap = ul * 64 + 1, len = 0;
    char* s = (char*)malloc(cap);
    int cp = 0, total = 0;
    const unsigned char* p;
    for (p = (const unsigned char*)unit; *p; p++) if ((*p & 0xc0) != 0x80) cp++;
    s[0] = 0;
    while (len + ul < cap && total < n_cp) { memcpy(s + len, unit, ul + 1); len += ul; total += cp; }
    *got = total;
    return s;
}

typedef struct item { const char* name; int kind; const char* text; float width; } item;
enum { K_LAYOUT, K_KEY };

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "C:/Windows/Fonts";
    char path[600];
    size_t n_segoe = 0, n_yahei = 0;
    uint8_t* segoe, * yahei;
    yol_ctx ol;
    ylay_lib* L;
    ylay_block b;
    int n_en, n_ar;
    char* en500 = repeat_to(EN_PARA, 500, &n_en);
    char* ar500 = repeat_to(AR_PARA, 500, &n_ar);
    item items[6];
    double med[6][ROUNDS];
    int r, i;
    printf("paragraphs: English %d codepoints, Arabic %d\n", n_en, n_ar);
    snprintf(path, sizeof path, "%s/segoeui.ttf", dir);
    segoe = read_file(path, &n_segoe);
    snprintf(path, sizeof path, "%s/msyh.ttc", dir);
    yahei = read_file(path, &n_yahei);
    if (!segoe || !yahei) { printf("layout_bench: needs segoeui.ttf and msyh.ttc in %s\n", dir); return 0; }
    yol_open(&ol, NULL);
    memset(&b, 0, sizeof b);

    /* adding a font, and the first build of glyphs */
    {
        double t_add[2];
        ylay_font_info fi;
        int64_t t0 = yrt_now_ns();
        ylay_create(&L, &(ylay_desc){ .ol = &ol });
        ylay_add_font(L, segoe, n_segoe, 0, "segoeui.ttf");
        t_add[0] = (double)(yrt_now_ns() - t0) * 1e-3;
        t0 = yrt_now_ns();
        ylay_add_font(L, yahei, n_yahei, 0, "msyh.ttc");
        t_add[1] = (double)(yrt_now_ns() - t0) * 1e-3;
        printf("add font: Segoe UI (%zu bytes) %.0f us, Microsoft YaHei (%zu bytes) %.0f us\n", n_segoe, t_add[0], n_yahei, t_add[1]);
        ylay_layout(L, en500, -1, &(ylay_style){ .size = 16, .width = 600 }, NULL, 0, &b);
        ylay_font(L, 0, &fi);
        printf("first build: Latin %u glyphs in %.0f us, %.1f us a glyph\n", fi.n_built, fi.build_us, fi.build_us / fi.n_built);
        ylay_layout(L, ZH_PARA, -1, &(ylay_style){ .size = 16, .width = 600, .lang = "zh-Hans" }, NULL, 0, &b);
        ylay_font(L, 1, &fi);
        printf("first build: CJK %u glyphs in %.0f us, %.1f us a glyph\n", fi.n_built, fi.build_us, fi.build_us / fi.n_built);
        /* Arabic glyphs too, so the timed layouts build nothing */
        ylay_layout(L, ar500, -1, &(ylay_style){ .size = 16, .width = 600 }, NULL, 0, &b);
        ylay_layout(L, AR_LABEL, -1, &(ylay_style){ .size = 32 }, NULL, 0, &b);
        ylay_layout(L, EN_LABEL, -1, &(ylay_style){ .size = 32 }, NULL, 0, &b);
    }

    items[0] = (item){ "label, English", K_LAYOUT, EN_LABEL, 0 };
    items[1] = (item){ "label, Arabic", K_LAYOUT, AR_LABEL, 0 };
    items[2] = (item){ "500 codepoints, English, 600 px", K_LAYOUT, en500, 600 };
    items[3] = (item){ "500 codepoints, Arabic, 600 px", K_LAYOUT, ar500, 600 };
    items[4] = (item){ "keystroke at 500, English", K_KEY, en500, 600 };
    items[5] = (item){ "keystroke at 500, Arabic", K_KEY, ar500, 600 };
    for (r = 0; r < ROUNDS; r++) {
        for (i = 0; i < 6; i++) {
            int n = 0;
            int64_t start = yrt_now_ns();
            skb_editor_t* ed = NULL;
            if (items[i].kind == K_KEY) {
                skb_attribute_t la[1], pa[1];
                skb_editor_params_t p;
                la[0] = skb_attribute_make_text_wrap(SKB_WRAP_WORD_CHAR);
                pa[0] = skb_attribute_make_font_size(16);
                memset(&p, 0, sizeof p);
                p.font_collection = ylay_skb_fonts(L);
                p.attribute_collection = ylay_skb_attributes(L);
                p.editor_width = items[i].width;
                p.editor_height = -1.0f;
                p.layout_attributes = (skb_attribute_set_t){ la, 1, 0, NULL };
                p.paragraph_attributes = (skb_attribute_set_t){ pa, 1, 0, NULL };
                ed = skb_editor_create(&p);
                skb_editor_set_text_utf8(ed, ylay_skb_temp(L), items[i].text, -1);
            }
            while (n < MAX_SAMPLES && (n < 30 || yrt_now_ns() - start < 300000000)) {
                int64_t t0 = yrt_now_ns();
                if (items[i].kind == K_LAYOUT) {
                    ylay_layout(L, items[i].text, -1, &(ylay_style){ .size = items[i].width > 0 ? 16.0f : 32.0f, .width = items[i].width },
                                  NULL, 0, &b);
                } else {
                    /* type a letter at the end, or take it back: the text stays near 500 */
                    int k, np;
                    if (n & 1) skb_editor_process_key_pressed(ed, ylay_skb_temp(L), SKB_KEY_BACKSPACE, 0);
                    else skb_editor_insert_codepoint(ed, ylay_skb_temp(L), SKB_CURRENT_SELECTION, 'e');
                    np = skb_editor_get_paragraph_count(ed);
                    for (k = 0; k < np; k++) {
                        skb_vec2_t off = skb_editor_get_paragraph_offset(ed, k);
                        ylay_from_skb(L, skb_editor_get_paragraph_layout(ed, k), off.x, off.y, 0, k > 0, &b);
                    }
                }
                samples[n++] = (double)(yrt_now_ns() - t0) * 1e-3;
            }
            med[i][r] = median(samples, n);
            if (ed) {
                skb_editor_set_text_utf8(ed, ylay_skb_temp(L), items[i].text, -1);
                if (r == ROUNDS - 1) printf("  %s: p90 %.1f us, max %.1f us of %d calls (last round)\n", items[i].name,
                                            samples[n * 9 / 10], samples[n - 1], n);
            } else if (r == ROUNDS - 1) {
                printf("  %s: p90 %.1f us, max %.1f us of %d calls (last round)\n", items[i].name, samples[n * 9 / 10], samples[n - 1], n);
            }
            if (ed) skb_editor_destroy(ed);
        }
    }
    printf("\nmedian us per call, rounds 1 / 2 / 3, median of the three:\n");
    for (i = 0; i < 6; i++) {
        double m[ROUNDS];
        memcpy(m, med[i], sizeof m);
        printf("  %-36s %8.1f %8.1f %8.1f  -> %.1f\n", items[i].name, med[i][0], med[i][1], med[i][2], median(m, ROUNDS));
    }
    ylay_block_free(&b);
    ylay_destroy(L);
    yol_close(&ol);
    free(segoe); free(yahei); free(en500); free(ar500);
    return 0;
}
