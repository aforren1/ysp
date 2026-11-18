/* colr_fuzz.c - fuzz target for the layout library's color glyphs
 * (pack/colr.c through ysp/layout.h): a font file is untrusted input to the
 * pack tool and to the bindings, and ylay_add_font() flattens every color
 * glyph of a COLR font with HarfBuzz's paint API, bakes transforms, cuts
 * clip boxes and checks group opacity for overlaps. The input is the font.
 *
 * Every input must return without a sanitizer report. A refused font must
 * carry a message. An accepted font's color glyphs must give layers whose
 * glyphs are in the set's table and whose paint indexes start a whole
 * record of a known kind (1 solid, 2 linear, 3 radial) with 1 to 16 stops
 * in the paint texels, every value finite; a refused glyph must carry a
 * message; the set from ylay_open_set() with every glyph built must pass
 * ygfx_cset_check() (or the build must fail with a message); with the
 * layout argument, a layout of a few emoji and letters must succeed or
 * fail with a message.
 *
 * libFuzzer, clang (link the layout library and its vendored stack, as
 * CMake's colr_fuzz target does).
 * Self-driven, gcc: CMake target colr_fuzz (YSP_BUILD_LAYOUT, tests on):
 * random mutations of the seed fonts (tests/adapt/colr_font_build.h),
 * a third of them inside the COLR table; on this kernel ASan needs
 * setarch -R:
 *     setarch -R build/sanitizers/pack/colr_fuzz 200000 [seed [layout]]
 * Replay of font files: colr_fuzz --replay FILES.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"
#include "ysp/layout.h"
#include "../adapt/colr_font_build.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static yol_ctx g_ol;
/* Lay text out too (third argument "layout"). Off by default: under a
 * Debug build Skribidi asserts on a shaped glyph id above 0xFFFF, which a
 * damaged cmap or GSUB gives (skb_layout.c, skb__shape_run); a Release
 * build keeps the low 16 bits, and the glue drops an id past the set. */
static int g_layout;
static long g_fonts, g_refused_fonts, g_color, g_refused_glyphs, g_layers, g_sets;

static void fail(const char* what) {
    fprintf(stderr, "colr_fuzz: %s\n", what);
    abort();
}

static void one(const uint8_t* d, size_t n) {
    ylay_lib* L = NULL;
    ylay_font_info fi;
    const float* pt;
    int f, np;
    uint32_t g;
    if (!g_ol.inited) yol_open(&g_ol, NULL);
    if (ylay_create(&L, &(ylay_desc){ .ol = &g_ol }) != YLAY_OK) fail("create");
    f = ylay_add_font(L, n ? d : (const uint8_t*)"", n, 0, "fuzz");
    g_fonts++;
    if (f < 0) {
        if (!ylay_error(L)[0] && f != YLAY_ERR_ARG) fail("a refused font without a message");
        g_refused_fonts++;
        ylay_destroy(L);
        return;
    }
    memset(&fi, 0, sizeof fi);
    ylay_font(L, f, &fi);
    np = ylay_color_paints(L, f, &pt);
    if (np < 0 || (uint32_t)np != fi.n_paint_texels) fail("paint texel count");
    for (g = 0; g < (uint32_t)fi.n_glyphs; g++) {
        const ylay_layer* ly;
        int k, nl = ylay_color_glyph(L, f, g, &ly);
        if (nl == YLAY_ERR_REFUSED) {
            if (!ylay_error(L)[0]) fail("a refused glyph without a message");
            g_refused_glyphs++;
            continue;
        }
        if (nl < 0) fail("ylay_color_glyph");
        if (nl) g_color++;
        g_layers += nl;
        for (k = 0; k < nl; k++) {
            const float* r;
            int kind, ns, size, q;
            if (ly[k].glyph >= fi.set_glyphs) { fprintf(stderr, "glyph %u layer %d: %u of %u (%u font glyphs)\n", g, k, ly[k].glyph, fi.set_glyphs, (unsigned)fi.n_glyphs); fail("a layer glyph past the set"); }
            if (ly[k].paint + 2 > (uint32_t)np) fail("a paint index past the texels");
            r = pt + 4 * (size_t)ly[k].paint;
            kind = (int)r[0];
            ns = (int)r[1];
            if (r[0] != (float)kind || kind < 1 || kind > 3) fail("a paint record of an unknown kind");
            size = kind == 1 ? 2 : (kind == 2 ? 2 : 3) + (ns + 3) / 4 + ns;
            if (kind != 1 && (ns < 1 || ns > 16)) fail("a gradient's stop count");
            if (ly[k].paint + (uint32_t)size > (uint32_t)np) fail("a paint record past the texels");
            for (q = 0; q < 4 * size; q++)
                if (!isfinite(r[q])) fail("a paint value that is not finite");
        }
    }
    if (fi.color) {   /* the whole set, as the pack tool builds it */
        yol_cset set;
        if (ylay_open_set(L, f, &set, &g_ol) == YLAY_OK) {
            if (ylay_build_glyphs(L, f, &set, NULL, 0, 0) == YLAY_OK) {
                ygfx_cset_desc cd;
                char msg[300];
                memset(&cd, 0, sizeof cd);
                cd.texels = set.texels; cd.n_texels = set.n_texels; cd.words = set.words; cd.n_words = set.n_words;
                if (ygfx_cset_check(&cd, msg, sizeof msg) != YGFX_OK) { fprintf(stderr, "%s\n", msg); fail("the set fails its check"); }
                if (np && memcmp(set.texels, pt, 16 * (size_t)np)) fail("the set's first texels are not the paints");
                g_sets++;
            } else if (!ylay_error(L)[0]) fail("a set build failed without a message");
            yol_cset_close(&set);
        }
    }
    if (g_layout) {
        static const char text[] = "a\xF0\x9F\x98\x80 \xE2\x98\x83\xEF\xB8\x8F \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9 \xEE\x80\x85\xEE\x80\x8A";
        ylay_block b;
        memset(&b, 0, sizeof b);
        if (ylay_layout(L, text, -1, &(ylay_style){ .size = 32 }, NULL, 0, &b) != YLAY_OK && !ylay_error(L)[0]) fail("a layout failed without a message");
        ylay_block_free(&b);
    }
    ylay_destroy(L);
}

#if defined(YSP_FUZZ_SELF)
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t mix(void) {
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
/* The COLR table's offset and length in a font, or 0. */
static size_t colr_at(const uint8_t* d, size_t n, size_t* len) {
    uint32_t nt = n >= 12 ? ((uint32_t)d[4] << 8 | d[5]) : 0, i;
    for (i = 0; i < nt && 12 + 16 * (size_t)i + 16 <= n; i++) {
        const uint8_t* r = d + 12 + 16 * i;
        if (!memcmp(r, "COLR", 4)) {
            *len = (size_t)r[12] << 24 | (size_t)r[13] << 16 | (size_t)r[14] << 8 | r[15];
            return (size_t)r[8] << 24 | (size_t)r[9] << 16 | (size_t)r[10] << 8 | r[11];
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    static uint8_t buf[1 << 16];
    cfb_buf s0 = { 0, 0, 0 }, s1 = { 0, 0, 0 };
    long iters = argc > 1 ? atol(argv[1]) : 10000, it;
    if (argc > 1 && !strcmp(argv[1], "--replay")) {
        int i;
        for (i = 2; i < argc; i++) {
            FILE* fp = fopen(argv[i], "rb");
            size_t n;
            if (!fp) { fprintf(stderr, "cannot read %s\n", argv[i]); return 1; }
            n = fread(buf, 1, sizeof buf, fp);
            fclose(fp);
            one(buf, n);
        }
        printf("colr_fuzz: replayed %d files\n", argc - 2);
        return 0;
    }
    if (argc > 2) g_rng = (uint64_t)strtoull(argv[2], NULL, 10);
    if (argc > 3) g_layout = !strcmp(argv[3], "layout");
    cfb_emoji_v0(&s0);
    cfb_emoji_v1(&s1);
    one(s0.d, s0.n);
    one(s1.d, s1.n);
    for (it = 0; it < iters; it++) {
        const cfb_buf* sd = mix() & 1 ? &s1 : &s0;
        size_t n = sd->n, cl = 0, co = colr_at(sd->d, sd->n, &cl), k;
        int edits = 1 + (int)(mix() % 8), e, inside = co && cl && mix() % 3 == 0;
        if (n > sizeof buf) continue;
        memcpy(buf, sd->d, n);
        for (e = 0; e < edits; e++) {
            size_t at = inside ? co + (size_t)(mix() % cl) : (size_t)(mix() % n);
            switch (mix() % 6) {
            case 0: case 1: buf[at] = (uint8_t)mix(); break;
            case 2: buf[at] ^= (uint8_t)(1u << (mix() % 8)); break;
            case 3: buf[at] = (uint8_t)(mix() % 4 ? buf[at] + 1 : 0xFF); break;   /* counts and offsets one off */
            case 4: if (at + 4 <= n) for (k = 0; k < 4; k++) buf[at + k] = (uint8_t)(mix() % 3 ? 0 : 0xFF); break;
            default: if (!inside && n + 1 < sizeof buf) { memmove(buf + at + 1, buf + at, n - at); buf[at] = (uint8_t)mix(); n++; } break;
            }
        }
        one(buf, n);
    }
    printf("colr_fuzz: %ld inputs: %ld fonts refused; %ld color glyphs flattened (%ld layers), %ld refused; %ld sets built and checked\n",
           g_fonts, g_refused_fonts, g_color, g_layers, g_refused_glyphs, g_sets);
    free(s0.d); free(s1.d);
    yol_close(&g_ol);
    return 0;
}
#else
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    g_layout = 1;
    one(data, size);
    return 0;
}
#endif
