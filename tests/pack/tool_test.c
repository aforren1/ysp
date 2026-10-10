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
#ifndef YPT_NO_LAYOUT
#include "ysp/layout.h"
#endif

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

typedef struct bb { uint8_t* d; size_t n, cap; } bb;
static void b_need(bb* b, size_t k) {
    if (b->n + k > b->cap) {
        while (b->n + k > b->cap) b->cap = b->cap ? 2 * b->cap : 256;
        b->d = (uint8_t*)realloc(b->d, b->cap);
        if (!b->d) { printf("out of memory\n"); exit(1); }
    }
}
static void b8(bb* b, uint32_t v) { b_need(b, 1); b->d[b->n++] = (uint8_t)v; }
static void b16(bb* b, uint32_t v) { b8(b, v >> 8 & 255); b8(b, v & 255); }
static void b32(bb* b, uint32_t v) { b16(b, v >> 16); b16(b, v & 0xffff); }
static void bput(bb* b, const void* p, size_t n) { b_need(b, n); memcpy(b->d + b->n, p, n); b->n += n; }
static void le16(bb* b, uint32_t v) { b8(b, v & 255); b8(b, v >> 8 & 255); }
static void le32(bb* b, uint32_t v) { le16(b, v & 0xffff); le16(b, v >> 16); }
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
    if (iccp) {
        static const uint8_t pr[] = { 'x', 0, 0, 0x78, 0x01, 0x01, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01 };
        chunk(&b, "iCCP", pr, sizeof pr);
    }
    chunk(&b, "IDAT", z.d, z.n);
    chunk(&b, "IEND", NULL, 0);
    write_file(path, b.d, b.n);
    free(b.d); free(z.d); free(raw);
}

static uint8_t g_rgba[33 * 21 * 4], g_gray[17 * 9], g_g16[11 * 7 * 2], g_c16[5 * 4 * 6];

static void make_sources(void) {
    bb f;
    uint32_t i;
    MKDIR(W); MKDIR(W "/fonts"); MKDIR(W "/audio"); MKDIR(W "/img"); MKDIR(W "/art"); MKDIR(W "/shaders"); MKDIR(W "/data");
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
    "    {\"kind\": \"file\", \"source\": \"data/big.bin\"}\n" \
    "  ]\n" \
    "}\n"

/* The core description: every kind but glyph runs. */
static const char* g_desc_core = DESC_HEAD DESC_TAIL;
#ifdef YPT_NO_LAYOUT
static const char* g_desc = DESC_HEAD DESC_TAIL;
#define N_ENTRIES 21u
#else
static const char* g_desc = DESC_HEAD DESC_RUNS DESC_TAIL;
#define N_ENTRIES 22u
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
    rc = ypak_open(&p, &d);
    CHECK(rc == 0, "open %s: %s", path, ypak_error(&p));
    if (rc) return;
    CHECK(p.n == N_ENTRIES, "entries %u", p.n);
    CHECK(ypak_first_of(&p, YPAK_KIND_EXPERIMENT, &e) == 0 && e.name_len == 20 && !memcmp(e.name, "experiment/main.json", 20), "experiment");
    if ((x = entry(&p, "conditions.pstb", &e)) != NULL) {
        ytb_table t;
        CHECK(ytb_view(&t, x, (size_t)e.size) && t.n_rows == 4 && strcmp(ytb_text(&t, 1, ytb_col(&t, "word")), "dog, big") == 0 &&
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
    if (ypak_open(&p, &d) != 0 || !yscr_open(&scr, &sd)) { CHECK(0, "round trip: open"); return; }
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
    expect_refusal("kind", "{\"kind\": \"video\", \"source\": \"readme.txt\"}", "not one this tool builds", 0);
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
        if (rc == 0 && ypak_open(&p, &d) == 0) {
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
    if (ypak_open(&p, &d) != 0) return -1;
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
            rc = ypak_open(&p, &d);
            CHECK(rc == 0 && p.zip_start == 12288 && p.base == 0, "appended pack opens: %s", ypak_error(&p));
            if (rc == 0) {
                ypak_pack q;
                d.path = W "/corpus.ysppak";
                CHECK(ypak_open(&q, &d) == 0 && memcmp(q.id, p.id, 32) == 0, "appended pack ID is the same");
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
            if (ypak_open(&p, &d) == 0) {
                const void* y = ypak_find(&p, "img/gray.ysptex", &e) == 0 ? ypak_data(&p, &e) : NULL;
                CHECK(x && y && n == (size_t)e.size && memcmp(x, y, n) == 0, "extracted bytes");
                ypak_close(&p);
            }
            free(x);
            rep = fopen(W "/list.txt", "w");
            CHECK(rep && ypt_list(W "/corpus.ysppak", rep, err, sizeof err) == 0 && ypt_info(W "/corpus.ysppak", rep, err, sizeof err) == 0, "list, info");
            if (rep) fclose(rep);
            x = read_all(W "/list.txt", &n);
            CHECK(x && strstr((const char*)x, N_ENTRIES == 22u ? "glyphruns" : "curveset") && strstr((const char*)x, "experiment: experiment/main.json") &&
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
            if (ypak_open(&p, &d) == 0) {
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
            if (f && ypak_open(&p, &d) == 0) {
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
                    CHECK(lines == (int)p.n - 1 - (N_ENTRIES == 22u ? 0 : 1) && same == lines, "corpus pin: %d of %d entries as pinned (%u in the pack besides the manifest)",
                          same, lines, p.n - 1);
                }
                ypak_close(&p);
            }
            if (f) fclose(f);
        }
    }
    test_core_pack();
    test_refusals();
    printf("tool_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
