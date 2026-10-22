/* pack_test.c - self-checking test of ysp/pack.h. Exit 0 on pass.
 *
 * The packs here come from a small writer in this file, written from
 * docs/pack.md and independent of the pack tool's, so the two writers and
 * the reader check each other. The writer has faults to switch on: each
 * makes one rule of the format false while every hash stays right, so the
 * reader's structural checks are what refuses it. Then every single-bit
 * flip and every truncation of a small pack, the four sources (memory, a
 * range reader, incremental, a file), cursors, the arena, a 4.3 GB entry
 * and 65,536 entries through a virtual file (zip64 by size, by offset and
 * by count), and the views of the pack's own forms.
 *
 * YPAK_TEST_QUICK=1 skips the bit flips and the large virtual packs. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_PACK_IMPLEMENTATION
#include "ysp/pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* --- a virtual file: segments of bytes, zeros, or a pattern ------------------ */

enum { SEG_BYTES, SEG_ZEROS, SEG_PATTERN };
typedef struct seg { int64_t off, len; size_t heap; int type; int64_t base; } seg;
typedef struct vfile {
    seg*     s;   int n, cap;
    uint8_t* heap; size_t heap_n, heap_cap;
    int64_t  size;
} vfile;

static uint8_t pattern_at(int64_t i) {
    uint32_t x = (uint32_t)(i & (YPAK_CHUNK - 1)) * 2654435761u;
    return (uint8_t)(x >> 24);
}

static size_t heap_put(vfile* f, const void* p, size_t n) {
    size_t at;
    if (f->heap_n + n > f->heap_cap) {
        size_t c = f->heap_cap ? f->heap_cap : 65536;
        while (c < f->heap_n + n) c *= 2;
        f->heap = (uint8_t*)realloc(f->heap, c);
        f->heap_cap = c;
    }
    at = f->heap_n;
    if (n) memcpy(f->heap + at, p, n);
    f->heap_n += n;
    return at;
}

static void seg_add(vfile* f, int type, const void* p, int64_t len, int64_t base) {
    seg* s;
    if (len <= 0) return;
    if (f->n == f->cap) {
        f->cap = f->cap ? 2 * f->cap : 256;
        f->s = (seg*)realloc(f->s, (size_t)f->cap * sizeof(seg));
    }
    s = &f->s[f->n++];
    s->off = f->size;
    s->len = len;
    s->type = type;
    s->base = base;
    s->heap = type == SEG_BYTES ? heap_put(f, p, (size_t)len) : 0;
    f->size += len;
}

static void vf_free(vfile* f) {
    free(f->s);
    free(f->heap);
    memset(f, 0, sizeof(*f));
}

static int64_t vf_read(void* ctx, int64_t off, void* buf, int64_t n) {
    vfile* f = (vfile*)ctx;
    uint8_t* o = (uint8_t*)buf;
    int64_t done = 0;
    int lo = 0, hi = f->n;
    if (off < 0 || n < 0 || off + n > f->size) return -1;
    while (lo + 1 < hi) {
        int mid = (lo + hi) / 2;
        if (f->s[mid].off <= off) lo = mid;
        else hi = mid;
    }
    while (done < n && lo < f->n) {
        const seg* s = &f->s[lo];
        int64_t at = off + done - s->off, k = s->len - at;
        if (k > n - done) k = n - done;
        if (s->type == SEG_BYTES) memcpy(o + done, f->heap + s->heap + at, (size_t)k);
        else if (s->type == SEG_ZEROS) memset(o + done, 0, (size_t)k);
        else {
            int64_t j;
            for (j = 0; j < k; j++) o[done + j] = pattern_at(s->base + at + j);
        }
        done += k;
        lo++;
    }
    return done;
}

/* The whole file in 8-byte aligned memory (small packs only). */
static uint8_t* vf_flat(vfile* f) {
    uint8_t* m = (uint8_t*)malloc((size_t)f->size + 16);
    vf_read(f, 0, m, f->size);
    return m;
}

/* --- the writer ------------------------------------------------------------- */

typedef struct tin { const char* name; uint32_t kind, flags; const uint8_t* data; int64_t size; int pattern; } tin;

enum {
    F_NONE, F_UNSORTED, F_GAP, F_DATAOFF, F_NO_EXT, F_EXTRA_EXT, F_METHOD, F_ENCRYPT, F_DESCRIPTOR,
    F_FORMAT2, F_KIND, F_NO_CHUNKS, F_CHUNK_INDEX, F_COMMENT, F_CDHASH, F_MANIFEST_SHA, F_LOCAL_NAME,
    F_PADDING, F_ATTR, F_TIME, F_ENTRY_HASH, F_CHUNK_TABLE, F_VERSION, F_TRAILING, F_LOCAL_CRC,
    F_DATA, F_SIZES, F_EXT_VERSION, F_FLAGS_UNKNOWN, F_MANIFEST_KIND, F_COUNT
};

static const char* const fault_names[F_COUNT] = {
    "none", "unsorted", "gap", "data offset", "no private record", "extra field", "deflate method", "encrypted",
    "data descriptor", "format 2", "unknown kind", "no chunks entry", "chunk index", "comment", "directory hash",
    "manifest sha", "local name", "padding", "attributes", "time", "entry hash", "chunk table", "version needed",
    "trailing bytes", "local crc", "data byte", "sizes", "record version", "unknown flags", "manifest kind"
};

static void w16(uint8_t* b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t* b, uint32_t v) { w16(b, v & 0xFFFF); w16(b + 2, v >> 16); }
static void w64(uint8_t* b, uint64_t v) { w32(b, (uint32_t)v); w32(b + 4, (uint32_t)(v >> 32)); }

typedef struct went {
    const char* name; size_t nl;
    uint32_t kind, flags;
    const uint8_t* data; int64_t size; int pattern;
    uint64_t hash; uint32_t crc, chunk;
    int64_t hrel, drel;
    uint64_t* ch; int64_t kc;
} went;

static int went_cmp(const void* a, const void* b) {
    const went* x = (const went*)a;
    const went* y = (const went*)b;
    int c = memcmp(x->name, y->name, x->nl < y->nl ? x->nl : y->nl);
    if (c) return c;
    return x->nl < y->nl ? -1 : x->nl > y->nl;
}

static void pattern_hashes(went* e) {
    int64_t j;
    uint8_t* b = (uint8_t*)malloc((size_t)YPAK_CHUNK);
    uint64_t full = 0;
    ypak_xxh64_state s;
    uint8_t le[8];
    for (j = 0; j < YPAK_CHUNK; j++) b[j] = pattern_at(j);
    e->kc = ypak_chunk_count(e->size);
    e->ch = (uint64_t*)malloc((size_t)e->kc * 8);
    ypak_xxh64_init(&s, (uint64_t)e->size);
    for (j = 0; j < e->kc; j++) {
        int64_t n = e->size - (j << YPAK_CHUNK_LOG2);
        if (n >= YPAK_CHUNK) {
            if (!full) full = ypak_xxh64(b, (size_t)YPAK_CHUNK, 0);
            e->ch[j] = full;
        } else {
            e->ch[j] = ypak_xxh64(b, (size_t)n, 0);
        }
        w64(le, e->ch[j]);
        ypak_xxh64_update(&s, le, 8);
    }
    e->hash = ypak_xxh64_digest(&s);
    e->crc = 0;   /* the reader does not check CRC-32; not computed for 4 GB */
    free(b);
}

/* Builds a pack into f. prefix: bytes of an "executable" before the pack;
 * absolute: offsets from the file start (an appended copy) instead of from
 * the zip start. */
static void build(vfile* f, const tin* in, int n_in, int fault, int64_t prefix, int absolute) {
    static const char manifest[] = "{\n  \"format\": \"ysp-pack\",\n  \"version\": 1\n}\n";
    went* e = (went*)calloc((size_t)n_in + 2, sizeof(went));
    int n = 0, i;
    uint8_t* chunks;
    int64_t n_hashes = 0, at, cd_start, cd_size, B;
    uint8_t* cd;
    size_t cdn = 0, cdcap = 4096;
    uint8_t sha[32];
    memset(f, 0, sizeof(*f));
    e[n].name = YPAK_MANIFEST; e[n].kind = YPAK_KIND_MANIFEST; e[n].data = (const uint8_t*)manifest;
    e[n].size = (int64_t)sizeof(manifest) - 1;
    n++;
    for (i = 0; i < n_in; i++, n++) {
        e[n].name = in[i].name; e[n].kind = in[i].kind; e[n].flags = in[i].flags;
        e[n].data = in[i].data; e[n].size = in[i].size; e[n].pattern = in[i].pattern;
    }
    for (i = 0; i < n; i++) e[i].nl = strlen(e[i].name);
    /* hashes and the chunk table */
    for (i = 0; i < n; i++) {
        if (e[i].pattern) {
            pattern_hashes(&e[i]);
        } else {
            e[i].kc = ypak_chunk_count(e[i].size);
            e[i].ch = (uint64_t*)malloc((size_t)e[i].kc * 8);
            e[i].hash = ypak_entry_hash(e[i].data, e[i].size, e[i].ch);
            e[i].crc = ypak_crc32(0, e[i].data, (size_t)e[i].size);
        }
    }
    qsort(e + 1, (size_t)(n - 1), sizeof(went), went_cmp);
    for (i = 1; i < n; i++)
        if (e[i].kc > 1) n_hashes += e[i].kc;
    chunks = (uint8_t*)calloc(1, (size_t)(32 + 8 * n_hashes));
    memcpy(chunks, "YSPCHNK1", 8);
    w32(chunks + 8, 1);
    w32(chunks + 12, YPAK_CHUNK_LOG2);
    w64(chunks + 16, (uint64_t)n_hashes);
    {
        int64_t k = 0;
        for (i = 1; i < n; i++) {
            if (e[i].kc > 1) {
                int64_t j;
                e[i].chunk = (uint32_t)k;
                for (j = 0; j < e[i].kc; j++) w64(chunks + 32 + 8 * (k + j), e[i].ch[j]);
                k += e[i].kc;
            } else {
                e[i].chunk = YPAK_NO_CHUNK;
            }
        }
    }
    e[0].chunk = YPAK_NO_CHUNK;
    if (fault == F_CHUNK_TABLE && n_hashes) chunks[32] ^= 1;
    if (fault != F_NO_CHUNKS) {
        went c;
        memset(&c, 0, sizeof(c));
        c.name = YPAK_CHUNKS; c.nl = strlen(YPAK_CHUNKS); c.kind = YPAK_KIND_CHUNKS;
        c.data = chunks; c.size = 32 + 8 * n_hashes; c.chunk = YPAK_NO_CHUNK;
        c.kc = ypak_chunk_count(c.size);
        c.ch = (uint64_t*)malloc((size_t)c.kc * 8);
        c.hash = ypak_entry_hash(c.data, c.size, c.ch);
        c.crc = ypak_crc32(0, c.data, (size_t)c.size);
        e[n++] = c;
        qsort(e + 1, (size_t)(n - 1), sizeof(went), went_cmp);
    }
    if (fault == F_UNSORTED && n >= 3) { went t = e[1]; e[1] = e[2]; e[2] = t; }
    /* layout */
    if (prefix) seg_add(f, SEG_ZEROS, NULL, prefix, 0);
    at = 0;
    for (i = 0; i < n; i++) {
        uint8_t h[64 + YPAK_MAX_NAME];
        int z = e[i].size >= 0xFFFFFFFFLL;
        int64_t pad;
        size_t hn = 0;
        if (fault == F_GAP && i == 2) { seg_add(f, SEG_ZEROS, NULL, 16, 0); at += 16; }
        e[i].hrel = at;
        e[i].drel = (at + 30 + (int64_t)e[i].nl + (z ? 20 : 0) + 6 + YPAK_ALIGN - 1) & ~(int64_t)(YPAK_ALIGN - 1);
        pad = e[i].drel - (at + 30 + (int64_t)e[i].nl + (z ? 20 : 0) + 6);
        w32(h, 0x04034B50u);
        w16(h + 4, z ? 45 : 20);
        w16(h + 6, 0x0800u | (fault == F_ENCRYPT && i == 1 ? 1u : 0u) | (fault == F_DESCRIPTOR && i == 1 ? 8u : 0u));
        w16(h + 8, fault == F_METHOD && i == 1 ? 8 : 0);
        w16(h + 10, 0);
        w16(h + 12, 0x21);
        w32(h + 14, e[i].crc ^ (fault == F_LOCAL_CRC && i == 1 ? 1u : 0u));
        w32(h + 18, z ? 0xFFFFFFFFu : (uint32_t)e[i].size);
        w32(h + 22, z ? 0xFFFFFFFFu : (uint32_t)e[i].size);
        w16(h + 26, (uint32_t)e[i].nl);
        w16(h + 28, (uint32_t)((z ? 20 : 0) + 6 + pad));
        memcpy(h + 30, e[i].name, e[i].nl);
        if (fault == F_LOCAL_NAME && i == 1) h[30] ^= 0x20;
        hn = 30 + e[i].nl;
        if (z) {
            w16(h + hn, 1); w16(h + hn + 2, 16); w64(h + hn + 4, (uint64_t)e[i].size); w64(h + hn + 12, (uint64_t)e[i].size);
            hn += 20;
        }
        w16(h + hn, 0xD935u); w16(h + hn + 2, (uint32_t)(2 + pad)); w16(h + hn + 4, YPAK_ALIGN);
        hn += 6;
        seg_add(f, SEG_BYTES, h, (int64_t)hn, 0);
        if (fault == F_PADDING && i == 1 && pad > 0) {
            uint8_t one = 1;
            seg_add(f, SEG_BYTES, &one, 1, 0);
            seg_add(f, SEG_ZEROS, NULL, pad - 1, 0);
        } else {
            seg_add(f, SEG_ZEROS, NULL, pad, 0);
        }
        if (e[i].pattern) {
            seg_add(f, SEG_PATTERN, NULL, e[i].size, 0);
        } else if (fault == F_DATA && i == 1 && e[i].size > 0) {
            uint8_t* d = (uint8_t*)malloc((size_t)e[i].size);
            memcpy(d, e[i].data, (size_t)e[i].size);
            d[e[i].size / 2] ^= 0x10;
            seg_add(f, SEG_BYTES, d, e[i].size, 0);
            free(d);
        } else {
            seg_add(f, SEG_BYTES, e[i].data, e[i].size, 0);
        }
        at = e[i].drel + e[i].size;
    }
    /* central directory */
    B = absolute ? 0 : prefix;
    cd_start = at;
    cd = (uint8_t*)malloc(cdcap);
    for (i = 0; i < n; i++) {
        uint8_t r[46 + YPAK_MAX_NAME + 64];
        int64_t lho = e[i].hrel + prefix - B;
        int zs = e[i].size >= 0xFFFFFFFFLL, zo = lho >= 0xFFFFFFFFLL, z = zs || zo;
        size_t rn = 46 + e[i].nl, el = 0;
        uint32_t kind = e[i].kind;
        if (fault == F_KIND && i == 1) kind = 99;
        if (fault == F_MANIFEST_KIND && i == 0) kind = YPAK_KIND_FILE;
        w32(r, 0x02014B50u);
        w16(r + 4, z ? 0x032Du : 0x0314u);
        w16(r + 6, (fault == F_VERSION && i == 1) ? 10 : z ? 45 : 20);
        w16(r + 8, 0x0800u | (fault == F_ENCRYPT && i == 1 ? 1u : 0u) | (fault == F_DESCRIPTOR && i == 1 ? 8u : 0u));
        w16(r + 10, fault == F_METHOD && i == 1 ? 8 : 0);
        w16(r + 12, fault == F_TIME && i == 1 ? 1 : 0);
        w16(r + 14, 0x21);
        w32(r + 16, e[i].crc);
        w32(r + 20, zs ? 0xFFFFFFFFu : (uint32_t)e[i].size + (fault == F_SIZES && i == 1 ? 1u : 0u));
        w32(r + 24, zs ? 0xFFFFFFFFu : (uint32_t)e[i].size);
        w16(r + 28, (uint32_t)e[i].nl);
        w16(r + 32, 0); w16(r + 34, 0); w16(r + 36, 0);
        w32(r + 38, fault == F_ATTR && i == 1 ? 0u : 0100644u << 16);
        w32(r + 42, zo ? 0xFFFFFFFFu : (uint32_t)lho);
        memcpy(r + 46, e[i].name, e[i].nl);
        if (z) {
            uint32_t want = (zs ? 16u : 0u) + (zo ? 8u : 0u);
            w16(r + rn, 1); w16(r + rn + 2, want);
            rn += 4;
            if (zs) { w64(r + rn, (uint64_t)e[i].size); w64(r + rn + 8, (uint64_t)e[i].size); rn += 16; }
            if (zo) { w64(r + rn, (uint64_t)lho); rn += 8; }
        }
        if (!(fault == F_NO_EXT && i == 1)) {
            w16(r + rn, YPAK_EXT_ID); w16(r + rn + 2, YPAK_EXT_SIZE);
            r[rn + 4] = fault == F_EXT_VERSION && i == 1 ? 2 : 1;
            r[rn + 5] = 0;
            w16(r + rn + 6, kind);
            w32(r + rn + 8, e[i].flags | (fault == F_FLAGS_UNKNOWN && i == 1 ? 0x80u : 0u));
            w64(r + rn + 12, (uint64_t)e[i].drel + (fault == F_DATAOFF && i == 1 ? 4096u : 0u));
            w64(r + rn + 20, e[i].hash ^ (fault == F_ENTRY_HASH && i == 1 ? 1u : 0u));
            w32(r + rn + 28, e[i].chunk + (fault == F_CHUNK_INDEX && e[i].chunk != YPAK_NO_CHUNK ? 1u : 0u));
            rn += 32;
        }
        if (fault == F_EXTRA_EXT && i == 1) {
            w16(r + rn, 0x5455u); w16(r + rn + 2, 5); r[rn + 4] = 1; w32(r + rn + 5, 0);
            rn += 9;
        }
        el = rn - 46 - e[i].nl;
        w16(r + 30, (uint32_t)el);
        while (cdn + rn > cdcap) { cdcap *= 2; cd = (uint8_t*)realloc(cd, cdcap); }
        memcpy(cd + cdn, r, rn);
        cdn += rn;
    }
    seg_add(f, SEG_BYTES, cd, (int64_t)cdn, 0);
    cd_size = (int64_t)cdn;
    {
        uint8_t t[200];
        char cm[160];
        int64_t cd_off = cd_start + prefix - B;
        int need64 = n >= 0xFFFF || cd_off >= 0xFFFFFFFFLL;
        size_t k;
        uint64_t cdh = ypak_xxh64(cd, cdn, 0);
        if (need64) {
            int64_t rec = cd_start + cd_size + prefix - B;
            w32(t, 0x06064B50u); w64(t + 4, 44); w16(t + 12, 0x032D); w16(t + 14, 45); w32(t + 16, 0); w32(t + 20, 0);
            w64(t + 24, (uint64_t)n); w64(t + 32, (uint64_t)n); w64(t + 40, (uint64_t)cd_size); w64(t + 48, (uint64_t)cd_off);
            w32(t + 56, 0x07064B50u); w32(t + 60, 0); w64(t + 64, (uint64_t)rec); w32(t + 72, 1);
            seg_add(f, SEG_BYTES, t, 76, 0);
        }
        ypak_sha256(manifest, sizeof(manifest) - 1, sha);
        if (fault == F_MANIFEST_SHA) sha[5] ^= 1;
        if (fault == F_CDHASH) cdh ^= 1;
        snprintf(cm, sizeof(cm), "ysp-pack/%d manifest=sha256:", fault == F_FORMAT2 ? 2 : 1);
        k = strlen(cm);
        for (i = 0; i < 32; i++) k += (size_t)snprintf(cm + k, sizeof(cm) - k, "%02x", sha[i]);
        k += (size_t)snprintf(cm + k, sizeof(cm) - k, " cd=xxh64:%016llx", (unsigned long long)cdh);
        if (fault == F_COMMENT) cm[k - 20] = 'X';
        w32(t, 0x06054B50u); w16(t + 4, 0); w16(t + 6, 0);
        w16(t + 8, n >= 0xFFFF ? 0xFFFFu : (uint32_t)n); w16(t + 10, n >= 0xFFFF ? 0xFFFFu : (uint32_t)n);
        w32(t + 12, (uint32_t)cd_size);
        w32(t + 16, cd_off >= 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)cd_off);
        w16(t + 20, (uint32_t)k);
        memcpy(t + 22, cm, k);
        seg_add(f, SEG_BYTES, t, (int64_t)(22 + k), 0);
        if (fault == F_TRAILING) seg_add(f, SEG_ZEROS, NULL, 3, 0);
    }
    for (i = 0; i < n; i++) free(e[i].ch);
    free(cd);
    free(chunks);
    free(e);
}

/* --- the tests ------------------------------------------------------------- */

static uint8_t g_small[3000], g_mid[5000], g_big[(5 << 19) + 77];   /* 2.5 MiB + 77: three chunks */

static void fill(void) {
    size_t i;
    uint32_t x = 12345;
    for (i = 0; i < sizeof(g_small); i++) { x = x * 1103515245u + 12345u; g_small[i] = (uint8_t)(x >> 16); }
    for (i = 0; i < sizeof(g_mid); i++) g_mid[i] = (uint8_t)(i * 7);
    for (i = 0; i < sizeof(g_big); i++) { x = x * 1103515245u + 12345u; g_big[i] = (uint8_t)(x >> 16); }
}

static const tin g_basic[] = {
    { "audio/tone.wav", YPAK_KIND_AUDIO, YPAK_F_STREAM, g_small, sizeof(g_small), 0 },
    { "conditions.pstb", YPAK_KIND_TABLE, 0, g_mid, sizeof(g_mid), 0 },
    { "empty.bin", YPAK_KIND_FILE, 0, NULL, 0, 0 },
    { "experiment/main.json", YPAK_KIND_EXPERIMENT, 0, (const uint8_t*)"{}", 2, 0 },
    { "Gesichter/M\xC3\xBCller.bin", YPAK_KIND_FILE, 0, g_small, 1000, 0 },
    { "video/film.mpg", YPAK_KIND_VIDEO, YPAK_F_STREAM, g_big, sizeof(g_big), 0 },
};
#define N_BASIC ((int)(sizeof(g_basic) / sizeof(g_basic[0])))

/* v0.2.0: ypak_open() is bool and the code is pk.error_code; these tests
 * kept their int checks through this: 0 open, YPAK_NEED when an incremental
 * open wants bytes, else the failure's code. */
static int open_rc(ypak_pack* pk, const ypak_desc* d) {
    if (!ypak_open(pk, d)) return pk ? pk->error_code : YPAK_ERR_ARG;
    return pk->need_len > 0 ? YPAK_NEED : 0;
}

static int open_mem(ypak_pack* pk, const uint8_t* m, int64_t n, ypak_verify v) {
    ypak_desc d;
    memset(&d, 0, sizeof(d));
    d.data = m;
    d.size = (size_t)n;
    d.verify = v;
    return open_rc(pk, &d);
}

/* Every entry of g_basic is in pk with its bytes. */
static void check_contents(ypak_pack* pk, const char* how) {
    int i;
    for (i = 0; i < N_BASIC; i++) {
        ypak_entry e;
        int rc = ypak_find(pk, g_basic[i].name, &e);
        CHECK(rc == 0, "%s: find '%s': %s", how, g_basic[i].name, ypak_error(pk));
        if (rc) continue;
        CHECK(e.kind == g_basic[i].kind && e.size == g_basic[i].size && e.flags == g_basic[i].flags, "%s: '%s' fields", how, g_basic[i].name);
        CHECK(e.data_off % YPAK_ALIGN == pk->zip_start % YPAK_ALIGN, "%s: '%s' alignment", how, g_basic[i].name);
        if (e.data) {
            const void* d = ypak_data(pk, &e);
            CHECK(d && (e.size == 0 || memcmp(d, g_basic[i].data, (size_t)e.size) == 0), "%s: '%s' data: %s", how, g_basic[i].name, ypak_error(pk));
            CHECK(((uintptr_t)d & 7u) == 0, "%s: '%s' pointer alignment", how, g_basic[i].name);
        }
    }
}

static void test_hashes(void) {
    static const char* const v[3] = { "", "abc", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" };
    static const char* const want[3] = {
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" };
    int i, k;
    char hex[65];
    uint8_t d[32];
    uint8_t* mil = (uint8_t*)malloc(1000000);
    ypak_sha256_state s;
    for (i = 0; i < 3; i++) {
        ypak_sha256(v[i], strlen(v[i]), d);
        for (k = 0; k < 32; k++) snprintf(hex + 2 * k, 3, "%02x", d[k]);
        CHECK(strcmp(hex, want[i]) == 0, "SHA-256 vector %d: %s", i, hex);
    }
    memset(mil, 'a', 1000000);
    ypak_sha256_init(&s);
    for (i = 0; i < 1000; i++) ypak_sha256_update(&s, mil + i * 1000, 1000);
    ypak_sha256_final(&s, d);
    for (k = 0; k < 32; k++) snprintf(hex + 2 * k, 3, "%02x", d[k]);
    CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0, "SHA-256 of a million a: %s", hex);
    /* XXH64 reference values (xxHash 0.8.3's XXH64) */
    CHECK(ypak_xxh64("", 0, 0) == 0xEF46DB3751D8E999ULL, "XXH64 empty");
    CHECK(ypak_xxh64("a", 1, 0) == 0xD24EC4F1A98C6E5BULL, "XXH64 a");
    CHECK(ypak_xxh64("abc", 3, 0) == 0x44BC2CF5AD770999ULL, "XXH64 abc");
    CHECK(ypak_xxh64("Nobody inspects the spammish repetition", 39, 0) == 0xFBCEA83C8A378BF1ULL, "XXH64 sentence");
    /* streaming equals one shot at every split */
    for (i = 0; i < 300; i += 7) {
        ypak_xxh64_state x;
        ypak_xxh64_init(&x, 99);
        ypak_xxh64_update(&x, g_small, (size_t)i);
        ypak_xxh64_update(&x, g_small + i, 1000 - (size_t)i);
        CHECK(ypak_xxh64_digest(&x) == ypak_xxh64(g_small, 1000, 99), "XXH64 streaming split %d", i);
    }
    CHECK(ypak_crc32(0, "123456789", 9) == 0xCBF43926u, "CRC-32 check value");
    CHECK(ypak_crc32(ypak_crc32(0, g_small, 1234), g_small + 1234, 1766) == ypak_crc32(0, g_small, 3000), "CRC-32 in pieces");
    /* the entry hash: chunks then the list */
    {
        uint64_t ch[3], h = ypak_entry_hash(g_big, sizeof(g_big), ch);
        uint8_t le[24];
        for (i = 0; i < 3; i++) w64(le + 8 * i, ch[i]);
        CHECK(ch[0] == ypak_xxh64(g_big, (size_t)YPAK_CHUNK, 0) && ch[2] == ypak_xxh64(g_big + 2 * YPAK_CHUNK, 77 + (1 << 19), 0), "chunk hashes");
        CHECK(h == ypak_xxh64(le, 24, sizeof(g_big)), "entry hash over the list");
        CHECK(ypak_chunk_count(0) == 1 && ypak_chunk_count(YPAK_CHUNK) == 1 && ypak_chunk_count(YPAK_CHUNK + 1) == 2, "chunk count");
    }
    free(mil);
}

static void test_names(void) {
    static const char* const good[] = { "a", "a/b", "fonts/Noto Sans.ttf", "x.y.z", "M\xC3\xBCller", "\xE4\xB8\xAD", "\xF0\x9F\x98\x80", "a..b", ".hidden" };
    static const char* const bad[] = { "/a", "a/", "a//b", "./a", "a/./b", "a/..", "../a", "..", ".", "a\\b", "a\tb",
                                       "\xC3", "\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xC2\x85", "a\x7F" };
    size_t i;
    char longname[1100];
    for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) CHECK(ypak_name_ok(good[i], strlen(good[i])), "good name '%s'", good[i]);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(!ypak_name_ok(bad[i], strlen(bad[i])), "bad name %u", (unsigned)i);
    memset(longname, 'a', sizeof(longname));
    CHECK(ypak_name_ok(longname, YPAK_MAX_NAME) && !ypak_name_ok(longname, YPAK_MAX_NAME + 1), "name length limit");
    CHECK(!ypak_name_ok("", 0) && !ypak_name_ok("a\0b", 3), "empty name, NUL");
}

static void test_basic(void) {
    vfile f;
    uint8_t* m;
    ypak_pack pk;
    ypak_entry e;
    int rc;
    char line[256], id[72];
    build(&f, g_basic, N_BASIC, F_NONE, 0, 0);
    m = vf_flat(&f);
    rc = open_mem(&pk, m, f.size, YPAK_VERIFY_USE);
    CHECK(rc == 0, "open: %s", ypak_error(&pk));
    if (rc == 0) {
        CHECK(pk.n == (uint32_t)N_BASIC + 2, "entry count %u", pk.n);
        CHECK(pk.zip_start == 0 && pk.format == 1 && pk.n_chunks == 3, "zip start, format, chunks");
        check_contents(&pk, "memory");
        CHECK(ypak_find(&pk, "ysp/manifest.json", &e) == 0 && e.index == 0 && e.kind == YPAK_KIND_MANIFEST, "manifest is entry 0");
        CHECK(ypak_first_of(&pk, YPAK_KIND_EXPERIMENT, &e) == 0 && pk.experiment == (int32_t)e.index, "experiment");
        CHECK(ypak_find(&pk, "AUDIO/TONE.WAV", &e) == YPAK_ERR_NOT_FOUND && strstr(ypak_error(&pk), "audio/tone.wav"), "case hint: %s", ypak_error(&pk));
        CHECK(ypak_find(&pk, "nothing", &e) == YPAK_ERR_NOT_FOUND, "not found");
        CHECK(ypak_find(&pk, "audio/tone.wa", &e) == YPAK_ERR_NOT_FOUND && ypak_find(&pk, "audio/tone.wavx", &e) == YPAK_ERR_NOT_FOUND, "prefix names");
        ypak_describe(&pk, line, sizeof(line));
        ypak_id(&pk, id);
        CHECK(strstr(line, id) && strstr(line, "verified on use"), "describe: %s", line);
        {
            uint8_t sha[32];
            static const char manifest[] = "{\n  \"format\": \"ysp-pack\",\n  \"version\": 1\n}\n";
            ypak_sha256(manifest, sizeof(manifest) - 1, sha);
            CHECK(memcmp(sha, pk.id, 32) == 0, "pack ID is the manifest's SHA-256");
        }
        /* cursors from memory: every split of the 3-chunk entry */
        {
            ypak_cursor c;
            uint8_t* out = (uint8_t*)malloc(sizeof(g_big));
            int64_t off = 0, step = 100003;
            CHECK(ypak_find(&pk, "video/film.mpg", &e) == 0 && e.chunk_first == 0, "film entry");
            CHECK(ypak_cursor_init(&c, &pk, &e, NULL, 0) == 0, "cursor: %s", ypak_error(&pk));
            while (off < e.size) {
                int64_t got = ypak_cursor_read(&c, off, out + off, step);
                if (got <= 0) break;
                off += got;
            }
            CHECK(off == e.size && memcmp(out, g_big, sizeof(g_big)) == 0, "cursor read the film");
            CHECK(ypak_cursor_read(&c, e.size, out, 10) == 0, "read at the end gives 0");
            free(out);
        }
        ypak_close(&pk);
    }
    /* every policy */
    rc = open_mem(&pk, m, f.size, YPAK_VERIFY_OPEN);
    CHECK(rc == 0 && pk.hashed_bytes >= sizeof(g_big), "VERIFY_OPEN: %s", ypak_error(&pk));
    if (rc == 0) { check_contents(&pk, "verify open"); ypak_close(&pk); }
    rc = open_mem(&pk, m, f.size, YPAK_VERIFY_NONE);
    CHECK(rc == 0, "VERIFY_NONE: %s", ypak_error(&pk));
    if (rc == 0) {
        ypak_describe(&pk, line, sizeof(line));
        CHECK(strstr(line, "NOT VERIFIED") != NULL, "describe says not verified");
        ypak_close(&pk);
    }
    /* the arena: too small, then the size asked for */
    {
        static uint64_t arena[4096];
        ypak_desc d;
        memset(&d, 0, sizeof(d));
        d.data = m; d.size = (size_t)f.size; d.arena = arena; d.arena_size = 64;
        rc = open_rc(&pk, &d);
        CHECK(rc == YPAK_ERR_FULL && pk.need > 64 && pk.need <= sizeof(arena), "arena too small: %d need %zu", rc, pk.need);
        d.arena_size = pk.need;
        rc = open_rc(&pk, &d);
        CHECK(rc == 0, "arena of pk.need: %s", ypak_error(&pk));
        if (rc == 0) { check_contents(&pk, "arena"); ypak_close(&pk); }
    }
    /* a damaged chunk of a multi-chunk entry: a cursor reads chunk 0, refuses
     * chunk 1, and the whole entry is refused */
    {
        ypak_entry fe;
        ypak_cursor c;
        static uint8_t got[64];
        rc = open_mem(&pk, m, f.size, YPAK_VERIFY_USE);
        if (rc == 0 && ypak_find(&pk, "video/film.mpg", &fe) == 0) {
            ypak_close(&pk);
            m[fe.data_off + YPAK_CHUNK + 5] ^= 0x20;
            rc = open_mem(&pk, m, f.size, YPAK_VERIFY_USE);
            CHECK(rc == 0, "a damaged chunk is not seen at open");
            if (rc == 0) {
                CHECK(ypak_cursor_init(&c, &pk, &fe, NULL, 0) == 0, "cursor on the damaged entry");
                CHECK(ypak_cursor_read(&c, 10, got, 64) == 64 && memcmp(got, g_big + 10, 64) == 0, "chunk 0 reads");
                CHECK(ypak_cursor_read(&c, YPAK_CHUNK + 100, got, 64) == YPAK_ERR_CORRUPT, "chunk 1 refused");
                CHECK(ypak_cursor_read(&c, 2 * YPAK_CHUNK + 1, got, 64) == 64, "chunk 2 reads");
                CHECK(ypak_data(&pk, &fe) == NULL && strstr(ypak_error(&pk), "chunk 1"), "the whole entry refused: %s", ypak_error(&pk));
                ypak_close(&pk);
            }
            m[fe.data_off + YPAK_CHUNK + 5] ^= 0x20;
        } else if (rc == 0) {
            ypak_close(&pk);
        }
    }
    /* a misaligned memory base is refused */
    {
        uint8_t* mis = (uint8_t*)malloc((size_t)f.size + 8);
        memcpy(mis + 1, m, (size_t)f.size);
        CHECK(open_mem(&pk, mis + 1, f.size, YPAK_VERIFY_USE) == YPAK_ERR_ARG, "misaligned base refused");
        free(mis);
    }
    free(m);
    vf_free(&f);
}

static void test_sources(void) {
    vfile f;
    uint8_t* m;
    ypak_pack pk, pr;
    ypak_desc d;
    ypak_reader rd;
    int rc, i;
    build(&f, g_basic, N_BASIC, F_NONE, 0, 0);
    m = vf_flat(&f);
    /* a range reader */
    rd.read = vf_read;
    rd.size = f.size;
    memset(&d, 0, sizeof(d));
    d.reader = &rd;
    d.reader_ctx = &f;
    rc = open_rc(&pr, &d);
    CHECK(rc == 0, "reader open: %s", ypak_error(&pr));
    if (rc == 0) {
        uint8_t* scratch = (uint8_t*)malloc((size_t)YPAK_CHUNK);
        uint8_t* out = (uint8_t*)malloc(sizeof(g_big));
        for (i = 0; i < N_BASIC; i++) {
            ypak_entry e;
            ypak_cursor c;
            int64_t got;
            CHECK(ypak_find(&pr, g_basic[i].name, &e) == 0 && e.data == NULL, "reader find");
            CHECK(ypak_data(&pr, &e) == NULL, "reader: ypak_data gives NULL");
            CHECK(ypak_cursor_init(&c, &pr, &e, NULL, 0) == (e.size ? YPAK_ERR_ARG : 0), "reader cursor needs scratch");
            CHECK(ypak_cursor_init(&c, &pr, &e, scratch, (size_t)YPAK_CHUNK) == 0, "reader cursor: %s", ypak_error(&pr));
            got = ypak_cursor_read(&c, 0, out, e.size);
            CHECK(got == e.size && (e.size == 0 || memcmp(out, g_basic[i].data, (size_t)e.size) == 0), "reader cursor '%s'", g_basic[i].name);
            /* random access backwards */
            if (e.size > 10) {
                uint8_t two[2];
                CHECK(ypak_cursor_read(&c, e.size - 2, two, 2) == 2 && memcmp(two, g_basic[i].data + e.size - 2, 2) == 0, "reader tail");
                CHECK(ypak_cursor_read(&c, 3, two, 2) == 2 && memcmp(two, g_basic[i].data + 3, 2) == 0, "reader head again");
            }
        }
        CHECK(ypak_verify_all(&pr) == 0, "reader verify all: %s", ypak_error(&pr));
        free(scratch);
        free(out);
        ypak_close(&pr);
    }
    /* incremental, with a 64 KB tail */
    {
        size_t tail = 65536;
        int steps = 0;
        if ((int64_t)tail > f.size) tail = (size_t)f.size;
        memset(&d, 0, sizeof(d));
        d.tail = m + f.size - (int64_t)tail;
        d.tail_size = tail;
        d.file_size = f.size;
        rc = open_rc(&pk, &d);
        while (rc == YPAK_NEED && steps < 10) {
            rc = ypak_feed(&pk, m + pk.need_off, (size_t)pk.need_len);
            steps++;
        }
        CHECK(rc == 0 && steps == 2, "incremental open: rc %d after %d steps: %s", rc, steps, ypak_error(&pk));
        if (rc == 0) {
            for (i = 0; i < N_BASIC; i++) {
                ypak_entry e;
                CHECK(ypak_find(&pk, g_basic[i].name, &e) == 0, "incremental find");
                CHECK(ypak_check(&pk, &e, m + e.header_off, (size_t)(e.data_off + e.size - e.header_off)) == 0,
                      "incremental check '%s': %s", g_basic[i].name, ypak_error(&pk));
                CHECK(ypak_check(&pk, &e, m + e.header_off, 5) == YPAK_ERR_ARG, "check: wrong length");
                CHECK(ypak_check(&pk, &e, m + e.header_off, (size_t)(e.data_off + e.size - e.header_off) + 1) == YPAK_ERR_ARG,
                      "check: one byte too many");
            }
            /* a damaged fetch */
            {
                ypak_entry e;
                uint8_t* cp;
                size_t n;
                ypak_find(&pk, "conditions.pstb", &e);
                n = (size_t)(e.data_off + e.size - e.header_off);
                cp = (uint8_t*)malloc(n);
                memcpy(cp, m + e.header_off, n);
                cp[n - 1] ^= 4;
                CHECK(ypak_check(&pk, &e, cp, n) == YPAK_ERR_CORRUPT, "check: damaged bytes");
                free(cp);
            }
            ypak_close(&pk);
        }
        /* a tail of the whole file opens without asking */
        memset(&d, 0, sizeof(d));
        d.tail = m;
        d.tail_size = (size_t)f.size;
        d.file_size = f.size;
        CHECK(open_rc(&pk, &d) == 0, "incremental with everything: %s", ypak_error(&pk));
        ypak_close(&pk);
        /* a short tail */
        memset(&d, 0, sizeof(d));
        d.tail = m + f.size - 100;
        d.tail_size = 100;
        d.file_size = f.size;
        CHECK(!ypak_open(&pk, &d) && pk.error_code < 0, "tail of 100 bytes refused");
        CHECK(ypak_feed(&pk, m, 10) == YPAK_ERR_ARG, "feed without a need");
    }
#if !defined(YPAK_NO_FILE) && !defined(__EMSCRIPTEN__)
    /* a file, mapped */
    {
        const char* path = "pack_test_tmp.ysppak";
        FILE* o = fopen(path, "wb");
        if (o) {
            fwrite(m, 1, (size_t)f.size, o);
            fclose(o);
            memset(&d, 0, sizeof(d));
            d.path = path;
            rc = open_rc(&pk, &d);
            CHECK(rc == 0, "file open: %s", ypak_error(&pk));
            if (rc == 0) {
                ypak_entry e;
                check_contents(&pk, "file");
                ypak_find(&pk, "video/film.mpg", &e);
                CHECK(((uintptr_t)e.data & (YPAK_ALIGN - 1)) == 0, "mapped data is 4096-aligned");
                ypak_close(&pk);
            }
            remove(path);
        }
        memset(&d, 0, sizeof(d));
        d.path = "no such pack.ysppak";
        CHECK(open_rc(&pk, &d) == YPAK_ERR_IO, "missing file");
    }
#endif
    memset(&d, 0, sizeof(d));
    CHECK(open_rc(&pk, &d) == YPAK_ERR_ARG && open_rc(&pk, NULL) == YPAK_ERR_ARG, "no source");
    free(m);
    vf_free(&f);
}

static void test_appended(void) {
    int absolute;
    for (absolute = 0; absolute < 2; absolute++) {
        vfile f;
        uint8_t* m;
        ypak_pack pk;
        int rc;
        build(&f, g_basic, N_BASIC, F_NONE, 3 * YPAK_ALIGN, absolute);
        m = vf_flat(&f);
        rc = open_mem(&pk, m, f.size, YPAK_VERIFY_OPEN);
        CHECK(rc == 0, "appended (%s offsets): %s", absolute ? "absolute" : "relative", ypak_error(&pk));
        if (rc == 0) {
            CHECK(pk.zip_start == 3 * YPAK_ALIGN, "zip start %lld", (long long)pk.zip_start);
            check_contents(&pk, absolute ? "appended absolute" : "appended relative");
            ypak_close(&pk);
        }
        free(m);
        vf_free(&f);
    }
    {
        vfile f;
        uint8_t* m;
        ypak_pack pk;
        build(&f, g_basic, N_BASIC, F_NONE, 100, 0);
        m = vf_flat(&f);
        CHECK(open_mem(&pk, m, f.size, YPAK_VERIFY_USE) == YPAK_ERR_FORMAT, "a zip start off the 4096 grid is refused");
        free(m);
        vf_free(&f);
    }
}

static void test_faults(void) {
    int fault;
    for (fault = 1; fault < F_COUNT; fault++) {
        vfile f;
        uint8_t* m;
        ypak_pack pk;
        int rc, rc2 = 0;
        build(&f, g_basic, N_BASIC, fault, 0, 0);
        m = vf_flat(&f);
        rc = open_mem(&pk, m, f.size, YPAK_VERIFY_USE);
        if (rc == 0) {
            /* faults found only when the entry is used */
            rc2 = ypak_verify_all(&pk);
            ypak_close(&pk);
        }
        if (getenv("YPAK_TEST_VERBOSE")) printf("fault %-18s open %3d use %3d: %s\n", fault_names[fault], rc, rc2, ypak_error(&pk));
        if (fault == F_ENTRY_HASH || fault == F_DATA)
            CHECK(rc == 0 && rc2 == YPAK_ERR_CORRUPT, "fault '%s': open %d, use %d", fault_names[fault], rc, rc2);
        else if (fault == F_LOCAL_NAME || fault == F_PADDING || fault == F_LOCAL_CRC)
            CHECK((rc < 0) || rc2 == YPAK_ERR_FORMAT, "fault '%s': open %d, use %d", fault_names[fault], rc, rc2);
        else if (fault == F_FORMAT2)
            CHECK(rc == YPAK_ERR_VERSION, "fault '%s': %d (%s)", fault_names[fault], rc, ypak_error(&pk));
        else
            /* refused for the fault's own reason: a hash fault as corrupt,
             * a rule fault as format (a rule fault that only a hash
             * catches would mean the rule's check is missing) */
            CHECK(rc == ((fault == F_CDHASH || fault == F_MANIFEST_SHA || fault == F_CHUNK_TABLE) ? YPAK_ERR_CORRUPT : YPAK_ERR_FORMAT),
                  "fault '%s': %d (%s)", fault_names[fault], rc, ypak_error(&pk));
        free(m);
        vf_free(&f);
    }
    /* inputs the writer accepts but the format does not */
    {
        static const tin dup[] = { { "a.bin", YPAK_KIND_FILE, 0, g_small, 10, 0 }, { "a.bin", YPAK_KIND_FILE, 0, g_small, 10, 0 } };
        static const tin rsv[] = { { "ysp/other", YPAK_KIND_FILE, 0, g_small, 10, 0 } };
        static const tin two[] = { { "a.json", YPAK_KIND_EXPERIMENT, 0, g_small, 10, 0 }, { "b.json", YPAK_KIND_EXPERIMENT, 0, g_small, 10, 0 } };
        static const tin chk[] = { { "zz", YPAK_KIND_CHUNKS, 0, g_small, 10, 0 } };
        static const tin bad[] = { { "a/../b", YPAK_KIND_FILE, 0, g_small, 10, 0 } };
        const tin* cases[5];
        int counts[5] = { 2, 1, 2, 1, 1 }, k;
        cases[0] = dup; cases[1] = rsv; cases[2] = two; cases[3] = chk; cases[4] = bad;
        for (k = 0; k < 5; k++) {
            vfile f;
            uint8_t* m;
            ypak_pack pk;
            build(&f, cases[k], counts[k], F_NONE, 0, 0);
            m = vf_flat(&f);
            CHECK(open_mem(&pk, m, f.size, YPAK_VERIFY_USE) == YPAK_ERR_FORMAT, "bad input %d accepted", k);
            free(m);
            vf_free(&f);
        }
    }
}

/* Every single-bit flip of a small pack is refused at open or at use, or
 * leaves the bytes it gives unchanged. Every truncation is refused. */
static void test_flips(void) {
    static const tin small[] = {
        { "a.bin", YPAK_KIND_FILE, 0, g_small, 300, 0 },
        { "b.pstb", YPAK_KIND_TABLE, 0, g_mid, 200, 0 },
    };
    vfile f;
    uint8_t* m;
    ypak_pack pk;
    int64_t bit, len, accepted = 0, flips = 0;
    build(&f, small, 2, F_NONE, 0, 0);
    m = vf_flat(&f);
    for (bit = 0; bit < 8 * f.size; bit++) {
        int rc;
        m[bit >> 3] ^= (uint8_t)(1u << (bit & 7));
        rc = open_mem(&pk, m, f.size, YPAK_VERIFY_OPEN);
        if (rc == 0) {
            accepted++;
            ypak_close(&pk);
        }
        flips++;
        m[bit >> 3] ^= (uint8_t)(1u << (bit & 7));
    }
    CHECK(accepted == 0, "%lld of %lld bit flips accepted", (long long)accepted, (long long)flips);
    for (len = 0; len < f.size; len++) {
        if (open_mem(&pk, m, len, YPAK_VERIFY_USE) == 0) {
            CHECK(0, "truncation to %lld accepted", (long long)len);
            ypak_close(&pk);
            break;
        }
    }
    printf("pack_test: %lld bit flips and %lld truncations refused\n", (long long)flips, (long long)f.size);
    free(m);
    vf_free(&f);
}

/* 4.3 GB in one entry, then an entry past 4 GiB: zip64 by size, by local
 * header offset and by directory offset, read through a range reader. */
static void test_big(void) {
    static const tin big[] = {
        { "a_film.bin", YPAK_KIND_VIDEO, YPAK_F_STREAM, NULL, 4300000000LL, 1 },
        { "z_after.bin", YPAK_KIND_FILE, 0, g_small, 3000, 0 },
    };
    vfile f;
    ypak_pack pk;
    ypak_desc d;
    ypak_reader rd;
    int rc;
    build(&f, big, 2, F_NONE, 0, 0);
    rd.read = vf_read;
    rd.size = f.size;
    memset(&d, 0, sizeof(d));
    d.reader = &rd;
    d.reader_ctx = &f;
    rc = open_rc(&pk, &d);
    CHECK(rc == 0, "4.3 GB pack: %s", ypak_error(&pk));
    if (rc == 0) {
        ypak_entry e;
        ypak_cursor c;
        uint8_t* scratch = (uint8_t*)malloc((size_t)YPAK_CHUNK);
        uint8_t got[64];
        int64_t at = 4299999990LL, j;
        CHECK(ypak_find(&pk, "a_film.bin", &e) == 0 && e.size == 4300000000LL, "big entry size");
        CHECK(ypak_cursor_init(&c, &pk, &e, scratch, (size_t)YPAK_CHUNK) == 0, "big cursor");
        CHECK(ypak_cursor_read(&c, at, got, 64) == 10, "read across the end");
        for (j = 0; j < 10; j++) if (got[j] != pattern_at(at + j)) break;
        CHECK(j == 10, "big entry bytes at the end");
        CHECK(ypak_cursor_read(&c, 0x100000000LL, got, 8) == 8 && got[0] == pattern_at(0x100000000LL), "big entry at 4 GiB");
        CHECK(ypak_find(&pk, "z_after.bin", &e) == 0 && e.header_off > 0xFFFFFFFFLL, "entry past 4 GiB");
        CHECK(ypak_cursor_init(&c, &pk, &e, scratch, (size_t)YPAK_CHUNK) == 0 && ypak_cursor_read(&c, 0, scratch, 3000) == 3000 &&
              memcmp(scratch, g_small, 3000) == 0, "entry past 4 GiB reads: %s", ypak_error(&pk));
        free(scratch);
        ypak_close(&pk);
    }
    vf_free(&f);
}

/* 65,536 entries: zip64 by count. */
static void test_count(void) {
    enum { N = 65535 };
    tin* in = (tin*)malloc(sizeof(tin) * N);
    char* names = (char*)malloc((size_t)N * 12);
    vfile f;
    ypak_pack pk;
    ypak_desc d;
    ypak_reader rd;
    int i, rc;
    for (i = 0; i < N; i++) {
        snprintf(names + 12 * i, 12, "e%06d", i);
        in[i].name = names + 12 * i;
        in[i].kind = YPAK_KIND_FILE;
        in[i].flags = 0;
        in[i].data = g_small + (i % 100);
        in[i].size = i % 7;
        in[i].pattern = 0;
    }
    build(&f, in, N, F_NONE, 0, 0);
    rd.read = vf_read;
    rd.size = f.size;
    memset(&d, 0, sizeof(d));
    d.reader = &rd;
    d.reader_ctx = &f;
    rc = open_rc(&pk, &d);
    CHECK(rc == 0 && pk.n == N + 2, "65,537 entries: %s", ypak_error(&pk));
    if (rc == 0) {
        ypak_entry e;
        uint8_t got[8];
        ypak_cursor c;
        CHECK(ypak_find(&pk, "e054321", &e) == 0 && e.size == 54321 % 7, "find among 65,537");
        CHECK(ypak_cursor_init(&c, &pk, &e, got, sizeof(got)) == 0 && ypak_cursor_read(&c, 0, got, e.size) == e.size &&
              memcmp(got, g_small + 54321 % 100, (size_t)e.size) == 0, "read among 65,537");
        ypak_close(&pk);
    }
    vf_free(&f);
    free(in);
    free(names);
}

/* --- views --------------------------------------------------------------- */

/* docs/gfx.md's test vector: a square with a square hole, 2 x 2 bands. */
static const float g_tex[40] = { 0, 0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0, 0,
                                 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.75f, 0.25f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f,
                                 0.75f, 0.25f, 0.75f, 0.25f, 0.25f, 0.25f, 0, 0 };
static const uint32_t g_words[60] = {
    0x43505359u, 1, 1, 0, 0, 0, 0, 0, 12, 0, 0, 0,
    0, 0, 0x3f800000u, 0x3f800000u, 0x00020002u, 0, 0, 10,
    36, 6, 0, 0, 42, 6, 0, 0, 48, 6, 0, 0, 54, 6, 0, 0,
    0, 1, 7, 8, 5, 3, 1, 2, 6, 7, 5, 3, 2, 3, 5, 6, 8, 0, 1, 2, 6, 7, 8, 0 };

static size_t make_cset(uint8_t* b, uint32_t units) {
    memset(b, 0, 64);
    memcpy(b, "YSPCSET1", 8);
    w32(b + 8, 1); w32(b + 12, 64); w32(b + 16, units); w32(b + 20, 1); w32(b + 24, 1); w32(b + 28, 0);
    w64(b + 32, 10); w64(b + 40, 60); w64(b + 48, 64); w64(b + 56, 64 + 160);
    memcpy(b + 64, g_tex, 160);
    memcpy(b + 224, g_words, 240);
    return 464;
}

static void test_views(void) {
    static uint64_t buf[4096];
    uint8_t* b = (uint8_t*)buf;
    char err[200];
    size_t n;
    /* curve set */
    {
        ypak_cset s;
        n = make_cset(b, 0);
        CHECK(ypak_cset_view(b, (int64_t)n, &s, err, sizeof(err)) == 0 && s.n_texels == 10 && s.n_words == 60 && s.n_glyphs == 1 &&
              s.texels[4] == 1.0f && s.words[8] == 12, "cset view: %s", err);
        CHECK(ypak_cset_view(b, (int64_t)n - 4, &s, err, sizeof(err)) == YPAK_ERR_FORMAT, "cset short");
        b[8] = 2;
        CHECK(ypak_cset_view(b, (int64_t)n, &s, err, sizeof(err)) == YPAK_ERR_FORMAT, "cset version");
        b[8] = 1; b[224] = 0;
        CHECK(ypak_cset_view(b, (int64_t)n, &s, err, sizeof(err)) == YPAK_ERR_FORMAT, "cset words magic");
        b[224] = 0x59;
        /* the words 16 bytes later and the size to match: only the offset rule refuses it */
        memmove(b + 240, b + 224, 240);
        memset(b + 224, 0, 16);
        w64(b + 56, 64 + 176);
        CHECK(ypak_cset_view(b, (int64_t)n + 16, &s, err, sizeof(err)) == YPAK_ERR_FORMAT, "cset offsets");
        CHECK(ypak_cset_view(b + 4, (int64_t)n, &s, err, sizeof(err)) == YPAK_ERR_FORMAT, "cset misaligned");
    }
    /* artwork */
    {
        ypak_art a;
        double vb[4] = { 0, 0, 10, 10 };
        int i;
        memset(b, 0, 128);
        memcpy(b, "YSPARTW1", 8);
        w32(b + 8, 1); w32(b + 12, 80); w32(b + 16, 2);
        for (i = 0; i < 4; i++) { uint64_t v; memcpy(&v, &vb[i], 8); w64(b + 24 + 8 * i, v); }
        w64(b + 56, 80); w64(b + 64, 112); w64(b + 72, 464);
        w32(b + 80, 0); w32(b + 84, 0xFF0000FFu); w32(b + 88, 1); w32(b + 92, 0);
        w32(b + 96, 0); w32(b + 100, 0x00FF00FFu); w32(b + 104, 2); w32(b + 108, 0);
        make_cset(b + 112, 1);
        CHECK(ypak_art_view(b, 576, &a, err, sizeof(err)) == 0 && a.n_layers == 2 && a.viewbox[2] == 10 && a.layers[1].rgba == 0x00FF00FFu,
              "art view: %s", err);
        w32(b + 96, 5);
        CHECK(ypak_art_view(b, 576, &a, err, sizeof(err)) == YPAK_ERR_FORMAT, "art glyph out of range");
        w32(b + 96, 0); w32(b + 128, 0);
        CHECK(ypak_art_view(b, 576, &a, err, sizeof(err)) == YPAK_ERR_FORMAT, "art set in em units");
    }
    /* shader */
    {
        ypak_shader s;
        static const char body[] = "float ysp_main(vec2 p) { return sin(p.x * ysp_param(0)); }";
        uint32_t bl = (uint32_t)strlen(body);
        size_t total;
        memset(b, 0, 256);
        memcpy(b, "YSPSHAD1", 8);
        w32(b + 8, 1); w32(b + 12, 64); w32(b + 16, 0); w32(b + 20, 0); w32(b + 24, 1); w32(b + 28, 0); w32(b + 32, 1);
        w64(b + 40, 1234); w32(b + 48, 64); w32(b + 52, bl); w32(b + 56, 64 + bl + 1); w32(b + 60, 5);
        memcpy(b + 64, body, bl + 1);
        memcpy(b + 64 + bl + 1, "plaid", 6);
        total = (64 + bl + 1 + 6 + 15) & ~(size_t)15;
        CHECK(ypak_shader_view(b, (int64_t)total, &s, err, sizeof(err)) == 0 && s.body_len == bl && strcmp(s.name, "plaid") == 0 &&
              s.params == 1 && s.contract == 1, "shader view: %s", err);
        b[64 + bl] = 'x';
        CHECK(ypak_shader_view(b, (int64_t)total, &s, err, sizeof(err)) == YPAK_ERR_FORMAT, "shader body not NUL-ended");
        b[64 + bl] = 0;
        w32(b + 16, 3);
        CHECK(ypak_shader_view(b, (int64_t)total, &s, err, sizeof(err)) == YPAK_ERR_FORMAT, "shader mode");
    }
    /* texture: raw and QOI round trip */
    {
        enum { W = 37, H = 23 };
        static uint8_t img[W * H * 4], dec[W * H * 4];
        ypak_texture t;
        size_t q, i;
        uint32_t x = 7;
        for (i = 0; i < sizeof(img); i++) {
            x = x * 1103515245u + 12345u;
            img[i] = (i / 4) % 5 == 0 ? (uint8_t)(x >> 16) : (uint8_t)((i / 4) / 13);   /* runs, diffs and noise */
            if (i % 4 == 3) img[i] = (i / 4) % 31 == 0 ? 128 : 255;
        }
        memset(b, 0, 64);
        memcpy(b, "YSPTEXR1", 8);
        w32(b + 8, 1); w32(b + 12, 64); w32(b + 16, W); w32(b + 20, H); w32(b + 24, YPAK_TEX_RGBA8);
        b[28] = 1; b[29] = 2; b[30] = 3;
        w32(b + 36, 0); w64(b + 40, sizeof(img)); w64(b + 48, sizeof(img)); w64(b + 56, ypak_xxh64(img, sizeof(img), 0));
        memcpy(b + 64, img, sizeof(img));
        n = (64 + sizeof(img) + 15) & ~(size_t)15;
        CHECK(ypak_texture_view(b, (int64_t)n, &t, err, sizeof(err)) == 0 && t.w == W && t.texel_bytes == 4 && memcmp(t.data, img, sizeof(img)) == 0,
              "texture raw: %s", err);
        q = ypak_qoi_encode(img, W, H, b + 64, sizeof(buf) - 64);
        CHECK(q > 22 && q < 14 + 5 * sizeof(img) / 4 + 8, "QOI encode: %zu", q);
        w32(b + 36, 1); w64(b + 40, q);
        n = (64 + q + 15) & ~(size_t)15;
        memset(b + 64 + q, 0, n - 64 - q);
        CHECK(ypak_texture_view(b, (int64_t)n, &t, err, sizeof(err)) == 0, "texture qoi view: %s", err);
        CHECK(ypak_qoi_decode(&t, dec, sizeof(dec), err, sizeof(err)) == 0 && memcmp(dec, img, sizeof(img)) == 0, "QOI round trip: %s", err);
        b[64 + q / 2] ^= 0x40;
        CHECK(ypak_qoi_decode(&t, dec, sizeof(dec), err, sizeof(err)) == YPAK_ERR_FORMAT, "QOI damaged");
        b[64 + q / 2] ^= 0x40;
        t.raw_hash ^= 1;
        CHECK(ypak_qoi_decode(&t, dec, sizeof(dec), err, sizeof(err)) == YPAK_ERR_FORMAT && strstr(err, "raw hash"), "QOI raw hash: %s", err);
        CHECK(ypak_qoi_decode(&t, dec, 10, err, sizeof(err)) == YPAK_ERR_FORMAT, "QOI short dst");
        w32(b + 24, YPAK_TEX_R8); w64(b + 48, (uint64_t)W * H);
        CHECK(ypak_texture_view(b, (int64_t)n, &t, err, sizeof(err)) == YPAK_ERR_FORMAT, "QOI on R8 refused");
    }
    /* glyph runs */
    {
        ypak_runs r;
        uint8_t* p = b;
        size_t po;
        float item[8] = { 1, 2, 0, 5, 1, 1, 1, 0 };
        memset(p, 0, 1024);
        memcpy(p, "YSPGRUN1", 8);
        w32(p + 8, 1); w32(p + 12, 112);
        w32(p + 16, 1); w32(p + 20, 2); w32(p + 24, 2); w32(p + 28, 2); w32(p + 32, 2);
        /* fonts at 112 (16), blocks at 128 (192), runs at 320 (32), items at 352 (64), clusters at 416 (8 -> 16), lines at 432 (64), pool at 496 */
        w64(p + 40, 112); w64(p + 48, 128); w64(p + 56, 320); w64(p + 64, 352); w64(p + 72, 416); w64(p + 80, 432); w64(p + 88, 496);
        po = 496;
        memcpy(p + po, "f.yspcset\0f.ttf\0a\0hi\0en\0b\0yo\0", 29);
        w64(p + 96, 29);
        w32(p + 112, 0); w32(p + 116, 9); w32(p + 120, 10); w32(p + 124, 5);
        /* block a: key 16 "a", text 18 "hi", lang 21 "en" */
        w32(p + 128, 16); w32(p + 132, 1); w32(p + 136, 18); w32(p + 140, 2); w32(p + 144, 21); w32(p + 148, 2);
        w32(p + 192 - 16 + 0, 0);
        w32(p + 128 + 64, 0); w32(p + 128 + 68, 1); w32(p + 128 + 72, 0); w32(p + 128 + 76, 1); w32(p + 128 + 80, 0); w32(p + 128 + 84, 1);
        /* block b: key 24 "b", text 26 "yo", lang 21 */
        w32(p + 224, 24); w32(p + 228, 1); w32(p + 232, 26); w32(p + 236, 2); w32(p + 240, 21); w32(p + 244, 2);
        w32(p + 224 + 64, 1); w32(p + 224 + 68, 1); w32(p + 224 + 72, 1); w32(p + 224 + 76, 1); w32(p + 224 + 80, 1); w32(p + 224 + 84, 1);
        /* runs: font 0, first 0, n 1 */
        w32(p + 320, 0); w32(p + 328, 0); w32(p + 332, 1);
        w32(p + 336, 0); w32(p + 344, 0); w32(p + 348, 1);
        memcpy(p + 352, item, 32);
        memcpy(p + 384, item, 32);
        n = (496 + 29 + 15) & ~(size_t)15;
        CHECK(ypak_runs_view(p, (int64_t)n, &r, err, sizeof(err)) == 0 && r.n_blocks == 2, "runs view: %s", err);
        CHECK(ypak_runs_find(&r, "b") == 1 && ypak_runs_find(&r, "a") == 0 && ypak_runs_find(&r, "c") == -1, "runs find");
        CHECK(strcmp(ypak_runs_str(&r, r.blocks[1].text_off), "yo") == 0 && strcmp(ypak_runs_str(&r, r.fonts[0].cset_off), "f.yspcset") == 0, "runs strings");
        w32(p + 224, 16);
        CHECK(ypak_runs_view(p, (int64_t)n, &r, err, sizeof(err)) == YPAK_ERR_FORMAT, "runs duplicate keys");
        w32(p + 224, 24); w32(p + 332, 2);
        CHECK(ypak_runs_view(p, (int64_t)n, &r, err, sizeof(err)) == YPAK_ERR_FORMAT, "runs range");
        w32(p + 332, 1); w32(p + 320, 1);
        CHECK(ypak_runs_view(p, (int64_t)n, &r, err, sizeof(err)) == YPAK_ERR_FORMAT, "runs font index");
    }
    CHECK(ypak_kind_id("glyphruns") == YPAK_KIND_GLYPHRUNS && ypak_kind_name(YPAK_KIND_ALPHA) && !ypak_kind_name(0) && !ypak_kind_name(18) &&
          ypak_kind_id("nope") == 0, "kind names");
}

/* v0.2.0: lookups on several threads fail at once; each failure writes the
 * message under an atomic claim, so the message after them is one whole
 * failure's text, never a mix. The two names share no byte past their
 * prefix, so a torn write shows as both letters. Close is safe on a zeroed
 * handle, after a failed open and twice. */
#define CLAIM_N 20000
static ypak_pack g_claim_pk;
static const char* const g_claim_names[2] = {
    "missing/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "missing/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
};
static int g_claim_bad[2];

static void claim_run(int k) {
    int i;
    ypak_entry e;
    for (i = 0; i < CLAIM_N; i++)
        if (ypak_find(&g_claim_pk, g_claim_names[k], &e) != YPAK_ERR_NOT_FOUND) g_claim_bad[k]++;
}

#if defined(_WIN32)
#include <windows.h>
static DWORD WINAPI claim_thread(LPVOID arg) { claim_run((int)(intptr_t)arg); return 0; }
#define CLAIM_THREADS 1
#elif !defined(__EMSCRIPTEN__)
#include <pthread.h>
static void* claim_thread(void* arg) { claim_run((int)(intptr_t)arg); return NULL; }
#define CLAIM_THREADS 1
#endif

/* ypak_runs_key(): the byte format of docs/pack.md 4.5, the canonical
 * numbers, the refusals, and sizing with cap 0 or a short buffer. */
static void test_runs_key(void) {
    ypak_text_style st;
    char buf[256];
    int64_t n;
    memset(&st, 0, sizeof st);
    st.font = "fonts/NotoSans-Regular.ttf";
    st.size = 72.0;
    st.width = 1000.0;
    n = ypak_runs_key(&st, "RED", 3, buf, sizeof buf);
    CHECK(n == 54 && !strcmp(buf, "fonts/NotoSans-Regular.ttf#0|72|1000|0|start|auto||RED"), "key: %lld '%s'", (long long)n, buf);
    st.face = 2; st.size = 0.1 + 0.2; st.width = 1e21; st.line_height = 1.25; st.align = YPAK_ALIGN_CENTER; st.dir = YPAK_DIR_RTL;
    st.lang = "he-IL";
    n = ypak_runs_key(&st, "a|b#c", 5, buf, sizeof buf);
    CHECK(n > 0 && !strcmp(buf, "fonts/NotoSans-Regular.ttf#2|0.30000000000000004|1e+21|1.25|center|rtl|he-IL|a|b#c"),
          "key with shortest doubles, an exponent and separators in the text: '%s'", buf);
    st.align = YPAK_ALIGN_END; st.dir = YPAK_DIR_LTR; st.size = 0.8; st.width = -0.0; st.line_height = 0; st.lang = NULL;
    n = ypak_runs_key(&st, "", 0, buf, sizeof buf);
    CHECK(n > 0 && !strcmp(buf, "fonts/NotoSans-Regular.ttf#2|0.8|0|0|end|ltr||"), "empty text, -0 as 0, NULL lang: '%s'", buf);
    /* sizing: cap 0 and a short buffer give the length and write nothing whole */
    memset(buf, 'x', sizeof buf);
    CHECK(ypak_runs_key(&st, "", 0, NULL, 0) == n && ypak_runs_key(&st, "", 0, buf, (size_t)n) == n && buf[0] == 'x' && buf[n] == 'x',
          "sizing: cap 0, and a buffer of n bytes left untouched");
    memset(buf, 'x', sizeof buf);
    CHECK(ypak_runs_key(&st, "", 0, buf, (size_t)n + 1) == n && buf[n] == 0, "a buffer of n + 1 holds the key and its NUL");
    /* refusals */
    {
        ypak_text_style b = st;
        volatile double zero = 0;   /* NaN and infinity at run time, not as constants */
        static const char* const fonts[4] = { "", "a#b.ttf", "a|b.ttf", "/abs.ttf" };
        int k;
        for (k = 0; k < 4; k++) { b = st; b.font = fonts[k]; CHECK(ypak_runs_key(&b, "x", 1, buf, sizeof buf) == YPAK_ERR_ARG, "font '%s'", fonts[k]); }
        b = st; b.font = NULL; CHECK(ypak_runs_key(&b, "x", 1, buf, sizeof buf) == YPAK_ERR_ARG, "no font");
        b = st; b.face = -1; CHECK(ypak_runs_key(&b, "x", 1, buf, sizeof buf) == YPAK_ERR_ARG, "face -1");
        b = st; b.size = 0; CHECK(ypak_runs_key(&b, "x", 1, buf, sizeof buf) == YPAK_ERR_ARG, "size 0");
        b = st; b.size = zero / zero; CHECK(ypak_runs_key(&b, "x", 1, buf, sizeof buf) == YPAK_ERR_ARG, "size NaN");
        b = st; b.width = -1; CHECK(ypak_runs_key(&b, "x", 1, buf, sizeof buf) == YPAK_ERR_ARG, "width -1");
        b = st; b.line_height = 1.0 / zero; CHECK(ypak_runs_key(&b, "x", 1, buf, sizeof buf) == YPAK_ERR_ARG, "line height inf");
        b = st; b.align = 3; CHECK(ypak_runs_key(&b, "x", 1, buf, sizeof buf) == YPAK_ERR_ARG, "align 3");
        b = st; b.dir = -1; CHECK(ypak_runs_key(&b, "x", 1, buf, sizeof buf) == YPAK_ERR_ARG, "dir -1");
        b = st; b.lang = "en|x"; CHECK(ypak_runs_key(&b, "x", 1, buf, sizeof buf) == YPAK_ERR_ARG, "lang with '|'");
        b = st; b.lang = "en_US"; CHECK(ypak_runs_key(&b, "x", 1, buf, sizeof buf) == YPAK_ERR_ARG, "lang with '_'");
        CHECK(ypak_runs_key(&st, "a\0b", 3, buf, sizeof buf) == YPAK_ERR_ARG, "a NUL in the text");
        CHECK(ypak_runs_key(NULL, "x", 1, buf, sizeof buf) == YPAK_ERR_ARG && ypak_runs_key(&st, NULL, 1, buf, sizeof buf) == YPAK_ERR_ARG,
              "NULL style or text");
    }
}

static void test_claim(void) {
    vfile f;
    uint8_t* m;
    ypak_pack z;
    ypak_desc d;
    char want[2][160];
    int k;
    memset(&z, 0, sizeof(z));
    ypak_close(&z);
    ypak_close(&z);
    ypak_close(NULL);
    memset(&d, 0, sizeof(d));
    CHECK(!ypak_open(&z, &d) && z.error_code == YPAK_ERR_ARG && strstr(ypak_error(&z), "names no source"),
          "a desc with no source: %s", ypak_error(&z));
    ypak_close(&z);
    ypak_close(&z);
    CHECK(strcmp(ypak_strerror(YPAK_ERR_CORRUPT), "hash mismatch") == 0 && strcmp(ypak_strerror(0), "ok") == 0 &&
          strcmp(ypak_strerror(-99), "unknown error") == 0, "ypak_strerror");
    build(&f, g_basic, N_BASIC, F_NONE, 0, 0);
    m = vf_flat(&f);
    if (open_mem(&g_claim_pk, m, f.size, YPAK_VERIFY_USE) != 0) {
        CHECK(0, "claim: open: %s", ypak_error(&g_claim_pk));
    } else {
        CHECK(ypak_error(&g_claim_pk)[0] == 0 && g_claim_pk.error_code == 0, "open leaves no message");
        for (k = 0; k < 2; k++) snprintf(want[k], sizeof(want[k]), "ysp_pack: no entry '%s'", g_claim_names[k]);
#if defined(CLAIM_THREADS)
        {
#if defined(_WIN32)
            HANDLE th[2];
            for (k = 0; k < 2; k++) th[k] = CreateThread(NULL, 0, claim_thread, (LPVOID)(intptr_t)k, 0, NULL);
            for (k = 0; k < 2; k++) if (th[k]) { WaitForSingleObject(th[k], INFINITE); CloseHandle(th[k]); }
#else
            pthread_t th[2];
            for (k = 0; k < 2; k++) pthread_create(&th[k], NULL, claim_thread, (void*)(intptr_t)k);
            for (k = 0; k < 2; k++) pthread_join(th[k], NULL);
#endif
        }
#else
        claim_run(0);
        claim_run(1);
#endif
        CHECK(g_claim_bad[0] == 0 && g_claim_bad[1] == 0, "claim: every lookup is NOT_FOUND");
        CHECK(strcmp(ypak_error(&g_claim_pk), want[0]) == 0 || strcmp(ypak_error(&g_claim_pk), want[1]) == 0,
              "claim: the message is one failure's, whole: %s", ypak_error(&g_claim_pk));
        CHECK(g_claim_pk.error_code == YPAK_ERR_NOT_FOUND && g_claim_pk.err_claim_ == 0, "claim: code, released");
        ypak_close(&g_claim_pk);
        ypak_close(&g_claim_pk);
    }
    free(m);
    vf_free(&f);
}

int main(void) {
    const char* quick = getenv("YPAK_TEST_QUICK");
    int full = !(quick && quick[0] == '1');
    fill();
    test_hashes();
    test_names();
    test_basic();
    test_sources();
    test_appended();
    test_faults();
    test_views();
    test_claim();
    test_runs_key();
    if (full) {
        test_flips();
        test_big();
        test_count();
    }
    printf("pack_test: %d checks, %d failed%s\n", g_checks, g_fail, full ? "" : " (quick)");
    return g_fail ? 1 : 0;
}
