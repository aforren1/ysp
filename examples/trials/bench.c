/* trials_bench.c - timings for ysp/table.h and ysp/trials.h v0.2: CSV parse
 * rate, the view of a block, open() of 10,000-trial designs, and the worst
 * next() + update() of a run. Not run by CI; the numbers go into
 * docs/table.md and docs/trials.md.
 *
 *     trials_bench [rounds]       (default 21; min, median and max)
 *
 * The inputs are made here from a fixed seed, so a run reproduces:
 *   strings  10,000 rows x 8 columns of words (3 to 12 letters)
 *   numbers  10,000 rows x 8 columns of %.17g doubles, as pandas writes
 *   quotes   10,000 rows x 8 columns, 30% quoted with commas and line breaks
 * Built with YTR_MAX_TRIALS and YTR_MAX_CONDITIONS at 16384, so one
 * handle holds a 10,000-trial list. Uses ysp/rt.h's clock; the measured
 * code itself reads no clock.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YRT_NO_THREADS
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"

#define YTR_MAX_TRIALS 16384
#define YTR_MAX_CONDITIONS 16384
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROWS 10000
#define COLS 8
#define MAX_ROUNDS 101

static int g_rounds = 21;
static char* g_text;
static size_t g_len;
static uint64_t* g_arena;
static size_t g_arena_size = (size_t)128 << 20;
static uint64_t* g_arena2;
static ytr_trials g_t;
static uint64_t g_seed;

static double now_ms(void) { return (double)yrt_now_ns() * 1e-6; }

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

typedef struct stat3 { double min, med, max; } stat3;

static stat3 stats(double* v, int n) {
    stat3 s;
    qsort(v, (size_t)n, sizeof(double), cmp_d);
    s.min = v[0];
    s.med = v[n / 2];
    s.max = v[n - 1];
    return s;
}

static uint64_t g_rs = 0x5EEDULL;
static uint64_t rnd64(void) {
    uint64_t z = (g_rs += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static size_t g_cap;
static void put(const char* fmt, const char* s) {
    int n = snprintf(g_text + g_len, g_cap - g_len, fmt, s);
    if (n > 0) g_len += (size_t)n;
}

static void make_input(int kind) {
    int r, c, k, n;
    char w[64];
    g_len = 0;
    g_rs = 0x5EEDULL + (uint64_t)kind;
    for (c = 0; c < COLS; c++) {
        snprintf(w, sizeof(w), "%scol%d", c ? "," : "", c);
        put("%s", w);
    }
    put("%s", "\n");
    for (r = 0; r < ROWS; r++) {
        for (c = 0; c < COLS; c++) {
            if (c) put("%s", ",");
            if (kind == 0) {
                n = 3 + (int)(rnd64() % 10);
                for (k = 0; k < n; k++) w[k] = (char)('a' + rnd64() % 26);
                w[n] = '\0';
                put("%s", w);
            } else if (kind == 1) {
                double v = (double)(rnd64() >> 11) * (1.0 / 9007199254740992.0) * 1000.0;
                snprintf(w, sizeof(w), "%.17g", v);
                put("%s", w);
            } else {
                n = 3 + (int)(rnd64() % 10);
                for (k = 0; k < n; k++) w[k] = (char)('a' + rnd64() % 26);
                w[n] = '\0';
                if (rnd64() % 10 < 3) {
                    put("\"%s, ", w);
                    put("%s\n\"\"x\"\"\"", w);
                } else {
                    put("%s", w);
                }
            }
        }
        put("%s", "\n");
    }
}

static bool parse(ytb_table* tb) {
    ytb_csv_desc d;
    memset(&d, 0, sizeof(d));
    d.text = g_text;
    d.len = g_len;
    d.arena = g_arena;
    d.arena_size = g_arena_size;
    return ytb_open_csv(tb, &d);
}

static void bench_parse(void) {
    static const char* const names[] = { "strings", "numbers", "quotes" };
    double v[MAX_ROUNDS];
    int kind, i;
    ytb_table tb, tv;
    stat3 s;
    memset(&tb, 0, sizeof(tb));
    printf("CSV parse, %d rows x %d columns, %d rounds:\n", ROWS, COLS, g_rounds);
    printf("  input     bytes    min ms   median ms   max ms   MB/s at median   block bytes   arena need\n");
    for (kind = 0; kind < 3; kind++) {
        make_input(kind);
        for (i = 0; i < g_rounds; i++) {
            double t0 = now_ms();
            bool ok = parse(&tb);
            v[i] = now_ms() - t0;
            if (!ok) { printf("parse failed: %s\n", ytb_error(&tb)); return; }
        }
        s = stats(v, g_rounds);
        printf("  %-8s %7lu  %8.2f  %10.2f  %7.2f  %15.1f  %12lu  %11lu\n", names[kind], (unsigned long)g_len,
               s.min, s.med, s.max, (double)g_len / (s.med * 1e3), (unsigned long)tb.size,
               (unsigned long)tb.need);
        /* The view of the same block, from a copy. */
        memcpy(g_arena2, tb.base, tb.size);
        for (i = 0; i < g_rounds; i++) {
            double t0 = now_ms();
            bool ok = ytb_open_view(&tv, g_arena2, tb.size);
            v[i] = now_ms() - t0;
            if (!ok) { printf("view failed: %s\n", ytb_error(&tv)); return; }
        }
        s = stats(v, g_rounds);
        printf("  %-8s view: min %.3f ms, median %.3f ms, max %.3f ms\n", names[kind], s.min, s.med, s.max);
    }
}

/* A 10,000-row table: cond (4 levels), kind (prime / target / fill, so
 * primes and targets are 2,500 each), lvl (8 levels), item (unique). */
static bool make_design_table(ytb_table* tb) {
    int r;
    char w[96];
    static const char* const kinds[4] = { "prime", "target", "fill", "fill" };
    g_len = 0;
    put("%s", "cond,kind,lvl,item\n");
    for (r = 0; r < ROWS; r++) {
        snprintf(w, sizeof(w), "c%d,%s,L%d,i%d\n", r % 4, kinds[(r / 4) % 4], r % 8, r);
        put("%s", w);
    }
    return parse(tb);
}

typedef struct design { const char* name; void (*fill)(ytr_desc* d); } design;

static ytb_table g_dt;
static ytb_table g_bt;

static void d_seq(ytr_desc* d) { d->table = &g_dt; d->reps = 1; }
static void d_full(ytr_desc* d) { d->table = &g_dt; d->reps = 1; d->order = YTR_ORDER_FULL_RANDOM; }
static void d_con(ytr_desc* d) {
    d->table = &g_dt; d->reps = 1; d->order = YTR_ORDER_CONSTRAINED;
    d->constraints[0] = ytr_max_run(0, YTR_ANY_LEVEL, 3);
    d->n_constraints = 1;
}
static void d_units(ytr_desc* d) {
    d->table = &g_dt; d->reps = 1; d->order = YTR_ORDER_CONSTRAINED;
    d->constraints[0] = ytr_followed_by(1, 0, 1);
    d->constraints[1] = ytr_max_run(0, YTR_ANY_LEVEL, 3);
    d->n_constraints = 2;
}
static void d_wr(ytr_desc* d) {
    d->table = &g_dt; d->order = YTR_ORDER_WITH_REPLACEMENT; d->draws = ROWS;
}
static void d_subset(ytr_desc* d) {
    d->table = &g_dt; d->reps = 1; d->subset = 5000; d->order = YTR_ORDER_FULL_RANDOM;
}
static void d_bal(ytr_desc* d) {
    /* 8 levels, lambda 156: 9,984 trials plus the lead-in. */
    d->table = &g_bt; d->reps = 8 * 156; d->order = YTR_ORDER_CONSTRAINED;
    d->constraints[0] = ytr_balance(0);
    d->n_constraints = 1;
}
static void d_groups(ytr_desc* d) {
    d->table = &g_dt; d->reps = 1; d->order = YTR_ORDER_CONSTRAINED;
    d->groups.mode = YTR_GROUPS_BLOCKED;
    d->groups.factor = 2;
    d->groups.order = YTR_GROUP_ORDER_BALANCED_LATIN;
    d->groups.participant = 3;
    d->constraints[0] = ytr_max_run(1, YTR_ANY_LEVEL, 3);
    d->n_constraints = 1;
}

static void bench_open(void) {
    static const design ds[] = {
        { "list in file order (SEQUENTIAL)", d_seq },
        { "FULL_RANDOM", d_full },
        { "CONSTRAINED, max_run 3 on 4 levels", d_con },
        { "units (followed_by) + max_run 3", d_units },
        { "WITH_REPLACEMENT, 10,000 draws of 10,000 rows", d_wr },
        { "subset 5,000 of 10,000, FULL_RANDOM", d_subset },
        { "balance, 8 levels, 9,985 trials", d_bal },
        { "BLOCKED groups (8, Williams) + max_run 3", d_groups },
    };
    double v[MAX_ROUNDS];
    int k, i;
    stat3 s;
    ytr_desc d;
    if (!make_design_table(&g_dt)) { printf("table: %s\n", ytb_error(&g_dt)); return; }
    {
        /* A second arena region for the 8-row balance table. */
        ytb_csv_desc cd;
        static const char csv8[] = "lvl\nA\nB\nC\nD\nE\nF\nG\nH\n";
        memset(&cd, 0, sizeof(cd));
        cd.text = csv8;
        cd.len = sizeof(csv8) - 1;
        cd.arena = g_arena2;
        cd.arena_size = 1 << 16;
        if (!ytb_open_csv(&g_bt, &cd)) { printf("table: %s\n", ytb_error(&g_bt)); return; }
    }
    printf("open(), %d rounds (sizeof(ytr_trials) = %lu at 16384 trials):\n", g_rounds,
           (unsigned long)sizeof(ytr_trials));
    printf("  design                                          trials   min ms  median ms   max ms  repair steps\n");
    for (k = 0; k < (int)(sizeof(ds) / sizeof(ds[0])); k++) {
        int swaps = 0;
        for (i = 0; i < g_rounds; i++) {
            double t0;
            bool ok;
            memset(&d, 0, sizeof(d));
            g_seed = 1000u + (uint64_t)i;
            d.rng = ytr_splitmix;
            d.rng_ctx = &g_seed;
            ds[k].fill(&d);
            t0 = now_ms();
            ok = ytr_open(&g_t, &d);
            v[i] = now_ms() - t0;
            if (!ok) { printf("  %s: %s\n", ds[k].name, ytr_error(&g_t)); break; }
            if (g_t.swaps > swaps) swaps = g_t.swaps;
        }
        if (i < g_rounds) continue;
        s = stats(v, g_rounds);
        printf("  %-46s %7d  %7.2f  %9.2f  %7.2f  %8d\n", ds[k].name, ytr_n_scheduled(&g_t), s.min, s.med,
               s.max, swaps);
    }
}

static const double g_soa[4] = { 0.1, 0.2, 0.3, 0.4 };

/* Four jitters, one of each kind, two snapped to 60000/1001 Hz frames. */
static void add_jitters(ytr_desc* d) {
    d->jitters[0] = ytr_frames(ytr_uniform("iti", 0.8, 1.2), 60000, 1001);
    d->jitters[1] = ytr_frames(ytr_exponential("fp", 0.5, 2.0, 0.4), 60000, 1001);
    d->jitters[2] = ytr_exponential("fp2", 0.5, 2.0, 0.4);
    d->jitters[3] = ytr_choice("soa", g_soa, 4);
    d->n_jitters = 4;
}

/* The worst next() + update() over whole runs: with units and re-queues
 * (forward moves of whole units after re-queues), and plain; with four
 * jitters when `jit`. */
static void bench_loop(int jit) {
    ytr_desc d;
    ytr_trial_info ti;
    int r, n;
    double worst = 0, total = 0, t0, dt;
    long calls = 0;
    uint64_t ts = 9;
    for (r = 0; r < 5; r++) {
        memset(&d, 0, sizeof(d));
        g_seed = 77u + (uint64_t)r;
        d.rng = ytr_splitmix;
        d.rng_ctx = &g_seed;
        d_units(&d);
        d.block_size = 100;
        if (jit) add_jitters(&d);
        if (!ytr_open(&g_t, &d)) { printf("loop open: %s\n", ytr_error(&g_t)); return; }
        n = 0;
        for (;;) {
            t0 = now_ms();
            if (ytr_next(&g_t, &ti) < 0) break;
            if (ytr_splitmix(&ts) < 0.05) ytr_requeue(&g_t);
            else ytr_update(&g_t, 1, NULL);
            dt = now_ms() - t0;
            total += dt;
            calls++;
            if (dt > worst) worst = dt;
            n++;
            if (n > 16000) break;
        }
    }
    printf("next() + update() over 5 runs of the units design with 5%% re-queues%s: %ld calls, "
           "mean %.2f us, worst %.1f us\n", jit ? " and 4 jitters" : "", calls,
           total / (double)calls * 1e3, worst * 1e3);
}

/* One ytr_jitter_draw() of each kind: the median over the rounds of the
 * mean over 100000 draws. */
static void bench_draw(void) {
    ytr_desc d;
    double v[MAX_ROUNDS], t0, sink = 0;
    int k, r, i;
    memset(&d, 0, sizeof(d));
    add_jitters(&d);
    printf("ytr_jitter_draw(), %d rounds of 100000 (splitmix included):\n", g_rounds);
    for (k = 0; k < 4; k++) {
        stat3 st;
        for (r = 0; r < g_rounds; r++) {
            t0 = now_ms();
            for (i = 0; i < 100000; i++) sink += ytr_jitter_draw(&d.jitters[k], ytr_splitmix, &g_seed).s;
            v[r] = (now_ms() - t0) * 1e6 / 100000.0;
        }
        st = stats(v, g_rounds);
        printf("  %-4s %s: min %.1f ns, median %.1f ns, max %.1f ns\n", d.jitters[k].name,
               k == 0 ? "uniform, frames    " : k == 1 ? "exponential, frames" : k == 2 ? "exponential        "
                                                                                       : "choice of 4        ",
               st.min, st.med, st.max);
    }
    if (sink == 42.0) printf("\n");
}

int main(int argc, char** argv) {
    if (argc > 1) g_rounds = atoi(argv[1]);
    if (g_rounds < 1 || g_rounds > MAX_ROUNDS) g_rounds = 21;
    g_cap = (size_t)16 << 20;
    g_text = (char*)malloc(g_cap);
    g_arena = (uint64_t*)malloc(g_arena_size);
    g_arena2 = (uint64_t*)malloc((size_t)16 << 20);
    if (!g_text || !g_arena || !g_arena2) return 1;
    printf("ysp_table %s, ysp_trials %s\n", ytb_version(), ytr_version());
    bench_parse();
    bench_open();
    bench_loop(0);
    bench_loop(1);
    bench_draw();
    return 0;
}
