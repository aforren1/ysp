// view_panels.cpp - the property panels (step 4, part C; docs/designer.md,
// "The panels"). OWNED BY PART C.
//
// The side column shows the selected object's keys, and the keys of the
// objects that hold it as far as its template (an exit, its state, its
// template). designer/core/schema.c gives the keys of each section and kind,
// their units and ranges, the object at a path, and the typed writes; this
// file draws them and queues the edits.
//
// Each edit is one undo step of the document, made after the panels are
// drawn: an edit parses the tree again, and the rows of the frame still
// read the tree they were drawn from. A text field commits on Enter, on the
// loss of its focus and when the selection changes; a drag of a number's
// label commits when the mouse button comes up.
#include "views.h"
#include "ui.h"
#include "schema.h"
#include "skf.h"

#include "imgui.h"
#include "imgui_internal.h"   // ClearActiveID: a selection change ends an edit

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const int kChain = 4;       // an exit, its state, its template (and a spare)
const int kPool = 48;       // Skribidi fields on screen at once
const int kText = 512;      // a number's or a short value's text
const int kActions = 16;    // edits queued in one frame

enum Role { ROLE_VALUE, ROLE_JSON, ROLE_NAME, ROLE_WHOLE };

// A Skribidi field of the pool, given to one row while it is drawn.
struct Slot {
    SkfField* f;
    ImGuiID id;             // the row's value item; 0: free
    bool drawn;
    uint64_t rev;           // the definition's revision its text came from
    Role role;
    dsg_obj obj;            // where its commit writes
    const dsg_field* field;
    ImVector<char> shown;   // the text it was given, NUL-ended
};

enum ActKind { ACT_WRITE, ACT_RESET, ACT_RENAME, ACT_WHOLE, ACT_SELECT, ACT_ADD, ACT_TEXT_AT };

struct Action {
    ActKind kind;
    dsg_obj obj;
    const dsg_field* field;
    unsigned how;
    char* text;             // malloc; the value, the name or a path
};

struct State {
    uint64_t sel_rev = 0;
    bool have_sel = false;
    int sel_buf = -1;
    char sel_path[DSG_SEL_PATH] = "";
    dsg_obj chain[kChain];
    int n_chain = 0;
    // the ImGui text field of a number while it has the keyboard
    ImGuiID edit_id = 0;
    bool edit_drawn = false;
    char edit_buf[kText] = "";
    char edit_orig[kText] = "";
    dsg_obj edit_obj;
    const dsg_field* edit_field = nullptr;
    // a drag of a number's label
    ImGuiID drag_id = 0;
    double drag_v0 = 0;
    char drag_text[64] = "";
    bool drag_moved = false;
    Slot pool[kPool];
    ImVector<char> collapsed;   // object kind names ("stimulus"), each NUL-ended
    bool reveal = false;        // scroll the selected key into view
    bool open = false;          // ui_reveal(): open its panel if collapsed
    ImVector<char> cands;
    Action acts[kActions];
    int n_acts = 0;
};
State* P = nullptr;

dsg_doc* doc() { return ui_doc(); }

// --- the view file's part: {"collapsed": [KIND, ...]} --------------------------

bool is_collapsed(const char* name) {
    for (int i = 0; i < P->collapsed.Size; i += (int)strlen(P->collapsed.Data + i) + 1)
        if (strcmp(P->collapsed.Data + i, name) == 0) return true;
    return false;
}

void append(ImVector<char>* v, const char* s) {
    const int n = (int)strlen(s) + 1;
    const int at = v->Size;
    v->resize(at + n);
    memcpy(v->Data + at, s, (size_t)n);
}

void set_collapsed(const char* name, bool c) {
    ImVector<char> keep;
    for (int i = 0; i < P->collapsed.Size; i += (int)strlen(P->collapsed.Data + i) + 1)
        if (strcmp(P->collapsed.Data + i, name) != 0) append(&keep, P->collapsed.Data + i);
    if (c) append(&keep, name);
    P->collapsed.swap(keep);
}

// --- edits, after the frame's rows ----------------------------------------------

char* dup(const char* s) {
    const size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

void queue(ActKind k, const dsg_obj* o, const dsg_field* f, const char* text, unsigned how = 0) {
    if (P->n_acts == kActions) {
        ui_set_status("Too many edits in one frame; one was not kept.");
        return;
    }
    Action& a = P->acts[P->n_acts];
    a.kind = k;
    if (o) a.obj = *o;
    a.field = f;
    a.how = how;
    a.text = dup(text ? text : "");
    if (a.text) P->n_acts++;
}

void select_path(const char* path) {
    dsg_sel s;
    dsg_sel_def(&s, path);
    ui_reveal(&s);
}

// The text view with the caret at a key: lists, maps and forms are edited
// there.
void open_text_at(const char* path, const char* fallback) {
    const yjs_value* root = dsg_doc_root(doc());
    uint32_t off = 0;
    if (!root || (!dsg_path_offset(root, path, &off) && !dsg_path_offset(root, fallback, &off))) return;
    ui_show_view("text");
    size_t n = 0;
    const char* t = dsg_doc_bytes(doc(), DSG_BUF_DEF, &n);
    skf_set_caret(ui_editor(), (int)dsg_codepoints_before(t, n, off));
}

void rename_now(const dsg_obj& o, const char* to) {
    char err[512] = "", label[DSG_NAME_MAX + 16];
    int changed = 0;
    snprintf(label, sizeof label, "Rename %s", o.name);
    dsg_doc_begin(doc(), label);
    if (dsg_schema_rename(doc(), &o, to, &changed, err, sizeof err)) {
        dsg_doc_cancel(doc());
        ui_set_status("Not renamed: %s", err);
        return;
    }
    // The selection follows a set member to its new key, in the same step,
    // so undo and redo select the name they give.
    const dsg_sel* s = dsg_doc_selection(doc());
    if (o.set[0] && s->buf == DSG_BUF_DEF && dsg_path_within(s->path, o.path)) {
        char p[DSG_SEL_PATH];
        if (dsg_path_key(p, sizeof p, o.set, to) == 0 && strlen(p) + strlen(s->path + strlen(o.path)) < sizeof p) {
            strcat(p, s->path + strlen(o.path));
            dsg_sel ns;
            dsg_sel_def(&ns, p);
            dsg_doc_select(doc(), &ns);
        }
    }
    if (dsg_doc_commit(doc())) ui_set_status("Not renamed: %s", dsg_doc_error(doc()));
    else ui_set_status("Renamed %s to %s; %d references changed. The check reports any it did not find.", o.name, to, changed);
}

void run_actions() {
    char err[512];
    for (int i = 0; i < P->n_acts; i++) {
        Action& a = P->acts[i];
        err[0] = 0;
        switch (a.kind) {
        case ACT_WRITE:
            if (dsg_schema_write(doc(), &a.obj, a.field, a.text, a.how, err, sizeof err))
                ui_set_status("%s was not written: %s", a.field->key, err);
            break;
        case ACT_RESET:
            if (dsg_schema_reset(doc(), &a.obj, a.field, err, sizeof err)) ui_set_status("%s was not reset: %s", a.field->key, err);
            break;
        case ACT_RENAME: rename_now(a.obj, a.text); break;
        case ACT_WHOLE: {
            // a value with no keys (a column, a variable): JSON, or else text
            const size_t n = strlen(a.text);
            if (dsg_def_set(doc(), a.obj.path, a.text, n) && dsg_def_set_string(doc(), a.obj.path, a.text))
                ui_set_status("%s was not written: %s", a.obj.name, dsg_doc_error(doc()));
            break;
        }
        case ACT_SELECT: select_path(a.text); break;
        case ACT_ADD:
            dsg_doc_begin(doc(), "Add");
            if (dsg_def_set(doc(), a.text, "{}", 2)) {
                dsg_doc_cancel(doc());
                ui_set_status("Not added: %s", dsg_doc_error(doc()));
                break;
            }
            {
                dsg_sel s;
                dsg_sel_def(&s, a.text);
                dsg_doc_select(doc(), &s);
            }
            dsg_doc_commit(doc());
            break;
        case ACT_TEXT_AT: open_text_at(a.text, a.obj.path); break;
        }
        free(a.text);
        a.text = nullptr;
    }
    P->n_acts = 0;
}

// --- commits of the text fields -----------------------------------------------------

void commit_slot(Slot& s) {
    ImVector<char> t;
    skf_text(s.f, &t);
    if (s.shown.Size && strcmp(t.Data, s.shown.Data) == 0) return;
    if (s.role == ROLE_NAME) queue(ACT_RENAME, &s.obj, nullptr, t.Data);
    else if (s.role == ROLE_WHOLE) queue(ACT_WHOLE, &s.obj, nullptr, t.Data);
    else queue(ACT_WRITE, &s.obj, s.field, t.Data, s.role == ROLE_JSON ? DSG_WRITE_JSON : DSG_WRITE_TYPED);
    // committed once: a later commit_all() or end_frame() finds no change,
    // and the next frame shows the document's text
    s.shown.swap(t);
    s.rev = 0;
}

void commit_edit() {
    if (!P->edit_id) return;
    if (strcmp(P->edit_buf, P->edit_orig) != 0) queue(ACT_WRITE, &P->edit_obj, P->edit_field, P->edit_buf);
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.ActiveId == P->edit_id) ImGui::ClearActiveID();
    P->edit_id = 0;
}

// Every field with typed text writes it and lets the keyboard go: a field
// that kept the focus would take the next Ctrl+Z from the document's stack.
void commit_all() {
    commit_edit();
    for (Slot& s : P->pool) {
        if (!s.id || !s.f) continue;
        commit_slot(s);
        skf_take_events(s.f);
        skf_blur(s.f);
    }
    run_actions();
}

// --- the issues' marks ------------------------------------------------------------------

// The first issue whose value is path or under it; -1 when none.
int issue_at(const char* path, bool exact) {
    const dsg_report* r = ui_report();
    for (int i = 0; r && i < r->n_issues; i++) {
        const char* p = ui_issue_path(i);
        if (!p[0]) continue;
        if (exact ? strcmp(p, path) == 0 : dsg_path_within(p, path)) return i;
    }
    return -1;
}

ImU32 sev_color(int i) {
    const dsg_report* r = ui_report();
    const int s = r && i >= 0 && i < r->n_issues ? r->issues[i].severity : DSG_SEV_NOTE;
    switch (s) {
    case DSG_SEV_ERROR: return IM_COL32(255, 110, 100, 255);
    case DSG_SEV_WARNING: return IM_COL32(240, 200, 90, 255);
    case DSG_SEV_NEED: return IM_COL32(140, 190, 255, 255);
    default: return IM_COL32(180, 180, 180, 255);
    }
}

// The mark: a click jumps to the issue, as a click in the issues panel does.
void issue_mark(int i, const char* id) {
    ImGui::PushStyleColor(ImGuiCol_Text, sev_color(i));
    if (ImGui::SmallButton(id)) ui_jump_to_issue(i);
    ImGui::PopStyleColor();
    const dsg_report* r = ui_report();
    if (r && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r->issues[i].message);
}

// --- rows ------------------------------------------------------------------------------

const ImU32 kGrey = IM_COL32(130, 130, 140, 255);

Slot* slot_for(ImGuiID id) {
    for (Slot& s : P->pool)
        if (s.id == id) return &s;
    for (Slot& s : P->pool)
        if (!s.id) {
            if (!s.f) s.f = skf_create(ui_text_size(), false);
            s.id = id;
            s.rev = 0;
            s.shown.resize(0);
            return &s;
        }
    return nullptr;
}

// A Skribidi field: user text, a name, a value as JSON.
void text_field(const dsg_obj& o, const dsg_field* f, Role role, const char* text, bool grey, bool ro) {
    const ImGuiID id = ImGui::GetID("##v");
    const ImVec2 size(ImGui::GetContentRegionAvail().x, ui_text_size() * 1.6f);
    Slot* s = nullptr;
    for (Slot& x : P->pool)
        if (x.id == id) s = &x;
    // A row out of view takes no field (an aep method has 83 keys), only
    // its place and ID, so a scroll or the Test Engine can reach it.
    if (!s && !ImGui::IsRectVisible(size)) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::ItemSize(size);
        ImGui::ItemAdd(ImRect(at, ImVec2(at.x + size.x, at.y + size.y)), id);
        return;
    }
    if (!s) s = slot_for(id);
    if (!s) {
        ImGui::TextUnformatted(text);
        return;
    }
    s->drawn = true;
    const uint64_t rev = dsg_doc_rev(doc(), DSG_BUF_DEF);
    if (!skf_focused(s->f) && (s->rev != rev || !s->shown.Size)) {
        const int n = (int)strlen(text);
        skf_set_text(s->f, text, n);
        s->shown.resize(n + 1);
        memcpy(s->shown.Data, text, (size_t)n + 1);
        s->rev = rev;
        s->role = role;
        s->obj = o;
        s->field = f;
    }
    if (ro) ImGui::BeginDisabled();
    if (grey && !skf_focused(s->f)) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.55f);
    const bool pushed = grey && !skf_focused(s->f);
    skf_draw("##v", s->f, size.x, size.y);
    if (pushed) ImGui::PopStyleVar();
    if (ro) ImGui::EndDisabled();
    const unsigned ev = skf_take_events(s->f);
    if (ev & SKF_EV_ESCAPE) {
        skf_set_text(s->f, s->shown.Data, s->shown.Size ? s->shown.Size - 1 : 0);
        skf_blur(s->f);
    } else if (ev & (SKF_EV_ENTER | SKF_EV_BLUR)) {
        commit_slot(*s);
        skf_blur(s->f);
    }
}

double number_of(const dsg_field* f, const yjs_value* v) {
    double x = 0;
    if (v && yjs_double(v, &x) == YJS_OK) return x;
    return f->def ? strtod(f->def, nullptr) : 0.0;
}

// A drag's step per pixel: the header's range over 500 px where it has a
// sensible one, else a two-hundredth of the value.
double drag_step(const dsg_field* f, double v0) {
    if (f->type == YEXP_T_INT) return 0.1;
    const double span = isfinite(f->min) && isfinite(f->max) ? f->max - f->min : 0.0;
    if (span > 0 && span <= 1e4) return span / 500.0;
    return fabs(v0) > 0 ? fabs(v0) / 200.0 : 0.01;
}

// The dragged value as decimal text, rounded to the step's decimals, so the
// document gets a short decimal and not a binary fraction's digits.
void drag_format(const dsg_field* f, double v, double step, char* out, size_t cap) {
    if (isfinite(f->min) && v < f->min) v = f->min;
    if (isfinite(f->max) && v > f->max) v = f->max;
    if (f->type == YEXP_T_INT) {
        snprintf(out, cap, "%lld", (long long)llround(v));
        return;
    }
    int dec = (int)ceil(-log10(step));
    dec = dec < 0 ? 0 : dec > 9 ? 9 : dec;
    snprintf(out, cap, "%.*f", dec, v);
    if (strchr(out, '.')) {
        size_t n = strlen(out);
        while (n > 0 && out[n - 1] == '0') out[--n] = 0;
        if (n > 0 && out[n - 1] == '.') out[--n] = 0;
    }
    if (strcmp(out, "-0") == 0) snprintf(out, cap, "0");
}

void key_tooltip(const dsg_field* f) {
    if (!ImGui::BeginItemTooltip()) return;
    ImGui::Text("%s%s", f->key, f->required ? " (required)" : "");
    if (f->doc[0]) ImGui::TextWrapped("%s", f->doc);
    if (isfinite(f->min) || isfinite(f->max)) ImGui::Text("Range %g to %g %s", f->min, f->max, f->unit);
    if (f->def) ImGui::Text("Default %s", f->def);
    else if (!f->required) ImGui::TextUnformatted("No default: none when left out");
    ImGui::TextDisabled("From %s", f->from);
    if (f->type == YEXP_T_DUR) ImGui::TextDisabled("Seconds; written as the exact decimal of the nanoseconds.");
    ImGui::EndTooltip();
}

// The key's name. A number's name drags its value.
void key_cell(const dsg_obj& o, const dsg_field* f, const yjs_value* v, bool numeric, bool ro) {
    const ImU32 col = v ? ImGui::GetColorU32(ImGuiCol_Text) : kGrey;
    if (!numeric || ro) {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(f->key);
        ImGui::PopStyleColor();
        key_tooltip(f);
        return;
    }
    const ImVec2 ts = ImGui::CalcTextSize(f->key);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##drag", ImVec2(ImMax(ts.x, 1.0f), ImGui::GetFrameHeight()));
    const ImGuiID id = ImGui::GetItemID();
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated()) {
        P->drag_id = id;
        P->drag_v0 = number_of(f, v);
        P->drag_moved = false;
        P->drag_text[0] = 0;
    }
    if (ImGui::IsItemActive() && P->drag_id == id) {
        const float dx = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x;
        if (dx != 0.0f) {
            const double step = drag_step(f, P->drag_v0);
            drag_format(f, P->drag_v0 + (double)dx * step, step, P->drag_text, sizeof P->drag_text);
            P->drag_moved = true;
        }
    }
    // One step at the drag's end: a check runs after each step, and the
    // stack would hold one step a frame.
    if (ImGui::IsItemDeactivated() && P->drag_id == id) {
        if (P->drag_moved && P->drag_text[0]) queue(ACT_WRITE, &o, f, P->drag_text);
        P->drag_id = 0;
        P->drag_moved = false;
    }
    ImGui::GetWindowDrawList()->AddText(ImVec2(at.x, at.y + ImGui::GetStyle().FramePadding.y), col, f->key);
    key_tooltip(f);
}

void number_field(const dsg_obj& o, const dsg_field* f, const yjs_value* v, ImGuiID drag) {
    char shown[kText], work[kText];
    dsg_schema_value_text(f, v, shown, sizeof shown);
    const ImGuiID id = ImGui::GetID("##v");
    if (P->drag_id == drag && P->drag_moved) snprintf(shown, sizeof shown, "%s", P->drag_text);
    memcpy(work, shown, sizeof work);
    const bool editing = P->edit_id == id;
    const bool grey = !v && !editing && !(P->drag_id == drag && P->drag_moved);
    if (grey) ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##v", f->required ? "required" : "none", editing ? P->edit_buf : work, kText);
    if (grey) ImGui::PopStyleColor();
    if (ImGui::IsItemActivated()) {
        P->edit_id = id;
        memcpy(P->edit_buf, work, sizeof work);
        memcpy(P->edit_orig, shown, sizeof shown);
        P->edit_obj = o;
        P->edit_field = f;
    }
    if (P->edit_id == id) P->edit_drawn = true;
    if (ImGui::IsItemDeactivated() && P->edit_id == id) commit_edit();
}

void enum_combo(const dsg_obj& o, const dsg_field* f, const yjs_value* v) {
    char cur[kText];
    dsg_schema_value_text(f, v, cur, sizeof cur);
    if (!v) ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool open = ImGui::BeginCombo("##v", cur[0] ? cur : "none");
    if (!v) ImGui::PopStyleColor();
    if (!open) return;
    const char* p = f->values;
    while (p && *p) {
        const char* eq = strchr(p, '=');
        if (!eq) break;
        char name[64];
        snprintf(name, sizeof name, "%.*s", (int)(eq - p), p);
        if (ImGui::Selectable(name, strcmp(name, cur) == 0)) queue(ACT_WRITE, &o, f, name);
        p = strchr(eq, ',');
        p = p ? p + 1 : nullptr;
    }
    if (!f->required && !f->def && ImGui::Selectable("(none)", !v)) queue(ACT_RESET, &o, f, nullptr);
    ImGui::EndCombo();
}

// A name: a combo of the names the key can take where the row is (format
// 3.9). A name that is not among them still shows; the check says why.
void name_combo(const dsg_obj& o, const dsg_field* f, const yjs_value* v, const yjs_value* root) {
    char cur[kText];
    dsg_schema_value_text(f, v, cur, sizeof cur);
    if (!v) ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool open = ImGui::BeginCombo("##v", cur[0] ? cur : (f->required ? "required" : "none"));
    if (!v) ImGui::PopStyleColor();
    if (!open) return;
    if (P->cands.Size < 8192) P->cands.resize(8192);
    const int n = dsg_schema_candidates(root, &o, f, P->cands.Data, (size_t)P->cands.Size);
    const char* s = P->cands.Data;
    for (int i = 0; i < n && *s; i++, s += strlen(s) + 1)
        if (ImGui::Selectable(s, strcmp(s, cur) == 0)) queue(ACT_WRITE, &o, f, s);
    if (n == 0) ImGui::TextDisabled("No names of this kind yet.");
    if (!f->required && ImGui::Selectable("(none)", !v)) queue(ACT_RESET, &o, f, nullptr);
    ImGui::EndCombo();
}

bool string_widget(int t) {
    return t == YEXP_T_STR || t == YEXP_T_TEXT || t == YEXP_T_RES || t == YEXP_T_NAMEZ || t == YEXP_T_NAME || t == YEXP_T_VAR ||
           t == YEXP_T_REF;
}

void value_cell(const dsg_obj& o, const dsg_field* f, const yjs_value* v, const char* path, const yjs_value* root, bool ro,
                ImGuiID drag) {
    const bool fits = dsg_schema_fits(f, v);
    const int holds = dsg_schema_holds(o.obj, f->key);
    char text[kText];
    if (fits && (f->type == YEXP_T_NUM || f->type == YEXP_T_INT || f->type == YEXP_T_DUR)) {
        if (ro) ImGui::BeginDisabled();
        number_field(o, f, v, drag);
        if (ro) ImGui::EndDisabled();
        return;
    }
    if (fits && f->type == YEXP_T_BOOL) {
        bool b = v ? v->type == YJS_TRUE : (f->def && strcmp(f->def, "true") == 0);
        if (ro) ImGui::BeginDisabled();
        if (!v) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.55f);
        if (ImGui::Checkbox("##v", &b)) queue(ACT_WRITE, &o, f, b ? "true" : "false");
        if (!v) ImGui::PopStyleVar();
        if (ro) ImGui::EndDisabled();
        return;
    }
    if (fits && f->type == YEXP_T_ENUM) {
        if (ro) ImGui::BeginDisabled();
        enum_combo(o, f, v);
        if (ro) ImGui::EndDisabled();
        return;
    }
    if (fits && f->refs && f->type != YEXP_T_VAR && (f->type == YEXP_T_NAME || f->type == YEXP_T_REF)) {
        if (ro) ImGui::BeginDisabled();
        name_combo(o, f, v, root);
        if (ro) ImGui::EndDisabled();
        return;
    }
    if (holds == 1) {
        // an object with a panel of its own: the response, the loop, over
        if (v) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%u keys", v->type == YJS_OBJECT ? v->n : 0u);
            ImGui::SameLine();
            if (ImGui::SmallButton("Select")) queue(ACT_SELECT, nullptr, nullptr, path);
        } else if (!ro) {
            if (ImGui::SmallButton("Add")) queue(ACT_ADD, nullptr, nullptr, path);
        }
        return;
    }
    if (holds == 2 || f->type == YEXP_T_LIST || f->type == YEXP_T_SET) {
        // states, steps, stimuli: the chart, the text view and the tree edit them
        ImGui::AlignTextToFramePadding();
        if (v && v->type == YJS_ARRAY) ImGui::TextDisabled("%u items", v->n);
        else if (v && v->type == YJS_OBJECT) ImGui::TextDisabled("%u members", v->n);
        else ImGui::TextDisabled("none");
        return;
    }
    dsg_schema_value_text(f, v, text, sizeof text);
    text_field(o, f, fits && string_widget(f->type) ? ROLE_VALUE : ROLE_JSON, text, !v, ro);
}

// One key: its name (and a drag for a number), its value, its unit, Reset.
void key_row(const dsg_obj& o, const dsg_field* f, const yjs_value* root, bool ro, bool innermost) {
    char path[DSG_SEL_PATH];
    if (dsg_schema_key_path(&o, f->key, path, sizeof path)) return;
    const yjs_value* v = dsg_path_get(root, path);
    const bool numeric = (f->type == YEXP_T_NUM || f->type == YEXP_T_INT || f->type == YEXP_T_DUR) && dsg_schema_fits(f, v);
    ImGui::TableNextRow();
    ImGui::PushID(f->key);
    const ImGuiID drag = ImGui::GetID("##drag");
    if (innermost && strcmp(o.focus, f->key) == 0) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(55, 60, 95, 255));
        if (P->reveal) {
            ImGui::SetScrollHereY(0.3f);
            P->reveal = false;
        }
    }
    ImGui::TableSetColumnIndex(0);
    key_cell(o, f, v, numeric, ro);
    const int is = issue_at(path, false);
    if (is >= 0) {
        ImGui::SameLine();
        issue_mark(is, "!##issue");
    }
    ImGui::TableSetColumnIndex(1);
    value_cell(o, f, v, path, root, ro, drag);
    ImGui::TableSetColumnIndex(2);
    ImGui::AlignTextToFramePadding();
    double secs = 0;
    // D4: a duration above a minute is more often a typo than a plan
    if (f->type == YEXP_T_DUR && v && yjs_double(v, &secs) == YJS_OK && secs > 60.0) {
        ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1.0f), "s > 60");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Above 60 s: the format's lint warns (D4). Seconds, not ms?");
    } else {
        ImGui::TextDisabled("%s", f->unit);
    }
    ImGui::TableSetColumnIndex(3);
    if (v && !ro && ImGui::SmallButton("Reset")) queue(ACT_RESET, &o, f, nullptr);
    if (v && ImGui::IsItemHovered()) ImGui::SetTooltip("Remove the key: its default applies");
    if (dsg_schema_complex(f->type) || !dsg_schema_fits(f, v)) {
        if (v) ImGui::SameLine();
        if (ImGui::SmallButton("Text")) queue(ACT_TEXT_AT, &o, nullptr, path);
    }
    ImGui::PopID();
}

// The name of a set member, a state or a node: a rename changes the
// references the schema can find too.
void name_row(const dsg_obj& o, bool ro) {
    ImGui::TableNextRow();
    ImGui::PushID("##name");
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("name");
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted("The name. A rename changes the references to it that the format's name keys hold.");
        ImGui::EndTooltip();
    }
    ImGui::TableSetColumnIndex(1);
    text_field(o, nullptr, ROLE_NAME, o.name, false, ro);
    ImGui::PopID();
}

// A value with no keys: a column, a node's data column, a plain variable.
void value_row(const dsg_obj& o, const yjs_value* root, bool ro) {
    const yjs_value* v = dsg_path_get(root, o.path);
    char text[kText];
    text[0] = 0;
    if (v && v->type == YJS_STRING) snprintf(text, sizeof text, "%s", v->u.s);
    else if (v && yjs_write_mem(v, YJS_WRITE_COMPACT, text, sizeof text) >= sizeof text) snprintf(text + sizeof text - 4, 4, "...");
    ImGui::TableNextRow();
    ImGui::PushID("##value");
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("value");
    const int is = issue_at(o.path, false);
    if (is >= 0) {
        ImGui::SameLine();
        issue_mark(is, "!##issue");
    }
    ImGui::TableSetColumnIndex(1);
    text_field(o, nullptr, ROLE_WHOLE, text, false, ro);
    ImGui::TableSetColumnIndex(3);
    if (ImGui::SmallButton("Text")) queue(ACT_TEXT_AT, &o, nullptr, o.path);
    ImGui::PopID();
}

void object_panel(const dsg_obj& o, const yjs_value* root, bool ro, bool innermost) {
    const char* kind = dsg_schema_obj_name(o.obj);
    ImGui::PushID(kind);
    char label[DSG_NAME_MAX * 2 + 64];
    snprintf(label, sizeof label, "%s %s%s%s%s###hdr", kind, o.name, o.kind[0] ? " (" : "", o.kind, o.kind[0] ? ")" : "");
    bool collapsed = is_collapsed(kind);
    if (innermost && P->open && collapsed) {
        collapsed = false;
        set_collapsed(kind, false);
    }
    ImGui::SetNextItemOpen(!collapsed, ImGuiCond_Always);
    const bool open = ImGui::CollapsingHeader(label);
    if (open == collapsed) set_collapsed(kind, !open);
    if (!open) {
        ImGui::PopID();
        return;
    }
    // an issue at the object itself (the player names a stimulus, a node)
    const int is = issue_at(o.path, true);
    if (is >= 0) {
        issue_mark(is, "! issue##object");
        ImGui::SameLine();
        ImGui::TextDisabled("at this %s", kind);
    }
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV;
    if (!ImGui::BeginTable("##keys", 4, tf)) {
        ImGui::PopID();
        return;
    }
    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("unit", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("reset", ImGuiTableColumnFlags_WidthFixed);
    if (dsg_schema_renamable(&o)) name_row(o, ro);
    if (o.section < 0) {
        value_row(o, root, ro);
    } else {
        const dsg_field* sel[256];
        const char* disc = o.obj == DSG_OBJ_STIMULUS ? "kind" : o.obj == DSG_OBJ_CONTROL ? "control" : o.obj == DSG_OBJ_METHOD ? "type" : nullptr;
        int n = o.kind_bit ? dsg_schema_select(o.section, o.kind_bit, sel, 256) : 0;
        if (!o.kind_bit && disc) {
            // no kind this format knows: only the key that picks one
            const dsg_field* f = dsg_schema_field(o.section, 0xffffu, disc);
            if (f) sel[n++] = f;
        }
        for (int i = 0; i < n && i < 256; i++) {
            // a state's and a node's name is the name row
            if ((o.obj == DSG_OBJ_STATE || o.obj == DSG_OBJ_NODE) && strcmp(sel[i]->key, "name") == 0) continue;
            key_row(o, sel[i], root, ro, innermost);
        }
    }
    ImGui::EndTable();
    if (!o.kind_bit && o.obj == DSG_OBJ_STEP) ImGui::TextDisabled("This step has no key that names a step of format 2.");
    ImGui::PopID();
}

// --- the view ------------------------------------------------------------------------------

bool init(char*, size_t) {
    P = IM_NEW(State)();
    for (Slot& s : P->pool) {
        s.f = nullptr;
        s.id = 0;
        s.drawn = false;
        s.rev = 0;
        s.role = ROLE_VALUE;
        s.field = nullptr;
    }
    return true;
}

void shutdown() {
    if (!P) return;
    for (Slot& s : P->pool) skf_destroy(s.f);
    for (int i = 0; i < P->n_acts; i++) free(P->acts[i].text);
    IM_DELETE(P);
    P = nullptr;
}

void opened(const dsg_view* v) {
    P->collapsed.resize(0);
    P->have_sel = false;
    size_t n = 0;
    const char* json = dsg_view_part_get(v, "panels", &n);
    if (!json) return;
    yjs_arena a;
    yjs_arena_init_heap(&a, (size_t)1 << 20);
    yjs_value* root = nullptr;
    yjs_error e;
    if (yjs_parse(&a, json, n, nullptr, &root, &e) == YJS_OK) {
        const yjs_value* c = yjs_get(root, "collapsed");
        for (uint32_t i = 0; c && c->type == YJS_ARRAY && i < c->n; i++)
            if (c->u.items[i]->type == YJS_STRING) set_collapsed(c->u.items[i]->u.s, true);
    }
    yjs_arena_free(&a);
}

void closed() {
    // the document is about to be another project's: typed text is not kept
    for (Slot& s : P->pool) {
        if (s.f) {
            skf_blur(s.f);
            skf_take_events(s.f);
        }
        s.id = 0;
    }
    for (int i = 0; i < P->n_acts; i++) free(P->acts[i].text);
    P->n_acts = 0;
    P->edit_id = 0;
    P->drag_id = 0;
    P->have_sel = false;
}

void save_view(dsg_view* v) {
    yjs_arena a;
    yjs_arena_init_heap(&a, (size_t)1 << 20);
    yjs_value* root = yjs_new(&a, YJS_OBJECT);
    yjs_value* list = yjs_new(&a, YJS_ARRAY);
    for (int i = 0; root && list && i < P->collapsed.Size; i += (int)strlen(P->collapsed.Data + i) + 1)
        yjs_push(&a, list, yjs_new_stringz(&a, P->collapsed.Data + i));
    if (root && list && yjs_set(&a, root, "collapsed", list) == YJS_OK) {
        const size_t n = yjs_write_mem(root, YJS_WRITE_COMPACT, nullptr, 0);
        char* buf = (char*)yjs_alloc(&a, n + 1);
        if (buf && yjs_write_mem(root, YJS_WRITE_COMPACT, buf, n + 1) == n) dsg_view_part_set(v, "panels", buf, n);
    }
    yjs_arena_free(&a);
}

void reveal(const dsg_sel*) {
    P->reveal = true;
    P->open = true;
}

// Fields not drawn this frame (their object went) write what was typed and
// go back to the pool; so does the number field.
void end_frame() {
    for (Slot& s : P->pool) {
        if (!s.id || s.drawn) continue;
        if (s.f) {
            commit_slot(s);
            skf_take_events(s.f);
            skf_blur(s.f);
        }
        s.id = 0;
    }
    if (P->edit_id && !P->edit_drawn) commit_edit();
    run_actions();
}

void draw() {
    dsg_doc* d = doc();
    const dsg_sel* s = dsg_doc_selection(d);
    // A new selection: what was typed for the last one is written first,
    // before this frame reads the tree.
    if (!P->have_sel || dsg_doc_selection_rev(d) != P->sel_rev || s->buf != P->sel_buf || strcmp(s->path, P->sel_path) != 0) {
        if (P->have_sel) commit_all();
        s = dsg_doc_selection(d);
        P->have_sel = true;
        P->sel_rev = dsg_doc_selection_rev(d);
        P->sel_buf = s->buf;
        snprintf(P->sel_path, sizeof P->sel_path, "%s", s->path);
        P->reveal = true;
    }
    for (Slot& sl : P->pool) sl.drawn = false;
    P->edit_drawn = false;

    bool stale = false;
    const yjs_value* root = dsg_doc_shown(d, &stale);
    if (!root) {
        ImGui::TextWrapped("The definition does not parse. Fix it in the text view.");
        end_frame();
        return;
    }
    if (s->buf != DSG_BUF_DEF) {
        ImGui::TextDisabled(s->buf < 0 ? "Nothing selected." : "A table cell is selected.");
        end_frame();
        return;
    }
    if (stale) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "The text does not parse: the panels are read only.");
    P->n_chain = dsg_schema_chain(root, s->path, P->chain, kChain);
    for (int i = 0; i < P->n_chain; i++) object_panel(P->chain[i], root, stale, i == 0);
    if (P->n_chain == 0) ImGui::TextDisabled("Nothing selected.");
    P->reveal = false;
    P->open = false;
    end_frame();
}

} // namespace

extern const UiView ui_view_panels = {
    "panels", "Properties", UI_SLOT_SIDE,
    init, shutdown, opened, closed, save_view,
    draw, nullptr, reveal, nullptr, nullptr,
};
