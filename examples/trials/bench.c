/* trials_bench.c - timings for ysp/table.h and ysp/trials.h: CSV parse
 * rate, the view of a block, open() of 10,000-trial designs, the worst
 * next() + update() of a run, and the data writer's cost per row (v0.3).
 * Not run by CI; the numbers go into docs/table.md and docs/trials.md.
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

static const int64_t g_soa[4] = { YRT_MS(100), YRT_MS(200), YRT_MS(300), YRT_MS(400) };

/* Four jitters, one of each kind, two snapped to 60000/1001 Hz frames. */
static void add_jitters(ytr_desc* d) {
    d->jitters[0] = ytr_frames(ytr_uniform("iti", YRT_MS(800), YRT_MS(1200)), 60000, 1001);
    d->jitters[1] = ytr_frames(ytr_exponential("fp", YRT_MS(500), YRT_MS(2000), YRT_MS(400)), 60000, 1001);
    d->jitters[2] = ytr_exponential("fp2", YRT_MS(500), YRT_MS(2000), YRT_MS(400));
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
            for (i = 0; i < 100000; i++) sink += YRT_TO_S(ytr_jitter_draw(&d.jitters[k], ytr_splitmix, &g_seed).ns);
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

/* The data writer (v0.4): a row of 26 columns (the session's 18: ten
 * integers, 4 table texts, 4 jitters; then 8 of the caller's: an int, a
 * bool, two times, a shortest double, a fixed double, a string and a CONST)
 * written after each trial of a FULL_RANDOM session. Per row: formatting
 * only (a write function that copies to memory), to a file with
 * YTR_FLUSH_ROW, and to a file with YTR_FLUSH_SYNC (fewer rows: each one
 * waits for the disk). The file is in the working directory. */
typedef struct bench_row {
    int key;
    bool correct;
    int64_t rt_ns, onset_ns;
    double level, contrast;
    const char* response;
} bench_row;

static char g_mem[1 << 16];
static bool mem_write(void* ctx, const void* bytes, size_t n) {
    (void)ctx;
    if (n > sizeof(g_mem)) return false;
    memcpy(g_mem, bytes, n);
    return true;
}

static ytr_data g_w;
static double g_row_us[16384];

static void bench_data(void) {
    static const ytr_col cols[] = {
        YTR_COL(bench_row, key, YTR_COL_INT),
        YTR_COL(bench_row, correct, YTR_COL_BOOL),
        YTR_COL(bench_row, rt_ns, YTR_COL_TIME_NS),
        YTR_COL_D(bench_row, onset_ns, YTR_COL_TIME_NS, 6),
        YTR_COL(bench_row, level, YTR_COL_DOUBLE),
        YTR_COL_D(bench_row, contrast, YTR_COL_DOUBLE, 3),
        YTR_COL(bench_row, response, YTR_COL_STRING),
        YTR_COL_TEXT("participant", "P07"),
    };
    static const char* const modes[] = { "format only (memory)", "file, YTR_FLUSH_ROW",
                                         "file, YTR_FLUSH_SYNC" };
    static const char* const path = "trials_bench_data.csv";
    int mode;
    printf("ytr_data_write(), 26 columns, per row:\n");
    for (mode = 0; mode < 3; mode++) {
        ytr_desc d;
        ytr_data_desc dd;
        ytr_trial_info ti;
        bench_row r;
        int n = 0, limit = mode == 2 ? 1000 : 10000;
        uint64_t rs = 3;
        stat3 s;
        double p99, total = 0;
        memset(&d, 0, sizeof(d));
        g_seed = 5u;
        d.rng = ytr_splitmix;
        d.rng_ctx = &g_seed;
        d_full(&d);
        add_jitters(&d);
        if (!ytr_open(&g_t, &d)) { printf("  open: %s\n", ytr_error(&g_t)); return; }
        memset(&dd, 0, sizeof(dd));
        if (mode == 0) dd.write = mem_write;
        else dd.path = path;
        dd.flush = mode == 2 ? YTR_FLUSH_SYNC : YTR_FLUSH_ROW;
        dd.trials = &g_t;
        dd.cols = cols;
        dd.n_cols = (int)(sizeof(cols) / sizeof(cols[0]));
        if (!ytr_data_open(&g_w, &dd)) { printf("  %s\n", ytr_data_error(&g_w)); return; }
        while (n < limit && ytr_next(&g_t, &ti) >= 0) {
            double t0;
            ytr_update(&g_t, 1, NULL);
            r.key = 40 + (int)(rnd64() % 4);
            r.correct = (rnd64() & 1) != 0;
            r.rt_ns = 250000000 + (int64_t)(rnd64() % 500000000);
            r.onset_ns = (int64_t)(rnd64() % 4000000000000ULL);
            r.level = (double)(rnd64() >> 11) * (1.0 / 9007199254740992.0);
            r.contrast = 0.01 * (double)(1 + rnd64() % 64);
            r.response = (rs++ & 1) ? "left" : "right, late";
            t0 = now_ms();
            if (ytr_data_write(&g_w, &r) != 0) { printf("  write: %s\n", ytr_data_error(&g_w)); break; }
            g_row_us[n] = (now_ms() - t0) * 1e3;
            total += g_row_us[n];
            n++;
        }
        if (ytr_data_finish(&g_w) < 0) printf("  finish: %s\n", ytr_data_error(&g_w));
        ytr_data_close(&g_w);
        if (mode) remove(path);
        if (n < 2) return;
        s = stats(g_row_us, n);
        p99 = g_row_us[(n * 99) / 100];
        printf("  %-22s %5d rows: median %8.2f us, mean %8.2f us, p99 %8.2f us, max %8.2f us\n",
               modes[mode], n, s.med, total / n, p99, s.max);
    }
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
    bench_data();
    return 0;
}
