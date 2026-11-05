/* video_check.c - the GPU paths against the upload path, on real hardware:
 * one MP4 drawn through UPLOAD (planes uploaded) and through YVID_PATH_GPU
 * (DXVA on the screen's device, one GPU copy a frame), frame by frame in manual
 * mode, each read back with ygfx_read_scene(). It checks that each frame
 * shows its own index (the 16 bars of tests/media/make_video_clips.sh) and
 * that the two paths draw the same values.
 *
 * --upload-only draws through UPLOAD alone: the check of the bars on a real GL
 * context where no GPU path exists (Linux).
 *
 * --raw FILE checks the index instead, with no window: FILE is tight NV12 from
 * another decoder (ffmpeg -hwaccel vaapi ... -pix_fmt nv12 -f rawvideo), and each
 * of its frames must have the index's hash. A hardware decoder shares no code
 * with the software decoder that made the index, so equal hashes are evidence
 * about the bytes that a clip decoded by the same library cannot give.
 *
 * Usage: video_check --file CLIP.mp4 [--frames N] [--ahead N] [--upload-only] [--raw FILE]
 *   The clip needs its .yspvi beside it and the bars (the script's c_*
 *   clips). The window is the clip's size; the screen opens with
 *   desc.d3d11_video. It needs a GPU and a window, so it is not a ctest.
 * Exit code: 0 when every frame matched; 1 on a mismatch or a failure to
 * open; 2 for a bad argument; 3 when the GPU path is not available here
 * (printed: the renderer or the screen cannot).
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef YVID_NO_PL_MPEG
#define YVID_NO_PL_MPEG
#endif
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static yscr_screen scr;
static ygfx_gfx gfx;
static yvid_movie mv[2];

static int bars(const float* row, int w) {
    int k, v = 0;
    for (k = 0; k < 16; k++) {
        int x = (k * w) / 16 + w / 32;
        if (row[(size_t)x * 4] > 0.5f) v |= 1 << k;
    }
    return v;
}

/* Every frame of a tight NV12 file against the index beside the clip. */
static int check_raw(const char* file, const char* raw) {
    yvid_desc q;
    yvid__index ix;
    char err[300];
    FILE* fh;
    size_t fb;
    uint8_t* b;
    int64_t n = 0, bad_n = 0, first_bad = -1;
    memset(&q, 0, sizeof q);
    q.path = file;
    if (yvid__index_load(&q, &ix, 1, err, sizeof err) < 0) { fprintf(stderr, "video_check: %s\n", err); return 1; }
    fb = (size_t)ix.c.w * (size_t)ix.c.h + (size_t)((ix.c.w + 1) / 2) * 2 * (size_t)((ix.c.h + 1) / 2);
    b = (uint8_t*)malloc(fb);
    fh = fopen(raw, "rb");
    if (!b || !fh) { fprintf(stderr, "video_check: cannot read %s\n", raw); return 1; }
    while (fread(b, 1, fb, fh) == fb) {
        if (n >= ix.c.frames || yvid_xxh64(b, fb, 0) != ix.hashes[n]) {
            if (first_bad < 0) first_bad = n;
            bad_n++;
        }
        n++;
    }
    fclose(fh);
    free(b);
    yvid__free(ix.hashes);
    printf("video_check: %s against %s: %lld frames of %lld, %lld differ (first %lld)\n", raw, file, (long long)n, (long long)ix.c.frames, (long long)bad_n, (long long)first_bad);
    return bad_n || n != ix.c.frames ? 1 : 0;
}

int main(int argc, char** argv) {
    const char* file = NULL;
    const char* raw = NULL;
    int frames = 60, ahead = 0, i, k, w, h, bad = 0, done = 0, upload_only = 0, paths = 2;
    yscr_desc sd;
    ygfx_desc gd;
    yvid_desc vd;
    yvid_info info;
    ygfx_stim st[2];
    yvid_stim_desc sdsc;
    float* img[2];
    double worst = 0.0;
    int worst_x = -1, worst_y = -1;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ahead") && i + 1 < argc) ahead = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--upload-only")) upload_only = 1;
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw = argv[++i];
        else { fprintf(stderr, "usage: video_check --file CLIP.mp4 [--frames N] [--ahead N] [--upload-only] [--raw FILE]\n"); return 2; }
    }
    if (!file || frames <= 0 || ahead < 0) { fprintf(stderr, "usage: video_check --file CLIP.mp4 [--frames N] [--ahead N] [--upload-only] [--raw FILE]\n"); return 2; }
    if (raw) return check_raw(file, raw);
    if (upload_only) paths = 1;
    {
        yvid_desc q;
        char err[300];
        memset(&q, 0, sizeof q);
        q.path = file;
        if (yvid_probe(&q, &info, err, sizeof err) < 0) { fprintf(stderr, "video_check: %s\n", err); return 1; }
    }
    w = info.w; h = info.h;
    memset(&sd, 0, sizeof sd);
    sd.windowed = true;
    sd.window_w = w; sd.window_h = h;
    sd.d3d11_video = !upload_only;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "video_check: %s\n", yscr_error(&scr)); return 3; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "video_check: %s\n", ygfx_error(&gfx)); return 1; }
    for (k = 0; k < paths; k++) {
        memset(&vd, 0, sizeof vd);
        vd.path = file;
        vd.light = YVID_LIGHT_CODES;
        vd.gpu_path = k ? YVID_PATH_GPU : YVID_PATH_UPLOAD;
        vd.hw_decode = upload_only ? YVID_HW_AUTO : YVID_HW_DXVA;
        vd.ahead = ahead;
        if (!yvid_open(&mv[k], &gfx, &vd)) {
            fprintf(stderr, "video_check: %s: %s\n", k ? "GPU" : "UPLOAD", yvid_error(&mv[k]));
            return k ? 3 : 1;
        }
        memset(&sdsc, 0, sizeof sdsc);
        st[k] = yvid_stim(&mv[k], &sdsc);
    }
    img[0] = (float*)malloc((size_t)w * h * 4 * sizeof(float));
    img[1] = (float*)malloc((size_t)w * h * 4 * sizeof(float));
    if (!img[0] || !img[1]) return 1;
    /* frames 0, 1, ... then a jump back and a few across GOPs */
    for (i = 0; i < frames && done < frames; i++) {
        int64_t want = i < frames / 2 ? i : (int64_t)((i * 37) % (int)info.frames);
        int got[2] = { 0, upload_only }, tries;
        for (k = 0; k < paths; k++) yvid_show(&mv[k], want);
        for (tries = 0; tries < 120 && !(got[0] && got[1]); tries++) {
            yscr_frame f;
            int which;
            yvid_record r;
            if (yscr_begin(&scr, &f) != YSCR_OK) { fprintf(stderr, "video_check: the screen closed\n"); return 1; }
            for (k = 0; k < paths; k++)
                if (yvid_update(&mv[k], &f) < 0) { fprintf(stderr, "video_check: %s: %s\n", k ? "GPU" : "UPLOAD", yvid_error(&mv[k])); return 1; }
            /* draw the path that has the frame and has not been read yet */
            which = -1;
            for (k = 0; k < paths && which < 0; k++)
                if (!got[k] && yvid_last(&mv[k], &r) == YVID_OK && r.display == f.index && r.frame == want) which = k;
            ygfx_begin(&gfx, &f);
            if (which >= 0) ygfx_draw(&gfx, &st[which]);
            ygfx_end(&gfx);
            if (which >= 0) {
                ygfx_read_scene(&gfx, 0, 0, w, h, img[which]);
                got[which] = 1;
            }
            yscr_flip(&scr);
        }
        if (!(got[0] && got[1])) { fprintf(stderr, "video_check: frame %lld never shown\n", (long long)want); return 1; }
        for (k = 0; k < paths; k++) {
            int b = bars(img[k] + (size_t)32 * w * 4, w);
            if (b != (int)(want & 0xffff)) { fprintf(stderr, "video_check: %s frame %lld shows %d\n", k ? "GPU" : "UPLOAD", (long long)want, b); bad++; }
        }
        if (!upload_only) {
            size_t p, n = (size_t)w * h * 4;
            for (p = 0; p < n; p++) {
                double d = fabs((double)img[0][p] - (double)img[1][p]);
                if (d > worst) { worst = d; worst_x = (int)((p / 4) % (size_t)w); worst_y = (int)((p / 4) / (size_t)w); }
            }
        }
        done++;
    }
    if (upload_only) printf("video_check: %s, %d x %d, %d frames through UPLOAD: bars %s\n", file, w, h, done, bad ? "WRONG" : "right");
    else printf("video_check: %s, %d x %d, %d frames: bars %s; GPU against UPLOAD, worst difference %.3g (pixel %d, %d)\n",
                file, w, h, done, bad ? "WRONG" : "right", worst, worst_x, worst_y);
    {
        char line[1024];
        yvid_describe(&mv[paths - 1], line, sizeof line);
        printf("%s\n", line);
    }
    for (k = 0; k < paths; k++) yvid_close(&mv[k]);
    ygfx_close(&gfx);
    yscr_close(&scr);
    free(img[0]); free(img[1]);
    return bad || worst > 1e-6 ? 1 : 0;
}
