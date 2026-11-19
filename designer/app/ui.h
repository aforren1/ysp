// ui.h - the designer's panels and state, with no SDL and no GL (ui.cpp).
// main.cpp gives it a window, events and a renderer; tests/designer/ui_test.cpp
// drives it headless.
#pragma once
#include <stddef.h>
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

// For the test and the platform layer.
const dsg_project* ui_project();      // NULL when none is open
const dsg_report* ui_report();
SkfField* ui_editor();
const char* ui_text(size_t* n);      // the editor's text as last read
const char* ui_status();
bool ui_dirty();
int ui_checks_run();                 // how many checks have run
bool ui_save();
void ui_jump_to_issue(int i);        // as a click on issue i
