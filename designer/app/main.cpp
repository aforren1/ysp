// main.cpp - the designer's window: SDL3, GL ES 3.0 and Dear ImGui's
// backends around ui.cpp, and the redraw rule (docs/designer.md, "Redraw
// rule"). Usage: ysp_designer [PROJECT]
#include "ui.h"
#include "skf.h"
#include "skt.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "imgui_internal.h"   // InputEventsQueue: see the loop

#include "ysp/gfx.h"

#include <SDL3/SDL.h>
#include <GLES3/gl3.h>

#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace {

struct App {
    SDL_Window* win = nullptr;
    SDL_GLContext gl = nullptr;
    Uint32 dialog_event = 0;   // the folder dialog's answer, on the main thread
    Uint32 check_event = 0;    // a result of the check thread is ready
    Uint32 run_event = 0;      // a run's watch thread saw progress (step 6)
    dsg_redraw redraw = {};
    bool quit = false;
    // The first gfx on the window's context (docs/designer.md, "Part S as
    // built"): its caps let the check thread record a session's gfx, and the
    // sessions take its programs (desc.share). Open from before ui_init()
    // until after ui_shutdown().
    ygfx_gfx gfx = {};
    ygfx_gl_caps caps = {};
    bool have_gfx = false;
    char player[DSG_PATH_MAX] = "";   // the ysp_player beside the designer
};
App A;

dsg_gl_fn gl_proc(void*, const char* name) { return (dsg_gl_fn)SDL_GL_GetProcAddress(name); }

int64_t now_ns(void*) { return (int64_t)SDL_GetTicksNS(); }

// Opens the first gfx; without it the preview's sessions draw on gfx's null
// backend and the pane shows no image, as in the headless tests. 64 x 64 into
// its own output texture: it never draws, so it never touches the window.
void open_first_gfx(UiPlatform* plat) {
    ygfx_desc d;
    memset(&d, 0, sizeof d);
    d.gl_proc = gl_proc;
    d.width = 64;
    d.height = 64;
    d.output_texture = true;
    if (ygfx_open(&A.gfx, &d) && ygfx_gl_caps_get(&A.gfx, &A.caps) == YGFX_OK) {
        A.have_gfx = true;
        plat->gl_caps = &A.caps;
        plat->gl_share = &A.gfx;
        plat->gl_proc = gl_proc;
        plat->gl_proc_ctx = nullptr;
    } else {
        fprintf(stderr, "ysp_designer: no gfx on the window's GL context (%s): the preview shows no image\n", ygfx_error(&A.gfx));
        ygfx_close(&A.gfx);
    }
}

// SDL may call a dialog's callback on another thread, and the UI is not
// thread safe: the answer goes through the event queue, which also wakes
// the loop's wait.
void SDLCALL dialog_done(void*, const char* const* files, int) {
    SDL_Event e;
    SDL_zero(e);
    e.type = A.dialog_event;
    if (!files) {
        e.user.code = 1;
        e.user.data1 = SDL_strdup(SDL_GetError());
    } else if (files[0]) {
        e.user.code = 0;
        e.user.data1 = SDL_strdup(files[0]);
    } else {
        return;   // cancelled
    }
    SDL_PushEvent(&e);
}

bool pick_folder(void*) {
    // On a Linux desktop with no xdg-desktop-portal and no zenity the
    // callback comes at once with an error (designer_ui_probe.md, part 1,
    // section 6): ui_dialog_failed() then points at the typed path.
    SDL_ShowOpenFolderDialog(dialog_done, nullptr, A.win, nullptr, false);
    return true;
}

// On the check thread. SDL's event queue is thread safe, and the event wakes
// SDL_WaitEvent(); the frame it brings takes the result (ui_frame()).
void SDLCALL check_done(void*) {
    SDL_Event e;
    SDL_zero(e);
    e.type = A.check_event;
    SDL_PushEvent(&e);
}

// On a run's watch thread: wakes the loop without counting as input, so the
// frame comes when ui_run_wait_ms() says (DSG_RUN_REDRAW_HZ at most).
void SDLCALL run_progress(void*) {
    SDL_Event e;
    SDL_zero(e);
    e.type = A.run_event;
    SDL_PushEvent(&e);
}

void set_title(void*, const char* t) { SDL_SetWindowTitle(A.win, t); }
void quit(void*) { A.quit = true; }

void handle(const SDL_Event& e) {
    // not input: the loop's wait asks ui_run_wait_ms() when to draw
    if (e.type == A.run_event) return;
    // The preview's keys are physical (scancodes), as the player's choices
    // are, so they come from SDL, not from ImGui's layout-mapped keys. A key
    // down that the preview takes does not reach ImGui; a key up always
    // does, so a key that ImGui saw go down (Ctrl before a click on the
    // display) does not stay down there.
    if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) &&
        ui_preview_key((int)e.key.scancode, e.type == SDL_EVENT_KEY_DOWN, e.key.repeat) && e.type == SDL_EVENT_KEY_DOWN) {
        dsg_redraw_event(&A.redraw);
        return;
    }
    if (e.type == SDL_EVENT_TEXT_INPUT && ui_preview_has_keyboard()) {
        dsg_redraw_event(&A.redraw);
        return;
    }
    if (e.type == SDL_EVENT_WINDOW_MINIMIZED || e.type == SDL_EVENT_WINDOW_HIDDEN) ui_set_window_shown(false);
    else if (e.type == SDL_EVENT_WINDOW_RESTORED || e.type == SDL_EVENT_WINDOW_SHOWN || e.type == SDL_EVENT_WINDOW_MAXIMIZED)
        ui_set_window_shown(true);
    if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(A.win))) {
        if (ui_request_quit()) A.quit = true;
    } else if (e.type == SDL_EVENT_TEXT_EDITING) {
        // the IME's composition goes to the focused Skribidi field, not
        // through ImGui, whose SDL3 backend ignores it
        skf_compose(e.edit.text ? e.edit.text : "", e.edit.start);
    } else if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
        ui_open(e.drop.data);
    } else if (e.type == A.dialog_event) {
        if (e.user.code == 0) ui_open((const char*)e.user.data1);
        else ui_dialog_failed((const char*)e.user.data1);
        SDL_free(e.user.data1);
    }
    ImGui_ImplSDL3_ProcessEvent(&e);
    dsg_redraw_event(&A.redraw);
}

void draw_frame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    skt_frame_begin();
    ui_frame();
    skt_frame_end();   // new glyphs, before ImGui::Render() collects the textures
    ImGui::Render();
    ImGuiIO& io = ImGui::GetIO();
    // The preview's gfx binds its texture's framebuffer while it draws, in
    // ui_frame(); gfx 0.23.0 binds 0 again at its end, and this makes sure
    // that ImGui never draws into the preview's texture.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x), (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y));
    glClearColor(0.12f, 0.12f, 0.12f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#ifndef __EMSCRIPTEN__
    SDL_GL_SwapWindow(A.win);
#endif
    dsg_redraw_drawn(&A.redraw);
    A.redraw.work = ui_work_pending();
}

int queued() { return ImGui::GetCurrentContext()->InputEventsQueue.Size; }

#ifdef __EMSCRIPTEN__
void web_iter() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) handle(e);
    if (dsg_redraw_due(&A.redraw, queued(), ui_animating())) draw_frame();
}
#endif

} // namespace

int main(int argc, char** argv) {
#ifndef __EMSCRIPTEN__
    // the EGL path ysp/gfx.h's other Linux users take, and the GL ES 3.0 a
    // browser gives
    SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
    // the Skribidi field draws the composition itself (SDL_EVENT_TEXT_EDITING)
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "ysp_designer: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    A.dialog_event = SDL_RegisterEvents(3);
    A.check_event = A.dialog_event + 1;
    A.run_event = A.dialog_event + 2;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    A.win = SDL_CreateWindow("ysp designer", 1400, 900, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!A.win) {
        fprintf(stderr, "ysp_designer: SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    A.gl = SDL_GL_CreateContext(A.win);
    if (!A.gl) {
        fprintf(stderr, "ysp_designer: SDL_GL_CreateContext: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(A.win, A.gl);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // the window layout lives in the project's view file (format D13)
    io.IniFilename = nullptr;
    // a blinking caret is an animation: an idle window would draw twice a second
    io.ConfigInputTextCursorBlink = false;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOpenGL(A.win, A.gl);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    char font_dir[DSG_PATH_MAX];
#ifdef __EMSCRIPTEN__
    snprintf(font_dir, sizeof font_dir, "/fonts");
#else
    const char* base = SDL_GetBasePath();
    snprintf(font_dir, sizeof font_dir, "%sfonts", base ? base : "");
#endif
    UiPlatform plat = {};
    plat.pick_folder = pick_folder;
    plat.set_title = set_title;
    plat.quit = quit;
    plat.wake = check_done;
    plat.now_ns = now_ns;
#ifndef __EMSCRIPTEN__
    // Step 6 (decision 5): the player that CMake put beside the designer.
    // SDL_GetBasePath() is absolute and ends in a separator, as Part X's
    // start with a working folder needs.
    if (base) {
#ifdef _WIN32
        snprintf(A.player, sizeof A.player, "%sysp_player.exe", base);
#else
        snprintf(A.player, sizeof A.player, "%sysp_player", base);
#endif
        plat.player_path = A.player;
    }
    plat.wake_run = run_progress;
#endif
    // The first gfx, on this thread and the window's context, before
    // ui_init() starts the check thread: the sessions that the thread loads
    // record their gfx with its caps. Its open is also the process's first
    // use of ysp/gfx.h, so it builds gfx's NOISE table before any other
    // thread exists (dsg_worker_start() builds it again, a no-op then).
    open_first_gfx(&plat);
    char err[512];
    // ui_init() starts the check thread, so it comes after everything that
    // the thread's wake-up uses (the event type) and after the GL context.
    if (!ui_init(font_dir, &plat, err, sizeof err)) {
        fprintf(stderr, "ysp_designer: %s\n", err);
        return 1;
    }
    if (argc > 1) ui_open(argv[1]);
    dsg_redraw_event(&A.redraw);

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(web_iter, 0, true);
#else
    while (!A.quit) {
        SDL_Event e;
        // Redraw on input only: block until an event unless a frame is due.
        // ImGui applies one key or button change a frame, so a frame is due
        // while its input queue holds events (designer_ui_probe.md, part 1).
        // A run's progress brings a frame at most DSG_RUN_REDRAW_HZ times a
        // second (decision 9): the wait times out when one is due.
        int wait = dsg_redraw_wait_ms(&A.redraw, queued(), ui_animating());
        const int run_wait = ui_run_wait_ms();
        if (run_wait >= 0 && (wait < 0 || run_wait < wait)) wait = run_wait;
        const bool got = wait < 0 ? SDL_WaitEvent(&e) : wait > 0 ? SDL_WaitEventTimeout(&e, wait) : SDL_PollEvent(&e);
        if (got) {
            handle(e);
            while (SDL_PollEvent(&e)) handle(e);
        }
        if (!A.quit && (dsg_redraw_due(&A.redraw, queued(), ui_animating()) || ui_run_wait_ms() == 0)) draw_frame();
    }
    // the sessions are detached and closed by now; the first gfx goes with
    // the context current
    ui_shutdown();
    if (A.have_gfx) ygfx_close(&A.gfx);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(A.gl);
    SDL_DestroyWindow(A.win);
    SDL_Quit();
#endif
    return 0;
}
