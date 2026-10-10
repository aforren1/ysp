/* video_bench.c - what the decode thread spends per frame: pl_mpeg's decode,
 * the frame hash, the I420 to RGBA8 conversion, and QOI's decode, at
 * 1280 x 720 and 1920 x 1080 (or --size). CPU only: no window, no GPU.
 *
 * The MPEG-1 clip is the caller's (--mpg PATH, an MPEG-PS file; make one with
 * ffmpeg's testsrc2, docs/video.md). --mp4 PATH (with its index) measures
 * Media Foundation instead, in one decoder configuration (--hw), with open
 * and seek times (tests/media/make_video_clips.sh --long makes the clips).
 * The other inputs are generated: a
 * smooth frame (gradient, bar, blocks, as video_play draws) and a noisy one,
 * which are QOI's best and worst cases.
 *
 * Usage: video_bench [--mpg PATH] [--mp4 PATH [--hw sw|dxva] [--seeks N]] [--size WxH] [--reps N]
 * Exit code: 0; 1 when the clip does not decode; 2 for a bad argument.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS   /* sscanf() and fopen() under /W4 /WX */
#endif
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}

static void report(const char* what, int64_t* t, int n, double bytes) {
    double mean = 0;
    int i;
    for (i = 0; i < n; i++) mean += (double)t[i];
    mean /= n;
    qsort(t, (size_t)n, sizeof *t, cmp_i64);
    printf("  %-34s mean %7.3f ms  p50 %7.3f  p99 %7.3f  max %7.3f", what, mean / 1e6, t[n / 2] / 1e6,
           t[(n * 99) / 100] / 1e6, t[n - 1] / 1e6);
    if (bytes > 0) printf("  %6.0f MB/s", bytes / (mean / 1e9) / 1e6);
    printf("\n");
}

static void bench_size(int w, int h, int reps) {
    size_t px = (size_t)w * h;
    uint8_t* rgba = (uint8_t*)malloc(px * 4);
    uint8_t* back = (uint8_t*)malloc(px * 4);
    uint8_t* enc = (uint8_t*)malloc(yvid_qoi_max_bytes(w, h, 4));
    uint8_t* yuv = (uint8_t*)malloc(px + 2 * (size_t)((w + 1) / 2) * ((h + 1) / 2));
    int16_t* rows = (int16_t*)malloc(yvid_yuv_rows_bytes(w));
    int64_t* t = (int64_t*)malloc(sizeof(int64_t) * (size_t)reps);
    yvid_planes p;
    int k, r, x, y;
    uint64_t sink = 0;
    if (!rgba || !back || !enc || !yuv || !rows || !t) { fprintf(stderr, "video_bench: out of memory\n"); exit(1); }
    printf("%d x %d, %d repetitions\n", w, h, reps);
    yvid__planes_layout(YVID_FMT_I420, w, h, yuv, &p);
    for (k = 0; k < (int)(px + 2 * (size_t)((w + 1) / 2) * ((h + 1) / 2)); k++) yuv[k] = (uint8_t)(k * 2654435761u >> 24);
    for (r = 0; r < reps; r++) {
        int64_t t0 = yrt_now_ns();
        yvid_yuv_to_rgba(&p, YVID_FMT_I420, w, h, YVID_MATRIX_BT709, YVID_RANGE_LIMITED, YVID_SITING_LEFT,
                           YVID_CHROMA_SITED, rgba, w * 4, rows);
        t[r] = yrt_now_ns() - t0;
    }
    report("I420 to RGBA8, sited chroma", t, reps, (double)px * 4);
    for (r = 0; r < reps; r++) {
        int64_t t0 = yrt_now_ns();
        yvid_yuv_to_rgba(&p, YVID_FMT_I420, w, h, YVID_MATRIX_BT709, YVID_RANGE_LIMITED, YVID_SITING_LEFT,
                           YVID_CHROMA_NEAREST, rgba, w * 4, rows);
        t[r] = yrt_now_ns() - t0;
    }
    report("I420 to RGBA8, nearest chroma", t, reps, (double)px * 4);
    for (r = 0; r < reps; r++) {
        int64_t t0 = yrt_now_ns();
        sink ^= yvid__hash_planes(YVID_FMT_I420, &p);
        t[r] = yrt_now_ns() - t0;
    }
    report("XXH64 of the I420 planes", t, reps, (double)px * 1.5);
    for (k = 0; k < 2; k++) {
        int64_t n = 0;
        char what[64];
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            uint8_t* q = rgba + ((size_t)y * w + x) * 4;
            int v = k == 0 ? (112 + (x * 32) / w + ((x / 16 + y / 16) & 1) * 8) : (int)((uint32_t)((y * w + x) * 2654435761u) >> 24);
            q[0] = (uint8_t)v; q[1] = (uint8_t)(k ? v ^ 0x55 : v); q[2] = (uint8_t)(k ? v ^ 0xaa : v); q[3] = 255;
        }
        n = yvid_qoi_encode(rgba, w, h, 4, enc, yvid_qoi_max_bytes(w, h, 4));
        for (r = 0; r < reps; r++) {
            int64_t t0 = yrt_now_ns();
            if (yvid_qoi_decode(enc, (size_t)n, back, w, h) != YVID_OK) { fprintf(stderr, "video_bench: QOI failed\n"); exit(1); }
            t[r] = yrt_now_ns() - t0;
        }
        if (memcmp(back, rgba, px * 4) != 0) { fprintf(stderr, "video_bench: QOI round trip differs\n"); exit(1); }
        snprintf(what, sizeof what, "QOI decode, %s (%.1f:1)", k == 0 ? "smooth" : "noise", (double)px * 4 / (double)n);
        report(what, t, reps, (double)px * 4);
    }
    printf("  (%llx)\n", (unsigned long long)(sink & 0xF));
    free(rgba); free(back); free(enc); free(yuv); free(rows); free(t);
}

#ifndef YVID_NO_PL_MPEG
static int bench_mpg(const char* path) {
    plm_t* plm = plm_create_with_filename(path);
    int64_t* t;
    int n = 0, cap = 1 << 16, w, h;
    uint8_t* rgba;
    int16_t* rows;
    int64_t *th, *tc;
    if (!plm) { fprintf(stderr, "video_bench: pl_mpeg cannot open %s\n", path); return 1; }
    plm_set_audio_enabled(plm, FALSE);
    w = plm_get_width(plm); h = plm_get_height(plm);
    t = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
    th = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
    tc = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
    rgba = (uint8_t*)malloc((size_t)w * h * 4);
    rows = (int16_t*)malloc(yvid_yuv_rows_bytes(w));
    if (!t || !th || !tc || !rgba || !rows) return 1;
    printf("%s: %d x %d at %.3f fps\n", path, w, h, plm_get_framerate(plm));
    for (;;) {
        int64_t t0 = yrt_now_ns(), t1, t2;
        plm_frame_t* f = plm_decode_video(plm);
        yvid_planes p;
        if (!f || n >= cap) break;
        t1 = yrt_now_ns();
        memset(&p, 0, sizeof p);
        p.data[0] = f->y.data; p.stride[0] = (int32_t)f->y.width; p.w[0] = w; p.h[0] = h;
        p.data[1] = f->cb.data; p.stride[1] = (int32_t)f->cb.width; p.w[1] = (w + 1) / 2; p.h[1] = (h + 1) / 2;
        p.data[2] = f->cr.data; p.stride[2] = (int32_t)f->cr.width; p.w[2] = (w + 1) / 2; p.h[2] = (h + 1) / 2;
        (void)yvid__hash_planes(YVID_FMT_I420, &p);
        t2 = yrt_now_ns();
        yvid_yuv_to_rgba(&p, YVID_FMT_I420, w, h, YVID_MATRIX_BT601, YVID_RANGE_LIMITED, YVID_SITING_CENTER,
                           YVID_CHROMA_SITED, rgba, w * 4, rows);
        tc[n] = yrt_now_ns() - t2;
        th[n] = t2 - t1;
        t[n] = t1 - t0;
        n++;
    }
    plm_destroy(plm);
    if (n == 0) { fprintf(stderr, "video_bench: no frames in %s\n", path); return 1; }
    printf("  %d frames\n", n);
    report("pl_mpeg decode", t, n, 0);
    report("XXH64 of the planes", th, n, (double)w * h * 1.5);
    report("I420 to RGBA8, sited", tc, n, (double)w * h * 4);
    free(t); free(th); free(tc); free(rgba); free(rows);
    return 0;
}
#endif

#if YVID__MF
static double cpu_s(int process) {
    FILETIME a, b, k, u;
    BOOL ok = process ? GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u) : GetThreadTimes(GetCurrentThread(), &a, &b, &k, &u);
    if (!ok) return 0.0;
    return ((double)(((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
            (double)(((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e7;
}

/* Media Foundation, one decoder configuration: what the decode thread pays
 * per frame (ReadSample and the lock, which reads a DXVA surface back; the
 * hash; the NV12 to RGBA8 conversion), the CPU time of the whole process per
 * frame (Media Foundation's own threads included), open, and seeks. */
static int bench_mp4(const char* path, int hw, int seeks) {
    yvid__mf* m = (yvid__mf*)malloc(sizeof(yvid__mf));
    yvid_decoder_open in;
    yvid_stream st;
    yvid__index ix;
    yvid_desc vd;
    char err[512], name[256];
    int64_t *td, *th, *tc, *tt, *tp, *tq, opens[5];
    uint8_t* slot;
    int64_t n = 0, cap, k;
    double p0, p1, c0, c1;
    uint8_t* rgba;
    int16_t* rows;
    int rc;
    static const char* const hwn[] = { "auto", "software", "DXVA" };
    if (!m) return 1;
    memset(&vd, 0, sizeof vd);
    vd.path = path;
    if (yvid__index_load(&vd, &ix, 0, err, sizeof err) < 0) { fprintf(stderr, "video_bench: %s\n", err); return 1; }
    memset(&in, 0, sizeof in);
    in.path = path;
    for (k = 0; k < 5; k++) {   /* the first is the cold one */
        int64_t t0 = yrt_now_ns();
        memset(&st, 0, sizeof st);
        rc = yvid__mf_open_ex(m, &in, &st, hw, NULL, 0, ix.c.fps_num, ix.c.fps_den, err, sizeof err);
        if (rc == YVID_OK) {
            yvid_out o;
            yvid_planes d;
            memset(&o, 0, sizeof o);
            rc = yvid__mf_next(m, &d, &o);   /* to the first frame in hand */
        }
        opens[k] = yrt_now_ns() - t0;
        if (rc != YVID_OK) { fprintf(stderr, "video_bench: %s: %s\n", path, err); return 1; }
        if (k < 4) yvid__mf_close(m);
    }
    yvid__mf_describe(m, name, sizeof name);
    yvid__mf_close(m);
    printf("%s: %d x %d, %d/%d fps, %lld frames; %s (asked: %s)\n", path, (int)ix.c.w, (int)ix.c.h, (int)ix.c.fps_num,
           (int)ix.c.fps_den, (long long)ix.c.frames, name, hwn[hw]);
    printf("  open to the first frame: %.1f ms cold, then %.1f %.1f %.1f %.1f ms\n", opens[0] / 1e6, opens[1] / 1e6,
           opens[2] / 1e6, opens[3] / 1e6, opens[4] / 1e6);
    cap = ix.c.frames + 16;
    td = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
    th = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
    tc = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
    tt = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
    tp = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
    tq = (int64_t*)malloc(sizeof(int64_t) * (size_t)cap);
    slot = (uint8_t*)malloc(yvid__planes_layout(YVID_FMT_NV12, ix.c.w, ix.c.h, NULL, NULL));
    rgba = (uint8_t*)malloc((size_t)ix.c.w * ix.c.h * 4);
    rows = (int16_t*)malloc(yvid_yuv_rows_bytes(ix.c.w));
    if (!td || !th || !tc || !tt || !tp || !tq || !slot || !rgba || !rows) return 1;
    memset(&st, 0, sizeof st);
    if (yvid__mf_open_ex(m, &in, &st, hw, NULL, 0, ix.c.fps_num, ix.c.fps_den, err, sizeof err) != YVID_OK) return 1;
    p0 = cpu_s(1); c0 = cpu_s(0);
    for (;;) {
        yvid_out o;
        yvid_planes d;
        int64_t t0 = yrt_now_ns(), t1, t2, t3, t4;
        memset(&o, 0, sizeof o);
        if (n >= cap || yvid__mf_next(m, &d, &o) != YVID_OK) break;
        t1 = yrt_now_ns();
        (void)yvid__hash_planes(YVID_FMT_NV12, &o.planes);
        t2 = yrt_now_ns();
        yvid_yuv_to_rgba(&o.planes, YVID_FMT_NV12, ix.c.w, ix.c.h, YVID_MATRIX_BT709, YVID_RANGE_LIMITED,
                           YVID_SITING_LEFT, YVID_CHROMA_SITED, rgba, ix.c.w * 4, rows);
        t3 = yrt_now_ns();
        {   /* the planar path's step instead of the conversion: the planes,
             * with the decoder's pitch, into a slot's tight ones */
            yvid_planes sp;
            int pk, y;
            yvid__planes_layout(YVID_FMT_NV12, ix.c.w, ix.c.h, slot, &sp);
            for (pk = 0; pk < 2; pk++) {
                size_t rb = yvid__row_bytes(YVID_FMT_NV12, &sp, pk);
                for (y = 0; y < sp.h[pk]; y++)
                    memcpy(sp.data[pk] + (size_t)y * (size_t)sp.stride[pk], o.planes.data[pk] + (size_t)y * (size_t)o.planes.stride[pk], rb);
            }
        }
        t4 = yrt_now_ns();
        td[n] = t1 - t0; th[n] = t2 - t1; tc[n] = t3 - t2; tt[n] = t3 - t0; tp[n] = t4 - t3; tq[n] = (t2 - t0) + (t4 - t3);
        n++;
    }
    p1 = cpu_s(1); c1 = cpu_s(0);
    if (n == 0) { fprintf(stderr, "video_bench: no frames\n"); return 1; }
    printf("  %lld frames; CPU per frame: process %.2f ms, this thread %.2f ms, the other threads (Media Foundation's) %.2f ms\n",
           (long long)n, (p1 - p0) * 1e3 / (double)n, (c1 - c0) * 1e3 / (double)n, ((p1 - p0) - (c1 - c0)) * 1e3 / (double)n);
    report("ReadSample + lock (read back)", td, (int)n, 0);
    report("XXH64 of the NV12 planes", th, (int)n, (double)ix.c.w * ix.c.h * 1.5);
    report("NV12 to RGBA8, sited", tc, (int)n, (double)ix.c.w * ix.c.h * 4);
    report("planes into a slot (planar path)", tp, (int)n, (double)ix.c.w * ix.c.h * 1.5);
    report("decode thread total, RGBA8 path", tt, (int)n, 0);
    report("decode thread total, planar path", tq, (int)n, 0);
    if (seeks > 0) {
        /* request to the target in hand: the seek, the discards, the target */
        uint64_t r = 0x9E3779B97F4A7C15ull;
        int64_t* ts = (int64_t*)malloc(sizeof(int64_t) * (size_t)seeks);
        for (k = 0; k < seeks; k++) {
            int64_t target, key, j, t0;
            yvid_out o;
            yvid_planes d;
            r ^= r << 13; r ^= r >> 7; r ^= r << 17;
            target = (int64_t)(r % (uint64_t)ix.c.frames);
            key = target - target % ix.c.gop;
            t0 = yrt_now_ns();
            if (yvid__mf_seek(m, key, 0) != YVID_OK) { fprintf(stderr, "video_bench: seek failed\n"); return 1; }
            for (j = key; j <= target; j++) {
                memset(&o, 0, sizeof o);
                if (yvid__mf_next(m, j == target ? &d : NULL, &o) != YVID_OK) { fprintf(stderr, "video_bench: decode after seek failed\n"); return 1; }
            }
            ts[k] = yrt_now_ns() - t0;
        }
        report("seek to a random frame", ts, seeks, 0);
        free(ts);
    }
    yvid__mf_close(m);
    free(td); free(th); free(tc); free(tt); free(tp); free(tq); free(slot); free(rgba); free(rows); free(m);
    return 0;
}
#endif

int main(int argc, char** argv) {
    const char* mpg = NULL;
    const char* mp4 = NULL;
    int w = 0, h = 0, reps = 100, i, hw = YVID_HW_OFF, seeks = 50;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mpg") && i + 1 < argc) mpg = argv[++i];
        else if (!strcmp(argv[i], "--mp4") && i + 1 < argc) mp4 = argv[++i];
        else if (!strcmp(argv[i], "--hw") && i + 1 < argc) {
            const char* v = argv[++i];
            hw = !strcmp(v, "sw") ? YVID_HW_OFF : !strcmp(v, "dxva") ? YVID_HW_DXVA : -1;
            if (hw < 0) { fprintf(stderr, "video_bench: --hw sw or dxva\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--seeks") && i + 1 < argc) seeks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc && sscanf(argv[i + 1], "%dx%d", &w, &h) == 2) i++;
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else { fprintf(stderr, "usage: video_bench [--mpg PATH] [--mp4 PATH [--hw sw|dxva] [--seeks N]] [--size WxH] [--reps N]\n"); return 2; }
    }
    if (reps <= 0 || (w != 0 && (w <= 0 || h <= 0))) { fprintf(stderr, "video_bench: bad size or count\n"); return 2; }
    if (mp4) {
#if YVID__MF
        return bench_mp4(mp4, hw, seeks);
#else
        (void)hw; (void)seeks;
        fprintf(stderr, "video_bench: MP4 needs Media Foundation (Windows)\n");
        return 1;
#endif
    }
    if (mpg) {
#ifndef YVID_NO_PL_MPEG
        return bench_mpg(mpg);
#else
        fprintf(stderr, "video_bench: built with YVID_NO_PL_MPEG\n");
        return 1;
#endif
    }
    if (w > 0) bench_size(w, h, reps);
    else { bench_size(1280, 720, reps); bench_size(1920, 1080, reps); }
    return 0;
}
