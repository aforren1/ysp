/* Compile check: ysp/video.h as a C translation unit with the implementation
 * enabled. CMake and CI build this as C11 with warnings as errors when SDL3
 * is found; video.cpp builds the same source as C++17. It writes a small
 * frame sequence beside the program, plays it for a few frames on the
 * simulated display (ysp/gfx.h's null backend) and removes it, so it passes
 * on a machine with no display and no GPU. */
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"

#include <stdio.h>

/* v0.3.0: the writer's lifecycle and the probe, by type, so a changed
 * signature is a compile error here rather than a surprise in a caller. */
typedef struct video_api {
    bool (*seq_open)(yvid_seq*, const yvid_seq_desc*);
    int  (*seq_write)(yvid_seq*, const void* const*, const int32_t*, const yvid_seq_meta*);
    int  (*seq_finish)(yvid_seq*);
    void (*seq_close)(yvid_seq*);
    const char* (*seq_error)(const yvid_seq*);
    int  (*probe)(const yvid_desc*, yvid_info*, char*, size_t);
    int64_t (*play_at)(yvid_movie*, int64_t);
    int64_t (*seek)(yvid_movie*, int64_t, int64_t);
} video_api;

static const video_api g_api = {
    yvid_seq_open, yvid_seq_write, yvid_seq_finish, yvid_seq_close, yvid_seq_error, yvid_probe,
    yvid_play_at, yvid_seek
};

int main(void) {
    yscr_screen s;
    yscr_desc d;
    ygfx_gfx g;
    ygfx_desc gd;
    yvid_seq w;
    yvid_seq_desc sd;
    yvid_movie mv;
    yvid_desc vd;
    yvid_stim_desc st;
    ygfx_stim film;
    yscr_frame f;
    static uint8_t px[16 * 8 * 4];
    const void* planes[3];
    int32_t strides[3] = { 16 * 4, 0, 0 };
    /* One file per language: the C and C++ checks share a working
     * directory and run side by side under ctest -j. */
#ifdef __cplusplus
    const char* path = "ysp_video_compile_cxx.yspseq";
#else
    const char* path = "ysp_video_compile.yspseq";
#endif
    char line[1024];
    int i, n = 0, rc = 0;
    memset(&s, 0, sizeof s);
    memset(&d, 0, sizeof d);
    memset(&g, 0, sizeof g);
    memset(&gd, 0, sizeof gd);
    memset(&w, 0, sizeof w);
    memset(&sd, 0, sizeof sd);
    memset(&mv, 0, sizeof mv);
    memset(&vd, 0, sizeof vd);
    memset(&st, 0, sizeof st);
    sd.path = path; sd.w = 16; sd.h = 8; sd.format = YVID_FMT_RGBA8;
    sd.compression = YVID_SEQ_QOI; sd.fps_num = 30; sd.fps_den = 1;
    if (!yvid_seq_open(&w, &sd)) return 1;
    planes[0] = px; planes[1] = NULL; planes[2] = NULL;
    for (i = 0; i < 4; i++) {
        memset(px, i * 40, sizeof px);
        if (yvid_seq_write(&w, planes, strides, NULL) != YVID_OK) return 2;
    }
    if (yvid_seq_finish(&w) != YVID_OK) return 3;
    g_api.seq_close(&w);   /* v0.3: void, after finish */
    yvid_seq_close(&w);
    d.backend = YSCR_BACKEND_SIM;
    d.sim_period_ns = 2000000;
    if (!yscr_open(&s, &d)) return 4;
    gd.screen = &s;
    if (!ygfx_open(&g, &gd)) return 5;
    vd.path = path;
    vd.ahead = 2;
    if (!yvid_open(&mv, &g, &vd)) { fprintf(stderr, "%s\n", yvid_error(&mv)); return 6; }
    film = yvid_stim(&mv, &st);
    if (yvid_play_at(&mv, YVID_ASAP) <= 0) return 7;
    for (i = 0; i < 400 && rc == YVID_OK; i++) {
        if (yscr_begin(&s, &f) != YSCR_OK) return 8;
        rc = yvid_update(&mv, &f);
        if (ygfx_begin(&g, &f) != YGFX_OK) return 9;
        if (ygfx_draw(&g, &film) != YGFX_OK) return 10;
        if (ygfx_end(&g) != YGFX_OK) return 11;
        if (yscr_flip(&s) != YSCR_OK) return 12;
    }
    if (rc != YVID_ENDED) { fprintf(stderr, "update: %d %s\n", rc, yvid_error(&mv)); return 13; }
    yvid_describe(&mv, line, sizeof line);
    yvid_close(&mv);
    ygfx_close(&g);
    yscr_close(&s);
    remove(path);
    return yvid_params(&n) && n > 0 && yvid_xxh64("", 0, 0) == UINT64_C(0xEF46DB3751D8E999) ? 0 : 14;
}
