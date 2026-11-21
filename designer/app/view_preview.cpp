// view_preview.cpp - the preview's pane (designer step 5, part B;
// docs/designer.md, "Plan of step 5" and "Part B as built"). OWNED BY PART B.
//
// The pane only shows and controls: ui.cpp ticks the preview (preview.h)
// once a frame before the views draw, so the session steps whether or not
// this view draws, and a hidden pane pauses it. The settings live in the
// preview, and ui.cpp keeps them in the view file's part "preview"; the
// pane reads them with dsg_preview_get() and changes them with
// dsg_preview_set(). Names from the definition and the data set's folder are
// user text, so Skribidi draws them (skt.h).
#include "views.h"
#include "skt.h"
#include "player.h"   // PL_NEED_FIELD

#include "imgui.h"
#include "imgui_internal.h"   // ItemAdd, RenderTextWrapped, BeginComboPreview

// skb_common.h tests __cplusplus__ (sic); see view_chart.cpp.
#include "skribidi/skb_layout.h"
#undef SKB_LITERAL
#define SKB_LITERAL(type) type

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace {

const ImU32 kText = IM_COL32(235, 235, 240, 255);
const ImU32 kBannerInfo = IM_COL32(40, 80, 140, 255), kBannerStale = IM_COL32(130, 40, 40, 255);
const ImU32 kBannerNote = IM_COL32(110, 85, 30, 255);
const ImU32 kNoImage = IM_COL32(96, 96, 96, 255), kNoFrame = IM_COL32(16, 16, 18, 255);
const ImU32 kKeys = IM_COL32(255, 200, 80, 255);
const ImVec4 kKeysText(1.0f, 0.78f, 0.31f, 1.0f);
const double kSpeeds[] = { 0.25, 0.5, 1.0, 2.0 };
const char* const kSpeedNames[] = { "0.25x", "0.5x", "1x", "2x" };
// The bounds of the view file's part (preview_part.c), so a value typed here
// reads back the same.
const int kMaxSide = 16384, kMaxHz = 1000;

struct Pane {
    // The cursor last given to the preview: a still mouse sends nothing, so
    // a paused display gets no input.
    bool pointer_in = false;
    float px = 0, py = 0;
    // Fields being edited: refreshed from the settings while not active.
    int display[2] = { 0, 0 };
    int hz = 0;
    char seed[24] = "";
    char participant[32] = "";
    float rt_s = 0;
    int pin_row = 0;
    char fields[DSG_SESSION_FIELDS][256] = {};
};
Pane P;

dsg_preview* pv() { return ui_preview(); }

void apply(const dsg_preview_settings& s) { dsg_preview_set(pv(), &s); }

// User text on the current line, as an item, so SameLine() follows it. The
// log line lets a test read it as it reads ImGui's own text.
void user_text(const char* s) {
    if (!s || !s[0]) return;
    ImGui::LogText("%s", s);
    const skb_layout_t* l = skt_layout(s, ui_text_size());
    const float h = ImGui::GetTextLineHeight();
    if (!l) {
        ImGui::TextUnformatted("?");
        return;
    }
    const skb_rect2_t b = skb_layout_get_bounds(l);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(b.width, h));
    skt_draw_layout(ImGui::GetWindowDrawList(), l, p.x, p.y + (h - b.height) * 0.5f - b.y, 1.0f, kText);
}

// A colored band the width of the pane, as the chart's read-only banner
// (view_chart.cpp says why a band, not gray text). id lets the Test Engine
// find it; RenderTextWrapped() puts the text in ImGui's log for the tests.
void banner(const char* id, ImU32 bg, const char* text) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImMax(ImGui::GetContentRegionAvail().x, 50.0f);
    const float wrap = w - 2 * st.FramePadding.x;
    const ImVec2 ts = ImGui::CalcTextSize(text, nullptr, false, wrap);
    const ImRect r(p, ImVec2(p.x + w, p.y + ts.y + 2 * st.FramePadding.y));
    ImGui::ItemSize(r);
    if (!ImGui::ItemAdd(r, ImGui::GetID(id))) return;
    ImGui::GetWindowDrawList()->AddRectFilled(r.Min, r.Max, bg, st.FrameRounding);
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::RenderTextWrapped(ImVec2(p.x + st.FramePadding.x, p.y + st.FramePadding.y), text, nullptr, wrap);
    ImGui::PopStyleColor();
}

// --- the status -------------------------------------------------------------------

// Gives the height of the rows that come and go (the issue), which the
// display's Fit size leaves out.
float status_line(const dsg_preview_info& in) {
    switch (in.status) {
    case DSG_PV_NONE: ImGui::TextWrapped("No session. The preview starts when the check accepts the definition."); break;
    case DSG_PV_WAITING: ImGui::TextWrapped("Waiting: the session loads."); break;
    case DSG_PV_ATTACHING: ImGui::TextWrapped("Attaching: the session's display is being made."); break;
    case DSG_PV_RUNNING: ImGui::TextWrapped("Running."); break;
    case DSG_PV_PAUSED: ImGui::TextWrapped("Paused."); break;
    case DSG_PV_ENDED: ImGui::TextWrapped("Session over: %d trials. Press Restart to run it again.", in.trials); break;
    case DSG_PV_FAILED: ImGui::TextWrapped("Failed: the Issues panel says why. Fix the definition, or press Restart."); break;
    default: ImGui::TextWrapped("Unknown state %d.", in.status); break;
    }
    dsg_issue is = {};
    float varying = 0.0f;
    if (dsg_preview_issue(pv(), &is) && is.message) {
        const float y0 = ImGui::GetCursorPosY();
        ImGui::TextDisabled("Issue:");
        ImGui::SameLine();
        user_text(is.message);
        varying = ImGui::GetCursorPosY() - y0;
    }
    // One row whether or not there is a note: "slow" comes and goes while a
    // session runs, and a row that comes and goes moves the display (hand
    // test, 2026-11-21).
    if (in.status == DSG_PV_RUNNING && in.slow) ImGui::TextColored(kKeysText, "Slower than real time: frames are dropped.");
    else if (in.attaching) ImGui::TextDisabled("A new session is being attached.");
    else if (in.loading && in.status != DSG_PV_WAITING) ImGui::TextDisabled("A new session loads. This one runs until it is ready.");
    else ImGui::NewLine();
    return varying;
}

// Where the shown session is.
void where_line() {
    // Two rows always, for the same reason as the status's note row.
    dsg_preview_where w = {};
    if (!dsg_preview_where_now(pv(), &w)) {
        ImGui::NewLine();
        ImGui::NewLine();
        return;
    }
    ImGui::TextDisabled("Trial %d", w.trial + 1);
    if (w.leaf) {
        ImGui::SameLine();
        user_text(w.leaf);
        if (w.condition >= 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("row %d", w.condition + 1);
        }
    }
    if (w.tmpl) {
        ImGui::SameLine();
        ImGui::TextDisabled("template");
        ImGui::SameLine();
        user_text(w.tmpl);
    }
    if (w.state) {
        ImGui::SameLine();
        ImGui::TextDisabled("state");
        ImGui::SameLine();
        user_text(w.state);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("frame %lld, %.3f s", (long long)w.frame, (double)w.t_ns * 1e-9);
    if (!w.outcome) ImGui::TextDisabled("Last trial: none yet.");
    else {
        ImGui::TextDisabled("Last trial: %s", w.outcome);
        if (w.response && w.response[0]) {
            ImGui::SameLine();
            ImGui::TextDisabled("response");
            ImGui::SameLine();
            user_text(w.response);
            ImGui::SameLine();
            ImGui::TextDisabled("rt %.3f s", (double)w.rt_ns * 1e-9);
        }
    }
}

// --- the controls -----------------------------------------------------------------

void controls(const dsg_preview_info& in, dsg_preview_settings& ps) {
    const bool have = in.status == DSG_PV_RUNNING || in.status == DSG_PV_PAUSED;
    ImGui::BeginDisabled(in.status != DSG_PV_PAUSED);
    if (ImGui::Button("Run")) dsg_preview_run(pv());
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Run the session in real time, at the speed chosen");
    ImGui::SameLine();
    ImGui::BeginDisabled(in.status != DSG_PV_RUNNING);
    if (ImGui::Button("Pause")) dsg_preview_pause(pv());
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Stop the frames; the display keeps the last one");
    ImGui::SameLine();
    ImGui::BeginDisabled(!have);
    if (ImGui::Button("Step")) dsg_preview_step(pv());
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Show one more frame, then pause");
    ImGui::SameLine();
    if (ImGui::Button("Restart")) dsg_preview_restart(pv());
    ImGui::SetItemTooltip("Load the session again, with the same seed and trial");
    ImGui::SameLine();
    if (ImGui::Button("New seed")) {
        // splitmix64 of the clock and the old seed: any 63-bit value
        uint64_t z = (uint64_t)time(nullptr) + ps.session.seed + 0x9e3779b97f4a7c15ull;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        z = (z ^ (z >> 31)) & 0x7fffffffffffffffull;
        ps.session.seed = z ? z : 1;
        apply(ps);
    }
    ImGui::SetItemTooltip("Draw a new seed and restart");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    int sp = 2;
    for (int i = 0; i < 4; i++)
        if (fabs(ps.speed - kSpeeds[i]) < 1e-9) sp = i;
    if (ImGui::Combo("Speed", &sp, kSpeedNames, 4)) {
        ps.speed = kSpeeds[sp];
        apply(ps);
    }
    ImGui::SetItemTooltip("Session time per real time");
    ImGui::SameLine();
    if (ImGui::RadioButton("Fit", ps.scale == DSG_PV_SCALE_FIT)) {
        ps.scale = DSG_PV_SCALE_FIT;
        apply(ps);
    }
    ImGui::SetItemTooltip("Scale the display to the pane");
    ImGui::SameLine();
    if (ImGui::RadioButton("1:1", ps.scale == DSG_PV_SCALE_1TO1)) {
        ps.scale = DSG_PV_SCALE_1TO1;
        apply(ps);
    }
    ImGui::SetItemTooltip("One participant pixel to one pixel of this monitor");
}

// --- the display ------------------------------------------------------------------

float snap(float v, float scale) { return floorf(v * scale + 0.5f) / scale; }

void draw_display(const dsg_preview_info& in, const dsg_preview_settings& ps, float height) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 fb(io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.0f,
                    io.DisplayFramebufferScale.y > 0 ? io.DisplayFramebufferScale.y : 1.0f);
    // the texture's size, else the shown session's, else the settings'
    int32_t W = in.width > 0 ? in.width : ps.session.width;
    int32_t H = in.height > 0 ? in.height : ps.session.height;
    if (W <= 0 || H <= 0) W = 1920, H = 1080;
    // Headless (no GL caps) the sessions draw on gfx's null backend: no
    // texture, a gray box.
    const bool gl = ui_preview_gl()->caps != nullptr;
    uint32_t tex = 0;
    int32_t tw = 0, th = 0;
    const bool have_tex = gl && dsg_preview_texture(pv(), &tex, &tw, &th) && tex != 0 && tw > 0 && th > 0;
    if (have_tex) W = tw, H = th;

    const bool one = ps.scale == DSG_PV_SCALE_1TO1;
    const float avail_w = ImMax(ImGui::GetContentRegionAvail().x, 40.0f);
    // framebuffer px per participant px: Fit is at most 1, never a blow-up
    float s = 1.0f;
    if (!one) s = ImMin(1.0f, ImMin(avail_w * fb.x / (float)W, height * fb.y / (float)H));
    const ImVec2 size((float)W * s / fb.x, (float)H * s / fb.y);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##display", ImVec2(0, one ? height : ImMin(height, size.y)), ImGuiChildFlags_None,
                      one ? ImGuiWindowFlags_HorizontalScrollbar : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    // at a whole framebuffer px, so at 1:1 a participant px is a device px
    const ImVec2 c = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(snap(c.x, fb.x), snap(c.y, fb.y)));
    ImGui::InvisibleButton("##screen", size);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (have_tex) {
        const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        // Decision 6: nearest at 1:1, so the bytes are exact; ImGui's linear
        // sampler at Fit. The texture has no mipmaps.
        if (one && pio.DrawCallback_SetSamplerNearest) dl->AddCallback(pio.DrawCallback_SetSamplerNearest, nullptr);
        // Rows bottom up (GL's order). A white tint and no style alpha, or
        // ImGui's blend mixes the device codes with the background.
        dl->AddImage(ImTextureRef((ImTextureID)tex), a, b, ImVec2(0, 1), ImVec2(1, 0), IM_COL32_WHITE);
        if (one && pio.DrawCallback_SetSamplerLinear) dl->AddCallback(pio.DrawCallback_SetSamplerLinear, nullptr);
    } else {
        dl->AddRectFilled(a, b, gl ? kNoFrame : kNoImage);
        const char* t = gl ? "No frame yet." : "No image: the session draws with no GL.";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        ImGui::LogText("%s", t);
        dl->AddText(ImVec2(a.x + ImMax(0.0f, (size.x - ts.x) * 0.5f), a.y + ImMax(0.0f, (size.y - ts.y) * 0.5f)), kText, t);
    }
    if (in.keyboard) dl->AddRect(a, b, kKeys, 0.0f, 2.0f);

    // A click gives the session the keyboard; Escape gives it back.
    if (ImGui::IsItemActivated()) dsg_preview_set_keyboard(pv(), true);
    // The cursor in participant px from the top-left corner.
    if (ImGui::IsItemHovered()) {
        const float x = (io.MousePos.x - a.x) * (float)W / size.x;
        const float y = (io.MousePos.y - a.y) * (float)H / size.y;
        if (!P.pointer_in || x != P.px || y != P.py) {
            P.pointer_in = true;
            P.px = x;
            P.py = y;
            dsg_preview_pointer(pv(), x, y, true);
        }
    } else if (P.pointer_in) {
        P.pointer_in = false;
        dsg_preview_pointer(pv(), P.px, P.py, false);
    }
    ImGui::EndChild();
    if (!one && s < 1.0f) ImGui::TextDisabled("Scaled: fine patterns alias; use 1:1");
}

// --- the choice strip -------------------------------------------------------------

// A button per choice of the running template, for choices that a keyboard
// cannot give (a button box, an XID). Held as long as the mouse holds it.
void choice_strip() {
    const int n = dsg_preview_choices(pv());
    if (n <= 0) return;
    ImGui::TextDisabled("Choices:");
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const float h = ImGui::GetFrameHeight();
    for (int i = 0; i < n; i++) {
        const char* label = dsg_preview_choice_label(pv(), i);
        const skb_layout_t* l = label ? skt_layout(label, ui_text_size()) : nullptr;
        const skb_rect2_t lb = l ? skb_layout_get_bounds(l) : skb_rect2_t{};
        ImGui::SameLine();
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Button("##choice", ImVec2(ImMax(lb.width, 12.0f) + 2 * pad.x, h));
        if (ImGui::IsItemActivated()) dsg_preview_choice(pv(), i, true);
        if (ImGui::IsItemDeactivated()) dsg_preview_choice(pv(), i, false);
        if (l) {
            ImGui::LogText("%s", label);
            skt_draw_layout(ImGui::GetWindowDrawList(), l, p.x + pad.x, p.y + (h - lb.height) * 0.5f - lb.y, 1.0f, kText);
        }
        ImGui::SetItemTooltip("Press this choice");
        ImGui::PopID();
    }
}

// --- the settings -----------------------------------------------------------------

bool editing(const char* label) { return ImGui::GetCurrentContext()->ActiveId == ImGui::GetID(label); }

// The pin: the trial that the person looks at.
void pin_fields(dsg_preview_settings& ps) {
    const int nl = dsg_preview_leaves(pv());
    int rows = 0;
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("Trial", ps.session.pin_leaf[0] ? "" : "Any: from the start")) {
        if (ImGui::Selectable("Any: from the start", !ps.session.pin_leaf[0])) {
            ps.session.pin_leaf[0] = 0;
            ps.session.pin_row = -1;
            apply(ps);
        }
        for (int i = 0; i < nl; i++) {
            dsg_preview_leaf lf = {};
            if (!dsg_preview_leaf_at(pv(), i, &lf) || !lf.path) continue;
            ImGui::PushID(i);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##leaf", strcmp(lf.path, ps.session.pin_leaf) == 0, 0, ImVec2(0, ImGui::GetFrameHeight()))) {
                snprintf(ps.session.pin_leaf, sizeof ps.session.pin_leaf, "%s", lf.path);
                ps.session.pin_row = -1;
                apply(ps);
            }
            char line[600];
            snprintf(line, sizeof line, "%s  %s", lf.path, lf.tmpl ? lf.tmpl : "");
            skt_draw_layout(ImGui::GetWindowDrawList(), skt_layout(line, ui_text_size()), at.x + 2, at.y + 1, 1.0f, kText);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ps.session.pin_leaf[0] && ImGui::BeginComboPreview()) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        skt_draw_layout(ImGui::GetWindowDrawList(), skt_layout(ps.session.pin_leaf, ui_text_size()), at.x, at.y - 1, 1.0f, kText);
        ImGui::EndComboPreview();
    }
    ImGui::SetItemTooltip("The trial to show: the session runs to the first trial of this part of the timeline");
    for (int i = 0; i < nl; i++) {
        dsg_preview_leaf lf = {};
        if (dsg_preview_leaf_at(pv(), i, &lf) && lf.path && strcmp(lf.path, ps.session.pin_leaf) == 0) rows = lf.rows;
    }
    if (!ps.session.pin_leaf[0] || rows <= 0) return;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    if (!editing("Row")) P.pin_row = ps.session.pin_row < 0 ? 0 : ps.session.pin_row + 1;
    ImGui::InputInt("Row", &P.pin_row, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit() && P.pin_row >= 0 && P.pin_row <= rows) {
        ps.session.pin_row = P.pin_row - 1;
        apply(ps);
    }
    ImGui::SetItemTooltip("The table's row, 1 to %d; 0 for any row", rows);
}

// The session fields that a real run needs (the check's needs), with the
// check's stand-ins (integer 1, text x) while empty.
void field_rows(dsg_preview_settings& ps) {
    const dsg_report* r = ui_report();
    int shown = 0;
    for (int i = 0; r && i < r->n_issues && shown < DSG_SESSION_FIELDS; i++) {
        const dsg_issue& is = r->issues[i];
        if (is.from != DSG_FROM_RUN || is.object != PL_NEED_FIELD || !is.name || !is.name[0]) continue;
        if (shown == 0) ImGui::TextDisabled("Session fields");
        char* buf = P.fields[shown++];
        int f = -1;
        for (int k = 0; k < ps.session.n_fields; k++)
            if (strcmp(ps.session.fields[k].name, is.name) == 0) f = k;
        ImGui::PushID(is.name);
        if (!editing("##value")) snprintf(buf, sizeof P.fields[0], "%s", f >= 0 ? ps.session.fields[f].value : "");
        ImGui::SetNextItemWidth(160);
        const bool integer = is.message && strstr(is.message, "(integer)");
        ImGui::InputTextWithHint("##value", integer ? "1" : "x", buf, sizeof P.fields[0]);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (f < 0 && buf[0] && ps.session.n_fields < DSG_SESSION_FIELDS) {
                f = ps.session.n_fields++;
                snprintf(ps.session.fields[f].name, sizeof ps.session.fields[f].name, "%s", is.name);
            }
            if (f >= 0 && buf[0]) snprintf(ps.session.fields[f].value, sizeof ps.session.fields[f].value, "%s", buf);
            else if (f >= 0) ps.session.fields[f] = ps.session.fields[--ps.session.n_fields];   // empty: the stand-in
            apply(ps);
        }
        ImGui::SetItemTooltip("Empty: the check's stand-in");
        ImGui::SameLine();
        user_text(is.name);
        ImGui::PopID();
    }
}

// A setting that restarts the session takes effect when its field is left.
void settings_section(dsg_preview_settings& ps) {
    ImGui::PushItemWidth(160);
    if (!editing("Display (px)")) {
        P.display[0] = ps.session.width;
        P.display[1] = ps.session.height;
    }
    ImGui::InputInt2("Display (px)", P.display);
    if (ImGui::IsItemDeactivatedAfterEdit() && P.display[0] >= 1 && P.display[0] <= kMaxSide && P.display[1] >= 1 &&
        P.display[1] <= kMaxSide) {
        ps.session.width = P.display[0];
        ps.session.height = P.display[1];
        apply(ps);
    }
    ImGui::SetItemTooltip("The simulated display's width and height in pixels; a change restarts");
    if (!editing("Rate (Hz)")) P.hz = ps.session.hz;
    ImGui::InputInt("Rate (Hz)", &P.hz, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit() && P.hz >= 1 && P.hz <= kMaxHz) {
        ps.session.hz = P.hz;
        apply(ps);
    }
    ImGui::SetItemTooltip("The simulated display's refresh rate; a change restarts");
    if (!editing("Seed")) snprintf(P.seed, sizeof P.seed, "%llu", (unsigned long long)ps.session.seed);
    ImGui::InputText("Seed", P.seed, sizeof P.seed, ImGuiInputTextFlags_CharsDecimal);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        char* end = nullptr;
        const unsigned long long v = strtoull(P.seed, &end, 10);
        if (end && end != P.seed && *end == 0) {
            ps.session.seed = (uint64_t)v;
            apply(ps);
        }
    }
    ImGui::SetItemTooltip("The seed of the session's random draws; a change restarts");
    if (!editing("Participant")) snprintf(P.participant, sizeof P.participant, "%s", ps.session.participant);
    ImGui::InputText("Participant", P.participant, sizeof P.participant);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        // the player's forms: correct, silent, or p= a probability
        bool ok = strcmp(P.participant, "correct") == 0 || strcmp(P.participant, "silent") == 0;
        if (!ok && strncmp(P.participant, "p=", 2) == 0) {
            char* end = nullptr;
            const double p = strtod(P.participant + 2, &end);
            ok = end && end != P.participant + 2 && *end == 0 && p >= 0 && p <= 1;
        }
        if (ok) {
            snprintf(ps.session.participant, sizeof ps.session.participant, "%s", P.participant);
            apply(ps);
        } else {
            ui_set_status("Type the participant as correct, silent, or p= and a probability, such as p=0.8.");
        }
    }
    ImGui::SetItemTooltip("Who answers the trials before the pin: correct, silent, or p=0.8 (correct with that probability)");
    if (!editing("Its rt (s)")) P.rt_s = (float)ps.session.rt_s;
    ImGui::InputFloat("Its rt (s)", &P.rt_s, 0, 0, "%.3f");
    if (ImGui::IsItemDeactivatedAfterEdit() && P.rt_s > 0 && P.rt_s <= 60) {
        ps.session.rt_s = (double)P.rt_s;
        apply(ps);
    }
    ImGui::SetItemTooltip("The scripted participant's response time, in seconds");
    ImGui::PopItemWidth();
    pin_fields(ps);
    field_rows(ps);
}

void outputs_section() {
    const int n = dsg_preview_outputs(pv());
    if (n == 0) {
        ImGui::TextDisabled("No codes sent yet.");
        return;
    }
    const float h = ImMin(8.0f, (float)n + 1.0f) * ImGui::GetTextLineHeightWithSpacing() + 4.0f;
    if (!ImGui::BeginTable("##outputs", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV, ImVec2(0, h)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time (s)");
    ImGui::TableSetupColumn("Role");
    ImGui::TableSetupColumn("Code");
    ImGui::TableHeadersRow();
    for (int i = 0; i < n; i++) {
        dsg_preview_output o = {};
        if (!dsg_preview_output_at(pv(), i, &o)) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", (double)o.t_ns * 1e-9);
        ImGui::TableNextColumn();
        user_text(o.role);
        ImGui::TableNextColumn();
        ImGui::Text("%u", o.code);
    }
    // the newest code stays in view unless the person scrolled up
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndTable();
}

void undriven_section() {
    const int n = dsg_preview_undriven(pv());
    for (int i = 0; i < n; i++) {
        const char* why = "";
        const char* role = dsg_preview_undriven_role(pv(), i, &why);
        if (!role) continue;
        ImGui::PushID(i);
        user_text(role);
        ImGui::SameLine();
        if (why && why[0]) ImGui::TextDisabled("not driven: %s", why);
        else ImGui::TextDisabled("not driven");
        ImGui::PopID();
    }
}

void data_section(const dsg_preview_info& in) {
    if (!in.out_dir || !in.out_dir[0]) {
        ImGui::TextDisabled("No data set yet.");
        return;
    }
    ImGui::TextDisabled("Folder:");
    ImGui::SameLine();
    user_text(in.out_dir);
    if (ImGui::Button("Open trials.csv")) {
        char path[DSG_PATH_MAX];
        if (dsg_join(path, sizeof path, in.out_dir, "trials.csv") || !ui_open_file(path))
            ui_set_status("No view opens trials.csv yet. Open it from the folder: %s", in.out_dir);
    }
    ImGui::SetItemTooltip("Show the session's trials, read-only");
    ImGui::SameLine();
    if (ImGui::Button("Copy path")) ImGui::SetClipboardText(in.out_dir);
    ImGui::SetItemTooltip("Copy the folder's path to the clipboard");
}

void draw() {
    dsg_preview_info in = {};
    dsg_preview_get_info(pv(), &in);
    dsg_preview_settings ps = {};
    dsg_preview_get(pv(), &ps);

    controls(in, ps);
    // The display's Fit size comes from the room below the controls, less the
    // rows that are always there, measured, not from the room left after
    // them: a banner or an issue that comes and goes moves the display down
    // but does not rescale it (hand test, 2026-11-21).
    const float room = ImGui::GetContentRegionAvail().y, top = ImGui::GetCursorPosY();
    float varying = status_line(in);
    char buf[256];
    snprintf(buf, sizeof buf,
             "Faithful in logic, not in timing: a simulated %d Hz display paced by this window; input times are frame times; no sound.",
             in.hz > 0 ? in.hz : ps.session.hz);
    banner("##faithful", kBannerInfo, buf);
    const float y_banners = ImGui::GetCursorPosY();
    if (in.stale) {
        char when[16] = "?";
        const time_t t = (time_t)in.shown_unix_s;
        const struct tm* lt = in.shown_unix_s ? localtime(&t) : nullptr;
        if (lt) strftime(when, sizeof when, "%H:%M:%S", lt);
        snprintf(buf, sizeof buf, "Shows the definition as of %s; the text has %d %s.", when, in.stale_errors,
                 in.stale_errors == 1 ? "error" : "errors");
        banner("##stale", kBannerStale, buf);
    }
    if (in.note && in.note[0]) {
        snprintf(buf, sizeof buf, "Stopped before the trial chosen: %s", in.note);
        banner("##note", kBannerNote, buf);
    }
    varying += ImGui::GetCursorPosY() - y_banners;
    where_line();
    if (in.keyboard) ImGui::TextColored(kKeysText, "Keys go to the session. Press Esc to give them back.");
    else ImGui::TextDisabled("Click the display to send it keys.");

    // The details below get 7 rows; the display at least a third of the room.
    const float fixed = ImGui::GetCursorPosY() - top - varying;
    draw_display(in, ps, ImMax(room - fixed - ImGui::GetFrameHeightWithSpacing() * 7.0f, room * 0.33f));
    choice_strip();

    ImGui::BeginChild("##details", ImVec2(0, 0));
    if (ImGui::CollapsingHeader("Settings")) settings_section(ps);
    if (dsg_preview_undriven(pv()) > 0 && ImGui::CollapsingHeader("Roles not driven", ImGuiTreeNodeFlags_DefaultOpen)) undriven_section();
    if (ImGui::CollapsingHeader("Outputs", ImGuiTreeNodeFlags_DefaultOpen)) outputs_section();
    if (ImGui::CollapsingHeader("Data set", ImGuiTreeNodeFlags_DefaultOpen)) data_section(in);
    ImGui::EndChild();

    // The pane losing the focus gives the keyboard back, as Escape does.
    if (dsg_preview_keyboard(pv()) && !ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) dsg_preview_set_keyboard(pv(), false);
}

bool animating() { return dsg_preview_animating(pv()); }

void closed() { P = Pane(); }

} // namespace

extern const UiView ui_view_preview = {
    "preview", "Preview", UI_SLOT_AUX,
    nullptr, nullptr, nullptr, closed, nullptr,
    draw, nullptr, nullptr, animating, nullptr, nullptr,
};
