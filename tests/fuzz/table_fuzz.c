/* table_fuzz.c - fuzz target for ysp/table.h. The first input byte
 * picks the target, the rest is its input:
 *   even  CSV: ytb_open_csv() (the second byte picks the delimiter, allow_empty
 *         and one declared type), then ytb_open_view() of a copy of the block
 *         it made and every lookup on every cell
 *   odd   block: ytb_open_view() of the raw bytes (copied to 8-byte aligned
 *         memory, as a pack gives), then every lookup
 * Every input must return without a sanitizer report. A refusal is a
 * correct answer to a bad input, so the target asserts nothing else.
 *
 * libFuzzer, MSVC 2022 x64 (from the repository root, in a vcvars shell):
 *     cl /nologo /O1 /Zi /fsanitize=address /fsanitize=fuzzer /Iinclude tests\fuzz\table_fuzz.c
 *     table_fuzz.exe -max_total_time=1200 -timeout=10 corpus_table
 * libFuzzer, clang:
 *     clang -O1 -g -fsanitize=fuzzer,address,undefined -Iinclude tests/fuzz/table_fuzz.c -o table_fuzz -lm
 * Seeds (no corpus is committed; the target writes its own):
 *     cc -DYSP_FUZZ_SEEDS -Iinclude tests/fuzz/table_fuzz.c -o seeds -lm && ./seeds corpus_table
 * Replay of a corpus without libFuzzer (gcc or clang under UBSan), every
 * file of the directory as an argument:
 *     gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
 *         -DYSP_FUZZ_REPLAY -Iinclude tests/fuzz/table_fuzz.c -o replay -lm
 *     ./replay corpus_table/<files>
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_TABLE_IMPLEMENTATION
#include "ysp/table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_arena[(8u << 20) / 8];
static uint64_t g_copy[(4u << 20) / 8];
static volatile unsigned g_sink;

static void sweep(const ytb_table* t) {
    int r, c, l, nl;
    for (c = 0; c < t->n_cols; c++) {
        const char* nm = ytb_col_name(t, c);
        g_sink += (unsigned)strlen(nm) + (unsigned)ytb_col_type(t, c) + (unsigned)ytb_col(t, nm);
        nl = ytb_n_levels(t, c);
        for (l = 0; l < nl && l < 64; l++) {
            const char* tx = ytb_level_text(t, c, l);
            g_sink += (unsigned)strlen(tx) + (unsigned)ytb_find(t, c, tx);
            g_sink += (unsigned)(ytb_level_num(t, c, l) == 1.0);
        }
        for (r = 0; r < t->n_rows; r++) {
            g_sink += (unsigned)ytb_level(t, r, c);
            g_sink += (unsigned)strlen(ytb_text(t, r, c));
            g_sink += (unsigned)ytb_int(t, r, c);
        }
    }
    g_sink += (unsigned)ytb_hash(t);
}

static void fuzz_csv(const uint8_t* p, size_t n) {
    static const char delims[3] = { ',', ';', '\t' };
    ytb_csv_desc d;
    ytb_table t, v;
    memset(&d, 0, sizeof(d));
    if (n > 0) {
        d.delimiter = delims[p[0] % 3];
        d.allow_empty = (p[0] & 4) != 0;
        if (p[0] & 8) {
            d.types[0].name = "a";
            d.types[0].type = (ytb_type)((p[0] >> 4) & 3);
            d.n_types = 1;
        }
        p++;
        n--;
    }
    d.text = p;
    d.len = n;
    d.arena = g_arena;
    d.arena_size = sizeof(g_arena);
    if (!ytb_open_csv(&t, &d)) return;
    sweep(&t);
    if (t.size <= sizeof(g_copy)) {
        memcpy(g_copy, t.base, t.size);
        if (ytb_open_view(&v, g_copy, t.size)) sweep(&v);
    }
}

static void fuzz_view(const uint8_t* p, size_t n) {
    ytb_table v;
    if (n > sizeof(g_copy)) return;
    memcpy(g_copy, p, n);
    if (ytb_open_view(&v, g_copy, n)) sweep(&v);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    if (data[0] % 2 == 0) fuzz_csv(data + 1, size - 1);
    else fuzz_view(data + 1, size - 1);
    return 0;
}

#if defined(YSP_FUZZ_SEEDS)
/* Seed inputs: CSV files that reach every dialect rule, and the blocks
 * their parses make (as view inputs). */
static void put(const char* dir, const char* name, const void* a, size_t na, const void* b, size_t nb) {
    char path[1024];
    FILE* f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(a, 1, na, f);
    if (nb) fwrite(b, 1, nb, f);
    fclose(f);
}

int main(int argc, char** argv) {
    static const char* const csvs[] = {
        "a,b\n1,x\n2,y\n",
        "\xEF\xBB\xBFtarget,contrast,word\r\na,0.25,cat\r\nb,0.5,\"dog, big\"\r\n",
        "x\n\"say \"\"hi\"\"\"\n\"two\nlines\"\n",
        "n,m\n1e-300,2.2250738585072011e-308\n0.30000000000000004,9007199254740993\n",
        "a;b\n1,5;x\n",
        "a\tb\n1\t\"x\ty\"\n",
        "x,y\n,1\n,\n\n3,\n",
        "code\n007\n7\n-0\n0.0\n",
        "a,b\n1,\xC3\xA9t\xC3\xA9\n2,\xE4\xB8\xAD\n",
    };
    static const unsigned char opts[] = { 0, 1, 2, 4, 8, 8 | 16, 8 | 32, 8 | 48 };
    size_t i, k;
    char name[64];
    int nseed = 0;
    if (argc < 2) { fprintf(stderr, "usage: seeds DIR\n"); return 1; }
    for (i = 0; i < sizeof(csvs) / sizeof(csvs[0]); i++) {
        for (k = 0; k < sizeof(opts); k++) {
            unsigned char hdr[2];
            hdr[0] = 0;
            hdr[1] = opts[k];
            snprintf(name, sizeof(name), "csv%02u_%u", (unsigned)i, (unsigned)k);
            put(argv[1], name, hdr, 2, csvs[i], strlen(csvs[i]));
            nseed++;
        }
        {
            ytb_csv_desc d;
            ytb_table t;
            unsigned char one = 1;
            memset(&d, 0, sizeof(d));
            d.text = csvs[i];
            d.len = strlen(csvs[i]);
            d.arena = g_arena;
            d.arena_size = sizeof(g_arena);
            if (i == 4) d.delimiter = ';';
            if (i == 5) d.delimiter = '\t';
            if (ytb_open_csv(&t, &d)) {
                snprintf(name, sizeof(name), "block%02u", (unsigned)i);
                put(argv[1], name, &one, 1, t.base, t.size);
                nseed++;
            }
        }
    }
    printf("table_fuzz: wrote %d seeds to %s\n", nseed, argv[1]);
    return 0;
}
#elif defined(YSP_FUZZ_REPLAY)
int main(int argc, char** argv) {
    static uint8_t buf[1 << 20];
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
    printf("table_fuzz: replayed %d inputs\n", done);
    return 0;
}
#endif
