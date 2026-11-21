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
// Exit 0 on pass, 1 on a failure.
#include "ui.h"
#include "views.h"
#include "skf.h"
#include "skt.h"

#include "imgui.h"
#include "imgui_internal.h"

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
    CHECK(ui_preview_where() == nullptr, "no session: nowhere");

    ui_shutdown();
    ImGui::DestroyContext();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
