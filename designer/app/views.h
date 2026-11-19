// views.h - the views of the designer's window and what they share
// (docs/designer.md, "Plan of step 4"). ui.cpp owns the window, the
// toolbar, the text view, the issues and the document; each other view is
// one UiView in a file of its own (view_chart.cpp, view_tables.cpp,
// view_panels.cpp), listed once in views.cpp. A view edits only through the
// document (designer/core/doc.h) and keeps its own state in its part of the
// view file (dsg_view_part_get/set, under its id).
#pragma once
#include <stddef.h>
#include "dsg.h"
#include "doc.h"

enum UiSlot {
    UI_SLOT_MAIN = 0,   // the main area: one of these shows, by the switch
    UI_SLOT_SIDE = 1    // the side column: always shown (the panels)
};

struct UiView {
    const char* id;      // "chart": the view file's part key and its "view"
    const char* label;   // "Chart": the switch's label and the child window's
                         // name, so its test path is "//##designer/Chart"
    UiSlot slot;
    // Each may be NULL.
    bool (*init)(char* err, size_t cap);  // once, after ui_init()'s fonts
    void (*shutdown)();
    void (*opened)(const dsg_view* v);    // a project opened; its view state
    void (*closed)();                     // the project closed
    void (*save_view)(dsg_view* v);       // put this view's part into v
    void (*draw)();                       // inside the view's child window
    // How much the view wants to show s (0: not at all). ui_reveal() shows
    // the main view with the highest claim; ties keep the one shown.
    int (*claims)(const dsg_sel* s);
    void (*reveal)(const dsg_sel* s);     // s is selected: bring it into view
    bool (*animating)();                  // draw every frame while true
};

// The views, in the switch's order (views.cpp).
const UiView* const* ui_views(int* n);

// What every view may use.
dsg_doc* ui_doc();                        // never NULL after ui_init()
const dsg_project* ui_project();          // NULL when none is open
const dsg_report* ui_report();            // NULL before the first check
// The JSON Pointer of the value that issue i names, "" when it names none
// (the text did not parse, or a need with no key). Same index as
// ui_report()->issues.
const char* ui_issue_path(int i);
// Selects s and shows the main view that claims it most.
void ui_reveal(const dsg_sel* s);
void ui_set_status(const char* fmt, ...);
// The size of user text (Skribidi), px.
float ui_text_size();
