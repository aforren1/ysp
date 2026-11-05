/* Compile check: ysp/rdk.h as a C translation unit with the implementation
 * enabled. CMake and CI build this as C11 with warnings as errors;
 * rdk.cpp builds the same source as C++17. The header has no threads,
 * so there is no YRDK_NO_THREADS variant. It runs and returns 0 to prove
 * it links and that a field opens, starts and moves. */
#define YSP_RDK_IMPLEMENTATION
#include "ysp/rdk.h"

#include <string.h>

/* The lifecycle and the calls v0.2.0 changed, by type. */
typedef struct rdk_api {
    bool (*open)(yrdk_field*, const yrdk_desc*);
    void (*close)(yrdk_field*);
    const char* (*error)(const yrdk_field*);
    int (*xy)(yrdk_field*, float*);
    int (*write)(yrdk_field*, const yrdk_out*);
    int (*snapshot)(yrdk_field*, void*, size_t);
} rdk_api;

static const rdk_api g_api = { yrdk_open, yrdk_close, yrdk_error, yrdk_xy, yrdk_write, yrdk_snapshot };

/* The unit macros are constant expressions. */
static const int64_t g_life = YRT_MS(250);

int main(void) {
    static yrdk_field f;
    static float xy[2 * 50], age[50];
    yrdk_desc d;
    yrdk_out o;
    memset(&d, 0, sizeof d);
    memset(&o, 0, sizeof o);
    d.w = 10;
    d.count = 50;
    d.coherence = 0.5f;
    d.speed = 2;
    d.lifetime_ns = g_life;
    if (!g_api.open(&f, &d) || g_api.error(&f)[0] != 0) return 1;
    if (yrdk_start(&f, 1, 0) < 0) return 1;
    if (yrdk_update(&f, YRT_NS(16666667)) != 50) return 1;
    if (g_api.xy(&f, xy) != 50) return 1;
    o.age_s = age;                       /* seconds: a GPU attribute */
    if (g_api.write(&f, &o) != 50 || !(age[0] >= 0.0f)) return 1;
    g_api.close(&f);
    g_api.close(&f);
    {   /* v0.3.0: values, after offset; no enum row, so NULL in each */
        int n = 0;
        const yrdk_param* t = yrdk_params(&n);
        if (!t || n != 3 || t[0].values || t[1].values || t[2].values) return 1;
    }
    return yrdk_version()[0] == '0' ? 0 : 1;
}
