// te_preview.cpp - the preview's pane driven by the Test Engine (designer
// step 5, part B; docs/designer.md, "Part B as built"). OWNED BY PART B.
//
//   preview/key_space     A click on the display gives it the keyboard; Space
//                         through main.cpp's key hook (ui_preview_key()) is
//                         the trial's response; a click outside the pane and
//                         Escape each give the keyboard back, and Escape does
//                         not reach the session.
//   preview/pause         The Pause button stops the frames and the view's
//                         animation; Step shows one frame; Run starts again.
//   preview/edit_restart  A value typed in a property panel restarts the
//                         preview after the settle, not before.
//   preview/choice        A button of the choice strip is the trial's
//                         response.
//   preview/stale         A text that does not parse shows the banner and
//                         keeps the session; Ctrl+Z restarts it.
//
// These run on part A's preview.c: real sessions, loaded on the check thread
// and stepped by ui.cpp. Part B wrote them on a test double
// (preview_double.h); with DSG_PREVIEW_DOUBLE 1 here and preview_double.c
// in test_designer_te's sources they run on it again.
#define DSG_PREVIEW_DOUBLE 0
#include "te_common.h"
#include "ui.h"
#include "views.h"
#include "preview_double.h"

#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const char* const kPane = "//##designer/Preview";
const char* const kDisplay = "//##designer/Preview/##display";
// SDL scancodes, as main.cpp passes them.
const int kSpace = 44, kEscape = 41;

// The double on for one test, off after it however the test ends.
struct Double {
    Double() {
#if DSG_PREVIEW_DOUBLE
        pvd_enable(true);
        static const char* const choices[] = { "left", "right" };
        pvd_set_choices(2, choices);
        pvd_set_output_role("trig");
#endif
    }
    ~Double() {
        ui_show_aux(false);
#if DSG_PREVIEW_DOUBLE
        pvd_enable(false);
#endif
    }
};

dsg_preview_info info() {
    dsg_preview_info in = {};
    dsg_preview_get_info(ui_preview(), &in);
    return in;
}

// A copy of rdk, opened, with the pane shown and the session running. space
// true: its choices are Space and the right arrow, so Space is a response.
bool start(ImGuiTestContext* ctx, const char* copy, bool space) {
    static const char* const files[] = { "pack.json", "experiment.json", "conditions.csv", nullptr };
    char dir[DSG_PATH_MAX], path[DSG_PATH_MAX], err[256];
    if (!te_copy_project("rdk", copy, files, dir, sizeof dir)) return false;
    if (space) {
        char* d = nullptr;
        size_t n = 0;
        if (dsg_join(path, sizeof path, dir, "experiment.json") || dsg_read_file(path, &d, &n, err, sizeof err)) return false;
        static const char from[] = "\"choices\": [\"left\", \"right\"]", to[] = "\"choices\": [\"space\",\"right\"]";
        static_assert(sizeof from == sizeof to, "the same length: no other byte moves");
        char* at = strstr(d, from);
        if (at) memcpy(at, to, sizeof to - 1);
        const bool ok = at && dsg_write_file(path, d, n, err, sizeof err) == 0;
        free(d);
        if (!ok) return false;
    }
    te_open(ctx, dir);
    if (!ui_aux_open()) ctx->ItemClick("//##designer/aux/Preview");
    // the settle (300 ms), the job on the check thread, the attach
    for (int i = 0; i < 240 && info().status != DSG_PV_RUNNING; i++) ctx->Yield();
    return info().status == DSG_PV_RUNNING;
}

// Frames until the running trial takes a response. rdk's collector waits
// from the trial's start, but its rt counts from the dots, 0.5 s in, and a
// press before them or under minimum_valid_rt_s (0.1 s) after them is no
// response. So 0.8 s of session time after a trial's window opens: inside
// the 1.5 s that the dots allow. The first trial seen may be part way
// through, so the wait starts at the next trial's start.
void to_response(ImGuiTestContext* ctx) {
    dsg_preview_where w = {};
    int64_t t0 = -1;
    int trial0 = -2;
    for (int i = 0; i < 600; i++) {
        if (dsg_preview_where_now(ui_preview(), &w)) {
            if (trial0 == -2) trial0 = w.trial;
            if (w.trial != trial0 && w.phase == DSG_PV_PHASE_RESPONSE) {
                if (t0 < 0) t0 = w.t_ns;
                if (w.t_ns - t0 >= 800000000) return;
            }
        }
        ctx->Yield();
    }
}

// Frames until a trial after trial0 ended with a response; that response,
// or "".
const char* last_response(ImGuiTestContext* ctx, int trial0) {
    dsg_preview_where w = {};
    for (int i = 0; i < 240; i++) {
        if (dsg_preview_where_now(ui_preview(), &w) && w.trial > trial0 && w.response) return w.response;
        ctx->Yield();
    }
    return "";
}

void test_key_space(ImGuiTestContext* ctx) {
    Double dbl;
    IM_CHECK(start(ctx, "preview_key", true));
    // Not claimed: the key is the designer's.
    IM_CHECK(!ui_preview_has_keyboard());
    IM_CHECK(!ui_preview_key(kSpace, true, false));
    ui_preview_key(kSpace, false, false);
    const ImGuiTestItemInfo disp = ctx->WindowInfo(kDisplay);
    IM_CHECK(disp.ID != 0);
    ctx->ItemClick(ctx->GetID("##screen", disp.ID));
    IM_CHECK(ui_preview_has_keyboard());
    IM_CHECK(info().keyboard);
    to_response(ctx);
    dsg_preview_where w = {};
    IM_CHECK(dsg_preview_where_now(ui_preview(), &w));
    const int trial0 = w.trial;
    IM_CHECK(ui_preview_key(kSpace, true, false));
    ctx->Yield();
    IM_CHECK(ui_preview_key(kSpace, false, false));
    IM_CHECK_STR_EQ(last_response(ctx, trial0), "space");
#if !DSG_PREVIEW_DOUBLE
    // A's session writes the row: the run's trials.csv has the response.
    {
        char path[DSG_PATH_MAX], err[256];
        char* d = nullptr;
        size_t n = 0;
        IM_CHECK(dsg_join(path, sizeof path, info().out_dir, "trials.csv") == 0);
        bool found = false;
        // the data set's writer thread writes the row a little later
        for (int i = 0; i < 120 && !found; i++) {
            if (dsg_read_file(path, &d, &n, err, sizeof err) == 0) {
                found = strstr(d, "space") != nullptr;
                free(d);
            }
            if (!found) ctx->Yield();
        }
        IM_CHECK(found);
    }
#endif
    // A click outside the pane gives the keyboard back; a click on the
    // display claims it again.
    ctx->ItemClick(te_in_child(ctx, "//##designer/Definition", "##definition"));
    IM_CHECK(!ui_preview_has_keyboard());
    ctx->ItemClick(ctx->GetID("##screen", ctx->WindowInfo(kDisplay).ID));
    IM_CHECK(ui_preview_has_keyboard());
    // Escape is taken (the designer does not see it) and gives the keyboard
    // back; a key after it is the designer's, so the session never sees it.
    IM_CHECK(ui_preview_key(kEscape, true, false));
    ui_preview_key(kEscape, false, false);   // its down was not the session's: the designer's
    IM_CHECK(!ui_preview_has_keyboard());
    IM_CHECK(!ui_preview_key(kSpace, true, false));
    IM_CHECK(!ui_preview_key(kSpace, false, false));
}

void test_pause(ImGuiTestContext* ctx) {
    Double dbl;
    IM_CHECK(start(ctx, "preview_pause", false));
    IM_CHECK(ui_animating());
    ctx->ItemClick(te_in_child(ctx, kPane, "Pause"));
    IM_CHECK_EQ(info().status, (int)DSG_PV_PAUSED);
    IM_CHECK(!ui_animating());
    dsg_preview_where a = {}, b = {};
    IM_CHECK(dsg_preview_where_now(ui_preview(), &a));
    ctx->Yield(10);
    IM_CHECK(dsg_preview_where_now(ui_preview(), &b));
    IM_CHECK_EQ(a.frame, b.frame);
    // Step: one frame, still paused.
    ctx->ItemClick(te_in_child(ctx, kPane, "Step"));
    ctx->Yield();
    IM_CHECK(dsg_preview_where_now(ui_preview(), &b));
    IM_CHECK_EQ(b.frame, a.frame + 1);
    IM_CHECK_EQ(info().status, (int)DSG_PV_PAUSED);
    ctx->ItemClick(te_in_child(ctx, kPane, "Run"));
    IM_CHECK_EQ(info().status, (int)DSG_PV_RUNNING);
    IM_CHECK(ui_animating());
    ctx->Yield(5);
    IM_CHECK(dsg_preview_where_now(ui_preview(), &b));
    IM_CHECK_GT(b.frame, a.frame + 1);
}

void test_edit_restart(ImGuiTestContext* ctx) {
    Double dbl;
    IM_CHECK(start(ctx, "preview_edit", false));
    char run0[DSG_PATH_MAX];
    snprintf(run0, sizeof run0, "%s", info().out_dir);
    IM_CHECK(run0[0] != 0);
    // A value typed in the dots' panel, as a person does.
    dsg_sel s = {};
    dsg_sel_def(&s, "/stimuli/dots");
    ui_reveal(&s);
    ctx->Yield(2);
    ctx->ItemClick(te_in_child(ctx, "//##designer/Properties", "stimulus/##keys/coherence/##v"));
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
    ctx->KeyChars("0.5");
    ctx->KeyPress(ImGuiKey_Enter);
    size_t n = 0;
    IM_CHECK(strstr(ui_text(&n), "\"coherence\": 0.5") != nullptr);
    // Not before the settle: the session shown is the old one.
    ctx->Yield(3);
    IM_CHECK_STR_EQ(info().out_dir, run0);
    IM_CHECK_EQ(info().status, (int)DSG_PV_RUNNING);
    for (int i = 0; i < 240 && strcmp(info().out_dir, run0) == 0; i++) ctx->Yield();
    IM_CHECK_STR_NE(info().out_dir, run0);
    IM_CHECK_EQ(info().status, (int)DSG_PV_RUNNING);
}

void test_choice(ImGuiTestContext* ctx) {
    Double dbl;
    IM_CHECK(start(ctx, "preview_choice", false));
    // the running template's choices: a trial must run
    to_response(ctx);
    IM_CHECK_EQ(dsg_preview_choices(ui_preview()), 2);
    IM_CHECK_STR_EQ(dsg_preview_choice_label(ui_preview(), 1), "right");
    dsg_preview_where w = {};
    IM_CHECK(dsg_preview_where_now(ui_preview(), &w));
    const int trial0 = w.trial;
    ctx->ItemClick(te_in_child(ctx, kPane, "$$1/##choice"));
    IM_CHECK_STR_EQ(last_response(ctx, trial0), "right");
}

void test_stale(ImGuiTestContext* ctx) {
    Double dbl;
    IM_CHECK(start(ctx, "preview_stale", false));
    char run0[DSG_PATH_MAX];
    snprintf(run0, sizeof run0, "%s", info().out_dir);
    IM_CHECK(!ctx->ItemExists(te_in_child(ctx, kPane, "##stale")));
    // A stray character at the text's end, typed in the editor.
    ctx->ItemClick(te_in_child(ctx, "//##designer/Definition", "##definition"));
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_End);
    ctx->KeyChars("x");
    for (int i = 0; i < 60 && !info().stale; i++) ctx->Yield();
    const dsg_preview_info in = info();
    IM_CHECK(in.stale);
    IM_CHECK_GT(in.stale_errors, 0);
    IM_CHECK_EQ(in.status, (int)DSG_PV_RUNNING);
    IM_CHECK(ctx->ItemExists(te_in_child(ctx, kPane, "##stale")));
    // Past the settle the shown session stays: nothing to load.
    ctx->Yield(30);
    IM_CHECK_STR_EQ(info().out_dir, run0);
    IM_CHECK_EQ(info().status, (int)DSG_PV_RUNNING);
    // Ctrl+Z: accepted again, a new session after the settle.
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    for (int i = 0; i < 240 && strcmp(info().out_dir, run0) == 0; i++) ctx->Yield();
    IM_CHECK(!info().stale);
    IM_CHECK_STR_NE(info().out_dir, run0);
    ctx->Yield();
    IM_CHECK(!ctx->ItemExists(te_in_child(ctx, kPane, "##stale")));
}

} // namespace

void te_register_preview(ImGuiTestEngine* e) {
    IM_REGISTER_TEST(e, "preview", "key_space")->TestFunc = test_key_space;
    IM_REGISTER_TEST(e, "preview", "pause")->TestFunc = test_pause;
    IM_REGISTER_TEST(e, "preview", "edit_restart")->TestFunc = test_edit_restart;
    IM_REGISTER_TEST(e, "preview", "choice")->TestFunc = test_choice;
    IM_REGISTER_TEST(e, "preview", "stale")->TestFunc = test_stale;
}
