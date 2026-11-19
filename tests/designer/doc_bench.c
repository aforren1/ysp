/* doc_bench.c - what one edit costs in the designer's document
 * (designer/core/doc.h, csv.h; docs/designer.md, "Plan of step 4",
 * "Measured"). Not a test: it prints times.
 *
 * Usage: bench_designer_doc [ROUNDS]
 *   Definitions: tests/experiment/good/kitchen_sink.json and
 *   tests/player/bimanual/experiment.json as they are, and the bimanual
 *   definition with its templates copied until it is about 100 KB and 1 MB.
 *   For each, per edit (ROUNDS edits, default 200):
 *     parse     yjs_parse() of the bytes (the cache's cost per revision)
 *     edit      one dsg_def_set_seconds_ns() as its own step: the work
 *               tree's parse, the change, the canonical write, the trim and
 *               the splice
 *     reparse   dsg_doc_root() after the edit
 *     typing    one character typed into the text (dsg_doc_replace() with
 *               DSG_EDIT_MERGE): the trim's scan
 *     yexp      yexp_load() and yexp_canonical(), no host: what Save does,
 *               and the loader's part of each check
 *   Tables: 10,000 and 32,767 rows (ysp/table.h's limit) of 8 columns.
 *     index     dsg_csv_index() of the whole table
 *     cell      one cell's splice (dsg_doc_splice()) and the index again
 * Each line: median, p99 and max in microseconds.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define _POSIX_C_SOURCE 199309L
#include "csv.h"
#include "doc.h"
#include "dsg.h"
#include "player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef YSP_SOURCE_DIR
#define YSP_SOURCE_DIR "."
#endif

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static void report(const char* what, const char* name, double* t, int n) {
    qsort(t, (size_t)n, sizeof *t, cmp_d);
    printf("  %-8s %-26s median %9.1f  p99 %9.1f  max %9.1f us\n", what, name, t[n / 2], t[(n * 99) / 100], t[n - 1]);
}

/* The bimanual definition with its templates copied under new names until
 * the canonical bytes reach `target`. malloc'd. */
static char* scaled(const char* src, size_t n, size_t target, size_t* out_n) {
    yjs_arena a;
    yjs_value* root = NULL;
    yjs_error e;
    yjs_value* trials;
    char* out = NULL;
    size_t len = 0;
    int k;
    yjs_arena_init_heap(&a, 0);
    if (yjs_parse(&a, src, n, NULL, &root, &e) != YJS_OK || !(trials = yjs_get(root, "trials"))) goto done;
    {
        uint32_t base = trials->n, i;
        for (k = 1; len < target && k < 100000; k++) {
            for (i = 0; i < base; i++) {
                char nm[160];
                snprintf(nm, sizeof nm, "%s_%d", trials->u.members[i].key, k);
                yjs_set(&a, trials, nm, yjs_copy(&a, trials->u.members[i].value));
            }
            len = yjs_write_mem(root, YJS_WRITE_PRETTY, NULL, 0);
        }
    }
    out = (char*)malloc(len + 2);
    if (out) {
        yjs_write_mem(root, YJS_WRITE_PRETTY, out, len + 1);
        out[len++] = '\n';
        out[len] = 0;
        *out_n = len;
    }
done:
    yjs_arena_free(&a);
    return out;
}

static void bench_def(const char* name, const char* bytes, size_t n, int rounds) {
    dsg_doc* d = dsg_doc_new();
    double* t = (double*)malloc((size_t)rounds * sizeof *t);
    yexp_report* rep = (yexp_report*)calloc(1, sizeof *rep);
    char path[DSG_SEL_PATH] = "";
    const yjs_value* root;
    int i;
    char label[64];
    snprintf(label, sizeof label, "%s (%zu B)", name, n);
    if (!d || !t || !rep) return;
    dsg_doc_reset(d, "experiment.json", bytes, n);
    root = dsg_doc_root(d);
    /* a duration to set: the first exit's after_s, else the first wait_s */
    {
        const yjs_value* trials = yjs_get(root, "trials");
        uint32_t i2, j;
        for (i2 = 0; trials && i2 < trials->n && !path[0]; i2++) {
            const yjs_value* st = yjs_get(trials->u.members[i2].value, "states");
            for (j = 0; st && j < st->n && !path[0]; j++) {
                const yjs_value* ex = yjs_get(st->u.items[j], "exits");
                char base[DSG_SEL_PATH];
                if (!ex || ex->n == 0) continue;
                dsg_path_key(base, sizeof base, "/trials", trials->u.members[i2].key);
                snprintf(path, sizeof path, "%s/states/%u/exits/0/after_s", base, j);
            }
        }
        if (!path[0]) snprintf(path, sizeof path, "/bench_s");
    }

    for (i = 0; i < rounds; i++) {
        yjs_arena a;
        yjs_value* r = NULL;
        yjs_error e;
        double t0;
        yjs_arena_init_heap(&a, 0);
        t0 = now_us();
        yjs_parse(&a, bytes, n, NULL, &r, &e);
        t[i] = now_us() - t0;
        yjs_arena_free(&a);
    }
    report("parse", label, t, rounds);

    for (i = 0; i < rounds; i++) {
        double t0 = now_us();
        if (dsg_def_set_seconds_ns(d, path, 100000000LL + i * 1000000LL)) {
            printf("  edit failed: %s\n", dsg_doc_error(d));
            break;
        }
        t[i] = now_us() - t0;
    }
    report("edit", label, t, i > 0 ? i : 1);

    for (i = 0; i < rounds; i++) {
        double t0;
        dsg_def_set_seconds_ns(d, path, 200000000LL + i * 1000000LL);
        t0 = now_us();
        dsg_doc_root(d);
        t[i] = now_us() - t0;
    }
    report("reparse", label, t, rounds);

    {
        size_t m;
        const char* cur = dsg_doc_bytes(d, DSG_BUF_DEF, &m);
        char* typed = (char*)malloc(m + (size_t)rounds + 1);
        size_t mid = m / 2;
        if (typed) {
            memcpy(typed, cur, m);
            for (i = 0; i < rounds; i++) {
                double t0;
                /* one more character at the middle each round, as typing does */
                memmove(typed + mid + 1, typed + mid, m - mid);
                typed[mid++] = ' ';
                m++;
                t0 = now_us();
                dsg_doc_replace(d, DSG_BUF_DEF, typed, m, DSG_EDIT_MERGE);
                t[i] = now_us() - t0;
            }
            report("typing", label, t, rounds);
            free(typed);
        }
    }

    for (i = 0; i < rounds && i < 100; i++) {
        yjs_arena a;
        yexp_def* def = NULL;
        const char* cb;
        size_t cn;
        double t0;
        int rc;
        yjs_arena_init_heap(&a, 0);
        t0 = now_us();
        rc = yexp_load(&a, bytes, n, NULL, &def, rep);
        if (rc == YEXP_OK) rc = yexp_canonical(def, &a, &cb, &cn, NULL);
        t[i] = now_us() - t0;
        yjs_arena_free(&a);
        if (rc != YEXP_OK) {
            printf("  yexp     %-26s refused: %s\n", label, rep->n_diags ? rep->diags[0].msg : "?");
            i = -1;
            break;
        }
    }
    if (i > 0) report("yexp", label, t, i);
    free(rep);
    free(t);
    dsg_doc_free(d);
}

static void bench_table(int rows, int rounds) {
    size_t cap = (size_t)rows * 96 + 256, n = 0;
    char* csv = (char*)malloc(cap);
    dsg_doc* d = dsg_doc_new();
    double* t = (double*)malloc((size_t)rounds * sizeof *t);
    dsg_csv c;
    char err[256], label[64];
    int r, i;
    static const char* const words[] = { "red", "green", "blue", "\xD8\xA3\xD8\xAD\xD9\x85\xD8\xB1", "\"a, b\"", "yellow" };
    memset(&c, 0, sizeof c);
    if (!csv || !d || !t) return;
    n += (size_t)snprintf(csv + n, cap - n, "trial,block,target,contrast,rotation_deg,rt_s,word,label\n");
    for (r = 0; r < rows; r++)
        n += (size_t)snprintf(csv + n, cap - n, "%d,%d,%d,%.3f,%.1f,%.4f,%s,item %d\n", r, r / 100, r % 8, 0.05 + (r % 20) * 0.05,
                              (double)((r * 37) % 360) - 180.0, 0.3 + (r % 97) * 0.004, words[r % 6], r);
    snprintf(label, sizeof label, "%d x 8 (%zu B)", rows, n);
    dsg_doc_reset(d, "experiment.json", "{}", 2);
    dsg_doc_add_buffer(d, DSG_KIND_TABLE, "t.csv", csv, n);

    for (i = 0; i < rounds; i++) {
        double t0 = now_us();
        if (dsg_csv_index(&c, csv, n, ',', err, sizeof err)) { printf("  index failed: %s\n", err); return; }
        t[i] = now_us() - t0;
    }
    report("index", label, t, rounds);

    for (i = 0; i < rounds; i++) {
        size_t bn;
        const char* b = dsg_doc_bytes(d, 1, &bn);
        int row = (i * 7919) % rows, col = 3;
        size_t at = (size_t)(row + 1) * 8 + (size_t)col;
        double t0;
        char v[32];
        dsg_csv_index(&c, b, bn, ',', err, sizeof err);
        snprintf(v, sizeof v, "%.3f", 0.001 * i);
        t0 = now_us();
        dsg_doc_splice(d, 1, c.off[at], c.len[at], v, strlen(v));
        b = dsg_doc_bytes(d, 1, &bn);
        dsg_csv_index(&c, b, bn, ',', err, sizeof err);
        t[i] = now_us() - t0;
    }
    report("cell", label, t, rounds);
    dsg_csv_free(&c);
    dsg_doc_free(d);
    free(t);
    free(csv);
}

int main(int argc, char** argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 200;
    char err[512];
    char* ks = NULL;
    char* bi = NULL;
    size_t ksn = 0, bin = 0;
    if (rounds < 10) rounds = 10;
    if (dsg_read_file(YSP_SOURCE_DIR "/tests/experiment/good/kitchen_sink.json", &ks, &ksn, err, sizeof err) ||
        dsg_read_file(YSP_SOURCE_DIR "/tests/player/bimanual/experiment.json", &bi, &bin, err, sizeof err)) {
        printf("%s\n", err);
        return 1;
    }
    printf("bench_designer_doc: %d rounds\n", rounds);
    bench_def("kitchen_sink", ks, ksn, rounds);
    bench_def("bimanual", bi, bin, rounds);
    {
        size_t sn = 0;
        char* s = scaled(bi, bin, 100000, &sn);
        if (s) bench_def("bimanual x~20", s, sn, rounds);
        free(s);
        s = scaled(bi, bin, 1000000, &sn);
        if (s) bench_def("bimanual x~200", s, sn, rounds);
        free(s);
    }
    bench_table(10000, rounds);
    bench_table(32767, rounds);
    free(ks);
    free(bi);
    return 0;
}
