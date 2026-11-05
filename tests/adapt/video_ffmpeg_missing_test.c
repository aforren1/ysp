/* video_ffmpeg_missing_test.c - ysp/video.h built with YVID_FFMPEG on a machine
 * without FFmpeg's libraries: the sonames are the test's, so dlopen() fails.
 * The program must start (nothing is linked), a movie must be refused with the
 * code and a message that names the library, an index must be refused the same
 * way, and a frame sequence must still play: the reason to load the libraries
 * at run time rather than link them (docs/video.md, "Linux"). */
#define YVID__FF_SO_UTIL "libysp_no_such_avutil.so.1"
#define YVID__FF_SO_CODEC "libysp_no_such_avcodec.so.1"
#define YVID__FF_SO_FORMAT "libysp_no_such_avformat.so.1"
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"

#include <stdio.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

static yscr_screen scr;
static ygfx_gfx gfx;
static yvid_movie mv;

int main(void) {
    static const char* const clip = "ysp_video_ff_missing.mp4";
    static const char* const seqp = "ysp_video_ff_missing.yspseq";
    uint8_t file[256];
    uint8_t idx[YVID__IDX_HDR + 8];
    yvid__canon c;
    yscr_desc sd;
    ygfx_desc gd;
    yvid_desc vd;
    char err[512];
    FILE* fh;
    uint64_t head, tail;
    int64_t n;
    memset(file, 0, sizeof file);
    file[3] = 32;
    memcpy(file + 4, "ftypisom", 8);
    fh = fopen(clip, "wb");
    CHECK(fh != NULL);
    if (!fh) return 1;
    fwrite(file, 1, sizeof file, fh);
    fclose(fh);
    head = tail = yvid_xxh64(file, sizeof file, 0);
    memset(&sd, 0, sizeof sd);
    sd.backend = YSCR_BACKEND_SIM;
    CHECK(yscr_open(&scr, &sd));
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    CHECK(ygfx_open(&gfx, &gd));

    /* the index maker */
    err[0] = 0;
    n = yvid_index_make(clip, "ysp_video_ff_missing.yspvi", NULL, err, sizeof err);
    CHECK(n == YVID_ERR_NOT_IMPLEMENTED);
    CHECK(strstr(err, "libysp_no_such_avutil.so.1") != NULL);

    /* a movie with a good index */
    memset(&c, 0, sizeof c);
    c.codec = YVID_CODEC_H264; c.w = 16; c.h = 16; c.format = YVID_FMT_NV12;
    c.fps_num = 30; c.fps_den = 1; c.frames = 1; c.gop = 1;
    c.matrix = YVID_MATRIX_BT709; c.range = YVID_RANGE_LIMITED; c.transfer = YVID_TRC_BT1886;
    c.primaries = YVID_PRIM_BT709; c.siting = YVID_SITING_LEFT;
    yvid__index_header(idx, &c, (int64_t)sizeof file, head, tail);
    memset(idx + YVID__IDX_HDR, 0, 8);
    memset(&vd, 0, sizeof vd);
    vd.path = clip;
    vd.index = idx;
    vd.index_size = sizeof idx;
    CHECK(!yvid_open(&mv, &gfx, &vd));
    CHECK(mv.error_code == YVID_ERR_NOT_IMPLEMENTED);
    CHECK(strstr(yvid_error(&mv), "did not load") != NULL);
    CHECK(strstr(yvid_error(&mv), "libysp_no_such_avutil.so.1") != NULL);

    /* a frame sequence is none of FFmpeg's business */
    {
        yvid_seq w;
        yvid_seq_desc sq;
        static uint8_t px[16 * 16 * 4];
        const void* planes[3];
        int32_t strides[3] = { 16 * 4, 0, 0 };
        yscr_frame f;
        int k, rc = YVID_OK;
        memset(&w, 0, sizeof w);
        memset(&sq, 0, sizeof sq);
        sq.path = seqp; sq.w = 16; sq.h = 16; sq.format = YVID_FMT_RGBA8; sq.fps_num = 30; sq.fps_den = 1;
        CHECK(yvid_seq_open(&w, &sq));
        planes[0] = px; planes[1] = NULL; planes[2] = NULL;
        for (k = 0; k < 4; k++) CHECK(yvid_seq_write(&w, planes, strides, NULL) == YVID_OK);
        CHECK(yvid_seq_finish(&w) == YVID_OK);
        yvid_seq_close(&w);
        memset(&vd, 0, sizeof vd);
        vd.path = seqp;
        vd.inline_decode = true;
        CHECK(yvid_open(&mv, &gfx, &vd));
        CHECK(mv.info.backend == YVID_BACKEND_SEQ);
        CHECK(yvid_play_at(&mv, YVID_ASAP) > 0);
        for (k = 0; k < 20 && rc == YVID_OK; k++) {
            if (yscr_begin(&scr, &f) != YSCR_OK) break;
            rc = yvid_update(&mv, &f);
            ygfx_begin(&gfx, &f); ygfx_end(&gfx);
            yscr_flip(&scr);
        }
        CHECK(rc >= 0);
        yvid_close(&mv);
    }
    ygfx_close(&gfx);
    yscr_close(&scr);
    remove(clip);
    remove(seqp);
    if (g_fail) { fprintf(stderr, "video_ffmpeg_missing_test: %d failure(s)\n", g_fail); return 1; }
    printf("video_ffmpeg_missing_test: all checks passed\n");
    return 0;
}
