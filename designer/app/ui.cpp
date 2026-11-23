// ui.cpp - the designer's window (docs/designer.md, steps 3 to 6): the
// toolbar, the switch between the main views, the text view (a Skribidi
// editor of the definition), the AUX pane (the preview, the Run view), the
// side column, the issues, open and save, and Build and Run (the builder,
// the run and its settings, step 6). Every view edits one document
// (designer/core/doc.h): its bytes are the truth, and one undo stack serves
// them all. The views other than the text are in their own files (views.h).
// Everything that needs no ImGui is in designer/core.
#include "ui.h"
#include "views.h"
#include "flow.h"
#include "skf.h"
#include "skt.h"

#include "imgui.h"
#include "imgui_internal.h"

#include "skribidi/skb_layout.h"
#include "ysp/json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace {

enum Pending { PENDING_NONE, PENDING_OPEN, PENDING_NEW, PENDING_QUIT };

// The views that views.cpp lists, at most: one AUX check box each.
const int kMaxViews = 16;

// A run's wake-ups (the watch thread): the UI draws for them when the count
// moved since its last frame, at most DSG_RUN_REDRAW_HZ times a second.
volatile int32_t g_run_woken = 0;

// The text view is a view like the others, kept here because the skeleton's
// tests and the issues' jumps reach into it.
const int kTextView = -1;
// S->selected for the preview's issue, which is not in the check's report.
const int kPreviewIssue = -2;

// The AUX pane's width: a share of the room that the side column leaves, so
// it follows the window's size. The splitter sets it, and the view file's
// part "aux" keeps it; a share out of the bounds reads as the default.
const float kAuxShare = 0.45f, kAuxShareMin = 0.15f, kAuxShareMax = 0.85f;
// In points: the pane's least width, and what the main view keeps.
const float kAuxMinW = 320.0f, kMainMinW = 240.0f;
// The path field's least width in the toolbar: below it, the path's row
// wraps (docs/designer.md, "Rows that wrap").
const float kPathMinW = 200.0f;

struct State {
    UiPlatform plat = {};
    dsg_project proj = {};
    bool open = false;
    dsg_worker* worker = nullptr;    // the check thread (worker.c)
    dsg_preview_gl gl = {};          // read by the check thread: fixed at ui_init()
    dsg_preview* pv = nullptr;       // the preview (preview.h)
    bool aux_on[kMaxViews] = {};     // each AUX view's check box
    float aux_share = kAuxShare;     // the view file's part "aux"
    // The preview in an OS window of its own (2026-11-23, "Preview size"):
    // ImGui's multi-viewport. popped is kept in the part "aux".
    bool popped = false;
    bool pop_place = false;          // place the window at the next frame
    ImVec2 pane_pos = ImVec2(0, 0), pane_size = ImVec2(0, 0);   // the AUX pane, last drawn
    bool popped_shown = true;        // its OS window is not minimized
    void* popped_handle = nullptr;   // ImGuiViewport::PlatformHandle of it
    bool popped_fs = false;          // full screen
    bool window_shown = true;
    int64_t now = 0;                 // the UI's clock this frame
    dsg_preview_where where = {};    // ui_preview_where()'s
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
    // Step 6: the merged report (the check's issues, the Build's, the run's)
    ImVector<dsg_issue> merged;
    dsg_report mreport = {};
    bool have_merged = false;
    // Build
    dsg_builder* builder = nullptr;
    const dsg_build_result* build = nullptr;   // the last since the open
    bool run_after_build = false;    // Run asked for this Build
    // Run
    dsg_run_settings run_set = {};   // the view file's part "run"
    char check_rig[DSG_RIG_NAME] = "";   // the rig of the last check posted
    bool rig_default = false;        // the rig folder lists profile.json
    int64_t rig_default_ns = 0;      // when the folder was last listed
    bool rig_default_seen = false;   // it was listed since the open
    dsg_run* run = nullptr;          // the last run since the open
    bool run_end_seen = false;
    char run_rig[DSG_RIG_NAME] = ""; // the rig that the run had
    dsg_issue run_issue = {};        // its issue, a copy (run_msg)
    bool have_run_issue = false;
    char run_msg[1024] = "";
    int32_t run_seen = 0;            // g_run_woken at the last frame
    int64_t run_frame_ns = 0;        // the last frame's clock
    bool preview_held = false;       // a real run closed the preview
    dsg_preview_settings held = {};  // its settings, for after the run
    bool ask_run = false;            // the "a run goes" question
    bool wait_run = false;           // pending waits for the run's end
    dsg_run_argv argv = {};
};
State* S = nullptr;

void look_rig_default(bool force);

const float kTextSize = 16.0f;

const UiView* const* views(int* n) {
    const UiView* const* v = ui_views(n);
    if (*n > kMaxViews) *n = kMaxViews;
    return v;
}

// An AUX view's index by id, -1 when none.
int aux_index(const char* id) {
    int nv;
    const UiView* const* v = views(&nv);
    for (int i = 0; i < nv; i++)
        if (v[i]->slot == UI_SLOT_AUX && strcmp(v[i]->id, id) == 0) return i;
    return -1;
}

bool aux_view_on(const char* id) {
    const int i = aux_index(id);
    return i >= 0 && S->aux_on[i];
}

void set_aux_view(const char* id, bool on) {
    const int i = aux_index(id);
    if (i >= 0) S->aux_on[i] = on;
}

int64_t clock_now() { return S->plat.now_ns ? S->plat.now_ns(S->plat.ctx) : (int64_t)(ImGui::GetTime() * 1e9); }

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

// The part "aux": {"width": share, "popped": bool}. Missing, of another
// type or out of the bounds: the default (0.45, in the pane).
void read_aux_part(const dsg_view* v) {
    S->aux_share = kAuxShare;
    S->popped = false;
    S->popped_fs = false;
    size_t n = 0;
    const char* json = dsg_view_part_get(v, "aux", &n);
    if (!json) return;
    yjs_arena a;
    yjs_arena_init_heap(&a, (size_t)1 << 16);
    yjs_value* root = nullptr;
    yjs_error e;
    memset(&e, 0, sizeof e);
    double w = 0;
    bool pop = false;
    if (yjs_parse(&a, json, n, nullptr, &root, &e) == YJS_OK && root && root->type == YJS_OBJECT) {
        if (yjs_double(yjs_get(root, "width"), &w) == YJS_OK && w >= (double)kAuxShareMin && w <= (double)kAuxShareMax) S->aux_share = (float)w;
        if (yjs_bool(yjs_get(root, "popped"), &pop) == YJS_OK && pop) {
            S->popped = true;
            S->pop_place = true;
        }
    }
    yjs_arena_free(&a);
}

void write_view() {
    if (!S->open) return;
    int nv;
    const UiView* const* v = views(&nv);
    for (int i = 0; i < nv; i++)
        if (v[i]->save_view) v[i]->save_view(&S->proj.view);
    dsg_preview_settings ps = {};
    dsg_preview_get(S->pv, &ps);
    if (dsg_preview_part_write(&S->proj.view, &ps, aux_view_on("preview")))
        set_status("The preview's view state was not kept: the view file is full.");
    if (dsg_run_part_write(&S->proj.view, &S->run_set, aux_view_on("run"))) set_status("The Run view's settings were not kept: the view file is full.");
    char aux_part[64];
    const int an = snprintf(aux_part, sizeof aux_part, "{\"popped\":%s,\"width\":%.4f}", S->popped ? "true" : "false", (double)S->aux_share);
    if (an <= 0 || dsg_view_part_set(&S->proj.view, "aux", aux_part, (size_t)an))
        set_status("The AUX pane's width was not kept: the view file is full.");
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

void rebuild_merged();

// The preview gets its session again after a real run (decision 9): the
// next check's result lets it post a session job.
void release_preview() {
    if (!S->preview_held) return;
    S->preview_held = false;
    if (!S->open) return;
    dsg_preview_open(S->pv, &S->proj);
    dsg_preview_set(S->pv, &S->held);
    S->check_due = true;
}

// The run goes from the designer; a player that still runs is left running
// (its output goes to its log file, decision 6).
void drop_run() {
    dsg_run_free(S->run);
    S->run = nullptr;
    release_preview();
    if (S->have_run_issue) {
        S->have_run_issue = false;
        rebuild_merged();
    }
}

void close_project() {
    int nv;
    const UiView* const* v = views(&nv);
    if (S->open) {
        write_view();
        for (int i = 0; i < nv; i++)
            if (v[i]->closed) v[i]->closed();
        // its sessions go back to the check thread to close
        dsg_preview_close(S->pv);
        dsg_project_close(&S->proj);
    }
    S->open = false;
    S->preview_held = false;
    drop_run();
    // a Build of this project gives nothing to the next one
    if (S->builder) dsg_build_cancel(S->builder);
    S->build = nullptr;
    S->run_after_build = false;
    S->wait_run = false;
    S->have_report = false;
    S->have_merged = false;
    S->merged.resize(0);
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
    // After the document's reset: its revision now is the preview's floor.
    dsg_preview_open(S->pv, &S->proj);
    dsg_preview_settings ps = {};
    bool aux = false;
    dsg_preview_part_read(&S->proj.view, &ps, &aux);
    dsg_preview_set(S->pv, &ps);
    for (int i = 0; i < nv; i++) S->aux_on[i] = false;
    set_aux_view("preview", aux);
    aux = false;
    dsg_run_part_read(&S->proj.view, &S->run_set, &aux);
    set_aux_view("run", aux);
    read_aux_part(&S->proj.view);
    look_rig_default(true);
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

// Runs what waited for the questions.
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

// After the run's question: the unsaved changes' question, then the action.
void continue_pending() {
    refresh_dirty();
    if (S->dirty) S->ask_unsaved = true;
    else run_pending();
}

void ask_or_run(Pending p, const char* path) {
    S->pending = p;
    snprintf(S->pending_path, sizeof S->pending_path, "%s", path ? path : "");
    // a run first: Stop it, leave it running, or cancel
    if (ui_run_active()) S->ask_run = true;
    else continue_pending();
}

// Each issue's value in the tree the check read, for the views to mark.
void map_issues() {
    S->issue_paths.resize(0);
    S->issue_path_at.resize(0);
    const yjs_value* root = dsg_doc_root(S->doc);
    char p[DSG_SEL_PATH];
    for (int i = 0; i < S->mreport.n_issues; i++) {
        const dsg_issue* is = &S->mreport.issues[i];
        p[0] = 0;
        if (root && is->place.line && dsg_path_at(root, is->place.offset, p, sizeof p)) p[0] = 0;
        S->issue_path_at.push_back(S->issue_paths.Size);
        const int k = (int)strlen(p);
        for (int j = 0; j <= k; j++) S->issue_paths.push_back(p[j]);
    }
}

// The merged report (ui_report()): the check's issues, then the last
// Build's, then the run's. Each part's storage stays until its next take
// (the run's issue is a copy), and this runs after each take.
void rebuild_merged() {
    S->merged.resize(0);
    if (S->have_report)
        for (int i = 0; i < S->report.n_issues; i++) S->merged.push_back(S->report.issues[i]);
    if (S->build)
        for (int i = 0; i < S->build->report.n_issues; i++) S->merged.push_back(S->build->report.issues[i]);
    if (S->have_run_issue) S->merged.push_back(S->run_issue);
    if (S->have_report) S->mreport = S->report;
    else {
        S->mreport = dsg_report{};
        S->mreport.status = DSG_CHECK_NONE;
    }
    S->mreport.issues = S->merged.Data;
    S->mreport.n_issues = S->merged.Size;
    S->mreport.n_errors = 0;
    for (const dsg_issue& is : S->merged) S->mreport.n_errors += is.severity == DSG_SEV_ERROR;
    S->have_merged = S->have_report || S->merged.Size > 0;
    map_issues();
    if (S->selected >= S->mreport.n_issues) S->selected = -1;
}

// Lists the rig folder for profile.json: at the open and at most once a
// second after it (a listing allocates; 5 us for 3 profiles, G).
void look_rig_default(bool force) {
    if (!force && S->rig_default_seen && S->now - S->rig_default_ns < 1000000000LL) return;
    S->rig_default_seen = true;
    S->rig_default_ns = S->now;
    char first[1][DSG_RIG_NAME] = { "" }, err[256] = "";
    // rig.c lists "profile" first when profile.json is there
    S->rig_default = dsg_rig_list(first, 1, err, sizeof err) == 1 && strcmp(first[0], "profile") == 0;
}

// The rig profile of the check and of a run, one name for both. No profile
// chosen ("") is "profile" for a real run while the rig folder has
// profile.json: the player with no --rig loads it (player/lower.c), and so
// does dsg_rig_load(""), but the checker's "" is no profile. --sim with no
// --rig takes none, as the check's "" does.
const char* run_rig_name() {
    if (S->run_set.rig[0] || S->run_set.sim) return S->run_set.rig;
    look_rig_default(false);
    return S->rig_default ? "profile" : "";
}

// The check runs on the check thread: the post copies the text and the
// tables as edited (so a table edit is checked before Save), and the result
// comes back in a later frame with the revision it was posted for. The
// rig is the Run view's (decision 13).
void post_check() {
    S->check_due = false;
    if (!S->open) return;
    size_t n;
    const char* t = def_bytes(&n);
    const uint64_t rev = dsg_doc_revision(S->doc);
    snprintf(S->check_rig, sizeof S->check_rig, "%s", run_rig_name());
    if (dsg_worker_check(S->worker, &S->proj, t, n, S->doc, rev, S->check_rig) == 0) S->posted = rev;
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
        rebuild_merged();
        return;
    }
    S->report = res->report;
    S->checked = res->revision;
    S->have_report = true;
    S->checks++;
    rebuild_merged();
    dsg_preview_checked(S->pv, res, S->now);
}

// A session job's outcome; the preview owns it from here, also when no
// project is open (it hands it back to the check thread).
void take_session() {
    dsg_session_result r = {};
    if (S->worker && dsg_worker_take_session(S->worker, &r)) dsg_preview_adopt(S->pv, &r, S->now);
}

// The preview's pane shows: the preview runs only then.
// The popped-out preview's own window decides whether it shows; the main
// window decides for the AUX pane.
bool can_pop();
bool preview_window_shown() { return S->popped && can_pop() ? S->popped_shown : S->window_shown; }

bool aux_shown() { return S->open && aux_view_on("preview") && preview_window_shown() && !S->preview_held; }

// ImGui makes OS windows for viewports now: the platform and the renderer
// have them, and main.cpp enabled them (NewFrame() clears the flag when a
// backend cannot, as SDL3's on Wayland).
bool can_pop() {
#ifdef IMGUI_HAS_VIEWPORT
    return (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0;
#else
    return false;
#endif
}

// The preview's window of its own: a top-level ImGui window that never
// merges into the main viewport, with the OS's decorations (move, resize,
// close) and a task bar entry. Its close button pops the preview back in.
void popped_window(const UiView* pv_view) {
#ifdef IMGUI_HAS_VIEWPORT
    ImGuiWindowClass wc;
    wc.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    wc.ViewportFlagsOverrideClear = ImGuiViewportFlags_NoDecoration | ImGuiViewportFlags_NoTaskBarIcon;
    ImGui::SetNextWindowClass(&wc);
    if (S->pop_place) {
        // where the pane was, at least 960 x 640, so the display grows
        const ImGuiViewport* mv = ImGui::GetMainViewport();
        const ImVec2 at = S->pane_size.x > 0 ? S->pane_pos : ImVec2(mv->Pos.x + 40, mv->Pos.y + 40);
        ImGui::SetNextWindowPos(at);
        ImGui::SetNextWindowSize(ImVec2(ImMax(S->pane_size.x, 960.0f), ImMax(S->pane_size.y, 640.0f)));
        S->pop_place = false;
    }
    bool open = true;
    // the scroll bar always, as the pane's
    ImGui::Begin("Preview##window", &open,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_AlwaysVerticalScrollbar);
    const ImGuiViewport* vp = ImGui::GetWindowViewport();
    S->popped_handle = vp && vp != ImGui::GetMainViewport() ? vp->PlatformHandle : nullptr;
    S->popped_shown = !vp || !(vp->Flags & ImGuiViewportFlags_IsMinimized);
    if (S->open) pv_view->draw();
    else ImGui::TextUnformatted("No project is open.");
    // Escape leaves full screen when the session does not hold the keys
    if (S->popped_fs && !dsg_preview_keyboard(S->pv) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        ui_preview_set_fullscreen(false);
    ImGui::End();
    if (!open) ui_preview_pop(false);
#else
    (void)pv_view;
#endif
}

// --- Build and Run (step 6) ------------------------------------------------

bool run_going() {
    if (!S->run) return false;
    dsg_run_info in = {};
    dsg_run_get_info(S->run, &in);
    return in.state != DSG_RUN_ENDED;
}

// Why the toolbar's Build is disabled; nullptr when it is not.
const char* build_refusal() {
    if (!S->open) return "No project is open.";
    if (!S->builder) return "No build thread.";
    if (S->run_after_build || dsg_build_busy(S->builder)) return "A Build goes.";
    if (run_going()) return "A run goes.";
    return nullptr;
}

// Why the toolbar's Run is disabled; nullptr when it is not.
const char* run_refusal() {
    const char* b = build_refusal();
    if (b) return b;
    if (!S->have_report || S->checked != dsg_doc_revision(S->doc)) return "The check of this text has not come back yet.";
    if (S->report.n_errors > 0) return "The check has errors: fix them first.";
    return nullptr;
}

// On a run's watch thread: ctx is S->plat, which ui_init() fixed.
void run_wake(void* ctx) {
    dsg_atomic_add(&g_run_woken, 1);
    const UiPlatform* p = (const UiPlatform*)ctx;
    if (p->wake_run) p->wake_run(p->ctx);
}

// The next run's output folder when the settings name none: decision 11's
// name, or with -2, -3, ... -99 after it when a run in the same second took
// it (R's name has whole seconds, and the player and dsg_run_start() refuse
// a folder or a log DIR.log that exists). 0, or -1 with the reason in why.
int next_out_dir(int64_t now_unix_s, char* out, size_t cap, char* why, size_t why_cap) {
    char base[DSG_PATH_MAX], log[DSG_PATH_MAX + 8];
    why[0] = 0;
    if (!S->open || dsg_run_out_dir(S->proj.folder, &S->run_set, now_unix_s, base, sizeof base)) {
        snprintf(why, why_cap, "the output folder's path is too long");
        return -1;
    }
    for (int k = 1; k <= 99; k++) {
        const int n = k == 1 ? snprintf(out, cap, "%s", base) : snprintf(out, cap, "%s-%d", base, k);
        if (n < 0 || (size_t)n >= cap) break;
        snprintf(log, sizeof log, "%s.log", out);
        if (!dsg_is_dir(out) && !dsg_is_file(out) && !dsg_is_dir(log) && !dsg_is_file(log)) return 0;
    }
    snprintf(why, why_cap, "%s and the names -2 to -99 after it exist", base);
    return -1;
}

// A path that does not depend on the working folder (POSIX, a drive, UNC).
bool is_absolute(const char* p) {
    return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
}

// Starts the player with the last Build's pack.
bool start_run() {
    char out_dir[DSG_PATH_MAX], tmp[DSG_PATH_MAX], err[1024] = "";
    if (!S->plat.player_path || !S->plat.player_path[0]) {
        set_status("Not run: there is no ysp_player beside the designer.");
        return false;
    }
    if (!S->build || S->build->status != DSG_BUILD_OK) {
        set_status("Not run: no pack was built.");
        return false;
    }
    if (S->run_set.out_dir[0]) snprintf(out_dir, sizeof out_dir, "%s", S->run_set.out_dir);
    else if (next_out_dir((int64_t)time(nullptr), out_dir, sizeof out_dir, err, sizeof err)) {
        set_status("Not run: %s.", err);
        return false;
    }
    // the player refuses a folder that exists (decision 11): say so first
    if (dsg_is_dir(out_dir) || dsg_is_file(out_dir)) {
        set_status("Not run: the output folder %s exists. Choose another in the Run view.", out_dir);
        return false;
    }
    // the run's profile is the check's (decision 13): --rig profile for a
    // real run with none chosen while profile.json is there
    look_rig_default(true);
    dsg_run_settings rs = S->run_set;
    snprintf(rs.rig, sizeof rs.rig, "%s", run_rig_name());
    if (dsg_run_args(&rs, S->plat.player_path, S->build->path, out_dir, &S->argv, err, sizeof err)) {
        set_status("Not run: %s", err);
        return false;
    }
    if (dsg_join(tmp, sizeof tmp, S->proj.folder, DSG_DIR) || dsg_make_dir(tmp, err, sizeof err)) {
        set_status("Not run: %s", err[0] ? err : "the project's path is too long");
        return false;
    }
    dsg_run_desc d = {};
    d.argv = S->argv.argv;
    // The project's folder, as the plan says; but a project opened by a
    // relative path has a relative pack and output folder, which hold only
    // from the designer's own working folder: the player starts there.
    d.cwd = is_absolute(S->proj.folder) ? S->proj.folder : nullptr;
    d.out_dir = out_dir;
    d.tmp_dir = tmp;
    d.sim = S->run_set.sim;
    d.wake = run_wake;
    d.ctx = &S->plat;
    d.now_ns = S->now;
    drop_run();   // the last one ended: run_refusal() said so
    dsg_run* r = nullptr;
    if (dsg_run_start(&r, &d, err, sizeof err)) {
        set_status("Not run: %s", err);
        rebuild_merged();
        return false;
    }
    S->run = r;
    S->run_end_seen = false;
    snprintf(S->run_rig, sizeof S->run_rig, "%s", rs.rig);
    // A real run wants the display and the GPU: the preview stops and closes
    // its session until the run ends (decision 9). --sim needs neither.
    if (!S->run_set.sim) {
        dsg_preview_get(S->pv, &S->held);
        dsg_preview_close(S->pv);
        S->preview_held = true;
    }
    S->run_seen = dsg_atomic_load(&g_run_woken);
    rebuild_merged();
    set_status("Running%s: %s", S->run_set.sim ? " (simulated)" : "", out_dir);
    return true;
}

// A Build's result: its issues join the report, and a Run that waited for
// it starts.
void take_build() {
    const dsg_build_result* r = nullptr;
    if (!S->builder || !dsg_build_take(S->builder, &r)) return;
    // the take recycled the storage of the result shown
    S->build = nullptr;
    if (!S->open || r->revision < S->floor) {
        rebuild_merged();
        return;
    }
    S->build = r;
    rebuild_merged();
    if (r->status == DSG_BUILD_OK)
        set_status("Built %s: %lld bytes, %s, %.0f ms.", r->path, (long long)r->size, r->pack_id, r->ms);
    else if (r->status == DSG_BUILD_CANCELED) set_status("The Build was canceled.");
    else set_status("The Build failed: %d errors. The Issues panel lists them.", r->report.n_errors);
    if (!S->run_after_build) return;
    S->run_after_build = false;
    if (r->status == DSG_BUILD_OK) start_run();
    else set_status("Not run: the Build failed (%d errors). The Issues panel lists them.", r->report.n_errors);
}

// Once a frame: the run's progress, and its end.
void tick_run() {
    S->run_seen = dsg_atomic_load(&g_run_woken);
    S->run_frame_ns = S->now;
    if (!S->run) return;
    bool refresh = dsg_run_tick(S->run, S->now);
    dsg_run_info in = {};
    dsg_run_get_info(S->run, &in);
    if (in.state == DSG_RUN_ENDED && !S->run_end_seen) {
        S->run_end_seen = true;
        refresh = true;
        release_preview();
        set_status("The run ended: %s", in.exit_text ? in.exit_text : "");
    }
    if (!refresh) return;
    dsg_issue is = {};
    const bool had = S->have_run_issue;
    S->have_run_issue = dsg_run_issue(S->run, &is);
    if (S->have_run_issue) {
        snprintf(S->run_msg, sizeof S->run_msg, "%s", is.message ? is.message : "");
        S->run_issue = is;
        S->run_issue.message = S->run_msg;
        S->run_issue.name = nullptr;
    }
    if (S->have_run_issue || had) rebuild_merged();
}

const char* sev_name(const dsg_issue* is) {
    if (is->from == DSG_FROM_PREVIEW) return "preview";
    if (is->from == DSG_FROM_BUILD) return "build";
    if (is->from == DSG_FROM_PLAYER_RUN) return "run";
    if (is->from == DSG_FROM_RIG) return "rig";
    switch (is->severity) {
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
    flow_next(flow_button_w("Open..."));
    if (ImGui::Button("Open...")) {
        if (!S->plat.pick_folder || !S->plat.pick_folder(S->plat.ctx)) ui_dialog_failed("no folder dialog on this platform");
    }
    flow_next(flow_button_w("Save"));
    ImGui::BeginDisabled(!S->open);
    if (ImGui::Button("Save")) ui_save();
    ImGui::EndDisabled();
    flow_next(flow_button_w("Undo"));
    ImGui::BeginDisabled(!dsg_doc_can_undo(S->doc));
    if (ImGui::Button("Undo")) undo();
    ImGui::SetItemTooltip("%s", dsg_doc_undo_label(S->doc));
    ImGui::EndDisabled();
    flow_next(flow_button_w("Redo"));
    ImGui::BeginDisabled(!dsg_doc_can_redo(S->doc));
    if (ImGui::Button("Redo")) redo();
    ImGui::SetItemTooltip("%s", dsg_doc_redo_label(S->doc));
    ImGui::EndDisabled();
#ifndef __EMSCRIPTEN__
    // Build, Run, Stop (step 6). Their own ID scope: the Run view's child is
    // "//##designer/Run". Not in a browser (decision 16).
    ImGui::PushID("tool");
    const char* why = build_refusal();
    flow_next(flow_button_w("Build"));
    ImGui::BeginDisabled(why != nullptr);
    if (ImGui::Button("Build")) ui_build();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", why ? why : "Save, then build the pack into the project's build folder");
    why = run_refusal();
    flow_next(flow_button_w("Run"));
    ImGui::BeginDisabled(why != nullptr);
    if (ImGui::Button("Run")) ui_run_start();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", why ? why : "Start the player with the pack (Build first when it is stale)");
    if (run_going()) {
        dsg_run_info in = {};
        dsg_run_get_info(S->run, &in);
        flow_next(flow_button_w("Stop"));
        if (ImGui::Button("Stop")) ui_run_stop();
        ImGui::SetItemTooltip("Ask the player to stop: it closes the data set");
        if (in.kill_offered) {
            flow_next(flow_button_w("Kill"));
            if (ImGui::Button("Kill")) ui_run_kill();
            ImGui::SetItemTooltip("End the player now. Its data set is not closed, and the display's mode and gamma ramp may stay changed");
        }
    }
    ImGui::PopID();
#endif
    // one check box per AUX view; their own ID scope, as a view's child has
    // the label
    int nv;
    const UiView* const* v = views(&nv);
    ImGui::PushID("aux");
    for (int i = 0; i < nv; i++) {
        if (v[i]->slot != UI_SLOT_AUX) continue;
        flow_next(flow_check_w(v[i]->label));
        ImGui::Checkbox(v[i]->label, &S->aux_on[i]);
        ImGui::SetItemTooltip("Show the %s view beside the main view", v[i]->label);
    }
    ImGui::PopID();
    // The path, its field and Open path stay on one row; the field takes
    // what the row leaves, at least kPathMinW, else the three start a row.
    const float sp = ImGui::GetStyle().ItemSpacing.x, open_w = flow_button_w("Open path");
    flow_next(flow_text_w("Path") + sp + kPathMinW + sp + open_w);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Path");
    ImGui::SameLine();
    if (S->focus_path) {
        ImGui::SetKeyboardFocusHere();
        S->focus_path = false;
    }
    ImGui::SetNextItemWidth(-(open_w + sp));
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
        flow_next(flow_check_w(v[i]->label));
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

// An issue's text: one that the rig profile causes names the profile.
const char* issue_text(const dsg_issue* is, char* buf, size_t cap) {
    const char* rig = is->from == DSG_FROM_PLAYER_RUN ? S->run_rig : S->report.rig;
    if (!is->rig) return is->message;
    snprintf(buf, cap, "rig profile '%s': %s", rig[0] ? rig : "?", is->message);
    return buf;
}

// One row of the issues panel; true when it was clicked.
bool issue_row(const dsg_issue* is, bool selected) {
    const float row = kTextSize * 1.5f;
    char place[48];
    if (is->place.line) snprintf(place, sizeof place, "%u:%u", is->place.line, is->place.column);
    else snprintf(place, sizeof place, "-");
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::Selectable("##issue", selected, 0, ImVec2(0, row));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(at.x, at.y + 2), IM_COL32(170, 170, 170, 255), place);
    dl->AddText(ImVec2(at.x + 70, at.y + 2), sev_color(is->severity), sev_name(is));
    // The message names the definition's own things, which can be in any
    // script, so it is laid out by Skribidi.
    char text[2048];
    const skb_layout_t* lay = skt_layout(issue_text(is, text, sizeof text), kTextSize);
    if (lay) {
        dl->PushClipRect(ImVec2(at.x + 150, at.y), ImVec2(at.x + ImGui::GetContentRegionAvail().x + 150, at.y + row), true);
        skt_draw_layout(dl, lay, at.x + 150, at.y + 2, 1.0f, IM_COL32(230, 230, 235, 255));
        dl->PopClipRect();
    }
    return clicked;
}

// The caret to an issue's place. The place is in the text that the check
// (or the session) read: the editor's text, or an older one while a newer
// check runs; dsg_codepoints_before() clamps an offset past the end.
void caret_to_issue(const dsg_issue* is) {
    if (!is->place.line) return;
    size_t n;
    const char* t = def_bytes(&n);
    skf_set_caret(S->editor, (int)dsg_codepoints_before(t, n, is->place.offset));
    S->last_caret = skf_caret(S->editor);
}

// As ui_jump_to_issue() for the preview's issue: it names the definition's
// object, as a tier 2 refusal does, so its path comes from its place.
void jump_to_preview_issue() {
    dsg_issue is = {};
    if (!dsg_preview_issue(S->pv, &is)) return;
    S->selected = kPreviewIssue;
    const yjs_value* root = dsg_doc_root(S->doc);
    char p[DSG_SEL_PATH] = "";
    if (root && is.place.line && dsg_path_at(root, is.place.offset, p, sizeof p) == 0 && p[0]) {
        dsg_sel s = {};
        dsg_sel_def(&s, p);
        ui_reveal(&s);
    }
    caret_to_issue(&is);
}

void issues_panel() {
    if (!S->open) return;
    if (!S->have_report) ImGui::TextUnformatted("Not checked yet.");
    if (!S->have_merged) return;
    const dsg_report* r = &S->mreport;
    if (S->have_report) {
        // the check's own counts: Build's and the run's issues follow them
        const dsg_report* c = &S->report;
        int warn = 0, need = 0;
        for (int i = 0; i < c->n_issues; i++) {
            warn += c->issues[i].severity == DSG_SEV_WARNING;
            need += c->issues[i].severity == DSG_SEV_NEED;
        }
        char rig[DSG_RIG_NAME + 32] = "";
        if (c->rig[0]) snprintf(rig, sizeof rig, " Rig profile: %s.", c->rig);
        if (c->status == DSG_CHECK_OK && c->n_errors == 0)
            ImGui::Text("The player loads this definition. %d warnings; a real run needs %d things.%s", warn, need, rig);
        else
            ImGui::Text("%d errors, %d warnings; a real run needs %d things.%s%s", c->n_errors, warn, need, rig,
                        c->have_pack ? "" : " No check pack yet: the player's own checks wait for one.");
    }
    if (S->build && S->build->report.n_errors > 0) ImGui::Text("The last Build failed: %d errors.", S->build->report.n_errors);
    ImGui::Separator();
    ImGui::BeginChild("##issue_list");
    for (int i = 0; i < r->n_issues; i++) {
        ImGui::PushID(i);
        if (issue_row(&r->issues[i], S->selected == i)) ui_jump_to_issue(i);
        ImGui::PopID();
    }
    // Read here, not kept from the frame's start: a view's control may
    // have changed it since (preview.h, "Pointers").
    dsg_issue pis = {};
    if (dsg_preview_issue(S->pv, &pis)) {
        ImGui::PushID("preview");
        if (issue_row(&pis, S->selected == kPreviewIssue)) jump_to_preview_issue();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// "A run goes" (Quit, Open, New while the player runs): Stop the run and
// go on at its end, leave it running, or cancel ("Quit during a run").
void run_modal() {
    if (S->ask_run) {
        ImGui::OpenPopup("A run goes");
        S->ask_run = false;
    }
    if (ImGui::BeginPopupModal("A run goes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        dsg_run_info in = {};
        if (S->run) dsg_run_get_info(S->run, &in);
        ImGui::Text("The player runs into %s.", in.out_dir ? in.out_dir : "its folder");
        if (ImGui::Button("Stop the run")) {
            ImGui::CloseCurrentPopup();
            ui_run_stop();
            S->wait_run = true;
            set_status("Stopping the run; the designer goes on when it ends. Kill ends it now.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Leave it running")) {
            ImGui::CloseCurrentPopup();
            S->wait_run = false;
            drop_run();
            continue_pending();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
            S->wait_run = false;
            S->pending = PENDING_NONE;
        }
        ImGui::EndPopup();
    }
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
    // The preview's session ops read S->gl on the check thread, so it is
    // filled before the thread starts and never written after.
    if (platform) {
        S->gl.caps = platform->gl_caps;
        S->gl.share = platform->gl_share;
        S->gl.proc = platform->gl_proc;
        S->gl.proc_ctx = platform->gl_proc_ctx;
    }
    dsg_worker_desc wd = {};
    if (platform) {
        wd.wake = platform->wake;
        wd.ctx = platform->ctx;
    }
    dsg_preview_session_ops(&S->gl, &wd.session);
    S->worker = dsg_worker_start(&wd, err, cap);
    if (!S->worker) return false;
    S->doc = dsg_doc_new();
    if (!S->doc || dsg_doc_reset(S->doc, "", "", 0)) {
        snprintf(err, cap, "out of memory for the document");
        return false;
    }
    S->pv = dsg_preview_new(&S->gl, S->worker, S->doc);
    if (!S->pv) {
        snprintf(err, cap, "out of memory for the preview");
        return false;
    }
    // The build thread wakes the loop as the check thread does.
    dsg_builder_desc bd = {};
    bd.wake = wd.wake;
    bd.ctx = wd.ctx;
    S->builder = dsg_builder_start(&bd, err, cap);
    if (!S->builder) return false;
    dsg_run_settings_default(&S->run_set);
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
    // before the worker stops: the preview hands its sessions to it
    dsg_preview_free(S->pv);
    S->pv = nullptr;
    dsg_builder_stop(S->builder);
    S->builder = nullptr;
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
    if (ui_run_active()) {
        ask_or_run(PENDING_QUIT, nullptr);
        return false;
    }
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
    if (!S->have_merged || i < 0 || i >= S->mreport.n_issues) return;
    S->selected = i;
    const dsg_issue* is = &S->mreport.issues[i];
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
    caret_to_issue(is);
}

void ui_frame() {
    S->now = S->plat.now_ns ? S->plat.now_ns(S->plat.ctx) : (int64_t)(ImGui::GetTime() * 1e9);
    // A result that landed since the last frame shows in this one: its
    // issues, before the views mark them.
    take_check();
    take_session();
    take_build();
    tick_run();
    // "Stop the run" of the run's question: go on when it ended
    if (S->wait_run && !run_going()) {
        S->wait_run = false;
        continue_pending();
    }
    // A hidden pane cannot hold the keyboard.
    if (!aux_shown() && dsg_preview_keyboard(S->pv)) dsg_preview_set_keyboard(S->pv, false);
    // Before the views draw: they show this frame's session.
    dsg_preview_tick(S->pv, S->now, aux_shown());
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
    int n_aux = 0;   // the AUX views whose check box is ticked, in the pane
    // the preview out of the pane when it pops out and the platform can
    const bool popped = S->popped && can_pop();
    const int pv_i = aux_index("preview");
    for (int i = 0; i < nv; i++) {
        side = side || v[i]->slot == UI_SLOT_SIDE;
        n_aux += v[i]->slot == UI_SLOT_AUX && S->aux_on[i] && !(popped && i == pv_i);
    }
    const bool aux = n_aux > 0;
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float issues_h = ImMax(120.0f, ImGui::GetContentRegionAvail().y * 0.3f);
    const float side_w = side ? ImMax(280.0f, avail_w * 0.3f) : 0.0f;
    // The AUX pane: its share of what the side column leaves, at least
    // kAuxMinW, and the main view keeps kMainMinW when there is room.
    const float aux_room = avail_w - side_w;
    const float aux_w = aux ? ImMax(kAuxMinW, ImMin(S->aux_share * aux_room, aux_room - kMainMinW)) : 0.0f;
    const float main_w = -((side ? side_w + sp : 0.0f) + (aux ? aux_w + sp : 0.0f));

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
    if (aux) {
        // The splitter fills the spacing between the main view and the pane
        // (2026-11-23): a drag sets the pane's share of the room.
        ImGui::SameLine(0, 0);
        const ImVec2 sp0 = ImGui::GetCursorScreenPos();
        const float split_h = ImMax(ImGui::GetContentRegionAvail().y - issues_h, 1.0f);
        ImGui::InvisibleButton("##aux_split", ImVec2(sp, split_h));
        if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.x != 0 && aux_room > 0) {
            const float w = ImMax(kAuxMinW, ImMin(aux_w - ImGui::GetIO().MouseDelta.x, aux_room - kMainMinW));
            S->aux_share = ImClamp(w / aux_room, kAuxShareMin, kAuxShareMax);
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(sp0.x + sp * 0.5f - 1.0f, sp0.y), ImVec2(sp0.x + sp * 0.5f + 1.0f, sp0.y + split_h),
                                                      ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_SeparatorActive : ImGuiCol_SeparatorHovered));
        }
        ImGui::SetItemTooltip("Drag to change the pane's width");
        // AUX views are children of the main window too: "//##designer/LABEL".
        ImGui::SameLine(0, 0);
        ImGui::BeginGroup();
        const float h = (ImGui::GetContentRegionAvail().y - issues_h - ImGui::GetStyle().ItemSpacing.y * (float)(n_aux - 1)) / (float)n_aux;
        for (int i = 0; i < nv; i++) {
            if (v[i]->slot != UI_SLOT_AUX || !S->aux_on[i] || (popped && i == pv_i)) continue;
            // The preview's scroll bar always shows: a stale banner can make
            // its content overflow, and a scroll bar that came then would
            // narrow the rows, wrap them again and rescale the display (the
            // fixed rows rule; "Rows that wrap").
            ImGui::BeginChild(v[i]->label, ImVec2(aux_w, ImMax(h, 40.0f)), ImGuiChildFlags_Borders,
                              i == pv_i ? ImGuiWindowFlags_AlwaysVerticalScrollbar : 0);
            if (i == pv_i) {
                S->pane_pos = ImGui::GetWindowPos();
                S->pane_size = ImGui::GetWindowSize();
            }
            if (S->open) v[i]->draw();
            else ImGui::TextUnformatted("No project is open.");
            ImGui::EndChild();
        }
        ImGui::EndGroup();
    }
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
    // the Run view chose another rig profile: the check takes it
    if (S->open && strcmp(S->check_rig, run_rig_name()) != 0) S->check_due = true;
    if (S->check_due) post_check();

    ImGui::BeginChild("Issues", ImVec2(0, 0), ImGuiChildFlags_Borders);
    issues_panel();
    ImGui::EndChild();
    run_modal();
    unsaved_modal();
    ImGui::End();
    if (popped && pv_i >= 0 && S->aux_on[pv_i]) {
        popped_window(v[pv_i]);
    } else {
        S->popped_shown = true;
        S->popped_handle = nullptr;
    }
    // after the views: an edit in this frame's draw shows in this title
    refresh_dirty();
    update_title();
}

// A check that runs on the thread is not work for a frame: its wake-up
// (UiPlatform.wake) brings the frame that takes the result.
bool ui_work_pending() {
    return S && (S->check_due || S->restore_scroll >= 0 || S->ask_unsaved || S->ask_run || (S->pv && dsg_preview_work(S->pv)) ||
                 (S->open && strcmp(S->check_rig, run_rig_name()) != 0) ||
                 (S->open && dsg_doc_revision(S->doc) != S->posted) || dsg_doc_rev(S->doc, DSG_BUF_DEF) != S->text_rev ||
                 (S->worker && !dsg_worker_threaded(S->worker) && S->open && S->checked != S->posted));
}

bool ui_animating() {
    if (!S) return false;
    int nv;
    const UiView* const* v = views(&nv);
    for (int i = 0; i < nv; i++) {
        // a hidden view does not animate (the preview pauses there)
        if (v[i]->slot == UI_SLOT_AUX &&
            !(S->open && S->aux_on[i] && (strcmp(v[i]->id, "preview") == 0 ? preview_window_shown() : S->window_shown)))
            continue;
        if (v[i]->animating && v[i]->animating()) return true;
    }
    return false;
}

bool ui_preview_key(int scancode, bool down, bool repeat) {
    if (!S || !S->pv) return false;
    if (repeat) return dsg_preview_keyboard(S->pv);
    return dsg_preview_key(S->pv, scancode, down);
}

bool ui_preview_has_keyboard() { return S && S->pv && dsg_preview_keyboard(S->pv); }

void ui_set_window_shown(bool shown) {
    if (S) S->window_shown = shown;
}

const dsg_project* ui_project() { return S && S->open ? &S->proj : nullptr; }
const dsg_report* ui_report() { return S && S->have_merged ? &S->mreport : nullptr; }
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
    if (!S->have_merged || i < 0 || i >= S->issue_path_at.Size) return "";
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
        if ((i == best || v[i]->slot == UI_SLOT_SIDE || (v[i]->slot == UI_SLOT_AUX && S->aux_on[i])) && v[i]->reveal) v[i]->reveal(s);
}

void ui_set_status(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    set_status_v(fmt, ap);
    va_end(ap);
}

float ui_text_size() { return kTextSize; }

dsg_preview* ui_preview() { return S->pv; }

const dsg_preview_gl* ui_preview_gl() { return &S->gl; }

int64_t ui_now_ns() { return S->now; }

const dsg_preview_where* ui_preview_where() {
    if (!S || !S->pv || !dsg_preview_where_now(S->pv, &S->where)) return nullptr;
    return &S->where;
}

bool ui_aux_open() { return aux_view_on("preview"); }

bool ui_preview_can_pop() { return S && can_pop(); }
bool ui_preview_popped() { return S && S->popped && can_pop(); }

void ui_preview_pop(bool out) {
    if (!S || S->popped == out) return;
    if (!out && S->popped_fs) ui_preview_set_fullscreen(false);
    S->popped = out;
    S->pop_place = out;
    S->popped_fs = false;
}

bool ui_preview_fullscreen() { return S && S->popped && S->popped_fs; }

bool ui_preview_set_fullscreen(bool on) {
    if (!S || !S->popped || !S->popped_handle || !S->plat.fullscreen) return false;
    if (!S->plat.fullscreen(S->plat.ctx, S->popped_handle, on)) return false;
    S->popped_fs = on;
    return true;
}

void ui_show_aux(bool open) { set_aux_view("preview", open); }

bool ui_aux_view_open(const char* id) { return S && aux_view_on(id); }

void ui_show_aux_view(const char* id, bool open) {
    if (S) set_aux_view(id, open);
}

bool ui_open_file(const char* path) {
    int nv;
    const UiView* const* v = views(&nv);
    for (int i = 0; i < nv; i++) {
        if (!v[i]->open_file || !v[i]->open_file(path)) continue;
        if (v[i]->slot == UI_SLOT_MAIN) S->main_view = i;
        else if (v[i]->slot == UI_SLOT_AUX) S->aux_on[i] = true;
        return true;
    }
    return false;
}

// --- step 6: Build and Run ----------------------------------------------------

bool ui_run_active() { return S && run_going(); }

int ui_run_wait_ms() {
    if (!S || !S->run || dsg_atomic_load(&g_run_woken) == S->run_seen) return -1;
    const int64_t due = S->run_frame_ns + 1000000000LL / DSG_RUN_REDRAW_HZ;
    const int64_t now = clock_now();
    return now >= due ? 0 : (int)((due - now + 999999) / 1000000);
}

bool ui_wait_builds(int64_t timeout_ns) { return S && S->builder && dsg_build_wait(S->builder, timeout_ns) == 0; }

bool ui_can_build() { return S && build_refusal() == nullptr; }

bool ui_build() {
    const char* why = build_refusal();
    if (why) {
        set_status("Not built: %s", why);
        return false;
    }
    // Build builds the files (decision 2): the edits go to them first
    refresh_dirty();
    if (S->dirty && !ui_save()) return false;
    char out[DSG_PATH_MAX], err[1024] = "";
    if (dsg_build_path(&S->proj, out, sizeof out)) {
        set_status("Not built: the pack's path is too long.");
        return false;
    }
    if (dsg_build_post(S->builder, &S->proj, out, dsg_doc_revision(S->doc), err, sizeof err)) {
        set_status("Not built: %s", err);
        return false;
    }
    set_status("Building %s ...", out);
    return true;
}

bool ui_build_busy() { return S && S->builder && dsg_build_busy(S->builder); }

const dsg_build_result* ui_build_last() { return S ? S->build : nullptr; }

bool ui_build_stale() { return !S || !S->open || dsg_build_stale(&S->proj, S->build, dsg_doc_revision(S->doc)); }

bool ui_can_run() { return S && run_refusal() == nullptr; }

bool ui_run_start() {
    const char* why = run_refusal();
    if (why) {
        set_status("Not run: %s", why);
        return false;
    }
    if (!S->plat.player_path || !S->plat.player_path[0]) {
        set_status("Not run: there is no ysp_player beside the designer.");
        return false;
    }
    refresh_dirty();
    if (!S->dirty && !ui_build_stale()) return start_run();
    // Run builds first when the pack is stale; the run starts at the result
    if (!ui_build()) return false;
    S->run_after_build = true;
    return true;
}

int ui_run_stop() {
    if (!S || !run_going()) return 0;
    const int r = dsg_run_stop(S->run, S->now);
    set_status(r ? "Stop sent: the player closes its data set and ends." : "Stop did not reach the player: Kill ends it.");
    return r;
}

void ui_run_kill() {
    if (!S || !run_going()) return;
    dsg_run_kill(S->run);
    set_status("Killed: the data set is not closed, and the display's mode and gamma ramp may stay changed.");
}

const dsg_run* ui_run() { return S ? S->run : nullptr; }

bool ui_run_next_out_dir(char* out, size_t cap) {
    char why[DSG_PATH_MAX + 64];
    if (!S || !S->open) return false;
    if (S->run_set.out_dir[0]) return snprintf(out, cap, "%s", S->run_set.out_dir) < (int)cap;
    return next_out_dir((int64_t)time(nullptr), out, cap, why, sizeof why) == 0;
}

dsg_run_settings* ui_run_settings() { return &S->run_set; }

void ui_check_again() {
    if (S) S->check_due = true;
}
