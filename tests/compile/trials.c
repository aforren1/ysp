/* Compile check: ysp/trials.h as a C translation unit with the implementation
 * enabled. CMake and CI build this as C11 with warnings as errors;
 * trials.cpp builds the same source as C++17. The header has no threads,
 * so there is no YTR_NO_THREADS variant. It runs and returns 0 to prove it
 * links. The table names the lifecycle, the jitter and the data writer
 * calls by type, so a signature change (seconds back in a jitter, say)
 * fails here and not in a user's build. The data writer's column macros
 * are used in a static array, which C needs to be constant expressions and
 * C++ must accept as aggregates. */
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"

#include <string.h>

typedef struct trials_api {
    bool (*open)(ytr_trials*, const ytr_desc*);
    bool (*open_saved)(ytr_trials*, const ytr_desc*, const void*, size_t);
    void (*close)(ytr_trials*);
    ytr_jitter_desc (*uniform)(const char*, int64_t, int64_t);
    ytr_jitter_desc (*choice)(const char*, const int64_t*, int);
    ytr_jitter_desc (*exponential)(const char*, int64_t, int64_t, int64_t);
    int (*jitter_check)(const ytr_jitter_desc*, char*, size_t);
    bool (*data_open)(ytr_data*, const ytr_data_desc*);
    int (*data_write)(ytr_data*, const void*);
    int (*data_set_time_ns)(ytr_data*, int, int64_t);
    int (*data_finish)(ytr_data*);
    void (*data_close)(ytr_data*);
    ytr_var_desc (*var_choice)(const char*, const char* const*, int);   /* v0.6 */
    int (*var)(const ytr_trials*, int, int);
    const char* (*var_text)(const ytr_trials*, int, int);
    uint64_t (*trial_seed)(const ytr_trials*, int);
} trials_api;

static const trials_api g_trials_api = {
    ytr_open, ytr_open_saved, ytr_close, ytr_uniform, ytr_choice, ytr_exponential,
    ytr_jitter_check, ytr_data_open, ytr_data_write, ytr_data_set_time_ns, ytr_data_finish,
    ytr_data_close, ytr_var_choice, ytr_var, ytr_var_text, ytr_trial_seed
};

typedef struct check_row {
    int     key;
    double  level;
    int64_t rt_ns;
    char    code[4];
} check_row;

static const ytr_col check_cols[] = {
    YTR_COL(check_row, key, YTR_COL_INT),
    YTR_COL_D(check_row, level, YTR_COL_DOUBLE, 3),
    YTR_COL(check_row, rt_ns, YTR_COL_TIME_NS),
    YTR_COL(check_row, code, YTR_COL_CHARS),
    YTR_COL_SET("note", YTR_COL_STRING),
    YTR_COL_TEXT("participant", "P01"),
};

static bool discard(void* ctx, const void* bytes, size_t n) {
    (void)ctx;
    (void)bytes;
    (void)n;
    return true;
}

int main(void) {
    static ytr_trials t;
    static ytr_data w;
    ytr_data_desc dd;
    check_row r = { 1, 0.5, 250000000, "ab" };
    ytr_jitter_desc j = g_trials_api.uniform("iti_s", YRT_MS(800), YRT_MS(1200));
    ytr_jitter_value v;
    if (g_trials_api.jitter_check(&j, NULL, 0) < 0) return 1;
    v = ytr_jitter_map(&j, 0.0);
    if (v.ns != YRT_MS(800) || v.frames != -1) return 2;
    if (g_trials_api.open(&t, NULL)) return 3;
    g_trials_api.close(&t);
    g_trials_api.close(&t);
    if (g_trials_api.open_saved(&t, NULL, NULL, 0)) return 4;
    (void)g_trials_api.choice;
    (void)g_trials_api.exponential;

    memset(&dd, 0, sizeof(dd));
    dd.write = discard;
    dd.cols = check_cols;
    dd.n_cols = (int)(sizeof(check_cols) / sizeof(check_cols[0]));
    if (!g_trials_api.data_open(&w, &dd)) return 5;
    if (ytr_data_set_string(&w, ytr_data_col(&w, "note"), "ok") != 0) return 6;
    if (g_trials_api.data_write(&w, &r) != 0) return 7;
    (void)g_trials_api.data_set_time_ns;
    if (g_trials_api.data_finish(&w) != 0) return 8;
    g_trials_api.data_close(&w);
    g_trials_api.data_close(&w);
    {
        /* v0.5: a TIME_NS column without _s or _ns is refused. */
        static const ytr_col bare[] = { YTR_COL_SET("rt", YTR_COL_TIME_NS) };
        dd.cols = bare;
        dd.n_cols = 1;
        if (g_trials_api.data_open(&w, &dd)) return 9;
    }
    {
        /* v0.6: a variable drawn per trial, a jitter at the display's rate. */
        static const char* const sides[] = { "left", "right" };
        static ytr_desc d;
        uint64_t s = 1;
        d.n_conditions = 1;
        d.reps = 2;
        d.rng = ytr_splitmix;
        d.rng_ctx = &s;
        d.seed = 7;
        d.vars[0] = g_trials_api.var_choice("side", sides, 2);
        d.n_vars = 1;
        d.jitters[0] = ytr_frames(j, YTR_RATE_DISPLAY, 0);
        d.n_jitters = 1;
        d.display_rate_num = 60;
        if (!g_trials_api.open(&t, &d) || ytr_next(&t, NULL) != 0) return 10;
        if (g_trials_api.var(&t, 0, 0) < 0 || !g_trials_api.var_text(&t, 0, 0)) return 11;
        if (g_trials_api.trial_seed(&t, 0) == 0 || ytr_jitter(&t, 0, 0).frames < 48) return 12;
        g_trials_api.close(&t);
    }
    return 0;
}
