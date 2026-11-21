// te_test.cpp - the designer's UI (designer/app/ui.cpp, skf.cpp, skt.cpp)
// driven by the Dear ImGui Test Engine, with no window and no GL. Built only
// with YSP_DESIGNER_TEST_ENGINE (docs/designer.md, "UI tests with the Test
// Engine"); test_designer_ui stays the default suite.
//
// Usage: test_designer_te WORK [FILTER]
//   designer/path_field  No folder dialog: the path field takes the keyboard;
//                        typed keys and Enter open the project.
//   designer/edit_jump   Keys typed into the Skribidi editor reach the check;
//                        a click on the issue puts the caret at its place,
//                        past Arabic text (bytes are not codepoints).
//   designer/unsaved     Opening another project with changes asks first;
//                        Cancel keeps the text, Discard opens the other.
//   designer/undo_views  A structured edit shows in the text; Ctrl+Z with
//                        another view shown takes it back (one undo stack).
//   chart/*              8 tests of the state chart (te_chart.cpp): drag,
//                        connect, issue, delete, rename, add, view, events.
//   tables/*             5 tests of the tables (te_tables.cpp): shows,
//                        scroll, edit, add_row, widths.
//   panels/*             9 tests of the property panels (te_panels.cpp):
//                        follows_text, contrast, issue_mark, hebrew,
//                        long_panel, drag, select_commits, rename,
//                        collapsed.
//   docs/designer.md, "Decisions of step 4", says what each one checks.
// Each test sends the engine's mouse and keys through ImGui's input queue,
// as a person would. Exit 0 when every test passes, 1 otherwise.
#include "te_common.h"
#include "ui.h"
#include "views.h"
#include "skf.h"
#include "skt.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#include "imgui_te_exporters.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef YSP_SOURCE_DIR
#define YSP_SOURCE_DIR "."
#endif
#ifndef YSP_DESIGNER_FONT_DIR
#define YSP_DESIGNER_FONT_DIR "fonts"
#endif

namespace {

const char* g_work = "";

// Three copies of tests/player/rdk, so a test never opens the folder that is
// already open: an open of the same folder would not show the question.
char dir[3][DSG_PATH_MAX];

bool no_dialog(void*) { return false; }

// One frame as main.cpp draws it, with a renderer that only accepts textures.
void frame(ImGuiTestEngine* engine) {
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    // the check thread's result of the last frame's post lands in this one
    ui_wait_checks(30000000000LL);
    ImGui::NewFrame();
    skt_frame_begin();
    ui_frame();
    skt_frame_end();
    ImGui::Render();
    for (ImTextureData* t : ImGui::GetPlatformIO().Textures) {
        if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates) {
            t->SetTexID((ImTextureID)1);
            t->SetStatus(ImTextureStatus_OK);
        } else if (t->Status == ImTextureStatus_WantDestroy) {
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
        }
    }
    ImGuiTestEngine_PostSwap(engine);
}

bool copy_file(const char* from, const char* to) {
    char* d;
    size_t n;
    char err[512];
    if (dsg_read_file(from, &d, &n, err, sizeof err)) return false;
    bool ok = dsg_write_file(to, d, n, err, sizeof err) == 0;
    free(d);
    return ok;
}

bool project(const char* work, const char* name, char* out, size_t cap) {
    static const char* const files[] = { "pack.json", "experiment.json", "conditions.csv" };
    char err[512], a[DSG_PATH_MAX], b[DSG_PATH_MAX], src[DSG_PATH_MAX];
    if (dsg_join(out, cap, work, name) || dsg_make_dir(out, err, sizeof err)) return false;
    if (dsg_join(src, sizeof src, YSP_SOURCE_DIR, "tests/player/rdk")) return false;
    for (const char* f : files)
        if (dsg_join(a, sizeof a, src, f) || dsg_join(b, sizeof b, out, f) || !copy_file(a, b)) return false;
    if (dsg_join(b, sizeof b, out, DSG_VIEW_FILE) == 0) remove(b);
    return true;
}

const char* editor_text(size_t* n) {
    static ImVector<char> buf;
    *n = (size_t)skf_text(ui_editor(), &buf);
    return buf.Data;
}

bool modal_open() { return ImGui::GetTopMostPopupModal() != nullptr; }

// An item in a child window. The engine's item paths do not descend into
// child windows (WindowInfo() does), so the child's ID seeds the item's.
ImGuiID in_child(ImGuiTestContext* ctx, const char* window, const char* item) {
    const ImGuiTestItemInfo w = ctx->WindowInfo(window);
    return w.ID ? ctx->GetID(item, w.ID) : 0;
}

ImGuiID editor_id(ImGuiTestContext* ctx) { return in_child(ctx, "//##designer/Definition", "##definition"); }

// Opens dir[i] through the path field, as a person types it. With unsaved
// changes the question comes up; Discard answers it.
void open_by_path(ImGuiTestContext* ctx, int i) {
    ctx->ItemInputValue("//##designer/##path", dir[i]);
    if (modal_open()) ctx->ItemClick("//Unsaved changes/Discard");
    // the open's check runs on the check thread; its result lands a frame later
    ctx->Yield();
    IM_CHECK(ui_project() != nullptr);
    IM_CHECK_STR_EQ(ui_project()->folder, dir[i]);
}

void test_path_field(ImGuiTestContext* ctx) {
    // As main.cpp does when SDL's folder dialog fails.
    ui_dialog_failed("test");
    // The focus request takes effect over the next frames.
    for (int i = 0; i < 8 && !ImGui::GetIO().WantTextInput; i++) ctx->Yield();
    IM_CHECK(strstr(ui_status(), "path field") != nullptr);
    IM_CHECK(ImGui::GetIO().WantTextInput);
    // No click: the keys go where the focus already is.
    ctx->KeyChars(dir[0]);
    ctx->KeyPress(ImGuiKey_Enter);
    IM_CHECK(ui_project() != nullptr);
    IM_CHECK_STR_EQ(ui_project()->folder, dir[0]);
}

void test_edit_jump(ImGuiTestContext* ctx) {
    open_by_path(ctx, 1);
    if (ctx->IsError()) return;
    IM_CHECK(ui_report() != nullptr && ui_report()->n_errors == 0);

    // The engine's Ctrl+click gives the Skribidi field the focus.
    ctx->ItemInput(editor_id(ctx));
    IM_CHECK(skf_focused(ui_editor()));

    // Arabic before an error makes byte and codepoint offsets differ. The
    // caret is placed directly; the typing is the engine's.
    size_t n;
    const char* t = editor_text(&n);
    const char* at = strstr(t, "\"format\"");
    IM_CHECK(at != nullptr);
    skf_set_caret(ui_editor(), (int)dsg_codepoints_before(t, n, (uint32_t)(at - t)));
    const int checks0 = ui_checks_run();
    ctx->KeyChars("\"note\": \"\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7\", ");
    ctx->Yield();   // the check's result lands a frame after the edit
    t = editor_text(&n);
    IM_CHECK(strstr(t, "\"note\": \"\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7\", \"format\"") != nullptr);
    IM_CHECK_GT(ui_checks_run(), checks0);
    IM_CHECK_EQ(ui_report()->n_errors, 0);

    const char* st = strstr(t, "\"stimuli\"");
    IM_CHECK(st != nullptr);
    skf_set_caret(ui_editor(), (int)dsg_codepoints_before(t, n, (uint32_t)(st - t)));
    ctx->KeyChars("x");
    ctx->Yield();   // the check's result lands a frame after the edit
    t = editor_text(&n);
    const dsg_report* r = ui_report();
    IM_CHECK_GT(r->n_errors, 0);
    const uint32_t off = r->issues[0].place.offset;
    IM_CHECK(off < n && t[off] == 'x');

    // Move the caret away with the engine's keys, then click the issue.
    ctx->KeyPress(ImGuiKey_UpArrow, 3);
    ctx->KeyPress(ImGuiKey_Home);
    const int want = (int)dsg_codepoints_before(t, n, off);
    IM_CHECK_NE(skf_caret(ui_editor()), want);
    ctx->ItemClick(in_child(ctx, "//##designer/Issues/##issue_list", "$$0/##issue"));
    IM_CHECK_EQ(skf_caret(ui_editor()), want);
    IM_CHECK_LT(want, (int)off);   // the Arabic is two bytes a codepoint

    // The click kept the editor's focus: Ctrl+Z goes to it.
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    ctx->Yield();   // the check's result lands a frame after the edit
    t = editor_text(&n);
    IM_CHECK(strstr(t, "x\"stimuli\"") == nullptr);
    IM_CHECK_EQ(ui_report()->n_errors, 0);
}

// One undo stack for every view: a structured edit through the document
// shows in the text view, and Ctrl+Z with the chart shown takes it back.
void test_undo_views(ImGuiTestContext* ctx) {
    open_by_path(ctx, 0);
    if (ctx->IsError()) return;
    IM_CHECK_EQ(dsg_def_set_string(ui_doc(), "/name", "undo across views"), 0);
    ctx->Yield();
    size_t n;
    const char* t = editor_text(&n);
    IM_CHECK(strstr(t, "\"name\": \"undo across views\"") != nullptr);
    te_show(ctx, "Chart");
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    t = editor_text(&n);
    IM_CHECK(strstr(t, "undo across views") == nullptr);
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z);
    t = editor_text(&n);
    IM_CHECK(strstr(t, "undo across views") != nullptr);
    te_show(ctx, "Text");
}

void test_unsaved(ImGuiTestContext* ctx) {
    open_by_path(ctx, 2);
    if (ctx->IsError()) return;
    ctx->ItemInput(editor_id(ctx));
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_End);
    ctx->KeyChars(" ");
    IM_CHECK(ui_dirty());
    size_t n0;
    ImVector<char> before;
    const char* t0 = editor_text(&n0);
    before.resize((int)n0);
    memcpy(before.Data, t0, n0);

    // Another project: asked, not switched.
    ctx->ItemInputValue("//##designer/##path", dir[0]);
    IM_CHECK(modal_open());
    IM_CHECK_STR_EQ(ui_project()->folder, dir[2]);

    // Cancel: the same project and text, still unsaved.
    ctx->ItemClick("//Unsaved changes/Cancel");
    IM_CHECK(!modal_open());
    IM_CHECK_STR_EQ(ui_project()->folder, dir[2]);
    size_t n;
    const char* t = editor_text(&n);
    IM_CHECK(n == n0 && memcmp(t, before.Data, n) == 0);
    IM_CHECK(ui_dirty());

    // The button asks too; Discard opens the other project.
    ctx->ItemClick("//##designer/Open path");
    IM_CHECK(modal_open());
    ctx->ItemClick("//Unsaved changes/Discard");
    IM_CHECK(!modal_open());
    IM_CHECK_STR_EQ(ui_project()->folder, dir[0]);
}

} // namespace

bool te_copy_project(const char* name, const char* copy, const char* const* files, char* out, size_t cap) {
    char err[512], a[DSG_PATH_MAX], b[DSG_PATH_MAX], src[DSG_PATH_MAX], rel[256];
    snprintf(rel, sizeof rel, "tests/player/%s", name);
    if (dsg_join(out, cap, g_work, copy) || dsg_make_dir(out, err, sizeof err)) return false;
    if (dsg_join(src, sizeof src, YSP_SOURCE_DIR, rel)) return false;
    for (int i = 0; files[i]; i++)
        if (dsg_join(a, sizeof a, src, files[i]) || dsg_join(b, sizeof b, out, files[i]) || !copy_file(a, b)) return false;
    if (dsg_join(b, sizeof b, out, DSG_VIEW_FILE) == 0) remove(b);
    return true;
}

void te_open(ImGuiTestContext* ctx, const char* folder) {
    ctx->ItemInputValue("//##designer/##path", folder);
    if (modal_open()) ctx->ItemClick("//Unsaved changes/Discard");
    // the open's check runs on the check thread; its result lands a frame later
    ctx->Yield();
    IM_CHECK(ui_project() != nullptr);
    IM_CHECK_STR_EQ(ui_project()->folder, folder);
}

ImGuiID te_in_child(ImGuiTestContext* ctx, const char* window, const char* item) { return in_child(ctx, window, item); }
bool te_modal_open() { return modal_open(); }

void te_show(ImGuiTestContext* ctx, const char* label) {
    char path[128];
    snprintf(path, sizeof path, "//##designer/switch/%s", label);
    ctx->ItemClick(path);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: test_designer_te WORK [FILTER]\n");
        return 2;
    }
    const char* work = argv[1];
    g_work = work;
    char err[512];
    static const char* const names[3] = { "rdk", "rdk2", "rdk3" };
    if (dsg_make_dir(work, err, sizeof err)) {
        printf("FAIL: the work folder: %s\n", err);
        return 1;
    }
    for (int i = 0; i < 3; i++)
        if (!project(work, names[i], dir[i], sizeof dir[i])) {
            printf("FAIL: could not copy the project to %s\n", work);
            return 1;
        }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1400, 900);
    io.ConfigInputTextCursorBlink = false;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    UiPlatform plat = { nullptr, no_dialog, nullptr, nullptr };
    if (!ui_init(YSP_DESIGNER_FONT_DIR, &plat, err, sizeof err)) {
        printf("FAIL: ui_init: %s\n", err);
        return 1;
    }

    ImGuiTestEngine* engine = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO& tio = ImGuiTestEngine_GetIO(engine);
    tio.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
    tio.ConfigNoThrottle = true;
    tio.ConfigFixedDeltaTime = 1.0f / 60.0f;
    tio.ConfigSavedSettings = false;
    tio.ConfigCaptureEnabled = false;
    tio.ConfigLogToTTY = true;
    tio.ConfigVerboseLevel = ImGuiTestVerboseLevel_Warning;
    tio.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    // ctest's own timeout is the last resort; these name the test that hung.
    tio.ConfigWatchdogWarning = 15.0f;
    tio.ConfigWatchdogKillTest = 30.0f;
    tio.ConfigWatchdogKillApp = 60.0f;

    IM_REGISTER_TEST(engine, "designer", "path_field")->TestFunc = test_path_field;
    IM_REGISTER_TEST(engine, "designer", "edit_jump")->TestFunc = test_edit_jump;
    IM_REGISTER_TEST(engine, "designer", "unsaved")->TestFunc = test_unsaved;
    IM_REGISTER_TEST(engine, "designer", "undo_views")->TestFunc = test_undo_views;
    te_register_chart(engine);
    te_register_tables(engine);
    te_register_panels(engine);

    ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext());
    ImGuiTestEngine_QueueTests(engine, ImGuiTestGroup_Tests, argc > 2 ? argv[2] : nullptr, ImGuiTestRunFlags_RunFromCommandLine);
    while (!ImGuiTestEngine_IsTestQueueEmpty(engine)) frame(engine);
    ImGuiTestEngine_Stop(engine);

    ImGuiTestEngineResultSummary sum;
    ImGuiTestEngine_GetResultSummary(engine, &sum);
    ImGuiTestEngine_PrintResultSummary(engine);

    ui_shutdown();
    // ImGui's context first: the engine unbinds from it (imgui_te_engine.h).
    ImGui::DestroyContext();
    ImGuiTestEngine_DestroyContext(engine);
    const bool pass = sum.CountTested > 0 && sum.CountSuccess == sum.CountTested && sum.CountInQueue == 0;
    printf("%s: %d of %d tests passed\n", pass ? "PASS" : "FAIL", sum.CountSuccess, sum.CountTested);
    return pass ? 0 : 1;
}
