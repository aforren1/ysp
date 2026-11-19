// te_chart.cpp - the state chart driven by the Test Engine (step 4, part
// A). OWNED BY PART A. A stub: the switch shows the chart's window.
#include "te_common.h"
#include "ui.h"

#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <string.h>

namespace {

void test_shows(ImGuiTestContext* ctx) {
    static const char* const files[] = { "pack.json", "experiment.json", nullptr };
    char dir[DSG_PATH_MAX];
    IM_CHECK(te_copy_project("bimanual", "chart_shows", files, dir, sizeof dir));
    te_open(ctx, dir);
    te_show(ctx, "Chart");
    IM_CHECK_STR_EQ(ui_main_view(), "chart");
    IM_CHECK(ctx->WindowInfo("//##designer/Chart").ID != 0);
}

} // namespace

void te_register_chart(ImGuiTestEngine* e) { IM_REGISTER_TEST(e, "chart", "shows")->TestFunc = test_shows; }
