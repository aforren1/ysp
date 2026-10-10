/* trials_test.c - self-checking test for ysp/trials.h. No framework: it
 * returns 0 when every check passed and 1 after printing each failure.
 *
 * Build it twice: once with a small history, so YTR_ERR_FULL costs a few
 * hundred trials instead of four thousand, and once at the defaults:
 *
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude \
 *         -DYTR_MAX_TRIALS=256 -o trials_test tests/adapt/trials_test.c
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude \
 *         -o trials_test tests/adapt/trials_test.c
 *
 * Every design here fits 256 trials, so both builds run the same checks.
 * The constraint checks use a checker written here from the manual's
 * wording (forward scans, per-level last positions), not a copy of the
 * header's backward-looking test, so the two can disagree.
 */
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"

#include <stdio.h>
#include <string.h>

/* What ytr_next() fills; a starting value so no path reads it unset. */
static const ytr_trial_info ti_zero = { 0, 0, 0, 0, 0, false, false, false, false, false };
#include <time.h>

/* ---------------------------------------------------------------- harness */

static int g_failures = 0;

static void fail(int line, const char* what) {
    fprintf(stderr, "trials_test: FAIL at line %d: %s\n", line, what);
    g_failures++;
}

static void fail_i(int line, const char* what, long got, long want) {
    fprintf(stderr, "trials_test: FAIL at line %d: %s (got %ld, want %ld)\n",
            line, what, got, want);
    g_failures++;
}

static void fail_s(int line, const char* what, const char* got, const char* want) {
    fprintf(stderr, "trials_test: FAIL at line %d: %s\n  got:  [%s]\n  want: [%s]\n",
            line, what, got, want);
    g_failures++;
}

#define CHECK(cond) do { if (!(cond)) fail(__LINE__, #cond); } while (0)
#define CHECK_I(got, want) do { long g_ = (long)(got), w_ = (long)(want); \
    if (g_ != w_) fail_i(__LINE__, #got, g_, w_); } while (0)
#define CHECK_S(got, want) do { const char* g_ = (got); const char* w_ = (want); \
    if (strcmp(g_, w_) != 0) fail_s(__LINE__, #got, g_, w_); } while (0)
#define CHECK_HAS(str, sub) do { if (!strstr((str), (sub))) \
    fail_s(__LINE__, "message lacks a substring", (str), (sub)); } while (0)

static bool is_nan(double x) { return !(x >= 0.0) && !(x < 0.0); }

/* Two handles: about 90 KB each at the defaults, so not on the stack. */
static ytr_trials g_t, g_u;

/* ------------------------------------------------------------ the checker */

typedef struct design {
    int n_levels[YTR_MAX_FACTORS];
    int n_factors;
    const ytr_constraint* c;
    int n_c;
} design;

/* Level by this file's own arithmetic, last factor fastest. */
static int lev_of(const design* g, int cond, int f) {
    int s = 1, j, nf;
    if (cond < 0) return -1;
    /* YTR_CONDITION (-1) means the row itself. The test is `f < 0`, and
     * every index below is bounded by the array too, because gcc 13 at -O3
     * carries a constant -1 from a caller past an `== -1` test and then
     * reports n_levels[-1]. */
    if (f < 0) return cond;
    if (f >= YTR_MAX_FACTORS) return -1;
    nf = g->n_factors < YTR_MAX_FACTORS ? g->n_factors : YTR_MAX_FACTORS;
    for (j = nf - 1; j > f && j >= 0; j--) s *= g->n_levels[j];
    if (g->n_levels[f] < 1 || s < 1) return -1;
    return (cond / s) % g->n_levels[f];
}

/* mark[k] = 1 + constraint index when a violation of some rule completes at
 * position k (its last trial is k), else 0. cut[k] starts a segment. */
static void find_violations(const design* g, const int* seq, const bool* cut, int n, int* mark) {
    int ci, k, j, seg, run, cur, cnt, lo, L, target;
    int last[YTR_MAX_CONDITIONS];
    for (k = 0; k < n; k++) mark[k] = 0;
    for (ci = 0; ci < g->n_c; ci++) {
        const ytr_constraint* c = &g->c[ci];
        seg = 0; run = 0; cur = -2;
        for (j = 0; j < YTR_MAX_CONDITIONS; j++) last[j] = -1;
        for (k = 0; k < n; k++) {
            if (cut[k]) {
                seg = k; run = 0; cur = -2;
                for (j = 0; j < YTR_MAX_CONDITIONS; j++) last[j] = -1;
            }
            L = lev_of(g, seq[k], c->factor);
            target = (c->level == YTR_ANY_LEVEL) ? L : c->level;
            switch (c->rule) {
            case YTR_RULE_MAX_RUN:
                if (L < 0 || L != target) { run = 0; cur = -2; break; }
                if (L == cur) run++; else { cur = L; run = 1; }
                if (run > c->n && !mark[k]) mark[k] = ci + 1;
                break;
            case YTR_RULE_MAX_IN_WINDOW:
                if (L < 0 || L != target) break;
                lo = k - c->window + 1;
                if (lo < seg) lo = seg;
                cnt = 0;
                for (j = lo; j <= k; j++) if (lev_of(g, seq[j], c->factor) == L) cnt++;
                if (cnt > c->n && !mark[k]) mark[k] = ci + 1;
                break;
            case YTR_RULE_MIN_GAP:
                if (L < 0 || L >= YTR_MAX_CONDITIONS || L != target) break;
                if (last[L] >= 0 && k - last[L] - 1 < c->n && !mark[k]) mark[k] = ci + 1;
                last[L] = k;
                break;
            case YTR_RULE_NO_TRANSITION:
                if (k > 0 && !cut[k] && L == c->level2 &&
                    lev_of(g, seq[k - 1], c->factor) == c->level && !mark[k])
                    mark[k] = ci + 1;
                break;
            case YTR_RULE_FIRST_NOT:
                if (k == 0 && L == c->level && !mark[k]) mark[k] = ci + 1;
                break;
            default:
                break;
            }
        }
    }
}

static int count_violations(const design* g, const int* seq, const bool* cut, int n) {
    int mark[YTR_MAX_TRIALS];
    int k, v = 0;
    find_violations(g, seq, cut, n, mark);
    for (k = 0; k < n; k++) if (mark[k]) v++;
    return v;
}

/* The main schedule as open() left it, with the segments the repair used. */
static int schedule_of(const ytr_trials* t, int* seq, bool* cut, int bs) {
    int np = t->desc.n_practice, n = ytr_n_scheduled(t) - np, k;
    for (k = 0; k < n; k++) {
        seq[k] = ytr_condition_at(t, np + k);
        cut[k] = (bs > 0 && k % bs == 0);
    }
    return n;
}

/* The realized main sequence: history without practice and warmup, a track
 * trial as -1, a cut where a block starts or any break flag appeared since
 * the previous main trial. `idx` maps back to trial indices. */
static int realized_of(const ytr_trials* t, int* seq, bool* cut, int* idx, bool span) {
    const ytr_trial* h;
    int n = 0, i, nh = 0, bs = t->desc.block_size;
    bool brk = false;
    h = ytr_history(t, &nh);
    for (i = 0; i < nh; i++) {
        if (h[i].flags & YTR_FLAG_AFTER_BREAK) brk = true;
        if (h[i].flags & (YTR_FLAG_PRACTICE | YTR_FLAG_WARMUP)) continue;
        seq[n] = h[i].condition;
        cut[n] = !span && (n == 0 || brk || (bs > 0 && n % bs == 0));
        if (idx) idx[n] = i;
        brk = false;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------- utilities */

static void zero_desc(ytr_desc* d) { memset(d, 0, sizeof(*d)); }

/* History entry i by value, or a blank one out of range, so a check never
 * dereferences the NULL ytr_history() gives a closed handle. */
static ytr_trial hist(const ytr_trials* t, int i) {
    ytr_trial z;
    int n = 0;
    const ytr_trial* h = ytr_history(t, &n);
    memset(&z, 0, sizeof(z));
    z.condition = -2;
    if (!h || i < 0 || i >= n) return z;
    return h[i];
}

/* The double record of trial i, or -1 when there is none. */
static double rec_d(const ytr_trials* t, int i) {
    const double* r = (const double*)ytr_record(t, i);
    return r ? *r : -1.0;
}

static uint64_t g_rng_state;

/* The test's own draws (outcomes, requeue decisions), separate from the
 * handle's generator so the two streams never interleave by accident. */
static uint64_t g_test_state;
static double test_u(void) { return ytr_splitmix(&g_test_state); }

static int outcome_for(int i, int cond) {
    uint32_t h = (uint32_t)i * 2654435761u ^ (uint32_t)(cond + 7) * 40503u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (int)(h % 3u) == 0 ? 0 : 1;
}

/* ------------------------------------------------------------ fake tracks */

/* A track that is done once `limit` of its trials have been handed out,
 * counted from the handle's own history, so a restored or loaded handle
 * gives the same answers with no state of its own to carry. */
typedef struct fake_track {
    const ytr_trials* t;
    int id;
    int limit;
    int calls;
    bool said_done;
    int calls_after_done;
} fake_track;

static bool fake_done(void* ctx) {
    fake_track* f = (fake_track*)ctx;
    const ytr_trial* h;
    int n = 0, i, k = 0;
    h = ytr_history(f->t, &n);
    for (i = 0; i < n; i++) if (h[i].track == f->id) k++;
    f->calls++;
    if (f->said_done) f->calls_after_done++;
    if (k >= f->limit) f->said_done = true;
    return k >= f->limit;
}

static void fake_init(fake_track* f, const ytr_trials* t, int id, int limit) {
    memset(f, 0, sizeof(*f));
    f->t = t;
    f->id = id;
    f->limit = limit;
}

/* ------------------------------------------------------------- version */

static void test_version(void) {
    char want[32];
    snprintf(want, sizeof(want), "%d.%d.%d",
             YTR_VERSION_MAJOR, YTR_VERSION_MINOR, YTR_VERSION_PATCH);
    CHECK(strcmp(YTR_VERSION_STRING, want) == 0);
    CHECK(strcmp(ytr_version(), YTR_VERSION_STRING) == 0);
    CHECK(strcmp(ytr_version(), "0.2.1") == 0);
}

/* splitmix64 from seed 0: the reference's first two outputs are
 * 0xE220A8397B1DCDAF and 0x6E789E6AA1B965F4 (Vigna's splitmix64.c). */
static void test_splitmix(void) {
    uint64_t s = 0;
    double a = ytr_splitmix(&s);
    double b = ytr_splitmix(&s);
    CHECK(a == (double)(0xE220A8397B1DCDAFULL >> 11) / 9007199254740992.0);
    CHECK(b == (double)(0x6E789E6AA1B965F4ULL >> 11) / 9007199254740992.0);
    CHECK(s == 2u * 0x9E3779B97F4A7C15ULL);
}

/* ------------------------------------------------------------- factorial */

static void test_factorial(void) {
    ytr_desc d;
    int lv[3], a, b, c, cond, want = 0;
    zero_desc(&d);
    d.factors[0] = ytr_factor("a", 2);
    d.factors[1] = ytr_factor("b", 3);
    d.factors[2] = ytr_factor("c", 4);
    d.n_factors = 3;
    d.reps = 1;
    CHECK(ytr_open(&g_t, &d));
    CHECK_I(ytr_n_conditions(&g_t), 24);
    CHECK_I(ytr_n_factors(&g_t), 3);
    for (a = 0; a < 2; a++)
        for (b = 0; b < 3; b++)
            for (c = 0; c < 4; c++) {
                lv[0] = a; lv[1] = b; lv[2] = c;
                cond = ytr_condition_from_levels(&g_t, lv);
                CHECK_I(cond, want);   /* last factor fastest */
                CHECK_I(ytr_level(&g_t, cond, 0), a);
                CHECK_I(ytr_level(&g_t, cond, 1), b);
                CHECK_I(ytr_level(&g_t, cond, 2), c);
                CHECK_I(ytr_level(&g_t, cond, YTR_CONDITION), cond);
                want++;
            }
    CHECK_I(ytr_level(&g_t, 24, 0), YTR_ERR_ARG);
    CHECK_I(ytr_level(&g_t, -1, 0), YTR_ERR_ARG);
    CHECK_I(ytr_level(&g_t, 0, 3), YTR_ERR_ARG);
    lv[0] = 2; lv[1] = 0; lv[2] = 0;
    CHECK_I(ytr_condition_from_levels(&g_t, lv), YTR_ERR_ARG);
    CHECK_I(ytr_condition_from_levels(&g_t, NULL), YTR_ERR_ARG);

    /* SEQUENTIAL with factors: the rows in order, once. */
    for (cond = 0; cond < 24; cond++) CHECK_I(ytr_condition_at(&g_t, cond), cond);
    CHECK_I(ytr_condition_at(&g_t, 24), YTR_ERR_ARG);

    /* Plain rows have no factors to look up. */
    zero_desc(&d);
    d.n_conditions = 5;
    d.reps = 1;
    CHECK(ytr_open(&g_t, &d));
    CHECK_I(ytr_level(&g_t, 3, 0), YTR_ERR_ARG);
    CHECK_I(ytr_level(&g_t, 3, YTR_CONDITION), 3);
    CHECK_I(ytr_condition_from_levels(&g_t, lv), YTR_ERR_ARG);
}

/* ------------------------------------------------------------ the orders */

static void test_orders(void) {
    enum { NC = 5, REPS = 8, SEEDS = 400 };
    static const int weights[NC] = { 3, 0, 1, 2, 5 };
    ytr_desc d;
    int seed, k, r, c, rep_seen[NC], cnt[NC], block_len, pos;
    int first_pos[NC];
    int seq_a[NC * REPS], differ = 0;
    int seen[NC];

    for (c = 0; c < NC; c++) first_pos[c] = 0;

    /* SEQUENTIAL: PsychoPy's identical order, rep-major. No rng needed. */
    zero_desc(&d);
    d.n_conditions = NC;
    d.reps = REPS;
    CHECK(ytr_open(&g_t, &d));
    CHECK_I(ytr_n_scheduled(&g_t), NC * REPS);
    for (k = 0; k < NC * REPS; k++) CHECK_I(ytr_condition_at(&g_t, k), k % NC);
    for (k = 0; k < NC * REPS; k++) CHECK_I(g_t.schedule_rep[k], k / NC);

    for (seed = 0; seed < SEEDS; seed++) {
        /* RANDOM: every condition once per repetition, blocks in order. */
        zero_desc(&d);
        d.n_conditions = NC;
        d.reps = REPS;
        d.order = YTR_ORDER_RANDOM;
        g_rng_state = (uint64_t)seed;
        d.rng = ytr_splitmix;
        d.rng_ctx = &g_rng_state;
        CHECK(ytr_open(&g_t, &d));
        for (r = 0; r < REPS; r++) {
            for (c = 0; c < NC; c++) seen[c] = 0;
            for (k = 0; k < NC; k++) {
                c = ytr_condition_at(&g_t, r * NC + k);
                if (c >= 0 && c < NC) seen[c]++;
                CHECK_I(g_t.schedule_rep[r * NC + k], r);
            }
            for (c = 0; c < NC; c++) CHECK_I(seen[c], 1);
        }

        /* FULL_RANDOM: every condition exactly REPS times, reps numbered by
         * occurrence, and the first slot close to uniform over seeds. */
        zero_desc(&d);
        d.n_conditions = NC;
        d.reps = REPS;
        d.order = YTR_ORDER_FULL_RANDOM;
        g_rng_state = (uint64_t)seed * 7919u;
        d.rng = ytr_splitmix;
        d.rng_ctx = &g_rng_state;
        CHECK(ytr_open(&g_t, &d));
        for (c = 0; c < NC; c++) { cnt[c] = 0; rep_seen[c] = 0; }
        for (k = 0; k < NC * REPS; k++) {
            c = ytr_condition_at(&g_t, k);
            if (c < 0 || c >= NC) { fail(__LINE__, "condition out of range"); continue; }
            CHECK_I(g_t.schedule_rep[k], rep_seen[c]);
            rep_seen[c]++;
            cnt[c]++;
            if (seed == 0) seq_a[k] = c;
            else if (seed == 1 && seq_a[k] != c) differ++;
        }
        for (c = 0; c < NC; c++) CHECK_I(cnt[c], REPS);
        first_pos[ytr_condition_at(&g_t, 0)]++;

        /* Weighted RANDOM: repetition r holds the rows with more than r. */
        zero_desc(&d);
        d.n_conditions = NC;
        d.cond_reps = weights;
        d.order = YTR_ORDER_RANDOM;
        g_rng_state = (uint64_t)seed + 1000u;
        d.rng = ytr_splitmix;
        d.rng_ctx = &g_rng_state;
        CHECK(ytr_open(&g_t, &d));
        CHECK_I(ytr_n_scheduled(&g_t), 11);
        pos = 0;
        for (r = 0; r < 5; r++) {
            block_len = 0;
            for (c = 0; c < NC; c++) { seen[c] = 0; if (weights[c] > r) block_len++; }
            for (k = 0; k < block_len; k++) {
                c = ytr_condition_at(&g_t, pos + k);
                if (c >= 0 && c < NC) seen[c]++;
            }
            for (c = 0; c < NC; c++) CHECK_I(seen[c], weights[c] > r ? 1 : 0);
            pos += block_len;
        }
    }
    CHECK(differ > 0);
    /* 400 draws over 5 rows: 80 expected, sd about 8. */
    for (c = 0; c < NC; c++) CHECK(first_pos[c] > 50 && first_pos[c] < 110);

    /* The same seed gives the same order. */
    zero_desc(&d);
    d.n_conditions = NC;
    d.reps = REPS;
    d.order = YTR_ORDER_FULL_RANDOM;
    d.rng = ytr_splitmix;
    d.rng_ctx = &g_rng_state;
    g_rng_state = 0;
    CHECK(ytr_open(&g_t, &d));
    for (k = 0; k < NC * REPS; k++) CHECK_I(ytr_condition_at(&g_t, k), seq_a[k]);
}

static double zero_rng(void* ctx) { (void)ctx; return 0.0; }
static double near_one_rng(void* ctx) { (void)ctx; return 0.9999999999; }
static double counting_rng(void* ctx) { (*(int*)ctx)++; return 0.5; }

static void test_draw_order(void) {
    ytr_desc d;
    int k, calls = 0;
    /* Fisher-Yates from the top with u = 0: for i = 3..1, swap i and 0.
     * [0 1 2 3] -> [3 1 2 0] -> [2 1 3 0] -> [1 2 3 0]. */
    zero_desc(&d);
    d.n_conditions = 4;
    d.reps = 1;
    d.order = YTR_ORDER_FULL_RANDOM;
    d.rng = zero_rng;
    CHECK(ytr_open(&g_t, &d));
    CHECK_I(ytr_condition_at(&g_t, 0), 1);
    CHECK_I(ytr_condition_at(&g_t, 1), 2);
    CHECK_I(ytr_condition_at(&g_t, 2), 3);
    CHECK_I(ytr_condition_at(&g_t, 3), 0);
    /* u just below 1: j = i every time, the identity. */
    d.rng = near_one_rng;
    CHECK(ytr_open(&g_t, &d));
    for (k = 0; k < 4; k++) CHECK_I(ytr_condition_at(&g_t, k), k);

    /* Draw counts: 3 practice + (10 - 1) shuffle at open; RANDOM has one
     * shuffle per repetition: 2 x (5 - 1). */
    zero_desc(&d);
    d.n_conditions = 10;
    d.reps = 1;
    d.order = YTR_ORDER_FULL_RANDOM;
    d.n_practice = 3;
    d.rng = counting_rng;
    d.rng_ctx = &calls;
    CHECK(ytr_open(&g_t, &d));
    CHECK_I(calls, 3 + 9);
    calls = 0;
    zero_desc(&d);
    d.n_conditions = 5;
    d.reps = 2;
    d.order = YTR_ORDER_RANDOM;
    d.rng = counting_rng;
    d.rng_ctx = &calls;
    CHECK(ytr_open(&g_t, &d));
    CHECK_I(calls, 8);
    /* The run itself draws nothing without tracks, practice or re-queues. */
    calls = 0;
    while (ytr_next(&g_t, NULL) >= 0) ytr_update(&g_t, 1, NULL);
    CHECK_I(calls, 0);
}

/* --------------------------------------------------------- constraints */

typedef struct cdesign {
    const char* name;
    int n_levels[3];
    int n_factors;
    int n_conditions;   /* plain rows when n_factors is 0 */
    const int* cond_reps;
    int reps;
    ytr_constraint c[4];
    int n_c;
    int block_size;
    bool span;
} cdesign;

static const int catch_reps[6] = { 8, 8, 8, 8, 8, 8 };
static const int catch_reps2[4] = { 6, 14, 14, 14 };
static const int two_rows[2] = { 10, 18 };

static void design_from(const cdesign* cd, design* g) {
    int f;
    memset(g, 0, sizeof(*g));
    g->n_factors = cd->n_factors;
    for (f = 0; f < cd->n_factors && f < 3; f++) g->n_levels[f] = cd->n_levels[f];
    g->c = cd->c;
    g->n_c = cd->n_c;
}

static void desc_from(const cdesign* cd, ytr_desc* d, uint64_t* state) {
    int f, i;
    zero_desc(d);
    for (f = 0; f < cd->n_factors && f < 3; f++) d->factors[f] = ytr_factor(NULL, cd->n_levels[f]);
    d->n_factors = cd->n_factors;
    d->n_conditions = cd->n_conditions;
    d->cond_reps = cd->cond_reps;
    d->reps = cd->reps;
    d->order = YTR_ORDER_CONSTRAINED;
    for (i = 0; i < cd->n_c; i++) d->constraints[i] = cd->c[i];
    d->n_constraints = cd->n_c;
    d->block_size = cd->block_size;
    d->constraints_span_blocks = cd->span;
    d->rng = ytr_splitmix;
    d->rng_ctx = state;
}

static void test_constraint_orders(void) {
    static cdesign designs[10];
    static int seq[YTR_MAX_TRIALS];
    static bool cut[YTR_MAX_TRIALS];
    ytr_desc d;
    design g;
    int di, seed, n = 0, k, c, total_swaps, cnt[YTR_MAX_CONDITIONS], cross, n_designs;
    uint64_t state;
    clock_t t0 = clock();
    const int SEEDS = 200;

    memset(designs, 0, sizeof(designs));
    n_designs = 0;
    /* The first USAGE example. */
    designs[n_designs].name = "2x5x20 max_run(0,any,3)";
    designs[n_designs].n_levels[0] = 2; designs[n_designs].n_levels[1] = 5;
    designs[n_designs].n_factors = 2; designs[n_designs].reps = 20;
    designs[n_designs].c[0] = ytr_max_run(0, YTR_ANY_LEVEL, 3);
    designs[n_designs].n_c = 1;
    n_designs++;
    designs[n_designs].name = "6 rows, catch at most 1 in 5";
    designs[n_designs].n_conditions = 6; designs[n_designs].cond_reps = catch_reps;
    designs[n_designs].c[0] = ytr_max_in_window(YTR_CONDITION, 0, 5, 1);
    designs[n_designs].n_c = 1;
    n_designs++;
    designs[n_designs].name = "catch min_gap 2 + first_not";
    designs[n_designs].n_conditions = 4; designs[n_designs].cond_reps = catch_reps2;
    designs[n_designs].c[0] = ytr_min_gap(YTR_CONDITION, 0, 2);
    designs[n_designs].c[1] = ytr_first_not(YTR_CONDITION, 0);
    designs[n_designs].n_c = 2;
    n_designs++;
    designs[n_designs].name = "3x4x6 no_transition + max_run(0,1,2)";
    designs[n_designs].n_levels[0] = 3; designs[n_designs].n_levels[1] = 4;
    designs[n_designs].n_factors = 2; designs[n_designs].reps = 6;
    designs[n_designs].c[0] = ytr_no_transition(0, 0, 1);
    designs[n_designs].c[1] = ytr_no_transition(1, 3, 3);
    designs[n_designs].c[2] = ytr_max_run(0, 1, 2);
    designs[n_designs].n_c = 3;
    n_designs++;
    designs[n_designs].name = "4x3x8 min_gap(0,any,1) + window(1,any,4,2)";
    designs[n_designs].n_levels[0] = 4; designs[n_designs].n_levels[1] = 3;
    designs[n_designs].n_factors = 2; designs[n_designs].reps = 8;
    designs[n_designs].c[0] = ytr_min_gap(0, YTR_ANY_LEVEL, 1);
    designs[n_designs].c[1] = ytr_max_in_window(1, YTR_ANY_LEVEL, 4, 2);
    designs[n_designs].n_c = 2;
    n_designs++;
    /* Blocks: two rows 10:18, max run 2. Across the whole session this is
     * tight (11 runs of 2 for 18); within blocks of 7 runs may straddle. */
    designs[n_designs].name = "2 rows 10:18, max_run 2, blocks of 7";
    designs[n_designs].n_conditions = 2; designs[n_designs].cond_reps = two_rows;
    designs[n_designs].c[0] = ytr_max_run(YTR_CONDITION, YTR_ANY_LEVEL, 2);
    designs[n_designs].n_c = 1; designs[n_designs].block_size = 7;
    n_designs++;
    designs[n_designs] = designs[n_designs - 1];
    designs[n_designs].name = "same, spanning blocks";
    designs[n_designs].span = true;
    n_designs++;
    designs[n_designs].name = "8 rows, window clipped at blocks of 10";
    designs[n_designs].n_conditions = 8; designs[n_designs].reps = 10;
    designs[n_designs].c[0] = ytr_max_in_window(YTR_CONDITION, YTR_ANY_LEVEL, 6, 1);
    designs[n_designs].n_c = 1; designs[n_designs].block_size = 10;
    n_designs++;

    for (di = 0; di < n_designs; di++) {
        const cdesign* cd = &designs[di];
        int bs = cd->span ? 0 : cd->block_size;
        total_swaps = 0;
        cross = 0;
        design_from(cd, &g);
        for (seed = 0; seed < SEEDS; seed++) {
            state = (uint64_t)seed * 1000003u + (uint64_t)di;
            desc_from(cd, &d, &state);
            if (!ytr_open(&g_t, &d)) {
                fprintf(stderr, "  %s seed %d: %s\n", cd->name, seed, ytr_error(&g_t));
                fail(__LINE__, "a feasible design failed to open");
                continue;
            }
            total_swaps += g_t.swaps;
            n = schedule_of(&g_t, seq, cut, bs);
            if (count_violations(&g, seq, cut, n) != 0) {
                fprintf(stderr, "  %s seed %d\n", cd->name, seed);
                fail(__LINE__, "a repaired order breaks a constraint");
            }
            /* The repair only permutes. */
            for (c = 0; c < ytr_n_conditions(&g_t); c++) cnt[c] = 0;
            for (k = 0; k < n; k++) cnt[seq[k]]++;
            for (c = 0; c < ytr_n_conditions(&g_t); c++)
                CHECK_I(cnt[c], cd->cond_reps ? cd->cond_reps[c] : cd->reps);
            /* And the run hands out exactly that schedule, rule-abiding. */
            k = 0;
            while (ytr_next(&g_t, NULL) >= 0) ytr_update(&g_t, 1, NULL);
            if (bs > 0) {
                /* How often the relaxation was used: a violation across a
                 * boundary when the segments are ignored. */
                static bool nocut[YTR_MAX_TRIALS];
                memset(nocut, 0, sizeof(nocut));
                if (count_violations(&g, seq, nocut, n) > 0) cross++;
            }
            {
                static int rseq[YTR_MAX_TRIALS];
                static bool rcut[YTR_MAX_TRIALS];
                int rn = realized_of(&g_t, rseq, rcut, NULL, cd->span);
                CHECK_I(rn, n);
                for (k = 0; k < rn && k < n; k++)
                    if (rseq[k] != seq[k]) { fail(__LINE__, "run order differs from schedule"); break; }
            }
        }
        printf("  %-44s %d seeds, %.1f swaps per open%s", cd->name, SEEDS,
               (double)total_swaps / SEEDS, bs > 0 ? "" : "\n");
        if (bs > 0) printf(", %d orders use the block boundary\n", cross);
        if (cd->block_size > 0 && !cd->span) CHECK(cross > 0);
    }
    printf("  constraint property checks: %.2f s\n",
           (double)(clock() - t0) / CLOCKS_PER_SEC);
}

static void test_impossible(void) {
    static const int unequal[2] = { 3, 5 };
    ytr_desc d;
    fake_track tr;
    clock_t t0;
    double secs;

    /* Two levels, max run 1, unequal counts: no order exists. */
    zero_desc(&d);
    d.n_conditions = 2;
    d.cond_reps = unequal;
    d.order = YTR_ORDER_CONSTRAINED;
    d.constraints[0] = ytr_max_run(YTR_CONDITION, YTR_ANY_LEVEL, 1);
    d.n_constraints = 1;
    d.rng = ytr_splitmix;
    d.rng_ctx = &g_rng_state;
    g_rng_state = 5;
    t0 = clock();
    CHECK(!ytr_open(&g_t, &d));
    secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    CHECK(!ytr_is_open(&g_t));
    CHECK_HAS(ytr_error(&g_t), "constraint 0, max_run(cond,any,1)");
    CHECK_HAS(ytr_error(&g_t), "after 100000 swaps");
    printf("  impossible 8-trial design: \"%s\"\n  (the whole 100000-swap budget: %.3f s)\n",
           ytr_error(&g_t), secs);

    /* The budget is the caller's. */
    d.max_swaps = 10;
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "after 10 swaps");

    /* A larger impossible design: 200 trials, one row may never repeat. */
    {
        static const int big[2] = { 90, 110 };
        d.cond_reps = big;
        d.max_swaps = 0;
        t0 = clock();
        CHECK(!ytr_open(&g_t, &d));
        printf("  impossible 200-trial design, whole budget: %.3f s\n",
               (double)(clock() - t0) / CLOCKS_PER_SEC);
    }
#if YTR_MAX_TRIALS >= 4000
    /* The worst open() a session of this size can have, for the manual's
     * cost note: 4000 trials, the whole budget spent. */
    {
        static const int huge[2] = { 1990, 2010 };
        d.cond_reps = huge;
        t0 = clock();
        CHECK(!ytr_open(&g_t, &d));
        printf("  impossible 4000-trial design, whole budget: %.3f s\n",
               (double)(clock() - t0) / CLOCKS_PER_SEC);
    }
#endif

    /* first_not on the only row. */
    zero_desc(&d);
    d.n_conditions = 1;
    d.reps = 3;
    d.order = YTR_ORDER_CONSTRAINED;
    d.constraints[0] = ytr_first_not(YTR_CONDITION, 0);
    d.n_constraints = 1;
    d.rng = ytr_splitmix;
    d.rng_ctx = &g_rng_state;
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "first_not(cond,0)");

    /* With a track the same design opens: track trials can separate. */
    fake_init(&tr, &g_t, 0, 10);
    d.tracks[0] = ytr_track(&tr, fake_done);
    d.n_tracks = 1;
    d.track_rate = 0.5;
    CHECK(ytr_open(&g_t, &d));
    CHECK(ytr_next(&g_t, NULL) >= 0);
    CHECK_I(g_t.history[0].track, 0);   /* the catch may not come first */
}

/* ----------------------------------------------------------------- tracks */

static void test_tracks(void) {
    fake_track tr[3];
    ytr_desc d;
    ytr_trial_info ti = ti_zero;
    int i, idx, n_track = 0, n_cond = 0, calls, k, n_before = 0, prev;
    int seq_rr[12];
    static const int want_rr[12] = { 0, 1, 2, 0, 1, 2, 1, 2, 1, 2, 1, -1 };

    /* Tracks only: the default rate is 1, and round robin cycles the live
     * ones. Limits 2, 5, 4: 0 1 2 0 1 2 | 1 2 1 2 | 1, and then done. */
    zero_desc(&d);
    fake_init(&tr[0], &g_t, 0, 2);
    fake_init(&tr[1], &g_t, 1, 5);
    fake_init(&tr[2], &g_t, 2, 4);
    for (i = 0; i < 3; i++) d.tracks[i] = ytr_track(&tr[i], fake_done);
    d.n_tracks = 3;
    d.interleave = YTR_INTERLEAVE_ROUND_ROBIN;
    CHECK(ytr_open(&g_t, &d));
    for (k = 0; k < 12; k++) {
        calls = tr[0].calls + tr[1].calls + tr[2].calls;
        idx = ytr_next(&g_t, &ti);
        /* At most one call per track per next(), none on a done track. */
        CHECK(tr[0].calls + tr[1].calls + tr[2].calls - calls <= 3);
        seq_rr[k] = idx >= 0 ? ti.track : -1;
        if (idx < 0) break;
        CHECK_I(ti.condition, -1);
        CHECK_I(ti.rep, -1);
        /* Pending: next() again is the same trial and asks no track. */
        calls = tr[0].calls + tr[1].calls + tr[2].calls;
        CHECK_I(ytr_next(&g_t, NULL), idx);
        CHECK_I(tr[0].calls + tr[1].calls + tr[2].calls, calls);
        CHECK_I(ytr_update(&g_t, 1, NULL), 0);
    }
    for (k = 0; k < 12; k++) CHECK_I(seq_rr[k], want_rr[k]);
    for (i = 0; i < 3; i++) CHECK_I(tr[i].calls_after_done, 0);
    CHECK(ytr_done(&g_t));
    CHECK_I(ytr_next(&g_t, NULL), YTR_DONE);

    /* track_rate 0 with both is a missing value, not a design. */
    zero_desc(&d);
    d.n_conditions = 1;
    d.reps = 10;
    fake_init(&tr[0], &g_t, 0, 10);
    d.tracks[0] = ytr_track(&tr[0], fake_done);
    d.n_tracks = 1;
    d.rng = ytr_splitmix;
    d.rng_ctx = &g_rng_state;
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "track_rate is required");

    /* track_rate 1: the tracks run to their end, then the schedule. */
    d.track_rate = 1.0;
    fake_init(&tr[0], &g_t, 0, 7);
    CHECK(ytr_open(&g_t, &d));
    k = 0;
    while ((idx = ytr_next(&g_t, &ti)) >= 0) {
        if (k < 7) CHECK_I(ti.track, 0); else CHECK_I(ti.track, -1);
        ytr_update(&g_t, 1, NULL);
        k++;
    }
    CHECK_I(k, 17);

    /* track_rate 0.5 with two tracks that finish at 40 and 60, and 100
     * scheduled trials: while a track is live about half the trials are
     * track trials; a finished track is never picked. */
    zero_desc(&d);
    d.n_conditions = 2;
    d.reps = 50;
    d.order = YTR_ORDER_FULL_RANDOM;
    fake_init(&tr[0], &g_t, 0, 40);
    fake_init(&tr[1], &g_t, 1, 60);
    d.tracks[0] = ytr_track(&tr[0], fake_done);
    d.tracks[1] = ytr_track(&tr[1], fake_done);
    d.n_tracks = 2;
    d.track_rate = 0.5;
    d.rng = ytr_splitmix;
    d.rng_ctx = &g_rng_state;
    g_rng_state = 42;
    CHECK(ytr_open(&g_t, &d));
    prev = 0;
    while ((idx = ytr_next(&g_t, &ti)) >= 0) {
        bool live = !(tr[0].said_done && tr[1].said_done);
        if (ti.track >= 0) {
            n_track++;
            CHECK(!tr[ti.track].said_done);
        } else if (live) {
            n_cond++;
        } else {
            n_before++;   /* after both tracks ended */
        }
        CHECK_I(tr[0].calls_after_done + tr[1].calls_after_done, 0);
        ytr_update(&g_t, 1, NULL);
        prev = idx;
    }
    CHECK_I(n_track, 100);
    CHECK_I(n_cond + n_before, 100);
    CHECK_I(prev, 199);
    printf("  track_rate 0.5: %d track and %d condition trials while tracks ran\n",
           n_track, n_cond);
    CHECK(n_cond > 70 && n_cond < 130);

    /* RANDOM interleave by weight, 1 : 3. */
    zero_desc(&d);
    fake_init(&tr[0], &g_t, 0, 1000000);
    fake_init(&tr[1], &g_t, 1, 1000000);
    d.tracks[0] = ytr_track(&tr[0], fake_done);
    d.tracks[1] = ytr_track(&tr[1], fake_done);
    d.tracks[1].weight = 3.0;
    d.n_tracks = 2;
    d.rng = ytr_splitmix;
    d.rng_ctx = &g_rng_state;
    CHECK(ytr_open(&g_t, &d));
    n_track = 0;
    for (k = 0; k < 200; k++) {
        idx = ytr_next(&g_t, &ti);
        if (idx < 0) { fail(__LINE__, "next"); break; }
        if (ti.track == 0) n_track++;
        ytr_update(&g_t, 1, NULL);
    }
    printf("  weights 1:3: track 0 got %d of 200\n", n_track);
    CHECK(n_track > 30 && n_track < 70);
}

/* ------------------------------------------ practice, warmup, breaks */

static void test_practice_warmup(void) {
    static const int easy[2] = { 3, 1 };
    ytr_desc d;
    ytr_trial_info ti = ti_zero;
    int idx, k, n_prac = 0, n_warm = 0, n_main = 0, blocks_seen = -2;
    int main_in_block = 0;

    /* SEQUENTIAL: practice cycles the list in order, no rng needed. */
    zero_desc(&d);
    d.n_conditions = 4;
    d.reps = 3;             /* 12 main trials */
    d.n_practice = 5;
    d.warmup_conditions = easy;
    d.n_warmup_conditions = 2;
    d.block_size = 5;       /* blocks 0,1,2: 5, 5, 2 main trials */
    d.n_warmup = 2;
    CHECK(ytr_open(&g_t, &d));
    CHECK_I(ytr_n_scheduled(&g_t), 17);
    for (k = 0; k < 5; k++) CHECK_I(ytr_condition_at(&g_t, k), easy[k % 2]);
    for (k = 0; k < 12; k++) CHECK_I(ytr_condition_at(&g_t, 5 + k), k % 4);

    k = 0;
    while ((idx = ytr_next(&g_t, &ti)) >= 0) {
        CHECK_I(idx, k);
        if (k < 5) {
            CHECK(ti.practice);
            CHECK_I(ti.block, -1);
            CHECK_I(ti.rep, -1);
            CHECK_I(ti.condition, easy[k % 2]);
            CHECK(ti.first_in_block == (k == 0));
            n_prac++;
        } else if (ti.warmup) {
            n_warm++;
            CHECK_I(ti.rep, -1);
            if (ti.first_in_block) CHECK_I(main_in_block, 5);
            /* Under SEQUENTIAL the cycle restarts in each block. */
            CHECK_I(ti.condition, easy[(n_warm - 1) % 2]);
            CHECK(ti.first_in_block == ((n_warm - 1) % 2 == 0));
            CHECK(ti.after_break == ((n_warm - 1) % 2 == 0));
            if (ti.first_in_block) { blocks_seen = ti.block; main_in_block = 0; }
            CHECK_I(ti.block, blocks_seen);
        } else {
            CHECK(!ti.practice);
            if (n_main == 0) {
                CHECK(ti.first_in_block);
                CHECK(!ti.after_break);
                blocks_seen = 0;
            } else {
                CHECK(!ti.first_in_block);   /* the warmups carry it */
            }
            CHECK_I(ti.block, n_main / 5);
            n_main++;
            main_in_block++;
        }
        /* Outcome 1 on everything: the practice and warmup trials must not
         * reach the tallies. */
        ytr_update(&g_t, 1, NULL);
        k++;
    }
    CHECK_I(n_prac, 5);
    CHECK_I(n_warm, 4);    /* before blocks 1 and 2, none after the last */
    CHECK_I(n_main, 12);
    for (k = 0; k < 4; k++) {
        CHECK_I(ytr_n_valid(&g_t, k), 3);
        CHECK_I(ytr_count(&g_t, k, 1), 3);
    }

    /* Warmup does not run when nothing follows it: 10 main trials in two
     * blocks of 5 end exactly at a boundary. */
    d.reps = 1;
    d.n_conditions = 10;
    d.n_practice = 0;
    CHECK(ytr_open(&g_t, &d));
    n_warm = 0;
    while (ytr_next(&g_t, &ti) >= 0) {
        if (ti.warmup) n_warm++;
        ytr_update(&g_t, 1, NULL);
    }
    CHECK_I(n_warm, 2);

    /* Random practice draws come from the list and are flagged. */
    zero_desc(&d);
    d.n_conditions = 6;
    d.reps = 2;
    d.order = YTR_ORDER_RANDOM;
    d.n_practice = 20;
    d.warmup_conditions = easy;
    d.n_warmup_conditions = 2;
    d.rng = ytr_splitmix;
    d.rng_ctx = &g_rng_state;
    CHECK(ytr_open(&g_t, &d));
    n_prac = 0;
    for (k = 0; k < 20; k++) {
        int c = ytr_condition_at(&g_t, k);
        CHECK(c == 3 || c == 1);
        if (c == 3) n_prac++;
    }
    CHECK(n_prac > 3 && n_prac < 17);
}

static void test_breaks(void) {
    ytr_desc d;
    ytr_trial_info ti = ti_zero;
    int idx, k, before[6];

    /* Two rows, 3 each, never the same twice running: the only orders are
     * 010101 and 101010. Re-queue the last trial: its copy has to follow
     * it, which breaks the rule. Without a break next() flags it; with a
     * marked break in between the run is over and it is legal. */
    zero_desc(&d);
    d.n_conditions = 2;
    d.reps = 3;
    d.order = YTR_ORDER_CONSTRAINED;
    d.constraints[0] = ytr_max_run(YTR_CONDITION, YTR_ANY_LEVEL, 1);
    d.n_constraints = 1;
    d.rng = ytr_splitmix;
    d.rng_ctx = &g_rng_state;
    g_rng_state = 3;
    CHECK(ytr_open(&g_t, &d));
    for (k = 0; k < 6; k++) before[k] = ytr_condition_at(&g_t, k);
    for (k = 1; k < 6; k++) CHECK(before[k] != before[k - 1]);
    for (k = 0; k < 5; k++) { ytr_next(&g_t, NULL); ytr_update(&g_t, 1, NULL); }
    idx = ytr_next(&g_t, &ti);
    CHECK_I(idx, 5);
    CHECK_I(ytr_requeue(&g_t), 0);
    idx = ytr_next(&g_t, &ti);
    CHECK_I(idx, 6);
    CHECK(ti.requeued);
    CHECK_I(ti.condition, before[5]);
    CHECK(hist(&g_t, 6).flags & YTR_FLAG_VIOLATION);
    ytr_update(&g_t, 1, NULL);

    /* The same with ytr_mark_break() between the two. */
    g_rng_state = 3;
    CHECK(ytr_open(&g_t, &d));
    for (k = 0; k < 5; k++) { ytr_next(&g_t, NULL); ytr_update(&g_t, 1, NULL); }
    ytr_next(&g_t, NULL);
    CHECK_I(ytr_requeue(&g_t), 0);
    CHECK_I(ytr_mark_break(&g_t), 0);
    idx = ytr_next(&g_t, &ti);
    CHECK(ti.after_break);
    CHECK(!ti.first_in_block);
    CHECK_I(ti.block, 0);   /* a marked break is not a block */
    CHECK(!(hist(&g_t, 6).flags & YTR_FLAG_VIOLATION));
    ytr_update(&g_t, 1, NULL);
    /* The order did not change. */
    for (k = 0; k < 6; k++) CHECK_I(ytr_condition_at(&g_t, k), before[k]);

    /* Marked with a trial pending: that trial carries it. */
    g_rng_state = 3;
    CHECK(ytr_open(&g_t, &d));
    ytr_next(&g_t, NULL);
    ytr_update(&g_t, 1, NULL);
    ytr_next(&g_t, NULL);
    CHECK_I(ytr_mark_break(&g_t), 0);
    CHECK_I(ytr_next(&g_t, &ti), 1);
    CHECK(ti.after_break);
    ytr_update(&g_t, 1, NULL);
    ytr_next(&g_t, &ti);
    CHECK(!ti.after_break);
    CHECK_I(g_t.seg_start, 1);

    /* constraints_span_blocks: a marked break does not cut, and the copy is
     * flagged as before. */
    d.constraints_span_blocks = true;
    g_rng_state = 3;
    CHECK(ytr_open(&g_t, &d));
    for (k = 0; k < 5; k++) { ytr_next(&g_t, NULL); ytr_update(&g_t, 1, NULL); }
    ytr_next(&g_t, NULL);
    ytr_requeue(&g_t);
    ytr_mark_break(&g_t);
    ytr_next(&g_t, &ti);
    CHECK(ti.after_break);
    CHECK(hist(&g_t, 6).flags & YTR_FLAG_VIOLATION);
}

/* ------------------------------------------------------------- requeue */

static void test_requeue(void) {
    ytr_desc d;
    ytr_trial_info ti = ti_zero;
    fake_track tr;
    int idx = 0, k, c0 = 0, gap_ok = 0, trials = 0;
    int seed;

    /* Gap 0: the copy goes at the end, keeps its rep, runs flagged, and is
     * tallied then. */
    zero_desc(&d);
    d.n_conditions = 3;
    d.reps = 2;
    d.n_practice = 1;
    CHECK(ytr_open(&g_t, &d));
    CHECK_I(ytr_requeue(&g_t), YTR_ERR_ORDER);
    ytr_next(&g_t, &ti);
    CHECK(ti.practice);
    CHECK_I(ytr_requeue(&g_t), YTR_ERR_ARG);   /* practice */
    ytr_update(&g_t, 1, NULL);
    idx = ytr_next(&g_t, &ti);                   /* slot 1: row 0, rep 0 */
    c0 = ti.condition;
    CHECK_I(c0, 0);
    CHECK_I(ytr_requeue(&g_t), 0);
    CHECK_I(ytr_n_scheduled(&g_t), 8);
    CHECK_I(ytr_condition_at(&g_t, 7), 0);
    CHECK_I(ytr_n_valid(&g_t, 0), 0);
    CHECK_I(hist(&g_t, idx).outcome, YTR_REQUEUE);
    CHECK(hist(&g_t, idx).flags & YTR_FLAG_DONE);
    CHECK_I(ytr_count(&g_t, 0, YTR_REQUEUE), 1);
    CHECK(is_nan(ytr_proportion(&g_t, 0, YTR_REQUEUE)));
    while ((idx = ytr_next(&g_t, &ti)) >= 0) {
        if (idx == 7) {
            CHECK(ti.requeued);
            CHECK_I(ti.condition, 0);
            CHECK_I(ti.rep, 0);
        } else {
            CHECK(!ti.requeued);
        }
        ytr_update(&g_t, 1, NULL);
    }
    CHECK_I(ytr_n_valid(&g_t, 0), 2);
    CHECK_I(ytr_n_run(&g_t), 8);

    /* Gap 3: at least three scheduled trials before the copy, over seeds. */
    for (seed = 0; seed < 100; seed++) {
        zero_desc(&d);
        d.n_conditions = 10;
        d.reps = 1;
        d.order = YTR_ORDER_FULL_RANDOM;
        d.requeue_gap = 3;
        d.rng = ytr_splitmix;
        d.rng_ctx = &g_rng_state;
        g_rng_state = (uint64_t)seed;
        CHECK(ytr_open(&g_t, &d));
        ytr_next(&g_t, &ti);
        ytr_next(&g_t, NULL);
        ytr_update(&g_t, 1, NULL);
        ytr_next(&g_t, &ti);      /* trial 1 */
        c0 = ti.condition;
        CHECK_I(ytr_requeue(&g_t), 0);
        k = 0;
        while ((idx = ytr_next(&g_t, &ti)) >= 0) {
            if (ti.requeued) {
                CHECK_I(ti.condition, c0);
                if (k >= 3) gap_ok++;
                trials++;
            }
            k++;
            ytr_update(&g_t, 1, NULL);
        }
    }
    CHECK_I(trials, 100);
    CHECK_I(gap_ok, 100);

    /* A copy that would follow its own row goes later when another row
     * fits: re-queue repeatedly under a no-repeat rule and check the run
     * against the checker; only flagged trials may break it. */
    {
        static int rseq[YTR_MAX_TRIALS], idxmap[YTR_MAX_TRIALS], mark[YTR_MAX_TRIALS];
        static bool rcut[YTR_MAX_TRIALS];
        design g;
        ytr_constraint cons[1];
        int flagged = 0, broken = 0, rn, requeues = 0;
        cons[0] = ytr_max_run(YTR_CONDITION, YTR_ANY_LEVEL, 1);
        memset(&g, 0, sizeof(g));
        g.c = cons;
        g.n_c = 1;
        for (seed = 0; seed < 100; seed++) {
            zero_desc(&d);
            d.n_conditions = 3;
            d.reps = 10;
            d.order = YTR_ORDER_CONSTRAINED;
            d.constraints[0] = cons[0];
            d.n_constraints = 1;
            d.requeue_gap = (seed % 2) ? 1 : 0;
            d.rng = ytr_splitmix;
            d.rng_ctx = &g_rng_state;
            g_rng_state = (uint64_t)seed + 77u;
            g_test_state = (uint64_t)seed;
            CHECK(ytr_open(&g_t, &d));
            while ((idx = ytr_next(&g_t, &ti)) >= 0) {
                if (test_u() < 0.2 && ytr_n_scheduled(&g_t) < YTR_MAX_TRIALS - 40) {
                    CHECK_I(ytr_requeue(&g_t), 0);
                    requeues++;
                } else {
                    ytr_update(&g_t, 1, NULL);
                }
            }
            rn = realized_of(&g_t, rseq, rcut, idxmap, false);
            find_violations(&g, rseq, rcut, rn, mark);
            for (k = 0; k < rn; k++) {
                bool fl = (hist(&g_t, idxmap[k]).flags & YTR_FLAG_VIOLATION) != 0;
                if (fl) flagged++;
                if (mark[k] && !fl) broken++;
                if (fl && !mark[k]) fail(__LINE__, "a flagged trial breaks no rule");
            }
        }
        printf("  re-queues under a no-repeat rule: %d re-queues, %d flagged, %d unflagged "
               "violations\n", requeues, flagged, broken);
        CHECK_I(broken, 0);
        CHECK(requeues > 0);
    }

    /* A track trial cannot be re-queued. */
    zero_desc(&d);
    fake_init(&tr, &g_t, 0, 3);
    d.tracks[0] = ytr_track(&tr, fake_done);
    d.n_tracks = 1;
    CHECK(ytr_open(&g_t, &d));
    ytr_next(&g_t, &ti);
    CHECK_I(ti.track, 0);
    CHECK_I(ytr_requeue(&g_t), YTR_ERR_ARG);
    CHECK_I(ytr_update(&g_t, 1, NULL), 0);
}

/* --------------------------------------------------------------- tallies */

static void test_tallies(void) {
    ytr_desc d;
    ytr_trial_info ti = ti_zero;
    int want_valid[4] = { 0, 0, 0, 0 }, want_one[4] = { 0, 0, 0, 0 };
    int want_two[4] = { 0, 0, 0, 0 }, want_inv[4] = { 0, 0, 0, 0 };
    int idx, out, c;

    zero_desc(&d);
    d.n_conditions = 4;
    d.reps = 15;
    d.order = YTR_ORDER_FULL_RANDOM;
    d.n_practice = 3;
    d.rng = ytr_splitmix;
    d.rng_ctx = &g_rng_state;
    g_rng_state = 11;
    g_test_state = 12;
    CHECK(ytr_open(&g_t, &d));
    while ((idx = ytr_next(&g_t, &ti)) >= 0) {
        double u = test_u();
        out = u < 0.1 ? YTR_INVALID : (u < 0.5 ? 1 : (u < 0.8 ? 0 : 2));
        CHECK_I(ytr_update(&g_t, out, NULL), 0);
        if (ti.practice) continue;
        c = ti.condition;
        if (out == YTR_INVALID) { want_inv[c]++; continue; }
        want_valid[c]++;
        if (out == 1) want_one[c]++;
        if (out == 2) want_two[c]++;
    }
    for (c = 0; c < 4; c++) {
        CHECK_I(ytr_n_valid(&g_t, c), want_valid[c]);
        CHECK_I(ytr_count(&g_t, c, 1), want_one[c]);
        CHECK_I(ytr_count(&g_t, c, 2), want_two[c]);
        CHECK_I(ytr_count(&g_t, c, YTR_INVALID), want_inv[c]);
        CHECK(ytr_proportion(&g_t, c, 1) == (double)want_one[c] / (double)want_valid[c]);
        CHECK(ytr_proportion(&g_t, c, 2) == (double)want_two[c] / (double)want_valid[c]);
        CHECK(is_nan(ytr_proportion(&g_t, c, YTR_INVALID)));
    }
    CHECK_I(ytr_count(&g_t, 4, 1), YTR_ERR_ARG);
    CHECK(is_nan(ytr_proportion(&g_t, 4, 1)));
    CHECK_I(ytr_n_valid(&g_t, 4), 0);

    /* No valid trials yet: NaN, not 0. */
    CHECK(ytr_open(&g_t, &d));
    CHECK(is_nan(ytr_proportion(&g_t, 0, 1)));
}

/* ------------------------------------------ restore, save and load */

/* One session with everything that changes state: practice, blocks with
 * warmups, constraints, two tracks, re-queues, invalid trials, marked
 * breaks (when `breaks`), and a record per trial. */
typedef struct rig {
    ytr_desc d;
    fake_track tr[2];
    uint64_t   seed;
    int        records[YTR_MAX_TRIALS];
} rig;

static const int rig_easy[2] = { 0, 5 };

static void rig_desc(rig* r, ytr_trials* t, bool tracks) {
    zero_desc(&r->d);
    r->d.factors[0] = ytr_factor("side", 2);
    r->d.factors[1] = ytr_factor("level", 3);
    r->d.n_factors = 2;
    r->d.reps = 12;                /* 72 scheduled */
    r->d.order = YTR_ORDER_CONSTRAINED;
    r->d.constraints[0] = ytr_max_run(0, YTR_ANY_LEVEL, 2);
    r->d.constraints[1] = ytr_min_gap(1, 2, 1);
    r->d.n_constraints = 2;
    r->d.block_size = 25;
    r->d.n_practice = 4;
    r->d.n_warmup = 2;
    r->d.warmup_conditions = rig_easy;
    r->d.n_warmup_conditions = 2;
    r->d.requeue_gap = 2;
    r->d.records = r->records;
    r->d.record_size = sizeof(int);
    r->d.rng = ytr_splitmix;
    r->d.rng_ctx = &r->seed;
    if (tracks) {
        fake_init(&r->tr[0], t, 0, 20);
        fake_init(&r->tr[1], t, 1, 30);
        r->d.tracks[0] = ytr_track(&r->tr[0], fake_done);
        r->d.tracks[1] = ytr_track(&r->tr[1], fake_done);
        r->d.tracks[1].weight = 2.0;
        r->d.n_tracks = 2;
        r->d.track_rate = 0.4;
    }
}

/* Run `steps` loop iterations (or to the end with -1). Every decision is a
 * function of the trial index, so two runs that reach the same state make
 * the same calls. `pending_at` stops after next() at that trial. */
static int rig_run(ytr_trials* t, int steps, bool breaks, int pending_at) {
    ytr_trial_info ti = ti_zero;
    int idx, k = 0, out, rec;
    while (steps < 0 || k < steps) {
        if (breaks && ytr_n_run(t) % 23 == 11 && t->current < 0) ytr_mark_break(t);
        idx = ytr_next(t, &ti);
        if (idx < 0) return idx;
        if (idx == pending_at) return idx;
        if (breaks && idx % 29 == 17) ytr_mark_break(t);
        out = outcome_for(idx, ti.condition);
        rec = idx * 10 + 1;
        if (!ti.practice && !ti.warmup && ti.track < 0 && idx % 9 == 4) {
            if (ytr_requeue(t) != 0) fail(__LINE__, "requeue");
        } else if (idx % 13 == 6) {
            if (ytr_update(t, YTR_INVALID, &rec) != 0) fail(__LINE__, "update");
        } else {
            if (ytr_update(t, out, &rec) != 0) fail(__LINE__, "update");
        }
        k++;
    }
    return 0;
}

static bool same_state(const ytr_trials* a, const ytr_trials* b) {
    int i;
    bool ok = true;
    if (a->n_run != b->n_run || a->n_done != b->n_done || a->n_scheduled != b->n_scheduled ||
        a->schedule_pos != b->schedule_pos || a->n_main != b->n_main || a->current != b->current)
        ok = false;
    for (i = 0; ok && i < a->n_run; i++)
        if (memcmp(&a->history[i], &b->history[i], sizeof(ytr_trial)) != 0) ok = false;
    for (i = 0; ok && i < a->n_scheduled; i++)
        if (a->schedule[i] != b->schedule[i] || a->schedule_rep[i] != b->schedule_rep[i] ||
            a->schedule_flags[i] != b->schedule_flags[i]) ok = false;
    for (i = 0; ok && i < a->n_cond; i++)
        if (a->tally_valid[i] != b->tally_valid[i] || a->tally_pos[i] != b->tally_pos[i]) ok = false;
    for (i = 0; ok && i < a->n_run; i++)
        if (memcmp(ytr_record(a, i), ytr_record(b, i), sizeof(int)) != 0) ok = false;
    return ok;
}

static rig g_ra, g_rb;
static unsigned char g_snap_a[YTR_MAX_TRIALS * 40 + 4096];
static unsigned char g_snap_b[YTR_MAX_TRIALS * 40 + 4096];

static void test_restore(void) {
    static int outcomes[YTR_MAX_TRIALS];
    const ytr_trial* h;
    int n = 0, i, pass, n_req = 0, n_warm = 0, n_tr = 0;
    bool tracks;

    for (pass = 0; pass < 2; pass++) {
        tracks = pass == 1;
        memset(&g_ra, 0, sizeof(g_ra));
        memset(&g_rb, 0, sizeof(g_rb));
        rig_desc(&g_ra, &g_t, tracks);
        g_ra.seed = 99;
        CHECK(ytr_open(&g_t, &g_ra.d));
        CHECK_I(rig_run(&g_t, -1, false, -1), YTR_DONE);
        h = ytr_history(&g_t, &n);
        n_req = 0;
        n_warm = 0;
        n_tr = 0;
        for (i = 0; i < n; i++) {
            outcomes[i] = h[i].outcome;
            if (h[i].outcome == YTR_REQUEUE) n_req++;
            if (h[i].flags & YTR_FLAG_WARMUP) n_warm++;
            if (h[i].track >= 0) n_tr++;
        }
        CHECK(n_req > 0);
        CHECK(n_warm > 0);
        if (tracks) CHECK_I(n_tr, 50);

        rig_desc(&g_rb, &g_u, tracks);
        g_rb.seed = 99;
        CHECK(ytr_open(&g_u, &g_rb.d));
        CHECK_I(ytr_restore(&g_u, outcomes, g_ra.records, n), 0);
        CHECK(same_state(&g_t, &g_u));
        CHECK(g_ra.seed == g_rb.seed);    /* the same draws, the same count */
        CHECK_I(ytr_restore(&g_u, outcomes, NULL, n), YTR_ERR_ORDER);
        printf("  restore %s tracks: %d trials, %d re-queues, %d warmups, identical\n",
               tracks ? "with" : "without", n, n_req, n_warm);
    }
    /* More outcomes than trials. */
    rig_desc(&g_rb, &g_u, false);
    g_rb.seed = 99;
    CHECK(ytr_open(&g_u, &g_rb.d));
    outcomes[n] = 1;
    CHECK_I(ytr_restore(&g_u, outcomes, NULL, n + 1), YTR_ERR_ARG);
}

static void test_save_load(void) {
    int cut, sz, sz2, n_cuts = 0, pend;
    uint64_t seed_at_cut;

    /* Run A through; for each cut point, run A' to the cut, snapshot it,
     * load into B with the generator state as it was, finish B, and
     * compare B with A byte for byte, snapshot against snapshot. */
    memset(&g_ra, 0, sizeof(g_ra));
    rig_desc(&g_ra, &g_t, true);
    g_ra.seed = 2026;
    CHECK(ytr_open(&g_t, &g_ra.d));
    CHECK_I(rig_run(&g_t, -1, true, -1), YTR_DONE);
    sz = ytr_save(&g_t, g_snap_a, sizeof(g_snap_a));
    CHECK(sz > 0);
    CHECK_I((size_t)sz, ytr_save_size(&g_t));

    for (cut = 0; cut < ytr_n_run(&g_t); cut += 7) {
        for (pend = 0; pend < 2; pend++) {
            memset(&g_rb, 0, sizeof(g_rb));
            rig_desc(&g_rb, &g_u, true);
            g_rb.seed = 2026;
            CHECK(ytr_open(&g_u, &g_rb.d));
            if (pend) CHECK(rig_run(&g_u, -1, true, cut) >= 0);
            else rig_run(&g_u, cut, true, -1);
            sz2 = ytr_save(&g_u, g_snap_b, sizeof(g_snap_b));
            CHECK(sz2 > 0);
            seed_at_cut = g_rb.seed;

            /* A fresh rig: the records buffer and the tracks start empty,
             * the tracks point at the loading handle. */
            memset(&g_rb, 0, sizeof(g_rb));
            memset(&g_u, 0xA5, sizeof(g_u));   /* load must not rely on zeroed */
            rig_desc(&g_rb, &g_u, true);
            g_rb.seed = seed_at_cut;
            if (!ytr_open_saved(&g_u, &g_rb.d, g_snap_b, (size_t)sz2)) {
                fail(__LINE__, ytr_error(&g_u));
                continue;
            }
            CHECK_I(rig_run(&g_u, -1, true, -1), YTR_DONE);
            CHECK(same_state(&g_t, &g_u));
            sz2 = ytr_save(&g_u, g_snap_b, sizeof(g_snap_b));
            CHECK_I(sz2, sz);
            CHECK(memcmp(g_snap_a, g_snap_b, (size_t)sz) == 0);
            CHECK(g_rb.seed == g_ra.seed);
            n_cuts++;
        }
    }
    printf("  save/load: %d resumes (after update and with a trial pending), "
           "all identical to the uninterrupted run; snapshot %d bytes\n", n_cuts, sz);

    /* Refusals, each with its message. */
    memset(&g_rb, 0, sizeof(g_rb));
    rig_desc(&g_rb, &g_u, true);
    g_rb.d.reps = 11;
    CHECK(!ytr_open_saved(&g_u, &g_rb.d, g_snap_a, (size_t)sz));
    CHECK_HAS(ytr_error(&g_u), "desc.reps does not match");
    CHECK(!ytr_is_open(&g_u));
    rig_desc(&g_rb, &g_u, true);
    g_rb.d.constraints[1] = ytr_min_gap(1, 2, 2);
    CHECK(!ytr_open_saved(&g_u, &g_rb.d, g_snap_a, (size_t)sz));
    CHECK_HAS(ytr_error(&g_u), "desc.constraints[].n does not match");
    rig_desc(&g_rb, &g_u, true);
    g_rb.d.tracks[1].weight = 3.0;
    CHECK(!ytr_open_saved(&g_u, &g_rb.d, g_snap_a, (size_t)sz));
    CHECK_HAS(ytr_error(&g_u), "tracks[].weight");
    rig_desc(&g_rb, &g_u, true);
    g_rb.d.record_size = 0;
    g_rb.d.records = NULL;
    CHECK(!ytr_open_saved(&g_u, &g_rb.d, g_snap_a, (size_t)sz));
    CHECK_HAS(ytr_error(&g_u), "record_size");
    rig_desc(&g_rb, &g_u, true);
    CHECK(!ytr_open_saved(&g_u, &g_rb.d, g_snap_a, (size_t)sz - 1));
    CHECK_HAS(ytr_error(&g_u), "truncated");
    CHECK(!ytr_open_saved(&g_u, &g_rb.d, g_snap_a, 20));
    CHECK(!ytr_open_saved(&g_u, &g_rb.d, g_snap_a, 3));
    memcpy(g_snap_b, g_snap_a, (size_t)sz);
    g_snap_b[sz] = 0;
    CHECK(!ytr_open_saved(&g_u, &g_rb.d, g_snap_b, (size_t)sz + 1));
    CHECK_HAS(ytr_error(&g_u), "after the snapshot's end");
    g_snap_b[0] = 'X';
    CHECK(!ytr_open_saved(&g_u, &g_rb.d, g_snap_b, (size_t)sz));
    CHECK_HAS(ytr_error(&g_u), "not a ysp_trials snapshot");
    memcpy(g_snap_b, g_snap_a, (size_t)sz);
    g_snap_b[4] = 2;
    CHECK(!ytr_open_saved(&g_u, &g_rb.d, g_snap_b, (size_t)sz));
    CHECK_HAS(ytr_error(&g_u), "format");
    /* A schedule entry out of range: find the state section by saving a
     * handle and corrupting the first schedule byte pair. The desc section
     * of rig_desc() is fixed, so its length is the offset. */
    {
        ytr__w w;
        memset(&w, 0, sizeof(w));
        w.pos = 8;
        ytr__put_desc(&w, &g_t);
        memcpy(g_snap_b, g_snap_a, (size_t)sz);
        g_snap_b[w.pos + 13 * 4] = 0x7f;   /* schedule[0], low byte */
        g_snap_b[w.pos + 13 * 4 + 1] = 0x00;
        CHECK(!ytr_open_saved(&g_u, &g_rb.d, g_snap_b, (size_t)sz));
        CHECK_HAS(ytr_error(&g_u), "corrupt");
        memcpy(g_snap_b, g_snap_a, (size_t)sz);
        g_snap_b[w.pos + 4] = 0xff;        /* n_run */
        CHECK(!ytr_open_saved(&g_u, &g_rb.d, g_snap_b, (size_t)sz));
        CHECK_HAS(ytr_error(&g_u), "counters");
    }
    CHECK(!ytr_open_saved(&g_u, &g_rb.d, NULL, 100));
    CHECK(!ytr_open_saved(&g_u, NULL, g_snap_a, (size_t)sz));
    CHECK(!ytr_open_saved(NULL, &g_rb.d, g_snap_a, (size_t)sz));

    /* save() into a buffer that is too small. */
    CHECK_I(ytr_save(&g_t, g_snap_b, (size_t)sz - 1), YTR_ERR_ARG);
    CHECK_I(ytr_save(&g_t, NULL, 0), YTR_ERR_ARG);
}

/* ------------------------------------------------------------------ full */

static void test_full(void) {
    ytr_desc d;
    fake_track tr;
    int k, rc = 0;

    /* A track that never finishes fills the history. */
    zero_desc(&d);
    fake_init(&tr, &g_t, 0, 1000000);
    d.tracks[0] = ytr_track(&tr, fake_done);
    d.n_tracks = 1;
    CHECK(ytr_open(&g_t, &d));
    for (k = 0; k < YTR_MAX_TRIALS; k++) {
        rc = ytr_next(&g_t, NULL);
        if (rc != k) { fail_i(__LINE__, "next", rc, k); break; }
        ytr_update(&g_t, 1, NULL);
    }
    CHECK_I(ytr_next(&g_t, NULL), YTR_ERR_FULL);
    CHECK(!ytr_done(&g_t));

    /* A schedule that fills the history exactly: a re-queue cannot fit. */
    zero_desc(&d);
    d.n_conditions = 1;
    d.reps = YTR_MAX_TRIALS;
    CHECK(ytr_open(&g_t, &d));
    ytr_next(&g_t, NULL);
    CHECK_I(ytr_requeue(&g_t), YTR_ERR_FULL);
    CHECK_I(g_t.history[0].outcome, YTR_INVALID);
    CHECK(g_t.history[0].flags & YTR_FLAG_DONE);
    CHECK_I(ytr_n_scheduled(&g_t), YTR_MAX_TRIALS);
    CHECK_I(ytr_next(&g_t, NULL), 1);

    /* Too many at open. */
    d.reps = YTR_MAX_TRIALS + 1;
    CHECK(!ytr_open(&g_t, &d));
    d.reps = YTR_MAX_TRIALS;
    d.n_practice = 1;
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "exceed YTR_MAX_TRIALS");
    /* Warmups count when they are known. */
    d.n_practice = 0;
    d.reps = YTR_MAX_TRIALS - 4;
    d.block_size = YTR_MAX_TRIALS / 2;
    d.n_warmup = 5;
    CHECK(!ytr_open(&g_t, &d));
    d.n_warmup = 4;
    CHECK(ytr_open(&g_t, &d));
}

/* ------------------------------------------------------------- the loop */

static void test_loop_errors(void) {
    ytr_desc d;
    ytr_trial_info ti = ti_zero;
    int idx;
    zero_desc(&d);
    d.n_conditions = 2;
    d.reps = 1;
    CHECK(ytr_open(&g_t, &d));
    CHECK_I(ytr_update(&g_t, 1, NULL), YTR_ERR_ORDER);
    idx = ytr_next(&g_t, &ti);
    CHECK_I(idx, 0);
    CHECK_I(ytr_next(&g_t, &ti), 0);
    CHECK_I(ytr_n_run(&g_t), 1);
    CHECK_I(ytr_n_done(&g_t), 0);
    CHECK(!ytr_done(&g_t));
    CHECK_I(ytr_update(&g_t, -2, NULL), YTR_ERR_ARG);
    CHECK_I(ytr_update(&g_t, -7, NULL), YTR_ERR_ARG);
    CHECK_I(ytr_update(&g_t, 12345, NULL), 0);
    CHECK_I(ytr_n_done(&g_t), 1);
    CHECK_I(g_t.history[0].outcome, 12345);
    /* A record pointer with no records buffer is ignored. */
    ytr_next(&g_t, NULL);
    CHECK_I(ytr_update(&g_t, 0, &idx), 0);
    CHECK(ytr_record(&g_t, 0) == NULL);
    CHECK_I(ytr_next(&g_t, NULL), YTR_DONE);
    CHECK_I(ytr_next(&g_t, NULL), YTR_DONE);
    CHECK(ytr_done(&g_t));
    CHECK_I(ytr_mark_break(&g_t), 0);

    /* Records: copied, and NULL zero-fills. */
    {
        double recs[YTR_MAX_TRIALS];
        double v = 2.5;
        memset(recs, 0x7f, sizeof(recs));
        d.records = recs;
        d.record_size = sizeof(double);
        CHECK(ytr_open(&g_t, &d));
        ytr_next(&g_t, NULL);
        ytr_update(&g_t, 1, &v);
        ytr_next(&g_t, NULL);
        ytr_update(&g_t, 1, NULL);
        CHECK(rec_d(&g_t, 0) == 2.5);
        CHECK(rec_d(&g_t, 1) == 0.0);
        CHECK(ytr_record(&g_t, 2) == NULL);
        CHECK(ytr_record(&g_t, -1) == NULL);
    }
}

static void test_closed(void) {
    static ytr_trials z;
    ytr_trial_info ti = ti_zero;
    int n = 5;
    char buf[8];
    memset(&z, 0, sizeof(z));
    CHECK(!ytr_is_open(&z));
    CHECK(!ytr_is_open(NULL));
    CHECK_I(ytr_next(&z, &ti), YTR_ERR_CLOSED);
    CHECK_I(ytr_next(NULL, &ti), YTR_ERR_ARG);
    CHECK_I(ytr_update(&z, 1, NULL), YTR_ERR_CLOSED);
    CHECK_I(ytr_update(NULL, 1, NULL), YTR_ERR_ARG);
    CHECK_I(ytr_requeue(&z), YTR_ERR_CLOSED);
    CHECK_I(ytr_mark_break(&z), YTR_ERR_CLOSED);
    CHECK_I(ytr_restore(&z, NULL, NULL, 0), YTR_ERR_CLOSED);
    CHECK_I(ytr_n_conditions(&z), 0);
    CHECK_I(ytr_n_factors(NULL), 0);
    CHECK_I(ytr_n_scheduled(&z), 0);
    CHECK_I(ytr_n_run(&z), 0);
    CHECK_I(ytr_n_done(NULL), 0);
    CHECK_I(ytr_n_valid(&z, 0), 0);
    CHECK_I(ytr_level(&z, 0, 0), YTR_ERR_CLOSED);
    CHECK_I(ytr_condition_at(&z, 0), YTR_ERR_CLOSED);
    CHECK_I(ytr_condition_at(NULL, 0), YTR_ERR_ARG);
    CHECK_I(ytr_count(&z, 0, 1), YTR_ERR_CLOSED);
    CHECK(is_nan(ytr_proportion(&z, 0, 1)));
    CHECK(ytr_history(&z, &n) == NULL);
    CHECK_I(n, 0);
    CHECK(ytr_record(&z, 0) == NULL);
    CHECK(!ytr_done(&z));
    CHECK_I(ytr_save_size(&z), 0);
    CHECK_I(ytr_save(&z, buf, sizeof(buf)), YTR_ERR_CLOSED);
    CHECK_I(ytr_format_row(&z, 0, buf, sizeof(buf)), YTR_ERR_CLOSED);
    CHECK_I(ytr_format_header(NULL, buf, sizeof(buf)), YTR_ERR_ARG);
    CHECK_I(ytr_format_meta(&z, buf, sizeof(buf)), YTR_ERR_CLOSED);
    CHECK(!ytr_open(NULL, NULL));
    CHECK(!ytr_open(&z, NULL));
    CHECK_HAS(ytr_error(&z), "null desc");
    CHECK_HAS(ytr_error(NULL), "null handle");
    CHECK_S(ytr_strerror(YTR_ERR_FULL), "trial history is full");
    CHECK_S(ytr_strerror(3), "ok");
    CHECK_S(ytr_strerror(-99), "unknown error");
}

/* ------------------------------------------------------------ bad descs */

static bool dummy_done(void* ctx) { (void)ctx; return false; }

static void expect_reject(int line, const ytr_desc* d, const char* sub) {
    if (ytr_open(&g_t, d)) {
        fail(line, "open accepted a bad desc");
        return;
    }
    if (!strstr(ytr_error(&g_t), sub)) fail_s(line, "rejection message", ytr_error(&g_t), sub);
}

#define REJECT(d, sub) expect_reject(__LINE__, &(d), (sub))

static void test_bad_descs(void) {
    static const int neg[3] = { 1, -1, 1 };
    static const int zeros[3] = { 0, 0, 0 };
    static const int bad_list[1] = { 7 };
    int recs[4];
    ytr_desc d, ok;

    zero_desc(&ok);
    ok.n_conditions = 3;
    ok.reps = 2;
    ok.order = YTR_ORDER_CONSTRAINED;
    ok.rng = ytr_splitmix;
    ok.rng_ctx = &g_rng_state;
    CHECK(ytr_open(&g_t, &ok));

    zero_desc(&d);                         REJECT(d, "conditions (n_conditions or factors) or tracks");
    d = ok; d.n_conditions = -1;           REJECT(d, "n_conditions is negative");
    d = ok; d.n_factors = -1;              REJECT(d, "n_factors");
    d = ok; d.n_factors = YTR_MAX_FACTORS + 1; REJECT(d, "n_factors");
    d = ok; d.n_conditions = 0; d.n_factors = 1; d.factors[0].n_levels = 0;
                                           REJECT(d, "factors[0].n_levels");
    d = ok; d.n_conditions = 5; d.n_factors = 1; d.factors[0].n_levels = 4;
                                           REJECT(d, "not 0 or the factors' product");
    d = ok; d.n_conditions = 0; d.n_factors = 2; d.factors[0].n_levels = 64;
            d.factors[1].n_levels = YTR_MAX_CONDITIONS; REJECT(d, "above YTR_MAX_CONDITIONS");
    d = ok; d.n_conditions = YTR_MAX_CONDITIONS + 1; REJECT(d, "above YTR_MAX_CONDITIONS");
    d = ok; d.reps = 0;                    REJECT(d, "desc.reps must be at least 1");
    d = ok; d.reps = -3;                   REJECT(d, "desc.reps must be at least 1");
    d = ok; d.cond_reps = neg;             REJECT(d, "cond_reps[1] is negative");
    d = ok; d.cond_reps = zeros;           REJECT(d, "sums to 0");
    /* Out-of-range enum values: within the enum's bit range, since in C++
     * a value outside it is undefined (UBSan says so); ytr_interleave
     * has no such value in C++, so that check is C only. */
    d = ok; d.order = (ytr_order)7;      REJECT(d, "desc.order");
#ifndef __cplusplus
    d = ok; d.interleave = (ytr_interleave)5; REJECT(d, "desc.interleave");
#endif
    d = ok; d.n_constraints = -1;          REJECT(d, "n_constraints");
    d = ok; d.n_constraints = YTR_MAX_CONSTRAINTS + 1; REJECT(d, "n_constraints");
    d = ok; d.order = YTR_ORDER_RANDOM; d.constraints[0] = ytr_max_run(YTR_CONDITION, 0, 1);
            d.n_constraints = 1;           REJECT(d, "need desc.order = YTR_ORDER_CONSTRAINED");
    d = ok; d.constraints[0] = ytr_max_run(YTR_CONDITION, 0, 1); d.n_constraints = 1;
            d.constraints[0].rule = (ytr_rule)11; REJECT(d, "rule is not a ytr_rule");
    d = ok; d.constraints[0] = ytr_max_run(0, 0, 1); d.n_constraints = 1;
                                           REJECT(d, "factor is not a factor");
    d = ok; d.constraints[0] = ytr_max_run(YTR_CONDITION, 3, 1); d.n_constraints = 1;
                                           REJECT(d, "level is out of range");
    d = ok; d.constraints[0] = ytr_max_run(YTR_CONDITION, -5, 1); d.n_constraints = 1;
                                           REJECT(d, "level is out of range");
    d = ok; d.constraints[0] = ytr_max_run(YTR_CONDITION, 0, 0); d.n_constraints = 1;
                                           REJECT(d, "n must be at least 1");
    d = ok; d.constraints[0] = ytr_min_gap(YTR_CONDITION, 0, 0); d.n_constraints = 1;
                                           REJECT(d, "n must be at least 1");
    d = ok; d.constraints[0] = ytr_max_in_window(YTR_CONDITION, 0, 0, 1); d.n_constraints = 1;
                                           REJECT(d, "window and count");
    d = ok; d.constraints[0] = ytr_max_in_window(YTR_CONDITION, 0, 4, 0); d.n_constraints = 1;
                                           REJECT(d, "window and count");
    d = ok; d.constraints[0] = ytr_no_transition(YTR_CONDITION, YTR_ANY_LEVEL, 1);
            d.n_constraints = 1;           REJECT(d, "takes no YTR_ANY_LEVEL");
    d = ok; d.constraints[0] = ytr_no_transition(YTR_CONDITION, 0, 3); d.n_constraints = 1;
                                           REJECT(d, "level2 is out of range");
    d = ok; d.constraints[0] = ytr_first_not(YTR_CONDITION, YTR_ANY_LEVEL); d.n_constraints = 1;
                                           REJECT(d, "takes no YTR_ANY_LEVEL");
    d = ok; d.max_swaps = -1;              REJECT(d, "max_swaps is negative");
    d = ok; d.n_tracks = -1;               REJECT(d, "n_tracks");
    d = ok; d.n_tracks = YTR_MAX_TRACKS + 1; REJECT(d, "n_tracks");
    d = ok; d.n_tracks = 1; d.track_rate = 0.5; REJECT(d, "tracks[0].is_done is NULL");
    d = ok; d.n_tracks = 1; d.track_rate = 0.5; d.tracks[0] = ytr_track(NULL, dummy_done);
            d.tracks[0].weight = -1.0;     REJECT(d, "tracks[0].weight");
    d = ok; d.track_rate = 1.5;            REJECT(d, "track_rate must be in [0, 1]");
    d = ok; d.track_rate = -0.1;           REJECT(d, "track_rate must be in [0, 1]");
    d = ok; d.n_tracks = 1; d.tracks[0] = ytr_track(NULL, dummy_done);
                                           REJECT(d, "track_rate is required");
    d = ok; d.block_size = -1;             REJECT(d, "block_size is negative");
    d = ok; d.n_practice = -1;             REJECT(d, "must not be negative");
    d = ok; d.n_warmup = -1;               REJECT(d, "must not be negative");
    d = ok; d.n_warmup = 1;                REJECT(d, "n_warmup needs desc.block_size");
    zero_desc(&d); d.n_tracks = 1; d.tracks[0] = ytr_track(NULL, dummy_done); d.n_practice = 1;
                                           REJECT(d, "practice and warmup trials need conditions");
    d = ok; d.n_warmup_conditions = -1;    REJECT(d, "n_warmup_conditions is negative");
    d = ok; d.n_warmup_conditions = 1;     REJECT(d, "without desc.warmup_conditions");
    d = ok; d.n_warmup_conditions = YTR_MAX_CONDITIONS + 1; d.warmup_conditions = bad_list;
                                           REJECT(d, "n_warmup_conditions is above YTR_MAX_CONDITIONS");
    d = ok; d.n_warmup_conditions = 1; d.warmup_conditions = bad_list;
                                           REJECT(d, "warmup_conditions[0] is not a condition");
    d = ok; d.requeue_gap = -1;            REJECT(d, "requeue_gap is negative");
    d = ok; d.record_size = 4;             REJECT(d, "record_size needs desc.records");
    d = ok; d.rng = NULL;                  REJECT(d, "rng is required for an order other than SEQUENTIAL");
    zero_desc(&d); d.n_tracks = 2; d.tracks[0] = ytr_track(NULL, dummy_done);
            d.tracks[1] = d.tracks[0];     REJECT(d, "RANDOM interleave of more than one track");
    zero_desc(&d); d.n_conditions = 1; d.reps = 1; d.n_tracks = 1;
            d.tracks[0] = ytr_track(NULL, dummy_done); d.track_rate = 0.5;
                                           REJECT(d, "track_rate below 1");
    zero_desc(&d); d.n_conditions = 1; d.reps = 1; d.requeue_gap = 2;
                                           REJECT(d, "requeue_gap");

    /* The accepted counterparts that need no generator. */
    zero_desc(&d); d.n_tracks = 2; d.tracks[0] = ytr_track(NULL, dummy_done);
    d.tracks[1] = d.tracks[0]; d.interleave = YTR_INTERLEAVE_ROUND_ROBIN;
    CHECK(ytr_open(&g_t, &d));
    zero_desc(&d); d.n_conditions = 2; d.reps = 1; d.n_practice = 3; d.block_size = 1;
    d.n_warmup = 1;
    CHECK(ytr_open(&g_t, &d));
    zero_desc(&d); d.n_conditions = 1; d.reps = 1; d.n_tracks = 1;
    d.tracks[0] = ytr_track(NULL, dummy_done); d.track_rate = 1.0;
    CHECK(ytr_open(&g_t, &d));
    zero_desc(&d); d.n_conditions = 1; d.reps = 1; d.records = recs; d.record_size = sizeof(int);
    CHECK(ytr_open(&g_t, &d));
    CHECK_S(ytr_error(&g_t), "");
}

/* ---------------------------------------------------------------- format */

static void test_format(void) {
    static double levels[YTR_MAX_TRIALS];
    ytr_desc d;
    fake_track tr;
    char buf[1024], tiny[8];
    int n = 0;

    zero_desc(&d);
    d.factors[0] = ytr_factor("orientation", 2);
    d.factors[1] = ytr_factor(NULL, 3);
    d.factors[2] = ytr_factor("a,\"b\"", 1);
    d.n_factors = 3;
    d.reps = 1;
    d.n_practice = 1;
    fake_init(&tr, &g_t, 0, 1);
    d.tracks[0] = ytr_track(&tr, fake_done);
    d.n_tracks = 1;
    d.track_rate = 1.0;
    d.records = levels;
    d.record_size = sizeof(double);
    CHECK(ytr_open(&g_t, &d));

    n = ytr_format_header(&g_t, buf, sizeof(buf));
    CHECK_S(buf, "index,block,rep,condition,track,practice,warmup,requeued,after_break,outcome,"
                 "orientation,factor1,\"a,\"\"b\"\"\"\n");
    CHECK_I(n, (long)strlen(buf));

    ytr_next(&g_t, NULL);        /* practice, condition 0 */
    n = ytr_format_row(&g_t, 0, buf, sizeof(buf));
    CHECK_S(buf, "0,-1,-1,0,-1,1,0,0,0,,0,0,0\n");   /* pending: no outcome */
    ytr_update(&g_t, 1, NULL);
    ytr_format_row(&g_t, 0, buf, sizeof(buf));
    CHECK_S(buf, "0,-1,-1,0,-1,1,0,0,0,1,0,0,0\n");
    ytr_next(&g_t, NULL);        /* the track (rate 1) */
    ytr_update(&g_t, YTR_INVALID, NULL);
    ytr_format_row(&g_t, 1, buf, sizeof(buf));
    CHECK_S(buf, "1,0,-1,-1,0,0,0,0,0,-1,,,\n");
    ytr_next(&g_t, NULL);        /* scheduled condition 0 */
    ytr_requeue(&g_t);
    ytr_mark_break(&g_t);
    ytr_next(&g_t, NULL);        /* condition 1: levels 0,1,0 */
    ytr_update(&g_t, 0, NULL);
    ytr_format_row(&g_t, 2, buf, sizeof(buf));
    CHECK_S(buf, "2,0,0,0,-1,0,0,0,0,-2,0,0,0\n");
    ytr_format_row(&g_t, 3, buf, sizeof(buf));
    CHECK_S(buf, "3,0,0,1,-1,0,0,0,1,0,0,1,0\n");
    CHECK_I(ytr_format_row(&g_t, 4, buf, sizeof(buf)), YTR_ERR_ARG);

    /* snprintf semantics: the full length back, a terminated prefix. */
    n = ytr_format_row(&g_t, 3, tiny, sizeof(tiny));
    CHECK_I(n, (long)strlen("3,0,0,1,-1,0,0,0,1,0,0,1,0\n"));
    CHECK_S(tiny, "3,0,0,1");
    CHECK_I(ytr_format_row(&g_t, 3, NULL, 0), n);

    n = ytr_format_meta(&g_t, buf, sizeof(buf));
    CHECK_S(buf, "ysp_trials=0.2.1 conditions=6 factors=3 levels=2x3x1 reps=1 "
                 "order=sequential constraints= max_swaps=100000 swaps=0 span_blocks=0 "
                 "tracks=1 interleave=random weights=1 track_rate=1 block_size=0 "
                 "practice=1 warmup=0 warmup_conditions= requeue_gap=0 record_size=8 "
                 "scheduled=8 run=4 done=4\n");
    CHECK_I(n, (long)strlen(buf));

    zero_desc(&d);
    d.n_conditions = 3;
    d.cond_reps = catch_reps;
    d.order = YTR_ORDER_CONSTRAINED;
    d.constraints[0] = ytr_max_in_window(YTR_CONDITION, 0, 5, 2);
    d.constraints[1] = ytr_no_transition(YTR_CONDITION, 1, 2);
    d.n_constraints = 2;
    d.max_swaps = 500;
    d.rng = ytr_splitmix;
    d.rng_ctx = &g_rng_state;
    CHECK(ytr_open(&g_t, &d));
    ytr_format_meta(&g_t, buf, sizeof(buf));
    CHECK_HAS(buf, " cond_reps=8,8,8 order=constrained "
                   "constraints=max_in_window(cond,0,5,2);no_transition(cond,1,2) max_swaps=500 ");
}

/* ---------------------------------------------- the second USAGE example */

static void test_catch_with_tracks(void) {
    static int rseq[YTR_MAX_TRIALS], idxmap[YTR_MAX_TRIALS], mark[YTR_MAX_TRIALS];
    static bool rcut[YTR_MAX_TRIALS];
    ytr_desc d;
    fake_track tr[3];
    ytr_trial_info ti = ti_zero;
    design g;
    ytr_constraint cons[1];
    int seed, i, rn, k, flagged = 0, broken = 0, sessions_flagged = 0;
    int n_catch;

    cons[0] = ytr_min_gap(YTR_CONDITION, 0, 1);
    memset(&g, 0, sizeof(g));
    g.c = cons;
    g.n_c = 1;
    for (seed = 0; seed < 200; seed++) {
        zero_desc(&d);
        d.n_conditions = 1;
        d.reps = 20;
        d.order = YTR_ORDER_CONSTRAINED;
        d.constraints[0] = cons[0];
        d.n_constraints = 1;
        for (i = 0; i < 3; i++) {
            fake_init(&tr[i], &g_t, i, 50 + 5 * i);
            d.tracks[i] = ytr_track(&tr[i], fake_done);
        }
        d.n_tracks = 3;
        d.track_rate = 0.9;
        d.max_swaps = 1000;
        d.rng = ytr_splitmix;
        d.rng_ctx = &g_rng_state;
        g_rng_state = (uint64_t)seed;
        if (!ytr_open(&g_t, &d)) { fail(__LINE__, ytr_error(&g_t)); continue; }
        n_catch = 0;
        while (ytr_next(&g_t, &ti) >= 0) {
            if (ti.track < 0) n_catch++;
            ytr_update(&g_t, 1, NULL);
        }
        CHECK_I(n_catch, 20);
        rn = realized_of(&g_t, rseq, rcut, idxmap, false);
        find_violations(&g, rseq, rcut, rn, mark);
        k = 0;
        for (i = 0; i < rn; i++) {
            bool fl = (hist(&g_t, idxmap[i]).flags & YTR_FLAG_VIOLATION) != 0;
            if (fl) { flagged++; k++; }
            if (mark[i] && !fl) broken++;
        }
        if (k) sessions_flagged++;
    }
    printf("  catch 1 in 10 with 3 tracks, 200 seeds: %d unflagged violations, %d flagged "
           "trials in %d sessions whose tracks ended first\n", broken, flagged,
           sessions_flagged);
    CHECK_I(broken, 0);
}

/* -------------------------------------------------------------------- main */

#include "trials_test_v02.h"

int main(void) {
    test_version();
    test_splitmix();
    test_factorial();
    test_orders();
    test_draw_order();
    test_constraint_orders();
    test_impossible();
    test_tracks();
    test_practice_warmup();
    test_breaks();
    test_requeue();
    test_tallies();
    test_restore();
    test_save_load();
    test_full();
    test_loop_errors();
    test_closed();
    test_bad_descs();
    test_format();
    test_catch_with_tracks();
    test_v02();

    if (g_failures != 0) {
        fprintf(stderr, "trials_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("trials_test: all checks passed (YTR_MAX_TRIALS=%d, "
           "sizeof(ytr_trials)=%lu)\n", YTR_MAX_TRIALS,
           (unsigned long)sizeof(ytr_trials));
    return 0;
}
