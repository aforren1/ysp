/* ysp/pack.h - v0.1.0 - public domain single-header pack reader
 *
 *   Reads a ysp pack (docs/pack.md): one zip file in store-only mode, every
 *   entry's data aligned to 4096 bytes, a JSON manifest first, a private
 *   central directory record per entry, XXH64 hashes over 1 MiB chunks, and
 *   the pack ID (the SHA-256 of the manifest) in the zip comment. Opens a
 *   pack from a mapped file, from memory, from a range reader (bytes by
 *   offset, for example HTTP range requests), or incrementally from bytes
 *   the caller fetched (a browser). Gives entries by name with no copy,
 *   verifies them on first use, and streams large entries through cursors
 *   that verify each chunk as it is read. Every byte is treated as hostile:
 *   a pack from another lab is checked for memory safety before any pointer
 *   leaves the reader. The pack tool (pack/ysp/pack_tool.h, the `ypak` CLI)
 *   writes packs; this header only reads them.
 *
 *   Written in the single-header style of the stb / sokol libraries. No
 *   heap after open, no threads, no dependency. File mapping uses the OS
 *   (Windows or POSIX) unless YPAK_NO_FILE is defined; everything else is
 *   pure computation. C99 is the floor: it builds as C99, C11 and C++17.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Do this:
 *
 *       #define YSP_PACK_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header to create the
 *   implementation. Every other file just includes the header normally.
 *
 *       ypak_pack pk;
 *       ypak_desc d = { 0 };
 *       d.path = "study.ysppak";
 *       if (ypak_open(&pk, &d) != 0) { fputs(ypak_error(&pk), stderr); return 1; }
 *       ypak_entry e;
 *       if (ypak_find(&pk, "conditions.pstb", &e) == 0) {
 *           const void* p = ypak_data(&pk, &e);      // verified, zero-copy
 *           ytb_table t;
 *           if (p && ytb_open_view(&t, p, (size_t)e.size)) ...
 *       }
 *       // a long sound or a film, read on the producer thread:
 *       ypak_cursor c;
 *       ypak_cursor_init(&c, &pk, &e, NULL, 0);     // mapped: no scratch
 *       yau_reader rd = { ypak_cursor_read, e.size };
 *       ... yau_wav_desc wd = { .reader = &rd, .reader_ctx = &c };
 *       ypak_close(&pk);
 *
 *   ---------------------------------------------------------------------
 *   SOURCES
 *   ---------------------------------------------------------------------
 *   desc.path     the file, mapped whole and read-only (YPAK_FILE, the
 *                 default; YPAK_NO_FILE removes it). UTF-8 on every
 *                 platform. A player with a pack appended to it opens its
 *                 own executable.
 *   desc.data     the whole pack in memory, 8-byte aligned, kept by the
 *                 caller until ypak_close().
 *   desc.reader   bytes by range: read(ctx, offset, buf, n) returns n, or
 *                 less on failure. The same layout as yau_reader and
 *                 yvid_reader. ypak_data() returns NULL in this mode; read
 *                 through a cursor or fetch and ypak_check().
 *   desc.tail     incremental: the last tail_size bytes of a file of
 *                 file_size bytes (a suffix range of 64 KB or more holds
 *                 the end record). ypak_open() returns YPAK_NEED with
 *                 pk.need_off and pk.need_len; fetch exactly that range and
 *                 pass it to ypak_feed(), which returns YPAK_NEED again or
 *                 0 when the pack is open. The reader does no I/O. Then
 *                 fetch each entry's [header_off, data_off + size) and pass
 *                 it to ypak_check().
 *
 *   ---------------------------------------------------------------------
 *   VERIFICATION
 *   ---------------------------------------------------------------------
 *   Always at open: the end record and the comment (format version, pack
 *   ID, directory hash), the central directory's XXH64, every rule of the
 *   container (docs/pack.md 2.2 to 2.10), the manifest's SHA-256 against the
 *   pack ID, the chunk table, and each multi-chunk entry's chunk list
 *   against its entry hash. Then, by desc.verify:
 *     YPAK_VERIFY_USE   (0, default) an entry when ypak_data() first gives
 *                       it, a chunk when a cursor first reads it, an entry
 *                       when ypak_check() gets it.
 *     YPAK_VERIFY_OPEN  every entry at open: the whole pack is read.
 *     YPAK_VERIFY_NONE  no data hashes after open; ypak_describe() says so.
 *   Each entry's local header is checked against its central record (and
 *   its padding against zeros) on first access in every policy. An entry
 *   or chunk is hashed once per open pack. A mismatch is YPAK_ERR_CORRUPT
 *   with the entry's name and the chunk. The hashes find corruption, not
 *   forgery.
 *
 *   ---------------------------------------------------------------------
 *   THREADS AND COST
 *   ---------------------------------------------------------------------
 *   After open, any number of threads may call the lookups, ypak_data(),
 *   ypak_check() and their own cursors; verified flags are bytes stored
 *   with release semantics, so two threads that verify one chunk both
 *   store 1. ypak_error() holds the last failure of any thread. A cursor
 *   belongs to one thread. Nothing allocates after open: open makes one
 *   block (desc.arena, or YPAK_MALLOC) for the index (56 bytes an entry),
 *   one flag byte per entry and per chunk, and, without a mapping, copies
 *   of the central directory and the chunk table. Lookups are a binary
 *   search. Hashing runs on the thread that asks: keep ypak_data() out of
 *   the frame loop (load time), and cursors on decode or producer threads.
 *
 *   ---------------------------------------------------------------------
 *   VIEWS
 *   ---------------------------------------------------------------------
 *   ypak_cset_view(), ypak_art_view(), ypak_runs_view(), ypak_shader_view()
 *   and ypak_texture_view() check the bounds of the pack's own headers
 *   (docs/pack.md 4) and give pointers into the entry. The deep checks stay
 *   with each form's owner: ygfx_cset_check() for curve sets, ytb_open_view()
 *   for tables, ysp/color.h for calibrations, ysp/audio.h for WAV,
 *   ysp/video.h for video and its index.
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   Nothing to link. Define YPAK_API to override `extern`, YPAK_MALLOC and
 *   YPAK_FREE to replace malloc and free (used at open and close only). A
 *   big-endian host refuses to open: the format is little-endian.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.1.0 - first version: the container of docs/pack.md format 1, the
 *          index and lookup, the four sources, the three verification
 *          policies, cursors, the views of the pack's own forms (curve
 *          set, artwork, glyph runs, shader, texture, QOI), XXH64,
 *          SHA-256 and CRC-32.
 *
 *   STATUS: v0.1.0, 2026-10-08. Built and run with MSVC 19.44 (/W4 /WX),
 *   MinGW-w64 gcc 16.1 (-Wall -Wextra -Wpedantic -Wshadow -Werror; also
 *   C99), emcc 6.0.10 under node, and Linux gcc 11.4 (WSL2) as C11 and
 *   C++17 under ASan and UBSan with no report. tests/adapt/pack_test.c
 *   (its own writer, independent of the tool's): every one of 135,240
 *   single-bit flips and 16,905 truncations of a small pack refused; 29
 *   rule faults each refused with its own error; the four sources;
 *   cursors over a damaged chunk; a virtual 4.3 GB entry and 65,537
 *   entries (zip64 by size, offset and count); the views. Fuzzed
 *   (tests/fuzz/pack_fuzz.c, MSVC libFuzzer with ASan): 28,706,212 inputs
 *   in 3601 s, no finding; the 750 corpus files replayed on Linux under
 *   ASan and UBSan with no report. Mutations: 36 in
 *   tests/mutate/pack.toml, 36 killed. Costs (Iris Xe laptop, AC,
 *   measurement lock, examples/pack/bench.c, gcc 16.1 -O2 / MSVC 19.44
 *   /O2): open of a mapped pack 0.23 / 0.25 ms at 100 entries and
 *   14.7 / 15.6 ms at 10,000 (most of it the manifest's SHA-256); lookup
 *   0.06 to 0.14 us; entry hash 11.6 to 17.1 GB/s; verify_all of a 1 GiB
 *   pack at mapped first touch 3.45 / 3.65 GB/s, of 10,000 small entries
 *   1.51 / 1.20 GB/s; one 1 MiB chunk through a cursor over fread 0.28 ms;
 *   0 allocations after open. Six zip readers read the packs the tool
 *   writes (docs/pack.md 9.1).
 *   What is NOT done: no run on macOS or ARM; a player signed after its
 *   pack was appended (the end record must end the file); big-endian
 *   hosts are refused.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_PACK_H_INCLUDED
#define YSP_PACK_H_INCLUDED

#define YPAK_VERSION_MAJOR 0
#define YPAK_VERSION_MINOR 1
#define YPAK_VERSION_PATCH 0
#define YPAK_VERSION_STRING "0.1.0"

/* The file source's mmap(), open() and fstat() under -std=c11: the same
 * feature-test macro, for the same reasons, as ysp/rt.h's (set before the
 * first system header; Linux and Emscripten only). */
#if (defined(__linux__) || defined(__EMSCRIPTEN__)) && !defined(_DEFAULT_SOURCE) &&     !defined(_GNU_SOURCE) && !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
    #define _DEFAULT_SOURCE 1
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YPAK_API
#define YPAK_API extern
#endif

#define YPAK_FORMAT       1                      /* the pack format this reader reads */
#define YPAK_ALIGN        4096
#define YPAK_CHUNK_LOG2   20
#define YPAK_CHUNK        ((int64_t)1 << YPAK_CHUNK_LOG2)
#define YPAK_MAX_ENTRIES  (1u << 20)
#define YPAK_MAX_NAME     1024
#define YPAK_MAX_CD       ((int64_t)256 << 20)
#define YPAK_NO_CHUNK     0xFFFFFFFFu
#define YPAK_MANIFEST     "ysp/manifest.json"
#define YPAK_CHUNKS       "ysp/chunks"
#define YPAK_EXT_ID       0x7379u                /* the private central record */
#define YPAK_EXT_SIZE     28u

/* Return codes. YPAK_NEED is not an error: fetch pk.need_off, need_len. */
#define YPAK_OK             0
#define YPAK_NEED           1
#define YPAK_ERR_ARG      (-1)
#define YPAK_ERR_IO       (-2)
#define YPAK_ERR_FULL     (-4)    /* desc.arena too small; pk.need bytes     */
#define YPAK_ERR_FORMAT   (-8)    /* outside the format; the message names it */
#define YPAK_ERR_NOMEM   (-10)
#define YPAK_ERR_CORRUPT (-13)    /* a hash does not match                   */
#define YPAK_ERR_VERSION (-14)    /* another pack format version             */
#define YPAK_ERR_NOT_FOUND (-15)

/* Kinds (docs/pack.md 2.11). The ids are part of format 1. */
enum {
    YPAK_KIND_MANIFEST = 1, YPAK_KIND_CHUNKS, YPAK_KIND_EXPERIMENT, YPAK_KIND_FILE,
    YPAK_KIND_TABLE, YPAK_KIND_CALIBRATION, YPAK_KIND_FONT, YPAK_KIND_CURVESET,
    YPAK_KIND_GLYPHRUNS, YPAK_KIND_ARTWORK, YPAK_KIND_AUDIO, YPAK_KIND_SHADER,
    YPAK_KIND_TEXTURE, YPAK_KIND_VIDEO, YPAK_KIND_VIDEO_INDEX, YPAK_KIND_FRAMESEQ,
    YPAK_KIND_ALPHA,
    YPAK_KIND_COUNT                  /* 18; SCRIPT (18) and EXTENSION (19) reserved */
};
#define YPAK_F_STREAM 0x1u           /* read through a cursor                  */

/* "table" for YPAK_KIND_TABLE; NULL for an unknown id. */
YPAK_API const char* ypak_kind_name(uint32_t kind);
/* The id of a name, or 0. */
YPAK_API uint32_t ypak_kind_id(const char* name);

typedef struct ypak_reader {
    int64_t (*read)(void* ctx, int64_t offset, void* buf, int64_t n);   /* bytes, or < 0 */
    int64_t size;
} ypak_reader;

typedef enum ypak_verify {
    YPAK_VERIFY_USE = 0,
    YPAK_VERIFY_OPEN,
    YPAK_VERIFY_NONE
} ypak_verify;

typedef struct ypak_desc {
    const char*        path;         /* a file, mapped (not with YPAK_NO_FILE); or */
    const void*        data;         /* the pack in memory, 8-byte aligned; or      */
    size_t             size;
    const ypak_reader* reader;       /* bytes by range; or                          */
    void*              reader_ctx;
    const void*        tail;         /* incremental: the last tail_size bytes ...   */
    size_t             tail_size;
    int64_t            file_size;    /* ... of a file of this many bytes            */
    ypak_verify        verify;
    void*              arena;        /* the index; NULL: YPAK_MALLOC at open        */
    size_t             arena_size;
} ypak_desc;

typedef struct ypak_entry {
    const char* name;                /* in the central directory; not NUL-ended     */
    uint32_t    name_len;
    uint32_t    index;               /* 0 is the manifest                           */
    uint32_t    kind, flags;
    int64_t     size;
    int64_t     header_off;          /* absolute file offsets                       */
    int64_t     data_off;
    uint64_t    hash;                /* the entry hash (docs/pack.md 2.8)           */
    uint32_t    crc32;
    uint32_t    chunk_first;         /* YPAK_NO_CHUNK: one chunk                    */
    const void* data;                /* in the mapping or memory; NULL otherwise.
                                      * Not verified: use ypak_data()              */
} ypak_entry;

typedef struct ypak__ent {           /* private: the index                         */
    int64_t  hdr, data, size;
    uint64_t hash;
    uint32_t name, name_len;
    uint32_t kind, flags, crc, chunk;
} ypak__ent;

typedef struct ypak_pack {
    uint32_t n;                      /* entries, the manifest included              */
    int32_t  format;
    int64_t  file_size;
    int64_t  zip_start;              /* absolute offset of the first local header   */
    int64_t  base;                   /* what the records count from: 0, or zip_start
                                      * (offsets relative to the zip start)         */
    uint8_t  id[32];                 /* the pack ID: SHA-256 of the manifest        */
    uint32_t n_chunks;               /* hashes in ysp/chunks                        */
    int32_t  experiment;             /* the EXPERIMENT entry's index, or -1          */
    int64_t  need_off, need_len;     /* after YPAK_NEED                             */
    size_t   need;                   /* arena bytes open needs (YPAK_ERR_FULL)      */
    uint64_t hashed_bytes;           /* data hashed so far (approximate with threads) */
    char     error[256];
    /* private */
    const uint8_t*     base_;
    const ypak_reader* rd_;
    void*              rdc_;
    const uint8_t*     cd_;
    int64_t            cd_abs_, cd_size_, b_;
    uint64_t           cd_hash_;
    ypak__ent*       ent_;
    uint8_t*           ok_;           /* per entry: 1 header checked, 2 hashed      */
    uint8_t*           cok_;          /* per chunk: 1 hashed                         */
    const uint8_t*     chunks_;       /* the hashes, after the table's header         */
    uint32_t           i_chunks_;
    int32_t            verify_;
    int32_t            state_;
    int32_t            open_;
    uint8_t*           arena_;
    size_t             arena_size_, arena_used_;
    void*              blk_[3];       /* YPAK_MALLOC blocks: directory, index, chunk table */
    uint32_t           n_rec_;
    int64_t            cd_off_rec_;
    void*              map_;
    intptr_t           fh_, mh_;
    int64_t            map_size_;
} ypak_pack;

YPAK_API int  ypak_open(ypak_pack* p, const ypak_desc* d);    /* 0, YPAK_NEED, or < 0 */
YPAK_API int  ypak_feed(ypak_pack* p, const void* bytes, size_t n);
YPAK_API void ypak_close(ypak_pack* p);
YPAK_API const char* ypak_error(const ypak_pack* p);

/* Lookups: 0 or YPAK_ERR_NOT_FOUND (the message names a match that differs
 * only in case, if one exists). */
YPAK_API int ypak_find(ypak_pack* p, const char* name, ypak_entry* e);
YPAK_API int ypak_find_n(ypak_pack* p, const char* name, size_t len, ypak_entry* e);
YPAK_API int ypak_at(const ypak_pack* p, uint32_t index, ypak_entry* e);
YPAK_API int ypak_first_of(const ypak_pack* p, uint32_t kind, ypak_entry* e);

/* The entry's bytes, checked and verified by the policy; NULL on a failure
 * (ypak_error) or without a mapping or memory. Zero-copy: 4096-aligned in a
 * mapping, as aligned as the base in memory. */
YPAK_API const void* ypak_data(ypak_pack* p, const ypak_entry* e);
/* Checks bytes the caller fetched: exactly [header_off, data_off + size). */
YPAK_API int ypak_check(ypak_pack* p, const ypak_entry* e, const void* bytes, size_t n);
/* Verifies every entry now (a preflight), whatever the policy. */
YPAK_API int ypak_verify_all(ypak_pack* p);

typedef struct ypak_cursor {
    ypak_pack* p;
    uint32_t     index;
    int64_t      size;
    uint8_t*     scratch;           /* range reader: at least min(size, 1 MiB)     */
    size_t       cap;
    int64_t      chunk;             /* the chunk in scratch, or -1                 */
    int32_t      error;             /* the last failure's code                     */
} ypak_cursor;
YPAK_API int ypak_cursor_init(ypak_cursor* c, ypak_pack* p, const ypak_entry* e,
                                void* scratch, size_t cap);
/* yau_reader / yvid_reader compatible: ctx is the cursor. Returns n, or
 * less at the end, or < 0 on a failure. */
YPAK_API int64_t ypak_cursor_read(void* cursor, int64_t offset, void* buf, int64_t n);

/* "sha256:<64 hex>" and a NUL. */
YPAK_API void ypak_id(const ypak_pack* p, char out[72]);
/* One line for a log: format, pack ID, entries, policy. */
YPAK_API int  ypak_describe(const ypak_pack* p, char* buf, size_t cap);

/* The reader's name rule (docs/pack.md 2.7): 1 when the bytes are a valid name. */
YPAK_API int ypak_name_ok(const char* name, size_t len);

/* --- hashes -------------------------------------------------------------- */

typedef struct ypak_xxh64_state {
    uint64_t v[4], total;
    uint8_t  mem[32];
    uint32_t mem_n;
    uint64_t seed;
} ypak_xxh64_state;
YPAK_API uint64_t ypak_xxh64(const void* data, size_t n, uint64_t seed);
YPAK_API void     ypak_xxh64_init(ypak_xxh64_state* s, uint64_t seed);
YPAK_API void     ypak_xxh64_update(ypak_xxh64_state* s, const void* data, size_t n);
YPAK_API uint64_t ypak_xxh64_digest(const ypak_xxh64_state* s);
/* The entry hash: XXH64, seed = size, over the XXH64 (seed 0) of each 1 MiB
 * chunk as little-endian u64 (one empty chunk for an empty entry). chunks,
 * when not NULL, gets the chunk hashes (max(1, ceil(size / 2^20)) of them). */
YPAK_API uint64_t ypak_entry_hash(const void* data, int64_t size, uint64_t* chunks);
YPAK_API int64_t  ypak_chunk_count(int64_t size);

typedef struct ypak_sha256_state {
    uint32_t h[8];
    uint64_t total;
    uint8_t  buf[64];
    uint32_t n;
} ypak_sha256_state;
YPAK_API void ypak_sha256_init(ypak_sha256_state* s);
YPAK_API void ypak_sha256_update(ypak_sha256_state* s, const void* data, size_t n);
YPAK_API void ypak_sha256_final(ypak_sha256_state* s, uint8_t out[32]);
YPAK_API void ypak_sha256(const void* data, size_t n, uint8_t out[32]);

/* zip's CRC-32; crc 0 to start, the result of the previous piece to go on. */
YPAK_API uint32_t ypak_crc32(uint32_t crc, const void* data, size_t n);

/* --- views of the pack's own forms (docs/pack.md 4) ----------------------- */

typedef struct ypak_cset {
    const float*    texels;   uint32_t n_texels;   /* for ygfx_cset_desc        */
    const uint32_t* words;    uint32_t n_words;
    uint32_t        n_glyphs, units, flags, face;
} ypak_cset;
#define YPAK_CSET_UNITS_EM   0u
#define YPAK_CSET_UNITS_USER 1u
#define YPAK_CSET_ALL        0x1u
YPAK_API int ypak_cset_view(const void* data, int64_t size, ypak_cset* out, char* err, size_t cap);

typedef struct ypak_art_layer { uint32_t glyph, rgba; int32_t source, element; } ypak_art_layer;
typedef struct ypak_art {
    const ypak_art_layer* layers;  uint32_t n_layers;
    double                  viewbox[4];
    ypak_cset             set;
} ypak_art;
YPAK_API int ypak_art_view(const void* data, int64_t size, ypak_art* out, char* err, size_t cap);

typedef struct ypak_shader {
    uint32_t    mode, flags, params, textures, contract;
    uint64_t    wrap_hash;
    const char* body;  uint32_t body_len;     /* NUL-terminated                  */
    const char* name;  uint32_t name_len;
} ypak_shader;
#define YPAK_SHADER_TIME      0x1u
#define YPAK_SHADER_SEED      0x2u
#define YPAK_SHADER_VALIDATED 0x4u
YPAK_API int ypak_shader_view(const void* data, int64_t size, ypak_shader* out, char* err, size_t cap);

/* Texture formats: ygfx_format's values. R16F and RGBA16F are stored as f32. */
enum { YPAK_TEX_R8 = 1, YPAK_TEX_RG8, YPAK_TEX_RGBA8, YPAK_TEX_R16F, YPAK_TEX_RGBA16F,
       YPAK_TEX_R32F, YPAK_TEX_RGBA32F, YPAK_TEX_R16UI };
#define YPAK_TEX_RAW 0u
#define YPAK_TEX_QOI 1u
typedef struct ypak_texture {
    uint32_t       w, h, format, compression;
    uint8_t        enc[8];            /* ygfx_encoding's bytes                  */
    const uint8_t* data;  uint64_t stored;
    uint64_t       raw_bytes, raw_hash; /* tight rows, row 0 at the top         */
    uint32_t       texel_bytes;
} ypak_texture;
YPAK_API int ypak_texture_view(const void* data, int64_t size, ypak_texture* out, char* err, size_t cap);
/* A QOI texture into dst (raw_bytes), checked against raw_hash. */
YPAK_API int ypak_qoi_decode(const ypak_texture* t, void* dst, size_t cap, char* err, size_t errcap);
/* QOI-encodes RGBA8 rows (the tool's; deterministic). Returns the size, or
 * the size needed when cap is short (then nothing useful is written). */
YPAK_API size_t ypak_qoi_encode(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* out, size_t cap);

typedef struct ypak_runs_font { uint32_t cset_off, cset_len, font_off, font_len; } ypak_runs_font;
typedef struct ypak_runs_block_rec {
    uint32_t key_off, key_len, text_off, text_len, lang_off, lang_len;
    float    size, width, line_height, color;
    int32_t  dir, align, resolved_dir;
    uint32_t n_missing;
    float    w, h;
    uint32_t run_first, n_runs, item_first, n_items, line_first, n_lines;
    uint32_t reserved_[2];
} ypak_runs_block_rec;
typedef struct ypak_runs_run { uint32_t font; float size; uint32_t first, n; } ypak_runs_run;
typedef struct ypak_runs_line {
    float x, baseline, width, ascent, descent;
    uint32_t text_start, text_end, reserved_;
} ypak_runs_line;
typedef struct ypak_runs {
    const ypak_runs_font*      fonts;   uint32_t n_fonts;
    const ypak_runs_block_rec* blocks;  uint32_t n_blocks;
    const ypak_runs_run*       runs;    uint32_t n_runs;
    const float*                 items;   uint32_t n_items;   /* 8 floats: ygfx_citem */
    const uint32_t*              clusters;
    const ypak_runs_line*      lines;   uint32_t n_lines;
    const char*                  pool;    uint64_t pool_bytes;
} ypak_runs;
YPAK_API int ypak_runs_view(const void* data, int64_t size, ypak_runs* out, char* err, size_t cap);
/* The block of a key (blocks are sorted by key), or -1. */
YPAK_API int ypak_runs_find(const ypak_runs* r, const char* key);
/* A pool string by offset; the view proved it in bounds and NUL-ended. */
YPAK_API const char* ypak_runs_str(const ypak_runs* r, uint32_t off);

#ifdef __cplusplus
}
#endif

#endif /* YSP_PACK_H_INCLUDED */

/* ======================================================================= *
 *                            IMPLEMENTATION                               *
 * ======================================================================= */
#ifdef YSP_PACK_IMPLEMENTATION
#ifndef YSP_PACK_IMPLEMENTATION_GUARD
#define YSP_PACK_IMPLEMENTATION_GUARD

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifndef YPAK_MALLOC
#include <stdlib.h>
#define YPAK_MALLOC(n) malloc(n)
#define YPAK_FREE(p)   free(p)
#endif

#if !defined(YPAK_NO_FILE)
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#endif

/* Flags are bytes another thread may store at the same time (same value). */
#if defined(__GNUC__) || defined(__clang__)
#define YPAK__LOAD(p)   __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define YPAK__SET(p, v) __atomic_fetch_or((p), (uint8_t)(v), __ATOMIC_RELEASE)
#else
#define YPAK__LOAD(p)   (*(volatile uint8_t*)(p))
#define YPAK__SET(p, v) (*(volatile uint8_t*)(p) = (uint8_t)(*(volatile uint8_t*)(p) | (uint8_t)(v)))
#endif

#define YPAK__M32 0xFFFFFFFFu
#define YPAK__HDR_MAX (30 + YPAK_MAX_NAME + 20 + 6 + YPAK_ALIGN)

static const char* const ypak__kinds[YPAK_KIND_COUNT] = {
    NULL, "manifest", "chunks", "experiment", "file", "table", "calibration", "font", "curveset",
    "glyphruns", "artwork", "audio", "shader", "texture", "video", "video_index", "frameseq", "alpha"
};

YPAK_API const char* ypak_kind_name(uint32_t kind) {
    return kind > 0 && kind < (uint32_t)YPAK_KIND_COUNT ? ypak__kinds[kind] : NULL;
}

YPAK_API uint32_t ypak_kind_id(const char* name) {
    uint32_t k;
    if (!name) return 0;
    for (k = 1; k < (uint32_t)YPAK_KIND_COUNT; k++)
        if (strcmp(name, ypak__kinds[k]) == 0) return k;
    return 0;
}

static uint16_t ypak__rd16(const uint8_t* b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t ypak__rd32(const uint8_t* b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
/* MSVC does not fuse the byte loads into one load, which halved XXH64's rate
 * there (measured, docs/pack.md 8); every MSVC target is little-endian. */
#if defined(_MSC_VER)
static uint64_t ypak__rd64(const uint8_t* b) { uint64_t v; memcpy(&v, b, 8); return v; }
#else
static uint64_t ypak__rd64(const uint8_t* b) { return (uint64_t)ypak__rd32(b) | ((uint64_t)ypak__rd32(b + 4) << 32); }
#endif
static void ypak__wr64(uint8_t* b, uint64_t v) {
    int i;
    for (i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
}

static int ypak__fail(ypak_pack* p, int code, const char* fmt, ...) {
    va_list ap;
    int n;
    if (!p) return code;
    n = snprintf(p->error, sizeof(p->error), "ysp_pack: ");
    va_start(ap, fmt);
    vsnprintf(p->error + n, sizeof(p->error) - (size_t)n, fmt, ap);
    va_end(ap);
    return code;
}

YPAK_API const char* ypak_error(const ypak_pack* p) { return p ? p->error : ""; }

/* --- XXH64 (Yann Collet's algorithm, written from its specification) ------- */

#define YPAK__P1 0x9E3779B185EBCA87ULL
#define YPAK__P2 0xC2B2AE3D27D4EB4FULL
#define YPAK__P3 0x165667B19E3779F9ULL
#define YPAK__P4 0x85EBCA77C2B2AE63ULL
#define YPAK__P5 0x27D4EB2F165667C5ULL

static uint64_t ypak__rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t ypak__round(uint64_t acc, uint64_t in) {
    acc += in * YPAK__P2;
    acc = ypak__rotl(acc, 31);
    return acc * YPAK__P1;
}
static uint64_t ypak__merge(uint64_t acc, uint64_t v) {
    acc ^= ypak__round(0, v);
    return acc * YPAK__P1 + YPAK__P4;
}
static uint64_t ypak__xxh_tail(uint64_t h, const uint8_t* p, size_t n) {
    while (n >= 8) {
        h ^= ypak__round(0, ypak__rd64(p));
        h = ypak__rotl(h, 27) * YPAK__P1 + YPAK__P4;
        p += 8; n -= 8;
    }
    if (n >= 4) {
        h ^= (uint64_t)ypak__rd32(p) * YPAK__P1;
        h = ypak__rotl(h, 23) * YPAK__P2 + YPAK__P3;
        p += 4; n -= 4;
    }
    while (n--) {
        h ^= (*p++) * YPAK__P5;
        h = ypak__rotl(h, 11) * YPAK__P1;
    }
    h ^= h >> 33; h *= YPAK__P2;
    h ^= h >> 29; h *= YPAK__P3;
    h ^= h >> 32;
    return h;
}

YPAK_API void ypak_xxh64_init(ypak_xxh64_state* s, uint64_t seed) {
    memset(s, 0, sizeof(*s));
    s->seed = seed;
    s->v[0] = seed + YPAK__P1 + YPAK__P2;
    s->v[1] = seed + YPAK__P2;
    s->v[2] = seed;
    s->v[3] = seed - YPAK__P1;
}

YPAK_API void ypak_xxh64_update(ypak_xxh64_state* s, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    if (!n) return;
    s->total += n;
    if (s->mem_n + n < 32) {
        memcpy(s->mem + s->mem_n, p, n);
        s->mem_n += (uint32_t)n;
        return;
    }
    if (s->mem_n) {
        size_t k = 32 - s->mem_n;
        memcpy(s->mem + s->mem_n, p, k);
        s->v[0] = ypak__round(s->v[0], ypak__rd64(s->mem));
        s->v[1] = ypak__round(s->v[1], ypak__rd64(s->mem + 8));
        s->v[2] = ypak__round(s->v[2], ypak__rd64(s->mem + 16));
        s->v[3] = ypak__round(s->v[3], ypak__rd64(s->mem + 24));
        p += k; n -= k;
        s->mem_n = 0;
    }
    {
        uint64_t v0 = s->v[0], v1 = s->v[1], v2 = s->v[2], v3 = s->v[3];
        while (n >= 32) {
            v0 = ypak__round(v0, ypak__rd64(p));
            v1 = ypak__round(v1, ypak__rd64(p + 8));
            v2 = ypak__round(v2, ypak__rd64(p + 16));
            v3 = ypak__round(v3, ypak__rd64(p + 24));
            p += 32; n -= 32;
        }
        s->v[0] = v0; s->v[1] = v1; s->v[2] = v2; s->v[3] = v3;
    }
    if (n) {
        memcpy(s->mem, p, n);
        s->mem_n = (uint32_t)n;
    }
}

YPAK_API uint64_t ypak_xxh64_digest(const ypak_xxh64_state* s) {
    uint64_t h;
    if (s->total >= 32) {
        h = ypak__rotl(s->v[0], 1) + ypak__rotl(s->v[1], 7) + ypak__rotl(s->v[2], 12) + ypak__rotl(s->v[3], 18);
        h = ypak__merge(h, s->v[0]);
        h = ypak__merge(h, s->v[1]);
        h = ypak__merge(h, s->v[2]);
        h = ypak__merge(h, s->v[3]);
    } else {
        h = s->seed + YPAK__P5;
    }
    h += s->total;
    return ypak__xxh_tail(h, s->mem, s->mem_n);
}

YPAK_API uint64_t ypak_xxh64(const void* data, size_t n, uint64_t seed) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h;
    size_t len = n;
    if (n >= 32) {
        uint64_t v0 = seed + YPAK__P1 + YPAK__P2, v1 = seed + YPAK__P2, v2 = seed, v3 = seed - YPAK__P1;
        while (n >= 32) {
            v0 = ypak__round(v0, ypak__rd64(p));
            v1 = ypak__round(v1, ypak__rd64(p + 8));
            v2 = ypak__round(v2, ypak__rd64(p + 16));
            v3 = ypak__round(v3, ypak__rd64(p + 24));
            p += 32; n -= 32;
        }
        h = ypak__rotl(v0, 1) + ypak__rotl(v1, 7) + ypak__rotl(v2, 12) + ypak__rotl(v3, 18);
        h = ypak__merge(h, v0);
        h = ypak__merge(h, v1);
        h = ypak__merge(h, v2);
        h = ypak__merge(h, v3);
    } else {
        h = seed + YPAK__P5;
    }
    h += (uint64_t)len;
    return ypak__xxh_tail(h, p, n);
}

YPAK_API int64_t ypak_chunk_count(int64_t size) {
    return size <= 0 ? 1 : (size + YPAK_CHUNK - 1) >> YPAK_CHUNK_LOG2;
}

YPAK_API uint64_t ypak_entry_hash(const void* data, int64_t size, uint64_t* chunks) {
    const uint8_t* p = (const uint8_t*)data;
    int64_t k = ypak_chunk_count(size), j;
    ypak_xxh64_state s;
    ypak_xxh64_init(&s, (uint64_t)size);
    for (j = 0; j < k; j++) {
        int64_t off = j << YPAK_CHUNK_LOG2, n = size - off < YPAK_CHUNK ? size - off : YPAK_CHUNK;
        uint8_t le[8];
        uint64_t c = ypak_xxh64(p ? p + off : p, (size_t)(n > 0 ? n : 0), 0);
        if (chunks) chunks[j] = c;
        ypak__wr64(le, c);
        ypak_xxh64_update(&s, le, 8);
    }
    return ypak_xxh64_digest(&s);
}

/* --- SHA-256 (FIPS 180-4) ----------------------------------------------- */

static const uint32_t ypak__k256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};
#define YPAK__ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void ypak__sha_block(uint32_t h[8], const uint8_t* q) {
    uint32_t w[64], a, b, c, d, e, f, g, hh, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)q[4 * i] << 24) | ((uint32_t)q[4 * i + 1] << 16) | ((uint32_t)q[4 * i + 2] << 8) | q[4 * i + 3];
    for (i = 16; i < 64; i++)
        w[i] = w[i - 16] + (YPAK__ROR(w[i - 15], 7) ^ YPAK__ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
               (YPAK__ROR(w[i - 2], 17) ^ YPAK__ROR(w[i - 2], 19) ^ (w[i - 2] >> 10));
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 64; i++) {
        t1 = hh + (YPAK__ROR(e, 6) ^ YPAK__ROR(e, 11) ^ YPAK__ROR(e, 25)) + ((e & f) ^ (~e & g)) + ypak__k256[i] + w[i];
        t2 = (YPAK__ROR(a, 2) ^ YPAK__ROR(a, 13) ^ YPAK__ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

YPAK_API void ypak_sha256_init(ypak_sha256_state* s) {
    static const uint32_t h0[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    memcpy(s->h, h0, sizeof(h0));
    s->total = 0;
    s->n = 0;
}

YPAK_API void ypak_sha256_update(ypak_sha256_state* s, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    s->total += n;
    if (s->n) {
        size_t k = 64 - s->n < n ? 64 - s->n : n;
        memcpy(s->buf + s->n, p, k);
        s->n += (uint32_t)k; p += k; n -= k;
        if (s->n < 64) return;
        ypak__sha_block(s->h, s->buf);
        s->n = 0;
    }
    while (n >= 64) {
        ypak__sha_block(s->h, p);
        p += 64; n -= 64;
    }
    if (n) {
        memcpy(s->buf, p, n);
        s->n = (uint32_t)n;
    }
}

YPAK_API void ypak_sha256_final(ypak_sha256_state* s, uint8_t out[32]) {
    uint64_t bits = s->total * 8u;
    uint8_t pad[72];
    size_t padn = (s->n < 56) ? 56 - s->n : 120 - s->n;
    int i;
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) pad[padn + (size_t)i] = (uint8_t)(bits >> (56 - 8 * i));
    {
        uint64_t keep = s->total;
        ypak_sha256_update(s, pad, padn + 8);
        s->total = keep;
    }
    for (i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

YPAK_API void ypak_sha256(const void* data, size_t n, uint8_t out[32]) {
    ypak_sha256_state s;
    ypak_sha256_init(&s);
    ypak_sha256_update(&s, data, n);
    ypak_sha256_final(&s, out);
}

/* --- CRC-32 -------------------------------------------------------------- */

/* Slice-by-8 with the tables built per call on the stack: no static state to
 * race on, and 2048 entries cost about 10 us, nothing beside the bytes this
 * is used on (the tool and `ypak verify`; the player never calls it). */
YPAK_API uint32_t ypak_crc32(uint32_t crc, const void* data, size_t n) {
    uint32_t t[8][256];
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c = ~crc, i;
    int s, k;
    for (i = 0; i < 256; i++) {
        uint32_t v = i;
        for (k = 0; k < 8; k++) v = (v >> 1) ^ (0xEDB88320u & (0u - (v & 1u)));
        t[0][i] = v;
    }
    for (s = 1; s < 8; s++)
        for (i = 0; i < 256; i++) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    while (n >= 8) {
        uint32_t a = ypak__rd32(p) ^ c, b = ypak__rd32(p + 4);
        c = t[7][a & 0xFF] ^ t[6][(a >> 8) & 0xFF] ^ t[5][(a >> 16) & 0xFF] ^ t[4][a >> 24] ^
            t[3][b & 0xFF] ^ t[2][(b >> 8) & 0xFF] ^ t[1][(b >> 16) & 0xFF] ^ t[0][b >> 24];
        p += 8; n -= 8;
    }
    while (n--) c = t[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

/* --- names ----------------------------------------------------------------- */

YPAK_API int ypak_name_ok(const char* name, size_t len) {
    const uint8_t* s = (const uint8_t*)name;
    size_t i = 0, comp = 0;
    if (!name || len == 0 || len > YPAK_MAX_NAME || s[0] == '/' || s[len - 1] == '/') return 0;
    while (i < len) {
        uint32_t c = s[i];
        if (c == '/') {
            if (comp == 0) return 0;
            if ((comp == 1 && s[i - 1] == '.') || (comp == 2 && s[i - 1] == '.' && s[i - 2] == '.')) return 0;
            comp = 0; i++;
            continue;
        }
        if (c < 0x20 || c == 0x7F || c == '\\') return 0;
        if (c < 0x80) { i++; comp++; continue; }
        {
            int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 ? 1 : -1, k;
            uint32_t cp;
            if (n < 0 || c >= 0xF5 || i + (size_t)n >= len) return 0;
            cp = c & (0x3Fu >> n);
            for (k = 1; k <= n; k++) {
                uint32_t b = s[i + (size_t)k];
                if ((b & 0xC0) != 0x80) return 0;
                cp = (cp << 6) | (b & 0x3F);
            }
            if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF ||
                (cp >= 0xD800 && cp <= 0xDFFF) || (cp >= 0x80 && cp < 0xA0))
                return 0;
            i += (size_t)n + 1;
            comp += (size_t)n + 1;
        }
    }
    if ((comp == 1 && s[len - 1] == '.') || (comp == 2 && s[len - 1] == '.' && s[len - 2] == '.')) return 0;
    return 1;
}

/* Bytewise order, a shorter prefix first. */
static int ypak__cmp(const uint8_t* a, size_t na, const uint8_t* b, size_t nb) {
    int c = memcmp(a, b, na < nb ? na : nb);
    if (c) return c;
    return na < nb ? -1 : na > nb ? 1 : 0;
}

/* --- open ------------------------------------------------------------------ */

static int ypak__host_le(void) {
    const uint16_t one = 1;
    return *(const uint8_t*)&one == 1;
}

/* n bytes from desc.arena, or one YPAK_MALLOC block in slot; NULL with
 * pk.need set (arena) or on no memory. */
static void* ypak__take(ypak_pack* p, size_t n, int slot) {
    void* r;
    n = (n + 7u) & ~(size_t)7u;
    if (p->arena_) {
        if (p->arena_used_ + n > p->arena_size_) {
            p->need = p->arena_used_ + n;
            return NULL;
        }
        r = p->arena_ + p->arena_used_;
        p->arena_used_ += n;
        return r;
    }
    p->blk_[slot] = YPAK_MALLOC(n ? n : 8);
    return p->blk_[slot];
}

static int ypak__nomem(ypak_pack* p, const char* what) {
    if (p->arena_) return ypak__fail(p, YPAK_ERR_FULL, "desc.arena holds %zu bytes; %s needs pk.need = %zu", p->arena_size_, what, p->need);
    return ypak__fail(p, YPAK_ERR_NOMEM, "out of memory for %s", what);
}

/* The bytes of [off, off + n): from the base, a window the caller gave, or
 * the reader into buf. NULL when they are not there. */
typedef struct ypak__src {
    const uint8_t* win;
    int64_t        win_off, win_len;
} ypak__src;

static const uint8_t* ypak__get(ypak_pack* p, const ypak__src* w, int64_t off, int64_t n, uint8_t* buf) {
    if (off < 0 || n < 0 || off > p->file_size || n > p->file_size - off) return NULL;
    if (p->base_) return p->base_ + off;
    if (w && w->win && off >= w->win_off && off + n <= w->win_off + w->win_len) return w->win + (off - w->win_off);
    if (p->rd_ && buf) {
        int64_t got = n ? p->rd_->read(p->rdc_, off, buf, n) : 0;
        return got == n ? buf : NULL;
    }
    return NULL;
}

static int ypak__hex(const uint8_t* s, int n, uint8_t* out) {
    int i;
    for (i = 0; i < n; i++) {
        int v[2], k;
        for (k = 0; k < 2; k++) {
            uint8_t c = s[2 * i + k];
            if (c >= '0' && c <= '9') v[k] = c - '0';
            else if (c >= 'a' && c <= 'f') v[k] = c - 'a' + 10;
            else return 0;
        }
        out[i] = (uint8_t)(v[0] * 16 + v[1]);
    }
    return 1;
}

/* Expected data offset (from the zip start) for a header at hrel. */
static int64_t ypak__data_rel(int64_t hrel, uint32_t name_len, int z64) {
    int64_t end = hrel + 30 + (int64_t)name_len + (z64 ? 20 : 0) + 6;
    return (end + YPAK_ALIGN - 1) & ~(int64_t)(YPAK_ALIGN - 1);
}

static int ypak__walk(ypak_pack* p, int fill, uint32_t* n_chunks);
static int ypak__after_cd(ypak_pack* p);
static int ypak__verify_special(ypak_pack* p, uint32_t i, const ypak__src* w);

/* The end record, the zip64 records and the comment, from the window of the
 * file's last bytes. Sets cd_abs_, cd_size_, b_, n. */
static int ypak__tail(ypak_pack* p, const ypak__src* w) {
    uint8_t buf[512];
    int64_t look = p->file_size < 22 + 255 ? p->file_size : 22 + 255, e = -1, at;
    const uint8_t* t;
    uint8_t idh[32], cdh[8];
    int64_t i;
    uint32_t n16, nt16, cds, cdo, clen;
    t = ypak__get(p, w, p->file_size - look, look, buf);
    if (!t) return ypak__fail(p, YPAK_ERR_IO, "cannot read the last %lld bytes", (long long)look);
    for (i = look - 22; i >= 0; i--)
        if (ypak__rd32(t + i) == 0x06054B50u && (int64_t)ypak__rd16(t + i + 20) == look - i - 22) { e = i; break; }
    if (e < 0)
        return ypak__fail(p, YPAK_ERR_FORMAT, "no zip end record with a ysp-pack comment in the last %lld bytes; "
                          "not a ysp pack (build one with ypak)", (long long)look);
    at = p->file_size - look + e;
    clen = ypak__rd16(t + e + 20);
    {
        const uint8_t* c = t + e + 22;
        static const char pre[] = "ysp-pack/";
        uint32_t v = 0, k = 9;
        if (clen < 9 || memcmp(c, pre, 9) != 0)
            return ypak__fail(p, YPAK_ERR_FORMAT, "the zip comment is not a ysp-pack line; not a ysp pack (build one with ypak)");
        while (k < clen && c[k] >= '0' && c[k] <= '9' && v < 100000) v = v * 10 + (uint32_t)(c[k++] - '0');
        if (k == 9) return ypak__fail(p, YPAK_ERR_FORMAT, "the ysp-pack comment has no format version");
        p->format = (int32_t)v;
        if (v != YPAK_FORMAT)
            return ypak__fail(p, YPAK_ERR_VERSION, "pack format %u; this reader (ysp/pack.h %s) reads format %d; "
                              "update the player or rebuild the pack", v, YPAK_VERSION_STRING, YPAK_FORMAT);
        if (clen != 117 || memcmp(c + 10, " manifest=sha256:", 17) != 0 || memcmp(c + 91, " cd=xxh64:", 10) != 0 ||
            !ypak__hex(c + 27, 32, idh) || !ypak__hex(c + 101, 8, cdh))
            return ypak__fail(p, YPAK_ERR_FORMAT, "the ysp-pack comment is malformed (expected 'ysp-pack/1 "
                              "manifest=sha256:<64 hex> cd=xxh64:<16 hex>')");
        memcpy(p->id, idh, 32);
        p->cd_hash_ = 0;
        for (k = 0; k < 8; k++) p->cd_hash_ = (p->cd_hash_ << 8) | cdh[k];
    }
    if (ypak__rd16(t + e + 4) != 0 || ypak__rd16(t + e + 6) != 0)
        return ypak__fail(p, YPAK_ERR_FORMAT, "end record: disk numbers are not 0 (a split archive)");
    n16 = ypak__rd16(t + e + 8);
    nt16 = ypak__rd16(t + e + 10);
    cds = ypak__rd32(t + e + 12);
    cdo = ypak__rd32(t + e + 16);
    if (n16 != nt16) return ypak__fail(p, YPAK_ERR_FORMAT, "end record: entry counts differ");
    {
        uint64_t n64 = n16, size64 = cds, off64 = cdo;
        int z64 = 0;
        int64_t cd_end;
        /* zip64 is there exactly when a field holds its overflow marker
         * (the writer's rule), so no signature is searched for. */
        if (n16 == 0xFFFF || cdo == YPAK__M32) {
            uint8_t lb[76];
            const uint8_t* l;
            uint64_t rec;
            if (at < 76) return ypak__fail(p, YPAK_ERR_FORMAT, "end record: overflow markers but no room for zip64 records");
            l = ypak__get(p, w, at - 76, 76, lb);
            if (!l) return ypak__fail(p, YPAK_ERR_IO, "cannot read before the end record");
            if (ypak__rd32(l + 56) != 0x07064B50u || ypak__rd32(l + 60) != 0 || ypak__rd32(l + 72) != 1)
                return ypak__fail(p, YPAK_ERR_FORMAT, "zip64 locator: missing, or disk fields are not 0 and 1");
            rec = ypak__rd64(l + 64);
            if (ypak__rd32(l) != 0x06064B50u || ypak__rd64(l + 4) != 44 || ypak__rd16(l + 12) != 0x032D ||
                ypak__rd16(l + 14) != 45 || ypak__rd32(l + 16) != 0 || ypak__rd32(l + 20) != 0)
                return ypak__fail(p, YPAK_ERR_FORMAT, "zip64 end record: not at its place before the locator, or not "
                                  "in this format's form");
            n64 = ypak__rd64(l + 24);
            if (ypak__rd64(l + 32) != n64) return ypak__fail(p, YPAK_ERR_FORMAT, "zip64 end record: entry counts differ");
            size64 = ypak__rd64(l + 40);
            off64 = ypak__rd64(l + 48);
            if (n16 != (n64 >= 0xFFFF ? 0xFFFFu : (uint32_t)n64) ||
                cds != (size64 >= YPAK__M32 ? YPAK__M32 : (uint32_t)size64) ||
                cdo != (off64 >= YPAK__M32 ? YPAK__M32 : (uint32_t)off64))
                return ypak__fail(p, YPAK_ERR_FORMAT, "end record: its fields disagree with the zip64 end record");
            if (off64 > (uint64_t)INT64_MAX / 4 || rec > (uint64_t)INT64_MAX / 4)
                return ypak__fail(p, YPAK_ERR_FORMAT, "zip64 offsets out of range");
            z64 = 1;
            p->b_ = (at - 76) - (int64_t)rec;
        }
        if (n64 == 0 || n64 > YPAK_MAX_ENTRIES)
            return ypak__fail(p, YPAK_ERR_FORMAT, "%llu entries; a pack has 1 to %u", (unsigned long long)n64, YPAK_MAX_ENTRIES);
        if ((int64_t)size64 > YPAK_MAX_CD || size64 < 46u * n64)
            return ypak__fail(p, YPAK_ERR_FORMAT, "central directory size %llu is out of range", (unsigned long long)size64);
        cd_end = z64 ? at - 76 : at;
        if (off64 > (uint64_t)cd_end || (int64_t)off64 + (int64_t)size64 > cd_end)
            return ypak__fail(p, YPAK_ERR_FORMAT, "central directory offset %llu is outside the file", (unsigned long long)off64);
        if (!z64) p->b_ = cd_end - (int64_t)off64 - (int64_t)size64;
        else if (p->b_ + (int64_t)off64 + (int64_t)size64 != cd_end)
            return ypak__fail(p, YPAK_ERR_FORMAT, "zip64 end record: the central directory does not end where it starts");
        if (p->b_ < 0) return ypak__fail(p, YPAK_ERR_FORMAT, "offsets point past their records");
        p->n = (uint32_t)n64;
        p->cd_size_ = (int64_t)size64;
        p->cd_off_rec_ = (int64_t)off64;
        p->cd_abs_ = p->b_ + (int64_t)off64;
        p->base = p->b_;
    }
    return 0;
}

/* The central directory is in memory and hashed: walk it. fill 0 checks and
 * counts; fill 1 writes the index. */
static int ypak__walk(ypak_pack* p, int fill, uint32_t* n_chunks) {
    const uint8_t* c = p->cd_;
    int64_t at = 0, end = p->cd_size_, prev_end = 0, z = 0;
    uint32_t i, chunks = 0, n_exp = 0, n_chk = 0;
    const uint8_t* prev_name = NULL;
    uint32_t prev_len = 0;
    for (i = 0; i < p->n; i++) {
        uint32_t made, need, flags, method, tm, dt, crc, csz, usz, nl, el, cl, dsk, ia, ea, lho;
        const uint8_t* nm;
        const uint8_t* ex;
        int64_t size, hrec, hrel, data_rel, k;
        uint32_t kind = 0, eflags = 0, chunk = 0;
        uint64_t hash = 0, data_off = 0;
        int z64, got_ext = 0;
        if (end - at < 46 || ypak__rd32(c + at) != 0x02014B50u)
            return ypak__fail(p, YPAK_ERR_FORMAT, "central record %u: missing or truncated", i);
        made = ypak__rd16(c + at + 4); need = ypak__rd16(c + at + 6); flags = ypak__rd16(c + at + 8);
        method = ypak__rd16(c + at + 10); tm = ypak__rd16(c + at + 12); dt = ypak__rd16(c + at + 14);
        crc = ypak__rd32(c + at + 16); csz = ypak__rd32(c + at + 20); usz = ypak__rd32(c + at + 24);
        nl = ypak__rd16(c + at + 28); el = ypak__rd16(c + at + 30); cl = ypak__rd16(c + at + 32);
        dsk = ypak__rd16(c + at + 34); ia = ypak__rd16(c + at + 36); ea = ypak__rd32(c + at + 38);
        lho = ypak__rd32(c + at + 42);
        if (46 + (int64_t)nl + el + cl > end - at)
            return ypak__fail(p, YPAK_ERR_FORMAT, "central record %u: runs past the directory", i);
        nm = c + at + 46;
        ex = nm + nl;
        if (!ypak_name_ok((const char*)nm, nl))
            return ypak__fail(p, YPAK_ERR_FORMAT, "central record %u: the name is not a valid entry name", i);
        if (flags & 0x1u) return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': encrypted", (int)nl, (const char*)nm);
        if (flags & 0x8u) return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': has a data descriptor", (int)nl, (const char*)nm);
        if (method != 0)
            return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': compressed (method %u); a pack stores every entry "
                              "(the pack was changed by another zip tool? rebuild it with ypak)", (int)nl, (const char*)nm, method);
        if (flags != 0x0800u || tm != 0 || dt != 0x21 || cl != 0 || dsk != 0 || ia != 0 || ea != (0100644u << 16))
            return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': a fixed field differs from the format (flags, time, date, "
                              "comment, disk or attributes)", (int)nl, (const char*)nm);
        if (csz != usz) return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': sizes differ", (int)nl, (const char*)nm);
        size = usz;
        hrec = lho;
        z64 = (usz == YPAK__M32 || lho == YPAK__M32);
        /* extra fields: [zip64] then the private record, nothing else */
        k = 0;
        if (z64) {
            uint32_t want = (usz == YPAK__M32 ? 16u : 0u) + (lho == YPAK__M32 ? 8u : 0u);
            const uint8_t* zf;
            if (el < 4 + want || ypak__rd16(ex) != 1 || ypak__rd16(ex + 2) != want)
                return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': the zip64 field is missing or has the wrong size", (int)nl, (const char*)nm);
            zf = ex + 4;
            if (usz == YPAK__M32) {
                uint64_t a = ypak__rd64(zf), b = ypak__rd64(zf + 8);
                if (a != b || a < YPAK__M32 || a > (uint64_t)INT64_MAX / 2)
                    return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': zip64 sizes are wrong", (int)nl, (const char*)nm);
                size = (int64_t)a;
                zf += 16;
            }
            if (lho == YPAK__M32) {
                uint64_t a = ypak__rd64(zf);
                if (a < YPAK__M32 || a > (uint64_t)INT64_MAX / 2)
                    return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': zip64 offset is wrong", (int)nl, (const char*)nm);
                hrec = (int64_t)a;
            }
            k = 4 + want;
        }
        if (need != (z64 ? 45u : 20u) || made != (z64 ? 0x032Du : 0x0314u))
            return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': version fields are not this format's", (int)nl, (const char*)nm);
        if (el - k == 4 + YPAK_EXT_SIZE && ypak__rd16(ex + k) == YPAK_EXT_ID && ypak__rd16(ex + k + 2) == YPAK_EXT_SIZE) {
            const uint8_t* r = ex + k + 4;
            if (r[0] != 1 || r[1] != 0)
                return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': private record version %u; this reader knows 1", (int)nl, (const char*)nm, r[0]);
            kind = ypak__rd16(r + 2);
            eflags = ypak__rd32(r + 4);
            data_off = ypak__rd64(r + 8);
            hash = ypak__rd64(r + 16);
            chunk = ypak__rd32(r + 24);
            got_ext = 1;
        }
        if (!got_ext)
            return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': the private ysp record (0x7379) is missing or has extra fields "
                              "beside it (the pack was changed by another zip tool? rebuild it with ypak)", (int)nl, (const char*)nm);
        if (kind == 0 || kind >= (uint32_t)YPAK_KIND_COUNT)
            return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': kind %u is unknown to ysp/pack.h %s", (int)nl, (const char*)nm, kind, YPAK_VERSION_STRING);
        if (eflags & ~YPAK_F_STREAM)
            return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': unknown flags 0x%x", (int)nl, (const char*)nm, eflags);
        /* names, kinds and order */
        if (i == 0) {
            if (nl != sizeof(YPAK_MANIFEST) - 1 || memcmp(nm, YPAK_MANIFEST, nl) != 0 || kind != YPAK_KIND_MANIFEST)
                return ypak__fail(p, YPAK_ERR_FORMAT, "the first entry is not ysp/manifest.json of kind manifest");
        } else {
            if (kind == YPAK_KIND_MANIFEST) return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': a second manifest", (int)nl, (const char*)nm);
            if (prev_name && ypak__cmp(prev_name, prev_len, nm, nl) >= 0)
                return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': out of name order or a duplicate", (int)nl, (const char*)nm);
            if (nl >= 4 && memcmp(nm, "ysp/", 4) == 0) {
                if (nl != sizeof(YPAK_CHUNKS) - 1 || memcmp(nm, YPAK_CHUNKS, nl) != 0 || kind != YPAK_KIND_CHUNKS)
                    return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': names under ysp/ are reserved", (int)nl, (const char*)nm);
            } else if (kind == YPAK_KIND_CHUNKS) {
                return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': kind chunks outside ysp/chunks", (int)nl, (const char*)nm);
            }
            prev_name = nm;
            prev_len = nl;
        }
        if (kind == YPAK_KIND_CHUNKS) { n_chk++; if (fill) p->i_chunks_ = i; }
        if (kind == YPAK_KIND_EXPERIMENT) { n_exp++; if (fill) p->experiment = (int32_t)i; }
        /* offsets: contiguous, aligned, canonical */
        if (hrec > INT64_MAX / 4 || p->b_ > INT64_MAX / 4) return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': offset out of range", (int)nl, (const char*)nm);
        if (i == 0) {
            z = p->b_ + hrec;
            if (z % YPAK_ALIGN) return ypak__fail(p, YPAK_ERR_FORMAT, "the zip start %lld is not a multiple of 4096", (long long)z);
            if (fill) p->zip_start = z;
        }
        hrel = p->b_ + hrec - z;
        if (hrel != prev_end)
            return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': its header is not right after the previous entry (a gap or an "
                              "overlap)", (int)nl, (const char*)nm);
        data_rel = ypak__data_rel(hrel, nl, size >= (int64_t)YPAK__M32);
        if (data_off != (uint64_t)data_rel)
            return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': data offset %llu, expected %lld", (int)nl, (const char*)nm,
                              (unsigned long long)data_off, (long long)data_rel);
        if (size > INT64_MAX / 4 - data_rel) return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': size out of range", (int)nl, (const char*)nm);
        prev_end = data_rel + size;
        /* chunks */
        if (kind == YPAK_KIND_CHUNKS || kind == YPAK_KIND_MANIFEST || ypak_chunk_count(size) == 1) {
            if (chunk != YPAK_NO_CHUNK)
                return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': a chunk index on an entry with no chunk list", (int)nl, (const char*)nm);
        } else {
            int64_t kc = ypak_chunk_count(size);
            if (chunk != chunks || kc > (int64_t)(YPAK__M32 - 1) - chunks)
                return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': chunk index %u, expected %u", (int)nl, (const char*)nm, chunk, chunks);
            chunks += (uint32_t)kc;
        }
        if (fill) {
            ypak__ent* t = &p->ent_[i];
            t->hdr = z + hrel;
            t->data = z + data_rel;
            t->size = size;
            t->hash = hash;
            t->name = (uint32_t)(nm - c);
            t->name_len = nl;
            t->kind = kind;
            t->flags = eflags;
            t->crc = crc;
            t->chunk = chunk;
        }
        at += 46 + (int64_t)nl + el;
    }
    if (at != end) return ypak__fail(p, YPAK_ERR_FORMAT, "central directory: %lld bytes after the last record", (long long)(end - at));
    if (n_chk != 1) return ypak__fail(p, YPAK_ERR_FORMAT, "no ysp/chunks entry");
    if (n_exp > 1) return ypak__fail(p, YPAK_ERR_FORMAT, "%u experiment entries; at most one", n_exp);
    if (p->cd_abs_ - z != prev_end)
        return ypak__fail(p, YPAK_ERR_FORMAT, "the central directory is not right after the last entry");
    *n_chunks = chunks;
    return 0;
}

/* The local header of entry i, from bytes h (its hdr_len bytes). */
static int ypak__check_header(ypak_pack* p, uint32_t i, const uint8_t* h) {
    const ypak__ent* t = &p->ent_[i];
    const uint8_t* nm = p->cd_ + t->name;
    int64_t hl = t->data - t->hdr;
    int z64 = t->size >= (int64_t)YPAK__M32;
    uint32_t nl, el, pad, k;
    if (ypak__rd32(h) != 0x04034B50u || ypak__rd16(h + 4) != (z64 ? 45u : 20u) || ypak__rd16(h + 6) != 0x0800u ||
        ypak__rd16(h + 8) != 0 || ypak__rd16(h + 10) != 0 || ypak__rd16(h + 12) != 0x21 || ypak__rd32(h + 14) != t->crc)
        return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': its local header differs from its central record", (int)t->name_len, (const char*)nm);
    if (ypak__rd32(h + 18) != (z64 ? YPAK__M32 : (uint32_t)t->size) || ypak__rd32(h + 22) != (z64 ? YPAK__M32 : (uint32_t)t->size))
        return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': local header sizes differ", (int)t->name_len, (const char*)nm);
    nl = ypak__rd16(h + 26);
    el = ypak__rd16(h + 28);
    if (nl != t->name_len || 30 + (int64_t)nl + el != hl || memcmp(h + 30, nm, nl) != 0)
        return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': local header name or length differs", (int)t->name_len, (const char*)nm);
    h += 30 + nl;
    if (z64) {
        if (el < 20 || ypak__rd16(h) != 1 || ypak__rd16(h + 2) != 16 || ypak__rd64(h + 4) != (uint64_t)t->size ||
            ypak__rd64(h + 12) != (uint64_t)t->size)
            return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': local zip64 field is wrong", (int)t->name_len, (const char*)nm);
        h += 20;
        el -= 20;
    }
    if (el < 6 || ypak__rd16(h) != 0xD935u || ypak__rd16(h + 2) != el - 4 || ypak__rd16(h + 4) != YPAK_ALIGN)
        return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': the alignment field is wrong", (int)t->name_len, (const char*)nm);
    pad = el - 6;
    for (k = 0; k < pad; k++)
        if (h[6 + k]) return ypak__fail(p, YPAK_ERR_FORMAT, "'%.*s': padding is not zeros", (int)t->name_len, (const char*)nm);
    return 0;
}

/* Hashes entry i's data from a getter over the whole data range. chunk
 * hashes are checked against the table for multi-chunk entries; the entry
 * hash against the record. sha (may be NULL) gets the SHA-256. j0, j1: the
 * chunk range to check (whole entry: 0, k). */
static int ypak__hash(ypak_pack* p, uint32_t i, const ypak__src* w, const uint8_t* direct, uint8_t* sha,
                       int64_t j0, int64_t j1) {
    const ypak__ent* t = &p->ent_[i];
    int64_t k = ypak_chunk_count(t->size), j;
    ypak_xxh64_state lst;
    ypak_sha256_state ss;
    uint8_t buf[16384];
    int whole = (j0 == 0 && j1 == k);
    ypak_xxh64_init(&lst, (uint64_t)t->size);
    if (sha) ypak_sha256_init(&ss);
    for (j = j0; j < j1; j++) {
        int64_t off = j << YPAK_CHUNK_LOG2, n = t->size - off < YPAK_CHUNK ? t->size - off : YPAK_CHUNK, o;
        uint64_t c;
        uint8_t le[8];
        if (n < 0) n = 0;
        if (direct) {
            c = ypak_xxh64(direct + off, (size_t)n, 0);
            if (sha) ypak_sha256_update(&ss, direct + off, (size_t)n);
        } else {
            ypak_xxh64_state cs;
            ypak_xxh64_init(&cs, 0);
            for (o = 0; o < n; o += (int64_t)sizeof(buf)) {
                int64_t m = n - o < (int64_t)sizeof(buf) ? n - o : (int64_t)sizeof(buf);
                const uint8_t* b = ypak__get(p, w, t->data + off + o, m, buf);
                if (!b) return ypak__fail(p, YPAK_ERR_IO, "'%.*s': cannot read its data", (int)t->name_len, (const char*)(p->cd_ + t->name));
                ypak_xxh64_update(&cs, b, (size_t)m);
                if (sha) ypak_sha256_update(&ss, b, (size_t)m);
            }
            c = ypak_xxh64_digest(&cs);
        }
        p->hashed_bytes += (uint64_t)n;
        if (t->chunk != YPAK_NO_CHUNK && p->chunks_) {
            if (ypak__rd64(p->chunks_ + 8 * ((int64_t)t->chunk + j)) != c)
                return ypak__fail(p, YPAK_ERR_CORRUPT, "'%.*s': chunk %lld does not match its hash (corrupt)",
                                  (int)t->name_len, (const char*)(p->cd_ + t->name), (long long)j);
            if (p->cok_) YPAK__SET(&p->cok_[t->chunk + (uint32_t)j], 1);
        }
        ypak__wr64(le, c);
        ypak_xxh64_update(&lst, le, 8);
    }
    if (whole && ypak_xxh64_digest(&lst) != t->hash)
        return ypak__fail(p, YPAK_ERR_CORRUPT, "'%.*s': does not match its entry hash (corrupt)", (int)t->name_len,
                          (const char*)(p->cd_ + t->name));
    if (sha) ypak_sha256_final(&ss, sha);
    if (whole && p->ok_) YPAK__SET(&p->ok_[i], 2);
    return 0;
}

static int ypak__header_from(ypak_pack* p, uint32_t i, const ypak__src* w) {
    uint8_t hb[YPAK__HDR_MAX];
    const ypak__ent* t = &p->ent_[i];
    const uint8_t* h;
    int rc;
    if (YPAK__LOAD(&p->ok_[i]) & 1) return 0;
    h = ypak__get(p, w, t->hdr, t->data - t->hdr, hb);
    if (!h) return ypak__fail(p, YPAK_ERR_IO, "'%.*s': cannot read its local header", (int)t->name_len, (const char*)(p->cd_ + t->name));
    rc = ypak__check_header(p, i, h);
    if (rc == 0) YPAK__SET(&p->ok_[i], 1);
    return rc;
}

/* The manifest (SHA-256 against the pack ID) or the chunk table. */
static int ypak__verify_special(ypak_pack* p, uint32_t i, const ypak__src* w) {
    const ypak__ent* t = &p->ent_[i];
    const uint8_t* direct = p->base_ ? p->base_ + t->data : NULL;
    uint8_t sha[32];
    int rc = ypak__header_from(p, i, w);
    if (rc) return rc;
    if (!direct && w && w->win && t->data >= w->win_off && t->data + t->size <= w->win_off + w->win_len)
        direct = w->win + (t->data - w->win_off);
    if (i == 0) {
        rc = ypak__hash(p, i, w, direct, sha, 0, ypak_chunk_count(t->size));
        if (rc) return rc;
        if (memcmp(sha, p->id, 32) != 0)
            return ypak__fail(p, YPAK_ERR_CORRUPT, "the manifest's SHA-256 is not the pack ID in the comment (corrupt)");
        return 0;
    }
    /* the chunk table: hashed whole, then copied if there is no base */
    rc = ypak__hash(p, i, w, direct, NULL, 0, ypak_chunk_count(t->size));
    if (rc) return rc;
    {
        uint8_t hb[32];
        const uint8_t* h = ypak__get(p, w, t->data, 32, hb);
        uint64_t n;
        if (t->size < 32 || !h) return ypak__fail(p, YPAK_ERR_FORMAT, "ysp/chunks: too short");
        n = ypak__rd64(h + 16);
        if (memcmp(h, "YSPCHNK1", 8) != 0 || ypak__rd32(h + 8) != 1 || ypak__rd32(h + 12) != YPAK_CHUNK_LOG2 ||
            ypak__rd64(h + 24) != 0 || n != p->n_chunks || (uint64_t)t->size != 32u + 8u * n)
            return ypak__fail(p, YPAK_ERR_FORMAT, "ysp/chunks: its header is wrong or it holds %llu hashes for %u chunks",
                              (unsigned long long)n, p->n_chunks);
        if (p->base_) {
            p->chunks_ = p->base_ + t->data + 32;
        } else {
            uint8_t* dst = (uint8_t*)ypak__take(p, (size_t)(8u * n), 2);
            const uint8_t* src;
            if (!dst) return ypak__nomem(p, "the chunk table");
            if (n) {
                src = ypak__get(p, w, t->data + 32, (int64_t)(8u * n), dst);
                if (!src) return ypak__fail(p, YPAK_ERR_IO, "cannot read the chunk table");
                if (src != dst) memcpy(dst, src, (size_t)(8u * n));
            }
            p->chunks_ = dst;
        }
    }
    /* every multi-chunk entry's list against its entry hash */
    {
        uint32_t e;
        for (e = 0; e < p->n; e++) {
            const ypak__ent* q = &p->ent_[e];
            if (q->chunk != YPAK_NO_CHUNK) {
                int64_t kc = ypak_chunk_count(q->size);
                if (ypak_xxh64(p->chunks_ + 8 * (int64_t)q->chunk, (size_t)(8 * kc), (uint64_t)q->size) != q->hash)
                    return ypak__fail(p, YPAK_ERR_CORRUPT, "'%.*s': its chunk list does not match its entry hash (corrupt)",
                                      (int)q->name_len, (const char*)(p->cd_ + q->name));
            }
        }
    }
    return 0;
}

/* The directory is in p->cd_ and hashed: walk, take the index, then go on
 * to the manifest. */
static int ypak__after_cd(ypak_pack* p) {
    uint32_t n_chunks = 0;
    size_t need;
    int rc = ypak__walk(p, 0, &n_chunks);
    if (rc) return rc;
    p->n_chunks = n_chunks;
    need = (size_t)p->n * sizeof(ypak__ent) + p->n + n_chunks + 32u;
    p->ent_ = (ypak__ent*)ypak__take(p, need, 1);
    if (!p->ent_) return ypak__nomem(p, "the index");
    p->ok_ = (uint8_t*)(p->ent_ + p->n);
    p->cok_ = p->ok_ + p->n;
    memset(p->ok_, 0, p->n);
    memset(p->cok_, 0, n_chunks);
    p->experiment = -1;
    rc = ypak__walk(p, 1, &n_chunks);
    if (rc) return rc;
    return 0;
}

static int ypak__need(ypak_pack* p, uint32_t i, int state) {
    const ypak__ent* t = &p->ent_[i];
    p->state_ = state;
    p->need_off = t->hdr;
    p->need_len = t->data + t->size - t->hdr;
    return YPAK_NEED;
}

static int ypak__finish(ypak_pack* p) {
    uint32_t i;
    p->open_ = 1;
    p->state_ = 0;
    p->need_off = p->need_len = 0;
    if (p->verify_ == YPAK_VERIFY_OPEN && (p->base_ || p->rd_))
        for (i = 0; i < p->n; i++) {
            ypak__src w;
            int rc;
            memset(&w, 0, sizeof(w));
            rc = ypak__header_from(p, i, &w);
            if (rc == 0 && !(YPAK__LOAD(&p->ok_[i]) & 2))
                rc = ypak__hash(p, i, &w, p->base_ ? p->base_ + p->ent_[i].data : NULL, NULL, 0, ypak_chunk_count(p->ent_[i].size));
            if (rc) return rc;
        }
    return 0;
}

static void ypak__release(ypak_pack* p) {
    int k;
    for (k = 0; k < 3; k++) {
        if (p->blk_[k]) YPAK_FREE(p->blk_[k]);
        p->blk_[k] = NULL;
    }
#if !defined(YPAK_NO_FILE)
    if (p->map_) {
#if defined(_WIN32)
        UnmapViewOfFile(p->map_);
        if (p->mh_) CloseHandle((HANDLE)p->mh_);
        if (p->fh_) CloseHandle((HANDLE)p->fh_);
#else
        munmap(p->map_, (size_t)p->map_size_);
#endif
        p->map_ = NULL;
    }
#if defined(_WIN32)
    else {
        if (p->mh_) CloseHandle((HANDLE)p->mh_);
        if (p->fh_) CloseHandle((HANDLE)p->fh_);
    }
#endif
    p->fh_ = p->mh_ = 0;
#endif
}

YPAK_API void ypak_close(ypak_pack* p) {
    if (!p) return;
    ypak__release(p);
    p->open_ = 0;
    p->n = 0;
    p->base_ = NULL;
    p->cd_ = NULL;
    p->ent_ = NULL;
}

#if !defined(YPAK_NO_FILE)
static int ypak__map(ypak_pack* p, const char* path) {
#if defined(_WIN32)
    wchar_t wp[1024];
    HANDLE f, m;
    LARGE_INTEGER sz;
    void* v;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wp, 1024))
        return ypak__fail(p, YPAK_ERR_ARG, "the path is not valid UTF-8 or longer than 1023 characters");
    f = CreateFileW(wp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return ypak__fail(p, YPAK_ERR_IO, "cannot open '%s' (error %lu)", path, (unsigned long)GetLastError());
    p->fh_ = (intptr_t)f;
    if (!GetFileSizeEx(f, &sz)) return ypak__fail(p, YPAK_ERR_IO, "cannot get the size of '%s'", path);
    if (sz.QuadPart < 22) return ypak__fail(p, YPAK_ERR_FORMAT, "'%s' is %lld bytes; too short for a pack", path, (long long)sz.QuadPart);
    m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!m) return ypak__fail(p, YPAK_ERR_IO, "cannot map '%s' (error %lu)", path, (unsigned long)GetLastError());
    p->mh_ = (intptr_t)m;
    v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (!v) return ypak__fail(p, YPAK_ERR_IO, "cannot map a view of '%s' (error %lu)", path, (unsigned long)GetLastError());
    p->map_ = v;
    p->map_size_ = sz.QuadPart;
#else
    struct stat st;
    void* v;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return ypak__fail(p, YPAK_ERR_IO, "cannot open '%s'", path);
    if (fstat(fd, &st) != 0 || st.st_size < 22) {
        close(fd);
        return ypak__fail(p, YPAK_ERR_FORMAT, "'%s' is too short for a pack", path);
    }
    v = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (v == MAP_FAILED) return ypak__fail(p, YPAK_ERR_IO, "cannot map '%s'", path);
    p->map_ = v;
    p->map_size_ = (int64_t)st.st_size;
#endif
    p->base_ = (const uint8_t*)p->map_;
    p->file_size = p->map_size_;
    return 0;
}
#endif

/* Runs the open's steps as far as the bytes allow. */
static int ypak__run(ypak_pack* p, const ypak__src* w) {
    int rc;
    for (;;) {
        if (p->state_ == 1) {   /* the central directory */
            const uint8_t* cd;
            if (p->base_) {
                cd = p->base_ + p->cd_abs_;
            } else {
                uint8_t* dst;
                if (!p->rd_ && !(w && w->win && p->cd_abs_ >= w->win_off && p->cd_abs_ + p->cd_size_ <= w->win_off + w->win_len)) {
                    p->need_off = p->cd_abs_;
                    p->need_len = p->cd_size_;
                    return YPAK_NEED;
                }
                dst = (uint8_t*)ypak__take(p, (size_t)p->cd_size_, 0);
                if (!dst) return ypak__nomem(p, "the central directory");
                cd = ypak__get(p, w, p->cd_abs_, p->cd_size_, dst);
                if (!cd) return ypak__fail(p, YPAK_ERR_IO, "cannot read the central directory");
                if (cd != dst) memcpy(dst, cd, (size_t)p->cd_size_);
                cd = dst;
            }
            if (ypak_xxh64(cd, (size_t)p->cd_size_, 0) != p->cd_hash_)
                return ypak__fail(p, YPAK_ERR_CORRUPT, "the central directory does not match its hash; was the pack "
                                  "changed by another zip tool? Rebuild it with ypak");
            p->cd_ = cd;
            rc = ypak__after_cd(p);
            if (rc) return rc;
            p->state_ = 2;
        }
        if (p->state_ == 2) {   /* the manifest */
            if (!p->base_ && !p->rd_ && !(w && w->win && w->win_off <= p->ent_[0].hdr &&
                                          w->win_off + w->win_len >= p->ent_[0].data + p->ent_[0].size))
                return ypak__need(p, 0, 2);
            rc = ypak__verify_special(p, 0, w);
            if (rc) return rc;
            p->state_ = 3;
        }
        if (p->state_ == 3) {   /* the chunk table */
            uint32_t i = p->i_chunks_;
            if (!p->base_ && !p->rd_ && !(w && w->win && w->win_off <= p->ent_[i].hdr &&
                                          w->win_off + w->win_len >= p->ent_[i].data + p->ent_[i].size))
                return ypak__need(p, i, 3);
            rc = ypak__verify_special(p, i, w);
            if (rc) return rc;
            return ypak__finish(p);
        }
        return ypak__fail(p, YPAK_ERR_ARG, "the pack is not opening");
    }
}

YPAK_API int ypak_open(ypak_pack* p, const ypak_desc* d) {
    ypak__src w;
    int rc;
    if (!p) return YPAK_ERR_ARG;
    memset(p, 0, sizeof(*p));
    p->experiment = -1;
    if (!d) return ypak__fail(p, YPAK_ERR_ARG, "desc is NULL");
    if (!ypak__host_le()) return ypak__fail(p, YPAK_ERR_ARG, "a big-endian host; the pack format is little-endian");
    if ((unsigned)d->verify > (unsigned)YPAK_VERIFY_NONE) return ypak__fail(p, YPAK_ERR_ARG, "desc.verify is not a policy");
    p->verify_ = (int32_t)d->verify;
    if (d->arena) {
        if (((uintptr_t)d->arena & 7u) != 0) return ypak__fail(p, YPAK_ERR_ARG, "desc.arena is not 8-byte aligned");
        p->arena_ = (uint8_t*)d->arena;
        p->arena_size_ = d->arena_size;
    }
    memset(&w, 0, sizeof(w));
    if (d->path) {
#if defined(YPAK_NO_FILE)
        return ypak__fail(p, YPAK_ERR_ARG, "built with YPAK_NO_FILE: no path source");
#else
        rc = ypak__map(p, d->path);
        if (rc) { ypak__release(p); return rc; }
#endif
    } else if (d->data) {
        if (((uintptr_t)d->data & 7u) != 0) return ypak__fail(p, YPAK_ERR_ARG, "desc.data is not 8-byte aligned");
        p->base_ = (const uint8_t*)d->data;
        p->file_size = (int64_t)d->size;
    } else if (d->reader) {
        if (!d->reader->read || d->reader->size < 0) return ypak__fail(p, YPAK_ERR_ARG, "desc.reader has no read or size");
        p->rd_ = d->reader;
        p->rdc_ = d->reader_ctx;
        p->file_size = d->reader->size;
    } else if (d->tail) {
        if (d->file_size < (int64_t)d->tail_size) return ypak__fail(p, YPAK_ERR_ARG, "desc.tail is longer than desc.file_size");
        p->file_size = d->file_size;
        w.win = (const uint8_t*)d->tail;
        w.win_off = d->file_size - (int64_t)d->tail_size;
        w.win_len = (int64_t)d->tail_size;
    } else {
        return ypak__fail(p, YPAK_ERR_ARG, "desc names no source (path, data, reader or tail)");
    }
    if (p->file_size < 22) { ypak__release(p); return ypak__fail(p, YPAK_ERR_FORMAT, "%lld bytes; too short for a pack", (long long)p->file_size); }
    rc = ypak__tail(p, &w);
    if (rc == 0) {
        p->state_ = 1;
        rc = ypak__run(p, &w);
    }
    if (rc < 0) ypak__release(p);
    return rc;
}

YPAK_API int ypak_feed(ypak_pack* p, const void* bytes, size_t n) {
    ypak__src w;
    int rc;
    if (!p || p->open_ || p->state_ == 0) return p ? ypak__fail(p, YPAK_ERR_ARG, "ypak_feed: nothing was asked for") : YPAK_ERR_ARG;
    if (!bytes || (int64_t)n != p->need_len) return ypak__fail(p, YPAK_ERR_ARG, "ypak_feed: give exactly the %lld bytes asked for", (long long)p->need_len);
    w.win = (const uint8_t*)bytes;
    w.win_off = p->need_off;
    w.win_len = (int64_t)n;
    rc = ypak__run(p, &w);
    if (rc < 0) ypak__release(p);
    return rc;
}

/* --- entries ------------------------------------------------------------- */

static void ypak__entry(const ypak_pack* p, uint32_t i, ypak_entry* e) {
    const ypak__ent* t = &p->ent_[i];
    e->name = (const char*)(p->cd_ + t->name);
    e->name_len = t->name_len;
    e->index = i;
    e->kind = t->kind;
    e->flags = t->flags;
    e->size = t->size;
    e->header_off = t->hdr;
    e->data_off = t->data;
    e->hash = t->hash;
    e->crc32 = t->crc;
    e->chunk_first = t->chunk;
    e->data = p->base_ ? p->base_ + t->data : NULL;
}

YPAK_API int ypak_at(const ypak_pack* p, uint32_t index, ypak_entry* e) {
    if (!p || !e || !p->open_ || index >= p->n) return YPAK_ERR_ARG;
    ypak__entry(p, index, e);
    return 0;
}

YPAK_API int ypak_find_n(ypak_pack* p, const char* name, size_t len, ypak_entry* e) {
    uint32_t lo, hi;
    if (!p || !name || !e || !p->open_) return YPAK_ERR_ARG;
    if (len == sizeof(YPAK_MANIFEST) - 1 && memcmp(name, YPAK_MANIFEST, len) == 0) {
        ypak__entry(p, 0, e);
        return 0;
    }
    lo = 1;
    hi = p->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const ypak__ent* t = &p->ent_[mid];
        int c = ypak__cmp(p->cd_ + t->name, t->name_len, (const uint8_t*)name, len);
        if (c == 0) {
            ypak__entry(p, mid, e);
            return 0;
        }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    {
        uint32_t i;
        for (i = 0; i < p->n; i++) {
            const ypak__ent* t = &p->ent_[i];
            size_t k;
            if (t->name_len != len) continue;
            for (k = 0; k < len; k++) {
                uint8_t a = p->cd_[t->name + k], b = (uint8_t)name[k];
                if (a >= 'A' && a <= 'Z') a = (uint8_t)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (uint8_t)(b + 32);
                if (a != b) break;
            }
            if (k == len)
                return ypak__fail(p, YPAK_ERR_NOT_FOUND, "no entry '%.*s'; names are case-sensitive and '%.*s' exists",
                                  (int)len, name, (int)len, (const char*)(p->cd_ + t->name));
        }
    }
    return ypak__fail(p, YPAK_ERR_NOT_FOUND, "no entry '%.*s'", (int)(len > 200 ? 200 : len), name);
}

YPAK_API int ypak_find(ypak_pack* p, const char* name, ypak_entry* e) {
    return ypak_find_n(p, name, name ? strlen(name) : 0, e);
}

YPAK_API int ypak_first_of(const ypak_pack* p, uint32_t kind, ypak_entry* e) {
    uint32_t i;
    if (!p || !e || !p->open_) return YPAK_ERR_ARG;
    for (i = 0; i < p->n; i++)
        if (p->ent_[i].kind == kind) {
            ypak__entry(p, i, e);
            return 0;
        }
    return YPAK_ERR_NOT_FOUND;
}

static int ypak__valid_entry(ypak_pack* p, const ypak_entry* e) {
    if (!p || !e || !p->open_ || e->index >= p->n) return 0;
    return 1;
}

YPAK_API const void* ypak_data(ypak_pack* p, const ypak_entry* e) {
    uint32_t i;
    ypak__src w;
    if (!ypak__valid_entry(p, e)) { if (p) ypak__fail(p, YPAK_ERR_ARG, "ypak_data: not an entry of this open pack"); return NULL; }
    if (!p->base_) { ypak__fail(p, YPAK_ERR_ARG, "ypak_data: no mapping or memory; use a cursor or ypak_check()"); return NULL; }
    i = e->index;
    memset(&w, 0, sizeof(w));
    if (ypak__header_from(p, i, &w)) return NULL;
    if (p->verify_ != YPAK_VERIFY_NONE && !(YPAK__LOAD(&p->ok_[i]) & 2))
        if (ypak__hash(p, i, &w, p->base_ + p->ent_[i].data, NULL, 0, ypak_chunk_count(p->ent_[i].size))) return NULL;
    return p->base_ + p->ent_[i].data;
}

YPAK_API int ypak_check(ypak_pack* p, const ypak_entry* e, const void* bytes, size_t n) {
    uint32_t i;
    const ypak__ent* t;
    ypak__src w;
    int rc;
    if (!ypak__valid_entry(p, e) || !bytes) return p ? ypak__fail(p, YPAK_ERR_ARG, "ypak_check: bad arguments") : YPAK_ERR_ARG;
    i = e->index;
    t = &p->ent_[i];
    if ((int64_t)n != t->data + t->size - t->hdr)
        return ypak__fail(p, YPAK_ERR_ARG, "ypak_check: give [header_off, data_off + size), %lld bytes", (long long)(t->data + t->size - t->hdr));
    w.win = (const uint8_t*)bytes;
    w.win_off = t->hdr;
    w.win_len = (int64_t)n;
    rc = ypak__check_header(p, i, (const uint8_t*)bytes);
    if (rc) return rc;
    YPAK__SET(&p->ok_[i], 1);
    if (p->verify_ == YPAK_VERIFY_NONE) return 0;
    return ypak__hash(p, i, &w, (const uint8_t*)bytes + (t->data - t->hdr), NULL, 0, ypak_chunk_count(t->size));
}

YPAK_API int ypak_verify_all(ypak_pack* p) {
    uint32_t i;
    ypak__src w;
    if (!p || !p->open_) return YPAK_ERR_ARG;
    if (!p->base_ && !p->rd_) return ypak__fail(p, YPAK_ERR_ARG, "ypak_verify_all: an incremental pack has no bytes to read");
    memset(&w, 0, sizeof(w));
    for (i = 0; i < p->n; i++) {
        int rc = ypak__header_from(p, i, &w);
        if (rc == 0 && !(YPAK__LOAD(&p->ok_[i]) & 2))
            rc = ypak__hash(p, i, &w, p->base_ ? p->base_ + p->ent_[i].data : NULL, NULL, 0, ypak_chunk_count(p->ent_[i].size));
        if (rc) return rc;
    }
    return 0;
}

/* --- cursors --------------------------------------------------------------- */

YPAK_API int ypak_cursor_init(ypak_cursor* c, ypak_pack* p, const ypak_entry* e, void* scratch, size_t cap) {
    ypak__src w;
    int rc;
    if (!c) return YPAK_ERR_ARG;
    memset(c, 0, sizeof(*c));
    c->chunk = -1;
    if (!ypak__valid_entry(p, e)) return p ? ypak__fail(p, YPAK_ERR_ARG, "ypak_cursor_init: not an entry of this open pack") : YPAK_ERR_ARG;
    if (!p->base_ && !p->rd_) return ypak__fail(p, YPAK_ERR_ARG, "ypak_cursor_init: an incremental pack has no bytes to read");
    if (!p->base_) {
        int64_t want = e->size < YPAK_CHUNK ? e->size : YPAK_CHUNK;
        if (want > 0 && (!scratch || (int64_t)cap < want))
            return ypak__fail(p, YPAK_ERR_ARG, "ypak_cursor_init: a range reader needs scratch of %lld bytes", (long long)want);
    }
    memset(&w, 0, sizeof(w));
    rc = ypak__header_from(p, e->index, &w);
    if (rc) return rc;
    c->p = p;
    c->index = e->index;
    c->size = e->size;
    c->scratch = (uint8_t*)scratch;
    c->cap = cap;
    return 0;
}

YPAK_API int64_t ypak_cursor_read(void* cursor, int64_t offset, void* buf, int64_t n) {
    ypak_cursor* c = (ypak_cursor*)cursor;
    ypak_pack* p;
    const ypak__ent* t;
    int64_t done = 0;
    if (!c || !c->p || offset < 0 || n < 0 || (!buf && n)) return YPAK_ERR_ARG;
    p = c->p;
    t = &p->ent_[c->index];
    if (offset >= t->size) return 0;
    if (n > t->size - offset) n = t->size - offset;
    while (done < n) {
        int64_t at = offset + done, j = at >> YPAK_CHUNK_LOG2, coff = j << YPAK_CHUNK_LOG2;
        int64_t clen = t->size - coff < YPAK_CHUNK ? t->size - coff : YPAK_CHUNK;
        int64_t take = coff + clen - at;
        const uint8_t* src;
        int need_hash;
        if (take > n - done) take = n - done;
        if (t->chunk == YPAK_NO_CHUNK) need_hash = p->verify_ != YPAK_VERIFY_NONE && !(YPAK__LOAD(&p->ok_[c->index]) & 2);
        else need_hash = p->verify_ != YPAK_VERIFY_NONE && !YPAK__LOAD(&p->cok_[t->chunk + (uint32_t)j]);
        if (p->base_) {
            src = p->base_ + t->data + coff;
            if (need_hash) {
                int rc = ypak__hash(p, c->index, NULL, p->base_ + t->data, NULL, j, j + 1);
                if (rc) { c->error = rc; return rc; }
            }
        } else {
            if (c->chunk != j) {
                int64_t got = clen ? p->rd_->read(p->rdc_, t->data + coff, c->scratch, clen) : 0;
                c->chunk = -1;
                if (got != clen) {
                    c->error = ypak__fail(p, YPAK_ERR_IO, "'%.*s': read failed at %lld", (int)t->name_len,
                                          (const char*)(p->cd_ + t->name), (long long)(t->data + coff));
                    return c->error;
                }
                if (need_hash) {
                    int rc;
                    if (t->chunk == YPAK_NO_CHUNK) {
                        ypak__src w;
                        w.win = c->scratch;
                        w.win_off = t->data;
                        w.win_len = clen;
                        rc = ypak__hash(p, c->index, &w, c->scratch, NULL, 0, 1);
                    } else {
                        uint64_t h = ypak_xxh64(c->scratch, (size_t)clen, 0);
                        p->hashed_bytes += (uint64_t)clen;
                        if (ypak__rd64(p->chunks_ + 8 * ((int64_t)t->chunk + j)) != h)
                            rc = ypak__fail(p, YPAK_ERR_CORRUPT, "'%.*s': chunk %lld does not match its hash (corrupt)",
                                            (int)t->name_len, (const char*)(p->cd_ + t->name), (long long)j);
                        else {
                            YPAK__SET(&p->cok_[t->chunk + (uint32_t)j], 1);
                            rc = 0;
                        }
                    }
                    if (rc) { c->error = rc; return rc; }
                }
                c->chunk = j;
            }
            src = c->scratch;
        }
        memcpy((uint8_t*)buf + done, src + (at - coff), (size_t)take);
        done += take;
    }
    return done;
}

/* --- description ------------------------------------------------------------- */

YPAK_API void ypak_id(const ypak_pack* p, char out[72]) {
    static const char hx[] = "0123456789abcdef";
    int i;
    memcpy(out, "sha256:", 7);
    for (i = 0; i < 32; i++) {
        out[7 + 2 * i] = hx[p ? p->id[i] >> 4 : 0];
        out[8 + 2 * i] = hx[p ? p->id[i] & 15 : 0];
    }
    out[71] = 0;
}

YPAK_API int ypak_describe(const ypak_pack* p, char* buf, size_t cap) {
    static const char* const pol[3] = { "verified on use", "verified at open", "NOT VERIFIED after open" };
    char id[72];
    if (!p || !buf || !cap) return 0;
    ypak_id(p, id);
    return snprintf(buf, cap, "ysp pack format %d, %s, %u entries, %u chunks, zip start %lld, %s (ysp/pack.h %s)",
                    p->format, id, p->n, p->n_chunks, (long long)p->zip_start,
                    pol[p->verify_ >= 0 && p->verify_ <= 2 ? p->verify_ : 0], YPAK_VERSION_STRING);
}

/* --- views ----------------------------------------------------------------- */

static int ypak__vfail(char* err, size_t cap, const char* fmt, ...) {
    va_list ap;
    if (err && cap) {
        va_start(ap, fmt);
        vsnprintf(err, cap, fmt, ap);
        va_end(ap);
    }
    return YPAK_ERR_FORMAT;
}

static int ypak__view_head(const uint8_t* b, int64_t size, const char* magic, uint32_t hsize, char* err, size_t cap) {
    if (!b || size < (int64_t)hsize) return ypak__vfail(err, cap, "%.8s: shorter than its %u-byte header", magic, hsize);
    if (((uintptr_t)b & 7u) != 0) return ypak__vfail(err, cap, "%.8s: not 8-byte aligned in memory", magic);
    if (memcmp(b, magic, 8) != 0) return ypak__vfail(err, cap, "not a %.8s entry (wrong magic)", magic);
    if (ypak__rd32(b + 8) != 1) return ypak__vfail(err, cap, "%.8s: version %u; this reader knows 1", magic, ypak__rd32(b + 8));
    if (ypak__rd32(b + 12) != hsize) return ypak__vfail(err, cap, "%.8s: header size %u, expected %u", magic, ypak__rd32(b + 12), hsize);
    return 0;
}

/* A curve set body at b, of exactly size bytes. */
static int ypak__cset_at(const uint8_t* b, int64_t size, ypak_cset* out, char* err, size_t cap) {
    uint64_t nt, nw, to, wo;
    int rc = ypak__view_head(b, size, "YSPCSET1", 64, err, cap);
    if (rc) return rc;
    memset(out, 0, sizeof(*out));
    out->units = ypak__rd32(b + 16);
    out->n_glyphs = ypak__rd32(b + 20);
    out->flags = ypak__rd32(b + 24);
    out->face = ypak__rd32(b + 28);
    nt = ypak__rd64(b + 32);
    nw = ypak__rd64(b + 40);
    to = ypak__rd64(b + 48);
    wo = ypak__rd64(b + 56);
    if (out->units > 1 || (out->flags & ~1u) || out->n_glyphs >= (1u << 24))
        return ypak__vfail(err, cap, "YSPCSET1: units, flags or glyph count out of range");
    if (nt > 0xFFFFFFFFu || nw > 0xFFFFFFFFu || nw % 4 || nw < 8u + out->n_glyphs || to != 64 || wo != 64 + 16 * nt ||
        (uint64_t)size != wo + 4 * nw)
        return ypak__vfail(err, cap, "YSPCSET1: counts and offsets do not describe the entry");
    out->texels = (const float*)(const void*)(b + to);
    out->words = (const uint32_t*)(const void*)(b + wo);
    out->n_texels = (uint32_t)nt;
    out->n_words = (uint32_t)nw;
    if (out->words[0] != 0x43505359u || out->words[1] != 1 || out->words[2] != out->n_glyphs)
        return ypak__vfail(err, cap, "YSPCSET1: the words do not start a format v1 curve set of %u glyphs", out->n_glyphs);
    return 0;
}

YPAK_API int ypak_cset_view(const void* data, int64_t size, ypak_cset* out, char* err, size_t cap) {
    if (!out) return YPAK_ERR_ARG;
    return ypak__cset_at((const uint8_t*)data, size, out, err, cap);
}

YPAK_API int ypak_art_view(const void* data, int64_t size, ypak_art* out, char* err, size_t cap) {
    const uint8_t* b = (const uint8_t*)data;
    uint64_t lo, co, cb;
    uint32_t i;
    int rc;
    if (!out) return YPAK_ERR_ARG;
    rc = ypak__view_head(b, size, "YSPARTW1", 80, err, cap);
    if (rc) return rc;
    memset(out, 0, sizeof(*out));
    out->n_layers = ypak__rd32(b + 16);
    for (i = 0; i < 4; i++) {
        uint64_t v = ypak__rd64(b + 24 + 8 * i);
        memcpy(&out->viewbox[i], &v, 8);
    }
    lo = ypak__rd64(b + 56);
    co = ypak__rd64(b + 64);
    cb = ypak__rd64(b + 72);
    if (ypak__rd32(b + 20) != 0 || out->n_layers > (1u << 24) || lo != 80 || co != 80 + 16 * (uint64_t)out->n_layers ||
        cb > (uint64_t)size || co > (uint64_t)size - cb || co + cb != (uint64_t)size)
        return ypak__vfail(err, cap, "YSPARTW1: counts and offsets do not describe the entry");
    rc = ypak__cset_at(b + co, (int64_t)cb, &out->set, err, cap);
    if (rc) return rc;
    if (out->set.units != YPAK_CSET_UNITS_USER) return ypak__vfail(err, cap, "YSPARTW1: its curve set is not in user units");
    out->layers = (const ypak_art_layer*)(const void*)(b + lo);
    for (i = 0; i < out->n_layers; i++) {
        const ypak_art_layer* l = &out->layers[i];
        if (l->glyph >= out->set.n_glyphs || (l->source != 1 && l->source != 2) || l->element < 0)
            return ypak__vfail(err, cap, "YSPARTW1: layer %u is out of range", i);
    }
    return 0;
}

/* A string of len bytes at off followed by a NUL, inside [lo, hi). */
static int ypak__str_in(const uint8_t* b, uint64_t lo, uint64_t hi, uint32_t off, uint32_t len) {
    uint64_t s = lo + off;
    if (off > hi - lo || (uint64_t)len + 1 > hi - s) return 0;
    if (b[s + len] != 0) return 0;
    return memchr(b + s, 0, len) == NULL;
}

YPAK_API int ypak_shader_view(const void* data, int64_t size, ypak_shader* out, char* err, size_t cap) {
    const uint8_t* b = (const uint8_t*)data;
    uint32_t bo, bl, no, nl;
    int rc;
    if (!out) return YPAK_ERR_ARG;
    rc = ypak__view_head(b, size, "YSPSHAD1", 64, err, cap);
    if (rc) return rc;
    memset(out, 0, sizeof(*out));
    out->mode = ypak__rd32(b + 16);
    out->flags = ypak__rd32(b + 20);
    out->params = ypak__rd32(b + 24);
    out->textures = ypak__rd32(b + 28);
    out->contract = ypak__rd32(b + 32);
    out->wrap_hash = ypak__rd64(b + 40);
    bo = ypak__rd32(b + 48); bl = ypak__rd32(b + 52);
    no = ypak__rd32(b + 56); nl = ypak__rd32(b + 60);
    if (out->mode > 2 || (out->flags & ~7u) || (out->textures & ~15u) || ypak__rd32(b + 36) != 0)
        return ypak__vfail(err, cap, "YSPSHAD1: mode, flags or masks out of range");
    if (bo != 64 || !ypak__str_in(b, 0, (uint64_t)size, bo, bl) || no != bo + bl + 1 || !ypak__str_in(b, 0, (uint64_t)size, no, nl) ||
        (((uint64_t)no + nl + 1 + 15) & ~(uint64_t)15) != (uint64_t)size)
        return ypak__vfail(err, cap, "YSPSHAD1: the body or the name is out of bounds or not NUL-ended");
    out->body = (const char*)(b + bo);
    out->body_len = bl;
    out->name = (const char*)(b + no);
    out->name_len = nl;
    return 0;
}

static uint32_t ypak__texel_bytes(uint32_t f) {
    switch (f) {
    case YPAK_TEX_R8: return 1;
    case YPAK_TEX_RG8: return 2;
    case YPAK_TEX_RGBA8: return 4;
    case YPAK_TEX_R16F: return 4;
    case YPAK_TEX_RGBA16F: return 16;
    case YPAK_TEX_R32F: return 4;
    case YPAK_TEX_RGBA32F: return 16;
    case YPAK_TEX_R16UI: return 2;
    default: return 0;
    }
}

YPAK_API int ypak_texture_view(const void* data, int64_t size, ypak_texture* out, char* err, size_t cap) {
    const uint8_t* b = (const uint8_t*)data;
    int rc;
    if (!out) return YPAK_ERR_ARG;
    rc = ypak__view_head(b, size, "YSPTEXR1", 64, err, cap);
    if (rc) return rc;
    memset(out, 0, sizeof(*out));
    out->w = ypak__rd32(b + 16);
    out->h = ypak__rd32(b + 20);
    out->format = ypak__rd32(b + 24);
    memcpy(out->enc, b + 28, 8);
    out->compression = ypak__rd32(b + 36);
    out->stored = ypak__rd64(b + 40);
    out->raw_bytes = ypak__rd64(b + 48);
    out->raw_hash = ypak__rd64(b + 56);
    out->texel_bytes = ypak__texel_bytes(out->format);
    if (!out->texel_bytes || out->w == 0 || out->h == 0 || out->w > 65536 || out->h > 65536)
        return ypak__vfail(err, cap, "YSPTEXR1: format %u or size %u x %u out of range", out->format, out->w, out->h);
    if (out->raw_bytes != (uint64_t)out->w * out->h * out->texel_bytes || out->compression > 1 ||
        (out->compression == YPAK_TEX_QOI && out->format != YPAK_TEX_RGBA8) ||
        (out->compression == YPAK_TEX_RAW && out->stored != out->raw_bytes) || out->stored > (uint64_t)size - 64 ||
        ((64 + out->stored + 15) & ~(uint64_t)15) != (uint64_t)size || out->enc[6] || out->enc[7])
        return ypak__vfail(err, cap, "YSPTEXR1: sizes, compression or encoding do not describe the entry");
    out->data = b + 64;
    return 0;
}

YPAK_API int ypak_qoi_decode(const ypak_texture* t, void* dst, size_t cap, char* err, size_t errcap) {
    const uint8_t* s;
    uint8_t* o = (uint8_t*)dst;
    uint8_t px[4] = { 0, 0, 0, 255 }, idx[64][4];
    uint64_t n, at = 14, end, i = 0, run = 0;
    if (!t || !dst || t->compression != YPAK_TEX_QOI) return YPAK_ERR_ARG;
    if (cap < t->raw_bytes) return ypak__vfail(err, errcap, "QOI: dst holds %zu bytes, needs %llu", cap, (unsigned long long)t->raw_bytes);
    s = t->data;
    n = t->stored;
    if (n < 22 || memcmp(s, "qoif", 4) != 0 ||
        (((uint32_t)s[4] << 24) | ((uint32_t)s[5] << 16) | ((uint32_t)s[6] << 8) | s[7]) != t->w ||
        (((uint32_t)s[8] << 24) | ((uint32_t)s[9] << 16) | ((uint32_t)s[10] << 8) | s[11]) != t->h || s[12] != 4 || s[13] > 1)
        return ypak__vfail(err, errcap, "QOI: header does not match the texture");
    end = n - 8;
    memset(idx, 0, sizeof(idx));
    while (i < (uint64_t)t->w * t->h) {
        if (run) {
            run--;
        } else {
            uint8_t b1;
            if (at >= end) return ypak__vfail(err, errcap, "QOI: data ends at pixel %llu", (unsigned long long)i);
            b1 = s[at++];
            if (b1 == 0xFE) {
                if (at + 3 > end) return ypak__vfail(err, errcap, "QOI: truncated RGB");
                px[0] = s[at]; px[1] = s[at + 1]; px[2] = s[at + 2]; at += 3;
            } else if (b1 == 0xFF) {
                if (at + 4 > end) return ypak__vfail(err, errcap, "QOI: truncated RGBA");
                px[0] = s[at]; px[1] = s[at + 1]; px[2] = s[at + 2]; px[3] = s[at + 3]; at += 4;
            } else if ((b1 & 0xC0) == 0x00) {
                memcpy(px, idx[b1], 4);
            } else if ((b1 & 0xC0) == 0x40) {
                px[0] = (uint8_t)(px[0] + ((b1 >> 4) & 3) - 2);
                px[1] = (uint8_t)(px[1] + ((b1 >> 2) & 3) - 2);
                px[2] = (uint8_t)(px[2] + (b1 & 3) - 2);
            } else if ((b1 & 0xC0) == 0x80) {
                uint8_t b2;
                int vg = (b1 & 0x3F) - 32;
                if (at >= end) return ypak__vfail(err, errcap, "QOI: truncated LUMA");
                b2 = s[at++];
                px[0] = (uint8_t)(px[0] + vg - 8 + ((b2 >> 4) & 15));
                px[1] = (uint8_t)(px[1] + vg);
                px[2] = (uint8_t)(px[2] + vg - 8 + (b2 & 15));
            } else {
                run = b1 & 0x3F;
            }
            memcpy(idx[(px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64], px, 4);
        }
        memcpy(o + 4 * i, px, 4);
        i++;
    }
    if (run || at != end || memcmp(s + end, "\0\0\0\0\0\0\0\1", 8) != 0)
        return ypak__vfail(err, errcap, "QOI: data after the last pixel, or no end marker");
    if (ypak_xxh64(dst, (size_t)t->raw_bytes, 0) != t->raw_hash)
        return ypak__vfail(err, errcap, "QOI: decoded texels do not match the raw hash");
    return 0;
}

YPAK_API size_t ypak_qoi_encode(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* out, size_t cap) {
    uint64_t n = (uint64_t)w * h, i;
    size_t need = (size_t)(14 + n * 5 + 8), at = 0;
    uint8_t px[4] = { 0, 0, 0, 255 }, idx[64][4];
    uint32_t run = 0;
    if (!rgba || !out || cap < need) return need;
    memset(idx, 0, sizeof(idx));
    memcpy(out, "qoif", 4);
    out[4] = (uint8_t)(w >> 24); out[5] = (uint8_t)(w >> 16); out[6] = (uint8_t)(w >> 8); out[7] = (uint8_t)w;
    out[8] = (uint8_t)(h >> 24); out[9] = (uint8_t)(h >> 16); out[10] = (uint8_t)(h >> 8); out[11] = (uint8_t)h;
    out[12] = 4;
    out[13] = 0;
    at = 14;
    for (i = 0; i < n; i++) {
        const uint8_t* c = rgba + 4 * i;
        if (memcmp(c, px, 4) == 0) {
            run++;
            if (run == 62 || i == n - 1) {
                out[at++] = (uint8_t)(0xC0 | (run - 1));
                run = 0;
            }
            continue;
        }
        if (run) {
            out[at++] = (uint8_t)(0xC0 | (run - 1));
            run = 0;
        }
        {
            int h6 = (c[0] * 3 + c[1] * 5 + c[2] * 7 + c[3] * 11) % 64;
            if (memcmp(idx[h6], c, 4) == 0) {
                out[at++] = (uint8_t)h6;
            } else {
                memcpy(idx[h6], c, 4);
                if (c[3] == px[3]) {
                    int vr = (int)(int8_t)(uint8_t)(c[0] - px[0]), vg = (int)(int8_t)(uint8_t)(c[1] - px[1]),
                        vb = (int)(int8_t)(uint8_t)(c[2] - px[2]);
                    int vgr = vr - vg, vgb = vb - vg;
                    if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2) {
                        out[at++] = (uint8_t)(0x40 | ((vr + 2) << 4) | ((vg + 2) << 2) | (vb + 2));
                    } else if (vgr > -9 && vgr < 8 && vg > -33 && vg < 32 && vgb > -9 && vgb < 8) {
                        out[at++] = (uint8_t)(0x80 | (vg + 32));
                        out[at++] = (uint8_t)(((vgr + 8) << 4) | (vgb + 8));
                    } else {
                        out[at++] = 0xFE;
                        out[at++] = c[0]; out[at++] = c[1]; out[at++] = c[2];
                    }
                } else {
                    out[at++] = 0xFF;
                    memcpy(out + at, c, 4);
                    at += 4;
                }
            }
        }
        memcpy(px, c, 4);
    }
    memcpy(out + at, "\0\0\0\0\0\0\0\1", 8);
    return at + 8;
}

YPAK_API int ypak_runs_view(const void* data, int64_t size, ypak_runs* out, char* err, size_t cap) {
    const uint8_t* b = (const uint8_t*)data;
    uint64_t fo, bo, ro, io, co, lo, po, pb;
    uint32_t i;
    int rc;
    if (!out) return YPAK_ERR_ARG;
    rc = ypak__view_head(b, size, "YSPGRUN1", 112, err, cap);
    if (rc) return rc;
    memset(out, 0, sizeof(*out));
    out->n_fonts = ypak__rd32(b + 16);
    out->n_blocks = ypak__rd32(b + 20);
    out->n_runs = ypak__rd32(b + 24);
    out->n_items = ypak__rd32(b + 28);
    out->n_lines = ypak__rd32(b + 32);
    fo = ypak__rd64(b + 40); bo = ypak__rd64(b + 48); ro = ypak__rd64(b + 56); io = ypak__rd64(b + 64);
    co = ypak__rd64(b + 72); lo = ypak__rd64(b + 80); po = ypak__rd64(b + 88); pb = ypak__rd64(b + 96);
#define YPAK__A16(x) (((x) + 15u) & ~(uint64_t)15u)
    if (ypak__rd32(b + 36) || ypak__rd64(b + 104) || out->n_fonts > 64 || out->n_blocks > (1u << 20) ||
        out->n_runs > (1u << 24) || out->n_items > (1u << 26) || out->n_lines > (1u << 24) ||
        fo != 112 || bo != YPAK__A16(fo + 16u * out->n_fonts) || ro != YPAK__A16(bo + 96u * out->n_blocks) ||
        io != YPAK__A16(ro + 16u * out->n_runs) || co != YPAK__A16(io + 32u * out->n_items) ||
        lo != YPAK__A16(co + 4u * out->n_items) || po != YPAK__A16(lo + 32u * out->n_lines) ||
        pb > (uint64_t)size || po > (uint64_t)size - pb || YPAK__A16(po + pb) != (uint64_t)size)
        return ypak__vfail(err, cap, "YSPGRUN1: counts and offsets do not describe the entry");
#undef YPAK__A16
    out->fonts = (const ypak_runs_font*)(const void*)(b + fo);
    out->blocks = (const ypak_runs_block_rec*)(const void*)(b + bo);
    out->runs = (const ypak_runs_run*)(const void*)(b + ro);
    out->items = (const float*)(const void*)(b + io);
    out->clusters = (const uint32_t*)(const void*)(b + co);
    out->lines = (const ypak_runs_line*)(const void*)(b + lo);
    out->pool = (const char*)(b + po);
    out->pool_bytes = pb;
    for (i = 0; i < out->n_fonts; i++) {
        const ypak_runs_font* f = &out->fonts[i];
        if (!ypak__str_in(b, po, po + pb, f->cset_off, f->cset_len) || !ypak__str_in(b, po, po + pb, f->font_off, f->font_len))
            return ypak__vfail(err, cap, "YSPGRUN1: font %u's names are out of bounds", i);
    }
    for (i = 0; i < out->n_blocks; i++) {
        const ypak_runs_block_rec* k = &out->blocks[i];
        uint32_t r;
        if (!ypak__str_in(b, po, po + pb, k->key_off, k->key_len) || !ypak__str_in(b, po, po + pb, k->text_off, k->text_len) ||
            !ypak__str_in(b, po, po + pb, k->lang_off, k->lang_len))
            return ypak__vfail(err, cap, "YSPGRUN1: block %u's strings are out of bounds", i);
        if (k->run_first > out->n_runs || k->n_runs > out->n_runs - k->run_first || k->item_first > out->n_items ||
            k->n_items > out->n_items - k->item_first || k->line_first > out->n_lines || k->n_lines > out->n_lines - k->line_first ||
            k->reserved_[0] || k->reserved_[1])
            return ypak__vfail(err, cap, "YSPGRUN1: block %u's ranges are out of bounds", i);
        if (i && ypak__cmp((const uint8_t*)out->pool + out->blocks[i - 1].key_off, out->blocks[i - 1].key_len,
                           (const uint8_t*)out->pool + k->key_off, k->key_len) >= 0)
            return ypak__vfail(err, cap, "YSPGRUN1: block keys are not sorted and unique at block %u", i);
        for (r = 0; r < k->n_runs; r++) {
            const ypak_runs_run* q = &out->runs[k->run_first + r];
            if (q->font >= out->n_fonts || q->first > k->n_items || q->n > k->n_items - q->first)
                return ypak__vfail(err, cap, "YSPGRUN1: block %u run %u is out of range", i, r);
        }
    }
    return 0;
}

YPAK_API int ypak_runs_find(const ypak_runs* r, const char* key) {
    uint32_t lo = 0, hi;
    size_t n;
    if (!r || !key) return -1;
    n = strlen(key);
    hi = r->n_blocks;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const ypak_runs_block_rec* k = &r->blocks[mid];
        int c = ypak__cmp((const uint8_t*)r->pool + k->key_off, k->key_len, (const uint8_t*)key, n);
        if (c == 0) return (int)mid;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

YPAK_API const char* ypak_runs_str(const ypak_runs* r, uint32_t off) {
    return (r && off < r->pool_bytes) ? r->pool + off : "";
}

#endif /* YSP_PACK_IMPLEMENTATION_GUARD */
#endif /* YSP_PACK_IMPLEMENTATION */

/* ------------------------------------------------------------------------
 * This software is available under the MIT-0 (MIT No Attribution) license.
 *
 * Copyright (c) 2026 ysp contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY.
 * ------------------------------------------------------------------------ */
