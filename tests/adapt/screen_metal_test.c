/* screen_metal_test.c - ysp/screen.h's METAL backend on a real window:
 * self-checking, macOS only, needs a window server session and ANGLE.
 *
 * The core test (screen_test.c) checks the Metal presenter's rules without
 * a Mac. This program runs the presenter itself:
 *
 *   rate     an 800 x 600 window, 120 frames, each cleared by GL to its own
 *            value and covered by a fragment shader of about 4.5 ms of GPU
 *            time; the
 *            header's command buffer reads the center pixel after its wait
 *            on ANGLE's shared event (the YSCR__MTL_SYNC_PIXEL seam below),
 *            and every pixel read must be its own frame's. Without the wait,
 *            2 to 5 of 120 frames were read unfinished under such a load
 *            (docs/screen_macos_probe.md). Also: one pbuffer per drawable
 *            IOSurface, kept (the describe line's count).
 *   sync     the same in a minimized window: no frame is shown, so nothing
 *            paces the frames, and a missing wait reads frames ANGLE has
 *            not drawn (on a shown screen it went unseen in 180 frames).
 *   vrr      open() refuses a display in a variable refresh mode (the two
 *            NSScreen refresh intervals apart, injected through the
 *            YSCR__MTL_SCREEN_INTERVALS seam: no ProMotion or variable-mode
 *            display was attached) and says how to set a fixed rate; it
 *            opens with equal intervals, and the real screen's are equal.
 *   period   the period comes from CADisplayLink's timestamps (injected
 *            through YSCR__MTL_LINK_TIMESTAMPS: 16680375 ns, not the
 *            duration's 16680417), from CVDisplayLink when no callback
 *            came or its steps are off the duration, and the describe line
 *            names the source and why; a first timestamp off the grid (a
 *            process's first windowed open, measured) is left out. The
 *            real read is printed (a display asleep gives no callback:
 *            then the fallback), and the process's first open, on an
 *            unlocked screen, must name cadisplaylink.
 *   fullscreen  rate's check in a fullscreen space at 1920 x 1080, with
 *            the shader's load scaled to the pixels: one frame a vblank
 *            at depth 2, where the probe's layer queued one drawable fewer
 *            than in a window.
 *   gfx      ysp/gfx.h on a METAL screen writes rows top-down (GL's row 0
 *            is the top of the drawable, as on Windows).
 *   tenbit   the same as sync at desc.color_depth 10 (a BGR10A2Unorm layer,
 *            'l10r' IOSurfaces), 30 frames.
 *   codes    fullscreen with a SOLID and a ROW code read back every frame:
 *            the self test passes and no code differs.
 *   records  a shown flip is tier 2 (presentedTime) and an estimated one
 *            tier 3; with the screen unlocked, most flips are shown, with
 *            onsets that rise, nine in ten a vblank apart (two frames in
 *            flight, v0.12.0) and at most one in 50 dropped. With the screen locked, every frame is
 *            OCCLUDED and estimated, and the test says that the presents
 *            were not checked.
 *
 *   state    (v0.16.0) the code risk's checks ran (no STATE_UNKNOWN), SCALED
 *            follows the mode against the panel's native pixels, the
 *            layer is untagged, and an EDR display (a seam) sets
 *            ADVANCED_COLOR.
 *   gamma    (v0.16.0) a transfer table one entry off the identity is set
 *            to it at a fullscreen open, read back, and put back at close;
 *            a refused set (a seam) and keep_os_gamma leave it, with
 *            CODE_RISK_GAMMA. ColorSync's table at the end.
 *   rawmice  (v0.16.0) desc.raw_mice opens and logs the reader's mice;
 *            YSCR_TEST_MOUSE_S=n: n s of a person moving the mouse, for the
 *            report interval and the stamp against arrival.
 *   panic    (v0.16.0) a child hangs with desc.panic and gets 3 presses
 *            through the tap's function: exit 99 and panic_fn; a child that
 *            pumps outside begin() is released and lives; presses for
 *            another process do not count. panic-post (not in the full run):
 *            the presses posted to the HID tap (Accessibility, unlocked).
 *            These four run alone with "test_screen_metal <case>" or
 *            YSCR_TEST_CASES=<case>, or all with "new".
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
/* The refresh intervals open() reads from the window's NSScreen, replaced
 * while g_iv_min is not 0, as a ProMotion panel in its variable mode would
 * give them (no such panel was attached). */
static long long g_iv_min, g_iv_max;
/* CADisplayLink's callbacks at open, replaced while g_link is 1 (six
 * timestamps a 16680375 ns step apart, duration 16.680417 ms, as measured
 * on the M27Q), 2 (none: the fallback to CVDisplayLink), 3 (as a process's
 * first windowed open: the first timestamp 15.8355 ms before the second,
 * measured) or 4 (a last step of 25 ms, off the grid). */
static int g_link;
#define YSCR__MTL_LINK_TIMESTAMPS(ts, n, dur) \
    do { if (g_link == 1 || g_link == 3 || g_link == 4) { int j_; \
             for (j_ = 0; j_ < 6; j_++) (ts)[j_] = 5000000000000LL + j_ * 16680375LL; \
             if (g_link == 3) (ts)[0] = (ts)[1] - 15835500LL; \
             if (g_link == 4) (ts)[5] = (ts)[4] + 25000000LL; \
             *(n) = 6; *(dur) = 0.016680417; } else if (g_link == 2) *(n) = 0; } while (0)
#define YSCR__MTL_SCREEN_INTERVALS(m, mn, mx) \
    do { (void)(m); if (g_iv_min) { *(mn) = g_iv_min; *(mx) = g_iv_max; } } while (0)
/* v0.16.0: the panic armed in a window (as examples/screen/abort.c on
 * Windows), a display that refuses a gamma table, an EDR-capable display
 * (none attached), and each raw mouse value's stamp against its arrival. */
static volatile int g_panic_windowed;
#define YSCR__PANIC_WINDOWED g_panic_windowed
static int g_gamma_refuse, g_hdr;
#define YSCR__MAC_GAMMA_SET(d, n, r, g, b) (g_gamma_refuse ? 1001 : yscr__mx.CGSetDisplayTransferByTable((d), (n), (r), (g), (b)))
#define YSCR__MAC_COLOR(c) do { if (g_hdr) (c)->edr_potential = 16.0; } while (0)
static void rm_arrival(int64_t t);
#define YSCR__RM_ARRIVAL(t) rm_arrival(t)
/* v0.16.1: every raw mouse value, to log the ones more than 1 s old */
static void rm_value(uint32_t page, uint32_t usage, int64_t v, int64_t t, int stale);
#define YSCR__RM_VALUE(page, usage, v, t, stale) rm_value((page), (usage), (v), (t), (stale))
/* An older macOS than 14 while g_old_os is 1 (no such system was at hand). */
static int g_old_os;
#define YSCR__MTL_OS_14(ok) do { if (g_old_os) *(ok) = 0; } while (0)
#define YSP_SCREEN_IMPLEMENTATION
#include "ysp/screen.h"
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"   /* the gfx case: its origin follows the backend */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(YSCR__METAL)
static void sync_pixel(uint64_t id, uint32_t px) { (void)id; (void)px; }
static void rm_arrival(int64_t t) { (void)t; }
static void rm_value(uint32_t page, uint32_t usage, int64_t v, int64_t t, int stale) {
    (void)page; (void)usage; (void)v; (void)t; (void)stale;
}
int main(void) { printf("screen_metal_test: macOS only (the METAL backend is)\n"); return 0; }
#else

#include <math.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <sys/event.h>
#include <sys/wait.h>

static int g_fail, g_checks, g_opened;
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
    void (*Uniform1i)(int, int);
    fn_genva GenVertexArrays; fn_viewport Viewport;
    unsigned prog;
    int loc, loc_n;
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
        "uniform int u_n;\n"
        "out vec4 o;\n"
        "void main() {\n"
        "  float x = gl_FragCoord.x * 1e-4;\n"
        "  for (int i = 0; i < u_n; i++) x = fract(sin(x) * 43758.5453 + 0.1);\n"
        "  o = vec4(u_c + vec3(x * 1e-7), 1.0);\n"
        "}\n";
    unsigned v, f, va = 0;
    LOAD(ClearColor, "glClearColor"); LOAD(Clear, "glClear"); LOAD(CreateShader, "glCreateShader");
    LOAD(ShaderSource, "glShaderSource"); LOAD(CompileShader, "glCompileShader"); LOAD(LinkProgram, "glLinkProgram");
    LOAD(UseProgram, "glUseProgram"); LOAD(BindVertexArray, "glBindVertexArray"); LOAD(CreateProgram, "glCreateProgram");
    LOAD(AttachShader, "glAttachShader"); LOAD(GetUniformLocation, "glGetUniformLocation"); LOAD(Uniform3f, "glUniform3f");
    LOAD(DrawArrays, "glDrawArrays"); LOAD(Uniform1i, "glUniform1i"); LOAD(GenVertexArrays, "glGenVertexArrays"); LOAD(Viewport, "glViewport");
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
    G.loc_n = G.GetUniformLocation(G.prog, "u_n");
    G.GenVertexArrays(1, &va);
    G.BindVertexArray(va);
    return 1;
}

/* Frame i's value: 0xA5 in blue marks the test's own frames; red and green
 * are the frame number. */
static uint32_t frame_code(int i) { return (uint32_t)(i & 0xFF) | ((uint32_t)((i >> 8) & 0xFF) << 8) | 0xA50000u; }

static void draw(int i, int w, int h, int iters) {
    uint32_t c = frame_code(i);
    float r = (float)(c & 0xFF) / 255.0f, g = (float)((c >> 8) & 0xFF) / 255.0f, b = (float)((c >> 16) & 0xFF) / 255.0f;
    G.Viewport(0, 0, w, h);
    G.ClearColor(r, g, b, 1.0f);
    G.Clear(0x4000);
    G.UseProgram(G.prog);
    G.Uniform3f(G.loc, r, g, b);
    G.Uniform1i(G.loc_n, iters);
    G.DrawArrays(0x0004, 0, 3);
}

static int is_locked(const yscr_screen* s) {
    char line[1024];
    yscr_describe(s, line, sizeof line);
    return strstr(line, "locked=1") != NULL;
}

/* Runs n frames and checks the pixels and the records. */
/* heavy: the shader's iterations per pixel (0: no GL draw); rate: check one
 * frame a vblank (a load the GPU finishes in a frame); hide: minimize the
 * window first, so no frame is shown and nothing paces the loop. */
static void run(const char* name, const yscr_desc* d0, int n, int heavy, int rate, int hide) {
    static yscr_screen s;
    static yscr_frame f;
    static yrt_event ev[4096];
    static unsigned char ring_mem[YRT_RING_BYTES(4096) + 64];
    yrt_ring ring;
    yrt_ring_desc rd;
    yscr_desc d = *d0;
    yscr_caps caps;
    uint64_t base;
    int i, k, bad = 0, read = 0, tier_bad = 0, shown = 0, flips = 0, locked, rising = 1, dropped = 0, step1 = 0;
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
    if (!g_opened++) {   /* the process's first open: the case that lost its period source */
        const char* p = strstr(line, "period=");
        char src[40] = "";
        long long per = 0;
        if (p) sscanf(p, "period=%lldns(%39[a-z-])", &per, src);
        if (locked) {
            printf("     first open: period=%lldns(%s), the session locked: not checked\n", per, src);
        } else {
            snprintf(msg, sizeof msg, "first open in the process: period=%lldns(%s), from CADisplayLink", per, src);
            check(!strcmp(src, "cadisplaylink"), msg);
        }
    }
    if (hide) {
        SDL_MinimizeWindow(yscr_window(&s));
        SDL_SyncWindow(yscr_window(&s));
        locked = 1;   /* as a locked screen: every frame estimated */
    }
    yscr_get_caps(&s, &caps);
    if (heavy && !gl_setup(&s)) check(0, "GL entry points");
    base = s.next_id + 1;
    for (i = 0; i < n; i++) {
        if (yscr_begin(&s, &f) != YSCR_OK) { check(0, "begin"); break; }
        if (heavy) draw(i, caps.mode.w, caps.mode.h, heavy);
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
            /* shown: presentedTime, tier 2; not shown: estimated, tier 3 */
            if (tier != ((flags & YSCR_FLIP_ESTIMATED) ? YSCR_TIER_3 : YSCR_TIER_2)) tier_bad++;
            if (!(flags & YSCR_FLIP_ESTIMATED)) {
                shown++;
                if (ev[k].u.u16[4]) dropped++;
                /* one vblank after the shown flip before: full rate */
                if (last_onset && llabs((long long)((int64_t)ev[k].t_ns - last_onset - caps.period_ns)) < caps.period_ns / 4)
                    step1++;
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
    snprintf(msg, sizeof msg, "%s: %d flip records, tier 2 shown and 3 estimated%s", name, flips,
             locked ? ", all OCCLUDED or shown (nothing shown)" : "");
    check(flips >= n && tier_bad == 0, msg);
    if (locked) {
        printf("     %s: locked screen or minimized window: no frame shown, presentedTime not checked\n", name);
    } else {
        snprintf(msg, sizeof msg, "%s: %d of %d flips shown, onsets rise", name, shown, flips);
        check(shown >= flips * 3 / 4 && rising, msg);
        /* two frames in flight: one frame a vblank, none dropped (v0.12.0) */
        snprintf(msg, sizeof msg, "%s: %d of %d shown flips a vblank after the one before, %d dropped", name, step1,
                 shown - 1, dropped);
        if (rate) check(step1 >= (shown - 1) * 9 / 10 && dropped <= shown / 50, msg);
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

/* The period's source in the describe line. */
static void period(const yscr_desc* d) {
    static yscr_screen s;
    static const char* const what[5] = {
        "", "period: from CADisplayLink's timestamps, named in the describe line",
        "period: CVDisplayLink when no callback came, the reason named",
        "period: CADisplayLink's off-grid first timestamp (a first open) left out",
        "period: CVDisplayLink when CADisplayLink's steps are off its duration, the reason named" };
    char line[1024];
    const char* p;
    long long per = 0;
    char src[40] = "";
    int k;
    for (k = 1; k <= 4; k++) {
        g_link = k;
        memset(&s, 0, sizeof s);
        if (!yscr_open(&s, d)) { check(0, "period: open"); g_link = 0; return; }
        yscr_describe(&s, line, sizeof line);
        yscr_close(&s);
        p = strstr(line, "period=");
        per = 0; src[0] = 0;
        if (p) sscanf(p, "period=%lldns(%39[a-z-])", &per, src);
        if (k == 1 || k == 3) check(per == 16680375 && !strcmp(src, "cadisplaylink"), what[k]);
        else if (k == 2) check(per > 0 && !strcmp(src, "cvdisplaylink-no-callback"), what[k]);
        else check(per > 0 && !strcmp(src, "cvdisplaylink-link-off-grid"), what[k]);
    }
    g_link = 0;
    memset(&s, 0, sizeof s);
    if (!yscr_open(&s, d)) { check(0, "period: open"); return; }
    yscr_describe(&s, line, sizeof line);
    yscr_close(&s);
    p = strstr(line, "period=");
    if (p) sscanf(p, "period=%lldns(%39[a-z-])", &per, src);
    printf("     the real read: period=%lldns(%s)%s\n", per, src,
           strcmp(src, "cadisplaylink") ? " (no CADisplayLink callback: the display asleep?)" : "");
}

/* An older macOS than 14: refused before ANGLE loads, the floor named;
 * then this system opens again. */
static void old_os(const yscr_desc* d) {
    static yscr_screen s;
    const char* e;
    memset(&s, 0, sizeof s);
    g_old_os = 1;
    check(!yscr_open(&s, d), "old os: an older macOS than 14 is refused");
    e = yscr_error(&s);
    printf("     %s\n", e);
    check(strstr(e, "macOS 14 (Sonoma) or later") != NULL, "old os: the message names the floor");
    if (yscr_is_open(&s)) yscr_close(&s);
    g_old_os = 0;
    memset(&s, 0, sizeof s);
    check(yscr_open(&s, d), "old os: this system (14 or later) opens");
    if (yscr_is_open(&s)) yscr_close(&s);
}

/* Variable refresh: refused, with the fix in the message; fixed: opens. */
static void vrr(const yscr_desc* d) {
    static yscr_screen s;
    char line[1024];
    const char* e;
    memset(&s, 0, sizeof s);
    g_iv_min = 8333333; g_iv_max = 33333333;   /* ProMotion: 120 down to 30 Hz */
    check(!yscr_open(&s, d), "vrr: a variable mode is refused");
    e = yscr_error(&s);
    printf("     %s\n", e);
    check(strstr(e, "variable refresh mode") && strstr(e, "8.333 to 33.333 ms") && strstr(e, "its variable refresh off") &&
          strstr(e, "System Settings > Displays > Refresh Rate"), "vrr: the message names the intervals and the fix");
    if (yscr_is_open(&s)) yscr_close(&s);
    g_iv_min = 8333333; g_iv_max = 8333333;    /* ProMotion set to a fixed 120 Hz */
    memset(&s, 0, sizeof s);
    if (!yscr_open(&s, d)) { check(0, "vrr: a fixed mode opens"); g_iv_min = 0; return; }
    yscr_describe(&s, line, sizeof line);
    check(strstr(line, "refresh_interval=8333333..8333333ns") != NULL, "vrr: a fixed mode opens, its intervals described");
    yscr_close(&s);
    g_iv_min = 0;                              /* the real screen */
    memset(&s, 0, sizeof s);
    if (!yscr_open(&s, d)) { check(0, "vrr: the real screen opens"); return; }
    yscr_describe(&s, line, sizeof line);
    {
        const char* r = strstr(line, "refresh_interval=");
        long long a = 0, b = 0;
        if (r) sscanf(r, "refresh_interval=%lld..%lldns", &a, &b);
        printf("     the real screen: refresh_interval=%lld..%lldns\n", a, b);
        check(r && a > 0 && a == b, "vrr: the real screen's intervals are equal (a fixed mode)");
    }
    yscr_close(&s);
}

/* --- input: NSEvent stamps through yscr_event_input() --------------------- */

/* CoreGraphics' event posting, declared here: Apple's headers bring
 * CarbonCore's globals into this file (docs/screen.md, METAL). */
typedef struct { double x, y; } in_pt;
extern void* CGEventCreateKeyboardEvent(void* source, uint16_t key, bool down);
extern void* CGEventCreateMouseEvent(void* source, uint32_t type, in_pt at, uint32_t button);
extern void  CGEventPost(uint32_t tap, void* event);
extern void  CGEventPostToPid(int pid, void* event);
extern bool  CGPreflightPostEventAccess(void);
extern void  CFRelease(const void* cf);

#define IN_F19    0x50
#define IN_LSHIFT 0x38
enum { IN_KEY_DOWN, IN_KEY_UP, IN_BTN_DOWN, IN_BTN_UP, IN_MOD_DOWN, IN_MOD_UP, IN_KINDS };
static const char* const in_kind_name[IN_KINDS] = { "key down", "key up", "button down", "button up",
                                                    "shift down", "shift up" };
#define IN_MAX 4096

/* the poster's: kind, mach ns before and after the post call */
static struct { int kind; int64_t a, b; } g_post[IN_MAX];
static volatile int g_npost, g_post_done;
/* the test's own NSEvent monitor: type, key code, NSEvent.timestamp in ns */
static struct { unsigned long type; unsigned code; int64_t ts; } g_mon[IN_MAX];
static volatile int g_nmon;
/* yscr_event_input()'s: kind, the event's t, its stamp tier, yscr_poll()'s t */
static struct { int kind; int64_t t, t_sdl, t_poll; int stamp; } g_got[IN_MAX];
static int g_ngot;

static int64_t in_now(void) { return yrt_now_ns(); }

typedef struct { int pid, to_pid, n, mods; in_pt at; } in_job;

static void in_post(const in_job* j, int kind, void* ev) {
    int k = g_npost;
    if (k >= IN_MAX || !ev) { if (ev) CFRelease(ev); return; }
    g_post[k].kind = kind;
    g_post[k].a = in_now();
    if (j->to_pid) CGEventPostToPid(j->pid, ev);
    else CGEventPost(0, ev);   /* kCGHIDEventTap */
    g_post[k].b = in_now();
    __atomic_store_n(&g_npost, k + 1, __ATOMIC_RELEASE);
    CFRelease(ev);
}

static void in_sleep_until(int64_t t) {
    while (in_now() < t) {
        struct timespec ts = { 0, 200000 };
        nanosleep(&ts, NULL);
    }
}

/* 40 to 50 ms apart at a random phase to the frames, as the probe's
 * input section: key down, key up 15 ms later, then the same for the left
 * button at the window's center, and with mods the left shift key. */
static void* in_poster(void* p) {
    const in_job* j = (const in_job*)p;
    uint32_t rng = 777;
    int64_t t = in_now() + 500000000;
    int i;
    for (i = 0; i < j->n; i++) {
        int k;
        for (k = 0; k < (j->mods ? 3 : 2); k++) {
            rng = rng * 1664525u + 1013904223u;
            t += 40000000 + (int64_t)((rng >> 8) % 10000000u);
            in_sleep_until(t);
            if (k == 0) in_post(j, IN_KEY_DOWN, CGEventCreateKeyboardEvent(NULL, IN_F19, true));
            else if (k == 1) in_post(j, IN_BTN_DOWN, CGEventCreateMouseEvent(NULL, 1, j->at, 0));
            else in_post(j, IN_MOD_DOWN, CGEventCreateKeyboardEvent(NULL, IN_LSHIFT, true));
            in_sleep_until(t + 15000000);
            if (k == 0) in_post(j, IN_KEY_UP, CGEventCreateKeyboardEvent(NULL, IN_F19, false));
            else if (k == 1) in_post(j, IN_BTN_UP, CGEventCreateMouseEvent(NULL, 2, j->at, 0));
            else in_post(j, IN_MOD_UP, CGEventCreateKeyboardEvent(NULL, IN_LSHIFT, false));
        }
    }
    in_sleep_until(in_now() + 300000000);
    __atomic_store_n(&g_post_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int in_kind_of_ns(unsigned long type, unsigned code) {
    if (type == 10 && code == IN_F19) return IN_KEY_DOWN;
    if (type == 11 && code == IN_F19) return IN_KEY_UP;
    if (type == 1) return IN_BTN_DOWN;
    if (type == 2) return IN_BTN_UP;
    return -1;   /* flags changed: down or up by the order */
}

static int in_cmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

/* min / p50 / p99 / max of n values, sorted in place */
static void in_stats(double* v, int n, double* mn, double* p50, double* p99, double* mx) {
    *mn = *p50 = *p99 = *mx = 0;
    if (n <= 0) return;
    qsort(v, (size_t)n, sizeof *v, in_cmp);
    *mn = v[0];
    *p50 = v[(n - 1) / 2];
    *p99 = v[(int)((n - 1) * 0.99 + 0.5)];
    *mx = v[n - 1];
}

/* One pass: post keys, buttons and shift while a frame loop reads them
 * through yscr_poll() and yscr_event_input(); pair the k-th of each kind
 * in the three lists. */
static void in_pass(yscr_screen* s, const char* name, int to_pid, int n) {
    static double a1[IN_MAX], a2[IN_MAX], a3[IN_MAX];
    static int idx_post[IN_KINDS][IN_MAX / 2], idx_mon[IN_KINDS][IN_MAX / 2], idx_got[IN_KINDS][IN_MAX / 2];
    int cnt_post[IN_KINDS], cnt_mon[IN_KINDS], cnt_got[IN_KINDS];
    in_job j;
    pthread_t th;
    yscr_frame f;
    int x = 0, y = 0, w = 0, h = 0, k, i, m_shift = 0;
    char msg[256];
    SDL_GetWindowPosition(yscr_window(s), &x, &y);
    SDL_GetWindowSize(yscr_window(s), &w, &h);
    memset(&j, 0, sizeof j);
    j.pid = (int)getpid();
    j.to_pid = to_pid;
    j.n = n;
    j.mods = to_pid;   /* shift through the HID tap would reach the active app's modifiers too */
    j.at.x = x + w / 2.0;
    j.at.y = y + h / 2.0;
    g_npost = 0; g_post_done = 0; g_nmon = 0; g_ngot = 0;
    pthread_create(&th, NULL, in_poster, &j);
    while (!__atomic_load_n(&g_post_done, __ATOMIC_ACQUIRE)) {
        SDL_Event ev;
        int64_t t;
        if (yscr_begin(s, &f) != YSCR_OK) break;
        while (yscr_poll(s, &ev, &t)) {
            yin_event e;
            int kind = -1;
            if (!yscr_event_input(s, &ev, &e)) continue;
            if (e.kind == YIN_KIND_KEYBOARD && e.control == SDL_SCANCODE_F19 && !(e.flags & YIN_REPEAT))
                kind = e.type == YIN_PRESS ? IN_KEY_DOWN : IN_KEY_UP;
            else if (e.kind == YIN_KIND_KEYBOARD && e.control == SDL_SCANCODE_LSHIFT)
                kind = e.type == YIN_PRESS ? IN_MOD_DOWN : IN_MOD_UP;
            else if (e.kind == YIN_KIND_MOUSE && e.control == 1 && (e.type == YIN_PRESS || e.type == YIN_RELEASE))
                kind = e.type == YIN_PRESS ? IN_BTN_DOWN : IN_BTN_UP;
            if (kind >= 0 && g_ngot < IN_MAX) {
                g_got[g_ngot].kind = kind;
                g_got[g_ngot].t = e.t;
                g_got[g_ngot].t_sdl = yscr_restamp(s, ev.common.timestamp);   /* SDL's own */
                g_got[g_ngot].t_poll = t;                                     /* yscr_poll()'s (v0.16.0) */
                g_got[g_ngot].stamp = e.stamp;
                g_ngot++;
            }
        }
        if (yscr_flip(s) != YSCR_OK) break;
    }
    pthread_join(th, NULL);
    memset(cnt_post, 0, sizeof cnt_post);
    memset(cnt_mon, 0, sizeof cnt_mon);
    memset(cnt_got, 0, sizeof cnt_got);
    for (i = 0; i < g_npost; i++) idx_post[g_post[i].kind][cnt_post[g_post[i].kind]++] = i;
    for (i = 0; i < g_nmon; i++) {
        int kind = in_kind_of_ns(g_mon[i].type, g_mon[i].code);
        if (g_mon[i].type == 12 && g_mon[i].code == IN_LSHIFT) kind = (m_shift++ & 1) ? IN_MOD_UP : IN_MOD_DOWN;
        if (kind >= 0) idx_mon[kind][cnt_mon[kind]++] = i;
    }
    for (i = 0; i < g_ngot; i++) idx_got[g_got[i].kind][cnt_got[g_got[i].kind]++] = i;
    printf("     input %s: %d posted, the test's monitor saw %d, yscr_event_input() gave %d\n", name, g_npost, g_nmon,
           g_ngot);
    printf("     %-12s %5s %9s %20s %34s %34s\n", "kind", "n", "fallback", "|stamp - NSEvent| max",
           "stamp - post min/p50/p99/max", "SDL - post min/p50/p99/max");
    for (k = 0; k < IN_KINDS; k++) {
        int n_k = cnt_post[k] < cnt_got[k] ? cnt_post[k] : cnt_got[k], nm = 0, fb = 0, bad = 0, before = 0, poll_off = 0;
        double pmn, p50, p99, mx, qmn, q50, q99, qmx, rmn, r50, r99, rmx;
        if (cnt_post[k] == 0) continue;
        if (cnt_mon[k] < n_k) nm = cnt_mon[k]; else nm = n_k;
        for (i = 0; i < n_k; i++) {
            int gi = idx_got[k][i], pi = idx_post[k][i];
            if (g_got[gi].stamp == YIN_TIER_3) fb++;
            if (g_got[gi].t_poll != g_got[gi].t) poll_off++;
            a2[i] = (double)(g_got[gi].t - g_post[pi].a) / 1000.0;
            a3[i] = (double)(g_got[gi].t_sdl - g_post[pi].a) / 1000.0;
            if (g_got[gi].t < g_post[pi].a) before++;
        }
        for (i = 0; i < nm; i++) {
            int gi = idx_got[k][i];
            int64_t d = g_got[gi].t - g_mon[idx_mon[k][i]].ts;
            a1[i] = (double)(d < 0 ? -d : d) / 1000.0;
            if (d < -1000 || d > 1000) bad++;
        }
        in_stats(a1, nm, &pmn, &p50, &p99, &mx);
        in_stats(a2, n_k, &qmn, &q50, &q99, &qmx);
        in_stats(a3, n_k, &rmn, &r50, &r99, &rmx);
        printf("     %-12s %5d %9d %20.3f %7.1f / %6.1f / %6.1f / %7.1f %7.1f / %6.1f / %6.1f / %7.1f  us\n",
               in_kind_name[k], n_k, fb, mx, qmn, q50, q99, qmx, rmn, r50, r99, rmx);
        if (to_pid && (k == IN_BTN_DOWN || k == IN_BTN_UP)) {
            /* SDL 3.4 delivers no button posted to the process (measured: 0
             * of 20): the monitor's entries are events SDL drops, which the
             * pairing must pass over; the keys' checks show that it did */
            printf("     input %s, %s: SDL delivered %d of %d, the monitors saw %d\n", name, in_kind_name[k], n_k,
                   cnt_post[k], cnt_mon[k]);
            continue;
        }
        snprintf(msg, sizeof msg, "input %s, %s: %d of %d posted delivered, %d with SDL's stamp, %d of %d off the "
                 "NSEvent stamp by more than 1 us, %d before the post, %d with yscr_poll()'s time not the event's",
                 name, in_kind_name[k], n_k, cnt_post[k], fb, bad, nm, before, poll_off);
        check(n_k == cnt_post[k] && fb == 0 && nm == n_k && bad == 0 && before == 0 && poll_off == 0, msg);
    }
}

static void input(const yscr_desc* d0) {
    static yscr_screen s;
    yscr_desc d = *d0;
    yscr_caps caps;
    char line[1024];
    id (^handler)(id) = ^id(id e) {
        int k = g_nmon;
        if (k < IN_MAX) {
            unsigned long type = ((unsigned long (*)(id, SEL))objc_msgSend)(e, sel_registerName("type"));
            double ts = ((double (*)(id, SEL))objc_msgSend)(e, sel_registerName("timestamp"));
            g_mon[k].type = type;
            g_mon[k].code = type >= 10 && type <= 12
                                ? ((unsigned short (*)(id, SEL))objc_msgSend)(e, sel_registerName("keyCode")) : 0u;
            g_mon[k].ts = (int64_t)llround(ts * 1e9);
            g_nmon = k + 1;
        }
        return e;
    };
    id mon, app;
    int n = getenv("YSCR_TEST_INPUT_N") ? atoi(getenv("YSCR_TEST_INPUT_N")) : 20, active = 0, tries;
    if (n < 1) n = 1;
    if (n > IN_MAX / 8) n = IN_MAX / 8;
    if (!CGPreflightPostEventAccess()) {
        printf("     input: posting events needs the Accessibility permission: not checked\n");
        return;
    }
    memset(&s, 0, sizeof s);
    if (!yscr_open(&s, &d)) { check(0, "input: open"); return; }
    yscr_get_caps(&s, &caps);
    check(caps.os_input_stamps, "input: caps.os_input_stamps on METAL");
    yscr_describe(&s, line, sizeof line);
    if (is_locked(&s)) {
        printf("     input: the session is locked: not checked\n");
        yscr_close(&s);
        return;
    }
    mon = ((id (*)(id, SEL, unsigned long long, id))objc_msgSend)(
        (id)objc_getClass("NSEvent"), sel_registerName("addLocalMonitorForEventsMatchingMask:handler:"),
        (1ull << 1) | (1ull << 2) | (1ull << 10) | (1ull << 11) | (1ull << 12), (id)handler);
    app = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
    for (tries = 0; tries < 120 && !active; tries++) {   /* up to 2 s for the app to come to the front */
        yscr_frame f;
        SDL_Event ev;
        if (yscr_begin(&s, &f) != YSCR_OK) break;
        while (yscr_poll(&s, &ev, NULL)) {}
        yscr_flip(&s);
        active = ((signed char (*)(id, SEL))objc_msgSend)(app, sel_registerName("isActive")) != 0;
    }
    in_pass(&s, "CGEventPostToPid", 1, n);
    /* a HID event goes to the active app: only when this one is */
    if (active) in_pass(&s, "CGEventPost(HID)", 0, n);
    else printf("     input CGEventPost(HID): skipped, this program is not the active app\n");
    yscr_describe(&s, line, sizeof line);
    {
        const char* o = strstr(line, "os_stamps=");
        char* e = o ? strchr(o, ')') : NULL;
        if (e) e[1] = 0;
        printf("     %s\n", o ? o : "(no os_stamps in the describe line)");
    }
    ((void (*)(id, SEL, id))objc_msgSend)((id)objc_getClass("NSEvent"), sel_registerName("removeMonitor:"), mon);
    yscr_close(&s);
}

/* --- v0.16.0: the display state, the gamma table, raw mice, the panic ----- */

extern int32_t CGGetDisplayTransferByTable(uint32_t d, uint32_t cap, float* r, float* g, float* b, uint32_t* n);
extern int32_t CGSetDisplayTransferByTable(uint32_t d, uint32_t n, const float* r, const float* g, const float* b);
extern void    CGDisplayRestoreColorSyncSettings(void);

/* the describe line's number after key, or -1 */
static long long word_num(const char* line, const char* key) {
    const char* p = strstr(line, key);
    return p ? atoll(p + strlen(key)) : -1;
}

/* the describe line's word after key, up to a space */
static void word(const char* line, const char* key, char* out, size_t cap) {
    const char* p = strstr(line, key);
    size_t n = 0;
    out[0] = 0;
    if (!p) return;
    p += strlen(key);
    while (p[n] && p[n] != ' ' && n + 1 < cap) { out[n] = p[n]; n++; }
    out[n] = 0;
}

/* state: the code risk's checks run on METAL (STATE_UNKNOWN clear), and
 * SCALED follows the display mode against the panel's native pixels. */
static void state(const yscr_desc* d0) {
    static yscr_screen s;
    yscr_desc d = *d0;
    char line[1600], g[48], msg[300];
    long long pw, ph, mw, mh;
    uint16_t r;
    int fs;
    for (fs = 1; fs >= 0; fs--) {
        d.windowed = !fs;
        memset(&s, 0, sizeof s);
        if (!yscr_open(&s, &d)) { snprintf(msg, sizeof msg, "state: open: %s", yscr_error(&s)); check(0, msg); return; }
        yscr_describe(&s, line, sizeof line);
        r = yscr_code_risk(&s);
        pw = ph = mw = mh = -1;
        if (strstr(line, "panel=")) sscanf(strstr(line, "panel="), "panel=%lldx%lld", &pw, &ph);
        if (strstr(line, "mode_px=")) sscanf(strstr(line, "mode_px="), "mode_px=%lldx%lld", &mw, &mh);
        word(line, "os_gamma=", g, sizeof g);
        printf("     state %s: risk 0x%x, panel %lldx%lld, mode %lldx%lld, os_gamma=%s\n", fs ? "fullscreen" : "window",
               (unsigned)r, pw, ph, mw, mh, g);
        {
            const char* o = strstr(line, "display=");
            if (o) printf("     %.*s\n", (int)(strstr(o, "nightshift") ? strstr(o, "nightshift") - o : 80), o);
        }
        snprintf(msg, sizeof msg, "state %s: every check ran (no STATE_UNKNOWN)", fs ? "fullscreen" : "window");
        check(!(r & YSCR_CODE_RISK_STATE_UNKNOWN), msg);
        snprintf(msg, sizeof msg, "state %s: SCALED is %s (mode %lldx%lld, panel %lldx%lld)", fs ? "fullscreen" : "window",
                 fs && (pw != mw || ph != mh) ? "set" : "clear", mw, mh, pw, ph);
        check(((r & YSCR_CODE_RISK_SCALED) != 0) == (fs && (pw != mw || ph != mh)), msg);
        snprintf(msg, sizeof msg, "state %s: os_gamma=%s", fs ? "fullscreen" : "window", g);
        check(fs ? (!strcmp(g, "identity") || !strcmp(g, "set")) : !strncmp(g, "identity(kept)", 14) ||
                   !strcmp(g, "not-identity(kept)"), msg);
        check(strstr(line, "layer_cs=untagged") != NULL, "state: the layer is untagged (no color matching)");
        yscr_close(&s);
    }
    g_hdr = 1;   /* an EDR-capable display: ADVANCED_COLOR */
    memset(&s, 0, sizeof s);
    if (yscr_open(&s, &d)) {
        check((yscr_code_risk(&s) & YSCR_CODE_RISK_ADVANCED_COLOR) != 0, "state: an EDR display sets ADVANCED_COLOR");
        yscr_close(&s);
    }
    g_hdr = 0;
}

/* gamma: a table that is not the identity is set to it at a fullscreen
 * open, read back, and put back at close; a refused set and
 * keep_os_gamma leave it and set CODE_RISK_GAMMA. One entry of 1024 is
 * changed, 0.5 to 0.52, for a few seconds; ColorSync's table at the end. */
static void gamma_case(const yscr_desc* d0) {
    static float r[1024], gg[1024], b[1024], back[3][1024];
    static yscr_screen s;
    yscr_desc d = *d0;
    char line[1600], g[48], msg[300];
    uint32_t did, n = 0, i;
    int pass;
    d.windowed = false;
    memset(&s, 0, sizeof s);
    if (!yscr_open(&s, &d)) { check(0, "gamma: open"); return; }
    yscr_describe(&s, line, sizeof line);
    did = (uint32_t)word_num(line, "display=");
    yscr_close(&s);
    for (i = 0; i < 1024; i++) r[i] = gg[i] = b[i] = (float)i / 1023.0f;
    r[512] = 0.52f;
    for (pass = 0; pass < 3; pass++) {
        const char* want = pass == 0 ? "set" : pass == 1 ? "refused" : "not-identity(kept)";
        CGSetDisplayTransferByTable(did, 1024, r, gg, b);
        g_gamma_refuse = pass == 1;
        d.keep_os_gamma = pass == 2;
        memset(&s, 0, sizeof s);
        if (!yscr_open(&s, &d)) { check(0, "gamma: open"); break; }
        yscr_describe(&s, line, sizeof line);
        word(line, "os_gamma=", g, sizeof g);
        CGGetDisplayTransferByTable(did, 1024, back[0], back[1], back[2], &n);
        snprintf(msg, sizeof msg, "gamma: os_gamma=%s (want %s), the table while open: R[512] %.4f", g, want, back[0][512]);
        check(!strcmp(g, want) && (pass == 0 ? yscr__table_identity(back[0], n) : back[0][512] > 0.51f), msg);
        snprintf(msg, sizeof msg, "gamma %s: CODE_RISK_GAMMA %s", want, pass == 0 ? "clear" : "set");
        check(((yscr_code_risk(&s) & YSCR_CODE_RISK_GAMMA) != 0) == (pass != 0), msg);
        yscr_close(&s);
        CGGetDisplayTransferByTable(did, 1024, back[0], back[1], back[2], &n);
        snprintf(msg, sizeof msg, "gamma %s: after close the table is the one before open (R[512] %.4f)", want, back[0][512]);
        check(n == 1024 && back[0][512] > 0.519f && back[0][512] < 0.521f, msg);
    }
    g_gamma_refuse = 0;
    CGDisplayRestoreColorSyncSettings();
    CGGetDisplayTransferByTable(did, 1024, back[0], back[1], back[2], &n);
    check(yscr__table_identity(back[0], n), "gamma: ColorSync's table again at the end");
}

/* rawmice: the reader opens (IOHIDManager, Input Monitoring), and its mice
 * are YSCR_EV_DEVICE records. YSCR_TEST_MOUSE_S=n: n seconds of a person
 * moving the mouse, for the report rate and the stamp against arrival. */
static int64_t g_arr[8192];
static volatile int g_narr;
static void rm_arrival(int64_t t) {
    int k = g_narr;
    if (k < 8192) { g_arr[k] = yrt_now_ns() - t; g_narr = k + 1; }
}

/* The values more than 1 s old at their callback, or found stale: the
 * reader's thread writes, the test reads after the run. */
static struct { uint32_t page, usage; int64_t v, age, since_start; int stale; } g_old[64];
static volatile int g_nold, g_nall;
static void rm_value(uint32_t page, uint32_t usage, int64_t v, int64_t t, int stale) {
    int64_t now = yrt_now_ns();
    int k = g_nold;
    g_nall++;
    if ((now - t > 1000000000 || stale) && k < 64) {
        g_old[k].page = page; g_old[k].usage = usage; g_old[k].v = v; g_old[k].age = now - t;
        g_old[k].since_start = now - yscr__rm.t_start; g_old[k].stale = stale;
        g_nold = k + 1;
    }
}

static void print_old(void) {
    int k;
    printf("     rawmice: %d values, %d of them more than 1 s old or stale:\n", g_nall, g_nold);
    for (k = 0; k < g_nold; k++)
        printf("       page 0x%02x usage 0x%03x value %lld: stamp %.3f s before its callback, callback %.3f s after the "
               "reader's start, %s\n", (unsigned)g_old[k].page, (unsigned)g_old[k].usage, (long long)g_old[k].v,
               (double)g_old[k].age / 1e9, (double)g_old[k].since_start / 1e9, g_old[k].stale ? "dropped" : "KEPT");
}

/* No hand on the mouse: a value dated before the reader's start (as IOKit
 * gave cached values after the open) and one older than its device's
 * newest are dropped, and a fresh one reaches yscr_event_input(). Calls
 * the reader's own function from this thread while the reader is idle. */
static void rm_stale_case(yscr_screen* s, uint32_t id, int64_t* newest) {
    yscr_frame f;
    uint32_t st0 = yscr__rm.stale;
    int64_t now = yrt_now_ns(), seen_fresh = 0, seen_stale = 0, seen_old = 0;
    int frames;
    yscr_mouse_event done;
    char msg[200];
    /* a stale button 2 press, 44 s old, before the open: as the hand run's values */
    yscr__rm_take(id, newest, now - 44000000000LL, 9, 2, 1, 0, 0, 1);
    yscr__rm_take(id, newest, now, 9, 2, 1, 0, 0, 1);                 /* a fresh press */
    yscr__rm_take(id, newest, now - 1000000, 9, 3, 1, 0, 0, 1);       /* older than the newest */
    if (yscr__hid_flush(&yscr__rm.acc, &done)) yscr__rm_push(&done);
    yscr__rm_take(id, newest, now + 1, 9, 2, 0, 0, 0, 1);             /* its release */
    if (yscr__hid_flush(&yscr__rm.acc, &done)) yscr__rm_push(&done);
    for (frames = 0; frames < 30 && yscr_begin(s, &f) == YSCR_OK; frames++) {
        SDL_Event e;
        while (yscr_poll(s, &e, NULL)) {
            yin_event in;
            if (!yscr_event_input(s, &e, &in) || in.kind != YIN_KIND_MOUSE || e.type != yscr_input_event_type()) continue;
            if (in.t == now) seen_fresh++;
            else if (in.t < yscr__rm.t_start) seen_stale++;
            else if (in.t == now - 1000000) seen_old++;
        }
        if (yscr_flip(s) != YSCR_OK) break;
    }
    if (yscr__rm.dropped && !seen_fresh) {
        printf("     rawmice: stale case: this program is not the active app, so the reader dropped every report: "
               "not checked\n");
        return;
    }
    snprintf(msg, sizeof msg, "rawmice: a value dated before the open and one older than its device's newest dropped "
             "(%u counted), a fresh one delivered (%lld)", yscr__rm.stale - st0, (long long)seen_fresh);
    check(yscr__rm.stale - st0 == 2 && seen_fresh == 1 && !seen_stale && !seen_old, msg);
}

static void rawmice(const yscr_desc* d0) {
    static yscr_screen s;
    static yrt_event ev[256];
    static unsigned char ring_mem[YRT_RING_BYTES(4096) + 64];
    static double iv[8192], lag[8192];
    static int64_t ts[8192];
    yrt_ring ring;
    yrt_ring_desc rd;
    yscr_desc d = *d0;
    char msg[300];
    int n, i, mice = 0, on = 0, secs = getenv("YSCR_TEST_MOUSE_S") ? atoi(getenv("YSCR_TEST_MOUSE_S")) : 0;
    uint32_t ids[8];
    memset(&rd, 0, sizeof rd);
    rd.memory = ring_mem;
    rd.bytes = sizeof ring_mem;
    if (!yrt_ring_open(&ring, &rd)) { check(0, "rawmice: ring"); return; }
    d.ring = &ring;
    d.raw_mice = true;
    memset(&s, 0, sizeof s);
    if (!yscr_open(&s, &d)) {
        snprintf(msg, sizeof msg, "rawmice: open: %s", yscr_error(&s));
        check(0, msg);
        yrt_ring_close(&ring);
        return;
    }
    while ((n = yrt_ring_drain(&ring, ev, 256)) > 0)
        for (i = 0; i < n; i++) {
            if (ev[i].kind == YSCR_EV_RAW_MICE && ev[i].u.u32[0] == YSCR_RAW_MICE_ON) on++;
            if (ev[i].kind == YSCR_EV_DEVICE && ev[i].u.u32[0] == YSCR_DEV_MOUSE && ev[i].u.u32[1] == YSCR_DEV_PRESENT && ev[i].u.u64[1] < 0x100000000ull &&
                yscr__a_load(&yscr__rm.users) > 0) {
                int k;
                for (k = 0; k < YSCR__RM_DEVS; k++)
                    if (yscr__rm.dev[k].id == (uint32_t)ev[i].u.u64[1] && mice < 8) {
                        ids[mice++] = (uint32_t)ev[i].u.u64[1];
                        printf("     rawmice: device 0x%08x \"%s\"\n", (unsigned)ev[i].u.u64[1], (const char*)ev[i].u.bytes + 16);
                    }
            }
        }
    check(on == 1, "rawmice: one YSCR_EV_RAW_MICE ON record");
    snprintf(msg, sizeof msg, "rawmice: the reader's mice are YSCR_EV_DEVICE records, present at open (%d)", mice);
    check(mice >= 1, msg);   /* present at open, not added at a later begin() */
    if (mice >= 1) {
        int k;
        for (k = 0; k < YSCR__RM_DEVS && yscr__rm.dev[k].id != ids[0]; k++) { }
        if (k < YSCR__RM_DEVS) rm_stale_case(&s, ids[0], &yscr__rm.dev[k].newest);
    }
    if (secs > 0) {
        yscr_frame f;
        int64_t end, prev = 0;
        int nts = 0, nev = 0, unlisted = 0;
        printf("     rawmice: MOVE THE MOUSE for %d s now\n", secs);
        fflush(stdout);
        g_narr = 0;
        g_nold = g_nall = 0;
        end = yrt_now_ns() + (int64_t)secs * 1000000000;
        while (yrt_now_ns() < end && yscr_begin(&s, &f) == YSCR_OK) {
            SDL_Event e;
            while (yscr_poll(&s, &e, NULL)) {
                yin_event in;
                if (!yscr_event_input(&s, &e, &in) || in.kind != YIN_KIND_MOUSE || e.type != yscr_input_event_type()) continue;
                nev++;
                if (in.flags & YIN_UNLISTED) unlisted++;
                if (in.t != prev && nts < 8192) ts[nts++] = in.t;
                prev = in.t;
            }
            if (yscr_flip(&s) != YSCR_OK) break;
        }
        for (i = 1; i < nts; i++) iv[i - 1] = (double)(ts[i] - ts[i - 1]) / 1e6;
        for (i = 0; i < g_narr; i++) lag[i] = (double)g_arr[i] / 1e3;
        {
            double a, b, c, e2;
            in_stats(iv, nts > 1 ? nts - 1 : 0, &a, &b, &c, &e2);
            printf("     rawmice: %d reports, %d events, %d unlisted; report interval ms min/p50/p99/max %.3f / %.3f / %.3f / %.3f\n",
                   nts, nev, unlisted, a, b, c, e2);
            in_stats(lag, g_narr, &a, &b, &c, &e2);
            printf("     rawmice: callback minus the value's stamp, us, min/p50/p99/max %.1f / %.1f / %.1f / %.1f (%d values)\n",
                   a, b, c, e2, g_narr);
        }
        print_old();
        {
            char line[1600];
            const char* r;
            yscr_describe(&s, line, sizeof line);
            r = strstr(line, "raw_mice=");
            printf("     rawmice: %.*s\n", r ? (int)(strchr(r, ')') ? strchr(r, ')') - r + 1 : 40) : 4, r ? r : "none");
        }
        check(nts > 10 && unlisted == 0, "rawmice: reports came, none unlisted");
        snprintf(msg, sizeof msg, "rawmice: no value kept more than 1 s after its stamp (max %.1f ms)",
                 g_narr ? lag[g_narr - 1] / 1e3 : 0.0);
        check(g_narr > 0 && lag[g_narr - 1] < 1e6, msg);
    } else {
        printf("     rawmice: YSCR_TEST_MOUSE_S not set: the report rate and the stamp need a person moving the mouse\n");
    }
    yscr_close(&s);
    yrt_ring_close(&ring);
}

/* --- the panic: a child process hangs, and the watchdog ends it --------- */

static yscr_screen g_ps;
static int g_panic_other;   /* the presses go to another process */
extern void* CGEventSourceCreate(int32_t state);

static void panic_last_words(void* ctx) {
    (void)ctx;
    printf("panic_fn ran\n");
    fflush(stdout);
}

/* 3 presses of Shift+Esc, 300 ms apart, from 1.5 s after the hang: by
 * the seam (as the tap would call it) or posted to the HID tap. */
static void* panic_presser(void* p) {
    int post = *(int*)p, i;
    /* YSCR_TEST_PANIC_DELAY_MS: a longer hang first, as the beach ball comes */
    in_sleep_until(in_now() + (getenv("YSCR_TEST_PANIC_DELAY_MS") ? atoll(getenv("YSCR_TEST_PANIC_DELAY_MS")) : 1500) * 1000000LL);
    for (i = 0; i < 3; i++) {
        int64_t t = in_now();
        if (i == 2) { printf("t3=%lld\n", (long long)t); fflush(stdout); }
        if (post) {
            void* down = CGEventCreateKeyboardEvent(NULL, 53, true);
            void* up = CGEventCreateKeyboardEvent(NULL, 53, false);
            void* sd = CGEventCreateKeyboardEvent(NULL, 0x38, true);
            void* su = CGEventCreateKeyboardEvent(NULL, 0x38, false);
            extern void CGEventSetFlags(void*, uint64_t);
            CGEventSetFlags(down, 0x00020000u);
            CGEventSetFlags(up, 0x00020000u);
            CGEventPost(0, sd); CGEventPost(0, down);
            in_sleep_until(t + 50000000);
            CGEventPost(0, up); CGEventPost(0, su);
            CFRelease(down); CFRelease(up); CFRelease(sd); CFRelease(su);
        } else {
            yscr__wd_feed(53, 1, 0x00020000u, g_panic_other ? 1 : (int64_t)getpid(), 0);
            in_sleep_until(t + 50000000);
            yscr__wd_feed(53, 0, 0x00020000u, g_panic_other ? 1 : (int64_t)getpid(), 0);
        }
        in_sleep_until(t + 300000000);
    }
    return NULL;
}

/* The child: open with desc.panic, 60 frames, then hang (the main thread
 * sleeps) or pump without begin() (release). */
static int panic_child(const char* mode, int fs, const char* angle) {
    yscr_desc d;
    yscr_frame f;
    pthread_t th;
    int i, post = !strcmp(mode, "post"), release = !strcmp(mode, "release");
    g_panic_other = !strcmp(mode, "other");
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_METAL;
    d.angle_dir = angle;
    d.windowed = !fs;
    d.settle = YSCR_SETTLE_WARN;
    d.panic = true;
    d.panic_fn = panic_last_words;
    if (release) d.panic_grace_ns = 1000000000;
    g_panic_windowed = 1;
    if (!yscr_open(&g_ps, &d)) { printf("open: %s\n", yscr_error(&g_ps)); return 2; }
    {
        char line[1600];
        yscr_describe(&g_ps, line, sizeof line);
        printf("panic_word=%s\n", strstr(line, "panic=armed") ? "armed" : "not-armed");
        if (strstr(line, "locked=1")) printf("locked\n");
    }
    for (i = 0; i < 60; i++) { if (yscr_begin(&g_ps, &f) != YSCR_OK) break; yscr_flip(&g_ps); }
    fflush(stdout);
    pthread_create(&th, NULL, panic_presser, &post);
    if (!release) {
        struct timespec ts = { g_panic_other ? 4 : 15, 0 };
        nanosleep(&ts, NULL);   /* hung: no pump, no begin() */
        printf("lived\n");
        return 3;
    } else {
        int64_t end = in_now() + 4000000000LL;
        int rc1, rc2;
        uint32_t a1, a2;
        while (in_now() < end) {   /* a loop outside begin() that pumps: an operator dialog */
            struct timespec ts = { 0, 10000000 };
            SDL_PumpEvents();
            nanosleep(&ts, NULL);
        }
        pthread_join(th, NULL);
        printf("before_begin minimized=%d\n", (SDL_GetWindowFlags(yscr_window(&g_ps)) & SDL_WINDOW_MINIMIZED) != 0);
        rc1 = yscr_begin(&g_ps, &f); a1 = f.abort;
        rc2 = yscr_begin(&g_ps, &f); a2 = f.abort;
        printf("release=%.1fms begin1=%d abort=0x%x begin2=%d abort=0x%x minimized=%d fullscreen=%d nsfullscreen=%d\n",
               (double)(yscr__wd.t_release - yscr__wd.t_panic) / 1e6, rc1, a1, rc2, a2,
               (SDL_GetWindowFlags(yscr_window(&g_ps)) & SDL_WINDOW_MINIMIZED) != 0,
               (SDL_GetWindowFlags(yscr_window(&g_ps)) & SDL_WINDOW_FULLSCREEN) != 0,
               (int)((((unsigned long (*)(id, SEL))objc_msgSend)((id)SDL_GetPointerProperty(SDL_GetWindowProperties(
                   yscr_window(&g_ps)), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL), sel_registerName("styleMask")) >> 14) & 1));
        yscr_close(&g_ps);
        return 0;
    }
}

/* The parent: runs the child, reads its lines, and times the end against
 * the third press. */
static void panic_case(const char* self, const char* angle, const char* mode, int fs, int want_code) {
    int fd[2], st = 0, kq;
    pid_t pid;
    char buf[4096], msg[300];
    size_t got = 0;
    ssize_t r;
    long long t3 = 0;
    int64_t t_end;
    struct kevent kev;
    if (pipe(fd) != 0) { check(0, "panic: pipe"); return; }
    pid = fork();
    if (pid == 0) {
        dup2(fd[1], 1);
        close(fd[0]);
        execl(self, self, "panic-child", mode, fs ? "1" : "0", angle ? angle : "", (char*)0);
        _exit(127);
    }
    close(fd[1]);
    kq = kqueue();
    EV_SET(&kev, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    kevent(kq, &kev, 1, NULL, 0, NULL);
    kevent(kq, NULL, 0, &kev, 1, NULL);
    t_end = in_now();
    while (got + 1 < sizeof buf && (r = read(fd[0], buf + got, sizeof buf - 1 - got)) > 0) got += (size_t)r;
    buf[got] = 0;
    close(fd[0]);
    close(kq);
    waitpid(pid, &st, 0);
    if (strstr(buf, "t3=")) t3 = atoll(strstr(buf, "t3=") + 3);
    {
        static char copy[4096];
        char* p = copy;
        memcpy(copy, buf, sizeof copy);
        while (*p) { char* e = strchr(p, '\n'); if (e) *e = 0; printf("     panic %s %s: child: %s\n", mode, fs ? "fullscreen" : "window", p); if (!e) break; p = e + 1; }
    }
    snprintf(msg, sizeof msg, "panic %s %s: exit code %d (want %d), %.1f ms after press 3", mode,
             fs ? "fullscreen" : "window", WIFEXITED(st) ? WEXITSTATUS(st) : -1, want_code,
             t3 ? (double)(t_end - t3) / 1e6 : -1.0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == want_code, msg);
    if (!strcmp(mode, "release")) {
        snprintf(msg, sizeof msg, "panic release %s: the next begin() reports 0x21, the one after 0x20, the window "
                 "minimized", fs ? "fullscreen" : "window");
        check(strstr(buf, "abort=0x21") && strstr(buf, "abort=0x20") && strstr(buf, "minimized=1"), msg);
    } else if (!strcmp(mode, "other")) {
        snprintf(msg, sizeof msg, "panic other %s: presses for another process: no panic", fs ? "fullscreen" : "window");
        check(strstr(buf, "lived") != NULL && strstr(buf, "panic_fn ran") == NULL, msg);
    } else {
        snprintf(msg, sizeof msg, "panic %s %s: panic_fn ran", mode, fs ? "fullscreen" : "window");
        check(strstr(buf, "panic_fn ran") != NULL, msg);
    }
}

/* the v0.16.0 cases: state, gamma, rawmice, panic (seam), panic-post
 * (Shift+Esc posted to the HID tap: an unlocked session, Accessibility),
 * new (all but panic-post) */
static void new_cases(const char* which, const yscr_desc* d, const char* self, const char* angle) {
    int all = !strcmp(which, "new");
    if (all || !strcmp(which, "state")) state(d);
    if (all || !strcmp(which, "gamma")) gamma_case(d);
    if (all || !strcmp(which, "rawmice")) rawmice(d);
    if (all || !strcmp(which, "panic")) {
        panic_case(self, angle, "seam", 0, 99);
        panic_case(self, angle, "seam", 1, 99);
        panic_case(self, angle, "release", 0, 0);
        panic_case(self, angle, "release", 1, 0);
        panic_case(self, angle, "other", 0, 3);
    }
    if (!strcmp(which, "panic-post")) {
        if (!CGPreflightPostEventAccess()) printf("     panic-post: posting needs Accessibility: not run\n");
        else { panic_case(self, angle, "post", 0, 99); panic_case(self, angle, "post", 1, 99); }
    }
}

int main(int argc, char** argv) {
    yscr_desc d;
    const char* angle = argc > 1 ? argv[1] : getenv("YSCR_ANGLE_DIR");
    if (argc > 4 && !strcmp(argv[1], "panic-child")) return panic_child(argv[2], atoi(argv[3]), argv[4][0] ? argv[4] : NULL);
    memset(&d, 0, sizeof d);
    d.backend = YSCR_BACKEND_METAL;
    d.windowed = true;
    d.settle = YSCR_SETTLE_WARN;
    {   /* one of the v0.16.0 cases: argv[1], or YSCR_TEST_CASES (the mutation list sets it) */
        const char* c = argc > 1 ? argv[1] : getenv("YSCR_TEST_CASES");
        if (c && (!strcmp(c, "new") || !strcmp(c, "state") || !strcmp(c, "gamma") || !strcmp(c, "rawmice") ||
                  !strncmp(c, "panic", 5))) {
            angle = argc > 2 ? argv[2] : getenv("YSCR_ANGLE_DIR");
            d.angle_dir = angle;
            new_cases(c, &d, argv[0], angle);
            printf("screen_metal_test: %d of %d checks passed\n", g_checks - g_fail, g_checks);
            return g_fail ? 1 : 0;
        }
    }
    d.angle_dir = angle;
    /* about 4.5 ms of GPU time a frame: shown one a vblank */
    run("rate", &d, 120, 400, 1, 0);
    /* a minimized window: no frame shown, so frames are not paced and ANGLE's
     * frame is still running when the header's command buffer is committed;
     * on a shown screen the missing wait went unseen (0 of 180 frames) */
    run("sync", &d, 120, 400, 0, 1);
    gfx_origin(&d);
    old_os(&d);
    vrr(&d);
    period(&d);
    input(&d);
    new_cases("new", &d, argv[0], angle);
    d.color_depth = 10;
    run("tenbit", &d, 30, 400, 1, 0);
    d.color_depth = 0;
    /* fullscreen: a fullscreen space, where the probe's layer kept one
     * drawable fewer queued than in a window (docs/screen.md, METAL); depth
     * 2 and two frames in flight must still show one frame a vblank */
    d.windowed = false;
    run("fullscreen", &d, 120, 90, 1, 0);   /* 1920 x 1080: 90 iterations, about the window's GPU time */
    d.windowed = true;
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
