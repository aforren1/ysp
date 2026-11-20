// te_tables.cpp - the tables' grid editor driven by the Test Engine (step
// 4, part B; docs/designer.md, "The tables"). OWNED BY PART B.
//
//   tables/shows      The switch shows the tables' window and its grid.
//   tables/scroll     A table of 10,000 rows: the engine scrolls to the end
//                     and back with the mouse wheel; each frame lays out
//                     only the visible rows' cells.
//   tables/edit       A double-click edits a cell; the engine's keys type
//                     Arabic text and Enter: one undo step, the bytes as
//                     ytb reads them; Ctrl+Z gives the old bytes back.
//   tables/add_row    A click on a row, then Add row: a copy below it, one
//                     step; Ctrl+Z takes it back.
//   tables/widths     The engine drags a column's border; the width is in
//                     the view file and comes back after another project
//                     was open, which did not show it. A rename keeps it;
//                     undo moves it back.
#include "te_common.h"
#include "ui.h"
#include "views.h"
#include "view_tables.h"
#include "skf.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const char* const kRdk[] = { "pack.json", "experiment.json", "conditions.csv", nullptr };

bool starts(const char* s, const char* head) { return strncmp(s, head, strlen(head)) == 0; }

const char* table_text(const char* source) {
    const int b = dsg_doc_find_buffer(ui_doc(), source);
    size_t n = 0;
    return b > 0 ? dsg_doc_bytes(ui_doc(), b, &n) : "";
}

void open_rdk(ImGuiTestContext* ctx, const char* copy, char* dir, size_t cap) {
    IM_CHECK(te_copy_project("rdk", copy, kRdk, dir, cap));
    te_open(ctx, dir);
    te_show(ctx, "Tables");
    ctx->Yield(2);
}

void test_shows(ImGuiTestContext* ctx) {
    char dir[DSG_PATH_MAX];
    open_rdk(ctx, "tables_shows", dir, sizeof dir);
    IM_CHECK_STR_EQ(ui_main_view(), "tables");
    IM_CHECK(ctx->WindowInfo("//##designer/Tables").ID != 0);
    IM_CHECK(view_tables_grid_window() != nullptr);
    IM_CHECK(ctx->ItemExists(view_tables_cell_id(-1, 0)));
}

// A copy of rdk with a second table of 10,000 rows, listed first.
void big_project(char* dir, size_t cap) {
    IM_CHECK(te_copy_project("rdk", "tables_big", kRdk, dir, cap));
    static const char desc[] =
        "{\"format\": \"ysp-pack-source\", \"version\": 1,\n"
        " \"experiment\": {\"entry\": \"experiment/main.json\", \"source\": \"experiment.json\", \"media_type\": \"application/vnd.ysp.experiment\"},\n"
        " \"resources\": [{\"name\": \"big.pstb\", \"kind\": \"table\", \"source\": \"big.csv\"},\n"
        "               {\"name\": \"conditions.pstb\", \"kind\": \"table\", \"source\": \"conditions.csv\"}]}\n";
    ImVector<char> csv;
    char line[160];
    const char* head = "trial,label,x,y\n";
    for (const char* p = head; *p; p++) csv.push_back(*p);
    for (int i = 0; i < 10000; i++) {
        const int n = snprintf(line, sizeof line, "%d,\xD8\xA7\xD9\x84\xD9\x87\xD8\xAF\xD9\x81 %d,%d,%d\n", i + 1, i + 1, i % 360, (i * 7) % 360);
        for (int k = 0; k < n; k++) csv.push_back(line[k]);
    }
    char path[DSG_PATH_MAX], err[512];
    IM_CHECK(dsg_join(path, sizeof path, dir, "pack.json") == 0 && dsg_write_file(path, desc, sizeof desc - 1, err, sizeof err) == 0);
    IM_CHECK(dsg_join(path, sizeof path, dir, "big.csv") == 0 && dsg_write_file(path, csv.Data, (size_t)csv.Size, err, sizeof err) == 0);
}

void test_scroll(ImGuiTestContext* ctx) {
    char dir[DSG_PATH_MAX];
    big_project(dir, sizeof dir);
    te_open(ctx, dir);
    te_show(ctx, "Tables");
    ctx->Yield(2);
    ImGuiWindow* w = view_tables_grid_window();
    IM_CHECK(w != nullptr);
    const int rows_shown = (int)(w->InnerRect.GetHeight() / 28.0f) + 3;
    int first = -1, last = -1;
    view_tables_rows_last_frame(&first, &last);
    IM_CHECK_EQ(first, 0);
    IM_CHECK_LE(view_tables_layouts_last_frame(), rows_shown * 4);
    // the engine's mouse over the grid, then its wheel
    ctx->MouseMoveToPos(w->InnerRect.GetCenter());
    ctx->MouseWheelY(-5.0f);
    ctx->Yield(2);
    view_tables_rows_last_frame(&first, &last);
    IM_CHECK_GT(first, 0);
    ctx->ScrollToBottom(w->ID);
    ctx->Yield(2);
    view_tables_rows_last_frame(&first, &last);
    IM_CHECK_EQ(last, 9999);
    IM_CHECK_LE(last - first + 1, rows_shown);
    IM_CHECK_LE(view_tables_layouts_last_frame(), rows_shown * 4);
    IM_CHECK(ctx->ItemExists(view_tables_cell_id(9999, 1)));
    ctx->ScrollToTop(w->ID);
    ctx->Yield(2);
    view_tables_rows_last_frame(&first, &last);
    IM_CHECK_EQ(first, 0);
}

void test_edit(ImGuiTestContext* ctx) {
    char dir[DSG_PATH_MAX];
    open_rdk(ctx, "tables_edit", dir, sizeof dir);
    ImVector<char> before;
    const char* t = table_text("conditions.csv");
    before.resize((int)strlen(t) + 1);
    memcpy(before.Data, t, (size_t)before.Size);
    IM_CHECK(starts(t, "coherence,coherent_direction,key_answer\n0.032,0,right\n"));
    const int depth0 = dsg_doc_undo_depth(ui_doc());

    ctx->ItemDoubleClick(view_tables_cell_id(0, 2));
    IM_CHECK(view_tables_field() != nullptr);
    IM_CHECK(skf_focused(view_tables_field()));
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
    // "مرحبا, x": the comma makes the cell quoted
    ctx->KeyChars("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7, x");
    ctx->KeyPress(ImGuiKey_Enter);
    IM_CHECK(view_tables_field() == nullptr);
    t = table_text("conditions.csv");
    IM_CHECK(starts(t, "coherence,coherent_direction,key_answer\n0.032,0,\"\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7, x\"\n0.032,180,left\n"));
    IM_CHECK_EQ(dsg_doc_undo_depth(ui_doc()), depth0 + 1);
    IM_CHECK_STR_EQ(dsg_doc_undo_label(ui_doc()), "Edit cell");
    // the edit reached the check: the pack was built from the document
    IM_CHECK(ui_report() != nullptr);

    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    IM_CHECK_STR_EQ(table_text("conditions.csv"), before.Data);
}

void test_add_row(ImGuiTestContext* ctx) {
    char dir[DSG_PATH_MAX];
    open_rdk(ctx, "tables_add_row", dir, sizeof dir);
    ImVector<char> before;
    const char* t = table_text("conditions.csv");
    before.resize((int)strlen(t) + 1);
    memcpy(before.Data, t, (size_t)before.Size);
    ctx->ItemClick(view_tables_cell_id(1, -1));
    IM_CHECK_EQ(dsg_doc_selection(ui_doc())->row, 1);
    ctx->ItemClick(te_in_child(ctx, "//##designer/Tables", "Add row"));
    t = table_text("conditions.csv");
    IM_CHECK(starts(t, "coherence,coherent_direction,key_answer\n0.032,0,right\n0.032,180,left\n0.032,180,left\n0.064,0,right\n"));
    IM_CHECK_STR_EQ(dsg_doc_undo_label(ui_doc()), "Add row");
    IM_CHECK_EQ(dsg_doc_selection(ui_doc())->row, 2);
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    IM_CHECK_STR_EQ(table_text("conditions.csv"), before.Data);
    IM_CHECK_EQ(dsg_doc_selection(ui_doc())->row, 1);
}

void test_widths(ImGuiTestContext* ctx) {
    char dir[DSG_PATH_MAX], other[DSG_PATH_MAX];
    open_rdk(ctx, "tables_widths", dir, sizeof dir);
    IM_CHECK(te_copy_project("rdk", "tables_widths_other", kRdk, other, sizeof other));
    ctx->TableResizeColumn(view_tables_table_id(), 2, 211.0f);
    ctx->Yield(2);
    IM_CHECK_EQ(view_tables_width("coherent_direction"), 211.0f);

    // Another project, then this one again: the width from the view file.
    te_open(ctx, other);
    char path[DSG_PATH_MAX], err[512];
    char* vf = nullptr;
    size_t n = 0;
    IM_CHECK(dsg_join(path, sizeof path, dir, DSG_VIEW_FILE) == 0 && dsg_read_file(path, &vf, &n, err, sizeof err) == 0);
    IM_CHECK(vf && strstr(vf, "\"coherent_direction\": 211") != nullptr);
    free(vf);
    IM_CHECK_EQ(view_tables_width("coherent_direction"), 0.0f);
    // the other project's grid does not take this one's widths
    te_show(ctx, "Tables");
    ctx->Yield(2);
    IM_CHECK_EQ(view_tables_width("coherent_direction"), 0.0f);
    IM_CHECK_EQ(ImGui::TableFindByID(view_tables_table_id())->Columns[2].WidthRequest, 120.0f);
    te_open(ctx, dir);
    te_show(ctx, "Tables");
    ctx->Yield(2);
    IM_CHECK_EQ(view_tables_width("coherent_direction"), 211.0f);
    ImGuiTable* tb = ImGui::TableFindByID(view_tables_table_id());
    IM_CHECK(tb != nullptr);
    IM_CHECK_EQ(tb->Columns[2].WidthRequest, 211.0f);

    // A rename through the header cell: the width follows the name.
    ctx->ItemDoubleClick(view_tables_cell_id(-1, 1));
    IM_CHECK(view_tables_field() != nullptr);
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
    ctx->KeyChars("direction_deg");
    ctx->KeyPress(ImGuiKey_Enter);
    IM_CHECK(starts(table_text("conditions.csv"), "coherence,direction_deg,key_answer\n"));
    IM_CHECK_EQ(view_tables_width("direction_deg"), 211.0f);
    IM_CHECK_EQ(view_tables_width("coherent_direction"), 0.0f);
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    IM_CHECK(starts(table_text("conditions.csv"), "coherence,coherent_direction,key_answer\n"));
    IM_CHECK_EQ(view_tables_width("coherent_direction"), 211.0f);
}

} // namespace

void te_register_tables(ImGuiTestEngine* e) {
    IM_REGISTER_TEST(e, "tables", "shows")->TestFunc = test_shows;
    IM_REGISTER_TEST(e, "tables", "scroll")->TestFunc = test_scroll;
    IM_REGISTER_TEST(e, "tables", "edit")->TestFunc = test_edit;
    IM_REGISTER_TEST(e, "tables", "add_row")->TestFunc = test_add_row;
    IM_REGISTER_TEST(e, "tables", "widths")->TestFunc = test_widths;
}
