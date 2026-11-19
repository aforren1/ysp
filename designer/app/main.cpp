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
    dsg_redraw redraw = {};
    bool quit = false;
};
App A;

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

void set_title(void*, const char* t) { SDL_SetWindowTitle(A.win, t); }
void quit(void*) { A.quit = true; }

void handle(const SDL_Event& e) {
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
    if (dsg_redraw_due(&A.redraw, queued(), false)) draw_frame();
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
    A.dialog_event = SDL_RegisterEvents(1);
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
    UiPlatform plat = { nullptr, pick_folder, set_title, quit };
    char err[512];
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
        const int wait = dsg_redraw_wait_ms(&A.redraw, queued(), false);
        if (wait < 0 ? SDL_WaitEvent(&e) : SDL_PollEvent(&e)) {
            handle(e);
            while (SDL_PollEvent(&e)) handle(e);
        }
        if (!A.quit && dsg_redraw_due(&A.redraw, queued(), false)) draw_frame();
    }
    ui_shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(A.gl);
    SDL_DestroyWindow(A.win);
    SDL_Quit();
#endif
    return 0;
}
