/* layout_test.c - checks of pack/layout (docs/layout.md, Checks).
 *
 *   1. The versions the glue logs: Skribidi's commit, the patch hash, the
 *      HarfBuzz version.
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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef YLAY_TEST_DIR
#define YLAY_TEST_DIR "tests/layout"
#endif
#ifndef YLAY_TEST_FONTS
#define YLAY_TEST_FONTS "C:/Windows/Fonts"
#endif

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

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

static void make_font(bb* out) {
    bb t[7];
    static const char* tags[7] = { "cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp" };
    uint32_t ng = 2, g, i, r;
    size_t dir;
    memset(t, 0, sizeof t);
    for (r = 0; r < N_RANGES; r++) ng += font_ranges[r][1] - font_ranges[r][0] + 1;
    /* cmap: format 4, the space, the ranges, 0xFFFF */
    {
        bb* c = &t[0];
        uint32_t nseg = N_RANGES + 2, base = 2, sr = 1, es = 0;
        while (sr * 2 <= nseg) { sr *= 2; es++; }
        b16(c, 0); b16(c, 1); b16(c, 3); b16(c, 1); b32(c, 12);
        b16(c, 4); b16(c, 16 + 8 * nseg); b16(c, 0);
        b16(c, 2 * nseg); b16(c, 2 * sr); b16(c, es); b16(c, 2 * nseg - 2 * sr);
        b16(c, 0x20);
        for (r = 0; r < N_RANGES; r++) b16(c, font_ranges[r][1]);
        b16(c, 0xffff); b16(c, 0);
        b16(c, 0x20);
        for (r = 0; r < N_RANGES; r++) b16(c, font_ranges[r][0]);
        b16(c, 0xffff);
        b16(c, (1 - 0x20) & 0xffff);
        for (r = 0; r < N_RANGES; r++) {
            b16(c, (base - font_ranges[r][0]) & 0xffff);
            base += font_ranges[r][1] - font_ranges[r][0] + 1;
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
    t[3].d[4] = 0x03; t[3].d[5] = 0x20;                 /* ascender 800  */
    t[3].d[6] = 0xff; t[3].d[7] = 0x38;                 /* descender -200 */
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
    CHECK(strlen(v->skribidi_patch) == 2 * (7 + 64) + 1 && strncmp(v->skribidi_patch, "sha256:", 7) == 0 &&
          strncmp(v->skribidi_patch + 72, "sha256:", 7) == 0,
          "the two patch hashes are not set, or the tree differs from the manifest: %s", v->skribidi_patch);
    CHECK(strcmp(v->harfbuzz, "14.6.0") == 0, "HarfBuzz is %s, the pin is 14.6.0", v->harfbuzz);

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

    /* 3. the corpus */
    corpus(&ol);
    editor_compare(&ol);
    yol_close(&ol);
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
