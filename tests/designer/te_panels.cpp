// te_panels.cpp - the property panels driven by the Test Engine (step 4,
// part C; docs/designer.md, "The panels"). OWNED BY PART C.
//
//   panels/follows_text  The caret in the text selects what the panels show.
//   panels/contrast      A stimulus selected; a contrast typed over its
//                        lookup, and a number typed in a number field, show
//                        in the text as canonical numbers; Ctrl+Z takes each
//                        back.
//   panels/issue_mark    A value out of rdk.h's range is written; the check's
//                        issue marks the field, and a click on the mark goes
//                        to the issue.
//   panels/hebrew        Hebrew typed with the engine's keys into a text
//                        stimulus's text; Ctrl+Z in a focused field is the
//                        field's own undo, not the document's.
//   panels/long_panel    Keys at the end of an aep method's 83 take typed
//                        text: rows out of view take no field.
//   panels/drag          A drag of a number's name changes the value in one
//                        undo step, at the drag's end.
//   panels/select_commits  Text typed in a field, then another selection:
//                        the text is written and the field lets the keyboard go.
//   panels/rename        The name row renames a stimulus and its references.
//   panels/collapsed     A collapsed panel is in the view file and stays
//                        collapsed after a reopen; a reveal opens it.
#include "te_common.h"
#include "ui.h"
#include "views.h"
#include "skf.h"

#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <stdlib.h>
#include <string.h>

namespace {

const char* const kProps = "//##designer/Properties";

ImGuiID field(ImGuiTestContext* ctx, const char* item) { return te_in_child(ctx, kProps, item); }

void select_path(ImGuiTestContext* ctx, const char* path) {
    dsg_sel s;
    dsg_sel_def(&s, path);
    ui_reveal(&s);
    ctx->Yield(2);
}

bool text_has(const char* s) {
    size_t n;
    const char* t = ui_text(&n);
    return t && strstr(t, s) != nullptr;
}

// Replaces a Skribidi field's text as a person does: a click, Ctrl+A, the
// characters, then Enter (enter false: no Enter).
void type_in(ImGuiTestContext* ctx, ImGuiID id, const char* text, bool enter = true) {
    ctx->ItemClick(id);
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
    ctx->KeyChars(text);
    if (enter) ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
}

void test_follows_text(ImGuiTestContext* ctx) {
    static const char* const files[] = { "pack.json", "experiment.json", "conditions.csv", nullptr };
    char dir[DSG_PATH_MAX];
    IM_CHECK(te_copy_project("rdk", "panels_follow", files, dir, sizeof dir));
    te_open(ctx, dir);
    te_show(ctx, "Text");
    IM_CHECK(ctx->WindowInfo(kProps).ID != 0);
    // A click in the editor gives it the focus; the caret then names a value.
    ctx->ItemClick(te_in_child(ctx, "//##designer/Definition", "##definition"));
    size_t n;
    const char* t = ui_text(&n);
    const char* at = strstr(t, "\"stimuli\"");
    IM_CHECK(at != nullptr);
    skf_set_caret(ui_editor(), (int)dsg_codepoints_before(t, n, (uint32_t)(at - t)));
    ctx->Yield();
    IM_CHECK_STR_EQ(dsg_doc_selection(ui_doc())->path, "/stimuli");
    // A value inside a stimulus: the stimulus's panel, with that key's row.
    at = strstr(t, "\"dot_size\"");
    IM_CHECK(at != nullptr);
    skf_set_caret(ui_editor(), (int)dsg_codepoints_before(t, n, (uint32_t)(at - t)));
    ctx->Yield(2);
    IM_CHECK(ctx->ItemExists(field(ctx, "stimulus/##keys/dot_size/##v")));
    // A click in a panel's field takes the keyboard from the editor: the
    // characters go to the field, not to the text at the editor's caret.
    IM_CHECK(skf_focused(ui_editor()));
    type_in(ctx, field(ctx, "stimulus/##keys/coherence/##v"), "0.5");
    IM_CHECK(!skf_focused(ui_editor()));
    IM_CHECK(text_has("\"coherence\": 0.5,"));
    IM_CHECK(text_has("\"dot_size\": 4"));
}

void test_contrast(ImGuiTestContext* ctx) {
    static const char* const files[] = { "pack.json", "experiment.json", nullptr };
    char dir[DSG_PATH_MAX];
    IM_CHECK(te_copy_project("afc2", "panels_contrast", files, dir, sizeof dir));
    te_open(ctx, dir);
    select_path(ctx, "/stimuli/gabor");
    IM_CHECK(text_has("\"michelson\": \"$level\""));
    const int depth = dsg_doc_undo_depth(ui_doc());
    // The contrast is a form ({"michelson": ...}), so its field shows JSON;
    // a number typed there is a number.
    type_in(ctx, field(ctx, "stimulus/##keys/contrast/##v"), "0.250");
    IM_CHECK(text_has("\"contrast\": 0.25,"));
    IM_CHECK(!text_has("michelson"));
    IM_CHECK_EQ(dsg_doc_undo_depth(ui_doc()), depth + 1);
    IM_CHECK(skf_focus_field() == nullptr);
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    IM_CHECK(text_has("\"michelson\": \"$level\""));
    // Escape gives the field its value back and writes nothing.
    ctx->ItemClick(field(ctx, "stimulus/##keys/contrast/##v"));
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
    ctx->KeyChars("9");
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    IM_CHECK(skf_focus_field() == nullptr);
    IM_CHECK(text_has("\"michelson\": \"$level\""));
    IM_CHECK_EQ(dsg_doc_undo_depth(ui_doc()), depth + 1);
    // A number field (ImGui's text field: a number is ASCII).
    ctx->ItemInputValue(field(ctx, "stimulus/##keys/sf/##v"), "2.50");
    IM_CHECK(text_has("\"sf\": 2.5,"));
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    IM_CHECK(text_has("\"sf\": 2,"));
    // Reset: the key goes, its default shows.
    ctx->ItemClick(field(ctx, "stimulus/##keys/sigma/Reset"));
    IM_CHECK(!text_has("\"sigma\""));
    ctx->Yield(2);
    IM_CHECK(!ctx->ItemExists(field(ctx, "stimulus/##keys/sigma/Reset")));
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    IM_CHECK(text_has("\"sigma\": 0.4"));
}

void test_issue_mark(ImGuiTestContext* ctx) {
    static const char* const files[] = { "pack.json", "experiment.json", "conditions.csv", nullptr };
    char dir[DSG_PATH_MAX];
    IM_CHECK(te_copy_project("rdk", "panels_issue", files, dir, sizeof dir));
    te_open(ctx, dir);
    select_path(ctx, "/stimuli/dots");
    IM_CHECK(ui_report() != nullptr && ui_report()->n_errors == 0);
    IM_CHECK(!ctx->ItemExists(field(ctx, "stimulus/##keys/coherence/!##issue")));
    // rdk.h's coherence is 0 to 1; the panel writes 1.5, the check refuses it.
    type_in(ctx, field(ctx, "stimulus/##keys/coherence/##v"), "1.5");
    IM_CHECK(text_has("\"coherence\": 1.5"));
    IM_CHECK(ui_report() != nullptr && ui_report()->n_errors > 0);
    IM_CHECK(ctx->ItemExists(field(ctx, "stimulus/##keys/coherence/!##issue")));
    IM_CHECK(!ctx->ItemExists(field(ctx, "stimulus/##keys/speed/!##issue")));
    // The mark goes to the issue's place in the text.
    ctx->ItemClick(field(ctx, "stimulus/##keys/coherence/!##issue"));
    size_t n;
    const char* t = ui_text(&n);
    const uint32_t off = ui_report()->issues[0].place.offset;
    IM_CHECK_EQ(skf_caret(ui_editor()), (int)dsg_codepoints_before(t, n, off));
}

void test_hebrew(ImGuiTestContext* ctx) {
    static const char* const files[] = { "pack.json", "stroop.json", "stroop.csv", nullptr };
    char dir[DSG_PATH_MAX];
    IM_CHECK(te_copy_project("stroop", "panels_hebrew", files, dir, sizeof dir));
    te_open(ctx, dir);
    select_path(ctx, "/stimuli/keys");
    const ImGuiID id = field(ctx, "stimulus/##keys/text/##v");
    // "shalom, R red" in Hebrew: right to left, with Latin inside.
    type_in(ctx, id, "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D, R \xD7\x90\xD7\x93\xD7\x95\xD7\x9D");
    IM_CHECK(text_has("\"text\": \"\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D, R \xD7\x90\xD7\x93\xD7\x95\xD7\x9D\""));
    // In a focused field Ctrl+Z is the field's: the document keeps its step.
    const int depth = dsg_doc_undo_depth(ui_doc());
    ctx->ItemClick(id);
    ctx->KeyPress(ImGuiKey_End);
    ctx->KeyChars("!");
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    IM_CHECK_EQ(dsg_doc_undo_depth(ui_doc()), depth);
    IM_CHECK(!text_has("\xD7\x9D!\""));
    IM_CHECK(text_has("\xD7\x90\xD7\x93\xD7\x95\xD7\x9D\""));
}

// An aep method has 83 keys, more than the fields on screen at once: a key
// at the end, scrolled to, takes typed text.
void test_long_panel(ImGuiTestContext* ctx) {
    static const char* const files[] = { "pack.json", "experiment.json", nullptr };
    char dir[DSG_PATH_MAX];
    IM_CHECK(te_copy_project("afc2", "panels_long", files, dir, sizeof dir));
    te_open(ctx, dir);
    static const char aep[] = "{\"type\":\"aep\",\"lo\":[0],\"hi\":[1]}";
    IM_CHECK_EQ(dsg_def_set(ui_doc(), "/methods/m", aep, strlen(aep)), 0);
    select_path(ctx, "/methods/m");
    type_in(ctx, field(ctx, "method/##keys/stop_context/##v"), "[0.5, 1.50]");
    IM_CHECK(text_has("\"stop_context\": [0.5, 1.5]"));
    type_in(ctx, field(ctx, "method/##keys/candidates/##v"), "[2]");
    IM_CHECK(text_has("\"candidates\": [2]"));
}

void test_drag(ImGuiTestContext* ctx) {
    static const char* const files[] = { "pack.json", "experiment.json", nullptr };
    char dir[DSG_PATH_MAX];
    IM_CHECK(te_copy_project("afc2", "panels_drag", files, dir, sizeof dir));
    te_open(ctx, dir);
    select_path(ctx, "/stimuli/gabor");
    const int depth = dsg_doc_undo_depth(ui_doc());
    const int checks = ui_checks_run();
    // A drag over several frames, as a hand moves.
    ctx->MouseMove(field(ctx, "stimulus/##keys/sigma/##drag"));
    const ImVec2 p0 = ImGui::GetIO().MousePos;
    ctx->MouseDown(ImGuiMouseButton_Left);
    for (int i = 1; i <= 6; i++) {
        ctx->MouseMoveToPos(ImVec2(p0.x + 10.0f * (float)i, p0.y));
        ctx->Yield();
    }
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield(2);
    IM_CHECK_EQ(dsg_doc_undo_depth(ui_doc()), depth + 1);
    IM_CHECK_EQ(ui_checks_run(), checks + 1);
    IM_CHECK(!text_has("\"sigma\": 0.4,"));
    IM_CHECK(text_has("\"sigma\": "));
}

void test_select_commits(ImGuiTestContext* ctx) {
    static const char* const files[] = { "pack.json", "experiment.json", "conditions.csv", nullptr };
    char dir[DSG_PATH_MAX];
    IM_CHECK(te_copy_project("rdk", "panels_select", files, dir, sizeof dir));
    te_open(ctx, dir);
    select_path(ctx, "/stimuli/dots");
    type_in(ctx, field(ctx, "stimulus/##keys/direction/##v"), "90", false);
    IM_CHECK(skf_focus_field() != nullptr);
    IM_CHECK(!text_has("\"direction\": 90"));
    // another key of the same stimulus: the field is still drawn, so only
    // the selection's change writes it
    select_path(ctx, "/stimuli/dots/speed");
    IM_CHECK(text_has("\"direction\": 90"));
    IM_CHECK(skf_focus_field() == nullptr);
    // Ctrl+Z is the document's again.
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    IM_CHECK(text_has("\"direction\": \"$coherent_direction\""));
}

void test_rename(ImGuiTestContext* ctx) {
    static const char* const files[] = { "pack.json", "experiment.json", "conditions.csv", nullptr };
    char dir[DSG_PATH_MAX];
    IM_CHECK(te_copy_project("rdk", "panels_rename", files, dir, sizeof dir));
    te_open(ctx, dir);
    select_path(ctx, "/stimuli/fix");
    type_in(ctx, field(ctx, "stimulus/##keys/##name/##v"), "cross");
    IM_CHECK(text_has("\"cross\": {"));
    IM_CHECK(text_has("\"on\": \"cross\""));
    IM_CHECK(!text_has("\"fix\""));
    IM_CHECK_STR_EQ(dsg_doc_selection(ui_doc())->path, "/stimuli/cross");
    IM_CHECK(ui_report() != nullptr && ui_report()->n_errors == 0);
    IM_CHECK(strstr(ui_status(), "Renamed fix to cross") != nullptr);
    // A committed field is not committed again when the selection moves.
    const int depth = dsg_doc_undo_depth(ui_doc());
    select_path(ctx, "/stimuli/dots");
    IM_CHECK(strstr(ui_status(), "Renamed fix to cross") != nullptr);
    IM_CHECK_EQ(dsg_doc_undo_depth(ui_doc()), depth);
}

// A collapsed panel stays collapsed in the view file's part "panels".
void test_collapsed(ImGuiTestContext* ctx) {
    static const char* const files[] = { "pack.json", "experiment.json", "conditions.csv", nullptr };
    char dir[DSG_PATH_MAX], other[DSG_PATH_MAX], path[DSG_PATH_MAX];
    IM_CHECK(te_copy_project("rdk", "panels_collapsed", files, dir, sizeof dir));
    IM_CHECK(te_copy_project("rdk", "panels_collapsed2", files, other, sizeof other));
    te_open(ctx, dir);
    select_path(ctx, "/stimuli/dots");
    IM_CHECK(ctx->ItemExists(field(ctx, "stimulus/##keys/speed/##v")));
    ctx->ItemClick(field(ctx, "stimulus/###hdr"));
    ctx->Yield(2);
    IM_CHECK(!ctx->ItemExists(field(ctx, "stimulus/##keys/speed/##v")));
    // The view file is written when the project closes.
    te_open(ctx, other);
    char* t = nullptr;
    size_t n = 0;
    char err[256];
    IM_CHECK(dsg_join(path, sizeof path, dir, DSG_VIEW_FILE) == 0 && dsg_read_file(path, &t, &n, err, sizeof err) == 0);
    IM_CHECK(t && strstr(t, "\"panels\"") && strstr(t, "\"collapsed\"") && strstr(t, "\"stimulus\""));
    free(t);
    te_open(ctx, dir);
    // a selection (the caret in the text) keeps it collapsed
    dsg_sel s;
    dsg_sel_def(&s, "/stimuli/fix/w");
    dsg_doc_select(ui_doc(), &s);
    ctx->Yield(2);
    IM_CHECK(ctx->ItemExists(field(ctx, "stimulus/###hdr")));
    IM_CHECK(!ctx->ItemExists(field(ctx, "stimulus/##keys/w/##v")));
    // A reveal (a click on an issue, a chart's selection) opens it again.
    select_path(ctx, "/stimuli/fix/w");
    IM_CHECK(ctx->ItemExists(field(ctx, "stimulus/##keys/w/##v")));
}

} // namespace

void te_register_panels(ImGuiTestEngine* e) {
    IM_REGISTER_TEST(e, "panels", "follows_text")->TestFunc = test_follows_text;
    IM_REGISTER_TEST(e, "panels", "contrast")->TestFunc = test_contrast;
    IM_REGISTER_TEST(e, "panels", "issue_mark")->TestFunc = test_issue_mark;
    IM_REGISTER_TEST(e, "panels", "hebrew")->TestFunc = test_hebrew;
    IM_REGISTER_TEST(e, "panels", "long_panel")->TestFunc = test_long_panel;
    IM_REGISTER_TEST(e, "panels", "drag")->TestFunc = test_drag;
    IM_REGISTER_TEST(e, "panels", "select_commits")->TestFunc = test_select_commits;
    IM_REGISTER_TEST(e, "panels", "rename")->TestFunc = test_rename;
    IM_REGISTER_TEST(e, "panels", "collapsed")->TestFunc = test_collapsed;
}
