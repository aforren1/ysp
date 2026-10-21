/* Compile check: ysp/aep.h as a C translation unit with the implementation
 * enabled. CMake and CI build this as C11 with warnings as errors; aep.cpp
 * builds the same source as C++17, and the _async variant defines YAEP_ASYNC.
 * It runs and returns 0 to prove it links, which for this header also means
 * libm resolved. The tables name the lifecycle by type, so a signature change
 * fails here and not in a user's build. */
#define YSP_AEP_IMPLEMENTATION
#include "ysp/aep.h"

typedef struct aep_api {
    bool (*open)(yaep_gp*, const yaep_desc*);
    bool (*open_saved)(yaep_gp*, const yaep_desc*, const void*, size_t);
    void (*close)(yaep_gp*);
    const char* (*error)(const yaep_gp*);
    int (*save)(const yaep_gp*, void*, size_t);
    const yaep_param* (*params)(int*);
} aep_api;

static const aep_api g_aep_api = {
    yaep_open, yaep_open_saved, yaep_close, yaep_error, yaep_save, yaep_params
};

#ifdef YAEP_ASYNC
typedef struct aep_async_api {
    bool (*open)(yaep_async*, const yaep_async_desc*);
    void (*close)(yaep_async*);
    int (*wait)(yaep_async*, uint32_t, int64_t, yaep_snapshot*);
} aep_async_api;

static const aep_async_api g_aep_async_api = {
    yaep_async_open, yaep_async_close, yaep_async_wait
};
#endif

int main(void) {
    static yaep_gp g;
    g_aep_api.close(&g);
    if (g_aep_api.open(&g, NULL)) return 1;
    if (g_aep_api.open_saved(&g, NULL, NULL, 0)) return 2;
    if (g_aep_api.save(&g, NULL, 0) >= 0) return 3;
    if (g_aep_api.error(&g)[0] == '\0') return 4;
    {
        int n = 0;
        if (!g_aep_api.params(&n) || n < 1) return 7;
    }
    g_aep_api.close(&g);
#ifdef YAEP_ASYNC
    {
        static yaep_async a;
        if (g_aep_async_api.open(&a, NULL)) return 5;
        g_aep_async_api.close(&a);
        g_aep_async_api.close(&a);
        if (g_aep_async_api.wait(&a, 1, YRT_MS(1), NULL) >= 0) return 6;
    }
#endif
    return 0;
}
