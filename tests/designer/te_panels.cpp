// te_panels.cpp - the property panels driven by the Test Engine (step 4,
// part C). OWNED BY PART C. A stub: the side column shows the panels, and
// the caret in the text selects what the panels show.
#include "te_common.h"
#include "ui.h"
#include "views.h"
#include "skf.h"

#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <string.h>

namespace {

void test_follows_text(ImGuiTestContext* ctx) {
    static const char* const files[] = { "pack.json", "experiment.json", "conditions.csv", nullptr };
    char dir[DSG_PATH_MAX];
    IM_CHECK(te_copy_project("rdk", "panels_follow", files, dir, sizeof dir));
    te_open(ctx, dir);
    te_show(ctx, "Text");
    IM_CHECK(ctx->WindowInfo("//##designer/Properties").ID != 0);
    // A click in the editor gives it the focus; the caret then names a value.
    ctx->ItemClick(te_in_child(ctx, "//##designer/Definition", "##definition"));
    size_t n;
    const char* t = ui_text(&n);
    const char* at = strstr(t, "\"stimuli\"");
    IM_CHECK(at != nullptr);
    skf_set_caret(ui_editor(), (int)dsg_codepoints_before(t, n, (uint32_t)(at - t)));
    ctx->Yield();
    IM_CHECK_STR_EQ(dsg_doc_selection(ui_doc())->path, "/stimuli");
}

} // namespace

void te_register_panels(ImGuiTestEngine* e) { IM_REGISTER_TEST(e, "panels", "follows_text")->TestFunc = test_follows_text; }
