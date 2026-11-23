/* fence_probe.c - does a second GL context that shares the preview's
 * texture see the whole frame that the first context drew into it, with no
 * sync, after glFlush(), or with a fence (docs/designer.md, "Preview size
 * (2026-11-23)", part 2). Not a test: it prints counts. Linux only: EGL's
 * device platform, two pbuffers, no window.
 *
 * Build:
 *   cc -O2 -o fence_probe tests/probe/designer_viewports/fence_probe.c -ldl -lGLESv2
 * Usage: fence_probe DEVICE MODE ROUNDS [W H PASSES]
 *   DEVICE: a substring of GL_RENDERER (llvmpipe, AMD, Intel)
 *   MODE:   none   context A draws, B samples at once (the designer's
 *                  order with ImGui's viewports: the main viewport renders,
 *                  then RenderPlatformWindowsDefault() makes B current)
 *           flush  glFlush() on A after its draw
 *           fence  glFenceSync() and glFlush() on A, glWaitSync() on B
 *           finish glFinish() on A (the reference)
 *   W x H: the texture, as the preview's output texture (default 1920 x
 *   1080). PASSES: full-screen passes that A draws each round, each in a
 *   fragment shader that loops, so the draw takes long enough for a race to
 *   show (default 8).
 * Each round A draws the round's color in PASSES tiled passes (the last
 * pass decides every px); B draws the texture into its own framebuffer
 * with a nearest sampler and reads 64 px spread over it. A round is bad
 * when any px is not the round's color. It prints the bad rounds and the
 * median and max of A's draw (CPU, to the end of the calls) and of B's
 * draw and read.
 */
#define _POSIX_C_SOURCE 200809L
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (*fn)(void);
static fn (*gpa)(const char*);
static void* sym(void* lib, const char* n) {
    void* p = dlsym(lib, n);
    if (!p && gpa) {
        fn f = gpa(n);
        memcpy(&p, &f, sizeof p);
    }
    return p;
}
#define LOAD(var, lib, name) do { void* p_ = sym(lib, name); memcpy(&(var), &p_, sizeof p_); } while (0)

static double ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}
static int cmpd(const void* a, const void* b) {
    const double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static GLuint program(const char* vs, const char* fs) {
    GLuint p = glCreateProgram(), s[2];
    const char* src[2] = { vs, fs };
    const GLenum kind[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
    for (int i = 0; i < 2; i++) {
        GLint ok = 0;
        s[i] = glCreateShader(kind[i]);
        glShaderSource(s[i], 1, &src[i], NULL);
        glCompileShader(s[i]);
        glGetShaderiv(s[i], GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog(s[i], sizeof log, NULL, log);
            fprintf(stderr, "shader: %s\n", log);
            exit(1);
        }
        glAttachShader(p, s[i]);
    }
    glLinkProgram(p);
    return p;
}

static const char* kVS = "#version 300 es\n"
                         "uniform vec4 rect;\n"
                         "out vec2 uv;\n"
                         "void main() {\n"
                         "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
                         "  uv = c;\n"
                         "  gl_Position = vec4(mix(rect.xy, rect.zw, c), 0.0, 1.0);\n"
                         "}\n";
/* A loop the compiler cannot drop: its result is multiplied by 0 at run time. */
static const char* kFS_A = "#version 300 es\n"
                           "precision highp float;\n"
                           "uniform vec4 color; uniform int work; uniform float zero;\n"
                           "in vec2 uv; out vec4 o;\n"
                           "void main() {\n"
                           "  float a = uv.x;\n"
                           "  for (int i = 0; i < work; i++) a = fract(sin(a * 12.9898 + float(i)) * 43758.5453);\n"
                           "  o = color + vec4(a * zero);\n"
                           "}\n";
static const char* kFS_B = "#version 300 es\n"
                           "precision highp float;\n"
                           "uniform sampler2D t;\n"
                           "in vec2 uv; out vec4 o;\n"
                           "void main() { o = texture(t, uv); }\n";

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: fence_probe DEVICE none|flush|fence|finish ROUNDS [W H PASSES]\n");
        return 2;
    }
    const char* want = argv[1];
    const char* mode = argv[2];
    const int rounds = atoi(argv[3]);
    const int W = argc > 5 ? atoi(argv[4]) : 1920, H = argc > 5 ? atoi(argv[5]) : 1080;
    const int passes = argc > 6 ? atoi(argv[6]) : 8;
    void* lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return 1;
    LOAD(gpa, lib, "eglGetProcAddress");
    unsigned (*query_devices)(int32_t, void**, int32_t*);
    void* (*gpd)(unsigned, void*, const int32_t*);
    unsigned (*init)(void*, int32_t*, int32_t*);
    unsigned (*bind_api)(unsigned);
    unsigned (*choose)(void*, const int32_t*, void**, int32_t, int32_t*);
    void* (*mkctx)(void*, void*, void*, const int32_t*);
    void* (*mkpb)(void*, void*, const int32_t*);
    unsigned (*cur)(void*, void*, void*, void*);
    LOAD(query_devices, lib, "eglQueryDevicesEXT");
    LOAD(gpd, lib, "eglGetPlatformDisplayEXT");
    LOAD(init, lib, "eglInitialize");
    LOAD(bind_api, lib, "eglBindAPI");
    LOAD(choose, lib, "eglChooseConfig");
    LOAD(mkctx, lib, "eglCreateContext");
    LOAD(mkpb, lib, "eglCreatePbufferSurface");
    LOAD(cur, lib, "eglMakeCurrent");
    void* devs[16];
    int32_t nd = 0;
    if (!query_devices || !query_devices(16, devs, &nd)) return 1;
    void *dpy = NULL, *ca = NULL, *cb = NULL, *pa = NULL, *pb = NULL;
    for (int32_t i = 0; i < nd && !cb; i++) {
        int32_t mj, mn, n = 0;
        void* cfg = NULL;
        dpy = gpd(0x313F, devs[i], NULL);
        if (!dpy || !init(dpy, &mj, &mn)) continue;
        bind_api(0x30A0);
        const int32_t at[] = { 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3033, 0x0001, 0x3040, 0x0040, 0x3038 };
        if (!choose(dpy, at, &cfg, 1, &n) || n < 1) continue;
        const int32_t xa[] = { 0x3098, 3, 0x30FB, 0, 0x3038 };
        const int32_t pba[] = { 0x3057, W, 0x3056, H, 0x3038 };
        ca = mkctx(dpy, cfg, NULL, xa);
        pa = ca ? mkpb(dpy, cfg, pba) : NULL;
        if (!pa || !cur(dpy, pa, pa, ca)) continue;
        const char* r = (const char*)glGetString(GL_RENDERER);
        if (!r || !strstr(r, want)) continue;
        printf("GL_RENDERER: %s; %s, %d rounds, %d x %d, %d passes\n", r, mode, rounds, W, H, passes);
        cb = mkctx(dpy, cfg, ca, xa);   /* shares A's objects, as SDL_GL_SHARE_WITH_CURRENT_CONTEXT */
        pb = cb ? mkpb(dpy, cfg, pba) : NULL;
    }
    if (!cb || !pb) {
        fprintf(stderr, "no such device, or no shared context\n");
        return 77;
    }
    /* A: the texture (the gfx output texture) and its framebuffer */
    cur(dpy, pa, pa, ca);
    GLuint tex, fbo, vao_a;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 1;
    glGenVertexArrays(1, &vao_a);
    const GLuint pA = program(kVS, kFS_A);
    const GLint u_rect_a = glGetUniformLocation(pA, "rect"), u_col = glGetUniformLocation(pA, "color");
    const GLint u_work = glGetUniformLocation(pA, "work"), u_zero = glGetUniformLocation(pA, "zero");
    glFinish();
    /* B: its own program and VAO (VAOs are not shared) */
    cur(dpy, pb, pb, cb);
    GLuint vao_b;
    glGenVertexArrays(1, &vao_b);
    const GLuint pB = program(kVS, kFS_B);
    const GLint u_rect_b = glGetUniformLocation(pB, "rect");
    glFinish();

    int bad = 0, first_bad = -1, stale = 0;
    double* ta = (double*)calloc((size_t)rounds, sizeof *ta);
    double* tb = (double*)calloc((size_t)rounds, sizeof *tb);
    const int tiles = 8;
    for (int r = 0; r < rounds; r++) {
        const unsigned char want_c[4] = { (unsigned char)(r & 255), (unsigned char)((r >> 8) & 255), (unsigned char)(37 + r % 200), 255 };
        const unsigned char old_c[4] = { (unsigned char)((r - 1) & 255), (unsigned char)(((r - 1) >> 8) & 255), (unsigned char)(37 + (r + 199) % 200), 255 };
        double t0 = ms();
        cur(dpy, pa, pa, ca);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, W, H);
        glUseProgram(pA);
        glBindVertexArray(vao_a);
        glUniform1i(u_work, 64);
        glUniform1f(u_zero, 0.0f);
        /* passes of tiles, row by row: a half-drawn texture shows old tiles */
        for (int p = 0; p < passes; p++) {
            const float k = p == passes - 1 ? 1.0f : 0.5f;
            glUniform4f(u_col, want_c[0] / 255.0f * k, want_c[1] / 255.0f * k, want_c[2] / 255.0f * k, 1.0f);
            for (int ty = 0; ty < tiles; ty++)
                for (int tx = 0; tx < tiles; tx++) {
                    const float x0 = -1.0f + 2.0f * tx / tiles, y0 = -1.0f + 2.0f * ty / tiles;
                    glUniform4f(u_rect_a, x0, y0, x0 + 2.0f / tiles, y0 + 2.0f / tiles);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                }
        }
        GLsync f = NULL;
        if (!strcmp(mode, "flush")) glFlush();
        else if (!strcmp(mode, "fence")) {
            f = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFlush();
        } else if (!strcmp(mode, "finish")) glFinish();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        double t1 = ms();
        cur(dpy, pb, pb, cb);
        if (f) glWaitSync(f, 0, GL_TIMEOUT_IGNORED);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, W, H);
        glUseProgram(pB);
        glBindVertexArray(vao_b);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);   /* bound again in B, as ImGui binds it per draw */
        glUniform4f(u_rect_b, -1, -1, 1, 1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        int round_bad = 0, round_stale = 0;
        for (int s = 0; s < 64; s++) {
            unsigned char px[4];
            const int x = (s % 8) * (W / 8) + W / 16, y = (s / 8) * (H / 8) + H / 16;
            glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            if (memcmp(px, want_c, 4) != 0) {
                round_bad = 1;
                round_stale += memcmp(px, old_c, 4) == 0;
            }
        }
        double t2 = ms();
        if (f) {
            cur(dpy, pa, pa, ca);
            glDeleteSync(f);
        }
        ta[r] = t1 - t0;
        tb[r] = t2 - t1;
        if (round_bad) {
            bad++;
            if (first_bad < 0) first_bad = r;
            stale += round_stale > 0;
        }
    }
    qsort(ta, (size_t)rounds, sizeof *ta, cmpd);
    qsort(tb, (size_t)rounds, sizeof *tb, cmpd);
    printf("bad rounds: %d of %d (with the last round's color in some px: %d; the first bad: %d)\n", bad, rounds, stale, first_bad);
    printf("A's draw: median %.3f ms, max %.3f ms; B's draw and read: median %.3f ms, max %.3f ms\n", ta[rounds / 2], ta[rounds - 1],
           tb[rounds / 2], tb[rounds - 1]);
    return 0;
}
