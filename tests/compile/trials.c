/* Compile check: ysp/trials.h as a C translation unit with the implementation
 * enabled. CMake and CI build this as C11 with warnings as errors;
 * trials.cpp builds the same source as C++17. The header has no threads,
 * so there is no YTR_NO_THREADS variant. It runs and returns 0 to prove it
 * links. The table names the lifecycle and the jitter calls by type, so a
 * signature change (seconds back in a jitter, say) fails here and not in a
 * user's build. */
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"

typedef struct trials_api {
    bool (*open)(ytr_trials*, const ytr_desc*);
    bool (*open_saved)(ytr_trials*, const ytr_desc*, const void*, size_t);
    void (*close)(ytr_trials*);
    ytr_jitter_desc (*uniform)(const char*, int64_t, int64_t);
    ytr_jitter_desc (*choice)(const char*, const int64_t*, int);
    ytr_jitter_desc (*exponential)(const char*, int64_t, int64_t, int64_t);
    int (*jitter_check)(const ytr_jitter_desc*, char*, size_t);
} trials_api;

static const trials_api g_trials_api = {
    ytr_open, ytr_open_saved, ytr_close, ytr_uniform, ytr_choice, ytr_exponential,
    ytr_jitter_check
};

int main(void) {
    static ytr_trials t;
    ytr_jitter_desc j = g_trials_api.uniform("iti", YRT_MS(800), YRT_MS(1200));
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
    return 0;
}
