// te_run.cpp - the Run view driven by the Test Engine (designer step 6, part
// V; docs/designer.md, "Part V as built"). OWNED BY PART V.
//
//   run/build_id      The view's Build gives the pack ID, which the view
//                     shows with "Up to date"; an edit makes it stale.
//   run/build_table   A Build whose error names a table's cell: the view
//                     says the Build failed, and a click on the issue in the
//                     Issues panel selects the cell in the Tables view.
//   run/sim_end       Simulated, Run: the trials, the last row and the log
//                     show while it goes; its end says done, with the
//                     folder and Open trials.csv, and no display restore.
//   run/stop          Stop gives "Stopped" (exit 3).
//   run/kill          Kill: at once when Stop cannot reach the player (a
//                     --sim run on Windows); after a Stop that went out, it
//                     comes only when the run offers it. Its end names the
//                     signal.
//   run/disabled      Run is disabled while the check has errors.
//   run/rig_combo     The combo lists the rig folder's profiles, profile.json
//                     first as the default; "No profile" only for a
//                     simulated run while profile.json exists. A choice is
//                     the check's and the preview's; the preview's band
//                     names it, and the preview's own combo goes back to the
//                     built-in simulated rig.
//   run/rig_changed   A change of the profile's file shows within a second
//                     of frames and posts a check.
//   run/needs         oddball: its session field and its role below tier
//                     come from the check's needs; with a profile chosen the
//                     role says that the profile gives it no tier.
//
// The rig folder is WORK/run_xdg/ysp/rig: XDG_CONFIG_HOME (Windows: APPDATA)
// points at WORK/run_xdg for the whole process, so no test reads the user's
// own rig folder. YSP_DESIGNER_TEST is set: dsg_run_args() refuses a real
// run.
//
// DSG_RUN_VIEW_DOUBLE 1: on run_view_double.c (build.c, run.c and rig.c
// scripted). 0, at the integration: on parts B, R and G, with R's
// fake_player as the player (YSP_TE_FAKE_PLAYER, its script in place of the
// built pack). docs/designer.md, "Part V as built", gives the steps.
#ifndef DSG_RUN_VIEW_DOUBLE
#define DSG_RUN_VIEW_DOUBLE 1
#endif
#include "te_common.h"
#include "ui.h"
#include "views.h"
#include "run_view_double.h"
#include "csv.h"
#include "player.h"   // PL_VERSION_STRING, yrig_dir()

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef YSP_TE_FAKE_PLAYER
#define YSP_TE_FAKE_PLAYER ""
#endif

namespace {

const char* const kRun = "//##designer/Run";
const char* const kRdk[] = { "pack.json", "experiment.json", "conditions.csv", nullptr };
const char* const kOddball[] = { "pack.json", "experiment.json", "oddball.csv", "standard.png", "target.png", nullptr };
char g_player[DSG_PATH_MAX] = "/double/ysp_player";
char g_rig_dir[DSG_PATH_MAX] = "";

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// The text of one frame: ImGui's own and what the views put in its log. The
// log unclips every item and opens tree nodes two deep, so a part of the
// view scrolled out of sight is in it too. The test runs after NewFrame(),
// in the window that ImGui ends last, whose End() would finish the log
// before the test reads it: so the log has no window of its own.
const char* frame_text(ImGuiTestContext* ctx) {
    static ImGuiTextBuffer buf;
    ImGui::LogToBuffer();
    ImGui::GetCurrentContext()->LogWindow = nullptr;
    ctx->Yield();
    buf.clear();
    buf.append(ImGui::GetCurrentContext()->LogBuffer.c_str());
    ImGui::LogFinish();
    return buf.c_str();
}

bool has_text(ImGuiTestContext* ctx, const char* s) {
    const char* t = frame_text(ctx);
    if (strstr(t, s)) return true;
    printf("te_run: '%s' is not in the frame's text:\n%.3000s\n", s, t);
    return false;
}

ImGuiID run_item(ImGuiTestContext* ctx, const char* label) { return te_in_child(ctx, kRun, label); }

bool disabled(ImGuiTestContext* ctx, ImGuiID id) {
    const ImGuiTestItemInfo it = ctx->ItemInfo(id, ImGuiTestOpFlags_NoError);
    return it.ID != 0 && (it.ItemFlags & ImGuiItemFlags_Disabled) != 0;
}

dsg_run_info run_info() {
    dsg_run_info in = {};
    if (ui_run()) dsg_run_get_info(ui_run(), &in);
    return in;
}

// Each test leaves no run going (the next open would ask), the double's
// script cleared and the AUX views hidden, however it ends.
struct Scope {
    Scope() {
#if DSG_RUN_VIEW_DOUBLE
        rvd_reset();
#endif
    }
    ~Scope() {
        if (ui_run_active()) {
            ui_run_stop();
            ui_run_kill();
        }
        ui_show_aux_view("run", false);
        ui_show_aux(false);
#if DSG_RUN_VIEW_DOUBLE
        rvd_reset();
#endif
    }
};

// The project in dir, opened, with the Run view shown.
bool open_dir(ImGuiTestContext* ctx, const char* dir) {
    te_open(ctx, dir);
    if (!ui_aux_view_open("run")) ctx->ItemClick("//##designer/aux/Run");
    // the check of the open lands a frame later
    for (int i = 0; i < 30 && !(ui_report() && ui_can_build()); i++) ctx->Yield();
    return ui_aux_view_open("run") && ui_report() != nullptr;
}

// A copy of a tests/player project, opened, with the Run view shown.
bool open_run(ImGuiTestContext* ctx, const char* name, const char* copy, const char* const* files, char* dir, size_t cap) {
    return te_copy_project(name, copy, files, dir, cap) && open_dir(ctx, dir);
}

// Frames until the Build that was posted came back.
void wait_build(ImGuiTestContext* ctx) {
    ui_wait_builds(60000000000LL);
    for (int i = 0; i < 10 && ui_build_busy(); i++) {
        ui_wait_builds(60000000000LL);
        ctx->Yield();
    }
    ctx->Yield();
}

bool write_text(const char* folder, const char* rel, const char* text) {
    char path[DSG_PATH_MAX], err[256];
    return dsg_join(path, sizeof path, folder, rel) == 0 && dsg_write_file(path, text, strlen(text), err, sizeof err) == 0;
}

// A profile in the test's rig folder: its rig name, its view, its roles.
bool write_profile(const char* file, const char* rig, double distance_mm, double width_mm, int width_px, const char* roles) {
    char text[1024];
    snprintf(text, sizeof text,
             "{\"format\": \"ysp-rig 1\", \"rig\": \"%s\", \"written\": \"2026-11-22T00:00:00Z\", \"roles\": {%s}, "
             "\"view\": {\"distance_mm\": %g, \"width_mm\": %g, \"width_px\": %d}}\n",
             rig, roles, distance_mm, width_mm, width_px);
    return write_text(g_rig_dir, file, text);
}

#if !DSG_RUN_VIEW_DOUBLE
// R's fake_player reads its PACK as a script: the test puts one in place of
// the pack that a Build wrote, and Run then starts it (the pack is no
// source, so Build's stamp still holds).
bool fake_script(const char* script) {
    if (!YSP_TE_FAKE_PLAYER[0]) return false;
    snprintf(g_player, sizeof g_player, "%s", YSP_TE_FAKE_PLAYER);
    set_env("YSP_FAKE_PLAYER_VERSION", PL_VERSION_STRING);
    const dsg_build_result* b = ui_build_last();
    char err[256];
    return b && b->status == DSG_BUILD_OK && dsg_write_file(b->path, script, strlen(script), err, sizeof err) == 0;
}
#endif

// A simulated run of the open project, started by the view's Run. Real
// parts: a Build first, then the script in place of the pack.
bool start_sim(ImGuiTestContext* ctx, const char* script) {
    if (!ui_run_settings()->sim) ctx->ItemClick(run_item(ctx, "Simulated (--sim)"));
    if (!ui_run_settings()->sim) return false;
#if DSG_RUN_VIEW_DOUBLE
    (void)script;
#else
    ctx->ItemClick(run_item(ctx, "Build"));
    wait_build(ctx);
    if (!fake_script(script)) return false;
#endif
    ctx->ItemClick(run_item(ctx, "Run"));
    for (int i = 0; i < 60 && !ui_run_active(); i++) {
        ui_wait_builds(60000000000LL);
        ctx->Yield();
    }
    return ui_run_active();
}

// Frames until the run ended, at most n.
bool wait_end(ImGuiTestContext* ctx, int n) {
    for (int i = 0; i < n && ui_run_active(); i++) ctx->Yield();
    ctx->Yield();
    return !ui_run_active() && ui_run() != nullptr;
}

void test_build_id(ImGuiTestContext* ctx) {
    Scope sc;
    char dir[DSG_PATH_MAX];
    IM_CHECK(open_run(ctx, "rdk", "run_build", kRdk, dir, sizeof dir));
    IM_CHECK(has_text(ctx, "No Build since the project opened."));
    ctx->ItemClick(run_item(ctx, "Build"));
    wait_build(ctx);
    const dsg_build_result* b = ui_build_last();
    if (!b || b->status != DSG_BUILD_OK) printf("te_run: the Build: %s (%d)\n", ui_status(), b ? b->status : -1);
    IM_CHECK(b != nullptr && b->status == DSG_BUILD_OK);
    if (!b) return;
    IM_CHECK(strncmp(b->pack_id, "sha256:", 7) == 0);
    // the status line names the pack and its ID too: look for the view's
    char id[96], pack[DSG_PATH_MAX + 16];
    snprintf(id, sizeof id, "Pack ID: %s", b->pack_id);
    snprintf(pack, sizeof pack, "Pack:%s/build/run_build.ysppak", dir);   // LogText() adds no space
    const char* t = frame_text(ctx);
    IM_CHECK(strstr(t, id) != nullptr);
    IM_CHECK(strstr(t, "Up to date.") != nullptr);
    IM_CHECK(strstr(t, pack) != nullptr);
    // an edit: the pack is stale
    IM_CHECK_EQ(dsg_def_set_string(ui_doc(), "/name", "stale now"), 0);
    ctx->Yield();
    t = frame_text(ctx);
    IM_CHECK(strstr(t, "Stale: the definition or a source changed") != nullptr);
    IM_CHECK(strstr(t, "Up to date.") == nullptr);
}

void test_build_table(ImGuiTestContext* ctx) {
    Scope sc;
    char dir[DSG_PATH_MAX];
#if DSG_RUN_VIEW_DOUBLE
    IM_CHECK(open_run(ctx, "rdk", "run_table", kRdk, dir, sizeof dir));
    rvd_build_script(DSG_BUILD_FAILED,
                     "'conditions.pstb': ysp_table: line 3, row 2, column 'coherent_direction' (2): a quoted field is not closed", 0, 0,
                     nullptr);
#else
    // a quote that does not close, in the table's second row
    IM_CHECK(te_copy_project("rdk", "run_table", kRdk, dir, sizeof dir));
    IM_CHECK(write_text(dir, "conditions.csv", "coherence,coherent_direction,key_answer\n0.032,0,right\n0.032,\"180,left\n"));
    IM_CHECK(open_dir(ctx, dir));
#endif
    ctx->ItemClick(run_item(ctx, "Build"));
    wait_build(ctx);
    IM_CHECK(ui_build_last() != nullptr && ui_build_last()->status == DSG_BUILD_FAILED);
    // not the Issues panel's "The last Build failed: 1 errors."
    IM_CHECK(has_text(ctx, "The last Build failed: 1 error. The Issues panel lists it."));
    const dsg_report* r = ui_report();
    int at = -1;
    for (int i = 0; r && i < r->n_issues; i++)
        if (r->issues[i].from == DSG_FROM_BUILD && r->issues[i].severity == DSG_SEV_ERROR) at = i;
    IM_CHECK_GE(at, 0);
    if (at < 0) return;
    char entry[128], name[64];
    int row = -2, col = -1;
    IM_CHECK(dsg_csv_place(r->issues[at].message, entry, sizeof entry, &row, &col, name, sizeof name));
    IM_CHECK_GE(row, 0);
    char item[32];
    snprintf(item, sizeof item, "$$%d/##issue", at);
    ctx->ItemClick(te_in_child(ctx, "//##designer/Issues/##issue_list", item));
    ctx->Yield();
    IM_CHECK_STR_EQ(ui_main_view(), "tables");
    const dsg_sel* s = dsg_doc_selection(ui_doc());
    IM_CHECK_GT(s->buf, 0);
    IM_CHECK_EQ(s->row, row);
#if DSG_RUN_VIEW_DOUBLE
    IM_CHECK_EQ(s->row, 1);
    IM_CHECK_EQ(s->col, 1);
#endif
    te_show(ctx, "Text");
}

void test_sim_end(ImGuiTestContext* ctx) {
    Scope sc;
    char dir[DSG_PATH_MAX];
    IM_CHECK(open_run(ctx, "rdk", "run_sim", kRdk, dir, sizeof dir));
    IM_CHECK(has_text(ctx, "No run since the project opened."));
    IM_CHECK(start_sim(ctx, "header trial,response,rt_s,outcome\nrows 3 20 %d,left,0.412,correct\nout trial 3\nsleep 300\nexit 0\n"));
    IM_CHECK_EQ(ui_run_settings()->sim, true);
#if DSG_RUN_VIEW_DOUBLE
    bool sim_arg = false;
    for (int i = 0; rvd_run_arg(i); i++) sim_arg = sim_arg || strcmp(rvd_run_arg(i), "--sim") == 0;
    IM_CHECK(sim_arg);
    IM_CHECK(has_text(ctx, "Running (simulated), "));   // not the status line's "Running (simulated):"
    rvd_run_progress(3, "left", "correct", 0.412, "trial 3");
    ctx->Yield();
#else
    for (int i = 0; i < 120 && run_info().trials < 3; i++) ctx->Yield();
#endif
    const char* t = frame_text(ctx);
    IM_CHECK(strstr(t, "Trials written: 3") != nullptr);
    IM_CHECK(strstr(t, "left") != nullptr);
    IM_CHECK(strstr(t, "correct") != nullptr);
    IM_CHECK(strstr(t, "rt 0.412 s") != nullptr);
    IM_CHECK(strstr(t, "trial 3") != nullptr);
#ifdef _WIN32
    // Stop cannot reach a --sim run on Windows (Part P): Kill, and no Stop
    IM_CHECK(ctx->ItemExists(run_item(ctx, "Kill")));
    IM_CHECK(!ctx->ItemExists(run_item(ctx, "Stop")));
#else
    IM_CHECK(!ctx->ItemExists(run_item(ctx, "Kill")));
#endif
#if DSG_RUN_VIEW_DOUBLE
    rvd_run_end(0);
#endif
    IM_CHECK(wait_end(ctx, 300));
    IM_CHECK_EQ(run_info().exit_status, 0);
    char want[DSG_PATH_MAX + 64];
    snprintf(want, sizeof want, "%s", run_info().exit_text);
    t = frame_text(ctx);
    IM_CHECK(strstr(t, "Ended:") != nullptr);
    IM_CHECK(want[0] != 0 && strstr(t, want) != nullptr);
    IM_CHECK(strstr(t, run_info().out_dir) != nullptr);
    IM_CHECK(ctx->ItemExists(run_item(ctx, "Open trials.csv")));
    IM_CHECK(!ctx->ItemExists(run_item(ctx, "##restore")));
}

void test_stop(ImGuiTestContext* ctx) {
    Scope sc;
#ifdef _WIN32
    // no Stop for a --sim run on Windows: run/kill tests what it has
    (void)ctx;
#else
    char dir[DSG_PATH_MAX];
    IM_CHECK(open_run(ctx, "rdk", "run_stop", kRdk, dir, sizeof dir));
#if DSG_RUN_VIEW_DOUBLE
    rvd_run_stop_ends(true);
#endif
    IM_CHECK(start_sim(ctx, "header trial,response,rt_s,outcome\nrow 1,left,0.4,correct\nterm exit 3\nsleep 30000\n"));
    IM_CHECK(ctx->ItemExists(run_item(ctx, "Stop")));
    ctx->ItemClick(run_item(ctx, "Stop"));
    IM_CHECK(wait_end(ctx, 120));
    IM_CHECK_EQ(run_info().exit_status, 3);
    const char* t = frame_text(ctx);
    IM_CHECK(strstr(t, "Stopped") != nullptr);
    IM_CHECK(strstr(t, "records the stop") != nullptr);
#endif
}

void test_kill(ImGuiTestContext* ctx) {
    Scope sc;
    char dir[DSG_PATH_MAX];
    IM_CHECK(open_run(ctx, "rdk", "run_kill", kRdk, dir, sizeof dir));
#if DSG_RUN_VIEW_DOUBLE
    // Stop cannot reach the player: Kill at once
    rvd_run_stop_reaches(false);
    IM_CHECK(start_sim(ctx, nullptr));
#ifndef _WIN32
    ctx->ItemClick(run_item(ctx, "Stop"));
    ctx->Yield();
#endif
    IM_CHECK(ctx->ItemExists(run_item(ctx, "Kill")));
    IM_CHECK(!ctx->ItemExists(run_item(ctx, "Stop")));
    IM_CHECK(has_text(ctx, "Kill ends the player now. The data set is not closed."));
    ctx->ItemClick(run_item(ctx, "Kill"));
    IM_CHECK(wait_end(ctx, 10));
    IM_CHECK_EQ(rvd_run_kills(), 1);
    IM_CHECK_EQ(run_info().exit_status, -9);
    const char* t = frame_text(ctx);
    IM_CHECK(strstr(t, "Ended by signal 9") != nullptr);
    IM_CHECK(strstr(t, "did not close its data set") != nullptr);
    // a simulated run has no display to put back
    IM_CHECK(!ctx->ItemExists(run_item(ctx, "##restore")));
    // Stop that goes out: no Kill until the run offers it
    rvd_run_stop_reaches(true);
#endif
#ifndef _WIN32
    // (Windows: no Stop goes out to a --sim run)
    IM_CHECK(start_sim(ctx, "header trial,response,rt_s,outcome\nterm ignore\nsleep 30000\n"));
    ctx->ItemClick(run_item(ctx, "Stop"));
    ctx->Yield();
    IM_CHECK(!ctx->ItemExists(run_item(ctx, "Kill")));
    IM_CHECK(disabled(ctx, run_item(ctx, "Stop")));
    for (int i = 0; i < 400 && !run_info().kill_offered; i++) ctx->Yield();
    IM_CHECK(run_info().kill_offered);
    ctx->Yield();
    IM_CHECK(ctx->ItemExists(run_item(ctx, "Kill")));
    ctx->ItemClick(run_item(ctx, "Kill"));
    IM_CHECK(wait_end(ctx, 120));
    IM_CHECK_LT(run_info().exit_status, 0);
#endif
}

void test_disabled(ImGuiTestContext* ctx) {
    Scope sc;
    char dir[DSG_PATH_MAX];
    IM_CHECK(open_run(ctx, "rdk", "run_disabled", kRdk, dir, sizeof dir));
    IM_CHECK(!disabled(ctx, run_item(ctx, "Run")));
    ctx->ItemClick(te_in_child(ctx, "//##designer/Definition", "##definition"));
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_End);
    ctx->KeyChars("x");
    for (int i = 0; i < 60 && !(ui_report() && ui_report()->n_errors > 0); i++) ctx->Yield();
    IM_CHECK(ui_report() && ui_report()->n_errors > 0);
    ctx->Yield();
    IM_CHECK(disabled(ctx, run_item(ctx, "Run")));
    IM_CHECK(!ui_can_run());
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    for (int i = 0; i < 60 && !ui_can_run(); i++) ctx->Yield();
    ctx->Yield();
    IM_CHECK(!disabled(ctx, run_item(ctx, "Run")));
}

// The rig folder that the real rig.c reads; "" when yrig_dir() refuses it
// (a work folder under /tmp, which all users can write).
bool rig_folder_ok() {
#if DSG_RUN_VIEW_DOUBLE
    return true;
#else
    char d[DSG_PATH_MAX];
    return yrig_dir(d, sizeof d) == YRIG_OK;
#endif
}

bool write_rigs() {
    return write_profile("profile.json", "desk", 570, 518.4, 1920, "") &&
           write_profile("lab2.json", "lab two", 600, 400, 1280, "\"trig\": {\"family\": \"triggerbox\", \"key\": \"port:/dev/null\"}");
}

// The preview's combo (in its Settings) to the built-in simulated rig. The
// Run view is hidden: the Settings are below the display, in the room left.
void preview_builtin(ImGuiTestContext* ctx) {
    const ImGuiTestItemInfo det = ctx->WindowInfo("//##designer/Preview/##details");
    IM_CHECK(det.ID != 0);
    if (!det.ID) return;
    ctx->ItemOpen(ctx->GetID("Settings", det.ID));
    ctx->ItemClick(ctx->GetID("Rig", det.ID));
    ctx->ItemClick("//$FOCUSED/Built-in simulated rig");
}

void test_rig_combo(ImGuiTestContext* ctx) {
    Scope sc;
    char dir[DSG_PATH_MAX];
    IM_CHECK(write_rigs());
    IM_CHECK(open_run(ctx, "rdk", "run_rig", kRdk, dir, sizeof dir));
    if (!rig_folder_ok()) {
        // the real rig.c refuses the folder: the view says so
        IM_CHECK(has_text(ctx, "The rig folder gives no profile"));
        return;
    }
    ctx->ItemClick("//##designer/aux/Preview");
    IM_CHECK(ui_aux_open());
    // a real run with no --rig takes profile.json: the setting says so
    ctx->Yield(2);
    IM_CHECK_STR_EQ(ui_run_settings()->rig, "profile");
    ctx->ItemClick(run_item(ctx, "Rig profile"));
    const char* t = frame_text(ctx);
    IM_CHECK(strstr(t, "profile (default)") != nullptr);
    IM_CHECK(strstr(t, "lab2") != nullptr);
    IM_CHECK(strstr(t, "No profile (keyboard only)") == nullptr);
    IM_CHECK(strstr(t, "Rig: profile (view 570 mm, 1920 px over 518.4 mm)") != nullptr);
    ctx->KeyPress(ImGuiKey_Escape);
    // simulated: "No profile" too
    ctx->ItemClick(run_item(ctx, "Simulated (--sim)"));
    IM_CHECK(ui_run_settings()->sim);
    ctx->ItemClick(run_item(ctx, "Rig profile"));
    IM_CHECK(has_text(ctx, "No profile (keyboard only)"));
    const int c0 = ui_checks_run();
    ctx->ItemClick("//$FOCUSED/$$1/##rig");
    IM_CHECK_STR_EQ(ui_run_settings()->rig, "lab2");
    for (int i = 0; i < 10 && ui_checks_run() == c0; i++) ctx->Yield();
    IM_CHECK_GT(ui_checks_run(), c0);
    // the preview takes the choice, and its band names it
    dsg_preview_settings ps = {};
    dsg_preview_get(ui_preview(), &ps);
    IM_CHECK_STR_EQ(ps.session.rig, "lab2");
    t = frame_text(ctx);
    IM_CHECK(strstr(t, "Rig: lab2 (view 600 mm, 1280 px over 400 mm)") != nullptr);
    // the profile's details: its role
    IM_CHECK(strstr(t, "triggerbox") != nullptr);
    // the Run view hidden, the line is the preview's band
    ctx->ItemClick("//##designer/aux/Run");
    t = frame_text(ctx);
    IM_CHECK(ctx->ItemExists(te_in_child(ctx, "//##designer/Preview", "##rig")));
    IM_CHECK(strstr(t, "Rig: lab2 (view 600 mm, 1280 px over 400 mm)") != nullptr);
    IM_CHECK(strstr(t, "Built-in simulated rig: view 570 mm") == nullptr);
    // the preview's own combo: the built-in rig
    preview_builtin(ctx);
    dsg_preview_get(ui_preview(), &ps);
    IM_CHECK_STR_EQ(ps.session.rig, "");
    t = frame_text(ctx);
    IM_CHECK(strstr(t, "Built-in simulated rig: view 570 mm") != nullptr);
    IM_CHECK(strstr(t, "Rig: lab2 (view 600 mm") == nullptr);
    // the Run view keeps its own profile
    ctx->ItemClick("//##designer/aux/Run");
    IM_CHECK_STR_EQ(ui_run_settings()->rig, "lab2");
    IM_CHECK(has_text(ctx, "Rig: lab2 (view 600 mm"));
}

void test_rig_changed(ImGuiTestContext* ctx) {
    Scope sc;
    char dir[DSG_PATH_MAX];
    IM_CHECK(write_rigs());
    IM_CHECK(open_run(ctx, "rdk", "run_rig_changed", kRdk, dir, sizeof dir));
    if (!rig_folder_ok()) return;
    snprintf(ui_run_settings()->rig, DSG_RIG_NAME, "lab2");
    ctx->Yield(3);
    IM_CHECK(has_text(ctx, "Rig: lab2 (view 600 mm"));
    const int c0 = ui_checks_run();
    // another view, so another size: the stat sees it
    IM_CHECK(write_profile("lab2.json", "lab two", 650.5, 400, 1280, ""));
    bool seen = false;
    for (int i = 0; i < 90 && !seen; i++) seen = strstr(frame_text(ctx), "Rig: lab2 (view 650.5 mm") != nullptr;
    IM_CHECK(seen);
    for (int i = 0; i < 10 && ui_checks_run() == c0; i++) ctx->Yield();
    IM_CHECK_GT(ui_checks_run(), c0);
}

void test_needs(ImGuiTestContext* ctx) {
    Scope sc;
    char dir[DSG_PATH_MAX];
    IM_CHECK(write_rigs());
    IM_CHECK(open_run(ctx, "oddball", "run_needs", kOddball, dir, sizeof dir));
    bool tier = false, field = false;
    for (int i = 0; ui_report() && i < ui_report()->n_issues; i++) {
        const dsg_issue& is = ui_report()->issues[i];
        tier = tier || (is.from == DSG_FROM_RUN && is.object == PL_NEED_ROLE_TIER && is.name && strcmp(is.name, "resp") == 0);
        field = field || (is.from == DSG_FROM_RUN && is.object == PL_NEED_FIELD && is.name && strcmp(is.name, "participant") == 0);
    }
    IM_CHECK(tier && field);
    // simulated with no profile: no word about a profile's tier
    ctx->ItemClick(run_item(ctx, "Simulated (--sim)"));
    snprintf(ui_run_settings()->rig, DSG_RIG_NAME, "%s", "");
    ctx->Yield(2);
    const char* t = frame_text(ctx);
    IM_CHECK(strstr(t, "below its tier") != nullptr);
    IM_CHECK(strstr(t, "The profile gives it no tier") == nullptr);
    // the role's box: --allow-below-tier
    ctx->ItemClick(run_item(ctx, "resp/##allow"));
    IM_CHECK_EQ(ui_run_settings()->n_below, 1);
    IM_CHECK_STR_EQ(ui_run_settings()->allow_below_tier[0], "resp");
    // the field: --field participant=s07, and the folder's name
    ctx->ItemInputValue(run_item(ctx, "participant/##value"), "s07");
    IM_CHECK_EQ(ui_run_settings()->n_fields, 1);
    IM_CHECK_STR_EQ(ui_run_settings()->fields[0].value, "s07");
    IM_CHECK(has_text(ctx, "/data/s07_"));
    if (!rig_folder_ok()) return;
    // a profile chosen: the need stays at no tier
    snprintf(ui_run_settings()->rig, DSG_RIG_NAME, "lab2");
    ctx->Yield(3);
    IM_CHECK(has_text(ctx, "The profile gives it no tier"));
}

} // namespace

const char* te_run_player() { return g_player; }

void te_register_run(ImGuiTestEngine* e) {
    // Before any frame: the check thread and the views read these.
    char work[DSG_PATH_MAX], d[DSG_PATH_MAX], err[256];
    static const char* const none[] = { nullptr };
    if (te_copy_project("rdk", "run_xdg", none, work, sizeof work)) {
#ifdef _WIN32
        set_env("APPDATA", work);
#else
        set_env("XDG_CONFIG_HOME", work);
#endif
        if (dsg_join(d, sizeof d, work, "ysp") == 0 && dsg_make_dir(d, err, sizeof err) == 0 &&
            dsg_join(g_rig_dir, sizeof g_rig_dir, d, "rig") == 0)
            dsg_make_dir(g_rig_dir, err, sizeof err);
    }
    set_env("YSP_DESIGNER_TEST", "1");
    IM_REGISTER_TEST(e, "run", "build_id")->TestFunc = test_build_id;
    IM_REGISTER_TEST(e, "run", "build_table")->TestFunc = test_build_table;
    IM_REGISTER_TEST(e, "run", "sim_end")->TestFunc = test_sim_end;
    IM_REGISTER_TEST(e, "run", "stop")->TestFunc = test_stop;
    IM_REGISTER_TEST(e, "run", "kill")->TestFunc = test_kill;
    IM_REGISTER_TEST(e, "run", "disabled")->TestFunc = test_disabled;
    IM_REGISTER_TEST(e, "run", "rig_combo")->TestFunc = test_rig_combo;
    IM_REGISTER_TEST(e, "run", "rig_changed")->TestFunc = test_rig_changed;
    IM_REGISTER_TEST(e, "run", "needs")->TestFunc = test_needs;
}
