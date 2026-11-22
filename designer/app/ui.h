// ui.h - the designer's window and state, with no SDL and no GL (ui.cpp).
// main.cpp gives it a window, events and a renderer; tests/designer/ui_test.cpp
// drives it headless.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "dsg.h"
#include "preview.h"

struct SkfField;

// What the UI asks of the platform layer.
struct UiPlatform {
    void* ctx;
    // Shows a folder picker; its answer comes through ui_open() or
    // ui_dialog_failed(). Returns false when there is no dialog to show.
    bool (*pick_folder)(void* ctx);
    void (*set_title)(void* ctx, const char* title);
    void (*quit)(void* ctx);
    // A result of the check thread is ready. Called on that thread: post an
    // event that wakes the loop (the loop then draws the frame that takes
    // it). NULL: none (the tests wait with ui_wait_checks()).
    void (*wake)(void* ctx);
    // Step 5. The UI's clock, ns (main.cpp: SDL_GetTicksNS()); NULL: ImGui's
    // time, which the tests drive with io.DeltaTime.
    int64_t (*now_ns)(void* ctx);
    // The GL gfx that main.cpp opened on the window's context before
    // ui_init() (ygfx_gl_caps_get() of it, and the gfx itself), and the GL
    // loader: the preview's sessions draw with them (preview.h,
    // dsg_preview_gl). Kept by the caller, unchanged, until ui_shutdown()
    // returns. gl_caps NULL (the headless tests): the sessions draw on gfx's
    // null backend and the pane shows no image.
    const struct ygfx_gl_caps* gl_caps;
    struct ygfx_gfx* gl_share;
    dsg_gl_fn (*gl_proc)(void* ctx, const char* name);
    void* gl_proc_ctx;
    // Step 6. The ysp_player beside the designer, absolute (main.cpp:
    // SDL_GetBasePath() and "ysp_player", ".exe" on Windows). NULL: Run
    // says that there is no player (the web; the tests that start none).
    // Kept by the caller until ui_shutdown() returns.
    const char* player_path;
    // On a run's watch thread (dsg_run_desc.wake) when the player's log or
    // trials.csv grew or the player ended: post an event that wakes the
    // loop and is not input, so it brings no settle frames. The loop then
    // draws when ui_run_wait_ms() says, at most DSG_RUN_REDRAW_HZ frames a
    // second. NULL: none (the tests call ui_run_wait_ms() themselves).
    void (*wake_run)(void* ctx);
};

bool ui_init(const char* font_dir, const UiPlatform* platform, char* err, size_t cap);
void ui_shutdown();

// Opens a project folder or description (a typed path, the command line, a
// dropped file, the dialog's answer). With unsaved changes it asks first.
void ui_open(const char* path);
// The folder dialog could not open: the typed path is the way (docs/designer.md).
void ui_dialog_failed(const char* why);
// The window's close button or the platform's quit: true to quit now, false
// when it asks about unsaved changes first (and quits through the platform
// layer after).
bool ui_request_quit();

// The panels, between ImGui::NewFrame() and ImGui::Render().
void ui_frame();
// Work that wants another frame: a check after an edit, a scroll to restore.
bool ui_work_pending();
// A view animates: draw every frame (the redraw rule).
bool ui_animating();

// Step 5, the preview's input. main.cpp gives each key event's SDL
// scancode, before ImGui sees it; true: the preview took it, so ImGui does
// not get it. Repeats are taken while the preview has the keyboard and are
// not passed to the session.
bool ui_preview_key(int scancode, bool down, bool repeat);
// The preview has the keyboard: main.cpp keeps text input from ImGui too.
bool ui_preview_has_keyboard();
// The window is shown (not minimized or hidden); while false the preview
// pauses.
void ui_set_window_shown(bool shown);

// Step 6. A run's player may still run: started, and not yet seen to end.
// ui_request_quit() asks first while this is true (Stop the run, Leave it
// running, Cancel).
bool ui_run_active();
// How long the loop may wait before it draws a frame for a run's progress,
// ms: -1 none is due (block on events), 0 now. A run's wake-up makes one
// due, DSG_RUN_REDRAW_HZ times a second at most; every frame drawn takes
// the run's progress and clears it.
int ui_run_wait_ms();
// Blocks until every Build posted so far has run, so the next frame takes
// its result (the tests, as ui_wait_checks()).
bool ui_wait_builds(int64_t timeout_ns);

// For the test and the platform layer.
const dsg_project* ui_project();      // NULL when none is open
// The merged report (step 6): the check's issues, then the last Build's
// (DSG_FROM_BUILD), then a failed run's (DSG_FROM_PLAYER_RUN). status,
// built, have_pack and rig are the check's; n_errors counts all.
const dsg_report* ui_report();
SkfField* ui_editor();
const char* ui_text(size_t* n);      // the editor's text as last read
const char* ui_status();
bool ui_dirty();
int ui_checks_run();                 // how many check results were taken
// Blocks until every check posted so far has run on the check thread, so
// the next frame takes its result: the tests' frames call it first.
bool ui_wait_checks(int64_t timeout_ns);
bool ui_save();
void ui_jump_to_issue(int i);        // as a click on issue i
// The main view shown: "text", "chart", "tables" (views.h ids).
const char* ui_main_view();
bool ui_show_view(const char* id);   // false: no such main view
