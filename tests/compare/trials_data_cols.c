/* trials_data_cols.c - the data writer's cost per row at 26, 64 and 256
 * columns (ysp/trials.h v0.6, gap G10), its handle sizes, and with glibc
 * the allocations of open() and of the writes. Build it against the
 * headers to compare (docs/trials.md, v0.6, has the table):
 *
 *     cc -O2 -Iinclude tests/compare/trials_data_cols.c -o cols -lm
 *     ./cols
 *
 * The file path rows write g10_bench.csv in the working directory and
 * remove it. Against a header of 64 columns the 256-column rows are skipped. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YRT_NO_THREADS
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GLIBC__)
/* Count every allocation, the C library's own included (glibc lets a
 * program replace malloc for itself and libc). */
extern void* __libc_malloc(size_t);
extern void* __libc_calloc(size_t, size_t);
extern void* __libc_realloc(void*, size_t);
static long g_allocs;
void* malloc(size_t n) { g_allocs++; return __libc_malloc(n); }
void* calloc(size_t a, size_t b) { g_allocs++; return __libc_calloc(a, b); }
void* realloc(void* p, size_t n) { g_allocs++; return __libc_realloc(p, n); }
#define ALLOCS() g_allocs
#else
#define ALLOCS() (-1L)
#endif

#define MAXC 256
typedef struct wrow {
    int64_t t[MAXC];
    double x[MAXC];
    int k[MAXC];
    bool b[MAXC];
    const char* s[MAXC];
} wrow;

static uint64_t g_arena[1 << 16];
static ytb_table g_tab;
static ytr_trials g_t;
static ytr_data g_w;
static wrow g_r;
static ytr_col g_cols[MAXC];
static char g_names[MAXC][16];
static double g_us[20000];
static char g_mem[1 << 16];
static uint64_t g_rs = 7;

static uint64_t rnd(void) {
    uint64_t z = (g_rs += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static bool mem_write(void* ctx, const void* bytes, size_t n) {
    (void)ctx;
    if (n > sizeof(g_mem)) return false;
    memcpy(g_mem, bytes, n);
    return true;
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static void caller_cols(int k) {
    int i;
    for (i = 0; i < k; i++) {
        ytr_col* c = &g_cols[i];
        memset(c, 0, sizeof(*c));
        c->name = g_names[i];
        switch (i % 6) {
        case 0: c->type = YTR_COL_INT; c->offset = offsetof(wrow, k) + (size_t)i * sizeof(int); c->size = sizeof(int); break;
        case 1: c->type = YTR_COL_BOOL; c->offset = offsetof(wrow, b) + (size_t)i * sizeof(bool); c->size = sizeof(bool); break;
        case 2: c->type = YTR_COL_TIME_NS; c->offset = offsetof(wrow, t) + (size_t)i * 8u; c->size = 8; break;
        case 3: c->type = YTR_COL_DOUBLE; c->offset = offsetof(wrow, x) + (size_t)i * 8u; c->size = 8; break;
        case 4: c->type = YTR_COL_DOUBLE; c->decimals = 3; c->offset = offsetof(wrow, x) + (size_t)i * 8u; c->size = 8; break;
        default: c->type = YTR_COL_STRING; c->offset = offsetof(wrow, s) + (size_t)i * sizeof(char*); c->size = sizeof(char*); break;
        }
        snprintf(g_names[i], sizeof(g_names[i]), (i % 6 == 2) ? "c%03d_s" : "c%03d", i);
    }
}

static void fill_row(int k) {
    int i;
    for (i = 0; i < k; i++) {
        g_r.k[i] = (int)(rnd() % 1000);
        g_r.b[i] = (rnd() & 1) != 0;
        g_r.t[i] = 250000000 + (int64_t)(rnd() % 500000000);
        g_r.x[i] = (double)(rnd() >> 11) * (1.0 / 9007199254740992.0);
        g_r.s[i] = (rnd() & 1) ? "left" : "right, late";
    }
}

static void run(int k, int mode, int rows) {
    ytr_desc d;
    ytr_data_desc dd;
    ytr_trial_info ti;
    uint64_t seed = 5;
    int n = 0, i;
    long a0, a1, a2, a3;
    double total = 0;
    static const int64_t soa[4] = { 100000000, 150000000, 200000000, 250000000 };
    memset(&d, 0, sizeof(d));
    d.table = &g_tab;
    d.reps = rows / g_tab.n_rows + 1;
    d.order = YTR_ORDER_FULL_RANDOM;
    d.rng = ytr_splitmix;
    d.rng_ctx = &seed;
    d.jitters[0] = ytr_frames(ytr_uniform("iti_s", YRT_MS(800), YRT_MS(1200)), 60000, 1001);
    d.jitters[1] = ytr_frames(ytr_exponential("fp_s", YRT_MS(500), YRT_MS(2000), YRT_MS(400)), 60000, 1001);
    d.jitters[2] = ytr_exponential("fp2_s", YRT_MS(500), YRT_MS(2000), YRT_MS(400));
    d.jitters[3] = ytr_choice("soa_s", soa, 4);
    d.n_jitters = 4;
    a0 = ALLOCS();
    if (!ytr_open(&g_t, &d)) { printf("open: %s\n", ytr_error(&g_t)); return; }
    caller_cols(k);
    memset(&dd, 0, sizeof(dd));
    if (mode == 0) dd.write = mem_write;
    else dd.path = "g10_bench.csv";
    dd.trials = &g_t;
    dd.cols = g_cols;
    dd.n_cols = k;
    a1 = ALLOCS();
    if (!ytr_data_open(&g_w, &dd)) { printf("  %d columns: %s\n", 18 + k, ytr_data_error(&g_w)); return; }
    a2 = ALLOCS();
    while (n < rows && ytr_next(&g_t, &ti) >= 0) {
        int64_t t0;
        ytr_update(&g_t, 1, NULL);
        fill_row(k);
        t0 = yrt_now_ns();
        if (ytr_data_write(&g_w, &g_r) != 0) { printf("write: %s\n", ytr_data_error(&g_w)); break; }
        g_us[n] = (double)(yrt_now_ns() - t0) * 1e-3;
        total += g_us[n];
        n++;
    }
    a3 = ALLOCS();
    ytr_data_finish(&g_w);
    if (mode) remove("g10_bench.csv");
    qsort(g_us, (size_t)n, sizeof(double), cmp_d);
    i = n * 99 / 100;
    printf("  %3d columns, %-6s %5d rows: median %7.2f us (%5.1f ns/col), mean %7.2f, p99 %7.2f, max %8.2f; "
           "allocs: open %ld, data_open %ld, writes %ld\n",
           18 + k, mode ? "file" : "memory", n, g_us[n / 2], g_us[n / 2] * 1e3 / (18 + k), total / n, g_us[i],
           g_us[n - 1], a1 - a0, a2 - a1, a3 - a2);
}

int main(void) {
    ytb_csv_desc cd;
    static char csv[1 << 14];
    int r, len = 0;
    len += snprintf(csv + len, sizeof(csv) - (size_t)len, "word,ink,cue,target\n");
    for (r = 0; r < 64; r++)
        len += snprintf(csv + len, sizeof(csv) - (size_t)len, "w%d,ink%d,c%d,\"t,%d\"\n", r, r % 3, r % 4, r);
    memset(&cd, 0, sizeof(cd));
    cd.text = csv;
    cd.len = (size_t)len;
    cd.arena = g_arena;
    cd.arena_size = sizeof(g_arena);
    if (!ytb_open_csv(&g_tab, &cd)) { printf("csv: %s\n", ytb_error(&g_tab)); return 1; }
    printf("ysp_trials %s: sizeof ytr_data %lu, ytr_trials %lu, ytb_csv_desc %lu; YTR_DATA_MAX_COLS %d\n",
           ytr_version(), (unsigned long)sizeof(ytr_data), (unsigned long)sizeof(ytr_trials),
           (unsigned long)sizeof(ytb_csv_desc), YTR_DATA_MAX_COLS);
    run(8, 0, 4000);
    run(46, 0, 4000);
    if (YTR_DATA_MAX_COLS >= 256) run(238, 0, 4000);
    run(8, 1, 4000);
    run(46, 1, 4000);
    if (YTR_DATA_MAX_COLS >= 256) run(238, 1, 4000);
    return 0;
}
