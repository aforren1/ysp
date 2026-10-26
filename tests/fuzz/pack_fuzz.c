/* pack_fuzz.c - fuzz target for ysp/pack.h. The first input byte picks the
 * target, the rest is its input:
 *   0  a pack in memory (8-byte aligned copy), VERIFY_USE: every lookup,
 *      every entry whole, through a cursor in odd pieces, then every view
 *      on every entry whatever its kind; ypak_verify_entry() before the
 *      data of every other entry, so it hashes first there
 *   1  the same pack opened incrementally: a tail of a length the second
 *      byte picks, then each range asked for, from the input when it is
 *      there; then ypak_check() on every entry
 *   2  the same pack through a range reader, VERIFY_OPEN, cursors with
 *      scratch
 *   3  a view on its own: the second byte picks cset, artwork, glyph
 *      runs, shader or texture (with QOI decode)
 * Every input must return without a sanitizer report. A refusal is a
 * correct answer to a bad input, so the target asserts nothing else.
 *
 * libFuzzer, MSVC 2022 x64 (from the repository root, in a vcvars shell):
 *     cl /nologo /O1 /Zi /fsanitize=address /fsanitize=fuzzer /Iinclude tests\fuzz\pack_fuzz.c
 *     pack_fuzz.exe -max_total_time=3600 -timeout=10 -rss_limit_mb=4096 corpus_pack
 * libFuzzer, clang:
 *     clang -O1 -g -fsanitize=fuzzer,address,undefined -Iinclude tests/fuzz/pack_fuzz.c -o pack_fuzz
 * Seeds (no corpus is committed): packs given as arguments, each as targets
 * 0, 1 and 2, and every entry of theirs as a view seed (target 3):
 *     cc -DYSP_FUZZ_SEEDS -Iinclude tests/fuzz/pack_fuzz.c -o seeds && ./seeds corpus_pack a.ysppak b.ysppak
 * Replay of a corpus without libFuzzer (gcc or clang under UBSan):
 *     gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
 *         -DYSP_FUZZ_REPLAY -Iinclude tests/fuzz/pack_fuzz.c -o replay
 *     ./replay corpus_pack/<files>
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_PACK_IMPLEMENTATION
#define YPAK_NO_FILE
#include "ysp/pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_copy[(8u << 20) / 8];
static uint8_t g_out[1 << 20];
static uint8_t g_scratch[1 << 20];
static volatile unsigned g_sink;

static void views(const void* p, int64_t n, int which) {
    char err[128];
    switch (which % 5) {
    case 0: {
        ypak_cset s;
        if (ypak_cset_view(p, n, &s, err, sizeof err) == 0) g_sink += s.n_glyphs + (unsigned)s.words[s.n_words - 1];
    } break;
    case 1: {
        ypak_art a;
        if (ypak_art_view(p, n, &a, err, sizeof err) == 0 && a.n_layers) g_sink += a.layers[a.n_layers - 1].rgba;
    } break;
    case 2: {
        ypak_runs r;
        if (ypak_runs_view(p, n, &r, err, sizeof err) == 0) {
            uint32_t i;
            for (i = 0; i < r.n_blocks; i++) {
                const ypak_runs_block_rec* b = &r.blocks[i];
                g_sink += (unsigned)strlen(ypak_runs_str(&r, b->text_off)) + (unsigned)ypak_runs_find(&r, ypak_runs_str(&r, b->key_off));
                if (b->n_items) g_sink += (unsigned)r.items[8 * (b->item_first + b->n_items - 1)] + r.clusters[b->item_first];
                if (b->n_lines) g_sink += r.lines[b->line_first + b->n_lines - 1].text_end;
            }
        }
    } break;
    case 3: {
        ypak_shader s;
        if (ypak_shader_view(p, n, &s, err, sizeof err) == 0) g_sink += (unsigned)strlen(s.body) + (unsigned)strlen(s.name);
    } break;
    default: {
        ypak_texture t;
        if (ypak_texture_view(p, n, &t, err, sizeof err) == 0) {
            if (t.compression == YPAK_TEX_QOI && t.raw_bytes <= sizeof g_out) g_sink += (unsigned)ypak_qoi_decode(&t, g_out, sizeof g_out, err, sizeof err);
            else if (t.stored) g_sink += t.data[t.stored - 1];
        }
    } break;
    }
}

static void sweep(ypak_pack* pk, int with_data, uint8_t* scratch, size_t cap) {
    uint32_t i;
    ypak_entry e;
    for (i = 0; i < pk->n; i++) {
        ypak_cursor c;
        if (ypak_at(pk, i, &e) != 0) continue;
        g_sink += (unsigned)ypak_find_n(pk, e.name, e.name_len, &e);
        if (i & 1) g_sink += (unsigned)ypak_verify_entry(pk, &e);
        if (with_data) {
            const void* d = ypak_data(pk, &e);
            if (d) {
                int w;
                for (w = 0; w < 5; w++) views(d, e.size, w);
            }
        }
        if (ypak_cursor_init(&c, pk, &e, scratch, cap) == 0) {
            int64_t off = 0, step = 777;
            while (off < e.size && off < (2 << 20)) {
                int64_t got = ypak_cursor_read(&c, off, g_out, step);
                if (got <= 0) break;
                off += got;
                step = step * 3 + 1;
                if (step > (int64_t)sizeof g_out) step = 1;
            }
        }
    }
    g_sink += (unsigned)ypak_find(pk, "ysp/manifest.json", &e) + (unsigned)ypak_find(pk, "nothing", &e) + (unsigned)ypak_first_of(pk, YPAK_KIND_EXPERIMENT, &e);
}

typedef struct mem { const uint8_t* p; int64_t n; } mem;
static int64_t mem_read(void* ctx, int64_t off, void* buf, int64_t n) {
    const mem* m = (const mem*)ctx;
    if (off < 0 || n < 0 || off > m->n || n > m->n - off) return -1;
    memcpy(buf, m->p + off, (size_t)n);
    return n;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ypak_pack pk;
    ypak_desc d;
    uint8_t* m = (uint8_t*)g_copy;
    if (size < 2 || size - 2 > sizeof g_copy) return 0;
    memcpy(m, data + 2, size - 2);
    size -= 2;
    memset(&d, 0, sizeof d);
    switch (data[0] % 4) {
    case 0:
        d.data = m;
        d.size = size;
        if (ypak_open(&pk, &d)) { sweep(&pk, 1, NULL, 0); ypak_close(&pk); }
        break;
    case 1: {
        size_t tail = (size_t)data[1] * 64 + 22;
        int rc, steps = 0;
        if (tail > size) tail = size;
        d.tail = m + size - tail;
        d.tail_size = tail;
        d.file_size = (int64_t)size;
        rc = !ypak_open(&pk, &d) ? pk.error_code : pk.need_len > 0 ? YPAK_NEED : 0;
        while (rc == YPAK_NEED && steps++ < 8) {
            if (pk.need_off < 0 || pk.need_len < 0 || pk.need_off + pk.need_len > (int64_t)size) { ypak_close(&pk); rc = -1; break; }
            rc = ypak_feed(&pk, m + pk.need_off, (size_t)pk.need_len);
        }
        if (rc == 0) {
            uint32_t i;
            for (i = 0; i < pk.n; i++) {
                ypak_entry e;
                ypak_at(&pk, i, &e);
                if (e.header_off >= 0 && e.data_off + e.size <= (int64_t)size)
                    g_sink += (unsigned)ypak_check(&pk, &e, m + e.header_off, (size_t)(e.data_off + e.size - e.header_off));
            }
            ypak_close(&pk);
        } else if (rc == YPAK_NEED) {
            ypak_close(&pk);
        }
    } break;
    case 2: {
        mem mm;
        ypak_reader rd;
        mm.p = m;
        mm.n = (int64_t)size;
        rd.read = mem_read;
        rd.size = (int64_t)size;
        d.reader = &rd;
        d.reader_ctx = &mm;
        d.verify = (data[1] & 1) ? YPAK_VERIFY_OPEN : YPAK_VERIFY_USE;
        if (ypak_open(&pk, &d)) { sweep(&pk, 0, g_scratch, sizeof g_scratch); ypak_close(&pk); }
    } break;
    default:
        views(m, (int64_t)size, data[1]);
        break;
    }
    return 0;
}

#if defined(YSP_FUZZ_SEEDS)
static void put(const char* dir, const char* name, uint8_t a, uint8_t b, const void* p, size_t n) {
    char path[1024];
    FILE* f;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fputc(a, f);
    fputc(b, f);
    if (n) fwrite(p, 1, n, f);
    fclose(f);
}

int main(int argc, char** argv) {
    int a, nseed = 0;
    if (argc < 3) { fprintf(stderr, "usage: seeds DIR PACK...\n"); return 1; }
    for (a = 2; a < argc; a++) {
        FILE* f = fopen(argv[a], "rb");
        size_t n;
        uint8_t* buf;
        char name[64];
        ypak_pack pk;
        ypak_desc d;
        if (!f) { perror(argv[a]); continue; }
        fseek(f, 0, SEEK_END);
        n = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = (uint8_t*)malloc(n + 8);
        if (!buf || fread(buf, 1, n, f) != n) { fclose(f); free(buf); continue; }
        fclose(f);
        snprintf(name, sizeof name, "pack%02d_mem", a); put(argv[1], name, 0, 0, buf, n); nseed++;
        snprintf(name, sizeof name, "pack%02d_inc", a); put(argv[1], name, 1, 4, buf, n); nseed++;
        snprintf(name, sizeof name, "pack%02d_rd", a); put(argv[1], name, 2, 1, buf, n); nseed++;
        memset(&d, 0, sizeof d);
        d.data = buf;
        d.size = n;
        if (ypak_open(&pk, &d)) {
            uint32_t i;
            for (i = 0; i < pk.n; i++) {
                ypak_entry e;
                const void* p;
                int w = -1;
                ypak_at(&pk, i, &e);
                switch (e.kind) {
                case YPAK_KIND_CURVESET: w = 0; break;
                case YPAK_KIND_ARTWORK: w = 1; break;
                case YPAK_KIND_GLYPHRUNS: w = 2; break;
                case YPAK_KIND_SHADER: w = 3; break;
                case YPAK_KIND_TEXTURE: w = 4; break;
                default: break;
                }
                p = ypak_data(&pk, &e);
                if (w >= 0 && p && e.size < (4 << 20)) {
                    snprintf(name, sizeof name, "view%02d_%03u", a, i);
                    put(argv[1], name, 3, (uint8_t)w, p, (size_t)e.size);
                    nseed++;
                }
            }
            ypak_close(&pk);
        }
        free(buf);
    }
    printf("pack_fuzz: wrote %d seeds to %s\n", nseed, argv[1]);
    return 0;
}
#elif defined(YSP_FUZZ_REPLAY)
int main(int argc, char** argv) {
    static uint8_t buf[9u << 20];
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
    printf("pack_fuzz: replayed %d inputs\n", done);
    return 0;
}
#endif
