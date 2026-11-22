/* layout.c - see ysp/layout.h and docs/layout.md. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L   /* clock_gettime under -std=c11 */
#endif
#include "ysp/layout.h"
#include "colr.h"

#include <hb.h>
#include <hb-ot.h>
#include "skribidi/skb_common.h"
#include "skribidi/skb_font_collection.h"
#include "skribidi/skb_attribute_collection.h"
#include "skribidi/skb_layout.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

/* The pins of third_party/ (third_party/README.md and tools/vendor_layout.py
 * hold the same). The patch hashes come from the build, which computes them
 * from the patch files and marks a tree that differs from the manifest. */
#define YLAY_SKRIBIDI_COMMIT  "dee63d6ba76aeddd49dea6d1b2508cf9aa391f46"
#define YLAY_SHEENBIDI_COMMIT "83f77108a2873600283f6da4b326a2dca7a3a7a6"
#define YLAY_LIBUNIBREAK_TAG  "libunibreak_6_1"
#define YLAY_BUDOUXC_COMMIT   "a044d49afc654117fac7623fff15bec15943270c"
#ifndef YLAY_PATCHES
#define YLAY_PATCHES "unknown"
#endif

/* --- time -------------------------------------------------------------------- */

static double ylay__now_us(void) {
#if defined(_WIN32)
    /* read at each call, not kept in a static: libraries on two threads
     * time their work at once (v0.3.5) */
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec * 1e-3;
#endif
}

/* --- SHA-256 (FIPS 180-4): the font hashes of the manifest ------------------- */

static const uint32_t ylay__k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define YLAY__ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void ylay__sha_block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64], a, b, c, d, e, f, g, k, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = YLAY__ROR(w[i - 15], 7) ^ YLAY__ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = YLAY__ROR(w[i - 2], 17) ^ YLAY__ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
    for (i = 0; i < 64; i++) {
        t1 = k + (YLAY__ROR(e, 6) ^ YLAY__ROR(e, 11) ^ YLAY__ROR(e, 25)) + ((e & f) ^ (~e & g)) + ylay__k[i] + w[i];
        t2 = (YLAY__ROR(a, 2) ^ YLAY__ROR(a, 13) ^ YLAY__ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

static void ylay__sha256_hex(const uint8_t* p, size_t n, char out[65]) {
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t tail[128];
    size_t full = n / 64 * 64, rest = n - full, i, tn;
    uint64_t bits = (uint64_t)n * 8;
    for (i = 0; i < full; i += 64) ylay__sha_block(h, p + i);
    memset(tail, 0, sizeof tail);
    if (rest) memcpy(tail, p + full, rest);
    tail[rest] = 0x80;
    tn = rest + 1 + 8 <= 64 ? 64 : 128;
    for (i = 0; i < 8; i++) tail[tn - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (i = 0; i < tn; i += 64) ylay__sha_block(h, tail + i);
    for (i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", (unsigned)h[i]);
}

/* --- the library -------------------------------------------------------------- */

typedef struct ylay__font {
    char              name[96];
    char              sha256[65];
    int               face;
    yol_font        f;
    skb_font_handle_t handle;
    yol_cset        set;
    uint8_t*          have;          /* one bit per glyph: in the set            */
    uint32_t*         fresh;         /* built since the last take                 */
    uint32_t          n_fresh, cap_fresh;
    uint32_t*         todo;          /* this call's glyphs to build               */
    uint32_t          n_todo, cap_todo;
    uint32_t*         taken;         /* what the last take returned               */
    uint32_t          cap_taken;
    uint32_t          n_built;
    double            build_us;
    int               color;         /* 0 none, 1 COLR v0, 2 COLR v1 paints      */
    uint32_t          G;             /* the set's glyph table: font and layer glyphs */
    ylay__colr        colr;          /* the color glyphs' layers and paints       */
} ylay__font;

typedef struct ylay__key { int font; float size; uint32_t n, at, flags; } ylay__key;

struct ylay_lib {
    yol_ctx*                  ol;
    skb_font_collection_t*      fc;
    skb_attribute_collection_t* ac;
    skb_temp_alloc_t*           temp;
    skb_layout_t*               layout;
    ylay__font                fonts[YLAY_MAX_FONTS];
    int                         n_fonts;
    /* per call scratch, grown only */
    skb_content_run_t*          cruns;   uint32_t cap_cruns;
    skb_attribute_t*            cattrs;  uint32_t cap_cattrs;
    float*                      ccolor;
    ylay__key*                keys;    uint32_t cap_keys;
    ylay_missing*             miss;    uint32_t n_miss, cap_miss;   /* the last call's */
    ylay_versions             versions;
    char                        error[256];
};

static int ylay__fail(ylay_lib* L, int code, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(L->error, sizeof L->error, fmt, ap);
    va_end(ap);
    return code;
}

/* Grows *p to hold need elements; the capacity only grows, so a block or a
 * scratch array laid out again at the same size allocates nothing. */
static int ylay__grow(void** p, uint32_t* cap, uint32_t need, size_t size) {
    uint32_t c;
    void* q;
    if (need <= *cap) return 1;
    c = *cap ? *cap : 16;
    while (c < need) c *= 2;
    q = realloc(*p, (size_t)c * size);
    if (!q) return 0;
    *p = q;
    *cap = c;
    return 1;
}

/* Constant, so threads that call ylay_get_versions() or ylay_create() at
 * once share it with no write (v0.3.5: the HarfBuzz field was written at
 * each call). HarfBuzz is compiled in from the vendored tree, so its
 * hb_version_string() is this HB_VERSION_STRING. */
static const ylay_versions ylay__static_versions = {
    YLAY_VERSION_STRING, YLAY_SKRIBIDI_COMMIT, YLAY_PATCHES, HB_VERSION_STRING,
    YLAY_SHEENBIDI_COMMIT, YLAY_LIBUNIBREAK_TAG, YLAY_BUDOUXC_COMMIT,
};

const ylay_versions* ylay_get_versions(void) { return &ylay__static_versions; }

int ylay_create(ylay_lib** out, const ylay_desc* d) {
    ylay_lib* L;
    if (!out || !d || !d->ol) return YLAY_ERR_ARG;
    *out = NULL;
    L = (ylay_lib*)calloc(1, sizeof *L);
    if (!L) return YLAY_ERR_NOMEM;
    L->ol = d->ol;
    L->fc = skb_font_collection_create();
    L->ac = skb_attribute_collection_create();
    L->temp = skb_temp_alloc_create(64 * 1024);
    if (!L->fc || !L->ac || !L->temp) { ylay_destroy(L); return YLAY_ERR_NOMEM; }
    L->versions = *ylay_get_versions();
    *out = L;
    return YLAY_OK;
}

void ylay_destroy(ylay_lib* L) {
    int i;
    if (!L) return;
    if (L->layout) skb_layout_destroy(L->layout);
    for (i = 0; i < L->n_fonts; i++) {
        ylay__font* F = &L->fonts[i];
        yol_cset_close(&F->set);
        ylay__colr_free(&F->colr);
        free(F->have); free(F->fresh); free(F->todo); free(F->taken);
    }
    if (L->fc) skb_font_collection_destroy(L->fc);
    if (L->ac) skb_attribute_collection_destroy(L->ac);
    if (L->temp) skb_temp_alloc_destroy(L->temp);
    free(L->cruns); free(L->cattrs); free(L->ccolor); free(L->keys); free(L->miss);
    free(L);
}

const char* ylay_error(const ylay_lib* L) { return L ? L->error : ""; }

/* The size of table tag in face (0: absent). */
static unsigned ylay__table_len(hb_face_t* face, hb_tag_t tag) {
    hb_blob_t* b = hb_face_reference_table(face, tag);
    unsigned n = hb_blob_get_length(b);
    hb_blob_destroy(b);
    return n;
}

int ylay_add_font(ylay_lib* L, const void* bytes, size_t n, int face, const char* name) {
    ylay__font* F;
    hb_blob_t* blob;
    hb_face_t* hf;
    hb_font_t* font = NULL;
    char err[200];
    const char* other = NULL;
    int colr = 0, colr_table = 0;
    yol_cset_desc cd;
    if (!L || !bytes || n == 0 || n > 0x7fffffffu || face < 0) return YLAY_ERR_ARG;
    if (L->n_fonts == YLAY_MAX_FONTS) return ylay__fail(L, YLAY_ERR_FULL, "ysp_layout: %d fonts already", YLAY_MAX_FONTS);
    F = &L->fonts[L->n_fonts];
    memset(F, 0, sizeof *F);
    snprintf(F->name, sizeof F->name, "%s", name ? name : "font");
    F->face = face;
    blob = hb_blob_create_or_fail((const char*)bytes, (unsigned)n, HB_MEMORY_MODE_READONLY, NULL, NULL);
    hf = blob ? hb_face_create_or_fail(blob, (unsigned)face) : NULL;
    hb_blob_destroy(blob);
    if (!hf) return ylay__fail(L, YLAY_ERR_FORMAT, "ysp_layout: %s: HarfBuzz could not open face %d", F->name, face);
    /* Color: COLR (versions 0 and 1) only. A font whose color glyphs are
     * bitmaps (CBDT, sbix) or SVG documents and that has no COLR would be
     * drawn as its monochrome outlines, or not at all: refused by name. A
     * COLR that HarfBuzz rejects would draw the same way: refused too. */
    colr_table = ylay__table_len(hf, HB_TAG('C', 'O', 'L', 'R')) > 0;
    colr = hb_ot_color_has_layers(hf) || hb_ot_color_has_paint(hf);
    if (ylay__table_len(hf, HB_TAG('C', 'B', 'D', 'T'))) other = "CBDT bitmaps";
    else if (ylay__table_len(hf, HB_TAG('s', 'b', 'i', 'x'))) other = "sbix bitmaps";
    else if (ylay__table_len(hf, HB_TAG('S', 'V', 'G', ' '))) other = "SVG documents";
    if (colr_table && !colr) {
        hb_face_destroy(hf);
        return ylay__fail(L, YLAY_ERR_FORMAT, "ysp_layout: %s: its COLR table is malformed or holds no color glyph (HarfBuzz reads none); "
                          "use a COLR font that HarfBuzz reads, such as Twemoji (docs/layout.md, Emoji)", F->name);
    }
    if (!colr && other) {
        hb_face_destroy(hf);
        return ylay__fail(L, YLAY_ERR_REFUSED, "ysp_layout: %s: its color glyphs are %s and it has no COLR table; ysp draws COLR color "
                          "fonts only: use Twemoji (Mozilla's COLR build) or another COLR font (docs/layout.md, Emoji)", F->name, other);
    }
    /* ysp/outline.h reads the outlines; a font it refuses (CFF2, no outlines)
     * cannot become a curve set, so it is refused here, by its reason. */
    if (yol_font_view(&F->f, bytes, n, face, err, sizeof err) < 0) {
        hb_face_destroy(hf);
        return ylay__fail(L, YLAY_ERR_FORMAT, "ysp_layout: %s: %s", F->name, err);
    }
    if (colr) {
        /* every color glyph flattened now, so the paint records sit at the
         * set's start in an order the font alone gives: the pack tool's set
         * (ylay_open_set) then has the same paint indexes */
        int rc = ylay__colr_build(&F->colr, hf, &F->f, L->ol, err, sizeof err);
        if (rc != YLAY_OK) {
            ylay__colr_free(&F->colr);
            hb_face_destroy(hf);
            return ylay__fail(L, rc, "ysp_layout: %s: %s", F->name, err);
        }
    }
    F->G = colr ? F->colr.G : (uint32_t)F->f.n_glyphs;
    font = hb_font_create(hf);
    /* A COLR font goes in Skribidi's emoji family, which its emoji runs
     * (emoji presentation, VS16, ZWJ and flag sequences) take; every other
     * font in the default family. Fonts of one family keep the order added. */
    F->color = colr ? (hb_ot_color_has_paint(hf) ? 2 : 1) : 0;
    if (font) F->handle = skb_font_collection_add_hb_font(L->fc, F->name, font, colr ? SKB_FONT_FAMILY_EMOJI : SKB_FONT_FAMILY_DEFAULT, NULL);
    hb_font_destroy(font);
    hb_face_destroy(hf);
    if (!F->handle) {
        ylay__colr_free(&F->colr);
        return ylay__fail(L, YLAY_ERR_FORMAT, "ysp_layout: %s: HarfBuzz could not open face %d", F->name, face);
    }
    memset(&cd, 0, sizeof cd);
    cd.n_glyphs = F->G;
    cd.backward = true;   /* the rays are 18 to 27 % faster with them (ysp/gfx.h) */
    F->have = (uint8_t*)calloc(((size_t)F->G + 7) / 8, 1);
    if (!F->have || !yol_cset_open(&F->set, L->ol, &cd) ||
        (F->colr.n_paint && yol_cset_add_texels(&F->set, F->colr.paints, F->colr.n_paint) != 0)) {
        skb_font_collection_remove_font(L->fc, F->handle);
        yol_cset_close(&F->set);
        ylay__colr_free(&F->colr);
        free(F->have);
        return ylay__fail(L, YLAY_ERR_NOMEM, "ysp_layout: %s: no memory for its curve set", F->name);
    }
    ylay__sha256_hex((const uint8_t*)bytes, n, F->sha256);
    return L->n_fonts++;
}

int ylay_font_count(const ylay_lib* L) { return L ? L->n_fonts : 0; }

int ylay_font(const ylay_lib* L, int font, ylay_font_info* out) {
    const ylay__font* F;
    if (!L || !out || font < 0 || font >= L->n_fonts) return YLAY_ERR_ARG;
    F = &L->fonts[font];
    memset(out, 0, sizeof *out);
    out->name = F->name;
    out->face = F->face;
    out->sha256 = F->sha256;
    out->units_per_em = F->f.units_per_em;
    out->n_glyphs = F->f.n_glyphs;
    out->ascender = (double)F->f.ascender / F->f.units_per_em;
    out->descender = (double)F->f.descender / F->f.units_per_em;
    out->line_gap = (double)F->f.line_gap / F->f.units_per_em;
    out->set = &F->set;
    out->n_built = F->n_built;
    out->build_us = F->build_us;
    out->color = F->color;
    out->set_glyphs = F->G;
    out->n_color_glyphs = F->colr.n_color;
    out->n_refused = F->colr.n_refused;
    out->n_layers = F->colr.n_layers;
    out->n_layer_glyphs = F->colr.n_lg;
    out->n_paint_texels = F->colr.n_paint;
    out->color_us = F->colr.build_us;
    return YLAY_OK;
}

int ylay_take_new_glyphs(ylay_lib* L, int font, const uint32_t** glyphs) {
    ylay__font* F;
    uint32_t n;
    if (!L || !glyphs || font < 0 || font >= L->n_fonts) return YLAY_ERR_ARG;
    F = &L->fonts[font];
    n = F->n_fresh;
    if (!ylay__grow((void**)&F->taken, &F->cap_taken, n ? n : 1, sizeof *F->taken)) return YLAY_ERR_NOMEM;
    if (n) memcpy(F->taken, F->fresh, n * sizeof *F->fresh);
    F->n_fresh = 0;
    *glyphs = F->taken;
    return (int)n;
}

static int ylay__font_index(const ylay_lib* L, skb_font_handle_t h) {
    int i;
    for (i = 0; i < L->n_fonts; i++)
        if (L->fonts[i].handle == h) return i;
    return -1;
}

/* Marks set glyph g of font F (a font or a layer glyph) to be built, once. */
static int ylay__want(ylay__font* F, uint32_t g) {
    if (g >= F->G || (F->have[g >> 3] & (1u << (g & 7)))) return 1;
    if (!ylay__grow((void**)&F->todo, &F->cap_todo, F->n_todo + 1, sizeof *F->todo)) return 0;
    F->have[g >> 3] |= (uint8_t)(1u << (g & 7));
    F->todo[F->n_todo++] = g;
    return 1;
}

/* Builds set glyphs (font glyphs, then layer glyphs) into set. */
static int ylay__build_set(ylay_lib* L, ylay__font* F, yol_cset* set, const uint32_t* glyphs, uint32_t n, double tol) {
    uint32_t i, k, nf = 0, ng = (uint32_t)F->f.n_glyphs;
    int rc = YOL_OK;
    yol_path path;
    for (i = 0; i < n; i++) if (glyphs[i] < ng) nf++;
    if (nf == n) rc = yol_cset_add_font(set, &F->f, glyphs, n, tol);
    else {
        /* font glyphs in runs of the list, each layer glyph by its path */
        yol_path_init(&path, L->ol);
        for (i = 0; i < n && rc >= 0; i = k) {
            for (k = i; k < n && glyphs[k] < ng; k++) {}
            if (k > i) { rc = yol_cset_add_font(set, &F->f, glyphs + i, k - i, tol); continue; }
            if (glyphs[i] >= F->G) { rc = YOL_ERR_RANGE; k = i + 1; break; }
            yol_path_clear(&path);
            rc = ylay__colr_layer_path(&F->colr, &F->f, L->ol, glyphs[i], &path);
            if (rc >= 0) rc = yol_cset_add(set, glyphs[i], &path);
            k = i + 1;
        }
        yol_path_free(&path);
    }
    if (rc < 0) return ylay__fail(L, YLAY_ERR_FORMAT, "ysp_layout: %s: %s", F->name, yol_error(L->ol));
    return YLAY_OK;
}

/* Builds every font's wanted glyphs into its set and lists them as fresh. */
static int ylay__build(ylay_lib* L, ylay_block* b) {
    int i;
    for (i = 0; i < L->n_fonts; i++) {
        ylay__font* F = &L->fonts[i];
        double t0;
        if (!F->n_todo) continue;
        t0 = ylay__now_us();
        if (ylay__build_set(L, F, &F->set, F->todo, F->n_todo, 0) != YLAY_OK) return YLAY_ERR_FORMAT;
        t0 = ylay__now_us() - t0;
        F->build_us += t0;
        b->build_us += t0;
        if (!ylay__grow((void**)&F->fresh, &F->cap_fresh, F->n_fresh + F->n_todo, sizeof *F->fresh))
            return ylay__fail(L, YLAY_ERR_NOMEM, "ysp_layout: no memory");
        memcpy(F->fresh + F->n_fresh, F->todo, F->n_todo * sizeof *F->todo);
        F->n_fresh += F->n_todo;
        F->n_built += F->n_todo;
        b->n_new_glyphs += F->n_todo;
        F->n_todo = 0;
    }
    return YLAY_OK;
}

static int ylay__mapped(const ylay_lib* L, uint32_t cp) {
    int f;
    for (f = 0; f < L->n_fonts; f++)
        if (skb_font_collection_font_has_codepoint(L->fc, L->fonts[f].handle, cp)) return 1;
    return 0;
}

static int ylay__add_missing(ylay_lib* L, uint32_t cp, uint32_t at, uint32_t tag, int in_a_font) {
    ylay_missing* m;
    uint32_t k;
    for (k = 0; k < L->n_miss; k++)
        if (L->miss[k].codepoint == cp) {
            if (at < L->miss[k].offset) {   /* RTL runs give items right to left */
                L->miss[k].offset = at;
                L->miss[k].script[0] = (char)(tag >> 24); L->miss[k].script[1] = (char)(tag >> 16);
                L->miss[k].script[2] = (char)(tag >> 8);  L->miss[k].script[3] = (char)tag;
            }
            return 1;
        }
    if (!ylay__grow((void**)&L->miss, &L->cap_miss, L->n_miss + 1, sizeof *L->miss)) return 0;
    m = &L->miss[L->n_miss++];
    m->codepoint = cp;
    m->offset = at;
    m->script[0] = (char)(tag >> 24); m->script[1] = (char)(tag >> 16);
    m->script[2] = (char)(tag >> 8);  m->script[3] = (char)tag; m->script[4] = 0;
    m->in_a_font = in_a_font;
    return 1;
}

static int ylay__by_offset(const void* a, const void* b) {
    uint32_t x = ((const ylay_missing*)a)->offset, y = ((const ylay_missing*)b)->offset;
    return x < y ? -1 : x > y;
}

/* Records the characters of a glyph-0 cluster that no font of L maps; when
 * every one is mapped (a font maps it to glyph 0, or shaping gave glyph 0),
 * the cluster's first, flagged, so a message never claims what is false. */
static int ylay__note_missing(ylay_lib* L, const skb_layout_t* lay, const skb_layout_run_t* run, const skb_cluster_t* cl) {
    const uint32_t* text = skb_layout_get_text(lay);
    uint32_t tag = skb_script_to_iso15924_tag(run->script);
    int32_t i, end = cl->text_offset + cl->text_count;
    int any = 0;
    for (i = cl->text_offset; i < end; i++)
        if (!ylay__mapped(L, text[i])) {
            any = 1;
            if (!ylay__add_missing(L, text[i], (uint32_t)i, tag, 0)) return 0;
        }
    if (!any && cl->text_count > 0) return ylay__add_missing(L, text[cl->text_offset], (uint32_t)cl->text_offset, tag, 1);
    return 1;
}

/* Appends lay's text runs to b as runs of one font and size each: a count
 * pass sizes the groups, a fill pass writes them, so every group's items are
 * contiguous for one ygfx_crun. colors: per content run, or NULL for color. */
/* The refusal of a color glyph used by a layout: the font, the cluster's
 * codepoints, the glyph and the feature. */
static int ylay__refused(ylay_lib* L, const skb_layout_t* lay, const ylay__font* F, const skb_cluster_t* cl, uint32_t gid) {
    const uint32_t* text = skb_layout_get_text(lay);
    char cps[96], why[300];
    size_t k = 0;
    int32_t i;
    cps[0] = 0;
    for (i = cl->text_offset; i < cl->text_offset + cl->text_count && k + 12 < sizeof cps; i++)
        k += (size_t)snprintf(cps + k, sizeof cps - k, "%sU+%04X", k ? " " : "", (unsigned)text[i]);
    ylay__colr_message(F->colr.ref[gid], F->colr.detail[gid], why, sizeof why);
    return ylay__fail(L, YLAY_ERR_REFUSED, "ysp_layout: %s: %s (glyph %u): %s", F->name, cps, (unsigned)gid, why);
}

/* The key of a glyph: one per font, size and paint, so a run is one draw. */
static int ylay__key_of(ylay_lib* L, int f, float size, uint32_t flags, uint32_t* n_keys) {
    uint32_t k;
    for (k = 0; k < *n_keys; k++)
        if (L->keys[k].font == f && L->keys[k].size == size && L->keys[k].flags == flags) return (int)k;
    if (!ylay__grow((void**)&L->keys, &L->cap_keys, *n_keys + 1, sizeof *L->keys)) return -1;
    L->keys[k].font = f; L->keys[k].size = size; L->keys[k].n = 0; L->keys[k].flags = flags;
    (*n_keys)++;
    return (int)k;
}

static int ylay__convert(ylay_lib* L, const skb_layout_t* lay, float ox, float oy,
                           const float* colors, int n_colors, float color, ylay_block* b) {
    const skb_layout_run_t* runs = skb_layout_get_layout_runs(lay);
    const skb_glyph_t* glyphs = skb_layout_get_glyphs(lay);
    const skb_cluster_t* clusters = skb_layout_get_clusters(lay);
    const skb_layout_line_t* lines = skb_layout_get_lines(lay);
    int32_t n_runs = skb_layout_get_layout_runs_count(lay);
    int32_t n_lines = skb_layout_get_lines_count(lay);
    uint32_t n_keys = 0, base = b->n_items, total = 0, k, missing0 = b->n_missing;
    int32_t r, gi, li;
    L->n_miss = 0;
    for (r = 0; r < n_runs; r++) {
        const skb_layout_run_t* run = &runs[r];
        int f;
        if (run->type != SKB_CONTENT_RUN_UTF8 && run->type != SKB_CONTENT_RUN_UTF32) continue;
        f = ylay__font_index(L, run->font_handle);
        if (f < 0) return ylay__fail(L, YLAY_ERR_ARG, "ysp_layout: a run's font is not this library's");
        if (!L->fonts[f].color) {
            int kk = ylay__key_of(L, f, run->font_size, 0, &n_keys);
            if (kk < 0) return YLAY_ERR_NOMEM;
            L->keys[kk].n += (uint32_t)(run->glyph_range.end - run->glyph_range.start);
            continue;
        }
        for (gi = run->glyph_range.start; gi < run->glyph_range.end; gi++) {
            const ylay__font* F = &L->fonts[f];
            uint32_t g = glyphs[gi].gid;
            int paint = g < F->colr.n_glyphs && F->colr.is_color[g];
            int kk;
            if (paint && F->colr.ref[g]) return ylay__refused(L, lay, F, &clusters[glyphs[gi].cluster_idx], g);
            kk = ylay__key_of(L, f, run->font_size, paint ? YLAY_RUN_PAINT : 0u, &n_keys);
            if (kk < 0) return YLAY_ERR_NOMEM;
            L->keys[kk].n += paint ? F->colr.count[g] : 1u;
        }
    }
    for (k = 0; k < n_keys; k++) { L->keys[k].at = base + total; total += L->keys[k].n; }
    {   /* cluster shares cap_items with items */
        uint32_t old = b->cap_items;
        if (!ylay__grow((void**)&b->items, &b->cap_items, base + total, sizeof *b->items)) return YLAY_ERR_NOMEM;
        if (b->cap_items != old) {
            void* q = realloc(b->cluster, (size_t)b->cap_items * sizeof *b->cluster);
            if (!q) return YLAY_ERR_NOMEM;
            b->cluster = (uint32_t*)q;
        }
    }
    if (!ylay__grow((void**)&b->runs, &b->cap_runs, b->n_runs + n_keys, sizeof *b->runs)) return YLAY_ERR_NOMEM;
    for (k = 0; k < n_keys; k++) {
        ylay_run* o = &b->runs[b->n_runs + k];
        o->font = L->keys[k].font; o->size = L->keys[k].size; o->first = L->keys[k].at; o->n = L->keys[k].n;
        o->flags = L->keys[k].flags;
        b->fonts_used |= (uint64_t)1 << L->keys[k].font;
    }
    for (r = 0; r < n_runs; r++) {
        const skb_layout_run_t* run = &runs[r];
        ylay__font* F;
        float c = color;
        int f;
        if (run->type != SKB_CONTENT_RUN_UTF8 && run->type != SKB_CONTENT_RUN_UTF32) continue;
        f = ylay__font_index(L, run->font_handle);
        F = &L->fonts[f];
        for (k = 0; k < n_keys; k++)
            if (L->keys[k].font == f && L->keys[k].size == run->font_size && !L->keys[k].flags) break;
        if (colors && run->content_run_idx >= 0 && run->content_run_idx < n_colors) c = colors[run->content_run_idx];
        for (gi = run->glyph_range.start; gi < run->glyph_range.end; gi++) {
            const skb_glyph_t* g = &glyphs[gi];
            uint32_t at;
            ygfx_citem* it;
            if (F->color && g->gid < F->colr.n_glyphs && F->colr.is_color[g->gid]) {
                /* a color glyph: one item per layer, the layer's glyph and paint */
                const ylay_layer* ly = F->colr.layers + F->colr.first[g->gid];
                uint32_t kp, lj;
                for (kp = 0; kp < n_keys; kp++)
                    if (L->keys[kp].font == f && L->keys[kp].size == run->font_size && L->keys[kp].flags) break;
                for (lj = 0; lj < F->colr.count[g->gid]; lj++) {
                    at = L->keys[kp].at++;
                    it = &b->items[at];
                    memset(it, 0, sizeof *it);
                    it->x = ox + g->offset_x;
                    it->y = oy + g->offset_y;
                    it->glyph = (float)ly[lj].glyph;
                    it->scale = 1.0f;
                    it->gate = 1.0f;
                    it->color = (float)ly[lj].paint;
                    b->cluster[at] = (uint32_t)clusters[g->cluster_idx].text_offset;
                    if (!ylay__want(F, ly[lj].glyph)) return YLAY_ERR_NOMEM;
                }
                continue;
            }
            at = L->keys[k].at++;
            it = &b->items[at];
            memset(it, 0, sizeof *it);
            it->x = ox + g->offset_x;
            it->y = oy + g->offset_y;
            it->glyph = (float)g->gid;
            it->scale = 1.0f;
            it->gate = 1.0f;
            it->color = c;
            b->cluster[at] = (uint32_t)clusters[g->cluster_idx].text_offset;
            if (g->gid == 0) b->n_missing++;
            if (!ylay__want(F, g->gid)) return YLAY_ERR_NOMEM;
        }
    }
    b->n_items = base + total;
    /* a second pass, only when a glyph is 0: the first stays as lean as it
     * was (noting in it cost 1 % of a 500-codepoint layout, docs/layout.md) */
    if (b->n_missing != missing0) {
        for (r = 0; r < n_runs; r++) {
            const skb_layout_run_t* run = &runs[r];
            if (run->type != SKB_CONTENT_RUN_UTF8 && run->type != SKB_CONTENT_RUN_UTF32) continue;
            for (gi = run->glyph_range.start; gi < run->glyph_range.end; gi++)
                if (glyphs[gi].gid == 0 && !ylay__note_missing(L, lay, run, &clusters[glyphs[gi].cluster_idx])) return YLAY_ERR_NOMEM;
        }
        if (L->n_miss > 1) qsort(L->miss, L->n_miss, sizeof *L->miss, ylay__by_offset);
    }
    b->n_runs += n_keys;
    if (!ylay__grow((void**)&b->lines, &b->cap_lines, b->n_lines + (uint32_t)n_lines, sizeof *b->lines)) return YLAY_ERR_NOMEM;
    for (li = 0; li < n_lines; li++) {
        ylay_line* o = &b->lines[b->n_lines++];
        o->x = ox + lines[li].bounds.x;
        o->baseline = oy + lines[li].baseline;
        o->width = lines[li].bounds.width;
        o->ascent = -lines[li].ascender;
        o->descent = lines[li].descender;
        o->text_start = (uint32_t)lines[li].text_range.start;
        o->text_end = (uint32_t)lines[li].text_range.end;
    }
    {
        skb_rect2_t bb = skb_layout_get_bounds(lay);
        float w = ox + bb.x + bb.width, h = oy + bb.y + bb.height;
        if (w > b->w) b->w = w;
        if (h > b->h) b->h = h;
    }
    b->dir = skb_layout_get_resolved_direction(lay) == SKB_DIRECTION_RTL ? YLAY_DIR_RTL : YLAY_DIR_LTR;
    return YLAY_OK;
}

static void ylay__reset(ylay_block* b) {
    b->n_items = b->n_runs = b->n_lines = 0;
    b->w = b->h = 0;
    b->n_missing = 0;
    b->fonts_used = 0;
    b->build_us = 0;
    b->n_new_glyphs = 0;
}

int ylay_layout(ylay_lib* L, const char* text, int n, const ylay_style* st,
                  const ylay_span* spans, int n_spans, ylay_block* b) {
    skb_attribute_t base[4];
    skb_layout_params_t params;
    uint32_t len, pos = 0, nr = 0;
    int i, nb = 0, rc;
    double t0 = ylay__now_us(), t1;
    if (!L || !text || !st || !b || !(st->size > 0) || st->width < 0 || n_spans < 0 || (n_spans && !spans))
        return L ? ylay__fail(L, YLAY_ERR_ARG, "ysp_layout: a NULL argument, a size <= 0 or a negative width") : YLAY_ERR_ARG;
    if (!L->n_fonts) return ylay__fail(L, YLAY_ERR_ARG, "ysp_layout: no font");
    len = n < 0 ? (uint32_t)strlen(text) : (uint32_t)n;
    for (i = 0; i < n_spans; i++)
        if (spans[i].start > spans[i].end || spans[i].end > len || (i && spans[i].start < spans[i - 1].end))
            return ylay__fail(L, YLAY_ERR_ARG, "ysp_layout: span %d is out of order or past the text", i);
    /* content runs: the text between spans gets the style, each span its own;
     * 2 per span plus 1 bounds the count */
    {   /* ccolor shares cap_cruns with cruns */
        uint32_t old = L->cap_cruns;
        if (!ylay__grow((void**)&L->cruns, &L->cap_cruns, 2 * (uint32_t)n_spans + 1, sizeof *L->cruns)) return YLAY_ERR_NOMEM;
        if (L->cap_cruns != old) {
            void* q = realloc(L->ccolor, (size_t)L->cap_cruns * sizeof *L->ccolor);
            if (!q) return YLAY_ERR_NOMEM;
            L->ccolor = (float*)q;
        }
    }
    if (!ylay__grow((void**)&L->cattrs, &L->cap_cattrs, 2 * L->cap_cruns, sizeof *L->cattrs)) return YLAY_ERR_NOMEM;
    for (i = 0; i <= n_spans; i++) {
        uint32_t s = i < n_spans ? spans[i].start : len;
        if (s > pos || (i == n_spans && nr == 0)) {   /* the style's text before span i */
            skb_attribute_t* a = &L->cattrs[2 * nr];
            a[0] = skb_attribute_make_font_size(st->size);
            a[1] = skb_attribute_make_lang(st->lang ? st->lang : "");
            L->cruns[nr] = skb_content_run_make_utf8(text + pos, (int32_t)(s - pos),
                                                     (skb_attribute_set_t){ a, 2, 0, NULL }, 0);
            L->ccolor[nr++] = st->color;
        }
        if (i < n_spans) {
            const ylay_span* sp = &spans[i];
            skb_attribute_t* a = &L->cattrs[2 * nr];
            a[0] = skb_attribute_make_font_size(sp->size > 0 ? sp->size : st->size);
            a[1] = skb_attribute_make_lang(sp->lang ? sp->lang : st->lang ? st->lang : "");
            L->cruns[nr] = skb_content_run_make_utf8(text + sp->start, (int32_t)(sp->end - sp->start),
                                                     (skb_attribute_set_t){ a, 2, 0, NULL }, 0);
            L->ccolor[nr++] = sp->color >= 0 ? sp->color : st->color;
            pos = sp->end;
        }
    }
    base[nb++] = skb_attribute_make_text_wrap(st->width > 0 ? SKB_WRAP_WORD : SKB_WRAP_NONE);
    base[nb++] = skb_attribute_make_horizontal_align(st->align == YLAY_ALIGN_END ? SKB_ALIGN_END
                                                     : st->align == YLAY_ALIGN_CENTER ? SKB_ALIGN_CENTER : SKB_ALIGN_START);
    base[nb++] = skb_attribute_make_text_base_direction(st->dir == YLAY_DIR_LTR ? SKB_DIRECTION_LTR
                                                        : st->dir == YLAY_DIR_RTL ? SKB_DIRECTION_RTL : SKB_DIRECTION_AUTO);
    if (st->line_height > 0) base[nb++] = skb_attribute_make_line_height(SKB_LINE_HEIGHT_METRICS_RELATIVE, st->line_height);
    memset(&params, 0, sizeof params);
    params.font_collection = L->fc;
    params.attribute_collection = L->ac;
    params.layout_width = st->width > 0 ? st->width : -1.0f;   /* < 0: unbounded */
    params.layout_height = -1.0f;
    params.layout_attributes = (skb_attribute_set_t){ base, nb, 0, NULL };
    if (!L->layout) {
        L->layout = skb_layout_create(&params);
        if (!L->layout) return ylay__fail(L, YLAY_ERR_NOMEM, "ysp_layout: no memory for the layout");
    }
    skb_layout_set_from_runs(L->layout, L->temp, &params, L->cruns, (int32_t)nr);
    skb_temp_alloc_reset(L->temp);
    ylay__reset(b);
    b->versions = &L->versions;
    rc = ylay__convert(L, L->layout, 0, 0, L->ccolor, (int)nr, st->color, b);
    if (rc != YLAY_OK) return rc < 0 && !L->error[0] ? ylay__fail(L, rc, "ysp_layout: no memory") : rc;
    t1 = ylay__now_us();
    b->layout_us = t1 - t0;
    return ylay__build(L, b);
}

int ylay_from_skb(ylay_lib* L, const struct skb_layout_t* lay, float x, float y,
                    float color, int append, ylay_block* b) {
    double t0 = ylay__now_us();
    int rc;
    if (!L || !lay || !b) return YLAY_ERR_ARG;
    if (!append) ylay__reset(b);
    b->versions = &L->versions;
    rc = ylay__convert(L, lay, x, y, NULL, 0, color, b);
    if (rc != YLAY_OK) return rc < 0 && !L->error[0] ? ylay__fail(L, rc, "ysp_layout: no memory") : rc;
    b->layout_us = ylay__now_us() - t0;
    return ylay__build(L, b);
}

void ylay_block_free(ylay_block* b) {
    if (!b) return;
    free(b->items); free(b->cluster); free(b->runs); free(b->lines);
    memset(b, 0, sizeof *b);
}

int ylay_stamp(const ylay_lib* L, const ylay_block* b, char* buf, size_t cap) {
    const ylay_versions* v;
    size_t need;
    int i, k, first = 1;
    char tmp[512];
    if (!L) return YLAY_ERR_ARG;
    v = &L->versions;
    /* Straight into buf: the patch list outgrows any fixed buffer (six
     * hashes made the line 600 bytes, and a 512-byte copy cut it). */
    k = snprintf(buf && cap ? buf : NULL, buf ? cap : 0,
                 "ylay=%s skribidi=%s patch=%s harfbuzz=%s sheenbidi=%s libunibreak=%s budouxc=%s fonts=",
                 v->ylay, v->skribidi, v->skribidi_patch, v->harfbuzz, v->sheenbidi, v->libunibreak, v->budouxc);
    need = (size_t)(k < 0 ? 0 : k);
    for (i = 0; i < L->n_fonts; i++) {
        const ylay__font* F = &L->fonts[i];
        if (b && !(b->fonts_used & ((uint64_t)1 << i))) continue;
        k = snprintf(tmp, sizeof tmp, "%s%s#%d=%s", first ? "" : ",", F->name, F->face, F->sha256);
        first = 0;
        if (k > 0) {
            if (buf && need < cap) snprintf(buf + need, cap - need, "%s", tmp);
            need += (size_t)k;
        }
    }
    return (int)need;
}

int ylay_missing_chars(const ylay_lib* L, ylay_missing* out, int cap) {
    int i;
    if (!L || cap < 0 || (cap && !out)) return YLAY_ERR_ARG;
    for (i = 0; i < cap && (uint32_t)i < L->n_miss; i++) out[i] = L->miss[i];
    return (int)L->n_miss;
}

struct skb_font_collection_t* ylay_skb_fonts(ylay_lib* L) { return L ? L->fc : NULL; }
struct skb_attribute_collection_t* ylay_skb_attributes(ylay_lib* L) { return L ? L->ac : NULL; }
struct skb_temp_alloc_t* ylay_skb_temp(ylay_lib* L) { return L ? L->temp : NULL; }

/* --- color glyphs (v0.3) ------------------------------------------------------- */

int ylay_color_glyph(ylay_lib* L, int font, uint32_t glyph, const ylay_layer** layers) {
    const ylay__font* F;
    char why[300];
    if (!L || !layers || font < 0 || font >= L->n_fonts) return YLAY_ERR_ARG;
    F = &L->fonts[font];
    *layers = NULL;
    if (!F->color || glyph >= F->colr.n_glyphs || !F->colr.is_color[glyph]) return 0;
    if (F->colr.ref[glyph]) {
        ylay__colr_message(F->colr.ref[glyph], F->colr.detail[glyph], why, sizeof why);
        return ylay__fail(L, YLAY_ERR_REFUSED, "ysp_layout: %s: glyph %u: %s", F->name, (unsigned)glyph, why);
    }
    *layers = F->colr.layers + F->colr.first[glyph];
    return (int)F->colr.count[glyph];
}

int ylay_color_paints(const ylay_lib* L, int font, const float** texels) {
    const ylay__font* F;
    if (!L || !texels || font < 0 || font >= L->n_fonts) return YLAY_ERR_ARG;
    F = &L->fonts[font];
    *texels = F->colr.paints;
    return (int)F->colr.n_paint;
}

int ylay_open_set(ylay_lib* L, int font, yol_cset* set, yol_ctx* ctx) {
    const ylay__font* F;
    yol_cset_desc cd;
    if (!L || !set || !ctx || font < 0 || font >= L->n_fonts) return YLAY_ERR_ARG;
    F = &L->fonts[font];
    memset(&cd, 0, sizeof cd);
    cd.n_glyphs = F->G;
    cd.backward = true;
    if (!yol_cset_open(set, ctx, &cd)) return ylay__fail(L, YLAY_ERR_NOMEM, "ysp_layout: %s: %s", F->name, yol_error(ctx));
    if (F->colr.n_paint && yol_cset_add_texels(set, F->colr.paints, F->colr.n_paint) != 0)
        return ylay__fail(L, YLAY_ERR_NOMEM, "ysp_layout: %s: %s", F->name, yol_error(ctx));
    return YLAY_OK;
}

int ylay_build_glyphs(ylay_lib* L, int font, yol_cset* set, const uint32_t* glyphs, uint32_t n, double tol) {
    ylay__font* F;
    uint32_t* all = NULL, i;
    int rc;
    if (!L || !set || font < 0 || font >= L->n_fonts) return YLAY_ERR_ARG;
    F = &L->fonts[font];
    if (!glyphs) {
        all = (uint32_t*)malloc(sizeof(uint32_t) * (F->G ? F->G : 1));
        if (!all) return ylay__fail(L, YLAY_ERR_NOMEM, "ysp_layout: no memory");
        for (i = 0; i < F->G; i++) all[i] = i;
        glyphs = all; n = F->G;
    }
    rc = ylay__build_set(L, F, set, glyphs, n, tol);
    free(all);
    return rc;
}
