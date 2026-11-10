/* player/lower.c - from the checked form to the headers' structs.
 *
 * Everything that sizes something happens here, once, before the first
 * frame: channels, stimuli, collectors, op-table capacity, the data set's
 * columns. The frame path then fills and runs what was made here. Each
 * refusal names the element, so a definition this version cannot lower
 * stops with a message rather than running as something else.
 */
#include "player.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

void pl_fail(pl_player* p, const char* fmt, ...) {
    va_list ap;
    if (p->failed) return;
    p->failed = 1;
    va_start(ap, fmt);
    vsnprintf(p->error, sizeof p->error, fmt, ap);
    va_end(ap);
}

typedef struct pl_block { struct pl_block* next; } pl_block;

void* pl_alloc(pl_player* p, size_t n) {
    pl_block* b = (pl_block*)calloc(1, sizeof(pl_block) + n);
    if (!b) { pl_fail(p, "out of memory (%zu bytes)", n); return NULL; }
    b->next = (pl_block*)p->blocks;
    p->blocks = b;
    return (void*)(b + 1);
}

static char* pl_strdup(pl_player* p, const char* s) {
    size_t n = strlen(s) + 1;
    char* c = (char*)pl_alloc(p, n);
    if (c) memcpy(c, s, n);
    return c;
}

/* --- seeds (D8) --------------------------------------------------------- */

/* D8 says "a fixed hash of S, the node's path and its iteration" and "version
 * 1 pins the hash" without naming one: this is the pin. SHA-256 because the
 * pack header already carries it and a node starts rarely; the prefix keeps
 * it apart from any other use of the hash on the same bytes. */
uint64_t pl_node_seed(uint64_t session_seed, const char* path, const int* iters, int n) {
    ypak_sha256_state st;
    uint8_t out[32], b[8];
    int i, j;
    ypak_sha256_init(&st);
    ypak_sha256_update(&st, "ysp-node-seed\0", 14);
    for (i = 0; i < 8; i++) b[i] = (uint8_t)(session_seed >> (i * 8));
    ypak_sha256_update(&st, b, 8);
    ypak_sha256_update(&st, path, strlen(path));
    ypak_sha256_update(&st, "\0", 1);
    for (i = 0; i < n; i++) {
        uint32_t it = (uint32_t)iters[i];
        for (j = 0; j < 4; j++) b[j] = (uint8_t)(it >> (j * 8));
        ypak_sha256_update(&st, b, 4);
    }
    ypak_sha256_final(&st, out);
    {
        uint64_t s = 0;
        for (i = 0; i < 8; i++) s |= (uint64_t)out[i] << (i * 8);
        return s;
    }
}

/* --- tables ------------------------------------------------------------- */

const ytb_table* pl_table_get(pl_player* p, const char* name) {
    int i;
    ypak_entry e;
    const void* data;
    pl_table_ref* r;
    for (i = 0; i < p->n_tables; i++)
        if (strcmp(p->tables[i].name, name) == 0) return p->tables[i].table;
    if (ypak_find(&p->pack, name, &e) != 0) return NULL;
    if (e.kind != YPAK_KIND_TABLE) return NULL;
    if (!(data = ypak_data(&p->pack, &e))) { pl_fail(p, "the pack entry '%s' cannot be read: %s", name, ypak_error(&p->pack)); return NULL; }
    if (p->n_tables == p->c_tables) {
        int nc = p->c_tables ? p->c_tables * 2 : 16;
        pl_table_ref* q = (pl_table_ref*)pl_alloc(p, sizeof(pl_table_ref) * (size_t)nc);
        if (!q) return NULL;
        if (p->n_tables) memcpy(q, p->tables, sizeof(pl_table_ref) * (size_t)p->n_tables);
        p->tables = q;
        p->c_tables = nc;
    }
    r = &p->tables[p->n_tables];
    r->name = pl_strdup(p, name);
    r->table = (ytb_table*)pl_alloc(p, sizeof(ytb_table));
    if (!r->name || !r->table) return NULL;
    if (!ytb_open_view(r->table, data, (size_t)e.size)) {
        pl_fail(p, "the table '%s': %s", name, ytb_error(r->table));
        return NULL;
    }
    p->n_tables++;
    return r->table;
}

/* --- marks ------------------------------------------------------------- */

int pl_mark_index(pl_player* p, const char* name) {
    int i;
    for (i = 0; i < p->marks.n; i++)
        if (strcmp(p->marks.names[i], name) == 0) return i;
    if (p->marks.n == p->marks.cap) {
        int nc = p->marks.cap ? p->marks.cap * 2 : 16;
        const char** q = (const char**)pl_alloc(p, sizeof(char*) * (size_t)nc);
        if (!q) return -1;
        if (p->marks.n) memcpy(q, p->marks.names, sizeof(char*) * (size_t)p->marks.n);
        p->marks.names = q;
        p->marks.cap = nc;
    }
    p->marks.names[p->marks.n] = name;
    return p->marks.n++;
}

/* --- channels ----------------------------------------------------------- */

static int pl_chan_add(pl_player* p, const char* stim, const char* prop) {
    int i;
    const yexp_stim* s = yexp_stim_get(p->def, stim);
    if (!s) { pl_fail(p, "a step names the stimulus '%s', which does not exist", stim); return -1; }
    if (prop && strcmp(prop, "visible") == 0) prop = NULL;
    for (i = 0; i < p->n_channels; i++)
        if (p->channels[i].stim == s &&
            ((!prop && !p->channels[i].prop) || (prop && p->channels[i].prop && strcmp(prop, p->channels[i].prop) == 0)))
            return i;
    if (p->n_channels >= YTL_MAX_CHANNELS) {
        pl_fail(p, "the stimulus '%s': more than %d channels (each stimulus visibility and each driven property is one)", stim, YTL_MAX_CHANNELS);
        return -1;
    }
    p->channels[p->n_channels].stim = s;
    p->channels[p->n_channels].prop = prop;
    return p->n_channels++;
}

int pl_channel(pl_player* p, const char* stim, const char* prop) {
    int i;
    const yexp_stim* s = yexp_stim_get(p->def, stim);
    if (!s) return -1;
    if (prop && strcmp(prop, "visible") == 0) prop = NULL;
    for (i = 0; i < p->n_channels; i++)
        if (p->channels[i].stim == s &&
            ((!prop && !p->channels[i].prop) || (prop && p->channels[i].prop && strcmp(prop, p->channels[i].prop) == 0)))
            return i;
    return -1;
}

int pl_trigger_channel(pl_player* p, const char* role) {
    int i;
    for (i = 0; i < p->n_triggers; i++)
        if (strcmp(p->trigger_roles[i]->name, role) == 0) return i;
    return -1;
}

static void pl_chan_steps(pl_player* p, const yexp_step* st, int n) {
    int i, k;
    for (i = 0; i < n; i++) {
        const yexp_step* s = &st[i];
        switch (s->op) {
        case YEXP_ON: case YEXP_OFF:
            for (k = 0; k < s->n_stims; k++) pl_chan_add(p, s->stims[k], NULL);
            break;
        case YEXP_SET: case YEXP_TO: case YEXP_THEN:
            for (k = 0; k < s->n_props; k++) {
                if (strcmp(s->props[k].key, "color") == 0) {
                    pl_chan_add(p, s->stims[0], "color_r");
                    pl_chan_add(p, s->stims[0], "color_g");
                    pl_chan_add(p, s->stims[0], "color_b");
                } else pl_chan_add(p, s->stims[0], s->props[k].key);
            }
            break;
        case YEXP_MARK:
            pl_mark_index(p, s->mark);
            break;
        case YEXP_IF: case YEXP_UNLESS:
            pl_chan_steps(p, s->steps, s->n_steps);
            break;
        default:
            break;
        }
        if (p->failed) return;
    }
}

/* The channels and marks of the whole definition, in its own order: a
 * template's steps, so the assignment depends on the file alone (the host
 * check uses the same call before anything else is lowered). */
int pl_channels_assign(pl_player* p) {
    int i, bound = 0;
    if (p->channels_assigned) return 0;
    /* trigger roles, in the order of the role names (2.9): roles are sorted
     * by name in the form (D3). The host's check needs them too. */
    p->trigger_roles = (const yexp_role**)pl_alloc(p, sizeof(yexp_role*) * (size_t)(p->def->n_roles + 1));
    if (!p->trigger_roles) return -1;
    for (i = 0; i < p->def->n_roles; i++)
        if (p->def->roles[i].direction != 0) p->trigger_roles[p->n_triggers++] = &p->def->roles[i];
    if (p->n_triggers > YSCR_MAX_TRIGGERS) { pl_fail(p, "more than %d output roles", YSCR_MAX_TRIGGERS); return -1; }
    for (i = 0; i < p->def->n_templates; i++)
        bound += pl_ops_bound(p->def->templates[i].events, p->def->templates[i].n_events) +
                 pl_ops_bound(p->def->templates[i].after, p->def->templates[i].n_after);
    p->channels = (pl_chan*)pl_alloc(p, sizeof(pl_chan) * (size_t)(bound + p->def->n_stimuli + 8));
    if (!p->channels) return -1;
    p->channels_assigned = true;
    /* A response's `from` needs a visible channel even when no step turns it
     * off, so it can land; the loader requires a step that shows it. */
    for (i = 0; i < p->def->n_templates; i++) {
        const yexp_template* t = &p->def->templates[i];
        pl_chan_steps(p, t->events, t->n_events);
        pl_chan_steps(p, t->after, t->n_after);
        if (t->response) pl_chan_add(p, t->response->from, NULL);
    }
    return p->failed ? -1 : 0;
}

int pl_ops_bound(const yexp_step* st, int n) {
    int i, c = 0;
    for (i = 0; i < n; i++) {
        const yexp_step* s = &st[i];
        switch (s->op) {
        case YEXP_ON: case YEXP_OFF: c += 2 * s->n_stims + 2; break;
        case YEXP_IF: case YEXP_UNLESS: c += pl_ops_bound(s->steps, s->n_steps); break;
        case YEXP_SET: case YEXP_TO: case YEXP_THEN:
            c += 3 * s->n_props + 1;
            break;
        default: c += 1; break;
        }
    }
    return c;
}

/* --- op tables ---------------------------------------------------------- */

static int pl_ease_of(const char* name) {
    static const struct { const char* n; int e; } T[] = {
        { "linear", YTL_EASE_LINEAR }, { "step", YTL_EASE_STEP }, { "cosine", YTL_EASE_COSINE },
        { "quad_in", YTL_EASE_QUAD_IN }, { "quad_out", YTL_EASE_QUAD_OUT }, { "quad_in_out", YTL_EASE_QUAD_IN_OUT },
        { "log", YTL_EASE_LOG }, { "cubic_in", YTL_EASE_CUBIC_IN }, { "cubic_out", YTL_EASE_CUBIC_OUT },
        { "cubic_in_out", YTL_EASE_CUBIC_IN_OUT }, { "bezier", YTL_EASE_BEZIER },
    };
    size_t i;
    if (!name) return YTL_EASE_LINEAR;
    for (i = 0; i < sizeof T / sizeof T[0]; i++)
        if (strcmp(T[i].n, name) == 0) return T[i].e;
    return -1;
}

/* A number read from a step in extreme mode: the check needs a finite value
 * of the right sign, not the trial's. */
static int pl_step_num(pl_player* p, const yexp_value* v, double* out) {
    if (p->env.extreme) {
        if (v->kind == YEXP_V_NUMBER && !(v->flags & YEXP_VF_DURATION)) { *out = v->num; return 0; }
        *out = 0.5;
        return 0;
    }
    return pl_eval_num(p, &p->env, v, out);
}

static int pl_step_ns(pl_player* p, const yexp_value* v, int64_t* ns) {
    if (p->env.extreme) {
        if (pl_extreme_ns(p, v, ns) < 0) return -1;
        /* ytl_check_ops() takes a negative WAIT, but the run would refuse it
         * on the trial that draws it */
        if (*ns < 0) { pl_fail(p, "a step's duration can be %g seconds; a duration is not negative", (double)*ns / 1e9); return -1; }
        return 0;
    }
    return pl_eval_ns(p, &p->env, v, ns);
}

static int pl_add_tween(pl_player* p, const yexp_step* s, int ch, const yexp_value* to_v, int component, ytl_op* o) {
    ytl_tween_desc* d;
    int e;
    double to;
    int64_t dur = 0, delay = 0;
    memset(o, 0, sizeof *o);
    o->op = s->op == YEXP_THEN ? YTL_OP_THEN : YTL_OP_TO;
    o->ch = ch;
    d = &o->tween;
    if (component >= 0) {
        pl_val arr;
        if (p->env.extreme && to_v->kind != YEXP_V_ARRAY) to = 0.5;
        else {
            if (pl_eval(p, &p->env, to_v, &arr) < 0) return -1;
            if (arr.kind != YEXP_V_ARRAY || arr.n != 3) { pl_fail(p, "a color tween's value is not [r, g, b]"); return -1; }
            if (pl_step_num(p, &arr.items[component], &to) < 0) return -1;
        }
    } else if (pl_step_num(p, to_v, &to) < 0) return -1;
    d->to = (float)to;
    if (s->duration && pl_step_ns(p, s->duration, &dur) < 0) return -1;
    if (s->delay && pl_step_ns(p, s->delay, &delay) < 0) return -1;
    d->duration_ns = dur;
    d->delay_ns = delay;
    e = pl_ease_of(s->ease);
    if (e < 0) { pl_fail(p, "the ease '%s' is not one of ysp/timeline.h's", s->ease); return -1; }
    d->ease = e;
    return 0;
}

/* The ops of steps[0..n). `cursor` is the base time before them and after
 * them: a stagger needs it to return the cursor to its start. */
int pl_build_ops(pl_player* p, const yexp_step* st, int n, ytl_op* ops, int cap, int64_t* cursor) {
    int i, c = 0;
    for (i = 0; i < n; i++) {
        const yexp_step* s = &st[i];
        switch (s->op) {
        case YEXP_ON: case YEXP_OFF: {
            int64_t stagger = 0;
            int64_t t = *cursor;
            int k;
            if (s->stagger && pl_step_ns(p, s->stagger, &stagger) < 0) return -1;
            if (c + s->n_stims * 2 + 1 > cap) { pl_fail(p, "the op table is full"); return -1; }
            for (k = 0; k < s->n_stims; k++) {
                int ch = pl_channel(p, s->stims[k], NULL);
                ytl_op* o;
                if (ch < 0) { pl_fail(p, "'%s' has no channel", s->stims[k]); return -1; }
                if (k > 0 && stagger > 0) {
                    o = &ops[c++];
                    memset(o, 0, sizeof *o);
                    o->op = YTL_OP_WAIT;
                    o->t_ns = stagger;
                    t += stagger;
                }
                o = &ops[c++];
                memset(o, 0, sizeof *o);
                o->op = s->op == YEXP_ON ? YTL_OP_ON : YTL_OP_OFF;
                o->ch = ch;
            }
            if (t > p->ops_latest) p->ops_latest = t;
            if (t != *cursor) {   /* ytl_on_n() leaves the cursor where it was */
                ytl_op* o = &ops[c++];
                memset(o, 0, sizeof *o);
                o->op = YTL_OP_AT;
                o->t_ns = *cursor;
            }
            break;
        }
        case YEXP_WAIT: {
            ytl_op* o;
            int64_t dt;
            if (pl_step_ns(p, s->t, &dt) < 0) return -1;
            if (c + 1 > cap) { pl_fail(p, "the op table is full"); return -1; }
            o = &ops[c++];
            memset(o, 0, sizeof *o);
            o->op = YTL_OP_WAIT;
            o->t_ns = dt;
            *cursor += dt;
            if (*cursor > p->ops_latest) p->ops_latest = *cursor;
            break;
        }
        case YEXP_AT: {
            ytl_op* o;
            int64_t t;
            if (pl_step_ns(p, s->t, &t) < 0) return -1;
            if (c + 1 > cap) { pl_fail(p, "the op table is full"); return -1; }
            o = &ops[c++];
            memset(o, 0, sizeof *o);
            o->op = YTL_OP_AT;
            o->t_ns = t;
            *cursor = t;
            if (t > p->ops_latest) p->ops_latest = t;
            break;
        }
        case YEXP_SET: {
            int k;
            for (k = 0; k < s->n_props; k++) {
                int comp, n_comp = strcmp(s->props[k].key, "color") == 0 ? 3 : 1;
                for (comp = 0; comp < n_comp; comp++) {
                    static const char* const cn[3] = { "color_r", "color_g", "color_b" };
                    int ch = pl_channel(p, s->stims[0], n_comp == 3 ? cn[comp] : s->props[k].key);
                    ytl_op* o;
                    double val;
                    if (ch < 0) { pl_fail(p, "'%s.%s' has no channel", s->stims[0], s->props[k].key); return -1; }
                    if (n_comp == 3) {
                        if (p->env.extreme && s->props[k].value->kind != YEXP_V_ARRAY) val = 0.5;
                        else {
                            pl_val arr;
                            if (pl_eval(p, &p->env, s->props[k].value, &arr) < 0) return -1;
                            if (arr.kind != YEXP_V_ARRAY || arr.n != 3) { pl_fail(p, "'%s.color' is not [r, g, b]", s->stims[0]); return -1; }
                            if (pl_step_num(p, &arr.items[comp], &val) < 0) return -1;
                        }
                    } else if (pl_step_num(p, s->props[k].value, &val) < 0) return -1;
                    if (c + 1 > cap) { pl_fail(p, "the op table is full"); return -1; }
                    o = &ops[c++];
                    memset(o, 0, sizeof *o);
                    o->op = YTL_OP_SET_VALUE;
                    o->ch = ch;
                    o->value = (float)val;
                }
            }
            break;
        }
        case YEXP_TO: case YEXP_THEN: {
            int k;
            int64_t dur = 0, delay = 0;
            for (k = 0; k < s->n_props; k++) {
                int comp, n_comp = strcmp(s->props[k].key, "color") == 0 ? 3 : 1;
                for (comp = 0; comp < n_comp; comp++) {
                    static const char* const cn[3] = { "color_r", "color_g", "color_b" };
                    int ch = pl_channel(p, s->stims[0], n_comp == 3 ? cn[comp] : s->props[k].key);
                    if (ch < 0) { pl_fail(p, "'%s.%s' has no channel", s->stims[0], s->props[k].key); return -1; }
                    if (c + 1 > cap) { pl_fail(p, "the op table is full"); return -1; }
                    if (pl_add_tween(p, s, ch, s->props[k].value, n_comp == 3 ? comp : -1, &ops[c]) < 0) return -1;
                    /* a THEN moves the cursor once; a table of them would
                     * chain, so only the last of a step keeps the move */
                    if (s->op == YEXP_THEN && !(k == s->n_props - 1 && comp == n_comp - 1)) ops[c].op = YTL_OP_TO;
                    dur = ops[c].tween.duration_ns;
                    delay = ops[c].tween.delay_ns;
                    c++;
                }
            }
            /* 2.5: only `then` moves time. A `to` still running at the
             * trial's end is cut there, so it does not hold back `after` */
            if (s->op == YEXP_THEN) {
                *cursor += delay + dur;
                if (*cursor > p->ops_latest) p->ops_latest = *cursor;
            }
            break;
        }
        case YEXP_TRIGGER: {
            ytl_op* o;
            int ch = pl_trigger_channel(p, s->role);
            int64_t code = 0;
            if (ch < 0) { pl_fail(p, "the trigger step's role '%s' is not an output role", s->role); return -1; }
            if (p->env.extreme) code = 1;
            else if (pl_eval_int(p, &p->env, s->code, &code) < 0) return -1;
            if (code < 0 || code > 255) {
                /* a pulse value is a port's byte; the headers take int32 but
                 * no output takes more */
                pl_fail(p, "the trigger code %lld is not in 0..255", (long long)code);
                return -1;
            }
            if (c + 1 > cap) { pl_fail(p, "the op table is full"); return -1; }
            o = &ops[c++];
            memset(o, 0, sizeof *o);
            o->op = YTL_OP_TRIGGER;
            o->ch = ch;
            o->code = (int32_t)code;
            break;
        }
        case YEXP_MARK: {
            ytl_op* o;
            int idx = pl_mark_index(p, s->mark);
            if (idx < 0) return -1;
            if (c + 1 > cap) { pl_fail(p, "the op table is full"); return -1; }
            o = &ops[c++];
            memset(o, 0, sizeof *o);
            o->op = YTL_OP_MARK;
            o->code = idx;
            break;
        }
        case YEXP_IF: case YEXP_UNLESS: {
            bool cond = true;
            if (!p->env.extreme) {
                pl_val x;
                if (pl_var(p, &p->env, s->cond, &x) < 0) return -1;
                if (x.kind == YEXP_V_BOOL) cond = x.i != 0;
                else if (x.kind == YEXP_V_NUMBER && (x.num == 0 || x.num == 1)) cond = x.num == 1;
                else { pl_fail(p, "'$%s' in an 'if' is neither true nor false", s->cond); return -1; }
                if (s->op == YEXP_UNLESS) cond = !cond;
            }
            if (cond) {   /* in extreme mode both branches are in the check */
                int r = pl_build_ops(p, s->steps, s->n_steps, ops + c, cap - c, cursor);
                if (r < 0) return -1;
                c += r;
            }
            break;
        }
        default:
            pl_fail(p, "unknown step");
            return -1;
        }
    }
    return c;
}

/* --- stimuli ------------------------------------------------------------- */

static int pl_kind_gfx(yexp_stim_kind k) {
    switch (k) {
    case YEXP_SHAPE:   return YGFX_SHAPE;
    case YEXP_GRATING: return YGFX_GRATING;
    case YEXP_GABOR:   return YGFX_GABOR;
    case YEXP_NOISE:   return YGFX_NOISE;
    case YEXP_IMAGE:   return YGFX_IMAGE;
    case YEXP_TEXT: case YEXP_ARTWORK: return YGFX_TEXT;
    case YEXP_RDK:     return YGFX_DOTS;
    default:           return YGFX_KIND_NONE;
    }
}

/* Whether a value depends on a variable. A gamut is a session constant. */
static bool pl_value_static(const yexp_value* v) {
    uint32_t i;
    switch (v->kind) {
    case YEXP_V_NUMBER: case YEXP_V_BOOL: case YEXP_V_STRING: case YEXP_V_GAMUT: case YEXP_V_NONE:
        return true;
    case YEXP_V_ARRAY:
        for (i = 0; i < v->n; i++) if (!pl_value_static(&v->items[i])) return false;
        return true;
    default:
        return false;
    }
}

static const ygfx_param* pl_gfx_row(const char* key, int gkind, int* index) {
    int n, i;
    const ygfx_param* r = ygfx_params(&n);
    for (i = 0; i < n; i++)
        if (strcmp(r[i].name, key) == 0 && (r[i].kinds & (1u << gkind))) { if (index) *index = i; return &r[i]; }
    return NULL;
}

static const ygfx_param* pl_gfx_enum_row(const char* key, int gkind, int* index) {
    int n, i;
    const ygfx_param* r = ygfx_enum_params(&n);
    for (i = 0; i < n; i++)
        if (strcmp(r[i].name, key) == 0 && (r[i].kinds & (1u << gkind))) { if (index) *index = i; return &r[i]; }
    return NULL;
}

static int pl_enum_value(const char* values, const char* name) {
    const char* s = values;
    size_t n = strlen(name);
    while (s && *s) {
        const char* e = strchr(s, '=');
        const char* c;
        if (!e) return -1;
        c = strchr(e, ',');
        if ((size_t)(e - s) == n && strncmp(s, name, n) == 0) return atoi(e + 1);
        if (!c) return -1;
        s = c + 1;
    }
    return -1;
}

/* Whether a value depends on the response (known only when `after` is
 * built): its key waits for that moment (2.7). */
bool pl_value_after(const yexp_value* v) {
    uint32_t i;
    if (!v) return false;
    switch (v->kind) {
    case YEXP_V_VAR:
        return strcmp(v->str, "response") == 0 || strcmp(v->str, "rt_s") == 0 || strcmp(v->str, "responded") == 0 ||
               strcmp(v->str, "correct") == 0 || strcmp(v->str, "outcome") == 0;
    case YEXP_V_LOOKUP:
        if (pl_value_after(v->by)) return true;
        for (i = 0; i < v->n; i++) if (pl_value_after(v->map[i].value)) return true;
        return false;
    case YEXP_V_ARRAY: case YEXP_V_CHOICE: case YEXP_V_LINSPACE:
        for (i = 0; i < v->n; i++) if (pl_value_after(&v->items[i])) return true;
        return false;
    case YEXP_V_MICHELSON:
        return pl_value_after(v->by);
    default:
        return false;
    }
}

/* Apply one key to the stimulus: `pass` is PL_KEY_LOAD (static keys), then
 * PL_KEY_TRIAL or PL_KEY_AFTER for the keys that hold a variable, split by
 * when that variable is known. */
static int pl_apply_key(pl_player* p, pl_stim* s, const yexp_prop* pr, int pass) {
    const char* key = pr->key;
    int gkind = pl_kind_gfx(s->def->kind);
    const yexp_value* v = pr->value;
    bool st = pl_value_static(v);
    if (pass == PL_KEY_LOAD && !st) return 0;
    if (pass != PL_KEY_LOAD && st) return 0;
    if (pass == PL_KEY_TRIAL && pl_value_after(v)) return 0;
    if (pass == PL_KEY_AFTER && !pl_value_after(v)) return 0;

    /* keys that are not fields of ygfx_stim */
    if (strcmp(key, "layer") == 0 || strcmp(key, "kind") == 0) return 0;
    /* Format gap: `visible` is a key and also what on/off drive. A stimulus
     * that no step shows is not drawn at all (the draw skips it), so the key
     * is ignored; if a step shows it, the channel owns visibility. */
    if (strcmp(key, "visible") == 0) return 0;
    if (strcmp(key, "color") == 0 && s->def->kind != YEXP_SOUND) {
        pl_val a;
        double r, g, b;
        if (pl_eval(p, &p->env, v, &a) < 0) return -1;
        if (a.kind != YEXP_V_ARRAY || a.n != 3) { pl_fail(p, "stimulus '%s': 'color' is not [r, g, b]", s->def->name); return -1; }
        if (pl_eval_num(p, &p->env, &a.items[0], &r) < 0 || pl_eval_num(p, &p->env, &a.items[1], &g) < 0 ||
            pl_eval_num(p, &p->env, &a.items[2], &b) < 0) return -1;
        s->gs.color[0] = (float)r;
        s->gs.color[1] = (float)g;
        s->gs.color[2] = (float)b;
        return 0;
    }
    if (strcmp(key, "contrast") == 0 && v->kind == YEXP_V_MICHELSON) return 0;   /* pl_stim_begin_trial() */

    if (s->def->kind == YEXP_RDK) {
        if (strcmp(key, "coherence") == 0 || strcmp(key, "direction") == 0 || strcmp(key, "speed") == 0) {
            double d;
            float* f = key[0] == 'c' ? &s->field.coherence : key[0] == 'd' ? &s->field.direction : &s->field.speed;
            if (pl_eval_num(p, &p->env, v, &d) < 0) return -1;
            *f = (float)d;
            return 0;
        }
        if (strcmp(key, "edge") == 0 || strcmp(key, "aperture") == 0 || strcmp(key, "count") == 0 || strcmp(key, "density") == 0 ||
            strcmp(key, "w") == 0 || strcmp(key, "h") == 0 || strcmp(key, "signal") == 0 || strcmp(key, "select") == 0 ||
            strcmp(key, "noise") == 0 || strcmp(key, "sets") == 0 || strcmp(key, "lifetime_s") == 0 || strcmp(key, "lifetime_frames") == 0 ||
            strcmp(key, "clock") == 0 || strcmp(key, "frame_s") == 0 || strcmp(key, "algorithm") == 0) {
            if (!st) { pl_fail(p, "stimulus '%s': '%s' on an rdk must be a literal: the field is opened at load", s->def->name, key); return -1; }
            return 0;   /* read by pl_rdk_open() */
        }
    }
    if (s->def->kind == YEXP_TEXT &&
        (strcmp(key, "font") == 0 || strcmp(key, "text") == 0 || strcmp(key, "size") == 0 || strcmp(key, "width") == 0 ||
         strcmp(key, "line_height") == 0 || strcmp(key, "align") == 0 || strcmp(key, "dir") == 0 || strcmp(key, "lang") == 0))
        return 0;
    if (s->def->kind == YEXP_IMAGE && strcmp(key, "texture") == 0) return 0;
    if (s->def->kind == YEXP_ARTWORK && (strcmp(key, "artwork") == 0 || strcmp(key, "size") == 0)) return 0;
    if (s->def->kind == YEXP_SOUND) return 0;

    {
        int idx;
        const ygfx_param* r = pl_gfx_row(key, gkind, &idx);
        if (r) {
            double d;
            if (pl_eval_num(p, &p->env, v, &d) < 0) return -1;
            *(float*)((char*)&s->gs + r->offset) = (float)d;
            return 0;
        }
        r = pl_gfx_enum_row(key, gkind, &idx);
        if (r) {
            int value = 0;
            pl_val x;
            if (pl_eval(p, &p->env, v, &x) < 0) return -1;
            if (x.kind == YEXP_V_STRING) {
                value = r->values ? pl_enum_value(r->values, x.str) : -1;
                if (value < 0) { pl_fail(p, "stimulus '%s': '%s' has no value '%s'", s->def->name, key, x.str); return -1; }
            } else if (x.kind == YEXP_V_NUMBER || x.kind == YEXP_V_BOOL) {
                if (strcmp(key, "seed") == 0) value = (int)(x.i & 0xffffff);
                else value = (int)x.num;
            } else { pl_fail(p, "stimulus '%s': '%s' has no value", s->def->name, key); return -1; }
            if (ygfx_enum_set(&s->gs, idx, value) != YGFX_OK) {
                pl_fail(p, "stimulus '%s': '%s' = %d is refused by ysp/gfx.h", s->def->name, key, value);
                return -1;
            }
            return 0;
        }
    }
    /* a key the loader accepted that this version has no lowering for: named,
     * not dropped */
    pl_fail(p, "stimulus '%s' (%d): the key '%s' is not lowered by this player", s->def->name, (int)s->def->kind, key);
    return -1;
}

/* One key now, whatever its phase: a michelson stimulus's direction. */
int pl_apply_prop(pl_player* p, pl_stim* s, const yexp_prop* pr) {
    return pl_value_static(pr->value) ? 0 : pl_apply_key(p, s, pr, PL_KEY_TRIAL);
}

int pl_apply_writes(pl_player* p, pl_stim* s, int pass) {
    int i;
    for (i = 0; i < s->def->n_props; i++)
        if (pl_apply_key(p, s, &s->def->props[i], pass) < 0) return -1;
    return 0;
}

static int pl_prop_int(pl_player* p, const yexp_stim* s, const char* key, int64_t* out) {
    const yexp_prop* pr = yexp_prop_get(s->props, s->n_props, key);
    if (!pr) { *out = 0; return 0; }
    return pl_eval_int(p, &p->env, pr->value, out);
}

static int pl_prop_num(pl_player* p, const yexp_stim* s, const char* key, double* out) {
    const yexp_prop* pr = yexp_prop_get(s->props, s->n_props, key);
    if (!pr) { *out = 0; return 0; }
    return pl_eval_num(p, &p->env, pr->value, out);
}

static int pl_prop_ns(pl_player* p, const yexp_stim* s, const char* key, int64_t* out) {
    const yexp_prop* pr = yexp_prop_get(s->props, s->n_props, key);
    if (!pr) { *out = 0; return 0; }
    return pl_eval_ns(p, &p->env, pr->value, out);
}

static const char* pl_prop_str(pl_player* p, const yexp_stim* s, const char* key) {
    const yexp_prop* pr = yexp_prop_get(s->props, s->n_props, key);
    pl_val v;
    if (!pr) return NULL;
    if (pl_eval(p, &p->env, pr->value, &v) < 0) return NULL;
    return v.kind == YEXP_V_STRING ? v.str : NULL;
}

static int pl_rdk_open(pl_player* p, pl_stim* s) {
    const yexp_stim* d = s->def;
    yrdk_desc rd;
    ygfx_dots_desc dd;
    int64_t count = 0, sets = 0, lf = 0, stream = 0;
    double density = 0, w = 0, h = 0, coh = 0, dir = 0, speed = 0;
    const char* v;
    memset(&rd, 0, sizeof rd);
    /* the field is opened at load, before any trial has a value */
    {
        static const char* const lit[] = { "count", "density", "sets", "w", "h", "lifetime_s", "lifetime_frames", "frame_s" };
        size_t i;
        for (i = 0; i < sizeof lit / sizeof lit[0]; i++) {
            const yexp_prop* pr = yexp_prop_get(d->props, d->n_props, lit[i]);
            if (pr && !pl_value_static(pr->value)) { pl_fail(p, "stimulus '%s': '%s' on an rdk must be a literal: the field is opened at load", d->name, lit[i]); return -1; }
        }
    }
    if (pl_prop_int(p, d, "count", &count) < 0 || pl_prop_num(p, d, "density", &density) < 0 ||
        pl_prop_int(p, d, "sets", &sets) < 0 || pl_prop_num(p, d, "w", &w) < 0 || pl_prop_num(p, d, "h", &h) < 0 ||
        pl_prop_int(p, d, "lifetime_frames", &lf) < 0)
        return -1;
    rd.count = (int)count;
    rd.density = (float)density;
    rd.sets = (int)sets;
    rd.w = (float)w;
    rd.h = (float)h;
    rd.lifetime_frames = (int)lf;
    if (pl_prop_ns(p, d, "lifetime_s", &rd.lifetime_ns) < 0 || pl_prop_ns(p, d, "frame_s", &rd.frame_ns) < 0) return -1;
    /* literal floats of the first frame; the channels and the trial's writes
     * take over before the first update */
    if (pl_value_static(yexp_prop_get(d->props, d->n_props, "coherence")->value) && pl_prop_num(p, d, "coherence", &coh) < 0) return -1;
    if (pl_value_static(yexp_prop_get(d->props, d->n_props, "direction")->value) && pl_prop_num(p, d, "direction", &dir) < 0) return -1;
    if (pl_value_static(yexp_prop_get(d->props, d->n_props, "speed")->value) && pl_prop_num(p, d, "speed", &speed) < 0) return -1;
    rd.coherence = (float)coh;
    rd.direction = (float)dir;
    rd.speed = (float)speed;
#define RD_ENUM(kname, field, typ) \
    if ((v = pl_prop_str(p, d, kname)) != NULL) { \
        const yexp_row* row_; int rn_; \
        const yexp_row* rows_ = yexp_defaults(1, &rn_); \
        int ri_, val_ = -1; \
        for (ri_ = 0; ri_ < rn_; ri_++) { \
            row_ = &rows_[ri_]; \
            if (row_->values && (row_->kinds & 0x0080u) && strcmp(row_->key, kname) == 0) { val_ = pl_enum_value(row_->values, v); break; } \
        } \
        if (val_ < 0) { pl_fail(p, "stimulus '%s': '%s' has no value '%s'", d->name, kname, v); return -1; } \
        rd.field = (typ)val_; \
    }
    RD_ENUM("aperture", aperture, yrdk_shape)
    RD_ENUM("signal", signal, yrdk_signal)
    RD_ENUM("select", select, yrdk_select)
    RD_ENUM("noise", noise, yrdk_noise)
    RD_ENUM("edge", edge, yrdk_edge)
    RD_ENUM("clock", clock, yrdk_clock)
    RD_ENUM("algorithm", algorithm, yrdk_algorithm)
#undef RD_ENUM
    rd.stream = (uint32_t)stream;
    if (!yrdk_open(&s->field, &rd)) { pl_fail(p, "stimulus '%s': %s", d->name, yrdk_error(&s->field)); return -1; }
    /* the field's own count: with `density` it is density x area (COUNTS) */
    s->rdk_count = s->field.n;
    s->xy = (float*)pl_alloc(p, sizeof(float) * 2 * (size_t)s->rdk_count);
    if (!s->xy) return -1;   /* the steps are sized from the templates, later */
    s->buf = ygfx_buffer(&p->gfx, sizeof(float) * 2 * (size_t)s->rdk_count);
    memset(&dd, 0, sizeof dd);
    dd.buf = s->buf;
    dd.count = (uint32_t)s->rdk_count;
    dd.aperture = rd.aperture == YRDK_RECT ? YGFX_RECT : YGFX_CIRCLE;
    dd.w = (float)w;
    dd.h = (float)h;
    s->gs = ygfx_dots(&dd);
    return 0;
}

/* Load a TEXTURE entry once; a variable's levels come through here at load. */
static int pl_texture_get(pl_player* p, const char* name, ygfx_tex* out) {
    int i;
    ypak_entry e;
    ypak_texture t;
    ygfx_texture_desc d;
    const void* data;
    char err[300];
    void* decoded = NULL;
    for (i = 0; i < p->n_tex; i++)
        if (strcmp(p->tex_names[i], name) == 0) { *out = p->tex[i]; return 0; }
    if (p->n_tex == PL_MAX_TEXTURES) { pl_fail(p, "more than %d textures", PL_MAX_TEXTURES); return -1; }
    if (ypak_find(&p->pack, name, &e) != 0 || e.kind != YPAK_KIND_TEXTURE || !(data = ypak_data(&p->pack, &e))) {
        pl_fail(p, "the texture '%s' is not a pack entry", name);
        return -1;
    }
    if (ypak_texture_view(data, e.size, &t, err, sizeof err) != 0) { pl_fail(p, "the texture '%s': %s", name, err); return -1; }
    memset(&d, 0, sizeof d);
    d.w = (int32_t)t.w;
    d.h = (int32_t)t.h;
    d.format = (ygfx_format)t.format;
    memcpy(&d.enc, t.enc, sizeof d.enc);
    if (t.compression == YPAK_TEX_RAW) d.data = t.data;
    else {
        decoded = pl_alloc(p, (size_t)t.raw_bytes);
        if (!decoded) return -1;
        if (ypak_qoi_decode(&t, decoded, (size_t)t.raw_bytes, err, sizeof err) != 0) { pl_fail(p, "the texture '%s': %s", name, err); return -1; }
        d.data = decoded;
    }
    p->tex[p->n_tex] = ygfx_texture(&p->gfx, &d);
    if (!p->tex[p->n_tex].id) { pl_fail(p, "the texture '%s': %s", name, ygfx_error(&p->gfx)); return -1; }
    p->tex_names[p->n_tex] = pl_strdup(p, name);
    *out = p->tex[p->n_tex++];
    return 0;
}

/* Every text a value can take, into cb: the levels of a table column, the
 * map's values, a `vars` value. Used to preload textures by entry name. */
static void pl_each_text(pl_player* p, const yexp_value* v, const yexp_node* scope, void (*cb)(pl_player*, const char*)) {
    uint32_t i;
    if (!v) return;
    switch (v->kind) {
    case YEXP_V_STRING: cb(p, v->str); return;
    case YEXP_V_LOOKUP:
        for (i = 0; i < v->n; i++) pl_each_text(p, v->map[i].value, scope, cb);
        return;
    case YEXP_V_VAR: {
        const yexp_node* n;
        for (n = scope; n; n = n->parent) {
            int k;
            for (k = 0; k < n->n_vars; k++)
                if (strcmp(n->vars[k].key, v->str) == 0) { pl_each_text(p, n->vars[k].value, n, cb); return; }
        }
        for (n = scope; n; n = n->parent) {
            const ytb_table* tb = n->table ? pl_table_get(p, n->table) : NULL;
            int col, l, nl;
            if (!tb) continue;
            col = ytb_col(tb, v->str);
            if (col < 0 || ytb_col_type(tb, col) != YTB_STRING) continue;
            nl = ytb_n_levels(tb, col);
            for (l = 0; l < nl; l++) {
                const char* lv = ytb_level_text(tb, col, l);
                if (lv) cb(p, lv);
            }
        }
        return;
    }
    case YEXP_V_CHOICE: case YEXP_V_ARRAY:
        for (i = 0; i < v->n; i++) pl_each_text(p, &v->items[i], scope, cb);
        return;
    default: return;
    }
}

static void pl_preload_tex_cb(pl_player* p, const char* name) {
    ygfx_tex t;
    if (!p->failed) (void)pl_texture_get(p, name, &t);
}

/* The widest number of runs any block has, for a text stimulus's array. */
static int pl_runs_max(const ypak_runs* r) {
    uint32_t i;
    uint32_t m = 1;
    for (i = 0; i < r->n_blocks; i++) if (r->blocks[i].n_runs > m) m = r->blocks[i].n_runs;
    return (int)m;
}

static int pl_runs_open(pl_player* p) {
    ypak_entry ex, e;
    char name[PL_MAX_PATH];
    const void* data;
    char err[300];
    uint32_t i;
    size_t ln;
    if (p->runs_open) return 0;
    if (ypak_first_of(&p->pack, YPAK_KIND_EXPERIMENT, &ex) != 0) { pl_fail(p, "no experiment entry"); return -1; }
    ln = ex.name_len;
    while (ln > 0 && ex.name[ln - 1] != '.') ln--;
    if (ln == 0 || ln + 8 >= sizeof name) { pl_fail(p, "the experiment entry's name has no extension"); return -1; }
    memcpy(name, ex.name, ln);
    memcpy(name + ln, "ysprun", 7);
    if (ypak_find(&p->pack, name, &e) != 0 || e.kind != YPAK_KIND_GLYPHRUNS || !(data = ypak_data(&p->pack, &e))) {
        pl_fail(p, "the pack has no glyph runs entry '%s'", name);
        return -1;
    }
    if (ypak_runs_view(data, e.size, &p->runs, err, sizeof err) != 0) { pl_fail(p, "'%s': %s", name, err); return -1; }
    p->csets = (ygfx_cset*)pl_alloc(p, sizeof(ygfx_cset) * (p->runs.n_fonts + 1));
    if (!p->csets) return -1;
    for (i = 0; i < p->runs.n_fonts; i++) {
        ypak_entry ce;
        ypak_cset cs;
        ygfx_cset_desc d;
        const ypak_runs_font* f = &p->runs.fonts[i];
        const void* cd;
        if (ypak_find_n(&p->pack, ypak_runs_str(&p->runs, f->cset_off), f->cset_len, &ce) != 0 ||
            ce.kind != YPAK_KIND_CURVESET || !(cd = ypak_data(&p->pack, &ce))) {
            pl_fail(p, "font %u of the glyph runs: no curve set entry", i);
            return -1;
        }
        if (ypak_cset_view(cd, ce.size, &cs, err, sizeof err) != 0) { pl_fail(p, "curve set: %s", err); return -1; }
        memset(&d, 0, sizeof d);
        d.texels = cs.texels;
        d.n_texels = cs.n_texels;
        d.words = cs.words;
        d.n_words = cs.n_words;
        p->csets[i] = ygfx_cset_make(&p->gfx, &d);
    }
    p->runs_open = true;
    return 0;
}

/* Text: the block for this trial's text, one ygfx_crun per run (D14). */
int pl_stim_set_text(pl_player* p, pl_stim* s) {
    char* text = p->text_buf;
    char* key = p->runs_key;
    size_t key_cap = 2 * p->max_text + 1024;
    int64_t klen;
    int b;
    uint32_t r;
    const ypak_runs_block_rec* block;
    if (pl_eval_text(p, &p->env, s->text_value, text, p->max_text + 1) < 0) return -1;
    klen = ypak_runs_key(&s->style, text, strlen(text), key, key_cap);
    if (klen < 0 || (size_t)klen >= key_cap) { pl_fail(p, "stimulus '%s': the glyph run key does not fit", s->def->name); return -1; }
    b = ypak_runs_find(&p->runs, key);
    if (b < 0) { pl_fail(p, "stimulus '%s': no glyph run block for the text '%s'", s->def->name, text); return -1; }
    block = &p->runs.blocks[b];
    s->text_box[0] = block->w;
    s->text_box[1] = block->h;
    s->n_runs = 0;
    for (r = 0; r < block->n_runs; r++) {
        const ypak_runs_run* run = &p->runs.runs[block->run_first + r];
        ygfx_crun_desc d;
        if (!run->n || run->font >= p->runs.n_fonts) continue;
        memset(&d, 0, sizeof d);
        d.set = p->csets[run->font];
        d.items = (const ygfx_citem*)(const void*)(p->runs.items + 8 * (size_t)(block->item_first + run->first));
        d.n = (int)run->n;
        d.size = run->size;
        d.w = block->w > 1 ? block->w : 1;
        d.h = block->h > 1 ? block->h : 1;
        s->text_runs[s->n_runs++] = ygfx_crun(&p->gfx, &d);
    }
    return 0;
}

static int pl_stim_make(pl_player* p, pl_stim* s) {
    const yexp_stim* d = s->def;
    memset(&s->gs, 0, sizeof s->gs);
    s->sound_scheduled = -1;
    s->sound_id = 0;
    switch (d->kind) {
    case YEXP_SHAPE: { ygfx_shape_desc x; memset(&x, 0, sizeof x); s->gs = ygfx_shape(&x); break; }
    case YEXP_GRATING: { ygfx_grating_desc x; memset(&x, 0, sizeof x); s->gs = ygfx_grating(&x); break; }
    case YEXP_GABOR: { ygfx_gabor_desc x; memset(&x, 0, sizeof x); s->gs = ygfx_gabor(&x); break; }
    case YEXP_NOISE: { ygfx_noise_desc x; memset(&x, 0, sizeof x); s->gs = ygfx_noise(&x); break; }
    case YEXP_IMAGE: { ygfx_image_desc x; memset(&x, 0, sizeof x); s->gs = ygfx_image(&p->gfx, &x); break; }
    case YEXP_TEXT: {
        const yexp_prop* f = yexp_prop_get(d->props, d->n_props, "font");
        const yexp_prop* t = yexp_prop_get(d->props, d->n_props, "text");
        double size = 0, width = 0, lh = 0;
        const char* align, *dir, *lang;
        if (pl_runs_open(p) < 0) return -1;
        s->text_value = t->value;
        if (pl_prop_num(p, d, "size", &size) < 0 || pl_prop_num(p, d, "width", &width) < 0 || pl_prop_num(p, d, "line_height", &lh) < 0)
            return -1;
        align = pl_prop_str(p, d, "align");
        dir = pl_prop_str(p, d, "dir");
        lang = pl_prop_str(p, d, "lang");
        memset(&s->style, 0, sizeof s->style);
        s->style.font = f->value->str;
        s->style.size = size;
        s->style.width = width;
        s->style.line_height = lh;
        s->style.align = align && strcmp(align, "end") == 0 ? YPAK_ALIGN_END : align && strcmp(align, "center") == 0 ? YPAK_ALIGN_CENTER : YPAK_ALIGN_START;
        s->style.dir = dir && strcmp(dir, "ltr") == 0 ? YPAK_DIR_LTR : dir && strcmp(dir, "rtl") == 0 ? YPAK_DIR_RTL : YPAK_DIR_AUTO;
        s->style.lang = lang ? lang : "";
        s->max_runs = pl_runs_max(&p->runs);
        s->text_runs = (ygfx_stim*)pl_alloc(p, sizeof(ygfx_stim) * (size_t)s->max_runs);
        if (!s->text_runs) return -1;
        { ygfx_crun_desc x; memset(&x, 0, sizeof x); x.size = 1.0f; (void)x; }
        {
            ygfx_shape_desc x;
            memset(&x, 0, sizeof x);
            s->gs = ygfx_shape(&x);    /* a shadow: bind rows land here, then
                                        * pl_frame copies them to the runs */
            s->gs.kind = YGFX_TEXT;
        }
        s->gs.opacity = 1.0f;
        break;
    }
    case YEXP_ARTWORK:
        pl_fail(p, "stimulus '%s': artwork is not lowered by this player (not exercised by any worked example; ygfx_crun over the ARTWORK entry)", d->name);
        return -1;
    case YEXP_RDK:
        if (pl_rdk_open(p, s) < 0) return -1;
        break;
    case YEXP_SOUND: {
        const yexp_prop* a = yexp_prop_get(d->props, d->n_props, "audio");
        ypak_entry e;
        yau_wav_desc w;
        const void* data;
        if (!p->audio_open) {
            yau_desc ad;
            memset(&ad, 0, sizeof ad);
            /* --sim: the null device on the virtual clock; a real run: the
             * one AUTO picks (ysp/audio.h, BACKENDS) */
            ad.backend = p->args.sim ? YAU_BACKEND_NULL : YAU_BACKEND_AUTO;
            if (!yau_open(&p->audio, &ad)) { pl_fail(p, "audio: %s", yau_error(&p->audio)); return -1; }
            p->audio_open = true;
        }
        if (!a || a->value->kind != YEXP_V_STRING) { pl_fail(p, "stimulus '%s': a sound's 'audio' must be a literal entry name", d->name); return -1; }
        if (ypak_find(&p->pack, a->value->str, &e) != 0 || e.kind != YPAK_KIND_AUDIO || !(data = ypak_data(&p->pack, &e))) {
            pl_fail(p, "stimulus '%s': no audio entry '%s'", d->name, a->value->str);
            return -1;
        }
        memset(&w, 0, sizeof w);
        w.data = data;
        w.size = (size_t)e.size;
        s->sound = yau_wav_load(&p->audio, &w);
        if (!s->sound.frames) { pl_fail(p, "stimulus '%s': %s", d->name, yau_error(&p->audio)); return -1; }
        break;
    }
    case YEXP_VIDEO:
        pl_fail(p, "stimulus '%s': video is not lowered by this player: the simulated display has no GL path for ysp/video.h", d->name);
        return -1;
    default:
        pl_fail(p, "stimulus '%s': unknown kind", d->name);
        return -1;
    }
    {
        const yexp_prop* mp = yexp_prop_get(d->props, d->n_props, "contrast");
        s->michelson = mp && mp->value->kind == YEXP_V_MICHELSON ? mp->value->by : NULL;
    }
    return 0;
}

/* --- methods ------------------------------------------------------------- */

static double pl_stair_next(pl_method* m) { return yst_next(m->stair); }
static int pl_stair_update(pl_method* m, double x, int c) { return yst_update(m->stair, x, c); }
static double pl_stair_done(pl_method* m) { return yst_done(m->stair) ? 1.0 : 0.0; }
static double pl_stair_est(pl_method* m) {
    return yst_estimate(m->stair, strcmp(m->def->estimate, "trials") == 0 ? YST_EST_TRIALS : YST_EST_REVERSALS);
}
static double pl_quest_next(pl_method* m) { return yqst_stim_value(m->quest, yqst_next(m->quest), 0); }
static int pl_quest_update(pl_method* m, double x, int c) { return yqst_update_values(m->quest, &x, c); }
static double pl_quest_done(pl_method* m) { return yqst_done(m->quest) ? 1.0 : 0.0; }
static double pl_quest_est(pl_method* m) {
    double est[8];
    int how = strcmp(m->def->estimate, "mode") == 0 ? YQST_EST_MODE : YQST_EST_MEAN;
    if (yqst_estimate(m->quest, how, est) < 0) return NAN;
    return est[0];
}
bool pl_track_done(void* ctx) {
    pl_method* m = (pl_method*)ctx;
    return m->done(m) != 0.0;
}

/* A number of a method's prop; a gamut in the method's domain (2.8). */
static int pl_method_num(pl_player* p, const yexp_method* d, const yexp_value* v, double* out) {
    if (pl_eval_num(p, &p->env, v, out) < 0) return -1;
    if (d->log10 && v->kind == YEXP_V_GAMUT) *out = log10(*out);
    return 0;
}

static int pl_get_num(pl_player* p, const yexp_method* d, const char* key, double* out) {
    const yexp_prop* pr = yexp_prop_get(d->props, d->n_props, key);
    if (!pr) { *out = 0; return 0; }
    return pl_method_num(p, d, pr->value, out);
}

static int pl_get_int(pl_player* p, const yexp_method* d, const char* key, int* out) {
    const yexp_prop* pr = yexp_prop_get(d->props, d->n_props, key);
    double x;
    if (!pr) { *out = 0; return 0; }
    if (pl_eval_num(p, &p->env, pr->value, &x) < 0) return -1;
    *out = (int)x;
    return 0;
}

static int pl_get_bool(pl_player* p, const yexp_method* d, const char* key, bool* out) {
    const yexp_prop* pr = yexp_prop_get(d->props, d->n_props, key);
    if (!pr) { *out = false; return 0; }
    return pl_eval_bool(p, &p->env, pr->value, out);
}

static int pl_get_enum(pl_player* p, const yexp_method* d, const char* key, const char* table, int* out) {
    const yexp_prop* pr = yexp_prop_get(d->props, d->n_props, key);
    pl_val v;
    if (!pr) { *out = 0; return 0; }
    if (pl_eval(p, &p->env, pr->value, &v) < 0) return -1;
    if (v.kind != YEXP_V_STRING || (*out = pl_enum_value(table, v.str)) < 0) {
        pl_fail(p, "method '%s': '%s' has no such value", d->name, key);
        return -1;
    }
    return 0;
}

static int pl_get_axis(pl_player* p, const yexp_method* d, const yexp_value* v, yqst_axis* out) {
    if (v->kind == YEXP_V_LINSPACE) {
        double lo, hi, n;
        if (pl_method_num(p, d, &v->items[0], &lo) < 0 || pl_method_num(p, d, &v->items[1], &hi) < 0 ||
            pl_eval_num(p, &p->env, &v->items[2], &n) < 0)
            return -1;
        *out = yqst_linspace(lo, hi, (int)n);
        return 0;
    }
    if (v->kind == YEXP_V_ARRAY) {
        pl_fail(p, "method '%s': an axis given as a list of points is not lowered (the format has no list form)", d->name);
        return -1;
    }
    {
        double x;
        if (pl_method_num(p, d, v, &x) < 0) return -1;
        *out = yqst_fixed(x);
    }
    return 0;
}

static int pl_method_open(pl_player* p, pl_method* m) {
    const yexp_method* d = m->def;
    pl_env* e = &p->env;
    memset(e, 0, sizeof *e);
    e->phase = PL_PH_SESSION;
    switch (d->type) {
    case YEXP_STAIR: {
        yst_desc sd;
        const yexp_prop* st;
        int step_type, rule;
        memset(&sd, 0, sizeof sd);
        m->stair = (yst_stair*)pl_alloc(p, sizeof(yst_stair));
        if (!m->stair) return -1;
        if (pl_get_num(p, d, "start", &sd.start) < 0 || pl_get_int(p, d, "n_up", &sd.n_up) < 0 || pl_get_int(p, d, "n_down", &sd.n_down) < 0 ||
            pl_get_num(p, d, "step_down_scale", &sd.step_down_scale) < 0 || pl_get_num(p, d, "target_p", &sd.target_p) < 0 ||
            pl_get_num(p, d, "min", &sd.min) < 0 || pl_get_num(p, d, "max", &sd.max) < 0 || pl_get_bool(p, d, "use_limits", &sd.use_limits) < 0 ||
            pl_get_bool(p, d, "harder_is_up", &sd.harder_is_up) < 0 || pl_get_bool(p, d, "initial_rule", &sd.initial_rule) < 0 ||
            pl_get_int(p, d, "stop_reversals", &sd.stop_reversals) < 0 || pl_get_int(p, d, "stop_trials", &sd.stop_trials) < 0 ||
            pl_get_int(p, d, "stop_at_limit", &sd.stop_at_limit) < 0 || pl_get_int(p, d, "est_reversals", &sd.est_reversals) < 0 ||
            pl_get_int(p, d, "est_trials", &sd.est_trials) < 0)
            return -1;
        if (pl_get_enum(p, d, "step_type", "lin=0,log=1,db=2", &step_type) < 0 || pl_get_enum(p, d, "rule", "updown=0,asa=1", &rule) < 0) return -1;
        sd.step_type = (yst_step_type)step_type;
        sd.rule = (yst_rule)rule;
        st = yexp_prop_get(d->props, d->n_props, "steps");
        if (st) {
            pl_val a;
            uint32_t i;
            if (pl_eval(p, e, st->value, &a) < 0) return -1;
            if (a.kind != YEXP_V_ARRAY || a.n == 0 || a.n > YST_MAX_STEPS) { pl_fail(p, "method '%s': 'steps' has %u entries", d->name, a.n); return -1; }
            for (i = 0; i < a.n; i++)
                if (pl_eval_num(p, e, &a.items[i], &sd.steps[i]) < 0) return -1;
            sd.n_steps = (int)a.n;
        }
        if (!yst_open(m->stair, &sd)) { pl_fail(p, "method '%s': %s", d->name, yst_error(m->stair)); return -1; }
        m->next_level = pl_stair_next;
        m->update = pl_stair_update;
        m->estimate = pl_stair_est;
        m->done = pl_stair_done;
        /* the C program floors the value it hears before a log step; the
         * definition has no key for it (reported as a gap). */
        m->log_domain = d->log10 || step_type == 1 || step_type == 2;
        return 0;
    }
    case YEXP_QUEST: {
        yqst_desc qd;
        const yexp_prop* stim, *par;
        int pf, sel, tb, no_table_i = 0;
        bool no_table = false;
        uint32_t i;
        memset(&qd, 0, sizeof qd);
        m->quest = (yqst_quest*)pl_alloc(p, sizeof(yqst_quest));
        if (!m->quest) return -1;
        if (pl_get_int(p, d, "n_outcomes", &qd.n_outcomes) < 0 || pl_get_int(p, d, "select_param", &qd.select_param) < 0 ||
            pl_get_num(p, d, "select_quantile", &qd.select_quantile) < 0 || pl_get_int(p, d, "subset_size", &qd.subset_size) < 0 ||
            pl_get_num(p, d, "tie_tolerance", &qd.tie_tolerance) < 0 || pl_get_int(p, d, "stop_trials", &qd.stop_trials) < 0 ||
            pl_get_num(p, d, "stop_entropy", &qd.stop_entropy) < 0 || pl_get_num(p, d, "stop_sd", &qd.stop_sd) < 0 ||
            pl_get_int(p, d, "stop_sd_param", &qd.stop_sd_param) < 0 || pl_get_bool(p, d, "no_table", &no_table) < 0)
            return -1;
        (void)no_table_i;
        qd.no_table = no_table;
        if (pl_get_enum(p, d, "pf", "gumbel=0,weibull=1,logistic=2,normal=3,hypsec=4,custom=5", &pf) < 0 ||
            pl_get_enum(p, d, "select", "entropy=0,quantile=1,mean=2,mode=3", &sel) < 0 ||
            pl_get_enum(p, d, "tiebreak", "lowest=0,nearest=1,alternate=2,random=3", &tb) < 0)
            return -1;
        qd.pf = (yqst_pf)pf;
        qd.select = (yqst_select)sel;
        qd.tiebreak = (yqst_tiebreak)tb;
        if (qd.pf == YQST_PF_CUSTOM) { pl_fail(p, "method '%s': a custom pf needs a script (not in version 1)", d->name); return -1; }
        if (qd.subset_size) { pl_fail(p, "method '%s': 'subset_size' needs a generator the format does not name", d->name); return -1; }
        stim = yexp_prop_get(d->props, d->n_props, "stim");
        par = yexp_prop_get(d->props, d->n_props, "param");
        if (!stim || !par || stim->value->kind != YEXP_V_ARRAY || par->value->kind != YEXP_V_ARRAY) {
            pl_fail(p, "method '%s': 'stim' and 'param' are arrays of axes", d->name);
            return -1;
        }
        if (stim->value->n > YQST_MAX_STIM_DIMS || par->value->n > YQST_MAX_PARAMS) { pl_fail(p, "method '%s': too many axes", d->name); return -1; }
        /* $level and method_value carry one number, and the update reads one
         * per stimulus axis (found by the fuzz target: a 3-axis quest read
         * past the value) */
        if (stim->value->n != 1) {
            pl_fail(p, "method '%s': 'stim' has %u axes; $level and method_value carry one value, so this version lowers a quest with one stimulus axis", d->name, (unsigned)stim->value->n);
            return -1;
        }
        for (i = 0; i < stim->value->n; i++)
            if (pl_get_axis(p, d, &stim->value->items[i], &qd.stim[i]) < 0) return -1;
        qd.n_stim = (int)stim->value->n;
        for (i = 0; i < par->value->n; i++)
            if (pl_get_axis(p, d, &par->value->items[i], &qd.param[i]) < 0) return -1;
        qd.n_param = (int)par->value->n;
        if (!yqst_open(m->quest, &qd)) { pl_fail(p, "method '%s': %s", d->name, yqst_error(m->quest)); return -1; }
        m->next_level = pl_quest_next;
        m->update = pl_quest_update;
        m->estimate = pl_quest_est;
        m->done = pl_quest_done;
        m->log_domain = d->log10;
        return 0;
    }
    case YEXP_AEP: {
        /* AEP has 80 fields and no worked example uses it. Lowering it key by
         * key without a test against a C program would be a guess, so it is
         * refused by name (a finding in the report). */
        pl_fail(p, "method '%s': aep is not lowered by this player (no worked example; run it through ysp/aep.h from C for now)", d->name);
        return -1;
    }
    default:
        pl_fail(p, "method '%s': unknown type", d->name);
        return -1;
    }
}

/* --- templates and responses --------------------------------------------- */

/* The stimuli a template touches: only their keys are evaluated per trial,
 * because a key can name a variable only the leaves running this template
 * have (the loader checks it per reaching template). */
static void pl_mark_stims(pl_player* p, pl_tmpl* t, const yexp_step* st, int n) {
    int i, k;
    for (i = 0; i < n; i++) {
        const yexp_step* s = &st[i];
        for (k = 0; k < s->n_stims; k++) {
            const pl_stim* ps = pl_stim_by_name(p, s->stims[k]);
            if (ps) t->stim_used[ps - p->stims] = true;
        }
        if (s->op == YEXP_IF || s->op == YEXP_UNLESS) pl_mark_stims(p, t, s->steps, s->n_steps);
    }
}

static int pl_response_kind(const pl_player* p, const pl_role* r) {
    /* A real run: the keyboard, or the device the rig binds. --sim has no
     * rig devices: a role whose first family is keyboard is a keyboard
     * role, anything else a box role, which the participant presses. */
    if (!p->args.sim) return r->keyboard ? 0 : 1;
    return r->def->n_family > 0 && strcmp(r->def->family[0], "keyboard") == 0 ? 0 : 1;
}

static int pl_tmpl_open(pl_player* p, pl_tmpl* t) {
    const yexp_template* d = t->def;
    int bound = 8 + pl_ops_bound(d->events, d->n_events) + pl_ops_bound(d->after, d->n_after);
    t->ops = (ytl_op*)pl_alloc(p, sizeof(ytl_op) * (size_t)bound);
    if (!t->ops) return -1;
    t->ops_cap = bound;
    t->stim_used = (bool*)pl_alloc(p, sizeof(bool) * (size_t)(p->n_stims + 1));
    if (!t->stim_used) return -1;
    pl_mark_stims(p, t, d->events, d->n_events);
    pl_mark_stims(p, t, d->after, d->n_after);
    if (d->response) {
        const pl_stim* ps = pl_stim_by_name(p, d->response->from);
        if (ps) t->stim_used[ps - p->stims] = true;
    }
    t->n_record = d->n_record;
    t->record_stims = (int*)pl_alloc(p, sizeof(int) * (size_t)(d->n_record + 1));
    if (!t->record_stims) return -1;
    {
        int i;
        for (i = 0; i < d->n_record; i++) {
            int k;
            t->record_stims[i] = -1;
            for (k = 0; k < p->n_stims; k++)
                if (strcmp(p->stims[k].def->name, d->record[i]) == 0) t->record_stims[i] = k;
        }
    }
    if (!d->response) return 0;
    {
        const yexp_response* r = d->response;
        const yexp_role* role = NULL;
        yrsp_desc rd;
        yrsp_source src;
        int i, kind;
        for (i = 0; i < p->def->n_roles; i++)
            if (strcmp(p->def->roles[i].name, r->role) == 0) { role = &p->def->roles[i]; t->role = i; }
        if (!role) { pl_fail(p, "template '%s': the response role '%s' does not exist", d->name, r->role); return -1; }
        kind = pl_response_kind(p, &p->roles[t->role]);
        t->n_choices = r->n_choices;
        t->choice_keys = (const char**)pl_alloc(p, sizeof(char*) * (size_t)(r->n_choices + 1));
        t->choice_codes = (uint32_t*)pl_alloc(p, sizeof(uint32_t) * (size_t)(r->n_choices + 1));
        if (!p->collectors) {
            p->collectors = (yrsp_collector*)pl_alloc(p, sizeof(yrsp_collector) * (size_t)(p->def->n_templates + 1));
            if (!p->collectors) return -1;
        }
        t->collector = &p->collectors[p->n_collectors++];
        if (!t->choice_keys || !t->choice_codes) return -1;
        {
            yrsp_choice* ch = (yrsp_choice*)pl_alloc(p, sizeof(yrsp_choice) * (size_t)(r->n_choices + 1));
            if (!ch) return -1;
            if (r->n_choices > YRSP_MAX_CHOICES) { pl_fail(p, "template '%s': %d choices is more than ysp/response.h's %d", d->name, r->n_choices, YRSP_MAX_CHOICES); return -1; }
            for (i = 0; i < r->n_choices; i++) {
                ch[i].name = r->choices[i].name;
                t->choice_keys[i] = r->choices[i].key;
                if (kind == 0) {
                    ch[i].key = r->choices[i].key;
                } else {
                    /* a button role: the role's buttons give the control */
                    int k, found = -1;
                    for (k = 0; k < role->n_buttons; k++)
                        if (strcmp(role->buttons[k].value->str, r->choices[i].key) == 0) found = k;
                    if (found < 0) { pl_fail(p, "template '%s': choice '%s' is not one of role '%s''s buttons", d->name, r->choices[i].key, role->name); return -1; }
                    ch[i].kind = YRSP_KIND_BOX;
                    ch[i].control = (uint32_t)strtoul(role->buttons[found].key, NULL, 10);
                }
            }
            memset(&rd, 0, sizeof rd);
            rd.choices = ch;
            rd.n_choices = r->n_choices;
            if (!r->n_choices) { rd.mode = YRSP_CHOICES_ALL; rd.kinds = YRSP_KINDS_ALL; }
            rd.persist = !r->ends_trial;
            rd.max_responses = r->max_responses;
            rd.allow_held_key = r->allow_held_key;
            rd.wait_for_key_release = r->wait_for_key_release;
            if (!p->args.sim) {
                /* the source's tier and bounds go into each result: the
                 * keyboard's, or the rig's for its device */
                src = p->roles[t->role].source;
                rd.sources = &src;   /* copied at open */
                rd.n_sources = 1;
                /* an any-key response hears its role's kind only: a click
                 * that brings the window to the front is no response */
                if (!r->n_choices) rd.kinds = kind == 0 ? 0 : YRSP_KIND_BIT(YRSP_KIND_BOX);
            }
            if (!yrsp_open(t->collector, &rd)) { pl_fail(p, "template '%s': %s", d->name, yrsp_error(t->collector)); return -1; }
        }
    }
    return 0;
}

/* --- leaves ------------------------------------------------------------- */

static int pl_leaf_prepare(pl_player* p, const yexp_node* n, pl_leaf* lf) {
    ytr_rules_desc rr;
    char err[512];
    int i;
    const ytb_table* table = n->table ? pl_table_get(p, n->table) : NULL;
    if (n->table && !table) { pl_fail(p, "node '%s': no table '%s'", n->path, n->table); return -1; }
    memset(lf, 0, sizeof *lf);
    lf->node = n;
    lf->table = table;
    for (i = 0; i < p->def->n_templates; i++)
        if (n->trial == &p->def->templates[i]) lf->tmpl = &p->tmpls[i];
    lf->arena_size = ((size_t)1 << 19) + (table ? 32u * (size_t)table->n_rows : 0u);
    lf->arena = pl_alloc(p, lf->arena_size);
    if (!lf->arena) return -1;
    if (n->rules[0]) {
        memset(&rr, 0, sizeof rr);
        rr.text = n->rules;
        rr.len = strlen(n->rules);
        rr.table = table;
        rr.arena = lf->arena;
        rr.arena_size = lf->arena_size;
        rr.participant = (int)p->participant;
        if (ytr_rules(&lf->desc, &rr, err, sizeof err) != 0) { pl_fail(p, "node '%s': %s", n->path, err); return -1; }
    }
    if (table) lf->desc.table = table;
    else if (n->n_tracks == 0 && !lf->desc.n_conditions && !lf->desc.n_factors) {
        /* 2.4: a leaf with neither a table nor tracks has one condition */
        lf->desc.n_conditions = 1;
    }
    if (lf->desc.reps == 0 && !lf->desc.cond_reps && (table || lf->desc.n_conditions) && lf->desc.order != YTR_ORDER_WITH_REPLACEMENT &&
        !lf->desc.n_order_list)
        lf->desc.reps = 1;
    for (i = 0; i < n->n_tracks; i++) {
        int m;
        pl_method* pm = NULL;
        for (m = 0; m < p->n_methods; m++)
            if (p->methods[m].def == n->tracks[i]) pm = &p->methods[m];
        if (!pm) { pl_fail(p, "node '%s': no method '%s'", n->path, n->tracks[i]->name); return -1; }
        lf->tracks[i] = pm;
        lf->desc.tracks[i] = ytr_track(pm, pl_track_done);
    }
    lf->desc.n_tracks = n->n_tracks;
    lf->n_tracks = n->n_tracks;
    lf->desc.interleave = n->interleave ? YTR_INTERLEAVE_ROUND_ROBIN : YTR_INTERLEAVE_RANDOM;
    lf->desc.track_rate = n->track_rate;
    lf->desc.display_rate_num = 60;
    lf->desc.display_rate_den = 1;
    if (p->scr_open) {
        /* the screen's real rate, for rate=display */
        int64_t per = p->caps.period_ns > 0 ? p->caps.period_ns : 16666667;
        lf->desc.display_rate_num = (int)llround(1e9 / (double)per);
    }
    /* Choice variables of this node and its ancestors become desc.vars (2.4).
     * The rules text's own `var` statements are already there. */
    {
        const yexp_node* s;
        int nv = lf->desc.n_vars;
        for (s = n; s; s = s->parent) {
            int k;
            for (k = 0; k < s->n_vars; k++) {
                const yexp_value* v = s->vars[k].value;
                int dup = 0, j;
                if (v->kind != YEXP_V_CHOICE) continue;
                for (j = 0; j < nv; j++)
                    if (strcmp(lf->desc.vars[j].name, s->vars[k].key) == 0) dup = 1;
                if (dup) continue;   /* nearer one shadows (the loader's order) */
                if (nv >= YTR_MAX_VARS) {
                    pl_fail(p, "node '%s': more than %d choice variables", n->path, YTR_MAX_VARS);
                    return -1;
                }
                {
                    const char** vals = (const char**)pl_alloc(p, sizeof(char*) * (v->n + 1));
                    uint32_t u;
                    if (!vals) return -1;
                    for (u = 0; u < v->n; u++) {
                        char buf[YTR_MAX_VAR_TEXT + 1];
                        if (pl_eval_text(p, &p->env, &v->items[u], buf, sizeof buf) < 0) return -1;
                        vals[u] = pl_strdup(p, buf);
                    }
                    lf->desc.vars[nv].name = s->vars[k].key;
                    lf->desc.vars[nv].values = vals;
                    lf->desc.vars[nv].n_values = (int)v->n;
                    nv++;
                }
            }
        }
        lf->desc.n_vars = nv;
    }
    return 0;
}

static int pl_count_leaves(const yexp_node* n, int count) {
    int i, c = 0;
    for (i = 0; i < count; i++) {
        if (n[i].trial) c++;
        else c += pl_count_leaves(n[i].children, n[i].n_children);
    }
    return c;
}

static int pl_leaves_collect(pl_player* p, const yexp_node* n, int count, int* idx) {
    int i;
    for (i = 0; i < count; i++) {
        if (n[i].trial) {
            if (pl_leaf_prepare(p, &n[i], &p->leaves[(*idx)++]) < 0) return -1;
        } else if (pl_leaves_collect(p, n[i].children, n[i].n_children, idx) < 0) return -1;
    }
    return 0;
}

static void pl_list_leaves(const yexp_node* n, int count, const yexp_node** out, int* idx) {
    int i;
    for (i = 0; i < count; i++) {
        if (n[i].trial) out[(*idx)++] = &n[i];
        else pl_list_leaves(n[i].children, n[i].n_children, out, idx);
    }
}

pl_leaf* pl_leaf_of(pl_player* p, const yexp_node* n) {
    int i;
    for (i = 0; i < p->n_leaves; i++)
        if (p->leaves[i].node == n) return &p->leaves[i];
    return NULL;
}

/* An integer session field in an `if` or `unless` must be 0 or 1 (the
 * loader accepts it; experiment.md's table: checked at session start). */
static int pl_check_cond(pl_player* p, const char* name, const char* where) {
    int i;
    for (i = 0; i < p->def->n_fields; i++)
        if (strcmp(p->def->fields[i].name, name) == 0 && !p->def->fields[i].is_string) {
            int64_t v = p->field_values[i].i;
            if (v != 0 && v != 1) {
                pl_fail(p, "%s: the session field '%s' is %lld, and an 'if' or 'unless' needs 0 or 1", where, name, (long long)v);
                return -1;
            }
        }
    return 0;
}

static int pl_check_steps(pl_player* p, const yexp_step* s, int n, const char* where) {
    int i;
    for (i = 0; i < n; i++) {
        if (s[i].op == YEXP_IF || s[i].op == YEXP_UNLESS) {
            if (pl_check_cond(p, s[i].cond, where) < 0) return -1;
            if (pl_check_steps(p, s[i].steps, s[i].n_steps, where) < 0) return -1;
        }
    }
    return 0;
}

static int pl_check_nodes(pl_player* p, const yexp_node* n, int count) {
    int i;
    for (i = 0; i < count; i++) {
        int a, b;
        /* one node, one name, two values: the row would have to pick one */
        for (a = 0; a < n[i].n_vars; a++)
            for (b = 0; b < n[i].n_data; b++)
                if (strcmp(n[i].vars[a].key, n[i].data[b].key) == 0) {
                    pl_fail(p, "node '%s': '%s' is both a 'vars' name and a 'data' column", n[i].path, n[i].vars[a].key);
                    return -1;
                }
        if (n[i].if_var && pl_check_cond(p, n[i].if_var, n[i].path) < 0) return -1;
        if (n[i].unless_var && pl_check_cond(p, n[i].unless_var, n[i].path) < 0) return -1;
        if (n[i].n_children && pl_check_nodes(p, n[i].children, n[i].n_children) < 0) return -1;
    }
    return 0;
}

/* --- bounds sized at load ----------------------------------------------- */

/* Container starts the walk makes, from repetitions, table rows and loop.max:
 * each iteration of a container (a loop pass, a repetition, a row) is one
 * start, with or without a leaf under it. */
static double pl_container_visits(pl_player* p, const yexp_node* n, int count) {
    double sum = 0;
    int i;
    for (i = 0; i < count; i++) {
        double loops, rows, starts;
        const ytb_table* t;
        if (n[i].trial) continue;
        loops = n[i].has_loop ? (double)(n[i].loop_max > 0 ? n[i].loop_max : 1) : 1.0;
        t = n[i].table ? pl_table_get(p, n[i].table) : NULL;
        rows = n[i].table ? (t ? (double)t->n_rows : 0) : 1.0;
        starts = loops * (double)(n[i].repetitions > 0 ? n[i].repetitions : 1) * rows;
        sum += starts + starts * pl_container_visits(p, n[i].children, n[i].n_children);
    }
    return sum;
}

/* Whether any leaf can run: a container over a table with no rows, or with
 * no children, runs nothing (`if` is read at run time and does not count). */
static bool pl_runnable(pl_player* p, const yexp_node* n, int count) {
    int i;
    for (i = 0; i < count; i++) {
        if (n[i].trial) return true;
        if (n[i].table) {
            const ytb_table* t = pl_table_get(p, n[i].table);
            if (!t || t->n_rows == 0) continue;
        }
        if (pl_runnable(p, n[i].children, n[i].n_children)) return true;
    }
    return false;
}

/* The longest text any value can make, from what the definition and the
 * pack hold: literals, table cells, session fields. A value only copies
 * one of these, or is a number, a name, a key name or a seed (64 bytes). */
static void pl_text_value(pl_player* p, const yexp_value* v, size_t* m) {
    uint32_t i;
    if (!v) return;
    switch (v->kind) {
    case YEXP_V_STRING: if (strlen(v->str) > *m) *m = strlen(v->str); return;
    case YEXP_V_ARRAY: if ((size_t)v->n * 26 + 2 > *m) *m = (size_t)v->n * 26 + 2; return;
    case YEXP_V_LOOKUP:
        pl_text_value(p, v->by, m);
        for (i = 0; i < v->n; i++) pl_text_value(p, v->map[i].value, m);
        return;
    case YEXP_V_CHOICE:
        for (i = 0; i < v->n; i++) pl_text_value(p, &v->items[i], m);
        return;
    case YEXP_V_MICHELSON: pl_text_value(p, v->by, m); return;
    default: return;
    }
}

static void pl_text_steps(pl_player* p, const yexp_step* s, int n, size_t* m) {
    int i, k;
    for (i = 0; i < n; i++) {
        pl_text_value(p, s[i].t, m);
        pl_text_value(p, s[i].code, m);
        for (k = 0; k < s[i].n_props; k++) pl_text_value(p, s[i].props[k].value, m);
        pl_text_steps(p, s[i].steps, s[i].n_steps, m);
    }
}

static void pl_text_nodes(pl_player* p, const yexp_node* n, int count, size_t* m) {
    int i, k;
    for (i = 0; i < count; i++) {
        for (k = 0; k < n[i].n_vars; k++) pl_text_value(p, n[i].vars[k].value, m);
        for (k = 0; k < n[i].n_data; k++) pl_text_value(p, n[i].data[k].value, m);
        for (k = 0; k < n[i].n_rule_vars; k++) {
            int j;
            for (j = 0; n[i].rule_vars[k].values && j < n[i].rule_vars[k].n_values; j++)
                if (strlen(n[i].rule_vars[k].values[j]) > *m) *m = strlen(n[i].rule_vars[k].values[j]);
        }
        pl_text_nodes(p, n[i].children, n[i].n_children, m);
    }
}

static int pl_text_bound(pl_player* p) {
    size_t m = 64;
    int i, k;
    for (i = 0; i < p->def->n_stimuli; i++)
        for (k = 0; k < p->def->stimuli[i].n_props; k++) pl_text_value(p, p->def->stimuli[i].props[k].value, &m);
    for (i = 0; i < p->def->n_templates; i++) {
        const yexp_template* t = &p->def->templates[i];
        pl_text_steps(p, t->events, t->n_events, &m);
        pl_text_steps(p, t->after, t->n_after, &m);
        for (k = 0; k < t->n_columns; k++) pl_text_value(p, t->columns[k].value, &m);
        pl_text_value(p, t->outcome, &m);
        if (t->response) {
            pl_text_value(p, t->response->key_answer, &m);
            pl_text_value(p, t->response->trigger_code, &m);
            for (k = 0; k < t->response->n_choices; k++) {
                if (strlen(t->response->choices[k].key) > m) m = strlen(t->response->choices[k].key);
                if (t->response->choices[k].name && strlen(t->response->choices[k].name) > m) m = strlen(t->response->choices[k].name);
            }
        }
    }
    pl_text_nodes(p, p->def->timeline, p->def->n_timeline, &m);
    for (i = 0; i < p->n_tables; i++) {
        const ytb_table* t = p->tables[i].table;
        int c;
        for (c = 0; c < t->n_cols; c++) {
            int l;
            for (l = 0; l < ytb_n_levels(t, c); l++) {
                const char* x = ytb_level_text(t, c, l);
                if (x && strlen(x) > m) m = strlen(x);
            }
        }
    }
    for (i = 0; i < p->def->n_fields; i++)
        if (p->field_values[i].str && strlen(p->field_values[i].str) > m) m = strlen(p->field_values[i].str);
    if (m > PL_MAX_TEXT) {
        pl_fail(p, "a value can be %zu bytes long (a literal, a table cell or a session field); the data set takes at most %d", m, PL_MAX_TEXT);
        return -1;
    }
    p->max_text = m;
    p->text_buf = (char*)pl_alloc(p, m + 1);
    p->key_answer = (char*)pl_alloc(p, m + 1);
    p->runs_key = (char*)pl_alloc(p, 2 * m + 1024);
    return p->text_buf && p->key_answer && p->runs_key ? 0 : -1;
}

/* An RDK records one step a frame into a fixed buffer; a full one goes to
 * the stream mid-trial (pl_rdk_flush(), run.c), so its size bounds memory,
 * not a trial's length. --rdk-buffer (tests) makes it small. */
static int pl_rdk_steps(pl_player* p) {
    int i;
    int n = p->args.rdk_buffer > 0 ? p->args.rdk_buffer : PL_RDK_STEPS;
    for (i = 0; i < p->n_stims; i++) {
        pl_stim* s = &p->stims[i];
        if (s->def->kind != YEXP_RDK) continue;
        s->max_steps = n;
        s->steps = (yrdk_step*)pl_alloc(p, sizeof(yrdk_step) * (size_t)n);
        if (!s->steps) return -1;
    }
    return 0;
}

/* --- load --------------------------------------------------------------- */

static void pl_hex(const uint8_t* id, char out[65]) {
    static const char H[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) { out[i * 2] = H[id[i] >> 4]; out[i * 2 + 1] = H[id[i] & 15]; }
    out[64] = 0;
}

/* --rig FILE (a path) or --rig NAME (a profile in the user's rig folder,
 * ysp/rigfile.h WHERE IT LIVES). A real run with no --rig takes the
 * folder's profile.json when there is one: the rig's own file binds its
 * roles, so an operator need not name it. */
static int pl_rig_load(pl_player* p) {
    char err[400];
    const char* a = p->args.rig_path;
    int rc;
    p->rig = (yrig_profile*)pl_alloc(p, sizeof(yrig_profile));
    if (!p->rig) return -1;
    if (a) {
        bool file = strchr(a, '/') || strchr(a, '\\') || (strlen(a) > 5 && strcmp(a + strlen(a) - 5, ".json") == 0);
        rc = file ? yrig_load_file(p->rig, a, err, sizeof err) : yrig_load(p->rig, a, err, sizeof err);
        if (rc != YRIG_OK) { pl_fail(p, "the rig profile '%s': %s", a, err); return -1; }
    } else if (!p->args.sim) {
        rc = yrig_load(p->rig, NULL, err, sizeof err);
        if (rc == YRIG_ERR_MISSING || rc == YRIG_ERR_FOLDER) return 0;   /* no profile: keyboard roles only */
        if (rc != YRIG_OK) { pl_fail(p, "the rig profile: %s", err); return -1; }
    } else {
        return 0;
    }
    p->have_rig = true;
    if (yrig_hash(p->rig, p->rig_hash) != YRIG_OK) p->rig_hash[0] = 0;
    return 0;
}

static int pl_cal_load(pl_player* p) {
    const char* path = p->args.cal_path;
    char joined[PL_MAX_PATH], err[300];
    void* bytes;
    FILE* fp;
    long n;
    ycol_ctx_desc cxd;
    static const ycol_rgb gray = { 0.0, 0.0, 0.0 };
    if (!path && p->have_rig && p->rig->display.calibration[0]) {
        snprintf(joined, sizeof joined, "%s/%s", p->rig->dir, p->rig->display.calibration);
        path = joined;
    }
    if (!path) {
        if (p->def->calibration_required) {
            pl_fail(p, "display.calibration is required: give --rig with a calibration, or --cal FILE");
            return -1;
        }
        return 0;
    }
    bytes = pl_alloc(p, sizeof(ycol_cal));
    if (!bytes) return -1;
    if (!(fp = fopen(path, "rb"))) { pl_fail(p, "cannot read the calibration '%s'", path); return -1; }
    n = (long)fread(bytes, 1, sizeof(ycol_cal), fp);
    fclose(fp);
    if (ycol_cal_load(&p->cal, bytes, (size_t)n, err, sizeof err) < 0) { pl_fail(p, "the calibration '%s': %s", path, err); return -1; }
    p->have_cal = true;
    memset(&cxd, 0, sizeof cxd);
    cxd.cal = &p->cal;
    cxd.background[0] = p->def->background[0];
    cxd.background[1] = p->def->background[1];
    cxd.background[2] = p->def->background[2];
    if (!ycol_ctx_open(&p->cx, &cxd)) { pl_fail(p, "calibration: %s", ycol_ctx_error(&p->cx)); return -1; }
    if (ycol_michelson(&p->cx, 0.0, gray, 8, &p->gray_sh) < 0) { pl_fail(p, "calibration: %s", p->gray_sh.why); return -1; }
    return 0;
}

/* --- roles and devices (2.9) --------------------------------------------- */

pl_role* pl_role_get(pl_player* p, const char* name) {
    int i;
    for (i = 0; i < p->def->n_roles; i++)
        if (strcmp(p->def->roles[i].name, name) == 0) return &p->roles[i];
    return NULL;
}

static bool pl_role_lists(const yexp_role* r, const char* family) {
    int i;
    for (i = 0; i < r->n_family; i++)
        if (strcmp(r->family[i], family) == 0) return true;
    return false;
}

/* A box's events go to the screen's input bridge, where yrsp_feed_screen()
 * reads them with the keys: one loop for every input (ysp/screen.h). */
static void pl_dev_sink(void* ctx, const yin_event* e) {
    (void)ctx;
    yscr_push_input(e);
}

/* What each role is bound to. A real run refuses a required role that has
 * neither a device in the rig profile nor the keyboard (an input role that
 * lists the family "keyboard"), naming it: the session would wait for a
 * press or write codes nowhere. */
static int pl_roles_bind(pl_player* p) {
    int i;
    p->roles = (pl_role*)pl_alloc(p, sizeof(pl_role) * (size_t)(p->def->n_roles + 1));
    if (!p->roles) return -1;
    for (i = 0; i < p->def->n_roles; i++) {
        pl_role* r = &p->roles[i];
        const yrig_role* rr = p->have_rig ? yrig_find(p->rig, p->def->roles[i].name) : NULL;
        r->def = &p->def->roles[i];
        r->rig = rr && rr->family[0] ? rr : NULL;
        r->trigger = pl_trigger_channel(p, r->def->name);
        r->rule_out = -1;
        r->rule_trial = -1;
        if (p->args.sim) continue;
        r->keyboard = !r->rig && r->def->direction == 0 && pl_role_lists(r->def, "keyboard");
        if (r->def->required && !r->rig && !r->keyboard) {
            pl_fail(p, "the role '%s' is required, and %s binds no device to it (its families: %s%s%s); bind one in the rig profile (ysp/rigfile.h)",
                    r->def->name, p->have_rig ? "the rig profile" : "there is no rig profile (--rig), so nothing",
                    r->def->n_family > 0 ? r->def->family[0] : "", r->def->n_family > 1 ? ", ..." : "", "");
            return -1;
        }
    }
    return 0;
}

/* A bound role's device, through the rig profile (yrig_open_device()), and
 * running before the first frame: an output that is not RUNNING refuses
 * its first code (ysp/device.h, LIFECYCLE). */
static int pl_role_open(pl_player* p, pl_role* r) {
    ydev_desc base;
    if (!r->rig || r->dev) return 0;
    r->dev = (ydev_device*)pl_alloc(p, sizeof(ydev_device));
    if (!r->dev) return -1;
    memset(&base, 0, sizeof base);
    base.sink = pl_dev_sink;
    base.ring = &p->ring;
    base.roles = &p->dev_roles;
    base.pulse_ns = r->def->pulse_ns;   /* the rig's pulse_s, when it fixes one, wins (yrig_desc()) */
    base.rule = r->rule;
    if (!yrig_open_device(r->dev, p->rig, r->def->name, &base)) {
        pl_fail(p, "the role '%s' (%s at %s): %s", r->def->name, r->rig->family, r->rig->key, ydev_error(r->dev));
        ydev_close(r->dev);
        r->dev = NULL;
        return -1;
    }
    if (ydev_wait_running(r->dev, YRT_S(3)) < 0) {
        pl_fail(p, "the role '%s' (%s at %s) did not start: %s", r->def->name, r->rig->family, r->rig->key, ydev_error(r->dev));
        return -1;
    }
    r->source = yrig_source(p->rig, r->dev);
    return 0;
}

/* The output roles, before the screen: each is a trigger channel. */
static int pl_outputs_open(pl_player* p) {
    int i;
    yrt_ring_desc rd;
    if (p->args.sim) return 0;
    p->ring_mem = (unsigned char*)pl_alloc(p, YRT_RING_BYTES(PL_DEV_RING));
    p->dev_recs = (yrt_event*)pl_alloc(p, sizeof(yrt_event) * PL_DEV_DRAIN);
    if (!p->ring_mem || !p->dev_recs) return -1;
    memset(&rd, 0, sizeof rd);
    rd.memory = p->ring_mem;
    rd.bytes = YRT_RING_BYTES(PL_DEV_RING);
    if (!yrt_ring_open(&p->ring, &rd)) { pl_fail(p, "the device ring: %s", yrt_ring_error(&p->ring)); return -1; }
    p->ring_open = true;
    for (i = 0; i < p->def->n_roles; i++)
        if (p->roles[i].def->direction != 0 && pl_role_open(p, &p->roles[i]) < 0) return -1;
    return 0;
}

/* A response's trigger on a device role is a rule on the role's reader
 * thread (2.6, G8): the code goes out where the press is read. The rule's
 * choices and codes are fixed when the device opens, so the code may
 * depend on $response only, and every template that responds on the role
 * must give the same codes. */
static int pl_rule_make(pl_player* p, pl_role* in) {
    ydev_rule_choice ch[YDEV_RULE_CHOICES];
    ydev_rule_desc rd;
    const yexp_template* first = NULL;
    int n = 0, i, k, out = -1, max_fires = 1;
    for (i = 0; i < p->def->n_templates; i++) {
        const yexp_template* t = &p->def->templates[i];
        const yexp_response* r = t->response;
        pl_role* o;
        if (!r || !r->trigger_role || strcmp(r->role, in->def->name) != 0) continue;
        o = pl_role_get(p, r->trigger_role);
        if (!o || o->trigger < 0) { pl_fail(p, "template '%s': response.trigger: '%s' is not an output role", t->name, r->trigger_role); return -1; }
        if (r->n_choices < 1 || r->n_choices > YDEV_RULE_CHOICES) {
            pl_fail(p, "template '%s': a response trigger on the device role '%s' needs 1 to %d named choices (a rule fires per choice)", t->name, in->def->name,
                    YDEV_RULE_CHOICES);
            return -1;
        }
        for (k = 0; k < r->n_choices; k++) {
            pl_env env;
            int64_t code = 0;
            int b, found = -1;
            ydev_rule_choice c;
            memset(&env, 0, sizeof env);
            env.t = t;
            env.phase = PL_PH_AFTER;
            env.responded = true;
            env.response = r->choices[k].key;
            env.rt_ns = YRSP_NO_TIME;
            if (pl_eval_int(p, &env, r->trigger_code, &code) < 0) {
                p->failed = 0;
                pl_fail(p, "template '%s': the response trigger's code on the device role '%s' must depend on $response only: a rule's codes are set when the device opens",
                        t->name, in->def->name);
                return -1;
            }
            if (code < 0 || code > 255) { pl_fail(p, "template '%s': the response trigger code %lld is not in 0..255", t->name, (long long)code); return -1; }
            for (b = 0; b < in->def->n_buttons; b++)
                if (strcmp(in->def->buttons[b].value->str, r->choices[k].key) == 0) found = b;
            if (found < 0) { pl_fail(p, "template '%s': choice '%s' is not one of role '%s''s buttons", t->name, r->choices[k].key, in->def->name); return -1; }
            memset(&c, 0, sizeof c);
            c.kind = (uint8_t)YIN_KIND_BOX;
            c.control = (uint32_t)strtoul(in->def->buttons[found].key, NULL, 10);
            c.code = (uint32_t)code;
            if (!first) ch[n++] = c;
            else if (k >= n || ch[k].control != c.control || ch[k].code != c.code || out != o->trigger) {
                pl_fail(p, "templates '%s' and '%s' give the device role '%s' different response triggers; a rule is set once, when the device opens",
                        first->name, t->name, in->def->name);
                return -1;
            }
        }
        if (!first) {
            first = t;
            out = o->trigger;
            max_fires = !r->ends_trial ? (r->max_responses > 0 ? r->max_responses : -1) : 1;
        }
    }
    if (!first) return 0;
    {
        pl_role* o = NULL;
        for (i = 0; i < p->def->n_roles; i++) if (p->roles[i].trigger == out) o = &p->roles[i];
        if (!o || !o->dev) return 0;   /* an optional output with no device: no code to send */
        in->rule = (ydev_rule*)pl_alloc(p, sizeof(ydev_rule));
        if (!in->rule) return -1;
        memset(&rd, 0, sizeof rd);
        rd.out = o->dev;
        rd.choices = ch;
        rd.n_choices = n;
        rd.max_fires = max_fires;
        if (!ydev_rule_open(in->rule, &rd)) { pl_fail(p, "the response rule of role '%s': %s", in->def->name, ydev_rule_error(in->rule)); return -1; }
        in->rule_out = out;
    }
    return 0;
}

/* The input roles, after the screen (its input bridge exists from its open)
 * and before the templates (a collector takes its sources at open). */
static int pl_inputs_open(pl_player* p) {
    int i;
    if (p->args.sim) return 0;
    for (i = 0; i < p->def->n_roles; i++) {
        pl_role* r = &p->roles[i];
#if !defined(YSCR_NO_SDL)
        if (r->keyboard) {
            /* the keyboard's tier and bounds: the kernel's evdev time where
             * the screen reads it (ysp/screen.h, INPUT), else the X server's */
            yin_sdl_ctx sc;
            memset(&sc, 0, sizeof sc);
            sc.raw_keyboard = p->caps.raw_keyboard;
            sc.os_stamps = p->caps.os_input_stamps;
            r->source = yrsp_sdl_source(&sc, YRSP_KIND_KEYBOARD);
        }
#endif
        if (r->def->direction != 0 || !r->rig) continue;
        if (pl_rule_make(p, r) < 0 || pl_role_open(p, r) < 0) return -1;
    }
    return 0;
}

static void pl_devices_close(pl_player* p) {
    int i;
    if (!p->roles) return;
    /* inputs, then their rules, then the outputs (ysp/device.h, RESPONSE
     * RULES): a reader may be writing through a rule to an output */
    for (i = 0; i < p->def->n_roles; i++)
        if (p->roles[i].dev && p->roles[i].def->direction == 0) { ydev_close(p->roles[i].dev); p->roles[i].dev = NULL; }
    for (i = 0; i < p->def->n_roles; i++)
        if (p->roles[i].rule) { ydev_rule_close(p->roles[i].rule); p->roles[i].rule = NULL; }
    for (i = 0; i < p->def->n_roles; i++)
        if (p->roles[i].dev) { ydev_close(p->roles[i].dev); p->roles[i].dev = NULL; }
    if (p->ring_open) { yrt_ring_close(&p->ring); p->ring_open = false; }
}

/* --- the screen ----------------------------------------------------------- */

/* The program cache's folder, made when it is missing (ygfx_default_cache_dir()
 * makes none): each part this user's alone, as yrt_user_dir() requires. */
static const ygfx_cache* pl_program_cache(pl_player* p) {
    char dir[512];
    size_t i, n;
    if (ygfx_default_cache_dir(dir, sizeof dir) != YGFX_OK) return NULL;
    n = strlen(dir);
    for (i = 1; i <= n; i++) {
        char c = dir[i];
        if (c != '/' && c != '\\' && c != 0) continue;
        dir[i] = 0;
#if defined(_WIN32)
        (void)CreateDirectoryA(dir, NULL);
#else
        (void)mkdir(dir, 0700);
#endif
        dir[i] = c;
    }
    return ygfx_file_cache_init(&p->pcache, dir);
}

/* The panic watchdog's last words (ysp/screen.h, PANIC), on its own thread
 * while the frame thread hangs: the process ends next, session.json stays
 * "closed": false, and the rows already written are in the OS. A file of
 * its own beside them says what ended it. Plain open() and write(): the
 * hung thread may hold any lock of the C library's streams. */
static void pl_panic_fn(void* ctx) {
    pl_player* p = (pl_player*)ctx;
    char buf[256];
    int n = snprintf(buf, sizeof buf, "{\n  \"format\": \"ysp-panic\",\n  \"trial_index\": %d,\n  \"trials_ended\": %d\n}\n",
                     p->in_trial ? p->global_trial - 1 : -1, p->trials_done);
#if defined(_WIN32)
    int fd = _open(p->panic_path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) return;
    if (n > 0) (void)_write(fd, buf, (unsigned)n);
    _close(fd);
#else
    int fd = open(p->panic_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (n > 0 && write(fd, buf, (size_t)n) < 0) { /* nothing more to do: the process ends */ }
    close(fd);
#endif
}

typedef struct pl_open_ctx { pl_player* p; ygfx_desc gd; } pl_open_ctx;

/* The screen's context hook (ysp/screen.h, CONTEXT HOOK): the mode is known
 * here, so a size in deg comes from the pixels the screen got, and the gfx
 * compiles while the screen settles (as ygfx_open_screen() does). */
static void pl_open_hook(void* ctx, yscr_screen* s) {
    pl_open_ctx* c = (pl_open_ctx*)ctx;
    pl_player* p = c->p;
    c->gd.screen = s;
    if (p->def->units_deg) {
        yscr_caps caps;
        double w;
        yscr_get_caps(s, &caps);
        /* the simulated display has no mode: the scene is 800 px wide (the
         * default window); a window keeps the panel's pitch (ysp/rigfile.h,
         * VIEWING GEOMETRY), so the width in mm follows the pixels */
        w = caps.mode.w > 0 ? (double)caps.mode.w : 800.0;
        c->gd.units = YGFX_DEG;
        c->gd.view.distance_mm = (float)p->rig->view.distance_mm;
        c->gd.view.width_mm = (float)(p->rig->view.width_mm * (w / (double)p->rig->view.width_px));
        p->view = c->gd.view;
    }
    (void)ygfx_open_start(&p->gfx, &c->gd);
}

/* A real run's first frames, blank, before the data set opens: a frame
 * after a long setup is often late (ysp/gfx.h, PRIME), and the screen's
 * open judges its settle frames without counting their tier, so these
 * frames' records give display.min_tier the tier this display gives. A
 * display below it refuses the start here, before any file is written. */
static int pl_lead_in(pl_player* p) {
    yscr_frame f;
    int i, rc;
    for (i = 0; i < PL_LEAD_IN; i++) {
        rc = yscr_begin(&p->scr, &f);
        if (rc == YSCR_QUIT) { pl_fail(p, "stopped by the operator before the first trial"); return -1; }
        if (rc != YSCR_OK) { pl_fail(p, "the screen: %s", yscr_error(&p->scr)); return -1; }
        ygfx_begin(&p->gfx, &f);
        ygfx_end(&p->gfx);
        if (yscr_flip(&p->scr) != YSCR_OK) { pl_fail(p, "the flip: %s", yscr_error(&p->scr)); return -1; }
    }
    /* the last records: one or two frames are in flight */
    if (yscr_wait_flip(&p->scr, NULL) != YSCR_OK) { pl_fail(p, "the flip: %s", yscr_error(&p->scr)); return -1; }
    yscr_get_caps(&p->scr, &p->caps);
    if (p->def->min_tier && (p->caps.worst_tier == 0 || (int)p->caps.worst_tier > p->def->min_tier)) {
        char line[1024];
        yscr_describe(&p->scr, line, sizeof line);
        pl_fail(p, "display.min_tier is %d, and this display's flips are tier %d, so the session does not start (a window gives tier 2 at best; "
                   "tier 1 needs a fullscreen flip the OS observes, ysp/screen.h TIERS): %s",
                p->def->min_tier, (int)p->caps.worst_tier, line);
        return -1;
    }
    return 0;
}

/* The screen and the gfx. --sim: the simulated display. A real run: the
 * backend AUTO picks (GLX_OML on X11), fullscreen unless --windowed, the
 * panic watchdog armed, compiled programs cached, and display.min_tier
 * refusing a start on a display that cannot give it (--sim only flags its
 * frames, BELOW_TIER: the simulated display has no tier to give). */
static int pl_screen_open(pl_player* p) {
    yscr_desc sd;
    pl_open_ctx c;
    yscr_trigger_desc* trig = NULL;
    int i;
    memset(&sd, 0, sizeof sd);
    memset(&c, 0, sizeof c);
    c.p = p;
    sd.backend = p->args.sim ? YSCR_BACKEND_SIM : YSCR_BACKEND_AUTO;
    sd.windowed = p->args.sim || p->args.windowed;
    sd.min_tier = p->def->min_tier;
    if (!p->args.sim && !sd.windowed) {
        /* Shift+Esc 3 times when the loop hangs */
        sd.panic = true;
        sd.panic_fn = pl_panic_fn;
        sd.panic_ctx = p;
        snprintf(p->panic_path, sizeof p->panic_path, "%s/panic.json", p->args.out_dir);
    }
    c.gd.background[0] = (float)p->def->background[0];
    c.gd.background[1] = (float)p->def->background[1];
    c.gd.background[2] = (float)p->def->background[2];
    if (p->def->units_deg && (!p->have_rig || !p->rig->view.set)) {
        pl_fail(p, "display.units is deg: the rig profile needs a view (rigfile_profile --view); give --rig");
        return -1;
    }
    if (p->have_cal) c.gd.cal = &p->cal;
    if (!p->args.sim) c.gd.cache = pl_program_cache(p);
    if (p->n_triggers) {
        /* a bound output role's device writes at the flip's vblank
         * (ysp/device.h, AT THE FLIP); a role with no device (--sim, an
         * optional role the rig leaves unbound) has the sink, which counts
         * the firings so a lost one is caught at the end */
        extern void pl_trigger_sink(void* ctx, const yscr_trigger_info* info);
        trig = (yscr_trigger_desc*)pl_alloc(p, sizeof(yscr_trigger_desc) * (size_t)p->n_triggers);
        if (!trig) return -1;
        for (i = 0; i < p->n_triggers; i++) {
            const pl_role* r = pl_role_get(p, p->trigger_roles[i]->name);
            if (r && r->dev) {
                trig[i] = ydev_trigger_channel(r->dev, 0);
            } else {
                trig[i].fn = pl_trigger_sink;
                trig[i].ctx = p;
                trig[i].name = p->trigger_roles[i]->name;
            }
        }
        sd.triggers = trig;
        sd.n_triggers = p->n_triggers;
    }
    sd.on_context = pl_open_hook;
    sd.on_context_ctx = &c;
    if (!yscr_open(&p->scr, &sd)) {
        ygfx_close(&p->gfx);
        pl_fail(p, "the screen: %s", yscr_error(&p->scr));
        return -1;
    }
    p->scr_open = true;
    c.gd.screen = &p->scr;
    if (!ygfx_open(&p->gfx, &c.gd)) { pl_fail(p, "the gfx: %s", ygfx_error(&p->gfx)); return -1; }
    p->gfx_open = true;
    /* the built-in kinds' first draws now, not on a trial's frames */
    if (ygfx_prime(&p->gfx) < 0) { pl_fail(p, "the gfx: %s", ygfx_error(&p->gfx)); return -1; }
    if (!p->args.sim && pl_lead_in(p) < 0) return -1;
    yscr_get_caps(&p->scr, &p->caps);
    return 0;
}

static int pl_fields_init(pl_player* p) {
    int i, k;
    p->field_values = (pl_val*)pl_alloc(p, sizeof(pl_val) * (size_t)(p->def->n_fields + 1));
    if (!p->field_values) return -1;
    p->participant = 0;
    for (i = 0; i < p->def->n_fields; i++) {
        const yexp_field* f = &p->def->fields[i];
        const char* given = NULL;
        pl_val* v = &p->field_values[i];
        for (k = 0; k < p->args.n_fields; k++)
            if (strcmp(p->args.fields[k].name, f->name) == 0) given = p->args.fields[k].value;
        if (!given) {
            if (f->required) { pl_fail(p, "the session field '%s' is required: give --field %s=VALUE", f->name, f->name); return -1; }
            v->kind = f->is_string ? YEXP_V_STRING : YEXP_V_NUMBER;
            v->flags = PL_VF_ABSENT;
            v->str = "";
            continue;
        }
        if (f->is_string) {
            v->kind = YEXP_V_STRING;
            v->str = given;
        } else {
            char* e;
            long long x = strtoll(given, &e, 10);
            if (*e || e == given) { pl_fail(p, "the session field '%s' takes an integer", f->name); return -1; }
            v->kind = YEXP_V_NUMBER;
            v->flags = YEXP_VF_INT;
            v->i = x;
            v->num = (double)x;
        }
        if (strcmp(f->name, "participant") == 0) p->participant = v->i;
    }
    /* A string participant cannot be @participant (the loader's gap; the
     * rules text is known now). */
    {
        int li;
        for (li = 0; li < p->def->n_fields; li++)
            if (strcmp(p->def->fields[li].name, "participant") == 0 && p->def->fields[li].is_string) {
                for (i = 0; i < p->n_leaves; i++)
                    if (strstr(p->leaves[i].node->rules, "@participant")) {
                        pl_fail(p, "node '%s': '@participant' needs an integer session field, but 'participant' is a string", p->leaves[i].node->path);
                        return -1;
                    }
            }
    }
    return 0;
}

extern int pl_columns_build(pl_player* p);
extern int pl_channels_assign(pl_player* p);

int pl_load(pl_player* p, const pl_args* a) {
    ypak_desc pd;
    ypak_entry e;
    const void* data;
    yexp_host host;
    int rc, i, count_leaves = 0;
    memset(p, 0, sizeof *p);
    p->args = *a;
    memset(&pd, 0, sizeof pd);
    pd.path = a->pack_path;
    if (!ypak_open(&p->pack, &pd)) { pl_fail(p, "the pack '%s': %s", a->pack_path, ypak_error(&p->pack)); return -1; }
    p->pack_open = true;
    ypak_id(&p->pack, p->pack_id);
    if (ypak_first_of(&p->pack, YPAK_KIND_EXPERIMENT, &e) != 0) { pl_fail(p, "the pack has no experiment entry"); return -1; }
    if (!(data = ypak_data(&p->pack, &e))) { pl_fail(p, "the experiment entry cannot be read: %s", ypak_error(&p->pack)); return -1; }
    if (a->def_text) {
        /* the fuzz target's bytes, against this pack's tables */
        data = a->def_text;
        e.size = (int64_t)a->def_len;
    }
    {
        uint8_t digest[32];
        ypak_sha256(data, (size_t)e.size, digest);
        pl_hex(digest, p->stored_id);
    }
    yjs_arena_init_heap(&p->arena, 0);
    p->arena_open = true;
    if (pl_rig_load(p) < 0) return -1;
    pl_host_init(p, &host);
    rc = yexp_load(&p->arena, (const char*)data, (size_t)e.size, &host, &p->def, &p->report);
    if (rc != YEXP_OK) {
        char buf[800] = "";
        size_t used = 0;
        for (i = 0; i < p->report.n_diags && used + 2 < sizeof buf; i++)
            if (p->report.diags[i].severity == YEXP_ERROR)
                used += (size_t)snprintf(buf + used, sizeof buf - used, "%u:%u: %s\n", p->report.diags[i].line, p->report.diags[i].column, p->report.diags[i].msg);
        if (p->failed && !buf[0]) return -1;
        p->failed = 0;   /* the report's message is the better one */
        pl_fail(p, "the definition is refused (%d errors):\n%s", p->report.n_errors, rc == YEXP_ERR_NOMEM ? "out of memory" : buf);
        return -1;
    }
    {
        uint8_t id[32];
        if (yexp_canonical(p->def, &p->arena, &p->canon, &p->canon_n, id) != YEXP_OK) { pl_fail(p, "cannot write the canonical form"); return -1; }
        pl_hex(id, p->experiment_id);
    }
    p->seed = a->have_seed ? a->seed : 0;
    if (!a->have_seed) {
        FILE* fp = fopen("/dev/urandom", "rb");
        uint64_t s = 0;
        if (fp) { if (fread(&s, sizeof s, 1, fp) != 1) s = 0; fclose(fp); }
        p->seed = s;
    }
    p->participant_rng = p->seed ^ 0x706c617965725f5fULL;
    memset(&p->env, 0, sizeof p->env);
    if (pl_cal_load(p) < 0) return -1;

    if (pl_channels_assign(p) < 0) return -1;
    if (pl_roles_bind(p) < 0 || pl_outputs_open(p) < 0) return -1;
    if (pl_screen_open(p) < 0) return -1;
    if (pl_inputs_open(p) < 0) return -1;

    /* stimuli */
    p->n_stims = p->def->n_stimuli;
    p->stims = (pl_stim*)pl_alloc(p, sizeof(pl_stim) * (size_t)(p->n_stims + 1));
    p->draw_order = (int*)pl_alloc(p, sizeof(int) * (size_t)(p->n_stims + 1));
    if (!p->stims || !p->draw_order) return -1;
    for (i = 0; i < p->n_stims; i++) p->stims[i].def = &p->def->stimuli[i];
    for (i = 0; i < p->n_stims; i++) {
        pl_stim* s = &p->stims[i];
        s->ch_visible = pl_channel(p, s->def->name, NULL);
        if (pl_stim_make(p, s) < 0) return -1;
    }
    /* draw order: layer, then the name (D3) */
    for (i = 0; i < p->n_stims; i++) p->draw_order[i] = i;
    for (i = 1; i < p->n_stims; i++) {
        int v = p->draw_order[i], j = i - 1;
        const pl_stim* sv = &p->stims[v];
        while (j >= 0 && (p->stims[p->draw_order[j]].def->layer > sv->def->layer)) {
            p->draw_order[j + 1] = p->draw_order[j];
            j--;
        }
        p->draw_order[j + 1] = v;
    }
    /* the text program's first draw now, off the frame path (docs/gfx.md),
     * as trial_stroop does */
    if (p->runs_open) {
        ygfx_crun_desc rd;
        ygfx_citem item;
        uint32_t f;
        memset(&item, 0, sizeof item);
        memset(&rd, 0, sizeof rd);
        rd.items = &item;
        rd.n = 1;
        rd.size = 1.0f;
        /* ygfx_crun() builds its per-set draw state on first use: do that here */
        for (f = 0; f < p->runs.n_fonts; f++) {
            rd.set = p->csets[f];
            (void)ygfx_crun(&p->gfx, &rd);
        }
        ygfx_prime(&p->gfx);
    }
    /* static properties once, with session-level values */
    memset(&p->env, 0, sizeof p->env);
    for (i = 0; i < p->n_stims; i++) {
        pl_stim* s = &p->stims[i];
        int k;
        for (k = 0; k < s->def->n_props; k++)
            if (pl_apply_key(p, s, &s->def->props[k], PL_KEY_LOAD) < 0) return -1;
    }
    /* textures of image stimuli, by every level a variable can take */
    {
        int n_all = pl_count_leaves(p->def->timeline, p->def->n_timeline), got = 0;
        p->def_leaves = (const yexp_node**)pl_alloc(p, sizeof(yexp_node*) * (size_t)(n_all + 1));
        if (!p->def_leaves) return -1;
        pl_list_leaves(p->def->timeline, p->def->n_timeline, p->def_leaves, &got);
        count_leaves = got;
    }
    for (i = 0; i < p->n_stims; i++) {
        const yexp_stim* d = p->stims[i].def;
        if (d->kind == YEXP_IMAGE) {
            const yexp_prop* tx = yexp_prop_get(d->props, d->n_props, "texture");
            int k;
            pl_each_text(p, tx->value, NULL, pl_preload_tex_cb);
            for (k = 0; k < count_leaves && !p->failed; k++)
                pl_each_text(p, tx->value, p->def_leaves[k], pl_preload_tex_cb);
            if (p->failed) return -1;
        }
    }

    /* methods */
    p->n_methods = p->def->n_methods;
    p->methods = (pl_method*)pl_alloc(p, sizeof(pl_method) * (size_t)(p->n_methods + 1));
    if (!p->methods) return -1;
    for (i = 0; i < p->n_methods; i++) {
        p->methods[i].def = &p->def->methods[i];
        if (pl_method_open(p, &p->methods[i]) < 0) return -1;
    }

    /* templates */
    p->n_tmpls = p->def->n_templates;
    p->tmpls = (pl_tmpl*)pl_alloc(p, sizeof(pl_tmpl) * (size_t)(p->n_tmpls + 1));
    if (!p->tmpls) return -1;
    for (i = 0; i < p->n_tmpls; i++) {
        p->tmpls[i].def = &p->def->templates[i];
        if (pl_tmpl_open(p, &p->tmpls[i]) < 0) return -1;
    }

    /* the timeline */
    p->n_binds = p->n_channels;
    p->binds = (ygfx_bind*)pl_alloc(p, sizeof(ygfx_bind) * (size_t)(p->n_channels + 1));
    p->tl_initial = (float*)pl_alloc(p, sizeof(float) * (size_t)(p->n_channels + 1));
    p->chan_src = (float**)pl_alloc(p, sizeof(float*) * (size_t)(p->n_channels + 1));
    if (!p->binds || !p->tl_initial || !p->chan_src) return -1;
    p->chan_base = (float*)pl_alloc(p, sizeof(float) * (size_t)(p->n_channels + 1));
    p->chan_dyn = (bool*)pl_alloc(p, sizeof(bool) * (size_t)(p->n_channels + 1));
    if (!p->chan_base || !p->chan_dyn) return -1;
    for (i = 0; i < p->n_channels; i++) {
        pl_chan* c = &p->channels[i];
        pl_stim* s = pl_stim_by_name(p, c->stim->name);
        ygfx_bind* b = &p->binds[i];
        int gkind = pl_kind_gfx(s->def->kind), idx;
        memset(b, 0, sizeof *b);
        b->channel = (uint16_t)i;
        if (!c->prop) {
            b->stim = &s->gs;
            b->param = YGFX_P_VISIBLE;
            p->chan_src[i] = NULL;     /* visibility starts each trial at 0 */
        } else if (s->def->kind == YEXP_RDK && (strcmp(c->prop, "coherence") == 0 || strcmp(c->prop, "direction") == 0 || strcmp(c->prop, "speed") == 0)) {
            float* f = c->prop[0] == 'c' ? &s->field.coherence : c->prop[0] == 'd' ? &s->field.direction : &s->field.speed;
            b->field = f;
            p->chan_src[i] = f;
        } else {
            const ygfx_param* r = pl_gfx_row(c->prop, gkind, &idx);
            if (!r) {
                pl_fail(p, "stimulus '%s': the step's property '%s' is not a float of its kind", s->def->name, c->prop);
                return -1;
            }
            b->stim = &s->gs;
            b->param = (uint16_t)idx;
            p->chan_src[i] = (float*)((char*)&s->gs + r->offset);
        }
        if (c->prop) {
            const char* key = strncmp(c->prop, "color_", 6) == 0 ? "color" : c->prop;
            const yexp_prop* pr = yexp_prop_get(s->def->props, s->def->n_props, key);
            /* A tween writes this field every frame, so the channel is its
             * one writer; a key known only after the response would have no
             * effect. Which should win is not defined: stop rather than
             * guess (docs/player.md). */
            if (pr && pl_value_after(pr->value)) {
                pl_fail(p, "stimulus '%s': the key '%s' depends on the response and a step drives it", s->def->name, key);
                return -1;
            }
            p->chan_base[i] = p->chan_src[i] ? *p->chan_src[i] : 0.0f;
            p->chan_dyn[i] = pr && !pl_value_static(pr->value);
        }
    }
    {
        int cap = 8;
        for (i = 0; i < p->n_tmpls; i++) cap += p->tmpls[i].ops_cap;
        p->tl_events = (ytl_event*)pl_alloc(p, sizeof(ytl_event) * (size_t)cap);
        p->tl_keys = (ytl_key*)pl_alloc(p, sizeof(ytl_key) * (size_t)(cap * 4));
        p->fired = (ytl_event*)pl_alloc(p, sizeof(ytl_event) * (size_t)cap);
        if (!p->tl_events || !p->tl_keys || !p->fired) return -1;
        p->fired_cap = cap;
        memset(&p->tl_desc, 0, sizeof p->tl_desc);
        p->tl_desc.events = p->tl_events;
        p->tl_desc.event_capacity = cap;
        p->tl_desc.n_channels = p->n_channels;
        p->tl_desc.initial = p->tl_initial;
        p->tl_desc.keys = p->tl_keys;
        p->tl_desc.key_capacity = cap * 4;
    }

    /* the leaves */
    {
        int count = pl_count_leaves(p->def->timeline, p->def->n_timeline), idx = 0;
        p->leaves = (pl_leaf*)pl_alloc(p, sizeof(pl_leaf) * (size_t)(count + 1));
        if (!p->leaves) return -1;
        p->n_leaves = count;
        if (pl_fields_init(p) < 0) return -1;
        memset(&p->env, 0, sizeof p->env);
        p->env.node = NULL;
        if (pl_leaves_collect(p, p->def->timeline, p->def->n_timeline, &idx) < 0) return -1;
    }

    if (pl_check_nodes(p, p->def->timeline, p->def->n_timeline) < 0) return -1;
    if (!pl_runnable(p, p->def->timeline, p->def->n_timeline)) {
        pl_fail(p, "the timeline has no leaf that can run: its containers have no children or tables with no rows");
        return -1;
    }
    {
        double visits = pl_container_visits(p, p->def->timeline, p->def->n_timeline);
        if (visits > PL_MAX_CONTAINER_VISITS) {
            pl_fail(p, "the tree starts its containers %.0f times (repetitions x rows x loop max); the walk takes at most %d, about %d s of blank frames at 60 Hz",
                    visits, PL_MAX_CONTAINER_VISITS, PL_MAX_CONTAINER_VISITS / PL_WALK_PER_FRAME / 60);
            return -1;
        }
    }
    if (pl_text_bound(p) < 0 || pl_rdk_steps(p) < 0) return -1;
    for (i = 0; i < p->def->n_templates; i++)
        if (pl_check_steps(p, p->def->templates[i].events, p->def->templates[i].n_events, p->def->templates[i].name) < 0 ||
            pl_check_steps(p, p->def->templates[i].after, p->def->templates[i].n_after, p->def->templates[i].name) < 0)
            return -1;
    if (pl_columns_build(p) < 0) return -1;
    {
        /* a row's text: each text column's longest value */
        int c, n_text = 0;
        for (c = 0; c < p->n_cols; c++) if (p->cols[c].type == YTR_COL_STRING) n_text++;
        p->row_text_cap = (size_t)n_text * (p->max_text + 1) + 64;
    }
    return p->failed ? -1 : 0;
}

void pl_close(pl_player* p) {
    int i;
    for (i = 0; i < p->n_stims && p->stims; i++) {
        pl_stim* s = &p->stims[i];
        if (s->def && s->def->kind == YEXP_RDK && s->xy) yrdk_close(&s->field);
    }
    for (i = 0; i < p->n_methods && p->methods; i++) {
        pl_method* m = &p->methods[i];
        if (m->quest) yqst_close(m->quest);
        if (m->stair) yst_close(m->stair);
    }
    for (i = 0; i < p->n_tmpls && p->tmpls; i++)
        if (p->tmpls[i].collector) yrsp_close(p->tmpls[i].collector);
    if (p->audio_open) yau_close(&p->audio);
    if (p->ds_open) yds_close(&p->ds);
    if (p->gfx_open) ygfx_close(&p->gfx);
    /* the screen first: its trigger worker writes through the outputs, and
     * its close puts the display's mode and ramp back */
    if (p->scr_open) yscr_close(&p->scr);
    pl_devices_close(p);
    if (p->trace_file) { fclose((FILE*)p->trace_file); p->trace_file = NULL; }
    if (p->arena_open) yjs_arena_free(&p->arena);
    if (p->pack_open) ypak_close(&p->pack);
    {
        pl_block* b = (pl_block*)p->blocks;
        while (b) {
            pl_block* n = b->next;
            free(b);
            b = n;
        }
    }
    p->blocks = NULL;
}
