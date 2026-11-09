/* experiment_dump.h - a stable text dump of ysp/experiment.h's checked form,
 * for the golden comparisons in tests/adapt/experiment_test.c and for the
 * fuzz target's round trip. Every field of the form is printed, one item
 * per line, so a change to the form or to a default shows in the diff.
 * Numbers print through their canonical text. */
#ifndef YEXP_DUMP_H
#define YEXP_DUMP_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct xd_buf {
    char*  p;
    size_t n, cap;
} xd_buf;

static void xd_printf(xd_buf* b, const char* fmt, ...) {
    char tmp[1024];
    va_list ap;
    int w;
    va_start(ap, fmt);
    w = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    if ((size_t)w >= sizeof tmp) w = (int)sizeof tmp - 1;
    if (b->n + (size_t)w + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        char* q;
        while (nc < b->n + (size_t)w + 1) nc *= 2;
        q = (char*)realloc(b->p, nc);
        if (!q) abort();
        b->p = q;
        b->cap = nc;
    }
    memcpy(b->p + b->n, tmp, (size_t)w);
    b->n += (size_t)w;
    b->p[b->n] = 0;
}

static void xd_value(xd_buf* b, const yexp_value* v);

static void xd_num(xd_buf* b, const yexp_value* v) {
    if (v->flags & YEXP_VF_DURATION) xd_printf(b, "dur:%lldns", (long long)v->i);
    else if (v->flags & YEXP_VF_INT) xd_printf(b, "int:%lld", (long long)v->i);
    else {
        char t[YJS_DOUBLE_TEXT];
        yjs_format_double(v->num, t, sizeof t);
        xd_printf(b, "real:%s", t);
    }
}

static void xd_str(xd_buf* b, const char* s) {
    xd_printf(b, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') xd_printf(b, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) xd_printf(b, "\\x%02x", (unsigned)(unsigned char)*s);
        else xd_printf(b, "%c", *s);
    }
    xd_printf(b, "\"");
}

static void xd_value(xd_buf* b, const yexp_value* v) {
    uint32_t i;
    if (!v) { xd_printf(b, "(null)"); return; }
    switch (v->kind) {
    case YEXP_V_NONE: xd_printf(b, "none"); break;
    case YEXP_V_BOOL: xd_printf(b, v->i ? "true" : "false"); break;
    case YEXP_V_NUMBER: xd_num(b, v); break;
    case YEXP_V_STRING: xd_str(b, v->str); break;
    case YEXP_V_VAR: xd_printf(b, "$%s", v->str); break;
    case YEXP_V_GAMUT: xd_printf(b, "gamut(%s)", v->str); break;
    case YEXP_V_MICHELSON: xd_printf(b, "michelson("); xd_value(b, v->by); xd_printf(b, ")"); break;
    case YEXP_V_ARRAY: case YEXP_V_LINSPACE: case YEXP_V_CHOICE:
        xd_printf(b, v->kind == YEXP_V_ARRAY ? "[" : v->kind == YEXP_V_LINSPACE ? "linspace[" : "choice[");
        for (i = 0; i < v->n; i++) { if (i) xd_printf(b, ", "); xd_value(b, &v->items[i]); }
        xd_printf(b, "]");
        break;
    case YEXP_V_LOOKUP:
        xd_printf(b, "lookup(");
        if (v->by) xd_value(b, v->by);
        xd_printf(b, "){");
        for (i = 0; i < v->n; i++) {
            if (i) xd_printf(b, ", ");
            xd_str(b, v->map[i].key);
            xd_printf(b, ": ");
            xd_value(b, v->map[i].value);
        }
        xd_printf(b, "}");
        break;
    default: xd_printf(b, "?"); break;
    }
}

static void xd_props(xd_buf* b, const char* ind, const yexp_prop* p, int n) {
    int i;
    for (i = 0; i < n; i++) {
        xd_printf(b, "%s%s%s = ", ind, p[i].key, p[i].given ? "" : " (default)");
        xd_value(b, p[i].value);
        xd_printf(b, "\n");
    }
}

static void xd_kvs(xd_buf* b, const char* ind, const char* label, const yexp_kv* kv, int n) {
    int i;
    for (i = 0; i < n; i++) {
        xd_printf(b, "%s%s %s = ", ind, label, kv[i].key);
        xd_value(b, kv[i].value);
        xd_printf(b, "\n");
    }
}

static void xd_steps(xd_buf* b, const char* ind, const yexp_step* steps, int n);

static const char* const xd_ops[] = { "on", "off", "wait", "at", "set", "to", "then", "trigger", "mark", "if", "unless" };

static void xd_step(xd_buf* b, const char* ind, const yexp_step* s) {
    int i;
    char sub[64];
    xd_printf(b, "%sstep %s", ind, xd_ops[s->op]);
    for (i = 0; i < s->n_stims; i++) xd_printf(b, " %s", s->stims[i]);
    if (s->t) { xd_printf(b, " t="); xd_value(b, s->t); }
    if (s->stagger) { xd_printf(b, " stagger="); xd_value(b, s->stagger); }
    if (s->duration) { xd_printf(b, " duration="); xd_value(b, s->duration); }
    if (s->delay) { xd_printf(b, " delay="); xd_value(b, s->delay); }
    if (s->ease) xd_printf(b, " ease=%s", s->ease);
    if (s->code) { xd_printf(b, " code="); xd_value(b, s->code); }
    if (s->role) xd_printf(b, " role=%s", s->role);
    if (s->mark) xd_printf(b, " mark=%s", s->mark);
    if (s->cond) xd_printf(b, " cond=%s", s->cond);
    xd_printf(b, "\n");
    snprintf(sub, sizeof sub, "%s  ", ind);
    xd_props(b, sub, s->props, s->n_props);
    if (s->steps) xd_steps(b, sub, s->steps, s->n_steps);
}

static void xd_steps(xd_buf* b, const char* ind, const yexp_step* steps, int n) {
    int i;
    for (i = 0; i < n; i++) xd_step(b, ind, &steps[i]);
}

static void xd_node(xd_buf* b, const yexp_node* n, int depth);

static void xd_strs(xd_buf* b, const char* label, const char* const* v, int n) {
    int i;
    xd_printf(b, "%s=[", label);
    for (i = 0; i < n; i++) xd_printf(b, "%s%s", i ? " " : "", v[i]);
    xd_printf(b, "]");
}

static void xd_node(xd_buf* b, const yexp_node* n, int depth) {
    char ind[80];
    int i;
    snprintf(ind, sizeof ind, "%*s", depth * 2, "");
    xd_printf(b, "%snode %s path=%s trial=%s table=%s reps=%d interleave=%d track_rate=", ind, n->name, n->path, n->trial ? n->trial->name : "-", n->table ? n->table : "-", n->repetitions, n->interleave);
    { char t[YJS_DOUBLE_TEXT]; yjs_format_double(n->track_rate, t, sizeof t); xd_printf(b, "%s", t); }
    xd_printf(b, " if=%s unless=%s loop=%d", n->if_var ? n->if_var : "-", n->unless_var ? n->unless_var : "-", (int)n->has_loop);
    if (n->has_loop) { char t[YJS_DOUBLE_TEXT]; yjs_format_double(n->loop_until, t, sizeof t); xd_printf(b, ":%s,%d", t, n->loop_max); }
    xd_printf(b, " requeue=%d record_data=%d\n", (int)n->requeue_no_response, (int)n->record_data);
    xd_printf(b, "%s  rules=", ind);
    xd_str(b, n->rules);
    xd_printf(b, "\n%s  ", ind);
    xd_strs(b, "jitters", n->jitters, n->n_jitters);
    xd_printf(b, " ");
    xd_printf(b, "rule_vars=[");
    for (i = 0; i < n->n_rule_vars; i++) {
        int k;
        xd_printf(b, "%s%s(", i ? " " : "", n->rule_vars[i].name);
        for (k = 0; k < n->rule_vars[i].n_values; k++) xd_printf(b, "%s%s", k ? "," : "", n->rule_vars[i].values[k]);
        xd_printf(b, ")");
    }
    xd_printf(b, "]");
    xd_printf(b, " tracks=[");
    for (i = 0; i < n->n_tracks; i++) xd_printf(b, "%s%s", i ? " " : "", n->tracks[i]->name);
    xd_printf(b, "]\n");
    {
        char sub[100];
        snprintf(sub, sizeof sub, "%s  ", ind);
        xd_kvs(b, sub, "var", n->vars, n->n_vars);
        xd_kvs(b, sub, "data", n->data, n->n_data);
    }
    for (i = 0; i < n->n_children; i++) xd_node(b, &n->children[i], depth + 1);
}

static void xd_def(xd_buf* b, const yexp_def* d) {
    int i, k;
    xd_printf(b, "definition version=%d name=", d->format_version);
    xd_str(b, d->name);
    xd_printf(b, "\n");
    xd_printf(b, "units_deg=%d background=%g,%g,%g calibration_required=%d min_tier=%d web_allowed=%d web_exclusion=%d\n", d->units_deg, d->background[0], d->background[1], d->background[2], (int)d->calibration_required, d->min_tier, (int)d->web_allowed, d->web_exclusion != NULL);
    for (i = 0; i < d->n_fields; i++) xd_printf(b, "field %s %s required=%d\n", d->fields[i].name, d->fields[i].is_string ? "string" : "integer", (int)d->fields[i].required);
    for (i = 0; i < d->n_roles; i++) {
        const yexp_role* r = &d->roles[i];
        xd_printf(b, "role %s dir=%d ", r->name, r->direction);
        xd_strs(b, "family", r->family, r->n_family);
        xd_printf(b, " required=%d min_tier=%d pulse_ns=%lld kind=%s\n", (int)r->required, r->min_tier, (long long)r->pulse_ns, r->kind ? r->kind : "");
        xd_kvs(b, "  ", "button", r->buttons, r->n_buttons);
        xd_kvs(b, "  ", "map", r->map, r->n_map);
    }
    for (i = 0; i < d->n_stimuli; i++) {
        const yexp_stim* s = &d->stimuli[i];
        xd_printf(b, "stimulus %s kind=%d layer=%d\n", s->name, (int)s->kind, s->layer);
        xd_props(b, "  ", s->props, s->n_props);
    }
    for (i = 0; i < d->n_methods; i++) {
        const yexp_method* m = &d->methods[i];
        xd_printf(b, "method %s type=%d log10=%d estimate=%s ", m->name, (int)m->type, (int)m->log10, m->estimate ? m->estimate : "-");
        xd_strs(b, "dims", m->dims, m->n_dims);
        xd_printf(b, "\n");
        xd_props(b, "  ", m->props, m->n_props);
    }
    for (i = 0; i < d->n_templates; i++) {
        const yexp_template* t = &d->templates[i];
        xd_printf(b, "template %s\n  events\n", t->name);
        xd_steps(b, "    ", t->events, t->n_events);
        xd_printf(b, "  after\n");
        xd_steps(b, "    ", t->after, t->n_after);
        if (t->response) {
            const yexp_response* r = t->response;
            xd_printf(b, "  response role=%s from=%s ends=%d persist=%d max=%d held=%d release=%d choices=", r->role, r->from, (int)r->ends_trial, (int)r->persist, r->max_responses, (int)r->allow_held_key, (int)r->wait_for_key_release);
            for (k = 0; k < r->n_choices; k++) xd_printf(b, "%s%s:%s", k ? "," : "", r->choices[k].key, r->choices[k].name);
            xd_printf(b, "\n    duration="); xd_value(b, r->duration);
            xd_printf(b, " min_rt="); xd_value(b, r->min_rt);
            xd_printf(b, " key_answer="); xd_value(b, r->key_answer);
            xd_printf(b, " trigger=%s ", r->trigger_role ? r->trigger_role : "-");
            if (r->trigger_code) xd_value(b, r->trigger_code);
            xd_printf(b, "\n");
        }
        xd_printf(b, "  record_given=%d ", (int)t->record_given);
        xd_strs(b, "record", t->record, t->n_record);
        xd_printf(b, "\n  outcome="); xd_value(b, t->outcome);
        xd_printf(b, " method_value="); xd_value(b, t->method_value);
        xd_printf(b, "\n");
        xd_kvs(b, "  ", "column", t->columns, t->n_columns);
    }
    for (i = 0; i < d->n_timeline; i++) xd_node(b, &d->timeline[i], 0);
}

static char* xd_dump(const yexp_def* d) {
    xd_buf b;
    memset(&b, 0, sizeof b);
    xd_printf(&b, "%s", "");
    xd_def(&b, d);
    return b.p ? b.p : (char*)calloc(1, 1);
}

#endif
