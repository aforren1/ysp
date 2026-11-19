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
// Exit 0 on pass, 1 on a failure.
#include "ui.h"
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

    ui_shutdown();
    ImGui::DestroyContext();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
