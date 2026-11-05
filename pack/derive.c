/* derive.c - the texts an experiment definition can show, for the glyph
 * runs that the pack tool lays out (pack/ysp/pack_tool.h, ypt_derive_texts;
 * docs/pack.md 4.5, docs/experiment_format.md D14 and gap G9).
 *
 * The player lays out no text at run time for a data-only definition
 * (D14), so every text a `text` stimulus can show must be known here: a
 * literal, the levels of a table column, the values of a node's `vars`, or
 * every value of a lookup's map. Anything else is refused by name, so a
 * pack never lacks a text that the player would need in the middle of a
 * session. The rules are deliberately an over-approximation: a column's
 * levels come from every table any node names, and a map gives all of its
 * values, whichever keys the trials can reach; an unused block costs a few
 * hundred bytes, a missing one stops a session.
 *
 * ypt_check_lookups() checks the other side of a lookup (tool v0.7.0):
 * its map has a key for every level its "by" can take, in each leaf that
 * reaches it. That check is exact, not wide: a missing key is an error.
 *
 * Needs ysp/json.h and ysp/table.h (their implementations come from
 * pack/pack_impl.c, or from the test or fuzz target that includes this
 * file). No layout here, so the core tool derives too and can name what
 * it would need the layout library for. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "ysp/pack_tool.h"
#include "ysp/pack.h"
#include "ysp/json.h"
#include "ysp/table.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A variable that holds another variable that holds ... A real definition
 * nests one or two; past this a loop is the likely cause. */
#define YPT_DERIVE_DEPTH 8
/* Values resolved in all: a vars name defined in many nodes, each naming
 * another such name, multiplies the work; the Stroop definition needs 9. */
#define YPT_DERIVE_STEPS 100000

typedef struct ypt__dmem {
    yjs_arena    a;
    ypt_text*    t;      int nt, ct;
    const char** tab;    int ntab, ctab;
} ypt__dmem;

/* One text stimulus's layout keys while its texts are collected. */
typedef struct ypt__stim {
    const char* name;
    ypt_text    proto;   /* every field but key and text */
} ypt__stim;

typedef struct ypt__d {
    ypt__dmem*       m;
    const yjs_value* root;
    ypt_table_fn     table;
    void*            ctx;
    /* from the timeline, every node: the TABLE names and the vars objects */
    const yjs_value* tv[256];    const char* tv_node[256];    int ntv;
    ytb_table*       tvt[256];   /* each table's view, opened at its first use */
    const yjs_value* vars[256];  const char* vars_node[256];  int nvars;
    long             steps;
    char*            err;
    size_t           cap;
} ypt__d;

static int dfail(ypt__d* d, int code, const char* fmt, ...) {
    va_list ap;
    if (d->err && d->cap && !d->err[0]) {
        va_start(ap, fmt);
        vsnprintf(d->err, d->cap, fmt, ap);
        va_end(ap);
    }
    return code;
}

static char* dstr(ypt__d* d, const char* s, size_t n) {
    char* p = (char*)yjs_alloc(&d->m->a, n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static const char* const ypt__builtins[] = {
    "trial_index", "block", "rep", "condition", "practice", "warmup", "first_in_block", "after_break", "trial_seed",
    "method", "level", "estimate", "response", "rt_s", "responded", "correct", "outcome", "contrast_shown", NULL };

static int is_builtin(const char* name) {
    int i;
    for (i = 0; ypt__builtins[i]; i++)
        if (!strcmp(name, ypt__builtins[i])) return 1;
    return 0;
}

/* One text of a stimulus, keyed by ypak_runs_key(): the player builds the
 * same key from the stimulus's style and the text it resolves. */
static int add_text(ypt__d* d, const ypt__stim* s, const char* text, size_t n) {
    ypt__dmem* m = d->m;
    ypt_text* t;
    int64_t kn = ypak_runs_key(&s->proto.style, text, n, NULL, 0);
    char* key;
    if (kn < 0) return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': a text with a NUL byte, or a style ypak_runs_key() refuses", s->name);
    if (m->nt == m->ct) {
        int c = m->ct ? 2 * m->ct : 32;
        ypt_text* q = (ypt_text*)realloc(m->t, (size_t)c * sizeof(ypt_text));
        if (!q) return dfail(d, YPAK_ERR_NOMEM, "out of memory");
        m->t = q;
        m->ct = c;
    }
    key = (char*)yjs_alloc(&m->a, (size_t)kn + 1);
    if (!key) return dfail(d, YPAK_ERR_NOMEM, "out of memory");
    ypak_runs_key(&s->proto.style, text, n, key, (size_t)kn + 1);
    t = &m->t[m->nt++];
    *t = s->proto;
    t->key = key;
    t->text = key + (size_t)kn - n;
    return 0;
}

static int add_table(ypt__d* d, const char* name) {
    ypt__dmem* m = d->m;
    int i;
    for (i = 0; i < m->ntab; i++)
        if (!strcmp(m->tab[i], name)) return 0;
    if (m->ntab == m->ctab) {
        int c = m->ctab ? 2 * m->ctab : 8;
        const char** q = (const char**)realloc((void*)m->tab, (size_t)c * sizeof(char*));
        if (!q) return dfail(d, YPAK_ERR_NOMEM, "out of memory");
        m->tab = q;
        m->ctab = c;
    }
    m->tab[m->ntab++] = name;
    return 0;
}

static int resolve(ypt__d* d, const ypt__stim* s, const yjs_value* v, int depth);

/* Every value "$name" can take: a table column's levels, a vars entry's
 * value or choices. A built-in or a session field is refused: its text is
 * not known here. */
static int resolve_var(ypt__d* d, const ypt__stim* s, const char* name, int depth) {
    int i, found = 0, rc;
    const yjs_value* fields;
    if (depth > YPT_DERIVE_DEPTH)
        return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': '$%s': variables nest more than %d deep; is there a loop?", s->name, name, YPT_DERIVE_DEPTH);
    if (is_builtin(name))
        return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': '$%s' is a built-in variable, whose text is not known at pack build; give each text "
                     "with a lookup: {\"by\": \"$%s\", \"map\": {...}}", s->name, name, name);
    for (i = 0; i < d->ntv; i++) {
        const char* tn = d->tv[i]->u.s;
        ytb_table* t = d->tvt[i];
        int col, lv, nl;
        if (!t) {
            size_t n = 0;
            const void* bytes = d->table(d->ctx, tn, &n);
            if (!bytes)
                return dfail(d, YPAK_ERR_FORMAT, "node '%s': 'timeline_variables' is '%s', which is not a table entry of this pack",
                             d->tv_node[i], tn);
            t = (ytb_table*)yjs_alloc(&d->m->a, sizeof(ytb_table));
            if (!t) return dfail(d, YPAK_ERR_NOMEM, "out of memory");
            if (!ytb_open_view(t, bytes, n)) return dfail(d, YPAK_ERR_FORMAT, "'%s': %s", tn, ytb_error(t));
            d->tvt[i] = t;
        }
        col = ytb_col(t, name);
        if (col < 0) continue;
        if (ytb_col_type(t, col) != YTB_STRING)
            return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': '$%s' is a column of '%s' of type %s; a text comes from a string column "
                         "(\"types\": {\"%s\": \"string\"} in the table's resource) or from a lookup", s->name, name, tn,
                         ytb_type_name(ytb_col_type(t, col)), name);
        found = 1;
        rc = add_table(d, tn);
        if (rc) return rc;
        nl = ytb_n_levels(t, col);
        for (lv = 0; lv < nl; lv++) {
            const char* x = ytb_level_text(t, col, lv);
            if (!x) return dfail(d, YPAK_ERR_FORMAT, "'%s': column '%s' has no level %d", tn, name, lv);
            rc = add_text(d, s, x, strlen(x));
            if (rc) return rc;
        }
    }
    for (i = 0; i < d->nvars; i++) {
        const yjs_value* v = yjs_get(d->vars[i], name);
        const yjs_value* ch;
        size_t k;
        if (!v) continue;
        found = 1;
        ch = v->type == YJS_OBJECT ? yjs_get(v, "choice") : NULL;
        if (ch && ch->type == YJS_ARRAY) {
            for (k = 0; k < ch->n; k++) {
                rc = resolve(d, s, ch->u.items[k], depth);
                if (rc) return rc;
            }
        } else {
            rc = resolve(d, s, v, depth);
            if (rc) return rc;
        }
    }
    if (found) return 0;
    fields = yjs_get(yjs_get(d->root, "session"), "fields");
    if (!strcmp(name, "participant") || (fields && fields->type == YJS_OBJECT && yjs_get(fields, name)))
        return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': '$%s' is a session field, given at session start; its text cannot be laid out at pack build",
                     s->name, name);
    return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': '$%s' names no table column, 'vars' entry or built-in variable (a jitter is a duration, not a text)",
                 s->name, name);
}

/* Every text a value can give (docs/experiment_format.md 2.2). */
static int resolve(ypt__d* d, const ypt__stim* s, const yjs_value* v, int depth) {
    if (++d->steps > YPT_DERIVE_STEPS)
        return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': its texts take more than %d steps to resolve; do 'vars' refer to each "
                     "other across many nodes?", s->name, YPT_DERIVE_STEPS);
    if (v->type == YJS_STRING) {
        const char* x = v->u.s;
        if (x[0] == '$' && x[1] == '$') return add_text(d, s, x + 1, v->n - 1);   /* "$$..." is a literal "$..." */
        if (x[0] == '$') return resolve_var(d, s, x + 1, depth + 1);
        return add_text(d, s, x, v->n);
    }
    if (v->type == YJS_OBJECT) {
        const yjs_value* map = yjs_get(v, "map");
        if (yjs_get(v, "lua"))
            return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': text from a script ({\"lua\": ...}) is laid out by the player at run time, and "
                         "version 1 has no scripts (D6)", s->name);
        if (yjs_get(v, "by") && map && map->type == YJS_OBJECT) {
            size_t k;
            int rc;
            for (k = 0; k < map->n; k++) {
                rc = resolve(d, s, map->u.members[k].value, depth);
                if (rc) return rc;
            }
            return 0;
        }
    }
    return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': a text is a string, a \"$variable\" or a lookup {\"by\": ..., \"map\": {...}}; this value "
                 "is none of them", s->name);
}

/* A layout key: a JSON number, as a double and in yjs_format_double()'s
 * spelling, which ypak_runs_key() puts in the key. */
static int num_key(ypt__d* d, const ypt__stim* s, const yjs_value* o, const char* key, double lo, int lo_open, double* val,
                   const char** out) {
    const yjs_value* v = yjs_get(o, key);
    char buf[64];
    size_t n;
    double x;
    if (!v) { *val = 0; *out = "0"; return 0; }
    if (v->type != YJS_NUMBER)
        return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': '%s' must be a number: glyph runs are laid out at pack build (D14), so a text's font, "
                     "size, width, line_height, align, dir and lang are literal values", s->name, key);
    n = yjs_write_mem(v, YJS_WRITE_COMPACT, buf, sizeof buf);
    if (n == 0 || n >= sizeof buf || yjs_double(v, &x) != YJS_OK || x != x || (lo_open ? !(x > lo) : !(x >= lo)) || x > 1e9)
        return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': '%s' is %.*s; it is %s %g and at most 1e9", s->name, key, (int)(n < sizeof buf ? n : 0), buf,
                     lo_open ? "above" : "at least", lo);
    *val = x;
    n = (size_t)yjs_format_double(x, buf, sizeof buf);
    *out = dstr(d, buf, n);
    return *out ? 0 : dfail(d, YPAK_ERR_NOMEM, "out of memory");
}

static int word_key(ypt__d* d, const ypt__stim* s, const yjs_value* o, const char* key, const char* const* allowed, const char** out) {
    const yjs_value* v = yjs_get(o, key);
    int i;
    if (!v) { *out = allowed ? allowed[0] : ""; return 0; }
    if (v->type != YJS_STRING)
        return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': '%s' must be a string: glyph runs are laid out at pack build (D14), so a text's font, "
                     "size, width, line_height, align, dir and lang are literal values", s->name, key);
    if (allowed) {
        for (i = 0; allowed[i]; i++)
            if (!strcmp(v->u.s, allowed[i])) { *out = allowed[i]; return 0; }
        return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': '%s' is \"%s\"; it is %s, %s or %s", s->name, key, v->u.s, allowed[0], allowed[1], allowed[2]);
    }
    if (v->u.s[0] == '$')
        return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': '%s' is a variable: glyph runs are laid out at pack build (D14), so a text's font, "
                     "size, width, line_height, align, dir and lang are literal values", s->name, key);
    *out = v->u.s;
    return 0;
}

static int text_stim(ypt__d* d, const char* name, const yjs_value* o) {
    static const char* const aligns[] = { "start", "end", "center", NULL };
    static const char* const dirs[] = { "auto", "ltr", "rtl", NULL };
    ypt__stim s;
    const yjs_value* tv = yjs_get(o, "text");
    ypt_text* p = &s.proto;
    size_t k;
    int rc;
    memset(&s, 0, sizeof s);
    s.name = name;
    s.proto.stimulus = name;
    if (!yjs_get(o, "font") || !yjs_get(o, "size") || !tv)
        return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': a text needs 'font' (a FONT entry of the pack), 'size' and 'text'", name);
    if ((rc = word_key(d, &s, o, "font", NULL, &p->font)) != 0 ||
        (rc = num_key(d, &s, o, "size", 0, 1, &p->style.size, &p->size)) != 0 ||
        (rc = num_key(d, &s, o, "width", 0, 0, &p->style.width, &p->width)) != 0 ||
        (rc = num_key(d, &s, o, "line_height", 0, 0, &p->style.line_height, &p->line_height)) != 0 ||
        (rc = word_key(d, &s, o, "align", aligns, &p->align)) != 0 || (rc = word_key(d, &s, o, "dir", dirs, &p->dir)) != 0 ||
        (rc = word_key(d, &s, o, "lang", NULL, &p->lang)) != 0)
        return rc;
    if (!p->font[0]) return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': 'font' is empty", name);
    if (strchr(p->font, '#') || strchr(p->font, '|') || !ypak_name_ok(p->font, strlen(p->font)))
        return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': 'font' is \"%s\", which is not a pack entry name", name, p->font);
    for (k = 0; p->lang[k]; k++) {
        char c = p->lang[k];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
            return dfail(d, YPAK_ERR_FORMAT, "stimulus '%s': 'lang' is \"%s\"; a BCP 47 tag has letters, digits and '-' only", name, p->lang);
    }
    p->style.font = p->font;
    p->style.face = 0;
    p->style.lang = p->lang;
    p->style.align = !strcmp(p->align, "end") ? YPAK_ALIGN_END : !strcmp(p->align, "center") ? YPAK_ALIGN_CENTER : YPAK_ALIGN_START;
    p->style.dir = !strcmp(p->dir, "ltr") ? YPAK_DIR_LTR : !strcmp(p->dir, "rtl") ? YPAK_DIR_RTL : YPAK_DIR_AUTO;
    return resolve(d, &s, tv, 0);
}

/* Every node's timeline_variables and vars, in tree order. */
static int walk(ypt__d* d, const yjs_value* arr, int depth) {
    size_t i;
    if (!arr || arr->type != YJS_ARRAY) return 0;
    if (depth > 64) return dfail(d, YPAK_ERR_FORMAT, "the timeline nests more than 64 deep");
    for (i = 0; i < arr->n; i++) {
        const yjs_value* node = arr->u.items[i];
        const yjs_value* tv;
        const yjs_value* vars;
        const yjs_value* nm;
        const char* name;
        int rc;
        if (node->type != YJS_OBJECT) continue;
        nm = yjs_get(node, "name");
        name = nm && nm->type == YJS_STRING ? nm->u.s : "";
        tv = yjs_get(node, "timeline_variables");
        vars = yjs_get(node, "vars");
        if (tv) {
            if (tv->type != YJS_STRING) return dfail(d, YPAK_ERR_FORMAT, "node '%s': 'timeline_variables' names a table entry", name);
            if (d->ntv == 256) return dfail(d, YPAK_ERR_FORMAT, "more than 256 nodes name a table");
            d->tv_node[d->ntv] = name;
            d->tv[d->ntv++] = tv;
        }
        if (vars && vars->type == YJS_OBJECT) {
            if (d->nvars == 256) return dfail(d, YPAK_ERR_FORMAT, "more than 256 nodes have 'vars'");
            d->vars_node[d->nvars] = name;
            d->vars[d->nvars++] = vars;
        }
        rc = walk(d, yjs_get(node, "timeline"), depth + 1);
        if (rc) return rc;
    }
    return 0;
}

static int text_cmp(const void* a, const void* b) {
    const ypt_text* x = (const ypt_text*)a;
    const ypt_text* y = (const ypt_text*)b;
    int c = strcmp(x->key, y->key);
    return c ? c : strcmp(x->stimulus, y->stimulus);
}
static int str_cmp(const void* a, const void* b) { return strcmp(*(const char* const*)a, *(const char* const*)b); }

void ypt_texts_free(ypt_texts* t) {
    ypt__dmem* m;
    if (!t) return;
    m = (ypt__dmem*)t->mem_;
    if (m) {
        yjs_arena_free(&m->a);
        free(m->t);
        free((void*)m->tab);
        free(m);
    }
    memset(t, 0, sizeof *t);
}

int ypt_derive_texts(const void* def, size_t n, ypt_table_fn table, void* ctx, ypt_texts* out, char* err, size_t cap) {
    ypt__d d;
    ypt__dmem* m;
    yjs_value* root;
    yjs_error je;
    const yjs_value* fmt;
    const yjs_value* ver;
    const yjs_value* stimuli;
    int rc = 0, i, k, walked = 0;
    if (err && cap) err[0] = 0;
    if (!out) return YPAK_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (!def || !table) return YPAK_ERR_ARG;
    m = (ypt__dmem*)calloc(1, sizeof *m);
    if (!m) { if (err && cap) snprintf(err, cap, "out of memory"); return YPAK_ERR_NOMEM; }
    out->mem_ = m;
    yjs_arena_init_heap(&m->a, 0);
    memset(&d, 0, sizeof d);
    d.m = m;
    d.table = table;
    d.ctx = ctx;
    d.err = err;
    d.cap = cap;
    /* Not JSON, or JSON of another format: opaque bytes, as before G9. But
     * a definition with a typo would silently lose its glyph runs, so bytes
     * that name the format and do not parse are refused with the place. */
    if (yjs_parse(&m->a, (const char*)def, n, NULL, &root, &je) != YJS_OK) {
        static const char tag[] = "\"ysp-experiment\"";
        const char* p = (const char*)def;
        size_t k2;
        for (k2 = 0; k2 + sizeof tag - 1 <= n; k2++)
            if (p[k2] == '"' && !memcmp(p + k2, tag, sizeof tag - 1)) {
                rc = dfail(&d, YPAK_ERR_FORMAT, "the definition names \"ysp-experiment\" but is not JSON: line %u, column %u: %s",
                           (unsigned)je.line, (unsigned)je.column, je.msg);
                ypt_texts_free(out);
                return rc;
            }
        return 0;
    }
    fmt = yjs_get(root, "format");
    if (!fmt || fmt->type != YJS_STRING || strcmp(fmt->u.s, "ysp-experiment")) return 0;
    ver = yjs_get(root, "version");
    if (!ver || !(ver->flags & YJS_NUM_I64) || ver->i != 1) {
        rc = dfail(&d, YPAK_ERR_FORMAT, "the definition's version is %s; this ypak lays out the texts of version 1",
                   ver && ver->type == YJS_NUMBER ? ver->u.s : "missing or not a number");
        ypt_texts_free(out);
        return rc;
    }
    out->definition = 1;
    d.root = root;
    stimuli = yjs_get(root, "stimuli");
    for (i = 0; rc == 0 && stimuli && stimuli->type == YJS_OBJECT && i < (int)stimuli->n; i++) {
        const yjs_member* s = &stimuli->u.members[i];
        const yjs_value* kind = s->value->type == YJS_OBJECT ? yjs_get(s->value, "kind") : NULL;
        if (!kind || kind->type != YJS_STRING || strcmp(kind->u.s, "text")) continue;
        /* the tree only when a text may need it: the rest is the loader's */
        if (!walked) {
            rc = walk(&d, yjs_get(root, "timeline"), 0);
            walked = 1;
        }
        if (rc == 0) rc = text_stim(&d, s->key, s->value);
    }
    if (rc) { ypt_texts_free(out); return rc; }
    /* sorted by key, each key once: the block order of a GLYPHRUNS entry;
     * stimuli of one style that show one text share its block */
    if (m->nt) qsort(m->t, (size_t)m->nt, sizeof(ypt_text), text_cmp);
    for (i = 0, k = 0; i < m->nt;) {
        int j = i, ns = 0, q;
        const char** names;
        while (j < m->nt && !strcmp(m->t[j].key, m->t[i].key)) j++;
        names = (const char**)yjs_alloc(&m->a, sizeof(char*) * (size_t)(j - i));
        if (!names) { rc = dfail(&d, YPAK_ERR_NOMEM, "out of memory"); ypt_texts_free(out); return rc; }
        for (q = i; q < j; q++)
            if (!ns || strcmp(names[ns - 1], m->t[q].stimulus)) names[ns++] = m->t[q].stimulus;
        m->t[k] = m->t[i];
        m->t[k].stimuli = names;
        m->t[k].n_stimuli = ns;
        k++;
        i = j;
    }
    m->nt = k;
    if (m->ntab) qsort((void*)m->tab, (size_t)m->ntab, sizeof(char*), str_cmp);
    out->texts = m->t;
    out->n_texts = m->nt;
    out->tables = m->tab;
    out->n_tables = m->ntab;
    return 0;
}

/* --- lookups: a key for every level (docs/experiment_format.md 2.2) -------------
 *
 * A lookup's map must have a key for every level its "by" can take, or the
 * player meets a level without a value in the middle of a session. The
 * levels are worked out per leaf node, in the leaf's own scope: its tables
 * and vars and its ancestors' (the nearest first), its template, and the
 * stimuli that template names. Wider (every table any node names, as the
 * texts take) would refuse a definition whose two leaves share a column
 * name but not its levels. A "by" whose levels are not known here (a
 * session field, a number the trial computes, a jitter) is not checked:
 * the player has those only at run time. */

/* Values visited and levels compared, in all: leaves times the size of
 * their templates and stimuli. A real definition needs thousands. */
#define YPT_LOOKUP_STEPS 5000000L

typedef struct ypt__lk {          /* one lookup, over every leaf that reaches it */
    const yjs_value* v;
    const char*      where;       /* "stimulus 'word'", "template 't'", "node 'a/b'" */
    const char*      path;        /* the key inside it: "text", "columns.feedback"  */
    size_t           seq;         /* order in the definition, for the warnings     */
    int              reached, unknown;
    const char**     lv;          /* the levels of every leaf that reaches it      */
    size_t           nlv, clv;
} ypt__lk;

typedef struct ypt__ltab { const char* name; ytb_table* t; } ypt__ltab;

typedef struct ypt__lc {
    yjs_arena        a;
    const yjs_value* stimuli;
    const yjs_value* trials;
    ypt_table_fn     table;
    void*            ctx;
    ypt__lk*         lk;   size_t nlk, clk;      /* sorted by v after the first pass */
    ypt__ltab*       tab;  size_t ntab, ctab;
    const yjs_value* chain[66];  int depth;      /* the leaf's nodes, root first    */
    const char*      node_path;                  /* "a/b"                           */
    int              rt[3];                      /* the leaf's rules can make practice,
                                                    warmup, after_break true (lrules) */
    const yjs_value* tmpl;                       /* the leaf's template, or NULL    */
    const char**     tmp;  size_t ntmp, ctmp;    /* the levels of one "by"          */
    const char**     nm;   size_t nnm, cnm;      /* the stimuli a template names    */
    long             steps;
    char             path[512];
    char*            err;
    size_t           cap;
} ypt__lc;

/* One line: a name, key or level may hold control bytes (JSON escapes),
 * and a message is one line of a log. */
static void lflat(char* s) {
    for (; *s; s++)
        if ((unsigned char)*s < 0x20) *s = '?';
}

static int lfail(ypt__lc* lc, int code, const char* fmt, ...) {
    va_list ap;
    if (lc->err && lc->cap && !lc->err[0]) {
        va_start(ap, fmt);
        vsnprintf(lc->err, lc->cap, fmt, ap);
        va_end(ap);
        lflat(lc->err);
    }
    return code;
}

static int lstep(ypt__lc* lc) {
    if (++lc->steps <= YPT_LOOKUP_STEPS) return 0;
    return lfail(lc, YPAK_ERR_FORMAT, "the definition's lookups take more than %ld steps to check; do many leaves share large templates, "
                 "or do 'vars' choices refer to each other?", YPT_LOOKUP_STEPS);
}

/* Grows a pointer list by one; 0 or YPAK_ERR_NOMEM. */
static int lpush(ypt__lc* lc, const char*** list, size_t* n, size_t* c, const char* s) {
    if (*n == *c) {
        size_t nc = *c ? 2 * *c : 16;
        const char** q = (const char**)realloc((void*)*list, nc * sizeof(char*));
        if (!q) return lfail(lc, YPAK_ERR_NOMEM, "out of memory");
        *list = q;
        *c = nc;
    }
    (*list)[(*n)++] = s;
    return 0;
}

static int is_lookup(const yjs_value* v) {
    const yjs_value* map;
    if (v->type != YJS_OBJECT || !yjs_get(v, "by")) return 0;
    map = yjs_get(v, "map");
    return map && map->type == YJS_OBJECT;
}

static int lk_cmp(const void* a, const void* b) {
    const ypt__lk* x = (const ypt__lk*)a;
    const ypt__lk* y = (const ypt__lk*)b;
    return x->v < y->v ? -1 : x->v > y->v;
}
static int lk_seq_cmp(const void* a, const void* b) {
    const ypt__lk* x = (const ypt__lk*)a;
    const ypt__lk* y = (const ypt__lk*)b;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* First pass: every lookup, with the place a message names. */
static int lcollect(ypt__lc* lc, const yjs_value* v, const char* where, size_t pn);

static int lcollect_member(ypt__lc* lc, const yjs_value* o, size_t k, const char* where, size_t pn) {
    const char* key = o->u.members[k].key;
    size_t kn = strlen(key);
    if (pn + 1 + kn < sizeof lc->path) {
        if (pn) lc->path[pn++] = '.';
        memcpy(lc->path + pn, key, kn + 1);
        pn += kn;
    }
    return lcollect(lc, o->u.members[k].value, where, pn);
}

static int lcollect(ypt__lc* lc, const yjs_value* v, const char* where, size_t pn) {
    size_t k;
    int rc;
    if ((rc = lstep(lc)) != 0) return rc;
    lc->path[pn] = 0;
    if (is_lookup(v)) {
        ypt__lk* r;
        char* p;
        if (lc->nlk == lc->clk) {
            size_t nc = lc->clk ? 2 * lc->clk : 16;
            ypt__lk* q = (ypt__lk*)realloc(lc->lk, nc * sizeof(ypt__lk));
            if (!q) return lfail(lc, YPAK_ERR_NOMEM, "out of memory");
            lc->lk = q;
            lc->clk = nc;
        }
        p = (char*)yjs_alloc(&lc->a, pn + 1);
        if (!p) return lfail(lc, YPAK_ERR_NOMEM, "out of memory");
        memcpy(p, lc->path, pn + 1);
        r = &lc->lk[lc->nlk];
        memset(r, 0, sizeof *r);
        r->v = v;
        r->where = where;
        r->path = p;
        r->seq = lc->nlk++;
    }
    if (v->type == YJS_OBJECT) {
        for (k = 0; k < v->n; k++)
            if ((rc = lcollect_member(lc, v, k, where, pn)) != 0) return rc;
    } else if (v->type == YJS_ARRAY) {
        for (k = 0; k < v->n; k++) {
            size_t q = pn;
            int w = snprintf(lc->path + pn, sizeof lc->path - pn, "[%u]", (unsigned)k);
            if (w > 0 && pn + (size_t)w < sizeof lc->path) q = pn + (size_t)w;
            if ((rc = lcollect(lc, v->u.items[k], where, q)) != 0) return rc;
        }
    }
    return 0;
}

/* A set of names (vars, data, columns): each member's value, never the
 * set itself, so variables named "by" and "map" are not a lookup. */
static int lcollect_names(ypt__lc* lc, const yjs_value* o, const char* where, const char* prefix) {
    size_t k, pn = strlen(prefix);
    int rc;
    if (!o || o->type != YJS_OBJECT) return 0;
    for (k = 0; k < o->n; k++) {
        memcpy(lc->path, prefix, pn + 1);
        if ((rc = lcollect_member(lc, o, k, where, pn)) != 0) return rc;
    }
    return 0;
}

static char* lwhere(ypt__lc* lc, const char* kind, const char* name) {
    size_t n = strlen(kind) + strlen(name) + 4;
    char* p = (char*)yjs_alloc(&lc->a, n);
    if (p) snprintf(p, n, "%s '%s'", kind, name);
    return p;
}

static int lcollect_template(ypt__lc* lc, const yjs_value* t, const char* where) {
    size_t k;
    int rc;
    if (t->type != YJS_OBJECT) return 0;
    for (k = 0; k < t->n; k++) {
        if (!strcmp(t->u.members[k].key, "columns")) rc = lcollect_names(lc, t->u.members[k].value, where, "columns");
        else rc = lcollect_member(lc, t, k, where, 0);
        if (rc) return rc;
    }
    return 0;
}

/* The nodes: "a/b" names a node by its path, as the data does (2.4). */
static int lcollect_nodes(ypt__lc* lc, const yjs_value* arr, const char* parent, int depth) {
    size_t i;
    int rc;
    if (!arr || arr->type != YJS_ARRAY) return 0;
    /* ysp/json.h's default depth (64) stops a tree at 31 nodes; this keeps
     * chain[] in bounds should a caller parse deeper */
    if (depth > 64) return lfail(lc, YPAK_ERR_FORMAT, "the timeline nests more than 64 deep");
    for (i = 0; i < arr->n; i++) {
        const yjs_value* node = arr->u.items[i];
        const yjs_value* nmv;
        char* path;
        char* where;
        size_t n;
        if (node->type != YJS_OBJECT) continue;
        nmv = yjs_get(node, "name");
        n = strlen(parent) + (nmv && nmv->type == YJS_STRING ? strlen(nmv->u.s) : 0) + 2;
        path = (char*)yjs_alloc(&lc->a, n);
        if (!path) return lfail(lc, YPAK_ERR_NOMEM, "out of memory");
        snprintf(path, n, "%s%s%s", parent, parent[0] ? "/" : "", nmv && nmv->type == YJS_STRING ? nmv->u.s : "");
        where = lwhere(lc, "node", path);
        if (!where) return lfail(lc, YPAK_ERR_NOMEM, "out of memory");
        if ((rc = lcollect_names(lc, yjs_get(node, "vars"), where, "vars")) != 0 ||
            (rc = lcollect_names(lc, yjs_get(node, "data"), where, "data")) != 0 ||
            (rc = lcollect_nodes(lc, yjs_get(node, "timeline"), path, depth + 1)) != 0)
            return rc;
    }
    return 0;
}

static int lpush_level(ypt__lc* lc, const char* s) {
    int rc = lstep(lc);
    return rc ? rc : lpush(lc, &lc->tmp, &lc->ntmp, &lc->ctmp, s);
}

static ytb_table* ltable(ypt__lc* lc, const char* name, const char* node) {
    size_t i, n = 0;
    const void* bytes;
    ytb_table* t;
    for (i = 0; i < lc->ntab; i++)
        if (!strcmp(lc->tab[i].name, name)) return lc->tab[i].t;
    bytes = lc->table(lc->ctx, name, &n);
    if (!bytes) {
        lfail(lc, YPAK_ERR_FORMAT, "node '%s': 'timeline_variables' is '%s', which is not a table entry of this pack", node, name);
        return NULL;
    }
    t = (ytb_table*)yjs_alloc(&lc->a, sizeof(ytb_table));
    if (!t) { lfail(lc, YPAK_ERR_NOMEM, "out of memory"); return NULL; }
    if (!ytb_open_view(t, bytes, n)) { lfail(lc, YPAK_ERR_FORMAT, "'%s': %s", name, ytb_error(t)); return NULL; }
    if (lc->ntab == lc->ctab) {
        size_t nc = lc->ctab ? 2 * lc->ctab : 8;
        ypt__ltab* q = (ypt__ltab*)realloc(lc->tab, nc * sizeof(ypt__ltab));
        if (!q) { lfail(lc, YPAK_ERR_NOMEM, "out of memory"); return NULL; }
        lc->tab = q;
        lc->ctab = nc;
    }
    lc->tab[lc->ntab].name = name;
    lc->tab[lc->ntab++].t = t;
    return t;
}

static int lvalue(ypt__lc* lc, const yjs_value* v, int depth);

/* The first three tokens of one line of rules text, as ytr_rules() splits
 * it (trials.h GRAMMAR): blanks between words, '#' outside quotes starts
 * a comment, "a b" is one token and "" in it a quote. A token longer than
 * its slot is cut, which no verb or count this reads needs. Moves *pp past
 * the line's LF. */
static int lline(const char** pp, char w[3][24]) {
    const char* p = *pp;
    int nw = 0;
    for (;;) {
        size_t k = 0;
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p || *p == '\n' || *p == '#') break;
        if (*p == '"') {
            for (p++; *p && *p != '\n'; p++) {
                if (*p == '"' && p[1] != '"') { p++; break; }
                if (*p == '"') p++;
                if (nw < 3 && k < 23) w[nw][k++] = *p;
            }
        } else {
            for (; *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '#'; p++)
                if (nw < 3 && k < 23) w[nw][k++] = *p;
        }
        if (nw < 3) w[nw][k] = 0;
        nw++;
    }
    while (*p && *p != '\n') p++;
    *pp = *p ? p + 1 : p;
    return nw;
}

/* A count of the rules text: 1 above 0, 0 for 0, -1 when it is not one. */
static int lcount(const char* t) {
    int any = 0;
    if (!*t) return -1;
    for (; *t; t++) {
        if (*t < '0' || *t > '9') return -1;
        if (*t != '0') any = 1;
    }
    return any;
}

/* Whether the leaf's rules can make practice, warmup and after_break true
 * (trials.h BLOCKS, BREAKS, PRACTICE, WARMUP): practice trials come from
 * `practice N`, warmup trials from `warmup N`, and a block after the first
 * (`block_size N`, or `groups COLUMN blocked`) starts with after_break.
 * The rules are the leaf's own, since the leaf opens the ytr_trials. A
 * value the rules cannot make is a key no level reaches; rules this
 * cannot read keep both values. */
static void lrules(ypt__lc* lc, const yjs_value* leaf) {
    const yjs_value* r = yjs_get(leaf, "rules");
    size_t i;
    lc->rt[0] = lc->rt[1] = lc->rt[2] = 0;
    if (!r) return;
    if (r->type != YJS_ARRAY) { lc->rt[0] = lc->rt[1] = lc->rt[2] = 1; return; }
    for (i = 0; i < r->n; i++) {
        const char* p;
        if (r->u.items[i]->type != YJS_STRING) { lc->rt[0] = lc->rt[1] = lc->rt[2] = 1; return; }
        for (p = r->u.items[i]->u.s; *p;) {
            char w[3][24];
            int nw = lline(&p, w), c;
            if (nw < 1) continue;
            if (!strcmp(w[0], "practice") || !strcmp(w[0], "warmup") || !strcmp(w[0], "block_size")) {
                int k = w[0][0] == 'p' ? 0 : w[0][0] == 'w' ? 1 : 2;
                c = nw >= 2 ? lcount(w[1]) : -1;
                if (c != 0) lc->rt[k] = 1;   /* above 0, or unreadable */
            } else if (!strcmp(w[0], "groups")) {
                if (nw < 3 || strcmp(w[2], "alternate")) lc->rt[2] = 1;   /* blocked, or unreadable */
            }
        }
    }
}

/* The levels "$name" takes in the current leaf: 1 known (pushed), 0 not
 * known here, < 0 an error. */
static int lvar(ypt__lc* lc, const char* name, int depth) {
    static const char* const bools[] = { "practice", "warmup", "after_break", "first_in_block", "responded", "correct", NULL };
    const yjs_value* resp = lc->tmpl ? yjs_get(lc->tmpl, "response") : NULL;
    int i, rc;
    if (YPT_DERIVE_DEPTH < depth) return 0;   /* a loop of vars: the loader's to refuse */
    if (resp && resp->type != YJS_OBJECT) resp = NULL;
    for (i = 0; bools[i]; i++)
        if (!strcmp(name, bools[i])) {
            /* the first three are true only on trials the rules make;
             * first_in_block, responded and correct can be either */
            if ((rc = lpush_level(lc, "false")) != 0) return rc;
            if ((i >= 3 || lc->rt[i]) && (rc = lpush_level(lc, "true")) != 0) return rc;
            return 1;
        }
    if (!strcmp(name, "outcome")) {
        const yjs_value* dur;
        double x = 0;
        if (!resp) return 0;
        if ((rc = lpush_level(lc, "correct")) != 0 || (rc = lpush_level(lc, "wrong")) != 0) return rc;
        /* a timeout needs a deadline; duration_s 0, or none, has none (2.6) */
        dur = yjs_get(resp, "duration_s");
        if (dur && !(dur->type == YJS_NUMBER && yjs_double(dur, &x) == YJS_OK && x == 0) && (rc = lpush_level(lc, "timeout")) != 0) return rc;
        return 1;
    }
    if (!strcmp(name, "response")) {
        /* the choice's key (2.3), which key_answer compares */
        const yjs_value* ch = resp ? yjs_get(resp, "choices") : NULL;
        size_t k;
        if (!ch || ch->type != YJS_ARRAY || !ch->n) return 0;
        for (k = 0; k < ch->n; k++) {
            const yjs_value* c = ch->u.items[k];
            if (c->type == YJS_OBJECT) c = yjs_get(c, "key");
            if (!c || c->type != YJS_STRING) return 0;
            if ((rc = lpush_level(lc, c->u.s)) != 0) return rc;
        }
        return 1;
    }
    if (!strcmp(name, "method")) {
        for (i = lc->depth; i >= 0; i--) {
            const yjs_value* tr = yjs_get(lc->chain[i], "tracks");
            size_t k;
            if (!tr) continue;
            if (tr->type != YJS_ARRAY || !tr->n) return 0;
            for (k = 0; k < tr->n; k++) {
                if (tr->u.items[k]->type != YJS_STRING) return 0;
                if ((rc = lpush_level(lc, tr->u.items[k]->u.s)) != 0) return rc;
            }
            return 1;
        }
        return 0;
    }
    if (is_builtin(name) != 0) return 0;
    for (i = lc->depth; i >= 0; i--) {
        const yjs_value* vars = yjs_get(lc->chain[i], "vars");
        const yjs_value* x = vars && vars->type == YJS_OBJECT ? yjs_get(vars, name) : NULL;
        if (x) return lvalue(lc, x, depth);
    }
    for (i = lc->depth; i >= 0; i--) {
        const yjs_value* tv = yjs_get(lc->chain[i], "timeline_variables");
        const yjs_value* nm = yjs_get(lc->chain[i], "name");
        ytb_table* t;
        int col, lv, nl;
        ytb_type ty;
        if (!tv || tv->type != YJS_STRING) continue;
        t = ltable(lc, tv->u.s, nm && nm->type == YJS_STRING ? nm->u.s : "");
        if (!t) return YPAK_ERR_FORMAT;
        col = ytb_col(t, name);
        if (col == -1) continue;
        ty = ytb_col_type(t, col);
        nl = ytb_n_levels(t, col);
        for (lv = 0; lv < nl; lv++) {
            const char* x = ytb_level_text(t, col, lv);
            if (!x) return lfail(lc, YPAK_ERR_FORMAT, "'%s': column '%s' has no level %d", tv->u.s, name, lv);
            if (ty != YTB_STRING) {
                /* a number's key is its canonical text, as a vars value's;
                 * an empty cell (NaN) is "" */
                char buf[YJS_DOUBLE_TEXT];
                double d = ytb_level_num(t, col, lv);
                int bn = d == d ? yjs_format_double(d, buf, sizeof buf) : 0;
                char* p;
                if (bn < 0) bn = 0;
                p = (char*)yjs_alloc(&lc->a, (size_t)bn + 1);
                if (!p) return lfail(lc, YPAK_ERR_NOMEM, "out of memory");
                memcpy(p, buf, (size_t)bn);
                p[bn] = 0;
                x = p;
            }
            if ((rc = lpush_level(lc, x)) != 0) return rc;
        }
        return 1;
    }
    return 0;   /* a session field, a jitter, a name the loader refuses */
}

/* The texts a value can give as a key: 1 known, 0 not, < 0 an error. */
static int lvalue(ypt__lc* lc, const yjs_value* v, int depth) {
    int rc;
    if ((rc = lstep(lc)) != 0) return rc;
    switch (v->type) {
    case YJS_STRING: {
        const char* sv = v->u.s;
        if (sv[0] == '$' && sv[1] == '$') return (rc = lpush_level(lc, sv + 1)) != 0 ? rc : 1;   /* "$$5" is "$5" */
        if (sv[0] == '$') return lvar(lc, sv + 1, depth + 1);
        return (rc = lpush_level(lc, sv)) != 0 ? rc : 1;
    }
    case YJS_TRUE: return (rc = lpush_level(lc, "true")) != 0 ? rc : 1;
    case YJS_FALSE: return (rc = lpush_level(lc, "false")) != 0 ? rc : 1;
    case YJS_NUMBER: {
        size_t n = yjs_write_mem(v, YJS_WRITE_COMPACT, NULL, 0);
        char* p = (char*)yjs_alloc(&lc->a, n + 1);
        if (!p) return lfail(lc, YPAK_ERR_NOMEM, "out of memory");
        yjs_write_mem(v, YJS_WRITE_COMPACT, p, n + 1);
        return (rc = lpush_level(lc, p)) != 0 ? rc : 1;
    }
    case YJS_OBJECT: {
        const yjs_value* cv = yjs_get(v, "choice");
        size_t k;
        int all = 1;
        if (cv && cv->type == YJS_ARRAY) {
            for (k = 0; k < cv->n; k++) {
                if ((rc = lvalue(lc, cv->u.items[k], depth)) < 0) return rc;
                all &= rc;
            }
            return all;
        }
        if (is_lookup(v)) {
            const yjs_value* mp = yjs_get(v, "map");
            for (k = 0; k < mp->n; k++) {
                if ((rc = lvalue(lc, mp->u.members[k].value, depth)) < 0) return rc;
                all &= rc;
            }
            return all;
        }
        return 0;
    }
    default: return 0;
    }
}

/* Sorts a list and drops repeats; the new count. */
static size_t lunique(const char** v, size_t n) {
    size_t i, k = 0;
    if (!n) return 0;
    qsort((void*)v, n, sizeof(char*), str_cmp);
    for (i = 1; i < n; i++)
        if (strcmp(v[i], v[k])) v[++k] = v[i];
    return k + 1;
}

/* "a", "b" and "c", at most 8, then how many more. */
static void llist(char* out, size_t cap, const char* const* v, size_t n) {
    size_t i, at = 0;
    out[0] = 0;
    for (i = 0; i < n && i < 8 && at < cap; i++) {
        int w = snprintf(out + at, cap - at, "%s\"%s\"", i == 0 ? "" : i + 1 == n ? " and " : ", ", v[i]);
        if (w < 0) break;
        at += (size_t)w;
    }
    if (n > 8 && at < cap) snprintf(out + at, cap - at, " and %u more", (unsigned)(n - 8));
}

/* by's text for a message: the variable, or the value in short. */
static void lby(const yjs_value* by, char* out, size_t cap) {
    size_t n;
    if (by->type == YJS_STRING) { snprintf(out, cap, "%s", by->u.s); return; }
    n = yjs_write_mem(by, YJS_WRITE_COMPACT, out, cap);
    if (n >= cap && cap > 4) memcpy(out + cap - 4, "...", 4);
}

/* One lookup in the current leaf: refuse a level without a key; keep the
 * levels for the reverse check. */
static int lcheck(ypt__lc* lc, const yjs_value* v) {
    ypt__lk key;
    ypt__lk* r;
    const yjs_value* map = yjs_get(v, "map");
    size_t i, nmiss = 0;
    int rc;
    key.v = v;
    r = (ypt__lk*)bsearch(&key, lc->lk, lc->nlk, sizeof(ypt__lk), lk_cmp);
    if (!r) return 0;   /* outside the parts the first pass reads */
    r->reached = 1;
    lc->ntmp = 0;
    rc = lvalue(lc, yjs_get(v, "by"), 0);
    if (rc < 0) return rc;
    if (rc == 0) { r->unknown = 1; return 0; }
    lc->ntmp = lunique(lc->tmp, lc->ntmp);
    for (i = 0; i < lc->ntmp; i++) {
        if ((rc = lstep(lc)) != 0) return rc;
        if (!yjs_get(map, lc->tmp[i])) lc->tmp[nmiss++] = lc->tmp[i];
        else if ((rc = lpush(lc, &r->lv, &r->nlv, &r->clv, lc->tmp[i])) != 0) return rc;
    }
    if (nmiss) {
        char list[400], by[80];
        llist(list, sizeof list, lc->tmp, nmiss);
        lby(yjs_get(v, "by"), by, sizeof by);
        return lfail(lc, YPAK_ERR_FORMAT, "%s: '%s': the lookup's map has no key for %s, which '%s' can take in node '%s'; a lookup needs a key "
                     "for every level of its 'by'", r->where, r->path, list, by, lc->node_path);
    }
    return 0;
}

static int lwalk(ypt__lc* lc, const yjs_value* v) {
    size_t k;
    int rc;
    if ((rc = lstep(lc)) != 0) return rc;
    if (is_lookup(v) && (rc = lcheck(lc, v)) != 0) return rc;
    if (v->type == YJS_OBJECT || v->type == YJS_ARRAY)
        for (k = 0; k < v->n; k++)
            if ((rc = lwalk(lc, v->type == YJS_OBJECT ? v->u.members[k].value : v->u.items[k])) != 0) return rc;
    return 0;
}

static int lwalk_names(ypt__lc* lc, const yjs_value* o) {
    size_t k;
    int rc;
    if (!o || o->type != YJS_OBJECT) return 0;
    for (k = 0; k < o->n; k++)
        if ((rc = lwalk(lc, o->u.members[k].value)) != 0) return rc;
    return 0;
}

/* The stimuli a template's steps name: on, off, set, to, then, and the
 * steps of an if or an unless. */
static int lnames(ypt__lc* lc, const yjs_value* steps) {
    static const char* const keys[] = { "on", "off", "set", "to", "then", NULL };
    size_t i, k;
    int j, rc;
    if (!steps || steps->type != YJS_ARRAY) return 0;
    for (i = 0; i < steps->n; i++) {
        const yjs_value* s = steps->u.items[i];
        if (s->type != YJS_OBJECT) continue;
        if ((rc = lstep(lc)) != 0) return rc;
        for (j = 0; keys[j]; j++) {
            const yjs_value* x = yjs_get(s, keys[j]);
            if (!x) continue;
            if (x->type == YJS_STRING && (rc = lpush(lc, &lc->nm, &lc->nnm, &lc->cnm, x->u.s)) != 0) return rc;
            if (x->type == YJS_ARRAY)
                for (k = 0; k < x->n; k++)
                    if (x->u.items[k]->type == YJS_STRING && (rc = lpush(lc, &lc->nm, &lc->nnm, &lc->cnm, x->u.items[k]->u.s)) != 0) return rc;
        }
        if ((rc = lnames(lc, yjs_get(s, "steps"))) != 0) return rc;
    }
    return 0;
}

/* One leaf: the vars and data of its chain, its template, and the stimuli
 * the template names (steps, the response's from, record). */
static int lleaf(ypt__lc* lc) {
    const yjs_value* t = lc->tmpl;
    const yjs_value* resp;
    const yjs_value* from;
    const yjs_value* rec;
    size_t k;
    int i, rc;
    for (i = 0; i <= lc->depth; i++)
        if ((rc = lwalk_names(lc, yjs_get(lc->chain[i], "vars"))) != 0 || (rc = lwalk_names(lc, yjs_get(lc->chain[i], "data"))) != 0) return rc;
    if (!t) return 0;
    for (k = 0; k < t->n; k++) {
        const yjs_value* x = t->u.members[k].value;
        if ((rc = !strcmp(t->u.members[k].key, "columns") ? lwalk_names(lc, x) : lwalk(lc, x)) != 0) return rc;
    }
    lc->nnm = 0;
    if ((rc = lnames(lc, yjs_get(t, "events"))) != 0 || (rc = lnames(lc, yjs_get(t, "after"))) != 0) return rc;
    resp = yjs_get(t, "response");
    from = resp && resp->type == YJS_OBJECT ? yjs_get(resp, "from") : NULL;
    rec = yjs_get(t, "record");
    if (from && from->type == YJS_STRING && (rc = lpush(lc, &lc->nm, &lc->nnm, &lc->cnm, from->u.s)) != 0) return rc;
    if (rec && rec->type == YJS_ARRAY)
        for (k = 0; k < rec->n; k++)
            if (rec->u.items[k]->type == YJS_STRING && (rc = lpush(lc, &lc->nm, &lc->nnm, &lc->cnm, rec->u.items[k]->u.s)) != 0) return rc;
    lc->nnm = lunique(lc->nm, lc->nnm);
    for (k = 0; k < lc->nnm; k++) {
        const yjs_value* s = lc->stimuli && lc->stimuli->type == YJS_OBJECT ? yjs_get(lc->stimuli, lc->nm[k]) : NULL;
        if (s && (rc = lwalk(lc, s)) != 0) return rc;
    }
    return 0;
}

/* Every leaf, in tree order, with its chain of nodes. */
static int lleaves(ypt__lc* lc, const yjs_value* arr, const char* parent) {
    size_t i;
    int rc;
    if (!arr || arr->type != YJS_ARRAY) return 0;
    for (i = 0; i < arr->n; i++) {
        const yjs_value* node = arr->u.items[i];
        const yjs_value* nmv;
        const yjs_value* tr;
        char* path;
        size_t n;
        if (node->type != YJS_OBJECT) continue;
        nmv = yjs_get(node, "name");
        n = strlen(parent) + (nmv && nmv->type == YJS_STRING ? strlen(nmv->u.s) : 0) + 2;
        path = (char*)yjs_alloc(&lc->a, n);
        if (!path) return lfail(lc, YPAK_ERR_NOMEM, "out of memory");
        snprintf(path, n, "%s%s%s", parent, parent[0] ? "/" : "", nmv && nmv->type == YJS_STRING ? nmv->u.s : "");
        lc->chain[++lc->depth] = node;   /* at most 65 deep: the first pass refused more */
        tr = yjs_get(node, "trial");
        if (tr && tr->type == YJS_STRING) {
            const yjs_value* t = lc->trials && lc->trials->type == YJS_OBJECT ? yjs_get(lc->trials, tr->u.s) : NULL;
            lc->tmpl = t && t->type == YJS_OBJECT ? t : NULL;
            lc->node_path = path;
            lrules(lc, node);
            if ((rc = lleaf(lc)) != 0) return rc;
        }
        if ((rc = lleaves(lc, yjs_get(node, "timeline"), path)) != 0) return rc;
        lc->depth--;
    }
    return 0;
}

int ypt_check_lookups(const void* def, size_t n, ypt_table_fn table, void* ctx, ypt_warn_fn warn, void* wctx, char* err, size_t cap) {
    ypt__lc lc;
    yjs_value* root;
    yjs_error je;
    const yjs_value* fmt;
    const yjs_value* ver;
    size_t i, k;
    int rc = 0;
    if (err && cap) err[0] = 0;
    if (!def || !table) return YPAK_ERR_ARG;
    memset(&lc, 0, sizeof lc);
    lc.err = err;
    lc.cap = cap;
    lc.table = table;
    lc.ctx = ctx;
    lc.depth = -1;
    yjs_arena_init_heap(&lc.a, 0);
    /* not a definition this tool reads: ypt_derive_texts() says why, or
     * the bytes are opaque */
    if (yjs_parse(&lc.a, (const char*)def, n, NULL, &root, &je) != YJS_OK) goto done;
    fmt = yjs_get(root, "format");
    ver = yjs_get(root, "version");
    if (!fmt || fmt->type != YJS_STRING || strcmp(fmt->u.s, "ysp-experiment") || !ver || !(ver->flags & YJS_NUM_I64) || 1 != ver->i) goto done;
    lc.stimuli = yjs_get(root, "stimuli");
    lc.trials = yjs_get(root, "trials");
    if (lc.stimuli && lc.stimuli->type == YJS_OBJECT)
        for (k = 0; k < lc.stimuli->n && !rc; k++) {
            char* w = lwhere(&lc, "stimulus", lc.stimuli->u.members[k].key);
            if (!w) { rc = lfail(&lc, YPAK_ERR_NOMEM, "out of memory"); break; }
            rc = lcollect(&lc, lc.stimuli->u.members[k].value, w, 0);
        }
    if (!rc && lc.trials && lc.trials->type == YJS_OBJECT)
        for (k = 0; k < lc.trials->n && !rc; k++) {
            char* w = lwhere(&lc, "template", lc.trials->u.members[k].key);
            if (!w) { rc = lfail(&lc, YPAK_ERR_NOMEM, "out of memory"); break; }
            rc = lcollect_template(&lc, lc.trials->u.members[k].value, w);
        }
    if (!rc) rc = lcollect_nodes(&lc, yjs_get(root, "timeline"), "", 0);
    if (rc || !lc.nlk) goto done;
    qsort(lc.lk, lc.nlk, sizeof(ypt__lk), lk_cmp);
    rc = lleaves(&lc, yjs_get(root, "timeline"), "");
    if (rc || !warn) goto done;
    /* keys no level reaches: harmless to the session, so a warning, not a
     * refusal; often a typo, whose real level the check above refused */
    qsort(lc.lk, lc.nlk, sizeof(ypt__lk), lk_seq_cmp);
    for (i = 0; i < lc.nlk; i++) {
        ypt__lk* r = &lc.lk[i];
        const yjs_value* keys = yjs_get(r->v, "map");
        size_t nx = 0;
        if (!r->reached || r->unknown) continue;
        r->nlv = lunique(r->lv, r->nlv);
        lc.ntmp = 0;
        for (k = 0; k < keys->n; k++) {
            const char* key = keys->u.members[k].key;
            if (r->nlv && bsearch(&key, (const void*)r->lv, r->nlv, sizeof(char*), str_cmp)) continue;
            if ((rc = lpush(&lc, &lc.tmp, &lc.ntmp, &lc.ctmp, key)) != 0) goto done;
            nx++;
        }
        if (nx) {
            char list[400], by[80], line[1200];
            llist(list, sizeof list, lc.tmp, nx);
            lby(yjs_get(r->v, "by"), by, sizeof by);
            snprintf(line, sizeof line, "%s: '%s': no level of '%s' reaches the map's key%s %s; a typo, or a level the trials no longer have?",
                     r->where, r->path, by, nx > 1 ? "s" : "", list);
            lflat(line);
            warn(wctx, line);
        }
    }
done:
    for (i = 0; i < lc.nlk; i++) free((void*)lc.lk[i].lv);
    free(lc.lk);
    free(lc.tab);
    free((void*)lc.tmp);
    free((void*)lc.nm);
    yjs_arena_free(&lc.a);
    return rc;
}
