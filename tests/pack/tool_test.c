/* tool_test.c - the pack tool (pack/ysp/pack_tool.h) end to end. Exit 0 on pass.
 *
 * Writes a corpus of sources into ./pack_tool_work (a font, WAV files and
 * PNG files made here, a calibration, and the files of tests/pack/corpus),
 * builds a pack of every kind from a source description, reads each entry
 * back through ysp/pack.h's views and the owners' checks, and then:
 * builds again (same bytes), verifies, rebuilds from the manifest (same
 * bytes), appends to a program, extracts, damages packs, and refuses a list
 * of bad descriptions, each by its message. It writes corpus.ysppak and
 * pack_corpus.txt (the pack ID and each entry's SHA-256) for the
 * comparison across compilers (docs/pack.md, open question 2).
 *
 * Built against the core tool (YPT_NO_LAYOUT) the corpus has no glyph runs,
 * and the test checks that the tool refuses them by name. Both builds write
 * core.ysppak from the description without glyph runs: CI compares the two
 * files byte for byte, and each build's ypak verifies the other's. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
/* opendir, utime, chmod and pthreads under -std=c11 (the entry cache's tests) */
#if (defined(__linux__) || defined(__EMSCRIPTEN__)) && !defined(_DEFAULT_SOURCE) && !defined(_GNU_SOURCE) && !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#ifndef YSCR_NO_SDL
#define YSCR_NO_SDL
#endif
#ifndef YAU_NO_MINIAUDIO
#define YAU_NO_MINIAUDIO
#endif
#include "ysp/pack_tool.h"
#include "ysp/pack.h"
#include "ysp/json.h"
#include "ysp/experiment.h"
#include "ysp/table.h"
#include "ysp/gfx.h"
#include "ysp/color.h"
#include "ysp/audio.h"
#include "ysp/video.h"
#ifndef YPT_NO_LAYOUT
#include "ysp/layout.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <direct.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utime.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <dirent.h>
#include <utime.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define RMDIR(p) rmdir(p)
#endif

#ifndef YPT_TEST_DIR
#define YPT_TEST_DIR "tests/pack"
#endif

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define W "pack_tool_work"

#include "../adapt/import_gen.h"
#include "../adapt/colr_font_build.h"

#ifndef YPT_NO_LAYOUT
/* Whether the n bytes at p hold the k bytes at q. */
static int memmem_(const uint8_t* p, size_t n, const char* q, size_t k) {
    size_t i;
    for (i = 0; i + k <= n; i++)
        if (!memcmp(p + i, q, k)) return 1;
    return 0;
}
#endif
static void bpad4(bb* b) { while (b->n & 3) b8(b, 0); }

static int write_file(const char* path, const void* d, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    if (n) fwrite(d, 1, n, f);
    fclose(f);
    return 0;
}

static uint8_t* read_all(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    uint8_t* d;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (uint8_t*)malloc((size_t)sz + 1);
    if (d && sz > 0 && fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); d = NULL; }
    fclose(f);
    if (d) *n = (size_t)sz;
    return d;
}

static void copy_in(const char* rel) {
    char a[1024], b[1024];
    size_t n;
    uint8_t* d;
    snprintf(a, sizeof a, "%s/corpus/%s", YPT_TEST_DIR, rel);
    snprintf(b, sizeof b, W "/%s", rel);
    d = read_all(a, &n);
    CHECK(d != NULL, "corpus file %s", a);
    if (d) { write_file(b, d, n); free(d); }
}

/* --- a font: boxes for ' ' to '~' (without '@' and '`') and Hebrew; with
 * an OS/2 table when fs_type >= 0 (layout_test.c's writer, extended) ---- */

static const uint32_t font_ranges[][2] = { { 0x21, 0x3f }, { 0x41, 0x5f }, { 0x61, 0x7e }, { 0x05b0, 0x05ea } };
#define N_RANGES 4

static void make_font_r(bb* out, int fs_type, const uint32_t (*rg)[2], uint32_t nr) {
    bb t[8];
    static const char* tags[8] = { "OS/2", "cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp" };
    uint32_t ng = 2, g, i, r, nt = fs_type >= 0 ? 8 : 7, first = fs_type >= 0 ? 0 : 1;
    size_t dir;
    memset(t, 0, sizeof t);
    for (r = 0; r < nr; r++) ng += rg[r][1] - rg[r][0] + 1;
    for (i = 0; i < 78; i++) b8(&t[0], 0);
    if (fs_type >= 0) { t[0].d[8] = (uint8_t)(fs_type >> 8); t[0].d[9] = (uint8_t)fs_type; }
    {
        bb* c = &t[1];
        uint32_t nseg = nr + 2, base = 2, sr = 1, es = 0;
        while (sr * 2 <= nseg) { sr *= 2; es++; }
        b16(c, 0); b16(c, 1); b16(c, 3); b16(c, 1); b32(c, 12);
        b16(c, 4); b16(c, 16 + 8 * nseg); b16(c, 0);
        b16(c, 2 * nseg); b16(c, 2 * sr); b16(c, es); b16(c, 2 * nseg - 2 * sr);
        b16(c, 0x20);
        for (r = 0; r < nr; r++) b16(c, rg[r][1]);
        b16(c, 0xffff); b16(c, 0);
        b16(c, 0x20);
        for (r = 0; r < nr; r++) b16(c, rg[r][0]);
        b16(c, 0xffff);
        b16(c, (1 - 0x20) & 0xffff);
        for (r = 0; r < nr; r++) {
            b16(c, (base - rg[r][0]) & 0xffff);
            base += rg[r][1] - rg[r][0] + 1;
        }
        b16(c, 1);
        for (i = 0; i < nseg; i++) b16(c, 0);
    }
    for (g = 0; g < ng; g++) {
        b32(&t[6], (uint32_t)t[2].n);
        if (g >= 2) {
            /* a box with a notch whose depth depends on the glyph, so glyphs differ */
            bb* q = &t[2];
            uint32_t d = 100 + (g % 7) * 40;
            b16(q, 1); b16(q, 50); b16(q, 0); b16(q, 450); b16(q, 700);
            b16(q, 5); b16(q, 0);
            b8(q, 0x01 | 0x20); b8(q, 0x01 | 0x20); b8(q, 0x01 | 0x10); b8(q, 0x01); b8(q, 0x01); b8(q, 0x01 | 0x20);
            /* x deltas: 50, +400, (0), -200, -200, (0) */
            b16(q, 50); b16(q, 400); b16(q, (uint32_t)(-200 & 0xffff)); b16(q, (uint32_t)(-200 & 0xffff));
            /* y deltas: (0), (0), +700, -d, +d, ... ; the last point back to y 700 */
            b16(q, 700); b16(q, (uint32_t)(-(int)d & 0xffff)); b16(q, d);
            bpad4(q);
        }
    }
    b32(&t[6], (uint32_t)t[2].n);
    for (i = 0; i < 54; i++) b8(&t[3], 0);
    t[3].d[18] = 1000 >> 8; t[3].d[19] = 1000 & 255; t[3].d[51] = 1;
    for (i = 0; i < 36; i++) b8(&t[4], 0);
    t[4].d[4] = 0x03; t[4].d[5] = 0x20;
    t[4].d[6] = 0xff; t[4].d[7] = 0x38;
    t[4].d[34] = (uint8_t)(ng >> 8); t[4].d[35] = (uint8_t)ng;
    for (g = 0; g < ng; g++) { b16(&t[5], 500); b16(&t[5], g >= 2 ? 50 : 0); }
    b32(&t[7], 0x00005000); b16(&t[7], ng);
    out->n = 0;
    b32(out, 0x00010000); b16(out, nt); b16(out, nt == 8 ? 128 : 64); b16(out, nt == 8 ? 3 : 2); b16(out, nt == 8 ? 0 : 48);
    dir = out->n;
    for (i = 0; i < nt; i++) { b32(out, 0); b32(out, 0); b32(out, 0); b32(out, 0); }
    for (i = first; i < 8; i++) {
        size_t off, k = i - first;
        bpad4(out);
        off = out->n;
        bput(out, t[i].d, t[i].n);
        memcpy(out->d + dir + 16 * k, tags[i], 4);
        out->d[dir + 16 * k + 8] = (uint8_t)(off >> 24); out->d[dir + 16 * k + 9] = (uint8_t)(off >> 16);
        out->d[dir + 16 * k + 10] = (uint8_t)(off >> 8); out->d[dir + 16 * k + 11] = (uint8_t)off;
        out->d[dir + 16 * k + 12] = (uint8_t)(t[i].n >> 24); out->d[dir + 16 * k + 13] = (uint8_t)(t[i].n >> 16);
        out->d[dir + 16 * k + 14] = (uint8_t)(t[i].n >> 8); out->d[dir + 16 * k + 15] = (uint8_t)t[i].n;
    }
    for (i = 0; i < 8; i++) free(t[i].d);
    bpad4(out);
}

static void make_font(bb* out, int fs_type) { make_font_r(out, fs_type, font_ranges, N_RANGES); }

/* --- WAV and PNG files ------------------------------------------------------- */

static void make_wav(const char* path, int tag, int bits, int ch, uint32_t rate, int frames, int bad) {
    bb b;
    int i, c;
    int bytes = frames * ch * bits / 8;
    memset(&b, 0, sizeof b);
    bput(&b, "RIFF", 4); le32(&b, (uint32_t)(36 + bytes)); bput(&b, "WAVE", 4);
    bput(&b, "fmt ", 4); le32(&b, 16); le16(&b, (uint32_t)tag); le16(&b, (uint32_t)ch); le32(&b, rate);
    le32(&b, rate * (uint32_t)(ch * bits / 8)); le16(&b, (uint32_t)(ch * bits / 8)); le16(&b, (uint32_t)bits);
    bput(&b, "data", 4); le32(&b, (uint32_t)bytes);
    for (i = 0; i < frames; i++)
        for (c = 0; c < ch; c++) {
            int v = (int)((i * 37 + c * 1013) % 2001) - 1000;   /* a sawtooth, -1000..1000 */
            if (bits == 8) b8(&b, (uint32_t)(128 + v / 8));
            else if (bits == 16) le16(&b, (uint32_t)(v * 30) & 0xffff);
            else if (bits == 32 && tag == 1) le32(&b, (uint32_t)(v * 7000) << 8);
            else if (bits == 32) {
                float f = (float)v / 1024.0f;
                uint32_t u;
                memcpy(&u, &f, 4);
                if (bad && i == 5) u = 0x7FC00000u;
                le32(&b, u);
            } else {
                double d = (double)v / 1024.0;
                uint64_t u;
                memcpy(&u, &d, 8);
                le32(&b, (uint32_t)u); le32(&b, (uint32_t)(u >> 32));
            }
        }
    write_file(path, b.d, b.n);
    free(b.d);
}

static void chunk(bb* b, const char* type, const uint8_t* d, size_t n) {
    uint32_t crc;
    b32(b, (uint32_t)n);
    bput(b, type, 4);
    if (n) bput(b, d, n);
    crc = ypak_crc32(ypak_crc32(0, type, 4), d, n);
    b32(b, crc);
}

/* A PNG with stored (uncompressed) deflate blocks. */
static void make_png(const char* path, uint32_t w, uint32_t h, int ct, int depth, const uint8_t* rows, int iccp) {
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    int chans = ct == 0 ? 1 : ct == 4 ? 2 : ct == 2 ? 3 : 4;
    size_t rb = (size_t)w * (size_t)chans * (size_t)depth / 8, i, raw_n = (rb + 1) * h, at = 0;
    uint8_t* raw = (uint8_t*)malloc(raw_n);
    uint8_t ih[13];
    bb z, b;
    uint32_t a = 1, s2 = 0;
    memset(&z, 0, sizeof z);
    memset(&b, 0, sizeof b);
    for (i = 0; i < h; i++) { raw[i * (rb + 1)] = 0; memcpy(raw + i * (rb + 1) + 1, rows + i * rb, rb); }
    for (i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; s2 = (s2 + a) % 65521; }
    b8(&z, 0x78); b8(&z, 0x01);
    while (at < raw_n || raw_n == 0) {
        size_t k = raw_n - at > 65535 ? 65535 : raw_n - at;
        b8(&z, at + k == raw_n ? 1 : 0);
        le16(&z, (uint32_t)k); le16(&z, (uint32_t)(~k & 0xffff));
        bput(&z, raw + at, k);
        at += k;
        if (raw_n == 0) break;
    }
    b32(&z, (s2 << 16) | a);
    bput(&b, sig, 8);
    ih[0] = (uint8_t)(w >> 24); ih[1] = (uint8_t)(w >> 16); ih[2] = (uint8_t)(w >> 8); ih[3] = (uint8_t)w;
    ih[4] = (uint8_t)(h >> 24); ih[5] = (uint8_t)(h >> 16); ih[6] = (uint8_t)(h >> 8); ih[7] = (uint8_t)h;
    ih[8] = (uint8_t)depth; ih[9] = (uint8_t)ct; ih[10] = 0; ih[11] = 0; ih[12] = 0;
    chunk(&b, "IHDR", ih, 13);
    if (iccp == 1) {
        static const uint8_t pr[] = { 'x', 0, 0, 0x78, 0x01, 0x01, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01 };
        chunk(&b, "iCCP", pr, sizeof pr);
    } else if (iccp) {
        /* a real profile, zlib with one stored block */
        bb pf, c;
        uint32_t a2 = 1, t2 = 0;
        memset(&pf, 0, sizeof pf);
        memset(&c, 0, sizeof c);
        make_icc(&pf, iccp == 2);
        for (i = 0; i < pf.n; i++) { a2 = (a2 + pf.d[i]) % 65521; t2 = (t2 + a2) % 65521; }
        bput(&c, "icc", 4);   /* name and its NUL */
        b8(&c, 0);            /* compression method */
        b8(&c, 0x78); b8(&c, 0x01); b8(&c, 1);
        le16(&c, (uint32_t)pf.n); le16(&c, (uint32_t)(~pf.n & 0xffff));
        bput(&c, pf.d, pf.n);
        b32(&c, (t2 << 16) | a2);
        chunk(&b, "iCCP", c.d, c.n);
        free(pf.d); free(c.d);
    }
    chunk(&b, "IDAT", z.d, z.n);
    chunk(&b, "IEND", NULL, 0);
    write_file(path, b.d, b.n);
    free(b.d); free(z.d); free(raw);
}

static void jpeg_file(const char* path, int w, int h, int nc, int sub, int marker, int prec, int flat_chroma, const bb* app) {
    bb b;
    make_jpeg(&b, w, h, nc, sub, marker, prec, flat_chroma, app);
    write_file(path, b.d, b.n);
    free(b.d);
}

static void xlsx_file(const char* path, const char* sheet1) {
    bb b;
    make_xlsx(&b, sheet1);
    write_file(path, b.d, b.n);
    free(b.d);
}

/* --- frame sequences (ysp/video.h's writer) ---------------------------------- */

static int make_seq(const char* path, int32_t fmt, int32_t comp, int frames) {
    static uint8_t px[16 * 8 * 4];
    yvid_seq w;
    yvid_seq_desc d;
    int i, k;
    memset(&d, 0, sizeof d);
    d.path = path;
    d.w = 16;
    d.h = 8;
    d.format = fmt;
    d.compression = comp;
    d.fps_num = 30;
    d.fps_den = 1;
    if (fmt == YVID_FMT_I420) {
        d.matrix = YVID_MATRIX_BT709; d.range = YVID_RANGE_LIMITED; d.transfer = YVID_TRC_BT1886; d.primaries = YVID_PRIM_BT709;
        d.siting = YVID_SITING_LEFT;
    }
    memset(&w, 0, sizeof w);
    if (!yvid_seq_open(&w, &d)) return -1;
    for (i = 0; i < frames; i++) {
        const void* planes[3];
        int32_t strides[3];
        for (k = 0; k < (int)sizeof px; k++) px[k] = (uint8_t)(k * 7 + i * 31 + (k % 4 == 3 ? 255 : 0));
        if (fmt == YVID_FMT_I420) {
            planes[0] = px; planes[1] = px + 128; planes[2] = px + 160;
            strides[0] = 16; strides[1] = 8; strides[2] = 8;
        } else {
            planes[0] = px; planes[1] = planes[2] = NULL;
            strides[0] = 64; strides[1] = strides[2] = 0;
        }
        if (yvid_seq_write(&w, planes, strides, NULL) < 0) { yvid_seq_close(&w); return -1; }
    }
    if (yvid_seq_finish(&w) < 0) return -1;
    yvid_seq_close(&w);
    return 0;
}

static uint8_t g_rgba[33 * 21 * 4], g_gray[17 * 9], g_g16[11 * 7 * 2], g_c16[5 * 4 * 6];

static void make_sources(void) {
    bb f;
    uint32_t i;
    MKDIR(W); MKDIR(W "/fonts"); MKDIR(W "/audio"); MKDIR(W "/img"); MKDIR(W "/art"); MKDIR(W "/shaders"); MKDIR(W "/data");
    MKDIR(W "/video"); MKDIR(W "/tables");
    memset(&f, 0, sizeof f);
    make_font(&f, -1);
    write_file(W "/fonts/box.ttf", f.d, f.n);
    make_font(&f, 2);
    write_file(W "/fonts/restricted.ttf", f.d, f.n);
    make_font(&f, 8);
    write_file(W "/fonts/editable.ttf", f.d, f.n);
    free(f.d);
    make_wav(W "/audio/tone.wav", 1, 16, 2, 48000, 4800, 0);
    make_wav(W "/audio/long.wav", 3, 32, 2, 48000, 300000, 0);   /* 2.4 MB: three chunks */
    make_wav(W "/audio/u8.wav", 1, 8, 1, 48000, 1000, 0);
    make_wav(W "/audio/s32.wav", 1, 32, 2, 48000, 1000, 0);
    make_wav(W "/audio/f64.wav", 3, 64, 1, 48000, 1000, 0);
    make_wav(W "/audio/at44.wav", 1, 16, 2, 44100, 1000, 0);
    make_wav(W "/audio/nan.wav", 3, 32, 2, 48000, 1000, 1);
    for (i = 0; i < sizeof g_rgba; i++) g_rgba[i] = (uint8_t)(i % 4 == 3 ? 255 : (i / 4 % 33) * 7 + (i / 132) * 3 + (i % 4) * 50);
    for (i = 0; i < sizeof g_gray; i++) g_gray[i] = (uint8_t)(i * 13);
    for (i = 0; i < sizeof g_g16; i++) g_g16[i] = (uint8_t)(i * 29 + 7);
    for (i = 0; i < sizeof g_c16; i++) g_c16[i] = (uint8_t)(i * 31 + 3);
    make_png(W "/img/rgba.png", 33, 21, 6, 8, g_rgba, 0);
    make_png(W "/img/gray.png", 17, 9, 0, 8, g_gray, 0);
    make_png(W "/img/gray16.png", 11, 7, 0, 16, g_g16, 0);
    make_png(W "/img/rgb16.png", 5, 4, 2, 16, g_c16, 0);
    make_png(W "/img/iccp.png", 17, 9, 0, 8, g_gray, 1);
    {
        static ycol_cal cal;
        static const float xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.329f } };
        static uint8_t buf[70000];
        int n;
        ycol_cal_init(&cal);
        CHECK(ycol_cal_nominal(&cal, xy, 100.0f, 2.2) >= 0, "nominal calibration");
        n = ycol_cal_save(&cal, buf, sizeof buf);
        CHECK(n == 62528, "calibration size %d", n);
        write_file(W "/display.yspcal", buf, (size_t)n);
    }
    {
        uint8_t* big = (uint8_t*)malloc(2621517);
        uint32_t x = 99;
        for (i = 0; i < 2621517; i++) { x = x * 1664525u + 1013904223u; big[i] = (uint8_t)(x >> 24); }
        write_file(W "/data/big.bin", big, 2621517);
        free(big);
    }
    copy_in("conditions.csv");
    copy_in("art/fixation.svg");
    copy_in("shaders/plaid.glsl");
    copy_in("main.json");
    copy_in("readme.txt");
    /* phase 3 sources (docs/pack.md 13) */
    {
        bb app;
        xlsx_file(W "/tables/cond.xlsx", g_sheet1);
        jpeg_file(W "/img/grayj.jpg", 20, 12, 1, 1, 0xC0, 8, 0, NULL);
        memset(&app, 0, sizeof app);
        app_exif(&app, 1);
        app_icc(&app, 1, 2);   /* an sRGB profile in two chunks */
        jpeg_file(W "/img/photo.jpg", 40, 24, 3, 2, 0xC0, 8, 0, &app);
        free(app.d);
        jpeg_file(W "/img/flat.jpg", 24, 16, 3, 1, 0xC2, 8, 1, NULL);
        make_png(W "/img/icc.png", 33, 21, 6, 8, g_rgba, 2);
        CHECK(make_seq(W "/video/bars.yspvseq", YVID_FMT_RGBA8, YVID_SEQ_QOI, 3) == 0, "frame sequence (QOI)");
        CHECK(make_seq(W "/video/yuv.yspvseq", YVID_FMT_I420, YVID_SEQ_RAW, 2) == 0, "frame sequence (I420)");
    }
}

#define DESC_HEAD \
    "{\n" \
    "  \"format\": \"ysp-pack-source\", \"version\": 1,\n" \
    "  \"experiment\": {\"entry\": \"experiment/main.json\", \"source\": \"main.json\", \"media_type\": \"application/vnd.ysp.experiment\"},\n" \
    "  \"audio\": {\"rate\": 48000, \"channels\": 2},\n" \
    "  \"resources\": [\n" \
    "    {\"kind\": \"table\", \"source\": \"conditions.csv\", \"types\": {\"word\": \"string\"}},\n" \
    "    {\"kind\": \"calibration\", \"source\": \"display.yspcal\"},\n" \
    "    {\"kind\": \"font\", \"source\": \"fonts/box.ttf\", \"license\": \"CC0-1.0\"},\n" \
    "    {\"kind\": \"curveset\", \"from\": \"fonts/box.ttf\"},\n"

#define DESC_RUNS \
    "    {\"kind\": \"glyphruns\", \"name\": \"text/pages.ysprun\", \"fonts\": [\"fonts/box.ttf\"], \"blocks\": [\n" \
    "      {\"key\": \"intro\", \"text\": \"Press a key to start.\", \"size\": \"32\", \"width\": \"400\"},\n" \
    "      {\"key\": \"rtl\", \"text\": \"\\u05e9\\u05dc\\u05d5\\u05dd world\", \"size\": \"24\"},\n" \
    "      {\"key\": \"end\", \"text\": \"Thank you!\", \"size\": \"40\", \"align\": \"center\", \"width\": \"600\"}]},\n"

#define DESC_TAIL \
    "    {\"kind\": \"artwork\", \"source\": \"art/fixation.svg\"},\n" \
    "    {\"kind\": \"audio\", \"source\": \"audio/tone.wav\"},\n" \
    "    {\"kind\": \"audio\", \"source\": \"audio/long.wav\", \"stream\": true},\n" \
    "    {\"kind\": \"audio\", \"source\": \"audio/u8.wav\"},\n" \
    "    {\"kind\": \"audio\", \"source\": \"audio/s32.wav\"},\n" \
    "    {\"kind\": \"audio\", \"source\": \"audio/f64.wav\"},\n" \
    "    {\"kind\": \"shader\", \"source\": \"shaders/plaid.glsl\", \"mode\": \"modulation\"},\n" \
    "    {\"kind\": \"texture\", \"source\": \"img/gray.png\"},\n" \
    "    {\"kind\": \"texture\", \"source\": \"img/rgba.png\", \"compression\": \"qoi\", \"primaries\": \"bt709\"},\n" \
    "    {\"kind\": \"texture\", \"name\": \"img/rgba_raw.ysptex\", \"source\": \"img/rgba.png\", \"primaries\": \"device\"},\n" \
    "    {\"kind\": \"texture\", \"source\": \"img/gray16.png\", \"encoding\": \"linear\"},\n" \
    "    {\"kind\": \"texture\", \"source\": \"img/rgb16.png\", \"format\": \"rgba32f\", \"encoding\": \"linear\"},\n" \
    "    {\"kind\": \"file\", \"source\": \"readme.txt\"},\n" \
    "    {\"kind\": \"file\", \"source\": \"data/big.bin\"},\n" \
    "    {\"kind\": \"table\", \"source\": \"tables/cond.xlsx\", \"types\": {\"contrast\": \"number\"}},\n" \
    "    {\"kind\": \"table\", \"name\": \"tables/sheet2.pstb\", \"source\": \"tables/cond.xlsx\", \"sheet\": \"Sheet2\"},\n" \
    "    {\"kind\": \"texture\", \"source\": \"img/grayj.jpg\"},\n" \
    "    {\"kind\": \"texture\", \"source\": \"img/photo.jpg\", \"primaries\": \"bt709\"},\n" \
    "    {\"kind\": \"texture\", \"source\": \"img/flat.jpg\", \"primaries\": \"device\", \"compression\": \"qoi\"},\n" \
    "    {\"kind\": \"texture\", \"source\": \"img/icc.png\", \"primaries\": \"device\"},\n" \
    "    {\"kind\": \"frameseq\", \"source\": \"video/bars.yspvseq\"},\n" \
    "    {\"kind\": \"frameseq\", \"source\": \"video/yuv.yspvseq\"},\n" \
    "    {\"kind\": \"audio\", \"name\": \"audio/at44_48k.wav\", \"source\": \"audio/at44.wav\", \"resample\": \"sinc_best\"}\n" \
    "  ]\n" \
    "}\n"

/* The core description: every kind but glyph runs. */
static const char* g_desc_core = DESC_HEAD DESC_TAIL;
#ifdef YPT_NO_LAYOUT
static const char* g_desc = DESC_HEAD DESC_TAIL;
#define N_ENTRIES 30u
#else
static const char* g_desc = DESC_HEAD DESC_RUNS DESC_TAIL;
#define N_ENTRIES 31u
#endif

static int build_text(const char* text, const char* out, char* err, size_t cap) {
    return ypt_build_text(text, strlen(text), W, out, NULL, err, cap);
}

static const void* entry(ypak_pack* p, const char* name, ypak_entry* e) {
    if (ypak_find(p, name, e) != 0) { CHECK(0, "no entry %s: %s", name, ypak_error(p)); return NULL; }
    return ypak_data(p, e);
}

/* matrix, range, transfer, primaries; the other four bytes 0 */
static int enc_is(const uint8_t* enc, int m, int r, int t, int pr) {
    return enc[0] == m && enc[1] == r && enc[2] == t && enc[3] == pr && !enc[4] && !enc[5] && !enc[6] && !enc[7];
}

static void check_entries(const char* path) {
    ypak_pack p;
    ypak_desc d;
    ypak_entry e;
    const void* x;
    char err[256];
    int rc;
    memset(&d, 0, sizeof d);
    d.path = path;
    d.verify = YPAK_VERIFY_OPEN;
    rc = ypak_open(&p, &d) ? 0 : p.error_code;
    CHECK(rc == 0, "open %s: %s", path, ypak_error(&p));
    if (rc) return;
    CHECK(p.n == N_ENTRIES, "entries %u", p.n);
    CHECK(ypak_first_of(&p, YPAK_KIND_EXPERIMENT, &e) == 0 && e.name_len == 20 && !memcmp(e.name, "experiment/main.json", 20), "experiment");
    if ((x = entry(&p, "conditions.pstb", &e)) != NULL) {
        ytb_table t;
        CHECK(ytb_open_view(&t, x, (size_t)e.size) && t.n_rows == 4 && strcmp(ytb_text(&t, 1, ytb_col(&t, "word")), "dog, big") == 0 &&
              ytb_num(&t, 3, ytb_col(&t, "contrast")) == 0.0625, "table: %s", ytb_error(&t));
    }
    if ((x = entry(&p, "display.yspcal", &e)) != NULL) {
        static ycol_cal cal;
        CHECK(e.size == 62528 && ycol_cal_load(&cal, x, (size_t)e.size, err, sizeof err) >= 0, "calibration");
    }
    if ((x = entry(&p, "fonts/box.yspcset", &e)) != NULL) {
        ypak_cset s;
        ygfx_cset_desc cd;
        CHECK(ypak_cset_view(x, e.size, &s, err, sizeof err) == 0 && (s.flags & YPAK_CSET_ALL) && s.units == YPAK_CSET_UNITS_EM, "cset view: %s", err);
        memset(&cd, 0, sizeof cd);
        cd.texels = s.texels; cd.n_texels = s.n_texels; cd.words = s.words; cd.n_words = s.n_words;
        CHECK(ygfx_cset_check(&cd, err, sizeof err) == 0, "cset check: %s", err);
        CHECK(s.n_glyphs == 2 + 31 + 31 + 30 + 59, "cset glyphs %u", s.n_glyphs);
    }
#ifndef YPT_NO_LAYOUT
    if ((x = entry(&p, "text/pages.ysprun", &e)) != NULL) {
        ypak_runs r;
        int k;
        CHECK(ypak_runs_view(x, e.size, &r, err, sizeof err) == 0 && r.n_blocks == 3 && r.n_fonts == 1, "runs view: %s", err);
        k = ypak_runs_find(&r, "intro");
        CHECK(k >= 0 && strcmp(ypak_runs_str(&r, r.blocks[k].text_off), "Press a key to start.") == 0 && r.blocks[k].n_items == 21 &&
              r.blocks[k].n_lines >= 1, "runs block intro: %d items", k >= 0 ? (int)r.blocks[k].n_items : -1);
        k = ypak_runs_find(&r, "rtl");
        CHECK(k >= 0 && r.blocks[k].resolved_dir == YLAY_DIR_RTL && r.blocks[k].n_runs >= 1, "runs rtl block");
        CHECK(strcmp(ypak_runs_str(&r, r.fonts[0].cset_off), "fonts/box.yspcset") == 0, "runs font names");
    }
#endif
    if ((x = entry(&p, "art/fixation.yspart", &e)) != NULL) {
        ypak_art a;
        ygfx_cset_desc cd;
        CHECK(ypak_art_view(x, e.size, &a, err, sizeof err) == 0 && a.n_layers == 6 && a.viewbox[2] == 100.0, "art view: %s (%u layers)", err, a.n_layers);
        CHECK(a.layers[0].rgba == 0x336699FFu && a.layers[1].source == 2, "art layers");
        memset(&cd, 0, sizeof cd);
        cd.texels = a.set.texels; cd.n_texels = a.set.n_texels; cd.words = a.set.words; cd.n_words = a.set.n_words;
        CHECK(ygfx_cset_check(&cd, err, sizeof err) == 0, "art cset check: %s", err);
    }
    {
        static const char* const wavs[5] = { "audio/tone.wav", "audio/long.wav", "audio/u8.wav", "audio/s32.wav", "audio/f64.wav" };
        static const int fmts[5] = { YAU_WAV_S16, YAU_WAV_F32, YAU_WAV_S16, YAU_WAV_S24_32, YAU_WAV_F32 };
        int k;
        for (k = 0; k < 5; k++) {
            if ((x = entry(&p, wavs[k], &e)) != NULL) {
                yau_wav_desc wd;
                yau_wav_info wi;
                memset(&wd, 0, sizeof wd);
                wd.data = x;
                wd.size = (size_t)e.size;
                CHECK(yau_wav_probe(&wd, &wi, err, sizeof err) == 0 && wi.rate == 48000 && wi.format == fmts[k], "wav %s: %s", wavs[k], err);
                CHECK((k == 1) == ((e.flags & YPAK_F_STREAM) != 0), "stream flag %s", wavs[k]);
            }
        }
        /* the streamed WAV through a cursor, chunk by chunk */
        if (ypak_find(&p, "audio/long.wav", &e) == 0) {
            ypak_cursor c;
            size_t n;
            uint8_t* src = read_all(W "/audio/long.wav", &n);
            uint8_t* got = (uint8_t*)malloc((size_t)e.size);
            CHECK(e.chunk_first != YPAK_NO_CHUNK, "long.wav has chunks");
            CHECK(ypak_cursor_init(&c, &p, &e, NULL, 0) == 0 && ypak_cursor_read(&c, 0, got, e.size) == e.size && src && n == (size_t)e.size &&
                  memcmp(got, src, n) == 0, "cursor reads long.wav");
            free(src); free(got);
        }
        /* resampled from 44.1 kHz: float at the project rate */
        if ((x = entry(&p, "audio/at44_48k.wav", &e)) != NULL) {
            yau_wav_desc wd;
            yau_wav_info wi;
            memset(&wd, 0, sizeof wd);
            wd.data = x;
            wd.size = (size_t)e.size;
            CHECK(yau_wav_probe(&wd, &wi, err, sizeof err) == 0 && wi.rate == 48000 && wi.format == YAU_WAV_F32 && wi.channels == 2 &&
                  wi.frames >= 1088 && wi.frames <= 1089, "resampled WAV: %s (%u Hz, %lld frames)", err, wi.rate, (long long)wi.frames);
        }
        /* the u8 conversion: exact */
        if ((x = entry(&p, "audio/u8.wav", &e)) != NULL) {
            const uint8_t* s = (const uint8_t*)x + 44;
            int v0 = (int16_t)(s[0] | (s[1] << 8));
            CHECK(v0 == ((128 + (-1000) / 8) - 128) * 256, "u8 to s16 sample 0: %d", v0);
        }
    }
    if ((x = entry(&p, "shaders/plaid.yspshd", &e)) != NULL) {
        ypak_shader s;
        size_t n;
        uint8_t* src = read_all(W "/shaders/plaid.glsl", &n);
        CHECK(ypak_shader_view(x, e.size, &s, err, sizeof err) == 0 && s.params == 7u && s.contract == YGFX_SHADER_CONTRACT &&
              s.mode == YGFX_MODULATION && src && s.body_len == n && memcmp(s.body, src, n) == 0, "shader view: %s", err);
        {
            static char wt[65536];
            int wn = ygfx_shader_wrap(s.body, YGFX_MODULATION, wt, sizeof wt);
            CHECK(wn > 0 && ypak_xxh64(wt, (size_t)wn, 0) == s.wrap_hash, "shader wrap hash");
        }
        free(src);
    }
    if ((x = entry(&p, "img/rgba.ysptex", &e)) != NULL) {
        ypak_texture t;
        static uint8_t dec[sizeof g_rgba];
        CHECK(ypak_texture_view(x, e.size, &t, err, sizeof err) == 0 && t.compression == YPAK_TEX_QOI && t.w == 33 && enc_is(t.enc, 1, 2, 3, 2), "qoi texture: %s", err);
        CHECK(ypak_qoi_decode(&t, dec, sizeof dec, err, sizeof err) == 0 && memcmp(dec, g_rgba, sizeof g_rgba) == 0, "qoi texels: %s", err);
    }
    if ((x = entry(&p, "img/rgba_raw.ysptex", &e)) != NULL) {
        ypak_texture t;
        CHECK(ypak_texture_view(x, e.size, &t, err, sizeof err) == 0 && t.compression == YPAK_TEX_RAW && memcmp(t.data, g_rgba, sizeof g_rgba) == 0 &&
              enc_is(t.enc, 1, 2, 3, 1), "raw texture");
        CHECK(((uintptr_t)t.data & 15u) == 0, "texture data alignment");
    }
    if ((x = entry(&p, "img/gray.ysptex", &e)) != NULL) {
        ypak_texture t;
        CHECK(ypak_texture_view(x, e.size, &t, err, sizeof err) == 0 && t.format == YPAK_TEX_R8 && memcmp(t.data, g_gray, sizeof g_gray) == 0 &&
              enc_is(t.enc, 1, 2, 3, 1), "gray texture: sRGB, primaries DEVICE");
    }
    if ((x = entry(&p, "img/gray16.ysptex", &e)) != NULL) {
        ypak_texture t;
        CHECK(ypak_texture_view(x, e.size, &t, err, sizeof err) == 0 && t.format == YPAK_TEX_R16UI && enc_is(t.enc, 0, 0, 0, 0) &&
              t.data[0] == g_g16[1] && t.data[1] == g_g16[0], "gray16 texture is little-endian");
    }
    if ((x = entry(&p, "img/rgb16.ysptex", &e)) != NULL) {
        ypak_texture t;
        float f0;
        CHECK(ypak_texture_view(x, e.size, &t, err, sizeof err) == 0 && t.format == YPAK_TEX_RGBA32F && enc_is(t.enc, 0, 0, 0, 0), "rgb16 texture: linear, no encoding");
        memcpy(&f0, t.data, 4);
        CHECK(f0 == (float)((g_c16[0] << 8) | g_c16[1]) / 65535.0f, "rgb16 to float");
        memcpy(&f0, t.data + 12, 4);
        CHECK(f0 == 1.0f, "rgb16 alpha is 1");
    }
    if ((x = entry(&p, "data/big.bin", &e)) != NULL) {
        size_t n;
        uint8_t* src = read_all(W "/data/big.bin", &n);
        CHECK(src && n == (size_t)e.size && memcmp(src, x, n) == 0 && e.chunk_first != YPAK_NO_CHUNK, "big file");
        free(src);
    }
    /* phase 3: an .xlsx sheet as a table */
    if ((x = entry(&p, "tables/cond.pstb", &e)) != NULL) {
        ytb_table t;
        int ok = ytb_open_view(&t, x, (size_t)e.size);
        int cw = ytb_col(&t, "word"), cc = ytb_col(&t, "contrast"), cd = ytb_col(&t, "code"), cf = ytb_col(&t, "flag"), cn = ytb_col(&t, "note");
        CHECK(ok && t.n_rows == 3 && t.n_cols == 5, "xlsx table: %s (%u rows)", ytb_error(&t), ok ? t.n_rows : 0);
        if (ok && t.n_rows == 3) {
            CHECK(!strcmp(ytb_text(&t, 0, cw), "cat") && !strcmp(ytb_text(&t, 1, cw), "d<og") && !strcmp(ytb_text(&t, 2, cw), "\xce\xb1\xce\xb2"),
                  "xlsx shared strings: rich text, phonetic text left out, character references");
            CHECK(ytb_num(&t, 0, cc) == 0.25 && ytb_num(&t, 1, cc) == 1.0 && ytb_num(&t, 2, cc) == 0.01, "xlsx numbers and a cached formula");
            CHECK(ytb_col_type(&t, cd) == YTB_STRING && !strcmp(ytb_text(&t, 0, cd), "007") && !strcmp(ytb_text(&t, 1, cd), "xAy"),
                  "xlsx text cells stay text ('007'), _xHHHH_ escapes");
            CHECK(!strcmp(ytb_text(&t, 0, cf), "TRUE") && !strcmp(ytb_text(&t, 1, cf), "FALSE"), "xlsx booleans");
            CHECK(!strcmp(ytb_text(&t, 0, cn), "a, \"b\"") && !strcmp(ytb_text(&t, 1, cn), "") && !strcmp(ytb_text(&t, 2, cn), "xy"),
                  "xlsx inline rich text with a comma and quotes, a formula string");
        }
    }
    if ((x = entry(&p, "tables/sheet2.pstb", &e)) != NULL) {
        ytb_table t;
        CHECK(ytb_open_view(&t, x, (size_t)e.size) && t.n_rows == 1 && ytb_int(&t, 0, ytb_col(&t, "n")) == 3, "xlsx sheet by name");
    }
    /* phase 3: JPEG textures */
    if ((x = entry(&p, "img/grayj.ysptex", &e)) != NULL) {
        ypak_texture t;
        int bad = 0, xx, yy;
        CHECK(ypak_texture_view(x, e.size, &t, err, sizeof err) == 0 && t.format == YPAK_TEX_R8 && t.w == 20 && t.h == 12 && enc_is(t.enc, 1, 2, 3, 1),
              "gray JPEG texture: %s", err);
        for (yy = 0; yy < 12 && t.data; yy++)
            for (xx = 0; xx < 20; xx++) bad += t.data[yy * 20 + xx] != jpeg_val(0, xx / 8, yy / 8, 0);
        CHECK(bad == 0, "gray JPEG: every texel is its block's value (%d differ)", bad);
    }
    if ((x = entry(&p, "img/photo.ysptex", &e)) != NULL) {
        ypak_texture t;
        CHECK(ypak_texture_view(x, e.size, &t, err, sizeof err) == 0 && t.format == YPAK_TEX_RGBA8 && t.w == 40 && t.h == 24 &&
              enc_is(t.enc, 1, 2, 3, 2) && t.data[3] == 255, "4:2:0 JPEG with EXIF and an sRGB profile: %s", err);
    }
    if ((x = entry(&p, "img/flat.ysptex", &e)) != NULL) {
        ypak_texture t;
        static uint8_t dec[24 * 16 * 4];
        int bad = 0, k;
        CHECK(ypak_texture_view(x, e.size, &t, err, sizeof err) == 0 && t.compression == YPAK_TEX_QOI && ypak_qoi_decode(&t, dec, sizeof dec, err, sizeof err) == 0,
              "progressive JPEG as QOI: %s", err);
        for (k = 0; k < 24 * 16; k++) {
            int v = jpeg_val(0, (k % 24) / 8, (k / 24) / 8, 1);
            bad += dec[4 * k] != v || dec[4 * k + 1] != v || dec[4 * k + 2] != v || dec[4 * k + 3] != 255;
        }
        CHECK(bad == 0, "progressive JPEG without chroma: R = G = B = Y (%d texels differ)", bad);
    }
    if ((x = entry(&p, "img/icc.ysptex", &e)) != NULL) {
        ypak_texture t;
        CHECK(ypak_texture_view(x, e.size, &t, err, sizeof err) == 0 && memcmp(t.data, g_rgba, sizeof g_rgba) == 0, "PNG with an sRGB profile");
    }
    /* phase 3: frame sequences, unchanged, streamed */
    {
        static const char* const seqs[2] = { "video/bars.yspvseq", "video/yuv.yspvseq" };
        int k;
        for (k = 0; k < 2; k++)
            if ((x = entry(&p, seqs[k], &e)) != NULL) {
                char src[256];
                size_t n;
                uint8_t* s;
                yvid_desc vd;
                yvid_info vi;
                snprintf(src, sizeof src, W "/%s", seqs[k]);
                s = read_all(src, &n);
                CHECK(e.kind == YPAK_KIND_FRAMESEQ && (e.flags & YPAK_F_STREAM) && s && n == (size_t)e.size && memcmp(s, x, n) == 0, "%s unchanged", seqs[k]);
                memset(&vd, 0, sizeof vd);
                vd.data = x;
                vd.size = (size_t)e.size;
                CHECK(yvid_probe(&vd, &vi, err, sizeof err) == 0 && vi.frames == (k ? 2 : 3) && vi.w == 16, "%s probes: %s", seqs[k], err);
                free(s);
            }
    }
    ypak_close(&p);
}

/* Every texture of the corpus into ygfx_texture() as stored, the
 * documented load path (docs/pack.md 6), on the null backend: the encoding
 * bytes go in unchanged. BT.709 primaries need a calibration with
 * chromaticities: refused without one, taken with one. */
static void gfx_round_trip(const char* path) {
    static const char* const names[5] = { "img/gray.ysptex", "img/gray16.ysptex", "img/rgb16.ysptex", "img/rgba.ysptex", "img/rgba_raw.ysptex" };
    static const float xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };
    static uint8_t dec[sizeof g_rgba];
    static yscr_screen scr;
    static ygfx_gfx g;
    static ycol_cal cal;
    yscr_desc sd;
    ygfx_desc gd;
    ypak_pack p;
    ypak_desc d;
    char err[512];
    int cal_on, i, made = 0;
    memset(&d, 0, sizeof d);
    d.path = path;
    d.verify = YPAK_VERIFY_OPEN;
    memset(&sd, 0, sizeof sd);
    sd.backend = YSCR_BACKEND_SIM;
    sd.sim_period_ns = 1000000;
    CHECK(ycol_cal_nominal(&cal, xy, 80.0f, 2.2) == 0, "nominal calibration");
    if (!ypak_open(&p, &d) || !yscr_open(&scr, &sd)) { CHECK(0, "round trip: open"); return; }
    for (cal_on = 0; cal_on < 2; cal_on++) {
        memset(&gd, 0, sizeof gd);
        gd.screen = &scr;
        gd.cal = cal_on ? &cal : NULL;
        if (!ygfx_open(&g, &gd)) { CHECK(0, "round trip: ygfx_open"); break; }
        for (i = 0; i < 5; i++) {
            ypak_entry e;
            ypak_texture t;
            ygfx_texture_desc td;
            ygfx_tex x;
            const void* v = ypak_find(&p, names[i], &e) == 0 ? ypak_data(&p, &e) : NULL;
            if (!v || ypak_texture_view(v, e.size, &t, err, sizeof err) != 0) { CHECK(0, "round trip: %s", names[i]); continue; }
            memset(&td, 0, sizeof td);
            td.w = (int32_t)t.w; td.h = (int32_t)t.h; td.format = (ygfx_format)t.format; td.data = t.data;
            memcpy(&td.enc, t.enc, sizeof td.enc);
            if (t.compression == YPAK_TEX_QOI) {
                CHECK(ypak_qoi_decode(&t, dec, sizeof dec, err, sizeof err) == 0, "round trip qoi: %s", err);
                td.data = dec;
            }
            x = ygfx_texture(&g, &td);
            if (i == 3 && !cal_on)
                CHECK(x.id == 0 && strstr(ygfx_error(&g), "calibration") != NULL, "BT.709 without a calibration refused: %s", ygfx_error(&g));
            else
                CHECK(x.id != 0, "%s into ygfx_texture()%s: %s", names[i], cal_on ? " with a calibration" : "", ygfx_error(&g));
            if (x.id) { made++; ygfx_texture_free(&g, x); }
        }
        ygfx_close(&g);
    }
    CHECK(made == 9, "round trip: %d of 9 textures made", made);
    yscr_close(&scr);
    ypak_close(&p);
}

static void expect_refusal(const char* label, const char* resources, const char* says, int priv) {
    char text[4096], err[512];
    int rc;
    snprintf(text, sizeof text,
             "{\"format\": \"ysp-pack-source\", \"version\": 1, \"audio\": {\"rate\": 48000, \"channels\": 2},%s \"resources\": [%s]}",
             priv ? " \"private\": true," : "", resources);
    rc = build_text(text, W "/refused.ysppak", err, sizeof err);
    CHECK(rc < 0 && strstr(err, says) != NULL, "%s: rc %d, '%s' (wanted '%s')", label, rc, err, says);
    CHECK(fopen(W "/refused.ysppak", "rb") == NULL, "%s: a failed build left a file", label);
}

static void test_refusals(void) {
    char err[512];
    expect_refusal("unknown key", "{\"kind\": \"file\", \"source\": \"readme.txt\", \"colour\": \"red\"}", "unknown key 'colour'", 0);
    expect_refusal("real as number", "{\"kind\": \"artwork\", \"source\": \"art/fixation.svg\", \"tol\": 0.5}", "write a real value as a string", 0);
    expect_refusal("duplicate key", "{\"kind\": \"file\", \"kind\": \"file\", \"source\": \"readme.txt\"}", "duplicate key", 0);
    expect_refusal("case collision", "{\"kind\": \"file\", \"name\": \"a.txt\", \"source\": \"readme.txt\"}, {\"kind\": \"file\", \"name\": \"A.TXT\", \"source\": \"readme.txt\"}",
                   "without regard to case", 0);
    expect_refusal("reserved", "{\"kind\": \"file\", \"name\": \"ysp/x\", \"source\": \"readme.txt\"}", "reserved", 0);
    expect_refusal("not portable", "{\"kind\": \"file\", \"name\": \"a b.txt\", \"source\": \"readme.txt\"}", "not a portable entry name", 0);
    expect_refusal("device name", "{\"kind\": \"file\", \"name\": \"data/con.txt\", \"source\": \"readme.txt\"}", "not a portable entry name", 0);
    expect_refusal("dotdot source", "{\"kind\": \"file\", \"name\": \"x.txt\", \"source\": \"../readme.txt\"}", "not a relative path", 0);
    expect_refusal("missing source", "{\"kind\": \"file\", \"source\": \"nothing.txt\"}", "cannot open", 0);
    expect_refusal("rate", "{\"kind\": \"audio\", \"source\": \"audio/at44.wav\"}", "44100 Hz; the project rate is 48000", 0);
    expect_refusal("nan", "{\"kind\": \"audio\", \"source\": \"audio/nan.wav\"}", "not a number", 0);
    expect_refusal("16-bit color", "{\"kind\": \"texture\", \"source\": \"img/rgb16.png\"}", "rgba32f", 0);
    expect_refusal("16-bit encoding", "{\"kind\": \"texture\", \"source\": \"img/gray16.png\"}", "no default encoding", 0);
    expect_refusal("iccp", "{\"kind\": \"texture\", \"source\": \"img/iccp.png\"}", "ICC profile", 0);
    expect_refusal("narrowing", "{\"kind\": \"texture\", \"source\": \"img/rgba.png\", \"format\": \"r8\"}", "would drop", 0);
    expect_refusal("rgb srgb primaries", "{\"kind\": \"texture\", \"source\": \"img/rgba.png\"}", "need \"primaries\"", 0);
    expect_refusal("gray primaries", "{\"kind\": \"texture\", \"source\": \"img/gray.png\", \"primaries\": \"device\"}", "a gray texture is the display's white", 0);
    expect_refusal("linear primaries", "{\"kind\": \"texture\", \"source\": \"img/rgba.png\", \"encoding\": \"linear\", \"primaries\": \"bt709\"}",
                   "linear values carry no encoding", 0);
    expect_refusal("device primaries", "{\"kind\": \"texture\", \"source\": \"img/rgba.png\", \"encoding\": \"device\", \"primaries\": \"device\"}",
                   "device codes are the display's", 0);
    expect_refusal("primaries name", "{\"kind\": \"texture\", \"source\": \"img/rgba.png\", \"primaries\": \"p3\"}", "primaries must be bt709 or device", 0);
    expect_refusal("r16ui srgb", "{\"kind\": \"texture\", \"source\": \"img/gray16.png\", \"encoding\": \"srgb\"}", "r16ui holds linear values", 0);
    expect_refusal("qoi gray", "{\"kind\": \"texture\", \"source\": \"img/gray.png\", \"compression\": \"qoi\"}", "QOI is for rgba8", 0);
    expect_refusal("restricted font", "{\"kind\": \"font\", \"source\": \"fonts/restricted.ttf\"}", "forbids embedding", 0);
    expect_refusal("shader mode", "{\"kind\": \"shader\", \"source\": \"shaders/plaid.glsl\", \"mode\": \"blend\"}", "mode must be", 0);
    expect_refusal("csv", "{\"kind\": \"table\", \"source\": \"readme.txt\", \"types\": {\"A\": \"integer\"}}", "", 0);
    expect_refusal("curveset from", "{\"kind\": \"curveset\", \"from\": \"readme.txt\"}, {\"kind\": \"file\", \"source\": \"readme.txt\"}", "not a font entry", 0);
    expect_refusal("kind", "{\"kind\": \"glyphs\", \"source\": \"readme.txt\"}", "not one this tool builds", 0);
    expect_refusal("alpha", "{\"kind\": \"alpha\", \"name\": \"a.yspalpha\"}", "alpha is not built yet", 0);
    expect_refusal("video index", "{\"kind\": \"video_index\", \"name\": \"a.yspvi\"}", "made with its video", 0);
    /* phase 3 refusals: workbooks */
    {
        static const char* const bad_cells[] = {
            "<row r=\"2\"><c r=\"A2\" t=\"s\"><v>4</v></c><c r=\"B2\" s=\"1\"><v>45000</v></c></row>",
            "<row r=\"2\"><c r=\"A2\" t=\"s\"><v>4</v></c><c r=\"B2\" s=\"2\"><v>45000</v></c></row>",
            "<row r=\"2\"><c r=\"A2\" t=\"s\"><v>4</v></c><c r=\"B2\"><f>1+1</f></c></row>",
            "<row r=\"2\"><c r=\"A2\" t=\"s\"><v>4</v></c><c r=\"B2\" t=\"e\"><v>#DIV/0!</v></c></row>",
            "<row r=\"2\"><c r=\"A2\" t=\"s\"><v>4</v></c><c r=\"G2\"><v>1</v></c></row>",
            "<row r=\"2\"><c r=\"A2\" t=\"s\"><v>99</v></c></row>",
            "<row r=\"3\"><c r=\"A3\" t=\"s\"><v>4</v></c></row><row r=\"2\"><c r=\"A2\" t=\"s\"><v>4</v></c></row>",
        };
        static const char* const says[] = { "is a date or a time", "is a date or a time", "no saved value", "holds the error #DIV/0!",
                                            "cell G2 has a value, but its column has no name", "shared string '99'", "out of order" };
        int k;
        char res[256];
        for (k = 0; k < 7; k++) {
            char sheet[4096], path[256];
            snprintf(sheet, sizeof sheet, "%s%s</sheetData></worksheet>", XL_SHEET_HEAD, bad_cells[k]);
            snprintf(path, sizeof path, W "/tables/bad%d.xlsx", k);
            xlsx_file(path, sheet);
            snprintf(res, sizeof res, "{\"kind\": \"table\", \"name\": \"t.pstb\", \"source\": \"tables/bad%d.xlsx\"}", k);
            expect_refusal(says[k], res, says[k], 0);
        }
        {
            static const uint8_t ole[16] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
            write_file(W "/tables/ole.xlsx", ole, sizeof ole);
        }
        expect_refusal("encrypted workbook", "{\"kind\": \"table\", \"name\": \"t.pstb\", \"source\": \"tables/ole.xlsx\"}", "password-protected", 0);
        expect_refusal("not a zip", "{\"kind\": \"table\", \"name\": \"t.pstb\", \"source\": \"readme.xlsx\"}", "cannot open", 0);
        write_file(W "/tables/text.xlsx", "hello", 5);
        expect_refusal("not a zip", "{\"kind\": \"table\", \"name\": \"t.pstb\", \"source\": \"tables/text.xlsx\"}", "not a zip", 0);
        expect_refusal("no such sheet", "{\"kind\": \"table\", \"source\": \"tables/cond.xlsx\", \"sheet\": \"Nope\"}",
                       "no worksheet named 'Nope'; the workbook has 'Conditions', 'Sheet2'", 0);
        expect_refusal("delimiter on a workbook", "{\"kind\": \"table\", \"source\": \"tables/cond.xlsx\", \"delimiter\": \";\"}", "not a workbook", 0);
        expect_refusal("sheet on a CSV", "{\"kind\": \"table\", \"source\": \"conditions.csv\", \"sheet\": \"A\"}", "for .xlsx workbooks", 0);
        /* a damaged deflate stream or CRC */
        {
            size_t n;
            uint8_t* d = read_all(W "/tables/cond.xlsx", &n);
            if (d) {
                size_t at;
                /* the sheet's data: the last local entry before Sheet2's; flip a byte inside it */
                for (at = 0; at + 24 < n && memcmp(d + at, "xl/worksheets/sheet1.xml", 24) != 0; at++) {}
                CHECK(at + 24 < n, "find the sheet in the zip");
                d[at + 24 + 40] ^= 0x10;
                write_file(W "/tables/damaged.xlsx", d, n);
                free(d);
                expect_refusal("damaged sheet", "{\"kind\": \"table\", \"name\": \"t.pstb\", \"source\": \"tables/damaged.xlsx\"}", "sheet1.xml", 0);
            }
        }
    }
    /* phase 3 refusals: images */
    {
        bb app;
        memset(&app, 0, sizeof app);
        app_exif(&app, 6);
        jpeg_file(W "/img/turned.jpg", 16, 16, 1, 1, 0xC0, 8, 0, &app);
        app.n = 0;
        app_icc(&app, 0, 1);
        jpeg_file(W "/img/panel.jpg", 16, 16, 3, 1, 0xC0, 8, 0, &app);
        free(app.d);
        jpeg_file(W "/img/cmyk.jpg", 16, 16, 4, 1, 0xC0, 8, 0, NULL);
        jpeg_file(W "/img/arith.jpg", 16, 16, 1, 1, 0xC9, 8, 0, NULL);
        jpeg_file(W "/img/twelve.jpg", 16, 16, 1, 1, 0xC1, 12, 0, NULL);
        make_png(W "/img/panel.png", 17, 9, 0, 8, g_gray, 3);
        expect_refusal("exif orientation", "{\"kind\": \"texture\", \"source\": \"img/turned.jpg\"}", "EXIF orientation is 6", 0);
        expect_refusal("jpeg icc", "{\"kind\": \"texture\", \"source\": \"img/panel.jpg\", \"primaries\": \"device\"}", "primaries are not sRGB's", 0);
        expect_refusal("png icc", "{\"kind\": \"texture\", \"source\": \"img/panel.png\"}", "primaries are not sRGB's", 0);
        expect_refusal("cmyk", "{\"kind\": \"texture\", \"source\": \"img/cmyk.jpg\"}", "CMYK", 0);
        expect_refusal("arithmetic", "{\"kind\": \"texture\", \"source\": \"img/arith.jpg\"}", "arithmetic coding", 0);
        expect_refusal("12-bit", "{\"kind\": \"texture\", \"source\": \"img/twelve.jpg\"}", "12-bit samples", 0);
        expect_refusal("jpeg r8 from color", "{\"kind\": \"texture\", \"source\": \"img/photo.jpg\", \"format\": \"r8\"}", "would drop or round", 0);
        expect_refusal("jpeg rg8", "{\"kind\": \"texture\", \"source\": \"img/grayj.jpg\", \"format\": \"rg8\"}", "does not hold a JPEG's values", 0);
        expect_refusal("jpeg primaries", "{\"kind\": \"texture\", \"source\": \"img/photo.jpg\"}", "need \"primaries\"", 0);
    }
    /* phase 3 refusals: video and frame sequences */
    {
        size_t n;
        uint8_t* d = read_all(W "/video/yuv.yspvseq", &n);
        if (d) {
            d[4096 + 64 + 5] ^= 0x01;   /* frame 0's texels */
            write_file(W "/video/damaged.yspvseq", d, n);
            d[4096 + 64 + 5] ^= 0x01;
            write_file(W "/video/short.yspvseq", d, n - 24);
            free(d);
        }
        expect_refusal("damaged frame", "{\"kind\": \"frameseq\", \"source\": \"video/damaged.yspvseq\"}", "frame 0 does not match its hash", 0);
        expect_refusal("short sequence", "{\"kind\": \"frameseq\", \"source\": \"video/short.yspvseq\"}", "the index does not end the file", 0);
        expect_refusal("not a sequence", "{\"kind\": \"frameseq\", \"source\": \"readme.txt\"}", "not a ysp/video.h frame sequence", 0);
        expect_refusal("video name", "{\"kind\": \"video\", \"source\": \"readme.txt\"}", "ends in .mp4 or .mpg", 0);
        expect_refusal("video bytes", "{\"kind\": \"video\", \"name\": \"v.mp4\", \"source\": \"readme.txt\"}", "neither MP4 nor MPEG-1", 0);
        expect_refusal("video color", "{\"kind\": \"video\", \"name\": \"v.mp4\", \"source\": \"readme.txt\", \"color\": {\"matrix\": \"bt999\"}}",
                       "color matrix 'bt999'", 0);
        expect_refusal("ffmpeg_input alone", "{\"kind\": \"video\", \"name\": \"v.mp4\", \"source\": \"readme.txt\", \"ffmpeg_input\": [\"-f\"]}",
                       "'ffmpeg_input' needs 'ffmpeg'", 0);
        {
            static const char t[] = "{\"format\": \"ysp-pack-source\", \"version\": 1, \"resources\": [{\"kind\": \"video\", \"name\": \"v.mp4\", "
                                    "\"source\": \"readme.txt\", \"ffmpeg\": [\"-c:v\", \"libx264\"]}]}";
            ypt_options o;
            memset(&o, 0, sizeof o);
            o.ffmpeg = "ypak-test-no-such-ffmpeg";
            CHECK(ypt_build_text(t, strlen(t), W, W "/refused.ysppak", &o, err, sizeof err) < 0 && strstr(err, "cannot run"), "no ffmpeg: %s", err);
        }
    }
    /* open question 7's default: a restricted font is accepted in a private pack */
    {
        static const char t[] = "{\"format\": \"ysp-pack-source\", \"version\": 1, \"private\": true, \"resources\": "
                                "[{\"kind\": \"font\", \"source\": \"fonts/restricted.ttf\"}, {\"kind\": \"font\", \"source\": \"fonts/editable.ttf\"}]}";
        CHECK(build_text(t, W "/private.ysppak", err, sizeof err) == 0, "private pack: %s", err);
        CHECK(ypt_verify(W "/private.ysppak", NULL, err, sizeof err) == 0, "private pack verifies: %s", err);
    }
#ifdef YPT_NO_LAYOUT
    /* the core tool names what it lacks */
    expect_refusal("runs in the core tool",
                   "{\"kind\": \"font\", \"source\": \"fonts/box.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/box.ttf\"}, {\"kind\": \"glyphruns\", "
                   "\"name\": \"t.ysprun\", \"fonts\": [\"fonts/box.ttf\"], \"blocks\": [{\"key\": \"a\", \"text\": \"hi\", \"size\": \"10\"}]}",
                   "glyph runs need the layout library", 0);
#else
    expect_refusal("runs without set",
                   "{\"kind\": \"font\", \"source\": \"fonts/box.ttf\"}, {\"kind\": \"glyphruns\", \"name\": \"t.ysprun\", \"fonts\": [\"fonts/box.ttf\"], "
                   "\"blocks\": [{\"key\": \"a\", \"text\": \"hi\", \"size\": \"10\"}]}", "no curveset entry", 0);
    expect_refusal("block without size",
                   "{\"kind\": \"font\", \"source\": \"fonts/box.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/box.ttf\"}, {\"kind\": \"glyphruns\", "
                   "\"name\": \"t.ysprun\", \"fonts\": [\"fonts/box.ttf\"], \"blocks\": [{\"key\": \"a\", \"text\": \"hi\"}]}",
                   "block 0 needs 'key', 'text' and 'size'", 0);
    expect_refusal("missing glyph",
                   "{\"kind\": \"font\", \"source\": \"fonts/box.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/box.ttf\"}, {\"kind\": \"glyphruns\", "
                   "\"name\": \"t.ysprun\", \"fonts\": [\"fonts/box.ttf\"], \"blocks\": [{\"key\": \"a\", \"text\": \"mail@example\", \"size\": \"10\"}]}",
                   "'t.ysprun' block 'a': no font in the list maps 1 character (script Latn: '@' U+0040); add a font that has it", 0);
    /* glyphs "used": .notdef and the glyphs of the text */
    {
        static const char t[] = "{\"format\": \"ysp-pack-source\", \"version\": 1, \"resources\": ["
                                "{\"kind\": \"font\", \"source\": \"fonts/box.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/box.ttf\", \"glyphs\": \"used\"},"
                                "{\"kind\": \"glyphruns\", \"name\": \"t.ysprun\", \"fonts\": [\"fonts/box.ttf\"], \"blocks\": [{\"key\": \"a\", \"text\": \"abba\", \"size\": \"10\"}]}]}";
        ypak_pack p;
        ypak_desc d;
        ypak_entry e;
        ypak_cset s;
        int rc = build_text(t, W "/used.ysppak", err, sizeof err), present = 0;
        uint32_t g;
        CHECK(rc == 0, "used glyphs: %s", err);
        memset(&d, 0, sizeof d);
        d.path = W "/used.ysppak";
        if (rc == 0 && ypak_open(&p, &d)) {
            const void* x = ypak_find(&p, "fonts/box.yspcset", &e) == 0 ? ypak_data(&p, &e) : NULL;
            if (x && ypak_cset_view(x, e.size, &s, err, sizeof err) == 0) {
                for (g = 0; g < s.n_glyphs; g++) if (s.words[8 + g]) present++;
                CHECK(present == 3 && !(s.flags & YPAK_CSET_ALL), "used glyphs: %d present", present);
            } else CHECK(0, "used cset view");
            ypak_close(&p);
        }
    }
#endif
}

static int same_file(const char* a, const char* b) {
    size_t na, nb;
    uint8_t* x = read_all(a, &na);
    uint8_t* y = read_all(b, &nb);
    int r = x && y && na == nb && memcmp(x, y, na) == 0;
    free(x); free(y);
    return r;
}

static void flip_copy(const char* src, const char* dst, long at) {
    size_t n;
    uint8_t* d = read_all(src, &n);
    if (!d) return;
    if (at < 0) at += (long)n;
    d[at] ^= 0x01;
    write_file(dst, d, n);
    free(d);
}

/* 1 when the pack's manifest names the layout library. */
static int names_layout(const char* path) {
    ypak_pack p;
    ypak_desc d;
    ypak_entry e;
    int r = -1;
    memset(&d, 0, sizeof d);
    d.path = path;
    if (!ypak_open(&p, &d)) return -1;
    if (ypak_find(&p, "ysp/manifest.json", &e) == 0) {
        const char* m = (const char*)ypak_data(&p, &e);
        char* z = (char*)malloc((size_t)e.size + 1);
        if (m && z) {
            memcpy(z, m, (size_t)e.size);
            z[e.size] = 0;
            r = strstr(z, "\"layout\": ") != NULL;
        }
        free(z);
    }
    ypak_close(&p);
    return r;
}

/* The opt-in resampling against the signal itself: a 997 Hz sine at
 * 44.1 kHz (32-bit float, amplitude 0.5) resampled to 48 kHz must be the
 * same sine at 48 kHz, away from the ends, where the filter has no data on
 * one side. libsamplerate's best converter states 97 dB of SNR. */
static void test_resample(void) {
    static const char t[] = "{\"format\": \"ysp-pack-source\", \"version\": 1, \"audio\": {\"rate\": 48000, \"channels\": 1}, \"resources\": ["
                            "{\"kind\": \"audio\", \"name\": \"sine48.wav\", \"source\": \"audio/sine441.wav\", \"resample\": \"sinc_best\"}]}";
    static float in[44100];
    bb b;
    char err[512];
    int i, rc;
    ypak_pack p;
    ypak_desc d;
    ypak_entry e;
    memset(&b, 0, sizeof b);
    for (i = 0; i < 44100; i++) in[i] = (float)(0.5 * sin(2.0 * 3.14159265358979323846 * 997.0 * i / 44100.0));
    bput(&b, "RIFF", 4); le32(&b, 36 + 4 * 44100); bput(&b, "WAVEfmt ", 8); le32(&b, 16); le16(&b, 3); le16(&b, 1); le32(&b, 44100);
    le32(&b, 44100 * 4); le16(&b, 4); le16(&b, 32); bput(&b, "data", 4); le32(&b, 4 * 44100); bput(&b, in, sizeof in);
    write_file(W "/audio/sine441.wav", b.d, b.n);
    free(b.d);
    rc = build_text(t, W "/sine.ysppak", err, sizeof err);
    CHECK(rc == 0, "resample: %s", err);
    memset(&d, 0, sizeof d);
    d.path = W "/sine.ysppak";
    if (rc == 0 && ypak_open(&p, &d)) {
        const uint8_t* x = (const uint8_t*)entry(&p, "sine48.wav", &e);
        yau_wav_desc wd;
        yau_wav_info wi;
        memset(&wd, 0, sizeof wd);
        wd.data = x;
        wd.size = (size_t)e.size;
        if (x && yau_wav_probe(&wd, &wi, err, sizeof err) == 0) {
            double worst = 0;
            int64_t k;
            for (k = 200; k < wi.frames - 200; k++) {
                float v;
                double want = 0.5 * sin(2.0 * 3.14159265358979323846 * 997.0 * (double)k / 48000.0);
                memcpy(&v, x + wi.data_offset + 4 * k, 4);
                if (fabs(v - want) > worst) worst = fabs(v - want);
            }
            CHECK(wi.rate == 48000 && wi.frames >= 47999 && wi.frames <= 48001 && worst < 1e-4,
                  "resampled sine: %lld frames, largest error %.2e (%.1f dB below the amplitude)", (long long)wi.frames, worst, 20 * log10(0.5 / worst));
            printf("tool_test: resampled sine, largest error %.2e (%.1f dB below the amplitude)\n", worst, 20 * log10(0.5 / worst));
        } else {
            CHECK(0, "resampled sine reads: %s", err);
        }
        ypak_close(&p);
    }
    expect_refusal("resample name", "{\"kind\": \"audio\", \"source\": \"audio/at44.wav\", \"resample\": \"linear\"}", "resample is \"sinc_best\"", 0);
}

/* The author's thread settings in an ffmpeg step are refused (decided
 * 2026-10-14, kept 2026-10-20): the tool sets the count, from "threads" or
 * the machine, and records it. No ffmpeg needed. */
static void test_ffmpeg_refusals(void) {
    expect_refusal("threads without ffmpeg", "{\"kind\": \"video\", \"name\": \"v.mpg\", \"source\": \"readme.txt\", \"threads\": 4}",
                   "'threads' needs 'ffmpeg'", 0);
    expect_refusal("threads 0", "{\"kind\": \"video\", \"name\": \"v.mpg\", \"source\": \"readme.txt\", \"ffmpeg\": [\"-q:v\", \"3\"], \"threads\": 0}",
                   "'threads' is 0; it is 1 to 64", 0);
    expect_refusal("threads 65", "{\"kind\": \"video\", \"name\": \"v.mpg\", \"source\": \"readme.txt\", \"ffmpeg\": [\"-q:v\", \"3\"], \"threads\": 65}",
                   "'threads' is 65; it is 1 to 64", 0);
    expect_refusal("threads text", "{\"kind\": \"video\", \"name\": \"v.mpg\", \"source\": \"readme.txt\", \"ffmpeg\": [\"-q:v\", \"3\"], \"threads\": \"4\"}",
                   "'threads' must be an integer", 0);
    expect_refusal("ffmpeg threads", "{\"kind\": \"video\", \"name\": \"v.mpg\", \"source\": \"readme.txt\", \"ffmpeg\": [\"-threads\", \"4\"]}",
                   "remove the thread setting from 'ffmpeg' (-threads)", 0);
    expect_refusal("x264 threads", "{\"kind\": \"video\", \"name\": \"v.mp4\", \"source\": \"readme.txt\", \"ffmpeg\": [\"-x264-params\", \"bframes=0:threads=8\"]}",
                   "(-x264-params bframes=0:threads=8)", 0);
    expect_refusal("x265 pools", "{\"kind\": \"video\", \"name\": \"v.mp4\", \"source\": \"readme.txt\", \"ffmpeg\": [\"-x265-params\", \"pools=4\"]}",
                   "(-x265-params pools=4)", 0);
    /* the CPU-independence flags (decided 2026-10-20): the author's
     * contrary settings refused, the encoder found by name or by default */
    expect_refusal("x265 info", "{\"kind\": \"video\", \"name\": \"v.mp4\", \"source\": \"readme.txt\", \"ffmpeg\": [\"-c:v\", \"libx265\", \"-x265-params\", \"bframes=0:info=1\"]}",
                   "remove info from 'ffmpeg' (-x265-params bframes=0:info=1)", 0);
    expect_refusal("x265 bare info", "{\"kind\": \"video\", \"name\": \"v.mp4\", \"source\": \"readme.txt\", \"ffmpeg\": [\"-c:v\", \"libx265\", \"-x265-params\", \"info\"]}",
                   "remove info from 'ffmpeg'", 0);
    expect_refusal("mpeg dct", "{\"kind\": \"video\", \"name\": \"v.mpg\", \"source\": \"readme.txt\", \"ffmpeg\": [\"-c:v\", \"mpeg1video\", \"-dct\", \"fastint\"]}",
                   "remove '-dct fastint' from 'ffmpeg'", 0);
    expect_refusal("mpeg2video", "{\"kind\": \"video\", \"name\": \"v.mpg\", \"source\": \"readme.txt\", \"ffmpeg\": [\"-c:v\", \"mpeg2video\"]}",
                   "mpeg2video cannot be packed", 0);
    expect_refusal("mpeg default encoder idct", "{\"kind\": \"video\", \"name\": \"v.mpg\", \"source\": \"readme.txt\", \"ffmpeg\": [\"-idct:v\", \"auto\"]}",
                   "remove '-idct:v auto' from 'ffmpeg'", 0);
    expect_refusal("mpeg bitexact off", "{\"kind\": \"video\", \"name\": \"v.mpg\", \"source\": \"readme.txt\", \"ffmpeg\": [\"-c:v\", \"mpeg1video\", \"-flags\", \"+cgop-bitexact\"]}",
                   "remove '-flags +cgop-bitexact' from 'ffmpeg'", 0);
}

/* --- tool v0.12.0: cancel and the entry cache (docs/pack.md 19) ----------------- */

static int exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* How often needle occurs in the file. */
static int count_text(const char* path, const char* needle) {
    size_t n, k = strlen(needle);
    uint8_t* d = read_all(path, &n);
    const char* at;
    int c = 0;
    if (!d) return -1;
    d[n] = 0;
    for (at = (const char*)d; (at = strstr(at, needle)) != NULL; at += k) c++;
    free(d);
    return c;
}

static int name_cmp(const void* a, const void* b) { return strcmp((const char*)a, (const char*)b); }

/* The files of dir whose names end in suffix ("" every file), sorted. */
static int list_dir(const char* dir, const char* suffix, char (*names)[96], int max) {
    int n = 0;
    size_t sl = strlen(suffix);
#if defined(_WIN32)
    char pat[1100];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        size_t l = strlen(fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (l < sl || strcmp(fd.cFileName + l - sl, suffix) || l >= 96 || n >= max) continue;
        snprintf(names[n++], 96, "%.95s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* dd = opendir(dir);
    struct dirent* de;
    if (!dd) return 0;
    while ((de = readdir(dd)) != NULL) {
        size_t l = strlen(de->d_name);
        if (de->d_name[0] == '.' || l < sl || strcmp(de->d_name + l - sl, suffix) || l >= 96 || n >= max) continue;
        snprintf(names[n++], 96, "%.95s", de->d_name);
    }
    closedir(dd);
#endif
    qsort(names, (size_t)n, 96, name_cmp);
    return n;
}

static void clear_dir(const char* dir) {
    char names[256][96], p[1200];
    int n = list_dir(dir, "", names, 256), i;
    for (i = 0; i < n; i++) {
        snprintf(p, sizeof p, "%.1000s/%.95s", dir, names[i]);
        remove(p);
    }
    RMDIR(dir);
}

static void set_mtime(const char* path, long long t) {
#if defined(_WIN32)
    struct _utimbuf u;
    u.actime = (time_t)t;
    u.modtime = (time_t)t;
    _utime(path, &u);
#else
    struct utimbuf u;
    u.actime = (time_t)t;
    u.modtime = (time_t)t;
    utime(path, &u);
#endif
}

static long long mtime_of(const char* path) {
#if defined(_WIN32)
    struct _stat64 st;
    return _stat64(path, &st) == 0 ? (long long)st.st_mtime : -1;
#else
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_mtime : -1;
#endif
}

static long long file_size(const char* path) {
    size_t n = 0;
    uint8_t* d = read_all(path, &n);
    free(d);
    return d ? (long long)n : -1;
}

/* A build of text with options; log and warn to files when named. */
static int build_with(const char* text, const char* out, const char* cache, int64_t cache_max, const char* log_path, const char* warn_path,
                      char* err, size_t cap) {
    ypt_options o;
    FILE* lf = log_path ? fopen(log_path, "w") : NULL;
    FILE* wf = warn_path ? fopen(warn_path, "w") : NULL;
    int rc;
    memset(&o, 0, sizeof o);
    o.cache = cache;
    o.cache_max = cache_max;
    o.log = lf;
    o.warn = wf;
    rc = ypt_build_text(text, strlen(text), W, out, &o, err, cap);
    if (lf) fclose(lf);
    if (wf) fclose(wf);
    return rc;
}

/* A cache record as the tool writes one (docs/pack.md 19), for the parse
 * checks and for records that are whole but wrong. */
typedef struct tpart { uint32_t flags, source; const char* deriv; const uint8_t* data; uint64_t size; } tpart;

static void tw32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void tw64(uint8_t* p, uint64_t v) { tw32(p, (uint32_t)v); tw32(p + 4, (uint32_t)(v >> 32)); }

/* The size and XXH64 fields again after an edit, so only the edit is wrong. */
static void rec_fix(uint8_t* d, size_t n) {
    tw64(d + 48, (uint64_t)n);
    tw64(d + 56, ypak_xxh64(d + 64, n - 64, 0));
}

/* The record's bytes (malloc'd) and their count in *n. */
static uint8_t* rec_make(const uint8_t key[32], const tpart* p, int np, size_t* n) {
    size_t at = 64 + 32 * (size_t)np, i, total = at;
    uint8_t* d;
    for (i = 0; i < (size_t)np; i++) total += ((strlen(p[i].deriv) + 15) & ~(size_t)15) + (((size_t)p[i].size + 15) & ~(size_t)15);
    d = (uint8_t*)calloc(1, total + 1);
    if (!d) return NULL;
    memcpy(d, "YSPCACHE", 8);
    tw32(d + 8, 1);
    tw32(d + 12, (uint32_t)np);
    memcpy(d + 16, key, 32);
    for (i = 0; i < (size_t)np; i++) {
        uint8_t* t = d + 64 + 32 * i;
        size_t dl = strlen(p[i].deriv);
        tw32(t, p[i].flags);
        tw32(t + 4, p[i].source);
        tw64(t + 8, dl);
        tw64(t + 16, p[i].size);
        memcpy(d + at, p[i].deriv, dl);
        at += (dl + 15) & ~(size_t)15;
        if (p[i].size) memcpy(d + at, p[i].data, (size_t)p[i].size);
        at += ((size_t)p[i].size + 15) & ~(size_t)15;
    }
    rec_fix(d, total);
    *n = total;
    return d;
}

/* ypt_cache_parse() on a good record and on each kind of damage. */
static void test_cache_parse(void) {
    static const uint8_t five[5] = { 1, 2, 3, 4, 5 };
    uint8_t key[32], other[32];
    tpart tp[2];
    ypt_cache_part got[4];
    size_t n, i;
    uint8_t* d;
    uint8_t* x;
    int r;
    for (i = 0; i < 32; i++) { key[i] = (uint8_t)(i * 7 + 1); other[i] = key[i]; }
    other[31] ^= 1;
    memset(tp, 0, sizeof tp);
    tp[0].source = 1;
    tp[0].deriv = "{\"op\": \"copy\"}";
    tp[1].flags = 1;
    tp[1].deriv = "{\"op\": \"index\", \"frames\": 3}";
    tp[1].data = five;
    tp[1].size = 5;
    d = rec_make(key, tp, 2, &n);
    if (!d) { CHECK(0, "out of memory"); return; }
    x = (uint8_t*)malloc(n + 64);
    if (!x) { free(d); CHECK(0, "out of memory"); return; }
    memset(got, 0, sizeof got);
    r = ypt_cache_parse(d, n, key, got, 4);
    CHECK(r == 2 && got[0].source == 1 && !got[0].data && got[0].size == 0 && got[0].deriv_len == strlen(tp[0].deriv) &&
          !memcmp(got[0].deriv, tp[0].deriv, got[0].deriv_len) && got[1].flags == 1 && !got[1].source && got[1].size == 5 &&
          got[1].data && !memcmp(got[1].data, five, 5) && got[1].deriv_len == strlen(tp[1].deriv),
          "cache record: two parts read back (%d)", r);
    CHECK(ypt_cache_parse(d, n, NULL, got, 4) == 2, "cache record: no key to compare");
    CHECK(ypt_cache_parse(d, n, other, got, 4) < 0, "cache record: another key refused");
    CHECK(ypt_cache_parse(d, n, key, got, 1) < 0, "cache record: more parts than the caller takes refused");
    CHECK(ypt_cache_parse(d, n - 1, key, got, 4) < 0, "cache record: one byte short refused");
    CHECK(ypt_cache_parse(d, 63, key, got, 4) < 0, "cache record: shorter than its header refused");
#define DAMAGE(what, edit, fix)                                           \
    do {                                                                  \
        size_t m = n;                                                     \
        int fx = fix;                                                     \
        memcpy(x, d, n);                                                  \
        edit;                                                             \
        if (fx) rec_fix(x, m);                                            \
        CHECK(ypt_cache_parse(x, m, key, got, 4) < 0, "cache record: %s refused", what); \
    } while (0)
    DAMAGE("another magic", x[0] = 'X', 0);
    DAMAGE("version 2", tw32(x + 8, 2), 0);
    DAMAGE("0 parts", tw32(x + 12, 0), 0);
    DAMAGE("5 parts", tw32(x + 12, 5), 0);
    DAMAGE("a size field one more", tw64(x + 48, (uint64_t)n + 1), 0);
    DAMAGE("a byte of the bytes changed", x[n - 16 + 2] ^= 0x10, 0);
    DAMAGE("the XXH64 changed", x[56] ^= 1, 0);
    DAMAGE("a reserved field set", tw64(x + 64 + 32 + 24, 1), 1);
    DAMAGE("a source mark of 2", tw32(x + 64 + 4, 2), 1);
    DAMAGE("a derivation past 1 MiB", tw64(x + 64 + 32 + 8, ((uint64_t)1 << 20) + 1), 1);
    DAMAGE("a derivation past the end", tw64(x + 64 + 32 + 8, (uint64_t)n), 1);
    DAMAGE("bytes past the end", tw64(x + 64 + 32 + 16, (uint64_t)n), 1);
    DAMAGE("padding not zero", x[n - 1] = 1, 1);
    DAMAGE("16 bytes after the last part", (memset(x + n, 0, 16), m = n + 16), 1);
    DAMAGE("a header of 0 parts and nothing else", (tw32(x + 12, 0), m = 64), 1);
#undef DAMAGE
    free(x);
    free(d);
    {   /* a part marked as the source's that holds bytes, whole otherwise */
        tpart sp = tp[1];
        sp.source = 1;
        d = rec_make(key, &sp, 1, &n);
        CHECK(d && ypt_cache_parse(d, n, key, got, 4) < 0, "cache record: a source part with bytes refused");
        free(d);
    }
    {   /* a derivation past 1 MiB, whole: refused before the JSON reader */
        size_t big = ((size_t)1 << 20) + 1, j;
        char* dv = (char*)malloc(big + 1);
        if (dv) {
            dv[0] = '{';
            for (j = 1; j + 1 < big; j++) dv[j] = ' ';
            dv[big - 1] = '}';
            dv[big] = 0;
            tp[0].source = 0;
            tp[0].deriv = dv;
            d = rec_make(key, tp, 1, &n);
            CHECK(d && ypt_cache_parse(d, n, key, got, 4) < 0, "cache record: a whole derivation of 1 MiB and a byte refused");
            tp[0].deriv = "{}";
            free(d);
            dv[big - 2] = '}';
            dv[big - 1] = 0;
            tp[0].deriv = dv;
            d = rec_make(key, tp, 1, &n);
            CHECK(d && ypt_cache_parse(d, n, key, got, 4) == 1, "cache record: a derivation of 1 MiB taken");
            free(d);
            free(dv);
        }
    }
}

static int g_calls, g_cancel_at;
static int cancel_at(void* ctx) { (void)ctx; return ++g_calls >= g_cancel_at; }

/* ypt_options.cancel between entries (designer step 6, decision 4). */
static void test_cancel(void) {
    char err[1024];
    ypt_options o;
    int rc;
    memset(&o, 0, sizeof o);
    o.cancel = cancel_at;
    remove(W "/canceled.ysppak");
    g_calls = 0;
    g_cancel_at = 2;
    rc = ypt_build_text(g_desc, strlen(g_desc), W, W "/canceled.ysppak", &o, err, sizeof err);
    CHECK(rc == YPT_ERR_CANCELED && strstr(err, "canceled") && g_calls == 2, "a cancel before the second resource: %d, %d calls: %s", rc, g_calls, err);
    CHECK(!exists(W "/canceled.ysppak") && !exists(W "/canceled.ysppak.tmp"), "a canceled build leaves no pack and no temporary file");
    g_calls = 0;
    g_cancel_at = 1;
    rc = ypt_build_text(g_desc, strlen(g_desc), W, W "/canceled.ysppak", &o, err, sizeof err);
    CHECK(rc == YPT_ERR_CANCELED && g_calls == 1 && !exists(W "/canceled.ysppak") && !exists(W "/canceled.ysppak.tmp"),
          "a cancel before the first resource: %d, %d calls", rc, g_calls);
    g_calls = 0;
    g_cancel_at = 1 << 30;
    rc = ypt_build_text(g_desc, strlen(g_desc), W, W "/not_canceled.ysppak", &o, err, sizeof err);
    /* every entry but the manifest and the chunk table, and the glyph runs' derivation */
    CHECK(rc == 0 && same_file(W "/corpus.ysppak", W "/not_canceled.ysppak") && g_calls == (int)N_ENTRIES - 1,
          "a cancel that never comes: the same bytes, read before each entry and the glyph runs' derivation (%d calls): %s", g_calls, err);
}

typedef struct tjob { const char* out; const char* cache; int rc; char err[512]; } tjob;
#if defined(_WIN32)
static unsigned __stdcall build_job(void* p) {
#else
static void* build_job(void* p) {
#endif
    tjob* j = (tjob*)p;
    j->rc = build_with(g_desc_core, j->out, j->cache, 0, NULL, NULL, j->err, sizeof j->err);
    return 0;
}

/* Two builds at once into one cold cache (the check pack's and Build's), of
 * the description without glyph runs: Skribidi's font collection counter
 * is a known race between two layouts (docs/designer.md, "The build
 * thread"), and this test is the cache's. */
static void two_builds(const char* cache) {
    tjob j[2];
    int k;
    char err[1024];
    CHECK(build_text(g_desc_core, W "/thread_ref.ysppak", err, sizeof err) == 0, "the core description: %s", err);
    memset(j, 0, sizeof j);
    j[0].out = W "/thread0.ysppak";
    j[1].out = W "/thread1.ysppak";
    j[0].cache = j[1].cache = cache;
#if defined(_WIN32)
    {
        HANDLE h[2];
        for (k = 0; k < 2; k++) h[k] = (HANDLE)_beginthreadex(NULL, 0, build_job, &j[k], 0, NULL);
        for (k = 0; k < 2; k++) if (h[k]) { WaitForSingleObject(h[k], INFINITE); CloseHandle(h[k]); } else build_job(&j[k]);
    }
#else
    {
        pthread_t t[2];
        int ok[2];
        for (k = 0; k < 2; k++) ok[k] = pthread_create(&t[k], NULL, build_job, &j[k]) == 0;
        for (k = 0; k < 2; k++) if (ok[k]) pthread_join(t[k], NULL); else build_job(&j[k]);
    }
#endif
    for (k = 0; k < 2; k++)
        CHECK(j[k].rc == 0 && same_file(W "/thread_ref.ysppak", j[k].out), "two builds at once into one cache, build %d: the same bytes: %s", k, j[k].err);
}

/* The entry cache on the corpus (decision 17): the pack's bytes with no
 * cache, a cold one and a warm one are the same; every damaged or wrong
 * record is a miss; the key holds the source, the resource and the
 * project's audio; the trim. */
#define CDIR W "/cache/a/b"
#define N_RECORDS 13   /* 9 textures; u8, s32 and f64 converted; at44 resampled */
static void test_cache(void) {
    char err[1024], names[64][96], p[1200];
    int nr, k, rc, hits;
    long long now = (long long)time(NULL);
    clear_dir(CDIR);
    RMDIR(W "/cache/a");
    RMDIR(W "/cache");
    rc = build_with(g_desc, W "/c_cold.ysppak", CDIR, 0, W "/c_cold.log", NULL, err, sizeof err);
    CHECK(rc == 0 && same_file(W "/corpus.ysppak", W "/c_cold.ysppak"), "a cold cache (its folder made with its parents): the same bytes: %s", err);
    CHECK(count_text(W "/c_cold.log", "(cache)") == 0, "a cold cache: no entry from it");
    nr = list_dir(CDIR, ".yspce", names, 64);
    CHECK(nr == N_RECORDS, "a cold cache: %d records written, want %d (textures and converted or resampled audio)", nr, N_RECORDS);
    CHECK(list_dir(CDIR, ".tmp", names + nr, 64 - nr) == 0, "a cold cache: no temporary file left");
    rc = build_with(g_desc, W "/c_warm.ysppak", CDIR, 0, W "/c_warm.log", NULL, err, sizeof err);
    hits = count_text(W "/c_warm.log", "(cache)");
    CHECK(rc == 0 && same_file(W "/corpus.ysppak", W "/c_warm.ysppak") && hits == N_RECORDS, "a warm cache: the same bytes, %d entries from it: %s", hits, err);
    /* every record damaged one way: each a miss, the same bytes, and each
     * written again by that build */
    for (k = 0; k < nr; k++) {
        size_t n = 0, m = 0;
        uint8_t* d;
        uint8_t* r2 = NULL;
        ypt_cache_part cp[4];
        tpart tp;
        snprintf(p, sizeof p, CDIR "/%.95s", names[k]);
        d = read_all(p, &n);
        if (!d || ypt_cache_parse(d, n, NULL, cp, 4) != 1) { CHECK(0, "record %s reads back", names[k]); free(d); continue; }
        memset(&tp, 0, sizeof tp);
        tp.flags = cp[0].flags;
        tp.deriv = "{\"op\": \"copy\"}";
        tp.data = cp[0].data;
        tp.size = cp[0].size;
        switch (k % 7) {
        case 0: d[n / 2] ^= 0x40; write_file(p, d, n); break;                  /* a flipped bit */
        case 1: write_file(p, d, n / 2); break;                                 /* cut short */
        case 2: write_file(p, d, 0); break;                                     /* empty */
        case 3: {                                                               /* another record's bytes */
            char q[1200];
            size_t on = 0;
            uint8_t* od;
            snprintf(q, sizeof q, CDIR "/%.95s", names[(k + 1) % nr]);
            od = read_all(q, &on);
            if (od) write_file(p, od, on);
            free(od);
        } break;
        case 4: tp.flags |= 0x100; r2 = rec_make(d + 16, &tp, 1, &m); break;    /* flags the entry cannot have */
        case 5: tp.source = 1; tp.size = 0; r2 = rec_make(d + 16, &tp, 1, &m); break;   /* marked as the source's bytes */
        default: tp.deriv = "[1]"; r2 = rec_make(d + 16, &tp, 1, &m); break;    /* a derivation that is not an object */
        }
        if (r2) write_file(p, r2, m);
        free(r2);
        free(d);
    }
    rc = build_with(g_desc, W "/c_bad.ysppak", CDIR, 0, W "/c_bad.log", NULL, err, sizeof err);
    hits = count_text(W "/c_bad.log", "(cache)");
    CHECK(rc == 0 && same_file(W "/corpus.ysppak", W "/c_bad.ysppak") && hits == 0, "damaged and wrong records: misses (%d hits), the same bytes: %s", hits, err);
    rc = build_with(g_desc, W "/c_again.ysppak", CDIR, 0, W "/c_again.log", NULL, err, sizeof err);
    hits = count_text(W "/c_again.log", "(cache)");
    CHECK(rc == 0 && same_file(W "/corpus.ysppak", W "/c_again.ysppak") && hits == N_RECORDS, "the damaged records were written again: %d hits", hits);
    /* the key: a resource's option, a source's bytes, the project's audio */
    {
        static const char from[] = "{\"kind\": \"texture\", \"source\": \"img/gray.png\"}";
        const char* at = strstr(g_desc, from);
        char* v = (char*)malloc(strlen(g_desc) + 64);
        if (at && v) {
            snprintf(v, strlen(g_desc) + 64, "%.*s{\"kind\": \"texture\", \"source\": \"img/gray.png\", \"encoding\": \"linear\"}%s", (int)(at - g_desc), g_desc,
                     at + strlen(from));
            CHECK(build_text(v, W "/c_opt_ref.ysppak", err, sizeof err) == 0 &&
                  build_with(v, W "/c_opt.ysppak", CDIR, 0, NULL, NULL, err, sizeof err) == 0 && same_file(W "/c_opt_ref.ysppak", W "/c_opt.ysppak"),
                  "another option of a resource: a miss, the bytes without a cache: %s", err);
        } else {
            CHECK(0, "the gray texture in the description");
        }
        free(v);
    }
    {
        size_t n = 0;
        uint8_t* keep = read_all(W "/img/gray.png", &n);
        static uint8_t g2[17 * 9];
        size_t i;
        for (i = 0; i < sizeof g2; i++) g2[i] = (uint8_t)(255 - i * 13);
        make_png(W "/img/gray.png", 17, 9, 0, 8, g2, 0);
        CHECK(build_text(g_desc, W "/c_src_ref.ysppak", err, sizeof err) == 0 &&
              build_with(g_desc, W "/c_src.ysppak", CDIR, 0, NULL, NULL, err, sizeof err) == 0 && same_file(W "/c_src_ref.ysppak", W "/c_src.ysppak") &&
              !same_file(W "/c_src.ysppak", W "/corpus.ysppak"),
              "a source with other bytes: a miss, the bytes without a cache: %s", err);
        if (keep) write_file(W "/img/gray.png", keep, n);
        free(keep);
    }
    {
        static const char a48[] = "{\"format\": \"ysp-pack-source\", \"version\": 1, \"audio\": {\"rate\": 48000, \"channels\": 2}, \"resources\": ["
                                  "{\"kind\": \"audio\", \"name\": \"a.wav\", \"source\": \"audio/at44.wav\", \"resample\": \"sinc_best\"}]}";
        static const char a96[] = "{\"format\": \"ysp-pack-source\", \"version\": 1, \"audio\": {\"rate\": 96000, \"channels\": 2}, \"resources\": ["
                                  "{\"kind\": \"audio\", \"name\": \"a.wav\", \"source\": \"audio/at44.wav\", \"resample\": \"sinc_best\"}]}";
        CHECK(build_with(a48, W "/c_a48.ysppak", CDIR, 0, NULL, NULL, err, sizeof err) == 0 && build_text(a96, W "/c_a96_ref.ysppak", err, sizeof err) == 0 &&
              build_with(a96, W "/c_a96.ysppak", CDIR, 0, NULL, NULL, err, sizeof err) == 0 && same_file(W "/c_a96_ref.ysppak", W "/c_a96.ysppak"),
              "another project rate: a miss, the bytes without a cache: %s", err);
    }
    /* the trim: least recently used first, a hit makes a record recent */
    {
        int i, recent = -1, n_recent = 0;
        long long total = 0;
        int64_t kept = -1;
        static const char gray[] = "{\"format\": \"ysp-pack-source\", \"version\": 1, \"audio\": {\"rate\": 48000, \"channels\": 2}, \"resources\": ["
                                   "{\"kind\": \"texture\", \"source\": \"img/gray.png\"}]}";
        nr = list_dir(CDIR, ".yspce", names, 64);
        for (i = 0; i < nr; i++) {
            snprintf(p, sizeof p, CDIR "/%.95s", names[i]);
            set_mtime(p, now - 5000 + i);
            total += file_size(p);
        }
        CHECK(ypt_cache_trim(CDIR, INT64_MAX, &kept, err, sizeof err) == 0 && kept == total && list_dir(CDIR, ".yspce", names, 64) == nr,
              "trim with room for all: every record kept (%lld of %lld bytes)", (long long)kept, total);
        rc = build_with(gray, W "/c_gray.ysppak", CDIR, 0, W "/c_gray.log", NULL, err, sizeof err);
        CHECK(rc == 0 && count_text(W "/c_gray.log", "(cache)") == 1, "the gray texture alone: a hit: %s", err);
        for (i = 0; i < nr; i++) {
            snprintf(p, sizeof p, CDIR "/%.95s", names[i]);
            if (mtime_of(p) > now - 60) { recent = i; n_recent++; }
        }
        CHECK(n_recent == 1, "a hit makes its record the most recent (%d records recent)", n_recent);
        snprintf(p, sizeof p, CDIR "/%064d.1-2.tmp", 0);
        write_file(p, "x", 1);
        set_mtime(p, now - 7200);
        snprintf(p, sizeof p, CDIR "/%064d.3-4.tmp", 0);
        write_file(p, "y", 1);
        write_file(CDIR "/keep.txt", "z", 1);
        set_mtime(CDIR "/keep.txt", now - 7200);
        if (recent >= 0) {
            long long rs;
            snprintf(p, sizeof p, CDIR "/%.95s", names[recent]);
            rs = file_size(p);
            CHECK(ypt_cache_trim(CDIR, rs, &kept, err, sizeof err) == 0 && kept == rs && list_dir(CDIR, ".yspce", names + 32, 32) == 1 &&
                  !strcmp(names[32], names[recent]),
                  "trim to one record's bytes: the most recent stays (%lld bytes kept)", (long long)kept);
        }
        snprintf(p, sizeof p, CDIR "/%064d.1-2.tmp", 0);
        CHECK(!exists(p), "trim: a temporary file older than an hour removed");
        snprintf(p, sizeof p, CDIR "/%064d.3-4.tmp", 0);
        CHECK(exists(p), "trim: a new temporary file kept (a build may be writing it)");
        remove(p);
        CHECK(ypt_cache_trim(CDIR, 0, &kept, err, sizeof err) == 0 && kept == 0 && list_dir(CDIR, ".yspce", names, 64) == 0 && exists(CDIR "/keep.txt"),
              "trim to 0: no record left, another file kept");
        remove(CDIR "/keep.txt");
        CHECK(ypt_cache_trim(W "/no-such-folder", 0, &kept, err, sizeof err) < 0 && strstr(err, "no-such-folder"), "trim of a folder that is not there: %s", err);
    }
    /* cache_max: the build trims after it writes */
    rc = build_with(g_desc, W "/c_max.ysppak", CDIR, 1, NULL, NULL, err, sizeof err);
    CHECK(rc == 0 && same_file(W "/corpus.ysppak", W "/c_max.ysppak") && list_dir(CDIR, ".yspce", names, 64) == 0,
          "cache_max of 1 byte: the build's records trimmed away: %s", err);
    rc = build_with(g_desc, W "/c_max2.ysppak", CDIR, (int64_t)1 << 40, NULL, NULL, err, sizeof err);
    CHECK(rc == 0 && list_dir(CDIR, ".yspce", names, 64) == N_RECORDS, "cache_max of 1 TiB: every record kept");
    /* a cache that cannot be made: a warning, the same bytes */
    rc = build_with(g_desc, W "/c_nodir.ysppak", W "/readme.txt/cache", 0, NULL, W "/c_nodir.warn", err, sizeof err);
    CHECK(rc == 0 && same_file(W "/corpus.ysppak", W "/c_nodir.ysppak") && count_text(W "/c_nodir.warn", "warning: the entry cache") == 1,
          "a cache folder that cannot be made: a warning and the same bytes: %s", err);
    /* two builds at once into one cold cache, then a warm build */
    clear_dir(W "/cache2");
    two_builds(W "/cache2");
    CHECK(list_dir(W "/cache2", ".yspce", names, 64) == N_RECORDS && list_dir(W "/cache2", ".tmp", names, 64) == 0,
          "two builds at once: %d records, no temporary file", list_dir(W "/cache2", ".yspce", names, 64));
    rc = build_with(g_desc_core, W "/c_two.ysppak", W "/cache2", 0, W "/c_two.log", NULL, err, sizeof err);
    CHECK(rc == 0 && same_file(W "/thread_ref.ysppak", W "/c_two.ysppak") && count_text(W "/c_two.log", "(cache)") == N_RECORDS,
          "after two builds at once: a warm build, the same bytes: %s", err);
}

/* 1 when the file holds the bytes of needle. */
static int has_bytes(const char* path, const char* needle) {
    size_t n, k = strlen(needle), i;
    uint8_t* d = read_all(path, &n);
    int r = 0;
    for (i = 0; d && i + k <= n && !r; i++) r = !memcmp(d + i, needle, k);
    free(d);
    return r;
}

static const char* g_watch;
static int g_saw;
static int cancel_when(void* ctx) {
    (void)ctx;
    if (!exists(g_watch)) return 0;
    g_saw++;
    return 1;
}

#if defined(_WIN32)
#define N_VIDEOS 2
#else
#define N_VIDEOS 1
#endif

/* Cancel and the entry cache with the ffmpeg step and the video index. */
static void test_video_cache(const char* t) {
    char err[1024], names[64][96];
    int rc, hits;
    {   /* a cancel set while ffmpeg runs (its output file exists): the step
         * goes to its end, then the build stops */
        ypt_options o;
        memset(&o, 0, sizeof o);
        o.cancel = cancel_when;
        g_watch = W "/cancel_ff.ysppak.tmp.v0.mpg";
        g_saw = 0;
        remove(g_watch);
        remove(W "/cancel_ff.ysppak");
        remove(W "/cancel_ff.ysppak.tmp");
        rc = ypt_build_text(t, strlen(t), W, W "/cancel_ff.ysppak", &o, err, sizeof err);
        CHECK(rc == YPT_ERR_CANCELED && g_saw >= 1, "a cancel during the ffmpeg step stops after it: %d, seen %d: %s", rc, g_saw, err);
        CHECK(!exists(W "/cancel_ff.ysppak") && !exists(W "/cancel_ff.ysppak.tmp") && !exists(g_watch) && !exists(W "/cancel_ff.ysppak.tmp.v0.yspvi"),
              "a cancel during the ffmpeg step: no pack, no temporary file");
    }
    clear_dir(W "/vcache");
    rc = build_with(t, W "/video_cold.ysppak", W "/vcache", 0, W "/video_cold.log", NULL, err, sizeof err);
    CHECK(rc == 0 && same_file(W "/video.ysppak", W "/video_cold.ysppak") && count_text(W "/video_cold.log", "(cache)") == 0 &&
          list_dir(W "/vcache", ".yspce", names, 64) == N_VIDEOS, "video, a cold cache: the same bytes, one record a video: %s", err);
    rc = build_with(t, W "/video_warm.ysppak", W "/vcache", 0, W "/video_warm.log", NULL, err, sizeof err);
    hits = count_text(W "/video_warm.log", "(cache)");
    CHECK(rc == 0 && same_file(W "/video.ysppak", W "/video_warm.ysppak") && hits == 2 * N_VIDEOS,
          "video, a warm cache: the same bytes, each video and its index from it (%d): %s", hits, err);
    {   /* another argument of the step: a miss */
        static const char from[] = "\"-q:v\", \"3\"";
        const char* at = strstr(t, from);
        char v[4096];
        if (at) {
            snprintf(v, sizeof v, "%.*s\"-q:v\", \"5\"%s", (int)(at - t), t, at + strlen(from));
            CHECK(build_text(v, W "/video_q5_ref.ysppak", err, sizeof err) == 0 &&
                  build_with(v, W "/video_q5.ysppak", W "/vcache", 0, NULL, NULL, err, sizeof err) == 0 &&
                  same_file(W "/video_q5_ref.ysppak", W "/video_q5.ysppak") && !same_file(W "/video_q5.ysppak", W "/video.ysppak"),
                  "video, another argument: a miss, the bytes without a cache: %s", err);
        }
    }
#if !defined(_WIN32)
    {   /* another ffmpeg: another version line, or only another configuration
         * (the same pack): each a miss */
        static const char wv[] = "#!/bin/sh\nif [ \"$1\" = \"-version\" ]; then echo 'ffmpeg version 0.0-ysp-test Copyright (c) the test'; "
                                 "echo 'configuration: --ysp-test'; exit 0; fi\nexec ffmpeg \"$@\"\n";
        static const char wc[] = "#!/bin/sh\nif [ \"$1\" = \"-version\" ]; then ffmpeg -version | head -n 1; "
                                 "echo 'configuration: --ysp-test'; exit 0; fi\nexec ffmpeg \"$@\"\n";
        ypt_options o;
        FILE* lf;
        write_file(W "/ffwrap_v.sh", wv, sizeof wv - 1);
        write_file(W "/ffwrap_c.sh", wc, sizeof wc - 1);
        chmod(W "/ffwrap_v.sh", 0755);
        chmod(W "/ffwrap_c.sh", 0755);
        memset(&o, 0, sizeof o);
        o.ffmpeg = W "/ffwrap_v.sh";
        rc = ypt_build_text(t, strlen(t), W, W "/video_wv_ref.ysppak", &o, err, sizeof err);
        o.cache = W "/vcache";
        lf = fopen(W "/video_wv.log", "w");
        o.log = lf;
        if (rc == 0) rc = ypt_build_text(t, strlen(t), W, W "/video_wv.ysppak", &o, err, sizeof err);
        if (lf) fclose(lf);
        CHECK(rc == 0 && count_text(W "/video_wv.log", "(cache)") == 0 && same_file(W "/video_wv_ref.ysppak", W "/video_wv.ysppak") &&
              has_bytes(W "/video_wv.ysppak", "ffmpeg version 0.0-ysp-test"), "video, another ffmpeg version: a miss: %s", err);
        o.ffmpeg = W "/ffwrap_c.sh";
        lf = fopen(W "/video_wc.log", "w");
        o.log = lf;
        rc = ypt_build_text(t, strlen(t), W, W "/video_wc.ysppak", &o, err, sizeof err);
        if (lf) fclose(lf);
        CHECK(rc == 0 && count_text(W "/video_wc.log", "(cache)") == 0 && same_file(W "/video.ysppak", W "/video_wc.ysppak"),
              "video, the same version line from another configuration: a miss, the same bytes: %s", err);
    }
#endif
    {   /* a copied MPEG-1 file: the record holds the index, not the video */
        static const char tc[] = "{\"format\": \"ysp-pack-source\", \"version\": 1, \"resources\": [{\"kind\": \"video\", \"name\": \"video/copy.mpg\", "
                                 "\"source\": \"video/copy.mpg\"}]}";
        ypak_pack p;
        ypak_desc d;
        ypak_entry e;
        long long vs = -1;
        int i, k, before, after, small = 0;
        char old[64][96];
        memset(&d, 0, sizeof d);
        d.path = W "/video.ysppak";
        if (ypak_open(&p, &d)) {
            const void* v = entry(&p, "video/clip.mpg", &e);
            if (v) { write_file(W "/video/copy.mpg", v, (size_t)e.size); vs = (long long)e.size; }
            ypak_close(&p);
        }
        before = list_dir(W "/vcache", ".yspce", old, 64);
        rc = build_text(tc, W "/copy_ref.ysppak", err, sizeof err);
        CHECK(rc == 0 && build_with(tc, W "/copy_cold.ysppak", W "/vcache", 0, NULL, NULL, err, sizeof err) == 0 &&
              same_file(W "/copy_ref.ysppak", W "/copy_cold.ysppak"), "a copied video, a cold cache: the same bytes: %s", err);
        after = list_dir(W "/vcache", ".yspce", names, 64);
        for (i = 0; i < after; i++) {
            char q[1200];
            int seen = 0;
            for (k = 0; k < before; k++) seen |= !strcmp(old[k], names[i]);
            snprintf(q, sizeof q, W "/vcache/%.95s", names[i]);
            if (!seen && file_size(q) < vs) small++;
        }
        rc = build_with(tc, W "/copy_warm.ysppak", W "/vcache", 0, W "/copy_warm.log", NULL, err, sizeof err);
        CHECK(rc == 0 && same_file(W "/copy_ref.ysppak", W "/copy_warm.ysppak") && count_text(W "/copy_warm.log", "(cache)") == 2 && after == before + 1 && small == 1,
              "a copied video, a warm cache: the same bytes, its new record smaller than the video (%d of %d): %s", small, after, err);
    }
}

/* VIDEO and VIDEO_INDEX through the ffmpeg step, when ffmpeg runs here: a
 * raw gray clip made in code becomes MPEG-1 (pl_mpeg indexes it on every
 * platform) and, on Windows, H.264 in MP4 (Media Foundation indexes it).
 * The build is repeated (ffmpeg's bytes must not change), the pack is
 * rebuilt with this ffmpeg (the step runs again) and with no ffmpeg (the
 * recorded output is used), both identical. Skipped without ffmpeg; CI
 * runners may not have it. */
static void test_video(void) {
    static const char t[] =
        "{\"format\": \"ysp-pack-source\", \"version\": 1, \"resources\": [\n"
        "  {\"kind\": \"video\", \"name\": \"video/clip.mpg\", \"source\": \"video/raw.gray\",\n"
        "   \"ffmpeg_input\": [\"-f\", \"rawvideo\", \"-pix_fmt\", \"gray\", \"-s\", \"64x48\", \"-framerate\", \"30\"],\n"
        "   \"ffmpeg\": [\"-c:v\", \"mpeg1video\", \"-q:v\", \"3\", \"-flags\", \"+psnr\", \"-g\", \"10\", \"-bf\", \"0\", \"-pix_fmt\", \"yuv420p\", \"-f\", \"mpeg\"],\n"
        "   \"threads\": 3}"
#if defined(_WIN32)
        ",\n  {\"kind\": \"video\", \"name\": \"video/clip.mp4\", \"source\": \"video/raw.gray\",\n"
        "   \"ffmpeg_input\": [\"-f\", \"rawvideo\", \"-pix_fmt\", \"gray\", \"-s\", \"64x48\", \"-framerate\", \"30\"],\n"
        "   \"ffmpeg\": [\"-c:v\", \"libx264\", \"-preset\", \"fast\", \"-crf\", \"20\", \"-pix_fmt\", \"yuv420p\", \"-bf\", \"0\", \"-g\", \"10\",\n"
        "               \"-keyint_min\", \"10\", \"-sc_threshold\", \"0\", \"-x264-params\",\n"
        "               \"bframes=0:scenecut=0:open-gop=0:colorprim=bt709:transfer=bt709:colormatrix=bt709:range=tv:chromaloc=0\",\n"
        "               \"-video_track_timescale\", \"30\", \"-an\"]}"
#endif
        "\n]}\n";
    static uint8_t raw[64 * 48 * 30];
    char err[1024];
    int k, rc;
    FILE* rep;
#if defined(_WIN32)
    if (system("ffmpeg -version > NUL 2>&1") != 0) { printf("tool_test: video through ffmpeg skipped (no ffmpeg on the PATH)\n"); return; }
#else
    if (system("ffmpeg -version > /dev/null 2>&1") != 0) { printf("tool_test: video through ffmpeg skipped (no ffmpeg on the PATH)\n"); return; }
#endif
    for (k = 0; k < (int)sizeof raw; k++) raw[k] = (uint8_t)((k % 64) * 3 + ((k / 64) % 48) * 5 + (k / (64 * 48)) * 7);
    write_file(W "/video/raw.gray", raw, sizeof raw);
    rc = build_text(t, W "/video.ysppak", err, sizeof err);
    CHECK(rc == 0, "video through ffmpeg: %s", err);
    if (rc) return;
    {
        ypak_pack p;
        ypak_desc d;
        static const char* const names[2] = { "video/clip.mpg", "video/clip.mp4" };
        memset(&d, 0, sizeof d);
        d.path = W "/video.ysppak";
        d.verify = YPAK_VERIFY_OPEN;
        if (ypak_open(&p, &d)) {
            for (k = 0; k < 2; k++) {
                ypak_entry e, ie;
                char in[64];
                yvid_desc vd;
                yvid_info vi;
                const void* v;
                const void* iv;
#if !defined(_WIN32)
                if (k == 1) break;
#endif
                snprintf(in, sizeof in, "%s.yspvi", names[k]);
                v = entry(&p, names[k], &e);
                iv = entry(&p, in, &ie);
                CHECK(v && iv && e.kind == YPAK_KIND_VIDEO && (e.flags & YPAK_F_STREAM) && ie.kind == YPAK_KIND_VIDEO_INDEX, "%s and its index", names[k]);
                if (!v || !iv) continue;
                memset(&vd, 0, sizeof vd);
                vd.data = v;
                vd.size = (size_t)e.size;
                vd.index = iv;
                vd.index_size = (size_t)ie.size;
                CHECK(yvid_probe(&vd, &vi, err, sizeof err) == 0 && vi.frames == 30 && vi.w == 64 && vi.h == 48 && vi.fps_num == 30 * vi.fps_den,
                      "%s: the index describes 30 frames of 64 x 48 at 30 fps: %s", names[k], err);
            }
            ypak_close(&p);
        } else {
            CHECK(0, "video pack opens: %s", ypak_error(&p));
        }
    }
    CHECK(build_text(t, W "/video_again.ysppak", err, sizeof err) == 0 && same_file(W "/video.ysppak", W "/video_again.ysppak"),
          "video through ffmpeg twice: same bytes (%s)", err);
    CHECK(ypt_verify(W "/video.ysppak", NULL, err, sizeof err) == 0, "video pack verifies: %s", err);
    rc = ypt_rebuild(W "/video.ysppak", W, W "/video_rebuilt.ysppak", NULL, err, sizeof err);
    CHECK(rc == 0 && same_file(W "/video.ysppak", W "/video_rebuilt.ysppak"), "video rebuild with this ffmpeg: %s", err);
    {
        ypt_options o;
        size_t n;
        uint8_t* text;
        memset(&o, 0, sizeof o);
        o.ffmpeg = "ypak-test-no-such-ffmpeg";
        rep = fopen(W "/video_rebuild.txt", "w");
        rc = ypt_rebuild_ex(W "/video.ysppak", W, W "/video_rebuilt2.ysppak", &o, rep, err, sizeof err);
        if (rep) fclose(rep);
        text = read_all(W "/video_rebuild.txt", &n);
        CHECK(rc == 0 && same_file(W "/video.ysppak", W "/video_rebuilt2.ysppak") && text && strstr((const char*)text, "recorded output is used"),
              "video rebuild without ffmpeg takes the recorded output: %s", err);
        free(text);
    }
    test_video_cache(t);
    /* the thread count (decided 2026-10-20) in the command and as a
     * parameter: the author's 3 for MPEG-1, the machine's for H.264. The
     * rebuild above was identical, so it ran with the recorded 3, not the
     * machine's count (on a machine with 3 logical processors that check
     * would not tell). */
    {
        ypak_pack p;
        ypak_desc d;
        ypak_entry e;
        memset(&d, 0, sizeof d);
        d.path = W "/video.ysppak";
        if (ypak_open(&p, &d)) {
            const char* m = (const char*)entry(&p, "ysp/manifest.json", &e);
            char* z = (char*)malloc((size_t)e.size + 1);
            if (m && z) {
                memcpy(z, m, (size_t)e.size);
                z[e.size] = 0;
                CHECK(strstr(z, "-flags +psnr+bitexact -g 10") != NULL && strstr(z, "-f mpeg -dct int -idct simple -threads 3 {output}") != NULL && strstr(z, "\"threads\": 3\n") != NULL,
                      "MPEG-1: bitexact merged into the author's -flags, the DCTs and the author's count in the command, the count in the derivation");
#if defined(_WIN32)
                {
                    /* the logical processors, as GetActiveProcessorCount() counts them on a machine of one group */
                    const char* np = getenv("NUMBER_OF_PROCESSORS");
                    int mt = np ? atoi(np) : 0;
                    char want[64], want2[64];
                    if (mt > 64) mt = 64;
                    snprintf(want, sizeof want, "-an -threads %d {output}", mt);
                    snprintf(want2, sizeof want2, "\"threads\": %d\n", mt);
                    CHECK(mt > 0 && strstr(z, want) != NULL && strstr(z, want2) != NULL, "H.264: the machine's count (%d) in the command and the derivation", mt);
                }
#endif
            }
            free(z);
            ypak_close(&p);
        }
    }
#if defined(_WIN32)
    /* libx265: its pools forced into the author's x265-params. Media
     * Foundation's HEVC decoder is a Store extension; without it the index
     * cannot be made, and only the command is checked by the refusal. */
    {
        static const char t5[] =
            "{\"format\": \"ysp-pack-source\", \"version\": 1, \"resources\": [\n"
            "  {\"kind\": \"video\", \"name\": \"video/clip265.mp4\", \"source\": \"video/raw.gray\",\n"
            "   \"ffmpeg_input\": [\"-f\", \"rawvideo\", \"-pix_fmt\", \"gray\", \"-s\", \"64x48\", \"-framerate\", \"30\"],\n"
            "   \"ffmpeg\": [\"-c:v\", \"libx265\", \"-crf\", \"24\", \"-pix_fmt\", \"yuv420p\", \"-x265-params\",\n"
            "               \"bframes=0:keyint=10:min-keyint=10:scenecut=0:log-level=error:colorprim=bt709:transfer=bt709:colormatrix=bt709:range=limited\",\n"
            "               \"-video_track_timescale\", \"30\", \"-an\"], \"threads\": 4,\n"
            "   \"color\": {\"matrix\": \"bt709\", \"range\": \"limited\", \"transfer\": \"bt1886\", \"primaries\": \"bt709\", \"siting\": \"left\"}}]}\n";
        rc = build_text(t5, W "/video265.ysppak", err, sizeof err);
        if (rc == 0) {
            ypak_pack p;
            ypak_desc d;
            ypak_entry e;
            memset(&d, 0, sizeof d);
            d.path = W "/video265.ysppak";
            if (ypak_open(&p, &d)) {
                const char* m = (const char*)entry(&p, "ysp/manifest.json", &e);
                char* z = (char*)malloc((size_t)e.size + 1);
                if (m && z) {
                    memcpy(z, m, (size_t)e.size);
                    z[e.size] = 0;
                    CHECK(strstr(z, "range=limited:pools=4:frame-threads=2:info=0 -video_track_timescale") && strstr(z, "-an -threads 4 {output}"),
                          "libx265: the pool, its frame threads and info=0 in the author's x265-params, in the recorded command");
                }
                free(z);
                ypak_close(&p);
            }
            CHECK(build_text(t5, W "/video265_again.ysppak", err, sizeof err) == 0 && same_file(W "/video265.ysppak", W "/video265_again.ysppak"),
                  "libx265 twice: same bytes");
        } else if (strstr(err, "Unknown encoder") || strstr(err, "Media Foundation")) {
            /* an ffmpeg without libx265, or no HEVC decoder in Media Foundation */
            printf("tool_test: the libx265 pack was not built here (%s)\n", err);
        } else {
            CHECK(0, "libx265 through ffmpeg: %s", err);
        }
    }
#endif
}

/* --- glyph runs from the experiment's texts (gap G9) ----------------------------
 * The Stroop example's pack source (examples/pack/stroop) with the box font
 * standing in for Noto Sans: the test needs the texts' glyphs, not the
 * design. Its description lists no glyph runs; the tool derives them. */

#ifndef YPT_STROOP_DIR
#define YPT_STROOP_DIR "examples/pack/stroop"
#endif
#define WS W "/stroop"

static void put_text(const char* path, const char* text) { write_file(path, text, strlen(text)); }

static int build_ws(const char* desc, const char* out, char* err, size_t cap) {
    return ypt_build_text(desc, strlen(desc), WS, out, NULL, err, cap);
}

#ifndef YPT_NO_LAYOUT   /* the core tool refuses the runs before these are needed */
static char* text_file(const char* path) {
    size_t n;
    uint8_t* d = read_all(path, &n);
    if (d) d[n] = 0;
    return (char*)d;
}

/* The bytes of one entry of a pack, malloc'd. */
static uint8_t* entry_bytes(const char* pack, const char* name, size_t* n) {
    ypak_pack p;
    ypak_desc d;
    ypak_entry e;
    uint8_t* r = NULL;
    memset(&d, 0, sizeof d);
    d.path = pack;
    if (!ypak_open(&p, &d)) return NULL;
    if (ypak_find(&p, name, &e) == 0) {
        const void* x = ypak_data(&p, &e);
        r = (uint8_t*)malloc((size_t)e.size + 1);
        if (r && x) { memcpy(r, x, (size_t)e.size); r[e.size] = 0; *n = (size_t)e.size; }
        else { free(r); r = NULL; }
    }
    ypak_close(&p);
    return r;
}

#define STROOP_DESC(extra) \
    "{\"format\": \"ysp-pack-source\", \"version\": 1,\n" \
    " \"experiment\": {\"entry\": \"experiment/stroop.json\", \"source\": \"stroop.json\", \"media_type\": \"application/vnd.ysp.experiment\"},\n" \
    " \"resources\": [\n" \
    "  {\"name\": \"stroop.pstb\", \"kind\": \"table\", \"source\": \"stroop.csv\"},\n" \
    "  {\"kind\": \"font\", \"source\": \"fonts/NotoSans-Regular.ttf\", \"license\": \"OFL-1.1\"},\n" \
    "  {\"kind\": \"curveset\", \"from\": \"fonts/NotoSans-Regular.ttf\", \"glyphs\": \"used\"}" extra "]}\n"

/* A one-stimulus definition in WS/defs/NAME.json and a description for it. */
static int build_def(const char* name, const char* text_stim, const char* extra_resources, const char* out, char* err, size_t cap) {
    char path[256], def[2048], desc[2048];
    snprintf(path, sizeof path, WS "/defs/%s.json", name);
    snprintf(def, sizeof def,
             "{\"format\": \"ysp-experiment\", \"version\": 1, \"stimuli\": {%s}, \"trials\": {\"t\": {}},\n"
             " \"timeline\": [{\"name\": \"main\", \"timeline_variables\": \"stroop.pstb\", \"trial\": \"t\"}]}\n", text_stim);
    put_text(path, def);
    snprintf(desc, sizeof desc,
             "{\"format\": \"ysp-pack-source\", \"version\": 1, \"experiment\": {\"entry\": \"exp/%s.json\", \"source\": \"defs/%s.json\"},\n"
             " \"resources\": [{\"name\": \"stroop.pstb\", \"kind\": \"table\", \"source\": \"stroop.csv\"},\n"
             "  {\"kind\": \"font\", \"source\": \"fonts/NotoSans-Regular.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/NotoSans-Regular.ttf\"}%s]}\n",
             name, name, extra_resources);
    return build_ws(desc, out, err, cap);
}

/* The box font's glyph of a printable ASCII character (make_font's cmap). */
static int box_glyph(int c) {
    int r, base = 2;
    for (r = 0; r < N_RANGES; r++) {
        if (c >= (int)font_ranges[r][0] && c <= (int)font_ranges[r][1]) return base + c - (int)font_ranges[r][0];
        base += (int)(font_ranges[r][1] - font_ranges[r][0] + 1);
    }
    return c == ' ' ? 1 : 0;
}
#endif

#ifndef YPT_NO_LAYOUT
/* The key of a derived block for a stimulus of this font, size and width
 * (the other keys at their defaults), from ysp/pack.h as the player builds
 * it. Four buffers in turn, so a call can use a few at once. */
static const char* skey(const char* font, double size, double width, const char* text) {
    static char buf[4][512];
    static int at;
    ypak_text_style st;
    char* b = buf[at++ & 3];
    memset(&st, 0, sizeof st);
    st.font = font;
    st.size = size;
    st.width = width;
    if (ypak_runs_key(&st, text, strlen(text), b, sizeof buf[0]) < 0) b[0] = 0;
    return b;
}

/* The player's side of the key: read the definition's text stimuli as a
 * loader would (ysp/json.h, doubles), and for every block of the entry
 * find the stimulus whose ypak_runs_key() of the block's text is the
 * block's key. Returns the blocks matched; *n_blocks gets the count. */
static int player_matches(const char* pack, const char* runs_name, const char* def_path, uint32_t* n_blocks) {
    size_t n, dn;
    uint8_t* x = entry_bytes(pack, runs_name, &n);
    char* def = text_file(def_path);
    yjs_arena a;
    yjs_value* root = NULL;
    yjs_error je;
    ypak_runs r;
    int matched = 0;
    uint32_t i;
    *n_blocks = 0;
    yjs_arena_init_heap(&a, 0);
    memset(&r, 0, sizeof r);
    dn = def ? strlen(def) : 0;
    if (x && def && yjs_parse(&a, def, dn, NULL, &root, &je) == YJS_OK && ypak_runs_view(x, (int64_t)n, &r, NULL, 0) == 0) {
        const yjs_value* stim = yjs_get(root, "stimuli");
        *n_blocks = r.n_blocks;
        for (i = 0; i < r.n_blocks; i++) {
            const char* key = ypak_runs_str(&r, r.blocks[i].key_off);
            const char* text = ypak_runs_str(&r, r.blocks[i].text_off);
            uint32_t k;
            for (k = 0; stim && k < stim->n; k++) {
                const yjs_value* o = stim->u.members[k].value;
                const yjs_value* v;
                ypak_text_style st;
                char kb[1024];
                const char* s;
                if (!yjs_get(o, "kind") || strcmp(yjs_get(o, "kind")->u.s, "text")) continue;
                memset(&st, 0, sizeof st);
                st.font = yjs_get(o, "font")->u.s;
                yjs_double(yjs_get(o, "size"), &st.size);
                if ((v = yjs_get(o, "width")) != NULL) yjs_double(v, &st.width);
                if ((v = yjs_get(o, "line_height")) != NULL) yjs_double(v, &st.line_height);
                s = (v = yjs_get(o, "align")) != NULL ? v->u.s : "start";
                st.align = !strcmp(s, "end") ? YPAK_ALIGN_END : !strcmp(s, "center") ? YPAK_ALIGN_CENTER : YPAK_ALIGN_START;
                s = (v = yjs_get(o, "dir")) != NULL ? v->u.s : "auto";
                st.dir = !strcmp(s, "ltr") ? YPAK_DIR_LTR : !strcmp(s, "rtl") ? YPAK_DIR_RTL : YPAK_DIR_AUTO;
                st.lang = (v = yjs_get(o, "lang")) != NULL ? v->u.s : "";
                if (ypak_runs_key(&st, text, strlen(text), kb, sizeof kb) > 0 && !strcmp(kb, key)) { matched++; break; }
            }
        }
    }
    yjs_arena_free(&a);
    free(x);
    free(def);
    return matched;
}

#endif

static void test_stroop_pack(void) {
    static const char* const texts[9] = { "R red     G green     B blue", "Practice. Name the ink color, not the word.",
                                          "Test. No feedback from here on.", "Correct", "Too slow", "Wrong", "BLUE", "GREEN", "RED" };
    static const float sizes[9] = { 22, 30, 30, 32, 32, 32, 72, 72, 72 };
    char err[1024];
    int rc, i;
    bb f;
    MKDIR(WS); MKDIR(WS "/fonts"); MKDIR(WS "/defs");
    {
        static const char* const files[3] = { "stroop.json", "stroop.csv", "pack.json" };
        for (i = 0; i < 3; i++) {
            char a[1024], b2[256];
            size_t n;
            uint8_t* d;
            snprintf(a, sizeof a, "%s/%s", YPT_STROOP_DIR, files[i]);
            snprintf(b2, sizeof b2, WS "/%s", files[i]);
            d = read_all(a, &n);
            CHECK(d != NULL, "Stroop example file %s", a);
            if (d) { write_file(b2, d, n); free(d); }
        }
    }
    memset(&f, 0, sizeof f);
    make_font(&f, -1);
    write_file(WS "/fonts/NotoSans-Regular.ttf", f.d, f.n);
    make_font(&f, 8);
    write_file(WS "/fonts/other.ttf", f.d, f.n);
    {
        static const uint32_t at_only[2][2] = { { 0x40, 0x5a }, { 0x61, 0x7a } };   /* with '@', which the box font lacks */
        make_font_r(&f, -1, at_only, 2);
        write_file(WS "/fonts/at.ttf", f.d, f.n);
    }
    {
        /* one script each, for the fallback past the script (layout v0.2.0):
         * Hebrew without numerals or punctuation, and ASCII without Hebrew */
        static const uint32_t he_only[1][2] = { { 0x05b0, 0x05ea } };
        static const uint32_t la_only[3][2] = { { 0x21, 0x3f }, { 0x41, 0x5f }, { 0x61, 0x7e } };
        make_font_r(&f, -1, he_only, 1);
        write_file(WS "/fonts/he.ttf", f.d, f.n);
        make_font_r(&f, -1, la_only, 3);
        write_file(WS "/fonts/la.ttf", f.d, f.n);
    }
    free(f.d);
    rc = ypt_build(WS "/pack.json", WS "/stroop.ysppak", NULL, err, sizeof err);
#ifdef YPT_NO_LAYOUT
    CHECK(rc < 0 && strstr(err, "'experiment/stroop.ysprun': the experiment's texts need glyph runs, and glyph runs need the layout library"),
          "the core tool refuses the Stroop pack by name: %s", err);
    /* a definition without text stimuli still builds in the core tool */
    put_text(WS "/defs/notext.json", "{\"format\": \"ysp-experiment\", \"version\": 1, \"stimuli\": {\"dot\": {\"kind\": \"shape\"}}, \"trials\": {\"t\": {}}, \"timeline\": [{\"name\": \"m\", \"trial\": \"t\"}]}\n");
    rc = build_ws("{\"format\": \"ysp-pack-source\", \"version\": 1, \"experiment\": {\"entry\": \"exp/notext.json\", \"source\": \"defs/notext.json\"},"
                  " \"resources\": []}", WS "/notext.ysppak", err, sizeof err);
    CHECK(rc == 0, "core tool, a definition without texts: %s", err);
    (void)texts;
    (void)sizes;
    return;
#else
    CHECK(rc == 0, "the Stroop pack, its runs derived: %s", err);
    if (rc) return;
    {
        ypak_pack p;
        ypak_desc d;
        ypak_entry e;
        const void* x;
        memset(&d, 0, sizeof d);
        d.path = WS "/stroop.ysppak";
        d.verify = YPAK_VERIFY_OPEN;
        if (!ypak_open(&p, &d)) { CHECK(0, "Stroop pack opens: %s", ypak_error(&p)); return; }
        if ((x = entry(&p, "experiment/stroop.ysprun", &e)) != NULL) {
            ypak_runs r;
            memset(&r, 0, sizeof r);
            CHECK(e.kind == YPAK_KIND_GLYPHRUNS && ypak_runs_view(x, e.size, &r, err, sizeof err) == 0 && r.n_blocks == 9 && r.n_fonts == 1,
                  "Stroop runs: %s (%u blocks)", err, r.n_blocks);
            for (i = 0; i < 9 && r.n_blocks == 9; i++) {
                const char* key = skey("fonts/NotoSans-Regular.ttf", sizes[i], 1000, texts[i]);
                int k = ypak_runs_find(&r, key);
                CHECK(k == i && !strcmp(ypak_runs_str(&r, r.blocks[k].text_off), texts[i]) && r.blocks[k].size == sizes[i] &&
                      r.blocks[k].width == 1000.0f && r.blocks[k].n_items == (uint32_t)strlen(texts[i]) && r.blocks[k].n_lines >= 1,
                      "Stroop block %s: index %d", key, k);
            }
            CHECK(!strcmp(ypak_runs_str(&r, r.fonts[0].cset_off), "fonts/NotoSans-Regular.yspcset") &&
                  !strcmp(ypak_runs_str(&r, r.fonts[0].font_off), "fonts/NotoSans-Regular.ttf"), "Stroop runs name the font and its set");
        }
        /* "glyphs": "used" holds the glyphs of the derived texts and no other */
        if ((x = entry(&p, "fonts/NotoSans-Regular.yspcset", &e)) != NULL) {
            ypak_cset s;
            if (ypak_cset_view(x, e.size, &s, err, sizeof err) == 0) {
                /* glyph of a printable ASCII character in the box font */
                int gz = box_glyph('z'), gR = box_glyph('R'), ga = box_glyph('a');
                CHECK(!(s.flags & YPAK_CSET_ALL) && s.words[8 + gR] && s.words[8 + ga] && !s.words[8 + gz],
                      "Stroop set: R and a present, z absent (%u %u %u)", s.words[8 + gR], s.words[8 + ga], s.words[8 + gz]);
            } else CHECK(0, "Stroop cset view: %s", err);
        }
        ypak_close(&p);
    }
    {
        size_t n;
        uint8_t* m = entry_bytes(WS "/stroop.ysppak", "ysp/manifest.json", &n);
        CHECK(m && strstr((char*)m, "\"experiment\": \"experiment/stroop.json\"") && strstr((char*)m, "\"stimuli\": [\"word\"]") &&
              strstr((char*)m, "\"key\": \"fonts/NotoSans-Regular.ttf#0|72|1000|0|start|auto||RED\"") && strstr((char*)m, "\"derived\": ["),
              "the manifest records the derived blocks and their experiment");
        CHECK(m && strstr((char*)m, "\"from\": [\"fonts/NotoSans-Regular.ttf\", \"experiment/stroop.json\"]"),
              "the runs' 'from': the font and the experiment (no table: the Stroop texts come from lookups)");
        free(m);
    }
    {
        uint32_t nb;
        int m = player_matches(WS "/stroop.ysppak", "experiment/stroop.ysprun", WS "/stroop.json", &nb);
        CHECK(nb == 9 && m == 9, "Stroop: the player's key, from the definition's values, matches every block (%d of %u)", m, nb);
    }
    /* deterministic: again, verified, rebuilt from the manifest */
    CHECK(ypt_build(WS "/pack.json", WS "/stroop_again.ysppak", NULL, err, sizeof err) == 0 && same_file(WS "/stroop.ysppak", WS "/stroop_again.ysppak"),
          "Stroop pack twice: same bytes (%s)", err);
    CHECK(ypt_verify(WS "/stroop.ysppak", NULL, err, sizeof err) == 0, "Stroop pack verifies: %s", err);
    rc = ypt_rebuild(WS "/stroop.ysppak", WS, WS "/stroop_rebuilt.ysppak", NULL, err, sizeof err);
    CHECK(rc == 0 && same_file(WS "/stroop.ysppak", WS "/stroop_rebuilt.ysppak"), "Stroop rebuild: %s", err);
    /* the resources in another order: the same pack */
    put_text(WS "/pack_rev.json",
             "{\"format\": \"ysp-pack-source\", \"version\": 1, \"resources\": [\n"
             "  {\"kind\": \"curveset\", \"from\": \"fonts/NotoSans-Regular.ttf\", \"glyphs\": \"used\"},\n"
             "  {\"kind\": \"font\", \"source\": \"fonts/NotoSans-Regular.ttf\", \"license\": \"OFL-1.1\"},\n"
             "  {\"name\": \"stroop.pstb\", \"kind\": \"table\", \"source\": \"stroop.csv\"}],\n"
             " \"experiment\": {\"media_type\": \"application/vnd.ysp.experiment\", \"source\": \"stroop.json\", \"entry\": \"experiment/stroop.json\"}}\n");
    CHECK(ypt_build(WS "/pack_rev.json", WS "/stroop_rev.ysppak", NULL, err, sizeof err) == 0 && same_file(WS "/stroop.ysppak", WS "/stroop_rev.ysppak"),
          "Stroop pack from a reordered description: same bytes (%s)", err);
    /* the definition respelled (other whitespace, 72.0 for 72) at another
     * source path: the manifest names the path, so the packs differ; the
     * glyph runs do not */
    {
        char* def = text_file(WS "/stroop.json");
        char* at = def ? strstr(def, "\"size\": 72,") : NULL;
        CHECK(at != NULL, "the Stroop definition has \"size\": 72,");
        if (at) {
            char* re = (char*)malloc(strlen(def) + 64);
            char d2[2048];
            size_t n1 = 0, n2 = 0;
            uint8_t* r1;
            uint8_t* r2;
            const char* desc = STROOP_DESC("");
            const char* src = strstr(desc, "\"source\": \"stroop.json\"");
            sprintf(re, "\n  %.*s\"size\": 72.0,%s", (int)(at - def), def, at + 11);
            put_text(WS "/stroop_respelled.json", re);
            snprintf(d2, sizeof d2, "%.*s\"source\": \"stroop_respelled.json\"%s", (int)(src - desc), desc, src + 23);
            rc = build_ws(d2, WS "/stroop_respelled.ysppak", err, sizeof err);
            r1 = entry_bytes(WS "/stroop.ysppak", "experiment/stroop.ysprun", &n1);
            r2 = entry_bytes(WS "/stroop_respelled.ysppak", "experiment/stroop.ysprun", &n2);
            CHECK(rc == 0 && r1 && r2 && n1 == n2 && !memcmp(r1, r2, n1) && !same_file(WS "/stroop.ysppak", WS "/stroop_respelled.ysppak"),
                  "a respelled definition gives the same glyph runs: %s", err);
            free(r1); free(r2); free(re);
        }
        free(def);
    }
    /* the definition respelled: compact, its keys in another order, 72 as
     * 7.2e1 and RED as \u0052ED. Its canonical bytes are the same, so the
     * experiment entry, the experiment ID and the pack ID are too (D2), and
     * a rebuild from it is identical */
    {
        size_t dn = 0, n1 = 0, n2 = 0;
        char* def = text_file(WS "/stroop.json");
        yjs_arena a;
        yjs_value* root = NULL;
        yjs_error je;
        char* canon = NULL;
        char* re = NULL;
        yjs_arena_init_heap(&a, 0);
        if (def) dn = strlen(def);
        if (def && yjs_parse(&a, def, dn, NULL, &root, &je) == YJS_OK) {
            /* the loader's bytes: keys at their defaults left out (D2) */
            size_t rn = yjs_write_mem(root, YJS_WRITE_COMPACT, NULL, 0);
            static yexp_report rp;
            yjs_arena ca;
            yexp_def* dd = NULL;
            const char* cb;
            size_t cn = 0;
            yjs_arena_init_heap(&ca, 0);
            if (yexp_load(&ca, (const char*)def, dn, NULL, &dd, &rp) == YEXP_OK && yexp_canonical(dd, &ca, &cb, &cn, NULL) == YEXP_OK) {
                canon = (char*)malloc(cn + 1);
                memcpy(canon, cb, cn);
                canon[cn] = 0;
            }
            yjs_arena_free(&ca);
            re = (char*)malloc(rn + 64);
            yjs_write_mem(root, YJS_WRITE_COMPACT, re, rn + 1);
        }
        CHECK(canon && re, "the Stroop definition parses");
        if (canon && re) {
            uint8_t* x = entry_bytes(WS "/stroop.ysppak", "experiment/stroop.json", &n1);
            uint8_t* m = entry_bytes(WS "/stroop.ysppak", "ysp/manifest.json", &n2);
            uint8_t sha[32];
            char hx[65];
            const char* at;
            int k, seen = 0;
            ypak_sha256(canon, strlen(canon), sha);
            for (k = 0; k < 32; k++) snprintf(hx + 2 * k, 3, "%02x", sha[k]);
            CHECK(x && n1 == strlen(canon) && !memcmp(x, canon, n1) && memcmp(x, def, dn < n1 ? dn : n1),
                  "the experiment entry is the definition's canonical bytes, not its source's");
            CHECK(x && !memmem_(x, n1, "\"required\"", 10), "a key at its default is left out of the entry");
            for (at = m ? strstr((char*)m, hx) : NULL; at; at = strstr(at + 1, hx)) seen++;
            CHECK(m && strstr((char*)m, "\"op\": \"canonical\"") && strstr((char*)m, "\"by\": \"ysp/experiment.h " YEXP_VERSION_STRING "\"") && seen == 2,
                  "the manifest: op canonical by ysp/experiment.h, and the entry's and the source's SHA-256 are the canonical bytes' (%d of 2)", seen);
            /* the player's key from the stored definition */
            if (x) {
                uint32_t nb;
                int mt;
                write_file(WS "/stroop_entry.json", x, n1);
                mt = player_matches(WS "/stroop.ysppak", "experiment/stroop.ysprun", WS "/stroop_entry.json", &nb);
                CHECK(nb == 9 && mt == 9, "the player's key from the pack's definition matches every block (%d of %u)", mt, nb);
            }
            free(x);
            free(m);
            {
                char* p72 = strstr(re, "\"size\":72,");
                char* pred = strstr(re, "\"RED\"");
                CHECK(p72 && pred && p72 < pred, "the compact definition has size 72 before RED");
                if (p72 && pred && p72 < pred) {
                    char* re2 = (char*)malloc(strlen(re) + 64);
                    sprintf(re2, "\n%.*s\"size\":7.2e1,%.*s\"\\u0052ED\"%s\n", (int)(p72 - re), re, (int)(pred - (p72 + 10)), p72 + 10, pred + 5);
                    MKDIR(WS "/re"); MKDIR(WS "/re/fonts");
                    put_text(WS "/re/stroop.json", re2);
                    free(re2);
                    {
                        static const char* const files[3] = { "stroop.csv", "pack.json", "fonts/NotoSans-Regular.ttf" };
                        int fi;
                        for (fi = 0; fi < 3; fi++) {
                            char a1[256], a2[256];
                            size_t fn;
                            uint8_t* fd;
                            snprintf(a1, sizeof a1, WS "/%s", files[fi]);
                            snprintf(a2, sizeof a2, WS "/re/%s", files[fi]);
                            fd = read_all(a1, &fn);
                            if (fd) { write_file(a2, fd, fn); free(fd); }
                        }
                    }
                    rc = ypt_build(WS "/re/pack.json", WS "/stroop_re.ysppak", NULL, err, sizeof err);
                    CHECK(rc == 0 && same_file(WS "/stroop.ysppak", WS "/stroop_re.ysppak"),
                          "a respelled definition gives the same pack, byte for byte (the same pack ID): %s", err);
                    rc = ypt_rebuild(WS "/stroop.ysppak", WS "/re", WS "/stroop_rebuilt_re.ysppak", NULL, err, sizeof err);
                    CHECK(rc == 0 && same_file(WS "/stroop.ysppak", WS "/stroop_rebuilt_re.ysppak"),
                          "a rebuild from the respelled source is identical: %s", err);
                    /* another value is another source */
                    {
                        char* other = text_file(WS "/re/stroop.json");
                        char* p = other ? strstr(other, "7.2e1") : NULL;
                        if (p) {
                            p[2] = '3';
                            put_text(WS "/re/stroop.json", other);
                            rc = ypt_rebuild(WS "/stroop.ysppak", WS "/re", NULL, NULL, err, sizeof err);
                            CHECK(rc < 0 && strstr(err, "source 'stroop.json' of 'experiment/stroop.json' is not the one") &&
                                  strstr(err, "of its canonical bytes"), "a changed definition: the rebuild names it: %s", err);
                        } else CHECK(0, "the respelled definition has 7.2e1");
                        free(other);
                    }
                }
            }
        }
        free(canon);
        free(re);
        free(def);
        yjs_arena_free(&a);
    }
    /* union: a glyphruns resource of the experiment's runs name adds blocks
     * and fallback fonts; a rebuild takes it back */
    rc = build_ws(STROOP_DESC(",\n  {\"kind\": \"font\", \"source\": \"fonts/other.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/other.ttf\"},\n"
                              "  {\"kind\": \"glyphruns\", \"name\": \"experiment/stroop.ysprun\", \"fonts\": [\"fonts/other.ttf\"],\n"
                              "   \"blocks\": [{\"key\": \"pause\", \"text\": \"Take a break.\", \"size\": \"30\"}]}"),
                  WS "/union.ysppak", err, sizeof err);
    CHECK(rc == 0, "union with a listed block: %s", err);
    if (rc == 0) {
        size_t n;
        uint8_t* x = entry_bytes(WS "/union.ysppak", "experiment/stroop.ysprun", &n);
        ypak_runs r;
        memset(&r, 0, sizeof r);
        if (x && ypak_runs_view(x, (int64_t)n, &r, err, sizeof err) == 0) {
            int kp = ypak_runs_find(&r, "pause"), kw = ypak_runs_find(&r, skey("fonts/NotoSans-Regular.ttf", 72, 1000, "RED"));
            CHECK(r.n_blocks == 10 && r.n_fonts == 2 && kp >= 0 && kw >= 0, "union: %u blocks, %u fonts", r.n_blocks, r.n_fonts);
            /* entry fonts: the listed one first, then the stimuli's; each
             * block's runs name the font that shaped it */
            CHECK(!strcmp(ypak_runs_str(&r, r.fonts[0].font_off), "fonts/other.ttf") &&
                  !strcmp(ypak_runs_str(&r, r.fonts[1].font_off), "fonts/NotoSans-Regular.ttf"), "union: font order");
            if (kp >= 0 && kw >= 0)
                CHECK(r.runs[r.blocks[kp].run_first].font == 0 && r.runs[r.blocks[kw].run_first].font == 1,
                      "union: the listed block in its font, the derived one in its stimulus's (%u, %u)", r.runs[r.blocks[kp].run_first].font,
                      r.runs[r.blocks[kw].run_first].font);
        } else CHECK(0, "union runs view: %s", err);
        free(x);
        rc = ypt_rebuild(WS "/union.ysppak", WS, WS "/union_rebuilt.ysppak", NULL, err, sizeof err);
        CHECK(rc == 0 && same_file(WS "/union.ysppak", WS "/union_rebuilt.ysppak"), "union rebuild: %s", err);
        {
            uint32_t nb;
            int m = player_matches(WS "/union.ysppak", "experiment/stroop.ysprun", WS "/stroop.json", &nb);
            CHECK(nb == 10 && m == 9, "union: the player's key matches the 9 derived blocks (%d of %u)", m, nb);
        }
        /* the layout stamp names every font of the entry, in its order */
        x = entry_bytes(WS "/union.ysppak", "ysp/manifest.json", &n);
        {
            char want[160], hx[65];
            uint8_t sha[32];
            size_t fn;
            uint8_t* fb = read_all(WS "/fonts/NotoSans-Regular.ttf", &fn);
            const char* a = x ? strstr((char*)x, " fonts=fonts/other.ttf#0=") : NULL;
            const char* q = a ? strchr(a, '"') : NULL;
            const char* b2;
            int k;
            if (fb) ypak_sha256(fb, fn, sha);
            for (k = 0; k < 32; k++) snprintf(hx + 2 * k, 3, "%02x", sha[k]);
            snprintf(want, sizeof want, ",fonts/NotoSans-Regular.ttf#0=%s", hx);
            b2 = a ? strstr(a, want) : NULL;
            CHECK(fb && a && q && b2 && b2 < q, "union: the stamp lists both fonts in the entry's order, each with its own SHA-256");
            free(fb);
        }
        free(x);
        /* the glyphs a derived text uses are marked in its own font's set */
        x = entry_bytes(WS "/union.ysppak", "fonts/NotoSans-Regular.yspcset", &n);
        {
            ypak_cset s;
            CHECK(x && ypak_cset_view(x, (int64_t)n, &s, err, sizeof err) == 0 && s.words[8 + box_glyph('R')] && s.words[8 + box_glyph('G')],
                  "union: the Stroop words' glyphs are in the stimulus font's set");
        }
        free(x);
    }
    /* two stimuli in two fonts: each shaped in its own (one library each) */
    rc = build_def("twofonts",
                   "\"a\": {\"kind\": \"text\", \"font\": \"fonts/other.ttf\", \"size\": 20, \"text\": \"$word\"},"
                   " \"b\": {\"kind\": \"text\", \"font\": \"fonts/NotoSans-Regular.ttf\", \"size\": 20, \"text\": \"$word\"}",
                   ", {\"kind\": \"font\", \"source\": \"fonts/other.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/other.ttf\"}",
                   WS "/twofonts.ysppak", err, sizeof err);
    CHECK(rc == 0, "two fonts: %s", err);
    if (rc == 0) {
        size_t n;
        uint8_t* x = entry_bytes(WS "/twofonts.ysppak", "exp/twofonts.ysprun", &n);
        ypak_runs r;
        memset(&r, 0, sizeof r);
        if (x && ypak_runs_view(x, (int64_t)n, &r, err, sizeof err) == 0) {
            static const char* const w[3] = { "blue", "green", "red" };
            int k, ok = r.n_blocks == 6 && r.n_fonts == 2 && !strcmp(ypak_runs_str(&r, r.fonts[0].font_off), "fonts/NotoSans-Regular.ttf");
            for (k = 0; ok && k < 3; k++) {
                int ia = ypak_runs_find(&r, skey("fonts/other.ttf", 20, 0, w[k]));
                int ib = ypak_runs_find(&r, skey("fonts/NotoSans-Regular.ttf", 20, 0, w[k]));
                ok = ia >= 0 && ib >= 0 && r.runs[r.blocks[ia].run_first].font == 1 && r.runs[r.blocks[ib].run_first].font == 0;
            }
            CHECK(ok, "two fonts: the levels of $word per stimulus, each in its stimulus's font (%u blocks)", r.n_blocks);
        } else CHECK(0, "two fonts runs view: %s", err);
        free(x);
        rc = ypt_rebuild(WS "/twofonts.ysppak", WS, WS "/twofonts_rebuilt.ysppak", NULL, err, sizeof err);
        CHECK(rc == 0 && same_file(WS "/twofonts.ysppak", WS "/twofonts_rebuilt.ysppak"), "two fonts rebuild: %s", err);
        {
            uint32_t nb;
            int m = player_matches(WS "/twofonts.ysppak", "exp/twofonts.ysprun", WS "/defs/twofonts.json", &nb);
            CHECK(nb == 6 && m == 6, "two fonts: the player's key matches all %u blocks (%d)", nb, m);
        }
    }
    /* two stimuli of one style that show one text share its block */
    rc = build_def("shared",
                   "\"a\": {\"kind\": \"text\", \"font\": \"fonts/NotoSans-Regular.ttf\", \"size\": 20, \"text\": \"$word\"},"
                   " \"b\": {\"kind\": \"text\", \"font\": \"fonts/NotoSans-Regular.ttf\", \"size\": 20.0, \"x\": 9, \"text\": \"$word\"},"
                   " \"c\": {\"kind\": \"text\", \"font\": \"fonts/NotoSans-Regular.ttf\", \"size\": 24, \"text\": \"red\"}", "",
                   WS "/shared.ysppak", err, sizeof err);
    CHECK(rc == 0, "shared style: %s", err);
    if (rc == 0) {
        uint32_t nb;
        size_t n;
        int m = player_matches(WS "/shared.ysppak", "exp/shared.ysprun", WS "/defs/shared.json", &nb);
        uint8_t* x = entry_bytes(WS "/shared.ysppak", "ysp/manifest.json", &n);
        CHECK(nb == 4 && m == 4, "shared style: 3 blocks for a and b, 1 for c, every key the player's (%u blocks, %d matched)", nb, m);
        CHECK(x && strstr((char*)x, "\"stimuli\": [\"a\", \"b\"]") && strstr((char*)x, "\"stimuli\": [\"c\"]"),
              "shared style: the record names both stimuli of a shared block");
        free(x);
        rc = ypt_rebuild(WS "/shared.ysppak", WS, WS "/shared_rebuilt.ysppak", NULL, err, sizeof err);
        CHECK(rc == 0 && same_file(WS "/shared.ysppak", WS "/shared_rebuilt.ysppak"), "shared style rebuild: %s", err);
    }
    /* a fallback from the description's 'fonts': '@' from at.ttf after the
     * stimulus's font, in one block of two runs */
    rc = build_def("fallback", "\"s\": {\"kind\": \"text\", \"font\": \"fonts/NotoSans-Regular.ttf\", \"size\": 20, \"text\": \"mail@example\"}",
                   ", {\"kind\": \"font\", \"source\": \"fonts/at.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/at.ttf\"},"
                   " {\"kind\": \"glyphruns\", \"name\": \"exp/fallback.ysprun\", \"fonts\": [\"fonts/at.ttf\"]}",
                   WS "/fallback.ysppak", err, sizeof err);
    CHECK(rc == 0, "a fallback font for a derived text: %s", err);
    if (rc == 0) {
        size_t n;
        uint8_t* x = entry_bytes(WS "/fallback.ysppak", "exp/fallback.ysprun", &n);
        ypak_runs r;
        memset(&r, 0, sizeof r);
        if (x && ypak_runs_view(x, (int64_t)n, &r, err, sizeof err) == 0 && r.n_blocks == 1) {
            /* the font of each character, by its cluster; Skribidi may keep
             * the fallback after '@', so only 'm' and '@' are pinned */
            int font_of[12], k2;
            uint32_t k, i2;
            const ypak_runs_block_rec* bk = &r.blocks[0];
            for (k2 = 0; k2 < 12; k2++) font_of[k2] = -1;
            for (k = 0; k < bk->n_runs; k++) {
                const ypak_runs_run* ru = &r.runs[bk->run_first + k];
                for (i2 = 0; i2 < ru->n; i2++) {
                    uint32_t cl = r.clusters[bk->item_first + ru->first + i2];
                    if (cl < 12) font_of[cl] = (int)ru->font;
                }
            }
            CHECK(r.n_fonts == 2 && !strcmp(ypak_runs_str(&r, r.fonts[0].font_off), "fonts/at.ttf") && font_of[0] == 1 && font_of[4] == 0,
                  "fallback: 'm' in the stimulus's font (%d), '@' in the listed fallback (%d)", font_of[0], font_of[4]);
        } else CHECK(0, "fallback runs view: %s", err);
        free(x);
    }
    /* numbers and punctuation in Hebrew text: the letters and spaces from
     * he.ttf, the rest from la.ttf, whichever is the stimulus's font and
     * which the listed fallback (ypak 0.9.0 refused these: Skribidi took
     * each script run's fonts from that script's fonts only) */
    {
        static const char* const htexts[3] = { "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d 123", "123 \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d",
                                              "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d 12:30, 4.5%" };
        static const char* const fonts[2] = { "fonts/he.ttf", "fonts/la.ttf" };
        int t, o;
        for (t = 0; t < 3; t++)
            for (o = 0; o < 2; o++) {
                char stim[256], extra[1024], name[32];
                snprintf(name, sizeof name, "hebrew%d%d", t, o);
                snprintf(stim, sizeof stim, "\"s\": {\"kind\": \"text\", \"font\": \"%s\", \"size\": 20, \"text\": \"%s\"}", fonts[o], htexts[t]);
                snprintf(extra, sizeof extra, ", {\"kind\": \"font\", \"source\": \"fonts/he.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/he.ttf\"},"
                         " {\"kind\": \"font\", \"source\": \"fonts/la.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/la.ttf\"},"
                         " {\"kind\": \"glyphruns\", \"name\": \"exp/%s.ysprun\", \"fonts\": [\"%s\"]}", name, fonts[1 - o]);
                rc = build_def(name, stim, extra, WS "/hebrew.ysppak", err, sizeof err);
                CHECK(rc == 0, "Hebrew '%s', stimulus font %s: %s", htexts[t], fonts[o], err);
                if (rc == 0) {
                    char rn[64];
                    size_t n;
                    uint8_t* x;
                    ypak_runs r;
                    snprintf(rn, sizeof rn, "exp/%s.ysprun", name);
                    x = entry_bytes(WS "/hebrew.ysppak", rn, &n);
                    memset(&r, 0, sizeof r);
                    if (x && ypak_runs_view(x, (int64_t)n, &r, err, sizeof err) == 0 && r.n_blocks == 1) {
                        const ypak_runs_block_rec* bk = &r.blocks[0];
                        const unsigned char* p = (const unsigned char*)htexts[t];
                        uint32_t cps[32], ncp = 0, k, i2;
                        int bad = 0;
                        while (*p && ncp < 32) {   /* the text's codepoints: ASCII or 2-byte Hebrew */
                            if (*p < 0x80) cps[ncp++] = *p++;
                            else { cps[ncp++] = (uint32_t)(p[0] & 31) << 6 | (p[1] & 63); p += 2; }
                        }
                        CHECK(bk->n_missing == 0, "Hebrew '%s': %u glyphs 0 in the block", htexts[t], bk->n_missing);
                        for (k = 0; k < bk->n_runs; k++) {
                            const ypak_runs_run* ru = &r.runs[bk->run_first + k];
                            const char* fn = ypak_runs_str(&r, r.fonts[ru->font].font_off);
                            for (i2 = 0; i2 < ru->n; i2++) {
                                uint32_t cl = r.clusters[bk->item_first + ru->first + i2];
                                uint32_t c = cl < ncp ? cps[cl] : 0;
                                const char* want = c >= 0x21 && c <= 0x7e ? "fonts/la.ttf" : "fonts/he.ttf";
                                if (strcmp(fn, want)) bad++;
                            }
                        }
                        CHECK(bad == 0 && bk->n_runs == 2, "Hebrew '%s', stimulus font %s: %d items in the wrong font, %u runs (want 0, 2)",
                              htexts[t], fonts[o], bad, bk->n_runs);
                    } else CHECK(0, "Hebrew runs view: %s", err);
                    free(x);
                }
            }
    }
    /* what no font in the list has: named with its run's script, the
     * character and its codepoint; at most 8, then a count */
    {
        static const struct { const char* text; const char* font; const char* fb; const char* says; } cases[3] = {
            { "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xe2\x98\x83\xe2\x98\x85 12", "fonts/he.ttf", "fonts/la.ttf",
              "no font in the list maps 2 characters (script Hebr: '\xe2\x98\x83' U+2603, '\xe2\x98\x85' U+2605); add a font that has them "
              "(a fallback in 'fonts' of a glyphruns resource of this name)" },
            { "abc \xe2\x98\x83", "fonts/la.ttf", "fonts/he.ttf",
              "no font in the list maps 1 character (script Latn: '\xe2\x98\x83' U+2603); add a font that has it (a fallback" },
            { "abc \xe2\x91\xa0\xe2\x91\xa1\xe2\x91\xa2\xe2\x91\xa3\xe2\x91\xa4 \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d "
              "\xe2\x91\xa5\xe2\x91\xa6\xe2\x91\xa7\xe2\x91\xa8\xe2\x91\xa9", "fonts/la.ttf", "fonts/he.ttf",
              "no font in the list maps 10 characters (script Latn: '\xe2\x91\xa0' U+2460, '\xe2\x91\xa1' U+2461, '\xe2\x91\xa2' U+2462, "
              "'\xe2\x91\xa3' U+2463, '\xe2\x91\xa4' U+2464; script Hebr: '\xe2\x91\xa5' U+2465, '\xe2\x91\xa6' U+2466, "
              "'\xe2\x91\xa7' U+2467 and 2 more); add a font that has them (a fallback" },
        };
        int t;
        for (t = 0; t < 3; t++) {
            char stim[512], extra[1024];
            snprintf(stim, sizeof stim, "\"s\": {\"kind\": \"text\", \"font\": \"%s\", \"size\": 20, \"text\": \"%s\"}", cases[t].font, cases[t].text);
            snprintf(extra, sizeof extra, ", {\"kind\": \"font\", \"source\": \"fonts/he.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/he.ttf\"},"
                     " {\"kind\": \"font\", \"source\": \"fonts/la.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/la.ttf\"},"
                     " {\"kind\": \"glyphruns\", \"name\": \"exp/unmapped.ysprun\", \"fonts\": [\"%s\"]}", cases[t].fb);
            remove(WS "/unmapped.ysppak");
            rc = build_def("unmapped", stim, extra, WS "/unmapped.ysppak", err, sizeof err);
            CHECK(rc < 0 && strstr(err, cases[t].says), "unmapped %d: rc %d, '%s' (wanted '%s')", t, rc, err, cases[t].says);
            CHECK(fopen(WS "/unmapped.ysppak", "rb") == NULL, "unmapped %d: a failed build left a file", t);
        }
    }
    /* refusals, each by its message; none may leave a pack (a file from
     * an earlier run is removed first, so the last check sees this run) */
    remove(WS "/x.ysppak");
    {
        char desc[4096], says[256];
        const char* k = skey("fonts/NotoSans-Regular.ttf", 72, 1000, "RED");
        snprintf(desc, sizeof desc, STROOP_DESC(",\n  {\"kind\": \"glyphruns\", \"name\": \"experiment/stroop.ysprun\", \"fonts\": [\"fonts/NotoSans-Regular.ttf\"],\n"
                                                "   \"blocks\": [{\"key\": \"%s\", \"text\": \"Red\", \"size\": \"30\"}]}"), k);
        snprintf(says, sizeof says, "key '%s' is a text of stimulus 'word', derived from the experiment", k);
        CHECK(build_ws(desc, WS "/x.ysppak", err, sizeof err) < 0 && strstr(err, says), "a listed block with a derived key: %s", err);
    }
    CHECK(build_ws(STROOP_DESC(",\n  {\"kind\": \"glyphruns\", \"name\": \"experiment/stroop.ysprun\", \"blocks\": [{\"key\": \"p\", \"text\": \"P\", \"size\": \"30\"}]}"),
                   WS "/x.ysppak", err, sizeof err) < 0 && strstr(err, "'fonts' lists 1 to 64 font entries"),
          "listed blocks without listed fonts: %s", err);
    CHECK(build_ws(STROOP_DESC(",\n  {\"kind\": \"file\", \"name\": \"experiment/stroop.ysprun\", \"source\": \"stroop.csv\"}"), WS "/x.ysppak", err,
                   sizeof err) < 0 && strstr(err, "'experiment/stroop.ysprun': the experiment's glyph runs take this name; give the file another"),
          "a resource on the runs' name: %s", err);
    CHECK(build_ws(STROOP_DESC(",\n  {\"kind\": \"file\", \"name\": \"experiment/Stroop.ysprun\", \"source\": \"stroop.csv\"}"), WS "/x.ysppak", err,
                   sizeof err) < 0 && strstr(err, "the experiment's glyph runs: the same name without regard to case"),
          "a resource on the runs' name in another case: %s", err);
    {
        char desc[2048];
        snprintf(desc, sizeof desc, "%s", STROOP_DESC(""));
        *strstr(desc, ",\n  {\"kind\": \"curveset\"") = 0;
        strcat(desc, "]}\n");
        CHECK(build_ws(desc, WS "/x.ysppak", err, sizeof err) < 0 && strstr(err, "no curveset entry is made from 'fonts/NotoSans-Regular.ttf' face 0"),
              "no curve set for the stimuli's font: %s", err);
        *strstr(desc, ",\n  {\"kind\": \"font\"") = 0;
        strcat(desc, "]}\n");
        CHECK(build_ws(desc, WS "/x.ysppak", err, sizeof err) < 0 &&
              strstr(err, "experiment 'experiment/stroop.json': stimulus 'banner': 'font': no pack entry named 'fonts/NotoSans-Regular.ttf' (line"),
              "no font entry for the stimuli's font: %s", err);
    }
    CHECK(build_def("builtin", "\"s\": {\"kind\": \"text\", \"font\": \"fonts/NotoSans-Regular.ttf\", \"size\": 20, \"text\": \"$trial_index\"}", "",
                    WS "/x.ysppak", err, sizeof err) < 0 &&
          strstr(err, "experiment 'exp/builtin.json': stimulus 's': 'text': '$trial_index' is a built-in variable"), "a built-in as text: %s", err);
    /* the loader's refusals stop the build, with the place */
    CHECK(build_def("bad_key", "\"s\": {\"kind\": \"shape\", \"bogus\": 1}", "", WS "/x.ysppak", err, sizeof err) < 0 &&
          strstr(err, "experiment 'exp/bad_key.json': stimulus 's': unknown key 'bogus' (line "), "the loader refuses an unknown key: %s", err);
    CHECK(build_def("number_column", "\"s\": {\"kind\": \"text\", \"font\": \"fonts/NotoSans-Regular.ttf\", \"size\": 20, \"text\": \"$weight\"}", "",
                    WS "/x.ysppak", err, sizeof err) < 0 && strstr(err, "'$weight' is a column of 'stroop.pstb' of type integer"),
          "a number column as text: %s", err);
    CHECK(build_def("missing", "\"s\": {\"kind\": \"text\", \"font\": \"fonts/NotoSans-Regular.ttf\", \"size\": 20, \"text\": \"mail@example\"}", "",
                    WS "/x.ysppak", err, sizeof err) < 0 && strstr(err, "'exp/missing.ysprun' block 'fonts/NotoSans-Regular.ttf#0|20|0|0|start|auto||mail@example': no font in the list maps 1 character (script Latn: '@' U+0040); add a font that has it") &&
          strstr(err, "a fallback in 'fonts'"), "a derived text the font lacks: %s", err);
    CHECK(fopen(WS "/x.ysppak", "rb") == NULL, "a refused build left a file");
#endif
}

/* --- the lookups' keys at build (ypt_check_lookups) ---------------------------
 * The Stroop definition with a level missing from a map, and with a key no
 * level reaches. Both tools check, before any layout. */

/* WS/defs/NAME.json: the Stroop definition with one text replaced; a
 * description for it at WS/NAME.desc.json. */
static int stroop_variant(const char* name, const char* find, const char* repl, const ypt_options* o, char* err, size_t cap) {
    size_t n;
    uint8_t* d = read_all(WS "/stroop.json", &n);
    char* at;
    char path[256], desc[2048];
    char* out;
    int rc;
    if (!d) { CHECK(0, "%s: no Stroop definition", name); return -1; }
    d[n] = 0;
    at = strstr((char*)d, find);
    if (!at) { CHECK(0, "%s: the Stroop definition has '%s'", name, find); free(d); return -1; }
    out = (char*)malloc(n + strlen(repl) + 1);
    sprintf(out, "%.*s%s%s", (int)(at - (char*)d), (char*)d, repl, at + strlen(find));
    snprintf(path, sizeof path, WS "/defs/%s.json", name);
    put_text(path, out);
    free(out);
    free(d);
    snprintf(desc, sizeof desc,
             "{\"format\": \"ysp-pack-source\", \"version\": 1,\n"
             " \"experiment\": {\"entry\": \"experiment/stroop.json\", \"source\": \"defs/%s.json\"},\n"
             " \"resources\": [{\"name\": \"stroop.pstb\", \"kind\": \"table\", \"source\": \"stroop.csv\"},\n"
             "  {\"kind\": \"font\", \"source\": \"fonts/NotoSans-Regular.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/NotoSans-Regular.ttf\"}]}\n",
             name);
    rc = ypt_build_text(desc, strlen(desc), WS, WS "/variant.ysppak", o, err, cap);
    return rc;
}

static void test_lookup_pack(void) {
    char err[1024];
    ypt_options o;
    FILE* wf;
    size_t n;
    uint8_t* w;
    int rc;
    remove(WS "/variant.ysppak");
    rc = stroop_variant("nomap", ", \"blue\": \"BLUE\"", "", NULL, err, sizeof err);
    CHECK(rc < 0 && strstr(err, "experiment 'experiment/stroop.json': stimulus 'word': 'text': the lookup's map has no key for \"blue\", "
                                "which '$word' can take in node 'main'"), "a map without a level: %s", err);
    CHECK(fopen(WS "/variant.ysppak", "rb") == NULL, "a map without a level left a pack");
    rc = stroop_variant("nooutcome", "\"timeout\": \"Too slow\"", "\"slow\": \"Too slow\"", NULL, err, sizeof err);
    CHECK(rc < 0 && strstr(err, "stimulus 'feedback': 'text': the lookup's map has no key for \"timeout\""), "outcome without timeout: %s", err);
    /* a key no level reaches: a warning, and the pack builds */
    memset(&o, 0, sizeof o);
    wf = fopen(WS "/warn.txt", "wb");
    o.warn = wf;
    rc = stroop_variant("purple", "\"blue\": \"BLUE\"", "\"blue\": \"BLUE\", \"purple\": \"PURPLE\"", &o, err, sizeof err);
    if (wf) fclose(wf);
    w = read_all(WS "/warn.txt", &n);
    if (w) w[n] = 0;
    CHECK(w && strstr((char*)w, "warning: experiment 'experiment/stroop.json': stimulus 'word': 'text': no level of '$word' reaches the "
                                "map's key \"purple\"; a typo, or a level the trials no longer have? (line "), "the warning: '%s'", w ? (char*)w : "");
    free(w);
#ifdef YPT_NO_LAYOUT
    CHECK(rc < 0 && strstr(err, "glyph runs need the layout library"), "the core tool: the lookups first, then the runs: %s", err);
#else
    CHECK(rc == 0, "a key no level reaches builds: %s", err);
    /* a rebuild checks again, and says so */
    wf = fopen(WS "/warn2.txt", "wb");
    o.warn = wf;
    rc = ypt_rebuild_ex(WS "/variant.ysppak", WS, NULL, &o, NULL, err, sizeof err);
    if (wf) fclose(wf);
    w = read_all(WS "/warn2.txt", &n);
    if (w) w[n] = 0;
    CHECK(rc == 0 && w && strstr((char*)w, "warning: experiment 'experiment/stroop.json': stimulus 'word': 'text': no level of '$word' reaches"),
          "the rebuild's warning: %s '%s'", err, w ? (char*)w : "");
    free(w);
    /* the rules through ytr_rules(): a bad statement stops the build */
    rc = stroop_variant("badrule", "\"order constrained\"", "\"order sideways\"", NULL, err, sizeof err);
    CHECK(rc < 0 && strstr(err, "node 'main': rules: "), "the host's rules check: %s", err);
    /* quiet without a warn file */
    rc = stroop_variant("purple", "\"blue\": \"BLUE\"", "\"blue\": \"BLUE\", \"purple\": \"PURPLE\"", NULL, err, sizeof err);
    CHECK(rc == 0, "a key no level reaches, no warn file: %s", err);
#endif
}

/* The pack both builds make: the core and the full tool must give the same
 * bytes, which needs a manifest that names no layout library. */
/* A definition of format version 2 (ysp/experiment.h 0.3.0, tool 0.9.0):
 * format 3.18's visuomotor example with its table builds, and its entry is
 * the loader's canonical bytes. */
static void test_v2_pack(void) {
    char err[512];
    int rc;
    static const char def[] =
        "{\"format\": \"ysp-experiment\", \"version\": 2,\n"
        " \"roles\": {\"hand\": {\"type\": \"position2d\", \"units\": \"display_px\", \"allow_relative\": true}},\n"
        " \"stimuli\": {\"start\": {\"kind\": \"shape\"}, \"target\": {\"kind\": \"shape\", \"x\": \"$target_x\", \"y\": \"$target_y\"}, \"dot\": {\"kind\": \"shape\"}},\n"
        " \"trials\": {\"reach\": {\n"
        "   \"controls\": {\"at_start\": {\"control\": \"region\", \"of\": \"hand\", \"radius\": 12, \"stay_s\": 0.5},\n"
        "                \"past\": {\"control\": \"region\", \"of\": \"hand\", \"radius\": 268.8, \"outside\": true, \"rate\": \"sample\"},\n"
        "                \"moved\": {\"control\": \"movement\", \"of\": \"hand\", \"onset\": 26.88, \"from\": \"at_start\"}},\n"
        "   \"states\": [{\"name\": \"back\", \"steps\": [{\"on\": \"start\"}, {\"set\": \"dot\", \"props\": {\"x\": {\"signal\": \"hand.x\"}}}],\n"
        "                \"exits\": [{\"on\": \"at_start\", \"to\": \"go\"}]},\n"
        "              {\"name\": \"go\", \"steps\": [{\"on\": \"target\"}], \"exits\": [{\"on\": \"past\", \"to\": \"end\"}]}],\n"
        "   \"response\": {\"choices\": [{\"signal\": \"moved\"}], \"from\": \"target\"},\n"
        "   \"columns\": {\"cross_s\": {\"time\": \"past\", \"from\": \"target\"}}}},\n"
        " \"timeline\": [{\"name\": \"b\", \"timeline_variables\": \"targets.pstb\", \"trial\": \"reach\", \"rules\": [\"order random\"]}]}\n";
    put_text(WS "/targets.csv", "target_deg,target_x,target_y\n0,268.8,0\n90,0,-268.8\n");
    put_text(WS "/defs/v2.json", def);
    rc = build_ws("{\"format\": \"ysp-pack-source\", \"version\": 1, \"experiment\": {\"entry\": \"exp/v2.json\", \"source\": \"defs/v2.json\"},"
                  " \"resources\": [{\"name\": \"targets.pstb\", \"kind\": \"table\", \"source\": \"targets.csv\"}]}", WS "/v2.ysppak", err, sizeof err);
    CHECK(rc == 0, "a version 2 definition builds: %s", err);
#ifndef YPT_NO_LAYOUT
    if (rc == 0) {
        size_t n = 0;
        uint8_t* x = entry_bytes(WS "/v2.ysppak", "exp/v2.json", &n);
        static yexp_report rp;
        yjs_arena ca;
        yexp_def* dd = NULL;
        const char* cb;
        size_t cn = 0;
        yjs_arena_init_heap(&ca, 0);
        CHECK(yexp_load(&ca, def, strlen(def), NULL, &dd, &rp) == YEXP_OK && dd->format_version == 2 && yexp_canonical(dd, &ca, &cb, &cn, NULL) == YEXP_OK &&
              x && n == cn && !memcmp(x, cb, cn), "the version 2 entry is its canonical bytes");
        yjs_arena_free(&ca);
        free(x);
    }
#endif
    /* a version 2 definition the loader refuses stops the build with its message */
    put_text(WS "/defs/v2bad.json", "{\"format\": \"ysp-experiment\", \"version\": 2, \"signals\": {}, \"trials\": {\"t\": {}}, \"timeline\": []}\n");
    rc = build_ws("{\"format\": \"ysp-pack-source\", \"version\": 1, \"experiment\": {\"entry\": \"exp/v2bad.json\", \"source\": \"defs/v2bad.json\"}, \"resources\": []}",
                  WS "/v2bad.ysppak", err, sizeof err);
    CHECK(rc < 0 && strstr(err, "version 3"), "a version 2 refusal names the loader's reason: %s", err);
}

static void test_core_pack(void) {
    char err[1024];
    int rc = build_text(g_desc_core, W "/core.ysppak", err, sizeof err);
    CHECK(rc == 0, "core pack: %s", err);
    if (rc) return;
    CHECK(ypt_verify(W "/core.ysppak", NULL, err, sizeof err) == 0, "core pack verifies: %s", err);
    CHECK(names_layout(W "/core.ysppak") == 0, "the core pack's manifest names no layout library");
#ifndef YPT_NO_LAYOUT
    CHECK(names_layout(W "/corpus.ysppak") == 1, "a pack with glyph runs names the layout library");
#endif
}

/* --- color glyphs (tool v0.11.0, pack format 2) ------------------------------- */

#define EM_GRIN "\xF0\x9F\x98\x80"
#define EM_FAMILY "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"

static void write_cfb(const char* path, const cfb_buf* b) { write_file(path, b->d, b->n); }

#ifndef YPT_NO_LAYOUT
/* The table tags of a single font's directory, as one string. */
static void font_tags(const uint8_t* d, size_t n, char* out, size_t cap) {
    uint32_t nt = n >= 12 ? ((uint32_t)d[4] << 8 | d[5]) : 0, i;
    size_t k = 0;
    out[0] = 0;
    for (i = 0; i < nt && 12 + 16 * (size_t)i + 4 <= n && k + 6 < cap; i++) {
        memcpy(out + k, d + 12 + 16 * i, 4);
        out[k + 4] = ' ';
        k += 5;
        out[k] = 0;
    }
}
#endif

#define COLOR_DESC(fonts, csets, runfonts, text) \
    "{\"format\": \"ysp-pack-source\", \"version\": 1, \"resources\": [" fonts csets \
    " {\"name\": \"text/emoji.ysprun\", \"kind\": \"glyphruns\", \"fonts\": [" runfonts "],\n" \
    "  \"blocks\": [{\"key\": \"k1\", \"text\": \"" text "\", \"size\": \"48\"}]}]}\n"

#ifndef YPT_NO_LAYOUT
/* The pack's color checks: format 2, a GLYPHRUNS of version 2 with a paint
 * run, a CURVESET with the color flag whose first texels are the layout
 * library's paints and that holds every glyph the paint run names. */
static void color_pack_checks(const char* path, const char* font_path, const char* cset_name, int font_index) {
    ypak_pack p;
    ypak_desc d;
    ypak_entry e, ce;
    ypak_runs r;
    ypak_cset cs;
    char err[300];
    uint32_t i, paint_runs = 0, items_ok = 1;
    memset(&d, 0, sizeof d);
    d.path = path;
    if (!ypak_open(&p, &d)) { CHECK(0, "%s: %s", path, ypak_error(&p)); return; }
    CHECK(p.format == 2, "%s: pack format %d, want 2", path, (int)p.format);
    if (ypak_find(&p, "text/emoji.ysprun", &e) == 0 && ypak_find(&p, cset_name, &ce) == 0) {
        const uint8_t* rd = (const uint8_t*)ypak_data(&p, &e);
        const void* cd = ypak_data(&p, &ce);
        CHECK(rd && rd[8] == 2, "%s: glyph runs version %d, want 2", path, rd ? rd[8] : -1);
        CHECK(ypak_runs_view(rd, e.size, &r, err, sizeof err) == 0, "%s: runs view: %s", path, err);
        CHECK(ypak_cset_view(cd, ce.size, &cs, err, sizeof err) == 0 && (cs.flags & YPAK_CSET_COLOR), "%s: color set: %s, flags %u", path, err, cs.flags);
        {
            ygfx_cset_desc gd;
            char msg[300];
            memset(&gd, 0, sizeof gd);
            gd.texels = cs.texels; gd.n_texels = cs.n_texels; gd.words = cs.words; gd.n_words = cs.n_words;
            CHECK(ygfx_cset_check(&gd, msg, sizeof msg) == YGFX_OK, "%s: the color set's check: %s", path, msg);
        }
        {   /* its first texels are the layout library's paints */
            size_t fn;
            uint8_t* fb = read_all(font_path, &fn);
            yol_ctx ol;
            ylay_lib* L;
            const float* pt;
            int np;
            yol_open(&ol, NULL);
            ylay_create(&L, &(ylay_desc){ .ol = &ol });
            ylay_add_font(L, fb, fn, 0, "f");
            np = ylay_color_paints(L, 0, &pt);
            CHECK(np > 0 && (uint32_t)np <= cs.n_texels && !memcmp(pt, cs.texels, 16 * (size_t)np), "%s: the set's first texels are not "
                  "the %d paint texels", path, np);
            for (i = 0; i < r.n_runs; i++) {
                const ypak_runs_run* q = &r.runs[i];
                uint32_t k;
                if (!(q->flags & YPAK_RUN_PAINT)) continue;
                paint_runs++;
                CHECK(q->font == font_index, "%s: a paint run in font %u, want %d", path, q->font, font_index);
                for (k = 0; k < q->n; k++) {
                    const float* it = r.items + 8 * ((size_t)r.blocks[0].item_first + q->first + k);
                    uint32_t g = (uint32_t)it[3], pi = (uint32_t)it[7];
                    if (g >= cs.n_glyphs || !cs.words[8 + g] || (int)pi >= np || it[7] != (float)pi) items_ok = 0;
                }
            }
            ylay_destroy(L);
            yol_close(&ol);
            free(fb);
        }
        CHECK(paint_runs == 1 && items_ok, "%s: %u paint runs (want 1); every layer's glyph in the set and paint a record: %d", path,
              paint_runs, (int)items_ok);
    } else CHECK(0, "%s: no glyph runs or no curve set entry", path);
    ypak_close(&p);
}
#endif

static void test_color_pack(void) {
    cfb_buf f0 = { 0, 0, 0 }, f1 = { 0, 0, 0 }, fx = { 0, 0, 0 };
    static cfb_font cf;
#ifndef YPT_NO_LAYOUT
    char err[1024];
    int rc;
#endif
    cfb_emoji_v0(&f0);
    cfb_emoji_v1(&f1);
    MKDIR(W "/fonts");
    write_cfb(W "/fonts/emoji0.ttf", &f0);
    write_cfb(W "/fonts/emoji1.ttf", &f1);
    /* bitmaps and no COLR: refused at the font entry */
    cfb_init(&cf, 1000);
    cfb_add_glyph(&cf);
    cfb_box(&cf.g[1], 100, 0, 900, 800);
    cfb_map(&cf, CFB_GRIN, 1);
    cfb_extra(&cf, "CBDT"); cfb_extra(&cf, "CBLC");
    cfb_write(&cf, &fx);
    write_cfb(W "/fonts/cbdt.ttf", &fx);
    expect_refusal("CBDT font", "{\"kind\": \"font\", \"source\": \"fonts/cbdt.ttf\"}", "CBDT bitmaps and it has no COLR table", 0);
#ifdef YPT_NO_LAYOUT
    expect_refusal("COLR curve set, core tool",
                   "{\"kind\": \"font\", \"source\": \"fonts/emoji0.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/emoji0.ttf\"}",
                   "needs the layout library", 0);
#else
    {   /* COLR with CBDT and SVG: the font entry keeps COLR, drops the rest */
        ypak_pack p;
        ypak_desc d;
        ypak_entry e;
        char tags[256];
        cfb_buf both = { 0, 0, 0 };
        static cfb_font cb;
        int g;
        cfb_init(&cb, 1000);
        for (g = 1; g < 3; g++) cfb_add_glyph(&cb);
        cfb_box(&cb.g[1], 100, 0, 900, 800); cfb_box(&cb.g[2], 200, 100, 800, 700);
        cfb_map(&cb, CFB_GRIN, 1);
        cfb_color(&cb, 0xFF0000FFu);
        { int gl = 2, pl = 0; cfb_v0(&cb, 1, 1, &gl, &pl); }
        cfb_extra(&cb, "CBDT"); cfb_extra(&cb, "CBLC"); cfb_extra(&cb, "SVG ");
        cfb_write(&cb, &both);
        write_cfb(W "/fonts/both.ttf", &both);
        rc = build_text("{\"format\": \"ysp-pack-source\", \"version\": 1, \"resources\": [{\"kind\": \"font\", \"source\": \"fonts/both.ttf\"}]}",
                        W "/both.ysppak", err, sizeof err);
        CHECK(rc == 0, "COLR with CBDT and SVG: %s", err);
        memset(&d, 0, sizeof d);
        d.path = W "/both.ysppak";
        if (rc == 0 && ypak_open(&p, &d)) {
            const uint8_t* x = ypak_find(&p, "fonts/both.ttf", &e) == 0 ? (const uint8_t*)ypak_data(&p, &e) : NULL;
            const uint8_t* man = ypak_find(&p, "ysp/manifest.json", &e) == 0 ? (const uint8_t*)ypak_data(&p, &e) : NULL;
            ypak_entry fe;
            ypak_find(&p, "fonts/both.ttf", &fe);
            if (x) font_tags(x, (size_t)fe.size, tags, sizeof tags);
            CHECK(x && strstr(tags, "COLR") && strstr(tags, "CPAL") && !strstr(tags, "CBDT") && !strstr(tags, "CBLC") && !strstr(tags, "SVG ") &&
                  (size_t)fe.size < both.n && p.format == 1,
                  "COLR with CBDT and SVG: stored tables '%s', %lld of %zu bytes, format %d", tags, (long long)fe.size, both.n, (int)p.format);
            CHECK(man && strstr((const char*)man, "\"drop_tables\"") && strstr((const char*)man, "\"CBDT\""), "the font entry's derivation records the drop");
            {
                yol_font yf;
                char m[200];
                CHECK(x && yol_font_view(&yf, x, (size_t)fe.size, 0, m, sizeof m) == 0, "the stripped font opens: %s", m);
            }
            ypak_close(&p);
        }
        free(both.d);
        CHECK(ypt_verify(W "/both.ysppak", NULL, err, sizeof err) == 0 && ypt_rebuild(W "/both.ysppak", W, NULL, NULL, err, sizeof err) == 0,
              "COLR with CBDT and SVG: verify and rebuild: %s", err);
    }
    {   /* COLR version 0: used glyphs, then all */
        static const char* desc_used = COLOR_DESC(
            "{\"kind\": \"font\", \"source\": \"fonts/box.ttf\"}, {\"kind\": \"font\", \"source\": \"fonts/emoji0.ttf\"},\n",
            " {\"kind\": \"curveset\", \"from\": \"fonts/box.ttf\", \"glyphs\": \"used\"},"
            " {\"kind\": \"curveset\", \"from\": \"fonts/emoji0.ttf\", \"glyphs\": \"used\"},\n",
            "\"fonts/box.ttf\", \"fonts/emoji0.ttf\"", "a" EM_GRIN "b " EM_FAMILY);
        static const char* desc_all = COLOR_DESC(
            "{\"kind\": \"font\", \"source\": \"fonts/box.ttf\"}, {\"kind\": \"font\", \"source\": \"fonts/emoji0.ttf\"},\n",
            " {\"kind\": \"curveset\", \"from\": \"fonts/box.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/emoji0.ttf\"},\n",
            "\"fonts/box.ttf\", \"fonts/emoji0.ttf\"", "a" EM_GRIN "b " EM_FAMILY);
        rc = build_text(desc_used, W "/emoji0.ysppak", err, sizeof err);
        CHECK(rc == 0, "emoji v0 pack: %s", err);
        if (rc == 0) {
            color_pack_checks(W "/emoji0.ysppak", W "/fonts/emoji0.ttf", "fonts/emoji0.yspcset", 1);
            CHECK(build_text(desc_used, W "/emoji0b.ysppak", err, sizeof err) == 0 && same_file(W "/emoji0.ysppak", W "/emoji0b.ysppak"),
                  "emoji v0 pack twice: the same bytes");
            CHECK(ypt_verify(W "/emoji0.ysppak", NULL, err, sizeof err) == 0, "emoji v0 pack verifies: %s", err);
            rc = ypt_rebuild(W "/emoji0.ysppak", W, W "/emoji0r.ysppak", NULL, err, sizeof err);
            CHECK(rc == 0 && same_file(W "/emoji0.ysppak", W "/emoji0r.ysppak"), "emoji v0 pack rebuilds: %s", err);
        }
        rc = build_text(desc_all, W "/emoji0all.ysppak", err, sizeof err);
        CHECK(rc == 0, "emoji v0 pack, all glyphs: %s", err);
        if (rc == 0) color_pack_checks(W "/emoji0all.ysppak", W "/fonts/emoji0.ttf", "fonts/emoji0.yspcset", 1);
    }
    {   /* COLR version 1: layer glyphs (a transform, a clip box) in a used set */
        char text[64], desc[2048];
        int n = 0;
        uint32_t cps[3] = { CFB1_CP(CFB1_B_XFORM), CFB1_CP(CFB1_B_CLIP), CFB1_CP(CFB1_B_LINEAR) }, k;
        for (k = 0; k < 3; k++) {   /* U+E00x in UTF-8 */
            text[n++] = (char)(0xE0 | cps[k] >> 12); text[n++] = (char)(0x80 | (cps[k] >> 6 & 63)); text[n++] = (char)(0x80 | (cps[k] & 63));
        }
        text[n] = 0;
        snprintf(desc, sizeof desc, COLOR_DESC("{\"kind\": \"font\", \"source\": \"fonts/box.ttf\"}, {\"kind\": \"font\", \"source\": \"fonts/emoji1.ttf\"},\n",
                                               " {\"kind\": \"curveset\", \"from\": \"fonts/box.ttf\", \"glyphs\": \"used\"},"
                                               " {\"kind\": \"curveset\", \"from\": \"fonts/emoji1.ttf\", \"glyphs\": \"used\"},\n",
                                               "\"fonts/box.ttf\", \"fonts/emoji1.ttf\"", "%s"), text);
        rc = build_text(desc, W "/emoji1.ysppak", err, sizeof err);
        CHECK(rc == 0, "emoji v1 pack: %s", err);
        if (rc == 0) color_pack_checks(W "/emoji1.ysppak", W "/fonts/emoji1.ttf", "fonts/emoji1.yspcset", 1);
        /* a refused glyph: its codepoint, glyph, feature and advice */
        n = 0;
        cps[0] = CFB1_CP(CFB1_B_SOFT);
        text[n++] = (char)(0xE0 | cps[0] >> 12); text[n++] = (char)(0x80 | (cps[0] >> 6 & 63)); text[n++] = (char)(0x80 | (cps[0] & 63));
        text[n] = 0;
        snprintf(desc, sizeof desc, COLOR_DESC("{\"kind\": \"font\", \"source\": \"fonts/box.ttf\"}, {\"kind\": \"font\", \"source\": \"fonts/emoji1.ttf\"},\n",
                                               " {\"kind\": \"curveset\", \"from\": \"fonts/box.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/emoji1.ttf\"},\n",
                                               "\"fonts/box.ttf\", \"fonts/emoji1.ttf\"", "%s"), text);
        rc = build_text(desc, W "/refused.ysppak", err, sizeof err);
        CHECK(rc < 0 && strstr(err, "U+E010 (glyph 16)") && strstr(err, "SOFT_LIGHT") && strstr(err, "Noto-COLRv1-noflags.ttf") &&
              strstr(err, "fonts/emoji1.ttf") && strstr(err, "block 'k1'"),
              "a soft light glyph: rc %d, '%s'", rc, err);
    }
    {   /* a pack without color glyphs stays format 1 */
        ypak_pack p;
        ypak_desc d;
        memset(&d, 0, sizeof d);
        d.path = W "/corpus.ysppak";
        CHECK(ypak_open(&p, &d) && p.format == 1, "the corpus pack: format %d, want 1", (int)p.format);
        ypak_close(&p);
    }
#endif
    free(f0.d); free(f1.d); free(fx.d);
}

#if defined(__linux__)
/* The ffmpeg step's child (tool v0.12.0, part K2): only descriptors 0 to
 * 2, SCHED_OTHER, SIGPIPE at its default. This test program stands in
 * for ffmpeg: with YPT_FAKE_FFMPEG_LOG set, main() comes here, appends
 * "fds 0 1 2 policy 0 sigpipe 0" (what it has) to that file, prints a
 * version line and ends. */
static int fake_ffmpeg(const char* log) {
    int fds[64], n = 0, i, policy = sched_getscheduler(0);
    struct sigaction sa;
    DIR* d = opendir("/proc/self/fd");
    FILE* f;
    if (d) {
        struct dirent* de;
        while ((de = readdir(d)) != NULL && n < 64) {
            int v;
            if (de->d_name[0] == '.') continue;
            v = atoi(de->d_name);
            if (v != dirfd(d)) fds[n++] = v;
        }
        closedir(d);
    }
    memset(&sa, 0, sizeof sa);
    sigaction(SIGPIPE, NULL, &sa);
    f = fopen(log, "a");
    if (f) {
        fprintf(f, "fds");
        for (i = 0; i < n; i++) fprintf(f, " %d", fds[i]);
        fprintf(f, " policy %d sigpipe %d\n", policy, sa.sa_handler == SIG_IGN);
        fclose(f);
    }
    printf("ffmpeg version 0.0-fdtest Copyright (c) the test\n");
    return 0;
}

static int int_cmp(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }

/* Each line of the log: its descriptors sorted are 0 1 2, the policy and
 * SIGPIPE as wanted. The number of good lines, or -1 for a bad one. */
static int fds_ok(const char* log, int policy) {
    size_t n;
    uint8_t* d = read_all(log, &n);
    char* line;
    char* save = NULL;
    int good = 0;
    if (!d) return -1;
    d[n] = 0;
    for (line = strtok_r((char*)d, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        int v[64], k = 0, pol = -1, sp = -1;
        char* tok;
        char* s2 = NULL;
        for (tok = strtok_r(line, " ", &s2); tok; tok = strtok_r(NULL, " ", &s2)) {
            if (!strcmp(tok, "fds")) continue;
            if (!strcmp(tok, "policy")) { tok = strtok_r(NULL, " ", &s2); pol = tok ? atoi(tok) : -1; continue; }
            if (!strcmp(tok, "sigpipe")) { tok = strtok_r(NULL, " ", &s2); sp = tok ? atoi(tok) : -1; continue; }
            if (k < 64) v[k++] = atoi(tok);
        }
        qsort(v, (size_t)k, sizeof v[0], int_cmp);
        if (k != 3 || v[0] != 0 || v[1] != 1 || v[2] != 2 || pol != policy || sp != 0) { printf("ffmpeg's child: %s\n", log); good = -1; break; }
        good++;
    }
    free(d);
    return good;
}

static void test_ffmpeg_child(void) {
    static const char t[] =
        "{\"format\": \"ysp-pack-source\", \"version\": 1, \"resources\": [\n"
        "  {\"kind\": \"video\", \"name\": \"video/fd.mpg\", \"source\": \"readme.txt\", \"ffmpeg\": [\"-c:v\", \"mpeg1video\", \"-f\", \"mpeg\"]}]}\n";
    char self[1024], err[1024];
    ssize_t k = readlink("/proc/self/exe", self, sizeof self - 1);
    ypt_options o;
    FILE* extra;
    int rc, lines;
    struct sigaction ign, old;
    if (k <= 0) { CHECK(0, "no /proc/self/exe"); return; }
    self[k] = 0;
    /* a descriptor the tool did not open, without close-on-exec, and
     * SIGPIPE ignored, as a GUI program has them */
    extra = fopen(W "/extra_fd.txt", "w");
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ign, &old);
    remove(W "/fake_ffmpeg.log");
    setenv("YPT_FAKE_FFMPEG_LOG", W "/fake_ffmpeg.log", 1);
    memset(&o, 0, sizeof o);
    o.ffmpeg = self;
    rc = ypt_build_text(t, strlen(t), W, W "/fd.ysppak", &o, err, sizeof err);
    lines = fds_ok(W "/fake_ffmpeg.log", SCHED_OTHER);
    CHECK(rc < 0 && lines == 2, "ffmpeg's child (-version, then the encode) has only descriptors 0 to 2, SCHED_OTHER and SIGPIPE at its default (%d lines): %s", lines, err);
    {   /* from a SCHED_FIFO thread (an rtprio grant; skipped without) */
        struct sched_param sp, sp0;
        int pol0 = 0;
        pthread_getschedparam(pthread_self(), &pol0, &sp0);
        memset(&sp, 0, sizeof sp);
        sp.sched_priority = 1;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) {
            remove(W "/fake_ffmpeg.log");
            rc = ypt_build_text(t, strlen(t), W, W "/fd.ysppak", &o, err, sizeof err);
            pthread_setschedparam(pthread_self(), pol0, &sp0);
            lines = fds_ok(W "/fake_ffmpeg.log", SCHED_OTHER);
            CHECK(rc < 0 && lines == 2, "ffmpeg's child of a SCHED_FIFO thread: SCHED_OTHER (%d lines)", lines);
        } else {
            printf("tool_test: ffmpeg's child from a SCHED_FIFO thread skipped (no rtprio grant)\n");
        }
    }
    unsetenv("YPT_FAKE_FFMPEG_LOG");
    sigaction(SIGPIPE, &old, NULL);
    if (extra) fclose(extra);
    CHECK(!exists(W "/fd.ysppak") && !exists(W "/fd.ysppak.tmp"), "the fake ffmpeg's build leaves no pack");
}
#endif

int main(void) {
    char err[1024];
    int rc;
#if defined(__linux__)
    if (getenv("YPT_FAKE_FFMPEG_LOG")) return fake_ffmpeg(getenv("YPT_FAKE_FFMPEG_LOG"));
#endif
    make_sources();
    rc = build_text(g_desc, W "/corpus.ysppak", err, sizeof err);
    CHECK(rc == 0, "build: %s", err);
    if (rc == 0) {
        FILE* rep;
        check_entries(W "/corpus.ysppak");
        gfx_round_trip(W "/corpus.ysppak");
        /* the same bytes again */
        CHECK(build_text(g_desc, W "/again.ysppak", err, sizeof err) == 0 && same_file(W "/corpus.ysppak", W "/again.ysppak"), "build twice: same bytes");
        /* verify */
        CHECK(ypt_verify(W "/corpus.ysppak", stdout, err, sizeof err) == 0, "verify: %s", err);
        /* rebuild from the manifest */
        rc = ypt_rebuild(W "/corpus.ysppak", W, W "/rebuilt.ysppak", stdout, err, sizeof err);
        CHECK(rc == 0 && same_file(W "/corpus.ysppak", W "/rebuilt.ysppak"), "rebuild: %s", err);
        /* a changed source: rebuild names the entry */
        {
            size_t n;
            uint8_t* d = read_all(W "/readme.txt", &n);
            write_file(W "/readme.txt", "changed\n", 8);
            rc = ypt_rebuild(W "/corpus.ysppak", W, NULL, NULL, err, sizeof err);
            CHECK(rc < 0 && strstr(err, "readme.txt"), "rebuild with a changed source: %s", err);
            if (d) { write_file(W "/readme.txt", d, n); free(d); }
        }
        /* append to a program */
        {
            static uint8_t prog[10001];
            ypak_pack p;
            ypak_desc d;
            memset(prog, 0x90, sizeof prog);
            write_file(W "/player.bin", prog, sizeof prog);
            CHECK(ypt_append(W "/player.bin", W "/corpus.ysppak", W "/player_with_pack.bin", err, sizeof err) == 0, "append: %s", err);
            memset(&d, 0, sizeof d);
            d.path = W "/player_with_pack.bin";
            d.verify = YPAK_VERIFY_OPEN;
            rc = ypak_open(&p, &d) ? 0 : p.error_code;
            CHECK(rc == 0 && p.zip_start == 12288 && p.base == 0, "appended pack opens: %s", ypak_error(&p));
            if (rc == 0) {
                ypak_pack q;
                d.path = W "/corpus.ysppak";
                CHECK(ypak_open(&q, &d) && memcmp(q.id, p.id, 32) == 0, "appended pack ID is the same");
                ypak_close(&q);
                ypak_close(&p);
            }
            CHECK(ypt_verify(W "/player_with_pack.bin", NULL, err, sizeof err) == 0, "appended verify: %s", err);
            CHECK(ypt_append(W "/player.bin", W "/player_with_pack.bin", W "/twice.bin", err, sizeof err) < 0, "append twice refused");
        }
        /* extract and cat */
        {
            const char* names[2] = { "conditions.pstb", "img/gray.ysptex" };
            ypak_pack p;
            ypak_desc d;
            ypak_entry e;
            size_t n;
            uint8_t* x;
            MKDIR(W "/out");
            remove(W "/out/conditions.pstb");
            CHECK(ypt_extract(W "/corpus.ysppak", names, 2, W "/out", 1, NULL, err, sizeof err) == 0, "extract: %s", err);
            CHECK(ypt_extract(W "/corpus.ysppak", names, 1, W "/out", 0, NULL, err, sizeof err) < 0 && strstr(err, "exists"), "extract over a file");
            memset(&d, 0, sizeof d);
            d.path = W "/corpus.ysppak";
            x = read_all(W "/out/img/gray.ysptex", &n);
            if (ypak_open(&p, &d)) {
                const void* y = ypak_find(&p, "img/gray.ysptex", &e) == 0 ? ypak_data(&p, &e) : NULL;
                CHECK(x && y && n == (size_t)e.size && memcmp(x, y, n) == 0, "extracted bytes");
                ypak_close(&p);
            }
            free(x);
            rep = fopen(W "/list.txt", "w");
            CHECK(rep && ypt_list(W "/corpus.ysppak", rep, err, sizeof err) == 0 && ypt_info(W "/corpus.ysppak", rep, err, sizeof err) == 0, "list, info");
            if (rep) fclose(rep);
            x = read_all(W "/list.txt", &n);
            CHECK(x && strstr((const char*)x, N_ENTRIES == 31u ? "glyphruns" : "curveset") && strstr((const char*)x, "experiment: experiment/main.json") &&
                  strstr((const char*)x, "audio: 48000 Hz"), "list and info text");
            free(x);
        }
        /* damage: an entry's data, the manifest's text, the comment */
        {
            ypak_pack p;
            ypak_desc d;
            ypak_entry e;
            long at = 0;
            memset(&d, 0, sizeof d);
            d.path = W "/corpus.ysppak";
            if (ypak_open(&p, &d)) {
                if (ypak_find(&p, "readme.txt", &e) == 0) at = (long)e.data_off + 3;
                ypak_close(&p);
            }
            flip_copy(W "/corpus.ysppak", W "/bad1.ysppak", at);
            CHECK(ypt_verify(W "/bad1.ysppak", NULL, err, sizeof err) == YPAK_ERR_CORRUPT, "damaged data: %s", err);
            flip_copy(W "/corpus.ysppak", W "/bad2.ysppak", 4096 + 10);
            CHECK(ypt_verify(W "/bad2.ysppak", NULL, err, sizeof err) < 0, "damaged manifest: %s", err);
            flip_copy(W "/corpus.ysppak", W "/bad3.ysppak", -3);
            CHECK(ypt_verify(W "/bad3.ysppak", NULL, err, sizeof err) < 0, "damaged comment: %s", err);
        }
        /* the corpus summary for the comparison across compilers */
        {
            ypak_pack p;
            ypak_desc d;
            FILE* f = fopen("pack_corpus.txt", "w");
            uint32_t i;
            char id[72];
            memset(&d, 0, sizeof d);
            d.path = W "/corpus.ysppak";
            if (f && ypak_open(&p, &d)) {
                ypak_id(&p, id);
                fprintf(f, "pack %s\n", id);
                for (i = 0; i < p.n; i++) {
                    ypak_entry e;
                    uint8_t sha[32];
                    int k;
                    ypak_at(&p, i, &e);
                    ypak_sha256(ypak_data(&p, &e), (size_t)e.size, sha);
                    fprintf(f, "%.*s ", (int)e.name_len, e.name);
                    for (k = 0; k < 32; k++) fprintf(f, "%02x", sha[k]);
                    fprintf(f, "\n");
                }
                printf("tool_test: corpus %s\n", id);
                /* every entry but the manifest (which names the library
                 * versions) against tests/pack/corpus.sha256, so each
                 * compiler and OS must make the same bytes */
                {
                    char pin_path[1024], line[1200];
                    FILE* pin;
                    int lines = 0, same = 0;
                    snprintf(pin_path, sizeof pin_path, "%s/corpus.sha256", YPT_TEST_DIR);
                    pin = fopen(pin_path, "r");
                    CHECK(pin != NULL, "no %s", pin_path);
                    while (pin && fgets(line, sizeof line, pin)) {
                        char hx[65], name[1024];
                        ypak_entry e;
                        uint8_t sha[32];
                        char got[65];
                        int k;
                        if (sscanf(line, "%64s %1023s", hx, name) != 2) continue;
#ifdef YPT_NO_LAYOUT
                        /* no glyph runs, so another chunk list */
                        if (!strcmp(name, "text/pages.ysprun") || !strcmp(name, "ysp/chunks")) continue;
#endif
                        lines++;
                        if (ypak_find(&p, name, &e) != 0) { CHECK(0, "pinned entry %s is not in the corpus pack", name); continue; }
                        ypak_sha256(ypak_data(&p, &e), (size_t)e.size, sha);
                        for (k = 0; k < 32; k++) snprintf(got + 2 * k, 3, "%02x", sha[k]);
                        if (strcmp(got, hx) == 0) same++;
                        else printf("corpus pin: %s is %s, pinned %s (if the change is intended, update %s from pack_corpus.txt)\n",
                                    name, got, hx, pin_path);
                    }
                    if (pin) fclose(pin);
                    CHECK(lines == (int)p.n - 1 - (N_ENTRIES == 31u ? 0 : 1) && same == lines, "corpus pin: %d of %d entries as pinned (%u in the pack besides the manifest)",
                          same, lines, p.n - 1);
                }
                ypak_close(&p);
            }
            if (f) fclose(f);
        }
        test_cancel();
        test_cache();
    }
    test_cache_parse();
    test_core_pack();
    test_stroop_pack();
    test_lookup_pack();
    test_v2_pack();
    test_resample();
    test_ffmpeg_refusals();
#if defined(__linux__)
    test_ffmpeg_child();
#endif
    test_video();
    test_refusals();
    test_color_pack();
    printf("tool_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
