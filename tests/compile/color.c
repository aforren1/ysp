/* Compile check: ysp/color.h as a C translation unit with the implementation
 * enabled. CMake and CI build this as C11 with warnings as errors;
 * color.cpp builds the same source as C++17. The header has no threads,
 * so there is no YCOL_NO_THREADS variant. It runs and returns 0 to prove it
 * links. */
#define YSP_COLOR_IMPLEMENTATION
#include "ysp/color.h"

#include <string.h>

/* The context's lifecycle, by type, so a changed signature is a compile
 * error here rather than a surprise in a caller. */
typedef struct ctx_api {
    bool (*open)(ycol_ctx*, const ycol_ctx_desc*);
    void (*close)(ycol_ctx*);
    const char* (*error)(const ycol_ctx*);
    int  (*set_background)(ycol_ctx*, const double*);
    int  (*describe)(const ycol_ctx*, char*, size_t);
} ctx_api;

static const ctx_api g_ctx_api = {
    ycol_ctx_open, ycol_ctx_close, ycol_ctx_error, ycol_ctx_set_background, ycol_ctx_describe
};

int main(void) {
    ycol_ctx cx;
    memset(&cx, 0, sizeof cx);
    g_ctx_api.close(&cx);   /* safe on a zeroed context */
    {   /* v0.3.0: values, after offset; set on the enum rows only */
        int n = 0, i, k = 0;
        const ycol_param* t = ycol_desc_params(&n);
        for (i = 0; t && i < n; i++) k += (t[i].values != NULL) == !strcmp(t[i].type, "enum");
        if (!t || k != n) return 2;
    }
    return ycol_version()[0] == '0' && g_ctx_api.error(&cx)[0] == '\0' ? 0 : 1;
}
