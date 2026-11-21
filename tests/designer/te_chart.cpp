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
//   chart/add       Add state appends a state at the middle of the canvas,
//                   selected, as one undo step.
//   chart/view      The wheel zooms about the mouse; a drag on empty space
//                   pans. Neither changes the definition.
//   chart/events    An events template is read-only: the banner shows, Add
//                   state is disabled, a box does not move, no port shows.
//
// Step 5, part C (docs/designer.md, "Part C as built"). The first three
// drive the chart through a test double of the preview (view_chart.h):
//   chart/ring      The ring on the running state, the end, an events
//                   template's part; none for another template, between
//                   trials or with no session.
//   chart/follow    Follow shows the running template and follows it to
//                   the next; it stays between trials, is kept in the view
//                   file, and a jump to another template turns it off.
//   chart/pin       "Preview this template" pins the shown template and
//                   shows the pane; a refusal says so and shows nothing.
// These two use the real preview.c and skip on S's stub (no session load):
//   chart/pin_real  The pin is the first leaf that runs the template, any row.
//   chart/ring_real A session that runs bimanual, paused: the ring is on the
//                   state that the preview says.
#include "te_common.h"
#include "ui.h"
#include "views.h"
#include "view_chart.h"
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

void test_add(ImGuiTestContext* ctx) {
    IM_CHECK(open_bimanual(ctx, "chart_add"));
    IM_CHECK(dsg_def_get(ui_doc(), "/trials/reach/states/3") == nullptr);
    ImVector<char> b0;
    bytes(&b0);
    const int depth = dsg_doc_undo_depth(ui_doc());
    ctx->ItemClick(te_in_child(ctx, kChart, "Add state"));
    IM_CHECK_STR_EQ(str_at("/trials/reach/states/3/name"), "state");
    IM_CHECK_EQ(dsg_doc_undo_depth(ui_doc()), depth + 1);
    IM_CHECK_STR_EQ(dsg_doc_selection(ui_doc())->path, "/trials/reach/states/3");
    ctx->Yield(2);
    const ImRect b = ctx->ItemInfo(box(ctx, "state")).RectFull;
    const ImRect c = ctx->ItemInfo(te_in_child(ctx, kChart, "##canvas")).RectFull;
    IM_CHECK_LT(fabsf(b.GetCenter().x - c.GetCenter().x), 2.0f);
    IM_CHECK_LT(fabsf(b.GetCenter().y - c.GetCenter().y), 2.0f);
    // a second one takes the next free name
    ctx->ItemClick(te_in_child(ctx, kChart, "Add state"));
    IM_CHECK_STR_EQ(str_at("/trials/reach/states/4/name"), "state2");
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    IM_CHECK(same_bytes(b0));
}

void test_view(ImGuiTestContext* ctx) {
    IM_CHECK(open_bimanual(ctx, "chart_view"));
    ImVector<char> b0;
    bytes(&b0);
    const ImGuiID id = box(ctx, "hold_start");
    const ImRect r0 = ctx->ItemInfo(id).RectFull;
    // the wheel over a point of the box: that point stays under the mouse
    const ImVec2 m(r0.Min.x + r0.GetWidth() * 0.25f, r0.Min.y + r0.GetHeight() * 0.75f);
    ctx->MouseMoveToPos(m);
    ctx->MouseWheelY(1.0f);
    ctx->Yield(2);
    const ImRect r1 = ctx->ItemInfo(id).RectFull;
    IM_CHECK_LT(fabsf(r1.GetWidth() / r0.GetWidth() - 1.15f), 0.01f);
    IM_CHECK_LT(fabsf(r1.Min.x + r1.GetWidth() * 0.25f - m.x), 1.0f);
    IM_CHECK_LT(fabsf(r1.Min.y + r1.GetHeight() * 0.75f - m.y), 1.0f);
    // a drag on empty space pans by the drag
    const ImRect c = ctx->ItemInfo(te_in_child(ctx, kChart, "##canvas")).RectFull;
    ImVec2 e(c.Min.x + 6, c.Max.y - 6);
    const char* const names[] = { "begin", "hold_start", "go" };
    for (const char* nm : names) IM_CHECK(!ctx->ItemInfo(box(ctx, nm)).RectFull.Contains(e));
    ctx->MouseMoveToPos(e);
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(e.x + 50, e.y - 30));
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield(2);
    const ImRect r2 = ctx->ItemInfo(id).RectFull;
    IM_CHECK_LT(fabsf(r2.Min.x - r1.Min.x - 50), 1.0f);
    IM_CHECK_LT(fabsf(r2.Min.y - r1.Min.y + 30), 1.0f);
    IM_CHECK(same_bytes(b0));
    IM_CHECK(!ui_dirty());
}

// oddball's one template uses events and has no until: one part.
void test_events(ImGuiTestContext* ctx) {
    const char* const files[] = { "pack.json", "experiment.json", "oddball.csv", "standard.png", "target.png", nullptr };
    char dir[DSG_PATH_MAX];
    IM_CHECK(te_copy_project("oddball", "chart_events", files, dir, sizeof dir));
    te_open(ctx, dir);
    te_show(ctx, "Chart");
    IM_CHECK_STR_EQ(ui_main_view(), "chart");
    IM_CHECK(ctx->ItemExists(te_in_child(ctx, kChart, "##readonly")));
    IM_CHECK((ctx->ItemInfo(te_in_child(ctx, kChart, "Add state")).ItemFlags & ImGuiItemFlags_Disabled) != 0);
    const ImGuiID id = box(ctx, "steps 1 to 6");
    const ImRect r0 = ctx->ItemInfo(id).RectFull;
    IM_CHECK(r0.GetWidth() > 0);
    IM_CHECK(!ctx->ItemExists(port(ctx, "steps 1 to 6")));
    ImVector<char> b0;
    bytes(&b0);
    ctx->ItemDragWithDelta(id, ImVec2(60, 45));
    const ImRect r1 = ctx->ItemInfo(id).RectFull;
    IM_CHECK_EQ(r1.Min.x, r0.Min.x);
    IM_CHECK_EQ(r1.Min.y, r0.Min.y);
    IM_CHECK(same_bytes(b0));
    // a template with states shows no banner
    IM_CHECK(open_bimanual(ctx, "chart_events_b"));
    IM_CHECK(!ctx->ItemExists(te_in_child(ctx, kChart, "##readonly")));
}

// --- step 5, part C: the preview's hooks --------------------------------------

// The test double: the place and the pin that the chart sees.
dsg_preview_where g_where;
bool g_where_on = false;
char g_pinned[256];
bool g_pin_ok = false;

const dsg_preview_where* fake_where() { return g_where_on ? &g_where : nullptr; }

bool fake_pin(dsg_preview*, const char* tmpl) {
    snprintf(g_pinned, sizeof g_pinned, "%s", tmpl);
    return g_pin_ok;
}

// Puts the double in for one test and the real calls back at its end, an
// IM_CHECK's early return too.
struct Double {
    Double() {
        g_where_on = false;
        g_pinned[0] = 0;
        g_pin_ok = false;
        const ChartPreviewHooks h = { fake_where, fake_pin };
        ui_chart_set_preview_hooks(&h);
    }
    ~Double() { ui_chart_set_preview_hooks(nullptr); }
};

void at(const char* tmpl, const char* state, int phase) {
    memset(&g_where, 0, sizeof g_where);
    g_where.trial = tmpl ? 3 : -1;
    g_where.condition = -1;
    g_where.tmpl = tmpl;
    g_where.state = state;
    g_where.phase = phase;
    g_where_on = true;
}

ImGuiID end_node(ImGuiTestContext* ctx) { return te_in_child(ctx, kChart, "end/##end"); }

// The ring's vertices in r (the last frame's draw list of the chart): the
// ring is the only mark of its color.
int ring_in(ImGuiTestContext* ctx, ImRect r) {
    ImGuiWindow* w = ctx->WindowInfo(kChart).Window;
    if (!w || !w->DrawList) return -1;
    const ImU32 col = ui_chart_ring_color();
    int n = 0;
    for (const ImDrawVert& v : w->DrawList->VtxBuffer)
        if (v.col == col && r.Contains(v.pos)) n++;
    return n;
}

// Around a node: the ring is 5 px out at zoom 1, 3 px wide.
int ring_at(ImGuiTestContext* ctx, ImGuiID node) {
    ImRect r = ctx->ItemInfo(node).RectFull;
    if (r.GetWidth() <= 0) return -1;
    r.Expand(10);
    return ring_in(ctx, r);
}

int ring_all(ImGuiTestContext* ctx) { return ring_in(ctx, ImRect(-1e6f, -1e6f, 1e6f, 1e6f)); }

bool follow_on(ImGuiTestContext* ctx) {
    return (ctx->ItemInfo(te_in_child(ctx, kChart, "Follow")).StatusFlags & ImGuiItemStatusFlags_Checked) != 0;
}

// S's stub of preview.c gives no session load; part A's real one does.
bool preview_is_stub() {
    dsg_session_ops ops = {};
    dsg_preview_session_ops(ui_preview_gl(), &ops);
    return ops.load == nullptr;
}

void test_ring(ImGuiTestContext* ctx) {
    Double dbl;
    IM_CHECK(open_bimanual(ctx, "chart_ring"));
    ctx->Yield(2);
    IM_CHECK_EQ(ring_all(ctx), 0);
    at("reach", "hold_start", DSG_PV_PHASE_EVENTS);
    ctx->Yield(2);
    const int on = ring_at(ctx, box(ctx, "hold_start"));
    IM_CHECK_GT(on, 0);
    IM_CHECK_EQ(ring_all(ctx), on);   // nowhere else
    // the selection's border does not hide it
    ctx->ItemClick(box(ctx, "hold_start"));
    ctx->Yield(2);
    IM_CHECK_GT(ring_at(ctx, box(ctx, "hold_start")), 0);
    at("reach", "go", DSG_PV_PHASE_RESPONSE);
    ctx->Yield(2);
    IM_CHECK_EQ(ring_at(ctx, box(ctx, "hold_start")), 0);
    IM_CHECK_GT(ring_at(ctx, box(ctx, "go")), 0);
    // no state: the trial is at its end
    at("reach", nullptr, DSG_PV_PHASE_AFTER);
    ctx->Yield(2);
    IM_CHECK_GT(ring_at(ctx, end_node(ctx)), 0);
    IM_CHECK_EQ(ring_all(ctx), ring_at(ctx, end_node(ctx)));
    // another template, with Follow off: the chart stays, with no ring
    at("track", "wait", DSG_PV_PHASE_EVENTS);
    ctx->Yield(2);
    IM_CHECK(ctx->ItemExists(box(ctx, "begin")));
    IM_CHECK_EQ(ring_all(ctx), 0);
    at(nullptr, nullptr, DSG_PV_PHASE_BETWEEN);
    ctx->Yield(2);
    IM_CHECK_EQ(ring_all(ctx), 0);
    g_where_on = false;
    ctx->Yield(2);
    IM_CHECK_EQ(ring_all(ctx), 0);

    // oddball's one template uses events: one part, then the end
    const char* const files[] = { "pack.json", "experiment.json", "oddball.csv", "standard.png", "target.png", nullptr };
    char dir[DSG_PATH_MAX];
    IM_CHECK(te_copy_project("oddball", "chart_ring_events", files, dir, sizeof dir));
    te_open(ctx, dir);
    te_show(ctx, "Chart");
    at("oddball", nullptr, DSG_PV_PHASE_EVENTS);
    ctx->Yield(2);
    IM_CHECK_GT(ring_at(ctx, box(ctx, "steps 1 to 6")), 0);
    IM_CHECK_EQ(ring_at(ctx, end_node(ctx)), 0);
    at("oddball", nullptr, DSG_PV_PHASE_AFTER);
    ctx->Yield(2);
    IM_CHECK_EQ(ring_at(ctx, box(ctx, "steps 1 to 6")), 0);
    IM_CHECK_GT(ring_at(ctx, end_node(ctx)), 0);
    // an until splits it into three parts: the place cannot say which one
    // runs, so each is ringed
    const char until[] = "{\"until\": \"x\"}";
    IM_CHECK_EQ(dsg_def_set(ui_doc(), "/trials/oddball/events/1", until, sizeof until - 1), 0);
    at("oddball", nullptr, DSG_PV_PHASE_EVENTS);
    ctx->Yield(2);
    const char* const parts[] = { "step 1", "until x", "steps 3 to 6" };
    for (const char* nm : parts) IM_CHECK_GT(ring_at(ctx, box(ctx, nm)), 0);
    IM_CHECK_EQ(ring_at(ctx, end_node(ctx)), 0);
}

void test_follow(ImGuiTestContext* ctx) {
    Double dbl;
    IM_CHECK(open_bimanual(ctx, "chart_follow"));
    at("track", "wait", DSG_PV_PHASE_EVENTS);
    ctx->Yield(2);
    IM_CHECK(!follow_on(ctx));
    IM_CHECK(ctx->ItemExists(box(ctx, "begin")));
    IM_CHECK(!ctx->ItemExists(box(ctx, "wait")));
    ctx->ItemClick(te_in_child(ctx, kChart, "Follow"));
    ctx->Yield(2);
    IM_CHECK(follow_on(ctx));
    IM_CHECK(ctx->ItemExists(box(ctx, "wait")));
    IM_CHECK_GT(ring_at(ctx, box(ctx, "wait")), 0);
    // the next trial runs the other template: the chart goes with it
    at("reach", "go", DSG_PV_PHASE_EVENTS);
    ctx->Yield(2);
    IM_CHECK(ctx->ItemExists(box(ctx, "go")));
    IM_CHECK_GT(ring_at(ctx, box(ctx, "go")), 0);
    // between trials, and with no session, the chart stays
    at(nullptr, nullptr, DSG_PV_PHASE_BETWEEN);
    ctx->Yield(2);
    IM_CHECK(ctx->ItemExists(box(ctx, "go")));
    g_where_on = false;
    ctx->Yield(2);
    IM_CHECK(ctx->ItemExists(box(ctx, "go")));
    // a template that the text no longer has: the chart stays
    at("gone", "x", DSG_PV_PHASE_EVENTS);
    ctx->Yield(2);
    IM_CHECK(ctx->ItemExists(box(ctx, "go")));
    // Follow is view state: Save keeps it in the view file
    IM_CHECK(ui_save());
    dsg_view v;
    char err[256];
    IM_CHECK(dsg_view_read(ui_project()->folder, &v, err, sizeof err) == 0);
    size_t n = 0;
    const char* part = dsg_view_part_get(&v, "chart", &n);
    const bool kept = part != nullptr && strstr(part, "\"follow\":true") != nullptr;
    dsg_view_free(&v);
    IM_CHECK(kept);
    // a jump to another template (an issue, a panel) is a person's choice:
    // Follow goes off, so the chart does not jump back
    at("reach", "go", DSG_PV_PHASE_EVENTS);
    ctx->Yield(2);
    dsg_sel s;
    dsg_sel_def(&s, "/trials/track/states/1");
    ui_reveal(&s);
    ctx->Yield(2);
    IM_CHECK(!follow_on(ctx));
    IM_CHECK(ctx->ItemExists(box(ctx, "wait")));
    IM_CHECK_EQ(ring_all(ctx), 0);
    // so is a pick in the list: on again, the chart goes back to reach;
    // track picked, Follow goes off and the chart stays on track
    ctx->ItemClick(te_in_child(ctx, kChart, "Follow"));
    ctx->Yield(2);
    IM_CHECK(follow_on(ctx));
    IM_CHECK(ctx->ItemExists(box(ctx, "go")));
    ctx->ItemClick(te_in_child(ctx, kChart, "##template"));
    ctx->ItemClick("//$FOCUSED/$$1/##t");   // track: the second in the canonical text
    ctx->Yield(2);
    IM_CHECK(!follow_on(ctx));
    IM_CHECK(ctx->ItemExists(box(ctx, "wait")));
}

void test_pin(ImGuiTestContext* ctx) {
    Double dbl;
    IM_CHECK(open_bimanual(ctx, "chart_pin"));
    IM_CHECK(!ui_aux_open());
    const ImGuiID button = te_in_child(ctx, kChart, "Preview this template");
    ctx->ItemClick(button);
    IM_CHECK_STR_EQ(g_pinned, "reach");
    IM_CHECK(!ui_aux_open());
    IM_CHECK(strstr(ui_status(), "no leaf of the timeline runs 'reach'") != nullptr);
    g_pin_ok = true;
    g_pinned[0] = 0;
    ctx->ItemClick(button);
    IM_CHECK_STR_EQ(g_pinned, "reach");
    IM_CHECK(ui_aux_open());
    ui_show_aux(false);
    // a read-only chart pins too: the pin edits no definition
    const char* const files[] = { "pack.json", "experiment.json", "oddball.csv", "standard.png", "target.png", nullptr };
    char dir[DSG_PATH_MAX];
    IM_CHECK(te_copy_project("oddball", "chart_pin_events", files, dir, sizeof dir));
    te_open(ctx, dir);
    te_show(ctx, "Chart");
    g_pinned[0] = 0;
    ctx->ItemClick(te_in_child(ctx, kChart, "Preview this template"));
    IM_CHECK_STR_EQ(g_pinned, "oddball");
    IM_CHECK(ui_aux_open());
}

void test_pin_real(ImGuiTestContext* ctx) {
    if (preview_is_stub()) {
        ctx->LogInfo("preview.c is part S's stub: skipped until part A merges");
        return;
    }
    IM_CHECK(open_bimanual(ctx, "chart_pin_real"));
    ctx->Yield(2);
    dsg_preview* pv = ui_preview();
    char leaf[512] = "";
    for (int i = 0; i < dsg_preview_leaves(pv) && !leaf[0]; i++) {
        dsg_preview_leaf l = {};
        if (dsg_preview_leaf_at(pv, i, &l) && l.tmpl && strcmp(l.tmpl, "reach") == 0) snprintf(leaf, sizeof leaf, "%s", l.path);
    }
    IM_CHECK(leaf[0] != 0);
    ctx->ItemClick(te_in_child(ctx, kChart, "Preview this template"));
    dsg_preview_settings st = {};
    dsg_preview_get(pv, &st);
    IM_CHECK_STR_EQ(st.session.pin_leaf, leaf);
    IM_CHECK_EQ(st.session.pin_row, -1);
    IM_CHECK(ui_aux_open());
    ui_show_aux(false);
}

void test_ring_real(ImGuiTestContext* ctx) {
    if (preview_is_stub()) {
        ctx->LogInfo("preview.c is part S's stub: skipped until part A merges");
        return;
    }
    IM_CHECK(open_bimanual(ctx, "chart_ring_real"));
    ctx->ItemClick(te_in_child(ctx, kChart, "Follow"));
    ui_show_aux(true);
    const dsg_preview_where* w = nullptr;
    for (int i = 0; i < 1200; i++) {
        w = ui_preview_where();
        if (w && w->tmpl && w->state) break;
        ctx->Yield();
    }
    IM_CHECK(w != nullptr && w->tmpl != nullptr && w->state != nullptr);
    dsg_preview_pause(ui_preview());
    ctx->Yield(3);
    w = ui_preview_where();
    IM_CHECK(w != nullptr && w->tmpl != nullptr && w->state != nullptr);
    char state[256];
    snprintf(state, sizeof state, "%s", w->state);
    IM_CHECK(ctx->ItemExists(box(ctx, state)));
    IM_CHECK_GT(ring_at(ctx, box(ctx, state)), 0);
    IM_CHECK_EQ(ring_all(ctx), ring_at(ctx, box(ctx, state)));
    ui_show_aux(false);
}

} // namespace

void te_register_chart(ImGuiTestEngine* e) {
    IM_REGISTER_TEST(e, "chart", "drag")->TestFunc = test_drag;
    IM_REGISTER_TEST(e, "chart", "connect")->TestFunc = test_connect;
    IM_REGISTER_TEST(e, "chart", "issue")->TestFunc = test_issue;
    IM_REGISTER_TEST(e, "chart", "delete")->TestFunc = test_delete;
    IM_REGISTER_TEST(e, "chart", "rename")->TestFunc = test_rename;
    IM_REGISTER_TEST(e, "chart", "add")->TestFunc = test_add;
    IM_REGISTER_TEST(e, "chart", "view")->TestFunc = test_view;
    IM_REGISTER_TEST(e, "chart", "events")->TestFunc = test_events;
    IM_REGISTER_TEST(e, "chart", "ring")->TestFunc = test_ring;
    IM_REGISTER_TEST(e, "chart", "follow")->TestFunc = test_follow;
    IM_REGISTER_TEST(e, "chart", "pin")->TestFunc = test_pin;
    IM_REGISTER_TEST(e, "chart", "pin_real")->TestFunc = test_pin_real;
    IM_REGISTER_TEST(e, "chart", "ring_real")->TestFunc = test_ring_real;
}
