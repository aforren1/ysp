/* adapt_fuzz.c - fuzz target for the snapshot readers of the adaptive
 * methods: yst_open_saved() (ysp/stair.h), yqst_open_saved() (ysp/quest.h)
 * and yaep_open_saved() (ysp/aep.h). A snapshot is read back from disk, so
 * each reader takes untrusted bytes.
 *
 * Input: byte 0 picks the header (mod 3), byte 1 one of its fixed descs, and
 * the rest is the snapshot. When a load succeeds the session runs a few
 * trials and its estimates; a staircase must also save back to exactly the
 * bytes it loaded (every field is stored as read), and aborts otherwise.
 * Every input must return without a sanitizer report; a refusal is a
 * correct answer, and a refused load must leave the handle closed.
 *
 * libFuzzer, clang (WSL or Linux), from the repository root:
 *     clang -O1 -g -fsanitize=fuzzer,address,undefined -Iinclude tests/fuzz/adapt_fuzz.c -o adapt_fuzz -lm
 *     ./adapt_fuzz -max_total_time=1200 -timeout=10 corpus_adapt
 * libFuzzer, MSVC 2022 x64 (in a vcvars shell):
 *     cl /nologo /O1 /Zi /fsanitize=address /fsanitize=fuzzer /Iinclude tests\fuzz\adapt_fuzz.c
 * Seeds (no corpus is committed; the target writes real snapshots of every
 * desc, cut at several trials):
 *     cc -DYSP_FUZZ_SEEDS -Iinclude tests/fuzz/adapt_fuzz.c -o seeds -lm && ./seeds corpus_adapt
 * Replay of a corpus without libFuzzer (gcc or clang under UBSan), every
 * file of the directory as an argument:
 *     gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
 *         -DYSP_FUZZ_REPLAY -Iinclude tests/fuzz/adapt_fuzz.c -o replay -lm
 *     ./replay corpus_adapt/<files>
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
/* Small capacities keep an input fast and let the fuzzer reach the caps. */
#define YST_MAX_TRIALS 64
#define YST_MAX_REVERSALS 16
#define YQST_MAX_TRIALS 64
#define YSP_STAIR_IMPLEMENTATION
#include "ysp/stair.h"
#define YSP_QUEST_IMPLEMENTATION
#include "ysp/quest.h"
#define YSP_AEP_IMPLEMENTATION
#include "ysp/aep.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static yst_stair g_st;
static yqst_quest g_q;
static yaep_gp g_g;
static unsigned char g_out[1 << 20];
static volatile double g_sink;

#define N_STAIR 3
#define N_QUEST 2
#define N_AEP 2

static void stair_desc(yst_desc* d, int v) {
    memset(d, 0, sizeof(*d));
    d->stop_trials = 40;
    switch (v % N_STAIR) {
    case 0:   /* 1-up-1-down, linear, a real 0 bound */
        d->start = 3.0; d->steps[0] = 1.0; d->n_steps = 1;
        d->use_limits = true; d->min = 0.0; d->max = 6.0; d->stop_at_limit = 5;
        break;
    case 1:   /* 1-up-3-down, log10 with a schedule and the initial rule */
        d->start = 0.5; d->n_down = 3; d->step_type = YST_STEP_LOG;
        d->steps[0] = 0.3; d->steps[1] = 0.15; d->steps[2] = 0.075; d->n_steps = 3;
        d->min = 0.01; d->max = 1.0; d->initial_rule = true; d->stop_reversals = 12;
        break;
    default:  /* ASA in dB */
        d->rule = YST_RULE_ASA; d->start = 2.0; d->step_type = YST_STEP_DB;
        d->steps[0] = 2.0; d->n_steps = 1; d->target_p = 0.75;
        break;
    }
}

static const double g_qbeta[3] = { 1.0, 2.0, 4.0 };

static void quest_desc(yqst_desc* d, int v) {
    memset(d, 0, sizeof(*d));
    d->stim[0] = yqst_linspace(-2.0, 0.0, 9);
    d->n_stim = 1;
    d->param[0] = yqst_linspace(-2.0, 0.0, 7);
    d->param[1] = yqst_values(g_qbeta, 3);
    d->param[2] = yqst_fixed(0.5);
    d->param[3] = yqst_fixed(0.02);
    d->n_param = 4;
    d->stop_trials = 30;
    if (v % N_QUEST == 1) {
        d->param[1].nuisance = true;   /* the permuted posterior */
        d->tiebreak = YQST_TIE_ALTERNATE;
    }
}

static void aep_desc(yaep_desc* d, int v) {
    memset(d, 0, sizeof(*d));
    d->n_dims = 2;
    d->lo[0] = 0.0; d->hi[0] = 1.0;
    d->lo[1] = -1.0; d->hi[1] = 1.0;
    d->target_p = 0.7;
    d->stop_trials = 20;
    d->max_trials = 16;
    d->n_candidates = 32;
    if (v % N_AEP == 1) {
        d->lik = YAEP_LIK_ORDINAL;
        d->n_outcomes = 3;
        d->fit = true;
        d->fit_every = 3;
    }
}

static int resp(int t) { return (t * 7 + 3) % 5 < 3; }

static void run_stair(const uint8_t* b, size_t n, int v) {
    yst_desc d;
    int t, len;
    stair_desc(&d, v);
    if (!yst_open_saved(&g_st, &d, b, n)) {
        if (yst_is_open(&g_st) || yst_error(&g_st)[0] == '\0') abort();
        return;
    }
    len = yst_save(&g_st, g_out, sizeof g_out);
    if (len < 0 || (size_t)len != n || memcmp(g_out, b, n) != 0) abort();
    for (t = 0; t < 4; t++) {
        double x = yst_next(&g_st);
        g_sink += x + yst_estimate(&g_st, (yst_estimator)t);
        if (yst_update(&g_st, x, resp(t)) == YST_ERR_FULL) break;
    }
    yst_close(&g_st);
}

static void run_quest(const uint8_t* b, size_t n, int v) {
    yqst_desc d;
    double est[YQST_MAX_PARAMS];
    int t, s;
    quest_desc(&d, v);
    if (!yqst_open_saved(&g_q, &d, b, n)) {
        if (yqst_is_open(&g_q) || yqst_error(&g_q)[0] == '\0') abort();
        return;
    }
    for (t = 0; t < 3; t++) {
        s = yqst_next(&g_q);
        if (s < 0 || yqst_update(&g_q, s, resp(t)) < 0) break;
    }
    if (yqst_estimate(&g_q, YQST_EST_MEAN, est) >= 0) g_sink += est[0];
    (void)yqst_save(&g_q, g_out, sizeof g_out);
    yqst_close(&g_q);
}

static void run_aep(const uint8_t* b, size_t n, int v) {
    yaep_desc d;
    double x[YAEP_MAX_DIMS];
    int t;
    aep_desc(&d, v);
    if (!yaep_open_saved(&g_g, &d, b, n)) {
        if (yaep_is_open(&g_g) || yaep_error(&g_g)[0] == '\0') abort();
        return;
    }
    for (t = 0; t < 2; t++) {
        if (yaep_next(&g_g, x) < -1) break;
        if (yaep_update(&g_g, x, d.lik == YAEP_LIK_ORDINAL ? t % 3 : resp(t)) < 0) break;
    }
    (void)yaep_save(&g_g, g_out, sizeof g_out);
    yaep_close(&g_g);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;
    switch (data[0] % 3) {
    case 0: run_stair(data + 2, size - 2, data[1]); break;
    case 1: run_quest(data + 2, size - 2, data[1]); break;
    default: run_aep(data + 2, size - 2, data[1]); break;
    }
    return 0;
}

#if defined(YSP_FUZZ_SEEDS)
static int write_seed(const char* dir, int h, int v, int cut, const unsigned char* b, int n) {
    char path[1024];
    unsigned char hdr[2];
    FILE* f;
    if (n <= 0) return 0;
    snprintf(path, sizeof(path), "%s/h%d_v%d_t%02d", dir, h, v, cut);
    f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    hdr[0] = (unsigned char)h;
    hdr[1] = (unsigned char)v;
    fwrite(hdr, 1, 2, f);
    fwrite(b, 1, (size_t)n, f);
    fclose(f);
    return 1;
}

int main(int argc, char** argv) {
    static const int cuts[] = { 0, 1, 5, 13 };
    int v, c, t, wrote = 0;
    if (argc < 2) { fprintf(stderr, "usage: seeds DIR\n"); return 1; }
    for (v = 0; v < N_STAIR; v++)
        for (c = 0; c < 4; c++) {
            yst_desc d;
            stair_desc(&d, v);
            if (!yst_open(&g_st, &d)) return 2;
            for (t = 0; t < cuts[c] * 3; t++) yst_update(&g_st, yst_next(&g_st), resp(t));
            wrote += write_seed(argv[1], 0, v, c, g_out, yst_save(&g_st, g_out, sizeof g_out));
        }
    for (v = 0; v < N_QUEST; v++)
        for (c = 0; c < 4; c++) {
            yqst_desc d;
            quest_desc(&d, v);
            if (!yqst_open(&g_q, &d)) return 3;
            for (t = 0; t < cuts[c]; t++) yqst_update(&g_q, yqst_next(&g_q), resp(t));
            wrote += write_seed(argv[1], 1, v, c, g_out, yqst_save(&g_q, g_out, sizeof g_out));
            yqst_close(&g_q);
        }
    for (v = 0; v < N_AEP; v++)
        for (c = 0; c < 4; c++) {
            yaep_desc d;
            double x[YAEP_MAX_DIMS];
            aep_desc(&d, v);
            if (!yaep_open(&g_g, &d)) return 4;
            for (t = 0; t < cuts[c]; t++) {
                yaep_next(&g_g, x);
                yaep_update(&g_g, x, d.lik == YAEP_LIK_ORDINAL ? t % 3 : resp(t));
            }
            wrote += write_seed(argv[1], 2, v, c, g_out, yaep_save(&g_g, g_out, sizeof g_out));
            yaep_close(&g_g);
        }
    printf("adapt_fuzz: wrote %d seeds to %s\n", wrote, argv[1]);
    return 0;
}
#elif defined(YSP_FUZZ_REPLAY)
int main(int argc, char** argv) {
    static uint8_t buf[1 << 21];
    int i, done = 0;
    for (i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");
        size_t n;
        if (!f) continue;
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        LLVMFuzzerTestOneInput(buf, n);
        done++;
    }
    printf("adapt_fuzz: replayed %d inputs\n", done);
    return 0;
}
#endif
