// view_chart.cpp - the state chart (step 4, part A; docs/designer.md, "The
// chart"). OWNED BY PART A.
//
// One template's states, the end and the edges between them, drawn with
// ImGui's draw list and edited through the document (chart.h): every edit is
// one transaction, so one undo step. Positions, pan and zoom are view state:
// the view file's "chart" part, keyed by template and state name, never the
// definition (format D13). State names and edge labels are user text, so
// Skribidi lays them out (skt.h).
#include "views.h"
#include "chart.h"
#include "skf.h"
#include "skt.h"

#include "imgui.h"
#include "imgui_internal.h"   // BeginComboPreview, SetKeyOwner

// skb_common.h tests __cplusplus__ (sic), so in C++ its SKB_LITERAL() makes a
// C compound literal, which MSVC refuses (C4576); skt.cpp does the same.
#include "skribidi/skb_layout.h"
#undef SKB_LITERAL
#define SKB_LITERAL(type) type

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// Chart units: px at zoom 1.
const float kBoxW = 150, kBoxMaxW = 320, kBoxH = 58, kEndR = 24, kPortR = 7;
const float kNameSize = 16, kLabelSize = 13;
const float kZoomMin = 0.25f, kZoomMax = 4.0f;

const ImU32 kBg = IM_COL32(28, 30, 34, 255), kGrid = IM_COL32(42, 45, 51, 255);
const ImU32 kBox = IM_COL32(50, 56, 68, 255), kFirst = IM_COL32(40, 72, 92, 255);
const ImU32 kBorder = IM_COL32(110, 120, 140, 255), kHover = IM_COL32(170, 180, 200, 255);
const ImU32 kSel = IM_COL32(255, 200, 80, 255), kText = IM_COL32(235, 235, 240, 255), kSub = IM_COL32(170, 175, 185, 255);
const ImU32 kEdge = IM_COL32(200, 200, 210, 255), kNext = IM_COL32(130, 130, 140, 255), kShared = IM_COL32(120, 170, 230, 255);
const ImU32 kOver = IM_COL32(200, 140, 220, 255), kLabel = IM_COL32(230, 220, 160, 255);
const ImU32 kErr = IM_COL32(255, 110, 100, 255), kWarn = IM_COL32(240, 200, 90, 255);

enum ActKind { ACT_NONE, ACT_ADD, ACT_CONNECT, ACT_DELETE_STATE, ACT_DELETE_EXIT, ACT_RENAME, ACT_FIRST, ACT_SELECT };

// An edit waits for the end of the frame's drawing: a command changes the
// document and, through the rename listener, the positions this frame reads.
struct Action {
    ActKind kind = ACT_NONE;
    int state = -1;
    char text[DSG_SEL_PATH] = "";   // a name, or a path
    ImVec2 at;                      // ACT_ADD: where the new box goes
};

struct EdgeGeom {
    ImVec2 a, c1, c2, b;   // the curve, screen px
    ImVec2 lmin, lmax;     // the label's box
};

struct Chart {
    dsg_chart_view view;
    dsg_chart model = {};
    bool have_model = false;
    uint64_t model_rev = 0;
    char tmpl[DSG_SEL_PATH] = "";       // the template of the model
    uint64_t view_gen = ~0ull;          // view.gen the cache was resolved at
    ImVector<int> pos_idx;              // per node (states, then the end): its
                                        // entry in the template's positions
    ImVector<ImVec2> pos;               // per node: top left, chart units
    ImVector<float> width;              // per state: the box's width
    ImVector<int> node_sev;             // per node: the worst issue, -1 none
    ImVector<int> node_issues;
    ImVector<int> edge_sev;             // per edge
    ImVector<EdgeGeom> geom;
    ImVector<char> scratch;
    int drag = -1;                      // the node being moved
    int connect = -1;                   // the state an edge is dragged from
    SkfField* rename = nullptr;         // the name field, while renaming
    int renaming = -1;
    uint64_t rename_sel = 0;            // the selection's revision at its start
    ImVec2 canvas_size = ImVec2(600, 400);
    char center[DSG_SEL_PATH] = "";     // a path to bring into view
    dsg_chart_tview events_view = {};   // an events template's pan and zoom:
                                        // not kept, as its positions are not
    Action act;
};
Chart* C = nullptr;

const char* trial_name(const yjs_value* trials, uint32_t i) { return trials->u.members[i].key; }

// The template to show: the chosen one when it is there, else the first with
// states, else the first.
const char* pick_template(const yjs_value* trials) {
    if (!trials || trials->type != YJS_OBJECT || trials->n == 0) return nullptr;
    if (C->view.current && yjs_get(trials, C->view.current)) return C->view.current;
    for (uint32_t i = 0; i < trials->n; i++)
        if (yjs_get(trials->u.members[i].value, "states")) return trial_name(trials, i);
    return trial_name(trials, 0);
}

int n_nodes() { return C->have_model ? C->model.n_states + 1 : 0; }
const char* node_name(int i) { return i < C->model.n_states ? C->model.states[i].name : DSG_CHART_END; }
bool editable(bool stale) { return C->have_model && C->model.has_states && !stale; }

// A grid of 4 columns; odd columns sit lower, so an edge that skips a state
// does not run through it.
ImVec2 default_pos(int i) { return ImVec2(40.0f + (float)(i % 4) * 230.0f, 40.0f + (float)(i / 4) * 150.0f + (float)(i % 2) * 40.0f); }

// Every node gets a position: the stored one, else the default, stored so
// that it stays when other states come and go. Events templates are
// read-only and keep no positions.
void resolve_positions() {
    const int n = n_nodes();
    C->pos.resize(n);
    C->pos_idx.resize(n);
    if (!C->model.has_states) {
        for (int i = 0; i < n; i++) {
            C->pos[i] = default_pos(i);
            C->pos_idx[i] = -1;
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        float x = 0, y = 0;
        if (!dsg_chart_view_pos(&C->view, C->tmpl, node_name(i), &x, &y)) {
            const ImVec2 p = default_pos(i);
            dsg_chart_view_set_pos(&C->view, C->tmpl, node_name(i), p.x, p.y);
        }
    }
    // indices only after every entry exists: an added entry can move them
    dsg_chart_tview* t = dsg_chart_view_template(&C->view, C->tmpl, true);
    for (int i = 0; i < n; i++) {
        C->pos_idx[i] = -1;
        for (int k = 0; t && k < t->n_pos; k++)
            if (strcmp(t->pos[k].name, node_name(i)) == 0) C->pos_idx[i] = k;
    }
    C->view_gen = C->view.gen;
}

void read_positions() {
    if (C->view_gen != C->view.gen) resolve_positions();
    dsg_chart_tview* t = C->model.has_states ? dsg_chart_view_template(&C->view, C->tmpl, true) : nullptr;
    for (int i = 0; t && i < n_nodes(); i++)
        if (C->pos_idx[i] >= 0) C->pos[i] = ImVec2(t->pos[C->pos_idx[i]].x, t->pos[C->pos_idx[i]].y);
}

void write_position(int i) {
    dsg_chart_tview* t = C->model.has_states ? dsg_chart_view_template(&C->view, C->tmpl, false) : nullptr;
    if (t && C->pos_idx[i] >= 0 && C->pos_idx[i] < t->n_pos) {
        t->pos[C->pos_idx[i]].x = C->pos[i].x;
        t->pos[C->pos_idx[i]].y = C->pos[i].y;
    }
}

void rebuild(const yjs_value* root, const char* tmpl) {
    snprintf(C->tmpl, sizeof C->tmpl, "%s", tmpl);
    C->have_model = dsg_chart_build(root, tmpl, &C->model) == 0;
    C->model_rev = dsg_doc_rev(ui_doc(), DSG_BUF_DEF);
    C->view_gen = ~0ull;
    C->drag = C->connect = -1;
    C->width.resize(C->model.n_states);
    for (int i = 0; i < C->model.n_states; i++) {
        const skb_layout_t* l = skt_layout(C->model.states[i].name, kNameSize);
        const float w = l ? skb_layout_get_bounds(l).width : 0.0f;
        C->width[i] = ImClamp(w + 24.0f, kBoxW, kBoxMaxW);
    }
}

void end_rename() {
    if (C->rename) skf_destroy(C->rename);   // also drops the keyboard focus
    C->rename = nullptr;
    C->renaming = -1;
}

void start_rename(int i) {
    if (i < 0 || i >= C->model.n_states) return;
    end_rename();
    C->rename = skf_create(ui_text_size(), false);
    C->renaming = i;
    const char* nm = C->model.states[i].name;
    skf_set_text(C->rename, nm, (int)strlen(nm));
    skf_set_caret(C->rename, (int)dsg_codepoints_before(nm, strlen(nm), (uint32_t)strlen(nm)));
    C->rename_sel = dsg_doc_selection_rev(ui_doc());
}

void select_path(const char* path) {
    dsg_sel s;
    dsg_sel_def(&s, path);
    dsg_doc_select(ui_doc(), &s);
}

void select_state(int i) {
    char p[DSG_SEL_PATH];
    if (dsg_chart_state_path(&C->model, i, p, sizeof p) == 0) select_path(p);
}

void select_template() {
    char p[DSG_SEL_PATH];
    if (dsg_chart_template_path(C->tmpl, p, sizeof p) == 0) select_path(p);
}

// The selected state, or -1; *edge_place the selected exit, if one is.
int selected_state(dsg_chart_place* pl) {
    const dsg_sel* s = dsg_doc_selection(ui_doc());
    pl->state = pl->kind = pl->index = -1;
    if (s->buf != DSG_BUF_DEF || !dsg_chart_locate(&C->model, s->path, pl)) return -1;
    return pl->kind < 0 ? pl->state : -1;
}

// The selected edge's path (an exit, a shared exit, `over`), or "".
void selected_edge_path(const dsg_chart_place* pl, char* out, size_t cap) {
    out[0] = 0;
    for (int i = 0; pl->kind >= 0 && i < C->model.n_edges; i++)
        if (dsg_chart_place_has_edge(pl, &C->model.edges[i])) {
            dsg_chart_edge_path(&C->model, &C->model.edges[i], out, cap);
            return;
        }
}

// A name no state has: "state", "state2", ...
void fresh_name(char* out, size_t cap) {
    for (int k = 1; k < 100000; k++) {
        if (k == 1) snprintf(out, cap, "state");
        else snprintf(out, cap, "state%d", k);
        if (dsg_chart_find(&C->model, out) < 0) return;
    }
}

void run_action(bool stale) {
    Action a = C->act;
    C->act.kind = ACT_NONE;
    if (a.kind == ACT_NONE) return;
    if (a.kind == ACT_SELECT) {
        if (a.text[0]) select_path(a.text);
        else select_template();
        return;
    }
    if (!editable(stale)) return;
    dsg_doc* d = ui_doc();
    int rc = 0;
    const char* verb = "";
    switch (a.kind) {
    case ACT_ADD:
        verb = "added";
        rc = dsg_chart_add_state(d, C->tmpl, a.text);
        if (rc == 0) dsg_chart_view_set_pos(&C->view, C->tmpl, a.text, a.at.x, a.at.y);
        break;
    case ACT_CONNECT: verb = "connected"; rc = dsg_chart_connect(d, C->tmpl, a.state, a.text); break;
    case ACT_DELETE_STATE: verb = "deleted"; rc = dsg_chart_delete_state(d, C->tmpl, a.state); break;
    case ACT_DELETE_EXIT: verb = "deleted"; rc = dsg_chart_delete_exit(d, a.text); break;
    case ACT_RENAME:
        verb = "renamed";
        rc = dsg_chart_rename_state(d, C->tmpl, a.state, a.text);
        if (rc == 0) end_rename();
        break;
    case ACT_FIRST: verb = "set first"; rc = dsg_chart_set_first(d, C->tmpl, a.state); break;
    default: break;
    }
    if (rc) ui_set_status("Not %s: %s", verb, dsg_chart_error());
}

// --- issues -----------------------------------------------------------------

void mark_issues() {
    const int n = n_nodes();
    C->node_sev.resize(n);
    C->node_issues.resize(n);
    C->edge_sev.resize(C->model.n_edges);
    for (int i = 0; i < n; i++) C->node_sev[i] = -1, C->node_issues[i] = 0;
    for (int i = 0; i < C->model.n_edges; i++) C->edge_sev[i] = -1;
    const dsg_report* r = ui_report();
    for (int k = 0; r && k < r->n_issues; k++) {
        const char* p = ui_issue_path(k);
        dsg_chart_place pl = { -1, -1, -1 };
        if (!p[0] || !dsg_chart_locate(&C->model, p, &pl)) continue;
        const int sev = r->issues[k].severity;
        if (sev > DSG_SEV_WARNING) continue;   // a need or a note is not a fault
        if (pl.state >= 0) {
            C->node_issues[pl.state]++;
            if (C->node_sev[pl.state] < 0 || sev < C->node_sev[pl.state]) C->node_sev[pl.state] = sev;
        }
        for (int e = 0; pl.kind >= 0 && e < C->model.n_edges; e++)
            if (dsg_chart_place_has_edge(&pl, &C->model.edges[e]) && (C->edge_sev[e] < 0 || sev < C->edge_sev[e])) C->edge_sev[e] = sev;
    }
}

ImU32 sev_color(int sev) { return sev == DSG_SEV_ERROR ? kErr : kWarn; }

// --- geometry ---------------------------------------------------------------

struct Xf {
    ImVec2 o;   // screen px of chart 0, 0
    float z;
    ImVec2 to_screen(ImVec2 w) const { return ImVec2(o.x + w.x * z, o.y + w.y * z); }
    ImVec2 to_chart(ImVec2 s) const { return ImVec2((s.x - o.x) / z, (s.y - o.y) / z); }
};

ImVec2 node_size(int i) { return i < C->model.n_states ? ImVec2(C->width[i], kBoxH) : ImVec2(2 * kEndR, 2 * kEndR); }

ImVec2 node_center(const Xf& xf, int i) {
    const ImVec2 s = node_size(i);
    return xf.to_screen(ImVec2(C->pos[i].x + s.x * 0.5f, C->pos[i].y + s.y * 0.5f));
}

// Where the ray from the node's center toward p leaves the node.
ImVec2 node_edge(const Xf& xf, int i, ImVec2 p) {
    const ImVec2 c = node_center(xf, i);
    const ImVec2 d(p.x - c.x, p.y - c.y);
    if (i == C->model.n_states) {
        const float l = sqrtf(d.x * d.x + d.y * d.y);
        return l < 1e-3f ? c : ImVec2(c.x + d.x / l * kEndR * xf.z, c.y + d.y / l * kEndR * xf.z);
    }
    const ImVec2 h(node_size(i).x * 0.5f * xf.z, kBoxH * 0.5f * xf.z);
    const float sx = d.x != 0 ? h.x / fabsf(d.x) : 1e9f, sy = d.y != 0 ? h.y / fabsf(d.y) : 1e9f;
    const float s = ImMin(1.0f, ImMin(sx, sy));
    return ImVec2(c.x + d.x * s, c.y + d.y * s);
}

int node_at(const Xf& xf, ImVec2 m) {
    for (int i = n_nodes() - 1; i >= 0; i--) {
        const ImVec2 a = xf.to_screen(C->pos[i]), s = node_size(i);
        if (m.x >= a.x && m.y >= a.y && m.x < a.x + s.x * xf.z && m.y < a.y + s.y * xf.z) return i;
    }
    return -1;
}

ImVec2 bezier(const EdgeGeom& g, float t) {
    const float u = 1 - t;
    const float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
    return ImVec2(a * g.a.x + b * g.c1.x + c * g.c2.x + d * g.b.x, a * g.a.y + b * g.c1.y + c * g.c2.y + d * g.b.y);
}

// The edge whose curve passes within 5 px of m, or -1.
int edge_at(ImVec2 m) {
    for (int e = C->geom.Size - 1; e >= 0; e--) {
        ImVec2 prev = C->geom[e].a;
        for (int k = 1; k <= 16; k++) {
            const ImVec2 p = bezier(C->geom[e], (float)k / 16);
            const ImVec2 ab(p.x - prev.x, p.y - prev.y), am(m.x - prev.x, m.y - prev.y);
            const float l2 = ab.x * ab.x + ab.y * ab.y;
            const float t = l2 > 0 ? ImClamp((am.x * ab.x + am.y * ab.y) / l2, 0.0f, 1.0f) : 0.0f;
            const float dx = m.x - (prev.x + ab.x * t), dy = m.y - (prev.y + ab.y * t);
            if (dx * dx + dy * dy < 25) return e;
            prev = p;
        }
    }
    return -1;
}

void make_geometry(const Xf& xf) {
    C->geom.resize(C->model.n_edges);
    for (int e = 0; e < C->model.n_edges; e++) {
        const dsg_chart_edge& ed = C->model.edges[e];
        EdgeGeom& g = C->geom[e];
        const int to = ed.to >= 0 ? ed.to : -1;
        const ImVec2 ca = node_center(xf, ed.from);
        if (to == ed.from) {
            // a loop over the box's top
            const ImVec2 s = node_size(ed.from);
            const float hx = s.x * 0.25f * xf.z, top = ca.y - kBoxH * 0.5f * xf.z;
            const float up = (40.0f + 18.0f * (float)ed.slot) * xf.z;
            g.a = ImVec2(ca.x + hx, top);
            g.b = ImVec2(ca.x - hx, top);
            g.c1 = ImVec2(g.a.x + hx, top - up);
            g.c2 = ImVec2(g.b.x - hx, top - up);
        } else {
            // A `to` that names no state ends in a short stub, red.
            const ImVec2 cb = to >= 0 ? node_center(xf, to) : ImVec2(ca.x + 110 * xf.z, ca.y - 60 * xf.z);
            const ImVec2 d(cb.x - ca.x, cb.y - ca.y);
            const float len = ImMax(1.0f, sqrtf(d.x * d.x + d.y * d.y));
            const ImVec2 nrm(-d.y / len, d.x / len);
            // edges between one pair bow to alternate sides; a backward one bows more
            float bow = ed.slot == 0 ? 0.0f : (ed.slot % 2 ? 1.0f : -1.0f) * (float)((ed.slot + 1) / 2) * 36.0f;
            if (to >= 0 && to < ed.from) bow += 30.0f;
            bow *= xf.z;
            const ImVec2 mid((ca.x + cb.x) * 0.5f + nrm.x * bow, (ca.y + cb.y) * 0.5f + nrm.y * bow);
            g.a = node_edge(xf, ed.from, mid);
            g.b = to >= 0 ? node_edge(xf, to, mid) : cb;
            g.c1 = ImVec2(g.a.x + (mid.x - g.a.x) * 0.9f, g.a.y + (mid.y - g.a.y) * 0.9f);
            g.c2 = ImVec2(g.b.x + (mid.x - g.b.x) * 0.9f, g.b.y + (mid.y - g.b.y) * 0.9f);
        }
        const skb_layout_t* l = skt_layout(ed.label, kLabelSize);
        const skb_rect2_t lb = l ? skb_layout_get_bounds(l) : skb_rect2_t{};
        const ImVec2 at = bezier(g, 0.5f);
        const ImVec2 hs(lb.width * 0.5f * xf.z + 4, lb.height * 0.5f * xf.z + 2);
        g.lmin = ImVec2(at.x - hs.x, at.y - hs.y);
        g.lmax = ImVec2(at.x + hs.x, at.y + hs.y);
    }
}

// --- drawing ------------------------------------------------------------------

void draw_grid(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Xf& xf) {
    const float step = 50.0f * xf.z;
    if (step < 10) return;
    for (float x = fmodf(xf.o.x - p0.x, step); x < p1.x - p0.x; x += step)
        if (x >= 0) dl->AddLine(ImVec2(p0.x + x, p0.y), ImVec2(p0.x + x, p1.y), kGrid);
    for (float y = fmodf(xf.o.y - p0.y, step); y < p1.y - p0.y; y += step)
        if (y >= 0) dl->AddLine(ImVec2(p0.x, p0.y + y), ImVec2(p1.x, p0.y + y), kGrid);
}

void arrow_head(ImDrawList* dl, ImVec2 tip, ImVec2 from, float size, ImU32 col) {
    ImVec2 t(tip.x - from.x, tip.y - from.y);
    const float l = sqrtf(t.x * t.x + t.y * t.y);
    if (l < 1e-3f) return;
    t.x /= l;
    t.y /= l;
    dl->AddTriangleFilled(tip, ImVec2(tip.x - t.x * size - t.y * size * 0.5f, tip.y - t.y * size + t.x * size * 0.5f),
                          ImVec2(tip.x - t.x * size + t.y * size * 0.5f, tip.y - t.y * size - t.x * size * 0.5f), col);
}

ImU32 edge_color(const dsg_chart_edge& e) {
    switch (e.kind) {
    case DSG_EDGE_SHARED: return kShared;
    case DSG_EDGE_NEXT: return kNext;
    case DSG_EDGE_OVER: return kOver;
    default: return kEdge;
    }
}

void draw_edges(ImDrawList* dl, const Xf& xf, const dsg_chart_place& sel) {
    for (int e = 0; e < C->model.n_edges; e++) {
        const dsg_chart_edge& ed = C->model.edges[e];
        const EdgeGeom& g = C->geom[e];
        const bool picked = dsg_chart_place_has_edge(&sel, &ed);
        ImU32 col = ed.to < 0 ? kErr : edge_color(ed);
        if (C->edge_sev[e] >= 0) col = sev_color(C->edge_sev[e]);
        if (picked) col = kSel;
        const float th = (picked ? 2.5f : 1.5f) * ImMax(1.0f, xf.z);
        if (ed.kind == DSG_EDGE_NEXT) {
            // dashed: no exit, the steps' end
            ImVec2 prev = g.a;
            for (int k = 1; k <= 24; k++) {
                const ImVec2 p = bezier(g, (float)k / 24);
                if (k % 2) dl->AddLine(prev, p, col, th);
                prev = p;
            }
        } else {
            dl->AddBezierCubic(g.a, g.c1, g.c2, g.b, col, th);
        }
        arrow_head(dl, g.b, g.c2, 10 * xf.z, col);
        dl->AddRectFilled(g.lmin, g.lmax, IM_COL32(28, 30, 34, 225), 3);
        if (picked || C->edge_sev[e] >= 0) dl->AddRect(g.lmin, g.lmax, col, 3);
        const skb_layout_t* l = skt_layout(ed.label, kLabelSize);
        if (l) {
            const skb_rect2_t b = skb_layout_get_bounds(l);
            skt_draw_layout(dl, l, g.lmin.x + 4 - b.x * xf.z, g.lmin.y + 2 - b.y * xf.z, xf.z, ed.to < 0 ? kErr : kLabel);
        }
    }
}

void draw_nodes(ImDrawList* dl, const Xf& xf, int sel_state, int hovered) {
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * 0.8f * xf.z;
    const bool states = C->model.has_states;
    for (int i = 0; i < n_nodes(); i++) {
        const ImVec2 a = xf.to_screen(C->pos[i]);
        const ImVec2 s = node_size(i);
        const ImVec2 b(a.x + s.x * xf.z, a.y + s.y * xf.z);
        const bool sel = i == sel_state;
        ImU32 border = sel ? kSel : (i == hovered ? kHover : kBorder);
        if (C->node_sev[i] >= 0 && !sel) border = sev_color(C->node_sev[i]);
        if (i == C->model.n_states) {
            const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            dl->AddCircleFilled(c, kEndR * xf.z, IM_COL32(70, 50, 50, 255));
            dl->AddCircle(c, kEndR * xf.z, border, 0, 2);
            const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, "end");
            dl->AddText(font, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), kText, "end");
            continue;
        }
        const dsg_chart_state& st = C->model.states[i];
        dl->AddRectFilled(a, b, i == 0 && states ? kFirst : kBox, 6 * xf.z);
        dl->AddRect(a, b, border, 6 * xf.z, sel || C->node_sev[i] >= 0 ? 2.5f : 1.5f);
        if (i == 0) {
            // the start: an arrow into the first box
            const ImVec2 m(a.x, (a.y + b.y) * 0.5f);
            dl->AddLine(ImVec2(m.x - 22 * xf.z, m.y), m, kText, 2);
            arrow_head(dl, m, ImVec2(m.x - 22 * xf.z, m.y), 8 * xf.z, kText);
        }
        dl->PushClipRect(a, b, true);
        skt_draw_layout(dl, skt_layout(st.name, kNameSize), a.x + 10 * xf.z, a.y + 6 * xf.z, xf.z, kText);
        char sub[96];
        if (!states && st.step >= 0 && st.n_steps == 0 && st.n_exits) snprintf(sub, sizeof sub, "waits");
        else if (st.max_visits) snprintf(sub, sizeof sub, "%d steps, %d exits, max %d", st.n_steps, st.n_exits, st.max_visits);
        else snprintf(sub, sizeof sub, "%d steps, %d exits", st.n_steps, st.n_exits);
        dl->AddText(font, fs, ImVec2(a.x + 10 * xf.z, a.y + 32 * xf.z), kSub, sub);
        dl->PopClipRect();
        if (C->node_issues[i]) {
            char n[16];
            snprintf(n, sizeof n, "%d", C->node_issues[i]);
            const ImVec2 c(b.x - 2 * xf.z, a.y + 2 * xf.z);
            dl->AddCircleFilled(c, 9 * xf.z, sev_color(C->node_sev[i]));
            const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, n);
            dl->AddText(font, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(20, 20, 20, 255), n);
        }
        if (states && C->rename == nullptr) {
            const ImVec2 pc(b.x, (a.y + b.y) * 0.5f);
            dl->AddCircleFilled(pc, kPortR * xf.z, i == hovered ? kSel : IM_COL32(90, 100, 120, 255));
        }
    }
}

// --- the toolbar ------------------------------------------------------------------

void template_combo(const yjs_value* trials) {
    const float row = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("##template", "")) {
        for (uint32_t i = 0; i < trials->n; i++) {
            const char* name = trial_name(trials, i);
            ImGui::PushID((int)i);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##t", strcmp(name, C->tmpl) == 0, 0, ImVec2(0, row))) dsg_chart_view_set_current(&C->view, name);
            skt_draw_layout(ImGui::GetWindowDrawList(), skt_layout(name, ui_text_size()), at.x + 2, at.y + 1, 1.0f, kText);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginComboPreview()) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        skt_draw_layout(ImGui::GetWindowDrawList(), skt_layout(C->tmpl, ui_text_size()), at.x, at.y - 1, 1.0f, kText);
        ImGui::EndComboPreview();
    }
    ImGui::SetItemTooltip("The template whose states the chart shows");
}

void toolbar(const yjs_value* trials, bool stale, int sel_state, const char* sel_edge) {
    template_combo(trials);
    const bool edit = editable(stale);
    ImGui::SameLine();
    if (C->renaming >= 0 && C->rename) {
        ImGui::TextUnformatted("Name");
        ImGui::SameLine();
        skf_draw("##rename", C->rename, 220, ImGui::GetFrameHeight());
        const bool focused = skf_focused(C->rename);
        ImGui::SameLine();
        const bool ok = ImGui::Button("OK") || (focused && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)));
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel") || (focused && ImGui::IsKeyPressed(ImGuiKey_Escape, false));
        if (ok && C->act.kind == ACT_NONE) {
            skf_text(C->rename, &C->scratch);
            C->act.kind = ACT_RENAME;
            C->act.state = C->renaming;
            snprintf(C->act.text, sizeof C->act.text, "%s", C->scratch.Data);
        } else if (cancel) {
            end_rename();
        }
    } else {
        ImGui::BeginDisabled(!edit);
        if (ImGui::Button("Add state") && C->act.kind == ACT_NONE) {
            Xf xf = { ImVec2(0, 0), 1 };
            const dsg_chart_tview* t = dsg_chart_view_template(&C->view, C->tmpl, true);
            if (t) xf = { ImVec2(t->pan_x, t->pan_y), t->zoom };
            C->act.kind = ACT_ADD;
            fresh_name(C->act.text, sizeof C->act.text);
            const ImVec2 c = xf.to_chart(ImVec2(C->canvas_size.x * 0.5f, C->canvas_size.y * 0.5f));
            C->act.at = ImVec2(c.x - kBoxW * 0.5f, c.y - kBoxH * 0.5f);
        }
        ImGui::SetItemTooltip("A new state, at the middle of the chart");
        ImGui::BeginDisabled(sel_state < 0);
        ImGui::SameLine();
        if (ImGui::Button("Rename")) start_rename(sel_state);
        ImGui::SetItemTooltip("Rename the state and every name that refers to it (F2, or a double click)");
        ImGui::SameLine();
        if (ImGui::Button("Set first") && C->act.kind == ACT_NONE) {
            C->act.kind = ACT_FIRST;
            C->act.state = sel_state;
        }
        ImGui::SetItemTooltip("Move the state to the head of the list: the trial starts there");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(sel_state < 0 && !sel_edge[0]);
        if (ImGui::Button("Delete") && C->act.kind == ACT_NONE) {
            C->act.kind = sel_state >= 0 ? ACT_DELETE_STATE : ACT_DELETE_EXIT;
            C->act.state = sel_state;
            snprintf(C->act.text, sizeof C->act.text, "%s", sel_edge);
        }
        ImGui::SetItemTooltip("Delete the state with the exits that lead to it, or the exit (Delete)");
        ImGui::EndDisabled();
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    const dsg_chart_tview* t = dsg_chart_view_template(&C->view, C->tmpl, false);
    ImGui::TextDisabled("%.0f %%", (t ? t->zoom : 1.0f) * 100.0f);
    if (stale) ImGui::TextColored(ImVec4(1, 0.43f, 0.4f, 1), "The text does not parse: this is the last text that parsed, read-only.");
    else if (C->have_model && !C->model.has_states)
        ImGui::TextDisabled("This template uses events. The chart shows its parts, split at each until, read-only; edit it in the text view.");
}

// --- the canvas -------------------------------------------------------------------

void canvas(bool stale, int sel_state, const dsg_chart_place& sel) {
    ImGuiIO& io = ImGui::GetIO();
    dsg_chart_tview* t = C->model.has_states ? dsg_chart_view_template(&C->view, C->tmpl, true) : nullptr;
    if (!t) {
        t = &C->events_view;
        if (t->zoom <= 0) t->zoom = 1, t->pan_x = t->pan_y = 40;
    }
    const bool edit = editable(stale);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImGui::GetContentRegionAvail();
    sz.x = ImMax(sz.x, 50.0f);
    sz.y = ImMax(sz.y, 50.0f);
    C->canvas_size = sz;
    const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);

    // a reveal: the node into the middle of the canvas
    if (C->center[0]) {
        dsg_chart_place pl = { -1, -1, -1 };
        int node = dsg_chart_locate(&C->model, C->center, &pl) ? pl.state : -1;
        for (int e = 0; node < 0 && pl.kind >= 0 && e < C->model.n_edges; e++)
            if (dsg_chart_place_has_edge(&pl, &C->model.edges[e])) node = C->model.edges[e].from;
        if (node >= 0 && node < n_nodes()) {
            const ImVec2 s = node_size(node);
            t->pan_x = sz.x * 0.5f - (C->pos[node].x + s.x * 0.5f) * t->zoom;
            t->pan_y = sz.y * 0.5f - (C->pos[node].y + s.y * 0.5f) * t->zoom;
        }
        C->center[0] = 0;
    }

    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##canvas", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const ImGuiID canvas_id = ImGui::GetItemID();
    const bool canvas_activated = ImGui::IsItemActivated();
    const bool canvas_active = ImGui::IsItemActive();
    const bool in_canvas = ImGui::IsWindowHovered() && ImRect(p0, p1).Contains(io.MousePos);

    // the wheel zooms about the mouse: the chart point under it stays put
    if (in_canvas) ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, canvas_id);
    if (in_canvas && io.MouseWheel != 0) {
        const float nz = ImClamp(t->zoom * powf(1.15f, io.MouseWheel), kZoomMin, kZoomMax);
        const ImVec2 w((io.MousePos.x - p0.x - t->pan_x) / t->zoom, (io.MousePos.y - p0.y - t->pan_y) / t->zoom);
        t->pan_x = io.MousePos.x - p0.x - w.x * nz;
        t->pan_y = io.MousePos.y - p0.y - w.y * nz;
        t->zoom = nz;
    }
    Xf xf = { ImVec2(p0.x + t->pan_x, p0.y + t->pan_y), t->zoom };
    make_geometry(xf);

    // A click on empty space selects an edge under it, or the template; a
    // drag there pans.
    if (canvas_activated) {
        const int e = edge_at(io.MousePos);
        C->act.kind = ACT_SELECT;
        C->act.text[0] = 0;
        if (e >= 0) {
            dsg_chart_edge_path(&C->model, &C->model.edges[e], C->act.text, sizeof C->act.text);
            if (!C->act.text[0]) dsg_chart_state_path(&C->model, C->model.edges[e].from, C->act.text, sizeof C->act.text);
        }
    }
    if (canvas_active && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
        t->pan_x += io.MouseDelta.x;
        t->pan_y += io.MouseDelta.y;
        xf.o = ImVec2(p0.x + t->pan_x, p0.y + t->pan_y);
        make_geometry(xf);
    }

    // Items: edge labels, then boxes, then ports; a later item wins the hover.
    // They hover and click only inside the canvas, and they do not count as
    // the window's content: a box off the canvas makes no scroll bar.
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const ImVec2 content_max = win->DC.CursorMaxPos;
    ImGui::PushClipRect(p0, p1, true);
    ImGui::PushID("edges");
    for (int e = 0; e < C->model.n_edges; e++) {
        const EdgeGeom& g = C->geom[e];
        ImGui::PushID(e);
        ImGui::SetCursorScreenPos(g.lmin);
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##label", ImVec2(ImMax(1.0f, g.lmax.x - g.lmin.x), ImMax(1.0f, g.lmax.y - g.lmin.y)))) {
            C->act.kind = ACT_SELECT;
            dsg_chart_edge_path(&C->model, &C->model.edges[e], C->act.text, sizeof C->act.text);
            if (!C->act.text[0]) dsg_chart_state_path(&C->model, C->model.edges[e].from, C->act.text, sizeof C->act.text);
        }
        ImGui::PopID();
    }
    ImGui::PopID();

    int hovered = -1;
    for (int i = 0; i < n_nodes(); i++) {
        const ImVec2 a = xf.to_screen(C->pos[i]), s = node_size(i);
        ImGui::PushID(node_name(i));
        ImGui::SetCursorScreenPos(a);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton(i < C->model.n_states ? "##box" : "##end", ImVec2(s.x * xf.z, s.y * xf.z));
        if (ImGui::IsItemHovered()) hovered = i;
        if (ImGui::IsItemActivated()) {
            C->drag = i;
            C->act.kind = ACT_SELECT;
            if (i < C->model.n_states) dsg_chart_state_path(&C->model, i, C->act.text, sizeof C->act.text);
            else C->act.text[0] = 0;
        }
        if (ImGui::IsItemActive() && C->drag == i && C->model.has_states && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            C->pos[i].x += io.MouseDelta.x / xf.z;
            C->pos[i].y += io.MouseDelta.y / xf.z;
            write_position(i);
        }
        if (ImGui::IsItemDeactivated() && C->drag == i) C->drag = -1;
        if (i < C->model.n_states && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && edit)
            start_rename(i);
        if (edit && i < C->model.n_states && !C->rename) {
            const ImVec2 pc(a.x + s.x * xf.z, a.y + s.y * xf.z * 0.5f);
            const float r = ImMax(kPortR * xf.z, 5.0f);
            ImGui::SetCursorScreenPos(ImVec2(pc.x - r, pc.y - r));
            ImGui::InvisibleButton("##out", ImVec2(2 * r, 2 * r));
            ImGui::SetItemTooltip("Drag to a state or the end: a new exit");
            if (ImGui::IsItemHovered()) hovered = i;
            if (ImGui::IsItemActivated()) C->connect = i;
            if (ImGui::IsItemDeactivated() && C->connect == i) {
                const int to = node_at(xf, io.MousePos);
                if (to >= 0 && C->act.kind == ACT_NONE) {
                    C->act.kind = ACT_CONNECT;
                    C->act.state = i;
                    snprintf(C->act.text, sizeof C->act.text, "%s", node_name(to));
                }
                C->connect = -1;
            }
        }
        ImGui::PopID();
    }
    ImGui::PopClipRect();
    win->DC.CursorMaxPos = content_max;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, p1, true);
    dl->AddRectFilled(p0, p1, kBg);
    draw_grid(dl, p0, p1, xf);
    draw_edges(dl, xf, sel);
    draw_nodes(dl, xf, sel_state, hovered);
    if (C->connect >= 0) {
        const ImVec2 a = node_edge(xf, C->connect, io.MousePos);
        dl->AddLine(a, io.MousePos, kSel, 2);
        arrow_head(dl, io.MousePos, a, 10, kSel);
    }
    dl->PopClipRect();
}

// --- the view's hooks --------------------------------------------------------------

bool init(char*, size_t) {
    C = IM_NEW(Chart)();
    dsg_chart_view_init(&C->view);
    dsg_doc_listen(ui_doc(), dsg_chart_view_on_rename, &C->view);
    return true;
}

void shutdown() {
    if (!C) return;
    dsg_doc_unlisten(ui_doc(), dsg_chart_view_on_rename, &C->view);
    end_rename();
    dsg_chart_view_free(&C->view);
    dsg_chart_free(&C->model);
    IM_DELETE(C);
    C = nullptr;
}

void reset() {
    end_rename();
    dsg_chart_view_free(&C->view);
    C->have_model = false;
    C->tmpl[0] = 0;
    C->model_rev = 0;
    C->drag = C->connect = -1;
    C->center[0] = 0;
    C->act.kind = ACT_NONE;
}

void opened(const dsg_view* v) {
    reset();
    size_t n = 0;
    const char* part = dsg_view_part_get(v, "chart", &n);
    if (part && dsg_chart_view_read(&C->view, part, n)) ui_set_status("The chart's view state was not read; it starts empty.");
}

void closed() { reset(); }

void save_view(dsg_view* v) {
    size_t n = 0;
    char* json = dsg_chart_view_write(&C->view, &n);
    if (json) dsg_view_part_set(v, "chart", json, n);
    free(json);
}

// A state, an exit of a state or a shared exit of a template.
int claims(const dsg_sel* s) {
    if (s->buf != DSG_BUF_DEF || !dsg_path_within(s->path, "/trials") || s->path[7] != '/') return 0;
    const char* slash = strchr(s->path + 8, '/');   // after the template's name
    return slash && (dsg_path_within(slash, "/states") || dsg_path_within(slash, "/exits")) ? 2 : 0;
}

void reveal(const dsg_sel* s) {
    if (s->buf != DSG_BUF_DEF || !dsg_path_within(s->path, "/trials") || s->path[7] != '/') return;
    // the template's name: the token after /trials, unescaped
    char head[DSG_SEL_PATH], tmpl[DSG_SEL_PATH], rest[DSG_SEL_PATH];
    const char* slash = strchr(s->path + 8, '/');
    const size_t k = slash ? (size_t)(slash - s->path) : strlen(s->path);
    if (k >= sizeof head) return;
    memcpy(head, s->path, k);
    head[k] = 0;
    if (dsg_path_split(head, rest, sizeof rest, tmpl, sizeof tmpl)) return;
    dsg_chart_view_set_current(&C->view, tmpl);
    snprintf(C->center, sizeof C->center, "%s", s->path);
}

void draw() {
    bool stale = false;
    const yjs_value* root = dsg_doc_shown(ui_doc(), &stale);
    if (!root) {
        ImGui::TextDisabled("The definition has not parsed yet.");
        return;
    }
    const yjs_value* trials = yjs_get(root, "trials");
    const char* tmpl = pick_template(trials);
    if (!tmpl) {
        ImGui::TextDisabled("The definition has no templates (\"trials\").");
        return;
    }
    if (!C->have_model || strcmp(tmpl, C->tmpl) != 0 || C->model_rev != dsg_doc_rev(ui_doc(), DSG_BUF_DEF)) {
        char name[DSG_SEL_PATH];
        snprintf(name, sizeof name, "%s", tmpl);   // tmpl may be C->view.current
        if (strcmp(name, C->tmpl) != 0) end_rename();
        rebuild(root, name);
    }
    if (C->renaming >= C->model.n_states || (C->renaming >= 0 && dsg_doc_selection_rev(ui_doc()) != C->rename_sel)) end_rename();
    if (C->have_model) {
        read_positions();
        mark_issues();
    }

    dsg_chart_place sel = { -1, -1, -1 };
    const int sel_state = C->have_model ? selected_state(&sel) : -1;
    char sel_edge[DSG_SEL_PATH] = "";
    if (C->have_model) selected_edge_path(&sel, sel_edge, sizeof sel_edge);
    toolbar(trials, stale, sel_state, sel_edge);
    if (!C->have_model) {
        ImGui::TextDisabled("This template has neither states nor events.");
        run_action(stale);
        return;
    }
    canvas(stale, sel_state, sel);

    // keys, while the chart has the keyboard and no field does
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !C->rename && !ImGui::GetIO().WantTextInput && editable(stale) &&
        C->act.kind == ACT_NONE) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && (sel_state >= 0 || sel_edge[0])) {
            C->act.kind = sel_state >= 0 ? ACT_DELETE_STATE : ACT_DELETE_EXIT;
            C->act.state = sel_state;
            snprintf(C->act.text, sizeof C->act.text, "%s", sel_edge);
        } else if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && sel_state >= 0) {
            start_rename(sel_state);
        }
    }
    if (C->connect >= 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) C->connect = -1;
    run_action(stale);
}

} // namespace

extern const UiView ui_view_chart = {
    "chart", "Chart", UI_SLOT_MAIN,
    init, shutdown, opened, closed, save_view,
    draw, claims, reveal, nullptr, nullptr,
};
