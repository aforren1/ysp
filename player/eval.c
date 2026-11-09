/* player/eval.c - per-trial evaluation of yexp_value (format 2.2, 2.3).
 *
 * Name lookup walks the node chain with strcmp at trial start and when
 * `after` is built. No lookup happens per frame, so the cost is a few
 * microseconds per trial (the measured frame work is in docs/player.md) and
 * the code needs no name table to keep in step with the tree.
 *
 * The order of the search mirrors yexp__resolve() in ysp/experiment.h (the
 * loader's scope rules, including that an outer `vars` shadows a closer
 * table column), so a name means here what the loader checked.
 */
#include "player.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void pl_num(pl_val* v, double d) {
    memset(v, 0, sizeof *v);
    v->kind = YEXP_V_NUMBER;
    v->num = d;
    v->i = (int64_t)d;
}

static void pl_int(pl_val* v, int64_t i) {
    memset(v, 0, sizeof *v);
    v->kind = YEXP_V_NUMBER;
    v->flags = YEXP_VF_INT;
    v->num = (double)i;
    v->i = i;
}

static void pl_bool(pl_val* v, bool b) {
    memset(v, 0, sizeof *v);
    v->kind = YEXP_V_BOOL;
    v->i = b;
    v->num = b;
}

static void pl_str(pl_val* v, const char* s) {
    memset(v, 0, sizeof *v);
    v->kind = YEXP_V_STRING;
    v->str = s;
}

static void pl_ns(pl_val* v, int64_t ns) {
    memset(v, 0, sizeof *v);
    v->kind = YEXP_V_NUMBER;
    v->flags = YEXP_VF_DURATION;
    v->num = (double)ns / 1e9;
    v->i = ns;
}

/* A column's value in a row, read through its declared type. A number
 * column keeps its double so that canonical text and a lookup key agree
 * with the level text the pack tool checked. */
static int pl_column(pl_player* p, const ytb_table* t, int col, int row, pl_val* out) {
    switch (ytb_col_type(t, col)) {
    case YTB_STRING: {
        const char* s = ytb_text(t, row, col);
        pl_str(out, s ? s : "");
        return 0;
    }
    case YTB_INTEGER:
        pl_int(out, ytb_int(t, row, col));
        return 0;
    default: {
        double d = ytb_num(t, row, col);
        if (d != d) { pl_str(out, ""); return 0; }   /* an empty cell */
        pl_num(out, d);
        return 0;
    }
    }
    (void)p;
}

/* A built-in (2.3). 1 and *out when name is one that is known now, 0 when
 * name is not a built-in, < 0 when it is known to the loader but not now. */
static int pl_builtin(pl_player* p, const pl_env* e, const char* name, pl_val* out) {
#define BI(s) (strcmp(name, (s)) == 0)
    if (BI("trial_index") || BI("block") || BI("rep") || BI("condition") || BI("practice") ||
        BI("warmup") || BI("first_in_block") || BI("after_break") || BI("trial_seed")) {
        if (!e->ti) { pl_fail(p, "'$%s' has no value before a trial starts", name); return -1; }
        if (BI("trial_index")) pl_int(out, e->ti->index);
        else if (BI("block")) pl_int(out, e->ti->block);
        else if (BI("rep")) pl_int(out, e->ti->rep);
        else if (BI("condition")) pl_int(out, e->ti->condition);
        else if (BI("practice")) pl_bool(out, e->ti->practice);
        else if (BI("warmup")) pl_bool(out, e->ti->warmup);
        else if (BI("first_in_block")) pl_bool(out, e->ti->first_in_block);
        else if (BI("after_break")) pl_bool(out, e->ti->after_break);
        else {
            uint64_t seed = ytr_trial_seed(e->trials, e->ti->index);
            memset(out, 0, sizeof *out);
            out->kind = YEXP_V_NUMBER;
            out->flags = YEXP_VF_INT;
            out->i = (int64_t)seed;
            out->num = (double)seed;
        }
        return 1;
    }
    if (BI("method") || BI("level")) {
        if (!e->method) { pl_fail(p, "'$%s' on a trial that no track chose", name); return -1; }
        if (BI("method")) pl_str(out, e->method);
        else pl_num(out, e->level);
        return 1;
    }
    if (BI("estimate")) {
        if (e->phase < PL_PH_ROW) { pl_fail(p, "'$estimate' has no value before the method's update"); return -1; }
        if (!e->method) { pl_str(out, ""); return 1; }
        pl_num(out, e->estimate);
        return 1;
    }
    if (BI("contrast_shown")) {
        if (!e->have_shown) { pl_fail(p, "'$contrast_shown' with no stimulus that has a michelson contrast"); return -1; }
        pl_num(out, e->contrast_shown);
        return 1;
    }
    if (BI("response") || BI("rt_s") || BI("responded") || BI("correct") || BI("outcome")) {
        if (e->phase < PL_PH_AFTER) { pl_fail(p, "'$%s' has no value before the response ends", name); return -1; }
        if (BI("response")) pl_str(out, e->response ? e->response : "");
        else if (BI("rt_s")) {
            if (e->rt_ns == YRSP_NO_TIME) pl_str(out, "");
            else pl_ns(out, e->rt_ns);
        } else if (BI("responded")) pl_bool(out, e->responded);
        else if (BI("correct")) {
            if (!e->has_correct) pl_str(out, "");
            else pl_bool(out, e->correct);
        } else pl_str(out, e->outcome ? e->outcome : "");
        return 1;
    }
#undef BI
    return 0;
}

/* The row of `node`'s own table now: a leaf's trial row, a container's row
 * from the walk. */
static int pl_node_row(pl_player* p, const pl_env* e, const yexp_node* n, int* row) {
    int s;
    if (n == e->node && e->ti && n->trial) {
        *row = e->ti->condition;
        return *row >= 0;
    }
    for (s = p->sp - 1; s >= 0; s--)
        if (p->stack[s].node == n) { *row = p->stack[s].row; return p->stack[s].table != NULL; }
    return 0;
}

int pl_var(pl_player* p, const pl_env* e, const char* name, pl_val* out) {
    const yexp_node* n;
    int i, rc;
    rc = pl_builtin(p, e, name, out);
    if (rc != 0) return rc < 0 ? -1 : 0;
    for (i = 0; i < p->def->n_fields; i++)
        if (strcmp(p->def->fields[i].name, name) == 0) { *out = p->field_values[i]; return 0; }
    for (n = e->node; n; n = n->parent) {
        int k;
        for (k = 0; k < n->n_vars; k++)
            if (strcmp(n->vars[k].key, name) == 0) {
                const yexp_value* v = n->vars[k].value;
                if (v->kind == YEXP_V_CHOICE) {
                    if (!e->trials || !e->ti) { pl_fail(p, "'$%s' is drawn per trial; it has no value here", name); return -1; }
                    int j = ytr_var_index(e->trials, name);
                    const char* s = j >= 0 ? ytr_var_text(e->trials, e->ti->index, j) : NULL;
                    if (!s) { pl_fail(p, "'$%s': the trial has no draw for it", name); return -1; }
                    /* A choice's values are text in the trials header; a
                     * number written in the definition reads back as that
                     * number through the literal, so `vars` keeps its type. */
                    for (k = 0; k < (int)v->n; k++) {
                        char buf[64];
                        if (pl_eval_text(p, e, &v->items[k], buf, sizeof buf) < 0) return -1;
                        if (strcmp(buf, s) == 0) return pl_eval(p, e, &v->items[k], out);
                    }
                    pl_str(out, s);
                    return 0;
                }
                return pl_eval(p, e, v, out);
            }
        for (k = 0; k < n->n_jitters; k++)
            if (strcmp(n->jitters[k], name) == 0) {
                ytr_jitter_value jv;
                int j;
                if (!e->trials || !e->ti) { pl_fail(p, "'$%s' is a jitter; it has no value here", name); return -1; }
                j = ytr_jitter_index(e->trials, name);
                jv = ytr_jitter(e->trials, e->ti->index, j);
                pl_ns(out, jv.ns);
                return 0;
            }
        for (k = 0; k < n->n_rule_vars; k++)
            if (strcmp(n->rule_vars[k].name, name) == 0) {
                const char* s;
                int j;
                if (!e->trials || !e->ti) { pl_fail(p, "'$%s' is drawn per trial; it has no value here", name); return -1; }
                j = ytr_var_index(e->trials, name);
                s = j >= 0 ? ytr_var_text(e->trials, e->ti->index, j) : NULL;
                if (!s) { pl_fail(p, "'$%s': the trial has no draw for it", name); return -1; }
                pl_str(out, s);
                return 0;
            }
        for (k = 0; k < n->n_tracks; k++) {
            int d;
            for (d = 0; d < n->tracks[k]->n_dims; d++)
                if (strcmp(n->tracks[k]->dims[d], name) == 0) {
                    const pl_method* m = p->leaf ? p->leaf->tracks[e->ti ? e->ti->track : 0] : NULL;
                    if (!m || m->def != n->tracks[k]) { pl_str(out, ""); return 0; }
                    pl_num(out, m->aep_x[d]);
                    return 0;
                }
        }
    }
    for (n = e->node; n; n = n->parent) {
        int col, row;
        const ytb_table* t;
        if (!n->table) continue;
        t = pl_table_get(p, n->table);
        if (!t) { pl_fail(p, "no table '%s'", n->table); return -1; }
        col = ytb_col(t, name);
        if (col < 0) continue;
        if (!pl_node_row(p, e, n, &row)) { pl_fail(p, "'$%s' is a column of '%s', which has no row now", name, n->table); return -1; }
        return pl_column(p, t, col, row, out);
    }
    pl_fail(p, "'$%s' is not defined here", name);
    return -1;
}

int pl_val_text(const pl_val* v, char* buf, size_t cap) {
    if (cap == 0) return -1;
    buf[0] = 0;
    switch (v->kind) {
    case YEXP_V_BOOL:   snprintf(buf, cap, "%s", v->i ? "true" : "false"); return 0;
    case YEXP_V_STRING: snprintf(buf, cap, "%s", v->str ? v->str : ""); return 0;
    case YEXP_V_NUMBER:
        if (v->flags & YEXP_VF_DURATION) {
            char t[YJS_DOUBLE_TEXT];
            int k = yjs_format_double(v->num, t, sizeof t);
            snprintf(buf, cap, "%s", k >= 0 ? t : "");
        } else if (v->flags & YEXP_VF_INT) {
            snprintf(buf, cap, "%lld", (long long)v->i);
        } else {
            char t[YJS_DOUBLE_TEXT];
            int k = yjs_format_double(v->num, t, sizeof t);
            snprintf(buf, cap, "%s", k >= 0 ? t : "");
        }
        return 0;
    default:
        return -1;
    }
}

/* ycol_michelson's max for the display's gray axis, clamped to 1 as the C
 * program's level_max: past 1 a Michelson contrast has no meaning. */
static double pl_gamut(pl_player* p, const char* what) {
    const char* dot = strchr(what, '.');
    if (!dot || strcmp(dot + 1, "contrast") != 0) {
        pl_fail(p, "{\"gamut\": \"%s\"}: only a contrast has a gamut in this version", what);
        return NAN;
    }
    if (!p->have_cal) {
        pl_fail(p, "{\"gamut\": \"%s\"} needs a calibration", what);
        return NAN;
    }
    return p->gray_sh.max < 1.0 ? p->gray_sh.max : 1.0;
}

int pl_eval(pl_player* p, const pl_env* e, const yexp_value* v, pl_val* out) {
    int depth = 0;
    for (;;) {
        if (++depth > 256) { pl_fail(p, "a value's lookups go 256 deep"); return -1; }
        switch (v->kind) {
        case YEXP_V_NONE:
            memset(out, 0, sizeof *out);
            return 0;
        case YEXP_V_BOOL:
            pl_bool(out, v->i != 0);
            return 0;
        case YEXP_V_NUMBER:
            if (v->flags & YEXP_VF_DURATION) pl_ns(out, v->i);
            else if (v->flags & YEXP_VF_INT) pl_int(out, v->i);
            else pl_num(out, v->num);
            return 0;
        case YEXP_V_STRING:
            pl_str(out, v->str);
            return 0;
        case YEXP_V_ARRAY:
            memset(out, 0, sizeof *out);
            out->kind = YEXP_V_ARRAY;
            out->n = v->n;
            out->items = v->items;
            return 0;
        case YEXP_V_VAR:
            return pl_var(p, e, v->str, out);
        case YEXP_V_LOOKUP: {
            char key[256];
            pl_val by;
            int lo = 0, hi = (int)v->n - 1, found = -1;
            if (pl_eval(p, e, v->by, &by) < 0) return -1;
            if (pl_val_text(&by, key, sizeof key) < 0) { pl_fail(p, "a lookup's 'by' is not a single value"); return -1; }
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                int c = strcmp(key, v->map[mid].key);
                if (c == 0) { found = mid; break; }
                if (c < 0) hi = mid - 1;
                else lo = mid + 1;
            }
            if (found < 0) { pl_fail(p, "a lookup has no entry for the value '%s'", key); return -1; }
            v = v->map[found].value;
            continue;
        }
        case YEXP_V_MICHELSON:
            /* the scene contrast is computed by pl_stim_begin_trial(); a
             * michelson read by a variable means its asked value */
            v = v->by;
            continue;
        case YEXP_V_LINSPACE:
        case YEXP_V_CHOICE:
            pl_fail(p, "a %s has no single value", v->kind == YEXP_V_LINSPACE ? "linspace" : "choice");
            return -1;
        case YEXP_V_GAMUT: {
            double d = pl_gamut(p, v->str);
            if (d != d) return -1;
            pl_num(out, d);
            return 0;
        }
        default:
            pl_fail(p, "unknown value kind %d", (int)v->kind);
            return -1;
        }
    }
}

int pl_eval_num(pl_player* p, const pl_env* e, const yexp_value* v, double* out) {
    pl_val x;
    if (pl_eval(p, e, v, &x) < 0) return -1;
    if (x.kind == YEXP_V_BOOL || x.kind == YEXP_V_NUMBER) { *out = x.num; return 0; }
    if (x.kind == YEXP_V_STRING) {
        pl_fail(p, "the text '%s' where a number goes", x.str ? x.str : "");
        return -1;
    }
    pl_fail(p, "no value where a number goes");
    return -1;
}

int pl_eval_int(pl_player* p, const pl_env* e, const yexp_value* v, int64_t* out) {
    pl_val x;
    if (pl_eval(p, e, v, &x) < 0) return -1;
    if (x.kind == YEXP_V_NUMBER) {
        if (x.flags & (YEXP_VF_INT | YEXP_VF_DURATION)) { *out = x.i; return 0; }
        if (x.num != floor(x.num) || fabs(x.num) > 9e18) { pl_fail(p, "%g where an integer goes", x.num); return -1; }
        *out = (int64_t)x.num;
        return 0;
    }
    if (x.kind == YEXP_V_BOOL) { *out = x.i; return 0; }
    pl_fail(p, "no integer value");
    return -1;
}

int pl_eval_bool(pl_player* p, const pl_env* e, const yexp_value* v, bool* out) {
    pl_val x;
    if (pl_eval(p, e, v, &x) < 0) return -1;
    if (x.kind == YEXP_V_BOOL) { *out = x.i != 0; return 0; }
    if (x.kind == YEXP_V_NUMBER && (x.num == 0.0 || x.num == 1.0)) { *out = x.num == 1.0; return 0; }
    {
        char t[64];
        if (pl_val_text(&x, t, sizeof t) == 0) pl_fail(p, "'%s' where a boolean goes (0 or 1, true or false)", t);
        else pl_fail(p, "no value where a boolean goes");
    }
    return -1;
}

int pl_eval_ns(pl_player* p, const pl_env* e, const yexp_value* v, int64_t* ns) {
    pl_val x;
    if (pl_eval(p, e, v, &x) < 0) return -1;
    if (x.kind == YEXP_V_NUMBER) {
        if (x.flags & YEXP_VF_DURATION) { *ns = x.i; return 0; }
        if (x.num < 0 || x.num > 3.2e9) { pl_fail(p, "the duration %g seconds", x.num); return -1; }
        *ns = (int64_t)(x.num * 1e9 + 0.5);
        return 0;
    }
    if (x.kind == YEXP_V_STRING) { pl_fail(p, "'%s' where a duration in seconds goes", x.str ? x.str : ""); return -1; }
    pl_fail(p, "no value where a duration in seconds goes");
    return -1;
}

int pl_eval_text(pl_player* p, const pl_env* e, const yexp_value* v, char* buf, size_t cap) {
    pl_val x;
    if (pl_eval(p, e, v, &x) < 0) return -1;
    if (pl_val_text(&x, buf, cap) < 0) { pl_fail(p, "a value that has no text"); return -1; }
    return 0;
}
