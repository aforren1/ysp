/* emoji_bench.c - what color glyphs cost, and what text and images cost
 * before and after them (docs/gfx.md, "v0.22.0"; docs/pack.md 18).
 *
 * Usage:
 *   emoji_bench --info FONT...
 *       per COLR font: ylay_add_font()'s time (the flattening), its color
 *       glyphs, refusals, layers, layer glyphs and paint texels; the whole
 *       set built as the pack tool builds it (ylay_open_set, every glyph):
 *       the time and the bytes
 *   emoji_bench --frames [--emoji FONT] [--mono FONT] [--frames N]
 *               [--only NAME] [--csv PATH]
 *       a 1920 x 1080 window on the display; each scene for N frames
 *       (default 600): host time from ygfx_begin() to ygfx_end()'s return,
 *       and GPU time from a GL_EXT_disjoint_timer_query around them, read
 *       4 frames later (both per frame; median and p99). Scenes:
 *         images     12 full-size RGBA8 images, nearest (gfx.md v0.21.0)
 *         page       a page of monochrome text, 16 px (2,000 glyphs)
 *         mono100    100 monochrome glyphs at 48 px
 *         mono600    one monochrome glyph at 600 px
 *         emoji100   100 emoji of FONT at 48 px (the first 100 the font
 *                    draws, in codepoint order)
 *         emoji600   one emoji at 600 px: U+1F600, else the first after it
 *         emoji600max the font's emoji with the most layers, at 600 px
 *         empty      nothing: the output stage alone
 *       The monochrome scenes use ygfx_text_make() (TEXT), so the same
 *       source builds against an older ysp/gfx.h with -DBENCH_OLD, which
 *       leaves the emoji scenes and the layout library out.
 * Take the measurement lock and run nothing else (CLAUDE.md).
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#ifndef BENCH_OLD
#include "ysp/layout.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_TIME_ELAPSED_EXT 0x88BF
#define GL_QUERY_RESULT_EXT 0x8866
#define GL_GPU_DISJOINT_EXT 0x8FBB
static void (*glGenQueries_)(int, unsigned*);
static void (*glBeginQuery_)(unsigned, unsigned);
static void (*glEndQuery_)(unsigned);
static void (*glGetQueryObjectui64v_)(unsigned, unsigned, uint64_t*);
static void (*glGetIntegerv_)(unsigned, int*);

static yscr_screen scr;
static ygfx_gfx gfx;

static unsigned char* read_file(const char* path, size_t* n) {
    FILE* fp = fopen(path, "rb");
    unsigned char* d;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    *n = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    d = (unsigned char*)malloc(*n ? *n : 1);
    if (d && fread(d, 1, *n, fp) != *n) { free(d); d = NULL; }
    fclose(fp);
    return d;
}

static int cmp_d(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }
static double pct(double* v, int n, double p) {
    qsort(v, (size_t)n, sizeof *v, cmp_d);
    return n ? v[(int)(p * (n - 1) + 0.5)] : 0;
}

#define MAXF 4000
static double host_ms[MAXF], gpu_ms[MAXF];
static FILE* g_csv;

/* Runs the stimuli for n frames; prints and records host and GPU time. */
static void scene(const char* name, const ygfx_stim* s, int ns, int n) {
    unsigned q[4];
    int i, ng = 0, disjoint = 0;
    yscr_frame f;
    if (n > MAXF) n = MAXF;
    yscr_bind(&scr);
    if (glGenQueries_) glGenQueries_(4, q);
    for (i = 0; i < n + 4; i++) {
        int64_t t0, t1;
        if (yscr_begin(&scr, &f) != YSCR_OK) break;
        if (glBeginQuery_ && i < n) glBeginQuery_(GL_TIME_ELAPSED_EXT, q[i & 3]);
        t0 = yrt_now_ns();
        ygfx_begin(&gfx, &f);
        if (i < n && ns) ygfx_draw_n(&gfx, s, ns);
        ygfx_end(&gfx);
        t1 = yrt_now_ns();
        if (glEndQuery_ && i < n) glEndQuery_(GL_TIME_ELAPSED_EXT);
        if (i < n) host_ms[i] = (double)(t1 - t0) * 1e-6;
        if (glGetQueryObjectui64v_ && i >= 4) {
            uint64_t ns_ = 0;
            int dj = 0;
            if (glGetIntegerv_) glGetIntegerv_(GL_GPU_DISJOINT_EXT, &dj);
            disjoint += dj != 0;
            glGetQueryObjectui64v_(q[(i - 4) & 3], GL_QUERY_RESULT_EXT, &ns_);
            gpu_ms[ng++] = (double)ns_ * 1e-6;
        }
        yscr_flip(&scr);
    }
    {
        double hm = pct(host_ms, n, 0.5), h99 = pct(host_ms, n, 0.99), gm = pct(gpu_ms, ng, 0.5), g99 = pct(gpu_ms, ng, 0.99);
        printf("| %-28s | %6d | %8.3f | %8.3f | %8.3f | %8.3f |%s\n", name, n, hm, h99, gm, g99, disjoint ? " disjoint" : "");
        if (g_csv) fprintf(g_csv, "%s,%d,%.4f,%.4f,%.4f,%.4f\n", name, n, hm, h99, gm, g99);
        fflush(stdout);
    }
}

static int want(const char* only, const char* name) { return !only || strstr(only, name) != NULL; }

/* ygfx_text_make()'s text of n copies of a string, wrapped. */
static int mono_text(ygfx_text* t, const char* font, const char* unit, int n, float size, float max_w, float x, float y) {
    ygfx_text_desc d;
    size_t ul = strlen(unit), k;
    char* s = (char*)malloc(ul * (size_t)n + 1);
    int rc;
    for (k = 0; k < (size_t)n; k++) memcpy(s + k * ul, unit, ul);
    s[ul * (size_t)n] = 0;
    memset(&d, 0, sizeof d);
    d.text = s; d.font = font; d.size = size; d.max_w = max_w;
    d.place = YGFX_TOP_LEFT; d.anchor = YGFX_TOP_LEFT; d.x = x; d.y = y;
    d.color[0] = d.color[1] = d.color[2] = 1;
    rc = ygfx_text_make(&gfx, t, &d);
    free(s);
    if (rc < 0) fprintf(stderr, "emoji_bench: text: %s\n", ygfx_error(&gfx));
    return rc;
}

#ifndef BENCH_OLD
static yol_ctx g_ol;

/* UTF-8 of a codepoint */
static int u8(uint32_t c, char* o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | (c >> 6 & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | (c >> 12 & 63)); o[2] = (char)(0x80 | (c >> 6 & 63)); o[3] = (char)(0x80 | (c & 63));
    return 4;
}

static int info(int argc, char** argv) {
    int i;
    yol_open(&g_ol, NULL);
    printf("| Font | Bytes | Add (flatten), ms | Color glyphs | Refused | Layers | Layer glyphs | Paint texels | Set build, ms | Set bytes |\n");
    printf("|---|---|---|---|---|---|---|---|---|---|\n");
    for (i = 0; i < argc; i++) {
        size_t n;
        unsigned char* d = read_file(argv[i], &n);
        ylay_lib* L;
        ylay_font_info fi;
        yol_cset set;
        int64_t t0, t1, t2;
        const char* base = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        if (!d) { fprintf(stderr, "emoji_bench: cannot read %s\n", argv[i]); continue; }
        ylay_create(&L, &(ylay_desc){ .ol = &g_ol });
        t0 = yrt_now_ns();
        if (ylay_add_font(L, d, n, 0, base) < 0) { printf("| %s | %zu | refused: %s |\n", base, n, ylay_error(L)); ylay_destroy(L); free(d); continue; }
        t1 = yrt_now_ns();
        ylay_font(L, 0, &fi);
        ylay_open_set(L, 0, &set, &g_ol);
        if (ylay_build_glyphs(L, 0, &set, NULL, 0, 0) < 0) fprintf(stderr, "emoji_bench: %s: %s\n", base, ylay_error(L));
        t2 = yrt_now_ns();
        printf("| %s | %zu | %.1f | %u | %u | %u | %u | %u | %.1f | %zu |\n", base, n, (double)(t1 - t0) * 1e-6, fi.n_color_glyphs,
               fi.n_refused, fi.n_layers, fi.n_layer_glyphs, fi.n_paint_texels, (double)(t2 - t1) * 1e-6,
               (size_t)set.n_texels * 16 + (size_t)set.n_words * 4);
        {   /* the refusals by feature: the message's text after the glyph */
            static char feat[32][96];
            static int cnt[32];
            int nf = 0, q;
            uint32_t g;
            for (g = 0; g < (uint32_t)fi.n_glyphs; g++) {
                const ylay_layer* ly;
                const char* m;
                char f[96];
                if (ylay_color_glyph(L, 0, g, &ly) != YLAY_ERR_REFUSED) continue;
                m = strstr(ylay_error(L), "): ");
                m = m ? m + 3 : ylay_error(L);
                snprintf(f, sizeof f, "%.*s", (int)(strcspn(m, ";")), m);
                for (q = 0; q < nf && strcmp(feat[q], f); q++) {}
                if (q == nf && nf < 32) { snprintf(feat[nf], sizeof feat[nf], "%s", f); cnt[nf++] = 0; }
                if (q < 32) cnt[q]++;
            }
            for (q = 0; q < nf; q++) printf("    refused %d: %s\n", cnt[q], feat[q]);
        }
        fflush(stdout);
        yol_cset_close(&set);
        ylay_destroy(L);
        free(d);
    }
    yol_close(&g_ol);
    return 0;
}

/* An emoji block: up to n emoji the font draws, cols a row, at size px. */
typedef struct emo { ylay_lib* L; ylay_block b; ygfx_cset set; ygfx_stim runs[8]; int n_runs; int count; uint32_t layers, cp; } emo;
static int emoji_scene(emo* e, const unsigned char* fd, size_t fn, int n, int cols, float size, uint32_t first_cp, int most) {
    static char text[1 << 16];
    ylay_font_info fi;
    ygfx_cset_desc cd;
    int k = 0, got = 0, r;
    uint32_t cp;
    const uint32_t* gl;
    memset(e, 0, sizeof *e);
    ylay_create(&e->L, &(ylay_desc){ .ol = &g_ol });
    if (ylay_add_font(e->L, fd, fn, 0, "emoji") < 0) { fprintf(stderr, "emoji_bench: %s\n", ylay_error(e->L)); return -1; }
    /* codepoints in order whose single layout draws color layers */
    uint32_t best = 0, best_n = 0;
    for (cp = first_cp ? first_cp : 0x1F300; cp < 0x1FB00 && got < n; cp++) {
        ylay_block b;
        char one[8];
        int m = u8(cp, one), ok = 0;
        uint32_t i, nl = 0;
        one[m] = 0;
        memset(&b, 0, sizeof b);
        if (ylay_layout(e->L, one, m, &(ylay_style){ .size = size }, NULL, 0, &b) == YLAY_OK && !b.n_missing)
            for (i = 0; i < b.n_runs; i++) if (b.runs[i].flags & YLAY_RUN_PAINT) { ok = 1; nl += b.runs[i].n; }
        ylay_block_free(&b);
        if (!ok) continue;
        if (most) {   /* the emoji with the most layers, in the whole range */
            if (nl > best_n) { best_n = nl; best = cp; }
            continue;
        }
        k += u8(cp, text + k);
        got++;
        if (got % cols == 0 && got < n) text[k++] = '\n';
    }
    if (most && best) { k = u8(best, text); got = 1; e->cp = best; }
    text[k] = 0;
    if (ylay_layout(e->L, text, k, &(ylay_style){ .size = size, .line_height = 1.0f }, NULL, 0, &e->b) < 0) {
        fprintf(stderr, "emoji_bench: %s\n", ylay_error(e->L));
        return -1;
    }
    ylay_take_new_glyphs(e->L, 0, &gl);
    ylay_font(e->L, 0, &fi);
    memset(&cd, 0, sizeof cd);
    cd.texels = fi.set->texels; cd.n_texels = fi.set->n_texels; cd.words = fi.set->words; cd.n_words = fi.set->n_words;
    e->set = ygfx_cset_make(&gfx, &cd);
    if (!e->set.id) { fprintf(stderr, "emoji_bench: set: %s\n", ygfx_error(&gfx)); return -1; }
    for (r = 0; r < (int)e->b.n_runs && e->n_runs < 8; r++) {
        ygfx_crun_desc d;
        memset(&d, 0, sizeof d);
        d.place = YGFX_TOP_LEFT; d.anchor = YGFX_TOP_LEFT; d.x = 40; d.y = 40;
        d.set = e->set; d.items = e->b.items + e->b.runs[r].first; d.n = (int)e->b.runs[r].n; d.size = e->b.runs[r].size;
        d.w = e->b.w; d.h = e->b.h;
        d.color[0] = d.color[1] = d.color[2] = 1;
        if (e->b.runs[r].flags & YLAY_RUN_PAINT) { d.fields = YGFX_I_PAINT; e->layers += e->b.runs[r].n; }
        e->runs[e->n_runs++] = ygfx_crun(&gfx, &d);
    }
    e->count = got;
    return 0;
}
#endif

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    const char* emoji = NULL, *mono = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", *only = NULL, *csv = NULL;
    int i, n = 600;
    if (argc > 1 && !strcmp(argv[1], "--info")) {
#ifndef BENCH_OLD
        return info(argc - 2, argv + 2);
#else
        return 2;
#endif
    }
    if (argc < 2 || strcmp(argv[1], "--frames")) { fprintf(stderr, "usage: emoji_bench --info FONT... | --frames [--emoji F] [--mono F] [--frames N] [--only NAMES] [--csv PATH]\n"); return 2; }
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--emoji") && i + 1 < argc) emoji = argv[++i];
        else if (!strcmp(argv[i], "--mono") && i + 1 < argc) mono = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
        else { fprintf(stderr, "emoji_bench: %s?\n", argv[i]); return 2; }
    }
    if (csv) g_csv = fopen(csv, "a");
    memset(&sd, 0, sizeof sd);
    sd.windowed = true; sd.window_w = 1920; sd.window_h = 1080;
    memset(&gd, 0, sizeof gd);
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    gd.max_draws = 4096;
    gd.max_instances = 1 << 16;
    if (!ygfx_open_screen(&gfx, &scr, &sd, &gd)) { fprintf(stderr, "emoji_bench: %s\n", ygfx_error(&gfx)); return 1; }
    glGenQueries_ = (void (*)(int, unsigned*))yscr_gl_proc(&scr, "glGenQueriesEXT");
    glBeginQuery_ = (void (*)(unsigned, unsigned))yscr_gl_proc(&scr, "glBeginQueryEXT");
    glEndQuery_ = (void (*)(unsigned))yscr_gl_proc(&scr, "glEndQueryEXT");
    glGetQueryObjectui64v_ = (void (*)(unsigned, unsigned, uint64_t*))yscr_gl_proc(&scr, "glGetQueryObjectui64vEXT");
    glGetIntegerv_ = (void (*)(unsigned, int*))yscr_gl_proc(&scr, "glGetIntegerv");
    if (!(glGenQueries_ && glBeginQuery_ && glEndQuery_ && glGetQueryObjectui64v_)) {
        glGenQueries_ = NULL; glBeginQuery_ = NULL; glEndQuery_ = NULL; glGetQueryObjectui64v_ = NULL;
        printf("no GL_EXT_disjoint_timer_query: GPU columns are 0\n");
    }
    printf("ysp/gfx.h %s, %d x %d, %s\n", YGFX_VERSION_STRING, gfx.w, gfx.h, yscr_gl_proc(&scr, "glGetString") ?
           (const char*)((const unsigned char* (*)(unsigned))yscr_gl_proc(&scr, "glGetString"))(0x1F01) : "?");
    printf("| Scene | Frames | Host p50, ms | Host p99 | GPU p50, ms | GPU p99 |\n|---|---|---|---|---|---|\n");
    if (want(only, "empty")) scene("empty (the output stage)", NULL, 0, n);
    if (want(only, "images")) {
        static uint8_t px[1920 * 1080 * 4];
        ygfx_texture_desc td;
        ygfx_tex tex;
        ygfx_stim im[12];
        int k;
        for (k = 0; k < 1920 * 1080 * 4; k++) px[k] = (uint8_t)(k * 2654435761u >> 24);
        memset(&td, 0, sizeof td);
        td.w = 1920; td.h = 1080; td.format = YGFX_RGBA8; td.data = px;
        tex = ygfx_texture(&gfx, &td);
        for (k = 0; k < 12; k++) {
            ygfx_image_desc id;
            memset(&id, 0, sizeof id);
            id.tex = tex; id.opacity = 0.5f;
            im[k] = ygfx_image(&gfx, &id);
        }
        ygfx_prime(&gfx);
        scene("images (12 x 1080p RGBA8)", im, 12, n);
    }
    if (want(only, "page") || want(only, "mono")) {
        static ygfx_text pg, m100, m600;
        if (want(only, "page") && mono_text(&pg, mono, "lorem ipsum dolor ", 112, 16, 1800, 40, 40) == 0) {
            ygfx_prime(&gfx);
            scene("page (2,016 glyphs, 16 px)", &pg.stim, 1, n);
            ygfx_text_free(&gfx, &pg);
        }
        if (want(only, "mono100") && mono_text(&m100, mono, "MMMMMMMMMM\n", 10, 48, 1800, 40, 40) == 0) {
            ygfx_prime(&gfx);
            scene("mono100 (48 px)", &m100.stim, 1, n);
            ygfx_text_free(&gfx, &m100);
        }
        if (want(only, "mono600") && mono_text(&m600, mono, "@", 1, 600, 1800, 40, 40) == 0) {
            ygfx_prime(&gfx);
            scene("mono600", &m600.stim, 1, n);
            ygfx_text_free(&gfx, &m600);
        }
    }
#ifndef BENCH_OLD
    if (emoji && want(only, "emoji")) {
        size_t fn;
        unsigned char* fd = read_file(emoji, &fn);
        emo e;
        char name[96];
        yol_open(&g_ol, NULL);
        if (!fd) { fprintf(stderr, "emoji_bench: cannot read %s\n", emoji); return 1; }
        if (want(only, "emoji100") && emoji_scene(&e, fd, fn, 100, 10, 48, 0, 0) == 0) {
            ygfx_prime(&gfx);
            snprintf(name, sizeof name, "emoji100 (%d emoji, %u layers)", e.count, e.layers);
            scene(name, e.runs, e.n_runs, n);
            ygfx_cset_free(&gfx, e.set);
            ylay_block_free(&e.b);
            ylay_destroy(e.L);
        }
        if (want(only, "emoji600") && emoji_scene(&e, fd, fn, 1, 1, 600, 0x1F600, 0) == 0) {
            ygfx_prime(&gfx);
            snprintf(name, sizeof name, "emoji600 (%u layers)", e.layers);
            scene(name, e.runs, e.n_runs, n);
            ygfx_cset_free(&gfx, e.set);
            ylay_block_free(&e.b);
            ylay_destroy(e.L);
        }
        if (want(only, "emoji600max") && emoji_scene(&e, fd, fn, 1, 1, 600, 0, 1) == 0) {
            ygfx_prime(&gfx);
            snprintf(name, sizeof name, "emoji600max (U+%04X, %u layers)", (unsigned)e.cp, e.layers);
            scene(name, e.runs, e.n_runs, n);
            ygfx_cset_free(&gfx, e.set);
            ylay_block_free(&e.b);
            ylay_destroy(e.L);
        }
        free(fd);
        yol_close(&g_ol);
    }
#else
    (void)emoji;
#endif
    ygfx_close(&gfx);
    yscr_close(&scr);
    if (g_csv) fclose(g_csv);
    return 0;
}
