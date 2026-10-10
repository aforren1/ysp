/* gfx_bench.c - what ysp/gfx.h costs per frame, offscreen.
 *
 * Opens a headless GL ES 3.0 context (tests/adapt/gfx_headless.h) at the
 * panel's size and times each workload of docs/gfx.md's measurement plan:
 *   cpu   ygfx_begin() to ygfx_end() on the frame thread, per frame,
 *         mean and p99 over the frames
 *   gpu   the workload's frames back to back between two glFinish() calls,
 *         per frame, minus nothing: GPU-bound throughput, the per-frame cost
 *         a frame loop must leave room for. No timer query, which cost about
 *         250 us of CPU per frame through ANGLE (docs/screen.md).
 * Nothing is shown. Take the measurement lock of your machine first: the
 * numbers mean nothing while another program loads the GPU.
 *
 * Usage: gfx_bench [--device hardware|warp|swiftshader|mesa|llvmpipe]
 *                  [--size W H] [--frames N] [--only NAME] [--scene 16|32]
 *                  [--cache DIR] [--no-cache] [--open-only]
 *   --scene  the scene format of the workloads after the empty frames
 *   --cache  a program cache in DIR (ygfx_file_cache_init); :mem: one in memory;
 *            the default is the per-user folder of ygfx_default_cache_dir()
 *   --no-cache  compile every program; read and write no cache file
 *   --open-only  print ygfx_open()'s time and what the cache did, then exit
 * Exit code: 0, 1 when no GL ES 3.0 context opened, 2 for a bad argument.
 * On Windows set YSCR_ANGLE_DIR to ANGLE's directory.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#include "../../tests/adapt/gfx_headless.h"
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #define GLCALL __stdcall
#else
    #define GLCALL
#endif

static ygfx_headless hl;
static yscr_screen scr;
static ygfx_gfx gfx;
static int W = 1920, H = 1200, FRAMES = 120;
static const char* only = NULL;
static ygfx_format scene_fmt = YGFX_FORMAT_NONE;   /* the workloads' scene; 0 = the default */
static ygfx_file_cache pcache;          /* --cache DIR or the default */
static char cache_dir[512];
static const ygfx_cache* cache_desc = NULL;
static int open_only = 0;
static void (GLCALL *glFinish_)(void);

#define MAXF 2000
static double cpu_us[MAXF];

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y);
}

static double p99(double* v, int n) {
    qsort(v, (size_t)n, sizeof v[0], cmp_d);
    return v[(int)(0.99 * (n - 1))];
}

static int run(const char* name) { return !only || strstr(name, only) != NULL; }

static void report(const char* name, int n, double wall_ns, int frames) {
    double mean = 0;
    int i;
    for (i = 0; i < n; i++) mean += cpu_us[i];
    mean /= n;
    printf("| %-52s | %9.1f | %9.1f | %9.3f |\n", name, mean, p99(cpu_us, n), wall_ns / frames * 1e-6);
    fflush(stdout);
}

/* Draws `s` (n of them) for FRAMES frames; prep() runs before each frame,
 * outside begin..end, for uploads. */
static void bench(const char* name, const ygfx_stim* s, int n, void (*prep)(int), int timed_prep) {
    yscr_frame f;
    uint64_t t0, a, b;
    int i;
    if (!run(name)) return;
    memset(&f, 0, sizeof f);
    f.period = 16666667;
    for (i = 0; i < 5; i++) {   /* warm: lazy shader variants, first uploads */
        if (prep) prep(i);
        ygfx_begin(&gfx, &f);
        ygfx_draw_n(&gfx, s, n);
        ygfx_end(&gfx);
    }
    glFinish_();
    t0 = yrt_now_ns();
    for (i = 0; i < FRAMES; i++) {
        f.index = i;
        f.onset = yrt_now_ns();
        a = yrt_now_ns();
        if (prep) prep(i);
        if (!timed_prep) a = yrt_now_ns();
        ygfx_begin(&gfx, &f);
        ygfx_draw_n(&gfx, s, n);
        ygfx_end(&gfx);
        b = yrt_now_ns();
        if (i < MAXF) cpu_us[i] = (double)(b - a) * 1e-3;
    }
    glFinish_();
    report(name, FRAMES < MAXF ? FRAMES : MAXF, (double)(yrt_now_ns() - t0), FRAMES);
}

/* --cache :mem: keeps the programs in memory: the cache's cost without the
 * file system's */
static struct { uint64_t key[64]; void* p[64]; size_t n[64]; int count; } memc;
static size_t memc_load(void* u, uint64_t key, void* dst, size_t cap) {
    int i;
    (void)u;
    for (i = 0; i < memc.count; i++)
        if (memc.key[i] == key) { if (dst && cap >= memc.n[i]) memcpy(dst, memc.p[i], memc.n[i]); return memc.n[i]; }
    return 0;
}
static int memc_store(void* u, uint64_t key, const void* data, size_t n) {
    (void)u;
    if (memc.count == 64 || !(memc.p[memc.count] = malloc(n))) return -1;
    memcpy(memc.p[memc.count], data, n);
    memc.key[memc.count] = key; memc.n[memc.count++] = n;
    return 0;
}
static const ygfx_cache memc_cache = { memc_load, memc_store, NULL };

static bool reopen(ygfx_format fmt, ygfx_dither dither, int lut);
static bool reopen(ygfx_format fmt, ygfx_dither dither, int lut) {
    ygfx_desc gd;
    ygfx_close(&gfx);
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    ygfx__test_scene32 = fmt == YGFX_RGBA32F;   /* a private seam: measured, then removed from the API */
    gd.dither = dither;
    gd.max_draws = 1100;
    gd.cache = cache_desc;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_bench: %s\n", ygfx_error(&gfx)); return false; }
    if (lut) {
        static float t[3 * 4096];
        int k;
        for (k = 0; k < 3 * 4096; k++) t[k] = (float)pow((double)(k % 4096) / 4095.0, 1.0 / 2.2);
        ygfx_set_lut(&gfx, t, 4096);
    }
    return true;
}

/* --- uploads ------------------------------------------------------------------ */

static ygfx_buf dot_buf;
static float* dot_xy;
static int dot_n;
static void prep_dots(int frame) {
    int i;
    for (i = 0; i < dot_n; i++) {
        dot_xy[2 * i] = (float)((i * 7919 + frame * 13) % 1800) - 900.0f;
        dot_xy[2 * i + 1] = (float)((i * 104729 + frame * 7) % 1100) - 550.0f;
    }
    ygfx_buffer_update(&gfx, dot_buf, 0, dot_xy, (size_t)dot_n * 8);
}

static ygfx_tex up_tex;
static void* up_data;
static void prep_upload(int frame) {
    (void)frame;
    ygfx_texture_update(&gfx, up_tex, 0, 0, W, H, up_data, 0);
}

/* --- raw GL: the variants ysp/gfx.h does not carry ------------------------------ */

typedef unsigned int GLu;
static void (GLCALL *glGenBuffers_)(int, GLu*);
static void (GLCALL *glBindBuffer_)(GLu, GLu);
static void (GLCALL *glBufferData_)(GLu, ptrdiff_t, const void*, GLu);
static void (GLCALL *glBufferSubData_)(GLu, ptrdiff_t, ptrdiff_t, const void*);
static void (GLCALL *glBindBufferRange_)(GLu, GLu, GLu, ptrdiff_t, ptrdiff_t);
static void (GLCALL *glBindTexture_)(GLu, GLu);
static void (GLCALL *glActiveTexture_)(GLu);
static void (GLCALL *glTexSubImage2D_)(GLu, int, int, int, int, int, GLu, GLu, const void*);
static void (GLCALL *glPixelStorei_)(GLu, int);
static void (GLCALL *glUniform4fv_)(int, int, const float*);
static int  (GLCALL *glGetUniformLocation_)(GLu, const char*);
static GLu  (GLCALL *glCreateShader_)(GLu);
static void (GLCALL *glShaderSource_)(GLu, int, const char* const*, const int*);
static void (GLCALL *glCompileShader_)(GLu);
static GLu  (GLCALL *glCreateProgram_)(void);
static void (GLCALL *glAttachShader_)(GLu, GLu);
static void (GLCALL *glLinkProgram_)(GLu);
static void (GLCALL *glUseProgram_)(GLu);
static void (GLCALL *glDrawArrays_)(GLu, int, int);
static void (GLCALL *glGenVertexArrays_)(int, GLu*);
static void (GLCALL *glBindVertexArray_)(GLu);
static void (GLCALL *glBindFramebuffer_)(GLu, GLu);
static void (GLCALL *glViewport_)(int, int, int, int);
static GLu  (GLCALL *glGetUniformBlockIndex_)(GLu, const char*);
static void (GLCALL *glUniformBlockBinding_)(GLu, GLu, GLu);
static void (GLCALL *glEnable_)(GLu);
static void (GLCALL *glDisable_)(GLu);
static void (GLCALL *glBlendFunc_)(GLu, GLu);

/* ISO C converts function pointers by cast; a copy keeps one macro for all. */
static void load_one(void* dst, const char* name) {
    yscr_proc p = yscr_gl_proc(&scr, name);
    memcpy(dst, &p, sizeof p);
}
#define LOAD(n) load_one(&n##_, #n)

static void load_raw(void) {
    LOAD(glGenBuffers); LOAD(glBindBuffer); LOAD(glBufferData); LOAD(glBufferSubData); LOAD(glBindBufferRange);
    LOAD(glBindTexture); LOAD(glActiveTexture); LOAD(glTexSubImage2D); LOAD(glPixelStorei); LOAD(glUniform4fv);
    LOAD(glGetUniformLocation); LOAD(glCreateShader); LOAD(glShaderSource); LOAD(glCompileShader);
    LOAD(glCreateProgram); LOAD(glAttachShader); LOAD(glLinkProgram); LOAD(glUseProgram); LOAD(glDrawArrays);
    LOAD(glGenVertexArrays); LOAD(glBindVertexArray); LOAD(glBindFramebuffer); LOAD(glViewport);
    LOAD(glGetUniformBlockIndex); LOAD(glUniformBlockBinding); LOAD(glEnable); LOAD(glDisable); LOAD(glBlendFunc);
}

static GLu raw_program(const char* fs_decl) {
    static char vs[1024], fs[1024];
    const char* pv = vs;
    const char* pf = fs;
    GLu v, f, p;
    snprintf(vs, sizeof vs,
             "#version 300 es\nprecision highp float;\n%s\nflat out vec4 c;\n"
             "void main(){ vec4 q = U(0); vec2 k = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;"
             " c = U(1); gl_Position = vec4(q.xy + k * q.zw, 0.0, 1.0); }\n", fs_decl);
    snprintf(fs, sizeof fs, "#version 300 es\nprecision highp float;\nflat in vec4 c; out vec4 o;\nvoid main(){ o = c; }\n");
    v = glCreateShader_(0x8B31); glShaderSource_(v, 1, &pv, NULL); glCompileShader_(v);
    f = glCreateShader_(0x8B30); glShaderSource_(f, 1, &pf, NULL); glCompileShader_(f);
    p = glCreateProgram_(); glAttachShader_(p, v); glAttachShader_(p, f); glLinkProgram_(p);
    return p;
}

/* N draws of a 256 x 256 quad, 16 vec4 of uniforms each: glUniform4fv per
 * draw, against one upload per frame and a bound range per draw. */
static void raw_uniforms(int n) {
    static float u[1024 * 64];
    GLu pu = raw_program("uniform vec4 u[16];\n#define U(i) u[i]");
    GLu pb = raw_program("layout(std140) uniform B { vec4 u[16]; };\n#define U(i) u[i]");
    GLu vao, ubo;
    int loc, i, fr, variant;
    char name[80];
    for (i = 0; i < n * 64; i += 64) {
        u[i] = -0.5f; u[i + 1] = 0.0f; u[i + 2] = 256.0f / (float)W; u[i + 3] = 256.0f / (float)H;
        u[i + 4] = u[i + 5] = u[i + 6] = 0.5f; u[i + 7] = 1.0f;
    }
    glGenVertexArrays_(1, &vao);
    glBindVertexArray_(vao);
    glGenBuffers_(1, &ubo);
    glBindBuffer_(0x8A11, ubo);
    glBufferData_(0x8A11, (ptrdiff_t)sizeof u, NULL, 0x88E8);
    glUniformBlockBinding_(pb, glGetUniformBlockIndex_(pb, "B"), 0);
    loc = glGetUniformLocation_(pu, "u");
    glBindFramebuffer_(0x8D40, 0);
    glViewport_(0, 0, W, H);
    glDisable_(0x0BE2);
    for (variant = 0; variant < 2; variant++) {
        uint64_t t0, a;
        glUseProgram_(variant ? pb : pu);
        glFinish_();
        t0 = yrt_now_ns();
        for (fr = 0; fr < FRAMES; fr++) {
            a = yrt_now_ns();
            if (variant) glBufferSubData_(0x8A11, 0, (ptrdiff_t)(n * 256), u);
            for (i = 0; i < n; i++) {
                if (variant) glBindBufferRange_(0x8A11, 0, ubo, (ptrdiff_t)i * 256, 256);
                else glUniform4fv_(loc, 16, u + i * 64);
                glDrawArrays_(0x0005, 0, 4);
            }
            if (fr < MAXF) cpu_us[fr] = (double)(yrt_now_ns() - a) * 1e-3;
        }
        glFinish_();
        snprintf(name, sizeof name, "raw GL, %d draws: %s", n, variant ? "UBO once + range per draw" : "glUniform4fv per draw");
        report(name, FRAMES < MAXF ? FRAMES : MAXF, (double)(yrt_now_ns() - t0), FRAMES);
    }
    ygfx_reset_state(&gfx);
}

/* Full-frame RGBA8 upload: glTexSubImage2D from memory against a pixel
 * unpack buffer, both on ysp/gfx.h's texture. */
static void raw_pbo(void) {
    GLu pbo;
    int fr, variant;
    for (variant = 0; variant < 2; variant++) {
        uint64_t t0, a;
        if (variant) {
            glGenBuffers_(1, &pbo);
            glBindBuffer_(0x88EC, pbo);
            glBufferData_(0x88EC, (ptrdiff_t)W * H * 4, NULL, 0x88E0);   /* STREAM_DRAW */
        }
        glActiveTexture_(0x84C0);
        glBindTexture_(0x0DE1, ((ygfx__gl*)gfx.bctx)->tex[gfx.tex[up_tex.id - 1].bid - 1].tex);
        glPixelStorei_(0x0CF5, 1);
        glFinish_();
        t0 = yrt_now_ns();
        for (fr = 0; fr < FRAMES; fr++) {
            a = yrt_now_ns();
            if (variant) {
                glBufferData_(0x88EC, (ptrdiff_t)W * H * 4, NULL, 0x88E0);
                glBufferSubData_(0x88EC, 0, (ptrdiff_t)W * H * 4, up_data);
                glTexSubImage2D_(0x0DE1, 0, 0, 0, W, H, 0x1908, 0x1401, (const void*)0);
            } else {
                glTexSubImage2D_(0x0DE1, 0, 0, 0, W, H, 0x1908, 0x1401, up_data);
            }
            if (fr < MAXF) cpu_us[fr] = (double)(yrt_now_ns() - a) * 1e-3;
        }
        glFinish_();
        report(variant ? "upload RGBA8, full frame: PBO (raw GL)" : "upload RGBA8, full frame: glTexSubImage2D (raw GL)",
               FRAMES < MAXF ? FRAMES : MAXF, (double)(yrt_now_ns() - t0), FRAMES);
        if (variant) glBindBuffer_(0x88EC, 0);
    }
    ygfx_reset_state(&gfx);
}


/* --- v0.2 ------------------------------------------------------------------ */

/* Interleaved comparison. The GPU's clock ramps over seconds, so rows run
 * one after another compare clock states, not workloads (measured: an empty
 * frame first in the run took 1.48 ms, the same frame with an empty target
 * pass later 0.41 ms). Here the workloads take turns in blocks of CMP_BLOCK
 * frames, each block between two glFinish calls; each workload's time is
 * the median over CMP_REPS rounds, and its cost is the median of its
 * difference from workload 0 (an empty frame) in the same round. */
typedef struct {
    const char* name;
    const ygfx_stim* s;          /* drawn into the scene                    */
    int n;
    ygfx_tex t;                  /* a target pass first when pass != 0      */
    const ygfx_stim* in;         /* drawn into that pass                    */
    int n_in;
    int pass;                      /* 2: in is drawn into blur's layer, then blurred */
    void (*prep)(int);
    ygfx_blur* blur;
} bench_work;

enum { CMP_MAX = 10, CMP_REPS = 25, CMP_BLOCK = 12 };
static double cmp_gpu[CMP_MAX][CMP_REPS], cmp_cpu[CMP_MAX][CMP_REPS], cmp_tmp[CMP_REPS];

static double median_of(const double* v, int n) {
    int i;
    for (i = 0; i < n; i++) cmp_tmp[i] = v[i];
    qsort(cmp_tmp, (size_t)n, sizeof(double), cmp_d);
    return n % 2 ? cmp_tmp[n / 2] : 0.5 * (cmp_tmp[n / 2 - 1] + cmp_tmp[n / 2]);
}

static void cmp_frame(const bench_work* w, yscr_frame* f, int i) {
    static const float zero[4] = { 0, 0, 0, 0 };
    if (w->prep) w->prep(i);   /* before begin: an upload must be */
    ygfx_begin(&gfx, f);
    if (w->pass == 2) {
        ygfx_begin_target(&gfx, w->blur->layer, zero);
        if (w->n_in) ygfx_draw_n(&gfx, w->in, w->n_in);
        ygfx_end_target(&gfx);
        ygfx_blur_apply(&gfx, w->blur);
        ygfx_draw(&gfx, &w->blur->image);
    } else if (w->pass) {
        ygfx_begin_target(&gfx, w->t, zero);
        if (w->n_in) ygfx_draw_n(&gfx, w->in, w->n_in);
        ygfx_end_target(&gfx);
    }
    if (w->n) ygfx_draw_n(&gfx, w->s, w->n);
    ygfx_end(&gfx);
}

static void bench_cmp(const char* title, const bench_work* w, int nw) {
    yscr_frame f;
    double d[CMP_REPS];
    uint64_t t0, a;
    int r, k, i, frame = 0;
    if (!run(title) || nw > CMP_MAX) return;
    memset(&f, 0, sizeof f);
    f.period = 16666667;
    for (k = 0; k < nw; k++)   /* warm: lazy variants, the clock up */
        for (i = 0; i < 30; i++) cmp_frame(&w[k], &f, frame++);
    for (r = 0; r < CMP_REPS; r++) {
        for (k = 0; k < nw; k++) {
            double cpu = 0;
            glFinish_();
            t0 = yrt_now_ns();
            for (i = 0; i < CMP_BLOCK; i++) {
                f.index = frame;
                a = yrt_now_ns();
                cmp_frame(&w[k], &f, frame++);
                cpu += (double)(yrt_now_ns() - a) * 1e-3;
            }
            glFinish_();
            cmp_gpu[k][r] = (double)(yrt_now_ns() - t0) * 1e-6 / CMP_BLOCK;
            cmp_cpu[k][r] = cpu / CMP_BLOCK;
        }
    }
    printf("\n%s: %d rounds of %d frames each, interleaved\n\n", title, CMP_REPS, CMP_BLOCK);
    printf("| %-48s | %8s | %8s | %8s | %8s |\n", "workload", "ms", "ms - [0]", "cpu us", "us - [0]");
    printf("|--------------------------------------------------|----------|----------|----------|----------|\n");
    for (k = 0; k < nw; k++) {
        double gm = median_of(cmp_gpu[k], CMP_REPS), cm = median_of(cmp_cpu[k], CMP_REPS), gd, cd;
        for (r = 0; r < CMP_REPS; r++) d[r] = cmp_gpu[k][r] - cmp_gpu[0][r];
        gd = median_of(d, CMP_REPS);
        for (r = 0; r < CMP_REPS; r++) d[r] = cmp_cpu[k][r] - cmp_cpu[0][r];
        cd = median_of(d, CMP_REPS);
        printf("| %-48s | %8.3f | %8.3f | %8.1f | %8.1f |\n", w[k].name, gm, gd, cm, cd);
    }
    fflush(stdout);
}

/* --- v0.4: video ---------------------------------------------------------------- */

static ygfx_tex vid_t[3];
static ygfx_planes vid_p[3];
static void prep_vid_rgba(int frame) { (void)frame; ygfx_texture_update(&gfx, vid_t[0], 0, 0, 1920, 1080, vid_p[0].data[0], 0); }
static void prep_vid_nv12(int frame) { (void)frame; ygfx_texture_update_planes(&gfx, vid_t[1], &vid_p[1]); }

/* The CPU time of one update call, mean and p99 over n calls. */
static void vid_update_cpu(const char* name, int k, int n) {
    int i;
    for (i = 0; i < n && i < MAXF; i++) {
        uint64_t a = yrt_now_ns();
        if (k == 0) ygfx_texture_update(&gfx, vid_t[0], 0, 0, 1920, 1080, vid_p[0].data[0], 0);
        else ygfx_texture_update_planes(&gfx, vid_t[k], &vid_p[k]);
        cpu_us[i] = (double)(yrt_now_ns() - a) * 1e-3;
        if (i % 4 == 3) glFinish_();   /* a frame loop's pace, not a queue of uploads */
    }
    glFinish_();
    {
        double mean = 0;
        int m = n < MAXF ? n : MAXF;
        for (i = 0; i < m; i++) mean += cpu_us[i];
        printf("| %-48s | %8.1f | %8.1f |\n", name, mean / m, p99(cpu_us, m));
    }
}

static void bench_v04_video(void) {
    static ygfx_stim st[8];
    bench_work w[CMP_MAX];
    ygfx_texture_desc td;
    ygfx_image_desc idd;
    unsigned char* mem;
    size_t ny = 1920 * 1080;
    int k, i;
    static const char* const tname[] = { "LINEAR", "BT1886", "DEVICE" };
    if (!run("v0.4 video")) return;
    if (W < 1920 || H < 1080) { printf("v0.4 video: needs a 1920 x 1080 scene or larger\n"); return; }
    if (!reopen(scene_fmt, YGFX_DITHER_NONE, 0)) return;
    mem = (unsigned char*)malloc(ny * 4 + ny * 3);
    if (!mem) return;
    for (i = 0; i < (int)(ny * 7); i++) mem[i] = (unsigned char)(16 + (i * 37) % 220);
    memset(vid_p, 0, sizeof vid_p);
    vid_p[0].data[0] = mem;                                                         /* RGBA8 */
    vid_p[1].data[0] = mem + ny * 4; vid_p[1].data[1] = mem + ny * 5;               /* NV12 */
    vid_p[2].data[0] = mem + ny * 4; vid_p[2].data[1] = mem + ny * 5; vid_p[2].data[2] = mem + ny * 5 + ny / 4;   /* I420 */
    for (k = 0; k < 3; k++) {
        memset(&td, 0, sizeof td);
        td.w = 1920; td.h = 1080; td.format = k == 0 ? YGFX_RGBA8 : (k == 1 ? YGFX_NV12 : YGFX_I420);
        if (k) {
            td.enc.matrix = YGFX_MATRIX_BT709; td.enc.range = YGFX_RANGE_LIMITED; td.enc.transfer = YGFX_TRC_BT1886;
            td.enc.primaries = YGFX_PRIM_DEVICE; td.enc.siting = YGFX_SITING_LEFT;
            td.planes = &vid_p[k];
        } else {
            td.data = mem;
        }
        vid_t[k] = ygfx_texture(&gfx, &td);
        if (!vid_t[k].id) { fprintf(stderr, "gfx_bench: %s\n", ygfx_error(&gfx)); free(mem); return; }
    }
    memset(&idd, 0, sizeof idd);
    for (k = 0; k < 3; k++) { idd.tex = vid_t[k]; st[k] = ygfx_image(&gfx, &idd); }
    memset(w, 0, sizeof w);
    w[0].name = "empty frame";
    w[1].name = "RGBA8 1920x1080, draw"; w[1].s = &st[0]; w[1].n = 1;
    w[2].name = "NV12 1920x1080 BT.709 BT1886, draw"; w[2].s = &st[1]; w[2].n = 1;
    w[3].name = "I420 1920x1080 BT.709 BT1886, draw"; w[3].s = &st[2]; w[3].n = 1;
    w[4].name = "RGBA8, upload + draw"; w[4].s = &st[0]; w[4].n = 1; w[4].prep = prep_vid_rgba;
    w[5].name = "NV12, upload planes + draw"; w[5].s = &st[1]; w[5].n = 1; w[5].prep = prep_vid_nv12;
    bench_cmp("v0.4 video, 1080p", w, 6);
    /* the transfers: the same NV12 frame, one texture per transfer */
    {
        ygfx_tex tt[3];
        for (k = 0; k < 3; k++) {
            memset(&td, 0, sizeof td);
            td.w = 1920; td.h = 1080; td.format = YGFX_NV12; td.planes = &vid_p[1];
            td.enc.matrix = YGFX_MATRIX_BT709; td.enc.range = YGFX_RANGE_LIMITED;
            td.enc.transfer = (uint8_t)(k == 0 ? YGFX_TRC_LINEAR : (k == 1 ? YGFX_TRC_BT1886 : YGFX_TRC_DEVICE));
            td.enc.primaries = YGFX_PRIM_DEVICE; td.enc.siting = YGFX_SITING_LEFT;
            tt[k] = ygfx_texture(&gfx, &td);
            idd.tex = tt[k];
            st[3 + k] = ygfx_image(&gfx, &idd);
        }
        memset(w, 0, sizeof w);
        w[0].name = "empty frame";
        w[1].name = "RGBA8 1920x1080, draw"; w[1].s = &st[0]; w[1].n = 1;
        for (k = 0; k < 3; k++) {
            static char nm[3][64];
            snprintf(nm[k], sizeof nm[k], "NV12 1920x1080, transfer %s, draw", tname[k]);
            w[2 + k].name = nm[k]; w[2 + k].s = &st[3 + k]; w[2 + k].n = 1;
        }
        bench_cmp("v0.4 video transfers, 1080p", w, 5);
        for (k = 0; k < 3; k++) ygfx_texture_free(&gfx, tt[k]);
    }
    printf("\nv0.4 video: the update call's CPU time, 1920 x 1080, 400 calls (a glFinish every 4)\n\n");
    printf("| %-48s | %8s | %8s |\n", "update", "mean us", "p99 us");
    printf("|--------------------------------------------------|----------|----------|\n");
    for (i = 0; i < 2; i++) {   /* A/B/A/B */
        vid_update_cpu("RGBA8, ygfx_texture_update (8.3 MB)", 0, 400);
        vid_update_cpu("NV12, ygfx_texture_update_planes (3.1 MB)", 1, 400);
        vid_update_cpu("I420, ygfx_texture_update_planes (3.1 MB)", 2, 400);
    }
    {   /* rebind: per frame, nothing but a store */
        uint64_t a = yrt_now_ns();
        for (i = 0; i < 100000; i++) ygfx_texture_rebind(&gfx, vid_t[1], (i & 1) ? vid_t[1] : vid_t[1]);
        printf("\nygfx_texture_rebind: %.3f us per call (100000 calls)\n", (double)(yrt_now_ns() - a) * 1e-3 / 100000);
    }
    for (k = 0; k < 3; k++) ygfx_texture_free(&gfx, vid_t[k]);
    free(mem);
}

/* --- v0.6: curve runs and the blur pass ------------------------------------------ */

/* Fonts through ysp/outline.h: a system font read at run time, so no
 * outline is committed. */
typedef struct tt_font {
    unsigned char* d; size_t n;
    yol_font f;
} tt_font;
static yol_ctx tt_cx;

static int tt_open(tt_font* f, const char* path) {
    FILE* fp = fopen(path, "rb");
    char err[200];
    memset(f, 0, sizeof *f);
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    f->n = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f->d = (unsigned char*)malloc(f->n);
    if (!f->d || fread(f->d, 1, f->n, fp) != f->n) { fclose(fp); free(f->d); f->d = NULL; return 0; }
    fclose(fp);
    if (yol_font_open(&f->f, f->d, f->n, 0, err, sizeof err) < 0) { printf("v0.6 text: %s: %s\n", path, err); return 0; }
    return 1;
}

static int tt_gid(const tt_font* f, int ch) { return (int)yol_font_glyph_index(&f->f, (uint32_t)ch); }

static double tt_adv(const tt_font* f, int g) {
    double a, l;
    return yol_font_hmetrics(&f->f, (uint32_t)g, &a, &l) >= 0 ? a : 0.5;
}

/* A set of glyphs gids[0 .. n - 1] (table entries by glyph id), resolved
 * unless YGFX_BENCH_UNRESOLVED, bands as given (0: the builder's rule). */
static int tt_build(const tt_font* f, yol_cset* s, const int* gids, int n, int bands, int backward, int keep) {
    static unsigned char seen[65536];
    static uint32_t list[65536];
    yol_cset_desc d;
    uint32_t m = 0;
    int i;
    memset(seen, 0, sizeof seen);
    for (i = 0; i < n; i++) if (gids[i] > 0 && gids[i] < 65536 && !seen[gids[i]]) { seen[gids[i]] = 1; list[m++] = (uint32_t)gids[i]; }
    memset(&d, 0, sizeof d);
    d.n_glyphs = (uint32_t)f->f.n_glyphs; d.nh = d.nv = bands; d.backward = backward != 0; d.keep_overlaps = keep != 0;
    if (yol_cset_init(s, &tt_cx, &d) != YOL_OK) return 0;
    if (yol_cset_add_font(s, &f->f, list, m, 0) < 0) { printf("v0.6 text: %s\n", yol_error(&tt_cx)); return 0; }
    return 1;
}

/* A page of text: lines of glyphs of em size px, advanced by the font. */
static int tt_page(const tt_font* f, const int* gids, int ng, double px, double lh, ygfx_citem* it, int cap) {
    double x = 4, y = px;
    int n = 0, k = 0;
    while (n < cap) {
        int g = gids[k++ % ng];
        double a = tt_adv(f, g) * px;
        if (x + a > W - 4) { x = 4; y += lh; }
        if (y > H - 4) break;
        memset(&it[n], 0, sizeof it[n]);
        it[n].x = (float)x; it[n].y = (float)y; it[n].glyph = (float)g;
        n++;
        x += a;
    }
    return n;
}

static ygfx_stim bt_run(ygfx_cset set, ygfx_buf buf, int n, double px, float ori) {
    ygfx_crun_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.place = YGFX_TOP_LEFT; rd.anchor = YGFX_TOP_LEFT; rd.set = set; rd.buf = buf; rd.n = n;
    rd.size = (float)px; rd.w = (float)W; rd.h = (float)H; rd.ori = ori;
    rd.color[0] = rd.color[1] = rd.color[2] = 0.9f;
    return ygfx_crun(&gfx, &rd);
}

static void bench_v06_text(void) {
    static ygfx_citem it[4][30000];
    static int lat[95], cjk[7000];
    tt_font fl, fc;
    yol_cset sl, sc;
    ygfx_cset setl, setc;
    ygfx_buf buf[4];
    ygfx_stim st[10];
    ygfx_tex page;
    bench_work w[CMP_MAX];
    ygfx_cset_desc d;
    int n[4], i, nl = 0, bands = getenv("YGFX_BENCH_BANDS") ? atoi(getenv("YGFX_BENCH_BANDS")) : 0;
    int bwd = getenv("YGFX_BENCH_FORWARD") ? 0 : 1, keep = getenv("YGFX_BENCH_UNRESOLVED") != NULL;
    uint64_t t0;
    if (!run("v0.6 text")) return;
    if (!tt_open(&fl, "C:/Windows/Fonts/segoeui.ttf") || !tt_open(&fc, "C:/Windows/Fonts/msyh.ttc")) {
        printf("v0.6 text: needs Segoe UI and Microsoft YaHei (C:/Windows/Fonts)\n");
        return;
    }
    if (getenv("YGFX_BENCH_TEXT_V3")) ygfx__text_v3 = atoi(getenv("YGFX_BENCH_TEXT_V3"));
    if (getenv("YGFX_BENCH_NOAUTO")) ygfx__text_auto = 0;   /* turned runs on the exact-area program, as v0.7 */
    if (!reopen(scene_fmt, YGFX_DITHER_NONE, 0)) return;
    for (i = 33; i < 127; i++) { int g = tt_gid(&fl, i); if (g) lat[nl++] = g; }
    for (i = 0; i < 7000; i++) cjk[i] = tt_gid(&fc, 0x4E00 + i);
    t0 = yrt_now_ns();
    if (yol_init(&tt_cx, NULL) != YOL_OK) return;
    if (!tt_build(&fl, &sl, lat, nl, bands, bwd, keep) || !tt_build(&fc, &sc, cjk, 7000, bands, bwd, keep)) { printf("v0.6 text: build failed\n"); return; }
    printf("v0.6 text: ysp/outline.h built %d Latin and 7000 CJK glyphs in %.0f ms (bands %d, backward %d, %s): %u + %u texels, "
           "%u + %u words\n", nl, (double)(yrt_now_ns() - t0) * 1e-6, bands, bwd, keep ? "overlaps kept" : "resolved",
           sl.n_texels, sc.n_texels, sl.n_words, sc.n_words);
    memset(&d, 0, sizeof d);
    d.texels = sl.texels; d.n_texels = sl.n_texels; d.words = sl.words; d.n_words = sl.n_words;
    t0 = yrt_now_ns();
    setl = ygfx_cset_make(&gfx, &d);
    printf("v0.6 text: ygfx_cset_make, Latin set (first, with the program): %.1f ms\n", (double)(yrt_now_ns() - t0) * 1e-6);
    d.texels = sc.texels; d.n_texels = sc.n_texels; d.words = sc.words; d.n_words = sc.n_words;
    t0 = yrt_now_ns();
    setc = ygfx_cset_make(&gfx, &d);
    printf("v0.6 text: ygfx_cset_make, 7000 CJK glyphs: %.1f ms\n", (double)(yrt_now_ns() - t0) * 1e-6);
    if (!setl.id || !setc.id) { printf("v0.6 text: %s\n", ygfx_error(&gfx)); return; }
    n[0] = tt_page(&fl, lat, nl, 12, 14, it[0], 30000);
    n[1] = tt_page(&fl, lat, nl, 48, 56, it[1], 30000);
    n[2] = tt_page(&fc, cjk, 7000, 16, 20, it[2], 30000);
    n[3] = tt_page(&fc, cjk + 100, 45, 200, 240, it[3], 45);
    for (i = 0; i < 4; i++) {
        buf[i] = ygfx_buffer(&gfx, (size_t)n[i] * sizeof(ygfx_citem));
        ygfx_buffer_update(&gfx, buf[i], 0, it[i], (size_t)n[i] * sizeof(ygfx_citem));
    }
    st[0] = bt_run(setl, buf[0], n[0], 12, 0);
    st[1] = bt_run(setl, buf[1], n[1], 48, 0);
    st[2] = bt_run(setc, buf[2], n[2], 16, 0);
    st[3] = bt_run(setc, buf[3], n[3], 200, 0);
    st[4] = bt_run(setl, buf[0], n[0], 12, 15);
    st[5] = bt_run(setc, buf[2], n[2], 16, 15);
    /* the 12 px page with .rays (v0.7), and drawn once into an RGBA16F
     * target that is composited each frame at 1:1 on whole px */
    {
        ygfx_crun_desc rd;
        memset(&rd, 0, sizeof rd);
        rd.place = YGFX_TOP_LEFT; rd.anchor = YGFX_TOP_LEFT; rd.set = setl; rd.buf = buf[0]; rd.n = n[0];
        rd.size = 12; rd.w = (float)W; rd.h = (float)H; rd.rays = true;
        rd.color[0] = rd.color[1] = rd.color[2] = 0.9f;
        st[7] = ygfx_crun(&gfx, &rd);
    }
    {
        ygfx_target_desc td;
        ygfx_image_desc idd;
        yscr_frame f;
        static const float zero[4] = { 0, 0, 0, 0 };
        memset(&td, 0, sizeof td);
        td.w = W; td.h = H; td.format = YGFX_RGBA16F;
        page = ygfx_target(&gfx, &td);
        memset(&f, 0, sizeof f);
        ygfx_begin(&gfx, &f);
        ygfx_begin_target(&gfx, page, zero);
        ygfx_draw(&gfx, &st[0]);
        ygfx_end_target(&gfx);
        ygfx_end(&gfx);
        memset(&idd, 0, sizeof idd);
        idd.tex = page;
        st[8] = ygfx_image(&gfx, &idd);
    }
    memset(w, 0, sizeof w);
    {
        static char nm[8][80];
        static const char* const what[8] = { "12 px Latin page", "48 px Latin", "16 px CJK page", "200 px CJK", "12 px Latin page, turned 15 deg",
                                             "16 px CJK page, turned 15 deg", "12 px Latin page, .rays", "12 px Latin page, cached in RGBA16F" };
        static const int src[8] = { 0, 1, 2, 3, 0, 2, 0, 0 }, sti[8] = { 0, 1, 2, 3, 4, 5, 7, 8 };
        w[0].name = "empty frame";
        for (i = 0; i < 8; i++) {
            snprintf(nm[i], sizeof nm[i], "%s, %d glyphs", what[i], n[src[i]]);
            w[1 + i].name = nm[i]; w[1 + i].s = &st[sti[i]]; w[1 + i].n = 1;
        }
        bench_cmp("v0.6 text", w, 9);
    }
    {   /* a 40-glyph line at 48 px with its items copied each frame: the CPU of draw() */
        ygfx_crun_desc rd;
        static ygfx_citem line[40];
        memcpy(line, it[1], sizeof line);
        memset(&rd, 0, sizeof rd);
        rd.place = YGFX_TOP_LEFT; rd.anchor = YGFX_TOP_LEFT; rd.set = setl; rd.items = line; rd.n = 40; rd.size = 48;
        rd.w = (float)W; rd.h = (float)H; rd.color[0] = 1; rd.fields = YGFX_I_ORI | YGFX_I_GATE;
        for (i = 0; i < 40; i++) line[i].gate = 1;
        st[6] = ygfx_crun(&gfx, &rd);
        memset(w, 0, sizeof w);
        w[0].name = "empty frame";
        w[1].name = "a 40-glyph line at 48 px, items copied"; w[1].s = &st[6]; w[1].n = 1;
        bench_cmp("v0.6 text line", w, 2);
    }
    for (i = 0; i < 4; i++) ygfx_buffer_free(&gfx, buf[i]);
    ygfx_texture_free(&gfx, page);
    ygfx_cset_free(&gfx, setl); ygfx_cset_free(&gfx, setc);
    yol_cset_free(&sl); yol_cset_free(&sc);
    yol_free(&tt_cx);
    free(fl.d); free(fc.d);
}

static void bench_v06_blur(void) {
    static const ygfx_format fm[3] = { YGFX_R16F, YGFX_RGBA16F, YGFX_RGBA32F };
    static const char* const fn[3] = { "R16F", "RGBA16F", "RGBA32F" };
    static ygfx_blur bl[8];
    static char nm[8][80];
    bench_work w[CMP_MAX];
    ygfx_shape_desc sd;
    ygfx_stim rect;
    int k;
    if (!run("v0.6 blur")) return;
    if (!reopen(scene_fmt, YGFX_DITHER_NONE, 0)) return;
    memset(&sd, 0, sizeof sd);
    sd.shape = YGFX_RECT; sd.w = 200; sd.h = 100; sd.color[0] = sd.color[1] = sd.color[2] = 1;
    rect = ygfx_shape(&sd);
    /* full screen, sigma 0.5, 1, 2, 4 (R16F), then the formats at 2, then 2x */
    {
        static const double sg[4] = { 0.5, 1, 2, 4 };
        for (k = 0; k < 4; k++) {
            ygfx_blur_desc bd;
            memset(&bd, 0, sizeof bd);
            bd.w = W; bd.h = H; bd.format = YGFX_R16F; bd.sigma = (float)sg[k];
            if (ygfx_blur_make(&gfx, &bl[k], &bd) != YGFX_OK) { printf("v0.6 blur: %s\n", ygfx_error(&gfx)); return; }
        }
        memset(w, 0, sizeof w);
        w[0].name = "empty frame";
        for (k = 0; k < 4; k++) {
            snprintf(nm[k], sizeof nm[k], "full screen R16F, sigma %.1f", sg[k]);
            w[1 + k].name = nm[k]; w[1 + k].pass = 2; w[1 + k].blur = &bl[k]; w[1 + k].in = &rect; w[1 + k].n_in = 1;
        }
        bench_cmp("v0.6 blur, full screen", w, 5);
        for (k = 0; k < 4; k++) ygfx_blur_free(&gfx, &bl[k]);
    }
    {
        for (k = 0; k < 3; k++) {
            ygfx_blur_desc bd;
            memset(&bd, 0, sizeof bd);
            bd.w = W; bd.h = H; bd.format = fm[k]; bd.sigma = 2;
            if (ygfx_blur_make(&gfx, &bl[k], &bd) != YGFX_OK) { printf("v0.6 blur: %s\n", ygfx_error(&gfx)); return; }
        }
        memset(w, 0, sizeof w);
        w[0].name = "empty frame";
        for (k = 0; k < 3; k++) {
            snprintf(nm[k], sizeof nm[k], "full screen %s, sigma 2", fn[k]);
            w[1 + k].name = nm[k]; w[1 + k].pass = 2; w[1 + k].blur = &bl[k]; w[1 + k].in = &rect; w[1 + k].n_in = 1;
        }
        bench_cmp("v0.6 blur, formats", w, 4);
        for (k = 0; k < 3; k++) ygfx_blur_free(&gfx, &bl[k]);
    }
    {   /* a word: 400 x 200, at 1x and 2x, sigma 0.5 to 32 */
        static const double sg[7] = { 0.5, 1, 2, 4, 8, 16, 32 };
        int q;
        for (q = 1; q <= 2; q++) {
            for (k = 0; k < 7; k++) {
                ygfx_blur_desc bd;
                memset(&bd, 0, sizeof bd);
                bd.w = 400; bd.h = 200; bd.format = YGFX_R16F; bd.sigma = (float)sg[k]; bd.supersample = q;
                if (ygfx_blur_make(&gfx, &bl[k], &bd) != YGFX_OK) { printf("v0.6 blur: %s\n", ygfx_error(&gfx)); return; }
            }
            memset(w, 0, sizeof w);
            w[0].name = "empty frame";
            for (k = 0; k < 7; k++) {
                snprintf(nm[k], sizeof nm[k], "word 400 x 200 R16F %dx, sigma %.1f", q, sg[k]);
                w[1 + k].name = nm[k]; w[1 + k].pass = 2; w[1 + k].blur = &bl[k]; w[1 + k].in = &rect; w[1 + k].n_in = 1;
            }
            bench_cmp(q == 1 ? "v0.6 blur, word 1x" : "v0.6 blur, word 2x", w, 8);
            for (k = 0; k < 7; k++) ygfx_blur_free(&gfx, &bl[k]);
        }
    }
}

/* --- v0.7: NOISE GAUSSIAN against UNIFORM and BINARY, full screen ----------------- */

static void bench_v07_noise(void) {
    static ygfx_stim st[6];
    static char nm[6][80];
    static const char* const dn[3] = { "UNIFORM", "BINARY", "GAUSSIAN" };
    bench_work w[CMP_MAX];
    int k;
    if (!run("v0.7 noise")) return;
    if (!reopen(scene_fmt, YGFX_DITHER_NONE, 0)) return;
    memset(w, 0, sizeof w);
    w[0].name = "empty frame";
    for (k = 0; k < 6; k++) {
        ygfx_noise_desc nd;
        memset(&nd, 0, sizeof nd);
        nd.w = (float)W; nd.h = (float)H; nd.check = k < 3 ? 1.0f : 4.0f; nd.contrast = 0.2f;
        nd.aperture = YGFX_NO_APERTURE; nd.dist = (ygfx_noise_dist)(k % 3); nd.seed = 7;
        st[k] = ygfx_noise(&nd);
        snprintf(nm[k], sizeof nm[k], "noise %s, check %d px, full screen", dn[k % 3], k < 3 ? 1 : 4);
        w[1 + k].name = nm[k]; w[1 + k].s = &st[k]; w[1 + k].n = 1;
    }
    bench_cmp("v0.7 noise", w, 7);
    /* the dither's hash runs in the output stage, every pixel, every frame */
    if (reopen(scene_fmt, YGFX_DITHER_ORDERED, 1)) bench("v0.7 noise: empty frame, CLUT, ordered dither", NULL, 0, NULL, 0);
    if (reopen(scene_fmt, YGFX_DITHER_NOISE, 1)) bench("v0.7 noise: empty frame, CLUT, noise dither", NULL, 0, NULL, 0);
}

/* --- v0.10: NOISE SIMPLEX, full screen, by octaves --------------------------------- */

static void bench_v10_simplex(void) {
    static ygfx_stim st[6];
    static char nm[6][80];
    static const int oc[5] = { 1, 2, 3, 4, 8 };
    bench_work w[CMP_MAX];
    int k;
    if (!run("v0.10 simplex")) return;
    if (!reopen(scene_fmt, YGFX_DITHER_NONE, 0)) return;
    memset(w, 0, sizeof w);
    w[0].name = "empty frame";
    for (k = 0; k < 6; k++) {
        ygfx_noise_desc nd;
        memset(&nd, 0, sizeof nd);
        nd.w = (float)W; nd.h = (float)H; nd.contrast = 0.4f; nd.aperture = YGFX_NO_APERTURE; nd.seed = 7;
        nd.dist = k < 5 ? YGFX_SIMPLEX : YGFX_UNIFORM; nd.scale = 32; nd.octaves = k < 5 ? oc[k] : 1; nd.z = 0.5f;
        st[k] = ygfx_noise(&nd);
        if (k < 5) snprintf(nm[k], sizeof nm[k], "SIMPLEX, %d octave(s), scale 32 px, full screen", oc[k]);
        else snprintf(nm[k], sizeof nm[k], "UNIFORM, 1 px checks, full screen");
        w[1 + k].name = nm[k]; w[1 + k].s = &st[k]; w[1 + k].n = 1;
    }
    bench_cmp("v0.10 simplex", w, 7);
}

/* --- v0.4: instances and interleaved kinds ------------------------------------- */

static ygfx_inst bi_el[10000];
static void prep_ring(int frame) { (void)frame; }
static void prep_turn(int frame) {   /* the elements change every frame, as a drifting field */
    int i;
    for (i = 0; i < 10000; i++) bi_el[i].ori = (float)((i * 7 + frame) % 360);
}

static bool open_big(int max_draws) {
    ygfx_desc gd;
    ygfx_close(&gfx);
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    ygfx__test_scene32 = 0;
    gd.max_draws = max_draws;
    gd.cache = cache_desc;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_bench: %s\n", ygfx_error(&gfx)); return false; }
    return true;
}

static void bench_v04_inst(void) {
    static ygfx_stim plain[10000];
    ygfx_stim inst[6];
    ygfx_instances_desc id;
    ygfx_gabor_desc gd;
    ygfx_shape_desc sd;
    bench_work w[CMP_MAX];
    int i;
    if (!run("v0.4 inst")) return;
    if (!open_big(10100)) return;
    /* 10000 gabors of 32 x 32 on a 100 x 100 grid */
    ygfx_inst_grid(bi_el, 100, 100, 19, 12);
    memset(&gd, 0, sizeof gd);
    gd.sigma = 4; gd.sf = 1 / 8.0f; gd.contrast = 0.2f;
    for (i = 0; i < 10000; i++) {
        gd.x = bi_el[i].x; gd.y = bi_el[i].y; gd.ori = (float)((i * 7) % 360);
        plain[i] = ygfx_gabor(&gd);
        bi_el[i].ori = (float)((i * 7) % 360);
    }
    gd.x = gd.y = 0; gd.ori = 0;
    memset(&id, 0, sizeof id);
    id.inst = bi_el; id.n = 10000; id.fields = YGFX_I_XY | YGFX_I_ORI;
    inst[0] = ygfx_gabor(&gd);
    inst[0] = ygfx_instances(&gfx, &inst[0], &id);
    memset(&sd, 0, sizeof sd);
    sd.shape = YGFX_LINE; sd.w = 16; sd.shape_p[0] = 2; sd.edge = YGFX_EDGE_COSINE; sd.edge_width = 1;
    sd.color[0] = sd.color[1] = sd.color[2] = 0.8f;
    inst[1] = ygfx_shape(&sd);
    inst[1] = ygfx_instances(&gfx, &inst[1], &id);
    sd.shape = YGFX_RRECT; sd.w = 14; sd.h = 9; sd.shape_p[0] = sd.shape_p[1] = sd.shape_p[2] = sd.shape_p[3] = 3;
    inst[2] = ygfx_shape(&sd);
    inst[2] = ygfx_instances(&gfx, &inst[2], &id);
    if (!inst[0].n_inst || !inst[1].n_inst || !inst[2].n_inst) { fprintf(stderr, "gfx_bench: %s\n", ygfx_error(&gfx)); return; }
    memset(w, 0, sizeof w);
    w[0].name = "empty frame"; w[0].prep = prep_ring;
    w[1].name = "10000 gabors 32x32, one stimulus each"; w[1].s = plain; w[1].n = 10000; w[1].prep = prep_ring;
    w[2].name = "10000 gabors 32x32, instanced, ring"; w[2].s = &inst[0]; w[2].n = 1; w[2].prep = prep_ring;
    w[3].name = "10000 gabors, instanced, ori set each frame"; w[3].s = &inst[0]; w[3].n = 1; w[3].prep = prep_turn;
    w[4].name = "10000 LINE 16 px, instanced"; w[4].s = &inst[1]; w[4].n = 1; w[4].prep = prep_ring;
    w[5].name = "10000 RRECT 14x9 (vector), instanced"; w[5].s = &inst[2]; w[5].n = 1; w[5].prep = prep_ring;
    bench_cmp("v0.4 inst, 10000 elements", w, 6);
    {   /* 1000 gabors of 256 x 256: the GPU should not care */
        static ygfx_stim big[1000];
        ygfx_inst_grid(bi_el, 40, 25, 45, 45);
        gd.sigma = 32; gd.sf = 1 / 32.0f;
        for (i = 0; i < 1000; i++) { gd.x = bi_el[i].x; gd.y = bi_el[i].y; big[i] = ygfx_gabor(&gd); }
        gd.x = gd.y = 0;
        id.n = 1000; id.fields = YGFX_I_XY;
        inst[3] = ygfx_gabor(&gd);
        inst[3] = ygfx_instances(&gfx, &inst[3], &id);
        memset(w, 0, sizeof w);
        w[0].name = "empty frame"; w[0].prep = prep_ring;
        w[1].name = "1000 gabors 256x256, one stimulus each"; w[1].s = big; w[1].n = 1000; w[1].prep = prep_ring;
        w[2].name = "1000 gabors 256x256, instanced"; w[2].s = &inst[3]; w[2].n = 1; w[2].prep = prep_ring;
        bench_cmp("v0.4 inst, 1000 large gabors", w, 3);
    }
    {   /* the hit test over 10000 rects: the target in a search array */
        uint64_t t0;
        int hits = 0;
        id.n = 10000; id.fields = YGFX_I_XY | YGFX_I_ORI;
        ygfx_inst_grid(bi_el, 100, 100, 19, 12);
        sd.shape = YGFX_RECT; sd.w = 14; sd.h = 4;
        inst[4] = ygfx_shape(&sd);
        inst[4] = ygfx_instances(&gfx, &inst[4], &id);
        t0 = yrt_now_ns();
        for (i = 0; i < 100; i++) hits += ygfx_hit_index(&gfx, &inst[4], 960.0f + (float)(i % 10), 600.0f + (float)(i / 10)) >= 0;
        printf("\nygfx_hit_index over 10000 RECT elements: %.1f us per call (100 calls, %d hits)\n",
               (double)(yrt_now_ns() - t0) * 1e-3 / 100, hits);
    }
}

static void prep_order(int frame) { (void)frame; gfx.no_reorder = 0; }
static void prep_call_order(int frame) { (void)frame; gfx.no_reorder = 1; }

/* Interleaved kinds: 1000 stimuli, four kinds in turn, against the same
 * stimuli in groups of one kind (what batching gets today). */
static void bench_v04_mixed(void) {
    static ygfx_stim inter[1000], grouped[1000], over[1000], dense[1000];
    ygfx_gabor_desc gd;
    ygfx_shape_desc sd;
    ygfx_grating_desc rd;
    bench_work w[CMP_MAX];
    int i, k, n = 0;
    if (!run("v0.4 mixed")) return;
    if (!open_big(1100)) return;
    ygfx_inst_grid(bi_el, 40, 25, 46, 46);
    for (i = 0; i < 1000; i++) {
        float x = bi_el[i].x, y = bi_el[i].y;
        switch (i % 4) {
        case 0:
            memset(&gd, 0, sizeof gd);
            gd.x = x; gd.y = y; gd.sigma = 5; gd.sf = 1 / 8.0f; gd.contrast = 0.2f; gd.ori = (float)(i % 180);
            inter[i] = ygfx_gabor(&gd);
            break;
        case 1:
            memset(&sd, 0, sizeof sd);
            sd.shape = YGFX_CIRCLE; sd.x = x; sd.y = y; sd.w = 30; sd.edge = YGFX_EDGE_COSINE; sd.edge_width = 1.5f;
            sd.color[0] = 0.8f; sd.color[1] = 0.3f; sd.color[2] = 0.2f;
            inter[i] = ygfx_shape(&sd);
            break;
        case 2:
            memset(&rd, 0, sizeof rd);
            rd.x = x; rd.y = y; rd.w = 32; rd.sf = 1 / 6.0f; rd.contrast = 0.2f; rd.aperture = YGFX_CIRCLE;
            inter[i] = ygfx_grating(&rd);
            break;
        default:
            memset(&sd, 0, sizeof sd);
            sd.shape = YGFX_RRECT; sd.x = x; sd.y = y; sd.w = 34; sd.h = 20;
            sd.shape_p[0] = sd.shape_p[1] = sd.shape_p[2] = sd.shape_p[3] = 5;
            sd.color[0] = 0.2f; sd.color[1] = 0.4f; sd.color[2] = 0.8f;
            inter[i] = ygfx_shape(&sd);
            break;
        }
    }
    for (k = 0; k < 4; k++) for (i = k; i < 1000; i += 4) grouped[n++] = inter[i];
    /* 10 % overlap their neighbor: every tenth moved onto the next cell */
    for (i = 0; i < 1000; i++) { over[i] = inter[i]; if (i % 10 == 0) over[i].x += 30.0f; }
    /* dense: random places, most draws overlap something (the reorder's
     * cost with little to gain) */
    for (i = 0; i < 1000; i++) {
        dense[i] = inter[i];
        dense[i].x = (float)((i * 7919) % 1800) - 900.0f;
        dense[i].y = (float)((i * 104729) % 1100) - 550.0f;
    }
    memset(w, 0, sizeof w);
    w[0].name = "empty frame"; w[0].prep = prep_order;
    w[1].name = "1000, 4 kinds interleaved, call order"; w[1].s = inter; w[1].n = 1000; w[1].prep = prep_call_order;
    w[2].name = "the same, reordered"; w[2].s = inter; w[2].n = 1000; w[2].prep = prep_order;
    w[3].name = "the same, grouped by kind by the caller"; w[3].s = grouped; w[3].n = 1000; w[3].prep = prep_call_order;
    w[4].name = "10 % overlapping, call order"; w[4].s = over; w[4].n = 1000; w[4].prep = prep_call_order;
    w[5].name = "10 % overlapping, reordered"; w[5].s = over; w[5].n = 1000; w[5].prep = prep_order;
    w[6].name = "random places (dense), call order"; w[6].s = dense; w[6].n = 1000; w[6].prep = prep_call_order;
    w[7].name = "random places (dense), reordered"; w[7].s = dense; w[7].n = 1000; w[7].prep = prep_order;
    bench_cmp("v0.4 mixed kinds", w, 8);
    gfx.no_reorder = 0;
}

static ygfx_group bench_group;
static void prep_group(int frame) { bench_group.x = (float)(frame % 200) - 100.0f; }

static void bench_v02(ygfx_stim* g) {
    static ygfx_stim st[8], sprites[1000], members[100], rects[2];
    static float pent[10];
    static unsigned char atlas[512 * 512 * 4];
    enum { M = 256, P = 16 };
    static uint8_t mask[M * M];
    static float sdf[(M + 2 * P) * (M + 2 * P)];
    bench_work w[CMP_MAX];
    ygfx_shape_desc sd;
    ygfx_texture_desc td;
    ygfx_target_desc rd;
    ygfx_image_desc idd;
    ygfx_group_desc gd;
    ygfx_tex mtex, atex, t;
    int i, x, y;

    /* strokes against fills: a large circle and a large pentagon */
    memset(&sd, 0, sizeof sd);
    sd.shape = YGFX_CIRCLE; sd.w = (float)(H - 40); sd.edge = YGFX_EDGE_COSINE; sd.edge_width = 2;
    sd.color[0] = sd.color[1] = sd.color[2] = 0.6f;
    st[0] = ygfx_shape(&sd);
    sd.stroke = 8;
    st[1] = ygfx_shape(&sd);
    for (i = 0; i < 5; i++) {
        pent[2 * i] = (float)(cos(i * 2 * 3.14159265358979 / 5) * 0.48 * (H - 40));
        pent[2 * i + 1] = (float)(sin(i * 2 * 3.14159265358979 / 5) * 0.48 * (H - 40));
    }
    sd.shape = YGFX_POLYGON; sd.stroke = 0; sd.shape_p[0] = 5; sd.vertices = pent; sd.h = sd.w;
    st[2] = ygfx_shape(&sd);
    sd.stroke = 8; sd.join = YGFX_JOIN_MITER;
    st[3] = ygfx_shape(&sd);
    sd.join = YGFX_JOIN_ROUND;
    st[4] = ygfx_shape(&sd);
    /* a mask of the same circle */
    for (y = 0; y < M; y++)
        for (x = 0; x < M; x++)
            mask[y * M + x] = (uint8_t)(((x - 128) * (x - 128) + (y - 128) * (y - 128) <= 120 * 120) ? 255 : 0);
    ygfx_sdf_from_mask(sdf, mask, M, M, P);
    memset(&td, 0, sizeof td);
    td.w = td.h = M + 2 * P; td.format = YGFX_R16F; td.data = sdf;
    mtex = ygfx_texture(&gfx, &td);
    memset(&sd, 0, sizeof sd);
    sd.shape = YGFX_MASK_TEX; sd.mask = mtex; sd.w = sd.h = (float)(H - 40);
    sd.edge = YGFX_EDGE_COSINE; sd.edge_width = 2;
    sd.color[0] = sd.color[1] = sd.color[2] = 0.6f;
    st[5] = ygfx_shape(&sd);
    /* a large rect, filled and with a stroke beveled below the limit */
    memset(&sd, 0, sizeof sd);
    sd.w = (float)(W - 40); sd.h = (float)(H - 40); sd.edge = YGFX_EDGE_COSINE; sd.edge_width = 2;
    sd.color[0] = sd.color[1] = sd.color[2] = 0.6f;
    rects[0] = ygfx_shape(&sd);
    sd.stroke = 8; sd.join = YGFX_JOIN_MITER; sd.miter_limit = 1.2f;
    rects[1] = ygfx_shape(&sd);
    memset(w, 0, sizeof w);
    w[0].name = "empty frame";
    w[1].name = "circle, fill";                           w[1].s = &st[0]; w[1].n = 1;
    w[2].name = "circle, 8 px stroke";                    w[2].s = &st[1]; w[2].n = 1;
    w[3].name = "pentagon, fill";                         w[3].s = &st[2]; w[3].n = 1;
    w[4].name = "pentagon, 8 px stroke, MITER";           w[4].s = &st[3]; w[4].n = 1;
    w[5].name = "pentagon, 8 px stroke, ROUND";           w[5].s = &st[4]; w[5].n = 1;
    w[6].name = "MASK_TEX circle (256 + 32 texels), fill"; w[6].s = &st[5]; w[6].n = 1;
    w[7].name = "rect, fill";                             w[7].s = &rects[0]; w[7].n = 1;
    bench_cmp("v0.2 strokes and masks", w, 8);
    w[1].name = "rect, 8 px stroke, MITER, limit 1.2";    w[1].s = &rects[1]; w[1].n = 1;
    w[2] = w[7];
    bench_cmp("v0.2 strokes, beveled rect", w, 3);
    ygfx_texture_free(&gfx, mtex);

    /* 1000 sprites of 32 x 32 from one 512 x 512 atlas */
    for (i = 0; i < 512 * 512 * 4; i++) atlas[i] = (unsigned char)(i * 13);
    memset(&td, 0, sizeof td);
    td.w = td.h = 512; td.format = YGFX_RGBA8; td.data = atlas;
    atex = ygfx_texture(&gfx, &td);
    for (i = 0; i < 1000; i++) {
        memset(&idd, 0, sizeof idd);
        idd.tex = atex; idd.src[0] = (float)((i % 16) * 32); idd.src[1] = (float)(((i / 16) % 16) * 32);
        idd.src[2] = idd.src[3] = 32;
        idd.x = (float)((i * 37) % 1800) - 900.0f; idd.y = (float)((i * 53) % 1100) - 550.0f;
        sprites[i] = ygfx_image(&gfx, &idd);
    }
    /* a group of 100 gabors moved each frame by a bound x, at scale 1 */
    memset(&gd, 0, sizeof gd);
    gd.ori = 10;
    bench_group = ygfx_group_make(&gd);
    for (i = 0; i < 100; i++) { members[i] = g[i]; members[i].group = &bench_group; }
    memset(w, 0, sizeof w);
    w[0].name = "empty frame";
    w[1].name = "1000 sprites 32 x 32, one atlas";        w[1].s = sprites; w[1].n = 1000;
    w[2].name = "100 gabors, no group";                   w[2].s = g; w[2].n = 100;
    w[3].name = "the same 100 gabors in a group, x moved"; w[3].s = members; w[3].n = 100; w[3].prep = prep_group;
    bench_cmp("v0.2 sprites and groups", w, 4);
    ygfx_texture_free(&gfx, atex);

    /* targets: a full-screen RGBA16F target; 1000 gabors drawn into it once
     * and added each frame, against drawing them each frame */
    memset(&rd, 0, sizeof rd);
    rd.w = W; rd.h = H;
    t = ygfx_target(&gfx, &rd);
    {
        yscr_frame f;
        static const float zero[4] = { 0, 0, 0, 0 };
        memset(&f, 0, sizeof f);
        ygfx_begin(&gfx, &f);
        ygfx_begin_target(&gfx, t, zero);
        ygfx_draw_n(&gfx, g, 1000);
        ygfx_end_target(&gfx);
        ygfx_end(&gfx);
    }
    memset(&idd, 0, sizeof idd);
    idd.tex = t; idd.add = true;
    st[6] = ygfx_image(&gfx, &idd);
    memset(&idd, 0, sizeof idd);
    idd.tex = t;
    st[7] = ygfx_image(&gfx, &idd);
    memset(w, 0, sizeof w);
    w[0].name = "empty frame";
    w[1].name = "an empty target pass (cleared)";         w[1].t = t; w[1].pass = 1;
    w[2].name = "full-screen target composite, COLOR";    w[2].s = &st[7]; w[2].n = 1;
    w[3].name = "full-screen target composite, ADD";      w[3].s = &st[6]; w[3].n = 1;
    w[4].name = "1000 gabors drawn each frame";           w[4].s = g; w[4].n = 1000;
    bench_cmp("v0.2 targets", w, 5);
    {   /* The pass switch: 16 passes a frame, each a small shape into the
         * target and one into the scene between them, against the same 32
         * shapes all in the scene. Each pass then costs two framebuffer
         * binds, two viewports and a frame block range. */
        yscr_frame f;
        ygfx_stim dot;
        double cpu[CMP_REPS], base[CMP_REPS], gpu[CMP_REPS], gbase[CMP_REPS];
        uint64_t a0, t0;
        int r, k;
        memset(&sd, 0, sizeof sd);
        sd.w = sd.h = 8; sd.color[0] = sd.color[1] = sd.color[2] = 0.6f;
        dot = ygfx_shape(&sd);
        memset(&f, 0, sizeof f);
        for (r = 0; r < CMP_REPS; r++) {
            for (k = 0; k < 2; k++) {
                double c = 0;
                glFinish_();
                t0 = yrt_now_ns();
                for (i = 0; i < CMP_BLOCK; i++) {
                    int q;
                    a0 = yrt_now_ns();
                    ygfx_begin(&gfx, &f);
                    for (q = 0; q < YGFX_MAX_PASSES; q++) {
                        if (k) ygfx_begin_target(&gfx, t, NULL);
                        ygfx_draw(&gfx, &dot);
                        if (k) ygfx_end_target(&gfx);
                        ygfx_draw(&gfx, &dot);
                    }
                    ygfx_end(&gfx);
                    c += (double)(yrt_now_ns() - a0) * 1e-3;
                }
                glFinish_();
                (k ? gpu : gbase)[r] = (double)(yrt_now_ns() - t0) * 1e-3 / CMP_BLOCK;
                (k ? cpu : base)[r] = c / CMP_BLOCK;
            }
        }
        for (r = 0; r < CMP_REPS; r++) {
            cpu[r] = (cpu[r] - base[r]) / YGFX_MAX_PASSES;
            gpu[r] = (gpu[r] - gbase[r]) / YGFX_MAX_PASSES;
        }
        printf("\nv0.2 pass switch: %.1f us of CPU and %.1f us of frame time per target pass (median of %d rounds, "
               "%d passes a frame, each between scene draws, against the same draws with no pass)\n",
               median_of(cpu, CMP_REPS), median_of(gpu, CMP_REPS), CMP_REPS, YGFX_MAX_PASSES);
    }
    ygfx_texture_free(&gfx, t);
}

/* --- v0.3 ------------------------------------------------------------------ */

static void bench_v03(void) {
    static ygfx_stim st[8], comp[6], many[100], glyphs;
    static ygfx_prim pr[6][32], sprims[100][4];
    static ycol_cal cal;
    static ygfx_paint rgbp, okp;
    static ygfx_fx fx;
    static float msdf[64 * 64 * 4];
    static ygfx_glyph gl[200];
    static const float xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };
    bench_work w[CMP_MAX];
    ygfx_shape_desc sd;
    ygfx_compound_desc cd;
    ygfx_desc gd;
    ygfx_texture_desc td;
    ygfx_glyphs_desc gld;
    ygfx_tex at;
    ygfx_buf gb;
    double open_ms[3];
    int i, k;
    /* open time with every program, three times */
    ycol_cal_nominal(&cal, xy, 80.0f, 2.2);
    for (k = 0; k < 3; k++) {
        uint64_t t0;
        ygfx_close(&gfx);
        memset(&gd, 0, sizeof gd);
        gd.screen = &scr; gd.cal = &cal; gd.max_draws = 4096;
        gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
        t0 = yrt_now_ns();
        if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx_bench: %s\n", ygfx_error(&gfx)); return; }
        open_ms[k] = (double)(yrt_now_ns() - t0) * 1e-6;
    }
    printf("\nv0.3 ygfx_open (10 programs, the vector and glyph programs included): %.0f, %.0f, %.0f ms\n",
           open_ms[0], open_ms[1], open_ms[2]);

    /* single primitives against v0.2's rect, near full screen */
    memset(&sd, 0, sizeof sd);
    sd.w = (float)(W - 40); sd.h = (float)(H - 40); sd.edge = YGFX_EDGE_COSINE; sd.edge_width = 2;
    sd.color[0] = sd.color[1] = sd.color[2] = 0.6f; sd.shape_p[1] = 40;
    st[0] = ygfx_shape(&sd);                                      /* v0.2 rounded RECT */
    sd.shape = YGFX_RRECT; sd.shape_p[0] = sd.shape_p[1] = sd.shape_p[2] = sd.shape_p[3] = 40;
    st[1] = ygfx_shape(&sd);
    memset(&rgbp, 0, sizeof rgbp);
    rgbp.kind = YGFX_PAINT_LINEAR; rgbp.n = 3; rgbp.x0 = (float)(-W / 2); rgbp.x1 = (float)(W / 2);
    rgbp.stops[0].color[0] = 0.8f; rgbp.stops[1].t = 0.5f; rgbp.stops[1].color[1] = 0.8f; rgbp.stops[2].t = 1; rgbp.stops[2].color[2] = 0.8f;
    okp = rgbp; okp.space = YGFX_SPACE_OKLAB;
    sd.paint = &rgbp; st[2] = ygfx_shape(&sd);
    sd.paint = &okp; st[3] = ygfx_shape(&sd);
    sd.paint = NULL;
    memset(&sd, 0, sizeof sd);
    sd.edge = YGFX_EDGE_COSINE; sd.edge_width = 2; sd.color[0] = sd.color[1] = sd.color[2] = 0.6f;
    sd.shape = YGFX_STAR; sd.w = (float)(H - 40); sd.shape_p[0] = 5; sd.shape_p[1] = 0.5f; sd.shape_p[2] = 10;
    st[4] = ygfx_shape(&sd);
    sd.shape = YGFX_ELLIPSE; sd.w = (float)(W - 40); sd.h = (float)(H - 40);
    st[5] = ygfx_shape(&sd);
    sd.shape = YGFX_CIRCLE; sd.w = sd.h = (float)(H - 40); sd.stroke = 8;
    st[6] = ygfx_shape(&sd);
    sd.dash[0] = 30; sd.dash[1] = 15;
    st[7] = ygfx_shape(&sd);
    memset(w, 0, sizeof w);
    w[0].name = "empty frame";
    w[1].name = "v0.2 rounded rect, fill";               w[1].s = &st[0]; w[1].n = 1;
    w[2].name = "RRECT (vector program), fill";          w[2].s = &st[1]; w[2].n = 1;
    w[3].name = "RRECT, linear paint, RGB";              w[3].s = &st[2]; w[3].n = 1;
    w[4].name = "RRECT, linear paint, OKLAB";            w[4].s = &st[3]; w[4].n = 1;
    w[5].name = "STAR 5, rounded, fill";                 w[5].s = &st[4]; w[5].n = 1;
    w[6].name = "ELLIPSE, fill";                         w[6].s = &st[5]; w[6].n = 1;
    w[7].name = "v0.2 circle, 8 px stroke";              w[7].s = &st[6]; w[7].n = 1;
    bench_cmp("v0.3 primitives", w, 8);
    w[1] = w[7];
    w[2].name = "circle, 8 px stroke, dashed";           w[2].s = &st[7]; w[2].n = 1;
    bench_cmp("v0.3 dashes", w, 3);

    /* compounds: n circles of 300 px over the screen, smooth union; then 8
     * with every effect in a 600 x 400 box */
    for (k = 0; k < 4; k++) {
        int n = k == 0 ? 1 : (k == 1 ? 2 : (k == 2 ? 8 : 32));
        for (i = 0; i < n; i++) {
            memset(&pr[k][i], 0, sizeof pr[k][i]);
            pr[k][i].shape = YGFX_CIRCLE; pr[k][i].w = 300; pr[k][i].op = YGFX_OP_SMOOTH_UNION; pr[k][i].k = 40;
            pr[k][i].x = (float)((i * 397) % (W - 300)) - (W - 300) / 2.0f;
            pr[k][i].y = (float)((i * 211) % (H - 300)) - (H - 300) / 2.0f;
        }
        memset(&cd, 0, sizeof cd);
        cd.prims = pr[k]; cd.n = n; cd.w = (float)W; cd.h = (float)H; cd.edge = YGFX_EDGE_COSINE; cd.edge_width = 2;
        cd.color[0] = cd.color[1] = cd.color[2] = 0.6f;
        comp[k] = ygfx_compound(&cd);
    }
    for (i = 0; i < 8; i++) {
        memset(&pr[4][i], 0, sizeof pr[4][i]);
        pr[4][i].shape = YGFX_CIRCLE; pr[4][i].w = 120; pr[4][i].op = YGFX_OP_SMOOTH_UNION; pr[4][i].k = 30;
        pr[4][i].x = (float)(i % 4 * 140 - 210); pr[4][i].y = (float)(i / 4 * 160 - 80);
    }
    memset(&fx, 0, sizeof fx);
    fx.dx = 8; fx.dy = 8; fx.drop.sigma = 8; fx.drop.opacity = 0.5f; fx.glow.sigma = 6; fx.glow.opacity = 0.4f;
    fx.inner.sigma = 4; fx.inner.opacity = 0.5f; fx.band[0].a1 = -2; fx.band[0].opacity = 1; fx.band[1].a1 = 3; fx.band[1].a2 = 5; fx.band[1].opacity = 1;
    memset(&cd, 0, sizeof cd);
    cd.prims = pr[4]; cd.n = 8; cd.w = 600; cd.h = 400; cd.edge = YGFX_EDGE_COSINE; cd.edge_width = 2;
    cd.color[0] = cd.color[1] = cd.color[2] = 0.6f;
    comp[4] = ygfx_compound(&cd);
    cd.fx = &fx;
    comp[5] = ygfx_compound(&cd);
    memset(w, 0, sizeof w);
    w[0].name = "empty frame";
    w[1].name = "compound, 1 circle, full screen";        w[1].s = &comp[0]; w[1].n = 1;
    w[2].name = "compound, 2 circles";                    w[2].s = &comp[1]; w[2].n = 1;
    w[3].name = "compound, 8 circles";                    w[3].s = &comp[2]; w[3].n = 1;
    w[4].name = "compound, 32 circles";                   w[4].s = &comp[3]; w[4].n = 1;
    w[5].name = "compound of 8, 600 x 400 box";           w[5].s = &comp[4]; w[5].n = 1;
    w[6].name = "the same with every effect";             w[6].s = &comp[5]; w[6].n = 1;
    bench_cmp("v0.3 compounds", w, 7);

    /* the CPU: 100 small compounds (one draw each) against 100 rects
     * (batched); a run of 200 MSDF glyphs */
    for (i = 0; i < 100; i++) {
        for (k = 0; k < 4; k++) {
            memset(&sprims[i][k], 0, sizeof sprims[i][k]);
            sprims[i][k].shape = YGFX_CIRCLE; sprims[i][k].w = 16; sprims[i][k].x = (float)(k * 10 - 15);
            sprims[i][k].op = YGFX_OP_SMOOTH_UNION; sprims[i][k].k = 4;
        }
        memset(&cd, 0, sizeof cd);
        cd.prims = sprims[i]; cd.n = 4; cd.w = 60; cd.h = 20; cd.color[0] = 0.6f; cd.edge = YGFX_EDGE_COSINE; cd.edge_width = 1;
        cd.x = (float)((i * 37) % 1800) - 900.0f; cd.y = (float)((i * 53) % 1100) - 550.0f;
        many[i] = ygfx_compound(&cd);
    }
    for (i = 0; i < 64 * 64; i++) {
        double x = i % 64 + 0.5 - 32, y = i / 64 + 0.5 - 32, d = sqrt(x * x + y * y) - 20, v = 0.5 - d / 16;
        msdf[4 * i] = msdf[4 * i + 1] = msdf[4 * i + 2] = msdf[4 * i + 3] = (float)(v < 0 ? 0 : (v > 1 ? 1 : v));
    }
    memset(&td, 0, sizeof td);
    td.w = td.h = 64; td.format = YGFX_RGBA16F; td.data = msdf; td.sdf = YGFX_SDF_MSDF; td.sdf_range = 16;
    at = ygfx_texture(&gfx, &td);
    for (i = 0; i < 200; i++) { memset(&gl[i], 0, sizeof gl[i]); gl[i].x = (float)(i % 40 * 40); gl[i].y = (float)(i / 40 * 40); gl[i].sw = gl[i].sh = 64; }
    gb = ygfx_buffer(&gfx, sizeof gl);
    ygfx_buffer_update(&gfx, gb, 0, gl, sizeof gl);
    memset(&gld, 0, sizeof gld);
    gld.atlas = at; gld.buf = gb; gld.count = 200; gld.scale = 0.6f; gld.w = 1600; gld.h = 220;
    gld.edge = YGFX_EDGE_COSINE; gld.edge_width = 1; gld.color[0] = 0.9f;
    glyphs = ygfx_glyphs(&gld);
    memset(w, 0, sizeof w);
    w[0].name = "empty frame";
    w[1].name = "100 compounds of 4, 60 x 20 (one draw each)"; w[1].s = many; w[1].n = 100;
    w[2].name = "a run of 200 MSDF glyphs, 38 px";             w[2].s = &glyphs; w[2].n = 1;
    bench_cmp("v0.3 CPU and glyphs", w, 3);
    ygfx_buffer_free(&gfx, gb);
    ygfx_texture_free(&gfx, at);
}

int main(int argc, char** argv) {
    yscr_desc d;
    ygfx_stim g[1000], one;
    ygfx_gabor_desc gd;
    int i, no_cache = 0;
    char line[640];
    hl.device = YGFX_HL_HARDWARE;
#if !defined(_WIN32)
    hl.device = YGFX_HL_MESA;
#endif
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            const char* v = argv[++i];
            if (!strcmp(v, "hardware")) hl.device = YGFX_HL_HARDWARE;
            else if (!strcmp(v, "warp")) hl.device = YGFX_HL_WARP;
            else if (!strcmp(v, "swiftshader")) hl.device = YGFX_HL_SWIFTSHADER;
            else if (!strcmp(v, "mesa")) hl.device = YGFX_HL_MESA;
            else if (!strcmp(v, "llvmpipe")) hl.device = YGFX_HL_MESA_SOFTWARE;
            else { fprintf(stderr, "gfx_bench: unknown device %s\n", v); return 2; }
        } else if (!strcmp(argv[i], "--size") && i + 2 < argc) {
            W = atoi(argv[++i]); H = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            FRAMES = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--only") && i + 1 < argc) {
            only = argv[++i];
        } else if (!strcmp(argv[i], "--cache") && i + 1 < argc) {
            i++;
            cache_desc = strcmp(argv[i], ":mem:") ? ygfx_file_cache_init(&pcache, argv[i]) : &memc_cache;
        } else if (!strcmp(argv[i], "--no-cache")) {
            no_cache = 1;
        } else if (!strcmp(argv[i], "--open-only")) {
            open_only = 1;
        } else if (!strcmp(argv[i], "--scene") && i + 1 < argc) {
            i++;
            scene_fmt = !strcmp(argv[i], "16") ? YGFX_RGBA16F : (!strcmp(argv[i], "32") ? YGFX_RGBA32F : YGFX_FORMAT_NONE);
        } else {
            fprintf(stderr, "usage: gfx_bench [--device hardware|warp|swiftshader|mesa|llvmpipe] [--size W H] "
                            "[--frames N] [--only NAME] [--scene 16|32] [--cache DIR] [--no-cache] [--open-only]\n");
            return 2;
        }
    }
    /* The per-user folder unless told otherwise, so that a second run loads
     * the programs instead of compiling them (PROGRAM CACHE). */
    if (no_cache) cache_desc = NULL;
    else if (!cache_desc && ygfx_default_cache_dir(cache_dir, sizeof cache_dir) == YGFX_OK)
        cache_desc = ygfx_file_cache_init(&pcache, cache_dir);
    if (W < 256 || H < 256 || FRAMES < 10 || FRAMES > MAXF) { fprintf(stderr, "gfx_bench: bad size or frames\n"); return 2; }
    hl.w = W; hl.h = H;
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_CUSTOM;
    d.presenter = &ygfx_headless_presenter;
    d.presenter_ctx = &hl;
    d.sim_period_ns = 1000000;
    if (!yscr_open(&scr, &d)) { fprintf(stderr, "gfx_bench: %s\n", yscr_error(&scr)); return 1; }
    load_one(&glFinish_, "glFinish");
    load_raw();
    {
        uint64_t t0 = yrt_now_ns();
        if (!reopen(scene_fmt, YGFX_DITHER_NONE, 0)) return 1;
        printf("ygfx_open (all built-in programs): %.0f ms, batch %d stimuli\n",
               (double)(yrt_now_ns() - t0) * 1e-6, YGFX__BATCH);
        if (open_only) {
            ygfx_programs ps;
            ygfx_program_stats(&gfx, &ps);
            printf("open_ms %.1f internal_ms %.1f loaded %u compiled %u rejected %u stored %u store_ms %.1f\n",
                   (double)(yrt_now_ns() - t0) * 1e-6, (double)ps.open_ns * 1e-6, ps.loaded, ps.compiled,
                   ps.rejected, ps.stored, (double)ps.store_ns * 1e-6);
            ygfx_describe(&gfx, line, sizeof line);
            printf("%s\n", line);
            ygfx_close(&gfx);
            yscr_close(&scr);
            return 0;
        }
    }
    ygfx_describe(&gfx, line, sizeof line);
    printf("%s\n%d x %d, %d frames per row\n\n", line, W, H, FRAMES);
    printf("| %-52s | %9s | %9s | %9s |\n", "workload", "cpu us", "cpu p99", "gpu ms");
    printf("|%s|%s|%s|%s|\n", "------------------------------------------------------", "-----------", "-----------", "-----------");

    /* the output stage and the scene format */
    bench("empty frame, RGBA16F scene, identity CLUT", NULL, 0, NULL, 0);
    if (reopen(YGFX_RGBA16F, YGFX_DITHER_ORDERED, 1)) bench("empty frame, RGBA16F, CLUT 4096, ordered dither", NULL, 0, NULL, 0);
    if (reopen(YGFX_RGBA32F, YGFX_DITHER_NONE, 0)) bench("empty frame, RGBA32F scene, identity CLUT", NULL, 0, NULL, 0);
    if (reopen(YGFX_RGBA32F, YGFX_DITHER_ORDERED, 1)) bench("empty frame, RGBA32F, CLUT 4096, ordered dither", NULL, 0, NULL, 0);
    if (run("bare")) {   /* the floor: a clear of framebuffer 0 and nothing else */
        uint64_t t0;
        void (GLCALL *clear)(GLu) = NULL;
        load_one(&clear, "glClear");
        glBindFramebuffer_(0x8D40, 0);
        glFinish_();
        t0 = yrt_now_ns();
        for (i = 0; i < FRAMES; i++) { uint64_t a = yrt_now_ns(); clear(0x4000); cpu_us[i] = (double)(yrt_now_ns() - a) * 1e-3; }
        glFinish_();
        report("bare: clear framebuffer 0 only (raw GL)", FRAMES, (double)(yrt_now_ns() - t0), FRAMES);
        ygfx_reset_state(&gfx);
    }
    if (!reopen(scene_fmt, YGFX_DITHER_NONE, 0)) return 1;

    /* gabors, batched and one draw each */
    memset(&gd, 0, sizeof gd);
    gd.sigma = 32; gd.sf = 1 / 32.0f; gd.contrast = 0.2f;   /* 256 x 256 */
    for (i = 0; i < 1000; i++) {
        gd.x = (float)((i * 37) % 1600) - 800.0f;
        gd.y = (float)((i * 53) % 900) - 450.0f;
        g[i] = ygfx_gabor(&gd);
    }
    {
        static const int ns[4] = { 1, 10, 100, 1000 };
        int k;
        for (k = 0; k < 4; k++) {
            char name[80];
            snprintf(name, sizeof name, "gabors 256x256 x %d, batched", ns[k]);
            gfx.max_batch = YGFX__BATCH;
            bench(name, g, ns[k], NULL, 0);
            snprintf(name, sizeof name, "gabors 256x256 x %d, one draw each", ns[k]);
            gfx.max_batch = 1;
            bench(name, g, ns[k], NULL, 0);
        }
        gfx.max_batch = YGFX__BATCH;
    }
    if (run("raw")) { raw_uniforms(100); raw_uniforms(1000); }

    /* dots, positions uploaded every frame */
    {
        static const int ns[3] = { 1000, 10000, 100000 };
        int k;
        dot_xy = (float*)malloc(100000 * 8);
        dot_buf = ygfx_buffer(&gfx, 100000 * 8);
        for (k = 0; k < 3; k++) {
            ygfx_dots_desc dd;
            char name[80];
            memset(&dd, 0, sizeof dd);
            dd.buf = dot_buf; dd.count = (uint32_t)ns[k]; dd.dot_size = 4; dd.edge = YGFX_EDGE_COSINE; dd.edge_width = 1;
            dd.color[0] = dd.color[1] = dd.color[2] = 0.6f;
            one = ygfx_dots(&dd);
            dot_n = ns[k];
            snprintf(name, sizeof name, "dots x %d, 4 px, upload every frame", ns[k]);
            bench(name, &one, 1, prep_dots, 1);
        }
        ygfx_buffer_free(&gfx, dot_buf);
        free(dot_xy);
    }

    /* noise */
    {
        ygfx_noise_desc nd;
        memset(&nd, 0, sizeof nd);
        nd.w = (float)W; nd.h = (float)H; nd.check = 1; nd.contrast = 0.2f; nd.aperture = YGFX_NO_APERTURE;
        one = ygfx_noise(&nd);
        bench("noise, full screen, GPU hash", &one, 1, NULL, 0);
    }
    {
        ygfx_texture_desc td;
        ygfx_image_desc idd;
        float* nb = (float*)malloc((size_t)W * H * sizeof(float));
        uint64_t t0 = yrt_now_ns();
        ygfx_noise_fill(nb, W, H, 7, YGFX_UNIFORM);
        if (run("noise")) {
            char name[80];
            snprintf(name, sizeof name, "noise_fill on the CPU, %dx%d (once)", W, H);
            printf("| %-52s | %9.1f | %9s | %9s |\n", name, (double)(yrt_now_ns() - t0) * 1e-3, "", "");
        }
        memset(&td, 0, sizeof td);
        td.w = W; td.h = H; td.format = YGFX_R32F;
        up_tex = ygfx_texture(&gfx, &td);
        up_data = nb;
        memset(&idd, 0, sizeof idd);
        idd.tex = up_tex; idd.modulation = true; idd.contrast = 0.2f;
        one = ygfx_image(&gfx, &idd);
        bench("noise, full screen, CPU R32F upload + draw", &one, 1, prep_upload, 1);
        ygfx_texture_free(&gfx, up_tex);
        free(nb);
    }

    /* a video-sized frame */
    {
        ygfx_texture_desc td;
        ygfx_image_desc idd;
        unsigned char* px = (unsigned char*)malloc((size_t)W * H * 4);
        for (i = 0; i < W * H * 4; i++) px[i] = (unsigned char)(i * 31);
        memset(&td, 0, sizeof td);
        td.w = W; td.h = H; td.format = YGFX_RGBA8;
        up_tex = ygfx_texture(&gfx, &td);
        up_data = px;
        memset(&idd, 0, sizeof idd);
        idd.tex = up_tex;
        one = ygfx_image(&gfx, &idd);
        bench("image RGBA8, full frame: upload + draw", &one, 1, prep_upload, 1);
        if (run("raw")) raw_pbo();
        ygfx_texture_free(&gfx, up_tex);
        free(px);
    }
    if (run("v0.2")) bench_v02(g);
    if (run("v0.3")) bench_v03();
    bench_v04_video(); bench_v04_inst(); bench_v04_mixed();   /* each checks --only */
    bench_v06_text(); bench_v06_blur(); bench_v07_noise(); bench_v10_simplex();
    ygfx_close(&gfx);
    yscr_close(&scr);
    return 0;
}
