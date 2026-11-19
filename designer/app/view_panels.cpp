// view_panels.cpp - the property panels (step 4, part C; docs/designer.md,
// "Plan of step 4"). OWNED BY PART C. A stub: it names the selection and
// the schema's fields of a stimulus.
#include "views.h"
#include "schema.h"

#include "imgui.h"

namespace {

void draw() {
    const dsg_sel* s = dsg_doc_selection(ui_doc());
    if (s->buf != DSG_BUF_DEF) {
        ImGui::TextDisabled("Nothing selected.");
        return;
    }
    ImGui::TextDisabled("The panels are not built yet (step 4, part C).");
    ImGui::Text("Selected: %s", s->path[0] ? s->path : "(the definition)");
}

} // namespace

extern const UiView ui_view_panels = {
    "panels", "Properties", UI_SLOT_SIDE,
    nullptr, nullptr, nullptr, nullptr, nullptr,
    draw, nullptr, nullptr, nullptr,
};
