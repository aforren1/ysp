/* trials_save_rows.c - one data row per trial with ytr_data (ysp/trials.h v0.3).
 *
 * A flanker task with a simulated observer: 4 conditions from a CSV text
 * (target and flanker arrows), 10 repetitions in random order, 2 practice
 * trials, and an inter-trial interval drawn per trial. After each trial the
 * program writes one row: the session's columns (index, block, condition,
 * the conditions file's columns, the ITI) and its own (the response, its
 * correctness, the RT in seconds, the participant). A missed response
 * leaves the response and the RT empty and sets a note.
 *
 * The file is CSV with LF line endings that ysp/table.h, pandas and R read
 * as written. A sidecar, <file>.meta, holds the session's settings and ends
 * with "rows=N closed=1" when the program closes the file normally.
 *
 * With --sim the program reads its own file back with ysp/table.h and
 * checks every row against what it wrote (exit 1 on a mismatch). Nothing
 * here touches hardware and nothing is a timing measurement.
 *
 * Build (from the repository root):
 *     cc -O2 -Iinclude -o trials_save_rows examples/trials/save_rows.c -lm
 *     cl /O2 /Iinclude examples\trials\save_rows.c
 *
 * Usage: trials_save_rows [--sim] [--out FILE] [--participant ID] [--seed N]
 * Exit code: 0; 1 if a handle could not be opened or --sim found a
 * mismatch; 2 for a usage error.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YTB_STDIO                   /* ytb_csv_file(), for --sim */
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char conditions[] =
    "target,flanker\n"
    "left,left\n"
    "left,right\n"
    "right,left\n"
    "right,right\n";

/* What the program writes per trial, beside the session's columns. */
typedef struct row {
    const char* response;   /* "left", "right", or NULL for a miss */
    bool        correct;
    int64_t     rt_ns;      /* YTR_MISSING_NS for a miss */
} row;

static const ytr_trial_info ti_zero = { 0, 0, 0, 0, 0, false, false, false, false, false };

int main(int argc, char** argv) {
    static ytr_trials t;
    static ytr_data w;
    static ytb_table tab, back;
    static uint64_t arena[1 << 12], back_arena[1 << 14];
    static int64_t rt_log[YTR_MAX_TRIALS];
    static bool missed[YTR_MAX_TRIALS];
    const char* out = "save_rows.csv";
    const char* participant = "sim01";
    char meta_path[1024], meta[256];
    bool sim = false;
    uint64_t seed = 20261010u, seed0, obs;
    ytb_csv_desc cd;
    ytr_desc d;
    ytr_data_desc dd;
    ytr_trial_info ti = ti_zero;
    row r;
    int i, note, n = 0, bad = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sim = true;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--participant") && i + 1 < argc) participant = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else {
            fprintf(stderr, "usage: trials_save_rows [--sim] [--out FILE] [--participant ID] [--seed N]\n");
            return 2;
        }
    }
    seed0 = seed;                   /* the generator advances it; log this */
    obs = seed ^ 0x5DEECE66DULL;

    memset(&cd, 0, sizeof(cd));
    cd.text = conditions;
    cd.len = sizeof(conditions) - 1;
    cd.arena = arena;
    cd.arena_size = sizeof(arena);
    if (!ytb_csv(&tab, &cd)) { fprintf(stderr, "trials_save_rows: %s\n", ytb_error(&tab)); return 1; }

    memset(&d, 0, sizeof(d));
    d.table = &tab;
    d.reps = 10;
    d.order = YTR_ORDER_FULL_RANDOM;
    d.n_practice = 2;
    d.jitters[0] = ytr_uniform("iti", 0.5, 1.0);
    d.n_jitters = 1;
    d.rng = ytr_splitmix;
    d.rng_ctx = &seed;
    if (!ytr_open(&t, &d)) { fprintf(stderr, "trials_save_rows: %s\n", ytr_error(&t)); return 1; }

    /* The data file: the session's columns, then these. */
    {
        const ytr_col cols[] = {
            YTR_COL(row, response, YTR_COL_STRING),
            YTR_COL(row, correct, YTR_COL_BOOL),
            YTR_COL_D(row, rt_ns, YTR_COL_TIME_NS, 6),   /* seconds, to the microsecond */
            YTR_COL_TEXT("participant", participant),
            YTR_COL_SET("note", YTR_COL_STRING),
        };
        snprintf(meta_path, sizeof(meta_path), "%s.meta", out);
        snprintf(meta, sizeof(meta), "participant=%s\nseed=%llu\n", participant,
                 (unsigned long long)seed0);
        memset(&dd, 0, sizeof(dd));
        dd.path = out;
        dd.trials = &t;
        dd.cols = cols;
        dd.n_cols = (int)(sizeof(cols) / sizeof(cols[0]));
        dd.meta_path = meta_path;
        dd.meta = meta;
        if (!ytr_data_open(&w, &dd)) { fprintf(stderr, "trials_save_rows: %s\n", ytr_data_error(&w)); return 1; }
    }
    note = ytr_data_col(&w, "note");

    while (ytr_next(&t, &ti) >= 0) {
        /* The simulated observer: slower and less accurate when the
         * flanker points the other way; 3 % misses. */
        const char* target = ytb_text(&tab, ti.condition, 0);
        bool congruent = !strcmp(target, ytb_text(&tab, ti.condition, 1));
        double u = ytr_splitmix(&obs);
        bool right = u < (congruent ? 0.95 : 0.85);
        double rt_s = (congruent ? 0.42 : 0.48) - 0.08 * log(1.0 - ytr_splitmix(&obs));
        if (ytr_splitmix(&obs) < 0.03) {
            r.response = NULL;
            r.correct = false;
            r.rt_ns = YTR_MISSING_NS;
            ytr_data_set_string(&w, note, "no response");
        } else {
            r.response = right ? target : (!strcmp(target, "left") ? "right" : "left");
            r.correct = right;
            r.rt_ns = (int64_t)(rt_s * 1e9 + 0.5);
        }
        ytr_update(&t, r.response ? (r.correct ? 1 : 0) : YTR_INVALID, NULL);
        /* Between trials: after update(), before the next next(). */
        if (ytr_data_write(&w, &r) != 0) {
            fprintf(stderr, "trials_save_rows: %s\n", ytr_data_error(&w));
            return 1;
        }
        rt_log[ti.index] = r.rt_ns;
        missed[ti.index] = r.response == NULL;
        /* The ITI the session drew is the program's to wait: ytr_jitter(&t,
         * ti.index, 0).ns. A simulation does not wait. */
        n++;
    }
    if (!ytr_data_close(&w)) { fprintf(stderr, "trials_save_rows: %s\n", ytr_data_error(&w)); return 1; }
    printf("trials_save_rows: %d rows in %s, settings in %s\n", n, out, meta_path);
    for (i = 0; i < ytr_n_conditions(&t); i++)
        printf("  %-5s %-5s  p(correct) %.2f over %d trials\n", ytb_text(&tab, i, 0),
               ytb_text(&tab, i, 1), ytr_proportion(&t, i, 1), ytr_n_valid(&t, i));
    if (!sim) return 0;

    /* --sim: the file read back, row by row, against what was written. */
    memset(&cd, 0, sizeof(cd));
    cd.allow_empty = true;
    cd.arena = back_arena;
    cd.arena_size = sizeof(back_arena);
    if (!ytb_csv_file(&back, out, &cd)) {
        fprintf(stderr, "trials_save_rows: --sim: %s\n", ytb_error(&back));
        return 1;
    }
    if (back.n_rows != n) { fprintf(stderr, "trials_save_rows: --sim: %d rows, not %d\n", back.n_rows, n); return 1; }
    {
        int c_index = ytb_col(&back, "index"), c_target = ytb_col(&back, "target");
        int c_rt = ytb_col(&back, "rt_ns"), c_resp = ytb_col(&back, "response");
        int c_note = ytb_col(&back, "note"), c_iti = ytb_col(&back, "iti");
        int c_part = ytb_col(&back, "participant");
        for (i = 0; i < n; i++) {
            int k = (int)ytb_int(&back, i, c_index);
            double rt = ytb_num(&back, i, c_rt);
            double iti = ytb_num(&back, i, c_iti);
            bool ok = k == i && !strcmp(ytb_text(&back, i, c_target), ytb_text(&tab, ytr_history(&t, NULL)[k].condition, 0));
            if (missed[k]) {
                ok = ok && rt != rt && !strcmp(ytb_text(&back, i, c_resp), "") &&
                     !strcmp(ytb_text(&back, i, c_note), "no response");
            } else {
                /* Six decimals: within half a microsecond of the logged ns. */
                ok = ok && fabs(rt - (double)rt_log[k] * 1e-9) <= 0.5e-6 + 1e-12 &&
                     !strcmp(ytb_text(&back, i, c_note), "");
            }
            ok = ok && iti == ytr_jitter(&t, k, 0).s && !strcmp(ytb_text(&back, i, c_part), participant);
            if (!ok && bad++ < 5) fprintf(stderr, "trials_save_rows: --sim: row %d differs\n", i);
        }
    }
    if (bad) { fprintf(stderr, "trials_save_rows: --sim: %d rows differ\n", bad); return 1; }
    printf("trials_save_rows: --sim: %d rows read back with ysp/table.h, all as written\n", n);
    return 0;
}
