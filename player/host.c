/* player/host.c - the yexp_host providers, real versions.
 *
 * Until the player, the loader's host checks ran only against test stubs
 * and the pack tool. Here the pack's tables and the rig profile answer, and
 * rules() opens a real ytr_trials dry. A pack that fails a check here is
 * refused before the screen opens.
 */
#include "player.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int h_entry(void* ctx, int kind, const char* name) {
    static const uint32_t want[] = { YPAK_KIND_TABLE, YPAK_KIND_TEXTURE, YPAK_KIND_FONT, YPAK_KIND_ARTWORK, YPAK_KIND_AUDIO, YPAK_KIND_VIDEO };
    pl_player* p = (pl_player*)ctx;
    ypak_entry e;
    if (kind < 0 || kind >= 6 || ypak_find(&p->pack, name, &e) != 0) return 0;
    return e.kind == want[kind];
}

static int h_column(void* ctx, const char* table, int i, yexp_column* out) {
    pl_player* p = (pl_player*)ctx;
    const ytb_table* t = pl_table_get(p, table);
    ytb_type ty;
    if (!t) return -1;
    if (i >= t->n_cols) return 0;
    out->name = ytb_col_name(t, i);
    ty = ytb_col_type(t, i);
    out->type = ty == YTB_STRING ? YEXP_COL_STRING : ty == YTB_INTEGER ? YEXP_COL_INTEGER : YEXP_COL_NUMBER;
    out->n_levels = ytb_n_levels(t, i);
    return 1;
}

static int h_level(void* ctx, const char* table, int col, int l, char* buf, size_t cap) {
    pl_player* p = (pl_player*)ctx;
    const ytb_table* t = pl_table_get(p, table);
    size_t n;
    if (!t || col < 0 || col >= t->n_cols || l >= ytb_n_levels(t, col)) return -1;
    if (ytb_col_type(t, col) != YTB_STRING) {
        char num[YJS_DOUBLE_TEXT];
        double d = ytb_level_num(t, col, l);
        int k = d == d ? yjs_format_double(d, num, sizeof num) : 0;
        if (k < 0) k = 0;
        if ((size_t)k < cap) { memcpy(buf, num, (size_t)k); buf[k] = 0; }
        return k;
    }
    {
        const char* x = ytb_level_text(t, col, l);
        if (!x) return -1;
        n = strlen(x);
        if (n < cap) memcpy(buf, x, n + 1);
        return (int)n;
    }
}

/* A role against the rig (devices_spec 11.1, format 2.9). With no rig this
 * provider is not installed, and report.skipped says so. */
static int h_role(void* ctx, const yexp_role* role, char* msg, size_t cap) {
    pl_player* p = (pl_player*)ctx;
    const yrig_role* r = yrig_find(p->rig, role->name);
    int i;
    if (!r || !r->family[0]) {
        /* --sim's scripted participant and in-process trigger sink stand in
         * for the devices, so an unbound role is not a refusal there; in a
         * real run an input role that lists the keyboard takes it, as it
         * needs no binding */
        if (role->required && !p->args.sim) {
            for (i = 0; role->direction == 0 && i < role->n_family; i++)
                if (strcmp(role->family[i], "keyboard") == 0) return 0;
            snprintf(msg, cap, "the rig profile has no device bound to the required role");
            return 1;
        }
        return 0;
    }
    for (i = 0; i < role->n_family; i++)
        if (strcmp(role->family[i], r->family) == 0) return 0;
    snprintf(msg, cap, "the rig binds it to family '%s', which the role does not list", r->family);
    return 1;
}

/* A value against a header's table, where the value is a literal. The
 * tables' ranges are the headers' own, so an out-of-range literal is
 * refused at load with the key's name, not at the first frame. */
static int h_param(void* ctx, int section, const char* kind, const yexp_prop* pr, char* msg, size_t cap) {
    const yexp_value* v = pr->value;
    (void)ctx;
    if (v->kind != YEXP_V_NUMBER || (v->flags & YEXP_VF_DURATION)) return 0;
    if (section == YEXP_PARAM_STIMULUS) {
        if (strcmp(kind, "rdk") == 0) {
            int n, i;
            const yrdk_param* r = yrdk_params(&n);
            for (i = 0; i < n; i++)
                if (strcmp(r[i].name, pr->key) == 0) {
                    if (v->num < r[i].min || v->num > r[i].max) {
                        snprintf(msg, cap, "'%s' = %g is outside ysp/rdk.h's %g to %g", pr->key, v->num, r[i].min, r[i].max);
                        return 1;
                    }
                    return 0;
                }
            /* a dots field: falls through to gfx's table */
        }
        {
            int n, i;
            const ygfx_param* r = ygfx_params(&n);
            for (i = 0; i < n; i++)
                if (strcmp(r[i].name, pr->key) == 0) {
                    if (v->num < r[i].min || v->num > r[i].max) {
                        snprintf(msg, cap, "'%s' = %g is outside ysp/gfx.h's %g to %g", pr->key, v->num, r[i].min, r[i].max);
                        return 1;
                    }
                    return 0;
                }
        }
        return 0;
    }
    if (strcmp(kind, "stair") == 0) {
        int n, i;
        const yst_param* r = yst_params(&n);
        for (i = 0; i < n; i++)
            if (strcmp(r[i].name, pr->key) == 0) {
                if (r[i].type[0] != 'b' && (v->num < r[i].min || v->num > r[i].max)) {
                    snprintf(msg, cap, "'%s' = %g is outside ysp/stair.h's %g to %g", pr->key, v->num, r[i].min, r[i].max);
                    return 1;
                }
                return 0;
            }
    }
    /* quest and aep keys are checked by their open() at load, which names them */
    return 0;
}

static bool h_dummy_done(void* ctx) { (void)ctx; return true; }

/* ytr_rules(), then a dry ytr_open(): an impossible constraint fails here,
 * not at trial 190. Track objects are not open yet, so the dry open uses
 * dummy tracks, which is all a constraint check needs. */
static int h_rules(void* ctx, const yexp_node* leaf, char* msg, size_t cap) {
    pl_player* p = (pl_player*)ctx;
    ytr_desc d;
    ytr_rules_desc r;
    static ytr_trials t;      /* 253 KB: not the stack */
    const ytb_table* table = leaf->table ? pl_table_get(p, leaf->table) : NULL;
    size_t bytes = ((size_t)1 << 19) + (table ? 32u * (size_t)table->n_rows : 0u);
    void* arena = pl_alloc(p, bytes);
    uint64_t rng = 1;
    int rc = 0, i;
    if (!arena) { snprintf(msg, cap, "out of memory for the rules"); return 1; }
    memset(&d, 0, sizeof d);
    memset(&r, 0, sizeof r);
    if (leaf->rules[0]) {
        r.text = leaf->rules;
        r.len = strlen(leaf->rules);
        r.table = table;
        r.arena = arena;
        r.arena_size = bytes;
        r.participant = 0;
        rc = ytr_rules(&d, &r, msg, cap);
        if (rc) return rc;
    }
    if (table) d.table = table;
    else if (leaf->n_tracks == 0 && !d.n_conditions && !d.n_factors) d.n_conditions = 1;
    if (d.reps == 0 && !d.cond_reps && (table || d.n_conditions) && d.order != YTR_ORDER_WITH_REPLACEMENT && !d.n_order_list)
        d.reps = 1;
    for (i = 0; i < leaf->n_tracks && i < YTR_MAX_TRACKS; i++) d.tracks[i] = ytr_track(&rng, h_dummy_done);
    d.n_tracks = leaf->n_tracks;
    d.interleave = leaf->interleave ? YTR_INTERLEAVE_ROUND_ROBIN : YTR_INTERLEAVE_RANDOM;
    d.track_rate = leaf->track_rate;
    d.rng = ytr_splitmix;
    d.rng_ctx = &rng;
    d.seed = 1;
    d.display_rate_num = 60;
    d.display_rate_den = 1;
    memset(&t, 0, sizeof t);
    if (!ytr_open(&t, &d)) {
        snprintf(msg, cap, "%s", ytr_error(&t));
        ytr_close(&t);
        return 1;
    }
    ytr_close(&t);
    return 0;
}

/* The op table of a template at the variables' extremes, through the real
 * ytl_check_ops() (2.5). A leaf's rules give a jitter's range; a table
 * column's its levels'; a lookup's its map. Two passes, low and high, and
 * both branches of every `if` are in the table. */
int pl_extreme_ns(pl_player* p, const yexp_value* v, int64_t* ns) {
    int high = p->env.extreme == 2;
    switch (v->kind) {
    case YEXP_V_NUMBER:
        *ns = (v->flags & YEXP_VF_DURATION) ? v->i : (int64_t)(v->num * 1e9);
        return 0;
    case YEXP_V_LOOKUP: {
        uint32_t i;
        int64_t best = high ? INT64_MIN : INT64_MAX;
        for (i = 0; i < v->n; i++) {
            int64_t x;
            if (pl_extreme_ns(p, v->map[i].value, &x) < 0) return -1;
            if (high ? x > best : x < best) best = x;
        }
        *ns = v->n ? best : 0;
        return 0;
    }
    case YEXP_V_VAR: {
        const yexp_node* n;
        for (n = p->env.node; n; n = n->parent) {
            int k;
            for (k = 0; k < n->n_vars; k++)
                if (strcmp(n->vars[k].key, v->str) == 0) return pl_extreme_ns(p, n->vars[k].value, ns);
            for (k = 0; k < n->n_jitters; k++)
                if (strcmp(n->jitters[k], v->str) == 0) {
                    int j;
                    for (j = 0; j < p->extreme_desc.n_jitters; j++)
                        if (strcmp(p->extreme_desc.jitters[j].name, v->str) == 0) {
                            *ns = high ? p->extreme_desc.jitters[j].hi_ns : p->extreme_desc.jitters[j].lo_ns;
                            return 0;
                        }
                }
        }
        for (n = p->env.node; n; n = n->parent) {
            const ytb_table* t;
            int col;
            if (!n->table) continue;
            t = pl_table_get(p, n->table);
            if (!t) continue;
            col = ytb_col(t, v->str);
            if (col >= 0 && ytb_col_type(t, col) != YTB_STRING) {
                int l;
                double best = high ? -1e300 : 1e300;
                for (l = 0; l < ytb_n_levels(t, col); l++) {
                    double d = ytb_level_num(t, col, l);
                    if (d == d && (high ? d > best : d < best)) best = d;
                }
                *ns = ytb_n_levels(t, col) ? (int64_t)(best * 1e9) : 0;
                return 0;
            }
        }
        *ns = high ? 3600000000000 : 0;   /* a built-in such as rt_s: a window */
        return 0;
    }
    case YEXP_V_CHOICE: case YEXP_V_ARRAY: {
        uint32_t i;
        int64_t best = high ? INT64_MIN : INT64_MAX;
        for (i = 0; i < v->n; i++) {
            int64_t x;
            if (pl_extreme_ns(p, &v->items[i], &x) < 0) return -1;
            if (high ? x > best : x < best) best = x;
        }
        *ns = v->n ? best : 0;
        return 0;
    }
    default:
        *ns = 0;
        return 0;
    }
}

static void h_find_leaves(const yexp_node* n, int count, const yexp_template* t, const yexp_node** out, int* got, int cap) {
    int i;
    for (i = 0; i < count; i++) {
        if (n[i].trial == t) { if (*got < cap) out[*got] = &n[i]; (*got)++; }
        else if (!n[i].trial) h_find_leaves(n[i].children, n[i].n_children, t, out, got, cap);
    }
}

static int h_check_ops(void* ctx, const yexp_def* d, const yexp_template* t, char* msg, size_t cap) {
    pl_player* p = (pl_player*)ctx;
    static ytl_timeline tl;
    ytl_event* ev;
    ytl_key* keys;
    ytl_desc td;
    const yexp_node** leaves;
    void* rules_arena;
    int n_leaves = 0, li;
    ytl_op* ops;
    int mode, bound, rc = 0;
    pl_env saved = p->env;
    p->def = (yexp_def*)d;
    if (pl_channels_assign(p) < 0) { snprintf(msg, cap, "%s", p->error); p->failed = 0; p->env = saved; return 1; }
    bound = 16 + pl_ops_bound(t->events, t->n_events) + pl_ops_bound(t->after, t->n_after);
    /* sized from this template, as the run's are: a fixed table would refuse
     * a long valid template */
    ops = (ytl_op*)pl_alloc(p, sizeof(ytl_op) * (size_t)bound);
    ev = (ytl_event*)pl_alloc(p, sizeof(ytl_event) * (size_t)bound);
    keys = (ytl_key*)pl_alloc(p, sizeof(ytl_key) * (size_t)bound * 4);
    if (!ops || !ev || !keys) { snprintf(msg, cap, "out of memory"); return 1; }
    h_find_leaves(d->timeline, d->n_timeline, t, NULL, &n_leaves, 0);
    leaves = (const yexp_node**)pl_alloc(p, sizeof(yexp_node*) * (size_t)(n_leaves + 1));
    rules_arena = pl_alloc(p, (size_t)1 << 19);
    if (!leaves || !rules_arena) { snprintf(msg, cap, "out of memory"); return 1; }
    li = n_leaves;
    n_leaves = 0;
    h_find_leaves(d->timeline, d->n_timeline, t, leaves, &n_leaves, li);
    if (n_leaves == 0) leaves[n_leaves++] = NULL;   /* a template no node runs */
    /* every leaf that runs it: their jitters and tables give other extremes */
    for (li = 0; li < n_leaves && rc == 0; li++) {
    const yexp_node* leaf = leaves[li];
    if (leaf && leaf->rules[0]) {
        /* a jitter's range is only in the rules text */
        ytr_rules_desc rr;
        char err[300];
        size_t bytes = (size_t)1 << 19;
        void* arena = rules_arena;
        memset(&p->extreme_desc, 0, sizeof p->extreme_desc);
        memset(&rr, 0, sizeof rr);
        rr.text = leaf->rules;
        rr.len = strlen(leaf->rules);
        rr.table = leaf->table ? pl_table_get(p, leaf->table) : NULL;
        rr.arena = arena;
        rr.arena_size = bytes;
        (void)ytr_rules(&p->extreme_desc, &rr, err, sizeof err);
    } else memset(&p->extreme_desc, 0, sizeof p->extreme_desc);

    memset(&td, 0, sizeof td);
    td.events = ev;
    td.event_capacity = bound;
    td.keys = keys;
    td.key_capacity = bound * 4;
    td.n_channels = p->n_channels;
    for (mode = 1; mode <= 2 && rc == 0; mode++) {
        int bad = -1, nops;
        int64_t cursor = 0;
        memset(&p->env, 0, sizeof p->env);
        p->env.extreme = mode;
        p->env.node = leaf;
        if (!ytl_open(&tl, &td)) { snprintf(msg, cap, "timeline: %s", ytl_error(&tl)); rc = 1; break; }
        ytl_anchor(&tl, PL_BASE_TRIAL, 0, 0);
        nops = pl_build_ops(p, t->events, t->n_events, ops, bound, &cursor);
        if (nops < 0) { snprintf(msg, cap, "%s", p->error); p->failed = 0; rc = 1; break; }
        /* the events and after tables run on one timeline, so they are
         * checked as one: a `set` in events and a `to` in after on the same
         * channel fail at run time otherwise, a trial in */
        {
            int n2, c2;
            memset(&ops[nops], 0, sizeof ops[nops]);
            ops[nops].op = YTL_OP_AT;
            ops[nops].t_ns = cursor;
            c2 = nops + 1;
            cursor = 0;
            n2 = pl_build_ops(p, t->after, t->n_after, ops + c2, bound - c2, &cursor);
            if (n2 < 0) { snprintf(msg, cap, "%s", p->error); p->failed = 0; rc = 1; break; }
            nops = c2 + n2;
        }
        if (nops > 0) {
            int e = ytl_check_ops(&tl, PL_BASE_TRIAL, ops, nops, &bad);
            if (e != 0) { snprintf(msg, cap, "template '%s', op %d of its steps: %s", t->name, bad, ytl_strerror(e)); rc = 1; break; }
        }
    }
    }
    p->env = saved;
    return rc;
}

void pl_host_init(pl_player* p, yexp_host* h) {
    memset(h, 0, sizeof *h);
    h->ctx = p;
    h->entry = h_entry;
    h->column = h_column;
    h->level = h_level;
    h->param = h_param;
    h->rules = h_rules;
    h->check_ops = h_check_ops;
    if (p->have_rig) h->role = h_role;
}
