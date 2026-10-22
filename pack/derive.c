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
