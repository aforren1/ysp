/* chart.c - the chart model, its commands and its view state (chart.h).
 * OWNED BY PART A. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "chart.h"
#include "preview.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- helpers ------------------------------------------------------------------- */

static const char* str_of(const yjs_value* v) { return v && v->type == YJS_STRING ? v->u.s : NULL; }

static bool str_is(const yjs_value* v, const char* s) {
    const char* t = str_of(v);
    return t && strcmp(t, s) == 0;
}

static int count_of(const yjs_value* v) { return v && v->type == YJS_ARRAY ? (int)v->n : 0; }

/* A scalar as written: a string, or a number's own spelling. */
static const char* text_of(const yjs_value* v) {
    if (!v) return NULL;
    if (v->type == YJS_STRING || v->type == YJS_NUMBER) return v->u.s;
    if (v->type == YJS_TRUE) return "true";
    if (v->type == YJS_FALSE) return "false";
    return "...";
}

/* --- the model ------------------------------------------------------------------ */

/* Strings go into the pool as offsets while it grows, and become pointers
 * at the end of the build. */
typedef size_t pool_off;

static bool pool_add(dsg_chart* c, const char* s, size_t n, pool_off* off) {
    if (c->pool_n_ + n + 1 > c->pool_cap_) {
        size_t cap = c->pool_cap_ ? c->pool_cap_ : 1024;
        char* p;
        while (cap < c->pool_n_ + n + 1) cap *= 2;
        p = (char*)realloc(c->pool_, cap);
        if (!p) return false;
        c->pool_ = p;
        c->pool_cap_ = cap;
    }
    memcpy(c->pool_ + c->pool_n_, s, n);
    c->pool_[c->pool_n_ + n] = 0;
    *off = c->pool_n_;
    c->pool_n_ += n + 1;
    return true;
}

static bool pool_addz(dsg_chart* c, const char* s, pool_off* off) { return pool_add(c, s, strlen(s), off); }

/* Offsets ride in the pointer fields until the build ends. */
static const char* off_ptr(pool_off o) { return (const char*)(uintptr_t)o; }
static pool_off ptr_off(const char* p) { return (pool_off)(uintptr_t)p; }

static dsg_chart_state* new_state(dsg_chart* c) {
    dsg_chart_state* s;
    if (c->n_states == c->states_cap_) {
        int cap = c->states_cap_ ? c->states_cap_ * 2 : 16;
        dsg_chart_state* p = (dsg_chart_state*)realloc(c->states, (size_t)cap * sizeof *p);
        if (!p) return NULL;
        c->states = p;
        c->states_cap_ = cap;
    }
    s = &c->states[c->n_states++];
    memset(s, 0, sizeof *s);
    s->step = -1;
    return s;
}

static dsg_chart_edge* new_edge(dsg_chart* c, int kind, int from, int index) {
    dsg_chart_edge* e;
    if (c->n_edges == c->edges_cap_) {
        int cap = c->edges_cap_ ? c->edges_cap_ * 2 : 32;
        dsg_chart_edge* p = (dsg_chart_edge*)realloc(c->edges, (size_t)cap * sizeof *p);
        if (!p) return NULL;
        c->edges = p;
        c->edges_cap_ = cap;
    }
    e = &c->edges[c->n_edges++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->from = from;
    e->to = -1;
    e->index = index;
    e->in_index = -1;
    return e;
}

/* "on X, after T s, if C, outcome O": what an exit waits for and sets. */
static bool exit_label(dsg_chart* c, const yjs_value* ex, pool_off* off) {
    char b[512];
    size_t k = 0;
    const char* on = text_of(yjs_get(ex, "on"));
    const char* after = text_of(yjs_get(ex, "after_s"));
    const char* cond = text_of(yjs_get(ex, "if"));
    const char* out = text_of(yjs_get(ex, "outcome"));
    b[0] = 0;
    if (on) k += (size_t)snprintf(b + k, sizeof b - k, "on %s", on);
    if (after && k < sizeof b) k += (size_t)snprintf(b + k, sizeof b - k, "%safter %s s", k ? ", " : "", after);
    if (!on && !after && k < sizeof b) k += (size_t)snprintf(b + k, sizeof b - k, "instant");
    if (cond && k < sizeof b) k += (size_t)snprintf(b + k, sizeof b - k, ", if %s", cond);
    if (out && k < sizeof b) k += (size_t)snprintf(b + k, sizeof b - k, ", outcome %s", out);
    return pool_addz(c, b, off);
}

static int target(const dsg_chart* c, const char* to) {
    int i;
    if (!to) return -1;
    if (strcmp(to, DSG_CHART_END) == 0) return c->n_states;
    for (i = 0; i < c->n_states; i++)
        if (strcmp(c->pool_ + ptr_off(c->states[i].name), to) == 0) return i;
    return -1;
}

static bool build_states(dsg_chart* c, const yjs_value* t) {
    const yjs_value* states = yjs_get(t, "states");
    const yjs_value* shared = yjs_get(t, "exits");
    int n = count_of(states), i, k, j;
    pool_off off = 0;
    for (i = 0; i < n; i++) {
        const yjs_value* s = yjs_at(states, (size_t)i);
        const char* name = str_of(yjs_get(s, "name"));
        dsg_chart_state* st = new_state(c);
        int64_t mv = 0;
        if (!st || !pool_addz(c, name ? name : "", &off)) return false;
        st->name = off_ptr(off);
        st->n_steps = count_of(yjs_get(s, "steps"));
        st->n_exits = count_of(yjs_get(s, "exits"));
        if (yjs_int64(yjs_get(s, "max_visits"), &mv) == YJS_OK && mv > 0 && mv <= INT32_MAX) st->max_visits = (int)mv;
    }
    /* targets resolve against every state, so edges come after the states */
    for (i = 0; i < n; i++) {
        const yjs_value* s = yjs_at(states, (size_t)i);
        const yjs_value* exits = yjs_get(s, "exits");
        const yjs_value* over = yjs_get(s, "over");
        bool listed = false;
        for (k = 0; k < count_of(exits); k++) {
            const yjs_value* ex = yjs_at(exits, (size_t)k);
            const char* to = str_of(yjs_get(ex, "to"));
            dsg_chart_edge* e = new_edge(c, DSG_EDGE_EXIT, i, k);
            if (!e) return false;
            e->to = target(c, to);
            if (!exit_label(c, ex, &off)) return false;
            e->label = off_ptr(off);
            if (!pool_addz(c, to ? to : "", &off)) return false;
            e->to_name = off_ptr(off);
        }
        if (over && over->type == YJS_OBJECT) {
            char b[256];
            const char* to = str_of(yjs_get(over, "to"));
            const char* out = text_of(yjs_get(over, "outcome"));
            dsg_chart_edge* e = new_edge(c, DSG_EDGE_OVER, i, -1);
            if (!e) return false;
            e->to = target(c, to);
            snprintf(b, sizeof b, "over %d visits%s%s", c->states[i].max_visits, out ? ", outcome " : "", out ? out : "");
            if (!pool_addz(c, b, &off)) return false;
            e->label = off_ptr(off);
            if (!pool_addz(c, to ? to : "", &off)) return false;
            e->to_name = off_ptr(off);
        }
        /* rule 10, as the player counts it: a state in a shared exit's `in`
         * has exits (player/v2.c) */
        for (j = 0; j < count_of(shared) && !listed; j++) {
            const yjs_value* in = yjs_get(yjs_at(shared, (size_t)j), "in");
            for (k = 0; k < count_of(in) && !listed; k++)
                listed = str_is(yjs_at(in, (size_t)k), c->pool_ + ptr_off(c->states[i].name));
        }
        if (count_of(exits) == 0 && !listed) {
            dsg_chart_edge* e = new_edge(c, DSG_EDGE_NEXT, i, -1);
            if (!e) return false;
            e->to = i + 1;
            if (!pool_addz(c, "steps end", &off)) return false;
            e->label = off_ptr(off);
            e->to_name = off_ptr(0);
        }
    }
    for (j = 0; j < count_of(shared); j++) {
        const yjs_value* ex = yjs_at(shared, (size_t)j);
        const yjs_value* in = yjs_get(ex, "in");
        const char* to = str_of(yjs_get(ex, "to"));
        pool_off lab = 0, ton = 0;
        if (!exit_label(c, ex, &lab) || !pool_addz(c, to ? to : "", &ton)) return false;
        for (k = 0; k < count_of(in); k++) {
            int from = target(c, str_of(yjs_at(in, (size_t)k)));
            dsg_chart_edge* e;
            if (from < 0 || from == n) continue;   /* the check names an unknown `in` */
            e = new_edge(c, DSG_EDGE_SHARED, from, j);
            if (!e) return false;
            e->in_index = k;
            e->to = target(c, to);
            e->label = off_ptr(lab);
            e->to_name = off_ptr(ton);
        }
    }
    return true;
}

/* An events template: its parts split at each `until` (rule 9), shown
 * read-only. */
static bool build_events(dsg_chart* c, const yjs_value* t) {
    const yjs_value* ev = yjs_get(t, "events");
    int n = count_of(ev), k, p0 = 0;
    pool_off off = 0;
    char b[512];
    for (k = 0; k <= n; k++) {
        const yjs_value* st = k < n ? yjs_at(ev, (size_t)k) : NULL;
        const yjs_value* until = st ? yjs_get(st, "until") : NULL;
        if (k < n && !until) continue;
        if (k > p0 || (k == n && c->n_states == 0)) {
            dsg_chart_state* s = new_state(c);
            dsg_chart_edge* e;
            if (!s) return false;
            if (k - p0 > 1) snprintf(b, sizeof b, "steps %d to %d", p0 + 1, k);
            else if (k > p0) snprintf(b, sizeof b, "step %d", k);
            else snprintf(b, sizeof b, "no steps");
            if (!pool_addz(c, b, &off)) return false;
            s->name = off_ptr(off);
            s->n_steps = k - p0;
            s->step = p0;
            e = new_edge(c, DSG_EDGE_NEXT, c->n_states - 1, -1);
            if (!e || !pool_addz(c, "steps end", &off)) return false;
            e->to = c->n_states;   /* the next part; the end is fixed below */
            e->label = off_ptr(off);
            e->to_name = off_ptr(0);
        }
        if (until) {
            const char* cn = text_of(until);
            const char* tmo = text_of(yjs_get(st, "timeout_s"));
            dsg_chart_state* s = new_state(c);
            dsg_chart_edge* e;
            if (!s) return false;
            snprintf(b, sizeof b, "until %s", cn ? cn : "?");
            if (!pool_addz(c, b, &off)) return false;
            s->name = off_ptr(off);
            s->step = k;
            s->n_exits = tmo ? 2 : 1;
            e = new_edge(c, DSG_EDGE_EXIT, c->n_states - 1, k);
            snprintf(b, sizeof b, "on %s", cn ? cn : "?");
            if (!e || !pool_addz(c, b, &off)) return false;
            e->to = c->n_states;
            e->label = off_ptr(off);
            e->to_name = off_ptr(0);
            if (tmo) {
                e = new_edge(c, DSG_EDGE_EXIT, c->n_states - 1, k);
                snprintf(b, sizeof b, "after %s s, outcome timeout", tmo);
                if (!e || !pool_addz(c, b, &off)) return false;
                e->to = -2;   /* the end, fixed below */
                e->label = off_ptr(off);
                e->to_name = off_ptr(0);
            }
            p0 = k + 1;
        }
    }
    for (k = 0; k < c->n_edges; k++)
        if (c->edges[k].to == -2 || c->edges[k].to > c->n_states) c->edges[k].to = c->n_states;
    return true;
}

int dsg_chart_build(const yjs_value* root, const char* tmpl, dsg_chart* c) {
    const yjs_value* t = yjs_get(yjs_get(root, "trials"), tmpl);
    pool_off name_off = 0;
    bool ok;
    int i, j;
    c->n_states = c->n_edges = 0;
    c->pool_n_ = 0;
    c->tmpl = NULL;
    c->has_states = false;
    if (!t || t->type != YJS_OBJECT || (!yjs_get(t, "states") && !yjs_get(t, "events"))) return -1;
    /* offset 0 is "", for edges with no `to` */
    if (!pool_addz(c, "", &name_off) || !pool_addz(c, tmpl, &name_off)) return -1;
    c->has_states = yjs_get(t, "events") == NULL;
    ok = c->has_states ? build_states(c, t) : build_events(c, t);
    if (!ok) {
        c->n_states = c->n_edges = 0;
        return -1;
    }
    for (i = 0; i < c->n_edges; i++) {
        dsg_chart_edge* e = &c->edges[i];
        for (j = 0; j < i; j++) {
            const dsg_chart_edge* f = &c->edges[j];
            if ((f->from == e->from && f->to == e->to) || (f->from == e->to && f->to == e->from)) e->slot++;
        }
    }
    /* the pool is final: offsets become pointers */
    c->tmpl = c->pool_ + name_off;
    for (i = 0; i < c->n_states; i++) c->states[i].name = c->pool_ + ptr_off(c->states[i].name);
    for (i = 0; i < c->n_edges; i++) {
        c->edges[i].label = c->pool_ + ptr_off(c->edges[i].label);
        c->edges[i].to_name = c->pool_ + ptr_off(c->edges[i].to_name);
    }
    return 0;
}

void dsg_chart_free(dsg_chart* c) {
    free(c->states);
    free(c->edges);
    free(c->pool_);
    memset(c, 0, sizeof *c);
}

int dsg_chart_find(const dsg_chart* c, const char* name) {
    int i;
    for (i = 0; i < c->n_states; i++)
        if (strcmp(c->states[i].name, name) == 0) return i;
    return -1;
}

int dsg_chart_template_path(const char* tmpl, char* out, size_t cap) { return dsg_path_key(out, cap, "/trials", tmpl); }

int dsg_chart_state_path(const dsg_chart* c, int i, char* out, size_t cap) {
    char t[DSG_SEL_PATH], a[DSG_SEL_PATH];
    if (!c->tmpl || i < 0 || i >= c->n_states || dsg_chart_template_path(c->tmpl, t, sizeof t)) return -1;
    if (!c->has_states) return dsg_path_key(a, sizeof a, t, "events") || dsg_path_index(out, cap, a, c->states[i].step);
    return dsg_path_key(a, sizeof a, t, "states") || dsg_path_index(out, cap, a, i);
}

int dsg_chart_edge_path(const dsg_chart* c, const dsg_chart_edge* e, char* out, size_t cap) {
    char a[DSG_SEL_PATH], b[DSG_SEL_PATH];
    if (cap) out[0] = 0;
    if (e->kind == DSG_EDGE_NEXT) return 0;
    if (!c->tmpl || dsg_chart_template_path(c->tmpl, a, sizeof a)) return -1;
    if (!c->has_states) return dsg_path_key(b, sizeof b, a, "events") || dsg_path_index(out, cap, b, e->index);
    if (e->kind == DSG_EDGE_SHARED) return dsg_path_key(b, sizeof b, a, "exits") || dsg_path_index(out, cap, b, e->index);
    if (dsg_chart_state_path(c, e->from, b, sizeof b)) return -1;
    if (e->kind == DSG_EDGE_OVER) return dsg_path_key(out, cap, b, "over");
    return dsg_path_key(a, sizeof a, b, "exits") || dsg_path_index(out, cap, a, e->index);
}

/* The index that path's next token names after prefix, and the rest of the
 * path after it; -1 when it is not a number. */
static int index_after(const char* path, size_t plen, const char** rest) {
    const char* p = path + plen;
    long v = 0;
    if (*p != '/' || p[1] < '0' || p[1] > '9') return -1;
    p++;
    while (*p >= '0' && *p <= '9' && v < 1000000) v = v * 10 + (*p++ - '0');
    if (*p && *p != '/') return -1;
    *rest = p;
    return (int)v;
}

bool dsg_chart_locate(const dsg_chart* c, const char* path, dsg_chart_place* out) {
    char t[DSG_SEL_PATH], pre[DSG_SEL_PATH];
    const char* rest = "";
    int i;
    out->state = out->kind = out->index = -1;
    if (!c->tmpl || dsg_chart_template_path(c->tmpl, t, sizeof t)) return false;
    if (!c->has_states) {
        if (dsg_path_key(pre, sizeof pre, t, "events") || !dsg_path_within(path, pre)) return false;
        i = index_after(path, strlen(pre), &rest);
        for (int s = 0; s < c->n_states && i >= 0; s++)
            if (c->states[s].step <= i) out->state = s;
        return out->state >= 0;
    }
    if (dsg_path_key(pre, sizeof pre, t, "states") == 0 && dsg_path_within(path, pre)) {
        i = index_after(path, strlen(pre), &rest);
        if (i < 0 || i >= c->n_states) return false;
        out->state = i;
        if (dsg_path_within(rest, "/over")) out->kind = DSG_EDGE_OVER;
        else if (dsg_path_within(rest, "/exits")) {
            int k = index_after(rest, 6, &rest);
            if (k >= 0) {
                out->kind = DSG_EDGE_EXIT;
                out->index = k;
            }
        }
        return true;
    }
    if (dsg_path_key(pre, sizeof pre, t, "exits") == 0 && dsg_path_within(path, pre)) {
        i = index_after(path, strlen(pre), &rest);
        if (i < 0) return false;
        out->kind = DSG_EDGE_SHARED;
        out->index = i;
        return true;
    }
    return false;
}

bool dsg_chart_place_has_edge(const dsg_chart_place* p, const dsg_chart_edge* e) {
    if (p->kind != e->kind) return false;
    switch (e->kind) {
    case DSG_EDGE_EXIT: return e->from == p->state && e->index == p->index;
    case DSG_EDGE_SHARED: return e->index == p->index;
    case DSG_EDGE_OVER: return e->from == p->state;
    default: return false;
    }
}

/* --- the preview's place ------------------------------------------------------- */

bool dsg_chart_running(const dsg_chart* c, const dsg_preview_where* w, int* first, int* last) {
    int i = -1, j = -1;
    *first = *last = -1;
    if (!w || !w->tmpl || w->phase == DSG_PV_PHASE_BETWEEN || !c->tmpl || strcmp(w->tmpl, c->tmpl) != 0) return false;
    if (c->has_states) {
        /* a state's name is unique in its template, so the first match is it */
        i = j = w->state ? dsg_chart_find(c, w->state) : c->n_states;
    } else if (w->phase == DSG_PV_PHASE_AFTER) {
        i = j = c->n_states;
    } else if (c->n_states > 0) {
        i = 0;
        j = c->n_states - 1;
    }
    if (i < 0) return false;
    *first = i;
    *last = j;
    return true;
}

const char* dsg_chart_follow(const yjs_value* root, const dsg_preview_where* w) {
    const yjs_value* trials = root ? yjs_get(root, "trials") : NULL;
    uint32_t i;
    if (!w || !w->tmpl || !trials || trials->type != YJS_OBJECT) return NULL;
    for (i = 0; i < trials->n; i++)
        if (strcmp(trials->u.members[i].key, w->tmpl) == 0) return trials->u.members[i].key;
    return NULL;
}

/* --- commands ------------------------------------------------------------------- */

static char chart_err[512];

static int refuse(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(chart_err, sizeof chart_err, fmt, ap);
    va_end(ap);
    return -1;
}

const char* dsg_chart_error(void) { return chart_err; }

/* Ends a transaction: commit, or cancel with the document's reason. */
static int finish(dsg_doc* d, int rc) {
    if (rc) {
        if (!chart_err[0]) snprintf(chart_err, sizeof chart_err, "%s", dsg_doc_error(d));
        dsg_doc_cancel(d);
        return -1;
    }
    if (dsg_doc_commit(d)) return refuse("%s", dsg_doc_error(d));
    return 0;
}

/* The template at its path, with `states` and no `events`. */
static const yjs_value* states_of(dsg_doc* d, const char* tmpl, char* tpath, size_t cap) {
    const yjs_value* t;
    const yjs_error* pe = NULL;
    if (dsg_chart_template_path(tmpl, tpath, cap)) { refuse("the template's name is too long"); return NULL; }
    if (!dsg_def_get(d, "") && (pe = dsg_doc_parse_error(d)) != NULL) {
        refuse("the definition's text does not parse (line %u, column %u: %s): fix it in the text view", pe->line, pe->column, pe->msg);
        return NULL;
    }
    t = dsg_def_get(d, tpath);
    if (!t || t->type != YJS_OBJECT) { refuse("no template '%s'", tmpl); return NULL; }
    if (yjs_get(t, "events")) { refuse("'%s' uses events: edit it in the text view", tmpl); return NULL; }
    return yjs_get(t, "states");
}

static bool name_ok(const yjs_value* states, const char* name, int skip) {
    int i;
    if (!name || !name[0]) return refuse("a state needs a name") == 0;
    if (strcmp(name, DSG_CHART_END) == 0 || strcmp(name, "response") == 0)
        return refuse("'%s' is reserved in a template with states (format 3.9)", name) == 0;
    if (!yjs_utf8_ok(name, strlen(name))) return refuse("the name is not UTF-8 text") == 0;
    for (i = 0; i < count_of(states); i++)
        if (i != skip && str_is(yjs_get(yjs_at(states, (size_t)i), "name"), name))
            return refuse("a state is named '%s' already", name) == 0;
    return true;
}

static void select_path(dsg_doc* d, const char* path) {
    dsg_sel s;
    dsg_sel_def(&s, path);
    dsg_doc_select(d, &s);
}

int dsg_chart_add_state(dsg_doc* d, const char* tmpl, const char* name) {
    char t[DSG_SEL_PATH], sp[DSG_SEL_PATH], p[DSG_SEL_PATH], np[DSG_SEL_PATH];
    const yjs_value* states;
    int n, rc = -1;
    chart_err[0] = 0;
    dsg_doc_begin(d, "Add state");
    states = states_of(d, tmpl, t, sizeof t);
    if (chart_err[0] || !name_ok(states, name, -1)) return finish(d, -1);
    n = count_of(states);
    if (dsg_path_key(sp, sizeof sp, t, "states") || dsg_path_index(p, sizeof p, sp, n) || dsg_path_key(np, sizeof np, p, "name"))
        return finish(d, refuse("the path is too long"));
    if ((states && states->type == YJS_ARRAY) || dsg_def_set(d, sp, "[]", 2) == 0) {
        char app[DSG_SEL_PATH];
        if (dsg_path_key(app, sizeof app, sp, "-") == 0 && dsg_def_set(d, app, "{}", 2) == 0 && dsg_def_set_string(d, np, name) == 0) {
            select_path(d, p);
            rc = 0;
        }
    }
    return finish(d, rc);
}

int dsg_chart_connect(dsg_doc* d, const char* tmpl, int from, const char* to) {
    char t[DSG_SEL_PATH], sp[DSG_SEL_PATH], s[DSG_SEL_PATH], ep[DSG_SEL_PATH], p[DSG_SEL_PATH], tp[DSG_SEL_PATH];
    const yjs_value* states;
    const yjs_value* exits;
    int n, rc = -1, i;
    bool known = to && strcmp(to, DSG_CHART_END) == 0;
    chart_err[0] = 0;
    dsg_doc_begin(d, "Connect");
    states = states_of(d, tmpl, t, sizeof t);
    if (chart_err[0]) return finish(d, -1);
    if (from < 0 || from >= count_of(states)) return finish(d, refuse("no state %d in '%s'", from, tmpl));
    for (i = 0; to && !known && i < count_of(states); i++) known = str_is(yjs_get(yjs_at(states, (size_t)i), "name"), to);
    if (!known) return finish(d, refuse("no state '%s' to go to", to ? to : ""));
    exits = yjs_get(yjs_at(states, (size_t)from), "exits");
    n = count_of(exits);
    if (dsg_path_key(sp, sizeof sp, t, "states") || dsg_path_index(s, sizeof s, sp, from) || dsg_path_key(ep, sizeof ep, s, "exits") ||
        dsg_path_index(p, sizeof p, ep, n) || dsg_path_key(tp, sizeof tp, p, "to"))
        return finish(d, refuse("the path is too long"));
    if ((exits && exits->type == YJS_ARRAY) || dsg_def_set(d, ep, "[]", 2) == 0) {
        char app[DSG_SEL_PATH];
        if (dsg_path_key(app, sizeof app, ep, "-") == 0 && dsg_def_set(d, app, "{}", 2) == 0 && dsg_def_set_string(d, tp, to) == 0) {
            select_path(d, p);
            rc = 0;
        }
    }
    return finish(d, rc);
}

static int rm_key(dsg_doc* d, const char* base, const char* key) {
    char a[DSG_SEL_PATH];
    if (dsg_path_key(a, sizeof a, base, key)) return refuse("the path is too long");
    return dsg_def_remove(d, a);
}

static int rm_index(dsg_doc* d, const char* base, int index) {
    char a[DSG_SEL_PATH];
    if (dsg_path_index(a, sizeof a, base, index)) return refuse("the path is too long");
    return dsg_def_remove(d, a);
}

/* The reads in the commands below see the tree as it was at begin: the
 * first edit makes the transaction's own copy, and each array is cut from
 * its last item to its first, so the indices read stay right. */

/* Removes the exits of state i (at path s) that go to name, and the array
 * when it empties; and `over` with its `max_visits` when it goes to name. */
static int drop_exits_to(dsg_doc* d, const yjs_value* st, const char* s, const char* name) {
    const yjs_value* exits = yjs_get(st, "exits");
    char ep[DSG_SEL_PATH];
    int n = count_of(exits), left = n, k, rc = 0;
    if (dsg_path_key(ep, sizeof ep, s, "exits")) return refuse("the path is too long");
    for (k = n - 1; k >= 0 && rc == 0; k--)
        if (str_is(yjs_get(yjs_at(exits, (size_t)k), "to"), name)) {
            rc = rm_index(d, ep, k);
            left--;
        }
    if (rc == 0 && n > 0 && left == 0) rc = dsg_def_remove(d, ep);
    if (rc == 0 && str_is(yjs_get(yjs_get(st, "over"), "to"), name)) {
        rc = rm_key(d, s, "over");
        if (rc == 0 && yjs_get(st, "max_visits")) rc = rm_key(d, s, "max_visits");
    }
    return rc;
}

/* Removes the shared exits that go to name, name from the others' `in`, a
 * shared exit whose `in` empties (the loader refuses an empty `in`), and
 * the array when it empties. */
static int drop_shared(dsg_doc* d, const yjs_value* shared, const char* ex, const char* name) {
    int ns = count_of(shared), left = ns, j, m, rc = 0;
    for (j = ns - 1; j >= 0 && rc == 0; j--) {
        const yjs_value* e = yjs_at(shared, (size_t)j);
        const yjs_value* in = yjs_get(e, "in");
        char ej[DSG_SEL_PATH], ip[DSG_SEL_PATH];
        int nin = count_of(in), in_left = nin;
        if (dsg_path_index(ej, sizeof ej, ex, j) || dsg_path_key(ip, sizeof ip, ej, "in")) return refuse("the path is too long");
        if (str_is(yjs_get(e, "to"), name)) {
            rc = dsg_def_remove(d, ej);
            left--;
            continue;
        }
        for (m = nin - 1; m >= 0 && rc == 0; m--)
            if (str_is(yjs_at(in, (size_t)m), name)) {
                rc = rm_index(d, ip, m);
                in_left--;
            }
        if (rc == 0 && nin > 0 && in_left == 0) {
            rc = dsg_def_remove(d, ej);
            left--;
        }
    }
    if (rc == 0 && ns > 0 && left == 0) rc = dsg_def_remove(d, ex);
    return rc;
}

int dsg_chart_delete_state(dsg_doc* d, const char* tmpl, int state) {
    char t[DSG_SEL_PATH], sp[DSG_SEL_PATH], s[DSG_SEL_PATH], name[DSG_SEL_PATH], ex[DSG_SEL_PATH];
    const yjs_value* states;
    const yjs_value* shared;
    const char* nm;
    int i, rc = 0;
    chart_err[0] = 0;
    dsg_doc_begin(d, "Delete state");
    states = states_of(d, tmpl, t, sizeof t);
    if (chart_err[0]) return finish(d, -1);
    if (state < 0 || state >= count_of(states)) return finish(d, refuse("no state %d in '%s'", state, tmpl));
    shared = yjs_get(dsg_def_get(d, t), "exits");
    nm = str_of(yjs_get(yjs_at(states, (size_t)state), "name"));
    snprintf(name, sizeof name, "%s", nm ? nm : "");
    if (dsg_path_key(sp, sizeof sp, t, "states") || dsg_path_key(ex, sizeof ex, t, "exits"))
        return finish(d, refuse("the path is too long"));
    for (i = count_of(states) - 1; name[0] && i >= 0 && rc == 0; i--) {
        if (i == state) continue;
        if (dsg_path_index(s, sizeof s, sp, i)) return finish(d, refuse("the path is too long"));
        rc = drop_exits_to(d, yjs_at(states, (size_t)i), s, name);
    }
    if (rc == 0 && name[0]) rc = drop_shared(d, shared, ex, name);
    if (rc == 0) rc = rm_index(d, sp, state);
    if (rc == 0) select_path(d, t);
    return finish(d, rc);
}

int dsg_chart_delete_exit(dsg_doc* d, const char* exit_path) {
    char parent[DSG_SEL_PATH], last[DSG_SEL_PATH], gp[DSG_SEL_PATH], arr[DSG_SEL_PATH], t[DSG_SEL_PATH], tk[DSG_SEL_PATH];
    const yjs_value* v;
    int rc = 0;
    chart_err[0] = 0;
    dsg_doc_begin(d, "Delete exit");
    v = dsg_def_get(d, exit_path);
    if (!v || v->type != YJS_OBJECT || !dsg_path_within(exit_path, "/trials") || dsg_path_split(exit_path, parent, sizeof parent, last, sizeof last))
        return finish(d, refuse("'%s' is not an exit", exit_path));
    /* the template's path: the first two tokens */
    {
        const char* p = exit_path + 8;
        const char* slash = strchr(p, '/');
        size_t k = slash ? (size_t)(slash - exit_path) : strlen(exit_path);
        if (k >= sizeof t) return finish(d, refuse("the path is too long"));
        memcpy(t, exit_path, k);
        t[k] = 0;
    }
    if (strcmp(last, "over") == 0) {
        rc = dsg_def_remove(d, exit_path);
        if (rc == 0 && yjs_get(dsg_def_get(d, parent), "max_visits")) rc = rm_key(d, parent, "max_visits");
    } else {
        if (dsg_path_split(parent, gp, sizeof gp, tk, sizeof tk) || strcmp(tk, "exits") != 0)
            return finish(d, refuse("'%s' is not an exit", exit_path));
        snprintf(arr, sizeof arr, "%s", parent);
        rc = dsg_def_remove(d, exit_path);
        if (rc == 0 && count_of(dsg_def_get(d, arr)) == 0) rc = dsg_def_remove(d, arr);
    }
    if (rc == 0) select_path(d, t);
    return finish(d, rc);
}

/* One path in a list of paths to rewrite. */
typedef struct path_list {
    char (*p)[DSG_SEL_PATH];
    int n, cap;
} path_list;

static int add_path(path_list* l, const char* base, const char* key, int index) {
    char a[DSG_SEL_PATH];
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        char (*np)[DSG_SEL_PATH] = (char (*)[DSG_SEL_PATH])realloc(l->p, (size_t)cap * sizeof *np);
        if (!np) return refuse("out of memory");
        l->p = np;
        l->cap = cap;
    }
    if (key && dsg_path_key(index >= 0 ? a : l->p[l->n], index >= 0 ? sizeof a : sizeof l->p[l->n], base, key))
        return refuse("the path is too long");
    if (index >= 0 && dsg_path_index(l->p[l->n], sizeof l->p[l->n], key ? a : base, index)) return refuse("the path is too long");
    l->n++;
    return 0;
}

/* The members of obj (controls, columns) whose from, to or time is name. */
static int refs_in_members(path_list* l, const yjs_value* obj, const char* base, const char* name) {
    static const char* const keys[] = { "from", "to", "time" };
    uint32_t i;
    size_t k;
    if (!obj || obj->type != YJS_OBJECT) return 0;
    for (i = 0; i < obj->n; i++) {
        char m[DSG_SEL_PATH];
        if (dsg_path_key(m, sizeof m, base, obj->u.members[i].key)) return refuse("the path is too long");
        for (k = 0; k < sizeof keys / sizeof keys[0]; k++)
            if (str_is(yjs_get(obj->u.members[i].value, keys[k]), name) && add_path(l, m, keys[k], -1)) return -1;
    }
    return 0;
}

static int collect_refs(path_list* l, const yjs_value* tv, const char* t, const char* name) {
    const yjs_value* states = yjs_get(tv, "states");
    const yjs_value* shared = yjs_get(tv, "exits");
    char a[DSG_SEL_PATH], s[DSG_SEL_PATH], e[DSG_SEL_PATH], b[DSG_SEL_PATH];
    int i, k;
    if (dsg_path_key(a, sizeof a, t, "states")) return refuse("the path is too long");
    for (i = 0; i < count_of(states); i++) {
        const yjs_value* st = yjs_at(states, (size_t)i);
        const yjs_value* exits = yjs_get(st, "exits");
        if (dsg_path_index(s, sizeof s, a, i) || dsg_path_key(e, sizeof e, s, "exits")) return refuse("the path is too long");
        for (k = 0; k < count_of(exits); k++) {
            if (!str_is(yjs_get(yjs_at(exits, (size_t)k), "to"), name)) continue;
            if (dsg_path_index(b, sizeof b, e, k) || add_path(l, b, "to", -1)) return -1;
        }
        if (str_is(yjs_get(yjs_get(st, "over"), "to"), name)) {
            if (dsg_path_key(b, sizeof b, s, "over") || add_path(l, b, "to", -1)) return -1;
        }
    }
    if (dsg_path_key(a, sizeof a, t, "exits")) return refuse("the path is too long");
    for (i = 0; i < count_of(shared); i++) {
        const yjs_value* ex = yjs_at(shared, (size_t)i);
        const yjs_value* in = yjs_get(ex, "in");
        if (dsg_path_index(e, sizeof e, a, i)) return refuse("the path is too long");
        if (str_is(yjs_get(ex, "to"), name) && add_path(l, e, "to", -1)) return -1;
        for (k = 0; k < count_of(in); k++)
            if (str_is(yjs_at(in, (size_t)k), name) && add_path(l, e, "in", k)) return -1;
    }
    if (dsg_path_key(a, sizeof a, t, "controls") || refs_in_members(l, yjs_get(tv, "controls"), a, name)) return -1;
    if (dsg_path_key(a, sizeof a, t, "columns") || refs_in_members(l, yjs_get(tv, "columns"), a, name)) return -1;
    return 0;
}

int dsg_chart_rename_state(dsg_doc* d, const char* tmpl, int state, const char* name) {
    char t[DSG_SEL_PATH], sp[DSG_SEL_PATH], s[DSG_SEL_PATH], old[DSG_SEL_PATH];
    const yjs_value* states;
    const char* nm;
    path_list l = { NULL, 0, 0 };
    int i, rc = 0;
    chart_err[0] = 0;
    dsg_doc_begin(d, "Rename state");
    states = states_of(d, tmpl, t, sizeof t);
    if (chart_err[0]) return finish(d, -1);
    if (state < 0 || state >= count_of(states)) return finish(d, refuse("no state %d in '%s'", state, tmpl));
    nm = str_of(yjs_get(yjs_at(states, (size_t)state), "name"));
    snprintf(old, sizeof old, "%s", nm ? nm : "");
    if (nm && name && strcmp(nm, name) == 0) return finish(d, 0);
    if (!name_ok(states, name, state)) return finish(d, -1);
    if (dsg_path_key(sp, sizeof sp, t, "states") || dsg_path_index(s, sizeof s, sp, state)) return finish(d, refuse("the path is too long"));
    if (old[0]) rc = collect_refs(&l, dsg_def_get(d, t), t, old);
    if (rc == 0 && add_path(&l, s, "name", -1)) rc = -1;
    for (i = 0; i < l.n && rc == 0; i++) rc = dsg_def_set_string(d, l.p[i], name);
    free(l.p);
    if (rc == 0 && old[0]) rc = dsg_doc_note_rename(d, sp, old, name);
    if (rc == 0) select_path(d, s);
    return finish(d, rc);
}

int dsg_chart_set_first(dsg_doc* d, const char* tmpl, int state) {
    char t[DSG_SEL_PATH], sp[DSG_SEL_PATH], s[DSG_SEL_PATH];
    const yjs_value* states;
    int rc;
    chart_err[0] = 0;
    dsg_doc_begin(d, "Set first state");
    states = states_of(d, tmpl, t, sizeof t);
    if (chart_err[0]) return finish(d, -1);
    if (state < 0 || state >= count_of(states)) return finish(d, refuse("no state %d in '%s'", state, tmpl));
    if (dsg_path_key(sp, sizeof sp, t, "states") || dsg_path_index(s, sizeof s, sp, 0)) return finish(d, refuse("the path is too long"));
    rc = state == 0 ? 0 : dsg_def_move(d, sp, state, 0);
    if (rc == 0 && state != 0) select_path(d, s);
    return finish(d, rc);
}

/* --- the view state ----------------------------------------------------------------- */

static char* dupz(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

void dsg_chart_view_init(dsg_chart_view* v) { memset(v, 0, sizeof *v); }

void dsg_chart_view_free(dsg_chart_view* v) {
    int i, k;
    uint64_t gen = v->gen;
    for (i = 0; i < v->n_t; i++) {
        for (k = 0; k < v->t[i].n_pos; k++) free(v->t[i].pos[k].name);
        free(v->t[i].pos);
        free(v->t[i].name);
    }
    free(v->t);
    free(v->current);
    memset(v, 0, sizeof *v);
    v->gen = gen + 1;
}

static int find_t(const dsg_chart_view* v, const char* tmpl) {
    int i;
    for (i = 0; i < v->n_t; i++)
        if (strcmp(v->t[i].name, tmpl) == 0) return i;
    return -1;
}

static int find_pos(const dsg_chart_tview* t, const char* state) {
    int k;
    for (k = 0; k < t->n_pos; k++)
        if (strcmp(t->pos[k].name, state) == 0) return k;
    return -1;
}

int dsg_chart_view_set_current(dsg_chart_view* v, const char* tmpl) {
    char* p;
    if (v->current && tmpl && strcmp(v->current, tmpl) == 0) return 0;
    p = tmpl ? dupz(tmpl) : NULL;
    if (tmpl && !p) return -1;
    free(v->current);
    v->current = p;
    return 0;
}

dsg_chart_tview* dsg_chart_view_template(dsg_chart_view* v, const char* tmpl, bool make) {
    int i = find_t(v, tmpl);
    dsg_chart_tview* t;
    if (i >= 0) return &v->t[i];
    if (!make) return NULL;
    if (v->n_t == v->cap_t) {
        int cap = v->cap_t ? v->cap_t * 2 : 8;
        dsg_chart_tview* p = (dsg_chart_tview*)realloc(v->t, (size_t)cap * sizeof *p);
        if (!p) return NULL;
        v->t = p;
        v->cap_t = cap;
    }
    t = &v->t[v->n_t];
    memset(t, 0, sizeof *t);
    t->name = dupz(tmpl);
    if (!t->name) return NULL;
    t->pan_x = t->pan_y = 40.0f;
    t->zoom = 1.0f;
    v->n_t++;
    v->gen++;
    return t;
}

bool dsg_chart_view_pos(const dsg_chart_view* v, const char* tmpl, const char* state, float* x, float* y) {
    int i = find_t(v, tmpl), k = -1;
    if (i < 0 || (k = find_pos(&v->t[i], state)) < 0) return false;
    *x = v->t[i].pos[k].x;
    *y = v->t[i].pos[k].y;
    return true;
}

int dsg_chart_view_set_pos(dsg_chart_view* v, const char* tmpl, const char* state, float x, float y) {
    dsg_chart_tview* t = dsg_chart_view_template(v, tmpl, true);
    int k;
    if (!t) return -1;
    k = find_pos(t, state);
    if (k < 0) {
        if (t->n_pos == t->cap_pos) {
            int cap = t->cap_pos ? t->cap_pos * 2 : 16;
            dsg_chart_pos* p = (dsg_chart_pos*)realloc(t->pos, (size_t)cap * sizeof *p);
            if (!p) return -1;
            t->pos = p;
            t->cap_pos = cap;
        }
        k = t->n_pos;
        t->pos[k].name = dupz(state);
        if (!t->pos[k].name) return -1;
        t->n_pos++;
        v->gen++;
    }
    t->pos[k].x = x;
    t->pos[k].y = y;
    return 0;
}

/* Renames entry from to `to` in a list of named entries, dropping one that
 * had the name `to` already. */
static void rename_pos(dsg_chart_tview* t, const char* from, const char* to) {
    int a = find_pos(t, from), b = find_pos(t, to);
    char* p;
    if (a < 0) return;
    if (b >= 0) {
        free(t->pos[b].name);
        t->pos[b] = t->pos[--t->n_pos];
        if (a == t->n_pos) a = b;
    }
    p = dupz(to);
    if (!p) return;
    free(t->pos[a].name);
    t->pos[a].name = p;
}

void dsg_chart_view_on_rename(void* ctx, const char* scope, const char* from, const char* to) {
    dsg_chart_view* v = (dsg_chart_view*)ctx;
    char parent[DSG_SEL_PATH], last[DSG_SEL_PATH], top[DSG_SEL_PATH], tmpl[DSG_SEL_PATH];
    if (strcmp(scope, "/trials") == 0) {
        int a = find_t(v, from), b = find_t(v, to);
        char* p;
        if (v->current && strcmp(v->current, from) == 0) dsg_chart_view_set_current(v, to);
        if (a < 0) return;
        if (b >= 0) {
            dsg_chart_tview* t = &v->t[b];
            int k;
            for (k = 0; k < t->n_pos; k++) free(t->pos[k].name);
            free(t->pos);
            free(t->name);
            v->t[b] = v->t[--v->n_t];
            if (a == v->n_t) a = b;
        }
        p = dupz(to);
        if (!p) return;
        free(v->t[a].name);
        v->t[a].name = p;
        v->gen++;
        return;
    }
    if (dsg_path_split(scope, parent, sizeof parent, last, sizeof last) || strcmp(last, "states") != 0 ||
        dsg_path_split(parent, top, sizeof top, tmpl, sizeof tmpl) || strcmp(top, "/trials") != 0)
        return;
    {
        int i = find_t(v, tmpl);
        if (i < 0) return;
        rename_pos(&v->t[i], from, to);
        v->gen++;
    }
}

static const yjs_value* num2(const yjs_value* a, double* x, double* y) {
    if (count_of(a) != 2 || yjs_double(yjs_at(a, 0), x) != YJS_OK || yjs_double(yjs_at(a, 1), y) != YJS_OK) return NULL;
    if (!isfinite(*x) || !isfinite(*y) || fabs(*x) > 1e7 || fabs(*y) > 1e7) return NULL;
    return a;
}

int dsg_chart_view_read(dsg_chart_view* v, const char* json, size_t n) {
    yjs_arena a;
    yjs_value* root = NULL;
    yjs_error e;
    const yjs_value* ts;
    memset(&e, 0, sizeof e);
    uint32_t i, k;
    yjs_arena_init_heap(&a, (size_t)64 << 20);
    if (yjs_parse(&a, json, n, NULL, &root, &e) != YJS_OK || root->type != YJS_OBJECT) {
        yjs_arena_free(&a);
        return -1;
    }
    if (str_of(yjs_get(root, "template"))) dsg_chart_view_set_current(v, str_of(yjs_get(root, "template")));
    {
        bool f = false;
        v->follow = yjs_bool(yjs_get(root, "follow"), &f) == YJS_OK && f;
    }
    ts = yjs_get(root, "templates");
    for (i = 0; ts && ts->type == YJS_OBJECT && i < ts->n; i++) {
        const yjs_value* tv = ts->u.members[i].value;
        const yjs_value* st = yjs_get(tv, "states");
        dsg_chart_tview* t;
        double x = 0, y = 0, z = 0;
        if (!tv || tv->type != YJS_OBJECT) continue;
        t = dsg_chart_view_template(v, ts->u.members[i].key, true);
        if (!t) break;
        if (num2(yjs_get(tv, "pan"), &x, &y)) {
            t->pan_x = (float)x;
            t->pan_y = (float)y;
        }
        if (yjs_double(yjs_get(tv, "zoom"), &z) == YJS_OK && z >= 0.05 && z <= 20) t->zoom = (float)z;
        for (k = 0; st && st->type == YJS_OBJECT && k < st->n; k++)
            if (num2(st->u.members[k].value, &x, &y)) dsg_chart_view_set_pos(v, t->name, st->u.members[k].key, (float)x, (float)y);
    }
    yjs_arena_free(&a);
    return 0;
}

static yjs_value* pair(yjs_arena* a, float x, float y) {
    yjs_value* arr = yjs_new(a, YJS_ARRAY);
    if (!arr || yjs_push(a, arr, yjs_new_int(a, (int64_t)lroundf(x))) != YJS_OK ||
        yjs_push(a, arr, yjs_new_int(a, (int64_t)lroundf(y))) != YJS_OK)
        return NULL;
    return arr;
}

char* dsg_chart_view_write(const dsg_chart_view* v, size_t* n) {
    yjs_arena a;
    yjs_value* root;
    yjs_value* ts;
    char* out = NULL;
    int i, k;
    bool ok;
    yjs_arena_init_heap(&a, (size_t)256 << 20);
    root = yjs_new(&a, YJS_OBJECT);
    ts = yjs_new(&a, YJS_OBJECT);
    ok = root && ts && yjs_set(&a, root, "templates", ts) == YJS_OK;
    /* only when on, so a project that never follows keeps its part's bytes */
    if (ok && v->follow) ok = yjs_set(&a, root, "follow", yjs_new(&a, YJS_TRUE)) == YJS_OK;
    if (ok && v->current) ok = yjs_set(&a, root, "template", yjs_new_stringz(&a, v->current)) == YJS_OK;
    for (i = 0; ok && i < v->n_t; i++) {
        const dsg_chart_tview* t = &v->t[i];
        yjs_value* tv = yjs_new(&a, YJS_OBJECT);
        yjs_value* st = yjs_new(&a, YJS_OBJECT);
        ok = tv && st && yjs_set(&a, ts, t->name, tv) == YJS_OK && yjs_set(&a, tv, "states", st) == YJS_OK &&
             yjs_set(&a, tv, "pan", pair(&a, t->pan_x, t->pan_y)) == YJS_OK &&
             yjs_set(&a, tv, "zoom", yjs_new_fixed(&a, (int64_t)llround((double)t->zoom * 1000.0), 3)) == YJS_OK;
        for (k = 0; ok && k < t->n_pos; k++) ok = yjs_set(&a, st, t->pos[k].name, pair(&a, t->pos[k].x, t->pos[k].y)) == YJS_OK;
    }
    if (ok) {
        size_t m = yjs_write_mem(root, YJS_WRITE_COMPACT, NULL, 0);
        out = (char*)malloc(m + 1);
        if (out) {
            yjs_write_mem(root, YJS_WRITE_COMPACT, out, m + 1);
            *n = m;
        }
    }
    yjs_arena_free(&a);
    return out;
}
