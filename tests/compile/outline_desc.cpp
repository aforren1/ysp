/* Compile check: ysp/outline.h's desc structs set by designated
 * initializers, as the manual's USAGE shows them, in C++20 (the first C++
 * with them, and only in declaration order). CMake builds this one target as
 * C++20, and with -Wno-missing-field-initializers on g++, which warns about
 * every field a designated initializer leaves out even though zero is the
 * default. Built as an earlier C++, it compiles to an empty program. */
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"

#if __cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
int main() {
    yol_ctx cx;
    yol_path p, o;
    yol_box b;
    yol_cset s;
    int ok = yol_open(&cx, nullptr);
    yol_path_init(&p, &cx);
    yol_path_init(&o, &cx);
    yol_rect(&p, 0, 0, 1, 1, 0.1, 0.1);
    ok = ok && yol_path_end(&p) == YOL_OK;
    yol_stroke_desc sd = { .width = 0.1, .join = YOL_JOIN_MITER, .mode = YOL_BOLD };
    ok = ok && yol_stroke(&cx, &p, &o, &sd) == YOL_OK;
    yol_raster_desc rd = { .scale = 16, .x = 1, .y = 1 };
    ok = ok && yol_raster(&cx, &o, &rd, &b) == YOL_OK && b.x1 > b.x0;
    yol_cset_desc cd = { .n_glyphs = 1, .nh = 2, .nv = 2 };
    ok = ok && yol_cset_open(&s, &cx, &cd) && yol_cset_add(&s, 0, &o) == YOL_OK;
    yol_glyph_desc gd = { .size = 24, .y_up = true };
    yol_svg_desc svd = { .tol = 0.01, .max_layers = 0 };
    (void)gd; (void)svd;
    yol_cset_close(&s);
    yol_path_free(&p);
    yol_path_free(&o);
    yol_close(&cx);
    return ok ? 0 : 1;
}
#else
int main() { return 0; }
#endif
