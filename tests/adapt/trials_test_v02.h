/* trials_test_v02.h - the v0.2 part of trials_test.c (included by
 * it, after its harness): the v0.1 pins, tables, ORDER_LIST,
 * WITH_REPLACEMENT, subsets, groups, units, balance, Latin squares, the
 * rules text, and the v0.2 snapshot. The statistical checks use fixed
 * seeds, so they are deterministic; their bounds are the 99.9% points
 * (df + 3.1 sqrt(2 df), close to the chi-square quantile for the df used).
 */
/* The pins hold at the default YTR_MAX_TRIALS only. */
#if YTR_MAX_TRIALS == 4096
#include "trials_pins_run.h"
#include "trials_pins.h"
#include "trials_pins_v02_run.h"
#include "trials_pins_v02.h"
#endif

/* The hash of test_v02_digest()'s sessions (see there); v0.2.0 and later
 * give the same value, since the rules text's version line is left out. */
#define YTR_TEST_V02_DIGEST 0xded5b4d1602c8f90ULL

static double chi_crit(int df) { return df + 3.1 * sqrt(2.0 * df); }

/* 8-byte aligned arenas for tables and rules. */
static uint64_t g_tab_arena64[(1u << 20) / 8];
static uint64_t g_rule_arena64[(1u << 17) / 8];
static ytb_table g_tab;

static bool csv_table(ytb_table* tb, const char* csv) {
    ytb_csv_desc cd;
    memset(&cd, 0, sizeof(cd));
    cd.text = csv;
    cd.len = strlen(csv);
    cd.arena = g_tab_arena64;
    cd.arena_size = sizeof(g_tab_arena64);
    if (!ytb_open_csv(tb, &cd)) {
        fprintf(stderr, "trials_test: table: %s\n", ytb_error(tb));
        return false;
    }
    return true;
}

/* The main schedule's conditions (no practice) into seq; returns length. */
static int main_sched(const ytr_trials* t, int* seq) {
    int i, n = ytr_n_scheduled(t), np = t->desc.n_practice, k = 0;
    for (i = np; i < n; i++) seq[k++] = ytr_condition_at(t, i);
    return k;
}

static int g_seq[YTR_MAX_TRIALS + 8];
static int g_seq2[YTR_MAX_TRIALS + 8];

/* ------------------------------------------------------------------ pins */

static void test_pins(void) {
#if YTR_MAX_TRIALS == 4096
    int k, s, bad = 0;
    for (k = 0; k < PIN_N_DESIGNS; k++)
        for (s = 0; s < PIN_N_SEEDS; s++)
            if (pin_run(k, s) != pin_want[k][s]) {
                if (bad++ < 5) fprintf(stderr, "trials_test: FAIL pin design %d seed %d\n", k, s);
            }
    CHECK_I(bad, 0);
    printf("  v0.1 pins: %d sessions (30 designs x 20 seeds) against %s, %d differ\n",
           PIN_N_DESIGNS * PIN_N_SEEDS, PIN_VERSION, bad);
    bad = 0;
    for (k = 0; k < PIN2_N_DESIGNS; k++)
        for (s = 0; s < PIN_N_SEEDS; s++)
            if (pin2_run(k, s) != pin2_want[k][s]) {
                if (bad++ < 5) fprintf(stderr, "trials_test: FAIL v0.2 pin design %d seed %d\n", k, s);
            }
    CHECK_I(bad, 0);
    printf("  v0.2 pins: %d sessions (%d designs x 20 seeds) against %s, %d differ\n",
           PIN2_N_DESIGNS * PIN_N_SEEDS, PIN2_N_DESIGNS, PIN2_VERSION, bad);
#endif
}

/* ---------------------------------------------------------------- tables */

static const char* const g_cond_csv =
    "target,contrast,word,pair\n"
    "a,0.25,cat,\n"
    "a,0.5,dog,\n"
    "b,0.25,\"say, hi\",\n"
    "b,0.5,ox,\n"
    "c,0.25,emu,\n"
    "c,0.5,yak,\n";

static void test_table_conditions(void) {
    ytr_desc d;
    ytr_trial_info ti = ti_zero;
    char buf[512];
    int i, n, c, lv[4];
    if (!csv_table(&g_tab, g_cond_csv)) { CHECK(false); return; }
    zero_desc(&d);
    d.table = &g_tab;
    d.reps = 4;
    d.order = YTR_ORDER_CONSTRAINED;
    d.constraints[0] = ytr_max_run(0, YTR_ANY_LEVEL, 1);
    d.n_constraints = 1;
    d.rng = ytr_splitmix;
    g_rng_state = 11;
    d.rng_ctx = &g_rng_state;
    CHECK(ytr_open(&g_t, &d));
    CHECK_I(ytr_n_conditions(&g_t), 6);
    CHECK_I(ytr_n_factors(&g_t), 4);
    for (c = 0; c < 6; c++)
        for (i = 0; i < 4; i++) CHECK_I(ytr_level(&g_t, c, i), ytb_level(&g_tab, c, i));
    CHECK_I(ytr_level(&g_t, 0, 4), YTR_ERR_ARG);
    lv[0] = 1; lv[1] = 1; lv[2] = 3; lv[3] = 0;
    CHECK_I(ytr_condition_from_levels(&g_t, lv), 3);
    lv[2] = 0;
    CHECK_I(ytr_condition_from_levels(&g_t, lv), YTR_ERR_ARG);
    n = main_sched(&g_t, g_seq);
    CHECK_I(n, 24);
    for (i = 1; i < n; i++) CHECK(ytb_level(&g_tab, g_seq[i], 0) != ytb_level(&g_tab, g_seq[i - 1], 0));
    CHECK(ytr_format_header(&g_t, buf, sizeof(buf)) > 0);
    CHECK_S(buf, "index,block,rep,condition,track,practice,warmup,requeued,after_break,outcome,"
                 "target,contrast,word,pair\n");
    while (ytr_next(&g_t, &ti) >= 0) {
        ytr_update(&g_t, 1, NULL);
        if (ti.condition == 2) break;
    }
    CHECK(ytr_format_row(&g_t, ti.index, buf, sizeof(buf)) > 0);
    CHECK_HAS(buf, ",1,b,0.25,\"say, hi\",\n");
    CHECK(ytr_format_meta(&g_t, buf, sizeof(buf)) > 0);
    snprintf(buf + 400, 100, "table=%016llx rows=6 cols=4", (unsigned long long)ytb_hash(&g_tab));
    CHECK_HAS(buf, buf + 400);
    /* A table needs rows, may not have factors beside it. */
    d.factors[0] = ytr_factor("x", 2);
    d.n_factors = 1;
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "desc.table and desc.factors");
    d.n_factors = 0;
    d.n_conditions = 5;
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "row count");
    /* A column with more than 256 levels: the level is 16-bit. */
    {
        static char big[8192];
        int k = snprintf(big, sizeof(big), "id,half\n"), r2;
        for (r2 = 0; r2 < 300; r2++) k += snprintf(big + k, sizeof(big) - (size_t)k, "i%d,h%d\n", r2, r2 % 2);
        static int one[300];
        if (!csv_table(&g_tab, big)) { CHECK(false); return; }
        one[299] = 1;    /* one trial, so the check fits YTR_MAX_TRIALS 256 */
        zero_desc(&d);
        d.table = &g_tab;
        d.cond_reps = one;
        CHECK(ytr_open(&g_t, &d));
        CHECK_I(ytr_level(&g_t, 299, 0), 299);
        CHECK_I(ytr_level(&g_t, 299, 1), 1);
    }
    printf("  table conditions: 6 rows x 4 columns, levels, names and texts in the data line\n");
}

/* ----------------------------------------------------------- ORDER_LIST */

static void test_list(void) {
    ytr_desc d;
    ytr_trial_info ti = ti_zero;
    static const int lst[] = { 3, 0, 2, 2, 5, 1, 4 };
    int i, n, k;
    zero_desc(&d);
    d.n_conditions = 6;
    d.order = YTR_ORDER_LIST;
    d.order_list = lst;
    d.n_order_list = 7;
    CHECK(ytr_open(&g_t, &d));     /* no rng needed */
    n = main_sched(&g_t, g_seq);
    CHECK_I(n, 7);
    for (i = 0; i < n; i++) CHECK_I(g_seq[i], lst[i]);
    k = 0;
    while (ytr_next(&g_t, &ti) >= 0) {
        CHECK_I(ti.condition, lst[k]);
        CHECK_I(ti.rep, k == 3 ? 1 : 0);
        ytr_update(&g_t, 1, NULL);
        k++;
    }
    CHECK_I(k, 7);
    d.reps = 2;
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "takes no reps");
    d.reps = 0;
    d.n_order_list = 0;
    CHECK(!ytr_open(&g_t, &d));
    d.n_order_list = 7;
    d.n_practice = 2;
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "rng is required for practice and warmup draws under YTR_ORDER_LIST");
    d.n_practice = 0;
    {
        static const int bad[] = { 0, 6 };
        d.order_list = bad;
        d.n_order_list = 2;
        CHECK(!ytr_open(&g_t, &d));
        CHECK_HAS(ytr_error(&g_t), "desc.order_list[1] is not a condition");
    }
    d.order = YTR_ORDER_FULL_RANDOM;
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "needs desc.order = YTR_ORDER_LIST");
    printf("  order list: played as given, reps counted per row, rejections\n");
}

/* ------------------------------------------------------ WITH_REPLACEMENT */

static double const_rng(void* ctx) { return *(const double*)ctx; }

static void test_with_replacement(void) {
    static const double wsets[3][4] = { { 1, 1, 1, 1 }, { 1, 2, 3, 4 }, { 0.5, 0, 2, 0.25 } };
    ytr_desc d;
    int w, i, k, opens, draws = 200, n;
    double chi, tot, worst = 0;
    long cnt[4], pair[3][3];
    for (w = 0; w < 3; w++) {
        memset(cnt, 0, sizeof(cnt));
        zero_desc(&d);
        d.n_conditions = 4;
        d.order = YTR_ORDER_WITH_REPLACEMENT;
        d.draws = draws;
        d.weights = wsets[w];
        d.rng = ytr_splitmix;
        g_rng_state = 1000u + (uint64_t)w;
        d.rng_ctx = &g_rng_state;
        for (opens = 0; opens < 500; opens++) {
            if (!ytr_open(&g_t, &d)) { CHECK(false); return; }
            n = main_sched(&g_t, g_seq);
            CHECK_I(n, draws);
            for (i = 0; i < n; i++) cnt[g_seq[i]]++;
        }
        tot = 0;
        for (i = 0; i < 4; i++) tot += wsets[w][i];
        chi = 0;
        k = 0;
        for (i = 0; i < 4; i++) {
            double e = 500.0 * draws * wsets[w][i] / tot;
            if (wsets[w][i] == 0) { CHECK_I(cnt[i], 0); continue; }
            chi += (cnt[i] - e) * (cnt[i] - e) / e;
            k++;
        }
        CHECK(chi < chi_crit(k - 1));
        if (chi / chi_crit(k - 1) > worst) worst = chi / chi_crit(k - 1);
    }
    /* Serial independence, equal weights over 3 rows: pairs uniform. */
    memset(pair, 0, sizeof(pair));
    zero_desc(&d);
    d.n_conditions = 3;
    d.order = YTR_ORDER_WITH_REPLACEMENT;
    d.draws = draws;
    d.rng = ytr_splitmix;
    g_rng_state = 77;
    d.rng_ctx = &g_rng_state;
    for (opens = 0; opens < 300; opens++) {
        if (!ytr_open(&g_t, &d)) { CHECK(false); return; }
        n = main_sched(&g_t, g_seq);
        for (i = 1; i < n; i++) pair[g_seq[i - 1]][g_seq[i]]++;
    }
    chi = 0;
    for (i = 0; i < 3; i++)
        for (k = 0; k < 3; k++) {
            double e = 300.0 * (draws - 1) / 9.0;
            chi += (pair[i][k] - e) * (pair[i][k] - e) / e;
        }
    CHECK(chi < chi_crit(8));
    /* A variate on a running sum belongs to the next row with weight: row
     * c owns [cum[c-1], cum[c]), so a zero-weight row is never drawn, even
     * by u = 0 (splitmix gives it with probability 2^-53). */
    {
        static const double w0101[4] = { 0, 1, 0, 1 };
        static const double us[2] = { 0.0, 0.5 };
        static const int want[2] = { 1, 3 };
        static double u;
        int j;
        zero_desc(&d);
        d.n_conditions = 4;
        d.order = YTR_ORDER_WITH_REPLACEMENT;
        d.draws = 5;
        d.weights = w0101;
        d.rng = const_rng;
        d.rng_ctx = &u;
        for (j = 0; j < 2; j++) {
            u = us[j];
            if (!ytr_open(&g_t, &d)) { CHECK(false); return; }
            n = main_sched(&g_t, g_seq);
            for (i = 0; i < n; i++) CHECK_I(g_seq[i], want[j]);
        }
    }
    /* Rejections. */
    d.reps = 3;
    CHECK(!ytr_open(&g_t, &d));
    d.reps = 0;
    d.draws = 0;
    CHECK(!ytr_open(&g_t, &d));
    d.draws = 5;
    d.constraints[0] = ytr_max_run(YTR_CONDITION, YTR_ANY_LEVEL, 1);
    d.n_constraints = 1;
    CHECK(!ytr_open(&g_t, &d));
    printf("  with replacement: counts against weights (worst chi2 at %.2f of its bound), pairs "
           "independent (chi2 %.1f, 8 df)\n", worst, chi);
}

/* ---------------------------------------------------------------- subset */

static void test_subset(void) {
    ytr_desc d;
    long incl[10], subs[10];
    int opens, i, n, a, b, k;
    double chi;
    memset(incl, 0, sizeof(incl));
    zero_desc(&d);
    d.n_conditions = 10;
    d.reps = 1;
    d.subset = 3;
    d.order = YTR_ORDER_FULL_RANDOM;
    d.rng = ytr_splitmix;
    g_rng_state = 5;
    d.rng_ctx = &g_rng_state;
    for (opens = 0; opens < 10000; opens++) {
        if (!ytr_open(&g_t, &d)) { CHECK(false); return; }
        n = main_sched(&g_t, g_seq);
        CHECK_I(n, 3);
        for (i = 0; i < n; i++) incl[g_seq[i]]++;
    }
    chi = 0;
    for (i = 0; i < 10; i++) chi += (incl[i] - 3000.0) * (incl[i] - 3000.0) / 3000.0;
    CHECK(chi < chi_crit(9));
    /* Every 2-subset of 5 rows equally often. */
    memset(subs, 0, sizeof(subs));
    d.n_conditions = 5;
    d.subset = 2;
    d.order = YTR_ORDER_SEQUENTIAL;
    for (opens = 0; opens < 10000; opens++) {
        if (!ytr_open(&g_t, &d)) { CHECK(false); return; }
        n = main_sched(&g_t, g_seq);
        CHECK_I(n, 2);
        a = g_seq[0];
        b = g_seq[1];
        CHECK(a < b);   /* SEQUENTIAL keeps row order */
        k = 0;
        for (i = 0; i < a; i++) k += 4 - i;
        k += b - a - 1;
        if (k >= 0 && k < 10) subs[k]++;
    }
    chi = 0;
    for (i = 0; i < 10; i++) chi += (subs[i] - 1000.0) * (subs[i] - 1000.0) / 1000.0;
    CHECK(chi < chi_crit(9));
    /* Unequal reps: the k rows' own counts. */
    {
        static const int cr[4] = { 1, 5, 0, 2 };
        d.n_conditions = 4;
        d.cond_reps = cr;
        d.subset = 3;
        CHECK(ytr_open(&g_t, &d));
        CHECK_I(main_sched(&g_t, g_seq), 8);
        d.subset = 4;
        CHECK(!ytr_open(&g_t, &d));
        CHECK_HAS(ytr_error(&g_t), "desc.subset (4) is more than the 3 rows with trials");
    }
    printf("  subset: inclusion uniform, 2-subsets of 5 uniform (chi2 %.1f, 9 df)\n", chi);
}

/* ---------------------------------------------------------------- Latin */

static void test_latin(void) {
    static int row[64], seen[64][64], pairs[64][64];
    int n, r, j, rows, bad = 0;
    for (n = 1; n <= 64; n++) {
        int bal;
        for (bal = 0; bal < 2; bal++) {
            memset(seen, 0, sizeof(seen));
            memset(pairs, 0, sizeof(pairs));
            rows = ytr_latin(n, 0, bal != 0, row);
            CHECK_I(rows, (bal && n % 2) ? 2 * n : n);
            for (r = 0; r < rows; r++) {
                int perm[64];
                memset(perm, 0, sizeof(perm));
                if (ytr_latin(n, r, bal != 0, row) != rows) bad++;
                for (j = 0; j < n; j++) {
                    if (row[j] < 0 || row[j] >= n || perm[row[j]]++) bad++;
                    else seen[j][row[j]]++;
                    if (j > 0) pairs[row[j - 1]][row[j]]++;
                }
            }
            for (j = 0; j < n; j++)
                for (r = 0; r < n; r++) {
                    if (seen[j][r] != rows / n) bad++;
                    if (bal && j != r && pairs[j][r] != (n % 2 ? 2 : 1)) bad++;
                }
            /* participant mod rows */
            ytr_latin(n, rows + 1, bal != 0, row);
            {
                int other[64];
                ytr_latin(n, 1 % rows, bal != 0, other);
                if (memcmp(row, other, sizeof(int) * (size_t)n) != 0) bad++;
            }
        }
    }
    CHECK_I(bad, 0);
    CHECK_I(ytr_latin(0, 0, false, row), YTR_ERR_ARG);
    CHECK_I(ytr_latin(3, -1, false, row), YTR_ERR_ARG);
    CHECK_I(ytr_latin(3, 0, false, NULL), YTR_ERR_ARG);
    ytr_latin(4, 0, true, row);
    CHECK(row[0] == 0 && row[1] == 1 && row[2] == 3 && row[3] == 2);
    printf("  Latin squares: n = 1..64, cyclic and Williams, every row, position and adjacent pair\n");
}

/* ---------------------------------------------------------------- groups */

static const char* const g_block_csv =
    "block,item\n"
    "A,1\nA,2\nA,3\n"
    "B,1\nB,2\nB,3\n"
    "C,1\nC,2\nC,3\n"
    "D,1\nD,2\nD,3\n";

static void test_groups(void) {
    ytr_desc d;
    ytr_trial_info ti = ti_zero;
    int p, i, n, g, want[4], opens, k;
    long perm[6];
    double chi;
    if (!csv_table(&g_tab, g_block_csv)) { CHECK(false); return; }
    zero_desc(&d);
    d.table = &g_tab;
    d.reps = 2;
    d.order = YTR_ORDER_FULL_RANDOM;
    d.groups.mode = YTR_GROUPS_BLOCKED;
    d.groups.factor = 0;
    d.groups.order = YTR_GROUP_ORDER_BALANCED_LATIN;
    d.n_warmup = 1;
    d.rng = ytr_splitmix;
    g_rng_state = 3;
    d.rng_ctx = &g_rng_state;
    for (p = 0; p < 8; p++) {
        d.groups.participant = p;
        if (!ytr_open(&g_t, &d)) { CHECK(false); fprintf(stderr, "%s\n", ytr_error(&g_t)); return; }
        ytr_latin(4, p, true, want);
        n = main_sched(&g_t, g_seq);
        CHECK_I(n, 24);
        for (i = 0; i < n; i++) CHECK_I(ytb_level(&g_tab, g_seq[i], 0), want[i / 6]);
        /* Run it: a block per group, a warmup at each later group. */
        k = 0;
        g = -1;
        while (ytr_next(&g_t, &ti) >= 0) {
            if (ti.warmup) {
                CHECK(ti.first_in_block);
                ytr_update(&g_t, 1, NULL);
                continue;
            }
            if (k % 6 == 0) {
                CHECK_I(ti.block, k / 6);
                CHECK(ti.first_in_block || k > 0);
            } else {
                CHECK(!ti.first_in_block);
            }
            CHECK_I(ti.block, k / 6);
            g = ti.block;
            ytr_update(&g_t, 1, NULL);
            k++;
        }
        CHECK_I(k, 24);
        CHECK_I(g, 3);
    }
    /* RANDOM group order: every one of 3! orders equally often. */
    memset(perm, 0, sizeof(perm));
    {
        const char* csv3 = "g,x\na,1\nb,1\nc,1\n";
        if (!csv_table(&g_tab, csv3)) { CHECK(false); return; }
    }
    d.reps = 1;
    d.n_warmup = 0;
    d.groups.order = YTR_GROUP_ORDER_RANDOM;
    for (opens = 0; opens < 6000; opens++) {
        if (!ytr_open(&g_t, &d)) { CHECK(false); return; }
        main_sched(&g_t, g_seq);
        k = g_seq[0] * 2 + (g_seq[1] > g_seq[2] ? 1 : 0);
        if (g_seq[0] == 0) k = g_seq[1] == 1 ? 0 : 1;
        else if (g_seq[0] == 1) k = g_seq[1] == 0 ? 2 : 3;
        else k = g_seq[1] == 0 ? 4 : 5;
        perm[k]++;
    }
    chi = 0;
    for (i = 0; i < 6; i++) chi += (perm[i] - 1000.0) * (perm[i] - 1000.0) / 1000.0;
    CHECK(chi < chi_crit(5));
    /* LIST group order; a group with trials missing from the list fails. */
    {
        static const int gl[3] = { 2, 0, 1 };
        d.groups.order = YTR_GROUP_ORDER_LIST;
        d.groups.list = gl;
        d.groups.n_list = 3;
        CHECK(ytr_open(&g_t, &d));
        main_sched(&g_t, g_seq);
        CHECK(g_seq[0] == 2 && g_seq[1] == 0 && g_seq[2] == 1);
        d.groups.n_list = 2;
        CHECK(!ytr_open(&g_t, &d));
        CHECK_HAS(ytr_error(&g_t), "is not in groups.list");
        d.groups.n_list = 3;
    }
    /* ALTERNATE: one of each group in turn; unequal groups rejected. */
    if (!csv_table(&g_tab, g_block_csv)) { CHECK(false); return; }
    d.groups.mode = YTR_GROUPS_ALTERNATE;
    d.groups.order = YTR_GROUP_ORDER_RANDOM;
    d.groups.list = NULL;
    d.groups.n_list = 0;
    d.reps = 3;
    d.order = YTR_ORDER_CONSTRAINED;
    d.constraints[0] = ytr_max_run(1, YTR_ANY_LEVEL, 1);  /* item never twice in a row */
    d.constraints[1] = ytr_no_transition(1, 0, 1);
    d.n_constraints = 2;
    for (opens = 0; opens < 200; opens++) {
        if (!ytr_open(&g_t, &d)) { CHECK(false); fprintf(stderr, "%s\n", ytr_error(&g_t)); return; }
        n = main_sched(&g_t, g_seq);
        CHECK_I(n, 36);
        for (i = 4; i < n; i++)
            CHECK_I(ytb_level(&g_tab, g_seq[i], 0), ytb_level(&g_tab, g_seq[i - 4], 0));
        for (i = 1; i < n; i++) {
            int a = ytb_level(&g_tab, g_seq[i - 1], 1), b = ytb_level(&g_tab, g_seq[i], 1);
            CHECK(a != b);
            CHECK(!(a == 0 && b == 1));
        }
    }
    {
        const char* uneq = "block,item\nA,1\nA,2\nB,1\n";
        if (!csv_table(&g_tab, uneq)) { CHECK(false); return; }
        d.n_constraints = 0;
        d.order = YTR_ORDER_FULL_RANDOM;
        d.reps = 1;
        CHECK(!ytr_open(&g_t, &d));
        CHECK_HAS(ytr_error(&g_t), "ALTERNATE needs groups of one size");
    }
    /* Groups refuse tracks and block_size (BLOCKED). */
    d.groups.mode = YTR_GROUPS_BLOCKED;
    d.block_size = 4;
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "block_size must be 0");
    printf("  groups: BLOCKED in Williams order for 8 participants, RANDOM group order uniform "
           "(chi2 %.1f, 5 df), LIST, ALTERNATE cycles exactly under a repair\n", chi);
}

/* ----------------------------------------------------------------- units */

/* Independent check of a realized run (the history): every main trial of
 * level a of column f is directly followed by one of level b. */
static int follow_breaks(const ytr_trials* t, const ytb_table* tb, int f, int a, int b) {
    int n = 0, i, bad = 0;
    const ytr_trial* h = ytr_history(t, &n);
    for (i = 0; i < n; i++) {
        if (h[i].condition < 0 || (h[i].flags & (YTR_FLAG_PRACTICE | YTR_FLAG_WARMUP))) continue;
        if (ytb_level(tb, h[i].condition, f) != a) continue;
        if (i + 1 >= n || h[i + 1].condition < 0 || ytb_level(tb, h[i + 1].condition, f) != b) bad++;
    }
    return bad;
}

static void test_units(void) {
    ytr_desc d;
    ytr_trial_info ti = ti_zero;
    fake_track tr;
    int s, i, n, bad = 0, runs = 0, worst = 0;
    const char* csv =
        "kind,item\n"
        "prime,1\nprime,2\ntarget,1\ntarget,2\nfill,1\nfill,2\nfill,3\nfill,4\n";
    if (!csv_table(&g_tab, csv)) { CHECK(false); return; }
    zero_desc(&d);
    d.table = &g_tab;
    d.reps = 5;
    d.order = YTR_ORDER_CONSTRAINED;
    d.constraints[0] = ytr_followed_by(0, 0, 1);
    d.constraints[1] = ytr_max_run(0, 2, 2);         /* no 3 fillers in a row */
    d.n_constraints = 2;
    d.block_size = 7;
    d.rng = ytr_splitmix;
    d.rng_ctx = &g_rng_state;
    for (s = 0; s < 200; s++) {
        g_rng_state = 500u + (uint64_t)s;
        if (!ytr_open(&g_t, &d)) { bad++; fprintf(stderr, "%s\n", ytr_error(&g_t)); continue; }
        n = main_sched(&g_t, g_seq);
        CHECK_I(n, 40);
        for (i = 0; i < n; i++)
            if (ytb_level(&g_tab, g_seq[i], 0) == 0 &&
                (i + 1 >= n || ytb_level(&g_tab, g_seq[i + 1], 0) != 1)) bad++;
        for (i = 2; i < n; i++)
            if (ytb_level(&g_tab, g_seq[i], 0) == 2 && ytb_level(&g_tab, g_seq[i - 1], 0) == 2 &&
                ytb_level(&g_tab, g_seq[i - 2], 0) == 2) bad++;
        /* Run it with re-queues; a block never starts inside a unit. */
        g_test_state = (uint64_t)s;
        while (ytr_next(&g_t, &ti) >= 0) {
            int prev_kind = ti.index > 0 ? ytb_level(&g_tab, hist(&g_t, ti.index - 1).condition, 0) : -1;
            if (ti.first_in_block && prev_kind == 0) bad++;
            if (test_u() < 0.1 && ytr_requeue(&g_t) == 0) continue;
            ytr_update(&g_t, 1, NULL);
        }
        if (follow_breaks(&g_t, &g_tab, 0, 0, 1)) bad++;
        if (g_t.swaps > worst) worst = g_t.swaps;
        runs++;
    }
    CHECK_I(bad, 0);
    /* With a track: a track trial never splits a unit. */
    d.n_constraints = 1;
    d.block_size = 0;
    d.track_rate = 0.5;
    d.n_tracks = 1;
    for (s = 0; s < 50; s++) {
        g_rng_state = 900u + (uint64_t)s;
        fake_init(&tr, &g_t, 0, 30);
        d.tracks[0] = ytr_track(&tr, fake_done);
        if (!ytr_open(&g_t, &d)) { CHECK(false); return; }
        while (ytr_next(&g_t, &ti) >= 0) ytr_update(&g_t, 1, NULL);
        if (follow_breaks(&g_t, &g_tab, 0, 0, 1)) bad++;
    }
    CHECK_I(bad, 0);
    d.n_tracks = 0;
    d.track_rate = 0;
    /* Uniform over valid orders without other rules: 2 a, 2 b, 2 c, a -> b
     * has 6 level sequences. */
    {
        const char* csv2 = "k\na\nb\nc\n";
        long cnt[729];
        int combos = 0, o;
        double chi = 0;
        if (!csv_table(&g_tab, csv2)) { CHECK(false); return; }
        memset(cnt, 0, sizeof(cnt));
        d.reps = 2;
        d.constraints[0] = ytr_followed_by(0, 0, 1);
        d.n_constraints = 1;
        for (o = 0; o < 12000; o++) {
            int code = 0;
            g_rng_state = 7000u + (uint64_t)o;
            if (!ytr_open(&g_t, &d)) { CHECK(false); return; }
            n = main_sched(&g_t, g_seq);
            for (i = 0; i < n; i++) code = code * 3 + g_seq[i];
            if (code >= 0 && code < 729) cnt[code]++;
        }
        for (o = 0; o < 729; o++)
            if (cnt[o]) {
                combos++;
                chi += (cnt[o] - 2000.0) * (cnt[o] - 2000.0) / 2000.0;
            }
        CHECK_I(combos, 6);
        CHECK(chi < chi_crit(5));
        printf("  units: followed_by + max_run over 200 seeds with re-queues and blocks, %d runs, "
               "0 broken pairs (most repair moves %d); with a track, 0 split units; uniform over the "
               "6 valid orders (chi2 %.1f, 5 df)\n", runs, worst, chi);
    }
    /* preceded_by and chunk. */
    {
        const char* csv3 =
            "kind,pair\n"
            "cue,p1\ntarget,p1\ncue,p2\ntarget,p2\nfill,\nfill,\nfill,\n";
        if (!csv_table(&g_tab, csv3)) { CHECK(false); return; }
        d.reps = 3;
        d.constraints[0] = ytr_chunk(1);
        d.constraints[1] = ytr_max_run(0, YTR_ANY_LEVEL, 2);
        d.n_constraints = 2;
        for (s = 0; s < 100; s++) {
            g_rng_state = 300u + (uint64_t)s;
            if (!ytr_open(&g_t, &d)) { CHECK(false); fprintf(stderr, "%s\n", ytr_error(&g_t)); return; }
            n = main_sched(&g_t, g_seq);
            for (i = 0; i < n; i++)
                if (g_seq[i] == 0 || g_seq[i] == 2) CHECK(i + 1 < n && g_seq[i + 1] == g_seq[i] + 1);
        }
        d.constraints[0] = ytr_preceded_by(0, 1, 0);
        d.n_constraints = 1;
        CHECK(ytr_open(&g_t, &d));
        n = main_sched(&g_t, g_seq);
        for (i = 0; i < n; i++)
            if (ytb_level(&g_tab, g_seq[i], 0) == 1) CHECK(i > 0 && ytb_level(&g_tab, g_seq[i - 1], 0) == 0);
        /* Too few partners; two rules over one row; a broken chunk. */
        d.constraints[0] = ytr_followed_by(0, 2, 0);    /* 9 fill trials, 6 cues */
        CHECK(!ytr_open(&g_t, &d));
        CHECK_HAS(ytr_error(&g_t), "followed_by: 9 trials need a follower and only 6 can follow");
        d.constraints[0] = ytr_followed_by(0, 0, 1);
        d.constraints[1] = ytr_chunk(1);
        d.n_constraints = 2;
        CHECK(!ytr_open(&g_t, &d));
        CHECK_HAS(ytr_error(&g_t), "both take condition");
        d.constraints[0] = ytr_followed_by(0, 0, 0);
        d.n_constraints = 1;
        CHECK(!ytr_open(&g_t, &d));
        CHECK_HAS(ytr_error(&g_t), "two different levels");
    }
    {
        const char* csv4 = "kind,pair\nx,p1\ny,p2\nz,p1\n";
        if (!csv_table(&g_tab, csv4)) { CHECK(false); return; }
        d.constraints[0] = ytr_chunk(1);
        d.n_constraints = 1;
        CHECK(!ytr_open(&g_t, &d));
        CHECK_HAS(ytr_error(&g_t), "not contiguous");
    }
    {
        /* A chunk is one repetition of its rows: with cond_reps 3, 3, 1 the
         * run x, y ends repetition 1 and starts repetition 2 in sequential
         * order, and the two must still be separate units. Three units of
         * x, y and the filler f: f's slot is 0, 2, 4 or 6, each a quarter of
         * the time; one unit of four would give 1/3, 1/6, 1/6, 1/3. */
        static const int cr[3] = { 3, 3, 1 };
        long at[7];
        double chi = 0;
        memset(at, 0, sizeof(at));
        if (!csv_table(&g_tab, "kind,pair\nx,p1\ny,p1\nf,\n")) { CHECK(false); return; }
        zero_desc(&d);
        d.table = &g_tab;
        d.cond_reps = cr;
        d.order = YTR_ORDER_CONSTRAINED;
        d.constraints[0] = ytr_chunk(1);
        d.n_constraints = 1;
        d.rng = ytr_splitmix;
        d.rng_ctx = &g_rng_state;
        for (s = 0; s < 4000; s++) {
            g_rng_state = 1200u + (uint64_t)s;
            if (!ytr_open(&g_t, &d)) { CHECK(false); return; }
            n = main_sched(&g_t, g_seq);
            CHECK_I(n, 7);
            for (i = 0; i < n; i++) {
                if (g_seq[i] == 0) CHECK(i + 1 < n && g_seq[i + 1] == 1);
                if (g_seq[i] == 2) at[i]++;
            }
        }
        for (i = 0; i < 7; i += 2) chi += (at[i] - 1000.0) * (at[i] - 1000.0) / 1000.0;
        CHECK(chi < chi_crit(3));
    }
}

/* --------------------------------------------------------------- balance */

static int pair_counts_ok(const int* seq, int n, int f_levels, const ytb_table* tb, int f, int lam,
                          bool loops) {
    int cnt[16][16], i, a, b;
    memset(cnt, 0, sizeof(cnt));
    for (i = 1; i < n; i++) {
        a = ytb_level(tb, seq[i - 1], f);
        b = ytb_level(tb, seq[i], f);
        if (a < 0 || a >= 16 || b < 0 || b >= 16) return 0;
        cnt[a][b]++;
    }
    for (a = 0; a < f_levels; a++)
        for (b = 0; b < f_levels; b++)
            if (cnt[a][b] != ((a == b && !loops) ? 0 : lam)) return 0;
    return 1;
}

static void test_balance(void) {
    static const struct { int n, lam, loops, leadin; } des[] = {
        { 2, 1, 1, 1 }, { 3, 1, 1, 1 }, { 3, 2, 1, 1 }, { 4, 2, 1, 1 }, { 3, 1, 0, 1 }, { 4, 3, 0, 1 },
        { 5, 2, 1, 0 }, { 3, 2, 0, 0 }
    };
    static char csv[2048];
    ytr_desc d;
    ytr_trial_info ti = ti_zero;
    int k, s, i, n, bad = 0, nl, ok = 0;
    for (k = 0; k < (int)(sizeof(des) / sizeof(des[0])); k++) {
        int pos = snprintf(csv, sizeof(csv), "lvl,item\n"), per;
        nl = des[k].n;
        per = des[k].loops ? nl : nl - 1;
        for (i = 0; i < nl; i++) pos += snprintf(csv + pos, sizeof(csv) - (size_t)pos, "L%d,a\nL%d,b\n", i, i);
        if (!csv_table(&g_tab, csv)) { CHECK(false); return; }
        zero_desc(&d);
        d.table = &g_tab;
        /* each level: per x lam trials over its 2 rows */
        d.reps = per * des[k].lam;
        if (d.reps % 2) { d.reps *= 2; }
        d.order = YTR_ORDER_CONSTRAINED;
        d.constraints[0] = ytr_balance_flags(0, (des[k].loops ? 0 : YTR_BALANCE_NO_REPEAT) |
                                                   (des[k].leadin ? 0 : YTR_BALANCE_NO_LEADIN));
        d.n_constraints = 1;
        d.rng = ytr_splitmix;
        d.rng_ctx = &g_rng_state;
        {
            /* reps x 2 rows per level = per x lam' trials: lam' = 2 reps / per */
            int lam = 2 * d.reps / per;
            for (s = 0; s < 200; s++) {
                g_rng_state = 40000u + (uint64_t)(s * 31 + k);
                if (!ytr_open(&g_t, &d)) { bad++; fprintf(stderr, "%s\n", ytr_error(&g_t)); break; }
                n = main_sched(&g_t, g_seq);
                CHECK_I(n, nl * 2 * d.reps + (des[k].leadin ? 1 : 0));
                if (des[k].leadin) {
                    if (!pair_counts_ok(g_seq, n, nl, &g_tab, 0, lam, des[k].loops != 0)) bad++;
                } else {
                    /* One transition, last to first, is missing. */
                    g_seq[n] = g_seq[0];
                    if (!pair_counts_ok(g_seq, n + 1, nl, &g_tab, 0, lam, des[k].loops != 0)) bad++;
                }
                ok++;
            }
        }
    }
    CHECK_I(bad, 0);
    /* Uniform over all balanced sequences: n = 3 with self pairs has 216
     * (Brooks 2012), without 18; with the lead-in each sequence is the walk. */
    {
        static long cnt[19683];
        int loops, combos;
        double chi;
        for (loops = 1; loops >= 0; loops--) {
            int per = loops ? 3 : 2, draws = loops ? 43200 : 18000, want = loops ? 216 : 18;
            memset(cnt, 0, sizeof(cnt));
            if (!csv_table(&g_tab, "lvl\nA\nB\nC\n")) { CHECK(false); return; }
            zero_desc(&d);
            d.table = &g_tab;
            d.reps = per;
            d.order = YTR_ORDER_CONSTRAINED;
            d.constraints[0] = ytr_balance_flags(0, loops ? 0 : YTR_BALANCE_NO_REPEAT);
            d.n_constraints = 1;
            d.rng = ytr_splitmix;
            d.rng_ctx = &g_rng_state;
            for (s = 0; s < draws; s++) {
                int code = 0;
                g_rng_state = 90000u + (uint64_t)s;
                if (!ytr_open(&g_t, &d)) { CHECK(false); return; }
                n = main_sched(&g_t, g_seq);
                for (i = 0; i < n && i < 9; i++) code = code * 3 + g_seq[i];
                if (n == 3 * per + 1) {
                    /* 10 trials with loops: code the first 9; the last equals the first. */
                    if (g_seq[n - 1] != g_seq[0]) bad++;
                }
                if (code >= 0 && code < 19683) cnt[code]++;
            }
            combos = 0;
            chi = 0;
            for (i = 0; i < 19683; i++)
                if (cnt[i]) {
                    double e = (double)draws / want;
                    combos++;
                    chi += (cnt[i] - e) * (cnt[i] - e) / e;
                }
            CHECK_I(combos, want);
            CHECK(chi < chi_crit(want - 1));
            printf("  balance uniformity, 3 levels %s self pairs: %d sequences seen of %d, chi2 %.1f on %d df\n",
                   loops ? "with" : "without", combos, want, chi, want - 1);
        }
    }
    /* The lead-in: flagged, rep -1, not tallied, in the data line. */
    if (!csv_table(&g_tab, "lvl,side\nA,l\nA,r\nB,l\nB,r\n")) { CHECK(false); return; }
    zero_desc(&d);
    d.table = &g_tab;
    d.reps = 2;
    d.order = YTR_ORDER_CONSTRAINED;
    d.constraints[0] = ytr_balance(0);
    d.constraints[1] = ytr_max_run(1, YTR_ANY_LEVEL, 2);
    d.n_constraints = 2;
    d.rng = ytr_splitmix;
    g_rng_state = 17;
    d.rng_ctx = &g_rng_state;
    CHECK(ytr_open(&g_t, &d));
    n = main_sched(&g_t, g_seq);
    CHECK_I(n, 9);
    CHECK(pair_counts_ok(g_seq, n, 2, &g_tab, 0, 2, true));
    for (i = 2; i < n; i++)
        CHECK(!(ytb_level(&g_tab, g_seq[i], 1) == ytb_level(&g_tab, g_seq[i - 1], 1) &&
                ytb_level(&g_tab, g_seq[i], 1) == ytb_level(&g_tab, g_seq[i - 2], 1)));
    {
        char buf[256];
        int first = 1, tallied = 0, c;
        while (ytr_next(&g_t, &ti) >= 0) {
            bool leadin = (hist(&g_t, ti.index).flags & YTR_FLAG_LEADIN) != 0;
            if (first) {
                CHECK(leadin);
                CHECK_I(ti.rep, -1);
                CHECK(ytr_format_row(&g_t, ti.index, buf, sizeof(buf)) > 0);
                CHECK_HAS(buf, "0,0,-1,");
            } else {
                CHECK(!leadin);
            }
            first = 0;
            ytr_update(&g_t, 1, NULL);
        }
        for (c = 0; c < 4; c++) tallied += ytr_n_valid(&g_t, c);
        CHECK_I(tallied, 8);
        CHECK(ytr_format_header(&g_t, buf, sizeof(buf)) > 0);
        CHECK_HAS(buf, "after_break,leadin,outcome");
    }
    /* Rejections: unequal counts, a rule on the balanced factor, tracks. */
    d.constraints[1] = ytr_max_run(0, YTR_ANY_LEVEL, 2);
    CHECK(!ytr_open(&g_t, &d));
    CHECK_HAS(ytr_error(&g_t), "constrains the balanced factor");
    d.n_constraints = 1;
    {
        static const int cr[4] = { 2, 2, 1, 1 };
        d.cond_reps = cr;
        CHECK(!ytr_open(&g_t, &d));
        CHECK_HAS(ytr_error(&g_t), "every level needs the same number of trials");
        d.cond_reps = NULL;
    }
    d.reps = 3;     /* 6 trials per level: not a multiple of 4? it is a multiple of 2 levels */
    CHECK(ytr_open(&g_t, &d));
    d.reps = 1;     /* 2 trials per level, 2 levels: lambda 1 */
    CHECK(ytr_open(&g_t, &d));
    d.constraints[0] = ytr_balance_no_repeat(0);
    d.reps = 1;
    CHECK(ytr_open(&g_t, &d));
    printf("  balance: exact pair counts on %d orders of 8 designs (lead-in and none), lead-in "
           "flagged, untallied, in the data line\n", ok);
}

/* ------------------------------------------------------------ rules text */

static int apply_rules(ytr_desc* d, const char* text, int participant, char* err, size_t cap) {
    ytr_rules_desc rd;
    memset(&rd, 0, sizeof(rd));
    rd.text = text;
    rd.len = strlen(text);
    rd.table = &g_tab;
    rd.participant = participant;
    rd.arena = g_rule_arena64;
    rd.arena_size = sizeof(g_rule_arena64);
    return ytr_rules(d, &rd, err, cap);
}

static void same_schedule(int line, const ytr_desc* a, const ytr_desc* b, uint64_t seed) {
    int n1, n2;
    uint64_t s1 = seed, s2 = seed;
    ytr_desc x = *a, y = *b;
    x.rng = y.rng = ytr_splitmix;
    x.rng_ctx = &s1;
    y.rng_ctx = &s2;
    if (!ytr_open(&g_t, &x) || !ytr_open(&g_u, &y)) {
        fprintf(stderr, "trials_test: FAIL at line %d: open: %s / %s\n", line, ytr_error(&g_t),
                ytr_error(&g_u));
        g_failures++;
        return;
    }
    n1 = main_sched(&g_t, g_seq);
    n2 = main_sched(&g_u, g_seq2);
    if (n1 != n2 || memcmp(g_seq, g_seq2, sizeof(int) * (size_t)n1) != 0) {
        fprintf(stderr, "trials_test: FAIL at line %d: rules and C give different schedules\n", line);
        g_failures++;
    }
}

static void test_rules(void) {
    ytr_desc c, r;
    char err[256], fmt[4096];
    int line;
    const char* csv =
        "target,contrast,word,list,n\n"
        "a,0.25,cat,1,2\na,0.5,dog,1,1\nb,0.25,\"new york\",2,2\nb,0.5,ox,2,1\n"
        "catch,0,emu,1,1\ncatch,0,yak,2,1\n";
    if (!csv_table(&g_tab, csv)) { CHECK(false); return; }
    /* The same design in C and as rules gives the same schedule. */
    zero_desc(&c);
    c.table = &g_tab;
    c.reps = 6;
    c.order = YTR_ORDER_CONSTRAINED;
    c.constraints[0] = ytr_max_run(0, YTR_ANY_LEVEL, 2);
    c.constraints[1] = ytr_max_in_window(0, 2, 2, 1);
    c.constraints[2] = ytr_first_not(0, 2);
    c.constraints[3] = ytr_no_transition(1, 0, 1);
    c.constraints[4] = ytr_min_gap(2, 2, 3);
    c.n_constraints = 5;
    c.block_size = 12;
    zero_desc(&r);
    line = apply_rules(&r,
        "# a comment line\n"
        "order constrained\r\n"
        "reps 6   # trailing comment\n"
        "  max_run target 2\n"
        "max_in_window target=catch 2 1\n"
        "first_not target=catch\n"
        "no_transition contrast 0.25 0.50\n"
        "min_gap word=\"new york\" 3\n"
        "\n"
        "block_size 12\n", 0, err, sizeof(err));
    CHECK_I(line, 0);
    if (line) fprintf(stderr, "%s\n", err);
    same_schedule(__LINE__, &c, &r, 99);
    /* format_rules pastes back. */
    {
        ytr_desc r2;
        uint64_t s = 99;
        r.rng = ytr_splitmix;
        r.rng_ctx = &s;
        CHECK(ytr_open(&g_t, &r));
        CHECK(ytr_format_rules(&g_t, fmt, sizeof(fmt)) > 0);
        CHECK_HAS(fmt, "max_in_window target=catch 2 1\n");
        CHECK_HAS(fmt, "min_gap word=\"new york\" 3\n");
        zero_desc(&r2);
        CHECK_I(apply_rules(&r2, fmt, 0, err, sizeof(err)), 0);
        same_schedule(__LINE__, &r, &r2, 99);
    }
    /* OpenSesame's constrain: mindist counts rows. */
    zero_desc(&c);
    c.table = &g_tab;
    c.reps = 4;
    c.order = YTR_ORDER_FULL_RANDOM;
    c.constraints[0] = ytr_max_run(0, YTR_ANY_LEVEL, 1);
    c.constraints[1] = ytr_min_gap(0, YTR_ANY_LEVEL, 1);
    c.n_constraints = 2;
    c.order = YTR_ORDER_CONSTRAINED;
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "order constrained\nreps 4\nconstrain target maxrep=1 mindist=2\n", 0, err, sizeof(err)), 0);
    same_schedule(__LINE__, &c, &r, 5);
    /* where, @participant, weight, practice from, groups, subset, draws. */
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "where list=@participant\nweight n\norder random\npractice 3 from target=catch\n",
                        1, err, sizeof(err)), 0);
    CHECK(r.cond_reps != NULL);
    if (r.cond_reps) {
        CHECK_I(r.cond_reps[0], 0);
        CHECK_I(r.cond_reps[2], 2);
        CHECK_I(r.cond_reps[3], 1);
        CHECK_I(r.cond_reps[5], 1);
    }
    CHECK_I(r.n_warmup_conditions, 2);
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "order with_replacement\ndraws 50 weights=n\nsubset 4\n", 0, err, sizeof(err)), 0);
    CHECK_I(r.draws, 50);
    CHECK(r.weights && r.weights[0] == 2.0);
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "order full_random\nreps 2\ngroups list blocked balanced_latin\n", 3, err, sizeof(err)), 0);
    CHECK_I(r.groups.mode, YTR_GROUPS_BLOCKED);
    CHECK_I(r.groups.participant, 3);
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "order constrained\nreps 2\nfollowed_by target a b\nchunk word\nbalance contrast no_repeat no_leadin\n", 0, err, sizeof(err)), 0);
    CHECK_I(r.n_constraints, 3);
    CHECK_I(r.constraints[2].n, YTR_BALANCE_NO_REPEAT | YTR_BALANCE_NO_LEADIN);
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "order balanced_latin\n", 2, err, sizeof(err)), 0);
    CHECK_I(r.order, YTR_ORDER_LIST);
    CHECK_I(r.n_order_list, 6);
    zero_desc(&r);
    CHECK_I(apply_rules(&r, "list 0 1 2\nlist 5 5\norder list\n", 0, err, sizeof(err)), 0);
    CHECK_I(r.n_order_list, 5);
    /* Errors name the line and column, and suggest. */
    {
        static const struct { const char* text; int line; const char* msg; } bad[] = {
            { "order constrained\nmaxrep target 2\n", 2, "unknown statement 'maxrep'; the statement here is 'max_run'" },
            { "max_runn target 2\n", 1, "did you mean 'max_run'?" },
            { "reps 2\nmax_run tagret 2\n", 2, "col 9: unknown column 'tagret' (target, contrast, word, list, n, @row)" },
            { "max_run target=d 2\n", 1, "column 'target' has no level 'd' (levels: a, b, catch)" },
            { "max_run target\n", 1, "expected: max_run COLUMN[=VALUE] N" },
            { "order random\nreps 2\norder random\n", 3, "order is given twice (first on line 1)" },
            { "slice 0 4\n", 1, "the OpenSesame operation 'slice' is not supported" },
            { "max_run target 0\n", 1, "is not a count of at least 1" },
            { "first_not target\n", 1, "first_not needs COLUMN=VALUE" },
            { "min_gap word=\"new york 3\n", 1, "a quoted token is not closed" },
            { "constrain target mindist=1\n", 1, "mindist must be at least 2" },
            { "where target=a\norder random\n", 0, "where needs reps" },
            { "list 0 9\n", 1, "row 9 is not below 6" },
            { "groups target sideways\n", 1, "expected blocked or alternate" },
        };
        int i;
        for (i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++) {
            zero_desc(&r);
            line = apply_rules(&r, bad[i].text, 0, err, sizeof(err));
            CHECK(line != 0);
            if (bad[i].line) CHECK_I(line, bad[i].line);
            CHECK_HAS(err, bad[i].msg);
        }
    }
    /* The arena: too small is an error with the bytes needed. */
    {
        ytr_rules_desc rd;
        static uint64_t tiny[2];
        memset(&rd, 0, sizeof(rd));
        rd.text = "list 0 1\n";
        rd.len = strlen(rd.text);
        rd.table = &g_tab;
        rd.arena = tiny;
        rd.arena_size = sizeof(tiny);
        zero_desc(&r);
        CHECK(ytr_rules(&r, &rd, err, sizeof(err)) != 0);
        CHECK_HAS(err, "the rules arena has 16 bytes and needs");
    }
    printf("  rules: C and rules give one schedule, format_rules pastes back, OpenSesame constrain, "
           "where/@participant/weight/practice from, 14 error messages\n");
}

/* ------------------------------------------------------ snapshot, format 2 */

static void test_snapshot_v2(void) {
    static unsigned char snap[1 << 18];
    ytr_desc d;
    ytr_trial_info ti = ti_zero;
    int cut, n_full, i, len = 0, k, designs;
    static int full_cond[YTR_MAX_TRIALS], full_out[YTR_MAX_TRIALS];
    const char* csv = "kind,blk\nprime,X\ntarget,X\nfill,X\nprime,Y\ntarget,Y\nfill,Y\n";
    for (designs = 0; designs < 3; designs++) {
        if (!csv_table(&g_tab, csv)) { CHECK(false); return; }
        zero_desc(&d);
        d.table = &g_tab;
        d.reps = 3;
        d.order = YTR_ORDER_CONSTRAINED;
        d.rng = ytr_splitmix;
        d.rng_ctx = &g_rng_state;
        d.requeue_gap = 2;
        if (designs == 0) {
            d.constraints[0] = ytr_followed_by(0, 0, 1);
            d.n_constraints = 1;
            d.block_size = 5;
        } else if (designs == 1) {
            d.constraints[0] = ytr_followed_by(0, 0, 1);
            d.n_constraints = 1;
            d.groups.mode = YTR_GROUPS_BLOCKED;
            d.groups.factor = 1;
            d.groups.order = YTR_GROUP_ORDER_RANDOM;
            d.n_warmup = 1;
        } else {
            d.reps = 3;
            d.constraints[0] = ytr_balance(0);
            d.n_constraints = 1;
        }
        /* The uninterrupted run. */
        g_rng_state = 4242;
        if (!ytr_open(&g_t, &d)) { CHECK(false); fprintf(stderr, "%s\n", ytr_error(&g_t)); return; }
        g_test_state = 1;
        n_full = 0;
        while (ytr_next(&g_t, &ti) >= 0) {
            full_cond[n_full] = ti.condition;
            if (!(hist(&g_t, ti.index).flags & YTR_FLAG_LEADIN) && !ti.warmup && test_u() < 0.15 && ytr_requeue(&g_t) == 0) {
                full_out[n_full++] = YTR_REQUEUE;
                continue;
            }
            full_out[n_full++] = 1;
            ytr_update(&g_t, 1, NULL);
        }
        CHECK(ytr_save(&g_t, snap, sizeof(snap)) > 8);
        CHECK(snap[4] == 2);           /* format 2 */
        /* Cut, save, load into a garbage handle, finish, compare. */
        for (cut = 0; cut < n_full; cut += 3) {
            uint64_t state_at_cut;
            g_rng_state = 4242;
            ytr_open(&g_t, &d);
            for (i = 0; i < cut; i++) {
                ytr_next(&g_t, &ti);
                if (full_out[i] == YTR_REQUEUE) ytr_requeue(&g_t);
                else ytr_update(&g_t, 1, NULL);
            }
            state_at_cut = g_rng_state;
            len = ytr_save(&g_t, snap, sizeof(snap));
            CHECK(len > 0);
            memset(&g_u, 0xA5, sizeof(g_u));
            if (!ytr_open_saved(&g_u, &d, snap, (size_t)len)) {
                CHECK(false);
                fprintf(stderr, "%s\n", ytr_error(&g_u));
                return;
            }
            g_rng_state = state_at_cut;
            for (k = cut; k < n_full; k++) {
                if (ytr_next(&g_u, &ti) < 0 || ti.condition != full_cond[k]) { CHECK(false); break; }
                if (full_out[k] == YTR_REQUEUE) ytr_requeue(&g_u);
                else ytr_update(&g_u, 1, NULL);
            }
            CHECK(ytr_next(&g_u, &ti) == YTR_DONE);
        }
        /* A v0.1 desc over a v0.2 snapshot is refused. */
        {
            ytr_desc v1;
            zero_desc(&v1);
            v1.n_conditions = 6;
            v1.reps = 3;
            CHECK(!ytr_open_saved(&g_u, &v1, snap, (size_t)len));
        }
    }
    printf("  snapshot format 2: units, BLOCKED groups and balance, a load at every third cut, "
           "identical to the uninterrupted run\n");
}


/* ---------------------------------------------------------- v0.2 digest */

/* One hash over scripted sessions of every v0.2 feature: the schedule at
 * open, a run with re-queues, the history, the data lines, the rules text
 * and the snapshot. gcc and MSVC printed the same value when it was pinned,
 * so a change of any draw or byte shows here. It holds at the default
 * YTR_MAX_TRIALS only. */
#if YTR_MAX_TRIALS == 4096
static uint64_t dg_fnv(uint64_t h, const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

static uint64_t dg_int(uint64_t h, long v) {
    unsigned char b[8];
    int i;
    for (i = 0; i < 8; i++) b[i] = (unsigned char)((unsigned long long)(long long)v >> (8 * i));
    return dg_fnv(h, b, 8);
}

static uint64_t dg_session(uint64_t h, ytr_desc* d, uint64_t seed) {
    static unsigned char snap[1 << 18];
    static char line[4096];
    ytr_trial_info ti = ti_zero;
    const ytr_trial* hs;
    int i, n, len;
    uint64_t ts = seed ^ 0xABCDu;
    g_rng_state = seed;
    d->rng = ytr_splitmix;
    d->rng_ctx = &g_rng_state;
    if (!ytr_open(&g_t, d)) return dg_fnv(h, ytr_error(&g_t), strlen(ytr_error(&g_t)));
    n = ytr_n_scheduled(&g_t);
    for (i = 0; i < n; i++) h = dg_int(h, ytr_condition_at(&g_t, i));
    for (i = 0; i < 5000 && ytr_next(&g_t, &ti) >= 0; i++) {
        bool lead = (hist(&g_t, ti.index).flags & YTR_FLAG_LEADIN) != 0;
        if (!lead && !ti.warmup && !ti.practice && ti.track < 0 && ytr_splitmix(&ts) < 0.1)
            h = dg_int(h, ytr_requeue(&g_t));
        else
            h = dg_int(h, ytr_update(&g_t, (int)(ytr_splitmix(&ts) * 3.0), NULL));
    }
    hs = ytr_history(&g_t, &n);
    for (i = 0; i < n; i++) {
        h = dg_fnv(h, &hs[i].condition, 2);
        h = dg_int(h, hs[i].flags);
        h = dg_int(h, hs[i].rep);
        h = dg_int(h, hs[i].block);
        h = dg_int(h, hs[i].outcome);
        if (ytr_format_row(&g_t, i, line, sizeof(line)) > 0) h = dg_fnv(h, line, strlen(line));
    }
    if (ytr_format_header(&g_t, line, sizeof(line)) > 0) h = dg_fnv(h, line, strlen(line));
    if (ytr_format_rules(&g_t, line, sizeof(line)) > 0) {
        /* Past the first line, which names the version. */
        const char* body = strchr(line, '\n');
        body = body ? body + 1 : line;
        h = dg_fnv(h, body, strlen(body));
    }
    len = ytr_save(&g_t, snap, sizeof(snap));
    if (len > 0) h = dg_fnv(h, snap, (size_t)len);
    return dg_int(h, len);
}

#endif

static void test_v02_digest(void) {
#if YTR_MAX_TRIALS == 4096
    ytr_desc d;
    uint64_t h = 0xCBF29CE484222325ULL;
    int s;
    for (s = 0; s < 5; s++) {
        uint64_t seed = 31337u + (uint64_t)s * 101u;
        if (!csv_table(&g_tab, g_cond_csv)) { CHECK(false); return; }
        zero_desc(&d);
        d.table = &g_tab; d.reps = 4; d.order = YTR_ORDER_CONSTRAINED;
        d.constraints[0] = ytr_max_run(0, YTR_ANY_LEVEL, 1); d.n_constraints = 1; d.block_size = 6;
        d.n_practice = 2; d.n_warmup = 1; d.requeue_gap = 2;
        h = dg_session(h, &d, seed);
        zero_desc(&d);
        d.table = &g_tab; d.order = YTR_ORDER_WITH_REPLACEMENT; d.draws = 30; d.subset = 4;
        h = dg_session(h, &d, seed);
        {
            static const int lst[] = { 5, 4, 3, 2, 1, 0, 0, 1 };
            zero_desc(&d);
            d.table = &g_tab; d.order = YTR_ORDER_LIST; d.order_list = lst; d.n_order_list = 8;
            h = dg_session(h, &d, seed);
        }
        if (!csv_table(&g_tab, g_block_csv)) { CHECK(false); return; }
        zero_desc(&d);
        d.table = &g_tab; d.reps = 2; d.order = YTR_ORDER_CONSTRAINED;
        d.constraints[0] = ytr_max_run(1, YTR_ANY_LEVEL, 1); d.n_constraints = 1;
        d.groups.mode = YTR_GROUPS_BLOCKED; d.groups.factor = 0;
        d.groups.order = YTR_GROUP_ORDER_BALANCED_LATIN; d.groups.participant = s; d.n_warmup = 1;
        h = dg_session(h, &d, seed);
        d.groups.mode = YTR_GROUPS_ALTERNATE; d.groups.order = YTR_GROUP_ORDER_RANDOM; d.n_warmup = 0;
        h = dg_session(h, &d, seed);
        if (!csv_table(&g_tab, "kind,item\nprime,1\nprime,2\ntarget,1\ntarget,2\nfill,1\nfill,2\nfill,3\nfill,4\n")) {
            CHECK(false);
            return;
        }
        zero_desc(&d);
        d.table = &g_tab; d.reps = 3; d.order = YTR_ORDER_CONSTRAINED;
        d.constraints[0] = ytr_followed_by(0, 0, 1);
        d.constraints[1] = ytr_max_run(0, 2, 2); d.n_constraints = 2; d.block_size = 5; d.requeue_gap = 3;
        h = dg_session(h, &d, seed);
        if (!csv_table(&g_tab, "lvl,side\nA,l\nA,r\nB,l\nB,r\nC,l\nC,r\n")) { CHECK(false); return; }
        zero_desc(&d);
        d.table = &g_tab; d.reps = 3; d.order = YTR_ORDER_CONSTRAINED;
        d.constraints[0] = ytr_balance(0);
        d.constraints[1] = ytr_max_run(1, YTR_ANY_LEVEL, 2); d.n_constraints = 2;
        h = dg_session(h, &d, seed);
        d.constraints[0] = ytr_balance_flags(0, YTR_BALANCE_NO_REPEAT | YTR_BALANCE_NO_LEADIN);
        d.reps = 2;
        h = dg_session(h, &d, seed);
    }
    printf("  v0.2 digest: %016llx\n", (unsigned long long)h);
    CHECK(h == YTR_TEST_V02_DIGEST);
#endif
}

#include "trials_test_jitter.h"

static void test_v02(void) {
    test_pins();
    test_table_conditions();
    test_list();
    test_with_replacement();
    test_subset();
    test_latin();
    test_groups();
    test_units();
    test_balance();
    test_rules();
    test_snapshot_v2();
    test_v02_digest();
    test_jitter();
}
