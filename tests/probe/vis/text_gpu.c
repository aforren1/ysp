/* text_gpu.c - the ysp-vis text loop's scene drawn from C, with the GPU's
 * time (docs/gfx.md, "Texts in a frame: the cause").
 *
 * Usage: text_gpu [--mode runs|one|empty] [--n N] [--seconds S] [--csv PATH]
 *                 [--gpu] [--finish] [--windowed]
 * The scene of tests/probe/vis/text_loop.py's words mode: N one-word texts
 * (Segoe UI, 28 px, 20 a row 40 px apart), each a curve run with its items
 * copied at each draw, as the bindings draw them (runs); the same glyphs
 * as one run (one); nothing (empty). Each frame: yscr_begin, ygfx_begin,
 * the draws, ygfx_end, yscr_flip.
 *   --gpu     a GL_EXT_disjoint_timer_query TIME_ELAPSED query around
 *             ygfx_begin .. ygfx_end, read 4 frames later: a diagnostic, not
 *             a mode to keep (a query cost about 250 us of CPU a frame
 *             through ANGLE, docs/screen.md)
 *   --finish  glFinish() after ygfx_end, timed: the GPU work left when the
 *             CPU has submitted the frame (it removes the overlap of the
 *             GPU with the next frame's CPU, so it is a diagnostic too)
 * One CSV row per flip record with the frame's own times: draws_ns
 * (ygfx_begin and the draws), end_ns (ygfx_end), finish_ns, gpu_ns (-1:
 * not measured). Windows, ANGLE beside the program or in YSCR_ANGLE_DIR.
 * Build: see README.md in this folder.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define GLAPI_ __stdcall
#else
#define GLAPI_
#endif

static yscr_screen scr;
static ygfx_gfx gfx;
static yol_ctx cx;

static const char* const WORDS[9] = { "reading", "fixation", "saccade", "parafovea", "lexical", "syntax", "meaning",
                                      "context", "stimulus" };
#define MAXW 2000
#define MAXF 8000
static ygfx_citem items[MAXW][12], all[MAXW * 12];
static ygfx_stim runs[MAXW], one;

typedef struct fr { int64_t draws, end, finish, gpu; } fr;
static fr frs[MAXF];
static yscr_record recs[MAXF + 8];
static int n_recs;

static void (GLAPI_ *glFinish_)(void);
static void (GLAPI_ *glGenQueries_)(int, unsigned*);
static void (GLAPI_ *glBeginQuery_)(unsigned, unsigned);
static void (GLAPI_ *glEndQuery_)(unsigned);
static void (GLAPI_ *glGetQueryObjectuiv_)(unsigned, unsigned, unsigned*);
static void (GLAPI_ *glGetQueryObjectui64v_)(unsigned, unsigned, uint64_t*);
static void (GLAPI_ *glGetIntegerv_)(unsigned, int*);
#define GL_TIME_ELAPSED_EXT 0x88BF
#define GL_QUERY_RESULT_EXT 0x8866
#define GL_QUERY_RESULT_AVAILABLE_EXT 0x8867
#define GL_GPU_DISJOINT_EXT 0x8FBB

static unsigned char* read_file(const char* path, size_t* n) {
    FILE* fp = fopen(path, "rb");
    unsigned char* d;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    *n = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    d = (unsigned char*)malloc(*n);
    if (d && fread(d, 1, *n, fp) != *n) { free(d); d = NULL; }
    fclose(fp);
    return d;
}

static void keep(const yscr_frame* f) {
    int i;
    for (i = 0; i < f->n_done && n_recs < MAXF + 8; i++) recs[n_recs++] = f->done[i];
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    yscr_frame f;
    yol_font font;
    yol_cset cs;
    yol_cset_desc cd;
    ygfx_cset_desc gcd;
    ygfx_cset set;
    static uint32_t list[128];
    unsigned q[4] = { 0 };
    unsigned char* fd;
    size_t fn;
    const char *mode = "runs", *csv = "text_gpu.csv";
    int i, k, n = 500, gpu = 0, finish = 0, windowed = 0, n_all = 0, m = 0, W, H;
    double seconds = 60;
    int64_t t_end = 0;
    char line[1024], err[200];
    FILE* out;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
        else if (!strcmp(argv[i], "--gpu")) gpu = 1;
        else if (!strcmp(argv[i], "--finish")) finish = 1;
        else if (!strcmp(argv[i], "--windowed")) windowed = 1;
        else { fprintf(stderr, "usage: text_gpu [--mode runs|one|empty] [--n N] [--seconds S] [--csv PATH] [--gpu] [--finish] [--windowed]\n"); return 2; }
    }
    if (n < 1 || n > MAXW || seconds <= 0 || seconds * 61 > MAXF) { fprintf(stderr, "text_gpu: n 1..%d, seconds up to %d\n", MAXW, MAXF / 61); return 2; }
    memset(&sd, 0, sizeof sd);
    sd.windowed = windowed != 0;
    if (windowed) { sd.window_w = 1280; sd.window_h = 720; }
    memset(&gd, 0, sizeof gd);
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    gd.max_draws = 4096;   /* 3 blocks a run before gfx.h 0.17.2 */
    if (!ygfx_open_screen(&gfx, &scr, &sd, &gd)) { fprintf(stderr, "text_gpu: %s\n", ygfx_error(&gfx)); return 1; }
    W = gfx.w; H = gfx.h;
    glFinish_ = (void (GLAPI_*)(void))yscr_gl_proc(&scr, "glFinish");
    glGenQueries_ = (void (GLAPI_*)(int, unsigned*))yscr_gl_proc(&scr, "glGenQueriesEXT");
    glBeginQuery_ = (void (GLAPI_*)(unsigned, unsigned))yscr_gl_proc(&scr, "glBeginQueryEXT");
    glEndQuery_ = (void (GLAPI_*)(unsigned))yscr_gl_proc(&scr, "glEndQueryEXT");
    glGetQueryObjectuiv_ = (void (GLAPI_*)(unsigned, unsigned, unsigned*))yscr_gl_proc(&scr, "glGetQueryObjectuivEXT");
    glGetQueryObjectui64v_ = (void (GLAPI_*)(unsigned, unsigned, uint64_t*))yscr_gl_proc(&scr, "glGetQueryObjectui64vEXT");
    glGetIntegerv_ = (void (GLAPI_*)(unsigned, int*))yscr_gl_proc(&scr, "glGetIntegerv");
    if (gpu && !(glGenQueries_ && glBeginQuery_ && glEndQuery_ && glGetQueryObjectui64v_)) {
        fprintf(stderr, "text_gpu: no GL_EXT_disjoint_timer_query\n");
        return 1;
    }
    if (gpu) { yscr_bind(&scr); glGenQueries_(4, q); }

    fd = read_file("C:/Windows/Fonts/segoeui.ttf", &fn);
    if (!fd || yol_font_view(&font, fd, fn, 0, err, sizeof err) < 0) { fprintf(stderr, "text_gpu: Segoe UI: %s\n", err); return 1; }
    for (i = 33; i < 127; i++) { uint32_t g = yol_font_glyph_index(&font, (uint32_t)i); if (g) list[m++] = g; }
    memset(&cd, 0, sizeof cd);
    cd.n_glyphs = (uint32_t)font.n_glyphs; cd.backward = true;
    if (!yol_open(&cx, NULL) || !yol_cset_open(&cs, &cx, &cd) || yol_cset_add_font(&cs, &font, list, (uint32_t)m, 0) < 0) {
        fprintf(stderr, "text_gpu: the set\n");
        return 1;
    }
    memset(&gcd, 0, sizeof gcd);
    gcd.texels = cs.texels; gcd.n_texels = cs.n_texels; gcd.words = cs.words; gcd.n_words = cs.n_words;
    set = ygfx_cset_make(&gfx, &gcd);
    if (!set.id) { fprintf(stderr, "text_gpu: %s\n", ygfx_error(&gfx)); return 1; }
    for (i = 0; i < n; i++) {
        const char* s = WORDS[i % 9];
        const int cols = n <= 100 ? 10 : 20;
        double x = 0, cxp = (i % cols) * ((double)W / cols) + (double)W / cols / 2, cyp = (i / cols) * 40.0 + 40.0;
        ygfx_crun_desc rd;
        for (k = 0; s[k]; k++) {
            double a, l;
            uint32_t g = yol_font_glyph_index(&font, (uint32_t)s[k]);
            memset(&items[i][k], 0, sizeof items[i][k]);
            items[i][k].x = (float)x; items[i][k].glyph = (float)g;
            all[n_all] = items[i][k];
            all[n_all].x = (float)(cxp - 50.0 + x); all[n_all].y = (float)(cyp + 10.0);
            n_all++;
            x += (yol_font_hmetrics(&font, g, &a, &l) >= 0 ? a : 0.5) * 28.0;
        }
        memset(&rd, 0, sizeof rd);
        rd.set = set; rd.items = items[i]; rd.n = k; rd.size = 28;
        rd.x = (float)(cxp - W / 2.0); rd.y = (float)(cyp - H / 2.0);
        rd.color[0] = rd.color[1] = rd.color[2] = 0.1f;
        runs[i] = ygfx_crun(&gfx, &rd);
    }
    {
        ygfx_crun_desc rd;
        memset(&rd, 0, sizeof rd);
        rd.place = YGFX_TOP_LEFT; rd.anchor = YGFX_TOP_LEFT; rd.set = set; rd.items = all; rd.n = n_all;
        rd.size = 28; rd.w = (float)W; rd.h = (float)H;
        rd.color[0] = rd.color[1] = rd.color[2] = 0.1f;
        one = ygfx_crun(&gfx, &rd);
    }
    ygfx_prime(&gfx);
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    ygfx_describe(&gfx, line, sizeof line);
    printf("%s\nmode %s n %d glyphs %d gpu %d finish %d\n", line, mode, n, n_all, gpu, finish);
    fflush(stdout);

    while (yscr_begin(&scr, &f) == YSCR_OK) {
        int64_t a, b, c, d;
        int fi = (int)f.index;
        keep(&f);
        if (!t_end) t_end = f.onset + (int64_t)(seconds * 1e9);
        if (f.onset >= t_end || fi >= MAXF) { yscr_flip(&scr); break; }
        if (gpu && fi >= 4) {   /* the query of 4 frames ago */
            unsigned qi = q[fi & 3], avail = 0;
            int disj = 0;
            uint64_t ns = 0;
            glGetQueryObjectuiv_(qi, GL_QUERY_RESULT_AVAILABLE_EXT, &avail);
            if (glGetIntegerv_) glGetIntegerv_(GL_GPU_DISJOINT_EXT, &disj);
            if (avail) glGetQueryObjectui64v_(qi, GL_QUERY_RESULT_EXT, &ns);
            frs[fi - 4].gpu = avail && !disj ? (int64_t)ns : -1;
        }
        a = yrt_now_ns();
        if (gpu) glBeginQuery_(GL_TIME_ELAPSED_EXT, q[fi & 3]);
        ygfx_begin(&gfx, &f);
        if (!strcmp(mode, "runs")) {
            int rc = ygfx_draw_n(&gfx, runs, n);
            if (rc < 0) { fprintf(stderr, "text_gpu: draw: %s\n", ygfx_strerror(rc)); break; }
        } else if (!strcmp(mode, "one")) {
            ygfx_draw(&gfx, &one);
        }
        b = yrt_now_ns();
        ygfx_end(&gfx);
        if (gpu) glEndQuery_(GL_TIME_ELAPSED_EXT);
        c = yrt_now_ns();
        if (finish) glFinish_();
        d = yrt_now_ns();
        frs[fi].draws = b - a; frs[fi].end = c - b; frs[fi].finish = finish ? d - c : -1;
        if (!gpu) frs[fi].gpu = -1;
        yscr_flip(&scr);
    }
    {
        yscr_record r;
        if (yscr_wait_flip(&scr, &r) == YSCR_OK && (!n_recs || r.index > recs[n_recs - 1].index) && n_recs < MAXF + 8)
            recs[n_recs++] = r;
    }
    out = fopen(csv, "w");
    if (!out) { fprintf(stderr, "text_gpu: %s\n", csv); return 1; }
    fprintf(out, "# c mode %s n %d glyphs %d gpu %d finish %d\n", mode, n, n_all, gpu, finish);
    fprintf(out, "index,target_ns,planned_ns,onset_ns,residual_ns,dropped,path,tier,flags,draw_ns,swap_ns,draws_ns,end_ns,finish_ns,gpu_ns\n");
    for (i = 0; i < n_recs; i++) {
        const yscr_record* r = &recs[i];
        const fr* x = r->index >= 0 && r->index < MAXF ? &frs[r->index] : NULL;
        fprintf(out, "%lld,%lld,%lld,%lld,%lld,%u,%u,%u,%u,%lld,%lld,%lld,%lld,%lld,%lld\n", (long long)r->index,
                (long long)r->target, (long long)r->planned, (long long)r->onset, (long long)r->residual, r->dropped, r->path,
                r->tier, r->flags,
                r->phase_ns[YSCR_PHASE_DRAW] == YSCR_PHASE_UNKNOWN ? -1LL : (long long)r->phase_ns[YSCR_PHASE_DRAW],
                r->phase_ns[YSCR_PHASE_SWAP] == YSCR_PHASE_UNKNOWN ? -1LL : (long long)r->phase_ns[YSCR_PHASE_SWAP],
                x ? (long long)x->draws : -1LL, x ? (long long)x->end : -1LL, x ? (long long)x->finish : -1LL,
                x ? (long long)x->gpu : -1LL);
    }
    fclose(out);
    printf("%d records -> %s\n", n_recs, csv);
    ygfx_cset_free(&gfx, set);
    ygfx_close(&gfx);
    yscr_close(&scr);
    yol_cset_close(&cs);
    yol_close(&cx);
    free(fd);
    return 0;
}
