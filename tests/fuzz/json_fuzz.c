/* json_fuzz.c - fuzz target for ysp/json.h and ysp/rigfile.h's profile reader.
 * The first input byte picks the target, the rest is its input:
 *   0  yjs_parse() into a fixed arena (the scratch stack at its top); on
 *      success every accessor on every value, then the canonical text is
 *      parsed again and must give the same bytes (a failed round trip
 *      aborts)
 *   1  the same into a heap arena with a 64 KB limit and max_depth from
 *      the second byte
 *   2  yrig_parse() of a rig profile; on success its canonical bytes are
 *      parsed again and must give the same bytes
 *   3  yrig_read_latency() of an output latency file and
 *      yrig_read_loopback() of any loopback file (ysp/rigfile.h v0.4.0),
 *      which must agree on an output latency file; and
 *      yjs_parse_fixed() / yjs_parse_double() on the whole input, and
 *      yjs_format_double() of its first 8 bytes as a double (ysp/json.h
 *      v0.2.0): the text reads back to the double and is canonical
 * Targets 0 and 1 also check that the canonical text holds the same values:
 * every number reads the same through yjs_double(), yjs_fixed() and
 * yjs_int64() before and after (v0.2.0).
 * Every input must return without a sanitizer report. A refusal is a
 * correct answer to a bad input; only a broken round trip is a finding of
 * its own.
 *
 * libFuzzer, MSVC 2022 x64 (from the repository root, in a vcvars shell):
 *     cl /nologo /O1 /Zi /fsanitize=address /fsanitize=fuzzer /Iinclude tests\fuzz\json_fuzz.c
 *     json_fuzz.exe -max_total_time=1800 -timeout=10 -rss_limit_mb=4096 corpus_json
 * libFuzzer, clang:
 *     clang -O1 -g -fsanitize=fuzzer,address,undefined -Iinclude tests/fuzz/json_fuzz.c -o json_fuzz -lpthread -lm
 * Seeds (no corpus is committed): a few documents and profiles of each
 * target written to DIR:
 *     cc -DYSP_FUZZ_SEEDS -Iinclude tests/fuzz/json_fuzz.c -o seeds -lpthread -lm && ./seeds corpus_json
 * Replay of a corpus without libFuzzer (gcc or clang under UBSan):
 *     gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
 *         -DYSP_FUZZ_REPLAY -Iinclude tests/fuzz/json_fuzz.c -o replay -lpthread -lm
 *     ./replay corpus_json/<files>
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_RIGFILE_IMPLEMENTATION
#include "ysp/rigfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_mem[(1u << 20) / 8];
static uint64_t g_mem2[(4u << 20) / 8];
static char g_t1[2u << 20], g_t2[2u << 20];
static yrig_profile g_p;
static volatile unsigned g_sink;

static void touch(const yjs_value* v, int depth) {
    int64_t i;
    double d;
    bool b;
    uint32_t k;
    if (!v || depth > YJS_MAX_DEPTH) return;
    switch (v->type) {
    case YJS_NUMBER:
        g_sink += (unsigned)yjs_int64(v, &i) + (unsigned)yjs_double(v, &d) + (unsigned)yjs_fixed(v, 9, &i) + (unsigned)yjs_fixed(v, 0, &i);
        break;
    case YJS_STRING:
        g_sink += (unsigned)strlen(yjs_string(v, NULL)) + (unsigned)yjs_bool(v, &b);
        break;
    case YJS_ARRAY:
        for (k = 0; k < v->n; k++) touch(yjs_at(v, k), depth + 1);
        break;
    case YJS_OBJECT:
        for (k = 0; k < v->n; k++) {
            if (yjs_get_n(v, v->u.members[k].key, v->u.members[k].key_len) != v->u.members[k].value) abort();
            touch(v->u.members[k].value, depth + 1);
        }
        break;
    default:
        g_sink += (unsigned)yjs_bool(v, &b);
        break;
    }
}

/* v and its canonical text's parse w hold the same values: every number
 * reads the same through each accessor (canonical numbers are exact). */
static void same_values(const yjs_value* v, const yjs_value* w, int depth) {
    uint32_t k;
    if (!v || !w || depth > YJS_MAX_DEPTH) return;
    if (v->type != w->type || ((v->type == YJS_ARRAY || v->type == YJS_OBJECT) && v->n != w->n)) abort();
    switch (v->type) {
    case YJS_NUMBER: {
        static const int dec[] = { 0, 3, 9, 18 };
        double d1 = 0, d2 = 0;
        int64_t i1 = 0, i2 = 0;
        int r1 = yjs_double(v, &d1), r2 = yjs_double(w, &d2), j;
        if (r1 != r2 || (r1 == YJS_OK && memcmp(&d1, &d2, sizeof d1) != 0 && d1 != 0.0)) abort();
        for (j = 0; j < 4; j++) {
            r1 = yjs_fixed(v, dec[j], &i1);
            r2 = yjs_fixed(w, dec[j], &i2);
            if (r1 != r2 || (r1 == YJS_OK && i1 != i2)) abort();
        }
        /* integer text stays integer text with the same value */
        if (yjs_int64(v, &i1) == YJS_OK && (yjs_int64(w, &i2) != YJS_OK || i1 != i2)) abort();
    } break;
    case YJS_STRING:
        if (v->n != w->n || memcmp(v->u.s, w->u.s, v->n) != 0) abort();
        break;
    case YJS_ARRAY:
        for (k = 0; k < v->n; k++) same_values(v->u.items[k], w->u.items[k], depth + 1);
        break;
    case YJS_OBJECT:
        for (k = 0; k < v->n; k++) {
            if (strcmp(v->u.members[k].key, w->u.members[k].key) != 0) abort();
            same_values(v->u.members[k].value, w->u.members[k].value, depth + 1);
        }
        break;
    default:
        break;
    }
}

/* The canonical text of v parses to itself and to v's values. */
static void round_trip(const yjs_value* v) {
    yjs_arena a;
    yjs_value* w;
    yjs_error e;
    size_t n1 = yjs_write_mem(v, YJS_WRITE_PRETTY, g_t1, sizeof g_t1), n2;
    if (n1 == 0 || n1 >= sizeof g_t1) return;
    yjs_arena_init(&a, g_mem2, sizeof g_mem2);
    if (yjs_parse(&a, g_t1, n1, NULL, &w, &e) != YJS_OK) {
        if (e.code == YJS_ERR_NOMEM) return;
        abort();
    }
    n2 = yjs_write_mem(w, YJS_WRITE_PRETTY, g_t2, sizeof g_t2);
    if (n1 != n2 || memcmp(g_t1, g_t2, n1) != 0) abort();
    same_values(v, w, 0);
    n2 = yjs_write_mem(w, YJS_WRITE_COMPACT, g_t2, sizeof g_t2);
    if (n2 == 0 || n2 > n1) abort();
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const char* t;
    size_t n;
    yjs_arena a;
    yjs_value* v;
    yjs_error e;
    char err[256];
    if (size < 2) return 0;
    t = (const char*)data + 2;
    n = size - 2;
    switch (data[0] % 4) {
    case 0:
        yjs_arena_init(&a, g_mem, sizeof g_mem);
        if (yjs_parse(&a, t, n, NULL, &v, &e) == YJS_OK) {
            touch(v, 0);
            round_trip(v);
        } else if (e.code == YJS_OK || e.line == 0 || e.column == 0 || e.offset > n) {
            abort();
        }
        break;
    case 1: {
        yjs_opts o;
        memset(&o, 0, sizeof o);
        o.max_depth = data[1] % 80;
        yjs_arena_init_heap(&a, 64 * 1024);
        if (yjs_parse(&a, t, n, &o, &v, &e) == YJS_OK) {
            yjs_value* c = yjs_copy(&a, v);
            touch(v, 0);
            if (c) round_trip(c);
        }
        yjs_arena_free(&a);
    } break;
    case 2:
        if (yrig_parse(&g_p, t, n, NULL, err, sizeof err) == YRIG_OK) {
            size_t n1 = yrig_write(&g_p, g_t1, sizeof g_t1), n2;
            if (n1 == 0 || n1 >= sizeof g_t1 || yrig_parse(&g_p, g_t1, n1, NULL, err, sizeof err) != YRIG_OK) abort();
            n2 = yrig_write(&g_p, g_t2, sizeof g_t2);
            if (n1 != n2 || memcmp(g_t1, g_t2, n1) != 0) abort();
        } else if (!err[0] || g_p.n_roles != 0) {
            abort();
        }
        break;
    default: {
        yrig_loopback lb;
        int64_t i;
        double d;
        yrig_loopback lb2;
        int r1 = yrig_read_latency(t, n, &lb, err, sizeof err);
        int r2 = yrig_read_loopback(t, n, &lb2, err, sizeof err);
        /* one parser behind both: an output latency file reads the same through each */
        if ((r1 == YRIG_OK) != (r2 == YRIG_FILE_OUT_LATENCY)) abort();
        if (r1 == YRIG_OK && (lb.n != lb2.n || lb.median_ns != lb2.median_ns || lb.max_ns != lb2.max_ns || strcmp(lb.sha256, lb2.sha256) != 0))
            abort();
        if (r2 > YRIG_FILE_DISPLAY_ONSET || (r2 < 0 && r2 != YRIG_ERR_FORMAT)) abort();
        g_sink += (unsigned)r1 + (unsigned)r2;
        g_sink += (unsigned)yjs_parse_fixed(t, n, data[1] % 19, &i) + (unsigned)yjs_parse_double(t, n, &d);
        /* the input's first 8 bytes as a double: its canonical text reads
         * back to it, is one JSON number and is its own canonical text */
        if (n >= 8) {
            char c[YJS_DOUBLE_TEXT], c2[64];
            double x, y = 0;
            unsigned f;
            int len;
            yjs_arena a2;
            yjs_value* nv;
            memcpy(&x, t, sizeof x);
            len = yjs_format_double(x, c, sizeof c);
            if (x != x || x - x != 0.0) {
                if (len != YJS_ERR_RANGE) abort();
                break;
            }
            if (len <= 0 || len >= YJS_DOUBLE_TEXT || !yjs_number_ok(c, (size_t)len, &f)) abort();
            if (yjs_parse_double(c, (size_t)len, &y) != YJS_OK ? x != 0.0 : (memcmp(&x, &y, sizeof x) != 0 && x != 0.0)) abort();
            yjs_arena_init(&a2, g_mem, sizeof g_mem);
            nv = yjs_new_number(&a2, c, (size_t)len);
            if (!nv || yjs_write_mem(nv, YJS_WRITE_COMPACT, c2, sizeof c2) != (size_t)len || memcmp(c, c2, (size_t)len) != 0) abort();
        }
    } break;
    }
    return 0;
}

#if defined(YSP_FUZZ_SEEDS)
static void put(const char* dir, const char* name, uint8_t a, uint8_t b, const char* p) {
    char path[1024];
    FILE* f;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fputc(a, f);
    fputc(b, f);
    fwrite(p, 1, strlen(p), f);
    fclose(f);
}

int main(int argc, char** argv) {
    static const char* const docs[] = {
        "{\"a\": [1, -2.5e3, true, false, null, \"x\\u00e9\\ud83d\\ude00\"], \"b\": {\"c\": {}}, \"\": []}",
        "[0, -0, 1e400, 123456789012345678901234567890, 0.1, 9223372036854775807, -9223372036854775808]",
        "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"",
        "{\"k1\":1,\"k2\":2,\"k3\":3,\"k4\":4,\"k5\":5,\"k6\":6,\"k7\":7,\"k8\":8,\"k9\":9,\"k10\":10,\"k11\":11,\"k12\":12,\"k13\":13,\"k14\":14,"
        "\"k15\":15,\"k16\":16,\"k17\":17}",
    };
    static const char profile[] =
        "{\"display\": {\"calibration\": {\"file\": \"display.yspcal\", \"sha256\": "
        "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}, \"onset_offset_s\": 0.0123}, \"format\": \"ysp-rig 1\", "
        "\"rig\": \"booth-2\", \"roles\": {\"mm\": {\"family\": \"mmbts\", \"key\": \"serial:2341:0043::\", \"options\": {\"latched\": true}}, "
        "\"resp\": {\"bounds\": {\"date\": \"2026-10-08T09:00:00Z\", \"hi_s\": 0.0011, \"lo_s\": -0.0003, \"n\": 200, \"sha256\": "
        "\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}, \"family\": \"xid\", \"key\": \"serial:0403:6001:FT4ABC12:\", "
        "\"options\": {\"baud\": 115200, \"buttons\": {\"1\": \"left\", \"2\": \"right\"}, \"ftdi_latency_s\": 0.001}}, \"trig\": {\"family\": "
        "\"triggerbox\", \"key\": \"serial:0403:6001:TB0123:\", \"latency\": {\"date\": \"2026-10-09T13:58:12Z\", \"max_s\": 0.0021, "
        "\"median_s\": 0.000577, \"n\": 200, \"p5_s\": 0.000201, \"p95_s\": 0.000983, \"sha256\": "
        "\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"}, \"pulse_s\": 0.002}}, \"written\": \"2026-10-09T14:03:00Z\"}";
    static const char profile4[] =
        "{\"display\": {\"latency\": {\"date\": \"2026-10-13T10:05:00Z\", \"max_s\": 0.013, \"median_s\": 0.0123, \"n\": 1180, "
        "\"p5_s\": 0.01201, \"p95_s\": 0.01261, \"sha256\": \"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\"}, "
        "\"onset_offset_s\": 0.0123}, \"format\": \"ysp-rig 1\", \"rig\": \"v04\", \"roles\": {}, \"written\": \"2026-10-13T10:05:00Z\"}";
    static const char bnd[] = "format ysp-in-bounds 1\ndate 2026-10-13T10:00:00Z\nn 398\nlo_s -0.000312000\nhi_s 0.001104000\n";
    static const char ons[] = "format ysp-display-onset 1\ndate 2026-10-13T10:05:00Z\nn 1180\np5_s 0.012010000\nmedian_s 0.012300000\n"
                              "p95_s 0.012610000\nmax_s 0.013000000\nonset_offset_s 0.012300000\n";
    static const char lat[] ="format ysp-out-latency 1\ndate 2026-10-09T13:58:12Z\nn 200\np5_s 0.0002010\nmedian_s 0.0005770\n"
                              "p95_s 0.0009830\nmax_s 0.0021000\n";
    char name[32];
    size_t i;
    if (argc < 2) { fprintf(stderr, "usage: seeds DIR\n"); return 1; }
    for (i = 0; i < sizeof docs / sizeof docs[0]; i++) {
        snprintf(name, sizeof name, "doc%u_fixed", (unsigned)i);
        put(argv[1], name, 0, 0, docs[i]);
        snprintf(name, sizeof name, "doc%u_heap", (unsigned)i);
        put(argv[1], name, 1, 3, docs[i]);
    }
    put(argv[1], "profile", 2, 0, profile);
    put(argv[1], "profile_json", 0, 0, profile);
    put(argv[1], "latency", 3, 9, lat);
    put(argv[1], "fixed", 3, 9, "-1.5e-3");
    put(argv[1], "double", 3, 9, "¹?");       /* 0.1 */
    put(argv[1], "numbers", 0, 0, "[1.50, 1.5e0, -0.0, 15E-1, 0.00012300e-7, 1e999999999, 123456789012345678901234567890, 9223372036854775808]");
    put(argv[1], "profile4", 2, 0, profile4);
    put(argv[1], "bounds", 3, 9, bnd);
    put(argv[1], "onset", 3, 9, ons);
    printf("json_fuzz: wrote %u seeds to %s\n", (unsigned)(2 * (sizeof docs / sizeof docs[0]) + 9), argv[1]);
    return 0;
}
#elif defined(YSP_FUZZ_REPLAY)
int main(int argc, char** argv) {
    static uint8_t buf[2u << 20];
    int i, done = 0;
    for (i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");
        size_t n;
        if (!f) continue;
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        LLVMFuzzerTestOneInput(buf, n);
        done++;
    }
    printf("json_fuzz: replayed %d inputs\n", done);
    return 0;
}
#endif
