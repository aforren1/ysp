// te_common.h - what the Test Engine runner (te_test.cpp) gives the parts'
// test files (te_chart.cpp, te_tables.cpp, te_panels.cpp, te_preview.cpp,
// te_run.cpp, te_wrap.cpp).
// Shared: a part adds its tests in its own file and does not change this
// one.
#pragma once
#include "imgui.h"
#include "dsg.h"

struct ImGuiTestEngine;
struct ImGuiTestContext;

// A fresh copy of tests/player/NAME in the work folder as COPY (its view
// file removed), with the files listed (NULL-ended); the folder in out.
bool te_copy_project(const char* name, const char* copy, const char* const* files, char* out, size_t cap);
// Opens folder through the path field as a person types it; with unsaved
// changes, Discard answers the question.
void te_open(ImGuiTestContext* ctx, const char* folder);
// An item in a child window: the engine's item paths do not go into child
// windows, so the child's ID seeds the item's.
ImGuiID te_in_child(ImGuiTestContext* ctx, const char* window, const char* item);
bool te_modal_open();
// Shows a main view by a click on its switch ("Text", "Chart", "Tables").
void te_show(ImGuiTestContext* ctx, const char* label);
// With --viewports (2026-11-23): the mock platform's OS window of vp
// minimized or not, and UiPlatform.fullscreen's calls and its last "on".
void te_viewport_minimize(ImGuiViewport* vp, bool on);
int te_fullscreen_calls(bool* on);

// Each part registers its tests under its own category.
void te_register_chart(ImGuiTestEngine* e);    // "chart"
void te_register_tables(ImGuiTestEngine* e);   // "tables"
void te_register_panels(ImGuiTestEngine* e);   // "panels"
void te_register_preview(ImGuiTestEngine* e);  // "preview" (step 5, part B)
// "run" (step 6, part V). Sets XDG_CONFIG_HOME (Windows: APPDATA) to a
// folder of the work folder and YSP_DESIGNER_TEST before any frame.
void te_register_run(ImGuiTestEngine* e);
// The player that UiPlatform.player_path names: a buffer that te_run.cpp
// fills per test (the double's name, or R's fake_player).
const char* te_run_player();
// Removes the config folder that te_register_run() made under HOME, if any
// (a work folder under /tmp, which yrt_user_dir() refuses). After the tests.
void te_run_cleanup();
// With the Run view shown: a simulated run of the open project started by
// its Run, with R's fake_player running script; true once the script's
// "out armed" line came (te_run.cpp). Then a Stop that the script ignores
// ("term ignore"), and frames until the run offers Kill; false on Windows,
// where a --sim run has no Stop.
bool te_run_start_sim(ImGuiTestContext* ctx, const char* script);
bool te_run_offer_kill(ImGuiTestContext* ctx);
// "wrap" (2026-11-23): the rows that wrap at narrow widths (te_wrap.cpp).
void te_register_wrap(ImGuiTestEngine* e);
