// ui.cpp - the designer's skeleton (docs/designer.md, "Order of work", step
// 3): a project's definition as text in a Skribidi editor, checked by the
// player's load on each change, its issues linked to their places, open and
// save. Everything that needs no ImGui is in designer/core.
#include "ui.h"
#include "skf.h"
#include "skt.h"

#include "imgui.h"
#include "imgui_internal.h"

#include "skribidi/skb_layout.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

enum Pending { PENDING_NONE, PENDING_OPEN, PENDING_NEW, PENDING_QUIT };

struct State {
    UiPlatform plat = {};
    dsg_project proj = {};
    bool open = false;
    dsg_checker* checker = nullptr;
    SkfField* editor = nullptr;
    ImVector<char> text;             // the editor's text, NUL-ended
    size_t text_len = 0;
    bool dirty = false;
    bool check_due = false;
    int checks = 0;
    dsg_report report = {};
    bool have_report = false;
    int selected = -1;               // the issue last clicked
    float scroll_y = 0;              // the editor's scroll, for the view file
    float restore_scroll = -1;       // a scroll to set at the next frame
    char path[DSG_PATH_MAX] = "";    // the typed path
    bool focus_path = false;
    char status[1024] = "";
    char title[DSG_PATH_MAX + 64] = "";
    Pending pending = PENDING_NONE;
    char pending_path[DSG_PATH_MAX] = "";
    bool ask_unsaved = false;
};
State* S = nullptr;

const float kTextSize = 16.0f;

void set_status(const char* fmt, ...) IM_FMTARGS(1);
void set_status(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(S->status, sizeof S->status, fmt, ap);
    va_end(ap);
}

void read_editor() {
    S->text_len = (size_t)skf_text(S->editor, &S->text);
    S->dirty = S->open && dsg_project_dirty(&S->proj, S->text.Data, S->text_len);
}

void set_editor_text(const char* t, size_t n) {
    skf_set_text(S->editor, t, (int)n);
    read_editor();
}

// The caret as line and column, for the view file.
void caret_line_col(uint32_t* line, uint32_t* col) {
    uint32_t b = dsg_byte_of_codepoint(S->text.Data, S->text_len, (uint32_t)skf_caret(S->editor));
    dsg_line_col(S->text.Data, S->text_len, b, line, col);
}

void place_caret(uint32_t line, uint32_t col) {
    uint32_t off = dsg_offset_of(S->text.Data, S->text_len, line, col);
    skf_set_caret(S->editor, (int)dsg_codepoints_before(S->text.Data, S->text_len, off));
}

void write_view() {
    if (!S->open) return;
    dsg_view v = {};
    caret_line_col(&v.caret_line, &v.caret_column);
    v.scroll_y = S->scroll_y;
    size_t n = 0;
    const char* ini = ImGui::SaveIniSettingsToMemory(&n);
    v.imgui_ini = (char*)ini;   // not owned: dsg_view_write only reads it
    v.imgui_ini_len = n;
    char err[512];
    if (dsg_view_write(S->proj.folder, &v, err, sizeof err)) set_status("The view state was not saved: %s", err);
}

void close_project() {
    if (S->open) {
        write_view();
        dsg_project_close(&S->proj);
    }
    S->open = false;
    S->have_report = false;
    S->selected = -1;
    S->dirty = false;
    set_editor_text("", 0);
    if (S->checker) dsg_checker_forget(S->checker);
}

void open_now(const char* path) {
    dsg_project p;
    char err[1024];
    if (dsg_project_open(&p, path, err, sizeof err)) {
        set_status("Not opened: %s", err);
        return;
    }
    close_project();
    S->proj = p;
    S->open = true;
    set_editor_text(S->proj.text, S->proj.text_len);
    if (S->proj.view.imgui_ini) ImGui::LoadIniSettingsFromMemory(S->proj.view.imgui_ini, S->proj.view.imgui_ini_len);
    if (S->proj.view.caret_line) place_caret(S->proj.view.caret_line, S->proj.view.caret_column);
    S->restore_scroll = S->proj.view.scroll_y;
    S->check_due = true;
    snprintf(S->path, sizeof S->path, "%s", S->proj.folder);
    if (S->proj.note[0]) set_status("Opened %s. %s.", S->proj.folder, S->proj.note);
    else set_status("Opened %s.", S->proj.folder);
}

void new_now(const char* path) {
    char err[1024];
    if (!path[0]) {
        set_status("Type the new project's folder in the path field, then press New.");
        S->focus_path = true;
        return;
    }
    if (dsg_project_create(path, err, sizeof err)) {
        set_status("Not made: %s", err);
        return;
    }
    open_now(path);
}

// Runs what waited for the unsaved-changes question.
void run_pending() {
    Pending p = S->pending;
    S->pending = PENDING_NONE;
    if (p == PENDING_OPEN) open_now(S->pending_path);
    else if (p == PENDING_NEW) new_now(S->pending_path);
    else if (p == PENDING_QUIT) {
        close_project();
        if (S->plat.quit) S->plat.quit(S->plat.ctx);
    }
}

void ask_or_run(Pending p, const char* path) {
    S->pending = p;
    snprintf(S->pending_path, sizeof S->pending_path, "%s", path ? path : "");
    if (S->dirty) S->ask_unsaved = true;
    else run_pending();
}

void run_check() {
    S->check_due = false;
    if (!S->open) return;
    dsg_check(S->checker, &S->proj, S->text.Data, S->text_len, &S->report);
    S->have_report = true;
    S->checks++;
    if (S->selected >= S->report.n_issues) S->selected = -1;
}

const char* sev_name(int s) {
    switch (s) {
    case DSG_SEV_ERROR: return "error";
    case DSG_SEV_WARNING: return "warning";
    case DSG_SEV_NEED: return "run needs";
    default: return "note";
    }
}

ImU32 sev_color(int s) {
    switch (s) {
    case DSG_SEV_ERROR: return IM_COL32(255, 110, 100, 255);
    case DSG_SEV_WARNING: return IM_COL32(240, 200, 90, 255);
    case DSG_SEV_NEED: return IM_COL32(140, 190, 255, 255);
    default: return IM_COL32(180, 180, 180, 255);
    }
}

void update_title() {
    char t[sizeof S->title];
    if (S->open) snprintf(t, sizeof t, "%s%s - %s - ysp designer", S->dirty ? "*" : "", S->proj.def_source, S->proj.folder);
    else snprintf(t, sizeof t, "ysp designer");
    if (strcmp(t, S->title) != 0) {
        memcpy(S->title, t, sizeof t);
        if (S->plat.set_title) S->plat.set_title(S->plat.ctx, S->title);
    }
}

void toolbar() {
    if (ImGui::Button("New")) ask_or_run(PENDING_NEW, S->path);
    ImGui::SameLine();
    if (ImGui::Button("Open...")) {
        if (!S->plat.pick_folder || !S->plat.pick_folder(S->plat.ctx)) ui_dialog_failed("no folder dialog on this platform");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!S->open);
    if (ImGui::Button("Save")) ui_save();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted("Path");
    ImGui::SameLine();
    if (S->focus_path) {
        ImGui::SetKeyboardFocusHere();
        S->focus_path = false;
    }
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Open path").x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x);
    const bool enter = ImGui::InputTextWithHint("##path", "a project folder, or its pack.json", S->path, sizeof S->path,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Open path") || enter) && S->path[0]) ask_or_run(PENDING_OPEN, S->path);
    ImGui::TextWrapped("%s", S->status);
}

void issues_panel() {
    if (!S->open) return;
    if (!S->have_report) {
        ImGui::TextUnformatted("Not checked yet.");
        return;
    }
    const dsg_report* r = &S->report;
    int warn = 0, need = 0;
    for (int i = 0; i < r->n_issues; i++) {
        warn += r->issues[i].severity == DSG_SEV_WARNING;
        need += r->issues[i].severity == DSG_SEV_NEED;
    }
    if (r->status == DSG_CHECK_OK && r->n_errors == 0)
        ImGui::Text("The player loads this definition. %d warnings; a real run needs %d things.", warn, need);
    else
        ImGui::Text("%d errors, %d warnings; a real run needs %d things.%s", r->n_errors, warn, need,
                    r->have_pack ? "" : " No check pack yet: the player's own checks wait for one.");
    ImGui::Separator();
    ImGui::BeginChild("##issue_list");
    const float row = kTextSize * 1.5f;
    for (int i = 0; i < r->n_issues; i++) {
        const dsg_issue* is = &r->issues[i];
        ImGui::PushID(i);
        char place[48];
        if (is->place.line) snprintf(place, sizeof place, "%u:%u", is->place.line, is->place.column);
        else snprintf(place, sizeof place, "-");
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##issue", S->selected == i, 0, ImVec2(0, row))) ui_jump_to_issue(i);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(at.x, at.y + 2), IM_COL32(170, 170, 170, 255), place);
        dl->AddText(ImVec2(at.x + 70, at.y + 2), sev_color(is->severity), sev_name(is->severity));
        // The message names the definition's own things, which can be in any
        // script, so it is laid out by Skribidi.
        const skb_layout_t* lay = skt_layout(is->message, kTextSize);
        if (lay) {
            dl->PushClipRect(ImVec2(at.x + 150, at.y), ImVec2(at.x + ImGui::GetContentRegionAvail().x + 150, at.y + row), true);
            skt_draw_layout(dl, lay, at.x + 150, at.y + 2, 1.0f, IM_COL32(230, 230, 235, 255));
            dl->PopClipRect();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void unsaved_modal() {
    if (S->ask_unsaved) {
        ImGui::OpenPopup("Unsaved changes");
        S->ask_unsaved = false;
    }
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s has changes that are not saved.", S->open ? S->proj.def_source : "The definition");
        if (ImGui::Button("Save")) {
            ImGui::CloseCurrentPopup();
            if (ui_save()) run_pending();
            else S->pending = PENDING_NONE;
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            ImGui::CloseCurrentPopup();
            S->dirty = false;
            run_pending();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
            S->pending = PENDING_NONE;
        }
        ImGui::EndPopup();
    }
}

} // namespace

bool ui_init(const char* font_dir, const UiPlatform* platform, char* err, size_t cap) {
    S = IM_NEW(State)();
    if (platform) S->plat = *platform;
    if (!skt_init(font_dir, err, cap)) return false;
    S->checker = dsg_checker_new();
    if (!S->checker) {
        snprintf(err, cap, "out of memory for the checker");
        return false;
    }
    S->editor = skf_create(kTextSize, true);
    set_editor_text("", 0);
    // The UI's own text: Noto Sans when it is there, else ImGui's font.
    char path[DSG_PATH_MAX];
    if (dsg_join(path, sizeof path, font_dir, "NotoSans-Regular.ttf") == 0 && dsg_is_file(path))
        ImGui::GetIO().Fonts->AddFontFromFileTTF(path, 16.0f);
    set_status("Open a project: a folder with a pack.json. Type its path, or press Open..., or drop it on the window.");
    return true;
}

void ui_shutdown() {
    if (!S) return;
    close_project();
    skf_destroy(S->editor);
    dsg_checker_free(S->checker);
    skt_shutdown();
    IM_DELETE(S);
    S = nullptr;
}

void ui_open(const char* path) { ask_or_run(PENDING_OPEN, path); }

void ui_dialog_failed(const char* why) {
    set_status("No folder dialog (%s). Type the project's folder in the path field and press Open path.", why);
    S->focus_path = true;
}

bool ui_request_quit() {
    if (!S->dirty) {
        close_project();
        return true;
    }
    ask_or_run(PENDING_QUIT, nullptr);
    return false;
}

bool ui_save() {
    if (!S->open) return false;
    uint32_t line = 0, col = 0;
    caret_line_col(&line, &col);
    dsg_saved s;
    char err[1024];
    if (dsg_project_save(&S->proj, S->text.Data, S->text_len, &s, err, sizeof err)) {
        set_status("Not saved: %s", err);
        return false;
    }
    if (s.n != S->text_len || memcmp(s.bytes, S->text.Data, s.n) != 0) {
        // the canonical form moves text: the caret keeps its line and column
        set_editor_text(s.bytes, s.n);
        place_caret(line, col);
        S->check_due = true;
    }
    read_editor();
    if (s.canonical) set_status("Saved %s: canonical bytes, format version 2.", S->proj.def_source);
    else set_status("Saved %s as typed: the loader refuses the text, so the bytes are not canonical. Fix the errors and save again.",
                    S->proj.def_source);
    dsg_saved_free(&s);
    write_view();
    return true;
}

void ui_jump_to_issue(int i) {
    if (!S->have_report || i < 0 || i >= S->report.n_issues) return;
    S->selected = i;
    const dsg_issue* is = &S->report.issues[i];
    if (!is->place.line) return;
    // the place is in the text the check read, which is the editor's text
    skf_set_caret(S->editor, (int)dsg_codepoints_before(S->text.Data, S->text_len, is->place.offset));
}

void ui_frame() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##designer", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) ui_save();
    toolbar();
    ImGui::Separator();

    const float issues_h = ImMax(120.0f, ImGui::GetContentRegionAvail().y * 0.3f);
    ImGui::BeginChild("Definition", ImVec2(0, -issues_h), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    if (S->open) {
        if (S->restore_scroll >= 0) {
            ImGui::SetScrollY(S->restore_scroll);
            S->restore_scroll = -1;
        }
        if (skf_draw("##definition", S->editor, 0, 0)) {
            read_editor();
            S->check_due = true;
        }
        S->scroll_y = ImGui::GetScrollY();
    } else {
        ImGui::TextUnformatted("No project is open.");
    }
    ImGui::EndChild();

    // The check runs in the frame of the change, after the editor, so the
    // issues below show this text's result. pl_check() takes 0.2 to 7 ms on
    // the example packs (player.md, Measured): inside a frame.
    if (S->check_due) run_check();

    ImGui::BeginChild("Issues", ImVec2(0, 0), ImGuiChildFlags_Borders);
    issues_panel();
    ImGui::EndChild();
    unsaved_modal();
    ImGui::End();
    update_title();
}

bool ui_work_pending() { return S && (S->check_due || S->restore_scroll >= 0 || S->ask_unsaved); }

const dsg_project* ui_project() { return S && S->open ? &S->proj : nullptr; }
const dsg_report* ui_report() { return S && S->have_report ? &S->report : nullptr; }
SkfField* ui_editor() { return S ? S->editor : nullptr; }
const char* ui_text(size_t* n) {
    *n = S->text_len;
    return S->text.Data;
}
const char* ui_status() { return S->status; }
bool ui_dirty() { return S->dirty; }
int ui_checks_run() { return S->checks; }
