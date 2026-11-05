/* Compile check: ysp/quest.h as a C translation unit with the implementation
 * enabled. CMake and CI build this as C11 with warnings as errors;
 * quest.cpp builds the same source as C++17, and the _async variant defines
 * YQST_ASYNC. The header owns no threads, so there is no no-threads variant.
 * It runs and returns 0 to prove it links. The tables name the lifecycle by
 * type, so a signature change fails here and not in a user's build. */
#define YSP_QUEST_IMPLEMENTATION
#include "ysp/quest.h"

typedef struct quest_api {
    bool (*open)(yqst_quest*, const yqst_desc*);
    bool (*open_saved)(yqst_quest*, const yqst_desc*, const void*, size_t);
    void (*close)(yqst_quest*);
    const char* (*error)(const yqst_quest*);
    int (*prior_normal)(const yqst_axis*, double, double, double*);
    int (*save)(const yqst_quest*, void*, size_t);
    const yqst_param* (*params)(int*);
} quest_api;

static const quest_api g_quest_api = {
    yqst_open, yqst_open_saved, yqst_close, yqst_error, yqst_prior_normal, yqst_save,
    yqst_params
};

#ifdef YQST_ASYNC
typedef struct quest_async_api {
    bool (*open)(yqst_async*, const yqst_async_desc*);
    void (*close)(yqst_async*);
    int (*wait)(yqst_async*, uint32_t, int64_t, yqst_snapshot*);
} quest_async_api;

static const quest_async_api g_quest_async_api = {
    yqst_async_open, yqst_async_close, yqst_async_wait
};
#endif

int main(void) {
    static yqst_quest q;
    double w[3];
    yqst_axis ax = yqst_linspace(-1.0, 1.0, 3);
    if (g_quest_api.prior_normal(&ax, 0.0, 1.0, w) < 0) return 1;
    g_quest_api.close(&q);
    if (g_quest_api.open(&q, NULL)) return 2;
    if (g_quest_api.open_saved(&q, NULL, NULL, 0)) return 3;
    if (g_quest_api.save(&q, NULL, 0) >= 0) return 4;
    {
        int n = 0;
        if (!g_quest_api.params(&n) || n < 1) return 8;
    }
    g_quest_api.close(&q);
#ifdef YQST_ASYNC
    {
        static yqst_async a;
        if (g_quest_async_api.open(&a, NULL)) return 5;
        g_quest_async_api.close(&a);
        g_quest_async_api.close(&a);
        if (g_quest_async_api.wait(&a, 1, YRT_MS(1), NULL) >= 0) return 6;
    }
#endif
    return g_quest_api.error(&q)[0] != '\0' ? 0 : 7;
}
