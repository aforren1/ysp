/* bench.c - the costs of reading and writing a rig profile and of
 * ysp/json.h on larger text (docs/json.md, docs/device.md). Run it under
 * the measurement lock on a quiet machine:
 *
 *   rigfile_bench [ROUNDS]          (default 21)
 *
 * Each round times a batch and reports the batch's mean per call; the
 * program prints the median, min and max over the rounds:
 *   yjs_parse    a typical profile (4 roles, every field) into a fixed arena
 *   yrig_parse   the same profile: the parse, every field check, the struct
 *                (no folder, so no file is read)
 *   yrig_write   its canonical bytes
 *   yrig_hash    the canonical bytes and their SHA-256 (with the read-back
 *                check)
 *   yjs_parse    a 1 MB manifest-like document, in MB/s
 *   yjs_write    its canonical text, in MB/s
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_RIGFILE_IMPLEMENTATION
#include "ysp/rigfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXR 101

static char g_profile[8192];
static size_t g_profile_n;
static yrig_profile g_p;
static uint64_t g_mem[(32u << 20) / 8];
static char g_out[4u << 20];
static volatile size_t g_sink;

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y);
}

static void report(const char* what, double* v, int n, const char* unit) {
    qsort(v, (size_t)n, sizeof v[0], cmp_d);
    printf("%-34s median %10.3f  min %10.3f  max %10.3f %s\n", what, v[n / 2], v[0], v[n - 1], unit);
}

static void make_profile(void) {
    static const char* const fam[4] = { "xid", "triggerbox", "line", "mmbts" };
    static const char* const name[4] = { "resp", "trig", "photo", "mm" };
    int k;
    yrig_role* r;
    yrig_init(&g_p, "booth-2");
    for (k = 0; k < 4; k++) {
        int f = 0, j;
        char key[64];
        for (j = 1; j <= YBOX_FAMILY_LAST; j++) if (strcmp(ybox_family_name(j), fam[k]) == 0) f = j;
        snprintf(key, sizeof key, "serial:0403:6001:FT%04dABC:", k);
        (void)yrig_bind(&g_p, name[k], f, key);
        r = yrig_find(&g_p, name[k]);
        r->baud = 115200;
        r->ftdi_latency_ns = 1000000;
        r->latched = f == YBOX_MMBTS;
        r->pulse_ns = 2000000;
        r->n_buttons = 2;
        strcpy(r->buttons[0].code, "1");
        strcpy(r->buttons[0].name, "left");
        strcpy(r->buttons[1].code, "2");
        strcpy(r->buttons[1].name, "right");
        r->latency.set = r->bounds.set = true;
        r->latency.n = r->bounds.n = 200;
        r->latency.p5_ns = 201000;
        r->latency.median_ns = 577300;
        r->latency.p95_ns = 983100;
        r->latency.max_ns = 2101700;
        r->bounds.lo_ns = -312000;
        r->bounds.hi_ns = 1104000;
        strcpy(r->latency.date, "2026-10-09T13:58:12Z");
        strcpy(r->bounds.date, "2026-10-09T13:58:12Z");
        memset(r->latency.sha256, 'c', 64);
        memset(r->bounds.sha256, 'b', 64);
    }
    g_p.display.set = true;
    g_p.display.onset_offset_ns = 12345678;
    strcpy(g_p.display.calibration, "display.yspcal");
    memset(g_p.display.calibration_sha256, 'a', 64);
    g_profile_n = yrig_write(&g_p, g_profile, sizeof g_profile);
}

/* A manifest-like document of about 1 MB: entries with hashes, sources
 * and derivations. */
static char* make_big(size_t* n) {
    size_t cap = 2u << 20, p = 0;
    char* t = (char*)malloc(cap);
    int k = 0;
    p += (size_t)snprintf(t + p, cap - p, "{\"format\": \"ysp-pack\", \"version\": 1, \"entries\": [");
    while (p < (1u << 20)) {
        p += (size_t)snprintf(t + p, cap - p,
                              "%s{\"name\": \"img/stim_%05d.ysptex\", \"kind\": \"texture\", \"size\": %d, \"sha256\": "
                              "\"%064d\", \"xxh64\": \"%016d\", \"crc32\": \"%08d\", \"sources\": [{\"path\": \"img/stim_%05d.png\", "
                              "\"sha256\": \"%064d\"}], \"derivation\": {\"op\": \"png\", \"by\": \"lodepng\", \"format\": \"rgba8\", "
                              "\"gamma\": \"2.2\", \"premultiplied\": false, \"levels\": [1, 2, 4, 8, 16]}}",
                              k ? ", " : "", k, 40000 + k, k, k, k, k, k);
        k++;
    }
    p += (size_t)snprintf(t + p, cap - p, "]}");
    *n = p;
    return t;
}

int main(int argc, char** argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 21, r, i;
    double v[MAXR];
    char err[256], hash[65];
    size_t bn;
    char* big;
    if (rounds < 3 || rounds > MAXR) { fprintf(stderr, "usage: rigfile_bench [ROUNDS 3..%d]\n", MAXR); return 2; }
    make_profile();
    big = make_big(&bn);
    printf("ysp_json %s, ysp_rig %s; profile %u bytes, 4 roles; big document %u bytes\n", yjs_version(), yrig_version(),
           (unsigned)g_profile_n, (unsigned)bn);
    (void)yrt_thread_elevate(NULL);

    for (r = 0; r < rounds; r++) {
        int64_t t0 = yrt_now_ns();
        for (i = 0; i < 2000; i++) {
            yjs_arena a;
            yjs_value* root;
            yjs_arena_init(&a, g_mem, 64 * 1024);
            if (yjs_parse(&a, g_profile, g_profile_n, NULL, &root, NULL) != YJS_OK) return 1;
            g_sink += root->n;
        }
        v[r] = (double)(yrt_now_ns() - t0) / 2000.0 / 1000.0;
    }
    report("yjs_parse, profile (fixed arena)", v, rounds, "us");

    for (r = 0; r < rounds; r++) {
        int64_t t0 = yrt_now_ns();
        for (i = 0; i < 1000; i++) {
            if (yrig_parse(&g_p, g_profile, g_profile_n, NULL, err, sizeof err) != YRIG_OK) { fprintf(stderr, "%s\n", err); return 1; }
            g_sink += (size_t)g_p.n_roles;
        }
        v[r] = (double)(yrt_now_ns() - t0) / 1000.0 / 1000.0;
    }
    report("yrig_parse, profile (all checks)", v, rounds, "us");

    for (r = 0; r < rounds; r++) {
        int64_t t0 = yrt_now_ns();
        for (i = 0; i < 1000; i++) g_sink += yrig_write(&g_p, g_out, sizeof g_out);
        v[r] = (double)(yrt_now_ns() - t0) / 1000.0 / 1000.0;
    }
    report("yrig_write, profile", v, rounds, "us");

    for (r = 0; r < rounds; r++) {
        int64_t t0 = yrt_now_ns();
        for (i = 0; i < 500; i++) g_sink += (size_t)yrig_hash(&g_p, hash);
        v[r] = (double)(yrt_now_ns() - t0) / 500.0 / 1000.0;
    }
    report("yrig_hash, profile", v, rounds, "us");

    for (r = 0; r < rounds; r++) {
        yjs_arena a;
        yjs_value* root;
        int64_t t0 = yrt_now_ns(), t1;
        yjs_arena_init(&a, g_mem, sizeof g_mem);
        if (yjs_parse(&a, big, bn, NULL, &root, NULL) != YJS_OK) return 1;
        t1 = yrt_now_ns();
        v[r] = (double)bn / ((double)(t1 - t0) / 1e9) / 1e6;
        g_sink += root->n;
    }
    report("yjs_parse, 1 MB document", v, rounds, "MB/s");

    {
        yjs_arena a;
        yjs_value* root;
        yjs_arena_init(&a, g_mem, sizeof g_mem);
        if (yjs_parse(&a, big, bn, NULL, &root, NULL) != YJS_OK) return 1;
        for (r = 0; r < rounds; r++) {
            int64_t t0 = yrt_now_ns(), t1;
            size_t n = yjs_write_mem(root, YJS_WRITE_PRETTY, g_out, sizeof g_out);
            t1 = yrt_now_ns();
            v[r] = (double)n / ((double)(t1 - t0) / 1e9) / 1e6;
            g_sink += n;
        }
        report("yjs_write, 1 MB document (pretty)", v, rounds, "MB/s");
        printf("arena for the 1 MB document: %u bytes\n", (unsigned)a.used);
    }
    free(big);
    return 0;
}
