// ui.h - the designer's window and state, with no SDL and no GL (ui.cpp).
// main.cpp gives it a window, events and a renderer; tests/designer/ui_test.cpp
// drives it headless.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "dsg.h"

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

// For the test and the platform layer.
const dsg_project* ui_project();      // NULL when none is open
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
