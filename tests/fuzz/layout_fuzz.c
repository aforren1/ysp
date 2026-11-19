/* layout_fuzz.c - fuzz target for text layout: Skribidi through the layout
 * glue (pack/layout.c, ysp/layout.h), and Skribidi's editor. Text is
 * untrusted input to the pack tool (a definition's text) and to the
 * designer's text fields, and bidi controls and scripts shaped against their
 * direction reach code that fonts alone do not (Skribidi patch 0007,
 * docs/layout.md).
 *
 * The input: byte 0 picks the base direction (bits 0-1: auto, LTR, RTL),
 * wrapping (bit 2, at a width of 8 to 263 px from byte 1) and the editor
 * step (bit 3, its keys from the bytes after the text); the rest, to the
 * first 0xFF byte, is the text as UTF-8 (any bytes).
 *
 * Every input must return without a sanitizer report, and:
 * - ylay_layout() succeeds or fails with a message, and each item's cluster
 *   is inside the text;
 * - in Skribidi's layout of the text, every codepoint is in exactly one
 *   cluster and no cluster is empty (the glue's glyph-to-text map and the
 *   editor's carets read the clusters);
 * - with the editor step, the text's first paragraph inserted into
 *   Skribidi's editor and the keys pressed (arrows, Home, End, Backspace,
 *   Delete, with and without Shift, select all) keep every paragraph's
 *   clusters so, and each paragraph goes through ylay_from_skb(). Left out,
 *   as pinned Skribidi fails them on any text (docs/layout.md, Known
 *   issues): Ctrl and the word moves (Ctrl+Right reads one past the text),
 *   Enter and later paragraphs (an empty paragraph passes NULL to memcpy),
 *   and keys before the first insert (they dereference NULL).
 *
 * The fonts are written here (tests/adapt/colr_font_build.h), a box for each
 * codepoint: Latin, Devanagari (two fonts, as a font holds 64 glyphs here;
 * U+0978 to U+097F get glyph 0), Arabic, Hebrew. None has U+25CC, so
 * HarfBuzz inserts no dotted circle, as with many fonts.
 *
 * libFuzzer, clang (link the layout library and its vendored stack, as
 * CMake's layout_fuzz target does).
 * Self-driven, gcc: CMake target layout_fuzz (YSP_BUILD_LAYOUT, tests on):
 * the seeds below, then N mutations of them; on this kernel ASan needs
 * setarch -R:
 *     setarch -R build/sanitizers/pack/layout_fuzz 100000 [seed]
 * layout_fuzz 0 runs the seeds only (ctest layout_fuzz_seeds).
 * Replay of input files: layout_fuzz --replay FILES. The seeds as files, for
 * a libFuzzer corpus: layout_fuzz --write-seeds DIR.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"
#include "ysp/layout.h"
#include "skribidi/skb_editor.h"
#include "skribidi/skb_layout.h"
#include "../adapt/colr_font_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INPUT 4096

static yol_ctx g_ol;
static ylay_lib* g_L;
static ylay_block g_b;
static unsigned char g_seen[MAX_INPUT + 1];
static long g_inputs, g_refused, g_edits;

static void fail(const char* what, const uint8_t* d, size_t n) {
    size_t i;
    fprintf(stderr, "layout_fuzz: %s; input:", what);
    for (i = 0; i < n && i < 256; i++) fprintf(stderr, " %02X", d[i]);
    fprintf(stderr, "\n");
    abort();
}

/* 0 when every codepoint is in exactly one cluster and no cluster is empty. */
static int tiling_fault(const skb_layout_t* lay) {
    const skb_layout_run_t* runs = skb_layout_get_layout_runs(lay);
    const skb_cluster_t* clusters = skb_layout_get_clusters(lay);
    const int nr = skb_layout_get_layout_runs_count(lay), nt = skb_layout_get_text_count(lay);
    int r, c, k;
    if (nt < 0 || nt > MAX_INPUT) return 1;
    memset(g_seen, 0, (size_t)nt);
    for (r = 0; r < nr; r++) {
        if (runs[r].type != SKB_CONTENT_RUN_UTF8 && runs[r].type != SKB_CONTENT_RUN_UTF32) continue;
        if (runs[r].cluster_range.start >= runs[r].cluster_range.end) return 1;
        for (c = runs[r].cluster_range.start; c < runs[r].cluster_range.end; c++) {
            const int s = clusters[c].text_offset, m = clusters[c].text_count;
            if (m <= 0 || s < 0 || s + m > nt) return 1;
            for (k = s; k < s + m; k++)
                if (g_seen[k]++) return 1;
        }
    }
    for (k = 0; k < nt; k++)
        if (!g_seen[k]) return 1;
    return 0;
}

static void box_font(cfb_buf* out, uint32_t first, uint32_t last) {
    static cfb_font f;
    uint32_t c;
    int g;
    cfb_init(&f, 1000);
    g = cfb_add_glyph(&f);
    cfb_map(&f, 0x20, g);
    for (c = first; c <= last; c++) {
        g = cfb_add_glyph(&f);
        cfb_box(&f.g[g], 100, 0, 900, 700);
        cfb_map(&f, c, g);
    }
    out->n = 0;
    cfb_write(&f, out);
}

static void setup(void) {
    static cfb_buf fonts[5];
    static const uint32_t ranges[5][2] = { { 0x21, 0x5A }, { 0x0900, 0x093B }, { 0x093C, 0x0977 }, { 0x0627, 0x064A }, { 0x05D0, 0x05EA } };
    int i;
    yol_open(&g_ol, NULL);
    if (ylay_create(&g_L, &(ylay_desc){ .ol = &g_ol }) != YLAY_OK) fail("create", NULL, 0);
    for (i = 0; i < 5; i++) {
        box_font(&fonts[i], ranges[i][0], ranges[i][1]);
        if (ylay_add_font(g_L, fonts[i].d, fonts[i].n, 0, "box") != i) fail("a box font did not open", NULL, 0);
    }
}

static void editor_step(const uint8_t* d, size_t n, const char* text, int len, int dir, float width, const uint8_t* keys, size_t nk) {
    skb_attribute_t pa[3];
    skb_editor_params_t p;
    skb_editor_t* ed;
    size_t k;
    int i;
    pa[0] = skb_attribute_make_font_size(16.0f);
    pa[1] = skb_attribute_make_text_base_direction(dir == 1 ? SKB_DIRECTION_LTR : dir == 2 ? SKB_DIRECTION_RTL : SKB_DIRECTION_AUTO);
    pa[2] = skb_attribute_make_text_wrap(SKB_WRAP_WORD);
    memset(&p, 0, sizeof p);
    p.font_collection = ylay_skb_fonts(g_L);
    p.attribute_collection = ylay_skb_attributes(g_L);
    p.editor_width = width > 0 ? width : -1.0f;
    p.editor_height = -1.0f;
    p.paragraph_attributes = (skb_attribute_set_t){ pa, width > 0 ? 3 : 2, 0, NULL };
    ed = skb_editor_create(&p);
    if (!ed) fail("no editor", d, n);
    {
        /* The first paragraph only: in pinned Skribidi an insert with an
         * empty paragraph in it (Enter, or text that starts with a paragraph
         * separator) passes NULL to memcpy, which UBSan reports
         * (skb_text_append_utf32_with_payload(); docs/layout.md, Known
         * issues). */
        static uint32_t cps[MAX_INPUT];
        const int nc = skb_utf8_to_utf32(text, len, cps, MAX_INPUT);
        int e = 0;
        while (e < nc && !skb_is_paragraph_separator(cps[e])) e++;
        if (e > 0) skb_editor_insert_text_utf32(ed, ylay_skb_temp(g_L), SKB_CURRENT_SELECTION, cps, e);
    }
    for (k = 0; k <= nk && k < 32; k++) {
        /* A key in an editor that never had text dereferences NULL in pinned
         * Skribidi (docs/layout.md, Known issues). */
        if (k > 0 && skb_editor_get_paragraph_count(ed) > 0) {
            static const skb_editor_key_t key_of[7] = { SKB_KEY_LEFT, SKB_KEY_RIGHT, SKB_KEY_UP, SKB_KEY_DOWN, SKB_KEY_HOME, SKB_KEY_END, SKB_KEY_BACKSPACE };
            const uint8_t b = keys[k - 1];
            if (b % 9 == 7) skb_editor_process_key_pressed(ed, ylay_skb_temp(g_L), SKB_KEY_DELETE, 0);
            else if (b % 9 == 8) skb_editor_select_all(ed);
            else skb_editor_process_key_pressed(ed, ylay_skb_temp(g_L), key_of[b % 9], (b & 0x80) ? SKB_MOD_SHIFT : 0);
            g_edits++;
        }
        for (i = 0; i < skb_editor_get_paragraph_count(ed); i++) {
            const skb_layout_t* lay = skb_editor_get_paragraph_layout(ed, i);
            if (tiling_fault(lay)) fail("an editor paragraph's clusters do not tile its text", d, n);
            if (ylay_from_skb(g_L, lay, 0, 0, 0, i > 0, &g_b) != YLAY_OK && !ylay_error(g_L)[0]) fail("ylay_from_skb failed without a message", d, n);
        }
    }
    skb_editor_destroy(ed);
}

static void one(const uint8_t* d, size_t n) {
    const uint8_t flags = n > 0 ? d[0] : 0;
    const int dir = flags & 3;
    const float width = (flags & 4) && n > 1 ? 8.0f + (float)d[1] : 0.0f;
    const char* text = n > 2 ? (const char*)d + 2 : "";
    int len = 0, rc, nt;
    uint32_t i;
    skb_attribute_t la[2], ta[1];
    skb_layout_params_t p;
    skb_layout_t* lay;
    if (!g_L) setup();
    if (n > MAX_INPUT) n = MAX_INPUT;
    while (2 + (size_t)len < n && d[2 + len] != 0xFF) len++;
    g_inputs++;

    /* Skribidi alone: the clusters. */
    la[0] = skb_attribute_make_text_base_direction(dir == 1 ? SKB_DIRECTION_LTR : dir == 2 ? SKB_DIRECTION_RTL : SKB_DIRECTION_AUTO);
    la[1] = skb_attribute_make_text_wrap(SKB_WRAP_WORD);
    ta[0] = skb_attribute_make_font_size(16.0f);
    memset(&p, 0, sizeof p);
    p.font_collection = ylay_skb_fonts(g_L);
    p.attribute_collection = ylay_skb_attributes(g_L);
    p.layout_width = width > 0 ? width : -1.0f;
    p.layout_height = -1.0f;
    p.layout_attributes = (skb_attribute_set_t){ la, width > 0 ? 2 : 1, 0, NULL };
    lay = skb_layout_create_utf8(ylay_skb_temp(g_L), &p, text, len, (skb_attribute_set_t){ ta, 1, 0, NULL });
    if (!lay) fail("no layout", d, n);
    if (tiling_fault(lay)) fail("the clusters do not tile the text", d, n);
    nt = skb_layout_get_text_count(lay);
    skb_layout_destroy(lay);

    /* The pack tool's path. */
    rc = ylay_layout(g_L, text, len, &(ylay_style){ .size = 16.0f, .width = width, .dir = dir == 3 ? 0 : dir }, NULL, 0, &g_b);
    if (rc != YLAY_OK) {
        if (!ylay_error(g_L)[0]) fail("a layout failed without a message", d, n);
        g_refused++;
    } else {
        for (i = 0; i < g_b.n_items; i++)
            if (g_b.cluster[i] >= (uint32_t)(nt > 0 ? nt : 1)) fail("an item's cluster is past the text", d, n);
    }

    if (flags & 8) {
        const size_t keys = 2 + (size_t)len + 1;   /* past the 0xFF */
        editor_step(d, n, text, len, dir, width, d + (keys < n ? keys : 0), keys < n ? n - keys : 0);
    }
}

/* The fault of patch 0007 and its neighbors; a header of 2 bytes, then the
 * text; 0xFF ends the text where keys follow. */
#define SEED(s) { s, (int)sizeof(s) - 1 }
static const struct { const char* d; int n; } g_seeds[] = {
    SEED("\x00\x00" "\xE2\x80\xAE" "\xE0\xA5\x8D" "\xE2\x80\x8C" "\xE0\xA5\x8E"),               /* RLO virama ZWNJ U+094E */
    SEED("\x02\x00" "\xE0\xA4\x95" "\xE2\x80\xAE" "\xE0\xA5\x8D" "\xE2\x80\x8C" "\xE0\xA5\x8E"), /* ka RLO virama ZWNJ U+094E, RTL */
    SEED("\x0C\x18" "\xE2\x80\xAE" "\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87"
         " a " "\xE0\xA5\x8D" "\xE2\x80\x8C" "\xE0\xA5\x8E" "\xFF" "\x00\x00\x86\x06\x07\x08\x06"),  /* RLO namaste, wrapped; the editor */
    SEED("\x08\x00" "\xE2\x80\xAD" "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7" " " "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D"
         "\xFF" "\x01\x01\x05\x06\x06"),                                                        /* LRO Arabic Hebrew; the editor */
    SEED("\x01\x00" "\xE2\x80\xAB" "\xE0\xA4\x95" "\xE2\x80\xAC" " " "\xE2\x81\xA7" "\xD7\x90" "\xE2\x81\xA9" " "
         "\xE2\x80\x8F" "\xE0\xA5\x8B" "\xE2\x80\x8E"),                                     /* RLE PDF RLI PDI RLM LRM, LTR */
    SEED("\x0A\x00" "a\n" "\xE2\x80\xAE" "\xE0\xA5\x8D" "\xE2\x80\x8C" "\xE0\xA5\x8E" "\nb" "\xFF" "\x00\x07\x07\x08\x06"), /* paragraphs, RTL; the editor */
};

#if defined(YSP_FUZZ_SELF)
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t mix(void) {
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Codepoints that reach bidi, script and shaping code, as UTF-8. */
static int pick(uint8_t* o) {
    static const uint32_t ctl[] = { 0x202A, 0x202B, 0x202C, 0x202D, 0x202E, 0x2066, 0x2067, 0x2068, 0x2069, 0x200E, 0x200F,
                                    0x200C, 0x200D, 0x20, 0x0A, 0x09, '1', 'a', 0x25CC };
    const uint64_t r = mix() % 8;
    const uint32_t c = r == 0 ? ctl[mix() % (sizeof ctl / sizeof ctl[0])] : r < 4 ? 0x0900 + (uint32_t)(mix() % 0x80)
                     : r == 4 ? 0x0600 + (uint32_t)(mix() % 0x100) : r == 5 ? 0x0591 + (uint32_t)(mix() % 0x63)
                     : r == 6 ? ctl[mix() % 5] : (uint32_t)(0x20 + mix() % 0x5F);
    if (c < 0x80) { o[0] = (uint8_t)c; return 1; }
    if (c < 0x800) { o[0] = (uint8_t)(0xC0 | c >> 6); o[1] = (uint8_t)(0x80 | (c & 63)); return 2; }
    o[0] = (uint8_t)(0xE0 | c >> 12); o[1] = (uint8_t)(0x80 | ((c >> 6) & 63)); o[2] = (uint8_t)(0x80 | (c & 63));
    return 3;
}

int main(int argc, char** argv) {
    static uint8_t buf[MAX_INPUT];
    const int n_seeds = (int)(sizeof g_seeds / sizeof g_seeds[0]);
    long iters = argc > 1 ? atol(argv[1]) : 10000, it;
    int i;
    if (argc > 1 && !strcmp(argv[1], "--replay")) {
        for (i = 2; i < argc; i++) {
            FILE* fp = fopen(argv[i], "rb");
            size_t n;
            if (!fp) { fprintf(stderr, "cannot read %s\n", argv[i]); return 1; }
            n = fread(buf, 1, sizeof buf, fp);
            fclose(fp);
            one(buf, n);
        }
        printf("layout_fuzz: replayed %d files\n", argc - 2);
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "--write-seeds")) {
        for (i = 0; i < n_seeds; i++) {
            char path[1024];
            FILE* fp;
            snprintf(path, sizeof path, "%s/seed_%02d", argv[2], i);
            if (!(fp = fopen(path, "wb")) || fwrite(g_seeds[i].d, 1, (size_t)g_seeds[i].n, fp) != (size_t)g_seeds[i].n) {
                fprintf(stderr, "cannot write %s\n", path);
                return 1;
            }
            fclose(fp);
        }
        printf("layout_fuzz: wrote %d seeds\n", n_seeds);
        return 0;
    }
    if (argc > 2) g_rng = (uint64_t)strtoull(argv[2], NULL, 10);
    for (i = 0; i < n_seeds; i++) one((const uint8_t*)g_seeds[i].d, (size_t)g_seeds[i].n);
    for (it = 0; it < iters; it++) {
        const int s = (int)(mix() % (uint64_t)n_seeds), edits = 1 + (int)(mix() % 6);
        size_t n = (size_t)g_seeds[s].n;
        int e;
        memcpy(buf, g_seeds[s].d, n);
        for (e = 0; e < edits; e++) {
            const size_t at = 2 + (n > 2 ? (size_t)(mix() % (n - 2)) : 0);
            uint8_t cp[3];
            const int m = pick(cp);
            switch (mix() % 5) {
            case 0: buf[0] = (uint8_t)mix(); buf[1] = (uint8_t)mix(); break;
            case 1: if (n + (size_t)m < sizeof buf) { memmove(buf + at + m, buf + at, n - at); memcpy(buf + at, cp, (size_t)m); n += (size_t)m; } break;
            case 2: if (at < n) { memmove(buf + at, buf + at + 1, n - at - 1); n--; } break;   /* may cut a codepoint */
            case 3: if (at < n) buf[at] = (uint8_t)mix(); break;
            default: if (at + (size_t)m <= n) memcpy(buf + at, cp, (size_t)m); break;
            }
        }
        one(buf, n);
    }
    printf("layout_fuzz: %ld inputs, %ld layouts refused, %ld editor keys\n", g_inputs, g_refused, g_edits);
    ylay_block_free(&g_b);
    ylay_destroy(g_L);
    yol_close(&g_ol);
    return 0;
}
#else
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    one(data, size);
    return 0;
}
#endif
