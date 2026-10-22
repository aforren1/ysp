/* dataset_bench.c - costs of ysp/dataset.h: a row of each table, the
 * atomic rewrite of session.json, the open, the finish and the zip.
 * Not run by CI for its numbers; docs/dataset.md holds them, with the
 * machine and the conditions. Run it alone on a quiet machine.
 *
 *     dataset_bench [NAME] [rows]   (default dataset_bench, 10000 rows)
 *
 * The program makes NAME_row and NAME_sync (two data sets, which must not
 * exist) and NAME_row.zip, and leaves them. Per call: median, p99 and max of
 * the times read around each call with ysp/rt.h's clock.
 *   frame, event, trial  YTR_FLUSH_ROW: one write call to the OS per row
 *   frame sync           YTR_FLUSH_SYNC: the row on the disk, rows / 20
 *   session rewrite      yds_session_set(): session.json.tmp written,
 *                        synced to the disk and renamed over, 200 times
 * Exit code: 0; 1 if a call failed; 2 for a usage error.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YRT_NO_THREADS
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"
#define YSP_DATASET_IMPLEMENTATION
#include "ysp/dataset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct trow {
    int         trial_index;
    const char* response;
    bool        correct;
    int64_t     rt_ns;
    double      level;
} trow;

static const ytr_col trial_cols[] = {
    YTR_COL_TEXT("participant", "P01"),
    YTR_COL(trow, trial_index, YTR_COL_INT),
    YTR_COL(trow, response, YTR_COL_STRING),
    YTR_COL(trow, correct, YTR_COL_BOOL),
    YTR_COL(trow, rt_ns, YTR_COL_TIME_NS),
    YTR_COL(trow, level, YTR_COL_DOUBLE),
};

static int cmp64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}

static void report(const char* what, int64_t* t, int n) {
    if (n <= 0) return;
    qsort(t, (size_t)n, sizeof t[0], cmp64);
    printf("%-18s n=%6d  median %8.1f us  p99 %8.1f us  max %8.1f us\n", what, n, (double)t[n / 2] / 1e3,
           (double)t[(int)((double)n * 0.99)] / 1e3, (double)t[n - 1] / 1e3);
}

static bool open_set(yds_dataset* ds, const char* dir, ytr_flush flush) {
    yds_desc d;
    memset(&d, 0, sizeof d);
    d.dir = dir;
    d.trial_cols = trial_cols;
    d.n_trial_cols = (int)(sizeof trial_cols / sizeof trial_cols[0]);
    d.flush = flush;
    if (!yds_open(ds, &d)) {
        fprintf(stderr, "%s\n", yds_error(ds));
        return false;
    }
    return true;
}

static bool g_bad = false;
static void ok(yds_dataset* ds, int rc) {
    if (rc < 0 && !g_bad) {
        fprintf(stderr, "%s\n", yds_error(ds));
        g_bad = true;
    }
}

static bool count_bytes(void* ctx, const void* b, size_t n) {
    (void)b;
    *(uint64_t*)ctx += n;
    return true;
}

int main(int argc, char** argv) {
    static yds_dataset ds;
    const char* name = argc > 1 ? argv[1] : "dataset_bench";
    int rows = argc > 2 ? atoi(argv[2]) : 10000, i, n;
    int64_t *t, t0, t1;
    char dir[900], zpath[1000];
    yds_frame f;
    yds_event e;
    trow r;
    yjs_arena a;
    yjs_value* v;
    uint64_t zbytes = 0;
    if (rows < 100 || rows > 10000000 || strlen(name) > 800) {
        fprintf(stderr, "usage: dataset_bench [NAME] [rows >= 100]\n");
        return 2;
    }
    t = (int64_t*)malloc((size_t)rows * sizeof t[0]);
    if (!t) return 1;
    yjs_arena_init_heap(&a, 0);

    snprintf(dir, sizeof dir, "%s_row", name);
    t0 = yrt_now_ns();
    if (!open_set(&ds, dir, YTR_FLUSH_ROW)) return 1;
    t1 = yrt_now_ns();
    printf("open               %8.1f us\n", (double)(t1 - t0) / 1e3);
    yds_frame_init(&f);
    f.trial_index = 0;
    f.path = 1;
    f.tier = 2;
    f.flags = 0;
    f.dropped = 0;
    f.code_risk = 0;
    for (i = 0; i < rows; i++) {
        f.frame = i;
        f.target_ns = 5000000000LL + (int64_t)i * 16666667;
        f.planned_ns = f.target_ns + 120;
        f.onset_ns = f.planned_ns + 35000 + i % 977;
        f.residual_ns = f.onset_ns - f.target_ns;
        f.evaluate_ns = 12000 + i % 31;
        f.draw_ns = 250000 + i % 1013;
        f.swap_ns = 900000 + i % 7;
        t0 = yrt_now_ns();
        ok(&ds, yds_write_frame(&ds, &f));
        t[i] = yrt_now_ns() - t0;
    }
    report("frame", t, rows);
    for (i = 0; i < rows; i++) {
        yds_event_init(&e);
        e.t_ns = 5000000000LL + (int64_t)i * 1000003;
        e.trial_index = i / 10;
        e.kind = (i & 1) ? "onset" : "keyboard";
        e.role = (i & 1) ? NULL : "resp";
        e.stimulus = (i & 1) ? "gabor" : NULL;
        e.frame = i;
        e.code = i & 255;
        e.tier = 2;
        t0 = yrt_now_ns();
        ok(&ds, yds_write_event(&ds, &e));
        t[i] = yrt_now_ns() - t0;
    }
    report("event", t, rows);
    n = rows / 10;
    for (i = 0; i < n; i++) {
        r.trial_index = i;
        r.response = (i & 1) ? "left" : "right";
        r.correct = (i % 3) != 0;
        r.rt_ns = 412345678 + (int64_t)i * 1013;
        r.level = 0.01 * (double)(i % 50);
        t0 = yrt_now_ns();
        ok(&ds, yds_write_trial(&ds, &r));
        t[i] = yrt_now_ns() - t0;
    }
    report("trial", t, n);
    n = 200;
    v = yjs_new(&a, YJS_OBJECT);
    yjs_set(&a, v, "at_trial", yjs_new_int(&a, 1));
    yjs_set(&a, v, "experiment", yjs_new_stringz(&a, "sha256:0123456789abcdef"));
    for (i = 0; i < n; i++) {
        t0 = yrt_now_ns();
        ok(&ds, yds_session_set(&ds, "last_change", v));
        t[i] = yrt_now_ns() - t0;
    }
    report("session rewrite", t, n);
    t0 = yrt_now_ns();
    ok(&ds, yds_finish(&ds));
    t1 = yrt_now_ns();
    printf("finish             %8.1f us\n", (double)(t1 - t0) / 1e3);
    snprintf(zpath, sizeof zpath, "%s.zip", dir);
    t0 = yrt_now_ns();
    ok(&ds, yds_zip(&ds, zpath, NULL, NULL));
    t1 = yrt_now_ns();
    ok(&ds, yds_zip(&ds, NULL, count_bytes, &zbytes));
    printf("zip to a file      %8.1f ms  %.1f MB, %.0f MB/s\n", (double)(t1 - t0) / 1e6, (double)zbytes / 1e6,
           (double)zbytes / 1e6 / ((double)(t1 - t0) / 1e9));
    yds_close(&ds);

    /* Open and close of an empty data set: the handle's memset, the
     * folder, session.json twice, five files. */
    printf("handle             %lu bytes (ytr_data %lu)\n", (unsigned long)sizeof ds,
           (unsigned long)sizeof(ytr_data));
    n = 20;
    for (i = 0; i < n; i++) {
        snprintf(dir, sizeof dir, "%s_oc%02d", name, i);
        t0 = yrt_now_ns();
        if (!open_set(&ds, dir, YTR_FLUSH_ROW)) return 1;
        t1 = yrt_now_ns();
        yds_close(&ds);
        t[i] = t1 - t0;
        t[n + i] = yrt_now_ns() - t1;
    }
    report("open (empty)", t, n);
    report("close (empty)", t + n, n);

    snprintf(dir, sizeof dir, "%s_sync", name);
    if (!open_set(&ds, dir, YTR_FLUSH_SYNC)) return 1;
    n = rows / 20;
    yds_frame_init(&f);
    for (i = 0; i < n; i++) {
        f.frame = i;
        f.onset_ns = 5000000000LL + (int64_t)i * 16666667;
        t0 = yrt_now_ns();
        ok(&ds, yds_write_frame(&ds, &f));
        t[i] = yrt_now_ns() - t0;
    }
    report("frame sync", t, n);
    yds_close(&ds);
    yjs_arena_free(&a);
    free(t);
    return g_bad ? 1 : 0;
}
