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
//   preview/steady        At Fit, the display keeps its size while rows
//                         above it come and go: the last trial's row and
//                         the stale banner (hand test, 2026-11-21).
//   preview/zoom          The wheel over the display zooms about the
//                         cursor, step by step to a whole zoom; a middle
//                         drag pans and claims no keyboard; the cursor's
//                         participant px follow the zoom and the pan
//                         (2026-11-23, "Preview size").
//   preview/split         A drag of the splitter widens the AUX pane, and
//                         the view file keeps the width.
//   preview/details       The Details check box folds the details: the
//                         display grows into their room, and keeps its size
//                         while the stale banner comes and goes.
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

#include "imgui_internal.h"

#include <float.h>
#include <math.h>
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

ImVec2 screen_size(ImGuiTestContext* ctx) {
    const ImGuiTestItemInfo it = ctx->ItemInfo(ctx->GetID("##screen", ctx->WindowInfo(kDisplay).ID));
    return it.ID ? it.RectFull.GetSize() : ImVec2(0, 0);
}

void test_steady(ImGuiTestContext* ctx) {
    Double dbl;
    IM_CHECK(start(ctx, "preview_steady", false));
    // A trial with no last trial yet: the first.
    dsg_preview_where w = {};
    for (int i = 0; i < 240 && !(dsg_preview_where_now(ui_preview(), &w) && !w.outcome); i++) ctx->Yield();
    IM_CHECK(w.outcome == nullptr);
    const ImVec2 s0 = screen_size(ctx);
    IM_CHECK_GT(s0.x, 0.0f);
    for (int i = 0; i < 600 && !(dsg_preview_where_now(ui_preview(), &w) && w.outcome); i++) ctx->Yield();
    IM_CHECK(w.outcome != nullptr);
    ImVec2 s1 = screen_size(ctx);
    IM_CHECK_EQ(s1.x, s0.x);
    IM_CHECK_EQ(s1.y, s0.y);
    // The stale banner moves the display down, at the same size.
    ctx->ItemClick(te_in_child(ctx, "//##designer/Definition", "##definition"));
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_End);
    ctx->KeyChars("x");
    for (int i = 0; i < 60 && !info().stale; i++) ctx->Yield();
    IM_CHECK(info().stale);
    ctx->Yield();
    IM_CHECK(ctx->ItemExists(te_in_child(ctx, kPane, "##stale")));
    s1 = screen_size(ctx);
    IM_CHECK_EQ(s1.x, s0.x);
    IM_CHECK_EQ(s1.y, s0.y);
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
}

// --- preview size (2026-11-23) ------------------------------------------------------

// A window by its path, child windows too; NULL when it is not there.
ImGuiWindow* win(ImGuiTestContext* ctx, const char* path) { return ctx->WindowInfo(path, ImGuiTestOpFlags_NoError).Window; }

// The image's rectangle, which the display's child clips: headless, the gray
// box that stands in for the texture. ImGui 1.93 fills a box whose corners
// are not on whole px with an antialiased fringe: opaque vertices 0.5 px
// inside the edge, clear ones 0.5 px outside; the edge is between them.
bool image_rect(ImGuiTestContext* ctx, ImVec2* mn, ImVec2* mx) {
    ImGuiWindow* w = win(ctx, kDisplay);
    if (!w) return false;
    ImVec2 o0(FLT_MAX, FLT_MAX), o1(-FLT_MAX, -FLT_MAX), c0(FLT_MAX, FLT_MAX), c1(-FLT_MAX, -FLT_MAX);
    bool opaque = false, clear = false;
    for (const ImDrawVert& v : w->DrawList->VtxBuffer) {
        if (v.col == IM_COL32(96, 96, 96, 255)) {
            o0 = ImMin(o0, v.pos);
            o1 = ImMax(o1, v.pos);
            opaque = true;
        } else if (v.col == IM_COL32(96, 96, 96, 0)) {
            c0 = ImMin(c0, v.pos);
            c1 = ImMax(c1, v.pos);
            clear = true;
        }
    }
    *mn = clear ? ImVec2((o0.x + c0.x) * 0.5f, (o0.y + c0.y) * 0.5f) : o0;
    *mx = clear ? ImVec2((o1.x + c1.x) * 0.5f, (o1.y + c1.y) * 0.5f) : o1;
    return opaque;
}

dsg_preview_settings settings() {
    dsg_preview_settings s = {};
    dsg_preview_get(ui_preview(), &s);
    return s;
}

// The participant px under pos in the image drawn last.
ImVec2 px_at(ImGuiTestContext* ctx, ImVec2 pos) {
    ImVec2 a, b;
    if (!image_rect(ctx, &a, &b)) return ImVec2(-1, -1);
    const dsg_preview_info in = info();
    return ImVec2((pos.x - a.x) * (float)in.width / (b.x - a.x), (pos.y - a.y) * (float)in.height / (b.y - a.y));
}

void test_zoom(ImGuiTestContext* ctx) {
    Double dbl;
    IM_CHECK(start(ctx, "preview_zoom", false));
    ImVec2 a0, b0;
    IM_CHECK(image_rect(ctx, &a0, &b0));
    IM_CHECK_EQ(settings().scale, (int)DSG_PV_SCALE_FIT);
    const float W = (float)info().width;
    const float fit = (b0.x - a0.x) / W;
    IM_CHECK_LT(fit, 1.0f);
    // a point off the middle, so the zoom's anchor shows
    const ImVec2 p(floorf(a0.x + (b0.x - a0.x) * 0.3f), floorf(a0.y + (b0.y - a0.y) * 0.4f));
    ctx->MouseMoveToPos(p);
    const ImVec2 u0 = px_at(ctx, p);
    float x = 0, y = 0;
    IM_CHECK(ui_preview_pane_pointer(&x, &y));
    IM_CHECK(fabsf(x - u0.x) < 1e-3f && fabsf(y - u0.y) < 1e-3f);
    // One notch: the next step above Fit. Fit puts the image at the area's
    // start, and an image smaller than the area stays inside it, so the
    // point under the cursor may move on this step.
    ctx->MouseWheelY(1.0f);
    ctx->Yield(2);
    dsg_preview_settings s = settings();
    IM_CHECK_EQ(s.scale, (int)DSG_PV_SCALE_ZOOM);
    IM_CHECK_GT(s.zoom, (double)fit);
    ImVec2 a1, b1;
    IM_CHECK(image_rect(ctx, &a1, &b1));
    IM_CHECK(fabsf((b1.x - a1.x) - W * (float)s.zoom) < 0.01f);
    IM_CHECK(ui_preview_pane_pointer(&x, &y));
    const ImVec2 u1 = px_at(ctx, p);
    IM_CHECK(fabsf(x - u1.x) < 1e-3f && fabsf(y - u1.y) < 1e-3f);
    // Up to 2, a whole zoom, one step at a time. On an axis where the image
    // was larger than the area before the step, the px under the cursor
    // stays within the half device px that the corner snaps to.
    const ImRect area0 = win(ctx, kDisplay)->Rect();
    ImVec2 u2 = u1;
    int held = 0;
    for (int i = 0; i < 8 && settings().zoom < 2.0; i++) {
        ImVec2 pa, pb;
        IM_CHECK(image_rect(ctx, &pa, &pb));
        const ImRect area = win(ctx, kDisplay)->Rect();   // its height follows the zoom
        const bool wide = pa.x <= area0.Min.x && pb.x >= area0.Max.x, tall = pa.y <= area.Min.y && pb.y >= area.Max.y;
        ctx->MouseWheelY(1.0f);
        ctx->Yield(2);
        const ImVec2 u = px_at(ctx, p);
        const float tol = 0.5f / (float)settings().zoom + 0.01f;
        if (wide) IM_CHECK_LE(fabsf(u.x - u2.x), tol);
        if (tall) IM_CHECK_LE(fabsf(u.y - u2.y), tol);
        held += wide && tall;
        u2 = u;
    }
    IM_CHECK_GE(held, 2);   // the steps to 1 and to 2 at least
    s = settings();
    IM_CHECK_EQ(s.zoom, 2.0);
    IM_CHECK(image_rect(ctx, &a1, &b1));
    IM_CHECK_EQ(b1.x - a1.x, 2.0f * W);
    IM_CHECK(a1.x == floorf(a1.x) && a1.y == floorf(a1.y));
    // A middle drag pans: the image moves with the cursor, so the px under
    // it stays; no keyboard claim.
    IM_CHECK(!ui_preview_has_keyboard());
    ctx->MouseDown(ImGuiMouseButton_Middle);
    ctx->MouseMoveToPos(ImVec2(p.x - 80, p.y - 50));
    ctx->MouseUp(ImGuiMouseButton_Middle);
    ctx->Yield();
    ImVec2 a2, b2;
    IM_CHECK(image_rect(ctx, &a2, &b2));
    IM_CHECK_EQ(a2.x - a1.x, -80.0f);
    IM_CHECK_EQ(a2.y - a1.y, -50.0f);
    IM_CHECK(!ui_preview_has_keyboard());
    IM_CHECK(ui_preview_pane_pointer(&x, &y));
    IM_CHECK(fabsf(x - u2.x) < 1e-3f && fabsf(y - u2.y) < 1e-3f);
    // the mapping at another place: (cursor - corner) / 2, exact
    const ImVec2 area = win(ctx, kDisplay)->Pos;
    const ImVec2 q(area.x + 51, area.y + 37);
    ctx->MouseMoveToPos(q);
    ctx->Yield();
    IM_CHECK(ui_preview_pane_pointer(&x, &y));
    IM_CHECK_EQ(x, (q.x - a2.x) * 0.5f);
    IM_CHECK_EQ(y, (q.y - a2.y) * 0.5f);
    // a left click on the zoomed display still claims the keyboard
    ctx->MouseClick(ImGuiMouseButton_Left);
    IM_CHECK(ui_preview_has_keyboard());
    ui_preview_key(kEscape, true, false);
    ui_preview_key(kEscape, false, false);
    // Fit again: the whole display in the pane
    ctx->ItemClick(te_in_child(ctx, kPane, "Fit"));
    ctx->Yield();
    IM_CHECK(image_rect(ctx, &a1, &b1));
    IM_CHECK_EQ(a1.x, a0.x);
    IM_CHECK_EQ(b1.x, b0.x);
    // Below Fit the image is smaller than the area: a drag moves it inside
    // the area, and Fit puts it back at the start.
    ctx->MouseMoveToPos(ImVec2((a1.x + b1.x) * 0.5f, (a1.y + b1.y) * 0.5f));
    ctx->MouseWheelY(-1.0f);
    ctx->Yield(2);
    IM_CHECK_LT(settings().zoom, (double)fit);
    IM_CHECK(image_rect(ctx, &a1, &b1));
    ctx->MouseMoveToPos(ImVec2((a1.x + b1.x) * 0.5f, (a1.y + b1.y) * 0.5f));
    const ImRect area1 = win(ctx, kDisplay)->Rect();
    const float room_x = area1.Max.x - b1.x;   // how far it can go right
    IM_CHECK_GT(room_x, 20.0f);
    ctx->MouseDown(ImGuiMouseButton_Middle);
    ctx->MouseMoveToPos(ImVec2((a1.x + b1.x) * 0.5f + 1000.0f, (a1.y + b1.y) * 0.5f));
    ctx->MouseUp(ImGuiMouseButton_Middle);
    ctx->Yield();
    IM_CHECK(image_rect(ctx, &a2, &b2));
    IM_CHECK_LE(fabsf(b2.x - area1.Max.x), 1.0f);   // at the area's right edge, not past it
    ctx->ItemClick(te_in_child(ctx, kPane, "Fit"));
    ctx->Yield();
    IM_CHECK(image_rect(ctx, &a1, &b1));
    IM_CHECK_EQ(a1.x, a0.x);
}

float pane_width(ImGuiTestContext* ctx) {
    ImGuiWindow* w = win(ctx, kPane);
    return w ? w->Size.x : 0.0f;
}

void test_split(ImGuiTestContext* ctx) {
    Double dbl;
    IM_CHECK(start(ctx, "preview_split", false));
    char dir[DSG_PATH_MAX], other[DSG_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", ui_project()->folder);
    static const char* const files[] = { "pack.json", "experiment.json", "conditions.csv", nullptr };
    IM_CHECK(te_copy_project("rdk", "preview_split_other", files, other, sizeof other));
    const float w0 = pane_width(ctx);
    IM_CHECK_GT(w0, 0.0f);
    const float main0 = win(ctx, "//##designer/Definition")->Size.x;
    ctx->ItemDragWithDelta("//##designer/##aux_split", ImVec2(-150, 0));
    ctx->Yield();
    const float w1 = pane_width(ctx);
    IM_CHECK_LE(fabsf(w1 - w0 - 150.0f), 1.0f);
    IM_CHECK_LE(fabsf(win(ctx, "//##designer/Definition")->Size.x - (main0 - 150.0f)), 1.0f);
    // kept: the view file has it when another project opens and this one
    // opens again (the open of the other saves this one's view file)
    IM_CHECK(ui_save());
    te_open(ctx, other);
    ctx->Yield();
    te_open(ctx, dir);
    ctx->Yield(2);
    IM_CHECK(ui_aux_open());
    IM_CHECK_LE(fabsf(pane_width(ctx) - w1), 1.0f);
    // bounds: a drag far to the left keeps the main view's least width
    ctx->ItemDragWithDelta("//##designer/##aux_split", ImVec2(-3000, 0));
    ctx->Yield();
    IM_CHECK_GE(win(ctx, "//##designer/Definition")->Size.x, 200.0f);
}

void test_details(ImGuiTestContext* ctx) {
    Double dbl;
    IM_CHECK(start(ctx, "preview_details", false));
    // a tall display, so Fit is bound by the height
    char run0[DSG_PATH_MAX];
    snprintf(run0, sizeof run0, "%s", info().out_dir);
    dsg_preview_settings s = settings();
    s.session.width = 600;
    s.session.height = 1200;
    dsg_preview_set(ui_preview(), &s);
    for (int i = 0; i < 240 && !(strcmp(info().out_dir, run0) != 0 && info().status == DSG_PV_RUNNING); i++) ctx->Yield();
    IM_CHECK_EQ(info().height, 1200);
    ctx->Yield();
    const ImVec2 s0 = screen_size(ctx);
    IM_CHECK_GT(s0.y, 0.0f);
    ImGuiWindow* d = win(ctx, "//##designer/Preview/##details");
    IM_CHECK(d != nullptr && d->Active);
    ctx->ItemClick(te_in_child(ctx, kPane, "Details"));
    ctx->Yield(2);
    IM_CHECK(!settings().details);
    d = win(ctx, "//##designer/Preview/##details");
    IM_CHECK(d == nullptr || !d->Active);
    const ImVec2 s1 = screen_size(ctx);
    IM_CHECK_GT(s1.y, s0.y + 50.0f);
    // the fixed rows hold folded too: the stale banner moves the display down
    // at the same size
    ctx->ItemClick(te_in_child(ctx, "//##designer/Definition", "##definition"));
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_End);
    ctx->KeyChars("x");
    for (int i = 0; i < 60 && !info().stale; i++) ctx->Yield();
    IM_CHECK(info().stale);
    ctx->Yield();
    IM_CHECK(ctx->ItemExists(te_in_child(ctx, kPane, "##stale")));
    const ImVec2 s2 = screen_size(ctx);
    IM_CHECK_EQ(s2.x, s1.x);
    IM_CHECK_EQ(s2.y, s1.y);
    // Unfolded again, the display gives the room back. (Not compared with
    // s0: the pane's own height follows the toolbar's status line.)
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    for (int i = 0; i < 240 && info().stale; i++) ctx->Yield();
    ctx->Yield();
    const ImVec2 s3 = screen_size(ctx);
    ctx->ItemClick(te_in_child(ctx, kPane, "Details"));
    ctx->Yield(2);
    IM_CHECK(settings().details);
    const ImVec2 s4 = screen_size(ctx);
    IM_CHECK_LT(s4.y, s3.y - 50.0f);
}

} // namespace

void te_register_preview(ImGuiTestEngine* e) {
    IM_REGISTER_TEST(e, "preview", "key_space")->TestFunc = test_key_space;
    IM_REGISTER_TEST(e, "preview", "pause")->TestFunc = test_pause;
    IM_REGISTER_TEST(e, "preview", "edit_restart")->TestFunc = test_edit_restart;
    IM_REGISTER_TEST(e, "preview", "choice")->TestFunc = test_choice;
    IM_REGISTER_TEST(e, "preview", "stale")->TestFunc = test_stale;
    IM_REGISTER_TEST(e, "preview", "steady")->TestFunc = test_steady;
    IM_REGISTER_TEST(e, "preview", "zoom")->TestFunc = test_zoom;
    IM_REGISTER_TEST(e, "preview", "split")->TestFunc = test_split;
    IM_REGISTER_TEST(e, "preview", "details")->TestFunc = test_details;
}
