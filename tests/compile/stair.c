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
    void (*close)(yst_stair*);
    const char* (*error)(const yst_stair*);
    int (*update)(yst_stair*, double, int);
} stair_api;

static const stair_api g_stair_api = { yst_open, yst_close, yst_error, yst_update };

int main(void) {
    static yst_stair s;
    yst_desc d;
    memset(&d, 0, sizeof(d));   /* g++ -Wextra flags = { 0 } in C++ */
    d.start = 0.5;
    d.steps[0] = 0.1;
    d.n_steps = 1;
    d.stop_trials = 4;
    if (!g_stair_api.open(&s, &d)) return 1;
    if (g_stair_api.update(&s, yst_next(&s), 1) < 0) return 2;
    g_stair_api.close(&s);
    g_stair_api.close(&s);
    return g_stair_api.error(&s)[0] == '\0' ? 0 : 3;
}
