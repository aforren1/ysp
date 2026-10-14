/* import_fuzz.c - fuzz target for the pack tool's readers of other formats
 * (pack/import.c): the .xlsx reader (zip, inflate, XML, shared strings,
 * styles, the sheet), the JPEG reader (segments, EXIF, ICC chunks, then
 * stb_image's decode) and the ICC profile check. The first input byte
 * picks the target, the rest is its input:
 *   0  an .xlsx file, the first sheet
 *   1  an .xlsx file, the sheet named "Sheet2"
 *   2  a JPEG file, checked and decoded
 *   3  an ICC profile
 * Every input must return without a sanitizer report. A refusal is a
 * correct answer to a bad input; the target also checks that an accepted
 * sheet's CSV parses with ysp/table.h or is refused by it with a message,
 * and that a decoded JPEG has the size its header states.
 *
 * One translation unit: it includes pack/import.c, pack/stb_image.c and
 * lodepng (compiled as C), so no CMake target is needed.
 *
 * libFuzzer, MSVC 2022 x64 (from the repository root, in a vcvars shell,
 * after tools/vendor_pack.py --write):
 *     cl /nologo /O1 /Zi /fsanitize=address /fsanitize=fuzzer /Iinclude /Ipack
 *        /Ithird_party/lodepng /Ithird_party/stb tests\fuzz\import_fuzz.c
 *     import_fuzz.exe -max_total_time=3600 -timeout=10 -rss_limit_mb=4096 -max_len=40000 corpus_import
 * libFuzzer, clang:
 *     clang -O1 -g -fsanitize=fuzzer,address,undefined -Iinclude -Ipack -Ithird_party/lodepng
 *         -Ithird_party/stb tests/fuzz/import_fuzz.c -o import_fuzz -lm
 * Seeds (no corpus is committed): files given as arguments, each with the
 * target byte its extension picks (.xlsx 0 and 1, .jpg 2, .icc 3):
 *     cc -DYSP_FUZZ_SEEDS -Iinclude -Ipack -Ithird_party/lodepng -Ithird_party/stb
 *         tests/fuzz/import_fuzz.c -o seeds -lm && ./seeds corpus_import a.xlsx b.jpg
 * Replay of a corpus without libFuzzer (gcc or clang under UBSan):
 *     gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
 *         -DYSP_FUZZ_REPLAY -Iinclude -Ipack -Ithird_party/lodepng -Ithird_party/stb
 *         tests/fuzz/import_fuzz.c -o replay -lm
 *     ./replay corpus_import/<files>
 * A dump of what the readers make of files (the CSV of a sheet, a JPEG's
 * frame), for comparisons with other readers:
 *     cc -DYSP_IMPORT_DUMP ... -o dump && ./dump a.xlsx [SHEET] | b.jpg
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define LODEPNG_NO_COMPILE_CPP
#define LODEPNG_NO_COMPILE_ENCODER
#define LODEPNG_NO_COMPILE_DISK
#define YSP_PACK_IMPLEMENTATION
#define YPAK_NO_FILE
#include "ysp/pack.h"
#define YSP_TABLE_IMPLEMENTATION
#include "ysp/table.h"
#include "lodepng.cpp"
#include "stb_image.c"
#include "import.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sheet_target(const uint8_t* d, size_t n, const char* want) {
    ypt_sheet s;
    char err[512];
    if (ypt_xlsx_read(d, n, want, &s, err, sizeof err) == 0) {
        /* the text must be what ysp/table.h reads, or what it refuses by name */
        static uint64_t arena[1 << 18];
        ytb_csv_desc cd;
        ytb_table t;
        if (!s.csv || strlen(s.csv) != s.csv_len) abort();
        memset(&cd, 0, sizeof cd);
        cd.text = s.csv;
        cd.len = s.csv_len;
        cd.arena = arena;
        cd.arena_size = sizeof arena;
        if (!ytb_open_csv(&t, &cd) && !ytb_error(&t)[0] && t.need <= sizeof arena) abort();
        ypt_sheet_free(&s);
    } else if (!err[0] || s.csv) {
        abort();
    }
}

static void one(const uint8_t* data, size_t size) {
    char err[512];
    if (size < 1) return;
    switch (data[0] & 3) {
    case 0: sheet_target(data + 1, size - 1, NULL); break;
    case 1: sheet_target(data + 1, size - 1, "Sheet2"); break;
    case 2: {
        ypt_jpeg j;
        /* the decode only up to 4 megapixels: a larger frame is legal
         * (stb_image decodes 52 megapixels in about a second), but under
         * ASan it costs seconds an input; the checks before it all run */
        if (ypt_jpeg_read(data + 1, size - 1, 0, &j, err, sizeof err) < 0) {
            if (!err[0]) abort();
            break;
        }
        if ((int64_t)j.w * j.h > ((int64_t)1 << 22)) break;
        if (ypt_jpeg_read(data + 1, size - 1, 1, &j, err, sizeof err) == 0) {
            if (!j.px || j.w < 1 || j.h < 1 || (j.channels != 1 && j.channels != 4)) abort();
            /* touch every texel: a short buffer is a sanitizer report */
            {
                size_t k, n = (size_t)j.w * (size_t)j.h * (size_t)j.channels;
                volatile unsigned sum = 0;
                for (k = 0; k < n; k++) sum += j.px[k];
                (void)sum;
            }
            ypt_jpeg_free(&j);
        } else if (!err[0] || j.px) {
            abort();
        }
    } break;
    default: {
        int r = ypt_icc_srgb(data + 1, size - 1, err, sizeof err);
        if (r < 1 && !err[0]) abort();
    } break;
    }
}

#if defined(YSP_FUZZ_REPLAY) || defined(YSP_FUZZ_SEEDS) || defined(YSP_IMPORT_DUMP)
static uint8_t* load(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    uint8_t* d;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (uint8_t*)malloc((size_t)sz + 1);
    if (!d || fread(d + 1, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(d); return NULL; }
    fclose(f);
    *n = (size_t)sz + 1;
    return d;
}
#endif

#if defined(YSP_FUZZ_REPLAY)
int main(int argc, char** argv) {
    int i;
    for (i = 1; i < argc; i++) {
        size_t n;
        uint8_t* d = load(argv[i], &n);
        if (!d) { fprintf(stderr, "cannot read %s\n", argv[i]); return 1; }
        /* the file is the fuzzer's input, target byte included */
        one(d + 1, n - 1);
        free(d);
    }
    printf("replayed %d inputs\n", argc - 1);
    return 0;
}
#elif defined(YSP_FUZZ_SEEDS)
int main(int argc, char** argv) {
    int i, k = 0;
    if (argc < 2) { fprintf(stderr, "usage: seeds DIR FILE...\n"); return 2; }
    for (i = 2; i < argc; i++) {
        size_t n, L = strlen(argv[i]);
        uint8_t* d = load(argv[i], &n);
        int t, lo, hi;
        if (!d) { fprintf(stderr, "cannot read %s\n", argv[i]); return 1; }
        if (L > 5 && !strcmp(argv[i] + L - 5, ".xlsx")) { lo = 0; hi = 1; }
        else if (L > 4 && (!strcmp(argv[i] + L - 4, ".jpg") || !strcmp(argv[i] + L - 5, ".jpeg"))) { lo = hi = 2; }
        else { lo = hi = 3; }
        for (t = lo; t <= hi; t++) {
            char p[1024];
            FILE* f;
            d[0] = (uint8_t)t;
            snprintf(p, sizeof p, "%s/seed_%03d", argv[1], k++);
            f = fopen(p, "wb");
            if (!f || fwrite(d, 1, n, f) != n) { fprintf(stderr, "cannot write %s\n", p); return 1; }
            fclose(f);
        }
        free(d);
    }
    printf("%d seeds\n", k);
    return 0;
}
#elif defined(YSP_IMPORT_DUMP)
int main(int argc, char** argv) {
    size_t n, L;
    uint8_t* d;
    char err[512];
    (void)one;
    if (argc < 2) { fprintf(stderr, "usage: dump FILE.xlsx [SHEET] | FILE.jpg\n"); return 2; }
    d = load(argv[1], &n);
    if (!d) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    L = strlen(argv[1]);
    if (L > 5 && !strcmp(argv[1] + L - 5, ".xlsx")) {
        ypt_sheet s;
        if (ypt_xlsx_read(d + 1, n - 1, argc > 2 ? argv[2] : NULL, &s, err, sizeof err) < 0) { printf("refused: %s\n", err); return 1; }
        fwrite(s.csv, 1, s.csv_len, stdout);
        ypt_sheet_free(&s);
    } else {
        ypt_jpeg j;
        if (ypt_jpeg_read(d + 1, n - 1, 1, &j, err, sizeof err) < 0) { printf("refused: %s\n", err); return 1; }
        printf("%d x %d, %d components, %s, %s, icc %s\n", j.w, j.h, j.components, j.sampling, j.progressive ? "progressive" : "sequential",
               j.icc_srgb ? "sRGB" : "none");
        if (argc > 2) {
            FILE* f = fopen(argv[2], "wb");
            if (f) { fwrite(j.px, 1, (size_t)j.w * j.h * j.channels, f); fclose(f); }
        }
        ypt_jpeg_free(&j);
    }
    free(d);
    return 0;
}
#else
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    one(data, size);
    return 0;
}
#endif
