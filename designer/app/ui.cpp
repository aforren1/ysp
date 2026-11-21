// ui.cpp - the designer's window (docs/designer.md, steps 3 and 4): the
// toolbar, the switch between the main views, the text view (a Skribidi
// editor of the definition), the side column, the issues, open and save.
// Every view edits one document (designer/core/doc.h): its bytes are the
// truth, and one undo stack serves them all. The views other than the text
// are in their own files (views.h). Everything that needs no ImGui is in
// designer/core.
#include "ui.h"
#include "views.h"
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

// The text view is a view like the others, kept here because the skeleton's
// tests and the issues' jumps reach into it.
const int kTextView = -1;

struct State {
    UiPlatform plat = {};
    dsg_project proj = {};
    bool open = false;
    dsg_worker* worker = nullptr;    // the check thread (worker.c)
    dsg_doc* doc = nullptr;
    SkfField* editor = nullptr;
    ImVector<char> scratch;          // the editor's text, read after a change
    uint64_t text_rev = 0;           // buffer 0's revision that the editor holds
    uint64_t checked = 0;            // the revision of the report shown
    uint64_t posted = 0;             // the document's revision last posted
    uint64_t floor = 0;              // a result below it is another project's
    int last_caret = -1;             // the editor's caret last frame
    bool dirty = false;
    uint64_t dirty_rev = 0;          // the document's revision that dirty is of
    bool check_due = false;          // post even if the revision is the same
    int checks = 0;                  // results taken
    dsg_report report = {};
    bool have_report = false;
    ImVector<char> issue_paths;      // NUL-ended paths, one per issue
    ImVector<int> issue_path_at;
    int selected = -1;               // the issue last clicked
    float scroll_y = 0;              // the editor's scroll, for the view file
    float restore_scroll = -1;       // a scroll to set at the next frame
    int main_view = kTextView;       // index into the views, or kTextView
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

const UiView* const* views(int* n) { return ui_views(n); }

void set_status_v(const char* fmt, va_list ap) { vsnprintf(S->status, sizeof S->status, fmt, ap); }

void set_status(const char* fmt, ...) IM_FMTARGS(1);
void set_status(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    set_status_v(fmt, ap);
    va_end(ap);
}

const char* def_bytes(size_t* n) { return dsg_doc_bytes(S->doc, DSG_BUF_DEF, n); }

void update_dirty() {
    bool d = false;
    for (int i = 0; S->open && i < dsg_doc_buffer_count(S->doc); i++) d = d || dsg_doc_dirty(S->doc, i);
    S->dirty = d;
    S->dirty_rev = dsg_doc_revision(S->doc);
}

// Any buffer's change: a table edit leaves the definition, and so
// sync_editor(), alone. dsg_doc_dirty() compares a buffer with its saved
// bytes, so it runs only when the document moved.
void refresh_dirty() {
    if (S->open && dsg_doc_revision(S->doc) != S->dirty_rev) update_dirty();
}

// The editor's text went into the document: typing, a paste, a cut.
void read_editor() {
    const int n = skf_text(S->editor, &S->scratch);
    if (dsg_doc_replace(S->doc, DSG_BUF_DEF, S->scratch.Data, (size_t)n, DSG_EDIT_MERGE))
        set_status("The edit was not kept: %s", dsg_doc_error(S->doc));
    S->text_rev = dsg_doc_rev(S->doc, DSG_BUF_DEF);
    update_dirty();
}

// The document's definition changed elsewhere (another view, undo, save):
// the editor shows it, with the caret at the change.
void sync_editor() {
    const uint64_t r = dsg_doc_rev(S->doc, DSG_BUF_DEF);
    if (r == S->text_rev) return;
    size_t n;
    const char* t = def_bytes(&n);
    skf_set_text(S->editor, t, (int)n);
    S->text_rev = r;
    int buf = -1;
    size_t off = 0, len = 0;
    if (dsg_doc_last_change(S->doc, &buf, &off, &len) && buf == DSG_BUF_DEF)
        skf_move_caret(S->editor, (int)dsg_codepoints_before(t, n, (uint32_t)(off + len)));
    S->last_caret = skf_caret(S->editor);
    update_dirty();
}

// The caret as line and column, for the view file.
void caret_line_col(uint32_t* line, uint32_t* col) {
    size_t n;
    const char* t = def_bytes(&n);
    uint32_t b = dsg_byte_of_codepoint(t, n, (uint32_t)skf_caret(S->editor));
    dsg_line_col(t, n, b, line, col);
}

void place_caret(uint32_t line, uint32_t col) {
    size_t n;
    const char* t = def_bytes(&n);
    uint32_t off = dsg_offset_of(t, n, line, col);
    skf_set_caret(S->editor, (int)dsg_codepoints_before(t, n, off));
}

const char* main_id() {
    int nv;
    const UiView* const* v = views(&nv);
    return S->main_view >= 0 && S->main_view < nv ? v[S->main_view]->id : "text";
}

void write_view() {
    if (!S->open) return;
    int nv;
    const UiView* const* v = views(&nv);
    for (int i = 0; i < nv; i++)
        if (v[i]->save_view) v[i]->save_view(&S->proj.view);
    // a shallow copy: the parts stay the project's, the ImGui text is ImGui's
    dsg_view w = S->proj.view;
    caret_line_col(&w.caret_line, &w.caret_column);
    w.scroll_y = S->scroll_y;
    size_t n = 0;
    w.imgui_ini = (char*)ImGui::SaveIniSettingsToMemory(&n);
    w.imgui_ini_len = n;
    snprintf(w.main_view, sizeof w.main_view, "%s", main_id());
    const dsg_sel* s = dsg_doc_selection(S->doc);
    snprintf(w.selection, sizeof w.selection, "%s", s->buf == DSG_BUF_DEF ? s->path : "");
    char err[512];
    if (dsg_view_write(S->proj.folder, &w, err, sizeof err)) set_status("The view state was not saved: %s", err);
}

void close_project() {
    int nv;
    const UiView* const* v = views(&nv);
    if (S->open) {
        write_view();
        for (int i = 0; i < nv; i++)
            if (v[i]->closed) v[i]->closed();
        dsg_project_close(&S->proj);
    }
    S->open = false;
    S->have_report = false;
    S->issue_paths.resize(0);
    S->issue_path_at.resize(0);
    S->selected = -1;
    S->dirty = false;
    dsg_doc_reset(S->doc, "", "", 0);
    sync_editor();
    // the files may change on disk before the next open
    if (S->worker) dsg_worker_forget(S->worker);
    S->floor = dsg_doc_revision(S->doc) + 1;
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
    if (dsg_doc_reset(S->doc, S->proj.def_source, S->proj.text, S->proj.text_len) == 0)
        dsg_doc_set_saved(S->doc, DSG_BUF_DEF, S->proj.saved, S->proj.saved_len);
    sync_editor();
    int nv;
    const UiView* const* v = views(&nv);
    S->main_view = kTextView;
    for (int i = 0; i < nv; i++) {
        if (v[i]->slot == UI_SLOT_MAIN && strcmp(v[i]->id, S->proj.view.main_view) == 0) S->main_view = i;
        if (v[i]->opened) v[i]->opened(&S->proj.view);
    }
    if (S->proj.view.selection[0]) {
        dsg_sel s;
        dsg_sel_def(&s, S->proj.view.selection);
        dsg_doc_select(S->doc, &s);
    }
    if (S->proj.view.imgui_ini) ImGui::LoadIniSettingsFromMemory(S->proj.view.imgui_ini, S->proj.view.imgui_ini_len);
    if (S->proj.view.caret_line) place_caret(S->proj.view.caret_line, S->proj.view.caret_column);
    S->last_caret = skf_caret(S->editor);
    S->restore_scroll = S->proj.view.scroll_y;
    S->check_due = true;
    update_dirty();
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
    refresh_dirty();
    if (S->dirty) S->ask_unsaved = true;
    else run_pending();
}

// Each issue's value in the tree the check read, for the views to mark.
void map_issues() {
    S->issue_paths.resize(0);
    S->issue_path_at.resize(0);
    const yjs_value* root = dsg_doc_root(S->doc);
    char p[DSG_SEL_PATH];
    for (int i = 0; i < S->report.n_issues; i++) {
        const dsg_issue* is = &S->report.issues[i];
        p[0] = 0;
        if (root && is->place.line && dsg_path_at(root, is->place.offset, p, sizeof p)) p[0] = 0;
        S->issue_path_at.push_back(S->issue_paths.Size);
        const int k = (int)strlen(p);
        for (int j = 0; j <= k; j++) S->issue_paths.push_back(p[j]);
    }
}

// The check runs on the check thread: the post copies the text and the
// tables as edited (so a table edit is checked before Save), and the result
// comes back in a later frame with the revision it was posted for.
void post_check() {
    S->check_due = false;
    if (!S->open) return;
    size_t n;
    const char* t = def_bytes(&n);
    const uint64_t rev = dsg_doc_revision(S->doc);
    if (dsg_worker_check(S->worker, &S->proj, t, n, S->doc, rev) == 0) S->posted = rev;
    else set_status("The check was not started: out of memory.");
    // without a thread the check runs here, and the next frame takes it
    if (!dsg_worker_threaded(S->worker)) dsg_worker_wait(S->worker, 0);
}

void take_check() {
    const dsg_check_result* res = nullptr;
    if (!S->worker || !dsg_worker_take_check(S->worker, &res)) return;
    // A result posted before the open belongs to the project before it.
    // The take recycled the storage of the report shown, so drop that too.
    if (!S->open || res->revision < S->floor) {
        S->report = dsg_report{};
        S->have_report = false;
        return;
    }
    S->report = res->report;
    S->checked = res->revision;
    S->have_report = true;
    S->checks++;
    map_issues();
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

// Ctrl+Z belongs to the document unless a text field with its own undo has
// the keyboard: ImGui's path field, or a Skribidi field that keeps its own.
bool undo_keys_free() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.ActiveId != 0 && g.InputTextState.ID == g.ActiveId) return false;
    SkfField* f = skf_focus_field();
    return !f || !skf_focused(f) || !skf_owns_undo(f);
}

void undo() {
    if (dsg_doc_can_undo(S->doc) && dsg_doc_undo(S->doc) == 0) set_status("Undid: %s.", dsg_doc_redo_label(S->doc));
}

void redo() {
    if (dsg_doc_can_redo(S->doc) && dsg_doc_redo(S->doc) == 0) set_status("Redid: %s.", dsg_doc_undo_label(S->doc));
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
    ImGui::BeginDisabled(!dsg_doc_can_undo(S->doc));
    if (ImGui::Button("Undo")) undo();
    ImGui::SetItemTooltip("%s", dsg_doc_undo_label(S->doc));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!dsg_doc_can_redo(S->doc));
    if (ImGui::Button("Redo")) redo();
    ImGui::SetItemTooltip("%s", dsg_doc_redo_label(S->doc));
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

// Text | Chart | Tables: which main view shows.
void view_switch() {
    int nv;
    const UiView* const* v = views(&nv);
    // its own ID scope: a view's child window has the view's label as its name
    ImGui::PushID("switch");
    if (ImGui::RadioButton("Text", S->main_view == kTextView)) S->main_view = kTextView;
    for (int i = 0; i < nv; i++) {
        if (v[i]->slot != UI_SLOT_MAIN) continue;
        ImGui::SameLine();
        if (ImGui::RadioButton(v[i]->label, S->main_view == i)) S->main_view = i;
    }
    ImGui::PopID();
}

void text_view() {
    if (!S->open) {
        ImGui::TextUnformatted("No project is open.");
        return;
    }
    if (S->restore_scroll >= 0) {
        ImGui::SetScrollY(S->restore_scroll);
        S->restore_scroll = -1;
    }
    if (skf_draw("##definition", S->editor, 0, 0)) read_editor();
    S->scroll_y = ImGui::GetScrollY();
    // The caret selects the value it is in, so the panels follow the text.
    const int c = skf_caret(S->editor);
    if (c != S->last_caret && skf_focused(S->editor)) {
        S->last_caret = c;
        size_t n;
        const char* t = def_bytes(&n);
        const yjs_value* root = dsg_doc_root(S->doc);
        char p[DSG_SEL_PATH];
        if (root && dsg_path_at(root, dsg_byte_of_codepoint(t, n, (uint32_t)c), p, sizeof p) == 0 &&
            (dsg_doc_selection(S->doc)->buf != DSG_BUF_DEF || strcmp(p, dsg_doc_selection(S->doc)->path) != 0)) {
            dsg_sel s;
            dsg_sel_def(&s, p);
            dsg_doc_select(S->doc, &s);
        }
    }
}

int text_claims(const dsg_sel* s) { return s->buf == DSG_BUF_DEF ? 1 : 0; }

void text_reveal(const dsg_sel* s) {
    uint32_t off = 0;
    const yjs_value* root = dsg_doc_root(S->doc);
    if (!root || !s->path[0] || !dsg_path_offset(root, s->path, &off)) return;
    size_t n;
    const char* t = def_bytes(&n);
    skf_move_caret(S->editor, (int)dsg_codepoints_before(t, n, off));
    S->last_caret = skf_caret(S->editor);
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
    // After skt_init(): Skribidi numbers its font collections with a plain
    // static counter, and the pack tool on the check thread makes them too.
    dsg_worker_desc wd = {};
    if (platform) {
        wd.wake = platform->wake;
        wd.ctx = platform->ctx;
    }
    S->worker = dsg_worker_start(&wd, err, cap);
    if (!S->worker) return false;
    S->doc = dsg_doc_new();
    if (!S->doc || dsg_doc_reset(S->doc, "", "", 0)) {
        snprintf(err, cap, "out of memory for the document");
        return false;
    }
    S->editor = skf_create(kTextSize, true);
    // the document's undo stack takes Ctrl+Z, so it spans every view
    skf_set_own_undo(S->editor, false);
    S->text_rev = 0;
    sync_editor();
    // The UI's own text: Noto Sans when it is there, else ImGui's font.
    char path[DSG_PATH_MAX];
    if (dsg_join(path, sizeof path, font_dir, "NotoSans-Regular.ttf") == 0 && dsg_is_file(path))
        ImGui::GetIO().Fonts->AddFontFromFileTTF(path, 16.0f);
    int nv;
    const UiView* const* v = views(&nv);
    for (int i = 0; i < nv; i++)
        if (v[i]->init && !v[i]->init(err, cap)) return false;
    set_status("Open a project: a folder with a pack.json. Type its path, or press Open..., or drop it on the window.");
    return true;
}

void ui_shutdown() {
    if (!S) return;
    close_project();
    int nv;
    const UiView* const* v = views(&nv);
    for (int i = 0; i < nv; i++)
        if (v[i]->shutdown) v[i]->shutdown();
    skf_destroy(S->editor);
    dsg_worker_stop(S->worker);
    dsg_doc_free(S->doc);
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
    refresh_dirty();
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
    size_t n;
    const char* t = def_bytes(&n);
    dsg_saved s;
    char err[1024];
    if (dsg_project_save(&S->proj, t, n, &s, err, sizeof err)) {
        set_status("Not saved: %s", err);
        return false;
    }
    if (s.n != n || memcmp(s.bytes, t, s.n) != 0) {
        // the canonical form is a step of its own: undo gives the text back
        dsg_doc_begin(S->doc, "Canonical form");
        dsg_doc_replace(S->doc, DSG_BUF_DEF, s.bytes, s.n, 0);
        dsg_doc_commit(S->doc);
        sync_editor();
        place_caret(line, col);   // the canonical form moves text: the caret keeps its line and column
    }
    dsg_doc_set_saved(S->doc, DSG_BUF_DEF, s.bytes, s.n);
    // The tables: their bytes as they are (the designer writes no other form).
    int failed = 0;
    for (int b = 1; b < dsg_doc_buffer_count(S->doc); b++) {
        if (!dsg_doc_dirty(S->doc, b)) continue;
        char path[DSG_PATH_MAX];
        size_t bn;
        const char* bytes = dsg_doc_bytes(S->doc, b, &bn);
        if (dsg_join(path, sizeof path, S->proj.folder, dsg_doc_buffer_name(S->doc, b)) ||
            dsg_write_file(path, bytes, bn, err, sizeof err)) {
            failed++;
            continue;
        }
        dsg_doc_mark_saved(S->doc, b);
    }
    update_dirty();
    if (failed) set_status("Saved %s; %d tables were not saved: %s", S->proj.def_source, failed, err);
    else if (s.canonical) set_status("Saved %s: canonical bytes, format version 2.", S->proj.def_source);
    else set_status("Saved %s as typed: the loader refuses the text, so the bytes are not canonical. Fix the errors and save again.",
                    S->proj.def_source);
    dsg_saved_free(&s);
    write_view();
    return failed == 0;
}

void ui_jump_to_issue(int i) {
    if (!S->have_report || i < 0 || i >= S->report.n_issues) return;
    S->selected = i;
    const dsg_issue* is = &S->report.issues[i];
    // A place in a view's own data (a table's cell) first: such an issue
    // has no place in the definition, or one less exact than the cell.
    int nv;
    const UiView* const* v = views(&nv);
    dsg_sel s = {};
    dsg_sel_none(&s);
    bool have = false;
    for (int k = 0; k < nv && !have; k++) have = v[k]->issue_sel && v[k]->issue_sel(i, &s);
    const char* p = ui_issue_path(i);
    if (!have && p[0]) {
        dsg_sel_def(&s, p);
        have = true;
    }
    if (have) ui_reveal(&s);
    if (!is->place.line) return;
    // The place is in the text the check read. That is the editor's text,
    // or an older one while a newer check runs; dsg_codepoints_before()
    // clamps an offset past the end.
    size_t n;
    const char* t = def_bytes(&n);
    skf_set_caret(S->editor, (int)dsg_codepoints_before(t, n, is->place.offset));
    S->last_caret = skf_caret(S->editor);
}

void ui_frame() {
    // A result that landed since the last frame shows in this one: its
    // issues, before the views mark them.
    take_check();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##designer", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) ui_save();
    if (undo_keys_free()) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) redo();
    }
    sync_editor();
    toolbar();
    ImGui::Separator();
    view_switch();

    int nv;
    const UiView* const* v = views(&nv);
    bool side = false;
    for (int i = 0; i < nv; i++) side = side || v[i]->slot == UI_SLOT_SIDE;
    const float issues_h = ImMax(120.0f, ImGui::GetContentRegionAvail().y * 0.3f);
    const float side_w = side ? ImMax(280.0f, ImGui::GetContentRegionAvail().x * 0.3f) : 0.0f;
    const float main_w = side ? -(side_w + ImGui::GetStyle().ItemSpacing.x) : 0.0f;

    // The text view keeps the skeleton's child name, so its tests' paths hold.
    if (S->main_view == kTextView) {
        ImGui::BeginChild("Definition", ImVec2(main_w, -issues_h), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        text_view();
    } else {
        ImGui::BeginChild(v[S->main_view]->label, ImVec2(main_w, -issues_h), ImGuiChildFlags_Borders);
        if (S->open) v[S->main_view]->draw();
        else ImGui::TextUnformatted("No project is open.");
    }
    ImGui::EndChild();
    if (side) {
        // Side views are children of the main window too, so a test finds
        // them as "//##designer/LABEL".
        ImGui::SameLine();
        ImGui::BeginGroup();
        for (int i = 0; i < nv; i++) {
            if (v[i]->slot != UI_SLOT_SIDE) continue;
            ImGui::BeginChild(v[i]->label, ImVec2(0, -issues_h), ImGuiChildFlags_Borders);
            if (S->open) v[i]->draw();
            ImGui::EndChild();
        }
        ImGui::EndGroup();
    }

    // Posted in the frame of the change, after the views; the issues show
    // the result from a later frame. bimanual's check takes up to 12.9 ms
    // and a generated project's 201 ms (docs/designer.md, "The check
    // thread"), so the check is not in the frame.
    if (S->open && dsg_doc_revision(S->doc) != S->posted) S->check_due = true;
    if (S->check_due) post_check();

    ImGui::BeginChild("Issues", ImVec2(0, 0), ImGuiChildFlags_Borders);
    issues_panel();
    ImGui::EndChild();
    unsaved_modal();
    ImGui::End();
    // after the views: an edit in this frame's draw shows in this title
    refresh_dirty();
    update_title();
}

// A check that runs on the thread is not work for a frame: its wake-up
// (UiPlatform.wake) brings the frame that takes the result.
bool ui_work_pending() {
    return S && (S->check_due || S->restore_scroll >= 0 || S->ask_unsaved ||
                 (S->open && dsg_doc_revision(S->doc) != S->posted) || dsg_doc_rev(S->doc, DSG_BUF_DEF) != S->text_rev ||
                 (S->worker && !dsg_worker_threaded(S->worker) && S->open && S->checked != S->posted));
}

bool ui_animating() {
    if (!S) return false;
    int nv;
    const UiView* const* v = views(&nv);
    for (int i = 0; i < nv; i++)
        if (v[i]->animating && v[i]->animating()) return true;
    return false;
}

const dsg_project* ui_project() { return S && S->open ? &S->proj : nullptr; }
const dsg_report* ui_report() { return S && S->have_report ? &S->report : nullptr; }
SkfField* ui_editor() { return S ? S->editor : nullptr; }
const char* ui_text(size_t* n) { return def_bytes(n); }
const char* ui_status() { return S->status; }
bool ui_dirty() {
    refresh_dirty();
    return S->dirty;
}
int ui_checks_run() { return S->checks; }
bool ui_wait_checks(int64_t timeout_ns) { return S && dsg_worker_wait(S->worker, timeout_ns) == 0; }

const char* ui_main_view() { return main_id(); }

bool ui_show_view(const char* id) {
    int nv;
    const UiView* const* v = views(&nv);
    if (strcmp(id, "text") == 0) {
        S->main_view = kTextView;
        return true;
    }
    for (int i = 0; i < nv; i++)
        if (v[i]->slot == UI_SLOT_MAIN && strcmp(v[i]->id, id) == 0) {
            S->main_view = i;
            return true;
        }
    return false;
}

// --- views.h ---------------------------------------------------------------

dsg_doc* ui_doc() { return S->doc; }

const char* ui_issue_path(int i) {
    if (!S->have_report || i < 0 || i >= S->issue_path_at.Size) return "";
    return S->issue_paths.Data + S->issue_path_at[i];
}

void ui_reveal(const dsg_sel* s) {
    dsg_doc_select(S->doc, s);
    int nv;
    const UiView* const* v = views(&nv);
    int best = S->main_view;
    int best_claim = S->main_view == kTextView ? text_claims(s) : (v[S->main_view]->claims ? v[S->main_view]->claims(s) : 0);
    if (text_claims(s) > best_claim) {
        best = kTextView;
        best_claim = text_claims(s);
    }
    for (int i = 0; i < nv; i++) {
        if (v[i]->slot != UI_SLOT_MAIN || !v[i]->claims) continue;
        const int c = v[i]->claims(s);
        if (c > best_claim) {
            best = i;
            best_claim = c;
        }
    }
    S->main_view = best;
    if (best == kTextView) text_reveal(s);
    for (int i = 0; i < nv; i++)
        if ((i == best || v[i]->slot == UI_SLOT_SIDE) && v[i]->reveal) v[i]->reveal(s);
}

void ui_set_status(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    set_status_v(fmt, ap);
    va_end(ap);
}

float ui_text_size() { return kTextSize; }
