/* player/run.c - the tree, the trial and the frame.
 *
 * pl_frame() is the whole frame loop body. rig_spec 4.2.1 proposes a callback
 * form of the loop; that form replaces only pl_run()'s for-loop, because
 * everything between begin and flip already lives here.
 *
 * The order inside a frame is the order the C programs use, and the one
 * ysp/response.h documents: the input, yrsp_update_frame(), ytl_evaluate()
 * and the landing, then the draw.
 */
#include "player.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------- */

pl_stim* pl_stim_by_name(pl_player* p, const char* name) {
    int lo = 0, hi = p->n_stims - 1;
    while (lo <= hi) {
        int m = lo + (hi - lo) / 2;
        int c = strcmp(name, p->stims[m].def->name);
        if (c == 0) return &p->stims[m];
        if (c < 0) hi = m - 1;
        else lo = m + 1;
    }
    return NULL;
}

/* The worker's sink for --sim's trigger channels: the screen has fired the
 * code at its deadline; a count shows at the end that none was lost. */
void pl_trigger_sink(void* ctx, const yscr_trigger_info* info) {
    pl_player* p = (pl_player*)ctx;
    (void)info;
    if (info->flags & YSCR_TRIG_FLUSHED) return;
#if defined(__GNUC__)
    __atomic_add_fetch(&p->trigger_fired, 1, __ATOMIC_RELAXED);
#else
    p->trigger_fired++;
#endif
}

/* --- columns (2.10) ---------------------------------------------------- */

enum {
    CS_FIELD, CS_NODE, CS_TRIAL_TYPE, CS_TRIAL_INDEX, CS_ELAPSED,
    CS_T_INDEX, CS_BLOCK, CS_REP, CS_CONDITION, CS_TRACK, CS_PRACTICE, CS_WARMUP, CS_REQUEUED,
    CS_AFTER_BREAK, CS_FIRST_IN_BLOCK,
    CS_VAR, CS_SEED, CS_DATA, CS_METHOD, CS_LEVEL, CS_ESTIMATE,
    CS_STIM_ONSET, CS_STIM_OFFSET, CS_STIM_FRAMES, CS_STIM_RESIDUAL, CS_STIM_TIER, CS_STIM_DROPPED, CS_STIM_FLAGS,
    CS_RDK_SEED, CS_RDK_UPDATES, CS_RDK_DIGEST,
    CS_TIER_WORST, CS_FRAMES_DROPPED, CS_FLIP_FLAGS,
    CS_ROLE_N, CS_ROLE_FAILED, CS_ROLE_LAG,
    CS_RSP_RESPONSE, CS_CORRECT, CS_OUTCOME, CS_RSP_OTHER, CS_COLUMN
};

typedef struct pl_csrc { int source; int ref; } pl_csrc;

static int pl_col_add(pl_player* p, const char* name, int type, int source, int ref, int family) {
    int i;
    pl_col* c;
    if (strlen(name) > YTB_MAX_NAME) {
        pl_fail(p, "the data column '%s' is longer than %d bytes: shorten the stimulus, role or column name", name, YTB_MAX_NAME);
        return -1;
    }
    for (i = 0; i < p->n_cols; i++)
        if (strcmp(p->cols[i].name, name) == 0) {
            if (p->col_family[i] != family) {
                pl_fail(p, "the data column '%s' comes from two sources (a table, a template's columns, a built-in column, ...)", name);
                return -1;
            }
            /* one name, a jitter in one leaf and a text in another: the
             * column holds text, and a duration is its exact decimal */
            if (p->cols[i].type != type) p->cols[i].type = YTR_COL_STRING;
            return i;
        }
    if (p->n_cols >= YTR_DATA_MAX_COLS) {
        pl_fail(p, "the session's rows need more than %d columns", YTR_DATA_MAX_COLS);
        return -1;
    }
    c = &p->cols[p->n_cols];
    snprintf(c->name, sizeof c->name, "%s", name);
    c->type = type;
    p->csrc[p->n_cols].source = source;
    p->csrc[p->n_cols].ref = ref;
    p->col_family[p->n_cols] = family;
    return p->n_cols++;
}

enum { CF_TABLE = 1, CF_VAR, CF_BUILTIN, CF_COLUMN, CF_RSP };

/* A column for each table column, var and jitter that a leaf has, in the
 * order of the tree, and the template's columns after the response's. */
static int pl_columns_tree(pl_player* p, int pass) {
    int i;
    for (i = 0; i < p->n_leaves; i++) {
        const yexp_node* n;
        const pl_leaf* lf = &p->leaves[i];
        /* 2.10 item 3's order: tables, vars, jitters; trial_seed and the
         * data come after (pass 1) */
        if (pass == 1) {
            for (n = lf->node; n; n = n->parent) {
                int k;
                for (k = 0; k < n->n_data; k++)
                    if (pl_col_add(p, n->data[k].key, YTR_COL_STRING, CS_VAR, 0, CF_VAR) < 0) return -1;
            }
            continue;
        }
        /* a leaf's table, then the containers' tables (2.10 item 3) */
        for (n = lf->node; n; n = n->parent) {
            if (n->table) {
                const ytb_table* t = pl_table_get(p, n->table);
                int c;
                if (!t) return -1;
                for (c = 0; c < t->n_cols; c++)
                    if (pl_col_add(p, ytb_col_name(t, c), YTR_COL_STRING, CS_VAR, c, CF_TABLE) < 0) return -1;
            }
        }
        for (n = lf->node; n; n = n->parent) {
            int k;
            for (k = 0; k < n->n_vars; k++)
                if (pl_col_add(p, n->vars[k].key, YTR_COL_STRING, CS_VAR, 0, CF_VAR) < 0) return -1;
            for (k = 0; k < n->n_rule_vars; k++)
                if (pl_col_add(p, n->rule_vars[k].name, YTR_COL_STRING, CS_VAR, 0, CF_VAR) < 0) return -1;
        }
        for (n = lf->node; n; n = n->parent) {
            int k;
            for (k = 0; k < n->n_jitters; k++)
                if (pl_col_add(p, n->jitters[k], YTR_COL_TIME_NS, CS_VAR, 0, CF_VAR) < 0) return -1;
        }
    }
    return 0;
}

static int pl_columns_stim(pl_player* p) {
    int i;
    char name[128];
    /* <s>_* for each recorded stimulus, in the order first recorded */
    for (i = 0; i < p->n_tmpls; i++) {
        int k;
        for (k = 0; k < p->tmpls[i].n_record; k++) {
            const char* s = p->tmpls[i].def->record[k];
            static const struct { const char* suf; int type; int src; } R[] = {
                { "_onset_s", YTR_COL_TIME_NS, CS_STIM_ONSET }, { "_offset_s", YTR_COL_TIME_NS, CS_STIM_OFFSET },
                { "_frames", YTR_COL_INT, CS_STIM_FRAMES }, { "_residual_s", YTR_COL_TIME_NS, CS_STIM_RESIDUAL },
                { "_tier", YTR_COL_INT, CS_STIM_TIER }, { "_dropped", YTR_COL_INT, CS_STIM_DROPPED },
                { "_flags", YTR_COL_INT, CS_STIM_FLAGS },
            };
            size_t j;
            for (j = 0; j < sizeof R / sizeof R[0]; j++) {
                snprintf(name, sizeof name, "%s%s", s, R[j].suf);
                if (strlen(name) > YTB_MAX_NAME) { pl_fail(p, "the column '%s' is longer than %d bytes", name, YTB_MAX_NAME); return -1; }
                if (pl_col_add(p, name, R[j].type, R[j].src, p->tmpls[i].record_stims[k], CF_BUILTIN) < 0) return -1;
            }
        }
    }
    /* <s>_seed, _updates, _digest: every rdk */
    for (i = 0; i < p->n_stims; i++)
        if (p->stims[i].def->kind == YEXP_RDK) {
            const char* s = p->stims[i].def->name;
            snprintf(name, sizeof name, "%s_seed", s);
            if (pl_col_add(p, name, YTR_COL_STRING, CS_RDK_SEED, i, CF_BUILTIN) < 0) return -1;
            snprintf(name, sizeof name, "%s_updates", s);
            if (pl_col_add(p, name, YTR_COL_INT, CS_RDK_UPDATES, i, CF_BUILTIN) < 0) return -1;
            snprintf(name, sizeof name, "%s_digest", s);
            if (pl_col_add(p, name, YTR_COL_STRING, CS_RDK_DIGEST, i, CF_BUILTIN) < 0) return -1;
        }
    return 0;
}

static yjs_value* pl_header_versions(pl_player* p, yjs_arena* a) {
    yjs_value* o = yjs_new(a, YJS_OBJECT);
    (void)p;
    yjs_set(a, o, "experiment", yjs_new_stringz(a, yexp_version()));
    yjs_set(a, o, "trials", yjs_new_stringz(a, ytr_version()));
    yjs_set(a, o, "timeline", yjs_new_stringz(a, ytl_version()));
    yjs_set(a, o, "response", yjs_new_stringz(a, yrsp_version()));
    yjs_set(a, o, "rdk", yjs_new_stringz(a, yrdk_version()));
    yjs_set(a, o, "dataset", yjs_new_stringz(a, yds_version()));
    yjs_set(a, o, "screen", yjs_new_stringz(a, yscr_version()));
    yjs_set(a, o, "gfx", yjs_new_stringz(a, ygfx_version()));
    yjs_set(a, o, "color", yjs_new_stringz(a, ycol_version()));
    yjs_set(a, o, "table", yjs_new_stringz(a, ytb_version()));
    yjs_set(a, o, "json", yjs_new_stringz(a, yjs_version()));
    yjs_set(a, o, "pack", yjs_new_stringz(a, YPAK_VERSION_STRING));
    yjs_set(a, o, "rigfile", yjs_new_stringz(a, yrig_version()));
    yjs_set(a, o, "stair", yjs_new_stringz(a, yst_version()));
    yjs_set(a, o, "quest", yjs_new_stringz(a, yqst_version()));
    yjs_set(a, o, "aep", yjs_new_stringz(a, yaep_version()));
    yjs_set(a, o, "audio", yjs_new_stringz(a, yau_version()));
    return o;
}

static int pl_dataset_open(pl_player* p) {
    yds_desc d;
    yjs_arena a;
    yjs_value* s, *fields;
    char line[1024], seedbuf[32];
    int i;
    memset(&d, 0, sizeof d);
    yjs_arena_init_heap(&a, 0);
    s = yjs_new(&a, YJS_OBJECT);
    fields = yjs_new(&a, YJS_OBJECT);
    yjs_set(&a, s, "experiment_id", yjs_new_stringz(&a, p->experiment_id));
    yjs_set(&a, s, "experiment_id_stored", yjs_new_stringz(&a, p->stored_id));
    yjs_set(&a, s, "pack_id", yjs_new_stringz(&a, p->pack_id));
    yjs_set(&a, s, "rig_profile_sha256", yjs_new_stringz(&a, p->rig_hash));
    snprintf(seedbuf, sizeof seedbuf, "%llu", (unsigned long long)p->seed);
    yjs_set(&a, s, "seed", yjs_new_stringz(&a, seedbuf));
    yjs_set(&a, s, "player_version", yjs_new_stringz(&a, PL_VERSION_STRING));
    yjs_set(&a, s, "format_version", yjs_new_int(&a, YEXP_FORMAT_VERSION));
    yjs_set(&a, s, "headers", pl_header_versions(p, &a));
    for (i = 0; i < p->def->n_fields; i++) {
        char v[256];
        pl_val_text(&p->field_values[i], v, sizeof v);
        yjs_set(&a, fields, p->def->fields[i].name, yjs_new_stringz(&a, v));
    }
    yjs_set(&a, s, "session_fields", fields);
    yscr_describe(&p->scr, line, sizeof line);
    yjs_set(&a, s, "screen", yjs_new_stringz(&a, line));
    if (p->have_cal) {
        ycol_ctx_describe(&p->cx, line, sizeof line);
        yjs_set(&a, s, "color", yjs_new_stringz(&a, line));
    }
    if (p->audio_open) {
        yau_describe(&p->audio, line, sizeof line);
        yjs_set(&a, s, "audio", yjs_new_stringz(&a, line));
    }
    /* 2.10's device rows: in --sim each role is the scripted participant or
     * the in-process sink; there is no device, port or serial number */
    {
        yjs_value* roles = yjs_new(&a, YJS_ARRAY);
        for (i = 0; i < p->def->n_roles; i++) {
            yjs_value* o = yjs_new(&a, YJS_OBJECT);
            const yrig_role* rr = p->have_rig ? yrig_find(p->rig, p->def->roles[i].name) : NULL;
            yjs_set(&a, o, "role", yjs_new_stringz(&a, p->def->roles[i].name));
            yjs_set(&a, o, "direction", yjs_new_stringz(&a, p->def->roles[i].direction == 0 ? "in" : p->def->roles[i].direction == 1 ? "out" : "both"));
            yjs_set(&a, o, "bound", yjs_new_stringz(&a, rr && rr->family[0] ? rr->family : "sim"));
            yjs_push(&a, roles, o);
        }
        yjs_set(&a, s, "devices", roles);
    }
    /* a sim's start date is fixed so that two runs are byte-identical */
    if (p->args.sim) yjs_set(&a, s, "start_utc", yjs_new_stringz(&a, "1970-01-01T00:00:00Z"));
    d.dir = p->args.out_dir;
    d.session = s;
    d.trial_cols = p->ytr_cols;
    d.n_trial_cols = p->n_cols;
    if (!yds_open(&p->ds, &d)) { pl_fail(p, "the data set '%s': %s", p->args.out_dir, yds_error(&p->ds)); yjs_arena_free(&a); return -1; }
    yjs_arena_free(&a);
    p->ds_open = true;
    /* a stream per rdk (2.7) */
    {
        static const ytr_col rdk_cols[] = {
            YTR_COL(pl_rdk_step_row, trial_index, YTR_COL_INT), YTR_COL(pl_rdk_step_row, update, YTR_COL_INT),
            YTR_COL_D(pl_rdk_step_row, t_ns, YTR_COL_TIME_NS, 9),
            YTR_COL_D(pl_rdk_step_row, coherence, YTR_COL_DOUBLE, 0), YTR_COL_D(pl_rdk_step_row, direction, YTR_COL_DOUBLE, 0),
            YTR_COL_D(pl_rdk_step_row, speed, YTR_COL_DOUBLE, 0),
        };
        for (i = 0; i < p->n_stims; i++)
            if (p->stims[i].def->kind == YEXP_RDK) {
                int sid = yds_stream(&p->ds, p->stims[i].def->name, rdk_cols, (int)(sizeof rdk_cols / sizeof rdk_cols[0]));
                if (sid < 0) { pl_fail(p, "the stream '%s': %s", p->stims[i].def->name, yds_error(&p->ds)); return -1; }
                p->stims[i].stream = sid;
            }
    }
    return 0;
}

/* 2.10's order; a column every leaf has is one column, and a leaf that lacks
 * one leaves it empty. */
int pl_columns_build(pl_player* p) {
    int i;
    static const ytr_col rsp[] = { YRSP_DATA_COLS };
    p->cols = (pl_col*)pl_alloc(p, sizeof(pl_col) * YTR_DATA_MAX_COLS);
    p->csrc = (pl_csrc*)pl_alloc(p, sizeof(pl_csrc) * YTR_DATA_MAX_COLS);
    p->col_family = (int*)pl_alloc(p, sizeof(int) * YTR_DATA_MAX_COLS);
    p->ytr_cols = (ytr_col*)pl_alloc(p, sizeof(ytr_col) * YTR_DATA_MAX_COLS);
    if (!p->cols || !p->csrc || !p->col_family || !p->ytr_cols) return -1;
    for (i = 0; i < p->def->n_fields; i++)
        if (pl_col_add(p, p->def->fields[i].name, YTR_COL_STRING, CS_FIELD, i, CF_BUILTIN) < 0) return -1;
    {
        static const struct { const char* n; int type; int src; } H[] = {
            { "node", YTR_COL_STRING, CS_NODE }, { "trial_type", YTR_COL_STRING, CS_TRIAL_TYPE },
            { "trial_index", YTR_COL_INT, CS_TRIAL_INDEX }, { "time_elapsed_s", YTR_COL_TIME_NS, CS_ELAPSED },
            { "index", YTR_COL_INT, CS_T_INDEX }, { "block", YTR_COL_INT, CS_BLOCK }, { "rep", YTR_COL_INT, CS_REP },
            { "condition", YTR_COL_INT, CS_CONDITION }, { "track", YTR_COL_INT, CS_TRACK },
            { "practice", YTR_COL_BOOL, CS_PRACTICE }, { "warmup", YTR_COL_BOOL, CS_WARMUP },
            { "requeued", YTR_COL_BOOL, CS_REQUEUED }, { "after_break", YTR_COL_BOOL, CS_AFTER_BREAK },
            { "first_in_block", YTR_COL_BOOL, CS_FIRST_IN_BLOCK },
        };
        size_t j;
        for (j = 0; j < sizeof H / sizeof H[0]; j++)
            if (pl_col_add(p, H[j].n, H[j].type, H[j].src, 0, CF_BUILTIN) < 0) return -1;
    }
    if (pl_columns_tree(p, 0) < 0) return -1;
    if (pl_col_add(p, "trial_seed", YTR_COL_STRING, CS_SEED, 0, CF_BUILTIN) < 0) return -1;
    if (pl_columns_tree(p, 1) < 0) return -1;
    if (pl_col_add(p, "method", YTR_COL_STRING, CS_METHOD, 0, CF_BUILTIN) < 0) return -1;
    if (pl_col_add(p, "level", YTR_COL_DOUBLE, CS_LEVEL, 0, CF_BUILTIN) < 0) return -1;
    if (pl_col_add(p, "estimate", YTR_COL_DOUBLE, CS_ESTIMATE, 0, CF_BUILTIN) < 0) return -1;
    if (pl_columns_stim(p) < 0) return -1;
    if (pl_col_add(p, "tier_worst", YTR_COL_INT, CS_TIER_WORST, 0, CF_BUILTIN) < 0) return -1;
    if (pl_col_add(p, "frames_dropped", YTR_COL_INT, CS_FRAMES_DROPPED, 0, CF_BUILTIN) < 0) return -1;
    if (pl_col_add(p, "flip_flags", YTR_COL_INT, CS_FLIP_FLAGS, 0, CF_BUILTIN) < 0) return -1;
    for (i = 0; i < p->n_triggers; i++) {
        char name[128];
        const char* r = p->trigger_roles[i]->name;
        snprintf(name, sizeof name, "%s_n", r);
        if (pl_col_add(p, name, YTR_COL_INT, CS_ROLE_N, i, CF_BUILTIN) < 0) return -1;
        snprintf(name, sizeof name, "%s_failed", r);
        if (pl_col_add(p, name, YTR_COL_INT, CS_ROLE_FAILED, i, CF_BUILTIN) < 0) return -1;
        snprintf(name, sizeof name, "%s_lag_max_s", r);
        if (pl_col_add(p, name, YTR_COL_TIME_NS, CS_ROLE_LAG, i, CF_BUILTIN) < 0) return -1;
    }
    for (i = 0; i < (int)(sizeof rsp / sizeof rsp[0]); i++)
        if (pl_col_add(p, rsp[i].name, rsp[i].type, CS_RSP_OTHER, i, CF_RSP) < 0) return -1;
    if (pl_col_add(p, "correct", YTR_COL_BOOL, CS_CORRECT, 0, CF_RSP) < 0) return -1;
    if (pl_col_add(p, "outcome", YTR_COL_STRING, CS_OUTCOME, 0, CF_RSP) < 0) return -1;
    for (i = 0; i < p->n_tmpls; i++) {
        int k;
        const yexp_template* t = p->tmpls[i].def;
        for (k = 0; k < t->n_columns; k++)
            if (pl_col_add(p, t->columns[k].key, YTR_COL_STRING, CS_COLUMN, 0, CF_COLUMN) < 0) return -1;
    }
    for (i = 0; i < p->n_cols; i++) {
        memset(&p->ytr_cols[i], 0, sizeof p->ytr_cols[i]);
        p->ytr_cols[i].name = p->cols[i].name;
        p->ytr_cols[i].type = (ytr_col_type)p->cols[i].type;
        p->ytr_cols[i].offset = YTR_UNBOUND;
    }
    return pl_dataset_open(p);
}

/* --- the participant --------------------------------------------------- */

int pl_script_load(pl_player* p, const char* path) {
    FILE* fp = fopen(path, "r");
    char line[512];
    int cap = 0;
    if (!fp) { pl_fail(p, "the script '%s' cannot be read", path); return -1; }
    if (!fgets(line, sizeof line, fp)) { fclose(fp); return 0; }   /* the header */
    while (fgets(line, sizeof line, fp)) {
        char resp[128] = "";
        int trial = -1;
        double rt = 0.3, hold = 0.08;
        int n;
        char* c1, *c2, *c3;
        size_t L = strcspn(line, "\r\n");
        line[L] = 0;
        c1 = strchr(line, ',');
        if (!c1) continue;
        *c1 = 0;
        trial = atoi(line);
        c2 = strchr(c1 + 1, ',');
        if (c2) *c2 = 0;
        snprintf(resp, sizeof resp, "%s", c1 + 1);
        if (c2) {
            c3 = strchr(c2 + 1, ',');
            if (c3) { *c3 = 0; hold = atof(c3 + 1); }
            rt = atof(c2 + 1);
        }
        (void)n;
        if (p->n_script == cap) {
            int nc = cap ? cap * 2 : 256;
            pl_script_row* q = (pl_script_row*)pl_alloc(p, sizeof(pl_script_row) * (size_t)nc);
            if (!q) { fclose(fp); return -1; }
            if (p->n_script) memcpy(q, p->script, sizeof(pl_script_row) * (size_t)p->n_script);
            p->script = q;
            cap = nc;
        }
        p->script[p->n_script].trial = trial;
        p->script[p->n_script].silent = !resp[0];
        p->script[p->n_script].rt_s = rt;
        p->script[p->n_script].hold_s = hold;
        p->script[p->n_script].resp = resp[0] ? (char*)pl_alloc(p, strlen(resp) + 1) : NULL;
        if (resp[0]) strcpy(p->script[p->n_script].resp, resp);
        p->n_script++;
    }
    fclose(fp);
    return 0;
}

volatile int pl_loop_active;

static double pl_participant_rand(pl_player* p) { return ytr_splitmix(&p->participant_rng); }

/* Decide this trial's press. Called once the response is armed. */
static void pl_participant_plan(pl_player* p) {
    pl_tmpl* t = p->tmpl;
    int answer = -1, i;
    p->press_planned = false;
    p->press_pending = false;
    p->silent = true;
    if (!t->collector) return;
    if (p->env.key_answer_known) {
        for (i = 0; i < t->n_choices; i++)
            if (strcmp(t->choice_keys[i], p->key_answer) == 0) answer = i;
    }
    p->press_rt = p->args.rt_s > 0 ? p->args.rt_s : 0.3;
    p->press_hold = 0.08;
    if (p->script) {
        int k = -1;
        if (p->global_trial < p->n_script && p->script[p->global_trial].trial == p->global_trial) k = p->global_trial;
        else for (i = 0; i < p->n_script; i++) if (p->script[i].trial == p->global_trial) k = i;
        if (k < 0) return;   /* not in the script: silent */
        if (p->script[k].silent) return;
        p->press_choice = -1;
        for (i = 0; i < t->n_choices; i++)
            if (strcmp(t->choice_keys[i], p->script[k].resp) == 0 ||
                strcmp(t->collector->ch[i].name ? t->collector->ch[i].name : "", p->script[k].resp) == 0)
                p->press_choice = i;
        if (p->press_choice < 0) { pl_fail(p, "trial %d: the script's response '%s' is not a choice", p->global_trial, p->script[k].resp); return; }
        p->press_rt = p->script[k].rt_s;
        p->press_hold = p->script[k].hold_s;
        p->silent = false;
        return;
    }
    {
        const char* m = p->args.participant ? p->args.participant : "correct";
        if (strcmp(m, "silent") == 0) return;
        if (t->n_choices == 0) { p->press_choice = -1; p->silent = false; return; }
        if (answer < 0) answer = 0;
        p->press_choice = answer;
        if (m[0] == 'p' && m[1] == '=') {
            double correct_p = atof(m + 2);
            if (pl_participant_rand(p) >= correct_p) p->press_choice = (answer + 1) % t->n_choices;
        }
        p->silent = false;
    }
}

static void pl_participant_arm(pl_player* p, int64_t onset) {
    if (p->silent || p->press_planned) return;
    p->press_at = onset + (int64_t)llround(p->press_rt * 1e9);
    p->release_at = p->press_at + (int64_t)llround(p->press_hold * 1e9);
    p->press_planned = true;
    p->press_pending = true;
}

/* --- trials ------------------------------------------------------------ */

static void pl_env_trial(pl_player* p) {
    pl_env* e = &p->env;
    memset(e, 0, sizeof *e);
    e->node = p->leaf->node;
    e->t = p->tmpl->def;
    e->trials = &p->trials;
    e->ti = &p->ti;
    e->phase = PL_PH_TRIAL;
    e->rt_ns = YRSP_NO_TIME;
    if (p->track) e->method = p->track->def->name;
}

static double pl_to_stim_units(const pl_method* m, double x) { return m->def->log10 ? pow(10.0, x) : x; }

static int pl_stim_begin_trial(pl_player* p, const yscr_frame* f) {
    int i;
    /* The michelson stimulus first: any other stimulus's key may read
     * $contrast_shown, and stimuli are sorted by name. Its direction keys
     * come before the contrast that depends on them. */
    for (i = 0; i < p->n_stims; i++) {
        pl_stim* s = &p->stims[i];
        ycol_shown sh;
        double m;
        ycol_rgb dir;
        int k;
        if (s->def->kind == YEXP_RDK) s->rdk_moving = false;   /* a template that does not use it must not see the last one's */
        if (!p->tmpl->stim_used[i] || !s->michelson) continue;
        for (k = 0; k < s->def->n_props; k++)
            if (strncmp(s->def->props[k].key, "dir_", 4) == 0 && pl_apply_prop(p, s, &s->def->props[k]) < 0) return -1;
        dir.r = s->gs.dir[0];
        dir.g = s->gs.dir[1];
        dir.b = s->gs.dir[2];
        if (!p->have_cal) { pl_fail(p, "stimulus '%s': a michelson contrast needs a calibration", s->def->name); return -1; }
        if (pl_eval_num(p, &p->env, s->michelson, &m) < 0) return -1;
        if (ycol_michelson(&p->cx, m, dir, 8, &sh) < 0) { pl_fail(p, "stimulus '%s': %s", s->def->name, sh.why ? sh.why : "michelson"); return -1; }
        s->gs.contrast = (float)sh.scene;
        /* the loader allows one michelson stimulus shown per template, so
         * one $contrast_shown per trial */
        p->env.contrast_shown = sh.shown;
        p->env.have_shown = true;
    }
    for (i = 0; i < p->n_stims; i++) {
        pl_stim* s = &p->stims[i];
        if (!p->tmpl->stim_used[i]) continue;
        if (pl_apply_writes(p, s, PL_KEY_TRIAL) < 0) return -1;
        if (s->def->kind == YEXP_TEXT && !pl_value_after(s->text_value)) {
            if (pl_stim_set_text(p, s) < 0) return -1;
        } else if (s->def->kind == YEXP_TEXT) {
            s->n_runs = 0;   /* no text until `after` is built */
        }
        if (s->def->kind == YEXP_IMAGE) {
            const yexp_prop* tx = yexp_prop_get(s->def->props, s->def->n_props, "texture");
            pl_val v;
            int k;
            if (pl_eval(p, &p->env, tx->value, &v) < 0) return -1;
            if (v.kind != YEXP_V_STRING) { pl_fail(p, "stimulus '%s': 'texture' is not an entry name", s->def->name); return -1; }
            for (k = 0; k < p->n_tex; k++)
                if (strcmp(p->tex_names[k], v.str) == 0) { s->gs.tex = p->tex[k]; break; }
            if (k == p->n_tex) { pl_fail(p, "stimulus '%s': the texture '%s' was not preloaded", s->def->name, v.str); return -1; }
        }
        if (s->def->kind == YEXP_RDK) {
            s->n_steps = 0;
            s->rdk_seed = ytr_trial_seed(&p->trials, p->ti.index);
        }
        if (s->def->kind == YEXP_SOUND) {
            s->sound_scheduled = -1;
            s->sound_id = 0;
            s->sound_first = -1;
        }
    }
    (void)f;
    return 0;
}

/* The initial value of each channel: 0 for a visibility, the stimulus's
 * value for a property, so each trial starts from its own values and the
 * previous trial's last tween cannot leak (ytl_open() resets the handle). */
static int pl_timeline_reset(pl_player* p) {
    int i;
    /* a literal key's value is read from the load's copy: the field itself
     * holds whatever the last trial's channel wrote */
    for (i = 0; i < p->n_channels; i++)
        p->tl_initial[i] = !p->chan_src[i] ? 0.0f : p->chan_dyn[i] ? *p->chan_src[i] : p->chan_base[i];
    if (!ytl_open(&p->tl, &p->tl_desc)) { pl_fail(p, "the timeline: %s", ytl_error(&p->tl)); return -1; }
    return 0;
}

static int pl_build_after(pl_player* p, int64_t start) {
    const yexp_template* d = p->tmpl->def;
    ytl_op* ops = p->tmpl->ops;
    int cap = p->tmpl->ops_cap, n = 0;
    int64_t cursor = start;
    int r, i;
    for (i = 0; i < p->n_stims; i++) {
        pl_stim* s = &p->stims[i];
        if (!p->tmpl->stim_used[i]) continue;
        if (pl_apply_writes(p, s, PL_KEY_AFTER) < 0) return -1;
        if (s->def->kind == YEXP_TEXT && pl_value_after(s->text_value) && pl_stim_set_text(p, s) < 0) return -1;
    }
    memset(&ops[0], 0, sizeof ops[0]);
    ops[0].op = YTL_OP_AT;
    ops[0].t_ns = start;
    n = 1;
    p->ops_latest = start;
    r = pl_build_ops(p, d->after, d->n_after, ops + n, cap - n - 2, &cursor);
    if (r < 0) return -1;
    n += r;
    /* a backward at_s leaves the cursor before steps already scheduled: the
     * end goes at the latest of them, so none is dropped */
    memset(&ops[n], 0, sizeof ops[n]);
    ops[n].op = YTL_OP_AT;
    ops[n].t_ns = p->ops_latest;
    n++;
    memset(&ops[n], 0, sizeof ops[n]);
    ops[n].op = YTL_OP_MARK;
    ops[n].code = PL_MARK_END;
    n++;
    p->q = ytl_seq_on(&p->tl, PL_BASE_TRIAL);
    r = ytl_run(&p->q, ops, n);
    if (r != 0 || p->q.err) { pl_fail(p, "template '%s': the 'after' table: %s (op %d)", d->name, ytl_strerror(r ? r : p->q.err), p->q.err_call); return -1; }
    return 0;
}

/* One leaf finished or could not start: push its iteration's loop rule. */
static void pl_leaf_close(pl_player* p) {
    if (p->trials_open) {
        ytr_close(&p->trials);
        p->trials_open = false;
    }
    p->leaf = NULL;
}

static int pl_stack_iters(pl_player* p, int* iters, int cap) {
    int i, n = 0;
    for (i = 1; i < p->sp && n < cap; i++) iters[n++] = p->stack[i].iteration;
    return n;
}

static int pl_leaf_open(pl_player* p, pl_leaf* lf, int iter) {
    int iters[PL_MAX_DEPTH_STACK];
    int n = pl_stack_iters(p, iters, PL_MAX_DEPTH_STACK - 1);
    uint64_t seed;
    iters[n++] = iter;
    seed = pl_node_seed(p->seed, lf->node->path, iters, n);
    /* compare.py and the operator use these; the session's seed is enough to
     * rederive them, but the seed row shows them without the hash */
    if (p->n_node_seeds >= PL_MAX_LEAF_RUNS) {
        pl_fail(p, "node '%s': the session has run %d leaves; the data set cannot record more seeds", lf->node->path, PL_MAX_LEAF_RUNS);
        return -1;
    }
    p->node_seeds[p->n_node_seeds].path = lf->node->path;
    p->node_seeds[p->n_node_seeds].iteration = iter;
    p->node_seeds[p->n_node_seeds].seed = seed;
    p->n_node_seeds++;
    p->leaf_rng = seed;
    lf->desc.seed = seed;
    lf->desc.rng = ytr_splitmix;
    lf->desc.rng_ctx = &p->leaf_rng;
    if (!ytr_open(&p->trials, &lf->desc)) { pl_fail(p, "node '%s': %s", lf->node->path, ytr_error(&p->trials)); return -1; }
    p->trials_open = true;
    p->leaf = lf;
    p->leaf_iter = iter;
    p->leaf_loop_pass = 0;
    p->leaf_n_correct = p->leaf_n_counted = 0;
    return 0;
}

/* Walk the tree to the next leaf that has a trial to give. */
int pl_walk_next_leaf(pl_player* p) {
    for (;;) {
        pl_stack* top;
        if (p->sp == 0) { p->walk_done = true; return 0; }
        /* a tree that loops over nothing: spread over frames, so a frame keeps
         * its time and the session's budget can stop it; a named error when
         * no trial ever comes */
        if (++p->walk_steps > PL_WALK_MAX) {
            pl_fail(p, "the tree ran %d node steps with no trial: a loop or repetition over nothing", PL_WALK_MAX);
            return -1;
        }
        if (++p->walk_frame_steps > PL_WALK_PER_FRAME) return 2;
        top = &p->stack[p->sp - 1];
        if (top->child >= top->n_children) {
            top->row++;
            if (top->row >= top->n_rows) { top->row = 0; top->rep++; }
            if (top->rep < top->n_reps) { top->child = 0; continue; }
            /* the iteration is complete: loop? */
            {
                const yexp_node* n = top->node;
                if (n && n->has_loop) {
                    double acc = top->n_counted > 0 ? (double)top->n_correct / top->n_counted : 0.0;
                    top->loop_pass++;
                    if (acc < n->loop_until && top->loop_pass < n->loop_max) {
                        top->rep = top->row = top->child = 0;
                        top->n_correct = top->n_counted = 0;
                        continue;
                    }
                }
            }
            p->sp--;
            continue;
        }
        {
            const yexp_node* n = &top->children[top->child++];
            int iter = top->child_iters[top->child - 1]++;
            /* if / unless read when the node starts (2.4) */
            {
                pl_env env;
                bool cond = true;
                memset(&env, 0, sizeof env);
                env.node = top->node;
                env.phase = PL_PH_SESSION;
                if (n->if_var) {
                    if (pl_eval_var_bool(p, &env, n->if_var, &cond) < 0) return -1;
                    if (!cond) continue;
                }
                if (n->unless_var) {
                    if (pl_eval_var_bool(p, &env, n->unless_var, &cond) < 0) return -1;
                    if (cond) continue;
                }
            }
            if (n->trial) {
                pl_leaf* lf = pl_leaf_of(p, n);
                if (!lf) { pl_fail(p, "node '%s': no leaf", n->path); return -1; }
                if (pl_leaf_open(p, lf, iter) < 0) return -1;
                p->walk_frame_steps += 64;   /* a ytr_open() is heavier than a node step */
                return 1;
            }
            if (p->sp >= PL_MAX_DEPTH_STACK) { pl_fail(p, "nodes nest more than %d levels", PL_MAX_DEPTH_STACK); return -1; }
            {
                pl_stack* s = &p->stack[p->sp++];
                memset(s, 0, sizeof *s);
                s->node = n;
                s->children = n->children;
                s->n_children = n->n_children;
                s->n_reps = n->repetitions > 0 ? n->repetitions : 1;
                s->iteration = iter;
                s->table = n->table ? pl_table_get(p, n->table) : NULL;
                s->n_rows = s->table ? (s->table->n_rows > 0 ? s->table->n_rows : 0) : 1;
                if (n->table && !s->table) { pl_fail(p, "node '%s': no table '%s'", n->path, n->table); return -1; }
                if (s->n_rows == 0) p->sp--;   /* a table with no rows runs nothing */
            }
        }
    }
}

int pl_eval_var_bool(pl_player* p, const pl_env* env, const char* name, bool* out) {
    pl_val v;
    if (pl_var(p, env, name, &v) < 0) return -1;
    if (v.kind == YEXP_V_BOOL) { *out = v.i != 0; return 0; }
    if (v.kind == YEXP_V_NUMBER && (v.num == 0.0 || v.num == 1.0)) { *out = v.num == 1.0; return 0; }
    pl_fail(p, "'$%s' in an 'if' or 'unless' is neither true nor false (0 or 1)", name);
    return -1;
}

/* The next trial of the session, through the leaves. 1 with a trial started,
 * 0 when the session is over, < 0 on an error. */
int pl_start_trial(pl_player* p, const yscr_frame* f) {
    int rc;
    p->walk_frame_steps = 0;
    for (;;) {
        if (!p->leaf) {
            rc = pl_walk_next_leaf(p);
            if (rc <= 0 || rc == 2) return rc;
        }
        if (p->args.max_trials && p->global_trial >= p->args.max_trials) { p->walk_done = true; pl_leaf_close(p); return 0; }
        rc = ytr_next(&p->trials, &p->ti);
        if (rc >= 0) break;
        if (rc != YTR_DONE) { pl_fail(p, "node '%s': %s", p->leaf->node->path, ytr_error(&p->trials)); return -1; }
        /* the leaf ran its last trial: its loop, else on to the next */
        {
            const yexp_node* n = p->leaf->node;
            if (n->has_loop) {
                double acc = p->leaf_n_counted > 0 ? (double)p->leaf_n_correct / p->leaf_n_counted : 0.0;
                p->leaf_loop_pass++;
                if (acc < n->loop_until && p->leaf_loop_pass < n->loop_max) {
                    pl_leaf* lf = p->leaf;
                    int pass = p->leaf_loop_pass;
                    pl_leaf_close(p);
                    p->iter_counter = p->stack[p->sp - 1].child_iters[p->stack[p->sp - 1].child - 1]++;
                    if (pl_leaf_open(p, lf, p->iter_counter) < 0) return -1;
                    p->leaf_loop_pass = pass;
                    continue;
                }
            }
            pl_leaf_close(p);
        }
    }

    p->walk_steps = 0;
    p->tmpl = p->leaf->tmpl;
    p->track = NULL;
    if (p->ti.track >= 0) {
        p->track = p->leaf->tracks[p->ti.track];
    }
    pl_env_trial(p);
    if (p->track) {
        double x = p->track->next_level(p->track);
        if (x != x) { pl_fail(p, "method '%s': no proposal", p->track->def->name); return -1; }
        p->env.level = pl_to_stim_units(p->track, x);
    }
    if (pl_stim_begin_trial(p, f) < 0) return -1;
    if (pl_timeline_reset(p) < 0) return -1;
    if (ytl_anchor(&p->tl, PL_BASE_TRIAL, f->onset, 0) != 0) { pl_fail(p, "ytl_anchor: %s", ytl_error(&p->tl)); return -1; }
    {
        const yexp_template* d = p->tmpl->def;
        ytl_op* ops = p->tmpl->ops;
        int64_t cursor = 0;
        int n, r;
        p->ops_latest = 0;
        n = pl_build_ops(p, d->events, d->n_events, ops, p->tmpl->ops_cap, &cursor);
        if (n < 0) return -1;
        if (d->response) {
            /* An `if` can leave `from` unshown, and then the window never
             * opens and no deadline can end the trial. */
            int ch = pl_channel(p, d->response->from, NULL), k, found = 0;
            for (k = 0; k < n; k++) if (ops[k].op == YTL_OP_ON && ops[k].ch == ch) found = 1;
            if (!found) {
                pl_fail(p, "template '%s', trial %d: the response's 'from' stimulus '%s' is not shown on this trial (its 'on' is inside an 'if' that is false), so the response can never end",
                        d->name, p->global_trial, d->response->from);
                return -1;
            }
        }
        p->q = ytl_seq_on(&p->tl, PL_BASE_TRIAL);
        r = n ? ytl_run(&p->q, ops, n) : 0;
        if (r != 0 || p->q.err) {
            pl_fail(p, "template '%s': the events table: %s (op %d)", d->name, ytl_strerror(r ? r : p->q.err), p->q.err_call);
            return -1;
        }
        p->events_end = p->q.t > p->ops_latest ? p->q.t : p->ops_latest;
        p->from_landed = false;
        p->from_frame = -1;
        p->press_pending = false;
        p->responding = false;
        p->in_trial = true;
        if (d->response) {
            yrsp_collector* c = p->tmpl->collector;
            int64_t dur = 0, minrt = 0;
            if (pl_eval_ns(p, &p->env, d->response->duration, &dur) < 0 || pl_eval_ns(p, &p->env, d->response->min_rt, &minrt) < 0) return -1;
            yrsp_arm(c, f->onset);
            yrsp_set_window(c, minrt, dur);
            p->responding = true;
            p->env.key_answer_known = false;
            if (d->response->key_answer) {
                if (pl_eval_text(p, &p->env, d->response->key_answer, p->key_answer, sizeof p->key_answer) < 0) return -1;
                p->env.key_answer_known = true;
            }
            pl_participant_plan(p);
            if (p->failed) return -1;
        } else {
            /* with no response, `after` follows the events at once (2.5) */
            int64_t start = p->events_end;
            p->env.phase = PL_PH_AFTER;
            if (pl_build_after(p, start) < 0) return -1;
        }
    }
    p->trial_first_frame = f->index;
    p->global_trial++;
    p->frame_trial[f->index % PL_FRAME_RING] = p->global_trial - 1;
    return 1;
}

/* --- rows ------------------------------------------------------------- */

static pl_row* pl_row_new(pl_player* p) {
    pl_row* r = &p->rows[p->row_head];
    if (r->used) {
        /* full: the frame records cannot keep up, or rows are faster than
         * the pipeline. Write the oldest with what it has. */
        pl_row_write_oldest(p);
        r = &p->rows[p->row_head];
    }
    memset(r, 0, offsetof(pl_row, text));
    r->used = true;
    r->tier_worst = 0;
    p->row_head = (p->row_head + 1) % PL_PENDING_ROWS;
    r->text_len = 0;
    return r;
}

static char* pl_row_str(pl_row* r, const char* s) {
    size_t n = strlen(s);
    char* out;
    if (r->text_len + n + 1 > PL_ROW_TEXT) return NULL;
    out = r->text + r->text_len;
    memcpy(out, s, n + 1);
    r->text_len += n + 1;
    return out;
}

/* A value's text as a row cell, in the row's arena. A value too long is a
 * named refusal: a cut value would be wrong data with nothing to flag it. */
static int pl_cell_text(pl_player* p, pl_row* r, const char* name, const pl_val* x, const char** out) {
    char buf[256];
    int rc = pl_val_text(x, buf, sizeof buf);
    if (rc == -2) { pl_fail(p, "the column '%s': its value is longer than %zu bytes", name, sizeof buf - 1); return -1; }
    if (rc < 0) { pl_fail(p, "the column '%s': the value has no text", name); return -1; }
    *out = pl_row_str(r, buf);
    if (!*out) { pl_fail(p, "the column '%s': the row's text is over %d bytes", name, PL_ROW_TEXT); return -1; }
    return 0;
}

/* Set the cells of the row's own columns, per trial, from the environment. */
static int pl_row_fill(pl_player* p, pl_row* r) {
    int i;
    for (i = 0; i < p->n_cols; i++) {
        int src = p->csrc[i].source;
        const char* name = p->cols[i].name;
        char buf[256];
        const char* s = NULL;
        int64_t v = 0;
        double d = 0;
        int have = 0;   /* 0 none, 1 int64, 2 double, 3 string, 4 time ns, 5 bool */
        switch (src) {
        case CS_FIELD: {
            int f = p->csrc[i].ref;
            pl_val_text(&p->field_values[f], buf, sizeof buf);
            s = pl_row_str(r, buf);
            have = 3;
            break;
        }
        case CS_NODE: s = p->leaf->node->path; have = 3; break;
        case CS_TRIAL_TYPE: s = p->tmpl->def->name; have = 3; break;
        case CS_TRIAL_INDEX: v = r->trial_index; have = 1; break;
        case CS_ELAPSED: v = r->elapsed_ns; have = 4; break;
        case CS_T_INDEX: v = p->ti.index; have = 1; break;
        case CS_BLOCK: v = p->ti.block; have = 1; break;
        case CS_REP: v = p->ti.rep; have = 1; break;
        case CS_CONDITION: v = p->ti.condition; have = 1; break;
        case CS_TRACK: v = p->ti.track; have = 1; break;
        case CS_PRACTICE: v = p->ti.practice; have = 5; break;
        case CS_WARMUP: v = p->ti.warmup; have = 5; break;
        case CS_REQUEUED: v = p->ti.requeued; have = 5; break;
        case CS_AFTER_BREAK: v = p->ti.after_break; have = 5; break;
        case CS_FIRST_IN_BLOCK: v = p->ti.first_in_block; have = 5; break;
        case CS_VAR: {
            pl_val x;
            if (p->col_family[i] == CF_VAR) {
                /* per leaf: this name can be a `vars` here and `data` elsewhere */
                const yexp_node* n;
                const yexp_value* dv = NULL;
                int is_var = 0;
                for (n = p->leaf->node; n && !is_var && !dv; n = n->parent) {
                    int k;
                    for (k = 0; k < n->n_vars; k++) if (strcmp(n->vars[k].key, name) == 0) is_var = 1;
                    for (k = 0; k < n->n_jitters; k++) if (strcmp(n->jitters[k], name) == 0) is_var = 1;
                    for (k = 0; k < n->n_rule_vars; k++) if (strcmp(n->rule_vars[k].name, name) == 0) is_var = 1;
                    if (!is_var) for (k = 0; k < n->n_data; k++) if (strcmp(n->data[k].key, name) == 0) dv = n->data[k].value;
                }
                if (!is_var && !dv) break;
                if (is_var ? pl_var(p, &p->env, name, &x) < 0 : pl_eval(p, &p->env, dv, &x) < 0) return -1;
                if (p->cols[i].type == YTR_COL_TIME_NS) {
                    if (x.kind == YEXP_V_NUMBER) { v = x.flags & YEXP_VF_DURATION ? x.i : (int64_t)llround(x.num * 1e9); have = 4; }
                } else {
                    if (pl_cell_text(p, r, name, &x, &s) < 0) return -1;
                    have = 3;
                }
                break;
            }
            if (p->col_family[i] == CF_TABLE) {
                const yexp_node* n;
                const ytb_table* tb = NULL;
                int col = -1, row = -1;
                for (n = p->leaf->node; n; n = n->parent) {
                    if (!n->table) continue;
                    tb = pl_table_get(p, n->table);
                    if (!tb) continue;
                    col = ytb_col(tb, name);
                    if (col < 0) continue;
                    if (n == p->leaf->node) row = p->ti.condition;
                    else {
                        int st;
                        for (st = p->sp - 1; st >= 0; st--)
                            if (p->stack[st].node == n) row = p->stack[st].row;
                    }
                    break;
                }
                if (col < 0 || row < 0) break;
                s = ytb_text(tb, row, col);
                have = s ? 3 : 0;
                break;
            }
            break;
        }
        case CS_SEED: {
            char t[YDS_SEED_TEXT];
            if (p->leaf->desc.seed || p->leaf->desc.n_vars) {
                yds_seed_text(ytr_trial_seed(&p->trials, p->ti.index), t);
                s = pl_row_str(r, t);
                have = 3;
            }
            break;
        }
        case CS_METHOD: if (p->track) { s = p->track->def->name; have = 3; } break;
        case CS_LEVEL: if (p->track) { d = p->env.level; have = 2; } break;
        case CS_ESTIMATE: if (p->track) { d = p->env.estimate; have = 2; } break;
        case CS_RDK_SEED: {
            char t[32];
            snprintf(t, sizeof t, "%llu", (unsigned long long)p->stims[p->csrc[i].ref].rdk_seed);
            if (p->stims[p->csrc[i].ref].rdk_moving) { s = pl_row_str(r, t); have = 3; }
            break;
        }
        case CS_RDK_UPDATES: if (p->stims[p->csrc[i].ref].rdk_moving) { v = p->stims[p->csrc[i].ref].n_steps; have = 1; } break;
        case CS_RDK_DIGEST: {
            char t[20];
            if (p->stims[p->csrc[i].ref].rdk_moving) {
                snprintf(t, sizeof t, "%016llx", (unsigned long long)yrdk_digest(&p->stims[p->csrc[i].ref].field));
                s = pl_row_str(r, t);
                have = 3;
            }
            break;
        }
        case CS_ROLE_N: v = r->out[p->csrc[i].ref].n; have = 1; break;
        case CS_ROLE_FAILED: v = r->out[p->csrc[i].ref].failed; have = 1; break;
        case CS_ROLE_LAG: v = r->out[p->csrc[i].ref].lag_max; have = 4; break;
        case CS_CORRECT:
            if (p->env.has_correct) { v = p->env.correct; have = 5; }
            break;
        case CS_OUTCOME:
            if (p->env.outcome) { s = p->env.outcome; have = 3; }
            break;
        case CS_COLUMN: {
            int k;
            const yexp_value* val = NULL;
            for (k = 0; k < p->tmpl->def->n_columns; k++)
                if (strcmp(p->tmpl->def->columns[k].key, name) == 0) val = p->tmpl->def->columns[k].value;
            if (!val) break;
            {
                pl_val x;
                if (pl_eval(p, &p->env, val, &x) < 0) return -1;
                if (pl_cell_text(p, r, name, &x, &s) < 0) return -1;
            }
            have = 3;
            break;
        }
        default: break;
        }
        if (have) {
            if (r->n_cells >= YTR_DATA_MAX_COLS) { pl_fail(p, "too many cells"); return -1; }
            r->cells[r->n_cells].col = i;
            r->cells[r->n_cells].type = have;
            r->cells[r->n_cells].i = v;
            r->cells[r->n_cells].d = d;
            r->cells[r->n_cells].s = s;
            r->n_cells++;
        }
    }
    return 0;
}

static int pl_stat_index(const pl_row* r, int stim_i) {
    int i;
    for (i = 0; i < r->n_stats; i++)
        if (r->stats[i].stim == stim_i) return i;
    return -1;
}

/* The row's flip-record-dependent columns, then the row. */
static int pl_row_flush(pl_player* p, pl_row* r) {
    ytr_data* w = yds_trials_writer(&p->ds);
    int i;
    if (!r->used) return 0;
    /* record_data false: nothing may be set, or the values would stay on the
     * writer and land in the next row that does not set those columns */
    if (!r->write_row) { r->used = false; return 0; }
    for (i = 0; i < r->n_cells; i++) {
        int rc = 0;
        switch (r->cells[i].type) {
        case 1: rc = p->cols[r->cells[i].col].type == YTR_COL_BOOL
                         ? ytr_data_set_bool(w, r->cells[i].col, r->cells[i].i != 0)
                         : ytr_data_set_int(w, r->cells[i].col, (int)r->cells[i].i); break;
        case 2: rc = ytr_data_set_double(w, r->cells[i].col, r->cells[i].d); break;
        case 3: rc = ytr_data_set_string(w, r->cells[i].col, r->cells[i].s); break;
        case 4: rc = ytr_data_set_time_ns(w, r->cells[i].col, r->cells[i].i); break;
        case 5: rc = ytr_data_set_bool(w, r->cells[i].col, r->cells[i].i != 0); break;
        default: break;
        }
        if (rc < 0) { pl_fail(p, "the row of trial %d: %s", r->trial_index, ytr_data_error(w)); return -1; }
    }
    for (i = 0; i < p->n_cols; i++) {
        int src = p->csrc[i].source;
        int ref = p->csrc[i].ref;
        int si;
        const pl_stim_stats* st;
        switch (src) {
        case CS_TIER_WORST: ytr_data_set_int(w, i, r->tier_worst); break;
        case CS_FRAMES_DROPPED: ytr_data_set_int(w, i, r->frames_dropped); break;
        case CS_FLIP_FLAGS: ytr_data_set_int(w, i, r->flip_flags); break;
        case CS_STIM_ONSET: case CS_STIM_OFFSET: case CS_STIM_FRAMES: case CS_STIM_RESIDUAL:
        case CS_STIM_TIER: case CS_STIM_DROPPED: case CS_STIM_FLAGS:
            si = pl_stat_index(r, ref);
            if (si < 0) break;
            st = &r->stats[si];
            if (src == CS_STIM_ONSET && st->onset_ns != YTR_MISSING_NS) ytr_data_set_time_ns(w, i, st->onset_ns);
            if (src == CS_STIM_OFFSET && st->offset_ns != YTR_MISSING_NS) ytr_data_set_time_ns(w, i, st->offset_ns);
            if (src == CS_STIM_RESIDUAL && st->residual_ns != YTR_MISSING_NS) ytr_data_set_time_ns(w, i, st->residual_ns);
            if (src == CS_STIM_FRAMES && st->onset_frame >= 0) ytr_data_set_int(w, i, st->frames);
            if (src == CS_STIM_TIER && st->onset_frame >= 0) ytr_data_set_int(w, i, st->tier);
            if (src == CS_STIM_DROPPED && st->onset_frame >= 0) ytr_data_set_int(w, i, st->dropped);
            if (src == CS_STIM_FLAGS && st->onset_frame >= 0) ytr_data_set_int(w, i, st->flags);
            break;
        default:
            break;
        }
    }
    if (r->has_res) {
        if (yrsp_data_set(w, &r->res) < 0) { pl_fail(p, "the row of trial %d: %s", r->trial_index, ytr_data_error(w)); return -1; }
    }
    if (yds_write_trial(&p->ds, NULL) < 0) { pl_fail(p, "the trial row: %s", yds_error(&p->ds)); return -1; }
    r->used = false;
    return 0;
}

void pl_row_write_oldest(pl_player* p) {
    pl_row* r = &p->rows[p->row_tail];
    if (!r->used) return;
    (void)pl_row_flush(p, r);
    p->row_tail = (p->row_tail + 1) % PL_PENDING_ROWS;
}

/* D10 is silent on a stimulus shown twice. Here: the first onset and the
 * last offset, and frames, tier, drops and flags over every frame it was
 * shown (frame_vis, written when the frame was drawn). */
static void pl_stats_update(const pl_player* p, pl_stim_stats* stats, int n, const yscr_record* rec) {
    int k;
    const uint8_t* vis = p->frame_vis + (size_t)(rec->index % PL_FRAME_RING) * (size_t)p->pool_stats;
    for (k = 0; k < n; k++) {
        pl_stim_stats* st = &stats[k];
        if (rec->index == st->onset_frame) {
            st->onset_ns = rec->onset ? rec->onset : rec->planned;
            st->residual_ns = rec->onset ? rec->residual : YTR_MISSING_NS;
        }
        if (rec->index == st->offset_frame) st->offset_ns = rec->onset ? rec->onset : rec->planned;
        if (!vis[k]) continue;
        st->frames++;
        if (st->tier < rec->tier) st->tier = rec->tier;
        st->dropped += (int)rec->dropped;
        st->flags |= rec->flags;
    }
}

/* A frame's flip record: its trial's counts, its stimuli's onsets. */
static int pl_on_record(pl_player* p, const yscr_record* rec) {
    int trial = p->frame_trial[rec->index % PL_FRAME_RING];
    yds_frame fr;
    int i;
    yds_frame_from_screen(&fr, rec, trial);
    if (p->args.sim) {
        /* the phase durations are real CPU costs (ysp/rt.h, COSTS), not the
         * virtual clock's, so two --sim runs would differ only in them */
        fr.evaluate_ns = fr.script_ns = fr.draw_ns = fr.upload_ns = fr.swap_ns = fr.gpu_ns = YTR_MISSING_NS;
    }
    if (yds_write_frame(&p->ds, &fr) < 0) { pl_fail(p, "frames.csv: %s", yds_error(&p->ds)); return -1; }
    if (p->in_trial && trial == p->global_trial - 1) {
        /* the running trial: its row does not exist yet */
        pl_stats_update(p, p->stats_now, p->tmpl->n_record, rec);
        if (p->now_tier < rec->tier) p->now_tier = rec->tier;
        p->now_dropped += (int)rec->dropped;
        p->now_flags |= rec->flags;
    } else {
        for (i = 0; i < PL_PENDING_ROWS; i++) {
            pl_row* r = &p->rows[(p->row_tail + i) % PL_PENDING_ROWS];
            if (!r->used || r->trial_index != trial) continue;
            pl_stats_update(p, r->stats, r->n_stats, rec);
            if (r->tier_worst < rec->tier) r->tier_worst = rec->tier;
            r->frames_dropped += (int)rec->dropped;
            r->flip_flags |= rec->flags;
            if (r->ended && rec->index >= r->last_frame) r->final = true;
            break;
        }
    }
    while (p->rows[p->row_tail].used && p->rows[p->row_tail].final) {
        if (pl_row_flush(p, &p->rows[p->row_tail]) < 0) return -1;
        p->row_tail = (p->row_tail + 1) % PL_PENDING_ROWS;
    }
    (void)pl_events_flush;
    return pl_events_flush(p, false);
}

static const yrsp_result* pl_finish(pl_player* p, yrsp_result* r) {
    yrsp_finish(p->tmpl->collector, r);
    return r;
}

static void pl_env_response(pl_player* p, const yrsp_result* r) {
    pl_env* e = &p->env;
    pl_tmpl* t = p->tmpl;
    const yexp_response* d = t->def->response;
    e->phase = PL_PH_AFTER;
    e->responded = (r->flags & YRSP_R_RESPONDED) != 0;   /* 2.3; an any-key response has no choice index */
    e->response = !e->responded ? NULL : r->response >= 0 ? t->choice_keys[r->response] : yrsp_key_name(r->control);
    if (e->responded && !e->response) e->response = "";
    e->rt_ns = r->rt_ns;
    e->has_correct = d->key_answer != NULL;
    e->correct = e->has_correct && e->responded && strcmp(e->response, p->key_answer) == 0;
    if (!e->responded) e->outcome = "timeout";
    else if (e->has_correct) e->outcome = e->correct ? "correct" : "wrong";
    else e->outcome = "responded";   /* no key_answer: the format has no name
                                      * for it; see docs/player.md */
}

static void pl_loop_count(pl_player* p, bool counted, bool correct) {
    int i;
    if (!counted) return;
    p->leaf_n_counted++;
    if (correct) p->leaf_n_correct++;
    for (i = 1; i < p->sp; i++) {
        p->stack[i].n_counted++;
        if (correct) p->stack[i].n_correct++;
    }
}

/* The method hears the value in its own domain. A value of 0 or less in a
 * log domain has no logarithm; the C program floors the level at 1e-4 before
 * the log, so a trial at the lowest contrast is told as "very low", not
 * lost. The definition has no key for the floor (a finding). */
static double pl_method_input(const pl_method* m, double value) {
    if (m->log_domain && value < 1e-4) value = 1e-4;
    return m->def->log10 ? log10(value) : value;
}

/* A response's code on its output role (2.6). --sim has no reader thread
 * for a device rule, so the code goes out when the frame loop sees the
 * response, as a keyboard role's would: ysp/response.h's rule window and
 * its mismatch flag are not exercised here (docs/player.md). */
static int pl_response_trigger(pl_player* p, const yrsp_result* r) {
    const yexp_response* d = p->tmpl->def->response;
    int ch;
    int64_t code = 0;
    yds_event ev;
    if (!d->trigger_role || !p->env.responded) return 0;
    ch = pl_trigger_channel(p, d->trigger_role);
    if (ch < 0) { pl_fail(p, "response.trigger: '%s' is not an output role", d->trigger_role); return -1; }
    if (pl_eval_int(p, &p->env, d->trigger_code, &code) < 0) return -1;
    if (code < 0 || code > 255) { pl_fail(p, "the response's trigger code %lld is not in 0..255", (long long)code); return -1; }
    p->out_now[ch].n++;
    yds_event_init(&ev);
    ev.t_ns = r->t_response;
    ev.trial_index = p->global_trial - 1;
    ev.kind = "out";
    ev.role = p->trigger_roles[ch]->name;
    ev.code = code;
    ev.deadline_ns = r->t_response;
    ev.flags = 1;   /* from the response, not a flip's planned vblank */
    return pl_event_queue(p, &ev);
}

/* The MARK fired: the trial is over. */
int pl_end_trial(pl_player* p, const yscr_frame* f) {
    pl_tmpl* t = p->tmpl;
    const yexp_template* d = t->def;
    yrsp_result res;
    pl_row* row = pl_row_new(p);
    int outcome_val = YTR_INVALID;
    bool requeue = false;
    bool have_res = false;
    if (t->collector) {
        pl_finish(p, &res);
        pl_env_response(p, &res);
        have_res = true;
    } else {
        /* no response: outcome is empty; no value to tally */
        p->env.phase = PL_PH_AFTER;
    }
    /* the method hears a track trial with a response (2.4) */
    if (p->track && d->method_value && have_res && p->env.responded) {
        double value;
        if (pl_eval_num(p, &p->env, d->method_value, &value) < 0) return -1;
        if (p->track->update(p->track, pl_method_input(p->track, value), p->env.correct) < 0) {
            pl_fail(p, "method '%s': the update failed", p->track->def->name);
            return -1;
        }
    }
    if (p->track) {
        double x = p->track->estimate(p->track);
        p->env.estimate = pl_to_stim_units(p->track, x);
    }
    /* ytr_update(): the template's outcome (2.5) */
    if (d->response) {
        if (d->outcome && d->outcome->kind == YEXP_V_STRING && strcmp(d->outcome->str, "correct") == 0) {
            if (p->env.responded) {
                if (p->env.has_correct) outcome_val = p->env.correct ? 1 : 0;
                else outcome_val = res.response >= 0 ? res.response : 0;   /* a category, the choice; 0 for any key */
            } else if (p->leaf->node->requeue_no_response && !p->ti.practice && !p->ti.warmup && p->ti.track < 0) {
                requeue = true;
            }
        } else if (d->outcome && p->env.responded) {
            int64_t x;
            if (pl_eval_int(p, &p->env, d->outcome, &x) < 0) return -1;
            outcome_val = x < 0 ? YTR_INVALID : (int)x;
        } else if (!p->env.responded && p->leaf->node->requeue_no_response && !p->ti.practice && !p->ti.warmup && p->ti.track < 0) {
            requeue = true;
        }
    }
    p->env.phase = PL_PH_ROW;
    {
        const yexp_node* n = p->leaf->node;
        row->trial_index = p->global_trial - 1;
        row->last_frame = f->index;
        row->first_frame = p->trial_first_frame;
        row->elapsed_ns = f->onset - p->t0_session;
        row->has_res = have_res;
        if (have_res) {
            row->res = res;
            /* an any-key response has no choice name: the key's is the data */
            if (!row->res.response_name && p->env.responded) row->res.response_name = p->env.response;
        }
        row->write_row = true;
        {
            const yexp_node* q;
            for (q = n; q; q = q->parent) if (!q->record_data) row->write_row = false;   /* inherited (2.4) */
        }
        row->ended = true;
        row->n_stats = t->n_record;
        row->tier_worst = p->now_tier;
        row->frames_dropped = p->now_dropped;
        row->flip_flags = p->now_flags;
        row->stats = p->stat_pool[row - p->rows];
        memset(row->stats, 0, sizeof(pl_stim_stats) * (size_t)p->pool_stats);
        row->out = p->out_pool[row - p->rows];
        memcpy(row->out, p->out_now, sizeof(pl_out_stats) * (size_t)p->n_triggers);
        {
            int k;
            for (k = 0; k < t->n_record; k++) {
                row->stats[k] = p->stats_now[k];
                row->stats[k].stim = t->record_stims[k];
            }
        }
        if (pl_row_fill(p, row) < 0) return -1;
    }
    if (have_res && p->env.responded) {
        yds_event ev;
        yds_event_init(&ev);
        ev.t_ns = res.t_response;
        ev.trial_index = p->global_trial - 1;
        ev.kind = "response";
        ev.role = d->response->role;
        ev.control = res.control;
        ev.code = res.code;
        ev.tier = res.stamp_tier;
        ev.text = p->env.response;
        if (pl_event_queue(p, &ev) < 0) return -1;
    }
    /* the steps of each moving rdk, to its stream (2.7) */
    {
        int i;
        for (i = 0; i < p->n_stims; i++) {
            pl_stim* s = &p->stims[i];
            int k;
            if (s->def->kind != YEXP_RDK || !s->rdk_moving) continue;
            for (k = 0; k < s->n_steps; k++) {
                pl_rdk_step_row sr;
                sr.trial_index = p->global_trial - 1;
                sr.update = k;
                sr.t_ns = s->steps[k].t;
                sr.coherence = s->steps[k].coherence;
                sr.direction = s->steps[k].direction;
                sr.speed = s->steps[k].speed;
                if (yds_write_stream(&p->ds, s->stream, &sr) < 0) { pl_fail(p, "the stream of '%s': %s", s->def->name, yds_error(&p->ds)); return -1; }
            }
        }
    }
    if (requeue) {
        int r = ytr_requeue(&p->trials);
        if (r < 0) { pl_fail(p, "node '%s': %s", p->leaf->node->path, ytr_error(&p->trials)); return -1; }
    } else {
        int r = ytr_update(&p->trials, outcome_val, NULL);
        if (r < 0) { pl_fail(p, "node '%s': %s", p->leaf->node->path, ytr_error(&p->trials)); return -1; }
    }
    pl_loop_count(p, have_res, have_res && p->env.responded && p->env.correct);
    p->in_trial = false;
    p->trials_done++;
    return 0;
}

/* --- the frame --------------------------------------------------------- */

/* Sounds are scheduled ahead on the audio clock (rig_spec 4.5.1): each
 * frame peeks the next two frames' events for a sound's onset. */
static int pl_sounds(pl_player* p, const yscr_frame* f) {
    int i;
    for (i = 0; i < p->n_stims; i++) {
        pl_stim* s = &p->stims[i];
        int k, n;
        if (s->def->kind != YEXP_SOUND || s->ch_visible < 0) continue;
        n = ytl_peek(&p->tl, PL_BASE_TRIAL, f->onset, f->onset + 2 * f->period + 1, p->peek, p->fired_cap);
        for (k = 0; k < n && k < p->fired_cap; k++) {
            /* every onset once: a sound can play more than once in a trial */
            if (p->peek[k].kind == YTL_ONSET && p->peek[k].target == s->ch_visible && p->peek[k].onset > s->sound_scheduled) {
                yau_id id = yau_play_at(&p->audio, s->sound, p->peek[k].onset);
                if (id < 0) { pl_fail(p, "'%s': %s", s->def->name, yau_error(&p->audio)); return -1; }
                if (s->sound_first < 0) { s->sound_first = 1; s->sound_id = id; }
                {
                    yds_event ev;
                    yds_event_init(&ev);
                    ev.t_ns = p->peek[k].onset;
                    ev.trial_index = p->global_trial - 1;
                    ev.kind = "sound";
                    ev.stimulus = s->def->name;
                    if (pl_event_queue(p, &ev) < 0) return -1;
                }
                s->sound_scheduled = p->peek[k].onset;
            }
        }
    }
    return 0;
}

/* The audio onset of the response's `from`, when `from` is a sound. */
static int pl_from_sound(pl_player* p) {
    yau_onset on;
    const pl_stim* s;
    if (!p->in_trial || !p->tmpl->collector || p->from_landed) return 0;
    s = pl_stim_by_name(p, p->tmpl->def->response->from);
    if (!s || s->def->kind != YEXP_SOUND || s->sound_scheduled < 0) return 0;
    if (yau_update(&p->audio) < 0) { pl_fail(p, "audio: %s", yau_error(&p->audio)); return -1; }
    if (yau_result(&p->audio, s->sound_id, &on) < 0) { pl_fail(p, "audio: %s", yau_error(&p->audio)); return -1; }
    if (on.onset) {
        yrsp_onset o = yrsp_onset_audio(&on);
        yrsp_set_onset(p->tmpl->collector, &o);
        if (o.final) {
            p->from_landed = true;
            if (!p->silent) pl_participant_arm(p, on.onset);
        }
    }
    return 0;
}

static int pl_on_fired(pl_player* p, const yscr_frame* f, int n) {
    int i;
    for (i = 0; i < n; i++) {
        const ytl_event* e = &p->fired[i];
        switch (e->kind) {
        case YTL_ONSET: case YTL_OFFSET: case YTL_SET: {
            const pl_chan* c = &p->channels[e->target];
            yds_event ev;
            yds_event_from_timeline(&ev, e, c->stim->name, p->global_trial - 1);
            if (pl_event_queue(p, &ev) < 0) return -1;
            if (e->kind != YTL_SET && !c->prop) {
                int k;
                for (k = 0; k < p->tmpl->n_record; k++)
                    if (p->tmpl->record_stims[k] >= 0 && p->stims[p->tmpl->record_stims[k]].def == c->stim) {
                        pl_stim_stats* st = &p->stats_now[k];
                        if (e->kind == YTL_ONSET && st->onset_frame < 0) st->onset_frame = e->frame;
                        if (e->kind == YTL_OFFSET) st->offset_frame = e->frame;
                    }
                if (e->kind == YTL_ONSET && !p->from_landed && p->tmpl->collector &&
                    c->stim->kind != YEXP_SOUND && strcmp(p->tmpl->def->response->from, c->stim->name) == 0) {
                    yrsp_onset o = yrsp_onset_landing(e);
                    yrsp_set_onset(p->tmpl->collector, &o);
                    p->from_landed = true;
                    p->from_frame = e->frame;
                }
            }
            break;
        }
        case YTL_TRIGGER: {
            yds_event ev;
            int ok = yscr_trigger(&p->scr, e->target, (uint32_t)e->code);
            pl_out_stats* o = &p->out_now[e->target];
            o->n++;
            if (ok != YSCR_OK) o->failed++;
            yds_event_from_timeline(&ev, e, NULL, p->global_trial - 1);
            ev.kind = "out";
            ev.role = p->trigger_roles[e->target]->name;
            ev.deadline_ns = e->onset;
            ev.t_ns = e->onset;
            if (pl_event_queue(p, &ev) < 0) return -1;
            break;
        }
        case YTL_MARK: {
            yds_event ev;
            if (e->code == PL_MARK_END) {
                if (pl_end_trial(p, f) < 0) return -1;
                break;
            }
            yds_event_from_timeline(&ev, e, NULL, p->global_trial - 1);
            ev.text = e->code >= 0 && e->code < p->marks.n ? p->marks.names[e->code] : NULL;
            if (pl_event_queue(p, &ev) < 0) return -1;
            break;
        }
        default:
            break;
        }
    }
    return 0;
}

static int pl_draw(pl_player* p, const yscr_frame* f) {
    int i;
    ygfx_begin(&p->gfx, f);
    for (i = 0; i < p->n_stims; i++) {
        pl_stim* s = &p->stims[p->draw_order[i]];
        /* a stimulus no step shows has no channel and is never drawn */
        if (s->def->kind == YEXP_SOUND || s->ch_visible < 0) continue;
        if (s->def->kind == YEXP_RDK) {
            if (s->gs.visible >= 0.5f) {
                if (!s->rdk_moving) {
                    s->rdk_t0 = f->onset;
                    yrdk_start(&s->field, s->rdk_seed, 0);
                    s->rdk_moving = true;
                }
                if (s->n_steps >= PL_MAX_STEPS_RDK) {
                    pl_fail(p, "stimulus '%s' has moved for %d frames in one trial, the most its stream can record", s->def->name, PL_MAX_STEPS_RDK);
                    return -1;
                }
                if (yrdk_update(&s->field, f->onset - s->rdk_t0) < 0) { pl_fail(p, "'%s': %s", s->def->name, yrdk_error(&s->field)); return -1; }
                s->steps[s->n_steps++] = s->field.last;
                yrdk_xy(&s->field, s->xy);
                ygfx_buffer_update(&p->gfx, s->buf, 0, s->xy, sizeof(float) * 2 * (size_t)s->rdk_count);
            }
            ygfx_draw(&p->gfx, &s->gs);
            continue;
        }
        if (s->def->kind == YEXP_TEXT) {
            int r;
            for (r = 0; r < s->n_runs; r++) {
                ygfx_stim* run = &s->text_runs[r];
                run->visible = s->gs.visible;
                run->x = s->gs.x;
                run->y = s->gs.y;
                run->ori = s->gs.ori;
                memcpy(run->color, s->gs.color, sizeof run->color);
                run->opacity = s->gs.opacity;
                run->gate = s->gs.gate;
                ygfx_draw(&p->gfx, run);
            }
            continue;
        }
        ygfx_draw(&p->gfx, &s->gs);
    }
    ygfx_end(&p->gfx);
    return 0;
}

int pl_frame(pl_player* p, const yscr_frame* f) {
    int i, rc, n;
    /* flip records first: a trial's press is planned from its stimulus's
     * record, and rows wait for their last frame's record */
    for (i = 0; i < f->n_done; i++) {
        const yscr_record* rec = &f->done[i];
        if (pl_on_record(p, rec) < 0) return -1;
        if (p->from_landed && !p->press_planned && rec->index == p->from_frame && p->tmpl && p->tmpl->collector && !p->silent)
            pl_participant_arm(p, rec->onset ? rec->onset : rec->planned);
    }
    p->frame_trial[f->index % PL_FRAME_RING] = YTR_MISSING_INT;   /* until a trial claims it */
    if (p->t0_session == 0) p->t0_session = f->onset;
    if (!p->in_trial) {
        int64_t t0 = p->args.measure ? yrt_real_ns() : 0;
        if (p->pending_stop) return 1;
        rc = pl_start_trial(p, f);
        if (p->args.measure) {
            int64_t d = yrt_real_ns() - t0;
            if (d > p->work_trial_max_ns) p->work_trial_max_ns = d;
        }
        if (rc < 0) return -1;
        if (rc == 0) {
            p->pending_stop = true;
            return 1;
        }
    }
    if (p->in_trial) p->frame_trial[f->index % PL_FRAME_RING] = p->global_trial - 1;
    /* the scripted participant's key, on the frames it falls in (as the C
     * programs feed theirs) */
    if (p->in_trial && p->press_pending && p->tmpl->collector) {
        yrsp_input ev;
        pl_tmpl* t = p->tmpl;
        if (p->press_at <= f->onset) {
            /* the last trial's release first, if it is still due: a key's
             * events must arrive in time order */
            if (p->release_due) { yrsp_feed(t->collector, &p->release_ev); p->release_due = false; }
            memset(&ev, 0, sizeof ev);
            ev.t = p->press_at;
            ev.type = YRSP_PRESS;
            ev.stamp = YRSP_TIER_SIM;
            ev.value = 1.0f;
            if (p->press_choice >= 0) {
                ev.kind = t->collector->ch[p->press_choice].kind;
                ev.control = t->collector->ch[p->press_choice].control;
            } else {
                ev.kind = YRSP_KIND_KEYBOARD;
                ev.control = (uint32_t)yrsp_scancode("space");
            }
            yrsp_feed(t->collector, &ev);
            p->press_pending = false;
            p->release_due = true;
            p->release_ev = ev;
            p->release_ev.t = p->release_at;
            p->release_ev.type = YRSP_RELEASE;
            p->release_ev.value = 0.0f;
        }
    }
    if (p->in_trial && p->release_due && p->tmpl->collector && p->release_ev.t <= f->onset) {
        yrsp_feed(p->tmpl->collector, &p->release_ev);
        p->release_due = false;
    }
    if (p->in_trial && p->tmpl->collector) {
        int st = yrsp_update_frame(p->tmpl->collector, f);
        if (p->responding && st == YRSP_ENDED) {
            yrsp_result live;
            int64_t bt = 0;
            yrsp_finish(p->tmpl->collector, &live);
            pl_env_response(p, &live);
            if (pl_response_trigger(p, &live) < 0) return -1;
            ytl_base_time(&p->tl, PL_BASE_TRIAL, f->onset, &bt);
            if (pl_build_after(p, bt > p->events_end ? bt : p->events_end) < 0) return -1;
            p->responding = false;
        }
    }
    if (p->in_trial && p->audio_open && (pl_sounds(p, f) < 0 || pl_from_sound(p) < 0)) return -1;
    n = 0;
    if (p->in_trial) {   /* false only on a frame where the walk yielded */
        n = ytl_evaluate(&p->tl, &(ytl_frame){ f->onset, f->period, f->index }, p->fired, p->fired_cap);
        if (n < 0) { pl_fail(p, "ytl_evaluate: %s", ytl_strerror(n)); return -1; }
        if (pl_on_fired(p, f, n) < 0) return -1;
        {
            /* who was on screen this frame: the row's counts need it per
             * frame, and a stimulus can be shown more than once (D10) */
            uint8_t* vis = p->frame_vis + (size_t)(f->index % PL_FRAME_RING) * (size_t)p->pool_stats;
            const float* values = ytl_values(&p->tl);
            int k;
            for (k = 0; k < p->tmpl->n_record; k++) {
                int si = p->tmpl->record_stims[k];
                int ch = si >= 0 ? p->stims[si].ch_visible : -1;
                vis[k] = (uint8_t)(ch >= 0 && values[ch] >= 0.5f);
            }
        }
    }
    /* also on the end's frame: a step that lands with the trial's last event
     * must reach this frame's draw */
    if (p->global_trial > 0) ygfx_apply(p->binds, p->n_binds, ytl_values(&p->tl));
    if (p->trace_file && p->global_trial > 0) {
        const float* values = ytl_values(&p->tl);
        int k;
        for (k = 0; k < p->n_channels; k++)
            fprintf((FILE*)p->trace_file, "%lld,%d,%s,%s,%.6f\n", (long long)f->index, p->global_trial - 1, p->channels[k].stim->name,
                    p->channels[k].prop ? p->channels[k].prop : "visible", (double)values[k]);
    }
    yscr_mark(&p->scr, YSCR_PHASE_EVALUATE);
    if (pl_draw(p, f) < 0) return -1;
    if (p->args.max_seconds && f->onset - (p->t0_session ? p->t0_session : f->onset) > p->args.max_seconds * 1000000000LL) {
        pl_fail(p, "the session passed its budget of %lld virtual seconds (a window with no deadline and no scripted press?)", (long long)p->args.max_seconds);
        return -1;
    }
    return 0;
}

/* The event queue goes out with the row of its trial, between trials. */
int pl_events_flush(pl_player* p, bool all) {
    int i;
    (void)all;
    for (i = 0; i < p->n_events; i++)
        if (yds_write_event(&p->ds, &p->events[i]) < 0) { pl_fail(p, "events.csv: %s", yds_error(&p->ds)); return -1; }
    p->n_events = 0;
    return 0;
}

int pl_event_queue(pl_player* p, const yds_event* ev) {
    if (p->n_events == PL_EVENT_QUEUE && pl_events_flush(p, false) < 0) return -1;
    p->events[p->n_events++] = *ev;
    return 0;
}

int pl_run(pl_player* p) {
    yscr_frame f;
    int rc = 0, i, r;
    p->sp = 0;
    memset(&p->stack[0], 0, sizeof p->stack[0]);
    p->stack[0].children = p->def->timeline;
    p->stack[0].n_children = p->def->n_timeline;
    p->stack[0].n_rows = 1;
    p->stack[0].n_reps = 1;
    p->sp = 1;
    p->row_head = p->row_tail = 0;
    p->events = (yds_event*)pl_alloc(p, sizeof(yds_event) * PL_EVENT_QUEUE);
    p->peek = (ytl_event*)pl_alloc(p, sizeof(ytl_event) * (size_t)(p->fired_cap + 1));
    if (!p->peek) return -1;
    if (!p->events) return -1;
    p->node_seeds = (pl_node_seed_rec*)pl_alloc(p, sizeof(pl_node_seed_rec) * PL_MAX_LEAF_RUNS);
    if (!p->node_seeds) return -1;
    p->pool_stats = 0;
    for (i = 0; i < p->n_tmpls; i++) if (p->tmpls[i].n_record > p->pool_stats) p->pool_stats = p->tmpls[i].n_record;
    p->frame_vis = (uint8_t*)pl_alloc(p, (size_t)PL_FRAME_RING * (size_t)(p->pool_stats + 1));
    if (!p->frame_vis) return -1;
    p->stats_now = (pl_stim_stats*)pl_alloc(p, sizeof(pl_stim_stats) * (size_t)(p->pool_stats + 1));
    if (!p->stats_now) return -1;
    p->out_now = (pl_out_stats*)pl_alloc(p, sizeof(pl_out_stats) * (size_t)(p->n_triggers + 1));
    if (!p->out_now) return -1;
    for (i = 0; i < PL_PENDING_ROWS; i++) {
        p->stat_pool[i] = (pl_stim_stats*)pl_alloc(p, sizeof(pl_stim_stats) * (size_t)(p->pool_stats + 1));
        p->out_pool[i] = (pl_out_stats*)pl_alloc(p, sizeof(pl_out_stats) * (size_t)(p->n_triggers + 1));
        if (!p->stat_pool[i] || !p->out_pool[i]) return -1;
    }
    if (p->args.trace_path) {
        p->trace_file = fopen(p->args.trace_path, "w");
        if (!p->trace_file) { pl_fail(p, "cannot write '%s'", p->args.trace_path); return -1; }
        fputs("frame,trial_index,stimulus,prop,value\n", (FILE*)p->trace_file);
    }
    if (p->args.participant && strncmp(p->args.participant, "script:", 7) == 0 && pl_script_load(p, p->args.participant + 7) < 0) return -1;
    for (i = 0; i < PL_FRAME_RING; i++) p->frame_trial[i] = YTR_MISSING_INT;
    for (;;) {
        rc = yscr_begin(&p->scr, &f);
        if (rc == YSCR_QUIT) break;
        if (rc != YSCR_OK) { pl_fail(p, "the screen: %s", yscr_error(&p->scr)); return -1; }
        if (p->in_trial == 0) {
            int k;
            for (k = 0; k < p->pool_stats; k++) {
                memset(&p->stats_now[k], 0, sizeof p->stats_now[k]);
                p->stats_now[k].onset_frame = p->stats_now[k].offset_frame = -1;
                p->stats_now[k].onset_ns = p->stats_now[k].offset_ns = p->stats_now[k].residual_ns = YTR_MISSING_NS;
            }
            memset(p->out_now, 0, sizeof(pl_out_stats) * (size_t)(p->n_triggers + 1));
            p->now_tier = p->now_dropped = p->now_flags = 0;
        }
        if (p->args.measure) {
            int64_t t0 = yrt_real_ns(), t1;
            pl_loop_active = 1;
            r = pl_frame(p, &f);
            pl_loop_active = 0;
            t1 = yrt_real_ns();
            p->work_n++;
            if (t1 - t0 > p->work_max_ns) p->work_max_ns = t1 - t0;
            if (t1 - t0 > 16000000) p->work_over_16ms++;
        } else {
            pl_loop_active = 1;
            r = pl_frame(p, &f);
            pl_loop_active = 0;
        }
        if (r < 0) return -1;
        if (r == 1) {   /* the session ended before this frame drew: a blank frame */
            ygfx_begin(&p->gfx, &f);
            ygfx_end(&p->gfx);
        }
        {
            int src = yscr_flip(&p->scr);
            if (src != YSCR_OK) { pl_fail(p, "the flip: %s", yscr_error(&p->scr)); return -1; }
        }
        if (r == 1) break;
    }
    /* drain: rows wait for their last frames' records; a few more flips show
     * the end of the session and bring those records in (the pipeline is at
     * most a few frames deep) */
    for (i = 0; i < 8 && rc == YSCR_OK; i++) {
        int k;
        rc = yscr_begin(&p->scr, &f);
        if (rc != YSCR_OK) break;
        p->frame_trial[f.index % PL_FRAME_RING] = YTR_MISSING_INT;
        for (k = 0; k < f.n_done; k++)
            if (pl_on_record(p, &f.done[k]) < 0) return -1;
        ygfx_begin(&p->gfx, &f);
        ygfx_end(&p->gfx);
        if (yscr_flip(&p->scr) != YSCR_OK) break;
    }
    /* by count, not head != tail: a full ring has head == tail */
    for (i = 0; i < PL_PENDING_ROWS; i++) {
        if (p->rows[p->row_tail].used && pl_row_flush(p, &p->rows[p->row_tail]) < 0) return -1;
        p->row_tail = (p->row_tail + 1) % PL_PENDING_ROWS;
    }
    if (pl_events_flush(p, true) < 0) return -1;
    {
        char buf[32];
        yjs_arena a;
        yjs_value* arr;
        yjs_arena_init_heap(&a, 0);
        arr = yjs_new(&a, YJS_ARRAY);
        for (i = 0; i < p->n_node_seeds; i++) {
            yjs_value* o = yjs_new(&a, YJS_OBJECT);
            yjs_set(&a, o, "path", yjs_new_stringz(&a, p->node_seeds[i].path));
            yjs_set(&a, o, "iteration", yjs_new_int(&a, p->node_seeds[i].iteration));
            snprintf(buf, sizeof buf, "%llu", (unsigned long long)p->node_seeds[i].seed);
            yjs_set(&a, o, "seed", yjs_new_stringz(&a, buf));
            yjs_push(&a, arr, o);
        }
        if (yds_session_set(&p->ds, "node_seeds", arr) < 0) pl_fail(p, "session.json: %s", yds_error(&p->ds));
        yjs_arena_free(&a);
    }
    if (p->trace_file) { fclose((FILE*)p->trace_file); p->trace_file = NULL; }
    if (yds_finish(&p->ds) < 0) { pl_fail(p, "the data set: %s", yds_error(&p->ds)); return -1; }
    return p->failed ? -1 : 0;
}
