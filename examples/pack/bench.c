/* pack_bench - the costs of docs/pack.md section 8, on packs ypak built:
 *
 *     pack_bench PACK [rounds]
 *
 * Per round, in a fresh open: ypak_open() of the mapped file (the walk, the
 * checks, the manifest's SHA-256, the chunk table), ypak_find() of every
 * name in a shuffled order, then ypak_verify_all() (every entry hashed at
 * mapped first touch), then one chunk through a cursor over a range reader
 * (fread, from the OS cache). Also the entry hash on memory already
 * touched, and the allocations made after open (must be 0). Medians, with
 * min and max, over the rounds. Run it under the measurement lock. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
/* fseeko and ftello under -std=c11, before the first system header (as
 * ysp/rt.h does) */
#if (defined(__linux__) || defined(__EMSCRIPTEN__)) && !defined(_DEFAULT_SOURCE) &&     !defined(_GNU_SOURCE) && !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long long g_mallocs;
static void* count_malloc(size_t n) { g_mallocs++; return malloc(n); }
#define YPAK_MALLOC(n) count_malloc(n)
#define YPAK_FREE(p) free(p)
#define YSP_PACK_IMPLEMENTATION
#include "ysp/pack.h"
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"

static int cmpd(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static void stat3(const char* what, double* v, int n, const char* unit) {
    qsort(v, (size_t)n, sizeof(double), cmpd);
    printf("  %-44s %10.3f (%.3f to %.3f) %s\n", what, v[n / 2], v[0], v[n - 1], unit);
}

typedef struct frd { FILE* f; } frd;
static int64_t file_read(void* ctx, int64_t off, void* buf, int64_t n) {
    FILE* f = ((frd*)ctx)->f;
#if defined(_WIN32)
    if (_fseeki64(f, off, SEEK_SET) != 0) return -1;
#else
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return -1;
#endif
    return (int64_t)fread(buf, 1, (size_t)n, f);
}

int main(int argc, char** argv) {
    int rounds = argc > 2 ? atoi(argv[2]) : 7, r;
    double t_open[64], t_find[64], t_ver[64], t_chunk[64], t_mem[64];
    double gbs_ver[64], gbs_mem[64];
    long long allocs_after = 0;
    uint32_t n = 0;
    int64_t total = 0;
    char line[512];
    if (argc < 2 || rounds < 1 || rounds > 64) {
        fprintf(stderr, "usage: pack_bench PACK [rounds 1..64]\n");
        return 2;
    }
    for (r = 0; r < rounds; r++) {
        ypak_pack p;
        ypak_desc d;
        uint64_t t0;
        uint32_t i, *order;
        long long before;
        memset(&d, 0, sizeof d);
        d.path = argv[1];
        t0 = yrt_now_ns();
        if (!ypak_open(&p, &d)) { fprintf(stderr, "%s\n", ypak_error(&p)); return 1; }
        t_open[r] = (double)(yrt_now_ns() - t0) / 1e6;
        before = g_mallocs;
        n = p.n;
        order = (uint32_t*)malloc(sizeof(uint32_t) * n);
        for (i = 0; i < n; i++) order[i] = i;
        for (i = n - 1; i > 0; i--) {
            uint32_t j = (uint32_t)(((uint64_t)rand() * 65536u + (uint64_t)rand()) % (i + 1)), t = order[i];
            order[i] = order[j]; order[j] = t;
        }
        {
            ypak_entry e = { 0 };   /* gcc -O2 cannot see ypak_at() fill it */
            char name[YPAK_MAX_NAME + 1];
            uint64_t t1;
            size_t* lens = (size_t*)malloc(sizeof(size_t) * n);
            char** names = (char**)malloc(sizeof(char*) * n);
            for (i = 0; i < n; i++) {
                ypak_at(&p, order[i], &e);
                names[i] = (char*)malloc(e.name_len + 1);
                memcpy(names[i], e.name, e.name_len);
                names[i][e.name_len] = 0;
                lens[i] = e.name_len;
            }
            before = g_mallocs;
            t1 = yrt_now_ns();
            for (i = 0; i < n; i++)
                if (ypak_find_n(&p, names[i], lens[i], &e) != 0) { fprintf(stderr, "find failed\n"); return 1; }
            t_find[r] = (double)(yrt_now_ns() - t1) / (double)n / 1e3;
            for (i = 0; i < n; i++) free(names[i]);
            free(names);
            free(lens);
            (void)name;
        }
        t0 = yrt_now_ns();
        if (ypak_verify_all(&p) != 0) { fprintf(stderr, "%s\n", ypak_error(&p)); return 1; }
        t_ver[r] = (double)(yrt_now_ns() - t0) / 1e6;
        total = (int64_t)p.hashed_bytes;
        gbs_ver[r] = (double)total / (t_ver[r] * 1e6);
        /* the largest entry again, its memory touched: the hash alone */
        {
            ypak_entry e, big;
            memset(&big, 0, sizeof big);
            for (i = 0; i < n; i++) { ypak_at(&p, i, &e); if (e.size > big.size) big = e; }
            t0 = yrt_now_ns();
            (void)ypak_entry_hash(big.data, big.size, NULL);
            t_mem[r] = (double)(yrt_now_ns() - t0) / 1e6;
            gbs_mem[r] = (double)big.size / (t_mem[r] * 1e6);
        }
        allocs_after += g_mallocs - before;
        ypak_describe(&p, line, sizeof line);
        ypak_close(&p);
        free(order);
        /* one chunk through a cursor over a range reader */
        {
            frd fr;
            ypak_reader rd;
            ypak_cursor c;
            ypak_entry e, big;
            static uint8_t scratch[1 << 20], out[1 << 20];
            fr.f = fopen(argv[1], "rb");
            rd.read = file_read;
#if defined(_WIN32)
            _fseeki64(fr.f, 0, SEEK_END);
            rd.size = _ftelli64(fr.f);
#else
            fseeko(fr.f, 0, SEEK_END);
            rd.size = (int64_t)ftello(fr.f);
#endif
            memset(&d, 0, sizeof d);
            d.reader = &rd;
            d.reader_ctx = &fr;
            if (!ypak_open(&p, &d)) { fprintf(stderr, "%s\n", ypak_error(&p)); return 1; }
            memset(&big, 0, sizeof big);
            for (i = 0; i < p.n; i++) { ypak_at(&p, i, &e); if (e.size > big.size) big = e; }
            ypak_cursor_init(&c, &p, &big, scratch, sizeof scratch);
            t0 = yrt_now_ns();
            (void)ypak_cursor_read(&c, big.size > YPAK_CHUNK ? YPAK_CHUNK : 0, out, 4096);
            t_chunk[r] = (double)(yrt_now_ns() - t0) / 1e6;
            ypak_close(&p);
            fclose(fr.f);
        }
    }
    printf("pack_bench: %s\n  %s\n  %u entries, %lld bytes hashed per verify, %d rounds, median (min to max)\n", argv[1], line, n,
           (long long)total, rounds);
    stat3("ypak_open, mapped, VERIFY_USE", t_open, rounds, "ms");
    stat3("ypak_find, shuffled names", t_find, rounds, "us");
    stat3("ypak_verify_all, mapped first touch", t_ver, rounds, "ms");
    stat3("  as throughput", gbs_ver, rounds, "GB/s");
    stat3("entry hash of the largest entry, touched", t_mem, rounds, "ms");
    stat3("  as throughput", gbs_mem, rounds, "GB/s");
    stat3("one 1 MiB chunk, range reader (fread), cursor", t_chunk, rounds, "ms");
    printf("  allocations after open: %lld\n", allocs_after);
    return 0;
}
