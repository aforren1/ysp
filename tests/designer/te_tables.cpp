// te_tables.cpp - the tables' grid editor driven by the Test Engine (step
// 4, part B). OWNED BY PART B. A stub: the switch shows the tables' window.
#include "te_common.h"
#include "ui.h"

#include "imgui_te_context.h"
#include "imgui_te_engine.h"

namespace {

void test_shows(ImGuiTestContext* ctx) {
    static const char* const files[] = { "pack.json", "experiment.json", "conditions.csv", nullptr };
    char dir[DSG_PATH_MAX];
    IM_CHECK(te_copy_project("rdk", "tables_shows", files, dir, sizeof dir));
    te_open(ctx, dir);
    te_show(ctx, "Tables");
    IM_CHECK_STR_EQ(ui_main_view(), "tables");
    IM_CHECK(ctx->WindowInfo("//##designer/Tables").ID != 0);
}

} // namespace

void te_register_tables(ImGuiTestEngine* e) { IM_REGISTER_TEST(e, "tables", "shows")->TestFunc = test_shows; }
