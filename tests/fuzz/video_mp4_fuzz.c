/* video_mp4_fuzz.c - fuzz target for ysp/video.h's own MP4 box readers on the
 * FFmpeg path: the video track's edit list and display matrix, which the index
 * maker reads itself (FFmpeg's demuxer is FFmpeg's to fuzz). Every input must
 * return without a sanitizer report. A refusal is a correct answer to a bad
 * input, so the target asserts nothing else. Linux only, as the path is.
 *
 * libFuzzer, clang:
 *     clang -O1 -g -fsanitize=fuzzer,address,undefined -DYVID_FFMPEG -DYVID_NO_PL_MPEG -DYSCR_NO_SDL
 *         -Iinclude tests/fuzz/video_mp4_fuzz.c -o video_mp4_fuzz -lm -ldl -lpthread
 * Without clang (gcc under ASan and UBSan), N mutations of the built-in seeds:
 *     gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
 *         -DYVID_FFMPEG -DYVID_NO_PL_MPEG -DYSCR_NO_SDL -DYSP_FUZZ_SELF -Iinclude tests/fuzz/video_mp4_fuzz.c
 *         -o video_mp4_self -lm -ldl -lpthread
 *     ./video_mp4_self 1000000
 * Replay of files: add -DYSP_FUZZ_REPLAY in place of -DYSP_FUZZ_SELF, and give
 * the files as arguments.
 */
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"

#include <stdio.h>
#include <unistd.h>

/* The readers take a path, as the index maker does. /dev/shm keeps a run in
 * memory; another process's file is never read, by the pid in the name. */
static char g_path[128];

static void run_one(const uint8_t* data, size_t size) {
    char err[512];
    FILE* fh;
    if (!g_path[0]) snprintf(g_path, sizeof g_path, "/dev/shm/ysp_video_mp4_fuzz.%d", (int)getpid());
    fh = fopen(g_path, "wb");
    if (!fh) return;
    if (size) fwrite(data, 1, size, fh);
    fclose(fh);
    err[0] = 0;
    (void)yvid__mp4_edits(g_path, err, sizeof err);
    err[0] = 0;
    (void)yvid__mp4_matrix(g_path, err, sizeof err);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    run_one(data, size);
    return 0;
}

#if defined(YSP_FUZZ_REPLAY) || defined(YSP_FUZZ_SELF)
static size_t be32(uint8_t* o, uint32_t v) {
    o[0] = (uint8_t)(v >> 24); o[1] = (uint8_t)(v >> 16); o[2] = (uint8_t)(v >> 8); o[3] = (uint8_t)v;
    return 4;
}
static size_t box(uint8_t* o, const char* type, const uint8_t* body, size_t n) {
    be32(o, (uint32_t)(n + 8));
    memcpy(o + 4, type, 4);
    if (n) memmove(o + 8, body, n);
    return n + 8;
}
/* An ftyp, then a moov with one video track: tkhd (version 0), edts with one
 * elst entry, and mdia with its hdlr. */
static size_t seed(uint8_t* o) {
    static uint8_t tkhd[84], hdlr[24], elst[20], t[1024], m[1024];
    size_t n = 0, k = 0, tn = 0;
    static const uint8_t ident[36] = { 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0, 0, 0 };
    n += box(o + n, "ftyp", (const uint8_t*)"isom\0\0\0\0isom", 12);
    memcpy(tkhd + 40, ident, 36);
    tn += box(t + tn, "tkhd", tkhd, sizeof tkhd);
    be32(elst + 4, 1);
    k = box(m, "elst", elst, sizeof elst);
    tn += box(t + tn, "edts", m, k);
    memcpy(hdlr + 8, "vide", 4);
    k = box(m, "hdlr", hdlr, sizeof hdlr);
    tn += box(t + tn, "mdia", m, k);
    k = box(m, "trak", t, tn);
    n += box(o + n, "moov", m, k);
    return n;
}
#endif

#if defined(YSP_FUZZ_REPLAY)
int main(int argc, char** argv) {
    static uint8_t b[1 << 20];
    int i;
    for (i = 1; i < argc; i++) {
        FILE* fh = fopen(argv[i], "rb");
        size_t n;
        if (!fh) continue;
        n = fread(b, 1, sizeof b, fh);
        fclose(fh);
        run_one(b, n);
    }
    if (g_path[0]) remove(g_path);
    printf("video_mp4_fuzz: %d files\n", argc - 1);
    return 0;
}
#elif defined(YSP_FUZZ_SELF)
int main(int argc, char** argv) {
    static uint8_t s[4096], b[4096];
    size_t ns = seed(s);
    long long runs = argc > 1 ? atoll(argv[1]) : 100000, r;
    uint64_t x = 0x9E3779B97F4A7C15ull;
    {   /* the seed must reach the readers: good as it is, refused when its
         * matrix is not the identity */
        char err[512];
        FILE* fh;
        size_t off = 0;
        run_one(s, ns);
        if (yvid__mp4_matrix(g_path, err, sizeof err) != YVID_OK) { fprintf(stderr, "video_mp4_fuzz: the seed is refused: %s\n", err); return 1; }
        memcpy(b, s, ns);
        for (off = 0; off + 4 <= ns; off++) if (memcmp(b + off, "tkhd", 4) == 0) break;
        b[off + 4 + 40] ^= 1;
        fh = fopen(g_path, "wb");
        if (fh) { fwrite(b, 1, ns, fh); fclose(fh); }
        if (yvid__mp4_matrix(g_path, err, sizeof err) == YVID_OK) { fprintf(stderr, "video_mp4_fuzz: the seed's matrix is not read\n"); return 1; }
    }
    for (r = 0; r < runs; r++) {
        size_t n = ns;
        int k, flips;
        memcpy(b, s, ns);
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        flips = 1 + (int)(x % 8);
        for (k = 0; k < flips; k++) {
            size_t at;
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            at = (size_t)(x >> 8) % ns;
            switch ((x >> 3) & 3) {
            case 0: b[at] = (uint8_t)x; break;                                /* a byte */
            case 1: b[at] ^= (uint8_t)(1u << (x & 7)); break;                 /* a bit  */
            case 2: if (at + 4 <= ns) be32(b + at, (x & 8) ? 0xFFFFFFFFu : (uint32_t)(x >> 32) % 512); break;   /* a size */
            default: n = at ? at : 1; break;                                  /* cut    */
            }
        }
        run_one(b, n);
    }
    if (g_path[0]) remove(g_path);
    printf("video_mp4_fuzz: %lld inputs\n", runs);
    return 0;
}
#endif
