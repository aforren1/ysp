// view_run.cpp - the Run view in the AUX pane (docs/designer.md, "The
// views" of step 6): the rig profile, the last Build, the run's settings,
// its output. OWNED BY PART V.
//
// Part S's stub: it says that it is not built yet. Part V replaces this
// file; ui.cpp keeps the settings (ui_run_settings()) and does Build, Run,
// Stop and Kill (views.h), so the view only shows and edits.
#include "views.h"

#include "imgui.h"

namespace {

void draw() { ImGui::TextWrapped("The Run view is not built yet (designer step 6, part V)."); }

} // namespace

extern const UiView ui_view_run = {
    "run", "Run", UI_SLOT_AUX, nullptr, nullptr, nullptr, nullptr, nullptr, draw, nullptr, nullptr, nullptr, nullptr, nullptr,
};
