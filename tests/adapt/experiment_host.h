/* experiment_host.h - a host for ysp/experiment.h over named tables and a
 * list of pack entries, shared by tests/adapt/experiment_test.c and
 * tests/fuzz/experiment_fuzz.c so both reach the same refusals. */
#ifndef YEXP_TEST_HOST_H
#define YEXP_TEST_HOST_H

#include <stdio.h>
#include <string.h>

/* --- a host over named tables ------------------------------------------- */

typedef struct st_col { const char* name; int type; const char* levels[8]; } st_col;
typedef struct st_tab { const char* name; st_col cols[6]; } st_tab;

static const st_tab g_tabs[] = {
    { "conditions.pstb", { { "coherence", YEXP_COL_NUMBER, { "0.128", "0.512", NULL } },
                           { "coherent_direction", YEXP_COL_NUMBER, { "0", "180", NULL } },
                           { "key_answer", YEXP_COL_STRING, { "left", "right", NULL } }, { NULL, 0, { NULL } } } },
    { "stroop.pstb", { { "word", YEXP_COL_STRING, { "red", "green", "blue", NULL } },
                       { "ink", YEXP_COL_STRING, { "red", "green", "blue", NULL } },
                       { "congruent", YEXP_COL_INTEGER, { "0", "1", NULL } },
                       { "answer", YEXP_COL_STRING, { "r", "g", "b", NULL } },
                       { "weight", YEXP_COL_INTEGER, { "1", "2", NULL } }, { NULL, 0, { NULL } } } },
    { "oddball.pstb", { { "image", YEXP_COL_STRING, { "faces/f01.ysptex", "faces/f02.ysptex", NULL } },
                        { "type", YEXP_COL_STRING, { "standard", "target", NULL } },
                        { "code", YEXP_COL_INTEGER, { "1", "2", NULL } },
                        { "answer", YEXP_COL_STRING, { "left", "right", NULL } }, { NULL, 0, { NULL } } } },
    { "base.pstb", { { "cond", YEXP_COL_STRING, { "a", "b", NULL } },
                     { "n", YEXP_COL_INTEGER, { "1", "2", NULL } },
                     { "tex", YEXP_COL_STRING, { "tex/a.ysptex", "tex/missing.ysptex", NULL } },
                     { "tex_ok", YEXP_COL_STRING, { "tex/a.ysptex", NULL } },
                     { "flag", YEXP_COL_INTEGER, { "0", "1", NULL } }, { NULL, 0, { NULL } } } },
    { "builtin.pstb", { { "level", YEXP_COL_NUMBER, { "1", NULL } }, { NULL, 0, { NULL } } } },
};
#define N_TABS ((int)(sizeof g_tabs / sizeof g_tabs[0]))

static const st_tab* tab_get(const char* name) {
    int i;
    for (i = 0; i < N_TABS; i++)
        if (!strcmp(g_tabs[i].name, name)) return &g_tabs[i];
    return NULL;
}

static int st_entry(void* ctx, int kind, const char* name) {
    static const char* const ok[] = {
        "fonts/NotoSans-Regular.ttf", "tex/a.ysptex", "faces/f01.ysptex", "faces/f02.ysptex", "art/a.ysp", "snd/a.wav", "vid/a.mp4", NULL
    };
    int i;
    (void)ctx;
    if (kind == YEXP_ENTRY_TABLE) return tab_get(name) != NULL;
    for (i = 0; ok[i]; i++)
        if (!strcmp(ok[i], name)) return 1;
    return 0;
}

static int st_column(void* ctx, const char* table, int i, yexp_column* out) {
    const st_tab* t = tab_get(table);
    (void)ctx;
    if (!t) return -1;
    if (i >= 6 || !t->cols[i].name) return 0;
    out->name = t->cols[i].name;
    out->type = t->cols[i].type;
    {
        int k = 0;
        while (k < 8 && t->cols[i].levels[k]) k++;
        out->n_levels = k;
    }
    return 1;
}

static int st_level(void* ctx, const char* table, int col, int l, char* buf, size_t cap) {
    const st_tab* t = tab_get(table);
    const char* s;
    size_t n;
    (void)ctx;
    if (!t || col >= 6 || l >= 8 || !t->cols[col].levels[l]) return -1;
    s = t->cols[col].levels[l];
    n = strlen(s);
    if (n + 1 > cap) return (int)n;   /* the text's length, as yexp_host says */
    memcpy(buf, s, n + 1);
    return (int)n;
}

static int st_role(void* ctx, const yexp_role* r, char* msg, size_t cap) {
    (void)ctx;
    if (!strcmp(r->name, "not_on_rig")) { snprintf(msg, cap, "this rig has no such role"); return 1; }
    return 0;
}

static int st_param(void* ctx, int section, const char* kind, const yexp_prop* p, char* msg, size_t cap) {
    (void)ctx; (void)section; (void)kind;
    if (!strcmp(p->key, "w") && p->value->kind == YEXP_V_NUMBER && p->value->num < 0) {
        snprintf(msg, cap, "below the parameter table's minimum 0");
        return 1;
    }
    return 0;
}

static int g_rules_calls;
static int st_rules(void* ctx, const yexp_node* leaf, char* msg, size_t cap) {
    (void)ctx;
    g_rules_calls++;
    if (strstr(leaf->rules, "bad_rule")) { snprintf(msg, cap, "ysp_trials: rules line 1: an unknown statement"); return 1; }
    return 0;
}

static int g_ops_calls;
static int st_ops(void* ctx, const yexp_def* d, const yexp_template* t, char* msg, size_t cap) {
    (void)ctx; (void)d;
    g_ops_calls++;
    if (!strcmp(t->name, "ops_refused")) { snprintf(msg, cap, "ytl_check_ops: a wait past 2^62"); return 1; }
    return 0;
}

static const yexp_host g_host = { NULL, st_entry, st_column, st_level, st_role, st_param, st_rules, st_ops };

/* --- a host that misbehaves, by mode ------------------------------------ */

enum { BH_SANE, BH_COL_ENDLESS, BH_COL_ENDLESS_LATE, BH_COL_NULL_NAME, BH_COL_ODD_TYPE, BH_LVL_LONG, BH_LVL_LONG_CHANGES, BH_LVL_HUGE, BH_LVL_EMPTY,
       BH_LVL_NO_NUL, BH_LVL_MANY, BH_MSG_NO_NUL, BH_ENTRY_ODD, BH_MODES };
static int g_bh_mode;
static int g_bh_calls;

static int bh_column(void* ctx, const char* table, int i, yexp_column* o) {
    (void)ctx; (void)table;
    /* the corpus's own column names, so the fuzz target's seeds reach level() */
    static const char* const names[] = { "c", "cond", "word", "ink", "answer", "key_answer", "image", "tex_ok" };
    if (g_bh_mode == BH_COL_ENDLESS || (g_bh_mode == BH_COL_ENDLESS_LATE && g_bh_calls++ > 8)) { o->name = "zz"; o->type = 0; return 1; }
    if (g_bh_mode == BH_COL_NULL_NAME) { if (i == 0) { o->name = NULL; return 1; } return 0; }
    if (i < 8) { o->name = names[i]; o->type = g_bh_mode == BH_COL_ODD_TYPE ? 12345 : YEXP_COL_STRING; o->n_levels = 2; return 1; }
    return 0;
}

static int bh_level(void* ctx, const char* table, int col, int l, char* buf, size_t cap) {
    (void)ctx; (void)table; (void)col;
    switch (g_bh_mode) {
    case BH_LVL_LONG: if (l == 0) { memset(buf, 'a', cap); return 300; } return -1;
    case BH_LVL_LONG_CHANGES: if (l == 0) { memset(buf, 'a', cap); return 300 + (g_bh_calls++ & 1); } return -1;
    case BH_LVL_HUGE: return 1000000;
    case BH_LVL_EMPTY: if (l < 2) { memset(buf, 'q', cap); return 0; } return -1;
    case BH_LVL_NO_NUL: if (l == 0) { memset(buf, 'z', cap); return (int)cap - 1; } return -1;
    case BH_LVL_MANY: return l < 200000 ? snprintf(buf, cap, "L%d", l) : -1;
    default: if (l == 0) { snprintf(buf, cap, "a"); return 1; } if (l == 1) { snprintf(buf, cap, "b"); return 1; } return -1;
    }
}

static int bh_entry(void* ctx, int kind, const char* name) { (void)ctx; (void)kind; (void)name; return g_bh_mode == BH_ENTRY_ODD ? -5 : 1; }
static int bh_msg(char* m, size_t cap) { if (g_bh_mode == BH_MSG_NO_NUL) { memset(m, 'x', cap); return 1; } return 0; }
static int bh_role(void* ctx, const yexp_role* r, char* m, size_t cap) { (void)ctx; (void)r; return bh_msg(m, cap); }
static int bh_param(void* ctx, int s, const char* k, const yexp_prop* p, char* m, size_t cap) { (void)ctx; (void)s; (void)k; (void)p; return bh_msg(m, cap); }
static int bh_rules(void* ctx, const yexp_node* l, char* m, size_t cap) { (void)ctx; (void)l; return bh_msg(m, cap); }
static int bh_ops(void* ctx, const yexp_def* d, const yexp_template* t, char* m, size_t cap) { (void)ctx; (void)d; (void)t; return bh_msg(m, cap); }
static const yexp_host g_bad_host = { NULL, bh_entry, bh_column, bh_level, bh_role, bh_param, bh_rules, bh_ops };

#endif
