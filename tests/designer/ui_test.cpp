// ui_test.cpp - the designer's UI (designer/app/ui.cpp, skf.cpp, skt.cpp)
// with no window and no GL: Dear ImGui's frames driven with made-up input,
// its textures accepted without a renderer (docs/designer.md, step 3).
//
// Usage: test_designer_ui WORK
//   1. No folder dialog: the status points at the path field, which takes
//      the keyboard; a typed path and Enter open the project.
//   2. Open: the editor holds the project's text (version 1 shown migrated,
//      so dirty), and one check ran.
//   3. Typing: a character typed into the editor changes the text, runs a
//      check, and its error's place is where the character went; a jump to
//      the issue puts the caret there, past Arabic text before it (bytes are
//      not codepoints). Enter starts a line. Ctrl+Z undoes.
//   4. Unsaved changes: opening another project asks first and keeps the text.
//   5. Ctrl+S: canonical bytes on disk, clean, the view file written with
//      the caret's line.
//   6. Idle: no work pending, so the loop would block.
//   7. The document (step 4): a structured edit shows in the text view; each
//      main view draws; Ctrl+Z with the chart shown takes the edit back, and
//      Ctrl+Shift+Z puts it in again.
//   8. The AUX pane (step 5, part S): hidden at first; shown, its view's
//      child draws beside the main view and the side column; hidden, it does
//      not. Its state and the preview's settings round-trip through the
//      view file's part "preview"; a project without the part gets the
//      defaults, and so does a part with values out of range. The keyboard:
//      no key is taken until the pane claims it, Escape gives it back, and
//      hiding the pane drops the claim.
//   9. The preview's pane (step 5, part B), on the test double of preview.h
//      (preview_double.h) with no GL caps: after a check and the settle a
//      session runs; the pane draws a gray box, never the texture; the
//      banner says faithful in logic, not in timing, at the session's rate;
//      the view animates while the session runs and not while it is paused
//      or the pane is hidden; a text that does not parse keeps the session
//      and says since when; the roles not driven are listed; the outputs
//      and the data set's folder show.
//  10. The display with GL caps (a fake caps pointer: the double never reads
//      it): at 1:1 the texture is drawn at its size in px, rows flipped,
//      with the nearest sampler around it; at Fit it is scaled down with no
//      sampler change and says that fine patterns alias. The cursor over it
//      reaches the preview in participant px, and leaving it says so.
// Exit 0 on pass, 1 on a failure.
#include "ui.h"
#include "views.h"
#include "skf.h"
#include "skt.h"
#include "preview_double.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef YSP_SOURCE_DIR
#define YSP_SOURCE_DIR "."
#endif
#ifndef YSP_DESIGNER_FONT_DIR
#define YSP_DESIGNER_FONT_DIR "fonts"
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int titles;
static char title[DSG_PATH_MAX + 64];
static bool dialog_offered;

static bool no_dialog(void*) { dialog_offered = true; return false; }
static void on_title(void*, const char* t) { titles++; snprintf(title, sizeof title, "%s", t); }

// One frame as main.cpp draws it, with the renderer's part for textures.
static void frame() {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
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
}

// One frame with ImGui's log on: every text the frame draws, ImGui's own and
// what the views log (the pane logs its user text), lands in g_log.
static char g_log[1 << 16];
static void frame_log() {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ui_wait_checks(30000000000LL);
    ImGui::NewFrame();
    ImGui::LogToBuffer();
    skt_frame_begin();
    ui_frame();
    skt_frame_end();
    ImGuiContext& g = *ImGui::GetCurrentContext();
    snprintf(g_log, sizeof g_log, "%s", g.LogBuffer.c_str());
    ImGui::LogFinish();
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
}

static void frames(int n) {
    for (int i = 0; i < n; i++) frame();
}

// Frames until ImGui has applied every queued event, as the loop does.
static void settle() {
    for (int i = 0; i < 64; i++) {
        frame();
        if (ImGui::GetCurrentContext()->InputEventsQueue.Size == 0 && i >= 2) break;
    }
}

static void key(ImGuiKey k, bool ctrl = false) {
    ImGuiIO& io = ImGui::GetIO();
    if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(k, true);
    io.AddKeyEvent(k, false);
    if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
    settle();
}

static void type(const char* utf8) {
    ImGui::GetIO().AddInputCharactersUTF8(utf8);
    settle();
}

static bool copy_file(const char* from, const char* to) {
    char* d;
    size_t n;
    char err[512];
    if (dsg_read_file(from, &d, &n, err, sizeof err)) return false;
    bool ok = dsg_write_file(to, d, n, err, sizeof err) == 0;
    free(d);
    return ok;
}

static bool project(const char* work, const char* name, char* out, size_t cap) {
    static const char* const files[] = { "pack.json", "experiment.json", "conditions.csv" };
    char err[512], a[DSG_PATH_MAX], b[DSG_PATH_MAX], src[DSG_PATH_MAX];
    if (dsg_join(out, cap, work, name) || dsg_make_dir(out, err, sizeof err)) return false;
    if (dsg_join(src, sizeof src, YSP_SOURCE_DIR, "tests/player/rdk")) return false;
    for (const char* f : files)
        if (dsg_join(a, sizeof a, src, f) || dsg_join(b, sizeof b, out, f) || !copy_file(a, b)) return false;
    if (dsg_join(b, sizeof b, out, DSG_VIEW_FILE) == 0) remove(b);
    return true;
}

// The child window that ui.cpp makes for a view, by its label, if it drew
// in the last frame.
static bool child_drawn(const char* label) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* top = ImGui::FindWindowByName("##designer");
    if (!top) return false;
    const ImGuiID id = ImHashStr(label, 0, top->ID);
    for (ImGuiWindow* w : g.Windows)
        if (w->ParentWindow == top && w->ChildId == id) return w->Active;
    return false;
}

// The preview pane's child window, or a child of it whose name has `name`.
static ImGuiWindow* pane_window(const char* name) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* top = ImGui::FindWindowByName("##designer");
    if (!top) return nullptr;
    const ImGuiID id = ImHashStr("Preview", 0, top->ID);
    ImGuiWindow* pane = nullptr;
    for (ImGuiWindow* w : g.Windows)
        if (w->ParentWindow == top && w->ChildId == id) pane = w;
    if (!pane || !name) return pane;
    for (ImGuiWindow* w : g.Windows)
        if (w->ParentWindow == pane && strstr(w->Name, name)) return w;
    return nullptr;
}

static dsg_preview_info pv_info() {
    dsg_preview_info in = {};
    dsg_preview_get_info(ui_preview(), &in);
    return in;
}

// Stand-ins for the GL backend's sampler callbacks: the draw list keeps
// their addresses, so the test can see where they go.
static void cb_nearest(const ImDrawList*, const ImDrawCmd*) {}
static void cb_linear(const ImDrawList*, const ImDrawCmd*) {}

// A draw command of the display's draw list with this texture: its index,
// and the box and the top-left corner's uv of its vertices.
static int find_image(ImDrawList* dl, ImTextureID tex, ImVec2* mn, ImVec2* mx, ImVec2* uv_tl) {
    for (int c = 0; c < dl->CmdBuffer.Size; c++) {
        const ImDrawCmd& cmd = dl->CmdBuffer[c];
        if (cmd.UserCallback || cmd.GetTexID() != tex || cmd.ElemCount == 0) continue;
        *mn = ImVec2(FLT_MAX, FLT_MAX);
        *mx = ImVec2(-FLT_MAX, -FLT_MAX);
        for (unsigned e = 0; e < cmd.ElemCount; e++) {
            const ImDrawVert& v = dl->VtxBuffer[(int)(cmd.VtxOffset + dl->IdxBuffer[(int)(cmd.IdxOffset + e)])];
            if (v.pos.x <= mn->x && v.pos.y <= mn->y) *uv_tl = v.uv;
            *mn = ImMin(*mn, v.pos);
            *mx = ImMax(*mx, v.pos);
        }
        return c;
    }
    return -1;
}

static bool has_color(ImDrawList* dl, ImU32 col) {
    for (const ImDrawVert& v : dl->VtxBuffer)
        if (v.col == col) return true;
    return false;
}

static bool same_settings(const dsg_preview_settings& a, const dsg_preview_settings& b) {
    const dsg_session_settings& x = a.session;
    const dsg_session_settings& y = b.session;
    if (x.width != y.width || x.height != y.height || x.hz != y.hz || x.seed != y.seed || strcmp(x.participant, y.participant) != 0 ||
        x.rt_s != y.rt_s || strcmp(x.pin_leaf, y.pin_leaf) != 0 || x.pin_row != y.pin_row || x.n_fields != y.n_fields ||
        a.speed != b.speed || a.scale != b.scale)
        return false;
    for (int i = 0; i < x.n_fields; i++) {
        // the file keeps fields sorted by name, so look each one up
        bool found = false;
        for (int j = 0; j < y.n_fields && !found; j++)
            found = strcmp(x.fields[i].name, y.fields[j].name) == 0 && strcmp(x.fields[i].value, y.fields[j].value) == 0;
        if (!found) return false;
    }
    return true;
}

static const char* editor_text(size_t* n) {
    static ImVector<char> buf;
    *n = (size_t)skf_text(ui_editor(), &buf);
    return buf.Data;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: test_designer_ui WORK\n");
        return 2;
    }
    const char* work = argv[1];
    char err[512], dir[DSG_PATH_MAX], dir2[DSG_PATH_MAX];
    if (dsg_make_dir(work, err, sizeof err) || !project(work, "rdk", dir, sizeof dir) || !project(work, "rdk2", dir2, sizeof dir2)) {
        printf("FAIL: the work folder: %s\n", err);
        return 1;
    }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1400, 900);
    io.ConfigInputTextCursorBlink = false;
    // On macOS AddKeyEvent() swaps Ctrl and Cmd, so the made-up Ctrl+Z and
    // Ctrl+S below would not set io.KeyCtrl there. One key map on every OS.
    io.ConfigMacOSXBehaviors = false;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    UiPlatform plat = { nullptr, no_dialog, on_title, nullptr };
    if (!ui_init(YSP_DESIGNER_FONT_DIR, &plat, err, sizeof err)) {
        printf("FAIL: ui_init: %s\n", err);
        return 1;
    }
    settle();
    CHECK(ui_project() == nullptr && strstr(ui_status(), "Open a project"), "no project at start: '%s'", ui_status());

    // 1. the dialog fails; the typed path opens the project
    ui_dialog_failed("test");
    settle();
    CHECK(strstr(ui_status(), "path field") != nullptr, "the status points at the path field: '%s'", ui_status());
    CHECK(ImGui::GetIO().WantTextInput, "the path field has the keyboard");
    type(dir);
    key(ImGuiKey_Enter);
    const dsg_project* p = ui_project();
    CHECK(p && strcmp(p->folder, dir) == 0, "Enter in the path field opened %s: '%s'", dir, ui_status());
    if (!p) {
        printf("FAIL: %d checks, %d failed (no project, stopping)\n", checks, fails + 1);
        return 1;
    }

    // 2. the editor holds the text; version 1 shows migrated, so dirty
    size_t n;
    const char* t = editor_text(&n);
    CHECK(n == p->text_len && memcmp(t, p->text, n) == 0, "the editor holds the project's text (%zu of %zu bytes)", n, p->text_len);
    size_t un;
    const char* ut = ui_text(&un);
    CHECK(un == n && memcmp(ut, t, n) == 0, "ui_text() is the editor's");
    CHECK(p->migrated && ui_dirty() && title[0] == '*', "migrated, dirty, and the title says so: '%s'", title);
    CHECK(ui_checks_run() == 1 && ui_report() && ui_report()->status == DSG_CHECK_OK && ui_report()->n_errors == 0,
          "one check ran, clean (%d checks)", ui_checks_run());

    // 3. Arabic in a note, then an error typed after it
    const char* at = strstr(t, "\"format\"");
    CHECK(at != nullptr, "the format key");
    int checks0 = ui_checks_run();
    skf_set_caret(ui_editor(), (int)dsg_codepoints_before(t, n, (uint32_t)(at - t)));
    settle();
    type("\"note\": \"\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7\", ");
    t = editor_text(&n);
    CHECK(strstr(t, "\"note\": \"\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7\", \"format\"") != nullptr, "the note went in before format");
    CHECK(ui_checks_run() > checks0 && ui_report()->n_errors == 0, "each change checks, and a note is valid (%d errors)",
          ui_report()->n_errors);
    const char* st = strstr(t, "\"stimuli\"");
    CHECK(st != nullptr, "the stimuli key");
    skf_set_caret(ui_editor(), (int)dsg_codepoints_before(t, n, (uint32_t)(st - t)));
    settle();
    type("x");
    t = editor_text(&n);
    const dsg_report* r = ui_report();
    CHECK(r->n_errors > 0 && r->issues[0].place.line > 0, "a stray x is an error with a place");
    if (r->n_errors > 0) {
        const uint32_t off = r->issues[0].place.offset;
        CHECK(off < n && t[off] == 'x', "the error is at the x (offset %u, '%.10s')", off, off < n ? t + off : "");
        skf_set_caret(ui_editor(), 0);
        settle();
        ui_jump_to_issue(0);
        settle();
        const int cp = skf_caret(ui_editor());
        const uint32_t b = dsg_byte_of_codepoint(t, n, (uint32_t)cp);
        CHECK(cp == (int)dsg_codepoints_before(t, n, off) && b == off, "the jump puts the caret at the x: codepoint %d, byte %u", cp, b);
        CHECK(cp < (int)off, "after Arabic, the codepoint offset (%d) is below the byte offset (%u)", cp, off);
    }
    key(ImGuiKey_Z, true);
    t = editor_text(&n);
    CHECK(!strstr(t, "x\"stimuli\"") && ui_report()->n_errors == 0, "Ctrl+Z took the x out and the check is clean again");
    key(ImGuiKey_Enter);
    t = editor_text(&n);
    CHECK(strstr(t, "\n\"stimuli\"") != nullptr, "Enter started a line before stimuli");

    // 4. another project with unsaved changes: asked, not switched
    ui_open(dir2);
    settle();
    CHECK(ui_project() && strcmp(ui_project()->folder, dir) == 0, "the open waits for the answer");
    CHECK(ImGui::GetTopMostPopupModal() != nullptr, "the unsaved-changes question is open");
    if (ImGui::GetTopMostPopupModal()) ImGui::ClosePopupToLevel(0, false);   // Cancel
    settle();

    // 5. Ctrl+S: canonical, clean, view written
    char* disk;
    size_t dn;
    key(ImGuiKey_S, true);
    CHECK(!ui_dirty() && strstr(ui_status(), "canonical") != nullptr, "Ctrl+S saved: '%s'", ui_status());
    CHECK(dsg_read_file(p->def_path, &disk, &dn, err, sizeof err) == 0, "read the saved file");
    t = editor_text(&n);
    CHECK(dn == n && memcmp(disk, t, n) == 0 && title[0] != '*', "the file is the editor's text, and the title is clean");
    CHECK(strstr(disk, "\"note\": \"\xD9\x85") != nullptr && strstr(disk, "\"version\": 2") != nullptr, "with the note, version 2");
    free(disk);
    dsg_view v;
    CHECK(dsg_view_read(dir, &v, err, sizeof err) == 0 && v.caret_line > 1, "the view file has the caret's line (%u)", v.caret_line);
    dsg_view_free(&v);

    // 6. idle
    settle();
    CHECK(!ui_work_pending(), "idle: nothing pending");
    int c1 = ui_checks_run();
    settle();
    CHECK(ui_checks_run() == c1, "idle frames run no check");

    // 7. the document under every view
    CHECK(dsg_def_set_string(ui_doc(), "/name", "views") == 0, "a structured edit: %s", dsg_doc_error(ui_doc()));
    settle();
    t = editor_text(&n);
    CHECK(strstr(t, "\"name\": \"views\"") != nullptr && ui_dirty(), "the text view shows it, dirty");
    CHECK(ui_checks_run() > c1, "the edit ran a check");
    static const char* const ids[] = { "chart", "tables", "text" };
    for (const char* id : ids) {
        CHECK(ui_show_view(id), "the %s view", id);
        settle();
        CHECK(strcmp(ui_main_view(), id) == 0, "%s shows", id);
    }
    CHECK(!ui_show_view("none"), "no such view");
    ui_show_view("chart");
    settle();
    key(ImGuiKey_Z, true);
    t = editor_text(&n);
    CHECK(strstr(t, "\"name\": \"views\"") == nullptr, "Ctrl+Z with the chart shown takes it back");
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
    key(ImGuiKey_Z, true);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
    settle();
    t = editor_text(&n);
    CHECK(strstr(t, "\"name\": \"views\"") != nullptr, "Ctrl+Shift+Z puts it in again");
    ui_show_view("text");
    settle();

    // 8. the AUX pane and the view file's part "preview"
    CHECK(!ui_aux_open() && !child_drawn("Preview"), "the preview pane is hidden at first");
    // A hidden pane posts no session job, so no session yet.
    CHECK(ui_preview_where() == nullptr, "no session: nowhere");
    CHECK(child_drawn("Definition") && child_drawn("Properties"), "the text view and the side column draw");
    ui_show_aux(true);
    settle();
    CHECK(ui_aux_open() && child_drawn("Preview"), "shown, the pane draws");
    CHECK(child_drawn("Definition") && child_drawn("Properties"), "beside the main view and the side column");
    ui_show_view("chart");
    settle();
    CHECK(child_drawn("Chart") && child_drawn("Preview"), "and beside the chart");
    ui_show_view("text");
    ui_show_aux(false);
    settle();
    CHECK(!child_drawn("Preview") && child_drawn("Definition"), "hidden, it does not draw");

    dsg_preview_settings ps = {}, got = {};
    dsg_preview_settings_default(&ps);
    dsg_preview_get(ui_preview(), &got);
    CHECK(same_settings(ps, got), "a project with no part has the default settings");
    ps.session.width = 800;
    ps.session.height = 600;
    ps.session.hz = 120;
    ps.session.seed = 9223372036854775813ull;   // above INT64_MAX: kept as text
    snprintf(ps.session.participant, sizeof ps.session.participant, "p=0.8");
    ps.session.rt_s = 0.45;
    snprintf(ps.session.pin_leaf, sizeof ps.session.pin_leaf, "/timeline/0");
    ps.session.pin_row = 3;
    ps.session.n_fields = 2;
    snprintf(ps.session.fields[0].name, sizeof ps.session.fields[0].name, "subject");
    snprintf(ps.session.fields[0].value, sizeof ps.session.fields[0].value, "s01");
    snprintf(ps.session.fields[1].name, sizeof ps.session.fields[1].name, "age");
    snprintf(ps.session.fields[1].value, sizeof ps.session.fields[1].value, "42");
    ps.speed = 0.5;
    ps.scale = DSG_PV_SCALE_1TO1;
    dsg_preview_set(ui_preview(), &ps);
    ui_show_aux(true);
    settle();
    CHECK(ui_save(), "saved with the pane open: '%s'", ui_status());
    CHECK(dsg_view_read(dir, &v, err, sizeof err) == 0, "the view file reads: %s", err);
    bool open = false;
    dsg_preview_part_read(&v, &got, &open);
    CHECK(open && same_settings(ps, got), "the view file's part holds the pane and the settings");
    size_t pn = 0;
    const char* part = dsg_view_part_get(&v, "preview", &pn);
    CHECK(part && strstr(part, "\"seed\":9223372036854775813") && strstr(part, "\"scale\":\"1:1\""), "the part's JSON: %.*s", (int)pn,
          part ? part : "");
    dsg_view_free(&v);

    // another project: no part, so the defaults
    ui_open(dir2);
    settle();
    CHECK(ui_project() && strcmp(ui_project()->folder, dir2) == 0, "the other project opened");
    dsg_preview_settings def = {};
    dsg_preview_settings_default(&def);
    dsg_preview_get(ui_preview(), &got);
    CHECK(!ui_aux_open() && same_settings(def, got) && !child_drawn("Preview"), "the other project: pane hidden, defaults");
    // and back: the pane and the settings as they were (saved first: the
    // other project is version 1, shown migrated, so the open would ask)
    CHECK(ui_save(), "the other project saved: '%s'", ui_status());
    ui_open(dir);
    settle();
    dsg_preview_get(ui_preview(), &got);
    CHECK(ui_aux_open() && same_settings(ps, got) && child_drawn("Preview"), "reopened: the pane shows, with the settings");

    // a part written by hand with values out of range: each takes its default
    {
        dsg_view hv;
        memset(&hv, 0, sizeof hv);
        static const char bad[] = "{\"open\":1,\"display\":[0,600],\"hz\":5000,\"speed\":3,\"seed\":-4,\"participant\":7,"
                                  "\"rt_s\":-1,\"pin\":{\"leaf\":5,\"row\":2},\"fields\":{\"a\":1},\"scale\":\"2:1\"}";
        CHECK(dsg_view_part_set(&hv, "preview", bad, sizeof bad - 1) == 0, "a part by hand");
        dsg_preview_part_read(&hv, &got, &open);
        CHECK(!open && same_settings(def, got), "out of range: the defaults");
        static const char junk[] = "{\"display\":[1024,768]";
        dsg_view_part_set(&hv, "preview", nullptr, 0);
        CHECK(dsg_view_part_set(&hv, "preview", junk, sizeof junk - 1) == -1, "the view file keeps no part that does not parse");
        dsg_preview_part_read(&hv, &got, &open);
        CHECK(!open && same_settings(def, got), "no part: the defaults");
        // a row with no leaf is no pin: the file says "any"
        dsg_preview_settings np = def;
        np.session.pin_row = 4;
        size_t hn = 0;
        CHECK(dsg_preview_part_write(&hv, &np, false) == 0, "the part written");
        const char* hj = dsg_view_part_get(&hv, "preview", &hn);
        CHECK(hj && strstr(hj, "\"pin\":{\"leaf\":\"\",\"row\":-1}"), "a row with no leaf is written as any: %.*s", (int)hn, hj ? hj : "");
        dsg_view_free(&hv);
    }

    // the keyboard: taken only while the pane claims it
    CHECK(!ui_preview_key(4, true, false) && !ui_preview_has_keyboard(), "no claim: a key is the designer's");
    dsg_preview_set_keyboard(ui_preview(), true);
    CHECK(ui_preview_has_keyboard() && ui_preview_key(4, true, false) && ui_preview_key(4, true, true) && ui_preview_key(4, false, false),
          "claimed: a key, its repeat and its release are the preview's");
    CHECK(ui_preview_key(DSG_SCANCODE_ESCAPE, true, false) && !ui_preview_has_keyboard(), "Escape gives the keyboard back");
    CHECK(!ui_preview_key(4, true, false), "and then a key is the designer's again");
    dsg_preview_set_keyboard(ui_preview(), true);
    ui_show_aux(false);
    settle();
    CHECK(!ui_preview_has_keyboard(), "hiding the pane drops the claim");
    // The pane was shown with an accepted text, so the real preview loaded a
    // session (the double, off, never does). The place goes with the shown
    // session: there is one exactly when a data set shows.
    {
        dsg_preview_info pi = {};
        dsg_preview_get_info(ui_preview(), &pi);
        const bool shown = pi.out_dir && pi.out_dir[0];
        CHECK((ui_preview_where() != nullptr) == shown, "a place only with a session shown (%s)", shown ? "shown" : "none");
#if !DSG_PREVIEW_DOUBLE
        CHECK(shown, "the real preview loaded a session while the pane showed");
#endif
    }

    // 9. the pane on the double, with no GL caps (the headless case)
#if DSG_PREVIEW_DOUBLE
    {
        pvd_enable(true);
        static const char* const choices[] = { "left", "right" };
        static const char* const roles[] = { "hand_rel", "grip" };
        static const char* const whys[] = { "relative position: no mouse capture", "scalar" };
        pvd_set_choices(2, choices);
        pvd_set_undriven(2, roles, whys);
        pvd_set_output_role("trig");
        pvd_set_texture(77);   // a texture the pane must not draw without caps
        dsg_preview_settings d = {};
        dsg_preview_settings_default(&d);
        d.session.hz = 75;
        dsg_preview_set(ui_preview(), &d);
        ui_show_aux(true);
        // the double learns of checks from now on: an edit gives one
        CHECK(dsg_def_set_string(ui_doc(), "/name", "pane") == 0, "an edit to check: %s", dsg_doc_error(ui_doc()));
        frames(4);
        CHECK(pv_info().status == DSG_PV_NONE && pvd_posts() == 0, "no session job before the settle (%d posts)", pvd_posts());
        frames(30);   // 0.5 s: the settle, the job, its adoption and the attach
        dsg_preview_info in = pv_info();
        CHECK(pvd_posts() == 1 && in.status == DSG_PV_RUNNING, "after the settle one job ran and the session runs (%d posts, status %d)",
              pvd_posts(), in.status);
        CHECK(child_drawn("Preview") && pane_window("##display") != nullptr, "the pane and its display draw");
        CHECK(ui_animating(), "running: the view animates");
        frame_log();
        CHECK(strstr(g_log, "Running.") != nullptr, "the status says running");
        CHECK(strstr(g_log, "Faithful in logic, not in timing: a simulated 75 Hz display paced by this window; input times are frame "
                            "times; no sound.") != nullptr,
              "the banner, at the session's rate");
        CHECK(strstr(g_log, "Shows the definition as of") == nullptr, "no stale banner while the text is accepted");
        CHECK(strstr(g_log, "hand_rel") && strstr(g_log, "not driven: relative position: no mouse capture") && strstr(g_log, "grip") &&
                  strstr(g_log, "not driven: scalar"),
              "the roles not driven are listed, each with why");
        CHECK(strstr(g_log, "left") && strstr(g_log, "right") && strstr(g_log, "Choices:"), "the choice strip");
        CHECK(strstr(g_log, "Click the display to send it keys.") != nullptr, "how to give the session keys");
        CHECK(strstr(g_log, "/.designer/preview/run-1") != nullptr && strstr(g_log, "Open trials.csv") != nullptr,
              "the data set's folder and its button");
        ImGuiWindow* disp = pane_window("##display");
        ImVec2 mn, mx, uv;
        CHECK(disp && find_image(disp->DrawList, (ImTextureID)77, &mn, &mx, &uv) < 0, "no GL caps: the texture is not drawn");
        // the gray box: kNoImage in view_preview.cpp
        CHECK(disp && has_color(disp->DrawList, IM_COL32(96, 96, 96, 255)), "a gray box in its place");
        CHECK(strstr(g_log, "No image: the session draws with no GL.") != nullptr, "and it says why");

        // a choice: a response, a code with its role in the outputs
        dsg_preview_choice(ui_preview(), 1, true);
        dsg_preview_choice(ui_preview(), 1, false);
        frame_log();
        CHECK(strstr(g_log, "No codes sent yet.") == nullptr && strstr(g_log, "trig") && strstr(g_log, "102"),
              "the outputs list the code with its role");
        CHECK(strstr(g_log, "Time (s)") && strstr(g_log, "Role") && strstr(g_log, "Code"), "the outputs' columns");

        dsg_preview_pause(ui_preview());
        frame_log();
        CHECK(pv_info().status == DSG_PV_PAUSED && !ui_animating(), "paused: the view does not animate");
        CHECK(strstr(g_log, "Paused.") != nullptr, "the status says paused");
        const int steps = pvd_steps();
        frames(10);
        CHECK(pvd_steps() == steps, "paused: no frame steps (%d)", pvd_steps() - steps);
        dsg_preview_run(ui_preview());
        frame();
        CHECK(ui_animating(), "run again: it animates");
        ui_show_aux(false);
        frame();
        CHECK(!ui_animating() && pv_info().status == DSG_PV_PAUSED, "a hidden pane pauses the session and does not animate");
        ui_show_aux(true);
        dsg_preview_run(ui_preview());
        frame();
        CHECK(ui_animating(), "shown and run: it animates");

        // a text that does not parse: the session stays, the banner says since when
        const int adopted = pvd_adopted();
        CHECK(dsg_doc_splice(ui_doc(), DSG_BUF_DEF, 0, 0, "x", 1) == 0, "a stray byte");
        frames(30);
        in = pv_info();
        CHECK(in.stale && in.stale_errors > 0 && in.status == DSG_PV_RUNNING && pvd_adopted() == adopted,
              "refused: the session stays and is stale (%d errors)", in.stale_errors);
        frame_log();
        char want[64];
        snprintf(want, sizeof want, "the text has %d %s.", in.stale_errors, in.stale_errors == 1 ? "error" : "errors");
        const char* sb = strstr(g_log, "Shows the definition as of ");
        CHECK(sb && sb[27 + 2] == ':' && sb[27 + 5] == ':' && strstr(sb, want), "the stale banner: '%.80s'", sb ? sb : "");
        dsg_doc_undo(ui_doc());
        frames(30);
        in = pv_info();
        CHECK(!in.stale && pvd_adopted() == adopted + 1, "accepted again: a new session after the settle (%d)", pvd_adopted() - adopted);

        // the end and a failure say so
        pvd_end();
        frame_log();
        CHECK(pv_info().status == DSG_PV_ENDED && !ui_animating() && strstr(g_log, "Session over: "), "ended: '%s'",
              strstr(g_log, "Session over") ? "Session over" : "none");
        pvd_fail("a refusal at run time");
        dsg_preview_restart(ui_preview());
        frames(3);
        pvd_fail("a refusal at run time");
        frame_log();
        CHECK(pv_info().status == DSG_PV_FAILED && strstr(g_log, "Failed: the Issues panel says why.") &&
                  strstr(g_log, "a refusal at run time"),
              "failed: the pane and the Issues panel say so");
    }

    // 10. the display with GL caps
    {
        ui_shutdown();
        static const unsigned char fake_caps = 0;   // never read: the double loads nothing
        UiPlatform gp = plat;
        gp.gl_caps = (const struct ygfx_gl_caps*)(const void*)&fake_caps;
        CHECK(ui_init(YSP_DESIGNER_FONT_DIR, &gp, err, sizeof err), "ui_init with caps: %s", err);
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        pio.DrawCallback_SetSamplerNearest = cb_nearest;
        pio.DrawCallback_SetSamplerLinear = cb_linear;
        pvd_enable(true);
        pvd_set_texture(77);
        ui_open(dir);
        settle();
        ui_show_aux(true);
        dsg_preview_settings one = {};
        dsg_preview_settings_default(&one);
        one.session.width = 800;
        one.session.height = 600;
        one.scale = DSG_PV_SCALE_1TO1;
        dsg_preview_set(ui_preview(), &one);
        CHECK(dsg_def_set_string(ui_doc(), "/name", "caps") == 0, "an edit to check");
        frames(40);
        dsg_preview_settings got1 = {};
        dsg_preview_get(ui_preview(), &got1);
        CHECK(pv_info().status == DSG_PV_RUNNING && got1.scale == DSG_PV_SCALE_1TO1 && got1.session.width == 800,
              "a session at 1:1, 800 x 600 (status %d)", pv_info().status);
        frame_log();
        ImGuiWindow* disp = pane_window("##display");
        ImVec2 mn, mx, uv;
        const int c = disp ? find_image(disp->DrawList, (ImTextureID)77, &mn, &mx, &uv) : -1;
        CHECK(c >= 0, "with caps the texture is drawn");
        if (c >= 0) {
            ImVector<ImDrawCmd>& cb = disp->DrawList->CmdBuffer;
            CHECK(mx.x - mn.x == 800.0f && mx.y - mn.y == 600.0f, "1:1: 800 x 600 px (%.2f x %.2f)", mx.x - mn.x, mx.y - mn.y);
            CHECK(mn.x == floorf(mn.x) && mn.y == floorf(mn.y), "at a whole px (%.2f, %.2f)", mn.x, mn.y);
            CHECK(uv.x == 0.0f && uv.y == 1.0f, "rows bottom up: the top-left corner's uv is (0, 1): (%.1f, %.1f)", uv.x, uv.y);
            CHECK(c > 0 && cb[c - 1].UserCallback == cb_nearest, "the nearest sampler before the image");
            CHECK(c + 1 < cb.Size && cb[c + 1].UserCallback == cb_linear, "and ImGui's linear one after it");
            CHECK(strstr(g_log, "fine patterns alias") == nullptr, "1:1 does not say scaled");

            // the cursor in participant px
            const int calls = pvd_pointer_calls();
            ImGui::GetIO().AddMousePosEvent(mn.x + 100, mn.y + 50);
            frame();
            frame();
            float x = 0, y = 0;
            bool inside = false;
            pvd_pointer(&x, &y, &inside);
            CHECK(inside && x == 100.0f && y == 50.0f, "1:1: the cursor at (100, 50) px: (%.2f, %.2f)", x, y);
            CHECK(pvd_pointer_calls() == calls + 1, "a still cursor sends nothing more (%d calls)", pvd_pointer_calls() - calls);
            ImGui::GetIO().AddMousePosEvent(mn.x - 40, mn.y - 40);
            frame();
            pvd_pointer(&x, &y, &inside);
            CHECK(!inside, "the cursor left the display");
        }
        // Fit: scaled down to the pane, ImGui's own sampler, and it says so
        dsg_preview_settings fit = got1;
        fit.scale = DSG_PV_SCALE_FIT;
        dsg_preview_set(ui_preview(), &fit);
        frame();
        frame_log();
        disp = pane_window("##display");
        const int f = disp ? find_image(disp->DrawList, (ImTextureID)77, &mn, &mx, &uv) : -1;
        CHECK(f >= 0, "Fit: drawn");
        if (f >= 0) {
            ImVector<ImDrawCmd>& cb = disp->DrawList->CmdBuffer;
            const float w = mx.x - mn.x, h = mx.y - mn.y;
            CHECK(w < 800.0f && fabsf(w / h - 800.0f / 600.0f) < 0.01f, "Fit: smaller, the same aspect (%.1f x %.1f)", w, h);
            bool cbs = false;
            for (const ImDrawCmd& cmd : cb) cbs = cbs || cmd.UserCallback == cb_nearest || cmd.UserCallback == cb_linear;
            CHECK(!cbs, "Fit: no sampler change");
            CHECK(strstr(g_log, "Scaled: fine patterns alias; use 1:1") != nullptr, "Fit says that fine patterns alias");
            ImGui::GetIO().AddMousePosEvent(floorf(mn.x + w * 0.5f), floorf(mn.y + h * 0.5f));
            frame();
            float x = 0, y = 0;
            bool inside = false;
            pvd_pointer(&x, &y, &inside);
            CHECK(inside && fabsf(x - 400.0f) <= 800.0f / w && fabsf(y - 300.0f) <= 600.0f / h, "Fit: the middle is (400, 300) px: (%.2f, %.2f)",
                  x, y);
        }
        pio.DrawCallback_SetSamplerNearest = nullptr;
        pio.DrawCallback_SetSamplerLinear = nullptr;
        pvd_enable(false);
    }
#endif

    ui_shutdown();
    ImGui::DestroyContext();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
