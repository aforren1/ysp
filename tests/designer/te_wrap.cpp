// te_wrap.cpp - rows that wrap (2026-11-23; docs/designer.md, "Rows that
// wrap"). The hand test on X11 did not find the preview's Pop out: the
// controls were one row of SameLine() items, and ImGui clipped the ones
// past the pane's right edge.
//
//   wrap/rows    At window widths of 800, 1024, 1280 and 1920 px, with the
//                splitter at the default and at each bound, every item of
//                the toolbar, the preview's pane, the Run view, the chart's
//                toolbar and the tables' toolbar is inside its window's
//                clip rect on x, and Pop out is in view. With a simulated
//                run going, Stop (and on Linux and macOS, Kill) too.
//   wrap/popped  The preview popped out (--viewports only): its window at
//                960 and 400 px wide has every item in view, Pop in too.
//
// A failure prints each item out of view: its window, label and rect.
#include "te_common.h"
#include "ui.h"
#include "views.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

const char* const kMain = "//##designer";
const char* const kPane = "//##designer/Preview";
const char* const kRun = "//##designer/Run";
const char* const kChart = "//##designer/Chart";
const char* const kTables = "//##designer/Tables";
const char* const kPopped = "//Preview##window";
const char* const kRdk[] = { "pack.json", "experiment.json", "conditions.csv", nullptr };

// The window size, the AUX views and the project put back however a test
// ends.
struct Scope {
    ImVec2 size;
    Scope() : size(ImGui::GetIO().DisplaySize) {}
    ~Scope() {
        if (ui_run_active()) {
            ui_run_stop();
            ui_run_kill();
        }
        if (ui_preview_popped()) ui_preview_pop(false);
        ui_show_aux_view("run", false);
        ui_show_aux(false);
        ImGui::GetIO().DisplaySize = size;
    }
};

ImGuiWindow* win(ImGuiTestContext* ctx, const char* path) {
    ImGuiWindow* w = ctx->WindowInfo(path, ImGuiTestOpFlags_NoError).Window;
    return w && w->Active ? w : nullptr;
}

// An item's label. ImGui gives the engine no label for an item that it
// clips whole, so the rows' labels are tried against the item's ID.
const char* name_of(const ImGuiTestItemInfo& it) {
    static const char* const labels[] = { "Run", "Pause", "Step", "Restart", "New seed", "Speed", "Fit", "1:1", "Details", "Pop out",
                                          "Pop in", "Full screen", "Leave full screen", "New", "Open...", "Save", "Undo", "Redo",
                                          "Build", "Stop", "Kill", "Preview", "##path", "Open path", "##template", "Follow",
                                          "Preview this template", "Add state", "Rename", "Set first", "Delete", "Add row",
                                          "Remove row", "Add column", "Remove column", "Copy ID", "Copy path", "Open trials.csv",
                                          "Rig", "##rig", "Copy the folder's path", "Simulated (--sim)", nullptr };
    if (it.DebugLabel[0]) return it.DebugLabel;
    for (int k = 0; labels[k]; k++)
        if (ImHashStr(labels[k], 0, it.ParentID) == it.ID) return labels[k];
    static char hex[16];
    snprintf(hex, sizeof hex, "#%08X", (unsigned)it.ID);
    return hex;
}

// The items of window w (not of its child windows) that ImGui clips on x:
// their rect is not inside the window's inner clip rect. Only items above
// y_end (the chart's canvas pans and clips its boxes on purpose), and, with
// names, only those. Each is printed; gives their count.
int out_of_view(const ImGuiTestItemList& items, ImGuiWindow* w, const char* what, const char* where, float y_end = FLT_MAX,
                const char* const* names = nullptr) {
    if (!w) return 0;
    const ImRect clip = w->InnerClipRect;
    int n = 0;
    for (const ImGuiTestItemInfo& it : items) {
        if (it.Window != w || it.RectFull.Min.y >= y_end) continue;
        if (names) {
            bool named = false;
            for (int k = 0; names[k]; k++) named = named || strcmp(it.DebugLabel, names[k]) == 0;
            if (!named) continue;
        }
        if (it.RectFull.Min.x >= clip.Min.x - 0.5f && it.RectFull.Max.x <= clip.Max.x + 0.5f) continue;
        char label[48];
        snprintf(label, sizeof label, "%s", name_of(it));
        printf("te_wrap: %s, %s: '%s' x %.1f to %.1f, the window's clip %.1f to %.1f\n", where, what, label, (double)it.RectFull.Min.x,
               (double)it.RectFull.Max.x, (double)clip.Min.x, (double)clip.Max.x);
        n++;
    }
    return n;
}

// An item of a child window, in view on both axes.
bool in_view(ImGuiTestContext* ctx, const char* window, const char* label, const char* where) {
    ImGuiWindow* w = win(ctx, window);
    const ImGuiTestItemInfo it = ctx->ItemInfo(te_in_child(ctx, window, label), ImGuiTestOpFlags_NoError);
    if (!w || !it.ID) {
        printf("te_wrap: %s: no '%s' in %s\n", where, label, window);
        return false;
    }
    const ImRect clip = w->InnerClipRect;
    if (clip.Contains(it.RectFull)) return true;
    printf("te_wrap: %s: '%s' (%.1f, %.1f to %.1f, %.1f) is not inside %s's clip (%.1f, %.1f to %.1f, %.1f)\n", where, label,
           (double)it.RectFull.Min.x, (double)it.RectFull.Min.y, (double)it.RectFull.Max.x, (double)it.RectFull.Max.y, window,
           (double)clip.Min.x, (double)clip.Min.y, (double)clip.Max.x, (double)clip.Max.y);
    return false;
}

// Every row checked at this layout: gives the count of items out of view.
int check_rows(ImGuiTestContext* ctx, const char* where) {
    ctx->Yield(3);
    ImGuiTestItemList items;
    ctx->GatherItems(&items, kMain);
    int n = out_of_view(items, win(ctx, kMain), "toolbar", where);
    n += out_of_view(items, win(ctx, kPane), "preview", where);
    n += out_of_view(items, win(ctx, kRun), "Run view", where);
    if (ImGuiWindow* c = win(ctx, kChart)) {
        const ImGuiTestItemInfo canvas = ctx->ItemInfo(te_in_child(ctx, kChart, "##canvas"), ImGuiTestOpFlags_NoError);
        n += out_of_view(items, c, "chart's toolbar", where, canvas.ID ? canvas.RectFull.Min.y : FLT_MAX);
    }
    static const char* const table_tools[] = { "Add row", "Remove row", "Add column", "Remove column", nullptr };
    n += out_of_view(items, win(ctx, kTables), "tables' toolbar", where, FLT_MAX, table_tools);
    if (win(ctx, kPane) && !in_view(ctx, kPane, "Pop out", where)) n++;
    // the path field fills the rest of its row, at least 200 points (ui.cpp's
    // kPathMinW): it shrank to 152 at 800 px with Stop and Kill
    const ImGuiTestItemInfo path = ctx->ItemInfo("//##designer/##path", ImGuiTestOpFlags_NoError);
    if (path.ID == 0 || path.RectFull.GetWidth() < 199.5f) {
        printf("te_wrap: %s: the path field is %.1f wide\n", where, (double)path.RectFull.GetWidth());
        n++;
    }
    return n;
}

// The main window at width w; then the splitter at each bound.
int check_width(ImGuiTestContext* ctx, float w, bool split) {
    ImGui::GetIO().DisplaySize = ImVec2(w, 900);
    char where[96];
    snprintf(where, sizeof where, "%.0f px", (double)w);
    int n = check_rows(ctx, where);
    if (!split) return n;
    // a drag to the left widens the pane to its bound, to the right narrows it
    ctx->ItemDragWithDelta("//##designer/##aux_split", ImVec2(-3000, 0));
    snprintf(where, sizeof where, "%.0f px, the pane at its widest (%.0f)", (double)w, (double)win(ctx, kPane)->Size.x);
    n += check_rows(ctx, where);
    ctx->ItemDragWithDelta("//##designer/##aux_split", ImVec2(3000, 0));
    snprintf(where, sizeof where, "%.0f px, the pane at its narrowest (%.0f)", (double)w, (double)win(ctx, kPane)->Size.x);
    n += check_rows(ctx, where);
    return n;
}

const float kWidths[] = { 800, 1024, 1280, 1920 };

// rdk open, with the preview running, the Run view shown, a main view.
bool open_all(ImGuiTestContext* ctx, const char* copy) {
    char dir[DSG_PATH_MAX];
    if (!te_copy_project("rdk", copy, kRdk, dir, sizeof dir)) return false;
    te_open(ctx, dir);
    if (!ui_aux_open()) ctx->ItemClick("//##designer/aux/Preview");
    if (!ui_aux_view_open("run")) ctx->ItemClick("//##designer/aux/Run");
    for (int i = 0; i < 240; i++) {
        dsg_preview_info in = {};
        dsg_preview_get_info(ui_preview(), &in);
        if (in.status == DSG_PV_RUNNING) return ui_aux_view_open("run");
        ctx->Yield();
    }
    return false;
}

void test_rows(ImGuiTestContext* ctx) {
    Scope sc;
    IM_CHECK(open_all(ctx, "wrap_rows"));
    te_show(ctx, "Chart");
    int n = 0;
    for (float w : kWidths) n += check_width(ctx, w, true);
    te_show(ctx, "Tables");
    for (float w : kWidths) n += check_width(ctx, w, false);
    te_show(ctx, "Chart");
    // Stop shows while a run goes, then Kill after a Stop that the player
    // ignores
    IM_CHECK(te_run_start_sim(ctx, "header trial,response,rt_s,outcome\nterm ignore\nout armed\nsleep 30000\n"));
    IM_CHECK(ctx->ItemExists("//##designer/tool/Stop"));
    n += check_width(ctx, kWidths[0], true);
    n += check_width(ctx, kWidths[3], false);
    if (te_run_offer_kill(ctx)) {
        ctx->Yield();
        IM_CHECK(ctx->ItemExists("//##designer/tool/Kill"));
        n += check_width(ctx, kWidths[0], true);
    }
    IM_CHECK_EQ(n, 0);
}

void test_popped(ImGuiTestContext* ctx) {
    if (!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) return;
    Scope sc;
    IM_CHECK(open_all(ctx, "wrap_popped"));
    ImGui::GetIO().DisplaySize = ImVec2(kWidths[0], 900);
    ctx->Yield(2);
    ctx->ItemDragWithDelta("//##designer/##aux_split", ImVec2(3000, 0));
    ctx->Yield(2);
    // at the narrowest, Pop out can be clicked
    ctx->ItemClick(te_in_child(ctx, kPane, "Pop out"));
    ctx->Yield(2);
    IM_CHECK(ui_preview_popped());
    int n = 0;
    static const float kPopWidths[] = { 960.0f, 400.0f };
    for (float w : kPopWidths) {
        ImGui::SetWindowSize("Preview##window", ImVec2(w, 700));
        ctx->Yield(3);
        ImGuiWindow* pw = win(ctx, kPopped);
        IM_CHECK(pw != nullptr);
        IM_CHECK_LE(fabsf(pw->Size.x - w), 1.0f);
        char where[64];
        snprintf(where, sizeof where, "popped out, %.0f px", (double)w);
        ImGuiTestItemList items;
        ctx->GatherItems(&items, kPopped);
        n += out_of_view(items, pw, "preview", where);
        if (!in_view(ctx, kPopped, "Pop in", where)) n++;
        if (!in_view(ctx, kPopped, "Full screen", where)) n++;
    }
    IM_CHECK_EQ(n, 0);
    ctx->ItemClick(te_in_child(ctx, kPopped, "Pop in"));
    ctx->Yield(2);
    IM_CHECK(!ui_preview_popped());
}

} // namespace

void te_register_wrap(ImGuiTestEngine* e) {
    IM_REGISTER_TEST(e, "wrap", "rows")->TestFunc = test_rows;
    IM_REGISTER_TEST(e, "wrap", "popped")->TestFunc = test_popped;
}
