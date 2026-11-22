/* layout_test.c - checks of pack/layout (docs/layout.md, Checks).
 *
 *   1. The versions the glue logs: Skribidi's commit, the ten patch
 *      hashes, the HarfBuzz version.
 *   2. Visual order on every platform: a TrueType font written here (a box
 *      for every ASCII, Hebrew and Arabic codepoint) and the strings that
 *      hit Skribidi's run-merge bug (docs/layout_probe.md 2.3). Each line's
 *      items, by x, must visit the string's bidi segments in the order the
 *      Unicode bidi algorithm gives, each segment's clusters ascending (LTR)
 *      or descending (RTL), and every non-space codepoint must have a glyph.
 *      Unpatched Skribidi fails all three strings.
 *   3. The corpus against Chromium: tests/layout/corpus.txt laid out with
 *      the system fonts gives the glyph ids, in visual order, that Edge put
 *      in its PDF (tests/layout/corpus_ref.txt, made by layout_ref.py), and
 *      positions within 2 font units: Chromium puts each bidi run on its
 *      1/64 px grid (0.32 font units at 100 px per em and 2048 units per em),
 *      so a bidi line drifts by a few of those; lines of one direction were
 *      exact. Only for fonts whose SHA-256 equals the reference's: another
 *      font version has other glyph ids.
 *   4. ygfx_cset_check() on every curve set the glue built.
 *   5. A second layout of the same text into the same block grows nothing;
 *      spans, wrapping and the stamp line.
 *   6. Editing against Edge (tests/layout/editor_ref.txt, made by
 *      editor_ref.py): line starts and run order at 10 widths per item for
 *      ylay_layout() and Skribidi's editor, each line's ink extent, and on
 *      one line, and wrapped at two widths, every selection's rectangles and
 *      every caret. All must equal Edge's (docs/layout.md).
 *   7. Font fallback (Skribidi patch 0003), with fonts written here of one
 *      script each: digits and punctuation in Hebrew and Arabic text come
 *      from the Latin font in either font order, with no glyph 0, in bidi
 *      order, on one baseline, the line with both fonts' metrics; a grapheme
 *      cluster moves whole when a font has it, else only the codepoint; a
 *      ZWJ stays; a character no font has is glyph 0, and
 *      ylay_missing_chars() names it with its run's script.
 *   8. Emoji (v0.3.0, Skribidi patch 0004), with fonts written here
 *      (tests/adapt/colr_font_build.h): a COLR font takes emoji runs,
 *      VS16 and ZWJ sequences in either font order; with no emoji font an
 *      emoji is glyph 0 and counted missing; with emoji fonts only, text
 *      is glyph 0 and counted; the first emoji font added wins; an emoji
 *      the emoji fonts lack comes from a text font that has it, and a
 *      text character the text fonts lack from an emoji font; a font of
 *      CBDT, sbix or SVG color glyphs without COLR is refused.
 *   9. Color glyphs (v0.3.0), with a COLR version 0 and a version 1 font
 *      written here: layers and paint records (solids premultiplied in
 *      linear light; linear, radial, two-point and elliptical gradients by
 *      their parameter at known points; group opacity as per-layer alpha),
 *      transforms baked into layer glyphs and a clip box cut into one (by
 *      their boxes in the set), the set's check, the same result from a
 *      second flattening, a layout's paint run, and the twelve refusals by
 *      name, a layout's with the codepoint and glyph.
 *  10. Threads (v0.3.5, Skribidi patch 0010): four threads make and
 *      destroy font and attribute collections, libraries and the versions
 *      at once; every collection gets an id of its own. Run under
 *      ThreadSanitizer, it also checks that they share no write.
 * Exit 0 on pass, 1 on a failure. YSP_FONTS_DIR overrides the font folder
 * (YLAY_TEST_FONTS, C:/Windows/Fonts by default).
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
/* ysp/gfx.h first: ysp/rt.h's implementation sets the POSIX feature macros,
 * which must come before any system header. */
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"
#include "ysp/layout.h"
#include "skribidi/skb_editor.h"
#include "skribidi/skb_font_collection.h"
#include "skribidi/skb_attribute_collection.h"
#include <hb.h>   /* hb_version_string(): the versions' HarfBuzz is the linked one */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../adapt/colr_font_build.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifndef YLAY_TEST_DIR
#define YLAY_TEST_DIR "tests/layout"
#endif
#ifndef YLAY_TEST_FONTS
#define YLAY_TEST_FONTS "C:/Windows/Fonts"
#endif

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
/* CHECK as an expression: the condition, for the checks that follow it */
static int check_n(int ok, int line, const char* fmt, ...) {
    va_list ap;
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL %s:%d: ", __FILE__, line);
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
    }
    return ok;
}
#define CHECK_N(cond, ...) check_n((cond) ? 1 : 0, __LINE__, __VA_ARGS__)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --- a font written here ---------------------------------------------------- */

typedef struct bb { uint8_t* d; size_t n, cap; } bb;
static void b_need(bb* b, size_t k) {
    if (b->n + k > b->cap) {
        while (b->n + k > b->cap) b->cap = b->cap ? 2 * b->cap : 256;
        b->d = (uint8_t*)realloc(b->d, b->cap);
        if (!b->d) { printf("out of memory\n"); exit(1); }
    }
}
static void b8(bb* b, uint32_t v) { b_need(b, 1); b->d[b->n++] = (uint8_t)v; }
static void b16(bb* b, uint32_t v) { b8(b, v >> 8 & 255); b8(b, v & 255); }
static void b32(bb* b, uint32_t v) { b16(b, v >> 16); b16(b, v & 0xffff); }
static void bpad4(bb* b) { while (b->n & 3) b8(b, 0); }

/* Codepoint ranges, each mapped by delta to consecutive glyphs from 2 on.
 * Glyph 1 is the space (empty); every other glyph is a box. '@' and '`' are
 * left out: Skribidi gives a font a script only when a run of its cmap starts
 * or ends with a character of that script, and ' ' to '~' in one run starts
 * and ends with Common characters. */
static const uint32_t font_ranges[][2] = {
    { 0x21, 0x3f }, { 0x41, 0x5f }, { 0x61, 0x7e }, { 0x05b0, 0x05ea }, { 0x0600, 0x06ff },
};
#define N_RANGES 5

/* A font of the ranges rg[0 .. nr - 1] (sorted, below 0xFFFF), ascender asc
 * and descender desc (font units, desc < 0, 1000 units per em). */
static void make_font_r(bb* out, const uint32_t (*rg)[2], uint32_t nr, int asc, int desc) {
    bb t[7];
    static const char* tags[7] = { "cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp" };
    uint32_t ng = 2, g, i, r;
    size_t dir;
    memset(t, 0, sizeof t);
    for (r = 0; r < nr; r++) ng += rg[r][1] - rg[r][0] + 1;
    /* cmap: format 4, the space, the ranges, 0xFFFF */
    {
        bb* c = &t[0];
        uint32_t nseg = nr + 2, base = 2, sr = 1, es = 0;
        while (sr * 2 <= nseg) { sr *= 2; es++; }
        b16(c, 0); b16(c, 1); b16(c, 3); b16(c, 1); b32(c, 12);
        b16(c, 4); b16(c, 16 + 8 * nseg); b16(c, 0);
        b16(c, 2 * nseg); b16(c, 2 * sr); b16(c, es); b16(c, 2 * nseg - 2 * sr);
        b16(c, 0x20);
        for (r = 0; r < nr; r++) b16(c, rg[r][1]);
        b16(c, 0xffff); b16(c, 0);
        b16(c, 0x20);
        for (r = 0; r < nr; r++) b16(c, rg[r][0]);
        b16(c, 0xffff);
        b16(c, (1 - 0x20) & 0xffff);
        for (r = 0; r < nr; r++) {
            b16(c, (base - rg[r][0]) & 0xffff);
            base += rg[r][1] - rg[r][0] + 1;
        }
        b16(c, 1);
        for (i = 0; i < nseg; i++) b16(c, 0);
    }
    /* glyf and loca: glyphs 0 and 1 empty, the rest a box 50..450 x 0..700 */
    for (g = 0; g < ng; g++) {
        b32(&t[5], (uint32_t)t[1].n);
        if (g >= 2) {
            bb* q = &t[1];
            b16(q, 1); b16(q, 50); b16(q, 0); b16(q, 450); b16(q, 700);
            b16(q, 3); b16(q, 0);                       /* end point, no instructions */
            /* on-curve points (50,0) (450,0) (450,700) (50,700) as word deltas;
             * flag 0x10 or 0x20 without the short bit: no change in x or y */
            b8(q, 0x01 | 0x20);
            b8(q, 0x01 | 0x20);
            b8(q, 0x01 | 0x10);
            b8(q, 0x01 | 0x20);
            b16(q, 50); b16(q, 400); b16(q, (uint32_t)(-400 & 0xffff));
            b16(q, 700);
            bpad4(q);
        }
    }
    b32(&t[5], (uint32_t)t[1].n);
    /* head (long loca), hhea, hmtx, maxp */
    for (i = 0; i < 54; i++) b8(&t[2], 0);
    t[2].d[18] = 1000 >> 8; t[2].d[19] = 1000 & 255; t[2].d[51] = 1;
    for (i = 0; i < 36; i++) b8(&t[3], 0);
    t[3].d[1] = 1;                                      /* version 1.0: HarfBuzz reads no metrics from another */
    t[3].d[4] = (uint8_t)((uint32_t)asc >> 8); t[3].d[5] = (uint8_t)asc;
    t[3].d[6] = (uint8_t)((uint32_t)desc >> 8); t[3].d[7] = (uint8_t)desc;
    t[3].d[34] = (uint8_t)(ng >> 8); t[3].d[35] = (uint8_t)ng;
    for (g = 0; g < ng; g++) { b16(&t[4], 500); b16(&t[4], g >= 2 ? 50 : 0); }
    b32(&t[6], 0x00005000); b16(&t[6], ng);
    /* the directory, tables sorted by tag */
    out->n = 0;
    b32(out, 0x00010000); b16(out, 7); b16(out, 64); b16(out, 2); b16(out, 48);
    dir = out->n;
    for (i = 0; i < 7; i++) { b32(out, 0); b32(out, 0); b32(out, 0); b32(out, 0); }
    for (i = 0; i < 7; i++) {
        size_t off;
        bpad4(out);
        off = out->n;
        b_need(out, t[i].n);
        memcpy(out->d + out->n, t[i].d, t[i].n);
        out->n += t[i].n;
        memcpy(out->d + dir + 16 * i, tags[i], 4);
        out->d[dir + 16 * i + 8] = (uint8_t)(off >> 24); out->d[dir + 16 * i + 9] = (uint8_t)(off >> 16);
        out->d[dir + 16 * i + 10] = (uint8_t)(off >> 8); out->d[dir + 16 * i + 11] = (uint8_t)off;
        out->d[dir + 16 * i + 12] = (uint8_t)(t[i].n >> 24); out->d[dir + 16 * i + 13] = (uint8_t)(t[i].n >> 16);
        out->d[dir + 16 * i + 14] = (uint8_t)(t[i].n >> 8); out->d[dir + 16 * i + 15] = (uint8_t)t[i].n;
        free(t[i].d);
    }
    bpad4(out);
}

static void make_font(bb* out) { make_font_r(out, font_ranges, N_RANGES, 800, -200); }

/* --- visual order ------------------------------------------------------------- */

/* UTF-8 to codepoints; returns the count. */
static int utf32(const char* s, uint32_t* out, int cap) {
    const unsigned char* p = (const unsigned char*)s;
    int n = 0;
    while (*p && n < cap) {
        uint32_t c = *p++;
        if (c >= 0xf0) { c = (c & 7) << 18; c |= (uint32_t)(*p++ & 0x3f) << 12; c |= (uint32_t)(*p++ & 0x3f) << 6; c |= *p++ & 0x3f; }
        else if (c >= 0xe0) { c = (c & 15) << 12; c |= (uint32_t)(*p++ & 0x3f) << 6; c |= *p++ & 0x3f; }
        else if (c >= 0xc0) { c = (c & 31) << 6; c |= *p++ & 0x3f; }
        out[n++] = c;
    }
    return n;
}

static const ylay_block* g_sort_block;
static int by_x(const void* a, const void* b) {
    const ygfx_citem* A = &g_sort_block->items[*(const uint32_t*)a];
    const ygfx_citem* B = &g_sort_block->items[*(const uint32_t*)b];
    if (A->x != B->x) return A->x < B->x ? -1 : 1;
    if (A->y != B->y) return A->y < B->y ? -1 : 1;
    return A->glyph < B->glyph ? -1 : A->glyph > B->glyph;
}

/* A segment of codepoints [start, end) of one direction; an order case lists
 * them in visual order, left to right. */
typedef struct seg { int start, end, rtl; } seg;

typedef struct order_case {
    const char* name;
    const char* text;
    int         dir;
    int         n_segs;
    seg         segs[6];
} order_case;

/* Checks one single-line block against the segments. Returns the count of
 * items out of order plus codepoints with no glyph. */
static int check_order(const order_case* oc, const ylay_block* b, int verbose) {
    static uint32_t idx[512], cps[512];
    uint8_t seen[512];
    int n = (int)b->n_items, ncp = utf32(oc->text, cps, 512), i, cur = 0, bad = 0;
    int last = -1;
    if (n > 512) return 1;
    for (i = 0; i < n; i++) idx[i] = (uint32_t)i;
    g_sort_block = b;
    qsort(idx, (size_t)n, sizeof idx[0], by_x);
    memset(seen, 0, sizeof seen);
    for (i = 0; i < n; i++) {
        int c = (int)b->cluster[idx[i]], s;
        if (c < ncp) seen[c] = 1;
        for (s = cur; s < oc->n_segs; s++)
            if (c >= oc->segs[s].start && c < oc->segs[s].end) break;
        if (s == oc->n_segs) {
            if (verbose) printf("  %s: item %d (cluster %d) is out of segment order (in segment %d or earlier)\n", oc->name, i, c, cur);
            bad++;
            continue;
        }
        if (s != cur) { cur = s; last = -1; }
        if (last >= 0 && (oc->segs[s].rtl ? c > last : c < last)) {
            if (verbose) printf("  %s: item %d (cluster %d) runs against its segment's direction\n", oc->name, i, c);
            bad++;
        }
        last = c;
    }
    /* every non-space codepoint starts a cluster with a glyph or lies inside
     * one (a mark): find the cluster start at or before it */
    for (i = 0; i < ncp; i++) {
        int k = i;
        if (cps[i] == ' ') continue;
        while (k > 0 && !seen[k] && cps[k] != ' ') k--;
        if (!seen[k]) {
            if (verbose) printf("  %s: codepoint %d (U+%04X) has no glyph\n", oc->name, i, (unsigned)cps[i]);
            bad++;
        }
    }
    return bad;
}

/* Positions of a substring in codepoints. */
static int cp_find(const char* text, const char* sub) {
    const char* p = strstr(text, sub);
    uint32_t tmp[512];
    char pre[1024];
    size_t k;
    if (!p) return -1;
    k = (size_t)(p - text);
    if (k >= sizeof pre) return -1;
    memcpy(pre, text, k);
    pre[k] = 0;
    return utf32(pre, tmp, 512);
}
static int cp_len(const char* s) { uint32_t tmp[512]; return utf32(s, tmp, 512); }

/* The bug strings and the editor's caret string (layout_probe.md 2.3, 6). */
#define AR_PHRASE "\xd9\x85\xd9\x8e\xd8\xb1\xd9\x92\xd8\xad\xd9\x8e\xd8\xa8\xd9\x8b\xd8\xa7 \xd8\xa8\xd9\x90\xd8\xa7\xd9\x84\xd9\x92\xd8\xb9\xd9\x8e\xd8\xa7\xd9\x84\xd9\x8e\xd9\x85\xd9\x90"
#define AR_MA     "\xd9\x85\xd8\xb9"
#define AR_WA     "\xd9\x88"
#define AR_MARHABA "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7"
static const char* S1 = "The word " AR_PHRASE " 123 means hello world.";
static const char* S2 = AR_MA " English " AR_WA " 123";
static const char* S3 = "abc " AR_MARHABA " 123 def";

static int n_order_cases(order_case* oc) {
    int n123, nend, nE, nEend;
    /* S1, LTR paragraph: "The word ", then the number (level 2), then the
     * Arabic phrase and the space before the number, reversed (level 1),
     * then the rest */
    n123 = cp_find(S1, "123"); nend = cp_len(S1);
    oc[0] = (order_case){ "LTR paragraph, Arabic phrase and number", S1, YLAY_DIR_LTR, 4,
        { { 0, 9, 0 }, { n123, n123 + 3, 0 }, { 9, n123, 1 }, { n123 + 3, nend, 0 } } };
    /* S2, RTL paragraph: the line reversed; "English" and "123" stay LTR */
    nE = cp_find(S2, "English"); nEend = nE + 7; n123 = cp_find(S2, "123"); nend = cp_len(S2);
    oc[1] = (order_case){ "RTL paragraph, Latin word and number", S2, YLAY_DIR_RTL, 4,
        { { n123, nend, 0 }, { nEend, n123, 1 }, { nE, nEend, 0 }, { 0, nE, 1 } } };
    n123 = cp_find(S3, "123"); nend = cp_len(S3);
    oc[2] = (order_case){ "caret string", S3, YLAY_DIR_LTR, 4,
        { { 0, 4, 0 }, { n123, n123 + 3, 0 }, { 4, n123, 1 }, { n123 + 3, nend, 0 } } };
    return 3;
}

/* --- font fallback ------------------------------------------------------------ */

/* Fonts written here, each with one script or none, so the run's script
 * picks one font and the fallback must find the others. QUIRK has the
 * ASCII numerals, the combining marks and Hebrew, but no Hebrew script for
 * Skribidi: each of its ranges starts and ends with characters of no
 * script or of Common or Inherited (U+0590 and U+05FF are unassigned). */
static const uint32_t fr_he[][2]     = { { 0x05b0, 0x05ea } };
static const uint32_t fr_hel[][2]    = { { 0x05d0, 0x05ea } };
static const uint32_t fr_la[][2]     = { { 0x21, 0x3f }, { 0x41, 0x5f }, { 0x61, 0x7e } };
static const uint32_t fr_ar[][2]     = { { 0x0600, 0x06ff } };
static const uint32_t fr_acute[][2]  = { { 0x0300, 0x036f } };
static const uint32_t fr_lacute[][2] = { { 0x21, 0x3f }, { 0x41, 0x5f }, { 0x61, 0x7e }, { 0x0300, 0x036f } };
static const uint32_t fr_quirk[][2]  = { { 0x30, 0x39 }, { 0x0300, 0x036f }, { 0x0590, 0x05ff } };
static const uint32_t fr_lazw[][2]   = { { 0x21, 0x3f }, { 0x41, 0x5f }, { 0x61, 0x7e }, { 0x200c, 0x200d } };

typedef struct fb_font { const char* name; const uint32_t (*rg)[2]; uint32_t nr; int asc, desc; } fb_font;
enum { FB_HE, FB_HEL, FB_LA, FB_AR, FB_ACUTE, FB_LACUTE, FB_QUIRK, FB_LAZW, FB_N };
static const fb_font fb_fonts[FB_N] = {
    { "he", fr_he, 1, 800, -200 }, { "hel", fr_hel, 1, 800, -200 },
    /* the Latin fonts are taller, so a line's metrics show which fonts it used */
    { "la", fr_la, 3, 900, -300 }, { "ar", fr_ar, 1, 800, -200 }, { "acute", fr_acute, 1, 800, -200 },
    { "lacute", fr_lacute, 4, 900, -300 }, { "quirk", fr_quirk, 3, 800, -200 }, { "lazw", fr_lazw, 4, 900, -300 },
};

/* The glyph of cp in a written font: the space is 1, the ranges count from 2. */
static uint32_t fb_gid(int font, uint32_t cp) {
    const fb_font* f = &fb_fonts[font];
    uint32_t r, g = 2;
    if (cp == ' ') return 1;
    for (r = 0; r < f->nr; r++) {
        if (cp >= f->rg[r][0] && cp <= f->rg[r][1]) return g + cp - f->rg[r][0];
        g += f->rg[r][1] - f->rg[r][0] + 1;
    }
    return 0;
}

/* Lays text out with the fonts list[0 .. n - 1] (indexes into fb_fonts), in
 * that order, into b; font_of[k] is the fb_fonts index of item k's font. */
static int fb_layout(yol_ctx* ol, bb* fonts, const int* list, int n, const char* text, ylay_lib** L, ylay_block* b, int* font_of) {
    int i;
    uint32_t r, k;
    if (ylay_create(L, &(ylay_desc){ .ol = ol }) != YLAY_OK) return -1;
    for (i = 0; i < n; i++)
        if (ylay_add_font(*L, fonts[list[i]].d, fonts[list[i]].n, 0, fb_fonts[list[i]].name) != i) return -1;
    if (ylay_layout(*L, text, -1, &(ylay_style){ .size = 100 }, NULL, 0, b) != YLAY_OK) return -1;
    for (r = 0; r < b->n_runs; r++)
        for (k = b->runs[r].first; k < b->runs[r].first + b->runs[r].n && k < 512; k++) font_of[k] = list[b->runs[r].font];
    return 0;
}

#define HE_SHALOM "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d"
#define AR_MARHABA_ "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7"

static void fallback_checks(yol_ctx* ol) {
    bb fonts[FB_N];
    ylay_lib* L = NULL;
    ylay_block b;
    int font_of[512], f, t, o;
    uint32_t cps[512], k;
    memset(&b, 0, sizeof b);
    for (f = 0; f < FB_N; f++) {
        memset(&fonts[f], 0, sizeof fonts[f]);
        make_font_r(&fonts[f], fb_fonts[f].rg, fb_fonts[f].nr, fb_fonts[f].asc, fb_fonts[f].desc);
    }

    /* Digits and punctuation in Hebrew and Arabic text, both font orders: the
     * letters (and U+060C) from the script's font, the digits and Latin
     * punctuation from the Latin font, the spaces from the script's font (the
     * run's font keeps what it has), no glyph 0, the bidi order kept, and one
     * baseline whose line has the taller Latin font's metrics. */
    {
        static const struct { const char* text; int script_font; int dir_rtl; } cases[] = {
            { HE_SHALOM " 123", FB_HE, 1 }, { "123 " HE_SHALOM, FB_HE, 1 }, { HE_SHALOM " 12:30, 4.5%", FB_HE, 1 },
            { AR_MARHABA_ " 123\xd8\x8c 4.5%", FB_AR, 1 },
        };
        for (t = 0; t < (int)(sizeof cases / sizeof cases[0]); t++)
            for (o = 0; o < 2; o++) {
                int list[2], ncp, bad = 0;
                list[o] = cases[t].script_font;
                list[1 - o] = FB_LA;
                ncp = utf32(cases[t].text, cps, 512);
                if (fb_layout(ol, fonts, list, 2, cases[t].text, &L, &b, font_of) < 0) { CHECK(0, "fallback %d/%d: %s", t, o, L ? ylay_error(L) : ""); }
                else {
                    CHECK(b.n_missing == 0, "fallback '%s' fonts [%s, %s]: %u glyphs 0", cases[t].text, fb_fonts[list[0]].name, fb_fonts[list[1]].name, b.n_missing);
                    CHECK(b.n_lines == 1 && b.dir == (cases[t].dir_rtl ? YLAY_DIR_RTL : YLAY_DIR_LTR), "fallback '%s': %u lines, dir %d", cases[t].text, b.n_lines, b.dir);
                    for (k = 0; k < b.n_items; k++) {
                        uint32_t c = b.cluster[k] < (uint32_t)ncp ? cps[b.cluster[k]] : 0;
                        int want = (c >= 0x21 && c <= 0x7e) ? FB_LA : cases[t].script_font;
                        if (font_of[k] != want || (uint32_t)b.items[k].glyph != fb_gid(want, c) || b.items[k].y != b.items[0].y) {
                            if (!bad) printf("  fallback '%s' fonts [%s, %s]: U+%04X in %s glyph %g at y %g, want %s glyph %u at y %g\n", cases[t].text,
                                             fb_fonts[list[0]].name, fb_fonts[list[1]].name, (unsigned)c, fb_fonts[font_of[k]].name,
                                             (double)b.items[k].glyph, (double)b.items[k].y, fb_fonts[want].name, (unsigned)fb_gid(want, c), (double)b.items[0].y);
                            bad++;
                        }
                    }
                    CHECK(bad == 0, "fallback '%s' fonts [%s, %s]: %d items in the wrong font, glyph or baseline", cases[t].text,
                          fb_fonts[list[0]].name, fb_fonts[list[1]].name, bad);
                    CHECK(b.n_lines == 1 && fabsf(b.lines[0].ascent - 90.0f) < 1e-3f && fabsf(b.lines[0].descent - 30.0f) < 1e-3f,
                          "fallback '%s': line ascent %g, descent %g, want the Latin font's 90, 30", cases[t].text,
                          (double)b.lines[0].ascent, (double)b.lines[0].descent);
                }
                ylay_destroy(L);
                L = NULL;
            }
        /* the bidi order of the first two, as the order checks above do */
        {
            int n1 = cp_len(HE_SHALOM " 123"), list[2] = { FB_HE, FB_LA };
            order_case oc1 = { "fallback, Hebrew then number", HE_SHALOM " 123", YLAY_DIR_AUTO, 2, { { 5, n1, 0 }, { 0, 5, 1 } } };
            order_case oc2 = { "fallback, number then Hebrew", "123 " HE_SHALOM, YLAY_DIR_AUTO, 2, { { 3, n1, 1 }, { 0, 3, 0 } } };
            if (fb_layout(ol, fonts, list, 2, oc1.text, &L, &b, font_of) == 0) CHECK(check_order(&oc1, &b, 1) == 0, "%s: out of bidi order", oc1.name);
            ylay_destroy(L);
            if (fb_layout(ol, fonts, list, 2, oc2.text, &L, &b, font_of) == 0) CHECK(check_order(&oc2, &b, 1) == 0, "%s: out of bidi order", oc2.name);
            ylay_destroy(L);
            L = NULL;
        }
        /* text the script's font covers keeps its own metrics: no fallback font
         * enters the line */
        {
            int list[2] = { FB_HE, FB_LA };
            if (fb_layout(ol, fonts, list, 2, HE_SHALOM " " HE_SHALOM, &L, &b, font_of) == 0)
                CHECK(b.n_runs == 1 && font_of[0] == FB_HE && fabsf(b.lines[0].ascent - 80.0f) < 1e-3f,
                      "Hebrew alone: %u runs, ascent %g (want 1 run in he, 80)", b.n_runs, (double)b.lines[0].ascent);
            ylay_destroy(L);
            L = NULL;
        }
    }

    /* The grapheme cluster is the unit. */
    {
        /* a numeral and an acute: neither in the Hebrew font; the Latin font
         * has the numeral, lacute both, so both come from lacute; the 6 after
         * them from la again, the first font that has it */
        int list[3] = { FB_HE, FB_LA, FB_LACUTE };
        const char* text = HE_SHALOM " 5\xcc\x81" "6";
        int n = 0;
        if (fb_layout(ol, fonts, list, 3, text, &L, &b, font_of) == 0) {
            for (k = 0; k < b.n_items; k++) {
                if (b.cluster[k] == 5) { n++; CHECK(font_of[k] == FB_LACUTE, "cluster 5+acute: item %u in %s, want lacute", k, fb_fonts[font_of[k]].name); }
                if (b.cluster[k] == 7) CHECK(font_of[k] == FB_LA, "the 6 after 5+acute: in %s, want la", fb_fonts[font_of[k]].name);
            }
            CHECK(n == 2 && b.n_missing == 0, "cluster 5+acute: %d items, %u glyphs 0 (want 2, 0)", n, b.n_missing);
        } else CHECK(0, "cluster 5+acute: %s", L ? ylay_error(L) : "");
        ylay_destroy(L);
        L = NULL;
    }
    {
        /* a numeral and a Hebrew point: he has the point, la the numeral,
         * quirk both; the point stays with its base in quirk, though the
         * run's font has it */
        int list[3] = { FB_HE, FB_LA, FB_QUIRK };
        const char* text = HE_SHALOM " 5\xd6\xb4";
        int n = 0;
        if (fb_layout(ol, fonts, list, 3, text, &L, &b, font_of) == 0) {
            for (k = 0; k < b.n_items; k++)
                if (b.cluster[k] >= 5) { n++; CHECK(font_of[k] == FB_QUIRK, "cluster 5+hiriq: item %u in %s, want quirk", k, fb_fonts[font_of[k]].name); }
            CHECK(n == 2 && b.n_missing == 0, "cluster 5+hiriq: %d items, %u glyphs 0 (want 2, 0)", n, b.n_missing);
        } else CHECK(0, "cluster 5+hiriq: %s", L ? ylay_error(L) : "");
        ylay_destroy(L);
        L = NULL;
    }
    {
        /* alef and an acute: the letter in hel, which lacks the acute; quirk
         * has both, so the cluster moves to it whole; the bet after it goes
         * back to hel */
        int list[2] = { FB_HEL, FB_QUIRK }, n0 = 0;
        const char* text = "\xd7\x90\xcc\x81\xd7\x91";
        if (fb_layout(ol, fonts, list, 2, text, &L, &b, font_of) == 0) {
            for (k = 0; k < b.n_items; k++) {
                int want = b.cluster[k] < 2 ? FB_QUIRK : FB_HEL;
                if (b.cluster[k] < 2) n0++;
                CHECK(font_of[k] == want, "cluster alef+acute: item %u (cluster %u) in %s, want %s", k, b.cluster[k], fb_fonts[font_of[k]].name, fb_fonts[want].name);
            }
            CHECK(n0 == 2 && b.n_missing == 0, "cluster alef+acute: %d items of the cluster, %u glyphs 0 (want 2, 0)", n0, b.n_missing);
        } else CHECK(0, "cluster alef+acute: %s", L ? ylay_error(L) : "");
        ylay_destroy(L);
        L = NULL;
    }
    {
        /* no font has alef and the acute together: the acute alone from the
         * first font that has it, the letters in hel */
        int list[3] = { FB_HEL, FB_LA, FB_ACUTE };
        const char* text = "\xd7\x90\xcc\x81\xd7\x91";
        if (fb_layout(ol, fonts, list, 3, text, &L, &b, font_of) == 0) {
            for (k = 0; k < b.n_items; k++) {
                int want = b.cluster[k] == 1 ? FB_ACUTE : FB_HEL;
                CHECK(font_of[k] == want, "split alef+acute: item %u (cluster %u) in %s, want %s", k, b.cluster[k], fb_fonts[font_of[k]].name, fb_fonts[want].name);
            }
            CHECK(b.n_items == 3 && b.n_missing == 0, "split alef+acute: %u items, %u glyphs 0 (want 3, 0)", b.n_items, b.n_missing);
        } else CHECK(0, "split alef+acute: %s", L ? ylay_error(L) : "");
        ylay_destroy(L);
        L = NULL;
    }
    {
        /* a ZWJ between Arabic letters: the Arabic font lacks it and lazw has
         * it, but it needs no glyph (HarfBuzz hides it), so it stays and the
         * run is not split */
        int list[2] = { FB_AR, FB_LAZW };
        const char* text = "\xd9\x84\xe2\x80\x8d\xd9\x84";
        if (fb_layout(ol, fonts, list, 2, text, &L, &b, font_of) == 0) {
            int others = 0;
            for (k = 0; k < b.n_items; k++) others += font_of[k] != FB_AR;
            CHECK(b.n_runs == 1 && others == 0 && b.n_missing == 0, "ZWJ: %u runs, %d items not in ar, %u glyphs 0 (want 1, 0, 0)", b.n_runs, others, b.n_missing);
        } else CHECK(0, "ZWJ: %s", L ? ylay_error(L) : "");
        ylay_destroy(L);
        L = NULL;
    }

    /* What no font has: glyph 0, named with its run's script. */
    {
        static const struct { const char* text; uint32_t cp; const char* script; } cases[] = {
            { HE_SHALOM " \xe2\x98\x83 12", 0x2603, "Hebr" },   /* a snowman in Hebrew text */
            { "abc \xe2\x98\x83", 0x2603, "Latn" },
            { "a\xcc\x81", 0x0301, "Latn" },                     /* the acute, not its base */
        };
        int list[2] = { FB_HE, FB_LA };
        for (t = 0; t < (int)(sizeof cases / sizeof cases[0]); t++) {
            ylay_missing m[4];
            int nm;
            if (fb_layout(ol, fonts, list, 2, cases[t].text, &L, &b, font_of) < 0) { CHECK(0, "unmapped %d: %s", t, L ? ylay_error(L) : ""); }
            else {
                nm = ylay_missing_chars(L, m, 4);
                CHECK(b.n_missing == 1 && nm == 1 && m[0].codepoint == cases[t].cp && !strcmp(m[0].script, cases[t].script) && m[0].in_a_font == 0,
                      "unmapped '%s': %u glyphs 0, %d characters, first U+%04X in %s (in a font %d); want 1, 1, U+%04X in %s", cases[t].text,
                      b.n_missing, nm, nm > 0 ? (unsigned)m[0].codepoint : 0u, nm > 0 ? m[0].script : "-", nm > 0 ? m[0].in_a_font : -1,
                      (unsigned)cases[t].cp, cases[t].script);
            }
            ylay_destroy(L);
            L = NULL;
        }
        /* the list empties with the next call on the same library */
        if (fb_layout(ol, fonts, list, 2, "abc \xe2\x98\x83", &L, &b, font_of) == 0) {
            CHECK(ylay_missing_chars(L, NULL, 0) == 1, "the snowman is not listed");
            CHECK(ylay_layout(L, "abc", -1, &(ylay_style){ .size = 100 }, NULL, 0, &b) == YLAY_OK && ylay_missing_chars(L, NULL, 0) == 0,
                  "the missing list was not emptied by a layout without glyph 0");
        }
        ylay_destroy(L);
        L = NULL;
    }
    ylay_block_free(&b);
    for (f = 0; f < FB_N; f++) free(fonts[f].d);
    printf("fallback: %d checks so far\n", checks);
}

/* --- the corpus against Chromium ------------------------------------------- */

/* The file with a newline at byte 0, so every line of a text file follows a
 * newline, and a NUL after it; bytes_of() gives the file itself. */
static uint8_t* read_file(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    uint8_t* d;
    long k;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); k = ftell(f); fseek(f, 0, SEEK_SET);
    d = k > 0 ? (uint8_t*)malloc((size_t)k + 2) : NULL;
    if (!d || fread(d + 1, 1, (size_t)k, f) != (size_t)k) { fclose(f); free(d); return NULL; }
    fclose(f);
    d[0] = '\n';
    d[k + 1] = 0;
    *n = (size_t)k;
    return d;
}
static const uint8_t* bytes_of(const uint8_t* d) { return d + 1; }

typedef struct sysfont { char file[64]; int face; uint8_t* bytes; size_t n; } sysfont;

/* One corpus line, "<id>|<file>|<face>|<dir>|<lang>|<text>", into buf. */
static int corpus_line(const char* corpus, const char* id, char* buf, size_t cap, int* dir, char* lang, size_t lang_cap, char** text) {
    char key[80], *fields[6];
    const char* q;
    const char* e;
    size_t len;
    int k;
    snprintf(key, sizeof key, "\n%s|", id);
    q = strstr(corpus, key);
    if (!q) return -1;
    q += 1;
    e = strchr(q, '\n');
    len = e ? (size_t)(e - q) : strlen(q);
    if (len >= cap) return -1;
    memcpy(buf, q, len); buf[len] = 0;
    if (len && buf[len - 1] == '\r') buf[len - 1] = 0;
    fields[0] = buf;
    for (k = 1; k < 6; k++) { fields[k] = strchr(fields[k - 1], '|'); if (!fields[k]) return -1; *fields[k]++ = 0; }
    *dir = strcmp(fields[3], "rtl") == 0 ? YLAY_DIR_RTL : strcmp(fields[3], "ltr") == 0 ? YLAY_DIR_LTR : YLAY_DIR_AUTO;
    snprintf(lang, lang_cap, "%s", fields[4]);
    *text = fields[5];
    return 0;
}

static void corpus(yol_ctx* ol) {
    const char* dir = getenv("YSP_FONTS_DIR");
    static char line[16384], buf[4096];
    char path[600];
    size_t n;
    uint8_t* corpus_txt;
    FILE* ref;
    sysfont fonts[16];
    int n_fonts = 0, matched = 0, skipped = 0, i;
    ylay_lib* L = NULL;
    ylay_block b;
    if (!dir) dir = YLAY_TEST_FONTS;
    snprintf(path, sizeof path, "%s/corpus.txt", YLAY_TEST_DIR);
    corpus_txt = read_file(path, &n);
    snprintf(path, sizeof path, "%s/corpus_ref.txt", YLAY_TEST_DIR);
    ref = fopen(path, "r");
    if (!corpus_txt || !ref) {
        printf("corpus: %s/corpus.txt or corpus_ref.txt missing: not checked\n", YLAY_TEST_DIR);
        free(corpus_txt);
        if (ref) fclose(ref);
        return;
    }
    memset(&b, 0, sizeof b);
    /* corpus_ref.txt: "item <id> <file> <face> <sha256> <n> gid:x:y ..." */
    while (fgets(line, sizeof line, ref)) {
        char id[64], file[64], sha[80], lang[32], *text, *p;
        int face, cnt, k, f, pos, dir_;
        ylay_font_info fi;
        if (strncmp(line, "item ", 5) != 0) continue;
        if (sscanf(line, "item %63s %63s %d %79s %d%n", id, file, &face, sha, &cnt, &pos) != 5) continue;
        p = line + pos;
        if (corpus_line((const char*)corpus_txt, id, buf, sizeof buf, &dir_, lang, sizeof lang, &text) < 0) {
            printf("FAIL corpus %s: not in corpus.txt, or malformed\n", id);
            failures++;
            continue;
        }
        /* each item lays out with its one font, as the reference page does */
        for (f = 0; f < n_fonts; f++) if (strcmp(fonts[f].file, file) == 0 && fonts[f].face == face) break;
        if (f == n_fonts) {
            if (n_fonts == 16) continue;
            snprintf(fonts[f].file, sizeof fonts[f].file, "%s", file);
            fonts[f].face = face;
            snprintf(path, sizeof path, "%s/%s", dir, file);
            fonts[f].bytes = read_file(path, &fonts[f].n);
            n_fonts++;
        }
        if (!fonts[f].bytes) { printf("corpus: %s: %s/%s not found, skipped\n", id, dir, file); skipped++; continue; }
        if (L) ylay_destroy(L);
        L = NULL;
        if (ylay_create(&L, &(ylay_desc){ .ol = ol }) != YLAY_OK || ylay_add_font(L, bytes_of(fonts[f].bytes), fonts[f].n, face, file) < 0) {
            printf("FAIL corpus %s: %s\n", id, L ? ylay_error(L) : "create failed");
            failures++;
            continue;
        }
        ylay_font(L, 0, &fi);
        if (strcmp(fi.sha256, sha) != 0) {
            printf("corpus: %s: %s is another version than the reference's (sha256 %.12s, reference %.12s): skipped\n",
                   id, file, fi.sha256, sha);
            skipped++;
            continue;
        }
        if (ylay_layout(L, text, -1, &(ylay_style){ .size = 100, .dir = dir_, .lang = lang }, NULL, 0, &b) != YLAY_OK) {
            printf("FAIL corpus %s: %s\n", id, ylay_error(L));
            failures++;
            continue;
        }
        /* visual order by x, then y, then id; positions in font units from
         * the first glyph's */
        {
            static uint32_t idx[2048];
            int m = (int)b.n_items, bad_id = 0;
            double dx = 0, dy = 0, x0 = 0, y0 = 0, s = fi.units_per_em / 100.0;
            if (m > 2048) m = 2048;
            for (k = 0; k < m; k++) idx[k] = (uint32_t)k;
            g_sort_block = &b;
            qsort(idx, (size_t)m, sizeof idx[0], by_x);
            checks++;
            if (m != cnt) {
                printf("FAIL corpus %s: %d glyphs, Chromium %d\n", id, m, cnt);
                failures++;
                continue;
            }
            for (k = 0; k < m; k++) {
                int gid, consumed;
                double rx, ry;
                const ygfx_citem* it = &b.items[idx[k]];
                if (sscanf(p, " %d:%lf:%lf%n", &gid, &rx, &ry, &consumed) != 3) { bad_id = 1; break; }
                p += consumed;
                if (k == 0) { x0 = it->x * s - rx; y0 = it->y * s - ry; }
                if ((int)it->glyph != gid) {
                    if (!bad_id) printf("FAIL corpus %s: glyph %d is %d, Chromium %d\n", id, k, (int)it->glyph, gid);
                    bad_id = 1;
                }
                if (fabs(it->x * s - x0 - rx) > dx) dx = fabs(it->x * s - x0 - rx);
                if (fabs(it->y * s - y0 - ry) > dy) dy = fabs(it->y * s - y0 - ry);
            }
            if (bad_id) failures++;
            else {
                printf("corpus %-16s %-12s %3d glyphs: same ids as Chromium, d %.2f / %.2f font units\n", id, file, m, dx, dy);
                CHECK(dx <= 2.0 && dy <= 2.0, "corpus %s: positions differ by %.2f / %.2f font units", id, dx, dy);
                matched++;
            }
        }
        {
            char msg[300];
            ygfx_cset_desc cd;
            memset(&cd, 0, sizeof cd);
            cd.texels = fi.set->texels; cd.n_texels = fi.set->n_texels; cd.words = fi.set->words; cd.n_words = fi.set->n_words;
            CHECK(ygfx_cset_check(&cd, msg, sizeof msg) == YGFX_OK, "corpus %s: curve set: %s", id, msg);
        }
    }
    fclose(ref);
    if (L) ylay_destroy(L);
    ylay_block_free(&b);
    for (i = 0; i < n_fonts; i++) free(fonts[i].bytes);
    free(corpus_txt);
    printf("corpus: %d items equal Chromium's, %d skipped\n", matched, skipped);
}

/* --- editing against Edge ---------------------------------------------------- */

/* tests/layout/editor_ref.txt (editor_ref.py): Edge's line starts and run
 * order at 10 widths per item for ylay_layout() and for Skribidi's
 * editor, and on one line its selection rectangles and carets. Font units,
 * x from the box's left edge; positions within ED_TOL count as equal (2 for
 * the glyph positions of 3., and Edge snaps each box to 1/64 px, 0.32 font
 * units at 100 px per em). */
#define ED_SIZE 100.0f
#define ED_TOL  4.0
#define ED_MAXN 512

typedef struct ed_item {
    char id[48], dir[8], lang[16];
    char fonts[4][48];
    int n_fonts;
    char text[1024];
} ed_item;

static int ed_items_read(const char* text, ed_item* it, int cap) {
    int n = 0;
    const char* p = text;
    while (*p && n < cap) {
        const char* e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[1400], *f[5];
        int k;
        if (len && len < sizeof line && p[0] != '#' && p[0] != '\n' && p[0] != '\r') {
            memcpy(line, p, len); line[len] = 0;
            if (line[len - 1] == '\r') line[len - 1] = 0;
            f[0] = line;
            for (k = 1; k < 5; k++) { f[k] = strchr(f[k - 1], '|'); if (!f[k]) break; *f[k]++ = 0; }
            if (k == 5) {
                char* q = f[1];
                memset(&it[n], 0, sizeof it[n]);
                snprintf(it[n].id, sizeof it[n].id, "%.47s", f[0]);
                snprintf(it[n].dir, sizeof it[n].dir, "%.7s", f[2]);
                snprintf(it[n].lang, sizeof it[n].lang, "%.15s", f[3]);
                snprintf(it[n].text, sizeof it[n].text, "%.1023s", f[4]);
                while (q && *q && it[n].n_fonts < 4) {
                    char* c = strchr(q, ',');
                    if (c) *c = 0;
                    snprintf(it[n].fonts[it[n].n_fonts++], 48, "%.47s", q);
                    q = c ? c + 1 : NULL;
                }
                n++;
            }
        }
        if (!e) break;
        p = e + 1;
    }
    return n;
}

/* Line starts and words per line in visual order, as editor_ref.py writes
 * them, from a block (lines' text ranges, items' clusters and pens). */
static void ed_summary(const ylay_block* b, const uint32_t* cps, int ncp, char* starts, size_t cs, char* order, size_t co) {
    int w_start[256], w_end[256], nw = 0, i, l, s0;
    size_t k = 0;
    starts[0] = order[0] = 0;
    for (i = 0, s0 = 0; i <= ncp; i++)
        if (i == ncp || cps[i] == ' ') {
            if (i > s0 && nw < 256) { w_start[nw] = s0; w_end[nw] = i; nw++; }
            s0 = i + 1;
        }
    for (l = 0; l < (int)b->n_lines; l++) {
        double xs[256];
        int ws[256], m = 0, a, c;
        k += (size_t)snprintf(starts + k, cs - k, "%s%u", l ? "," : "", b->lines[l].text_start);
        for (a = 0; a < nw; a++) {
            double mx = 1e30;
            uint32_t j;
            if (!((uint32_t)w_start[a] >= b->lines[l].text_start && (uint32_t)w_start[a] < b->lines[l].text_end)) continue;
            for (j = 0; j < b->n_items; j++)
                if ((int)b->cluster[j] >= w_start[a] && (int)b->cluster[j] < w_end[a] && b->items[j].x < mx) mx = b->items[j].x;
            xs[m] = mx; ws[m] = a; m++;
        }
        for (a = 1; a < m; a++)   /* insertion sort by x */
            for (c = a; c > 0 && xs[c] < xs[c - 1]; c--) {
                double tx = xs[c]; int tw = ws[c];
                xs[c] = xs[c - 1]; ws[c] = ws[c - 1]; xs[c - 1] = tx; ws[c - 1] = tw;
            }
        if (m) {
            size_t o = strlen(order);
            o += (size_t)snprintf(order + o, co - o, "%s", o ? "|" : "");
            for (a = 0; a < m; a++) o += (size_t)snprintf(order + o, co - o, "%s%d", a ? "," : "", ws[a]);
        }
    }
}

typedef struct ed_iv { int line; double x0, x1; } ed_iv;
typedef struct ed_rects { ed_iv v[64]; int n; double scale; const skb_layout_t* lay; } ed_rects;

/* The line of a layout whose baseline is nearest y. */
static int ed_line_at(const skb_layout_t* lay, float y) {
    const skb_layout_line_t* lines = skb_layout_get_lines(lay);
    int n = skb_layout_get_lines_count(lay), i, best = 0;
    for (i = 1; i < n; i++)
        if (fabsf(lines[i].baseline - y) < fabsf(lines[best].baseline - y)) best = i;
    return best;
}

static void ed_rect_cb(skb_rect2_t r, void* ctx) {
    ed_rects* R = (ed_rects*)ctx;
    if (R->n < 64 && r.width > 0.01f) {
        /* a rectangle spans its line's ascender to descender: its center is
         * nearest that line's baseline of all lines */
        R->v[R->n].line = R->lay ? ed_line_at(R->lay, r.y + r.height * 0.5f) : 0;
        R->v[R->n].x0 = r.x * R->scale;
        R->v[R->n].x1 = (r.x + r.width) * R->scale;
        R->n++;
    }
}
static int ed_iv_cmp(const void* a, const void* b) {
    const ed_iv* A = (const ed_iv*)a; const ed_iv* B = (const ed_iv*)b;
    if (A->line != B->line) return A->line - B->line;
    return A->x0 < B->x0 ? -1 : A->x0 > B->x0;
}
/* Sorts and merges touching intervals, as editor_ref.py does. */
static int ed_merge(ed_iv* v, int n) {
    int i, m = 0;
    qsort(v, (size_t)n, sizeof *v, ed_iv_cmp);
    for (i = 0; i < n; i++) {
        if (m && v[m - 1].line == v[i].line && v[i].x0 <= v[m - 1].x1 + 0.05) { if (v[i].x1 > v[m - 1].x1) v[m - 1].x1 = v[i].x1; }
        else v[m++] = v[i];
    }
    return m;
}

static skb_editor_t* ed_make(ylay_lib* L, const ed_item* it, float width) {
    skb_attribute_t la[3], pa[2];
    skb_editor_params_t p;
    skb_editor_t* ed;
    la[0] = skb_attribute_make_text_wrap(SKB_WRAP_WORD_CHAR);
    la[1] = skb_attribute_make_text_base_direction(strcmp(it->dir, "rtl") == 0 ? SKB_DIRECTION_RTL : SKB_DIRECTION_LTR);
    la[2] = skb_attribute_make_horizontal_align(SKB_ALIGN_START);
    pa[0] = skb_attribute_make_font_size(ED_SIZE);
    pa[1] = skb_attribute_make_lang(it->lang);
    memset(&p, 0, sizeof p);
    p.font_collection = ylay_skb_fonts(L);
    p.attribute_collection = ylay_skb_attributes(L);
    p.editor_width = width > 0 ? width : -1.0f;
    p.editor_height = -1.0f;
    p.layout_attributes = (skb_attribute_set_t){ la, 3, 0, NULL };
    p.paragraph_attributes = (skb_attribute_set_t){ pa, 2, 0, NULL };
    ed = skb_editor_create(&p);
    if (ed) skb_editor_set_text_utf8(ed, ylay_skb_temp(L), it->text, -1);
    return ed;
}

/* Words next to offset k that are spaces at a direction change: the place
 * a gap was reported. */
static int ed_space_at_run_edge(const uint32_t* cps, int ncp, int k) {
    int i;
    for (i = k - 1; i <= k; i++) {
        int rtl_l, rtl_r;
        if (i < 0 || i >= ncp || cps[i] != ' ' || i == 0 || i + 1 >= ncp) continue;
        rtl_l = cps[i - 1] >= 0x0590 && cps[i - 1] < 0x0800;
        rtl_r = cps[i + 1] >= 0x0590 && cps[i + 1] < 0x0800;
        if (rtl_l != rtl_r) return 1;
    }
    return 0;
}

typedef struct ed_stats {
    int wrap_n, wrap_starts, wrap_order, ink_n, ink_bad, sel_n, sel_skipped, sel_bad, sel_bad_space, caret_n, caret_bad, caret_alt, sel_pieces;
    int wsel_n, wsel_bad, wcaret_n, wcaret_bad;
} ed_stats;

/* The combining marks of the items, as editor_ref.py's is_mark(). */
static int ed_is_mark(uint32_t c) { return (c >= 0x0591 && c <= 0x05C7) || (c >= 0x064B && c <= 0x065F) || c == 0x0670; }

/* Per line, the leftmost and rightmost edge of the characters other than
 * spaces and marks, from the editor's selection rectangle of each one's
 * grapheme (Skribidi gives no rectangle for part of a grapheme), font
 * units, as "x0:x1|x0:x1". */
static void ed_ink(skb_editor_t* ed, const ylay_block* b, const uint32_t* cps, int ncp, double scale, char* out, size_t cap) {
    const skb_layout_t* lay = skb_editor_get_paragraph_layout(ed, 0);
    uint32_t l;
    size_t k = 0;
    out[0] = 0;
    for (l = 0; l < b->n_lines; l++) {
        double lo = 1e30, hi = -1e30;
        uint32_t i;
        for (i = b->lines[l].text_start; i < b->lines[l].text_end && (int)i < ncp; i++) {
            ed_rects R;
            int j, g0, g1;
            if (cps[i] == ' ' || ed_is_mark(cps[i])) continue;
            g0 = skb_layout_align_grapheme_offset(lay, (int32_t)i);
            g1 = skb_layout_get_next_grapheme_offset(lay, g0);
            memset(&R, 0, sizeof R);
            R.scale = scale;
            skb_editor_iterate_text_range_bounds(ed, (skb_text_range_t){ { g0, SKB_AFFINITY_TRAILING }, { g1, SKB_AFFINITY_TRAILING } },
                                                 ed_rect_cb, &R);
            for (j = 0; j < R.n; j++) { if (R.v[j].x0 < lo) lo = R.v[j].x0; if (R.v[j].x1 > hi) hi = R.v[j].x1; }
        }
        if (hi > lo) k += (size_t)snprintf(out + k, cap - k, "%s%.1f:%.1f", k ? "|" : "", lo, hi);
    }
}

/* 1 when every extent of a and b is within ED_TOL. */
static int ed_ink_same(const char* a, const char* b) {
    for (;;) {
        double a0, a1, b0, b1;
        int na, nb;
        if (sscanf(a, "%lf:%lf%n", &a0, &a1, &na) != 2) return sscanf(b, "%lf:%lf", &b0, &b1) != 2;
        if (sscanf(b, "%lf:%lf%n", &b0, &b1, &nb) != 2) return 0;
        if (fabs(a0 - b0) > ED_TOL || fabs(a1 - b1) > ED_TOL) return 0;
        a += na; b += nb;
        if (*a == '|') a++;
        if (*b == '|') b++;
    }
}

static void editor_compare(yol_ctx* ol) {
    const char* fdir = getenv("YSP_FONTS_DIR");
    char path[600];
    size_t n;
    uint8_t* items_txt, * ref_txt;
    static ed_item items[16];
    int n_items, i, n_ran = 0;
    ed_stats S;
    memset(&S, 0, sizeof S);
    if (!fdir) fdir = YLAY_TEST_FONTS;
    snprintf(path, sizeof path, "%s/editor_items.txt", YLAY_TEST_DIR);
    items_txt = read_file(path, &n);
    snprintf(path, sizeof path, "%s/editor_ref.txt", YLAY_TEST_DIR);
    ref_txt = read_file(path, &n);
    if (!items_txt || !ref_txt) { printf("editing: editor_items.txt or editor_ref.txt missing: not checked\n"); free(items_txt); free(ref_txt); return; }
    n_items = ed_items_read((const char*)bytes_of(items_txt), items, 16);
    for (i = 0; i < n_items; i++) {
        const ed_item* it = &items[i];
        ylay_lib* L = NULL;
        ylay_block b;
        uint8_t* fb[4] = { 0 };
        size_t fn[4];
        uint32_t cps[ED_MAXN];
        int ncp = utf32(it->text, cps, ED_MAXN), f, ok = 1, upem = 2048;
        double scale;
        const char* p;
        memset(&b, 0, sizeof b);
        ylay_create(&L, &(ylay_desc){ .ol = ol });
        for (f = 0; f < it->n_fonts && ok; f++) {
            char key[200];
            const char* q;
            ylay_font_info fi;
            snprintf(path, sizeof path, "%s/%s", fdir, it->fonts[f]);
            fb[f] = read_file(path, &fn[f]);
            if (!fb[f] || ylay_add_font(L, bytes_of(fb[f]), fn[f], 0, it->fonts[f]) < 0) {
                printf("editing: %s: %s not found or not opened, skipped\n", it->id, path); ok = 0; break;
            }
            ylay_font(L, f, &fi);
            snprintf(key, sizeof key, "\nfont %s %s ", it->fonts[f], fi.sha256);
            q = strstr((const char*)ref_txt, key);
            if (!q) { printf("editing: %s: %s is another version than the reference's, skipped\n", it->id, it->fonts[f]); ok = 0; break; }
            if (f == 0) upem = atoi(q + strlen(key));
        }
        scale = upem / ED_SIZE;
        /* wrap lines: "\nwrap <id> <model> <w> <starts> <order>" */
        for (p = (const char*)ref_txt; ok && (p = strstr(p, "\nwrap ")) != NULL; p++) {
            char id[48], model[16], st[256], ord[512], ink[1024], my_st[256], my_ord[512], my_ink[1024];
            double w;
            int editor_model = 0;
            if (sscanf(p, "\nwrap %47s %15s %lf %255s %511s %1023s", id, model, &w, st, ord, ink) != 6 || strcmp(id, it->id) != 0) continue;
            my_ink[0] = 0;
            if (strcmp(model, "layout") == 0) {
                ylay_layout(L, it->text, -1, &(ylay_style){ .size = ED_SIZE, .width = (float)w, .lang = it->lang,
                              .dir = strcmp(it->dir, "rtl") == 0 ? YLAY_DIR_RTL : YLAY_DIR_LTR }, NULL, 0, &b);
            } else {
                skb_editor_t* ed = ed_make(L, it, (float)w);
                ylay_from_skb(L, skb_editor_get_paragraph_layout(ed, 0), 0, 0, 0, 0, &b);
                ed_ink(ed, &b, cps, ncp, scale, my_ink, sizeof my_ink);
                skb_editor_destroy(ed);
                editor_model = 1;
            }
            ed_summary(&b, cps, ncp, my_st, sizeof my_st, my_ord, sizeof my_ord);
            S.wrap_n++;
            if (strcmp(st, my_st) != 0) {
                S.wrap_starts++;
                printf("  wrap %s %s %.2f px: line starts %s, Edge %s\n", it->id, model, w, my_st, st);
            } else if (strcmp(ord, my_ord) != 0) {
                S.wrap_order++;
                printf("  wrap %s %s %.2f px: run order %s, Edge %s\n", it->id, model, w, my_ord, ord);
            } else if (editor_model) {
                /* the same lines and order: where the ink sits on each line */
                S.ink_n++;
                if (!ed_ink_same(my_ink, ink)) {
                    S.ink_bad++;
                    printf("  wrap %s %s %.2f px: ink extents %s, Edge %s\n", it->id, model, w, my_ink, ink);
                }
            }
        }
        /* selections and carets on one line */
        if (ok) {
            skb_editor_t* ed = ed_make(L, it, -1.0f);
            for (p = (const char*)ref_txt; (p = strstr(p, "\nsel ")) != NULL; p++) {
                char id[48];
                int s, e, pos, m = 0, k, bad = 0;
                ed_iv ref[64];
                ed_rects R;
                const char* q;
                if (sscanf(p, "\nsel %47s %d %d%n", id, &s, &e, &pos) != 3 || strcmp(id, it->id) != 0) continue;
                {
                    const skb_layout_t* lay = skb_editor_get_paragraph_layout(ed, 0);
                    if (skb_layout_align_grapheme_offset(lay, s) != s || skb_layout_align_grapheme_offset(lay, e) != e) { S.sel_skipped++; continue; }
                }
                q = p + pos;
                while (m < 64) {
                    int c, l;
                    double x0, x1;
                    if (*q == '\n' || !*q) break;
                    if (sscanf(q, " %d:%lf:%lf%n", &l, &x0, &x1, &c) != 3) break;
                    ref[m].line = l; ref[m].x0 = x0; ref[m].x1 = x1; m++;
                    q += c;
                }
                memset(&R, 0, sizeof R);
                R.scale = scale;
                skb_editor_iterate_text_range_bounds(ed, (skb_text_range_t){ { s, SKB_AFFINITY_TRAILING }, { e, SKB_AFFINITY_TRAILING } }, ed_rect_cb, &R);
                { int raw = R.n; R.n = ed_merge(R.v, R.n); if (raw > R.n) S.sel_pieces++; }
                S.sel_n++;
                if (R.n != m) bad = 1;
                for (k = 0; !bad && k < m; k++)
                    if (fabs(R.v[k].x0 - ref[k].x0) > ED_TOL || fabs(R.v[k].x1 - ref[k].x1) > ED_TOL) bad = 1;
                if (bad) {
                    int edge = ed_space_at_run_edge(cps, ncp, s) || ed_space_at_run_edge(cps, ncp, e);
                    S.sel_bad++;
                    if (edge) S.sel_bad_space++;
                    if (S.sel_bad <= 40) {
                        printf("  sel %s [%d, %d)%s: skribidi", it->id, s, e, edge ? " (a space at a direction change)" : "");
                        for (k = 0; k < R.n; k++) printf(" %.1f-%.1f", R.v[k].x0, R.v[k].x1);
                        printf("; Edge");
                        for (k = 0; k < m; k++) printf(" %.1f-%.1f", ref[k].x0, ref[k].x1);
                        printf("\n");
                    }
                }
            }
            for (p = (const char*)ref_txt; (p = strstr(p, "\ncaret ")) != NULL; p++) {
                char id[48];
                int k;
                double x, a, c2;
                skb_caret_info_t c;
                if (sscanf(p, "\ncaret %47s %d %lf", id, &k, &x) != 3 || strcmp(id, it->id) != 0) continue;
                c = skb_editor_get_caret_info_at(ed, (skb_text_position_t){ k, SKB_AFFINITY_TRAILING });
                a = c.x * scale;
                c2 = a;
                if (k > 0) { c = skb_editor_get_caret_info_at(ed, (skb_text_position_t){ k - 1, SKB_AFFINITY_LEADING }); c2 = c.x * scale; }
                S.caret_n++;
                if (fabs(a - x) <= ED_TOL) continue;
                if (fabs(c2 - x) <= ED_TOL) { S.caret_alt++; continue; }
                S.caret_bad++;
                printf("  caret %s %d: skribidi %.1f (or %.1f with the other affinity), Edge %.1f%s\n", it->id, k, a, c2, x,
                       ed_space_at_run_edge(cps, ncp, k) ? " (a space at a direction change)" : "");
            }
            skb_editor_destroy(ed);
            /* wrapped: "\nwsel <id> <w> <s> <e> <line:x0:x1 ...>", "\nwcaret <id> <w> <k> <line> <x>" */
            for (p = (const char*)ref_txt; (p = strstr(p, "\nwsel ")) != NULL; p++) {
                char id[48];
                int s, e, pos, m = 0, k, bad = 0;
                double w;
                ed_iv ref[64];
                ed_rects R;
                const char* q;
                skb_editor_t* wed;
                if (sscanf(p, "\nwsel %47s %lf %d %d%n", id, &w, &s, &e, &pos) != 4 || strcmp(id, it->id) != 0) continue;
                q = p + pos;
                while (m < 64) {
                    int c, l;
                    double x0, x1;
                    if (*q == '\n' || !*q) break;
                    if (sscanf(q, " %d:%lf:%lf%n", &l, &x0, &x1, &c) != 3) break;
                    ref[m].line = l; ref[m].x0 = x0; ref[m].x1 = x1; m++;
                    q += c;
                }
                wed = ed_make(L, it, (float)w);
                memset(&R, 0, sizeof R);
                R.scale = scale;
                R.lay = skb_editor_get_paragraph_layout(wed, 0);
                skb_editor_iterate_text_range_bounds(wed, (skb_text_range_t){ { s, SKB_AFFINITY_TRAILING }, { e, SKB_AFFINITY_TRAILING } }, ed_rect_cb, &R);
                R.n = ed_merge(R.v, R.n);
                skb_editor_destroy(wed);
                S.wsel_n++;
                if (R.n != m) bad = 1;
                for (k = 0; !bad && k < m; k++)
                    if (R.v[k].line != ref[k].line || fabs(R.v[k].x0 - ref[k].x0) > ED_TOL || fabs(R.v[k].x1 - ref[k].x1) > ED_TOL) bad = 1;
                if (bad) {
                    S.wsel_bad++;
                    if (S.wsel_bad <= 20) {
                        printf("  wrapped sel %s %.2f px [%d, %d): skribidi", it->id, w, s, e);
                        for (k = 0; k < R.n; k++) printf(" %d:%.1f-%.1f", R.v[k].line, R.v[k].x0, R.v[k].x1);
                        printf("; Edge");
                        for (k = 0; k < m; k++) printf(" %d:%.1f-%.1f", ref[k].line, ref[k].x0, ref[k].x1);
                        printf("\n");
                    }
                }
            }
            for (p = (const char*)ref_txt; (p = strstr(p, "\nwcaret ")) != NULL; p++) {
                char id[48];
                int k, line, la, lb;
                double w, x, a, c2;
                skb_caret_info_t c;
                skb_editor_t* wed;
                const skb_layout_t* lay;
                if (sscanf(p, "\nwcaret %47s %lf %d %d %lf", id, &w, &k, &line, &x) != 5 || strcmp(id, it->id) != 0) continue;
                wed = ed_make(L, it, (float)w);
                lay = skb_editor_get_paragraph_layout(wed, 0);
                c = skb_editor_get_caret_info_at(wed, (skb_text_position_t){ k, SKB_AFFINITY_TRAILING });
                a = c.x * scale; la = ed_line_at(lay, c.y);
                c2 = a; lb = la;
                if (k > 0) { c = skb_editor_get_caret_info_at(wed, (skb_text_position_t){ k - 1, SKB_AFFINITY_LEADING }); c2 = c.x * scale; lb = ed_line_at(lay, c.y); }
                skb_editor_destroy(wed);
                S.wcaret_n++;
                if ((la == line && fabs(a - x) <= ED_TOL) || (lb == line && fabs(c2 - x) <= ED_TOL)) continue;
                S.wcaret_bad++;
                printf("  wrapped caret %s %.2f px %d: skribidi line %d x %.1f (or line %d x %.1f), Edge line %d x %.1f\n",
                       it->id, w, k, la, a, lb, c2, line, x);
            }
        }
        ylay_block_free(&b);
        ylay_destroy(L);
        for (f = 0; f < 4; f++) free(fb[f]);
        n_ran += ok;
    }
    printf("editing: %d wraps: %d differ in line starts, %d in run order; of the %d editor wraps with the same lines and order, %d differ in "
           "ink extents; %d selections (%d inside a grapheme skipped): %d differ (%d at a space at a direction change); "
           "%d carets: %d differ, %d match with the other affinity; %d selections came as rectangles that touch or overlap\n",
           S.wrap_n, S.wrap_starts, S.wrap_order, S.ink_n, S.ink_bad, S.sel_n, S.sel_skipped, S.sel_bad, S.sel_bad_space,
           S.caret_n, S.caret_bad, S.caret_alt, S.sel_pieces);
    printf("editing, wrapped: %d selections: %d differ; %d carets: %d differ\n", S.wsel_n, S.wsel_bad, S.wcaret_n, S.wcaret_bad);
    /* Every check equals Edge since skribidi-0002 (rule L1 per line); before
     * it, 5 line starts, 10 ink extents, 160 wrapped selections and 12
     * wrapped carets differed (docs/layout.md). Any difference means
     * Skribidi or the reference changed: list it before accepting it
     * (layout_probe.md 7). */
    if (n_ran == n_items) {
        CHECK(S.wrap_starts == 0 && S.wrap_order == 0 && S.ink_bad == 0, "editing: %d line starts, %d orders, %d ink extents differ from Edge",
              S.wrap_starts, S.wrap_order, S.ink_bad);
        CHECK(S.sel_bad == 0 && S.caret_bad == 0 && S.wsel_bad == 0 && S.wcaret_bad == 0,
              "editing: %d selections, %d carets, %d wrapped selections and %d wrapped carets differ from Edge",
              S.sel_bad, S.caret_bad, S.wsel_bad, S.wcaret_bad);
    } else {
        printf("editing: %d of %d items ran: the counts are not checked\n", n_ran, n_items);
    }
    free(items_txt); free(ref_txt);
}

/* --- main ---------------------------------------------------------------------- */

/* --- 8. emoji ------------------------------------------------------------------ */

/* The UTF-8 of a codepoint. */
static int u8(uint32_t c, char* o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | (c >> 6 & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | (c >> 12 & 63)); o[2] = (char)(0x80 | (c >> 6 & 63)); o[3] = (char)(0x80 | (c & 63));
    return 4;
}
/* UTF-8 text from codepoints, 0-ended; one of two static buffers. */
static const char* cps(const uint32_t* c) {
    static char buf[2][128];
    static int which;
    char* o = buf[which ^= 1];
    int n = 0;
    while (*c) n += u8(*c++, o + n);
    o[n] = 0;
    return o;
}

/* One layout with the fonts fl[0 .. n - 1] in that order; for each item in
 * logical order, its font's index in the list and its glyph. Returns the
 * item count, or -1. */
typedef struct em_font { const bb* f; const char* name; } em_font;
static int em_layout(yol_ctx* ol, const em_font* fl, int n, const char* text, int* font_of, uint32_t* glyph_of, uint32_t* missing,
                     ylay_missing* miss, int* n_miss) {
    ylay_lib* L;
    ylay_block b;
    int i, n_items = -1;
    uint32_t r, k;
    memset(&b, 0, sizeof b);
    if (ylay_create(&L, &(ylay_desc){ .ol = ol }) != YLAY_OK) return -1;
    for (i = 0; i < n; i++)
        if (ylay_add_font(L, fl[i].f->d, fl[i].f->n, 0, fl[i].name) != i) { printf("  add %s: %s\n", fl[i].name, ylay_error(L)); goto done; }
    if (ylay_layout(L, text, -1, &(ylay_style){ .size = 100 }, NULL, 0, &b) != YLAY_OK) { printf("  layout: %s\n", ylay_error(L)); goto done; }
    {   /* logical order: by cluster, then by x (one LTR line here); a color
         * glyph's layers (one cluster in a paint run) count as one entry,
         * its glyph the color glyph whose layers they are */
        int order[64], run_of[64], m = (int)(b.n_items < 64 ? b.n_items : 64), a, c, outn = 0;
        for (a = 0; a < m; a++) { order[a] = a; font_of[a] = -1; glyph_of[a] = 0; run_of[a] = 0; }
        for (a = 1; a < m; a++)
            for (c = a; c > 0 && (b.cluster[order[c - 1]] > b.cluster[order[c]] ||
                                  (b.cluster[order[c - 1]] == b.cluster[order[c]] && b.items[order[c - 1]].x > b.items[order[c]].x)); c--) {
                int t = order[c]; order[c] = order[c - 1]; order[c - 1] = t;
            }
        for (r = 0; r < b.n_runs; r++)
            for (k = b.runs[r].first; k < b.runs[r].first + b.runs[r].n && k < 64; k++) run_of[k] = (int)r;
        for (a = 0; a < m;) {
            int k0 = order[a], rr = run_of[k0];
            font_of[outn] = b.runs[rr].font;
            if (b.runs[rr].flags & YLAY_RUN_PAINT) {
                int e = a, nl;
                uint32_t g;
                while (e < m && run_of[order[e]] == rr && b.cluster[order[e]] == b.cluster[k0]) e++;
                glyph_of[outn] = 0xFFFFFFFFu;
                for (g = 0; g < 64; g++) {
                    const ylay_layer* ly;
                    int q, same = 1;
                    nl = ylay_color_glyph(L, b.runs[rr].font, g, &ly);
                    if (nl != e - a) continue;
                    for (q = 0; q < nl; q++)
                        if (b.items[order[a + q]].glyph != (float)ly[q].glyph || b.items[order[a + q]].color != (float)ly[q].paint) same = 0;
                    if (same) { glyph_of[outn] = g; break; }
                }
                a = e;
            } else {
                glyph_of[outn] = (uint32_t)b.items[k0].glyph;
                a++;
            }
            outn++;
        }
        n_items = outn;
    }
    *missing = b.n_missing;
    if (miss) *n_miss = ylay_missing_chars(L, miss, 8);
done:
    ylay_block_free(&b);
    ylay_destroy(L);
    return n_items;
}

static void emoji_checks(yol_ctx* ol) {
    static const uint32_t fr_heart[][2] = { { 0x21, 0x3f }, { 0x41, 0x5f }, { 0x61, 0x7e }, { 0x2764, 0x2764 } };
    bb text = { 0, 0, 0 }, heart = { 0, 0, 0 };
    cfb_buf emo = { 0, 0, 0 }, emo2 = { 0, 0, 0 }, bad = { 0, 0, 0 };
    bb e1, e2;
    int fo[64], n, o, nm = 0;
    uint32_t gl[64], miss = 0;
    ylay_missing mc[8];
    char t[256];
    const uint32_t a_grin_b[] = { 'a', CFB_GRIN, 'b', 0 };
    const uint32_t snow_vs16[] = { 'a', CFB_SNOW, 0xFE0F, 'b', 0 };
    const uint32_t snow_text[] = { 'a', CFB_SNOW, 'b', 0 };
    const uint32_t family[] = { 'x', CFB_MAN, CFB_ZWJ, CFB_WOMAN, 0 };
    const uint32_t heart_vs16[] = { 'a', CFB_HEART, 0xFE0F, 0 };
    make_font_r(&text, fb_fonts[FB_LA].rg, fb_fonts[FB_LA].nr, 900, -300);
    make_font_r(&heart, fr_heart, 4, 900, -300);
    cfb_emoji_v0(&emo);
    cfb_emoji_v0(&emo2);
    e1.d = emo.d; e1.n = emo.n; e1.cap = emo.cap;
    e2.d = emo2.d; e2.n = emo2.n; e2.cap = emo2.cap;
    for (o = 0; o < 2; o++) {   /* both font orders */
        em_font fl[2];
        int ti = o ? 1 : 0, ei = o ? 0 : 1;
        fl[ti].f = &text; fl[ti].name = "la";
        fl[ei].f = &e1; fl[ei].name = "emoji";
        n = em_layout(ol, fl, 2, cps(a_grin_b), fo, gl, &miss, NULL, NULL);
        CHECK(n == 3 && fo[0] == ti && fo[1] == ei && gl[1] == CFB0_GRIN && fo[2] == ti && miss == 0,
              "emoji, order %d: a grin b gave %d items, fonts %d %d %d, glyph %u, %u missing", o, n, fo[0], fo[1], fo[2], gl[1], miss);
        n = em_layout(ol, fl, 2, cps(snow_vs16), fo, gl, &miss, NULL, NULL);
        CHECK(n >= 3 && fo[1] == ei && gl[1] == CFB0_SNOW && miss == 0,
              "emoji, order %d: snowman VS16: %d items, font %d glyph %u, %u missing", o, n, fo[1], gl[1], miss);
        /* text presentation, which the text font lacks: the emoji font's, by the fallback to the other family */
        n = em_layout(ol, fl, 2, cps(snow_text), fo, gl, &miss, NULL, NULL);
        CHECK(n == 3 && fo[1] == ei && gl[1] == CFB0_SNOW && fo[0] == ti && miss == 0,
              "emoji, order %d: snowman as text: %d items, font %d glyph %u, %u missing", o, n, fo[1], gl[1], miss);
        n = em_layout(ol, fl, 2, cps(family), fo, gl, &miss, NULL, NULL);
        CHECK(n == 2 && fo[1] == ei && gl[1] == CFB0_FAMILY && miss == 0,
              "emoji, order %d: the ZWJ sequence: %d items, font %d glyph %u (want the ligature %d), %u missing", o, n, fo[1], gl[1],
              CFB0_FAMILY, miss);
    }
    {   /* no emoji font: glyph 0, counted, named */
        em_font fl[1] = { { &text, "la" } };
        n = em_layout(ol, fl, 1, cps(a_grin_b), fo, gl, &miss, mc, &nm);
        CHECK(n == 3 && gl[1] == 0 && miss == 1 && nm == 1 && mc[0].codepoint == CFB_GRIN && !mc[0].in_a_font,
              "emoji, no emoji font: %d items (want 3), glyph %u, %u missing, %d named (want U+1F600, in no font)", n, gl[1], miss, nm);
    }
    {   /* emoji fonts only: the letters are glyph 0, counted */
        em_font fl[1] = { { &e1, "emoji" } };
        n = em_layout(ol, fl, 1, cps(a_grin_b), fo, gl, &miss, NULL, NULL);
        CHECK(n == 3 && gl[0] == 0 && gl[1] == CFB0_GRIN && gl[2] == 0 && miss == 2,
              "emoji only: %d items (want 3), glyphs %u %u %u, %u missing (want 2)", n, gl[0], gl[1], gl[2], miss);
    }
    {   /* two emoji fonts: the first added wins, either way round */
        em_font fl[3] = { { &text, "la" }, { &e1, "emoji1" }, { &e2, "emoji2" } };
        n = em_layout(ol, fl, 3, cps(a_grin_b), fo, gl, &miss, NULL, NULL);
        CHECK(n == 3 && fo[1] == 1, "two emoji fonts: the grin from font %d, want 1", fo[1]);
        fl[1].f = &e2; fl[1].name = "emoji2"; fl[2].f = &e1; fl[2].name = "emoji1";
        n = em_layout(ol, fl, 3, cps(a_grin_b), fo, gl, &miss, NULL, NULL);
        CHECK(n == 3 && fo[1] == 1, "two emoji fonts, swapped: the grin from font %d, want 1", fo[1]);
    }
    {   /* an emoji the emoji font lacks, which a text font has */
        em_font fl[2] = { { &heart, "la+heart" }, { &e1, "emoji" } };
        n = em_layout(ol, fl, 2, cps(heart_vs16), fo, gl, &miss, NULL, NULL);
        CHECK(n >= 2 && fo[1] == 0 && gl[1] == fb_gid(FB_LA, 0x7e) + 1 && miss == 0,
              "a heart only the text font has: font %d glyph %u, %u missing", fo[1], gl[1], miss);
    }
    {   /* determinism: the same layout twice gives the same items */
        em_font fl[2] = { { &text, "la" }, { &e1, "emoji" } };
        int fo2[64];
        uint32_t gl2[64], m2 = 0;
        int n2;
        snprintf(t, sizeof t, "%s %s", cps(a_grin_b), cps(family));
        n = em_layout(ol, fl, 2, t, fo, gl, &miss, NULL, NULL);
        n2 = em_layout(ol, fl, 2, t, fo2, gl2, &m2, NULL, NULL);
        CHECK(n > 0 && n == n2 && !memcmp(fo, fo2, sizeof(int) * (size_t)n) && !memcmp(gl, gl2, sizeof(uint32_t) * (size_t)n) && miss == 0,
              "emoji: two layouts of one text differ, or %u missing", miss);
    }
    {   /* color glyphs the layout cannot draw: refused by name */
        static const char* const tags[3] = { "CBDT", "sbix", "SVG " };
        int k;
        for (k = 0; k < 3; k++) {
            static cfb_font f;
            ylay_lib* L;
            int rc;
            cfb_init(&f, 1000);
            cfb_add_glyph(&f);
            cfb_box(&f.g[1], 100, 0, 900, 800);
            cfb_map(&f, CFB_GRIN, 1);
            cfb_extra(&f, tags[k]);
            cfb_write(&f, &bad);
            ylay_create(&L, &(ylay_desc){ .ol = ol });
            rc = ylay_add_font(L, bad.d, bad.n, 0, "bitmaps");
            CHECK(rc == YLAY_ERR_REFUSED && strstr(ylay_error(L), "no COLR") && strstr(ylay_error(L), "Twemoji"),
                  "a font with %s and no COLR: %d, '%s'", tags[k], rc, ylay_error(L));
            ylay_destroy(L);
        }
    }
    {   /* the COLR font's info */
        ylay_lib* L;
        ylay_font_info fi;
        memset(&fi, 0, sizeof fi);
        ylay_create(&L, &(ylay_desc){ .ol = ol });
        CHECK(ylay_add_font(L, emo.d, emo.n, 0, "emoji") == 0 && ylay_font(L, 0, &fi) == YLAY_OK && fi.color == 1,
              "the COLR v0 font: color %d, want 1", fi.color);
        ylay_destroy(L);
    }
    printf("emoji: done\n");
    free(text.d); free(heart.d); free(emo.d); free(emo2.d); free(bad.d);
}

/* --- 9. color glyphs ----------------------------------------------------------- */

/* A paint record's gradient parameter at em point x, y, as the shader
 * computes it (ysp/gfx.h PAINT RECORDS), in double. */
static double rec_t(const float* r, double x, double y) {
    double qx, qy, dx, dy, a, b, c, D;
    if ((int)r[0] == 2) return r[4] * x + r[5] * y + r[6];
    qx = r[4] * x + r[5] * y + r[6]; qy = r[7] * x + r[8] * y + r[9]; dx = r[10]; dy = r[11];
    a = dx * dx + dy * dy - 1; b = qx * dx + qy * dy; c = qx * qx + qy * qy;
    D = sqrt(fmax(b * b - a * c, 0.0));
    return b > 0 ? c / (D + b) : (D - b) / -a;
}
static int near_(double a, double b, double tol) { return fabs(a - b) <= tol; }
/* The sRGB code c in linear light, as the flattening converts it. */
static double lin8(int c) { double v = c / 255.0; return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }

/* The bbox (em) of set glyph g of a set, from its record. */
static int set_box(const yol_cset* s, uint32_t g, float box[4]) {
    uint32_t o;
    if (g >= s->words[2] || !(o = s->words[8 + g])) return 0;
    memcpy(box, &s->words[o], 16);
    return 1;
}

static void color_checks(yol_ctx* ol) {
    cfb_buf v0 = { 0, 0, 0 }, v1 = { 0, 0, 0 };
    bb text = { 0, 0, 0 };
    ylay_lib* L;
    ylay_font_info fi;
    const ylay_layer* ly;
    const float* pt;
    int n, np, f1;
    char msg[300];
    cfb_emoji_v0(&v0);
    cfb_emoji_v1(&v1);
    make_font_r(&text, fb_fonts[FB_LA].rg, fb_fonts[FB_LA].nr, 900, -300);
    /* COLR version 0: layers and solid paints */
    ylay_create(&L, &(ylay_desc){ .ol = ol });
    CHECK(ylay_add_font(L, v0.d, v0.n, 0, "v0") == 0, "color: the v0 font: %s", ylay_error(L));
    np = ylay_color_paints(L, 0, &pt);
    n = ylay_color_glyph(L, 0, CFB0_GRIN, &ly);
    CHECK(n == 2 && ly[0].glyph == CFB0_L_DISC && ly[1].glyph == CFB0_L_EYE && np > 0,
          "color v0: the grin has %d layers (want 2: the disc, the eyes), %d paint texels", n, np);
    if (n == 2) {
        const float* y0 = pt + 4 * (size_t)ly[0].paint, *y1 = pt + 4 * (size_t)ly[1].paint;
        CHECK(y0[0] == 1 && y0[1] == 0 && (float)y0[4] == 1.0f && y0[5] == (float)lin8(0xCC) && y0[6] == (float)lin8(0x22) && y0[7] == 1.0f,
              "color v0: the yellow record: %g %g | %g %g %g %g", y0[0], y0[1], y0[4], y0[5], y0[6], y0[7]);
        CHECK(y1[4] == (float)lin8(0x20) && y1[7] == 1.0f, "color v0: the near-black record: %g %g", y1[4], y1[7]);
    }
    n = ylay_color_glyph(L, 0, CFB0_SNOW, &ly);
    CHECK(n == 1 && pt[4 * ly[0].paint + 7] == (float)(0x80 / 255.0) && pt[4 * ly[0].paint + 4] == (float)(0x80 / 255.0),
          "color v0: the snowman: %d layers, white at half alpha premultiplied", n);
    CHECK(ylay_color_glyph(L, 0, CFB0_L_BOX, &ly) == 0, "color v0: a layer glyph is not a color glyph");
    ylay_font(L, 0, &fi);
    CHECK(fi.n_color_glyphs == 5 && fi.n_refused == 0 && fi.n_layer_glyphs == 0 && fi.set_glyphs == (uint32_t)CFB0_N &&
          fi.n_paint_texels == (uint32_t)np,
          "color v0: %u color glyphs (want 5), %u refused, %u layer glyphs (want 0: no transform), set %u", fi.n_color_glyphs,
          fi.n_refused, fi.n_layer_glyphs, fi.set_glyphs);
    ylay_destroy(L);

    /* COLR version 1 */
    ylay_create(&L, &(ylay_desc){ .ol = ol });
    f1 = ylay_add_font(L, v1.d, v1.n, 0, "v1");
    CHECK(f1 == 0, "color: the v1 font: %s", ylay_error(L));
    if (f1 == 0) {
        static const struct { int g; const char* says; } refused[] = {
            { CFB1_B_OVERLAP, "overlap" }, { CFB1_B_SOFT, "SOFT_LIGHT" }, { CFB1_B_SWEEP, "sweep" },
            { CFB1_B_COLRGLYPH, "PaintColrGlyph" }, { CFB1_B_FG, "foreground" }, { CFB1_B_REPEAT, "repeat" },
            { CFB1_B_VAR, "variable" }, { CFB1_B_DEEP, "HarfBuzz's limits" }, { CFB1_B_WIDE, "HarfBuzz's limits" },
            { CFB1_B_STOPS17, "17 stops" }, { CFB1_B_R0, "start radius" }, { CFB1_B_FOCAL, "start circle" },
        };
        size_t k;
        yol_cset set;
        ygfx_cset_desc cd;
        float box[4];
        ylay_font(L, 0, &fi);
        np = ylay_color_paints(L, 0, &pt);
        CHECK(fi.color == 2 && fi.n_color_glyphs == (uint32_t)(CFB1_N - CFB1_B_SOLID) && fi.n_refused == 12,
              "color v1: color %d, %u color glyphs (want %d), %u refused (want 12)", fi.color, fi.n_color_glyphs,
              CFB1_N - CFB1_B_SOLID, fi.n_refused);
        for (k = 0; k < sizeof refused / sizeof refused[0]; k++) {
            n = ylay_color_glyph(L, 0, (uint32_t)refused[k].g, &ly);
            CHECK(n == YLAY_ERR_REFUSED && strstr(ylay_error(L), refused[k].says) && strstr(ylay_error(L), "glyph"),
                  "color v1: glyph %d gave %d, '%s' (want refused: %s)", refused[k].g, n, ylay_error(L), refused[k].says);
        }
        n = ylay_color_glyph(L, 0, CFB1_B_SOFT, &ly);
        CHECK(strstr(ylay_error(L), "Noto-COLRv1-noflags.ttf") != NULL, "color v1: the soft light refusal names Noto's build without flags");
        /* solids: alpha premultiplied, linear light */
        n = ylay_color_glyph(L, 0, CFB1_B_SOLID, &ly);
        CHECK(n == 2 && ly[0].glyph == CFB1_L_BOX && ly[1].glyph == CFB1_L_DISC, "color v1: the solid case: %d layers", n);
        if (n == 2) {
            const float* r0 = pt + 4 * (size_t)ly[0].paint, *r1 = pt + 4 * (size_t)ly[1].paint;
            /* alpha reaches the flattening as HarfBuzz's 8 bits: 0.5 is 128 / 255 */
            CHECK(r0[0] == 1 && r0[4] == 1 && r0[5] == 0 && r0[7] == 1 && r1[5] == (float)(128 / 255.0) && r1[7] == (float)(128 / 255.0) && r1[4] == 0,
                  "color v1: solids %g %g %g %g, %g %g %g %g", r0[4], r0[5], r0[6], r0[7], r1[4], r1[5], r1[6], r1[7]);
        }
        /* linear: t from x 0.1 em to 0.9 em */
        n = ylay_color_glyph(L, 0, CFB1_B_LINEAR, &ly);
        if (CHECK_N(n == 1, "color v1: the linear case: %d layers", n)) {
            const float* r = pt + 4 * (size_t)ly[0].paint;
            CHECK(r[0] == 2 && r[1] == 2 && near_(rec_t(r, 0.1, -0.3), 0, 1e-6) && near_(rec_t(r, 0.9, -0.7), 1, 1e-6) &&
                  near_(rec_t(r, 0.5, 0), 0.5, 1e-6) && r[8] == 0 && r[9] == 1 && r[12] == 1 && r[13] == 0 && r[18] == 1,
                  "color v1: linear record: kind %g n %g, t %g %g %g, offsets %g %g, stop colors %g / %g", r[0], r[1],
                  rec_t(r, 0.1, -0.3), rec_t(r, 0.9, -0.7), rec_t(r, 0.5, 0), r[8], r[9], r[12], r[18]);
        }
        n = ylay_color_glyph(L, 0, CFB1_B_RADIAL, &ly);
        if (CHECK_N(n == 1, "color v1: the radial case: %d layers", n)) {
            const float* r = pt + 4 * (size_t)ly[0].paint;
            CHECK(r[0] == 3 && r[1] == 3 && near_(rec_t(r, 0.5, -0.4), 0, 1e-6) && near_(rec_t(r, 0.8, -0.4), 1, 1e-6) &&
                  near_(rec_t(r, 0.5, -0.55), 0.5, 1e-6) && r[12] == 0 && r[13] == 0.5f && r[14] == 1 &&
                  r[16 + 4 + 3] == (float)(128 / 255.0) && r[16 + 4 + 1] == (float)(128 / 255.0),
                  "color v1: radial record: t %g %g %g, offsets %g %g %g, the middle stop %g %g %g %g", rec_t(r, 0.5, -0.4),
                  rec_t(r, 0.8, -0.4), rec_t(r, 0.5, -0.55), r[12], r[13], r[14], r[20], r[21], r[22], r[23]);
        }
        n = ylay_color_glyph(L, 0, CFB1_B_TWOPT, &ly);
        if (CHECK_N(n == 1, "color v1: the two-point case: %d layers", n)) {
            const float* r = pt + 4 * (size_t)ly[0].paint;
            CHECK(near_(rec_t(r, 0.4, -0.4), 0, 1e-6) && near_(rec_t(r, 0.8, -0.4), 1, 1e-6) && near_(rec_t(r, 0.5, -0.1), 1, 1e-6),
                  "color v1: two-point t at the focal point %g, on the end circle %g %g", rec_t(r, 0.4, -0.4), rec_t(r, 0.8, -0.4),
                  rec_t(r, 0.5, -0.1));
        }
        n = ylay_color_glyph(L, 0, CFB1_B_ELLIPSE, &ly);
        if (CHECK_N(n == 1, "color v1: the ellipse case: %d layers", n)) {
            const float* r = pt + 4 * (size_t)ly[0].paint;
            CHECK(ly[0].glyph == CFB1_L_BOX && near_(rec_t(r, 0.5, -0.4), 0, 1e-6) && near_(rec_t(r, 0.8, -0.4), 1, 1e-6) &&
                  near_(rec_t(r, 0.5, -0.55), 1, 1e-6) && near_(rec_t(r, 0.65, -0.4), 0.5, 1e-6),
                  "color v1: ellipse t %g %g %g %g", rec_t(r, 0.5, -0.4), rec_t(r, 0.8, -0.4), rec_t(r, 0.5, -0.55), rec_t(r, 0.65, -0.4));
        }
        n = ylay_color_glyph(L, 0, CFB1_B_ILLFORMED, &ly);
        CHECK(n == 1 && ly[0].glyph == CFB1_L_DISC, "color v1: ill-formed gradients (p2 = p0; both radii 0) draw nothing, the disc "
              "beside them does: %d layers (want 1)", n);
        n = ylay_color_glyph(L, 0, CFB1_B_TURNED_RAD, &ly);
        if (CHECK_N(n == 1, "color v1: the skewed radial: %d layers", n)) {
            /* x' = 1.5 x - 0.2 y, y' = 0.3 x + y (Fixed 16.16 rounds 0.3 and 0.2), font units y up; in em y down */
            const float* r = pt + 4 * (size_t)ly[0].paint;
            const double a = 1.5, b = 19661.0 / 65536.0, c = -13107.0 / 65536.0, d = 1.0;
            const double px = 500 + 300 * cos(0.7), py = 400 + 300 * sin(0.7);
            const double cx = (a * 500 + c * 400) / 1000, cy = -(b * 500 + d * 400) / 1000;
            const double ex = (a * px + c * py) / 1000, ey = -(b * px + d * py) / 1000;
            CHECK(near_(rec_t(r, cx, cy), 0, 1e-6) && near_(rec_t(r, ex, ey), 1, 1e-6),
                  "color v1: the skewed radial: t %g at the mapped center, %g on its mapped circle", rec_t(r, cx, cy), rec_t(r, ex, ey));
        }
        n = ylay_color_glyph(L, 0, CFB1_B_OPACITY, &ly);
        if (CHECK_N(n == 2, "color v1: group opacity over two boxes apart: %d layers (want 2)", n)) {
            const float* r0 = pt + 4 * (size_t)ly[0].paint, *r1 = pt + 4 * (size_t)ly[1].paint;
            const double a = 128 / 255.0, b8 = 204 / 255.0 * a;   /* 0.5 and 0.8 in 8 bits */
            CHECK(r0[6] == (float)a && r0[7] == (float)a && r1[4] == (float)b8 && r1[7] == (float)b8,
                  "color v1: group opacity 0.5: alphas %g, %g (want 128/255, 204/255 of that)", r0[7], r1[7]);
        }
        /* the set: paints first, then every font and layer glyph; checked */
        CHECK(ylay_open_set(L, 0, &set, ol) == YLAY_OK && ylay_build_glyphs(L, 0, &set, NULL, 0, 0) == YLAY_OK,
              "color v1: the set: %s", ylay_error(L));
        memset(&cd, 0, sizeof cd);
        cd.texels = set.texels; cd.n_texels = set.n_texels; cd.words = set.words; cd.n_words = set.n_words;
        CHECK(ygfx_cset_check(&cd, msg, sizeof msg) == YGFX_OK && !memcmp(set.texels, pt, 16 * (size_t)np) &&
              set.words[2] == fi.set_glyphs && fi.set_glyphs > (uint32_t)CFB1_N,
              "color v1: the set fails its check (%s), or its first texels are not the paints, or it has %u glyphs", msg, set.words[2]);
        n = ylay_color_glyph(L, 0, CFB1_B_XFORM, &ly);
        CHECK(n == 1 && ly[0].glyph >= (uint32_t)CFB1_N && set_box(&set, ly[0].glyph, box) &&
              near_(box[0], 0.3, 1e-6) && near_(box[2], 0.5, 1e-6) && near_(box[1], -0.4, 1e-6) && near_(box[3], -0.2, 1e-6),
              "color v1: the translated box: layer glyph %u, box %g %g %g %g (want 0.3 -0.4 0.5 -0.2)", n == 1 ? ly[0].glyph : 0,
              box[0], box[1], box[2], box[3]);
        n = ylay_color_glyph(L, 0, CFB1_B_ROTATE, &ly);
        if (CHECK_N(n == 1 && set_box(&set, ly[0].glyph, box), "color v1: the turned box: %d layers", n)) {
            const float* r = pt + 4 * (size_t)ly[0].paint;
            const double an = 2731.0 / 16384.0 * M_PI, cs = cos(an), sn = sin(an);   /* 30 degrees in F2DOT14 */
            CHECK(near_(box[0], -0.3 * sn, 1e-6) && near_(box[2], 0.2 * cs - 0.1 * sn, 1e-6) &&
                  near_(box[1], -(0.2 * sn + 0.3 * cs), 1e-6) && near_(box[3], -0.1 * cs, 1e-6),
                  "color v1: the turned box's box %g %g %g %g", box[0], box[1], box[2], box[3]);
            CHECK(near_(rec_t(r, 0, 0), 0, 1e-6) && near_(rec_t(r, 0.2 * cs, -0.2 * sn), 1, 1e-6),
                  "color v1: the turned gradient: t %g at the origin, %g at the turned end", rec_t(r, 0, 0), rec_t(r, 0.2 * cs, -0.2 * sn));
        }
        n = ylay_color_glyph(L, 0, CFB1_B_CLIP, &ly);
        CHECK(n == 1 && ly[0].glyph >= (uint32_t)CFB1_N && set_box(&set, ly[0].glyph, box) && near_(box[0], 0.2, 1e-6) &&
              near_(box[2], 0.7, 1e-6) && near_(box[1], -0.6, 1e-6) && near_(box[3], -0.1, 1e-6),
              "color v1: the box cut by its clip box: %g %g %g %g (want 0.2 -0.6 0.7 -0.1)", box[0], box[1], box[2], box[3]);
        yol_cset_close(&set);
        {   /* the same font again gives the same paints and layers */
            ylay_lib* L2;
            const float* pt2;
            const ylay_layer* l2;
            int same = 1;
            uint32_t g;
            ylay_create(&L2, &(ylay_desc){ .ol = ol });
            ylay_add_font(L2, v1.d, v1.n, 0, "v1");
            same = ylay_color_paints(L2, 0, &pt2) == np && !memcmp(pt, pt2, 16 * (size_t)np);
            for (g = 0; g < (uint32_t)CFB1_N; g++) {
                int a = ylay_color_glyph(L, 0, g, &ly), b2 = ylay_color_glyph(L2, 0, g, &l2);
                if (a != b2 || (a > 0 && memcmp(ly, l2, sizeof *ly * (size_t)a))) same = 0;
            }
            CHECK(same, "color v1: a second flattening of the font differs");
            ylay_destroy(L2);
        }
    }
    ylay_destroy(L);
    {   /* a layout: the color glyph's layers in a paint run; a refused glyph by name */
        ylay_block b;
        char t[32];
        uint32_t cp[3] = { 'a', CFB1_CP(CFB1_B_XFORM), 0 }, r, paint_runs = 0, layer_items = 0;
        int rc;
        memset(&b, 0, sizeof b);
        ylay_create(&L, &(ylay_desc){ .ol = ol });
        ylay_add_font(L, text.d, text.n, 0, "la");
        ylay_add_font(L, v1.d, v1.n, 0, "v1");
        snprintf(t, sizeof t, "%s", cps(cp));
        rc = ylay_layout(L, t, -1, &(ylay_style){ .size = 64 }, NULL, 0, &b);
        for (r = 0; r < b.n_runs; r++)
            if (b.runs[r].flags & YLAY_RUN_PAINT) { paint_runs++; layer_items += b.runs[r].n; }
        CHECK(rc == YLAY_OK && paint_runs == 1 && layer_items == 1 && b.n_missing == 0, "color layout: %d, %u paint runs, %u layer items",
              rc, paint_runs, layer_items);
        ylay_font(L, 1, &fi);
        {
            ygfx_cset_desc cd;
            memset(&cd, 0, sizeof cd);
            cd.texels = fi.set->texels; cd.n_texels = fi.set->n_texels; cd.words = fi.set->words; cd.n_words = fi.set->n_words;
            CHECK(ygfx_cset_check(&cd, msg, sizeof msg) == YGFX_OK && fi.n_built == 1, "color layout: the font's set: %s, %u built", msg, fi.n_built);
        }
        cp[1] = CFB1_CP(CFB1_B_SOFT);
        snprintf(t, sizeof t, "%s", cps(cp));
        rc = ylay_layout(L, t, -1, &(ylay_style){ .size = 64 }, NULL, 0, &b);
        snprintf(msg, sizeof msg, "U+%04X (glyph %d)", (unsigned)CFB1_CP(CFB1_B_SOFT), CFB1_B_SOFT);
        CHECK(rc == YLAY_ERR_REFUSED && strstr(ylay_error(L), msg) && strstr(ylay_error(L), "SOFT_LIGHT") && strstr(ylay_error(L), "v1"),
              "color layout: a refused glyph gave %d, '%s' (want '%s', the mode and the font)", rc, ylay_error(L), msg);
        ylay_block_free(&b);
        ylay_destroy(L);
    }
    printf("color glyphs: done\n");
    free(v0.d); free(v1.d); free(text.d);
}

/* --- 10. threads ------------------------------------------------------------ */

#define TH_N 4
#define TH_ROUNDS 2000
typedef struct th_work {
    uint32_t font_ids[TH_ROUNDS], attr_ids[TH_ROUNDS];
    int bad_versions, bad_libs;
} th_work;

#if defined(_WIN32)
static DWORD WINAPI th_main(LPVOID arg)
#else
static void* th_main(void* arg)
#endif
{
    th_work* w = (th_work*)arg;
    yol_ctx ol;
    int r;
    yol_open(&ol, NULL);
    for (r = 0; r < TH_ROUNDS; r++) {
        skb_font_collection_t* fc = skb_font_collection_create();
        skb_attribute_collection_t* ac = skb_attribute_collection_create();
        const ylay_versions* v = ylay_get_versions();
        w->font_ids[r] = skb_font_collection_get_id(fc);
        w->attr_ids[r] = skb_attribute_collection_get_id(ac);
        skb_font_collection_destroy(fc);
        skb_attribute_collection_destroy(ac);
        if (!v || strcmp(v->harfbuzz, "14.6.0") != 0) w->bad_versions++;
        if (r % 100 == 0) {
            ylay_lib* L = NULL;
            if (ylay_create(&L, &(ylay_desc){ .ol = &ol }) != YLAY_OK) w->bad_libs++;
            ylay_destroy(L);
        }
    }
    yol_close(&ol);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static int th_cmp(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

/* The ids of n collections: how many are another's too. */
static int th_dups(uint32_t* ids, int n) {
    int i, d = 0;
    qsort(ids, (size_t)n, sizeof *ids, th_cmp);
    for (i = 1; i < n; i++) d += ids[i] == ids[i - 1];
    return d;
}

static void thread_checks(void) {
    static th_work w[TH_N];
    static uint32_t all[TH_N * TH_ROUNDS];
    int i, r, started = 0, bad_v = 0, bad_l = 0;
#if defined(_WIN32)
    HANDLE h[TH_N];
    for (i = 0; i < TH_N; i++) {
        h[i] = CreateThread(NULL, 0, th_main, &w[i], 0, NULL);
        started += h[i] != NULL;
    }
    for (i = 0; i < TH_N; i++)
        if (h[i]) {
            WaitForSingleObject(h[i], INFINITE);
            CloseHandle(h[i]);
        }
#else
    pthread_t h[TH_N];
    int ok[TH_N];
    for (i = 0; i < TH_N; i++) {
        ok[i] = pthread_create(&h[i], NULL, th_main, &w[i]) == 0;
        started += ok[i];
    }
    for (i = 0; i < TH_N; i++)
        if (ok[i]) pthread_join(h[i], NULL);
#endif
    CHECK(started == TH_N, "threads: %d of %d started", started, TH_N);
    if (started != TH_N) return;
    for (i = 0; i < TH_N; i++) {
        bad_v += w[i].bad_versions;
        bad_l += w[i].bad_libs;
    }
    CHECK(bad_v == 0 && bad_l == 0, "threads: %d versions and %d libraries wrong", bad_v, bad_l);
    for (i = 0; i < TH_N; i++)
        for (r = 0; r < TH_ROUNDS; r++) all[i * TH_ROUNDS + r] = w[i].font_ids[r];
    r = th_dups(all, TH_N * TH_ROUNDS);
    CHECK(r == 0, "threads: %d font collections got another's id", r);
    for (i = 0; i < TH_N; i++)
        for (r = 0; r < TH_ROUNDS; r++) all[i * TH_ROUNDS + r] = w[i].attr_ids[r];
    r = th_dups(all, TH_N * TH_ROUNDS);
    CHECK(r == 0, "threads: %d attribute collections got another's id", r);
    printf("threads: %d threads, %d font and %d attribute collections each\n", TH_N, TH_ROUNDS, TH_ROUNDS);
}

int main(void) {
    yol_ctx ol;
    ylay_lib* L;
    ylay_block b;
    bb font = { 0, 0, 0 };
    order_case oc[3];
    const ylay_versions* v = ylay_get_versions();
    int i, nc, fi;
    char stamp[1024];
    yol_open(&ol, NULL);

    /* 1. versions */
    printf("skribidi %s, patch %s, harfbuzz %s\n", v->skribidi, v->skribidi_patch, v->harfbuzz);
    CHECK(strlen(v->skribidi) == 40, "Skribidi's commit is not set: %s", v->skribidi);
    {
        int ok = strlen(v->skribidi_patch) == 10 * (7 + 64) + 9, k;
        for (k = 0; k < 10 && ok; k++) ok = strncmp(v->skribidi_patch + 72 * k, "sha256:", 7) == 0;
        CHECK(ok, "the ten patch hashes are not set, or the tree differs from the manifest: %s", v->skribidi_patch);
    }
    CHECK(strcmp(v->harfbuzz, "14.6.0") == 0 && strcmp(v->harfbuzz, hb_version_string()) == 0, "HarfBuzz is %s (%s linked), the pin is 14.6.0",
          v->harfbuzz, hb_version_string());

    /* 10. threads */
    thread_checks();

    /* 2. visual order with the written font */
    make_font(&font);
    CHECK(ylay_create(&L, &(ylay_desc){ .ol = &ol }) == YLAY_OK, "create");
    fi = ylay_add_font(L, font.d, font.n, 0, "boxes");
    CHECK(fi == 0, "the written font: %s", ylay_error(L));
    memset(&b, 0, sizeof b);
    nc = n_order_cases(oc);
    for (i = 0; fi == 0 && i < nc; i++) {
        int bad;
        CHECK(ylay_layout(L, oc[i].text, -1, &(ylay_style){ .size = 20, .dir = oc[i].dir }, NULL, 0, &b) == YLAY_OK,
              "%s: %s", oc[i].name, ylay_error(L));
        CHECK(b.n_lines == 1, "%s: %u lines", oc[i].name, b.n_lines);
        CHECK(b.n_missing == 0, "%s: %u glyphs missing", oc[i].name, b.n_missing);
        bad = check_order(&oc[i], &b, 1);
        CHECK(bad == 0, "%s: %d items out of the bidi order", oc[i].name, bad);
        if (!bad) printf("order: %s: %u items in bidi order\n", oc[i].name, b.n_items);
    }
    /* 5. the same text again: nothing grows */
    if (fi == 0) {
        ygfx_citem* items = b.items;
        uint32_t cap = b.cap_items, capr = b.cap_runs, capl = b.cap_lines, nnew;
        const uint32_t* g;
        ylay_take_new_glyphs(L, 0, &g);
        ylay_layout(L, S3, -1, &(ylay_style){ .size = 20, .dir = YLAY_DIR_LTR }, NULL, 0, &b);
        nnew = (uint32_t)ylay_take_new_glyphs(L, 0, &g);
        CHECK(b.items == items && b.cap_items == cap && b.cap_runs == capr && b.cap_lines == capl, "a second layout grew the block");
        CHECK(nnew == 0 && b.n_new_glyphs == 0, "a second layout built %u glyphs again", nnew);
        /* spans: a different color and size inside the text */
        {
            ylay_span sp = { 4, 4 + 10, 30, 3, NULL };   /* the Arabic word, 2 bytes a letter */
            CHECK(ylay_layout(L, S3, -1, &(ylay_style){ .size = 20, .color = 1 }, &sp, 1, &b) == YLAY_OK, "spans: %s", ylay_error(L));
            CHECK(b.n_runs == 2, "spans: %u runs, want 2 (two sizes)", b.n_runs);
            for (i = 0; i < (int)b.n_items; i++) {
                int in = b.cluster[i] >= 4 && b.cluster[i] < 9;
                if (b.items[i].color != (in ? 3.0f : 1.0f)) { CHECK(0, "spans: item %d has color %g", i, b.items[i].color); break; }
            }
        }
        /* a wrapped paragraph: more lines when narrower */
        {
            uint32_t wide, narrow;
            ylay_layout(L, "aaa bbb ccc ddd eee fff ggg hhh", -1, &(ylay_style){ .size = 20, .width = 200 }, NULL, 0, &b);
            wide = b.n_lines;
            ylay_layout(L, "aaa bbb ccc ddd eee fff ggg hhh", -1, &(ylay_style){ .size = 20, .width = 100 }, NULL, 0, &b);
            narrow = b.n_lines;
            CHECK(wide == 2 && narrow == 4, "wrap: %u lines at 200, %u at 100 (want 2, 4: 30 px words, 10 px spaces)", wide, narrow);
            CHECK(b.w <= 100.5f, "wrap: the block is %g wide at width 100", b.w);
        }
        ylay_stamp(L, &b, stamp, sizeof stamp);
        printf("stamp: %s\n", stamp);
        CHECK(strstr(stamp, "harfbuzz=14.6.0") && strstr(stamp, "boxes#0="), "the stamp: %s", stamp);
    }
    /* 4. the written font's curve set */
    if (fi == 0) {
        ylay_font_info info;
        ygfx_cset_desc cd;
        char msg[300];
        ylay_font(L, 0, &info);
        memset(&cd, 0, sizeof cd);
        cd.texels = info.set->texels; cd.n_texels = info.set->n_texels; cd.words = info.set->words; cd.n_words = info.set->n_words;
        CHECK(ygfx_cset_check(&cd, msg, sizeof msg) == YGFX_OK, "the written font's set: %s", msg);
        printf("the written font: %u glyphs built, set checked\n", info.n_built);
    }
    ylay_block_free(&b);
    ylay_destroy(L);
    free(font.d);

    /* 7. font fallback past the script */
    fallback_checks(&ol);
    /* 8. emoji */
    emoji_checks(&ol);
    /* 9. color glyphs */
    color_checks(&ol);

    /* 3. the corpus */
    corpus(&ol);
    editor_compare(&ol);
    yol_close(&ol);
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
