/* ysp_trials.c - MATLAB/Octave MEX binding for ysp/trials.h (builds to ysp_trials.<mexext>)
 *
 * Trial sequencing: conditions and repetitions, sequential, random and
 * constrained orders, interleaved adaptive tracks, blocks, practice, warmup,
 * catch and re-queued trials, tallies and a history that replays. Command
 * dispatch in the style of widmann's ppdev-mex.
 *
 *     d = struct('factors', {{{'orientation', 2}, {'contrast', 5}}}, ...
 *                'reps', 20, 'order', 'constrained', 'rng', 20260923);
 *     d.constraints = ysp_trials('max_run', 'orientation', 'any', 3);
 *     h = ysp_trials('open', d);
 *
 *     % v0.2: a conditions file, rules text, names
 *     d = struct('table', fileread('conditions.csv'), 'participant', 7, 'rng', 1, ...
 *                'rules', sprintf('order constrained\nreps 10\nmax_run target 3\n'));
 *     h = ysp_trials('open', d);
 *     row = ysp_trials('values', h, ti.condition);   % struct of the row
 *     ti = ysp_trials('next', h);
 *     while ~isempty(ti)
 *         ysp_trials('update', h, run_trial(ti.levels));
 *         ti = ysp_trials('next', h);
 *     end
 *     ysp_trials('close', h);
 *
 * Build with build.m. See README.md for the command reference.
 *
 * TRACKS. A track is {'stair', h}, {'quest', h} or {'aep', h}, a handle of one
 * of the other MEX modules, or a function handle that returns true when the
 * track is done. The header asks a track is_done() from inside ytr_next()
 * and ytr_done(). Each MEX module is a separate shared library with its own
 * handle table, so this module cannot reach ysp_stair's table in C without
 * exporting symbols across libraries, which neither MATLAB nor Octave
 * supports portably. Instead is_done() calls ysp_stair('done', h) (or quest,
 * aep) through mexCallMATLABWithTrap. That is safe because is_done() runs on
 * the interpreter thread, inside the next() or done() call the script made,
 * and it costs one MEX call per live track per trial, microseconds.
 */
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"

#define PM_MOD "ysp_trials"
#include "ysp_mex_util.h"

struct tn;
typedef struct tn_track {
    struct tn* n;
    mxArray*   fn;       /* persistent: the track's function handle, or the
                          * MEX module's (@ysp_stair)                       */
    mxArray*   h;        /* persistent: the module handle, or NULL          */
} tn_track;

typedef struct tn {
    ytr_trials t;
    int          busy;
    pm_rng       rng;
    uint8_t*     records;     /* record_size x YTR_MAX_TRIALS               */
    uint8_t*     rec_tmp;     /* one record, for update()                    */
    size_t       record_size;
    char*        names[YTR_MAX_FACTORS];
    uint64_t*    table_mem;   /* the parsed table block (desc.table)          */
    ytb_table  table;
    int*         order_list;
    double*      weights;
    int*         group_list;
    uint64_t*    rules_arena; /* ytr_rules() output, read at open            */
    char*        jit_str[YTR_MAX_JITTERS][4];   /* name and column names     */
    int64_t      jit_vals[YTR_MAX_JITTERS][YTR_MAX_JITTER_VALUES];
    tn_track     tracks[YTR_MAX_TRACKS];
    int          n_tracks;
    mxArray*     done_str;    /* persistent 'done', the command a track gets  */
} tn;

static void tn_destroy(void* obj) {
    tn* n = (tn*)obj;
    int i;
    for (i = 0; i < YTR_MAX_FACTORS; i++) free(n->names[i]);
    for (i = 0; i < n->n_tracks; i++) {
        if (n->tracks[i].fn) mxDestroyArray(n->tracks[i].fn);
        if (n->tracks[i].h) mxDestroyArray(n->tracks[i].h);
    }
    if (n->done_str) mxDestroyArray(n->done_str);
    pm_rng_free(&n->rng);
    free(n->records);
    free(n->rec_tmp);
    free(n->table_mem);
    free(n->order_list);
    free(n->weights);
    free(n->group_list);
    free(n->rules_arena);
    {
        int j, k;
        for (j = 0; j < YTR_MAX_JITTERS; j++)
            for (k = 0; k < 4; k++) free(n->jit_str[j][k]);
    }
    free(n);
}

static const char* const t_order_names[] = { "sequential", "random", "full_random", "constrained",
                                              "list", "with_replacement" };
static const char* const t_inter_names[] = { "random", "round_robin" };
static const char* const t_rule_names[]  = { "max_run", "max_in_window", "min_gap", "no_transition", "first_not",
                                              "followed_by", "preceded_by", "chunk", "balance" };
static const char* const t_gmode_names[] = { "none", "blocked", "alternate" };
static const char* const t_gorder_names[] = { "sequential", "random", "latin", "balanced_latin", "list" };

static void t_check(int rc) {
    if (rc >= 0 || rc == YTR_DONE) return;
    switch (rc) {
        case YTR_ERR_ARG:    pm_err("arg", "%s", ytr_strerror(rc)); break;
        case YTR_ERR_CLOSED: pm_err("closed", "%s", ytr_strerror(rc)); break;
        case YTR_ERR_ORDER:  pm_err("order", "%s", ytr_strerror(rc)); break;
        case YTR_ERR_FULL:   pm_err("full", "%s", ytr_strerror(rc)); break;
        default:               pm_err("error", "%s", ytr_strerror(rc)); break;
    }
}

/* ---- tracks ------------------------------------------------------------- */

static bool tn_is_done(void* ctx) {
    tn_track* tr = (tn_track*)ctx;
    mxArray* out = NULL;
    mxArray* in[3];
    int ok;
    bool done = false;
    if (tr->h) {
        in[0] = tr->fn;
        in[1] = tr->n->done_str;
        in[2] = tr->h;
        ok = pm_feval(1, &out, 3, in);
    } else {
        in[0] = tr->fn;
        ok = pm_feval(1, &out, 1, in);
    }
    /* On an error the track reads as not done; the error is raised when the
     * header call returns. */
    if (ok && out && (mxIsLogical(out) || mxIsNumeric(out)) && mxGetNumberOfElements(out) >= 1)
        done = mxGetScalar(out) != 0.0;
    if (out) mxDestroyArray(out);
    return done;
}

/* ---- constraints (module commands) ------------------------------------- */

static const char* const t_con_fields[] = { "rule", "factor", "level", "level2", "n", "window" };

/* The struct a helper returns. factor and level stay as the caller wrote them
 * (a name or 'condition', 'any') and are resolved at open, where the factor
 * names are known. */
static mxArray* t_con_make(int rule, const mxArray* factor, const mxArray* level,
                           const mxArray* level2, double nval, double window) {
    mxArray* s = mxCreateStructMatrix(1, 1, 6, (const char**)t_con_fields);
    mxSetField(s, 0, "rule", mxCreateString(t_rule_names[rule]));
    mxSetField(s, 0, "factor", mxDuplicateArray(factor));
    mxSetField(s, 0, "level", mxDuplicateArray(level));
    mxSetField(s, 0, "level2", level2 ? mxDuplicateArray(level2) : mxCreateDoubleMatrix(0, 0, mxREAL));
    mxSetField(s, 0, "n", mxCreateDoubleScalar(nval));
    mxSetField(s, 0, "window", mxCreateDoubleScalar(window));
    return s;
}

/* Factor: a 1-based index, a factor or table column name, or 'condition'
 * for the row. */
static int t_factor(const mxArray* a, const ytr_desc* d, tn* n) {
    int i;
    if (mxIsChar(a)) {
        char* v = mxArrayToString(a);
        if (pm_lower_eq(v, "condition")) return YTR_CONDITION;
        if (d->table) {
            i = ytb_col(d->table, v);
            if (i >= 0) return i;
        }
        for (i = 0; i < d->n_factors; i++)
            if (n->names[i] && strcmp(n->names[i], v) == 0) return i;
        pm_err("arg", "constraint names unknown factor '%s'", v);
    }
    i = pm_int(a, "constraint factor");
    if (i < 1) pm_err("arg", "constraint factor must be >= 1, a name, or 'condition'");
    return i - 1;
}

/* Level: a 1-based level number, 'any', or with a table a column value
 * (text, or a number looked up by value when given as a string). */
static int t_level(const mxArray* a, const ytr_desc* d, int factor) {
    int i;
    if (mxIsChar(a)) {
        char* v = mxArrayToString(a);
        if (pm_lower_eq(v, "any")) return YTR_ANY_LEVEL;
        if (d->table && factor >= 0) {
            i = ytb_find(d->table, factor, v);
            if (i < 0) pm_err("arg", "column '%s' has no level '%s'", ytb_col_name(d->table, factor), v);
            return i;
        }
        pm_err("arg", "a constraint level is a 1-based index, 'any', or a table value");
    }
    i = pm_int(a, "constraint level");
    if (i < 1) pm_err("arg", "constraint level must be >= 1 or 'any'");
    return i - 1;
}

static void t_constraint(const mxArray* s, size_t e, const ytr_desc* d, tn* n, ytr_constraint* c) {
    const mxArray* f;
    memset(c, 0, sizeof(*c));
    if (!(f = mxGetField(s, e, "rule"))) pm_err("arg", "a constraint needs a rule; build it with ysp_trials('max_run', ...)");
    c->rule = (ytr_rule)pm_enum(f, "rule", t_rule_names, 9);
    if (!(f = mxGetField(s, e, "factor"))) pm_err("arg", "a constraint needs a factor");
    c->factor = t_factor(f, d, n);
    if (c->rule != YTR_RULE_CHUNK && c->rule != YTR_RULE_BALANCE) {
        if (!(f = mxGetField(s, e, "level"))) pm_err("arg", "a constraint needs a level");
        c->level = t_level(f, d, c->factor);
        if ((f = mxGetField(s, e, "level2")) && !mxIsEmpty(f)) c->level2 = t_level(f, d, c->factor);
    }
    if ((f = mxGetField(s, e, "n")) && !mxIsEmpty(f)) c->n = pm_int(f, "n");
    if ((f = mxGetField(s, e, "window")) && !mxIsEmpty(f)) c->window = pm_int(f, "window");
}

/* ---- desc --------------------------------------------------------------- */

static const char* const t_fields[] = {
    "n_conditions", "factors", "reps", "cond_reps", "order", "constraints",
    "max_swaps", "tracks", "track_weights", "interleave", "track_rate",
    "block_size", "constraints_span_blocks", "n_practice", "n_warmup",
    "warmup_conditions", "requeue_gap", "rng", "record_size",
    "table", "order_list", "draws", "weights", "subset", "groups", "rules", "participant",
    "jitters"
};

/* ---- jitter ------------------------------------------------------------- */

static const char* const t_jit_dists[] = { "uniform", "choice", "exponential" };
static const char* t_jit_fields[] = { "name", "dist", "lo", "hi", "scale", "values", "rate" };

/* A malloc'd copy of a char argument, or NULL when empty. */
static char* t_dup(const mxArray* a, const char* what) {
    char* v = pm_string(a, what);
    size_t n = strlen(v);
    char* c = (char*)malloc(n + 1);
    if (!c) pm_err("memory", "out of memory");
    memcpy(c, v, n + 1);
    return c;
}

/* Element e of a jitter struct array into j. Strings go to str[4] (name,
 * lo, hi, scale column; malloc'd, the caller frees), values to vals. The
 * struct holds seconds; the header takes nanoseconds since v0.3.0, and
 * ytr__s_ns() converts the way v0.2.1 read seconds (-1 ns, which the header
 * refuses by name, for a value outside [0, 1e6] s). */
static void t_jitter_desc(const mxArray* s, size_t e, ytr_jitter_desc* j, char** str, int64_t* vals) {
    const mxArray* f;
    int k;
    int64_t* num[3];
    const char** col[3];
    static const char* const keys[3] = { "lo", "hi", "scale" };
    memset(j, 0, sizeof(*j));
    if (!mxIsStruct(s)) pm_err("arg", "a jitter is a struct from ysp_trials('uniform', ...), 'choice' or 'exponential'");
    if ((f = mxGetField(s, e, "name")) && !mxIsEmpty(f)) { str[0] = t_dup(f, "jitter name"); j->name = str[0]; }
    if (!(f = mxGetField(s, e, "dist"))) pm_err("arg", "a jitter needs a dist");
    j->dist = (ytr_jitter_dist)pm_enum(f, "jitter dist", t_jit_dists, 3);
    num[0] = &j->lo_ns; num[1] = &j->hi_ns; num[2] = &j->scale_ns;
    col[0] = &j->lo_column; col[1] = &j->hi_column; col[2] = &j->scale_column;
    for (k = 0; k < 3; k++) {
        f = mxGetField(s, e, keys[k]);
        if (!f || mxIsEmpty(f)) continue;
        if (mxIsChar(f)) { str[1 + k] = t_dup(f, keys[k]); *col[k] = str[1 + k]; }
        else *num[k] = ytr__s_ns(pm_scalar(f, keys[k]));
    }
    if ((f = mxGetField(s, e, "values")) && !mxIsEmpty(f)) {
        int m = (int)mxGetNumberOfElements(f);
        if (m > YTR_MAX_JITTER_VALUES) pm_err("arg", "a choice takes at most %d values", YTR_MAX_JITTER_VALUES);
        {
            double sv[YTR_MAX_JITTER_VALUES];
            int i;
            pm_vector(f, "values", sv, m);
            for (i = 0; i < m; i++) vals[i] = ytr__s_ns(sv[i]);
        }
        j->values_ns = vals;
        j->n_values = m;
    }
    if ((f = mxGetField(s, e, "rate")) && !mxIsEmpty(f)) {
        double r[2] = { 0.0, 1.0 };
        int m = (int)mxGetNumberOfElements(f);
        if (m < 1 || m > 2) pm_err("arg", "rate is NUM or [NUM DEN]");
        pm_vector(f, "rate", r, m);
        if (r[0] < 1 || r[1] < 1 || r[0] != floor(r[0]) || r[1] != floor(r[1]) || r[0] > 2147483647.0 || r[1] > 2147483647.0)
            pm_err("arg", "rate needs positive whole NUM and DEN");
        j->rate_num = (int)r[0];
        j->rate_den = (int)r[1];
    }
}

static mxArray* t_jitter_out(ytr_jitter_value v) {
    static const char* fields[] = { "s", "ns", "frames" };
    mxArray* s = mxCreateStructMatrix(1, 1, 3, fields);
    mxArray* ns = mxCreateNumericMatrix(1, 1, mxINT64_CLASS, mxREAL);
    static volatile double billion = 1e9;   /* .s as the header's v0.2.1 .s */
    *(int64_t*)mxGetData(ns) = v.ns;
    mxSetField(s, 0, "s", mxCreateDoubleScalar(v.ns < 0 ? mxGetNaN() : (double)v.ns / billion));
    mxSetField(s, 0, "ns", ns);
    mxSetField(s, 0, "frames", mxCreateDoubleScalar((double)v.frames));
    return s;
}

/* ysp_trials('uniform', name, lo, hi [, rate]) and friends: a jitter struct. */
static mxArray* t_jitter_make(int dist, const mxArray* name, const mxArray* lo, const mxArray* hi,
                              const mxArray* scale, const mxArray* values, const mxArray* rate) {
    mxArray* s = mxCreateStructMatrix(1, 1, 7, t_jit_fields);
    mxSetField(s, 0, "name", mxDuplicateArray(name));
    mxSetField(s, 0, "dist", mxCreateString(t_jit_dists[dist]));
    if (lo) mxSetField(s, 0, "lo", mxDuplicateArray(lo));
    if (hi) mxSetField(s, 0, "hi", mxDuplicateArray(hi));
    if (scale) mxSetField(s, 0, "scale", mxDuplicateArray(scale));
    if (values) mxSetField(s, 0, "values", mxDuplicateArray(values));
    if (rate) mxSetField(s, 0, "rate", mxDuplicateArray(rate));
    return s;
}

/* Parse CSV text (char) or struct('csv', text, 'types', struct(name, type),
 * 'delimiter', ',', 'allow_empty', false) into a block the caller frees,
 * viewed in *out. Temporary memory is mxMalloc'd, freed on an error. */
static uint64_t* t_parse_table(const mxArray* a, ytb_table* out) {
    static const char* const tfields[] = { "csv", "types", "delimiter", "allow_empty" };
    static const char* const tnames[] = { "auto", "integer", "number", "string" };
    ytb_csv_desc cd;
    ytb_table t;
    const mxArray* f = a;
    char* text;
    uint64_t *arena, *mem;
    memset(&cd, 0, sizeof(cd));
    if (mxIsStruct(a)) {
        pm_check_fields(a, tfields, 4, "table");
        if (!(f = mxGetField(a, 0, "csv"))) pm_err("arg", "table needs csv");
        {
            const mxArray* ty = mxGetField(a, 0, "types");
            if (ty && !mxIsEmpty(ty)) {
                int k, nk;
                if (!mxIsStruct(ty)) pm_err("arg", "table.types must be a struct of column name to type");
                nk = mxGetNumberOfFields(ty);
                if (nk > YTB_MAX_COLUMNS) pm_err("arg", "too many table.types");
                for (k = 0; k < nk; k++) {
                    cd.types[k].name = mxGetFieldNameByNumber(ty, k);
                    cd.types[k].type = (ytb_type)pm_enum(mxGetFieldByNumber(ty, 0, k), "type", tnames, 4);
                }
                cd.n_types = nk;
            }
            ty = mxGetField(a, 0, "delimiter");
            if (ty && !mxIsEmpty(ty)) {
                char* dl = pm_string(ty, "delimiter");
                if (strlen(dl) != 1) pm_err("arg", "table.delimiter is one character");
                cd.delimiter = dl[0];
            }
            ty = mxGetField(a, 0, "allow_empty");
            if (ty && !mxIsEmpty(ty)) cd.allow_empty = pm_bool(ty, "allow_empty") != 0;
        }
    }
    if (!mxIsChar(f)) pm_err("arg", "desc.table is CSV text or struct('csv', text, ...)");
    /* HAVE_OCTAVE as ysp_mex_util.h uses it: OCTMEX_API exists only in newer
     * Octave (10.1 has it, CI's 7.3 does not), and mxArrayToUTF8String is
     * MATLAB's alone. */
#if defined(HAVE_OCTAVE) || defined(OCTMEX_API)
    text = mxArrayToString(f);           /* Octave's char arrays hold UTF-8 */
#else
    text = mxArrayToUTF8String(f);       /* MATLAB's hold UTF-16          */
#endif
    if (!text) pm_err("arg", "the CSV text is not convertible to UTF-8");
    cd.text = text;
    cd.len = strlen(text);
    ytb_open_csv(&t, &cd);                       /* sizing */
    if (t.need == 0) pm_err("table", "%s", ytb_error(&t));
    arena = (uint64_t*)mxMalloc((t.need + 7) & ~(size_t)7);
    cd.arena = arena;
    cd.arena_size = t.need;
    if (!ytb_open_csv(&t, &cd)) pm_err("table", "%s", ytb_error(&t));
    mem = (uint64_t*)malloc((t.size + 7) & ~(size_t)7);
    if (!mem) pm_err("memory", "out of memory");
    memcpy(mem, t.base, t.size);
    mxFree(arena);
    if (!ytb_open_view(out, mem, t.size)) { free(mem); pm_err("table", "%s", ytb_error(out)); }
    return mem;
}

static int t_rows(const ytr_desc* d) {
    int rows = d->table ? d->table->n_rows : d->n_conditions, f;
    if (rows == 0 && d->n_factors > 0) {
        rows = 1;
        for (f = 0; f < d->n_factors; f++) rows *= d->factors[f].n_levels > 0 ? d->factors[f].n_levels : 1;
    }
    return rows;
}

/* Reads the desc into d and fills the node's owned copies (factor names,
 * tracks, records). Everything that can raise on a bad argument runs before
 * the first malloc into the node, or leaves only what tn_destroy frees. */
static void t_read_desc(const mxArray* s, ytr_desc* d, tn* n, int** cond_reps, int** warm) {
    const mxArray* f;
    int i;
    pm_check_fields(s, t_fields, (int)(sizeof(t_fields) / sizeof(t_fields[0])), "desc");
    if ((f = pm_field(s, "table"))) {
        n->table_mem = t_parse_table(f, &n->table);
        d->table = &n->table;
    }
    if ((f = pm_field(s, "n_conditions"))) d->n_conditions = pm_int(f, "n_conditions");
    if ((f = pm_field(s, "factors"))) {
        /* A cell of {name, n_levels} pairs, or an n x 2 cell. */
        int nf;
        int two_col = mxIsCell(f) && mxGetN(f) == 2 && mxGetM(f) >= 1 && !mxIsCell(mxGetCell(f, 0));
        if (!mxIsCell(f)) pm_err("arg", "desc.factors must be a cell of {name, n_levels} pairs");
        nf = two_col ? (int)mxGetM(f) : (int)mxGetNumberOfElements(f);
        if (nf > YTR_MAX_FACTORS) pm_err("arg", "at most %d factors", YTR_MAX_FACTORS);
        for (i = 0; i < nf; i++) {
            const mxArray *nm, *nl;
            if (two_col) {
                nm = mxGetCell(f, (size_t)i);
                nl = mxGetCell(f, (size_t)(i + nf));
            } else {
                const mxArray* pair = mxGetCell(f, (size_t)i);
                if (!pair || !mxIsCell(pair) || mxGetNumberOfElements(pair) != 2)
                    pm_err("arg", "desc.factors{%d} must be {name, n_levels}", i + 1);
                nm = mxGetCell(pair, 0);
                nl = mxGetCell(pair, 1);
            }
            d->factors[i].n_levels = pm_int(nl, "n_levels");
            if (nm && !mxIsEmpty(nm)) {
                char* v = pm_string(nm, "factor name");
                n->names[i] = (char*)malloc(strlen(v) + 1);
                if (!n->names[i]) pm_err("memory", "out of memory");
                strcpy(n->names[i], v);
            }
            d->factors[i].name = n->names[i];
        }
        d->n_factors = nf;
    }
    if ((f = pm_field(s, "reps"))) d->reps = pm_int(f, "reps");
    if ((f = pm_field(s, "cond_reps"))) {
        int m = (int)mxGetNumberOfElements(f);
        double* v = (double*)mxMalloc((size_t)m * sizeof(double));
        int* c = (int*)mxMalloc((size_t)m * sizeof(int));
        pm_vector(f, "cond_reps", v, m);
        for (i = 0; i < m; i++) c[i] = (int)v[i];
        *cond_reps = c;
        d->cond_reps = c;
    }
    if ((f = pm_field(s, "order")))     d->order = (ytr_order)pm_enum(f, "order", t_order_names, 6);
    if ((f = pm_field(s, "max_swaps"))) d->max_swaps = pm_int(f, "max_swaps");
    if ((f = pm_field(s, "constraints"))) {
        /* A struct array from the helpers, or a cell of them. */
        size_t m = mxGetNumberOfElements(f), e;
        if (m > YTR_MAX_CONSTRAINTS) pm_err("arg", "at most %d constraints", YTR_MAX_CONSTRAINTS);
        for (e = 0; e < m; e++) {
            if (mxIsStruct(f)) t_constraint(f, e, d, n, &d->constraints[e]);
            else if (mxIsCell(f) && mxIsStruct(mxGetCell(f, e))) t_constraint(mxGetCell(f, e), 0, d, n, &d->constraints[e]);
            else pm_err("arg", "desc.constraints must be structs from the constraint helpers");
        }
        d->n_constraints = (int)m;
    }
    if ((f = pm_field(s, "tracks"))) {
        int m;
        if (!mxIsCell(f)) f = NULL;
        if (!f) pm_err("arg", "desc.tracks must be a cell of {'stair'|'quest'|'aep', handle} or function handles");
        m = (int)mxGetNumberOfElements(f);
        if (m > YTR_MAX_TRACKS) pm_err("arg", "at most %d tracks", YTR_MAX_TRACKS);
        /* Validate them all first; persistent copies come after. */
        for (i = 0; i < m; i++) {
            const mxArray* tr = mxGetCell(f, (size_t)i);
            if (tr && mxIsClass(tr, "function_handle")) continue;
            if (!tr || !mxIsCell(tr) || mxGetNumberOfElements(tr) != 2 || !mxIsChar(mxGetCell(tr, 0)))
                pm_err("arg", "desc.tracks{%d} must be {'stair'|'quest'|'aep', handle} or a function handle", i + 1);
            {
                char* mod = mxArrayToString(mxGetCell(tr, 0));
                if (!pm_lower_eq(mod, "stair") && !pm_lower_eq(mod, "quest") && !pm_lower_eq(mod, "aep") &&
                    !pm_lower_eq(mod, "ysp_stair") && !pm_lower_eq(mod, "ysp_quest") && !pm_lower_eq(mod, "ysp_aep"))
                    pm_err("arg", "desc.tracks{%d}: unknown module '%s'", i + 1, mod);
            }
        }
        n->done_str = mxCreateString("done");
        mexMakeArrayPersistent(n->done_str);
        for (i = 0; i < m; i++) {
            const mxArray* tr = mxGetCell(f, (size_t)i);
            tn_track* t = &n->tracks[i];
            t->n = n;
            if (mxIsClass(tr, "function_handle")) {
                t->fn = mxDuplicateArray(tr);
            } else {
                char* mod = mxArrayToString(mxGetCell(tr, 0));
                char full[16];
                snprintf(full, sizeof(full), "%s%s", strncmp(mod, "ysp_", 4) == 0 ? "" : "ysp_", mod);
                {
                    /* A function handle, not a name: feval and Octave's
                     * cellfun wrapper both take one. */
                    mxArray* nm = mxCreateString(full);
                    mexCallMATLAB(1, &t->fn, 1, &nm, "str2func");
                    mxDestroyArray(nm);
                }
                t->h = mxDuplicateArray(mxGetCell(tr, 1));
                mexMakeArrayPersistent(t->h);
            }
            mexMakeArrayPersistent(t->fn);
            n->n_tracks = i + 1;
            d->tracks[i] = ytr_track(t, tn_is_done);
        }
        d->n_tracks = m;
    }
    if ((f = pm_field(s, "track_weights"))) {
        double w[YTR_MAX_TRACKS];
        pm_vector_n(f, "track_weights", w, d->n_tracks);
        for (i = 0; i < d->n_tracks; i++) d->tracks[i].weight = w[i];
    }
    if ((f = pm_field(s, "interleave")))  d->interleave = (ytr_interleave)pm_enum(f, "interleave", t_inter_names, 2);
    if ((f = pm_field(s, "track_rate")))  d->track_rate = pm_scalar(f, "track_rate");
    if ((f = pm_field(s, "block_size")))  d->block_size = pm_int(f, "block_size");
    if ((f = pm_field(s, "constraints_span_blocks"))) d->constraints_span_blocks = pm_bool(f, "constraints_span_blocks") != 0;
    if ((f = pm_field(s, "n_practice")))  d->n_practice = pm_int(f, "n_practice");
    if ((f = pm_field(s, "n_warmup")))    d->n_warmup = pm_int(f, "n_warmup");
    if ((f = pm_field(s, "warmup_conditions"))) {
        int m = (int)mxGetNumberOfElements(f);
        double* v = (double*)mxMalloc((size_t)m * sizeof(double));
        int* c = (int*)mxMalloc((size_t)m * sizeof(int));
        pm_vector(f, "warmup_conditions", v, m);
        for (i = 0; i < m; i++) {
            if (v[i] != floor(v[i]) || v[i] < 1) pm_err("arg", "warmup_conditions are 1-based condition indices");
            c[i] = (int)v[i] - 1;
        }
        *warm = c;
        d->warmup_conditions = c;
        d->n_warmup_conditions = m;
    }
    if ((f = pm_field(s, "requeue_gap"))) d->requeue_gap = pm_int(f, "requeue_gap");
    if ((f = pm_field(s, "order_list"))) {
        int m = (int)mxGetNumberOfElements(f);
        double* v = (double*)mxMalloc((size_t)(m > 0 ? m : 1) * sizeof(double));
        pm_vector(f, "order_list", v, m);
        n->order_list = (int*)malloc((size_t)(m > 0 ? m : 1) * sizeof(int));
        if (!n->order_list) pm_err("memory", "out of memory");
        for (i = 0; i < m; i++) {
            if (v[i] != floor(v[i]) || v[i] < 1) pm_err("arg", "order_list holds 1-based condition indices");
            n->order_list[i] = (int)v[i] - 1;
        }
        d->order_list = n->order_list;
        d->n_order_list = m;
    }
    if ((f = pm_field(s, "draws")))  d->draws = pm_int(f, "draws");
    if ((f = pm_field(s, "subset"))) d->subset = pm_int(f, "subset");
    if ((f = pm_field(s, "weights"))) {
        int m = (int)mxGetNumberOfElements(f), rows = t_rows(d);
        if (m != rows) pm_err("arg", "weights has %d entries, the design has %d conditions", m, rows);
        n->weights = (double*)malloc((size_t)(m > 0 ? m : 1) * sizeof(double));
        if (!n->weights) pm_err("memory", "out of memory");
        pm_vector(f, "weights", n->weights, m);
        d->weights = n->weights;
    }
    if ((f = pm_field(s, "participant"))) d->groups.participant = pm_int(f, "participant");
    if ((f = pm_field(s, "groups"))) {
        /* struct('factor', name or 1-based, 'mode', 'blocked', 'order',
         * 'sequential', 'participant', p, 'list', {values} or 1-based levels) */
        static const char* const gfields[] = { "factor", "mode", "order", "participant", "list" };
        const mxArray* g;
        if (!mxIsStruct(f)) pm_err("arg", "desc.groups must be a struct");
        pm_check_fields(f, gfields, 5, "groups");
        if (!(g = mxGetField(f, 0, "factor"))) pm_err("arg", "groups needs a factor");
        d->groups.factor = t_factor(g, d, n);
        d->groups.mode = YTR_GROUPS_BLOCKED;
        if ((g = mxGetField(f, 0, "mode")) && !mxIsEmpty(g))
            d->groups.mode = (ytr_group_mode)pm_enum(g, "groups.mode", t_gmode_names, 3);
        if ((g = mxGetField(f, 0, "order")) && !mxIsEmpty(g))
            d->groups.order = (ytr_group_order)pm_enum(g, "groups.order", t_gorder_names, 5);
        if ((g = mxGetField(f, 0, "participant")) && !mxIsEmpty(g))
            d->groups.participant = pm_int(g, "groups.participant");
        if ((g = mxGetField(f, 0, "list")) && !mxIsEmpty(g)) {
            int m = (int)mxGetNumberOfElements(g), k;
            n->group_list = (int*)malloc((size_t)(m > 0 ? m : 1) * sizeof(int));
            if (!n->group_list) pm_err("memory", "out of memory");
            for (k = 0; k < m; k++) {
                if (mxIsCell(g)) n->group_list[k] = t_level(mxGetCell(g, (size_t)k), d, d->groups.factor);
                else {
                    double v = mxGetPr(g)[k];
                    if (v != floor(v) || v < 1) pm_err("arg", "groups.list holds 1-based levels or a cell of values");
                    n->group_list[k] = (int)v - 1;
                }
            }
            d->groups.list = n->group_list;
            d->groups.n_list = m;
        }
    }
    if ((f = pm_field(s, "record_size"))) {
        double r = pm_scalar(f, "record_size");
        if (r < 0 || r != floor(r) || r > 65536) pm_err("arg", "record_size must be an integer in 0..65536");
        n->record_size = (size_t)r;
    }
    if ((f = pm_field(s, "rng"))) {
        pm_rng_parse(f, &n->rng);
        d->rng = pm_rng_call;
        d->rng_ctx = &n->rng;
    }
    if ((f = pm_field(s, "jitters")) && !mxIsEmpty(f)) {
        size_t m = mxGetNumberOfElements(f), e;
        if (!mxIsStruct(f)) pm_err("arg", "desc.jitters must be a struct array (ysp_trials('uniform', ...))");
        if (m > YTR_MAX_JITTERS) pm_err("arg", "desc.jitters holds %d; the maximum is %d", (int)m, YTR_MAX_JITTERS);
        for (e = 0; e < m; e++) t_jitter_desc(f, e, &d->jitters[e], n->jit_str[e], n->jit_vals[e]);
        d->n_jitters = (int)m;
    }
    if ((f = pm_field(s, "rules"))) {
        /* Rules text over the fields above (ytr_rules); its arrays live in
         * the node until open() has read them. */
        ytr_rules_desc rd;
        char err[512];
        char* txt = pm_string(f, "rules");
        int rows = t_rows(d);
        size_t sz = (size_t)4 * YTR_MAX_TRIALS + (size_t)32 * (size_t)(rows > 0 ? rows : YTR_MAX_CONDITIONS) + 1024 +
                    (size_t)320 * YTR_MAX_JITTERS;
        n->rules_arena = (uint64_t*)malloc(sz);
        if (!n->rules_arena) pm_err("memory", "out of memory");
        memset(&rd, 0, sizeof(rd));
        rd.text = txt;
        rd.len = strlen(txt);
        rd.table = d->table;
        rd.participant = d->groups.participant;
        rd.arena = n->rules_arena;
        rd.arena_size = sz;
        if (ytr_rules(d, &rd, err, sizeof(err)) != 0) pm_err("rules", "%s", err);
    }
    if (n->record_size) {
        n->records = (uint8_t*)calloc(YTR_MAX_TRIALS, n->record_size);
        n->rec_tmp = (uint8_t*)calloc(1, n->record_size);
        if (!n->records || !n->rec_tmp) pm_err("memory", "out of memory");
        d->records = n->records;
        d->record_size = n->record_size;
    }
}

/* The node is registered before the desc is read, so an error in the middle
 * of parsing leaves nothing that the handle table would not free: the node
 * is found by pm_cleanup, and unregistered again here on the normal paths. */
static tn* t_make(const mxArray* s, const uint8_t* bytes, size_t len) {
    ytr_desc d;
    int* cond_reps = NULL;
    int* warm = NULL;
    tn* n;
    bool ok;
    memset(&d, 0, sizeof(d));
    n = (tn*)calloc(1, sizeof(*n));
    if (!n) pm_err("memory", "out of memory");
    pm_register(n);
    t_read_desc(s, &d, n, &cond_reps, &warm);
    n->busy = 1;
    ok = bytes ? ytr_open_saved(&n->t, &d, bytes, len) : ytr_open(&n->t, &d);
    n->busy = 0;
    free(n->rules_arena);
    n->rules_arena = NULL;
    pm_unregister(n);
    if (!ok || pm_pending) {
        char msg[300];
        snprintf(msg, sizeof(msg), "%s", ok ? "" : ytr_error(&n->t));
        tn_destroy(n);
        pm_rethrow();
        pm_err(bytes ? "load" : "open", "%s", msg);
    }
    return n;
}

/* ---- outputs ------------------------------------------------------------ */

static mxArray* t_info(const tn* n, const ytr_trial_info* ti) {
    static const char* fields[] = { "index", "condition", "track", "rep", "block", "levels",
                                    "first_in_block", "after_break", "practice", "warmup", "requeued" };
    mxArray* s = mxCreateStructMatrix(1, 1, 11, fields);
    int nf = ytr_n_factors(&n->t), i;
    mxArray* lv = mxCreateDoubleMatrix(1, (size_t)(ti->condition >= 0 ? nf : 0), mxREAL);
    if (ti->condition >= 0)
        for (i = 0; i < nf; i++) mxGetPr(lv)[i] = ytr_level(&n->t, ti->condition, i) + 1.0;
    mxSetField(s, 0, "index", mxCreateDoubleScalar(ti->index + 1.0));
    mxSetField(s, 0, "condition", mxCreateDoubleScalar(ti->condition + 1.0));
    mxSetField(s, 0, "track", mxCreateDoubleScalar(ti->track + 1.0));
    mxSetField(s, 0, "rep", mxCreateDoubleScalar(ti->rep + 1.0));
    mxSetField(s, 0, "block", mxCreateDoubleScalar(ti->block + 1.0));
    mxSetField(s, 0, "levels", lv);
    mxSetField(s, 0, "first_in_block", mxCreateLogicalScalar(ti->first_in_block));
    mxSetField(s, 0, "after_break", mxCreateLogicalScalar(ti->after_break));
    mxSetField(s, 0, "practice", mxCreateLogicalScalar(ti->practice));
    mxSetField(s, 0, "warmup", mxCreateLogicalScalar(ti->warmup));
    mxSetField(s, 0, "requeued", mxCreateLogicalScalar(ti->requeued));
    return s;
}

static mxArray* t_history(const ytr_trials* t) {
    static const char* fields[] = { "condition", "track", "rep", "block", "outcome", "flags",
                                    "practice", "requeued", "done", "warmup", "after_break",
                                    "first_in_block", "violation", "leadin" };
    static const int bits[] = { YTR_FLAG_PRACTICE, YTR_FLAG_REQUEUED, YTR_FLAG_DONE,
                                YTR_FLAG_WARMUP, YTR_FLAG_AFTER_BREAK,
                                YTR_FLAG_FIRST_IN_BLOCK, YTR_FLAG_VIOLATION, YTR_FLAG_LEADIN };
    int n = 0, i, b;
    const ytr_trial* h = ytr_history(t, &n);
    mxArray* s = mxCreateStructMatrix(1, 1, 14, fields);
    mxArray* col[6];
    mxArray* flag[8];
    for (i = 0; i < 6; i++) col[i] = mxCreateDoubleMatrix((size_t)n, 1, mxREAL);
    for (b = 0; b < 8; b++) flag[b] = mxCreateLogicalMatrix((size_t)n, 1);
    for (i = 0; i < n; i++) {
        mxGetPr(col[0])[i] = h[i].condition + 1.0;   /* 0 = a track trial     */
        mxGetPr(col[1])[i] = h[i].track + 1.0;       /* 0 = a condition trial */
        mxGetPr(col[2])[i] = h[i].rep + 1.0;         /* 0 = none              */
        mxGetPr(col[3])[i] = h[i].block + 1.0;       /* 0 = practice          */
        mxGetPr(col[4])[i] = h[i].outcome;           /* a value, not an index */
        mxGetPr(col[5])[i] = h[i].flags;
        for (b = 0; b < 8; b++) mxGetLogicals(flag[b])[i] = (h[i].flags & bits[b]) != 0;
    }
    for (i = 0; i < 6; i++) mxSetField(s, 0, fields[i], col[i]);
    for (b = 0; b < 8; b++) mxSetField(s, 0, fields[6 + b], flag[b]);
    return s;
}

/* snprintf-style formatter into an exact-size MATLAB string. */
typedef int (*t_fmt_fn)(const ytr_trials*, int, char*, size_t);
static int t_fmt_header(const ytr_trials* t, int i, char* b, size_t c) { (void)i; return ytr_format_header(t, b, c); }
static int t_fmt_meta(const ytr_trials* t, int i, char* b, size_t c) { (void)i; return ytr_format_meta(t, b, c); }
static int t_fmt_rules(const ytr_trials* t, int i, char* b, size_t c) { (void)i; return ytr_format_rules(t, b, c); }

/* A table's cell as a MATLAB value: double for numbers, char for text. */
static mxArray* t_cell(const ytb_table* tb, int r, int c) {
    if (ytb_col_type(tb, c) == YTB_STRING) return mxCreateString(ytb_text(tb, r, c));
    return mxCreateDoubleScalar(ytb_num(tb, r, c));
}

static mxArray* t_row_struct(const ytb_table* tb, int r) {
    const char* names[YTB_MAX_COLUMNS];
    mxArray* s;
    int c;
    for (c = 0; c < tb->n_cols; c++) names[c] = ytb_col_name(tb, c);
    s = mxCreateStructMatrix(1, 1, tb->n_cols, names);
    for (c = 0; c < tb->n_cols; c++) mxSetFieldByNumber(s, 0, c, t_cell(tb, r, c));
    return s;
}

/* Everything about a table, for inspection: columns, types, levels, values. */
static mxArray* t_table_info(const ytb_table* tb) {
    static const char* fields[] = { "columns", "types", "n_rows", "n_skipped", "levels", "values", "hash" };
    mxArray* s = mxCreateStructMatrix(1, 1, 7, fields);
    mxArray *cols = mxCreateCellMatrix(1, (size_t)tb->n_cols), *types = mxCreateCellMatrix(1, (size_t)tb->n_cols);
    mxArray *levels = mxCreateCellMatrix(1, (size_t)tb->n_cols);
    mxArray* vals = mxCreateCellMatrix((size_t)tb->n_rows, (size_t)tb->n_cols);
    char hx[20];
    int c, r, l;
    for (c = 0; c < tb->n_cols; c++) {
        int nl = ytb_n_levels(tb, c);
        mxArray* lv = mxCreateCellMatrix(1, (size_t)nl);
        mxSetCell(cols, (size_t)c, mxCreateString(ytb_col_name(tb, c)));
        mxSetCell(types, (size_t)c, mxCreateString(ytb_type_name(ytb_col_type(tb, c))));
        for (l = 0; l < nl; l++) mxSetCell(lv, (size_t)l, mxCreateString(ytb_level_text(tb, c, l)));
        mxSetCell(levels, (size_t)c, lv);
        for (r = 0; r < tb->n_rows; r++)
            mxSetCell(vals, (size_t)r + (size_t)c * (size_t)tb->n_rows, t_cell(tb, r, c));
    }
    snprintf(hx, sizeof(hx), "%016llx", (unsigned long long)ytb_hash(tb));
    mxSetField(s, 0, "columns", cols);
    mxSetField(s, 0, "types", types);
    mxSetField(s, 0, "n_rows", mxCreateDoubleScalar(tb->n_rows));
    mxSetField(s, 0, "n_skipped", mxCreateDoubleScalar(tb->n_skipped));
    mxSetField(s, 0, "levels", levels);
    mxSetField(s, 0, "values", vals);
    mxSetField(s, 0, "hash", mxCreateString(hx));
    return s;
}

static mxArray* t_format(const ytr_trials* t, t_fmt_fn fn, int i) {
    char small[512];
    char* buf;
    int len = fn(t, i, small, sizeof(small));
    t_check(len);
    if ((size_t)len < sizeof(small)) return mxCreateString(small);
    buf = (char*)mxMalloc((size_t)len + 1);
    fn(t, i, buf, (size_t)len + 1);
    return mxCreateString(buf);
}

/* A record: the raw bytes of any numeric array, at most record_size of them,
 * zero-padded. So a double scalar is an 8-byte record, and typecast() gets it
 * back from 'record'. */
static const void* t_record_arg(tn* n, const mxArray* a) {
    size_t bytes;
    if (!n->record_size || !a || mxIsEmpty(a)) return NULL;
    if ((!mxIsNumeric(a) && !mxIsLogical(a) && !mxIsChar(a)) || mxIsComplex(a))
        pm_err("arg", "a record must be a real numeric array");
    bytes = mxGetNumberOfElements(a) * mxGetElementSize(a);
    if (bytes > n->record_size)
        pm_err("arg", "a record is at most record_size (%d) bytes, got %d", (int)n->record_size, (int)bytes);
    memset(n->rec_tmp, 0, n->record_size);
    memcpy(n->rec_tmp, mxGetData(a), bytes);
    return n->rec_tmp;
}

/* ---- dispatch ----------------------------------------------------------- */

static const char* const t_con_usage[] = {
    "c = ysp_trials('max_run', factor, level, n)",
    "c = ysp_trials('max_in_window', factor, level, window, n)",
    "c = ysp_trials('min_gap', factor, level, gap)",
    "c = ysp_trials('no_transition', factor, from_level, to_level)",
    "c = ysp_trials('first_not', factor, level)",
    "c = ysp_trials('followed_by', factor, level, next_level)",
    "c = ysp_trials('preceded_by', factor, level, prev_level)",
    "c = ysp_trials('chunk', factor)",
    "c = ysp_trials('balance', factor [, 'no_repeat'] [, 'no_leadin'])"
};

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char* cmd;
    tn* n;
    ytr_trials* t;
    int r;
    pm_destroy = tn_destroy;
    pm_pending = NULL;
    if (nrhs < 1 || !mxIsChar(prhs[0]))
        pm_err("usage", "first argument must be a command string");
    cmd = mxArrayToString(prhs[0]);

    /* ---- module commands ---- */
    if (strcmp(cmd, "open") == 0) {
        pm_nargs(nrhs, 2, "h = ysp_trials('open', desc)");
        n = t_make(prhs[1], NULL, 0);
        plhs[0] = pm_handle_out(pm_register(n));
        return;
    }
    if (strcmp(cmd, "load") == 0) {
        size_t len;
        const uint8_t* b;
        pm_nargs(nrhs, 3, "h = ysp_trials('load', bytes, desc)");
        b = pm_bytes_in(prhs[1], "bytes", &len);
        n = t_make(prhs[2], b, len);
        plhs[0] = pm_handle_out(pm_register(n));
        return;
    }
    if (strcmp(cmd, "version") == 0) { plhs[0] = mxCreateString(ytr_version()); return; }
    if (strcmp(cmd, "strerror") == 0) {
        pm_nargs(nrhs, 2, "ysp_trials('strerror', code)");
        plhs[0] = mxCreateString(ytr_strerror(pm_int(prhs[1], "code")));
        return;
    }
    if (strcmp(cmd, "splitmix") == 0) {
        /* One step of the generator an integer seed drives: [u, state]. */
        uint64_t st;
        pm_nargs(nrhs, 2, "[u, state] = ysp_trials('splitmix', state)");
        st = pm_u64(prhs[1], "state");
        plhs[0] = mxCreateDoubleScalar(pm_splitmix(&st));
        if (nlhs >= 2) plhs[1] = pm_u64_out(st);
        return;
    }
    if (strcmp(cmd, "latin") == 0) {
        /* Row `row` (0-based, taken modulo the design's rows) of an n x n
         * square, as 1-based items; the second output is the row count. */
        int nn, row, bal = 0, rows, j;
        int* out;
        pm_nargs(nrhs, 3, "[items, rows] = ysp_trials('latin', n, row [, balanced])");
        nn = pm_int(prhs[1], "n");
        row = pm_int(prhs[2], "row");
        if (nrhs >= 4) bal = pm_bool(prhs[3], "balanced");
        if (nn < 1 || nn > YTR_MAX_CONDITIONS || row < 0) pm_err("arg", "n in [1, %d] and row >= 0", YTR_MAX_CONDITIONS);
        out = (int*)mxMalloc((size_t)nn * sizeof(int));
        rows = ytr_latin(nn, row, bal != 0, out);
        plhs[0] = mxCreateDoubleMatrix(1, (size_t)nn, mxREAL);
        for (j = 0; j < nn; j++) mxGetPr(plhs[0])[j] = out[j] + 1.0;
        if (nlhs >= 2) plhs[1] = mxCreateDoubleScalar(rows);
        return;
    }
    if (strcmp(cmd, "table") == 0) {
        ytb_table tb;
        uint64_t* mem;
        pm_nargs(nrhs, 2, "info = ysp_trials('table', csv_text_or_struct)");
        mem = t_parse_table(prhs[1], &tb);
        plhs[0] = t_table_info(&tb);
        free(mem);
        return;
    }
    if (strcmp(cmd, "balance") == 0) {
        int k, flags = 0;
        mxArray* z = mxCreateDoubleMatrix(0, 0, mxREAL);
        pm_nargs(nrhs, 2, t_con_usage[8]);
        for (k = 2; k < nrhs; k++) {
            char* o = pm_string(prhs[k], "balance option");
            if (pm_lower_eq(o, "no_repeat")) flags |= YTR_BALANCE_NO_REPEAT;
            else if (pm_lower_eq(o, "no_leadin")) flags |= YTR_BALANCE_NO_LEADIN;
            else pm_err("arg", "balance options are 'no_repeat' and 'no_leadin'");
        }
        plhs[0] = t_con_make(YTR_RULE_BALANCE, prhs[1], z, NULL, flags, 0);
        mxDestroyArray(z);
        return;
    }
    if (strcmp(cmd, "uniform") == 0) {
        pm_nargs(nrhs, 4, "j = ysp_trials('uniform', name, lo, hi [, rate])");
        plhs[0] = t_jitter_make(0, prhs[1], prhs[2], prhs[3], NULL, NULL, nrhs > 4 ? prhs[4] : NULL);
        return;
    }
    if (strcmp(cmd, "choice") == 0) {
        pm_nargs(nrhs, 3, "j = ysp_trials('choice', name, values [, rate])");
        plhs[0] = t_jitter_make(1, prhs[1], NULL, NULL, NULL, prhs[2], nrhs > 3 ? prhs[3] : NULL);
        return;
    }
    if (strcmp(cmd, "exponential") == 0) {
        pm_nargs(nrhs, 5, "j = ysp_trials('exponential', name, lo, hi, scale [, rate])");
        plhs[0] = t_jitter_make(2, prhs[1], prhs[2], prhs[3], prhs[4], NULL, nrhs > 5 ? prhs[5] : NULL);
        return;
    }
    if (strcmp(cmd, "jitter_map") == 0) {
        ytr_jitter_desc j;
        char* str[4] = { NULL, NULL, NULL, NULL };
        int64_t vals[YTR_MAX_JITTER_VALUES];
        char err[256];
        int k;
        bool ok;
        double u;
        pm_nargs(nrhs, 3, "v = ysp_trials('jitter_map', j, u)");
        u = pm_scalar(prhs[2], "u");
        t_jitter_desc(prhs[1], 0, &j, str, vals);
        ok = ytr_jitter_check(&j, err, sizeof(err)) == 0;
        if (ok) plhs[0] = t_jitter_out(ytr_jitter_map(&j, u));
        for (k = 0; k < 4; k++) free(str[k]);
        if (!ok) pm_err("arg", "%s", err);
        return;
    }
    if (strcmp(cmd, "chunk") == 0) {
        mxArray* z = mxCreateDoubleMatrix(0, 0, mxREAL);
        pm_nargs(nrhs, 2, t_con_usage[7]);
        plhs[0] = t_con_make(YTR_RULE_CHUNK, prhs[1], z, NULL, 0, 0);
        mxDestroyArray(z);
        return;
    }
    for (r = 0; r < 7; r++) {
        if (strcmp(cmd, t_rule_names[r]) == 0) {
            static const int need[] = { 4, 5, 4, 4, 3, 4, 4 };
            pm_nargs(nrhs, need[r], t_con_usage[r]);
            switch (r) {
                case YTR_RULE_MAX_RUN:
                    plhs[0] = t_con_make(r, prhs[1], prhs[2], NULL, pm_int(prhs[3], "n"), 0); break;
                case YTR_RULE_MAX_IN_WINDOW:
                    plhs[0] = t_con_make(r, prhs[1], prhs[2], NULL, pm_int(prhs[4], "n"), pm_int(prhs[3], "window")); break;
                case YTR_RULE_MIN_GAP:
                    plhs[0] = t_con_make(r, prhs[1], prhs[2], NULL, pm_int(prhs[3], "gap"), 0); break;
                case YTR_RULE_NO_TRANSITION:
                case YTR_RULE_FOLLOWED_BY:
                case YTR_RULE_PRECEDED_BY:
                    plhs[0] = t_con_make(r, prhs[1], prhs[2], prhs[3], 0, 0); break;
                default:
                    plhs[0] = t_con_make(r, prhs[1], prhs[2], NULL, 0, 0); break;
            }
            return;
        }
    }

    /* ---- handle commands ---- */
    if (nrhs < 2) pm_err("usage", "'%s' needs a handle", cmd);
    n = (tn*)pm_lookup(prhs[1]);
    t = &n->t;
    if (n->busy) pm_err("busy", "a track or rng callback may not call into the Trials it runs under");

    if (strcmp(cmd, "close") == 0) {
        pm_unregister(n);
        tn_destroy(n);
    } else if (strcmp(cmd, "next") == 0) {
        ytr_trial_info ti;
        int rc;
        n->busy = 1;
        rc = ytr_next(t, &ti);
        n->busy = 0;
        pm_rethrow();
        t_check(rc);
        plhs[0] = rc == YTR_DONE ? mxCreateDoubleMatrix(0, 0, mxREAL) : t_info(n, &ti);
    } else if (strcmp(cmd, "update") == 0) {
        pm_nargs(nrhs, 3, "ysp_trials('update', h, outcome [, record])");
        {
            int k = pm_int(prhs[2], "outcome");
            const void* rec = t_record_arg(n, nrhs >= 4 ? prhs[3] : NULL);
            t_check(ytr_update(t, k, rec));
        }
    } else if (strcmp(cmd, "requeue") == 0) {
        int rc;
        n->busy = 1;
        rc = ytr_requeue(t);
        n->busy = 0;
        pm_rethrow();
        t_check(rc);
    } else if (strcmp(cmd, "mark_break") == 0) {
        t_check(ytr_mark_break(t));
    } else if (strcmp(cmd, "done") == 0) {
        bool d;
        n->busy = 1;
        d = ytr_done(t);
        n->busy = 0;
        pm_rethrow();
        plhs[0] = mxCreateLogicalScalar(d);
    } else if (strcmp(cmd, "level") == 0) {
        int rc;
        pm_nargs(nrhs, 4, "l = ysp_trials('level', h, condition, factor)");
        rc = ytr_level(t, pm_index(prhs[2], "condition", ytr_n_conditions(t)),
                         pm_index(prhs[3], "factor", ytr_n_factors(t)));
        t_check(rc);
        plhs[0] = mxCreateDoubleScalar(rc + 1.0);
    } else if (strcmp(cmd, "levels") == 0) {
        int c, i, nf = ytr_n_factors(t);
        mxArray* out;
        pm_nargs(nrhs, 3, "l = ysp_trials('levels', h, condition)");
        c = pm_index(prhs[2], "condition", ytr_n_conditions(t));
        out = mxCreateDoubleMatrix(1, (size_t)nf, mxREAL);
        for (i = 0; i < nf; i++) mxGetPr(out)[i] = ytr_level(t, c, i) + 1.0;
        plhs[0] = out;
    } else if (strcmp(cmd, "condition_from_levels") == 0) {
        double v[YTR_MAX_FACTORS];
        int lv[YTR_MAX_FACTORS], i, nf = ytr_n_factors(t), rc;
        pm_nargs(nrhs, 3, "c = ysp_trials('condition_from_levels', h, levels)");
        pm_vector_n(prhs[2], "levels", v, nf);
        for (i = 0; i < nf; i++) {
            if (v[i] != floor(v[i])) pm_err("arg", "levels must be integers");
            lv[i] = (int)v[i] - 1;
        }
        rc = ytr_condition_from_levels(t, lv);
        t_check(rc);
        plhs[0] = mxCreateDoubleScalar(rc + 1.0);
    } else if (strcmp(cmd, "condition_at") == 0) {
        int rc;
        pm_nargs(nrhs, 3, "c = ysp_trials('condition_at', h, slot)");
        rc = ytr_condition_at(t, pm_index(prhs[2], "slot", ytr_n_scheduled(t) > 0 ? ytr_n_scheduled(t) : 1));
        t_check(rc);
        plhs[0] = mxCreateDoubleScalar(rc + 1.0);
    } else if (strcmp(cmd, "schedule") == 0) {
        int m = ytr_n_scheduled(t), i;
        mxArray* out = mxCreateDoubleMatrix((size_t)m, 1, mxREAL);
        for (i = 0; i < m; i++) mxGetPr(out)[i] = ytr_condition_at(t, i) + 1.0;
        plhs[0] = out;
    } else if (strcmp(cmd, "n_scheduled") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)ytr_n_scheduled(t));
    } else if (strcmp(cmd, "n_conditions") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)ytr_n_conditions(t));
    } else if (strcmp(cmd, "n_factors") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)ytr_n_factors(t));
    } else if (strcmp(cmd, "n_run") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)ytr_n_run(t));
    } else if (strcmp(cmd, "n_done") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)ytr_n_done(t));
    } else if (strcmp(cmd, "swaps") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)t->swaps);
    } else if (strcmp(cmd, "n_valid") == 0) {
        pm_nargs(nrhs, 3, "n = ysp_trials('n_valid', h, condition)");
        plhs[0] = mxCreateDoubleScalar((double)ytr_n_valid(t, pm_index(prhs[2], "condition", ytr_n_conditions(t))));
    } else if (strcmp(cmd, "count") == 0 || strcmp(cmd, "proportion") == 0) {
        int c, k;
        pm_nargs(nrhs, 3, "ysp_trials('count', h, condition [, outcome])");
        c = pm_index(prhs[2], "condition", ytr_n_conditions(t));
        k = nrhs >= 4 ? pm_int(prhs[3], "outcome") : 1;
        if (cmd[0] == 'c') {
            int rc = ytr_count(t, c, k);
            t_check(rc);
            plhs[0] = mxCreateDoubleScalar((double)rc);
        } else {
            plhs[0] = mxCreateDoubleScalar(ytr_proportion(t, c, k));
        }
    } else if (strcmp(cmd, "history") == 0) {
        plhs[0] = t_history(t);
    } else if (strcmp(cmd, "record") == 0) {
        const void* rec;
        pm_nargs(nrhs, 3, "bytes = ysp_trials('record', h, trial)");
        rec = ytr_record(t, pm_index(prhs[2], "trial", ytr_n_run(t) > 0 ? ytr_n_run(t) : 1));
        if (!rec && n->record_size) pm_err("arg", "no record for that trial");
        plhs[0] = pm_bytes_out(rec, rec ? n->record_size : 0);
    } else if (strcmp(cmd, "format_row") == 0) {
        pm_nargs(nrhs, 3, "line = ysp_trials('format_row', h, trial)");
        plhs[0] = t_format(t, ytr_format_row, pm_index(prhs[2], "trial", ytr_n_run(t) > 0 ? ytr_n_run(t) : 1));
    } else if (strcmp(cmd, "format_header") == 0) {
        plhs[0] = t_format(t, t_fmt_header, 0);
    } else if (strcmp(cmd, "format_meta") == 0) {
        plhs[0] = t_format(t, t_fmt_meta, 0);
    } else if (strcmp(cmd, "format_rules") == 0) {
        plhs[0] = t_format(t, t_fmt_rules, 0);
    } else if (strcmp(cmd, "values") == 0) {
        pm_nargs(nrhs, 3, "row = ysp_trials('values', h, condition)");
        if (!t->desc.table) pm_err("arg", "values needs a session with a table");
        plhs[0] = t_row_struct(t->desc.table, pm_index(prhs[2], "condition", ytr_n_conditions(t)));
    } else if (strcmp(cmd, "jitter") == 0) {
        /* 1-based trial and jitter (or the jitter's name). */
        int i, j;
        pm_nargs(nrhs, 4, "v = ysp_trials('jitter', h, trial, jitter)");
        i = pm_index(prhs[2], "trial", ytr_n_run(t));
        if (mxIsChar(prhs[3])) {
            j = ytr_jitter_index(t, pm_string(prhs[3], "jitter"));
            if (j < 0) pm_err("arg", "no jitter of that name");
        } else {
            j = pm_index(prhs[3], "jitter", t->n_jit);
        }
        plhs[0] = t_jitter_out(ytr_jitter(t, i, j));
    } else if (strcmp(cmd, "jitter_names") == 0) {
        int j;
        plhs[0] = mxCreateCellMatrix(1, (size_t)t->n_jit);
        for (j = 0; j < t->n_jit; j++) mxSetCell(plhs[0], (size_t)j, mxCreateString(t->jit[j].name));
    } else if (strcmp(cmd, "table_info") == 0) {
        if (!t->desc.table) pm_err("arg", "the session has no table");
        plhs[0] = t_table_info(t->desc.table);
    } else if (strcmp(cmd, "save") == 0) {
        size_t sz = ytr_save_size(t);
        mxArray* out = mxCreateNumericMatrix(1, sz, mxUINT8_CLASS, mxREAL);
        t_check(ytr_save(t, mxGetData(out), sz));
        plhs[0] = out;
    } else if (strcmp(cmd, "restore") == 0) {
        /* outcomes: a vector; records: an n x record_size uint8 matrix, or []. */
        int m, i, rc;
        double* v;
        int* oc;
        uint8_t* recs = NULL;
        pm_nargs(nrhs, 3, "ysp_trials('restore', h, outcomes [, records])");
        m = (int)mxGetNumberOfElements(prhs[2]);
        v = (double*)mxMalloc((size_t)(m > 0 ? m : 1) * sizeof(double));
        oc = (int*)mxMalloc((size_t)(m > 0 ? m : 1) * sizeof(int));
        pm_vector(prhs[2], "outcomes", v, m);
        for (i = 0; i < m; i++) {
            if (v[i] != floor(v[i])) pm_err("arg", "outcomes must be integers");
            oc[i] = (int)v[i];
        }
        if (nrhs >= 4 && !mxIsEmpty(prhs[3])) {
            size_t rs = n->record_size, j;
            const uint8_t* src;
            if (!mxIsUint8(prhs[3]) || mxGetM(prhs[3]) != (size_t)m || mxGetN(prhs[3]) != rs)
                pm_err("arg", "records must be a %d x %d uint8 matrix", m, (int)rs);
            src = (const uint8_t*)mxGetData(prhs[3]);
            recs = (uint8_t*)mxMalloc((size_t)m * rs + 1);
            for (i = 0; i < m; i++)
                for (j = 0; j < rs; j++) recs[(size_t)i * rs + j] = src[j * (size_t)m + i];
        }
        n->busy = 1;
        rc = ytr_restore(t, oc, recs, m);
        n->busy = 0;
        pm_rethrow();
        t_check(rc);
    } else if (strcmp(cmd, "rng_state") == 0) {
        if (!n->rng.has_seed) pm_err("arg", "rng_state needs an integer-seed rng");
        if (nrhs >= 3) n->rng.state = pm_u64(prhs[2], "state");
        else plhs[0] = pm_u64_out(n->rng.state);
    } else if (strcmp(cmd, "record_size") == 0) {
        plhs[0] = mxCreateDoubleScalar((double)n->record_size);
    } else {
        pm_err("usage", "unknown command '%s'", cmd);
    }
    (void)nlhs;
}
