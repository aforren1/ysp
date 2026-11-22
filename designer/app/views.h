// views.h - the views of the designer's window and what they share
// (docs/designer.md, "Plan of step 4" and "Plan of step 5"). ui.cpp owns the
// window, the toolbar, the text view, the issues, the document and the
// preview; each other view is one UiView in a file of its own
// (view_chart.cpp, view_tables.cpp, view_panels.cpp, view_preview.cpp),
// listed once in views.cpp. A view edits only through the document
// (designer/core/doc.h) and keeps its own state in its part of the view file
// (dsg_view_part_get/set, under its id). Frozen during step 5 and step 6
// (part S).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "dsg.h"
#include "doc.h"
#include "preview.h"

enum UiSlot {
    UI_SLOT_MAIN = 0,   // the main area: one of these shows, by the switch
    UI_SLOT_SIDE = 1,   // the side column: always shown (the panels)
    // Step 5: a pane at the right of the main area, beside whichever main
    // view shows (so the chart and the preview show at once). Step 6: each
    // AUX view has its own toolbar check box ("//##designer/aux/LABEL"); the
    // pane shows while one is ticked, and the ticked views stack in it, each
    // a child window named by its label ("//##designer/Preview",
    // "//##designer/Run"). The view file keeps each one's "open" in its
    // part ("preview", "run").
    UI_SLOT_AUX = 2
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
    // The selection that issue i (ui_report()->issues[i]) names in this
    // view's own data, such as a table's cell; false when it names none.
    // ui_jump_to_issue() takes the first view's answer before the issue's
    // path in the definition, and reveals it as above.
    bool (*issue_sel)(int i, dsg_sel* out);
    // Step 5: opens a file that is not in the project (the preview's
    // trials.csv) read-only; false when the view cannot. ui_open_file()
    // asks each view in turn.
    bool (*open_file)(const char* path);
};

// The views, in the switch's order (views.cpp).
const UiView* const* ui_views(int* n);

// What every view may use: the calls below, and three of ui.h's, which
// change what the window shows as a person's action would:
// ui_show_view() (show a main view, such as the text at a key),
// ui_editor() (the text view's field, to put its caret) and
// ui_jump_to_issue() (as a click on an issue in the issues panel).
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

// Step 5, the preview (preview.h). ui.cpp makes it, feeds it the check's
// and the session jobs' results and ticks it once a frame before the views
// draw; a view calls its controls and reads its state. Never NULL after
// ui_init(). The view file's part "preview" is ui.cpp's: it reads the part
// into dsg_preview_set() at an open and writes dsg_preview_get() with the
// view file, so no view reads or writes that part.
dsg_preview* ui_preview();
// The GL context the sessions draw on (UiPlatform's): caps NULL headless.
const dsg_preview_gl* ui_preview_gl();
// The UI's clock this frame, ns: UiPlatform.now_ns, or ImGui's time when
// the platform gives none (the tests, which set io.DeltaTime).
int64_t ui_now_ns();
// Where the shown session is (the chart's ring and Follow), or NULL when no
// session shows. Each call reads it again; the strings stay valid until a
// call that changes the preview (preview.h, "Pointers").
const dsg_preview_where* ui_preview_where();
// The Preview view in the AUX pane, as its toolbar check box shows and hides
// it (step 5's names: the Preview was the pane's only view).
bool ui_aux_open();
void ui_show_aux(bool open);
// Step 6: any AUX view by its id ("preview", "run"); false for no such view.
bool ui_aux_view_open(const char* id);
void ui_show_aux_view(const char* id, bool open);
// Opens a file outside the project read-only in the first view whose
// open_file hook takes it, and shows that view; false when none does.
bool ui_open_file(const char* path);

// Step 6, Build and Run (docs/designer.md, "Plan of step 6"). ui.cpp owns
// the builder (dsg_builder_*), the run (dsg_run_*) and the run's settings,
// which it keeps in the view file's part "run". The Run view (part V) shows
// them and calls these, which do what the toolbar's buttons do.
//
// Build: Save, then a Build of the files into dsg_build_path(); false when
// refused (no project, a Build or a run goes, Save failed), the status says
// why. Disabled in the toolbar as ui_can_build() says.
bool ui_build();
bool ui_can_build();
bool ui_build_busy();                    // a Build waits or runs
// The last Build's result since the open, NULL: none. Valid until the frame
// that takes the next one.
const dsg_build_result* ui_build_last();
// dsg_build_stale() of the last Build for the document's revision (a stat
// a source: once a second at most).
bool ui_build_stale();
// Run: enabled when a project is open, the check of the document's current
// revision came back with no error, and no Build or run goes. Run builds
// first when the pack is stale, then starts the player with the settings
// (decision 11's folder when out_dir is ""). false when refused; the status
// says why. A real run (not sim) closes the preview's session until it
// ends (decision 9).
bool ui_can_run();
bool ui_run_start();
// Stop: dsg_run_stop()'s answer (0: Kill now). Kill: only after a Stop.
int  ui_run_stop();
void ui_run_kill();
// The last run since the open (it may have ended), NULL: none. Read it with
// dsg_run_get_info() and dsg_run_issue(); valid until the next frame.
const dsg_run* ui_run();
// The run's settings, which the Run view edits in place. A change of rig
// posts a check with the new profile at the next frame; the next Run takes
// the rest.
dsg_run_settings* ui_run_settings();
// A check of the current text now, though nothing changed (a rig profile's
// file changed: dsg_rig_changed()).
void ui_check_again();
