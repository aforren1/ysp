// te_common.h - what the Test Engine runner (te_test.cpp) gives the parts'
// test files (te_chart.cpp, te_tables.cpp, te_panels.cpp, te_preview.cpp).
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

// Each part registers its tests under its own category.
void te_register_chart(ImGuiTestEngine* e);    // "chart"
void te_register_tables(ImGuiTestEngine* e);   // "tables"
void te_register_panels(ImGuiTestEngine* e);   // "panels"
void te_register_preview(ImGuiTestEngine* e);  // "preview" (step 5, part B)
