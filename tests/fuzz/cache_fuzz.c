/* cache_fuzz.c - fuzz target for the pack tool's entry cache records
 * (pack/pack_tool.c, ypt_cache_parse(); docs/pack.md 19). A record is a
 * file that a build reads back from a folder other programs can write, so
 * its bytes are untrusted input. The record's reader is ypt_cache_parse();
 * a part's derivation then goes through ysp/json.h (json_fuzz.c), and a
 * part's bytes become an entry's bytes, which the tool does not read.
 *
 * Every input must return without a sanitizer report. A record that
 * parses must have its parts inside the input, a part marked as the
 * source's with no bytes, and nothing after its last part. In the build
 * mode a record of a mutated or a whole but wrong form sits where a
 * build looks for it, and the build must still succeed: a record can cost
 * a miss, never a failed build.
 *
 * Self-driven, gcc: CMake target cache_fuzz (YSP_BUILD_PACK, tests on):
 * random records, then mutations; half of them get their size, XXH64 and
 * key mended, so the mutation reaches the part table. On this kernel ASan
 * needs setarch -R:
 *     setarch -R build/sanitizers/pack/cache_fuzz 300000 [seed [build]]
 * With "build", every 16th input is also a build's record (an 8-bit WAV
 * that the tool converts; the folder ./cache_fuzz_work).
 * Replay: cache_fuzz --replay FILES.
 * libFuzzer, clang: link the pack tool (as CMake's target does) without
 * YSP_FUZZ_SELF; the first input byte 0 parses with no key, 1 with the
 * key in the input, others with a key the input does not hold.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if (defined(__linux__) || defined(__EMSCRIPTEN__)) && !defined(_DEFAULT_SOURCE) && !defined(_GNU_SOURCE) && !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
#define _DEFAULT_SOURCE 1   /* opendir under -std=c11 */
#endif
#include "ysp/pack_tool.h"
#include "ysp/pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <dirent.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

static long g_inputs, g_parsed, g_parts, g_builds, g_hits;

static void fail(const char* what) {
    fprintf(stderr, "cache_fuzz: %s\n", what);
    abort();
}

static uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void parse_one(const uint8_t* d, size_t n, const uint8_t* key) {
    ypt_cache_part p[4];
    int r, i;
    uint64_t at;
    g_inputs++;
    memset(p, 0, sizeof p);
    r = ypt_cache_parse(d, n, key, p, 4);
    if (r > 4) fail("more parts than the room given");
    if (r <= 0) return;
    g_parsed++;
    g_parts += r;
    at = 64 + 32 * (uint64_t)r;
    for (i = 0; i < r; i++) {
        const uint8_t* dv = (const uint8_t*)p[i].deriv;
        if (dv < d || dv + p[i].deriv_len > d + n) fail("a derivation outside the record");
        if ((uint64_t)(dv - d) != at) fail("a derivation not where its table entry puts it");
        at += (p[i].deriv_len + 15) & ~(uint64_t)15;
        if (p[i].source && (p[i].data || p[i].size)) fail("a source part with bytes");
        if (!p[i].source && (!p[i].data || p[i].data < d || (uint64_t)(p[i].data - d) != at || p[i].size > n - (size_t)(p[i].data - d)))
            fail("a part's bytes outside the record");
        at += (p[i].size + 15) & ~(uint64_t)15;
    }
    if (at != n) fail("bytes after the last part");
    if (key && memcmp(d + 16, key, 32)) fail("another key taken");
    if (rd64(d + 48) != n) fail("a size field that is not the record's");
}

#if defined(YSP_FUZZ_SELF)
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t mix(void) {
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static void w32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void w64(uint8_t* p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }
static void mend(uint8_t* d, size_t n, const uint8_t* key) {
    if (n < 64) return;
    memcpy(d + 16, key, 32);
    w64(d + 48, n);
    w64(d + 56, ypak_xxh64(d + 64, n - 64, 0));
}

static const char* const g_derivs[] = {
    "{\"op\": \"convert\", \"by\": \"ysp/audio.h\", \"stream\": false}",
    "{}", "[]", "{\"op\": 1.5e308}", "{\"a\": {\"b\": [1, 2, {\"c\": null}]}}", "{\"op\": \"\\u00e9\"}", "not json", "",
};

/* A record whose form is right (parts, padding, size, XXH64, key). */
static size_t make(uint8_t* d, size_t cap, const uint8_t* key) {
    int np = 1 + (int)(mix() % 4), i;
    size_t at = 64 + 32 * (size_t)np;
    if (at > cap) return 0;
    memset(d, 0, cap < 4096 ? cap : 4096);
    memcpy(d, "YSPCACHE", 8);
    w32(d + 8, 1);
    w32(d + 12, (uint32_t)np);
    for (i = 0; i < np; i++) {
        uint8_t* t = d + 64 + 32 * i;
        const char* dv = g_derivs[mix() % (sizeof g_derivs / sizeof g_derivs[0])];
        size_t dl = strlen(dv), sz = mix() % 3 ? (size_t)(mix() % 300) : 0, k;
        int src = mix() % 4 == 0;
        if (src) sz = 0;
        if (at + dl + 16 + sz + 16 > cap) return 0;
        w32(t, mix() % 4 ? 0u : 1u);
        w32(t + 4, src ? 1u : 0u);
        w64(t + 8, dl);
        w64(t + 16, sz);
        memcpy(d + at, dv, dl);
        at += dl;
        while (at & 15) d[at++] = 0;
        for (k = 0; k < sz; k++) d[at + k] = (uint8_t)mix();
        at += sz;
        while (at & 15) d[at++] = 0;
    }
    mend(d, at, key);
    return at;
}

static void mutate(uint8_t* d, size_t* n, size_t cap) {
    int edits = 1 + (int)(mix() % 4), e;
    for (e = 0; e < edits && *n; e++) {
        size_t at = mix() % 2 ? (size_t)(mix() % (*n < 64 + 128 ? *n : 64 + 128)) : (size_t)(mix() % *n);
        switch (mix() % 7) {
        case 0: case 1: d[at] = (uint8_t)mix(); break;
        case 2: d[at] ^= (uint8_t)(1u << (mix() % 8)); break;
        case 3: d[at] = (uint8_t)(mix() % 2 ? d[at] + 1 : d[at] - 1); break;   /* lengths and counts one off */
        case 4: if (at + 8 <= *n) { uint64_t v = mix() % 3 ? mix() % 64 : ~(uint64_t)0 - mix() % 64; w64(d + at, v); } break;
        case 5: *n = at; break;                                                  /* cut */
        default: if (*n + 16 <= cap) { memset(d + *n, 0, 16); *n += 16; } break; /* grown */
        }
    }
}

/* The build mode: a description whose one entry is cached, its record's
 * path, and the pack without a cache. */
#define WD "cache_fuzz_work"
static char g_rec[256];
static uint8_t* g_ref;
static size_t g_ref_n;
static const char g_desc[] = "{\"format\": \"ysp-pack-source\", \"version\": 1, \"audio\": {\"rate\": 48000, \"channels\": 1}, \"resources\": ["
                             "{\"kind\": \"audio\", \"name\": \"u8.wav\", \"source\": \"u8.wav\"}]}";

static uint8_t* slurp(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    uint8_t* d;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (uint8_t*)malloc((size_t)sz + 1);
    if (d && fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); d = NULL; }
    fclose(f);
    if (d) *n = (size_t)sz;
    return d;
}

static void spit(const char* path, const void* d, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) fail("cannot write a file in " WD);
    if (n && fwrite(d, 1, n, f) != n) fail("cannot write a file in " WD);
    fclose(f);
}

static int build(const char* out, const char* cache, char* err, size_t cap, FILE* log) {
    ypt_options o;
    memset(&o, 0, sizeof o);
    o.cache = cache;
    o.log = log;
    return ypt_build_text(g_desc, strlen(g_desc), WD, out, &o, err, cap);
}

/* The one record in the folder (64 hex digits and ".yspce"). */
static int find_record(const char* dir, char* out, size_t cap) {
    int found = 0;
#if defined(_WIN32)
    char pat[300];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*.yspce", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (strlen(fd.cFileName) == 70) { snprintf(out, cap, "%.150s/%.80s", dir, fd.cFileName); found++; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* dd = opendir(dir);
    struct dirent* de;
    if (!dd) return 0;
    while ((de = readdir(dd)) != NULL)
        if (strlen(de->d_name) == 70 && !strcmp(de->d_name + 64, ".yspce")) { snprintf(out, cap, "%.150s/%.80s", dir, de->d_name); found++; }
    closedir(dd);
#endif
    return found;
}

static void build_setup(uint8_t key[32]) {
    uint8_t wav[44 + 100];
    char err[512];
    size_t n = 0;
    uint8_t* d;
    int i;
    MKDIR(WD);
    memcpy(wav, "RIFF", 4); w32(wav + 4, 36 + 100); memcpy(wav + 8, "WAVEfmt ", 8); w32(wav + 16, 16);
    wav[20] = 1; wav[21] = 0; wav[22] = 1; wav[23] = 0; w32(wav + 24, 48000); w32(wav + 28, 48000); wav[32] = 1; wav[33] = 0; wav[34] = 8; wav[35] = 0;
    memcpy(wav + 36, "data", 4); w32(wav + 40, 100);
    for (i = 0; i < 100; i++) wav[44 + i] = (uint8_t)(128 + (i % 20) * 5);
    spit(WD "/u8.wav", wav, sizeof wav);
    if (build(WD "/ref.ysppak", NULL, err, sizeof err, NULL)) fail(err);
    g_ref = slurp(WD "/ref.ysppak", &g_ref_n);
    if (ypt_cache_trim(WD "/c", 0, NULL, err, sizeof err) == 0) {}   /* a cold folder */
    if (build(WD "/cold.ysppak", WD "/c", err, sizeof err, NULL)) fail(err);
    if (find_record(WD "/c", g_rec, sizeof g_rec) != 1) fail("the cold build did not write one record");
    d = slurp(g_rec, &n);
    if (!d || n < 64) fail("the record does not read");
    memcpy(key, d + 16, 32);
    free(d);
}

/* A build with rec where the build looks: it must succeed, and give the
 * pack without a cache unless the record was taken. */
static void build_one(const uint8_t* rec, size_t n) {
    char err[512];
    size_t m = 0;
    uint8_t* d;
    FILE* log = fopen(WD "/log.txt", "w");
    int rc;
    spit(g_rec, rec, n);
    rc = build(WD "/fz.ysppak", WD "/c", err, sizeof err, log);
    if (log) fclose(log);
    g_builds++;
    if (rc) { fprintf(stderr, "cache_fuzz: %s\n", err); fail("a build failed on a cache record"); }
    d = slurp(WD "/log.txt", &m);
    if (d) {
        d[m] = 0;
        if (strstr((const char*)d, "(cache)")) g_hits++;
        else {
            size_t pn = 0;
            uint8_t* pk = slurp(WD "/fz.ysppak", &pn);
            if (!pk || pn != g_ref_n || memcmp(pk, g_ref, pn)) fail("a miss gave another pack");
            free(pk);
        }
        free(d);
    }
}

int main(int argc, char** argv) {
    static uint8_t buf[1 << 14];
    uint8_t key[32];
    long iters = argc > 1 ? atol(argv[1]) : 10000, it;
    int builds = 0, i;
    if (argc > 1 && !strcmp(argv[1], "--replay")) {
        for (i = 2; i < argc; i++) {
            size_t n = 0;
            uint8_t* d = slurp(argv[i], &n);
            if (!d) { fprintf(stderr, "cannot read %s\n", argv[i]); return 1; }
            parse_one(d, n, NULL);
            if (n >= 48) parse_one(d, n, d + 16);
            free(d);
        }
        printf("cache_fuzz: replayed %d files\n", argc - 2);
        return 0;
    }
    if (argc > 2) g_rng = (uint64_t)strtoull(argv[2], NULL, 10);
    if (argc > 3) builds = !strcmp(argv[3], "build");
    for (i = 0; i < 32; i++) key[i] = (uint8_t)mix();
    if (builds) build_setup(key);
    for (it = 0; it < iters; it++) {
        size_t n = make(buf, sizeof buf, key);
        int mended = mix() % 2;
        if (!n) continue;
        if (mix() % 8) mutate(buf, &n, sizeof buf);
        if (mended) mend(buf, n, key);
        parse_one(buf, n, key);
        parse_one(buf, n, NULL);
        if (builds && it % 16 == 0) build_one(buf, n);
    }
    printf("cache_fuzz: %ld parses, %ld records whole (%ld parts); %ld builds, %ld from a record\n", g_inputs, g_parsed, g_parts, g_builds, g_hits);
    free(g_ref);
    return 0;
}
#else
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static const uint8_t other[32] = { 1 };
    if (!size) return 0;
    if (data[0] == 0) parse_one(data + 1, size - 1, NULL);
    else if (data[0] == 1 && size >= 49) parse_one(data + 1, size - 1, data + 1 + 16);
    else parse_one(data + 1, size - 1, other);
    return 0;
}
#endif
