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
#ifndef YSCR_NO_SDL
#define YSCR_NO_SDL
#endif
#ifndef YAU_NO_MINIAUDIO
#define YAU_NO_MINIAUDIO
#endif
#include "ysp/pack_tool.h"
#include "ysp/pack.h"
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
#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#ifndef YPT_TEST_DIR
#define YPT_TEST_DIR "tests/pack"
#endif

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define W "pack_tool_work"

#include "../adapt/import_gen.h"
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

static void make_font(bb* out, int fs_type) {
    bb t[8];
    static const char* tags[8] = { "OS/2", "cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp" };
    uint32_t ng = 2, g, i, r, nt = fs_type >= 0 ? 8 : 7, first = fs_type >= 0 ? 0 : 1;
    size_t dir;
    memset(t, 0, sizeof t);
    for (r = 0; r < N_RANGES; r++) ng += font_ranges[r][1] - font_ranges[r][0] + 1;
    for (i = 0; i < 78; i++) b8(&t[0], 0);
    if (fs_type >= 0) { t[0].d[8] = (uint8_t)(fs_type >> 8); t[0].d[9] = (uint8_t)fs_type; }
    {
        bb* c = &t[1];
        uint32_t nseg = N_RANGES + 2, base = 2, sr = 1, es = 0;
        while (sr * 2 <= nseg) { sr *= 2; es++; }
        b16(c, 0); b16(c, 1); b16(c, 3); b16(c, 1); b32(c, 12);
        b16(c, 4); b16(c, 16 + 8 * nseg); b16(c, 0);
        b16(c, 2 * nseg); b16(c, 2 * sr); b16(c, es); b16(c, 2 * nseg - 2 * sr);
        b16(c, 0x20);
        for (r = 0; r < N_RANGES; r++) b16(c, font_ranges[r][1]);
        b16(c, 0xffff); b16(c, 0);
        b16(c, 0x20);
        for (r = 0; r < N_RANGES; r++) b16(c, font_ranges[r][0]);
        b16(c, 0xffff);
        b16(c, (1 - 0x20) & 0xffff);
        for (r = 0; r < N_RANGES; r++) {
            b16(c, (base - font_ranges[r][0]) & 0xffff);
            base += font_ranges[r][1] - font_ranges[r][0] + 1;
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
    expect_refusal("missing glyph",
                   "{\"kind\": \"font\", \"source\": \"fonts/box.ttf\"}, {\"kind\": \"curveset\", \"from\": \"fonts/box.ttf\"}, {\"kind\": \"glyphruns\", "
                   "\"name\": \"t.ysprun\", \"fonts\": [\"fonts/box.ttf\"], \"blocks\": [{\"key\": \"a\", \"text\": \"mail@example\", \"size\": \"10\"}]}",
                   "no font maps", 0);
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
    expect_refusal("mpeg dct", "{\"kind\": \"video\", \"name\": \"v.mpg\", \"source\": \"readme.txt\", \"ffmpeg\": [\"-c:v\", \"mpeg2video\", \"-dct\", \"fastint\"]}",
                   "remove '-dct fastint' from 'ffmpeg'", 0);
    expect_refusal("mpeg default encoder idct", "{\"kind\": \"video\", \"name\": \"v.mpg\", \"source\": \"readme.txt\", \"ffmpeg\": [\"-idct:v\", \"auto\"]}",
                   "remove '-idct:v auto' from 'ffmpeg'", 0);
    expect_refusal("mpeg bitexact off", "{\"kind\": \"video\", \"name\": \"v.mpg\", \"source\": \"readme.txt\", \"ffmpeg\": [\"-c:v\", \"mpeg1video\", \"-flags\", \"+cgop-bitexact\"]}",
                   "remove '-flags +cgop-bitexact' from 'ffmpeg'", 0);
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

/* The pack both builds make: the core and the full tool must give the same
 * bytes, which needs a manifest that names no layout library. */
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

int main(void) {
    char err[1024];
    int rc;
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
    }
    test_core_pack();
    test_resample();
    test_ffmpeg_refusals();
    test_video();
    test_refusals();
    printf("tool_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
