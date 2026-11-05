/* screen_metal_test.c - ysp/screen.h's METAL backend on a real window:
 * self-checking, macOS only, needs a window server session and ANGLE.
 *
 * The core test (screen_test.c) checks the Metal presenter's rules without
 * a Mac. This program runs the presenter itself:
 *
 *   sync     an 800 x 600 window, 120 frames, each cleared by GL to its own
 *            value and covered by a fragment shader that loads the GPU; the
 *            header's command buffer reads the center pixel after its wait
 *            on ANGLE's shared event (the YSCR__MTL_SYNC_PIXEL seam below),
 *            and every pixel read must be its own frame's. Without the wait,
 *            2 to 5 of 120 frames were read unfinished under such a load
 *            (docs/screen_macos_probe.md). Also: one pbuffer per drawable
 *            IOSurface, kept (the describe line's count).
 *   gfx      ysp/gfx.h on a METAL screen writes rows top-down (GL's row 0
 *            is the top of the drawable, as on Windows).
 *   tenbit   the same as sync at desc.color_depth 10 (a BGR10A2Unorm layer,
 *            'l10r' IOSurfaces), 30 frames.
 *   codes    fullscreen with a SOLID and a ROW code read back every frame:
 *            the self test passes and no code differs.
 *   records  every flip record is tier 3 (no window kind claims more until
 *            measured); with the screen unlocked, most flips are shown,
 *            with onsets that rise. With the screen locked, every frame is
 *            OCCLUDED and estimated, and the test says that the presents
 *            were not checked.
 *
 * It prints one line per check and returns 0 when every check passed, 1
 * otherwise. A window or a fullscreen display shows for about 15 s.
 *
 *     test_screen_metal [ANGLE folder]      (or YSCR_ANGLE_DIR)
 *
 * CMake builds it on macOS as test_screen_metal and does not run it: CI
 * has no ANGLE. tests/mutate/screen.toml's Metal mutants run it.
 */
#include <stdint.h>
static void sync_pixel(uint64_t id, uint32_t px);
#define YSCR__MTL_SYNC_PIXEL(id, px) sync_pixel((id), (px))
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"   /* the gfx case: its origin follows the backend */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(YSCR__METAL)
static void sync_pixel(uint64_t id, uint32_t px) { (void)id; (void)px; }
int main(void) { printf("screen_metal_test: macOS only (the METAL backend is)\n"); return 0; }
#else

static int g_fail, g_checks;
static void check(int ok, const char* what) {
    g_checks++;
    if (!ok) g_fail++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

/* The pixels the header's command buffer read: present id and value. */
static uint64_t g_px_id[4096];
static uint32_t g_px[4096];
static int g_npx, g_bits10;
static void sync_pixel(uint64_t id, uint32_t px) {
    if (g_npx < 4096) { g_px_id[g_npx] = id; g_px[g_npx] = px; g_npx++; }
}

/* GL through yscr_gl_proc: a clear and a shader whose dependent loop takes
 * a few milliseconds of GPU time per frame and writes the cleared value. */
typedef void (*fn_clearcolor)(float, float, float, float);
typedef void (*fn_clear)(unsigned);
typedef unsigned (*fn_createshader)(unsigned);
typedef void (*fn_shadersource)(unsigned, int, const char* const*, const int*);
typedef void (*fn_u)(unsigned);
typedef unsigned (*fn_createprogram)(void);
typedef void (*fn_uu)(unsigned, unsigned);
typedef int (*fn_uniformloc)(unsigned, const char*);
typedef void (*fn_uniform3f)(int, float, float, float);
typedef void (*fn_drawarrays)(unsigned, int, int);
typedef void (*fn_genva)(int, unsigned*);
typedef void (*fn_viewport)(int, int, int, int);

static struct {
    fn_clearcolor ClearColor; fn_clear Clear; fn_createshader CreateShader; fn_shadersource ShaderSource;
    fn_u CompileShader, LinkProgram, UseProgram, BindVertexArray; fn_createprogram CreateProgram;
    fn_uu AttachShader; fn_uniformloc GetUniformLocation; fn_uniform3f Uniform3f; fn_drawarrays DrawArrays;
    fn_genva GenVertexArrays; fn_viewport Viewport;
    unsigned prog;
    int loc;
} G;

#define LOAD(f, n) do { yscr_proc p_ = yscr_gl_proc(s, n); memcpy((void*)&G.f, &p_, sizeof p_); } while (0)
static int gl_setup(yscr_screen* s) {
    static const char* vs =
        "#version 300 es\n"
        "void main() { vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);\n"
        "  gl_Position = vec4(p, 0.0, 1.0); }\n";
    static const char* fs =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform vec3 u_c;\n"
        "out vec4 o;\n"
        "void main() {\n"
        "  float x = gl_FragCoord.x * 1e-4;\n"
        "  for (int i = 0; i < 400; i++) x = fract(sin(x) * 43758.5453 + 0.1);\n"
        "  o = vec4(u_c + vec3(x * 1e-7), 1.0);\n"
        "}\n";
    unsigned v, f, va = 0;
    LOAD(ClearColor, "glClearColor"); LOAD(Clear, "glClear"); LOAD(CreateShader, "glCreateShader");
    LOAD(ShaderSource, "glShaderSource"); LOAD(CompileShader, "glCompileShader"); LOAD(LinkProgram, "glLinkProgram");
    LOAD(UseProgram, "glUseProgram"); LOAD(BindVertexArray, "glBindVertexArray"); LOAD(CreateProgram, "glCreateProgram");
    LOAD(AttachShader, "glAttachShader"); LOAD(GetUniformLocation, "glGetUniformLocation"); LOAD(Uniform3f, "glUniform3f");
    LOAD(DrawArrays, "glDrawArrays"); LOAD(GenVertexArrays, "glGenVertexArrays"); LOAD(Viewport, "glViewport");
    if (!G.Clear || !G.CreateShader || !G.DrawArrays || !G.GenVertexArrays) return 0;
    v = G.CreateShader(0x8B31);
    G.ShaderSource(v, 1, &vs, NULL);
    G.CompileShader(v);
    f = G.CreateShader(0x8B30);
    G.ShaderSource(f, 1, &fs, NULL);
    G.CompileShader(f);
    G.prog = G.CreateProgram();
    G.AttachShader(G.prog, v);
    G.AttachShader(G.prog, f);
    G.LinkProgram(G.prog);
    G.loc = G.GetUniformLocation(G.prog, "u_c");
    G.GenVertexArrays(1, &va);
    G.BindVertexArray(va);
    return 1;
}

/* Frame i's value: 0xA5 in blue marks the test's own frames; red and green
 * are the frame number. */
static uint32_t frame_code(int i) { return (uint32_t)(i & 0xFF) | ((uint32_t)((i >> 8) & 0xFF) << 8) | 0xA50000u; }

static void draw(int i, int w, int h) {
    uint32_t c = frame_code(i);
    float r = (float)(c & 0xFF) / 255.0f, g = (float)((c >> 8) & 0xFF) / 255.0f, b = (float)((c >> 16) & 0xFF) / 255.0f;
    G.Viewport(0, 0, w, h);
    G.ClearColor(r, g, b, 1.0f);
    G.Clear(0x4000);
    G.UseProgram(G.prog);
    G.Uniform3f(G.loc, r, g, b);
    G.DrawArrays(0x0004, 0, 3);
}

static int is_locked(const yscr_screen* s) {
    char line[1024];
    yscr_describe(s, line, sizeof line);
    return strstr(line, "locked=1") != NULL;
}

/* Runs n frames and checks the pixels and the records. */
static void run(const char* name, const yscr_desc* d0, int n, int heavy) {
    static yscr_screen s;
    static yscr_frame f;
    static yrt_event ev[4096];
    static unsigned char ring_mem[YRT_RING_BYTES(4096) + 64];
    yrt_ring ring;
    yrt_ring_desc rd;
    yscr_desc d = *d0;
    yscr_caps caps;
    uint64_t base;
    int i, k, bad = 0, read = 0, tier_bad = 0, shown = 0, flips = 0, locked, rising = 1;
    int64_t last_onset = 0;
    char msg[256], line[1024];
    memset(&rd, 0, sizeof rd);
    rd.memory = ring_mem;
    rd.bytes = sizeof ring_mem;
    if (!yrt_ring_open(&ring, &rd)) { check(0, "ring"); return; }
    d.ring = &ring;
    memset(&s, 0, sizeof s);
    g_npx = 0;
    g_bits10 = d.color_depth == 10;
    if (!yscr_open(&s, &d)) {
        snprintf(msg, sizeof msg, "%s: open: %s", name, yscr_error(&s));
        check(0, msg);
        yrt_ring_close(&ring);
        return;
    }
    yscr_describe(&s, line, sizeof line);
    printf("     %s\n", line);
    locked = is_locked(&s);
    yscr_get_caps(&s, &caps);
    if (heavy && !gl_setup(&s)) check(0, "GL entry points");
    base = s.next_id + 1;
    for (i = 0; i < n; i++) {
        if (yscr_begin(&s, &f) != YSCR_OK) { check(0, "begin"); break; }
        if (heavy) draw(i, caps.mode.w, caps.mode.h);
        if (yscr_flip(&s) != YSCR_OK) { check(0, "flip"); break; }
    }
    yscr_wait_flip(&s, NULL);
    {   /* one pbuffer per drawable IOSurface: the layer cycles 3 or 4 */
        const char* pb;
        unsigned made = 0;
        yscr_describe(&s, line, sizeof line);
        pb = strstr(line, "pbuffers=");
        if (pb) made = (unsigned)strtoul(pb + 9, NULL, 10);
        snprintf(msg, sizeof msg, "%s: %u pbuffers made for %d frames and the settling", name, made, n);
        check(pb && made >= 1 && made <= 6, msg);
    }
    yscr_close(&s);
    for (k = 0; k < g_npx; k++) {
        uint32_t code;
        int want;
        if (g_px_id[k] < base || g_px_id[k] >= base + (uint64_t)n) continue;
        want = (int)(g_px_id[k] - base);
        code = yscr__mtl_code_of(g_px[k], g_bits10);
        read++;
        if (heavy && code != frame_code(want)) {
            if (bad < 5) printf("     frame %d: read 0x%06x, drew 0x%06x\n", want, code, frame_code(want));
            bad++;
        }
    }
    {
        int m = yrt_ring_drain(&ring, ev, 4096);
        for (k = 0; k < m; k++) {
            uint32_t flags, tier;
            if (ev[k].source != YRT_SRC_SCREEN || ev[k].kind != YSCR_EV_FLIP) continue;
            flags = YSCR_EV_FLAGS_OF(ev[k].u.u16[5]);
            tier = YSCR_EV_TIER_OF(ev[k].u.u16[5]);
            flips++;
            if (tier != YSCR_TIER_3) tier_bad++;
            if (!(flags & YSCR_FLIP_ESTIMATED)) {
                shown++;
                if ((int64_t)ev[k].t_ns <= last_onset) rising = 0;
                last_onset = (int64_t)ev[k].t_ns;
            } else if (locked && !(flags & YSCR_FLIP_OCCLUDED)) {
                tier_bad++;   /* a locked session's frame must say OCCLUDED */
            }
        }
    }
    yrt_ring_close(&ring);
    if (heavy) {
        snprintf(msg, sizeof msg, "%s: %d of %d frames read back after the wait, %d not their own", name, read, n, bad);
        check(read == n && bad == 0, msg);
    }
    snprintf(msg, sizeof msg, "%s: %d flip records, all tier 3%s", name, flips,
             locked ? ", all OCCLUDED or shown (screen locked)" : "");
    check(flips >= n && tier_bad == 0, msg);
    if (locked) {
        printf("     %s: the screen is locked: presents were not shown, so presentedTime was not checked\n", name);
    } else {
        snprintf(msg, sizeof msg, "%s: %d of %d flips shown, onsets rise", name, shown, flips);
        check(shown >= flips * 3 / 4 && rising, msg);
    }
}

static void gfx_origin(const yscr_desc* d) {
    static yscr_screen s;
    static ygfx_gfx g;
    ygfx_desc gd;
    char line[1024];
    memset(&s, 0, sizeof s);
    if (!yscr_open(&s, d)) { check(0, "gfx: screen open"); return; }
    memset(&g, 0, sizeof g);
    memset(&gd, 0, sizeof gd);
    gd.screen = &s;
    if (!ygfx_open(&g, &gd)) {
        snprintf(line, sizeof line, "gfx: open: %s", ygfx_error(&g));
        check(0, line);
    } else {
        ygfx_describe(&g, line, sizeof line);
        check(strstr(line, "origin top") != NULL, "gfx: origin top on METAL");
        ygfx_close(&g);
    }
    yscr_close(&s);
}

int main(int argc, char** argv) {
    yscr_desc d;
    const char* angle = argc > 1 ? argv[1] : getenv("YSCR_ANGLE_DIR");
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_METAL;
    d.angle_dir = angle;
    d.windowed = true;
    d.settle = YSCR_SETTLE_WARN;
    run("sync", &d, 120, 1);
    gfx_origin(&d);
    d.color_depth = 10;
    run("tenbit", &d, 30, 1);
    d.color_depth = 0;
    {   /* codes: fullscreen, read back every frame */
        static yscr_screen s;
        static yscr_frame f;
        static uint32_t row[16];
        uint32_t checked = 0, failed = 0;
        int i;
        char msg[160];
        d.windowed = false;
        d.n_codes = 2;
        d.codes[0].kind = YSCR_CODE_SOLID;
        d.codes[0].x = 100; d.codes[0].y = 100; d.codes[0].w = 8; d.codes[0].h = 8; d.codes[0].rest = 0x102030u;
        d.codes[1].kind = YSCR_CODE_ROW;
        d.codes[1].x = 200; d.codes[1].y = 50; d.codes[1].w = 16; d.codes[1].h = 1; d.codes[1].rest = 0;
        d.verify_codes = 1;
        memset(&s, 0, sizeof s);
        if (!yscr_open(&s, &d)) {
            snprintf(msg, sizeof msg, "codes: open: %s", yscr_error(&s));
            check(0, msg);
        } else {
            for (i = 0; i < 60; i++) {
                int k;
                for (k = 0; k < 16; k++) row[k] = (uint32_t)((i * 16 + k) * 0x010203u) & 0xFFFFFFu;
                yscr_code(&s, 0, (uint32_t)(i * 0x030507u) & 0xFFFFFFu);
                yscr_code_row(&s, 1, row, 16, 1);
                if (yscr_begin(&s, &f) != YSCR_OK || yscr_flip(&s) != YSCR_OK) { check(0, "codes: frame"); break; }
            }
            yscr_wait_flip(&s, NULL);
            check(!(yscr_code_risk(&s) & YSCR_CODE_RISK_UNVERIFIED), "codes: the self test passed");
            yscr_code_verify(&s, &checked, &failed);
            yscr_close(&s);
            snprintf(msg, sizeof msg, "codes: %u codes read back, %u differed", checked, failed);
            check(checked >= 60 && failed == 0, msg);
        }
    }
    printf("screen_metal_test: %d of %d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
#endif
