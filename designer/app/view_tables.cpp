// view_tables.cpp - the tables' grid editor (step 4, part B; docs/designer.md,
// "Plan of step 4"). OWNED BY PART B. A stub: it lists the table buffers.
#include "views.h"
#include "csv.h"

#include "imgui.h"

namespace {

int claims(const dsg_sel* s) {
    return s->buf > DSG_BUF_DEF && dsg_doc_buffer_kind(ui_doc(), s->buf) == DSG_KIND_TABLE ? 2 : 0;
}

void draw() {
    dsg_doc* d = ui_doc();
    ImGui::TextDisabled("The table editor is not built yet (step 4, part B). Tables open:");
    for (int i = 0; i < dsg_doc_buffer_count(d); i++)
        if (dsg_doc_buffer_kind(d, i) == DSG_KIND_TABLE) ImGui::BulletText("%s", dsg_doc_buffer_name(d, i));
}

} // namespace

extern const UiView ui_view_tables = {
    "tables", "Tables", UI_SLOT_MAIN,
    nullptr, nullptr, nullptr, nullptr, nullptr,
    draw, claims, nullptr, nullptr,
};
