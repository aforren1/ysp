// view_run.cpp - the Run view in the AUX pane (designer step 6, part V;
// docs/designer.md, "The views" of step 6 and "Part V as built"). OWNED BY
// PART V.
//
// The view only shows and edits: ui.cpp owns the builder, the run and the
// run's settings (views.h: ui_build(), ui_run_start(), ui_run_settings() and
// the rest) and keeps the settings in the view file's part "run". At the
// top: Run, Stop and Kill, and one line of the run's state. Then four
// sections: Rig (the profile of the check and of a run, read-only), Build
// (the last Build), Run settings, and Output (the run's progress and end).
//
// The view keeps its own dsg_rig (dsg.h: one thread's each). A profile's
// load costs about 176 us, so it loads once per choice, and again only when
// dsg_rig_changed() says so; that stat and the folder's list run at most
// once a second, and only in frames that the view draws. The view adds no
// frame of its own: while a run goes, ui.cpp's wake-ups bring at most
// DSG_RUN_REDRAW_HZ frames a second, and input brings the rest.
//
// Names from the rig folder, the profile, the log and the paths are user
// text, so Skribidi draws them (skt.h), and each also goes to ImGui's log
// for the tests, as the preview's pane does.
#include "views.h"
#include "skt.h"
#include "player.h"   // PL_NEED_*, yrig_profile

#include "imgui.h"
#include "imgui_internal.h"   // ItemAdd, BeginComboPreview, RenderTextWrapped

// skb_common.h tests __cplusplus__ (sic); see view_chart.cpp.
#include "skribidi/skb_layout.h"
#undef SKB_LITERAL
#define SKB_LITERAL(type) type

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace {

const ImU32 kText = IM_COL32(235, 235, 240, 255);
const ImU32 kBandWarn = IM_COL32(130, 40, 40, 255);
const ImVec4 kOk(0.55f, 0.85f, 0.55f, 1.0f), kWarn(1.0f, 0.78f, 0.31f, 1.0f), kBad(1.0f, 0.45f, 0.4f, 1.0f);
const int64_t kSecondNs = 1000000000LL;
const int kMaxRigs = 64;
// run_part.c's bounds, so a value typed here reads back the same.
const double kMaxSeconds = 172800.0, kMaxGain = 1e6;
// The display's restore after a Kill of a real run on X11 (CLAUDE.md, "Linux
// X11 work"; docs/designer.md, "Risks" of step 6).
const char* const kXrandr = "xrandr --output OUT --mode 1920x1080 --rate 60 --gamma 1:1:1";

// A clock that ticks at most once a second, on the UI's clock.
struct Every {
    int64_t last = 0;
    bool known = false;
    bool due() {
        const int64_t now = ui_now_ns();
        if (known && now >= last && now - last < kSecondNs) return false;
        last = now;
        known = true;
        return true;
    }
};

struct View {
    // The rig folder's profiles (dsg_rig_list()), read at most once a second.
    char names[kMaxRigs][DSG_RIG_NAME] = {};
    int n_names = 0;
    bool list_ok = false;
    bool listed = false;
    char list_err[256] = "";
    Every list_clock;
    // The profile that the settings name, loaded once per choice or change.
    dsg_rig* rig = nullptr;
    char rig_name[DSG_RIG_NAME] = "";   // the name of the last load, done or not
    bool rig_tried = false;
    char rig_err[256] = "";
    Every rig_clock;
    // dsg_build_stale() at most once a second (a stat a source, views.h).
    bool stale = true;
    Every stale_clock;
    const dsg_build_result* stale_of = nullptr;
    uint64_t stale_rev = 0;
    // Fields being edited: refreshed from the settings while not active.
    char seed[24] = "";
    char participant[32] = "";
    double max_seconds = 0;
    char out_dir[DSG_PATH_MAX] = "";
    char fields[DSG_SESSION_FIELDS][256] = {};
    double gains[DSG_RUN_ROLES] = {};
    // The run last seen: was it simulated? A run's command has --sim; the
    // build_run double gives no command, so the settings at its first frame
    // stand in.
    const dsg_run* run = nullptr;
    char run_out[DSG_PATH_MAX] = "";
    bool run_sim = false;
    bool run_ended = false;
    bool log_follow = true;
};
View V;

// --- small helpers --------------------------------------------------------------

// User text on the current line, as an item, so SameLine() follows it. The
// log line lets a test read it as it reads ImGui's own text.
void user_text(const char* s, int n = -1) {
    if (!s || !s[0] || n == 0) return;
    if (n < 0) n = (int)strlen(s);
    ImGui::LogText("%.*s", n, s);
    const skb_layout_t* l = skt_layout_n(s, n, ui_text_size());
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

// A label in ImGui's font, then user text on the same line.
void labeled(const char* label, const char* s) {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine();
    if (s && s[0]) user_text(s);
    else ImGui::TextDisabled("none");
}

// A colored band the width of the view, as the preview's banners. id lets
// the Test Engine find it; the text goes to ImGui's log.
void band(const char* id, ImU32 bg, const char* text) {
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

bool editing(const char* label) { return ImGui::GetCurrentContext()->ActiveId == ImGui::GetID(label); }

// Each of the check's needs of this kind (DSG_FROM_RUN, PL_NEED_*), in the
// merged report's order (the check's issues come first).
template <class F> void each_need(int kind, F f) {
    const dsg_report* r = ui_report();
    for (int i = 0; r && i < r->n_issues; i++) {
        const dsg_issue& is = r->issues[i];
        if (is.from == DSG_FROM_RUN && is.object == kind && is.name && is.name[0]) f(is);
    }
}

// The errors of the check alone: ui_report() also counts Build's and the run's.
int check_errors() {
    const dsg_report* r = ui_report();
    int n = 0;
    for (int i = 0; r && i < r->n_issues; i++)
        n += r->issues[i].severity == DSG_SEV_ERROR && r->issues[i].from != DSG_FROM_BUILD && r->issues[i].from != DSG_FROM_PLAYER_RUN;
    return n;
}

bool has_profile_json() {
    for (int i = 0; i < V.n_names; i++)
        if (strcmp(V.names[i], "profile") == 0) return true;
    return false;
}

// --- the rig profile ------------------------------------------------------------

void list_rigs() {
    V.list_err[0] = 0;
    const int n = dsg_rig_list(V.names, kMaxRigs, V.list_err, sizeof V.list_err);
    V.list_ok = n >= 0;
    V.n_names = n > 0 ? (n < kMaxRigs ? n : kMaxRigs) : 0;
    V.listed = true;
}

void free_rig() {
    dsg_rig_free(V.rig);
    V.rig = nullptr;
    V.rig_tried = false;
    V.rig_name[0] = 0;
    V.rig_err[0] = 0;
}

void load_rig(const char* name) {
    char keep[DSG_RIG_NAME];
    snprintf(keep, sizeof keep, "%s", name);
    free_rig();
    snprintf(V.rig_name, sizeof V.rig_name, "%s", keep);
    V.rig_tried = true;
    dsg_rig* r = nullptr;
    if (keep[0] && dsg_rig_load(keep, &r, V.rig_err, sizeof V.rig_err) == 0) V.rig = r;
}

// The settings' profile: loaded at a new name, and again when its file
// changed. A change of the file is the check's and the preview's too
// (decisions 12 to 14): each loads its own copy at its next check or
// session, so the view asks for both.
void sync_rig(const char* name) {
    if (!V.rig_tried || strcmp(name, V.rig_name) != 0) {
        load_rig(name);
        return;
    }
    if (!name[0] || !V.rig_clock.due()) return;
    // a profile that did not load is tried again too: it may be there now
    if (V.rig && !dsg_rig_changed(V.rig)) return;
    load_rig(name);
    ui_check_again();
    dsg_preview_restart(ui_preview());
}

// The combo's name for a profile: profile.json is the player's default.
void rig_label(const char* name, char* out, size_t cap) {
    if (!name[0]) snprintf(out, cap, "No profile (keyboard only)");
    else if (strcmp(name, "profile") == 0) snprintf(out, cap, "profile (default)");
    else snprintf(out, cap, "%s", name);
}

void choose_rig(dsg_run_settings* rs, const char* name) {
    if (strcmp(rs->rig, name) == 0) return;
    // ui.cpp posts a check with the new profile at the next frame
    snprintf(rs->rig, sizeof rs->rig, "%s", name);
    // The preview takes the choice too ("The views"). "No profile" is its
    // built-in simulated rig, which also has the keyboard only.
    dsg_preview_settings ps = {};
    dsg_preview_get(ui_preview(), &ps);
    if (strcmp(ps.session.rig, name) != 0) {
        snprintf(ps.session.rig, sizeof ps.session.rig, "%s", name);
        dsg_preview_set(ui_preview(), &ps);
    }
}

void rig_combo(dsg_run_settings* rs) {
    // "No profile" while profile.json exists is offered for a simulated run
    // only: a real run with no --rig takes profile.json (player/lower.c), so
    // its check would not be the run's.
    const bool none_ok = rs->sim || !has_profile_json();
    char label[DSG_RIG_NAME + 32];
    rig_label(rs->rig, label, sizeof label);
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("Rig profile", "")) {
        for (int i = 0; i < V.n_names; i++) {
            ImGui::PushID(i);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##rig", strcmp(rs->rig, V.names[i]) == 0, 0, ImVec2(0, ImGui::GetFrameHeight())))
                choose_rig(rs, V.names[i]);
            char item[DSG_RIG_NAME + 32];
            rig_label(V.names[i], item, sizeof item);
            ImGui::LogText("%s", item);
            skt_draw_layout(ImGui::GetWindowDrawList(), skt_layout(item, ui_text_size()), at.x + 2, at.y + 1, 1.0f, kText);
            ImGui::PopID();
        }
        if (none_ok && ImGui::Selectable("No profile (keyboard only)", !rs->rig[0])) choose_rig(rs, "");
        ImGui::EndCombo();
    }
    if (ImGui::BeginComboPreview()) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        skt_draw_layout(ImGui::GetWindowDrawList(), skt_layout(label, ui_text_size()), at.x, at.y - 1, 1.0f, kText);
        ImGui::LogText("%s", label);
        ImGui::EndComboPreview();
    }
    ImGui::SetItemTooltip("The profile in the user's rig folder that the check and a run use");
}

const char* check_word(int c) {
    switch (c) {
    case YRIG_CHECK_NONE: return "none";
    case YRIG_CHECK_OK: return "OK";
    case YRIG_CHECK_MISSING: return "MISSING";
    case YRIG_CHECK_CHANGED: return "CHANGED";
    case YRIG_CHECK_DIFFERS: return "DIFFERS";
    case YRIG_CHECK_NOT_RUN: return "not run";
    default: return "?";
    }
}

void loopback_line(const char* what, const yrig_loopback& lb) {
    if (!lb.set) return;
    ImGui::TextDisabled("%s:", what);
    ImGui::SameLine();
    ImGui::TextColored(lb.check == YRIG_CHECK_OK ? kOk : kBad, "%s", check_word(lb.check));
    ImGui::SameLine();
    ImGui::TextDisabled("(n %d, %s)", (int)lb.n, lb.date);
}

// The profile, read-only (decision 12): its roles and devices, the display
// and its calibration, the view, the signals, the notes of the load.
void rig_details(const yrig_profile* p) {
    labeled("Rig:", p->rig);
    ImGui::SameLine();
    ImGui::TextDisabled("written %s", p->written);
    labeled("File:", dsg_rig_path(V.rig));
    if (ImGui::TreeNodeEx("Roles", ImGuiTreeNodeFlags_DefaultOpen, "Roles (%d)", p->n_roles)) {
        if (p->n_roles == 0) ImGui::TextDisabled("No role is bound. The keyboard takes each role that it can.");
        for (int i = 0; i < p->n_roles; i++) {
            const yrig_role& r = p->role[i];
            ImGui::PushID(i);
            user_text(r.name);
            ImGui::SameLine();
            ImGui::TextDisabled("family");
            ImGui::SameLine();
            user_text(r.family);
            ImGui::SameLine();
            ImGui::TextDisabled("key");
            ImGui::SameLine();
            user_text(r.key);
            ImGui::Indent();
            if (r.baud) ImGui::TextDisabled("baud %u", (unsigned)r.baud);
            if (r.latched) ImGui::TextDisabled("latched");
            if (r.ftdi_latency_ns >= 0) ImGui::TextDisabled("FTDI latency %.3f s", (double)r.ftdi_latency_ns * 1e-9);
            if (r.pulse_ns >= 0) ImGui::TextDisabled("pulse %.6f s", (double)r.pulse_ns * 1e-9);
            if (r.n_buttons > 0) ImGui::TextDisabled("%d buttons", r.n_buttons);
            loopback_line("latency", r.latency);
            loopback_line("bounds", r.bounds);
            ImGui::Unindent();
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNodeEx("Display and view", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!p->display.set) ImGui::TextDisabled("No display block.");
        else {
            ImGui::TextDisabled("onset offset %.6f s", (double)p->display.onset_offset_ns * 1e-9);
            loopback_line("photodiode latency", p->display.latency);
            if (p->display.calibration[0]) {
                labeled("Calibration:", p->display.calibration);
                ImGui::SameLine();
                const bool ok = p->display.check == YRIG_CHECK_OK;
                ImGui::TextColored(ok ? kOk : kBad, "%s", check_word(p->display.check));
                if (ok && !dsg_rig_cal(V.rig)) ImGui::TextColored(kWarn, "The calibration file did not load.");
            } else {
                ImGui::TextDisabled("No calibration file.");
            }
        }
        if (!p->view.set) ImGui::TextDisabled("No view. A definition in deg or cm cannot run.");
        else
            ImGui::TextDisabled("view: %g mm from the eye, %d px over %g mm, %.2f px per degree", p->view.distance_mm, (int)p->view.width_px,
                                p->view.width_mm, yrig_ppd(p));
        ImGui::TreePop();
    }
    if (p->n_signals > 0 && ImGui::TreeNodeEx("Signals", ImGuiTreeNodeFlags_DefaultOpen, "Signals (%d)", p->n_signals)) {
        for (int i = 0; i < p->n_signals; i++) {
            const yrig_signal& s = p->signal[i];
            ImGui::PushID(i);
            user_text(s.name);
            ImGui::SameLine();
            ImGui::TextDisabled("%s, %s, %g x %g a count, device %s", yrig_adapter_name(s.adapter), yrig_units_name(s.units), s.per_count[0],
                                s.per_count[1], s.device == YRIG_DEVICE_CLICK ? "click" : "any");
            if (s.bounded) ImGui::TextDisabled("  bounds x %g to %g, y %g to %g", s.lo[0], s.hi[0], s.lo[1], s.hi[1]);
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (p->n_notes > 0) {
        ImGui::TextColored(kWarn, "The load's checks found %d problems:", p->n_notes);
        for (int i = 0; i < p->n_notes; i++) {
            ImGui::PushID(i);
            ImGui::Bullet();
            user_text(p->notes[i]);
            ImGui::PopID();
        }
    }
}

void rig_section(dsg_run_settings* rs) {
    if (V.list_clock.due()) list_rigs();
    // A real run with no --rig takes profile.json: the setting says so, and
    // the check takes the same profile.
    if (!rs->sim && !rs->rig[0] && has_profile_json()) choose_rig(rs, "profile");
    sync_rig(rs->rig);
    rig_combo(rs);
    if (V.listed && !V.list_ok) {
        ImGui::TextColored(kWarn, "The rig folder gives no profile:");
        ImGui::SameLine();
        user_text(V.list_err[0] ? V.list_err : "no reason given");
    }
    if (!rs->rig[0]) {
        ImGui::TextDisabled("No profile: the keyboard takes each role that it can. A real run has no view, signals or calibration.");
        return;
    }
    if (!V.rig) {
        ImGui::TextColored(kBad, "The profile does not load:");
        ImGui::SameLine();
        user_text(V.rig_err[0] ? V.rig_err : "no reason given");
        return;
    }
    char line[512];
    dsg_rig_summary(V.rig, line, sizeof line);
    user_text(line);
    const yrig_profile* p = dsg_rig_profile(V.rig);
    if (p && p->n_notes > 0) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "%d check notes", p->n_notes);
    }
    if (ImGui::SmallButton("Copy the folder's path")) {
        char dir[DSG_PATH_MAX];
        snprintf(dir, sizeof dir, "%s", dsg_rig_path(V.rig));
        char* slash = strrchr(dir, '/');
#ifdef _WIN32
        char* bs = strrchr(dir, '\\');
        if (bs && (!slash || bs > slash)) slash = bs;
#endif
        if (slash) *slash = 0;
        ImGui::SetClipboardText(dir);
        ui_set_status("Copied the rig folder's path: %s", dir);
    }
    ImGui::SetItemTooltip("Edit the profile's view or signals in a text editor. The designer loads it again when it changes");
    if (p && ImGui::TreeNode("Profile details")) {
        rig_details(p);
        ImGui::TreePop();
    }
}

// --- Build ------------------------------------------------------------------------

void build_section() {
    const dsg_project* pj = ui_project();
    char path[DSG_PATH_MAX] = "";
    if (pj && dsg_build_path(pj, path, sizeof path) == 0) labeled("Pack:", path);
    const dsg_build_result* b = ui_build_last();
    if (ui_build_busy()) ImGui::TextColored(kWarn, "Building ...");
    if (!b) ImGui::TextDisabled("No Build since the project opened.");
    else if (b->status == DSG_BUILD_OK) {
        ImGui::TextDisabled("Pack ID:");
        ImGui::SameLine();
        ImGui::TextUnformatted(b->pack_id);
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy ID")) ImGui::SetClipboardText(b->pack_id);
        ImGui::SetItemTooltip("Copy the pack ID. A methods section cites it");
        char when[16] = "?";
        const time_t t = (time_t)b->done_unix_s;
        const struct tm* lt = b->done_unix_s ? localtime(&t) : nullptr;
        if (lt) strftime(when, sizeof when, "%H:%M:%S", lt);
        ImGui::TextDisabled("%lld bytes, built in %.0f ms at %s", (long long)b->size, b->ms, when);
        // a stat a source: once a second at most, and at a new result or text
        const uint64_t rev = dsg_doc_revision(ui_doc());
        if (b != V.stale_of || rev != V.stale_rev || V.stale_clock.due()) {
            V.stale = ui_build_stale();
            V.stale_of = b;
            V.stale_rev = rev;
        }
        if (V.stale) ImGui::TextColored(kWarn, "Stale: the definition or a source changed after this Build. Run builds again first.");
        else ImGui::TextColored(kOk, "Up to date.");
    } else if (b->status == DSG_BUILD_CANCELED) {
        ImGui::TextColored(kWarn, "The last Build was canceled.");
    } else {
        ImGui::TextColored(kBad, "The last Build failed: %d errors. The Issues panel lists them.", b->report.n_errors);
    }
    ImGui::BeginDisabled(!ui_can_build());
    if (ImGui::Button("Build")) ui_build();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", ui_can_build() ? "Save, then build the pack into the project's build folder"
                                               : "Not now: no project is open, or a Build or a run goes");
}

// --- the run's settings -----------------------------------------------------------

bool participant_ok(const char* s) {
    if (!s[0] || strcmp(s, "correct") == 0 || strcmp(s, "silent") == 0) return true;
    if (strncmp(s, "p=", 2) != 0) return false;
    char* end = nullptr;
    const double p = strtod(s + 2, &end);
    return end && end != s + 2 && *end == 0 && p >= 0 && p <= 1;
}

bool seed_ok(const char* s) {
    size_t i = 0;
    for (; s[i]; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return i <= 20;
}

int field_index(const dsg_run_settings* rs, const char* name) {
    for (int k = 0; k < rs->n_fields; k++)
        if (strcmp(rs->fields[k].name, name) == 0) return k;
    return -1;
}

// One session field (--field NAME=VALUE); empty removes it.
void field_row(dsg_run_settings* rs, int slot, const char* name, bool integer) {
    char* buf = V.fields[slot];
    int f = field_index(rs, name);
    ImGui::PushID(name);
    if (!editing("##value")) snprintf(buf, sizeof V.fields[0], "%s", f >= 0 ? rs->fields[f].value : "");
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##value", integer ? "an integer" : "text", buf, sizeof V.fields[0]);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (f < 0 && buf[0] && rs->n_fields < DSG_SESSION_FIELDS) {
            f = rs->n_fields++;
            snprintf(rs->fields[f].name, sizeof rs->fields[f].name, "%s", name);
        }
        if (f >= 0 && buf[0]) snprintf(rs->fields[f].value, sizeof rs->fields[f].value, "%s", buf);
        else if (f >= 0) rs->fields[f] = rs->fields[--rs->n_fields];
        f = field_index(rs, name);
    }
    ImGui::SetItemTooltip("The session field's value (--field NAME=VALUE)");
    ImGui::SameLine();
    user_text(name);
    if (f < 0 || !rs->fields[f].value[0]) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "required");
    }
    ImGui::PopID();
}

void fields_rows(dsg_run_settings* rs) {
    // the fields that the check's needs name, each once
    char seen[DSG_SESSION_FIELDS][64];
    int n = 0;
    each_need(PL_NEED_FIELD, [&](const dsg_issue& is) {
        if (n >= DSG_SESSION_FIELDS) return;
        for (int k = 0; k < n; k++)
            if (strcmp(seen[k], is.name) == 0) return;
        snprintf(seen[n], sizeof seen[n], "%s", is.name);
        if (n == 0) ImGui::TextDisabled("Session fields");
        field_row(rs, n, is.name, is.message && strstr(is.message, "(integer)"));
        n++;
    });
    if (n == 0) ImGui::TextDisabled("The definition needs no session field.");
}

void gains_rows(dsg_run_settings* rs) {
    int slot = 0;
    each_need(PL_NEED_SIM_GAIN, [&](const dsg_issue& is) {
        if (slot >= DSG_RUN_ROLES) return;
        int g = -1;
        for (int k = 0; k < rs->n_gains; k++)
            if (strcmp(rs->gains[k].role, is.name) == 0) g = k;
        ImGui::PushID(is.name);
        if (!editing("##gain")) V.gains[slot] = g >= 0 ? rs->gains[g].units : 0.0;
        ImGui::SetNextItemWidth(120);
        ImGui::InputDouble("##gain", &V.gains[slot], 0, 0, "%g");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            const double u = V.gains[slot];
            if (u == 0 && g >= 0) {
                rs->gains[g] = rs->gains[--rs->n_gains];
            } else if (u != 0 && isfinite(u) && fabs(u) <= kMaxGain && !strchr(is.name, ',') && !strchr(is.name, '=')) {
                if (g < 0 && rs->n_gains < DSG_RUN_ROLES) {
                    g = rs->n_gains++;
                    snprintf(rs->gains[g].role, sizeof rs->gains[g].role, "%s", is.name);
                }
                if (g >= 0) rs->gains[g].units = u;
            } else if (u != 0) {
                ui_set_status("Type a gain that is not 0, at most %g role units a count.", kMaxGain);
            }
        }
        ImGui::SetItemTooltip("--sim-gain: role units a count of the simulated mouse. 0 removes it");
        ImGui::SameLine();
        ImGui::TextDisabled("gain of");
        ImGui::SameLine();
        user_text(is.name);
        ImGui::PopID();
        slot++;
    });
}

// The roles below their tier (--allow-below-tier), each with the check's need.
void tier_rows(dsg_run_settings* rs) {
    bool any = false;
    each_need(PL_NEED_ROLE_TIER, [&](const dsg_issue& is) {
        if (!any) ImGui::TextDisabled("Roles below their tier");
        any = true;
        int k = -1;
        for (int i = 0; i < rs->n_below; i++)
            if (strcmp(rs->allow_below_tier[i], is.name) == 0) k = i;
        bool on = k >= 0;
        ImGui::PushID(is.name);
        if (ImGui::Checkbox("##allow", &on)) {
            if (!on && k >= 0) {
                if (k != rs->n_below - 1) memcpy(rs->allow_below_tier[k], rs->allow_below_tier[rs->n_below - 1], sizeof rs->allow_below_tier[0]);
                rs->n_below--;
            } else if (on && k < 0 && rs->n_below < DSG_RUN_ROLES && !strchr(is.name, ',') && !strchr(is.name, '=')) {
                snprintf(rs->allow_below_tier[rs->n_below++], sizeof rs->allow_below_tier[0], "%s", is.name);
            }
        }
        ImGui::SetItemTooltip("--allow-below-tier: run with this role below its tier. session.json records it");
        ImGui::SameLine();
        ImGui::TextDisabled("Allow");
        ImGui::SameLine();
        user_text(is.name);
        ImGui::SameLine();
        ImGui::TextDisabled("below its tier");
        // ysp/rigfile.h gives a role tier 1 or UNKNOWN, so a need that stays
        // with a profile is at UNKNOWN (Part P)
        if (rs->rig[0]) ImGui::TextColored(kWarn, "The profile gives it no tier: it has no checked loopback for this role.");
        ImGui::PopID();
    });
}

void settings_section(dsg_run_settings* rs) {
    ImGui::Checkbox("Simulated (--sim)", &rs->sim);
    ImGui::SetItemTooltip("The whole session on a virtual clock, with no display, devices or sound");
    ImGui::PushItemWidth(160);
    if (rs->sim) {
        if (!editing("Simulated participant")) snprintf(V.participant, sizeof V.participant, "%s", rs->participant);
        ImGui::InputTextWithHint("Simulated participant", "the player's", V.participant, sizeof V.participant);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (participant_ok(V.participant)) snprintf(rs->participant, sizeof rs->participant, "%s", V.participant);
            else ui_set_status("Type the participant as correct, silent, or p= and a probability, such as p=0.8.");
        }
        ImGui::SetItemTooltip("--participant: correct, silent, or p=0.8 (correct with that probability)");
        if (!editing("Max seconds")) V.max_seconds = rs->max_seconds;
        ImGui::InputDouble("Max seconds", &V.max_seconds, 0, 0, "%g");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (V.max_seconds > 0 && V.max_seconds <= kMaxSeconds) rs->max_seconds = V.max_seconds;
            else ui_set_status("Type the limit in seconds: above 0, at most %g.", kMaxSeconds);
        }
        ImGui::SetItemTooltip("--max-seconds: the simulated session ends after this time on its clock");
        gains_rows(rs);
    } else {
        ImGui::Checkbox("Window (development)", &rs->windowed);
        ImGui::SetItemTooltip("--windowed: a window, not the full screen. Tier 2 at best");
    }
    if (!editing("Seed")) snprintf(V.seed, sizeof V.seed, "%s", rs->seed);
    ImGui::InputTextWithHint("Seed", "the player draws one", V.seed, sizeof V.seed, ImGuiInputTextFlags_CharsDecimal);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (seed_ok(V.seed)) snprintf(rs->seed, sizeof rs->seed, "%s", V.seed);
        else ui_set_status("Type the seed as digits only, or leave it empty.");
    }
    ImGui::SetItemTooltip("--seed: the session's seed. Empty: the player draws one and records it");
    ImGui::PopItemWidth();
    fields_rows(rs);
    tier_rows(rs);

    // the output folder (decision 11): the player refuses one that exists
    const dsg_project* pj = ui_project();
    char def[DSG_PATH_MAX] = "";
    if (pj && dsg_run_out_dir(pj->folder, rs, (int64_t)time(nullptr), def, sizeof def)) def[0] = 0;
    ImGui::TextDisabled("Output folder");
    if (!editing("##out_dir")) snprintf(V.out_dir, sizeof V.out_dir, "%s", rs->out_dir);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##out_dir", "data/PARTICIPANT_DATE-TIME in the project", V.out_dir, sizeof V.out_dir);
    if (ImGui::IsItemDeactivatedAfterEdit()) snprintf(rs->out_dir, sizeof rs->out_dir, "%s", V.out_dir);
    ImGui::SetItemTooltip("Empty: the project's data folder, the participant field and the time (UTC)");
    const char* out = rs->out_dir[0] ? rs->out_dir : def;
    if (out[0]) {
        labeled("The next run writes to", out);
        if (dsg_is_dir(out) || dsg_is_file(out)) ImGui::TextColored(kBad, "This folder exists. The player refuses it: choose another.");
    }
}

// --- Run, Stop, Kill and the output -----------------------------------------------

bool run_is_sim(const dsg_run* r, const dsg_run_info& in) {
    const char* out = in.out_dir ? in.out_dir : "";
    // a new run: another pointer or folder, or one that goes after an end
    // (the allocator can give a new run the old one's address)
    if (r != V.run || strcmp(V.run_out, out) != 0 || (V.run_ended && in.state != DSG_RUN_ENDED)) {
        V.run = r;
        snprintf(V.run_out, sizeof V.run_out, "%s", out);
        V.run_sim = ui_run_settings()->sim;
        V.log_follow = true;
    }
    V.run_ended = in.state == DSG_RUN_ENDED;
    if (in.command && in.command[0]) return strstr(in.command, " --sim") != nullptr;
    return V.run_sim;
}

const char* state_word(int s) {
    switch (s) {
    case DSG_RUN_STARTING: return "Starting";
    case DSG_RUN_RUNNING: return "Running";
    case DSG_RUN_STOPPING: return "Stopping";
    case DSG_RUN_ENDED: return "Ended";
    default: return "No run";
    }
}

void controls(const dsg_run_info& in, bool going, bool sim) {
    ImGui::BeginDisabled(!ui_can_run());
    if (ImGui::Button("Run")) ui_run_start();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        const char* why = "Start the player with the pack (Build first when it is stale)";
        if (!ui_project()) why = "No project is open.";
        else if (going || ui_build_busy()) why = "A Build or a run goes.";
        else if (check_errors() > 0) why = "The check has errors: fix them first.";
        else if (!ui_can_run()) why = "The check of this text has not come back yet.";
        ImGui::SetTooltip("%s", why);
    }
    if (!going) return;
#ifdef _WIN32
    // Windows: no process can send SIGTERM to another, and a --sim run has no
    // window for WM_CLOSE, so Stop cannot reach it (Part P): Kill at once.
    const bool stop_reaches = !sim;
#else
    const bool stop_reaches = true;
#endif
    if (stop_reaches && !in.kill_offered) {
        ImGui::SameLine();
        ImGui::BeginDisabled(in.state == DSG_RUN_STOPPING);
        if (ImGui::Button("Stop")) ui_run_stop();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Ask the player to stop. It closes the data set (exit 3)");
    }
    if (in.kill_offered || !stop_reaches) {
        ImGui::SameLine();
        // dsg_run_kill() comes after a Stop; a Stop that cannot reach the
        // player gives 0 at once
        if (ImGui::Button("Kill") && (in.kill_offered || ui_run_stop() == 0)) ui_run_kill();
        ImGui::SetItemTooltip("End the player now");
        ImGui::TextColored(kWarn, "Kill ends the player now. The data set is not closed%s.",
                           sim ? "" : ", and the display's mode and gamma ramp can stay changed");
    }
}

// The log's last lines, the newest in view unless the person scrolled up.
void log_view(const char* log) {
    if (!log || !log[0]) {
        ImGui::TextDisabled("The player wrote nothing yet.");
        return;
    }
    // the line starts: DSG_RUN_LOG_LINES lines at most, so no allocation
    static int starts[DSG_RUN_LOG_LINES + 2];
    int n = 0;
    const char* p = log;
    while (*p && n < DSG_RUN_LOG_LINES + 1) {
        starts[n++] = (int)(p - log);
        const char* e = strchr(p, '\n');
        if (!e) {
            p += strlen(p);
            break;
        }
        p = e + 1;
    }
    starts[n] = (int)(p - log) + (p[-1] == '\n' ? 0 : 1);
    const float h = ImMin(10.0f, (float)n + 0.5f) * ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##log", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clip;
    clip.Begin(n);
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const char* s = log + starts[i];
            int len = starts[i + 1] - starts[i] - 1;
            if (len > 0 && s[len - 1] == '\r') len--;
            if (len > 0) user_text(s, len);
            else ImGui::NewLine();
        }
    }
    if (V.log_follow) ImGui::SetScrollHereY(1.0f);
    V.log_follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    ImGui::EndChild();
}

// What the exit means and what to do next ("What the Run view shows").
void end_text(const dsg_run_info& in, bool sim) {
    const int st = in.exit_status;
    ImGui::TextColored(st == 0 ? kOk : st == 3 ? kWarn : kBad, "Ended:");
    ImGui::SameLine();
    user_text(in.exit_text && in.exit_text[0] ? in.exit_text : "no exit text");
    if (st == 1) {
        ImGui::TextDisabled("The Issues panel shows the player's message. The check with the same profile says most of these first.");
    } else if (st == 2) {
        ImGui::TextColored(kBad, "The designer gave the player a bad argument. Report it with this command line:");
        if (in.command && in.command[0]) {
            user_text(in.command);
            if (ImGui::SmallButton("Copy the command line")) ImGui::SetClipboardText(in.command);
        }
    } else if (st == 3) {
        ImGui::TextDisabled("session.json in the folder records the stop (\"aborted\").");
    } else if (st == 99) {
        ImGui::TextDisabled("The panic watchdog ended the session and put the display back. panic.json in the folder names the trial.");
    } else if (st < 0) {
        ImGui::TextColored(kBad, "The player did not close its data set.");
        // SIGKILL and TerminateProcess() skip the player's restore of the
        // display's mode and gamma ramp; a --sim run has no display
        if (!sim) {
#if defined(_WIN32)
            band("##restore", kBandWarn,
                 "If the display's resolution, refresh rate or colors are wrong, set the resolution and rate again in Settings, "
                 "Display, or restart the computer.");
#elif defined(__APPLE__)
            band("##restore", kBandWarn,
                 "If the display's resolution, refresh rate or colors are wrong, set them again in System Settings, Displays, or "
                 "restart the computer.");
#else
            band("##restore", kBandWarn,
                 "If the display's mode or gamma ramp is wrong, put them back with this command. Use the output's name, mode and "
                 "rate (xrandr lists them) in place of OUT, 1920x1080 and 60:");
            ImGui::TextUnformatted(kXrandr);
            if (ImGui::SmallButton("Copy the command")) ImGui::SetClipboardText(kXrandr);
#endif
        }
    }
}

void output_section(const dsg_run* r, const dsg_run_info& in, bool sim) {
    if (!r) {
        ImGui::TextDisabled("No run since the project opened.");
        return;
    }
    if (!sim && in.state != DSG_RUN_ENDED) ImGui::TextDisabled("The preview stops while a real run goes.");
    if (in.trials > 0) {
        ImGui::TextDisabled("Last trial:");
        ImGui::SameLine();
        user_text(in.last_outcome && in.last_outcome[0] ? in.last_outcome : "no outcome");
        if (in.last_response && in.last_response[0]) {
            ImGui::SameLine();
            ImGui::TextDisabled("response");
            ImGui::SameLine();
            user_text(in.last_response);
        }
        if (in.last_rt_s >= 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("rt %.3f s", in.last_rt_s);
        }
    }
    if (in.state == DSG_RUN_ENDED) end_text(in, sim);
    if (in.out_dir && in.out_dir[0]) {
        labeled("Folder:", in.out_dir);
        if (ImGui::SmallButton("Copy path")) ImGui::SetClipboardText(in.out_dir);
        ImGui::SetItemTooltip("Copy the output folder's path to the clipboard");
        if (in.state == DSG_RUN_ENDED && in.trials > 0) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Open trials.csv")) {
                char path[DSG_PATH_MAX];
                if (dsg_join(path, sizeof path, in.out_dir, "trials.csv") || !ui_open_file(path))
                    ui_set_status("No view opens trials.csv yet. Open it from the folder: %s", in.out_dir);
            }
            ImGui::SetItemTooltip("Show the run's trials, read-only");
        }
    }
    if (in.log_path && in.log_path[0]) labeled("Log:", in.log_path);
    log_view(in.log);
}

void draw() {
    dsg_run_settings* rs = ui_run_settings();
    const dsg_run* r = ui_run();
    dsg_run_info in = {};
    bool going = false, sim = false;
    if (r) {
        dsg_run_get_info(r, &in);
        going = in.state != DSG_RUN_ENDED;
        sim = run_is_sim(r, in);
    }
    controls(in, going, sim);
    // one line that stays at the top: the state and the trials
    if (r) {
        ImGui::SameLine();
        ImGui::Text("%s%s, %.1f s. Trials written: %d.", state_word(in.state), sim ? " (simulated)" : "", (double)in.elapsed_ns * 1e-9,
                    in.trials);
    }
    // "##section": the Build section's header and its button are both "Build"
    if (ImGui::CollapsingHeader("Rig##section", ImGuiTreeNodeFlags_DefaultOpen)) rig_section(rs);
    if (ImGui::CollapsingHeader("Build##section", ImGuiTreeNodeFlags_DefaultOpen)) build_section();
    if (ImGui::CollapsingHeader("Run settings##section", ImGuiTreeNodeFlags_DefaultOpen)) settings_section(rs);
    if (ImGui::CollapsingHeader("Output##section", ImGuiTreeNodeFlags_DefaultOpen)) output_section(r, in, sim);
}

void reset() {
    free_rig();
    V = View();
}

void closed() { reset(); }
void shutdown() { reset(); }

} // namespace

extern const UiView ui_view_run = {
    "run", "Run", UI_SLOT_AUX, nullptr, shutdown, nullptr, closed, nullptr, draw, nullptr, nullptr, nullptr, nullptr, nullptr,
};
