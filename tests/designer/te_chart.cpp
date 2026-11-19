// te_chart.cpp - the state chart driven by the Test Engine (step 4, part A;
// docs/designer.md, "The chart"). OWNED BY PART A.
//
//   chart/drag      A drag moves a state's box; the definition's bytes do not
//                   change, and the view file keeps the position.
//   chart/connect   A drag from a state's port to another state makes an exit.
//   chart/issue     A click on an issue of a state shows the chart with the
//                   state selected and marked.
//   chart/delete    A click selects a state (the panels get its path); Delete
//                   removes it and Ctrl+Z restores it.
//   chart/rename    A double click opens the name field; keys typed with the
//                   engine and Enter rename the state, its references and its
//                   position; Ctrl+Z after it reaches the document.
#include "te_common.h"
#include "ui.h"
#include "views.h"
#include "skf.h"

#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const char* const kFiles[] = { "pack.json", "experiment.json", nullptr };
const char* const kChart = "//##designer/Chart";

// A copy of bimanual, opened, with the chart shown. Its first template in
// byte order is "reach": begin, hold_start, go.
bool open_bimanual(ImGuiTestContext* ctx, const char* copy) {
    char dir[DSG_PATH_MAX];
    if (!te_copy_project("bimanual", copy, kFiles, dir, sizeof dir)) return false;
    te_open(ctx, dir);
    te_show(ctx, "Chart");
    return strcmp(ui_main_view(), "chart") == 0;
}

ImGuiID box(ImGuiTestContext* ctx, const char* state) {
    char p[256];
    snprintf(p, sizeof p, "%s/##box", state);
    return te_in_child(ctx, kChart, p);
}

ImGuiID port(ImGuiTestContext* ctx, const char* state) {
    char p[256];
    snprintf(p, sizeof p, "%s/##out", state);
    return te_in_child(ctx, kChart, p);
}

// The definition's bytes, copied.
void bytes(ImVector<char>* out) {
    size_t n = 0;
    const char* t = dsg_doc_bytes(ui_doc(), DSG_BUF_DEF, &n);
    out->resize((int)n);
    memcpy(out->Data, t, n);
}

bool same_bytes(const ImVector<char>& a) {
    size_t n = 0;
    const char* t = dsg_doc_bytes(ui_doc(), DSG_BUF_DEF, &n);
    return (int)n == a.Size && memcmp(t, a.Data, n) == 0;
}

const char* str_at(const char* path) {
    const yjs_value* v = dsg_def_get(ui_doc(), path);
    return v && v->type == YJS_STRING ? v->u.s : "";
}

void test_drag(ImGuiTestContext* ctx) {
    IM_CHECK(open_bimanual(ctx, "chart_drag"));
    const ImGuiID id = box(ctx, "hold_start");
    const ImRect r0 = ctx->ItemInfo(id).RectFull;
    IM_CHECK(r0.GetWidth() > 0);
    ImVector<char> b0;
    bytes(&b0);
    ctx->ItemDragWithDelta(id, ImVec2(60, 45));
    const ImRect r1 = ctx->ItemInfo(id).RectFull;
    IM_CHECK_EQ(r1.Min.x - r0.Min.x, 60.0f);
    IM_CHECK_EQ(r1.Min.y - r0.Min.y, 45.0f);
    IM_CHECK(same_bytes(b0));
    IM_CHECK(!ui_dirty());
    // the position is view state: Save writes it to the view file only
    IM_CHECK(ui_save());
    dsg_view v;
    char err[256];
    IM_CHECK(dsg_view_read(ui_project()->folder, &v, err, sizeof err) == 0);
    size_t n = 0;
    const char* part = dsg_view_part_get(&v, "chart", &n);
    IM_CHECK(part != nullptr && strstr(part, "\"hold_start\":[") != nullptr);
    dsg_view_free(&v);
    // the bytes Save wrote are the canonical form: no view state in them
    size_t m = 0;
    IM_CHECK(strstr(dsg_doc_bytes(ui_doc(), DSG_BUF_DEF, &m), "hold_start\":[") == nullptr);
}

void test_connect(ImGuiTestContext* ctx) {
    IM_CHECK(open_bimanual(ctx, "chart_connect"));
    IM_CHECK_STR_EQ(str_at("/trials/reach/states/1/name"), "hold_start");
    IM_CHECK(dsg_def_get(ui_doc(), "/trials/reach/states/1/exits/1") == nullptr);
    const int depth = dsg_doc_undo_depth(ui_doc());
    ctx->ItemDragAndDrop(port(ctx, "hold_start"), box(ctx, "begin"));
    IM_CHECK_STR_EQ(str_at("/trials/reach/states/1/exits/1/to"), "begin");
    IM_CHECK_EQ(dsg_doc_undo_depth(ui_doc()), depth + 1);
    IM_CHECK_STR_EQ(dsg_doc_selection(ui_doc())->path, "/trials/reach/states/1/exits/1");
    // a drop on empty space makes nothing
    const ImRect g = ctx->ItemInfo(port(ctx, "go")).RectFull;
    ctx->ItemDragToPos(port(ctx, "go"), ImVec2(g.Max.x + 4, g.Max.y + 160));
    IM_CHECK_EQ(dsg_doc_undo_depth(ui_doc()), depth + 1);
}

void test_issue(ImGuiTestContext* ctx) {
    IM_CHECK(open_bimanual(ctx, "chart_issue"));
    // max_visits with no over: the loader names the state's key
    IM_CHECK_EQ(dsg_def_set_int(ui_doc(), "/trials/reach/states/1/max_visits", 2), 0);
    te_show(ctx, "Text");
    ctx->Yield(2);
    const dsg_report* r = ui_report();
    IM_CHECK(r != nullptr && r->n_errors > 0);
    int k = -1;
    for (int i = 0; r && i < r->n_issues && k < 0; i++)
        if (dsg_path_within(ui_issue_path(i), "/trials/reach/states/1")) k = i;
    IM_CHECK_GE(k, 0);
    char item[32];
    snprintf(item, sizeof item, "$$%d/##issue", k);
    ctx->ItemClick(te_in_child(ctx, "//##designer/Issues/##issue_list", item));
    IM_CHECK_STR_EQ(ui_main_view(), "chart");
    IM_CHECK(dsg_path_within(dsg_doc_selection(ui_doc())->path, "/trials/reach/states/1"));
    ctx->Yield(2);
    // the box is in view, in the canvas's middle
    const ImRect b = ctx->ItemInfo(box(ctx, "hold_start")).RectFull;
    const ImRect c = ctx->ItemInfo(te_in_child(ctx, kChart, "##canvas")).RectFull;
    IM_CHECK(c.Contains(b.GetCenter()));
    IM_CHECK_LT(fabsf(b.GetCenter().x - c.GetCenter().x), 2.0f);
}

void test_delete(ImGuiTestContext* ctx) {
    IM_CHECK(open_bimanual(ctx, "chart_delete"));
    ImVector<char> b0;
    bytes(&b0);
    ctx->ItemClick(box(ctx, "hold_start"));
    IM_CHECK_STR_EQ(dsg_doc_selection(ui_doc())->path, "/trials/reach/states/1");
    ctx->KeyPress(ImGuiKey_Delete);
    IM_CHECK_STR_EQ(str_at("/trials/reach/states/1/name"), "go");
    IM_CHECK(dsg_def_get(ui_doc(), "/trials/reach/states/2") == nullptr);
    IM_CHECK_STR_EQ(dsg_doc_selection(ui_doc())->path, "/trials/reach");
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    IM_CHECK(same_bytes(b0));
    IM_CHECK_STR_EQ(dsg_doc_selection(ui_doc())->path, "/trials/reach/states/1");
    IM_CHECK(ctx->ItemExists(box(ctx, "hold_start")));
}

void test_rename(ImGuiTestContext* ctx) {
    IM_CHECK(open_bimanual(ctx, "chart_rename"));
    const ImRect r0 = ctx->ItemInfo(box(ctx, "go")).RectFull;
    ctx->ItemDoubleClick(box(ctx, "go"));
    IM_CHECK(ctx->ItemExists(te_in_child(ctx, kChart, "##rename")));
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
    ctx->KeyChars("reach_go");
    ctx->KeyPress(ImGuiKey_Enter);
    IM_CHECK_STR_EQ(str_at("/trials/reach/states/2/name"), "reach_go");
    IM_CHECK_STR_EQ(str_at("/trials/reach/states/0/exits/1/to"), "reach_go");
    IM_CHECK_STR_EQ(str_at("/trials/reach/states/1/exits/0/to"), "reach_go");
    ctx->Yield(2);
    IM_CHECK(!ctx->ItemExists(te_in_child(ctx, kChart, "##rename")));
    const ImRect r1 = ctx->ItemInfo(box(ctx, "reach_go")).RectFull;
    IM_CHECK_EQ(r1.Min.x, r0.Min.x);
    IM_CHECK_EQ(r1.Min.y, r0.Min.y);
    IM_CHECK_EQ(ui_report()->n_errors, 0);
    // Ctrl+Z reaches the document: the field is gone with its focus
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    IM_CHECK_STR_EQ(str_at("/trials/reach/states/2/name"), "go");
}

} // namespace

void te_register_chart(ImGuiTestEngine* e) {
    IM_REGISTER_TEST(e, "chart", "drag")->TestFunc = test_drag;
    IM_REGISTER_TEST(e, "chart", "connect")->TestFunc = test_connect;
    IM_REGISTER_TEST(e, "chart", "issue")->TestFunc = test_issue;
    IM_REGISTER_TEST(e, "chart", "delete")->TestFunc = test_delete;
    IM_REGISTER_TEST(e, "chart", "rename")->TestFunc = test_rename;
}
