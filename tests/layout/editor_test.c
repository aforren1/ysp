/* editor_test.c - checks of Skribidi's editor (skb_editor.h) with no window,
 * for the designer's text fields (docs/designer.md, order of work 2; the
 * faults are in docs/designer_ui_probe.md part 2, section 3).
 *
 *   1. Grapheme breaks of the editor's text (Skribidi patch 0005). Typed one
 *      codepoint at a time, "नमस्ते" is three graphemes, as the layout finds;
 *      select all and Backspace empty the field (U+0947 was left). The same
 *      after a virama is inserted in the middle and the first letter removed.
 *   2. Carets in a grapheme that HarfBuzz splits into two clusters (patch
 *      0006): after "नमस्ते" the caret is at the end of the word (it was
 *      drawn after "नम"), every caret stop and hit is at a grapheme start,
 *      and the last grapheme's selection is one finite rectangle.
 *   3. Font fallback on the editor's path (patch 0003): a digit in Hebrew
 *      text comes from the Latin font, and an arrow from a font of no
 *      script, with no glyph 0.
 *   4. Parts 1 to 3 with the probe's Noto fonts, if the folder has them
 *      (YSP_NOTO_DIR, else YSP_EDITOR_TEST_FONTS), and the arrow from Noto
 *      Sans Symbols; else skipped, with the reason printed.
 * Parts 1 to 3 use fonts written here (tests/adapt/colr_font_build.h): a
 * box for each codepoint, 1000 units per em, at 16 px a glyph is 16 px wide.
 * Exit 0 on pass, 1 on a failure.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "skribidi/skb_editor.h"
#include "skribidi/skb_font_collection.h"
#include "skribidi/skb_layout.h"
#include "skribidi/skb_text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../adapt/colr_font_build.h"

#ifndef YSP_EDITOR_TEST_FONTS
#define YSP_EDITOR_TEST_FONTS "/usr/share/fonts/truetype/noto"
#endif

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define SIZE 16.0f
#define NAMASTE_N 6
static const uint32_t namaste[NAMASTE_N] = { 0x0928, 0x092E, 0x0938, 0x094D, 0x0924, 0x0947 };   /* na ma sa virama ta e */

static skb_temp_alloc_t* temp;

/* --- fonts ------------------------------------------------------------------- */

/* A box for each codepoint of cps[0 .. n - 1], and an empty space. */
static void box_font(cfb_buf* out, const uint32_t* cps, int n) {
    static cfb_font f;
    int i, g;
    cfb_init(&f, 1000);
    g = cfb_add_glyph(&f);
    cfb_map(&f, 0x20, g);
    for (i = 0; i < n; i++) {
        g = cfb_add_glyph(&f);
        cfb_box(&f.g[g], 100, 0, 900, 700);
        cfb_map(&f, cps[i], g);
    }
    out->n = 0;
    cfb_write(&f, out);
}

static void range_cps(uint32_t* cps, int* n, uint32_t first, uint32_t last) {
    uint32_t c;
    for (c = first; c <= last; c++) cps[(*n)++] = c;
}

static skb_font_handle_t add_font(skb_font_collection_t* fc, const char* name, const cfb_buf* b) {
    return skb_font_collection_add_font_from_data(fc, name, b->d, b->n, NULL, NULL, SKB_FONT_FAMILY_DEFAULT, NULL);
}

/* --- the editor ---------------------------------------------------------------- */

static skb_editor_t* editor_make(skb_font_collection_t* fc) {
    static skb_attribute_t pa[1];
    skb_editor_params_t p;
    pa[0] = skb_attribute_make_font_size(SIZE);
    memset(&p, 0, sizeof p);
    p.font_collection = fc;
    p.editor_width = -1.0f;
    p.editor_height = -1.0f;
    p.paragraph_attributes = (skb_attribute_set_t){ pa, 1, 0, NULL };
    return skb_editor_create(&p);
}

static void type_cps(skb_editor_t* ed, const uint32_t* cps, int n) {
    int i;
    for (i = 0; i < n; i++) skb_editor_insert_codepoint(ed, temp, SKB_CURRENT_SELECTION, cps[i]);
}

static void type_utf8(skb_editor_t* ed, const char* s) {
    uint32_t cps[64];
    int n = 0;
    const unsigned char* p = (const unsigned char*)s;
    while (*p && n < 64) {
        uint32_t c = *p++;
        int k = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c &= k == 3 ? 0x07u : k == 2 ? 0x0Fu : k == 1 ? 0x1Fu : 0x7Fu;
        while (k-- > 0 && *p) c = (c << 6) | (*p++ & 0x3Fu);
        cps[n++] = c;
    }
    type_cps(ed, cps, n);
}

static int text_of(const skb_editor_t* ed, uint32_t* out, int cap) {
    const int n = skb_editor_get_text_utf32_count(ed);
    if (n + 1 > cap) return -1;
    skb_editor_get_text_utf32(ed, out, cap);
    return n;
}

static const skb_layout_t* layout_of(const skb_editor_t* ed) { return skb_editor_get_paragraph_layout(ed, 0); }

/* The first offset where the text's grapheme breaks (what removal and caret
 * moves use) differ from the layout's (what select all and the caret use),
 * or -1. */
static int grapheme_mismatch(const skb_editor_t* ed) {
    const skb_text_t* t = skb_editor_get_paragraph_text(ed, 0);
    const skb_layout_t* lay = layout_of(ed);
    const int n = skb_text_get_utf32_count(t);
    int i;
    for (i = 0; i <= n; i++)
        if (skb_text_get_next_grapheme_offset(t, i) != skb_layout_get_next_grapheme_offset(lay, i)) return i;
    return -1;
}

static float layout_width(const skb_layout_t* lay) {
    const skb_glyph_t* g = skb_layout_get_glyphs(lay);
    const int n = skb_layout_get_glyphs_count(lay);
    float w = 0.0f;
    int i;
    for (i = 0; i < n; i++) w += g[i].advance_x;
    return w;
}

static float caret_x(const skb_editor_t* ed, int offset, uint8_t affinity) {
    return skb_editor_get_caret_info_at(ed, (skb_text_position_t){ offset, affinity }).x;
}

static int is_grapheme_start(const skb_editor_t* ed, int offset) {
    return skb_layout_align_grapheme_offset(layout_of(ed), offset) == offset;
}

typedef struct rects { int n; float x0, x1; int finite; } rects;
static void rect_cb(skb_rect2_t r, void* ctx) {
    rects* R = (rects*)ctx;
    if (R->n == 0) { R->x0 = r.x; R->x1 = r.x + r.width; }
    if (!(r.x == r.x) || !(r.width == r.width)) R->finite = 0;   /* NaN */
    R->n++;
}

/* The font of the glyphs of codepoint cp, and whether any of them is glyph 0;
 * 0 when no glyph has it. */
static skb_font_handle_t font_of_cp(const skb_layout_t* lay, uint32_t cp, int* has_gid0) {
    const skb_layout_run_t* runs = skb_layout_get_layout_runs(lay);
    const skb_glyph_t* glyphs = skb_layout_get_glyphs(lay);
    const skb_cluster_t* clusters = skb_layout_get_clusters(lay);
    const uint32_t* text = skb_layout_get_text(lay);
    const int nr = skb_layout_get_layout_runs_count(lay);
    skb_font_handle_t font = 0;
    int r, g;
    *has_gid0 = 0;
    for (r = 0; r < nr; r++) {
        if (runs[r].type != SKB_CONTENT_RUN_UTF8 && runs[r].type != SKB_CONTENT_RUN_UTF32) continue;
        for (g = runs[r].glyph_range.start; g < runs[r].glyph_range.end; g++) {
            const skb_cluster_t* c = &clusters[glyphs[g].cluster_idx];
            int k;
            for (k = c->text_offset; k < c->text_offset + c->text_count; k++) {
                if (text[k] != cp) continue;
                font = runs[r].font_handle;
                if (glyphs[g].gid == 0) *has_gid0 = 1;
            }
        }
    }
    return font;
}

static int count_gid0(const skb_layout_t* lay) {
    const skb_glyph_t* g = skb_layout_get_glyphs(lay);
    const int n = skb_layout_get_glyphs_count(lay);
    int i, k = 0;
    for (i = 0; i < n; i++) k += g[i].gid == 0;
    return k;
}

/* --- 1 and 2: Devanagari ----------------------------------------------------- */

/* glyph_w > 0: every glyph is that wide (the box font), so the caret stops
 * are known; else only their order and the ends are checked. */
static void check_devanagari(skb_font_collection_t* fc, const char* tag, float glyph_w) {
    uint32_t t[16];
    int n, k;
    skb_editor_t* ed = editor_make(fc);
    float w, prev;
    skb_text_range_t sel;

    /* 1. typed one codepoint at a time */
    type_cps(ed, namaste, NAMASTE_N);
    n = text_of(ed, t, 16);
    CHECK(n == NAMASTE_N, "%s: typed %d codepoints, the field has %d", tag, NAMASTE_N, n);
    k = grapheme_mismatch(ed);
    CHECK(k < 0, "%s: typed: the text's next grapheme after offset %d differs from the layout's", tag, k);
    CHECK(skb_layout_get_next_grapheme_offset(layout_of(ed), 2) == NAMASTE_N, "%s: sa virama ta e is not one grapheme (GB9c)", tag);

    /* 2. the caret after typing, and the stops */
    sel = skb_editor_get_current_selection(ed);
    CHECK(is_grapheme_start(ed, sel.end.offset), "%s: after typing the caret is at offset %d, inside a grapheme", tag, sel.end.offset);
    CHECK(skb_editor_get_text_offset_from_text_position(ed, sel.end) == NAMASTE_N,
          "%s: after typing the caret is at offset %d of %d", tag, skb_editor_get_text_offset_from_text_position(ed, sel.end), NAMASTE_N);
    w = layout_width(layout_of(ed));
    {
        const float x = skb_editor_get_caret_info_at(ed, SKB_CURRENT_SELECTION_END).x;
        CHECK(fabsf(x - w) < 0.01f, "%s: after typing the caret is at x %.2f, the word is %.2f wide", tag, (double)x, (double)w);
    }
    CHECK(fabsf(caret_x(ed, NAMASTE_N, SKB_AFFINITY_TRAILING) - w) < 0.01f, "%s: the caret at the end is at x %.2f of %.2f", tag,
          (double)caret_x(ed, NAMASTE_N, SKB_AFFINITY_TRAILING), (double)w);
    CHECK(fabsf(caret_x(ed, 2, SKB_AFFINITY_LEADING) - w) < 0.01f, "%s: the caret after the last grapheme is at x %.2f of %.2f", tag,
          (double)caret_x(ed, 2, SKB_AFFINITY_LEADING), (double)w);
    prev = -1.0f;
    for (k = 0; k <= NAMASTE_N; k++) {
        const float x = caret_x(ed, k, SKB_AFFINITY_TRAILING);
        CHECK(x >= prev - 0.01f && x <= w + 0.01f, "%s: the caret before offset %d is at x %.2f (the one before at %.2f)", tag, k, (double)x, (double)prev);
        prev = x;
    }
    if (glyph_w > 0) {
        CHECK(fabsf(caret_x(ed, 1, SKB_AFFINITY_TRAILING) - glyph_w) < 0.01f, "%s: caret before ma at %.2f", tag, (double)caret_x(ed, 1, SKB_AFFINITY_TRAILING));
        CHECK(fabsf(caret_x(ed, 2, SKB_AFFINITY_TRAILING) - 2 * glyph_w) < 0.01f, "%s: caret before sa at %.2f", tag, (double)caret_x(ed, 2, SKB_AFFINITY_TRAILING));
        CHECK(fabsf(w - NAMASTE_N * glyph_w) < 0.01f, "%s: the word is %.2f wide", tag, (double)w);
    }
    {
        rects R = { 0, 0.0f, 0.0f, 1 };
        const float x2 = caret_x(ed, 2, SKB_AFFINITY_TRAILING);
        skb_editor_iterate_text_range_bounds(ed, (skb_text_range_t){ { 2, SKB_AFFINITY_TRAILING }, { 2, SKB_AFFINITY_LEADING } }, rect_cb, &R);
        CHECK(R.n == 1 && R.finite && fabsf(R.x0 - x2) < 0.01f && fabsf(R.x1 - w) < 0.01f,
              "%s: the last grapheme's selection is %d rectangles, the first %.2f..%.2f, want %.2f..%.2f", tag, R.n, (double)R.x0, (double)R.x1,
              (double)x2, (double)w);
    }
    {
        const skb_rect2_t b = skb_editor_get_layout_bounds(ed);
        int bad = 0, bad_offset = 0;
        for (k = 0; k <= 40; k++) {
            const skb_text_position_t h = skb_editor_hit_test(ed, SKB_MOVEMENT_CARET, (float)k * w / 40.0f, b.y + b.height * 0.5f);
            if (!is_grapheme_start(ed, h.offset) || h.affinity == SKB_AFFINITY_NONE) { bad++; bad_offset = h.offset; }
        }
        CHECK(bad == 0, "%s: %d of 41 hits are not at a grapheme start (one at offset %d)", tag, bad, bad_offset);
    }

    /* 1. select all and Backspace */
    skb_editor_select_all(ed);
    skb_editor_process_key_pressed(ed, temp, SKB_KEY_BACKSPACE, 0);
    n = text_of(ed, t, 16);
    CHECK(n == 0, "%s: select all and Backspace left %d codepoints (the first U+%04X)", tag, n, n > 0 ? (unsigned)t[0] : 0u);
    skb_editor_destroy(ed);

    /* 1. a virama inserted in the middle, then the first letter removed */
    ed = editor_make(fc);
    {
        const uint32_t no_virama[5] = { 0x0928, 0x092E, 0x0938, 0x0924, 0x0947 };
        type_cps(ed, no_virama, 5);
    }
    k = grapheme_mismatch(ed);
    CHECK(k < 0, "%s: typed without the virama: the graphemes differ after offset %d", tag, k);
    skb_editor_insert_codepoint(ed, temp, (skb_text_range_t){ { 3, SKB_AFFINITY_TRAILING }, { 3, SKB_AFFINITY_TRAILING } }, 0x094D);
    n = text_of(ed, t, 16);
    CHECK(n == NAMASTE_N && memcmp(t, namaste, sizeof namaste) == 0, "%s: inserting the virama gave %d codepoints", tag, n);
    k = grapheme_mismatch(ed);
    CHECK(k < 0, "%s: virama inserted: the graphemes differ after offset %d", tag, k);
    skb_editor_remove(ed, temp, (skb_text_range_t){ { 0, SKB_AFFINITY_TRAILING }, { 1, SKB_AFFINITY_TRAILING } });
    n = text_of(ed, t, 16);
    CHECK(n == NAMASTE_N - 1 && memcmp(t, namaste + 1, sizeof namaste - sizeof namaste[0]) == 0, "%s: removing na left %d codepoints", tag, n);
    k = grapheme_mismatch(ed);
    CHECK(k < 0, "%s: na removed: the graphemes differ after offset %d", tag, k);
    skb_editor_select_all(ed);
    skb_editor_process_key_pressed(ed, temp, SKB_KEY_BACKSPACE, 0);
    CHECK(text_of(ed, t, 16) == 0, "%s: select all and Backspace after the edits left text", tag);
    skb_editor_destroy(ed);
}

/* --- 3: font fallback -------------------------------------------------------- */

/* The fonts are tried in the probe's order: Latin first, the font that has
 * the character last. */
static void check_fallback(skb_font_collection_t* fc, const char* tag, skb_font_handle_t latin, skb_font_handle_t arrow) {
    skb_editor_t* ed = editor_make(fc);
    skb_font_handle_t f;
    int gid0;
    type_utf8(ed, "\xD7\x9E\xD7\x98\xD7\xA8\xD7\x94 \xD7\x9E\xD7\xA1\xD7\xA4\xD7\xA8 3");   /* "goal number 3" in Hebrew */
    f = font_of_cp(layout_of(ed), '3', &gid0);
    CHECK(f != 0 && !gid0, "%s: the digit in Hebrew text is glyph 0 (font %u)", tag, (unsigned)f);
    CHECK(f == latin, "%s: the digit in Hebrew text is from font %u, the Latin font is %u", tag, (unsigned)f, (unsigned)latin);
    CHECK(count_gid0(layout_of(ed)) == 0, "%s: the Hebrew line has %d glyph 0", tag, count_gid0(layout_of(ed)));
    skb_editor_destroy(ed);
    if (!arrow) return;
    ed = editor_make(fc);
    type_utf8(ed, "go \xE2\x86\x92 \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7");                 /* "go -> marhaba" */
    f = font_of_cp(layout_of(ed), 0x2192, &gid0);
    CHECK(f == arrow && !gid0, "%s: the arrow is from font %u (glyph 0: %d), the arrow's font is %u", tag, (unsigned)f, gid0, (unsigned)arrow);
    CHECK(count_gid0(layout_of(ed)) == 0, "%s: the arrow line has %d glyph 0", tag, count_gid0(layout_of(ed)));
    skb_editor_destroy(ed);
}

/* --- 4: the probe's fonts ---------------------------------------------------- */

static int file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void check_noto(void) {
    static const char* files[4] = { "NotoSans-Regular.ttf", "NotoSansArabic-Regular.ttf", "NotoSansHebrew-Regular.ttf",
                                    "NotoSansDevanagari-Regular.ttf" };
    const char* dir = getenv("YSP_NOTO_DIR");
    skb_font_collection_t* fc;
    skb_font_handle_t h[5];
    char path[1024];
    int i;
    if (!dir || !*dir) dir = YSP_EDITOR_TEST_FONTS;
    for (i = 0; i < 4; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        if (!file_exists(path)) { printf("noto: %s not found: parts 1 to 3 with the probe's fonts skipped\n", path); return; }
    }
    fc = skb_font_collection_create();
    for (i = 0; i < 4; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        h[i] = skb_font_collection_add_font(fc, path, SKB_FONT_FAMILY_DEFAULT, NULL);
        CHECK(h[i] != 0, "noto: %s did not open", path);
        if (!h[i]) { skb_font_collection_destroy(fc); return; }
    }
    check_devanagari(fc, "noto", 0.0f);
    /* None of the four has U+2192 (probe part 2, section 3); Noto Sans
     * Symbols has it, and no Latin or Arabic: last, as a font list would
     * have it. */
    snprintf(path, sizeof path, "%s/%s", dir, "NotoSansSymbols-Regular.ttf");
    h[4] = file_exists(path) ? skb_font_collection_add_font(fc, path, SKB_FONT_FAMILY_DEFAULT, NULL) : 0;
    if (!h[4]) printf("noto: %s not found: the arrow not checked\n", path);
    check_fallback(fc, "noto", h[0], h[4]);
    printf("noto: checked with %s\n", dir);
    skb_font_collection_destroy(fc);
}

int main(void) {
    static cfb_buf deva, latin, hebrew, arabic, arrows;
    uint32_t cps[64];
    int n;
    skb_font_collection_t* fc;
    skb_font_handle_t h_latin, h_arrow;

    temp = skb_temp_alloc_create(64 * 1024);

    /* 1 and 2 */
    box_font(&deva, namaste, NAMASTE_N);
    fc = skb_font_collection_create();
    CHECK(add_font(fc, "deva", &deva) != 0, "the Devanagari box font did not open");
    check_devanagari(fc, "box", SIZE);
    skb_font_collection_destroy(fc);

    /* 3. Latin: digits, "g", "o"; Hebrew and Arabic: letters, no digits; the
     * arrow font: U+2192 only, so it has no script. */
    n = 0; range_cps(cps, &n, '0', '9'); cps[n++] = 'g'; cps[n++] = 'o';
    box_font(&latin, cps, n);
    n = 0; range_cps(cps, &n, 0x0627, 0x064A); box_font(&arabic, cps, n);
    n = 0; range_cps(cps, &n, 0x05D0, 0x05EA); box_font(&hebrew, cps, n);
    cps[0] = 0x2192; box_font(&arrows, cps, 1);
    fc = skb_font_collection_create();
    h_latin = add_font(fc, "latin", &latin);
    CHECK(add_font(fc, "arabic", &arabic) != 0 && add_font(fc, "hebrew", &hebrew) != 0, "a box font did not open");
    h_arrow = add_font(fc, "arrows", &arrows);
    CHECK(h_latin != 0 && h_arrow != 0, "a box font did not open");
    check_fallback(fc, "box", h_latin, h_arrow);
    skb_font_collection_destroy(fc);

    /* 4 */
    check_noto();

    skb_temp_alloc_destroy(temp);
    free(deva.d); free(latin.d); free(hebrew.d); free(arabic.d); free(arrows.d);
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
