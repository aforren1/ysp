// view_tables.cpp - the tables' grid editor (step 4, part B; docs/designer.md,
// "The tables"). OWNED BY PART B.
//
// Each "table" resource of pack.json is one table here. A CSV source is a
// buffer of the document, so its edits share the one undo stack and Save
// writes its bytes. An Excel source (.xlsx) shows read-only: its bytes are
// not CSV, and the designer writes no workbook. The grid is a cache of the
// buffer (csv.h), made again when the buffer's revision moves.
#include "views.h"
#include "view_tables.h"
#include "csv.h"
#include "skf.h"
#include "skt.h"

#include "imgui.h"
#include "imgui_internal.h"

#include "skribidi/skb_layout.h"
#include "skribidi/skb_layout_cache.h"

// skb_common.h tests __cplusplus__ (sic), so in C++ too its SKB_LITERAL()
// makes a C compound literal, which MSVC refuses (C4576). As a C++20
// designated initializer it is the same value.
#undef SKB_LITERAL
#define SKB_LITERAL(type) type

#include "ysp/json.h"
#include "ysp/pack_tool.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const float kDefaultWidth = 120.0f;
const int kCellCap = 4097;   // ysp/table.h's longest field, and its NUL

struct Width {
    char name[64];
    float px;
};

struct Table {
    char source[DSG_BUF_NAME] = "";   // the "source" in pack.json
    char entry[256] = "";             // the pack entry's "name"
    char delim = ',';
    bool sheet = false;               // an Excel workbook: read-only
    int buf = -1;                     // the document's buffer; -1: none
    char* sheet_csv = nullptr;        // a sheet's text as CSV (malloc)
    size_t sheet_len = 0;
    char note[512] = "";              // why the table does not show
    dsg_csv idx = {};
    ImVector<Width> widths;           // by column name
    int scroll_row = 0, scroll_col = 0;
    bool restore = false;             // put the scroll back at the next draw
};

// A place in the open table that an issue names.
struct Mark {
    int row, col;   // row -2: the whole column; col -1: the whole row
    int issue;
};

struct State {
    ImVector<Table*> tables;
    int open = 0;
    bool select_tab = false;
    skb_layout_cache_t* cache = nullptr;
    ImVector<char> cell;
    // the cell being edited
    SkfField* field = nullptr;
    int edit_table = -1, edit_row = -2, edit_col = -1;
    ImRect field_rect;
    int last_frame = -10;
    uint64_t sel_rev = 0;
    bool scroll_to_sel = false;
    ImVector<Mark> marks;
    bool listening = false;
    // for the tests (view_tables.h)
    int layouts = 0, layouts_last = 0;
    int first_row = -1, last_row = -1;
    ImGuiID grid_seed = 0;
    ImGuiWindow* grid = nullptr;
    ImGuiID grid_table = 0;
    // ImGui keeps a table's widths in memory by its ID: a new ID at each
    // open, so a project never shows the last one's widths
    unsigned generation = 0;
};
State* T = nullptr;

Table* open_table() { return T && T->open >= 0 && T->open < T->tables.Size ? T->tables[T->open] : nullptr; }

const char* table_bytes(const Table* t, size_t* n) {
    if (t->sheet) {
        *n = t->sheet_len;
        return t->sheet_csv ? t->sheet_csv : "";
    }
    return dsg_doc_bytes(ui_doc(), t->buf, n);
}

// The index of the table's bytes now: made again once per change.
bool sync(Table* t) {
    if (t->sheet || t->buf < 0) return t->sheet && t->idx.ok;
    return dsg_csv_sync(&t->idx, ui_doc(), t->buf, t->delim) == 0;
}

float* width_slot(Table* t, const char* name) {
    for (Width& w : t->widths)
        if (strcmp(w.name, name) == 0) return &w.px;
    return nullptr;
}

void set_width(Table* t, const char* name, float px) {
    if (float* w = width_slot(t, name)) {
        *w = px;
        return;
    }
    Width w;
    snprintf(w.name, sizeof w.name, "%s", name);
    w.px = px;
    t->widths.push_back(w);
}

// A column's width follows its rename, and the rename's undo and redo.
void on_rename(void*, const char* scope, const char* from, const char* to) {
    if (!T || scope[0] != '#') return;
    for (Table* t : T->tables) {
        const size_t k = strlen(t->source);
        if (strncmp(scope + 1, t->source, k) != 0 || strcmp(scope + 1 + k, "/columns") != 0) continue;
        if (!width_slot(t, from)) continue;
        for (int i = 0; i < t->widths.Size; i++)
            if (strcmp(t->widths[i].name, to) == 0) {
                t->widths.erase(&t->widths[i]);
                break;
            }
        for (Width& w : t->widths)
            if (strcmp(w.name, from) == 0) snprintf(w.name, sizeof w.name, "%s", to);
    }
}

// --- the layout of a cell's text ------------------------------------------

// The view's own layout cache, compacted each frame: a scroll through
// 10,000 rows would leave every cell's layout in the shared cache, which
// never evicts (2.4 KB a layout, docs/designer_ui_probe.md part 2,
// section 4).
const skb_layout_t* layout(const char* utf8, int n) {
    const skb_attribute_t attrs[] = { skb_attribute_make_font_size(ui_text_size()) };
    skb_layout_params_t params = {};
    params.font_collection = skt_fonts();
    params.layout_width = SKB_SIZE_AUTO;
    params.layout_height = SKB_SIZE_AUTO;
    T->layouts++;
    return skb_layout_cache_get_utf8(T->cache, skt_temp(), &params, utf8, n, SKB_ATTRIBUTE_SET_FROM_STATIC_ARRAY(attrs));
}

// Right-to-left text starts at the cell's right edge.
void draw_text(ImDrawList* dl, const char* s, int n, ImVec2 p, float w, float h, ImU32 col) {
    if (n <= 0) return;
    const skb_layout_t* l = layout(s, n);
    if (!l) return;
    const skb_rect2_t b = skb_layout_get_bounds(l);
    const float x = skb_is_rtl(skb_layout_get_resolved_direction(l)) ? p.x + w - b.width - 4 : p.x + 4;
    dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
    skt_draw_layout(dl, l, x, p.y + (h - b.height) * 0.5f - b.y, 1.0f, col);
    dl->PopClipRect();
}

// --- the edit of one cell ------------------------------------------------

void end_edit() {
    // destroyed, the field gives up the keyboard: Ctrl+Z is the document's again
    if (T->field) skf_destroy(T->field);
    T->field = nullptr;
    T->edit_table = -1;
    T->edit_row = -2;
    T->edit_col = -1;
}

// The field's text into the table: one undo step. A header cell renames
// its column.
void commit_edit() {
    if (!T->field) return;
    const int ti = T->edit_table, row = T->edit_row, col = T->edit_col;
    ImVector<char> text;
    const int n = skf_text(T->field, &text);
    end_edit();
    if (ti < 0 || ti >= T->tables.Size) return;
    Table* t = T->tables[ti];
    if (!sync(t)) {
        ui_set_status("Not edited: %s", t->idx.error);
        return;
    }
    const int rc = row < 0 ? dsg_csv_rename_column(ui_doc(), &t->idx, col, text.Data)
                           : dsg_csv_set_cell(ui_doc(), &t->idx, row, col, text.Data, (size_t)n);
    if (rc) ui_set_status("Not edited: %s", t->idx.error);
}

// keep_text: the cell's text with the caret at its end; else empty, for
// the characters typed on a selected cell.
void begin_edit(int ti, int row, int col, bool keep_text) {
    commit_edit();
    Table* t = T->tables[ti];
    if (t->sheet || !sync(t)) return;
    size_t n = 0;
    const char* b = table_bytes(t, &n);
    T->cell.resize(kCellCap);
    const int len = keep_text ? dsg_csv_cell(&t->idx, b, row, col, T->cell.Data, (size_t)T->cell.Size) : 0;
    if (len < 0) return;
    T->field = skf_create(ui_text_size(), false);
    skf_set_text(T->field, T->cell.Data, len);
    skf_set_caret(T->field, (int)dsg_codepoints_before(T->cell.Data, (size_t)len, (uint32_t)len));
    T->edit_table = ti;
    T->edit_row = row;
    T->edit_col = col;
    T->field_rect = ImRect();
}

void select_cell(Table* t, int row, int col) {
    dsg_sel s;
    dsg_sel_none(&s);
    s.buf = t->buf;
    s.row = row;
    s.col = col;
    dsg_doc_select(ui_doc(), &s);
    T->sel_rev = dsg_doc_selection_rev(ui_doc());
}

// --- opening ---------------------------------------------------------------

bool ends_with(const char* s, const char* tail) {
    const size_t a = strlen(s), b = strlen(tail);
    if (a < b) return false;
    for (size_t i = 0; i < b; i++) {
        char x = s[a - b + i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (x != tail[i]) return false;
    }
    return true;
}

void load_sheet(Table* t, const dsg_project* p, const char* sheet) {
    char path[DSG_PATH_MAX], err[512];
    char* data = nullptr;
    size_t n = 0;
    if (dsg_join(path, sizeof path, p->folder, t->source) || dsg_read_file(path, &data, &n, err, sizeof err)) {
        snprintf(t->note, sizeof t->note, "Cannot read %s.", t->source);
        return;
    }
    ypt_sheet sh;
    memset(&sh, 0, sizeof sh);
    if (ypt_xlsx_read(data, n, sheet, &sh, err, sizeof err) < 0) {
        snprintf(t->note, sizeof t->note, "%s: %s", t->source, err);
    } else {
        t->sheet_csv = sh.csv;
        t->sheet_len = sh.csv_len;
        sh.csv = nullptr;
        t->idx.ok = dsg_csv_index(&t->idx, t->sheet_csv, t->sheet_len, ',', t->idx.error, sizeof t->idx.error) == 0;
    }
    ypt_sheet_free(&sh);
    free(data);
}

void load_table(Table* t, const dsg_project* p) {
    dsg_doc* d = ui_doc();
    t->buf = dsg_doc_find_buffer(d, t->source);
    if (t->buf > DSG_BUF_DEF) return;
    char path[DSG_PATH_MAX], err[512];
    char* data = nullptr;
    size_t n = 0;
    if (dsg_join(path, sizeof path, p->folder, t->source) || dsg_read_file(path, &data, &n, err, sizeof err)) {
        snprintf(t->note, sizeof t->note, "Cannot read %s: %s", t->source, err);
        t->buf = -1;
        return;
    }
    t->buf = dsg_doc_add_buffer(d, DSG_KIND_TABLE, t->source, data, n);
    if (t->buf < 0) snprintf(t->note, sizeof t->note, "Not opened: %s", dsg_doc_error(d));
    free(data);
}

void read_tables(const dsg_project* p) {
    yjs_arena a;
    yjs_value* root = nullptr;
    yjs_error e = {};
    yjs_arena_init_heap(&a, 0);
    if (yjs_parse(&a, p->desc, p->desc_len, nullptr, &root, &e) == YJS_OK && root->type == YJS_OBJECT) {
        const yjs_value* res = yjs_get(root, "resources");
        for (uint32_t i = 0; res && res->type == YJS_ARRAY && i < res->n; i++) {
            const yjs_value* r = res->u.items[i];
            const yjs_value* kind = r->type == YJS_OBJECT ? yjs_get(r, "kind") : nullptr;
            const yjs_value* src = r->type == YJS_OBJECT ? yjs_get(r, "source") : nullptr;
            if (!kind || kind->type != YJS_STRING || strcmp(kind->u.s, "table") != 0 || !src || src->type != YJS_STRING) continue;
            bool seen = false;
            for (const Table* o : T->tables) seen = seen || strcmp(o->source, src->u.s) == 0;
            if (seen) continue;   // one source, one buffer
            Table* t = IM_NEW(Table)();
            t->idx.buf = -1;
            snprintf(t->source, sizeof t->source, "%s", src->u.s);
            const yjs_value* name = yjs_get(r, "name");
            snprintf(t->entry, sizeof t->entry, "%s", name && name->type == YJS_STRING ? name->u.s : "");
            const yjs_value* dl = yjs_get(r, "delimiter");
            if (dl && dl->type == YJS_STRING && dl->n == 1 && (dl->u.s[0] == ';' || dl->u.s[0] == '\t')) t->delim = dl->u.s[0];
            t->sheet = ends_with(t->source, ".xlsx") || ends_with(t->source, ".xlsm");
            const yjs_value* sheet = yjs_get(r, "sheet");
            if (t->sheet) load_sheet(t, p, sheet && sheet->type == YJS_STRING ? sheet->u.s : nullptr);
            else load_table(t, p);
            T->tables.push_back(t);
        }
    }
    yjs_arena_free(&a);
}

// The view file's part: {"open": SOURCE, "tables": {SOURCE: {"widths":
// {COLUMN: px}, "scroll": [row, column]}}}.
void read_part(const dsg_view* v) {
    size_t n = 0;
    const char* json = dsg_view_part_get(v, "tables", &n);
    if (!json) return;
    yjs_arena a;
    yjs_value* root = nullptr;
    yjs_error e = {};
    yjs_arena_init_heap(&a, 0);
    if (yjs_parse(&a, json, n, nullptr, &root, &e) == YJS_OK && root->type == YJS_OBJECT) {
        const yjs_value* op = yjs_get(root, "open");
        const yjs_value* all = yjs_get(root, "tables");
        for (int i = 0; i < T->tables.Size; i++) {
            Table* t = T->tables[i];
            if (op && op->type == YJS_STRING && strcmp(op->u.s, t->source) == 0) T->open = i;
            const yjs_value* tv = all && all->type == YJS_OBJECT ? yjs_get(all, t->source) : nullptr;
            if (!tv || tv->type != YJS_OBJECT) continue;
            const yjs_value* ws = yjs_get(tv, "widths");
            for (uint32_t k = 0; ws && ws->type == YJS_OBJECT && k < ws->n; k++) {
                double px = 0;
                if (yjs_double(ws->u.members[k].value, &px) == YJS_OK && px >= 8 && px < 10000) set_width(t, ws->u.members[k].key, (float)px);
            }
            const yjs_value* sc = yjs_get(tv, "scroll");
            int64_t r = 0, c = 0;
            if (sc && sc->type == YJS_ARRAY && sc->n == 2 && yjs_int64(sc->u.items[0], &r) == YJS_OK &&
                yjs_int64(sc->u.items[1], &c) == YJS_OK && r >= 0 && c >= 0 && r < 1000000 && c < 1000) {
                t->scroll_row = (int)r;
                t->scroll_col = (int)c;
                t->restore = true;
            }
        }
    }
    yjs_arena_free(&a);
}

void free_tables() {
    if (!T) return;
    end_edit();
    for (Table* t : T->tables) {
        dsg_csv_free(&t->idx);
        free(t->sheet_csv);
        IM_DELETE(t);
    }
    T->tables.clear();
    T->open = 0;
    T->marks.clear();
    T->grid = nullptr;
}

// --- hooks -----------------------------------------------------------------

bool init(char* err, size_t cap) {
    T = IM_NEW(State)();
    T->cache = skb_layout_cache_create();
    if (!T->cache) {
        snprintf(err, cap, "out of memory for the tables' layout cache");
        return false;
    }
    return true;
}

void shutdown() {
    if (!T) return;
    free_tables();
    if (T->listening) dsg_doc_unlisten(ui_doc(), on_rename, nullptr);
    dsg_check_use_tables(nullptr);
    skb_layout_cache_destroy(T->cache);
    IM_DELETE(T);
    T = nullptr;
}

void opened(const dsg_view* v) {
    free_tables();
    const dsg_project* p = ui_project();
    if (!p) return;
    T->generation++;
    read_tables(p);
    read_part(v);
    // The check builds its pack from these buffers, so it sees an edit
    // before Save.
    dsg_check_use_tables(ui_doc());
    // the document keeps its listeners across projects
    if (!T->listening) T->listening = dsg_doc_listen(ui_doc(), on_rename, nullptr) == 0;
    T->sel_rev = dsg_doc_selection_rev(ui_doc());
    T->select_tab = true;
}

void closed() {
    free_tables();
    dsg_check_use_tables(nullptr);
}

void save_view(dsg_view* v) {
    if (!T || T->tables.Size == 0) return;
    yjs_arena a;
    yjs_arena_init_heap(&a, 0);
    yjs_value* root = yjs_new(&a, YJS_OBJECT);
    yjs_value* all = yjs_new(&a, YJS_OBJECT);
    if (const Table* o = open_table()) yjs_set(&a, root, "open", yjs_new_stringz(&a, o->source));
    for (const Table* t : T->tables) {
        yjs_value* tv = yjs_new(&a, YJS_OBJECT);
        yjs_value* ws = yjs_new(&a, YJS_OBJECT);
        for (const Width& w : t->widths) yjs_set(&a, ws, w.name, yjs_new_int(&a, (int64_t)lroundf(w.px)));
        yjs_value* sc = yjs_new(&a, YJS_ARRAY);
        yjs_push(&a, sc, yjs_new_int(&a, t->scroll_row));
        yjs_push(&a, sc, yjs_new_int(&a, t->scroll_col));
        yjs_set(&a, tv, "widths", ws);
        yjs_set(&a, tv, "scroll", sc);
        yjs_set(&a, all, t->source, tv);
    }
    yjs_set(&a, root, "tables", all);
    const size_t n = yjs_write_mem(root, YJS_WRITE_COMPACT, nullptr, 0);
    char* json = (char*)malloc(n + 1);
    if (json) {
        yjs_write_mem(root, YJS_WRITE_COMPACT, json, n + 1);
        dsg_view_part_set(v, "tables", json, n);
        free(json);
    }
    yjs_arena_free(&a);
}

int claims(const dsg_sel* s) {
    return s->buf > DSG_BUF_DEF && dsg_doc_buffer_kind(ui_doc(), s->buf) == DSG_KIND_TABLE ? 2 : 0;
}

void reveal(const dsg_sel* s) {
    if (!T || s->buf <= DSG_BUF_DEF) return;
    for (int i = 0; i < T->tables.Size; i++)
        if (T->tables[i]->buf == s->buf) {
            if (T->open != i) {
                commit_edit();
                T->select_tab = true;
            }
            T->open = i;
            T->scroll_to_sel = true;
        }
}

// --- drawing ---------------------------------------------------------------

// The issues of the last check that name a place in this table.
void collect_marks(const Table* t) {
    T->marks.resize(0);
    const dsg_report* r = ui_report();
    if (!r || !t->entry[0]) return;
    size_t n = 0;
    const char* b = table_bytes(t, &n);
    char entry[256], name[128], h[128];
    for (int i = 0; i < r->n_issues; i++) {
        int row = -2, col = -1;
        if (!dsg_csv_place(r->issues[i].message, entry, sizeof entry, &row, &col, name, sizeof name) || strcmp(entry, t->entry) != 0) continue;
        if (col < 0 && name[0])
            for (int c = 0; c < t->idx.n_cols; c++)
                if (dsg_csv_cell(&t->idx, b, -1, c, h, sizeof h) >= 0 && strcmp(h, name) == 0) col = c;
        Mark m = { row, col, i };
        T->marks.push_back(m);
    }
}

bool marked(int row, int col) {
    for (const Mark& m : T->marks) {
        if (m.row == row && m.col == col) return true;
        if (m.row == -2 && row == -1 && m.col == col) return true;   // a column: its name
        if (m.col < 0 && col < 0 && m.row == row) return true;       // a row: its number
    }
    return false;
}

enum Action { ADD_ROW, REMOVE_ROW, ADD_COLUMN, REMOVE_COLUMN };

void act(Table* t, Action what) {
    commit_edit();
    if (!sync(t)) return;
    const dsg_sel* s = dsg_doc_selection(ui_doc());
    const bool here = s->buf == t->buf;
    const int row = here ? s->row : -1, col = here ? s->col : -1;
    dsg_csv* c = &t->idx;
    dsg_doc* d = ui_doc();
    int rc = 0;
    switch (what) {
    case ADD_ROW: rc = row >= 0 ? dsg_csv_insert_row(d, c, row + 1, row) : dsg_csv_insert_row(d, c, c->n_rows, c->n_rows - 1); break;
    case REMOVE_ROW: rc = dsg_csv_remove_row(d, c, row); break;
    case ADD_COLUMN: rc = dsg_csv_insert_column(d, c, col >= 0 ? col + 1 : c->n_cols, nullptr); break;
    case REMOVE_COLUMN: rc = dsg_csv_remove_column(d, c, col); break;
    }
    if (rc) ui_set_status("Not done: %s", c->error);
    T->sel_rev = dsg_doc_selection_rev(d);
    T->scroll_to_sel = true;
}

void toolbar(Table* t) {
    const dsg_sel* s = dsg_doc_selection(ui_doc());
    const bool here = s->buf == t->buf && t->buf > 0;
    const bool can = !t->sheet && t->buf > 0 && t->idx.ok;
    ImGui::BeginDisabled(!can || t->idx.n_cols == 0);
    if (ImGui::Button("Add row")) act(t, ADD_ROW);
    ImGui::SetItemTooltip("A copy of the selected row, below it; with no row selected, of the last row, at the end.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!here || s->row < 0);
    if (ImGui::Button("Remove row")) act(t, REMOVE_ROW);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Add column")) act(t, ADD_COLUMN);
    ImGui::SetItemTooltip("An empty column after the selected one. Double-click its name to rename it.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!here || s->col < 0 || t->idx.n_cols < 2);
    if (ImGui::Button("Remove column")) act(t, REMOVE_COLUMN);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (t->sheet)
        ImGui::TextDisabled("%s: an Excel workbook, read-only. Edit it in Excel.", t->source);
    else if (t->buf > 0)
        ImGui::TextDisabled("%d rows, %d columns; %s; %s%s", t->idx.n_rows, t->idx.n_cols,
                            t->delim == '\t' ? "tabs" : t->delim == ';' ? "';'" : "','", t->idx.crlf ? "CRLF" : "LF",
                            dsg_doc_dirty(ui_doc(), t->buf) ? "; not saved" : "");
}

// The issues that name this table, each a link to its cell.
void issue_list(Table* t) {
    const dsg_report* r = ui_report();
    if (!r || T->marks.Size == 0) return;
    ImGui::PushID("marks");
    for (int i = 0; i < T->marks.Size && i < 4; i++) {
        const Mark& m = T->marks[i];
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 100, 255));
        if (ImGui::Selectable(r->issues[m.issue].message, false)) {
            select_cell(t, m.row >= -1 ? m.row : -1, m.col);
            T->scroll_to_sel = true;
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::PopID();
}

// Keys on the selected cell, when the grid has the focus and no cell is
// edited: arrows move, Enter or F2 edits, Delete empties, typing replaces.
void keys(Table* t, int ti, bool focused) {
    if (!focused || T->field || t->sheet || !t->idx.ok) return;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.ActiveId != 0 && g.InputTextState.ID == g.ActiveId) return;
    const dsg_sel* s = dsg_doc_selection(ui_doc());
    if (s->buf != t->buf) return;
    int row = s->row, col = s->col;
    const int r0 = row, c0 = col;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) row = ImMax(-1, row - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) row = ImMin(t->idx.n_rows - 1, row + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) col = ImMax(0, col - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) col = ImMin(t->idx.n_cols - 1, col + 1);
    if (row != r0 || col != c0) {
        select_cell(t, row, col);
        T->scroll_to_sel = true;
        return;
    }
    if (col < 0) return;
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        begin_edit(ti, row, col, true);
    } else if (row >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        if (dsg_csv_set_cell(ui_doc(), &t->idx, row, col, "", 0)) ui_set_status("Not edited: %s", t->idx.error);
    } else if (row >= 0 && !io.KeyCtrl && !io.InputQueueCharacters.empty()) {
        // the field reads the queued characters when it draws, this frame
        begin_edit(ti, row, col, false);
    }
}

void grid(Table* t, int ti) {
    const dsg_csv* c = &t->idx;
    size_t nb = 0;
    const char* bytes = table_bytes(t, &nb);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float row_h = ImCeil(ui_text_size() * 1.75f);
    const float cell_h = row_h - 2 * st.CellPadding.y;
    char num[16];
    snprintf(num, sizeof num, "%d", c->n_rows);
    const float num_w = ImGui::CalcTextSize(num).x + 12;
    const dsg_sel* sel = dsg_doc_selection(ui_doc());
    const bool sel_here = sel->buf == t->buf && t->buf > 0;
    T->cell.resize(kCellCap);
    char* cell = T->cell.Data;

    // No saved settings: the view file keeps widths by name, ImGui by index.
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_SizingFixedFit;
    char id[32];
    snprintf(id, sizeof id, "##grid%d.%u", ti, T->generation);
    if (!ImGui::BeginTable(id, c->n_cols + 1, flags, ImVec2(-1, -1))) return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("##row", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, num_w);
    for (int j = 0; j < c->n_cols; j++) {
        dsg_csv_cell(c, bytes, -1, j, cell, kCellCap);
        const float* w = width_slot(t, cell);
        ImGui::PushID(j);
        ImGui::TableSetupColumn("##col", ImGuiTableColumnFlags_WidthFixed, w ? *w : kDefaultWidth);
        ImGui::PopID();
    }
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImGuiTable* tb = ImGui::GetCurrentTable();
    T->grid = win;
    T->grid_table = tb->ID;
    T->grid_seed = win->IDStack.back();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The scroll: put back after an open, or the selection brought into view.
    const float view_h = win->InnerRect.GetHeight() - row_h;
    if (t->restore) {
        t->restore = false;
        ImGui::SetScrollY((float)t->scroll_row * row_h);
        float x = 0;
        for (int j = 0; j < t->scroll_col && j < c->n_cols; j++) x += tb->Columns[j + 1].WidthGiven + st.CellPadding.x * 2 + 1;
        ImGui::SetScrollX(x);
    } else if (T->scroll_to_sel && sel_here) {
        T->scroll_to_sel = false;
        if (sel->row >= 0) {
            const float y = (float)sel->row * row_h;
            if (y < win->Scroll.y || y + row_h > win->Scroll.y + view_h) ImGui::SetScrollY(ImMax(0.0f, y - view_h * 0.5f));
        }
        if (sel->col >= 0 && sel->col < c->n_cols) {
            const ImGuiTableColumn& col = tb->Columns[sel->col + 1];
            const float left = tb->Columns[0].MaxX;
            if (col.MinX < left) ImGui::SetScrollX(ImMax(0.0f, win->Scroll.x - (left - col.MinX)));
            else if (col.MaxX > win->InnerRect.Max.x) ImGui::SetScrollX(win->Scroll.x + (col.MaxX - win->InnerRect.Max.x));
        }
    }

    // A cell: an item to click, its text, and its marks.
    auto cell_item = [&](int row, int col, const char* text, int len, bool header) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
        const bool editing = T->field && T->edit_table == ti && T->edit_row == row && T->edit_col == col;
        ImGui::PushID(row);
        ImGui::PushID(col);
        if (editing) {
            skf_draw("##edit", T->field, w, cell_h);
            T->field_rect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        } else {
            ImGui::InvisibleButton("cell", ImVec2(w, cell_h));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) select_cell(t, row, col);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && col >= 0) begin_edit(ti, row, col, true);
            const bool is_sel = sel_here && ((sel->row == row && sel->col == col) || (sel->row == row && sel->col < 0 && row >= 0) ||
                                             (sel->col == col && sel->row < 0 && col >= 0));
            if (is_sel) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + cell_h), IM_COL32(50, 80, 130, 255));
            if (marked(row, col)) dl->AddRect(p, ImVec2(p.x + w, p.y + cell_h), IM_COL32(255, 110, 100, 255), 0.0f, 2.0f);
            draw_text(dl, text, len, p, w, cell_h, header ? IM_COL32(200, 210, 255, 255) : IM_COL32(230, 230, 235, 255));
        }
        ImGui::PopID();
        ImGui::PopID();
    };

    // the header: the columns' names
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers, row_h);
    ImGui::TableNextColumn();
    for (int j = 0; j < c->n_cols; j++) {
        if (!ImGui::TableNextColumn()) continue;
        const int len = dsg_csv_cell(c, bytes, -1, j, cell, kCellCap);
        cell_item(-1, j, cell, len, true);
    }

    ImGuiListClipper clip;
    clip.Begin(c->n_rows, row_h);
    T->first_row = T->last_row = -1;
    while (clip.Step()) {
        if (clip.DisplayStart < clip.DisplayEnd) {
            if (T->first_row < 0 || clip.DisplayStart < T->first_row) T->first_row = clip.DisplayStart;
            if (clip.DisplayEnd - 1 > T->last_row) T->last_row = clip.DisplayEnd - 1;
        }
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            ImGui::TableNextRow(0, row_h);
            ImGui::TableNextColumn();
            {
                // the row number, as ysp/table.h counts rows: selects the row
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::PushID(i);
                ImGui::PushID(-1);
                ImGui::InvisibleButton("cell", ImVec2(ImMax(ImGui::GetContentRegionAvail().x, 1.0f), cell_h));
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) select_cell(t, i, -1);
                ImGui::PopID();
                ImGui::PopID();
                char rn[16];
                snprintf(rn, sizeof rn, "%d", i + 1);
                dl->AddText(ImVec2(p.x + 4, p.y + (cell_h - ImGui::GetFontSize()) * 0.5f),
                            marked(i, -1) ? IM_COL32(255, 110, 100, 255) : IM_COL32(150, 150, 160, 255), rn);
            }
            for (int j = 0; j < c->n_cols; j++) {
                if (!ImGui::TableNextColumn()) continue;   // scrolled out of view
                const int len = dsg_csv_cell(c, bytes, i, j, cell, kCellCap);
                cell_item(i, j, cell, len, false);
            }
        }
    }

    // Widths by name, as the person left them; the scroll as the first row
    // and column shown.
    for (int j = 0; j < c->n_cols; j++) {
        const float w = tb->Columns[j + 1].WidthRequest;
        if (w <= 0) continue;
        dsg_csv_cell(c, bytes, -1, j, cell, kCellCap);
        const float* old = width_slot(t, cell);
        if (old ? fabsf(w - *old) > 0.5f : fabsf(w - kDefaultWidth) > 0.5f) set_width(t, cell, w);
    }
    t->scroll_row = (int)(win->Scroll.y / row_h);
    float x = 0;
    int k = 0;
    while (k < c->n_cols && x + tb->Columns[k + 1].WidthGiven * 0.5f < win->Scroll.x) {
        x += tb->Columns[k + 1].WidthGiven + st.CellPadding.x * 2 + 1;
        k++;
    }
    t->scroll_col = k;
    ImGui::EndTable();
}

void draw() {
    dsg_doc* d = ui_doc();
    if (T->tables.Size == 0) {
        ImGui::TextDisabled("This project has no tables: a table is a resource of kind \"table\" in %s.", DSG_DESC_FILE);
        return;
    }
    T->layouts = 0;
    skb_layout_cache_compact(T->cache);
    const int frame = ImGui::GetFrameCount();
    // The view was not drawn: the field had no keys, and its edit ends.
    if (T->field && T->last_frame != frame - 1) commit_edit();
    T->last_frame = frame;
    // A click outside the field ends the edit before the click acts.
    if (T->field && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !T->field_rect.Contains(ImGui::GetIO().MousePos)) commit_edit();
    if (T->field && (!skf_focused(T->field) || skf_focus_field() != T->field)) commit_edit();

    // Follow the selection: undo, redo and an issue put it on a cell.
    const uint64_t sr = dsg_doc_selection_rev(d);
    if (sr != T->sel_rev) {
        T->sel_rev = sr;
        reveal(dsg_doc_selection(d));
    }

    if (T->tables.Size > 1 && ImGui::BeginTabBar("##tables")) {
        for (int i = 0; i < T->tables.Size; i++) {
            const ImGuiTabItemFlags f = T->select_tab && T->open == i ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(T->tables[i]->source, nullptr, f)) {
                if (!T->select_tab && T->open != i) {
                    commit_edit();
                    T->open = i;
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    T->select_tab = false;
    Table* t = open_table();
    if (!t) return;
    const int ti = T->open;
    if (t->note[0]) {
        ImGui::TextWrapped("%s", t->note);
        return;
    }
    sync(t);
    collect_marks(t);
    toolbar(t);
    if (!t->idx.ok) {
        ImGui::TextWrapped("%s does not read as CSV: %s. Fix it in a text editor; the grid shows it when it reads.", t->source,
                           t->idx.error);
        return;
    }
    issue_list(t);
    // Enter keeps the text and Escape drops it; the field itself takes
    // neither. The Enter that starts an edit does not end it.
    const bool editing = T->field != nullptr;
    keys(t, ti, ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows));
    if (editing && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) end_edit();
    const bool enter = editing && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
    grid(t, ti);
    if (enter && T->field) commit_edit();
    T->layouts_last = T->layouts;
}

} // namespace

int view_tables_layouts_last_frame() { return T ? T->layouts_last : 0; }

ImGuiID view_tables_cell_id(int row, int col) {
    if (!T || !T->grid) return 0;
    // as PushID(row), PushID(col), then the item "cell"
    const ImGuiID r = ImHashData(&row, sizeof row, T->grid_seed);
    const ImGuiID c = ImHashData(&col, sizeof col, r);
    return ImHashStr("cell", 0, c);
}

void view_tables_rows_last_frame(int* first, int* last) {
    *first = T ? T->first_row : -1;
    *last = T ? T->last_row : -1;
}

ImGuiWindow* view_tables_grid_window() { return T ? T->grid : nullptr; }
ImGuiID view_tables_table_id() { return T ? T->grid_table : 0; }

float view_tables_width(const char* name) {
    Table* t = open_table();
    if (!t) return 0;
    const float* w = width_slot(t, name);
    return w ? *w : 0;
}

SkfField* view_tables_field() { return T ? T->field : nullptr; }

extern const UiView ui_view_tables = {
    "tables", "Tables", UI_SLOT_MAIN,
    init, shutdown, opened, closed, save_view,
    draw, claims, reveal, nullptr,
};
