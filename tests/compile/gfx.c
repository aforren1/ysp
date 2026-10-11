/* Compile check: ysp/gfx.h as a C translation unit with the implementation
 * enabled. CMake and CI build this as C11 with warnings as errors when SDL3
 * is found; gfx.cpp builds the same source as C++17. It runs three
 * frames on the simulated display, where ysp/gfx.h opens its null backend,
 * so it passes on a machine with no display and no GPU. The frames hold a
 * group member and a target pass, so v0.2's calls link and run too, and
 * a user shader that samples the target (v0.10.4), refused inside its pass,
 * and an sRGB-coded gray texture as a pack stores one (v0.10.5), and a
 * capture of every other frame and a marked one, written by the pump on
 * this thread (v0.12). */
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"

/* v0.11.1: the gfx starts in the screen's hook; ygfx_open() finishes it */
typedef struct start_ctx { ygfx_gfx* g; ygfx_desc* d; int ok; } start_ctx;
static void start_gfx(void* ctx, yscr_screen* s) {
    start_ctx* c = (start_ctx*)ctx;
    c->d->screen = s;
    c->ok = ygfx_open_start(c->g, c->d);
}

/* v0.12: the writer hands each captured frame here */
static void on_frame(void* ctx, const ygfx_capture_frame* fr) {
    if (fr->rgba && fr->w == 64) (*(int*)ctx)++;
}

int main(void) {
    yscr_screen s;
    yscr_desc d;
    ygfx_gfx g;
    ygfx_desc gd;
    ygfx_gabor_desc gab;
    ygfx_stim st;
    yscr_frame f;
    ygfx_group_desc grd;
    ygfx_group grp;
    ygfx_target_desc td;
    ygfx_tex t;
    ygfx_pipeline_desc pd;
    ygfx_user_desc ud;
    ygfx_stim us;
    ygfx_texture_desc xd;
    ygfx_tex gray;
    static const float zero[4] = { 0, 0, 0, 0 };
    static const uint8_t mask[4] = { 0, 255, 255, 0 };
    float sdf[16];
    start_ctx sc;
    char line[700];
    int i, n = 0, m = 0, k = 0, got = 0;
    ygfx_capture_desc cd;
    ygfx_capture_stats cs;
    memset(&s, 0, sizeof s);
    memset(&d, 0, sizeof d);
    memset(&g, 0, sizeof g);
    memset(&gd, 0, sizeof gd);
    memset(&gab, 0, sizeof gab);
    memset(&grd, 0, sizeof grd);
    memset(&td, 0, sizeof td);
    memset(&pd, 0, sizeof pd);
    memset(&ud, 0, sizeof ud);
    memset(&xd, 0, sizeof xd);
    d.backend = YSCR_BACKEND_SIM;
    d.sim_period_ns = 2000000;
    sc.g = &g; sc.d = &gd; sc.ok = 0;
    d.on_context = start_gfx;
    d.on_context_ctx = &sc;
    if (!yscr_open(&s, &d)) return 1;
    if (!sc.ok || ygfx_is_open(&g)) return 18;
    if (!ygfx_open(&g, &gd)) return 2;
    ygfx_describe(&g, line, sizeof line);   /* the null backend: no parallel compile */
    if (!strstr(line, "open_start=serial(")) return 19;
    gab.sf = 1.0f / 32.0f;
    gab.sigma = 32.0f;
    gab.group = &grp;
    grd.scale = 2.0f;
    grp = ygfx_group_make(&grd);
    st = ygfx_gabor(&gab);
    td.w = 64; td.h = 64;
    t = ygfx_target(&g, &td);
    if (t.id == 0) return 9;
    pd.body = "float ysp_main(vec2 p) { return texelFetch(ysp_tex0, ivec2(p + ysp_size.xy), 0).r; }\n";
    ud.pipe = ygfx_pipeline(&g, &pd);
    ud.w = ud.h = 64;
    ud.tex = t;
    us = ygfx_user(&ud);
    if (ud.pipe.id == 0 || us.tex.id != t.id) return 13;
    xd.w = 2; xd.h = 2; xd.format = YGFX_R8; xd.data = mask;
    xd.enc.matrix = YGFX_MATRIX_RGB; xd.enc.range = YGFX_RANGE_FULL; xd.enc.transfer = YGFX_TRC_SRGB;
    xd.enc.primaries = YGFX_PRIM_DEVICE;
    gray = ygfx_texture(&g, &xd);
    if (gray.id == 0) return 16;
    if (ygfx_prime(&g) < 1) return 17;   /* v0.11: every program, once */
    ygfx_texture_free(&g, gray);
    memset(&cd, 0, sizeof cd);
    cd.every = 2; cd.w = 64; cd.h = 32; cd.manual = true;
    cd.on_frame = on_frame; cd.ctx = &got;
    if (ygfx_capture_start(&g, &cd) != YGFX_OK) return 20;
    for (i = 0; i < 3; i++) {
        if (yscr_begin(&s, &f) != YSCR_OK) return 3;
        if (ygfx_begin(&g, &f) != YGFX_OK) return 4;
        if (ygfx_begin_target(&g, t, zero) != YGFX_OK) return 10;
        if (ygfx_draw(&g, &st) != YGFX_OK) return 5;
        if (ygfx_draw(&g, &us) != YGFX_ERR_ORDER) return 14;
        if (ygfx_end_target(&g) != YGFX_OK) return 11;
        if (ygfx_draw(&g, &us) != YGFX_OK) return 15;
        if (ygfx_draw(&g, &st) != YGFX_OK) return 5;
        if (i == 1 && ygfx_capture_mark(&g) != YGFX_OK) return 21;
        if (ygfx_end(&g) != YGFX_OK) return 6;
        if (yscr_flip(&s) != YSCR_OK) return 7;
    }
    ygfx_capture_pump(&g, 0);
    if (ygfx_capture_stop(&g, &cs) != 3 || got != 3 || cs.chosen != 3) return 22;   /* frames 0, 1 (marked), 2 */
    if (ygfx_image_write(NULL, mask, 1, 1, 0, NULL, NULL, 0) != YGFX_ERR_ARG) return 23;
    ygfx_texture_free(&g, t);
    ygfx_close(&g);
    yscr_close(&s);
    if (ygfx_sdf_from_mask(sdf, mask, 2, 2, 1) != YGFX_OK) return 12;
    return ygfx_params(&n) && n == YGFX_P_COUNT && ygfx_desc_params(&m) && m > 0 &&
           ygfx_group_params(&k) && k > 0 ? 0 : 8;
}
