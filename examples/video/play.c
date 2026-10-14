/* video_play.c - play a movie in a window: ysp/video.h's USAGE example, and
 * the frame-thread cost of yvid_update() (decode-ahead, the decision and
 * the upload) per display frame.
 *
 * With no --file it writes a frame sequence of generated frames first (a
 * low-contrast gradient, a moving bar and the frame index as 16 blocks), so
 * it needs no clip. With --file it plays a frame sequence or an MPEG-1 file
 * (pl_mpeg; --index makes the .yspvi beside it first).
 *
 * Usage: video_play [--sim] [--file PATH [--index]] [--size WxH] [--frames N]
 *                   [--fps N] [--qoi] [--yuv] [--loop] [--seconds S]
 *                   [--window WxH] [--display ID] [--ahead N] [--csv PATH]
 *                   [--hw sw|dxva] [--gpu]
 *   --sim       no window and no GL: the simulated display, for CI
 *   --size      generated frames, pixels (default 320x180)
 *   --frames    generated frames (default 60)
 *   --fps       generated rate (default 30)
 *   --qoi       QOI frames (RGBA8); --yuv: I420 frames, converted on the pump
 *   --seconds   stop after S seconds (default: the movie's end)
 *   --window    window size (default 640x360)
 *   --csv       one line per display frame: index, decision, why, frame, the
 *               update's ns
 *   --hw        Media Foundation's decoder for an MP4 file (desc.hw_decode)
 *   --gpu       decode and draw on the GPU (desc.gpu_path YVID_PATH_GPU;
 *               the screen opens with desc.d3d11_video)
 * It prints yvid_update()'s cost on frames that uploaded a new frame and
 * on repeats separately, the draw and flip's cost, and the process's CPU,
 * after frame 120. Shift+Esc ends it (ysp/screen.h's default abort).
 * Exit code: 0; 1 when the screen, the gfx or the movie did not open, or a
 * frame update failed; 2 for a bad argument.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS   /* sscanf() and fopen() under /W4 /WX */
#endif
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static yscr_screen scr;
static ygfx_gfx gfx;
static yvid_movie mv;

/* The process's CPU time, ns: the decode thread and the decoder's own
 * threads count too, which a per-call timer does not see. */
static int64_t cpu_ns(void) {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    return (int64_t)((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                     (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100;
#else
    return (int64_t)((double)clock() * 1e9 / CLOCKS_PER_SEC);
#endif
}

/* A low-contrast frame: the display may be somebody's working screen. */
static void make_frame(uint8_t* rgba, int w, int h, int i, int n) {
    int x, y, bar = (int)((long long)i * w / (n > 0 ? n : 1));
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t* p = rgba + ((size_t)y * w + x) * 4;
            int v = 112 + (x * 32) / (w > 1 ? w : 1);
            int blk = x / 16;
            if (y < 16 && blk < 16) v = ((i >> blk) & 1) ? 150 : 100;
            else if (x >= bar && x < bar + 8) v = 150;
            p[0] = (uint8_t)v; p[1] = (uint8_t)v; p[2] = (uint8_t)v; p[3] = 255;
        }
    }
}

static int write_seq(const char* path, int w, int h, int n, int fps, int qoi, int yuv) {
    yvid_seq sq;
    yvid_seq_desc d;
    uint8_t* rgba = (uint8_t*)malloc((size_t)w * h * 4);
    uint8_t* yp = (uint8_t*)malloc((size_t)w * h + 2 * (size_t)((w + 1) / 2) * ((h + 1) / 2));
    int i, x, y, rc = 0;
    if (!rgba || !yp) return 1;
    memset(&d, 0, sizeof d);
    d.path = path; d.w = w; d.h = h; d.fps_num = fps; d.fps_den = 1;
    d.format = yuv ? YVID_FMT_I420 : YVID_FMT_RGBA8;
    d.compression = qoi && !yuv ? YVID_SEQ_QOI : YVID_SEQ_RAW;
    if (yuv) {
        d.matrix = YVID_MATRIX_BT709; d.range = YVID_RANGE_LIMITED; d.transfer = YVID_TRC_BT1886;
        d.primaries = YVID_PRIM_BT709; d.siting = YVID_SITING_LEFT;
    }
    d.max_frames = n;
    if (!yvid_seq_open(&sq, &d)) { fprintf(stderr, "video_play: %s\n", yvid_seq_error(&sq)); free(rgba); free(yp); return 1; }
    for (i = 0; i < n && rc == 0; i++) {
        const void* planes[3];
        int32_t strides[3];
        make_frame(rgba, w, h, i, n);
        if (yuv) {
            /* gray: Y from the code, chroma neutral */
            int cw = (w + 1) / 2, ch = (h + 1) / 2;
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) yp[(size_t)y * w + x] = (uint8_t)(16 + rgba[((size_t)y * w + x) * 4] * 219 / 255);
            memset(yp + (size_t)w * h, 128, 2 * (size_t)cw * ch);
            planes[0] = yp; planes[1] = yp + (size_t)w * h; planes[2] = yp + (size_t)w * h + (size_t)cw * ch;
            strides[0] = w; strides[1] = cw; strides[2] = cw;
        } else {
            planes[0] = rgba; planes[1] = NULL; planes[2] = NULL;
            strides[0] = w * 4; strides[1] = 0; strides[2] = 0;
        }
        if (yvid_seq_write(&sq, planes, strides, NULL) != YVID_OK) rc = 1;
    }
    if (yvid_seq_finish(&sq) != YVID_OK) { fprintf(stderr, "video_play: %s\n", yvid_seq_error(&sq)); rc = 1; }
    yvid_seq_close(&sq);
    free(rgba);
    free(yp);
    return rc;
}

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char** argv) {
    yscr_desc sd;
    ygfx_desc gd;
    yvid_desc vd;
    yvid_stim_desc st;
    ygfx_stim film;
    yscr_frame f;
    const char* file = NULL;
    const char* csv = NULL;
    const char* gen = "video_play_demo.yspseq";
    int w = 320, h = 180, n = 60, fps = 30, qoi = 0, yuv = 0, loop = 0, index = 0, ahead = 0;
    int ww = 640, wh = 360, i, rc = YVID_OK, failed = 0, hw = 0, gpu = 0;
    unsigned display = 0;
    double seconds = 0;
    int64_t* cost = NULL;
    int64_t* rcost = NULL;   /* repeats */
    int64_t* dcost = NULL;   /* draw + flip, every display frame */
    int64_t n_dcost = 0, cap_dcost = 0, cpu0 = 0, wall0 = 0;
    int64_t n_cost = 0, cap_cost = 0, n_rcost = 0, cap_rcost = 0, t_start = 0;
    uint64_t heap0 = 0;
    FILE* out = NULL;
    char line[1024];

    memset(&sd, 0, sizeof sd);
    sd.windowed = true;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim")) sd.backend = YSCR_BACKEND_SIM;
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "--index")) index = 1;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc && sscanf(argv[i + 1], "%dx%d", &w, &h) == 2) i++;
        else if (!strcmp(argv[i], "--window") && i + 1 < argc && sscanf(argv[i + 1], "%dx%d", &ww, &wh) == 2) i++;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--qoi")) qoi = 1;
        else if (!strcmp(argv[i], "--yuv")) yuv = 1;
        else if (!strcmp(argv[i], "--loop")) loop = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--display") && i + 1 < argc) display = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--ahead") && i + 1 < argc) ahead = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
        else if (!strcmp(argv[i], "--gpu")) gpu = 1;
        else if (!strcmp(argv[i], "--hw") && i + 1 < argc) {
            const char* v = argv[++i];
            hw = !strcmp(v, "sw") ? YVID_HW_OFF : !strcmp(v, "dxva") ? YVID_HW_DXVA : -1;
            if (hw < 0) { fprintf(stderr, "video_play: --hw sw or dxva\n"); return 2; }
        }
        else {
            fprintf(stderr, "usage: video_play [--sim] [--file PATH [--index]] [--size WxH] [--frames N] [--fps N]\n"
                            "                  [--qoi] [--yuv] [--loop] [--seconds S] [--window WxH] [--display ID]\n"
                            "                  [--ahead N] [--csv PATH] [--hw sw|dxva] [--gpu]\n");
            return 2;
        }
    }
    /* --sim runs on ysp/rt.h's virtual clock: the same records on any host */
    if (sd.backend == YSCR_BACKEND_SIM) yrt_virtual_begin(0);
    if (w <= 0 || h <= 0 || n <= 0 || fps <= 0 || ww <= 0 || wh <= 0) { fprintf(stderr, "video_play: bad size, count or rate\n"); return 2; }

    if (!file) {
        if (write_seq(gen, w, h, n, fps, qoi, yuv) != 0) { fprintf(stderr, "video_play: cannot write %s\n", gen); return 1; }
        file = gen;
    } else if (index) {
        char err[300];
        int64_t k = yvid_index_make(file, NULL, NULL, err, sizeof err);
        if (k < 0) { fprintf(stderr, "video_play: index: %s\n", err); return 1; }
        printf("video_play: indexed %lld frames of %s\n", (long long)k, file);
    }

    /* In C99 the opens are compound literals with designated initializers:
     *   yvid_open(&mv, &gfx, &(yvid_desc){ .path = file, .loop = loop });
     * Written out field by field so the file also builds as C++17. */
    sd.window_w = ww;
    sd.window_h = wh;
    sd.display = display;
    sd.d3d11_video = gpu != 0;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "video_play: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.45f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "video_play: %s\n", ygfx_error(&gfx)); return 1; }
    memset(&vd, 0, sizeof vd);
    vd.path = file;
    vd.loop = loop != 0;
    vd.ahead = ahead;
    vd.hw_decode = (yvid_hw)hw;
    if (gpu) vd.gpu_path = YVID_PATH_GPU;
    if (!yvid_open(&mv, &gfx, &vd)) { fprintf(stderr, "video_play: %s\n", yvid_error(&mv)); return 1; }
    memset(&st, 0, sizeof st);
    film = yvid_stim(&mv, &st);
    yscr_describe(&scr, line, sizeof line);
    printf("%s\n", line);
    if (csv) {
        out = fopen(csv, "w");
        if (!out) { fprintf(stderr, "video_play: cannot write %s\n", csv); return 1; }
        fprintf(out, "display,vblank,onset,decision,why,frame,movie_t,update_ns,last_index,last_onset,last_dropped,last_flags,n_done\n");
    }
    yvid_play_at(&mv, YVID_ASAP);

    while (yscr_begin(&scr, &f) == YSCR_OK) {   /* Shift+Esc ends it */
        int64_t t0 = yrt_now_ns(), dt;
        yvid_record r;
        rc = yvid_update(&mv, &f);
        dt = yrt_now_ns() - t0;
        if (rc < 0) { fprintf(stderr, "video_play: %s (%s)\n", yvid_strerror(rc), yvid_error(&mv)); failed = 1; break; }
        if (!t_start) t_start = f.onset;
        if (f.index == 120) {
            heap0 = yvid_heap_calls();
            cpu0 = cpu_ns();
            wall0 = yrt_now_ns();
        }
        if (yvid_last(&mv, &r) == YVID_OK && r.display == f.index) {
            int shown = r.decision == YVID_SHOWN;
            int64_t** cv = shown ? &cost : &rcost;
            int64_t* nv = shown ? &n_cost : &n_rcost;
            int64_t* cp = shown ? &cap_cost : &cap_rcost;
            if (*nv == *cp) {
                int64_t* nc;
                *cp = *cp ? *cp * 2 : 4096;
                nc = (int64_t*)realloc(*cv, (size_t)*cp * sizeof **cv);
                if (!nc) break;
                *cv = nc;
            }
            if (f.index >= 120) (*cv)[(*nv)++] = dt;
            /* the flip record that arrived with this frame is the previous
             * frame's: its onset, vblanks dropped and flags */
            if (out) fprintf(out, "%lld,%lld,%lld,%d,%d,%lld,%lld,%lld,%lld,%lld,%u,%u,%d\n", (long long)f.index, (long long)f.vblank,
                             (long long)f.onset, r.decision, r.why, (long long)r.frame, (long long)r.movie_t, (long long)dt,
                             f.last ? (long long)f.last->index : -1LL, f.last ? (long long)f.last->onset : 0LL,
                             f.last ? (unsigned)f.last->dropped : 0u, f.last ? (unsigned)f.last->flags : 0u, (int)f.n_done);
        }
        {
            /* on the GPU path a decoder shares the screen's device and
             * context, so the GL and present calls may wait for it */
            int64_t d0 = yrt_now_ns();
            ygfx_begin(&gfx, &f);
            ygfx_draw(&gfx, &film);
            ygfx_end(&gfx);
            yscr_flip(&scr);
            if (f.index >= 120) {
                if (n_dcost == cap_dcost) {
                    int64_t* nc;
                    cap_dcost = cap_dcost ? cap_dcost * 2 : 4096;
                    nc = (int64_t*)realloc(dcost, (size_t)cap_dcost * sizeof *dcost);
                    if (!nc) break;
                    dcost = nc;
                }
                dcost[n_dcost++] = yrt_now_ns() - d0;
            }
        }
        if (rc == YVID_ENDED) break;
        if (seconds > 0 && (double)(f.onset - t_start) * 1e-9 >= seconds) break;
    }
    yscr_wait_flip(&scr, NULL);
    {
        yscr_frame g2;
        /* one more frame so the last flip's record reaches the movie */
        if (!failed && yscr_begin(&scr, &g2) == YSCR_OK) {
            int k;
            for (k = 0; k < g2.n_done; k++) yvid_flip_done(&mv, &g2.done[k]);
            ygfx_begin(&gfx, &g2); ygfx_end(&gfx); yscr_flip(&scr);
        }
    }
    yvid_describe(&mv, line, sizeof line);
    printf("%s\n", line);
    if (wall0) {
        int64_t dc = cpu_ns() - cpu0, dw = yrt_now_ns() - wall0;
        printf("video_play: process CPU after frame 120: %.1f%% of one core (%.3f s over %.3f s)\n",
               100.0 * (double)dc / (double)dw, (double)dc / 1e9, (double)dw / 1e9);
    }
    for (i = 0; i < 3; i++) {
        int64_t* cv = i == 2 ? dcost : i ? rcost : cost;
        int64_t nv = i == 2 ? n_dcost : i ? n_rcost : n_cost, k;
        double mean = 0;
        if (nv <= 0) continue;
        for (k = 0; k < nv; k++) mean += (double)cv[k];
        mean /= (double)nv;
        qsort(cv, (size_t)nv, sizeof *cv, cmp_i64);
        printf("video_play: %s on the frame thread, %s, %lld frames after 120: mean %.3f ms, p50 %.3f, p99 %.3f, max %.3f\n",
               i == 2 ? "draw + flip" : "yvid_update()", i == 2 ? "every display frame" : i ? "repeats" : "with an upload", (long long)nv, mean / 1e6, (double)cv[nv / 2] / 1e6, (double)cv[(nv * 99) / 100] / 1e6,
               (double)cv[nv - 1] / 1e6);
    }
    printf("video_play: heap calls after frame 120: %llu\n", (unsigned long long)(yvid_heap_calls() - heap0));
    if (out) fclose(out);
    free(cost);
    free(rcost);
    free(dcost);
    yvid_close(&mv);
    ygfx_close(&gfx);
    yscr_close(&scr);
    if (file == gen) remove(gen);
    return failed ? 1 : 0;
}
