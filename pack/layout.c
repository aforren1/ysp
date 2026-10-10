/* layout.c - see ysp/layout.h and docs/layout.md. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L   /* clock_gettime under -std=c11 */
#endif
#include "ysp/layout.h"

#include <hb.h>
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
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
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
} ylay__font;

typedef struct ylay__key { int font; float size; uint32_t n, at; } ylay__key;

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

static ylay_versions ylay__static_versions = {
    YLAY_VERSION_STRING, YLAY_SKRIBIDI_COMMIT, YLAY_PATCHES, "",
    YLAY_SHEENBIDI_COMMIT, YLAY_LIBUNIBREAK_TAG, YLAY_BUDOUXC_COMMIT,
};

const ylay_versions* ylay_get_versions(void) {
    ylay__static_versions.harfbuzz = hb_version_string();
    return &ylay__static_versions;
}

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
        free(F->have); free(F->fresh); free(F->todo); free(F->taken);
    }
    if (L->fc) skb_font_collection_destroy(L->fc);
    if (L->ac) skb_attribute_collection_destroy(L->ac);
    if (L->temp) skb_temp_alloc_destroy(L->temp);
    free(L->cruns); free(L->cattrs); free(L->ccolor); free(L->keys);
    free(L);
}

const char* ylay_error(const ylay_lib* L) { return L ? L->error : ""; }

int ylay_add_font(ylay_lib* L, const void* bytes, size_t n, int face, const char* name) {
    ylay__font* F;
    hb_blob_t* blob;
    hb_face_t* hf;
    hb_font_t* font;
    char err[200];
    yol_cset_desc cd;
    if (!L || !bytes || n == 0 || n > 0x7fffffffu || face < 0) return YLAY_ERR_ARG;
    if (L->n_fonts == YLAY_MAX_FONTS) return ylay__fail(L, YLAY_ERR_FULL, "ysp_layout: %d fonts already", YLAY_MAX_FONTS);
    F = &L->fonts[L->n_fonts];
    memset(F, 0, sizeof *F);
    snprintf(F->name, sizeof F->name, "%s", name ? name : "font");
    F->face = face;
    /* ysp/outline.h reads the outlines; a font it refuses (CFF2, no outlines)
     * cannot become a curve set, so it is refused here, by its reason. */
    if (yol_font_view(&F->f, bytes, n, face, err, sizeof err) < 0)
        return ylay__fail(L, YLAY_ERR_FORMAT, "ysp_layout: %s: %s", F->name, err);
    blob = hb_blob_create_or_fail((const char*)bytes, (unsigned)n, HB_MEMORY_MODE_READONLY, NULL, NULL);
    hf = blob ? hb_face_create_or_fail(blob, (unsigned)face) : NULL;
    font = hf ? hb_font_create(hf) : NULL;
    if (font) F->handle = skb_font_collection_add_hb_font(L->fc, F->name, font, SKB_FONT_FAMILY_DEFAULT, NULL);
    hb_font_destroy(font);
    hb_face_destroy(hf);
    hb_blob_destroy(blob);
    if (!F->handle) return ylay__fail(L, YLAY_ERR_FORMAT, "ysp_layout: %s: HarfBuzz could not open face %d", F->name, face);
    memset(&cd, 0, sizeof cd);
    cd.n_glyphs = (uint32_t)F->f.n_glyphs;
    cd.backward = true;   /* the rays are 18 to 27 % faster with them (ysp/gfx.h) */
    F->have = (uint8_t*)calloc(((size_t)F->f.n_glyphs + 7) / 8, 1);
    if (!F->have || !yol_cset_open(&F->set, L->ol, &cd)) {
        skb_font_collection_remove_font(L->fc, F->handle);
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

/* Marks glyph g of font F to be built, once. */
static int ylay__want(ylay__font* F, uint32_t g) {
    if (g >= (uint32_t)F->f.n_glyphs || (F->have[g >> 3] & (1u << (g & 7)))) return 1;
    if (!ylay__grow((void**)&F->todo, &F->cap_todo, F->n_todo + 1, sizeof *F->todo)) return 0;
    F->have[g >> 3] |= (uint8_t)(1u << (g & 7));
    F->todo[F->n_todo++] = g;
    return 1;
}

/* Builds every font's wanted glyphs into its set and lists them as fresh. */
static int ylay__build(ylay_lib* L, ylay_block* b) {
    int i;
    for (i = 0; i < L->n_fonts; i++) {
        ylay__font* F = &L->fonts[i];
        double t0;
        if (!F->n_todo) continue;
        t0 = ylay__now_us();
        if (yol_cset_add_font(&F->set, &F->f, F->todo, F->n_todo, 0) != YOL_OK)
            return ylay__fail(L, YLAY_ERR_FORMAT, "ysp_layout: %s: %s", F->name, yol_error(L->ol));
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

/* Appends lay's text runs to b as runs of one font and size each: a count
 * pass sizes the groups, a fill pass writes them, so every group's items are
 * contiguous for one ygfx_crun. colors: per content run, or NULL for color. */
static int ylay__convert(ylay_lib* L, const skb_layout_t* lay, float ox, float oy,
                           const float* colors, int n_colors, float color, ylay_block* b) {
    const skb_layout_run_t* runs = skb_layout_get_layout_runs(lay);
    const skb_glyph_t* glyphs = skb_layout_get_glyphs(lay);
    const skb_cluster_t* clusters = skb_layout_get_clusters(lay);
    const skb_layout_line_t* lines = skb_layout_get_lines(lay);
    int32_t n_runs = skb_layout_get_layout_runs_count(lay);
    int32_t n_lines = skb_layout_get_lines_count(lay);
    uint32_t n_keys = 0, base = b->n_items, total = 0, k;
    int32_t r, gi, li;
    for (r = 0; r < n_runs; r++) {
        const skb_layout_run_t* run = &runs[r];
        int f;
        if (run->type != SKB_CONTENT_RUN_UTF8 && run->type != SKB_CONTENT_RUN_UTF32) continue;
        f = ylay__font_index(L, run->font_handle);
        if (f < 0) return ylay__fail(L, YLAY_ERR_ARG, "ysp_layout: a run's font is not this library's");
        for (k = 0; k < n_keys; k++)
            if (L->keys[k].font == f && L->keys[k].size == run->font_size) break;
        if (k == n_keys) {
            if (!ylay__grow((void**)&L->keys, &L->cap_keys, n_keys + 1, sizeof *L->keys)) return YLAY_ERR_NOMEM;
            L->keys[k].font = f; L->keys[k].size = run->font_size; L->keys[k].n = 0;
            n_keys++;
        }
        L->keys[k].n += (uint32_t)(run->glyph_range.end - run->glyph_range.start);
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
            if (L->keys[k].font == f && L->keys[k].size == run->font_size) break;
        if (colors && run->content_run_idx >= 0 && run->content_run_idx < n_colors) c = colors[run->content_run_idx];
        for (gi = run->glyph_range.start; gi < run->glyph_range.end; gi++) {
            const skb_glyph_t* g = &glyphs[gi];
            uint32_t at = L->keys[k].at++;
            ygfx_citem* it = &b->items[at];
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
    k = snprintf(tmp, sizeof tmp, "ylay=%s skribidi=%s patch=%s harfbuzz=%s sheenbidi=%s libunibreak=%s budouxc=%s fonts=",
                 v->ylay, v->skribidi, v->skribidi_patch, v->harfbuzz, v->sheenbidi, v->libunibreak, v->budouxc);
    need = (size_t)(k < 0 ? 0 : k);
    if (buf && cap) snprintf(buf, cap, "%s", tmp);
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

struct skb_font_collection_t* ylay_skb_fonts(ylay_lib* L) { return L ? L->fc : NULL; }
struct skb_attribute_collection_t* ylay_skb_attributes(ylay_lib* L) { return L ? L->ac : NULL; }
struct skb_temp_alloc_t* ylay_skb_temp(ylay_lib* L) { return L ? L->temp : NULL; }
