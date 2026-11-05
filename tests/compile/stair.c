/* Compile check: ysp/stair.h as a C translation unit with the implementation
 * enabled. CMake and CI build this as C11 with warnings as errors;
 * stair.cpp builds the same source as C++17. The header has no threads,
 * so there is no YST_NO_THREADS variant. It runs and returns 0 to prove it
 * links. The table names the lifecycle by type, so a signature change
 * fails here and not in a user's build. */
#define YSP_STAIR_IMPLEMENTATION
#include "ysp/stair.h"

#include <string.h>

typedef struct stair_api {
    bool (*open)(yst_stair*, const yst_desc*);
    bool (*open_saved)(yst_stair*, const yst_desc*, const void*, size_t);
    void (*close)(yst_stair*);
    const char* (*error)(const yst_stair*);
    int (*update)(yst_stair*, double, int);
    size_t (*save_size)(const yst_stair*);
    int (*save)(const yst_stair*, void*, size_t);
    const yst_param* (*params)(int*);
} stair_api;

static const stair_api g_stair_api = {
    yst_open, yst_open_saved, yst_close, yst_error, yst_update,
    yst_save_size, yst_save, yst_params
};

int main(void) {
    static yst_stair s;
    static unsigned char buf[512];
    int n = 0, len;
    /* The zero-init rule (README, Conventions): { 0 } in C, {} in C++. */
#ifdef __cplusplus
    yst_desc d = {};
#else
    yst_desc d = { 0 };
#endif
    d.start = 0.5;
    d.steps[0] = 0.1;
    d.n_steps = 1;
    d.stop_trials = 4;
    if (!g_stair_api.open(&s, &d)) return 1;
    if (g_stair_api.update(&s, yst_next(&s), 1) < 0) return 2;
    if (g_stair_api.save_size(&s) > sizeof buf) return 4;
    len = g_stair_api.save(&s, buf, sizeof buf);
    if (len <= 0) return 5;
    g_stair_api.close(&s);
    if (!g_stair_api.open_saved(&s, &d, buf, (size_t)len)) return 6;
    if (!g_stair_api.params(&n) || n < 1) return 7;
    g_stair_api.close(&s);
    g_stair_api.close(&s);
    return g_stair_api.error(&s)[0] == '\0' ? 0 : 3;
}
