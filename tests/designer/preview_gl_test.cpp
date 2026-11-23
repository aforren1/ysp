// preview_gl_test.cpp - the preview on a GL context (designer step 5, part
// D; docs/designer.md, "Plan of step 5", "Tests" and "Redraw and cost").
// Linux: EGL's device platform, a pbuffer, no window. The UI runs as
// main.cpp runs it: the first gfx on the context, ui_frame(), ImGui's
// OpenGL3 backend into framebuffer 0.
//
// Usage: test_designer_preview_gl WORK
//   A copy of stroop, its pane at 1:1 and 1100 x 360 px, paused before the
//   first session, then stepped to frame 130 (the first trial's word is on
//   from frame 120: banner 1.5 s, fixation 0.5 s).
//   1. The session loads on the check thread and attaches on the context.
//   2. One ImGui frame later, glReadPixels() of framebuffer 0 at the
//      image's rectangle gives the texture's bytes: alpha 255, no tint.
//   2b. Zoom 2 (2026-11-23, "Preview size"), panned by a middle drag of
//      an odd number of px: framebuffer 0 shows each texel as a 2 x 2
//      square of px, exactly, where the display's child shows it.
//   3. The word is at its place, in its row's ink: stroop's text box is
//      1000 px wide at the display's center, and its lines start at the
//      box's left edge (align "start"), so the word's left edge is at
//      W / 2 - 500 and its middle at H / 2.
//   2c. Popped out (2026-11-23): ImGui's multi-viewport on a platform of
//      this test's (each OS window a pbuffer with a context that shares
//      the first's), the preview in a window of its own at 1:1: that
//      context's framebuffer is the texture's bytes, through main.cpp's
//      fence (gl_viewports.h).
//   4. Restart, the same seed: stepped to the same frame with no ImGui
//      frame drawn between the steps, gfx gives the same bytes.
//   Skips (exit 77) with no EGL, no device whose GL_RENDERER has
//   YSP_GL_DEVICE (default "llvmpipe"), or no stroop font.
//
// Usage: test_designer_preview_gl --bench PROJECT HZ FRAMES [hidden | WxH]
//   Not a test: the UI frame's cost with the preview running on PROJECT (a
//   folder: it writes its data sets in it), at the default 1920 x 1080
//   display (or W x H) at HZ, Fit, on the device that YSP_GL_DEVICE names. A test
//   clock of 60 Hz paces the session; the frames run back to back. Each
//   line: median, p99 and max in microseconds of FRAMES frames once the
//   session runs; the adoption's frames apart. "hidden": the pane hidden,
//   so no session; the views' cost without the pane. "popout": the preview
//   popped out into a 1920 x 1080 window of its own (a second context and
//   pbuffer), the main window without the pane; render then includes that
//   window's draw and its glFinish().
#include "ui.h"
#include "views.h"
#include "skt.h"
#include "preview.h"
#include "player.h"
#include "ysp/gfx.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"
#include <GLES3/gl3.h>
#include "gl_viewports.h"

#include <dlfcn.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef YSP_SOURCE_DIR
#define YSP_SOURCE_DIR "."
#endif
#ifndef YSP_DESIGNER_FONT_DIR
#define YSP_DESIGNER_FONT_DIR "fonts"
#endif
#ifndef YSP_DESIGNER_TEST_FONT
#define YSP_DESIGNER_TEST_FONT ""
#endif

static int fails, checks;
#define CHECK(c, ...)                                                                                                                      \
    do {                                                                                                                                   \
        checks++;                                                                                                                          \
        if (!(c)) {                                                                                                                        \
            fails++;                                                                                                                       \
            printf("FAIL line %d: ", __LINE__);                                                                                            \
            printf(__VA_ARGS__);                                                                                                           \
            printf("\n");                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

static const int kSkip = 77;
// The pbuffer, as main.cpp's window. The test's is wide enough that the
// pane (45 % of what the side column leaves) holds stroop's 1000 px text
// box at 1:1; the bench's is a window that fills a 1920 x 1080 monitor.
static int FBW = 3800, FBH = 1200;
static const int64_t FR = 16666667;   // a 60 Hz UI frame

// --- EGL: a pbuffer context on the device whose renderer has a name -------------

typedef dsg_gl_fn (*gpa_fn)(const char*);
static gpa_fn g_gpa;
static char g_renderer[200];
// What the viewports' platform needs: the display, the config, the first
// context and its pbuffer, and EGL's calls.
static void *g_dpy, *g_cfg, *g_ctx, *g_pb;
static void* (*g_mkctx)(void*, void*, void*, const int32_t*);
static void* (*g_mkpb)(void*, void*, const int32_t*);
static unsigned (*g_cur)(void*, void*, void*, void*);
static unsigned (*g_destroy_surface)(void*, void*);
static unsigned (*g_destroy_ctx)(void*, void*);

static dsg_gl_fn gl_proc(void*, const char* name) { return g_gpa(name); }

static void* sym(void* lib, const char* n) {
    void* p = dlsym(lib, n);
    if (!p && g_gpa) {
        dsg_gl_fn f = g_gpa(n);
        memcpy(&p, &f, sizeof p);
    }
    return p;
}

template <typename F> static void load(F* f, void* lib, const char* n) {
    void* p = sym(lib, n);
    memcpy(f, &p, sizeof p);
}

// 0, or why not. No eglTerminate() and no dlclose(): Mesa's terminate
// unloads llvmpipe, which LeakSanitizer then cannot name (gfx.md).
static const char* egl_open(const char* want) {
    void* lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    unsigned (*query_devices)(int32_t, void**, int32_t*) = nullptr;
    void* (*gpd)(unsigned, void*, const int32_t*) = nullptr;
    unsigned (*init)(void*, int32_t*, int32_t*) = nullptr;
    unsigned (*bind_api)(unsigned) = nullptr;
    unsigned (*choose)(void*, const int32_t*, void**, int32_t, int32_t*) = nullptr;
    void* (*mkctx)(void*, void*, void*, const int32_t*) = nullptr;
    void* (*mkpb)(void*, void*, const int32_t*) = nullptr;
    unsigned (*cur)(void*, void*, void*, void*) = nullptr;
    if (!lib) return "no libEGL.so.1";
    load(&g_gpa, lib, "eglGetProcAddress");
    if (!g_gpa) return "no eglGetProcAddress";
    load(&query_devices, lib, "eglQueryDevicesEXT");
    load(&gpd, lib, "eglGetPlatformDisplayEXT");
    load(&init, lib, "eglInitialize");
    load(&bind_api, lib, "eglBindAPI");
    load(&choose, lib, "eglChooseConfig");
    load(&mkctx, lib, "eglCreateContext");
    load(&mkpb, lib, "eglCreatePbufferSurface");
    load(&cur, lib, "eglMakeCurrent");
    load(&g_destroy_surface, lib, "eglDestroySurface");
    load(&g_destroy_ctx, lib, "eglDestroyContext");
    void* devs[16];
    int32_t nd = 0;
    if (!query_devices || !gpd || !init || !bind_api || !choose || !mkctx || !mkpb || !cur || !query_devices(16, devs, &nd))
        return "EGL has no device platform";
    for (int32_t i = 0; i < nd; i++) {
        int32_t mj = 0, mn = 0, n = 0;
        void* cfg = nullptr;
        void* dpy = gpd(0x313F, devs[i], nullptr);   // EGL_PLATFORM_DEVICE_EXT
        if (!dpy || !init(dpy, &mj, &mn)) continue;
        bind_api(0x30A0);   // EGL_OPENGL_ES_API
        // RGBA8, a pbuffer, GL ES 3
        const int32_t ca[] = { 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3033, 0x0001, 0x3040, 0x0040, 0x3038 };
        if (!choose(dpy, ca, &cfg, 1, &n) || n < 1) continue;
        const int32_t xa[] = { 0x3098, 3, 0x30FB, 0, 0x3038 };
        void* ctx = mkctx(dpy, cfg, nullptr, xa);
        const int32_t pa[] = { 0x3057, FBW, 0x3056, FBH, 0x3038 };
        void* pb = ctx ? mkpb(dpy, cfg, pa) : nullptr;
        if (!pb || !cur(dpy, pb, pb, ctx)) continue;
        const char* r = (const char*)glGetString(GL_RENDERER);
        snprintf(g_renderer, sizeof g_renderer, "%s", r ? r : "?");
        if (!want || !want[0] || strstr(g_renderer, want)) {
            g_dpy = dpy, g_cfg = cfg, g_ctx = ctx, g_pb = pb;
            g_mkctx = mkctx, g_mkpb = mkpb, g_cur = cur;
            return nullptr;
        }
        cur(dpy, nullptr, nullptr, nullptr);
    }
    return "no EGL device with that renderer";
}

// --- the viewports' platform: an OS window is a pbuffer and a shared context --------

struct PbWindow {
    void* pb = nullptr;
    void* ctx = nullptr;
    ImVec2 pos, size;
    bool minimized = false;
};
static PbWindow g_main_window;
static int g_pb_windows;
static const int kPbW = 2560, kPbH = 1600;   // the largest window a test makes

static PbWindow* pbw(ImGuiViewport* vp) { return (PbWindow*)vp->PlatformUserData; }
static void pb_create(ImGuiViewport* vp) {
    PbWindow* w = IM_NEW(PbWindow)();
    const int32_t xa[] = { 0x3098, 3, 0x30FB, 0, 0x3038 };
    const int32_t pa[] = { 0x3057, kPbW, 0x3056, kPbH, 0x3038 };
    // shares the first context's objects, as SDL_GL_SHARE_WITH_CURRENT_CONTEXT
    w->ctx = g_mkctx(g_dpy, g_cfg, g_ctx, xa);
    w->pb = w->ctx ? g_mkpb(g_dpy, g_cfg, pa) : nullptr;
    w->pos = vp->Pos;
    w->size = vp->Size;
    vp->PlatformUserData = vp->PlatformHandle = w;
    g_pb_windows++;
}
static void pb_destroy(ImGuiViewport* vp) {
    PbWindow* w = pbw(vp);
    if (w && w != &g_main_window) {
        g_cur(g_dpy, g_pb, g_pb, g_ctx);
        if (w->pb) g_destroy_surface(g_dpy, w->pb);
        if (w->ctx) g_destroy_ctx(g_dpy, w->ctx);
        IM_DELETE(w);
    }
    vp->PlatformUserData = vp->PlatformHandle = nullptr;
}
static void pb_show(ImGuiViewport*) {}
static void pb_set_pos(ImGuiViewport* vp, ImVec2 p) { pbw(vp)->pos = p; }
static ImVec2 pb_get_pos(ImGuiViewport* vp) { return pbw(vp)->pos; }
static void pb_set_size(ImGuiViewport* vp, ImVec2 s) { pbw(vp)->size = s; }
static ImVec2 pb_get_size(ImGuiViewport* vp) { return pbw(vp)->size; }
static bool pb_get_minimized(ImGuiViewport* vp) { return pbw(vp)->minimized; }
static void pb_set_title(ImGuiViewport*, const char*) {}
// as SDL3's backend: the window's context current before the renderer draws
static void pb_render(ImGuiViewport* vp, void*) {
    PbWindow* w = pbw(vp);
    if (w && w->ctx) g_cur(g_dpy, w->pb, w->pb, w->ctx);
}
// what a swap waits for, less the vsync
static void pb_swap(ImGuiViewport*, void*) { glFinish(); }

static void pb_install() {
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports;
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_CreateWindow = pb_create;
    pio.Platform_DestroyWindow = pb_destroy;
    pio.Platform_ShowWindow = pb_show;
    pio.Platform_SetWindowPos = pb_set_pos;
    pio.Platform_GetWindowPos = pb_get_pos;
    pio.Platform_SetWindowSize = pb_set_size;
    pio.Platform_GetWindowSize = pb_get_size;
    pio.Platform_GetWindowMinimized = pb_get_minimized;
    pio.Platform_SetWindowTitle = pb_set_title;
    pio.Platform_RenderWindow = pb_render;
    pio.Platform_SwapBuffers = pb_swap;
    ImGuiPlatformMonitor m;
    m.MainPos = m.WorkPos = ImVec2(0, 0);
    m.MainSize = m.WorkSize = ImVec2(7680, 2160);
    pio.Monitors.push_back(m);
    g_main_window.pb = g_pb;
    g_main_window.ctx = g_ctx;
    g_main_window.size = ImVec2((float)FBW, (float)FBH);
    ImGuiViewport* mv = ImGui::GetMainViewport();
    mv->PlatformUserData = mv->PlatformHandle = &g_main_window;
}

// --- the UI, as main.cpp runs it ----------------------------------------------------

static int64_t g_now = 5000000000LL;
static int64_t now_ns(void*) { return g_now; }
static ygfx_gfx g_first;
static ygfx_gl_caps g_caps;

static double us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

// The bench's split of a frame, us.
struct Split {
    double tick, attach, ui, render, finish, total;
};
static Split g_split;
static bool g_in_tick;

extern "C" {
void __real_dsg_preview_tick(dsg_preview* pv, int64_t now_ns, bool shown);
void __wrap_dsg_preview_tick(dsg_preview* pv, int64_t now_ns, bool shown) {
    const double t0 = us();
    g_in_tick = true;
    __real_dsg_preview_tick(pv, now_ns, shown);
    g_in_tick = false;
    g_split.tick += us() - t0;
}
int __real_pl_sim_attach(pl_player* p, int64_t budget_ns);
int __wrap_pl_sim_attach(pl_player* p, int64_t budget_ns) {
    const double t0 = us();
    const int rc = __real_pl_sim_attach(p, budget_ns);
    g_split.attach += us() - t0;
    return rc;
}
}

// One UI frame. wait: the check thread's jobs finish first, so a session
// loads within a frame (the tests). render: ImGui draws into framebuffer 0,
// as main.cpp does; false leaves GL as the tick left it.
static void frame(bool wait, bool render) {
    g_now += FR;
    if (wait) ui_wait_checks(30000000000LL);
    memset(&g_split, 0, sizeof g_split);
    const double t0 = us();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)FBW, (float)FBH);
    io.DeltaTime = (float)FR * 1e-9f;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    skt_frame_begin();
    ui_frame();
    skt_frame_end();
    const double t1 = us();
    ImGui::Render();
    if (render) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, FBW, FBH);
        glClearColor(0.12f, 0.12f, 0.12f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
    // the other viewports, as main.cpp draws them
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        if (render) glvp::begin();
        ImGui::UpdatePlatformWindows();
        if (render) ImGui::RenderPlatformWindowsDefault();
        g_cur(g_dpy, g_pb, g_pb, g_ctx);
        glvp::end();
    }
    const double t2 = us();
    glFinish();   // what a swap waits for, less the vsync
    const double t3 = us();
    g_split.ui = t1 - t0;
    g_split.render = t2 - t1;
    g_split.finish = t3 - t2;
    g_split.total = t3 - t0;
}

static int ui_start(char* err, size_t cap) {
    ygfx_desc d;
    memset(&d, 0, sizeof d);
    d.gl_proc = gl_proc;
    d.width = 64;
    d.height = 64;
    d.output_texture = true;
    if (!ygfx_open(&g_first, &d) || ygfx_gl_caps_get(&g_first, &g_caps) != YGFX_OK) {
        snprintf(err, cap, "the first gfx: %s", ygfx_error(&g_first));
        return -1;
    }
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigInputTextCursorBlink = false;
    ImGui::StyleColorsDark();
    io.DisplaySize = ImVec2((float)FBW, (float)FBH);
    pb_install();
    if (!ImGui_ImplOpenGL3_Init("#version 300 es")) {
        snprintf(err, cap, "ImGui's OpenGL3 backend did not start");
        return -1;
    }
    glvp::install();
    UiPlatform plat = {};
    plat.now_ns = now_ns;
    plat.gl_caps = &g_caps;
    plat.gl_share = &g_first;
    plat.gl_proc = gl_proc;
    return ui_init(YSP_DESIGNER_FONT_DIR, &plat, err, cap) ? 0 : -1;
}

static void ui_stop() {
    ui_shutdown();
    ygfx_close(&g_first);
    ImGui::DestroyPlatformWindows();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
}

static dsg_preview_info info() {
    dsg_preview_info in = {};
    dsg_preview_get_info(ui_preview(), &in);
    return in;
}

// --- the projects ---------------------------------------------------------------------

static bool copy_file(const char* from, const char* to) {
    char err[512];
    char* d = nullptr;
    size_t n = 0;
    if (dsg_read_file(from, &d, &n, err, sizeof err)) return false;
    const bool ok = dsg_write_file(to, d, n, err, sizeof err) == 0;
    free(d);
    return ok;
}

static bool copy_stroop(const char* work, char* dir, size_t cap) {
    static const char* const files[] = { "pack.json", "stroop.json", "stroop.csv" };
    char err[512], a[DSG_PATH_MAX], b[DSG_PATH_MAX], src[DSG_PATH_MAX];
    if (dsg_make_dir(work, err, sizeof err) || dsg_join(dir, cap, work, "stroop") || dsg_make_dir(dir, err, sizeof err)) return false;
    if (dsg_join(src, sizeof src, YSP_SOURCE_DIR, "tests/player/stroop")) return false;
    for (const char* f : files)
        if (dsg_join(a, sizeof a, src, f) || dsg_join(b, sizeof b, dir, f) || !copy_file(a, b)) return false;
    if (dsg_join(b, sizeof b, dir, "fonts") || dsg_make_dir(b, err, sizeof err)) return false;
    if (dsg_join(b, sizeof b, dir, "fonts/NotoSans-Regular.ttf")) return false;
    return copy_file(YSP_DESIGNER_TEST_FONT, b);
}

// The ink of stroop.csv's row (its column "ink"): 0 red, 1 green, 2 blue.
static int ink_of_row(int row) {
    char path[DSG_PATH_MAX], err[256];
    char* d = nullptr;
    size_t n = 0;
    if (row < 0 || dsg_join(path, sizeof path, YSP_SOURCE_DIR, "tests/player/stroop/stroop.csv") ||
        dsg_read_file(path, &d, &n, err, sizeof err))
        return -1;
    const char* line = strchr(d, '\n');   // past the header: word,ink,...
    for (int i = 0; line && i < row; i++) line = strchr(line + 1, '\n');
    int ink = -1;
    if (line) {
        const char* c = strchr(line + 1, ',');
        if (c) ink = strncmp(c + 1, "red", 3) == 0 ? 0 : strncmp(c + 1, "green", 5) == 0 ? 1 : strncmp(c + 1, "blue", 4) == 0 ? 2 : -1;
    }
    free(d);
    return ink;
}

// --- reading back -----------------------------------------------------------------------

// The texture's bytes, RGBA, rows bottom up as GL keeps them.
static bool read_texture(uint32_t tex, int w, int h, unsigned char* out) {
    GLuint fb = 0;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    return ok && glGetError() == GL_NO_ERROR;
}

// The pane's child window whose name has `name`.
static ImGuiWindow* pane_window(const char* name) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* top = ImGui::FindWindowByName("##designer");
    if (!top) return nullptr;
    const ImGuiID id = ImHashStr("Preview", 0, top->ID);
    ImGuiWindow* pane = nullptr;
    for (ImGuiWindow* w : g.Windows)
        if (w->ParentWindow == top && w->ChildId == id) pane = w;
    if (!pane) return nullptr;
    for (ImGuiWindow* w : g.Windows)
        if (w->ParentWindow == pane && strstr(w->Name, name)) return w;
    return nullptr;
}

// The rectangle of the draw command that samples tex; false if none.
static bool image_rect(ImDrawList* dl, ImTextureID tex, ImVec2* mn, ImVec2* mx) {
    for (const ImDrawCmd& cmd : dl->CmdBuffer) {
        if (cmd.UserCallback || cmd.GetTexID() != tex || cmd.ElemCount == 0) continue;
        *mn = ImVec2(FLT_MAX, FLT_MAX);
        *mx = ImVec2(-FLT_MAX, -FLT_MAX);
        for (unsigned e = 0; e < cmd.ElemCount; e++) {
            const ImDrawVert& v = dl->VtxBuffer[(int)(cmd.VtxOffset + dl->IdxBuffer[(int)(cmd.IdxOffset + e)])];
            *mn = ImMin(*mn, v.pos);
            *mx = ImMax(*mx, v.pos);
        }
        return true;
    }
    return false;
}

// --- the test ---------------------------------------------------------------------------

static const int DW = 1100, DH = 360, KFRAME = 130;

// Frames until fn() or k frames; true if fn().
template <typename F> static bool until(int k, F fn, bool render = true) {
    for (int i = 0; i < k; i++) {
        if (fn()) return true;
        frame(true, render);
    }
    return fn();
}

static int64_t shown_frame() {
    dsg_preview_where w = {};
    return dsg_preview_where_now(ui_preview(), &w) ? w.frame : -1;
}

// Steps the paused session to frame `to` (where.frame: the frames drawn),
// one Step a UI frame.
static void step_to(int64_t to, bool render) {
    for (int i = 0; i < 400 && shown_frame() >= 0 && shown_frame() < to; i++) {
        dsg_preview_step(ui_preview());
        frame(true, render);
    }
}

static int run_test(const char* work) {
    if (!dsg_is_file(YSP_DESIGNER_TEST_FONT)) {
        printf("SKIP: no stroop font at '%s' (YSP_DESIGNER_TEST_FONT)\n", YSP_DESIGNER_TEST_FONT);
        return kSkip;
    }
    const char* want = getenv("YSP_GL_DEVICE");
    if (!want || !want[0]) want = "llvmpipe";
    const char* why = egl_open(want);
    if (why) {
        printf("SKIP: %s ('%s')\n", why, want);
        return kSkip;
    }
    printf("GL_RENDERER: %s\n", g_renderer);
    char err[1024], dir[DSG_PATH_MAX];
    if (!copy_stroop(work, dir, sizeof dir)) {
        printf("FAIL: copy stroop under %s\n", work);
        return 1;
    }
    if (ui_start(err, sizeof err)) {
        printf("FAIL: %s\n", err);
        return 1;
    }
    ui_open(dir);
    frame(true, true);
    CHECK(ui_project() != nullptr, "stroop opened: '%s'", ui_status());
    dsg_preview_settings ps = {};
    dsg_preview_get(ui_preview(), &ps);
    ps.session.width = DW;
    ps.session.height = DH;
    ps.session.hz = 60;
    ps.scale = DSG_PV_SCALE_1TO1;
    dsg_preview_set(ui_preview(), &ps);
    // Paused before the first session: it attaches, draws frame 0, and
    // waits for Step.
    dsg_preview_pause(ui_preview());
    ui_show_aux(true);

    // 1. loaded on the check thread, attached on this one
    uint32_t tex = 0;
    int32_t tw = 0, th = 0;
    CHECK(until(600, [&] { return info().status == DSG_PV_PAUSED && dsg_preview_texture(ui_preview(), &tex, &tw, &th); }),
          "a session attached, paused (status %d)", info().status);
    CHECK(tex != 0 && tw == DW && th == DH, "its texture: %u, %d x %d", tex, tw, th);
    char run1[DSG_PATH_MAX];
    snprintf(run1, sizeof run1, "%s", info().out_dir);
    step_to(KFRAME, true);
    CHECK(shown_frame() == KFRAME, "stepped to frame %d (%lld)", KFRAME, (long long)shown_frame());
    dsg_preview_where w = {};
    dsg_preview_where_now(ui_preview(), &w);
    const int ink = ink_of_row(w.condition);

    // 2. framebuffer 0 against the texture
    const size_t nb = (size_t)DW * DH * 4;
    unsigned char* t1 = (unsigned char*)malloc(nb);
    unsigned char* fbb = (unsigned char*)malloc(nb);
    unsigned char* t2 = (unsigned char*)malloc(nb);
    if (!t1 || !fbb || !t2) {
        printf("FAIL: out of memory\n");
        return 1;
    }
    memset(t1, 0, nb);
    memset(fbb, 0, nb);
    memset(t2, 0, nb);
    CHECK(dsg_preview_texture(ui_preview(), &tex, &tw, &th) && read_texture(tex, DW, DH, t1), "the texture read back");
    ImGuiWindow* disp = pane_window("##display");
    ImVec2 mn(0, 0), mx(0, 0);
    const bool have_rect = disp && image_rect(disp->DrawList, (ImTextureID)tex, &mn, &mx);
    CHECK(have_rect, "the pane drew the texture");
    if (have_rect) {
        CHECK(mx.x - mn.x == (float)DW && mx.y - mn.y == (float)DH && mn.x == floorf(mn.x) && mn.y == floorf(mn.y),
              "1:1 at a whole px: (%.2f, %.2f) to (%.2f, %.2f)", mn.x, mn.y, mx.x, mx.y);
        const ImRect& c = disp->InnerClipRect;
        CHECK(mx.x <= c.Max.x && mx.y <= c.Max.y && mn.x >= c.Min.x && mn.y >= c.Min.y,
              "the whole image shows: (%.0f, %.0f) to (%.0f, %.0f) in (%.0f, %.0f) to (%.0f, %.0f)", mn.x, mn.y, mx.x, mx.y, c.Min.x, c.Min.y,
              c.Max.x, c.Max.y);
        // GL's rows go up from the bottom, ImGui's down from the top
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels((int)mn.x, FBH - (int)mx.y, DW, DH, GL_RGBA, GL_UNSIGNED_BYTE, fbb);
        CHECK(glGetError() == GL_NO_ERROR, "glReadPixels() of framebuffer 0");
        size_t diff = 0, first = nb, alpha_t = 0, alpha_f = 0;
        for (size_t i = 0; i < nb; i++) {
            if (t1[i] != fbb[i]) {
                diff++;
                if (first == nb) first = i;
            }
            if ((i & 3) == 3) {
                alpha_t += t1[i] != 255;
                alpha_f += fbb[i] != 255;
            }
        }
        CHECK(alpha_t == 0 && alpha_f == 0, "alpha 255: %zu texture px and %zu framebuffer px are not", alpha_t, alpha_f);
        CHECK(diff == 0, "framebuffer 0 is the texture's bytes: %zu bytes differ, the first at px (%zu, %zu): %u against %u", diff,
              first / 4 % DW, first / 4 / DW, first < nb ? fbb[first] : 0, first < nb ? t1[first] : 0);
    }

    // 2b. zoom 2, panned: each texel a 2 x 2 square, exactly
    {
        dsg_preview_settings z = {};
        dsg_preview_get(ui_preview(), &z);
        z.scale = DSG_PV_SCALE_ZOOM;
        z.zoom = 2;
        dsg_preview_set(ui_preview(), &z);
        frame(true, true);
        ImGuiWindow* d = pane_window("##display");
        ImGuiIO& io = ImGui::GetIO();
        if (d) {
            // a middle drag of (-37, -21): the pan is not a whole number of texels
            const ImVec2 at(d->Pos.x + 100, d->Pos.y + 60);
            io.AddMousePosEvent(at.x, at.y);
            frame(true, true);
            io.AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
            frame(true, true);
            io.AddMousePosEvent(at.x - 37, at.y - 21);
            frame(true, true);
            io.AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
            frame(true, true);
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            frame(true, true);
        }
        d = pane_window("##display");
        ImVec2 za(0, 0), zb(0, 0);
        const bool zr = d && image_rect(d->DrawList, (ImTextureID)tex, &za, &zb);
        CHECK(zr, "zoom 2: the pane drew the texture");
        if (zr) {
            CHECK(zb.x - za.x == 2.0f * DW && zb.y - za.y == 2.0f * DH && za.x == floorf(za.x) && za.y == floorf(za.y),
                  "zoom 2 at a whole px: (%.2f, %.2f) to (%.2f, %.2f)", za.x, za.y, zb.x, zb.y);
            CHECK(za.x == d->Pos.x - 37.0f && za.y <= d->Pos.y, "panned by the drag: the corner at (%.1f, %.1f) of the area's (%.1f, %.1f)",
                  za.x, za.y, d->Pos.x, d->Pos.y);
            // the part that shows: the image inside the child's clip
            const ImRect& c = d->InnerClipRect;
            const int x0 = (int)ImMax(za.x, c.Min.x), y0 = (int)ImMax(za.y, c.Min.y);
            const int x1 = (int)ImMin(zb.x, c.Max.x), y1 = (int)ImMin(zb.y, c.Max.y);
            const int vw = x1 - x0, vh = y1 - y0;
            CHECK(vw >= 400 && vh >= 200, "zoom 2: %d x %d px show", vw, vh);
            unsigned char* zf = vw > 0 && vh > 0 ? (unsigned char*)malloc((size_t)vw * (size_t)vh * 4) : nullptr;
            if (zf) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(x0, FBH - y1, vw, vh, GL_RGBA, GL_UNSIGNED_BYTE, zf);
                CHECK(glGetError() == GL_NO_ERROR, "zoom 2: glReadPixels() of framebuffer 0");
                size_t zd = 0, zfirst = (size_t)-1;
                for (int y = 0; y < vh; y++)
                    for (int x = 0; x < vw; x++) {
                        // framebuffer rows go up, ImGui's down; the texture's rows go up
                        const int sy = y1 - 1 - y, sx = x0 + x;
                        const int tx = (sx - (int)za.x) / 2, ty = DH - 1 - (sy - (int)za.y) / 2;
                        const unsigned char* f = zf + ((size_t)y * (size_t)vw + (size_t)x) * 4;
                        const unsigned char* t = t1 + ((size_t)ty * DW + (size_t)tx) * 4;
                        if (memcmp(f, t, 4) != 0) {
                            if (zfirst == (size_t)-1) zfirst = (size_t)sy * 100000 + (size_t)sx;
                            zd++;
                        }
                    }
                CHECK(zd == 0, "zoom 2: framebuffer 0 is the texture, 2 x 2 px a texel: %zu px differ, the first at (%zu, %zu)", zd,
                      zfirst == (size_t)-1 ? 0 : zfirst % 100000, zfirst == (size_t)-1 ? 0 : zfirst / 100000);
                free(zf);
            }
        }
        // back to 1:1 for the rest
        z.scale = DSG_PV_SCALE_1TO1;
        z.zoom = 1;
        dsg_preview_set(ui_preview(), &z);
        frame(true, true);
    }

    // 2c. popped out: the second context's framebuffer is the texture
    {
        CHECK(ui_preview_can_pop(), "the viewports' platform: Pop out offered");
        ui_preview_pop(true);
        frame(true, true);
        frame(true, true);
        ImGuiWindow* pw = ImGui::FindWindowByName("Preview##window");
        CHECK(pw && pw->Active && pw->Viewport != ImGui::GetMainViewport() && pbw(pw->Viewport) && pbw(pw->Viewport)->ctx,
              "popped: a window of its own, on a context of its own (%d made)", g_pb_windows);
        ImGuiWindow* pd = nullptr;
        for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
            if (pw && w->ParentWindow == pw && strstr(w->Name, "##display")) pd = w;
        ImVec2 pa(0, 0), pb2(0, 0);
        const bool pr = pd && image_rect(pd->DrawList, (ImTextureID)tex, &pa, &pb2);
        CHECK(pr, "popped: its display drew the texture");
        if (pr && pw && pbw(pw->Viewport)) {
            const ImGuiViewport* vp = pw->Viewport;
            CHECK(pb2.x - pa.x == (float)DW && pb2.y - pa.y == (float)DH && pa.x == floorf(pa.x) && pa.y == floorf(pa.y),
                  "popped, 1:1 at a whole px: (%.2f, %.2f) to (%.2f, %.2f)", pa.x, pa.y, pb2.x, pb2.y);
            // the window's own framebuffer: its rows go up from its height
            const int x0 = (int)(pa.x - vp->Pos.x), y0 = (int)(pa.y - vp->Pos.y);
            const int vh = (int)vp->Size.y;
            PbWindow* w = pbw(pw->Viewport);
            g_cur(g_dpy, w->pb, w->pb, w->ctx);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            memset(fbb, 0, nb);
            glReadPixels(x0, vh - y0 - DH, DW, DH, GL_RGBA, GL_UNSIGNED_BYTE, fbb);
            const bool read_ok = glGetError() == GL_NO_ERROR;
            g_cur(g_dpy, g_pb, g_pb, g_ctx);
            CHECK(read_ok, "popped: glReadPixels() of its framebuffer");
            size_t pd_diff = 0;
            for (size_t i = 0; i < nb; i++) pd_diff += t1[i] != fbb[i];
            CHECK(pd_diff == 0, "popped: its framebuffer is the texture's bytes: %zu bytes differ", pd_diff);
        }
        ui_preview_pop(false);
        frame(true, true);
        frame(true, true);
        CHECK(!ui_preview_popped() && ImGui::GetPlatformIO().Viewports.Size == 1, "popped in: one viewport again (%d)",
              ImGui::GetPlatformIO().Viewports.Size);
    }

    // 3. the word at its place, in its row's ink
    {
        const unsigned char* bg = t1;   // a corner: the background
        int x0 = DW, y0 = DH, x1 = -1, y1 = -1;
        double sum[3] = { 0, 0, 0 };
        for (int y = 0; y < DH; y++)
            for (int x = 0; x < DW; x++) {
                const unsigned char* p = t1 + ((size_t)y * DW + (size_t)x) * 4;
                const int d = abs(p[0] - bg[0]) + abs(p[1] - bg[1]) + abs(p[2] - bg[2]);
                if (d < 48) continue;   // the glyphs' antialiased rims count only where they are strong
                x0 = x < x0 ? x : x0;
                x1 = x > x1 ? x : x1;
                y0 = y < y0 ? y : y0;
                y1 = y > y1 ? y : y1;
                for (int c = 0; c < 3; c++) sum[c] += p[c];
            }
        const bool any = x1 >= x0;
        // rows bottom up: y from the top is DH - 1 - row
        const double cy = any ? DH - 1 - 0.5 * (y0 + y1) : -1;
        const int top = sum[0] >= sum[1] && sum[0] >= sum[2] ? 0 : sum[1] >= sum[2] ? 1 : 2;
        printf("word: rows %d to %d, columns %d to %d, middle row %.1f from the top, ink %d, the strongest channel %d\n", DH - 1 - y1,
               DH - 1 - y0, x0, x1, cy, ink, top);
        CHECK(any && x1 - x0 >= 60 && y1 - y0 >= 30, "a word drawn: %d x %d px", x1 - x0 + 1, y1 - y0 + 1);
        // The left edge: the box's, plus the first capital's side bearing
        // (7 px for B: the seed's first word is BLUE). The middle: the capitals'
        // middle is not the line's, 2 px off here.
        const int left = DW / 2 - 500;
        CHECK(x0 >= left && x0 <= left + 12, "the word starts at the box's left edge, %d px: %d px", left, x0);
        CHECK(fabs(cy - 0.5 * DH) <= 12, "the word's middle is at the display's, row %.1f: %.1f", 0.5 * DH, cy);
        CHECK(ink >= 0 && top == ink, "in its row's ink: row %d, ink %d, the strongest channel %d", w.condition, ink, top);
    }

    // 4. the same frame with no ImGui frame drawn between the steps
    dsg_preview_restart(ui_preview());
    CHECK(until(600, [&] { return strcmp(info().out_dir, run1) != 0 && info().status == DSG_PV_PAUSED && shown_frame() == 1; }),
          "the restart's session shows, paused at frame 1 (status %d, frame %lld)", info().status, (long long)shown_frame());
    step_to(KFRAME, false);
    CHECK(shown_frame() == KFRAME, "stepped again to frame %d (%lld)", KFRAME, (long long)shown_frame());
    CHECK(dsg_preview_texture(ui_preview(), &tex, &tw, &th) && read_texture(tex, DW, DH, t2), "the second texture read back");
    size_t d2 = 0;
    for (size_t i = 0; i < nb; i++) d2 += t1[i] != t2[i];
    CHECK(d2 == 0, "gfx's bytes with and without an ImGui frame first: %zu bytes differ", d2);
    frame(true, true);

    free(t1);
    free(fbb);
    free(t2);
    ui_stop();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}

// --- the bench --------------------------------------------------------------------------

static int cmp_d(const void* a, const void* b) {
    const double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static void report(const char* what, double* v, int n) {
    if (n <= 0) {
        printf("  %-8s n 0\n", what);
        return;
    }
    qsort(v, (size_t)n, sizeof *v, cmp_d);
    const int p99 = (n * 99 + 99) / 100 - 1;
    printf("  %-8s n %5d  median %8.1f  p99 %8.1f  max %8.1f us\n", what, n, v[n / 2], v[p99 < 0 ? 0 : p99], v[n - 1]);
}

static void loadavg(const char* when) {
    char b[128] = "";
    FILE* f = fopen("/proc/loadavg", "rb");
    if (f) {
        const size_t k = fread(b, 1, sizeof b - 1, f);
        b[k] = 0;
        fclose(f);
    }
    printf("load average %s: %s", when, b[0] ? b : "?\n");
}

static int run_bench(const char* project, int hz, int n, bool hidden, int dw, int dh, bool popout) {
    FBW = 1920;
    FBH = 1080;
    const char* want = getenv("YSP_GL_DEVICE");
    const char* why = egl_open(want);
    if (why) {
        printf("no GL: %s ('%s')\n", why, want ? want : "");
        return kSkip;
    }
    char err[1024];
    if (ui_start(err, sizeof err)) {
        printf("FAIL: %s\n", err);
        return 1;
    }
    printf("%s, %d Hz display%s, %d frames%s; GL_RENDERER %s\n", project, hz, dw > 0 ? " (size given)" : "", n,
           hidden ? ", the pane hidden" : "", g_renderer);
    loadavg("before");
    ui_open(project);
    frame(true, true);
    if (!ui_project()) {
        printf("FAIL: open %s: %s\n", project, ui_status());
        return 1;
    }
    dsg_preview_settings ps = {};
    dsg_preview_get(ui_preview(), &ps);
    ps.session.hz = hz;
    if (dw > 0 && dh > 0) {
        ps.session.width = dw;
        ps.session.height = dh;
    }
    printf("  display %d x %d\n", ps.session.width, ps.session.height);
    dsg_preview_set(ui_preview(), &ps);
    ui_show_aux(!hidden);
    // The adoption: no wait for the check thread, as the UI does not wait.
    // Its frames: those with attach work, and the next three (the first
    // drawn frame is the one after the attach ends).
    struct timespec nap = { 0, 2000000 };
    Split adopt[24];
    int n_adopt = 0, after = -1, i = 0;
    for (; i < 3000 && !hidden && after < 3; i++) {
        if (info().status == DSG_PV_FAILED) {
            dsg_issue is = {};
            printf("FAIL: the session failed: %s\n", dsg_preview_issue(ui_preview(), &is) && is.message ? is.message : "?");
            return 1;
        }
        frame(false, true);
        if (g_split.attach > 0) after = 0;
        else if (after >= 0) after++;
        if (after >= 0 && n_adopt < 24) adopt[n_adopt++] = g_split;
        if (after < 0 && (info().status == DSG_PV_WAITING || info().status == DSG_PV_NONE)) nanosleep(&nap, nullptr);
    }
    if (!hidden && info().status != DSG_PV_RUNNING) {
        printf("FAIL: no running session after %d frames (status %d)\n", i, info().status);
        return 1;
    }
    if (popout) {
        // a window of its own, 1920 x 1080 beside the main one: a second
        // monitor's worth, as the user would make it full screen
        ui_preview_pop(true);
        frame(false, true);
        ImGui::SetWindowPos("Preview##window", ImVec2(1920, 0));
        ImGui::SetWindowSize("Preview##window", ImVec2(1920, 1080));
        frame(false, true);
        frame(false, true);
        ImGuiWindow* pw = ImGui::FindWindowByName("Preview##window");
        if (!pw || pw->Viewport == ImGui::GetMainViewport() || ImGui::GetPlatformIO().Viewports.Size != 2) {
            printf("FAIL: the preview did not pop out\n");
            return 1;
        }
        printf("  popped out: %.0f x %.0f at (%.0f, %.0f), %d viewports\n", pw->Size.x, pw->Size.y, pw->Pos.x, pw->Pos.y,
               ImGui::GetPlatformIO().Viewports.Size);
    }
    for (int k = 0; k < n_adopt; k++)
        printf("  adoption frame %d: tick %.1f (attach %.1f), ui_frame %.1f, render %.1f, finish %.1f, total %.1f us\n", k + 1, adopt[k].tick,
               adopt[k].attach, adopt[k].ui, adopt[k].render, adopt[k].finish, adopt[k].total);
    for (int k = 0; k < 60; k++) frame(false, true);
    double* v = (double*)calloc((size_t)n * 7, sizeof *v);
    if (!v) return 1;
    double *tick = v, *attach = v + n, *ui = v + 2 * n, *views = v + 3 * n, *render = v + 4 * n, *finish = v + 5 * n, *total = v + 6 * n;
    int ended = 0;
    for (int k = 0; k < n; k++) {
        frame(false, true);
        tick[k] = g_split.tick;
        attach[k] = g_split.attach;
        ui[k] = g_split.ui;
        views[k] = g_split.ui - g_split.tick;
        render[k] = g_split.render;
        finish[k] = g_split.finish;
        total[k] = g_split.total;
        ended += !hidden && info().status != DSG_PV_RUNNING;
    }
    report("tick", tick, n);
    report("attach", attach, n);
    report("ui_frame", ui, n);
    report("views", views, n);
    report("render", render, n);
    report("finish", finish, n);
    report("total", total, n);
    if (ended) printf("  note: the session was not running in %d of the frames\n", ended);
    loadavg("after");
    free(v);
    ui_stop();
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 5 && strcmp(argv[1], "--bench") == 0) {
        int dw = 0, dh = 0;
        const bool hidden = argc > 5 && strcmp(argv[5], "hidden") == 0;
        const bool popout = argc > 5 && strcmp(argv[5], "popout") == 0;
        if (argc > 5 && !hidden && !popout && sscanf(argv[5], "%dx%d", &dw, &dh) != 2) dw = dh = 0;
        return run_bench(argv[2], atoi(argv[3]), atoi(argv[4]), hidden, dw, dh, popout);
    }
    if (argc < 2) {
        printf("usage: test_designer_preview_gl WORK\n"
               "       test_designer_preview_gl --bench PROJECT HZ FRAMES [hidden | popout | WxH]\n");
        return 2;
    }
    return run_test(argv[1]);
}
