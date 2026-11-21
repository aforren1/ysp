// view_preview.cpp - the preview's pane (docs/designer.md, "Plan of step
// 5"). OWNED BY PART B (designer step 5).
//
// STUB from part S: the pane registers in the AUX slot and says that the
// preview is not built. It reads nothing of the view file: the part
// "preview" is ui.cpp's (views.h).
#include "views.h"

#include "imgui.h"

namespace {

void draw() { ImGui::TextDisabled("Preview not built."); }

} // namespace

extern const UiView ui_view_preview = {
    "preview", "Preview", UI_SLOT_AUX,
    nullptr, nullptr, nullptr, nullptr, nullptr,
    draw, nullptr, nullptr, nullptr, nullptr, nullptr,
};
