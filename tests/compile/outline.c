/* Compile check: ysp/outline.h as a C translation unit with the
 * implementation enabled. CMake and CI build this as C11 with warnings as
 * errors; outline.cpp builds the same source as C++17. The header has no
 * threads, so there is no YOL_NO_THREADS variant. It runs and returns 0 to
 * prove it links. */
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"

#include <string.h>

/* The lifecycle and the calls that report on a context, by type, so a
 * changed signature is a compile error here rather than a surprise in a
 * caller. */
typedef struct outline_api {
    bool (*open)(yol_ctx*, const yol_ctx_desc*);
    void (*close)(yol_ctx*);
    const char* (*error)(const yol_ctx*);
    void (*path_init)(yol_path*, yol_ctx*);
    void (*path_free)(yol_path*);
    void (*close_path)(yol_path*);
    int  (*font_view)(yol_font*, const void*, size_t, int, char*, size_t);
    bool (*cset_open)(yol_cset*, yol_ctx*, const yol_cset_desc*);
    void (*cset_close)(yol_cset*);
    int  (*cset_add)(yol_cset*, uint32_t, const yol_path*);
    int  (*svg)(yol_ctx*, const char*, size_t, const yol_svg_desc*, double*, char*, size_t);
} outline_api;

static const outline_api g_api = {
    yol_open, yol_close, yol_error, yol_path_init, yol_path_free, yol_close_path,
    yol_font_view, yol_cset_open, yol_cset_close, yol_cset_add, yol_svg
};

int main(void) {
    yol_ctx c;
    yol_cset s;
    memset(&c, 0, sizeof c);
    memset(&s, 0, sizeof s);
    g_api.cset_close(&s);   /* safe on zeroed handles */
    g_api.close(&c);
    return yol_version()[0] == '0' && g_api.error(&c)[0] == '\0' ? 0 : 1;
}
