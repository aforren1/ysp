// view_chart.cpp - the state chart (step 4, part A; docs/designer.md, "Plan
// of step 4"). OWNED BY PART A. A stub: it shows the templates that have
// states and claims their states and exits.
#include "views.h"
#include "chart.h"

#include "imgui.h"

#include <string.h>

namespace {

// A state, an exit of a state or a shared exit of a template.
int claims(const dsg_sel* s) {
    if (s->buf != DSG_BUF_DEF || !dsg_path_within(s->path, "/trials") || s->path[7] != '/') return 0;
    const char* slash = strchr(s->path + 8, '/');   // after the template's name
    return slash && (dsg_path_within(slash, "/states") || dsg_path_within(slash, "/exits")) ? 2 : 0;
}

void draw() {
    bool stale = false;
    const yjs_value* root = dsg_doc_shown(ui_doc(), &stale);
    if (!root) {
        ImGui::TextDisabled("The definition has not parsed yet.");
        return;
    }
    ImGui::TextDisabled("The chart is not built yet (step 4, part A). Templates with states:");
    const yjs_value* trials = yjs_get(root, "trials");
    for (uint32_t i = 0; trials && trials->type == YJS_OBJECT && i < trials->n; i++)
        if (yjs_get(trials->u.members[i].value, "states")) ImGui::BulletText("%s", trials->u.members[i].key);
}

} // namespace

extern const UiView ui_view_chart = {
    "chart", "Chart", UI_SLOT_MAIN,
    nullptr, nullptr, nullptr, nullptr, nullptr,
    draw, claims, nullptr, nullptr,
};
